//
//  ViewController.m
//  iSH
//
//  Created by Theodore Dubois on 10/17/17.
//

#import "TerminalViewController.h"
#import "AppDelegate.h"
#import "TerminalView.h"
#import "BarButton.h"
#import "ArrowBarButton.h"
#import "UserPreferences.h"
#import "AboutViewController.h"
#import "CurrentRoot.h"
#import "Roots.h"
#import "NSObject+SaneKVO.h"
#import "UIViewController+Extras.h"
#import "WorkspaceViewController.h"
#import "SceneDelegate.h"
#import "LinuxInterop.h"
#include "kernel/init.h"
#include "kernel/task.h"
#include "kernel/calls.h"
#include "fs/devices.h"

static UISceneSession *ISHFindExistingWorkspaceSceneSession(UISceneSession *excludedSession) API_AVAILABLE(ios(13.0));
static UISceneSession *ISHFindExistingWorkspaceSceneSession(UISceneSession *excludedSession) {
    UISceneSession *bestSession = nil;
    for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
        UISceneSession *session = scene.session;
        if (session == nil || session == excludedSession)
            continue;
        if (![session.stateRestorationActivity.activityType isEqualToString:ISHSceneActivityTypeWorkspace])
            continue;
        if (scene.activationState == UISceneActivationStateForegroundActive)
            return session;
        if (bestSession == nil || scene.activationState == UISceneActivationStateForegroundInactive) {
            bestSession = session;
        }
    }
    return bestSession;
}

@interface TerminalViewController () <UIGestureRecognizerDelegate>

@property UITapGestureRecognizer *tapRecognizer;
@property (weak, nonatomic) IBOutlet TerminalView *termView;
@property (weak, nonatomic) IBOutlet NSLayoutConstraint *bottomConstraint;

@property (weak, nonatomic) IBOutlet UIButton *tabKey;
@property (weak, nonatomic) IBOutlet UIButton *controlKey;
@property (weak, nonatomic) IBOutlet UIButton *escapeKey;
@property (strong, nonatomic) IBOutletCollection(id) NSArray *barButtons;
@property (strong, nonatomic) IBOutletCollection(id) NSArray *barControls;

@property (weak, nonatomic) IBOutlet UIInputView *barView;
@property (weak, nonatomic) IBOutlet UIStackView *bar;
@property (weak, nonatomic) IBOutlet NSLayoutConstraint *barTop;
@property (weak, nonatomic) IBOutlet NSLayoutConstraint *barBottom;
@property (weak, nonatomic) IBOutlet NSLayoutConstraint *barLeading;
@property (weak, nonatomic) IBOutlet NSLayoutConstraint *barTrailing;
@property (weak, nonatomic) IBOutlet NSLayoutConstraint *barButtonWidth;
@property (weak, nonatomic) IBOutlet NSLayoutConstraint *barHeight;
@property (weak, nonatomic) IBOutlet UIView *settingsBadge;

@property (weak, nonatomic) IBOutlet UIButton *infoButton;
@property (strong, nonatomic) UIButton *workspaceButton;
@property (strong, nonatomic) UIButton *terminalSwitcherButton;
@property (weak, nonatomic) IBOutlet UIButton *pasteButton;
@property (weak, nonatomic) IBOutlet UIButton *hideKeyboardButton;
@property (strong, nonatomic) UIButton *floatingWorkspaceButton;
@property (strong, nonatomic) UIButton *floatingSettingsButton;
@property (strong, nonatomic) UIButton *floatingTerminalSwitcherButton;
@property (strong, nonatomic) UIView *floatingSettingsBadge;
@property (strong, nonatomic) NSLayoutConstraint *floatingSettingsBottomConstraint;
@property (strong, nonatomic) UIView *terminalStartupOverlay;
@property (strong, nonatomic) UIActivityIndicatorView *terminalStartupSpinner;
@property (strong, nonatomic) UILabel *terminalStartupLabel;

@property int sessionPid;
@property (nonatomic) Terminal *sessionTerminal;
@property BOOL ignoreKeyboardMotion;
@property (nonatomic) BOOL hasExternalKeyboard;
@property (nonatomic) BOOL didApplyDeferredSafeAreaUpdate;

@end

@implementation TerminalViewController

static const NSInteger kMinimumTerminalFontSize = 1;
static const NSInteger kMaximumTerminalFontSize = 72;

- (Terminal *)currentConsoleTerminal {
    int major = TTY_CONSOLE_MAJOR;
    int minor = 1;
    get_console_device(&major, &minor);
    return [Terminal terminalWithType:major number:minor];
}

- (BOOL)isConsoleTerminal:(Terminal *)terminal {
    if (terminal == nil)
        return NO;
    int major = TTY_CONSOLE_MAJOR;
    int minor = 1;
    get_console_device(&major, &minor);
    return terminal.type == major && terminal.number == minor;
}

- (NSString *)consoleDisplayName {
    int major = TTY_CONSOLE_MAJOR;
    int minor = 1;
    get_console_device(&major, &minor);
    if (major == TTY_CONSOLE_MAJOR)
        return [NSString stringWithFormat:@"System Console (/dev/console -> tty%d)", minor];
    if (major == TTY_PSEUDO_SLAVE_MAJOR)
        return [NSString stringWithFormat:@"System Console (/dev/console -> pts/%d)", minor];
    return [NSString stringWithFormat:@"System Console (/dev/console -> %d:%d)", major, minor];
}

- (BOOL)shouldPreferConsoleForFreshSession {
    NSString *initialWindow = [NSUserDefaults.standardUserDefaults stringForKey:kPreferenceInitialWindowKey];
    if ([initialWindow isEqualToString:@"session-shell"])
        return NO;
    return YES;
}

- (Terminal *)preferredTerminalForFreshSession {
    if (![self shouldPreferConsoleForFreshSession])
        return self.sessionTerminal;
    Terminal *console = [self currentConsoleTerminal];
    return console ?: self.sessionTerminal;
}

- (BOOL)_isTerminalInstalledElsewhere:(Terminal *)terminal {
    if (terminal == nil)
        return NO;
    UIView *superview = terminal.webView.superview;
    if (superview == nil)
        return NO;
    return self.termView.terminal != terminal;
}

