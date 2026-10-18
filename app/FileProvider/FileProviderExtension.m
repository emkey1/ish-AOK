//
//  FileProviderExtension.m
//  iSHFiles
//
//  Created by Theodore Dubois on 9/20/18.
//

#import "FileProviderExtension.h"
#import "FileProviderItem.h"
#import "FileProviderEnumerator.h"
#import "NSError+ISHErrno.h"
#import "ISHFileProviderDomainCleanup.h"
#import "../AppGroup.h"
#include "fs/fake-db.h"
#include "fs/fake-path.h"
#import <os/log.h>
#include <unistd.h>

// The extension's breadcrumbs go to a JSON file in the app group container and
// every write there is passed error:nil, so a failure is completely silent --
// and in practice no breadcrumbs file has been observed on device at all. The
// mount open/close pair is the part worth being able to see from the outside
// (it is what decides whether a database is still open at suspension, which is
// what RunningBoard kills with 0xdead10cc), and both are rare enough to log
// unconditionally. Visible in Console.app with subsystem app.ish.iSH-AOK,
// category fileprovider.
os_log_t ISHFileProviderLog(void) {
    static os_log_t log;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        log = os_log_create("app.ish.iSH-AOK", "fileprovider");
    });
    return log;
}

NSString *ISHHostPathForGuestPath(NSString *path) {
    const char *guest = path.fileSystemRepresentation;
    size_t size = strlen(guest) * 3 + 2;
    char *buf = malloc(size);
    if (buf == NULL)
        return path;
    fake_path_to_host(guest, buf, size); // can't overflow: size is the worst case
    NSString *result = [NSFileManager.defaultManager stringWithFileSystemRepresentation:buf length:strlen(buf)];
    free(buf);
    return result;
}

NSString *ISHGuestPathForHostPath(NSString *path) {
    char *buf = strdup(path.fileSystemRepresentation);
    if (buf == NULL)
        return path;
    fake_path_from_host(buf);
    NSString *result = [NSFileManager.defaultManager stringWithFileSystemRepresentation:buf length:strlen(buf)];
    free(buf);
    return result;
}

static NSNumber *ISHFileProviderDurationMilliseconds(NSTimeInterval start) {
    return @((NSInteger) ((NSDate.date.timeIntervalSinceReferenceDate - start) * 1000.0));
}

static NSString *const kISHFileProviderDiagnosticsDirectory = @"Diagnostics";
static NSString *const kISHFileProviderDiagnosticsBreadcrumbsFile = @"breadcrumbs.json";

static dispatch_queue_t ISHFileProviderBreadcrumbQueue(void) {
    static dispatch_queue_t queue;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        queue = dispatch_queue_create("app.ish.iSH-AOK.FileProvider.Diagnostics", DISPATCH_QUEUE_SERIAL);
    });
    return queue;
}

static NSURL *ISHFileProviderDiagnosticsDirectoryURL(void) {
    NSURL *baseURL = ContainerURL();
    if (baseURL == nil) {
        baseURL = [NSFileManager.defaultManager URLsForDirectory:NSApplicationSupportDirectory
                                                      inDomains:NSUserDomainMask].firstObject;
    }
    if (baseURL == nil)
        return nil;
    return [baseURL URLByAppendingPathComponent:kISHFileProviderDiagnosticsDirectory isDirectory:YES];
}

static NSURL *ISHFileProviderBreadcrumbsURL(void) {
    NSURL *directoryURL = ISHFileProviderDiagnosticsDirectoryURL();
    if (directoryURL == nil)
        return nil;
    return [directoryURL URLByAppendingPathComponent:kISHFileProviderDiagnosticsBreadcrumbsFile isDirectory:NO];
}

static NSString *ISHFileProviderISO8601StringFromDate(NSDate *date) {
    if (date == nil)
        return nil;
    static NSISO8601DateFormatter *formatter;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        formatter = [NSISO8601DateFormatter new];
        formatter.formatOptions = NSISO8601DateFormatWithInternetDateTime;
    });
    return [formatter stringFromDate:date];
}

void ISHFileProviderRecordBreadcrumb(NSString *event, NSDictionary<NSString *, id> *details) {
    if (event.length == 0)
        return;
    dispatch_async(ISHFileProviderBreadcrumbQueue(), ^{
        NSURL *directoryURL = ISHFileProviderDiagnosticsDirectoryURL();
        NSURL *breadcrumbsURL = ISHFileProviderBreadcrumbsURL();
        if (directoryURL == nil || breadcrumbsURL == nil)
            return;

        [NSFileManager.defaultManager createDirectoryAtURL:directoryURL
                               withIntermediateDirectories:YES
                                                attributes:nil
                                                     error:nil];
        NSData *existingData = [NSData dataWithContentsOfURL:breadcrumbsURL];
        NSMutableArray<NSDictionary<NSString *, id> *> *breadcrumbs = [NSMutableArray array];
        if (existingData.length > 0) {
            id existingObject = [NSJSONSerialization JSONObjectWithData:existingData options:NSJSONReadingMutableContainers error:nil];
            if ([existingObject isKindOfClass:[NSArray class]])
                [breadcrumbs addObjectsFromArray:existingObject];
        }

        NSMutableDictionary<NSString *, id> *entry = [NSMutableDictionary dictionary];
        entry[@"timestamp"] = ISHFileProviderISO8601StringFromDate([NSDate date]) ?: @"";
        entry[@"event"] = event;
        if (details.count != 0)
            entry[@"details"] = details;
        [breadcrumbs addObject:entry];

        const NSUInteger maxBreadcrumbs = 200;
        if (breadcrumbs.count > maxBreadcrumbs) {
            NSRange overflow = NSMakeRange(0, breadcrumbs.count - maxBreadcrumbs);
            [breadcrumbs removeObjectsInRange:overflow];
        }

        NSData *jsonData = [NSJSONSerialization dataWithJSONObject:breadcrumbs options:NSJSONWritingPrettyPrinted error:nil];
        if (jsonData != nil)
            [jsonData writeToURL:breadcrumbsURL options:NSDataWritingAtomic error:nil];
    });
}

static NSMutableDictionary<NSString *, id> *ISHFileProviderDetails(NSString * _Nullable domainIdentifier) {
    NSMutableDictionary<NSString *, id> *details = [NSMutableDictionary dictionary];
    if (domainIdentifier.length != 0)
        details[@"domain"] = domainIdentifier;
    return details;
}

static NSString *ISHFileProviderCleanupDefaultsKey(NSString * _Nullable domainIdentifier) {
    NSString *suffix = domainIdentifier.length != 0 ? domainIdentifier : @"default";
    return [NSString stringWithFormat:@"ISHFileProviderLastCleanup.%@", suffix];
}

static int ISHFileProviderAcquireRootLock(NSString *domainIdentifier, BOOL exclusive) {
    NSError *error = nil;
    int fd = ISHAppGroupAcquireNamedLock(@"root", domainIdentifier, exclusive, &error);
    if (fd < 0) {
        NSMutableDictionary<NSString *, id> *details = ISHFileProviderDetails(domainIdentifier);
        details[@"mode"] = exclusive ? @"exclusive" : @"shared";
        details[@"error"] = error.localizedDescription ?: @"unknown";
        ISHFileProviderRecordBreadcrumb(@"fileprovider.rootlock.failed", details);
    }
    return fd;
}

