//
//  RootsTableViewController.h
//  iSH
//
//  Created by Theodore Dubois on 6/7/20.
//

#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

@interface RootsTableViewController : UITableViewController <UIDocumentPickerDelegate>

@property (nonatomic) BOOL choosesRootOnSelection;
@property (nonatomic, copy, nullable) void (^rootSelectionHandler)(NSString *rootName);

// Imports a root archive as a new filesystem, named after the file, with this
// screen's progress and error alerts. securityScoped: the URL came from
// outside the app's container. removeWhenDone: the file is the app's own copy
// (an archive another app handed over arrives in Documents/Inbox) and is
// deleted once the import has finished, whether or not it succeeded.
- (void)importArchiveAtURL:(NSURL *)url securityScoped:(BOOL)securityScoped removeWhenDone:(BOOL)removeWhenDone;

@end

// The archive types Browse Files offers and that the app is listed for in
// Files' Share and other apps' "Open in" (CFBundleDocumentTypes in Info.plist;
// keep the two the same).
NSArray<NSString *> *RootArchiveContentTypes(void);

NS_ASSUME_NONNULL_END
