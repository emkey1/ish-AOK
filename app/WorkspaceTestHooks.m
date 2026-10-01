//
//  WorkspaceTestHooks.m
//
//  A remote control for the Workspace, so that saving and restoring it can be
//  tested end to end: the Desktops arrangement across a relaunch, and the
//  layout a suspend files beside its image across a resume. Neither has a
//  guest-side view of terminal windows (/proc/ish/applets leaves them out on
//  purpose), and both are UI, so tests/workspace/restore_test.sh drives the
//  app from the Mac through this instead.
//
//  The channel is a directory and a Darwin notification. The harness writes
//  one command line to <app data>/tmp/workspace-test/cmd.txt, then posts
//  "app.ish.iSH-AOK.workspace-test" (simctl spawn ... notifyutil -p). The app
//  answers in result-<seq>.json beside it. A command line is
//  "<seq> <verb> [args...]"; windows are named by the "id" a dump gave them,
//  which lasts for this run only -- after a relaunch the harness finds a
//  window again by what its terminal shows.
//
//  Verbs:
//    dump                         every window, terminals' tabs and contents
//    workspace                    switch the scene to the Workspace
//    open-terminal                New Terminal, as the root menu does it
//    new-tab <id>                 New Tab in that Terminal window
//    type <id> <tab> <text>       send text to a tab (\n \r \t \\ decoded)
//    frame <id> <x> <y> <w> <h>   place a window, fractions of the desktop
//    desktops <n>                 have n Desktops
//    assign <id> <desktop>        move a window to a Desktop (from 0)
//    switch <desktop>             show a Desktop
//    save-arrangement             the Desktops applet's Save
//    restore-arrangement          the Desktops applet's Restore
//    save-session                 Save Session (the guest keeps running)
//    suspend-exit                 Suspend: save the session and exit
//    menu-save                    the root menu's Save Session, prompts and all
//    tap <title>                  press the button of the alert on screen whose
//                                 title starts with <title> (the resume picker)
//
//  Off unless ISH_WORKSPACE_TEST=1 is in the app's environment.
//

#import <UIKit/UIKit.h>
#import <notify.h>
#import <objc/runtime.h>
#import "WorkspaceTestHooks.h"
#import "WorkspaceViewController.h"
#import "TerminalViewController.h"
#import "Terminal.h"
#import "SceneDelegate.h"
#import "AppDelegate.h"
#include "kernel/checkpoint.h"

// The Workspace's own methods, implemented in WorkspaceViewController.m.
@interface WorkspaceViewController (WorkspaceTestHooks)
@property (nonatomic, strong) UIView *desktopSurfaceView;
@property (nonatomic) NSInteger activeDesktopIndex;
@property (nonatomic) NSInteger desktopCount;
- (NSDictionary<NSString *, NSNumber *> *)normalizedFrameDescriptorForFrame:(CGRect)frame;
- (void)applySavedFrameDescriptor:(NSDictionary<NSString *, id> *)descriptor
                         toWindow:(UIView *)windowView
                     fallbackSize:(CGSize)fallbackSize;
- (NSString *)persistentTerminalRoleForWindow:(UIView *)windowView;
- (nullable id)terminalTabsForWindow:(UIView *)windowView;
- (NSArray<TerminalViewController *> *)terminalViewControllersInWindow:(UIView *)windowView;
- (void)openNewTerminalTabInWindow:(UIView *)windowView;
- (nullable UIView *)openDesktopTerminalHerePreferringConsole:(BOOL)preferConsole
                                  reuseExisting:(BOOL)reuseExisting
                                trackPrimaryRole:(BOOL)trackPrimaryRole;
- (void)assignRestoredWindow:(UIView *)windowView toDesktopFromDescriptor:(NSDictionary<NSString *, id> *)descriptor;
- (void)switchToDesktopIndex:(NSInteger)index;
- (void)applyDesktopVisibility;
- (void)saveWorkspaceDesktops;
- (void)restoreWorkspaceDesktops;
- (void)saveSessionFromRootMenu;
@end

// ISHWorkspaceTerminalTabsViewController's, likewise private.
@interface NSObject (WorkspaceTestHooksTabs)
- (NSString *)roleForTab:(TerminalViewController *)tab;
@end

static NSString *const ISHWorkspaceTestNotification = @"app.ish.iSH-AOK.workspace-test";
static const void *ISHWorkspaceTestIdKey = &ISHWorkspaceTestIdKey;
static NSInteger ISHWorkspaceTestNextId = 1;

static NSString *ISHWorkspaceTestDirectory(void) {
    return [NSTemporaryDirectory() stringByAppendingPathComponent:@"workspace-test"];
}

