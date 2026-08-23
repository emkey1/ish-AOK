//
//  GuestFileBridge.m
//  iSH-AOK
//

#import "GuestFileBridge.h"
#import "AppGroup.h"

#include <fcntl.h>
#include <stdio.h>
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
#include "fs/tty.h"
#include "util/sync.h"
#include "debug.h"

#ifndef DT_UNKNOWN
#define DT_UNKNOWN 0
#endif

NSString *const ISHGuestFileErrorDomain = @"ISHGuestFileErrorDomain";

static NSString *const kPersistGuestPrefix = @"/AOK/persist/";
static const NSUInteger kExtractionChunkSize = 1 << 20;  // 1 MiB
static const NSUInteger kMixedBackendCopyMaxBytes = 256 * 1024 * 1024;

static ISHGuestFileKind ISHGuestFileKindFromMode(mode_t mode) {
    if (S_ISDIR(mode)) return ISHGuestFileKindDirectory;
    if (S_ISREG(mode)) return ISHGuestFileKindRegular;
    if (S_ISLNK(mode)) return ISHGuestFileKindSymlink;
    if (S_ISFIFO(mode)) return ISHGuestFileKindFIFO;
    if (S_ISSOCK(mode)) return ISHGuestFileKindSocket;
    if (S_ISCHR(mode)) return ISHGuestFileKindCharDevice;
    if (S_ISBLK(mode)) return ISHGuestFileKindBlockDevice;
    return ISHGuestFileKindOther;
}

#pragma mark - ISHGuestFileItem

@implementation ISHGuestFileItem

- (BOOL)isSymlink {
    return self.symlinkTarget != nil;
}

- (BOOL)isBrokenSymlink {
    return self.symlinkTarget != nil && self.kind == ISHGuestFileKindSymlink;
}

@end

#pragma mark - Extraction cache entry

@interface ISHGuestFileExtractionCacheEntry : NSObject
@property (nonatomic, copy) NSString *guestPath;
@property (nonatomic) unsigned long long size;
@property (nonatomic, nullable) NSDate *modificationDate;
@property (nonatomic, copy) NSURL *fileURL;
@end

@implementation ISHGuestFileExtractionCacheEntry
@end

#pragma mark - ISHGuestFileBridge

@implementation ISHGuestFileBridge {
    // Plain dictionary, deliberately NOT an NSCache: NSCache evicts on its
    // own schedule (memory pressure, backgrounding), and evicting an entry
    // whose temp file a consumer still has open -- AVPlayer reopens local
    // files by path on seek -- would yank the file mid-use. Space is
    // reclaimed only by clearExtractionCache at app launch; the dictionary
    // also guarantees an unchanged file is never extracted twice, so disk
    // use is bounded by the set of distinct files opened in one app run.
    // Accessed only on ioQueue.
    NSMutableDictionary<NSString *, ISHGuestFileExtractionCacheEntry *> *_extractionCache;
    // Token bookkeeping, guarded by @synchronized(self): callers cancel from
    // the main thread while the ioQueue polls between chunks.
    NSMutableSet<NSUUID *> *_inflightExtractions;
    NSMutableSet<NSUUID *> *_cancelledExtractions;
}

+ (instancetype)sharedBridge {
    static ISHGuestFileBridge *shared;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ shared = [[ISHGuestFileBridge alloc] init]; });
    return shared;
}

- (instancetype)init {
    self = [super init];
    if (self) {
        _ioQueue = dispatch_queue_create("app.ish.guestfilebridge.io", DISPATCH_QUEUE_SERIAL);
        _cancelledExtractions = [NSMutableSet set];
        _inflightExtractions = [NSMutableSet set];
        _extractionCache = [NSMutableDictionary dictionary];
        [self extractionCacheDirectory];  // ensure it exists
    }
    return self;
}

#pragma mark Errors

- (NSError *)errorWithCode:(ISHGuestFileBridgeErrorCode)code message:(NSString *)message {
    return [NSError errorWithDomain:ISHGuestFileErrorDomain code:code
                           userInfo:@{NSLocalizedDescriptionKey: message}];
}

// `err` is a negative Linux-style errno (as returned by generic_open/generic_*
// via PTR_ERR). strerror() disagrees with Linux's numbering above the small
// POSIX-common range, but for the messages users actually see (ENOENT,
// EACCES, ENOTDIR, EEXIST, ...) the two agree, which is all this needs to be.
- (NSError *)errorWithGuestErrno:(long)err message:(NSString *)fallbackMessage {
    int magnitude = (int)(err < 0 ? -err : err);
    const char *cString = magnitude > 0 ? strerror(magnitude) : NULL;
    NSString *detail = cString != NULL ? [NSString stringWithUTF8String:cString] : nil;
    NSString *message = detail.length > 0 ? detail : fallbackMessage;
    return [NSError errorWithDomain:ISHGuestFileErrorDomain code:-magnitude
                           userInfo:@{NSLocalizedDescriptionKey: message}];
}

- (NSError *)notReadyError {
    return [self errorWithCode:ISHGuestFileBridgeErrorNotReady message:@"The guest filesystem isn't ready yet"];
}

- (NSError *)hostErrnoError {
    return [NSError errorWithDomain:NSPOSIXErrorDomain code:errno
                           userInfo:@{NSLocalizedDescriptionKey: [NSString stringWithUTF8String:strerror(errno)] ?: @"Unknown error"}];
}

#pragma mark Borrowed task context

// Run a guest-VFS operation under a borrowed task context. fs/path.c resolves
// paths through the thread-local `current` task, which is unset on GCD/UI
// threads, so we borrow pid 1 exactly as the app's boot code does
// (PushInitTaskAsCurrent) and restore afterward. Returns NO if init isn't
// usable yet (e.g. before the guest has booted). Identical in AudioLibrary.m
// and MotePadDocumentStore.m; those migrate onto this bridge in a later pass.
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

- (BOOL)isGuestAvailable {
    struct task *init = pid_get_task_ref(1);
    if (init == NULL)
        return NO;
    bool usable;
    lock(&init->general_lock, 0);
    usable = init->mm != NULL && init->mem != NULL && init->files != NULL && init->fs != NULL;
    unlock(&init->general_lock);
    task_ref_cnt_mod(init, -1);
    return usable;
}

#pragma mark Realfs (/AOK/persist) fast path

- (nullable NSURL *)persistHostBaseURL {
    NSURL *container = ContainerURL();
    if (container == nil) return nil;
    return [[container URLByAppendingPathComponent:@"AOK" isDirectory:YES]
            URLByAppendingPathComponent:@"persist" isDirectory:YES];
}

- (nullable NSURL *)hostURLForRealfsGuestPath:(NSString *)guestPath {
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
        // ".." would walk the host URL out of the persist base into the rest
        // of the app container -- decline the fast path and let the VFS
        // resolve it (path_normalize handles dot-dot inside guest namespace).
        if ([component isEqualToString:@"."] || [component isEqualToString:@".."])
            return nil;
        hostURL = [hostURL URLByAppendingPathComponent:component];
    }
    return hostURL;
}

// The persist base with host symlinks resolved (e.g. /var vs /private/var),
// with a trailing slash so prefix checks can't match a sibling like
// ".../persistXYZ". Used to confirm a resolved symlink target stayed inside
// the persist area before treating it as realfs-reachable.
- (nullable NSString *)resolvedPersistBasePrefix {
    NSURL *base = [self persistHostBaseURL];
    if (base == nil) return nil;
    NSString *resolved = base.URLByResolvingSymlinksInPath.path;
    if (resolved.length == 0) return nil;
    return [resolved hasSuffix:@"/"] ? resolved : [resolved stringByAppendingString:@"/"];
}

