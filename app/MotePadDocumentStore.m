//
//  MotePadDocumentStore.m
//  iSH-AOK
//

#import "MotePadDocumentStore.h"
#import "AppGroup.h"

#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/task.h"
#include "fs/fd.h"
#include "fs/stat.h"
#include "fs/path.h"
#include "util/sync.h"
#include "debug.h"

#ifndef DT_UNKNOWN
#define DT_UNKNOWN 0
#endif
#ifndef DT_DIR
#define DT_DIR 4
#endif
#ifndef DT_REG
#define DT_REG 8
#endif
#ifndef DT_LNK
#define DT_LNK 10
#endif

// How many levels of guest symlink to resolve manually before giving up.
#define MOTEPAD_MAX_SYMLINK_DEPTH 8

static NSString *const kPersistGuestPrefix = @"/AOK/persist/";

@implementation MotePadDirectoryEntry
@end

@implementation MotePadDocumentStore

+ (instancetype)sharedStore {
    static MotePadDocumentStore *shared;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ shared = [[MotePadDocumentStore alloc] init]; });
    return shared;
}

+ (dispatch_queue_t)ioQueue {
    static dispatch_queue_t queue;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        queue = dispatch_queue_create("app.ish.motepad.io", DISPATCH_QUEUE_SERIAL);
    });
    return queue;
}

#pragma mark Asynchronous API

- (void)listDirectoryAtGuestPath:(NSString *)guestPath
                      completion:(void (^)(NSArray<MotePadDirectoryEntry *> * _Nullable))completion {
    NSString *path = [guestPath copy];
    dispatch_async(MotePadDocumentStore.ioQueue, ^{
        NSArray<MotePadDirectoryEntry *> *entries = [self listDirectoryAtGuestPath:path];
        dispatch_async(dispatch_get_main_queue(), ^{ if (completion) completion(entries); });
    });
}

- (void)readTextFileAtGuestPath:(NSString *)guestPath
                     completion:(void (^)(NSString * _Nullable, NSError * _Nullable))completion {
    NSString *path = [guestPath copy];
    dispatch_async(MotePadDocumentStore.ioQueue, ^{
        NSError *error = nil;
        NSString *text = [self readTextFileAtGuestPath:path error:&error];
        dispatch_async(dispatch_get_main_queue(), ^{ if (completion) completion(text, error); });
    });
}

- (void)writeText:(NSString *)text
      toGuestPath:(NSString *)guestPath
       completion:(void (^)(BOOL, NSError * _Nullable))completion {
    NSString *snapshot = [text copy];
    NSString *path = [guestPath copy];
    dispatch_async(MotePadDocumentStore.ioQueue, ^{
        NSError *error = nil;
        BOOL ok = [self writeText:snapshot toGuestPath:path error:&error];
        dispatch_async(dispatch_get_main_queue(), ^{ if (completion) completion(ok, error); });
    });
}

- (void)defaultDirectoryGuestPathWithCompletion:(void (^)(NSString *))completion {
    dispatch_async(MotePadDocumentStore.ioQueue, ^{
        NSString *directory = [self defaultDirectoryGuestPath];
        dispatch_async(dispatch_get_main_queue(), ^{ if (completion) completion(directory); });
    });
}

- (void)modificationDateAtGuestPath:(NSString *)guestPath
                         completion:(void (^)(NSDate * _Nullable))completion {
    NSString *path = [guestPath copy];
    dispatch_async(MotePadDocumentStore.ioQueue, ^{
        NSDate *date = [self modificationDateAtGuestPath:path];
        dispatch_async(dispatch_get_main_queue(), ^{ if (completion) completion(date); });
    });
}

#pragma mark Host paths (realfs /AOK/persist fast path)

- (NSURL *)persistHostBaseURL {
    NSURL *container = ContainerURL();
    if (container == nil) return nil;
    return [[container URLByAppendingPathComponent:@"AOK" isDirectory:YES]
            URLByAppendingPathComponent:@"persist" isDirectory:YES];
}