- (void)_applyCurrentTerminalToViewIfPossible {
    if (!self.isViewLoaded)
        return;

    if (_terminal == nil) {
        self.termView.terminal = nil;
        return;
    }

    if ([self _isTerminalInstalledElsewhere:_terminal]) {
        self.termView.terminal = nil;
        NSLog(@"Skipping terminal attach for %@ because it is already installed elsewhere", _terminal.uuid.UUIDString);
        [self _showTerminalStartupFailureOverlayWithText:@"Terminal already open in another window."];
        return;
    }

    self.termView.terminal = _terminal;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    [self _installTerminalStartupOverlay];

#if !ISH_LINUX
    if (!Roots.instance.needsInitialRootSelection) {
        intptr_t bootError = [AppDelegate ensureBooted];
        if (bootError < 0) {
            NSString *message = [NSString stringWithFormat:@"could not boot"];
            NSString *subtitle = [NSString stringWithFormat:@"error code %ld", bootError];
            if (bootError == _EINVAL)
                subtitle = [subtitle stringByAppendingString:@"\n(try reinstalling the app, see release notes for details)"];
            [self _showTerminalStartupFailureOverlayWithText:@"Could not boot iSH-AOK."];
            [self showMessage:message subtitle:subtitle];
            NSLog(@"boot failed with code %ld", bootError);
        }
    }
#endif

    [self _applyCurrentTerminalToViewIfPossible];
    [self.termView becomeFirstResponder];

    NSNotificationCenter *center = [NSNotificationCenter defaultCenter];
    [center addObserver:self
               selector:@selector(keyboardDidSomething:)
                   name:UIKeyboardWillChangeFrameNotification
                 object:nil];
    [center addObserver:self
               selector:@selector(keyboardDidSomething:)
                   name:UIKeyboardDidChangeFrameNotification
                 object:nil];
    [center addObserver:self
               selector:@selector(_updateBadge)
                   name:FsUpdatedNotification
                 object:nil];
    [center addObserver:self
               selector:@selector(terminalDidLoad:)
                   name:TerminalDidLoadNotification
                 object:nil];
    [center addObserver:self
               selector:@selector(terminalLoadFailed:)
                   name:TerminalLoadFailedNotification
                 object:nil];


    [self _updateStyleFromPreferences:NO];
    [self _installFloatingWorkspaceButton];
    [self _installFloatingSettingsButton];
    [self _installFloatingTerminalSwitcherButton];
    [self _installWorkspaceButton];
    [self _installTerminalSwitcherButton];
    
    if (UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad) {
        [self.bar removeArrangedSubview:self.hideKeyboardButton];
        [self.hideKeyboardButton removeFromSuperview];
    }
    if (UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPhone) {
        self.barHeight.constant = 36;
    } else {
        self.barHeight.constant = 43;
    }
    
    // SF Symbols is cool
    if (@available(iOS 13, *)) {
        [self.infoButton setImage:[UIImage systemImageNamed:@"gear"] forState:UIControlStateNormal];
        [self.pasteButton setImage:[UIImage systemImageNamed:@"doc.on.clipboard"] forState:UIControlStateNormal];
        [self.hideKeyboardButton setImage:[UIImage systemImageNamed:@"keyboard.chevron.compact.down"] forState:UIControlStateNormal];
        
        [self.tabKey setTitle:nil forState:UIControlStateNormal];
        [self.tabKey setImage:[UIImage systemImageNamed:@"arrow.right.to.line.alt"] forState:UIControlStateNormal];
        [self.controlKey setTitle:nil forState:UIControlStateNormal];
        [self.controlKey setImage:[UIImage systemImageNamed:@"control"] forState:UIControlStateNormal];
        [self.escapeKey setTitle:nil forState:UIControlStateNormal];
        [self.escapeKey setImage:[UIImage systemImageNamed:@"escape"] forState:UIControlStateNormal];
    }
    self.infoButton.accessibilityLabel = @"Settings";
    self.infoButton.accessibilityHint = @"Opens the application settings.";
    self.pasteButton.accessibilityLabel = @"Paste";
    self.pasteButton.accessibilityHint = @"Pastes text from the clipboard.";
    self.hideKeyboardButton.accessibilityLabel = @"Hide Keyboard";
    self.hideKeyboardButton.accessibilityHint = @"Dismisses the on-screen keyboard.";
    self.tabKey.accessibilityLabel = @"Tab";
    self.tabKey.accessibilityHint = @"Sends a tab character.";
    self.controlKey.accessibilityLabel = @"Control";
    self.controlKey.accessibilityHint = @"Toggles the control key modifier.";
    self.escapeKey.accessibilityLabel = @"Escape";
    self.escapeKey.accessibilityHint = @"Sends an escape character.";
    [self _installTerminalSwitcherGestureOnView:self.infoButton];
    
    [UserPreferences.shared observe:@[@"hideStatusBar"] options:0 owner:self usingBlock:^(typeof(self) self) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self setNeedsStatusBarAppearanceUpdate];
        });
    }];
    [UserPreferences.shared observe:@[@"colorScheme", @"theme", @"hideExtraKeysWithExternalKeyboard"]
                            options:0 owner:self usingBlock:^(typeof(self) self) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self _updateStyleFromPreferences:YES];
        });
    }];
    [self _updateBadge];
}

- (void)_installFloatingSettingsButton {
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    button.translatesAutoresizingMaskIntoConstraints = NO;
    button.hidden = YES;
    button.accessibilityLabel = @"Settings";
    button.accessibilityHint = @"Opens the application settings.";
    button.backgroundColor = [UIColor colorWithWhite:0 alpha:0.35];
    button.layer.cornerRadius = 22;
    button.layer.masksToBounds = NO;
    button.layer.shadowColor = UIColor.blackColor.CGColor;
    button.layer.shadowOpacity = 0.2;
    button.layer.shadowRadius = 8;
    button.layer.shadowOffset = CGSizeMake(0, 2);
    if (@available(iOS 13, *)) {
        UIImageSymbolConfiguration *config = [UIImageSymbolConfiguration configurationWithPointSize:18 weight:UIImageSymbolWeightSemibold];
        UIImage *image = [UIImage systemImageNamed:@"gearshape.fill" withConfiguration:config];
        [button setImage:image forState:UIControlStateNormal];
    } else {
        [button setTitle:@"⚙︎" forState:UIControlStateNormal];
        button.titleLabel.font = [UIFont systemFontOfSize:18 weight:UIFontWeightSemibold];
    }
    [button addTarget:self action:@selector(showAbout:) forControlEvents:UIControlEventPrimaryActionTriggered];
    [self.view addSubview:button];
    self.floatingSettingsButton = button;
    [self _installTerminalSwitcherGestureOnView:button];

    UIView *badge = [[UIView alloc] init];
    badge.translatesAutoresizingMaskIntoConstraints = NO;
    badge.hidden = YES;
    if (@available(iOS 13, *)) {
        badge.backgroundColor = UIColor.systemRedColor;
    } else {
        badge.backgroundColor = UIColor.redColor;
    }
    badge.layer.cornerRadius = 5;
    [button addSubview:badge];
    self.floatingSettingsBadge = badge;

    self.floatingSettingsBottomConstraint = [button.bottomAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.bottomAnchor constant:-12];
    [NSLayoutConstraint activateConstraints:@[
        [button.trailingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.trailingAnchor constant:-12],
        self.floatingSettingsBottomConstraint,
        [button.widthAnchor constraintEqualToConstant:44],
        [button.heightAnchor constraintEqualToConstant:44],
        [badge.widthAnchor constraintEqualToConstant:10],
        [badge.heightAnchor constraintEqualToConstant:10],
        [badge.topAnchor constraintEqualToAnchor:button.topAnchor constant:5],
        [badge.trailingAnchor constraintEqualToAnchor:button.trailingAnchor constant:-5],
    ]];
}

