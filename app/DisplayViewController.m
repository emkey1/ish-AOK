#import "DisplayViewController.h"
#import "AboutViewController.h"
#import "DisplayRFBClient.h"
#import "DisplayRFBView.h"
#import "Terminal.h"
#import "AppDelegate.h"
#import "GuestFileBridge.h"
#import "UserPreferences.h"
#import "NSObject+SaneKVO.h"
#import <GameController/GameController.h>
#include "kernel/init.h"
#include "kernel/task.h"
#include "kernel/calls.h"
#include "kernel/fs.h"
#include "fs/devices.h"
#include "fs/path.h"

NS_ASSUME_NONNULL_BEGIN

// Written by /AOK/tools/start-wayland.sh once wayvnc is confirmed listening
// (its content is the guest TCP port, as ASCII decimal). Polled rather than
// parsed from the session's own stdout, which is carried over a real pty and
// would otherwise need unwinding from whatever escape sequences the shell
// emits, not a plain byte stream.
static NSString *const DisplayReadyGuestPath = @"/tmp/ish-display.ready";
static const NSTimeInterval DisplayReadyPollInterval = 0.3;
static NSArray<NSString *> *const DisplayPlainRootCommand = @[@"/bin/sh", @"/AOK/tools/start-wayland.sh"];

// "Open Everything as Default User" (UserPreferences.shouldLoginAsDefaultUser):
// TerminalViewController honors this by substituting the username inside an
// already-interactive `/bin/login -f root` and letting login itself drop
// privileges. This session isn't interactive -- it execve's a script
// directly, no login prompt involved -- so `su -c` is the equivalent for
// that case: root can su to any other user without a password, and `-`
// resets HOME/USER/LOGNAME/PATH to match a genuine login as that user
// instead of leaving the hardcoded root envp below in effect. No account is
// provisioned for this -- [AppDelegate defaultUserAccountName] looks up
// whatever's actually at UID 1000 on this rootfs; falls back to root if
// there isn't one.
static NSArray<NSString *> *DisplayGuestSessionCommand(void) {
    if (!UserPreferences.shared.shouldLoginAsDefaultUser)
        return DisplayPlainRootCommand;
    NSString *accountName = [AppDelegate defaultUserAccountName];
    if (accountName.length == 0)
        return DisplayPlainRootCommand;
    return @[@"/bin/su", @"-", accountName, @"-c", @"sh /AOK/tools/start-wayland.sh"];
}
static const NSTimeInterval DisplayReadyTimeout = 45.0;

typedef NS_ENUM(NSInteger, DisplayConnectionState) {
    DisplayConnectionStateIdle,
    DisplayConnectionStateStartingGuestSession,
    DisplayConnectionStateWaitingForReady,
    DisplayConnectionStateConnected,
    DisplayConnectionStateFailed,
};

@interface DisplayViewController () <DisplayRFBClientDelegate>
@end

@implementation DisplayViewController {
    UIView *_toolbarCard;
    UILabel *_statusLabel;
    UIButton *_ctrlAltDelButton;
    UIButton *_pasteButton;
    UIButton *_reconnectButton;
    UIButton *_Nullable _menuPip; // standalone mode only
    NSLayoutConstraint *_Nullable _menuPipBottomConstraint;
    // Standalone mode only: DisplayRFBView's accessory key strip, hosted
    // directly in this controller's view (NOT as an inputAccessoryView --
    // see accessoryBarExternallyHosted in DisplayRFBView.h for why), pinned
    // flush to the physical bottom edge. Collapsed to zero height when a
    // hardware keyboard is attached and the user asked to hide extra keys.
    UIView *_Nullable _accessoryStrip;
    NSLayoutConstraint *_Nullable _accessoryStripZeroHeightConstraint;
    DisplayRFBView *_displayView;
    // Two alternate top constraints for _displayView, swapped by
    // -_updateMaximizeScreenSpaceLayout: the normal one sits below the
    // toolbar card (safe-area-relative, like the toolbar itself); the
    // maximized one bypasses the toolbar and the safe area entirely,
    // mirroring how -displayView's bottom anchor already always does in
    // standalone mode. Only ever both non-nil once -displayView has run.
    NSLayoutConstraint *_Nullable _displayViewTopToToolbarConstraint;
    NSLayoutConstraint *_Nullable _displayViewTopToViewConstraint;

    // The guest session is owned exactly the way TerminalViewController owns
    // its shell session (a real pty via +[Terminal createPseudoTerminal:]),
    // but _sessionTerminal itself is never shown -- only _displayView (the
    // native RFB renderer) is user-visible. Tearing this Terminal down
    // hangs up the pty (SIGHUP), which start-wayland.sh's trap uses to tear
    // the whole guest Wayland stack down.
    Terminal *_Nullable _sessionTerminal;
    int _sessionPid; // the currently active session's pid, 0 if none

    // A prior session we've asked to tear down but haven't yet confirmed is
    // actually gone. -teardownSession only *initiates* teardown (hangs up
    // the pty) -- the guest side (start-wayland.sh's trap killing labwc/
    // foot/wayvnc, releasing the fixed WAYVNC_PORT) happens asynchronously,
    // whenever the guest scheduler gets to it. Starting a new session before
    // that's confirmed races the old labwc/foot/wayvnc for the same port and
    // leaves them un-reaped, piling up zombie generations that drag the
    // whole guest's scheduling down until the applet looks wedged (found via
    // a device backtrace showing 4 generations of labwc and 9 of foot alive
    // at once after repeated Reconnect clicks).
    int _teardownPid;
    BOOL _reconnectPendingAfterTeardown;

    DisplayRFBClient *_Nullable _rfbClient;
    DisplayConnectionState _state;
    NSDate *_Nullable _readyPollDeadline;
    BOOL _startedOnce;
}