// Lists the valid installed roots in the shared app-group container. Mirrors the
// app-side RootURLLooksValid check (a "data" directory plus a "meta.db").
static NSArray<NSString *> *ISHFileProviderInstalledRootNames(void) {
    NSURL *rootsDir = [ContainerURL() URLByAppendingPathComponent:@"roots" isDirectory:YES];
    if (rootsDir == nil)
        return @[];
    NSArray<NSURL *> *entries = [NSFileManager.defaultManager contentsOfDirectoryAtURL:rootsDir
                                                            includingPropertiesForKeys:nil
                                                                               options:0
                                                                                 error:nil];
    NSMutableArray<NSString *> *names = [NSMutableArray array];
    for (NSURL *entry in entries) {
        NSURL *data = [entry URLByAppendingPathComponent:@"data" isDirectory:YES];
        NSURL *meta = [entry URLByAppendingPathComponent:@"meta.db" isDirectory:NO];
        BOOL isDir = NO;
        if ([NSFileManager.defaultManager fileExistsAtPath:data.path isDirectory:&isDir] && isDir &&
            [NSFileManager.defaultManager fileExistsAtPath:meta.path])
            [names addObject:entry.lastPathComponent];
    }
    [names sortUsingSelector:@selector(localizedStandardCompare:)];
    return names;
}

NSString *const ISHFileProviderPersistRootName = @".persist";

// /AOK/persist itself (see AOKPersistDirectoryURL in AppDelegate.m / PersistRootsDir
// in Roots.m) -- a real, writable, host-backed directory shared by every booted
// root, mounted by the guest kernel as a plain realfs mount (no fakefs metadata
// db of its own). Unlike an installed root, it has no "data" subdirectory: this
// directory itself is the mount's real content.
static NSURL *ISHFileProviderPersistDirectoryURL(void) {
    NSURL *container = ContainerURL();
    if (container == nil)
        return nil;
    return [[container URLByAppendingPathComponent:@"AOK" isDirectory:YES]
            URLByAppendingPathComponent:@"persist" isDirectory:YES];
}

// The fakefs metadata db this extension keeps for browsing /AOK/persist through
// Files. Deliberately a sibling of the persist directory (inside "AOK/", which
// Files never lists) rather than a child of it, so it never shows up as a stray
// entry in the Persist folder's own listing.
static NSURL *ISHFileProviderPersistMetadataURL(void) {
    NSURL *container = ContainerURL();
    if (container == nil)
        return nil;
    return [[container URLByAppendingPathComponent:@"AOK" isDirectory:YES]
            URLByAppendingPathComponent:@".persist-fileprovider-meta.db" isDirectory:NO];
}

@interface ISHFileProviderMount ()

@property (nonatomic) struct fakefs_mount storage;
@property (nonatomic, readwrite) NSString *rootName;
@property (nonatomic, readwrite) NSURL *rootURL;
@property (nonatomic, readwrite) NSRecursiveLock *ioLock;

@end

@implementation ISHFileProviderMount

- (nullable instancetype)initWithRootName:(NSString *)rootName error:(NSError **)error {
    self = [super init];
    if (self == nil)
        return nil;

    BOOL isPersist = [rootName isEqualToString:ISHFileProviderPersistRootName];

    _storage.root_fd = -1;
    NSURL *metaURL;
    if (isPersist) {
        _rootURL = ISHFileProviderPersistDirectoryURL();
        metaURL = ISHFileProviderPersistMetadataURL();
        if (_rootURL == nil || metaURL == nil) {
            if (error != nil)
                *error = [NSError errorWithDomain:NSPOSIXErrorDomain code:ENOENT userInfo:nil];
            return nil;
        }
        // The extension may run before the main app has ever booted (and thus
        // ever created/mounted /AOK/persist) -- e.g. the very first time a
        // fresh install is browsed from Files before iSH-AOK is launched.
        [NSFileManager.defaultManager createDirectoryAtURL:_rootURL
                                withIntermediateDirectories:YES
                                                 attributes:nil
                                                      error:nil];
    } else {
        NSURL *container = ContainerURL();
        NSURL *fs_dir = [[container URLByAppendingPathComponent:@"roots"]
                         URLByAppendingPathComponent:rootName];
        _rootURL = [fs_dir URLByAppendingPathComponent:@"data"];
        metaURL = [fs_dir URLByAppendingPathComponent:@"meta.db"];
    }
    _rootName = [rootName copy];
    _ioLock = [[NSRecursiveLock alloc] init];
    _storage.source = strdup(_rootURL.fileSystemRepresentation);
    _storage.root_fd = open(_storage.source, O_RDONLY | O_DIRECTORY);
    if (_storage.root_fd < 0) {
        if (error != nil)
            *error = [NSError errorWithDomain:NSPOSIXErrorDomain code:errno userInfo:nil];
        free((void *) _storage.source);
        _storage.source = NULL;
        return nil;
    }

    if (isPersist && ![NSFileManager.defaultManager fileExistsAtPath:metaURL.path]) {
        int createErr = fake_db_create_schema(metaURL.fileSystemRepresentation);
        if (createErr < 0) {
            close(_storage.root_fd);
            _storage.root_fd = -1;
            free((void *) _storage.source);
            _storage.source = NULL;
            if (error != nil)
                *error = [NSError errorWithISHErrno:createErr itemIdentifier:NSFileProviderRootContainerItemIdentifier];
            return nil;
        }
    }

    int err = fake_db_init(&_storage.db, metaURL.fileSystemRepresentation, _storage.root_fd);
    if (err < 0) {
        NSLog(@"error opening root: %d", err);
        close(_storage.root_fd);
        _storage.root_fd = -1;
        free((void *) _storage.source);
        _storage.source = NULL;
        if (error != nil)
            *error = [NSError errorWithISHErrno:err itemIdentifier:NSFileProviderRootContainerItemIdentifier];
        return nil;
    }

    if (isPersist) {
        // A brand-new persist db has no root ("") row yet -- every real
        // installed root gets one from fakefs_import/fakefs_init_empty, but
        // this db skipped that step (fake_db_create_schema only lays down
        // the tables). Mode 01777 matches fakefs_init_empty's shared-root
        // convention: /AOK/persist is writable by every guest uid.
        db_begin_write(&_storage.db);
        if (!path_read_stat(&_storage.db, "", NULL, NULL)) {
            struct ish_stat rootStat = {.mode = S_IFDIR | 01777};
            path_create(&_storage.db, "", &rootStat);
        }
        db_commit(&_storage.db);
    }

    return self;
}

- (struct fakefs_mount *)mount {
    return &_storage;
}

- (void)dealloc {
    if (_storage.source != NULL) {
        free((void *) _storage.source);
        _storage.source = NULL;
    }
    if (_storage.root_fd >= 0) {
        close(_storage.root_fd);
        _storage.root_fd = -1;
    }
    if (_storage.db.db != NULL)
        fake_db_deinit(&_storage.db);
}

@end