- (void)_installFloatingWorkspaceButton {
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    button.translatesAutoresizingMaskIntoConstraints = NO;
    button.hidden = YES;
    button.accessibilityLabel = @"Dashboard";
    button.accessibilityHint = @"Opens the workspace dashboard.";
    button.backgroundColor = [UIColor colorWithWhite:0 alpha:0.35];
    button.layer.cornerRadius = 22;
    button.layer.masksToBounds = NO;
    button.layer.shadowColor = UIColor.blackColor.CGColor;
    button.layer.shadowOpacity = 0.2;
    button.layer.shadowRadius = 8;
    button.layer.shadowOffset = CGSizeMake(0, 2);
    if (@available(iOS 13, *)) {
        UIImageSymbolConfiguration *config = [UIImageSymbolConfiguration configurationWithPointSize:18 weight:UIImageSymbolWeightSemibold];
        UIImage *image = [UIImage systemImageNamed:@"square.grid.2x2" withConfiguration:config];
        [button setImage:image forState:UIControlStateNormal];
    } else {
        [button setTitle:@"WS" forState:UIControlStateNormal];
        button.titleLabel.font = [UIFont systemFontOfSize:14 weight:UIFontWeightSemibold];
    }
    [button addTarget:self action:@selector(showWorkspaceDashboard:) forControlEvents:UIControlEventPrimaryActionTriggered];
    [self.view addSubview:button];
    self.floatingWorkspaceButton = button;

    [NSLayoutConstraint activateConstraints:@[
        [button.trailingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.trailingAnchor constant:-12],
        [button.bottomAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.bottomAnchor constant:-12],
        [button.widthAnchor constraintEqualToConstant:44],
        [button.heightAnchor constraintEqualToConstant:44],
    ]];
}

- (void)_installFloatingTerminalSwitcherButton {
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    button.translatesAutoresizingMaskIntoConstraints = NO;
    button.hidden = YES;
    button.accessibilityLabel = @"Switch Terminal";
    button.accessibilityHint = @"Switches between the session shell and tty terminals.";
    button.backgroundColor = [UIColor colorWithWhite:0 alpha:0.35];
    button.layer.cornerRadius = 22;
    button.layer.masksToBounds = NO;
    button.layer.shadowColor = UIColor.blackColor.CGColor;
    button.layer.shadowOpacity = 0.2;
    button.layer.shadowRadius = 8;
    button.layer.shadowOffset = CGSizeMake(0, 2);
    if (@available(iOS 13, *)) {
        UIImageSymbolConfiguration *config = [UIImageSymbolConfiguration configurationWithPointSize:18 weight:UIImageSymbolWeightSemibold];
        UIImage *image = [UIImage systemImageNamed:@"rectangle.3.group" withConfiguration:config];
        [button setImage:image forState:UIControlStateNormal];
    } else {
        [button setTitle:@"TTY" forState:UIControlStateNormal];
        button.titleLabel.font = [UIFont systemFontOfSize:18 weight:UIFontWeightSemibold];
    }
    [button addTarget:self action:@selector(showTerminalSwitcherFromButton:) forControlEvents:UIControlEventPrimaryActionTriggered];
    [self.view addSubview:button];
    self.floatingTerminalSwitcherButton = button;
    [self _installTerminalSwitcherGestureOnView:button];

    [NSLayoutConstraint activateConstraints:@[
        [button.trailingAnchor constraintEqualToAnchor:self.floatingSettingsButton.leadingAnchor constant:-12],
        [button.bottomAnchor constraintEqualToAnchor:self.floatingSettingsButton.bottomAnchor],
        [button.widthAnchor constraintEqualToConstant:44],
        [button.heightAnchor constraintEqualToConstant:44],
    ]];
}

- (void)_installWorkspaceButton {
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    button.translatesAutoresizingMaskIntoConstraints = NO;
    button.accessibilityLabel = @"Dashboard";
    button.accessibilityHint = @"Opens the workspace dashboard.";
    if (@available(iOS 13, *)) {
        [button setImage:[UIImage systemImageNamed:@"square.grid.2x2"] forState:UIControlStateNormal];
    } else {
        [button setTitle:@"WS" forState:UIControlStateNormal];
    }
    [button addTarget:self action:@selector(showWorkspaceDashboard:) forControlEvents:UIControlEventPrimaryActionTriggered];

    UIView *infoContainer = self.infoButton.superview;
    NSUInteger infoIndex = [self.bar.arrangedSubviews indexOfObject:infoContainer];
    if (infoIndex == NSNotFound)
        infoIndex = self.bar.arrangedSubviews.count;
    [self.bar insertArrangedSubview:button atIndex:infoIndex];
    [button.widthAnchor constraintEqualToAnchor:self.infoButton.widthAnchor].active = YES;

    self.workspaceButton = button;
}

- (void)_installTerminalSwitcherButton {
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    button.translatesAutoresizingMaskIntoConstraints = NO;
    button.accessibilityLabel = @"Switch Terminal";
    button.accessibilityHint = @"Switches between the session shell and tty terminals.";
    if (@available(iOS 13, *)) {
        [button setImage:[UIImage systemImageNamed:@"rectangle.3.group"] forState:UIControlStateNormal];
    } else {
        [button setTitle:@"TTY" forState:UIControlStateNormal];
    }
    [button addTarget:self action:@selector(showTerminalSwitcherFromButton:) forControlEvents:UIControlEventPrimaryActionTriggered];

    UIView *infoContainer = self.infoButton.superview;
    NSUInteger infoIndex = [self.bar.arrangedSubviews indexOfObject:infoContainer];
    if (infoIndex == NSNotFound)
        infoIndex = self.bar.arrangedSubviews.count;
    [self.bar insertArrangedSubview:button atIndex:infoIndex];
    [button.widthAnchor constraintEqualToAnchor:self.infoButton.widthAnchor].active = YES;

    self.terminalSwitcherButton = button;
    [self _installTerminalSwitcherGestureOnView:button];
}

