//
//  Roots.m
//  iSH
//
//  Created by Theodore Dubois on 6/7/20.
//

#import <FileProvider/FileProvider.h>
#import "Diagnostics.h"
#import "Roots.h"
#import "AppGroup.h"
#import "AppDelegate.h"
#import "GuestFileBridge.h"
#include "fs/proc/ish.h"
#include "kernel/errno.h"
#import "NSObject+SaneKVO.h"
#include <archive.h>
#include <archive_entry.h>
#include "tools/fakefs.h"
#include "kernel/calls.h"
#ifdef __APPLE__
#include <errno.h>
#include <sys/resource.h>
#define IOPOL_TYPE_VFS_HFS_CASE_SENSITIVITY 1
#define IOPOL_VFS_HFS_CASE_SENSITIVITY_FORCE_CASE_SENSITIVE 1
#endif

static NSURL *RootsDir(void) {
    static NSURL *rootsDir;
    static dispatch_once_t token;
    dispatch_once(&token, ^{
        NSURL *container = ContainerURL();
        if (container == nil) {
            NSLog(@"RootsDir: no app group container available (missing or misconfigured App Group entitlement?)");
            return;
        }
        rootsDir = [container URLByAppendingPathComponent:@"roots"];
        NSFileManager *manager = [NSFileManager defaultManager];
        NSError *error = nil;
        if (![manager createDirectoryAtURL:rootsDir
                withIntermediateDirectories:YES
                                 attributes:@{}
                                      error:&error]) {
            NSLog(@"RootsDir: couldn't create roots directory: %@", error);
        }
    });
    return rootsDir;
}

// /AOK/persist is a single shared real-fs mount -- the AppGroup container's
// AOK/persist directory (see AOKPersistDirectoryURL / the do_mount in
// AppDelegate). It is the same regardless of which root is booted and is NOT
// inside any root's fakefs data dir (that's just an empty mount point), so
// both cachedRootArchiveURLs and the bundled-archive downloader address it
// directly as a host directory.
static NSURL *PersistRootsDir(void) {
    NSURL *container = ContainerURL();
    if (container == nil)
        return nil;
    return [[[container URLByAppendingPathComponent:@"AOK" isDirectory:YES]
             URLByAppendingPathComponent:@"persist" isDirectory:YES]
            URLByAppendingPathComponent:@"roots" isDirectory:YES];
}

static NSString *kDefaultRoot = @"Default Root";
// A single file-provider domain hosts every installed root as a folder, instead
// of registering one domain per root (which spammed the Files sidebar). Must
// stay in sync with the displayName shown in Files and the deep-link path.
static NSString *const kISHFileProviderDomainIdentifier = @"iSH-AOK";
static NSString *const kBundledRootIdentifierKey = @"identifier";
static NSString *const kBundledRootDisplayNameKey = @"displayName";
static NSString *const kBundledRootArchiveNameKey = @"archiveName";
static NSString *const kBundledRootImportNameKey = @"importName";
static NSString *const kBundledRootInitialWindowKey = @"initialWindow";
static NSString *const kBundledRootGuestABIKey = @"guestABI";
// Groups arch variants of the same distro together in the picker UI (e.g. the
// four Alpine entries below share family "alpine3233") so RootsTableViewController
// can show one row per distro and offer architecture as a sub-choice, instead of
// one flat row per distro_arch combination.
static NSString *const kBundledRootFamilyKey = @"family";
static NSString *const kBundledRootFamilyDisplayNameKey = @"familyDisplayName";
// "official" (maintained as part of iSH-AOK's regression-tested base images) vs
// "community" (contributed/experimental, no distro-specific patching guarantees).
static NSString *const kBundledRootTierKey = @"tier";
static NSString *const kBundledRootTierOfficial = @"official";
static NSString *const kBundledRootTierCommunity = @"community";
// Present only for choices whose archive isn't shipped in the app bundle --
// importing them downloads this URL into /AOK/persist/roots on demand
// instead (see DownloadBundledArchive / importBundledRootChoice:).
static NSString *const kBundledRootDownloadURLKey = @"downloadURL";
static NSString *const kBundledRootDownloadSizeKey = @"downloadSize";
static NSString *const kRootsErrorDomain = @"iSH.Roots";
static NSString *const kRootMetadataFileName = @"ish-root.plist";
static NSString *const kRootMetadataGuestABIKey = @"guestABI";

NSNotificationName const RootsDidFinishInitialSelectionNotification = @"RootsDidFinishInitialSelectionNotification";

// The download-backed choices (everything with a kBundledRootDownloadURLKey)
// live in the deps/rootfs-manifest submodule (github.com/emkey1/ish-AOK-rootfs)
// as manifest.json, bundled into the app as the "rootfs-manifest" resource --
// see that repo's README for the entry schema and how to contribute a new
// rootfs without touching this file. Required keys are validated here so a
// malformed community PR degrades to "entry silently dropped", never a crash.
static NSArray<NSString *> *RequiredManifestKeys(void) {
    static NSArray<NSString *> *keys;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        keys = @[kBundledRootIdentifierKey, kBundledRootDisplayNameKey, kBundledRootArchiveNameKey,
                 kBundledRootImportNameKey, kBundledRootInitialWindowKey, kBundledRootGuestABIKey,
                 kBundledRootDownloadURLKey, kBundledRootDownloadSizeKey, kBundledRootFamilyKey,
                 kBundledRootFamilyDisplayNameKey];
    });
    return keys;
}

static NSArray<NSDictionary<NSString *, NSString *> *> *LoadDownloadableRootChoicesFromManifest(void) {
    NSURL *manifestURL = [NSBundle.mainBundle URLForResource:@"manifest" withExtension:@"json"];
    if (manifestURL == nil) {
        NSLog(@"manifest.json not found in app bundle (deps/rootfs-manifest submodule) -- no downloadable filesystem choices available");
        return @[];
    }
    NSData *data = [NSData dataWithContentsOfURL:manifestURL];
    NSError *error = nil;
    id parsed = data != nil ? [NSJSONSerialization JSONObjectWithData:data options:0 error:&error] : nil;
    if (![parsed isKindOfClass:NSArray.class]) {
        NSLog(@"rootfs-manifest.json could not be parsed: %@", error);
        return @[];
    }

    NSMutableArray<NSDictionary<NSString *, NSString *> *> *entries = [NSMutableArray array];
    NSMutableSet<NSString *> *seenIdentifiers = [NSMutableSet set];
    for (id rawEntry in (NSArray *) parsed) {
        if (![rawEntry isKindOfClass:NSDictionary.class]) {
            NSLog(@"rootfs-manifest.json: skipping non-object entry");
            continue;
        }
        NSDictionary<NSString *, NSString *> *entry = rawEntry;
        BOOL valid = YES;
        for (NSString *key in RequiredManifestKeys()) {
            id value = entry[key];
            if (![value isKindOfClass:NSString.class] || [(NSString *) value length] == 0) {
                NSLog(@"rootfs-manifest.json: entry missing required key '%@', skipping", key);
                valid = NO;
                break;
            }
        }
        if (!valid)
            continue;
        NSString *identifier = entry[kBundledRootIdentifierKey];
        if ([seenIdentifiers containsObject:identifier]) {
            NSLog(@"rootfs-manifest.json: duplicate identifier '%@', skipping", identifier);
            continue;
        }
        [seenIdentifiers addObject:identifier];

        NSString *tier = entry[kBundledRootTierKey];
        if (![tier isEqualToString:kBundledRootTierOfficial] && ![tier isEqualToString:kBundledRootTierCommunity]) {
            NSMutableDictionary<NSString *, NSString *> *fixedUp = [entry mutableCopy];
            fixedUp[kBundledRootTierKey] = kBundledRootTierCommunity;
            entry = fixedUp;
        }
        [entries addObject:entry];
    }
    return entries;
}

static NSArray<NSDictionary<NSString *, NSString *> *> *BundledRootChoices(void) {
    static NSArray<NSDictionary<NSString *, NSString *> *> *choices;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        NSMutableArray<NSDictionary<NSString *, NSString *> *> *mutableChoices = [@[
            // Bundled-in-the-app choices (arm64 guests -- the only architecture
            // still shipped in the IPA); every download-backed choice comes from
            // the rootfs-manifest submodule instead (loaded below).
            // RootsTableViewController groups these by kBundledRootFamilyKey
            // (one row per distro, architecture picked as a sub-choice) and
            // splits into "Official Distributions" / "Community Distributions"
            // table sections along kBundledRootTierKey.
            @{
                kBundledRootIdentifierKey: @"alpine3233arm64",
                kBundledRootDisplayNameKey: @"Alpine3.23.3(arm64)",
                kBundledRootArchiveNameKey: @"alpine-minirootfs-3.23.3-aarch64",
                // Native AArch64 guest (same-architecture dispatch on Apple
                // silicon — see aarch64_guest_plan.md). Import name follows
                // the RootNameIsValid rules like the x86_64 entry above.
                kBundledRootImportNameKey: @"Alpine3.23.3-arm64",
                kBundledRootInitialWindowKey: @"session-shell",
                kBundledRootGuestABIKey: @"arm64",
                kBundledRootFamilyKey: @"alpine3233",
                kBundledRootFamilyDisplayNameKey: @"Alpine 3.23.3",
                kBundledRootTierKey: kBundledRootTierOfficial,
            },
            @{
                kBundledRootIdentifierKey: @"devuan6arm64",
                kBundledRootDisplayNameKey: @"Devuan 6 (excalibur, arm64)",
                kBundledRootArchiveNameKey: @"devuan-minirootfs-6.0-aarch64",
                kBundledRootImportNameKey: @"Devuan6-arm64",
                kBundledRootInitialWindowKey: @"session-shell",
                kBundledRootGuestABIKey: @"arm64",
                kBundledRootFamilyKey: @"devuan6",
                kBundledRootFamilyDisplayNameKey: @"Devuan 6 (excalibur)",
                kBundledRootTierKey: kBundledRootTierOfficial,
            },
        ] mutableCopy];
        [mutableChoices addObjectsFromArray:LoadDownloadableRootChoicesFromManifest()];
        choices = mutableChoices;
    });
    return choices;
}

