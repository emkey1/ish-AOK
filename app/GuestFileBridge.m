//
//  GuestFileBridge.m
//  iSH-AOK
//

#import "GuestFileBridge.h"
#import "AppGroup.h"

#include <fcntl.h>
#include <mach/mach_time.h>
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

// A read or write at or below this size is treated as an interactive operation;
// anything larger is a transfer and goes on the bulk lane. Sized so every viewer
// that is reading a document to display stays interactive (8 KiB preview, 64 KiB
// /etc/passwd, 4 MiB markdown) and the image viewer's 64 MiB does not.
static const NSUInteger kInteractiveByteBudget = 8 << 20;  // 8 MiB

// The two latency classes. Both queues are serial; see the ORDERING section in
// GuestFileBridge.h and docs/guest_file_bridge_lanes.md.
typedef NS_ENUM(NSInteger, ISHGuestFileLane) {
    ISHGuestFileLaneInteractive,
    ISHGuestFileLaneBulk,
};

// ISH_BRIDGE_LANE_LOG=1 logs one line per operation: lane, how long it waited to
// start, how long it ran, and its path. The whole point of two lanes is a claim
// about WAITING TIME, and eyeballing a spinner cannot tell "the listing ran
// during the copy" from "the listing ran after it" -- this can. Same shape as
// ISH_FAKEFS_LOCKSTATS, read once, off by default, one branch on a plain bool
// when it is off. Sampled with mach_absolute_time so it costs nothing to read.
static bool ISHBridgeLaneLogOn(void) {
    static bool on;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ on = getenv("ISH_BRIDGE_LANE_LOG") != NULL; });
    return on;
}

static double ISHBridgeMillisecondsSince(uint64_t start) {
    static mach_timebase_info_data_t timebase;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ mach_timebase_info(&timebase); });
    uint64_t elapsed = mach_absolute_time() - start;
    return (double)elapsed * timebase.numer / timebase.denom / 1e6;
}

// Does `ancestor` name a directory that `path` lives somewhere under? Both are
// already normalized (absolute, no trailing slash), so this is a prefix test
// plus the component boundary that stops "/ab" from matching "/abc".
static BOOL ISHGuestPathContains(NSString *ancestor, NSString *path) {
    if ([ancestor isEqualToString:@"/"])
        return ![path isEqualToString:@"/"];
    return path.length > ancestor.length
        && [path hasPrefix:ancestor]
        && [path characterAtIndex:ancestor.length] == '/';
}

// One guest path an operation has claimed for ordering.
//
// `subtree` marks an operation whose reach is the whole tree beneath `path` --
// only the recursive delete -- which is what makes work anywhere INSIDE that
// tree conflict with it. Everything else reaches exactly one path.
@interface ISHGuestFileClaim : NSObject
@property (nonatomic, copy) NSString *path;
@property (nonatomic) BOOL subtree;
@end

@implementation ISHGuestFileClaim
@end

static ISHGuestFileClaim *ISHGuestFileClaimAt(NSString *path, BOOL subtree) {
    ISHGuestFileClaim *claim = [ISHGuestFileClaim new];
    claim.path = path;
    claim.subtree = subtree;
    return claim;
}

// Do two claims reach any of the same filesystem state?
//
// The first version of this said "equal, or one is an ancestor of the other",
// which is wrong in the direction that matters: it made a copy into /tmp/foo
// conflict with a listing of "/", and "/" is an ancestor of everything, so ANY
// transfer anywhere would have pushed every listing onto the bulk lane -- which
// is the head-of-line blocking this whole design exists to remove.
//
// What a write at P actually reaches is P itself and the entry list of P's
// parent directory (a stat of that directory sees its mtime move too). Only a
// subtree operation reaches deeper. So:
static BOOL ISHGuestClaimsConflict(ISHGuestFileClaim *a, ISHGuestFileClaim *b) {
    if ([a.path isEqualToString:b.path])
        return YES;
    // A listing of D against a write at D/name, in either order.
    if ([a.path isEqualToString:b.path.stringByDeletingLastPathComponent]) return YES;
    if ([b.path isEqualToString:a.path.stringByDeletingLastPathComponent]) return YES;
    // ...and a recursive delete against anything inside the tree it removes.
    if (a.subtree && ISHGuestPathContains(a.path, b.path)) return YES;
    if (b.subtree && ISHGuestPathContains(b.path, a.path)) return YES;
    return NO;
}

static BOOL ISHGuestClaimsConflictAny(NSArray<ISHGuestFileClaim *> *claims,
                                      NSArray<ISHGuestFileClaim *> *others) {
    for (ISHGuestFileClaim *claim in claims)
        for (ISHGuestFileClaim *other in others)
            if (ISHGuestClaimsConflict(claim, other))
                return YES;
    return NO;
}

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
    // Read on the interactive lane and written on the bulk lane, so it is
    // guarded by @synchronized(self) like the token bookkeeping below.
    NSMutableDictionary<NSString *, ISHGuestFileExtractionCacheEntry *> *_extractionCache;
    // Token bookkeeping, guarded by @synchronized(self): callers cancel from
    // the main thread while a lane polls between chunks or entries.
    NSMutableSet<NSUUID *> *_inflightOperations;
    NSMutableSet<NSUUID *> *_cancelledOperations;

    // The two lanes. Serial, both of them -- see the header.
    dispatch_queue_t _interactiveQueue;
    dispatch_queue_t _bulkQueue;

    // Claims held by work queued or running on each lane, which is how per-path
    // ordering survives there being two of them. Arrays rather than sets: two
    // operations can claim the same path and each must release its own. Guarded
    // by @synchronized(self), which is never held across a wait.
    NSMutableArray<ISHGuestFileClaim *> *_interactiveClaims;
    NSMutableArray<ISHGuestFileClaim *> *_bulkClaims;
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
        // Named .io rather than .interactive so the lane that carries what the
        // user is looking at keeps the label that shows up in a spindump.
        _interactiveQueue = dispatch_queue_create("app.ish.guestfilebridge.io", DISPATCH_QUEUE_SERIAL);
        _bulkQueue = dispatch_queue_create("app.ish.guestfilebridge.bulk", DISPATCH_QUEUE_SERIAL);
        // ISH_BRIDGE_SINGLE_LANE collapses the two lanes back into one, which is
        // exactly the behaviour this design replaced. It exists so the numbers
        // the self-test prints have a control taken on the same hardware in the
        // same run -- "a listing is fast now" means nothing without it.
        if (getenv("ISH_BRIDGE_SINGLE_LANE") != NULL) {
            _bulkQueue = _interactiveQueue;
            NSLog(@"[bridge] ISH_BRIDGE_SINGLE_LANE: both lanes are one serial queue");
        }
        _interactiveClaims = [NSMutableArray array];
        _bulkClaims = [NSMutableArray array];
        _cancelledOperations = [NSMutableSet set];
        _inflightOperations = [NSMutableSet set];
        _extractionCache = [NSMutableDictionary dictionary];
        [self extractionCacheDirectory];  // ensure it exists
    }
    return self;
}

#pragma mark Lane scheduling