static void ISHWorkspaceTestWriteResult(NSString *seq, NSDictionary *result) {
    NSData *json = [NSJSONSerialization dataWithJSONObject:result
                                                   options:NSJSONWritingPrettyPrinted | NSJSONWritingSortedKeys
                                                     error:NULL];
    NSString *path = [ISHWorkspaceTestDirectory()
        stringByAppendingPathComponent:[NSString stringWithFormat:@"result-%@.json", seq]];
    [json writeToFile:path atomically:YES];
}

// Every Workspace window, in stacking order (back to front), each given an id
// the first time it is seen.
static NSArray<UIView *> *ISHWorkspaceTestWindows(WorkspaceViewController *workspace) {
    Class windowClass = NSClassFromString(@"ISHWorkspaceContainedWindowView");
    NSMutableArray<UIView *> *windows = [NSMutableArray array];
    for (UIView *subview in workspace.desktopSurfaceView.subviews) {
        if (windowClass == nil || ![subview isKindOfClass:windowClass])
            continue;
        if (objc_getAssociatedObject(subview, ISHWorkspaceTestIdKey) == nil)
            objc_setAssociatedObject(subview, ISHWorkspaceTestIdKey, @(ISHWorkspaceTestNextId++),
                                     OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        [windows addObject:subview];
    }
    return windows;
}

static UIView *ISHWorkspaceTestWindowWithId(WorkspaceViewController *workspace, NSInteger windowId) {
    for (UIView *window in ISHWorkspaceTestWindows(workspace)) {
        if ([objc_getAssociatedObject(window, ISHWorkspaceTestIdKey) integerValue] == windowId)
            return window;
    }
    return nil;
}

static NSString *ISHWorkspaceTestDecode(NSString *text) {
    NSMutableString *out = [NSMutableString string];
    for (NSUInteger i = 0; i < text.length; i++) {
        unichar c = [text characterAtIndex:i];
        if (c == '\\' && i + 1 < text.length) {
            unichar next = [text characterAtIndex:++i];
            switch (next) {
                case 'n': [out appendString:@"\n"]; continue;
                case 'r': [out appendString:@"\r"]; continue;
                case 't': [out appendString:@"\t"]; continue;
                case '\\': [out appendString:@"\\"]; continue;
                default: [out appendFormat:@"\\%C", next]; continue;
            }
        }
        [out appendFormat:@"%C", c];
    }
    return out;
}

static void ISHWorkspaceTestDump(NSString *seq) {
    WorkspaceViewController *workspace = ISHWorkspaceCurrentController();
    struct checkpoint_status ck;
    checkpoint_get_status(&ck);
    NSMutableDictionary *result = [@{@"ok": @YES,
                                     @"workspace": @(workspace != nil),
                                     @"restored": @(ck.restored != 0),
                                     @"saves": @(ck.saves),
                                     @"lastError": @(ck.last_err),
                                     @"lastRefusal": @(ck.last_refusal),
                                     @"tasks": @(ck.tasks),
                                     @"leftOut": @(ck.left_out),
                                     @"leftOutNote": @(ck.left_out_note)} mutableCopy];
    if (workspace == nil) {
        ISHWorkspaceTestWriteResult(seq, result);
        return;
    }
    result[@"desktopCount"] = @(workspace.desktopCount);
    result[@"activeDesktop"] = @(workspace.activeDesktopIndex);
    NSMutableArray *windows = [NSMutableArray array];
    dispatch_group_t contents = dispatch_group_create();
    for (UIView *windowView in ISHWorkspaceTestWindows(workspace)) {
        NSMutableDictionary *window = [@{
            @"id": objc_getAssociatedObject(windowView, ISHWorkspaceTestIdKey),
            @"hidden": @(windowView.hidden),
            @"desktop": [windowView valueForKey:@"workspaceDesktopIndex"] ?: @0,
        } mutableCopy];
        NSDictionary *frame = [workspace normalizedFrameDescriptorForFrame:windowView.frame];
        if (frame != nil)
            window[@"frame"] = frame;
        NSString *tool = [windowView valueForKey:@"workspaceToolIdentifier"];
        if (tool.length > 0)
            window[@"tool"] = tool;
        if ([windowView valueForKey:@"hostedTerminalViewController"] != nil) {
            window[@"kind"] = @"terminal";
            id tabsController = [workspace terminalTabsForWindow:windowView];
            NSArray<TerminalViewController *> *tabControllers = [workspace terminalViewControllersInWindow:windowView];
            window[@"selectedTab"] = tabsController != nil ? [tabsController valueForKey:@"selectedIndex"] : @0;
            NSMutableArray *tabs = [NSMutableArray array];
            for (TerminalViewController *tab in tabControllers) {
                int sessionPid = tab.sessionTerminal.guestSessionId;
                if (sessionPid <= 0)
                    sessionPid = tab.sessionPid;
                NSMutableDictionary *tabInfo = [@{
                    @"role": (tabsController != nil ? [tabsController roleForTab:tab]
                                                    : [workspace persistentTerminalRoleForWindow:windowView]) ?: @"",
                    @"sessionPid": @(sessionPid),
                    @"hasTerminal": @(tab.terminal != nil),
                } mutableCopy];
                [tabs addObject:tabInfo];
                if (tab.terminal != nil) {
                    dispatch_group_enter(contents);
                    [tab.terminal fetchContentsWithCompletion:^(NSString *text) {
                        tabInfo[@"contents"] = text ?: @"";
                        dispatch_group_leave(contents);
                    }];
                }
            }
            window[@"tabs"] = tabs;
        } else {
            window[@"kind"] = tool.length > 0 ? @"tool" : @"other";
        }
        [windows addObject:window];
    }
    result[@"windows"] = windows;
    // fetchContents answers on the main queue; wait there, but bounded, so a
    // web view that never answers still gives a dump.
    __block BOOL written = NO;
    dispatch_group_notify(contents, dispatch_get_main_queue(), ^{
        if (written)
            return;
        written = YES;
        ISHWorkspaceTestWriteResult(seq, result);
    });
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t) (5 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        if (written)
            return;
        written = YES;
        result[@"contentsTimedOut"] = @YES;
        ISHWorkspaceTestWriteResult(seq, result);
    });
}