@interface FileProviderExtension () {
}
// One open fakefs mount per installed root, opened lazily. Replaces the old
// single-mount-per-domain model now that one domain hosts every root.
@property (nonatomic) NSMutableDictionary<NSString *, ISHFileProviderMount *> *mountsByRoot;
// Bumped on every mount access; the idle-close block only fires if its own
// generation is still current, so a later access silently cancels an earlier
// pending close without needing a cancellable timer.
@property (nonatomic) uint64_t mountIdleGeneration;
@end

// How long the mount cache may sit unused before its databases are closed.
// This exists to move the close OFF the suspension path. NSFileProviderExtension
// has no invalidate hook and iOS does not tell us when we are about to be
// suspended, so previously the only fake_db_deinit was -[ISHFileProviderMount
// dealloc] at process teardown. sqlite3_close() checkpoints the entire WAL back
// into the database on the last connection, and doing that while being suspended
// (still holding the database lock) is what RunningBoard kills with 0xdead10cc:
// every one of build 546's crash reports had a thread inside sqlite3WalClose.
// Closing while we are still running and untimed makes that checkpoint free of
// any deadline.
static const NSTimeInterval kISHFileProviderMountIdleSeconds = 3.0;

#pragma mark - Retirement on a Mac

// On a Mac this extension can never serve a request (see
// ISHFileProviderRunningOnMac), and removing domains from the app is not enough:
// the app does that when it launches (Roots.m), but fileproviderd launches the
// extension on its own schedule, seconds after a wake, dark wakes included.
// Build 554 has the app-side removal and still aborted this way on two Macs.
//
// So on a Mac the extension retires before NSExtensionMain runs. It removes this
// provider's registered domains, notes that it did, and exits cleanly. The
// request that would abort is never received, so it does not matter how or when
// a given macOS checks the principal class. This is a C constructor, not +load
// or +initialize: by now dyld has initialised every framework we link and no
// objc runtime lock is held. The framework has not started a request either, so
// the removal cannot wait on anything this process holds for one. It can still
// wait on fileproviderd, though, which launched us to serve a domain and may be
// waiting on this launch while we ask it to remove that domain. Nothing here
// rules that out; the 3 s bound on the removal is what covers it. The attempt is
// then abandoned, and the failure record says whether getDomains had answered by
// the deadline. If it had not, this launch requests no removal at all, even if
// the answer arrives in the moment before we exit: the deadline and the answer
// are taken under one lock, and whichever comes second knows about the first.
// If it had, the removals were requested, and fileproviderd may still finish
// them after we exit.
//
// A removal that comes back complete and error-free is final: a marker in this
// extension's defaults says so, and later launches -- for instance for the
// default domain fileproviderd keeps per provider and will not remove -- exit
// without asking fileproviderd anything. A Mac where it never comes back clean
// (a timeout every time, a removal fileproviderd refuses) must not pay up to 4 s
// and a breadcrumb on every launch, hour after hour, indefinitely. So failures
// are counted in the same defaults: after three, launches exit at once until the
// last attempt is a day old, then try once more, and a new build starts the count
// over. The breadcrumb, which shares a 200-entry file with the app's own,
// is written only when an attempt's outcome differs from the previous one's;
// os_log gets every launch.
static NSString *const kISHFileProviderRetiredOnMacKey = @"ISHFileProviderRetiredOnMac";
static NSString *const kISHFileProviderRetireFailuresKey = @"ISHFileProviderRetireOnMacFailures";
static const int64_t kISHFileProviderRetireRemovalTimeoutMs = 3000;
static const int64_t kISHFileProviderRetireRecordTimeoutMs = 1000;
static const NSInteger kISHFileProviderRetireFailuresBeforeBackoff = 3;
static const NSTimeInterval kISHFileProviderRetireBackoffSeconds = 24 * 60 * 60;

__attribute__((constructor))
static void ISHFileProviderRetireOnMac(void) {
    @autoreleasepool {
        if (!ISHFileProviderRunningOnMac())
            return;

        NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
        if ([defaults objectForKey:kISHFileProviderRetiredOnMacKey] != nil) {
            os_log(ISHFileProviderLog(), "Mac retirement: retired on an earlier launch, exiting");
            _exit(0);
        }

        // Earlier failures, for this build only. Types are checked: an exception
        // thrown here would abort the process, which is what this is avoiding.
        NSString *build = NSBundle.mainBundle.infoDictionary[@"CFBundleVersion"] ?: @"unknown";
        NSDictionary *failures = [defaults objectForKey:kISHFileProviderRetireFailuresKey];
        if (![failures isKindOfClass:NSDictionary.class] || ![failures[@"build"] isEqual:build])
            failures = nil;
        NSNumber *failedCount = failures[@"count"];
        NSInteger failed = [failedCount isKindOfClass:NSNumber.class] ? failedCount.integerValue : 0;
        NSDate *lastFailure = failures[@"last"];
        if (failed >= kISHFileProviderRetireFailuresBeforeBackoff && [lastFailure isKindOfClass:NSDate.class]) {
            NSTimeInterval since = -lastFailure.timeIntervalSinceNow;
            // A negative age means the clock went back; try rather than wait.
            if (since >= 0 && since < kISHFileProviderRetireBackoffSeconds) {
                os_log(ISHFileProviderLog(), "Mac retirement: %ld attempts have failed, the last %.1f h ago; next attempt when that is 24 h old, exiting",
                       (long) failed, since / 3600.0);
                _exit(0);
            }
        }
        NSInteger attempt = failed + 1;

        os_log(ISHFileProviderLog(), "Mac retirement: this extension cannot run on a Mac; removing registered domains, then exiting");
        NSTimeInterval start = NSDate.date.timeIntervalSinceReferenceDate;
        dispatch_semaphore_t answered = dispatch_semaphore_create(0);
        __block NSDictionary<NSString *, id> *summary = nil;
        // What getDomains returned, filled in when it answers, and whether this
        // launch has stopped waiting. The answer can come after the deadline, so
        // both are only touched under the listing's lock. An answer that finds
        // the deadline already passed requests no removal, so a record that says
        // listed = NO always means this launch asked for none.
        NSMutableDictionary<NSString *, id> *listing = [NSMutableDictionary dictionary];
        __block BOOL gaveUp = NO;
        ISHFileProviderRemoveRegisteredDomains(^BOOL(NSDictionary<NSString *, id> *list) {
            BOOL proceed;
            @synchronized (listing) {
                proceed = !gaveUp;
                if (proceed)
                    [listing addEntriesFromDictionary:list];
            }
            if (!proceed)
                os_log(ISHFileProviderLog(), "Mac retirement: getDomains answered after the deadline; no removal requested");
            return proceed;
        }, ^(NSDictionary<NSString *, id> *result) {
            summary = result;
            dispatch_semaphore_signal(answered);
        });
        BOOL timedOut = dispatch_semaphore_wait(answered, dispatch_time(DISPATCH_TIME_NOW, kISHFileProviderRetireRemovalTimeoutMs * (int64_t) NSEC_PER_MSEC)) != 0;

        // What this attempt found, without its timing, so that it can be
        // compared with the previous attempt's. After a timeout `summary` may
        // still be written by the completion, so it is not read at all.
        NSMutableDictionary<NSString *, id> *outcome = [NSMutableDictionary dictionary];
        if (timedOut) {
            outcome[@"timedOut"] = @YES;
            // What it gave up waiting for. Not listed: getDomains had not
            // answered, and with gaveUp set it cannot start a removal later.
            // Listed: the removals (if domains > 0) were requested, and not all
            // of them had answered.
            @synchronized (listing) {
                gaveUp = YES;
                outcome[@"listed"] = listing.count != 0 ? @YES : @NO;
                [outcome addEntriesFromDictionary:listing];
            }
        } else {
            [outcome addEntriesFromDictionary:summary];
        }
        // Only a complete answer with nothing left registered is final.
        BOOL retired = !timedOut && summary[@"listError"] == nil && summary[@"errors"] == nil;
        outcome[@"retired"] = @(retired);
        // A recorded outcome is never a retired one, so a retirement always
        // counts as a change.
        BOOL outcomeChanged = ![failures[@"outcome"] isEqual:outcome];

        NSMutableDictionary<NSString *, id> *details = [outcome mutableCopy];
        details[@"attempt"] = @(attempt);
        details[@"duration_ms"] = ISHFileProviderDurationMilliseconds(start);
        // Logged before anything is recorded: a launch whose log has no details
        // line was stopped before it recorded anything, and does not count
        // towards the backoff.
        os_log(ISHFileProviderLog(), "Mac retirement: %{public}@", details);

        if (retired) {
            [defaults setObject:@{
                @"date": NSDate.date,
                @"build": build,
                @"domains": summary[@"domains"] ?: @0,
            } forKey:kISHFileProviderRetiredOnMacKey];
            [defaults removeObjectForKey:kISHFileProviderRetireFailuresKey];
        } else {
            [defaults setObject:@{
                @"build": build,
                @"count": @(attempt),
                @"last": NSDate.date,
                @"outcome": outcome.copy,
            } forKey:kISHFileProviderRetireFailuresKey];
        }
        // The write reaches cfprefsd asynchronously, and _exit won't wait.
        dispatch_group_t recorded = dispatch_group_create();
        dispatch_group_async(recorded, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
            [defaults synchronize];
        });
        if (outcomeChanged) {
            // The breadcrumb also lands in the app's Diagnostics export. It is
            // written on its own queue (group container lookup, file I/O,
            // nothing that needs the main thread), so wait behind it, briefly.
            ISHFileProviderRecordBreadcrumb(@"fileprovider.extension.retiredOnMac", details);
            dispatch_group_async(recorded, ISHFileProviderBreadcrumbQueue(), ^{});
        }
        dispatch_group_wait(recorded, dispatch_time(DISPATCH_TIME_NOW, kISHFileProviderRetireRecordTimeoutMs * (int64_t) NSEC_PER_MSEC));
    }
    // _exit, not exit: no atexit handlers or static destructors while XPC and
    // dispatch threads are still live.
    _exit(0);
}