#pragma mark - Lifecycle

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Wayland";

    _toolbarCard = [self workspaceThemeCardView];
    [self.toolContentView addSubview:_toolbarCard];

    _statusLabel = [self workspaceThemeSecondaryLabelWithTextStyle:UIFontTextStyleFootnote monospaced:NO];
    _statusLabel.text = @"Starting…";
    _statusLabel.numberOfLines = 1;
    _statusLabel.font = [UIFont systemFontOfSize:11.0];
    [_toolbarCard addSubview:_statusLabel];

    _ctrlAltDelButton = [self displayButtonWithTitle:@"Ctrl+Alt+Del" action:@selector(sendCtrlAltDel:)];
    [_toolbarCard addSubview:_ctrlAltDelButton];

    _pasteButton = [self displayButtonWithTitle:@"Paste" action:@selector(pasteToGuest:)];
    [_toolbarCard addSubview:_pasteButton];

    _reconnectButton = [self displayButtonWithTitle:@"Reconnect" action:@selector(reconnect:)];
    _reconnectButton.hidden = YES;
    [_toolbarCard addSubview:_reconnectButton];

    CGFloat inset = 8.0;
    NSMutableArray<NSLayoutConstraint *> *constraints = [NSMutableArray arrayWithArray:@[
        [_toolbarCard.topAnchor constraintEqualToAnchor:self.toolContentView.topAnchor constant:inset],
        [_toolbarCard.leadingAnchor constraintEqualToAnchor:self.toolContentView.leadingAnchor constant:inset],
        [_toolbarCard.trailingAnchor constraintEqualToAnchor:self.toolContentView.trailingAnchor constant:-inset],
        [_toolbarCard.heightAnchor constraintEqualToConstant:22.0],

        [_statusLabel.leadingAnchor constraintEqualToAnchor:_toolbarCard.leadingAnchor constant:10.0],
        [_statusLabel.centerYAnchor constraintEqualToAnchor:_toolbarCard.centerYAnchor],

        [_reconnectButton.trailingAnchor constraintEqualToAnchor:_toolbarCard.trailingAnchor constant:-6.0],
        [_reconnectButton.centerYAnchor constraintEqualToAnchor:_toolbarCard.centerYAnchor],

        [_ctrlAltDelButton.trailingAnchor constraintEqualToAnchor:_reconnectButton.leadingAnchor constant:-6.0],
        [_ctrlAltDelButton.centerYAnchor constraintEqualToAnchor:_toolbarCard.centerYAnchor],

        [_pasteButton.trailingAnchor constraintEqualToAnchor:_ctrlAltDelButton.leadingAnchor constant:-6.0],
        [_pasteButton.centerYAnchor constraintEqualToAnchor:_toolbarCard.centerYAnchor],
    ]];
    [constraints addObject:
        [_statusLabel.trailingAnchor constraintLessThanOrEqualToAnchor:_pasteButton.leadingAnchor constant:-6.0]];
    [NSLayoutConstraint activateConstraints:constraints];

    // Standalone (startup-mode) only: the same lower-right "Workspace menu"
    // pip the Workspace desktop shows (visual style copied from
    // WorkspaceViewController's makeModernMenuPip), so the corner menu button
    // is reachable from every mode. Added to self.view, not toolContentView,
    // so it floats above the display surface.
    if (self.standaloneMode) {
        UIButton *pip = [UIButton buttonWithType:UIButtonTypeSystem];
        pip.translatesAutoresizingMaskIntoConstraints = NO;
        [pip setImage:[UIImage systemImageNamed:@"line.3.horizontal"] forState:UIControlStateNormal];
        pip.tintColor = UIColor.whiteColor;
        NSDictionary<NSString *, UIColor *> *theme = [self workspaceTheme];
        pip.backgroundColor = theme[@"accent"] ?: [UIColor colorWithRed:0.20 green:0.48 blue:0.96 alpha:1.0];
        pip.layer.cornerRadius = 22.0;
        pip.layer.borderWidth = 1.5;
        pip.layer.borderColor = [UIColor colorWithWhite:1.0 alpha:0.85].CGColor;
        pip.layer.shadowColor = UIColor.blackColor.CGColor;
        pip.layer.shadowOpacity = 0.35;
        pip.layer.shadowRadius = 6.0;
        pip.layer.shadowOffset = CGSizeMake(0.0, 2.0);
        pip.accessibilityLabel = @"Display menu";
        [pip addTarget:self action:@selector(menuPipTapped:) forControlEvents:UIControlEventTouchUpInside];
        _menuPip = pip;
        [self.view addSubview:pip];
        _menuPipBottomConstraint = [pip.bottomAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.bottomAnchor constant:-16.0];
        [NSLayoutConstraint activateConstraints:@[
            [pip.trailingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.trailingAnchor constant:-16.0],
            _menuPipBottomConstraint,
            [pip.widthAnchor constraintEqualToConstant:44.0],
            [pip.heightAnchor constraintEqualToConstant:44.0],
        ]];
        // (The pip's bottom anchor is re-targeted onto the accessory strip
        // once that's set up below -- the strip is what actually occupies
        // the bottom edge in standalone mode.)
    }

    [NSNotificationCenter.defaultCenter addObserver:self
                                            selector:@selector(guestProcessExited:)
                                                name:ProcessExitedNotification
                                              object:nil];

    if (self.standaloneMode) {
        // Force -displayView's lazy creation now so its top-constraint pair
        // exists to update below, and apply the preference's current value
        // immediately (not just on a future change).
        [self displayView];
        [self _updateMaximizeScreenSpaceLayout];
        [UserPreferences.shared observe:@[@"maximizeScreenSpace"]
                                options:0 owner:self usingBlock:^(typeof(self) self) {
            dispatch_async(dispatch_get_main_queue(), ^{
                [self _updateMaximizeScreenSpaceLayout];
                [self setNeedsStatusBarAppearanceUpdate];
            });
        }];

        // Self-host the accessory key strip ONLY while a hardware keyboard is
        // attached (see -_updateAccessoryPresentationMode below): that's the
        // case UIKit's own keyboard-host window mishandles -- it bumps a
        // docked accessory view up to clear the home indicator the moment
        // the indicator becomes visible (first touch after launch) and never
        // lowers it again, which is exactly the observed "starts flush at
        // the bottom, then pops up and stays" drift. As a regular subview
        // pinned to the physical bottom edge, its position never moves.
        //
        // Without a hardware keyboard, UIKit's own inputAccessoryView
        // hosting is used instead (accessoryBarExternallyHosted toggled to
        // NO there): the self-hosted strip returning nil from
        // -inputAccessoryView while still holding first-responder status
        // left the real software keyboard never appearing at all (user-
        // confirmed, 2026-07-24 -- even a direct becomeFirstResponder retry
        // via the strip's own "Show Keyboard" key did nothing), suggesting
        // UIKit's normal keyboard presentation depends on actually being
        // asked for an accessory view, not on first-responder status +
        // UIKeyInput conformance alone. Splitting by hardware-keyboard
        // presence keeps the self-hosted fix for the case it was built for
        // while letting UIKit's own (apparently load-bearing) machinery run
        // the software-keyboard case, which self-hosting was never asked to
        // fix and may have been working fine before today's changes touched it.
        //
        // -accessoryKeyStack is embedded in a plain UIView WE build and
        // fully constrain here, deliberately NOT via allowsSelfSizing/the
        // stack's own safeAreaLayoutGuide the way -accessoryBar does it for
        // real inputAccessoryView hosting (see that method's comment) --
        // that self-sizing trick only works when UIKit's keyboard window is
        // actually the one hosting the view. Reusing it as a plain dangling
        // subview once left its height ambiguous, and Auto Layout resolved
        // the ambiguity by covering most of the screen, silently eating
        // every touch meant for the display surface underneath it
        // (2026-07-24 regression). Every dimension below is pinned to a
        // real, always-correct anchor -- this view's own leading/trailing/
        // bottom, or self.view.safeAreaLayoutGuide for the keys' bottom
        // clearance -- so the container's size is always fully determined.
        DisplayRFBView *displayView = self.displayView;
        UIStackView *stack = displayView.accessoryKeyStack;
        UIView *strip = [[UIView alloc] initWithFrame:CGRectZero];
        strip.translatesAutoresizingMaskIntoConstraints = NO;
        strip.clipsToBounds = YES;
        strip.backgroundColor = [UIColor colorWithWhite:0.11 alpha:1.0]; // approximates the system keyboard's dark chrome
        [self.view addSubview:strip];
        [strip addSubview:stack];
        _accessoryStrip = strip;
        NSLayoutYAxisAnchor *stripBottomAnchor;
        if (@available(iOS 17.0, *)) {
            // Rides the software keyboard's top when one is up; with
            // usesBottomSafeArea off, sits exactly on the view's bottom
            // edge (not the safe area's) when there's no keyboard.
            self.view.keyboardLayoutGuide.usesBottomSafeArea = NO;
            stripBottomAnchor = self.view.keyboardLayoutGuide.topAnchor;
        } else {
            // Pre-iOS-17 fallback: always flush at the bottom. A software
            // keyboard would cover the strip, but this mode's primary use
            // is with a hardware keyboard, and the strip's whole point is
            // the keys that keyboard lacks.
            stripBottomAnchor = self.view.bottomAnchor;
        }
        // Priority 999 on the strip-height-defining link: the required
        // zero-height override (hidden case, see -_updateAccessoryPresentationMode)
        // must be able to win without an unsatisfiable-constraint conflict.
        NSLayoutConstraint *stripTopFollowsStack = [strip.topAnchor constraintEqualToAnchor:stack.topAnchor constant:-6.0];
        stripTopFollowsStack.priority = UILayoutPriorityRequired - 1;
        _accessoryStripZeroHeightConstraint = [strip.heightAnchor constraintEqualToConstant:0.0];
        [NSLayoutConstraint activateConstraints:@[
            [strip.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
            [strip.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
            [strip.bottomAnchor constraintEqualToAnchor:stripBottomAnchor],
            stripTopFollowsStack,
            [stack.leadingAnchor constraintEqualToAnchor:strip.leadingAnchor constant:8.0],
            [stack.trailingAnchor constraintEqualToAnchor:strip.trailingAnchor constant:-8.0],
            [stack.heightAnchor constraintEqualToConstant:44.0],
            // The controller's OWN safe area (always correctly configured,
            // unlike a dangling subview's) -- keeps the keys clear of the
            // home indicator while the strip's background (bottom-anchored
            // above) still extends past it to the true edge.
            [stack.bottomAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.bottomAnchor constant:-6.0],
        ]];
        if (_menuPip != nil) {
            _menuPipBottomConstraint.active = NO;
            _menuPipBottomConstraint = [_menuPip.bottomAnchor constraintEqualToAnchor:strip.topAnchor constant:-16.0];
            _menuPipBottomConstraint.active = YES;
            [self.view bringSubviewToFront:_menuPip];
        }
        [self _updateAccessoryPresentationMode];
        if (@available(iOS 14.0, *)) {
            NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
            [center addObserver:self
                       selector:@selector(_hardwareKeyboardChangedForStrip:)
                           name:GCKeyboardDidConnectNotification
                         object:nil];
            [center addObserver:self
                       selector:@selector(_hardwareKeyboardChangedForStrip:)
                           name:GCKeyboardDidDisconnectNotification
                         object:nil];
        }
        [UserPreferences.shared observe:@[@"hideExtraKeysWithExternalKeyboard"]
                                options:0 owner:self usingBlock:^(typeof(self) self) {
            dispatch_async(dispatch_get_main_queue(), ^{
                [self _updateAccessoryPresentationMode];
            });
        }];
    }
}

// Picks which of the two accessory presentations is active, and shows/hides
// the self-hosted strip to match (see the setup comment above for the full
// reasoning): self-hosted while a hardware keyboard is attached (fixes the
// bar-drift bug that self-hosting exists for), UIKit-hosted inputAccessoryView
// otherwise (needed for the real software keyboard to appear at all). The
// self-hosted strip is ADDITIONALLY hidden -- same as -inputAccessoryView's
// own hide policy -- when a hardware keyboard is present AND the user asked
// to hide extra keys with one attached. The zero-height constraint collapses
// the strip's layout footprint too (a hidden view still occupies space under
// Auto Layout), which also drops the pip back to the bottom corner since
// it's anchored to the strip's top.
- (void)_updateAccessoryPresentationMode {
    if (_accessoryStrip == nil)
        return;
    BOOL hardwareKeyboardPresent = NO;
    if (@available(iOS 14.0, *)) {
        hardwareKeyboardPresent = GCKeyboard.coalescedKeyboard != nil;
    }
    BOOL selfHosted = hardwareKeyboardPresent;
    BOOL stripHidden = !selfHosted || (hardwareKeyboardPresent && UserPreferences.shared.hideExtraKeysWithExternalKeyboard);
    BOOL modeChanged = self.displayView.accessoryBarExternallyHosted != selfHosted;
    self.displayView.accessoryBarExternallyHosted = selfHosted;
    if (stripHidden != _accessoryStrip.hidden || _accessoryStripZeroHeightConstraint.active != stripHidden) {
        _accessoryStrip.hidden = stripHidden;
        _accessoryStripZeroHeightConstraint.active = stripHidden;
        [self.view setNeedsLayout];
    }
    // Switching hosting modes is a bigger change than reloadInputViews is
    // really meant for -- it's documented for "the SAME kind of accessory
    // view changed shape/content", not "stop being hosted by UIKit at all
    // and start being a plain subview instead" (or the reverse). A plain
    // reloadInputViews call here left the strip simply never reappearing on
    // a hardware-keyboard reconnect mid-session (user-reported, 2026-07-24)
    // -- UIKit's keyboard-presentation state apparently doesn't fully reset
    // across that boundary just from a reload. Force it: resign first
    // responder, then immediately reclaim it, so UIKit tears down whatever
    // it was presenting before re-evaluating -inputAccessoryView from a
    // clean slate.
    if (modeChanged && self.displayView.isFirstResponder) {
        [self.displayView resignFirstResponder];
        [self.displayView becomeFirstResponder];
    }
}

- (void)_hardwareKeyboardChangedForStrip:(NSNotification *)notification {
    // GCKeyboard notifications are not guaranteed to arrive on the main queue.
    dispatch_async(dispatch_get_main_queue(), ^{
        [self _updateAccessoryPresentationMode];
        // A hardware keyboard disconnecting mid-session is exactly the
        // no-hardware-keyboard condition -_autoShowKeyboardIfAppropriate
        // gates on; catches the "started with one attached, unplugged it
        // later" case, not just the at-connect one. Harmless no-op if a
        // keyboard just connected instead -- the guard bails immediately.
        [self _autoShowKeyboardIfAppropriate];
    });
}

// "Maximize Screen Space" (About > External Keyboard) is otherwise only
// wired up in TerminalViewController, where it drops the bottom safe-area
// inset with a hardware keyboard attached. Standalone Display mode already
// unconditionally extends -displayView's BOTTOM past the safe area (see
// -displayView's own comment), but its TOP never did -- the status bar and
// an always-visible toolbar card together reserve roughly the safe-area
// inset plus another ~38pt below it, all the time, regardless of this
// preference, which a user enabling "Maximize Screen Space" here would
// reasonably expect it to reclaim. When on: hide the status bar, collapse
// the toolbar card, and swap -displayView's top constraint to run flush to
// the view's real top edge. Ctrl+Alt+Del/Paste move to the pip's menu (see
// -menuPipTapped:) so they stay reachable with the toolbar hidden.
- (void)_updateMaximizeScreenSpaceLayout {
    BOOL maximize = UserPreferences.shared.maximizeScreenSpace;
    _toolbarCard.hidden = maximize;
    _displayViewTopToToolbarConstraint.active = !maximize;
    _displayViewTopToViewConstraint.active = maximize;
    [self.view setNeedsLayout];
}

- (BOOL)prefersStatusBarHidden {
    return self.standaloneMode && UserPreferences.shared.maximizeScreenSpace;
}

- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    if (!_startedOnce) {
        _startedOnce = YES;
        [self startGuestSession];
    }
}