// Host URL for a path under the realfs /AOK/persist mount, or nil if not under it.
// /AOK/persist is a real host directory, so anything under it can be reached with
// NSFileManager from any thread — no guest VFS, no borrowed task context needed.
- (NSURL *)hostURLForPersistGuestPath:(NSString *)guestPath {
    BOOL isPersistRoot = [guestPath isEqualToString:@"/AOK/persist"] || [guestPath isEqualToString:@"/AOK/persist/"];
    if (!isPersistRoot && ![guestPath hasPrefix:kPersistGuestPrefix])
        return nil;
    NSURL *base = [self persistHostBaseURL];
    if (base == nil)
        return nil;
    NSString *rel = isPersistRoot ? @"" : [guestPath substringFromIndex:kPersistGuestPrefix.length];
    NSURL *hostURL = base;
    for (NSString *component in rel.pathComponents) {
        if (component.length == 0 || [component isEqualToString:@"/"])
            continue;
        hostURL = [hostURL URLByAppendingPathComponent:component];
    }
    return hostURL;
}

#pragma mark Borrowed task context

// Run a guest-VFS operation under a borrowed task context. fs/path.c resolves
// paths through the thread-local `current` task, which is unset on GCD/UI threads,
// so we borrow pid 1 exactly as the app's boot code does (PushInitTaskAsCurrent)
// and restore afterward. Returns NO if init isn't usable yet.
- (BOOL)withGuestTaskContext:(void (^)(void))block {
    struct task *previous = current;
    struct task *init = pid_get_task_ref(1);
    if (init != NULL) {
        bool usable;
        lock(&init->general_lock, 0);
        usable = init->mm != NULL && init->mem != NULL && init->files != NULL && init->fs != NULL;
        unlock(&init->general_lock);
        if (!usable) { task_ref_cnt_mod(init, -1); init = NULL; }
    }
    if (init == NULL)
        return NO;
    current = init;
    block();
    current = previous;
    task_ref_cnt_mod(init, -1);
    return YES;
}

#pragma mark Directory listing

- (nullable NSArray<MotePadDirectoryEntry *> *)listDirectoryAtGuestPath:(NSString *)guestPath {
    guestPath = [self normalizedGuestPath:guestPath];

    NSURL *hostDir = [self hostURLForPersistGuestPath:guestPath];
    BOOL hostIsDir = NO;
    if (hostDir != nil && [NSFileManager.defaultManager fileExistsAtPath:hostDir.path isDirectory:&hostIsDir] && hostIsDir)
        return [self listHostDirectory:hostDir guestBasePath:guestPath];

    __block NSArray<MotePadDirectoryEntry *> *result = nil;
    [self withGuestTaskContext:^{
        result = [self listGuestDirectoryViaVFS:guestPath];
    }];
    return result;
}

- (NSArray<MotePadDirectoryEntry *> *)listHostDirectory:(NSURL *)hostDir guestBasePath:(NSString *)guestBasePath {
    NSArray<NSURL *> *contents =
        [NSFileManager.defaultManager contentsOfDirectoryAtURL:hostDir
                                    includingPropertiesForKeys:@[NSURLIsDirectoryKey, NSURLIsRegularFileKey]
                                                       options:NSDirectoryEnumerationSkipsHiddenFiles
                                                         error:NULL];
    NSMutableArray<MotePadDirectoryEntry *> *entries = [NSMutableArray array];
    for (NSURL *fileURL in contents) {
        MotePadDirectoryEntry *entry = [MotePadDirectoryEntry new];
        entry.name = fileURL.lastPathComponent;
        entry.guestPath = [guestBasePath stringByAppendingPathComponent:entry.name];
        // Classify by the *target's* type, following symlinks (stat, not lstat), so a
        // symlinked directory is enterable and a symlinked file is openable. Everything
        // that isn't a directory or a regular file — FIFOs, sockets, devices, broken
        // symlinks (stat fails) — is Other, which the browser renders unselectable.
        // A guest `mkfifo /AOK/persist/p` must never be classified as an openable file:
        // opening it would block forever.
        struct stat st;
        MotePadEntryType type = MotePadEntryTypeOther;
        if (stat(fileURL.fileSystemRepresentation, &st) == 0) {
            if (S_ISDIR(st.st_mode))
                type = MotePadEntryTypeDirectory;
            else if (S_ISREG(st.st_mode))
                type = MotePadEntryTypeFile;
        }
        entry.type = type;
        [entries addObject:entry];
    }
    return [self sortedEntries:entries];
}