- (NSString *)normalizedGuestPath:(NSString *)guestPath {
    NSString *path = guestPath.length ? guestPath : @"/";
    if (![path hasPrefix:@"/"])
        path = [@"/" stringByAppendingString:path];
    while (path.length > 1 && [path hasSuffix:@"/"])
        path = [path substringToIndex:path.length - 1];
    return path;
}

#pragma mark Item construction

- (ISHGuestFileKind)kindFromHostFileType:(nullable NSString *)fileType {
    if ([fileType isEqualToString:NSFileTypeDirectory]) return ISHGuestFileKindDirectory;
    if ([fileType isEqualToString:NSFileTypeRegular]) return ISHGuestFileKindRegular;
    if ([fileType isEqualToString:NSFileTypeSymbolicLink]) return ISHGuestFileKindSymlink;
    if ([fileType isEqualToString:NSFileTypeSocket]) return ISHGuestFileKindSocket;
    if ([fileType isEqualToString:NSFileTypeCharacterSpecial]) return ISHGuestFileKindCharDevice;
    if ([fileType isEqualToString:NSFileTypeBlockSpecial]) return ISHGuestFileKindBlockDevice;
    return ISHGuestFileKindOther;  // NSFileManager has no distinct FIFO type; rare under /AOK/persist anyway
}

// Builds an item for a host path (realfs). For a symlink, resolves the chain
// (URLByResolvingSymlinksInPath, like realpath) to report the target's kind
// and size; a target that doesn't exist leaves kind = Symlink (broken).
- (nullable ISHGuestFileItem *)itemForHostURL:(NSURL *)hostURL guestPath:(NSString *)guestPath {
    NSDictionary<NSFileAttributeKey, id> *attrs = [NSFileManager.defaultManager attributesOfItemAtPath:hostURL.path error:NULL];
    if (attrs == nil)
        return nil;

    ISHGuestFileItem *item = [ISHGuestFileItem new];
    item.name = guestPath.lastPathComponent;
    item.guestPath = guestPath;
    item.posixMode = [attrs[NSFilePosixPermissions] unsignedShortValue];
    item.uid = [attrs[NSFileOwnerAccountID] unsignedIntValue];
    item.gid = [attrs[NSFileGroupOwnerAccountID] unsignedIntValue];
    item.size = [attrs[NSFileSize] unsignedLongLongValue];
    item.modificationDate = attrs[NSFileModificationDate];
    item.kind = [self kindFromHostFileType:attrs[NSFileType]];
    item.realfsURL = hostURL;

    if (![attrs[NSFileType] isEqualToString:NSFileTypeSymbolicLink])
        return item;

    NSString *target = [NSFileManager.defaultManager destinationOfSymbolicLinkAtPath:hostURL.path error:NULL];
    item.symlinkTarget = target ?: @"";

    // Only follow the link if its target stays inside the persist area. An
    // absolute target like "/etc/hosts" names a guest path, but resolving it
    // with host APIs would land on iOS's /etc/hosts -- the wrong namespace.
    // Escaping links display as unresolved rather than showing host files.
    NSString *resolved = hostURL.URLByResolvingSymlinksInPath.path;
    NSString *basePrefix = [self resolvedPersistBasePrefix];
    if (resolved != nil && (basePrefix == nil || ![resolved hasPrefix:basePrefix]))
        resolved = nil;
    NSDictionary *followedAttrs = resolved != nil ? [NSFileManager.defaultManager attributesOfItemAtPath:resolved error:NULL] : nil;
    if (followedAttrs != nil) {
        item.kind = [self kindFromHostFileType:followedAttrs[NSFileType]];
        item.size = [followedAttrs[NSFileSize] unsignedLongLongValue];
        item.modificationDate = followedAttrs[NSFileModificationDate];
        item.realfsURL = [NSURL fileURLWithPath:resolved];
    } else {
        item.kind = ISHGuestFileKindSymlink;
        item.realfsURL = nil;
    }
    return item;
}

// Caller must already hold a borrowed task context. Classifies by the lstat
// result; a symlink entry gets a follow-up stat to resolve size/kind for
// display (a symlinked file/folder shows its target's icon+size with an
// alias badge, matching Finder), falling back to the symlink's own info if
// the target is missing (broken link).
- (nullable ISHGuestFileItem *)itemForGuestPathViaVFS:(NSString *)guestPath {
    // Resolving from AT_PWD walks the whole path. That is the right thing for a
    // one-off stat; -listGuestDirectoryViaVFS: passes its open directory and a
    // bare name instead, which is the same answer for one component of work.
    return [self itemAtName:guestPath.fileSystemRepresentation
                 relativeTo:AT_PWD
                  guestPath:guestPath];
}

// `name` is resolved relative to `at` -- either AT_PWD with a full path, or an
// open directory with a single component. `guestPath` is the absolute path the
// item should report regardless, since every consumer navigates and acts on it.
//
// Caller must already hold a borrowed task context.
- (nullable ISHGuestFileItem *)itemAtName:(const char *)name
                                relativeTo:(struct fd *)at
                                 guestPath:(NSString *)guestPath {
    struct statbuf lst;
    memset(&lst, 0, sizeof(lst));
    if (generic_statat(at, name, &lst, AT_SYMLINK_NOFOLLOW_) < 0)
        return nil;  // dangling entry (TOCTOU race) or permission error; caller skips it

    ISHGuestFileItem *item = [ISHGuestFileItem new];
    item.name = guestPath.lastPathComponent;
    item.guestPath = guestPath;
    item.posixMode = (mode_t)lst.mode;
    item.uid = (uid_t)lst.uid;
    item.gid = (gid_t)lst.gid;
    item.size = lst.size;
    item.modificationDate = [NSDate dateWithTimeIntervalSince1970:(NSTimeInterval)lst.mtime];
    item.kind = ISHGuestFileKindFromMode((mode_t)lst.mode);

    if (!S_ISLNK((mode_t)lst.mode))
        return item;

    char target[MAX_PATH];
    ssize_t n = generic_readlinkat(at, name, target, sizeof(target) - 1);
    if (n > 0) {
        target[n] = '\0';
        item.symlinkTarget = [NSString stringWithUTF8String:target] ?: @"";
    } else {
        item.symlinkTarget = @"";  // still mark it a symlink even if readlink itself failed
    }

    struct statbuf followed;
    memset(&followed, 0, sizeof(followed));
    if (generic_statat(at, name, &followed, 0) >= 0) {
        item.kind = ISHGuestFileKindFromMode((mode_t)followed.mode);
        item.size = followed.size;
        item.modificationDate = [NSDate dateWithTimeIntervalSince1970:(NSTimeInterval)followed.mtime];
    } else {
        item.kind = ISHGuestFileKindSymlink;  // broken link
    }
    return item;
}

#pragma mark Directory listing

- (void)listDirectoryAtGuestPath:(NSString *)guestPath
                       completion:(void (^)(NSArray<ISHGuestFileItem *> * _Nullable, NSError * _Nullable))completion {
    NSString *path = [self normalizedGuestPath:guestPath];
    dispatch_async(self.ioQueue, ^{
        NSArray<ISHGuestFileItem *> *items = nil;
        NSError *error = nil;
        NSURL *hostDir = [self hostURLForRealfsGuestPath:path];
        BOOL hostIsDir = NO;
        BOOL hostExists = hostDir != nil && [NSFileManager.defaultManager fileExistsAtPath:hostDir.path isDirectory:&hostIsDir];
        if (hostExists && hostIsDir) {
            items = [self listHostDirectory:hostDir guestBasePath:path];
        } else if (hostExists) {
            error = [self errorWithCode:ISHGuestFileBridgeErrorNotDirectory message:@"That path is not a directory"];
        } else if (hostDir != nil) {
            error = [self errorWithGuestErrno:_ENOENT message:@"No such file or directory"];
        } else {
            __block NSArray<ISHGuestFileItem *> *vfsItems = nil;
            __block NSError *vfsError = nil;
            BOOL hadContext = [self withGuestTaskContext:^{
                vfsItems = [self listGuestDirectoryViaVFS:path error:&vfsError];
            }];
            if (!hadContext) error = [self notReadyError];
            else { items = vfsItems; error = vfsError; }
        }
        NSArray<ISHGuestFileItem *> *result = items;
        NSError *finalError = error ?: (result == nil ? [self errorWithCode:ISHGuestFileBridgeErrorUnknown message:@"Unknown error"] : nil);
        dispatch_async(dispatch_get_main_queue(), ^{ if (completion) completion(result, finalError); });
    });
}