// Standalone fullscreen: the desktop extends under the home indicator (see
// -displayView), so let the indicator fade out when idle like a video player.
- (BOOL)prefersHomeIndicatorAutoHidden {
    return self.standaloneMode;
}

// A rotation round trip has been observed leaving DisplayRFBView's own
// inputAccessoryView (the accessory key strip) stuck at a stale size/position
// -- UIKit's automatic keyboard-frame layout doesn't reliably resettle it
// against the new orientation on its own, matching the same class of "stale
// after rotation" issue -menuPipKeyboardDidSomething: has its own guard for
// above. Forcing a fresh reloadInputViews once the rotation animation
// finishes (not mid-transition, when the view's bounds are still animating)
// makes UIKit fully recompute the accessory view's layout against the new
// size, the same nudge -hardwareKeyboardDidChange: already gives it on a
// GCKeyboard connect/disconnect.
- (void)viewWillTransitionToSize:(CGSize)size withTransitionCoordinator:(id<UIViewControllerTransitionCoordinator>)coordinator {
    [super viewWillTransitionToSize:size withTransitionCoordinator:coordinator];
    [coordinator animateAlongsideTransition:nil completion:^(id<UIViewControllerTransitionCoordinatorContext> context) {
        if (self->_displayView.isFirstResponder)
            [self->_displayView reloadInputViews];
        [self _requestDesktopSizeForViewSize:size];
    }];
}