// Caller must already hold a borrowed task context.
- (NSArray<MotePadDirectoryEntry *> *)listGuestDirectoryViaVFS:(NSString *)guestPath {
    struct fd *fd = generic_open(guestPath.fileSystemRepresentation, O_RDONLY_, 0);
    if (IS_ERR(fd))
        return nil;
    if (!S_ISDIR(fd->type) || fd->ops->readdir == NULL) { fd_close(fd); return nil; }

    NSMutableArray<MotePadDirectoryEntry *> *entries = [NSMutableArray array];
    if (fd->ops->readdir_begin) fd->ops->readdir_begin(fd);
    while (true) {
        struct dir_entry raw;
        memset(&raw, 0, sizeof(raw));
        raw.type = DT_UNKNOWN;
        int err = fd->ops->readdir(fd, &raw);
        if (err <= 0) break;  // 0 = end, <0 = error
        NSString *name = [NSString stringWithUTF8String:raw.name];
        if (name == nil || [name isEqualToString:@"."] || [name isEqualToString:@".."])
            continue;
        NSString *childPath = [guestPath stringByAppendingPathComponent:name];

        // Symlinks are classified by their *target* (generic_statat with flags 0
        // follows links), so on merged-usr roots /bin -> usr/bin is enterable and a
        // symlinked file is openable. A broken symlink stats out and stays Other.
        // DT_UNKNOWN gets the same treatment, and only S_ISREG maps to File — a FIFO
        // or socket must never be presented as openable text (opening one can block).
        int type = raw.type;
        if (type == DT_UNKNOWN || type == DT_LNK) {
            struct statbuf st;
            memset(&st, 0, sizeof(st));
            if (generic_statat(AT_PWD, childPath.fileSystemRepresentation, &st, 0) >= 0)
                type = S_ISDIR(st.mode) ? DT_DIR
                     : S_ISREG(st.mode) ? DT_REG
                     : DT_UNKNOWN;
            else
                type = DT_UNKNOWN;
        }

        MotePadDirectoryEntry *entry = [MotePadDirectoryEntry new];
        entry.name = name;
        entry.guestPath = childPath;
        entry.type = (type == DT_DIR) ? MotePadEntryTypeDirectory
                   : (type == DT_REG) ? MotePadEntryTypeFile
                   : MotePadEntryTypeOther;
        [entries addObject:entry];
    }
    if (fd->ops->readdir_end) fd->ops->readdir_end(fd);
    fd_close(fd);
    return [self sortedEntries:entries];
}

- (NSArray<MotePadDirectoryEntry *> *)sortedEntries:(NSArray<MotePadDirectoryEntry *> *)entries {
    return [entries sortedArrayUsingComparator:^NSComparisonResult(MotePadDirectoryEntry *a, MotePadDirectoryEntry *b) {
        BOOL aDir = a.type == MotePadEntryTypeDirectory;
        BOOL bDir = b.type == MotePadEntryTypeDirectory;
        if (aDir != bDir)
            return aDir ? NSOrderedAscending : NSOrderedDescending;  // directories first
        return [a.name caseInsensitiveCompare:b.name];
    }];
}

#pragma mark Reading

- (nullable NSString *)readTextFileAtGuestPath:(NSString *)guestPath error:(NSError **)error {
    guestPath = [self normalizedGuestPath:guestPath];

    NSURL *hostURL = [self hostURLForPersistGuestPath:guestPath];
    if (hostURL != nil && [NSFileManager.defaultManager fileExistsAtPath:hostURL.path]) {
        NSData *data = [self readHostRegularFileAtURL:hostURL error:error];
        if (data == nil) return nil;
        return [self stringFromData:data];
    }

    __block NSData *data = nil;
    __block NSError *localError = nil;
    BOOL hadContext = [self withGuestTaskContext:^{
        data = [self readGuestFileViaVFS:guestPath error:&localError];
    }];
    if (!hadContext) {
        if (error) *error = [self errorWithCode:0 message:@"Guest filesystem not ready"];
        return nil;
    }
    if (data == nil) {
        if (error) *error = localError;
        return nil;
    }
    return [self stringFromData:data];
}

