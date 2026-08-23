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
//  There are TWO such queues, an interactive lane and a bulk lane, because one
//  queue for everything meant a 300 MB extraction sat in front of a twelve-
//  entry readdir and froze the file browsers. Each entry point below says which
//  lane it uses. They are both SERIAL, deliberately: AOK's fakefs metadata
//  mutex saturates under a single thread already, and parallel metadata work
//  measures slower, so this separates latency classes rather than chasing
//  parallelism. See docs/guest_file_bridge_lanes.md.
//
//  ORDERING. Operations that reach the same filesystem state keep the order they
//  were enqueued in, whichever lanes they land on; the bridge enforces that
//  itself. "The same state" means the same path, a path and the directory it
//  lives in (so a listing of D is ordered against a write at D/name), or -- for
//  a recursive delete alone -- a subtree and anything inside it. Operations on
//  unrelated paths have no ordering relationship and never did.
//
//  What every caller in the tree actually relies on is stronger than either: an
//  operation's completion runs after its effects are visible, so write-then-
//  reload works by chaining the reload inside the write's completion block,
//  which is what they all do.
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

// Opaque handle for a cancelable operation; pass to -cancelOperation:.
// Cancelling completes the operation with ISHGuestFileBridgeErrorCancelled --
// the completion always runs, so a caller can rely on it to release state.
typedef NSUUID *ISHGuestFileOperationToken;
// The original name, from when only extraction was cancelable. Kept so
// extraction call sites keep reading as extraction.
typedef ISHGuestFileOperationToken ISHGuestFileExtractionToken;

@interface ISHGuestFileBridge : NSObject

+ (instancetype)sharedBridge;

// (There is deliberately no exposed queue. The bridge used to publish its one
// serial queue so callers could order follow-up work behind pending operations;
// nothing ever did, and with two lanes there is no single queue to publish. Chain
// through a completion block instead -- see ORDERING at the top of this file.)

// True if a guest task is currently available to borrow (pid 1 booted with a
// usable mm/mem/files/fs). Safe to call from any thread. This is a hint for
// empty-state UI, not a lock -- the guest can boot, or the active root can
// switch, between this check and the next operation.
- (BOOL)isGuestAvailable;

#pragma mark Directory / metadata

// Interactive lane. Returns a token: a listing of a big directory is seconds of
// readdir + per-entry stat, and a caller that navigates away or gets dismissed
// should stop the WORK, not just discard the answer. Ignore the token and
// nothing changes.
- (ISHGuestFileOperationToken)listDirectoryAtGuestPath:(NSString *)guestPath
                       completion:(void (^)(NSArray<ISHGuestFileItem *> * _Nullable items,
                                             NSError * _Nullable error))completion;

// Interactive lane.
- (void)statAtGuestPath:(NSString *)guestPath
              completion:(void (^)(ISHGuestFileItem * _Nullable item,
                                    NSError * _Nullable error))completion;

// Interactive lane. Free/total space of the filesystem containing guestPath
// (statfs). Virtual
// filesystems that report no numbers (proc, devpts) come back as 0/0 with no
// error -- display should treat 0 total as "no free-space figure", not "full".
- (void)filesystemStatusAtGuestPath:(NSString *)guestPath
                          completion:(void (^)(int64_t availableBytes, int64_t totalBytes,
                                                NSError * _Nullable error))completion;

#pragma mark Reading

// Whole-file read capped at maxBytes; a file larger than the cap fails with
// ISHGuestFileBridgeErrorTooLarge rather than silently truncating. Callers
// must always pass a real cap -- there is no unbounded-read entry point.
//
// The cap also picks the lane, because choosing it is the caller declaring how
// much work it is asking for: up to 8 MiB is interactive, above that is bulk.
// The viewers reading a document (8 KiB preview, 64 KiB /etc/passwd, 4 MiB
// markdown) stay interactive; the image viewer's 64 MiB really is a transfer.
- (ISHGuestFileOperationToken)readFileAtGuestPath:(NSString *)guestPath
                    maxBytes:(NSUInteger)maxBytes
                  completion:(void (^)(NSData * _Nullable data,
                                        NSError * _Nullable error))completion;