// Per-orientation compositor resolution (docs/wayland_rotation_resize_plan.md):
// instead of stretching the landscape-shaped canvas into a portrait viewport,
// ask the server to resize its actual output to match the orientation --
// wayvnc forwards SetDesktopSize to labwc via wlr-output-management on
// headless outputs, and the maximized windows reflow (all verified live
// on-device before this was built). Fixed 1280x720 <-> 720x1280 pair rather
// than deriving from the exact view aspect: it matches the wlroots headless
// default area, and a stable, predictable pair beats a slightly-truer aspect
// that changes with every device model. Standalone mode only: the windowed
// Workspace applet's canvas follows a user-resizable window, where a fixed
// per-orientation size makes no sense. Harmless when unsupported
// server-side: no confirmation rect ever arrives and everything stays as-is.
- (void)_requestDesktopSizeForViewSize:(CGSize)size {
    if (!self.standaloneMode || _rfbClient == nil)
        return;
    BOOL landscape = size.width >= size.height;
    [_rfbClient requestDesktopSizeWidth:(landscape ? 1280 : 720) height:(landscape ? 720 : 1280)];
}

- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:self];
    [self teardownSession];
}

- (UIButton *)displayButtonWithTitle:(NSString *)title action:(SEL)action {
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    button.translatesAutoresizingMaskIntoConstraints = NO;
    [button setTitle:title forState:UIControlStateNormal];
    button.titleLabel.font = [UIFont systemFontOfSize:11.0 weight:UIFontWeightSemibold];
    button.contentEdgeInsets = UIEdgeInsetsZero;
    [button addTarget:self action:action forControlEvents:UIControlEventTouchUpInside];
    return button;
}