// Bundled root archives may ship in any container format libarchive (and thus
// fakefs_import) can read -- gzip, xz, zstd, bzip2, ... -- so resolve the
// bundled resource by trying each supported extension instead of assuming
// .tar.gz. (Keep the extension list in sync with the cached-archive suffixes in
// cachedRootArchiveURLs and the filters in fakefs_import / tools/fakefs.c.)
static NSURL *BundledRootArchiveURL(NSString *archiveName) {
    if (archiveName.length == 0)
        return nil;
    static NSArray<NSString *> *extensions;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        extensions = @[@"tar.xz", @"tar.zst", @"tar.gz", @"tar.bz2",
                       @"tar.lz", @"tar.lzma", @"txz", @"tzst", @"tgz", @"tar"];
    });
    for (NSString *ext in extensions) {
        NSURL *url = [NSBundle.mainBundle URLForResource:archiveName withExtension:ext];
        if (url != nil)
            return url;
    }
    return nil;
}

// Where a download-backed bundled choice's archive lands once fetched --
// alongside (and indistinguishable from, once present) any archive a user
// drops into /AOK/persist/roots themselves.
static NSURL *DownloadedBundledArchiveURL(NSDictionary<NSString *, NSString *> *choice) {
    NSString *archiveName = choice[kBundledRootArchiveNameKey];
    NSURL *dir = PersistRootsDir();
    if (archiveName.length == 0 || dir == nil)
        return nil;
    return [dir URLByAppendingPathComponent:[archiveName stringByAppendingString:@".tar.xz"]];
}

// The guest path an installed root other than the booted one is exposed at.
// One spelling, because unmounting the old name and mounting the new one have
// to agree with each other and with what boot produced. (Only the iSH kernel
// has /AOK/roots; the iSH+Linux build has no mounts to name.)
static NSString *ExposedRootPoint(NSString *name) {
    return [@"/AOK/roots/" stringByAppendingString:name];
}

// "It wasn't there" is a fine outcome for something we are removing, not a
// failure worth logging. (AppDelegate.m has its own copy for the same reason;
// it is four lines and neither file is the natural home for the other's.)
static BOOL RootsIsFileNotFoundError(NSError *error) {
    if (error == nil)
        return YES;
    return [error.domain isEqualToString:NSCocoaErrorDomain] &&
           (error.code == NSFileNoSuchFileError || error.code == NSFileReadNoSuchFileError);
}

static NSURL *RootMetadataURL(NSURL *rootURL) {
    return [rootURL URLByAppendingPathComponent:kRootMetadataFileName];
}

static NSDictionary<NSString *, id> *ReadRootMetadata(NSURL *rootURL) {
    NSDictionary<NSString *, id> *metadata =
        [NSDictionary dictionaryWithContentsOfURL:RootMetadataURL(rootURL)];
    if (![metadata isKindOfClass:NSDictionary.class])
        return nil;
    return metadata;
}

static void WriteRootMetadata(NSURL *rootURL, NSDictionary<NSString *, id> *metadata) {
    if (metadata.count == 0)
        return;
    NSError *error = nil;
    if (![metadata writeToURL:RootMetadataURL(rootURL) error:&error]) {
        NSLog(@"could not write root metadata for %@: %@", rootURL.lastPathComponent, error);
    }
}

static BOOL RootURLLooksValid(NSURL *url) {
    if (url == nil)
        return NO;

    BOOL isDirectory = NO;
    if (![NSFileManager.defaultManager fileExistsAtPath:url.path isDirectory:&isDirectory] || !isDirectory)
        return NO;

    NSURL *dataURL = [url URLByAppendingPathComponent:@"data"];
    NSURL *metaURL = [url URLByAppendingPathComponent:@"meta.db"];
    BOOL isDataDirectory = NO;
    if (![NSFileManager.defaultManager fileExistsAtPath:dataURL.path isDirectory:&isDataDirectory] || !isDataDirectory)
        return NO;
    if (![NSFileManager.defaultManager fileExistsAtPath:metaURL.path])
        return NO;

    return YES;
}

static NSString *FormatByteCount(long long value) {
    return [NSByteCountFormatter stringFromByteCount:value countStyle:NSByteCountFormatterCountStyleFile];
}

static NSError *RootsStorageError(NSString *description, NSString *recoverySuggestion) {
    NSMutableDictionary *userInfo = [NSMutableDictionary dictionaryWithObject:description
                                                                       forKey:NSLocalizedDescriptionKey];
    if (recoverySuggestion != nil)
        userInfo[NSLocalizedRecoverySuggestionErrorKey] = recoverySuggestion;
    return [NSError errorWithDomain:kRootsErrorDomain code:NSFileWriteOutOfSpaceError userInfo:userInfo];
}

static NSError *FakefsImportNSError(struct fakefsify_error fs_err) {
    NSString *description = nil;
    NSString *recoverySuggestion = nil;
    NSString *domain = NSPOSIXErrorDomain;
    if (fs_err.type == ERR_SQLITE)
        domain = @"SQLite";

    if (fs_err.type == ERR_POSIX && fs_err.code == ENOSPC) {
        description = @"Not enough free space to extract the filesystem.";
        recoverySuggestion = @"Free up storage space and try again.";
    } else if (fs_err.type == ERR_ARCHIVE) {
        description = @"The filesystem archive could not be read.";
    } else if (fs_err.type == ERR_SQLITE) {
        description = @"The filesystem metadata database could not be created.";
    } else {
        description = [NSString stringWithUTF8String:fs_err.message];
    }

    NSMutableDictionary *userInfo = [NSMutableDictionary dictionaryWithObject:description
                                                                       forKey:NSLocalizedDescriptionKey];
    if (recoverySuggestion != nil)
        userInfo[NSLocalizedRecoverySuggestionErrorKey] = recoverySuggestion;
    if (fs_err.message != NULL && fs_err.message[0] != '\0') {
        userInfo[NSLocalizedFailureReasonErrorKey] = [NSString stringWithFormat:@"%s (line %d)",
                                                      fs_err.message, fs_err.line];
    }
    return [NSError errorWithDomain:domain code:fs_err.code userInfo:userInfo];
}

static BOOL EstimateArchiveExtractionRequirement(NSURL *archiveURL, long long *requiredBytes, NSError **error) {
    struct archive *archive = archive_read_new();
    if (archive == NULL) {
        if (error != NULL) {
            *error = [NSError errorWithDomain:@"libarchive"
                                         code:ENOMEM
                                     userInfo:@{NSLocalizedDescriptionKey: @"Could not allocate archive reader."}];
        }
        return NO;
    }
    archive_read_support_filter_all(archive); // gzip, bzip2, xz, zstd, ... (match fakefs_import)
    archive_read_support_format_tar(archive);
    if (archive_read_open_filename(archive, archiveURL.fileSystemRepresentation, 65536) != ARCHIVE_OK) {
        if (error != NULL) {
            *error = [NSError errorWithDomain:@"libarchive"
                                         code:archive_errno(archive)
                                     userInfo:@{NSLocalizedDescriptionKey:
                                                    @"The filesystem archive could not be read."}];
        }
        archive_read_free(archive);
        return NO;
    }

    long long payloadBytes = 0;
    long long entryCount = 0;
    struct archive_entry *entry = NULL;
    int err = ARCHIVE_OK;
    while (true) {
        err = archive_read_next_header(archive, &entry);
        if (err == ARCHIVE_EOF)
            break;
        // ARCHIVE_WARN is non-fatal (e.g. a UTF-8 link path that can't also be
        // rendered in the process locale); keep estimating. Only hard errors abort.
        if (err != ARCHIVE_OK && err != ARCHIVE_WARN) {
            if (error != NULL) {
                *error = [NSError errorWithDomain:@"libarchive"
                                             code:archive_errno(archive)
                                         userInfo:@{NSLocalizedDescriptionKey:
                                                        @"The filesystem archive could not be read."}];
            }
            archive_read_free(archive);
            return NO;
        }
        entryCount++;
        la_int64_t entrySize = archive_entry_size(entry);
        if (entrySize > 0)
            payloadBytes += entrySize;
        archive_read_data_skip(archive);
    }
    archive_read_free(archive);

    long long metadataOverhead = MAX(entryCount * 2048LL, 8LL * 1024 * 1024);
    long long safetyMargin = 64LL * 1024 * 1024;
    *requiredBytes = payloadBytes + metadataOverhead + safetyMargin;
    return YES;
}

static BOOL RootsCheckAvailableSpaceForArchive(NSURL *archiveURL, NSURL *destinationDirectory, NSError **error) {
    long long requiredBytes = 0;
    NSError *estimateError = nil;
    if (!EstimateArchiveExtractionRequirement(archiveURL, &requiredBytes, &estimateError)) {
        if (error != NULL)
            *error = estimateError;
        return NO;
    }

    NSNumber *availableBytes = nil;
    NSError *resourceError = nil;
    if (@available(iOS 11.0, *)) {
        NSDictionary<NSURLResourceKey, id> *values = [destinationDirectory resourceValuesForKeys:@[
            NSURLVolumeAvailableCapacityForImportantUsageKey,
            NSURLVolumeAvailableCapacityKey,
        ] error:&resourceError];
        availableBytes = values[NSURLVolumeAvailableCapacityForImportantUsageKey];
        if (availableBytes == nil)
            availableBytes = values[NSURLVolumeAvailableCapacityKey];
    }
    if (availableBytes == nil) {
        NSDictionary<NSFileAttributeKey, id> *attributes =
            [NSFileManager.defaultManager attributesOfFileSystemForPath:destinationDirectory.path error:&resourceError];
        availableBytes = attributes[NSFileSystemFreeSize];
    }
    if (availableBytes == nil) {
        if (error != NULL)
            *error = resourceError;
        return NO;
    }

    if (availableBytes.longLongValue < requiredBytes) {
        if (error != NULL) {
            *error = RootsStorageError([NSString stringWithFormat:
                                        @"Not enough free space to extract the filesystem. About %@ is needed, but only %@ is available.",
                                        FormatByteCount(requiredBytes), FormatByteCount(availableBytes.longLongValue)],
                                       @"Free up storage space and try again.");
        }
        return NO;
    }
    return YES;
}

// Drives one bundled-archive download for DownloadBundledArchive(). Progress
// and cancellation are reported through the same id<ProgressReporter> that
// fakefs_import's root_progress_callback (below) reports import progress
// through, so the caller sees one continuous "Downloading..."/"Extracting..."
// progress sheet.
@interface RootsArchiveDownload : NSObject <NSURLSessionDownloadDelegate>
@property (nonatomic, weak) id<ProgressReporter> progress;
@property (nonatomic) NSURL *destinationURL;
@property (nonatomic) NSError *error;
@property (nonatomic) BOOL cancelled;
@property (nonatomic) dispatch_semaphore_t semaphore;
@end

@implementation RootsArchiveDownload

