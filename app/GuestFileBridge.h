//
//  GuestFileBridge.h
//  iSH-AOK
//
//  Shared async bridge from native UIKit applets (file manager, viewers, and in
//  time Music/MotePad) to the guest filesystem. Replaces the near-identical
//  private copies of "borrow pid 1 as `current`, call generic_open/readdir"
//  that grew up independently in AudioLibrary.m, MotePadDocumentStore.m, and
//  the FileProvider extension. Kept in its own translation unit so the kernel
//  C headers never mix into the big UIKit WorkspaceViewController.m (same
//  rule MotePadDocumentStore follows).
//
//  Every operation runs on a private serial queue and completes on main.
//  Guest-VFS calls block on emulator locks (fakefs SQLite, inodes_lock), so
//  callers must always use the async API here -- never call the emulator's
//  fs/ C API directly from UI code.
//
//  Paths under /AOK/persist are realfs-backed (a real host directory via the
//  AppGroup container): those are read/written directly with no VFS borrow
//  and no copy. Everything else lives in the per-root fakefs and is reached
//  through the emulated VFS under a borrowed task context.
//
//  Directory listings are returned in whatever order the filesystem yields
//  them (stably re-sorted by name for determinism only) -- sort-by-size/
//  date/kind is a presentation concern for the file manager UI, not this
//  bridge.
//

#import <Foundation/Foundation.h>
#import <sys/types.h>

NS_ASSUME_NONNULL_BEGIN

// Domain for every NSError this bridge produces. Negative codes are guest
// errno values (as returned by generic_open/generic_* via PTR_ERR, negated
// Linux-style, e.g. -2 = ENOENT) with a strerror()-derived message. Positive
// codes are bridge-local conditions from ISHGuestFileBridgeErrorCode below.
extern NSString *const ISHGuestFileErrorDomain;

typedef NS_ENUM(NSInteger, ISHGuestFileBridgeErrorCode) {
    ISHGuestFileBridgeErrorUnknown = 0,
    ISHGuestFileBridgeErrorNotReady,          // pid 1 / guest not booted yet
    ISHGuestFileBridgeErrorCancelled,
    ISHGuestFileBridgeErrorNotRegularFile,
    ISHGuestFileBridgeErrorIsDirectory,
    ISHGuestFileBridgeErrorNotDirectory,
    ISHGuestFileBridgeErrorTooLarge,
    ISHGuestFileBridgeErrorUnsupported,       // e.g. recursive cross-filesystem copy
};

typedef NS_ENUM(NSInteger, ISHGuestFileKind) {
    ISHGuestFileKindRegular,
    ISHGuestFileKindDirectory,
    ISHGuestFileKindSymlink,   // only reported when the link is broken; see -isSymlink
    ISHGuestFileKindFIFO,
    ISHGuestFileKindSocket,
    ISHGuestFileKindCharDevice,
    ISHGuestFileKindBlockDevice,
    ISHGuestFileKindOther,
};

// One filesystem entry. For a symlink, `kind` reflects the *target's* type
// (so a symlinked folder shows as a folder, matching Finder) unless the link
// is broken, in which case `kind` is ISHGuestFileKindSymlink. Check
// `symlinkTarget` (non-nil) to know an entry is itself a symlink at all, and
// `isBrokenSymlink` for the broken case specifically.
@interface ISHGuestFileItem : NSObject
@property (nonatomic, copy) NSString *name;                 // last path component
@property (nonatomic, copy) NSString *guestPath;             // absolute guest path
@property (nonatomic) ISHGuestFileKind kind;
@property (nonatomic) unsigned long long size;
@property (nonatomic, nullable) NSDate *modificationDate;
@property (nonatomic) mode_t posixMode;                      // full mode incl. type bits
@property (nonatomic) uid_t uid;
@property (nonatomic) gid_t gid;
@property (nonatomic, copy, nullable) NSString *symlinkTarget;  // raw readlink() text
// Set only when guestPath resolves under a realfs mount (currently
// /AOK/persist): the host file IS the backing store, reachable directly with
// no VFS round-trip. For a resolved symlink this points at the *target's*
// host location; nil for a broken symlink.
@property (nonatomic, nullable) NSURL *realfsURL;

@property (nonatomic, readonly) BOOL isSymlink;
@property (nonatomic, readonly) BOOL isBrokenSymlink;
@end

// Opaque handle for a cancelable extraction; pass to -cancelExtraction:.
typedef NSUUID *ISHGuestFileExtractionToken;

@interface ISHGuestFileBridge : NSObject

+ (instancetype)sharedBridge;

// The serial queue every guest-VFS operation runs on. Exposed so callers can
// order their own follow-up work behind pending bridge operations (same
// pattern as MotePadDocumentStore.ioQueue).
@property (nonatomic, readonly) dispatch_queue_t ioQueue;

// True if a guest task is currently available to borrow (pid 1 booted with a
// usable mm/mem/files/fs). Safe to call from any thread. This is a hint for
// empty-state UI, not a lock -- the guest can boot, or the active root can
// switch, between this check and the next operation.
- (BOOL)isGuestAvailable;

