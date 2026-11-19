//
//  MotePadDocumentStore.h
//  AOK-iSH
//
//  Guest-filesystem text I/O for the MotePad Workspace applet. Reads/writes/lists
//  files through the emulated VFS (borrowing pid 1 as `current`, exactly like
//  AudioLibrary) with an NSFileManager fast path for the realfs /AOK/persist mount.
//  Kept in its own translation unit so the kernel C headers never mix into the big
//  UIKit WorkspaceViewController.m.
//

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

typedef NS_ENUM(NSInteger, MotePadEntryType) {
    MotePadEntryTypeFile,
    MotePadEntryTypeDirectory,
    MotePadEntryTypeOther,
};

@interface MotePadDirectoryEntry : NSObject
@property (nonatomic, copy) NSString *name;       // last path component
@property (nonatomic, copy) NSString *guestPath;  // absolute guest path
@property (nonatomic) MotePadEntryType type;
@end

@interface MotePadDocumentStore : NSObject

+ (instancetype)sharedStore;

// The serial queue every MotePad filesystem operation runs on. Serial on purpose:
// a save can never race a load. Exposed so callers can order their own FS work
// (e.g. draft-vs-file timestamp checks) behind pending operations.
+ (dispatch_queue_t)ioQueue;

#pragma mark Asynchronous API (preferred)
// These run the matching synchronous operation on +ioQueue and invoke the completion
// on the main queue. Guest-VFS operations block on emulator locks (fakefs SQLite,
// inodes_lock), so UI code must use these, never the synchronous methods below.

// Directory listing (directories first, then files, each sorted case-insensitively).
// entries is nil if the directory can't be opened.
- (void)listDirectoryAtGuestPath:(NSString *)guestPath
                      completion:(void (^)(NSArray<MotePadDirectoryEntry *> * _Nullable entries))completion;

// Read a file's contents as text. Tries UTF-8, then falls back to a lossy decode so
// a non-UTF-8 file still opens (degrades gracefully). text is nil + error on failure.
- (void)readTextFileAtGuestPath:(NSString *)guestPath
                     completion:(void (^)(NSString * _Nullable text, NSError * _Nullable error))completion;

// Write text as UTF-8 to a guest path atomically (temp file + rename), creating the
// file if needed. ok is NO + error on failure.
- (void)writeText:(NSString *)text
      toGuestPath:(NSString *)guestPath
       completion:(void (^)(BOOL ok, NSError * _Nullable error))completion;

// A sensible writable starting directory for the browser: /AOK/persist when present, else "/".
- (void)defaultDirectoryGuestPathWithCompletion:(void (^)(NSString *directory))completion;

// Last-modification date of a path, or nil if it doesn't exist / can't be statted.
// Used to decide whether an autosaved draft is newer than the file it shadows.
- (void)modificationDateAtGuestPath:(NSString *)guestPath
                         completion:(void (^)(NSDate * _Nullable date))completion;

#pragma mark Synchronous API (blocking — never call on the main thread)
// Workers behind the asynchronous API. They block on host I/O and emulator locks;
// call them from +ioQueue (or another background queue), never from UI code.

- (nullable NSArray<MotePadDirectoryEntry *> *)listDirectoryAtGuestPath:(NSString *)guestPath;
- (nullable NSString *)readTextFileAtGuestPath:(NSString *)guestPath error:(NSError **)error;
- (BOOL)writeText:(NSString *)text toGuestPath:(NSString *)guestPath error:(NSError **)error;

// Whether a path exists, and (optionally) whether it's a directory.
- (BOOL)fileExistsAtGuestPath:(NSString *)guestPath isDirectory:(nullable BOOL *)isDirectory;

- (NSString *)defaultDirectoryGuestPath;

@end

NS_ASSUME_NONNULL_END