- (NSArray<ISHGuestFileItem *> *)listHostDirectory:(NSURL *)hostDir guestBasePath:(NSString *)guestBasePath {
    NSArray<NSString *> *names = [NSFileManager.defaultManager contentsOfDirectoryAtPath:hostDir.path error:NULL] ?: @[];
    NSMutableArray<ISHGuestFileItem *> *items = [NSMutableArray arrayWithCapacity:names.count];
    for (NSString *name in names) {
        NSURL *childURL = [hostDir URLByAppendingPathComponent:name];
        NSString *childGuestPath = [guestBasePath stringByAppendingPathComponent:name];
        ISHGuestFileItem *item = [self itemForHostURL:childURL guestPath:childGuestPath];
        if (item != nil) [items addObject:item];
    }
    [items sortUsingComparator:^NSComparisonResult(ISHGuestFileItem *a, ISHGuestFileItem *b) {
        return [a.name caseInsensitiveCompare:b.name];
    }];
    return items;
}

// Caller must already hold a borrowed task context. O_NONBLOCK_ so being
// handed a FIFO path can never park the ioQueue in a blocking open; the
// S_ISDIR check below rejects it (harmless for real directories).
- (nullable NSArray<ISHGuestFileItem *> *)listGuestDirectoryViaVFS:(NSString *)guestPath error:(NSError **)error {
    struct fd *fd = generic_open(guestPath.fileSystemRepresentation, O_RDONLY_ | O_NONBLOCK_, 0);
    if (IS_ERR(fd)) {
        if (error) *error = [self errorWithGuestErrno:(long)PTR_ERR(fd) message:@"Cannot open directory"];
        return nil;
    }
    if (!S_ISDIR(fd->type) || fd->ops->readdir == NULL) {
        fd_close(fd);
        if (error) *error = [self errorWithCode:ISHGuestFileBridgeErrorNotDirectory message:@"That path is not a directory"];
        return nil;
    }

    NSMutableArray<NSString *> *names = [NSMutableArray array];
    if (fd->ops->readdir_begin) fd->ops->readdir_begin(fd);
    while (true) {
        struct dir_entry entry;
        memset(&entry, 0, sizeof(entry));
        entry.type = DT_UNKNOWN;
        int err = fd->ops->readdir(fd, &entry);
        if (err <= 0) break;  // 0 = end, <0 = error
        NSString *name = [NSString stringWithUTF8String:entry.name];
        if (name == nil || [name isEqualToString:@"."] || [name isEqualToString:@".."])
            continue;
        [names addObject:name];
    }
    if (fd->ops->readdir_end) fd->ops->readdir_end(fd);

    // Stat each entry against the directory we already have open, NOT by its
    // absolute path. Closing the fd here and rebuilding "/a/b/c/name" per entry
    // made every stat re-resolve every component from the root -- and each
    // component costs a readlink attempt AND a stat inside fs/path.c, so the
    // work was O(entries x depth) with a fakefs read transaction per step.
    // Against the open directory it is one component per entry, and a symlink's
    // extra readlink and follow-stat get the same reduction (they were three
    // full walks per link, which is why symlink-dense directories like /bin
    // were the worst case).
    NSMutableArray<ISHGuestFileItem *> *items = [NSMutableArray arrayWithCapacity:names.count];
    for (NSString *name in names) {
        // Per-iteration pool: the path string and -fileSystemRepresentation's
        // buffer are autoreleased, and on a directory with tens of thousands of
        // entries they would otherwise all live until the listing finished. The
        // item itself is retained by `items` and survives the drain.
        @autoreleasepool {
            NSString *childPath = [guestPath stringByAppendingPathComponent:name];
            ISHGuestFileItem *item = [self itemAtName:name.fileSystemRepresentation
                                           relativeTo:fd
                                            guestPath:childPath];
            if (item != nil) [items addObject:item];  // skip TOCTOU-dangling entries rather than aborting the listing
        }
    }
    fd_close(fd);
    [items sortUsingComparator:^NSComparisonResult(ISHGuestFileItem *a, ISHGuestFileItem *b) {
        return [a.name caseInsensitiveCompare:b.name];
    }];
    return items;
}

#pragma mark Stat

// Must run on ioQueue.
- (nullable ISHGuestFileItem *)statSync:(NSString *)guestPath error:(NSError **)error {
    NSURL *hostURL = [self hostURLForRealfsGuestPath:guestPath];
    if (hostURL != nil) {
        ISHGuestFileItem *item = [self itemForHostURL:hostURL guestPath:guestPath];
        if (item == nil && error) *error = [self errorWithGuestErrno:_ENOENT message:@"No such file or directory"];
        return item;
    }

    __block ISHGuestFileItem *item = nil;
    __block NSError *vfsError = nil;
    BOOL hadContext = [self withGuestTaskContext:^{
        struct statbuf st;
        memset(&st, 0, sizeof(st));
        int err = generic_statat(AT_PWD, guestPath.fileSystemRepresentation, &st, 0);
        if (err < 0) { vfsError = [self errorWithGuestErrno:err message:@"Cannot stat path"]; return; }
        item = [ISHGuestFileItem new];
        item.name = guestPath.lastPathComponent;
        item.guestPath = guestPath;
        item.posixMode = (mode_t)st.mode;
        item.uid = (uid_t)st.uid;
        item.gid = (gid_t)st.gid;
        item.size = st.size;
        item.modificationDate = [NSDate dateWithTimeIntervalSince1970:(NSTimeInterval)st.mtime];
        item.kind = ISHGuestFileKindFromMode((mode_t)st.mode);
    }];
    if (!hadContext) { if (error) *error = [self notReadyError]; return nil; }
    if (item == nil && error) *error = vfsError;
    return item;
}

- (void)statAtGuestPath:(NSString *)guestPath completion:(void (^)(ISHGuestFileItem * _Nullable, NSError * _Nullable))completion {
    NSString *path = [self normalizedGuestPath:guestPath];
    dispatch_async(self.ioQueue, ^{
        NSError *error = nil;
        ISHGuestFileItem *item = [self statSync:path error:&error];
        dispatch_async(dispatch_get_main_queue(), ^{ if (completion) completion(item, error); });
    });
}

#pragma mark Filesystem status (statfs)