@implementation FileProviderExtension

- (nullable ISHFileProviderMount *)mountForRootName:(NSString *)rootName error:(NSError **)error {
    if (rootName.length == 0) {
        if (error != nil)
            *error = [NSError errorWithDomain:NSFileProviderErrorDomain code:NSFileProviderErrorNoSuchItem userInfo:nil];
        return nil;
    }
    @synchronized (self) {
        if (self.mountsByRoot == nil)
            self.mountsByRoot = [NSMutableDictionary dictionary];
        [self scheduleMountIdleCloseLocked];
        ISHFileProviderMount *mountOwner = self.mountsByRoot[rootName];
        if (mountOwner != nil)
            return mountOwner;

        ISHFileProviderRecordBreadcrumb(@"fileprovider.mount.create.begin", ISHFileProviderDetails(rootName));
        NSTimeInterval start = NSDate.date.timeIntervalSinceReferenceDate;
        mountOwner = [[ISHFileProviderMount alloc] initWithRootName:rootName error:error];
        if (mountOwner == nil) {
            NSMutableDictionary<NSString *, id> *details = ISHFileProviderDetails(rootName);
            details[@"duration_ms"] = ISHFileProviderDurationMilliseconds(start);
            if (error != nil && *error != nil)
                details[@"error"] = (*error).localizedDescription ?: @"unknown";
            ISHFileProviderRecordBreadcrumb(@"fileprovider.mount.create.failed", details);
            return nil;
        }
        self.mountsByRoot[rootName] = mountOwner;
        NSMutableDictionary<NSString *, id> *details = ISHFileProviderDetails(rootName);
        details[@"duration_ms"] = ISHFileProviderDurationMilliseconds(start);
        ISHFileProviderRecordBreadcrumb(@"fileprovider.mount.create.end", details);
        os_log(ISHFileProviderLog(), "mount opened for root %{public}@ (%{public}lu open)",
               rootName, (unsigned long) self.mountsByRoot.count);
        return mountOwner;
    }
}

// Caller must hold @synchronized (self).
- (void)scheduleMountIdleCloseLocked {
    uint64_t generation = ++_mountIdleGeneration;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t) (kISHFileProviderMountIdleSeconds * NSEC_PER_SEC)),
                   dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        [self closeIdleMountsForGeneration:generation];
    });
}

- (void)closeIdleMountsForGeneration:(uint64_t)generation {
    NSArray<ISHFileProviderMount *> *closing = nil;
    @synchronized (self) {
        // A later access bumped the generation, so that access owns the next
        // close and this one is stale.
        if (generation != self.mountIdleGeneration)
            return;
        if (self.mountsByRoot.count == 0)
            return;
        closing = self.mountsByRoot.allValues;
        [self.mountsByRoot removeAllObjects];
    }

    // Dropping the cache's reference is not necessarily the last one: an
    // in-flight operation holds its ISHFileProviderMount as a strong local, and
    // a live FileProviderItem retains its mountOwner. Those keep the database
    // open until they are done with it, which is what makes closing here safe.
    // Whichever reference goes last runs -dealloc -> fake_db_deinit, and that
    // must not happen under our lock, so `closing` is released after leaving it.
    NSMutableDictionary<NSString *, id> *details = ISHFileProviderDetails(nil);
    details[@"mounts"] = @(closing.count);
    ISHFileProviderRecordBreadcrumb(@"fileprovider.mount.idle_close", details);
    os_log(ISHFileProviderLog(), "idle close: dropping %{public}lu mount(s) after %{public}.1fs idle",
           (unsigned long) closing.count, kISHFileProviderMountIdleSeconds);
    closing = nil;
    os_log(ISHFileProviderLog(), "idle close: done, databases released");
}

- (NSArray<NSString *> *)installedRootNames {
    return ISHFileProviderInstalledRootNames();
}

// Periodically removes materialized files whose backing inode no longer exists.
// Runs at most hourly; triggered when the domain root is enumerated.
- (void)maybeCleanupStorage {
    NSString *cleanupKey = ISHFileProviderCleanupDefaultsKey(@"all");
    NSDate *lastCleanup = [NSUserDefaults.standardUserDefaults objectForKey:cleanupKey] ?: NSDate.distantPast;
    if ([NSDate.date timeIntervalSinceDate:lastCleanup] <= 60 * 60 /* 1 hour */)
        return;
    [self cleanupStorage];
    [NSUserDefaults.standardUserDefaults setObject:NSDate.date forKey:cleanupKey];
}