- (void)URLSession:(NSURLSession *)session
      downloadTask:(NSURLSessionDownloadTask *)downloadTask
      didWriteData:(int64_t)bytesWritten
 totalBytesWritten:(int64_t)totalBytesWritten
totalBytesExpectedToWrite:(int64_t)totalBytesExpectedToWrite {
    double fraction = totalBytesExpectedToWrite > 0
        ? (double) totalBytesWritten / (double) totalBytesExpectedToWrite : 0;
    NSString *message = totalBytesExpectedToWrite > 0
        ? [NSString stringWithFormat:@"Downloading… %@ of %@",
           FormatByteCount(totalBytesWritten), FormatByteCount(totalBytesExpectedToWrite)]
        : [NSString stringWithFormat:@"Downloading… %@", FormatByteCount(totalBytesWritten)];
    [self.progress updateProgress:fraction message:message];
    if ([self.progress shouldCancel]) {
        self.cancelled = YES;
        [downloadTask cancel];
    }
}

- (void)URLSession:(NSURLSession *)session
      downloadTask:(NSURLSessionDownloadTask *)downloadTask
didFinishDownloadingToURL:(NSURL *)location {
    NSHTTPURLResponse *response = (NSHTTPURLResponse *) downloadTask.response;
    if ([response isKindOfClass:NSHTTPURLResponse.class] && response.statusCode != 200) {
        self.error = [NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorBadServerResponse userInfo:@{
            NSLocalizedDescriptionKey: [NSString stringWithFormat:@"Server returned status %ld", (long) response.statusCode],
        }];
        return;
    }
    [NSFileManager.defaultManager removeItemAtURL:self.destinationURL error:nil];
    NSError *moveError = nil;
    if (![NSFileManager.defaultManager moveItemAtURL:location toURL:self.destinationURL error:&moveError])
        self.error = moveError;
}

- (void)URLSession:(NSURLSession *)session
              task:(NSURLSessionTask *)task
didCompleteWithError:(NSError *)error {
    if (error != nil && self.error == nil && !self.cancelled)
        self.error = error;
    dispatch_semaphore_signal(self.semaphore);
}

@end

// Synchronously downloads a bundled root's archive into /AOK/persist/roots.
// Runs on a background queue already (see importBundledRootChoice: callers),
// so blocking on a semaphore here is fine -- mirrors the rest of this file's
// synchronous archive-import style rather than threading async completion
// handlers through Roots' public API.
static BOOL DownloadBundledArchive(NSURL *url, NSURL *destination, id<ProgressReporter> progress, NSError **error) {
    NSError *dirError = nil;
    if (![NSFileManager.defaultManager createDirectoryAtURL:destination.URLByDeletingLastPathComponent
                                  withIntermediateDirectories:YES
                                                   attributes:nil
                                                        error:&dirError]) {
        if (error != NULL)
            *error = dirError;
        return NO;
    }

    RootsArchiveDownload *delegate = [RootsArchiveDownload new];
    delegate.progress = progress;
    delegate.destinationURL = destination;
    delegate.semaphore = dispatch_semaphore_create(0);

    NSURLSessionConfiguration *config = NSURLSessionConfiguration.ephemeralSessionConfiguration;
    NSURLSession *session = [NSURLSession sessionWithConfiguration:config delegate:delegate delegateQueue:nil];
    NSURLSessionDownloadTask *task = [session downloadTaskWithURL:url];
    [progress updateProgress:0 message:@"Downloading…"];
    [task resume];
    dispatch_semaphore_wait(delegate.semaphore, DISPATCH_TIME_FOREVER);
    [session finishTasksAndInvalidate];

    if (delegate.cancelled) {
        if (error != NULL)
            *error = nil;
        return NO;
    }
    if (delegate.error != nil) {
        if (error != NULL) {
            *error = [NSError errorWithDomain:@"iSH" code:0 userInfo:@{
                NSLocalizedDescriptionKey: @"Couldn't download the filesystem image.",
                NSLocalizedRecoverySuggestionErrorKey: @"Check your internet connection and try again.",
                NSUnderlyingErrorKey: delegate.error,
            }];
        }
        return NO;
    }
    return YES;
}

static void EnableCaseSensitiveFilesystemLookupsIfPossible(void) {
#ifdef __APPLE__
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        int err = setiopolicy_np(IOPOL_TYPE_VFS_HFS_CASE_SENSITIVITY,
                                 IOPOL_SCOPE_PROCESS,
                                 IOPOL_VFS_HFS_CASE_SENSITIVITY_FORCE_CASE_SENSITIVE);
        if (err != 0 && errno != EPERM) {
            NSLog(@"could not enable case-sensitive filesystem lookups: %s", strerror(errno));
        }
    });
#endif
}

@interface Roots ()
@property NSMutableOrderedSet<NSString *> *roots;
@property BOOL updatingDomains;
@property BOOL domainsNeedUpdate;
@property BOOL fileProviderDomainSyncEnabled;
@property NSUInteger fileProviderDomainSyncGeneration;
@property BOOL wantsVersionFile;
@property BOOL initialBundledRootImportInProgress;
@property (nullable) NSError *initialBundledRootImportError;
@end

// A root's name becomes a directory name and, once mounted, the filesystem
// "source" string that guest init scripts parse with whitespace-delimited
// tools (df, mount, ...). A space or shell/path metacharacter there breaks
// stock scripts (e.g. mountall.sh's df parsing) or allows path traversal, so
// restrict names to a conservative, path- and shell-safe character set.
static BOOL RootNameIsValid(NSString *name, NSError **error) {
    NSString *reason = nil;
    if (name.length == 0) {
        reason = @"Filesystem name can't be empty";
    } else if ([name hasPrefix:@"."]) {
        reason = @"Filesystem name can't start with '.'";
    } else {
        static NSCharacterSet *disallowed;
        static dispatch_once_t onceToken;
        dispatch_once(&onceToken, ^{
            disallowed = [[NSCharacterSet characterSetWithCharactersInString:
                @"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-"] invertedSet];
        });
        if ([name rangeOfCharacterFromSet:disallowed].location != NSNotFound)
            reason = @"Filesystem name can only contain letters, numbers, '.', '-', and '_' (no spaces)";
    }
    if (reason != nil) {
        if (error != NULL)
            *error = [NSError errorWithDomain:@"iSH" code:0 userInfo:@{NSLocalizedDescriptionKey: reason}];
        return NO;
    }
    return YES;
}

// Finds the bundled/manifest choice a root's name was imported from, matching
// by prefix to account for the "_2", "_3", ... dedup suffix
// importBundledRootChoice: appends when a name collides. Returns nil for
// roots that didn't come from a bundled choice (e.g. a user's own import).
static NSDictionary<NSString *, NSString *> *BundledRootChoiceMatchingName(NSString *name) {
    for (NSDictionary<NSString *, NSString *> *choice in BundledRootChoices()) {
        NSString *baseName = choice[kBundledRootImportNameKey];
        if (baseName.length == 0)
            continue;
        if ([name isEqualToString:baseName] ||
                [name hasPrefix:[baseName stringByAppendingString:@"_"]]) {
            return choice;
        }
    }
    return nil;
}

// Prefers a root whose bundled-choice origin isn't the experimental
// "community" tier (or a root with no bundled-choice match at all -- e.g. a
// user's own import) over one that is, when picking a fallback default with
// no valid stored preference to honor. contentsOfDirectoryAtPath: order is
// otherwise arbitrary (see -init), so without this an experimental
// community-tier root (e.g. a downloaded Arch Linux choice) installed
// alongside the official Alpine/Devuan roots could silently become the
// default with no user prompt.
static NSString *PreferredDefaultRootName(NSOrderedSet<NSString *> *roots) {
    for (NSString *name in roots) {
        NSDictionary<NSString *, NSString *> *choice = BundledRootChoiceMatchingName(name);
        if (![choice[kBundledRootTierKey] isEqualToString:kBundledRootTierCommunity])
            return name;
    }
    return roots.firstObject;
}

@implementation Roots

- (instancetype)init {
    if (self = [super init]) {
        EnableCaseSensitiveFilesystemLookupsIfPossible();
        NSMutableOrderedSet<NSString *> *roots = [NSMutableOrderedSet orderedSet];
        NSURL *rootsDir = RootsDir();
        if (rootsDir == nil) {
            NSLog(@"Roots: roots directory unavailable, starting with no roots");
        } else {
            NSError *error = nil;
            NSArray<NSString *> *rootNames = [NSFileManager.defaultManager contentsOfDirectoryAtPath:rootsDir.path error:&error];
            if (error != nil) {
                NSLog(@"Roots: couldn't list roots: %@", error);
            }
            for (NSString *rootName in rootNames) {
                if (RootURLLooksValid([self rootUrl:rootName])) {
                    [roots addObject:rootName];
                } else {
                    NSLog(@"ignoring invalid root entry %@", rootName);
                }
            }
        }
        self.roots = roots;
        self.fileProviderDomainSyncEnabled = NO;
        [self observe:@[@"roots"] options:0 owner:self usingBlock:^(typeof(self) self) {
            if (self.defaultRoot == nil && self.roots.count)
                self.defaultRoot = PreferredDefaultRootName(self.roots);
            [self requestFileProviderDomainSync];
        }];
        [self requestFileProviderDomainSync];

        if ((!self.defaultRoot || ![self.roots containsObject:self.defaultRoot]) && self.roots.count)
            self.defaultRoot = PreferredDefaultRootName(self.roots);
    }
    return self;
}

- (NSString *)defaultRoot {
    return [NSUserDefaults.standardUserDefaults stringForKey:kDefaultRoot];
}
- (void)setDefaultRoot:(NSString *)defaultRoot {
    [NSUserDefaults.standardUserDefaults setObject:defaultRoot forKey:kDefaultRoot];
}

- (BOOL)needsInitialRootSelection {
    return self.roots.count == 0;
}

- (NSArray<NSDictionary<NSString *,NSString *> *> *)bundledRootChoices {
    return BundledRootChoices();
}

- (BOOL)bundledRootChoiceNeedsDownload:(NSDictionary<NSString *, NSString *> *)choice {
    if (choice[kBundledRootDownloadURLKey].length == 0)
        return NO;
    if (BundledRootArchiveURL(choice[kBundledRootArchiveNameKey]) != nil)
        return NO;
    // importBundledRootChoice: always fetches fresh for a download-backed
    // choice now (see its comment), so this is "will download", full stop --
    // not "download only if not already cached".
    return YES;
}