// Reads a whole host file, refusing anything that isn't a regular file. Opens with
// O_NONBLOCK so that even a TOCTOU race (a FIFO appearing between the browser's
// classification and this open) cannot block: a FIFO open succeeds immediately with
// O_NONBLOCK, and the fstat afterwards rejects it.
- (NSData *)readHostRegularFileAtURL:(NSURL *)hostURL error:(NSError **)error {
    int fd = open(hostURL.fileSystemRepresentation, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        if (error) *error = [NSError errorWithDomain:NSPOSIXErrorDomain code:errno
                                            userInfo:@{NSLocalizedDescriptionKey: @"Cannot open file"}];
        return nil;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        if (error) *error = [self errorWithCode:0 message:@"That path is not a regular file"];
        return nil;
    }
    NSMutableData *data = [NSMutableData data];
    char buffer[65536];
    ssize_t n;
    while ((n = read(fd, buffer, sizeof(buffer))) > 0)
        [data appendBytes:buffer length:(NSUInteger)n];
    int readErrno = errno;
    close(fd);
    if (n < 0) {
        if (error) *error = [NSError errorWithDomain:NSPOSIXErrorDomain code:readErrno
                                            userInfo:@{NSLocalizedDescriptionKey: @"Failed reading file"}];
        return nil;
    }
    return data;
}

// Caller must already hold a borrowed task context.
- (NSData *)readGuestFileViaVFS:(NSString *)guestPath error:(NSError **)error {
    // O_NONBLOCK_ so a FIFO that slips past the browser's classification cannot
    // block the I/O queue inside the emulator's open path; the type check below
    // then rejects it (and every other non-regular file) cleanly.
    struct fd *fd = generic_open(guestPath.fileSystemRepresentation, O_RDONLY_ | O_NONBLOCK_, 0);
    if (IS_ERR(fd)) {
        if (error) *error = [self errorWithCode:(int)PTR_ERR(fd) message:@"Cannot open file"];
        return nil;
    }
    if (S_ISDIR(fd->type)) {
        fd_close(fd);
        if (error) *error = [self errorWithCode:0 message:@"That path is a directory"];
        return nil;
    }
    if (!S_ISREG(fd->type)) {
        fd_close(fd);
        if (error) *error = [self errorWithCode:0 message:@"That path is not a regular file"];
        return nil;
    }
    NSMutableData *data = [NSMutableData data];
    char buffer[65536];
    ssize_t n;
    while ((n = fd->ops->read(fd, buffer, sizeof(buffer))) > 0)
        [data appendBytes:buffer length:(NSUInteger)n];
    fd_close(fd);
    if (n < 0) {
        if (error) *error = [self errorWithCode:(int)n message:@"Failed reading file"];
        return nil;
    }
    return data;
}

- (NSString *)stringFromData:(NSData *)data {
    NSString *text = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
    if (text == nil)  // non-UTF-8: degrade gracefully rather than refuse to open
        text = [[NSString alloc] initWithData:data encoding:NSISOLatin1StringEncoding];
    return text ?: @"";
}

#pragma mark Writing