- (NSURL *)storageURL {
    NSURL *storage = NSFileProviderManager.defaultManager.documentStorageURL;
    if (self.domain != nil)
        storage = [storage URLByAppendingPathComponent:self.domain.pathRelativeToDocumentStorage isDirectory:YES];
    return storage;
}

- (nullable NSFileProviderItem)itemForIdentifier:(NSFileProviderItemIdentifier)identifier error:(NSError * _Nullable *)error {
    NSTimeInterval start = NSDate.date.timeIntervalSinceReferenceDate;
    NSString *rootName = ISHFileProviderRootNameForIdentifier(identifier);
    if (rootName == nil) {
        // The domain root container: a synthetic folder listing the roots.
        return [[FileProviderItem alloc] initWithIdentifier:NSFileProviderRootContainerItemIdentifier mountOwner:nil error:error];
    }
    ISHFileProviderMount *mountOwner = [self mountForRootName:rootName error:error];
    if (mountOwner == nil) {
        NSMutableDictionary<NSString *, id> *details = ISHFileProviderDetails(rootName);
        details[@"identifier"] = identifier ?: @"";
        details[@"duration_ms"] = ISHFileProviderDurationMilliseconds(start);
        if (error != nil && *error != nil)
            details[@"error"] = (*error).localizedDescription ?: @"unknown";
        ISHFileProviderRecordBreadcrumb(@"fileprovider.item.lookup.failed", details);
        return nil;
    }
    NSFileProviderItemIdentifier inner = ISHFileProviderInnerIdentifier(identifier);
    int rootLockFd = ISHFileProviderAcquireRootLock(rootName, NO);
    @try {
        NSLog(@"item for id %@", identifier);
        NSError *err;
        FileProviderItem *item = [[FileProviderItem alloc] initWithIdentifier:inner mountOwner:mountOwner error:&err];
        if (item == nil) {
            if (error != nil)
                *error = err;
            NSMutableDictionary<NSString *, id> *details = ISHFileProviderDetails(rootName);
            details[@"identifier"] = identifier ?: @"";
            details[@"duration_ms"] = ISHFileProviderDurationMilliseconds(start);
            details[@"error"] = err.localizedDescription ?: @"unknown";
            ISHFileProviderRecordBreadcrumb(@"fileprovider.item.lookup.failed", details);
            return nil;
        }
        NSNumber *duration = ISHFileProviderDurationMilliseconds(start);
        if (duration.integerValue >= 200) {
            NSMutableDictionary<NSString *, id> *details = ISHFileProviderDetails(rootName);
            details[@"identifier"] = identifier ?: @"";
            details[@"duration_ms"] = duration;
            ISHFileProviderRecordBreadcrumb(@"fileprovider.item.lookup.slow", details);
        }
        return item;
    } @finally {
        ISHAppGroupReleaseLock(rootLockFd);
    }
}

- (nullable NSURL *)URLForItemWithPersistentIdentifier:(NSFileProviderItemIdentifier)identifier {
    if ([identifier isEqualToString:NSFileProviderRootContainerItemIdentifier])
        return self.storageURL;
    FileProviderItem *item = [self itemForIdentifier:identifier error:nil];
    if (item == nil)
        return nil;
    NSURL *url = [self.storageURL URLByAppendingPathComponent:identifier isDirectory:YES];
    url = [url URLByAppendingPathComponent:item.path.lastPathComponent isDirectory:NO];
    NSLog(@"url for id %@ = %@", identifier, url);
    return url;
}

- (nullable NSFileProviderItemIdentifier)persistentIdentifierForItemAtURL:(NSURL *)url {
    if ([url.URLByDeletingLastPathComponent isEqual:NSFileProviderManager.defaultManager.documentStorageURL]) {
        return NSFileProviderRootContainerItemIdentifier;
    }
    NSString *identifier = url.pathComponents[url.pathComponents.count - 2];
    // Identifiers are now "<root>" or "<root>+<inner>" (a single path component),
    // so the old "must be a decimal inode or virt_" check no longer applies.
    if (identifier.length == 0)
        return nil;
    NSLog(@"id for url %@ = %@", url, identifier);
    return identifier;
}

- (BOOL)enhanceSanityOfURL:(NSURL *)url error:(NSError **)error {
    NSURL *dir = url.URLByDeletingLastPathComponent;
    NSFileManager *manager = NSFileManager.defaultManager;
    BOOL isDir;
    if ([manager fileExistsAtPath:dir.path isDirectory:&isDir] && !isDir)
        [manager removeItemAtURL:dir error:nil];
    return [manager createDirectoryAtURL:dir
             withIntermediateDirectories:YES
                              attributes:nil
                                   error:error];
}

- (void)providePlaceholderAtURL:(NSURL *)url completionHandler:(void (^)(NSError * _Nullable error))completionHandler {
    NSError *err;
    FileProviderItem *item = [self itemForIdentifier:[self persistentIdentifierForItemAtURL:url] error:&err];
    if (item == nil) {
        completionHandler(err);
        return;
    }
    if (![self enhanceSanityOfURL:url error:&err]) {
        completionHandler(err);
        return;
    }
    if (![NSFileProviderManager writePlaceholderAtURL:[NSFileProviderManager placeholderURLForURL:url]
                                         withMetadata:item
                                                error:&err]) {
        completionHandler(err);
        return;
    }
    completionHandler(nil);
}

- (void)startProvidingItemAtURL:(NSURL *)url completionHandler:(void (^)(NSError *))completionHandler {
    // Should ensure that the actual file is in the position returned by URLForItemWithIdentifier:, then call the completion handler
    NSError *err;
    FileProviderItem *item = [self itemForIdentifier:[self persistentIdentifierForItemAtURL:url] error:&err];
    if (item == nil) {
        completionHandler(err);
        return;
    }
    if (![self enhanceSanityOfURL:url error:&err]) {
        completionHandler(err);
        return;
    }
    [item loadToURL:url];
    completionHandler(nil);
}

- (void)itemChangedAtURL:(NSURL *)url {
    FileProviderItem *item = [self itemForIdentifier:[self persistentIdentifierForItemAtURL:url] error:nil];
    if (item == nil)
        return;
    [item saveFromURL:url];
}

#pragma mark - Action helpers

// FIXME: not dry enough
// These helpers operate on a specific root's mount, resolved by the caller from
// the parent/item identifier.
- (BOOL)doCreateDirectoryAt:(NSString *)path mount:(struct fakefs_mount *)mount inode:(ino_t *)inode error:(NSError **)error {
    NSURL *url = [[NSURL fileURLWithPath:[NSString stringWithUTF8String:mount->source]] URLByAppendingPathComponent:ISHHostPathForGuestPath(path)];
    if (![NSFileManager.defaultManager createDirectoryAtURL:url
                                withIntermediateDirectories:NO
                                                 attributes:@{NSFilePosixPermissions: @0777}
                                                      error:error]) {
        return NO;
    }
    db_begin_write(&mount->db);
    struct ish_stat stat;
    NSString *parentPath = [path substringToIndex:[path rangeOfString:@"/" options:NSBackwardsSearch].location];
    if (!path_read_stat(&mount->db, parentPath.fileSystemRepresentation, &stat, NULL)) {
        db_rollback(&mount->db);
        [NSFileManager.defaultManager removeItemAtURL:url error:nil];
        if (error != nil)
            *error = [NSError errorWithDomain:NSFileProviderErrorDomain code:NSFileProviderErrorNoSuchItem userInfo:nil];
        return NO;
    }
    stat.mode = (stat.mode & ~S_IFMT) | S_IFDIR;
    path_create(&mount->db, path.fileSystemRepresentation, &stat);
    if (inode != NULL)
        *inode = path_get_inode(&mount->db, path.fileSystemRepresentation);
    db_commit(&mount->db);
    return YES;
}

