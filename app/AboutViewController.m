//
//  AboutViewController.m
//  iSH
//
//  Created by Theodore Dubois on 9/23/18.
//

#import "AboutViewController.h"
#import "AppDelegate.h"
#import "CurrentRoot.h"
#import "AppGroup.h"
#import "Diagnostics.h"
#import "UserPreferences.h"
#import "iOSFS.h"
#import "UIApplication+OpenURL.h"
#import "NSObject+SaneKVO.h"
#import "SceneDelegate.h"
#import "Terminal.h"
#import "UIViewController+Extras.h"
#import "WorkspaceViewController.h"
#import "MarkdownRenderer.h"
#import "LLMChatInternal.h"
#if __has_include("libiSH_AOKApp-Swift.h")
#import "libiSH_AOKApp-Swift.h" // AOKFoundationModelsBridge (Swift, iOS 26+ FoundationModels wrapper)
#endif
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include "kernel/init.h" // run_guest_command_capture (guest-shell tool)


NSString *const kPreferenceOpenDiagnosticsOnLaunchKey = @"openDiagnosticsOnLaunch";

UINavigationController *ISHCreateAboutNavigationController(BOOL recoveryMode, BOOL startInDiagnostics) {
    UINavigationController *navigationController = [[UIStoryboard storyboardWithName:@"About" bundle:nil] instantiateInitialViewController];
    AboutViewController *aboutViewController = (AboutViewController *) navigationController.topViewController;
    aboutViewController.recoveryMode = recoveryMode;
    aboutViewController.startInDiagnostics = startInDiagnostics;
    return navigationController;
}

@interface DiagnosticsViewController : UIViewController
@end


UIViewController *ISHCreateDiagnosticsViewController(void) {
    return [DiagnosticsViewController new];
}



// Settings -> Keyboard Toolbar (#609); defined at the end of this file.
@interface ISHToolbarKeysViewController : UITableViewController <WorkspaceTextScaledPage>
@end

UIViewController *ISHCreateToolbarKeysViewController(void) {
    return [ISHToolbarKeysViewController new];
}


@interface AboutViewController () <WorkspaceTextScaledPage>
@property (weak, nonatomic) IBOutlet UITableViewCell *capsLockMappingCell;
@property (weak, nonatomic) IBOutlet UITableViewCell *keyboardToolbarCell;
@property (weak, nonatomic) IBOutlet UITableViewCell *themeCell;
@property (weak, nonatomic) IBOutlet UITableViewCell *initialWindowCell;
@property (weak, nonatomic) IBOutlet UITableViewCell *diagnosticsCell;
@property (weak, nonatomic) IBOutlet UISwitch *disableDimmingSwitch;
@property (weak, nonatomic) IBOutlet UISwitch *enableMulticoreSwitch;
@property (weak, nonatomic) IBOutlet UISwitch *enableHLESwitch;
@property (weak, nonatomic) IBOutlet UISwitch *enableCryptoAccelSwitch;
@property (weak, nonatomic) IBOutlet UISwitch *enablePixAccelSwitch;
@property (weak, nonatomic) IBOutlet UISwitch *enableExtraLockingSwitch;
@property (weak, nonatomic) IBOutlet UITextField *launchCommandField;
@property (weak, nonatomic) IBOutlet UITextField *bootCommandField;

@property (weak, nonatomic) IBOutlet UITableViewCell *sendFeedback;
@property (weak, nonatomic) IBOutlet UITableViewCell *openGithub;
@property (weak, nonatomic) IBOutlet UITableViewCell *openDiscord;
@property (weak, nonatomic) IBOutlet UITableViewCell *customDnsCell;

@property (weak, nonatomic) IBOutlet UITableViewCell *upgradeApkCell;
@property (weak, nonatomic) IBOutlet UILabel *upgradeApkLabel;
@property (weak, nonatomic) IBOutlet UIView *upgradeApkBadge;
@property (weak, nonatomic) IBOutlet UITableViewCell *exportContainerCell;
@property (weak, nonatomic) IBOutlet UITableViewCell *resetMountsCell;

@property (weak, nonatomic) IBOutlet UILabel *versionLabel;

@property (nonatomic, strong) UISwitch *llmClientSwitch;
@property (nonatomic, strong) UISwitch *loginAsDefaultUserSwitch;
@property (nonatomic, strong) UISwitch *shortcutsRunCommandsSwitch;

@end

// The report's point size at text scale 1.0. In a Workspace window it follows
// the window's text size (Cmd+= / Cmd+-).
static const CGFloat kDiagnosticsFontSize = 12;

// How close to the end counts as "at the end", for deciding whether to follow
// the tail. One line of the monospaced 12pt font, near enough; it grows with
// the font.
static const CGFloat kDiagnosticsBottomSlack = 16;

@interface DiagnosticsViewController () <WorkspaceTextScaledPage>
// Set when the workspace embeds this in its own window, which already draws a
// title bar saying "Diagnostics". Without it the pane shows that word twice,
// stacked.
@property (nonatomic) BOOL embeddedInWorkspaceWindow;
@end

@implementation DiagnosticsViewController {
    UITextView *_textView;
    BOOL _everLoaded;         // the first load starts at the top; later ones do not
    CGFloat _reportFontSize;  // kDiagnosticsFontSize at the window's text scale
}

- (void)viewDidLoad {
    [super viewDidLoad];
    if (!self.embeddedInWorkspaceWindow)
        self.title = NSLocalizedString(@"Diagnostics", @"Diagnostics screen title");
    if (@available(iOS 13.0, *)) {
        self.view.backgroundColor = UIColor.systemBackgroundColor;
    } else {
        self.view.backgroundColor = UIColor.whiteColor;
    }

    _textView = [[UITextView alloc] initWithFrame:self.view.bounds];
    _textView.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    _textView.editable = NO;
    _textView.alwaysBounceVertical = YES;
    if (@available(iOS 13.0, *)) {
        _textView.backgroundColor = UIColor.systemBackgroundColor;
        _textView.textColor = UIColor.labelColor;
    } else {
        _textView.backgroundColor = UIColor.whiteColor;
        _textView.textColor = UIColor.blackColor;
    }
    [self applyReportFont];
    [self.view addSubview:_textView];

    UIBarButtonItem *exportButton = [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemAction
                                                                                  target:self
                                                                                  action:@selector(exportDiagnostics:)];
    exportButton.accessibilityLabel = NSLocalizedString(@"Export", @"Accessibility label for the export diagnostics button");
    exportButton.accessibilityHint = NSLocalizedString(@"Shares the diagnostics report.", @"Accessibility hint for the export diagnostics button");
    UIBarButtonItem *refreshButton = [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemRefresh
                                                                                   target:self
                                                                                   action:@selector(refreshDiagnostics:)];
    refreshButton.accessibilityLabel = NSLocalizedString(@"Refresh", @"Accessibility label for the refresh diagnostics button");
    refreshButton.accessibilityHint = NSLocalizedString(@"Reloads the diagnostics report.", @"Accessibility hint for the refresh diagnostics button");
    self.navigationItem.rightBarButtonItems = @[exportButton, refreshButton];

    // DELIBERATELY NOT observing ISHDiagnosticsStoreDidUpdateNotification.
    //
    // The store posts it for every breadcrumb, so an open pane would rebuild
    // itself while it is being read -- and reassigning a text view's `.text`
    // drops any selection the reader has made. Someone highlighting a few lines
    // to copy them lost the highlight to the next guest process exit, which for
    // a screen whose whole purpose is getting the log to somebody else is worse
    // than showing figures a minute old. Refreshing is the Refresh button's job
    // and nothing else's; the report is a snapshot, and it says when it was
    // taken. See rebuildReport.
    [self refreshDiagnostics:nil];
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    // No refresh here either. viewDidLoad has already taken the snapshot, and
    // this controller is created fresh every time the screen is opened -- from
    // the Settings row, from the workspace tool, from the launch-diagnostics
    // preference -- so there is no path where a reappearance is showing an
    // empty pane. What it CAN be is a reappearance while the pane is open and
    // selected, and replacing the text there would drop the selection for no
    // new information.
}

- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    // Cleared once it has done its job, and that is deliberate.
    //
    // This is a one-shot: "open Diagnostics on the NEXT launch", not "always
    // open Diagnostics". Without the disarm it is a trap, because the launch
    // override wins over every other root controller -- so if it cannot be
    // cleared from inside the app, the app opens into Diagnostics forever and
    // the only way out is the Settings app successfully writing the key.
    // That happened: it was switched off in Settings and the preference on disk
    // stayed true, so every launch still landed here.
    //
    // I removed this once, reasoning that a Settings SWITCH should stay where
    // it is put. The reasoning was fine; the premise was wrong. The report that
    // prompted it -- "I set it and got the shell" -- was the app CRASHING in
    // willFinishLaunching, which runs before the scene connects, so Diagnostics
    // never got its turn and the disarm was never involved. The Settings title
    // now says "Next Launch", so the one-shot is stated rather than surprising.
    if ([NSUserDefaults.standardUserDefaults boolForKey:kPreferenceOpenDiagnosticsOnLaunchKey]) {
        [NSUserDefaults.standardUserDefaults setBool:NO forKey:kPreferenceOpenDiagnosticsOnLaunchKey];
        // Straight to disk. The Settings app and iSH-AOK are different
        // processes sharing this domain, and the stuck state above is what a
        // lost write looks like from the outside.
        [NSUserDefaults.standardUserDefaults synchronize];
    }
}

// The Refresh button. The ONLY thing that replaces the text after the first
// load; see the comment in viewDidLoad for why nothing else may.
- (void)refreshDiagnostics:(id)sender {
    [self rebuildReport];
}

// Whether the reader is looking at the end of the report, which is where the
// interesting part is: the sections are ordered summary-first and Recent
// Breadcrumbs last.
- (BOOL)textViewIsAtBottom {
    CGFloat insetBottom = 0;
    if (@available(iOS 11.0, *))
        insetBottom = _textView.adjustedContentInset.bottom;
    else
        insetBottom = _textView.contentInset.bottom;
    CGFloat maxOffset = _textView.contentSize.height - _textView.bounds.size.height + insetBottom;
    if (maxOffset <= 0)
        return YES;   // it all fits; there is nowhere else to be
    return _textView.contentOffset.y >= maxOffset - kDiagnosticsBottomSlack * _reportFontSize / kDiagnosticsFontSize;
}

// Take a snapshot of the report and show it, without moving the reader.
//
// Only ever called for the initial load and for the Refresh button. It used to
// run on every store update -- one per breadcrumb, i.e. per guest process exit
// and per keystroke -- and did `setContentOffset:CGPointZero` afterwards, so the
// pane snapped to the top faster than a finger could drag it. Reported from
// Discord twice over: first that it could not be scrolled down at all, then that
// the periodic rebuild was dropping copy/paste selections mid-copy. Both are the
// same root cause, replacing the text under someone who is reading it, and the
// answer to both is to do it only when asked.
//
// The offset is still preserved rather than reset, because a manual Refresh
// means "show me the latest", not "take me back to the top".
- (void)rebuildReport {
    NSString *report = [ISHDiagnosticsStore diagnosticsReport] ?: @"";
    if (_everLoaded && [report isEqualToString:_textView.text]) {
        // Identical: re-laying it out would cost a relayout and, on a text view
        // being read, a visible flicker, for no new information.
        return;
    }

    BOOL follow = _everLoaded && [self textViewIsAtBottom];
    CGPoint offset = _textView.contentOffset;

    _textView.text = report;
    [_textView layoutIfNeeded];   // so contentSize below describes the NEW text

    CGFloat insetBottom = 0;
    if (@available(iOS 11.0, *))
        insetBottom = _textView.adjustedContentInset.bottom;
    else
        insetBottom = _textView.contentInset.bottom;
    CGFloat maxOffset = _textView.contentSize.height - _textView.bounds.size.height + insetBottom;
    if (maxOffset < 0)
        maxOffset = 0;

    if (!_everLoaded) {
        // The first look starts at the summary, which names the app, the device
        // and the OS -- the part someone reporting a problem is asked for.
        offset = CGPointZero;
        if (@available(iOS 11.0, *))
            offset.y = -_textView.adjustedContentInset.top;
    } else if (follow) {
        // Reading the end when Refresh was pressed: stay on the end, which has
        // grown. Anchoring to the old offset instead would silently slide the
        // newest lines out from under someone who pressed Refresh precisely to
        // see them.
        offset.y = maxOffset;
    } else if (offset.y > maxOffset) {
        // The report shrank under them (the breadcrumb ring wraps at 200, the
        // exits ring at 32), so the old offset is past the end now.
        offset.y = maxOffset;
    }
    [_textView setContentOffset:offset animated:NO];
    _everLoaded = YES;
}

// The report's font, at the text size of the Workspace window showing it.
- (void)applyReportFont {
    _reportFontSize = ISHWorkspaceScaledPointSize(kDiagnosticsFontSize, ISHWorkspaceTextScaleForViewController(self));
    if (@available(iOS 13.0, *)) {
        _textView.font = [UIFont monospacedSystemFontOfSize:_reportFontSize weight:UIFontWeightRegular];
    } else {
        _textView.font = [UIFont fontWithName:@"Menlo-Regular" size:_reportFontSize] ?: [UIFont systemFontOfSize:_reportFontSize];
    }
}