#pragma mark Directory / metadata

- (void)listDirectoryAtGuestPath:(NSString *)guestPath
                       completion:(void (^)(NSArray<ISHGuestFileItem *> * _Nullable items,
                                             NSError * _Nullable error))completion;

- (void)statAtGuestPath:(NSString *)guestPath
              completion:(void (^)(ISHGuestFileItem * _Nullable item,
                                    NSError * _Nullable error))completion;

// Free/total space of the filesystem containing guestPath (statfs). Virtual
// filesystems that report no numbers (proc, devpts) come back as 0/0 with no
// error -- display should treat 0 total as "no free-space figure", not "full".
- (void)filesystemStatusAtGuestPath:(NSString *)guestPath
                          completion:(void (^)(int64_t availableBytes, int64_t totalBytes,
                                                NSError * _Nullable error))completion;

#pragma mark Reading

// Whole-file read capped at maxBytes; a file larger than the cap fails with
// ISHGuestFileBridgeErrorTooLarge rather than silently truncating. Callers
// must always pass a real cap -- there is no unbounded-read entry point.
- (void)readFileAtGuestPath:(NSString *)guestPath
                    maxBytes:(NSUInteger)maxBytes
                  completion:(void (^)(NSData * _Nullable data,
                                        NSError * _Nullable error))completion;

// Copies a guest file to a private temp file that a native framework
// (AVFoundation, ImageIO, ...) can open directly, in ~1 MiB chunks, with
// progress and cancellation. Realfs-backed paths (/AOK/persist) return the
// backing host URL immediately with no copy. Results are cached by
// (guestPath, size, modificationDate); a second call for an unchanged file
// returns from cache without touching the VFS. Cached temp files are only
// reclaimed by clearExtractionCache (call it at app launch) -- deliberately
// not evicted while the app runs, because a consumer (AVPlayer especially)
// may hold the URL and reopen the file at any time during playback.
- (ISHGuestFileExtractionToken)extractToTempFileAtGuestPath:(NSString *)guestPath
    progress:(nullable void (^)(int64_t bytesWritten, int64_t totalBytes))progress
    completion:(void (^)(NSURL * _Nullable fileURL, NSError * _Nullable error))completion;

- (void)cancelExtraction:(ISHGuestFileExtractionToken)token;

// Drops every cached extraction and deletes its backing temp files. Safe to
// call at app launch to reclaim space left by a previous run.
- (void)clearExtractionCache;

#pragma mark Writing

// Atomic write: temp file + rename on the VFS, NSDataWritingAtomic on realfs.
// Writing through a symlink updates the target rather than replacing the
// link itself. Refuses to clobber a non-regular file (FIFO, socket, device).
- (void)writeData:(NSData *)data
       toGuestPath:(NSString *)guestPath
        completion:(void (^)(BOOL ok, NSError * _Nullable error))completion;

- (void)createDirectoryAtGuestPath:(NSString *)guestPath
                         completion:(void (^)(BOOL ok, NSError * _Nullable error))completion;

// Renames/moves within one backing store; copies-then-deletes across a
// realfs/fakefs boundary (matches real EXDEV — no single rename spans two
// filesystems). The cross-filesystem fallback supports regular files only;
// moving a directory across the realfs/fakefs boundary is not yet supported.
- (void)moveItemAtGuestPath:(NSString *)sourcePath
                toGuestPath:(NSString *)destinationPath
                 completion:(void (^)(BOOL ok, NSError * _Nullable error))completion;

// Regular files only in v1 -- recursive directory copy is not yet implemented.
- (void)copyItemAtGuestPath:(NSString *)sourcePath
                toGuestPath:(NSString *)destinationPath
                 completion:(void (^)(BOOL ok, NSError * _Nullable error))completion;

// Deletes a file, empty directory, or (recursive:YES) a whole directory
// tree. Never follows symlinks while recursing -- a symlinked directory
// inside the tree is unlinked as itself, never traversed into.
- (void)removeItemAtGuestPath:(NSString *)guestPath
                     recursive:(BOOL)recursive
                    completion:(void (^)(BOOL ok, NSError * _Nullable error))completion;


#pragma mark Terminal integration

// Best-effort working directory of the process group in the foreground of the
// tty identified by (type, number) -- "where the shell is standing right now".
// Used by the terminal's file browser to open on the shell's own directory
// instead of a fixed root.
//
// Completes with nil when the guest has not booted, the tty has no foreground
// group yet, or the path cannot be resolved. That is a normal state, not a
// failure, so this reports no NSError: a nil result simply means the caller
// should fall back to its own default.
//
// Note this follows the FOREGROUND group, so while a job is running it is that
// job's directory, not the shell's. At a prompt -- when a user actually opens
// a file browser -- the foreground group is the shell.
- (void)foregroundDirectoryForTTYType:(int)type
                                number:(int)number
                            completion:(void (^)(NSString * _Nullable guestPath))completion;

@end

NS_ASSUME_NONNULL_END