- (BOOL)doCreateFileAt:(NSString *)path importFrom:(NSURL *)importURL mount:(struct fakefs_mount *)mount inode:(ino_t *)inode error:(NSError **)error {
    NSURL *url = [[NSURL fileURLWithPath:[NSString stringWithUTF8String:mount->source]] URLByAppendingPathComponent:ISHHostPathForGuestPath(path)];
    if (![NSFileManager.defaultManager copyItemAtURL:importURL
                                               toURL:url
                                               error:error]) {
        return NO;
    }
    db_begin_write(&mount->db);
    struct ish_stat stat;
    NSString *parentPath = [path substringToIndex:[path rangeOfString:@"/" options:NSBackwardsSearch].location];
    if (!path_read_stat(&mount->db, parentPath.fileSystemRepresentation, &stat, NULL)) {
        db_rollback(&mount->db);
        [NSFileManager.defaultManager removeItemAtURL:url error:nil];
        if (error != nil)
            *error = [NSError errorWithDomain:NSFileProviderErrorDomain code:NSFileProviderErrorNoSuchItem userInfo:nil];
        return NO;
    }
    stat.mode = (stat.mode & ~S_IFMT & ~0111) | S_IFREG;
    path_create(&mount->db, path.fileSystemRepresentation, &stat);
    if (inode != NULL)
        *inode = path_get_inode(&mount->db, path.fileSystemRepresentation);
    db_commit(&mount->db);
    return YES;
}

- (NSString *)pathOfItemWithIdentifier:(NSFileProviderItemIdentifier)identifier error:(NSError **)error {
    FileProviderItem *parent = [self itemForIdentifier:identifier error:error];
    if (parent == nil)
        return nil;
    return parent.path;
}

#pragma mark - Actions

/* TODO: implement the actions for items here
 each of the actions follows the same pattern:
 - make a note of the change in the local model
 - schedule a server request as a background task to inform the server of the change
 - call the completion block with the modified item in its post-modification state
 */

- (void)createDirectoryWithName:(NSString *)directoryName inParentItemIdentifier:(NSFileProviderItemIdentifier)parentItemIdentifier completionHandler:(void (^)(NSFileProviderItem _Nullable, NSError * _Nullable))completionHandler {
    NSError *error;
    NSString *rootName = ISHFileProviderRootNameForIdentifier(parentItemIdentifier);
    if (rootName == nil) {
        // The top level lists roots; you can't create a directory directly there.
        completionHandler(nil, [NSError errorWithDomain:NSFileProviderErrorDomain code:NSFileProviderErrorNoSuchItem userInfo:nil]);
        return;
    }
    ISHFileProviderMount *mountOwner = [self mountForRootName:rootName error:&error];
    if (mountOwner == nil) {
        completionHandler(nil, error);
        return;
    }
    NSString *parentPath = [self pathOfItemWithIdentifier:parentItemIdentifier error:&error];
    if (parentPath == nil) {
        completionHandler(nil, error);
        return;
    }
    ino_t inode;
    int rootLockFd = ISHFileProviderAcquireRootLock(rootName, YES);
    [mountOwner.ioLock lock];
    NSString *newPath = [parentPath stringByAppendingFormat:@"/%@", directoryName];
    BOOL worked = [self doCreateDirectoryAt:newPath mount:mountOwner.mount inode:&inode error:&error];
    [mountOwner.ioLock unlock];
    ISHAppGroupReleaseLock(rootLockFd);
    if (!worked) {
        completionHandler(nil, error);
        return;
    }
    NSFileProviderItemIdentifier newIdentifier = ISHFileProviderComposeIdentifier(rootName, [NSString stringWithFormat:@"%lu", (unsigned long) inode]);
    FileProviderItem *item = [self itemForIdentifier:newIdentifier error:&error];
    if (item == nil)
        completionHandler(nil, error);
    else
        completionHandler(item, nil);
}

- (void)importDocumentAtURL:(NSURL *)fileURL toParentItemIdentifier:(NSFileProviderItemIdentifier)parentItemIdentifier completionHandler:(void (^)(NSFileProviderItem _Nullable, NSError * _Nullable))completionHandler {
    NSError *error;
    NSString *rootName = ISHFileProviderRootNameForIdentifier(parentItemIdentifier);
    if (rootName == nil) {
        completionHandler(nil, [NSError errorWithDomain:NSFileProviderErrorDomain code:NSFileProviderErrorNoSuchItem userInfo:nil]);
        return;
    }
    ISHFileProviderMount *mountOwner = [self mountForRootName:rootName error:&error];
    if (mountOwner == nil) {
        completionHandler(nil, error);
        return;
    }
    NSString *parentPath = [self pathOfItemWithIdentifier:parentItemIdentifier error:&error];
    if (parentPath == nil) {
        completionHandler(nil, error);
        return;
    }

    // Balance the stop against the start only when the start succeeded, which
    // is Apple's contract. A URL that is not security-scoped returns NO and is
    // still perfectly readable, so NO is not by itself a failure.
    BOOL accessing = [fileURL startAccessingSecurityScopedResource];

    // Both of these are ordinary, user-reachable outcomes rather than broken
    // invariants: Files hands us a directory, or the source has moved (or its
    // security scope was denied) so the path no longer resolves. They were an
    // assert(), which turned each into an abort() that killed the whole
    // extension from inside Apple's createItemBasedOnTemplate: call. Five
    // reported crashes on build 545 alone, across three devices on iOS 26.5
    // and 26.6. Report them as errors and let Files show a refusal.
    BOOL isDir = NO;
    if (![NSFileManager.defaultManager fileExistsAtPath:fileURL.path isDirectory:&isDir]) {
        if (accessing)
            [fileURL stopAccessingSecurityScopedResource];
        completionHandler(nil, [NSError errorWithDomain:NSPOSIXErrorDomain code:ENOENT userInfo:nil]);
        return;
    }
    if (isDir) {
        // Importing a directory would need the recursive copy this path does
        // not do; doCreateFileAt:importFrom: copies one file's contents.
        if (accessing)
            [fileURL stopAccessingSecurityScopedResource];
        completionHandler(nil, [NSError errorWithDomain:NSPOSIXErrorDomain code:EISDIR userInfo:nil]);
        return;
    }
    ino_t inode;
    int rootLockFd = ISHFileProviderAcquireRootLock(rootName, YES);
    [mountOwner.ioLock lock];
    NSString *newPath = [parentPath stringByAppendingFormat:@"/%@", fileURL.lastPathComponent];
    BOOL worked = [self doCreateFileAt:newPath
                            importFrom:fileURL
                                 mount:mountOwner.mount
                                 inode:&inode
                                 error:&error];
    [mountOwner.ioLock unlock];
    ISHAppGroupReleaseLock(rootLockFd);
    if (accessing)
        [fileURL stopAccessingSecurityScopedResource];
    if (!worked) {
        completionHandler(nil, error);
        return;
    }

    NSFileProviderItemIdentifier newIdentifier = ISHFileProviderComposeIdentifier(rootName, [NSString stringWithFormat:@"%lu", (unsigned long) inode]);
    FileProviderItem *item = [self itemForIdentifier:newIdentifier error:&error];
    if (item == nil)
        completionHandler(nil, error);
    else
        completionHandler(item, nil);
}

