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
#import "Diagnostics.h"
#import "Roots.h"
#import "NSObject+SaneKVO.h"
#import "UIViewController+Extras.h"
#import "WorkspaceViewController.h"
#import "SceneDelegate.h"
#import "LinuxInterop.h"
#import <GameController/GameController.h>
#include "kernel/init.h"
#include "kernel/task.h"
#include "kernel/calls.h"
#include "kernel/fs.h"
#include "fs/devices.h"
#include "fs/path.h"

static UISceneSession *ISHFindExistingWorkspaceSceneSession(UISceneSession *excludedSession) API_AVAILABLE(ios(13.0));
static UISceneSession *ISHFindExistingWorkspaceSceneSession(UISceneSession *excludedSession) {
    NSArray<NSString *> *forgottenHiddenSessions = [NSUserDefaults.standardUserDefaults arrayForKey:@"ISHWorkspaceForgottenHiddenSessions"];
    UISceneSession *bestSession = nil;
    for (UISceneSession *session in UIApplication.sharedApplication.openSessions) {
        if (session == nil || session == excludedSession)
            continue;
        if (![session.stateRestorationActivity.activityType isEqualToString:ISHSceneActivityTypeWorkspace])
            continue;
        UIScene *connectedScene = nil;
        for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
            if (scene.session == session) {
                connectedScene = scene;
                break;
            }
        }
        if (connectedScene == nil && [forgottenHiddenSessions containsObject:session.persistentIdentifier])
            continue;
        if (connectedScene == nil && bestSession == nil) {
            bestSession = session;
            continue;
        }
        if (connectedScene == nil)
            continue;
        if (connectedScene.activationState == UISceneActivationStateForegroundActive)
            return session;
        if (bestSession == nil || connectedScene.activationState == UISceneActivationStateForegroundInactive) {
            bestSession = session;
        }
    }
    return bestSession;
}

#if !ISH_LINUX
static BOOL ISHCommandIsDefaultLogin(NSArray<NSString *> *command) {
    return command.count == 3 &&
            [command[0] isEqualToString:@"/bin/login"] &&
            [command[1] isEqualToString:@"-f"] &&
            [command[2] isEqualToString:@"root"];
}

// "Open everything as the UID 1000 (default) user account": only swaps a command that resolved
// to exactly the standard root login -- never a raw fallback shell (which can't switch users
// anyway) and never a launch command the user customized to something else, since a customized
// command means they've already taken explicit control of who they log in as. No account is
// provisioned for this -- [AppDelegate defaultUserAccountName] looks up whatever's actually at
// UID 1000 on this rootfs; keeps the root login unchanged if there isn't one.
static NSArray<NSString *> *ISHCommandWithDefaultUserSubstitution(NSArray<NSString *> *command) {
    if (!UserPreferences.shared.shouldLoginAsDefaultUser || !ISHCommandIsDefaultLogin(command))
        return command;
    NSString *accountName = [AppDelegate defaultUserAccountName];
    if (accountName.length == 0)
        return command;
    return @[command[0], command[1], accountName];
}

static BOOL ISHGuestExecutableExists(NSString *path, intptr_t *errOut) {
    struct task *previousCurrent = NULL;
    BOOL borrowedInit = [AppDelegate pushUsableInitTaskAsCurrent:&previousCurrent];
    if (!borrowedInit) {
        [AppDelegate popCurrentTask:previousCurrent];
        if (errOut != NULL)
            *errOut = _ENOENT;
        return NO;
    }
    struct statbuf stat;
    int err = generic_statat(AT_PWD, path.UTF8String, &stat, 0);
    [AppDelegate popCurrentTask:previousCurrent];
    if (err < 0) {
        if (errOut != NULL)
            *errOut = err;
        return NO;
    }
    if (!S_ISREG(stat.mode)) {
        if (errOut != NULL)
            *errOut = _EACCES;
        return NO;
    }
    if (!(stat.mode & 0111)) {
        if (errOut != NULL)
            *errOut = _EACCES;
        return NO;
    }
    if (errOut != NULL)
        *errOut = 0;
    return YES;
}

static NSArray<NSString *> *ISHCommandDescriptions(NSArray<NSArray<NSString *> *> *commands) {
    NSMutableArray<NSString *> *descriptions = [NSMutableArray arrayWithCapacity:commands.count];
    for (NSArray<NSString *> *command in commands) {
        [descriptions addObject:[command componentsJoinedByString:@" "]];
    }
    return descriptions;
}

static NSArray<NSString *> *ISHSessionCommandWithFallback(NSArray<NSString *> *command,
                                                          NSString **failureTitleOut,
                                                          NSString **failureMessageOut,
                                                          NSString **failureOverlayOut) {
    if (!ISHCommandIsDefaultLogin(command))
        return command;

    intptr_t configuredErr = 0;
    if (ISHGuestExecutableExists(command[0], &configuredErr))
        return command;

    NSArray<NSArray<NSString *> *> *candidates = @[
        @[@"/usr/bin/login", @"-f", @"root"],
        @[@"/bin/sh", @"-l"],
        @[@"/usr/bin/sh", @"-l"],
        @[@"/bin/ash", @"-l"],
        @[@"/usr/bin/ash", @"-l"],
        @[@"/bin/bash", @"-l"],
        @[@"/usr/bin/bash", @"-l"],
        @[@"/bin/busybox", @"sh"],
        @[@"/usr/bin/busybox", @"sh"],
    ];
    NSMutableArray<NSDictionary<NSString *, id> *> *attempts = [NSMutableArray arrayWithCapacity:candidates.count + 1];
    [attempts addObject:@{@"command": [command componentsJoinedByString:@" "],
                          @"path": command[0],
                          @"error": @(configuredErr),
                          @"errorDescription": [AppDelegate descriptionForISHErrno:configuredErr]}];

    for (NSArray<NSString *> *candidate in candidates) {
        NSString *path = candidate.firstObject;
        intptr_t err = 0;
        if (ISHGuestExecutableExists(path, &err)) {
            [ISHDiagnosticsStore recordBreadcrumb:@"terminal.session.fallback.selected"
                                          details:@{@"configuredCommand": [command componentsJoinedByString:@" "],
                                                    @"fallbackCommand": [candidate componentsJoinedByString:@" "],
                                                    @"fallbackPath": path ?: @"",
                                                    @"missingLoginError": @(configuredErr),
                                                    @"missingLoginErrorDescription": [AppDelegate descriptionForISHErrno:configuredErr],
                                                    @"candidates": ISHCommandDescriptions(candidates),
                                                    @"attempts": attempts}];
            return candidate;
        }
        [attempts addObject:@{@"command": [candidate componentsJoinedByString:@" "],
                              @"path": path ?: @"",
                              @"error": @(err),
                              @"errorDescription": [AppDelegate descriptionForISHErrno:err]}];
    }

    NSString *configuredCommand = [command componentsJoinedByString:@" "];
    if (failureTitleOut != NULL)
        *failureTitleOut = @"No usable login shell found";
    if (failureMessageOut != NULL) {
        *failureMessageOut = [NSString stringWithFormat:
                              @"The root filesystem does not contain the default session command, and no fallback shell was found.\n\nCommand: %@\nError: %@\nFallbacks checked: %@\n\nInstall login or a shell such as /bin/sh, or change Settings -> Launch Command.",
                              configuredCommand,
                              [AppDelegate descriptionForISHErrno:configuredErr],
                              [ISHCommandDescriptions(candidates) componentsJoinedByString:@", "]];
    }
    if (failureOverlayOut != NULL)
        *failureOverlayOut = @"No usable login shell found.";
    [ISHDiagnosticsStore recordBreadcrumb:@"terminal.session.fallback.none"
                                  details:@{@"configuredCommand": configuredCommand ?: @"",
                                            @"missingLoginError": @(configuredErr),
                                            @"missingLoginErrorDescription": [AppDelegate descriptionForISHErrno:configuredErr],
                                            @"candidates": ISHCommandDescriptions(candidates),
                                            @"attempts": attempts}];
    return command;
}
#endif