#pragma mark - Guest session

- (void)startGuestSession {
    if (_state != DisplayConnectionStateIdle && _state != DisplayConnectionStateFailed)
        return;
    _reconnectButton.hidden = YES;
    _state = DisplayConnectionStateStartingGuestSession;
    _statusLabel.text = @"Starting Wayland session…";
    _readyPollDeadline = nil;

    intptr_t err = [AppDelegate ensureBooted];
    if (err < 0) {
        [self failWithMessage:[NSString stringWithFormat:@"Boot failed: %@", [AppDelegate descriptionForISHErrno:err]]];
        return;
    }

    err = become_new_init_child();
    if (err < 0) {
        [self failWithMessage:[NSString stringWithFormat:@"Could not create guest session: %@",
                                [AppDelegate descriptionForISHErrno:err]]];
        return;
    }

    // Stale copies of the ready/error files can be owned by a different uid
    // than this session will run as: a root session leaves them root-owned,
    // and an "Open Everything as Default User" session (su - <user>) can then
    // neither remove nor overwrite them in sticky /tmp. Observed on-device as
    // rm/tee EACCES cascading into the compositor dying before its socket
    // existed. Unlink them here with root credentials so every session starts
    // clean no matter who ran the last one. (ENOENT is the normal case.)
    generic_unlinkat(AT_PWD, "/tmp/ish-display.ready");
    generic_unlinkat(AT_PWD, "/tmp/ish-display.ready.error");

    struct tty *tty;
    Terminal *terminal = [Terminal createPseudoTerminal:&tty];
    if (terminal == nil) {
        [self failWithMessage:@"Could not allocate a pseudo-terminal for the Wayland session"];
        return;
    }
    _sessionTerminal = terminal;
    NSString *stdioFile = [NSString stringWithFormat:@"/dev/pts/%d", tty->num];
    err = create_stdio(stdioFile.fileSystemRepresentation, TTY_PSEUDO_SLAVE_MAJOR, tty->num);
    if (err < 0) {
        [self failWithMessage:[NSString stringWithFormat:@"Could not attach session I/O: %@",
                                [AppDelegate descriptionForISHErrno:err]]];
        return;
    }
    tty_release(tty);

    NSArray<NSString *> *command = DisplayGuestSessionCommand();
    char argv[4096];
    [Terminal convertCommand:command toArgs:argv limitSize:sizeof(argv)];
    const char *envp = "PATH=/AOK/persist/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin\0"
                        "HOME=/root\0"
                        "TERM=xterm\0";
    err = do_execve(command[0].UTF8String, command.count, argv, envp);
    if (err < 0 && ![command isEqualToArray:DisplayPlainRootCommand]) {
        // "su" missing (or otherwise failed) on this root -- fall back to
        // running as root rather than failing the whole session over a
        // preference that's a nice-to-have, not a hard requirement.
        command = DisplayPlainRootCommand;
        [Terminal convertCommand:command toArgs:argv limitSize:sizeof(argv)];
        err = do_execve(command[0].UTF8String, command.count, argv, envp);
    }
    if (err < 0) {
        [self failWithMessage:[NSString stringWithFormat:
            @"Could not start /AOK/tools/start-wayland.sh: %@\n\nIf labwc/foot/wayvnc aren't "
            @"installed yet, run 'sudo sh /AOK/tools/setup-wayland.sh' in a terminal first.",
            [AppDelegate descriptionForISHErrno:err]]];
        return;
    }
    _sessionPid = current->pid;
    if (task_start(current) < 0) {
        struct task *failed = current;
        current = NULL;
        _sessionPid = 0;
        task_never_ran_destroy(failed);
        [self failWithMessage:@"Could not start the Wayland session task"];
        return;
    }

    _state = DisplayConnectionStateWaitingForReady;
    _statusLabel.text = @"Waiting for compositor…";
    [self pollForReadyFile];
}