- (void)_installTerminalSwitcherGestureOnView:(UIView *)view {
    UILongPressGestureRecognizer *recognizer =
        [[UILongPressGestureRecognizer alloc] initWithTarget:self
                                                      action:@selector(showTerminalSwitcher:)];
    recognizer.minimumPressDuration = 0.5;
    [view addGestureRecognizer:recognizer];
}

- (void)_installTerminalStartupOverlay {
    UIView *overlay = [[UIView alloc] init];
    overlay.translatesAutoresizingMaskIntoConstraints = NO;
    overlay.userInteractionEnabled = NO;
    if (@available(iOS 13, *)) {
        overlay.backgroundColor = [UIColor.secondarySystemBackgroundColor colorWithAlphaComponent:0.96];
    } else {
        overlay.backgroundColor = [UIColor colorWithWhite:1 alpha:0.96];
    }

    UIActivityIndicatorViewStyle style = UIActivityIndicatorViewStyleGray;
    if (@available(iOS 13, *)) {
        style = UIActivityIndicatorViewStyleMedium;
    }
    UIActivityIndicatorView *spinner = [[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:style];
    spinner.translatesAutoresizingMaskIntoConstraints = NO;
    [spinner startAnimating];
    if (@available(iOS 13, *)) {
        spinner.color = UIColor.secondaryLabelColor;
    }

    UILabel *label = [[UILabel alloc] init];
    label.translatesAutoresizingMaskIntoConstraints = NO;
    label.text = @"Starting terminal...";
    label.textAlignment = NSTextAlignmentCenter;
    label.numberOfLines = 0;
    label.font = [UIFont systemFontOfSize:17 weight:UIFontWeightSemibold];
    if (@available(iOS 13, *)) {
        label.textColor = UIColor.labelColor;
    } else {
        label.textColor = UIColor.blackColor;
    }

    [overlay addSubview:spinner];
    [overlay addSubview:label];
    [self.view addSubview:overlay];

    self.terminalStartupOverlay = overlay;
    self.terminalStartupSpinner = spinner;
    self.terminalStartupLabel = label;

    [NSLayoutConstraint activateConstraints:@[
        [overlay.topAnchor constraintEqualToAnchor:self.view.topAnchor],
        [overlay.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor],
        [overlay.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
        [overlay.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
        [spinner.centerXAnchor constraintEqualToAnchor:overlay.centerXAnchor],
        [spinner.centerYAnchor constraintEqualToAnchor:overlay.centerYAnchor constant:-18],
        [label.topAnchor constraintEqualToAnchor:spinner.bottomAnchor constant:16],
        [label.leadingAnchor constraintGreaterThanOrEqualToAnchor:overlay.leadingAnchor constant:24],
        [label.trailingAnchor constraintLessThanOrEqualToAnchor:overlay.trailingAnchor constant:-24],
        [label.centerXAnchor constraintEqualToAnchor:overlay.centerXAnchor],
    ]];
}

- (void)_bringTerminalRecoveryControlsToFront {
    if (self.floatingWorkspaceButton.superview != nil)
        [self.view bringSubviewToFront:self.floatingWorkspaceButton];
    if (self.floatingSettingsButton.superview != nil)
        [self.view bringSubviewToFront:self.floatingSettingsButton];
    if (self.floatingTerminalSwitcherButton.superview != nil)
        [self.view bringSubviewToFront:self.floatingTerminalSwitcherButton];
}

- (void)_showTerminalStartupOverlayWithText:(NSString *)text {
    self.terminalStartupLabel.text = text;
    self.terminalStartupOverlay.hidden = NO;
    [self.terminalStartupSpinner startAnimating];
    [self.view bringSubviewToFront:self.terminalStartupOverlay];
    [self _bringTerminalRecoveryControlsToFront];
}

- (void)_showTerminalStartupFailureOverlayWithText:(NSString *)text {
    self.terminalStartupLabel.text = text;
    self.terminalStartupOverlay.hidden = NO;
    [self.terminalStartupSpinner stopAnimating];
    [self.view bringSubviewToFront:self.terminalStartupOverlay];
    [self _bringTerminalRecoveryControlsToFront];
}

- (void)_hideTerminalStartupOverlay {
    self.terminalStartupOverlay.hidden = YES;
    [self.terminalStartupSpinner stopAnimating];
}

- (BOOL)_shouldShowFloatingSettingsButton {
    return self.termView.inputAccessoryView == nil;
}

- (void)_updateFloatingSettingsButtonVisibility {
    BOOL visible = [self _shouldShowFloatingSettingsButton];
    BOOL showWorkspaceButtons = self.showsWorkspaceDashboardButton;
    BOOL settingsEnabled = !self.embeddedInWorkspaceWindow;
    self.floatingWorkspaceButton.hidden = !(visible && showWorkspaceButtons);
    self.floatingWorkspaceButton.userInteractionEnabled = visible && showWorkspaceButtons;
    self.floatingSettingsButton.hidden = !(visible && settingsEnabled);
    self.floatingSettingsButton.userInteractionEnabled = visible && settingsEnabled;
    self.floatingTerminalSwitcherButton.hidden = !visible;
    self.floatingTerminalSwitcherButton.userInteractionEnabled = visible;
    self.terminalSwitcherButton.hidden = visible;
    self.terminalSwitcherButton.userInteractionEnabled = !visible;
    self.workspaceButton.hidden = !showWorkspaceButtons || visible;
    self.workspaceButton.userInteractionEnabled = showWorkspaceButtons && !visible;
    self.infoButton.alpha = settingsEnabled ? 1 : 0;
    self.infoButton.userInteractionEnabled = settingsEnabled;
    self.floatingSettingsBottomConstraint.constant = -12;
}

- (void)awakeFromNib {
    [super awakeFromNib];
#if !ISH_LINUX
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(processExited:)
                                               name:ProcessExitedNotification
                                             object:nil];
#else
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(kernelPanicked:)
                                               name:KernelPanicNotification
                                             object:nil];
#endif
}

- (void)viewDidAppear:(BOOL)animated {
    [AppDelegate maybePresentStartupMessageOnViewController:self];
    [super viewDidAppear:animated];
    [self _updateSafeAreaCompensation];
    if (!self.didApplyDeferredSafeAreaUpdate) {
        self.didApplyDeferredSafeAreaUpdate = YES;
        dispatch_async(dispatch_get_main_queue(), ^{
            [self _updateSafeAreaCompensation];
            [self.view layoutIfNeeded];
        });
    }
}

- (void)startNewSession {
    [self _showTerminalStartupOverlayWithText:@"Starting terminal..."];
    intptr_t err = [self startSession];
    if (err < 0) {
        [self _showTerminalStartupFailureOverlayWithText:@"Could not start session."];
        [self showMessage:@"could not start session"
                 subtitle:[NSString stringWithFormat:@"error code %ld", err]];
        return;
    }
    self.terminal = [self preferredTerminalForFreshSession];
}

- (void)showSystemConsoleForCurrentSession {
    Terminal *console = [self currentConsoleTerminal];
    if (console != nil) {
        self.terminal = console;
        return;
    }
    if (self.sessionTerminal != nil)
        self.terminal = self.sessionTerminal;
}

- (void)showSessionShellForCurrentSession {
    if (self.sessionTerminal != nil) {
        self.terminal = self.sessionTerminal;
        return;
    }
    Terminal *console = [self currentConsoleTerminal];
    if (console != nil)
        self.terminal = console;
}

- (void)reconnectSessionFromTerminalUUID:(NSUUID *)uuid {
    Terminal *terminal = [Terminal terminalWithUUID:uuid];
    if (terminal == nil) {
        [self startNewSession];
        return;
    }
    if (terminal.type == TTY_PSEUDO_SLAVE_MAJOR) {
        self.sessionTerminal = terminal;
        self.terminal = self.sessionTerminal;
        return;
    }
    self.sessionTerminal = nil;
    self.terminal = terminal;
}

- (NSUUID *)sessionTerminalUUID {
    if (self.sessionTerminal != nil)
        return self.sessionTerminal.uuid;
    return self.terminal.uuid;
}

- (intptr_t)startSession {
    NSArray<NSString *> *command = UserPreferences.shared.launchCommand;

#if !ISH_LINUX
    intptr_t err = [AppDelegate ensureBooted];
    if (err < 0)
        return err;
    err = become_new_init_child();
    if (err < 0)
        return err;
    struct tty *tty;
    self.sessionTerminal = nil;
    Terminal *terminal = [Terminal createPseudoTerminal:&tty];
    if (terminal == nil) {
        NSAssert(IS_ERR(tty), @"tty should be error");
        return (int) PTR_ERR(tty);
    }
    self.sessionTerminal = terminal;
    NSString *stdioFile = [NSString stringWithFormat:@"/dev/pts/%d", tty->num];
    err = create_stdio(stdioFile.fileSystemRepresentation, TTY_PSEUDO_SLAVE_MAJOR, tty->num);
    if (err < 0)
        return err;
    tty_release(tty);

    char argv[4096];
    [Terminal convertCommand:command toArgs:argv limitSize:sizeof(argv)];
    const char *envp = "TERM=xterm-256color\0";
    err = do_execve(command[0].UTF8String, command.count, argv, envp);
    if (err < 0)
        return err;
    self.sessionPid = current->pid;
    task_start(current);
#else
    const char *argv_arr[command.count + 1];
    for (NSUInteger i = 0; i < command.count; i++)
        argv_arr[i] = command[i].UTF8String;
    argv_arr[command.count] = NULL;
    const char *envp_arr[] = {
        "TERM=xterm-256color",
        NULL,
    };
    const char *const *argv = argv_arr;
    const char *const *envp = envp_arr;
    __block Terminal *terminal = nil;
    __block int sessionPid = 0;
    __block int err = 1;
    sync_do_in_workqueue(^(void (^done)(void)) {
        linux_start_session(argv[0], argv, envp, ^(int retval, int pid, nsobj_t term) {
            err = retval;
            if (term)
                terminal = CFBridgingRelease(term);
            sessionPid = pid;
            done();
        });
    });
    NSAssert(err <= 0, @"session start did not finish??");
    if (err < 0)
        return err;
    self.sessionTerminal = terminal;
    self.sessionPid = sessionPid;
#endif
    return 0;
}

#if !ISH_LINUX
- (void)processExited:(NSNotification *)notif {
    int pid = [notif.userInfo[@"pid"] intValue];
    if (pid != self.sessionPid)
        return;

    [self.sessionTerminal destroy];
    // On iOS 13, there are multiple windows, so just close this one.
    if (@available(iOS 13, *)) {
        // On iPhone, destroying scenes will fail, but the error doesn't actually go to the error handler, which is really stupid. Apple doesn't fix bugs, so I'm forced to just add a check here.
        if (UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad && self.sceneSession != nil) {
            [UIApplication.sharedApplication requestSceneSessionDestruction:self.sceneSession options:nil errorHandler:^(NSError *error) {
                NSLog(@"scene destruction error %@", error);
                self.sceneSession = nil;
                [self processExited:notif];
            }];
            return;
        }
    }
    current = NULL; // it's been freed
    [self startNewSession];
}
#endif

#if ISH_LINUX
- (void)kernelPanicked:(NSNotification *)notif {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"panik" message:notif.userInfo[@"message"] preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"k" style:UIAlertActionStyleDefault handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}
#endif

- (void)showMessage:(NSString *)message subtitle:(NSString *)subtitle {
    dispatch_async(dispatch_get_main_queue(), ^{
        if (self.presentedViewController != nil)
            return;
        UIAlertController *alert = [UIAlertController alertControllerWithTitle:message message:subtitle preferredStyle:UIAlertControllerStyleAlert];
        [alert addAction:[UIAlertAction actionWithTitle:@"k"
                                                  style:UIAlertActionStyleDefault
                                                handler:nil]];
        [self presentViewController:alert animated:YES completion:nil];
    });
}

- (void)terminalDidLoad:(NSNotification *)notification {
    if (notification.object != self.terminal)
        return;
    [self _hideTerminalStartupOverlay];
}

- (void)terminalLoadFailed:(NSNotification *)notification {
    if (notification.object != self.terminal)
        return;
    NSError *error = notification.userInfo[@"error"];
    NSString *subtitle = error.localizedDescription ?: @"unknown error";
    [self _showTerminalStartupFailureOverlayWithText:@"Terminal UI failed to load."];
    [self showMessage:@"terminal UI failed to load" subtitle:subtitle];
}

- (void)observeValueForKeyPath:(NSString *)keyPath ofObject:(id)object change:(NSDictionary *)change context:(void *)context {
    if (object == [UserPreferences shared]) {
        [self _updateStyleFromPreferences:YES];
    } else {
        [super observeValueForKeyPath:keyPath ofObject:object change:change context:context];
    }
}

- (void)_updateStyleFromPreferences:(BOOL)animated {
    NSAssert(NSThread.isMainThread, @"This method needs to be called on the main thread");
    NSTimeInterval duration = animated ? 0.1 : 0;
    [UIView animateWithDuration:duration animations:^{
        self.view.backgroundColor = [[UIColor alloc] ish_initWithHexString:UserPreferences.shared.palette.backgroundColor];
        UIKeyboardAppearance keyAppearance = UserPreferences.shared.keyboardAppearance;
        self.termView.keyboardAppearance = keyAppearance;
        for (BarButton *button in self.barButtons) {
            button.keyAppearance = keyAppearance;
        }
        UIColor *tintColor = keyAppearance == UIKeyboardAppearanceLight ? UIColor.blackColor : UIColor.whiteColor;
        for (UIControl *control in self.barControls) {
            control.tintColor = tintColor;
        }
        self.terminalSwitcherButton.tintColor = tintColor;
        self.floatingTerminalSwitcherButton.tintColor = tintColor;
        self.floatingTerminalSwitcherButton.backgroundColor = keyAppearance == UIKeyboardAppearanceLight ?
            [UIColor colorWithWhite:1 alpha:0.78] :
            [UIColor colorWithWhite:0 alpha:0.45];
        self.workspaceButton.tintColor = tintColor;
        self.floatingWorkspaceButton.tintColor = tintColor;
        self.floatingWorkspaceButton.backgroundColor = keyAppearance == UIKeyboardAppearanceLight ?
            [UIColor colorWithWhite:1 alpha:0.78] :
            [UIColor colorWithWhite:0 alpha:0.45];
        self.floatingSettingsButton.tintColor = tintColor;
        self.floatingSettingsButton.backgroundColor = keyAppearance == UIKeyboardAppearanceLight ?
            [UIColor colorWithWhite:1 alpha:0.78] :
            [UIColor colorWithWhite:0 alpha:0.45];
    }];
    UIView *oldBarView = self.termView.inputAccessoryView;
    BOOL hideAccessoryBar = self.hasExternalKeyboard &&
        (UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad ||
         UserPreferences.shared.hideExtraKeysWithExternalKeyboard);
    if (hideAccessoryBar) {
        self.termView.inputAccessoryView = nil;
    } else {
        self.termView.inputAccessoryView = self.barView;
    }
    [self _updateFloatingSettingsButtonVisibility];
    if (self.termView.inputAccessoryView != oldBarView && self.termView.isFirstResponder) {
        dispatch_async(dispatch_get_main_queue(), ^{
            self.ignoreKeyboardMotion = YES; // avoid infinite recursion
            [self.termView reloadInputViews];
            self.ignoreKeyboardMotion = NO;
        });
    }
}
- (void)_updateStyleAnimated {
    [self _updateStyleFromPreferences:YES];
}

- (void)_updateBadge {
    BOOL showBadge = FsNeedsRepositoryUpdate();
    self.settingsBadge.hidden = !showBadge;
    self.floatingSettingsBadge.hidden = !showBadge;
    self.infoButton.accessibilityValue = showBadge ? @"Update available" : nil;
    self.floatingSettingsButton.accessibilityValue = showBadge ? @"Update available" : nil;
}

- (UIStatusBarStyle)preferredStatusBarStyle {
    return UserPreferences.shared.statusBarStyle;
}

- (BOOL)prefersStatusBarHidden {
    return UserPreferences.shared.hideStatusBar;
}

- (void)_updateSafeAreaCompensation {
    if (self.embeddedInWorkspaceWindow) {
        if (!UIEdgeInsetsEqualToEdgeInsets(self.additionalSafeAreaInsets, UIEdgeInsetsZero)) {
            self.additionalSafeAreaInsets = UIEdgeInsetsZero;
        }
        if (self.hasExternalKeyboard) {
            self.bottomConstraint.constant = 0;
        }
        [self _updateFloatingSettingsButtonVisibility];
        return;
    }

    CGFloat statusBarHeight = 0;
    if (@available(iOS 13.0, *)) {
        statusBarHeight = self.view.window.windowScene.statusBarManager.statusBarFrame.size.height;
    } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        statusBarHeight = UIApplication.sharedApplication.statusBarFrame.size.height;
#pragma clang diagnostic pop
    }

    CGFloat baseTopInset = MAX(0, self.view.safeAreaInsets.top - self.additionalSafeAreaInsets.top);
    UIEdgeInsets extraInsets = self.additionalSafeAreaInsets;
    extraInsets.top = MAX(0, statusBarHeight - baseTopInset);
    if (!UIEdgeInsetsEqualToEdgeInsets(self.additionalSafeAreaInsets, extraInsets)) {
        self.additionalSafeAreaInsets = extraInsets;
    }

    if (self.hasExternalKeyboard) {
        self.bottomConstraint.constant = self.view.safeAreaInsets.bottom;
    }
    [self _updateFloatingSettingsButtonVisibility];
}

