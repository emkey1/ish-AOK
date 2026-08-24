//
//  SceneDelegate.m
//  iSH
//
//  Created by Theodore Dubois on 10/26/19.
//

#import <objc/runtime.h>
#import "SceneDelegate.h"
#import "AboutViewController.h"
#import "AppDelegate.h"
#import "Diagnostics.h"
#import "DisplayViewController.h"
#import "NSObject+SaneKVO.h"
#import "Roots.h"
#import "RootsTableViewController.h"
#import "ShortcutActivityBridge.h"
#import "UserPreferences.h"
#import "WorkspaceViewController.h"

TerminalViewController *currentTerminalViewController = NULL;

@interface SceneDelegate ()

@property NSString *terminalUUID;
@property BOOL waitingForInitialRootImport;

@end

static NSString *const ISHSceneActivityTypeLegacy = @"app.ish.scene";
NSString *const ISHSceneActivityTypeTerminal = @"app.ish.scene.terminal";
NSString *const ISHSceneActivityTypeWorkspace = @"app.ish.scene.workspace";
NSString *const ISHSceneTerminalUUIDUserInfoKey = @"TerminalUUID";
NSString *const ISHSceneTerminalDisplayModeUserInfoKey = @"TerminalDisplayMode";
NSString *const ISHSceneWorkspaceToolUserInfoKey = @"WorkspaceTool";
static NSString *const ISHSceneTerminalDisplayModeSessionShellValue = @"session-shell";
static NSString *const ISHSceneTerminalDisplayModeSystemConsoleValue = @"system-console";

static BOOL ISHShouldChooseFilesystemAtStartup(void) {
    NSString *initialWindow = [NSUserDefaults.standardUserDefaults stringForKey:kPreferenceInitialWindowKey];
    return [initialWindow isEqualToString:ISHInitialWindowChooseFilesystemValue];
}

static UIViewController *CreateRootSelectionViewController(BOOL choosesRootOnSelection, void (^ _Nullable rootSelectionHandler)(NSString *rootName)) {
    UIViewController *rootsViewController = [[UIStoryboard storyboardWithName:@"Roots" bundle:nil] instantiateInitialViewController];
    UINavigationController *navigationController = [[UINavigationController alloc] initWithRootViewController:rootsViewController];
    if ([rootsViewController isKindOfClass:RootsTableViewController.class]) {
        RootsTableViewController *rootsTableViewController = (RootsTableViewController *) rootsViewController;
        rootsTableViewController.choosesRootOnSelection = choosesRootOnSelection;
        rootsTableViewController.rootSelectionHandler = rootSelectionHandler;
        if (choosesRootOnSelection)
            rootsTableViewController.title = @"Choose Filesystem";
    }
    return navigationController;
}

static UIViewController *CreateStandaloneDisplayViewController(void) {
    DisplayViewController *displayViewController = [DisplayViewController new];
    displayViewController.standaloneMode = YES;
    return displayViewController;
}

static TerminalViewController *CreateTerminalViewController(void) {
    UIViewController *viewController = [[UIStoryboard storyboardWithName:@"Terminal" bundle:nil] instantiateInitialViewController];
    return [viewController isKindOfClass:TerminalViewController.class] ? (TerminalViewController *) viewController : nil;
}

#pragma mark - Session Shell <-> Workspace switching (GH #546)

// Each mode's root view controller is kept alive on the window it was shown in,
// so switching modes is a swap between two live controllers rather than a
// rebuild. That is the whole point: a rebuilt Workspace comes back empty, with
// every tool window closed and the desktop layout reset, and a rebuilt terminal
// would start a second session on top of the one already running.
static const void *kISHWindowWorkspaceRootKey = &kISHWindowWorkspaceRootKey;
static const void *kISHWindowTerminalRootKey = &kISHWindowTerminalRootKey;