- (void)pollForReadyFile {
    if (_state != DisplayConnectionStateWaitingForReady)
        return;
    if (_readyPollDeadline == nil)
        _readyPollDeadline = [NSDate dateWithTimeIntervalSinceNow:DisplayReadyTimeout];
    if (_readyPollDeadline.timeIntervalSinceNow < 0) {
        [self failWithMessage:@"Timed out waiting for the Wayland compositor to start.\n\n"
                                "If labwc/foot/wayvnc aren't installed, run "
                                "'sudo sh /AOK/tools/setup-wayland.sh' in a terminal, then Reconnect."];
        return;
    }
    __weak typeof(self) weakSelf = self;
    [ISHGuestFileBridge.sharedBridge readFileAtGuestPath:DisplayReadyGuestPath
                                                 maxBytes:64
                                               completion:^(NSData *_Nullable data, NSError *_Nullable error) {
        typeof(self) strongSelf = weakSelf;
        if (strongSelf == nil || strongSelf->_state != DisplayConnectionStateWaitingForReady)
            return;
        if (data != nil) {
            NSString *text = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
            NSInteger port = text.integerValue;
            if (port > 0 && port <= UINT16_MAX) {
                [strongSelf connectRFBToGuestPort:(uint16_t) port];
                return;
            }
        }
        // start-wayland.sh writes this alongside (never instead of) the
        // ready file whenever it die()s -- labwc/foot/wayvnc missing,
        // crashing, or never reaching a listening state -- so a real
        // failure surfaces immediately instead of only ever producing this
        // poll's generic timeout message after the full deadline.
        [ISHGuestFileBridge.sharedBridge readFileAtGuestPath:[DisplayReadyGuestPath stringByAppendingString:@".error"]
                                                     maxBytes:4096
                                                   completion:^(NSData *_Nullable errorData, NSError *_Nullable readError) {
            typeof(self) innerSelf = weakSelf;
            if (innerSelf == nil || innerSelf->_state != DisplayConnectionStateWaitingForReady)
                return;
            if (errorData.length > 0) {
                NSString *reason = [[NSString alloc] initWithData:errorData encoding:NSUTF8StringEncoding];
                [innerSelf failWithMessage:[NSString stringWithFormat:@"start-wayland.sh failed:\n\n%@", reason]];
                return;
            }
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t) (DisplayReadyPollInterval * NSEC_PER_SEC)),
                            dispatch_get_main_queue(), ^{
                [innerSelf pollForReadyFile];
            });
        }];
    }];
}

- (void)connectRFBToGuestPort:(uint16_t)guestPort {
    _state = DisplayConnectionStateConnected; // optimistic; refined by DisplayRFBClientDelegate callbacks
    _statusLabel.text = @"Connecting to compositor…";
    _rfbClient = [DisplayRFBClient new];
    _rfbClient.delegate = self;
    [_rfbClient connectToGuestPort:guestPort];
}

- (void)teardownSession {
    [_rfbClient disconnect];
    _rfbClient = nil;
    _displayView.rfbClient = nil;
    Terminal *terminal = _sessionTerminal;
    _sessionTerminal = nil;
    if (_sessionPid != 0) {
        _teardownPid = _sessionPid;
        _sessionPid = 0;
    }
    [terminal setPendingDestroyReason:@"display-applet-teardown"];
    [terminal destroy];
}

- (void)guestProcessExited:(NSNotification *)notification {
    int pid = [notification.userInfo[@"pid"] intValue];
    if (pid == 0)
        return;
    if (pid == _teardownPid) {
        // Confirms the guest side actually finished (labwc/foot/wayvnc
        // killed, WAYVNC_PORT released) -- see the _teardownPid doc above
        // for why this can't just be assumed once -teardownSession returns.
        _teardownPid = 0;
        if (_reconnectPendingAfterTeardown) {
            _reconnectPendingAfterTeardown = NO;
            [self startGuestSession];
        }
        return;
    }
    if (pid != _sessionPid)
        return;
    _sessionPid = 0;
    _sessionTerminal = nil; // the kernel already tore this down; don't double-destroy it
    [_rfbClient disconnect];
    _rfbClient = nil;
    _displayView.rfbClient = nil;
    __weak typeof(self) weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        typeof(self) strongSelf = weakSelf;
        if (strongSelf == nil)
            return;
        BOOL diedDuringStartup = strongSelf->_state == DisplayConnectionStateStartingGuestSession
            || strongSelf->_state == DisplayConnectionStateWaitingForReady;
        strongSelf->_state = DisplayConnectionStateFailed;
        strongSelf->_statusLabel.text = @"Wayland session ended";
        strongSelf->_reconnectButton.hidden = NO;
        // A session that exits before ever reaching Connected means
        // start-wayland.sh die()d (or never ran) -- and this exit
        // notification always wins the race against pollForReadyFile's
        // .error read, so without this every guest-side startup failure
        // presented as the bare "Wayland session ended" with the actual
        // reason left unread in the guest. Fetch and show it.
        if (diedDuringStartup)
            [strongSelf showStartupExitReason];
    });
}

- (void)showStartupExitReason {
    __weak typeof(self) weakSelf = self;
    [ISHGuestFileBridge.sharedBridge readFileAtGuestPath:[DisplayReadyGuestPath stringByAppendingString:@".error"]
                                                 maxBytes:4096
                                               completion:^(NSData *_Nullable errorData, NSError *_Nullable readError) {
        typeof(self) strongSelf = weakSelf;
        if (strongSelf == nil || strongSelf->_state != DisplayConnectionStateFailed)
            return;
        if (errorData.length > 0) {
            NSString *reason = [[NSString alloc] initWithData:errorData encoding:NSUTF8StringEncoding];
            [strongSelf failWithMessage:[NSString stringWithFormat:@"Wayland session ended:\n\n%@", reason]];
            return;
        }
        // No error file: the script never got far enough to write one (or the
        // failure was outside its die() paths). Point at the places the
        // evidence actually lands. The su note matters because the
        // default-user preference launches the script via `su - <user> -c`,
        // whose runtime failures (e.g. a nologin shell) exit before the
        // script ever runs.
        NSMutableString *message = [@"Wayland session ended before the compositor came up.\n\n"
                                    @"Check /tmp/ish-wayland-debug.log in a terminal, or run "
                                    @"'sh /AOK/tools/start-wayland.sh' there to see the failure directly." mutableCopy];
        if (UserPreferences.shared.shouldLoginAsDefaultUser)
            [message appendString:@"\n\nIf that works, the failure is in the \"Open Everything as "
                                  @"Default User\" su path -- try disabling that setting."];
        [strongSelf failWithMessage:message];
    }];
}