- (void)viewSafeAreaInsetsDidChange {
    [super viewSafeAreaInsetsDidChange];
    [self _updateSafeAreaCompensation];
}

- (void)keyboardDidSomething:(NSNotification *)notification {
    if (self.ignoreKeyboardMotion)
        return;

    CGRect screenKeyboardFrame = [notification.userInfo[UIKeyboardFrameEndUserInfoKey] CGRectValue];
    UIScreen *screen;
    if (@available(iOS 13.0, *)) {
        screen = self.view.window.windowScene.screen ?: UIScreen.mainScreen;
    } else {
        screen = UIScreen.mainScreen;
    }
    // notification.object is nil before iOS 16.1 and the correct UIScreen after iOS 16.1
    if (notification.object != nil)
        screen = notification.object;
    CGRect keyboardFrame = [self.view convertRect:screenKeyboardFrame fromCoordinateSpace:screen.coordinateSpace];
    if (CGRectEqualToRect(keyboardFrame, CGRectZero))
        return;
    CGRect intersection = CGRectIntersection(keyboardFrame, self.view.bounds);
    keyboardFrame = intersection;
    self.hasExternalKeyboard = keyboardFrame.size.height < 100;
    CGFloat pad = self.view.safeAreaInsets.bottom;
    if (!self.hasExternalKeyboard) {
        pad = CGRectGetMaxY(self.view.bounds) - CGRectGetMinY(keyboardFrame);
        // The keyboard appears to be undocked. This means it can either be split or
        // truly floating. In the former case we want to keep the pad, but in the
        // latter we should fall back to the input accessory view instead of the
        // keyboard.
        if (pad != keyboardFrame.size.height && keyboardFrame.size.width != screen.bounds.size.width) {
            pad = MAX(self.view.safeAreaInsets.bottom, self.termView.inputAccessoryView.frame.size.height);
        }
    }
    // NSLog(@"pad %f", pad);
    self.bottomConstraint.constant = pad;
    [self _updateFloatingSettingsButtonVisibility];

    BOOL initialLayout = self.termView.needsUpdateConstraints;
    [self.view setNeedsUpdateConstraints];
    if (!initialLayout) {
        // if initial layout hasn't happened yet, the terminal view is going to be at a really weird place, so animating it is going to look really bad
        NSNumber *interval = notification.userInfo[UIKeyboardAnimationDurationUserInfoKey];
        NSNumber *curve = notification.userInfo[UIKeyboardAnimationCurveUserInfoKey];
        [UIView animateWithDuration:interval.doubleValue
                              delay:0
                            options:curve.integerValue << 16
                         animations:^{
                             [self.view layoutIfNeeded];
                         }
                         completion:nil];
    }
}

