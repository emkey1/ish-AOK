//
//  AudioLibrary.m
//  iSH-AOK
//

#import "AudioLibrary.h"
#import "AppGroup.h"

// Forward-declared rather than importing AudioPCMDecoder.h, which would drag
// AVFoundation into this translation unit alongside the kernel C headers below.
FOUNDATION_EXPORT NSSet<NSString *> *ISHAudioSupportedExtensions(void);

#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/task.h"
#include "fs/fd.h"
#include "fs/path.h"
#include "util/sync.h"
#include "debug.h"

#ifndef DT_UNKNOWN
#define DT_UNKNOWN 0
#endif
#ifndef DT_DIR
#define DT_DIR 4
#endif

static NSString *const kPersistGuestPrefix = @"/AOK/persist/";
static const int kMaxScanDepth = 8;

@implementation ISHAudioLibrary {
    NSMutableDictionary<NSString *, NSURL *> *_tempURLCache;  // guestPath -> extracted temp file
}

+ (instancetype)sharedLibrary {
    static ISHAudioLibrary *shared;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ shared = [[ISHAudioLibrary alloc] init]; });
    return shared;
}

- (instancetype)init {
    self = [super init];
    if (self) {
        _tempURLCache = [NSMutableDictionary dictionary];
    }
    return self;
}

#pragma mark Host paths

// Host URL for the /AOK/persist mount (the AppGroup container's AOK/persist).
- (NSURL *)persistHostBaseURL {
    NSURL *container = ContainerURL();
    if (container == nil) return nil;
    return [[container URLByAppendingPathComponent:@"AOK" isDirectory:YES]
            URLByAppendingPathComponent:@"persist" isDirectory:YES];
}

- (NSString *)musicGuestPath { return @"/AOK/persist/music"; }

- (NSURL *)musicDirectoryURL {
    NSURL *base = [self persistHostBaseURL];
    if (base == nil) return nil;
    NSURL *music = [base URLByAppendingPathComponent:@"music" isDirectory:YES];
    [NSFileManager.defaultManager createDirectoryAtURL:music
                           withIntermediateDirectories:YES attributes:nil error:NULL];
    return music;
}

- (NSURL *)playlistsDirectoryURL {
    NSURL *base = [self persistHostBaseURL];
    if (base == nil) return nil;
    NSURL *dir = [base URLByAppendingPathComponent:@"playlists" isDirectory:YES];
    [NSFileManager.defaultManager createDirectoryAtURL:dir
                           withIntermediateDirectories:YES attributes:nil error:NULL];
    return dir;
}

#pragma mark Scanning

+ (BOOL)isSupportedAudioFileName:(NSString *)name {
    NSString *ext = name.pathExtension.lowercaseString;
    return ext.length > 0 && [ISHAudioSupportedExtensions() containsObject:ext];
}

- (NSArray<ISHAudioTrack *> *)scanDefaultMusicDirectory {
    [self musicDirectoryURL];  // ensure it exists
    return [self scanGuestDirectoryAtPath:self.musicGuestPath];
}

- (NSArray<ISHAudioTrack *> *)scanGuestDirectoryAtPath:(NSString *)guestPath {
    // /AOK/persist is realfs (a real host directory), so enumerate it with
    // NSFileManager — this works from any thread. The guest VFS, by contrast,
    // resolves paths through the thread-local `current` task, which is unset on
    // our GCD worker threads (see -withGuestTaskContext:).
    NSURL *hostDir = [self hostURLForPersistGuestPath:guestPath];
    if (hostDir != nil)
        return [self scanHostDirectory:hostDir guestBasePath:guestPath];

    // Anywhere else lives in the per-root fakefs; walk it via the VFS under a
    // borrowed task context.
    NSMutableArray<ISHAudioTrack *> *tracks = [NSMutableArray array];
    [self withGuestTaskContext:^{
        [self scanGuestDirectory:guestPath depth:0 into:tracks];
    }];
    [tracks sortUsingComparator:^NSComparisonResult(ISHAudioTrack *a, ISHAudioTrack *b) {
        return [a.guestPath caseInsensitiveCompare:b.guestPath];
    }];
    return tracks;
}

// Host URL for a path under the realfs /AOK/persist mount, or nil if not under it.
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

