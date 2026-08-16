//
//  Roots.h
//  iSH
//
//  Created by Theodore Dubois on 6/7/20.
//

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@protocol ProgressReporter

- (void)updateProgress:(double)progressFraction message:(NSString *)progressMessage;
- (BOOL)shouldCancel;

@end

FOUNDATION_EXPORT NSNotificationName const RootsDidFinishInitialSelectionNotification;

@interface Roots : NSObject

+ (instancetype)instance;

@property (readonly) NSOrderedSet<NSString *> *roots;
@property NSString *defaultRoot;
// The root actually mounted as / this session, recorded by the app when it
// boots; nil until then. NOT interchangeable with defaultRoot, which is a
// preference the user can change at any time and that only takes effect at the
// next launch -- renaming or deleting the running root because it no longer
// matches defaultRoot would move / out from under the live guest.
@property (nullable) NSString *bootedRoot;
@property (readonly) BOOL wantsVersionFile;
@property (readonly) BOOL needsInitialRootSelection;
@property (readonly) BOOL initialBundledRootImportInProgress;
@property (readonly, nullable) NSError *initialBundledRootImportError;
- (NSArray<NSDictionary<NSString *, NSString *> *> *)bundledRootChoices;
// YES if this bundled choice's archive isn't shipped in the app bundle and
// hasn't already been downloaded into /AOK/persist/roots -- i.e. selecting it
// will trigger a network download before it can be imported.
- (BOOL)bundledRootChoiceNeedsDownload:(NSDictionary<NSString *, NSString *> *)choice;
- (NSURL *)rootUrl:(NSString *)name;
- (nullable NSString *)guestABIForRootNamed:(NSString *)name;
// Archive files in the shared /AOK/persist/roots directory (the AppGroup
// container's AOK/persist mount, common to all roots). Surfaced in the
// Filesystems screen as the "Root Cached Filesystems" section.
- (NSArray<NSURL *> *)cachedRootArchiveURLs;
- (void)resumeDeferredFileProviderDomainSync;
- (BOOL)importRootFromArchive:(NSURL *)archive name:(NSString *)name error:(NSError **)error progressReporter:(id<ProgressReporter> _Nullable)progress;
- (BOOL)importBundledRootChoice:(NSString *)identifier error:(NSError **)error progressReporter:(id<ProgressReporter> _Nullable)progress;
- (BOOL)exportRootNamed:(NSString *)name toArchive:(NSURL *)archive error:(NSError **)error progressReporter:(id<ProgressReporter> _Nullable)progress;
- (BOOL)destroyRootNamed:(NSString *)name error:(NSError **)error;
- (BOOL)renameRoot:(NSString *)name toName:(NSString *)newName error:(NSError **)error;
// Mount an installed root read-write at /AOK/roots/<name> for the guest. Boot
// calls this for every root other than the one it booted; rename calls it again
// under the new name. Returns whether the root ended up mounted.
- (BOOL)exposeRootNamed:(NSString *)name;

@end

NS_ASSUME_NONNULL_END