- (void)setHasExternalKeyboard:(BOOL)hasExternalKeyboard {
    _hasExternalKeyboard = hasExternalKeyboard;
    [self _updateStyleFromPreferences:YES];
}

- (void)prepareForSegue:(UIStoryboardSegue *)segue sender:(id)sender {
    if ([segue.identifier isEqualToString:@"embed"]) {
        // You might want to check if this is your embed segue here
        // in case there are other segues triggered from this view controller.
        segue.destinationViewController.view.translatesAutoresizingMaskIntoConstraints = NO;
    }
}

- (void)traitCollectionDidChange:(UITraitCollection *)previousTraitCollection {
    // Hack to resolve a layering mismatch between the the UI and preferences.
    if (@available(iOS 12.0, *)) {
        if (previousTraitCollection.userInterfaceStyle != self.traitCollection.userInterfaceStyle) {
            // Ensure that the relevant things listening for this will update.
            UserPreferences.shared.colorScheme = UserPreferences.shared.colorScheme;
        }
    }
}

#pragma mark Bar

- (IBAction)showAbout:(id)sender {
    UINavigationController *navigationController = [[UIStoryboard storyboardWithName:@"About" bundle:nil] instantiateInitialViewController];
    if ([sender isKindOfClass:[UIGestureRecognizer class]]) {
        UIGestureRecognizer *recognizer = sender;
        if (recognizer.state == UIGestureRecognizerStateBegan) {
            AboutViewController *aboutViewController = (AboutViewController *) navigationController.topViewController;
            aboutViewController.includeDebugPanel = YES;
        } else {
            return;
        }
    }
    [self presentViewController:navigationController animated:YES completion:nil];
    [self.termView resignFirstResponder];
}

