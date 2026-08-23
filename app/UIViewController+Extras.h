//
//  UIViewController+Extras.h
//  iSH
//
//  Created by Theodore Dubois on 9/23/18.
//

#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

@interface UIViewController (Extras)

- (void)presentError:(NSError *)error title:(NSString *)title;

// Anchors an action-sheet UIAlertController's popover so it can be presented on
// iPad without throwing "you must provide location information for this popover".
// `source` may be a UIBarButtonItem, a UIView (e.g. the tapped button or cell),
// or nil (falls back to the center of this controller's view). A no-op when the
// alert is not presented as a popover (e.g. iPhone, or a plain alert).
- (void)anchorPopoverForAlertController:(UIAlertController *)alertController toSource:(nullable id)source;

// Whether pushing a sub-page from here will actually give the user a way back.
//
// A non-nil navigationController is NOT enough. -navigationController walks the
// PARENT chain, so a bare child view controller inherits its host's navigation
// controller -- and Workspace mode's host hides its navigation bar
// (WorkspaceViewController.m, ISHCreateWorkspaceNavigationControllerForTool).
// Pushing there succeeds and strands the user: no chevron, no title, and the
// interactive pop gesture does not fire while the bar is hidden (measured --
// isEnabled still reads YES, so do not trust that flag).
//
// Callers that present a sub-page should branch on this rather than on
// `self.navigationController != nil`, and present modally when it is NO.
@property (nonatomic, readonly) BOOL ish_canPushSubpage;

@end

NS_ASSUME_NONNULL_END