// The Workspace root is a UINavigationController wrapping the WorkspaceViewController
// (see ISHCreateWorkspaceNavigationControllerForTool), so test the wrapper's root too.
static BOOL ISHViewControllerIsWorkspaceRoot(UIViewController *viewController) {
    if (viewController == nil)
        return NO;
    if ([viewController isKindOfClass:WorkspaceViewController.class])
        return YES;
    if ([viewController isKindOfClass:UINavigationController.class]) {
        UIViewController *root = ((UINavigationController *) viewController).viewControllers.firstObject;
        return [root isKindOfClass:WorkspaceViewController.class];
    }
    return NO;
}

BOOL ISHWindowIsShowingWorkspace(UIWindow *window) {
    return ISHViewControllerIsWorkspaceRoot(window.rootViewController);
}

// Only the two switchable modes are remembered. Any other root (the filesystem
// picker, the standalone Wayland display, Settings owning its own scene) is
// transient and must not come back as "the" shell or workspace later.
static void ISHWindowRememberRoot(UIWindow *window, UIViewController *root) {
    if (root == nil)
        return;
    if (ISHViewControllerIsWorkspaceRoot(root))
        objc_setAssociatedObject(window, kISHWindowWorkspaceRootKey, root, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    else if ([root isKindOfClass:TerminalViewController.class])
        objc_setAssociatedObject(window, kISHWindowTerminalRootKey, root, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}

static void ISHWindowTransitionToRoot(UIWindow *window, UIViewController *newRoot) {
    ISHWindowRememberRoot(window, window.rootViewController);
    [UIView transitionWithView:window
                      duration:0.3
                       options:UIViewAnimationOptionTransitionCrossDissolve
                    animations:^{
        // Cross-dissolve the swap itself, but don't let the incoming controller
        // animate its own initial layout inside that transition.
        BOOL wereAnimationsEnabled = UIView.areAnimationsEnabled;
        [UIView setAnimationsEnabled:NO];
        window.rootViewController = newRoot;
        [UIView setAnimationsEnabled:wereAnimationsEnabled];
    }
                    completion:nil];
    [window makeKeyAndVisible];
    // Mirror -sceneDidBecomeActive:'s bookkeeping; the root changed without one.
    currentTerminalViewController = [newRoot isKindOfClass:TerminalViewController.class]
        ? (TerminalViewController *) newRoot
        : NULL;
}

void ISHWindowShowWorkspace(UIWindow *window) {
    if (window == nil || ISHWindowIsShowingWorkspace(window))
        return;
    UIViewController *workspace = objc_getAssociatedObject(window, kISHWindowWorkspaceRootKey);
    BOOL restored = workspace != nil;
    if (!restored)
        workspace = ISHCreateWorkspaceNavigationControllerForTool(nil);
    [ISHDiagnosticsStore recordBreadcrumb:@"scene.mode.workspace"
                                  details:@{@"restored": @(restored)}];
    ISHWindowTransitionToRoot(window, workspace);
}

void ISHWindowShowSessionShell(UIWindow *window) {
    if (window == nil || [window.rootViewController isKindOfClass:TerminalViewController.class])
        return;
    TerminalViewController *terminal = objc_getAssociatedObject(window, kISHWindowTerminalRootKey);
    // A window that launched straight into the Workspace has no shell to go back
    // to yet, so the first switch builds one and starts its session, exactly as
    // the launch path does. A restored one already has a live session; starting
    // another would strand the first.
    BOOL needsSession = terminal == nil;
    if (needsSession)
        terminal = CreateTerminalViewController();
    if (terminal == nil)
        return;
    [ISHDiagnosticsStore recordBreadcrumb:@"scene.mode.sessionShell"
                                  details:@{@"restored": @(!needsSession)}];
    ISHWindowTransitionToRoot(window, terminal);
    if (needsSession)
        [terminal startNewSession];
}

static NSUserActivity *SceneRequestedActivity(UISceneSession *session, UISceneConnectionOptions *connectionOptions) API_AVAILABLE(ios(13.0));
static NSUserActivity *SceneRequestedActivity(UISceneSession *session, UISceneConnectionOptions *connectionOptions) {
    NSUserActivity *activity = connectionOptions.userActivities.anyObject;
    if (activity != nil)
        return activity;
    return session.stateRestorationActivity;
}

static NSUserActivity *SceneEffectiveRequestedActivity(UISceneSession *session, UISceneConnectionOptions *connectionOptions) API_AVAILABLE(ios(13.0));
static NSUserActivity *SceneEffectiveRequestedActivity(UISceneSession *session, UISceneConnectionOptions *connectionOptions) {
    NSUserActivity *explicitActivity = connectionOptions.userActivities.anyObject;
    if (explicitActivity != nil)
        return explicitActivity;

    if (ISHShouldChooseFilesystemAtStartup())
        return nil;
    // The Wayland Display startup mode always boots into the standalone
    // Display -- ignore whatever scene state restoration would bring back.
    if (ISHShouldLaunchWaylandDisplayAtStartup())
        return nil;

    NSUserActivity *restorationActivity = session.stateRestorationActivity;
    NSString *restorationType = restorationActivity.activityType;
    if (restorationType.length == 0)
        return restorationActivity;

    BOOL prefersWorkspace = ISHShouldLaunchWorkspaceAtStartup();
    BOOL restoresWorkspace = [restorationType isEqualToString:ISHSceneActivityTypeWorkspace];
    if (prefersWorkspace != restoresWorkspace)
        return nil;
    return restorationActivity;
}

static void ISHScheduleDeferredFileProviderDomainSyncRelease(void) {
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t) (5 * NSEC_PER_SEC)),
                       dispatch_get_main_queue(), ^{
            [Roots.instance resumeDeferredFileProviderDomainSync];
        });
    });
}