// Enumerate a real host directory (recursively) for supported audio files,
// pinning each track's host URL so playback skips path resolution entirely.
- (NSArray<ISHAudioTrack *> *)scanHostDirectory:(NSURL *)hostDir guestBasePath:(NSString *)guestBasePath {
    NSMutableArray<ISHAudioTrack *> *tracks = [NSMutableArray array];
    NSDirectoryEnumerator<NSURL *> *enumerator =
        [NSFileManager.defaultManager enumeratorAtURL:hostDir
                           includingPropertiesForKeys:@[NSURLIsRegularFileKey]
                                              options:NSDirectoryEnumerationSkipsHiddenFiles
                                         errorHandler:nil];
    NSString *base = hostDir.URLByStandardizingPath.path;
    for (NSURL *fileURL in enumerator) {
        if (![ISHAudioLibrary isSupportedAudioFileName:fileURL.lastPathComponent])
            continue;
        NSNumber *isRegular = nil;
        [fileURL getResourceValue:&isRegular forKey:NSURLIsRegularFileKey error:NULL];
        if (isRegular != nil && !isRegular.boolValue)
            continue;
        NSString *full = fileURL.URLByStandardizingPath.path;
        NSString *rel = [full hasPrefix:base] ? [full substringFromIndex:base.length] : fileURL.lastPathComponent;
        while ([rel hasPrefix:@"/"]) rel = [rel substringFromIndex:1];
        ISHAudioTrack *track = [[ISHAudioTrack alloc] initWithGuestPath:[guestBasePath stringByAppendingPathComponent:rel]];
        track.resolvedFileURL = fileURL;  // real host file — no VFS resolution needed at play time
        [tracks addObject:track];
    }
    [tracks sortUsingComparator:^NSComparisonResult(ISHAudioTrack *a, ISHAudioTrack *b) {
        return [a.guestPath caseInsensitiveCompare:b.guestPath];
    }];
    return tracks;
}