- (NSURL *)rootUrl:(NSString *)name {
    return [RootsDir() URLByAppendingPathComponent:name];
}

- (nullable NSString *)guestABIForRootNamed:(NSString *)name {
    NSDictionary<NSString *, id> *metadata = ReadRootMetadata([self rootUrl:name]);
    NSString *guestABI = metadata[kRootMetadataGuestABIKey];
    if ([guestABI isKindOfClass:NSString.class] && guestABI.length != 0)
        return guestABI;

    NSString *choiceGuestABI = BundledRootChoiceMatchingName(name)[kBundledRootGuestABIKey];
    return choiceGuestABI.length != 0 ? choiceGuestABI : nil;
}

- (NSArray<NSURL *> *)cachedRootArchiveURLs {
    NSURL *cacheDir = PersistRootsDir();
    if (cacheDir == nil)
        return @[];
    NSArray<NSURL *> *entries = [NSFileManager.defaultManager
        contentsOfDirectoryAtURL:cacheDir
      includingPropertiesForKeys:@[NSURLIsRegularFileKey, NSURLFileSizeKey]
                         options:NSDirectoryEnumerationSkipsHiddenFiles
                           error:nil];

    static NSArray<NSString *> *suffixes;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        suffixes = @[@".tar", @".tar.gz", @".tgz", @".tar.bz2", @".tbz", @".tbz2",
                     @".tar.xz", @".txz", @".tar.zst", @".tzst", @".tar.lz", @".tar.lzma"];
    });

    NSMutableArray<NSURL *> *archives = [NSMutableArray array];
    for (NSURL *entry in entries) {
        NSNumber *isRegular = nil;
        [entry getResourceValue:&isRegular forKey:NSURLIsRegularFileKey error:nil];
        if (!isRegular.boolValue)
            continue;
        NSString *lower = entry.lastPathComponent.lowercaseString;
        for (NSString *suffix in suffixes) {
            if ([lower hasSuffix:suffix]) {
                [archives addObject:entry];
                break;
            }
        }
    }
    [archives sortUsingComparator:^NSComparisonResult(NSURL *a, NSURL *b) {
        return [a.lastPathComponent localizedStandardCompare:b.lastPathComponent];
    }];
    return archives;
}

- (void)syncFileProviderDomains {
    [self requestFileProviderDomainSync];
}

- (void)resumeDeferredFileProviderDomainSync {
    @synchronized (self) {
        if (self.fileProviderDomainSyncEnabled)
            return;
        self.fileProviderDomainSyncEnabled = YES;
        self.domainsNeedUpdate = YES;
    }
    [ISHDiagnosticsStore recordBreadcrumb:@"fileprovider.domainSync.resumed"];
    [self requestFileProviderDomainSync];
}

// The File Provider extension cannot run on a Mac, and registering a domain
// there does not fail politely -- it crashes.
//
// FileProviderExtension subclasses NSFileProviderExtension, whose availability
// macro in Apple's own SDK is
//
//     API_AVAILABLE(ios(8.0)) API_UNAVAILABLE(macos, macCatalyst)
//
// and the extension point is com.apple.fileprovider-nonui, the classic API.
// macOS hosts only the replicated API (NSFileProviderReplicatedExtension). So
// on an Apple Silicon Mac running this as a "Designed for iPad" app, the
// framework loads the extension, finds one it cannot host, and aborts it with
// __FILEPROVIDER_BAD_EXTENSION__ inside beginRequestWithDomain: -- before a
// single line of ours runs, which is why the crash reports carry no AOK frame.
//
// Nothing inside the extension can prevent that; the only lever is here, in the
// app: do not register a domain, and the extension is never asked to begin a
// request. Everything else about AOK works on a Mac -- it is the Files
// integration specifically that is unavailable.
static BOOL ISHFileProviderUnavailableOnThisPlatform(void) {
    if (@available(iOS 14.0, *)) {
        NSProcessInfo *info = NSProcessInfo.processInfo;
        return info.isiOSAppOnMac || info.isMacCatalystApp;
    }
    return NO;
}

// Drop a domain a previous run (or an earlier build) left registered, so a Mac
// -- or a re-signed build that has since lost its app group -- does not keep a
// stale one that Finder or Files would try to open.
- (void)removeAllFileProviderDomains {
    [NSFileProviderManager getDomainsWithCompletionHandler:^(NSArray<NSFileProviderDomain *> *domains, NSError *error) {
        if (error != nil || domains.count == 0)
            return;
        for (NSFileProviderDomain *domain in domains) {
            [NSFileProviderManager removeDomain:domain completionHandler:^(NSError *removeError) {
                if (removeError != nil)
                    NSLog(@"error removing file provider domain: %@", removeError);
            }];
        }
    }];
}

- (void)requestFileProviderDomainSync {
    if (ISHFileProviderUnavailableOnThisPlatform()) {
        [ISHDiagnosticsStore recordBreadcrumb:@"fileprovider.domainSync.unsupportedPlatform"
                                      details:@{@"reason": @"NSFileProviderExtension is unavailable on macOS"}];
        [self removeAllFileProviderDomains];
        return;
    }

    // Without an app group the app runs out of its own private container
    // (ContainerURL()), which the extension -- a separate process with a
    // separate private container -- cannot see. A domain registered here would
    // be an empty folder in Files whose every operation failed, so don't
    // register one. Everything else about AOK works in that state.
    if (!ContainerIsSharedAppGroup()) {
        [ISHDiagnosticsStore recordBreadcrumb:@"fileprovider.domainSync.noAppGroup"
                                      details:@{@"reason": @"no shared app group container; using the private fallback"}];
        [self removeAllFileProviderDomains];
        return;
    }

    NSArray<NSString *> *rootsSnapshot = nil;
    NSUInteger requestedGeneration = 0;
    @synchronized (self) {
        self.fileProviderDomainSyncGeneration++;
        requestedGeneration = self.fileProviderDomainSyncGeneration;
        if (!self.fileProviderDomainSyncEnabled) {
            self.domainsNeedUpdate = YES;
            [ISHDiagnosticsStore recordBreadcrumb:@"fileprovider.domainSync.deferred"
                                          details:@{@"roots": @(self.roots.count),
                                                    @"generation": @(requestedGeneration)}];
            return;
        }
        if (self.updatingDomains) {
            self.domainsNeedUpdate = YES;
            return;
        }
        self.updatingDomains = YES;
        self.domainsNeedUpdate = NO;
        rootsSnapshot = self.roots.array.copy;
    }

    [NSFileProviderManager getDomainsWithCompletionHandler:^(NSArray<NSFileProviderDomain *> *domains, NSError *error) {
        void (^onError)(NSError *error) = ^(NSError *error) {
            if (error != nil)
                NSLog(@"error adjusting domains: %@", error);
        };
        onError(error);
        NSLog(@"syncing the iSH-AOK file provider domain for %lu root(s)", (unsigned long) rootsSnapshot.count);

        // Tell Files to re-list the roots (the domain root container) whenever the
        // set of installed roots changes.
        void (^signalRoots)(NSFileProviderDomain *) = ^(NSFileProviderDomain *domain) {
            if (domain == nil)
                return;
            NSFileProviderManager *manager = [NSFileProviderManager managerForDomain:domain];
            [manager signalEnumeratorForContainerItemIdentifier:NSFileProviderRootContainerItemIdentifier
                                              completionHandler:^(NSError *signalError) {
                if (signalError != nil)
                    NSLog(@"error signaling root enumerator: %@", signalError);
            }];
        };

        // Keep exactly one domain ("iSH-AOK"); remove everything else, including
        // the legacy one-domain-per-root entries from older builds.
        NSFileProviderDomain *ourDomain = nil;
        for (NSFileProviderDomain *domain in domains) {
            if (ourDomain == nil && [domain.identifier isEqualToString:kISHFileProviderDomainIdentifier]) {
                ourDomain = domain;
                continue;
            }
            [NSFileManager.defaultManager removeItemAtURL:
             [NSFileProviderManager.defaultManager.documentStorageURL
              URLByAppendingPathComponent:domain.pathRelativeToDocumentStorage]
                                                    error:nil];
            [NSFileProviderManager removeDomain:domain completionHandler:onError];
        }

        if (ourDomain == nil) {
            NSFileProviderDomain *domain = [[NSFileProviderDomain alloc]
                       initWithIdentifier:kISHFileProviderDomainIdentifier
                              displayName:kISHFileProviderDomainIdentifier
            pathRelativeToDocumentStorage:kISHFileProviderDomainIdentifier];
            [NSFileProviderManager addDomain:domain completionHandler:^(NSError *addError) {
                onError(addError);
                if (addError == nil)
                    signalRoots(domain);
            }];
        } else {
            signalRoots(ourDomain);
        }

        BOOL shouldResync = NO;
        @synchronized (self) {
            shouldResync = self.domainsNeedUpdate ||
                self.fileProviderDomainSyncGeneration != requestedGeneration;
            self.updatingDomains = NO;
            self.domainsNeedUpdate = NO;
        }
        if (shouldResync)
            [self requestFileProviderDomainSync];
    }];
}

- (BOOL)accessInstanceVariablesDirectly {
    return YES;
}