static void EnsureSceneWindow(SceneDelegate *delegate, UIScene *scene) API_AVAILABLE(ios(13.0));
static void EnsureSceneWindow(SceneDelegate *delegate, UIScene *scene) {
    if (delegate.window == nil) {
        if (![scene isKindOfClass:UIWindowScene.class])
            return;
        delegate.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *) scene];
    }
}

// Drills into a navigation controller's top view controller and follows any
// chain of already-presented view controllers, landing on whatever's
// actually frontmost rather than a navigation shell or a screen something
// else is already presenting over.
static UIViewController *ISHTopmostViewController(UIViewController *root) {
    UIViewController *top = root;
    if ([top isKindOfClass:UINavigationController.class])
        top = ((UINavigationController *) top).topViewController ?: top;
    while (top.presentedViewController != nil && !top.presentedViewController.isBeingDismissed) {
        top = top.presentedViewController;
        if ([top isKindOfClass:UINavigationController.class])
            top = ((UINavigationController *) top).topViewController ?: top;
    }
    return top;
}

UIViewController *ISHActivePresentationViewController(void) {
    // Cheap common case: a terminal scene is frontmost.
    if (currentTerminalViewController != nil && currentTerminalViewController.viewIfLoaded.window != nil)
        return ISHTopmostViewController(currentTerminalViewController);

    // Otherwise scan every connected scene -- this is what makes Workspace
    // mode work (its root view controller is never a TerminalViewController,
    // so currentTerminalViewController is always nil there) and what makes a
    // request driven over ssh with no scene "active" work (prefer a
    // foreground-active scene, but fall back to any window scene at all so
    // the picker is queued and appears once the app is reopened).
    UIWindow *bestWindow = nil;
    for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
        if (![scene isKindOfClass:UIWindowScene.class])
            continue;
        UIWindowScene *windowScene = (UIWindowScene *) scene;
        UIWindow *window = nil;
        for (UIWindow *candidate in windowScene.windows) {
            if (candidate.isKeyWindow) {
                window = candidate;
                break;
            }
        }
        if (window == nil)
            window = windowScene.windows.firstObject;
        if (window == nil || window.rootViewController == nil)
            continue;
        if (windowScene.activationState == UISceneActivationStateForegroundActive)
            return ISHTopmostViewController(window.rootViewController);
        if (bestWindow == nil)
            bestWindow = window;
    }
    return bestWindow != nil ? ISHTopmostViewController(bestWindow.rootViewController) : nil;
}