// Cmd+= / Cmd+- in a Workspace window. Only the font changes; the text is not
// replaced, so a selection survives (viewDidLoad says why that matters here).
// The reader stays where they were: on the end if they were following it,
// otherwise on the line that was at the top, which the new size moves.
- (void)workspaceTextScaleDidChange {
    if (!_everLoaded) {
        [self applyReportFont];
        return;
    }
    BOOL follow = [self textViewIsAtBottom];
    CGFloat insetTop = _textView.adjustedContentInset.top;
    UITextPosition *top = [_textView closestPositionToPoint:CGPointMake(_textView.textContainerInset.left + 1,
                                                                        _textView.contentOffset.y + insetTop)];
    NSInteger topIndex = top != nil ? [_textView offsetFromPosition:_textView.beginningOfDocument toPosition:top] : 0;

    [self applyReportFont];
    [_textView layoutIfNeeded];   // so contentSize and the caret below describe the NEW size

    CGFloat maxOffset = _textView.contentSize.height - _textView.bounds.size.height + _textView.adjustedContentInset.bottom;
    CGPoint offset = _textView.contentOffset;
    if (follow) {
        offset.y = maxOffset;
    } else {
        UITextPosition *position = [_textView positionFromPosition:_textView.beginningOfDocument offset:topIndex];
        if (position != nil)
            offset.y = CGRectGetMinY([_textView caretRectForPosition:position]) - _textView.textContainerInset.top - insetTop;
        offset.y = MIN(offset.y, maxOffset);
    }
    offset.y = MAX(offset.y, -insetTop);
    [_textView setContentOffset:offset animated:NO];
}

- (void)exportDiagnostics:(id)sender {
    NSError *error = nil;
    NSURL *bundleURL = [ISHDiagnosticsStore prepareExportBundle:&error];
    if (bundleURL == nil) {
        [self presentError:error title:NSLocalizedString(@"Export failed", @"Diagnostics export error alert title")];
        return;
    }

    UIActivityViewController *activityViewController =
        [[UIActivityViewController alloc] initWithActivityItems:@[bundleURL] applicationActivities:nil];
    UIPopoverPresentationController *popover = activityViewController.popoverPresentationController;
    if (popover != nil) {
        popover.barButtonItem = sender;
    }
    [self presentViewController:activityViewController animated:YES completion:nil];
}

@end

// The same screen for a workspace tool window, wrapped in its own navigation
// controller.
//
// A workspace tool gets a navigation bar only if the factory hands one back --
// the window chrome draws a title bar with a close button and (in Modern) the
// root menu, and nothing else. Diagnostics was returned bare, so its
// navigationItem.rightBarButtonItems -- Share and Refresh -- had nowhere to
// render, and in Workspace mode the screen had no way to export at all.
// Reported from Discord alongside the scrolling: the Share sheet is how people
// get the log off the device to read it.
//
// Filesystems and Settings already solved this the same way, for the same
// reason; see their comments in ISHWorkspaceViewControllerForToolIdentifier.
// The bar's title is suppressed (see embeddedInWorkspaceWindow) because the
// window's own title bar already says "Diagnostics". The navigation controller
// is the Workspace's own class, which carries the window's text size
// (Cmd+= / Cmd+-) to the report.
UIViewController *ISHCreateDiagnosticsNavigationController(void) {
    DiagnosticsViewController *diagnostics = [DiagnosticsViewController new];
    diagnostics.embeddedInWorkspaceWindow = YES;
    return [[WorkspaceToolNavigationController alloc] initWithRootViewController:diagnostics];
}


@implementation AboutViewController
{
    BOOL _didPresentInitialDiagnostics;
    __weak UIBarButtonItem *_modeButton;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    ISHSizeTableSectionTitlesOnMac(self.tableView);
    [self _updateUI];
    // Title and destination depend on which mode the window is currently in, and
    // the window isn't known until the view is in the hierarchy, so the title is
    // filled in by -_updateModeButton from -viewWillAppear: (GH #546).
    UIBarButtonItem *workspaceButton = [[UIBarButtonItem alloc] initWithTitle:NSLocalizedString(@"Workspace", @"Settings button that switches to the Workspace")
                                                                        style:UIBarButtonItemStylePlain
                                                                       target:self
                                                                       action:@selector(toggleWorkspaceMode:)];
    _modeButton = workspaceButton;
    if (self.recoveryMode) {
        self.navigationItem.title = NSLocalizedString(@"Recovery Mode", @"Settings title when launched in recovery mode");
        self.navigationItem.leftBarButtonItem = workspaceButton;
        self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithTitle:NSLocalizedString(@"Exit", @"Button that leaves recovery mode")
                                                                                  style:UIBarButtonItemStyleDone
                                                                                 target:self
                                                                                 action:@selector(exitRecovery:)];
    } else {
        self.navigationItem.rightBarButtonItem = workspaceButton;
    }
    _versionLabel.text = [NSString stringWithFormat:NSLocalizedString(@"iSH-AOK %@ (Build %@)", @"Settings version label; version, then build number"),
                          [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleShortVersionString"],
                          [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleVersion"]];

    [UserPreferences.shared observe:@[@"capsLockMapping", @"fontSize", @"launchCommand", @"bootCommand", @"shouldLockSleepNanoseconds"]
                            options:0 owner:self usingBlock:^(typeof(self) self) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self _updateUI];
        });
    }];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(_updateUI:) name:FsUpdatedNotification object:nil];
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [self _updateUI];
    [self _updateModeButton];
}

// The button switches to whichever mode you are NOT in, so it is labelled with
// its destination: "Workspace" from the shell, "Shell" from the Workspace. It
// used to always say "Workspace" and only ever go one way, which left no way
// back to the shell short of force-quitting the app (GH #546).
- (void)_updateModeButton {
    UIBarButtonItem *modeButton = _modeButton;
    if (modeButton == nil)
        return;
    // -viewWillAppear: can run before the view has a window (both when Settings
    // is presented modally and when the Workspace embeds it as a tool window),
    // and a nil window reads as "not the Workspace" -- which is why this is
    // refreshed again from -viewDidAppear:, where the window is attached. Fall
    // back to the app's own window so an early call still resolves the mode
    // rather than silently guessing "shell".
    UIWindow *window = self.view.window ?: ISHActivePresentationViewController().view.window;
    modeButton.title = ISHWindowIsShowingWorkspace(window) ? NSLocalizedString(@"Shell", @"Settings button that switches to the shell") : NSLocalizedString(@"Workspace", @"Settings button that switches to the Workspace");
}

- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    // The window is guaranteed attached by now; -viewWillAppear:'s attempt may
    // have had none to read.
    [self _updateModeButton];
    if (self.startInDiagnostics && !_didPresentInitialDiagnostics) {
        _didPresentInitialDiagnostics = YES;
        [self showDiagnostics:self.diagnosticsCell ?: self];
    }
}

- (IBAction)dismiss:(id)sender {
    [self dismissViewControllerAnimated:self completion:nil];
}

- (void)exitRecovery:(id)sender {
    [NSUserDefaults.standardUserDefaults setBool:NO forKey:@"recovery"];
    exit(0);
}

- (void)showDiagnostics:(id)sender {
    UIViewController *viewController = ISHCreateDiagnosticsViewController();
    [self.navigationController pushViewController:viewController animated:YES];
}

- (void)toggleWorkspaceMode:(id)sender {
    (void) sender;
    // Switch the current scene's root between the two modes, the same way
    // launch-at-startup picks one (SceneDelegate sets window.rootViewController).
    // Asking iPadOS to activate a *separate* scene instead opened split screen on
    // some iPads and failed outright on others (Stage Manager / M4). Swapping the
    // root sidesteps all of that, and the scene's state restoration still reports
    // a workspace scene because its root is one.
    UIWindow *window = self.view.window;
    if (window == nil) {
        // No window to swap the root of (Settings presented detached). Only the
        // forward direction has a sensible fallback here.
        if (!ISHWindowIsShowingWorkspace(window))
            [self presentViewController:ISHCreateWorkspaceNavigationControllerForTool(nil) animated:YES completion:nil];
        return;
    }
    if (ISHWindowIsShowingWorkspace(window))
        ISHWindowShowSessionShell(window);
    else
        ISHWindowShowWorkspace(window);
    [self _updateModeButton];
}

- (void)_updateUI:(NSNotification *)notification {
    [self _updateUI];
}

- (void)_updateUI {
    NSAssert(NSThread.isMainThread, @"This method needs to be called on the main thread");
    self.disableDimmingSwitch.on = UserPreferences.shared.shouldDisableDimming;
    self.enableMulticoreSwitch.on = UserPreferences.shared.shouldEnableMulticore;
    self.enableHLESwitch.on = UserPreferences.shared.shouldEnableHLE;
    self.enableCryptoAccelSwitch.on = UserPreferences.shared.shouldEnableCryptoAccel;
    self.enablePixAccelSwitch.on = UserPreferences.shared.shouldEnablePixAccel;
    self.enableExtraLockingSwitch.on = UserPreferences.shared.shouldEnableExtraLocking;
    self.initialWindowCell.textLabel.text = NSLocalizedString(@"Startup Mode", @"Settings row label");
    self.initialWindowCell.detailTextLabel.text = [self _initialWindowTitle];
    self.launchCommandField.text = [UserPreferences.shared.launchCommand componentsJoinedByString:@" "];
    self.bootCommandField.text = [UserPreferences.shared.bootCommand componentsJoinedByString:@" "];
    self.customDnsCell.textLabel.text = NSLocalizedString(@"Custom DNS Servers", @"Settings row label");
    NSString *customDnsServers = [UserPreferences.shared.customDnsServers stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if (UserPreferences.shared.shouldDisableResolvConfRewrite)
        self.customDnsCell.detailTextLabel.text = NSLocalizedString(@"Off (guest manages)", @"Custom DNS Servers value when resolv.conf is left alone");
    else
        self.customDnsCell.detailTextLabel.text = customDnsServers.length > 0 ? customDnsServers : NSLocalizedString(@"Automatic", @"Custom DNS Servers value when none are set");

    self.upgradeApkCell.userInteractionEnabled = FsNeedsRepositoryUpdate();
    self.upgradeApkLabel.enabled = FsNeedsRepositoryUpdate();
    self.upgradeApkBadge.hidden = !FsNeedsRepositoryUpdate();
    self.upgradeApkCell.accessibilityValue = FsNeedsRepositoryUpdate() ? NSLocalizedString(@"Update available", @"Accessibility value on the filesystem upgrade row") : nil;
    [self.tableView reloadData];
}

- (NSInteger)_visibleStoryboardSectionCount {
    return [super numberOfSectionsInTableView:self.tableView];
}

// Appended sections live past the storyboard's static ones, in a fixed order:
// user-account section, then LLM section, then Shortcuts section, then Other
// Filesystems, then Translated Code.
- (NSInteger)_userAccountSectionIndex {
    return [self _visibleStoryboardSectionCount];
}

- (NSInteger)_llmSectionIndex {
    return [self _visibleStoryboardSectionCount] + 1;
}

- (NSInteger)_shortcutsSectionIndex {
    return [self _visibleStoryboardSectionCount] + 2;
}

- (NSInteger)_foreignExecSectionIndex {
    return [self _visibleStoryboardSectionCount] + 3;
}

- (NSInteger)_jitCacheSectionIndex {
    return [self _visibleStoryboardSectionCount] + 4;
}

// Installing and choosing the Wayland display's desktop (GH #639).
- (NSInteger)_desktopSectionIndex {
    return [self _visibleStoryboardSectionCount] + 5;
}

- (UITableViewCell *)_desktopSetupCell {
    UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:nil];
    cell.textLabel.text = NSLocalizedString(@"Install a Desktop…", @"Settings row and Wayland display menu action: opens the desktop setup page");
    cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
    return cell;
}

static NSString *ISHForeignExecTitle(NSString *mode) {
    if ([mode isEqualToString:@"libs"])
        return NSLocalizedString(@"Use Their Libraries Here", @"Programs From Other Roots option");
    if ([mode isEqualToString:@"off"])
        return NSLocalizedString(@"Off", @"Programs From Other Roots option");
    return NSLocalizedString(@"Run Inside Their Root", @"Programs From Other Roots option");
}

- (UITableViewCell *)_foreignExecCell {
    UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleValue1 reuseIdentifier:nil];
    cell.textLabel.text = NSLocalizedString(@"Programs From Other Roots", @"Settings row label");
    cell.detailTextLabel.text = ISHForeignExecTitle(UserPreferences.shared.foreignExecMode);
    cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
    return cell;
}

- (void)_showForeignExecPickerFromCell:(UITableViewCell *)cell {
    ISHActionSheet *alert = [ISHActionSheet actionSheetWithTitle:NSLocalizedString(@"Programs From Other Roots", @"Action sheet title")
                                                         message:NSLocalizedString(@"How a program from another installed filesystem, such as /AOK/roots/Devuan6-arm64/usr/bin/tmux, runs when its libraries are not in this one. Applies to the next program started.", @"Programs From Other Roots action sheet message")];
    NSString *current = UserPreferences.shared.foreignExecMode;
    for (NSString *mode in @[@"root", @"libs", @"off"]) {
        NSString *title = ISHForeignExecTitle(mode);
        if ([mode isEqualToString:current])
            title = [NSString stringWithFormat:NSLocalizedString(@"%@  Current", @"Action sheet option marked as the current choice; %@ is the option name"), title];
        [alert addActionWithTitle:title
                            style:UIAlertActionStyleDefault
                          handler:^(__unused UIAlertAction *action) {
            UserPreferences.shared.foreignExecMode = mode;
            [self.tableView reloadData];
        }];
    }
    [alert addActionWithTitle:NSLocalizedString(@"Cancel", @"Action sheet button") style:UIAlertActionStyleCancel handler:nil];
    [alert presentFromViewController:self sourceView:cell sourceRect:cell.bounds];
}