- (void)filesystemStatusAtGuestPath:(NSString *)guestPath
                          completion:(void (^)(int64_t, int64_t, NSError * _Nullable))completion {
    NSString *path = [self normalizedGuestPath:guestPath];
    dispatch_async(self.ioQueue, ^{
        NSURL *hostURL = [self hostURLForRealfsGuestPath:path];
        if (hostURL != nil) {
            NSDictionary<NSFileAttributeKey, id> *attrs =
                [NSFileManager.defaultManager attributesOfFileSystemForPath:hostURL.path error:NULL];
            int64_t available = [attrs[NSFileSystemFreeSize] longLongValue];
            int64_t total = [attrs[NSFileSystemSize] longLongValue];
            dispatch_async(dispatch_get_main_queue(), ^{ if (completion) completion(available, total, nil); });
            return;
        }

        __block int64_t available = 0;
        __block int64_t total = 0;
        __block NSError *vfsError = nil;
        BOOL hadContext = [self withGuestTaskContext:^{
            // Mirrors sys_statfs (kernel/fs.c): normalize, find the mount, ask
            // it via the same mount_statfs the syscalls use -- which follows a
            // bind to its backing mount, where calling mount->fs->statfs
            // directly reported EBADF. Filesystems without a statfs op (proc,
            // devpts) just leave the buffer zeroed -- reported as 0/0.
            char normalized[MAX_PATH];
            int err = path_normalize(AT_PWD, path.fileSystemRepresentation, normalized, N_SYMLINK_NOFOLLOW);
            if (err < 0) { vfsError = [self errorWithGuestErrno:err message:@"Cannot resolve path"]; return; }
            struct mount *mount = mount_find(normalized);
            if (mount == NULL) { vfsError = [self errorWithGuestErrno:_ENOENT message:@"No such mount"]; return; }
            struct statfsbuf buf;
            memset(&buf, 0, sizeof(buf));
            err = mount_statfs(mount, &buf);
            mount_release(mount);
            if (err < 0) { vfsError = [self errorWithGuestErrno:err message:@"Cannot stat filesystem"]; return; }
            long blockSize = buf.frsize > 0 ? buf.frsize : buf.bsize;
            if (blockSize > 0) {
                available = (int64_t)buf.bavail * blockSize;
                total = (int64_t)buf.blocks * blockSize;
            }
        }];
        NSError *error = hadContext ? vfsError : [self notReadyError];
        int64_t finalAvailable = available, finalTotal = total;
        dispatch_async(dispatch_get_main_queue(), ^{
            if (completion) completion(error == nil ? finalAvailable : 0, error == nil ? finalTotal : 0, error);
        });
    });
}

#pragma mark Reading

- (void)readFileAtGuestPath:(NSString *)guestPath maxBytes:(NSUInteger)maxBytes
                  completion:(void (^)(NSData * _Nullable, NSError * _Nullable))completion {
    NSString *path = [self normalizedGuestPath:guestPath];
    dispatch_async(self.ioQueue, ^{
        NSError *error = nil;
        NSData *data = [self readDataSync:path maxBytes:maxBytes error:&error];
        dispatch_async(dispatch_get_main_queue(), ^{ if (completion) completion(data, data != nil ? nil : error); });
    });
}

// Must run on ioQueue.
- (nullable NSData *)readDataSync:(NSString *)guestPath maxBytes:(NSUInteger)maxBytes error:(NSError **)error {
    NSURL *hostURL = [self hostURLForRealfsGuestPath:guestPath];
    if (hostURL != nil)
        return [self readHostRegularFileAtURL:hostURL maxBytes:maxBytes error:error];

    __block NSData *data = nil;
    __block NSError *vfsError = nil;
    BOOL hadContext = [self withGuestTaskContext:^{
        data = [self readGuestFileViaVFS:guestPath maxBytes:maxBytes error:&vfsError];
    }];
    if (!hadContext) { if (error) *error = [self notReadyError]; return nil; }
    if (data == nil && error) *error = vfsError;
    return data;
}

// Opens with O_NONBLOCK so a TOCTOU race (a FIFO appearing where a regular
// file was expected) can't block the io queue; the fstat check afterward
// rejects anything that isn't a regular file.
- (nullable NSData *)readHostRegularFileAtURL:(NSURL *)hostURL maxBytes:(NSUInteger)maxBytes error:(NSError **)error {
    int fd = open(hostURL.fileSystemRepresentation, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        if (error) *error = [self hostErrnoError];
        return nil;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        if (error) *error = [self errorWithCode:ISHGuestFileBridgeErrorNotRegularFile message:@"That path is not a regular file"];
        return nil;
    }
    if ((unsigned long long)st.st_size > maxBytes) {
        close(fd);
        if (error) *error = [self errorWithCode:ISHGuestFileBridgeErrorTooLarge message:@"File is larger than the allowed read size"];
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
                                            userInfo:@{NSLocalizedDescriptionKey: [NSString stringWithUTF8String:strerror(readErrno)] ?: @"Unknown error"}];
        return nil;
    }
    return data;
}

// Caller must already hold a borrowed task context. O_NONBLOCK_ so a FIFO
// slipping past a caller's earlier classification can't block; the type
// check below then rejects it (and every other non-regular file) cleanly.
- (nullable NSData *)readGuestFileViaVFS:(NSString *)guestPath maxBytes:(NSUInteger)maxBytes error:(NSError **)error {
    struct statbuf precheck;
    memset(&precheck, 0, sizeof(precheck));
    if (generic_statat(AT_PWD, guestPath.fileSystemRepresentation, &precheck, 0) >= 0 && precheck.size > maxBytes) {
        if (error) *error = [self errorWithCode:ISHGuestFileBridgeErrorTooLarge message:@"File is larger than the allowed read size"];
        return nil;
    }

    struct fd *fd = generic_open(guestPath.fileSystemRepresentation, O_RDONLY_ | O_NONBLOCK_, 0);
    if (IS_ERR(fd)) {
        if (error) *error = [self errorWithGuestErrno:(long)PTR_ERR(fd) message:@"Cannot open file"];
        return nil;
    }
    if (S_ISDIR(fd->type)) {
        fd_close(fd);
        if (error) *error = [self errorWithCode:ISHGuestFileBridgeErrorIsDirectory message:@"That path is a directory"];
        return nil;
    }
    if (!S_ISREG(fd->type)) {
        fd_close(fd);
        if (error) *error = [self errorWithCode:ISHGuestFileBridgeErrorNotRegularFile message:@"That path is not a regular file"];
        return nil;
    }

    NSMutableData *data = [NSMutableData data];
    char buffer[65536];
    ssize_t n;
    BOOL tooLarge = NO;
    while ((n = fd->ops->read(fd, buffer, sizeof(buffer))) > 0) {
        if (data.length + (NSUInteger)n > maxBytes) { tooLarge = YES; break; }
        [data appendBytes:buffer length:(NSUInteger)n];
    }
    ssize_t lastErr = (n < 0) ? n : 0;
    fd_close(fd);
    if (tooLarge) {
        if (error) *error = [self errorWithCode:ISHGuestFileBridgeErrorTooLarge message:@"File is larger than the allowed read size"];
        return nil;
    }
    if (lastErr < 0) {
        if (error) *error = [self errorWithGuestErrno:lastErr message:@"Failed reading file"];
        return nil;
    }
    return data;
}

#pragma mark Extraction (VFS -> temp file, for native frameworks)

- (NSURL *)extractionCacheDirectory {
    NSURL *dir = [NSURL fileURLWithPath:[NSTemporaryDirectory() stringByAppendingPathComponent:@"GuestFileBridgeExtractions"]];
    [NSFileManager.defaultManager createDirectoryAtURL:dir withIntermediateDirectories:YES attributes:nil error:NULL];
    return dir;
}

- (void)clearExtractionCache {
    dispatch_async(self.ioQueue, ^{
        [self->_extractionCache removeAllObjects];
        NSURL *dir = [self extractionCacheDirectory];
        [NSFileManager.defaultManager removeItemAtURL:dir error:NULL];
        [self extractionCacheDirectory];  // recreate empty
    });
}

- (nullable NSURL *)cachedExtractionURLForGuestPath:(NSString *)guestPath
                                                 size:(unsigned long long)size
                                     modificationDate:(nullable NSDate *)modificationDate {
    ISHGuestFileExtractionCacheEntry *entry = [_extractionCache objectForKey:guestPath];
    if (entry == nil) return nil;
    if (entry.size != size) return nil;
    if (entry.modificationDate != modificationDate && ![entry.modificationDate isEqualToDate:modificationDate]) return nil;
    if (![NSFileManager.defaultManager fileExistsAtPath:entry.fileURL.path]) return nil;
    return entry.fileURL;
}

