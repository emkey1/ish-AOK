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

// ISHActionSheet: a choice sheet that can still be used when AOK runs on a Mac.
//
// On iPhone and iPad it IS a UIAlertController. It has the same style, the same
// actions in the same order, the same popover anchor, and is presented the same
// way, so nothing changes there.
//
// A "Designed for iPad" app on an Apple Silicon Mac draws an action sheet as one
// horizontal row: title and message on the left, then one button per action.
// The row does not wrap or scroll. The Launcher's Add Built-in sheet (22
// actions) ran off the right edge of the screen, and the actions past the edge
// could not be clicked.
//
// So on a Mac, a sheet with more than two non-cancel actions, or with button
// titles too long to share one row, becomes a list instead: a small table in a
// navigation controller. It uses the sheet's title and message, and the cancel
// action becomes a bar button with the same title. An action sheet shows the
// list in a popover anchored where the sheet would have pointed. An alert shows
// it as a centred form sheet. Up to two short choices stay a real alert, which
// is the shape macOS alerts are designed for (at most three buttons, counting
// Cancel), so short confirmations such as "Close Workspace" are unchanged.
//
// Picking a row dismisses the list and THEN runs the handler, passing the same
// UIAlertAction object. Handlers can present the next controller straight
// away, and the existing dispatch_async deferrals still work. Clicking outside
// a popover, or the cancel button, runs the cancel action's handler, as
// dismissing an iPad action-sheet popover does. The handler runs once at most.
//
// Debug override: ISH_FORCE_MAC_SHEETS=1 in the environment takes the Mac path
// on iPhone and iPad too, so the list can be tested in the simulator:
//     SIMCTL_CHILD_ISH_FORCE_MAC_SHEETS=1 xcrun simctl launch <device> app.ish.iSH-AOK
@interface ISHActionSheet : NSObject

+ (instancetype)actionSheetWithTitle:(nullable NSString *)title message:(nullable NSString *)message;
// UIAlertControllerStyleAlert. Only for alerts without text fields: a list has
// nowhere to put one.
+ (instancetype)alertWithTitle:(nullable NSString *)title message:(nullable NSString *)message;

// Same meaning as +[UIAlertAction actionWithTitle:style:handler:]. Returns the
// action so a caller can still set `enabled`. The list honours it too.
- (UIAlertAction *)addActionWithTitle:(NSString *)title
                                style:(UIAlertActionStyle)style
                              handler:(void (^_Nullable)(UIAlertAction *action))handler;

// YES when -present... will show the list instead of the UIAlertController.
@property (nonatomic, readonly) BOOL presentsAsList;

// An action sheet points at sourceRect in sourceView. A nil sourceView points
// at the centre of the presenter's view. An alert ignores the anchor.
- (void)presentFromViewController:(UIViewController *)presenter
                       sourceView:(nullable UIView *)sourceView
                       sourceRect:(CGRect)sourceRect;
// `source` is anchored like -anchorPopoverForAlertController:toSource:. It can be
// a UIBarButtonItem, a UIView (its bounds), or nil (the presenter's centre).
- (void)presentFromViewController:(UIViewController *)presenter source:(nullable id)source;

@end

// Whether ISHActionSheet takes its Mac path: running as an iOS app on a Mac,
// Mac Catalyst, or ISH_FORCE_MAC_SHEETS=1.
BOOL ISHActionSheetUsesMacPresentation(void);

// Gives a storyboard table self-sizing section headers and footers when the app
// runs on a Mac. Call it from -viewDidLoad.
//
// A table from a storyboard decodes with 18pt section heights and estimated
// section heights of 0, which is UIKit's non-self-sizing path. On an iPhone or
// iPad that path still sizes a section with a title to fit its text. Run as an
// iOS app on a Mac it does not: measured in Filesystems, every header and footer
// was exactly 18pt and the footer views had no textLabel, so the headers
// vanished and the footers were cut to a strip. Self-sizing measures the header
// and footer views themselves.
//
// Nothing changes anywhere else. Self-sizing there would give an untitled first
// section a 17.5pt header it does not have today.
void ISHSizeTableSectionTitlesOnMac(UITableView *tableView);

// How far down, in `view`'s own coordinates, content has to start to stay clear
// of the system's window controls (close, minimize, zoom).
//
// iPadOS 26 draws those controls over the top-leading corner of a window, and
// the plain safe area does not account for them: a windowed scene reports a
// top safe area of 10pt while the controls reach ~50pt down. The status bar is
// no guide either -- the status bar manager still reports its 32pt for a window
// that is nowhere near the status bar. The window's safe area with vertical
// corner adaptation is the value that includes the controls, so that is what
// this returns (#580).
//
// If a window's corner adaptation makes no room at all -- the value is just the
// plain safe area, as the 555 screenshot in #580 shows for a window at the top
// of the screen -- this assumes the clearance measured everywhere else instead.
//
// It is measured on the WINDOW, not on `view`, deliberately: a view
// controller's own additionalSafeAreaInsets feed into its view's safe area, so
// asking the view would read back the inset being computed from it.
//
// It is 0 wherever the controls are not over the content: a window that covers
// its screen (full screen, where they live in the menu bar; iPhone), iOS before
// 26, or a view that is not in a window. So taking the larger of this and an
// existing top inset changes nothing in those cases.
CGFloat ISHWindowingControlsTopInset(UIView *view);

NS_ASSUME_NONNULL_END