- (UITableViewCell *)_loginAsDefaultUserCell {
    UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:nil];
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
    cell.textLabel.text = NSLocalizedString(@"Open Everything as Default User", @"Settings switch label");
    UISwitch *enabledSwitch = [UISwitch new];
    enabledSwitch.on = UserPreferences.shared.shouldLoginAsDefaultUser;
    [enabledSwitch addTarget:self action:@selector(loginAsDefaultUserChanged:) forControlEvents:UIControlEventValueChanged];
    cell.accessoryView = enabledSwitch;
    self.loginAsDefaultUserSwitch = enabledSwitch;
    return cell;
}

- (UITableViewCell *)_llmEnabledCell {
    UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:nil];
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
    cell.textLabel.text = NSLocalizedString(@"LLM Client", @"Settings switch label");
    UISwitch *enabledSwitch = [UISwitch new];
    enabledSwitch.on = UserPreferences.shared.shouldEnableLLMClient;
    [enabledSwitch addTarget:self action:@selector(llmClientEnabledChanged:) forControlEvents:UIControlEventValueChanged];
    cell.accessoryView = enabledSwitch;
    self.llmClientSwitch = enabledSwitch;
    return cell;
}

- (UITableViewCell *)_llmSettingsCell {
    UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleValue1 reuseIdentifier:nil];
    cell.textLabel.text = NSLocalizedString(@"LLM Settings", @"Settings row label");
    cell.detailTextLabel.text = UserPreferences.shared.llmModel;
    cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
    return cell;
}

- (UITableViewCell *)_jitCacheCell {
    UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:nil];
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
    cell.textLabel.text = NSLocalizedString(@"Cache Translated Code (arm64)", @"Settings switch label");
    cell.textLabel.adjustsFontSizeToFitWidth = YES;
    UISwitch *enabledSwitch = [UISwitch new];
    enabledSwitch.on = UserPreferences.shared.shouldEnableJITCache;
    [enabledSwitch addTarget:self action:@selector(jitCacheChanged:) forControlEvents:UIControlEventValueChanged];
    cell.accessoryView = enabledSwitch;
    return cell;
}

- (UITableViewCell *)_shortcutsRunCommandsCell {
    UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:nil];
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
    cell.textLabel.text = NSLocalizedString(@"Allow Shortcuts to Run Commands", @"Settings switch label");
    UISwitch *enabledSwitch = [UISwitch new];
    enabledSwitch.on = UserPreferences.shared.shortcutsRunCommandsEnabled;
    [enabledSwitch addTarget:self action:@selector(shortcutsRunCommandsChanged:) forControlEvents:UIControlEventValueChanged];
    cell.accessoryView = enabledSwitch;
    self.shortcutsRunCommandsSwitch = enabledSwitch;
    return cell;
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    if (indexPath.section == [self _foreignExecSectionIndex]) {
        [self _showForeignExecPickerFromCell:[tableView cellForRowAtIndexPath:indexPath]];
        [tableView deselectRowAtIndexPath:indexPath animated:YES];
        return;
    }
    if (indexPath.section == [self _desktopSectionIndex]) {
        UIViewController *setup = ISHCreateDesktopSetupViewController();
        if (self.ish_canPushSubpage) {
            [self.navigationController pushViewController:setup animated:YES];
        } else {
            UINavigationController *navigationController = [[UINavigationController alloc] initWithRootViewController:setup];
            ISHConfigureLLMSettingsNavigationController(navigationController);
            [self presentViewController:navigationController animated:YES completion:nil];
        }
        [tableView deselectRowAtIndexPath:indexPath animated:YES];
        return;
    }
    if (indexPath.section == [self _llmSectionIndex]) {
        if (indexPath.row == 1) {
            UIViewController *settingsViewController = ISHCreateLLMSettingsViewController();
            if (self.ish_canPushSubpage) {
                [self.navigationController pushViewController:settingsViewController animated:YES];
            } else {
                UINavigationController *navigationController = [[UINavigationController alloc] initWithRootViewController:settingsViewController];
                ISHConfigureLLMSettingsNavigationController(navigationController);
                [self presentViewController:navigationController animated:YES completion:nil];
            }
        }
        [tableView deselectRowAtIndexPath:indexPath animated:YES];
        return;
    }
    UITableViewCell *cell = [tableView cellForRowAtIndexPath:indexPath];
    if (cell == self.sendFeedback) {
        [UIApplication openURL:@"https://github.com/emkey1/ish-AOK/issues/new"];
    } else if (cell == self.diagnosticsCell) {
        [self showDiagnostics:cell];
    } else if (cell == self.initialWindowCell) {
        [self _showInitialWindowPickerFromCell:cell];
    } else if (cell == self.openGithub) {
        [UIApplication openURL:@"https://github.com/emkey1/ish-AOK"];
    } else if (cell == self.openDiscord) {
        [UIApplication openURL:@"https://discord.gg/RkdBXHMbgc"];
    } else if (cell == self.exportContainerCell) {
        // copy the files to the app container so they can be extracted from iTunes file sharing
        NSURL *container = ContainerURL();
        NSURL *documents = [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask][0];
        [NSFileManager.defaultManager removeItemAtURL:[documents URLByAppendingPathComponent:@"roots copy"] error:nil];
        [NSFileManager.defaultManager copyItemAtURL:[container URLByAppendingPathComponent:@"roots"]
                                              toURL:[documents URLByAppendingPathComponent:@"roots copy"]
                                              error:nil];
    } else if (cell == self.resetMountsCell) {
        iosfs_clear_all_bookmarks();
    } else if (cell == self.customDnsCell) {
        [self _showCustomDnsServersEditorFromCell:cell];
    } else if (cell == self.keyboardToolbarCell) {
        UIViewController *toolbarKeys = ISHCreateToolbarKeysViewController();
        if (self.ish_canPushSubpage) {
            [self.navigationController pushViewController:toolbarKeys animated:YES];
        } else {
            UINavigationController *navigationController =
                [[UINavigationController alloc] initWithRootViewController:toolbarKeys];
            ISHConfigureLLMSettingsNavigationController(navigationController);
            [self presentViewController:navigationController animated:YES completion:nil];
        }
    }
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
}

- (void)_showCustomDnsServersEditorFromCell:(UITableViewCell *)cell {
    (void) cell;
    UIAlertController *alert =
        [UIAlertController alertControllerWithTitle:NSLocalizedString(@"Custom DNS Servers", @"Alert title")
                                            message:NSLocalizedString(@"Space- or comma-separated nameserver IPs written into the guest's /etc/resolv.conf on every refresh. Leave blank to follow this device's network-provided DNS automatically, or pick Don't Manage to leave the file alone entirely, for a root that runs its own resolver.", @"Custom DNS Servers alert message")
                                     preferredStyle:UIAlertControllerStyleAlert];
    [alert addTextFieldWithConfigurationHandler:^(UITextField *textField) {
        textField.text = UserPreferences.shared.customDnsServers;
        textField.placeholder = NSLocalizedString(@"e.g. 1.1.1.1 1.0.0.1", @"Custom DNS Servers field placeholder");
        textField.clearButtonMode = UITextFieldViewModeWhileEditing;
        textField.autocapitalizationType = UITextAutocapitalizationTypeNone;
        textField.autocorrectionType = UITextAutocorrectionTypeNo;
        textField.spellCheckingType = UITextSpellCheckingTypeNo;
        textField.keyboardType = UIKeyboardTypeURL;
    }];
    [alert addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"Cancel", @"Alert button") style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"Don't Manage", @"Custom DNS Servers button that leaves resolv.conf alone") style:UIAlertActionStyleDestructive handler:^(__unused UIAlertAction *action) {
        // Mutually exclusive from here: picking this drops any pinned list, so
        // the cell never shows servers it is no longer writing.
        UserPreferences.shared.customDnsServers = @"";
        UserPreferences.shared.shouldDisableResolvConfRewrite = YES;
        [self _updateUI];
        AppDelegate *dnsOffDelegate = (AppDelegate *) UIApplication.sharedApplication.delegate;
        if ([dnsOffDelegate isKindOfClass:AppDelegate.class]) {
            [dnsOffDelegate refreshDnsConfiguration];
        }
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"Save", @"Alert button") style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
        NSString *value = [alert.textFields.firstObject.text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet] ?: @"";
        UserPreferences.shared.customDnsServers = value;
        UserPreferences.shared.shouldDisableResolvConfRewrite = NO;
        [self _updateUI];
        AppDelegate *appDelegate = (AppDelegate *) UIApplication.sharedApplication.delegate;
        if ([appDelegate isKindOfClass:AppDelegate.class]) {
            [appDelegate refreshDnsConfiguration];
        }
    }]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (NSString *)_initialWindowPreferenceValue {
    NSString *value = [NSUserDefaults.standardUserDefaults stringForKey:kPreferenceInitialWindowKey];
    if ([value isEqualToString:ISHInitialWindowWorkspaceValue])
        return value;
    if ([value isEqualToString:ISHInitialWindowChooseFilesystemValue])
        return value;
    if ([value isEqualToString:ISHInitialWindowWaylandValue])
        return value;
    if ([value isEqualToString:@"session-shell"])
        return value;
    return @"terminal";
}

- (NSString *)_initialWindowTitle {
    if ([[self _initialWindowPreferenceValue] isEqualToString:ISHInitialWindowWorkspaceValue])
        return NSLocalizedString(@"Workspace", @"Startup Mode option");
    if ([[self _initialWindowPreferenceValue] isEqualToString:ISHInitialWindowChooseFilesystemValue])
        return NSLocalizedString(@"Choose Filesystem", @"Startup Mode option");
    if ([[self _initialWindowPreferenceValue] isEqualToString:ISHInitialWindowWaylandValue])
        return NSLocalizedString(@"Wayland Display", @"Startup Mode option");
    if ([[self _initialWindowPreferenceValue] isEqualToString:@"session-shell"])
        return NSLocalizedString(@"Session Shell (pts/1)", @"Startup Mode option");
    return NSLocalizedString(@"Plain Terminal", @"Startup Mode option");
}

- (void)_setInitialWindowPreferenceValue:(NSString *)value {
    [NSUserDefaults.standardUserDefaults setObject:value forKey:kPreferenceInitialWindowKey];
    [self _updateUI];
}

- (void)_showInitialWindowPickerFromCell:(UITableViewCell *)cell {
    ISHActionSheet *alert = [ISHActionSheet actionSheetWithTitle:NSLocalizedString(@"Startup Mode", @"Action sheet title")
                                                         message:NSLocalizedString(@"Choose whether new app launches open the Workspace, the Wayland Display, show a filesystem chooser, or open a terminal.", @"Startup Mode action sheet message")];

    NSString *currentValue = [self _initialWindowPreferenceValue];
    NSString *workspaceTitle = [currentValue isEqualToString:ISHInitialWindowWorkspaceValue]
        ? NSLocalizedString(@"Workspace  Current", @"Startup Mode option, marked current")
        : NSLocalizedString(@"Workspace", @"Startup Mode option");
    NSString *chooseFilesystemTitle = [currentValue isEqualToString:ISHInitialWindowChooseFilesystemValue]
        ? NSLocalizedString(@"Choose Filesystem  Current", @"Startup Mode option, marked current")
        : NSLocalizedString(@"Choose Filesystem", @"Startup Mode option");
    NSString *waylandTitle = [currentValue isEqualToString:ISHInitialWindowWaylandValue]
        ? NSLocalizedString(@"Wayland Display  Current", @"Startup Mode option, marked current")
        : NSLocalizedString(@"Wayland Display", @"Startup Mode option");
    NSString *terminalTitle = [currentValue isEqualToString:@"terminal"]
        ? NSLocalizedString(@"Plain Terminal  Current", @"Startup Mode option, marked current")
        : NSLocalizedString(@"Plain Terminal", @"Startup Mode option");
    NSString *sessionTitle = [currentValue isEqualToString:@"session-shell"]
        ? NSLocalizedString(@"Session Shell (pts/1)  Current", @"Startup Mode option, marked current")
        : NSLocalizedString(@"Session Shell (pts/1)", @"Startup Mode option");

    [alert addActionWithTitle:workspaceTitle
                        style:UIAlertActionStyleDefault
                      handler:^(__unused UIAlertAction *action) {
        [self _setInitialWindowPreferenceValue:ISHInitialWindowWorkspaceValue];
    }];
    [alert addActionWithTitle:waylandTitle
                        style:UIAlertActionStyleDefault
                      handler:^(__unused UIAlertAction *action) {
        [self _setInitialWindowPreferenceValue:ISHInitialWindowWaylandValue];
    }];
    [alert addActionWithTitle:chooseFilesystemTitle
                        style:UIAlertActionStyleDefault
                      handler:^(__unused UIAlertAction *action) {
        [self _setInitialWindowPreferenceValue:ISHInitialWindowChooseFilesystemValue];
    }];
    [alert addActionWithTitle:terminalTitle
                        style:UIAlertActionStyleDefault
                      handler:^(__unused UIAlertAction *action) {
        [self _setInitialWindowPreferenceValue:@"terminal"];
    }];
    [alert addActionWithTitle:sessionTitle
                        style:UIAlertActionStyleDefault
                      handler:^(__unused UIAlertAction *action) {
        [self _setInitialWindowPreferenceValue:@"session-shell"];
    }];
    [alert addActionWithTitle:NSLocalizedString(@"Cancel", @"Action sheet button")
                        style:UIAlertActionStyleCancel
                      handler:nil];

    [alert presentFromViewController:self sourceView:cell sourceRect:cell.bounds];
}

- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    if (section == [self _userAccountSectionIndex]) {
        NSString *accountName = [AppDelegate defaultUserAccountName];
        return accountName.length != 0
            ? [NSString stringWithFormat:NSLocalizedString(@"When enabled, new Workspace terminals, app sessions, and headless commands (LLM Chat's shell tool, Shortcuts' Run Command) run as \"%@\" (UID %d) instead of root. The Session Shell always signs in as root.", @"Default User section footer; %@ is an account name, %d its UID"),
               accountName, ISHDefaultUserAccountUID]
            : [NSString stringWithFormat:NSLocalizedString(@"When enabled, new Workspace terminals, app sessions, and headless commands (LLM Chat's shell tool, Shortcuts' Run Command) run as the UID %d account instead of root -- but this filesystem doesn't have one yet. The Session Shell always signs in as root.", @"Default User section footer; %d is a UID"),
               ISHDefaultUserAccountUID];
    }
    if (section == [self _llmSectionIndex])
        return UserPreferences.shared.shouldEnableLLMClient
            ? NSLocalizedString(@"When enabled, LLM Chat appears in Switch Terminal and Workspace menus.", @"LLM Client section footer")
            : NSLocalizedString(@"Enable to show an OpenAI-compatible LLM client in terminal and Workspace menus.", @"LLM Client section footer");
    if (section == [self _shortcutsSectionIndex])
        return NSLocalizedString(@"When enabled, the Shortcuts app's \"Run Command\" action can run shell commands in the guest system without opening iSH-AOK.", @"Shortcuts section footer");
    if (section == [self _foreignExecSectionIndex])
        return NSLocalizedString(@"Run Inside Their Root: the program sees that filesystem's own files, as if you had chrooted into it (mount-root.sh). "
               @"Use Their Libraries Here: it sees this filesystem's files and your home, borrowing only its libraries; programs that need data files of their own may not work. "
               @"Off: it fails, as on Linux.", @"Other Filesystems section footer; option names must match their translations");
    if (section == [self _jitCacheSectionIndex])
        return NSLocalizedString(@"Keeps the code iSH-AOK has translated after the program that needed it exits, so the next program that loads the same code starts faster. It helps scripts and builds that start many short programs (about a fifth faster in tests) and does little for one long-running program.\n\n"
               @"A cached translation is found by the address its code was loaded at, so while this is on, address space layout randomization (ASLR) is off in the Linux system: every program and library loads at the same addresses each time. ASLR does not prevent bugs; it makes a memory-corruption bug harder to turn into an attack, because the attacker cannot know where anything is. With it off, such a bug in a program you run here -- a network service above all -- is easier to exploit. iOS's own protection of the app is not affected.\n\n"
               @"Applies to programs started after the change, on arm64 filesystems. Uses up to 96 MB of memory.", @"Translated Code section footer: what the translation cache does and that it turns ASLR off");
    if (section == [self _desktopSectionIndex])
        return NSLocalizedString(@"Installs the desktop the Wayland display shows, in the filesystem that is running, and chooses which one it starts.", @"Wayland Desktop section footer in Settings");
    if (section == 1) { // filesystems / upgrade
        if (!FsIsManaged()) {
            return NSLocalizedString(@"The current filesystem is not managed by iSH-AOK.", @"Filesystem section footer");
        } else if (!FsNeedsRepositoryUpdate()) {
            return [NSString stringWithFormat:NSLocalizedString(@"The current filesystem is using %s, which is the latest version.", @"Filesystem section footer; %s is a version"), NEWEST_APK_VERSION];
        } else {
            return [NSString stringWithFormat:NSLocalizedString(@"An upgrade to %s is available.", @"Filesystem section footer; %s is a version"), NEWEST_APK_VERSION];
        }
    }
    return [super tableView:tableView titleForFooterInSection:section];
}

- (NSString *)tableView:(UITableView *)tableView titleForHeaderInSection:(NSInteger)section {
    if (section == [self _userAccountSectionIndex])
        return NSLocalizedString(@"Default User", @"Settings section header");
    if (section == [self _llmSectionIndex])
        return NSLocalizedString(@"LLM Client", @"Settings section header");
    if (section == [self _shortcutsSectionIndex])
        return NSLocalizedString(@"Shortcuts", @"Settings section header");
    if (section == [self _foreignExecSectionIndex])
        return NSLocalizedString(@"Other Filesystems", @"Settings section header");
    if (section == [self _jitCacheSectionIndex])
        return NSLocalizedString(@"Translated Code", @"Settings section header");
    if (section == [self _desktopSectionIndex])
        return NSLocalizedString(@"Wayland Desktop", @"Desktop setup page title");
    return [super tableView:tableView titleForHeaderInSection:section];
}

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView {
    return [self _visibleStoryboardSectionCount] + 6;
}

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    if (section == [self _desktopSectionIndex])
        return 1;
    if (section == [self _userAccountSectionIndex])
        return 1;
    if (section == [self _llmSectionIndex])
        return UserPreferences.shared.shouldEnableLLMClient ? 2 : 1;
    if (section == [self _shortcutsSectionIndex])
        return 1;
    if (section == [self _foreignExecSectionIndex])
        return 1;
    if (section == [self _jitCacheSectionIndex])
        return 1;
    return [super tableView:tableView numberOfRowsInSection:section];
}

// At the text size of the Workspace window Settings is in; see
// WorkspaceTextScaledPage. Anywhere else the rows are left as they are. The
// version line under the table stays as it is: a table footer view with a fixed
// 44-point frame, which the table does not re-measure. So do the section headers
// and footers, which UIKit makes and measures from its own font.
- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    UITableViewCell *cell = [self unscaledTableView:tableView cellForRowAtIndexPath:indexPath];
    ISHWorkspaceScaleTableViewCell(cell, ISHWorkspaceTextScaleForViewController(self));
    return cell;
}

- (void)workspaceTextScaleDidChange {
    ISHWorkspaceRescaleTableView(self.tableView, ISHWorkspaceTextScaleForViewController(self));
}

- (UITableViewCell *)unscaledTableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    if (indexPath.section == [self _userAccountSectionIndex])
        return [self _loginAsDefaultUserCell];
    if (indexPath.section == [self _llmSectionIndex]) {
        if (indexPath.row == 0)
            return [self _llmEnabledCell];
        return [self _llmSettingsCell];
    }
    if (indexPath.section == [self _shortcutsSectionIndex])
        return [self _shortcutsRunCommandsCell];
    if (indexPath.section == [self _foreignExecSectionIndex])
        return [self _foreignExecCell];
    if (indexPath.section == [self _jitCacheSectionIndex])
        return [self _jitCacheCell];
    if (indexPath.section == [self _desktopSectionIndex])
        return [self _desktopSetupCell];
    return [super tableView:tableView cellForRowAtIndexPath:indexPath];
}

// iOS 27's UIKit bounds-checks the storyboard's static-section array. Our
// appended sections (user account, LLM) live at indexes >= the static section
// count, so any UITableViewController static *layout* delegate method reached
// via [super ...] for one of them indexes out of bounds -> "index N beyond
// bounds [0..N-1]" (reloadData asks the static layout for the appended
// section's heights). Older UIKit tolerated it; 27 throws. Guard these the
// same way we guard the data-source methods above, generalized to any
// section past the static ones rather than one specific index, since there
// are now two appended sections sharing this guard. heightForRow/
// indentationLevel are part of the static-cell support (proven by the
// [super ...] calls above); header/footer heights may be table defaults, so
// only forward those when super implements them.
- (BOOL)_isAppendedSection:(NSInteger)section {
    return section >= [self _visibleStoryboardSectionCount];
}

- (CGFloat)tableView:(UITableView *)tableView heightForRowAtIndexPath:(NSIndexPath *)indexPath {
    CGFloat height = [self _isAppendedSection:indexPath.section]
        ? 44
        : [super tableView:tableView heightForRowAtIndexPath:indexPath];
    // The storyboard's switch and command rows centre their label and control
    // with nothing above or below, so they stay 44 points tall whatever the text
    // size, and larger text would be clipped.
    return ISHWorkspaceTextScaledRowHeight(height, ISHWorkspaceTextScaleForViewController(self));
}

- (NSInteger)tableView:(UITableView *)tableView indentationLevelForRowAtIndexPath:(NSIndexPath *)indexPath {
    if ([self _isAppendedSection:indexPath.section])
        return 0;
    return [super tableView:tableView indentationLevelForRowAtIndexPath:indexPath];
}

- (CGFloat)tableView:(UITableView *)tableView heightForHeaderInSection:(NSInteger)section {
    if ([self _isAppendedSection:section])
        return UITableViewAutomaticDimension;
    if ([UITableViewController instancesRespondToSelector:_cmd])
        return [super tableView:tableView heightForHeaderInSection:section];
    return UITableViewAutomaticDimension;
}

- (CGFloat)tableView:(UITableView *)tableView heightForFooterInSection:(NSInteger)section {
    if ([self _isAppendedSection:section])
        return UITableViewAutomaticDimension;
    if ([UITableViewController instancesRespondToSelector:_cmd])
        return [super tableView:tableView heightForFooterInSection:section];
    return UITableViewAutomaticDimension;
}

// Without an override here, UIKit falls back to its own default header/footer
// view synthesis (-[UITableViewDataSource tableView:viewForHeaderInSection:]),
// which hits the same static-section-array bounds check as the [super ...]
// calls above -- crashing on an appended section ("index N beyond bounds
// [0..N-1]"). Returning nil lets UITableView fall back to the plain
// title-based header/footer from titleForHeaderInSection/titleForFooterInSection.
- (UIView *)tableView:(UITableView *)tableView viewForHeaderInSection:(NSInteger)section {
    if ([self _isAppendedSection:section])
        return nil;
    if ([UITableViewController instancesRespondToSelector:_cmd])
        return [super tableView:tableView viewForHeaderInSection:section];
    return nil;
}

- (UIView *)tableView:(UITableView *)tableView viewForFooterInSection:(NSInteger)section {
    if ([self _isAppendedSection:section])
        return nil;
    if ([UITableViewController instancesRespondToSelector:_cmd])
        return [super tableView:tableView viewForFooterInSection:section];
    return nil;
}

- (IBAction)disableDimmingChanged:(id)sender {
    UserPreferences.shared.shouldDisableDimming = self.disableDimmingSwitch.on;
}

- (IBAction)enableCryptoAccelChanged:(id)sender {
    UserPreferences.shared.shouldEnableCryptoAccel = self.enableCryptoAccelSwitch.on;
}

- (IBAction)enablePixAccelChanged:(id)sender {
    UserPreferences.shared.shouldEnablePixAccel = self.enablePixAccelSwitch.on;
}

- (IBAction)enableHLEChanged:(id)sender {
    UserPreferences.shared.shouldEnableHLE = self.enableHLESwitch.on;
}

- (IBAction)enableMulticoreChanged:(id)sender {
    UserPreferences.shared.shouldEnableMulticore = self.enableMulticoreSwitch.on;
}

- (IBAction)enableExtraLockingChanged:(id)sender {
    UserPreferences.shared.shouldEnableExtraLocking = self.enableExtraLockingSwitch.on;
}

- (void)llmClientEnabledChanged:(UISwitch *)sender {
    UserPreferences.shared.shouldEnableLLMClient = sender.on;
    [self.tableView reloadData];
}

- (void)loginAsDefaultUserChanged:(UISwitch *)sender {
    UserPreferences.shared.shouldLoginAsDefaultUser = sender.on;
}

- (void)jitCacheChanged:(UISwitch *)sender {
    UserPreferences.shared.shouldEnableJITCache = sender.on;
}

- (void)shortcutsRunCommandsChanged:(UISwitch *)sender {
    UserPreferences.shared.shortcutsRunCommandsEnabled = sender.on;
}

//- (IBAction)shouldLockSleepNanoseconds:(id)sender {
//    UserPreferences.shared.shouldLockSleepNanoseconds = self.shouldLockSleepNanosecondsSwitch.on;
//}

- (IBAction)textBoxSubmit:(id)sender {
    [sender resignFirstResponder];
}

// Splitting a command line typed into a text field. componentsSeparatedByString:@" "
// was wrong in two ways that both end in an unusable session:
//
//   - it splits on SINGLE spaces, so a trailing space -- which a mobile keyboard
//     offers freely -- turns "/bin/login -f root " into four components, the last
//     of them empty. That is no longer the default login command as far as
//     ISHCommandIsDefaultLogin is concerned, and login is handed "" as the
//     username. Two spaces between arguments do the same thing mid-line.
//   - it never returns an empty array: clearing the field stores @[@""], one
//     empty string. registerDefaults only supplies a value for an ABSENT key, so
//     that is not "back to the default", it is a permanently broken command that
//     survives a restart.
//
// Split on whitespace, drop the empties, and treat "nothing left" as a request
// for the default by removing the key entirely.
static NSArray<NSString *> *ISHCommandFromFieldText(NSString *text) {
    NSMutableArray<NSString *> *words = [NSMutableArray array];
    for (NSString *word in [text componentsSeparatedByCharactersInSet:
                            NSCharacterSet.whitespaceAndNewlineCharacterSet]) {
        if (word.length != 0)
            [words addObject:word];
    }
    return words;
}

- (IBAction)launchCommandChanged:(id)sender {
    NSArray<NSString *> *command = ISHCommandFromFieldText(self.launchCommandField.text);
    if (command.count == 0)
        [UserPreferences.shared resetLaunchCommand];
    else
        UserPreferences.shared.launchCommand = command;
}

- (IBAction)bootCommandChanged:(id)sender {
    NSArray<NSString *> *command = ISHCommandFromFieldText(self.bootCommandField.text);
    if (command.count == 0)
        [UserPreferences.shared resetBootCommand];
    else
        UserPreferences.shared.bootCommand = command;
}

@end

#pragma mark - Keyboard Toolbar (#609)

// The extra-keys bar's keys, arranged by hand. One section per place on the bar
// (ISHToolbarKeyGroupLeft, ISHToolbarKeyGroupCenter), always in editing mode so a
// key drags within and between them and deletes with the usual control; a third
// section holds Add Key and Reset. Every change is saved at once, and the bar
// follows it live (-[TerminalViewController _applyToolbarKeys]).
@implementation ISHToolbarKeysViewController {
    NSMutableArray<NSMutableArray<NSDictionary *> *> *_groups;   // left, center
}