@interface TerminalViewController () <UIGestureRecognizerDelegate>

@property UITapGestureRecognizer *tapRecognizer;
@property (weak, nonatomic) IBOutlet TerminalView *termView;
@property (weak, nonatomic) IBOutlet NSLayoutConstraint *bottomConstraint;

@property (weak, nonatomic) IBOutlet UIButton *tabKey;
@property (weak, nonatomic) IBOutlet UIButton *controlKey;
@property (weak, nonatomic) IBOutlet UIButton *escapeKey;
@property (weak, nonatomic) IBOutlet ArrowBarButton *arrowKey;
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
@property (strong, nonatomic) BarButton *dotKey;
@property (strong, nonatomic) BarButton *slashKey;
@property (strong, nonatomic) BarButton *dashKey;
@property (strong, nonatomic) BarButton *colonKey;
@property (strong, nonatomic) BarButton *bangKey;
@property (strong, nonatomic) BarButton *pipeKey;
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
@property (copy, nonatomic) NSString *sessionFailureTitle;
@property (copy, nonatomic) NSString *sessionFailureMessage;
@property (copy, nonatomic) NSString *sessionFailureOverlayText;

@property int sessionPid;
@property (nonatomic) Terminal *sessionTerminal;
@property (nonatomic) BOOL sessionStartInProgress;
@property (nonatomic) NSTimeInterval sessionStartedAt;
@property (nonatomic) NSInteger consecutiveQuickSessionExits;
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
    switch (self.freshSessionTerminalDisplayMode) {
        case ISHFreshSessionTerminalDisplayModeSessionShell:
            return NO;
        case ISHFreshSessionTerminalDisplayModeSystemConsole:
            return YES;
        case ISHFreshSessionTerminalDisplayModeAuto:
            break;
    }
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
        [ISHDiagnosticsStore recordBreadcrumb:@"terminalVC.applyTerminal"
                                      details:@{@"action": @"clear"}];
        self.termView.terminal = nil;
        return;
    }

    if ([self _isTerminalInstalledElsewhere:_terminal]) {
        [ISHDiagnosticsStore recordBreadcrumb:@"terminalVC.applyTerminal"
                                      details:@{@"action": @"skip-installed-elsewhere",
                                                @"terminalUUID": _terminal.uuid.UUIDString ?: @"",
                                                @"type": @(_terminal.type),
                                                @"number": @(_terminal.number)}];
        self.termView.terminal = nil;
        NSLog(@"Skipping terminal attach for %@ because it is already installed elsewhere", _terminal.uuid.UUIDString);
        [self _showTerminalStartupFailureOverlayWithText:@"Terminal already open in another window."];
        return;
    }

    [ISHDiagnosticsStore recordBreadcrumb:@"terminalVC.applyTerminal"
                                  details:@{@"action": @"attach",
                                            @"terminalUUID": _terminal.uuid.UUIDString ?: @"",
                                            @"type": @(_terminal.type),
                                            @"number": @(_terminal.number)}];
    self.termView.terminal = _terminal;
}

- (void)focusTerminal {
    if (!UserPreferences.shared.autoShowKeyboard)
        return;
    [self.termView becomeFirstResponder];
}