- (BOOL)writeText:(NSString *)text toGuestPath:(NSString *)guestPath error:(NSError **)error {
    guestPath = [self normalizedGuestPath:guestPath];
    NSData *data = [text dataUsingEncoding:NSUTF8StringEncoding] ?: [NSData data];

    NSURL *hostURL = [self hostURLForPersistGuestPath:guestPath];
    if (hostURL != nil) {
        // A symlink to a regular file is written through (NSDataWritingAtomic would
        // otherwise replace the link itself); anything else that exists and is not a
        // regular file (FIFO, socket, device) is refused rather than clobbered.
        struct stat lst;
        if (lstat(hostURL.fileSystemRepresentation, &lst) == 0) {
            if (S_ISLNK(lst.st_mode)) {
                char resolved[PATH_MAX];
                if (realpath(hostURL.fileSystemRepresentation, resolved) == NULL) {
                    if (error) *error = [self errorWithCode:0 message:@"Cannot save through a broken symbolic link"];
                    return NO;
                }
                struct stat rst;
                if (stat(resolved, &rst) != 0 || !S_ISREG(rst.st_mode)) {
                    if (error) *error = [self errorWithCode:0 message:@"That path is not a regular file"];
                    return NO;
                }
                hostURL = [NSURL fileURLWithPath:[NSString stringWithUTF8String:resolved] isDirectory:NO];
            } else if (!S_ISREG(lst.st_mode)) {
                if (error) *error = [self errorWithCode:0 message:@"That path is not a regular file"];
                return NO;
            }
        }
        [NSFileManager.defaultManager createDirectoryAtURL:hostURL.URLByDeletingLastPathComponent
                               withIntermediateDirectories:YES attributes:nil error:NULL];
        return [data writeToURL:hostURL options:NSDataWritingAtomic error:error];
    }

    __block BOOL ok = NO;
    __block NSError *localError = nil;
    BOOL hadContext = [self withGuestTaskContext:^{
        ok = [self writeData:data toGuestPathViaVFS:guestPath error:&localError];
    }];
    if (!hadContext) {
        if (error) *error = [self errorWithCode:0 message:@"Guest filesystem not ready"];
        return NO;
    }
    if (!ok && error) *error = localError;
    return ok;
}

// Resolves guest symlinks in the final path component so a save through a symlink
// updates the target instead of the rename below replacing the link with a regular
// file. Stops at the first non-link (or nonexistent) path. Returns nil if the chain
// is deeper than MOTEPAD_MAX_SYMLINK_DEPTH (caller falls back to the in-place write,
// whose generic_open follows arbitrarily deep chains).
// Caller must already hold a borrowed task context.
- (NSString *)resolvedGuestWriteTargetForPath:(NSString *)guestPath {
    NSString *path = guestPath;
    for (int depth = 0; depth < MOTEPAD_MAX_SYMLINK_DEPTH; depth++) {
        struct statbuf st;
        memset(&st, 0, sizeof(st));
        if (generic_statat(AT_PWD, path.fileSystemRepresentation, &st, AT_SYMLINK_NOFOLLOW_) < 0)
            return path;  // doesn't exist yet: create right here
        if (!S_ISLNK(st.mode))
            return path;
        char target[MAX_PATH];
        ssize_t n = generic_readlinkat(AT_PWD, path.fileSystemRepresentation, target, sizeof(target) - 1);
        if (n <= 0 || (size_t)n >= sizeof(target))
            return path;
        target[n] = '\0';
        NSString *targetString = [NSString stringWithUTF8String:target];
        if (targetString == nil)
            return path;
        if ([targetString hasPrefix:@"/"])
            path = [self normalizedGuestPath:targetString];
        else
            path = [self normalizedGuestPath:
                    [path.stringByDeletingLastPathComponent stringByAppendingPathComponent:targetString]];
    }
    return nil;
}

