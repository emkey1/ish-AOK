#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

extern NSString *const ISHInitialWindowWorkspaceValue;
extern NSString *const ISHInitialWindowChooseFilesystemValue;
extern NSString *const ISHInitialWindowWaylandValue;
extern UINavigationController *ISHCreateWorkspaceNavigationController(void);
extern UINavigationController *ISHCreateWorkspaceNavigationControllerForTool(NSString *_Nullable toolIdentifier);
extern BOOL ISHShouldLaunchWorkspaceAtStartup(void);
// The "Wayland Display" startup mode: launch straight into a fullscreen
// standalone DisplayViewController (no Workspace desktop around it).
extern BOOL ISHShouldLaunchWaylandDisplayAtStartup(void);
extern NSString *_Nullable ISHWorkspaceToolIdentifierForViewController(UIViewController *viewController);

// Adopted by an applet that can be told to open/reveal a specific guest path
// (e.g. MotePad opening a text file, or the File Manager revealing a file's
// containing folder). Routed through
// -[WorkspaceViewController openWorkspaceToolWithIdentifier:fileGuestPath:].
@protocol WorkspaceFileOpenable <NSObject>
- (void)workspaceOpenFileAtGuestPath:(NSString *)guestPath;
@end

// Adopted by an applet with its own keyboard-input view (e.g. MotePad's text
// view) that should reclaim first responder whenever its window becomes
// frontmost -- a tap, Ctrl+Tab window cycling, or Cmd+Arrow Desktop switching
// all funnel through here. Terminal-hosted windows don't need this: they
// already focus via hostedTerminalViewController.
@protocol WorkspaceFocusable <NSObject>
- (void)workspaceToolDidBecomeFrontmost;
@end

// Adopted by an applet whose text can be made bigger or smaller: Cmd+= (or
// Cmd++), Cmd+- and Cmd+0, as a terminal window already does.
//
// The Workspace owns the chords and the steps, so an applet declares the
// protocol and nothing else about keys. The scale is per window: 1.0 is the
// applet's own default, and the Workspace saves it in the window's layout
// descriptor ("textScale"), so nothing goes in the applet's own state.
// WorkspaceThemedToolViewController implements the property; a subclass
// overrides -workspaceApplyTextScale to re-font its own views.
//
// The Workspace also takes keyboard focus away from a background terminal when
// a window adopting this comes to the front. Otherwise the terminal keeps first
// responder and its own Cmd+= resizes the terminal instead.
@protocol WorkspaceTextScalable <NSObject>
@property (nonatomic) CGFloat workspaceTextScale;
@end

// A page of an applet whose window holds a navigation controller -- Settings,
// Diagnostics, Filesystems -- and whose text follows that window's text size.
//
// Those pages are ordinary UIKit screens that iSH-AOK also shows outside the
// Workspace, so the scale cannot be theirs. The Workspace looks for
// WorkspaceTextScalable on the window's content view controller, which for
// these applets is the navigation controller, so that is what holds it. A page
// reads it with ISHWorkspaceTextScaleForViewController whenever it makes text,
// which is what sizes a page pushed later and a row scrolled in later. Anywhere
// else it reads 1.0, and the page's text is not touched.
@protocol WorkspaceTextScaledPage <NSObject>
// The window's scale changed. Sent to every page on the stack whose view is
// loaded, not only the top one: Back shows the others again.
- (void)workspaceTextScaleDidChange;
@end

// The navigation controller the Workspace wraps Diagnostics and Filesystems in.
// Only the Workspace makes one; the same pages anywhere else sit in a plain
// UINavigationController. (Settings gets a subclass of its storyboard's own
// navigation controller class instead; see ISHCreateWorkspaceToolViewController.)
@interface WorkspaceToolNavigationController : UINavigationController <WorkspaceTextScalable>
@end

// The text scale of the Workspace window `viewController` is shown in: that of
// the nearest view controller up its parent chain adopting
// WorkspaceTextScalable. 1.0 anywhere else, including a screen presented
// modally from inside such a window.
extern CGFloat ISHWorkspaceTextScaleForViewController(UIViewController *viewController);
// `size` at `scale`, rounded to half a point as the applets round it.
extern CGFloat ISHWorkspaceScaledPointSize(CGFloat size, CGFloat scale);
// Scale a UILabel's or UITextField's font from its unscaled font, which is
// remembered, so calling it again never compounds. A font set on the view since
// the last call is taken as the new unscaled font. At 1.0 a view that was never
// scaled is not touched at all.
extern void ISHWorkspaceScaleTextFont(UIView *_Nullable view, CGFloat scale);
// The same for a table cell's labels and text fields. A page calls it on every
// cell it returns from -tableView:cellForRowAtIndexPath:, which covers reloads,
// reuse and static cells alike.
extern void ISHWorkspaceScaleTableViewCell(UITableViewCell *_Nullable cell, CGFloat scale);
// What -workspaceTextScaleDidChange does for a table page.
extern void ISHWorkspaceRescaleTableView(UITableView *tableView, CGFloat scale);
// The font a label or text field had before it was scaled. For code that makes
// a new font from the current one's size, which would otherwise start from the
// scaled size and be scaled again.
extern UIFont *_Nullable ISHWorkspaceUnscaledFont(UIView *view);
// The row height for a table whose rows keep to the 44-point standard because
// nothing in them pushes on the height (a label and a switch, each centred).
// `height` unchanged at 1.0 and below; above, at least 44 points at the scale.
extern CGFloat ISHWorkspaceTextScaledRowHeight(CGFloat height, CGFloat scale);