- (void)viewDidLoad {
    [super viewDidLoad];
    [self _installTerminalStartupOverlay];

#if !ISH_LINUX
    if (!Roots.instance.needsInitialRootSelection) {
        intptr_t bootError = [AppDelegate ensureBooted];
        if (bootError < 0) {
            NSString *message = [AppDelegate bootFailureTitle] ?: @"Could not boot iSH-AOK";
            NSString *subtitle = [AppDelegate bootFailureMessage] ?: [AppDelegate descriptionForISHErrno:bootError];
            NSString *overlayText = [AppDelegate bootFailureOverlayText] ?: @"Could not boot iSH-AOK.";
            [self _showTerminalStartupFailureOverlayWithText:overlayText];
            [self showMessage:message subtitle:subtitle];
            NSLog(@"boot failed: %@", subtitle);
        }
    }
#endif

    [self _applyCurrentTerminalToViewIfPossible];
    if (UserPreferences.shared.autoShowKeyboard)
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
    // hasExternalKeyboard is otherwise only recomputed inside keyboardDidSomething:, which
    // fires off software-keyboard frame changes. A hardware keyboard attaching/detaching
    // while the terminal isn't first responder (Settings screen, Workspace, backgrounded
    // app) or a Bluetooth keyboard power-cycling doesn't reliably produce one of those, so
    // the accessory bar/Esc key could go stale until some unrelated keyboard-frame event
    // happened to fire later. Listen to the authoritative hardware signal directly.
    [center addObserver:self
               selector:@selector(hardwareKeyboardDidChange:)
                   name:GCKeyboardDidConnectNotification
                 object:nil];
    [center addObserver:self
               selector:@selector(hardwareKeyboardDidChange:)
                   name:GCKeyboardDidDisconnectNotification
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
    [self _installCenterKeys];

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

    // Hold the arrow key in place (no directional drag) to switch Desktops without adding a
    // dedicated key to an already-tight accessory bar. Only wired up when this terminal is a
    // window inside the Workspace desktop manager, which is what owns desktop switching.
    __weak typeof(self) weakSelf = self;
    self.arrowKey.longPressHandler = ^{
        typeof(self) strongSelf = weakSelf;
        if (strongSelf == nil || !strongSelf.embeddedInWorkspaceWindow)
            return;
        UIViewController *host = strongSelf.parentViewController;
        if (![host isKindOfClass:WorkspaceViewController.class])
            return;
        [(WorkspaceViewController *)host presentDesktopSwitchMenuFromView:strongSelf.arrowKey
                                                                sourceRect:strongSelf.arrowKey.bounds];
    };
    
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
    [UserPreferences.shared observe:@[@"maximizeScreenSpace", @"hideExtraKeysWithExternalKeyboard"]
                            options:0 owner:self usingBlock:^(typeof(self) self) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self _updateSafeAreaCompensation];
            [self _applyScreenPadding];
            [self.view setNeedsUpdateConstraints];
            [self.view setNeedsLayout];
        });
    }];
    [UserPreferences.shared observe:@[@"showTerminalQuickButtons"]
                            options:0 owner:self usingBlock:^(typeof(self) self) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self _updateFloatingSettingsButtonVisibility];
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
    button.accessibilityLabel = @"Layout Manager";
    button.accessibilityHint = @"Opens the workspace layout manager.";
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
    button.accessibilityLabel = @"Layout Manager";
    button.accessibilityHint = @"Opens the workspace layout manager.";
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

- (BarButton *)_makeCenterKeyWithTitle:(NSString *)title action:(SEL)action {
    BarButton *button = [BarButton buttonWithType:UIButtonTypeCustom];
    button.translatesAutoresizingMaskIntoConstraints = NO;
    [button setTitle:title forState:UIControlStateNormal];
    button.titleLabel.font = [UIFont systemFontOfSize:20];
    // Replicate BarButton's -awakeFromNib, which only runs for storyboard instances.
    button.layer.cornerRadius = 5;
    button.layer.shadowOffset = CGSizeMake(0, 1);
    button.layer.shadowOpacity = 0.4;
    button.layer.shadowRadius = 0;
    button.accessibilityTraits |= UIAccessibilityTraitKeyboardKey;
    button.keyAppearance = UIKeyboardAppearanceLight; // setter applies background/tint/title color
    [button addTarget:self action:action forControlEvents:UIControlEventPrimaryActionTriggered];
    return button;
}

// Up to six extra keys centered on the accessory bar -- characters a shell user
// reaches for constantly. '.' and '/' (./  ../  /usr  *.c) are always present and
// straddle the bar's horizontal center; '-', ':', '!' and '|' (command flags,
// host:path, history !!/!$, pipelines) flank them but only when there's room -- hidden
// on an iPhone in portrait, where the bar is already full, and shown in landscape and on iPad.
// Flexible space on either side keeps the left keys and right controls on their own
// edges. Present on both the plain and Workspace bars.
- (void)_installCenterKeys {
    // The storyboard bar has a single flexible spacer (a plain, childless UIView)
    // between the left keys and the right controls; reuse it as the left spacer and
    // add a matching one to the right of the new keys.
    UIView *leftSpacer = nil;
    for (UIView *view in self.bar.arrangedSubviews) {
        if ([view isMemberOfClass:UIView.class] && view.subviews.count == 0) {
            leftSpacer = view;
            break;
        }
    }
    if (leftSpacer == nil)
        return;

    self.dashKey = [self _makeCenterKeyWithTitle:@"-" action:@selector(pressDash:)];
    self.dashKey.accessibilityLabel = @"Hyphen";
    self.dashKey.accessibilityHint = @"Sends a hyphen.";
    self.dotKey = [self _makeCenterKeyWithTitle:@"." action:@selector(pressPeriod:)];
    self.dotKey.accessibilityLabel = @"Period";
    self.dotKey.accessibilityHint = @"Sends a period.";
    self.slashKey = [self _makeCenterKeyWithTitle:@"/" action:@selector(pressSlash:)];
    self.slashKey.accessibilityLabel = @"Slash";
    self.slashKey.accessibilityHint = @"Sends a forward slash.";
    self.colonKey = [self _makeCenterKeyWithTitle:@":" action:@selector(pressColon:)];
    self.colonKey.accessibilityLabel = @"Colon";
    self.colonKey.accessibilityHint = @"Sends a colon.";
    self.bangKey = [self _makeCenterKeyWithTitle:@"!" action:@selector(pressBang:)];
    self.bangKey.accessibilityLabel = @"Exclamation mark";
    self.bangKey.accessibilityHint = @"Sends an exclamation mark.";
    self.pipeKey = [self _makeCenterKeyWithTitle:@"|" action:@selector(pressPipe:)];
    self.pipeKey.accessibilityLabel = @"Vertical bar";
    self.pipeKey.accessibilityHint = @"Sends a vertical bar.";

    UIView *rightSpacer = [[UIView alloc] init];
    rightSpacer.translatesAutoresizingMaskIntoConstraints = NO;

    // Order across the center: dash dot slash colon bang pipe, with the dot/slash gap
    // pinned to the bar center below.
    NSUInteger spacerIndex = [self.bar.arrangedSubviews indexOfObject:leftSpacer];
    [self.bar insertArrangedSubview:self.dashKey atIndex:spacerIndex + 1];
    [self.bar insertArrangedSubview:self.dotKey atIndex:spacerIndex + 2];
    [self.bar insertArrangedSubview:self.slashKey atIndex:spacerIndex + 3];
    [self.bar insertArrangedSubview:self.colonKey atIndex:spacerIndex + 4];
    [self.bar insertArrangedSubview:self.bangKey atIndex:spacerIndex + 5];
    [self.bar insertArrangedSubview:self.pipeKey atIndex:spacerIndex + 6];
    [self.bar insertArrangedSubview:rightSpacer atIndex:spacerIndex + 7];

    // Both spacers yield their width freely so the keys can reach the center; the
    // >= 0 floor keeps a very narrow bar from forcing them (and the keys) to overlap.
    for (UIView *spacer in @[leftSpacer, rightSpacer]) {
        [spacer setContentHuggingPriority:UILayoutPriorityDefaultLow forAxis:UILayoutConstraintAxisHorizontal];
        [spacer setContentCompressionResistancePriority:UILayoutPriorityDefaultLow forAxis:UILayoutConstraintAxisHorizontal];
        [spacer.widthAnchor constraintGreaterThanOrEqualToConstant:0].active = YES;
    }

    // Center the whole key cluster by making the flexible space on each side equal, so
    // the gap to the left controls matches the gap to the right controls no matter how
    // many keys are shown (and when they collapse on iPhone portrait). Pinning one
    // interior gap to the bar center instead left the cluster visibly lopsided once more
    // keys sat to its right than to its left. Just-breakable so a very narrow bar still
    // degrades gracefully.
    NSLayoutConstraint *center = [leftSpacer.widthAnchor constraintEqualToAnchor:rightSpacer.widthAnchor];
    center.priority = UILayoutPriorityRequired - 1;
    center.active = YES;

    [self.dotKey.widthAnchor constraintEqualToAnchor:self.infoButton.widthAnchor].active = YES;
    [self.slashKey.widthAnchor constraintEqualToAnchor:self.infoButton.widthAnchor].active = YES;
    // '-', ':', '!' and '|' match the other keys' width, but just-breakable so
    // UIStackView can collapse them to zero width when hidden (portrait iPhone)
    // without a conflict.
    NSLayoutConstraint *dashWidth = [self.dashKey.widthAnchor constraintEqualToAnchor:self.infoButton.widthAnchor];
    NSLayoutConstraint *colonWidth = [self.colonKey.widthAnchor constraintEqualToAnchor:self.infoButton.widthAnchor];
    NSLayoutConstraint *bangWidth = [self.bangKey.widthAnchor constraintEqualToAnchor:self.infoButton.widthAnchor];
    NSLayoutConstraint *pipeWidth = [self.pipeKey.widthAnchor constraintEqualToAnchor:self.infoButton.widthAnchor];
    dashWidth.priority = UILayoutPriorityRequired - 1;
    colonWidth.priority = UILayoutPriorityRequired - 1;
    bangWidth.priority = UILayoutPriorityRequired - 1;
    pipeWidth.priority = UILayoutPriorityRequired - 1;
    dashWidth.active = YES;
    colonWidth.active = YES;
    bangWidth.active = YES;
    pipeWidth.active = YES;

    [self _updateCenterKeyVisibility];
}