- (IBAction)showWorkspaceDashboard:(id)sender {
    [self.termView resignFirstResponder];
    if (@available(iOS 13.0, *)) {
        UISceneSession *workspaceSession = ISHFindExistingWorkspaceSceneSession(self.sceneSession);
        NSUserActivity *activity = workspaceSession == nil
            ? [[NSUserActivity alloc] initWithActivityType:ISHSceneActivityTypeWorkspace]
            : nil;
        [UIApplication.sharedApplication requestSceneSessionActivation:workspaceSession
                                                         userActivity:activity
                                                              options:nil
                                                         errorHandler:^(__unused NSError *error) {
            dispatch_async(dispatch_get_main_queue(), ^{
                UINavigationController *navigationController = ISHCreateWorkspaceNavigationController();
                UIWindow *window = self.view.window;
                window.rootViewController = navigationController;
                [window makeKeyAndVisible];
            });
        }];
        return;
    }

    UINavigationController *navigationController = ISHCreateWorkspaceNavigationController();
    UIWindow *window = self.view.window;
    window.rootViewController = navigationController;
    [window makeKeyAndVisible];
}

- (NSString *)terminalDisplayName:(Terminal *)terminal {
    if (terminal == nil)
        return @"Unknown Terminal";
    if (terminal == self.sessionTerminal)
        return [NSString stringWithFormat:@"Session Shell (pts/%d)", terminal.number];
    if ([self isConsoleTerminal:terminal])
        return [self consoleDisplayName];
    if (terminal.type == TTY_CONSOLE_MAJOR)
        return [NSString stringWithFormat:@"Terminal (tty%d)", terminal.number];
    if (terminal.type == TTY_PSEUDO_SLAVE_MAJOR)
        return [NSString stringWithFormat:@"Pseudo Terminal (pts/%d)", terminal.number];
    return [NSString stringWithFormat:@"Terminal (%d:%d)", terminal.type, terminal.number];
}

- (IBAction)showTerminalSwitcherFromButton:(id)sender {
    UIView *sourceView = [sender isKindOfClass:UIView.class] ? sender : self.infoButton;
    [self presentTerminalSwitcherFromView:sourceView];
}

- (void)showTerminalSwitcher:(UILongPressGestureRecognizer *)recognizer {
    if (recognizer.state != UIGestureRecognizerStateBegan)
        return;

    [self presentTerminalSwitcherFromView:recognizer.view];
}

- (void)presentTerminalSwitcherFromView:(UIView *)sourceView {
    if (sourceView == nil)
        sourceView = self.infoButton;

    UIAlertController *alert =
        [UIAlertController alertControllerWithTitle:@"Switch Terminal"
                                            message:@"Boot via init creates both a session shell and system consoles."
                                     preferredStyle:UIAlertControllerStyleActionSheet];

    Terminal *sessionTerminal = self.sessionTerminal;
    if (sessionTerminal != nil) {
        NSString *title = (self.terminal == sessionTerminal)
            ? [[self terminalDisplayName:sessionTerminal] stringByAppendingString:@" (Current)"]
            : [self terminalDisplayName:sessionTerminal];
        [alert addAction:[UIAlertAction actionWithTitle:title
                                                  style:UIAlertActionStyleDefault
                                                handler:^(__unused UIAlertAction *action) {
            self.terminal = sessionTerminal;
        }]];
    }

    Terminal *consoleTerminal = [self currentConsoleTerminal];
    if (consoleTerminal != nil && consoleTerminal.type != TTY_CONSOLE_MAJOR) {
        NSString *title = [self.terminal.uuid isEqual:consoleTerminal.uuid]
            ? [[self terminalDisplayName:consoleTerminal] stringByAppendingString:@" (Current)"]
            : [self terminalDisplayName:consoleTerminal];
        [alert addAction:[UIAlertAction actionWithTitle:title
                                                  style:UIAlertActionStyleDefault
                                                handler:^(__unused UIAlertAction *action) {
            self.terminal = consoleTerminal;
        }]];
    }

    for (int i = 1; i <= 7; i++) {
        Terminal *console = [Terminal terminalWithType:TTY_CONSOLE_MAJOR number:i];
        NSString *title = [self.terminal.uuid isEqual:console.uuid]
            ? [[self terminalDisplayName:console] stringByAppendingString:@" (Current)"]
            : [self terminalDisplayName:console];
        [alert addAction:[UIAlertAction actionWithTitle:title
                                                  style:UIAlertActionStyleDefault
                                                handler:^(__unused UIAlertAction *action) {
            self.terminal = console;
        }]];
    }

    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel"
                                              style:UIAlertActionStyleCancel
                                            handler:nil]];
    UIPopoverPresentationController *popover = alert.popoverPresentationController;
    if (popover != nil) {
        popover.sourceView = sourceView;
        popover.sourceRect = sourceView.bounds;
    }
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)resizeBar {
    CGSize bar = self.barView.bounds.size;
    // set sizing parameters on bar
    // numbers stolen from iVim and modified somewhat
    if (UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPhone) {
        // phone
        [self setBarHorizontalPadding:6 verticalPadding:6 buttonWidth:32];
    } else if (bar.width >= 450) {
        // wide ipad
        [self setBarHorizontalPadding:15 verticalPadding:8 buttonWidth:43];
    } else {
        // narrow ipad (slide over)
        [self setBarHorizontalPadding:10 verticalPadding:8 buttonWidth:36];
    }
    [UIView performWithoutAnimation:^{
        [self.barView layoutIfNeeded];
    }];
}