- (BOOL)importBundledRootChoice:(NSString *)identifier error:(NSError **)error progressReporter:(id<ProgressReporter>)progress {
    NSDictionary<NSString *, NSString *> *selectedChoice = nil;
    for (NSDictionary<NSString *, NSString *> *choice in BundledRootChoices()) {
        if ([choice[kBundledRootIdentifierKey] isEqualToString:identifier]) {
            selectedChoice = choice;
            break;
        }
    }
    if (selectedChoice == nil) {
        if (error != NULL) {
            *error = [NSError errorWithDomain:@"iSH" code:0 userInfo:@{
                NSLocalizedDescriptionKey: @"Unknown bundled root choice"
            }];
        }
        return NO;
    }

    NSURL *archive = BundledRootArchiveURL(selectedChoice[kBundledRootArchiveNameKey]);
    if (archive == nil) {
        NSString *downloadURLString = selectedChoice[kBundledRootDownloadURLKey];
        NSURL *downloadDestination = DownloadedBundledArchiveURL(selectedChoice);
        if (downloadURLString.length != 0 && downloadDestination != nil) {
            // Always fetch fresh from the network here -- this is the explicit
            // "remote" choice (Official/Community Distributions), distinct from
            // the "Root Cached Filesystems" section that imports an on-disk
            // archive directly. Silently reusing a stale cached download from a
            // previous import made "remote" indistinguishable from "cached",
            // with no way to force a fresh fetch (e.g. to pick up an updated
            // archive, or replace a corrupted one) short of manually deleting
            // the file first. DownloadBundledArchive already removes any
            // existing file at the destination before moving the new one in.
            NSURL *downloadURL = [NSURL URLWithString:downloadURLString];
            if (downloadURL != nil &&
                DownloadBundledArchive(downloadURL, downloadDestination, progress, error)) {
                archive = downloadDestination;
            } else {
                return NO;
            }
        }
    }
    if (archive == nil) {
        if (error != NULL) {
            *error = [NSError errorWithDomain:@"iSH" code:0 userInfo:@{
                NSLocalizedDescriptionKey: @"Bundled root archive is missing"
            }];
        }
        return NO;
    }

    NSString *baseName = selectedChoice[kBundledRootImportNameKey];
    NSString *importName = baseName;
    unsigned suffix = 2;
    while ([self.roots containsObject:importName]) {
        // Use '_' (not a space) so the deduped name also passes RootNameIsValid.
        importName = [NSString stringWithFormat:@"%@_%u", baseName, suffix++];
    }

    BOOL ok = [self importRootFromArchive:archive
                                     name:importName
                                    error:error
                         progressReporter:progress];
    if (ok) {
        NSString *guestABI = selectedChoice[kBundledRootGuestABIKey];
        if (guestABI.length != 0) {
            WriteRootMetadata([self rootUrl:importName], @{
                kRootMetadataGuestABIKey: guestABI,
            });
        }
        _wantsVersionFile = YES;
    }
    return ok;
}

void root_progress_callback(void *cookie, double progress, const char *message, bool *should_cancel) {
    id <ProgressReporter> reporter = (__bridge id<ProgressReporter>) cookie;
    [reporter updateProgress:progress message:[NSString stringWithUTF8String:message]];
    if ([reporter shouldCancel])
        *should_cancel = true;
}

- (BOOL)importRootFromArchive:(NSURL *)archive name:(NSString *)name error:(NSError **)error progressReporter:(id<ProgressReporter> _Nullable)progress {
    EnableCaseSensitiveFilesystemLookupsIfPossible();
    if (!RootNameIsValid(name, error))
        return NO;
    NSAssert(![self.roots containsObject:name], @"root already exists: %@", name);
    struct fakefsify_error fs_err;
    NSURL *destination = [self rootUrl:name];
    // RootsDir() returns nil when the app group container is unavailable (a
    // missing or misconfigured App Group entitlement -- notably a build made
    // with CODE_SIGNING_ALLOWED=NO, which strips entitlements entirely). It
    // logs and returns gracefully, but a nil destination then flows into
    // -moveItemAtURL:toURL:options:error: below, which raises
    // NSInvalidArgumentException and aborts the whole app instead of surfacing
    // the "Import failed" alert this method's error return already drives.
    if (destination == nil) {
        if (error != NULL) {
            *error = [NSError errorWithDomain:@"iSH" code:0 userInfo:@{
                NSLocalizedDescriptionKey: @"No filesystem storage available "
                    @"(the app group container is missing -- check the App Group entitlement)"
            }];
        }
        return NO;
    }
    NSURL *temporaryDirectory = NSFileManager.defaultManager.temporaryDirectory;
    NSURL *tempDestination = [temporaryDirectory URLByAppendingPathComponent:[NSProcessInfo.processInfo globallyUniqueString]];
    if (tempDestination == nil)
        return NO;
    // libarchive (used by the preflight below and by fakefs_import) needs a UTF-8
    // LC_CTYPE to read Debian/Devuan tarballs whose entries carry UTF-8 link paths.
    fakefs_ensure_utf8_locale();
    NSError *spaceError = nil;
    if (!RootsCheckAvailableSpaceForArchive(archive, temporaryDirectory, &spaceError)) {
        if (error != NULL)
            *error = spaceError;
        [ISHDiagnosticsStore recordBreadcrumb:@"root.importPreflightFailed"
                                      details:@{@"name": name ?: @"",
                                                @"error": spaceError.localizedDescription ?: @"unknown"}];
        return NO;
    }
    if (!fakefs_import(archive.fileSystemRepresentation,
                       tempDestination.fileSystemRepresentation,
                       &fs_err, (struct progress) {(__bridge void *) progress, root_progress_callback})) {
        if (error != NULL) {
            *error = FakefsImportNSError(fs_err);
            if (fs_err.type == ERR_CANCELLED)
                *error = nil;
        }
        if (fs_err.type != ERR_CANCELLED) {
            NSError *reportedError = error != NULL ? *error : FakefsImportNSError(fs_err);
            [ISHDiagnosticsStore recordBreadcrumb:@"root.importFailed"
                                          details:@{@"name": name ?: @"",
                                                    @"error": reportedError.localizedDescription ?: @"unknown"}];
        }
        free(fs_err.message);
        [NSFileManager.defaultManager removeItemAtURL:tempDestination error:nil];
        return NO;
    }
    if (![NSFileManager.defaultManager moveItemAtURL:tempDestination toURL:destination error:error]) {
        NSError *reportedError = error != NULL ? *error : nil;
        [ISHDiagnosticsStore recordBreadcrumb:@"root.importMoveFailed"
                                      details:@{@"name": name ?: @"",
                                                @"error": reportedError.localizedDescription ?: @"unknown"}];
        [NSFileManager.defaultManager removeItemAtURL:tempDestination error:nil];
        return NO;
    }

    [self mutateRoots:^(NSMutableOrderedSet<NSString *> *roots) {
        [roots addObject:name];
    }];
    return YES;
}

// Every change to the installed set goes through here. The Filesystems screen
// and the file-provider domain sync observe "roots" through KVO and expect
// their callbacks on the main thread, but installs, renames and deletes all
// run on a background queue (and, via /proc/ish/roots, on the guest's own
// thread). Synchronous so the caller can read self.roots back immediately
// afterwards, which install does to work out what the root ended up named.
- (void)mutateRoots:(void (^)(NSMutableOrderedSet<NSString *> *roots))mutate {
    void (^apply)(void) = ^{
        mutate([self mutableOrderedSetValueForKey:@"roots"]);
    };
    if (!NSThread.isMainThread)
        dispatch_sync(dispatch_get_main_queue(), apply);
    else
        apply();
}

- (BOOL)exportRootNamed:(NSString *)name toArchive:(NSURL *)archive error:(NSError **)error progressReporter:(id<ProgressReporter> _Nullable)progress {
    NSAssert([self.roots containsObject:name], @"trying to export a root that doesn't exist: %@", name);
    struct fakefsify_error fs_err;
    if (!fakefs_export([self rootUrl:name].fileSystemRepresentation,
                       archive.fileSystemRepresentation,
                       &fs_err, (struct progress) {(__bridge void *) progress, root_progress_callback})) {
        // TODO: dedup with above method
        NSString *domain = NSPOSIXErrorDomain;
        if (fs_err.type == ERR_SQLITE)
            domain = @"SQLite";
        *error = [NSError errorWithDomain:domain
                                     code:fs_err.code
                                 userInfo:@{NSLocalizedDescriptionKey: [NSString stringWithUTF8String:fs_err.message]}];
        if (fs_err.type == ERR_CANCELLED)
            *error = nil;
        free(fs_err.message);
        return NO;
    }
    return YES;
}

// Every root other than the booted one is auto-exposed read-write at
// /AOK/roots/<name> (see AppDelegate's boot), and opt/AOK/tools/mount-root.sh
// can additionally bind /proc, /sys, /dev, /dev/pts, /run and /AOK/tools into
// it for a working chroot. Deleting or renaming that root's backing
// directory out from under a still-mounted (or actively chrooted-into) fake
// filesystem would corrupt or orphan it, so tear those mounts down first.
//
// This body was once guarded by `#if ISH_LINUX`, which was backwards --
// /AOK/roots only ever existed in the iSH kernel, not the retired Linux one --
// so in every shipping build the method compiled down to `return YES`: no
// unmount, and no busy check at all. A rename then moved a live fakefs's data/
// and meta.db out from under it while a chroot was still running on them, and
// a delete removed them outright. The guard is gone with that kernel; the
// history is kept because the failure it caused is not obvious from the code.
//
// Returns NO with *error set only if the root is genuinely busy (something
// still has an open fd/cwd/root inside it, e.g. an active chroot session --
// mount_detach reports this as EBUSY). Any other outcome (most commonly
// EINVAL: this root was never touched via mount-root.sh, so there's nothing
// mounted at all) is harmless and silently ignored -- mount_detach is safe to
// call unconditionally on a path that isn't currently mounted.
- (BOOL)unmountExposedRootNamed:(NSString *)name error:(NSError **)error {
    NSString *base = ExposedRootPoint(name);
    NSArray<NSString *> *binds = @[@"AOK/tools", @"run", @"dev/pts", @"dev", @"sys", @"proc"];
    for (NSString *bind in binds)
        mount_detach([base stringByAppendingPathComponent:bind].UTF8String);

    if (mount_detach(base.UTF8String) == _EBUSY) {
        if (error != NULL) {
            *error = [NSError errorWithDomain:@"iSH" code:0 userInfo:@{NSLocalizedDescriptionKey:
                @"This filesystem is currently mounted or in use (e.g. via mount-root.sh) -- exit any active session into it first"}];
        }
        return NO;
    }
    return YES;
}