static void ConfigureTerminalViewController(SceneDelegate *delegate, TerminalViewController *vc, UISceneSession *session, NSUserActivity *activity) API_AVAILABLE(ios(13.0));
static void ConfigureTerminalViewController(SceneDelegate *delegate, TerminalViewController *vc, UISceneSession *session, NSUserActivity *activity) {
    vc.sceneSession = session;
    NSString *terminalDisplayMode = activity.userInfo[ISHSceneTerminalDisplayModeUserInfoKey];
    if ([terminalDisplayMode isEqualToString:ISHSceneTerminalDisplayModeSessionShellValue]) {
        vc.freshSessionTerminalDisplayMode = ISHFreshSessionTerminalDisplayModeSessionShell;
    } else if ([terminalDisplayMode isEqualToString:ISHSceneTerminalDisplayModeSystemConsoleValue]) {
        vc.freshSessionTerminalDisplayMode = ISHFreshSessionTerminalDisplayModeSystemConsole;
    } else {
        vc.freshSessionTerminalDisplayMode = ISHFreshSessionTerminalDisplayModeAuto;
    }
    NSString *terminalUUID = activity.userInfo[ISHSceneTerminalUUIDUserInfoKey];
    if (terminalUUID.length == 0) {
        [ISHDiagnosticsStore recordLaunchStage:@"scene.terminal.startNewSession"
                                       details:@{@"session": session.persistentIdentifier ?: @""}];
        [vc startNewSession];
    } else {
        delegate.terminalUUID = terminalUUID;
        [ISHDiagnosticsStore recordLaunchStage:@"scene.terminal.reconnectSession"
                                       details:@{@"session": session.persistentIdentifier ?: @"",
                                                 @"terminalUUID": terminalUUID}];
        [vc reconnectSessionFromTerminalUUID:
         [[NSUUID alloc] initWithUUIDString:delegate.terminalUUID]];
    }
}

@implementation SceneDelegate

- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)connectionOptions {
    [ISHDiagnosticsStore recordLaunchStage:@"scene.willConnect"
                                   details:@{@"session": session.persistentIdentifier ?: @"",
                                             @"recovery": @([NSUserDefaults.standardUserDefaults boolForKey:@"recovery"])}];
    [ISHDiagnosticsStore recordBreadcrumb:@"scene.willConnect"
                                  details:@{@"session": session.persistentIdentifier ?: @"",
                                            @"recovery": @([NSUserDefaults.standardUserDefaults boolForKey:@"recovery"])}];
    EnsureSceneWindow(self, scene);
    NSUserActivity *requestedActivity = SceneEffectiveRequestedActivity(session, connectionOptions);

    if ([NSUserDefaults.standardUserDefaults boolForKey:kPreferenceOpenDiagnosticsOnLaunchKey]) {
        [ISHDiagnosticsStore recordLaunchStage:@"scene.rootController.diagnostics"
                                       details:@{@"session": session.persistentIdentifier ?: @""}];
        self.window.rootViewController = ISHCreateAboutNavigationController(NO, YES);
        [self.window makeKeyAndVisible];
        ISHScheduleLaunchJournalCompletion(@{@"rootController": @"diagnostics",
                                             @"session": session.persistentIdentifier ?: @""});
        return;
    }
    if ([NSUserDefaults.standardUserDefaults boolForKey:@"recovery"]) {
        [ISHDiagnosticsStore recordLaunchStage:@"scene.rootController.recovery"
                                       details:@{@"session": session.persistentIdentifier ?: @""}];
        self.window.rootViewController = ISHCreateAboutNavigationController(YES, NO);
        [self.window makeKeyAndVisible];
        ISHScheduleLaunchJournalCompletion(@{@"rootController": @"recovery",
                                             @"session": session.persistentIdentifier ?: @""});
        return;
    }

    if (Roots.instance.needsInitialRootSelection) {
        [NSNotificationCenter.defaultCenter removeObserver:self
                                                      name:RootsDidFinishInitialSelectionNotification
                                                    object:nil];
        [NSNotificationCenter.defaultCenter addObserver:self
                                               selector:@selector(rootsDidFinishInitialSelection:)
                                                   name:RootsDidFinishInitialSelectionNotification
                                                 object:nil];
        [ISHDiagnosticsStore recordLaunchStage:@"scene.rootController.initialRootSelection"
                                       details:@{@"session": session.persistentIdentifier ?: @""}];
        self.window.rootViewController = CreateRootSelectionViewController(NO, nil);
        [self.window makeKeyAndVisible];
        ISHScheduleLaunchJournalCompletion(@{@"rootController": @"initial-root-selection",
                                             @"session": session.persistentIdentifier ?: @""});
        return;
    }

    if (requestedActivity.activityType.length == 0 && ISHShouldChooseFilesystemAtStartup()) {
        __weak typeof(self) weakSelf = self;
        [ISHDiagnosticsStore recordLaunchStage:@"scene.rootController.chooseFilesystem"
                                       details:@{@"session": session.persistentIdentifier ?: @""}];
        self.window.rootViewController = CreateRootSelectionViewController(YES, ^(__unused NSString *rootName) {
            __strong typeof(weakSelf) strongSelf = weakSelf;
            UISceneSession *activeSession = strongSelf.window.windowScene.session ?: session;
            [strongSelf continueAfterInitialRootImportForSession:activeSession];
        });
        [self.window makeKeyAndVisible];
        ISHScheduleLaunchJournalCompletion(@{@"rootController": @"choose-filesystem",
                                             @"session": session.persistentIdentifier ?: @""});
        return;
    }

    NSString *activityType = requestedActivity.activityType;
    BOOL wantsWorkspace = [activityType isEqualToString:ISHSceneActivityTypeWorkspace];
    BOOL wantsTerminal = activityType.length == 0
        || [activityType isEqualToString:ISHSceneActivityTypeTerminal]
        || [activityType isEqualToString:ISHSceneActivityTypeLegacy];
    if (wantsWorkspace) {
        NSString *toolIdentifier = requestedActivity.userInfo[ISHSceneWorkspaceToolUserInfoKey];
        [ISHDiagnosticsStore recordLaunchStage:@"scene.rootController.workspace"
                                       details:@{@"session": session.persistentIdentifier ?: @"",
                                                 @"tool": toolIdentifier ?: @""}];
        self.window.rootViewController = ISHCreateWorkspaceNavigationControllerForTool(toolIdentifier);
        [self.window makeKeyAndVisible];
        ISHScheduleLaunchJournalCompletion(@{@"rootController": @"workspace",
                                             @"session": session.persistentIdentifier ?: @"",
                                             @"tool": toolIdentifier ?: @""});
        return;
    }
    if (activityType.length == 0 && ISHShouldLaunchWaylandDisplayAtStartup()) {
        [ISHDiagnosticsStore recordLaunchStage:@"scene.rootController.waylandDisplay"
                                       details:@{@"session": session.persistentIdentifier ?: @""}];
        self.window.rootViewController = CreateStandaloneDisplayViewController();
        [self.window makeKeyAndVisible];
        ISHScheduleLaunchJournalCompletion(@{@"rootController": @"wayland-display",
                                             @"session": session.persistentIdentifier ?: @""});
        return;
    }
    if (activityType.length == 0 && ISHShouldLaunchWorkspaceAtStartup()) {
        [ISHDiagnosticsStore recordLaunchStage:@"scene.rootController.workspace.default"
                                       details:@{@"session": session.persistentIdentifier ?: @""}];
        self.window.rootViewController = ISHCreateWorkspaceNavigationControllerForTool(nil);
        [self.window makeKeyAndVisible];
        ISHScheduleLaunchJournalCompletion(@{@"rootController": @"workspace",
                                             @"session": session.persistentIdentifier ?: @""});
        return;
    }
    if (!wantsTerminal) {
        [ISHDiagnosticsStore recordBreadcrumb:@"scene.unknownActivityType"
                                      details:@{@"activityType": activityType ?: @""}];
    }

    TerminalViewController *vc = [self.window.rootViewController isKindOfClass:TerminalViewController.class]
        ? (TerminalViewController *) self.window.rootViewController
        : CreateTerminalViewController();
    if (vc == nil)
        return;
    if (self.window.rootViewController != vc) {
        self.window.rootViewController = vc;
        [self.window makeKeyAndVisible];
    }
    [ISHDiagnosticsStore recordLaunchStage:@"scene.rootController.terminal"
                                   details:@{@"session": session.persistentIdentifier ?: @"",
                                             @"activityType": activityType ?: @""}];
    ConfigureTerminalViewController(self, vc, session, requestedActivity);
}