// Enqueue `work` on `lane`, preserving per-path ordering across both lanes.
//
// `ordering` is the set of claims this operation must stay ordered against;
// `claimed` is the subset it takes ownership of for the duration (the same list
// for every operation but the read-only preamble of an extraction, which orders
// against its path and hands the claim to the copy that follows).
//
// The invariant that makes this safe with two queues:
//
//   The interactive lane never waits. If an interactive operation would have to
//   wait for anything, it is MOVED to the bulk lane instead. The bulk lane may
//   wait, because a bulk operation is already slow by construction.
//
// so the wait graph has no cycle and this cannot deadlock. Concretely:
//
//   bulk in front of interactive -> the interactive operation is re-routed onto
//     the bulk lane, landing behind the work it has to follow. It is slow, as it
//     must be, but the interactive lane stays free and listings of every OTHER
//     directory are unaffected, which is the entire point of splitting.
//   interactive in front of bulk -> the bulk operation drains the interactive
//     lane first. Bounded by interactive-operation cost, and only on an overlap.
//
// Deciding the barrier at enqueue time leaves no hole: anything conflicting that
// arrives afterward sees our path already claimed and is itself re-routed behind
// us. If the conflicting operation has finished by the time we run, the barrier
// is an empty dispatch_sync and costs nothing.
- (void)enqueueOnLane:(ISHGuestFileLane)lane
              ordering:(NSArray<ISHGuestFileClaim *> *)ordering
               claimed:(NSArray<ISHGuestFileClaim *> *)claimed
                  work:(dispatch_block_t)work {
    ISHGuestFileLane finalLane = lane;
    BOOL needsInteractiveBarrier = NO;

    @synchronized (self) {
        if (finalLane == ISHGuestFileLaneInteractive && ISHGuestClaimsConflictAny(ordering, _bulkClaims))
            finalLane = ISHGuestFileLaneBulk;
        if (finalLane == ISHGuestFileLaneBulk) {
            needsInteractiveBarrier = ISHGuestClaimsConflictAny(ordering, _interactiveClaims);
            [_bulkClaims addObjectsFromArray:claimed];
        } else {
            [_interactiveClaims addObjectsFromArray:claimed];
        }
    }

    BOOL onBulkLane = (finalLane == ISHGuestFileLaneBulk);
    BOOL rerouted = (finalLane != lane);
    dispatch_queue_t queue = onBulkLane ? _bulkQueue : _interactiveQueue;
    uint64_t enqueuedAt = ISHBridgeLaneLogOn() ? mach_absolute_time() : 0;
    dispatch_async(queue, ^{
        // Only when the lanes are genuinely two queues. Under
        // ISH_BRIDGE_SINGLE_LANE they are the same one, and a dispatch_sync onto
        // the queue you are already running on deadlocks -- which it duly did,
        // the first time the control run reached the write-then-copy section.
        // With one queue the ordering the barrier exists to restore is already
        // the queue's own, so skipping it is correct and not merely safe.
        if (needsInteractiveBarrier && queue != self->_interactiveQueue)
            dispatch_sync(self->_interactiveQueue, ^{});
        double waited = enqueuedAt != 0 ? ISHBridgeMillisecondsSince(enqueuedAt) : 0;
        uint64_t startedAt = enqueuedAt != 0 ? mach_absolute_time() : 0;
        work();
        if (enqueuedAt != 0) {
            NSLog(@"[bridge] %@%@ waited %.1fms ran %.1fms %@",
                  onBulkLane ? @"bulk" : @"interactive",
                  rerouted ? @"(rerouted)" : @"",
                  waited, ISHBridgeMillisecondsSince(startedAt),
                  ordering.firstObject.path ?: @"-");
        }
        if (claimed.count == 0)
            return;
        @synchronized (self) {
            NSMutableArray<ISHGuestFileClaim *> *table = onBulkLane ? self->_bulkClaims : self->_interactiveClaims;
            for (ISHGuestFileClaim *claim in claimed)
                [table removeObjectIdenticalTo:claim];  // its own claim object, so identity is exact
        }
    });
}