static const NSInteger ISHToolbarKeysActionsSection = 2;

- (instancetype)init {
    self = [super initWithStyle:UITableViewStyleInsetGrouped];
    if (self != nil)
        self.title = NSLocalizedString(@"Keyboard Toolbar", @"Keyboard toolbar settings title");
    return self;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    ISHSizeTableSectionTitlesOnMac(self.tableView);
    [self _load];
    self.tableView.allowsSelectionDuringEditing = YES;
    [self setEditing:YES animated:NO];
}

- (void)_load {
    NSDictionary<NSString *, NSArray *> *layout = ISHToolbarKeysCurrentLayout();
    _groups = [NSMutableArray array];
    for (NSString *group in @[ISHToolbarKeyGroupLeft, ISHToolbarKeyGroupCenter])
        [_groups addObject:[layout[group] mutableCopy] ?: [NSMutableArray array]];
}

- (void)_save {
    UserPreferences.shared.toolbarKeys = @{
        ISHToolbarKeyGroupLeft: [_groups[0] copy],
        ISHToolbarKeyGroupCenter: [_groups[1] copy],
    };
}

- (BOOL)_isKeySection:(NSInteger)section {
    return section == 0 || section == 1;
}

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView {
    return 3;
}

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    return [self _isKeySection:section] ? (NSInteger) _groups[section].count : 2;
}

- (NSString *)tableView:(UITableView *)tableView titleForHeaderInSection:(NSInteger)section {
    if (section == 0)
        return NSLocalizedString(@"Left", @"Keyboard toolbar settings section header");
    if (section == 1)
        return NSLocalizedString(@"Center", @"Keyboard toolbar settings section header");
    return nil;
}

- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    if (section == 0)
        return NSLocalizedString(@"From the bar's left edge.", @"Keyboard toolbar Left section footer");
    if (section == 1)
        return NSLocalizedString(@"Centered on the bar. On an iPhone held upright, a key set to hide on a narrow bar is left off.", @"Keyboard toolbar Center section footer");
    return NSLocalizedString(@"A custom key types its text as written. \\e is Escape, \\t Tab, \\n Return, and \\xHH any "
           @"character up to 7F -- \\x03 is Control-C. \\\\ is a backslash. With Control on, a key of one "
           @"character sends that character's control code.\n\n"
           @"Settings, Files, Paste and the other buttons at the bar's right end stay where they are.", @"Keyboard toolbar settings footer; keep the escape sequences unchanged");
}

- (NSString *)_symbolForItem:(NSDictionary *)item {
    NSString *builtin = item[ISHToolbarKeyBuiltin];
    return builtin != nil ? ISHToolbarBuiltinKeySymbol(builtin) : item[ISHToolbarKeyTitle];
}

- (NSString *)_nameForItem:(NSDictionary *)item {
    NSString *builtin = item[ISHToolbarKeyBuiltin];
    return builtin != nil ? ISHToolbarBuiltinKeyName(builtin)
                          : [NSString stringWithFormat:NSLocalizedString(@"Types %@", @"Keyboard toolbar custom key description; %@ is the text it sends"), item[ISHToolbarKeySends]];
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    UITableViewCell *cell;
    if ([self _isKeySection:indexPath.section]) {
        NSDictionary *item = _groups[indexPath.section][indexPath.row];
        cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:nil];
        cell.textLabel.text = [NSString stringWithFormat:@"%@    %@", [self _symbolForItem:item],
                               item[ISHToolbarKeyBuiltin] != nil ? [self _nameForItem:item] : @""];
        BOOL narrow = [item[ISHToolbarKeyNarrow] boolValue];
        NSMutableArray<NSString *> *detail = [NSMutableArray array];
        if (item[ISHToolbarKeyBuiltin] == nil)
            [detail addObject:[self _nameForItem:item]];
        if (!narrow)
            [detail addObject:NSLocalizedString(@"Hidden on a narrow bar", @"Keyboard toolbar key detail")];
        cell.detailTextLabel.text = [detail componentsJoinedByString:@" \u00b7 "];
        cell.detailTextLabel.textColor = UIColor.secondaryLabelColor;
        cell.editingAccessoryType = UITableViewCellAccessoryDetailButton;
        NSString *spokenName = item[ISHToolbarKeyBuiltin] != nil ? [self _nameForItem:item]
                                              : [NSString stringWithFormat:@"%@, %@", item[ISHToolbarKeyTitle],
                                                 [self _nameForItem:item]];
        cell.accessibilityLabel = narrow ? spokenName
            : [NSString stringWithFormat:NSLocalizedString(@"%@, hidden on a narrow bar", @"Keyboard toolbar key accessibility label; %@ is the key name"), spokenName];
    } else {
        cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:nil];
        if (indexPath.row == 0) {
            cell.textLabel.text = NSLocalizedString(@"Add Key\u2026", @"Keyboard toolbar settings row");
            cell.textLabel.textColor = self.view.tintColor;
        } else {
            cell.textLabel.text = NSLocalizedString(@"Reset to Default", @"Keyboard toolbar settings row");
            cell.textLabel.textColor = UIColor.systemRedColor;
        }
        cell.accessibilityTraits |= UIAccessibilityTraitButton;
    }
    ISHWorkspaceScaleTableViewCell(cell, ISHWorkspaceTextScaleForViewController(self));
    return cell;
}

- (void)workspaceTextScaleDidChange {
    ISHWorkspaceRescaleTableView(self.tableView, ISHWorkspaceTextScaleForViewController(self));
}

- (BOOL)tableView:(UITableView *)tableView canEditRowAtIndexPath:(NSIndexPath *)indexPath {
    return [self _isKeySection:indexPath.section];
}

- (BOOL)tableView:(UITableView *)tableView canMoveRowAtIndexPath:(NSIndexPath *)indexPath {
    return [self _isKeySection:indexPath.section];
}

- (UITableViewCellEditingStyle)tableView:(UITableView *)tableView editingStyleForRowAtIndexPath:(NSIndexPath *)indexPath {
    return [self _isKeySection:indexPath.section] ? UITableViewCellEditingStyleDelete : UITableViewCellEditingStyleNone;
}

- (BOOL)tableView:(UITableView *)tableView shouldIndentWhileEditingRowAtIndexPath:(NSIndexPath *)indexPath {
    return [self _isKeySection:indexPath.section];
}

// A key goes anywhere in Left or Center, and never among the action rows.
- (NSIndexPath *)tableView:(UITableView *)tableView
    targetIndexPathForMoveFromRowAtIndexPath:(NSIndexPath *)source
                         toProposedIndexPath:(NSIndexPath *)proposed {
    if ([self _isKeySection:proposed.section])
        return proposed;
    NSInteger last = (NSInteger) _groups[1].count - (source.section == 1 ? 1 : 0);
    return [NSIndexPath indexPathForRow:MAX(last, 0) inSection:1];
}

- (void)tableView:(UITableView *)tableView moveRowAtIndexPath:(NSIndexPath *)source toIndexPath:(NSIndexPath *)destination {
    NSDictionary *item = _groups[source.section][source.row];
    [_groups[source.section] removeObjectAtIndex:source.row];
    [_groups[destination.section] insertObject:item atIndex:destination.row];
    [self _save];
}

- (void)tableView:(UITableView *)tableView commitEditingStyle:(UITableViewCellEditingStyle)editingStyle forRowAtIndexPath:(NSIndexPath *)indexPath {
    if (editingStyle != UITableViewCellEditingStyleDelete || ![self _isKeySection:indexPath.section])
        return;
    [_groups[indexPath.section] removeObjectAtIndex:indexPath.row];
    [self _save];
    [tableView deleteRowsAtIndexPaths:@[indexPath] withRowAnimation:UITableViewRowAnimationAutomatic];
}

- (void)tableView:(UITableView *)tableView accessoryButtonTappedForRowWithIndexPath:(NSIndexPath *)indexPath {
    [self tableView:tableView didSelectRowAtIndexPath:indexPath];
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    UITableViewCell *cell = [tableView cellForRowAtIndexPath:indexPath];
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    if (indexPath.section == ISHToolbarKeysActionsSection) {
        if (indexPath.row == 0)
            [self _showAddKeyMenuFrom:cell];
        else
            [self _confirmResetFrom:cell];
        return;
    }
    [self _showOptionsForKeyAt:indexPath from:cell];
}

- (void)_showOptionsForKeyAt:(NSIndexPath *)indexPath from:(UIView *)source {
    NSDictionary *item = _groups[indexPath.section][indexPath.row];
    BOOL narrow = [item[ISHToolbarKeyNarrow] boolValue];
    UIAlertController *sheet =
        [UIAlertController alertControllerWithTitle:[self _symbolForItem:item]
                                            message:[self _nameForItem:item]
                                     preferredStyle:UIAlertControllerStyleActionSheet];
    if (item[ISHToolbarKeyBuiltin] == nil) {
        [sheet addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"Edit\u2026", @"Keyboard toolbar key action") style:UIAlertActionStyleDefault handler:^(UIAlertAction *action) {
            [self _editCustomKey:item completion:^(NSDictionary *edited) {
                self->_groups[indexPath.section][indexPath.row] = edited;
                [self _save];
                [self.tableView reloadRowsAtIndexPaths:@[indexPath] withRowAnimation:UITableViewRowAnimationNone];
            }];
        }]];
    }
    NSString *toggle = narrow ? NSLocalizedString(@"Hide on a Narrow Bar", @"Keyboard toolbar key action") : NSLocalizedString(@"Show on a Narrow Bar", @"Keyboard toolbar key action");
    [sheet addAction:[UIAlertAction actionWithTitle:toggle style:UIAlertActionStyleDefault handler:^(UIAlertAction *action) {
        NSMutableDictionary *changed = [item mutableCopy];
        changed[ISHToolbarKeyNarrow] = @(!narrow);
        self->_groups[indexPath.section][indexPath.row] = changed;
        [self _save];
        [self.tableView reloadRowsAtIndexPaths:@[indexPath] withRowAnimation:UITableViewRowAnimationNone];
    }]];
    [sheet addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"Cancel", @"Action sheet button") style:UIAlertActionStyleCancel handler:nil]];
    [self anchorPopoverForAlertController:sheet toSource:source];
    [self presentViewController:sheet animated:YES completion:nil];
}

// Built-in keys not on the bar, then a custom one. A key comes back to the place
// it had by default: the left for Tab, Control, Escape and the arrows, the centre
// for the rest and for custom keys.
- (void)_showAddKeyMenuFrom:(UIView *)source {
    NSMutableSet<NSString *> *present = [NSMutableSet set];
    for (NSArray *group in _groups)
        for (NSDictionary *item in group)
            if (item[ISHToolbarKeyBuiltin] != nil)
                [present addObject:item[ISHToolbarKeyBuiltin]];
    NSSet *leftKeys = [NSSet setWithArray:@[@"tab", @"ctrl", @"esc", @"arrows"]];
    UIAlertController *sheet = [UIAlertController alertControllerWithTitle:NSLocalizedString(@"Add Key", @"Keyboard toolbar Add Key sheet title")
                                                                   message:nil
                                                            preferredStyle:UIAlertControllerStyleActionSheet];
    for (NSString *key in ISHToolbarBuiltinKeyIDs()) {
        if ([present containsObject:key])
            continue;
        NSString *title = [NSString stringWithFormat:@"%@  %@", ISHToolbarBuiltinKeySymbol(key), ISHToolbarBuiltinKeyName(key)];
        [sheet addAction:[UIAlertAction actionWithTitle:title style:UIAlertActionStyleDefault handler:^(UIAlertAction *action) {
            NSInteger section = [leftKeys containsObject:key] ? 0 : 1;
            [self _appendItem:@{ISHToolbarKeyBuiltin: key, ISHToolbarKeyNarrow: @YES} toSection:section];
        }]];
    }
    [sheet addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"Custom Key\u2026", @"Keyboard toolbar Add Key option") style:UIAlertActionStyleDefault handler:^(UIAlertAction *action) {
        [self _editCustomKey:nil completion:^(NSDictionary *created) {
            [self _appendItem:created toSection:1];
        }];
    }]];
    [sheet addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"Cancel", @"Action sheet button") style:UIAlertActionStyleCancel handler:nil]];
    [self anchorPopoverForAlertController:sheet toSource:source];
    [self presentViewController:sheet animated:YES completion:nil];
}

- (void)_appendItem:(NSDictionary *)item toSection:(NSInteger)section {
    [_groups[section] addObject:item];
    [self _save];
    NSIndexPath *path = [NSIndexPath indexPathForRow:(NSInteger) _groups[section].count - 1 inSection:section];
    [self.tableView insertRowsAtIndexPaths:@[path] withRowAnimation:UITableViewRowAnimationAutomatic];
}