- (void)continueAfterInitialRootImportForSession:(UISceneSession *)session {
    if (Roots.instance.needsInitialRootSelection)
        return;
    if ([self.window.rootViewController isKindOfClass:TerminalViewController.class])
        return;

    self.waitingForInitialRootImport = NO;
    if (ISHShouldLaunchWaylandDisplayAtStartup()) {
        [ISHDiagnosticsStore recordLaunchStage:@"scene.rootController.waylandDisplay.afterInitialImport"
                                       details:@{@"session": session.persistentIdentifier ?: @""}];
        self.window.rootViewController = CreateStandaloneDisplayViewController();
        [self.window makeKeyAndVisible];
        ISHScheduleLaunchJournalCompletion(@{@"rootController": @"wayland-display",
                                             @"session": session.persistentIdentifier ?: @""});
        return;
    }
    if (ISHShouldLaunchWorkspaceAtStartup()) {
        [ISHDiagnosticsStore recordLaunchStage:@"scene.rootController.workspace.afterInitialImport"
                                       details:@{@"session": session.persistentIdentifier ?: @""}];
        self.window.rootViewController = ISHCreateWorkspaceNavigationControllerForTool(nil);
        [self.window makeKeyAndVisible];
        ISHScheduleLaunchJournalCompletion(@{@"rootController": @"workspace",
                                             @"session": session.persistentIdentifier ?: @""});
        return;
    }
    TerminalViewController *vc = CreateTerminalViewController();
    if (vc == nil)
        return;

    self.window.rootViewController = vc;
    [self.window makeKeyAndVisible];
    [ISHDiagnosticsStore recordLaunchStage:@"scene.rootController.terminal.afterInitialImport"
                                   details:@{@"session": session.persistentIdentifier ?: @""}];
    ConfigureTerminalViewController(self, vc, session, SceneEffectiveRequestedActivity(session, nil));
}

- (void)rootsDidFinishInitialSelection:(__unused NSNotification *)notification {
    UISceneSession *session = self.window.windowScene.session;
    if (session == nil)
        return;
    [self continueAfterInitialRootImportForSession:session];
}

// Routes an app.ish.scene.* destination activity onto an already-connected
// scene's window. The App Shortcuts intent asks for a destination with
// requestSceneSessionActivation(nil, userActivity:...), which only delivers
// the activity when it CREATES a scene (iPad); on a single-scene device it
// just foregrounds the existing scene and drops the activity -- verified by
// breadcrumb: -scene:continueUserActivity: is never called for it either.
static void ISHWindowApplyShortcutActivity(UIWindow *window, NSUserActivity *userActivity) {
    // The special launch states (diagnostics, recovery, first-run root
    // selection) own the window; leave them alone.
    if ([NSUserDefaults.standardUserDefaults boolForKey:kPreferenceOpenDiagnosticsOnLaunchKey] ||
        [NSUserDefaults.standardUserDefaults boolForKey:@"recovery"] ||
        Roots.instance.needsInitialRootSelection)
        return;

    NSString *activityType = userActivity.activityType;
    if ([activityType isEqualToString:ISHSceneActivityTypeWorkspace]) {
        ISHWindowShowWorkspace(window);
        NSString *toolIdentifier = userActivity.userInfo[ISHSceneWorkspaceToolUserInfoKey];
        if ([toolIdentifier isKindOfClass:NSString.class] && toolIdentifier.length > 0) {
            // Next runloop turn: ISHWindowShowWorkspace may have just installed
            // (or rebuilt) the workspace root, and the tool open needs it live.
            dispatch_async(dispatch_get_main_queue(), ^{
                UIViewController *root = window.rootViewController;
                UIViewController *hosted = [root isKindOfClass:UINavigationController.class]
                    ? ((UINavigationController *) root).viewControllers.firstObject
                    : root;
                if ([hosted isKindOfClass:WorkspaceViewController.class])
                    [(WorkspaceViewController *) hosted openWorkspaceToolWithIdentifier:toolIdentifier];
            });
        }
        return;
    }

    if ([activityType isEqualToString:ISHSceneActivityTypeTerminal]) {
        NSString *mode = userActivity.userInfo[ISHSceneTerminalDisplayModeUserInfoKey];
        BOOL wantsConsole = [mode isEqualToString:ISHSceneTerminalDisplayModeSystemConsoleValue];
        if (![window.rootViewController isKindOfClass:TerminalViewController.class])
            ISHWindowShowSessionShell(window);
        TerminalViewController *terminal = [window.rootViewController isKindOfClass:TerminalViewController.class]
            ? (TerminalViewController *) window.rootViewController
            : nil;
        if (wantsConsole)
            [terminal showSystemConsoleForCurrentSession];
        else
            [terminal showSessionShellForCurrentSession];
        return;
    }
}