// '-', ':', '!' and '|' only appear when the accessory bar has room: hidden on an iPhone in
// portrait (regular height, compact width), shown in landscape and on iPad. A hidden
// UIStackView arranged subview is collapsed, so '.' and '/' stay centered either way.
- (void)_updateCenterKeyVisibility {
    BOOL phonePortrait = UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPhone &&
        self.traitCollection.verticalSizeClass == UIUserInterfaceSizeClassRegular;
    self.dashKey.hidden = phonePortrait;
    self.colonKey.hidden = phonePortrait;
    self.bangKey.hidden = phonePortrait;
    self.pipeKey.hidden = phonePortrait;
}

- (void)_installTerminalSwitcherGestureOnView:(UIView *)view {
    UILongPressGestureRecognizer *recognizer =
        [[UILongPressGestureRecognizer alloc] initWithTarget:self
                                                      action:@selector(showTerminalSwitcher:)];
    recognizer.minimumPressDuration = 0.5;
    [view addGestureRecognizer:recognizer];
    if ([view respondsToSelector:@selector(setAccessibilityHint:)]) {
        NSString *currentHint = view.accessibilityHint;
        if (currentHint.length > 0 && ![currentHint containsString:@"Touch and hold to switch terminals."]) {
            view.accessibilityHint = [NSString stringWithFormat:@"%@ Touch and hold to switch terminals.", currentHint];
        } else if (currentHint.length == 0) {
            view.accessibilityHint = @"Touch and hold to switch terminals.";
        }
    }
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
    // User-toggleable (Settings ▸ Appearance ▸ Terminal Buttons): the settings gear and the
    // terminal-switcher button, in both their bar and floating forms.
    BOOL settingsEnabled = UserPreferences.shared.showTerminalQuickButtons;
    self.floatingWorkspaceButton.hidden = !(visible && showWorkspaceButtons);
    self.floatingWorkspaceButton.userInteractionEnabled = visible && showWorkspaceButtons;
    self.floatingSettingsButton.hidden = !(visible && settingsEnabled);
    self.floatingSettingsButton.userInteractionEnabled = visible && settingsEnabled;
    self.floatingTerminalSwitcherButton.hidden = !(visible && settingsEnabled);
    self.floatingTerminalSwitcherButton.userInteractionEnabled = visible && settingsEnabled;
    self.terminalSwitcherButton.hidden = visible || !settingsEnabled;
    self.terminalSwitcherButton.userInteractionEnabled = !visible && settingsEnabled;
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
    // Re-sync against the live hardware state: a keyboard connect/disconnect notification
    // could have been missed while this view wasn't visible (e.g. it was posted to a
    // different scene, or arrived during app launch before observers were registered).
    self.hasExternalKeyboard = GCKeyboard.coalescedKeyboard != nil;
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
    if (self.sessionStartInProgress) {
        [ISHDiagnosticsStore recordBreadcrumb:@"terminal.session.start.ignored"
                                      details:@{@"reason": @"already-starting"}];
        return;
    }
    if (self.sessionPid > 0 && self.sessionTerminal != nil) {
        [ISHDiagnosticsStore recordBreadcrumb:@"terminal.session.start.ignored"
                                      details:@{@"reason": @"already-running",
                                                @"pid": @(self.sessionPid)}];
        self.terminal = [self preferredTerminalForFreshSession];
        return;
    }

    self.sessionStartInProgress = YES;
    [self _showTerminalStartupOverlayWithText:@"Starting terminal..."];
    intptr_t err = [self startSession];
    self.sessionStartInProgress = NO;
	    if (err < 0) {
	        Terminal *failedTerminal = self.sessionTerminal;
	        self.sessionTerminal = nil;
	        self.sessionPid = 0;
	        [failedTerminal destroy];
	        NSString *message = self.sessionFailureTitle ?: @"Could not start session";
	        NSString *subtitle = self.sessionFailureMessage ?: [AppDelegate descriptionForISHErrno:err];
	        NSString *overlayText = self.sessionFailureOverlayText ?: @"Could not start session.";
	        if (err == [AppDelegate bootError] && [AppDelegate bootFailureMessage] != nil) {
	            message = [AppDelegate bootFailureTitle] ?: message;
	            subtitle = [AppDelegate bootFailureMessage];
            overlayText = [AppDelegate bootFailureOverlayText] ?: overlayText;
        }
        [self _showTerminalStartupFailureOverlayWithText:overlayText];
        [self showMessage:message subtitle:subtitle];
        return;
    }
    self.terminal = [self preferredTerminalForFreshSession];
}