// The common case: an operation orders against and claims the same paths, none
// of them reaching past the path itself.
- (void)enqueueOnLane:(ISHGuestFileLane)lane
                 paths:(NSArray<NSString *> *)paths
                  work:(dispatch_block_t)work {
    NSMutableArray<ISHGuestFileClaim *> *claims = [NSMutableArray arrayWithCapacity:paths.count];
    for (NSString *path in paths)
        [claims addObject:ISHGuestFileClaimAt(path, NO)];
    [self enqueueOnLane:lane ordering:claims claimed:claims work:work];
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

- (ISHGuestFileOperationToken)listDirectoryAtGuestPath:(NSString *)guestPath
                       completion:(void (^)(NSArray<ISHGuestFileItem *> * _Nullable, NSError * _Nullable))completion {
    NSString *path = [self normalizedGuestPath:guestPath];
    NSUUID *token = [NSUUID UUID];
    [self beginOperation:token];  // before returning it, so an immediate cancel is honored

    [self enqueueOnLane:ISHGuestFileLaneInteractive paths:@[path] work:^{
        NSArray<ISHGuestFileItem *> *items = nil;
        NSError *error = nil;
        if ([self isOperationCancelled:token]) {
            error = [self cancelledError];  // cancelled before it ever started
        } else {
        NSURL *hostDir = [self hostURLForRealfsGuestPath:path];
        BOOL hostIsDir = NO;
        BOOL hostExists = hostDir != nil && [NSFileManager.defaultManager fileExistsAtPath:hostDir.path isDirectory:&hostIsDir];
        if (hostExists && hostIsDir) {
            items = [self listHostDirectory:hostDir guestBasePath:path token:token];
            if (items == nil) error = [self cancelledError];
        } else if (hostExists) {
            error = [self errorWithCode:ISHGuestFileBridgeErrorNotDirectory message:@"That path is not a directory"];
        } else if (hostDir != nil) {
            error = [self errorWithGuestErrno:_ENOENT message:@"No such file or directory"];
        } else {
            __block NSArray<ISHGuestFileItem *> *vfsItems = nil;
            __block NSError *vfsError = nil;
            BOOL hadContext = [self withGuestTaskContext:^{
                vfsItems = [self listGuestDirectoryViaVFS:path token:token error:&vfsError];
            }];
            if (!hadContext) error = [self notReadyError];
            else { items = vfsItems; error = vfsError; }
        }
        }
        [self finishOperation:token];
        NSArray<ISHGuestFileItem *> *result = items;
        NSError *finalError = error ?: (result == nil ? [self errorWithCode:ISHGuestFileBridgeErrorUnknown message:@"Unknown error"] : nil);
        dispatch_async(dispatch_get_main_queue(), ^{ if (completion) completion(result, finalError); });
    }];

    return token;
}

// Returns nil if `token` was cancelled part-way. /AOK/persist is a real host
// directory so this is fast, but it is still O(entries) with an attribute fetch
// each, and a caller who has navigated away should not be paying for it.
- (nullable NSArray<ISHGuestFileItem *> *)listHostDirectory:(NSURL *)hostDir
                                              guestBasePath:(NSString *)guestBasePath
                                                      token:(NSUUID *)token {
    NSArray<NSString *> *names = [NSFileManager.defaultManager contentsOfDirectoryAtPath:hostDir.path error:NULL] ?: @[];
    NSMutableArray<ISHGuestFileItem *> *items = [NSMutableArray arrayWithCapacity:names.count];
    for (NSString *name in names) {
        if ([self isOperationCancelled:token])
            return nil;
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
// handed a FIFO path can never park a lane in a blocking open; the
// S_ISDIR check below rejects it (harmless for real directories).
- (nullable NSArray<ISHGuestFileItem *> *)listGuestDirectoryViaVFS:(NSString *)guestPath
                                                             token:(NSUUID *)token
                                                             error:(NSError **)error {
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

    // Both loops below poll for cancellation. Before this the enumeration always
    // ran to completion and the CALLER discarded the answer (ShellFileBrowser's
    // _loadGeneration guard), so dismissing a sheet over /usr/bin left seconds of
    // VFS work still occupying the lane with nobody waiting for it. The poll is
    // one uncontended @synchronized against a per-entry stat, so it is noise.
    BOOL cancelled = NO;
    NSMutableArray<NSString *> *names = [NSMutableArray array];
    if (fd->ops->readdir_begin) fd->ops->readdir_begin(fd);
    while (true) {
        if ([self isOperationCancelled:token]) { cancelled = YES; break; }
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
    if (cancelled) {
        fd_close(fd);
        if (error) *error = [self cancelledError];
        return nil;
    }

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
        if ([self isOperationCancelled:token]) {
            fd_close(fd);
            if (error) *error = [self cancelledError];
            return nil;
        }
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

// Must run on a lane.
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
    [self enqueueOnLane:ISHGuestFileLaneInteractive paths:@[path] work:^{
        NSError *error = nil;
        ISHGuestFileItem *item = [self statSync:path error:&error];
        dispatch_async(dispatch_get_main_queue(), ^{ if (completion) completion(item, error); });
    }];
}

#pragma mark Filesystem status (statfs)

- (void)filesystemStatusAtGuestPath:(NSString *)guestPath
                          completion:(void (^)(int64_t, int64_t, NSError * _Nullable))completion {
    NSString *path = [self normalizedGuestPath:guestPath];
    [self enqueueOnLane:ISHGuestFileLaneInteractive paths:@[path] work:^{
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
    }];
}

#pragma mark Reading

- (ISHGuestFileOperationToken)readFileAtGuestPath:(NSString *)guestPath maxBytes:(NSUInteger)maxBytes
                  completion:(void (^)(NSData * _Nullable, NSError * _Nullable))completion {
    NSString *path = [self normalizedGuestPath:guestPath];
    NSUUID *token = [NSUUID UUID];
    [self beginOperation:token];  // before returning it, so an immediate cancel is honored

    // The cap is the caller's own declaration of how much work this is; see the
    // header. A 64 MiB image read is a transfer and belongs behind the copies.
    ISHGuestFileLane lane = maxBytes > kInteractiveByteBudget ? ISHGuestFileLaneBulk
                                                              : ISHGuestFileLaneInteractive;
    [self enqueueOnLane:lane paths:@[path] work:^{
        NSError *error = nil;
        NSData *data = [self isOperationCancelled:token] ? nil
                     : [self readDataSync:path maxBytes:maxBytes token:token error:&error];
        if (data == nil && error == nil) error = [self cancelledError];
        [self finishOperation:token];
        dispatch_async(dispatch_get_main_queue(), ^{ if (completion) completion(data, data != nil ? nil : error); });
    }];

    return token;
}

// Must run on a lane.
- (nullable NSData *)readDataSync:(NSString *)guestPath maxBytes:(NSUInteger)maxBytes
                             token:(nullable NSUUID *)token error:(NSError **)error {
    NSURL *hostURL = [self hostURLForRealfsGuestPath:guestPath];
    if (hostURL != nil)
        return [self readHostRegularFileAtURL:hostURL maxBytes:maxBytes token:token error:error];

    __block NSData *data = nil;
    __block NSError *vfsError = nil;
    BOOL hadContext = [self withGuestTaskContext:^{
        data = [self readGuestFileViaVFS:guestPath maxBytes:maxBytes token:token error:&vfsError];
    }];
    if (!hadContext) { if (error) *error = [self notReadyError]; return nil; }
    if (data == nil && error) *error = vfsError;
    return data;
}

// Opens with O_NONBLOCK so a TOCTOU race (a FIFO appearing where a regular
// file was expected) can't block the io queue; the fstat check afterward
// rejects anything that isn't a regular file.
- (nullable NSData *)readHostRegularFileAtURL:(NSURL *)hostURL maxBytes:(NSUInteger)maxBytes
                                        token:(nullable NSUUID *)token error:(NSError **)error {
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
    BOOL cancelled = NO;
    while ((n = read(fd, buffer, sizeof(buffer))) > 0) {
        [data appendBytes:buffer length:(NSUInteger)n];
        if (token != nil && [self isOperationCancelled:token]) { cancelled = YES; break; }
    }
    int readErrno = errno;
    close(fd);
    if (cancelled) {
        if (error) *error = [self cancelledError];
        return nil;
    }
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
- (nullable NSData *)readGuestFileViaVFS:(NSString *)guestPath maxBytes:(NSUInteger)maxBytes
                                    token:(nullable NSUUID *)token error:(NSError **)error {
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
    BOOL tooLarge = NO, cancelled = NO;
    while ((n = fd->ops->read(fd, buffer, sizeof(buffer))) > 0) {
        if (data.length + (NSUInteger)n > maxBytes) { tooLarge = YES; break; }
        [data appendBytes:buffer length:(NSUInteger)n];
        if (token != nil && [self isOperationCancelled:token]) { cancelled = YES; break; }
    }
    ssize_t lastErr = (n < 0) ? n : 0;
    fd_close(fd);
    if (cancelled) {
        if (error) *error = [self cancelledError];
        return nil;
    }
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

// Bulk: deleting a run's worth of extracted temp files is host filesystem work
// proportional to how many there are, and nothing is waiting on it.
- (void)clearExtractionCache {
    [self enqueueOnLane:ISHGuestFileLaneBulk paths:@[] work:^{
        @synchronized (self) { [self->_extractionCache removeAllObjects]; }
        NSURL *dir = [self extractionCacheDirectory];
        [NSFileManager.defaultManager removeItemAtURL:dir error:NULL];
        [self extractionCacheDirectory];  // recreate empty
    }];
}

// The cache is read on the interactive lane (extraction's preamble) and written
// on the bulk lane once the copy lands, so it needs the same monitor the token
// bookkeeping uses. It was documented "accessed only on ioQueue" when there was
// only one queue.
- (nullable NSURL *)cachedExtractionURLForGuestPath:(NSString *)guestPath
                                                 size:(unsigned long long)size
                                     modificationDate:(nullable NSDate *)modificationDate {
    ISHGuestFileExtractionCacheEntry *entry;
    @synchronized (self) { entry = [_extractionCache objectForKey:guestPath]; }
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
    @synchronized (self) { _extractionCache[guestPath] = entry; }
}

- (void)beginOperation:(NSUUID *)token {
    @synchronized (self) {
        [_inflightOperations addObject:token];
    }
}

- (BOOL)isOperationCancelled:(NSUUID *)token {
    @synchronized (self) {
        return [_cancelledOperations containsObject:token];
    }
}

// Ignores tokens that aren't in flight: cancelling after completion (a
// window closing that always cancels defensively, say) must not grow the
// cancelled set with UUIDs nothing will ever remove.
- (void)cancelOperation:(ISHGuestFileOperationToken)token {
    if (token == nil) return;
    @synchronized (self) {
        if ([_inflightOperations containsObject:token])
            [_cancelledOperations addObject:token];
    }
}

- (void)cancelExtraction:(ISHGuestFileExtractionToken)token {
    [self cancelOperation:token];
}

- (void)finishOperation:(NSUUID *)token {
    @synchronized (self) {
        [_inflightOperations removeObject:token];
        [_cancelledOperations removeObject:token];
    }
}

- (NSError *)cancelledError {
    return [self errorWithCode:ISHGuestFileBridgeErrorCancelled message:@"Cancelled"];
}

- (ISHGuestFileExtractionToken)extractToTempFileAtGuestPath:(NSString *)guestPath
    progress:(void (^)(int64_t, int64_t))progress
    completion:(void (^)(NSURL * _Nullable, NSError * _Nullable))completion {
    NSUUID *token = [NSUUID UUID];
    NSString *path = [self normalizedGuestPath:guestPath];
    [self beginOperation:token];  // before returning it, so an immediate cancel is honored

    // Phase 1, interactive lane: everything up to the point where bytes actually
    // have to move. Three of these four outcomes finish here, and two of them --
    // a realfs-backed path and a cache hit -- never touch the VFS at all. Making
    // the whole call bulk would have put "share the image I am already looking
    // at" behind a video copy for no reason.
    //
    // It orders against `path` but claims nothing: it only reads, and the claim
    // belongs to phase 2. Claiming here as well would make phase 2's overlap
    // check fire on phase 1's own claim and drain the interactive lane for
    // nothing.
    [self enqueueOnLane:ISHGuestFileLaneInteractive ordering:@[ISHGuestFileClaimAt(path, NO)] claimed:@[] work:^{
        if ([self isOperationCancelled:token]) {
            [self finishOperation:token];
            dispatch_async(dispatch_get_main_queue(), ^{
                if (completion) completion(nil, [self cancelledError]);
            });
            return;
        }

        NSError *statError = nil;
        ISHGuestFileItem *info = [self statSync:path error:&statError];
        if (info == nil) {
            [self finishOperation:token];
            dispatch_async(dispatch_get_main_queue(), ^{ if (completion) completion(nil, statError); });
            return;
        }
        if (info.kind == ISHGuestFileKindDirectory) {
            [self finishOperation:token];
            dispatch_async(dispatch_get_main_queue(), ^{
                if (completion) completion(nil, [self errorWithCode:ISHGuestFileBridgeErrorIsDirectory message:@"That path is a directory"]);
            });
            return;
        }
        if (info.realfsURL != nil) {
            // Realfs-backed: the host file already exists, no copy needed.
            [self finishOperation:token];
            dispatch_async(dispatch_get_main_queue(), ^{ if (completion) completion(info.realfsURL, nil); });
            return;
        }

        NSURL *cached = [self cachedExtractionURLForGuestPath:path size:info.size modificationDate:info.modificationDate];
        if (cached != nil) {
            [self finishOperation:token];
            dispatch_async(dispatch_get_main_queue(), ^{ if (completion) completion(cached, nil); });
            return;
        }

        // Phase 2, bulk lane: the chunked copy, which is the part that is
        // allowed to take a minute.
        [self enqueueOnLane:ISHGuestFileLaneBulk paths:@[path] work:^{
            __block NSURL *result = nil;
            __block NSError *vfsError = nil;
            int64_t total = (int64_t)info.size;
            BOOL hadContext = [self withGuestTaskContext:^{
                result = [self extractGuestPathViaVFS:path token:token total:total progress:progress error:&vfsError];
            }];
            [self finishOperation:token];
            if (!hadContext) {
                dispatch_async(dispatch_get_main_queue(), ^{ if (completion) completion(nil, [self notReadyError]); });
                return;
            }
            if (result != nil)
                [self cacheExtractedURL:result forGuestPath:path size:info.size modificationDate:info.modificationDate];
            dispatch_async(dispatch_get_main_queue(), ^{ if (completion) completion(result, result != nil ? nil : vfsError); });
        }];
    }];

    return token;
}

// Caller must already hold a borrowed task context. O_NONBLOCK_ is load-
// bearing: a FIFO with no writer blocks generic_open forever otherwise
// (fs/fifo.c), and a wedged open here parks the bulk lane for good -- every
// later transfer would hang with no recovery. The S_ISREG check
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
        if ([self isOperationCancelled:token]) { cancelled = YES; break; }
    }
    if (n < 0 && ok) ok = NO;
    free(buffer);
    fclose(out);
    fd_close(fd);

    if (cancelled) {
        [NSFileManager.defaultManager removeItemAtURL:tempURL error:NULL];
        if (error) *error = [self cancelledError];
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
    // The payload is already in memory, so its length IS the cost of the write.
    ISHGuestFileLane lane = snapshot.length > kInteractiveByteBudget ? ISHGuestFileLaneBulk
                                                                    : ISHGuestFileLaneInteractive;
    [self enqueueOnLane:lane paths:@[path] work:^{
        NSError *error = nil;
        BOOL ok = [self writeDataSync:snapshot toGuestPath:path error:&error];
        dispatch_async(dispatch_get_main_queue(), ^{ if (completion) completion(ok, ok ? nil : error); });
    }];
}

// Must run on a lane.
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
    // No token: this stash is the file's only copy while the truncating write
    // below runs, so cancelling here would lose the data it exists to protect.
    NSData *previous = [self readGuestFileViaVFS:guestPath maxBytes:NSUIntegerMax token:nil error:NULL];  // nil if new

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
    [self enqueueOnLane:ISHGuestFileLaneInteractive paths:@[path] work:^{
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
    }];
}

- (ISHGuestFileOperationToken)moveItemAtGuestPath:(NSString *)sourcePath toGuestPath:(NSString *)destinationPath
                  completion:(void (^)(BOOL, NSError * _Nullable))completion {
    NSString *src = [self normalizedGuestPath:sourcePath];
    NSString *dst = [self normalizedGuestPath:destinationPath];
    NSUUID *token = [NSUUID UUID];
    [self beginOperation:token];

    // Within one backing store this is a single rename; across the realfs/fakefs
    // boundary -moveSync: falls back to copy-then-delete, which is O(file size).
    // -hostURLForRealfsGuestPath: answers which it is from the paths alone, with
    // no VFS call, so the lane is settled before anything is enqueued.
    BOOL crossesStores = ([self hostURLForRealfsGuestPath:src] != nil) != ([self hostURLForRealfsGuestPath:dst] != nil);
    ISHGuestFileLane lane = crossesStores ? ISHGuestFileLaneBulk : ISHGuestFileLaneInteractive;
    [self enqueueOnLane:lane paths:@[src, dst] work:^{
        NSError *error = nil;
        BOOL ok = [self moveSync:src toGuestPath:dst token:token error:&error];
        [self finishOperation:token];
        dispatch_async(dispatch_get_main_queue(), ^{ if (completion) completion(ok, ok ? nil : error); });
    }];

    return token;
}

// Must run on a lane.
- (BOOL)moveSync:(NSString *)sourcePath toGuestPath:(NSString *)destinationPath
            token:(nullable NSUUID *)token error:(NSError **)error {
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
    if (![self copySync:sourcePath toGuestPath:destinationPath token:token error:error]) {
        // The size cap belongs to the copy engine; surface it as what the
        // user actually attempted.
        if (error && [(*error).domain isEqualToString:ISHGuestFileErrorDomain] && (*error).code == ISHGuestFileBridgeErrorTooLarge)
            *error = [self errorWithCode:ISHGuestFileBridgeErrorUnsupported
                                  message:@"Moving files this large across filesystems isn't supported yet"];
        return NO;
    }
    return [self removeSync:sourcePath recursive:NO error:error];
}

- (ISHGuestFileOperationToken)copyItemAtGuestPath:(NSString *)sourcePath toGuestPath:(NSString *)destinationPath
                  completion:(void (^)(BOOL, NSError * _Nullable))completion {
    NSString *src = [self normalizedGuestPath:sourcePath];
    NSString *dst = [self normalizedGuestPath:destinationPath];
    NSUUID *token = [NSUUID UUID];
    [self beginOperation:token];

    [self enqueueOnLane:ISHGuestFileLaneBulk paths:@[src, dst] work:^{
        NSError *error = nil;
        BOOL ok = [self copySync:src toGuestPath:dst token:token error:&error];
        [self finishOperation:token];
        dispatch_async(dispatch_get_main_queue(), ^{ if (completion) completion(ok, ok ? nil : error); });
    }];

    return token;
}

// Must run on a lane. Regular files only in v1 -- directory copy needs a
// recursive tree walk with mkdir mirroring that no caller needs yet.
- (BOOL)copySync:(NSString *)sourcePath toGuestPath:(NSString *)destinationPath
            token:(nullable NSUUID *)token error:(NSError **)error {
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

    // Crossing the realfs/fakefs boundary. This used to read the WHOLE file into
    // one NSData and then write it, capped at 256 MiB -- so copying a large file
    // into /AOK/persist spiked memory by the size of the file (a jetsam risk on
    // a device) and refused outright above the cap. Stream it instead, the same
    // way extractGuestPathViaVFS: already does.
    return [self copyStreamingFrom:sourcePath toGuestPath:destinationPath token:token error:error];
}

// Chunked copy across the realfs/fakefs boundary, atomic at the destination
// (temp file + rename) and bounded in memory by one chunk regardless of file
// size. Exactly one side is realfs here; copySync: handled realfs->realfs above
// with NSFileManager, which already streams.
- (BOOL)copyStreamingFrom:(NSString *)sourcePath toGuestPath:(NSString *)destinationPath
                     token:(nullable NSUUID *)token error:(NSError **)error {
    NSURL *srcHost = [self hostURLForRealfsGuestPath:sourcePath];
    NSURL *dstHost = [self hostURLForRealfsGuestPath:destinationPath];

    __block BOOL ok = NO;
    __block NSError *failure = nil;

    BOOL hadContext = [self withGuestTaskContext:^{
        FILE *hostIn = NULL;
        struct fd *guestIn = NULL;
        FILE *hostOut = NULL;
        struct fd *guestOut = NULL;
        NSString *guestTmpPath = nil;
        NSString *guestDestPath = destinationPath;  // symlink-resolved below; the parameter is captured, so not reassigned
        NSURL *hostTmpURL = nil;
        char *buffer = NULL;

        // ---- source
        if (srcHost != nil) {
            hostIn = fopen(srcHost.fileSystemRepresentation, "rb");
            if (hostIn == NULL) {
                failure = [self errorWithCode:ISHGuestFileBridgeErrorUnknown message:@"Cannot open source file"];
                goto done;
            }
        } else {
            guestIn = generic_open(sourcePath.fileSystemRepresentation, O_RDONLY_ | O_NONBLOCK_, 0);
            if (IS_ERR(guestIn)) {
                failure = [self errorWithGuestErrno:(long)PTR_ERR(guestIn) message:@"Cannot open source file"];
                guestIn = NULL;
                goto done;
            }
            if (S_ISDIR(guestIn->type)) {
                failure = [self errorWithCode:ISHGuestFileBridgeErrorIsDirectory message:@"That path is a directory"];
                goto done;
            }
            if (!S_ISREG(guestIn->type)) {
                failure = [self errorWithCode:ISHGuestFileBridgeErrorNotRegularFile message:@"That path is not a regular file"];
                goto done;
            }
        }

        // ---- destination temp, beside the target so the rename stays on one filesystem
        if (dstHost != nil) {
            [NSFileManager.defaultManager createDirectoryAtURL:dstHost.URLByDeletingLastPathComponent
                                  withIntermediateDirectories:YES attributes:nil error:NULL];
            NSString *tmpName = [NSString stringWithFormat:@".%@.guestfilebridge-tmp",
                                 dstHost.lastPathComponent];
            hostTmpURL = [dstHost.URLByDeletingLastPathComponent URLByAppendingPathComponent:tmpName];
            [NSFileManager.defaultManager removeItemAtURL:hostTmpURL error:NULL];
            hostOut = fopen(hostTmpURL.fileSystemRepresentation, "wb");
            if (hostOut == NULL) {
                failure = [self errorWithCode:ISHGuestFileBridgeErrorUnknown message:@"Cannot create destination file"];
                goto done;
            }
        } else {
            // Follow a symlink destination and inherit an existing file's mode,
            // matching -writeDataViaVFS:.
            NSString *target = [self resolvedGuestWriteTargetForPath:destinationPath] ?: destinationPath;
            int mode = 0644;
            struct statbuf st;
            memset(&st, 0, sizeof(st));
            if (generic_statat(AT_PWD, target.fileSystemRepresentation, &st, AT_SYMLINK_NOFOLLOW_) >= 0) {
                if (!S_ISREG(st.mode)) {
                    failure = [self errorWithCode:ISHGuestFileBridgeErrorNotRegularFile message:@"That path is not a regular file"];
                    goto done;
                }
                mode = (int)(st.mode & 07777);
            }
            guestTmpPath = [target.stringByDeletingLastPathComponent stringByAppendingPathComponent:
                            [NSString stringWithFormat:@".%@.guestfilebridge-tmp", target.lastPathComponent]];
            generic_unlinkat(AT_PWD, guestTmpPath.fileSystemRepresentation);  // usually ENOENT
            guestOut = generic_open(guestTmpPath.fileSystemRepresentation,
                                    O_WRONLY_ | O_CREAT_ | O_TRUNC_, mode);
            if (IS_ERR(guestOut)) {
                failure = [self errorWithGuestErrno:(long)PTR_ERR(guestOut) message:@"Cannot create destination file"];
                guestOut = NULL;
                goto done;
            }
            guestDestPath = target;   // rename onto what we actually sized/moded
        }

        buffer = malloc(kExtractionChunkSize);
        if (buffer == NULL) {
            failure = [self errorWithCode:ISHGuestFileBridgeErrorUnknown message:@"Out of memory"];
            goto done;
        }

        while (true) {
            ssize_t n;
            if (hostIn != NULL) {
                size_t got = fread(buffer, 1, kExtractionChunkSize, hostIn);
                if (got == 0) {
                    if (ferror(hostIn)) {
                        failure = [self errorWithCode:ISHGuestFileBridgeErrorUnknown message:@"Failed reading source file"];
                        goto done;
                    }
                    break;  // clean EOF
                }
                n = (ssize_t)got;
            } else {
                n = guestIn->ops->read(guestIn, buffer, kExtractionChunkSize);
                if (n < 0) {
                    failure = [self errorWithGuestErrno:(long)n message:@"Failed reading source file"];
                    goto done;
                }
                if (n == 0) break;
            }

            if (hostOut != NULL) {
                if (fwrite(buffer, 1, (size_t)n, hostOut) != (size_t)n) {
                    failure = [self errorWithCode:ISHGuestFileBridgeErrorUnknown message:@"Failed writing destination file"];
                    goto done;
                }
            } else {
                // Short writes are legal; keep pushing the remainder.
                ssize_t off = 0;
                while (off < n) {
                    ssize_t w = guestOut->ops->write(guestOut, buffer + off, (size_t)(n - off));
                    if (w <= 0) {
                        failure = [self errorWithGuestErrno:(long)(w < 0 ? w : -_EIO) message:@"Failed writing destination file"];
                        goto done;
                    }
                    off += w;
                }
            }

            // Between chunks, never mid-chunk: the destination is a temp file
            // that `done:` unlinks, so bailing out here leaves nothing behind
            // and the real destination is untouched.
            if (token != nil && [self isOperationCancelled:token]) {
                failure = [self cancelledError];
                goto done;
            }
        }

        // ---- commit
        if (hostOut != NULL) {
            fflush(hostOut);
            fsync(fileno(hostOut));
            fclose(hostOut);
            hostOut = NULL;
            [NSFileManager.defaultManager removeItemAtURL:dstHost error:NULL];
            NSError *moveError = nil;
            if (![NSFileManager.defaultManager moveItemAtURL:hostTmpURL toURL:dstHost error:&moveError]) {
                failure = moveError ?: [self errorWithCode:ISHGuestFileBridgeErrorUnknown message:@"Cannot place destination file"];
                goto done;
            }
            hostTmpURL = nil;
        } else {
            if (guestOut->ops->fsync != NULL)
                guestOut->ops->fsync(guestOut);
            fd_close(guestOut);
            guestOut = NULL;
            int err = generic_renameat(AT_PWD, guestTmpPath.fileSystemRepresentation,
                                       AT_PWD, guestDestPath.fileSystemRepresentation, 0);
            if (err < 0) {
                failure = [self errorWithGuestErrno:err message:@"Cannot place destination file"];
                goto done;
            }
            guestTmpPath = nil;
        }
        ok = YES;

    done:
        free(buffer);
        if (hostIn != NULL) fclose(hostIn);
        if (guestIn != NULL) fd_close(guestIn);
        if (hostOut != NULL) fclose(hostOut);
        if (guestOut != NULL) fd_close(guestOut);
        if (hostTmpURL != nil) [NSFileManager.defaultManager removeItemAtURL:hostTmpURL error:NULL];
        if (guestTmpPath != nil) generic_unlinkat(AT_PWD, guestTmpPath.fileSystemRepresentation);
    }];

    if (!hadContext) {
        if (error) *error = [self notReadyError];
        return NO;
    }
    if (!ok && error) *error = failure ?: [self errorWithCode:ISHGuestFileBridgeErrorUnknown message:@"Copy failed"];
    return ok;
}

- (void)removeItemAtGuestPath:(NSString *)guestPath recursive:(BOOL)recursive
                    completion:(void (^)(BOOL, NSError * _Nullable))completion {
    NSString *path = [self normalizedGuestPath:guestPath];
    // A recursive delete is an unbounded tree walk, so it is bulk even when the
    // folder turns out to be empty -- there is no way to know that without doing
    // the walk. Deliberately NOT cancellable; see the header.
    ISHGuestFileLane lane = recursive ? ISHGuestFileLaneBulk : ISHGuestFileLaneInteractive;
    NSArray<ISHGuestFileClaim *> *claims = @[ISHGuestFileClaimAt(path, recursive)];
    [self enqueueOnLane:lane ordering:claims claimed:claims work:^{
        NSError *error = nil;
        BOOL ok = [self removeSync:path recursive:recursive error:&error];
        dispatch_async(dispatch_get_main_queue(), ^{ if (completion) completion(ok, ok ? nil : error); });
    }];
}

// Must run on a lane.
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
    // No guest path to order against -- this asks the tty where the shell is,
    // it does not touch the filesystem the answer names.
    [self enqueueOnLane:ISHGuestFileLaneInteractive paths:@[] work:^{
        __block NSString *path = nil;
        [self withGuestTaskContext:^{
            path = ISHForegroundDirectoryForTTY(type, number);
        }];
        dispatch_async(dispatch_get_main_queue(), ^{
            completion(path);
        });
    }];
}

@end

#pragma mark - Lane self-test (ISH_BRIDGE_LANE_SELFTEST)

// Verification harness for the two-lane scheduler, off unless the environment
// variable is set. It exists because the two claims this design makes cannot be
// checked by looking at a spinner:
//
//   1. a listing completes promptly WHILE a bulk transfer is running, and
//   2. per-path ordering survives having two lanes -- an intermittent property,
//      so it has to be run many times rather than once.
//
// Everything goes through the public API against the live guest fs, so what it
// tests is the shipped scheduler and not a model of it.

// One async bridge call, made synchronous. Runs on the harness's own queue --
// never main, never a lane -- and waits for the completion the bridge dispatches
// to main, the same way Roots.m's importer does.
static BOOL ISHSelfTestWait(void (^start)(dispatch_semaphore_t done)) {
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    start(done);
    return dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(180 * NSEC_PER_SEC))) == 0;
}

static NSArray<NSString *> *ISHSelfTestListNames(NSString *dir) {
    __block NSArray<NSString *> *names = nil;
    ISHSelfTestWait(^(dispatch_semaphore_t done) {
        [ISHGuestFileBridge.sharedBridge listDirectoryAtGuestPath:dir completion:^(NSArray<ISHGuestFileItem *> *items, NSError *error) {
            names = [items valueForKey:@"name"];
            dispatch_semaphore_signal(done);
        }];
    });
    return names ?: @[];
}

static BOOL ISHSelfTestMkdir(NSString *path) {
    __block BOOL ok = NO;
    ISHSelfTestWait(^(dispatch_semaphore_t done) {
        [ISHGuestFileBridge.sharedBridge createDirectoryAtGuestPath:path completion:^(BOOL succeeded, NSError *error) {
            ok = succeeded;
            dispatch_semaphore_signal(done);
        }];
    });
    return ok;
}

static BOOL ISHSelfTestRemove(NSString *path, BOOL recursive) {
    __block BOOL ok = NO;
    ISHSelfTestWait(^(dispatch_semaphore_t done) {
        [ISHGuestFileBridge.sharedBridge removeItemAtGuestPath:path recursive:recursive completion:^(BOOL succeeded, NSError *error) {
            ok = succeeded;
            dispatch_semaphore_signal(done);
        }];
    });
    return ok;
}

static BOOL ISHSelfTestWrite(NSUInteger length, NSString *path) {
    __block BOOL ok = NO;
    ISHSelfTestWait(^(dispatch_semaphore_t done) {
        [ISHGuestFileBridge.sharedBridge writeData:[NSMutableData dataWithLength:length] toGuestPath:path
                                        completion:^(BOOL succeeded, NSError *error) {
            ok = succeeded;
            dispatch_semaphore_signal(done);
        }];
    });
    return ok;
}

static BOOL ISHSelfTestCopy(NSString *src, NSString *dst) {
    __block BOOL ok = NO;
    ISHSelfTestWait(^(dispatch_semaphore_t done) {
        [ISHGuestFileBridge.sharedBridge copyItemAtGuestPath:src toGuestPath:dst completion:^(BOOL succeeded, NSError *error) {
            ok = succeeded;
            dispatch_semaphore_signal(done);
        }];
    });
    return ok;
}

static double ISHSelfTestNowMs(void) {
    static mach_timebase_info_data_t timebase;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ mach_timebase_info(&timebase); });
    return (double)mach_absolute_time() * timebase.numer / timebase.denom / 1e6;
}

