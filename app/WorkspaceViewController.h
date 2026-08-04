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

// Adopted by an applet whose content should survive an app restart (e.g. the
// File Manager's current directory, a viewer's open file). The returned
// dictionary is stored inside the saved window-layout descriptor in
// NSUserDefaults, so it must contain only plist types and stay small.
// Restore is called after viewDidLoad, when the window is recreated from a
// saved layout.
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

@end

@interface WorkspaceViewController : UIViewController

- (void)presentDesktopSwitchMenuFromView:(UIView *)sourceView sourceRect:(CGRect)sourceRect;

// Opens (or reuses, for a singleton tool) the window for `toolIdentifier`,
// brings it to the front, and — if its content view controller conforms to
// WorkspaceFileOpenable — delivers `guestPath` to it. No-op if the tool
// identifier has no factory registration.
- (void)openWorkspaceToolWithIdentifier:(NSString *)toolIdentifier fileGuestPath:(NSString *)guestPath;

// Opens a new terminal window and, once its shell is up, injects `command` as a typed line
// (empty command just opens a fresh shell). Same mechanism a Launcher shortcut's command runs
// through -- see -runLauncherShortcutWithCommand:title: in the .m.
- (void)launchTerminalWithCommand:(NSString *)command title:(nullable NSString *)title;

@end

NS_ASSUME_NONNULL_END