- (void)_editCustomKey:(NSDictionary *)item completion:(void (^)(NSDictionary *))completion {
    UIAlertController *alert =
        [UIAlertController alertControllerWithTitle:item == nil ? NSLocalizedString(@"Custom Key", @"Custom key editor title") : NSLocalizedString(@"Edit Key", @"Custom key editor title")
                                            message:NSLocalizedString(@"What the key shows, and the text it types. "
                                                    @"\\e Escape, \\t Tab, \\n Return, \\xHH a character.", @"Custom key editor message; keep the escape sequences unchanged")
                                     preferredStyle:UIAlertControllerStyleAlert];
    for (int i = 0; i < 2; i++) {
        [alert addTextFieldWithConfigurationHandler:^(UITextField *field) {
            field.autocapitalizationType = UITextAutocapitalizationTypeNone;
            field.autocorrectionType = UITextAutocorrectionTypeNo;
            field.spellCheckingType = UITextSpellCheckingTypeNo;
            field.smartQuotesType = UITextSmartQuotesTypeNo;
            field.smartDashesType = UITextSmartDashesTypeNo;
            field.clearButtonMode = UITextFieldViewModeWhileEditing;
            if (i == 0) {
                field.placeholder = NSLocalizedString(@"Shows, e.g. ~", @"Custom key editor: label field placeholder");
                field.text = item[ISHToolbarKeyTitle];
                field.accessibilityLabel = NSLocalizedString(@"What the key shows", @"Custom key editor: label field");
            } else {
                field.placeholder = NSLocalizedString(@"Types, e.g. ls -la\\n", @"Custom key editor: text field placeholder");
                field.text = item[ISHToolbarKeySends];
                field.accessibilityLabel = NSLocalizedString(@"What the key types", @"Custom key editor: text field");
            }
        }];
    }
    [alert addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"Cancel", @"Alert button") style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"Save", @"Alert button") style:UIAlertActionStyleDefault handler:^(UIAlertAction *action) {
        NSString *title = [alert.textFields[0].text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        NSString *sends = alert.textFields[1].text ?: @"";
        if (title.length == 0 || sends.length == 0)
            return;   // nothing to show or nothing to type: not a key
        completion(@{ISHToolbarKeyTitle: title, ISHToolbarKeySends: sends,
                     ISHToolbarKeyNarrow: item[ISHToolbarKeyNarrow] ?: @YES});
    }]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)_confirmResetFrom:(UIView *)source {
    UIAlertController *alert =
        [UIAlertController alertControllerWithTitle:NSLocalizedString(@"Reset the Toolbar?", @"Reset keyboard toolbar alert title")
                                            message:NSLocalizedString(@"Tab, Control, Escape and the arrows at the left, and - . / : ! | "
                                                    @"in the center, as the toolbar came. Custom keys are removed.", @"Reset keyboard toolbar alert message")
                                     preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"Cancel", @"Alert button") style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"Reset", @"Reset keyboard toolbar alert button") style:UIAlertActionStyleDestructive handler:^(UIAlertAction *action) {
        UserPreferences.shared.toolbarKeys = nil;
        [self _load];
        [self.tableView reloadData];
    }]];
    [self presentViewController:alert animated:YES completion:nil];
}

@end

#pragma mark - Wayland desktop setup (GH #639)

// The desktop the Wayland display shows is ordinary packages in the guest,
// installed once per filesystem by /AOK/tools/setup-*.sh. That meant opening a
// terminal and typing `sudo sh /AOK/tools/setup-wayland.sh`, which is what the
// display's own failure message told people to do. These two pages run the
// same scripts from the app: one lists the desktops with what is installed
// and which one starts, the other runs a script and shows what it prints.

// The line the wrapper prints when the script is done, carrying its status.
// A guest process started with pipes reports no exit status of its own.
static NSString *const ISHDesktopScriptDoneMarker = @"__AOK_SCRIPT_DONE__";

// One queue for everything that starts, stops or runs a command in the guest
// from these pages: guest_process_spawn_user and run_guest_command_capture
// must not be called from the main thread or the shared pool.
static dispatch_queue_t ISHDesktopSetupQueue(void) {
    static dispatch_queue_t queue;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        queue = dispatch_queue_create("app.ish.desktop-setup", DISPATCH_QUEUE_SERIAL);
    });
    return queue;
}

// Runs one script as root in the guest and shows its output as it comes.
@interface ISHDesktopScriptViewController : UIViewController <UITextViewDelegate>
- (instancetype)initWithTitle:(NSString *)title command:(NSString *)command completion:(void (^_Nullable)(BOOL succeeded))completion;
@end

@implementation ISHDesktopScriptViewController {
    NSString *_command;
    void (^_completion)(BOOL);
    UITextView *_textView;
    UIActivityIndicatorView *_spinner;
    struct guest_process _process;
    BOOL _started, _running, _stopRequested;
    NSMutableData *_pending; // bytes read and not yet a whole line
    BOOL _idleTimerWasDisabled;
    // Progress (GH #646), read from what the script prints: the step from its
    // "==>" lines, and a bar for the package transaction under way.
    UILabel *_stepLabel, *_countLabel;
    UIProgressView *_progressView;
    NSInteger _apkDone, _apkTotal;
    NSString *_apkVerb;
    BOOL _apkDownloaded; // this transaction downloaded first (apk 3's --cache-predownload)
    NSInteger _aptInstall, _aptRemove, _aptGets, _aptUnpacks, _aptSetups, _aptRemoves;
    // The output follows what is printed until the person scrolls up, and
    // again once they scroll back to the end.
    BOOL _scrolledAway;
}

- (instancetype)initWithTitle:(NSString *)title command:(NSString *)command completion:(void (^)(BOOL))completion {
    self = [super initWithNibName:nil bundle:nil];
    if (self != nil) {
        self.title = title;
        _command = [command copy];
        _completion = [completion copy];
        _pending = [NSMutableData data];
    }
    return self;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.systemBackgroundColor;
    _textView = [UITextView new];
    _textView.translatesAutoresizingMaskIntoConstraints = NO;
    _textView.editable = NO;
    _textView.alwaysBounceVertical = YES;
    _textView.font = [UIFont monospacedSystemFontOfSize:12 weight:UIFontWeightRegular];
    _textView.textContainerInset = UIEdgeInsetsMake(10, 8, 10, 8);
    _textView.delegate = self;
    [self.view addSubview:_textView];

    // How far along it is: the script's own step, and how many packages of
    // the transaction under way are done. A script runs several transactions,
    // so the bar fills once for each; the step line says which.
    _stepLabel = [UILabel new];
    _stepLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];
    _stepLabel.adjustsFontForContentSizeCategory = YES;
    _stepLabel.numberOfLines = 2;
    _stepLabel.text = NSLocalizedString(@"Starting…", @"Wayland display status text");
    _countLabel = [UILabel new];
    _countLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleFootnote];
    _countLabel.adjustsFontForContentSizeCategory = YES;
    _countLabel.textColor = UIColor.secondaryLabelColor;
    _countLabel.hidden = YES;
    _progressView = [[UIProgressView alloc] initWithProgressViewStyle:UIProgressViewStyleDefault];
    UIStackView *progress = [[UIStackView alloc] initWithArrangedSubviews:@[_stepLabel, _progressView, _countLabel]];
    progress.axis = UILayoutConstraintAxisVertical;
    progress.spacing = 6;
    progress.translatesAutoresizingMaskIntoConstraints = NO;
    progress.layoutMarginsRelativeArrangement = YES;
    progress.directionalLayoutMargins = NSDirectionalEdgeInsetsMake(12, 16, 10, 16);
    progress.accessibilityElements = @[_stepLabel, _countLabel];
    [self.view addSubview:progress];
    UIView *rule = [UIView new];
    rule.translatesAutoresizingMaskIntoConstraints = NO;
    rule.backgroundColor = UIColor.separatorColor;
    [self.view addSubview:rule];

    [NSLayoutConstraint activateConstraints:@[
        [progress.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor],
        [progress.leadingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.leadingAnchor],
        [progress.trailingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.trailingAnchor],
        [rule.topAnchor constraintEqualToAnchor:progress.bottomAnchor],
        [rule.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
        [rule.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
        [rule.heightAnchor constraintEqualToConstant:1.0 / UIScreen.mainScreen.scale],
        [_textView.topAnchor constraintEqualToAnchor:rule.bottomAnchor],
        [_textView.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
        [_textView.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
        [_textView.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor],
    ]];
    _spinner = [[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleMedium];
}

- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    if (!_started) {
        _started = YES;
        [self startScript];
    }
}

- (void)setRunning:(BOOL)running {
    _running = running;
    // Leaving the page would not stop the script, so there is no leaving it
    // while it runs: Stop is how it ends early.
    [self.navigationItem setHidesBackButton:running animated:YES];
    self.navigationController.interactivePopGestureRecognizer.enabled = !running;
    self.modalInPresentation = running;
    if (running) {
        [_spinner startAnimating];
        self.navigationItem.rightBarButtonItems = @[
            [[UIBarButtonItem alloc] initWithTitle:NSLocalizedString(@"Stop", @"Desktop setup: button that stops the running script")
                                             style:UIBarButtonItemStylePlain target:self action:@selector(stopScript)],
            [[UIBarButtonItem alloc] initWithCustomView:_spinner],
        ];
        // A package download that the screen locking suspends is a download
        // started over, and the scripts say so themselves.
        _idleTimerWasDisabled = UIApplication.sharedApplication.idleTimerDisabled;
        UIApplication.sharedApplication.idleTimerDisabled = YES;
    } else {
        [_spinner stopAnimating];
        self.navigationItem.rightBarButtonItems = nil;
        UIApplication.sharedApplication.idleTimerDisabled = _idleTimerWasDisabled;
    }
}

- (void)appendOutput:(NSString *)text {
    if (text.length == 0)
        return;
    NSAttributedString *more = [[NSAttributedString alloc] initWithString:text attributes:@{
        NSFontAttributeName: _textView.font ?: [UIFont monospacedSystemFontOfSize:12 weight:UIFontWeightRegular],
        NSForegroundColorAttributeName: UIColor.labelColor,
    }];
    [_textView.textStorage appendAttributedString:more];
    if (!_scrolledAway)
        [_textView scrollRangeToVisible:NSMakeRange(_textView.textStorage.length, 0)];
}

- (BOOL)outputIsAtEnd {
    return _textView.contentOffset.y + _textView.bounds.size.height >= _textView.contentSize.height - 40.0;
}

- (void)scrollViewWillBeginDragging:(UIScrollView *)scrollView {
    _scrolledAway = YES;
}

- (void)scrollViewDidEndDragging:(UIScrollView *)scrollView willDecelerate:(BOOL)decelerate {
    if (!decelerate)
        _scrolledAway = ![self outputIsAtEnd];
}

- (void)scrollViewDidEndDecelerating:(UIScrollView *)scrollView {
    _scrolledAway = ![self outputIsAtEnd];
}

// What the scripts print is made for a terminal: colour sequences, and
// carriage returns that redraw a progress line. A text view gets the text.
static NSString *ISHDesktopPlainText(NSString *text) {
    static NSRegularExpression *escapes;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        escapes = [NSRegularExpression regularExpressionWithPattern:@"\\x1b\\[[0-9;?]*[A-Za-z]|\\x1b[()][A-Za-z0-9]" options:0 error:nil];
    });
    NSString *plain = [escapes stringByReplacingMatchesInString:text options:0 range:NSMakeRange(0, text.length) withTemplate:@""];
    plain = [plain stringByReplacingOccurrencesOfString:@"\r\n" withString:@"\n"];
    return [plain stringByReplacingOccurrencesOfString:@"\r" withString:@"\n"];
}

- (void)startScript {
    intptr_t booted = [AppDelegate ensureBooted];
    if (booted < 0) {
        [self finishWithStatus:-1 startError:(int) booted];
        return;
    }
    [self setRunning:YES];
    [self appendOutput:[NSString stringWithFormat:@"# %@\n\n", _command]];
    // stderr into the same stream, in order, and the status on a line of its own.
    NSString *wrapped = [NSString stringWithFormat:@"{ %@; echo; echo %@ $?; } 2>&1", _command, ISHDesktopScriptDoneMarker];
    __weak typeof(self) weakSelf = self;
    dispatch_async(ISHDesktopSetupQueue(), ^{
        struct guest_process process = {0};
        // No terminal, so apt and apk print lines rather than redrawn
        // progress bars, and nothing asks a question.
        int rc = guest_process_spawn_user(NULL, wrapped.UTF8String,
                                          "PATH=/AOK/persist/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin\0"
                                          "HOME=/root\0TERM=dumb\0LANG=C.UTF-8\0DEBIAN_FRONTEND=noninteractive\0",
                                          &process);
        dispatch_async(dispatch_get_main_queue(), ^{
            typeof(self) strongSelf = weakSelf;
            if (strongSelf == nil) {
                if (rc >= 0) {
                    dispatch_async(ISHDesktopSetupQueue(), ^{
                        struct guest_process orphan = process;
                        guest_process_stop(&orphan);
                    });
                }
                return;
            }
            if (rc < 0) {
                [strongSelf finishWithStatus:-1 startError:rc];
                return;
            }
            strongSelf->_process = process;
            [strongSelf readOutputFrom:process.stdout_fd drain:process.stderr_fd];
        });
    });
}

- (void)readOutputFrom:(int)stdoutFD drain:(int)stderrFD {
    __weak typeof(self) weakSelf = self;
    // Nothing is written to stderr (it is folded into stdout above), but a
    // pipe nobody reads stalls whoever does write to it.
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        char sink[512];
        while (read(stderrFD, sink, sizeof(sink)) > 0)
            ;
    });
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        char buffer[4096];
        ssize_t n;
        while ((n = read(stdoutFD, buffer, sizeof(buffer))) > 0) {
            NSData *chunk = [NSData dataWithBytes:buffer length:(NSUInteger) n];
            dispatch_async(dispatch_get_main_queue(), ^{
                [weakSelf consumeOutput:chunk atEnd:NO];
            });
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            [weakSelf consumeOutput:[NSData data] atEnd:YES];
        });
    });
}

