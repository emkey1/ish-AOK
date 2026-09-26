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
// Posted on the main thread when a catalogue refresh changed what is offered.
FOUNDATION_EXPORT NSNotificationName const RootsCatalogDidChangeNotification;

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
// The root this launch boots: ISH_BOOT_ROOT when that is set, otherwise
// defaultRoot. Read before the boot; after it, bootedRoot is the answer.
@property (readonly, nullable) NSString *rootToBoot;
// ISH_BOOT_ROOT as given, whether or not a root has that name; nil if unset.
@property (readonly, nullable) NSString *bootRootOverride;
@property (readonly) BOOL wantsVersionFile;
@property (readonly) BOOL needsInitialRootSelection;
@property (readonly) BOOL initialBundledRootImportInProgress;
@property (readonly, nullable) NSError *initialBundledRootImportError;
// Every choice the app knows about, including versions of a series the picker
// no longer offers -- use this to identify where an already-imported root came
// from, or to honour an identifier a user named explicitly.
- (NSArray<NSDictionary<NSString *, NSString *> *> *)bundledRootChoices;
// What to actually put in front of someone choosing a filesystem: the same
// list with every superseded version of a series removed (only the two most
// recent survive). See the series/version notes in Roots.m.
- (NSArray<NSDictionary<NSString *, NSString *> *> *)offeredRootChoices;
// Fetches the published catalogue so this build offers the filesystems that
// exist now rather than the ones that existed when it was built. Returns
// immediately; posts RootsCatalogDidChangeNotification if the offered list
// actually changed. Safe to call often -- it rate-limits itself, and every
// failure leaves the current catalogue untouched.
- (void)refreshRootCatalogFromNetwork;
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