- (void)failWithMessage:(NSString *)message {
    _state = DisplayConnectionStateFailed;
    __weak typeof(self) weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        typeof(self) strongSelf = weakSelf;
        if (strongSelf == nil)
            return;
        strongSelf->_statusLabel.text = message;
        strongSelf->_statusLabel.numberOfLines = 0;
        strongSelf->_reconnectButton.hidden = NO;
    });
}

- (void)reconnect:(id)sender {
    if (_reconnectPendingAfterTeardown)
        return; // already reconnecting; ignore extra taps while we wait for the old session to actually exit
    BOOL hadActiveSession = _sessionPid != 0;
    [self teardownSession];
    _statusLabel.numberOfLines = 1;
    _reconnectButton.hidden = YES;
    _state = DisplayConnectionStateIdle;

    if (!hadActiveSession) {
        [self startGuestSession];
        return;
    }

    // Deferred until -guestProcessExited: confirms the old session's guest
    // processes actually exited -- see the _teardownPid doc above for why.
    _statusLabel.text = @"Disconnecting…";
    _reconnectPendingAfterTeardown = YES;
    __weak typeof(self) weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t) (5.0 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        typeof(self) strongSelf = weakSelf;
        // Fallback in case the exit notification never arrives (e.g. the
        // old session was already gone by the time we asked to tear it
        // down) -- don't leave Reconnect stuck waiting forever.
        if (strongSelf == nil || !strongSelf->_reconnectPendingAfterTeardown)
            return;
        strongSelf->_reconnectPendingAfterTeardown = NO;
        strongSelf->_teardownPid = 0;
        [strongSelf startGuestSession];
    });
}

// (The old menuPipKeyboardDidSomething: keyboard-frame chasing is gone: the
// pip is now anchored directly to the self-hosted accessory strip's top, and
// the strip itself rides the keyboardLayoutGuide -- both follow the keyboard
// automatically through plain Auto Layout, with no frame math to get wrong.)

// Standalone-only lower-right pip: mirrors the Workspace desktop's corner
// menu button so it's reachable from every mode. Workspace-desktop actions
// (windows, desktops, launcher) don't exist here; this carries the ones that
// do. Ctrl+Alt+Del/Paste are normally on the toolbar card, but that's hidden
// under "Maximize Screen Space" (see -_updateMaximizeScreenSpaceLayout) --
// carry them here too so they're never the ONLY way to reach those actions.
- (void)menuPipTapped:(UIButton *)sender {
    UIAlertController *sheet =
        [UIAlertController alertControllerWithTitle:@"Wayland Display"
                                            message:nil
                                     preferredStyle:UIAlertControllerStyleActionSheet];
    __weak typeof(self) weakSelf = self;
    [sheet addAction:[UIAlertAction actionWithTitle:@"Send Ctrl+Alt+Del"
                                              style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction *action) {
        [weakSelf sendCtrlAltDel:sender];
    }]];
    [sheet addAction:[UIAlertAction actionWithTitle:@"Paste"
                                              style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction *action) {
        [weakSelf pasteToGuest:sender];
    }]];
    // Same global preference + toggle UX as WorkspaceViewController's
    // "Workspace" root menu (-presentDesktopRootMenuFromView:sourceRect:) --
    // Display mode has no other way to reach it, and without exposing it
    // here a user who turned it off in Workspace has no way back to auto-
    // focus in Wayland mode short of switching back to Workspace to flip it.
    BOOL autoShowKeyboard = UserPreferences.shared.autoShowKeyboard;
    [sheet addAction:[UIAlertAction actionWithTitle:[NSString stringWithFormat:@"Auto-Show Keyboard: %@", autoShowKeyboard ? @"On" : @"Off"]
                                              style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction *action) {
        UserPreferences.shared.autoShowKeyboard = !autoShowKeyboard;
        [weakSelf _autoShowKeyboardIfAppropriate];
        // Re-present so the toggled state is reflected, matching the same
        // deferred re-present WorkspaceViewController's handler uses.
        dispatch_async(dispatch_get_main_queue(), ^{
            [weakSelf menuPipTapped:sender];
        });
    }]];
    // GH #529: no way to dismiss the on-screen keyboard in standalone Wayland
    // mode short of leaving the session (Terminal has hideKeyboardButton for
    // exactly this; Display had nothing). resignFirstResponder is a no-op if
    // the keyboard isn't up, so this is safe to always offer rather than
    // tracking first-responder state just to conditionally hide the action.
    [sheet addAction:[UIAlertAction actionWithTitle:@"Hide Keyboard"
                                              style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction *action) {
        [weakSelf.displayView resignFirstResponder];
    }]];
    [sheet addAction:[UIAlertAction actionWithTitle:@"Open Workspace…"
                                              style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction *action) {
        [weakSelf switchToWorkspace:sender];
    }]];
    [sheet addAction:[UIAlertAction actionWithTitle:@"Settings"
                                              style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction *action) {
        // Form sheet: dismissable by swipe-down, so Settings can't strand the
        // session (the About screen has no Done button of its own when it
        // isn't hosted in a Workspace window).
        UINavigationController *settings = ISHCreateAboutNavigationController(NO, NO);
        settings.modalPresentationStyle = UIModalPresentationFormSheet;
        [weakSelf presentViewController:settings animated:YES completion:nil];
    }]];
    [sheet addAction:[UIAlertAction actionWithTitle:@"Reconnect"
                                              style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction *action) {
        [weakSelf reconnect:sender];
    }]];
    [sheet addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    UIPopoverPresentationController *popover = sheet.popoverPresentationController;
    if (popover != nil) {
        popover.sourceView = sender;
        popover.sourceRect = sender.bounds;
    }
    [self presentViewController:sheet animated:YES completion:nil];
}