// Split across lanes. The cheap part -- the stat, the realfs fast path and the
// cache lookup, two of which answer without touching the VFS at all -- runs on
// the interactive lane, so sharing an already-extracted image stays instant
// while a video is still copying; only the chunked copy itself goes bulk.
//
// Copies a guest file to a private temp file that a native framework
// (AVFoundation, ImageIO, ...) can open directly, in ~1 MiB chunks, with
// progress and cancellation. Realfs-backed paths (/AOK/persist) return the
// backing host URL immediately with no copy. Results are cached by
// (guestPath, size, modificationDate); a second call for an unchanged file
// returns from cache without touching the VFS. Cached temp files are only
// reclaimed by clearExtractionCache, which runs at app launch -- deliberately
// not evicted while the app runs, because a consumer (AVPlayer especially)
// may hold the URL and reopen the file at any time during playback.
- (ISHGuestFileExtractionToken)extractToTempFileAtGuestPath:(NSString *)guestPath
    progress:(nullable void (^)(int64_t bytesWritten, int64_t totalBytes))progress
    completion:(void (^)(NSURL * _Nullable fileURL, NSError * _Nullable error))completion;

// Cancels any operation that returned a token. Cancelling something already
// finished, or a token from an operation that does not poll for cancellation,
// is a no-op rather than an error.
//
// Polled by: directory listing (both the readdir loop and the per-entry stat
// loop), whole-file read, extraction, and the cross-store streaming copy. NOT
// by recursive delete -- stopping a tree walk halfway leaves a half-deleted
// tree and no way to say what survived, so a delete that has begun finishes.
- (void)cancelOperation:(ISHGuestFileOperationToken)token;
- (void)cancelExtraction:(ISHGuestFileExtractionToken)token;  // synonym for -cancelOperation:

// Bulk lane. Drops every cached extraction and deletes its backing temp files.
// Called at app launch, from -application:didFinishLaunchingWithOptions:, to
// reclaim the space left by a previous run -- nothing else calls it, and
// nothing else should: see the extraction comment above for why entries are
// never evicted while the app is running.
- (void)clearExtractionCache;

#pragma mark Writing

// Interactive lane for up to 8 MiB of data, bulk above that (the same rule the
// read cap follows). Atomic write: temp file + rename on the VFS, NSDataWritingAtomic on realfs.
// Writing through a symlink updates the target rather than replacing the
// link itself. Refuses to clobber a non-regular file (FIFO, socket, device).
- (void)writeData:(NSData *)data
       toGuestPath:(NSString *)guestPath
        completion:(void (^)(BOOL ok, NSError * _Nullable error))completion;

// Interactive lane.
- (void)createDirectoryAtGuestPath:(NSString *)guestPath
                         completion:(void (^)(BOOL ok, NSError * _Nullable error))completion;

// Interactive lane within one backing store (it is one rename), bulk across the
// realfs/fakefs boundary (it is a whole-file copy). Which one it is falls out of
// the two paths alone, with no VFS call, so the lane is settled before enqueue.
//
// Renames/moves within one backing store; copies-then-deletes across a
// realfs/fakefs boundary (matches real EXDEV — no single rename spans two
// filesystems). The cross-filesystem fallback supports regular files only;
// moving a directory across the realfs/fakefs boundary is not yet supported.
- (ISHGuestFileOperationToken)moveItemAtGuestPath:(NSString *)sourcePath
                toGuestPath:(NSString *)destinationPath
                 completion:(void (^)(BOOL ok, NSError * _Nullable error))completion;

// Bulk lane -- a copy is O(file size) however small this particular file is.
// Regular files only in v1 -- recursive directory copy is not yet implemented.
- (ISHGuestFileOperationToken)copyItemAtGuestPath:(NSString *)sourcePath
                toGuestPath:(NSString *)destinationPath
                 completion:(void (^)(BOOL ok, NSError * _Nullable error))completion;

// Interactive lane when recursive:NO (one unlink/rmdir), bulk when recursive:YES
// (an unbounded tree walk), even where the tree turns out to be empty.
//
// Deletes a file, empty directory, or (recursive:YES) a whole directory
// tree. Never follows symlinks while recursing -- a symlinked directory
// inside the tree is unlinked as itself, never traversed into.
- (void)removeItemAtGuestPath:(NSString *)guestPath
                     recursive:(BOOL)recursive
                    completion:(void (^)(BOOL ok, NSError * _Nullable error))completion;


#pragma mark Terminal integration

// Interactive lane. Best-effort working directory of the process group in the foreground of the
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

// Diagnostic, off unless ISH_BRIDGE_LANE_SELFTEST is set in the environment.
// Exercises the two-lane scheduler against the live guest filesystem and logs a
// PASS/FAIL summary: how long a listing waits while a bulk copy is in flight,
// and whether per-path ordering holds over many repetitions of the shapes a
// naive queue split breaks (mutate-then-list issued back to back, in both
// directions across the lanes). Returns immediately when the variable is unset.
//
// Companion knob: ISH_BRIDGE_LANE_LOG logs one line per operation with its lane
// and how long it waited to start.
void ISHGuestFileBridgeRunSelfTestIfRequested(void);

NS_ASSUME_NONNULL_END