// Reads one line of output for the progress header. The scripts' own steps
// are "==> ..." (log() in each setup script). apk numbers each package of a
// transaction, "(12/345) Installing foo"; apt says how many first ("3
// upgraded, 40 newly installed, 0 to remove") and then names each package as
// it is downloaded (Get:), unpacked and set up, or removed.
- (void)noteProgressLine:(NSString *)line {
    static NSRegularExpression *apkLine, *aptSummary;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        apkLine = [NSRegularExpression regularExpressionWithPattern:@"^\\(\\s*(\\d+)/(\\d+)\\)\\s+(\\S+)" options:0 error:nil];
        aptSummary = [NSRegularExpression regularExpressionWithPattern:@"^(\\d+) upgraded, (\\d+) newly installed, (\\d+) to remove" options:0 error:nil];
    });
    NSRange all = NSMakeRange(0, line.length);
    if ([line hasPrefix:@"==> "]) {
        _stepLabel.text = [[line substringFromIndex:4] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        _apkTotal = 0;
        _apkDownloaded = NO;
        _aptInstall = _aptRemove = 0;
        [self showProgress];
        return;
    }
    NSTextCheckingResult *m = [apkLine firstMatchInString:line options:0 range:all];
    if (m != nil) {
        _apkDone = [line substringWithRange:[m rangeAtIndex:1]].integerValue;
        _apkTotal = [line substringWithRange:[m rangeAtIndex:2]].integerValue;
        _apkVerb = [line substringWithRange:[m rangeAtIndex:3]];
        if ([_apkVerb isEqualToString:@"Downloading"])
            _apkDownloaded = YES;
        [self showProgress];
        return;
    }
    m = [aptSummary firstMatchInString:line options:0 range:all];
    if (m != nil) {
        _aptInstall = [line substringWithRange:[m rangeAtIndex:1]].integerValue + [line substringWithRange:[m rangeAtIndex:2]].integerValue;
        _aptRemove = [line substringWithRange:[m rangeAtIndex:3]].integerValue;
        _aptGets = _aptUnpacks = _aptSetups = _aptRemoves = 0;
        _apkTotal = 0;
        [self showProgress];
        return;
    }
    if (_aptInstall + _aptRemove == 0)
        return;
    if ([line hasPrefix:@"Get:"])
        _aptGets++;
    else if ([line hasPrefix:@"Unpacking "])
        _aptUnpacks++;
    else if ([line hasPrefix:@"Setting up "])
        _aptSetups++;
    else if ([line hasPrefix:@"Removing "])
        _aptRemoves++;
    else
        return;
    [self showProgress];
}

- (void)showProgress {
    NSString *count = @"";
    float fraction = 0;
    if (_apkTotal > 0) {
        // apk 3 with --cache-predownload numbers the downloads first, then
        // the installs: each takes half the bar.
        NSInteger done = MIN(_apkDone, _apkTotal);
        fraction = (float) done / (float) _apkTotal;
        NSString *format;
        if ([_apkVerb isEqualToString:@"Downloading"]) {
            format = NSLocalizedString(@"Downloading %1$ld of %2$ld packages", @"Desktop setup progress; %1$ld packages done, %2$ld in all");
            fraction /= 2;
        } else if ([_apkVerb isEqualToString:@"Purging"] || [_apkVerb isEqualToString:@"Deleting"]) {
            format = NSLocalizedString(@"Removing %1$ld of %2$ld packages", @"Desktop setup progress; %1$ld packages done, %2$ld in all");
        } else {
            format = NSLocalizedString(@"Installing %1$ld of %2$ld packages", @"Desktop setup progress; %1$ld packages done, %2$ld in all");
            if (_apkDownloaded)
                fraction = 0.5f + fraction / 2;
        }
        count = [NSString localizedStringWithFormat:format, (long) done, (long) _apkTotal];
    } else if (_aptInstall + _aptRemove > 0) {
        // Each package to install is downloaded, unpacked and set up: three
        // steps; one to remove is one. A package already downloaded skips
        // its Get:, so the bar can end a little short of full.
        NSInteger gets = MIN(_aptGets, _aptInstall), unpacks = MIN(_aptUnpacks, _aptInstall);
        NSInteger setups = MIN(_aptSetups, _aptInstall), removes = MIN(_aptRemoves, _aptRemove);
        fraction = (float) (gets + unpacks + setups + removes) / (float) (3 * _aptInstall + _aptRemove);
        if (removes > 0 || _aptInstall == 0)
            count = [NSString localizedStringWithFormat:NSLocalizedString(@"Removing %1$ld of %2$ld packages", @"Desktop setup progress; %1$ld packages done, %2$ld in all"), (long) removes, (long) _aptRemove];
        else if (setups > 0)
            count = [NSString localizedStringWithFormat:NSLocalizedString(@"Setting up %1$ld of %2$ld packages", @"Desktop setup progress; %1$ld packages done, %2$ld in all"), (long) setups, (long) _aptInstall];
        else if (unpacks > 0)
            count = [NSString localizedStringWithFormat:NSLocalizedString(@"Unpacking %1$ld of %2$ld packages", @"Desktop setup progress; %1$ld packages done, %2$ld in all"), (long) unpacks, (long) _aptInstall];
        else
            count = [NSString localizedStringWithFormat:NSLocalizedString(@"Downloading %1$ld of %2$ld packages", @"Desktop setup progress; %1$ld packages done, %2$ld in all"), (long) gets, (long) _aptInstall];
    }
    _countLabel.text = count;
    _countLabel.hidden = count.length == 0;
    [_progressView setProgress:fraction animated:fraction > _progressView.progress];
}

// Whole lines only, so that a multi-byte character split across two reads is
// never decoded in halves, and the status line is seen whole.
- (void)consumeOutput:(NSData *)chunk atEnd:(BOOL)atEnd {
    [_pending appendData:chunk];
    const char *bytes = _pending.bytes;
    NSUInteger length = _pending.length, lineStart = 0;
    NSMutableString *text = [NSMutableString string];
    int status = INT_MIN;
    for (NSUInteger i = 0; i < length; i++) {
        if (bytes[i] != '\n')
            continue;
        NSString *line = [[NSString alloc] initWithBytes:bytes + lineStart length:i - lineStart encoding:NSUTF8StringEncoding]
            ?: [[NSString alloc] initWithBytes:bytes + lineStart length:i - lineStart encoding:NSISOLatin1StringEncoding] ?: @"";
        lineStart = i + 1;
        if ([line hasPrefix:ISHDesktopScriptDoneMarker]) {
            status = [line substringFromIndex:ISHDesktopScriptDoneMarker.length].intValue;
            continue;
        }
        for (NSString *part in [ISHDesktopPlainText(line) componentsSeparatedByString:@"\n"])
            [self noteProgressLine:part];
        [text appendString:line];
        [text appendString:@"\n"];
    }
    [_pending replaceBytesInRange:NSMakeRange(0, lineStart) withBytes:NULL length:0];
    if (atEnd && _pending.length > 0) {
        NSString *rest = [[NSString alloc] initWithData:_pending encoding:NSUTF8StringEncoding] ?: @"";
        [text appendString:rest];
        [_pending setLength:0];
    }
    [self appendOutput:ISHDesktopPlainText(text)];
    if (status != INT_MIN)
        [self finishWithStatus:status startError:0];
    else if (atEnd && _running)
        [self finishWithStatus:-1 startError:0]; // ended without saying how: stopped, or killed
}

- (void)stopScript {
    if (!_running || _stopRequested)
        return;
    _stopRequested = YES;
    [self appendOutput:[NSString stringWithFormat:@"\n%@\n", NSLocalizedString(@"Stopping…", @"Desktop setup: shown in the output while the script is being stopped")]];
    struct guest_process process = _process;
    __weak typeof(self) weakSelf = self;
    dispatch_async(ISHDesktopSetupQueue(), ^{
        struct guest_process stopping = process;
        guest_process_stop(&stopping);
        dispatch_async(dispatch_get_main_queue(), ^{
            typeof(self) strongSelf = weakSelf;
            if (strongSelf != nil && strongSelf->_running)
                [strongSelf finishWithStatus:-1 startError:0];
        });
    });
}

- (void)finishWithStatus:(int)status startError:(int)startError {
    BOOL wasRunning = _running;
    if (wasRunning)
        [self setRunning:NO];
    else if (!_started || startError == 0)
        return; // already finished
    BOOL stopped = _stopRequested;
    if (wasRunning && !stopped) {
        // The script is gone; let go of its pipes and reap it.
        struct guest_process process = _process;
        dispatch_async(ISHDesktopSetupQueue(), ^{
            struct guest_process done = process;
            guest_process_stop(&done);
        });
    }
    BOOL succeeded = status == 0 && startError == 0 && !stopped;
    NSString *summary;
    if (startError != 0)
        summary = [NSString stringWithFormat:NSLocalizedString(@"The script could not be started (error %d).", @"Desktop setup result; %d is an error number"), startError];
    else if (stopped)
        summary = NSLocalizedString(@"Stopped.", @"Desktop setup result: the user stopped the script");
    else if (succeeded)
        summary = NSLocalizedString(@"Finished.", @"Desktop setup result: the script succeeded");
    else
        summary = [NSString stringWithFormat:NSLocalizedString(@"The script failed (status %d). What went wrong is in its output above.", @"Desktop setup result; %d is an exit status"), status];
    [self appendOutput:[NSString stringWithFormat:@"\n%@\n", summary]];
    _stepLabel.text = summary;
    _countLabel.hidden = YES;
    if (succeeded)
        [_progressView setProgress:1.0f animated:YES];
    [_textView scrollRangeToVisible:NSMakeRange(_textView.textStorage.length, 0)];
    UIAccessibilityPostNotification(UIAccessibilityAnnouncementNotification, summary);
    void (^completion)(BOOL) = _completion;
    _completion = nil;
    if (completion != nil)
        completion(succeeded);
}

- (void)dealloc {
    if (_running) {
        struct guest_process process = _process;
        dispatch_async(ISHDesktopSetupQueue(), ^{
            struct guest_process orphan = process;
            guest_process_stop(&orphan);
        });
        BOOL idle = _idleTimerWasDisabled;
        dispatch_async(dispatch_get_main_queue(), ^{
            UIApplication.sharedApplication.idleTimerDisabled = idle;
        });
    }
}

@end

@interface ISHDesktopSetupViewController : UITableViewController <WorkspaceTextScaledPage>
@end

typedef NS_ENUM(NSInteger, ISHDesktopSetupSection) {
    ISHDesktopSetupSectionDesktops,
    ISHDesktopSetupSectionExtras,
    ISHDesktopSetupSectionCount,
};

@implementation ISHDesktopSetupViewController {
    // What select-desktop.sh reports: which of the four are installed, and
    // which one the next session starts. nil until the first answer.
    NSSet<NSString *> *_Nullable _installed;
    NSString *_Nullable _selected;
    BOOL _probing, _probeFailed;
}

static NSArray<NSString *> *ISHDesktopNames(void) {
    return @[@"labwc", @"xfce", @"wayfire", @"sway"];
}

static NSString *ISHDesktopTitle(NSString *name) {
    if ([name isEqualToString:@"labwc"])
        return NSLocalizedString(@"Basic Desktop (labwc)", @"Desktop setup: desktop name");
    if ([name isEqualToString:@"xfce"])
        return @"Xfce";
    if ([name isEqualToString:@"wayfire"])
        return @"Wayfire";
    return @"Sway";
}

// The script that installs each. labwc and sway come together. Wayfire is
// told which panel the person chose (wayfirePanelSheet), so that it has
// nothing to ask.
static NSString *ISHDesktopInstallCommand(NSString *name) {
    if ([name isEqualToString:@"xfce"])
        return @"sh /AOK/tools/setup-xfce.sh";
    return @"sh /AOK/tools/setup-wayland.sh";
}

- (instancetype)init {
    self = [super initWithStyle:UITableViewStyleInsetGrouped];
    if (self != nil)
        self.title = NSLocalizedString(@"Wayland Desktop", @"Desktop setup page title");
    return self;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    ISHSizeTableSectionTitlesOnMac(self.tableView);
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [self probe];
}

// Asks the guest: `select-desktop.sh` with no argument lists the four with
// "installed" or "not installed", a star on the one that starts.
- (void)probe {
    if (_probing)
        return;
    _probing = YES;
    [self.tableView reloadData];
    __weak typeof(self) weakSelf = self;
    dispatch_async(ISHDesktopSetupQueue(), ^{
        NSMutableSet<NSString *> *installed = nil;
        NSString *selected = nil;
        if ([AppDelegate ensureBooted] >= 0) {
            struct guest_command_result result = {0};
            int rc = run_guest_command_capture("sh /AOK/tools/select-desktop.sh 2>&1", NULL, 30000, 16384, &result);
            if (rc == 0 && result.launched && result.output != NULL) {
                installed = [NSMutableSet set];
                NSString *output = [[NSString alloc] initWithBytes:result.output length:result.output_len encoding:NSUTF8StringEncoding] ?: @"";
                for (NSString *line in [output componentsSeparatedByString:@"\n"]) {
                    NSArray<NSString *> *words = [[line stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet]
                        componentsSeparatedByCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
                    words = [words filteredArrayUsingPredicate:[NSPredicate predicateWithFormat:@"length > 0"]];
                    BOOL starred = words.count > 0 && [words[0] isEqualToString:@"*"];
                    if (starred)
                        words = [words subarrayWithRange:NSMakeRange(1, words.count - 1)];
                    if (words.count < 2 || ![ISHDesktopNames() containsObject:words[0]])
                        continue;
                    if ([words[1] isEqualToString:@"installed"])
                        [installed addObject:words[0]];
                    if (starred)
                        selected = words[0];
                }
            }
            free(result.output);
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            typeof(self) strongSelf = weakSelf;
            if (strongSelf == nil)
                return;
            strongSelf->_probing = NO;
            strongSelf->_probeFailed = installed == nil;
            strongSelf->_installed = installed;
            strongSelf->_selected = selected;
            [strongSelf.tableView reloadData];
        });
    });
}