// The alert on top of any window, or nil.
static UIAlertController *ISHWorkspaceTestTopAlert(void) {
    for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
        if (![scene isKindOfClass:UIWindowScene.class])
            continue;
        for (UIWindow *window in ((UIWindowScene *) scene).windows) {
            UIViewController *top = window.rootViewController;
            while (top.presentedViewController != nil)
                top = top.presentedViewController;
            if ([top isKindOfClass:UIAlertController.class])
                return (UIAlertController *) top;
        }
    }
    return nil;
}

static void ISHWorkspaceTestRun(NSString *line) {
    NSArray<NSString *> *words = [line componentsSeparatedByString:@" "];
    if (words.count < 2)
        return;
    NSString *seq = words[0];
    NSString *verb = words[1];
    NSArray<NSString *> *args = [words subarrayWithRange:NSMakeRange(2, words.count - 2)];
    WorkspaceViewController *workspace = ISHWorkspaceCurrentController();
    void (^answer)(BOOL, NSString *) = ^(BOOL ok, NSString *error) {
        ISHWorkspaceTestWriteResult(seq, ok ? @{@"ok": @YES} : @{@"ok": @NO, @"error": error ?: @"failed"});
    };

    if ([verb isEqualToString:@"dump"]) {
        ISHWorkspaceTestDump(seq);
        return;
    }
    if ([verb isEqualToString:@"tap"] && args.count >= 1) {
        NSString *title = [args componentsJoinedByString:@" "];
        UIAlertController *alert = ISHWorkspaceTestTopAlert();
        for (UIAlertAction *action in alert.actions) {
            if (![action.title hasPrefix:title])
                continue;
            // What a tap does: the alert goes, then its handler runs. The
            // handler is not public API, which is acceptable in a test hook.
            void (^handler)(UIAlertAction *) = [action valueForKey:@"handler"];
            [alert.presentingViewController dismissViewControllerAnimated:NO completion:^{
                if (handler != nil)
                    handler(action);
                answer(YES, nil);
            }];
            return;
        }
        NSMutableArray<NSString *> *titles = [NSMutableArray array];
        for (UIAlertAction *action in alert.actions)
            [titles addObject:action.title ?: @""];
        answer(NO, alert == nil ? @"no alert on screen"
                                : [@"no such button; have: " stringByAppendingString:[titles componentsJoinedByString:@" | "]]);
        return;
    }
    if ([verb isEqualToString:@"workspace"]) {
        for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
            if (![scene isKindOfClass:UIWindowScene.class])
                continue;
            for (UIWindow *window in ((UIWindowScene *) scene).windows) {
                if (window.isKeyWindow || ((UIWindowScene *) scene).windows.count == 1) {
                    ISHWindowShowWorkspace(window);
                    answer(YES, nil);
                    return;
                }
            }
        }
        answer(NO, @"no window");
        return;
    }
    // From here on the Workspace has to be up.
    if (workspace == nil) {
        answer(NO, @"no workspace");
        return;
    }
    if ([verb isEqualToString:@"open-terminal"]) {
        [workspace openDesktopTerminalHerePreferringConsole:NO reuseExisting:NO trackPrimaryRole:NO];
        answer(YES, nil);
        return;
    }
    if ([verb isEqualToString:@"menu-save"]) {
        [workspace saveSessionFromRootMenu];
        answer(YES, nil);
        return;
    }
    if ([verb isEqualToString:@"save-arrangement"]) {
        [workspace saveWorkspaceDesktops];
        answer(YES, nil);
        return;
    }
    if ([verb isEqualToString:@"restore-arrangement"]) {
        [workspace restoreWorkspaceDesktops];
        answer(YES, nil);
        return;
    }
    if ([verb isEqualToString:@"desktops"] && args.count == 1) {
        workspace.desktopCount = MAX(1, args[0].integerValue);
        [workspace applyDesktopVisibility];
        answer(YES, nil);
        return;
    }
    if ([verb isEqualToString:@"switch"] && args.count == 1) {
        [workspace switchToDesktopIndex:args[0].integerValue];
        answer(YES, nil);
        return;
    }
    if ([verb isEqualToString:@"save-session"] || [verb isEqualToString:@"suspend-exit"]) {
        BOOL exits = [verb isEqualToString:@"suspend-exit"];
        // Off the main thread, as the app's own callers do: the layout capture
        // waits on the main queue for the terminals' contents.
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            int err = exits ? ISHSuspendSessionSuspendAndExit(NO) : ISHSuspendSessionSaveNow(NO);
            // Reached only when the save failed, or for save-session.
            ISHWorkspaceTestWriteResult(seq, @{@"ok": @(err == 0), @"rc": @(err)});
        });
        return;
    }

    // The rest name a window.
    if (args.count < 1) {
        answer(NO, @"missing window id");
        return;
    }
    UIView *windowView = ISHWorkspaceTestWindowWithId(workspace, args[0].integerValue);
    if (windowView == nil) {
        answer(NO, @"no such window");
        return;
    }
    if ([verb isEqualToString:@"new-tab"]) {
        [workspace openNewTerminalTabInWindow:windowView];
        answer(YES, nil);
        return;
    }
    if ([verb isEqualToString:@"type"] && args.count >= 3) {
        NSArray<TerminalViewController *> *tabs = [workspace terminalViewControllersInWindow:windowView];
        NSInteger tabIndex = args[1].integerValue;
        if (tabIndex < 0 || tabIndex >= (NSInteger) tabs.count || tabs[tabIndex].terminal == nil) {
            answer(NO, @"no such tab");
            return;
        }
        NSString *text = [[args subarrayWithRange:NSMakeRange(2, args.count - 2)] componentsJoinedByString:@" "];
        [tabs[tabIndex].terminal sendInput:[ISHWorkspaceTestDecode(text) dataUsingEncoding:NSUTF8StringEncoding]];
        answer(YES, nil);
        return;
    }
    if ([verb isEqualToString:@"frame"] && args.count == 5) {
        [workspace applySavedFrameDescriptor:@{@"x": @(args[1].doubleValue), @"y": @(args[2].doubleValue),
                                               @"width": @(args[3].doubleValue), @"height": @(args[4].doubleValue)}
                                    toWindow:windowView
                                fallbackSize:CGSizeZero];
        answer(YES, nil);
        return;
    }
    if ([verb isEqualToString:@"assign"] && args.count == 2) {
        [workspace assignRestoredWindow:windowView toDesktopFromDescriptor:@{@"desktopIndex": @(args[1].integerValue)}];
        [workspace applyDesktopVisibility];
        answer(YES, nil);
        return;
    }
    answer(NO, [@"unknown command: " stringByAppendingString:line]);
}