// Files is told what each item allows (-capabilities), but the actions below
// are reached by identifier and never asked. A root folder, a virtual item
// (an entry with no db row), or a path that is a root's own "/" is never
// deleted, renamed, or moved, whatever the request says.
static BOOL ISHFileProviderItemIsChangeable(FileProviderItem *item, NSFileProviderItemCapabilities needed) {
    if ((item.capabilities & needed) != needed)
        return NO;
    NSString *path = item.path;
    return path.length > 1 && [path hasPrefix:@"/"];
}

- (NSString *)pathFromURL:(NSURL *)url mount:(struct fakefs_mount *)mount {
    NSURL *root = [NSURL fileURLWithPath:[NSString stringWithUTF8String:mount->source]];
    assert([url.path hasPrefix:root.path]);
    NSString *path = [url.path substringFromIndex:root.path.length];
    assert([path hasPrefix:@"/"]);
    if ([path hasSuffix:@"/"])
        path = [path substringToIndex:path.length - 1];
    // the URL names the escaped on-disk file; return the guest path
    return ISHGuestPathForHostPath(path);
}

- (BOOL)doDelete:(NSString *)path mount:(struct fakefs_mount *)mount itemIdentifier:(NSFileProviderItemIdentifier)identifier error:(NSError **)error {
    NSURL *url = [[NSURL fileURLWithPath:[NSString stringWithUTF8String:mount->source]] URLByAppendingPathComponent:ISHHostPathForGuestPath(path)];
    NSDirectoryEnumerator<NSURL *> *enumerator = [NSFileManager.defaultManager enumeratorAtURL:url
                                                                    includingPropertiesForKeys:nil
                                                                                       options:NSDirectoryEnumerationSkipsSubdirectoryDescendants
                                                                                  errorHandler:nil];
    for (NSURL *suburl in enumerator) {
        if (![self doDelete:[self pathFromURL:suburl mount:mount] mount:mount itemIdentifier:identifier error:error])
            return NO;
    }
    db_begin_write(&mount->db);
    path_unlink(&mount->db, path.fileSystemRepresentation);
    const char *hostPath = ISHHostPathForGuestPath(path).fileSystemRepresentation;
    int err = unlinkat(mount->root_fd, fix_path(hostPath), 0);
    if (err < 0)
        err = unlinkat(mount->root_fd, fix_path(hostPath), AT_REMOVEDIR);
    if (err < 0) {
        db_rollback(&mount->db);
        if (error != nil)
            *error = [NSError errorWithDomain:NSPOSIXErrorDomain code:errno userInfo:nil];
        return NO;
    }
    db_commit(&mount->db);
    return YES;
}

- (void)deleteItemWithIdentifier:(NSFileProviderItemIdentifier)itemIdentifier completionHandler:(void (^)(NSError * _Nullable))completionHandler {
    NSError *error;
    NSString *rootName = ISHFileProviderRootNameForIdentifier(itemIdentifier);
    if (rootName == nil || [ISHFileProviderInnerIdentifier(itemIdentifier) isEqualToString:NSFileProviderRootContainerItemIdentifier]) {
        // Don't allow deleting the top level or an entire root through Files.
        completionHandler([NSError errorWithDomain:NSFileProviderErrorDomain code:NSFileProviderErrorNoSuchItem userInfo:nil]);
        return;
    }
    ISHFileProviderMount *mountOwner = [self mountForRootName:rootName error:&error];
    if (mountOwner == nil) {
        completionHandler(error);
        return;
    }
    FileProviderItem *item = [self itemForIdentifier:itemIdentifier error:&error];
    if (item == nil) {
        completionHandler(error);
        return;
    }
    if (!ISHFileProviderItemIsChangeable(item, NSFileProviderItemCapabilitiesAllowsDeleting)) {
        completionHandler([NSError errorWithDomain:NSCocoaErrorDomain code:NSFileWriteNoPermissionError userInfo:nil]);
        return;
    }
    NSString *path = item.path;
    int rootLockFd = ISHFileProviderAcquireRootLock(rootName, YES);
    [mountOwner.ioLock lock];
    BOOL worked = [self doDelete:path mount:mountOwner.mount itemIdentifier:itemIdentifier error:&error];
    [mountOwner.ioLock unlock];
    ISHAppGroupReleaseLock(rootLockFd);
    if (!worked)
        completionHandler(error);
    else
        completionHandler(nil);
}

- (BOOL)doRename:(NSString *)src to:(NSString *)dst mount:(struct fakefs_mount *)mount itemIdentifier:(NSFileProviderItemIdentifier)identifier error:(NSError **)error {
    db_begin_write(&mount->db);
    path_rename(&mount->db, src.fileSystemRepresentation, dst.fileSystemRepresentation);
    int err = renameat(mount->root_fd, fix_path(ISHHostPathForGuestPath(src).fileSystemRepresentation),
                       mount->root_fd, fix_path(ISHHostPathForGuestPath(dst).fileSystemRepresentation));
    if (err < 0) {
        db_rollback(&mount->db);
        if (error != nil)
            *error = [NSError errorWithDomain:NSPOSIXErrorDomain code:errno userInfo:nil];
        return NO;
    }
    db_commit(&mount->db);
    return YES;
}

- (void)renameItemWithIdentifier:(NSFileProviderItemIdentifier)itemIdentifier toName:(NSString *)itemName completionHandler:(void (^)(NSFileProviderItem _Nullable, NSError * _Nullable))completionHandler {
    NSError *error;
    NSString *rootName = ISHFileProviderRootNameForIdentifier(itemIdentifier);
    if (rootName == nil || [ISHFileProviderInnerIdentifier(itemIdentifier) isEqualToString:NSFileProviderRootContainerItemIdentifier]) {
        completionHandler(nil, [NSError errorWithDomain:NSFileProviderErrorDomain code:NSFileProviderErrorNoSuchItem userInfo:nil]);
        return;
    }
    ISHFileProviderMount *mountOwner = [self mountForRootName:rootName error:&error];
    if (mountOwner == nil) {
        completionHandler(nil, error);
        return;
    }
    FileProviderItem *item = [self itemForIdentifier:itemIdentifier error:&error];
    if (item == nil) {
        completionHandler(nil, error);
        return;
    }
    if (!ISHFileProviderItemIsChangeable(item, NSFileProviderItemCapabilitiesAllowsRenaming)) {
        completionHandler(nil, [NSError errorWithDomain:NSCocoaErrorDomain code:NSFileWriteNoPermissionError userInfo:nil]);
        return;
    }
    NSString *dstPath = [item.path.stringByDeletingLastPathComponent stringByAppendingPathComponent:itemName];
    int rootLockFd = ISHFileProviderAcquireRootLock(rootName, YES);
    [mountOwner.ioLock lock];
    BOOL worked = [self doRename:item.path to:dstPath mount:mountOwner.mount itemIdentifier:itemIdentifier error:&error];
    [mountOwner.ioLock unlock];
    ISHAppGroupReleaseLock(rootLockFd);
    if (!worked) {
        completionHandler(nil, error);
        return;
    }
    completionHandler(item, nil);
}