- (void)cacheExtractedURL:(NSURL *)url forGuestPath:(NSString *)guestPath
                       size:(unsigned long long)size modificationDate:(nullable NSDate *)modificationDate {
    ISHGuestFileExtractionCacheEntry *entry = [ISHGuestFileExtractionCacheEntry new];
    entry.guestPath = guestPath;
    entry.size = size;
    entry.modificationDate = modificationDate;
    entry.fileURL = url;
    _extractionCache[guestPath] = entry;
}

- (void)beginExtraction:(NSUUID *)token {
    @synchronized (self) {
        [_inflightExtractions addObject:token];
    }
}

- (BOOL)isExtractionCancelled:(NSUUID *)token {
    @synchronized (self) {
        return [_cancelledExtractions containsObject:token];
    }
}

// Ignores tokens that aren't in flight: cancelling after completion (a
// window closing that always cancels defensively, say) must not grow the
// cancelled set with UUIDs nothing will ever remove.
- (void)cancelExtraction:(ISHGuestFileExtractionToken)token {
    if (token == nil) return;
    @synchronized (self) {
        if ([_inflightExtractions containsObject:token])
            [_cancelledExtractions addObject:token];
    }
}

- (void)finishExtraction:(NSUUID *)token {
    @synchronized (self) {
        [_inflightExtractions removeObject:token];
        [_cancelledExtractions removeObject:token];
    }
}

- (ISHGuestFileExtractionToken)extractToTempFileAtGuestPath:(NSString *)guestPath
    progress:(void (^)(int64_t, int64_t))progress
    completion:(void (^)(NSURL * _Nullable, NSError * _Nullable))completion {
    NSUUID *token = [NSUUID UUID];
    NSString *path = [self normalizedGuestPath:guestPath];
    [self beginExtraction:token];  // before returning it, so an immediate cancel is honored

    dispatch_async(self.ioQueue, ^{
        if ([self isExtractionCancelled:token]) {
            [self finishExtraction:token];
            dispatch_async(dispatch_get_main_queue(), ^{
                if (completion) completion(nil, [self errorWithCode:ISHGuestFileBridgeErrorCancelled message:@"Cancelled"]);
            });
            return;
        }

        NSError *statError = nil;
        ISHGuestFileItem *info = [self statSync:path error:&statError];
        if (info == nil) {
            [self finishExtraction:token];
            dispatch_async(dispatch_get_main_queue(), ^{ if (completion) completion(nil, statError); });
            return;
        }
        if (info.kind == ISHGuestFileKindDirectory) {
            [self finishExtraction:token];
            dispatch_async(dispatch_get_main_queue(), ^{
                if (completion) completion(nil, [self errorWithCode:ISHGuestFileBridgeErrorIsDirectory message:@"That path is a directory"]);
            });
            return;
        }
        if (info.realfsURL != nil) {
            // Realfs-backed: the host file already exists, no copy needed.
            [self finishExtraction:token];
            dispatch_async(dispatch_get_main_queue(), ^{ if (completion) completion(info.realfsURL, nil); });
            return;
        }

        NSURL *cached = [self cachedExtractionURLForGuestPath:path size:info.size modificationDate:info.modificationDate];
        if (cached != nil) {
            [self finishExtraction:token];
            dispatch_async(dispatch_get_main_queue(), ^{ if (completion) completion(cached, nil); });
            return;
        }

        __block NSURL *result = nil;
        __block NSError *vfsError = nil;
        int64_t total = (int64_t)info.size;
        BOOL hadContext = [self withGuestTaskContext:^{
            result = [self extractGuestPathViaVFS:path token:token total:total progress:progress error:&vfsError];
        }];
        [self finishExtraction:token];
        if (!hadContext) {
            dispatch_async(dispatch_get_main_queue(), ^{ if (completion) completion(nil, [self notReadyError]); });
            return;
        }
        if (result != nil)
            [self cacheExtractedURL:result forGuestPath:path size:info.size modificationDate:info.modificationDate];
        dispatch_async(dispatch_get_main_queue(), ^{ if (completion) completion(result, result != nil ? nil : vfsError); });
    });

    return token;
}

// Caller must already hold a borrowed task context. O_NONBLOCK_ is load-
// bearing: a FIFO with no writer blocks generic_open forever otherwise
// (fs/fifo.c), and a wedged open here parks the serial ioQueue -- every
// bridge operation app-wide would hang with no recovery. The S_ISREG check
// then rejects the FIFO (and sockets/devices) cleanly.
- (nullable NSURL *)extractGuestPathViaVFS:(NSString *)guestPath
                                       token:(NSUUID *)token
                                       total:(int64_t)totalBytes
                                    progress:(void (^)(int64_t, int64_t))progress
                                       error:(NSError **)error {
    struct fd *fd = generic_open(guestPath.fileSystemRepresentation, O_RDONLY_ | O_NONBLOCK_, 0);
    if (IS_ERR(fd)) {
        if (error) *error = [self errorWithGuestErrno:(long)PTR_ERR(fd) message:@"Cannot open guest file"];
        return nil;
    }
    if (S_ISDIR(fd->type)) {
        fd_close(fd);
        if (error) *error = [self errorWithCode:ISHGuestFileBridgeErrorIsDirectory message:@"That path is a directory"];
        return nil;
    }
    if (!S_ISREG(fd->type)) {
        fd_close(fd);
        if (error) *error = [self errorWithCode:ISHGuestFileBridgeErrorNotRegularFile message:@"That path is not a regular file"];
        return nil;
    }

    NSString *ext = guestPath.pathExtension;
    NSString *tempName = [[NSUUID UUID].UUIDString stringByAppendingPathExtension:ext.length ? ext : @"tmp"];
    NSURL *tempURL = [[self extractionCacheDirectory] URLByAppendingPathComponent:tempName];

    FILE *out = fopen(tempURL.fileSystemRepresentation, "wb");
    if (out == NULL) {
        fd_close(fd);
        if (error) *error = [self errorWithCode:ISHGuestFileBridgeErrorUnknown message:@"Cannot create temp file"];
        return nil;
    }

    char *buffer = malloc(kExtractionChunkSize);
    if (buffer == NULL) {
        fclose(out);
        fd_close(fd);
        if (error) *error = [self errorWithCode:ISHGuestFileBridgeErrorUnknown message:@"Out of memory"];
        return nil;
    }

    ssize_t n;
    int64_t written = 0;
    BOOL ok = YES, cancelled = NO;
    while ((n = fd->ops->read(fd, buffer, kExtractionChunkSize)) > 0) {
        if (fwrite(buffer, 1, (size_t)n, out) != (size_t)n) { ok = NO; break; }
        written += n;
        if (progress) {
            dispatch_async(dispatch_get_main_queue(), ^{ progress(written, totalBytes); });
        }
        if ([self isExtractionCancelled:token]) { cancelled = YES; break; }
    }
    if (n < 0 && ok) ok = NO;
    free(buffer);
    fclose(out);
    fd_close(fd);

    if (cancelled) {
        [NSFileManager.defaultManager removeItemAtURL:tempURL error:NULL];
        if (error) *error = [self errorWithCode:ISHGuestFileBridgeErrorCancelled message:@"Cancelled"];
        return nil;
    }
    if (!ok) {
        [NSFileManager.defaultManager removeItemAtURL:tempURL error:NULL];
        if (error) *error = [self errorWithCode:ISHGuestFileBridgeErrorUnknown message:@"Failed reading guest file"];
        return nil;
    }
    return tempURL;
}

#pragma mark Writing

- (void)writeData:(NSData *)data toGuestPath:(NSString *)guestPath
        completion:(void (^)(BOOL, NSError * _Nullable))completion {
    NSData *snapshot = [data copy];
    NSString *path = [self normalizedGuestPath:guestPath];
    dispatch_async(self.ioQueue, ^{
        NSError *error = nil;
        BOOL ok = [self writeDataSync:snapshot toGuestPath:path error:&error];
        dispatch_async(dispatch_get_main_queue(), ^{ if (completion) completion(ok, ok ? nil : error); });
    });
}