// Atomic guest-VFS write: write everything to a temp file in the same directory,
// fsync it, then rename over the target. A mid-write failure (ENOSPC, ...) can then
// never leave the user's file truncated — the original survives untouched and the
// temp is unlinked. Falls back to an in-place write with stash/restore when the
// filesystem has no rename (or the symlink chain is too deep to resolve).
// Caller must already hold a borrowed task context.
- (BOOL)writeData:(NSData *)data toGuestPathViaVFS:(NSString *)guestPath error:(NSError **)error {
    NSString *target = [self resolvedGuestWriteTargetForPath:guestPath];
    if (target == nil)
        return [self writeDataInPlace:data toGuestPathViaVFS:guestPath mode:0644 error:error];

    // Preserve the existing file's permissions; refuse to clobber a non-regular file.
    int mode = 0644;
    BOOL existed = NO;
    struct statbuf st;
    memset(&st, 0, sizeof(st));
    if (generic_statat(AT_PWD, target.fileSystemRepresentation, &st, AT_SYMLINK_NOFOLLOW_) >= 0) {
        if (!S_ISREG(st.mode)) {
            if (error) *error = [self errorWithCode:0 message:@"That path is not a regular file"];
            return NO;
        }
        mode = (int)(st.mode & 07777);
        existed = YES;
    }

    // Deterministic temp name in the same directory: a leftover from a crashed
    // earlier save is unlinked and recreated (all writes go through one serial
    // queue, so two saves can never race on it). Unlink rather than truncate so a
    // stale temp with restrictive permissions can't make the reopen fail.
    NSString *tmpPath = [target.stringByDeletingLastPathComponent stringByAppendingPathComponent:
                         [NSString stringWithFormat:@".%@.motepad-tmp", target.lastPathComponent]];
    generic_unlinkat(AT_PWD, tmpPath.fileSystemRepresentation);  // ignore result; usually ENOENT

    struct fd *fd = generic_open(tmpPath.fileSystemRepresentation, O_WRONLY_ | O_CREAT_ | O_TRUNC_, mode);
    if (IS_ERR(fd)) {
        if (error) *error = [self errorWithCode:(int)PTR_ERR(fd) message:@"Cannot create file"];
        return NO;
    }
    const char *bytes = data.bytes;
    size_t remaining = data.length;
    BOOL ok = YES;
    while (remaining > 0) {
        ssize_t n = fd->ops->write(fd, bytes, remaining);
        if (n <= 0) { ok = NO; break; }
        bytes += n;
        remaining -= (size_t)n;
    }
    if (ok && fd->ops->fsync != NULL)
        fd->ops->fsync(fd);
    fd_close(fd);
    if (!ok) {
        generic_unlinkat(AT_PWD, tmpPath.fileSystemRepresentation);
        if (error) *error = [self errorWithCode:0 message:@"Failed writing file"];
        return NO;
    }

    // The rename replaces the target's inode with one the borrowed pid-1 task
    // created, so a save over a user-owned file would silently hand it to root
    // and the user's own session could no longer write it. Restore the displaced
    // owner on the temp first (best-effort; on a realfs mount chown maps to a
    // host fchownat the sandboxed app cannot perform). Same as -[ISHGuestFileBridge
    // writeDataViaVFS:toGuestPath:error:], until this store migrates onto it.
    if (existed) {
        generic_setattrat(AT_PWD, tmpPath.fileSystemRepresentation, make_attr(uid, st.uid), false);
        generic_setattrat(AT_PWD, tmpPath.fileSystemRepresentation, make_attr(gid, st.gid), false);
    }

    int err = generic_renameat(AT_PWD, tmpPath.fileSystemRepresentation,
                               AT_PWD, target.fileSystemRepresentation, 0);
    if (err < 0) {
        // Filesystem without rename (e.g. proc). Clean up the temp and fall back to
        // the guarded in-place write so saving still works where it possibly can.
        generic_unlinkat(AT_PWD, tmpPath.fileSystemRepresentation);
        return [self writeDataInPlace:data toGuestPathViaVFS:target mode:mode error:error];
    }
    return YES;
}

// Non-atomic fallback for filesystems without rename: stash the old contents first,
// attempt the truncating write, and restore the stash if the write fails so the
// user's file is never left truncated silently.
// Caller must already hold a borrowed task context.
- (BOOL)writeDataInPlace:(NSData *)data toGuestPathViaVFS:(NSString *)guestPath mode:(int)mode error:(NSError **)error {
    // Refuse to open-and-truncate anything that isn't a regular file (or absent).
    struct statbuf st;
    memset(&st, 0, sizeof(st));
    if (generic_statat(AT_PWD, guestPath.fileSystemRepresentation, &st, 0) >= 0 && !S_ISREG(st.mode)) {
        if (error) *error = [self errorWithCode:0 message:@"That path is not a regular file"];
        return NO;
    }
    NSData *previous = [self readGuestFileViaVFS:guestPath error:NULL];  // nil if the file is new

    struct fd *fd = generic_open(guestPath.fileSystemRepresentation, O_WRONLY_ | O_CREAT_ | O_TRUNC_, mode);
    if (IS_ERR(fd)) {
        if (error) *error = [self errorWithCode:(int)PTR_ERR(fd) message:@"Cannot create file"];
        return NO;
    }
    BOOL ok = [self writeAllData:data toOpenFd:fd];
    fd_close(fd);
    if (ok)
        return YES;

    // The write failed mid-stream; try to put the old contents back.
    BOOL restored = NO;
    if (previous != nil) {
        struct fd *restoreFd = generic_open(guestPath.fileSystemRepresentation,
                                            O_WRONLY_ | O_CREAT_ | O_TRUNC_, mode);
        if (!IS_ERR(restoreFd)) {
            restored = [self writeAllData:previous toOpenFd:restoreFd];
            fd_close(restoreFd);
        }
    }
    if (error) {
        NSString *message = restored
            ? @"Failed writing file (the previous contents were restored)"
            : @"Failed writing file, and the previous contents could not be restored";
        *error = [self errorWithCode:0 message:message];
    }
    return NO;
}