// Adopted by an applet whose content should survive an app restart (e.g. the
// File Manager's current directory, a viewer's open file). The returned
// dictionary is stored inside the saved window-layout descriptor in
// NSUserDefaults, so it must contain only plist types and stay small.
// Restore is called after viewDidLoad, when the window is recreated from a
// saved layout.
// Capture the live Workspace arrangement into the saved layout, as part of
// taking a checkpoint.
//
// A checkpoint saves the guest; windows and applets are app state, and the two
// have to be captured in the same act or they describe different machines. A
// no-op when no Workspace is on screen (shell mode), which leaves any earlier
// layout untouched rather than clearing it.
// Capture the arrangement for a suspend. Pass the image being written and the
// layout is filed BESIDE it, so the arrangement that comes back is the one that
// belongs to the machine being resumed; pass nil (a background save, with no
// particular image in hand) and it lands in the shared defaults as before.
void ISHWorkspaceCaptureLayoutForSuspend(NSString *_Nullable imagePath);
// The layout filed with that image, or nil if it has none.
NSArray<NSDictionary<NSString *, id> *> *_Nullable ISHWorkspaceLayoutForSessionImage(NSString *_Nullable imagePath);
// Remove an image's layout when the image itself goes.
void ISHWorkspaceForgetLayoutForSessionImage(NSString *_Nullable imagePath);

@protocol WorkspaceStatefulTool <NSObject>
- (nullable NSDictionary<NSString *, id> *)workspaceToolStateForSaving;
- (void)workspaceRestoreToolState:(NSDictionary<NSString *, id> *)state;
@end

@class WorkspaceViewController;

// Base class for every Workspace applet's content view controller. Provides a
// themed background, `toolContentView` (the safe-area-inset area a subclass
// builds its UI in), and factory methods for theme-tracked cards/labels/text
// views/progress views that automatically recolor on a theme change. A
// subclass overrides -viewDidLoad (calling super first) and builds its UI
// there; see WorkspaceFileManager.m or MotePadDocumentStore's owner for a
// worked example.
@interface WorkspaceThemedToolViewController : UIViewController

@property (nonatomic, strong, readonly) UIView *toolContentView;
@property (nonatomic, weak) WorkspaceViewController *workspaceHostViewController;

- (UIView *)workspaceThemeCardView;
- (UILabel *)workspaceThemePrimaryLabelWithTextStyle:(UIFontTextStyle)textStyle monospaced:(BOOL)monospaced;
- (UILabel *)workspaceThemeSecondaryLabelWithTextStyle:(UIFontTextStyle)textStyle monospaced:(BOOL)monospaced;
- (UILabel *)workspaceThemeAccentLabelWithTextStyle:(UIFontTextStyle)textStyle monospaced:(BOOL)monospaced;
- (UITextView *)workspaceThemeTextView;
- (UIProgressView *)workspaceThemeProgressView;
- (NSDictionary<NSString *, UIColor *> *)workspaceTheme;
- (void)workspaceApplyTheme;

// The WorkspaceTextScalable implementation, for subclasses that adopt it. 1.0
// is the default. Setting it re-applies at once when the view is loaded, and
// again on every appearance.
@property (nonatomic) CGFloat workspaceTextScale;
// Re-font for the current workspaceTextScale. The base re-fonts the text views
// made by -workspaceThemeTextView. A subclass calls super, then scales its own
// views.
- (void)workspaceApplyTextScale;
// `size` scaled by workspaceTextScale, rounded to half a point.
- (CGFloat)workspaceScaledFontSize:(CGFloat)size;
// Scale a label whose font the applet set at its unscaled size. Remembers that
// size, so repeated calls do not compound. A font the applet sets later is
// taken as the new unscaled size.
- (void)workspaceScaleLabel:(UILabel *)label;

@end

@interface WorkspaceViewController : UIViewController

- (void)presentDesktopSwitchMenuFromView:(UIView *)sourceView sourceRect:(CGRect)sourceRect;

// Opens (or reuses, for a singleton tool) the window for `toolIdentifier`,
// brings it to the front, and — if its content view controller conforms to
// WorkspaceFileOpenable — delivers `guestPath` to it. No-op if the tool
// identifier has no factory registration.
- (void)openWorkspaceToolWithIdentifier:(NSString *)toolIdentifier fileGuestPath:(NSString *)guestPath;
- (void)openWorkspaceToolWithIdentifier:(NSString *)toolIdentifier;
// Brings the tool's existing window forward, or opens one.
- (void)openOrFocusWorkspaceToolIdentifier:(NSString *)toolIdentifier;

// One text-size step for an applet adopting WorkspaceTextScalable: +1 bigger,
// -1 smaller, 0 back to the default. What the Cmd+= / Cmd+- / Cmd+0 chords do,
// for an applet that also offers it as a menu item.
- (void)adjustTextSizeForToolViewController:(UIViewController *)viewController step:(NSInteger)step;

// Opens a new terminal window and, once its shell is up, injects `command` as a typed line
// (empty command just opens a fresh shell). Same mechanism a Launcher shortcut's command runs
// through -- see -runLauncherShortcutWithCommand:title: in the .m.
- (void)launchTerminalWithCommand:(NSString *)command title:(nullable NSString *)title;

@end

NS_ASSUME_NONNULL_END
