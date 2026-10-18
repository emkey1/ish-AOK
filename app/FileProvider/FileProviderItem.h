//
//  FileProviderItem.h
//  iSHFiles
//
//  Created by Theodore Dubois on 9/20/18.
//

#import <FileProvider/FileProvider.h>
#import "FileProviderExtension.h"

NS_ASSUME_NONNULL_BEGIN

FOUNDATION_EXPORT NSString *ISHFileProviderVirtualIdentifierForPath(NSString *path);
FOUNDATION_EXPORT BOOL ISHFileProviderIsVirtualIdentifier(NSString *identifier);
FOUNDATION_EXPORT NSString * _Nullable ISHFileProviderPathForIdentifier(NSString *identifier);

// Under the single "iSH-AOK" file-provider domain, every item identifier is
// scoped to a root: the domain root container lists the installed roots, each
// root is a folder whose identifier is the bare root name, and items inside a
// root are "<rootName>+<innerIdentifier>" where innerIdentifier is the existing
// per-root scheme (decimal inode or "virt_..."). '+' is illegal in root names
// (RootNameIsValid) and never appears in an inode or base64url payload, so the
// composed identifier is a single, round-trippable path component.
FOUNDATION_EXPORT NSString *ISHFileProviderComposeIdentifier(NSString *rootName, NSFileProviderItemIdentifier innerIdentifier);
// Returns nil for the domain root container, the root name otherwise.
FOUNDATION_EXPORT NSString * _Nullable ISHFileProviderRootNameForIdentifier(NSFileProviderItemIdentifier identifier);
// Returns the per-root inner identifier; NSFileProviderRootContainerItemIdentifier
// for a root folder (i.e. that root's "/").
FOUNDATION_EXPORT NSFileProviderItemIdentifier ISHFileProviderInnerIdentifier(NSFileProviderItemIdentifier identifier);

@interface FileProviderItem : NSObject <NSFileProviderItem>

// `mountOwner` is nil only for the synthetic domain root (the folder listing the
// installed roots); every other item is scoped to a specific root's mount.
- (instancetype)initWithIdentifier:(NSFileProviderItemIdentifier)identifier mountOwner:(nullable ISHFileProviderMount *)mountOwner error:(NSError *_Nullable *)err;
- (void)loadToURL:(NSURL *)url;
- (void)saveFromURL:(NSURL *)url;
- (int)openNewFDWithError:(NSError *_Nullable *)err;

@property (readonly) NSString *path;
// When set, -parentItemIdentifier returns this instead of computing one.
@property (nullable, copy) NSFileProviderItemIdentifier parentIdentifierOverride;
@property (readonly) ISHFileProviderMount *mountOwner;
@property (readonly) struct fakefs_mount *mount;

@end

NS_ASSUME_NONNULL_END