- (NSArray<NSDictionary<NSString *, NSString *> *> *)extras {
    return @[
        @{@"title": NSLocalizedString(@"Games", @"Desktop setup: a set of extra programs"), @"command": @"sh /AOK/tools/setup-wayland-extras.sh games"},
        @{@"title": NSLocalizedString(@"Tools", @"Desktop setup: a set of extra programs"), @"command": @"sh /AOK/tools/setup-wayland-extras.sh tools"},
        @{@"title": NSLocalizedString(@"X11 Programs", @"Desktop setup: a set of extra programs"), @"command": @"sh /AOK/tools/setup-wayland-extras.sh x11"},
        @{@"title": NSLocalizedString(@"GPU Drivers", @"Desktop setup: installs the GPU drivers"), @"command": @"sh /AOK/tools/setup-gpu.sh"},
    ];
}

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView {
    return ISHDesktopSetupSectionCount;
}

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    if (section == ISHDesktopSetupSectionDesktops)
        return (NSInteger) ISHDesktopNames().count;
    return (NSInteger) [self extras].count;
}

- (NSString *)tableView:(UITableView *)tableView titleForHeaderInSection:(NSInteger)section {
    if (section == ISHDesktopSetupSectionDesktops)
        return NSLocalizedString(@"Desktops", @"Desktop setup section header");
    return NSLocalizedString(@"More Programs", @"Desktop setup section header");
}

- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    if (section == ISHDesktopSetupSectionDesktops) {
        if (_probeFailed)
            return NSLocalizedString(@"The Linux system did not answer. It has to be running for a desktop to be installed in it.", @"Desktop setup footer when the guest cannot be asked");
        return [NSString stringWithFormat:@"%@ %@",
                NSLocalizedString(@"Tap a desktop to install it in the filesystem that is running, or to make it the one the Wayland display starts. Installing downloads packages and takes several minutes; keep iSH-AOK open until it finishes. Wayfire needs Devuan and the GPU.", @"Desktop setup footer"),
                NSLocalizedString(@"To remove an installed desktop, swipe left on it, or touch and hold it.", @"Desktop setup footer, second sentence")];
    }
    return NSLocalizedString(@"Programs for the desktop's Applications menu, and the drivers that let the desktop and its programs use the GPU.", @"Desktop setup footer for the extra programs");
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleValue1 reuseIdentifier:nil];
    if (indexPath.section == ISHDesktopSetupSectionDesktops) {
        NSString *name = ISHDesktopNames()[(NSUInteger) indexPath.row];
        cell.textLabel.text = ISHDesktopTitle(name);
        if (_installed == nil) {
            cell.detailTextLabel.text = _probing ? NSLocalizedString(@"Checking…", @"Desktop setup: state while asking the guest") : @"";
        } else if ([_installed containsObject:name]) {
            BOOL selected = [name isEqualToString:_selected];
            cell.detailTextLabel.text = selected
                ? NSLocalizedString(@"Starts", @"Desktop setup: the desktop the Wayland display starts")
                : NSLocalizedString(@"Installed", @"Desktop setup: desktop state");
            cell.accessoryType = selected ? UITableViewCellAccessoryCheckmark : UITableViewCellAccessoryNone;
        } else {
            cell.detailTextLabel.text = NSLocalizedString(@"Not Installed", @"Desktop setup: desktop state");
            cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
        }
    } else {
        cell.textLabel.text = [self extras][(NSUInteger) indexPath.row][@"title"];
        cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
    }
    cell.accessibilityTraits |= UIAccessibilityTraitButton;
    ISHWorkspaceScaleTableViewCell(cell, ISHWorkspaceTextScaleForViewController(self));
    return cell;
}

- (void)workspaceTextScaleDidChange {
    ISHWorkspaceRescaleTableView(self.tableView, ISHWorkspaceTextScaleForViewController(self));
}

- (void)confirmRunning:(NSString *)title command:(NSString *)command fromCell:(UITableViewCell *)cell {
    ISHActionSheet *sheet = [ISHActionSheet actionSheetWithTitle:title
                                                         message:NSLocalizedString(@"This downloads packages into the filesystem that is running and takes several minutes. Keep iSH-AOK open until it finishes.", @"Desktop setup: confirmation before installing")];
    __weak typeof(self) weakSelf = self;
    [sheet addActionWithTitle:NSLocalizedString(@"Install", @"Desktop setup: confirm button")
                        style:UIAlertActionStyleDefault
                      handler:^(__unused UIAlertAction *action) {
        [weakSelf pushScript:title command:command];
    }];
    [sheet addActionWithTitle:NSLocalizedString(@"Cancel", @"Action sheet button") style:UIAlertActionStyleCancel handler:nil];
    [sheet presentFromViewController:self sourceView:cell sourceRect:cell.bounds];
}

- (void)pushScript:(NSString *)title command:(NSString *)command {
    ISHDesktopScriptViewController *runner = [[ISHDesktopScriptViewController alloc] initWithTitle:title command:command completion:nil];
    [self.navigationController pushViewController:runner animated:YES];
}

// Wayfire comes with a choice of panel (setup-wayfire.sh --panel). The
// install asks it here, in place of the confirmation the others get.
- (void)wayfirePanelSheetFromCell:(UITableViewCell *)cell {
    ISHActionSheet *sheet = [ISHActionSheet actionSheetWithTitle:ISHDesktopTitle(@"wayfire")
                                                         message:NSLocalizedString(@"Choose Wayfire's panel. This downloads packages into the filesystem that is running and takes several minutes; keep iSH-AOK open until it finishes. With both, select-desktop.sh --panel switches between them later.", @"Desktop setup: Wayfire install, the panel choice")];
    NSArray<NSArray<NSString *> *> *panels = @[
        @[NSLocalizedString(@"Install with the Fast Panel", @"Desktop setup: Wayfire with the faster wf-panel"), @"fast"],
        @[NSLocalizedString(@"Install with Waybar", @"Desktop setup: Wayfire with waybar as its panel"), @"waybar"],
        @[NSLocalizedString(@"Install with Both Panels", @"Desktop setup: Wayfire with the fast wf-panel and waybar"), @"both"],
        @[NSLocalizedString(@"Install with the Packaged Panel", @"Desktop setup: Wayfire with the distribution's own wf-panel"), @"stock"],
    ];
    __weak typeof(self) weakSelf = self;
    for (NSArray<NSString *> *panel in panels) {
        NSString *command = [@"sh /AOK/tools/setup-wayfire.sh --panel " stringByAppendingString:panel[1]];
        [sheet addActionWithTitle:panel[0] style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
            [weakSelf pushScript:ISHDesktopTitle(@"wayfire") command:command];
        }];
    }
    [sheet addActionWithTitle:NSLocalizedString(@"Cancel", @"Action sheet button") style:UIAlertActionStyleCancel handler:nil];
    [sheet presentFromViewController:self sourceView:cell sourceRect:cell.bounds];
}

// Removing a desktop takes out its own packages (select-desktop.sh --remove,
// GH #646); what the other desktops and programs use stays.
- (BOOL)canRemoveDesktopAtIndexPath:(NSIndexPath *)indexPath {
    return indexPath.section == ISHDesktopSetupSectionDesktops && _installed != nil
        && [_installed containsObject:ISHDesktopNames()[(NSUInteger) indexPath.row]];
}

- (void)confirmRemovingDesktopAtIndexPath:(NSIndexPath *)indexPath {
    NSString *name = ISHDesktopNames()[(NSUInteger) indexPath.row];
    UITableViewCell *cell = [self.tableView cellForRowAtIndexPath:indexPath];
    ISHActionSheet *sheet = [ISHActionSheet actionSheetWithTitle:[NSString stringWithFormat:NSLocalizedString(@"Remove %@?", @"Desktop setup: confirmation title; %@ is the desktop's name"), ISHDesktopTitle(name)]
                                                         message:NSLocalizedString(@"This removes the desktop's own packages from the filesystem that is running. Whatever other desktops and programs still use stays installed.", @"Desktop setup: confirmation before removing a desktop")];
    __weak typeof(self) weakSelf = self;
    [sheet addActionWithTitle:NSLocalizedString(@"Remove", @"Desktop setup: button that removes a desktop")
                        style:UIAlertActionStyleDestructive
                      handler:^(__unused UIAlertAction *action) {
        [weakSelf pushScript:[NSString stringWithFormat:NSLocalizedString(@"Remove %@", @"Desktop setup: title of the page that removes a desktop; %@ is its name"), ISHDesktopTitle(name)]
                     command:[@"sh /AOK/tools/select-desktop.sh --remove " stringByAppendingString:name]];
    }];
    [sheet addActionWithTitle:NSLocalizedString(@"Cancel", @"Action sheet button") style:UIAlertActionStyleCancel handler:nil];
    if (cell != nil)
        [sheet presentFromViewController:self sourceView:cell sourceRect:cell.bounds];
    else
        [sheet presentFromViewController:self sourceView:self.tableView sourceRect:self.tableView.bounds];
}

- (UISwipeActionsConfiguration *)tableView:(UITableView *)tableView trailingSwipeActionsConfigurationForRowAtIndexPath:(NSIndexPath *)indexPath {
    if (![self canRemoveDesktopAtIndexPath:indexPath])
        return nil;
    __weak typeof(self) weakSelf = self;
    UIContextualAction *remove = [UIContextualAction contextualActionWithStyle:UIContextualActionStyleDestructive
                                                                         title:NSLocalizedString(@"Remove", @"Desktop setup: button that removes a desktop")
                                                                       handler:^(__unused UIContextualAction *action, __unused UIView *view, void (^done)(BOOL)) {
        done(YES);
        [weakSelf confirmRemovingDesktopAtIndexPath:indexPath];
    }];
    UISwipeActionsConfiguration *configuration = [UISwipeActionsConfiguration configurationWithActions:@[remove]];
    configuration.performsFirstActionWithFullSwipe = NO;
    return configuration;
}

- (UIContextMenuConfiguration *)tableView:(UITableView *)tableView contextMenuConfigurationForRowAtIndexPath:(NSIndexPath *)indexPath point:(CGPoint)point {
    if (![self canRemoveDesktopAtIndexPath:indexPath])
        return nil;
    __weak typeof(self) weakSelf = self;
    return [UIContextMenuConfiguration configurationWithIdentifier:nil previewProvider:nil actionProvider:^UIMenu *(__unused NSArray<UIMenuElement *> *suggested) {
        UIAction *remove = [UIAction actionWithTitle:NSLocalizedString(@"Remove…", @"Desktop setup: menu item that removes a desktop")
                                               image:[UIImage systemImageNamed:@"trash"]
                                          identifier:nil
                                             handler:^(__unused UIAction *action) {
            [weakSelf confirmRemovingDesktopAtIndexPath:indexPath];
        }];
        remove.attributes = UIMenuElementAttributesDestructive;
        return [UIMenu menuWithChildren:@[remove]];
    }];
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    UITableViewCell *cell = [tableView cellForRowAtIndexPath:indexPath];
    if (indexPath.section == ISHDesktopSetupSectionExtras) {
        NSDictionary<NSString *, NSString *> *extra = [self extras][(NSUInteger) indexPath.row];
        [self confirmRunning:extra[@"title"] command:extra[@"command"] fromCell:cell];
        return;
    }
    if (_installed == nil)
        return; // not known yet, or the guest did not answer
    NSString *name = ISHDesktopNames()[(NSUInteger) indexPath.row];
    if (![_installed containsObject:name]) {
        if ([name isEqualToString:@"wayfire"])
            [self wayfirePanelSheetFromCell:cell];
        else
            [self confirmRunning:ISHDesktopTitle(name) command:ISHDesktopInstallCommand(name) fromCell:cell];
        return;
    }
    if ([name isEqualToString:_selected])
        return;
    // Installed, and not the one that starts: make it so. Quick, and with
    // nothing to show but the new checkmark.
    // The status comes back in the output, on a line of its own: the guest's
    // init is the command's parent as well, and when it reaps the command
    // first the capture has no exit status to give.
    NSString *command = [NSString stringWithFormat:@"sh /AOK/tools/select-desktop.sh %@ 2>&1; echo; echo %@ $?", name, ISHDesktopScriptDoneMarker];
    __weak typeof(self) weakSelf = self;
    dispatch_async(ISHDesktopSetupQueue(), ^{
        struct guest_command_result result = {0};
        int rc = run_guest_command_capture(command.UTF8String, NULL, 30000, 4096, &result);
        NSString *output = result.output != NULL
            ? [[NSString alloc] initWithBytes:result.output length:result.output_len encoding:NSUTF8StringEncoding] : nil;
        free(result.output);
        BOOL ok = NO;
        NSRange marker = [output rangeOfString:ISHDesktopScriptDoneMarker options:NSBackwardsSearch];
        if (rc == 0 && marker.location != NSNotFound) {
            ok = [output substringFromIndex:NSMaxRange(marker)].intValue == 0;
            output = [[output substringToIndex:marker.location] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            typeof(self) strongSelf = weakSelf;
            if (strongSelf == nil)
                return;
            if (!ok) {
                UIAlertController *alert = [UIAlertController alertControllerWithTitle:ISHDesktopTitle(name)
                                                                               message:output.length > 0 ? output : NSLocalizedString(@"The Linux system did not answer. It has to be running for a desktop to be installed in it.", @"Desktop setup footer when the guest cannot be asked")
                                                                        preferredStyle:UIAlertControllerStyleAlert];
                [alert addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"OK", @"Alert button") style:UIAlertActionStyleDefault handler:nil]];
                [strongSelf presentViewController:alert animated:YES completion:nil];
            }
            [strongSelf probe];
        });
    });
}

@end

UIViewController *ISHCreateDesktopSetupViewController(void) {
    return [ISHDesktopSetupViewController new];
}