// Mount an installed root read-write at /AOK/roots/<name>, exactly as boot
// does -- boot's per-root loop is now this method, so a root that appears
// after a rename is indistinguishable from one that was there at launch.
// Returns whether it ended up mounted; a root that can't be exposed (its
// backing store is gone, another process holds its lock, the mount itself
// fails) is a diagnosable non-event, not an error the caller must handle.
//
// Boot defers this to a background queue because fakefs's mount can run a
// migration/rebuild pass over the whole tree and iOS's launch watchdog was
// killing the app; the rename path calls it inline instead, because there the
// root was mounted moments earlier (so no migration is pending) and the guest
// must see the new name before the caller reports the rename as done.
- (BOOL)exposeRootNamed:(NSString *)name {
    // The booted root is already /, and /AOK/roots is deliberately "every
    // OTHER root": mounting it a second time would give the same fakefs two
    // live SQLite connections in one process.
    if (name.length == 0 || [name isEqualToString:self.bootedRoot])
        return NO;
    NSURL *exposureDir = ISHRootsExposureDirectoryURL();
    if (exposureDir == nil)
        return NO;

    NSURL *rootURL = [self rootUrl:name];
    NSURL *dataURL = [rootURL URLByAppendingPathComponent:@"data" isDirectory:YES];
    NSURL *metadataURL = [rootURL URLByAppendingPathComponent:@"meta.db" isDirectory:NO];
    BOOL isDirectory = NO;
    if (![NSFileManager.defaultManager fileExistsAtPath:dataURL.path isDirectory:&isDirectory] || !isDirectory)
        return NO;
    isDirectory = NO;
    if (![NSFileManager.defaultManager fileExistsAtPath:metadataURL.path isDirectory:&isDirectory] || isDirectory)
        return NO;

    NSURL *mountPointURL = [exposureDir URLByAppendingPathComponent:name isDirectory:YES];
    if (![NSFileManager.defaultManager createDirectoryAtURL:mountPointURL
                                 withIntermediateDirectories:YES
                                                  attributes:nil
                                                       error:nil])
        return NO;

    // Best-effort: a root whose lock is already held (the FileProvider
    // extension is mid-operation on it) is skipped rather than blocking, and
    // the lock is released as soon as this root's own mount is set up -- it
    // only needs to guard that, not the whole guest runtime.
    int lockFd = ISHAppGroupTryAcquireNamedLock(@"root", name, YES, nil);
    if (lockFd < 0) {
        [ISHDiagnosticsStore recordLaunchStage:@"boot.root.secondary.busy"
                                       details:@{@"root": name}];
        return NO;
    }
    NSString *mountPoint = ExposedRootPoint(name);
    int err = mount_attach(&fakefs, dataURL.fileSystemRepresentation, mountPoint.UTF8String, "", 0);
    ISHAppGroupReleaseLock(lockFd);
    if (err < 0) {
        NSLog(@"Could not mount secondary root %@ at %@: %d", name, mountPoint, err);
        return NO;
    }
    // Report the root's name rather than its backing path, for the same reason
    // 88496575 does it for / : the source here is an app group container path
    // whose only variable part is a UUID the guest can do nothing with, and it
    // is long enough to wrap every df row onto a second line. With several
    // roots installed that is most of the table. Display only -- mount->source
    // still opens the SQLite db.
    mount_set_display_source(mountPoint.UTF8String, name.UTF8String);
    [ISHDiagnosticsStore recordLaunchStage:@"boot.root.secondary.mounted"
                                   details:@{@"root": name, @"path": mountPoint}];
    return YES;
}

// The mkdir'd directory the mount above sits on. Once that mount is gone the
// directory is a stale, empty entry that `ls /AOK/roots` cannot tell apart
// from a real root -- the exact leftovers boot's prune pass exists to sweep up
// (see AppDelegate). Renaming or deleting a root is where they are created, so
// it is also where they should be removed, rather than leaving a phantom under
// the old name until the next launch.
- (void)removeExposedMountPointForRootNamed:(NSString *)name {
    NSURL *exposureDir = ISHRootsExposureDirectoryURL();
    if (exposureDir == nil || name.length == 0)
        return;
    NSURL *mountPointURL = [exposureDir URLByAppendingPathComponent:name isDirectory:YES];
    NSError *error = nil;
    if (![NSFileManager.defaultManager removeItemAtURL:mountPointURL error:&error] &&
            !RootsIsFileNotFoundError(error)) {
        NSLog(@"Could not remove stale root mount point %@: %@", name, error);
    }
}

// Renaming or deleting the root that is CURRENTLY BOOTED is not the same
// question as renaming the default root, and checking only the latter was a
// hole: defaultRoot is a plain user default that takes effect at the next
// launch, so choosing a different one and then acting on the running root
// walked straight past the guard and moved (or deleted) / out from under the
// live guest.
- (BOOL)rejectIfBootedOrDefault:(NSString *)name verb:(NSString *)verb error:(NSError **)error {
    NSString *reason = nil;
    if ([name isEqualToString:self.bootedRoot])
        reason = [NSString stringWithFormat:@"Cannot %@ the filesystem this session is booted from", verb];
    else if ([name isEqualToString:self.defaultRoot])
        reason = [NSString stringWithFormat:@"Cannot %@ the default filesystem", verb];
    if (reason == nil)
        return NO;
    if (error != NULL)
        *error = [NSError errorWithDomain:@"iSH" code:0 userInfo:@{NSLocalizedDescriptionKey: reason}];
    return YES;
}

// A name the caller believes is installed but isn't. Reachable from a stale
// Filesystems screen and from /proc/ish/roots (the guest can write a command
// naming anything), so it has to be an error rather than the NSAssert that
// used to stand here -- the app is built with assertions enabled.
static BOOL RootIsInstalled(Roots *roots, NSString *name, NSError **error) {
    if ([roots.roots containsObject:name])
        return YES;
    if (error != NULL) {
        *error = [NSError errorWithDomain:@"iSH" code:0 userInfo:@{NSLocalizedDescriptionKey:
            [NSString stringWithFormat:@"There is no filesystem named %@", name ?: @""]}];
    }
    return NO;
}

- (BOOL)destroyRootNamed:(NSString *)name error:(NSError **)error {
    if ([self rejectIfBootedOrDefault:name verb:@"delete" error:error])
        return NO;
    if (!RootIsInstalled(self, name, error))
        return NO;
    if (![self unmountExposedRootNamed:name error:error])
        return NO;
    if (![NSFileManager.defaultManager removeItemAtURL:[self rootUrl:name] error:error]) {
        // The store is still there, so put the guest's view of it back rather
        // than leaving a root that is installed but no longer reachable.
        [self exposeRootNamed:name];
        return NO;
    }
    [self removeExposedMountPointForRootNamed:name];
    [self mutateRoots:^(NSMutableOrderedSet<NSString *> *roots) {
        [roots removeObject:name];
    }];
    return YES;
}

- (BOOL)renameRoot:(NSString *)name toName:(NSString *)newName error:(NSError **)error {
    if (name.length == 0) {
        if (error != NULL)
            *error = [NSError errorWithDomain:@"iSH" code:0 userInfo:@{NSLocalizedDescriptionKey: @"Filesystem name can't be empty"}];
        return NO;
    }
    if ([self rejectIfBootedOrDefault:name verb:@"rename" error:error])
        return NO;
    if (!RootNameIsValid(newName, error))
        return NO;
    // Tapping out of the name field without changing anything fires the same
    // action as a real rename. That used to unmount the root and then fail the
    // move ("file exists"), leaving a working filesystem detached from the
    // guest with an "I couldn't rename it" alert on top.
    if ([newName isEqualToString:name])
        return YES;
    if (!RootIsInstalled(self, name, error))
        return NO;
    if ([self.roots containsObject:newName]) {
        if (error != NULL) {
            *error = [NSError errorWithDomain:@"iSH" code:0 userInfo:@{NSLocalizedDescriptionKey:
                [NSString stringWithFormat:@"There is already a filesystem named %@", newName]}];
        }
        return NO;
    }

    if (![self unmountExposedRootNamed:name error:error])
        return NO;
    if (![NSFileManager.defaultManager moveItemAtURL:[self rootUrl:name] toURL:[self rootUrl:newName] error:error]) {
        [self exposeRootNamed:name];
        return NO;
    }
    // Order matters: drop the old mount point before the new one goes up, so
    // `ls /AOK/roots` never shows the same filesystem under two names.
    [self removeExposedMountPointForRootNamed:name];
    [self mutateRoots:^(NSMutableOrderedSet<NSString *> *roots) {
        NSUInteger index = [roots indexOfObject:name];
        if (index != NSNotFound)
            [roots replaceObjectAtIndex:index withObject:newName];
    }];
    // Re-expose under the new name, so the rename is complete from the guest's
    // side too. Without this the root kept serving under its OLD path for the
    // rest of the session -- reading a data/ and meta.db that had been moved
    // away underneath it -- and the name the app now showed named nothing at
    // all, so there was no renamed entry to chroot into.
    [self exposeRootNamed:newName];
    return YES;
}

+ (instancetype)instance {
    static Roots *instance;
    static dispatch_once_t token;
    dispatch_once(&token, ^{
        instance = [Roots new];
    });
    return instance;
}

@end

#pragma mark - /proc/ish/roots

// The guest side of everything above: /AOK/tools/manage-roots.sh writes a
// command to /proc/ish/roots and reads the result back, so a root can be
// installed and chosen from a shell with no app UI in reach (over ssh, from a
// script, on a device whose screen you are not looking at).
//
// This lives in Roots.m rather than a file of its own so it can call the static
// helpers above -- DownloadBundledArchive in particular, which is the machinery
// the Filesystems screen already uses for a URL, retries and all.
//
// Everything here runs off the guest's thread. A write enqueues onto one serial
// queue and returns, because a download can take minutes and a guest `echo` that
// blocked for minutes would be indistinguishable from a hang. Progress comes
// back through the status text instead.

static NSString *const kRootsJobStateIdle = @"idle";
static NSString *const kRootsJobStateRunning = @"running";
static NSString *const kRootsJobStateDone = @"done";

@interface ISHRootsControl : NSObject <ProgressReporter>
@property (nonatomic) NSString *state;
@property (nonatomic) NSString *op;
@property (nonatomic) NSString *name;
@property (nonatomic) NSString *message;
@property (nonatomic) NSString *result;
@property (nonatomic) int percent;
@property (nonatomic) BOOL cancelRequested;
@end

@implementation ISHRootsControl {
    NSLock *_lock;
    NSMutableString *_pending;
}

+ (instancetype)shared {
    static ISHRootsControl *shared;
    static dispatch_once_t token;
    dispatch_once(&token, ^{ shared = [ISHRootsControl new]; });
    return shared;
}

+ (dispatch_queue_t)queue {
    static dispatch_queue_t queue;
    static dispatch_once_t token;
    dispatch_once(&token, ^{
        // Serial on purpose: two imports at once would race over the same roots
        // directory, and one job at a time is also what makes a single status
        // record enough to describe what is happening.
        queue = dispatch_queue_create("app.ish.roots-control", DISPATCH_QUEUE_SERIAL);
    });
    return queue;
}

- (instancetype)init {
    if ((self = [super init])) {
        _lock = [NSLock new];
        _pending = [NSMutableString string];
        _state = kRootsJobStateIdle;
        _op = @"";
        _name = @"";
        _message = @"";
        _result = @"";
    }
    return self;
}

- (void)beginOp:(NSString *)op name:(NSString *)name {
    [_lock lock];
    _state = kRootsJobStateRunning;
    _op = op;
    _name = name ?: @"";
    _message = @"starting";
    _result = @"";
    _percent = 0;
    _cancelRequested = NO;
    [_lock unlock];
}