BOOL ISHApplyShortcutActivityToConnectedScene(NSUserActivity *activity) {
    UIWindow *bestWindow = nil;
    for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
        if (![scene isKindOfClass:UIWindowScene.class])
            continue;
        SceneDelegate *delegate = (SceneDelegate *) scene.delegate;
        if (![delegate isKindOfClass:SceneDelegate.class] || delegate.window == nil)
            continue;
        if (scene.activationState == UISceneActivationStateForegroundActive) {
            bestWindow = delegate.window;
            break;
        }
        if (bestWindow == nil)
            bestWindow = delegate.window;
    }
    if (bestWindow == nil)
        return NO;
    [ISHDiagnosticsStore recordBreadcrumb:@"scene.applyShortcutActivity"
                                  details:@{@"type": activity.activityType ?: @""}];
    ISHWindowApplyShortcutActivity(bestWindow, activity);
    return YES;
}

// Kept for completeness: if UIKit ever does hand an activity to a connected
// scene (e.g. Handoff-style delivery), route it the same way.
- (void)scene:(UIScene *)scene continueUserActivity:(NSUserActivity *)userActivity {
    [ISHDiagnosticsStore recordBreadcrumb:@"scene.continueUserActivity"
                                  details:@{@"type": userActivity.activityType ?: @""}];
    ISHWindowApplyShortcutActivity(self.window, userActivity);
}