- (BOOL)writeAllData:(NSData *)data toOpenFd:(struct fd *)fd {
    const char *bytes = data.bytes;
    size_t remaining = data.length;
    while (remaining > 0) {
        ssize_t n = fd->ops->write(fd, bytes, remaining);
        if (n <= 0)
            return NO;
        bytes += n;
        remaining -= (size_t)n;
    }
    return YES;
}

#pragma mark Existence / defaults

- (BOOL)fileExistsAtGuestPath:(NSString *)guestPath isDirectory:(BOOL *)isDirectory {
    guestPath = [self normalizedGuestPath:guestPath];

    NSURL *hostURL = [self hostURLForPersistGuestPath:guestPath];
    if (hostURL != nil) {
        BOOL dir = NO;
        BOOL exists = [NSFileManager.defaultManager fileExistsAtPath:hostURL.path isDirectory:&dir];
        if (isDirectory) *isDirectory = dir;
        return exists;
    }

    __block BOOL exists = NO;
    __block BOOL dir = NO;
    [self withGuestTaskContext:^{
        struct statbuf st;
        memset(&st, 0, sizeof(st));
        if (generic_statat(AT_PWD, guestPath.fileSystemRepresentation, &st, 0) >= 0) {
            exists = YES;
            dir = S_ISDIR(st.mode);
        }
    }];
    if (isDirectory) *isDirectory = dir;
    return exists;
}

- (NSString *)defaultDirectoryGuestPath {
    BOOL isDir = NO;
    if ([self fileExistsAtGuestPath:@"/AOK/persist" isDirectory:&isDir] && isDir)
        return @"/AOK/persist";
    return @"/";
}

// Blocking worker for -modificationDateAtGuestPath:completion:.
- (NSDate *)modificationDateAtGuestPath:(NSString *)guestPath {
    guestPath = [self normalizedGuestPath:guestPath];

    NSURL *hostURL = [self hostURLForPersistGuestPath:guestPath];
    if (hostURL != nil) {
        NSDictionary *attributes = [NSFileManager.defaultManager attributesOfItemAtPath:hostURL.path error:NULL];
        return attributes.fileModificationDate;
    }

    __block NSDate *date = nil;
    [self withGuestTaskContext:^{
        struct statbuf st;
        memset(&st, 0, sizeof(st));
        if (generic_statat(AT_PWD, guestPath.fileSystemRepresentation, &st, 0) >= 0)
            date = [NSDate dateWithTimeIntervalSince1970:(NSTimeInterval)st.mtime];
    }];
    return date;
}

#pragma mark Helpers

- (NSString *)normalizedGuestPath:(NSString *)guestPath {
    NSString *path = guestPath ?: @"/";
    if (![path hasPrefix:@"/"])
        path = [@"/" stringByAppendingString:path];
    // Collapse a trailing slash (except the root itself) so path arithmetic is stable.
    while (path.length > 1 && [path hasSuffix:@"/"])
        path = [path substringToIndex:path.length - 1];
    return path;
}

- (NSError *)errorWithCode:(int)code message:(NSString *)message {
    return [NSError errorWithDomain:@"MotePadDocumentStore" code:code
                           userInfo:@{NSLocalizedDescriptionKey: message}];
}

@end