- (void)reparentItemWithIdentifier:(NSFileProviderItemIdentifier)itemIdentifier toParentItemWithIdentifier:(NSFileProviderItemIdentifier)parentItemIdentifier newName:(NSString *)newName completionHandler:(void (^)(NSFileProviderItem _Nullable, NSError * _Nullable))completionHandler {
    NSError *error;
    NSString *rootName = ISHFileProviderRootNameForIdentifier(itemIdentifier);
    NSString *parentRootName = ISHFileProviderRootNameForIdentifier(parentItemIdentifier);
    if (rootName == nil || parentRootName == nil || ![rootName isEqualToString:parentRootName]) {
        // Moving across roots (or to/from the top level) isn't supported.
        completionHandler(nil, [NSError errorWithDomain:NSFileProviderErrorDomain code:NSFileProviderErrorNoSuchItem userInfo:nil]);
        return;
    }
    ISHFileProviderMount *mountOwner = [self mountForRootName:rootName error:&error];
    if (mountOwner == nil) {
        completionHandler(nil, error);
        return;
    }
    FileProviderItem *item = [self itemForIdentifier:itemIdentifier error:&error];
    if (item == nil) {
        completionHandler(nil, error);
        return;
    }
    FileProviderItem *parent = [self itemForIdentifier:parentItemIdentifier error:&error];
    if (parent == nil) {
        completionHandler(nil, error);
        return;
    }
    if (!ISHFileProviderItemIsChangeable(item, NSFileProviderItemCapabilitiesAllowsReparenting)) {
        completionHandler(nil, [NSError errorWithDomain:NSCocoaErrorDomain code:NSFileWriteNoPermissionError userInfo:nil]);
        return;
    }
    if (newName == nil)
        newName = item.path.lastPathComponent;
    // Join with an explicit slash: a root folder's path is "", and
    // -stringByAppendingPathComponent: on "" gives a RELATIVE "name", which
    // went into the db as a path no guest lookup can reach -- the moved file
    // vanished from the guest while its host file sat in the root's top folder.
    NSString *dstPath = [NSString stringWithFormat:@"%@/%@", parent.path, newName];
    int rootLockFd = ISHFileProviderAcquireRootLock(rootName, YES);
    [mountOwner.ioLock lock];
    BOOL worked = [self doRename:item.path to:dstPath mount:mountOwner.mount itemIdentifier:itemIdentifier error:&error];
    [mountOwner.ioLock unlock];
    ISHAppGroupReleaseLock(rootLockFd);
    if (!worked) {
        completionHandler(nil, error);
        return;
    }
    completionHandler(item, nil);
}

#pragma mark - Enumeration

- (nullable id<NSFileProviderEnumerator>)enumeratorForContainerItemIdentifier:(NSFileProviderItemIdentifier)containerItemIdentifier error:(NSError **)error {
    FileProviderItem *item = [self itemForIdentifier:containerItemIdentifier error:error];
    if (item == nil)
        return nil;
    if (item.mountOwner == nil) // the domain root: opportune moment to GC storage
        [self maybeCleanupStorage];
    return [[FileProviderEnumerator alloc] initWithItem:item extension:self];
}

#pragma mark - Storage deletion

// According to an engineer I talked to at WWDC, -stopProvidingItemAtURL: is never ever called, so that can't be used to free up disk space.
// Solution for now is to periodically look for and delete files in file provider storage where the original is missing.
// TODO: Delete files in file provider storage when the original file is deleted
// TODO: Create hardlinks into file provider storage instead of copies
//
- (void)cleanupStorage {
    NSTimeInterval start = NSDate.date.timeIntervalSinceReferenceDate;
    NSFileManager *manager = NSFileManager.defaultManager;
    NSArray<NSURL *> *storageDirs = [manager contentsOfDirectoryAtURL:self.storageURL includingPropertiesForKeys:nil options:0 error:nil];
    NSUInteger removedCount = 0;
    for (NSURL *dir in storageDirs) {
        // Materialized storage dirs are named by the fully-scoped identifier,
        // "<root>+<inode>". Root folders and virtual items have no inode and are
        // left alone.
        NSString *fullIdentifier = dir.lastPathComponent;
        NSString *rootName = ISHFileProviderRootNameForIdentifier(fullIdentifier);
        NSFileProviderItemIdentifier inner = ISHFileProviderInnerIdentifier(fullIdentifier);
        inode_t inode = inner.longLongValue;
        if (rootName == nil || inode == 0)
            continue;
        ISHFileProviderMount *mountOwner = [self mountForRootName:rootName error:nil];
        if (mountOwner == nil)
            continue;

        int rootLockFd = ISHFileProviderAcquireRootLock(rootName, NO);
        BOOL exists = inode_exists(&mountOwner.mount->db, inode);
        ISHAppGroupReleaseLock(rootLockFd);

        if (!exists) {
            NSLog(@"removing dead inode %llu in %@", inode, rootName);
            NSError *err;
            if (![manager removeItemAtURL:dir error:&err])
                NSLog(@"failed to remove dead inode: %@", err);
            else
                removedCount++;
        }
    }
    NSNumber *duration = ISHFileProviderDurationMilliseconds(start);
    if (removedCount != 0 || duration.integerValue >= 500) {
        NSMutableDictionary<NSString *, id> *details = ISHFileProviderDetails(@"all");
        details[@"duration_ms"] = duration;
        details[@"removed"] = @(removedCount);
        ISHFileProviderRecordBreadcrumb(@"fileprovider.cleanup", details);
    }
}

// The system does call this, when it evicts a materialized copy. It must not
// write that copy back: edits already arrived through -itemChangedAtURL:, so
// all a write-back can do is put an old copy over what the guest has written
// since -- or, for a materialized folder, replace the folder with its snapshot.
- (void)stopProvidingItemAtURL:(NSURL *)url {
    FileProviderItem *item = [self itemForIdentifier:[self persistentIdentifierForItemAtURL:url] error:nil];
    if (item == nil)
        return;
    [[NSFileManager defaultManager] removeItemAtURL:url error:nil];
    [NSFileProviderManager writePlaceholderAtURL:[NSFileProviderManager placeholderURLForURL:url]
                                    withMetadata:item
                                           error:nil];
}

@end

void die(const char *msg, ...) {
    va_list args;
    va_start(args, msg);
    [NSException raise:@"ish die" format:[NSString stringWithUTF8String:msg] arguments:args];
    abort();
    va_end(args);
}

void ish_printk(const char *msg, ...) {
    va_list args;
    va_start(args, msg);
    NSLogv([NSString stringWithUTF8String:msg], args);
    va_end(args);
}