// Keeps the bulk lane busy for as long as `running` stays set, so the ordering
// sections below run against genuinely contended lanes rather than an idle one.
@interface ISHSelfTestChurn : NSObject
@property (atomic) BOOL running;
@property (atomic) NSUInteger copies;
@property (atomic) double totalCopyMs;
@property (nonatomic) dispatch_semaphore_t stopped;
@end

@implementation ISHSelfTestChurn
- (void)startCopying:(NSString *)source to:(NSString *)destination {
    self.running = YES;
    self.stopped = dispatch_semaphore_create(0);
    dispatch_async(dispatch_queue_create("app.ish.guestfilebridge.selftest.churn", DISPATCH_QUEUE_SERIAL), ^{
        while (self.running) {
            double started = ISHSelfTestNowMs();
            if (!ISHSelfTestCopy(source, destination)) break;
            self.totalCopyMs = self.totalCopyMs + (ISHSelfTestNowMs() - started);
            self.copies = self.copies + 1;
        }
        dispatch_semaphore_signal(self.stopped);
    });
}
- (void)stop {
    self.running = NO;
    dispatch_semaphore_wait(self.stopped, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(300 * NSEC_PER_SEC)));
}
- (double)meanCopyMs { return self.copies ? self.totalCopyMs / self.copies : 0; }
@end