// Run a guest-VFS operation under a borrowed task context. fs/path.c resolves
// paths through the thread-local `current` task, which is unset on GCD worker
// threads, so we borrow pid 1 exactly as the app's boot code does
// (PushInitTaskAsCurrent in AppDelegate) and restore afterward. Returns NO if
// init isn't usable yet (e.g. before the guest has booted).
- (BOOL)withGuestTaskContext:(void (^)(void))block {
    struct task *previous = current;
    struct task *init = pid_get_task_global_ref(1);
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

// Recursive VFS walk — works for fakefs roots as well as the realfs /AOK/persist.
- (void)scanGuestDirectory:(NSString *)dirPath depth:(int)depth into:(NSMutableArray<ISHAudioTrack *> *)tracks {
    if (depth > kMaxScanDepth) return;
    struct fd *fd = generic_open(dirPath.fileSystemRepresentation, O_RDONLY_, 0);
    if (IS_ERR(fd)) return;
    if (!S_ISDIR(fd->type) || fd->ops->readdir == NULL) { fd_close(fd); return; }

    if (fd->ops->readdir_begin) fd->ops->readdir_begin(fd);
    NSMutableArray<NSString *> *subdirs = [NSMutableArray array];
    while (true) {
        struct dir_entry entry;
        memset(&entry, 0, sizeof(entry));
        entry.type = DT_UNKNOWN;
        int err = fd->ops->readdir(fd, &entry);
        if (err <= 0) break;  // 0 = end, <0 = error
        NSString *name = [NSString stringWithUTF8String:entry.name];
        if (name == nil || [name isEqualToString:@"."] || [name isEqualToString:@".."])
            continue;
        NSString *childPath = [dirPath stringByAppendingPathComponent:name];
        if ([ISHAudioLibrary isSupportedAudioFileName:name])
            [tracks addObject:[[ISHAudioTrack alloc] initWithGuestPath:childPath]];
        else if (entry.type == DT_DIR || entry.type == DT_UNKNOWN)
            [subdirs addObject:childPath];  // DT_UNKNOWN no-ops below if not a dir
    }
    if (fd->ops->readdir_end) fd->ops->readdir_end(fd);
    fd_close(fd);

    for (NSString *sub in subdirs)
        [self scanGuestDirectory:sub depth:depth + 1 into:tracks];
}

#pragma mark Resolving guest paths to playable host URLs

- (NSURL *)resolvePlayableURLForGuestPath:(NSString *)guestPath error:(NSError **)error {
    // Fast path: /AOK/persist is realfs-backed, so the host file IS the backing
    // file — no copy, and reachable from any thread.
    NSURL *hostURL = [self hostURLForPersistGuestPath:guestPath];
    if (hostURL != nil && [NSFileManager.defaultManager fileExistsAtPath:hostURL.path])
        return hostURL;

    @synchronized (_tempURLCache) {
        NSURL *cached = _tempURLCache[guestPath];
        if (cached != nil && [NSFileManager.defaultManager fileExistsAtPath:cached.path])
            return cached;
    }

    // Fakefs-resident file: read it out through the VFS under a borrowed task context.
    __block NSURL *extracted = nil;
    __block NSError *extractError = nil;
    BOOL hadContext = [self withGuestTaskContext:^{
        extracted = [self extractGuestFileToTemp:guestPath error:&extractError];
    }];
    if (!hadContext) {
        if (error) *error = [self errorWithCode:0 message:@"Guest filesystem not ready"];
        return nil;
    }
    if (extracted != nil) {
        @synchronized (_tempURLCache) { _tempURLCache[guestPath] = extracted; }
    } else if (error) {
        *error = extractError;
    }
    return extracted;
}

// Read a fakefs-resident file out through the VFS into a temp file the decoder
// can open. The temp keeps the original extension so the factory dispatches it.
- (NSURL *)extractGuestFileToTemp:(NSString *)guestPath error:(NSError **)error {
    struct fd *fd = generic_open(guestPath.fileSystemRepresentation, O_RDONLY_, 0);
    if (IS_ERR(fd)) {
        if (error) *error = [self errorWithCode:(int)PTR_ERR(fd) message:@"Cannot open guest file"];
        return nil;
    }

    NSString *ext = guestPath.pathExtension;
    NSString *tempName = [[NSUUID UUID].UUIDString stringByAppendingPathExtension:ext.length ? ext : @"audio"];
    NSURL *tempURL = [NSURL fileURLWithPath:[NSTemporaryDirectory() stringByAppendingPathComponent:tempName]];

    FILE *out = fopen(tempURL.fileSystemRepresentation, "wb");
    if (out == NULL) {
        fd_close(fd);
        if (error) *error = [self errorWithCode:0 message:@"Cannot create temp file"];
        return nil;
    }

    char buffer[65536];
    ssize_t n;
    BOOL ok = YES;
    while ((n = generic_read(fd, buffer, sizeof(buffer))) > 0) {
        if (fwrite(buffer, 1, (size_t)n, out) != (size_t)n) { ok = NO; break; }
    }
    if (n < 0) ok = NO;
    fclose(out);
    fd_close(fd);

    if (!ok) {
        [NSFileManager.defaultManager removeItemAtURL:tempURL error:NULL];
        if (error) *error = [self errorWithCode:0 message:@"Failed reading guest file"];
        return nil;
    }
    return tempURL;
}

- (NSError *)errorWithCode:(int)code message:(NSString *)message {
    return [NSError errorWithDomain:@"ISHAudioLibrary" code:code
                           userInfo:@{NSLocalizedDescriptionKey: message}];
}

#pragma mark Playlists

- (NSString *)sanitizedPlaylistFileName:(NSString *)name {
    NSCharacterSet *allowed = [NSCharacterSet characterSetWithCharactersInString:
        @"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -_"];
    NSMutableString *safe = [NSMutableString string];
    for (NSUInteger i = 0; i < name.length; i++) {
        unichar c = [name characterAtIndex:i];
        [safe appendString:[allowed characterIsMember:c] ? [NSString stringWithCharacters:&c length:1] : @"_"];
    }
    NSString *trimmed = [safe stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
    return trimmed.length ? trimmed : @"Playlist";
}

- (NSArray<NSString *> *)playlistNames {
    NSURL *dir = [self playlistsDirectoryURL];
    if (dir == nil) return @[];
    NSArray<NSURL *> *files = [NSFileManager.defaultManager
        contentsOfDirectoryAtURL:dir includingPropertiesForKeys:nil
                         options:NSDirectoryEnumerationSkipsHiddenFiles error:NULL];
    NSMutableArray<NSString *> *names = [NSMutableArray array];
    for (NSURL *file in files) {
        if ([file.pathExtension.lowercaseString isEqualToString:@"json"])
            [names addObject:[file.lastPathComponent stringByDeletingPathExtension]];
    }
    [names sortUsingSelector:@selector(caseInsensitiveCompare:)];
    return names;
}

- (NSURL *)urlForPlaylistNamed:(NSString *)name {
    NSURL *dir = [self playlistsDirectoryURL];
    if (dir == nil) return nil;
    NSString *file = [[self sanitizedPlaylistFileName:name] stringByAppendingPathExtension:@"json"];
    return [dir URLByAppendingPathComponent:file];
}

- (NSArray<ISHAudioTrack *> *)tracksForPlaylistNamed:(NSString *)name {
    NSURL *url = [self urlForPlaylistNamed:name];
    if (url == nil) return @[];
    NSData *data = [NSData dataWithContentsOfURL:url];
    if (data == nil) return @[];
    id json = [NSJSONSerialization JSONObjectWithData:data options:0 error:NULL];
    if (![json isKindOfClass:NSArray.class]) return @[];
    NSMutableArray<ISHAudioTrack *> *tracks = [NSMutableArray array];
    for (id entry in json) {
        if (![entry isKindOfClass:NSDictionary.class]) continue;
        ISHAudioTrack *track = [ISHAudioTrack trackWithDictionary:entry];
        if (track) [tracks addObject:track];
    }
    return tracks;
}

- (void)savePlaylistNamed:(NSString *)name tracks:(NSArray<ISHAudioTrack *> *)tracks {
    NSURL *url = [self urlForPlaylistNamed:name];
    if (url == nil) return;
    NSMutableArray *array = [NSMutableArray array];
    for (ISHAudioTrack *track in tracks)
        [array addObject:track.dictionaryRepresentation];
    NSData *data = [NSJSONSerialization dataWithJSONObject:array
                                                  options:NSJSONWritingPrettyPrinted error:NULL];
    [data writeToURL:url atomically:YES];
}

- (void)deletePlaylistNamed:(NSString *)name {
    NSURL *url = [self urlForPlaylistNamed:name];
    if (url != nil) [NSFileManager.defaultManager removeItemAtURL:url error:NULL];
}

@end