// A catalog install does not know what the root will be called until it is
// done, and the front end reads this name back to act on it (install --default
// switches to what was just installed). Reporting the catalog id here means
// switching to a root name that does not exist.
- (void)renameJobTo:(NSString *)name {
    [_lock lock];
    _name = name ?: @"";
    [_lock unlock];
}

- (void)finishOk:(NSString *)message {
    [_lock lock];
    _state = kRootsJobStateDone;
    _result = @"ok";
    _message = message ?: @"";
    _percent = 100;
    [_lock unlock];
}

- (void)finishError:(NSString *)message {
    [_lock lock];
    _state = kRootsJobStateDone;
    _result = @"error";
    _message = message ?: @"failed";
    [_lock unlock];
}

// A synchronous rejection is reported the same way a failed job is, so the CLI
// has one place to look for what went wrong instead of two.
- (void)rejectOp:(NSString *)op message:(NSString *)message {
    [_lock lock];
    _state = kRootsJobStateDone;
    _op = op ?: @"";
    _name = @"";
    _result = @"error";
    _message = message ?: @"rejected";
    _percent = 0;
    [_lock unlock];
}

- (BOOL)isBusy {
    [_lock lock];
    BOOL busy = [_state isEqualToString:kRootsJobStateRunning];
    [_lock unlock];
    return busy;
}

- (void)requestCancel {
    [_lock lock];
    _cancelRequested = YES;
    [_lock unlock];
}

// A write is a FRAGMENT, not a command. The shell's printf issues one write(2)
// per line, so `printf 'op=default\nname=X\n' > /proc/ish/roots` arrives here as
// two separate writes, and a knob that treated each write as a whole command
// would see "op=default" and then "name=X" and reject both. (Piping through cat
// does deliver one write, which is why this was invisible at first -- and cat
// exits 0 even when the write failed, so it hid the rejections too.)
//
// So fragments accumulate until a line that is exactly `run` commits them. Any
// writer then works: one write or twenty, printf or cat or dd, or a person
// echoing lines into an open fd one at a time.
//
// Returns the complete command, or nil while there is more to come. A fragment
// beginning a new command resets whatever was pending, so an abandoned partial
// command cannot contaminate the next one.
- (NSString *)accumulate:(NSString *)fragment {
    [_lock lock];
    if ([fragment hasPrefix:@"op="] || _pending.length + fragment.length > 8192)
        [_pending setString:@""];
    [_pending appendString:fragment];
    NSString *complete = nil;
    NSRange run = [_pending rangeOfString:@"\nrun\n"];
    if (_pending.length >= 4 && [[_pending substringToIndex:4] isEqualToString:@"run\n"])
        run = NSMakeRange(0, 0);   // a command whose only content is `run`
    if (run.location != NSNotFound) {
        complete = [_pending substringToIndex:run.location];
        [_pending setString:@""];
    }
    [_lock unlock];
    return complete;
}

#pragma mark ProgressReporter

- (void)updateProgress:(double)progressFraction message:(NSString *)progressMessage {
    [_lock lock];
    if (progressFraction >= 0 && progressFraction <= 1)
        _percent = (int)(progressFraction * 100);
    if (progressMessage.length)
        _message = progressMessage;
    [_lock unlock];
}

- (BOOL)shouldCancel {
    [_lock lock];
    BOOL cancel = _cancelRequested;
    [_lock unlock];
    return cancel;
}

- (NSString *)statusText {
    Roots *roots = Roots.instance;
    NSString *defaultRoot = roots.defaultRoot ?: @"";
    NSMutableString *text = [NSMutableString string];
    // This runs on the guest's thread while the app may be reloading the set.
    // Holding a reference is enough: the reload builds a new ordered set and
    // assigns it (see the roots property), so it is swapped, never mutated
    // underneath an enumeration. Re-reading roots.roots mid-loop would not be.
    NSOrderedSet<NSString *> *installed = roots.roots;

    // One key=value per line, value to end of line. Anything free-form (a name,
    // a label, a message) therefore gets a line to itself rather than sharing
    // one with fields a reader has to find by position.
    // Strictly one key=value per line, including the ones that would fit
    // together. Packing several onto a line silently breaks any reader that
    // takes the value as "the rest of the line", which is the rule this format
    // promises: a `result=error` tucked onto the end of the state line is a
    // result nobody finds, and a failed job then reads as a successful one.
    // The last field of each record is its terminator, so a reader knows when
    // it has the whole thing.
    for (NSString *name in installed) {
        [text appendFormat:@"root abi=%@\n", [roots guestABIForRootNamed:name] ?: @"unknown"];
        [text appendFormat:@"root default=%d\n", [name isEqualToString:defaultRoot] ? 1 : 0];
        [text appendFormat:@"root name=%@\n", name];
    }
    for (NSDictionary<NSString *, NSString *> *choice in [roots bundledRootChoices]) {
        [text appendFormat:@"available id=%@\n", choice[kBundledRootIdentifierKey] ?: @""];
        [text appendFormat:@"available abi=%@\n", choice[kBundledRootGuestABIKey] ?: @"unknown"];
        [text appendFormat:@"available download=%d\n", [roots bundledRootChoiceNeedsDownload:choice] ? 1 : 0];
        [text appendFormat:@"available label=%@\n", choice[kBundledRootDisplayNameKey] ?: @""];
    }

    // Every job field is emitted even when empty, so a reader never has to
    // tell "no such key" apart from "key with an empty value".
    [_lock lock];
    [text appendFormat:@"job state=%@\n", _state];
    [text appendFormat:@"job op=%@\n", _op];
    [text appendFormat:@"job percent=%d\n", _percent];
    [text appendFormat:@"job result=%@\n", _result];
    [text appendFormat:@"job name=%@\n", _name];
    [text appendFormat:@"job message=%@\n", _message];
    [_lock unlock];

    [text appendFormat:@"default name=%@\n", defaultRoot];
    return text;
}

@end

// One key=value per line, value running to the end of the line. Repeating a key
// is an error rather than last-one-wins: two `name=` lines mean the caller built
// the command wrong, and picking one of them silently is how you install over
// the wrong root.
static NSDictionary<NSString *, NSString *> *ParseRootsCommand(NSString *text, NSString **problem) {
    NSMutableDictionary<NSString *, NSString *> *fields = [NSMutableDictionary dictionary];
    for (NSString *rawLine in [text componentsSeparatedByString:@"\n"]) {
        NSString *line = [rawLine stringByTrimmingCharactersInSet:
                          [NSCharacterSet characterSetWithCharactersInString:@"\r"]];
        if (line.length == 0)
            continue;
        NSRange equals = [line rangeOfString:@"="];
        if (equals.location == NSNotFound || equals.location == 0) {
            *problem = [NSString stringWithFormat:@"not a key=value line: %@", line];
            return nil;
        }
        NSString *key = [line substringToIndex:equals.location];
        NSString *value = [line substringFromIndex:equals.location + 1];
        if (fields[key] != nil) {
            *problem = [NSString stringWithFormat:@"%@ given twice", key];
            return nil;
        }
        fields[key] = value;
    }
    if (fields[@"op"] == nil) {
        *problem = @"no op= line";
        return nil;
    }
    return fields;
}

// What the user's "validate" step means in practice: the import said it worked,
// the root is really in the list, and there is something in it that could be
// executed. Not a boot test, which would need the live switch that is
// deliberately not part of this.
//
// The guest ABI is reported, never required. It is written into a root's
// metadata from the CATALOG entry it came from, so the app simply does not know
// it for an archive somebody brought themselves, and roots without it are
// ordinary: the stock "default" root has no ABI metadata either and boots fine.
// Requiring it failed every path and URL install, after the import had already
// succeeded.
static NSString *ValidateInstalledRoot(NSString *name, NSString **abiOut) {
    Roots *roots = Roots.instance;
    if (![roots.roots containsObject:name])
        return @"the import reported success but the root is not in the list";
    NSString *abi = [roots guestABIForRootNamed:name];
    NSURL *data = [[roots rootUrl:name] URLByAppendingPathComponent:@"data"];
    NSFileManager *fm = NSFileManager.defaultManager;
    BOOL executable = NO;
    for (NSString *probe in @[@"bin/sh", @"bin/busybox", @"sbin/init", @"usr/bin/sh"]) {
        if ([fm fileExistsAtPath:[data URLByAppendingPathComponent:probe].path]) {
            executable = YES;
            break;
        }
    }
    if (!executable)
        return @"the new root has no /bin/sh, /bin/busybox or /sbin/init; it is probably not a root filesystem";
    if (abiOut != NULL)
        *abiOut = abi.length ? abi : @"ABI not recorded";
    return nil;
}

// Turns a guest path into something the importer can open. /AOK/persist is a
// real-fs mount, so the bridge hands back the backing host file with no copy;
// anything else (a tarball sitting in a fakefs root) gets extracted to a temp
// file first, because fakefs escapes names on the host and the path a guest
// sees is not a path the host can open.
static NSURL *HostURLForGuestPath(NSString *guestPath, id<ProgressReporter> progress, NSString **problem) {
    __block NSURL *result = nil;
    __block NSError *failure = nil;
    __block ISHGuestFileExtractionToken token = nil;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    ISHGuestFileBridge *bridge = ISHGuestFileBridge.sharedBridge;
    if (![bridge isGuestAvailable]) {
        *problem = @"the guest filesystem is not reachable from the app right now";
        return nil;
    }
    token = [bridge extractToTempFileAtGuestPath:guestPath
        progress:^(int64_t written, int64_t total) {
            if (total > 0)
                [progress updateProgress:(double)written / (double)total message:@"reading the archive"];
            // The copy is the long part for a big archive, so cancel has to
            // reach it here; the completion then arrives with an error and the
            // wait below ends normally.
            if ([progress shouldCancel] && token != nil)
                [bridge cancelExtraction:token];
        }
        completion:^(NSURL *fileURL, NSError *error) {
            result = fileURL;
            failure = error;
            dispatch_semaphore_signal(done);
        }];
    // Safe to wait: this always runs on the roots-control queue, never on main,
    // and the bridge does its work on a lane of its own -- the completion that
    // signals us is dispatched to MAIN, which is why nothing may ever block main
    // on the roots-control queue. The timeout is a backstop rather than a policy
    // -- a completion that never arrives would wedge the one serial queue for
    // good, and every later command would answer EBUSY with nothing running to
    // explain it.
    if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(30 * 60 * NSEC_PER_SEC))) != 0) {
        if (token != nil)
            [bridge cancelExtraction:token];
        *problem = @"gave up waiting for the archive to be read out of the guest";
        return nil;
    }
    if (result == nil)
        *problem = failure.localizedDescription ?: [NSString stringWithFormat:@"could not read %@", guestPath];
    return result;
}