// Times `samples` listings of `dir`, reporting the mean and the worst.
static void ISHSelfTestTimeListings(NSString *dir, NSUInteger samples, double *meanMs, double *worstMs) {
    double total = 0, worst = 0;
    for (NSUInteger i = 0; i < samples; i++) {
        double started = ISHSelfTestNowMs();
        ISHSelfTestListNames(dir);
        double elapsed = ISHSelfTestNowMs() - started;
        total += elapsed;
        if (elapsed > worst) worst = elapsed;
    }
    *meanMs = samples ? total / samples : 0;
    *worstMs = worst;
}

static void ISHGuestFileBridgeRunSelfTest(void) {
    ISHGuestFileBridge *bridge = ISHGuestFileBridge.sharedBridge;
    NSString *root = @"/tmp/ish-bridge-selftest";
    NSString *big = [root stringByAppendingPathComponent:@"big.bin"];      // slow enough to observe
    NSString *mid = [root stringByAppendingPathComponent:@"mid.bin"];      // slow enough to race
    NSString *small = [root stringByAppendingPathComponent:@"small.bin"];  // cheap to repeat

    const char *sizeText = getenv("ISH_BRIDGE_LANE_SELFTEST");
    NSUInteger megabytes = (sizeText != NULL && atoi(sizeText) > 1) ? (NSUInteger)atoi(sizeText) : 32;

    NSLog(@"[bridge-selftest] start in %@, payload %lu MiB", root, (unsigned long)megabytes);
    ISHSelfTestRemove(root, YES);  // a previous run's leftovers
    if (!ISHSelfTestMkdir(root)) {
        NSLog(@"[bridge-selftest] FAIL: cannot create %@", root);
        return;
    }
    if (!ISHSelfTestWrite(megabytes << 20, big) || !ISHSelfTestWrite(4 << 20, mid) || !ISHSelfTestWrite(64 << 10, small)) {
        NSLog(@"[bridge-selftest] FAIL: cannot write the payloads");
        ISHSelfTestRemove(root, YES);
        return;
    }

    NSUInteger failures = 0;

    // ---- 1. Head-of-line blocking.
    //
    // Measured as the latency of listing an UNRELATED directory with the bulk
    // lane saturated, against the same measurement with it idle -- and the pass
    // bar is stated against the cost of the bulk work itself, because that is
    // what a listing used to have to wait for.
    //
    // The first version of this counted listings completing during ONE copy,
    // which proved nothing here: a 32 MiB fakefs copy takes about 12ms on a
    // simulator against a warm page cache, so there was no window to observe.
    // Saturating the lane makes the measurement independent of how fast any one
    // copy happens to be.
    //
    // Run it again with ISH_BRIDGE_SINGLE_LANE=1 for the control: that collapses
    // the lanes into the one queue this design replaced, and the loaded numbers
    // should rise to the cost of a copy.
    {
        double idleMean = 0, idleWorst = 0, loadedMean = 0, loadedWorst = 0;
        ISHSelfTestTimeListings(@"/etc", 40, &idleMean, &idleWorst);

        ISHSelfTestChurn *churn = [ISHSelfTestChurn new];
        [churn startCopying:big to:[root stringByAppendingPathComponent:@"big-copy.bin"]];
        ISHSelfTestTimeListings(@"/etc", 40, &loadedMean, &loadedWorst);
        [churn stop];
        ISHSelfTestRemove([root stringByAppendingPathComponent:@"big-copy.bin"], NO);

        NSLog(@"[bridge-selftest] 1. head-of-line: listing /etc -- idle mean %.1fms worst %.1fms; "
               "with the bulk lane saturated (%lu copies of %lu MiB, mean %.0fms each) mean %.1fms worst %.1fms",
              idleMean, idleWorst, (unsigned long)churn.copies, (unsigned long)megabytes,
              churn.meanCopyMs, loadedMean, loadedWorst);

        // A listing serialized behind bulk work waits up to a whole copy; one
        // that is not waits about as long as it does when idle. Comparing
        // against the MEASURED copy cost keeps this honest on any hardware --
        // but only while a copy actually costs more than a listing does. At
        // ISH_BRIDGE_LANE_SELFTEST=8 a copy is about 4ms and an idle listing's
        // worst is 3.6ms, so the bar cannot tell the two apart and firing it
        // would be reporting noise as a regression.
        if (churn.copies == 0) {
            NSLog(@"[bridge-selftest] 1. FAIL: the churn never completed a copy");
            failures++;
        } else if (churn.meanCopyMs < idleWorst * 4) {
            NSLog(@"[bridge-selftest] 1. INCONCLUSIVE: a copy costs %.0fms and an idle listing already costs up to %.1fms, "
                   "so this cannot discriminate -- re-run with a bigger ISH_BRIDGE_LANE_SELFTEST payload",
                  churn.meanCopyMs, idleWorst);
        } else if (loadedWorst >= churn.meanCopyMs) {
            NSLog(@"[bridge-selftest] 1. FAIL: the worst listing under load (%.1fms) cost as much as a copy (%.0fms) -- still serialized behind it",
                  loadedWorst, churn.meanCopyMs);
            failures++;
        }
    }

    // ---- 2. Write-then-reload the way every caller actually does it: the
    // reload issued from inside the mutation's completion. Repeated, with a bulk
    // copy churning underneath so the lanes are genuinely contended.
    {
        NSUInteger iterations = 40, mismatches = 0;
        NSString *churnDst = [root stringByAppendingPathComponent:@"churn.bin"];
        ISHSelfTestChurn *churn = [ISHSelfTestChurn new];
        [churn startCopying:big to:churnDst];

        for (NSUInteger i = 0; i < iterations; i++) {
            NSString *name = [NSString stringWithFormat:@"folder-%lu", (unsigned long)i];
            NSString *path = [root stringByAppendingPathComponent:name];
            NSString *dupName = [NSString stringWithFormat:@"dup-%lu.bin", (unsigned long)i];

            if (!ISHSelfTestMkdir(path)) { mismatches++; continue; }
            if (![ISHSelfTestListNames(root) containsObject:name]) {
                NSLog(@"[bridge-selftest] 2. iteration %lu: new folder %@ missing from the reload", (unsigned long)i, name);
                mismatches++;
            }
            if (ISHSelfTestCopy(small, [root stringByAppendingPathComponent:dupName])
                && ![ISHSelfTestListNames(root) containsObject:dupName]) {
                NSLog(@"[bridge-selftest] 2. iteration %lu: duplicate %@ missing from the reload", (unsigned long)i, dupName);
                mismatches++;
            }
            ISHSelfTestRemove([root stringByAppendingPathComponent:dupName], NO);
            ISHSelfTestRemove(path, YES);
            NSArray<NSString *> *after = ISHSelfTestListNames(root);
            if ([after containsObject:name] || [after containsObject:dupName]) {
                NSLog(@"[bridge-selftest] 2. iteration %lu: deleted entries still present in the reload", (unsigned long)i);
                mismatches++;
            }
        }
        [churn stop];
        ISHSelfTestRemove(churnDst, NO);

        NSLog(@"[bridge-selftest] 2. write-then-reload under load: %lu iterations, %lu mismatches",
              (unsigned long)iterations, (unsigned long)mismatches);
        if (mismatches > 0) failures++;
    }

    // ---- 3. The shape a naive split actually breaks, and the reason the path
    // table exists: a mutation and a listing enqueued BACK TO BACK, without
    // waiting for the mutation's completion. No caller does this today; the
    // point is that one could, and be right.
    {
        NSUInteger iterations = 40, sameLaneMisses = 0, crossLaneMisses = 0;
        for (NSUInteger i = 0; i < iterations; i++) {
            // 3a. Interactive mutation then listing: same lane, so this is the
            // serial queue's own ordering. The control.
            NSString *name = [NSString stringWithFormat:@"race-%lu", (unsigned long)i];
            __block NSArray<NSString *> *seen = nil;
            ISHSelfTestWait(^(dispatch_semaphore_t done) {
                [bridge createDirectoryAtGuestPath:[root stringByAppendingPathComponent:name] completion:^(BOOL ok, NSError *e) {}];
                [bridge listDirectoryAtGuestPath:root completion:^(NSArray<ISHGuestFileItem *> *items, NSError *e) {
                    seen = [items valueForKey:@"name"];
                    dispatch_semaphore_signal(done);
                }];
            });
            if (![seen containsObject:name]) sameLaneMisses++;

            // 3b. BULK mutation then a listing of its parent. The listing is
            // interactive by class, overlaps a claimed bulk path, and so must be
            // re-routed onto the bulk lane behind the copy. This is the case a
            // naive split loses.
            NSString *copyName = [NSString stringWithFormat:@"race-copy-%lu.bin", (unsigned long)i];
            __block NSArray<NSString *> *seenAfterCopy = nil;
            ISHSelfTestWait(^(dispatch_semaphore_t done) {
                [bridge copyItemAtGuestPath:mid toGuestPath:[root stringByAppendingPathComponent:copyName]
                                  completion:^(BOOL ok, NSError *e) {}];
                [bridge listDirectoryAtGuestPath:root completion:^(NSArray<ISHGuestFileItem *> *items, NSError *e) {
                    seenAfterCopy = [items valueForKey:@"name"];
                    dispatch_semaphore_signal(done);
                }];
            });
            if (![seenAfterCopy containsObject:copyName]) {
                NSLog(@"[bridge-selftest] 3b. iteration %lu: listing overtook the bulk copy of %@", (unsigned long)i, copyName);
                crossLaneMisses++;
            }

            ISHSelfTestRemove([root stringByAppendingPathComponent:name], YES);
            ISHSelfTestRemove([root stringByAppendingPathComponent:copyName], NO);
        }
        NSLog(@"[bridge-selftest] 3. unchained mutate-then-list: %lu iterations, %lu same-lane misses, %lu cross-lane misses",
              (unsigned long)iterations, (unsigned long)sameLaneMisses, (unsigned long)crossLaneMisses);
        if (sameLaneMisses > 0 || crossLaneMisses > 0) failures++;
    }

    // ---- 4. The other direction: an interactive write, then a bulk copy of it
    // enqueued without waiting. The copy must see the written bytes, which is
    // what the bulk lane's interactive-drain barrier is for.
    {
        NSUInteger iterations = 30, staleReads = 0;
        NSString *src = [root stringByAppendingPathComponent:@"barrier-src.bin"];
        for (NSUInteger i = 0; i < iterations; i++) {
            // A distinct length per iteration, so a copy of the PREVIOUS content
            // is detectable rather than silently identical.
            NSUInteger length = 4096 + i * 137;
            NSString *dst = [NSString stringWithFormat:@"%@/barrier-dst-%lu.bin", root, (unsigned long)i];
            ISHSelfTestWait(^(dispatch_semaphore_t done) {
                [bridge writeData:[NSMutableData dataWithLength:length] toGuestPath:src completion:^(BOOL ok, NSError *e) {}];
                [bridge copyItemAtGuestPath:src toGuestPath:dst completion:^(BOOL ok, NSError *e) {
                    dispatch_semaphore_signal(done);
                }];
            });

            __block unsigned long long copiedSize = 0;
            ISHSelfTestWait(^(dispatch_semaphore_t done) {
                [bridge statAtGuestPath:dst completion:^(ISHGuestFileItem *item, NSError *e) {
                    copiedSize = item.size;
                    dispatch_semaphore_signal(done);
                }];
            });
            if (copiedSize != length) {
                NSLog(@"[bridge-selftest] 4. iteration %lu: the copy saw %llu bytes, the write put %lu",
                      (unsigned long)i, copiedSize, (unsigned long)length);
                staleReads++;
            }
            ISHSelfTestRemove(dst, NO);
        }
        ISHSelfTestRemove(src, NO);
        NSLog(@"[bridge-selftest] 4. unchained write-then-copy: %lu iterations, %lu stale reads",
              (unsigned long)iterations, (unsigned long)staleReads);
        if (staleReads > 0) failures++;
    }

    // ---- 5. Cancelling a listing must stop the WORK, not merely discard the
    // answer. Cancelled MID-WALK, not at the head, so it is the readdir and
    // per-entry-stat loops being tested and not just the early-out at the top.
    //
    // Against a directory this test builds itself, because the rootfs offers no
    // guarantee that /usr/bin is big enough for the full walk to cost anything.
    {
        NSString *crowd = [root stringByAppendingPathComponent:@"crowd"];
        ISHSelfTestMkdir(crowd);
        for (NSUInteger i = 0; i < 900; i++)
            ISHSelfTestWrite(64, [crowd stringByAppendingPathComponent:[NSString stringWithFormat:@"entry-%03lu", (unsigned long)i]]);

        NSArray<NSString *> *probe = ISHSelfTestListNames(crowd);
        double fullStarted = ISHSelfTestNowMs();
        ISHSelfTestListNames(crowd);
        double fullMs = ISHSelfTestNowMs() - fullStarted;

        double cancelStarted = ISHSelfTestNowMs();
        __block double cancelledMs = 0;
        ISHSelfTestWait(^(dispatch_semaphore_t done) {
            ISHGuestFileOperationToken token = [bridge listDirectoryAtGuestPath:crowd
                                                                     completion:^(NSArray<ISHGuestFileItem *> *items, NSError *e) {
                cancelledMs = ISHSelfTestNowMs() - cancelStarted;
                dispatch_semaphore_signal(done);
            }];
            // Let it get properly underway first: cancelling immediately would
            // be caught by the check at the top of the block and prove nothing
            // about the loops.
            [NSThread sleepForTimeInterval:fullMs / 4000.0];
            [bridge cancelOperation:token];
        });
        ISHSelfTestRemove(crowd, YES);

        NSLog(@"[bridge-selftest] 5. cancellation: %lu entries; full listing %.1fms, cancelled mid-walk %.1fms",
              (unsigned long)probe.count, fullMs, cancelledMs);
        // Only meaningful once the full walk costs something; on a trivial
        // directory both numbers are noise.
        if (probe.count > 500 && fullMs > 50 && cancelledMs > fullMs * 0.75) {
            NSLog(@"[bridge-selftest] 5. FAIL: the cancelled listing ran about as long as the full one");
            failures++;
        }
    }

    ISHSelfTestRemove(root, YES);
    NSLog(@"[bridge-selftest] %@ (%lu failing section%@)",
          failures == 0 ? @"PASS" : @"FAIL", (unsigned long)failures, failures == 1 ? @"" : @"s");
}

void ISHGuestFileBridgeRunSelfTestIfRequested(void) {
    if (getenv("ISH_BRIDGE_LANE_SELFTEST") == NULL)
        return;
    // Its own queue: it blocks on semaphores the bridge signals from MAIN, so
    // running it on main -- or on either lane -- would deadlock immediately.
    dispatch_async(dispatch_queue_create("app.ish.guestfilebridge.selftest", DISPATCH_QUEUE_SERIAL), ^{
        for (int attempt = 0; attempt < 120 && !ISHGuestFileBridge.sharedBridge.isGuestAvailable; attempt++)
            [NSThread sleepForTimeInterval:0.5];
        if (!ISHGuestFileBridge.sharedBridge.isGuestAvailable) {
            NSLog(@"[bridge-selftest] skipped: the guest never became available");
            return;
        }
        ISHGuestFileBridgeRunSelfTest();
    });
}