// Standalone (startup-mode) escape hatch: swap the scene's root over to the
// Workspace. Releasing this controller tears the guest Wayland session down
// (dealloc -> teardownSession -> pty SIGHUP), exactly like closing the
// windowed applet -- which also means the Workspace's own Display applet can
// then start a fresh session without racing the old one for WAYVNC_PORT.
- (void)switchToWorkspace:(id)sender {
    UIAlertController *alert =
        [UIAlertController alertControllerWithTitle:@"Open Workspace?"
                                            message:@"This ends the current Wayland session. You can reopen it from the Workspace's Display applet."
                                     preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    __weak typeof(self) weakSelf = self;
    [alert addAction:[UIAlertAction actionWithTitle:@"Open Workspace"
                                              style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction *action) {
        UIWindow *window = weakSelf.view.window;
        if (window == nil)
            return;
        window.rootViewController = ISHCreateWorkspaceNavigationController();
    }]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)sendCtrlAltDel:(id)sender {
    [_rfbClient sendCtrlAltDel];
}

- (void)pasteToGuest:(id)sender {
    NSString *text = UIPasteboard.generalPasteboard.string;
    if (text.length == 0)
        return;
    [_rfbClient sendClientCutText:text];
}

#pragma mark - DisplayRFBView

// No card wrapper: unlike the toolbar, the display fills all remaining
// space edge-to-edge (no rounded corners/border/shadow) so the remote
// framebuffer gets as much room as possible.
- (DisplayRFBView *)displayView {
    if (_displayView == nil) {
        _displayView = [[DisplayRFBView alloc] initWithFrame:CGRectZero];
        [self.toolContentView addSubview:_displayView];
        // Standalone fullscreen: extend to the view's real bottom edge, not
        // the safe-area-inset toolContentView -- otherwise the home-indicator
        // inset leaves a dead black band under the desktop. (toolContentView
        // doesn't clip, so a subview may extend past its bottom.) The rounded
        // corners only make sense inside a Workspace window; at the physical
        // screen edge they'd just notch the desktop.
        NSLayoutYAxisAnchor *bottomAnchor = self.standaloneMode
            ? self.view.bottomAnchor
            : self.toolContentView.bottomAnchor;
        if (self.standaloneMode)
            _displayView.layer.cornerRadius = 0.0;
        // Two alternate top constraints (see the ivar comments and
        // -_updateMaximizeScreenSpaceLayout): only one is ever active at a
        // time, chosen by that method rather than here, so toggling
        // "Maximize Screen Space" live doesn't need to recreate anything.
        _displayViewTopToToolbarConstraint =
            [_displayView.topAnchor constraintEqualToAnchor:_toolbarCard.bottomAnchor constant:8.0];
        _displayViewTopToViewConstraint =
            [_displayView.topAnchor constraintEqualToAnchor:self.view.topAnchor];
        _displayViewTopToViewConstraint.active = NO;
        [NSLayoutConstraint activateConstraints:@[
            _displayViewTopToToolbarConstraint,
            [_displayView.leadingAnchor constraintEqualToAnchor:self.toolContentView.leadingAnchor],
            [_displayView.trailingAnchor constraintEqualToAnchor:self.toolContentView.trailingAnchor],
            [_displayView.bottomAnchor constraintEqualToAnchor:bottomAnchor],
        ]];
        if (_menuPip != nil)
            [self.view bringSubviewToFront:_menuPip];
    }
    return _displayView;
}

#pragma mark - DisplayRFBClientDelegate

- (void)rfbClientDidConnect:(DisplayRFBClient *)client {
    if (client != _rfbClient)
        return;
    self.displayView.rfbClient = client;
    if (client.desktopName.length > 0)
        self.title = client.desktopName;
    _state = DisplayConnectionStateConnected;
    _statusLabel.text = @"Connected";
    _statusLabel.numberOfLines = 1;
    _reconnectButton.hidden = YES;
    // The session always starts at the compositor's landscape default; if
    // the app launched (or reconnected) while the device is in portrait,
    // bring the output in line with the current orientation right away
    // rather than waiting for the next physical rotation.
    [self _requestDesktopSizeForViewSize:self.view.bounds.size];
    [self _autoShowKeyboardIfAppropriate];
}

// Mirrors TerminalViewController's -viewDidLoad/-focusTerminal gating on
// the same preference. DisplayRFBView otherwise only becomes first
// responder from a touch (-touchesBegan:), so without this the software
// keyboard never appears on its own even with the preference on -- there
// was previously no auto-focus call site here at all to gate.
//
// Additionally gated on there being no hardware keyboard attached (user
// request, 2026-07-24): with one attached, becoming first responder would
// only bring up the accessory strip anyway (real software keyboards don't
// render alongside hardware input), so forcing a touch-keyboard-shaped
// grab of first-responder status there is pure downside -- it steals a
// touch-driven decision the user might make deliberately later, for zero
// benefit. Auto-show is meant for the touch-only case.
- (void)_autoShowKeyboardIfAppropriate {
    if (!UserPreferences.shared.autoShowKeyboard)
        return;
    if (@available(iOS 14.0, *)) {
        if (GCKeyboard.coalescedKeyboard != nil)
            return;
    }
    [self.displayView becomeFirstResponder];
}

- (void)rfbClientDidUpdateFramebuffer:(DisplayRFBClient *)client {
    if (client != _rfbClient)
        return;
    [_displayView setNeedsDisplay];
}

- (void)rfbClient:(DisplayRFBClient *)client didUpdateCursorWithWidth:(uint16_t)width height:(uint16_t)height
         hotspotX:(uint16_t)hotspotX hotspotY:(uint16_t)hotspotY bgra:(NSData *)bgra {
    if (client != _rfbClient)
        return;
    [_displayView updateCursorWithWidth:width height:height hotspotX:hotspotX hotspotY:hotspotY bgra:bgra];
}

- (void)rfbClient:(DisplayRFBClient *)client didReceiveServerCutText:(NSString *)text {
    if (client != _rfbClient)
        return;
    UIPasteboard.generalPasteboard.string = text;
}

- (void)rfbClient:(DisplayRFBClient *)client didFailWithMessage:(NSString *)message {
    if (client != _rfbClient)
        return;
    [self failWithMessage:message];
}

@end

NS_ASSUME_NONNULL_END