static void ISHWorkspaceTestHandleCommand(void) {
    NSString *path = [ISHWorkspaceTestDirectory() stringByAppendingPathComponent:@"cmd.txt"];
    NSString *line = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:NULL];
    if (line == nil)
        return;
    [NSFileManager.defaultManager removeItemAtPath:path error:NULL];
    line = [line stringByTrimmingCharactersInSet:NSCharacterSet.newlineCharacterSet];
    if (line.length > 0)
        ISHWorkspaceTestRun(line);
}

void ISHWorkspaceTestHooksStart(void) {
    const char *enabled = getenv("ISH_WORKSPACE_TEST");
    if (enabled == NULL || strcmp(enabled, "1") != 0)
        return;
    static int token;
    NSString *directory = ISHWorkspaceTestDirectory();
    [NSFileManager.defaultManager createDirectoryAtPath:directory withIntermediateDirectories:YES attributes:nil error:NULL];
    notify_register_dispatch(ISHWorkspaceTestNotification.UTF8String, &token, dispatch_get_main_queue(), ^(__unused int t) {
        ISHWorkspaceTestHandleCommand();
    });
    // So the harness knows this launch is listening.
    [[NSString stringWithFormat:@"%d\n", getpid()] writeToFile:[directory stringByAppendingPathComponent:@"ready"]
                                                    atomically:YES
                                                      encoding:NSUTF8StringEncoding
                                                         error:NULL];
}