// Must run on ioQueue.
- (BOOL)writeDataSync:(NSData *)data toGuestPath:(NSString *)guestPath error:(NSError **)error {
    NSURL *hostURL = [self hostURLForRealfsGuestPath:guestPath];
    if (hostURL != nil) {
        // A symlink to a regular file is written through (NSDataWritingAtomic
        // would otherwise replace the link itself); anything else that exists
        // and isn't a regular file (FIFO, socket, device) is refused.
        struct stat lst;
        if (lstat(hostURL.fileSystemRepresentation, &lst) == 0) {
            if (S_ISLNK(lst.st_mode)) {
                char resolved[PATH_MAX];
                if (realpath(hostURL.fileSystemRepresentation, resolved) == NULL) {
                    if (error) *error = [self errorWithCode:ISHGuestFileBridgeErrorNotRegularFile message:@"Cannot save through a broken symbolic link"];
                    return NO;
                }
                // A link whose target leaves the persist area names a guest
                // path (e.g. "/etc/hosts"); following it with host APIs would
                // write to iOS's file of that name. Refuse rather than cross
                // namespaces -- the VFS route handles such links correctly.
                NSString *resolvedPath = [NSString stringWithUTF8String:resolved] ?: @"";
                NSString *basePrefix = [self resolvedPersistBasePrefix];
                if (basePrefix == nil || ![resolvedPath hasPrefix:basePrefix]) {
                    if (error) *error = [self errorWithCode:ISHGuestFileBridgeErrorNotRegularFile message:@"That link points outside the persist area"];
                    return NO;
                }
                struct stat rst;
                if (stat(resolved, &rst) != 0 || !S_ISREG(rst.st_mode)) {
                    if (error) *error = [self errorWithCode:ISHGuestFileBridgeErrorNotRegularFile message:@"That path is not a regular file"];
                    return NO;
                }
                hostURL = [NSURL fileURLWithPath:resolvedPath isDirectory:NO];
            } else if (!S_ISREG(lst.st_mode)) {
                if (error) *error = [self errorWithCode:ISHGuestFileBridgeErrorNotRegularFile message:@"That path is not a regular file"];
                return NO;
            }
        }
        [NSFileManager.defaultManager createDirectoryAtURL:hostURL.URLByDeletingLastPathComponent
                               withIntermediateDirectories:YES attributes:nil error:NULL];
        return [data writeToURL:hostURL options:NSDataWritingAtomic error:error];
    }

    __block BOOL ok = NO;
    __block NSError *vfsError = nil;
    BOOL hadContext = [self withGuestTaskContext:^{
        ok = [self writeDataViaVFS:data toGuestPath:guestPath error:&vfsError];
    }];
    if (!hadContext) {
        if (error) *error = [self notReadyError];
        return NO;
    }
    if (!ok && error) *error = vfsError;
    return ok;
}