- (void)disposeSessionForWorkspaceClose {
    Terminal *sessionTerminal = self.sessionTerminal;
    if (sessionTerminal == nil)
        return;
    // Clear our session bookkeeping first so a process-exit notification racing in from the
    // hangup below is filtered out by the pid guard in processExited: (and so a second call is a
    // no-op). destroy hangs up the pty, which delivers SIGHUP to the shell so it exits.
    self.sessionTerminal = nil;
    self.sessionPid = 0;
    [sessionTerminal setPendingDestroyReason:@"workspace-window-closed"];
    [sessionTerminal destroy];
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
        if (self.sessionStartInProgress || self.sessionPid > 0 || self.sessionTerminal != nil) {
            [ISHDiagnosticsStore recordBreadcrumb:@"terminal.session.reconnect.deferred"
                                          details:@{@"reason": self.sessionStartInProgress ? @"starting" : @"session-exists",
                                                    @"terminalUUID": uuid.UUIDString ?: @""}];
            if (self.sessionTerminal != nil)
                self.terminal = self.sessionTerminal;
            return;
        }
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
	    NSString *commandString = [command componentsJoinedByString:@" "];
	    self.sessionFailureTitle = nil;
	    self.sessionFailureMessage = nil;
	    self.sessionFailureOverlayText = nil;

#if !ISH_LINUX
	    intptr_t err = [AppDelegate ensureBooted];
	    if (err < 0) {
	        [ISHDiagnosticsStore recordBreadcrumb:@"terminal.session.start.failed"
	                                      details:@{@"stage": @"ensureBooted",
	                                                @"error": @(err),
	                                                @"command": commandString ?: @""}];
	        return err;
	    }
	    if ([AppDelegate bootUsesConsoleSessionFallback]) {
	        [ISHDiagnosticsStore recordBreadcrumb:@"terminal.session.consoleFallback"
	                                      details:@{@"reason": @"boot-init-fallback",
	                                                @"command": commandString ?: @""}];
	        return 0;
	    }
	    NSString *sessionFailureTitle = nil;
	    NSString *sessionFailureMessage = nil;
	    NSString *sessionFailureOverlayText = nil;
	    command = ISHSessionCommandWithFallback(command,
	                                            &sessionFailureTitle,
	                                            &sessionFailureMessage,
	                                            &sessionFailureOverlayText);
	    // The Session Shell always opens as root: the main terminal's session (the
	    // "Session Shell (pts/1)" surface) and workspace Session Shell windows are
	    // the admin surface, so only workspace terminals opened outside that role
	    // get the default-user substitution.
	    if (self.embeddedInWorkspaceWindow && !self.alwaysLoginAsRoot)
	        command = ISHCommandWithDefaultUserSubstitution(command);
	    commandString = [command componentsJoinedByString:@" "];
	    if (sessionFailureMessage.length != 0) {
	        self.sessionFailureTitle = sessionFailureTitle;
	        self.sessionFailureMessage = sessionFailureMessage;
	        self.sessionFailureOverlayText = sessionFailureOverlayText;
	        [ISHDiagnosticsStore recordBreadcrumb:@"terminal.session.start.failed"
	                                      details:@{@"stage": @"launchCommandFallback",
	                                                @"error": @(_ENOENT),
	                                                @"command": commandString ?: @""}];
	        return _ENOENT;
	    }
	    err = become_new_init_child();
	    if (err < 0) {
	        [ISHDiagnosticsStore recordBreadcrumb:@"terminal.session.start.failed"
                                      details:@{@"stage": @"become_new_init_child",
                                                @"error": @(err),
                                                @"command": commandString ?: @""}];
        return err;
    }
    struct tty *tty;
    self.sessionTerminal = nil;
    Terminal *terminal = [Terminal createPseudoTerminal:&tty];
    if (terminal == nil) {
        NSAssert(IS_ERR(tty), @"tty should be error");
        [ISHDiagnosticsStore recordBreadcrumb:@"terminal.session.start.failed"
                                      details:@{@"stage": @"createPseudoTerminal",
                                                @"error": @((int) PTR_ERR(tty)),
                                                @"command": commandString ?: @""}];
        return (int) PTR_ERR(tty);
    }
    self.sessionTerminal = terminal;
    NSString *stdioFile = [NSString stringWithFormat:@"/dev/pts/%d", tty->num];
    err = create_stdio(stdioFile.fileSystemRepresentation, TTY_PSEUDO_SLAVE_MAJOR, tty->num);
    if (err < 0) {
        [ISHDiagnosticsStore recordBreadcrumb:@"terminal.session.start.failed"
                                      details:@{@"stage": @"create_stdio",
                                                @"error": @(err),
                                                @"command": commandString ?: @"",
                                                @"stdio": stdioFile ?: @""}];
        return err;
    }
    tty_release(tty);

    char argv[4096];
    [Terminal convertCommand:command toArgs:argv limitSize:sizeof(argv)];
    const char *envp = "TERM=screen-256color\0";
	    err = do_execve(command[0].UTF8String, command.count, argv, envp);
	    if (err < 0) {
	        NSString *failureTitle = @"Could not start session command";
	        NSString *failureMessage = [NSString stringWithFormat:
	                                    @"The root filesystem was mounted, but the session command could not be executed.\n\nCommand: %@\nPath: %@\nError: %@\n\nInstall the missing program, choose a root filesystem with a shell, or change Settings -> Launch Command.",
	                                    commandString ?: @"",
	                                    command.firstObject ?: @"",
	                                    [AppDelegate descriptionForISHErrno:err]];
	        self.sessionFailureTitle = failureTitle;
	        self.sessionFailureMessage = failureMessage;
	        self.sessionFailureOverlayText = @"Could not start session command.";
	        [ISHDiagnosticsStore recordBreadcrumb:@"terminal.session.start.failed"
	                                      details:@{@"stage": @"do_execve",
	                                                @"error": @(err),
	                                                @"errorDescription": [AppDelegate descriptionForISHErrno:err],
	                                                @"command": commandString ?: @"",
	                                                @"path": command.firstObject ?: @""}];
	        return err;
    }
    self.sessionPid = current->pid;
    self.sessionStartedAt = CFAbsoluteTimeGetCurrent();
    [ISHDiagnosticsStore recordBreadcrumb:@"terminal.session.start.execed"
                                  details:@{@"pid": @(self.sessionPid),
                                            @"command": commandString ?: @"",
                                            @"path": command.firstObject ?: @""}];
    if (task_start(current) < 0) {
        [ISHDiagnosticsStore recordBreadcrumb:@"terminal.session.start.thread-failed"
                                      details:@{@"pid": @(self.sessionPid)}];
        struct task *failed = current;
        current = NULL;
        self.sessionPid = 0;
        task_never_ran_destroy(failed);
        return _EAGAIN;
    }
#else
    const char *argv_arr[command.count + 1];
    for (NSUInteger i = 0; i < command.count; i++)
        argv_arr[i] = command[i].UTF8String;
    argv_arr[command.count] = NULL;
    const char *envp_arr[] = {
        "TERM=screen-256color",
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

// If the launch command (e.g. "/bin/login -f root") exits this fast, treat it as a crash rather
// than a normal short-lived command, for the purposes of loop detection below.
static const NSTimeInterval kQuickSessionExitThreshold = 2.0;
// Stop auto-restarting after this many consecutive crash-fast exits, so an incompatible root
// filesystem (e.g. a login binary that a distro's PAM stack refuses on our pty) can't wedge the
// app in a tight respawn loop that burns CPU until iOS's watchdog kills it.
static const NSInteger kMaxConsecutiveQuickSessionExits = 3;

#if !ISH_LINUX
- (void)processExited:(NSNotification *)notif {
    int pid = [notif.userInfo[@"pid"] intValue];
    if (pid != self.sessionPid)
        return;

    Terminal *exitedTerminal = self.sessionTerminal;
    // A workspace-embedded terminal closes its window when its shell exits; only the main
    // full-screen terminal relaunches the login shell.
    dispatch_block_t didEndHandler = self.workspaceSessionDidEndHandler;
    NSTimeInterval elapsed = CFAbsoluteTimeGetCurrent() - self.sessionStartedAt;
    if (elapsed < kQuickSessionExitThreshold) {
        self.consecutiveQuickSessionExits++;
    } else {
        self.consecutiveQuickSessionExits = 0;
    }
    BOOL crashLooping = didEndHandler == nil && self.consecutiveQuickSessionExits >= kMaxConsecutiveQuickSessionExits;
    [ISHDiagnosticsStore recordBreadcrumb:@"terminal.session.processExited"
                                  details:@{@"pid": @(pid),
                                            @"sceneSession": self.sceneSession.persistentIdentifier ?: @"",
                                            @"restarting": @(didEndHandler == nil && !crashLooping),
                                            @"elapsed": @(elapsed),
                                            @"consecutiveQuickExits": @(self.consecutiveQuickSessionExits)}];
    self.sessionTerminal = nil;
    self.sessionPid = 0;
    [exitedTerminal setPendingDestroyReason:@"session-process-exited"];
    [exitedTerminal destroy];
    current = NULL; // it's been freed
    if (didEndHandler != nil) {
        // Defer to the next runloop turn: the handler tears down this view controller (removing
        // it from its parent can release the last reference), so don't run it while we're still
        // executing one of its instance methods.
        dispatch_async(dispatch_get_main_queue(), didEndHandler);
        return;
    }
    if (crashLooping) {
        self.consecutiveQuickSessionExits = 0;
        NSArray<NSString *> *command = UserPreferences.shared.launchCommand;
        NSString *commandString = [command componentsJoinedByString:@" "];
        [ISHDiagnosticsStore recordBreadcrumb:@"terminal.session.crashLoop.stopped"
                                      details:@{@"command": commandString ?: @""}];
        [self _showTerminalStartupFailureOverlayWithText:@"Session command keeps exiting immediately."];
        [self showMessage:@"Session command keeps exiting immediately"
                  subtitle:[NSString stringWithFormat:
                            @"\"%@\" exited right away several times in a row, so iSH stopped auto-restarting it to avoid a restart loop.\n\nThis usually means the root filesystem's login program isn't compatible with iSH (for example, it refuses root login on this pseudo-terminal). Try changing Settings -> Launch Command to something like \"/bin/sh\", or use a different root filesystem.",
                            commandString ?: @""]];
        return;
    }
    [self startNewSession];
}
#endif

#if ISH_LINUX
- (void)kernelPanicked:(NSNotification *)notif {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"panik" message:notif.userInfo[@"message"] preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}
#endif

- (void)showMessage:(NSString *)message subtitle:(NSString *)subtitle {
    dispatch_async(dispatch_get_main_queue(), ^{
        if (self.presentedViewController != nil)
            return;
        UIAlertController *alert = [UIAlertController alertControllerWithTitle:message message:subtitle preferredStyle:UIAlertControllerStyleAlert];
        [alert addAction:[UIAlertAction actionWithTitle:@"OK"
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
        self.dotKey.keyAppearance = keyAppearance;
        self.slashKey.keyAppearance = keyAppearance;
        self.dashKey.keyAppearance = keyAppearance;
        self.colonKey.keyAppearance = keyAppearance;
        self.bangKey.keyAppearance = keyAppearance;
        self.pipeKey.keyAppearance = keyAppearance;
        UIColor *tintColor = keyAppearance == UIKeyboardAppearanceLight ? UIColor.blackColor : UIColor.whiteColor;
        // Give the in-bar control buttons a key-like background so they stay visible on any terminal
        // theme. Without it they are bare glyphs that vanish on a dark terminal (they only showed in
        // the Workspace because they sat over the lighter desktop, not the black terminal).
        UIColor *controlBackground = keyAppearance == UIKeyboardAppearanceLight ?
            [UIColor colorWithWhite:1.0 alpha:0.9] :
            [UIColor colorWithWhite:1.0 alpha:0.18];
        for (UIControl *control in self.barControls) {
            control.tintColor = tintColor;
            control.backgroundColor = controlBackground;
            control.layer.cornerRadius = 6;
            control.layer.masksToBounds = YES;
        }
        self.terminalSwitcherButton.tintColor = tintColor;
        self.terminalSwitcherButton.backgroundColor = controlBackground;
        self.terminalSwitcherButton.layer.cornerRadius = 6;
        self.terminalSwitcherButton.layer.masksToBounds = YES;
        self.floatingTerminalSwitcherButton.tintColor = tintColor;
        self.floatingTerminalSwitcherButton.backgroundColor = keyAppearance == UIKeyboardAppearanceLight ?
            [UIColor colorWithWhite:1 alpha:0.78] :
            [UIColor colorWithWhite:0 alpha:0.45];
        self.workspaceButton.tintColor = tintColor;
        self.workspaceButton.backgroundColor = controlBackground;
        self.workspaceButton.layer.cornerRadius = 6;
        self.workspaceButton.layer.masksToBounds = YES;
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
    // Whether the bar is hidden with a hardware keyboard attached is entirely the user's
    // call via the "Hide extra keys with external keyboard" switch -- it used to be forced
    // on iPad regardless of that switch, which defeated the setting there and is exactly
    // what discussion #450 asked to have back (an Esc key alongside a hardware keyboard).
    BOOL hideAccessoryBar = self.hasExternalKeyboard && UserPreferences.shared.hideExtraKeysWithExternalKeyboard;
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
    NSString *badgeValue = showBadge ? @"Update available" : nil;
    self.infoButton.accessibilityValue = badgeValue;
    self.floatingSettingsButton.accessibilityValue = badgeValue;
}

- (UIStatusBarStyle)preferredStatusBarStyle {
    return UserPreferences.shared.statusBarStyle;
}

- (BOOL)prefersStatusBarHidden {
    return UserPreferences.shared.hideStatusBar;
}

// When "Maximize Screen Space" is enabled together with "Hide extra keys with external
// keyboard", drop the bottom safe-area inset so the terminal extends into the
// home-indicator strip. Only consulted on the external-keyboard layout paths, where the
// on-screen keyboard isn't covering the bottom of the screen.
- (CGFloat)_externalKeyboardBottomInset {
    if (UserPreferences.shared.maximizeScreenSpace &&
        UserPreferences.shared.hideExtraKeysWithExternalKeyboard)
        return 0;
    return self.view.safeAreaInsets.bottom;
}

// Mirror that decision into hterm's internal screen padding: with an external keyboard
// and maximize enabled, drop the 4pt edge padding too. No-op if the terminal webview
// hasn't loaded yet; a later keyboard-frame change re-applies it.
- (void)_applyScreenPadding {
    int padding = (UserPreferences.shared.maximizeScreenSpace && self.hasExternalKeyboard) ? 0 : 4;
    NSString *script = [NSString stringWithFormat:@"exports.setScreenPaddingSize(%d)", padding];
    [self.termView.terminal.webView evaluateJavaScript:script completionHandler:nil];
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
        self.bottomConstraint.constant = [self _externalKeyboardBottomInset];
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
    UIWindow *window = self.view.window;
    if (window == nil)
        return; // not installed in a window yet: nothing to lay the keyboard out against
    UIScreen *screen;
    if (@available(iOS 13.0, *)) {
        screen = window.windowScene.screen ?: UIScreen.mainScreen;
    } else {
        screen = UIScreen.mainScreen;
    }
    // Convert the keyboard frame through the WINDOW (fromView:), never through a UIScreen
    // coordinate space. The frame arrives in the window's screen space, but on iOS 16.1+
    // the keyboard is hosted in a separate scene that vends its OWN UIScreen wrapper for
    // the same physical display (identical bounds + shared UIScreenMode), and self.view's
    // window can resolve to a DIFFERENT wrapper. Converting a rect between two distinct
    // UIScreen objects is refused by UIKit ("Invalid UIScreen coordinate space conversion",
    // returning CGRectNull) -- which both spammed the log AND made this handler early-return
    // below, so keyboard avoidance silently stopped working on those setups (seen on an M4
    // iPad with a separate keyboard scene: two UIScreen wrappers, identical 1210x834 bounds).
    // A view<->window conversion is unambiguous and never crosses screens; for a full-screen
    // window it is identical to the old screen-space result, so the common case is unchanged.
    // (Matches Apple's documented keyboard-frame conversion, which uses fromView:self.view.window.)
    CGRect keyboardFrame = [self.view convertRect:screenKeyboardFrame fromView:window];
    // CGRectNull (from a refused conversion) is not equal to CGRectZero, so guard both --
    // otherwise an infinite/garbage rect slips through and gets mis-read as a hardware kbd.
    if (CGRectIsNull(keyboardFrame) || CGRectEqualToRect(keyboardFrame, CGRectZero))
        return;
    CGRect intersection = CGRectIntersection(keyboardFrame, self.view.bounds);
    keyboardFrame = intersection;
    // A short keyboard frame usually means a hardware keyboard is attached (only the
    // shortcut/assistant bar shows). But on iPad the *software* keyboard can also report
    // a short intersected frame in portrait, which used to mark it "external" and strip
    // the accessory bar entirely (hasExternalKeyboard also drives the safe-area/padding
    // math below, so misdetecting it was wrong even independent of the accessory bar).
    // Only trust the height heuristic when GameController confirms a hardware keyboard is
    // actually connected.
    BOOL smallKeyboard = keyboardFrame.size.height < 100;
    BOOL hardwareKeyboardPresent = smallKeyboard; // pre-iOS 14 fallback: old heuristic
    if (@available(iOS 14.0, *)) {
        hardwareKeyboardPresent = GCKeyboard.coalescedKeyboard != nil;
    }
    self.hasExternalKeyboard = smallKeyboard && hardwareKeyboardPresent;
    CGFloat pad = [self _externalKeyboardBottomInset];
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
    // A workspace floating terminal is positioned clear of the keyboard by the workspace, so the
    // terminal must fill its own window. Applying a keyboard inset here (the narrow floating window
    // also trips the "undocked keyboard" fallback above) only reserves a dead strip at the bottom
    // of the terminal that the cursor can't reach -- _updateSafeAreaCompensation already zeroes the
    // inset for the embedded case, so match it here.
    if (self.embeddedInWorkspaceWindow)
        pad = 0;
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

- (void)hardwareKeyboardDidChange:(NSNotification *)notification {
    self.hasExternalKeyboard = GCKeyboard.coalescedKeyboard != nil;
    [self _updateSafeAreaCompensation];
}

- (void)setHasExternalKeyboard:(BOOL)hasExternalKeyboard {
    BOOL changed = _hasExternalKeyboard != hasExternalKeyboard;
    _hasExternalKeyboard = hasExternalKeyboard;
    [self _updateStyleFromPreferences:YES];
    if (changed)
        [self _applyScreenPadding];
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
    // Rotating between portrait and landscape flips the vertical size class; re-evaluate
    // whether the '-' and ':' keys fit.
    if (previousTraitCollection.verticalSizeClass != self.traitCollection.verticalSizeClass)
        [self _updateCenterKeyVisibility];
}

#pragma mark Bar

- (IBAction)showAbout:(id)sender {
    UINavigationController *navigationController = [[UIStoryboard storyboardWithName:@"About" bundle:nil] instantiateInitialViewController];
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

    if (ISHLLMClientEnabled()) {
        [alert addAction:[UIAlertAction actionWithTitle:@"LLM Chat"
                                                  style:UIAlertActionStyleDefault
                                                handler:^(__unused UIAlertAction *action) {
            UINavigationController *navigationController = [[UINavigationController alloc] initWithRootViewController:ISHCreateLLMClientViewController()];
            [self presentViewController:navigationController animated:YES completion:nil];
        }]];
        NSString *terminalContext = Terminal_debugReadRows(self.terminal.type, self.terminal.number, 80) ?: @"";
        [alert addAction:[UIAlertAction actionWithTitle:@"LLM: Explain Current Terminal"
                                                  style:UIAlertActionStyleDefault
                                                handler:^(__unused UIAlertAction *action) {
            NSString *prompt = [NSString stringWithFormat:@"Explain the important details in this terminal output. If there is an error, identify the likely cause.\n\nTerminal output:\n```text\n%@\n```", terminalContext];
            UINavigationController *navigationController = [[UINavigationController alloc] initWithRootViewController:ISHCreateLLMClientViewControllerWithInitialPrompt(prompt)];
            [self presentViewController:navigationController animated:YES completion:nil];
        }]];
        [alert addAction:[UIAlertAction actionWithTitle:@"LLM: Suggest Fix"
                                                  style:UIAlertActionStyleDefault
                                                handler:^(__unused UIAlertAction *action) {
            NSString *prompt = [NSString stringWithFormat:@"Find the most likely error in this terminal output and suggest concrete commands or edits to fix it.\n\nTerminal output:\n```text\n%@\n```", terminalContext];
            UINavigationController *navigationController = [[UINavigationController alloc] initWithRootViewController:ISHCreateLLMClientViewControllerWithInitialPrompt(prompt)];
            [self presentViewController:navigationController animated:YES completion:nil];
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
    // No layoutIfNeeded here: this is called from BarView's layoutSubviews, right before
    // [super layoutSubviews] applies the constraints -- forcing layout mid-pass was at
    // best redundant and at worst reentrant.
}

- (void)setBarHorizontalPadding:(CGFloat)horizontal verticalPadding:(CGFloat)vertical buttonWidth:(CGFloat)buttonWidth {
    // Only touch constants that actually change: this runs from inside the bar's
    // layoutSubviews, and an unconditional write re-dirties layout every pass.
    if (self.barLeading.constant != horizontal)
        self.barLeading.constant = horizontal;
    if (self.barTrailing.constant != horizontal)
        self.barTrailing.constant = horizontal;
    if (self.barTop.constant != vertical)
        self.barTop.constant = vertical;
    if (self.barBottom.constant != vertical)
        self.barBottom.constant = vertical;
    if (self.barButtonWidth.constant != buttonWidth)
        self.barButtonWidth.constant = buttonWidth;
}

- (IBAction)pressEscape:(id)sender {
    [self pressKey:@"\x1b"];
}
- (IBAction)pressTab:(id)sender {
    [self pressKey:@"\t"];
}
- (IBAction)pressPeriod:(id)sender {
    [self pressKey:@"."];
}
- (IBAction)pressSlash:(id)sender {
    [self pressKey:@"/"];
}
- (IBAction)pressDash:(id)sender {
    [self pressKey:@"-"];
}
- (IBAction)pressColon:(id)sender {
    [self pressKey:@":"];
}
- (IBAction)pressBang:(id)sender {
    [self pressKey:@"!"];
}
- (IBAction)pressPipe:(id)sender {
    [self pressKey:@"|"];
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
    if (self.embeddedInWorkspaceWindow) {
        // Unpin: fall back to tracking the global preference again.
        self.termView.overrideFontSize = 0;
        return;
    }
    [self updatePersistentFontSize:UserPreferences.shared.defaultFontSize.doubleValue];
}

- (CGFloat)overrideFontSize {
    return self.termView.overrideFontSize;
}

- (void)setOverrideFontSize:(CGFloat)overrideFontSize {
    [self loadViewIfNeeded]; // termView is an outlet; restore may apply this early
    self.termView.overrideFontSize = overrideFontSize;
}

- (void)updatePersistentFontSize:(CGFloat)fontSize {
    NSInteger clampedFontSize = MAX(kMinimumTerminalFontSize, MIN(kMaximumTerminalFontSize, lround(fontSize)));
    if (self.embeddedInWorkspaceWindow) {
        // Workspace desktop windows size independently; the global preference
        // stays the default for windows that haven't been adjusted.
        self.termView.overrideFontSize = clampedFontSize;
        return;
    }
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
    [ISHDiagnosticsStore recordBreadcrumb:@"terminalVC.setTerminal"
                                  details:@{@"terminalUUID": terminal.uuid.UUIDString ?: @"",
                                            @"type": terminal != nil ? @(terminal.type) : @(-1),
                                            @"number": terminal != nil ? @(terminal.number) : @(-1)}];
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
    // Set the size-class-dependent constraint constants BEFORE super runs, so the pass
    // that follows lays the keys out against the right metrics.
    [self.terminalViewController resizeBar];
    // This override historically never called super -- but super is what applies this
    // view's Auto Layout constraints to its subviews (the key stack is pinned to the
    // input view's safe-area guide). Skipping it left the keys' frames stale whenever
    // UIKit invalidated the bar's layout after the fact (safe-area propagation into the
    // keyboard scene, trait changes), which could leave the bar visually drifted until
    // some unrelated layout pass happened to fix it up.
    [super layoutSubviews];
}

@end