static void RunInstallJob(NSDictionary<NSString *, NSString *> *fields) {
    ISHRootsControl *control = ISHRootsControl.shared;
    Roots *roots = Roots.instance;
    NSString *source = fields[@"source"];
    NSString *name = fields[@"name"];
    NSError *error = nil;
    NSString *problem = nil;
    // What the root ends up being called is not always what was asked for: a
    // catalog install names the root itself, and appends _2, _3 ... when that
    // name is taken. The set difference is the only honest answer for every
    // source, so it is taken the same way for all of them.
    NSOrderedSet<NSString *> *before = [roots.roots copy];
    NSString *installed = nil;

    if ([source isEqualToString:@"catalog"]) {
        if (![roots importBundledRootChoice:fields[@"id"] error:&error progressReporter:control]) {
            [control finishError:error.localizedDescription ?: @"the install failed"];
            return;
        }
    } else {
        NSURL *archive = nil;
        if ([source isEqualToString:@"url"]) {
            NSURL *url = [NSURL URLWithString:fields[@"url"]];
            NSURL *dir = PersistRootsDir();
            if (url == nil || url.host == nil) {
                [control finishError:@"that is not a usable URL"];
                return;
            }
            if (dir == nil) {
                [control finishError:@"no shared roots directory to download into"];
                return;
            }
            [NSFileManager.defaultManager createDirectoryAtURL:dir withIntermediateDirectories:YES attributes:nil error:nil];
            // Downloaded into /AOK/persist/roots, the same place the Filesystems
            // screen caches archives, so a failed import can be retried without
            // fetching it again and the guest can see the file at that path.
            archive = [dir URLByAppendingPathComponent:url.lastPathComponent.length ? url.lastPathComponent : @"root.tar.gz"];
            if (!DownloadBundledArchive(url, archive, control, &error)) {
                [control finishError:error.localizedDescription ?: @"the download failed"];
                return;
            }
        } else {
            archive = HostURLForGuestPath(fields[@"path"], control, &problem);
            if (archive == nil) {
                [control finishError:problem];
                return;
            }
        }
        [control updateProgress:0 message:@"importing"];
        if (![roots importRootFromArchive:archive name:name error:&error progressReporter:control]) {
            [control finishError:error.localizedDescription ?: @"the import failed"];
            return;
        }
    }

    for (NSString *candidate in roots.roots) {
        if (![before containsObject:candidate]) {
            installed = candidate;
            break;
        }
    }
    // A path or URL install asked for a specific name, so if the list has not
    // caught up yet, the name that was asked for is still the right answer.
    if (installed == nil && name.length != 0 && [roots.roots containsObject:name])
        installed = name;
    if (installed == nil) {
        [control finishError:@"the install reported success but added no root"];
        return;
    }

    [control renameJobTo:installed];

    NSString *abi = nil;
    problem = ValidateInstalledRoot(installed, &abi);
    if (problem != nil) {
        // Take back what this job created rather than reporting an error and
        // leaving the thing behind anyway: a root that failed its check is one
        // the archive never justified, and a failed install that silently adds
        // an entry to the Filesystems screen is worse than either outcome on
        // its own. Only ever removes a root installed moments ago by this job.
        NSError *cleanup = nil;
        if ([roots destroyRootNamed:installed error:&cleanup])
            problem = [problem stringByAppendingString:@"; the partial root was removed"];
        else
            problem = [problem stringByAppendingFormat:@"; %@ is still installed and may be unusable", installed];
        [control finishError:problem];
        return;
    }
    [control finishOk:[NSString stringWithFormat:@"installed %@ (%@)", installed, abi]];
}

static void RunExitApp(void) {
    dispatch_async(dispatch_get_main_queue(), ^{
        // Never a bare exit(): the fakefs holds SQLite state that has to be let
        // go of first, which is the whole reason the suspend guard exists
        // (RUNNINGBOARD 0xdead10cc). Same path a real suspension takes.
        ISHSuspendGuardEnterBackground();
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.4 * NSEC_PER_SEC)),
                       dispatch_get_main_queue(), ^{ exit(0); });
    });
}

static int ISHRootsCommandImpl(const char *fragment) {
    ISHRootsControl *control = ISHRootsControl.shared;
    NSString *chunk = fragment != NULL ? @(fragment) : nil;
    if (chunk == nil) {
        [control rejectOp:@"" message:@"the command was not valid UTF-8"];
        return _EINVAL;
    }
    // Nothing happens until `run` arrives; see -accumulate: for why a write is
    // not a command.
    NSString *text = [control accumulate:chunk];
    if (text == nil)
        return 0;

    NSString *problem = nil;
    NSDictionary<NSString *, NSString *> *fields = ParseRootsCommand(text, &problem);
    if (fields == nil) {
        [control rejectOp:@"" message:problem];
        return _EINVAL;
    }
    NSString *op = fields[@"op"];
    Roots *roots = Roots.instance;

    // Everything that can be rejected is rejected here, on the caller's thread,
    // because a write(2) that returns 0 and then fails silently in the
    // background is the worst of both worlds.
    if ([op isEqualToString:@"cancel"]) {
        [control requestCancel];
        return 0;
    }
    if ([op isEqualToString:@"exit-app"]) {
        RunExitApp();
        return 0;
    }
    if ([op isEqualToString:@"default"]) {
        NSString *name = fields[@"name"];
        if (![roots.roots containsObject:name]) {
            [control rejectOp:op message:[NSString stringWithFormat:@"no such root: %@", name ?: @""]];
            return _ENOENT;
        }
        // Instant, but it still claims the job slot: overwriting a running
        // install's status with "chosen" would lose the only progress report
        // there is.
        if ([control isBusy])
            return _EBUSY;
        // The property is plain NSUserDefaults, but the Filesystems screen
        // observes it, so the write goes where its observers expect to be
        // called: this one arrives on a guest thread.
        dispatch_async(dispatch_get_main_queue(), ^{ roots.defaultRoot = name; });
        [control beginOp:op name:name];
        [control finishOk:@"chosen; it boots at the next app launch"];
        return 0;
    }
    if ([op isEqualToString:@"rename"] || [op isEqualToString:@"remove"]) {
        NSString *name = fields[@"name"];
        if (![roots.roots containsObject:name]) {
            [control rejectOp:op message:[NSString stringWithFormat:@"no such root: %@", name ?: @""]];
            return _ENOENT;
        }
        if ([op isEqualToString:@"remove"] && ![fields[@"confirm"] isEqualToString:@"yes"]) {
            [control rejectOp:op message:@"removing a root needs confirm=yes"];
            return _EINVAL;
        }
        NSError *nameError = nil;
        if ([op isEqualToString:@"rename"] && !RootNameIsValid(fields[@"to"], &nameError)) {
            [control rejectOp:op message:nameError.localizedDescription ?: @"that is not a usable root name"];
            return _EINVAL;
        }
        if ([control isBusy])
            return _EBUSY;
        [control beginOp:op name:name];
        dispatch_async(ISHRootsControl.queue, ^{
            NSError *error = nil;
            BOOL ok;
            if ([op isEqualToString:@"remove"])
                ok = [roots destroyRootNamed:name error:&error];
            else
                ok = [roots renameRoot:name toName:fields[@"to"] error:&error];
            if (ok)
                [control finishOk:[op isEqualToString:@"remove"] ? @"removed" : @"renamed"];
            else
                [control finishError:error.localizedDescription ?: @"failed"];
        });
        return 0;
    }
    if ([op isEqualToString:@"install"]) {
        NSString *source = fields[@"source"];
        if ([source isEqualToString:@"catalog"]) {
            NSString *identifier = fields[@"id"];
            BOOL known = NO;
            for (NSDictionary<NSString *, NSString *> *choice in [roots bundledRootChoices]) {
                if ([choice[kBundledRootIdentifierKey] isEqualToString:identifier]) {
                    known = YES;
                    break;
                }
            }
            if (!known) {
                [control rejectOp:op message:[NSString stringWithFormat:@"no such catalog id: %@", identifier ?: @""]];
                return _ENOENT;
            }
            if (fields[@"name"] != nil) {
                [control rejectOp:op message:@"a catalog install names the root itself; drop name="];
                return _EINVAL;
            }
        } else if ([source isEqualToString:@"path"] || [source isEqualToString:@"url"]) {
            NSString *name = fields[@"name"];
            if (name.length == 0) {
                [control rejectOp:op message:@"installing from a path or URL needs name="];
                return _EINVAL;
            }
            // The importer's own rule, not a second one written here: a name
            // this rejects would fail later anyway, and two spellings of
            // "valid name" is how they drift apart.
            NSError *nameError = nil;
            if (!RootNameIsValid(name, &nameError)) {
                [control rejectOp:op message:nameError.localizedDescription ?: @"that is not a usable root name"];
                return _EINVAL;
            }
            if ([roots.roots containsObject:name]) {
                [control rejectOp:op message:[NSString stringWithFormat:@"a root named %@ is already installed", name]];
                return _EEXIST;
            }
            if (fields[[source isEqualToString:@"path"] ? @"path" : @"url"].length == 0) {
                [control rejectOp:op message:@"missing path= or url="];
                return _EINVAL;
            }
        } else {
            [control rejectOp:op message:@"source must be catalog, path or url"];
            return _EINVAL;
        }
        if ([control isBusy])
            return _EBUSY;
        [control beginOp:op name:fields[@"name"] ?: fields[@"id"]];
        dispatch_async(ISHRootsControl.queue, ^{ RunInstallJob(fields); });
        return 0;
    }

    [control rejectOp:op message:[NSString stringWithFormat:@"unknown op: %@", op]];
    return _EINVAL;
}

static char *ISHRootsStatusImpl(void) {
    @autoreleasepool {
        const char *text = [ISHRootsControl.shared statusText].UTF8String;
        return text != NULL ? strdup(text) : NULL;
    }
}

@interface ISHRootsControlInstaller : NSObject
@end
@implementation ISHRootsControlInstaller
// +load rather than any app startup hook: the guest can read /proc/ish/roots
// the moment it is running, and a NULL pointer there would report the knob as
// unavailable rather than merely early.
+ (void)load {
    ish_roots_status = ISHRootsStatusImpl;
    ish_roots_command = ISHRootsCommandImpl;
}
@end