- (void)setBarHorizontalPadding:(CGFloat)horizontal verticalPadding:(CGFloat)vertical buttonWidth:(CGFloat)buttonWidth {
    self.barLeading.constant = self.barTrailing.constant = horizontal;
    self.barTop.constant = self.barBottom.constant = vertical;
    self.barButtonWidth.constant = buttonWidth;
}

- (IBAction)pressEscape:(id)sender {
    [self pressKey:@"\x1b"];
}
- (IBAction)pressTab:(id)sender {
    [self pressKey:@"\t"];
}
- (void)pressKey:(NSString *)key {
    [self.termView insertText:key];
}

- (IBAction)pressControl:(id)sender {
    self.controlKey.selected = !self.controlKey.selected;
}
    
- (IBAction)pressArrow:(ArrowBarButton *)sender {
    switch (sender.direction) {
        case ArrowUp: [self pressKey:[self.terminal arrow:'A']]; break;
        case ArrowDown: [self pressKey:[self.terminal arrow:'B']]; break;
        case ArrowLeft: [self pressKey:[self.terminal arrow:'D']]; break;
        case ArrowRight: [self pressKey:[self.terminal arrow:'C']]; break;
        case ArrowNone: break;
    }
}

- (void)switchTerminal:(UIKeyCommand *)sender {
    unsigned i = (unsigned) sender.input.integerValue;
    if (i == 7)
        self.terminal = self.sessionTerminal;
    else
        self.terminal = [Terminal terminalWithType:TTY_CONSOLE_MAJOR number:i];
}

- (void)increaseFontSize:(UIKeyCommand *)command {
    [self updatePersistentFontSize:self.termView.effectiveFontSize + 1];
}
- (void)decreaseFontSize:(UIKeyCommand *)command {
    [self updatePersistentFontSize:self.termView.effectiveFontSize - 1];
}
- (void)resetFontSize:(UIKeyCommand *)command {
    [self updatePersistentFontSize:UserPreferences.shared.defaultFontSize.doubleValue];
}

- (void)updatePersistentFontSize:(CGFloat)fontSize {
    NSInteger clampedFontSize = MAX(kMinimumTerminalFontSize, MIN(kMaximumTerminalFontSize, lround(fontSize)));
    self.termView.overrideFontSize = 0;
    UserPreferences.shared.fontSize = @(clampedFontSize);
}

- (NSArray<UIKeyCommand *> *)keyCommands {
    static NSMutableArray<UIKeyCommand *> *commands = nil;
    if (commands == nil) {
        commands = [NSMutableArray new];
        for (unsigned i = 1; i <= 7; i++) {
            NSString *title = i == 7
                ? @"Switch to Session Shell"
                : [NSString stringWithFormat:@"Switch to tty%u", i];
            [commands addObject:
             [UIKeyCommand keyCommandWithInput:[NSString stringWithFormat:@"%d", i]
                                 modifierFlags:UIKeyModifierCommand|UIKeyModifierAlternate|UIKeyModifierShift
                                        action:@selector(switchTerminal:)
                          discoverabilityTitle:title]];
        }
        [commands addObject:
         [UIKeyCommand keyCommandWithInput:@"+"
                             modifierFlags:UIKeyModifierCommand
                                    action:@selector(increaseFontSize:)
                      discoverabilityTitle:@"Increase Font Size"]];
        [commands addObject:
         [UIKeyCommand keyCommandWithInput:@"="
                             modifierFlags:UIKeyModifierCommand
                                    action:@selector(increaseFontSize:)]];
        [commands addObject:
         [UIKeyCommand keyCommandWithInput:@"-"
                             modifierFlags:UIKeyModifierCommand
                                    action:@selector(decreaseFontSize:)
                      discoverabilityTitle:@"Decrease Font Size"]];
        [commands addObject:
         [UIKeyCommand keyCommandWithInput:@"0"
                             modifierFlags:UIKeyModifierCommand
                                    action:@selector(resetFontSize:)
                      discoverabilityTitle:@"Reset Font Size"]];
        [commands addObject:
         [UIKeyCommand keyCommandWithInput:@","
                             modifierFlags:UIKeyModifierCommand
                                    action:@selector(showAbout:)
                      discoverabilityTitle:@"Settings"]];
    }
    return commands;
}

- (void)setTerminal:(Terminal *)terminal {
    _terminal = terminal;
    [self _applyCurrentTerminalToViewIfPossible];
    BOOL installedElsewhere = [self _isTerminalInstalledElsewhere:_terminal];
    if (installedElsewhere) {
        [self _showTerminalStartupFailureOverlayWithText:@"Terminal already open in another window."];
    } else if (_terminal != nil && _terminal.loaded) {
        [self _hideTerminalStartupOverlay];
    } else if (_terminal != nil) {
        [self _showTerminalStartupOverlayWithText:@"Loading terminal UI..."];
    } else {
        [self _showTerminalStartupOverlayWithText:@"Starting terminal..."];
    }
}

- (void)setSessionTerminal:(Terminal *)sessionTerminal {
    if (_terminal == _sessionTerminal)
        self.terminal = sessionTerminal;
    _sessionTerminal = sessionTerminal;
}

@end

@interface BarView : UIInputView
@property (weak) IBOutlet TerminalViewController *terminalViewController;
@property (nonatomic) IBInspectable BOOL allowsSelfSizing;
@end
@implementation BarView
@dynamic allowsSelfSizing;

- (void)layoutSubviews {
    [self.terminalViewController resizeBar];
}

@end