- (NSUserActivity *)stateRestorationActivityForScene:(UIScene *)scene {
    UIViewController *rootViewController = self.window.rootViewController;
    // A standalone Wayland Display root isn't a workspace scene: its launch is
    // driven purely by the startup-mode preference (and ignored the moment the
    // preference changes), so it contributes no restoration state -- without
    // this it would masquerade as a Workspace+Display scene via the
    // tool-identifier branch below.
    if ([rootViewController isKindOfClass:DisplayViewController.class] &&
        ((DisplayViewController *) rootViewController).standaloneMode)
        return nil;
    UIViewController *topViewController = [rootViewController isKindOfClass:UINavigationController.class]
        ? ((UINavigationController *) rootViewController).topViewController
        : rootViewController;
    UIViewController *presentedViewController = topViewController.presentedViewController;
    UIViewController *presentedTopViewController = [presentedViewController isKindOfClass:UINavigationController.class]
        ? ((UINavigationController *) presentedViewController).topViewController
        : presentedViewController;
    NSString *presentedWorkspaceToolIdentifier = ISHWorkspaceToolIdentifierForViewController(presentedTopViewController);
    if (presentedWorkspaceToolIdentifier.length > 0) {
        NSUserActivity *activity = [[NSUserActivity alloc] initWithActivityType:ISHSceneActivityTypeWorkspace];
        [activity addUserInfoEntriesFromDictionary:@{ISHSceneWorkspaceToolUserInfoKey: presentedWorkspaceToolIdentifier}];
        return activity;
    }
    if ([topViewController isKindOfClass:WorkspaceViewController.class]) {
        NSUserActivity *activity = [[NSUserActivity alloc] initWithActivityType:ISHSceneActivityTypeWorkspace];
        NSString *toolIdentifier = ISHWorkspaceToolIdentifierForViewController(topViewController);
        if (toolIdentifier.length > 0) {
            [activity addUserInfoEntriesFromDictionary:@{ISHSceneWorkspaceToolUserInfoKey: toolIdentifier}];
        }
        return activity;
    }
    NSString *workspaceToolIdentifier = ISHWorkspaceToolIdentifierForViewController(topViewController);
    if (workspaceToolIdentifier.length > 0) {
        NSUserActivity *activity = [[NSUserActivity alloc] initWithActivityType:ISHSceneActivityTypeWorkspace];
        [activity addUserInfoEntriesFromDictionary:@{ISHSceneWorkspaceToolUserInfoKey: workspaceToolIdentifier}];
        return activity;
    }

    NSUserActivity *activity = [[NSUserActivity alloc] initWithActivityType:ISHSceneActivityTypeTerminal];
    TerminalViewController *vc = (TerminalViewController *) rootViewController;
    if ([vc isKindOfClass:TerminalViewController.class]) {
        self.terminalUUID = vc.sessionTerminalUUID.UUIDString;
        if (self.terminalUUID != nil) {
            [activity addUserInfoEntriesFromDictionary:@{ISHSceneTerminalUUIDUserInfoKey: self.terminalUUID}];
        }
    }
    return activity;
}

- (void)sceneDidBecomeActive:(UIScene *)scene {
    [ISHDiagnosticsStore recordLaunchStage:@"scene.didBecomeActive"
                                   details:@{@"session": scene.session.persistentIdentifier ?: @""}];
    [ISHDiagnosticsStore recordBreadcrumb:@"scene.didBecomeActive"
                                  details:@{@"session": scene.session.persistentIdentifier ?: @""}];
    UIViewController *rootViewController = self.window.rootViewController;
    if ([rootViewController isKindOfClass:TerminalViewController.class]) {
        currentTerminalViewController = (TerminalViewController *) rootViewController;
    } else {
        currentTerminalViewController = NULL;
    }
    ISHScheduleDeferredFileProviderDomainSyncRelease();
    ISHScheduleLaunchJournalCompletion(@{@"rootController": NSStringFromClass(rootViewController.class) ?: @"unknown",
                                         @"session": scene.session.persistentIdentifier ?: @""});
}

- (void)sceneWillResignActive:(UIScene *)scene {
    [ISHDiagnosticsStore recordBreadcrumb:@"scene.willResignActive"
                                  details:@{@"session": scene.session.persistentIdentifier ?: @""}];
    UIViewController *rootViewController = self.window.rootViewController;
    TerminalViewController *terminalViewController = [rootViewController isKindOfClass:TerminalViewController.class]
        ? (TerminalViewController *) rootViewController
        : NULL;

    if (currentTerminalViewController == terminalViewController) {
        currentTerminalViewController = NULL;
    }
}

- (void)sceneDidEnterBackground:(UIScene *)scene {
    [ISHDiagnosticsStore recordBreadcrumb:@"scene.didEnterBackground"
                                  details:@{@"session": scene.session.persistentIdentifier ?: @""}];
    // The app supports multiple scenes, and the PROCESS is only heading for
    // suspension once every one of them is in the background. Arming the
    // suspend guard while another window is still frontmost would quiesce the
    // filesystem out from under a live session.
    for (UIScene *other in UIApplication.sharedApplication.connectedScenes) {
        if (other.activationState != UISceneActivationStateBackground)
            return;
    }
    ISHSuspendGuardEnterBackground();
}

- (void)sceneWillEnterForeground:(UIScene *)scene {
    [ISHDiagnosticsStore recordBreadcrumb:@"scene.willEnterForeground"
                                  details:@{@"session": scene.session.persistentIdentifier ?: @""}];
    ISHSuspendGuardEnterForeground();
}

@end