// Resolves guest symlinks in the final path component so a save through a
// symlink updates the target instead of the rename below replacing the link
// with a regular file. Stops at the first non-link (or nonexistent) path.
// Returns nil if the chain is deeper than the depth limit (caller falls back
// to the in-place write, whose generic_open follows arbitrarily deep chains).
// Caller must already hold a borrowed task context.
- (nullable NSString *)resolvedGuestWriteTargetForPath:(NSString *)guestPath {
    static const int kMaxSymlinkDepth = 8;
    NSString *path = guestPath;
    for (int depth = 0; depth < kMaxSymlinkDepth; depth++) {
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

// Atomic guest-VFS write: everything goes to a temp file in the same
// directory, then a rename over the target, so a mid-write failure (ENOSPC,
// ...) never leaves the file truncated. Falls back to an in-place write with
// stash/restore when the filesystem has no rename or the symlink chain is
// too deep to resolve. Caller must already hold a borrowed task context.
- (BOOL)writeDataViaVFS:(NSData *)data toGuestPath:(NSString *)guestPath error:(NSError **)error {
    NSString *target = [self resolvedGuestWriteTargetForPath:guestPath];
    if (target == nil)
        return [self writeDataInPlaceViaVFS:data toGuestPath:guestPath mode:0644 error:error];

    int mode = 0644;
    struct statbuf st;
    memset(&st, 0, sizeof(st));
    if (generic_statat(AT_PWD, target.fileSystemRepresentation, &st, AT_SYMLINK_NOFOLLOW_) >= 0) {
        if (!S_ISREG(st.mode)) {
            if (error) *error = [self errorWithCode:ISHGuestFileBridgeErrorNotRegularFile message:@"That path is not a regular file"];
            return NO;
        }
        mode = (int)(st.mode & 07777);
    }

    NSString *tmpPath = [target.stringByDeletingLastPathComponent stringByAppendingPathComponent:
                         [NSString stringWithFormat:@".%@.guestfilebridge-tmp", target.lastPathComponent]];
    generic_unlinkat(AT_PWD, tmpPath.fileSystemRepresentation);  // ignore result; usually ENOENT

    struct fd *fd = generic_open(tmpPath.fileSystemRepresentation, O_WRONLY_ | O_CREAT_ | O_TRUNC_, mode);
    if (IS_ERR(fd)) {
        if (error) *error = [self errorWithGuestErrno:(long)PTR_ERR(fd) message:@"Cannot create file"];
        return NO;
    }
    BOOL ok = [self writeAllData:data toOpenFd:fd];
    if (ok && fd->ops->fsync != NULL)
        fd->ops->fsync(fd);
    fd_close(fd);
    if (!ok) {
        generic_unlinkat(AT_PWD, tmpPath.fileSystemRepresentation);
        if (error) *error = [self errorWithCode:ISHGuestFileBridgeErrorUnknown message:@"Failed writing file"];
        return NO;
    }

    int err = generic_renameat(AT_PWD, tmpPath.fileSystemRepresentation, AT_PWD, target.fileSystemRepresentation, 0);
    if (err < 0) {
        // Filesystem without rename (e.g. proc): clean up and fall back.
        generic_unlinkat(AT_PWD, tmpPath.fileSystemRepresentation);
        return [self writeDataInPlaceViaVFS:data toGuestPath:target mode:mode error:error];
    }
    return YES;
}

// Non-atomic fallback for filesystems without rename: stash the old contents
// first, attempt the truncating write, and restore the stash if the write
// fails so the file is never left truncated silently. Caller must already
// hold a borrowed task context.
- (BOOL)writeDataInPlaceViaVFS:(NSData *)data toGuestPath:(NSString *)guestPath mode:(int)mode error:(NSError **)error {
    struct statbuf st;
    memset(&st, 0, sizeof(st));
    if (generic_statat(AT_PWD, guestPath.fileSystemRepresentation, &st, 0) >= 0 && !S_ISREG(st.mode)) {
        if (error) *error = [self errorWithCode:ISHGuestFileBridgeErrorNotRegularFile message:@"That path is not a regular file"];
        return NO;
    }
    NSData *previous = [self readGuestFileViaVFS:guestPath maxBytes:NSUIntegerMax error:NULL];  // nil if new

    struct fd *fd = generic_open(guestPath.fileSystemRepresentation, O_WRONLY_ | O_CREAT_ | O_TRUNC_, mode);
    if (IS_ERR(fd)) {
        if (error) *error = [self errorWithGuestErrno:(long)PTR_ERR(fd) message:@"Cannot create file"];
        return NO;
    }
    BOOL ok = [self writeAllData:data toOpenFd:fd];
    fd_close(fd);
    if (ok)
        return YES;

    BOOL restored = NO;
    if (previous != nil) {
        struct fd *restoreFd = generic_open(guestPath.fileSystemRepresentation, O_WRONLY_ | O_CREAT_ | O_TRUNC_, mode);
        if (!IS_ERR(restoreFd)) {
            restored = [self writeAllData:previous toOpenFd:restoreFd];
            fd_close(restoreFd);
        }
    }
    if (error) {
        NSString *message = restored
            ? @"Failed writing file (the previous contents were restored)"
            : @"Failed writing file, and the previous contents could not be restored";
        *error = [self errorWithCode:ISHGuestFileBridgeErrorUnknown message:message];
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

#pragma mark mkdir / move / copy / remove

- (void)createDirectoryAtGuestPath:(NSString *)guestPath completion:(void (^)(BOOL, NSError * _Nullable))completion {
    NSString *path = [self normalizedGuestPath:guestPath];
    dispatch_async(self.ioQueue, ^{
        NSError *error = nil;
        BOOL ok = NO;
        NSURL *hostURL = [self hostURLForRealfsGuestPath:path];
        if (hostURL != nil) {
            [NSFileManager.defaultManager createDirectoryAtURL:hostURL.URLByDeletingLastPathComponent
                                   withIntermediateDirectories:YES attributes:nil error:NULL];
            ok = (mkdir(hostURL.fileSystemRepresentation, 0755) == 0);
            if (!ok) error = [self hostErrnoError];
        } else {
            __block BOOL vfsOk = NO;
            __block NSError *vfsError = nil;
            BOOL hadContext = [self withGuestTaskContext:^{
                int err = generic_mkdirat(AT_PWD, path.fileSystemRepresentation, 0755);
                if (err < 0) vfsError = [self errorWithGuestErrno:err message:@"Cannot create directory"];
                else vfsOk = YES;
            }];
            if (!hadContext) error = [self notReadyError];
            else { ok = vfsOk; error = vfsError; }
        }
        dispatch_async(dispatch_get_main_queue(), ^{ if (completion) completion(ok, ok ? nil : error); });
    });
}

- (void)moveItemAtGuestPath:(NSString *)sourcePath toGuestPath:(NSString *)destinationPath
                  completion:(void (^)(BOOL, NSError * _Nullable))completion {
    NSString *src = [self normalizedGuestPath:sourcePath];
    NSString *dst = [self normalizedGuestPath:destinationPath];
    dispatch_async(self.ioQueue, ^{
        NSError *error = nil;
        BOOL ok = [self moveSync:src toGuestPath:dst error:&error];
        dispatch_async(dispatch_get_main_queue(), ^{ if (completion) completion(ok, ok ? nil : error); });
    });
}

// Must run on ioQueue.
- (BOOL)moveSync:(NSString *)sourcePath toGuestPath:(NSString *)destinationPath error:(NSError **)error {
    if ([sourcePath isEqualToString:destinationPath])
        return YES;  // moving something onto itself is a no-op, not an error
    NSURL *srcHost = [self hostURLForRealfsGuestPath:sourcePath];
    NSURL *dstHost = [self hostURLForRealfsGuestPath:destinationPath];

    if (srcHost != nil && dstHost != nil) {
        [NSFileManager.defaultManager createDirectoryAtURL:dstHost.URLByDeletingLastPathComponent
                               withIntermediateDirectories:YES attributes:nil error:NULL];
        if (rename(srcHost.fileSystemRepresentation, dstHost.fileSystemRepresentation) == 0)
            return YES;
        if (error) *error = [self hostErrnoError];
        return NO;
    }

    if (srcHost == nil && dstHost == nil) {
        __block BOOL ok = NO;
        __block NSError *vfsError = nil;
        BOOL hadContext = [self withGuestTaskContext:^{
            int err = generic_renameat(AT_PWD, sourcePath.fileSystemRepresentation, AT_PWD, destinationPath.fileSystemRepresentation, 0);
            if (err < 0) vfsError = [self errorWithGuestErrno:err message:@"Cannot move item"];
            else ok = YES;
        }];
        if (!hadContext) { if (error) *error = [self notReadyError]; return NO; }
        if (!ok && error) *error = vfsError;
        return ok;
    }

    // Mixed realfs/fakefs move: no single rename spans two backing stores
    // (matches real EXDEV), so copy the bytes across then remove the source.
    ISHGuestFileItem *sourceInfo = [self statSync:sourcePath error:error];
    if (sourceInfo == nil) return NO;
    if (sourceInfo.kind == ISHGuestFileKindDirectory) {
        if (error) *error = [self errorWithCode:ISHGuestFileBridgeErrorUnsupported message:@"Moving a folder across filesystems isn't supported yet"];
        return NO;
    }
    if (![self copySync:sourcePath toGuestPath:destinationPath error:error]) {
        // The size cap belongs to the copy engine; surface it as what the
        // user actually attempted.
        if (error && [(*error).domain isEqualToString:ISHGuestFileErrorDomain] && (*error).code == ISHGuestFileBridgeErrorTooLarge)
            *error = [self errorWithCode:ISHGuestFileBridgeErrorUnsupported
                                  message:@"Moving files this large across filesystems isn't supported yet"];
        return NO;
    }
    return [self removeSync:sourcePath recursive:NO error:error];
}

- (void)copyItemAtGuestPath:(NSString *)sourcePath toGuestPath:(NSString *)destinationPath
                  completion:(void (^)(BOOL, NSError * _Nullable))completion {
    NSString *src = [self normalizedGuestPath:sourcePath];
    NSString *dst = [self normalizedGuestPath:destinationPath];
    dispatch_async(self.ioQueue, ^{
        NSError *error = nil;
        BOOL ok = [self copySync:src toGuestPath:dst error:&error];
        dispatch_async(dispatch_get_main_queue(), ^{ if (completion) completion(ok, ok ? nil : error); });
    });
}

// Must run on ioQueue. Regular files only in v1 -- directory copy needs a
// recursive tree walk with mkdir mirroring that no caller needs yet.
- (BOOL)copySync:(NSString *)sourcePath toGuestPath:(NSString *)destinationPath error:(NSError **)error {
    if ([sourcePath isEqualToString:destinationPath]) {
        // The host branch below deletes the destination before copying; for
        // the same path that would destroy the only copy, then fail.
        if (error) *error = [self errorWithGuestErrno:_EINVAL message:@"Source and destination are the same file"];
        return NO;
    }
    NSURL *srcHost = [self hostURLForRealfsGuestPath:sourcePath];
    NSURL *dstHost = [self hostURLForRealfsGuestPath:destinationPath];

    if (srcHost != nil && dstHost != nil) {
        [NSFileManager.defaultManager createDirectoryAtURL:dstHost.URLByDeletingLastPathComponent
                               withIntermediateDirectories:YES attributes:nil error:NULL];
        [NSFileManager.defaultManager removeItemAtURL:dstHost error:NULL];  // POSIX cp semantics: overwrite
        return [NSFileManager.defaultManager copyItemAtURL:srcHost toURL:dstHost error:error];
    }

    NSData *data;
    if (srcHost != nil) {
        data = [self readHostRegularFileAtURL:srcHost maxBytes:kMixedBackendCopyMaxBytes error:error];
    } else {
        __block NSData *vfsData = nil;
        __block NSError *vfsError = nil;
        BOOL hadContext = [self withGuestTaskContext:^{
            vfsData = [self readGuestFileViaVFS:sourcePath maxBytes:kMixedBackendCopyMaxBytes error:&vfsError];
        }];
        if (!hadContext) { if (error) *error = [self notReadyError]; return NO; }
        data = vfsData;
        if (data == nil && error) *error = vfsError;
    }
    if (data == nil) return NO;
    return [self writeDataSync:data toGuestPath:destinationPath error:error];
}

- (void)removeItemAtGuestPath:(NSString *)guestPath recursive:(BOOL)recursive
                    completion:(void (^)(BOOL, NSError * _Nullable))completion {
    NSString *path = [self normalizedGuestPath:guestPath];
    dispatch_async(self.ioQueue, ^{
        NSError *error = nil;
        BOOL ok = [self removeSync:path recursive:recursive error:&error];
        dispatch_async(dispatch_get_main_queue(), ^{ if (completion) completion(ok, ok ? nil : error); });
    });
}

// Must run on ioQueue.
- (BOOL)removeSync:(NSString *)guestPath recursive:(BOOL)recursive error:(NSError **)error {
    NSURL *hostURL = [self hostURLForRealfsGuestPath:guestPath];
    if (hostURL != nil) {
        struct stat lst;
        if (lstat(hostURL.fileSystemRepresentation, &lst) != 0) {
            if (error) *error = [self hostErrnoError];
            return NO;
        }
        BOOL ok;
        if (S_ISDIR(lst.st_mode) && recursive) {
            ok = [NSFileManager.defaultManager removeItemAtURL:hostURL error:error];
        } else if (S_ISDIR(lst.st_mode)) {
            ok = (rmdir(hostURL.fileSystemRepresentation) == 0);
            if (!ok && error) *error = [self hostErrnoError];
        } else {
            ok = (unlink(hostURL.fileSystemRepresentation) == 0);
            if (!ok && error) *error = [self hostErrnoError];
        }
        return ok;
    }

    __block BOOL ok = NO;
    __block NSError *vfsError = nil;
    BOOL hadContext = [self withGuestTaskContext:^{
        ok = recursive ? [self removeGuestPathRecursivelyViaVFS:guestPath error:&vfsError]
                       : [self removeGuestPathViaVFS:guestPath error:&vfsError];
    }];
    if (!hadContext) { if (error) *error = [self notReadyError]; return NO; }
    if (!ok && error) *error = vfsError;
    return ok;
}

// Caller must already hold a borrowed task context.
- (BOOL)removeGuestPathViaVFS:(NSString *)guestPath error:(NSError **)error {
    struct statbuf lst;
    memset(&lst, 0, sizeof(lst));
    if (generic_statat(AT_PWD, guestPath.fileSystemRepresentation, &lst, AT_SYMLINK_NOFOLLOW_) < 0) {
        if (error) *error = [self errorWithGuestErrno:_ENOENT message:@"No such file or directory"];
        return NO;
    }
    int err = S_ISDIR((mode_t)lst.mode)
        ? generic_rmdirat(AT_PWD, guestPath.fileSystemRepresentation)
        : generic_unlinkat(AT_PWD, guestPath.fileSystemRepresentation);
    if (err < 0) {
        if (error) *error = [self errorWithGuestErrno:err message:@"Cannot delete item"];
        return NO;
    }
    return YES;
}

// Caller must already hold a borrowed task context. Iterative post-order
// walk with an explicit worklist -- a recursive walk burns one stack frame
// per directory level, and a guest shell can trivially build trees deep
// enough (thousands of levels) to overflow a 512 KB GCD worker stack. Never
// follows a symlinked directory into its target: entries are classified by
// lstat, so a symlink inside the tree is unlinked as itself and deleting a
// folder can never reach outside it.
- (BOOL)removeGuestPathRecursivelyViaVFS:(NSString *)guestPath error:(NSError **)error {
    struct statbuf rootStat;
    memset(&rootStat, 0, sizeof(rootStat));
    if (generic_statat(AT_PWD, guestPath.fileSystemRepresentation, &rootStat, AT_SYMLINK_NOFOLLOW_) < 0) {
        if (error) *error = [self errorWithGuestErrno:_ENOENT message:@"No such file or directory"];
        return NO;
    }
    if (!S_ISDIR((mode_t)rootStat.mode))
        return [self removeGuestPathViaVFS:guestPath error:error];

    // `stack` is the pending work (top = current); a directory is expanded
    // (children pushed above it) exactly once, tracked in `expanded`, and
    // rmdir'd when it surfaces again with its subtree already gone.
    NSMutableArray<NSString *> *stack = [NSMutableArray arrayWithObject:guestPath];
    NSMutableSet<NSString *> *expanded = [NSMutableSet set];
    while (stack.count > 0) {
        NSString *path = stack.lastObject;

        struct statbuf lst;
        memset(&lst, 0, sizeof(lst));
        if (generic_statat(AT_PWD, path.fileSystemRepresentation, &lst, AT_SYMLINK_NOFOLLOW_) < 0) {
            [stack removeLastObject];  // vanished mid-walk (concurrent guest activity): already gone
            [expanded removeObject:path];
            continue;
        }

        BOOL isDirectory = S_ISDIR((mode_t)lst.mode);
        if (!isDirectory || [expanded containsObject:path]) {
            if (![self removeGuestPathViaVFS:path error:error])
                return NO;
            [stack removeLastObject];
            [expanded removeObject:path];
            continue;
        }

        [expanded addObject:path];
        struct fd *fd = generic_open(path.fileSystemRepresentation, O_RDONLY_ | O_NONBLOCK_, 0);
        if (IS_ERR(fd)) {
            if (error) *error = [self errorWithGuestErrno:(long)PTR_ERR(fd) message:@"Cannot open directory"];
            return NO;
        }
        if (fd->ops->readdir_begin) fd->ops->readdir_begin(fd);
        while (true) {
            struct dir_entry entry;
            memset(&entry, 0, sizeof(entry));
            entry.type = DT_UNKNOWN;
            int err = fd->ops->readdir(fd, &entry);
            if (err <= 0) break;
            NSString *name = [NSString stringWithUTF8String:entry.name];
            if (name == nil || [name isEqualToString:@"."] || [name isEqualToString:@".."])
                continue;
            [stack addObject:[path stringByAppendingPathComponent:name]];
        }
        if (fd->ops->readdir_end) fd->ops->readdir_end(fd);
        fd_close(fd);
    }
    return YES;
}

#pragma mark - Terminal integration

// Resolve the foreground process group of one tty to a working directory.
// Caller must already hold a borrowed task context (generic_getpath resolves
// through `current`).
//
// fg_group is a process group id, and a group's leader has pid == pgid, so the
// leader is one pid_get_task_ref away. Using the leader rather than walking
// every task in the group is deliberate: at a shell prompt the foreground
// group IS the shell, and that is when a file browser gets opened.
static NSString *ISHForegroundDirectoryForTTY(int type, int number) {
    struct tty *tty = tty_lookup_ref(type, number, NULL);
    if (tty == NULL)
        return nil;
    lock(&tty->lock, 0);
    pid_t_ fg = tty->fg_group;
    unlock(&tty->lock);
    tty_put(tty);
    if (fg <= 0)
        return nil;

    struct task *task = pid_get_task_ref(fg);
    if (task == NULL)
        return nil;
    struct fs_info *fs = NULL;
    lock(&task->general_lock, 0);
    if (!task->exiting && task->fs != NULL)
        fs = fs_info_retain(task->fs);
    unlock(&task->general_lock);
    task_ref_cnt_mod(task, -1);
    if (fs == NULL)
        return nil;

    complex_lockt(&fs->lock, 0);
    struct fd *pwd = fs->pwd != NULL ? fd_retain(fs->pwd) : NULL;
    unlock(&fs->lock);
    fs_info_release(fs);
    if (pwd == NULL)
        return nil;

    char buf[MAX_PATH + 1];
    memset(buf, 0, sizeof(buf));
    int err = generic_getpath(pwd, buf);
    fd_close(pwd);
    if (err < 0)
        return nil;
    // An unlinked cwd resolves to "" (the root fd itself also yields ""), and
    // both mean "/" to a browser that only ever shows absolute paths.
    if (buf[0] == '\0')
        return @"/";
    return [NSString stringWithUTF8String:buf];
}

- (void)foregroundDirectoryForTTYType:(int)type
                                number:(int)number
                            completion:(void (^)(NSString * _Nullable guestPath))completion {
    dispatch_async(self.ioQueue, ^{
        __block NSString *path = nil;
        [self withGuestTaskContext:^{
            path = ISHForegroundDirectoryForTTY(type, number);
        }];
        dispatch_async(dispatch_get_main_queue(), ^{
            completion(path);
        });
    });
}

@end
