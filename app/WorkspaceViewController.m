#import "WorkspaceViewController.h"

#import "AboutViewController.h"
#import "Diagnostics.h"
#import "Roots.h"
#import "SceneDelegate.h"
#include "fs/devices.h"
#include "kernel/init.h"
#import "TerminalViewController.h"
#import "DisplayViewController.h"
#import "UserPreferences.h"
#import "NSObject+SaneKVO.h"
#import "AudioPlayerEngine.h"
#import "AudioLibrary.h"
#import "MotePadDocumentStore.h"
#import "WorkspaceFileManager.h"
#import "WorkspaceMarkdownViewer.h"
#import "WorkspaceImageViewer.h"
#import "WorkspaceVideoPlayer.h"
#import "UIApplication+OpenURL.h"
#import <WebKit/WebKit.h>
#include "kernel/task.h"
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <mach/mach.h>
#include <mach/task_info.h>
#include <net/if.h>
#include <sys/sysctl.h>
#include <string.h>

@class ISHWorkspaceContainedWindowView;

@interface WorkspaceViewController () <UIGestureRecognizerDelegate>

@property (nonatomic, copy) NSString *initialToolIdentifier;
@property (nonatomic) BOOL didOpenInitialTool;
@property (nonatomic, strong) UIButton *modernMenuPip;
@property (nonatomic) BOOL didEnsureDefaultWorkspaceUtilities;
@property (nonatomic, strong) UIView *desktopSurfaceView;
@property (nonatomic, strong) UIImageView *desktopWallpaperView;
@property (nonatomic, copy) NSString *appliedWallpaperThemeIdentifier;
@property (nonatomic) CGSize appliedWallpaperImageSize;
@property (nonatomic, strong) NSMutableArray<UIView *> *desktopWindows;
@property (nonatomic) NSInteger activeDesktopIndex;
@property (nonatomic) NSInteger desktopCount;
@property (nonatomic, strong) NSMutableIndexSet *lockedDesktopIndices;
@property (nonatomic, weak) UILabel *desktopIndicatorLabel;
@property (nonatomic) NSInteger desktopWindowCascadeIndex;
@property (nonatomic, weak) ISHWorkspaceContainedWindowView *dashboardWindow;
@property (nonatomic, weak) ISHWorkspaceContainedWindowView *dockWindow;
@property (nonatomic) BOOL didEvaluateStartupMemoryWarning;
@property (nonatomic, strong) UIView *startupMemoryWarningOverlayView;
@property (nonatomic, strong) UISwitch *startupMemoryWarningDisableSwitch;
@property (nonatomic, strong) UIButton *dockUtilsButton;
@property (nonatomic, strong) UIButton *dockTerminalButton;
@property (nonatomic, strong) UIStackView *bodyStack;
@property (nonatomic, strong) UIView *windowCard;
@property (nonatomic, strong) UILabel *layoutManagerWorkspaceLabel;

- (void)openWorkspaceToolWithIdentifier:(NSString *)toolIdentifier;
- (void)openOrFocusWorkspaceToolIdentifier:(NSString *)toolIdentifier;
- (void)switchToDesktopIndex:(NSInteger)index;
- (void)createNewDesktop;
- (void)removeDesktopAtIndex:(NSInteger)index;
- (BOOL)isDesktopLockedAtIndex:(NSInteger)index;
- (void)toggleDesktopLockAtIndex:(NSInteger)index;
- (void)ensureDefaultWorkspaceUtilitiesOpen;
- (void)ensureDefaultLLMChatWindowOpenIfNeeded;
- (void)persistDefaultWorkspaceUtilityFrames;
- (NSString *)persistentWorkspacesWindowFrameDefaultsKey;
- (void)applyInitialPlacementToWorkspacesWindow:(ISHWorkspaceContainedWindowView *)windowView;
- (void)applyInitialPlacementToLauncherWindow:(ISHWorkspaceContainedWindowView *)windowView;
- (void)persistLauncherWindowFrame;
- (void)restoreLauncherWindowPlacement;
- (void)persistDockWindowFrame;
- (NSString *)persistentDockWindowDescriptorDefaultsKey;
- (void)applyInitialPlacementToDockWindow:(ISHWorkspaceContainedWindowView *)windowView;
- (void)workspaceActivationDidChange:(NSNotification *)notification;
- (void)workspaceDockFrameDidChange:(NSNotification *)notification;
- (void)workspaceWorkspacesFrameDidChange:(NSNotification *)notification;
- (NSDictionary<NSString *, NSNumber *> *)absoluteFrameDescriptorForFrame:(CGRect)frame;
- (void)applyAbsoluteFrameDescriptor:(NSDictionary<NSString *, id> *)descriptor toWindow:(ISHWorkspaceContainedWindowView *)windowView;
- (NSString *)currentWorkspaceLayoutStorageIdentifier;
- (NSArray<NSDictionary<NSString *, id> *> *)savedWorkspaceLayoutForCurrentScene;
- (void)openDashboardWindow:(id)sender;
- (void)openNewWorkspaceWindow:(id)sender;
- (void)closeHiddenWorkspaceWindows:(id)sender;
- (NSArray<UISceneSession *> *)hiddenWorkspaceSceneSessions API_AVAILABLE(ios(13.0));
- (void)openTerminalHerePreferringConsole:(BOOL)preferConsole;
- (void)openExistingTerminalHereWithUUID:(NSUUID *)terminalUUID;
- (void)focusSceneWithPersistentIdentifier:(NSString *)identifier;
- (void)closeSceneWithPersistentIdentifier:(NSString *)identifier title:(NSString *)title;
- (ISHWorkspaceContainedWindowView *)desktopWindowForToolIdentifier:(NSString *)toolIdentifier;
- (ISHWorkspaceContainedWindowView *)desktopWindowHostingTerminalUUID:(NSUUID *)terminalUUID;
- (void)focusDesktopWindow:(ISHWorkspaceContainedWindowView *)windowView;

- (UISceneSession *)sceneSessionHostingTerminalUUID:(NSUUID *)terminalUUID API_AVAILABLE(ios(13.0));
- (BOOL)focusSceneSession:(UISceneSession *)sceneSession title:(NSString *)title API_AVAILABLE(ios(13.0));

@end

NSString *const ISHInitialWindowWorkspaceValue = @"workspace";
NSString *const ISHInitialWindowChooseFilesystemValue = @"choose-filesystem";
NSString *const ISHInitialWindowWaylandValue = @"wayland";
static NSString *const ISHWorkspaceToolClockIdentifier = @"clock";
static NSString *const ISHWorkspaceToolInfoIdentifier = @"info";
static NSString *const ISHWorkspaceToolMonitorIdentifier = @"monitor";
static NSString *const ISHWorkspaceToolNetworksIdentifier = @"networks";
static NSString *const ISHWorkspaceToolStatusIdentifier = @"status";
static NSString *const ISHWorkspaceToolWorkspacesIdentifier = @"workspaces";
static NSString *const ISHWorkspaceToolSessionsIdentifier = @"sessions";
static NSString *const ISHWorkspaceToolStorageIdentifier = @"storage";
static NSString *const ISHWorkspaceToolShortcutsIdentifier = @"shortcuts";
static NSString *const ISHWorkspaceToolBrowserIdentifier = @"browser";
static NSString *const ISHWorkspaceToolThemesIdentifier = @"themes";
static NSString *const ISHWorkspaceToolFilesystemsIdentifier = @"filesystems";
static NSString *const ISHWorkspaceToolSettingsIdentifier = @"settings";
static NSString *const ISHWorkspaceToolDiagnosticsIdentifier = @"diagnostics";
static NSString *const ISHWorkspaceToolLLMIdentifier = @"llm";
static NSString *const ISHWorkspaceToolLauncherIdentifier = @"launcher";
static NSString *const ISHWorkspaceToolAudioIdentifier = @"audio";
static NSString *const ISHWorkspaceToolMotePadIdentifier = @"motepad";
static NSString *const ISHWorkspaceToolFileManagerIdentifier = @"filemanager";
static NSString *const ISHWorkspaceToolMarkdownViewerIdentifier = @"markdown";
static NSString *const ISHWorkspaceToolImageViewerIdentifier = @"imageviewer";
static NSString *const ISHWorkspaceToolVideoPlayerIdentifier = @"videoplayer";
static NSString *const ISHWorkspaceToolDisplayIdentifier = @"display";
static NSString *const ISHWorkspaceSavedLayoutDefaultsKey = @"ISHWorkspaceSavedLayout";
static NSString *const ISHWorkspacePersistentWorkspacesWindowFrameDefaultsKey = @"ISHWorkspacePersistentWorkspacesWindowFrame";
static NSString *const ISHWorkspaceLegacyPersistentWorkspacesWindowFrameDefaultsKeyPrefix = @"ISHWorkspacePersistentWorkspacesWindowFrame";
static NSString *const ISHWorkspacePersistentDockWindowDescriptorDefaultsKey = @"ISHWorkspacePersistentDockWindowDescriptor";
static NSString *const ISHWorkspacePersistentLauncherWindowFrameDefaultsKey = @"ISHWorkspacePersistentLauncherWindowFrame";
static NSString *const ISHWorkspaceForgottenHiddenSessionsDefaultsKey = @"ISHWorkspaceForgottenHiddenSessions";
static NSString *const ISHWorkspaceDockFrameDidChangeNotification = @"ISHWorkspaceDockFrameDidChange";
static NSString *const ISHWorkspaceWorkspacesFrameDidChangeNotification = @"ISHWorkspaceWorkspacesFrameDidChange";
static NSString *const ISHWorkspaceDesktopsDidChangeNotification = @"ISHWorkspaceDesktopsDidChange";
static NSString *const ISHWorkspaceSavedLayoutKindDashboard = @"dashboard";
static NSString *const ISHWorkspaceSavedLayoutKindDock = @"dock";
static NSString *const ISHWorkspaceSavedLayoutKindTool = @"tool";
static NSString *const ISHWorkspaceSavedLayoutKindTerminal = @"terminal";
static NSString *const ISHWorkspaceTerminalRoleSessionShell = @"session-shell";
static NSString *const ISHWorkspaceTerminalRoleSystemConsole = @"system-console";
static NSString *const ISHWorkspaceTerminalRoleGeneric = @"terminal";
static const CGFloat ISHWorkspaceWindowCornerRadius = 22.0;
static const CGFloat ISHWorkspaceWindowTitleBarHeight = 24.0;
static const CGFloat ISHWorkspaceWindowButtonSize = 18.0;
static const CGFloat ISHWorkspaceWindowButtonInset = 8.0;
static const CGFloat ISHWorkspaceWindowTitleSideInset = 34.0;

static BOOL ISHWorkspaceUsesPhoneLayout(void) {
    return UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPhone;
}

// The Workspace ships two coexisting experiences, selected by the "Workspace Style"
// preference: Classic (the long-standing look) and Modern (the flat, ctwm-inspired
// reskin). Modern currently mirrors Classic; each rung fills in its divergences behind
// this gate so the Classic experience is never disturbed.
static BOOL ISHWorkspaceUsesModernStyle(void) {
    return UserPreferences.shared.workspaceStyle == WorkspaceStyleModern;
}

// User-defined launcher shortcuts: each is @{@"name", @"command"} (a leaf that runs a command
// in a fresh terminal) or @{@"name", @"children"} (a group whose "children" is itself an array
// of shortcuts, recursively — lets a shortcut like "Remote Login" hold multiple hosts). Stored
// as a single tree in NSUserDefaults.
static NSString *const ISHWorkspaceLauncherShortcutsKey = @"ISHWorkspaceLauncherShortcuts";
static NSString *const ISHWorkspaceLauncherShortcutsDidChangeNotification = @"ISHWorkspaceLauncherShortcutsDidChangeNotification";

static NSArray<NSDictionary<NSString *, id> *> *ISHWorkspaceLauncherShortcuts(void) {
    NSArray *stored = [NSUserDefaults.standardUserDefaults arrayForKey:ISHWorkspaceLauncherShortcutsKey];
    return [stored isKindOfClass:NSArray.class] ? stored : @[];
}

static void ISHWorkspaceSetLauncherShortcuts(NSArray<NSDictionary<NSString *, id> *> *shortcuts) {
    [NSUserDefaults.standardUserDefaults setObject:shortcuts forKey:ISHWorkspaceLauncherShortcutsKey];
    // Live-update any open Launcher applet (and the menu next time it's built).
    [NSNotificationCenter.defaultCenter postNotificationName:ISHWorkspaceLauncherShortcutsDidChangeNotification object:nil];
}

static BOOL ISHWorkspaceShortcutIsGroup(NSDictionary<NSString *, id> *shortcut) {
    return [shortcut[@"children"] isKindOfClass:NSArray.class];
}

static NSArray<NSDictionary<NSString *, id> *> *ISHWorkspaceShortcutChildren(NSDictionary<NSString *, id> *shortcut) {
    NSArray *children = shortcut[@"children"];
    return [children isKindOfClass:NSArray.class] ? children : @[];
}

// Resolves the shortcut array living at `path` (a chain of child indices from the root),
// e.g. an empty path is the root list; @[@1] is the children of the group at root index 1.
// Returns an empty array for a path that no longer resolves (deleted out from under a stale view).
static NSArray<NSDictionary<NSString *, id> *> *ISHWorkspaceLauncherArrayAtPath(NSArray<NSNumber *> *path) {
    NSArray<NSDictionary<NSString *, id> *> *current = ISHWorkspaceLauncherShortcuts();
    for (NSNumber *indexNumber in path) {
        NSUInteger index = indexNumber.unsignedIntegerValue;
        if (index >= current.count)
            return @[];
        current = ISHWorkspaceShortcutChildren(current[index]);
    }
    return current;
}

// Deep-copies a shortcut tree into nested NSMutableArrays (dictionaries stay immutable leaves;
// only the arrays need to be mutable since edits replace/insert/remove/move whole dictionaries).
static NSMutableArray<NSDictionary<NSString *, id> *> *ISHWorkspaceLauncherDeepMutableCopy(NSArray<NSDictionary<NSString *, id> *> *array) {
    NSMutableArray<NSDictionary<NSString *, id> *> *copy = [NSMutableArray arrayWithCapacity:array.count];
    for (NSDictionary<NSString *, id> *item in array) {
        if (ISHWorkspaceShortcutIsGroup(item)) {
            NSMutableDictionary<NSString *, id> *mutableItem = [item mutableCopy];
            mutableItem[@"children"] = ISHWorkspaceLauncherDeepMutableCopy(ISHWorkspaceShortcutChildren(item));
            [copy addObject:mutableItem];
        } else {
            [copy addObject:item];
        }
    }
    return copy;
}

typedef void (^ISHWorkspaceLauncherMutationBlock)(NSMutableArray<NSDictionary<NSString *, id> *> *level);

// Walks a deep-mutable-copied tree (as produced by ISHWorkspaceLauncherDeepMutableCopy) down to
// `path`, materializing an empty mutable "children" array for any group along the way that
// doesn't have one yet, and returns the mutable array living at that path. Returns nil on a
// stale/invalid path (an index that no longer exists). Shared by both -MutateAtPath: (a single
// level) and -MoveShortcut:::: (moving an item between two levels of the same tree).
static NSMutableArray<NSDictionary<NSString *, id> *> *ISHWorkspaceLauncherWalkToMutableLevel(
    NSMutableArray<NSDictionary<NSString *, id> *> *root, NSArray<NSNumber *> *path) {
    NSMutableArray<NSDictionary<NSString *, id> *> *level = root;
    for (NSNumber *indexNumber in path) {
        NSUInteger index = indexNumber.unsignedIntegerValue;
        if (index >= level.count)
            return nil;
        NSMutableDictionary<NSString *, id> *item = [level[index] mutableCopy];
        NSMutableArray<NSDictionary<NSString *, id> *> *children = ISHWorkspaceShortcutIsGroup(item)
            ? item[@"children"] : [NSMutableArray new];
        item[@"children"] = children;
        level[index] = item;
        level = children;
    }
    return level;
}

// Mutates the shortcut array living at `path` and persists the whole tree back. Hands the
// mutable array at that level to `block` (add/remove/replace/move as needed), then saves.
// Silently no-ops on a stale/invalid path.
static void ISHWorkspaceLauncherMutateAtPath(NSArray<NSNumber *> *path, ISHWorkspaceLauncherMutationBlock block) {
    NSMutableArray<NSDictionary<NSString *, id> *> *root = ISHWorkspaceLauncherDeepMutableCopy(ISHWorkspaceLauncherShortcuts());
    NSMutableArray<NSDictionary<NSString *, id> *> *level = ISHWorkspaceLauncherWalkToMutableLevel(root, path);
    if (level == nil)
        return;
    block(level);
    ISHWorkspaceSetLauncherShortcuts(root);
}

// Relocates a single shortcut from one level of the tree to another (e.g. dragging it into a
// group, or out to that group's parent via the "‹ Back" row) — both within the same persisted
// write, so the tree never transiently loses the item. Silently no-ops on a stale path/index.
static void ISHWorkspaceLauncherMoveShortcut(NSArray<NSNumber *> *sourcePath, NSUInteger sourceIndex,
                                              NSArray<NSNumber *> *destinationPath, NSUInteger destinationIndex) {
    NSMutableArray<NSDictionary<NSString *, id> *> *root = ISHWorkspaceLauncherDeepMutableCopy(ISHWorkspaceLauncherShortcuts());
    NSMutableArray<NSDictionary<NSString *, id> *> *sourceLevel = ISHWorkspaceLauncherWalkToMutableLevel(root, sourcePath);
    if (sourceLevel == nil || sourceIndex >= sourceLevel.count)
        return;
    // Resolve the destination level BEFORE removing the source item. destinationPath's indices
    // describe the tree's state right now — e.g. "the group currently at index 2" — which can be
    // the very same array sourceIndex is being removed from (dropping onto a sibling group).
    // Removing first would shift every index after sourceIndex down by one, so looking up the
    // destination afterward could land on the wrong item (or past the end) whenever the
    // destination sits later in that shared array — which read as the shortcut just vanishing.
    NSMutableArray<NSDictionary<NSString *, id> *> *destinationLevel = ISHWorkspaceLauncherWalkToMutableLevel(root, destinationPath);
    if (destinationLevel == nil)
        return;

    NSDictionary<NSString *, id> *moved = sourceLevel[sourceIndex];
    [sourceLevel removeObjectAtIndex:sourceIndex];
    [destinationLevel insertObject:moved atIndex:MIN(destinationIndex, destinationLevel.count)];
    ISHWorkspaceSetLauncherShortcuts(root);
}

// A launcher shortcut whose command is just a {token} opens a built-in tool/applet instead of
// running a shell command. Returns the tool identifier for a recognized token, or nil.
static NSString *ISHWorkspaceLauncherToolIdentifierForCommand(NSString *command) {
    NSString *trimmed = [command stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if (trimmed.length < 3 || ![trimmed hasPrefix:@"{"] || ![trimmed hasSuffix:@"}"])
        return nil;
    NSString *token = [[trimmed substringWithRange:NSMakeRange(1, trimmed.length - 2)]
                       stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet].lowercaseString;
    NSDictionary<NSString *, NSString *> *map = @{
        @"browser": ISHWorkspaceToolBrowserIdentifier,
        @"web": ISHWorkspaceToolBrowserIdentifier,
        @"web_browser": ISHWorkspaceToolBrowserIdentifier,
        @"webbrowser": ISHWorkspaceToolBrowserIdentifier,
        @"clock": ISHWorkspaceToolClockIdentifier,
        @"settings": ISHWorkspaceToolSettingsIdentifier,
        @"themes": ISHWorkspaceToolThemesIdentifier,
        @"sessions": ISHWorkspaceToolSessionsIdentifier,
        @"storage": ISHWorkspaceToolStorageIdentifier,
        @"monitor": ISHWorkspaceToolMonitorIdentifier,
        @"networks": ISHWorkspaceToolNetworksIdentifier,
        @"logs": ISHWorkspaceToolStatusIdentifier,
        @"status": ISHWorkspaceToolStatusIdentifier,
        @"diagnostics": ISHWorkspaceToolDiagnosticsIdentifier,
        @"filesystems": ISHWorkspaceToolFilesystemsIdentifier,
        @"images": ISHWorkspaceToolFilesystemsIdentifier,
        @"boot_images": ISHWorkspaceToolFilesystemsIdentifier,
        @"info": ISHWorkspaceToolInfoIdentifier,
        @"workspaces": ISHWorkspaceToolWorkspacesIdentifier,
        @"shortcuts": ISHWorkspaceToolShortcutsIdentifier,
        @"quick_actions": ISHWorkspaceToolShortcutsIdentifier,
        @"llm": ISHWorkspaceToolLLMIdentifier,
        @"chat": ISHWorkspaceToolLLMIdentifier,
        @"music": ISHWorkspaceToolAudioIdentifier,
        @"audio": ISHWorkspaceToolAudioIdentifier,
        @"motepad": ISHWorkspaceToolMotePadIdentifier,
        @"editor": ISHWorkspaceToolMotePadIdentifier,
        @"notepad": ISHWorkspaceToolMotePadIdentifier,
        @"textedit": ISHWorkspaceToolMotePadIdentifier,
        @"text": ISHWorkspaceToolMotePadIdentifier,
        @"files": ISHWorkspaceToolFileManagerIdentifier,
        @"filemanager": ISHWorkspaceToolFileManagerIdentifier,
        @"finder": ISHWorkspaceToolFileManagerIdentifier,
        @"markdown": ISHWorkspaceToolMarkdownViewerIdentifier,
        @"md": ISHWorkspaceToolMarkdownViewerIdentifier,
        @"imageviewer": ISHWorkspaceToolImageViewerIdentifier,
        @"image": ISHWorkspaceToolImageViewerIdentifier,
        @"img": ISHWorkspaceToolImageViewerIdentifier,
        @"videoplayer": ISHWorkspaceToolVideoPlayerIdentifier,
        @"video": ISHWorkspaceToolVideoPlayerIdentifier,
        @"display": ISHWorkspaceToolDisplayIdentifier,
        @"vnc": ISHWorkspaceToolDisplayIdentifier,
        @"wayland": ISHWorkspaceToolDisplayIdentifier,
    };
    return map[token];
}

// A launcher shortcut whose command is {shell} or {console} opens/focuses the primary Session
// Shell or System Console — the same singleton terminals as the dock's Terminal menu — rather
// than a generic tool applet (they're not tool identifiers) or an ad-hoc fresh terminal (a plain
// blank-command shortcut already covers that case). Returns the matching terminal role, or nil.
static NSString *ISHWorkspaceLauncherTerminalRoleForCommand(NSString *command) {
    NSString *trimmed = [command stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if (trimmed.length < 3 || ![trimmed hasPrefix:@"{"] || ![trimmed hasSuffix:@"}"])
        return nil;
    NSString *token = [[trimmed substringWithRange:NSMakeRange(1, trimmed.length - 2)]
                       stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet].lowercaseString;
    NSDictionary<NSString *, NSString *> *map = @{
        @"shell": ISHWorkspaceTerminalRoleSessionShell,
        @"session_shell": ISHWorkspaceTerminalRoleSessionShell,
        @"sessionshell": ISHWorkspaceTerminalRoleSessionShell,
        @"console": ISHWorkspaceTerminalRoleSystemConsole,
        @"system_console": ISHWorkspaceTerminalRoleSystemConsole,
        @"systemconsole": ISHWorkspaceTerminalRoleSystemConsole,
    };
    return map[token];
}

static BOOL ISHWorkspaceSupportsSceneWindows(void) {
    if (ISHWorkspaceUsesPhoneLayout())
        return NO;
    if (@available(iOS 13.0, *))
        return UIApplication.sharedApplication.supportsMultipleScenes;
    return NO;
}

static CGRect ISHWorkspaceRectWithRoundedOriginPreservingSize(CGRect frame) {
    frame.origin.x = round(frame.origin.x);
    frame.origin.y = round(frame.origin.y);
    return frame;
}

@interface ISHWorkspaceContainedWindowView : UIView <UIGestureRecognizerDelegate>

@property (nonatomic) CGSize preferredSize;
// The frame this window had before a layout clamp shrank/moved it to fit a smaller
// desktop surface (rotation to portrait, or the portrait-sized snapshot layout pass iOS
// runs when the app is backgrounded). CGRectNull when no restore is pending. Cleared by
// any deliberate frame change (drag, resize, programmatic placement via preferredSize).
@property (nonatomic) CGRect frameBeforeBoundsClamp;
@property (nonatomic) BOOL didApplyInitialFrame;
@property (nonatomic) BOOL draggable;
@property (nonatomic) BOOL resizable;
@property (nonatomic, strong) UIView *titleBarView;
@property (nonatomic, strong) UILabel *titleLabel;
@property (nonatomic, strong) UIButton *closeButton;
@property (nonatomic, strong) UIButton *utilityButton;
@property (nonatomic, strong) UIView *contentContainerView;
@property (nonatomic, strong) UIView *panelView;
@property (nonatomic, strong) UIView *resizeHandleView;
@property (nonatomic, strong) NSLayoutConstraint *resizeHandleTopConstraint;
@property (nonatomic, strong) NSLayoutConstraint *resizeHandleBottomConstraint;
@property (nonatomic, strong) NSLayoutConstraint *resizeHandleTrailingConstraint;
@property (nonatomic, strong) NSLayoutConstraint *resizeHandleLeadingConstraint;
@property (nonatomic, copy, nullable) dispatch_block_t closeHandler;
@property (nonatomic, copy, nullable) dispatch_block_t utilityHandler;
@property (nonatomic, copy, nullable) dispatch_block_t didBecomeFrontmostHandler;
@property (nonatomic, copy, nullable) dispatch_block_t frameDidChangeHandler;
@property (nonatomic, weak) TerminalViewController *hostedTerminalViewController;
@property (nonatomic) NSInteger workspaceDesktopIndex;
@property (nonatomic, copy) NSString *workspaceToolIdentifier;
@property (nonatomic, copy) NSString *workspaceTerminalRole;
@property (nonatomic) BOOL pinnedToBottomCenter;
- (void)bringWindowToFront;
@property (nonatomic) BOOL resizeHandleAtTopRight;
@property (nonatomic) BOOL titleBarDoubleTapZoomEnabled;
@property (nonatomic) BOOL zoomedToFullscreen;
@property (nonatomic) CGRect restoreFrameBeforeZoom;
@property (nonatomic) CGSize minimumSize;
@property (nonatomic) CGSize maximumSize;

- (instancetype)initWithTitle:(NSString *)title showsCloseButton:(BOOL)showsCloseButton;
- (void)setUtilityButtonTitle:(nullable NSString *)title handler:(nullable dispatch_block_t)handler;
- (void)applyWorkspaceChromeTheme:(NSDictionary<NSString *, UIColor *> *)theme active:(BOOL)active;
- (void)applyModernWorkspaceChromeTheme:(NSDictionary<NSString *, UIColor *> *)theme active:(BOOL)active;

@end

@implementation ISHWorkspaceContainedWindowView

- (instancetype)initWithTitle:(NSString *)title showsCloseButton:(BOOL)showsCloseButton {
    self = [super initWithFrame:CGRectZero];
    if (self == nil)
        return nil;

    self.autoresizingMask = UIViewAutoresizingNone;
    self.backgroundColor = UIColor.clearColor;
    self.layer.cornerRadius = ISHWorkspaceWindowCornerRadius;
    self.layer.masksToBounds = NO;
    self.layer.shadowColor = UIColor.blackColor.CGColor;
    self.layer.shadowOpacity = 0.18;
    self.layer.shadowRadius = 28;
    self.layer.shadowOffset = CGSizeMake(0, 16);
    self.draggable = YES;
    self.resizable = NO;
    self.pinnedToBottomCenter = NO;
    self.resizeHandleAtTopRight = NO;
    self.minimumSize = CGSizeMake(280, 180);
    self.maximumSize = CGSizeZero;
    self.frameBeforeBoundsClamp = CGRectNull;

    self.panelView = [UIView new];
    self.panelView.translatesAutoresizingMaskIntoConstraints = NO;
    self.panelView.layer.cornerRadius = ISHWorkspaceWindowCornerRadius;
    self.panelView.layer.masksToBounds = YES;
    self.panelView.layer.borderWidth = 1;
    if (@available(iOS 13.0, *)) {
        self.panelView.backgroundColor = UIColor.secondarySystemBackgroundColor;
    } else {
        self.panelView.backgroundColor = UIColor.whiteColor;
    }
    [self addSubview:self.panelView];

    self.titleBarView = [UIView new];
    self.titleBarView.translatesAutoresizingMaskIntoConstraints = NO;
    if (@available(iOS 13.0, *)) {
        self.titleBarView.backgroundColor = [UIColor.secondarySystemBackgroundColor colorWithAlphaComponent:0.96];
    } else {
        self.titleBarView.backgroundColor = [UIColor colorWithWhite:0.94 alpha:1.0];
    }
    [self.panelView addSubview:self.titleBarView];

    self.titleLabel = [UILabel new];
    self.titleLabel.translatesAutoresizingMaskIntoConstraints = NO;
    self.titleLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleSubheadline];
    self.titleLabel.textAlignment = NSTextAlignmentCenter;
    self.titleLabel.text = title;
    if (@available(iOS 13.0, *)) {
        self.titleLabel.textColor = UIColor.labelColor;
    } else {
        self.titleLabel.textColor = UIColor.blackColor;
    }
    [self.titleBarView addSubview:self.titleLabel];

    self.closeButton = [UIButton buttonWithType:UIButtonTypeSystem];
    self.closeButton.translatesAutoresizingMaskIntoConstraints = NO;
    [self.closeButton setTitle:@"×" forState:UIControlStateNormal];
    self.closeButton.titleLabel.font = [UIFont systemFontOfSize:14 weight:UIFontWeightSemibold];
    self.closeButton.hidden = !showsCloseButton;
    self.closeButton.alpha = showsCloseButton ? 1.0 : 0.0;
    self.closeButton.backgroundColor = [UIColor colorWithWhite:1.0 alpha:0.82];
    self.closeButton.layer.cornerRadius = ISHWorkspaceWindowButtonSize * 0.5;
    self.closeButton.accessibilityLabel = @"Close Window";
    [self.closeButton addTarget:self action:@selector(closePressed:) forControlEvents:UIControlEventTouchUpInside];
    [self.titleBarView addSubview:self.closeButton];

    self.utilityButton = [UIButton buttonWithType:UIButtonTypeSystem];
    self.utilityButton.translatesAutoresizingMaskIntoConstraints = NO;
    self.utilityButton.hidden = YES;
    self.utilityButton.alpha = 0.0;
    self.utilityButton.backgroundColor = [UIColor colorWithWhite:1.0 alpha:0.82];
    self.utilityButton.layer.cornerRadius = ISHWorkspaceWindowButtonSize * 0.5;
    self.utilityButton.titleLabel.font = [UIFont systemFontOfSize:10 weight:UIFontWeightSemibold];
    [self.utilityButton addTarget:self action:@selector(utilityPressed:) forControlEvents:UIControlEventTouchUpInside];
    [self.titleBarView addSubview:self.utilityButton];

    self.contentContainerView = [UIView new];
    self.contentContainerView.translatesAutoresizingMaskIntoConstraints = NO;
    if (@available(iOS 13.0, *)) {
        self.contentContainerView.backgroundColor = UIColor.systemBackgroundColor;
    } else {
        self.contentContainerView.backgroundColor = UIColor.whiteColor;
    }
    [self.panelView addSubview:self.contentContainerView];

    self.resizeHandleView = [UIView new];
    self.resizeHandleView.translatesAutoresizingMaskIntoConstraints = NO;
    self.resizeHandleView.hidden = YES;
    self.resizeHandleView.alpha = 0.0;
    self.resizeHandleView.layer.cornerRadius = 10;
    if (@available(iOS 13.0, *)) {
        self.resizeHandleView.backgroundColor = [UIColor.tertiaryLabelColor colorWithAlphaComponent:0.9];
    } else {
        self.resizeHandleView.backgroundColor = [UIColor colorWithWhite:0.65 alpha:0.9];
    }
    [self.panelView addSubview:self.resizeHandleView];

    self.resizeHandleLeadingConstraint =
        [self.resizeHandleView.leadingAnchor constraintGreaterThanOrEqualToAnchor:self.panelView.leadingAnchor constant:12];
    self.resizeHandleTrailingConstraint =
        [self.resizeHandleView.trailingAnchor constraintEqualToAnchor:self.panelView.trailingAnchor constant:-12];
    self.resizeHandleTopConstraint =
        [self.resizeHandleView.topAnchor constraintEqualToAnchor:self.panelView.topAnchor constant:12];
    self.resizeHandleBottomConstraint =
        [self.resizeHandleView.bottomAnchor constraintEqualToAnchor:self.panelView.bottomAnchor constant:-12];

    [NSLayoutConstraint activateConstraints:@[
        [self.panelView.topAnchor constraintEqualToAnchor:self.topAnchor],
        [self.panelView.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
        [self.panelView.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
        [self.panelView.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],

        [self.titleBarView.topAnchor constraintEqualToAnchor:self.panelView.topAnchor],
        [self.titleBarView.leadingAnchor constraintEqualToAnchor:self.panelView.leadingAnchor],
        [self.titleBarView.trailingAnchor constraintEqualToAnchor:self.panelView.trailingAnchor],
        [self.titleBarView.heightAnchor constraintEqualToConstant:ISHWorkspaceWindowTitleBarHeight],

        [self.closeButton.leadingAnchor constraintEqualToAnchor:self.titleBarView.leadingAnchor constant:ISHWorkspaceWindowButtonInset],
        [self.closeButton.centerYAnchor constraintEqualToAnchor:self.titleBarView.centerYAnchor],
        [self.closeButton.widthAnchor constraintEqualToConstant:ISHWorkspaceWindowButtonSize],
        [self.closeButton.heightAnchor constraintEqualToConstant:ISHWorkspaceWindowButtonSize],

        [self.utilityButton.trailingAnchor constraintEqualToAnchor:self.titleBarView.trailingAnchor constant:-ISHWorkspaceWindowButtonInset],
        [self.utilityButton.centerYAnchor constraintEqualToAnchor:self.titleBarView.centerYAnchor],
        [self.utilityButton.widthAnchor constraintEqualToConstant:34],
        [self.utilityButton.heightAnchor constraintEqualToConstant:ISHWorkspaceWindowButtonSize],

        [self.titleLabel.leadingAnchor constraintEqualToAnchor:self.titleBarView.leadingAnchor constant:ISHWorkspaceWindowTitleSideInset],
        [self.titleLabel.trailingAnchor constraintEqualToAnchor:self.titleBarView.trailingAnchor constant:-ISHWorkspaceWindowTitleSideInset],
        [self.titleLabel.centerYAnchor constraintEqualToAnchor:self.titleBarView.centerYAnchor],

        [self.contentContainerView.topAnchor constraintEqualToAnchor:self.titleBarView.bottomAnchor],
        [self.contentContainerView.leadingAnchor constraintEqualToAnchor:self.panelView.leadingAnchor],
        [self.contentContainerView.trailingAnchor constraintEqualToAnchor:self.panelView.trailingAnchor],
        [self.contentContainerView.bottomAnchor constraintEqualToAnchor:self.panelView.bottomAnchor],

        [self.resizeHandleView.widthAnchor constraintEqualToConstant:20],
        [self.resizeHandleView.heightAnchor constraintEqualToConstant:20],
        self.resizeHandleLeadingConstraint,
        self.resizeHandleTrailingConstraint,
        self.resizeHandleBottomConstraint,
    ]];

    UIPanGestureRecognizer *panGestureRecognizer = [[UIPanGestureRecognizer alloc] initWithTarget:self action:@selector(handlePan:)];
    panGestureRecognizer.delegate = self;
    [self.titleBarView addGestureRecognizer:panGestureRecognizer];

    UITapGestureRecognizer *titleBarDoubleTapGestureRecognizer =
        [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(handleTitleBarDoubleTap:)];
    titleBarDoubleTapGestureRecognizer.numberOfTapsRequired = 2;
    titleBarDoubleTapGestureRecognizer.cancelsTouchesInView = NO;
    titleBarDoubleTapGestureRecognizer.delegate = self;
    [self.titleBarView addGestureRecognizer:titleBarDoubleTapGestureRecognizer];

    UIPanGestureRecognizer *resizeGestureRecognizer = [[UIPanGestureRecognizer alloc] initWithTarget:self action:@selector(handleResizePan:)];
    [self.resizeHandleView addGestureRecognizer:resizeGestureRecognizer];

    UITapGestureRecognizer *tapGestureRecognizer = [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(bringWindowToFront)];
    tapGestureRecognizer.cancelsTouchesInView = NO;
    tapGestureRecognizer.delegate = self;
    [tapGestureRecognizer requireGestureRecognizerToFail:titleBarDoubleTapGestureRecognizer];
    [self addGestureRecognizer:tapGestureRecognizer];

    return self;
}

- (void)applyWorkspaceChromeTheme:(NSDictionary<NSString *, UIColor *> *)theme active:(BOOL)active {
    if (![theme isKindOfClass:NSDictionary.class])
        return;

    if (ISHWorkspaceUsesModernStyle()) {
        [self applyModernWorkspaceChromeTheme:theme active:active];
        return;
    }

    UIColor *strokeColor = active
        ? [(theme[@"focusRing"] ?: theme[@"accent"]) colorWithAlphaComponent:0.95]
        : [theme[@"stroke"] colorWithAlphaComponent:0.98];
    self.panelView.layer.borderWidth = active ? 1.6 : 1.0;
    self.panelView.layer.borderColor = strokeColor.CGColor;
    self.panelView.backgroundColor = [theme[@"card"] colorWithAlphaComponent:0.98];
    self.titleBarView.backgroundColor = [theme[@"cardAlt"] colorWithAlphaComponent:0.98];
    self.contentContainerView.backgroundColor = [theme[@"card"] colorWithAlphaComponent:0.98];
    self.titleLabel.textColor = theme[@"primary"];

    UIColor *buttonBackground = [theme[@"backgroundTop"] colorWithAlphaComponent:0.14];
    UIColor *buttonBorder = [theme[@"accentAlt"] colorWithAlphaComponent:0.34];
    self.closeButton.backgroundColor = buttonBackground;
    self.closeButton.layer.borderWidth = 1;
    self.closeButton.layer.borderColor = buttonBorder.CGColor;
    [self.closeButton setTitleColor:theme[@"accent"] forState:UIControlStateNormal];

    self.utilityButton.backgroundColor = buttonBackground;
    self.utilityButton.layer.borderWidth = 1;
    self.utilityButton.layer.borderColor = buttonBorder.CGColor;
    [self.utilityButton setTitleColor:theme[@"accent"] forState:UIControlStateNormal];

    self.resizeHandleView.backgroundColor = active
        ? [(theme[@"focusRing"] ?: theme[@"accent"]) colorWithAlphaComponent:0.92]
        : [(theme[@"focusRing"] ?: theme[@"accentAlt"]) colorWithAlphaComponent:0.55];
    self.layer.shadowColor = [theme[@"backgroundTop"] colorWithAlphaComponent:0.55].CGColor;
}

- (void)applyModernWorkspaceChromeTheme:(NSDictionary<NSString *, UIColor *> *)theme active:(BOOL)active {
    BOOL dark = UserPreferences.shared.requestingDarkAppearance;
    UIColor *accent = theme[@"accent"];
    UIColor *focusRing = theme[@"focusRing"] ?: accent;

    // Modern uses neutral light/dark surfaces (the chrome theme only supplies the
    // accent), so the windows stay coherent with the flat desktop in both appearances.
    UIColor *card = dark ? [UIColor colorWithRed:0.13 green:0.14 blue:0.17 alpha:1.0]
                         : UIColor.whiteColor;
    UIColor *titleText = dark ? [UIColor colorWithWhite:0.92 alpha:1.0]
                              : [UIColor colorWithRed:0.11 green:0.13 blue:0.18 alpha:1.0];
    UIColor *hairline = dark ? [UIColor colorWithWhite:1.0 alpha:0.14]
                             : [UIColor colorWithWhite:0.0 alpha:0.10];
    UIColor *mutedGlyph = dark ? [UIColor colorWithWhite:0.62 alpha:1.0]
                               : [UIColor colorWithWhite:0.42 alpha:1.0];

    self.panelView.backgroundColor = card;
    self.contentContainerView.backgroundColor = card;
    // Focused window: solid accent title bar with white text. Otherwise the flat card.
    self.titleBarView.backgroundColor = active ? accent : card;
    self.titleLabel.textColor = active ? UIColor.whiteColor : titleText;

    // Hairline border in repose; a crisp accent ring marks focus.
    self.panelView.layer.borderWidth = active ? 1.5 : 1.0;
    self.panelView.layer.borderColor = (active ? focusRing : hairline).CGColor;

    // Quiet, flat window controls: tinted glyphs, no filled chips or borders.
    self.closeButton.backgroundColor = UIColor.clearColor;
    self.closeButton.layer.borderWidth = 0.0;
    [self.closeButton setTitleColor:(active ? UIColor.whiteColor : mutedGlyph) forState:UIControlStateNormal];
    self.utilityButton.backgroundColor = UIColor.clearColor;
    self.utilityButton.layer.borderWidth = 0.0;
    [self.utilityButton setTitleColor:(active ? UIColor.whiteColor : mutedGlyph) forState:UIControlStateNormal];

    self.resizeHandleView.backgroundColor = active ? [focusRing colorWithAlphaComponent:0.9] : hairline;

    // Neutral flat elevation in both appearances.
    self.layer.shadowColor = UIColor.blackColor.CGColor;
}

- (void)layoutSubviews {
    [super layoutSubviews];
    UIBezierPath *shadowPath = [UIBezierPath bezierPathWithRoundedRect:self.bounds cornerRadius:ISHWorkspaceWindowCornerRadius];
    self.layer.shadowPath = shadowPath.CGPath;
}

- (void)bringWindowToFront {
    [self.superview bringSubviewToFront:self];
    if (self.didBecomeFrontmostHandler != nil)
        self.didBecomeFrontmostHandler();
}

- (void)closePressed:(id)sender {
    if (self.closeHandler != nil)
        self.closeHandler();
}

- (void)utilityPressed:(id)sender {
    if (self.utilityHandler != nil)
        self.utilityHandler();
}

// Keep the window's drag / bring-to-front / double-tap-zoom gestures from intercepting touches
// that land on a control (the close "×" and utility buttons). The title-bar pan recognizer's
// cancelsTouchesInView defaults to YES, so without this the slightest finger movement on the
// small × cancels the button's touch and touchUpInside never fires — the window looks
// unclosable. Letting the buttons own their own touches makes them respond immediately.
- (BOOL)gestureRecognizer:(UIGestureRecognizer *)gestureRecognizer shouldReceiveTouch:(UITouch *)touch {
    for (UIView *view = touch.view; view != nil && view != self; view = view.superview) {
        if ([view isKindOfClass:UIControl.class])
            return NO;
    }
    return YES;
}

- (void)setUtilityButtonTitle:(NSString *)title handler:(dispatch_block_t)handler {
    self.utilityHandler = handler;
    BOOL visible = title.length > 0 && handler != nil;
    [self.utilityButton setTitle:title forState:UIControlStateNormal];
    self.utilityButton.accessibilityLabel = title;
    self.utilityButton.hidden = !visible;
    self.utilityButton.alpha = visible ? 1.0 : 0.0;
}

- (void)setResizable:(BOOL)resizable {
    _resizable = resizable;
    self.resizeHandleView.hidden = !resizable;
    self.resizeHandleView.alpha = resizable ? 1.0 : 0.0;
}

- (void)setResizeHandleAtTopRight:(BOOL)resizeHandleAtTopRight {
    if (_resizeHandleAtTopRight == resizeHandleAtTopRight)
        return;
    _resizeHandleAtTopRight = resizeHandleAtTopRight;
    self.resizeHandleTopConstraint.active = resizeHandleAtTopRight;
    self.resizeHandleBottomConstraint.active = !resizeHandleAtTopRight;
}

- (void)handlePan:(UIPanGestureRecognizer *)recognizer {
    if (!self.draggable || self.superview == nil || self.zoomedToFullscreen)
        return;

    if (recognizer.state == UIGestureRecognizerStateBegan) {
        self.pinnedToBottomCenter = NO;
        [self bringWindowToFront];
    }

    CGPoint translation = [recognizer translationInView:self.superview];
    CGRect frame = self.frame;
    frame.origin.x += translation.x;
    frame.origin.y += translation.y;
    CGFloat visibleWidth = MIN(CGRectGetWidth(frame), 140.0);
    CGFloat minX = -(CGRectGetWidth(frame) - visibleWidth);
    CGFloat maxX = CGRectGetWidth(self.superview.bounds) - visibleWidth;
    CGFloat visibleHeight = MIN(CGRectGetHeight(frame), ISHWorkspaceWindowTitleBarHeight);
    CGFloat minY = 0;
    CGFloat maxY = CGRectGetHeight(self.superview.bounds) - visibleHeight;
    if (maxX < minX)
        maxX = minX;
    if (maxY < minY)
        maxY = minY;
    frame.origin.x = MIN(MAX(frame.origin.x, minX), maxX);
    frame.origin.y = MIN(MAX(frame.origin.y, minY), maxY);
    self.frame = ISHWorkspaceRectWithRoundedOriginPreservingSize(frame);
    // The user chose this spot; forget any pre-clamp frame a smaller surface stashed,
    // or the next layout pass would yank the window back out from under them.
    self.frameBeforeBoundsClamp = CGRectNull;
    if (self.frameDidChangeHandler != nil)
        self.frameDidChangeHandler();
    [recognizer setTranslation:CGPointZero inView:self.superview];
}

- (void)setPreferredSize:(CGSize)preferredSize {
    _preferredSize = preferredSize;
    // Every deliberate size change flows through here (resize pan, zoom restore,
    // programmatic placement); a stale pre-clamp stash must not override it later.
    _frameBeforeBoundsClamp = CGRectNull;
}

- (void)handleResizePan:(UIPanGestureRecognizer *)recognizer {
    if (!self.resizable || self.superview == nil || self.zoomedToFullscreen)
        return;

    if (recognizer.state == UIGestureRecognizerStateBegan) {
        [self bringWindowToFront];
    }

    CGPoint translation = [recognizer translationInView:self.superview];
    CGRect frame = self.frame;
    CGFloat maxWidth = self.pinnedToBottomCenter ? CGRectGetWidth(self.superview.bounds)
                                                 : CGRectGetWidth(self.superview.bounds) - CGRectGetMinX(frame);
    CGFloat maxHeight = self.pinnedToBottomCenter ? CGRectGetMaxY(frame)
                                                  : CGRectGetHeight(self.superview.bounds) - CGRectGetMinY(frame);
    CGSize minimumSize = self.minimumSize;
    CGFloat targetWidth = MAX(minimumSize.width, CGRectGetWidth(frame) + translation.x);
    CGFloat targetHeight = MAX(minimumSize.height,
                               CGRectGetHeight(frame) + (self.resizeHandleAtTopRight ? -translation.y : translation.y));
    if (self.maximumSize.width > 0) {
        targetWidth = MIN(targetWidth, self.maximumSize.width);
    }
    if (self.maximumSize.height > 0) {
        targetHeight = MIN(targetHeight, self.maximumSize.height);
    }
    frame.size.width = MIN(targetWidth, maxWidth);
    frame.size.height = MIN(targetHeight, maxHeight);
    if (self.pinnedToBottomCenter) {
        frame.origin.x = (CGRectGetWidth(self.superview.bounds) - CGRectGetWidth(frame)) * 0.5;
        frame.origin.y = CGRectGetHeight(self.superview.bounds) - CGRectGetHeight(frame);
    }
    self.frame = CGRectIntegral(frame);
    self.preferredSize = frame.size;
    if (self.frameDidChangeHandler != nil)
        self.frameDidChangeHandler();
    [recognizer setTranslation:CGPointZero inView:self.superview];
}

- (void)handleTitleBarDoubleTap:(UITapGestureRecognizer *)recognizer {
    if (!self.titleBarDoubleTapZoomEnabled || self.superview == nil || recognizer.state != UIGestureRecognizerStateRecognized)
        return;

    UIView *touchedView = [self.titleBarView hitTest:[recognizer locationInView:self.titleBarView] withEvent:nil];
    for (UIView *view = touchedView; view != nil; view = view.superview) {
        if ([view isKindOfClass:UIControl.class])
            return;
        if (view == self.titleBarView)
            break;
    }

    [self bringWindowToFront];
    BOOL didRestore;
    if (self.zoomedToFullscreen) {
        CGRect restoreFrame = self.restoreFrameBeforeZoom;
        if (CGRectIsEmpty(restoreFrame) || CGRectIsNull(restoreFrame))
            return;
        self.zoomedToFullscreen = NO;
        restoreFrame = ISHWorkspaceRectWithRoundedOriginPreservingSize(CGRectIntegral(restoreFrame));
        self.frame = restoreFrame;
        self.preferredSize = restoreFrame.size;
        didRestore = YES;
    } else {
        self.restoreFrameBeforeZoom = ISHWorkspaceRectWithRoundedOriginPreservingSize(CGRectIntegral(self.frame));
        self.zoomedToFullscreen = YES;
        CGRect fullscreenFrame = self.superview.bounds;
        self.frame = ISHWorkspaceRectWithRoundedOriginPreservingSize(CGRectIntegral(fullscreenFrame));
        didRestore = NO;
    }

    // Only persist on the way back down. A window with a frameDidChangeHandler
    // that autosaves position on every change (the pinned Launcher/Desktops
    // utilities) would otherwise have its fullscreen frame saved as the new
    // "normal" spot the instant it's zoomed, so relaunching would restore it
    // maximized instead of wherever the user actually left it. Restoring is
    // safe to persist -- it's exactly the frame that's already saved.
    if (didRestore && self.frameDidChangeHandler != nil)
        self.frameDidChangeHandler();
}

@end

UINavigationController *ISHCreateWorkspaceNavigationController(void) {
    return ISHCreateWorkspaceNavigationControllerForTool(nil);
}

UINavigationController *ISHCreateWorkspaceNavigationControllerForTool(NSString *toolIdentifier) {
    WorkspaceViewController *workspaceViewController = [WorkspaceViewController new];
    workspaceViewController.initialToolIdentifier = toolIdentifier;
    UINavigationController *navigationController = [[UINavigationController alloc] initWithRootViewController:workspaceViewController];
    navigationController.navigationBarHidden = YES;
    return navigationController;
}

static UINavigationController *ISHCreateRootsNavigationController(void) {
    UIViewController *rootsViewController = [[UIStoryboard storyboardWithName:@"Roots" bundle:nil] instantiateInitialViewController];
    return [[UINavigationController alloc] initWithRootViewController:rootsViewController];
}

static UIViewController *ISHCreateRootsViewController(void) {
    return [[UIStoryboard storyboardWithName:@"Roots" bundle:nil] instantiateInitialViewController];
}

// The Launcher applet sizes itself to its item count: an optional "‹ Back" row, the item list
// (a real UITableView since the native-reorder rewrite), and the Add/Edit footer row, with
// insets — each of those three pieces has its own height, so they're tracked separately rather
// than folded into one "row count" the way the old custom-button stack view allowed (that's
// what let this go stale and clip the footer off-window after the table-view rewrite: the old
// formula's per-row height and its single "Edit Shortcuts" button height no longer matched the
// table's actual row height or the new two-button footer). Used both as the preferred open size
// (root level, no back row) and by -autosizeLauncherWindowForItemCount:showsBackRow: (current
// level) when shortcuts are added/removed/drilled into.
static CGSize ISHWorkspaceLauncherContentSize(NSUInteger itemRowCount, BOOL showsBackRow) {
    BOOL phone = ISHWorkspaceUsesPhoneLayout();
    CGFloat inset = phone ? 12.0 : 16.0;
    CGFloat spacing = 8.0;
    CGFloat width = phone ? 168.0 : 200.0;
    CGFloat tableRowHeight = phone ? 34.0 : 38.0;              // matches _tableView.rowHeight
    CGFloat backRowHeight = phone ? 28.0 : 30.0;                // matches launcherBackButton's minimum height
    CGFloat footerRowHeight = (phone ? 26.0 : 28.0) + 8.0;      // Add/Edit pill height + its 4pt top/bottom margins

    CGFloat itemsHeight = itemRowCount > 0 ? (itemRowCount * tableRowHeight) : (phone ? 28.0 : 32.0);
    CGFloat height = inset + itemsHeight + spacing + footerRowHeight + inset;
    if (showsBackRow)
        height += backRowHeight + spacing;
    return CGSizeMake(width, height);
}

// The Desktops applet sizes itself to the Desktop count: a vertical list of "Desktop N" rows
// plus the "New Desktop" button and the Save/Restore row. Used as its preferred/fallback size
// and by -autosizeWorkspacesWindow when Desktops are added/removed.
static CGSize ISHWorkspaceWorkspacesContentSize(NSUInteger count) {
    BOOL phone = ISHWorkspaceUsesPhoneLayout();
    NSUInteger n = MAX(count, (NSUInteger)1);
    CGFloat rowHeight = phone ? 30.0 : 38.0;
    CGFloat newDesktopHeight = phone ? 30.0 : 40.0;
    CGFloat spacing = phone ? 5.0 : 6.0;
    CGFloat cardPadding = phone ? 12.0 : 16.0;
    CGFloat actionsHeight = phone ? 36.0 : 44.0;
    CGFloat chrome = phone ? 24.0 : 32.0;
    CGFloat width = phone ? 214.0 : 254.0;
    CGFloat rowsHeight = n * rowHeight + n * spacing + newDesktopHeight;
    CGFloat height = rowsHeight + cardPadding + actionsHeight + chrome;
    return CGSizeMake(width, height);
}

static CGSize ISHWorkspaceMonitorContentSize(void);

// The Music player is a fixed-width "device": the window hugs it horizontally
// (locked, non-resizable) and only grows/shrinks vertically. Width = the device
// body width plus a thin margin on each side.
static CGFloat ISHWorkspaceAudioDeviceWidth(void) {
    return ISHWorkspaceUsesPhoneLayout() ? 240.0 : 200.0;
}
static CGFloat ISHWorkspaceAudioWindowWidth(void) {
    return ISHWorkspaceAudioDeviceWidth() + 12.0;  // 6pt margin each side
}

static CGSize ISHWorkspacePreferredToolContentSize(NSString *toolIdentifier) {
    if (ISHWorkspaceUsesPhoneLayout()) {
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolClockIdentifier])
            return CGSizeMake(144, 74);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolInfoIdentifier])
            return CGSizeMake(280, 154);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolMonitorIdentifier])
            return ISHWorkspaceMonitorContentSize();
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolNetworksIdentifier])
            return CGSizeMake(328, 176);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolStatusIdentifier])
            return CGSizeMake(340, 248);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolWorkspacesIdentifier])
            return ISHWorkspaceWorkspacesContentSize(1);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolSessionsIdentifier])
            return CGSizeMake(332, 238);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolStorageIdentifier])
            return CGSizeMake(336, 256);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolShortcutsIdentifier])
            return CGSizeMake(312, 184);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolBrowserIdentifier])
            return CGSizeMake(352, 248);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolThemesIdentifier])
            return CGSizeMake(360, 620);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolDiagnosticsIdentifier])
            return CGSizeMake(352, 620);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolFilesystemsIdentifier])
            return CGSizeMake(352, 620);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolSettingsIdentifier])
            return CGSizeMake(352, 620);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolLLMIdentifier])
            return CGSizeMake(352, 560);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolLauncherIdentifier])
            return ISHWorkspaceLauncherContentSize(ISHWorkspaceLauncherShortcuts().count, NO);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolAudioIdentifier])
            return CGSizeMake(ISHWorkspaceAudioWindowWidth(), 510);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolMotePadIdentifier])
            return CGSizeMake(340, 460);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolFileManagerIdentifier])
            return CGSizeMake(340, 460);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolMarkdownViewerIdentifier])
            return CGSizeMake(340, 460);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolImageViewerIdentifier])
            return CGSizeMake(340, 420);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolVideoPlayerIdentifier])
            return CGSizeMake(340, 260);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolDisplayIdentifier])
            return CGSizeMake(344, 300);
        return CGSizeMake(344, 580);
    }
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolClockIdentifier])
        return CGSizeMake(156, 76);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolInfoIdentifier])
        return CGSizeMake(318, 168);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolMonitorIdentifier])
        return ISHWorkspaceMonitorContentSize();
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolNetworksIdentifier])
        return CGSizeMake(360, 188);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolStatusIdentifier])
        return CGSizeMake(460, 300);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolWorkspacesIdentifier])
        return ISHWorkspaceWorkspacesContentSize(1);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolSessionsIdentifier])
        return CGSizeMake(460, 286);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolStorageIdentifier])
        return CGSizeMake(500, 320);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolShortcutsIdentifier])
        return CGSizeMake(400, 220);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolBrowserIdentifier])
        return CGSizeMake(620, 420);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolThemesIdentifier])
        return CGSizeMake(820, 760);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolDiagnosticsIdentifier])
        return CGSizeMake(760, 700);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolFilesystemsIdentifier])
        return CGSizeMake(760, 720);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolSettingsIdentifier])
        return CGSizeMake(760, 760);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolLLMIdentifier])
        return CGSizeMake(560, 620);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolLauncherIdentifier])
        return ISHWorkspaceLauncherContentSize(ISHWorkspaceLauncherShortcuts().count, NO);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolAudioIdentifier])
        return CGSizeMake(ISHWorkspaceAudioWindowWidth(), 470);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolMotePadIdentifier])
        return CGSizeMake(620, 560);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolFileManagerIdentifier])
        return CGSizeMake(720, 480);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolMarkdownViewerIdentifier])
        return CGSizeMake(620, 640);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolImageViewerIdentifier])
        return CGSizeMake(620, 520);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolVideoPlayerIdentifier])
        return CGSizeMake(720, 460);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolDisplayIdentifier])
        return CGSizeMake(680, 480);
    return CGSizeMake(720, 640);
}

static CGSize ISHWorkspacePreferredTerminalContentSize(void) {
    if (ISHWorkspaceUsesPhoneLayout())
        return CGSizeMake(372, 288);
    return CGSizeMake(900, 620);
}

static CGSize ISHWorkspacePreferredDockContentSize(void) {
    if (ISHWorkspaceUsesPhoneLayout())
        return CGSizeMake(188, 56);
    return CGSizeMake(220, 64);
}

static CGSize ISHWorkspacePreferredDashboardContentSize(void) {
    if (ISHWorkspaceUsesPhoneLayout())
        return CGSizeMake(312, 176);
    return CGSizeMake(360, 214);
}

static CGSize ISHWorkspaceMinimumDashboardContentSize(void) {
    if (ISHWorkspaceUsesPhoneLayout())
        return CGSizeMake(284, 154);
    return CGSizeMake(324, 184);
}

static CGSize ISHWorkspaceMinimumDockContentSize(void) {
    if (ISHWorkspaceUsesPhoneLayout())
        return CGSizeMake(156, 48);
    return CGSizeMake(180, 56);
}

static CGSize ISHWorkspaceMaximumDockContentSize(void) {
    if (ISHWorkspaceUsesPhoneLayout())
        return CGSizeMake(240, 88);
    return CGSizeMake(320, 104);
}

static CGSize ISHWorkspaceMinimumTerminalContentSize(void) {
    // Half the previous minimums, so terminals can be dragged down to ~half their old smallest.
    if (ISHWorkspaceUsesPhoneLayout())
        return CGSizeMake(150, 110);
    return CGSizeMake(260, 170);
}

static CGSize ISHWorkspaceMinimumToolContentSize(NSString *toolIdentifier) {
    if (ISHWorkspaceUsesPhoneLayout()) {
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolClockIdentifier])
            return CGSizeMake(132, 68);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolInfoIdentifier])
            return CGSizeMake(232, 132);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolMonitorIdentifier])
            return CGSizeMake(280, 144);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolNetworksIdentifier])
            return CGSizeMake(280, 150);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolStatusIdentifier])
            return CGSizeMake(300, 220);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolWorkspacesIdentifier])
            return CGSizeMake(110, 108);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolSessionsIdentifier])
            return CGSizeMake(280, 176);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolStorageIdentifier])
            return CGSizeMake(288, 188);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolShortcutsIdentifier])
            return CGSizeMake(260, 148);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolBrowserIdentifier])
            return CGSizeMake(300, 184);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolThemesIdentifier])
            return CGSizeMake(320, 420);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolDiagnosticsIdentifier])
            return CGSizeMake(320, 420);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolFilesystemsIdentifier])
            return CGSizeMake(320, 420);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolSettingsIdentifier])
            return CGSizeMake(320, 420);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolLLMIdentifier])
            return CGSizeMake(300, 360);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolLauncherIdentifier])
            return CGSizeMake(200, 80);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolAudioIdentifier])
            return CGSizeMake(ISHWorkspaceAudioWindowWidth(), 390);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolMotePadIdentifier])
            return CGSizeMake(280, 300);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolFileManagerIdentifier])
            return CGSizeMake(280, 300);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolMarkdownViewerIdentifier])
            return CGSizeMake(260, 260);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolImageViewerIdentifier])
            return CGSizeMake(240, 240);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolVideoPlayerIdentifier])
            return CGSizeMake(240, 180);
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolDisplayIdentifier])
            return CGSizeMake(280, 220);
        return CGSizeMake(300, 220);
    }

    if ([toolIdentifier isEqualToString:ISHWorkspaceToolClockIdentifier])
        return CGSizeMake(150, 72);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolInfoIdentifier])
        return CGSizeMake(260, 144);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolMonitorIdentifier])
        return CGSizeMake(300, 150);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolNetworksIdentifier])
        return CGSizeMake(300, 156);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolStatusIdentifier])
        return CGSizeMake(360, 220);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolWorkspacesIdentifier])
        return CGSizeMake(124, 116);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolSessionsIdentifier])
        return CGSizeMake(340, 208);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolStorageIdentifier])
        return CGSizeMake(360, 220);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolShortcutsIdentifier])
        return CGSizeMake(300, 164);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolBrowserIdentifier])
        return CGSizeMake(360, 240);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolThemesIdentifier])
        return CGSizeMake(520, 560);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolLLMIdentifier])
        return CGSizeMake(420, 420);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolLauncherIdentifier])
        return CGSizeMake(220, 96);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolAudioIdentifier])
        return CGSizeMake(ISHWorkspaceAudioWindowWidth(), 360);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolMotePadIdentifier])
        return CGSizeMake(360, 300);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolFileManagerIdentifier])
        return CGSizeMake(420, 320);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolMarkdownViewerIdentifier])
        return CGSizeMake(360, 320);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolImageViewerIdentifier])
        return CGSizeMake(320, 280);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolVideoPlayerIdentifier])
        return CGSizeMake(320, 220);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolDisplayIdentifier])
        return CGSizeMake(400, 300);
    return CGSizeZero;
}

// Per-tool maximum content size. Width > 0 locks the window's width; height 0
// leaves vertical resize unbounded. Only the Music player uses it (fixed width).
static CGSize ISHWorkspaceMaximumToolContentSize(NSString *toolIdentifier) {
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolAudioIdentifier])
        return CGSizeMake(ISHWorkspaceAudioWindowWidth(), 0);
    return CGSizeZero;
}

static NSDictionary<NSString *, NSNumber *> *ISHWorkspaceSizeDescriptor(CGSize size) {
    return @{
        @"width": @(MAX(0, size.width)),
        @"height": @(MAX(0, size.height)),
    };
}

static CGSize ISHWorkspaceSizeFromDescriptor(NSDictionary<NSString *, id> *descriptor) {
    if (![descriptor isKindOfClass:NSDictionary.class])
        return CGSizeZero;
    return CGSizeMake([descriptor[@"width"] doubleValue], [descriptor[@"height"] doubleValue]);
}

static NSString *ISHWorkspaceToolTitle(NSString *toolIdentifier) {
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolClockIdentifier])
        return @"Clock";
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolInfoIdentifier])
        return @"Info";
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolMonitorIdentifier])
        return @"Monitor";
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolNetworksIdentifier])
        return @"Networks";
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolStatusIdentifier])
        return @"Logs";
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolWorkspacesIdentifier])
        return @"Desktops";
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolSessionsIdentifier])
        return @"Sessions";
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolStorageIdentifier])
        return @"Storage";
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolShortcutsIdentifier])
        return @"Quick Actions";
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolBrowserIdentifier])
        return @"Browser";
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolThemesIdentifier])
        return @"Themes";
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolDiagnosticsIdentifier])
        return @"Diagnostics";
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolFilesystemsIdentifier])
        return @"Boot Images";
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolSettingsIdentifier])
        return @"Settings";
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolLLMIdentifier])
        return @"LLM Chat";
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolLauncherIdentifier])
        return @"Launcher";
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolAudioIdentifier])
        return @"Music";
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolMotePadIdentifier])
        return @"MotePad";
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolFileManagerIdentifier])
        return @"File Manager";
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolMarkdownViewerIdentifier])
        return @"Markdown";
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolImageViewerIdentifier])
        return @"Image Viewer";
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolVideoPlayerIdentifier])
        return @"Video Player";
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolDisplayIdentifier])
        return @"Wayland";
    return @"Window";
}

BOOL ISHShouldLaunchWorkspaceAtStartup(void) {
    NSString *initialWindow = [NSUserDefaults.standardUserDefaults stringForKey:kPreferenceInitialWindowKey];
    return [initialWindow isEqualToString:ISHInitialWindowWorkspaceValue];
}

BOOL ISHShouldLaunchWaylandDisplayAtStartup(void) {
    NSString *initialWindow = [NSUserDefaults.standardUserDefaults stringForKey:kPreferenceInitialWindowKey];
    return [initialWindow isEqualToString:ISHInitialWindowWaylandValue];
}

static NSString *ISHInitialWindowTitle(void) {
    NSString *initialWindow = [NSUserDefaults.standardUserDefaults stringForKey:kPreferenceInitialWindowKey];
    if ([initialWindow isEqualToString:ISHInitialWindowWorkspaceValue])
        return @"Workspace";
    if ([initialWindow isEqualToString:ISHInitialWindowChooseFilesystemValue])
        return @"Choose Filesystem";
    if ([initialWindow isEqualToString:ISHInitialWindowWaylandValue])
        return @"Wayland Display";
    if ([initialWindow isEqualToString:@"session-shell"])
        return @"Session Shell (pts/1)";
    return @"Plain Terminal";
}

static NSString *ISHWorkspaceTerminalDisplayName(Terminal *terminal) {
    if (terminal == nil)
        return @"Unknown Terminal";
    int consoleMajor = TTY_CONSOLE_MAJOR;
    int consoleMinor = 1;
    get_console_device(&consoleMajor, &consoleMinor);
    if (terminal.type == consoleMajor && terminal.number == consoleMinor) {
        if (consoleMajor == TTY_CONSOLE_MAJOR)
            return [NSString stringWithFormat:@"System Console (tty%d)", consoleMinor];
        if (consoleMajor == TTY_PSEUDO_SLAVE_MAJOR)
            return [NSString stringWithFormat:@"System Console (pts/%d)", consoleMinor];
        return [NSString stringWithFormat:@"System Console (%d:%d)", consoleMajor, consoleMinor];
    }
    if (terminal.type == TTY_CONSOLE_MAJOR)
        return [NSString stringWithFormat:@"Terminal (tty%d)", terminal.number];
    if (terminal.type == TTY_PSEUDO_SLAVE_MAJOR)
        return [NSString stringWithFormat:@"Pseudo Terminal (pts/%d)", terminal.number];
    return [NSString stringWithFormat:@"Terminal (%d:%d)", terminal.type, terminal.number];
}

static NSString *ISHWorkspaceTerminalRoleForTerminal(Terminal *terminal) {
    if (terminal == nil)
        return ISHWorkspaceTerminalRoleGeneric;
    int consoleMajor = TTY_CONSOLE_MAJOR;
    int consoleMinor = 1;
    get_console_device(&consoleMajor, &consoleMinor);
    if ((terminal.type == consoleMajor && terminal.number == consoleMinor) ||
        terminal.type == TTY_CONSOLE_MAJOR) {
        return ISHWorkspaceTerminalRoleSystemConsole;
    }
    if (terminal.type == TTY_PSEUDO_SLAVE_MAJOR)
        return ISHWorkspaceTerminalRoleSessionShell;
    return ISHWorkspaceTerminalRoleGeneric;
}

static NSString *ISHWorkspaceDiagnosticsString(id value) {
    if (value == nil || value == (id)kCFNull)
        return nil;
    if ([value isKindOfClass:NSString.class])
        return (NSString *)value;
    if ([value respondsToSelector:@selector(stringValue)])
        return [value stringValue];
    return [value description];
}

static NSString *ISHWorkspaceTitleForTerminalRole(NSString *terminalRole, Terminal *terminal) {
    if ([terminalRole isEqualToString:ISHWorkspaceTerminalRoleSystemConsole])
        return @"System Console";
    if ([terminalRole isEqualToString:ISHWorkspaceTerminalRoleSessionShell])
        return @"Session Shell";
    if (terminal != nil)
        return ISHWorkspaceTerminalDisplayName(terminal);
    return @"Terminal";
}

static NSString *ISHWorkspaceSceneRoleDescription(UISceneSession *session) API_AVAILABLE(ios(13.0));
static NSString *ISHWorkspaceSceneRoleDescription(UISceneSession *session) {
    NSString *activityType = session.stateRestorationActivity.activityType;
    if ([activityType isEqualToString:ISHSceneActivityTypeWorkspace])
        return @"Workspace";
    if ([activityType isEqualToString:ISHSceneActivityTypeTerminal] ||
        [activityType isEqualToString:@"app.ish.scene"]) {
        return @"Terminal";
    }
    return @"Unknown";
}

static UIViewController *ISHWorkspaceRootViewControllerForScene(UIScene *scene) API_AVAILABLE(ios(13.0));
static UIViewController *ISHWorkspaceRootViewControllerForScene(UIScene *scene) {
    if (![scene isKindOfClass:UIWindowScene.class])
        return nil;
    for (UIWindow *window in ((UIWindowScene *) scene).windows) {
        if (window.rootViewController != nil)
            return window.rootViewController;
    }
    return nil;
}

static UIViewController *ISHWorkspaceTopViewController(UIViewController *viewController) {
    if ([viewController isKindOfClass:UINavigationController.class])
        return ((UINavigationController *) viewController).topViewController;
    return viewController;
}

static NSString *ISHWorkspaceSceneRoleDescriptionForScene(UISceneSession *session, UIScene *scene) API_AVAILABLE(ios(13.0));
static NSString *ISHWorkspaceSceneRoleDescriptionForScene(UISceneSession *session, UIScene *scene) {
    NSString *role = ISHWorkspaceSceneRoleDescription(session);
    if (![role isEqualToString:@"Unknown"] || scene == nil)
        return role;

    UIViewController *rootViewController = ISHWorkspaceRootViewControllerForScene(scene);
    UIViewController *topViewController = ISHWorkspaceTopViewController(rootViewController);
    if ([topViewController isKindOfClass:WorkspaceViewController.class])
        return @"Workspace";
    if ([rootViewController isKindOfClass:TerminalViewController.class] ||
            [topViewController isKindOfClass:TerminalViewController.class])
        return @"Terminal";
    return role;
}

static NSString *ISHWorkspaceOrdinalName(NSUInteger index) {
    static NSArray<NSString *> *names;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        names = @[@"One", @"Two", @"Three", @"Four", @"Five", @"Six", @"Seven", @"Eight", @"Nine", @"Ten"];
    });
    if (index < names.count)
        return names[index];
    return [NSString stringWithFormat:@"%lu", (unsigned long) index + 1];
}

static NSArray<UISceneSession *> *ISHWorkspaceSortedSceneSessions(void) API_AVAILABLE(ios(13.0));
static NSArray<UISceneSession *> *ISHWorkspaceSortedSceneSessions(void) {
    NSMutableDictionary<NSString *, UISceneSession *> *sessionsByIdentifier = [NSMutableDictionary dictionary];
    for (UISceneSession *session in UIApplication.sharedApplication.openSessions) {
        NSString *identifier = session.persistentIdentifier ?: @"";
        if (identifier.length > 0)
            sessionsByIdentifier[identifier] = session;
    }
    for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
        UISceneSession *session = scene.session;
        NSString *identifier = session.persistentIdentifier ?: @"";
        if (identifier.length > 0)
            sessionsByIdentifier[identifier] = session;
    }
    return [sessionsByIdentifier.allValues sortedArrayUsingComparator:^NSComparisonResult(UISceneSession *left, UISceneSession *right) {
        NSString *leftIdentifier = left.persistentIdentifier ?: @"";
        NSString *rightIdentifier = right.persistentIdentifier ?: @"";
        return [leftIdentifier compare:rightIdentifier];
    }];
}

static UISceneSession *ISHWorkspaceSceneSessionWithPersistentIdentifier(NSString *identifier) API_AVAILABLE(ios(13.0));
static UISceneSession *ISHWorkspaceSceneSessionWithPersistentIdentifier(NSString *identifier) {
    if (identifier.length == 0)
        return nil;
    for (UISceneSession *session in ISHWorkspaceSortedSceneSessions()) {
        if ([session.persistentIdentifier isEqualToString:identifier])
            return session;
    }
    return nil;
}

static NSString *ISHWorkspaceNameForSession(UISceneSession *session) API_AVAILABLE(ios(13.0));
static NSString *ISHWorkspaceNameForSession(UISceneSession *session) {
    NSArray<UISceneSession *> *sessions = ISHWorkspaceSortedSceneSessions();
    NSUInteger index = [sessions indexOfObject:session];
    if (index == NSNotFound)
        index = 0;
    return ISHWorkspaceOrdinalName(index);
}

static UIScene *ISHWorkspaceConnectedSceneForSession(UISceneSession *session) API_AVAILABLE(ios(13.0));
static UIScene *ISHWorkspaceConnectedSceneForSession(UISceneSession *session) {
    for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
        if (scene.session == session)
            return scene;
    }
    return nil;
}

static NSMutableSet<NSString *> *ISHWorkspaceForgottenHiddenSessionIdentifiers(void) {
    NSArray<NSString *> *stored = [NSUserDefaults.standardUserDefaults arrayForKey:ISHWorkspaceForgottenHiddenSessionsDefaultsKey];
    return stored != nil ? [NSMutableSet setWithArray:stored] : [NSMutableSet set];
}

static BOOL ISHWorkspaceHiddenSessionIsForgotten(UISceneSession *session) API_AVAILABLE(ios(13.0));
static BOOL ISHWorkspaceHiddenSessionIsForgotten(UISceneSession *session) {
    NSString *identifier = session.persistentIdentifier;
    return identifier.length > 0 && [ISHWorkspaceForgottenHiddenSessionIdentifiers() containsObject:identifier];
}

static void ISHWorkspaceForgetHiddenSession(UISceneSession *session) API_AVAILABLE(ios(13.0));
static void ISHWorkspaceForgetHiddenSession(UISceneSession *session) {
    NSString *identifier = session.persistentIdentifier;
    if (identifier.length == 0)
        return;
    NSMutableSet<NSString *> *identifiers = ISHWorkspaceForgottenHiddenSessionIdentifiers();
    [identifiers addObject:identifier];
    [NSUserDefaults.standardUserDefaults setObject:identifiers.allObjects forKey:ISHWorkspaceForgottenHiddenSessionsDefaultsKey];
}

static NSString *ISHWorkspaceSceneActivationDescription(UIScene *scene) API_AVAILABLE(ios(13.0));
static NSString *ISHWorkspaceSceneActivationDescription(UIScene *scene) {
    switch (scene.activationState) {
        case UISceneActivationStateForegroundActive:
            return @"Foreground active";
        case UISceneActivationStateForegroundInactive:
            return @"Foreground inactive";
        case UISceneActivationStateBackground:
            return @"Background";
        case UISceneActivationStateUnattached:
            return @"Unattached";
    }
}

static NSArray<NSDictionary<NSString *, id> *> *ISHWorkspaceSceneDescriptors(UIWindowScene *currentWindowScene) API_AVAILABLE(ios(13.0));
static NSArray<NSDictionary<NSString *, id> *> *ISHWorkspaceSceneDescriptors(UIWindowScene *currentWindowScene) {
    NSArray<UISceneSession *> *sessions = ISHWorkspaceSortedSceneSessions();

    NSMutableArray<NSDictionary<NSString *, id> *> *descriptors = [NSMutableArray array];
    for (UISceneSession *session in sessions) {
        UIScene *scene = ISHWorkspaceConnectedSceneForSession(session);
        if (scene == nil && ISHWorkspaceHiddenSessionIsForgotten(session))
            continue;
        NSString *role = ISHWorkspaceSceneRoleDescriptionForScene(session, scene);
        NSString *state = scene != nil ? ISHWorkspaceSceneActivationDescription(scene) : @"Hidden";
        NSString *identifier = session.persistentIdentifier ?: @"";
        NSString *terminalUUID = session.stateRestorationActivity.userInfo[ISHSceneTerminalUUIDUserInfoKey];
        Terminal *terminal = terminalUUID.length > 0
            ? [Terminal terminalWithUUID:[[NSUUID alloc] initWithUUIDString:terminalUUID]]
            : nil;
        NSString *title = [NSString stringWithFormat:@"Workspace %@", ISHWorkspaceNameForSession(session)];
        NSString *detail = terminal != nil
            ? [NSString stringWithFormat:@"%@: %@", role, ISHWorkspaceTerminalDisplayName(terminal)]
            : [NSString stringWithFormat:@"%@: %@", role, state];
        [descriptors addObject:@{
            @"identifier": identifier,
            @"title": title,
            @"detail": detail ?: @"Unknown",
            @"role": role ?: @"Unknown",
            @"state": state ?: @"Unknown",
            @"isCurrent": @(session == currentWindowScene.session),
            @"isHidden": @(scene == nil),
        }];
    }
    return descriptors;
}

static NSString *ISHWorkspaceNetworkSummaryText(void) {
    struct ifaddrs *interfaces = NULL;
    if (getifaddrs(&interfaces) != 0 || interfaces == NULL)
        return @"Network: unavailable";

    NSMutableArray<NSString *> *lines = [NSMutableArray array];
    NSMutableArray<NSString *> *interfaceOrder = [NSMutableArray array];
    NSMutableDictionary<NSString *, NSMutableDictionary<NSString *, NSString *> *> *interfaceAddresses = [NSMutableDictionary dictionary];
    NSString *loopbackLine = nil;
    char addressBuffer[INET6_ADDRSTRLEN] = {0};

    for (struct ifaddrs *cursor = interfaces; cursor != NULL; cursor = cursor->ifa_next) {
        if (cursor->ifa_addr == NULL || cursor->ifa_name == NULL)
            continue;
        sa_family_t family = cursor->ifa_addr->sa_family;
        if (family != AF_INET && family != AF_INET6)
            continue;
        if ((cursor->ifa_flags & IFF_UP) == 0)
            continue;

        const void *source = family == AF_INET
            ? (const void *) &((const struct sockaddr_in *) cursor->ifa_addr)->sin_addr
            : (const void *) &((const struct sockaddr_in6 *) cursor->ifa_addr)->sin6_addr;
        if (inet_ntop(family, source, addressBuffer, sizeof(addressBuffer)) == NULL)
            continue;

        if (family == AF_INET6 && strncmp(addressBuffer, "fe80", 4) == 0)
            continue;  // IPv6 link-local (fe80::) is noise; humans want IPv4
        NSString *interfaceName = [NSString stringWithUTF8String:cursor->ifa_name];
        NSString *address = [NSString stringWithUTF8String:addressBuffer];
        BOOL isLoopback = (cursor->ifa_flags & IFF_LOOPBACK) != 0;
        if (isLoopback) {
            // Prefer the IPv4 loopback address (127.0.0.1) over ::1.
            if (loopbackLine == nil || family == AF_INET)
                loopbackLine = [NSString stringWithFormat:@"Loopback: %@ (%@)", interfaceName, address];
            continue;
        }

        NSMutableDictionary<NSString *, NSString *> *addresses = interfaceAddresses[interfaceName];
        if (addresses == nil) {
            addresses = [NSMutableDictionary dictionary];
            interfaceAddresses[interfaceName] = addresses;
            [interfaceOrder addObject:interfaceName];
        }
        NSString *familyKey = family == AF_INET ? @"ipv4" : @"ipv6";
        if (addresses[familyKey] == nil)
            addresses[familyKey] = address;
    }

    freeifaddrs(interfaces);

    NSUInteger activeInterfaces = 0;
    for (NSString *interfaceName in interfaceOrder) {
        NSDictionary<NSString *, NSString *> *addresses = interfaceAddresses[interfaceName];
        NSString *ipv4 = addresses[@"ipv4"];
        NSString *ipv6 = addresses[@"ipv6"];
        NSString *address = ipv4 ?: ipv6;
        if (address.length == 0)
            continue;
        NSString *familyName = ipv4.length > 0 ? @"IPv4" : @"IPv6";
        [lines addObject:[NSString stringWithFormat:@"%@: %@  %@", interfaceName, familyName, address]];
        activeInterfaces += 1;
        if (activeInterfaces >= 3)
            break;
    }

    if (loopbackLine != nil)
        [lines addObject:loopbackLine];

    if (activeInterfaces == 0 && loopbackLine == nil)
        return @"Network: no active interfaces";

    return [NSString stringWithFormat:@"Active interfaces: %lu\n%@",
                                      (unsigned long) activeInterfaces,
                                      [lines componentsJoinedByString:@"\n"]];
}

static NSString *ISHWorkspaceBatterySummaryText(void) {
    if (UIDevice.currentDevice.batteryState == UIDeviceBatteryStateUnknown || UIDevice.currentDevice.batteryLevel < 0)
        return @"unavailable";

    NSString *stateDescription = @"On battery";
    switch (UIDevice.currentDevice.batteryState) {
        case UIDeviceBatteryStateCharging:
            stateDescription = @"Charging";
            break;
        case UIDeviceBatteryStateFull:
            stateDescription = @"Fully charged";
            break;
        case UIDeviceBatteryStateUnplugged:
            stateDescription = @"On battery";
            break;
        case UIDeviceBatteryStateUnknown:
            break;
    }
    NSInteger percent = (NSInteger) llround(UIDevice.currentDevice.batteryLevel * 100.0);
    return [NSString stringWithFormat:@"%@ (%ld%%)", stateDescription, (long) percent];
}

static NSString *ISHWorkspaceStorageSummaryText(void) {
    NSDictionary<NSFileAttributeKey, id> *attributes =
        [NSFileManager.defaultManager attributesOfFileSystemForPath:NSHomeDirectory() error:nil];
    NSNumber *freeSize = attributes[NSFileSystemFreeSize];
    if (freeSize == nil)
        return @"Free storage: unavailable";
    NSString *formattedSize = [NSByteCountFormatter stringFromByteCount:freeSize.longLongValue
                                                              countStyle:NSByteCountFormatterCountStyleFile];
    return [NSString stringWithFormat:@"Free storage: %@", formattedSize];
}

static NSString *ISHWorkspacePrimaryNetworkLine(void) {
    NSArray<NSString *> *lines = [ISHWorkspaceNetworkSummaryText() componentsSeparatedByString:@"\n"];
    if (lines.count >= 2)
        return lines[1];
    return lines.firstObject ?: @"Network: unavailable";
}

static NSString *ISHWorkspaceByteCountString(uint64_t bytes) {
    return [NSByteCountFormatter stringFromByteCount:(long long) bytes
                                          countStyle:NSByteCountFormatterCountStyleFile];
}

static NSDictionary<NSString *, NSNumber *> *ISHWorkspaceDirectoryUsage(NSURL *directoryURL) {
    if (directoryURL == nil)
        return @{@"bytes": @0, @"files": @0, @"directories": @0};

    uint64_t totalBytes = 0;
    NSUInteger fileCount = 0;
    NSUInteger directoryCount = 0;
    NSArray<NSURLResourceKey> *keys = @[
        NSURLIsDirectoryKey,
        NSURLFileAllocatedSizeKey,
        NSURLTotalFileAllocatedSizeKey,
        NSURLFileSizeKey,
    ];
    NSDirectoryEnumerator<NSURL *> *enumerator =
        [NSFileManager.defaultManager enumeratorAtURL:directoryURL
                           includingPropertiesForKeys:keys
                                              options:0
                                         errorHandler:^BOOL(__unused NSURL *url, __unused NSError *error) {
        return YES;
    }];
    for (NSURL *itemURL in enumerator) {
        NSNumber *isDirectory = nil;
        [itemURL getResourceValue:&isDirectory forKey:NSURLIsDirectoryKey error:nil];
        if (isDirectory.boolValue) {
            directoryCount += 1;
            continue;
        }
        NSNumber *size = nil;
        [itemURL getResourceValue:&size forKey:NSURLTotalFileAllocatedSizeKey error:nil];
        if (size == nil)
            [itemURL getResourceValue:&size forKey:NSURLFileAllocatedSizeKey error:nil];
        if (size == nil)
            [itemURL getResourceValue:&size forKey:NSURLFileSizeKey error:nil];
        totalBytes += size.unsignedLongLongValue;
        fileCount += 1;
    }
    return @{
        @"bytes": @(totalBytes),
        @"files": @(fileCount),
        @"directories": @(directoryCount),
    };
}

static NSArray<NSDictionary<NSString *, id> *> *ISHWorkspaceRootUsageRecords(void) {
    NSMutableArray<NSDictionary<NSString *, id> *> *records = [NSMutableArray array];
    NSString *defaultRoot = Roots.instance.defaultRoot;
    for (NSString *rootName in Roots.instance.roots) {
        NSURL *rootURL = [Roots.instance rootUrl:rootName];
        NSDictionary<NSString *, NSNumber *> *usage = ISHWorkspaceDirectoryUsage(rootURL);
        NSString *guestABI = [Roots.instance guestABIForRootNamed:rootName] ?: @"Unknown ABI";
        [records addObject:@{
            @"name": rootName,
            @"isDefault": @([rootName isEqualToString:defaultRoot]),
            @"abi": guestABI,
            @"path": rootURL.path ?: @"",
            @"bytes": usage[@"bytes"] ?: @0,
            @"files": usage[@"files"] ?: @0,
            @"directories": usage[@"directories"] ?: @0,
        }];
    }
    return records;
}

static NSString *ISHWorkspaceUsageBarString(double ratio, NSUInteger width) {
    double clampedRatio = MAX(0.0, MIN(1.0, ratio));
    NSUInteger filled = (NSUInteger) llround(clampedRatio * (double) width);
    filled = MIN(width, filled);
    return [NSString stringWithFormat:@"[%@%@]",
                                      [@"" stringByPaddingToLength:filled withString:@"#" startingAtIndex:0],
                                      [@"" stringByPaddingToLength:(width - filled) withString:@"-" startingAtIndex:0]];
}

static NSString *ISHWorkspaceDurationString(NSTimeInterval interval) {
    NSInteger totalSeconds = MAX(0, (NSInteger) llround(interval));
    NSInteger days = totalSeconds / 86400;
    NSInteger hours = (totalSeconds % 86400) / 3600;
    NSInteger minutes = (totalSeconds % 3600) / 60;
    NSInteger seconds = totalSeconds % 60;
    if (days > 0)
        return [NSString stringWithFormat:@"%ldd %02ldh %02ldm", (long) days, (long) hours, (long) minutes];
    if (hours > 0)
        return [NSString stringWithFormat:@"%ldh %02ldm %02lds", (long) hours, (long) minutes, (long) seconds];
    return [NSString stringWithFormat:@"%ldm %02lds", (long) minutes, (long) seconds];
}

static BOOL ISHWorkspaceMemoryUsage(uint64_t *footprint, uint64_t *resident, uint64_t *physical) {
    if (footprint != NULL)
        *footprint = 0;
    if (resident != NULL)
        *resident = 0;
    if (physical != NULL)
        *physical = NSProcessInfo.processInfo.physicalMemory;

    task_vm_info_data_t vmInfo;
    mach_msg_type_number_t vmInfoCount = TASK_VM_INFO_COUNT;
    kern_return_t result = task_info(mach_task_self(), TASK_VM_INFO, (task_info_t) &vmInfo, &vmInfoCount);
    if (result == KERN_SUCCESS) {
        if (footprint != NULL)
            *footprint = vmInfo.phys_footprint;
        if (resident != NULL)
            *resident = vmInfo.resident_size;
        return YES;
    }

    task_basic_info_data_t basicInfo;
    mach_msg_type_number_t basicInfoCount = TASK_BASIC_INFO_COUNT;
    result = task_info(mach_task_self(), TASK_BASIC_INFO, (task_info_t) &basicInfo, &basicInfoCount);
    if (result == KERN_SUCCESS) {
        if (footprint != NULL)
            *footprint = basicInfo.resident_size;
        if (resident != NULL)
            *resident = basicInfo.resident_size;
        return YES;
    }
    return NO;
}

static uint64_t ISHWorkspacePhysicalMemoryBytes(void) {
    return NSProcessInfo.processInfo.physicalMemory;
}

static NSUInteger ISHWorkspacePhysicalMemoryMarketedGB(void) {
    const uint64_t oneGB = 1000ull * 1000ull * 1000ull;
    return (NSUInteger) ((ISHWorkspacePhysicalMemoryBytes() + oneGB - 1) / oneGB);
}

static BOOL ISHWorkspaceDeviceHasLowMemoryForWorkspace(void) {
    return ISHWorkspacePhysicalMemoryMarketedGB() < 4;
}

static NSUInteger ISHWorkspaceBrowserMaximumTabCount(void) {
    NSUInteger marketedGB = ISHWorkspacePhysicalMemoryMarketedGB();
    if (marketedGB < 4)
        return 2;
    return 3 + ((marketedGB - 4) / 2);
}

static NSString *ISHWorkspaceSystemStatusText(void) {
    NSMutableArray<NSString *> *lines = [NSMutableArray array];
    NSString *version = [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"] ?: @"?";
    NSString *build = [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleVersion"] ?: @"?";
    [lines addObject:[NSString stringWithFormat:@"App: %@ (%@)", version, build]];
    [lines addObject:[NSString stringWithFormat:@"Device: %@ / iOS %@",
                      UIDevice.currentDevice.model ?: @"Unknown",
                      UIDevice.currentDevice.systemVersion ?: @"?"]];
    NSString *defaultRoot = Roots.instance.defaultRoot;
    [lines addObject:[NSString stringWithFormat:@"Current root: %@",
                      defaultRoot.length > 0 ? defaultRoot : @"unavailable"]];
    [lines addObject:ISHWorkspaceStorageSummaryText()];
    [lines addObject:[NSString stringWithFormat:@"Startup screen: %@", ISHInitialWindowTitle()]];
    [lines addObject:[NSString stringWithFormat:@"Installed roots: %lu",
                      (unsigned long) Roots.instance.roots.count]];
    [lines addObject:[NSString stringWithFormat:@"Active terminals: %lu",
                      (unsigned long) Terminal.activeTerminals.count]];
    if (@available(iOS 13.0, *)) {
        [lines addObject:[NSString stringWithFormat:@"Open scenes: %lu",
                          (unsigned long) UIApplication.sharedApplication.connectedScenes.count]];
    }
    [lines addObject:@""];
    [lines addObject:ISHWorkspaceNetworkSummaryText()];

    NSArray<NSDictionary<NSString *, id> *> *breadcrumbs = [ISHDiagnosticsStore recentBreadcrumbsWithLimit:5];
    if (breadcrumbs.count > 0) {
        [lines addObject:@""];
        [lines addObject:@"Recent events:"];
        for (NSDictionary<NSString *, id> *entry in breadcrumbs) {
            NSString *event = entry[@"event"] ?: @"event";
            NSString *timestamp = entry[@"timestamp"] ?: @"";
            [lines addObject:[NSString stringWithFormat:@"%@  %@", timestamp, event]];
        }
    }
    return [lines componentsJoinedByString:@"\n"];
}

static NSString *const ISHWorkspaceToolThemePreferenceKey = @"ISHWorkspaceToolTheme";
static NSString *const ISHWorkspaceCustomThemesDefaultsKey = @"ISHWorkspaceCustomThemes";
static NSString *const ISHWorkspaceToolDensityPreferenceKey = @"ISHWorkspaceToolDensity";
static NSString *const ISHWorkspaceGaugeStylePreferenceKey = @"ISHWorkspaceGaugeStyle";
static NSString *const ISHWorkspaceBrowserHomePreferenceKey = @"ISHWorkspaceBrowserHome";
static NSString *const ISHWorkspaceBrowserBookmarksDefaultsKey = @"ISHWorkspaceBrowserBookmarks";
static NSString *const ISHWorkspaceStartupLowMemoryWarningDisabledPreferenceKey = @"ISHWorkspaceStartupLowMemoryWarningDisabled";
static NSString *const ISHWorkspaceToolThemeDidChangeNotification = @"ISHWorkspaceToolThemeDidChange";
static NSString *const ISHWorkspaceGaugeStyleDidChangeNotification = @"ISHWorkspaceGaugeStyleDidChange";
static NSString *const ISHWorkspaceToolThemeAuroraIdentifier = @"aurora";
static NSString *const ISHWorkspaceToolThemeSolsticeIdentifier = @"solstice";
static NSString *const ISHWorkspaceToolThemeGraphiteIdentifier = @"graphite";
static BOOL ISHWorkspaceLowMemoryWarningShownThisLaunch = NO;
static const uint64_t ISHWorkspaceOneGB = 1000ull * 1000ull * 1000ull;

static UIColor *ISHWorkspaceThemeColor(CGFloat red, CGFloat green, CGFloat blue, CGFloat alpha) {
    return [UIColor colorWithRed:red / 255.0
                           green:green / 255.0
                            blue:blue / 255.0
                           alpha:alpha];
}

static NSDictionary<NSString *, NSNumber *> *ISHWorkspaceThemeColorDescriptor(CGFloat red, CGFloat green, CGFloat blue) {
    return @{
        @"red": @(red),
        @"green": @(green),
        @"blue": @(blue),
    };
}

static UIColor *ISHWorkspaceThemeColorFromDescriptor(NSDictionary<NSString *, NSNumber *> *descriptor) {
    if (![descriptor isKindOfClass:NSDictionary.class])
        return UIColor.blackColor;
    return ISHWorkspaceThemeColor([descriptor[@"red"] doubleValue],
                                  [descriptor[@"green"] doubleValue],
                                  [descriptor[@"blue"] doubleValue],
                                  1.0);
}

static NSDictionary<NSString *, NSNumber *> *ISHWorkspaceThemeColorDescriptorFromUIColor(UIColor *color) {
    CGFloat red = 0;
    CGFloat green = 0;
    CGFloat blue = 0;
    CGFloat alpha = 0;
    if (![color getRed:&red green:&green blue:&blue alpha:&alpha]) {
        CGFloat white = 0;
        if ([color getWhite:&white alpha:&alpha]) {
            red = white;
            green = white;
            blue = white;
        }
    }
    return @{
        @"red": @(llround(red * 255.0)),
        @"green": @(llround(green * 255.0)),
        @"blue": @(llround(blue * 255.0)),
    };
}

static BOOL ISHWorkspaceThemeRGBAComponents(UIColor *color,
                                            CGFloat *red,
                                            CGFloat *green,
                                            CGFloat *blue,
                                            CGFloat *alpha) {
    CGFloat localRed = 0;
    CGFloat localGreen = 0;
    CGFloat localBlue = 0;
    CGFloat localAlpha = 0;
    if ([color getRed:&localRed green:&localGreen blue:&localBlue alpha:&localAlpha]) {
        if (red != NULL)
            *red = localRed;
        if (green != NULL)
            *green = localGreen;
        if (blue != NULL)
            *blue = localBlue;
        if (alpha != NULL)
            *alpha = localAlpha;
        return YES;
    }

    CGFloat white = 0;
    if ([color getWhite:&white alpha:&localAlpha]) {
        if (red != NULL)
            *red = white;
        if (green != NULL)
            *green = white;
        if (blue != NULL)
            *blue = white;
        if (alpha != NULL)
            *alpha = localAlpha;
        return YES;
    }
    return NO;
}

static CGFloat ISHWorkspaceThemeLinearizedComponent(CGFloat component) {
    if (component <= 0.04045)
        return component / 12.92;
    return pow((component + 0.055) / 1.055, 2.4);
}

static CGFloat ISHWorkspaceThemeRelativeLuminance(UIColor *color) {
    CGFloat red = 0;
    CGFloat green = 0;
    CGFloat blue = 0;
    if (!ISHWorkspaceThemeRGBAComponents(color, &red, &green, &blue, NULL))
        return 0;
    return (0.2126 * ISHWorkspaceThemeLinearizedComponent(red)) +
           (0.7152 * ISHWorkspaceThemeLinearizedComponent(green)) +
           (0.0722 * ISHWorkspaceThemeLinearizedComponent(blue));
}

static CGFloat ISHWorkspaceThemeContrastRatio(UIColor *first, UIColor *second) {
    CGFloat luminanceA = ISHWorkspaceThemeRelativeLuminance(first);
    CGFloat luminanceB = ISHWorkspaceThemeRelativeLuminance(second);
    CGFloat lighter = MAX(luminanceA, luminanceB);
    CGFloat darker = MIN(luminanceA, luminanceB);
    return (lighter + 0.05) / (darker + 0.05);
}

static UIColor *ISHWorkspaceThemeBlendColors(UIColor *foreground, UIColor *background) {
    CGFloat foreRed = 0;
    CGFloat foreGreen = 0;
    CGFloat foreBlue = 0;
    CGFloat foreAlpha = 0;
    CGFloat backRed = 0;
    CGFloat backGreen = 0;
    CGFloat backBlue = 0;
    CGFloat backAlpha = 0;
    if (!ISHWorkspaceThemeRGBAComponents(foreground, &foreRed, &foreGreen, &foreBlue, &foreAlpha))
        return foreground ?: background ?: UIColor.blackColor;
    if (!ISHWorkspaceThemeRGBAComponents(background, &backRed, &backGreen, &backBlue, &backAlpha))
        return foreground;

    CGFloat outputAlpha = foreAlpha + (backAlpha * (1.0 - foreAlpha));
    if (outputAlpha <= 0.0001)
        return UIColor.clearColor;
    CGFloat red = ((foreRed * foreAlpha) + (backRed * backAlpha * (1.0 - foreAlpha))) / outputAlpha;
    CGFloat green = ((foreGreen * foreAlpha) + (backGreen * backAlpha * (1.0 - foreAlpha))) / outputAlpha;
    CGFloat blue = ((foreBlue * foreAlpha) + (backBlue * backAlpha * (1.0 - foreAlpha))) / outputAlpha;
    return [UIColor colorWithRed:red green:green blue:blue alpha:1.0];
}

static UIColor *ISHWorkspaceThemeAverageColor(UIColor *first, UIColor *second) {
    CGFloat firstRed = 0;
    CGFloat firstGreen = 0;
    CGFloat firstBlue = 0;
    CGFloat secondRed = 0;
    CGFloat secondGreen = 0;
    CGFloat secondBlue = 0;
    if (!ISHWorkspaceThemeRGBAComponents(first, &firstRed, &firstGreen, &firstBlue, NULL))
        return second ?: UIColor.blackColor;
    if (!ISHWorkspaceThemeRGBAComponents(second, &secondRed, &secondGreen, &secondBlue, NULL))
        return first;
    return [UIColor colorWithRed:((firstRed + secondRed) * 0.5)
                           green:((firstGreen + secondGreen) * 0.5)
                            blue:((firstBlue + secondBlue) * 0.5)
                           alpha:1.0];
}

static UIColor *ISHWorkspaceThemeColorAdjustedForContrast(UIColor *color, UIColor *background, CGFloat minimumContrast) {
    if (ISHWorkspaceThemeContrastRatio(color, background) >= minimumContrast)
        return color;

    UIColor *darkCandidate = UIColor.blackColor;
    UIColor *lightCandidate = UIColor.whiteColor;
    UIColor *target = ISHWorkspaceThemeContrastRatio(darkCandidate, background) >=
                      ISHWorkspaceThemeContrastRatio(lightCandidate, background)
        ? darkCandidate
        : lightCandidate;

    CGFloat colorRed = 0;
    CGFloat colorGreen = 0;
    CGFloat colorBlue = 0;
    CGFloat targetRed = 0;
    CGFloat targetGreen = 0;
    CGFloat targetBlue = 0;
    if (!ISHWorkspaceThemeRGBAComponents(color, &colorRed, &colorGreen, &colorBlue, NULL) ||
        !ISHWorkspaceThemeRGBAComponents(target, &targetRed, &targetGreen, &targetBlue, NULL)) {
        return target;
    }

    UIColor *best = target;
    for (NSInteger step = 1; step <= 24; step++) {
        CGFloat amount = (CGFloat) step / 24.0;
        UIColor *candidate = [UIColor colorWithRed:(colorRed + ((targetRed - colorRed) * amount))
                                             green:(colorGreen + ((targetGreen - colorGreen) * amount))
                                              blue:(colorBlue + ((targetBlue - colorBlue) * amount))
                                             alpha:1.0];
        if (ISHWorkspaceThemeContrastRatio(candidate, background) >= minimumContrast) {
            best = candidate;
            break;
        }
    }
    return best;
}

// The focus ring (active-window border / resize handle) is drawn on the workspace
// background gradient, not on a card, so it must contrast with the background — reusing
// the card-tuned accent makes it vanish on same-hue backgrounds (e.g. teal accent on a
// teal gradient). Bias the adjustment toward white on dark backgrounds and black on light
// ones so a focused window reads as a deliberate glow/outline. Uses the WCAG 1.4.11
// minimum (3:1) for UI-component boundaries. Keeps any accent that already passes, so
// well-chosen themes (and custom ones from the editor) are left untouched.
static UIColor *ISHWorkspaceThemeFocusRingColor(UIColor *accent, UIColor *background) {
    static const CGFloat minimumContrast = 3.0;
    if (ISHWorkspaceThemeContrastRatio(accent, background) >= minimumContrast)
        return accent;

    UIColor *target = ISHWorkspaceThemeRelativeLuminance(background) < 0.45
        ? UIColor.whiteColor
        : UIColor.blackColor;
    if (ISHWorkspaceThemeContrastRatio(target, background) < minimumContrast) {
        // Mid-tone background: neither pole clears the bar comfortably, so take the better one.
        target = ISHWorkspaceThemeContrastRatio(UIColor.blackColor, background) >=
                 ISHWorkspaceThemeContrastRatio(UIColor.whiteColor, background)
            ? UIColor.blackColor
            : UIColor.whiteColor;
    }

    CGFloat accentRed = 0;
    CGFloat accentGreen = 0;
    CGFloat accentBlue = 0;
    CGFloat targetRed = 0;
    CGFloat targetGreen = 0;
    CGFloat targetBlue = 0;
    if (!ISHWorkspaceThemeRGBAComponents(accent, &accentRed, &accentGreen, &accentBlue, NULL) ||
        !ISHWorkspaceThemeRGBAComponents(target, &targetRed, &targetGreen, &targetBlue, NULL)) {
        return target;
    }

    UIColor *best = target;
    for (NSInteger step = 1; step <= 24; step++) {
        CGFloat amount = (CGFloat) step / 24.0;
        UIColor *candidate = [UIColor colorWithRed:(accentRed + ((targetRed - accentRed) * amount))
                                             green:(accentGreen + ((targetGreen - accentGreen) * amount))
                                              blue:(accentBlue + ((targetBlue - accentBlue) * amount))
                                             alpha:1.0];
        if (ISHWorkspaceThemeContrastRatio(candidate, background) >= minimumContrast) {
            best = candidate;
            break;
        }
    }
    return best;
}

static NSArray<NSString *> *ISHWorkspaceThemeEditableColorKeys(void) {
    return @[@"backgroundTop", @"backgroundBottom", @"card", @"primary", @"secondary", @"accent", @"accentAlt"];
}

static NSArray<NSDictionary<NSString *, NSString *> *> *ISHWorkspaceBuiltInThemeChoices(void) {
    return @[
        @{@"identifier": ISHWorkspaceToolThemeAuroraIdentifier, @"title": @"Aurora"},
        @{@"identifier": ISHWorkspaceToolThemeSolsticeIdentifier, @"title": @"Solstice"},
        @{@"identifier": ISHWorkspaceToolThemeGraphiteIdentifier, @"title": @"Graphite"},
    ];
}

static NSDictionary<NSString *, NSDictionary<NSString *, NSNumber *> *> *ISHWorkspaceBuiltInThemePalette(NSString *identifier) {
    if ([identifier isEqualToString:ISHWorkspaceToolThemeSolsticeIdentifier]) {
        return @{
            @"backgroundTop": ISHWorkspaceThemeColorDescriptor(255, 240, 218),
            @"backgroundBottom": ISHWorkspaceThemeColorDescriptor(255, 174, 111),
            @"card": ISHWorkspaceThemeColorDescriptor(255, 250, 242),
            @"primary": ISHWorkspaceThemeColorDescriptor(71, 39, 24),
            @"secondary": ISHWorkspaceThemeColorDescriptor(108, 73, 49),
            @"accent": ISHWorkspaceThemeColorDescriptor(170, 70, 30),
            @"accentAlt": ISHWorkspaceThemeColorDescriptor(128, 87, 16),
        };
    }
    if ([identifier isEqualToString:ISHWorkspaceToolThemeGraphiteIdentifier]) {
        return @{
            @"backgroundTop": ISHWorkspaceThemeColorDescriptor(15, 20, 32),
            @"backgroundBottom": ISHWorkspaceThemeColorDescriptor(26, 34, 50),
            @"card": ISHWorkspaceThemeColorDescriptor(44, 55, 76),
            @"primary": ISHWorkspaceThemeColorDescriptor(240, 244, 255),
            @"secondary": ISHWorkspaceThemeColorDescriptor(182, 194, 218),
            @"accent": ISHWorkspaceThemeColorDescriptor(112, 232, 204),
            @"accentAlt": ISHWorkspaceThemeColorDescriptor(138, 176, 255),
        };
    }
    return @{
        @"backgroundTop": ISHWorkspaceThemeColorDescriptor(10, 26, 58),
        @"backgroundBottom": ISHWorkspaceThemeColorDescriptor(22, 74, 104),
        @"card": ISHWorkspaceThemeColorDescriptor(240, 251, 255),
        @"primary": ISHWorkspaceThemeColorDescriptor(13, 38, 62),
        @"secondary": ISHWorkspaceThemeColorDescriptor(52, 80, 110),
        @"accent": ISHWorkspaceThemeColorDescriptor(0, 118, 156),
        @"accentAlt": ISHWorkspaceThemeColorDescriptor(20, 128, 120),
    };
}

static NSArray<NSDictionary<NSString *, id> *> *ISHWorkspaceCustomThemeRecords(void) {
    NSArray *records = [NSUserDefaults.standardUserDefaults arrayForKey:ISHWorkspaceCustomThemesDefaultsKey];
    return [records isKindOfClass:NSArray.class] ? records : @[];
}

static NSDictionary<NSString *, id> *ISHWorkspaceThemeRecordForIdentifier(NSString *identifier) {
    for (NSDictionary<NSString *, NSString *> *choice in ISHWorkspaceBuiltInThemeChoices()) {
        if ([choice[@"identifier"] isEqualToString:identifier])
            return @{
                @"identifier": choice[@"identifier"],
                @"title": choice[@"title"],
                @"palette": ISHWorkspaceBuiltInThemePalette(identifier),
                @"builtIn": @YES,
            };
    }
    for (NSDictionary<NSString *, id> *record in ISHWorkspaceCustomThemeRecords()) {
        if ([record[@"identifier"] isEqualToString:identifier])
            return record;
    }
    return nil;
}

static NSArray<NSDictionary<NSString *, id> *> *ISHWorkspaceThemeChoices(void) {
    NSMutableArray<NSDictionary<NSString *, id> *> *choices = [NSMutableArray array];
    for (NSDictionary<NSString *, NSString *> *choice in ISHWorkspaceBuiltInThemeChoices()) {
        [choices addObject:@{
            @"identifier": choice[@"identifier"],
            @"title": choice[@"title"],
            @"palette": ISHWorkspaceBuiltInThemePalette(choice[@"identifier"]),
            @"builtIn": @YES,
        }];
    }
    [choices addObjectsFromArray:ISHWorkspaceCustomThemeRecords()];
    return choices;
}

static BOOL ISHWorkspaceThemeIdentifierIsValid(NSString *identifier) {
    return ISHWorkspaceThemeRecordForIdentifier(identifier) != nil;
}

static NSString *ISHWorkspaceCurrentThemeIdentifier(void) {
    NSString *identifier = [NSUserDefaults.standardUserDefaults stringForKey:ISHWorkspaceToolThemePreferenceKey];
    if (!ISHWorkspaceThemeIdentifierIsValid(identifier))
        return ISHWorkspaceToolThemeAuroraIdentifier;
    return identifier;
}

static NSString *ISHWorkspaceCurrentThemeTitle(void) {
    NSDictionary<NSString *, id> *record = ISHWorkspaceThemeRecordForIdentifier(ISHWorkspaceCurrentThemeIdentifier());
    return record[@"title"] ?: @"Aurora";
}

static NSDictionary<NSString *, NSDictionary<NSString *, NSNumber *> *> *ISHWorkspaceThemeEditablePaletteForIdentifier(NSString *identifier) {
    NSDictionary<NSString *, id> *record = ISHWorkspaceThemeRecordForIdentifier(identifier);
    NSDictionary<NSString *, NSDictionary<NSString *, NSNumber *> *> *palette = record[@"palette"];
    if (![palette isKindOfClass:NSDictionary.class])
        return ISHWorkspaceBuiltInThemePalette(ISHWorkspaceToolThemeAuroraIdentifier);
    return palette;
}

static NSDictionary<NSString *, UIColor *> *ISHWorkspaceThemeDescriptorFromEditablePalette(NSDictionary<NSString *, NSDictionary<NSString *, NSNumber *> *> *palette) {
    UIColor *backgroundTop = ISHWorkspaceThemeColorFromDescriptor(palette[@"backgroundTop"]);
    UIColor *backgroundBottom = ISHWorkspaceThemeColorFromDescriptor(palette[@"backgroundBottom"]);
    UIColor *cardBase = ISHWorkspaceThemeColorFromDescriptor(palette[@"card"]);
    UIColor *surfaceBackdrop = ISHWorkspaceThemeAverageColor(backgroundTop, backgroundBottom);
    UIColor *card = ISHWorkspaceThemeBlendColors([cardBase colorWithAlphaComponent:0.97], surfaceBackdrop);
    UIColor *cardAlt = ISHWorkspaceThemeBlendColors([cardBase colorWithAlphaComponent:0.94], surfaceBackdrop);
    UIColor *contrastSurface = ISHWorkspaceThemeAverageColor(card, cardAlt);
    UIColor *primary = ISHWorkspaceThemeColorAdjustedForContrast(
        ISHWorkspaceThemeColorFromDescriptor(palette[@"primary"]), contrastSurface, 7.0);
    UIColor *secondary = ISHWorkspaceThemeColorAdjustedForContrast(
        ISHWorkspaceThemeColorFromDescriptor(palette[@"secondary"]), contrastSurface, 4.9);
    UIColor *accent = ISHWorkspaceThemeColorAdjustedForContrast(
        ISHWorkspaceThemeColorFromDescriptor(palette[@"accent"]), contrastSurface, 4.8);
    UIColor *accentAlt = ISHWorkspaceThemeColorAdjustedForContrast(
        ISHWorkspaceThemeColorFromDescriptor(palette[@"accentAlt"]), contrastSurface, 4.8);
    // Focus ring is anchored to the background (where it's actually drawn), not the card.
    UIColor *focusRing = ISHWorkspaceThemeFocusRingColor(
        ISHWorkspaceThemeColorFromDescriptor(palette[@"accent"]), backgroundBottom);
    return @{
        @"backgroundTop": backgroundTop,
        @"backgroundBottom": backgroundBottom,
        @"card": card,
        @"cardAlt": cardAlt,
        @"primary": primary,
        @"secondary": secondary,
        @"accent": accent,
        @"accentAlt": accentAlt,
        @"focusRing": focusRing,
        @"stroke": [focusRing colorWithAlphaComponent:0.28],
    };
}

static NSDictionary<NSString *, UIColor *> *ISHWorkspaceThemeDescriptor(void) {
    return ISHWorkspaceThemeDescriptorFromEditablePalette(
        ISHWorkspaceThemeEditablePaletteForIdentifier(ISHWorkspaceCurrentThemeIdentifier()));
}

static CGFloat ISHWorkspaceCurrentDensity(void) {
    id value = [NSUserDefaults.standardUserDefaults objectForKey:ISHWorkspaceToolDensityPreferenceKey];
    if (![value isKindOfClass:NSNumber.class])
        return 0.0;
    return MAX(0.0, MIN(1.0, [value doubleValue]));
}

// Workspace-wide gauge style for the status applets. Defaults to ring gauges.
static BOOL ISHWorkspaceUsesRingGauges(void) {
    id value = [NSUserDefaults.standardUserDefaults objectForKey:ISHWorkspaceGaugeStylePreferenceKey];
    if (![value isKindOfClass:NSString.class])
        return YES;
    return ![value isEqualToString:@"bar"];
}

static void ISHWorkspaceSetUsesRingGauges(BOOL usesRings) {
    [NSUserDefaults.standardUserDefaults setObject:(usesRings ? @"ring" : @"bar")
                                            forKey:ISHWorkspaceGaugeStylePreferenceKey];
    [NSNotificationCenter.defaultCenter postNotificationName:ISHWorkspaceGaugeStyleDidChangeNotification object:nil];
}

// The Monitor applet stacks two gauge rows (CPU/Memory, Battery/Storage) over a details card. Ring
// gauges are much taller than bars, so size the window to the ACTIVE gauge style — a window sized
// for bars clips the bottom row of dials when rings are selected (the reported bug), and one sized
// for rings would leave dead space under bars. Returns the window frame height (incl. title bar).
static CGSize ISHWorkspaceMonitorContentSize(void) {
    BOOL phone = ISHWorkspaceUsesPhoneLayout();
    BOOL rings = ISHWorkspaceUsesRingGauges();
    CGFloat width = phone ? 328.0 : 360.0;
    CGFloat gaugeHeight = rings ? (phone ? 42.0 : 54.0) : (phone ? 20.0 : 28.0);
    CGFloat tileInset = phone ? 5.0 : 7.0;
    // tile = vertical inset×2 + header row + stack spacing + gauge; floored at the card min height.
    CGFloat tileHeight = MAX(phone ? 50.0 : 64.0, tileInset * 2.0 + 20.0 + 5.0 + gaugeHeight);
    CGFloat detailsCardHeight = phone ? 112.0 : 124.0;
    // contentStack: 6pt top/bottom inset, 8pt between its three children (2 gauge rows + card).
    CGFloat contentHeight = 6.0 * 2.0 + tileHeight * 2.0 + 8.0 * 2.0 + detailsCardHeight;
    return CGSizeMake(width, contentHeight + ISHWorkspaceWindowTitleBarHeight);
}

static CGFloat ISHWorkspaceDensityValue(CGFloat compact, CGFloat roomy) {
    return compact + ((roomy - compact) * ISHWorkspaceCurrentDensity());
}

static CGFloat ISHWorkspaceThemeFontSize(UIFontTextStyle textStyle) {
    CGFloat size = 0;
    if ([textStyle isEqualToString:UIFontTextStyleCaption2])
        size = ISHWorkspaceDensityValue(8, 11);
    else if ([textStyle isEqualToString:UIFontTextStyleCaption1])
        size = ISHWorkspaceDensityValue(9, 12);
    else if ([textStyle isEqualToString:UIFontTextStyleFootnote])
        size = ISHWorkspaceDensityValue(10, 13);
    else if ([textStyle isEqualToString:UIFontTextStyleSubheadline])
        size = ISHWorkspaceDensityValue(11, 15);
    else if ([textStyle isEqualToString:UIFontTextStyleHeadline])
        size = ISHWorkspaceDensityValue(12, 17);
    else if ([textStyle isEqualToString:UIFontTextStyleBody])
        size = ISHWorkspaceDensityValue(12, 16);
    else if ([textStyle isEqualToString:UIFontTextStyleTitle3])
        size = ISHWorkspaceDensityValue(15, 21);
    else if ([textStyle isEqualToString:UIFontTextStyleTitle2])
        size = ISHWorkspaceDensityValue(17, 25);
    else if ([textStyle isEqualToString:UIFontTextStyleLargeTitle])
        size = ISHWorkspaceDensityValue(24, 34);
    else
        size = ISHWorkspaceDensityValue(12, 16);

    if (ISHWorkspaceUsesPhoneLayout())
        size *= 0.92;
    return size;
}

static void ISHWorkspaceSetCurrentDensity(CGFloat density) {
    CGFloat clamped = MAX(0.0, MIN(1.0, density));
    [NSUserDefaults.standardUserDefaults setDouble:clamped forKey:ISHWorkspaceToolDensityPreferenceKey];
    [NSNotificationCenter.defaultCenter postNotificationName:ISHWorkspaceToolThemeDidChangeNotification object:nil];
}

static NSString *ISHWorkspaceCurrentDensityTitle(void) {
    CGFloat density = ISHWorkspaceCurrentDensity();
    if (density < 0.18)
        return @"Ultra Compact";
    if (density < 0.42)
        return @"Compact";
    if (density < 0.72)
        return @"Balanced";
    return @"Comfortable";
}

static void ISHWorkspaceThemeDrawLinearGradient(CGContextRef context,
                                                CGRect rect,
                                                UIColor *startColor,
                                                UIColor *endColor) {
    CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
    NSArray *colors = @[(id) startColor.CGColor, (id) endColor.CGColor];
    CGGradientRef gradient = CGGradientCreateWithColors(colorSpace, (__bridge CFArrayRef) colors, NULL);
    CGPoint startPoint = CGPointMake(CGRectGetMidX(rect), CGRectGetMinY(rect));
    CGPoint endPoint = CGPointMake(CGRectGetMidX(rect), CGRectGetMaxY(rect));
    CGContextDrawLinearGradient(context, gradient, startPoint, endPoint, 0);
    CGGradientRelease(gradient);
    CGColorSpaceRelease(colorSpace);
}

static UIImage *ISHWorkspaceThemeArtworkImage(NSString *identifier,
                                              NSDictionary<NSString *, NSDictionary<NSString *, NSNumber *> *> *palette,
                                              CGSize size,
                                              BOOL wallpaper) {
    if (size.width < 1 || size.height < 1)
        return nil;

    NSDictionary<NSString *, UIColor *> *theme = ISHWorkspaceThemeDescriptorFromEditablePalette(palette);
    UIGraphicsImageRendererFormat *format = [UIGraphicsImageRendererFormat defaultFormat];
    format.opaque = YES;
    UIGraphicsImageRenderer *renderer = [[UIGraphicsImageRenderer alloc] initWithSize:size format:format];
    return [renderer imageWithActions:^(UIGraphicsImageRendererContext *rendererContext) {
        CGContextRef context = rendererContext.CGContext;
        CGRect rect = CGRectMake(0, 0, size.width, size.height);
        CGFloat width = CGRectGetWidth(rect);
        CGFloat height = CGRectGetHeight(rect);

        UIColor *backgroundTop = theme[@"backgroundTop"];
        UIColor *backgroundBottom = theme[@"backgroundBottom"];
        UIColor *card = [theme[@"card"] colorWithAlphaComponent:wallpaper ? 0.26 : 0.18];
        UIColor *accent = [theme[@"accent"] colorWithAlphaComponent:wallpaper ? 0.46 : 0.28];
        UIColor *accentAlt = [theme[@"accentAlt"] colorWithAlphaComponent:wallpaper ? 0.34 : 0.24];
        UIColor *secondary = [theme[@"secondary"] colorWithAlphaComponent:wallpaper ? 0.22 : 0.18];

        ISHWorkspaceThemeDrawLinearGradient(context, rect, backgroundTop, backgroundBottom);

        UIBezierPath *glowOrb = [UIBezierPath bezierPathWithOvalInRect:CGRectMake(width * 0.58, -height * 0.1,
                                                                                   width * 0.44, height * 0.46)];
        [accent setFill];
        [glowOrb fill];

        UIBezierPath *secondaryOrb = [UIBezierPath bezierPathWithOvalInRect:CGRectMake(-width * 0.16, height * 0.52,
                                                                                        width * 0.46, height * 0.38)];
        [accentAlt setFill];
        [secondaryOrb fill];

        if ([identifier isEqualToString:ISHWorkspaceToolThemeSolsticeIdentifier]) {
            UIBezierPath *sun = [UIBezierPath bezierPathWithOvalInRect:CGRectMake(width * 0.62, height * 0.12,
                                                                                  width * 0.18, width * 0.18)];
            [[theme[@"card"] colorWithAlphaComponent:wallpaper ? 0.82 : 0.9] setFill];
            [sun fill];

            NSArray<NSNumber *> *bandOrigins = @[@0.58, @0.68, @0.79];
            NSArray<UIColor *> *bandColors = @[
                [theme[@"accent"] colorWithAlphaComponent:0.34],
                [theme[@"accentAlt"] colorWithAlphaComponent:0.28],
                [theme[@"card"] colorWithAlphaComponent:0.22],
            ];
            for (NSUInteger index = 0; index < bandOrigins.count; index++) {
                CGFloat origin = bandOrigins[index].doubleValue * height;
                UIBezierPath *band = [UIBezierPath bezierPath];
                [band moveToPoint:CGPointMake(-width * 0.1, origin)];
                [band addCurveToPoint:CGPointMake(width * 1.1, origin - height * 0.04)
                        controlPoint1:CGPointMake(width * 0.26, origin - height * 0.08)
                        controlPoint2:CGPointMake(width * 0.72, origin + height * 0.02)];
                [band addLineToPoint:CGPointMake(width * 1.1, height * 1.1)];
                [band addLineToPoint:CGPointMake(-width * 0.1, height * 1.1)];
                [band closePath];
                [bandColors[index] setFill];
                [band fill];
            }
        } else if ([identifier isEqualToString:ISHWorkspaceToolThemeGraphiteIdentifier]) {
            CGContextSaveGState(context);
            CGContextSetLineWidth(context, MAX(1.0, MIN(width, height) * 0.006));
            CGContextSetStrokeColorWithColor(context, [secondary colorWithAlphaComponent:0.55].CGColor);
            CGFloat step = MAX(26.0, MIN(width, height) * 0.12);
            for (CGFloat x = -height; x <= width + height; x += step) {
                CGContextMoveToPoint(context, x, 0);
                CGContextAddLineToPoint(context, x + height * 0.4, height);
            }
            CGContextStrokePath(context);
            CGContextRestoreGState(context);

            UIBezierPath *panel = [UIBezierPath bezierPathWithRoundedRect:CGRectMake(width * 0.1, height * 0.16,
                                                                                     width * 0.48, height * 0.26)
                                                             cornerRadius:MIN(width, height) * 0.06];
            [[theme[@"card"] colorWithAlphaComponent:wallpaper ? 0.18 : 0.28] setFill];
            [panel fill];
        } else {
            UIBezierPath *ribbonA = [UIBezierPath bezierPath];
            [ribbonA moveToPoint:CGPointMake(-width * 0.1, height * 0.28)];
            [ribbonA addCurveToPoint:CGPointMake(width * 1.05, height * 0.12)
                       controlPoint1:CGPointMake(width * 0.22, height * 0.54)
                       controlPoint2:CGPointMake(width * 0.7, -height * 0.02)];
            [ribbonA addLineToPoint:CGPointMake(width * 1.05, height * 0.3)];
            [ribbonA addCurveToPoint:CGPointMake(-width * 0.1, height * 0.46)
                       controlPoint1:CGPointMake(width * 0.74, height * 0.5)
                       controlPoint2:CGPointMake(width * 0.2, height * 0.18)];
            [ribbonA closePath];
            [accent setFill];
            [ribbonA fill];

            UIBezierPath *ribbonB = [UIBezierPath bezierPath];
            [ribbonB moveToPoint:CGPointMake(-width * 0.1, height * 0.5)];
            [ribbonB addCurveToPoint:CGPointMake(width * 1.05, height * 0.38)
                       controlPoint1:CGPointMake(width * 0.28, height * 0.68)
                       controlPoint2:CGPointMake(width * 0.76, height * 0.18)];
            [ribbonB addLineToPoint:CGPointMake(width * 1.05, height * 0.55)];
            [ribbonB addCurveToPoint:CGPointMake(-width * 0.1, height * 0.66)
                       controlPoint1:CGPointMake(width * 0.76, height * 0.72)
                       controlPoint2:CGPointMake(width * 0.28, height * 0.42)];
            [ribbonB closePath];
            [accentAlt setFill];
            [ribbonB fill];
        }

        NSArray<NSValue *> *cards = @[
            [NSValue valueWithCGRect:CGRectMake(width * 0.08, height * 0.12, width * 0.18, height * 0.11)],
            [NSValue valueWithCGRect:CGRectMake(width * 0.32, height * 0.22, width * 0.22, height * 0.12)],
            [NSValue valueWithCGRect:CGRectMake(width * 0.62, height * 0.62, width * 0.22, height * 0.12)],
        ];
        for (NSValue *value in cards) {
            UIBezierPath *cardPath = [UIBezierPath bezierPathWithRoundedRect:value.CGRectValue
                                                                cornerRadius:MIN(width, height) * 0.05];
            [card setFill];
            [cardPath fill];
        }
    }];
}

static void ISHWorkspaceSetCurrentThemeIdentifier(NSString *identifier) {
    if (!ISHWorkspaceThemeIdentifierIsValid(identifier))
        return;
    NSString *currentIdentifier = ISHWorkspaceCurrentThemeIdentifier();
    if ([currentIdentifier isEqualToString:identifier])
        return;
    [NSUserDefaults.standardUserDefaults setObject:identifier forKey:ISHWorkspaceToolThemePreferenceKey];
    [NSNotificationCenter.defaultCenter postNotificationName:ISHWorkspaceToolThemeDidChangeNotification object:nil];
}

static NSString *ISHWorkspaceCreateCustomThemeIdentifier(void) {
    return [NSString stringWithFormat:@"custom-%@", NSUUID.UUID.UUIDString.lowercaseString];
}

static void ISHWorkspaceSaveCustomThemeRecord(NSString *identifier,
                                              NSString *title,
                                              NSDictionary<NSString *, NSDictionary<NSString *, NSNumber *> *> *palette) {
    if (identifier.length == 0 || title.length == 0 || ![palette isKindOfClass:NSDictionary.class])
        return;

    NSMutableArray<NSDictionary<NSString *, id> *> *records = [ISHWorkspaceCustomThemeRecords() mutableCopy];
    NSMutableDictionary<NSString *, id> *record = [@{
        @"identifier": identifier,
        @"title": title,
        @"palette": palette,
        @"builtIn": @NO,
    } mutableCopy];
    BOOL replaced = NO;
    for (NSUInteger index = 0; index < records.count; index++) {
        if ([records[index][@"identifier"] isEqualToString:identifier]) {
            records[index] = record;
            replaced = YES;
            break;
        }
    }
    if (!replaced)
        [records addObject:record];
    [NSUserDefaults.standardUserDefaults setObject:records forKey:ISHWorkspaceCustomThemesDefaultsKey];
    [NSNotificationCenter.defaultCenter postNotificationName:ISHWorkspaceToolThemeDidChangeNotification object:nil];
}

static void ISHWorkspaceDeleteCustomThemeRecord(NSString *identifier) {
    if (identifier.length == 0)
        return;
    NSMutableArray<NSDictionary<NSString *, id> *> *records = [ISHWorkspaceCustomThemeRecords() mutableCopy];
    NSIndexSet *indexes = [records indexesOfObjectsPassingTest:^BOOL(NSDictionary<NSString *, id> *record, NSUInteger idx, BOOL *stop) {
        return [record[@"identifier"] isEqualToString:identifier];
    }];
    if (indexes.count == 0)
        return;
    [records removeObjectsAtIndexes:indexes];
    [NSUserDefaults.standardUserDefaults setObject:records forKey:ISHWorkspaceCustomThemesDefaultsKey];
    if ([[NSUserDefaults.standardUserDefaults stringForKey:ISHWorkspaceToolThemePreferenceKey] isEqualToString:identifier]) {
        [NSUserDefaults.standardUserDefaults setObject:ISHWorkspaceToolThemeAuroraIdentifier
                                                forKey:ISHWorkspaceToolThemePreferenceKey];
    }
    [NSNotificationCenter.defaultCenter postNotificationName:ISHWorkspaceToolThemeDidChangeNotification object:nil];
}

static BOOL ISHWorkspaceThemeIdentifierIsBuiltIn(NSString *identifier) {
    for (NSDictionary<NSString *, NSString *> *choice in ISHWorkspaceBuiltInThemeChoices()) {
        if ([choice[@"identifier"] isEqualToString:identifier])
            return YES;
    }
    return NO;
}

// WorkspaceThemedToolViewController's @interface now lives in
// WorkspaceViewController.h so applets defined in their own translation unit
// (e.g. WorkspaceFileManager.m) can subclass it.

@interface WorkspaceClockToolViewController : WorkspaceThemedToolViewController
@end

@interface WorkspaceInfoToolViewController : WorkspaceThemedToolViewController
@end

@interface WorkspaceMonitorToolViewController : WorkspaceThemedToolViewController
@end

@interface WorkspaceNetworksToolViewController : WorkspaceThemedToolViewController
@end

@interface WorkspaceStatusToolViewController : WorkspaceThemedToolViewController
@end

@interface WorkspaceWorkspacesToolViewController : WorkspaceThemedToolViewController
@end


@interface WorkspaceSessionsToolViewController : WorkspaceThemedToolViewController
@end

@interface WorkspaceStorageToolViewController : WorkspaceThemedToolViewController
@end

@interface WorkspaceShortcutsToolViewController : WorkspaceThemedToolViewController
@end

@interface WorkspaceLauncherToolViewController : WorkspaceThemedToolViewController <UITableViewDataSource, UITableViewDelegate, UITableViewDragDelegate, UITableViewDropDelegate, UIDropInteractionDelegate>
@end

@interface WorkspaceBrowserToolViewController : WorkspaceThemedToolViewController <UITextFieldDelegate, WKNavigationDelegate, WKUIDelegate>
@end

@interface WorkspaceThemesToolViewController : WorkspaceThemedToolViewController
@end

@interface WorkspaceAudioPlayerToolViewController : WorkspaceThemedToolViewController
@end

@interface WorkspaceMotePadToolViewController : WorkspaceThemedToolViewController
@end

static UIViewController *ISHCreateWorkspaceToolViewController(NSString *toolIdentifier) {
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolLLMIdentifier])
        return ISHCreateLLMClientViewController();
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolClockIdentifier])
        return [WorkspaceClockToolViewController new];
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolInfoIdentifier])
        return [WorkspaceInfoToolViewController new];
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolMonitorIdentifier])
        return [WorkspaceMonitorToolViewController new];
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolNetworksIdentifier])
        return [WorkspaceNetworksToolViewController new];
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolStatusIdentifier])
        return [WorkspaceStatusToolViewController new];
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolWorkspacesIdentifier])
        return [WorkspaceWorkspacesToolViewController new];
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolSessionsIdentifier])
        return [WorkspaceSessionsToolViewController new];
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolStorageIdentifier])
        return [WorkspaceStorageToolViewController new];
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolShortcutsIdentifier])
        return [WorkspaceShortcutsToolViewController new];
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolBrowserIdentifier])
        return [WorkspaceBrowserToolViewController new];
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolThemesIdentifier])
        return [WorkspaceThemesToolViewController new];
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolFilesystemsIdentifier])
        return ISHCreateRootsViewController();
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolSettingsIdentifier]) {
        // Embed the whole navigation controller (like Filesystems/Roots above), not just its
        // top view controller. AboutViewController drills into sub-pages with
        // "if (self.navigationController) push; else present fullscreen;" — stripping the nav
        // controller forced the fullscreen-modal branch, which had no way back to the workspace.
        return ISHCreateAboutNavigationController(NO, NO);
    }
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolDiagnosticsIdentifier])
        return ISHCreateDiagnosticsViewController();
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolLauncherIdentifier])
        return [WorkspaceLauncherToolViewController new];
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolAudioIdentifier])
        return [WorkspaceAudioPlayerToolViewController new];
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolMotePadIdentifier])
        return [WorkspaceMotePadToolViewController new];
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolFileManagerIdentifier])
        return [WorkspaceFileManagerToolViewController new];
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolMarkdownViewerIdentifier])
        return [WorkspaceMarkdownViewerToolViewController new];
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolImageViewerIdentifier])
        return [WorkspaceImageViewerToolViewController new];
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolVideoPlayerIdentifier])
        return [WorkspaceVideoPlayerToolViewController new];
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolDisplayIdentifier])
        return [DisplayViewController new];
    return nil;
}

NSString *ISHWorkspaceToolIdentifierForViewController(UIViewController *viewController) {
    if ([viewController isKindOfClass:NSClassFromString(@"LLMClientViewController")])
        return ISHWorkspaceToolLLMIdentifier;
    if ([viewController isKindOfClass:WorkspaceClockToolViewController.class])
        return ISHWorkspaceToolClockIdentifier;
    if ([viewController isKindOfClass:WorkspaceInfoToolViewController.class])
        return ISHWorkspaceToolInfoIdentifier;
    if ([viewController isKindOfClass:WorkspaceMonitorToolViewController.class])
        return ISHWorkspaceToolMonitorIdentifier;
    if ([viewController isKindOfClass:WorkspaceNetworksToolViewController.class])
        return ISHWorkspaceToolNetworksIdentifier;
    if ([viewController isKindOfClass:WorkspaceStatusToolViewController.class])
        return ISHWorkspaceToolStatusIdentifier;
    if ([viewController isKindOfClass:WorkspaceWorkspacesToolViewController.class])
        return ISHWorkspaceToolWorkspacesIdentifier;
    if ([viewController isKindOfClass:WorkspaceSessionsToolViewController.class])
        return ISHWorkspaceToolSessionsIdentifier;
    if ([viewController isKindOfClass:WorkspaceStorageToolViewController.class])
        return ISHWorkspaceToolStorageIdentifier;
    if ([viewController isKindOfClass:WorkspaceShortcutsToolViewController.class])
        return ISHWorkspaceToolShortcutsIdentifier;
    if ([viewController isKindOfClass:WorkspaceLauncherToolViewController.class])
        return ISHWorkspaceToolLauncherIdentifier;
    if ([viewController isKindOfClass:WorkspaceBrowserToolViewController.class])
        return ISHWorkspaceToolBrowserIdentifier;
    if ([viewController isKindOfClass:WorkspaceThemesToolViewController.class])
        return ISHWorkspaceToolThemesIdentifier;
    if ([viewController isKindOfClass:NSClassFromString(@"DiagnosticsViewController")])
        return ISHWorkspaceToolDiagnosticsIdentifier;
    if ([viewController isKindOfClass:NSClassFromString(@"AboutViewController")])
        return ISHWorkspaceToolSettingsIdentifier;
    if ([viewController isKindOfClass:NSClassFromString(@"RootsTableViewController")])
        return ISHWorkspaceToolFilesystemsIdentifier;
    if ([viewController isKindOfClass:WorkspaceAudioPlayerToolViewController.class])
        return ISHWorkspaceToolAudioIdentifier;
    if ([viewController isKindOfClass:WorkspaceMotePadToolViewController.class])
        return ISHWorkspaceToolMotePadIdentifier;
    if ([viewController isKindOfClass:WorkspaceFileManagerToolViewController.class])
        return ISHWorkspaceToolFileManagerIdentifier;
    if ([viewController isKindOfClass:WorkspaceMarkdownViewerToolViewController.class])
        return ISHWorkspaceToolMarkdownViewerIdentifier;
    if ([viewController isKindOfClass:WorkspaceImageViewerToolViewController.class])
        return ISHWorkspaceToolImageViewerIdentifier;
    if ([viewController isKindOfClass:WorkspaceVideoPlayerToolViewController.class])
        return ISHWorkspaceToolVideoPlayerIdentifier;
    if ([viewController isKindOfClass:DisplayViewController.class])
        return ISHWorkspaceToolDisplayIdentifier;
    return nil;
}

@implementation WorkspaceViewController

- (void)applyWorkspaceWallpaperImage:(UIImage *)image {
    self.desktopWallpaperView.image = image;
    self.desktopWallpaperView.hidden = (image == nil);
    [self.desktopSurfaceView sendSubviewToBack:self.desktopWallpaperView];
    self.desktopSurfaceView.layer.contents = nil;
}

- (CGSize)workspaceWallpaperTargetImageSize {
    CGSize targetSize = self.desktopSurfaceView.bounds.size;
    if (targetSize.width <= 1 || targetSize.height <= 1)
        targetSize = self.view.bounds.size;
    if (targetSize.width <= 1 || targetSize.height <= 1)
        targetSize = UIScreen.mainScreen.bounds.size;

    CGFloat scale = MAX(UIScreen.mainScreen.scale, 2.0);
    return CGSizeMake(MAX(1.0, round(targetSize.width * scale)),
                      MAX(1.0, round(targetSize.height * scale)));
}

- (void)applyCurrentThemeWallpaperIfNeededForced:(BOOL)forced {
    CGSize imageSize = [self workspaceWallpaperTargetImageSize];
    if (imageSize.width <= 1 || imageSize.height <= 1)
        return;

    NSString *identifier = ISHWorkspaceCurrentThemeIdentifier();
    BOOL sizeMatches = CGSizeEqualToSize(self.appliedWallpaperImageSize, imageSize);
    if (!forced &&
        self.desktopWallpaperView.image != nil &&
        sizeMatches &&
        [self.appliedWallpaperThemeIdentifier isEqualToString:identifier]) {
        return;
    }

    UIImage *image = ISHWorkspaceThemeArtworkImage(identifier,
                                                   ISHWorkspaceThemeEditablePaletteForIdentifier(identifier),
                                                   imageSize,
                                                   YES);
    if (image == nil)
        return;

    [self applyWorkspaceWallpaperImage:image];
    self.appliedWallpaperThemeIdentifier = identifier;
    self.appliedWallpaperImageSize = imageSize;
}

- (UILabel *)workspaceLabelWithTextStyle:(UIFontTextStyle)textStyle monospaced:(BOOL)monospaced {
    UILabel *label = [UILabel new];
    label.numberOfLines = 0;
    UIFont *preferredFont = [UIFont preferredFontForTextStyle:textStyle];
    if (@available(iOS 13.0, *)) {
        label.textColor = UIColor.labelColor;
        if (monospaced) {
            label.font = [UIFont monospacedDigitSystemFontOfSize:preferredFont.pointSize
                                                           weight:UIFontWeightSemibold];
        } else {
            label.font = preferredFont;
        }
    } else {
        label.textColor = UIColor.blackColor;
        label.font = preferredFont;
    }
    return label;
}

- (UILabel *)workspaceSectionTitle:(NSString *)title {
    UILabel *label = [self workspaceLabelWithTextStyle:UIFontTextStyleHeadline monospaced:NO];
    label.text = title;
    return label;
}

- (UIButton *)workspaceActionButtonWithTitle:(NSString *)title selector:(SEL)selector {
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    button.translatesAutoresizingMaskIntoConstraints = NO;
    button.contentHorizontalAlignment = UIControlContentHorizontalAlignmentLeft;
    button.titleLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];
    [button setTitle:title forState:UIControlStateNormal];
    [button addTarget:self action:selector forControlEvents:UIControlEventTouchUpInside];
    return button;
}

- (UIButton *)workspaceDockTileButtonWithTitle:(NSString *)title
                                      selector:(SEL)selector
                                    identifier:(NSString *)identifier {
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    button.translatesAutoresizingMaskIntoConstraints = NO;
    button.accessibilityIdentifier = identifier;
    if (ISHWorkspaceUsesPhoneLayout()) {
        button.contentEdgeInsets = UIEdgeInsetsMake(1, 4, 1, 4);
        button.layer.cornerRadius = 8;
        [button.heightAnchor constraintGreaterThanOrEqualToConstant:18].active = YES;
    } else {
        button.contentEdgeInsets = UIEdgeInsetsMake(1, 5, 1, 5);
        button.layer.cornerRadius = 10;
        [button.heightAnchor constraintGreaterThanOrEqualToConstant:20].active = YES;
    }
    button.contentHorizontalAlignment = UIControlContentHorizontalAlignmentCenter;
    button.contentVerticalAlignment = UIControlContentVerticalAlignmentCenter;
    button.titleLabel.numberOfLines = 2;
    button.titleLabel.textAlignment = NSTextAlignmentCenter;
    button.titleLabel.adjustsFontForContentSizeCategory = YES;
    button.layer.borderWidth = 1;
    [button addTarget:self action:selector forControlEvents:UIControlEventTouchUpInside];
    return button;
}

- (void)configureDockTileButton:(UIButton *)button
                          title:(NSString *)title
                          state:(NSString *)state
                         active:(BOOL)active
                      frontmost:(BOOL)frontmost {
    if (button == nil)
        return;

    CGFloat dockFontSize = ISHWorkspaceUsesPhoneLayout() ? 9.0 : 10.0;
    UIFont *titleFont = [UIFont systemFontOfSize:dockFontSize weight:UIFontWeightSemibold];
    UIFont *stateFont = [UIFont systemFontOfSize:dockFontSize weight:UIFontWeightMedium];
    NSMutableParagraphStyle *paragraphStyle = [NSMutableParagraphStyle new];
    paragraphStyle.alignment = NSTextAlignmentCenter;

    UIColor *fillColor = nil;
    UIColor *borderColor = nil;
    UIColor *titleColor = nil;
    UIColor *stateColor = nil;
    if (@available(iOS 13.0, *)) {
        if (frontmost) {
            fillColor = [UIColor.systemBlueColor colorWithAlphaComponent:0.18];
            borderColor = [UIColor.systemBlueColor colorWithAlphaComponent:0.45];
            titleColor = UIColor.labelColor;
            stateColor = UIColor.systemBlueColor;
        } else if (active) {
            fillColor = [UIColor.secondarySystemFillColor colorWithAlphaComponent:0.72];
            borderColor = [UIColor.separatorColor colorWithAlphaComponent:0.75];
            titleColor = UIColor.labelColor;
            stateColor = UIColor.secondaryLabelColor;
        } else {
            fillColor = [UIColor.tertiarySystemBackgroundColor colorWithAlphaComponent:0.92];
            borderColor = [UIColor.separatorColor colorWithAlphaComponent:0.65];
            titleColor = UIColor.secondaryLabelColor;
            stateColor = UIColor.systemBlueColor;
        }
    } else {
        if (frontmost) {
            fillColor = [UIColor colorWithRed:0.84 green:0.90 blue:1.0 alpha:1.0];
            borderColor = [UIColor colorWithRed:0.26 green:0.46 blue:0.92 alpha:1.0];
            titleColor = UIColor.blackColor;
            stateColor = [UIColor colorWithRed:0.13 green:0.31 blue:0.82 alpha:1.0];
        } else if (active) {
            fillColor = [UIColor colorWithWhite:0.92 alpha:1.0];
            borderColor = [UIColor colorWithWhite:0.75 alpha:1.0];
            titleColor = UIColor.blackColor;
            stateColor = UIColor.darkGrayColor;
        } else {
            fillColor = [UIColor colorWithWhite:0.96 alpha:1.0];
            borderColor = [UIColor colorWithWhite:0.78 alpha:1.0];
            titleColor = UIColor.darkGrayColor;
            stateColor = [UIColor colorWithRed:0.13 green:0.31 blue:0.82 alpha:1.0];
        }
    }

    NSDictionary<NSAttributedStringKey, id> *titleAttributes = @{
        NSFontAttributeName: titleFont,
        NSForegroundColorAttributeName: titleColor,
        NSParagraphStyleAttributeName: paragraphStyle,
    };
    NSDictionary<NSAttributedStringKey, id> *stateAttributes = @{
        NSFontAttributeName: stateFont,
        NSForegroundColorAttributeName: stateColor,
        NSParagraphStyleAttributeName: paragraphStyle,
    };
    NSMutableAttributedString *attributedTitle =
        [[NSMutableAttributedString alloc] initWithString:title attributes:titleAttributes];
    [attributedTitle appendAttributedString:[[NSAttributedString alloc] initWithString:@"\n" attributes:stateAttributes]];
    [attributedTitle appendAttributedString:[[NSAttributedString alloc] initWithString:state attributes:stateAttributes]];
    [button setAttributedTitle:attributedTitle forState:UIControlStateNormal];
    button.backgroundColor = fillColor;
    button.layer.borderColor = borderColor.CGColor;
    button.accessibilityValue = state;
    if (frontmost) {
        button.accessibilityTraits |= UIAccessibilityTraitSelected;
    } else {
        button.accessibilityTraits &= ~UIAccessibilityTraitSelected;
    }
}

- (UIView *)workspaceCardWithContentStack:(UIStackView **)contentStackOut {
    UIView *card = [UIView new];
    card.translatesAutoresizingMaskIntoConstraints = NO;
    CGFloat cornerRadius = ISHWorkspaceUsesPhoneLayout() ? 14.0 : 18.0;
    CGFloat contentInset = ISHWorkspaceUsesPhoneLayout() ? 14.0 : 18.0;
    CGFloat contentSpacing = ISHWorkspaceUsesPhoneLayout() ? 10.0 : 14.0;
    CGFloat shadowRadius = ISHWorkspaceUsesPhoneLayout() ? 12.0 : 18.0;
    CGFloat shadowOffset = ISHWorkspaceUsesPhoneLayout() ? 6.0 : 8.0;
    card.layer.cornerRadius = cornerRadius;
    card.layer.masksToBounds = NO;
    card.layer.shadowColor = UIColor.blackColor.CGColor;
    card.layer.shadowOpacity = 0.08;
    card.layer.shadowRadius = shadowRadius;
    card.layer.shadowOffset = CGSizeMake(0, shadowOffset);
    if (@available(iOS 13.0, *)) {
        card.backgroundColor = UIColor.secondarySystemBackgroundColor;
    } else {
        card.backgroundColor = [UIColor colorWithWhite:0.96 alpha:1.0];
    }

    UIStackView *contentStack = [UIStackView new];
    contentStack.axis = UILayoutConstraintAxisVertical;
    contentStack.spacing = contentSpacing;
    contentStack.translatesAutoresizingMaskIntoConstraints = NO;
    [card addSubview:contentStack];

    [NSLayoutConstraint activateConstraints:@[
        [contentStack.topAnchor constraintEqualToAnchor:card.topAnchor constant:contentInset],
        [contentStack.leadingAnchor constraintEqualToAnchor:card.leadingAnchor constant:contentInset],
        [contentStack.trailingAnchor constraintEqualToAnchor:card.trailingAnchor constant:-contentInset],
        [contentStack.bottomAnchor constraintEqualToAnchor:card.bottomAnchor constant:-contentInset],
    ]];

    if (contentStackOut != NULL)
        *contentStackOut = contentStack;
    return card;
}

- (CGRect)desktopUsableBounds {
    UIEdgeInsets insets = self.view.safeAreaInsets;
    CGRect bounds = self.desktopSurfaceView.bounds;
    if (ISHWorkspaceUsesPhoneLayout()) {
        return UIEdgeInsetsInsetRect(bounds, UIEdgeInsetsMake(insets.top, insets.left, insets.bottom, insets.right));
    }
    return UIEdgeInsetsInsetRect(bounds, UIEdgeInsetsMake(insets.top, 0, 0, 0));
}

- (CGRect)desktopFrameForWindowWithPreferredSize:(CGSize)preferredSize {
    CGRect usableBounds = [self desktopUsableBounds];
    CGFloat width = MIN(preferredSize.width, CGRectGetWidth(usableBounds));
    CGFloat height = MIN(preferredSize.height, CGRectGetHeight(usableBounds));
    CGFloat cascadeStep = ISHWorkspaceUsesPhoneLayout() ? 14.0 : 28.0;
    NSUInteger cascadeCount = ISHWorkspaceUsesPhoneLayout() ? 4 : 6;
    CGFloat verticalOffsetFactor = ISHWorkspaceUsesPhoneLayout() ? 0.08 : 0.16;
    CGFloat offset = (CGFloat) (self.desktopWindowCascadeIndex % cascadeCount) * cascadeStep;
    CGFloat originX = CGRectGetMinX(usableBounds) + MAX(0, (CGRectGetWidth(usableBounds) - width) * 0.5) + offset;
    CGFloat originY = CGRectGetMinY(usableBounds) + MAX(0, (CGRectGetHeight(usableBounds) - height) * verticalOffsetFactor) + offset;
    originX = MIN(originX, CGRectGetMaxX(usableBounds) - width);
    originY = MIN(originY, CGRectGetMaxY(usableBounds) - height);
    self.desktopWindowCascadeIndex += 1;
    return CGRectIntegral(CGRectMake(originX, originY, width, height));
}

- (CGRect)clampedDesktopFrame:(CGRect)frame forWindow:(ISHWorkspaceContainedWindowView *)windowView {
    // Enforce the window's own min/max content size first (e.g. the Music player
    // locks its width); a no-op for windows that leave these at 0.
    CGSize minSize = windowView.minimumSize;
    CGSize maxSize = windowView.maximumSize;
    if (minSize.width > 0) frame.size.width = MAX(frame.size.width, minSize.width);
    if (minSize.height > 0) frame.size.height = MAX(frame.size.height, minSize.height);
    if (maxSize.width > 0) frame.size.width = MIN(frame.size.width, maxSize.width);
    if (maxSize.height > 0) frame.size.height = MIN(frame.size.height, maxSize.height);

    // Before the desktop surface has been laid out its usable bounds are 0x0; clamping
    // against them collapses the window to zero size and Auto Layout breaks the chrome
    // constraints. Skip the surface clamp until real bounds exist — placement is
    // re-applied from viewDidLayoutSubviews once the surface has a size.
    CGRect usableBounds = [self desktopUsableBounds];
    if (CGRectGetWidth(usableBounds) <= 0 || CGRectGetHeight(usableBounds) <= 0)
        return ISHWorkspaceRectWithRoundedOriginPreservingSize(frame);
    if (CGRectGetWidth(frame) > CGRectGetWidth(usableBounds))
        frame.size.width = CGRectGetWidth(usableBounds);
    if (CGRectGetHeight(frame) > CGRectGetHeight(usableBounds))
        frame.size.height = CGRectGetHeight(usableBounds);

    CGFloat visibleWidth = MIN(CGRectGetWidth(frame), 140.0);
    CGFloat minX = CGRectGetMinX(usableBounds) - (CGRectGetWidth(frame) - visibleWidth);
    CGFloat maxX = CGRectGetMaxX(usableBounds) - visibleWidth;
    CGFloat visibleHeight = MIN(CGRectGetHeight(frame), ISHWorkspaceWindowTitleBarHeight);
    CGFloat minY = CGRectGetMinY(usableBounds);
    CGFloat maxY = CGRectGetMaxY(usableBounds) - visibleHeight;

    if (maxX < minX)
        maxX = minX;
    if (maxY < minY)
        maxY = minY;

    frame.origin.x = MIN(MAX(frame.origin.x, minX), maxX);
    frame.origin.y = MIN(MAX(frame.origin.y, minY), maxY);
    return ISHWorkspaceRectWithRoundedOriginPreservingSize(frame);
}

- (void)clampDesktopWindowToVisibleBounds:(ISHWorkspaceContainedWindowView *)windowView {
    if (windowView.superview == nil || CGRectIsEmpty(windowView.bounds))
        return;

    // If a previous pass shrank/moved this window for a smaller surface (rotation to
    // portrait, or the portrait-sized snapshot layout iOS runs while the app is
    // backgrounded -- the source of "my full-width window came back at 75%"), try the
    // remembered frame first so the window pops back the moment the surface fits it.
    CGRect remembered = windowView.frameBeforeBoundsClamp;
    if (!CGRectIsNull(remembered)) {
        CGRect restored = [self clampedDesktopFrame:remembered forWindow:windowView];
        if (CGRectEqualToRect(restored, remembered)) {
            windowView.frame = remembered;
            windowView.frameBeforeBoundsClamp = CGRectNull;
        } else {
            // Surface still too small: show the clamped version of the REMEMBERED frame
            // (not of the current one) so repeated passes can't ratchet the window down,
            // and keep the memory for when the surface grows back.
            windowView.frame = restored;
        }
        return;
    }

    CGRect frame = windowView.frame;
    CGRect clamped = [self clampedDesktopFrame:frame forWindow:windowView];
    if (!CGRectEqualToRect(clamped, frame))
        windowView.frameBeforeBoundsClamp = frame;
    windowView.frame = clamped;
}

- (void)pinDesktopWindowToBottomCenter:(ISHWorkspaceContainedWindowView *)windowView {
    if (windowView == nil || windowView.superview == nil)
        return;
    CGRect usableBounds = [self desktopUsableBounds];
    CGRect frame = windowView.frame;
    frame.origin.x = CGRectGetMinX(usableBounds) + (CGRectGetWidth(usableBounds) - CGRectGetWidth(frame)) * 0.5;
    frame.origin.y = CGRectGetMaxY(usableBounds) - CGRectGetHeight(frame);
    windowView.frame = ISHWorkspaceRectWithRoundedOriginPreservingSize(frame);
}

- (void)positionDesktopWindowAtTopRight:(ISHWorkspaceContainedWindowView *)windowView {
    if (windowView == nil || windowView.superview == nil)
        return;
    CGRect usableBounds = [self desktopUsableBounds];
    CGRect frame = windowView.frame;
    CGFloat inset = ISHWorkspaceUsesPhoneLayout() ? 8.0 : 12.0;
    frame.origin.x = CGRectGetMaxX(usableBounds) - CGRectGetWidth(frame) - inset;
    frame.origin.y = CGRectGetMinY(usableBounds) + inset;
    windowView.frame = [self clampedDesktopFrame:frame forWindow:windowView];
}

- (void)resizeDesktopWindow:(ISHWorkspaceContainedWindowView *)windowView
                     toSize:(CGSize)size
                   animated:(BOOL)animated {
    if (windowView == nil || windowView.superview == nil)
        return;

    CGRect usableBounds = [self desktopUsableBounds];
    CGSize minimumSize = windowView.minimumSize;
    CGFloat width = MAX(minimumSize.width, size.width);
    CGFloat height = MAX(minimumSize.height, size.height);
    if (windowView.maximumSize.width > 0) {
        width = MIN(width, windowView.maximumSize.width);
    }
    if (windowView.maximumSize.height > 0) {
        height = MIN(height, windowView.maximumSize.height);
    }
    // Same zero-bounds hazard as clampedDesktopFrame: — don't clamp to an unlaid-out surface.
    if (CGRectGetWidth(usableBounds) > 0 && CGRectGetHeight(usableBounds) > 0) {
        width = MIN(width, CGRectGetWidth(usableBounds));
        height = MIN(height, CGRectGetHeight(usableBounds));
    }

    CGRect frame = windowView.frame;
    frame.size = CGSizeMake(width, height);
    frame = [self clampedDesktopFrame:frame forWindow:windowView];
    windowView.preferredSize = frame.size;
    if (windowView.pinnedToBottomCenter) {
        frame.origin.x = CGRectGetMinX(usableBounds) + (CGRectGetWidth(usableBounds) - CGRectGetWidth(frame)) * 0.5;
        frame.origin.y = CGRectGetMaxY(usableBounds) - CGRectGetHeight(frame);
        frame = ISHWorkspaceRectWithRoundedOriginPreservingSize(frame);
    }
    void (^changes)(void) = ^{
        windowView.frame = frame;
    };
    if (animated) {
        [UIView animateWithDuration:0.22 animations:changes];
    } else {
        changes();
    }
}

- (NSDictionary<NSString *, NSNumber *> *)normalizedFrameDescriptorForFrame:(CGRect)frame {
    CGRect usableBounds = [self desktopUsableBounds];
    CGFloat usableWidth = CGRectGetWidth(usableBounds);
    CGFloat usableHeight = CGRectGetHeight(usableBounds);
    if (usableWidth <= 0 || usableHeight <= 0)
        return nil;

    return @{
        @"x": @((CGRectGetMinX(frame) - CGRectGetMinX(usableBounds)) / usableWidth),
        @"y": @((CGRectGetMinY(frame) - CGRectGetMinY(usableBounds)) / usableHeight),
        @"width": @(CGRectGetWidth(frame) / usableWidth),
        @"height": @(CGRectGetHeight(frame) / usableHeight),
    };
}

- (CGRect)frameFromNormalizedDescriptor:(NSDictionary<NSString *, id> *)descriptor
                           fallbackSize:(CGSize)fallbackSize {
    CGRect usableBounds = [self desktopUsableBounds];
    CGFloat usableWidth = CGRectGetWidth(usableBounds);
    CGFloat usableHeight = CGRectGetHeight(usableBounds);
    if (usableWidth <= 0 || usableHeight <= 0)
        return CGRectMake(0, 0, MAX(1, fallbackSize.width), MAX(1, fallbackSize.height));

    CGFloat normalizedWidth = [descriptor[@"width"] doubleValue];
    CGFloat normalizedHeight = [descriptor[@"height"] doubleValue];
    CGFloat width = normalizedWidth > 0 ? normalizedWidth * usableWidth : fallbackSize.width;
    CGFloat height = normalizedHeight > 0 ? normalizedHeight * usableHeight : fallbackSize.height;
    width = MIN(MAX(width, 1), usableWidth);
    height = MIN(MAX(height, 1), usableHeight);

    CGFloat normalizedX = [descriptor[@"x"] doubleValue];
    CGFloat normalizedY = [descriptor[@"y"] doubleValue];
    CGFloat originX = CGRectGetMinX(usableBounds) + normalizedX * usableWidth;
    CGFloat originY = CGRectGetMinY(usableBounds) + normalizedY * usableHeight;
    originX = MIN(MAX(originX, CGRectGetMinX(usableBounds)), CGRectGetMaxX(usableBounds) - width);
    originY = MIN(MAX(originY, CGRectGetMinY(usableBounds)), CGRectGetMaxY(usableBounds) - height);
    return CGRectIntegral(CGRectMake(originX, originY, width, height));
}

- (void)applySavedFrameDescriptor:(NSDictionary<NSString *, id> *)descriptor
                         toWindow:(ISHWorkspaceContainedWindowView *)windowView
                     fallbackSize:(CGSize)fallbackSize {
    if (![descriptor isKindOfClass:NSDictionary.class] || windowView == nil)
        return;
    CGRect frame = [self frameFromNormalizedDescriptor:descriptor fallbackSize:fallbackSize];
    windowView.frame = frame;
    windowView.preferredSize = frame.size;
    windowView.didApplyInitialFrame = YES;
    [self clampDesktopWindowToVisibleBounds:windowView];
}

- (NSDictionary<NSString *, NSNumber *> *)absoluteFrameDescriptorForFrame:(CGRect)frame {
    if (CGRectIsEmpty(frame))
        return nil;
    return @{
        @"originX": @(CGRectGetMinX(frame)),
        @"originY": @(CGRectGetMinY(frame)),
        @"width": @(CGRectGetWidth(frame)),
        @"height": @(CGRectGetHeight(frame)),
    };
}

- (void)applyAbsoluteFrameDescriptor:(NSDictionary<NSString *, id> *)descriptor toWindow:(ISHWorkspaceContainedWindowView *)windowView {
    if (![descriptor isKindOfClass:NSDictionary.class] || windowView == nil)
        return;
    CGRect frame = CGRectMake([descriptor[@"originX"] doubleValue],
                              [descriptor[@"originY"] doubleValue],
                              [descriptor[@"width"] doubleValue],
                              [descriptor[@"height"] doubleValue]);
    if (CGRectGetWidth(frame) <= 0 || CGRectGetHeight(frame) <= 0)
        return;
    windowView.frame = frame;
    windowView.preferredSize = frame.size;
    windowView.didApplyInitialFrame = YES;
    [self clampDesktopWindowToVisibleBounds:windowView];
}

- (void)applyInitialFrameIfNeededToDesktopWindow:(ISHWorkspaceContainedWindowView *)windowView {
    if (windowView.didApplyInitialFrame || self.desktopSurfaceView.bounds.size.width <= 0 || self.desktopSurfaceView.bounds.size.height <= 0)
        return;
    windowView.frame = [self desktopFrameForWindowWithPreferredSize:windowView.preferredSize];
    windowView.didApplyInitialFrame = YES;
}

- (ISHWorkspaceContainedWindowView *)createDesktopWindowWithTitle:(NSString *)title
                                                    preferredSize:(CGSize)preferredSize
                                                 showsCloseButton:(BOOL)showsCloseButton {
    return [self createDesktopWindowWithTitle:title
                                preferredSize:preferredSize
                             showsCloseButton:showsCloseButton
                       appliesInitialPlacement:YES];
}

- (ISHWorkspaceContainedWindowView *)createDesktopWindowWithTitle:(NSString *)title
                                                    preferredSize:(CGSize)preferredSize
                                                 showsCloseButton:(BOOL)showsCloseButton
                                           appliesInitialPlacement:(BOOL)appliesInitialPlacement {
    ISHWorkspaceContainedWindowView *windowView = [[ISHWorkspaceContainedWindowView alloc] initWithTitle:title
                                                                                         showsCloseButton:showsCloseButton];
    windowView.preferredSize = preferredSize;
    windowView.frame = CGRectIntegral(CGRectMake(0, 0,
                                                 MAX(1, preferredSize.width),
                                                 MAX(1, preferredSize.height)));
    [self.desktopSurfaceView addSubview:windowView];
    [self.desktopWindows addObject:windowView];
    windowView.workspaceDesktopIndex = self.activeDesktopIndex;
    if (appliesInitialPlacement) {
        [self applyInitialFrameIfNeededToDesktopWindow:windowView];
        [self.desktopSurfaceView bringSubviewToFront:windowView];
    }
    __weak typeof(self) weakSelf = self;
    __weak typeof(windowView) weakWindowView = windowView;
    windowView.didBecomeFrontmostHandler = ^{
        typeof(self) strongSelf = weakSelf;
        typeof(windowView) strongWindowView = weakWindowView;
        if (strongSelf == nil || strongWindowView == nil)
            return;
        if (strongWindowView.hostedTerminalViewController != nil) {
            [strongWindowView.hostedTerminalViewController focusTerminal];
        } else {
            // Non-terminal applets (MotePad, ...) don't have a
            // hostedTerminalViewController to focus -- ask the window's own
            // content view controller instead, if it opts in. Without this,
            // cycling back to e.g. a MotePad window (via tap, Ctrl+Tab, or
            // Cmd+Arrow Desktop switching) left no first responder: no
            // cursor, no typing, until the user tapped inside it again.
            UIViewController *contentViewController = [strongSelf contentViewControllerForDesktopWindow:strongWindowView];
            if ([contentViewController conformsToProtocol:@protocol(WorkspaceFocusable)])
                [(id<WorkspaceFocusable>) contentViewController workspaceToolDidBecomeFrontmost];
        }
        [strongSelf refreshDockButtons];
    };
    return windowView;
}

// Depth-first search for the view that currently holds first-responder status
// inside a (sub)tree — used by the shared close path to hand keyboard focus back
// to the workspace before a window containing the responder is torn down.
static UIView *ISHWorkspaceFindFirstResponder(UIView *view) {
    if (view.isFirstResponder)
        return view;
    for (UIView *subview in view.subviews) {
        UIView *responder = ISHWorkspaceFindFirstResponder(subview);
        if (responder != nil)
            return responder;
    }
    return nil;
}

- (void)attachViewController:(UIViewController *)viewController toDesktopWindow:(ISHWorkspaceContainedWindowView *)windowView {
    if ([viewController isKindOfClass:WorkspaceThemedToolViewController.class]) {
        ((WorkspaceThemedToolViewController *) viewController).workspaceHostViewController = self;
    }
    // Once a controller is embedded inside a workspace window, the contained window's frame is the
    // authoritative size. Clearing preferredContentSize avoids UIKit reusing the child's standalone
    // preferred size for the outer app window when the contained window is restored from fullscreen.
    viewController.preferredContentSize = CGSizeZero;
    [self addChildViewController:viewController];
    viewController.view.translatesAutoresizingMaskIntoConstraints = NO;
    [windowView.contentContainerView addSubview:viewController.view];
    [NSLayoutConstraint activateConstraints:@[
        [viewController.view.topAnchor constraintEqualToAnchor:windowView.contentContainerView.topAnchor],
        [viewController.view.leadingAnchor constraintEqualToAnchor:windowView.contentContainerView.leadingAnchor],
        [viewController.view.trailingAnchor constraintEqualToAnchor:windowView.contentContainerView.trailingAnchor],
        [viewController.view.bottomAnchor constraintEqualToAnchor:windowView.contentContainerView.bottomAnchor],
    ]];
    [viewController didMoveToParentViewController:self];

    __weak typeof(self) weakSelf = self;
    __weak typeof(viewController) weakViewController = viewController;
    __weak typeof(windowView) weakWindowView = windowView;
    windowView.closeHandler = ^{
        typeof(self) strongSelf = weakSelf;
        UIViewController *strongViewController = weakViewController;
        ISHWorkspaceContainedWindowView *strongWindowView = weakWindowView;
        if (strongSelf == nil || strongViewController == nil || strongWindowView == nil)
            return;
        // First-responder handoff: tearing down a window while one of its views (e.g.
        // MotePad's text view — the workspace's first text-input applet) is first
        // responder leaves the workspace with no responder, wedging keyboard input
        // until the user taps a terminal. Resign it before the teardown and afterwards
        // revive input the same way the tap-to-focus path does (focusTerminal on the
        // frontmost terminal window). Done here in the shared tool close path — every
        // close route (chrome ×, ⌘W, menu Quit) funnels through this handler, and any
        // future text-input applet gets the same protection for free.
        UIView *closingFirstResponder = ISHWorkspaceFindFirstResponder(strongWindowView);
        if (closingFirstResponder != nil)
            [closingFirstResponder resignFirstResponder];
        [strongViewController willMoveToParentViewController:nil];
        [strongViewController.view removeFromSuperview];
        [strongViewController removeFromParentViewController];
        [strongSelf.desktopWindows removeObject:strongWindowView];
        [strongWindowView removeFromSuperview];
        [strongSelf refreshDockButtons];
        if (closingFirstResponder != nil)
            [[strongSelf frontmostDesktopTerminalWindow].hostedTerminalViewController focusTerminal];
    };
}

- (NSUUID *)persistentTerminalUUIDForWindow:(ISHWorkspaceContainedWindowView *)windowView {
    TerminalViewController *terminalViewController = windowView.hostedTerminalViewController;
    if (terminalViewController == nil)
        return nil;
    NSUUID *sessionUUID = terminalViewController.sessionTerminalUUID;
    if (sessionUUID != nil)
        return sessionUUID;
    return terminalViewController.terminal.uuid;
}

- (NSUUID *)displayedTerminalUUIDForWindow:(ISHWorkspaceContainedWindowView *)windowView {
    TerminalViewController *terminalViewController = windowView.hostedTerminalViewController;
    if (terminalViewController == nil)
        return nil;
    return terminalViewController.terminal.uuid;
}

- (NSString *)persistentTerminalRoleForWindow:(ISHWorkspaceContainedWindowView *)windowView {
    if (windowView.workspaceTerminalRole.length > 0)
        return windowView.workspaceTerminalRole;
    TerminalViewController *terminalViewController = windowView.hostedTerminalViewController;
    if (terminalViewController == nil)
        return nil;
    return ISHWorkspaceTerminalRoleForTerminal(terminalViewController.terminal);
}

- (NSDictionary<NSString *, id> *)savedLayoutDescriptorForWindow:(ISHWorkspaceContainedWindowView *)windowView {
    NSDictionary<NSString *, NSNumber *> *frameDescriptor = [self normalizedFrameDescriptorForFrame:windowView.frame];
    if (frameDescriptor == nil)
        return nil;

    if (windowView == self.dashboardWindow) {
        return @{
            @"kind": ISHWorkspaceSavedLayoutKindDashboard,
            @"frame": frameDescriptor,
            @"hidden": @(self.dashboardWindow.hidden),
        };
    }

    if (windowView == self.dockWindow) {
        return @{
            @"kind": ISHWorkspaceSavedLayoutKindDock,
            @"frame": frameDescriptor,
            @"hidden": @(self.dockWindow.hidden),
            @"pinned": @(self.dockWindow.pinnedToBottomCenter),
        };
    }

    if (windowView.hostedTerminalViewController != nil) {
        NSUUID *sessionTerminalUUID = [self persistentTerminalUUIDForWindow:windowView];
        NSUUID *displayTerminalUUID = [self displayedTerminalUUIDForWindow:windowView];
        NSString *terminalRole = [self persistentTerminalRoleForWindow:windowView];
        if (sessionTerminalUUID == nil && displayTerminalUUID == nil)
            return nil;
        NSMutableDictionary<NSString *, id> *descriptor = [@{
            @"kind": ISHWorkspaceSavedLayoutKindTerminal,
            @"frame": frameDescriptor,
            @"terminalUUID": (displayTerminalUUID ?: sessionTerminalUUID).UUIDString,
        } mutableCopy];
        if (sessionTerminalUUID != nil)
            descriptor[@"sessionTerminalUUID"] = sessionTerminalUUID.UUIDString;
        if (terminalRole.length > 0)
            descriptor[@"terminalRole"] = terminalRole;
        CGFloat overrideFontSize = windowView.hostedTerminalViewController.overrideFontSize;
        if (overrideFontSize > 0)
            descriptor[@"fontSize"] = @(overrideFontSize);
        descriptor[@"desktopIndex"] = @(windowView.workspaceDesktopIndex);
        return descriptor;
    }

    if (windowView.workspaceToolIdentifier.length > 0) {
        NSMutableDictionary<NSString *, id> *descriptor = [@{
            @"kind": ISHWorkspaceSavedLayoutKindTool,
            @"frame": frameDescriptor,
            @"toolIdentifier": windowView.workspaceToolIdentifier,
            @"desktopIndex": @(windowView.workspaceDesktopIndex),
        } mutableCopy];
        // Applet-provided content state (current directory, open file) rides
        // along in the layout descriptor so the window comes back showing
        // what it showed, not just where it was.
        UIViewController *contentViewController = [self contentViewControllerForDesktopWindow:windowView];
        if ([contentViewController conformsToProtocol:@protocol(WorkspaceStatefulTool)]) {
            NSDictionary<NSString *, id> *state = [(id<WorkspaceStatefulTool>) contentViewController workspaceToolStateForSaving];
            if (state.count > 0)
                descriptor[@"state"] = state;
        }
        return descriptor;
    }

    return nil;
}

- (void)closeAllRestorableDesktopWindows {
    for (ISHWorkspaceContainedWindowView *windowView in self.desktopWindows.copy) {
        if (windowView == self.dashboardWindow || windowView == self.dockWindow)
            continue;
        if (windowView.closeHandler != nil)
            windowView.closeHandler();
    }
}

- (void)applySavedDashboardDescriptor:(NSDictionary<NSString *, id> *)descriptor {
    NSDictionary<NSString *, id> *frameDescriptor = descriptor[@"frame"];
    CGSize fallbackSize = self.dashboardWindow.bounds.size.width > 0
        ? self.dashboardWindow.bounds.size
        : self.dashboardWindow.preferredSize;
    [self applySavedFrameDescriptor:frameDescriptor toWindow:self.dashboardWindow fallbackSize:fallbackSize];
    self.dashboardWindow.hidden = [descriptor[@"hidden"] boolValue];
}

- (void)applySavedDockDescriptor:(NSDictionary<NSString *, id> *)descriptor {
    if (self.dockWindow == nil)
        return;
    NSDictionary<NSString *, id> *frameDescriptor = descriptor[@"frame"];
    CGSize fallbackSize = self.dockWindow.bounds.size.width > 0
        ? self.dockWindow.bounds.size
        : self.dockWindow.preferredSize;
    self.dockWindow.pinnedToBottomCenter = [descriptor[@"pinned"] boolValue];
    [self applySavedFrameDescriptor:frameDescriptor toWindow:self.dockWindow fallbackSize:fallbackSize];
    self.dockWindow.hidden = [descriptor[@"hidden"] boolValue];
}

- (NSString *)terminalRoleFromSavedDescriptor:(NSDictionary<NSString *, id> *)descriptor
                               displayTerminal:(Terminal *)displayTerminal {
    NSString *terminalRole = descriptor[@"terminalRole"];
    if (terminalRole.length > 0)
        return terminalRole;
    if (displayTerminal != nil)
        return ISHWorkspaceTerminalRoleForTerminal(displayTerminal);
    return ISHWorkspaceTerminalRoleSessionShell;
}

- (void)configureRestoredTerminalViewController:(TerminalViewController *)terminalViewController
                                    inWindow:(ISHWorkspaceContainedWindowView *)windowView
                            displayTerminalUUID:(NSUUID *)displayTerminalUUID
                                  terminalRole:(NSString *)terminalRole {
    Terminal *displayTerminal = displayTerminalUUID != nil ? [Terminal terminalWithUUID:displayTerminalUUID] : terminalViewController.terminal;
    if ([terminalRole isEqualToString:ISHWorkspaceTerminalRoleSystemConsole]) {
        if ([ISHWorkspaceTerminalRoleForTerminal(displayTerminal) isEqualToString:ISHWorkspaceTerminalRoleSystemConsole]) {
            terminalViewController.terminal = displayTerminal;
        } else {
            [terminalViewController showSystemConsoleForCurrentSession];
            displayTerminal = terminalViewController.terminal;
        }
    } else if ([terminalRole isEqualToString:ISHWorkspaceTerminalRoleSessionShell]) {
        if ([ISHWorkspaceTerminalRoleForTerminal(displayTerminal) isEqualToString:ISHWorkspaceTerminalRoleSessionShell]) {
            terminalViewController.terminal = displayTerminal;
        } else {
            [terminalViewController showSessionShellForCurrentSession];
            displayTerminal = terminalViewController.terminal;
        }
    } else if (displayTerminal != nil) {
        terminalViewController.terminal = displayTerminal;
    }
    NSString *effectiveRole = terminalRole.length > 0 ? terminalRole : ISHWorkspaceTerminalRoleForTerminal(displayTerminal);
    windowView.workspaceTerminalRole = effectiveRole;
    windowView.titleLabel.text = ISHWorkspaceTitleForTerminalRole(effectiveRole, displayTerminal);
}

- (ISHWorkspaceContainedWindowView *)restoreDesktopTerminalWindowWithSessionUUID:(NSUUID *)sessionTerminalUUID
                                                             displayTerminalUUID:(NSUUID *)displayTerminalUUID
                                                                   terminalRole:(NSString *)terminalRole {
    if (displayTerminalUUID == nil && sessionTerminalUUID == nil)
        return nil;

    if (@available(iOS 13.0, *)) {
        UISceneSession *existingSession = displayTerminalUUID != nil
            ? [self sceneSessionHostingTerminalUUID:displayTerminalUUID]
            : nil;
        if (existingSession == nil && sessionTerminalUUID != nil) {
            existingSession = [self sceneSessionHostingTerminalUUID:sessionTerminalUUID];
        }
        UISceneSession *currentSession = self.view.window.windowScene.session;
        if (existingSession != nil && existingSession != currentSession)
            return nil;
    }

    ISHWorkspaceContainedWindowView *containedWindow = displayTerminalUUID != nil
        ? [self desktopWindowDisplayingTerminalUUID:displayTerminalUUID]
        : nil;
    if (containedWindow == nil && sessionTerminalUUID != nil) {
        containedWindow = [self desktopWindowHostingTerminalUUID:sessionTerminalUUID];
        if (containedWindow != nil && terminalRole.length > 0 &&
            ![containedWindow.workspaceTerminalRole isEqualToString:terminalRole]) {
            containedWindow = nil;
        }
    }
    if (containedWindow != nil)
        return containedWindow;

    Terminal *displayTerminal = displayTerminalUUID != nil ? [Terminal terminalWithUUID:displayTerminalUUID] : nil;
    if (displayTerminal != nil && displayTerminal.webView.superview != nil)
        return nil;

    TerminalViewController *terminalViewController = [self createDesktopTerminalViewController];
    if (terminalViewController == nil)
        return nil;

    NSUUID *restoreUUID = sessionTerminalUUID ?: displayTerminalUUID;
    NSString *title = ISHWorkspaceTitleForTerminalRole(terminalRole, displayTerminal);
    // Must be set before reconnect: a dead saved terminal makes reconnect start a
    // fresh session, and a restored Session Shell window has to come back as root.
    terminalViewController.alwaysLoginAsRoot =
        [terminalRole isEqualToString:ISHWorkspaceTerminalRoleSessionShell];
    ISHWorkspaceContainedWindowView *windowView =
        [self openDesktopTerminalWindowWithTitle:title terminalViewController:terminalViewController];
    [terminalViewController reconnectSessionFromTerminalUUID:restoreUUID];
    [self configureRestoredTerminalViewController:terminalViewController
                                         inWindow:windowView
                                 displayTerminalUUID:displayTerminalUUID
                                       terminalRole:terminalRole];
    return windowView;
}

- (ISHWorkspaceContainedWindowView *)openWorkspaceToolWindowWithIdentifier:(NSString *)toolIdentifier {
    // Global tools (Desktops/Launcher) are singletons shared by every Desktop — reuse the one
    // window instead of creating a second with its own separate state.
    if ([self isGlobalToolIdentifier:toolIdentifier]) {
        ISHWorkspaceContainedWindowView *existing = [self desktopWindowForToolIdentifier:toolIdentifier];
        if (existing != nil)
            return existing;
    }
    // The Music player is a window onto one shared playback engine (ISHAudioPlayerEngine.sharedEngine),
    // not independent state -- a second window wouldn't be a second player, just a second remote that
    // mirrors the first. Reuse whichever Music window is already open instead of spawning a duplicate
    // when another song is opened, summoning it to the current Desktop if it was hidden on another one
    // (same reveal behavior as openOrFocusWorkspaceToolIdentifier:).
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolAudioIdentifier]) {
        ISHWorkspaceContainedWindowView *existing = [self desktopWindowForToolIdentifier:toolIdentifier];
        if (existing != nil) {
            existing.workspaceDesktopIndex = self.activeDesktopIndex;
            existing.hidden = NO;
            return existing;
        }
    }
    UIViewController *viewController = ISHCreateWorkspaceToolViewController(toolIdentifier);
    if (viewController == nil)
        return nil;
    CGSize preferredSize = ISHWorkspacePreferredToolContentSize(toolIdentifier);
    viewController.preferredContentSize = preferredSize;
    BOOL workspacesTool = [toolIdentifier isEqualToString:ISHWorkspaceToolWorkspacesIdentifier];
    // On iPad the Desktops applet is a pinned utility with its own persisted top-right placement.
    // On a phone that placement misbehaves (and the persist key is shared across idioms), so there
    // the Desktops applet opens like any other tool — normal cascade placement, same as Launcher.
    BOOL pinnedWorkspaces = workspacesTool && !ISHWorkspaceUsesPhoneLayout();
    // The Launcher (a global tool) persists its own frame like the dock, so it returns to exactly
    // where the user left it after a foreground transition instead of drifting to its open spot.
    BOOL launcherTool = [toolIdentifier isEqualToString:ISHWorkspaceToolLauncherIdentifier];
    // Settings embeds its own navigation bar and gets a "Done" item there instead (see below),
    // so suppress the redundant chrome × that would otherwise sit right above that bar.
    BOOL settingsTool = [toolIdentifier isEqualToString:ISHWorkspaceToolSettingsIdentifier];
    ISHWorkspaceContainedWindowView *windowView =
        [self createDesktopWindowWithTitle:ISHWorkspaceToolTitle(toolIdentifier)
                             preferredSize:preferredSize
                          showsCloseButton:!settingsTool
                    appliesInitialPlacement:!pinnedWorkspaces];
    windowView.workspaceToolIdentifier = toolIdentifier;
    // Same double-tap-to-maximize convenience Terminal windows get (see
    // openDesktopTerminalWindowWithTitle:terminalViewController:), now on every tool
    // window. Safe even for the pinned Launcher/Desktops utilities: handleTitleBarDoubleTap:
    // only invokes frameDidChangeHandler (their autosave) when restoring back down, never
    // while zoomed, so their persisted "normal" position can't get clobbered by a zoom.
    windowView.titleBarDoubleTapZoomEnabled = YES;
    if ([self isGlobalToolIdentifier:toolIdentifier])
        windowView.workspaceDesktopIndex = 0;  // global singletons live on the first Desktop
    CGSize minimumSize = ISHWorkspaceMinimumToolContentSize(toolIdentifier);
    if (!CGSizeEqualToSize(minimumSize, CGSizeZero)) {
        windowView.resizable = YES;
        windowView.minimumSize = minimumSize;
    }
    windowView.maximumSize = ISHWorkspaceMaximumToolContentSize(toolIdentifier);
    if (pinnedWorkspaces) {
        __weak typeof(self) weakSelf = self;
        windowView.frameDidChangeHandler = ^{
            [weakSelf persistDefaultWorkspaceUtilityFrames];
        };
    }
    if (launcherTool) {
        __weak typeof(self) weakSelf = self;
        windowView.frameDidChangeHandler = ^{
            [weakSelf persistLauncherWindowFrame];
        };
    }
    [self attachViewController:viewController toDesktopWindow:windowView];
    if (settingsTool && [viewController isKindOfClass:UINavigationController.class]) {
        // Give the embedded Settings screen a real, labelled dismiss control in its own
        // navigation bar. AboutViewController's own buttons ("Workspace"/"Done") assume it was
        // presented modally or owns a scene, so they don't close a tool window; this routes
        // through the same teardown the chrome × uses.
        UIViewController *settingsRoot = ((UINavigationController *) viewController).viewControllers.firstObject;
        settingsRoot.navigationItem.leftBarButtonItem =
            [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemDone
                                                          target:self
                                                          action:@selector(dismissSettingsToolWindow:)];
    }
    if (pinnedWorkspaces)
        [self applyInitialPlacementToWorkspacesWindow:windowView];
    if (launcherTool)
        [self applyInitialPlacementToLauncherWindow:windowView];
    if (workspacesTool)
        [self.desktopSurfaceView bringSubviewToFront:windowView];
    [self refreshDockButtons];
    return windowView;
}

// The content view controller isn't stored anywhere new -- it's already a
// child view controller of self (see attachViewController:toDesktopWindow:),
// so it's found the same way UIKit finds any child: by whose view lives
// inside the window's contentContainerView.
- (nullable UIViewController *)contentViewControllerForDesktopWindow:(ISHWorkspaceContainedWindowView *)windowView {
    for (UIViewController *child in self.childViewControllers) {
        if (child.view.superview == windowView.contentContainerView)
            return child;
    }
    return nil;
}

- (void)openWorkspaceToolWithIdentifier:(NSString *)toolIdentifier fileGuestPath:(NSString *)guestPath {
    ISHWorkspaceContainedWindowView *windowView = [self openWorkspaceToolWindowWithIdentifier:toolIdentifier];
    if (windowView == nil)
        return;
    [self focusDesktopWindow:windowView];
    UIViewController *contentViewController = [self contentViewControllerForDesktopWindow:windowView];
    if ([contentViewController conformsToProtocol:@protocol(WorkspaceFileOpenable)])
        [(id<WorkspaceFileOpenable>) contentViewController workspaceOpenFileAtGuestPath:guestPath];
}

- (void)dismissSettingsToolWindow:(id)sender {
    (void) sender;
    ISHWorkspaceContainedWindowView *windowView =
        [self desktopWindowForToolIdentifier:ISHWorkspaceToolSettingsIdentifier];
    if (windowView.closeHandler != nil)
        windowView.closeHandler();
}

- (void)openDashboardWindow:(id)sender {
    if (self.dashboardWindow != nil) {
        self.dashboardWindow.hidden = NO;
        [self focusDesktopWindow:self.dashboardWindow];
    }
}

- (ISHWorkspaceContainedWindowView *)createDockWindow {
    CGSize preferredSize = ISHWorkspacePreferredDockContentSize();
    CGFloat stackInsetY = ISHWorkspaceUsesPhoneLayout() ? 2.0 : 3.0;
    CGFloat stackInsetX = ISHWorkspaceUsesPhoneLayout() ? 4.0 : 6.0;
    CGFloat tileSpacing = ISHWorkspaceUsesPhoneLayout() ? 3.0 : 4.0;
    ISHWorkspaceContainedWindowView *windowView =
        [self createDesktopWindowWithTitle:@"Dock"
                             preferredSize:preferredSize
                          showsCloseButton:NO
                    appliesInitialPlacement:NO];
    self.dockWindow = windowView;
    windowView.draggable = YES;
    windowView.resizable = YES;
    windowView.resizeHandleAtTopRight = YES;
    windowView.minimumSize = ISHWorkspaceMinimumDockContentSize();
    windowView.maximumSize = ISHWorkspaceMaximumDockContentSize();
    windowView.pinnedToBottomCenter = YES;
    __weak typeof(self) weakSelf = self;
    windowView.frameDidChangeHandler = ^{
        [weakSelf persistDockWindowFrame];
    };

    UIStackView *stack = [UIStackView new];
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 0;
    [windowView.contentContainerView addSubview:stack];

    UIStackView *appsRow = [UIStackView new];
    appsRow.axis = UILayoutConstraintAxisHorizontal;
    appsRow.spacing = tileSpacing;
    appsRow.distribution = UIStackViewDistributionFillEqually;
    self.dockUtilsButton = [self workspaceDockTileButtonWithTitle:@"Utils"
                                                         selector:@selector(toggleClockFromDock:)
                                                       identifier:@"utils"];
    self.dockUtilsButton.accessibilityHint = @"Touch and hold to show utilities menu.";
    UILongPressGestureRecognizer *utilsLongPressRecognizer =
        [[UILongPressGestureRecognizer alloc] initWithTarget:self action:@selector(handleUtilsDockLongPress:)];
    utilsLongPressRecognizer.minimumPressDuration = 0.25;
    [self.dockUtilsButton addGestureRecognizer:utilsLongPressRecognizer];
    self.dockTerminalButton = [self workspaceDockTileButtonWithTitle:@"Terminal"
                                                            selector:@selector(openOrFocusTerminalFromDock:)
                                                          identifier:ISHWorkspaceTerminalRoleSessionShell];
    self.dockTerminalButton.accessibilityHint = @"Touch and hold to show terminal options.";
    UILongPressGestureRecognizer *terminalLongPressRecognizer =
        [[UILongPressGestureRecognizer alloc] initWithTarget:self action:@selector(handleTerminalDockLongPress:)];
    terminalLongPressRecognizer.minimumPressDuration = 0.25;
    [self.dockTerminalButton addGestureRecognizer:terminalLongPressRecognizer];

    [appsRow addArrangedSubview:self.dockUtilsButton];
    [appsRow addArrangedSubview:self.dockTerminalButton];

    [stack addArrangedSubview:appsRow];

    [NSLayoutConstraint activateConstraints:@[
        [stack.topAnchor constraintEqualToAnchor:windowView.contentContainerView.topAnchor constant:stackInsetY],
        [stack.leadingAnchor constraintEqualToAnchor:windowView.contentContainerView.leadingAnchor constant:stackInsetX],
        [stack.trailingAnchor constraintEqualToAnchor:windowView.contentContainerView.trailingAnchor constant:-stackInsetX],
        [stack.bottomAnchor constraintEqualToAnchor:windowView.contentContainerView.bottomAnchor constant:-stackInsetY],
    ]];

    [self applyInitialPlacementToDockWindow:windowView];
    [self refreshDockButtons];
    return windowView;
}

- (void)saveWorkspaceLayout:(id)sender {
    NSMutableArray<NSDictionary<NSString *, id> *> *layout = [NSMutableArray array];
    for (UIView *subview in self.desktopSurfaceView.subviews) {
        if (![subview isKindOfClass:ISHWorkspaceContainedWindowView.class])
            continue;
        NSDictionary<NSString *, id> *descriptor =
            [self savedLayoutDescriptorForWindow:(ISHWorkspaceContainedWindowView *) subview];
        if (descriptor != nil)
            [layout addObject:descriptor];
    }
    if (ISHWorkspaceSupportsSceneWindows()) {
        id storedValue = [NSUserDefaults.standardUserDefaults objectForKey:ISHWorkspaceSavedLayoutDefaultsKey];
        NSMutableDictionary<NSString *, id> *layoutsByScene = [storedValue isKindOfClass:NSDictionary.class]
            ? [storedValue mutableCopy]
            : [NSMutableDictionary dictionary];
        NSString *sceneIdentifier = [self currentWorkspaceLayoutStorageIdentifier];
        if (sceneIdentifier.length == 0)
            sceneIdentifier = @"default";
        layoutsByScene[sceneIdentifier] = layout;
        [NSUserDefaults.standardUserDefaults setObject:layoutsByScene forKey:ISHWorkspaceSavedLayoutDefaultsKey];
    } else {
        [NSUserDefaults.standardUserDefaults setObject:layout forKey:ISHWorkspaceSavedLayoutDefaultsKey];
    }
}

- (void)restoreWorkspaceLayout:(id)sender {
    NSArray<NSDictionary<NSString *, id> *> *layout = [self savedWorkspaceLayoutForCurrentScene];
    if (![layout isKindOfClass:NSArray.class] || layout.count == 0) {
        UIAlertController *alert =
            [UIAlertController alertControllerWithTitle:@"No Saved Layout"
                                                message:@"Save a workspace arrangement for this workspace first, then restore it from here."
                                         preferredStyle:UIAlertControllerStyleAlert];
        [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
        [self presentViewController:alert animated:YES completion:nil];
        return;
    }

    NSDictionary<NSString *, id> *dashboardDescriptor = nil;
    NSDictionary<NSString *, id> *dockDescriptor = nil;
    NSMutableArray<NSDictionary<NSString *, id> *> *windowDescriptors = [NSMutableArray array];
    for (NSDictionary<NSString *, id> *descriptor in layout) {
        NSString *kind = descriptor[@"kind"];
        if ([kind isEqualToString:ISHWorkspaceSavedLayoutKindDashboard]) {
            dashboardDescriptor = descriptor;
        } else if ([kind isEqualToString:ISHWorkspaceSavedLayoutKindDock]) {
            dockDescriptor = descriptor;
        } else {
            [windowDescriptors addObject:descriptor];
        }
    }

    [self closeAllRestorableDesktopWindows];
    if (dashboardDescriptor != nil)
        [self applySavedDashboardDescriptor:dashboardDescriptor];
    if (dockDescriptor != nil)
        [self applySavedDockDescriptor:dockDescriptor];

    NSSet<NSString *> *deduplicatedTerminalRoles =
        [NSSet setWithArray:@[ISHWorkspaceTerminalRoleSessionShell, ISHWorkspaceTerminalRoleSystemConsole]];
    NSMutableSet<NSString *> *restoredTerminalRoles = [NSMutableSet set];
    for (NSDictionary<NSString *, id> *descriptor in windowDescriptors) {
        NSString *kind = descriptor[@"kind"];
        NSDictionary<NSString *, id> *frameDescriptor = descriptor[@"frame"];
        if ([kind isEqualToString:ISHWorkspaceSavedLayoutKindTool]) {
            NSString *toolIdentifier = descriptor[@"toolIdentifier"];
            if (toolIdentifier.length == 0)
                continue;
            // Global singletons (Desktops/Launcher): restore only the first occurrence, ignoring
            // any saved copies on later Desktops.
            if ([self isGlobalToolIdentifier:toolIdentifier] && [self desktopWindowForToolIdentifier:toolIdentifier] != nil)
                continue;
            ISHWorkspaceContainedWindowView *windowView = [self openWorkspaceToolWindowWithIdentifier:toolIdentifier];
            [self applySavedFrameDescriptor:frameDescriptor
                                   toWindow:windowView
                               fallbackSize:ISHWorkspacePreferredToolContentSize(toolIdentifier)];
            [self assignRestoredWindow:windowView toDesktopFromDescriptor:descriptor];
            NSDictionary<NSString *, id> *toolState = [descriptor[@"state"] isKindOfClass:NSDictionary.class] ? descriptor[@"state"] : nil;
            if (toolState != nil) {
                UIViewController *contentViewController = [self contentViewControllerForDesktopWindow:windowView];
                if ([contentViewController conformsToProtocol:@protocol(WorkspaceStatefulTool)])
                    [(id<WorkspaceStatefulTool>) contentViewController workspaceRestoreToolState:toolState];
            }
            continue;
        }
        if ([kind isEqualToString:ISHWorkspaceSavedLayoutKindTerminal]) {
            NSUUID *displayTerminalUUID = [[NSUUID alloc] initWithUUIDString:descriptor[@"terminalUUID"]];
            NSUUID *sessionTerminalUUID = [[NSUUID alloc] initWithUUIDString:descriptor[@"sessionTerminalUUID"]];
            if (displayTerminalUUID == nil && sessionTerminalUUID == nil)
                continue;
            Terminal *displayTerminal = displayTerminalUUID != nil ? [Terminal terminalWithUUID:displayTerminalUUID] : nil;
            NSString *terminalRole = [self terminalRoleFromSavedDescriptor:descriptor displayTerminal:displayTerminal];
            if ([deduplicatedTerminalRoles containsObject:terminalRole] && [restoredTerminalRoles containsObject:terminalRole])
                continue;
            ISHWorkspaceContainedWindowView *windowView =
                [self restoreDesktopTerminalWindowWithSessionUUID:(sessionTerminalUUID ?: displayTerminalUUID)
                                              displayTerminalUUID:displayTerminalUUID
                                                    terminalRole:terminalRole];
            if (windowView != nil) {
                if ([deduplicatedTerminalRoles containsObject:terminalRole])
                    [restoredTerminalRoles addObject:terminalRole];
                [self applySavedFrameDescriptor:frameDescriptor
                                       toWindow:windowView
                                   fallbackSize:ISHWorkspacePreferredTerminalContentSize()];
                [self assignRestoredWindow:windowView toDesktopFromDescriptor:descriptor];
                CGFloat savedFontSize = [descriptor[@"fontSize"] doubleValue];
                if (savedFontSize > 0)
                    windowView.hostedTerminalViewController.overrideFontSize = savedFontSize;
            }
        }
    }

    [self applyDesktopVisibility];
    [self ensureDefaultWorkspaceUtilitiesOpen];
    [self refreshWorkspaceStatus];
    [self applyCompactSizingToOpenWorkspaceToolWindows];
}

// Returns the saved tool-window descriptor for a tool identifier in a saved layout, or nil.
- (NSDictionary<NSString *, id> *)savedLayout:(NSArray<NSDictionary<NSString *, id> *> *)savedLayout
                    toolDescriptorForIdentifier:(NSString *)toolIdentifier {
    if (toolIdentifier.length == 0 || ![savedLayout isKindOfClass:NSArray.class])
        return nil;
    for (NSDictionary<NSString *, id> *descriptor in savedLayout) {
        if ([descriptor[@"kind"] isEqualToString:ISHWorkspaceSavedLayoutKindTool] &&
            [descriptor[@"toolIdentifier"] isEqualToString:toolIdentifier])
            return descriptor;
    }
    return nil;
}

- (ISHWorkspaceContainedWindowView *)desktopWindowDisplayingTerminalUUID:(NSUUID *)terminalUUID {
    if (terminalUUID == nil)
        return nil;

    for (UIView *view in self.desktopWindows) {
        if (![view isKindOfClass:ISHWorkspaceContainedWindowView.class])
            continue;
        ISHWorkspaceContainedWindowView *windowView = (ISHWorkspaceContainedWindowView *) view;
        TerminalViewController *terminalViewController = windowView.hostedTerminalViewController;
        if (terminalViewController == nil)
            continue;
        if ([terminalViewController.terminal.uuid isEqual:terminalUUID])
            return windowView;
    }
    return nil;
}

- (ISHWorkspaceContainedWindowView *)desktopWindowForTerminalRole:(NSString *)terminalRole {
    if (terminalRole.length == 0)
        return nil;

    for (UIView *view in self.desktopWindows) {
        if (![view isKindOfClass:ISHWorkspaceContainedWindowView.class])
            continue;
        ISHWorkspaceContainedWindowView *windowView = (ISHWorkspaceContainedWindowView *) view;
        if (windowView.hostedTerminalViewController == nil)
            continue;
        NSString *windowRole = [self persistentTerminalRoleForWindow:windowView];
        if ([windowRole isEqualToString:terminalRole])
            return windowView;
    }
    return nil;
}

- (ISHWorkspaceContainedWindowView *)frontmostDesktopTerminalWindow {
    for (UIView *view in [self.desktopSurfaceView.subviews reverseObjectEnumerator]) {
        if (![view isKindOfClass:ISHWorkspaceContainedWindowView.class])
            continue;
        ISHWorkspaceContainedWindowView *windowView = (ISHWorkspaceContainedWindowView *) view;
        if (windowView.hidden || windowView.hostedTerminalViewController == nil)
            continue;
        return windowView;
    }
    return nil;
}

- (ISHWorkspaceContainedWindowView *)desktopWindowForToolIdentifier:(NSString *)toolIdentifier {
    if (toolIdentifier.length == 0)
        return nil;

    for (UIView *view in [self.desktopSurfaceView.subviews reverseObjectEnumerator]) {
        if (![view isKindOfClass:ISHWorkspaceContainedWindowView.class])
            continue;
        ISHWorkspaceContainedWindowView *windowView = (ISHWorkspaceContainedWindowView *) view;
        // Don't skip hidden windows: in the Desktop model a tool on another Desktop is hidden,
        // not closed, and is still "open" — skipping it makes openOrFocus spawn a duplicate.
        if ([windowView.workspaceToolIdentifier isEqualToString:toolIdentifier])
            return windowView;
    }
    return nil;
}

- (ISHWorkspaceContainedWindowView *)frontmostDesktopWindowExcludingDock {
    for (UIView *view in [self.desktopSurfaceView.subviews reverseObjectEnumerator]) {
        if (![view isKindOfClass:ISHWorkspaceContainedWindowView.class])
            continue;
        ISHWorkspaceContainedWindowView *windowView = (ISHWorkspaceContainedWindowView *) view;
        if (windowView == self.dockWindow || windowView.hidden)
            continue;
        return windowView;
    }
    return nil;
}

- (void)focusDesktopWindow:(ISHWorkspaceContainedWindowView *)windowView {
    if (windowView == nil)
        return;
    [self.desktopSurfaceView bringSubviewToFront:windowView];
    [windowView.hostedTerminalViewController focusTerminal];
    [self refreshDockButtons];
}

- (BOOL)gestureRecognizer:(UIGestureRecognizer *)gestureRecognizer shouldReceiveTouch:(UITouch *)touch {
    // The desktop root menu only fires on the bare desktop, never on a window.
    if (gestureRecognizer.view == self.desktopSurfaceView)
        return touch.view == self.desktopSurfaceView || touch.view == self.desktopWallpaperView;
    return YES;
}

- (void)handleDesktopLongPress:(UILongPressGestureRecognizer *)recognizer {
    if (recognizer.state != UIGestureRecognizerStateBegan)
        return;
    // Modern-only: the root menu is part of the ctwm-style experience; Classic stays bare.
    if (!ISHWorkspaceUsesModernStyle())
        return;
    CGPoint point = [recognizer locationInView:self.desktopSurfaceView];
    [self presentDesktopRootMenuFromView:self.desktopSurfaceView sourceRect:CGRectMake(point.x, point.y, 1.0, 1.0)];
}

- (void)handleDesktopTwoFingerLongPress:(UILongPressGestureRecognizer *)recognizer {
    if (recognizer.state != UIGestureRecognizerStateBegan)
        return;
    if (!ISHWorkspaceUsesModernStyle())
        return;
    // Two fingers work anywhere, including on top of a window, so the menu is always
    // reachable even when windows cover the whole desktop.
    CGPoint point = [recognizer locationInView:self.desktopSurfaceView];
    [self presentDesktopRootMenuFromView:self.desktopSurfaceView sourceRect:CGRectMake(point.x, point.y, 1.0, 1.0)];
}

- (UIButton *)makeModernMenuPip {
    UIButton *pip = [UIButton buttonWithType:UIButtonTypeSystem];
    pip.translatesAutoresizingMaskIntoConstraints = NO;
    if (@available(iOS 13.0, *)) {
        [pip setImage:[UIImage systemImageNamed:@"line.3.horizontal"] forState:UIControlStateNormal];
    } else {
        [pip setTitle:@"☰" forState:UIControlStateNormal];
    }
    pip.tintColor = UIColor.whiteColor;
    [pip setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
    NSDictionary<NSString *, UIColor *> *pipTheme = ISHWorkspaceThemeDescriptor();
    pip.backgroundColor = pipTheme[@"accent"] ?: [UIColor colorWithRed:0.20 green:0.48 blue:0.96 alpha:1.0];
    pip.layer.cornerRadius = 22.0;
    pip.layer.borderWidth = 1.5;
    pip.layer.borderColor = [UIColor colorWithWhite:1.0 alpha:0.85].CGColor;
    pip.layer.shadowColor = UIColor.blackColor.CGColor;
    pip.layer.shadowOpacity = 0.35;
    pip.layer.shadowRadius = 6.0;
    pip.layer.shadowOffset = CGSizeMake(0.0, 2.0);
    pip.accessibilityLabel = @"Workspace menu";
    [pip addTarget:self action:@selector(menuPipTapped:) forControlEvents:UIControlEventTouchUpInside];
    pip.hidden = !ISHWorkspaceUsesModernStyle();
    return pip;
}

- (void)menuPipTapped:(UIButton *)sender {
    [self presentDesktopRootMenuFromView:sender sourceRect:sender.bounds];
}

- (void)presentIconManagerFromView:(UIView *)sourceView sourceRect:(CGRect)sourceRect {
    UIAlertController *sheet =
        [UIAlertController alertControllerWithTitle:@"Windows"
                                            message:nil
                                     preferredStyle:UIAlertControllerStyleActionSheet];

    NSUInteger listed = 0;
    for (ISHWorkspaceContainedWindowView *windowView in self.desktopWindows.copy) {
        if (![windowView isKindOfClass:ISHWorkspaceContainedWindowView.class] || windowView.hidden)
            continue;
        if (windowView == self.dashboardWindow || windowView == self.dockWindow)
            continue;
        NSString *name = windowView.titleLabel.text.length > 0 ? windowView.titleLabel.text : @"Window";
        __weak typeof(self) weakSelf = self;
        __weak typeof(windowView) weakWindow = windowView;
        [sheet addAction:[UIAlertAction actionWithTitle:name
                                                  style:UIAlertActionStyleDefault
                                                handler:^(__unused UIAlertAction *action) {
            [weakSelf focusDesktopWindow:weakWindow];
        }]];
        listed++;
    }
    if (listed == 0) {
        UIAlertAction *empty = [UIAlertAction actionWithTitle:@"No open windows" style:UIAlertActionStyleDefault handler:nil];
        empty.enabled = NO;
        [sheet addAction:empty];
    }
    [sheet addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];

    UIPopoverPresentationController *popover = sheet.popoverPresentationController;
    if (popover != nil) {
        popover.sourceView = sourceView;
        popover.sourceRect = sourceRect;
        popover.permittedArrowDirections = UIPopoverArrowDirectionAny;
    }
    [self presentViewController:sheet animated:YES completion:nil];
}

// Run a launcher shortcut: a {token} command opens the matching built-in tool or singleton
// terminal; anything else runs in a fresh terminal (an empty command just opens a terminal).
- (void)runLauncherShortcutWithCommand:(NSString *)command title:(NSString *)title {
    NSString *terminalRole = ISHWorkspaceLauncherTerminalRoleForCommand(command);
    if (terminalRole.length > 0) {
        [self openTerminalHerePreferringConsole:[terminalRole isEqualToString:ISHWorkspaceTerminalRoleSystemConsole]];
        return;
    }
    NSString *toolIdentifier = ISHWorkspaceLauncherToolIdentifierForCommand(command);
    if (toolIdentifier.length > 0) {
        [self openOrFocusWorkspaceToolIdentifier:toolIdentifier];
        return;
    }
    [self launchTerminalWithCommand:command title:title];
}

- (void)launchTerminalWithCommand:(NSString *)command {
    [self launchTerminalWithCommand:command title:nil];
}

// An empty command just opens a fresh terminal (so a shortcut named "terminal" with no
// command gives you a plain shell); a non-empty command is injected once the shell is up.
- (void)launchTerminalWithCommand:(NSString *)command title:(NSString *)title {
    NSString *windowTitle = title.length > 0 ? title : (command.length > 0 ? command : @"Terminal");
    TerminalViewController *terminalViewController = [self createDesktopTerminalViewController];
    if (terminalViewController == nil) {
        [self presentSceneActivationError:nil];
        return;
    }
    terminalViewController.freshSessionTerminalDisplayMode = ISHFreshSessionTerminalDisplayModeSessionShell;
    ISHWorkspaceContainedWindowView *windowView =
        [self openDesktopTerminalWindowWithTitle:windowTitle terminalViewController:terminalViewController];
    [terminalViewController startNewSession];
    [terminalViewController showSessionShellForCurrentSession];
    windowView.workspaceTerminalRole = ISHWorkspaceTerminalRoleGeneric;
    windowView.titleLabel.text = windowTitle;
    [self refreshDockButtons];

    if (command.length == 0)
        return;

    // Inject the command once the shell has had a moment to come up. The pty buffers
    // it, so the shell runs it as soon as it starts reading.
    Terminal *terminal = terminalViewController.terminal;
    NSData *line = [[command stringByAppendingString:@"\n"] dataUsingEncoding:NSUTF8StringEncoding];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.8 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        [terminal sendInput:line];
    });
}

- (void)presentLauncherFromView:(UIView *)sourceView sourceRect:(CGRect)sourceRect {
    [self presentLauncherFromView:sourceView sourceRect:sourceRect path:@[]];
}

// Quick-launch popup. A leaf action runs its command; a group action re-presents this same
// sheet scoped to the group's children (with a "‹ Back" action to return), so nested shortcuts
// are reachable without leaving the popup.
- (void)presentLauncherFromView:(UIView *)sourceView sourceRect:(CGRect)sourceRect path:(NSArray<NSNumber *> *)path {
    NSArray<NSDictionary<NSString *, id> *> *shortcuts = ISHWorkspaceLauncherArrayAtPath(path);
    UIAlertController *sheet =
        [UIAlertController alertControllerWithTitle:@"Launcher"
                                            message:shortcuts.count == 0 ? @"Add a shortcut to run a command in a new terminal." : nil
                                     preferredStyle:UIAlertControllerStyleActionSheet];
    [shortcuts enumerateObjectsUsingBlock:^(NSDictionary<NSString *, id> *shortcut, NSUInteger idx, __unused BOOL *stop) {
        NSString *command = shortcut[@"command"] ?: @"";
        NSString *shortcutName = shortcut[@"name"];
        NSString *name = shortcutName.length > 0 ? shortcutName
                       : (command.length > 0 ? command : @"Terminal");
        if (ISHWorkspaceShortcutIsGroup(shortcut)) {
            [sheet addAction:[UIAlertAction actionWithTitle:[name stringByAppendingString:@" ›"]
                                                      style:UIAlertActionStyleDefault
                                                    handler:^(__unused UIAlertAction *action) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    [self presentLauncherFromView:sourceView sourceRect:sourceRect
                                              path:[path arrayByAddingObject:@(idx)]];
                });
            }]];
        } else {
            [sheet addAction:[UIAlertAction actionWithTitle:name
                                                      style:UIAlertActionStyleDefault
                                                    handler:^(__unused UIAlertAction *action) {
                [self runLauncherShortcutWithCommand:command title:name];
            }]];
        }
    }];
    if (path.count > 0) {
        [sheet addAction:[UIAlertAction actionWithTitle:@"‹ Back"
                                                  style:UIAlertActionStyleDefault
                                                handler:^(__unused UIAlertAction *action) {
            dispatch_async(dispatch_get_main_queue(), ^{
                [self presentLauncherFromView:sourceView sourceRect:sourceRect
                                          path:[path subarrayWithRange:NSMakeRange(0, path.count - 1)]];
            });
        }]];
    }
    if (path.count == 0) {
        [sheet addAction:[UIAlertAction actionWithTitle:@"Show on Desktop"
                                                  style:UIAlertActionStyleDefault
                                                handler:^(__unused UIAlertAction *action) {
            [self openOrFocusWorkspaceToolIdentifier:ISHWorkspaceToolLauncherIdentifier];
        }]];
    }
    [sheet addAction:[UIAlertAction actionWithTitle:@"Edit Shortcuts…"
                                              style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction *action) {
        // Defer: presenting from a sheet handler races the sheet dismissal and
        // iOS 27 drops it (see presentDesktopRootMenuFromView).
        dispatch_async(dispatch_get_main_queue(), ^{
            [self presentLauncherEditorFromView:sourceView path:path];
        });
    }]];
    [sheet addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];

    UIPopoverPresentationController *popover = sheet.popoverPresentationController;
    if (popover != nil) {
        popover.sourceView = sourceView;
        popover.sourceRect = sourceRect;
        popover.permittedArrowDirections = UIPopoverArrowDirectionAny;
    }
    [self presentViewController:sheet animated:YES completion:nil];
}

// Lean "+" entry point (used by the Launcher applet's own Add button, alongside its Edit
// toggle): just the three add actions, scoped to `path` — no item list, unlike
// -presentLauncherEditorFromView:path: (which the popup-menu "Edit Shortcuts…" flow still uses
// and which also lists items for rename/delete — redundant with the applet's own inline
// delete/drag chrome, so the applet uses this instead).
- (void)presentAddLauncherOptionsFromView:(UIView *)sourceView path:(NSArray<NSNumber *> *)path {
    UIAlertController *sheet =
        [UIAlertController alertControllerWithTitle:@"Add"
                                            message:nil
                                     preferredStyle:UIAlertControllerStyleActionSheet];
    [sheet addAction:[UIAlertAction actionWithTitle:@"Add Shortcut…"
                                              style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction *action) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self presentAddLauncherShortcutAtPath:path];
        });
    }]];
    [sheet addAction:[UIAlertAction actionWithTitle:@"Add Built-in…"
                                              style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction *action) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self presentAddLauncherBuiltinFromView:sourceView path:path];
        });
    }]];
    [sheet addAction:[UIAlertAction actionWithTitle:@"Add Group…"
                                              style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction *action) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self presentAddLauncherGroupAtPath:path];
        });
    }]];
    [sheet addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];

    UIPopoverPresentationController *popover = sheet.popoverPresentationController;
    if (popover != nil) {
        popover.sourceView = sourceView;
        popover.sourceRect = sourceView.bounds;
        popover.permittedArrowDirections = UIPopoverArrowDirectionAny;
    }
    [self presentViewController:sheet animated:YES completion:nil];
}

- (void)presentAddLauncherShortcutAtPath:(NSArray<NSNumber *> *)path {
    UIAlertController *alert =
        [UIAlertController alertControllerWithTitle:@"Add Shortcut"
                                            message:@"A name, and a command to run in a new terminal. Leave it blank for just a terminal, or use a {token} like {clock} or {browser} to open a built-in."
                                     preferredStyle:UIAlertControllerStyleAlert];
    [alert addTextFieldWithConfigurationHandler:^(UITextField *textField) {
        textField.placeholder = @"Name (e.g. skippy or terminal)";
        textField.autocapitalizationType = UITextAutocapitalizationTypeNone;
    }];
    [alert addTextFieldWithConfigurationHandler:^(UITextField *textField) {
        textField.placeholder = @"Command (e.g. ssh skippy — blank for a shell)";
        textField.autocapitalizationType = UITextAutocapitalizationTypeNone;
        textField.autocorrectionType = UITextAutocorrectionTypeNo;
    }];
    [alert addAction:[UIAlertAction actionWithTitle:@"Save"
                                              style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction *action) {
        NSString *command = alert.textFields[1].text ?: @"";
        NSString *nameField = alert.textFields[0].text ?: @"";
        if (command.length == 0 && nameField.length == 0)
            return;
        NSString *name = nameField.length > 0 ? nameField : command;
        ISHWorkspaceLauncherMutateAtPath(path, ^(NSMutableArray<NSDictionary<NSString *, id> *> *level) {
            [level addObject:@{@"name": name, @"command": command}];
        });
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}

// A group holds its own nested list of shortcuts (or further groups) — created empty, then
// populated by drilling into it and using "Add Shortcut…"/"Add Built-in…"/"Add Group…" again.
- (void)presentAddLauncherGroupAtPath:(NSArray<NSNumber *> *)path {
    UIAlertController *alert =
        [UIAlertController alertControllerWithTitle:@"Add Group"
                                            message:@"A group holds its own list of shortcuts — e.g. \"Remote Login\" holding several hosts. Add shortcuts to it after creating it."
                                     preferredStyle:UIAlertControllerStyleAlert];
    [alert addTextFieldWithConfigurationHandler:^(UITextField *textField) {
        textField.placeholder = @"Name (e.g. Remote Login)";
        textField.autocapitalizationType = UITextAutocapitalizationTypeWords;
    }];
    [alert addAction:[UIAlertAction actionWithTitle:@"Save"
                                              style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction *action) {
        NSString *name = alert.textFields[0].text ?: @"";
        if (name.length == 0)
            return;
        ISHWorkspaceLauncherMutateAtPath(path, ^(NSMutableArray<NSDictionary<NSString *, id> *> *level) {
            [level addObject:@{@"name": name, @"children": @[]}];
        });
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}

// "Edit Shortcuts" entry point: pick a shortcut to edit/delete, or add a new one. Replaces
// the old separate Add/Remove sheets and is shared by the root-menu Launcher and the applet.
// `path` scopes this to a level of the tree (root, or a group's children when drilled in).
- (void)presentLauncherEditorFromView:(UIView *)sourceView {
    [self presentLauncherEditorFromView:sourceView path:@[]];
}

- (void)presentLauncherEditorFromView:(UIView *)sourceView path:(NSArray<NSNumber *> *)path {
    NSArray<NSDictionary<NSString *, id> *> *shortcuts = ISHWorkspaceLauncherArrayAtPath(path);
    UIAlertController *sheet =
        [UIAlertController alertControllerWithTitle:@"Edit Shortcuts"
                                            message:shortcuts.count == 0 ? @"Add a shortcut to run a command — or just open a terminal." : @"Pick a shortcut to rename, change, move, or delete."
                                     preferredStyle:UIAlertControllerStyleActionSheet];
    [shortcuts enumerateObjectsUsingBlock:^(NSDictionary<NSString *, id> *shortcut, NSUInteger idx, __unused BOOL *stop) {
        NSString *command = shortcut[@"command"];
        NSString *shortcutName = shortcut[@"name"];
        NSString *name = shortcutName.length > 0 ? shortcutName
                       : (command.length > 0 ? command : @"Terminal");
        if (ISHWorkspaceShortcutIsGroup(shortcut))
            name = [name stringByAppendingString:@" ›"];
        [sheet addAction:[UIAlertAction actionWithTitle:name
                                                  style:UIAlertActionStyleDefault
                                                handler:^(__unused UIAlertAction *action) {
            dispatch_async(dispatch_get_main_queue(), ^{
                [self presentEditLauncherShortcutAtPath:path index:idx];
            });
        }]];
    }];
    [sheet addAction:[UIAlertAction actionWithTitle:@"Add Shortcut…"
                                              style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction *action) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self presentAddLauncherShortcutAtPath:path];
        });
    }]];
    [sheet addAction:[UIAlertAction actionWithTitle:@"Add Built-in…"
                                              style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction *action) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self presentAddLauncherBuiltinFromView:sourceView path:path];
        });
    }]];
    [sheet addAction:[UIAlertAction actionWithTitle:@"Add Group…"
                                              style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction *action) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self presentAddLauncherGroupAtPath:path];
        });
    }]];
    [sheet addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];

    UIPopoverPresentationController *popover = sheet.popoverPresentationController;
    if (popover != nil) {
        popover.sourceView = sourceView;
        popover.sourceRect = sourceView.bounds;
        popover.permittedArrowDirections = UIPopoverArrowDirectionAny;
    }
    [self presentViewController:sheet animated:YES completion:nil];
}

// Discoverable companion to the {token} syntax: pick a built-in tool and it's added as a
// shortcut whose command is the matching {token}.
- (void)presentAddLauncherBuiltinFromView:(UIView *)sourceView path:(NSArray<NSNumber *> *)path {
    NSMutableArray<NSDictionary<NSString *, NSString *> *> *builtins = [@[
        @{@"name": @"New Terminal", @"command": @""},
        @{@"name": @"Session Shell", @"command": @"{shell}"},
        @{@"name": @"System Console", @"command": @"{console}"},
        @{@"name": @"Web Browser", @"command": @"{browser}"},
        @{@"name": @"Music", @"command": @"{music}"},
        @{@"name": @"MotePad", @"command": @"{motepad}"},
        @{@"name": @"File Manager", @"command": @"{files}"},
        @{@"name": @"Markdown", @"command": @"{markdown}"},
        @{@"name": @"Image Viewer", @"command": @"{imageviewer}"},
        @{@"name": @"Video Player", @"command": @"{videoplayer}"},
        @{@"name": @"Wayland", @"command": @"{display}"},
        @{@"name": @"Clock", @"command": @"{clock}"},
        @{@"name": @"Monitor", @"command": @"{monitor}"},
        @{@"name": @"Networks", @"command": @"{networks}"},
        @{@"name": @"Logs", @"command": @"{logs}"},
        @{@"name": @"Storage", @"command": @"{storage}"},
        @{@"name": @"Boot Images", @"command": @"{images}"},
        @{@"name": @"Themes", @"command": @"{themes}"},
        @{@"name": @"Sessions", @"command": @"{sessions}"},
        @{@"name": @"Diagnostics", @"command": @"{diagnostics}"},
        @{@"name": @"Settings", @"command": @"{settings}"},
    ] mutableCopy];
    if (ISHLLMClientEnabled())
        [builtins addObject:@{@"name": @"LLM Chat", @"command": @"{llm}"}];

    UIAlertController *sheet =
        [UIAlertController alertControllerWithTitle:@"Add Built-in"
                                            message:@"Open a built-in tool straight from the Launcher."
                                     preferredStyle:UIAlertControllerStyleActionSheet];
    for (NSDictionary<NSString *, NSString *> *builtin in builtins) {
        [sheet addAction:[UIAlertAction actionWithTitle:builtin[@"name"]
                                                  style:UIAlertActionStyleDefault
                                                handler:^(__unused UIAlertAction *action) {
            ISHWorkspaceLauncherMutateAtPath(path, ^(NSMutableArray<NSDictionary<NSString *, id> *> *level) {
                [level addObject:builtin];
            });
        }]];
    }
    [sheet addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];

    UIPopoverPresentationController *popover = sheet.popoverPresentationController;
    if (popover != nil) {
        popover.sourceView = sourceView;
        popover.sourceRect = sourceView.bounds;
        popover.permittedArrowDirections = UIPopoverArrowDirectionAny;
    }
    [self presentViewController:sheet animated:YES completion:nil];
}

- (void)presentEditLauncherShortcutAtPath:(NSArray<NSNumber *> *)path index:(NSUInteger)index {
    NSArray<NSDictionary<NSString *, id> *> *shortcuts = ISHWorkspaceLauncherArrayAtPath(path);
    if (index >= shortcuts.count)
        return;
    NSDictionary<NSString *, id> *shortcut = shortcuts[index];
    BOOL isGroup = ISHWorkspaceShortcutIsGroup(shortcut);

    UIAlertController *alert =
        [UIAlertController alertControllerWithTitle:isGroup ? @"Edit Group" : @"Edit Shortcut"
                                            message:isGroup ? @"Rename, move, or delete this group. Drill into it from the list to edit what's inside."
                                                             : @"Leave the command blank to just open a terminal."
                                     preferredStyle:UIAlertControllerStyleAlert];
    [alert addTextFieldWithConfigurationHandler:^(UITextField *textField) {
        textField.placeholder = @"Name";
        textField.text = shortcut[@"name"];
        textField.autocapitalizationType = UITextAutocapitalizationTypeNone;
    }];
    if (!isGroup) {
        [alert addTextFieldWithConfigurationHandler:^(UITextField *textField) {
            textField.placeholder = @"Command (blank for a shell)";
            textField.text = shortcut[@"command"];
            textField.autocapitalizationType = UITextAutocapitalizationTypeNone;
            textField.autocorrectionType = UITextAutocorrectionTypeNo;
        }];
    }
    [alert addAction:[UIAlertAction actionWithTitle:@"Save"
                                              style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction *action) {
        NSString *nameField = alert.textFields[0].text ?: @"";
        NSString *command = isGroup ? nil : (alert.textFields[1].text ?: @"");
        if (nameField.length == 0 && command.length == 0)
            return;
        NSString *name = nameField.length > 0 ? nameField : command;
        ISHWorkspaceLauncherMutateAtPath(path, ^(NSMutableArray<NSDictionary<NSString *, id> *> *level) {
            if (index >= level.count)
                return;
            if (isGroup)
                level[index] = @{@"name": name, @"children": ISHWorkspaceShortcutChildren(level[index])};
            else
                level[index] = @{@"name": name, @"command": command};
        });
    }]];
    if (index > 0) {
        [alert addAction:[UIAlertAction actionWithTitle:@"Move Up"
                                                  style:UIAlertActionStyleDefault
                                                handler:^(__unused UIAlertAction *action) {
            ISHWorkspaceLauncherMutateAtPath(path, ^(NSMutableArray<NSDictionary<NSString *, id> *> *level) {
                if (index == 0 || index >= level.count)
                    return;
                [level exchangeObjectAtIndex:index withObjectAtIndex:index - 1];
            });
        }]];
    }
    if (index + 1 < shortcuts.count) {
        [alert addAction:[UIAlertAction actionWithTitle:@"Move Down"
                                                  style:UIAlertActionStyleDefault
                                                handler:^(__unused UIAlertAction *action) {
            ISHWorkspaceLauncherMutateAtPath(path, ^(NSMutableArray<NSDictionary<NSString *, id> *> *level) {
                if (index + 1 >= level.count)
                    return;
                [level exchangeObjectAtIndex:index withObjectAtIndex:index + 1];
            });
        }]];
    }
    NSString *deleteTitle = isGroup && ISHWorkspaceShortcutChildren(shortcut).count > 0
        ? [NSString stringWithFormat:@"Delete Group and %lu Item%@", (unsigned long)ISHWorkspaceShortcutChildren(shortcut).count,
                                      ISHWorkspaceShortcutChildren(shortcut).count == 1 ? @"" : @"s"]
        : @"Delete";
    [alert addAction:[UIAlertAction actionWithTitle:deleteTitle
                                              style:UIAlertActionStyleDestructive
                                            handler:^(__unused UIAlertAction *action) {
        ISHWorkspaceLauncherMutateAtPath(path, ^(NSMutableArray<NSDictionary<NSString *, id> *> *level) {
            if (index < level.count)
                [level removeObjectAtIndex:index];
        });
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}

// Resize the open Launcher applet to match its currently displayed content (auto-size) — the
// caller passes the item count for whatever level (root or a drilled-into group) is currently
// on screen, plus whether a "‹ Back" row is showing above it.
- (void)autosizeLauncherWindowForItemCount:(NSUInteger)itemCount showsBackRow:(BOOL)showsBackRow {
    ISHWorkspaceContainedWindowView *window = [self desktopWindowForToolIdentifier:ISHWorkspaceToolLauncherIdentifier];
    if (window == nil)
        return;
    [self resizeDesktopWindow:window toSize:ISHWorkspaceLauncherContentSize(itemCount, showsBackRow) animated:YES];
}

- (void)autosizeMonitorWindow {
    ISHWorkspaceContainedWindowView *window = [self desktopWindowForToolIdentifier:ISHWorkspaceToolMonitorIdentifier];
    if (window == nil)
        return;
    [self resizeDesktopWindow:window toSize:ISHWorkspaceMonitorContentSize() animated:YES];
}

// In-app Desktops: a Desktop is a set of contained windows sharing a workspaceDesktopIndex.
// Only the active Desktop's windows are visible; switching just shows/hides by index (the dock
// and Layout Manager are global chrome and stay put). Terminals keep running while hidden.
// The Desktops applet and the Launcher are global singletons — one window shared by every
// Desktop, never duplicated, so their state (launcher items, the Desktop list) stays consistent.
- (BOOL)isGlobalToolIdentifier:(NSString *)toolIdentifier {
    return [toolIdentifier isEqualToString:ISHWorkspaceToolWorkspacesIdentifier] ||
           [toolIdentifier isEqualToString:ISHWorkspaceToolLauncherIdentifier];
}

// Defensive: if more than one window exists for a global tool — duplicates created before the
// singleton guard, or restored from an old layout that had them on several Desktops — keep the
// first and close the rest so the Launcher / Desktops applet is genuinely one shared instance.
- (void)collapseGlobalToolDuplicates {
    for (NSString *toolIdentifier in @[ISHWorkspaceToolWorkspacesIdentifier, ISHWorkspaceToolLauncherIdentifier]) {
        ISHWorkspaceContainedWindowView *kept = nil;
        for (UIView *view in self.desktopWindows.copy) {
            if (![view isKindOfClass:ISHWorkspaceContainedWindowView.class])
                continue;
            ISHWorkspaceContainedWindowView *windowView = (ISHWorkspaceContainedWindowView *) view;
            if (![windowView.workspaceToolIdentifier isEqualToString:toolIdentifier])
                continue;
            if (kept == nil) {
                kept = windowView;
                windowView.workspaceDesktopIndex = 0;
                windowView.hidden = NO;
            } else if (windowView.closeHandler != nil) {
                windowView.closeHandler();
            }
        }
    }
}

- (void)applyDesktopVisibility {
    [self collapseGlobalToolDuplicates];
    for (UIView *view in self.desktopWindows) {
        if (![view isKindOfClass:ISHWorkspaceContainedWindowView.class])
            continue;
        ISHWorkspaceContainedWindowView *windowView = (ISHWorkspaceContainedWindowView *) view;
        if (windowView == self.dockWindow || windowView == self.dashboardWindow)
            continue;
        // Global chrome appears on every Desktop, brought to front so it isn't covered.
        if ([self isGlobalToolIdentifier:windowView.workspaceToolIdentifier]) {
            windowView.hidden = NO;
            [self.desktopSurfaceView bringSubviewToFront:windowView];
            continue;
        }
        windowView.hidden = (windowView.workspaceDesktopIndex != self.activeDesktopIndex);
    }
}

// Put a restored window back on its saved Desktop, growing the Desktop count to fit.
- (void)assignRestoredWindow:(ISHWorkspaceContainedWindowView *)windowView toDesktopFromDescriptor:(NSDictionary<NSString *, id> *)descriptor {
    if (windowView == nil)
        return;
    if ([self isGlobalToolIdentifier:windowView.workspaceToolIdentifier]) {
        windowView.workspaceDesktopIndex = 0;  // global singletons aren't tied to a Desktop
        return;
    }
    NSInteger index = MAX((NSInteger)0, [descriptor[@"desktopIndex"] integerValue]);
    windowView.workspaceDesktopIndex = index;
    if (index + 1 > self.desktopCount)
        self.desktopCount = index + 1;
}

- (void)postDesktopsDidChange {
    [NSNotificationCenter.defaultCenter postNotificationName:ISHWorkspaceDesktopsDidChangeNotification object:self];
}

- (void)switchToDesktopIndex:(NSInteger)index {
    index = MAX((NSInteger)0, MIN(index, self.desktopCount - 1));
    if (index == self.activeDesktopIndex)
        return;
    self.activeDesktopIndex = index;
    [self applyDesktopVisibility];
    [self showDesktopIndicator];
    [self postDesktopsDidChange];
}

- (void)createNewDesktop {
    self.desktopCount += 1;
    [self switchToDesktopIndex:self.desktopCount - 1];
    [self postDesktopsDidChange];
}

- (void)removeDesktopAtIndex:(NSInteger)indexToRemove {
    if (self.desktopCount <= 1)
        return;
    indexToRemove = MAX((NSInteger)0, MIN(indexToRemove, self.desktopCount - 1));
    // A locked Desktop can't be removed.
    if ([self isDesktopLockedAtIndex:indexToRemove])
        return;
    // Close the removed Desktop's windows; shift higher Desktops down. Global tools stay put.
    for (UIView *view in self.desktopWindows.copy) {
        if (![view isKindOfClass:ISHWorkspaceContainedWindowView.class])
            continue;
        ISHWorkspaceContainedWindowView *windowView = (ISHWorkspaceContainedWindowView *) view;
        if (windowView == self.dockWindow || windowView == self.dashboardWindow)
            continue;
        if ([self isGlobalToolIdentifier:windowView.workspaceToolIdentifier])
            continue;
        if (windowView.workspaceDesktopIndex == indexToRemove) {
            if (windowView.closeHandler != nil)
                windowView.closeHandler();
        } else if (windowView.workspaceDesktopIndex > indexToRemove) {
            windowView.workspaceDesktopIndex -= 1;
        }
    }
    // Renumber locked Desktops to match the shift (the removed index is never locked).
    if (_lockedDesktopIndices.count > 0) {
        NSMutableIndexSet *shifted = [NSMutableIndexSet indexSet];
        [_lockedDesktopIndices enumerateIndexesUsingBlock:^(NSUInteger i, BOOL *stop) {
            (void) stop;
            if ((NSInteger)i > indexToRemove)
                [shifted addIndex:i - 1];
            else if ((NSInteger)i < indexToRemove)
                [shifted addIndex:i];
        }];
        _lockedDesktopIndices = shifted;
    }
    self.desktopCount -= 1;
    if (self.activeDesktopIndex >= self.desktopCount)
        self.activeDesktopIndex = self.desktopCount - 1;
    else if (self.activeDesktopIndex > indexToRemove)
        self.activeDesktopIndex -= 1;
    [self applyDesktopVisibility];
    [self postDesktopsDidChange];
}

- (NSMutableIndexSet *)lockedDesktopIndices {
    if (_lockedDesktopIndices == nil)
        _lockedDesktopIndices = [NSMutableIndexSet indexSet];
    return _lockedDesktopIndices;
}

- (BOOL)isDesktopLockedAtIndex:(NSInteger)index {
    if (index == 0)
        return YES;  // the first Desktop is permanently protected and can't be removed
    return index >= 0 && [self.lockedDesktopIndices containsIndex:(NSUInteger)index];
}

- (void)toggleDesktopLockAtIndex:(NSInteger)index {
    if (index <= 0 || index >= self.desktopCount)
        return;  // the first Desktop is always locked; only Desktops 2+ can be toggled
    if ([self.lockedDesktopIndices containsIndex:(NSUInteger)index])
        [self.lockedDesktopIndices removeIndex:(NSUInteger)index];
    else
        [self.lockedDesktopIndices addIndex:(NSUInteger)index];
    [self postDesktopsDidChange];
}

- (void)handleDesktopSwitchSwipe:(UISwipeGestureRecognizer *)recognizer {
    NSInteger delta = recognizer.direction == UISwipeGestureRecognizerDirectionLeft ? 1 : -1;
    [self switchToDesktopIndex:self.activeDesktopIndex + delta];
}

// The line (paragraph, hard-newline-delimited) containing `index` — mirrors the same convention
// MotePad's own "Ln/Col" status bar uses (see -updateStatusBar), not the visually-wrapped line.
static NSRange ISHWorkspaceLineRangeContainingIndex(NSString *text, NSUInteger index) {
    NSUInteger length = text.length;
    index = MIN(index, length);
    NSUInteger start = index;
    while (start > 0 && [text characterAtIndex:start - 1] != '\n')
        start--;
    NSUInteger end = index;
    while (end < length && [text characterAtIndex:end] != '\n')
        end++;
    return NSMakeRange(start, end - start);
}

// Cmd+Arrow, not Ctrl+Arrow: the guest terminal already claims Ctrl/Shift/Alt+Arrow (and every
// other modifier combo on arrows) for escape-sequence passthrough — e.g. Ctrl-Left/Right is a
// shell word-jump. Cmd+Arrow is the one combination arrows don't already use anywhere in the app.
//
// When a text-editing view has focus (MotePad today, any future text-input applet for free),
// this moves the cursor to the start/end of the current line instead of switching Desktops —
// the standard text-editing convention for Cmd+Arrow, reimplemented here rather than left to
// iOS's own default handling. -keyCommands always registers these two with system priority
// regardless of what's focused (see there for why): the branch lives here in the action instead.
- (void)hotkeyPreviousDesktop:(UIKeyCommand *)command {
    UIView *responder = ISHWorkspaceFindFirstResponder(self.view);
    if ([responder isKindOfClass:UITextView.class]) {
        UITextView *textView = (UITextView *)responder;
        NSRange line = ISHWorkspaceLineRangeContainingIndex(textView.text, textView.selectedRange.location);
        textView.selectedRange = NSMakeRange(line.location, 0);
        return;
    }
    [self switchToDesktopIndex:self.activeDesktopIndex - 1];
}

- (void)hotkeyNextDesktop:(UIKeyCommand *)command {
    UIView *responder = ISHWorkspaceFindFirstResponder(self.view);
    if ([responder isKindOfClass:UITextView.class]) {
        UITextView *textView = (UITextView *)responder;
        NSRange line = ISHWorkspaceLineRangeContainingIndex(textView.text, textView.selectedRange.location);
        textView.selectedRange = NSMakeRange(NSMaxRange(line), 0);
        return;
    }
    [self switchToDesktopIndex:self.activeDesktopIndex + 1];
}

// Ctrl+Tab / Ctrl+Shift+Tab: cycle which applet window is frontmost on the
// active Desktop. Forward rotates the current frontmost to the back of the
// window stack (kept above the wallpaper and other non-window subviews) and
// activates the next one, so repeated presses visit every open window;
// Shift reverses (the backmost window comes forward). Activation goes
// through the window's own bringWindowToFront -- the same path a tap uses --
// so focus hand-off (didBecomeFrontmostHandler) and the dock highlight stay
// consistent with pointer interaction. Ctrl rather than Cmd: Cmd+Tab is the
// iOS app switcher (system-reserved, unwinnable), and the guest terminal
// doesn't claim Ctrl+Tab (plain Tab passes through; the RFB applet registers
// unmodified \t only).
- (void)hotkeyCycleWindows:(UIKeyCommand *)command {
    NSMutableArray<ISHWorkspaceContainedWindowView *> *windows = [NSMutableArray array];
    for (UIView *view in self.desktopSurfaceView.subviews) {
        if (![view isKindOfClass:ISHWorkspaceContainedWindowView.class])
            continue;
        ISHWorkspaceContainedWindowView *windowView = (ISHWorkspaceContainedWindowView *) view;
        if (windowView == self.dockWindow || windowView.hidden)
            continue;
        [windows addObject:windowView];
    }
    if (windows.count < 2)
        return;
    if ((command.modifierFlags & UIKeyModifierShift) != 0) {
        [windows.firstObject bringWindowToFront];
    } else {
        [self.desktopSurfaceView insertSubview:windows.lastObject belowSubview:windows.firstObject];
        [windows[windows.count - 2] bringWindowToFront];
    }
}

- (NSArray<UIKeyCommand *> *)keyCommands {
    BOOL textEditing = [ISHWorkspaceFindFirstResponder(self.view) isKindOfClass:UITextView.class];
    UIKeyCommand *previous = [UIKeyCommand keyCommandWithInput:UIKeyInputLeftArrow
                                                 modifierFlags:UIKeyModifierCommand
                                                        action:@selector(hotkeyPreviousDesktop:)
                                          discoverabilityTitle:textEditing ? @"Move to Start of Line" : @"Previous Desktop"];
    UIKeyCommand *next = [UIKeyCommand keyCommandWithInput:UIKeyInputRightArrow
                                             modifierFlags:UIKeyModifierCommand
                                                    action:@selector(hotkeyNextDesktop:)
                                      discoverabilityTitle:textEditing ? @"Move to End of Line" : @"Next Desktop"];
    UIKeyCommand *cycleForward = [UIKeyCommand keyCommandWithInput:@"\t"
                                                     modifierFlags:UIKeyModifierControl
                                                            action:@selector(hotkeyCycleWindows:)
                                              discoverabilityTitle:@"Next Window"];
    UIKeyCommand *cycleBackward = [UIKeyCommand keyCommandWithInput:@"\t"
                                                      modifierFlags:UIKeyModifierControl | UIKeyModifierShift
                                                             action:@selector(hotkeyCycleWindows:)
                                               discoverabilityTitle:@"Previous Window"];
    // Without this, iOS's own system-reserved default for Cmd+Arrow silently wins over the app:
    // the command still shows up in the Cmd-hold discoverability HUD (which lists everything
    // registered, not just what wins), but the action never fires. Same fix TerminalView.m
    // already applies to its own keys.
    //
    // Crucially, these two commands are ALWAYS returned here, never conditionally omitted — only
    // the discoverability title varies with focus. Omitting them entirely while MotePad's text
    // view had focus (the first cut of this fix) left Cmd+Arrow permanently defeated by the
    // system default afterward, even once MotePad closed, until the app was relaunched: iOS
    // appears to decide app-vs-system priority for a given key command once per responder
    // instance and stick with it, rather than re-deciding on every query. Keeping presence and
    // the priority flag constant sidesteps that; see the actions above for the actual branch.
    if (@available(iOS 15, *)) {
        previous.wantsPriorityOverSystemBehavior = YES;
        next.wantsPriorityOverSystemBehavior = YES;
        // Ctrl+Tab is iOS's own focus-navigation chord in some contexts;
        // claim it for window cycling the same way (and per the same
        // always-present rule documented above).
        cycleForward.wantsPriorityOverSystemBehavior = YES;
        cycleBackward.wantsPriorityOverSystemBehavior = YES;
    }
    return @[previous, next, cycleForward, cycleBackward];
}

// A brief "Desktop N / M" toast so the swipe-only switch stays oriented.
- (void)showDesktopIndicator {
    if (self.desktopIndicatorLabel == nil) {
        UILabel *label = [UILabel new];
        label.translatesAutoresizingMaskIntoConstraints = NO;
        label.textAlignment = NSTextAlignmentCenter;
        label.font = [UIFont systemFontOfSize:15.0 weight:UIFontWeightSemibold];
        label.textColor = UIColor.whiteColor;
        label.backgroundColor = [UIColor colorWithWhite:0.0 alpha:0.55];
        label.layer.cornerRadius = 14.0;
        label.layer.masksToBounds = YES;
        label.userInteractionEnabled = NO;
        [self.view addSubview:label];
        [NSLayoutConstraint activateConstraints:@[
            [label.centerXAnchor constraintEqualToAnchor:self.view.centerXAnchor],
            [label.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor constant:12.0],
            [label.heightAnchor constraintEqualToConstant:28.0],
            [label.widthAnchor constraintGreaterThanOrEqualToConstant:130.0],
        ]];
        self.desktopIndicatorLabel = label;
    }
    self.desktopIndicatorLabel.text =
        [NSString stringWithFormat:@"  Desktop %ld / %ld  ", (long)(self.activeDesktopIndex + 1), (long)self.desktopCount];
    [self.view bringSubviewToFront:self.desktopIndicatorLabel];
    self.desktopIndicatorLabel.hidden = NO;
    self.desktopIndicatorLabel.alpha = 1.0;
    [UIView animateWithDuration:0.3 delay:0.7 options:UIViewAnimationOptionBeginFromCurrentState animations:^{
        self.desktopIndicatorLabel.alpha = 0.0;
    } completion:nil];
}

- (void)presentDesktopRootMenuFromView:(UIView *)sourceView sourceRect:(CGRect)sourceRect {
    UIAlertController *sheet =
        [UIAlertController alertControllerWithTitle:@"Workspace"
                                            message:nil
                                     preferredStyle:UIAlertControllerStyleActionSheet];

    [sheet addAction:[UIAlertAction actionWithTitle:@"New Terminal"
                                              style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction *action) {
        [self openDesktopTerminalHerePreferringConsole:NO
                                         reuseExisting:NO
                                       trackPrimaryRole:NO];
    }]];
    [sheet addAction:[UIAlertAction actionWithTitle:@"Terminal…"
                                              style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction *action) {
        // Any item that presents another controller (nested sheet, Launcher,
        // Windows, Utilities, Style chooser) must defer to the next runloop:
        // presenting synchronously from a UIAlertAction handler races this
        // sheet's own dismissal and UIKit drops it as "already presenting", so
        // the item silently does nothing. Used to bite only iPad popovers; iOS
        // 27 makes it consistent (iPhone action sheets too).
        dispatch_async(dispatch_get_main_queue(), ^{
            [self presentTerminalDockActionsFromView:sourceView];
        });
    }]];
    [sheet addAction:[UIAlertAction actionWithTitle:@"Launcher"
                                              style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction *action) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self presentLauncherFromView:sourceView sourceRect:sourceRect];
        });
    }]];
    [sheet addAction:[UIAlertAction actionWithTitle:@"Windows"
                                              style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction *action) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self presentIconManagerFromView:sourceView sourceRect:sourceRect];
        });
    }]];
    [sheet addAction:[UIAlertAction actionWithTitle:@"New Desktop"
                                              style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction *action) {
        [self createNewDesktop];
    }]];
    [sheet addAction:[UIAlertAction actionWithTitle:@"Utilities…"
                                              style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction *action) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self presentUtilsDockActionsFromView:sourceView];
        });
    }]];
    BOOL autoShowKeyboard = UserPreferences.shared.autoShowKeyboard;
    [sheet addAction:[UIAlertAction actionWithTitle:[NSString stringWithFormat:@"Auto-Show Keyboard: %@", autoShowKeyboard ? @"On" : @"Off"]
                                              style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction *action) {
        UserPreferences.shared.autoShowKeyboard = !autoShowKeyboard;
        // Re-present so the toggled state is reflected; see the "Terminal…"
        // handler above for why this must defer to the next runloop.
        dispatch_async(dispatch_get_main_queue(), ^{
            [self presentDesktopRootMenuFromView:sourceView sourceRect:sourceRect];
        });
    }]];
    [sheet addAction:[UIAlertAction actionWithTitle:@"Settings"
                                              style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction *action) {
        [self openOrFocusWorkspaceToolIdentifier:ISHWorkspaceToolSettingsIdentifier];
    }]];
    [sheet addAction:[UIAlertAction actionWithTitle:@"Workspace Style…"
                                              style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction *action) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self presentWorkspaceStyleChooserFromView:sourceView];
        });
    }]];
    [sheet addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];

    UIPopoverPresentationController *popover = sheet.popoverPresentationController;
    if (popover != nil) {
        popover.sourceView = sourceView;
        popover.sourceRect = sourceRect;
        popover.permittedArrowDirections = UIPopoverArrowDirectionAny;
    }
    [self presentViewController:sheet animated:YES completion:nil];
}

// Reached by long-pressing the terminal accessory bar's 4-way arrow key in place (see
// ArrowBarButton.longPressHandler) — the keyboard-adjacent equivalent of the Cmd+Arrow hotkeys
// and the two-finger swipe, for when reaching either of those isn't convenient.
- (void)presentDesktopSwitchMenuFromView:(UIView *)sourceView sourceRect:(CGRect)sourceRect {
    if (self.desktopCount <= 1)
        return;

    UIAlertController *sheet =
        [UIAlertController alertControllerWithTitle:@"Switch Desktop"
                                            message:nil
                                     preferredStyle:UIAlertControllerStyleActionSheet];

    if (self.activeDesktopIndex > 0) {
        [sheet addAction:[UIAlertAction actionWithTitle:@"Previous Desktop"
                                                  style:UIAlertActionStyleDefault
                                                handler:^(__unused UIAlertAction *action) {
            [self switchToDesktopIndex:self.activeDesktopIndex - 1];
        }]];
    }
    if (self.activeDesktopIndex < self.desktopCount - 1) {
        [sheet addAction:[UIAlertAction actionWithTitle:@"Next Desktop"
                                                  style:UIAlertActionStyleDefault
                                                handler:^(__unused UIAlertAction *action) {
            [self switchToDesktopIndex:self.activeDesktopIndex + 1];
        }]];
    }
    for (NSInteger index = 0; index < self.desktopCount; index++) {
        if (index == self.activeDesktopIndex)
            continue;
        NSString *title = [NSString stringWithFormat:@"Desktop %ld", (long)(index + 1)];
        [sheet addAction:[UIAlertAction actionWithTitle:title
                                                  style:UIAlertActionStyleDefault
                                                handler:^(__unused UIAlertAction *action) {
            [self switchToDesktopIndex:index];
        }]];
    }
    [sheet addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];

    UIPopoverPresentationController *popover = sheet.popoverPresentationController;
    if (popover != nil) {
        popover.sourceView = sourceView;
        popover.sourceRect = sourceRect;
        popover.permittedArrowDirections = UIPopoverArrowDirectionAny;
    }
    [self presentViewController:sheet animated:YES completion:nil];
}

- (ISHWorkspaceContainedWindowView *)desktopWindowHostingTerminalUUID:(NSUUID *)terminalUUID {
    if (terminalUUID == nil)
        return nil;

    for (UIView *view in self.desktopWindows) {
        if (![view isKindOfClass:ISHWorkspaceContainedWindowView.class])
            continue;
        ISHWorkspaceContainedWindowView *windowView = (ISHWorkspaceContainedWindowView *) view;
        TerminalViewController *terminalViewController = windowView.hostedTerminalViewController;
        if (terminalViewController == nil)
            continue;
        if ([terminalViewController.sessionTerminalUUID isEqual:terminalUUID])
            return windowView;
        if ([terminalViewController.terminal.uuid isEqual:terminalUUID])
            return windowView;
    }
    return nil;
}

- (TerminalViewController *)createDesktopTerminalViewController {
    TerminalViewController *terminalViewController =
        (TerminalViewController *) [[UIStoryboard storyboardWithName:@"Terminal" bundle:nil] instantiateInitialViewController];
    if (![terminalViewController isKindOfClass:TerminalViewController.class])
        return nil;
    if (@available(iOS 13.0, *)) {
        terminalViewController.sceneSession = self.view.window.windowScene.session;
    }
    terminalViewController.showsWorkspaceDashboardButton = NO;
    terminalViewController.embeddedInWorkspaceWindow = YES;
    return terminalViewController;
}

- (ISHWorkspaceContainedWindowView *)openDesktopTerminalWindowWithTitle:(NSString *)title
                                                 terminalViewController:(TerminalViewController *)terminalViewController {
    CGSize preferredSize = ISHWorkspacePreferredTerminalContentSize();
    terminalViewController.preferredContentSize = preferredSize;
    // Default open: a window scaled to orientation so terminals don't dominate. In landscape,
    // half the usable width x half the height (a quadrant); in portrait, the full usable width
    // x 1/4 the height (a wide strip). Both iPad and iPhone. Clamped/placed by
    // desktopFrameForWindowWithPreferredSize; the window stays user-resizable down to
    // ISHWorkspaceMinimumTerminalContentSize.
    CGSize windowSize = preferredSize;
    CGRect usable = [self desktopUsableBounds];
    if (CGRectGetWidth(usable) > 1.0 && CGRectGetHeight(usable) > 1.0) {
        BOOL landscape = CGRectGetWidth(usable) >= CGRectGetHeight(usable);
        windowSize = landscape
            ? CGSizeMake(CGRectGetWidth(usable) / 2.0, CGRectGetHeight(usable) / 2.0)
            : CGSizeMake(CGRectGetWidth(usable), CGRectGetHeight(usable) / 4.0);
    }
    ISHWorkspaceContainedWindowView *windowView =
        [self createDesktopWindowWithTitle:title
                             preferredSize:windowSize
                          showsCloseButton:YES];
    windowView.hostedTerminalViewController = terminalViewController;
    windowView.resizable = YES;
    windowView.titleBarDoubleTapZoomEnabled = YES;
    windowView.minimumSize = ISHWorkspaceMinimumTerminalContentSize();
    [self attachViewController:terminalViewController toDesktopWindow:windowView];

    // A workspace terminal window owns its shell session (desktop-style): closing the window
    // ends the shell, and an exiting shell ("exit") closes the window instead of relaunching the
    // login shell. Both converge on the same teardown the chrome × uses.
    //   - Wrap the generic close handler set by attachViewController: so the × (and any
    //     programmatic close) first disposes the session, then tears the window down.
    //   - Hand the terminal a workspaceSessionDidEndHandler so a shell exit routes back through
    //     that same close handler rather than respawning.
    dispatch_block_t genericCloseHandler = windowView.closeHandler;
    __weak typeof(terminalViewController) weakTerminalViewController = terminalViewController;
    windowView.closeHandler = ^{
        [weakTerminalViewController disposeSessionForWorkspaceClose];
        if (genericCloseHandler != nil)
            genericCloseHandler();
    };
    __weak typeof(windowView) weakWindowView = windowView;
    terminalViewController.workspaceSessionDidEndHandler = ^{
        ISHWorkspaceContainedWindowView *strongWindowView = weakWindowView;
        if (strongWindowView.closeHandler != nil)
            strongWindowView.closeHandler();
    };
    return windowView;
}

- (void)workspaceClearArrangedSubviewsFromStack:(UIStackView *)stackView {
    NSArray<UIView *> *arrangedSubviews = stackView.arrangedSubviews.copy;
    for (UIView *view in arrangedSubviews) {
        [stackView removeArrangedSubview:view];
        [view removeFromSuperview];
    }
}

- (void)rebuildWorkspaceColumns {
    [self workspaceClearArrangedSubviewsFromStack:self.bodyStack];
    self.bodyStack.axis = UILayoutConstraintAxisVertical;
    self.bodyStack.spacing = ISHWorkspaceUsesPhoneLayout() ? 14.0 : 20.0;
    self.bodyStack.alignment = UIStackViewAlignmentFill;
    self.bodyStack.distribution = UIStackViewDistributionFill;
    if (self.windowCard != nil)
        [self.bodyStack addArrangedSubview:self.windowCard];
}

- (void)applyWorkspaceDesktopBackground {
    self.modernMenuPip.hidden = !ISHWorkspaceUsesModernStyle();
    // Modern hides the dock and the standalone Layout Manager — replaced by the root menu
    // and the Workspaces applet (which now carries Save/Restore).
    self.dockWindow.hidden = ISHWorkspaceUsesModernStyle();
    if (ISHWorkspaceUsesModernStyle())
        self.dashboardWindow.hidden = YES;
    if (ISHWorkspaceUsesModernStyle()) {
        // Modern: a calm flat desktop that follows the user's light/dark choice,
        // so the whole canvas changes — not just the window frames.
        self.desktopSurfaceView.backgroundColor = UserPreferences.shared.requestingDarkAppearance
            ? [UIColor colorWithRed:0.07 green:0.08 blue:0.11 alpha:1.0]
            : [UIColor colorWithRed:0.92 green:0.93 blue:0.96 alpha:1.0];
    } else if (@available(iOS 13.0, *)) {
        self.desktopSurfaceView.backgroundColor = [UIColor.systemGroupedBackgroundColor colorWithAlphaComponent:1.0];
    } else {
        self.desktopSurfaceView.backgroundColor = [UIColor colorWithWhite:0.92 alpha:1.0];
    }
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Desktop";
    if (@available(iOS 13.0, *)) {
        self.view.backgroundColor = UIColor.systemBackgroundColor;
    } else {
        self.view.backgroundColor = UIColor.whiteColor;
    }
    self.desktopWindows = [NSMutableArray array];
    self.activeDesktopIndex = 0;
    self.desktopCount = MAX((NSInteger)1, UserPreferences.shared.workspaceLaunchCount);

    self.desktopSurfaceView = [UIView new];
    self.desktopSurfaceView.translatesAutoresizingMaskIntoConstraints = NO;
    [self applyWorkspaceDesktopBackground];
    [self.view addSubview:self.desktopSurfaceView];

    UILongPressGestureRecognizer *desktopRootMenuRecognizer =
        [[UILongPressGestureRecognizer alloc] initWithTarget:self action:@selector(handleDesktopLongPress:)];
    desktopRootMenuRecognizer.minimumPressDuration = 0.4;
    desktopRootMenuRecognizer.delegate = self;
    [self.desktopSurfaceView addGestureRecognizer:desktopRootMenuRecognizer];

    UILongPressGestureRecognizer *desktopTwoFingerMenuRecognizer =
        [[UILongPressGestureRecognizer alloc] initWithTarget:self action:@selector(handleDesktopTwoFingerLongPress:)];
    desktopTwoFingerMenuRecognizer.minimumPressDuration = 0.4;
    desktopTwoFingerMenuRecognizer.numberOfTouchesRequired = 2;
    [self.view addGestureRecognizer:desktopTwoFingerMenuRecognizer];

    // Two-finger horizontal swipe switches between in-app Desktops.
    for (NSNumber *direction in @[@(UISwipeGestureRecognizerDirectionLeft), @(UISwipeGestureRecognizerDirectionRight)]) {
        UISwipeGestureRecognizer *swipe =
            [[UISwipeGestureRecognizer alloc] initWithTarget:self action:@selector(handleDesktopSwitchSwipe:)];
        swipe.direction = (UISwipeGestureRecognizerDirection) direction.unsignedIntegerValue;
        swipe.numberOfTouchesRequired = 2;
        [self.view addGestureRecognizer:swipe];
    }

    self.modernMenuPip = [self makeModernMenuPip];
    [self.view addSubview:self.modernMenuPip];
    [NSLayoutConstraint activateConstraints:@[
        [self.modernMenuPip.trailingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.trailingAnchor constant:-16.0],
        [self.modernMenuPip.bottomAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.bottomAnchor constant:-16.0],
        [self.modernMenuPip.widthAnchor constraintEqualToConstant:44.0],
        [self.modernMenuPip.heightAnchor constraintEqualToConstant:44.0],
    ]];

    [NSLayoutConstraint activateConstraints:@[
        [self.desktopSurfaceView.topAnchor constraintEqualToAnchor:self.view.topAnchor],
        [self.desktopSurfaceView.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
        [self.desktopSurfaceView.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
        [self.desktopSurfaceView.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor],
    ]];

    self.desktopWallpaperView = [UIImageView new];
    self.desktopWallpaperView.translatesAutoresizingMaskIntoConstraints = NO;
    self.desktopWallpaperView.contentMode = UIViewContentModeScaleAspectFill;
    self.desktopWallpaperView.clipsToBounds = YES;
    self.desktopWallpaperView.hidden = YES;
    [self.desktopSurfaceView addSubview:self.desktopWallpaperView];
    [NSLayoutConstraint activateConstraints:@[
        [self.desktopWallpaperView.topAnchor constraintEqualToAnchor:self.desktopSurfaceView.topAnchor],
        [self.desktopWallpaperView.leadingAnchor constraintEqualToAnchor:self.desktopSurfaceView.leadingAnchor],
        [self.desktopWallpaperView.trailingAnchor constraintEqualToAnchor:self.desktopSurfaceView.trailingAnchor],
        [self.desktopWallpaperView.bottomAnchor constraintEqualToAnchor:self.desktopSurfaceView.bottomAnchor],
    ]];

    ISHWorkspaceContainedWindowView *dashboardWindow =
        [self createDesktopWindowWithTitle:@"Layout Manager"
                             preferredSize:ISHWorkspacePreferredDashboardContentSize()
                          showsCloseButton:YES];
    self.dashboardWindow = dashboardWindow;
    dashboardWindow.draggable = YES;
    dashboardWindow.resizable = YES;
    dashboardWindow.minimumSize = ISHWorkspaceMinimumDashboardContentSize();
    __weak typeof(self) weakSelf = self;
    __weak typeof(dashboardWindow) weakDashboardWindow = dashboardWindow;
    dashboardWindow.closeHandler = ^{
        weakDashboardWindow.hidden = YES;
        [weakSelf refreshDockButtons];
    };
    [self createDockWindow];
    self.dockWindow.hidden = ISHWorkspaceUsesModernStyle();
    if (ISHWorkspaceUsesModernStyle())
        self.dashboardWindow.hidden = YES;

    UIScrollView *scrollView = [UIScrollView new];
    scrollView.translatesAutoresizingMaskIntoConstraints = NO;
    [dashboardWindow.contentContainerView addSubview:scrollView];

    UIStackView *contentStack = [UIStackView new];
    contentStack.axis = UILayoutConstraintAxisVertical;
    contentStack.spacing = ISHWorkspaceUsesPhoneLayout() ? 14.0 : 20.0;
    contentStack.translatesAutoresizingMaskIntoConstraints = NO;
    [scrollView addSubview:contentStack];

    UIStackView *windowCardStack = nil;
    self.windowCard = [self workspaceCardWithContentStack:&windowCardStack];
    self.layoutManagerWorkspaceLabel = [self workspaceLabelWithTextStyle:UIFontTextStyleFootnote monospaced:NO];
    self.layoutManagerWorkspaceLabel.numberOfLines = 2;
    [windowCardStack addArrangedSubview:self.layoutManagerWorkspaceLabel];
    [windowCardStack addArrangedSubview:[self workspaceActionButtonWithTitle:@"Save Current Layout"
                                                                    selector:@selector(saveWorkspaceLayout:)]];
    [windowCardStack addArrangedSubview:[self workspaceActionButtonWithTitle:@"Restore Saved Layout"
                                                                   selector:@selector(restoreWorkspaceLayout:)]];
    if (ISHWorkspaceSupportsSceneWindows() && !ISHWorkspaceUsesModernStyle()) {
        [windowCardStack addArrangedSubview:[self workspaceActionButtonWithTitle:@"New Workspace Window"
                                                                       selector:@selector(openNewWorkspaceWindow:)]];
    }

    self.bodyStack = [UIStackView new];
    self.bodyStack.translatesAutoresizingMaskIntoConstraints = NO;
    [contentStack addArrangedSubview:self.bodyStack];
    [self rebuildWorkspaceColumns];

    UILayoutGuide *safeArea = dashboardWindow.contentContainerView.safeAreaLayoutGuide;
    CGFloat contentInsetY = ISHWorkspaceUsesPhoneLayout() ? 16.0 : 24.0;
    CGFloat contentInsetX = ISHWorkspaceUsesPhoneLayout() ? 12.0 : 20.0;
    [NSLayoutConstraint activateConstraints:@[
        [scrollView.topAnchor constraintEqualToAnchor:safeArea.topAnchor],
        [scrollView.leadingAnchor constraintEqualToAnchor:safeArea.leadingAnchor],
        [scrollView.trailingAnchor constraintEqualToAnchor:safeArea.trailingAnchor],
        [scrollView.bottomAnchor constraintEqualToAnchor:safeArea.bottomAnchor],

        [contentStack.topAnchor constraintEqualToAnchor:scrollView.topAnchor constant:contentInsetY],
        [contentStack.leadingAnchor constraintEqualToAnchor:scrollView.leadingAnchor constant:contentInsetX],
        [contentStack.trailingAnchor constraintEqualToAnchor:scrollView.trailingAnchor constant:-contentInsetX],
        [contentStack.bottomAnchor constraintEqualToAnchor:scrollView.bottomAnchor constant:-contentInsetY],
        [contentStack.widthAnchor constraintEqualToAnchor:scrollView.widthAnchor constant:-(contentInsetX * 2.0)],
    ]];

    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(refreshWorkspaceStatus)
                                               name:TerminalRegistryDidChangeNotification
                                             object:nil];
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(refreshWorkspaceStatus)
                                               name:TerminalDidLoadNotification
                                             object:nil];
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(refreshWorkspaceStatus)
                                               name:TerminalLoadFailedNotification
                                             object:nil];
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(workspaceActivationDidChange:)
                                               name:UIApplicationDidBecomeActiveNotification
                                             object:nil];
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(workspaceThemeDidChange:)
                                               name:ISHWorkspaceToolThemeDidChangeNotification
                                             object:nil];
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(workspaceDockFrameDidChange:)
                                               name:ISHWorkspaceDockFrameDidChangeNotification
                                             object:nil];
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(workspaceWorkspacesFrameDidChange:)
                                               name:ISHWorkspaceWorkspacesFrameDidChangeNotification
                                             object:nil];
    if (@available(iOS 13.0, *)) {
        [NSNotificationCenter.defaultCenter addObserver:self
                                               selector:@selector(workspaceActivationDidChange:)
                                                   name:UISceneDidActivateNotification
                                                 object:nil];
        [NSNotificationCenter.defaultCenter addObserver:self
                                               selector:@selector(refreshWorkspaceStatus)
                                                   name:UISceneDidDisconnectNotification
                                                 object:nil];
    }

    [UserPreferences.shared observe:@[@"workspaceStyle", @"colorScheme"]
                            options:0 owner:self usingBlock:^(typeof(self) self) {
        // Re-skin every open window and the desktop when the Classic/Modern style is
        // switched from anywhere (Settings, the dock menu, or the guest `defaults` tool).
        dispatch_async(dispatch_get_main_queue(), ^{
            [self applyWorkspaceDesktopBackground];
            [self refreshDockButtons];
        });
    }];

    [self refreshWorkspaceStatus];
    [self refreshDockButtons];
}

- (void)traitCollectionDidChange:(UITraitCollection *)previousTraitCollection {
    [super traitCollectionDidChange:previousTraitCollection];
    if (previousTraitCollection == nil)
        return;
    if (previousTraitCollection.horizontalSizeClass != self.traitCollection.horizontalSizeClass) {
        [self rebuildWorkspaceColumns];
    }
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    [self applyCurrentThemeWallpaperIfNeededForced:NO];
    for (UIView *view in self.desktopWindows) {
        if (![view isKindOfClass:ISHWorkspaceContainedWindowView.class])
            continue;
        ISHWorkspaceContainedWindowView *windowView = (ISHWorkspaceContainedWindowView *) view;
        [self applyInitialFrameIfNeededToDesktopWindow:windowView];
        [self clampDesktopWindowToVisibleBounds:windowView];
        if (windowView.pinnedToBottomCenter)
            [self pinDesktopWindowToBottomCenter:windowView];
    }
}

- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:self];
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [self refreshWorkspaceStatus];
}

- (void)viewWillDisappear:(BOOL)animated {
    [super viewWillDisappear:animated];
    [self.view.window endEditing:YES];
}

- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    [self applyCurrentThemeWallpaperIfNeededForced:NO];
    [self applyCompactSizingToOpenWorkspaceToolWindows];
    if (!self.didOpenInitialTool && self.initialToolIdentifier.length > 0) {
        self.didOpenInitialTool = YES;
        [self openWorkspaceToolWithIdentifier:self.initialToolIdentifier];
    }
    if (!self.didEnsureDefaultWorkspaceUtilities) {
        self.didEnsureDefaultWorkspaceUtilities = YES;
        NSArray<NSDictionary<NSString *, id> *> *savedLayout = [self savedWorkspaceLayoutForCurrentScene];
        BOOL hasSavedLayout = [savedLayout isKindOfClass:NSArray.class] && savedLayout.count > 0;
        [self ensureDefaultWorkspaceUtilitiesOpen];
        // Honor a saved arrangement: don't force the LLM chat back open if it was closed before
        // saving, and reopen the Launcher (at its saved spot) if it was shown on the desktop.
        if (!hasSavedLayout || [self savedLayout:savedLayout toolDescriptorForIdentifier:ISHWorkspaceToolLLMIdentifier] != nil)
            [self ensureDefaultLLMChatWindowOpenIfNeeded];
        NSDictionary<NSString *, id> *launcherDescriptor =
            hasSavedLayout ? [self savedLayout:savedLayout toolDescriptorForIdentifier:ISHWorkspaceToolLauncherIdentifier] : nil;
        if (launcherDescriptor != nil && [self desktopWindowForToolIdentifier:ISHWorkspaceToolLauncherIdentifier] == nil) {
            ISHWorkspaceContainedWindowView *launcherWindow = [self openWorkspaceToolWindowWithIdentifier:ISHWorkspaceToolLauncherIdentifier];
            [self applySavedFrameDescriptor:launcherDescriptor[@"frame"]
                                   toWindow:launcherWindow
                               fallbackSize:ISHWorkspacePreferredToolContentSize(ISHWorkspaceToolLauncherIdentifier)];
        }
        [self applyDesktopVisibility];
    }
    if (self.dockWindow != nil) {
        [self applyInitialPlacementToDockWindow:self.dockWindow];
    }
    // Pinned placement is iPad-only; on phone the Desktops applet keeps its normal placement.
    if (!ISHWorkspaceUsesPhoneLayout()) {
        ISHWorkspaceContainedWindowView *workspacesWindow =
            [self desktopWindowForToolIdentifier:ISHWorkspaceToolWorkspacesIdentifier];
        if (workspacesWindow != nil) {
            [self applyInitialPlacementToWorkspacesWindow:workspacesWindow];
        }
    }
    [self presentStartupLowMemoryWarningIfNeeded];
}

- (void)viewDidDisappear:(BOOL)animated {
    [super viewDidDisappear:animated];
}

- (void)presentStartupLowMemoryWarningIfNeeded {
    if (self.didEvaluateStartupMemoryWarning)
        return;
    self.didEvaluateStartupMemoryWarning = YES;
    if (ISHWorkspaceLowMemoryWarningShownThisLaunch)
        return;
    if (!ISHWorkspaceDeviceHasLowMemoryForWorkspace())
        return;
    if ([NSUserDefaults.standardUserDefaults boolForKey:ISHWorkspaceStartupLowMemoryWarningDisabledPreferenceKey])
        return;
    if (self.presentedViewController != nil)
        return;

    ISHWorkspaceLowMemoryWarningShownThisLaunch = YES;

    NSDictionary<NSString *, UIColor *> *theme = ISHWorkspaceThemeDescriptor();
    UIView *overlay = [UIView new];
    overlay.translatesAutoresizingMaskIntoConstraints = NO;
    overlay.backgroundColor = [UIColor colorWithWhite:0.0 alpha:0.34];

    UIView *card = [UIView new];
    card.translatesAutoresizingMaskIntoConstraints = NO;
    card.backgroundColor = [theme[@"card"] colorWithAlphaComponent:0.98];
    card.layer.cornerRadius = 18.0;
    card.layer.borderWidth = 1.0;
    card.layer.borderColor = [theme[@"stroke"] colorWithAlphaComponent:0.95].CGColor;
    [overlay addSubview:card];

    UILabel *titleLabel = [UILabel new];
    titleLabel.translatesAutoresizingMaskIntoConstraints = NO;
    titleLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];
    titleLabel.textColor = theme[@"primary"];
    titleLabel.text = @"Limited Memory Device";
    [card addSubview:titleLabel];

    UILabel *bodyLabel = [UILabel new];
    bodyLabel.translatesAutoresizingMaskIntoConstraints = NO;
    bodyLabel.numberOfLines = 0;
    bodyLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    bodyLabel.textColor = theme[@"secondary"];
    bodyLabel.text = @"This device has less than 4 GB of RAM. Workspace is more stable with fewer windows and browser tabs open. Browser tabs are limited to 2 on this device.";
    [card addSubview:bodyLabel];

    UIView *toggleRow = [UIView new];
    toggleRow.translatesAutoresizingMaskIntoConstraints = NO;
    [card addSubview:toggleRow];

    UISwitch *disableSwitch = [UISwitch new];
    disableSwitch.translatesAutoresizingMaskIntoConstraints = NO;
    disableSwitch.onTintColor = theme[@"accent"];
    [toggleRow addSubview:disableSwitch];
    self.startupMemoryWarningDisableSwitch = disableSwitch;

    UILabel *toggleLabel = [UILabel new];
    toggleLabel.translatesAutoresizingMaskIntoConstraints = NO;
    toggleLabel.numberOfLines = 2;
    toggleLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleSubheadline];
    toggleLabel.textColor = theme[@"primary"];
    toggleLabel.text = @"Don't show this warning again";
    [toggleRow addSubview:toggleLabel];

    UIButton *okButton = [UIButton buttonWithType:UIButtonTypeSystem];
    okButton.translatesAutoresizingMaskIntoConstraints = NO;
    okButton.layer.cornerRadius = 12.0;
    okButton.layer.borderWidth = 1.0;
    okButton.layer.borderColor = theme[@"stroke"].CGColor;
    okButton.backgroundColor = [theme[@"cardAlt"] colorWithAlphaComponent:0.96];
    okButton.titleLabel.font = [UIFont systemFontOfSize:15 weight:UIFontWeightSemibold];
    [okButton setTitle:@"OK" forState:UIControlStateNormal];
    [okButton setTitleColor:theme[@"primary"] forState:UIControlStateNormal];
    [okButton addTarget:self action:@selector(dismissStartupLowMemoryWarning:) forControlEvents:UIControlEventTouchUpInside];
    [card addSubview:okButton];

    [self.view addSubview:overlay];
    self.startupMemoryWarningOverlayView = overlay;

    CGFloat horizontalInset = ISHWorkspaceUsesPhoneLayout() ? 18.0 : 28.0;
    [NSLayoutConstraint activateConstraints:@[
        [overlay.topAnchor constraintEqualToAnchor:self.view.topAnchor],
        [overlay.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
        [overlay.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
        [overlay.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor],

        [card.centerXAnchor constraintEqualToAnchor:overlay.centerXAnchor],
        [card.centerYAnchor constraintEqualToAnchor:overlay.centerYAnchor],
        [card.leadingAnchor constraintGreaterThanOrEqualToAnchor:overlay.leadingAnchor constant:horizontalInset],
        [card.trailingAnchor constraintLessThanOrEqualToAnchor:overlay.trailingAnchor constant:-horizontalInset],
        [card.widthAnchor constraintGreaterThanOrEqualToConstant:(ISHWorkspaceUsesPhoneLayout() ? 280.0 : 360.0)],
        [card.widthAnchor constraintLessThanOrEqualToConstant:(ISHWorkspaceUsesPhoneLayout() ? 340.0 : 420.0)],

        [titleLabel.topAnchor constraintEqualToAnchor:card.topAnchor constant:18.0],
        [titleLabel.leadingAnchor constraintEqualToAnchor:card.leadingAnchor constant:18.0],
        [titleLabel.trailingAnchor constraintEqualToAnchor:card.trailingAnchor constant:-18.0],

        [bodyLabel.topAnchor constraintEqualToAnchor:titleLabel.bottomAnchor constant:10.0],
        [bodyLabel.leadingAnchor constraintEqualToAnchor:card.leadingAnchor constant:18.0],
        [bodyLabel.trailingAnchor constraintEqualToAnchor:card.trailingAnchor constant:-18.0],

        [toggleRow.topAnchor constraintEqualToAnchor:bodyLabel.bottomAnchor constant:16.0],
        [toggleRow.leadingAnchor constraintEqualToAnchor:card.leadingAnchor constant:18.0],
        [toggleRow.trailingAnchor constraintEqualToAnchor:card.trailingAnchor constant:-18.0],

        [disableSwitch.leadingAnchor constraintEqualToAnchor:toggleRow.leadingAnchor],
        [disableSwitch.topAnchor constraintEqualToAnchor:toggleRow.topAnchor],
        [disableSwitch.bottomAnchor constraintEqualToAnchor:toggleRow.bottomAnchor],

        [toggleLabel.leadingAnchor constraintEqualToAnchor:disableSwitch.trailingAnchor constant:12.0],
        [toggleLabel.trailingAnchor constraintEqualToAnchor:toggleRow.trailingAnchor],
        [toggleLabel.centerYAnchor constraintEqualToAnchor:disableSwitch.centerYAnchor],

        [okButton.topAnchor constraintEqualToAnchor:toggleRow.bottomAnchor constant:18.0],
        [okButton.leadingAnchor constraintEqualToAnchor:card.leadingAnchor constant:18.0],
        [okButton.trailingAnchor constraintEqualToAnchor:card.trailingAnchor constant:-18.0],
        [okButton.heightAnchor constraintEqualToConstant:44.0],
        [okButton.bottomAnchor constraintEqualToAnchor:card.bottomAnchor constant:-18.0],
    ]];

    overlay.alpha = 0.0;
    card.transform = CGAffineTransformMakeScale(0.96, 0.96);
    [UIView animateWithDuration:0.18 animations:^{
        overlay.alpha = 1.0;
        card.transform = CGAffineTransformIdentity;
    }];
}

- (void)dismissStartupLowMemoryWarning:(id)sender {
    (void) sender;
    if (self.startupMemoryWarningDisableSwitch.on) {
        [NSUserDefaults.standardUserDefaults setBool:YES forKey:ISHWorkspaceStartupLowMemoryWarningDisabledPreferenceKey];
    }
    UIView *overlay = self.startupMemoryWarningOverlayView;
    self.startupMemoryWarningOverlayView = nil;
    self.startupMemoryWarningDisableSwitch = nil;
    [UIView animateWithDuration:0.16 animations:^{
        overlay.alpha = 0.0;
    } completion:^(__unused BOOL finished) {
        [overlay removeFromSuperview];
    }];
}

- (void)workspaceThemeDidChange:(__unused NSNotification *)notification {
    [self applyCurrentThemeWallpaperIfNeededForced:YES];
    [self applyCompactSizingToOpenWorkspaceToolWindows];
    [self refreshDockButtons];
}

- (void)workspaceActivationDidChange:(NSNotification *)notification {
    if (@available(iOS 13.0, *)) {
        if ([notification.name isEqualToString:UISceneDidActivateNotification]) {
            UIWindowScene *currentScene = self.view.window.windowScene;
            if (notification.object != nil && notification.object != currentScene)
                return;
        }
    }
    [self refreshWorkspaceStatus];
    if (self.dockWindow != nil) {
        [self applyInitialPlacementToDockWindow:self.dockWindow];
    }
    [self restoreLauncherWindowPlacement];
    // The Desktops applet is a pinned utility only on iPad. On phone it uses normal cascade
    // placement, so re-pinning it here would yank it off-screen on every scene activation
    // (which is why it "vanished" after interacting). Leave the phone applet where it is.
    if (!ISHWorkspaceUsesPhoneLayout()) {
        ISHWorkspaceContainedWindowView *workspacesWindow =
            [self desktopWindowForToolIdentifier:ISHWorkspaceToolWorkspacesIdentifier];
        if (workspacesWindow != nil) {
            [self applyInitialPlacementToWorkspacesWindow:workspacesWindow];
        }
    }
}

- (void)workspaceDockFrameDidChange:(NSNotification *)notification {
    if (notification.object == self)
        return;
    if (self.dockWindow != nil) {
        [self applyInitialPlacementToDockWindow:self.dockWindow];
    }
}

- (void)workspaceWorkspacesFrameDidChange:(NSNotification *)notification {
    if (notification.object == self)
        return;
    // iPad-only pinned-utility frame sync across scenes; on phone the applet isn't pinned, so
    // re-placing it here would push it off-screen.
    if (ISHWorkspaceUsesPhoneLayout())
        return;
    ISHWorkspaceContainedWindowView *workspacesWindow =
        [self desktopWindowForToolIdentifier:ISHWorkspaceToolWorkspacesIdentifier];
    if (workspacesWindow != nil) {
        [self applyInitialPlacementToWorkspacesWindow:workspacesWindow];
    }
}

- (NSString *)currentWorkspaceLayoutStorageIdentifier {
    // In-app Desktops are a single workspace (no per-scene layouts). On a phone the scene can be
    // discarded and reconnected on foreground with a fresh persistentIdentifier, which would
    // orphan a scene-keyed saved layout — so use a stable key there and Save/Restore survives.
    if (ISHWorkspaceUsesPhoneLayout())
        return @"default";
    if (@available(iOS 13.0, *)) {
        NSString *identifier = self.view.window.windowScene.session.persistentIdentifier;
        if (identifier.length > 0)
            return identifier;
    }
    return @"default";
}

- (NSArray<NSDictionary<NSString *, id> *> *)savedWorkspaceLayoutForCurrentScene {
    id storedValue = [NSUserDefaults.standardUserDefaults objectForKey:ISHWorkspaceSavedLayoutDefaultsKey];
    if ([storedValue isKindOfClass:NSArray.class])
        return (NSArray<NSDictionary<NSString *, id> *> *) storedValue;
    if (![storedValue isKindOfClass:NSDictionary.class])
        return nil;

    NSDictionary<NSString *, id> *layoutsByScene = (NSDictionary<NSString *, id> *) storedValue;
    NSString *sceneIdentifier = [self currentWorkspaceLayoutStorageIdentifier];
    id sceneLayout = sceneIdentifier.length > 0 ? layoutsByScene[sceneIdentifier] : nil;
    if ([sceneLayout isKindOfClass:NSArray.class])
        return (NSArray<NSDictionary<NSString *, id> *> *) sceneLayout;

    id defaultLayout = layoutsByScene[@"default"];
    if ([defaultLayout isKindOfClass:NSArray.class])
        return (NSArray<NSDictionary<NSString *, id> *> *) defaultLayout;
    return nil;
}

- (NSString *)currentWorkspaceDisplayName {
    if (@available(iOS 13.0, *)) {
        UIScene *scene = self.view.window.windowScene;
        if (scene != nil)
            return [NSString stringWithFormat:@"Workspace %@", ISHWorkspaceNameForSession(scene.session)];
    }
    return @"Workspace One";
}

- (void)refreshWorkspaceStatus {
    if (!NSThread.isMainThread) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self refreshWorkspaceStatus];
        });
        return;
    }

    self.layoutManagerWorkspaceLabel.text = [NSString stringWithFormat:@"%@ layout. Save and restore only affects this workspace.", [self currentWorkspaceDisplayName]];
    [self refreshDockButtons];
}

- (void)refreshDockButtons {
    ISHWorkspaceContainedWindowView *frontmostWindow = [self frontmostDesktopWindowExcludingDock];

    ISHWorkspaceContainedWindowView *clockWindow = [self desktopWindowForToolIdentifier:ISHWorkspaceToolClockIdentifier];
    ISHWorkspaceContainedWindowView *workspacesWindow = [self desktopWindowForToolIdentifier:ISHWorkspaceToolWorkspacesIdentifier];
    BOOL utilsFrontmost = frontmostWindow == clockWindow || frontmostWindow == workspacesWindow;
    BOOL utilsActive = clockWindow != nil || workspacesWindow != nil;
    NSString *utilsState = clockWindow != nil ? @"Clock" : (workspacesWindow != nil ? @"Scenes" : @"Menu");
    [self configureDockTileButton:self.dockUtilsButton
                            title:@"Utils"
                            state:utilsState
                           active:utilsActive
                        frontmost:utilsFrontmost];

    ISHWorkspaceContainedWindowView *shellWindow = [self desktopWindowForTerminalRole:ISHWorkspaceTerminalRoleSessionShell];
    ISHWorkspaceContainedWindowView *frontmostTerminalWindow = [self frontmostDesktopTerminalWindow];
    BOOL hasTerminalWindow = frontmostTerminalWindow != nil;
    BOOL frontmostIsTerminal = frontmostTerminalWindow != nil && frontmostWindow == frontmostTerminalWindow;
    NSString *terminalState = @"Shell";
    if (shellWindow != nil || hasTerminalWindow)
        terminalState = @"Active";
    [self configureDockTileButton:self.dockTerminalButton
                            title:@"Terminal"
                            state:terminalState
                           active:hasTerminalWindow
                        frontmost:frontmostIsTerminal];

    NSDictionary<NSString *, UIColor *> *theme = ISHWorkspaceThemeDescriptor();
    for (ISHWorkspaceContainedWindowView *windowView in self.desktopWindows.copy) {
        if (![windowView isKindOfClass:ISHWorkspaceContainedWindowView.class] || windowView.hidden)
            continue;
        BOOL activeWindow = windowView == self.desktopSurfaceView.subviews.lastObject;
        [windowView applyWorkspaceChromeTheme:theme active:activeWindow];
        if (ISHWorkspaceUsesModernStyle()) {
            // In Modern, the window's upper-right utility button becomes the ☰ menu —
            // a per-window way to summon the root menu (the dock replacement).
            __weak typeof(self) weakSelf = self;
            __weak typeof(windowView) weakWindow = windowView;
            [windowView setUtilityButtonTitle:@"☰" handler:^{
                [weakSelf presentDesktopRootMenuFromView:weakWindow.utilityButton
                                              sourceRect:weakWindow.utilityButton.bounds];
            }];
        } else {
            [windowView setUtilityButtonTitle:nil handler:nil];
        }
    }
}

- (void)applyCompactSizingToOpenWorkspaceToolWindows {
    for (ISHWorkspaceContainedWindowView *windowView in self.desktopWindows.copy) {
        NSString *toolIdentifier = windowView.workspaceToolIdentifier;
        if (toolIdentifier.length == 0)
            continue;
        if ([toolIdentifier isEqualToString:ISHWorkspaceToolWorkspacesIdentifier])
            continue;
        if (!ISHWorkspaceUsesPhoneLayout() && [toolIdentifier isEqualToString:ISHWorkspaceToolThemesIdentifier])
            continue;

        CGSize targetSize = ISHWorkspacePreferredToolContentSize(toolIdentifier);
        CGRect currentFrame = windowView.frame;
        BOOL shouldShrinkWidth = CGRectGetWidth(currentFrame) > targetSize.width + 24.0;
        BOOL shouldShrinkHeight = CGRectGetHeight(currentFrame) > targetSize.height + 24.0;
        if (!shouldShrinkWidth && !shouldShrinkHeight)
            continue;

        CGSize resized = CGSizeMake(shouldShrinkWidth ? targetSize.width : CGRectGetWidth(currentFrame),
                                    shouldShrinkHeight ? targetSize.height : CGRectGetHeight(currentFrame));
        [self resizeDesktopWindow:windowView toSize:resized animated:NO];
    }
}

- (void)openWorkspaceToolWithIdentifier:(NSString *)toolIdentifier {
    // The Desktops applet manages in-app Desktops, available on every device (incl. iPhone).
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolWorkspacesIdentifier]) {
        ISHWorkspaceContainedWindowView *existingWindow = [self desktopWindowForToolIdentifier:toolIdentifier];
        if (existingWindow != nil) {
            [self focusDesktopWindow:existingWindow];
            return;
        }
    }
    [self openWorkspaceToolWindowWithIdentifier:toolIdentifier];
}

- (void)ensureDefaultWorkspaceUtilitiesOpen {
    // In-app Desktops are no longer scene-window-only, so the Desktops applet auto-opens on every
    // device now, including iPhone. This was previously gated off here by the iPad-only scene
    // check (ISHWorkspaceSupportsSceneWindows), which is exactly why the applet never appeared on
    // iPhone.
    if ([self desktopWindowForToolIdentifier:ISHWorkspaceToolWorkspacesIdentifier] != nil)
        return;
    [self openWorkspaceToolWithIdentifier:ISHWorkspaceToolWorkspacesIdentifier];
}

// When the LLM client is enabled, open the chat as a desktop window by default so
// it's a first-class workspace tile rather than something you must summon from the
// dock menu each time. Skipped if it's already on the desktop (e.g. restored from a
// saved layout), and the user can still close it for the session.
- (void)ensureDefaultLLMChatWindowOpenIfNeeded {
    if (!ISHLLMClientEnabled())
        return;
    if ([self desktopWindowForToolIdentifier:ISHWorkspaceToolLLMIdentifier] != nil)
        return;
    [self openWorkspaceToolWithIdentifier:ISHWorkspaceToolLLMIdentifier];
    // Bring it to the front so it isn't hidden behind the default Workspaces
    // window that was just opened above.
    ISHWorkspaceContainedWindowView *window =
        [self desktopWindowForToolIdentifier:ISHWorkspaceToolLLMIdentifier];
    if (window != nil)
        [self focusDesktopWindow:window];
}

- (void)persistDefaultWorkspaceUtilityFrames {
    ISHWorkspaceContainedWindowView *workspacesWindow = [self desktopWindowForToolIdentifier:ISHWorkspaceToolWorkspacesIdentifier];
    if (workspacesWindow == nil || workspacesWindow.hidden)
        return;
    NSDictionary<NSString *, NSNumber *> *frameDescriptor = [self absoluteFrameDescriptorForFrame:workspacesWindow.frame];
    if (frameDescriptor != nil) {
        [NSUserDefaults.standardUserDefaults setObject:frameDescriptor
                                                forKey:[self persistentWorkspacesWindowFrameDefaultsKey]];
        [NSNotificationCenter.defaultCenter postNotificationName:ISHWorkspaceWorkspacesFrameDidChangeNotification
                                                          object:self];
    }
}

- (NSString *)persistentWorkspacesWindowFrameDefaultsKey {
    return ISHWorkspacePersistentWorkspacesWindowFrameDefaultsKey;
}

- (void)persistLauncherWindowFrame {
    ISHWorkspaceContainedWindowView *launcherWindow = [self desktopWindowForToolIdentifier:ISHWorkspaceToolLauncherIdentifier];
    if (launcherWindow == nil || launcherWindow.hidden)
        return;
    NSDictionary<NSString *, NSNumber *> *frameDescriptor = [self absoluteFrameDescriptorForFrame:launcherWindow.frame];
    if (frameDescriptor != nil)
        [NSUserDefaults.standardUserDefaults setObject:frameDescriptor
                                                forKey:ISHWorkspacePersistentLauncherWindowFrameDefaultsKey];
}

- (void)applyInitialPlacementToLauncherWindow:(ISHWorkspaceContainedWindowView *)windowView {
    if (windowView == nil)
        return;
    NSDictionary<NSString *, id> *frameDescriptor =
        [NSUserDefaults.standardUserDefaults dictionaryForKey:ISHWorkspacePersistentLauncherWindowFrameDefaultsKey];
    if ([frameDescriptor isKindOfClass:NSDictionary.class])
        [self applyAbsoluteFrameDescriptor:frameDescriptor toWindow:windowView];
}

- (void)restoreLauncherWindowPlacement {
    ISHWorkspaceContainedWindowView *launcherWindow = [self desktopWindowForToolIdentifier:ISHWorkspaceToolLauncherIdentifier];
    if (launcherWindow != nil)
        [self applyInitialPlacementToLauncherWindow:launcherWindow];
}

- (void)persistDockWindowFrame {
    if (self.dockWindow == nil || self.dockWindow.hidden)
        return;
    NSDictionary<NSString *, NSNumber *> *frameDescriptor = [self absoluteFrameDescriptorForFrame:self.dockWindow.frame];
    if (frameDescriptor == nil)
        return;
    NSDictionary<NSString *, id> *descriptor = @{
        @"frame": frameDescriptor,
        @"pinned": @(self.dockWindow.pinnedToBottomCenter),
    };
    [NSUserDefaults.standardUserDefaults setObject:descriptor
                                            forKey:[self persistentDockWindowDescriptorDefaultsKey]];
    [NSNotificationCenter.defaultCenter postNotificationName:ISHWorkspaceDockFrameDidChangeNotification
                                                      object:self];
}

- (NSString *)persistentDockWindowDescriptorDefaultsKey {
    return ISHWorkspacePersistentDockWindowDescriptorDefaultsKey;
}

- (void)applyInitialPlacementToDockWindow:(ISHWorkspaceContainedWindowView *)windowView {
    if (windowView == nil)
        return;
    NSDictionary<NSString *, id> *descriptor =
        [NSUserDefaults.standardUserDefaults dictionaryForKey:[self persistentDockWindowDescriptorDefaultsKey]];
    NSDictionary<NSString *, id> *frameDescriptor =
        [descriptor isKindOfClass:NSDictionary.class] ? descriptor[@"frame"] : nil;
    BOOL pinned = ![descriptor isKindOfClass:NSDictionary.class] || [descriptor[@"pinned"] boolValue];
    if ([frameDescriptor isKindOfClass:NSDictionary.class] &&
        frameDescriptor[@"originX"] != nil &&
        frameDescriptor[@"originY"] != nil &&
        frameDescriptor[@"height"] != nil) {
        windowView.pinnedToBottomCenter = pinned;
        [self applyAbsoluteFrameDescriptor:frameDescriptor toWindow:windowView];
        if (windowView.pinnedToBottomCenter)
            [self pinDesktopWindowToBottomCenter:windowView];
        return;
    }
    windowView.pinnedToBottomCenter = YES;
    [self pinDesktopWindowToBottomCenter:windowView];
}

- (void)applyInitialPlacementToWorkspacesWindow:(ISHWorkspaceContainedWindowView *)windowView {
    if (windowView == nil)
        return;
    NSDictionary<NSString *, id> *frameDescriptor =
        [NSUserDefaults.standardUserDefaults dictionaryForKey:[self persistentWorkspacesWindowFrameDefaultsKey]];
    if ([frameDescriptor isKindOfClass:NSDictionary.class] &&
        frameDescriptor[@"originX"] != nil &&
        frameDescriptor[@"originY"] != nil &&
        frameDescriptor[@"height"] != nil) {
        [self applyAbsoluteFrameDescriptor:frameDescriptor toWindow:windowView];
        return;
    }
    if (![frameDescriptor isKindOfClass:NSDictionary.class]) {
        if (@available(iOS 13.0, *)) {
            NSString *sceneIdentifier = self.view.window.windowScene.session.persistentIdentifier;
            if (sceneIdentifier.length > 0) {
                NSString *legacyKey =
                    [NSString stringWithFormat:@"%@.%@", ISHWorkspaceLegacyPersistentWorkspacesWindowFrameDefaultsKeyPrefix, sceneIdentifier];
                frameDescriptor = [NSUserDefaults.standardUserDefaults dictionaryForKey:legacyKey];
                if ([frameDescriptor isKindOfClass:NSDictionary.class]) {
                    [NSUserDefaults.standardUserDefaults setObject:frameDescriptor
                                                            forKey:[self persistentWorkspacesWindowFrameDefaultsKey]];
                }
            }
        }
    }
    if ([frameDescriptor isKindOfClass:NSDictionary.class]) {
        [self applySavedFrameDescriptor:frameDescriptor
                               toWindow:windowView
                           fallbackSize:ISHWorkspacePreferredToolContentSize(ISHWorkspaceToolWorkspacesIdentifier)];
    } else {
        [self positionDesktopWindowAtTopRight:windowView];
    }
}

// Resize the open Desktops applet to match the Desktop count (keeps its restored origin).
- (void)autosizeWorkspacesWindow {
    ISHWorkspaceContainedWindowView *window = [self desktopWindowForToolIdentifier:ISHWorkspaceToolWorkspacesIdentifier];
    if (window == nil)
        return;
    [self resizeDesktopWindow:window
                       toSize:ISHWorkspaceWorkspacesContentSize((NSUInteger)MAX((NSInteger)1, self.desktopCount))
                     animated:NO];
}

- (void)openDiagnostics:(id)sender {
    [self openWorkspaceToolWithIdentifier:ISHWorkspaceToolDiagnosticsIdentifier];
}

- (void)toggleClockFromDock:(id)sender {
    ISHWorkspaceContainedWindowView *clockWindow = [self desktopWindowForToolIdentifier:ISHWorkspaceToolClockIdentifier];
    if (clockWindow != nil) {
        if (clockWindow.closeHandler != nil)
            clockWindow.closeHandler();
        return;
    }
    [self openWorkspaceToolWithIdentifier:ISHWorkspaceToolClockIdentifier];
}

- (void)openOrFocusWorkspaceToolFromDock:(UIButton *)sender {
    NSString *toolIdentifier = sender.accessibilityIdentifier;
    if (toolIdentifier.length == 0)
        return;

    [self openOrFocusWorkspaceToolIdentifier:toolIdentifier];
}

- (void)openOrFocusTerminalFromDock:(UIButton *)sender {
    (void) sender;
    [self openDesktopTerminalHerePreferringConsole:NO
                                     reuseExisting:NO
                                   trackPrimaryRole:NO];
}

- (void)openOrFocusWorkspaceToolIdentifier:(NSString *)toolIdentifier {
    if (toolIdentifier.length == 0)
        return;
    ISHWorkspaceContainedWindowView *existingWindow = [self desktopWindowForToolIdentifier:toolIdentifier];
    if (existingWindow != nil) {
        // Summon the existing tool to the current Desktop and reveal it (it may have been on
        // another Desktop, i.e. hidden) instead of spawning a duplicate.
        existingWindow.workspaceDesktopIndex = self.activeDesktopIndex;
        existingWindow.hidden = NO;
        [self focusDesktopWindow:existingWindow];
        return;
    }
    [self openWorkspaceToolWithIdentifier:toolIdentifier];
}

- (NSArray<NSDictionary<NSString *, id> *> *)dockUtilityGroupDescriptors {
    NSMutableArray<NSDictionary<NSString *, id> *> *workspaceItems = [NSMutableArray arrayWithObject:@{@"title": @"Layout Manager", @"identifier": @"dashboard"}];
    // Desktops are in-app (not iOS scenes), so the applet is available on every device.
    [workspaceItems addObject:@{@"title": @"Desktops", @"identifier": ISHWorkspaceToolWorkspacesIdentifier}];
    [workspaceItems addObjectsFromArray:@[
        @{@"title": @"Launcher", @"identifier": ISHWorkspaceToolLauncherIdentifier},
        @{@"title": @"Quick Actions", @"identifier": ISHWorkspaceToolShortcutsIdentifier},
        @{@"title": @"Browser", @"identifier": ISHWorkspaceToolBrowserIdentifier},
        @{@"title": @"Music", @"identifier": ISHWorkspaceToolAudioIdentifier},
        @{@"title": @"MotePad", @"identifier": ISHWorkspaceToolMotePadIdentifier},
        @{@"title": @"File Manager", @"identifier": ISHWorkspaceToolFileManagerIdentifier},
        @{@"title": @"Sessions", @"identifier": ISHWorkspaceToolSessionsIdentifier},
        @{@"title": @"Themes", @"identifier": ISHWorkspaceToolThemesIdentifier},
    ]];
    if (ISHLLMClientEnabled())
        [workspaceItems addObject:@{@"title": @"LLM Chat", @"identifier": ISHWorkspaceToolLLMIdentifier}];
    return @[
        @{
            @"title": @"Workspace",
            @"message": @"Launchers and workspace-wide controls.",
            @"items": workspaceItems,
        },
        @{
            @"title": @"Media",
            @"message": @"Document and image viewers.",
            @"items": @[
                @{@"title": @"Markdown", @"identifier": ISHWorkspaceToolMarkdownViewerIdentifier},
                @{@"title": @"Image Viewer", @"identifier": ISHWorkspaceToolImageViewerIdentifier},
                @{@"title": @"Video Player", @"identifier": ISHWorkspaceToolVideoPlayerIdentifier},
                @{@"title": @"Wayland", @"identifier": ISHWorkspaceToolDisplayIdentifier},
            ],
        },
        @{
            @"title": @"Status",
            @"message": @"Live clocks, runtime summaries, and process views.",
            @"items": @[
                @{@"title": @"Clock", @"identifier": ISHWorkspaceToolClockIdentifier},
                @{@"title": @"Monitor", @"identifier": ISHWorkspaceToolMonitorIdentifier},
                @{@"title": @"Networks", @"identifier": ISHWorkspaceToolNetworksIdentifier},
                @{@"title": @"Logs", @"identifier": ISHWorkspaceToolStatusIdentifier},
            ],
        },
        @{
            @"title": @"Storage",
            @"message": @"Roots, filesystem summaries, and storage detail.",
            @"items": @[
                @{@"title": @"Storage", @"identifier": ISHWorkspaceToolStorageIdentifier},
                @{@"title": @"Boot Images", @"identifier": ISHWorkspaceToolFilesystemsIdentifier},
            ],
        },
        @{
            @"title": @"Support",
            @"message": @"Settings and diagnostics tools.",
            @"items": @[
                @{@"title": @"Settings", @"identifier": ISHWorkspaceToolSettingsIdentifier},
                @{@"title": @"Diagnostics", @"identifier": ISHWorkspaceToolDiagnosticsIdentifier},
            ],
        },
    ];
}

- (void)handleUtilsDockLongPress:(UILongPressGestureRecognizer *)recognizer {
    if (recognizer.state != UIGestureRecognizerStateBegan)
        return;
    [self presentUtilsDockActionsFromView:recognizer.view];
}

- (void)presentUtilityGroup:(NSDictionary<NSString *, id> *)groupDescriptor fromView:(UIView *)sourceView {
    NSString *groupTitle = groupDescriptor[@"title"] ?: @"Utils";
    NSString *groupMessage = groupDescriptor[@"message"];
    UIAlertController *sheet =
        [UIAlertController alertControllerWithTitle:groupTitle
                                            message:groupMessage
                                     preferredStyle:UIAlertControllerStyleActionSheet];

    for (NSDictionary<NSString *, NSString *> *descriptor in groupDescriptor[@"items"]) {
        NSString *toolIdentifier = descriptor[@"identifier"];
        NSString *title = descriptor[@"title"];
        BOOL isDashboardDescriptor = [toolIdentifier isEqualToString:@"dashboard"];
        ISHWorkspaceContainedWindowView *existingWindow = isDashboardDescriptor
            ? (self.dashboardWindow.hidden ? nil : self.dashboardWindow)
            : [self desktopWindowForToolIdentifier:toolIdentifier];
        NSString *actionTitle = existingWindow != nil
            ? [NSString stringWithFormat:@"Focus %@", title]
            : [NSString stringWithFormat:@"Open %@", title];
        [sheet addAction:[UIAlertAction actionWithTitle:actionTitle
                                                  style:UIAlertActionStyleDefault
                                                handler:^(__unused UIAlertAction *action) {
            if (existingWindow != nil) {
                [self focusDesktopWindow:existingWindow];
            } else if (isDashboardDescriptor) {
                [self openDashboardWindow:nil];
            } else {
                [self openWorkspaceToolWithIdentifier:toolIdentifier];
            }
        }]];
    }

    [sheet addAction:[UIAlertAction actionWithTitle:@"Back"
                                              style:UIAlertActionStyleCancel
                                            handler:^(__unused UIAlertAction *action) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self presentUtilsDockActionsFromView:sourceView];
        });
    }]];

    UIPopoverPresentationController *popoverPresentationController = sheet.popoverPresentationController;
    if (popoverPresentationController != nil) {
        popoverPresentationController.sourceView = sourceView ?: self.dockUtilsButton;
        popoverPresentationController.sourceRect = sourceView != nil ? sourceView.bounds : self.dockUtilsButton.bounds;
        popoverPresentationController.permittedArrowDirections = UIPopoverArrowDirectionAny;
    }
    [self presentViewController:sheet animated:YES completion:nil];
}

- (void)presentUtilsDockActionsFromView:(UIView *)sourceView {
    UIAlertController *sheet =
        [UIAlertController alertControllerWithTitle:@"Utils"
                                            message:@"Choose a utility group."
                                     preferredStyle:UIAlertControllerStyleActionSheet];

    for (NSDictionary<NSString *, id> *groupDescriptor in [self dockUtilityGroupDescriptors]) {
        NSString *title = groupDescriptor[@"title"] ?: @"Group";
        NSArray *items = groupDescriptor[@"items"];
        NSString *actionTitle = items.count > 0
            ? [NSString stringWithFormat:@"%@ (%lu)", title, (unsigned long) items.count]
            : title;
        [sheet addAction:[UIAlertAction actionWithTitle:actionTitle
                                                  style:UIAlertActionStyleDefault
                                                handler:^(__unused UIAlertAction *action) {
            dispatch_async(dispatch_get_main_queue(), ^{
                [self presentUtilityGroup:groupDescriptor fromView:sourceView];
            });
        }]];
    }

    NSString *workspaceStyleTitle = ISHWorkspaceUsesModernStyle() ? @"Modern" : @"Classic";
    [sheet addAction:[UIAlertAction actionWithTitle:[NSString stringWithFormat:@"Workspace Style: %@", workspaceStyleTitle]
                                              style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction *action) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self presentWorkspaceStyleChooserFromView:sourceView];
        });
    }]];

    [sheet addAction:[UIAlertAction actionWithTitle:@"Cancel"
                                              style:UIAlertActionStyleCancel
                                            handler:nil]];

    UIPopoverPresentationController *popoverPresentationController = sheet.popoverPresentationController;
    if (popoverPresentationController != nil) {
        popoverPresentationController.sourceView = sourceView ?: self.dockUtilsButton;
        popoverPresentationController.sourceRect = sourceView != nil ? sourceView.bounds : self.dockUtilsButton.bounds;
        popoverPresentationController.permittedArrowDirections = UIPopoverArrowDirectionAny;
    }
    [self presentViewController:sheet animated:YES completion:nil];
}

- (void)presentWorkspaceStyleChooserFromView:(UIView *)sourceView {
    UserPreferences *preferences = UserPreferences.shared;
    UIAlertController *sheet =
        [UIAlertController alertControllerWithTitle:@"Workspace Style"
                                            message:@"Pick the Classic or Modern workspace experience. Both stay available; this only changes how the desktop and its windows look and behave."
                                     preferredStyle:UIAlertControllerStyleActionSheet];

    NSArray<NSNumber *> *styles = @[@(WorkspaceStyleClassic), @(WorkspaceStyleModern)];
    NSDictionary<NSNumber *, NSString *> *styleTitles = @{
        @(WorkspaceStyleClassic): @"Classic",
        @(WorkspaceStyleModern): @"Modern",
    };
    for (NSNumber *style in styles) {
        BOOL selected = preferences.workspaceStyle == (WorkspaceStyle) style.integerValue;
        NSString *actionTitle = selected
            ? [NSString stringWithFormat:@"✓ %@", styleTitles[style]]
            : styleTitles[style];
        [sheet addAction:[UIAlertAction actionWithTitle:actionTitle
                                                  style:UIAlertActionStyleDefault
                                                handler:^(__unused UIAlertAction *action) {
            preferences.workspaceStyle = (WorkspaceStyle) style.integerValue;
            // Re-skin every open window immediately. Modern currently mirrors Classic, so
            // this is a no-op visual change until the Modern skin rung lands.
            [self refreshDockButtons];
        }]];
    }

    [sheet addAction:[UIAlertAction actionWithTitle:@"Back"
                                              style:UIAlertActionStyleCancel
                                            handler:^(__unused UIAlertAction *action) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self presentUtilsDockActionsFromView:sourceView];
        });
    }]];

    UIPopoverPresentationController *popoverPresentationController = sheet.popoverPresentationController;
    if (popoverPresentationController != nil) {
        popoverPresentationController.sourceView = sourceView ?: self.dockUtilsButton;
        popoverPresentationController.sourceRect = sourceView != nil ? sourceView.bounds : self.dockUtilsButton.bounds;
        popoverPresentationController.permittedArrowDirections = UIPopoverArrowDirectionAny;
    }
    [self presentViewController:sheet animated:YES completion:nil];
}

- (void)handleTerminalDockLongPress:(UILongPressGestureRecognizer *)recognizer {
    if (recognizer.state != UIGestureRecognizerStateBegan)
        return;
    [self presentTerminalDockActionsFromView:recognizer.view];
}

- (void)presentTerminalDockActionsFromView:(UIView *)sourceView {
    UIAlertController *sheet =
        [UIAlertController alertControllerWithTitle:@"Terminal"
                                            message:@"Open or focus shell, console, or another active terminal."
                                     preferredStyle:UIAlertControllerStyleActionSheet];

    ISHWorkspaceContainedWindowView *primaryShellWindow = [self desktopWindowForTerminalRole:ISHWorkspaceTerminalRoleSessionShell];
    NSString *primaryActionTitle = primaryShellWindow != nil ? @"Focus Session Shell" : @"Open Session Shell";
    [sheet addAction:[UIAlertAction actionWithTitle:primaryActionTitle
                                              style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction *action) {
        [self openTerminalHerePreferringConsole:NO];
    }]];

    ISHWorkspaceContainedWindowView *primaryConsoleWindow = [self desktopWindowForTerminalRole:ISHWorkspaceTerminalRoleSystemConsole];
    NSString *consoleActionTitle = primaryConsoleWindow != nil ? @"Focus System Console" : @"Open System Console";
    [sheet addAction:[UIAlertAction actionWithTitle:consoleActionTitle
                                              style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction *action) {
        [self openTerminalHerePreferringConsole:YES];
    }]];

    [sheet addAction:[UIAlertAction actionWithTitle:@"Open Another Shell Window"
                                              style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction *action) {
        [self openDesktopTerminalHerePreferringConsole:NO
                                         reuseExisting:NO
                                       trackPrimaryRole:NO];
    }]];

    NSUUID *primaryTerminalUUID = primaryShellWindow.hostedTerminalViewController.terminal.uuid;
    for (Terminal *terminal in [Terminal activeTerminals]) {
        if (primaryTerminalUUID != nil && [terminal.uuid isEqual:primaryTerminalUUID])
            continue;
        NSString *title = [NSString stringWithFormat:@"Focus %@", ISHWorkspaceTerminalDisplayName(terminal)];
        [sheet addAction:[UIAlertAction actionWithTitle:title
                                                  style:UIAlertActionStyleDefault
                                                handler:^(__unused UIAlertAction *action) {
            [self openExistingTerminalHereWithUUID:terminal.uuid];
        }]];
    }

    [sheet addAction:[UIAlertAction actionWithTitle:@"Cancel"
                                              style:UIAlertActionStyleCancel
                                            handler:nil]];

    UIPopoverPresentationController *popoverPresentationController = sheet.popoverPresentationController;
    if (popoverPresentationController != nil) {
        popoverPresentationController.sourceView = sourceView ?: self.dockTerminalButton;
        popoverPresentationController.sourceRect = sourceView != nil ? sourceView.bounds : self.dockTerminalButton.bounds;
        popoverPresentationController.permittedArrowDirections = UIPopoverArrowDirectionAny;
    }
    [self presentViewController:sheet animated:YES completion:nil];
}

- (void)openDesktopTerminalHerePreferringConsole:(BOOL)preferConsole
                                  reuseExisting:(BOOL)reuseExisting
                                trackPrimaryRole:(BOOL)trackPrimaryRole {
    NSString *terminalRole = preferConsole ? ISHWorkspaceTerminalRoleSystemConsole : ISHWorkspaceTerminalRoleSessionShell;
    if (reuseExisting) {
        ISHWorkspaceContainedWindowView *existingWindow = [self desktopWindowForTerminalRole:terminalRole];
        if (existingWindow != nil) {
            [self focusDesktopWindow:existingWindow];
            return;
        }
    }

    TerminalViewController *terminalViewController = [self createDesktopTerminalViewController];
    if (terminalViewController == nil) {
        [self presentSceneActivationError:nil];
        return;
    }
    terminalViewController.freshSessionTerminalDisplayMode =
        preferConsole ? ISHFreshSessionTerminalDisplayModeSystemConsole
                      : ISHFreshSessionTerminalDisplayModeSessionShell;
    // The tracked Session Shell window is the admin surface: always root. The
    // untracked dock "Terminal" windows are ordinary terminals that honor the
    // default-user preference, so they're titled accordingly.
    terminalViewController.alwaysLoginAsRoot = !preferConsole && trackPrimaryRole;

    NSString *title = preferConsole ? @"System Console"
                    : trackPrimaryRole ? @"Session Shell" : @"Terminal";
    ISHWorkspaceContainedWindowView *windowView =
        [self openDesktopTerminalWindowWithTitle:title terminalViewController:terminalViewController];
    [terminalViewController startNewSession];
    if (preferConsole) {
        [terminalViewController showSystemConsoleForCurrentSession];
    } else {
        [terminalViewController showSessionShellForCurrentSession];
    }

    if (trackPrimaryRole) {
        windowView.workspaceTerminalRole = terminalRole;
        windowView.titleLabel.text = ISHWorkspaceTitleForTerminalRole(terminalRole, terminalViewController.terminal);
    } else {
        windowView.workspaceTerminalRole = ISHWorkspaceTerminalRoleGeneric;
        windowView.titleLabel.text = title;
    }
    [self refreshDockButtons];
}

- (void)openTerminalHerePreferringConsole:(BOOL)preferConsole {
    [self openDesktopTerminalHerePreferringConsole:preferConsole
                                     reuseExisting:YES
                                   trackPrimaryRole:YES];
}

- (UISceneSession *)sceneSessionHostingTerminalUUID:(NSUUID *)terminalUUID {
    NSString *terminalUUIDString = terminalUUID.UUIDString;
    if (terminalUUIDString.length == 0)
        return nil;
    for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
        // Prefer the scene's live root/top view controller: stateRestorationActivity
        // is only populated lazily (e.g. on backgrounding), so a scene that has been
        // foreground the whole time -- such as one opened directly in terminal mode --
        // can have a stale or nil restoration activity even while it's actively hosting
        // the terminal we're looking for.
        UIViewController *rootViewController = ISHWorkspaceRootViewControllerForScene(scene);
        UIViewController *topViewController = ISHWorkspaceTopViewController(rootViewController);
        TerminalViewController *terminalViewController = nil;
        if ([topViewController isKindOfClass:TerminalViewController.class]) {
            terminalViewController = (TerminalViewController *) topViewController;
        } else if ([rootViewController isKindOfClass:TerminalViewController.class]) {
            terminalViewController = (TerminalViewController *) rootViewController;
        }
        if ([terminalViewController.sessionTerminalUUID isEqual:terminalUUID])
            return scene.session;

        NSString *sceneTerminalUUID = scene.session.stateRestorationActivity.userInfo[ISHSceneTerminalUUIDUserInfoKey];
        if ([sceneTerminalUUID isEqualToString:terminalUUIDString])
            return scene.session;
    }
    return nil;
}

- (BOOL)focusSceneSession:(UISceneSession *)sceneSession title:(NSString *)title {
    if (sceneSession == nil)
        return NO;
    [self.view.window endEditing:YES];
    [UIApplication.sharedApplication requestSceneSessionActivation:sceneSession
                                                     userActivity:nil
                                                          options:nil
                                                     errorHandler:^(NSError *error) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self presentSceneActivationError:error title:title];
        });
    }];
    return YES;
}

- (void)focusSceneWithPersistentIdentifier:(NSString *)identifier {
    if (@available(iOS 13.0, *)) {
        [self.view.window endEditing:YES];
        if (identifier.length == 0) {
            [self presentSceneActivationError:nil title:@"Unable to focus window"];
            return;
        }
        UISceneSession *targetSession = ISHWorkspaceSceneSessionWithPersistentIdentifier(identifier);
        if (targetSession == nil) {
            [self presentSceneActivationError:nil title:@"Unable to focus window"];
            return;
        }
        [UIApplication.sharedApplication requestSceneSessionActivation:targetSession
                                                         userActivity:nil
                                                              options:nil
                                                         errorHandler:^(NSError *error) {
            dispatch_async(dispatch_get_main_queue(), ^{
                [self presentSceneActivationError:error title:@"Unable to focus window"];
            });
        }];
    } else {
        return;
    }
}

- (void)closeSceneWithPersistentIdentifier:(NSString *)identifier title:(NSString *)title {
    if (@available(iOS 13.0, *)) {
        UISceneSession *targetSession = ISHWorkspaceSceneSessionWithPersistentIdentifier(identifier);
        if (targetSession == nil) {
            [self presentSceneActivationError:nil title:@"Unable to close workspace"];
            return;
        }
        if (ISHWorkspaceConnectedSceneForSession(targetSession) == nil) {
            ISHWorkspaceForgetHiddenSession(targetSession);
            return;
        }
        [UIApplication.sharedApplication requestSceneSessionDestruction:targetSession
                                                                options:nil
                                                           errorHandler:^(NSError *error) {
            dispatch_async(dispatch_get_main_queue(), ^{
                [self presentSceneActivationError:error title:title ?: @"Unable to close workspace"];
            });
        }];
    } else {
        [self presentSceneActivationError:nil title:@"Unable to close workspace"];
        return;
    }
}

- (NSArray<UISceneSession *> *)hiddenWorkspaceSceneSessions {
    if (@available(iOS 13.0, *)) {
        NSMutableArray<UISceneSession *> *sessions = [NSMutableArray array];
        for (UISceneSession *session in UIApplication.sharedApplication.openSessions) {
            if (![ISHWorkspaceSceneRoleDescription(session) isEqualToString:@"Workspace"])
                continue;
            if (ISHWorkspaceConnectedSceneForSession(session) != nil)
                continue;
            if (ISHWorkspaceHiddenSessionIsForgotten(session))
                continue;
            [sessions addObject:session];
        }
        return sessions;
    } else {
        return @[];
    }
}

- (void)closeHiddenWorkspaceWindows:(id)sender {
    (void) sender;
    if (@available(iOS 13.0, *)) {
        NSArray<UISceneSession *> *sessions = [self hiddenWorkspaceSceneSessions];
        if (sessions.count == 0) {
            [self presentSceneActivationError:nil title:@"No hidden workspace windows"];
            return;
        }
        for (UISceneSession *session in sessions) {
            ISHWorkspaceForgetHiddenSession(session);
            [UIApplication.sharedApplication requestSceneSessionDestruction:session
                                                                    options:nil
                                                               errorHandler:^(__unused NSError *error) {
            }];
        }
    } else {
        [self presentSceneActivationError:nil title:@"Unable to close workspace"];
    }
}

- (void)focusExistingSceneFromButton:(UIButton *)sender {
    if (@available(iOS 13.0, *)) {
        [self focusSceneWithPersistentIdentifier:sender.accessibilityIdentifier];
    } else {
        [self presentSceneActivationError:nil title:@"Unable to focus window"];
    }
}

- (void)openExistingTerminalHereWithUUID:(NSUUID *)terminalUUID {
    Terminal *terminal = [Terminal terminalWithUUID:terminalUUID];
    if (terminal == nil) {
        [self presentSceneActivationError:nil];
        return;
    }
    if (@available(iOS 13.0, *)) {
        UISceneSession *existingSession = [self sceneSessionHostingTerminalUUID:terminalUUID];
        UISceneSession *currentSession = self.view.window.windowScene.session;
        if (existingSession != nil && existingSession != currentSession) {
            [self focusSceneSession:existingSession title:@"Unable to focus terminal window"];
            return;
        }
    }
    ISHWorkspaceContainedWindowView *containedWindow = [self desktopWindowHostingTerminalUUID:terminalUUID];
    if (containedWindow != nil) {
        [self focusDesktopWindow:containedWindow];
        return;
    }
    if (terminal.webView.superview != nil) {
        [self presentSceneActivationError:nil title:@"Terminal already open in another window"];
        return;
    }

    TerminalViewController *terminalViewController = [self createDesktopTerminalViewController];
    if (terminalViewController == nil) {
        [self presentSceneActivationError:nil];
        return;
    }
    [self openDesktopTerminalWindowWithTitle:ISHWorkspaceTerminalDisplayName(terminal)
                      terminalViewController:terminalViewController];
    [terminalViewController reconnectSessionFromTerminalUUID:terminalUUID];
    [self refreshDockButtons];
}

- (void)openNewWorkspaceWindow:(id)sender {
    if (!ISHWorkspaceSupportsSceneWindows()) {
        [self presentSceneActivationError:nil title:@"Workspace windows are unavailable on this device"];
        return;
    }
    [self requestSceneWithActivityType:ISHSceneActivityTypeWorkspace
                               title:@"Unable to open workspace"
                            userInfo:nil];
}

- (void)requestSceneWithActivityType:(NSString *)activityType
                               title:(NSString *)title
                            userInfo:(NSDictionary<NSString *, id> *)userInfo {
    if (@available(iOS 13.0, *)) {
        [self.view.window endEditing:YES];
        NSUserActivity *activity = [[NSUserActivity alloc] initWithActivityType:activityType];
        if (userInfo.count > 0)
            activity.userInfo = userInfo;
        [UIApplication.sharedApplication requestSceneSessionActivation:nil
                                                         userActivity:activity
                                                              options:nil
                                                         errorHandler:^(NSError *error) {
            dispatch_async(dispatch_get_main_queue(), ^{
                [self presentSceneActivationError:error title:title];
            });
        }];
        return;
    }

    [self presentSceneActivationError:nil title:title];
}

- (void)presentSceneActivationError:(NSError *)error {
    [self presentSceneActivationError:error title:@"Unable to open terminal"];
}

- (void)presentSceneActivationError:(NSError *)error title:(NSString *)title {
    NSString *message = @"This device cannot open a separate terminal scene right now.";
    if (error.localizedDescription.length > 0) {
        message = error.localizedDescription;
    }
    UIAlertController *alert =
        [UIAlertController alertControllerWithTitle:title
                                            message:message
                                     preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}

@end

// A self-contained gauge that draws either a ring (default) or a horizontal bar, switching
// live when the workspace gauge-style preference changes. Used by the status applets.
@interface WorkspaceGaugeView : UIView
@property (nonatomic) double progress;
@property (nonatomic) BOOL compact;
@property (nonatomic, copy, nullable) NSString *valueText;
@property (nonatomic, strong, nullable) UIColor *fillColor;
@property (nonatomic, strong, nullable) UIColor *trackColor;
@property (nonatomic, strong, nullable) UIColor *valueColor;
@end

@implementation WorkspaceGaugeView

- (instancetype)initWithFrame:(CGRect)frame {
    if (self = [super initWithFrame:frame]) {
        self.backgroundColor = UIColor.clearColor;
        self.contentMode = UIViewContentModeRedraw;
        _progress = 0.0;
        [NSNotificationCenter.defaultCenter addObserver:self
                                               selector:@selector(gaugeStyleDidChange)
                                                   name:ISHWorkspaceGaugeStyleDidChangeNotification
                                                 object:nil];
    }
    return self;
}

- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:self name:ISHWorkspaceGaugeStyleDidChangeNotification object:nil];
}

- (void)gaugeStyleDidChange {
    [self invalidateIntrinsicContentSize];
    [self setNeedsDisplay];
}

- (void)setProgress:(double)progress { _progress = MAX(0.0, MIN(1.0, progress)); [self setNeedsDisplay]; }
- (void)setValueText:(NSString *)valueText { _valueText = [valueText copy]; [self setNeedsDisplay]; }
- (void)setFillColor:(UIColor *)fillColor { _fillColor = fillColor; [self setNeedsDisplay]; }
- (void)setTrackColor:(UIColor *)trackColor { _trackColor = trackColor; [self setNeedsDisplay]; }
- (void)setValueColor:(UIColor *)valueColor { _valueColor = valueColor; [self setNeedsDisplay]; }
- (void)setCompact:(BOOL)compact {
    if (_compact == compact) return;
    _compact = compact;
    [self invalidateIntrinsicContentSize];
    [self setNeedsDisplay];
}

- (CGSize)intrinsicContentSize {
    if (ISHWorkspaceUsesRingGauges()) {
        CGFloat dim = _compact ? 32.0 : (ISHWorkspaceUsesPhoneLayout() ? 42.0 : 54.0);
        return CGSizeMake(UIViewNoIntrinsicMetric, dim);
    }
    return CGSizeMake(UIViewNoIntrinsicMetric, _compact ? 20.0 : 28.0);
}

- (void)drawRect:(CGRect)rect {
    (void) rect;
    UIColor *fill = self.fillColor ?: UIColor.systemTealColor;
    UIColor *track = self.trackColor ?: [UIColor colorWithWhite:0.5 alpha:0.25];
    UIColor *valueColor = self.valueColor ?: fill;
    NSString *text = self.valueText ?: @"";
    CGRect b = self.bounds;
    double clamped = MAX(0.0, MIN(1.0, self.progress));

    if (ISHWorkspaceUsesRingGauges()) {
        CGFloat dim = MIN(CGRectGetWidth(b), CGRectGetHeight(b));
        CGFloat lineWidth = MAX(6.0, dim * 0.11);
        CGFloat radius = (dim - lineWidth) / 2.0 - 1.0;
        if (radius <= 1.0)
            return;
        CGPoint center = CGPointMake(CGRectGetMidX(b), CGRectGetMidY(b));
        UIBezierPath *trackPath = [UIBezierPath bezierPathWithArcCenter:center radius:radius startAngle:0 endAngle:(2 * M_PI) clockwise:YES];
        trackPath.lineWidth = lineWidth;
        [track setStroke];
        [trackPath stroke];
        if (clamped > 0.0) {
            CGFloat start = -M_PI_2;
            UIBezierPath *progressPath = [UIBezierPath bezierPathWithArcCenter:center radius:radius startAngle:start endAngle:(start + (2 * M_PI * clamped)) clockwise:YES];
            progressPath.lineWidth = lineWidth;
            progressPath.lineCapStyle = kCGLineCapRound;
            [fill setStroke];
            [progressPath stroke];
        }
        if (text.length > 0) {
            UIFont *font = [UIFont systemFontOfSize:MAX(8.5, radius * 0.66) weight:UIFontWeightBold];
            NSMutableParagraphStyle *para = [NSMutableParagraphStyle new];
            para.alignment = NSTextAlignmentCenter;
            NSDictionary *attrs = @{NSFontAttributeName: font, NSForegroundColorAttributeName: valueColor, NSParagraphStyleAttributeName: para};
            CGSize ts = [text sizeWithAttributes:attrs];
            [text drawInRect:CGRectMake(CGRectGetMinX(b), center.y - (ts.height / 2.0), CGRectGetWidth(b), ts.height) withAttributes:attrs];
        }
        return;
    }

    CGFloat barHeight = _compact ? 5.0 : 7.0;
    CGFloat barY = CGRectGetMaxY(b) - barHeight;
    if (text.length > 0) {
        UIFont *font = [UIFont systemFontOfSize:(_compact ? 11.0 : 14.0) weight:UIFontWeightBold];
        NSDictionary *attrs = @{NSFontAttributeName: font, NSForegroundColorAttributeName: valueColor};
        [text drawAtPoint:CGPointMake(CGRectGetMinX(b), CGRectGetMinY(b)) withAttributes:attrs];
    }
    UIBezierPath *barTrack = [UIBezierPath bezierPathWithRoundedRect:CGRectMake(CGRectGetMinX(b), barY, CGRectGetWidth(b), barHeight) cornerRadius:(barHeight / 2.0)];
    [track setFill];
    [barTrack fill];
    CGFloat fillWidth = CGRectGetWidth(b) * clamped;
    if (fillWidth > 0.5) {
        UIBezierPath *barFill = [UIBezierPath bezierPathWithRoundedRect:CGRectMake(CGRectGetMinX(b), barY, MAX(barHeight, fillWidth), barHeight) cornerRadius:(barHeight / 2.0)];
        [fill setFill];
        [barFill fill];
    }
}

@end

@implementation WorkspaceThemedToolViewController {
    CAGradientLayer *_backgroundGradientLayer;
    UIView *_toolContentView;
    NSMutableArray<UIImageView *> *_trackedAccentIconViews;
    NSMutableArray<WorkspaceGaugeView *> *_trackedGaugeViews;
    NSMutableArray<UIView *> *_trackedCardViews;
    NSMutableArray<UILabel *> *_trackedPrimaryLabels;
    NSMutableArray<UILabel *> *_trackedSecondaryLabels;
    NSMutableArray<UILabel *> *_trackedAccentLabels;
    NSMutableArray<UITextView *> *_trackedTextViews;
    NSMutableArray<UIProgressView *> *_trackedProgressViews;
}

- (void)viewDidLoad {
    [super viewDidLoad];

    _trackedCardViews = [NSMutableArray array];
    _trackedPrimaryLabels = [NSMutableArray array];
    _trackedSecondaryLabels = [NSMutableArray array];
    _trackedAccentLabels = [NSMutableArray array];
    _trackedTextViews = [NSMutableArray array];
    _trackedProgressViews = [NSMutableArray array];
    _trackedAccentIconViews = [NSMutableArray array];
    _trackedGaugeViews = [NSMutableArray array];

    self.view.clipsToBounds = YES;
    self.view.backgroundColor = UIColor.blackColor;

    _backgroundGradientLayer = [CAGradientLayer layer];
    _backgroundGradientLayer.startPoint = CGPointMake(0.12, 0.0);
    _backgroundGradientLayer.endPoint = CGPointMake(0.88, 1.0);
    [self.view.layer insertSublayer:_backgroundGradientLayer atIndex:0];

    _toolContentView = [UIView new];
    _toolContentView.translatesAutoresizingMaskIntoConstraints = NO;
    _toolContentView.backgroundColor = UIColor.clearColor;
    [self.view addSubview:_toolContentView];

    [NSLayoutConstraint activateConstraints:@[
        [_toolContentView.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor],
        [_toolContentView.leadingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.leadingAnchor],
        [_toolContentView.trailingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.trailingAnchor],
        [_toolContentView.bottomAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.bottomAnchor],
    ]];

    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(workspaceThemeDidChange:)
                                               name:ISHWorkspaceToolThemeDidChangeNotification
                                             object:nil];
    [self workspaceApplyTheme];
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    // Subclasses build their views after [super viewDidLoad] has already run workspaceApplyTheme,
    // so re-apply it here, once every subclass view exists. Without this a tool's labels keep
    // UILabel's default color until a theme-change notification happens to fire (the bug that
    // left the Clock readout invisible on dark themes).
    [self workspaceApplyTheme];
}

- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:self
                                                  name:ISHWorkspaceToolThemeDidChangeNotification
                                                object:nil];
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    _backgroundGradientLayer.frame = self.view.bounds;
    CGFloat width = CGRectGetWidth(_toolContentView.bounds);
    BOOL compact = (width > 0.0 && width < 330.0);
    for (WorkspaceGaugeView *gaugeView in _trackedGaugeViews) {
        gaugeView.compact = compact;
    }
}

- (UIView *)toolContentView {
    return _toolContentView;
}

- (NSDictionary<NSString *, UIColor *> *)workspaceTheme {
    return ISHWorkspaceThemeDescriptor();
}

- (UILabel *)workspaceThemeLabelWithTextStyle:(UIFontTextStyle)textStyle
                                   monospaced:(BOOL)monospaced
                                      tracker:(NSMutableArray<UILabel *> *)tracker {
    UILabel *label = [UILabel new];
    label.translatesAutoresizingMaskIntoConstraints = NO;
    label.numberOfLines = 0;
    CGFloat pointSize = ISHWorkspaceThemeFontSize(textStyle);
    UIFont *font = [UIFont systemFontOfSize:pointSize];
    if (monospaced) {
        if (@available(iOS 13.0, *)) {
            font = [UIFont monospacedSystemFontOfSize:pointSize weight:UIFontWeightMedium];
        } else {
            font = [UIFont fontWithName:@"Menlo-Regular" size:pointSize] ?: font;
        }
    }
    label.font = font;
    [tracker addObject:label];
    return label;
}

- (UILabel *)workspaceThemePrimaryLabelWithTextStyle:(UIFontTextStyle)textStyle monospaced:(BOOL)monospaced {
    return [self workspaceThemeLabelWithTextStyle:textStyle monospaced:monospaced tracker:_trackedPrimaryLabels];
}

- (UILabel *)workspaceThemeSecondaryLabelWithTextStyle:(UIFontTextStyle)textStyle monospaced:(BOOL)monospaced {
    return [self workspaceThemeLabelWithTextStyle:textStyle monospaced:monospaced tracker:_trackedSecondaryLabels];
}

- (UILabel *)workspaceThemeAccentLabelWithTextStyle:(UIFontTextStyle)textStyle monospaced:(BOOL)monospaced {
    return [self workspaceThemeLabelWithTextStyle:textStyle monospaced:monospaced tracker:_trackedAccentLabels];
}

- (UIView *)workspaceThemeCardView {
    UIView *card = [UIView new];
    card.translatesAutoresizingMaskIntoConstraints = NO;
    CGFloat compactCornerRadius = ISHWorkspaceUsesPhoneLayout() ? 10.0 : 14.0;
    CGFloat roomyCornerRadius = ISHWorkspaceUsesPhoneLayout() ? 16.0 : 22.0;
    card.layer.cornerRadius = ISHWorkspaceDensityValue(compactCornerRadius, roomyCornerRadius);
    card.layer.borderWidth = 1;
    card.layer.shadowColor = UIColor.blackColor.CGColor;
    card.layer.shadowOpacity = 0.16;
    card.layer.shadowRadius = ISHWorkspaceDensityValue(ISHWorkspaceUsesPhoneLayout() ? 7.0 : 10.0,
                                                       ISHWorkspaceUsesPhoneLayout() ? 12.0 : 18.0);
    card.layer.shadowOffset = CGSizeMake(0, ISHWorkspaceDensityValue(ISHWorkspaceUsesPhoneLayout() ? 4.0 : 6.0,
                                                                     ISHWorkspaceUsesPhoneLayout() ? 7.0 : 10.0));
    [_trackedCardViews addObject:card];
    return card;
}

- (UITextView *)workspaceThemeTextView {
    UITextView *textView = [UITextView new];
    textView.translatesAutoresizingMaskIntoConstraints = NO;
    textView.editable = NO;
    textView.alwaysBounceVertical = YES;
    textView.backgroundColor = UIColor.clearColor;
    CGFloat pointSize = ISHWorkspaceDensityValue(ISHWorkspaceUsesPhoneLayout() ? 8.5 : 9.5,
                                                 ISHWorkspaceUsesPhoneLayout() ? 11.0 : 12.0);
    if (@available(iOS 13.0, *)) {
        textView.font = [UIFont monospacedSystemFontOfSize:pointSize weight:UIFontWeightRegular];
    } else {
        textView.font = [UIFont fontWithName:@"Menlo-Regular" size:pointSize] ?: [UIFont systemFontOfSize:pointSize];
    }
    [_trackedTextViews addObject:textView];
    return textView;
}

- (UIProgressView *)workspaceThemeProgressView {
    UIProgressView *progressView = [[UIProgressView alloc] initWithProgressViewStyle:UIProgressViewStyleDefault];
    progressView.translatesAutoresizingMaskIntoConstraints = NO;
    progressView.transform = CGAffineTransformMakeScale(1.0,
                                                        ISHWorkspaceDensityValue(ISHWorkspaceUsesPhoneLayout() ? 0.98 : 1.04,
                                                                                 ISHWorkspaceUsesPhoneLayout() ? 1.16 : 1.28));
    [_trackedProgressViews addObject:progressView];
    return progressView;
}

- (UIImageView *)workspaceThemeAccentIconNamed:(NSString *)symbolName pointSize:(CGFloat)pointSize {
    UIImageView *iconView = [UIImageView new];
    iconView.translatesAutoresizingMaskIntoConstraints = NO;
    iconView.contentMode = UIViewContentModeScaleAspectFit;
    iconView.tintColor = self.workspaceTheme[@"accent"];
    if (@available(iOS 13.0, *)) {
        UIImageSymbolConfiguration *config = [UIImageSymbolConfiguration configurationWithPointSize:pointSize weight:UIImageSymbolWeightSemibold];
        UIImage *image = [UIImage systemImageNamed:symbolName withConfiguration:config];
        iconView.image = [image imageWithRenderingMode:UIImageRenderingModeAlwaysTemplate];
    }
    [iconView setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
    [iconView setContentCompressionResistancePriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
    [_trackedAccentIconViews addObject:iconView];
    return iconView;
}

- (UIStackView *)workspaceTileHeaderRowWithIcon:(NSString *)symbolName caption:(NSString *)caption trailingLabel:(UILabel * __strong *)trailingLabel {
    UIStackView *header = [UIStackView new];
    header.axis = UILayoutConstraintAxisHorizontal;
    header.spacing = 6;
    header.alignment = UIStackViewAlignmentCenter;
    UIImageView *icon = [self workspaceThemeAccentIconNamed:symbolName pointSize:13];
    [icon.widthAnchor constraintEqualToConstant:16].active = YES;
    [icon.heightAnchor constraintEqualToConstant:16].active = YES;
    UILabel *captionLabel = [self workspaceThemeSecondaryLabelWithTextStyle:UIFontTextStyleCaption1 monospaced:NO];
    captionLabel.text = caption;
    captionLabel.font = [UIFont systemFontOfSize:11 weight:UIFontWeightSemibold];
    [header addArrangedSubview:icon];
    [header addArrangedSubview:captionLabel];
    if (trailingLabel != NULL) {
        UILabel *trailing = [self workspaceThemeSecondaryLabelWithTextStyle:UIFontTextStyleCaption1 monospaced:NO];
        trailing.font = [UIFont systemFontOfSize:12 weight:UIFontWeightRegular];
        trailing.textAlignment = NSTextAlignmentRight;
        [captionLabel setContentHuggingPriority:UILayoutPriorityDefaultLow forAxis:UILayoutConstraintAxisHorizontal];
        [header addArrangedSubview:trailing];
        *trailingLabel = trailing;
    }
    return header;
}

- (UIView *)workspaceStatTileWithIcon:(NSString *)symbolName caption:(NSString *)caption valueLabel:(UILabel * __strong *)valueLabel {
    UIView *card = [self workspaceThemeCardView];
    UIStackView *outer = [UIStackView new];
    outer.translatesAutoresizingMaskIntoConstraints = NO;
    outer.axis = UILayoutConstraintAxisVertical;
    outer.spacing = 6;
    outer.alignment = UIStackViewAlignmentLeading;
    [card addSubview:outer];

    UILabel *value = [self workspaceThemePrimaryLabelWithTextStyle:UIFontTextStyleTitle2 monospaced:NO];
    value.font = [UIFont systemFontOfSize:(ISHWorkspaceUsesPhoneLayout() ? 15.0 : 17.0) weight:UIFontWeightSemibold];
    value.numberOfLines = 1;
    value.adjustsFontSizeToFitWidth = YES;
    value.minimumScaleFactor = 0.5;

    [outer addArrangedSubview:[self workspaceTileHeaderRowWithIcon:symbolName caption:caption trailingLabel:NULL]];
    [outer addArrangedSubview:value];

    CGFloat statInsetY = ISHWorkspaceUsesPhoneLayout() ? 5.0 : 7.0;
    [NSLayoutConstraint activateConstraints:@[
        [card.heightAnchor constraintGreaterThanOrEqualToConstant:(ISHWorkspaceUsesPhoneLayout() ? 40.0 : 52.0)],
        [outer.topAnchor constraintEqualToAnchor:card.topAnchor constant:statInsetY],
        [outer.leadingAnchor constraintEqualToAnchor:card.leadingAnchor constant:12],
        [outer.trailingAnchor constraintEqualToAnchor:card.trailingAnchor constant:-12],
        [outer.bottomAnchor constraintEqualToAnchor:card.bottomAnchor constant:-statInsetY],
    ]];
    if (valueLabel != NULL)
        *valueLabel = value;
    return card;
}

- (UIView *)workspaceGaugeTileWithIcon:(NSString *)symbolName caption:(NSString *)caption subtitleLabel:(UILabel * __strong *)subtitleLabel gauge:(WorkspaceGaugeView * __strong *)gauge {
    UIView *card = [self workspaceThemeCardView];
    UIStackView *outer = [UIStackView new];
    outer.translatesAutoresizingMaskIntoConstraints = NO;
    outer.axis = UILayoutConstraintAxisVertical;
    outer.spacing = 5;
    [card addSubview:outer];

    UILabel *subtitle = nil;
    UIStackView *header = [self workspaceTileHeaderRowWithIcon:symbolName caption:caption trailingLabel:&subtitle];

    WorkspaceGaugeView *gaugeView = [[WorkspaceGaugeView alloc] initWithFrame:CGRectZero];
    gaugeView.translatesAutoresizingMaskIntoConstraints = NO;
    [_trackedGaugeViews addObject:gaugeView];

    [outer addArrangedSubview:header];
    [outer addArrangedSubview:gaugeView];

    CGFloat gaugeInsetY = ISHWorkspaceUsesPhoneLayout() ? 5.0 : 7.0;
    [NSLayoutConstraint activateConstraints:@[
        [card.heightAnchor constraintGreaterThanOrEqualToConstant:(ISHWorkspaceUsesPhoneLayout() ? 50.0 : 64.0)],
        [outer.topAnchor constraintEqualToAnchor:card.topAnchor constant:gaugeInsetY],
        [outer.leadingAnchor constraintEqualToAnchor:card.leadingAnchor constant:12],
        [outer.trailingAnchor constraintEqualToAnchor:card.trailingAnchor constant:-12],
        [outer.bottomAnchor constraintEqualToAnchor:card.bottomAnchor constant:-gaugeInsetY],
    ]];
    if (subtitleLabel != NULL)
        *subtitleLabel = subtitle;
    if (gauge != NULL)
        *gauge = gaugeView;
    return card;
}

- (UIView *)workspaceIconRowWithIcon:(NSString *)symbolName title:(NSString *)title valueLabel:(UILabel * __strong *)valueLabel {
    UIStackView *row = [UIStackView new];
    row.translatesAutoresizingMaskIntoConstraints = NO;
    row.axis = UILayoutConstraintAxisHorizontal;
    row.spacing = 10;
    row.alignment = UIStackViewAlignmentCenter;

    UIImageView *icon = [self workspaceThemeAccentIconNamed:symbolName pointSize:12];
    [icon.widthAnchor constraintEqualToConstant:18].active = YES;
    [icon.heightAnchor constraintEqualToConstant:16].active = YES;

    UILabel *titleLabel = [self workspaceThemePrimaryLabelWithTextStyle:UIFontTextStyleSubheadline monospaced:NO];
    titleLabel.text = title;
    titleLabel.font = [UIFont systemFontOfSize:12 weight:UIFontWeightRegular];

    UILabel *value = [self workspaceThemeSecondaryLabelWithTextStyle:UIFontTextStyleSubheadline monospaced:NO];
    value.font = [UIFont systemFontOfSize:12 weight:UIFontWeightSemibold];
    value.textAlignment = NSTextAlignmentRight;
    value.numberOfLines = 1;
    value.adjustsFontSizeToFitWidth = YES;
    value.minimumScaleFactor = 0.6;

    [row addArrangedSubview:icon];
    [row addArrangedSubview:titleLabel];
    [row addArrangedSubview:value];
    [titleLabel setContentHuggingPriority:UILayoutPriorityDefaultLow forAxis:UILayoutConstraintAxisHorizontal];
    [titleLabel setContentCompressionResistancePriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
    [value setContentCompressionResistancePriority:UILayoutPriorityDefaultLow forAxis:UILayoutConstraintAxisHorizontal];

    if (valueLabel != NULL)
        *valueLabel = value;
    return row;
}

- (UIView *)workspaceRowsCardWithRows:(NSArray<UIView *> *)rows {
    UIView *card = [self workspaceThemeCardView];
    UIStackView *stack = [UIStackView new];
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 2;
    for (UIView *row in rows)
        [stack addArrangedSubview:row];
    [card addSubview:stack];
    [NSLayoutConstraint activateConstraints:@[
        [stack.topAnchor constraintEqualToAnchor:card.topAnchor constant:8],
        [stack.leadingAnchor constraintEqualToAnchor:card.leadingAnchor constant:12],
        [stack.trailingAnchor constraintEqualToAnchor:card.trailingAnchor constant:-12],
        [stack.bottomAnchor constraintEqualToAnchor:card.bottomAnchor constant:-8],
    ]];
    return card;
}

- (void)workspaceThemeDidChange:(__unused NSNotification *)notification {
    [self workspaceApplyTheme];
}

- (void)workspaceApplyTheme {
    NSDictionary<NSString *, UIColor *> *theme = self.workspaceTheme;
    _backgroundGradientLayer.colors = @[
        (id) theme[@"backgroundTop"].CGColor,
        (id) theme[@"backgroundBottom"].CGColor,
    ];

    for (UIView *card in _trackedCardViews) {
        card.layer.cornerRadius = ISHWorkspaceDensityValue(14, 22);
        card.layer.shadowRadius = ISHWorkspaceDensityValue(10, 18);
        card.layer.shadowOffset = CGSizeMake(0, ISHWorkspaceDensityValue(6, 10));
        card.backgroundColor = [theme[@"card"] colorWithAlphaComponent:0.95];
        card.layer.borderColor = theme[@"stroke"].CGColor;
    }
    for (UILabel *label in _trackedPrimaryLabels) {
        label.textColor = theme[@"primary"];
    }
    for (UILabel *label in _trackedSecondaryLabels) {
        label.textColor = theme[@"secondary"];
    }
    for (UILabel *label in _trackedAccentLabels) {
        label.textColor = theme[@"accent"];
    }
    for (UITextView *textView in _trackedTextViews) {
        CGFloat pointSize = ISHWorkspaceDensityValue(9.5, 12.0);
        if (@available(iOS 13.0, *)) {
            textView.font = [UIFont monospacedSystemFontOfSize:pointSize weight:UIFontWeightRegular];
        } else {
            textView.font = [UIFont fontWithName:@"Menlo-Regular" size:pointSize] ?: [UIFont systemFontOfSize:pointSize];
        }
        textView.textColor = theme[@"primary"];
        textView.tintColor = theme[@"accent"];
    }
    for (NSUInteger index = 0; index < _trackedProgressViews.count; index++) {
        UIProgressView *progressView = _trackedProgressViews[index];
        progressView.trackTintColor = [theme[@"accentAlt"] colorWithAlphaComponent:0.18];
        progressView.progressTintColor = index % 2 == 0 ? theme[@"accent"] : theme[@"accentAlt"];
    }
    for (UIImageView *iconView in _trackedAccentIconViews) {
        iconView.tintColor = theme[@"accent"];
    }
    UIColor *gaugeTrack = [theme[@"secondary"] colorWithAlphaComponent:0.22];
    for (WorkspaceGaugeView *gaugeView in _trackedGaugeViews) {
        gaugeView.fillColor = theme[@"accent"];
        gaugeView.trackColor = gaugeTrack;
        gaugeView.valueColor = theme[@"primary"];
    }
}

@end

@implementation WorkspaceThemesToolViewController {
    UIScrollView *_scrollView;
    UIStackView *_contentStack;
    UIStackView *_themeListStack;
    UILabel *_activeThemeLabel;
    UILabel *_editorThemeLabel;
    UIView *_previewSurfaceView;
    CAGradientLayer *_previewGradientLayer;
    UILabel *_previewTitleLabel;
    UILabel *_previewBodyLabel;
    UIProgressView *_previewProgressView;
    UIImageView *_backgroundPreviewImageView;
    UILabel *_densityValueLabel;
    UISlider *_densitySlider;
    UISegmentedControl *_gaugeStyleControl;
    NSMutableDictionary<NSString *, UIView *> *_swatchViewsByKey;
    NSMutableDictionary<NSString *, NSMutableDictionary<NSString *, UISlider *> *> *_channelSlidersByKey;
    NSMutableDictionary<NSString *, NSMutableDictionary<NSString *, UILabel *> *> *_channelValueLabelsByKey;
    NSMutableDictionary<NSString *, UIImageView *> *_themePreviewImageViewsByIdentifier;
    NSMutableDictionary<NSString *, UILabel *> *_themeTitleLabelsByIdentifier;
    NSMutableDictionary<NSString *, UILabel *> *_themeDetailLabelsByIdentifier;
    NSMutableArray<UIButton *> *_themeSelectionButtons;
    NSMutableArray<UIButton *> *_editorActionButtons;
    NSString *_editingThemeIdentifier;
}

- (NSString *)themeEditorTitleForKey:(NSString *)key {
    if ([key isEqualToString:@"backgroundTop"])
        return @"Background Top";
    if ([key isEqualToString:@"backgroundBottom"])
        return @"Background Bottom";
    if ([key isEqualToString:@"card"])
        return @"Card Surface";
    if ([key isEqualToString:@"primary"])
        return @"Primary Text";
    if ([key isEqualToString:@"secondary"])
        return @"Secondary Text";
    if ([key isEqualToString:@"accent"])
        return @"Accent";
    if ([key isEqualToString:@"accentAlt"])
        return @"Accent Alt";
    return key;
}

- (NSMutableDictionary<NSString *, NSDictionary<NSString *, NSNumber *> *> *)mutablePaletteForIdentifier:(NSString *)identifier {
    NSMutableDictionary<NSString *, NSDictionary<NSString *, NSNumber *> *> *palette =
        [[ISHWorkspaceThemeEditablePaletteForIdentifier(identifier) mutableCopy] ?: @{}.mutableCopy mutableCopy];
    for (NSString *key in ISHWorkspaceThemeEditableColorKeys()) {
        NSDictionary<NSString *, NSNumber *> *descriptor = palette[key];
        if (![descriptor isKindOfClass:NSDictionary.class]) {
            descriptor = ISHWorkspaceBuiltInThemePalette(ISHWorkspaceToolThemeAuroraIdentifier)[key];
        }
        if (descriptor != nil)
            palette[key] = [descriptor copy];
    }
    return palette;
}

- (NSMutableDictionary<NSString *, NSDictionary<NSString *, NSNumber *> *> *)draftPalette {
    NSMutableDictionary *palette = [NSMutableDictionary dictionary];
    for (NSString *key in ISHWorkspaceThemeEditableColorKeys()) {
        NSMutableDictionary<NSString *, UISlider *> *channels = _channelSlidersByKey[key];
        if (channels == nil)
            continue;
        palette[key] = @{
            @"red": @((NSInteger) lround(channels[@"red"].value)),
            @"green": @((NSInteger) lround(channels[@"green"].value)),
            @"blue": @((NSInteger) lround(channels[@"blue"].value)),
        };
    }
    return palette;
}

- (UIButton *)themeUtilityButtonWithTitle:(NSString *)title selector:(SEL)selector {
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    button.translatesAutoresizingMaskIntoConstraints = NO;
    button.layer.cornerRadius = 10;
    button.layer.borderWidth = 1;
    button.contentEdgeInsets = UIEdgeInsetsMake(7, 10, 7, 10);
    button.titleLabel.font = [UIFont systemFontOfSize:ISHWorkspaceThemeFontSize(UIFontTextStyleCaption1) weight:UIFontWeightSemibold];
    [button setTitle:title forState:UIControlStateNormal];
    [button addTarget:self action:selector forControlEvents:UIControlEventTouchUpInside];
    [_editorActionButtons addObject:button];
    return button;
}

- (UIButton *)themeSelectionButtonWithTitle:(NSString *)title identifier:(NSString *)identifier {
    UIButton *button = [UIButton buttonWithType:UIButtonTypeCustom];
    button.translatesAutoresizingMaskIntoConstraints = NO;
    button.layer.cornerRadius = 12;
    button.layer.borderWidth = 1;
    button.clipsToBounds = NO;
    [button.heightAnchor constraintGreaterThanOrEqualToConstant:68].active = YES;
    button.accessibilityIdentifier = identifier;
    [button addTarget:self action:@selector(selectThemeFromButton:) forControlEvents:UIControlEventTouchUpInside];

    UIStackView *contentRow = [UIStackView new];
    contentRow.translatesAutoresizingMaskIntoConstraints = NO;
    contentRow.axis = UILayoutConstraintAxisHorizontal;
    contentRow.spacing = 8;
    contentRow.alignment = UIStackViewAlignmentCenter;
    contentRow.userInteractionEnabled = NO;
    [button addSubview:contentRow];

    UIImageView *previewView = [[UIImageView alloc] initWithImage:
        ISHWorkspaceThemeArtworkImage(identifier,
                                      ISHWorkspaceThemeEditablePaletteForIdentifier(identifier),
                                      CGSizeMake(92, 52),
                                      NO)];
    previewView.translatesAutoresizingMaskIntoConstraints = NO;
    previewView.contentMode = UIViewContentModeScaleAspectFill;
    previewView.clipsToBounds = YES;
    previewView.layer.cornerRadius = 8;
    previewView.layer.borderWidth = 1;
    [previewView.widthAnchor constraintEqualToConstant:92].active = YES;
    [previewView.heightAnchor constraintEqualToConstant:52].active = YES;
    [contentRow addArrangedSubview:previewView];

    UIStackView *textStack = [UIStackView new];
    textStack.axis = UILayoutConstraintAxisVertical;
    textStack.spacing = 2;
    textStack.alignment = UIStackViewAlignmentLeading;
    textStack.userInteractionEnabled = NO;
    UILabel *titleLabel = [self workspaceThemePrimaryLabelWithTextStyle:UIFontTextStyleBody monospaced:NO];
    titleLabel.font = [UIFont systemFontOfSize:ISHWorkspaceThemeFontSize(UIFontTextStyleSubheadline) weight:UIFontWeightSemibold];
    titleLabel.numberOfLines = 1;
    titleLabel.text = title;
    UILabel *detailLabel = [self workspaceThemeSecondaryLabelWithTextStyle:UIFontTextStyleFootnote monospaced:NO];
    detailLabel.numberOfLines = 2;
    [textStack addArrangedSubview:titleLabel];
    [textStack addArrangedSubview:detailLabel];
    [contentRow addArrangedSubview:textStack];

    [NSLayoutConstraint activateConstraints:@[
        [contentRow.topAnchor constraintEqualToAnchor:button.topAnchor constant:7],
        [contentRow.leadingAnchor constraintEqualToAnchor:button.leadingAnchor constant:8],
        [contentRow.trailingAnchor constraintEqualToAnchor:button.trailingAnchor constant:-8],
        [contentRow.bottomAnchor constraintEqualToAnchor:button.bottomAnchor constant:-7],
    ]];

    _themePreviewImageViewsByIdentifier[identifier] = previewView;
    _themeTitleLabelsByIdentifier[identifier] = titleLabel;
    _themeDetailLabelsByIdentifier[identifier] = detailLabel;
    [_themeSelectionButtons addObject:button];
    return button;
}

- (UIView *)sliderRowWithTitle:(NSString *)title
                         key:(NSString *)key {
    UIView *card = [self workspaceThemeCardView];

    UIStackView *stack = [UIStackView new];
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 3;
    [card addSubview:stack];

    UIStackView *headerRow = [UIStackView new];
    headerRow.axis = UILayoutConstraintAxisHorizontal;
    headerRow.spacing = 8;
    headerRow.alignment = UIStackViewAlignmentCenter;

    UILabel *titleLabel = [self workspaceThemePrimaryLabelWithTextStyle:UIFontTextStyleSubheadline monospaced:NO];
    titleLabel.text = title;
    UIView *swatch = [UIView new];
    swatch.translatesAutoresizingMaskIntoConstraints = NO;
    swatch.layer.cornerRadius = 8;
    swatch.layer.borderWidth = 1;
    [_swatchViewsByKey setObject:swatch forKey:key];
    [swatch.widthAnchor constraintEqualToConstant:34].active = YES;
    [swatch.heightAnchor constraintEqualToConstant:18].active = YES;
    [headerRow addArrangedSubview:titleLabel];
    [headerRow addArrangedSubview:swatch];
    [titleLabel setContentHuggingPriority:UILayoutPriorityDefaultLow forAxis:UILayoutConstraintAxisHorizontal];
    [swatch setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];

    [stack addArrangedSubview:headerRow];

    NSMutableDictionary<NSString *, UISlider *> *channels = [NSMutableDictionary dictionary];
    NSArray<NSDictionary<NSString *, NSString *> *> *channelDescriptors = @[
        @{@"name": @"R", @"key": @"red"},
        @{@"name": @"G", @"key": @"green"},
        @{@"name": @"B", @"key": @"blue"},
    ];
    for (NSDictionary<NSString *, NSString *> *channelDescriptor in channelDescriptors) {
        UIStackView *row = [UIStackView new];
        row.axis = UILayoutConstraintAxisHorizontal;
        row.spacing = 6;
        row.alignment = UIStackViewAlignmentCenter;

        UILabel *channelLabel = [self workspaceThemeSecondaryLabelWithTextStyle:UIFontTextStyleCaption1 monospaced:YES];
        channelLabel.text = channelDescriptor[@"name"];
        channelLabel.textAlignment = NSTextAlignmentCenter;
        [channelLabel.widthAnchor constraintEqualToConstant:16].active = YES;

        UISlider *slider = [UISlider new];
        slider.minimumValue = 0.0f;
        slider.maximumValue = 255.0f;
        slider.accessibilityIdentifier = [NSString stringWithFormat:@"%@:%@", key, channelDescriptor[@"key"]];
        [slider addTarget:self action:@selector(themeSliderChanged:) forControlEvents:UIControlEventValueChanged];

        UILabel *valueLabel = [self workspaceThemeAccentLabelWithTextStyle:UIFontTextStyleCaption1 monospaced:YES];
        valueLabel.textAlignment = NSTextAlignmentRight;
        [valueLabel.widthAnchor constraintEqualToConstant:28].active = YES;

        [row addArrangedSubview:channelLabel];
        [row addArrangedSubview:slider];
        [row addArrangedSubview:valueLabel];
        [channelLabel setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
        [valueLabel setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
        [stack addArrangedSubview:row];
        channels[channelDescriptor[@"key"]] = slider;
        if (_channelValueLabelsByKey[key] == nil)
            _channelValueLabelsByKey[key] = [NSMutableDictionary dictionary];
        _channelValueLabelsByKey[key][channelDescriptor[@"key"]] = valueLabel;
    }
    _channelSlidersByKey[key] = channels;

    [NSLayoutConstraint activateConstraints:@[
        [stack.topAnchor constraintEqualToAnchor:card.topAnchor constant:10],
        [stack.leadingAnchor constraintEqualToAnchor:card.leadingAnchor constant:10],
        [stack.trailingAnchor constraintEqualToAnchor:card.trailingAnchor constant:-10],
        [stack.bottomAnchor constraintEqualToAnchor:card.bottomAnchor constant:-10],
    ]];
    return card;
}

- (void)updateSliderValueLabels {
    for (NSString *key in ISHWorkspaceThemeEditableColorKeys()) {
        NSDictionary<NSString *, UISlider *> *channels = _channelSlidersByKey[key];
        for (NSString *channel in channels) {
            UILabel *label = _channelValueLabelsByKey[key][channel];
            label.text = [NSString stringWithFormat:@"%ld", (long) lround(channels[channel].value)];
        }
    }
}

- (void)clearArrangedSubviewsFromStack:(UIStackView *)stackView {
    NSArray<UIView *> *arrangedSubviews = stackView.arrangedSubviews.copy;
    for (UIView *view in arrangedSubviews) {
        [stackView removeArrangedSubview:view];
        [view removeFromSuperview];
    }
}

- (void)loadThemeIntoEditorWithIdentifier:(NSString *)identifier {
    _editingThemeIdentifier = identifier;
    NSDictionary<NSString *, NSDictionary<NSString *, NSNumber *> *> *palette = [self mutablePaletteForIdentifier:identifier];
    for (NSString *key in ISHWorkspaceThemeEditableColorKeys()) {
        NSDictionary<NSString *, NSNumber *> *descriptor = palette[key];
        NSDictionary<NSString *, UISlider *> *channels = _channelSlidersByKey[key];
        channels[@"red"].value = [descriptor[@"red"] floatValue];
        channels[@"green"].value = [descriptor[@"green"] floatValue];
        channels[@"blue"].value = [descriptor[@"blue"] floatValue];
    }
    [self updateSliderValueLabels];
    [self updateDraftPreview];
    [self refreshThemeSelectionButtons];
}

- (void)refreshThemeSelectionButtons {
    [self clearArrangedSubviewsFromStack:_themeListStack];
    [_themeSelectionButtons removeAllObjects];
    [_themePreviewImageViewsByIdentifier removeAllObjects];
    [_themeTitleLabelsByIdentifier removeAllObjects];
    [_themeDetailLabelsByIdentifier removeAllObjects];

    NSString *currentIdentifier = ISHWorkspaceCurrentThemeIdentifier();
    for (NSDictionary<NSString *, id> *choice in ISHWorkspaceThemeChoices()) {
        NSString *identifier = choice[@"identifier"];
        NSString *title = choice[@"title"];
        NSString *detail = [choice[@"builtIn"] boolValue] ? @"Built-in theme" : @"Saved custom theme";
        UIButton *button = [self themeSelectionButtonWithTitle:title identifier:identifier];
        UILabel *detailLabel = _themeDetailLabelsByIdentifier[identifier];
        detailLabel.text = [identifier isEqualToString:currentIdentifier]
            ? [NSString stringWithFormat:@"Applied • %@", detail]
            : detail;
        [_themeListStack addArrangedSubview:button];
    }
    [self workspaceApplyTheme];
}

- (void)updateDraftPreview {
    NSDictionary<NSString *, NSDictionary<NSString *, NSNumber *> *> *palette = [self draftPalette];
    NSDictionary<NSString *, UIColor *> *previewTheme = ISHWorkspaceThemeDescriptorFromEditablePalette(palette);
    _previewGradientLayer.colors = @[
        (id) previewTheme[@"backgroundTop"].CGColor,
        (id) previewTheme[@"backgroundBottom"].CGColor,
    ];
    _previewSurfaceView.backgroundColor = [previewTheme[@"card"] colorWithAlphaComponent:0.96];
    _previewSurfaceView.layer.borderColor = previewTheme[@"stroke"].CGColor;
    _previewTitleLabel.textColor = previewTheme[@"accent"];
    _previewBodyLabel.textColor = previewTheme[@"primary"];
    _previewProgressView.trackTintColor = [previewTheme[@"accentAlt"] colorWithAlphaComponent:0.18];
    _previewProgressView.progressTintColor = previewTheme[@"accentAlt"];
    _previewProgressView.progress = 0.72f;
    _backgroundPreviewImageView.image = ISHWorkspaceThemeArtworkImage(_editingThemeIdentifier,
                                                                      palette,
                                                                      CGSizeMake(1200, 675),
                                                                      YES);
    _backgroundPreviewImageView.layer.borderColor = previewTheme[@"stroke"].CGColor;
    _densityValueLabel.text = ISHWorkspaceCurrentDensityTitle();

    for (NSString *key in ISHWorkspaceThemeEditableColorKeys()) {
        UIView *swatch = _swatchViewsByKey[key];
        UIColor *color = ISHWorkspaceThemeColorFromDescriptor(palette[key]);
        swatch.backgroundColor = color;
        swatch.layer.borderColor = [previewTheme[@"stroke"] colorWithAlphaComponent:0.9].CGColor;
    }

    NSDictionary<NSString *, id> *record = ISHWorkspaceThemeRecordForIdentifier(_editingThemeIdentifier);
    NSString *title = record[@"title"];
    if (title.length == 0)
        title = @"Draft Theme";
    _editorThemeLabel.text = [NSString stringWithFormat:@"Editing: %@", title];
}

- (void)themeSliderChanged:(UISlider *)sender {
    sender.value = roundf(sender.value);
    [self updateSliderValueLabels];
    [self updateDraftPreview];
}

- (void)selectThemeFromButton:(UIButton *)sender {
    NSString *identifier = sender.accessibilityIdentifier;
    if (identifier.length == 0)
        return;
    ISHWorkspaceSetCurrentThemeIdentifier(identifier);
    [self loadThemeIntoEditorWithIdentifier:identifier];
}

- (void)saveThemeAsNew:(id)sender {
    (void) sender;
    UIAlertController *alert =
        [UIAlertController alertControllerWithTitle:@"Save Theme"
                                            message:@"Save the current editor palette as a custom Workspace theme."
                                     preferredStyle:UIAlertControllerStyleAlert];
    [alert addTextFieldWithConfigurationHandler:^(UITextField *textField) {
        textField.placeholder = @"Theme name";
        textField.text = @"Custom Theme";
    }];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel"
                                              style:UIAlertActionStyleCancel
                                            handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Save"
                                              style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction *action) {
        NSString *title = alert.textFields.firstObject.text ?: @"";
        title = [title stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        if (title.length == 0)
            title = @"Custom Theme";
        NSString *identifier = ISHWorkspaceCreateCustomThemeIdentifier();
        ISHWorkspaceSaveCustomThemeRecord(identifier, title, [self draftPalette]);
        ISHWorkspaceSetCurrentThemeIdentifier(identifier);
        [self loadThemeIntoEditorWithIdentifier:identifier];
        [self refreshThemeSelectionButtons];
    }]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)updateSelectedCustomTheme:(id)sender {
    (void) sender;
    if (ISHWorkspaceThemeIdentifierIsBuiltIn(_editingThemeIdentifier))
        return;
    NSDictionary<NSString *, id> *record = ISHWorkspaceThemeRecordForIdentifier(_editingThemeIdentifier);
    NSString *title = record[@"title"] ?: @"Custom Theme";
    ISHWorkspaceSaveCustomThemeRecord(_editingThemeIdentifier, title, [self draftPalette]);
    ISHWorkspaceSetCurrentThemeIdentifier(_editingThemeIdentifier);
    [self refreshThemeSelectionButtons];
    [self loadThemeIntoEditorWithIdentifier:_editingThemeIdentifier];
}

- (void)deleteSelectedCustomTheme:(id)sender {
    (void) sender;
    if (ISHWorkspaceThemeIdentifierIsBuiltIn(_editingThemeIdentifier))
        return;
    NSDictionary<NSString *, id> *record = ISHWorkspaceThemeRecordForIdentifier(_editingThemeIdentifier);
    NSString *title = record[@"title"] ?: @"this theme";
    UIAlertController *alert =
        [UIAlertController alertControllerWithTitle:@"Delete Theme?"
                                            message:[NSString stringWithFormat:@"Remove %@ from saved custom themes.", title]
                                     preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel"
                                              style:UIAlertActionStyleCancel
                                            handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Delete"
                                              style:UIAlertActionStyleDestructive
                                            handler:^(__unused UIAlertAction *action) {
        ISHWorkspaceDeleteCustomThemeRecord(self->_editingThemeIdentifier);
        ISHWorkspaceSetCurrentThemeIdentifier(ISHWorkspaceToolThemeAuroraIdentifier);
        [self loadThemeIntoEditorWithIdentifier:ISHWorkspaceCurrentThemeIdentifier()];
        [self refreshThemeSelectionButtons];
    }]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)densitySliderChanged:(UISlider *)sender {
    sender.value = roundf(sender.value * 100.0f) / 100.0f;
    ISHWorkspaceSetCurrentDensity(sender.value);
    _densityValueLabel.text = ISHWorkspaceCurrentDensityTitle();
}

- (void)gaugeStyleControlChanged:(UISegmentedControl *)sender {
    ISHWorkspaceSetUsesRingGauges(sender.selectedSegmentIndex == 0);
}

- (void)generateThemeBackgroundImage:(id)sender {
    (void) sender;
    WorkspaceViewController *workspaceViewController = self.workspaceHostViewController;
    if (workspaceViewController == nil)
        return;
    CGSize targetSize = workspaceViewController.desktopSurfaceView.bounds.size;
    if (targetSize.width <= 1 || targetSize.height <= 1)
        targetSize = UIScreen.mainScreen.bounds.size;
    CGFloat scale = MAX(UIScreen.mainScreen.scale, 2.0);
    CGSize imageSize = CGSizeMake(MAX(1.0, targetSize.width * scale), MAX(1.0, targetSize.height * scale));
    UIImage *image = ISHWorkspaceThemeArtworkImage(_editingThemeIdentifier,
                                                   [self draftPalette],
                                                   imageSize,
                                                   YES);
    if (image == nil)
        return;
    [workspaceViewController applyWorkspaceWallpaperImage:image];
    _backgroundPreviewImageView.image = image;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Themes";

    _swatchViewsByKey = [NSMutableDictionary dictionary];
    _channelSlidersByKey = [NSMutableDictionary dictionary];
    _channelValueLabelsByKey = [NSMutableDictionary dictionary];
    _themePreviewImageViewsByIdentifier = [NSMutableDictionary dictionary];
    _themeTitleLabelsByIdentifier = [NSMutableDictionary dictionary];
    _themeDetailLabelsByIdentifier = [NSMutableDictionary dictionary];
    _themeSelectionButtons = [NSMutableArray array];
    _editorActionButtons = [NSMutableArray array];

    _scrollView = [UIScrollView new];
    _scrollView.translatesAutoresizingMaskIntoConstraints = NO;
    _scrollView.alwaysBounceVertical = YES;
    [self.toolContentView addSubview:_scrollView];

    _contentStack = [UIStackView new];
    _contentStack.translatesAutoresizingMaskIntoConstraints = NO;
    _contentStack.axis = UILayoutConstraintAxisVertical;
    _contentStack.spacing = 12;
    [_scrollView addSubview:_contentStack];

    UIView *headerCard = [self workspaceThemeCardView];
    UIStackView *headerStack = [UIStackView new];
    headerStack.translatesAutoresizingMaskIntoConstraints = NO;
    headerStack.axis = UILayoutConstraintAxisVertical;
    headerStack.spacing = 6;
    [headerCard addSubview:headerStack];
    UILabel *headerEyebrow = [self workspaceThemeSecondaryLabelWithTextStyle:UIFontTextStyleCaption1 monospaced:NO];
    headerEyebrow.text = @"WORKSPACE THEMES";
    headerEyebrow.font = [UIFont systemFontOfSize:11 weight:UIFontWeightSemibold];
    _activeThemeLabel = [self workspaceThemeAccentLabelWithTextStyle:UIFontTextStyleTitle2 monospaced:NO];
    _activeThemeLabel.numberOfLines = 0;
    UILabel *headerBody = [self workspaceThemePrimaryLabelWithTextStyle:UIFontTextStyleBody monospaced:NO];
    headerBody.text = @"Change themes from one native utility, then fine-tune and save custom palettes for every Workspace app.";
    [headerStack addArrangedSubview:headerEyebrow];
    [headerStack addArrangedSubview:_activeThemeLabel];
    [headerStack addArrangedSubview:headerBody];
    [NSLayoutConstraint activateConstraints:@[
        [headerStack.topAnchor constraintEqualToAnchor:headerCard.topAnchor constant:14],
        [headerStack.leadingAnchor constraintEqualToAnchor:headerCard.leadingAnchor constant:14],
        [headerStack.trailingAnchor constraintEqualToAnchor:headerCard.trailingAnchor constant:-14],
        [headerStack.bottomAnchor constraintEqualToAnchor:headerCard.bottomAnchor constant:-14],
    ]];

    UIView *libraryCard = [self workspaceThemeCardView];
    UIStackView *libraryStack = [UIStackView new];
    libraryStack.translatesAutoresizingMaskIntoConstraints = NO;
    libraryStack.axis = UILayoutConstraintAxisVertical;
    libraryStack.spacing = 8;
    [libraryCard addSubview:libraryStack];
    UILabel *libraryTitle = [self workspaceThemePrimaryLabelWithTextStyle:UIFontTextStyleHeadline monospaced:NO];
    libraryTitle.text = @"Theme Library";
    UILabel *librarySubtitle = [self workspaceThemeSecondaryLabelWithTextStyle:UIFontTextStyleFootnote monospaced:NO];
    librarySubtitle.text = @"Tap any theme to apply it everywhere and load it into the editor below.";
    _themeListStack = [UIStackView new];
    _themeListStack.axis = UILayoutConstraintAxisVertical;
    _themeListStack.spacing = 6;
    [libraryStack addArrangedSubview:libraryTitle];
    [libraryStack addArrangedSubview:librarySubtitle];
    [libraryStack addArrangedSubview:_themeListStack];
    [NSLayoutConstraint activateConstraints:@[
        [libraryStack.topAnchor constraintEqualToAnchor:libraryCard.topAnchor constant:14],
        [libraryStack.leadingAnchor constraintEqualToAnchor:libraryCard.leadingAnchor constant:14],
        [libraryStack.trailingAnchor constraintEqualToAnchor:libraryCard.trailingAnchor constant:-14],
        [libraryStack.bottomAnchor constraintEqualToAnchor:libraryCard.bottomAnchor constant:-14],
    ]];

    UIView *editorCard = [self workspaceThemeCardView];
    UIStackView *editorStack = [UIStackView new];
    editorStack.translatesAutoresizingMaskIntoConstraints = NO;
    editorStack.axis = UILayoutConstraintAxisVertical;
    editorStack.spacing = 10;
    [editorCard addSubview:editorStack];
    UILabel *editorTitle = [self workspaceThemePrimaryLabelWithTextStyle:UIFontTextStyleHeadline monospaced:NO];
    editorTitle.text = @"Palette Editor";
    _editorThemeLabel = [self workspaceThemeSecondaryLabelWithTextStyle:UIFontTextStyleFootnote monospaced:NO];

    _previewSurfaceView = [UIView new];
    _previewSurfaceView.translatesAutoresizingMaskIntoConstraints = NO;
    _previewSurfaceView.layer.cornerRadius = 18;
    _previewSurfaceView.layer.borderWidth = 1;
    _previewSurfaceView.layer.masksToBounds = YES;
    _previewGradientLayer = [CAGradientLayer layer];
    _previewGradientLayer.startPoint = CGPointMake(0.1, 0.0);
    _previewGradientLayer.endPoint = CGPointMake(0.9, 1.0);
    [_previewSurfaceView.layer insertSublayer:_previewGradientLayer atIndex:0];

    UIStackView *previewStack = [UIStackView new];
    previewStack.translatesAutoresizingMaskIntoConstraints = NO;
    previewStack.axis = UILayoutConstraintAxisVertical;
    previewStack.spacing = 6;
    [_previewSurfaceView addSubview:previewStack];
    _previewTitleLabel = [UILabel new];
    _previewTitleLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleTitle3];
    _previewTitleLabel.text = @"Preview";
    _previewBodyLabel = [UILabel new];
    _previewBodyLabel.numberOfLines = 0;
    _previewBodyLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    _previewBodyLabel.text = @"Buttons, cards, text, and monitor bars will all update when you apply this theme.";
    _previewProgressView = [[UIProgressView alloc] initWithProgressViewStyle:UIProgressViewStyleDefault];
    _previewProgressView.translatesAutoresizingMaskIntoConstraints = NO;
    _previewProgressView.transform = CGAffineTransformMakeScale(1.0, 1.4);
    [previewStack addArrangedSubview:_previewTitleLabel];
    [previewStack addArrangedSubview:_previewBodyLabel];
    [previewStack addArrangedSubview:_previewProgressView];
    [NSLayoutConstraint activateConstraints:@[
        [_previewSurfaceView.heightAnchor constraintGreaterThanOrEqualToConstant:126],
        [previewStack.topAnchor constraintEqualToAnchor:_previewSurfaceView.topAnchor constant:14],
        [previewStack.leadingAnchor constraintEqualToAnchor:_previewSurfaceView.leadingAnchor constant:14],
        [previewStack.trailingAnchor constraintEqualToAnchor:_previewSurfaceView.trailingAnchor constant:-14],
        [previewStack.bottomAnchor constraintEqualToAnchor:_previewSurfaceView.bottomAnchor constant:-14],
    ]];

    UIView *backgroundCard = [self workspaceThemeCardView];
    UIStackView *backgroundStack = [UIStackView new];
    backgroundStack.translatesAutoresizingMaskIntoConstraints = NO;
    backgroundStack.axis = UILayoutConstraintAxisVertical;
    backgroundStack.spacing = 6;
    [backgroundCard addSubview:backgroundStack];
    UILabel *backgroundTitle = [self workspaceThemePrimaryLabelWithTextStyle:UIFontTextStyleSubheadline monospaced:NO];
    backgroundTitle.text = @"Wallpaper Preview";
    UILabel *backgroundSubtitle = [self workspaceThemeSecondaryLabelWithTextStyle:UIFontTextStyleCaption1 monospaced:NO];
    backgroundSubtitle.text = @"Generated from the current palette and sized for the current screen.";
    _backgroundPreviewImageView = [UIImageView new];
    _backgroundPreviewImageView.translatesAutoresizingMaskIntoConstraints = NO;
    _backgroundPreviewImageView.contentMode = UIViewContentModeScaleAspectFill;
    _backgroundPreviewImageView.clipsToBounds = YES;
    _backgroundPreviewImageView.layer.cornerRadius = 12;
    _backgroundPreviewImageView.layer.borderWidth = 1;
    [backgroundStack addArrangedSubview:backgroundTitle];
    [backgroundStack addArrangedSubview:backgroundSubtitle];
    [backgroundStack addArrangedSubview:_backgroundPreviewImageView];
    [NSLayoutConstraint activateConstraints:@[
        [_backgroundPreviewImageView.heightAnchor constraintEqualToConstant:124],
        [backgroundStack.topAnchor constraintEqualToAnchor:backgroundCard.topAnchor constant:14],
        [backgroundStack.leadingAnchor constraintEqualToAnchor:backgroundCard.leadingAnchor constant:14],
        [backgroundStack.trailingAnchor constraintEqualToAnchor:backgroundCard.trailingAnchor constant:-14],
        [backgroundStack.bottomAnchor constraintEqualToAnchor:backgroundCard.bottomAnchor constant:-14],
    ]];

    UIView *densityCard = [self workspaceThemeCardView];
    UIStackView *densityStack = [UIStackView new];
    densityStack.translatesAutoresizingMaskIntoConstraints = NO;
    densityStack.axis = UILayoutConstraintAxisVertical;
    densityStack.spacing = 6;
    [densityCard addSubview:densityStack];
    UILabel *densityTitle = [self workspaceThemePrimaryLabelWithTextStyle:UIFontTextStyleSubheadline monospaced:NO];
    densityTitle.text = @"Utility Density";
    _densityValueLabel = [self workspaceThemeAccentLabelWithTextStyle:UIFontTextStyleSubheadline monospaced:NO];
    _densitySlider = [UISlider new];
    _densitySlider.minimumValue = 0.0f;
    _densitySlider.maximumValue = 1.0f;
    _densitySlider.value = (float) ISHWorkspaceCurrentDensity();
    [_densitySlider addTarget:self action:@selector(densitySliderChanged:) forControlEvents:UIControlEventValueChanged];
    UILabel *densitySubtitle = [self workspaceThemeSecondaryLabelWithTextStyle:UIFontTextStyleCaption1 monospaced:NO];
    densitySubtitle.text = @"Lower values pack the utility cards tighter for smaller screens.";
    [densityStack addArrangedSubview:densityTitle];
    [densityStack addArrangedSubview:_densityValueLabel];
    [densityStack addArrangedSubview:_densitySlider];
    [densityStack addArrangedSubview:densitySubtitle];
    [NSLayoutConstraint activateConstraints:@[
        [densityStack.topAnchor constraintEqualToAnchor:densityCard.topAnchor constant:14],
        [densityStack.leadingAnchor constraintEqualToAnchor:densityCard.leadingAnchor constant:14],
        [densityStack.trailingAnchor constraintEqualToAnchor:densityCard.trailingAnchor constant:-14],
        [densityStack.bottomAnchor constraintEqualToAnchor:densityCard.bottomAnchor constant:-14],
    ]];

    UIView *gaugeCard = [self workspaceThemeCardView];
    UIStackView *gaugeStack = [UIStackView new];
    gaugeStack.translatesAutoresizingMaskIntoConstraints = NO;
    gaugeStack.axis = UILayoutConstraintAxisVertical;
    gaugeStack.spacing = 6;
    [gaugeCard addSubview:gaugeStack];
    UILabel *gaugeTitle = [self workspaceThemePrimaryLabelWithTextStyle:UIFontTextStyleSubheadline monospaced:NO];
    gaugeTitle.text = @"Gauge Style";
    _gaugeStyleControl = [[UISegmentedControl alloc] initWithItems:@[@"Rings", @"Bars"]];
    _gaugeStyleControl.selectedSegmentIndex = ISHWorkspaceUsesRingGauges() ? 0 : 1;
    [_gaugeStyleControl addTarget:self action:@selector(gaugeStyleControlChanged:) forControlEvents:UIControlEventValueChanged];
    UILabel *gaugeSubtitle = [self workspaceThemeSecondaryLabelWithTextStyle:UIFontTextStyleCaption1 monospaced:NO];
    gaugeSubtitle.text = @"How the status applets draw CPU, memory, battery, and storage.";
    [gaugeStack addArrangedSubview:gaugeTitle];
    [gaugeStack addArrangedSubview:_gaugeStyleControl];
    [gaugeStack addArrangedSubview:gaugeSubtitle];
    [NSLayoutConstraint activateConstraints:@[
        [gaugeStack.topAnchor constraintEqualToAnchor:gaugeCard.topAnchor constant:14],
        [gaugeStack.leadingAnchor constraintEqualToAnchor:gaugeCard.leadingAnchor constant:14],
        [gaugeStack.trailingAnchor constraintEqualToAnchor:gaugeCard.trailingAnchor constant:-14],
        [gaugeStack.bottomAnchor constraintEqualToAnchor:gaugeCard.bottomAnchor constant:-14],
    ]];

    UIStackView *actionStack = [UIStackView new];
    actionStack.axis = UILayoutConstraintAxisVertical;
    actionStack.spacing = 8;

    UIStackView *actionRowTop = [UIStackView new];
    actionRowTop.axis = UILayoutConstraintAxisHorizontal;
    actionRowTop.spacing = 8;
    actionRowTop.distribution = UIStackViewDistributionFillEqually;
    [actionRowTop addArrangedSubview:[self themeUtilityButtonWithTitle:@"Save As New"
                                                              selector:@selector(saveThemeAsNew:)]];
    [actionRowTop addArrangedSubview:[self themeUtilityButtonWithTitle:@"Update Selected"
                                                              selector:@selector(updateSelectedCustomTheme:)]];

    UIStackView *actionRowBottom = [UIStackView new];
    actionRowBottom.axis = UILayoutConstraintAxisHorizontal;
    actionRowBottom.spacing = 8;
    actionRowBottom.distribution = UIStackViewDistributionFillEqually;
    [actionRowBottom addArrangedSubview:[self themeUtilityButtonWithTitle:@"Delete Selected"
                                                                 selector:@selector(deleteSelectedCustomTheme:)]];
    [actionRowBottom addArrangedSubview:[self themeUtilityButtonWithTitle:@"Apply Wallpaper"
                                                                 selector:@selector(generateThemeBackgroundImage:)]];

    [actionStack addArrangedSubview:actionRowTop];
    [actionStack addArrangedSubview:actionRowBottom];

    [editorStack addArrangedSubview:editorTitle];
    [editorStack addArrangedSubview:_editorThemeLabel];
    [editorStack addArrangedSubview:_previewSurfaceView];
    [editorStack addArrangedSubview:densityCard];
    [editorStack addArrangedSubview:gaugeCard];
    [editorStack addArrangedSubview:actionStack];
    for (NSString *key in ISHWorkspaceThemeEditableColorKeys()) {
        [editorStack addArrangedSubview:[self sliderRowWithTitle:[self themeEditorTitleForKey:key] key:key]];
    }
    [NSLayoutConstraint activateConstraints:@[
        [editorStack.topAnchor constraintEqualToAnchor:editorCard.topAnchor constant:14],
        [editorStack.leadingAnchor constraintEqualToAnchor:editorCard.leadingAnchor constant:14],
        [editorStack.trailingAnchor constraintEqualToAnchor:editorCard.trailingAnchor constant:-14],
        [editorStack.bottomAnchor constraintEqualToAnchor:editorCard.bottomAnchor constant:-14],
    ]];

    [_contentStack addArrangedSubview:headerCard];
    [_contentStack addArrangedSubview:backgroundCard];
    [_contentStack addArrangedSubview:libraryCard];
    [_contentStack addArrangedSubview:editorCard];

    [NSLayoutConstraint activateConstraints:@[
        [_scrollView.topAnchor constraintEqualToAnchor:self.toolContentView.topAnchor],
        [_scrollView.leadingAnchor constraintEqualToAnchor:self.toolContentView.leadingAnchor],
        [_scrollView.trailingAnchor constraintEqualToAnchor:self.toolContentView.trailingAnchor],
        [_scrollView.bottomAnchor constraintEqualToAnchor:self.toolContentView.bottomAnchor],

        [_contentStack.topAnchor constraintEqualToAnchor:_scrollView.contentLayoutGuide.topAnchor constant:14],
        [_contentStack.leadingAnchor constraintEqualToAnchor:_scrollView.frameLayoutGuide.leadingAnchor constant:14],
        [_contentStack.trailingAnchor constraintEqualToAnchor:_scrollView.frameLayoutGuide.trailingAnchor constant:-14],
        [_contentStack.bottomAnchor constraintEqualToAnchor:_scrollView.contentLayoutGuide.bottomAnchor constant:-14],
        [_contentStack.widthAnchor constraintEqualToAnchor:_scrollView.frameLayoutGuide.widthAnchor constant:-28],
    ]];

    [self refreshThemeSelectionButtons];
    [self loadThemeIntoEditorWithIdentifier:ISHWorkspaceCurrentThemeIdentifier()];
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    _previewGradientLayer.frame = _previewSurfaceView.bounds;
}

- (void)workspaceThemeDidChange:(NSNotification *)notification {
    [super workspaceThemeDidChange:notification];
    _activeThemeLabel.text = [NSString stringWithFormat:@"Active Theme: %@", ISHWorkspaceCurrentThemeTitle()];
    [self refreshThemeSelectionButtons];
    if (_editingThemeIdentifier.length == 0 || ISHWorkspaceThemeRecordForIdentifier(_editingThemeIdentifier) == nil) {
        [self loadThemeIntoEditorWithIdentifier:ISHWorkspaceCurrentThemeIdentifier()];
    }
}

- (void)workspaceApplyTheme {
    [super workspaceApplyTheme];
    NSDictionary<NSString *, UIColor *> *theme = self.workspaceTheme;
    _activeThemeLabel.text = [NSString stringWithFormat:@"Active Theme: %@", ISHWorkspaceCurrentThemeTitle()];

    for (UIButton *button in _themeSelectionButtons) {
        NSString *identifier = button.accessibilityIdentifier;
        BOOL selected = [identifier isEqualToString:ISHWorkspaceCurrentThemeIdentifier()];
        if (selected) {
            button.accessibilityTraits |= UIAccessibilityTraitSelected;
        } else {
            button.accessibilityTraits &= ~UIAccessibilityTraitSelected;
        }
        button.backgroundColor = selected
            ? [theme[@"accent"] colorWithAlphaComponent:0.34]
            : [theme[@"cardAlt"] colorWithAlphaComponent:0.92];
        button.layer.borderWidth = selected ? 2.0 : 1.0;
        button.layer.borderColor = (selected ? theme[@"accentAlt"] : theme[@"stroke"]).CGColor;
        button.layer.shadowColor = selected ? theme[@"accent"].CGColor : UIColor.clearColor.CGColor;
        button.layer.shadowOpacity = selected ? 0.35 : 0.0;
        button.layer.shadowRadius = selected ? 10.0 : 0.0;
        button.layer.shadowOffset = CGSizeMake(0, 0);
        _themePreviewImageViewsByIdentifier[identifier].layer.borderColor =
            (selected ? theme[@"accentAlt"] : theme[@"stroke"]).CGColor;
        _themeTitleLabelsByIdentifier[identifier].textColor = selected ? theme[@"card"] : theme[@"primary"];
        _themeDetailLabelsByIdentifier[identifier].textColor = selected ? theme[@"cardAlt"] : theme[@"secondary"];
    }
    BOOL editingCustom = !ISHWorkspaceThemeIdentifierIsBuiltIn(_editingThemeIdentifier);
    for (UIButton *button in _editorActionButtons) {
        button.backgroundColor = [theme[@"cardAlt"] colorWithAlphaComponent:0.92];
        button.layer.borderColor = theme[@"stroke"].CGColor;
        [button setTitleColor:theme[@"accent"] forState:UIControlStateNormal];
    }
    _backgroundPreviewImageView.layer.borderColor = theme[@"stroke"].CGColor;
    _densitySlider.minimumTrackTintColor = theme[@"accent"];
    _densitySlider.maximumTrackTintColor = [theme[@"accentAlt"] colorWithAlphaComponent:0.24];
    _densityValueLabel.text = ISHWorkspaceCurrentDensityTitle();
    if (@available(iOS 13.0, *)) {
        _gaugeStyleControl.selectedSegmentTintColor = theme[@"accent"];
    } else {
        _gaugeStyleControl.tintColor = theme[@"accent"];
    }
    [_gaugeStyleControl setTitleTextAttributes:@{NSForegroundColorAttributeName: theme[@"secondary"]} forState:UIControlStateNormal];
    [_gaugeStyleControl setTitleTextAttributes:@{NSForegroundColorAttributeName: theme[@"card"]} forState:UIControlStateSelected];
    if (_editorActionButtons.count >= 3) {
        _editorActionButtons[1].enabled = editingCustom;
        _editorActionButtons[1].alpha = editingCustom ? 1.0 : 0.45;
        _editorActionButtons[2].enabled = editingCustom;
        _editorActionButtons[2].alpha = editingCustom ? 1.0 : 0.45;
    }
}

@end

@implementation WorkspaceSessionsToolViewController {
    UIScrollView *_scrollView;
    UIStackView *_contentStack;
    UILabel *_summaryLabel;
    UIStackView *_quickActionsStack;
    UIStackView *_sessionButtonsStack;
    NSMutableArray<UIButton *> *_trackedButtons;
}

- (UIButton *)sessionButtonWithTitle:(NSString *)title subtitle:(NSString *)subtitle selector:(SEL)selector {
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    button.translatesAutoresizingMaskIntoConstraints = NO;
    button.contentEdgeInsets = UIEdgeInsetsMake(7, 10, 7, 10);
    button.contentHorizontalAlignment = UIControlContentHorizontalAlignmentLeft;
    button.titleLabel.numberOfLines = 0;
    button.layer.cornerRadius = 12;
    button.layer.borderWidth = 1;

    NSMutableParagraphStyle *style = [NSMutableParagraphStyle new];
    style.alignment = NSTextAlignmentLeft;
    NSMutableAttributedString *titleString =
        [[NSMutableAttributedString alloc] initWithString:title
                                               attributes:@{
        NSFontAttributeName: [UIFont systemFontOfSize:ISHWorkspaceThemeFontSize(UIFontTextStyleSubheadline)
                                               weight:UIFontWeightSemibold],
        NSParagraphStyleAttributeName: style,
    }];
    [titleString appendAttributedString:[[NSAttributedString alloc] initWithString:@"\n"
                                                                        attributes:@{
        NSFontAttributeName: [UIFont systemFontOfSize:ISHWorkspaceThemeFontSize(UIFontTextStyleCaption1)
                                               weight:UIFontWeightMedium],
        NSParagraphStyleAttributeName: style,
    }]];
    [titleString appendAttributedString:[[NSAttributedString alloc] initWithString:subtitle
                                                                        attributes:@{
        NSFontAttributeName: [UIFont systemFontOfSize:ISHWorkspaceThemeFontSize(UIFontTextStyleCaption1)
                                               weight:UIFontWeightMedium],
        NSParagraphStyleAttributeName: style,
    }]];
    [button setAttributedTitle:titleString forState:UIControlStateNormal];
    [button addTarget:self action:selector forControlEvents:UIControlEventTouchUpInside];
    [_trackedButtons addObject:button];
    return button;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Sessions";
    _trackedButtons = [NSMutableArray array];

    _scrollView = [UIScrollView new];
    _scrollView.translatesAutoresizingMaskIntoConstraints = NO;
    _scrollView.alwaysBounceVertical = YES;
    [self.toolContentView addSubview:_scrollView];

    _contentStack = [UIStackView new];
    _contentStack.translatesAutoresizingMaskIntoConstraints = NO;
    _contentStack.axis = UILayoutConstraintAxisVertical;
    _contentStack.spacing = 8;
    [_scrollView addSubview:_contentStack];

    UIView *summaryCard = [self workspaceThemeCardView];
    UIStackView *summaryStack = [UIStackView new];
    summaryStack.translatesAutoresizingMaskIntoConstraints = NO;
    summaryStack.axis = UILayoutConstraintAxisVertical;
    summaryStack.spacing = 6;
    [summaryCard addSubview:summaryStack];
    UILabel *summaryTitle = [self workspaceThemeSecondaryLabelWithTextStyle:UIFontTextStyleCaption1 monospaced:NO];
    summaryTitle.text = @"ACTIVE SESSIONS";
    summaryTitle.font = [UIFont systemFontOfSize:9 weight:UIFontWeightSemibold];
    _summaryLabel = [self workspaceThemeAccentLabelWithTextStyle:UIFontTextStyleHeadline monospaced:NO];
    _summaryLabel.numberOfLines = 0;
    [summaryStack addArrangedSubview:summaryTitle];
    [summaryStack addArrangedSubview:_summaryLabel];
    [NSLayoutConstraint activateConstraints:@[
        [summaryStack.topAnchor constraintEqualToAnchor:summaryCard.topAnchor constant:12],
        [summaryStack.leadingAnchor constraintEqualToAnchor:summaryCard.leadingAnchor constant:12],
        [summaryStack.trailingAnchor constraintEqualToAnchor:summaryCard.trailingAnchor constant:-12],
        [summaryStack.bottomAnchor constraintEqualToAnchor:summaryCard.bottomAnchor constant:-12],
    ]];

    UIView *quickActionsCard = [self workspaceThemeCardView];
    _quickActionsStack = [UIStackView new];
    _quickActionsStack.translatesAutoresizingMaskIntoConstraints = NO;
    _quickActionsStack.axis = UILayoutConstraintAxisVertical;
    _quickActionsStack.spacing = 6;
    [quickActionsCard addSubview:_quickActionsStack];

    UIStackView *rowOne = [UIStackView new];
    rowOne.axis = UILayoutConstraintAxisHorizontal;
    rowOne.spacing = 6;
    rowOne.distribution = UIStackViewDistributionFillEqually;
    UIButton *shellButton = [self sessionButtonWithTitle:@"Session Shell"
                                                subtitle:@"Open or focus the primary shell"
                                                selector:@selector(openShellShortcut:)];
    UIButton *consoleButton = [self sessionButtonWithTitle:@"System Console"
                                                  subtitle:@"Open or focus the console"
                                                  selector:@selector(openConsoleShortcut:)];
    [rowOne addArrangedSubview:shellButton];
    [rowOne addArrangedSubview:consoleButton];

    UIStackView *rowTwo = [UIStackView new];
    rowTwo.axis = UILayoutConstraintAxisHorizontal;
    rowTwo.spacing = 6;
    rowTwo.distribution = UIStackViewDistributionFillEqually;
    UIButton *dashboardButton = [self sessionButtonWithTitle:@"Layout Manager"
                                                    subtitle:@"Save or restore this workspace"
                                                    selector:@selector(openDashboardShortcut:)];
    [rowTwo addArrangedSubview:dashboardButton];
    if (ISHWorkspaceSupportsSceneWindows()) {
        UIButton *workspaceButton = [self sessionButtonWithTitle:@"New Workspace"
                                                        subtitle:@"Open another workspace window"
                                                        selector:@selector(openWorkspaceShortcut:)];
        [rowTwo addArrangedSubview:workspaceButton];
    } else {
        UIButton *themesButton = [self sessionButtonWithTitle:@"Themes"
                                                     subtitle:@"Adjust colors and density"
                                                     selector:@selector(openThemesShortcut:)];
        [rowTwo addArrangedSubview:themesButton];
    }
    [_quickActionsStack addArrangedSubview:rowOne];
    [_quickActionsStack addArrangedSubview:rowTwo];
    [NSLayoutConstraint activateConstraints:@[
        [_quickActionsStack.topAnchor constraintEqualToAnchor:quickActionsCard.topAnchor constant:8],
        [_quickActionsStack.leadingAnchor constraintEqualToAnchor:quickActionsCard.leadingAnchor constant:8],
        [_quickActionsStack.trailingAnchor constraintEqualToAnchor:quickActionsCard.trailingAnchor constant:-8],
        [_quickActionsStack.bottomAnchor constraintEqualToAnchor:quickActionsCard.bottomAnchor constant:-8],
    ]];

    UIView *sessionsCard = [self workspaceThemeCardView];
    UIStackView *sessionsStack = [UIStackView new];
    sessionsStack.translatesAutoresizingMaskIntoConstraints = NO;
    sessionsStack.axis = UILayoutConstraintAxisVertical;
    sessionsStack.spacing = 6;
    [sessionsCard addSubview:sessionsStack];
    UILabel *sessionsTitle = [self workspaceThemePrimaryLabelWithTextStyle:UIFontTextStyleSubheadline monospaced:NO];
    sessionsTitle.text = @"Live terminals";
    _sessionButtonsStack = [UIStackView new];
    _sessionButtonsStack.axis = UILayoutConstraintAxisVertical;
    _sessionButtonsStack.spacing = 6;
    [sessionsStack addArrangedSubview:sessionsTitle];
    [sessionsStack addArrangedSubview:_sessionButtonsStack];
    [NSLayoutConstraint activateConstraints:@[
        [sessionsStack.topAnchor constraintEqualToAnchor:sessionsCard.topAnchor constant:10],
        [sessionsStack.leadingAnchor constraintEqualToAnchor:sessionsCard.leadingAnchor constant:10],
        [sessionsStack.trailingAnchor constraintEqualToAnchor:sessionsCard.trailingAnchor constant:-10],
        [sessionsStack.bottomAnchor constraintEqualToAnchor:sessionsCard.bottomAnchor constant:-10],
    ]];

    [_contentStack addArrangedSubview:summaryCard];
    [_contentStack addArrangedSubview:quickActionsCard];
    [_contentStack addArrangedSubview:sessionsCard];

    [NSLayoutConstraint activateConstraints:@[
        [_scrollView.topAnchor constraintEqualToAnchor:self.toolContentView.topAnchor],
        [_scrollView.leadingAnchor constraintEqualToAnchor:self.toolContentView.leadingAnchor],
        [_scrollView.trailingAnchor constraintEqualToAnchor:self.toolContentView.trailingAnchor],
        [_scrollView.bottomAnchor constraintEqualToAnchor:self.toolContentView.bottomAnchor],

        [_contentStack.topAnchor constraintEqualToAnchor:_scrollView.contentLayoutGuide.topAnchor constant:6],
        [_contentStack.leadingAnchor constraintEqualToAnchor:_scrollView.frameLayoutGuide.leadingAnchor constant:6],
        [_contentStack.trailingAnchor constraintEqualToAnchor:_scrollView.frameLayoutGuide.trailingAnchor constant:-6],
        [_contentStack.bottomAnchor constraintEqualToAnchor:_scrollView.contentLayoutGuide.bottomAnchor constant:-6],
        [_contentStack.widthAnchor constraintEqualToAnchor:_scrollView.frameLayoutGuide.widthAnchor constant:-12],
    ]];

    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(refreshSessionsNotification:)
                                               name:TerminalRegistryDidChangeNotification
                                             object:nil];
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(refreshSessionsNotification:)
                                               name:TerminalDidLoadNotification
                                             object:nil];
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(refreshSessionsNotification:)
                                               name:TerminalLoadFailedNotification
                                             object:nil];
    [self refreshSessions];
}

- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:self
                                                  name:TerminalRegistryDidChangeNotification
                                                object:nil];
    [NSNotificationCenter.defaultCenter removeObserver:self
                                                  name:TerminalDidLoadNotification
                                                object:nil];
    [NSNotificationCenter.defaultCenter removeObserver:self
                                                  name:TerminalLoadFailedNotification
                                                object:nil];
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [self refreshSessions];
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    _contentStack.spacing = ISHWorkspaceDensityValue(4, 8);
    _quickActionsStack.spacing = ISHWorkspaceDensityValue(4, 6);
    _sessionButtonsStack.spacing = ISHWorkspaceDensityValue(4, 6);
}

- (void)refreshSessionsNotification:(__unused NSNotification *)notification {
    [self refreshSessions];
}

- (void)refreshSessions {
    NSUInteger sceneCount = 0;
    if (@available(iOS 13.0, *)) {
        sceneCount = UIApplication.sharedApplication.connectedScenes.count;
    }
    _summaryLabel.text = [NSString stringWithFormat:@"%lu terminals  •  %lu roots  •  %lu scenes",
                          (unsigned long) Terminal.activeTerminals.count,
                          (unsigned long) Roots.instance.roots.count,
                          (unsigned long) sceneCount];

    NSArray<UIView *> *existingRows = _sessionButtonsStack.arrangedSubviews.copy;
    for (UIView *view in existingRows) {
        [_sessionButtonsStack removeArrangedSubview:view];
        [view removeFromSuperview];
    }
    if (_trackedButtons.count > 4) {
        [_trackedButtons removeObjectsInRange:NSMakeRange(4, _trackedButtons.count - 4)];
    }

    if (Terminal.activeTerminals.count == 0) {
        UILabel *emptyLabel = [self workspaceThemeSecondaryLabelWithTextStyle:UIFontTextStyleFootnote monospaced:NO];
        emptyLabel.text = @"No active terminals. Use the quick actions above to start a shell or console.";
        [_sessionButtonsStack addArrangedSubview:emptyLabel];
        return;
    }

    for (Terminal *terminal in Terminal.activeTerminals) {
        NSString *uuidString = terminal.uuid.UUIDString ?: @"";
        ISHWorkspaceContainedWindowView *existingWindow = [self.workspaceHostViewController desktopWindowHostingTerminalUUID:terminal.uuid];
        NSString *subtitle = existingWindow != nil
            ? @"Focus current workspace window"
            : (terminal.webView.superview != nil ? @"Focus another window" : @"Reconnect in this workspace");
        UIButton *button = [self sessionButtonWithTitle:ISHWorkspaceTerminalDisplayName(terminal)
                                               subtitle:subtitle
                                               selector:@selector(openSessionTerminal:)];
        button.accessibilityIdentifier = uuidString;
        [_sessionButtonsStack addArrangedSubview:button];
    }
    [self workspaceApplyTheme];
}

- (void)openShellShortcut:(id)sender {
    (void) sender;
    [self.workspaceHostViewController openTerminalHerePreferringConsole:NO];
}

- (void)openConsoleShortcut:(id)sender {
    (void) sender;
    [self.workspaceHostViewController openTerminalHerePreferringConsole:YES];
}

- (void)openDashboardShortcut:(id)sender {
    (void) sender;
    [self.workspaceHostViewController openDashboardWindow:nil];
}

- (void)openWorkspaceShortcut:(id)sender {
    (void) sender;
    [self.workspaceHostViewController openNewWorkspaceWindow:nil];
}

- (void)openThemesShortcut:(id)sender {
    (void) sender;
    [self.workspaceHostViewController openOrFocusWorkspaceToolIdentifier:ISHWorkspaceToolThemesIdentifier];
}

- (void)openSessionTerminal:(UIButton *)sender {
    NSString *uuidString = sender.accessibilityIdentifier;
    if (uuidString.length == 0)
        return;
    [self.workspaceHostViewController openExistingTerminalHereWithUUID:[[NSUUID alloc] initWithUUIDString:uuidString]];
}

- (void)workspaceApplyTheme {
    [super workspaceApplyTheme];
    NSDictionary<NSString *, UIColor *> *theme = self.workspaceTheme;
    _summaryLabel.textColor = theme[@"accentAlt"];
    for (UIButton *button in _trackedButtons) {
        button.backgroundColor = [theme[@"cardAlt"] colorWithAlphaComponent:0.94];
        button.layer.borderColor = theme[@"stroke"].CGColor;
        NSMutableAttributedString *title =
            [[NSMutableAttributedString alloc] initWithAttributedString:[button attributedTitleForState:UIControlStateNormal]];
        [title addAttributes:@{
            NSForegroundColorAttributeName: theme[@"primary"],
        } range:NSMakeRange(0, title.length)];
        if (title.length > 0) {
            NSRange newlineRange = [[title string] rangeOfString:@"\n"];
            if (newlineRange.location != NSNotFound) {
                NSUInteger subtitleLocation = newlineRange.location + newlineRange.length;
                if (subtitleLocation < title.length) {
                    [title addAttributes:@{
                        NSForegroundColorAttributeName: theme[@"secondary"],
                    } range:NSMakeRange(subtitleLocation, title.length - subtitleLocation)];
                }
            }
        }
        [button setAttributedTitle:title forState:UIControlStateNormal];
    }
}

@end

@implementation WorkspaceStorageToolViewController {
    UIScrollView *_scrollView;
    UIStackView *_contentStack;
    UILabel *_summaryLabel;
    UITextView *_detailsTextView;
    UIButton *_refreshButton;
    NSUInteger _refreshGeneration;
}

- (UIButton *)storageActionButton {
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    button.translatesAutoresizingMaskIntoConstraints = NO;
    button.layer.cornerRadius = 10;
    button.layer.borderWidth = 1;
    button.contentEdgeInsets = UIEdgeInsetsMake(5, 10, 5, 10);
    button.titleLabel.font = [UIFont systemFontOfSize:ISHWorkspaceThemeFontSize(UIFontTextStyleCaption1)
                                               weight:UIFontWeightSemibold];
    [button setTitle:@"Refresh" forState:UIControlStateNormal];
    [button addTarget:self action:@selector(refreshStorage:) forControlEvents:UIControlEventTouchUpInside];
    return button;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Storage";

    _scrollView = [UIScrollView new];
    _scrollView.translatesAutoresizingMaskIntoConstraints = NO;
    _scrollView.alwaysBounceVertical = YES;
    [self.toolContentView addSubview:_scrollView];

    _contentStack = [UIStackView new];
    _contentStack.translatesAutoresizingMaskIntoConstraints = NO;
    _contentStack.axis = UILayoutConstraintAxisVertical;
    _contentStack.spacing = 8;
    [_scrollView addSubview:_contentStack];

    UIView *summaryCard = [self workspaceThemeCardView];
    UIStackView *summaryStack = [UIStackView new];
    summaryStack.translatesAutoresizingMaskIntoConstraints = NO;
    summaryStack.axis = UILayoutConstraintAxisVertical;
    summaryStack.spacing = 6;
    [summaryCard addSubview:summaryStack];
    UILabel *summaryTitle = [self workspaceThemeSecondaryLabelWithTextStyle:UIFontTextStyleCaption1 monospaced:NO];
    summaryTitle.text = @"ROOT STORAGE";
    summaryTitle.font = [UIFont systemFontOfSize:9 weight:UIFontWeightSemibold];
    _summaryLabel = [self workspaceThemeAccentLabelWithTextStyle:UIFontTextStyleHeadline monospaced:NO];
    _summaryLabel.numberOfLines = 0;
    [summaryStack addArrangedSubview:summaryTitle];
    [summaryStack addArrangedSubview:_summaryLabel];
    [NSLayoutConstraint activateConstraints:@[
        [summaryStack.topAnchor constraintEqualToAnchor:summaryCard.topAnchor constant:12],
        [summaryStack.leadingAnchor constraintEqualToAnchor:summaryCard.leadingAnchor constant:12],
        [summaryStack.trailingAnchor constraintEqualToAnchor:summaryCard.trailingAnchor constant:-12],
        [summaryStack.bottomAnchor constraintEqualToAnchor:summaryCard.bottomAnchor constant:-12],
    ]];

    UIView *actionsCard = [self workspaceThemeCardView];
    UIStackView *actionsStack = [UIStackView new];
    actionsStack.translatesAutoresizingMaskIntoConstraints = NO;
    actionsStack.axis = UILayoutConstraintAxisHorizontal;
    actionsStack.alignment = UIStackViewAlignmentCenter;
    actionsStack.spacing = 8;
    [actionsCard addSubview:actionsStack];
    UILabel *actionsLabel = [self workspaceThemeSecondaryLabelWithTextStyle:UIFontTextStyleFootnote monospaced:NO];
    actionsLabel.text = @"Rescan roots and container directories.";
    _refreshButton = [self storageActionButton];
    [actionsStack addArrangedSubview:actionsLabel];
    [actionsStack addArrangedSubview:_refreshButton];
    [actionsLabel setContentCompressionResistancePriority:UILayoutPriorityDefaultLow forAxis:UILayoutConstraintAxisHorizontal];
    [NSLayoutConstraint activateConstraints:@[
        [actionsStack.topAnchor constraintEqualToAnchor:actionsCard.topAnchor constant:8],
        [actionsStack.leadingAnchor constraintEqualToAnchor:actionsCard.leadingAnchor constant:10],
        [actionsStack.trailingAnchor constraintEqualToAnchor:actionsCard.trailingAnchor constant:-10],
        [actionsStack.bottomAnchor constraintEqualToAnchor:actionsCard.bottomAnchor constant:-8],
    ]];

    UIView *detailsCard = [self workspaceThemeCardView];
    _detailsTextView = [self workspaceThemeTextView];
    [detailsCard addSubview:_detailsTextView];
    [NSLayoutConstraint activateConstraints:@[
        [detailsCard.heightAnchor constraintGreaterThanOrEqualToConstant:(ISHWorkspaceUsesPhoneLayout() ? 132.0 : 168.0)],
        [_detailsTextView.topAnchor constraintEqualToAnchor:detailsCard.topAnchor constant:8],
        [_detailsTextView.leadingAnchor constraintEqualToAnchor:detailsCard.leadingAnchor constant:8],
        [_detailsTextView.trailingAnchor constraintEqualToAnchor:detailsCard.trailingAnchor constant:-8],
        [_detailsTextView.bottomAnchor constraintEqualToAnchor:detailsCard.bottomAnchor constant:-8],
    ]];

    [_contentStack addArrangedSubview:summaryCard];
    [_contentStack addArrangedSubview:actionsCard];
    [_contentStack addArrangedSubview:detailsCard];

    [NSLayoutConstraint activateConstraints:@[
        [_scrollView.topAnchor constraintEqualToAnchor:self.toolContentView.topAnchor],
        [_scrollView.leadingAnchor constraintEqualToAnchor:self.toolContentView.leadingAnchor],
        [_scrollView.trailingAnchor constraintEqualToAnchor:self.toolContentView.trailingAnchor],
        [_scrollView.bottomAnchor constraintEqualToAnchor:self.toolContentView.bottomAnchor],

        [_contentStack.topAnchor constraintEqualToAnchor:_scrollView.contentLayoutGuide.topAnchor constant:6],
        [_contentStack.leadingAnchor constraintEqualToAnchor:_scrollView.frameLayoutGuide.leadingAnchor constant:6],
        [_contentStack.trailingAnchor constraintEqualToAnchor:_scrollView.frameLayoutGuide.trailingAnchor constant:-6],
        [_contentStack.bottomAnchor constraintEqualToAnchor:_scrollView.contentLayoutGuide.bottomAnchor constant:-6],
        [_contentStack.widthAnchor constraintEqualToAnchor:_scrollView.frameLayoutGuide.widthAnchor constant:-12],
    ]];

    [self refreshStorage:nil];
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [self refreshStorage:nil];
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    _contentStack.spacing = ISHWorkspaceDensityValue(4, 8);
}

- (void)refreshStorage:(id)sender {
    (void) sender;
    NSUInteger generation = ++_refreshGeneration;
    _detailsTextView.text = @"Scanning roots and container directories…";
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        NSArray<NSDictionary<NSString *, id> *> *rootRecords = ISHWorkspaceRootUsageRecords();
        NSDictionary<NSString *, NSNumber *> *tmpUsage = ISHWorkspaceDirectoryUsage([NSURL fileURLWithPath:NSTemporaryDirectory()]);
        NSArray<NSURL *> *cacheDirectories =
            [NSFileManager.defaultManager URLsForDirectory:NSCachesDirectory inDomains:NSUserDomainMask];
        NSDictionary<NSString *, NSNumber *> *cacheUsage =
            ISHWorkspaceDirectoryUsage(cacheDirectories.firstObject ?: [NSURL fileURLWithPath:NSHomeDirectory()]);
        uint64_t totalRootBytes = 0;
        NSMutableArray<NSString *> *lines = [NSMutableArray array];
        [lines addObject:@"Workspace roots:"];
        for (NSDictionary<NSString *, id> *record in rootRecords) {
            totalRootBytes += [record[@"bytes"] unsignedLongLongValue];
            NSString *marker = [record[@"isDefault"] boolValue] ? @"default" : @"root";
            [lines addObject:[NSString stringWithFormat:@"%@  •  %@  •  %@",
                              record[@"name"],
                              record[@"abi"],
                              marker]];
            [lines addObject:[NSString stringWithFormat:@"  %@  •  %@ files  •  %@ dirs",
                              ISHWorkspaceByteCountString([record[@"bytes"] unsignedLongLongValue]),
                              record[@"files"],
                              record[@"directories"]]];
        }
        if (rootRecords.count == 0) {
            [lines addObject:@"No installed roots."];
        }
        [lines addObject:@""];
        [lines addObject:@"Container directories:"];
        [lines addObject:[NSString stringWithFormat:@"tmp  •  %@", ISHWorkspaceByteCountString([tmpUsage[@"bytes"] unsignedLongLongValue])]];
        [lines addObject:[NSString stringWithFormat:@"cache  •  %@", ISHWorkspaceByteCountString([cacheUsage[@"bytes"] unsignedLongLongValue])]];

        NSDictionary<NSFileAttributeKey, id> *attributes =
            [NSFileManager.defaultManager attributesOfFileSystemForPath:NSHomeDirectory() error:nil];
        uint64_t freeBytes = [attributes[NSFileSystemFreeSize] unsignedLongLongValue];
        NSString *summary = [NSString stringWithFormat:@"%@ free  •  %@ across %lu roots",
                                                       ISHWorkspaceByteCountString(freeBytes),
                                                       ISHWorkspaceByteCountString(totalRootBytes),
                                                       (unsigned long) rootRecords.count];
        dispatch_async(dispatch_get_main_queue(), ^{
            if (generation != self->_refreshGeneration)
                return;
            self->_summaryLabel.text = summary;
            self->_detailsTextView.text = [lines componentsJoinedByString:@"\n"];
        });
    });
}

- (void)workspaceApplyTheme {
    [super workspaceApplyTheme];
    NSDictionary<NSString *, UIColor *> *theme = self.workspaceTheme;
    _summaryLabel.textColor = theme[@"accent"];
    _refreshButton.backgroundColor = [theme[@"cardAlt"] colorWithAlphaComponent:0.96];
    _refreshButton.layer.borderColor = theme[@"stroke"].CGColor;
    [_refreshButton setTitleColor:theme[@"accentAlt"] forState:UIControlStateNormal];
}

@end

@implementation WorkspaceLauncherToolViewController {
    // Outer scroll + vertical stack, matching every other applet in this file (see e.g. the
    // Monitor/Sessions/Storage tool view controllers): proven to position header/list/footer
    // controls correctly, unlike UITableView's tableHeaderView/tableFooterView self-sizing,
    // which placed the footer controls at the top instead of the bottom. The table view itself
    // is just one fixed-height arranged subview in that stack, sized to its own content — the
    // outer scroll view is what actually scrolls if the window can't show everything.
    UIScrollView *_scrollView;
    UIStackView *_contentStack;
    UITableView *_tableView;
    NSLayoutConstraint *_tableViewHeightConstraint;
    // Indices from the root shortcut list down to the group currently being displayed; empty
    // at the top level. Lets the applet drill into a group ("Remote Login") and show its
    // nested shortcuts in place, with a "‹ Back" row to return.
    NSMutableArray<NSNumber *> *_currentPath;
    // Whether the list is in native table-view editing mode (toggled by the pill button at the
    // bottom, not a separate sheet — see launcherEditToggleButton). Reordering and delete both
    // use UIKit's own table-view editing machinery rather than custom gesture code: three
    // attempts at a hand-rolled drag (UIStackView + transform, then + a floating snapshot, then
    // + gesture-recognizer tuning) all had reliability problems that were hard to diagnose
    // without being able to drive the touch interaction directly. UITableView's reordering is
    // Apple-maintained and used everywhere in iOS, so it sidesteps that whole bug class.
    BOOL _isEditing;
    // The current level's shortcuts, kept as the table's live data source; committed to
    // NSUserDefaults on every edit (see -deleteRowAtIndex:indexPath: and -moveRowAtIndexPath:).
    NSMutableArray<NSDictionary<NSString *, id> *> *_rowShortcuts;
    // Set right before a self-inflicted persist (delete/move) so the resulting shortcuts-changed
    // notification doesn't immediately undo the table's own animation with a redundant reload —
    // other open Launcher windows still react to the notification normally.
    BOOL _suppressNextReload;
}

static NSString *const ISHWorkspaceLauncherRowReuseIdentifier = @"launcher.row";

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Launcher";
    _currentPath = [NSMutableArray new];

    _scrollView = [UIScrollView new];
    _scrollView.translatesAutoresizingMaskIntoConstraints = NO;
    [self.toolContentView addSubview:_scrollView];

    _contentStack = [UIStackView new];
    _contentStack.translatesAutoresizingMaskIntoConstraints = NO;
    _contentStack.axis = UILayoutConstraintAxisVertical;
    _contentStack.spacing = 8;
    [_scrollView addSubview:_contentStack];

    CGFloat inset = ISHWorkspaceUsesPhoneLayout() ? 12.0 : 16.0;
    [NSLayoutConstraint activateConstraints:@[
        [_scrollView.topAnchor constraintEqualToAnchor:self.toolContentView.topAnchor],
        [_scrollView.leadingAnchor constraintEqualToAnchor:self.toolContentView.leadingAnchor],
        [_scrollView.trailingAnchor constraintEqualToAnchor:self.toolContentView.trailingAnchor],
        [_scrollView.bottomAnchor constraintEqualToAnchor:self.toolContentView.bottomAnchor],
        [_contentStack.topAnchor constraintEqualToAnchor:_scrollView.contentLayoutGuide.topAnchor constant:inset],
        [_contentStack.leadingAnchor constraintEqualToAnchor:_scrollView.frameLayoutGuide.leadingAnchor constant:inset],
        [_contentStack.trailingAnchor constraintEqualToAnchor:_scrollView.frameLayoutGuide.trailingAnchor constant:-inset],
        [_contentStack.bottomAnchor constraintEqualToAnchor:_scrollView.contentLayoutGuide.bottomAnchor constant:-inset],
        [_contentStack.widthAnchor constraintEqualToAnchor:_scrollView.frameLayoutGuide.widthAnchor constant:-(inset * 2.0)],
    ]];

    _tableView = [[UITableView alloc] initWithFrame:CGRectZero style:UITableViewStylePlain];
    _tableView.translatesAutoresizingMaskIntoConstraints = NO;
    _tableView.dataSource = self;
    _tableView.delegate = self;
    _tableView.dragDelegate = self;
    _tableView.dropDelegate = self;
    // Modern drag & drop, not the classic canMoveRowAtIndexPath/moveRowAtIndexPath reorder: the
    // classic API can only reorder within one flat array, but dropping a row onto a group (to
    // nest it inside) or onto the "‹ Back" row (to move it out to the parent level) both need to
    // relocate an item between two different levels of the tree, which drag & drop's
    // per-drop-target delegate callbacks support and the classic API has no concept of.
    _tableView.dragInteractionEnabled = YES;
    _tableView.allowsSelectionDuringEditing = YES;
    _tableView.backgroundColor = UIColor.clearColor;
    _tableView.separatorInset = UIEdgeInsetsZero;
    // The outer _scrollView is the one that actually scrolls (matching every other applet in
    // this file); the table is sized to fit its own content exactly (see -updateTableViewHeight),
    // so it never needs to scroll on its own — this also avoids a repeat of the earlier bug
    // where an ancestor scroll view's pan gesture competed with a drag inside this list.
    _tableView.scrollEnabled = NO;
    _tableView.rowHeight = ISHWorkspaceUsesPhoneLayout() ? 34.0 : 38.0;
    _tableView.separatorColor = [self launcherColorForKey:@"stroke" fallback:[UIColor colorWithWhite:0.5 alpha:0.35]];
    [_tableView registerClass:UITableViewCell.class forCellReuseIdentifier:ISHWorkspaceLauncherRowReuseIdentifier];
    _tableViewHeightConstraint = [_tableView.heightAnchor constraintEqualToConstant:_tableView.rowHeight];
    _tableViewHeightConstraint.active = YES;

    [self rebuildLauncherList];
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(launcherShortcutsDidChange)
                                               name:ISHWorkspaceLauncherShortcutsDidChangeNotification
                                             object:nil];
    // Re-assert the Launcher's placement when the app returns to the foreground. The Launcher is
    // otherwise the one window the activation transition can leave displaced; the Desktops applet
    // already self-corrects the same way via its own foreground observers.
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(reassertLauncherPlacement)
                                               name:UIApplicationDidBecomeActiveNotification
                                             object:nil];
    if (@available(iOS 13.0, *)) {
        [NSNotificationCenter.defaultCenter addObserver:self
                                               selector:@selector(reassertLauncherPlacement)
                                                   name:UISceneDidActivateNotification
                                                 object:nil];
    }
}

- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:self];
}

- (void)launcherShortcutsDidChange {
    if (_suppressNextReload) {
        _suppressNextReload = NO;
        return;
    }
    // A path can be invalidated by an edit made elsewhere (e.g. the group we're inside of was
    // just deleted from the root-menu Launcher popup) — clamp back to the nearest valid level.
    [self clampCurrentPathToValidLevel];
    [self rebuildLauncherList];
}

- (void)clampCurrentPathToValidLevel {
    while (_currentPath.count > 0) {
        NSArray<NSNumber *> *parentPath = [_currentPath subarrayWithRange:NSMakeRange(0, _currentPath.count - 1)];
        NSArray<NSDictionary<NSString *, id> *> *parentLevel = ISHWorkspaceLauncherArrayAtPath(parentPath);
        NSUInteger index = _currentPath.lastObject.unsignedIntegerValue;
        if (index < parentLevel.count && ISHWorkspaceShortcutIsGroup(parentLevel[index]))
            return;
        [_currentPath removeLastObject];
    }
}

- (void)reassertLauncherPlacement {
    // Deferred so it runs after the foreground layout pass settles, then restores the Launcher to
    // its persisted frame (like the dock) so it returns to exactly where the user left it.
    __weak typeof(self) weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        [(id)weakSelf.workspaceHostViewController restoreLauncherWindowPlacement];
    });
}

- (UIColor *)launcherColorForKey:(NSString *)key fallback:(UIColor *)fallback {
    UIColor *color = self.workspaceTheme[key];
    return color != nil ? color : fallback;
}

- (void)rebuildLauncherList {
    for (UIView *view in [_contentStack.arrangedSubviews copy]) {
        [_contentStack removeArrangedSubview:view];
        [view removeFromSuperview];
    }

    if (_currentPath.count > 0)
        [_contentStack addArrangedSubview:[self launcherBackButton]];

    NSArray<NSDictionary<NSString *, id> *> *shortcuts = ISHWorkspaceLauncherArrayAtPath(_currentPath);
    _rowShortcuts = [shortcuts mutableCopy];

    if (shortcuts.count == 0) {
        UILabel *empty = [self workspaceThemeSecondaryLabelWithTextStyle:UIFontTextStyleFootnote monospaced:NO];
        empty.numberOfLines = 0;
        empty.text = _currentPath.count > 0 ? @"No shortcuts in this group yet." : @"No shortcuts yet.";
        [_contentStack addArrangedSubview:empty];
    } else {
        [_contentStack addArrangedSubview:_tableView];
    }
    [self updateTableViewHeight];

    [_tableView setEditing:_isEditing animated:NO];
    [_tableView reloadData];

    [_contentStack addArrangedSubview:[self launcherBottomControlsRow]];

    // Grow/shrink the window to match the currently displayed content.
    [(id)self.workspaceHostViewController autosizeLauncherWindowForItemCount:shortcuts.count showsBackRow:_currentPath.count > 0];
}

// The table has no natural intrinsic size, so its height in the stack is driven explicitly —
// exactly rowHeight * count, since it never scrolls internally (see -viewDidLoad).
- (void)updateTableViewHeight {
    _tableViewHeightConstraint.constant = _tableView.rowHeight * MAX(_rowShortcuts.count, (NSUInteger)1);
}

#pragma mark - UITableViewDataSource / UITableViewDelegate

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    return (NSInteger)_rowShortcuts.count;
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:ISHWorkspaceLauncherRowReuseIdentifier forIndexPath:indexPath];
    NSUInteger index = (NSUInteger)indexPath.row;
    NSDictionary<NSString *, id> *shortcut = index < _rowShortcuts.count ? _rowShortcuts[index] : nil;
    BOOL isGroup = ISHWorkspaceShortcutIsGroup(shortcut);
    NSString *command = shortcut[@"command"];
    NSString *shortcutName = shortcut[@"name"];
    NSString *name = shortcutName.length > 0 ? shortcutName : (command.length > 0 ? command : @"Terminal");

    cell.backgroundColor = UIColor.clearColor;
    cell.textLabel.text = name;
    cell.textLabel.font = [UIFont systemFontOfSize:ISHWorkspaceThemeFontSize(UIFontTextStyleSubheadline) weight:UIFontWeightMedium];
    cell.textLabel.textColor = [self launcherColorForKey:@"primary" fallback:UIColor.darkTextColor];
    cell.accessoryType = (isGroup && !_isEditing) ? UITableViewCellAccessoryDisclosureIndicator : UITableViewCellAccessoryNone;
    cell.selectionStyle = UITableViewCellSelectionStyleDefault;
    // A folder icon marks groups regardless of editing state — the trailing disclosure chevron
    // above is a group-only cue too, but it's replaced by the reorder handle while editing
    // (exactly when you're most likely dragging something and need to see which rows are valid
    // "drop into" targets), so groups need a cue that survives into edit mode.
    cell.imageView.image = isGroup
        ? [[UIImage systemImageNamed:@"folder.fill"] imageByApplyingSymbolConfiguration:[UIImageSymbolConfiguration configurationWithPointSize:15 weight:UIImageSymbolWeightMedium]]
        : nil;
    cell.imageView.tintColor = [self launcherColorForKey:@"accent" fallback:UIColor.systemBlueColor];
    // A visible "grab here to move" cue: drag-and-drop (unlike the classic reorder control) has
    // no dedicated handle of its own — any part of the row is draggable — so without this, edit
    // mode gives no hint that rows can be reordered at all.
    cell.editingAccessoryView = _isEditing ? [self launcherReorderHintView] : nil;
    return cell;
}

- (UIView *)launcherReorderHintView {
    UIImageView *icon = [[UIImageView alloc] initWithImage:[[UIImage systemImageNamed:@"line.3.horizontal"]
        imageByApplyingSymbolConfiguration:[UIImageSymbolConfiguration configurationWithPointSize:15 weight:UIImageSymbolWeightSemibold]]];
    icon.tintColor = [self launcherColorForKey:@"secondary" fallback:UIColor.secondaryLabelColor];
    icon.contentMode = UIViewContentModeCenter;
    icon.frame = CGRectMake(0, 0, 28, 28);
    return icon;
}

// A short tap always drills into a group — in edit mode too, since that's the only way to reach
// (and drag things into/out of) what's inside it. Renaming/deleting a group now lives behind a
// long-press context menu instead (see -tableView:contextMenuConfigurationForRowAtIndexPath:).
// A leaf row keeps the old split: short tap edits while editing, runs otherwise.
- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    NSUInteger index = (NSUInteger)indexPath.row;
    if (index >= _rowShortcuts.count)
        return;
    NSDictionary<NSString *, id> *shortcut = _rowShortcuts[index];
    if (ISHWorkspaceShortcutIsGroup(shortcut)) {
        // Drilling in leaves editing state exactly as it was — forcing it on turned out to be
        // more surprising than helpful: it's not obvious you're now in edit mode.
        [_currentPath addObject:@(index)];
        [self rebuildLauncherList];
        return;
    }
    if (_isEditing) {
        [(id)self.workspaceHostViewController presentEditLauncherShortcutAtPath:_currentPath index:index];
        return;
    }
    [(id)self.workspaceHostViewController runLauncherShortcutWithCommand:shortcut[@"command"] title:shortcut[@"name"]];
}

// Long-press (while editing) reveals the rename/delete dialog via a native context menu — this
// coexists cleanly with the table's own drag interaction on the same rows (UIKit disambiguates
// hold-still-for-a-menu vs. hold-then-move-to-drag internally), unlike a second custom gesture
// recognizer would, which is exactly the class of bug the reorder feature went through earlier.
- (UIContextMenuConfiguration *)tableView:(UITableView *)tableView contextMenuConfigurationForRowAtIndexPath:(NSIndexPath *)indexPath point:(CGPoint)point {
    if (!_isEditing)
        return nil;
    NSUInteger index = (NSUInteger)indexPath.row;
    if (index >= _rowShortcuts.count)
        return nil;
    BOOL isGroup = ISHWorkspaceShortcutIsGroup(_rowShortcuts[index]);
    NSArray<NSNumber *> *path = [_currentPath copy];
    __weak typeof(self) weakSelf = self;
    return [UIContextMenuConfiguration configurationWithIdentifier:nil
                                                    previewProvider:nil
                                                     actionProvider:^UIMenu *(NSArray<UIMenuElement *> *suggestedActions) {
        UIAction *editAction = [UIAction actionWithTitle:isGroup ? @"Edit Group…" : @"Edit Shortcut…"
                                                    image:[UIImage systemImageNamed:@"pencil"]
                                               identifier:nil
                                                  handler:^(__unused UIAction *action) {
            [(id)weakSelf.workspaceHostViewController presentEditLauncherShortcutAtPath:path index:index];
        }];
        if (!isGroup)
            return [UIMenu menuWithTitle:@"" children:@[editAction]];
        // A group can be added to without drilling into it first — the destination path is the
        // group's own path (path + its index), not the level it's sitting at.
        NSArray<NSNumber *> *groupPath = [path arrayByAddingObject:@(index)];
        UIAction *addAction = [UIAction actionWithTitle:@"Add…"
                                                   image:[UIImage systemImageNamed:@"plus"]
                                              identifier:nil
                                                 handler:^(__unused UIAction *action) {
            [(id)weakSelf.workspaceHostViewController presentAddLauncherOptionsFromView:weakSelf.view path:groupPath];
        }];
        return [UIMenu menuWithTitle:@"" children:@[addAction, editAction]];
    }];
}

- (BOOL)tableView:(UITableView *)tableView canEditRowAtIndexPath:(NSIndexPath *)indexPath {
    return YES;
}

- (UITableViewCellEditingStyle)tableView:(UITableView *)tableView editingStyleForRowAtIndexPath:(NSIndexPath *)indexPath {
    return UITableViewCellEditingStyleDelete;
}

- (void)tableView:(UITableView *)tableView commitEditingStyle:(UITableViewCellEditingStyle)editingStyle forRowAtIndexPath:(NSIndexPath *)indexPath {
    if (editingStyle != UITableViewCellEditingStyleDelete)
        return;
    NSUInteger index = (NSUInteger)indexPath.row;
    if (index >= _rowShortcuts.count)
        return;
    NSDictionary<NSString *, id> *shortcut = _rowShortcuts[index];
    NSArray<NSDictionary<NSString *, id> *> *children = ISHWorkspaceShortcutChildren(shortcut);
    if (children.count == 0) {
        [self deleteRowAtIndex:index indexPath:indexPath];
        return;
    }
    UIAlertController *confirm =
        [UIAlertController alertControllerWithTitle:@"Delete Group?"
                                            message:[NSString stringWithFormat:@"This deletes \"%@\" and %lu item%@ inside it.",
                                                     shortcut[@"name"], (unsigned long)children.count, children.count == 1 ? @"" : @"s"]
                                     preferredStyle:UIAlertControllerStyleAlert];
    [confirm addAction:[UIAlertAction actionWithTitle:@"Delete"
                                                style:UIAlertActionStyleDestructive
                                              handler:^(__unused UIAlertAction *action) {
        [self deleteRowAtIndex:index indexPath:indexPath];
    }]];
    [confirm addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [self presentViewController:confirm animated:YES completion:nil];
}

- (void)deleteRowAtIndex:(NSUInteger)index indexPath:(NSIndexPath *)indexPath {
    if (index >= _rowShortcuts.count)
        return;
    [_rowShortcuts removeObjectAtIndex:index];

    _suppressNextReload = YES;
    NSArray<NSNumber *> *path = [_currentPath copy];
    ISHWorkspaceLauncherMutateAtPath(path, ^(NSMutableArray<NSDictionary<NSString *, id> *> *level) {
        if (index < level.count)
            [level removeObjectAtIndex:index];
    });

    if (_rowShortcuts.count == 0) {
        // Nothing left to show in the table — swap it for the empty-state label via a full
        // rebuild rather than animating a delete down to zero rows.
        [self rebuildLauncherList];
        return;
    }
    [_tableView deleteRowsAtIndexPaths:@[indexPath] withRowAnimation:UITableViewRowAnimationFade];
    [self updateTableViewHeight];
    [UIView animateWithDuration:0.25 animations:^{
        [self.view layoutIfNeeded];
    }];
    [(id)self.workspaceHostViewController autosizeLauncherWindowForItemCount:_rowShortcuts.count showsBackRow:_currentPath.count > 0];
}

#pragma mark - UITableViewDragDelegate / UITableViewDropDelegate

// Drag is a local, in-process reorder/move — the item provider's content never leaves the app
// (or even gets read back out via pasteboard), so an empty placeholder is enough; the real
// payload is `localObject`, resolved synchronously against _rowShortcuts at drop time.
- (NSArray<UIDragItem *> *)tableView:(UITableView *)tableView itemsForBeginningDragSession:(id<UIDragSession>)session atIndexPath:(NSIndexPath *)indexPath {
    if (!_isEditing)
        return @[];
    NSUInteger index = (NSUInteger)indexPath.row;
    if (index >= _rowShortcuts.count)
        return @[];
    UIDragItem *item = [[UIDragItem alloc] initWithItemProvider:[NSItemProvider new]];
    item.localObject = @(index);
    return @[item];
}

- (BOOL)tableView:(UITableView *)tableView canHandleDropSession:(id<UIDropSession>)session {
    return _isEditing && session.localDragSession != nil;
}

// Dropping exactly onto a group row nests the dragged item inside it; dropping between rows
// (including between two groups) is a plain reorder — matching how Files treats dropping onto
// vs. between folder icons.
- (UITableViewDropProposal *)tableView:(UITableView *)tableView dropSessionDidUpdate:(id<UIDropSession>)session
                   withDestinationIndexPath:(NSIndexPath *)destinationIndexPath {
    if (destinationIndexPath == nil || (NSUInteger)destinationIndexPath.row >= _rowShortcuts.count)
        return [[UITableViewDropProposal alloc] initWithDropOperation:UIDropOperationMove intent:UITableViewDropIntentInsertAtDestinationIndexPath];
    BOOL destinationIsGroup = ISHWorkspaceShortcutIsGroup(_rowShortcuts[(NSUInteger)destinationIndexPath.row]);
    UITableViewDropIntent intent = destinationIsGroup ? UITableViewDropIntentInsertIntoDestinationIndexPath
                                                       : UITableViewDropIntentInsertAtDestinationIndexPath;
    return [[UITableViewDropProposal alloc] initWithDropOperation:UIDropOperationMove intent:intent];
}

- (void)tableView:(UITableView *)tableView performDropWithCoordinator:(id<UITableViewDropCoordinator>)coordinator {
    id<UITableViewDropItem> item = coordinator.items.firstObject;
    NSIndexPath *sourceIndexPath = item.sourceIndexPath;
    NSNumber *sourceIndexNumber = [item.dragItem.localObject isKindOfClass:NSNumber.class] ? item.dragItem.localObject : nil;
    if (sourceIndexPath == nil || sourceIndexNumber == nil)
        return;
    NSUInteger from = sourceIndexNumber.unsignedIntegerValue;
    if (from >= _rowShortcuts.count)
        return;

    NSIndexPath *destinationIndexPath = coordinator.destinationIndexPath;
    NSUInteger to = destinationIndexPath != nil ? (NSUInteger)destinationIndexPath.row : _rowShortcuts.count - 1;
    BOOL destinationIsGroup = destinationIndexPath != nil && to != from && to < _rowShortcuts.count
        && ISHWorkspaceShortcutIsGroup(_rowShortcuts[to]);

    if (destinationIsGroup) {
        [self moveShortcutAtIndex:from intoGroupAtIndex:to];
    } else if (to != from) {
        [self reorderShortcutFromIndex:from toIndex:to];
        [tableView moveRowAtIndexPath:sourceIndexPath toIndexPath:destinationIndexPath];
    }
    [coordinator dropItem:item.dragItem toRowAtIndexPath:destinationIndexPath ?: sourceIndexPath];
}

- (void)reorderShortcutFromIndex:(NSUInteger)from toIndex:(NSUInteger)to {
    NSDictionary<NSString *, id> *moved = _rowShortcuts[from];
    [_rowShortcuts removeObjectAtIndex:from];
    [_rowShortcuts insertObject:moved atIndex:to];

    _suppressNextReload = YES;
    NSArray<NSNumber *> *path = [_currentPath copy];
    NSArray<NSDictionary<NSString *, id> *> *newOrder = [_rowShortcuts copy];
    ISHWorkspaceLauncherMutateAtPath(path, ^(NSMutableArray<NSDictionary<NSString *, id> *> *level) {
        [level removeAllObjects];
        [level addObjectsFromArray:newOrder];
    });
}

// Nests the shortcut at `sourceIndex` into the group at `groupIndex`, both at the current level.
// Removed from the visible table immediately (with a full local-array-driven rebuild, since the
// destination group's own row count isn't visible from here — it's a sibling, not expanded).
- (void)moveShortcutAtIndex:(NSUInteger)sourceIndex intoGroupAtIndex:(NSUInteger)groupIndex {
    NSArray<NSNumber *> *path = [_currentPath copy];
    NSArray<NSNumber *> *groupPath = [path arrayByAddingObject:@(groupIndex)];
    _suppressNextReload = YES;
    ISHWorkspaceLauncherMoveShortcut(path, sourceIndex, groupPath, NSUIntegerMax);
    [_rowShortcuts removeObjectAtIndex:sourceIndex];
    [self rebuildLauncherList];
}

#pragma mark - Header / footer controls

- (UIButton *)launcherBackButton {
    UIButton *back = [UIButton buttonWithType:UIButtonTypeSystem];
    back.translatesAutoresizingMaskIntoConstraints = NO;
    UIColor *primary = [self launcherColorForKey:@"primary" fallback:UIColor.darkTextColor];
    [back setTitle:@"‹ Back" forState:UIControlStateNormal];
    [back setTitleColor:primary forState:UIControlStateNormal];
    back.titleLabel.font = [UIFont systemFontOfSize:ISHWorkspaceThemeFontSize(UIFontTextStyleSubheadline) weight:UIFontWeightSemibold];
    back.contentHorizontalAlignment = UIControlContentHorizontalAlignmentCenter;
    CGFloat minHeight = ISHWorkspaceUsesPhoneLayout() ? 28.0 : 30.0;
    [back.heightAnchor constraintGreaterThanOrEqualToConstant:minHeight].active = YES;
    [back addTarget:self action:@selector(backButtonTapped:) forControlEvents:UIControlEventTouchUpInside];
    if (_isEditing) {
        // While editing, the Back row doubles as a drop target: dragging an item onto it moves
        // that item out to the parent level. This is the only way to "un-nest" something, since
        // a group's parent level isn't itself visible while drilled into the group — the row
        // list only ever shows one level of the tree at a time.
        back.layer.borderWidth = 1;
        back.layer.cornerRadius = 12;
        back.layer.borderColor = [self launcherColorForKey:@"stroke" fallback:[UIColor colorWithWhite:0.5 alpha:0.35]].CGColor;
        [back addInteraction:[[UIDropInteraction alloc] initWithDelegate:self]];
    }
    return back;
}

#pragma mark - UIDropInteractionDelegate (the "‹ Back" row, as a drop-to-un-nest target)

- (BOOL)dropInteraction:(UIDropInteraction *)interaction canHandleSession:(id<UIDropSession>)session {
    return _isEditing && _currentPath.count > 0 && session.localDragSession != nil;
}

- (UIDropProposal *)dropInteraction:(UIDropInteraction *)interaction sessionDidUpdate:(id<UIDropSession>)session {
    return [[UIDropProposal alloc] initWithDropOperation:UIDropOperationMove];
}

- (void)dropInteraction:(UIDropInteraction *)interaction sessionDidEnter:(id<UIDropSession>)session {
    interaction.view.backgroundColor = [[self launcherColorForKey:@"accent" fallback:UIColor.systemBlueColor] colorWithAlphaComponent:0.2];
}

- (void)dropInteraction:(UIDropInteraction *)interaction sessionDidExit:(id<UIDropSession>)session {
    interaction.view.backgroundColor = UIColor.clearColor;
}

- (void)dropInteraction:(UIDropInteraction *)interaction performDrop:(id<UIDropSession>)session {
    interaction.view.backgroundColor = UIColor.clearColor;
    if (_currentPath.count == 0)
        return;
    UIDragItem *dragItem = session.items.firstObject;
    NSNumber *sourceIndexNumber = [dragItem.localObject isKindOfClass:NSNumber.class] ? dragItem.localObject : nil;
    if (sourceIndexNumber == nil)
        return;
    NSUInteger sourceIndex = sourceIndexNumber.unsignedIntegerValue;
    if (sourceIndex >= _rowShortcuts.count)
        return;

    NSArray<NSNumber *> *sourcePath = [_currentPath copy];
    NSArray<NSNumber *> *parentPath = [_currentPath subarrayWithRange:NSMakeRange(0, _currentPath.count - 1)];
    NSUInteger groupIndexInParent = _currentPath.lastObject.unsignedIntegerValue;

    _suppressNextReload = YES;
    // Insert right after the group we're currently inside of, at the parent level — keeps the
    // moved item spatially close to where it came from rather than appending far away.
    ISHWorkspaceLauncherMoveShortcut(sourcePath, sourceIndex, parentPath, groupIndexInParent + 1);
    [_rowShortcuts removeObjectAtIndex:sourceIndex];
    [self rebuildLauncherList];
}

// Footer row: an Add ("+") button alongside the Edit/Done toggle, both unbordered/muted (vs.
// the accessory-styled shortcut rows above) so they read as controls, not launchable items.
- (UIView *)launcherBottomControlsRow {
    UIStackView *row = [UIStackView new];
    row.translatesAutoresizingMaskIntoConstraints = NO;
    row.axis = UILayoutConstraintAxisHorizontal;
    row.spacing = 8;
    row.layoutMarginsRelativeArrangement = YES;
    row.layoutMargins = UIEdgeInsetsMake(4, 8, 4, 8);
    row.distribution = UIStackViewDistributionFillEqually;
    [row addArrangedSubview:[self launcherAddButton]];
    [row addArrangedSubview:[self launcherEditToggleButton]];
    return row;
}

// Opens the same Add Shortcut/Group/Built-in sheet the popup-menu "Edit Shortcuts…" flow uses,
// scoped to whatever level (root or a drilled-into group) is currently displayed.
- (UIButton *)launcherAddButton {
    UIButton *add = [UIButton buttonWithType:UIButtonTypeSystem];
    add.translatesAutoresizingMaskIntoConstraints = NO;
    UIColor *secondary = [self launcherColorForKey:@"secondary" fallback:UIColor.secondaryLabelColor];
    [add setTitle:@"+ Add" forState:UIControlStateNormal];
    [add setTitleColor:secondary forState:UIControlStateNormal];
    add.titleLabel.font = [UIFont systemFontOfSize:ISHWorkspaceThemeFontSize(UIFontTextStyleFootnote) weight:UIFontWeightMedium];
    add.backgroundColor = [[self launcherColorForKey:@"cardAlt" fallback:[UIColor colorWithWhite:0.5 alpha:0.12]] colorWithAlphaComponent:0.25];
    add.layer.cornerRadius = 10;
    CGFloat height = ISHWorkspaceUsesPhoneLayout() ? 26.0 : 28.0;
    [add.heightAnchor constraintEqualToConstant:height].active = YES;
    [add addTarget:self action:@selector(addButtonTapped:) forControlEvents:UIControlEventTouchUpInside];
    return add;
}

- (void)addButtonTapped:(UIButton *)sender {
    [(id)self.workspaceHostViewController presentAddLauncherOptionsFromView:sender path:_currentPath];
}

// The Edit/Done pill: deliberately unbordered and muted (vs. the shortcut rows above it) so it
// reads as a mode toggle, not another launchable item. This is the sole entry point into
// editing — no separate "Edit Shortcuts" sheet in the applet anymore.
- (UIButton *)launcherEditToggleButton {
    UIButton *toggle = [UIButton buttonWithType:UIButtonTypeSystem];
    toggle.translatesAutoresizingMaskIntoConstraints = NO;
    UIColor *secondary = [self launcherColorForKey:@"secondary" fallback:UIColor.secondaryLabelColor];
    [toggle setTitle:_isEditing ? @"Done" : @"Edit" forState:UIControlStateNormal];
    [toggle setTitleColor:secondary forState:UIControlStateNormal];
    toggle.titleLabel.font = [UIFont systemFontOfSize:ISHWorkspaceThemeFontSize(UIFontTextStyleFootnote) weight:UIFontWeightMedium];
    toggle.backgroundColor = [[self launcherColorForKey:@"cardAlt" fallback:[UIColor colorWithWhite:0.5 alpha:0.12]] colorWithAlphaComponent:0.25];
    toggle.layer.cornerRadius = 10;
    CGFloat height = ISHWorkspaceUsesPhoneLayout() ? 26.0 : 28.0;
    [toggle.heightAnchor constraintEqualToConstant:height].active = YES;
    [toggle addTarget:self action:@selector(editToggleTapped:) forControlEvents:UIControlEventTouchUpInside];
    return toggle;
}

- (void)editToggleTapped:(UIButton *)sender {
    _isEditing = !_isEditing;
    [self rebuildLauncherList];
}

- (void)backButtonTapped:(UIButton *)sender {
    if (_currentPath.count == 0)
        return;
    [_currentPath removeLastObject];
    [self rebuildLauncherList];
}

@end

#pragma mark - MotePad text editor applet

// A minimal guest-filesystem browser used for Open and Save As. It walks the guest
// VFS through MotePadDocumentStore (directories first, then files) and either returns
// a chosen file (Open) or a chosen directory + typed filename (Save). Presented inside
// a UINavigationController; descending into a directory pushes a fresh instance.
typedef NS_ENUM(NSInteger, MotePadBrowserMode) {
    MotePadBrowserModeOpen,
    MotePadBrowserModeSave,
};

@interface MotePadFileBrowserViewController : UITableViewController
- (instancetype)initWithMode:(MotePadBrowserMode)mode
                   directory:(NSString *)directory
               suggestedName:(NSString *)suggestedName
                  completion:(void (^)(NSString *selectedGuestPath))completion;
@end

@implementation MotePadFileBrowserViewController {
    MotePadBrowserMode _mode;
    NSString *_directory;
    NSString *_suggestedName;
    void (^_completion)(NSString *);
    NSArray<MotePadDirectoryEntry *> *_entries;
    BOOL _showsParent;
    BOOL _loading;
    NSInteger _loadGeneration;  // discards stale async listings
}

- (instancetype)initWithMode:(MotePadBrowserMode)mode
                   directory:(NSString *)directory
               suggestedName:(NSString *)suggestedName
                  completion:(void (^)(NSString *))completion {
    self = [super initWithStyle:UITableViewStylePlain];
    if (self) {
        _mode = mode;
        _directory = directory.length ? directory : @"/";
        _suggestedName = suggestedName;
        _completion = [completion copy];
    }
    return self;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = _directory.lastPathComponent.length ? _directory.lastPathComponent : @"/";
    self.navigationItem.prompt = _directory;
    self.navigationItem.leftBarButtonItem =
        [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemCancel
                                                      target:self action:@selector(cancel)];
    if (_mode == MotePadBrowserModeSave) {
        self.navigationItem.rightBarButtonItem =
            [[UIBarButtonItem alloc] initWithTitle:@"Save Here" style:UIBarButtonItemStyleDone
                                            target:self action:@selector(saveHere)];
    }
    [self reload];
}

// Guest-VFS listings block on emulator locks, so they run on the document store's
// serial I/O queue; while one is in flight the table shows a spinner and rejects
// taps so a half-loaded directory can't be interacted with.
- (void)reload {
    _showsParent = ![_directory isEqualToString:@"/"];
    NSInteger generation = ++_loadGeneration;
    [self setLoading:YES];
    __weak typeof(self) weakSelf = self;
    [[MotePadDocumentStore sharedStore] listDirectoryAtGuestPath:_directory
                                                      completion:^(NSArray<MotePadDirectoryEntry *> *entries) {
        typeof(self) strongSelf = weakSelf;
        if (strongSelf == nil || strongSelf->_loadGeneration != generation)
            return;
        strongSelf->_entries = entries ?: @[];
        [strongSelf setLoading:NO];
        [strongSelf.tableView reloadData];
    }];
}

- (void)setLoading:(BOOL)loading {
    _loading = loading;
    if (loading) {
        UIActivityIndicatorView *spinner =
            [[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleMedium];
        [spinner startAnimating];
        self.tableView.backgroundView = spinner;
    } else {
        self.tableView.backgroundView = nil;
    }
    // Cancel (left bar button) stays live so the user can always bail out.
    self.tableView.userInteractionEnabled = !loading;
    self.navigationItem.rightBarButtonItem.enabled = !loading;
}

// Dismiss first, then run the completion from the dismissal completion block: the
// completion may present an alert (Couldn't Open / Couldn't Save), and presenting
// while this browser is still being dismissed silently fails ("already presenting"),
// swallowing the error — a failed Save As would look like it succeeded.
- (void)cancel {
    [self finishWithPath:nil];
}

- (void)finishWithPath:(NSString *)path {
    void (^completion)(NSString *) = _completion;
    _completion = nil;
    [self dismissViewControllerAnimated:YES completion:^{
        if (completion) completion(path);
    }];
}

- (NSString *)parentDirectory {
    NSString *parent = _directory.stringByDeletingLastPathComponent;
    return parent.length ? parent : @"/";
}

- (void)pushDirectory:(NSString *)directory {
    MotePadFileBrowserViewController *child =
        [[MotePadFileBrowserViewController alloc] initWithMode:_mode directory:directory
                                                 suggestedName:_suggestedName completion:_completion];
    [self.navigationController pushViewController:child animated:YES];
}

- (void)saveHere {
    [self promptForFilename:_suggestedName];
}

- (void)promptForFilename:(NSString *)initialName {
    [self promptForFilename:initialName message:nil];
}

- (void)promptForFilename:(NSString *)initialName message:(NSString *)message {
    UIAlertController *alert =
        [UIAlertController alertControllerWithTitle:@"Save As"
                                            message:message.length ? message : _directory
                                     preferredStyle:UIAlertControllerStyleAlert];
    [alert addTextFieldWithConfigurationHandler:^(UITextField *textField) {
        textField.placeholder = @"filename.txt";
        textField.text = initialName;
        textField.autocapitalizationType = UITextAutocapitalizationTypeNone;
        textField.autocorrectionType = UITextAutocorrectionTypeNo;
        textField.clearButtonMode = UITextFieldViewModeWhileEditing;
    }];
    __weak typeof(self) weakSelf = self;
    [alert addAction:[UIAlertAction actionWithTitle:@"Save" style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction *action) {
        typeof(self) strongSelf = weakSelf;
        if (strongSelf == nil) return;
        NSString *name = [alert.textFields.firstObject.text
                          stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        if (name.length == 0 || [name containsString:@"/"] ||
            [name isEqualToString:@"."] || [name isEqualToString:@".."]) {
            // Bad name: don't silently swallow the save — re-present the prompt with an
            // inline explanation once the alert's own dismissal has finished.
            dispatch_async(dispatch_get_main_queue(), ^{
                [strongSelf promptForFilename:name
                                      message:@"Enter a file name without “/” (and not “.” or “..”)."];
            });
            return;
        }
        // Let the alert finish dismissing before finishWithPath dismisses the browser;
        // overlapping dismissals at two levels of the hierarchy can cancel each other.
        NSString *path = [strongSelf->_directory stringByAppendingPathComponent:name];
        dispatch_async(dispatch_get_main_queue(), ^{
            [strongSelf finishWithPath:path];
        });
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    return (NSInteger)_entries.count + (_showsParent ? 1 : 0);
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:@"motepad.browser"];
    if (cell == nil)
        cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:@"motepad.browser"];

    if (_showsParent && indexPath.row == 0) {
        cell.textLabel.text = @"..";
        cell.imageView.image = [UIImage systemImageNamed:@"arrow.turn.left.up"];
        cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
        cell.textLabel.textColor = UIColor.labelColor;
        return cell;
    }

    MotePadDirectoryEntry *entry = _entries[(NSUInteger)indexPath.row - (_showsParent ? 1 : 0)];
    cell.textLabel.text = entry.name;
    if (entry.type == MotePadEntryTypeDirectory) {
        cell.imageView.image = [UIImage systemImageNamed:@"folder"];
        cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
        cell.textLabel.textColor = UIColor.labelColor;
    } else {
        cell.imageView.image = [UIImage systemImageNamed:@"doc.text"];
        cell.accessoryType = UITableViewCellAccessoryNone;
        BOOL selectable = (entry.type == MotePadEntryTypeFile);
        cell.textLabel.textColor = selectable ? UIColor.labelColor : UIColor.secondaryLabelColor;
    }
    return cell;
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];

    if (_showsParent && indexPath.row == 0) {
        [self pushDirectory:[self parentDirectory]];
        return;
    }

    MotePadDirectoryEntry *entry = _entries[(NSUInteger)indexPath.row - (_showsParent ? 1 : 0)];
    if (entry.type == MotePadEntryTypeDirectory) {
        [self pushDirectory:entry.guestPath];
        return;
    }
    if (entry.type != MotePadEntryTypeFile)
        return;  // sockets/fifos/etc. aren't openable as text

    if (_mode == MotePadBrowserModeOpen) {
        [self finishWithPath:entry.guestPath];
    } else {
        // Save mode: tapping an existing file pre-fills its name for an overwrite.
        [self promptForFilename:entry.name];
    }
}

@end


// Left-margin line-number gutter for the MotePad editor. iOS has no NSRulerView, so this
// is a plain view driven by the editor's TextKit layout manager: it draws one number per
// logical line (paragraph), aligned to that line's first laid-out fragment, and the editor
// asks it to redraw on scroll/edit/wrap/font changes. Word-wrapped continuation fragments
// get no number, matching a desktop editor.
@interface MotePadLineNumberGutterView : UIView
@property (nonatomic, weak) UITextView *textView;
@property (nonatomic, strong) UIColor *numberColor;
@property (nonatomic, strong) UIColor *separatorColor;
@end

@implementation MotePadLineNumberGutterView

- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        self.contentMode = UIViewContentModeRedraw;  // redraw when bounds change
        self.userInteractionEnabled = NO;
    }
    return self;
}

- (UIFont *)gutterFont {
    UIFont *base = self.textView.font ?: [UIFont monospacedSystemFontOfSize:12 weight:UIFontWeightRegular];
    return [UIFont monospacedDigitSystemFontOfSize:MAX(9.0, base.pointSize - 1.0) weight:UIFontWeightRegular];
}

- (NSUInteger)newlineCountIn:(NSString *)text upTo:(NSUInteger)index {
    NSUInteger count = 0;
    NSUInteger limit = MIN(index, text.length);
    for (NSUInteger i = 0; i < limit; i++)
        if ([text characterAtIndex:i] == '\n') count++;
    return count;
}

- (void)drawNumber:(NSUInteger)number atY:(CGFloat)y font:(UIFont *)font {
    NSDictionary *attrs = @{ NSFontAttributeName: font,
                            NSForegroundColorAttributeName: self.numberColor ?: UIColor.secondaryLabelColor };
    NSString *s = [NSString stringWithFormat:@"%lu", (unsigned long)number];
    CGSize size = [s sizeWithAttributes:attrs];
    [s drawAtPoint:CGPointMake(self.bounds.size.width - size.width - 6.0, y) withAttributes:attrs];
}

- (void)drawRect:(__unused CGRect)rect {
    UITextView *tv = self.textView;
    if (tv == nil) return;

    // Right-edge hairline separator.
    CGFloat hair = 1.0 / MAX(1.0, UIScreen.mainScreen.scale);
    [(self.separatorColor ?: [UIColor colorWithWhite:0.5 alpha:0.35]) setFill];
    UIRectFill(CGRectMake(self.bounds.size.width - hair, 0, hair, self.bounds.size.height));

    UIFont *font = [self gutterFont];
    CGFloat insetTop = tv.textContainerInset.top;
    CGFloat offsetY = tv.contentOffset.y;
    NSString *text = tv.text;
    if (text.length == 0) {  // empty document still shows "1"
        [self drawNumber:1 atY:insetTop - offsetY font:font];
        return;
    }

    NSLayoutManager *lm = tv.layoutManager;
    NSTextContainer *tc = tv.textContainer;
    CGRect visibleContainerRect = CGRectMake(0, offsetY - insetTop, tc.size.width, tv.bounds.size.height);
    NSRange visibleGlyphRange = [lm glyphRangeForBoundingRect:visibleContainerRect inTextContainer:tc];
    if (visibleGlyphRange.length == 0) return;

    NSUInteger firstChar = [lm characterIndexForGlyphAtIndex:visibleGlyphRange.location];
    __block NSUInteger paragraph = 1 + [self newlineCountIn:text upTo:firstChar];
    __block BOOL first = YES;
    [lm enumerateLineFragmentsForGlyphRange:visibleGlyphRange
                                 usingBlock:^(CGRect fragRect, __unused CGRect usedRect,
                                              __unused NSTextContainer *container,
                                              NSRange lineGlyphRange, __unused BOOL *stop) {
        NSUInteger charIndex = [lm characterIndexForGlyphAtIndex:lineGlyphRange.location];
        BOOL startsParagraph = (charIndex == 0) || ([text characterAtIndex:charIndex - 1] == '\n');
        if (startsParagraph) {
            if (!first) paragraph++;  // fragments are in document order; advance past the last paragraph
            [self drawNumber:paragraph atY:CGRectGetMinY(fragRect) + insetTop - offsetY font:font];
        }
        first = NO;
    }];
}

@end


#pragma mark - MotePad custom compact menu

// A native pull-down UIMenu has a ~250pt minimum width on iPad, which leaves short
// menus (File, Help, …) mostly empty. These lightweight classes render a themed popover
// menu sized to exactly fit its widest row instead.
@interface MotePadMenuRow : NSObject
@property (nonatomic, copy) NSString *title;
@property (nonatomic, copy, nullable) NSString *shortcut;
@property (nonatomic) BOOL checkable;
@property (nonatomic) BOOL checked;
@property (nonatomic, copy) void (^action)(void);
@end

@implementation MotePadMenuRow
@end

@interface MotePadMenuRowControl : UIControl
@property (nonatomic, strong) UIColor *highlightColor;
@end

@implementation MotePadMenuRowControl
- (void)setHighlighted:(BOOL)highlighted {
    [super setHighlighted:highlighted];
    self.backgroundColor = highlighted ? self.highlightColor : UIColor.clearColor;
}
@end

@interface MotePadPopupMenuController : UIViewController <UIPopoverPresentationControllerDelegate>
- (instancetype)initWithSections:(NSArray<NSArray<MotePadMenuRow *> *> *)sections
                           theme:(NSDictionary<NSString *, UIColor *> *)theme;
@end

@implementation MotePadPopupMenuController {
    NSArray<NSArray<MotePadMenuRow *> *> *_sections;
    NSDictionary<NSString *, UIColor *> *_theme;
    NSMutableArray<void (^)(void)> *_actions;
}

- (instancetype)initWithSections:(NSArray<NSArray<MotePadMenuRow *> *> *)sections
                           theme:(NSDictionary<NSString *, UIColor *> *)theme {
    self = [super init];
    if (self) {
        _sections = sections;
        _theme = theme;
        _actions = [NSMutableArray array];
    }
    return self;
}

- (UIColor *)color:(NSString *)key fallback:(UIColor *)fallback {
    return _theme[key] ?: fallback;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    UIColor *card = [self color:@"card" fallback:UIColor.secondarySystemBackgroundColor];
    UIColor *primary = [self color:@"primary" fallback:UIColor.labelColor];
    UIColor *secondary = [self color:@"secondary" fallback:UIColor.secondaryLabelColor];
    UIColor *accent = [self color:@"accent" fallback:UIColor.systemBlueColor];
    UIColor *stroke = [self color:@"stroke" fallback:[UIColor colorWithWhite:0.5 alpha:0.35]];
    UIColor *highlight = [accent colorWithAlphaComponent:0.22];
    self.view.backgroundColor = card;

    UIFont *titleFont = [UIFont systemFontOfSize:16];
    UIFont *shortcutFont = [UIFont systemFontOfSize:15];

    // Measure the columns so shortcuts right-align and the popover is exactly as wide
    // as its content needs.
    CGFloat maxTitle = 0, maxShortcut = 0;
    BOOL anyCheckable = NO;
    for (NSArray<MotePadMenuRow *> *section in _sections) {
        for (MotePadMenuRow *row in section) {
            maxTitle = MAX(maxTitle, [row.title sizeWithAttributes:@{NSFontAttributeName: titleFont}].width);
            if (row.shortcut.length)
                maxShortcut = MAX(maxShortcut, [row.shortcut sizeWithAttributes:@{NSFontAttributeName: shortcutFont}].width);
            if (row.checkable) anyCheckable = YES;
        }
    }
    CGFloat inset = 14, gap = (maxShortcut > 0) ? 28 : 0;
    CGFloat checkWidth = anyCheckable ? 18 : 0, checkGap = anyCheckable ? 8 : 0;
    CGFloat rowHeight = 34, sepHeight = 9, pad = 6;
    CGFloat contentWidth = inset + ceil(maxTitle) + gap + ceil(maxShortcut) + checkGap + checkWidth + inset;
    contentWidth = MAX(150, MIN(contentWidth, 320));

    UIStackView *stack = [UIStackView new];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 0;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:stack];
    // Center the rows vertically (with a minimum pad each side). The popover often makes
    // its content view a bit taller than preferredContentSize (arrow allowance); centering
    // splits that surplus evenly so the top and bottom padding always match, instead of it
    // all pooling at the bottom.
    [NSLayoutConstraint activateConstraints:@[
        [stack.centerYAnchor constraintEqualToAnchor:self.view.centerYAnchor],
        [stack.topAnchor constraintGreaterThanOrEqualToAnchor:self.view.topAnchor constant:pad],
        [stack.bottomAnchor constraintLessThanOrEqualToAnchor:self.view.bottomAnchor constant:-pad],
        [stack.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
        [stack.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
    ]];

    NSUInteger totalRows = 0;
    for (NSUInteger s = 0; s < _sections.count; s++) {
        if (s > 0) {
            UIView *sep = [UIView new];
            sep.translatesAutoresizingMaskIntoConstraints = NO;
            [sep.heightAnchor constraintEqualToConstant:sepHeight].active = YES;
            UIView *hair = [UIView new];
            hair.translatesAutoresizingMaskIntoConstraints = NO;
            hair.backgroundColor = stroke;
            [sep addSubview:hair];
            [NSLayoutConstraint activateConstraints:@[
                [hair.leadingAnchor constraintEqualToAnchor:sep.leadingAnchor constant:inset],
                [hair.trailingAnchor constraintEqualToAnchor:sep.trailingAnchor constant:-inset],
                [hair.centerYAnchor constraintEqualToAnchor:sep.centerYAnchor],
                [hair.heightAnchor constraintEqualToConstant:1.0 / MAX(1.0, UIScreen.mainScreen.scale)],
            ]];
            [stack addArrangedSubview:sep];
        }
        for (MotePadMenuRow *row in _sections[s]) {
            MotePadMenuRowControl *rowView = [MotePadMenuRowControl new];
            rowView.translatesAutoresizingMaskIntoConstraints = NO;
            rowView.highlightColor = highlight;
            rowView.tag = (NSInteger)_actions.count;
            [rowView.heightAnchor constraintEqualToConstant:rowHeight].active = YES;
            [rowView addTarget:self action:@selector(rowTapped:) forControlEvents:UIControlEventTouchUpInside];
            [_actions addObject:(row.action ?: ^{})];

            UILabel *titleLabel = [UILabel new];
            titleLabel.translatesAutoresizingMaskIntoConstraints = NO;
            titleLabel.text = row.title;
            titleLabel.font = titleFont;
            titleLabel.textColor = primary;
            [rowView addSubview:titleLabel];
            [NSLayoutConstraint activateConstraints:@[
                [titleLabel.leadingAnchor constraintEqualToAnchor:rowView.leadingAnchor constant:inset],
                [titleLabel.centerYAnchor constraintEqualToAnchor:rowView.centerYAnchor],
            ]];

            UIImageView *check = nil;
            if (anyCheckable) {
                check = [UIImageView new];
                check.translatesAutoresizingMaskIntoConstraints = NO;
                check.contentMode = UIViewContentModeScaleAspectFit;
                check.tintColor = accent;
                if (@available(iOS 13.0, *))
                    check.image = [UIImage systemImageNamed:@"checkmark"];
                check.hidden = !(row.checkable && row.checked);
                [rowView addSubview:check];
                [NSLayoutConstraint activateConstraints:@[
                    [check.trailingAnchor constraintEqualToAnchor:rowView.trailingAnchor constant:-inset],
                    [check.centerYAnchor constraintEqualToAnchor:rowView.centerYAnchor],
                    [check.widthAnchor constraintEqualToConstant:checkWidth],
                    [check.heightAnchor constraintEqualToConstant:checkWidth],
                ]];
            }

            if (row.shortcut.length) {
                UILabel *sc = [UILabel new];
                sc.translatesAutoresizingMaskIntoConstraints = NO;
                sc.text = row.shortcut;
                sc.font = shortcutFont;
                sc.textColor = secondary;
                sc.textAlignment = NSTextAlignmentRight;
                [rowView addSubview:sc];
                NSLayoutConstraint *scTrailing = check
                    ? [sc.trailingAnchor constraintEqualToAnchor:check.leadingAnchor constant:-checkGap]
                    : [sc.trailingAnchor constraintEqualToAnchor:rowView.trailingAnchor constant:-inset];
                [NSLayoutConstraint activateConstraints:@[
                    scTrailing,
                    [sc.centerYAnchor constraintEqualToAnchor:rowView.centerYAnchor],
                    [sc.leadingAnchor constraintGreaterThanOrEqualToAnchor:titleLabel.trailingAnchor constant:12],
                ]];
            }

            [stack addArrangedSubview:rowView];
            totalRows++;
        }
    }

    CGFloat totalHeight = totalRows * rowHeight
        + (_sections.count > 1 ? (_sections.count - 1) * sepHeight : 0) + pad * 2;
    self.preferredContentSize = CGSizeMake(contentWidth, totalHeight);
}

- (void)rowTapped:(UIControl *)sender {
    void (^action)(void) = ((NSUInteger)sender.tag < _actions.count) ? _actions[(NSUInteger)sender.tag] : nil;
    // Run the action after the menu finishes dismissing, so actions that present their
    // own UI (Open, Save As, alerts) don't collide with the in-flight dismissal.
    [self dismissViewControllerAnimated:YES completion:^{ if (action) action(); }];
}

- (UIModalPresentationStyle)adaptivePresentationStyleForPresentationController:(__unused UIPresentationController *)controller
                                                              traitCollection:(__unused UITraitCollection *)traitCollection {
    return UIModalPresentationNone;  // stay a popover even at compact width (iPhone)
}

@end


@interface WorkspaceMotePadToolViewController () <UITextViewDelegate, UIFontPickerViewControllerDelegate, WorkspaceFileOpenable, WorkspaceFocusable>
@end

@implementation WorkspaceMotePadToolViewController {
    UIView *_menuBar;
    UIView *_menuBarSeparator;
    NSMutableArray<UIButton *> *_menuButtons;
    UILabel *_docTitleLabel;
    UITextView *_textView;
    UIView *_statusBar;
    UILabel *_statusLabel;
    NSLayoutConstraint *_statusBarHeight;
    MotePadLineNumberGutterView *_gutter;
    NSLayoutConstraint *_gutterWidth;
    CGFloat _gutterContentWidth;

    NSString *_currentGuestPath;  // nil == an untitled document
    BOOL _dirty;
    BOOL _wordWrap;
    BOOL _statusBarVisible;
    BOOL _lineNumbersVisible;
    NSString *_lastSearch;  // last Find term, so Find Next (⌘G) can repeat it

    NSInteger _busyCount;      // >0 while a queued file operation is in flight (editor locked)
    NSString *_draftSlotName;  // this window's autosave draft file name (claimed in viewDidLoad)
    BOOL _grabbedInitialFocus; // becomeFirstResponder on first viewDidAppear only
    NSTimer *_draftTimer;      // debounce timer for keystroke-driven autosave
}

// Give the editor the keyboard as soon as the applet opens: without this a
// fresh MotePad window showed no text cursor and ignored typing until the
// user tapped inside it.
- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    if (!_grabbedInitialFocus) {
        _grabbedInitialFocus = YES;
        [_textView becomeFirstResponder];
    }
}

// WorkspaceFocusable: called whenever this window becomes frontmost again
// after the first time -- a tap, Ctrl+Tab window cycling, or Cmd+Arrow
// Desktop switching. Without this, cycling back to a MotePad window left no
// first responder at all: no cursor, no typing, until the user tapped
// inside it again (viewDidAppear's initial grab only fires once).
- (void)workspaceToolDidBecomeFrontmost {
    [_textView becomeFirstResponder];
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"MotePad";
    _wordWrap = YES;
    _statusBarVisible = YES;
    _lineNumbersVisible = NO;
    _menuButtons = [NSMutableArray array];

    UIView *content = self.toolContentView;

    // --- Menu bar (native UIMenu dropdowns) --------------------------------------
    _menuBar = [UIView new];
    _menuBar.translatesAutoresizingMaskIntoConstraints = NO;
    [content addSubview:_menuBar];

    UIStackView *menuStack = [UIStackView new];
    menuStack.translatesAutoresizingMaskIntoConstraints = NO;
    menuStack.axis = UILayoutConstraintAxisHorizontal;
    menuStack.spacing = ISHWorkspaceUsesPhoneLayout() ? 2 : 6;
    menuStack.alignment = UIStackViewAlignmentCenter;
    [_menuBar addSubview:menuStack];

    for (NSString *name in @[@"File", @"Edit", @"Format", @"View", @"Help"]) {
        UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
        button.translatesAutoresizingMaskIntoConstraints = NO;
        [button setTitle:name forState:UIControlStateNormal];
        button.titleLabel.font = [UIFont systemFontOfSize:ISHWorkspaceUsesPhoneLayout() ? 13 : 14 weight:UIFontWeightMedium];
        button.contentEdgeInsets = UIEdgeInsetsMake(4, ISHWorkspaceUsesPhoneLayout() ? 6 : 8, 4, ISHWorkspaceUsesPhoneLayout() ? 6 : 8);
        [button addTarget:self action:@selector(menuButtonTapped:) forControlEvents:UIControlEventTouchUpInside];
        [_menuButtons addObject:button];
        [menuStack addArrangedSubview:button];
    }

    _docTitleLabel = [UILabel new];
    _docTitleLabel.translatesAutoresizingMaskIntoConstraints = NO;
    _docTitleLabel.font = [UIFont systemFontOfSize:ISHWorkspaceUsesPhoneLayout() ? 11 : 12 weight:UIFontWeightSemibold];
    _docTitleLabel.textAlignment = NSTextAlignmentRight;
    _docTitleLabel.adjustsFontSizeToFitWidth = YES;
    _docTitleLabel.minimumScaleFactor = 0.7;
    [_docTitleLabel setContentHuggingPriority:UILayoutPriorityDefaultLow forAxis:UILayoutConstraintAxisHorizontal];
    [_docTitleLabel setContentCompressionResistancePriority:UILayoutPriorityDefaultLow forAxis:UILayoutConstraintAxisHorizontal];
    [_menuBar addSubview:_docTitleLabel];

    _menuBarSeparator = [UIView new];
    _menuBarSeparator.translatesAutoresizingMaskIntoConstraints = NO;
    [_menuBar addSubview:_menuBarSeparator];

    // --- Editor ------------------------------------------------------------------
    _textView = [UITextView new];
    _textView.translatesAutoresizingMaskIntoConstraints = NO;
    _textView.delegate = self;
    _textView.editable = YES;
    _textView.font = [UIFont fontWithName:@"Courier" size:14] ?: [UIFont monospacedSystemFontOfSize:14 weight:UIFontWeightRegular];
    _textView.autocapitalizationType = UITextAutocapitalizationTypeNone;
    _textView.autocorrectionType = UITextAutocorrectionTypeNo;
    _textView.smartQuotesType = UITextSmartQuotesTypeNo;
    _textView.smartDashesType = UITextSmartDashesTypeNo;
    _textView.spellCheckingType = UITextSpellCheckingTypeNo;
    _textView.keyboardType = UIKeyboardTypeASCIICapable;
    _textView.alwaysBounceVertical = YES;
    _textView.textContainerInset = UIEdgeInsetsMake(8, 6, 8, 6);
    [content addSubview:_textView];

    // --- Line-number gutter (hidden until toggled in the View menu) --------------
    _gutter = [MotePadLineNumberGutterView new];
    _gutter.translatesAutoresizingMaskIntoConstraints = NO;
    _gutter.textView = _textView;
    [content addSubview:_gutter];

    // --- Status bar --------------------------------------------------------------
    _statusBar = [UIView new];
    _statusBar.translatesAutoresizingMaskIntoConstraints = NO;
    [content addSubview:_statusBar];

    _statusLabel = [UILabel new];
    _statusLabel.translatesAutoresizingMaskIntoConstraints = NO;
    _statusLabel.font = [UIFont monospacedDigitSystemFontOfSize:11 weight:UIFontWeightRegular];
    [_statusBar addSubview:_statusLabel];

    CGFloat barHeight = ISHWorkspaceUsesPhoneLayout() ? 32 : 34;
    _statusBarHeight = [_statusBar.heightAnchor constraintEqualToConstant:22];
    _gutterWidth = [_gutter.widthAnchor constraintEqualToConstant:0];
    [NSLayoutConstraint activateConstraints:@[
        [_menuBar.topAnchor constraintEqualToAnchor:content.topAnchor],
        [_menuBar.leadingAnchor constraintEqualToAnchor:content.leadingAnchor],
        [_menuBar.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [_menuBar.heightAnchor constraintEqualToConstant:barHeight],

        [menuStack.leadingAnchor constraintEqualToAnchor:_menuBar.leadingAnchor constant:4],
        [menuStack.centerYAnchor constraintEqualToAnchor:_menuBar.centerYAnchor],

        [_docTitleLabel.leadingAnchor constraintGreaterThanOrEqualToAnchor:menuStack.trailingAnchor constant:6],
        [_docTitleLabel.trailingAnchor constraintEqualToAnchor:_menuBar.trailingAnchor constant:-8],
        [_docTitleLabel.centerYAnchor constraintEqualToAnchor:_menuBar.centerYAnchor],

        [_menuBarSeparator.leadingAnchor constraintEqualToAnchor:_menuBar.leadingAnchor],
        [_menuBarSeparator.trailingAnchor constraintEqualToAnchor:_menuBar.trailingAnchor],
        [_menuBarSeparator.bottomAnchor constraintEqualToAnchor:_menuBar.bottomAnchor],
        [_menuBarSeparator.heightAnchor constraintEqualToConstant:1.0 / MAX(1.0, UIScreen.mainScreen.scale)],

        [_gutter.topAnchor constraintEqualToAnchor:_menuBar.bottomAnchor],
        [_gutter.leadingAnchor constraintEqualToAnchor:content.leadingAnchor],
        [_gutter.bottomAnchor constraintEqualToAnchor:_statusBar.topAnchor],
        _gutterWidth,

        [_textView.topAnchor constraintEqualToAnchor:_menuBar.bottomAnchor],
        [_textView.leadingAnchor constraintEqualToAnchor:_gutter.trailingAnchor],
        [_textView.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [_textView.bottomAnchor constraintEqualToAnchor:_statusBar.topAnchor],

        [_statusBar.leadingAnchor constraintEqualToAnchor:content.leadingAnchor],
        [_statusBar.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [_statusBar.bottomAnchor constraintEqualToAnchor:content.bottomAnchor],
        _statusBarHeight,

        [_statusLabel.leadingAnchor constraintEqualToAnchor:_statusBar.leadingAnchor constant:10],
        [_statusLabel.trailingAnchor constraintEqualToAnchor:_statusBar.trailingAnchor constant:-10],
        [_statusLabel.centerYAnchor constraintEqualToAnchor:_statusBar.centerYAnchor],
    ]];

    [self applyWordWrap];
    [self updateDocTitle];
    [self updateStatusBar];
    [self applyStatusBarVisibility];
    [self updateGutterWidth];
    [self applyLineNumbersVisibility];
    [self workspaceApplyTheme];

    // --- Draft autosave (survives a jetsam kill) ----------------------------------
    [self claimDraftSlot];
    [self restoreDraftIfNewer];
    // Flush the draft the moment the app heads to the background: a background kill
    // is routine for this app, and the debounce timer won't fire once suspended.
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(motePadFlushDraftForBackground:)
                                               name:UIApplicationDidEnterBackgroundNotification
                                             object:nil];
    if (@available(iOS 13.0, *)) {
        [NSNotificationCenter.defaultCenter addObserver:self
                                               selector:@selector(motePadFlushDraftForBackground:)
                                                   name:UISceneDidEnterBackgroundNotification
                                                 object:nil];
    }
}

- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:self];
    [_draftTimer invalidate];
    [WorkspaceMotePadToolViewController releaseDraftSlotNamed:_draftSlotName];
}

#pragma mark Theming

- (void)workspaceApplyTheme {
    [super workspaceApplyTheme];
    if (_textView == nil)  // base runs this once before our subviews exist
        return;
    NSDictionary<NSString *, UIColor *> *theme = self.workspaceTheme;
    UIColor *primary = theme[@"primary"] ?: UIColor.labelColor;
    UIColor *secondary = theme[@"secondary"] ?: UIColor.secondaryLabelColor;
    UIColor *accent = theme[@"accent"] ?: UIColor.systemBlueColor;
    UIColor *card = theme[@"card"] ?: UIColor.secondarySystemBackgroundColor;
    UIColor *stroke = theme[@"stroke"] ?: [UIColor colorWithWhite:0.5 alpha:0.35];

    _menuBar.backgroundColor = [card colorWithAlphaComponent:0.96];
    _menuBarSeparator.backgroundColor = stroke;
    for (UIButton *button in _menuButtons)
        [button setTitleColor:primary forState:UIControlStateNormal];
    _docTitleLabel.textColor = secondary;

    _textView.backgroundColor = [card colorWithAlphaComponent:0.6];
    _textView.textColor = primary;
    _textView.tintColor = accent;
    _statusBar.backgroundColor = [card colorWithAlphaComponent:0.96];
    _statusLabel.textColor = secondary;

    _gutter.backgroundColor = [card colorWithAlphaComponent:0.6];
    _gutter.numberColor = secondary;
    _gutter.separatorColor = stroke;
    [_gutter setNeedsDisplay];
}

#pragma mark Menus

- (MotePadMenuRow *)row:(NSString *)title shortcut:(NSString *)shortcut handler:(void (^)(void))handler {
    MotePadMenuRow *row = [MotePadMenuRow new];
    row.title = title;
    row.shortcut = shortcut;
    row.action = handler;
    return row;
}

- (MotePadMenuRow *)toggle:(NSString *)title shortcut:(NSString *)shortcut checked:(BOOL)checked handler:(void (^)(void))handler {
    MotePadMenuRow *row = [self row:title shortcut:shortcut handler:handler];
    row.checkable = YES;
    row.checked = checked;
    return row;
}

- (NSArray<NSArray<MotePadMenuRow *> *> *)fileMenuSections {
    __weak typeof(self) ws = self;
    return @[
        @[ [self row:@"New" shortcut:@"⌘N" handler:^{ [ws mpNew]; }],
           [self row:@"Open…" shortcut:@"⌘O" handler:^{ [ws mpOpen]; }],
           [self row:@"Save" shortcut:@"⌘S" handler:^{ [ws mpSave]; }],
           [self row:@"Save As…" shortcut:@"⇧⌘S" handler:^{ [ws mpSaveAs]; }] ],
        @[ [self row:@"Page Setup…" shortcut:@"⇧⌘P" handler:^{ [ws mpPageSetup]; }],
           [self row:@"Print…" shortcut:@"⌘P" handler:^{ [ws mpPrint]; }] ],
        @[ [self row:@"Close" shortcut:@"⌘W" handler:^{ [ws mpClose]; }],
           [self row:@"Quit MotePad" shortcut:@"⌘Q" handler:^{ [ws mpQuit]; }] ],
    ];
}

- (NSArray<NSArray<MotePadMenuRow *> *> *)editMenuSections {
    __weak typeof(self) ws = self;
    return @[
        @[ [self row:@"Undo" shortcut:@"⌘Z" handler:^{ [ws editUndo]; }],
           [self row:@"Redo" shortcut:@"⇧⌘Z" handler:^{ [ws editRedo]; }] ],
        @[ [self row:@"Cut" shortcut:@"⌘X" handler:^{ [ws editCut]; }],
           [self row:@"Copy" shortcut:@"⌘C" handler:^{ [ws editCopy]; }],
           [self row:@"Paste" shortcut:@"⌘V" handler:^{ [ws editPaste]; }],
           [self row:@"Delete" shortcut:nil handler:^{ [ws editDelete]; }],
           [self row:@"Select All" shortcut:@"⌘A" handler:^{ [ws editSelectAll]; }] ],
        @[ [self row:@"Find…" shortcut:@"⌘F" handler:^{ [ws mpFind]; }],
           [self row:@"Find Next" shortcut:@"⌘G" handler:^{ [ws mpFindNext]; }],
           [self row:@"Replace…" shortcut:@"⇧⌘F" handler:^{ [ws mpReplace]; }],
           [self row:@"Go to Line…" shortcut:@"⌘L" handler:^{ [ws mpGoToLine]; }],
           [self row:@"Insert Date and Time" shortcut:@"⌘D" handler:^{ [ws mpInsertDateTime]; }] ],
    ];
}

- (NSArray<NSArray<MotePadMenuRow *> *> *)formatMenuSections {
    __weak typeof(self) ws = self;
    return @[ @[
        [self toggle:@"Word Wrap" shortcut:@"⌥⌘W" checked:_wordWrap handler:^{ [ws mpToggleWordWrap]; }],
        [self row:@"Show Fonts" shortcut:@"⌘T" handler:^{ [ws mpShowFonts]; }],
    ] ];
}

- (NSArray<NSArray<MotePadMenuRow *> *> *)viewMenuSections {
    __weak typeof(self) ws = self;
    return @[ @[
        [self toggle:@"Status Bar" shortcut:@"⌥⌘S" checked:_statusBarVisible handler:^{ [ws mpToggleStatusBar]; }],
        [self toggle:@"Line Numbers" shortcut:@"⌥⌘L" checked:_lineNumbersVisible handler:^{ [ws mpToggleLineNumbers]; }],
    ] ];
}

- (NSArray<NSArray<MotePadMenuRow *> *> *)helpMenuSections {
    __weak typeof(self) ws = self;
    return @[ @[ [self row:@"MotePad Help" shortcut:@"⌘?" handler:^{ [ws mpAbout]; }] ] ];
}

- (NSArray<NSArray<MotePadMenuRow *> *> *)menuSectionsForTitle:(NSString *)title {
    if ([title isEqualToString:@"File"]) return [self fileMenuSections];
    if ([title isEqualToString:@"Edit"]) return [self editMenuSections];
    if ([title isEqualToString:@"Format"]) return [self formatMenuSections];
    if ([title isEqualToString:@"View"]) return [self viewMenuSections];
    if ([title isEqualToString:@"Help"]) return [self helpMenuSections];
    return @[];
}

- (void)menuButtonTapped:(UIButton *)button {
    [self openMenuForButton:button];
}

- (void)openMenuForButton:(UIButton *)button {
    NSArray<NSArray<MotePadMenuRow *> *> *sections = [self menuSectionsForTitle:[button titleForState:UIControlStateNormal]];
    if (sections.count == 0)
        return;
    MotePadPopupMenuController *menu = [[MotePadPopupMenuController alloc] initWithSections:sections theme:self.workspaceTheme];
    menu.modalPresentationStyle = UIModalPresentationPopover;
    UIPopoverPresentationController *popover = menu.popoverPresentationController;
    popover.sourceView = button;
    popover.sourceRect = button.bounds;
    popover.permittedArrowDirections = UIPopoverArrowDirectionUp;
    popover.delegate = menu;
    popover.backgroundColor = self.workspaceTheme[@"card"] ?: UIColor.secondarySystemBackgroundColor;
    [self presentViewController:menu animated:YES completion:nil];
}

#pragma mark Hardware-keyboard shortcuts

- (UIKeyCommand *)key:(NSString *)input flags:(UIKeyModifierFlags)flags action:(SEL)action title:(NSString *)title {
    UIKeyCommand *command = [UIKeyCommand keyCommandWithInput:input modifierFlags:flags action:action];
    command.discoverabilityTitle = title;  // shown in the iPad ⌘-hold shortcut HUD
    return command;
}

// Open a named in-window menu from the keyboard (the custom popover works on iOS 15+).
- (void)openMenuForButtonTitled:(NSString *)title {
    for (UIButton *button in _menuButtons) {
        if ([[button titleForState:UIControlStateNormal] isEqualToString:title]) {
            [self openMenuForButton:button];
            return;
        }
    }
}

- (void)openFileMenu { [self openMenuForButtonTitled:@"File"]; }
- (void)openEditMenu { [self openMenuForButtonTitled:@"Edit"]; }
- (void)openFormatMenu { [self openMenuForButtonTitled:@"Format"]; }
- (void)openViewMenu { [self openMenuForButtonTitled:@"View"]; }
- (void)openHelpMenu { [self openMenuForButtonTitled:@"Help"]; }

- (NSArray<UIKeyCommand *> *)keyCommands {
    UIKeyModifierFlags cmd = UIKeyModifierCommand;
    UIKeyModifierFlags shiftCmd = UIKeyModifierCommand | UIKeyModifierShift;
    UIKeyModifierFlags optCmd = UIKeyModifierCommand | UIKeyModifierAlternate;
    NSMutableArray<UIKeyCommand *> *commands = [@[
        // File
        [self key:@"n" flags:cmd action:@selector(mpNew) title:@"New"],
        [self key:@"o" flags:cmd action:@selector(mpOpen) title:@"Open…"],
        [self key:@"s" flags:cmd action:@selector(mpSave) title:@"Save"],
        [self key:@"s" flags:shiftCmd action:@selector(mpSaveAs) title:@"Save As…"],
        [self key:@"p" flags:cmd action:@selector(mpPrint) title:@"Print…"],
        [self key:@"p" flags:shiftCmd action:@selector(mpPageSetup) title:@"Page Setup…"],
        [self key:@"w" flags:cmd action:@selector(mpClose) title:@"Close"],
        [self key:@"q" flags:cmd action:@selector(mpQuit) title:@"Quit MotePad"],
        // Edit — Undo/Redo/Cut/Copy/Paste/Select All are provided natively by UITextView.
        [self key:@"f" flags:cmd action:@selector(mpFind) title:@"Find…"],
        [self key:@"g" flags:cmd action:@selector(mpFindNext) title:@"Find Next"],
        [self key:@"f" flags:shiftCmd action:@selector(mpReplace) title:@"Replace…"],
        [self key:@"l" flags:cmd action:@selector(mpGoToLine) title:@"Go to Line…"],
        [self key:@"d" flags:cmd action:@selector(mpInsertDateTime) title:@"Insert Date and Time"],
        // Format
        [self key:@"w" flags:optCmd action:@selector(mpToggleWordWrap) title:@"Word Wrap"],
        [self key:@"t" flags:cmd action:@selector(mpShowFonts) title:@"Show Fonts"],
        // View
        [self key:@"s" flags:optCmd action:@selector(mpToggleStatusBar) title:@"Status Bar"],
        [self key:@"l" flags:optCmd action:@selector(mpToggleLineNumbers) title:@"Line Numbers"],
        // Help
        [self key:@"?" flags:cmd action:@selector(mpAbout) title:@"MotePad Help"],
    ] mutableCopy];
    // Open each in-window menu from the keyboard (Control-Option-letter).
    UIKeyModifierFlags ctrlOpt = UIKeyModifierControl | UIKeyModifierAlternate;
    [commands addObjectsFromArray:@[
        [self key:@"f" flags:ctrlOpt action:@selector(openFileMenu) title:@"File Menu"],
        [self key:@"e" flags:ctrlOpt action:@selector(openEditMenu) title:@"Edit Menu"],
        [self key:@"o" flags:ctrlOpt action:@selector(openFormatMenu) title:@"Format Menu"],
        [self key:@"v" flags:ctrlOpt action:@selector(openViewMenu) title:@"View Menu"],
        [self key:@"h" flags:ctrlOpt action:@selector(openHelpMenu) title:@"Help Menu"],
    ]];
    return commands;
}

#pragma mark Document state

- (NSString *)documentDisplayName {
    return _currentGuestPath.lastPathComponent.length ? _currentGuestPath.lastPathComponent : @"Untitled";
}

- (void)updateDocTitle {
    NSString *name = [self documentDisplayName];
    _docTitleLabel.text = _dirty ? [@"● " stringByAppendingString:name] : name;
}

- (void)setDirty:(BOOL)dirty {
    if (_dirty == dirty) return;
    _dirty = dirty;
    [self updateDocTitle];
}

- (void)loadText:(NSString *)text guestPath:(NSString *)guestPath {
    _textView.text = text ?: @"";
    _currentGuestPath = guestPath;
    [_textView.undoManager removeAllActions];
    [self setDirty:NO];
    [self updateDocTitle];  // setDirty: no-ops when already clean; refresh the filename explicitly
    [self updateStatusBar];
    if (_lineNumbersVisible) { [self updateGutterWidth]; [_gutter setNeedsDisplay]; }
    [self autosaveDraftNow];  // document is clean now — this clears any stale draft
}

#pragma mark Draft autosave

// Unsaved text used to live only in _textView.text, so a background (jetsam) kill —
// routine for this app — silently discarded the document. Every MotePad window now
// mirrors its dirty text to a small host-side plist under Application Support (never
// inside the guest FS), debounced ~2s after the last keystroke and flushed
// immediately when the app backgrounds. On window (re)creation the draft is restored
// if it is newer than the underlying file's last save. A corrupt/unreadable draft is
// ignored (dictionaryWithContentsOfURL just returns nil) — never a launch crash.
//
// The workspace can host several MotePad windows (it is not a "global" singleton
// tool), so each window claims a numbered draft slot in creation order; a restored
// layout recreates windows in the saved order, so the slots line back up.

static NSURL *MotePadDraftDirectoryURL(void) {
    NSURL *base = [NSFileManager.defaultManager URLsForDirectory:NSApplicationSupportDirectory
                                                       inDomains:NSUserDomainMask].firstObject;
    if (base == nil)
        return nil;
    NSURL *dir = [base URLByAppendingPathComponent:@"MotePadDrafts" isDirectory:YES];
    [NSFileManager.defaultManager createDirectoryAtURL:dir withIntermediateDirectories:YES
                                            attributes:nil error:NULL];
    return dir;
}

// Main-thread only (claim/release both run on main).
static NSMutableSet<NSString *> *MotePadClaimedDraftSlots(void) {
    static NSMutableSet<NSString *> *slots;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ slots = [NSMutableSet set]; });
    return slots;
}

+ (void)releaseDraftSlotNamed:(NSString *)name {
    if (name.length == 0)
        return;
    if (NSThread.isMainThread)
        [MotePadClaimedDraftSlots() removeObject:name];
    else
        dispatch_async(dispatch_get_main_queue(), ^{ [MotePadClaimedDraftSlots() removeObject:name]; });
}

- (void)claimDraftSlot {
    NSMutableSet<NSString *> *claimed = MotePadClaimedDraftSlots();
    for (NSUInteger slot = 0; slot < 64; slot++) {
        NSString *name = [NSString stringWithFormat:@"draft-%lu.plist", (unsigned long)slot];
        if (![claimed containsObject:name]) {
            [claimed addObject:name];
            _draftSlotName = name;
            return;
        }
    }
    _draftSlotName = nil;  // implausibly many windows: skip autosave rather than share a slot
}

- (NSURL *)draftFileURL {
    if (_draftSlotName.length == 0)
        return nil;
    return [MotePadDraftDirectoryURL() URLByAppendingPathComponent:_draftSlotName isDirectory:NO];
}

- (void)scheduleDraftAutosave {
    [_draftTimer invalidate];
    __weak typeof(self) ws = self;  // block-based timer: a target-based one would retain self
    _draftTimer = [NSTimer scheduledTimerWithTimeInterval:2.0 repeats:NO block:^(__unused NSTimer *t) {
        [ws autosaveDraftNow];
    }];
}

- (NSData *)draftDataSnapshot {
    NSMutableDictionary *draft = [NSMutableDictionary dictionary];
    draft[@"text"] = _textView.text ?: @"";
    draft[@"dirty"] = @YES;
    draft[@"date"] = [NSDate date];
    if (_currentGuestPath != nil)
        draft[@"path"] = _currentGuestPath;
    return [NSPropertyListSerialization dataWithPropertyList:draft
                                                      format:NSPropertyListBinaryFormat_v1_0
                                                     options:0 error:NULL];
}

- (void)autosaveDraftNow {
    [_draftTimer invalidate];
    _draftTimer = nil;
    NSURL *url = [self draftFileURL];
    if (url == nil)
        return;
    if (!_dirty) {
        [self clearDraft];
        return;
    }
    NSData *data = [self draftDataSnapshot];  // snapshot on main; write off it
    dispatch_async(MotePadDocumentStore.ioQueue, ^{
        [data writeToURL:url options:NSDataWritingAtomic error:NULL];
    });
}

// Synchronous flush: the debounce timer won't fire once the app is suspended, and a
// queued async write might sit behind a slow VFS operation, so write inline here.
- (void)motePadFlushDraftForBackground:(__unused NSNotification *)notification {
    [_draftTimer invalidate];
    _draftTimer = nil;
    NSURL *url = [self draftFileURL];
    if (url == nil)
        return;
    if (!_dirty) {
        [NSFileManager.defaultManager removeItemAtURL:url error:NULL];
        return;
    }
    [[self draftDataSnapshot] writeToURL:url options:NSDataWritingAtomic error:NULL];
}

- (void)clearDraft {
    NSURL *url = [self draftFileURL];
    if (url == nil)
        return;
    dispatch_async(MotePadDocumentStore.ioQueue, ^{
        [NSFileManager.defaultManager removeItemAtURL:url error:NULL];
    });
}

- (void)restoreDraftIfNewer {
    NSURL *url = [self draftFileURL];
    if (url == nil)
        return;
    NSDictionary *draft = [NSDictionary dictionaryWithContentsOfURL:url];
    NSString *text = draft[@"text"];
    if (![text isKindOfClass:NSString.class])
        return;  // missing or corrupt draft: start clean
    if (![draft[@"dirty"] isKindOfClass:NSNumber.class] || ![draft[@"dirty"] boolValue])
        return;  // clean drafts are never restored (and shouldn't exist)
    NSString *path = [draft[@"path"] isKindOfClass:NSString.class] ? draft[@"path"] : nil;
    NSDate *draftDate = [draft[@"date"] isKindOfClass:NSDate.class] ? draft[@"date"] : nil;

    __weak typeof(self) ws = self;
    void (^apply)(void) = ^{
        typeof(self) ss = ws;
        if (ss == nil || ss->_dirty || ss->_textView.text.length > 0)
            return;  // the user already started typing; don't stomp their work
        ss->_textView.text = text;
        ss->_currentGuestPath = path;
        [ss->_textView.undoManager removeAllActions];
        [ss setDirty:YES];  // restored text is unsaved — show the ● indicator
        [ss updateStatusBar];
        if (ss->_lineNumbersVisible) { [ss updateGutterWidth]; [ss->_gutter setNeedsDisplay]; }
    };

    if (path == nil) {
        apply();  // untitled draft: nothing on disk to compare against
        return;
    }
    // Only restore when the draft is newer than the document's last save; if the file
    // was saved more recently (e.g. edited from the guest), the draft is stale.
    [[MotePadDocumentStore sharedStore] modificationDateAtGuestPath:path completion:^(NSDate *fileDate) {
        if (fileDate != nil && draftDate != nil && [fileDate compare:draftDate] != NSOrderedAscending) {
            [ws clearDraft];
            return;
        }
        apply();
    }];
}

#pragma mark File actions

- (void)mpNew {
    __weak typeof(self) ws = self;
    [self promptToSaveIfDirtyThen:^{ [ws loadText:@"" guestPath:nil]; }];
}

// Editor lock while a queued file operation is in flight: the guest-VFS work runs on
// the store's serial background queue (it blocks on emulator locks and must never run
// on the main thread), and until it completes the buffer must not change underneath
// the pending read/write. Counted so overlapping operations can't re-enable early.
- (void)beginFileOperation:(NSString *)statusText {
    _busyCount++;
    _textView.editable = NO;
    _statusLabel.text = statusText;
}

- (void)endFileOperation {
    if (_busyCount > 0)
        _busyCount--;
    if (_busyCount == 0) {
        _textView.editable = YES;
        [self updateStatusBar];
    }
}

- (void)mpOpen {
    __weak typeof(self) ws = self;
    [self promptToSaveIfDirtyThen:^{
        [ws presentBrowserWithMode:MotePadBrowserModeOpen suggestedName:nil completion:^(NSString *path) {
            if (path == nil) return;
            typeof(self) ss = ws;
            if (ss == nil) return;
            [ss beginFileOperation:@"Opening…"];
            [[MotePadDocumentStore sharedStore] readTextFileAtGuestPath:path
                                                             completion:^(NSString *text, NSError *error) {
                typeof(self) s2 = ws;
                if (s2 == nil) return;
                [s2 endFileOperation];
                if (text == nil) {
                    [s2 presentError:error title:@"Couldn’t Open"];
                    return;
                }
                [s2 loadText:text guestPath:path];
            }];
        }];
    }];
}

// WorkspaceFileOpenable: same read/load pipeline as mpOpen's browser-selection
// completion, just entered with an already-known path (from the File Manager)
// instead of from MotePad's own file browser.
- (void)workspaceOpenFileAtGuestPath:(NSString *)guestPath {
    __weak typeof(self) ws = self;
    [self promptToSaveIfDirtyThen:^{
        typeof(self) ss = ws;
        if (ss == nil) return;
        [ss beginFileOperation:@"Opening…"];
        [[MotePadDocumentStore sharedStore] readTextFileAtGuestPath:guestPath
                                                         completion:^(NSString *text, NSError *error) {
            typeof(self) s2 = ws;
            if (s2 == nil) return;
            [s2 endFileOperation];
            if (text == nil) {
                [s2 presentError:error title:@"Couldn’t Open"];
                return;
            }
            [s2 loadText:text guestPath:guestPath];
        }];
    }];
}

- (void)mpSave {
    if (_currentGuestPath == nil) {
        [self mpSaveAs];
        return;
    }
    [self writeToGuestPath:_currentGuestPath completion:nil];
}

- (void)mpSaveAs {
    __weak typeof(self) ws = self;
    [self presentBrowserWithMode:MotePadBrowserModeSave suggestedName:[self documentDisplayName]
                      completion:^(NSString *path) {
        if (path == nil) return;
        [ws writeToGuestPath:path completion:nil];
    }];
}

- (void)mpPrint {
    UIPrintInteractionController *controller = [UIPrintInteractionController sharedPrintController];
    if (controller == nil) return;  // device can't print
    UIPrintInfo *info = [UIPrintInfo printInfo];
    info.outputType = UIPrintInfoOutputGeneral;
    info.jobName = [self documentDisplayName];
    controller.printInfo = info;
    UISimpleTextPrintFormatter *formatter = [[UISimpleTextPrintFormatter alloc] initWithText:_textView.text ?: @""];
    formatter.font = _textView.font;
    controller.printFormatter = formatter;
    // Anchor on the File menu button — presentFromRect:inView: is valid on both idioms
    // (iPad requires a source; iPhone presents modally regardless).
    UIView *anchor = _menuButtons.firstObject ?: self.view;
    [controller presentFromRect:anchor.bounds inView:anchor animated:YES completionHandler:nil];
}

- (void)mpPageSetup {
    // iOS folds paper size/orientation into the print sheet, so Page Setup opens the same UI.
    [self mpPrint];
}

- (void)mpClose {
    __weak typeof(self) ws = self;
    [self promptToSaveIfDirtyThen:^{
        // Deliberate close (the user saved or chose Don't Save): drop the autosaved
        // draft so discarded text isn't resurrected in the next MotePad window. The
        // chrome × path skips this on purpose — it never prompts, so its draft is
        // the only thing protecting unsaved text.
        [ws clearDraft];
        [ws closeHostWindow];
    }];
}

- (void)mpQuit {
    // iOS has no app-level quit; the applet's Quit just closes its window, like Close.
    [self mpClose];
}

// Walk up to the containing Workspace window and fire its close handler (the same path
// the window's × button uses, so the host's bookkeeping/layout save runs).
- (void)closeHostWindow {
    UIView *view = self.view;
    while (view != nil && ![view isKindOfClass:ISHWorkspaceContainedWindowView.class])
        view = view.superview;
    ISHWorkspaceContainedWindowView *window = (ISHWorkspaceContainedWindowView *)view;
    if (window.closeHandler != nil)
        window.closeHandler();
}

// Queued save: the write happens on the store's serial background queue, the editor
// is locked while it's in flight, and the completion (main thread) reports success.
- (void)writeToGuestPath:(NSString *)path completion:(void (^)(BOOL ok))completion {
    __weak typeof(self) ws = self;
    [self beginFileOperation:@"Saving…"];
    [[MotePadDocumentStore sharedStore] writeText:_textView.text toGuestPath:path
                                       completion:^(BOOL ok, NSError *error) {
        typeof(self) ss = ws;
        if (ss == nil) {
            if (completion) completion(NO);
            return;
        }
        [ss endFileOperation];
        if (!ok) {
            [ss presentError:error title:@"Couldn’t Save"];
        } else {
            ss->_currentGuestPath = path;
            [ss setDirty:NO];
            [ss updateDocTitle];  // reflect the (possibly new) filename even if it was already clean
            [ss clearDraft];      // the document is safely on disk; the draft is obsolete
        }
        if (completion) completion(ok);
    }];
}

// Present the guest file browser rooted at the current document's directory (or the
// store's default writable directory for an untitled document — resolved on the I/O
// queue, since checking /AOK/persist may touch the guest VFS).
- (void)presentBrowserWithMode:(MotePadBrowserMode)mode
                 suggestedName:(NSString *)suggestedName
                    completion:(void (^)(NSString *))completion {
    if (_currentGuestPath.length) {
        [self presentBrowserWithMode:mode directory:_currentGuestPath.stringByDeletingLastPathComponent
                       suggestedName:suggestedName completion:completion];
        return;
    }
    __weak typeof(self) ws = self;
    [[MotePadDocumentStore sharedStore] defaultDirectoryGuestPathWithCompletion:^(NSString *directory) {
        [ws presentBrowserWithMode:mode directory:directory
                     suggestedName:suggestedName completion:completion];
    }];
}

- (void)presentBrowserWithMode:(MotePadBrowserMode)mode
                     directory:(NSString *)directory
                 suggestedName:(NSString *)suggestedName
                    completion:(void (^)(NSString *))completion {
    MotePadFileBrowserViewController *browser =
        [[MotePadFileBrowserViewController alloc] initWithMode:mode directory:directory
                                                 suggestedName:suggestedName completion:completion];
    UINavigationController *nav = [[UINavigationController alloc] initWithRootViewController:browser];
    nav.modalPresentationStyle = UIModalPresentationFormSheet;
    [self presentViewController:nav animated:YES completion:nil];
}

- (void)promptToSaveIfDirtyThen:(void (^)(void))proceed {
    if (!_dirty) {
        if (proceed) proceed();
        return;
    }
    UIAlertController *alert =
        [UIAlertController alertControllerWithTitle:@"Unsaved Changes"
                                            message:[NSString stringWithFormat:@"Save changes to %@?", [self documentDisplayName]]
                                     preferredStyle:UIAlertControllerStyleAlert];
    __weak typeof(self) ws = self;
    [alert addAction:[UIAlertAction actionWithTitle:@"Save" style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction *a) {
        typeof(self) ss = ws;
        if (ss == nil) return;
        void (^proceedIfSaved)(BOOL) = ^(BOOL ok) {
            if (ok && proceed) proceed();
        };
        if (ss->_currentGuestPath == nil) {
            // No path yet: run Save As, and only proceed if the write actually happened.
            [ss presentBrowserWithMode:MotePadBrowserModeSave suggestedName:[ss documentDisplayName]
                            completion:^(NSString *path) {
                typeof(self) s2 = ws;
                if (path != nil && s2 != nil)
                    [s2 writeToGuestPath:path completion:proceedIfSaved];
            }];
        } else {
            [ss writeToGuestPath:ss->_currentGuestPath completion:proceedIfSaved];
        }
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Don’t Save" style:UIAlertActionStyleDestructive
                                            handler:^(__unused UIAlertAction *a) { if (proceed) proceed(); }]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}

#pragma mark Edit actions

// Standard editing routed through the text view. Kept as methods (not ivar access
// through the menu's weak `self`) — messaging a possibly-nil weak pointer is a safe
// no-op, whereas dereferencing `ws->_textView` is disallowed under ARC.
- (void)editUndo { [_textView.undoManager undo]; }
- (void)editRedo { [_textView.undoManager redo]; }
- (void)editCut { [_textView cut:nil]; }
- (void)editCopy { [_textView copy:nil]; }
- (void)editPaste { [_textView paste:nil]; }
- (void)editSelectAll { [_textView selectAll:nil]; }
- (void)editDelete {
    // Remove the current selection; with an empty selection there's nothing to delete
    // (the hardware Delete/Backspace key handles single-character deletion natively).
    if (_textView.selectedRange.length > 0)
        [_textView deleteBackward];
}

- (void)mpFind {
    if (@available(iOS 16.0, *)) {
        _textView.findInteractionEnabled = YES;
        [_textView.findInteraction presentFindNavigatorShowingReplace:NO];
        return;
    }
    // Pre-iOS 16 fallback: a one-shot forward find from the caret.
    UIAlertController *alert =
        [UIAlertController alertControllerWithTitle:@"Find" message:nil
                                     preferredStyle:UIAlertControllerStyleAlert];
    [alert addTextFieldWithConfigurationHandler:^(UITextField *tf) {
        tf.placeholder = @"Search";
        tf.autocapitalizationType = UITextAutocapitalizationTypeNone;
    }];
    __weak typeof(self) ws = self;
    [alert addAction:[UIAlertAction actionWithTitle:@"Find Next" style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction *a) {
        [ws findNext:alert.textFields.firstObject.text];
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)mpFindNext {
    if (_lastSearch.length > 0) { [self findNext:_lastSearch]; return; }
    [self mpFind];
}

- (void)mpReplace {
    if (@available(iOS 16.0, *)) {
        _textView.findInteractionEnabled = YES;
        [_textView.findInteraction presentFindNavigatorShowingReplace:YES];
        return;
    }
    [self mpFind];  // pre-iOS 16 has no system replace UI
}

- (void)findNext:(NSString *)needle {
    if (needle.length == 0) return;
    _lastSearch = [needle copy];  // remember for Find Next (⌘G)
    NSString *haystack = _textView.text;
    NSUInteger from = NSMaxRange(_textView.selectedRange);
    if (from > haystack.length) from = 0;
    NSRange search = NSMakeRange(from, haystack.length - from);
    NSRange found = [haystack rangeOfString:needle options:0 range:search];
    if (found.location == NSNotFound)  // wrap to the top
        found = [haystack rangeOfString:needle options:0 range:NSMakeRange(0, haystack.length)];
    if (found.location == NSNotFound)
        return;
    _textView.selectedRange = found;
    [_textView scrollRangeToVisible:found];
    [_textView becomeFirstResponder];
}

- (void)mpGoToLine {
    UIAlertController *alert =
        [UIAlertController alertControllerWithTitle:@"Go to Line" message:nil
                                     preferredStyle:UIAlertControllerStyleAlert];
    [alert addTextFieldWithConfigurationHandler:^(UITextField *tf) {
        tf.placeholder = @"Line number";
        tf.keyboardType = UIKeyboardTypeNumberPad;
    }];
    __weak typeof(self) ws = self;
    [alert addAction:[UIAlertAction actionWithTitle:@"Go" style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction *a) {
        [ws goToLine:alert.textFields.firstObject.text.integerValue];
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)goToLine:(NSInteger)line {
    if (line < 1) line = 1;
    NSString *text = _textView.text;
    NSUInteger index = 0;
    NSInteger current = 1;
    while (current < line && index < text.length) {
        NSRange nl = [text rangeOfString:@"\n" options:0 range:NSMakeRange(index, text.length - index)];
        if (nl.location == NSNotFound) { index = text.length; break; }
        index = NSMaxRange(nl);
        current++;
    }
    NSRange target = NSMakeRange(index, 0);
    _textView.selectedRange = target;
    [_textView scrollRangeToVisible:target];
    [_textView becomeFirstResponder];
    [self updateStatusBar];
}

- (void)mpInsertDateTime {
    NSDateFormatter *formatter = [NSDateFormatter new];
    formatter.dateStyle = NSDateFormatterMediumStyle;
    formatter.timeStyle = NSDateFormatterShortStyle;
    NSString *stamp = [formatter stringFromDate:[NSDate date]];
    if (!_textView.isFirstResponder)
        [_textView becomeFirstResponder];
    [_textView insertText:stamp];  // routes through UITextView so undo + dirty tracking fire
}

#pragma mark Format / View actions

- (void)mpToggleWordWrap {
    _wordWrap = !_wordWrap;
    [self applyWordWrap];
}

- (void)applyWordWrap {
    if (_wordWrap) {
        _textView.textContainer.widthTracksTextView = YES;
        _textView.textContainer.size = CGSizeMake(0, CGFLOAT_MAX);
    } else {
        // Let long lines run off to the right; UITextView (a UIScrollView) then scrolls
        // horizontally to reach them.
        _textView.textContainer.widthTracksTextView = NO;
        _textView.textContainer.size = CGSizeMake(1.0e7, CGFLOAT_MAX);
    }
    [_gutter setNeedsDisplay];
}

- (void)mpToggleStatusBar {
    _statusBarVisible = !_statusBarVisible;
    [self applyStatusBarVisibility];
}

- (void)applyStatusBarVisibility {
    _statusBar.hidden = !_statusBarVisible;
    _statusBarHeight.constant = _statusBarVisible ? 22 : 0;
}

- (void)mpToggleLineNumbers {
    _lineNumbersVisible = !_lineNumbersVisible;
    [self updateGutterWidth];
    [self applyLineNumbersVisibility];
}

- (void)applyLineNumbersVisibility {
    _gutter.hidden = !_lineNumbersVisible;
    _gutterWidth.constant = _lineNumbersVisible ? _gutterContentWidth : 0;
    [_gutter setNeedsDisplay];
}

// Size the gutter to the widest line number the document currently needs.
- (void)updateGutterWidth {
    NSString *text = _textView.text;
    NSUInteger lines = 1;
    for (NSUInteger i = 0; i < text.length; i++)
        if ([text characterAtIndex:i] == '\n') lines++;
    NSUInteger digits = 1;
    for (NSUInteger n = lines; n >= 10; n /= 10) digits++;
    if (digits < 2) digits = 2;  // reserve room for 2 digits so short files don't reflow on the 10th line
    UIFont *font = [UIFont monospacedDigitSystemFontOfSize:MAX(9.0, (_textView.font.pointSize > 0 ? _textView.font.pointSize : 13) - 1.0)
                                                    weight:UIFontWeightRegular];
    CGFloat digitWidth = [@"0" sizeWithAttributes:@{NSFontAttributeName: font}].width;
    _gutterContentWidth = ceil(digitWidth * digits) + 12.0;  // ~6pt padding each side
    if (_lineNumbersVisible)
        _gutterWidth.constant = _gutterContentWidth;
}

- (void)mpShowFonts {
    UIFontPickerViewControllerConfiguration *config = [UIFontPickerViewControllerConfiguration new];
    config.includeFaces = YES;
    UIFontPickerViewController *picker = [[UIFontPickerViewController alloc] initWithConfiguration:config];
    picker.delegate = self;
    [self presentViewController:picker animated:YES completion:nil];
}

- (void)fontPickerViewControllerDidPickFont:(UIFontPickerViewController *)viewController {
    UIFontDescriptor *descriptor = viewController.selectedFontDescriptor;
    if (descriptor != nil) {
        CGFloat size = _textView.font.pointSize > 0 ? _textView.font.pointSize : 14;
        _textView.font = [UIFont fontWithDescriptor:descriptor size:size];
    }
    if (_lineNumbersVisible) { [self updateGutterWidth]; [_gutter setNeedsDisplay]; }
    [viewController dismissViewControllerAnimated:YES completion:nil];
}

#pragma mark Help

- (void)mpAbout {
    UIAlertController *alert =
        [UIAlertController alertControllerWithTitle:@"MotePad"
                                            message:@"A Notepad-style text editor for the iSH-AOK Workspace.\n\nInspired by MotePad / TinyRetroPad — reads and writes files on the guest filesystem."
                                     preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}

#pragma mark Errors

- (void)presentError:(NSError *)error title:(NSString *)title {
    UIAlertController *alert =
        [UIAlertController alertControllerWithTitle:title
                                            message:error.localizedDescription ?: @"Unknown error"
                                     preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}

#pragma mark Status bar (Ln/Col)

- (void)updateStatusBar {
    NSString *text = _textView.text;
    NSUInteger caret = MIN(_textView.selectedRange.location, text.length);
    NSUInteger line = 1;
    NSUInteger lineStart = 0;
    for (NSUInteger i = 0; i < caret; i++) {
        if ([text characterAtIndex:i] == '\n') { line++; lineStart = i + 1; }
    }
    NSUInteger column = caret - lineStart + 1;
    _statusLabel.text = [NSString stringWithFormat:@"Ln %lu, Col %lu", (unsigned long)line, (unsigned long)column];
}

#pragma mark UITextViewDelegate

- (void)textViewDidChange:(__unused UITextView *)textView {
    [self setDirty:YES];
    [self updateStatusBar];
    if (_lineNumbersVisible) { [self updateGutterWidth]; [_gutter setNeedsDisplay]; }
    [self scheduleDraftAutosave];
}

- (void)textViewDidChangeSelection:(__unused UITextView *)textView {
    [self updateStatusBar];
}

- (void)scrollViewDidScroll:(__unused UIScrollView *)scrollView {
    if (_lineNumbersVisible) [_gutter setNeedsDisplay];
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    if (_lineNumbersVisible) [_gutter setNeedsDisplay];
}

@end

#pragma mark - Audio Player applet

static UIColor *ISHAudioHexColor(uint32_t hex) {
    return [UIColor colorWithRed:((hex >> 16) & 0xFF) / 255.0
                           green:((hex >> 8) & 0xFF) / 255.0
                            blue:(hex & 0xFF) / 255.0
                           alpha:1.0];
}

// Animated rainbow equalizer for the Music player's "device screen". Cheap: a
// low-rate timer nudges bar heights, and the player only runs it while playing
// and on-screen.
@interface WorkspaceAudioEQView : UIView
- (void)setActive:(BOOL)active;
@end

@implementation WorkspaceAudioEQView {
    NSArray<NSLayoutConstraint *> *_heights;
    NSTimer *_timer;
}

- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        NSArray<UIColor *> *colors = @[
            ISHAudioHexColor(0xFF4D4D), ISHAudioHexColor(0xFF8A3C), ISHAudioHexColor(0xFFD23C),
            ISHAudioHexColor(0xB6E83C), ISHAudioHexColor(0x4FE06A), ISHAudioHexColor(0x36D6A0),
            ISHAudioHexColor(0x33C9D6), ISHAudioHexColor(0x3AA0FF), ISHAudioHexColor(0x6B7BFF),
            ISHAudioHexColor(0xA86BFF), ISHAudioHexColor(0xE25BFF), ISHAudioHexColor(0xFF5BBF),
            ISHAudioHexColor(0xFF4D7A),
        ];
        UIStackView *row = [UIStackView new];
        row.translatesAutoresizingMaskIntoConstraints = NO;
        row.axis = UILayoutConstraintAxisHorizontal;
        row.alignment = UIStackViewAlignmentBottom;
        row.distribution = UIStackViewDistributionFillEqually;
        row.spacing = 3;
        [self addSubview:row];
        [NSLayoutConstraint activateConstraints:@[
            [row.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
            [row.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
            [row.topAnchor constraintEqualToAnchor:self.topAnchor],
            [row.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],
        ]];
        NSMutableArray<NSLayoutConstraint *> *heights = [NSMutableArray array];
        for (UIColor *color in colors) {
            UIView *bar = [UIView new];
            bar.translatesAutoresizingMaskIntoConstraints = NO;
            bar.backgroundColor = color;
            bar.layer.cornerRadius = 1.0;
            [row addArrangedSubview:bar];
            NSLayoutConstraint *height = [bar.heightAnchor constraintEqualToConstant:5];
            height.active = YES;
            [heights addObject:height];
        }
        _heights = heights;
    }
    return self;
}

- (void)setActive:(BOOL)active {
    [_timer invalidate];
    _timer = nil;
    if (active) {
        _timer = [NSTimer scheduledTimerWithTimeInterval:0.13 target:self selector:@selector(ish_tick) userInfo:nil repeats:YES];
        [self ish_tick];
    } else {
        for (NSLayoutConstraint *height in _heights) height.constant = 5;
        [UIView animateWithDuration:0.2 animations:^{ [self layoutIfNeeded]; }];
    }
}

- (void)ish_tick {
    CGFloat maxHeight = MAX(CGRectGetHeight(self.bounds), 10);
    for (NSLayoutConstraint *height in _heights)
        height.constant = 4 + (CGFloat)arc4random_uniform((uint32_t)(maxHeight - 4));
    [UIView animateWithDuration:0.11 delay:0
                        options:UIViewAnimationOptionBeginFromCurrentState | UIViewAnimationOptionCurveEaseOut
                     animations:^{ [self layoutIfNeeded]; } completion:nil];
}

- (void)dealloc { [_timer invalidate]; }

@end

@interface WorkspaceAudioPlayerToolViewController () <UITableViewDataSource, UITableViewDelegate, WorkspaceFileOpenable>
@end

@implementation WorkspaceAudioPlayerToolViewController {
    UIView *_deviceBody;
    UIView *_wheelView;
    UILabel *_counterLabel;
    WorkspaceAudioEQView *_eqView;
    UILabel *_titleLabel;
    UISlider *_progressSlider;
    UILabel *_elapsedLabel;
    UILabel *_durationLabel;
    UIButton *_shuffleButton;
    UIButton *_repeatButton;
    UIButton *_mButton;
    UIButton *_prevButton;
    UIButton *_playButton;
    UIButton *_nextButton;
    UIButton *_volButton;
    UIImageView *_speakerView;
    UISlider *_volumeSlider;
    float _volumeBeforeMute;
    UITableView *_tableView;
    NSTimer *_progressTimer;
    BOOL _scrubbing;
}

- (ISHAudioPlayerEngine *)engine { return ISHAudioPlayerEngine.sharedEngine; }

// WorkspaceFileOpenable: "open" replaces the queue with just this track and
// plays it (Finder/double-click semantics), rather than appending -- that's
// what "Add Folder to Music" in the File Manager's directory context menu is
// for. setQueue:startIndex: resolves the guest path and decodes on the
// engine's own background queue; nothing here needs its own VFS threading.
- (void)workspaceOpenFileAtGuestPath:(NSString *)guestPath {
    ISHAudioTrack *track = [[ISHAudioTrack alloc] initWithGuestPath:guestPath];
    [self.engine setQueue:@[track] startIndex:0];
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Music";

    CGFloat bodyMaxWidth = ISHWorkspaceAudioDeviceWidth();

    // --- Device body (color comes from applyDeviceColors / the workspace theme) ---
    _deviceBody = [UIView new];
    _deviceBody.translatesAutoresizingMaskIntoConstraints = NO;
    _deviceBody.layer.cornerRadius = 18;
    [self.toolContentView addSubview:_deviceBody];

    // --- Screen (always a dark display, regardless of theme) ---
    UIView *screen = [UIView new];
    screen.translatesAutoresizingMaskIntoConstraints = NO;
    screen.backgroundColor = ISHAudioHexColor(0x10141B);
    screen.layer.cornerRadius = 10;
    screen.clipsToBounds = YES;
    [_deviceBody addSubview:screen];

    _shuffleButton = [self deviceSymbolButton:@"shuffle" fallback:@"S" pointSize:12 tint:UIColor.grayColor action:@selector(toggleShuffle)];
    _repeatButton = [self deviceSymbolButton:@"repeat" fallback:@"R" pointSize:12 tint:UIColor.grayColor action:@selector(cycleRepeat)];
    [_shuffleButton setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
    [_repeatButton setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
    _counterLabel = [UILabel new];
    _counterLabel.font = [UIFont monospacedDigitSystemFontOfSize:10 weight:UIFontWeightRegular];
    _counterLabel.textColor = ISHAudioHexColor(0xE8EEF6);
    _counterLabel.textAlignment = NSTextAlignmentCenter;
    _counterLabel.text = @"0 / 0";
    UIStackView *topRow = [[UIStackView alloc] initWithArrangedSubviews:@[_shuffleButton, _counterLabel, _repeatButton]];
    topRow.axis = UILayoutConstraintAxisHorizontal;
    topRow.alignment = UIStackViewAlignmentCenter;

    _eqView = [[WorkspaceAudioEQView alloc] initWithFrame:CGRectZero];
    _eqView.translatesAutoresizingMaskIntoConstraints = NO;
    [_eqView.heightAnchor constraintEqualToConstant:20].active = YES;

    _titleLabel = [UILabel new];
    _titleLabel.font = [UIFont systemFontOfSize:11 weight:UIFontWeightMedium];
    _titleLabel.textColor = UIColor.whiteColor;
    _titleLabel.textAlignment = NSTextAlignmentCenter;
    _titleLabel.lineBreakMode = NSLineBreakByTruncatingTail;
    _titleLabel.text = @"Nothing playing";

    _progressSlider = [UISlider new];
    _progressSlider.continuous = YES;
    _progressSlider.maximumTrackTintColor = ISHAudioHexColor(0x3A3F49);
    [_progressSlider addTarget:self action:@selector(scrubStarted) forControlEvents:UIControlEventTouchDown];
    [_progressSlider addTarget:self action:@selector(scrubChanged) forControlEvents:UIControlEventValueChanged];
    [_progressSlider addTarget:self action:@selector(scrubEnded) forControlEvents:UIControlEventTouchUpInside | UIControlEventTouchUpOutside | UIControlEventTouchCancel];

    _elapsedLabel = [self screenTimeLabelWithAlignment:NSTextAlignmentLeft];
    _durationLabel = [self screenTimeLabelWithAlignment:NSTextAlignmentRight];
    UIStackView *timeRow = [[UIStackView alloc] initWithArrangedSubviews:@[_elapsedLabel, _durationLabel]];
    timeRow.axis = UILayoutConstraintAxisHorizontal;
    timeRow.distribution = UIStackViewDistributionFillEqually;

    UIStackView *screenStack = [[UIStackView alloc] initWithArrangedSubviews:@[topRow, _eqView, _titleLabel, _progressSlider, timeRow]];
    screenStack.axis = UILayoutConstraintAxisVertical;
    screenStack.spacing = 4;
    screenStack.translatesAutoresizingMaskIntoConstraints = NO;
    [screen addSubview:screenStack];
    [NSLayoutConstraint activateConstraints:@[
        [screenStack.topAnchor constraintEqualToAnchor:screen.topAnchor constant:7],
        [screenStack.leadingAnchor constraintEqualToAnchor:screen.leadingAnchor constant:9],
        [screenStack.trailingAnchor constraintEqualToAnchor:screen.trailingAnchor constant:-9],
        [screenStack.bottomAnchor constraintEqualToAnchor:screen.bottomAnchor constant:-7],
    ]];

    // --- Click wheel ---
    _wheelView = [UIView new];
    _wheelView.translatesAutoresizingMaskIntoConstraints = NO;
    _wheelView.layer.borderWidth = 1;
    [_deviceBody addSubview:_wheelView];

    _mButton = [self deviceSymbolButton:nil fallback:@"M" pointSize:11 tint:UIColor.grayColor action:@selector(menuTapped)];
    _prevButton = [self deviceSymbolButton:@"backward.fill" fallback:@"|<" pointSize:11 tint:UIColor.grayColor action:@selector(prevTapped)];
    _nextButton = [self deviceSymbolButton:@"forward.fill" fallback:@">|" pointSize:11 tint:UIColor.grayColor action:@selector(nextTapped)];
    _volButton = [self deviceSymbolButton:nil fallback:@"VOL" pointSize:9 tint:UIColor.grayColor action:@selector(volTapped)];
    _volButton.titleLabel.font = [UIFont systemFontOfSize:9 weight:UIFontWeightMedium];
    _playButton = [self deviceSymbolButton:@"play.fill" fallback:@">" pointSize:16 tint:UIColor.grayColor action:@selector(playPauseTapped)];
    _playButton.layer.cornerRadius = 23;
    _playButton.layer.borderWidth = 1;
    for (UIView *control in @[_mButton, _prevButton, _nextButton, _volButton, _playButton])
        [_wheelView addSubview:control];
    [NSLayoutConstraint activateConstraints:@[
        [_mButton.centerXAnchor constraintEqualToAnchor:_wheelView.centerXAnchor],
        [_mButton.topAnchor constraintEqualToAnchor:_wheelView.topAnchor constant:7],
        [_prevButton.centerYAnchor constraintEqualToAnchor:_wheelView.centerYAnchor],
        [_prevButton.leadingAnchor constraintEqualToAnchor:_wheelView.leadingAnchor constant:9],
        [_nextButton.centerYAnchor constraintEqualToAnchor:_wheelView.centerYAnchor],
        [_nextButton.trailingAnchor constraintEqualToAnchor:_wheelView.trailingAnchor constant:-9],
        [_volButton.centerXAnchor constraintEqualToAnchor:_wheelView.centerXAnchor],
        [_volButton.bottomAnchor constraintEqualToAnchor:_wheelView.bottomAnchor constant:-8],
        [_playButton.centerXAnchor constraintEqualToAnchor:_wheelView.centerXAnchor],
        [_playButton.centerYAnchor constraintEqualToAnchor:_wheelView.centerYAnchor],
        [_playButton.widthAnchor constraintEqualToConstant:46],
        [_playButton.heightAnchor constraintEqualToConstant:46],
    ]];

    // --- Volume slider (below the wheel) ---
    _speakerView = [UIImageView new];
    _speakerView.translatesAutoresizingMaskIntoConstraints = NO;
    _speakerView.contentMode = UIViewContentModeScaleAspectFit;
    if (@available(iOS 13.0, *)) _speakerView.image = [UIImage systemImageNamed:@"speaker.fill"];
    [_speakerView setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
    [_speakerView.widthAnchor constraintEqualToConstant:13].active = YES;
    _volumeSlider = [UISlider new];
    _volumeSlider.minimumValue = 0;
    _volumeSlider.maximumValue = 1;
    _volumeSlider.value = self.engine.volume;
    [_volumeSlider addTarget:self action:@selector(volumeChanged) forControlEvents:UIControlEventValueChanged];
    UIStackView *volumeRow = [[UIStackView alloc] initWithArrangedSubviews:@[_speakerView, _volumeSlider]];
    volumeRow.axis = UILayoutConstraintAxisHorizontal;
    volumeRow.alignment = UIStackViewAlignmentCenter;
    volumeRow.spacing = 6;
    volumeRow.translatesAutoresizingMaskIntoConstraints = NO;
    [_deviceBody addSubview:volumeRow];

    // --- Device body internal layout ---
    CGFloat pad = 11;
    [NSLayoutConstraint activateConstraints:@[
        [screen.topAnchor constraintEqualToAnchor:_deviceBody.topAnchor constant:pad],
        [screen.leadingAnchor constraintEqualToAnchor:_deviceBody.leadingAnchor constant:pad],
        [screen.trailingAnchor constraintEqualToAnchor:_deviceBody.trailingAnchor constant:-pad],
        [_wheelView.topAnchor constraintEqualToAnchor:screen.bottomAnchor constant:11],
        [_wheelView.centerXAnchor constraintEqualToAnchor:_deviceBody.centerXAnchor],
        [_wheelView.widthAnchor constraintEqualToAnchor:_deviceBody.widthAnchor multiplier:0.6],
        [_wheelView.heightAnchor constraintEqualToAnchor:_wheelView.widthAnchor],
        [volumeRow.topAnchor constraintEqualToAnchor:_wheelView.bottomAnchor constant:8],
        [volumeRow.leadingAnchor constraintEqualToAnchor:_deviceBody.leadingAnchor constant:pad],
        [volumeRow.trailingAnchor constraintEqualToAnchor:_deviceBody.trailingAnchor constant:-pad],
        [volumeRow.bottomAnchor constraintEqualToAnchor:_deviceBody.bottomAnchor constant:-pad],
    ]];

    // --- Place the device in the window: fill width, capped + centered ---
    CGFloat inset = 6.0;
    NSLayoutConstraint *bodyWidthFill = [_deviceBody.widthAnchor constraintEqualToAnchor:self.toolContentView.widthAnchor constant:-2 * inset];
    bodyWidthFill.priority = UILayoutPriorityDefaultHigh;
    [NSLayoutConstraint activateConstraints:@[
        [_deviceBody.topAnchor constraintEqualToAnchor:self.toolContentView.topAnchor constant:inset],
        [_deviceBody.centerXAnchor constraintEqualToAnchor:self.toolContentView.centerXAnchor],
        [_deviceBody.leadingAnchor constraintGreaterThanOrEqualToAnchor:self.toolContentView.leadingAnchor constant:inset],
        [_deviceBody.widthAnchor constraintLessThanOrEqualToConstant:bodyMaxWidth],
        bodyWidthFill,
    ]];

    // --- Song list below the device ---
    _tableView = [[UITableView alloc] initWithFrame:CGRectZero style:UITableViewStylePlain];
    _tableView.translatesAutoresizingMaskIntoConstraints = NO;
    _tableView.dataSource = self;
    _tableView.delegate = self;
    _tableView.backgroundColor = UIColor.clearColor;
    _tableView.rowHeight = 38;
    _tableView.alwaysBounceVertical = YES;
    _tableView.showsVerticalScrollIndicator = YES;
    [self.toolContentView addSubview:_tableView];
    // The device body is tall; if the window is shorter than it, let this yield
    // (the list compresses) rather than hard-conflicting.
    NSLayoutConstraint *tableTop = [_tableView.topAnchor constraintEqualToAnchor:_deviceBody.bottomAnchor constant:8];
    tableTop.priority = UILayoutPriorityDefaultHigh;
    [NSLayoutConstraint activateConstraints:@[
        tableTop,
        [_tableView.leadingAnchor constraintEqualToAnchor:self.toolContentView.leadingAnchor],
        [_tableView.trailingAnchor constraintEqualToAnchor:self.toolContentView.trailingAnchor],
        [_tableView.bottomAnchor constraintEqualToAnchor:self.toolContentView.bottomAnchor],
    ]];

    for (NSString *name in @[ISHAudioPlayerStateDidChangeNotification,
                             ISHAudioPlayerTrackDidChangeNotification,
                             ISHAudioPlayerQueueDidChangeNotification]) {
        [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(engineDidChange:)
                                                   name:name object:nil];
    }
    [self applyDeviceColors];
    [self refreshNowPlaying];
    [self refreshTransport];
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    _wheelView.layer.cornerRadius = CGRectGetWidth(_wheelView.bounds) / 2.0;
}

- (void)dealloc {
    [_progressTimer invalidate];
    [NSNotificationCenter.defaultCenter removeObserver:self];
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [_progressTimer invalidate];
    _progressTimer = [NSTimer scheduledTimerWithTimeInterval:0.4 target:self
                                                    selector:@selector(progressTick) userInfo:nil repeats:YES];
    [_tableView reloadData];
    [self refreshNowPlaying];
    [self refreshTransport];
}

- (void)viewWillDisappear:(BOOL)animated {
    [super viewWillDisappear:animated];
    [_progressTimer invalidate];
    _progressTimer = nil;  // playback keeps running; we just stop polling the UI
    [_eqView setActive:NO];
}

#pragma mark Button builders

- (UIButton *)deviceSymbolButton:(NSString *)symbol fallback:(NSString *)text pointSize:(CGFloat)pointSize tint:(UIColor *)tint action:(SEL)action {
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    button.translatesAutoresizingMaskIntoConstraints = NO;
    button.tintColor = tint;

    NSString *accessibilityLabel = text;
    if ([text isEqualToString:@"M"]) {
        accessibilityLabel = @"Menu";
    } else if ([text isEqualToString:@"|<"]) {
        accessibilityLabel = @"Previous Track";
    } else if ([text isEqualToString:@">|"]) {
        accessibilityLabel = @"Next Track";
    } else if ([text isEqualToString:@"VOL"]) {
        accessibilityLabel = @"Volume";
    } else if ([text isEqualToString:@">"]) {
        accessibilityLabel = @"Play/Pause";
    } else if ([text isEqualToString:@"S"]) {
        accessibilityLabel = @"Shuffle";
    } else if ([text isEqualToString:@"R"]) {
        accessibilityLabel = @"Repeat";
    }
    button.accessibilityLabel = accessibilityLabel;

    BOOL haveSymbol = NO;
    if (symbol != nil) {
        if (@available(iOS 13.0, *)) {
            UIImageSymbolConfiguration *config = [UIImageSymbolConfiguration configurationWithPointSize:pointSize weight:UIImageSymbolWeightSemibold];
            UIImage *image = [UIImage systemImageNamed:symbol withConfiguration:config];
            if (image) { [button setImage:image forState:UIControlStateNormal]; haveSymbol = YES; }
        }
    }
    if (!haveSymbol) {
        [button setTitle:text forState:UIControlStateNormal];
        [button setTitleColor:tint forState:UIControlStateNormal];
        button.titleLabel.font = [UIFont systemFontOfSize:pointSize weight:UIFontWeightSemibold];
    }
    [button addTarget:self action:action forControlEvents:UIControlEventTouchUpInside];
    return button;
}

- (UILabel *)screenTimeLabelWithAlignment:(NSTextAlignment)alignment {
    UILabel *label = [UILabel new];
    label.font = [UIFont monospacedDigitSystemFontOfSize:9 weight:UIFontWeightRegular];
    label.textColor = ISHAudioHexColor(0xAEB6C2);
    label.textAlignment = alignment;
    label.text = @"0:00";
    return label;
}

#pragma mark UI refresh

- (void)engineDidChange:(NSNotification *)note {
    [self refreshNowPlaying];
    [self refreshTransport];
    [_tableView reloadData];
}

- (void)refreshNowPlaying {
    ISHAudioTrack *track = self.engine.currentTrack;
    NSInteger total = (NSInteger)self.engine.queue.count;
    NSInteger index = self.engine.currentIndex;
    _counterLabel.text = (total > 0 && index >= 0)
        ? [NSString stringWithFormat:@"%ld / %ld", (long)(index + 1), (long)total]
        : [NSString stringWithFormat:@"0 / %ld", (long)total];
    if (track == nil) {
        _titleLabel.text = @"Nothing playing";
    } else {
        NSString *title = track.title.length ? track.title : track.guestPath.lastPathComponent;
        _titleLabel.text = track.artist.length ? [NSString stringWithFormat:@"%@ — %@", title, track.artist] : title;
    }
    NSTimeInterval duration = self.engine.duration;
    _durationLabel.text = [self stringForTime:duration];
    _progressSlider.maximumValue = duration > 0 ? duration : 1;
    _progressSlider.enabled = duration > 0;
}

- (void)refreshTransport {
    BOOL playing = (self.engine.state == ISHAudioPlaybackStatePlaying);
    [self setSymbol:(playing ? @"pause.fill" : @"play.fill") text:(playing ? @"||" : @">")
           onButton:_playButton pointSize:26];

    UIColor *activeTint = self.workspaceTheme[@"accent"] ?: UIColor.systemBlueColor;
    UIColor *inactiveTint = ISHAudioHexColor(0x5A6472);
    UIColor *shuffleTint = self.engine.shuffleEnabled ? activeTint : inactiveTint;
    _shuffleButton.tintColor = shuffleTint;
    [_shuffleButton setTitleColor:shuffleTint forState:UIControlStateNormal];
    UIColor *repeatTint = (self.engine.repeatMode == ISHAudioRepeatModeOff) ? inactiveTint : activeTint;
    _repeatButton.tintColor = repeatTint;
    [_repeatButton setTitleColor:repeatTint forState:UIControlStateNormal];
    switch (self.engine.repeatMode) {
        case ISHAudioRepeatModeOff:
            [self setSymbol:@"repeat" text:@"R" onButton:_repeatButton pointSize:15];
            break;
        case ISHAudioRepeatModeAll:
            [self setSymbol:@"repeat" text:@"R*" onButton:_repeatButton pointSize:15];
            break;
        case ISHAudioRepeatModeOne:
            [self setSymbol:@"repeat.1" text:@"R1" onButton:_repeatButton pointSize:15];
            break;
    }
    _volumeSlider.value = self.engine.volume;
    [_eqView setActive:(playing && _progressTimer != nil)];
}

// Paint the device from the workspace theme. The screen stays a dark display and
// the equalizer stays rainbow; everything else follows the theme so the player
// matches the rest of the workspace, live (this is also the workspaceApplyTheme hook).
- (void)applyDeviceColors {
    if (_deviceBody == nil) return;  // the base may invoke the theme hook before our views exist
    NSDictionary<NSString *, UIColor *> *theme = self.workspaceTheme;
    UIColor *accent = theme[@"accent"] ?: UIColor.systemBlueColor;
    UIColor *card = theme[@"card"] ?: UIColor.whiteColor;
    UIColor *cardAlt = theme[@"cardAlt"] ?: card;
    UIColor *primary = theme[@"primary"] ?: UIColor.darkTextColor;
    UIColor *stroke = theme[@"stroke"] ?: [UIColor colorWithWhite:0.0 alpha:0.15];

    _deviceBody.backgroundColor = accent;
    _wheelView.backgroundColor = card;
    _wheelView.layer.borderColor = stroke.CGColor;
    _playButton.backgroundColor = cardAlt;
    _playButton.layer.borderColor = stroke.CGColor;
    _playButton.tintColor = primary;
    [_playButton setTitleColor:primary forState:UIControlStateNormal];
    for (UIButton *button in @[_mButton, _prevButton, _nextButton, _volButton]) {
        button.tintColor = primary;
        [button setTitleColor:primary forState:UIControlStateNormal];
    }
    _speakerView.tintColor = primary;
    _volumeSlider.minimumTrackTintColor = primary;
    _volumeSlider.maximumTrackTintColor = stroke;
    _progressSlider.minimumTrackTintColor = accent;
    [self refreshTransport];  // shuffle/repeat active tint follows the accent too
}

- (void)workspaceApplyTheme {
    [super workspaceApplyTheme];
    [self applyDeviceColors];
}

// VOL on the wheel toggles mute (the slider below it is fine-grained control).
- (void)volTapped {
    if (self.engine.volume > 0.001f) {
        _volumeBeforeMute = self.engine.volume;
        self.engine.volume = 0;
    } else {
        self.engine.volume = (_volumeBeforeMute > 0.001f) ? _volumeBeforeMute : 1.0f;
    }
    _volumeSlider.value = self.engine.volume;
}

- (void)menuTapped {
    UIAlertController *sheet = [UIAlertController alertControllerWithTitle:@"Music" message:nil
                                                           preferredStyle:UIAlertControllerStyleActionSheet];
    [sheet addAction:[UIAlertAction actionWithTitle:@"Add Music…" style:UIAlertActionStyleDefault
        handler:^(__unused UIAlertAction *a) { dispatch_async(dispatch_get_main_queue(), ^{ [self addMusicTapped:self->_mButton]; }); }]];
    [sheet addAction:[UIAlertAction actionWithTitle:@"Playlists…" style:UIAlertActionStyleDefault
        handler:^(__unused UIAlertAction *a) { dispatch_async(dispatch_get_main_queue(), ^{ [self playlistsTapped:self->_mButton]; }); }]];
    [sheet addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [self presentSheet:sheet fromView:_mButton];
}

- (void)setSymbol:(NSString *)symbol text:(NSString *)text onButton:(UIButton *)button pointSize:(CGFloat)pointSize {
    if (@available(iOS 13.0, *)) {
        UIImageSymbolConfiguration *config = [UIImageSymbolConfiguration configurationWithPointSize:pointSize weight:UIImageSymbolWeightSemibold];
        UIImage *image = [UIImage systemImageNamed:symbol withConfiguration:config];
        if (image) { [button setImage:image forState:UIControlStateNormal]; return; }
    }
    [button setTitle:text forState:UIControlStateNormal];
}

- (void)progressTick {
    if (_scrubbing) return;
    NSTimeInterval t = self.engine.currentTime;
    if (self.engine.duration > 0) _progressSlider.value = t;
    _elapsedLabel.text = [self stringForTime:t];
}

- (NSString *)stringForTime:(NSTimeInterval)time {
    if (time < 0 || !isfinite(time)) time = 0;
    int total = (int)llround(time);
    return [NSString stringWithFormat:@"%d:%02d", total / 60, total % 60];
}

#pragma mark Transport actions

- (void)playPauseTapped { [self.engine togglePlayPause]; }
- (void)prevTapped { [self.engine previous]; }
- (void)nextTapped { [self.engine next]; }
- (void)toggleShuffle { self.engine.shuffleEnabled = !self.engine.shuffleEnabled; [self refreshTransport]; }

- (void)cycleRepeat {
    switch (self.engine.repeatMode) {
        case ISHAudioRepeatModeOff: self.engine.repeatMode = ISHAudioRepeatModeAll; break;
        case ISHAudioRepeatModeAll: self.engine.repeatMode = ISHAudioRepeatModeOne; break;
        case ISHAudioRepeatModeOne: self.engine.repeatMode = ISHAudioRepeatModeOff; break;
    }
    [self refreshTransport];
}

- (void)scrubStarted { _scrubbing = YES; }
- (void)scrubChanged { _elapsedLabel.text = [self stringForTime:_progressSlider.value]; }
- (void)scrubEnded {
    if (!_scrubbing) return;
    _scrubbing = NO;
    if (self.engine.duration > 0) [self.engine seekToTime:_progressSlider.value];
}

- (void)volumeChanged { self.engine.volume = _volumeSlider.value; }

#pragma mark Add Music / Playlists

- (void)addMusicTapped:(UIButton *)sender {
    UIAlertController *sheet = [UIAlertController alertControllerWithTitle:@"Add Music"
        message:@"Music lives in /AOK/persist/music by default, or add tracks from any path in the guest."
        preferredStyle:UIAlertControllerStyleActionSheet];
    [sheet addAction:[UIAlertAction actionWithTitle:@"Play Music Folder" style:UIAlertActionStyleDefault
        handler:^(__unused UIAlertAction *a) { [self loadDefaultFolderReplacing:YES]; }]];
    [sheet addAction:[UIAlertAction actionWithTitle:@"Add Music Folder to Queue" style:UIAlertActionStyleDefault
        handler:^(__unused UIAlertAction *a) { [self loadDefaultFolderReplacing:NO]; }]];
    [sheet addAction:[UIAlertAction actionWithTitle:@"Add from Path…" style:UIAlertActionStyleDefault
        handler:^(__unused UIAlertAction *a) { [self promptAddFromPath]; }]];
    [sheet addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [self presentSheet:sheet fromView:sender];
}

- (void)loadDefaultFolderReplacing:(BOOL)replace {
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSArray<ISHAudioTrack *> *tracks = [ISHAudioLibrary.sharedLibrary scanDefaultMusicDirectory];
        dispatch_async(dispatch_get_main_queue(), ^{ [self applyScannedTracks:tracks replace:replace emptyMessage:@"No audio files in /AOK/persist/music."]; });
    });
}

- (void)promptAddFromPath {
    [self promptForTextWithTitle:@"Add from Path" message:@"A guest folder or audio file path."
                     placeholder:@"/root/music" initial:nil handler:^(NSString *path) {
        NSString *trimmed = [path stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        if (trimmed.length == 0) return;
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            NSArray<ISHAudioTrack *> *tracks;
            if ([ISHAudioLibrary isSupportedAudioFileName:trimmed])
                tracks = @[[[ISHAudioTrack alloc] initWithGuestPath:trimmed]];
            else
                tracks = [ISHAudioLibrary.sharedLibrary scanGuestDirectoryAtPath:trimmed];
            dispatch_async(dispatch_get_main_queue(), ^{ [self applyScannedTracks:tracks replace:NO emptyMessage:@"No audio files found at that path."]; });
        });
    }];
}

- (void)applyScannedTracks:(NSArray<ISHAudioTrack *> *)tracks replace:(BOOL)replace emptyMessage:(NSString *)emptyMessage {
    if (tracks.count == 0) { [self showMessage:emptyMessage]; return; }
    if (replace || self.engine.queue.count == 0)
        [self.engine setQueue:tracks startIndex:0];
    else
        [self.engine enqueueTracks:tracks];
    [_tableView reloadData];
}

- (void)playlistsTapped:(UIButton *)sender {
    UIAlertController *sheet = [UIAlertController alertControllerWithTitle:@"Playlists" message:nil
        preferredStyle:UIAlertControllerStyleActionSheet];
    [sheet addAction:[UIAlertAction actionWithTitle:@"Save Queue as Playlist…" style:UIAlertActionStyleDefault
        handler:^(__unused UIAlertAction *a) { [self promptSavePlaylist]; }]];
    NSArray<NSString *> *names = [ISHAudioLibrary.sharedLibrary playlistNames];
    for (NSString *name in names) {
        [sheet addAction:[UIAlertAction actionWithTitle:[NSString stringWithFormat:@"Play “%@”", name]
            style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *a) { [self loadPlaylistNamed:name]; }]];
    }
    if (names.count > 0) {
        [sheet addAction:[UIAlertAction actionWithTitle:@"Delete Playlist…" style:UIAlertActionStyleDestructive
            handler:^(__unused UIAlertAction *a) { [self promptDeletePlaylistFromView:sender]; }]];
    }
    [sheet addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [self presentSheet:sheet fromView:sender];
}

- (void)promptSavePlaylist {
    if (self.engine.queue.count == 0) { [self showMessage:@"The queue is empty."]; return; }
    [self promptForTextWithTitle:@"Save Playlist" message:nil placeholder:@"Playlist name" initial:nil
                         handler:^(NSString *name) {
        NSString *trimmed = [name stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        if (trimmed.length == 0) return;
        [ISHAudioLibrary.sharedLibrary savePlaylistNamed:trimmed tracks:self.engine.queue];
    }];
}

- (void)loadPlaylistNamed:(NSString *)name {
    NSArray<ISHAudioTrack *> *tracks = [ISHAudioLibrary.sharedLibrary tracksForPlaylistNamed:name];
    [self applyScannedTracks:tracks replace:YES emptyMessage:@"That playlist is empty."];
}

- (void)promptDeletePlaylistFromView:(UIView *)sender {
    UIAlertController *sheet = [UIAlertController alertControllerWithTitle:@"Delete Playlist" message:nil
        preferredStyle:UIAlertControllerStyleActionSheet];
    for (NSString *name in [ISHAudioLibrary.sharedLibrary playlistNames]) {
        [sheet addAction:[UIAlertAction actionWithTitle:name style:UIAlertActionStyleDestructive
            handler:^(__unused UIAlertAction *a) { [ISHAudioLibrary.sharedLibrary deletePlaylistNamed:name]; }]];
    }
    [sheet addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [self presentSheet:sheet fromView:sender];
}

#pragma mark Alert helpers

- (void)promptForTextWithTitle:(NSString *)title message:(NSString *)message
                   placeholder:(NSString *)placeholder initial:(NSString *)initial
                       handler:(void (^)(NSString *))handler {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:title message:message
                                                           preferredStyle:UIAlertControllerStyleAlert];
    [alert addTextFieldWithConfigurationHandler:^(UITextField *field) {
        field.placeholder = placeholder;
        field.text = initial;
        field.autocorrectionType = UITextAutocorrectionTypeNo;
        field.autocapitalizationType = UITextAutocapitalizationTypeNone;
    }];
    [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault
        handler:^(__unused UIAlertAction *a) { handler(alert.textFields.firstObject.text ?: @""); }]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)showMessage:(NSString *)message {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:nil message:message
                                                           preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)presentSheet:(UIAlertController *)sheet fromView:(UIView *)sourceView {
    UIPopoverPresentationController *popover = sheet.popoverPresentationController;
    if (popover != nil) {
        popover.sourceView = sourceView ?: self.toolContentView;
        popover.sourceRect = sourceView ? sourceView.bounds : self.toolContentView.bounds;
        popover.permittedArrowDirections = UIPopoverArrowDirectionAny;
    }
    [self presentViewController:sheet animated:YES completion:nil];
}

#pragma mark Table

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    return self.engine.queue.count;
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    static NSString *const reuseID = @"AudioTrackCell";
    UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:reuseID];
    if (cell == nil)
        cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:reuseID];
    NSArray<ISHAudioTrack *> *queue = self.engine.queue;
    if ((NSUInteger)indexPath.row >= queue.count) return cell;
    ISHAudioTrack *track = queue[indexPath.row];
    cell.backgroundColor = UIColor.clearColor;
    cell.textLabel.text = track.title.length ? track.title : track.guestPath.lastPathComponent;
    cell.detailTextLabel.text = track.artist.length ? track.artist : nil;
    BOOL isCurrent = (indexPath.row == self.engine.currentIndex);
    UIColor *accent = self.workspaceTheme[@"accent"] ?: UIColor.systemBlueColor;
    UIColor *primary = self.workspaceTheme[@"primary"] ?: UIColor.labelColor;
    cell.textLabel.textColor = isCurrent ? accent : primary;
    cell.textLabel.font = [UIFont systemFontOfSize:ISHWorkspaceThemeFontSize(UIFontTextStyleSubheadline)
                                            weight:isCurrent ? UIFontWeightSemibold : UIFontWeightRegular];
    cell.detailTextLabel.textColor = self.workspaceTheme[@"secondary"] ?: UIColor.secondaryLabelColor;
    cell.detailTextLabel.font = [UIFont systemFontOfSize:ISHWorkspaceThemeFontSize(UIFontTextStyleCaption2)];
    return cell;
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    [self.engine playTrackAtIndex:indexPath.row];
}

- (UITableViewCellEditingStyle)tableView:(UITableView *)tableView editingStyleForRowAtIndexPath:(NSIndexPath *)indexPath {
    return UITableViewCellEditingStyleDelete;
}

- (void)tableView:(UITableView *)tableView commitEditingStyle:(UITableViewCellEditingStyle)editingStyle
forRowAtIndexPath:(NSIndexPath *)indexPath {
    if (editingStyle == UITableViewCellEditingStyleDelete)
        [self.engine removeTrackAtIndex:indexPath.row];
}

@end

@implementation WorkspaceShortcutsToolViewController {
    UIScrollView *_scrollView;
    UIStackView *_contentStack;
    NSMutableArray<UIButton *> *_shortcutButtons;
}

- (UIButton *)shortcutButtonWithTitle:(NSString *)title subtitle:(NSString *)subtitle identifier:(NSString *)identifier {
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    button.translatesAutoresizingMaskIntoConstraints = NO;
    button.accessibilityIdentifier = identifier;
    button.contentEdgeInsets = UIEdgeInsetsMake(8, 10, 8, 10);
    button.contentHorizontalAlignment = UIControlContentHorizontalAlignmentLeft;
    button.titleLabel.numberOfLines = 0;
    button.layer.cornerRadius = 12;
    button.layer.borderWidth = 1;
    [button setContentCompressionResistancePriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisVertical];
    [button setContentHuggingPriority:UILayoutPriorityDefaultHigh forAxis:UILayoutConstraintAxisVertical];
    CGFloat minimumHeight = ISHWorkspaceUsesPhoneLayout() ? 52.0 : 58.0;
    [button.heightAnchor constraintGreaterThanOrEqualToConstant:minimumHeight].active = YES;

    NSMutableParagraphStyle *style = [NSMutableParagraphStyle new];
    style.alignment = NSTextAlignmentLeft;
    NSMutableAttributedString *label =
        [[NSMutableAttributedString alloc] initWithString:title
                                               attributes:@{
        NSFontAttributeName: [UIFont systemFontOfSize:ISHWorkspaceThemeFontSize(UIFontTextStyleSubheadline)
                                               weight:UIFontWeightSemibold],
        NSParagraphStyleAttributeName: style,
    }];
    [label appendAttributedString:[[NSAttributedString alloc] initWithString:@"\n"
                                                                  attributes:@{
        NSFontAttributeName: [UIFont systemFontOfSize:ISHWorkspaceThemeFontSize(UIFontTextStyleCaption1)
                                               weight:UIFontWeightMedium],
        NSParagraphStyleAttributeName: style,
    }]];
    [label appendAttributedString:[[NSAttributedString alloc] initWithString:subtitle
                                                                  attributes:@{
        NSFontAttributeName: [UIFont systemFontOfSize:ISHWorkspaceThemeFontSize(UIFontTextStyleCaption1)
                                               weight:UIFontWeightMedium],
        NSParagraphStyleAttributeName: style,
    }]];
    [button setAttributedTitle:label forState:UIControlStateNormal];
    [button addTarget:self action:@selector(runShortcut:) forControlEvents:UIControlEventTouchUpInside];
    [_shortcutButtons addObject:button];
    return button;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Quick Actions";
    _shortcutButtons = [NSMutableArray array];

    _scrollView = [UIScrollView new];
    _scrollView.translatesAutoresizingMaskIntoConstraints = NO;
    [self.toolContentView addSubview:_scrollView];

    _contentStack = [UIStackView new];
    _contentStack.translatesAutoresizingMaskIntoConstraints = NO;
    _contentStack.axis = UILayoutConstraintAxisVertical;
    _contentStack.spacing = 6;
    [_scrollView addSubview:_contentStack];

    UILabel *header = [self workspaceThemeAccentLabelWithTextStyle:UIFontTextStyleHeadline monospaced:NO];
    header.text = @"Quick workspace actions";
    UILabel *detail = [self workspaceThemeSecondaryLabelWithTextStyle:UIFontTextStyleFootnote monospaced:NO];
    detail.text = @"Open the most common tools and terminal actions without leaving the workspace.";
    [_contentStack addArrangedSubview:header];
    [_contentStack addArrangedSubview:detail];

    // Desktops (row 1) are in-app and available on every device, incl. iPhone. "New Workspace"
    // opens a real iOS scene window, so it's offered only on scene-capable devices (iPad
    // multi-window); elsewhere the last row is just the Clock quick action.
    NSMutableArray<NSArray<NSDictionary<NSString *, NSString *> *> *> *rows = [NSMutableArray arrayWithArray:@[
        @[
            @{@"title": @"Layout Manager", @"subtitle": @"Save or restore this workspace", @"identifier": @"dashboard"},
            @{@"title": @"Desktops", @"subtitle": @"Manage in-app Desktops", @"identifier": ISHWorkspaceToolWorkspacesIdentifier},
        ],
        @[
            @{@"title": @"Session Shell", @"subtitle": @"Open or focus the shell", @"identifier": @"shell"},
            @{@"title": @"System Console", @"subtitle": @"Open or focus the console", @"identifier": @"console"},
        ],
        @[
            @{@"title": @"Sessions", @"subtitle": @"Inspect live terminals", @"identifier": ISHWorkspaceToolSessionsIdentifier},
            @{@"title": @"Storage", @"subtitle": @"Root and container usage", @"identifier": ISHWorkspaceToolStorageIdentifier},
        ],
        @[
            @{@"title": @"Themes", @"subtitle": @"Colors, density, wallpaper", @"identifier": ISHWorkspaceToolThemesIdentifier},
            @{@"title": @"Boot Images", @"subtitle": @"Manage installed roots", @"identifier": ISHWorkspaceToolFilesystemsIdentifier},
        ],
    ]];
    if (ISHWorkspaceSupportsSceneWindows()) {
        [rows addObject:@[
            @{@"title": @"New Workspace", @"subtitle": @"Open another workspace window", @"identifier": @"new-workspace"},
            @{@"title": @"Clock", @"subtitle": @"Quick local time", @"identifier": ISHWorkspaceToolClockIdentifier},
        ]];
    } else {
        [rows addObject:@[
            @{@"title": @"Clock", @"subtitle": @"Quick local time", @"identifier": ISHWorkspaceToolClockIdentifier},
        ]];
    }

    for (NSArray<NSDictionary<NSString *, NSString *> *> *rowDescriptors in rows) {
        UIStackView *row = [UIStackView new];
        row.axis = UILayoutConstraintAxisHorizontal;
        row.spacing = 6;
        row.distribution = UIStackViewDistributionFillEqually;
        [row setContentCompressionResistancePriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisVertical];
        [row setContentHuggingPriority:UILayoutPriorityDefaultHigh forAxis:UILayoutConstraintAxisVertical];
        for (NSDictionary<NSString *, NSString *> *descriptor in rowDescriptors) {
            [row addArrangedSubview:[self shortcutButtonWithTitle:descriptor[@"title"]
                                                         subtitle:descriptor[@"subtitle"]
                                                       identifier:descriptor[@"identifier"]]];
        }
        [_contentStack addArrangedSubview:row];
    }

    [NSLayoutConstraint activateConstraints:@[
        [_scrollView.topAnchor constraintEqualToAnchor:self.toolContentView.topAnchor],
        [_scrollView.leadingAnchor constraintEqualToAnchor:self.toolContentView.leadingAnchor],
        [_scrollView.trailingAnchor constraintEqualToAnchor:self.toolContentView.trailingAnchor],
        [_scrollView.bottomAnchor constraintEqualToAnchor:self.toolContentView.bottomAnchor],

        [_contentStack.topAnchor constraintEqualToAnchor:_scrollView.contentLayoutGuide.topAnchor constant:8],
        [_contentStack.leadingAnchor constraintEqualToAnchor:_scrollView.frameLayoutGuide.leadingAnchor constant:8],
        [_contentStack.trailingAnchor constraintEqualToAnchor:_scrollView.frameLayoutGuide.trailingAnchor constant:-8],
        [_contentStack.bottomAnchor constraintEqualToAnchor:_scrollView.contentLayoutGuide.bottomAnchor constant:-8],
        [_contentStack.widthAnchor constraintEqualToAnchor:_scrollView.frameLayoutGuide.widthAnchor constant:-16],
    ]];
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    _contentStack.spacing = ISHWorkspaceDensityValue(4, 6);
}

- (void)runShortcut:(UIButton *)sender {
    NSString *identifier = sender.accessibilityIdentifier;
    if (identifier.length == 0)
        return;
    if ([identifier isEqualToString:@"dashboard"]) {
        [self.workspaceHostViewController openDashboardWindow:nil];
    } else if ([identifier isEqualToString:@"shell"]) {
        [self.workspaceHostViewController openTerminalHerePreferringConsole:NO];
    } else if ([identifier isEqualToString:@"console"]) {
        [self.workspaceHostViewController openTerminalHerePreferringConsole:YES];
    } else if ([identifier isEqualToString:@"new-workspace"]) {
        [self.workspaceHostViewController openNewWorkspaceWindow:nil];
    } else {
        [self.workspaceHostViewController openOrFocusWorkspaceToolIdentifier:identifier];
    }
}

- (void)workspaceApplyTheme {
    [super workspaceApplyTheme];
    NSDictionary<NSString *, UIColor *> *theme = self.workspaceTheme;
    for (UIButton *button in _shortcutButtons) {
        button.backgroundColor = [theme[@"cardAlt"] colorWithAlphaComponent:0.94];
        button.layer.borderColor = theme[@"stroke"].CGColor;
        NSMutableAttributedString *title =
            [[NSMutableAttributedString alloc] initWithAttributedString:[button attributedTitleForState:UIControlStateNormal]];
        [title addAttributes:@{NSForegroundColorAttributeName: theme[@"primary"]} range:NSMakeRange(0, title.length)];
        NSRange newlineRange = [[title string] rangeOfString:@"\n"];
        if (newlineRange.location != NSNotFound) {
            NSUInteger subtitleLocation = newlineRange.location + newlineRange.length;
            if (subtitleLocation < title.length) {
                [title addAttributes:@{NSForegroundColorAttributeName: theme[@"secondary"]}
                               range:NSMakeRange(subtitleLocation, title.length - subtitleLocation)];
            }
        }
        [button setAttributedTitle:title forState:UIControlStateNormal];
    }
}

@end

static NSString *ISHWorkspaceBrowserDefaultAddress(void) {
    return @"https://duckduckgo.com/";
}

static NSString *ISHWorkspaceBrowserHomeAddress(void) {
    NSString *stored = [NSUserDefaults.standardUserDefaults stringForKey:ISHWorkspaceBrowserHomePreferenceKey];
    NSString *trimmed = [stored stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    return trimmed.length > 0 ? trimmed : ISHWorkspaceBrowserDefaultAddress();
}

static void ISHWorkspaceSetBrowserHomeAddress(NSString *address) {
    NSString *trimmed = [[address ?: @"" stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet] copy];
    if (trimmed.length == 0 || [trimmed isEqualToString:ISHWorkspaceBrowserDefaultAddress()]) {
        [NSUserDefaults.standardUserDefaults removeObjectForKey:ISHWorkspaceBrowserHomePreferenceKey];
        return;
    }
    [NSUserDefaults.standardUserDefaults setObject:trimmed forKey:ISHWorkspaceBrowserHomePreferenceKey];
}

static NSArray<NSDictionary<NSString *, NSString *> *> *ISHWorkspaceBrowserBookmarkRecords(void) {
    NSArray *records = [NSUserDefaults.standardUserDefaults arrayForKey:ISHWorkspaceBrowserBookmarksDefaultsKey];
    return [records isKindOfClass:NSArray.class] ? records : @[];
}

static void ISHWorkspaceSetBrowserBookmarkRecords(NSArray<NSDictionary<NSString *, NSString *> *> *records) {
    [NSUserDefaults.standardUserDefaults setObject:records forKey:ISHWorkspaceBrowserBookmarksDefaultsKey];
}

static NSInteger ISHWorkspaceBrowserBookmarkIndexForURL(NSString *urlString) {
    NSArray<NSDictionary<NSString *, NSString *> *> *records = ISHWorkspaceBrowserBookmarkRecords();
    for (NSUInteger index = 0; index < records.count; index++) {
        if ([records[index][@"url"] isEqualToString:urlString])
            return (NSInteger)index;
    }
    return NSNotFound;
}

static BOOL ISHWorkspaceBrowserIsBookmarked(NSString *urlString) {
    return urlString.length > 0 && ISHWorkspaceBrowserBookmarkIndexForURL(urlString) != NSNotFound;
}

static void ISHWorkspaceAddBrowserBookmark(NSString *title, NSString *urlString) {
    if (urlString.length == 0 || ISHWorkspaceBrowserIsBookmarked(urlString))
        return;
    NSMutableArray<NSDictionary<NSString *, NSString *> *> *records = [ISHWorkspaceBrowserBookmarkRecords() mutableCopy];
    [records addObject:@{@"title": title.length > 0 ? title : urlString, @"url": urlString}];
    ISHWorkspaceSetBrowserBookmarkRecords(records);
}

static void ISHWorkspaceRemoveBrowserBookmarkForURL(NSString *urlString) {
    NSInteger index = ISHWorkspaceBrowserBookmarkIndexForURL(urlString);
    if (index == NSNotFound)
        return;
    NSMutableArray<NSDictionary<NSString *, NSString *> *> *records = [ISHWorkspaceBrowserBookmarkRecords() mutableCopy];
    [records removeObjectAtIndex:(NSUInteger)index];
    ISHWorkspaceSetBrowserBookmarkRecords(records);
}

// Pinned tabs are keyed by tab *position* (stringified index), not by URL: pinning tab 3 to
// Gmail means "whenever a new tab lands in slot 3, load Gmail" — it does not retroactively touch
// a tab already open in that slot, and unpinning just removes the record so new tabs there fall
// back to the normal home address.
static NSString *const ISHWorkspaceBrowserPinnedTabsDefaultsKey = @"ISHWorkspaceBrowserPinnedTabs";

static NSDictionary<NSString *, NSDictionary<NSString *, NSString *> *> *ISHWorkspaceBrowserPinnedTabRecords(void) {
    NSDictionary *records = [NSUserDefaults.standardUserDefaults dictionaryForKey:ISHWorkspaceBrowserPinnedTabsDefaultsKey];
    return [records isKindOfClass:NSDictionary.class] ? records : @{};
}

static void ISHWorkspaceSetBrowserPinnedTabRecords(NSDictionary<NSString *, NSDictionary<NSString *, NSString *> *> *records) {
    [NSUserDefaults.standardUserDefaults setObject:records forKey:ISHWorkspaceBrowserPinnedTabsDefaultsKey];
}

static NSDictionary<NSString *, NSString *> *ISHWorkspaceBrowserPinnedTabRecordForIndex(NSInteger index) {
    NSDictionary *record = ISHWorkspaceBrowserPinnedTabRecords()[[NSString stringWithFormat:@"%ld", (long) index]];
    return [record isKindOfClass:NSDictionary.class] ? record : nil;
}

static NSString *ISHWorkspaceBrowserPinnedURLForTabIndex(NSInteger index) {
    return ISHWorkspaceBrowserPinnedTabRecordForIndex(index)[@"url"];
}

static NSString *ISHWorkspaceBrowserPinnedTitleForTabIndex(NSInteger index) {
    return ISHWorkspaceBrowserPinnedTabRecordForIndex(index)[@"title"];
}

static BOOL ISHWorkspaceBrowserTabIndexIsPinned(NSInteger index) {
    return ISHWorkspaceBrowserPinnedURLForTabIndex(index).length > 0;
}

static void ISHWorkspaceSetBrowserPinnedTabForIndex(NSInteger index, NSString *title, NSString *urlString) {
    if (urlString.length == 0)
        return;
    NSMutableDictionary<NSString *, NSDictionary<NSString *, NSString *> *> *records = [ISHWorkspaceBrowserPinnedTabRecords() mutableCopy];
    records[[NSString stringWithFormat:@"%ld", (long) index]] = @{@"title": title.length > 0 ? title : urlString, @"url": urlString};
    ISHWorkspaceSetBrowserPinnedTabRecords(records);
}

static void ISHWorkspaceRemoveBrowserPinnedTabForIndex(NSInteger index) {
    NSMutableDictionary<NSString *, NSDictionary<NSString *, NSString *> *> *records = [ISHWorkspaceBrowserPinnedTabRecords() mutableCopy];
    [records removeObjectForKey:[NSString stringWithFormat:@"%ld", (long) index]];
    ISHWorkspaceSetBrowserPinnedTabRecords(records);
}

static NSURL *ISHWorkspaceBrowserURLFromInput(NSString *input) {
    NSString *trimmed = [[input ?: @"" stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet] copy];
    if (trimmed.length == 0)
        trimmed = ISHWorkspaceBrowserDefaultAddress();

    NSURLComponents *components = [NSURLComponents componentsWithString:trimmed];
    if (components.scheme.length > 0 && components.URL != nil)
        return components.URL;

    if ([trimmed rangeOfCharacterFromSet:NSCharacterSet.whitespaceCharacterSet].location != NSNotFound) {
        NSString *query = [trimmed stringByAddingPercentEncodingWithAllowedCharacters:NSCharacterSet.URLQueryAllowedCharacterSet];
        return [NSURL URLWithString:[NSString stringWithFormat:@"https://duckduckgo.com/?q=%@", query ?: @""]];
    }

    BOOL localHost = [trimmed hasPrefix:@"localhost"] ||
        [trimmed hasPrefix:@"127."] ||
        [trimmed hasPrefix:@"10."] ||
        [trimmed hasPrefix:@"192.168."] ||
        [trimmed hasPrefix:@"172.16."] ||
        [trimmed hasPrefix:@"172.17."] ||
        [trimmed hasPrefix:@"172.18."] ||
        [trimmed hasPrefix:@"172.19."] ||
        [trimmed hasPrefix:@"172.2"] ||
        [trimmed hasPrefix:@"172.30."] ||
        [trimmed hasPrefix:@"172.31."];
    NSString *scheme = localHost ? @"http" : @"https";
    return [NSURL URLWithString:[NSString stringWithFormat:@"%@://%@", scheme, trimmed]];
}

@implementation WorkspaceWorkspacesToolViewController {
    UIScrollView *_scrollView;
    UIStackView *_contentStack;
    UIStackView *_rowsStack;
    UIButton *_newWorkspaceButton;
    UIButton *_closeHiddenButton;
    NSMutableArray<UIButton *> *_trackedButtons;
    NSMutableDictionary<NSString *, UIImageView *> *_previewImageViewsByIdentifier;
    NSArray<NSDictionary<NSString *, id> *> *_sceneDescriptors;
}

- (NSDictionary<NSString *, id> *)sceneDescriptorWithIdentifier:(NSString *)identifier {
    for (NSDictionary<NSString *, id> *descriptor in _sceneDescriptors) {
        if ([descriptor[@"identifier"] isEqualToString:identifier])
            return descriptor;
    }
    return nil;
}

- (UIButton *)workspacesActionButtonWithTitle:(NSString *)title action:(SEL)action {
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    button.translatesAutoresizingMaskIntoConstraints = NO;
    button.contentHorizontalAlignment = UIControlContentHorizontalAlignmentCenter;
    button.titleLabel.font = [UIFont systemFontOfSize:(ISHWorkspaceUsesPhoneLayout() ? 14.0 : 16.0) weight:UIFontWeightSemibold];
    button.titleLabel.adjustsFontSizeToFitWidth = YES;
    button.titleLabel.minimumScaleFactor = 0.7;
    button.layer.cornerRadius = ISHWorkspaceUsesPhoneLayout() ? 9.0 : 12.0;
    button.layer.borderWidth = 1;
    [button setTitle:title forState:UIControlStateNormal];
    [button addTarget:self action:action forControlEvents:UIControlEventTouchUpInside];
    [button.heightAnchor constraintEqualToConstant:ISHWorkspaceUsesPhoneLayout() ? 30.0 : 40.0].active = YES;
    return button;
}

- (UIButton *)workspacesIconButtonWithSymbol:(NSString *)symbol fallback:(NSString *)fallback action:(SEL)action {
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    button.translatesAutoresizingMaskIntoConstraints = NO;
    if (@available(iOS 13.0, *)) {
        [button setImage:[UIImage systemImageNamed:symbol] forState:UIControlStateNormal];
    } else {
        [button setTitle:fallback forState:UIControlStateNormal];
    }
    button.accessibilityLabel = fallback;
    button.layer.cornerRadius = 12;
    [button addTarget:self action:action forControlEvents:UIControlEventTouchUpInside];
    [button.heightAnchor constraintEqualToConstant:ISHWorkspaceUsesPhoneLayout() ? 36.0 : 40.0].active = YES;
    return button;
}

- (void)saveLayoutFromApplet:(id)sender {
    (void) sender;
    [(id)self.workspaceHostViewController saveWorkspaceLayout:nil];
}

- (void)restoreLayoutFromApplet:(id)sender {
    (void) sender;
    [(id)self.workspaceHostViewController restoreWorkspaceLayout:nil];
}

- (UIImage *)scenePreviewImageForDescriptor:(NSDictionary<NSString *, id> *)descriptor size:(CGSize)size {
    NSDictionary<NSString *, UIColor *> *theme = self.workspaceTheme;
    UIColor *backgroundTop = [theme[@"backgroundTop"] colorWithAlphaComponent:0.94];
    UIColor *backgroundBottom = [theme[@"backgroundBottom"] colorWithAlphaComponent:0.98];
    UIColor *frameColor = [theme[@"card"] colorWithAlphaComponent:0.96];
    UIColor *titleBarColor = [theme[@"cardAlt"] colorWithAlphaComponent:0.98];
    UIColor *accentColor = theme[@"accent"];
    UIColor *accentAlt = theme[@"accentAlt"];
    BOOL current = [descriptor[@"isCurrent"] boolValue];
    NSString *role = descriptor[@"role"] ?: @"Unknown";

    UIGraphicsImageRenderer *renderer = [[UIGraphicsImageRenderer alloc] initWithSize:size];
    return [renderer imageWithActions:^(UIGraphicsImageRendererContext *rendererContext) {
        CGContextRef context = rendererContext.CGContext;
        CGRect rect = CGRectMake(0, 0, size.width, size.height);
        ISHWorkspaceThemeDrawLinearGradient(context, rect, backgroundTop, backgroundBottom);

        CGRect mainFrame = CGRectInset(rect, 4, 4);
        UIBezierPath *outerPath = [UIBezierPath bezierPathWithRoundedRect:mainFrame cornerRadius:9];
        [frameColor setFill];
        [outerPath fill];

        CGRect titleBar = CGRectMake(CGRectGetMinX(mainFrame),
                                     CGRectGetMinY(mainFrame),
                                     CGRectGetWidth(mainFrame),
                                     12);
        UIBezierPath *titlePath = [UIBezierPath bezierPathWithRoundedRect:titleBar cornerRadius:9];
        [titleBarColor setFill];
        [titlePath fill];

        CGRect dotRect = CGRectMake(CGRectGetMinX(titleBar) + 5, CGRectGetMidY(titleBar) - 1.75, 3.5, 3.5);
        UIBezierPath *dotPath = [UIBezierPath bezierPathWithOvalInRect:dotRect];
        [(current ? accentColor : accentAlt) setFill];
        [dotPath fill];

        if ([role isEqualToString:@"Workspace"]) {
            NSArray<NSValue *> *cards = @[
                [NSValue valueWithCGRect:CGRectMake(CGRectGetMinX(mainFrame) + 6, CGRectGetMinY(mainFrame) + 18, 18, 11)],
                [NSValue valueWithCGRect:CGRectMake(CGRectGetMinX(mainFrame) + 9, CGRectGetMinY(mainFrame) + 33, 23, 14)],
                [NSValue valueWithCGRect:CGRectMake(CGRectGetMinX(mainFrame) + 28, CGRectGetMinY(mainFrame) + 22, 20, 17)],
            ];
            for (NSUInteger index = 0; index < cards.count; index++) {
                UIColor *fill = index == 1
                    ? [accentColor colorWithAlphaComponent:0.24]
                    : [accentAlt colorWithAlphaComponent:0.18];
                UIBezierPath *path = [UIBezierPath bezierPathWithRoundedRect:cards[index].CGRectValue cornerRadius:4];
                [fill setFill];
                [path fill];
            }
        } else {
            CGRect terminalRect = CGRectMake(CGRectGetMinX(mainFrame) + 6,
                                             CGRectGetMinY(mainFrame) + 18,
                                             CGRectGetWidth(mainFrame) - 12,
                                             CGRectGetHeight(mainFrame) - 24);
            UIBezierPath *terminalPath = [UIBezierPath bezierPathWithRoundedRect:terminalRect cornerRadius:5];
            [[UIColor colorWithWhite:0.08 alpha:0.9] setFill];
            [terminalPath fill];
            NSDictionary<NSAttributedStringKey, id> *attributes = @{
                NSFontAttributeName: [UIFont monospacedSystemFontOfSize:5.5 weight:UIFontWeightMedium],
                NSForegroundColorAttributeName: [accentColor colorWithAlphaComponent:0.82],
            };
            [@"$" drawAtPoint:CGPointMake(CGRectGetMinX(terminalRect) + 4, CGRectGetMinY(terminalRect) + 4) withAttributes:attributes];
            [@">" drawAtPoint:CGPointMake(CGRectGetMinX(terminalRect) + 12, CGRectGetMinY(terminalRect) + 10) withAttributes:attributes];
            [@"_" drawAtPoint:CGPointMake(CGRectGetMinX(terminalRect) + 8, CGRectGetMaxY(terminalRect) - 10) withAttributes:attributes];
        }
    }];
}

- (UIButton *)workspaceSceneButtonWithDescriptor:(NSDictionary<NSString *, id> *)descriptor {
    NSString *identifier = descriptor[@"identifier"] ?: NSUUID.UUID.UUIDString;
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    button.translatesAutoresizingMaskIntoConstraints = NO;
    button.accessibilityIdentifier = identifier;
    button.accessibilityLabel = descriptor[@"title"];
    button.accessibilityValue = descriptor[@"detail"];
    button.contentHorizontalAlignment = UIControlContentHorizontalAlignmentFill;
    button.contentVerticalAlignment = UIControlContentVerticalAlignmentFill;
    button.layer.cornerRadius = 12;
    button.layer.borderWidth = 1;
    [button.heightAnchor constraintEqualToConstant:(ISHWorkspaceUsesPhoneLayout() ? 54.0 : 62.0)].active = YES;
    [button addTarget:self action:@selector(focusWorkspaceSceneButton:) forControlEvents:UIControlEventTouchUpInside];
    button.accessibilityHint = @"Touch and hold for workspace options.";
    UILongPressGestureRecognizer *longPressRecognizer =
        [[UILongPressGestureRecognizer alloc] initWithTarget:self action:@selector(handleWorkspaceSceneButtonLongPress:)];
    longPressRecognizer.minimumPressDuration = 0.35;
    longPressRecognizer.cancelsTouchesInView = YES;
    [button addGestureRecognizer:longPressRecognizer];

    UIImageView *previewView = [UIImageView new];
    previewView.translatesAutoresizingMaskIntoConstraints = NO;
    previewView.contentMode = UIViewContentModeScaleAspectFill;
    previewView.clipsToBounds = YES;
    previewView.layer.cornerRadius = 9;
    previewView.layer.borderWidth = 1;
    previewView.userInteractionEnabled = NO;
    [button addSubview:previewView];

    [NSLayoutConstraint activateConstraints:@[
        [previewView.topAnchor constraintEqualToAnchor:button.topAnchor constant:6],
        [previewView.leadingAnchor constraintEqualToAnchor:button.leadingAnchor constant:6],
        [previewView.trailingAnchor constraintEqualToAnchor:button.trailingAnchor constant:-6],
        [previewView.bottomAnchor constraintEqualToAnchor:button.bottomAnchor constant:-6],
    ]];

    _previewImageViewsByIdentifier[identifier] = previewView;
    [_trackedButtons addObject:button];
    return button;
}

// Renders the Desktop list: one row per Desktop (active one highlighted, tap to jump, remove
// when there's more than one), then a "New Desktop" button. Refreshed on every Desktop change.
- (void)rebuildSceneButtons {
    NSArray<UIView *> *existingRows = _rowsStack.arrangedSubviews.copy;
    for (UIView *view in existingRows) {
        [_rowsStack removeArrangedSubview:view];
        [view removeFromSuperview];
    }
    [_trackedButtons removeAllObjects];
    [_previewImageViewsByIdentifier removeAllObjects];

    WorkspaceViewController *host = self.workspaceHostViewController;
    NSInteger count = MAX((NSInteger)1, host.desktopCount);
    NSInteger active = host.activeDesktopIndex;
    for (NSInteger i = 0; i < count; i++) {
        [_rowsStack addArrangedSubview:[self desktopRowForIndex:i active:(i == active) removable:(count > 1)]];
    }
    [_rowsStack addArrangedSubview:[self workspacesActionButtonWithTitle:@"New Desktop" action:@selector(addDesktopFromApplet:)]];

    // Size the window to the Desktop count. Deferred so it runs after the host's initial
    // placement, which would otherwise restore a stale frame.
    __weak typeof(self) weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        [(id)weakSelf.workspaceHostViewController autosizeWorkspacesWindow];
    });
}

- (UIView *)desktopRowForIndex:(NSInteger)index active:(BOOL)active removable:(BOOL)removable {
    NSDictionary<NSString *, UIColor *> *theme = self.workspaceTheme;
    UIColor *accent = theme[@"accent"] ?: UIColor.systemBlueColor;
    UIColor *muted = [(theme[@"primary"] ?: UIColor.grayColor) colorWithAlphaComponent:0.5];
    BOOL locked = [self.workspaceHostViewController isDesktopLockedAtIndex:index];

    UIStackView *row = [UIStackView new];
    row.axis = UILayoutConstraintAxisHorizontal;
    row.spacing = 6;
    row.alignment = UIStackViewAlignmentFill;

    // Lock / unlock toggle, left of the card. A locked Desktop can't be removed.
    UIButton *lock = [UIButton buttonWithType:UIButtonTypeSystem];
    lock.translatesAutoresizingMaskIntoConstraints = NO;
    lock.tag = index;
    if (@available(iOS 13.0, *)) {
        UIImageSymbolConfiguration *config =
            [UIImageSymbolConfiguration configurationWithPointSize:13.0 weight:UIImageSymbolWeightSemibold];
        [lock setImage:[UIImage systemImageNamed:(locked ? @"lock.fill" : @"lock.open")
                               withConfiguration:config]
              forState:UIControlStateNormal];
    } else {
        [lock setTitle:(locked ? @"🔒" : @"🔓") forState:UIControlStateNormal];
    }
    lock.tintColor = locked ? accent : muted;
    // The first Desktop is permanently protected, so its lock shows closed and isn't toggleable.
    lock.enabled = (index != 0);
    lock.accessibilityLabel = (index == 0)
        ? @"Desktop 1 is protected"
        : [NSString stringWithFormat:@"%@ Desktop %ld", locked ? @"Unlock" : @"Lock", (long)(index + 1)];
    [lock setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
    [lock.widthAnchor constraintEqualToConstant:28.0].active = YES;
    [lock addTarget:self action:@selector(toggleLockFromApplet:) forControlEvents:UIControlEventTouchUpInside];
    [row addArrangedSubview:lock];

    UIButton *jump = [UIButton buttonWithType:UIButtonTypeSystem];
    jump.translatesAutoresizingMaskIntoConstraints = NO;
    jump.tag = index;
    jump.contentHorizontalAlignment = UIControlContentHorizontalAlignmentLeft;
    jump.contentEdgeInsets = UIEdgeInsetsMake(4, 12, 4, 12);
    jump.titleLabel.font = [UIFont systemFontOfSize:ISHWorkspaceThemeFontSize(UIFontTextStyleSubheadline)
                                             weight:active ? UIFontWeightSemibold : UIFontWeightRegular];
    jump.layer.cornerRadius = 10;
    jump.layer.borderWidth = 1;
    jump.layer.borderColor = (theme[@"stroke"] ?: [UIColor colorWithWhite:0.5 alpha:0.35]).CGColor;
    jump.backgroundColor = active ? [accent colorWithAlphaComponent:0.22] : nil;
    [jump setTitle:[NSString stringWithFormat:@"Desktop %ld", (long)(index + 1)] forState:UIControlStateNormal];
    [jump setTitleColor:active ? accent : (theme[@"primary"] ?: UIColor.darkTextColor) forState:UIControlStateNormal];
    if (active) {
        jump.accessibilityTraits |= UIAccessibilityTraitSelected;
    } else {
        jump.accessibilityTraits &= ~UIAccessibilityTraitSelected;
    }
    [jump.heightAnchor constraintEqualToConstant:ISHWorkspaceUsesPhoneLayout() ? 30.0 : 38.0].active = YES;
    [jump addTarget:self action:@selector(jumpToDesktopFromApplet:) forControlEvents:UIControlEventTouchUpInside];
    [row addArrangedSubview:jump];

    // The delete x is hidden entirely while the Desktop is locked; unlock to remove it.
    if (removable && !locked) {
        UIButton *remove = [UIButton buttonWithType:UIButtonTypeSystem];
        remove.translatesAutoresizingMaskIntoConstraints = NO;
        remove.tag = index;
        if (@available(iOS 13.0, *)) {
            UIImageSymbolConfiguration *config =
                [UIImageSymbolConfiguration configurationWithPointSize:11.0 weight:UIImageSymbolWeightSemibold];
            [remove setImage:[UIImage systemImageNamed:@"xmark" withConfiguration:config]
                    forState:UIControlStateNormal];
        } else {
            [remove setTitle:@"x" forState:UIControlStateNormal];
        }
        remove.tintColor = UIColor.systemRedColor;
        remove.accessibilityLabel = [NSString stringWithFormat:@"Remove Desktop %ld", (long)(index + 1)];
        [remove setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
        [remove.widthAnchor constraintEqualToConstant:28.0].active = YES;
        [remove addTarget:self action:@selector(removeDesktopFromApplet:) forControlEvents:UIControlEventTouchUpInside];
        [row addArrangedSubview:remove];
    }
    return row;
}

- (void)jumpToDesktopFromApplet:(UIButton *)sender {
    [self.workspaceHostViewController switchToDesktopIndex:sender.tag];
}

- (void)addDesktopFromApplet:(id)sender {
    (void) sender;
    [self.workspaceHostViewController createNewDesktop];
}

- (void)removeDesktopFromApplet:(UIButton *)sender {
    [self.workspaceHostViewController removeDesktopAtIndex:sender.tag];
}

- (void)toggleLockFromApplet:(UIButton *)sender {
    [self.workspaceHostViewController toggleDesktopLockAtIndex:sender.tag];
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Desktops";
    _trackedButtons = [NSMutableArray array];
    _previewImageViewsByIdentifier = [NSMutableDictionary dictionary];

    _scrollView = [UIScrollView new];
    _scrollView.translatesAutoresizingMaskIntoConstraints = NO;
    _scrollView.alwaysBounceVertical = YES;
    [self.toolContentView addSubview:_scrollView];

    _contentStack = [UIStackView new];
    _contentStack.translatesAutoresizingMaskIntoConstraints = NO;
    _contentStack.axis = UILayoutConstraintAxisVertical;
    _contentStack.spacing = 0;
    [_scrollView addSubview:_contentStack];

    UIView *listCard = [self workspaceThemeCardView];
    _rowsStack = [UIStackView new];
    _rowsStack.translatesAutoresizingMaskIntoConstraints = NO;
    _rowsStack.axis = UILayoutConstraintAxisVertical;
    _rowsStack.spacing = 6;
    [listCard addSubview:_rowsStack];

    if (!ISHWorkspaceUsesModernStyle()) {
        _newWorkspaceButton = [self workspacesActionButtonWithTitle:@"New Workspace" action:@selector(openNewWorkspaceFromApplet:)];
        [_contentStack addArrangedSubview:_newWorkspaceButton];
        _closeHiddenButton = [self workspacesActionButtonWithTitle:@"Close Hidden Windows" action:@selector(confirmCloseHiddenWindows:)];
        [_contentStack addArrangedSubview:_closeHiddenButton];
    } else {
        // Modern folds the Layout Manager into this applet as two icons.
        UIStackView *layoutRow = [UIStackView new];
        layoutRow.axis = UILayoutConstraintAxisHorizontal;
        layoutRow.distribution = UIStackViewDistributionFillEqually;
        layoutRow.spacing = 6;
        [layoutRow addArrangedSubview:[self workspacesIconButtonWithSymbol:@"square.and.arrow.down" fallback:@"Save" action:@selector(saveLayoutFromApplet:)]];
        [layoutRow addArrangedSubview:[self workspacesIconButtonWithSymbol:@"arrow.clockwise" fallback:@"Restore" action:@selector(restoreLayoutFromApplet:)]];
        [_contentStack addArrangedSubview:layoutRow];
    }
    CGFloat listInset = ISHWorkspaceUsesPhoneLayout() ? 6.0 : 8.0;
    [NSLayoutConstraint activateConstraints:@[
        [_rowsStack.topAnchor constraintEqualToAnchor:listCard.topAnchor constant:listInset],
        [_rowsStack.leadingAnchor constraintEqualToAnchor:listCard.leadingAnchor constant:listInset],
        [_rowsStack.trailingAnchor constraintEqualToAnchor:listCard.trailingAnchor constant:-listInset],
        [_rowsStack.bottomAnchor constraintEqualToAnchor:listCard.bottomAnchor constant:-listInset],
    ]];

    [_contentStack addArrangedSubview:listCard];
    [NSLayoutConstraint activateConstraints:@[
        [_scrollView.topAnchor constraintEqualToAnchor:self.toolContentView.topAnchor],
        [_scrollView.leadingAnchor constraintEqualToAnchor:self.toolContentView.leadingAnchor],
        [_scrollView.trailingAnchor constraintEqualToAnchor:self.toolContentView.trailingAnchor],
        [_scrollView.bottomAnchor constraintEqualToAnchor:self.toolContentView.bottomAnchor],

        [_contentStack.topAnchor constraintEqualToAnchor:_scrollView.contentLayoutGuide.topAnchor constant:6],
        [_contentStack.leadingAnchor constraintEqualToAnchor:_scrollView.frameLayoutGuide.leadingAnchor constant:6],
        [_contentStack.trailingAnchor constraintEqualToAnchor:_scrollView.frameLayoutGuide.trailingAnchor constant:-6],
        [_contentStack.bottomAnchor constraintEqualToAnchor:_scrollView.contentLayoutGuide.bottomAnchor constant:-6],
        [_contentStack.widthAnchor constraintEqualToAnchor:_scrollView.frameLayoutGuide.widthAnchor constant:-12],
    ]];

    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(refreshWorkspaceScenesNotification:)
                                               name:UIApplicationDidBecomeActiveNotification
                                             object:nil];
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(refreshWorkspaceScenesNotification:)
                                               name:TerminalRegistryDidChangeNotification
                                             object:nil];
    if (@available(iOS 13.0, *)) {
        [NSNotificationCenter.defaultCenter addObserver:self
                                               selector:@selector(refreshWorkspaceScenesNotification:)
                                                   name:UISceneDidActivateNotification
                                                 object:nil];
        [NSNotificationCenter.defaultCenter addObserver:self
                                               selector:@selector(refreshWorkspaceScenesNotification:)
                                                   name:UISceneDidDisconnectNotification
                                                 object:nil];
    }
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(rebuildSceneButtons)
                                               name:ISHWorkspaceDesktopsDidChangeNotification
                                             object:nil];

    [self refreshWorkspaceScenes];
}

- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:self];
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [self refreshWorkspaceScenes];
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    _rowsStack.spacing = ISHWorkspaceDensityValue(4, 6);
}

- (void)refreshWorkspaceScenesNotification:(__unused NSNotification *)notification {
    [self refreshWorkspaceScenes];
}

- (void)refreshWorkspaceScenes {
    if (@available(iOS 13.0, *)) {
        _sceneDescriptors = ISHWorkspaceSceneDescriptors(self.workspaceHostViewController.view.window.windowScene);
    } else {
        _sceneDescriptors = @[];
    }
    [self rebuildSceneButtons];
    [self workspaceApplyTheme];
}

- (void)focusWorkspaceSceneButton:(UIButton *)sender {
    if (@available(iOS 13.0, *)) {
    } else {
        return;
    }
    [self.workspaceHostViewController focusSceneWithPersistentIdentifier:sender.accessibilityIdentifier];
}

- (void)handleWorkspaceSceneButtonLongPress:(UILongPressGestureRecognizer *)recognizer {
    if (recognizer.state != UIGestureRecognizerStateBegan || ![recognizer.view isKindOfClass:UIButton.class])
        return;
    [self presentWorkspaceSceneMenuForButton:(UIButton *) recognizer.view];
}

- (void)presentWorkspaceSceneMenuForButton:(UIButton *)sender {
    if (@available(iOS 13.0, *)) {
    } else {
        return;
    }
    NSDictionary<NSString *, id> *descriptor = [self sceneDescriptorWithIdentifier:sender.accessibilityIdentifier];
    NSString *title = descriptor[@"title"] ?: @"Workspace";
    UIAlertController *sheet = [UIAlertController alertControllerWithTitle:title
                                                                   message:descriptor[@"detail"]
                                                            preferredStyle:UIAlertControllerStyleActionSheet];
    [sheet addAction:[UIAlertAction actionWithTitle:@"Focus Workspace" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
        [self.workspaceHostViewController focusSceneWithPersistentIdentifier:sender.accessibilityIdentifier];
    }]];
    UIAlertAction *closeAction = [UIAlertAction actionWithTitle:@"Close Workspace" style:UIAlertActionStyleDestructive handler:^(__unused UIAlertAction *action) {
            [self confirmCloseWorkspaceWithIdentifier:sender.accessibilityIdentifier title:title];
    }];
    closeAction.enabled = ![title isEqualToString:@"Workspace One"];
    [sheet addAction:closeAction];
    [sheet addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    UIPopoverPresentationController *popover = sheet.popoverPresentationController;
    if (popover != nil) {
        popover.sourceView = sender;
        popover.sourceRect = sender.bounds;
    }
    [self presentViewController:sheet animated:YES completion:nil];
}

- (void)openNewWorkspaceFromApplet:(id)sender {
    (void) sender;
    [self.workspaceHostViewController openNewWorkspaceWindow:nil];
}

- (void)confirmCloseHiddenWindows:(id)sender {
    (void) sender;
    if (@available(iOS 13.0, *)) {
        NSArray<UISceneSession *> *sessions = [self.workspaceHostViewController hiddenWorkspaceSceneSessions];
        if (sessions.count == 0) {
            UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"No Hidden Windows"
                                                                           message:@"There are no hidden Workspace windows to close."
                                                                    preferredStyle:UIAlertControllerStyleAlert];
            [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
            [self presentViewController:alert animated:YES completion:nil];
            return;
        }
        UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Close Hidden Windows?"
                                                                       message:[NSString stringWithFormat:@"This will close %lu hidden Workspace window%@.", (unsigned long) sessions.count, sessions.count == 1 ? @"" : @"s"]
                                                                preferredStyle:UIAlertControllerStyleAlert];
        [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
        [alert addAction:[UIAlertAction actionWithTitle:@"Close Hidden" style:UIAlertActionStyleDestructive handler:^(__unused UIAlertAction *action) {
            [self.workspaceHostViewController closeHiddenWorkspaceWindows:nil];
            [self refreshWorkspaceScenes];
        }]];
        [self presentViewController:alert animated:YES completion:nil];
        return;
    }
}

- (void)confirmCloseWorkspaceWithIdentifier:(NSString *)identifier title:(NSString *)title {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:[NSString stringWithFormat:@"Close %@?", title ?: @"Workspace"]
                                                                   message:@"This closes that Workspace window. Running terminals in that Workspace may be detached or closed with the scene."
                                                            preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Close Workspace" style:UIAlertActionStyleDestructive handler:^(__unused UIAlertAction *action) {
        [self.workspaceHostViewController closeSceneWithPersistentIdentifier:identifier title:@"Unable to close workspace"];
        [self refreshWorkspaceScenes];
    }]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)workspaceApplyTheme {
    [super workspaceApplyTheme];
    NSDictionary<NSString *, UIColor *> *theme = self.workspaceTheme;
    _newWorkspaceButton.backgroundColor = [theme[@"cardAlt"] colorWithAlphaComponent:0.94];
    _newWorkspaceButton.layer.borderColor = theme[@"stroke"].CGColor;
    [_newWorkspaceButton setTitleColor:theme[@"primary"] forState:UIControlStateNormal];
    _closeHiddenButton.backgroundColor = [theme[@"cardAlt"] colorWithAlphaComponent:0.94];
    _closeHiddenButton.layer.borderColor = theme[@"stroke"].CGColor;
    [_closeHiddenButton setTitleColor:theme[@"primary"] forState:UIControlStateNormal];
    for (NSDictionary<NSString *, id> *descriptor in _sceneDescriptors) {
        NSString *identifier = descriptor[@"identifier"];
        UIButton *button = nil;
        for (UIButton *candidate in _trackedButtons) {
            if ([candidate.accessibilityIdentifier isEqualToString:identifier]) {
                button = candidate;
                break;
            }
        }
        if (button == nil)
            continue;
        BOOL current = [descriptor[@"isCurrent"] boolValue];
        if (current) {
            button.accessibilityTraits |= UIAccessibilityTraitSelected;
        } else {
            button.accessibilityTraits &= ~UIAccessibilityTraitSelected;
        }
        button.backgroundColor = current
            ? [theme[@"accent"] colorWithAlphaComponent:0.16]
            : [theme[@"cardAlt"] colorWithAlphaComponent:0.94];
        button.layer.borderColor = (current ? theme[@"accentAlt"] : theme[@"stroke"]).CGColor;
        UIImageView *previewView = _previewImageViewsByIdentifier[identifier];
        CGSize previewSize = previewView.bounds.size;
        if (previewSize.width < 24 || previewSize.height < 24)
            previewSize = ISHWorkspaceUsesPhoneLayout() ? CGSizeMake(92, 42) : CGSizeMake(106, 50);
        previewView.image =
            [self scenePreviewImageForDescriptor:descriptor size:previewSize];
        previewView.layer.borderColor =
            (current ? theme[@"accentAlt"] : theme[@"stroke"]).CGColor;
    }
}

@end

@implementation WorkspaceBrowserToolViewController {
    UIView *_toolbarCard;
    UIView *_browserCard;
    UIView *_addressContainerView;
    UIScrollView *_tabsScrollView;
    UIStackView *_tabsStack;
    UITextField *_addressField;
    UIButton *_backButton;
    UIButton *_forwardButton;
    UIButton *_reloadButton;
    UIButton *_homeButton;
    UIButton *_bookmarkButton;
    UIButton *_goButton;
    UIButton *_addTabButton;
    UIButton *_closeTabButton;
    NSMutableArray<UIButton *> *_actionButtons;
    NSMutableArray<UIButton *> *_tabButtons;
    UIProgressView *_progressView;
    NSMutableArray<WKWebView *> *_tabWebViews;
    WKWebView *_webView;
    NSInteger _selectedTabIndex;
    NSUInteger _maximumTabCount;
}

- (UIButton *)browserButtonWithTitle:(NSString *)title action:(SEL)action {
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    button.translatesAutoresizingMaskIntoConstraints = NO;
    button.layer.cornerRadius = 10.0;
    button.layer.borderWidth = 1.0;
    button.titleLabel.font = [UIFont systemFontOfSize:ISHWorkspaceThemeFontSize(UIFontTextStyleFootnote)
                                               weight:UIFontWeightSemibold];
    [button setTitle:title forState:UIControlStateNormal];
    [button addTarget:self action:action forControlEvents:UIControlEventTouchUpInside];
    CGFloat width = ISHWorkspaceUsesPhoneLayout() ? 30.0 : 34.0;
    if ([title isEqualToString:@"Go"]) {
        width = ISHWorkspaceUsesPhoneLayout() ? 42.0 : 48.0;
    } else if ([title isEqualToString:@"Home"]) {
        width = ISHWorkspaceUsesPhoneLayout() ? 52.0 : 60.0;
    } else if ([title isEqualToString:@"Tabs"]) {
        width = ISHWorkspaceUsesPhoneLayout() ? 44.0 : 50.0;
    }
    [NSLayoutConstraint activateConstraints:@[
        [button.widthAnchor constraintEqualToConstant:width],
        [button.heightAnchor constraintEqualToConstant:ISHWorkspaceUsesPhoneLayout() ? 30.0 : 34.0],
    ]];
    return button;
}

- (UIButton *)browserTabButtonWithIndex:(NSInteger)index {
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    button.translatesAutoresizingMaskIntoConstraints = NO;
    button.tag = index;
    button.layer.cornerRadius = 10.0;
    button.layer.borderWidth = 1.0;
    button.titleLabel.font = [UIFont systemFontOfSize:ISHWorkspaceThemeFontSize(UIFontTextStyleFootnote)
                                               weight:UIFontWeightSemibold];
    button.titleLabel.adjustsFontSizeToFitWidth = YES;
    button.titleLabel.minimumScaleFactor = 0.6;
    [button addTarget:self action:@selector(selectTabFromButton:) forControlEvents:UIControlEventTouchUpInside];
    // Tap selects the tab; touch-and-hold offers pin/unpin — the same tap-vs-hold split used by
    // Home, Reload, and Bookmark elsewhere in this toolbar.
    UILongPressGestureRecognizer *pinLongPressRecognizer =
        [[UILongPressGestureRecognizer alloc] initWithTarget:self action:@selector(handleTabButtonLongPress:)];
    pinLongPressRecognizer.minimumPressDuration = 0.35;
    [button addGestureRecognizer:pinLongPressRecognizer];
    [NSLayoutConstraint activateConstraints:@[
        [button.widthAnchor constraintEqualToConstant:ISHWorkspaceUsesPhoneLayout() ? 30.0 : 34.0],
        [button.heightAnchor constraintEqualToConstant:ISHWorkspaceUsesPhoneLayout() ? 26.0 : 30.0],
    ]];
    [self updateTitleAndAccessibilityForTabButton:button atIndex:index];
    return button;
}

- (void)updateTitleAndAccessibilityForTabButton:(UIButton *)button atIndex:(NSInteger)index {
    BOOL pinned = ISHWorkspaceBrowserTabIndexIsPinned(index);
    [button setTitle:[NSString stringWithFormat:pinned ? @"%ld📌" : @"%ld", (long) (index + 1)]
             forState:UIControlStateNormal];
    if (pinned) {
        NSString *pinnedTitle = ISHWorkspaceBrowserPinnedTitleForTabIndex(index);
        button.accessibilityLabel = [NSString stringWithFormat:@"Tab %ld, pinned to %@", (long) (index + 1), pinnedTitle];
        button.accessibilityHint = @"Touch and hold to unpin.";
    } else {
        button.accessibilityLabel = [NSString stringWithFormat:@"Tab %ld", (long) (index + 1)];
        button.accessibilityHint = @"Touch and hold to pin this tab's current page.";
    }
}

- (void)refreshTabButtonPinIndicators {
    for (UIButton *button in _tabButtons) {
        [self updateTitleAndAccessibilityForTabButton:button atIndex:button.tag];
    }
}

- (WKWebView *)buildBrowserWebView {
    WKWebViewConfiguration *configuration = [WKWebViewConfiguration new];
    WKWebView *webView = [[WKWebView alloc] initWithFrame:CGRectZero configuration:configuration];
    webView.translatesAutoresizingMaskIntoConstraints = NO;
    webView.navigationDelegate = self;
    webView.UIDelegate = self;
    webView.allowsBackForwardNavigationGestures = YES;
    webView.layer.cornerRadius = ISHWorkspaceUsesPhoneLayout() ? 12.0 : 16.0;
    webView.layer.masksToBounds = YES;
    webView.scrollView.keyboardDismissMode = UIScrollViewKeyboardDismissModeInteractive;
    [webView addObserver:self forKeyPath:@"estimatedProgress" options:NSKeyValueObservingOptionNew context:nil];
    return webView;
}

- (WKWebView *)currentBrowserWebView {
    if (_selectedTabIndex < 0 || _selectedTabIndex >= (NSInteger) _tabWebViews.count)
        return nil;
    return _tabWebViews[_selectedTabIndex];
}

- (void)loadAddressString:(NSString *)addressString inWebView:(WKWebView *)webView {
    NSURL *URL = ISHWorkspaceBrowserURLFromInput(addressString);
    if (URL == nil || webView == nil)
        return;
    [webView loadRequest:[NSURLRequest requestWithURL:URL]];
}

- (void)attachCurrentBrowserWebView {
    WKWebView *currentWebView = [self currentBrowserWebView];
    if (currentWebView == nil)
        return;
    if (_webView != nil && _webView != currentWebView) {
        [_webView removeFromSuperview];
    }
    _webView = currentWebView;
    if (_webView.superview != _browserCard) {
        [_webView removeFromSuperview];
        [_browserCard addSubview:_webView];
        CGFloat inset = ISHWorkspaceUsesPhoneLayout() ? 6.0 : 8.0;
        [NSLayoutConstraint activateConstraints:@[
            [_webView.topAnchor constraintEqualToAnchor:_browserCard.topAnchor constant:inset],
            [_webView.leadingAnchor constraintEqualToAnchor:_browserCard.leadingAnchor constant:inset],
            [_webView.trailingAnchor constraintEqualToAnchor:_browserCard.trailingAnchor constant:-inset],
            [_webView.bottomAnchor constraintEqualToAnchor:_browserCard.bottomAnchor constant:-inset],
        ]];
    }
}

- (void)rebuildTabButtons {
    NSArray<UIView *> *arrangedSubviews = _tabsStack.arrangedSubviews.copy;
    for (UIView *view in arrangedSubviews) {
        [_tabsStack removeArrangedSubview:view];
        [view removeFromSuperview];
    }
    [_tabButtons removeAllObjects];

    for (NSInteger index = 0; index < (NSInteger) _tabWebViews.count; index++) {
        UIButton *button = [self browserTabButtonWithIndex:index];
        [_tabsStack addArrangedSubview:button];
        [_tabButtons addObject:button];
    }
}

- (void)selectBrowserTabAtIndex:(NSInteger)index {
    if (index < 0 || index >= (NSInteger) _tabWebViews.count)
        return;
    _selectedTabIndex = index;
    [self attachCurrentBrowserWebView];
    [self refreshBrowserChrome];
}

- (void)addBrowserTabLoadingAddress:(NSString *)addressString activate:(BOOL)activate {
    if (_tabWebViews.count >= _maximumTabCount)
        return;
    WKWebView *webView = [self buildBrowserWebView];
    [_tabWebViews addObject:webView];
    [self rebuildTabButtons];
    NSInteger newIndex = _tabWebViews.count - 1;
    NSString *pinnedURL = ISHWorkspaceBrowserPinnedURLForTabIndex(newIndex);
    [self loadAddressString:(pinnedURL.length > 0 ? pinnedURL : addressString) inWebView:webView];
    if (activate || _tabWebViews.count == 1) {
        [self selectBrowserTabAtIndex:newIndex];
    } else {
        [self refreshBrowserChrome];
    }
}

- (void)presentTabLimitAlert {
    _maximumTabCount = ISHWorkspaceBrowserMaximumTabCount();
    NSString *message;
    if (ISHWorkspaceDeviceHasLowMemoryForWorkspace()) {
        message = [NSString stringWithFormat:@"This device is limited to %lu browser tabs in Workspace because it has less than 4 GB of RAM.",
                   (unsigned long) _maximumTabCount];
    } else {
        CGFloat ramGB = (CGFloat) ISHWorkspacePhysicalMemoryBytes() / (CGFloat) ISHWorkspaceOneGB;
        message = [NSString stringWithFormat:@"This device is limited to %lu browser tabs in Workspace based on its available RAM (%.0f GB).",
                   (unsigned long) _maximumTabCount,
                   floor(ramGB)];
    }
    UIAlertController *alert =
        [UIAlertController alertControllerWithTitle:@"Tab Limit Reached"
                                            message:message
                                     preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Browser";
    _selectedTabIndex = NSNotFound;
    _maximumTabCount = ISHWorkspaceBrowserMaximumTabCount();
    _actionButtons = [NSMutableArray array];
    _tabButtons = [NSMutableArray array];
    _tabWebViews = [NSMutableArray array];

    _toolbarCard = [self workspaceThemeCardView];
    _browserCard = [self workspaceThemeCardView];
    [self.toolContentView addSubview:_toolbarCard];
    [self.toolContentView addSubview:_browserCard];

    UIView *controlsRow = [UIView new];
    controlsRow.translatesAutoresizingMaskIntoConstraints = NO;
    [_toolbarCard addSubview:controlsRow];

    _backButton = [self browserButtonWithTitle:@"<" action:@selector(goBack:)];
    _backButton.accessibilityLabel = @"Back";
    _forwardButton = [self browserButtonWithTitle:@">" action:@selector(goForward:)];
    _forwardButton.accessibilityLabel = @"Forward";
    _reloadButton = [self browserButtonWithTitle:@"R" action:@selector(reloadOrStop:)];
    _reloadButton.accessibilityLabel = @"Reload";
    _reloadButton.accessibilityHint = @"Touch and hold to open the current page in Safari.";
    _homeButton = [self browserButtonWithTitle:@"Home" action:@selector(goHome:)];
    _homeButton.accessibilityLabel = @"Home";
    _homeButton.accessibilityHint = @"Touch and hold to set the home page.";
    _bookmarkButton = [self browserButtonWithTitle:@"☆" action:@selector(toggleBookmark:)];
    _bookmarkButton.accessibilityLabel = @"Bookmark";
    _bookmarkButton.accessibilityHint = @"Toggles a bookmark for the current page. Touch and hold to view bookmarks.";
    _goButton = [self browserButtonWithTitle:@"Go" action:@selector(commitAddress:)];
    _goButton.accessibilityLabel = @"Go";
    _addTabButton = [self browserButtonWithTitle:@"+" action:@selector(addTab:)];
    _addTabButton.accessibilityLabel = @"Add Tab";
    _closeTabButton = [self browserButtonWithTitle:@"×" action:@selector(closeCurrentTab:)];
    _closeTabButton.accessibilityLabel = @"Close Tab";
    UILongPressGestureRecognizer *homeLongPressRecognizer =
        [[UILongPressGestureRecognizer alloc] initWithTarget:self action:@selector(handleHomeButtonLongPress:)];
    homeLongPressRecognizer.minimumPressDuration = 0.35;
    [_homeButton addGestureRecognizer:homeLongPressRecognizer];
    // Some sites (e.g. Gmail/Google sign-in) deliberately reject embedded WKWebViews regardless
    // of actual capability — the fix is handing the page to real Safari, not spoofing anything.
    UILongPressGestureRecognizer *reloadLongPressRecognizer =
        [[UILongPressGestureRecognizer alloc] initWithTarget:self action:@selector(handleReloadButtonLongPress:)];
    reloadLongPressRecognizer.minimumPressDuration = 0.35;
    [_reloadButton addGestureRecognizer:reloadLongPressRecognizer];
    UILongPressGestureRecognizer *bookmarkLongPressRecognizer =
        [[UILongPressGestureRecognizer alloc] initWithTarget:self action:@selector(handleBookmarkButtonLongPress:)];
    bookmarkLongPressRecognizer.minimumPressDuration = 0.35;
    [_bookmarkButton addGestureRecognizer:bookmarkLongPressRecognizer];
    [_actionButtons addObjectsFromArray:@[_backButton, _forwardButton, _reloadButton, _homeButton, _bookmarkButton, _goButton, _addTabButton, _closeTabButton]];
    for (UIButton *button in _actionButtons) {
        if (button == _homeButton || button == _addTabButton || button == _closeTabButton)
            continue;
        [controlsRow addSubview:button];
    }

    _addressContainerView = [UIView new];
    _addressContainerView.translatesAutoresizingMaskIntoConstraints = NO;
    _addressContainerView.layer.cornerRadius = 10.0;
    _addressContainerView.layer.borderWidth = 1.0;
    [controlsRow addSubview:_addressContainerView];

    _addressField = [UITextField new];
    _addressField.translatesAutoresizingMaskIntoConstraints = NO;
    _addressField.accessibilityLabel = @"Address bar";
    _addressField.delegate = self;
    _addressField.clearButtonMode = UITextFieldViewModeWhileEditing;
    _addressField.returnKeyType = UIReturnKeyGo;
    _addressField.autocapitalizationType = UITextAutocapitalizationTypeNone;
    _addressField.autocorrectionType = UITextAutocorrectionTypeNo;
    _addressField.spellCheckingType = UITextSpellCheckingTypeNo;
    if (@available(iOS 11.0, *)) {
        _addressField.smartDashesType = UITextSmartDashesTypeNo;
        _addressField.smartQuotesType = UITextSmartQuotesTypeNo;
        _addressField.smartInsertDeleteType = UITextSmartInsertDeleteTypeNo;
    }
    [_addressContainerView addSubview:_addressField];

    _progressView = [self workspaceThemeProgressView];
    [_toolbarCard addSubview:_progressView];

    UIView *tabsRow = [UIView new];
    tabsRow.translatesAutoresizingMaskIntoConstraints = NO;
    [_toolbarCard addSubview:tabsRow];

    _tabsScrollView = [UIScrollView new];
    _tabsScrollView.translatesAutoresizingMaskIntoConstraints = NO;
    _tabsScrollView.showsHorizontalScrollIndicator = NO;
    [tabsRow addSubview:_tabsScrollView];

    _tabsStack = [UIStackView new];
    _tabsStack.translatesAutoresizingMaskIntoConstraints = NO;
    _tabsStack.axis = UILayoutConstraintAxisHorizontal;
    _tabsStack.alignment = UIStackViewAlignmentCenter;
    _tabsStack.spacing = 6.0;
    [_tabsScrollView addSubview:_tabsStack];
    [tabsRow addSubview:_homeButton];
    [tabsRow addSubview:_addTabButton];
    [tabsRow addSubview:_closeTabButton];

    CGFloat inset = ISHWorkspaceUsesPhoneLayout() ? 6.0 : 8.0;
    [NSLayoutConstraint activateConstraints:@[
        [_toolbarCard.topAnchor constraintEqualToAnchor:self.toolContentView.topAnchor constant:inset],
        [_toolbarCard.leadingAnchor constraintEqualToAnchor:self.toolContentView.leadingAnchor constant:inset],
        [_toolbarCard.trailingAnchor constraintEqualToAnchor:self.toolContentView.trailingAnchor constant:-inset],

        [_browserCard.topAnchor constraintEqualToAnchor:_toolbarCard.bottomAnchor constant:inset],
        [_browserCard.leadingAnchor constraintEqualToAnchor:self.toolContentView.leadingAnchor constant:inset],
        [_browserCard.trailingAnchor constraintEqualToAnchor:self.toolContentView.trailingAnchor constant:-inset],
        [_browserCard.bottomAnchor constraintEqualToAnchor:self.toolContentView.bottomAnchor constant:-inset],

        [controlsRow.topAnchor constraintEqualToAnchor:_toolbarCard.topAnchor constant:inset],
        [controlsRow.leadingAnchor constraintEqualToAnchor:_toolbarCard.leadingAnchor constant:inset],
        [controlsRow.trailingAnchor constraintEqualToAnchor:_toolbarCard.trailingAnchor constant:-inset],

        [_backButton.leadingAnchor constraintEqualToAnchor:controlsRow.leadingAnchor],
        [_backButton.centerYAnchor constraintEqualToAnchor:_addressContainerView.centerYAnchor],
        [_forwardButton.leadingAnchor constraintEqualToAnchor:_backButton.trailingAnchor constant:6.0],
        [_forwardButton.centerYAnchor constraintEqualToAnchor:_addressContainerView.centerYAnchor],
        [_reloadButton.leadingAnchor constraintEqualToAnchor:_forwardButton.trailingAnchor constant:6.0],
        [_reloadButton.centerYAnchor constraintEqualToAnchor:_addressContainerView.centerYAnchor],
        [_bookmarkButton.leadingAnchor constraintEqualToAnchor:_reloadButton.trailingAnchor constant:6.0],
        [_bookmarkButton.centerYAnchor constraintEqualToAnchor:_addressContainerView.centerYAnchor],

        [_goButton.trailingAnchor constraintEqualToAnchor:controlsRow.trailingAnchor],
        [_goButton.centerYAnchor constraintEqualToAnchor:_addressContainerView.centerYAnchor],

        [_addressContainerView.leadingAnchor constraintEqualToAnchor:_bookmarkButton.trailingAnchor constant:6.0],
        [_addressContainerView.trailingAnchor constraintEqualToAnchor:_goButton.leadingAnchor constant:-6.0],
        [_addressContainerView.topAnchor constraintEqualToAnchor:controlsRow.topAnchor],
        [_addressContainerView.bottomAnchor constraintEqualToAnchor:controlsRow.bottomAnchor],
        [_addressContainerView.heightAnchor constraintEqualToConstant:ISHWorkspaceUsesPhoneLayout() ? 30.0 : 34.0],

        [_addressField.topAnchor constraintEqualToAnchor:_addressContainerView.topAnchor],
        [_addressField.leadingAnchor constraintEqualToAnchor:_addressContainerView.leadingAnchor constant:8.0],
        [_addressField.trailingAnchor constraintEqualToAnchor:_addressContainerView.trailingAnchor constant:-8.0],
        [_addressField.bottomAnchor constraintEqualToAnchor:_addressContainerView.bottomAnchor],

        [_progressView.topAnchor constraintEqualToAnchor:controlsRow.bottomAnchor constant:6.0],
        [_progressView.leadingAnchor constraintEqualToAnchor:_toolbarCard.leadingAnchor constant:inset],
        [_progressView.trailingAnchor constraintEqualToAnchor:_toolbarCard.trailingAnchor constant:-inset],
        [_progressView.heightAnchor constraintEqualToConstant:2.0],

        [tabsRow.topAnchor constraintEqualToAnchor:_progressView.bottomAnchor constant:6.0],
        [tabsRow.leadingAnchor constraintEqualToAnchor:_toolbarCard.leadingAnchor constant:inset],
        [tabsRow.trailingAnchor constraintEqualToAnchor:_toolbarCard.trailingAnchor constant:-inset],
        [tabsRow.bottomAnchor constraintEqualToAnchor:_toolbarCard.bottomAnchor constant:-inset],
        [tabsRow.heightAnchor constraintEqualToConstant:ISHWorkspaceUsesPhoneLayout() ? 30.0 : 34.0],

        [_homeButton.leadingAnchor constraintEqualToAnchor:tabsRow.leadingAnchor],
        [_homeButton.topAnchor constraintEqualToAnchor:tabsRow.topAnchor],
        [_homeButton.bottomAnchor constraintEqualToAnchor:tabsRow.bottomAnchor],

        [_tabsScrollView.leadingAnchor constraintEqualToAnchor:_homeButton.trailingAnchor constant:6.0],
        [_tabsScrollView.topAnchor constraintEqualToAnchor:tabsRow.topAnchor],
        [_tabsScrollView.bottomAnchor constraintEqualToAnchor:tabsRow.bottomAnchor],
        [_tabsScrollView.trailingAnchor constraintEqualToAnchor:_addTabButton.leadingAnchor constant:-6.0],

        [_tabsStack.topAnchor constraintEqualToAnchor:_tabsScrollView.contentLayoutGuide.topAnchor],
        [_tabsStack.leadingAnchor constraintEqualToAnchor:_tabsScrollView.contentLayoutGuide.leadingAnchor],
        [_tabsStack.trailingAnchor constraintEqualToAnchor:_tabsScrollView.contentLayoutGuide.trailingAnchor],
        [_tabsStack.bottomAnchor constraintEqualToAnchor:_tabsScrollView.contentLayoutGuide.bottomAnchor],
        [_tabsStack.heightAnchor constraintEqualToAnchor:_tabsScrollView.frameLayoutGuide.heightAnchor],

        [_addTabButton.topAnchor constraintEqualToAnchor:tabsRow.topAnchor],
        [_addTabButton.bottomAnchor constraintEqualToAnchor:tabsRow.bottomAnchor],
        [_closeTabButton.topAnchor constraintEqualToAnchor:tabsRow.topAnchor],
        [_closeTabButton.bottomAnchor constraintEqualToAnchor:tabsRow.bottomAnchor],
        [_closeTabButton.trailingAnchor constraintEqualToAnchor:tabsRow.trailingAnchor],
        [_addTabButton.trailingAnchor constraintEqualToAnchor:_closeTabButton.leadingAnchor constant:-6.0],
    ]];

    [self addBrowserTabLoadingAddress:ISHWorkspaceBrowserHomeAddress() activate:YES];
}

- (void)dealloc {
    for (WKWebView *webView in _tabWebViews.copy) {
        [webView removeObserver:self forKeyPath:@"estimatedProgress"];
        webView.navigationDelegate = nil;
        webView.UIDelegate = nil;
    }
}

- (void)refreshBrowserChrome {
    _maximumTabCount = ISHWorkspaceBrowserMaximumTabCount();
    WKWebView *currentWebView = [self currentBrowserWebView];
    _backButton.enabled = currentWebView.canGoBack;
    _forwardButton.enabled = currentWebView.canGoForward;
    _backButton.alpha = _backButton.enabled ? 1.0 : 0.42;
    _forwardButton.alpha = _forwardButton.enabled ? 1.0 : 0.42;
    _reloadButton.alpha = 1.0;
    _homeButton.alpha = 1.0;
    [self updateBookmarkButtonState];
    [self refreshTabButtonPinIndicators];
    _goButton.alpha = 1.0;
    _addTabButton.enabled = _tabWebViews.count < _maximumTabCount;
    _addTabButton.alpha = _addTabButton.enabled ? 1.0 : 0.42;
    _closeTabButton.enabled = _tabWebViews.count > 1;
    _closeTabButton.alpha = _closeTabButton.enabled ? 1.0 : 0.42;
    [_reloadButton setTitle:(currentWebView.loading ? @"X" : @"R") forState:UIControlStateNormal];
    NSString *reloadLabel = currentWebView.loading ? @"Stop loading" : @"Reload";
    _reloadButton.accessibilityLabel = reloadLabel;
    _progressView.hidden = !currentWebView.loading && currentWebView.estimatedProgress >= 0.999;
    _progressView.alpha = _progressView.hidden ? 0.0 : 1.0;
    if (!_addressField.isFirstResponder) {
        NSString *address = currentWebView.URL.absoluteString;
        _addressField.text = address.length > 0 ? address : ISHWorkspaceBrowserDefaultAddress();
    }
    [self workspaceApplyTheme];
}

- (void)goBack:(id)sender {
    (void) sender;
    WKWebView *currentWebView = [self currentBrowserWebView];
    if (currentWebView.canGoBack)
        [currentWebView goBack];
}

- (void)goForward:(id)sender {
    (void) sender;
    WKWebView *currentWebView = [self currentBrowserWebView];
    if (currentWebView.canGoForward)
        [currentWebView goForward];
}

- (void)goHome:(id)sender {
    (void) sender;
    [self loadAddressString:ISHWorkspaceBrowserHomeAddress() inWebView:[self currentBrowserWebView]];
}

- (void)handleHomeButtonLongPress:(UILongPressGestureRecognizer *)recognizer {
    if (recognizer.state != UIGestureRecognizerStateBegan)
        return;
    [self presentHomePagePrompt];
}

- (void)presentHomePagePrompt {
    UIAlertController *alert =
        [UIAlertController alertControllerWithTitle:@"Browser Home"
                                            message:@"Set the page opened by the Home button."
                                     preferredStyle:UIAlertControllerStyleAlert];
    [alert addTextFieldWithConfigurationHandler:^(UITextField *textField) {
        textField.text = ISHWorkspaceBrowserHomeAddress();
        textField.placeholder = ISHWorkspaceBrowserDefaultAddress();
        textField.clearButtonMode = UITextFieldViewModeWhileEditing;
        textField.autocapitalizationType = UITextAutocapitalizationTypeNone;
        textField.autocorrectionType = UITextAutocorrectionTypeNo;
        textField.spellCheckingType = UITextSpellCheckingTypeNo;
        if (@available(iOS 11.0, *)) {
            textField.smartDashesType = UITextSmartDashesTypeNo;
            textField.smartQuotesType = UITextSmartQuotesTypeNo;
            textField.smartInsertDeleteType = UITextSmartInsertDeleteTypeNo;
        }
    }];
    __weak typeof(self) weakSelf = self;
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Use Default" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
        ISHWorkspaceSetBrowserHomeAddress(nil);
        [weakSelf goHome:nil];
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Save" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
        UITextField *textField = alert.textFields.firstObject;
        ISHWorkspaceSetBrowserHomeAddress(textField.text);
        [weakSelf goHome:nil];
    }]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)handleReloadButtonLongPress:(UILongPressGestureRecognizer *)recognizer {
    if (recognizer.state != UIGestureRecognizerStateBegan)
        return;
    [self presentOpenInSafariPromptFromView:_reloadButton];
}

- (void)presentOpenInSafariPromptFromView:(UIView *)sourceView {
    NSURL *url = [self currentBrowserWebView].URL;
    if (url == nil)
        return;
    UIAlertController *sheet =
        [UIAlertController alertControllerWithTitle:nil
                                            message:url.absoluteString
                                     preferredStyle:UIAlertControllerStyleActionSheet];
    [sheet addAction:[UIAlertAction actionWithTitle:@"Open in Safari"
                                              style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction *action) {
        [UIApplication openURL:url.absoluteString];
    }]];
    [sheet addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    UIPopoverPresentationController *popover = sheet.popoverPresentationController;
    if (popover != nil) {
        popover.sourceView = sourceView;
        popover.sourceRect = sourceView.bounds;
        popover.permittedArrowDirections = UIPopoverArrowDirectionAny;
    }
    [self presentViewController:sheet animated:YES completion:nil];
}

// Tap toggles a bookmark for the current page; touch-and-hold browses/manages the list — the
// same tap-vs-hold split as Home (go/configure) and Reload (reload/open-in-Safari) elsewhere in
// this toolbar.
- (void)toggleBookmark:(id)sender {
    (void) sender;
    NSString *url = [self currentBrowserWebView].URL.absoluteString;
    if (url.length == 0)
        return;
    if (ISHWorkspaceBrowserIsBookmarked(url)) {
        ISHWorkspaceRemoveBrowserBookmarkForURL(url);
    } else {
        ISHWorkspaceAddBrowserBookmark([self currentBrowserWebView].title, url);
    }
    [self updateBookmarkButtonState];
}

- (void)updateBookmarkButtonState {
    NSString *url = [self currentBrowserWebView].URL.absoluteString;
    BOOL isBookmarked = ISHWorkspaceBrowserIsBookmarked(url);
    [_bookmarkButton setTitle:isBookmarked ? @"★" : @"☆" forState:UIControlStateNormal];
    if (isBookmarked) {
        _bookmarkButton.accessibilityTraits |= UIAccessibilityTraitSelected;
        _bookmarkButton.accessibilityValue = @"Bookmarked";
    } else {
        _bookmarkButton.accessibilityTraits &= ~UIAccessibilityTraitSelected;
        _bookmarkButton.accessibilityValue = @"Not bookmarked";
    }
}

- (void)handleBookmarkButtonLongPress:(UILongPressGestureRecognizer *)recognizer {
    if (recognizer.state != UIGestureRecognizerStateBegan)
        return;
    [self presentBookmarksListFromView:_bookmarkButton editing:NO];
}

- (void)presentBookmarksListFromView:(UIView *)sourceView editing:(BOOL)editing {
    NSArray<NSDictionary<NSString *, NSString *> *> *bookmarks = ISHWorkspaceBrowserBookmarkRecords();
    NSString *message = bookmarks.count == 0
        ? @"No bookmarks yet. Tap the star to bookmark the current page."
        : (editing ? @"Tap a bookmark to remove it." : nil);
    UIAlertController *sheet =
        [UIAlertController alertControllerWithTitle:@"Bookmarks"
                                            message:message
                                     preferredStyle:UIAlertControllerStyleActionSheet];
    __weak typeof(self) weakSelf = self;
    for (NSDictionary<NSString *, NSString *> *bookmark in bookmarks) {
        NSString *title = bookmark[@"title"];
        NSString *url = bookmark[@"url"];
        [sheet addAction:[UIAlertAction actionWithTitle:title.length > 0 ? title : url
                                                  style:editing ? UIAlertActionStyleDestructive : UIAlertActionStyleDefault
                                                handler:^(__unused UIAlertAction *action) {
            if (editing) {
                ISHWorkspaceRemoveBrowserBookmarkForURL(url);
                [weakSelf updateBookmarkButtonState];
                // Re-presenting from inside this handler races the sheet's own dismissal (same
                // reason presentDesktopRootMenuFromView:sourceRect:'s nested items defer) — drop
                // to the next runloop turn so it doesn't silently no-op.
                dispatch_async(dispatch_get_main_queue(), ^{
                    [weakSelf presentBookmarksListFromView:sourceView editing:YES];
                });
            } else {
                [weakSelf loadAddressString:url inWebView:[weakSelf currentBrowserWebView]];
            }
        }]];
    }
    if (bookmarks.count > 0) {
        [sheet addAction:[UIAlertAction actionWithTitle:editing ? @"Done" : @"Edit Bookmarks…"
                                                  style:UIAlertActionStyleDefault
                                                handler:^(__unused UIAlertAction *action) {
            dispatch_async(dispatch_get_main_queue(), ^{
                [weakSelf presentBookmarksListFromView:sourceView editing:!editing];
            });
        }]];
    }
    [sheet addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    UIPopoverPresentationController *popover = sheet.popoverPresentationController;
    if (popover != nil) {
        popover.sourceView = sourceView;
        popover.sourceRect = sourceView.bounds;
        popover.permittedArrowDirections = UIPopoverArrowDirectionAny;
    }
    [self presentViewController:sheet animated:YES completion:nil];
}

- (void)handleTabButtonLongPress:(UILongPressGestureRecognizer *)recognizer {
    if (recognizer.state != UIGestureRecognizerStateBegan)
        return;
    UIButton *button = (UIButton *) recognizer.view;
    [self presentPinOptionsForTabAtIndex:button.tag fromView:button];
}

- (void)presentPinOptionsForTabAtIndex:(NSInteger)index fromView:(UIView *)sourceView {
    if (index < 0 || index >= (NSInteger) _tabWebViews.count)
        return;
    NSString *currentURL = _tabWebViews[index].URL.absoluteString;
    NSString *pinnedURL = ISHWorkspaceBrowserPinnedURLForTabIndex(index);
    NSString *title = [NSString stringWithFormat:@"Tab %ld", (long) (index + 1)];
    NSString *message = pinnedURL.length > 0
        ? [NSString stringWithFormat:@"Pinned to %@. New tabs opened in this slot load that page instead of Home.", pinnedURL]
        : @"Pin this tab so new tabs opened in this slot load its current page instead of Home.";
    UIAlertController *sheet =
        [UIAlertController alertControllerWithTitle:title
                                            message:message
                                     preferredStyle:UIAlertControllerStyleActionSheet];
    __weak typeof(self) weakSelf = self;
    if (pinnedURL.length > 0) {
        [sheet addAction:[UIAlertAction actionWithTitle:@"Unpin Tab"
                                                  style:UIAlertActionStyleDestructive
                                                handler:^(__unused UIAlertAction *action) {
            ISHWorkspaceRemoveBrowserPinnedTabForIndex(index);
            [weakSelf refreshTabButtonPinIndicators];
        }]];
    } else if (currentURL.length > 0) {
        NSString *currentTitle = _tabWebViews[index].title;
        [sheet addAction:[UIAlertAction actionWithTitle:@"Pin Tab to Current Page"
                                                  style:UIAlertActionStyleDefault
                                                handler:^(__unused UIAlertAction *action) {
            ISHWorkspaceSetBrowserPinnedTabForIndex(index, currentTitle, currentURL);
            [weakSelf refreshTabButtonPinIndicators];
        }]];
    }
    [sheet addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    UIPopoverPresentationController *popover = sheet.popoverPresentationController;
    if (popover != nil) {
        popover.sourceView = sourceView;
        popover.sourceRect = sourceView.bounds;
        popover.permittedArrowDirections = UIPopoverArrowDirectionAny;
    }
    [self presentViewController:sheet animated:YES completion:nil];
}

- (void)addTab:(id)sender {
    (void) sender;
    _maximumTabCount = ISHWorkspaceBrowserMaximumTabCount();
    if (_tabWebViews.count >= _maximumTabCount) {
        [self presentTabLimitAlert];
        return;
    }
    [self addBrowserTabLoadingAddress:ISHWorkspaceBrowserHomeAddress() activate:YES];
}

- (void)closeCurrentTab:(id)sender {
    (void) sender;
    if (_tabWebViews.count <= 1 || _selectedTabIndex == NSNotFound)
        return;
    WKWebView *webView = [self currentBrowserWebView];
    [webView removeObserver:self forKeyPath:@"estimatedProgress"];
    webView.navigationDelegate = nil;
    webView.UIDelegate = nil;
    [webView removeFromSuperview];
    [_tabWebViews removeObjectAtIndex:_selectedTabIndex];
    NSInteger nextIndex = MIN(_selectedTabIndex, (NSInteger) _tabWebViews.count - 1);
    [self rebuildTabButtons];
    [self selectBrowserTabAtIndex:nextIndex];
}

- (void)selectTabFromButton:(UIButton *)sender {
    [self selectBrowserTabAtIndex:sender.tag];
}

- (void)reloadOrStop:(id)sender {
    (void) sender;
    WKWebView *currentWebView = [self currentBrowserWebView];
    if (currentWebView.loading) {
        [currentWebView stopLoading];
    } else {
        [currentWebView reload];
    }
    [self refreshBrowserChrome];
}

- (void)commitAddress:(id)sender {
    (void) sender;
    [self loadAddressString:_addressField.text inWebView:[self currentBrowserWebView]];
    [_addressField resignFirstResponder];
}

- (BOOL)textFieldShouldReturn:(UITextField *)textField {
    [self loadAddressString:textField.text inWebView:[self currentBrowserWebView]];
    [textField resignFirstResponder];
    return YES;
}

- (void)webView:(WKWebView *)webView didStartProvisionalNavigation:(WKNavigation *)navigation {
    (void) webView;
    (void) navigation;
    if (webView == [self currentBrowserWebView])
        [self refreshBrowserChrome];
}

- (void)webView:(WKWebView *)webView didFinishNavigation:(WKNavigation *)navigation {
    (void) webView;
    (void) navigation;
    if (webView == [self currentBrowserWebView])
        [self refreshBrowserChrome];
}

- (void)webView:(WKWebView *)webView didFailNavigation:(WKNavigation *)navigation withError:(NSError *)error {
    (void) webView;
    (void) navigation;
    if (error.code != NSURLErrorCancelled && webView == [self currentBrowserWebView])
        [self refreshBrowserChrome];
}

- (void)webView:(WKWebView *)webView didFailProvisionalNavigation:(WKNavigation *)navigation withError:(NSError *)error {
    (void) webView;
    (void) navigation;
    if (error.code != NSURLErrorCancelled && webView == [self currentBrowserWebView])
        [self refreshBrowserChrome];
}

- (nullable WKWebView *)webView:(WKWebView *)webView
    createWebViewWithConfiguration:(WKWebViewConfiguration *)configuration
               forNavigationAction:(WKNavigationAction *)navigationAction
                    windowFeatures:(WKWindowFeatures *)windowFeatures {
    (void) webView;
    (void) configuration;
    (void) windowFeatures;
    if (!navigationAction.targetFrame.isMainFrame && navigationAction.request.URL != nil) {
        [[self currentBrowserWebView] loadRequest:navigationAction.request];
    }
    return nil;
}

- (void)observeValueForKeyPath:(NSString *)keyPath
                      ofObject:(id)object
                        change:(NSDictionary<NSKeyValueChangeKey, id> *)change
                       context:(void *)context {
    (void) change;
    (void) context;
    if ([keyPath isEqualToString:@"estimatedProgress"]) {
        if (object == [self currentBrowserWebView]) {
            [_progressView setProgress:(float) [self currentBrowserWebView].estimatedProgress animated:YES];
            [self refreshBrowserChrome];
        }
        return;
    }
    [super observeValueForKeyPath:keyPath ofObject:object change:change context:context];
}

- (void)workspaceApplyTheme {
    [super workspaceApplyTheme];
    NSDictionary<NSString *, UIColor *> *theme = self.workspaceTheme;
    UIColor *buttonBackground = [theme[@"cardAlt"] colorWithAlphaComponent:0.92];
    UIColor *buttonBorder = theme[@"stroke"];
    for (UIButton *button in _actionButtons) {
        button.backgroundColor = buttonBackground;
        button.layer.borderColor = buttonBorder.CGColor;
        [button setTitleColor:(button.enabled ? theme[@"primary"] : theme[@"secondary"]) forState:UIControlStateNormal];
    }
    for (UIButton *button in _tabButtons) {
        BOOL selected = button.tag == _selectedTabIndex;
        if (selected) {
            button.accessibilityTraits |= UIAccessibilityTraitSelected;
        } else {
            button.accessibilityTraits &= ~UIAccessibilityTraitSelected;
        }
        button.backgroundColor = selected
            ? [theme[@"accent"] colorWithAlphaComponent:0.18]
            : [theme[@"cardAlt"] colorWithAlphaComponent:0.92];
        button.layer.borderColor = (selected ? theme[@"accentAlt"] : theme[@"stroke"]).CGColor;
        [button setTitleColor:(selected ? theme[@"accent"] : theme[@"primary"]) forState:UIControlStateNormal];
    }
    _addressContainerView.backgroundColor = [theme[@"backgroundTop"] colorWithAlphaComponent:0.18];
    _addressContainerView.layer.borderColor = theme[@"stroke"].CGColor;
    _tabsScrollView.indicatorStyle = ISHWorkspaceThemeRelativeLuminance(theme[@"card"]) > 0.5
        ? UIScrollViewIndicatorStyleBlack
        : UIScrollViewIndicatorStyleWhite;
    _addressField.textColor = theme[@"primary"];
    _addressField.tintColor = theme[@"accent"];
    _addressField.attributedPlaceholder =
        [[NSAttributedString alloc] initWithString:@"Enter URL or search"
                                        attributes:@{NSForegroundColorAttributeName: theme[@"secondary"]}];
    _progressView.trackTintColor = [theme[@"backgroundTop"] colorWithAlphaComponent:0.24];
    _progressView.progressTintColor = theme[@"accent"];
}

@end

@implementation WorkspaceClockToolViewController {
    UIView *_heroCard;
    UILabel *_timeLabel;
    UILabel *_dateLabel;
    UILabel *_zoneLabel;
    UIStackView *_stackView;
    NSTimer *_timer;
    NSDateFormatter *_timeFormatter;
    NSDateFormatter *_dateFormatter;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Clock";

    _timeFormatter = [NSDateFormatter new];
    _timeFormatter.timeStyle = NSDateFormatterMediumStyle;
    _timeFormatter.dateStyle = NSDateFormatterNoStyle;
    _dateFormatter = [NSDateFormatter new];
    _dateFormatter.dateStyle = NSDateFormatterFullStyle;
    _dateFormatter.timeStyle = NSDateFormatterNoStyle;

    _heroCard = [self workspaceThemeCardView];
    [self.toolContentView addSubview:_heroCard];

    _stackView = [UIStackView new];
    _stackView.axis = UILayoutConstraintAxisVertical;
    _stackView.spacing = 8;
    _stackView.translatesAutoresizingMaskIntoConstraints = NO;
    _stackView.alignment = UIStackViewAlignmentCenter;
    [_heroCard addSubview:_stackView];

    _timeLabel = [self workspaceThemeAccentLabelWithTextStyle:UIFontTextStyleLargeTitle monospaced:YES];
    _timeLabel.numberOfLines = 1;
    _timeLabel.adjustsFontSizeToFitWidth = YES;
    _timeLabel.minimumScaleFactor = 0.5;

    _dateLabel = [self workspaceThemePrimaryLabelWithTextStyle:UIFontTextStyleTitle3 monospaced:NO];
    _dateLabel.numberOfLines = 0;
    _dateLabel.textAlignment = NSTextAlignmentCenter;
    _zoneLabel = [self workspaceThemeSecondaryLabelWithTextStyle:UIFontTextStyleFootnote monospaced:NO];
    _zoneLabel.textAlignment = NSTextAlignmentCenter;

    [_stackView addArrangedSubview:_timeLabel];
    [_stackView addArrangedSubview:_dateLabel];
    [_stackView addArrangedSubview:_zoneLabel];

    [NSLayoutConstraint activateConstraints:@[
        [_heroCard.centerXAnchor constraintEqualToAnchor:self.toolContentView.centerXAnchor],
        [_heroCard.centerYAnchor constraintEqualToAnchor:self.toolContentView.centerYAnchor],
        [_heroCard.leadingAnchor constraintGreaterThanOrEqualToAnchor:self.toolContentView.leadingAnchor constant:10],
        [_heroCard.trailingAnchor constraintLessThanOrEqualToAnchor:self.toolContentView.trailingAnchor constant:-10],
        [_heroCard.widthAnchor constraintLessThanOrEqualToConstant:280],

        [_stackView.topAnchor constraintEqualToAnchor:_heroCard.topAnchor constant:8],
        [_stackView.leadingAnchor constraintEqualToAnchor:_heroCard.leadingAnchor constant:10],
        [_stackView.trailingAnchor constraintEqualToAnchor:_heroCard.trailingAnchor constant:-10],
        [_stackView.bottomAnchor constraintEqualToAnchor:_heroCard.bottomAnchor constant:-8],
    ]];

    // super's viewDidLoad already ran workspaceApplyTheme, but that was before these labels
    // existed, and nothing re-themes them until a theme-change notification fires. Apply the
    // theme now that the labels are built so they pick up the theme colors immediately instead
    // of keeping UILabel's default color (which is invisible against dark theme cards).
    [self workspaceApplyTheme];
    [self refreshClock:nil];
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];

    CGFloat insetY = ISHWorkspaceDensityValue(4, 12);
    CGFloat insetX = ISHWorkspaceDensityValue(6, 16);
    CGRect bounds = UIEdgeInsetsInsetRect(self.toolContentView.bounds, UIEdgeInsetsMake(insetY, insetX, insetY, insetX));
    CGFloat width = MAX(1, CGRectGetWidth(bounds));
    CGFloat height = MAX(1, CGRectGetHeight(bounds));
    CGFloat timeFontSize = MIN(width * 0.22, height * 0.42);
    timeFontSize = MIN(MAX(timeFontSize, 18), 42);
    CGFloat dateFontSize = MIN(width * 0.055, height * 0.14);
    dateFontSize = MIN(MAX(dateFontSize, 9), 14);

    _timeLabel.font = [UIFont monospacedDigitSystemFontOfSize:timeFontSize weight:UIFontWeightBold];
    _dateLabel.font = [UIFont systemFontOfSize:dateFontSize weight:UIFontWeightSemibold];
    _zoneLabel.font = [UIFont systemFontOfSize:MAX(9, round(dateFontSize * 0.76)) weight:UIFontWeightMedium];
    _stackView.spacing = MAX(2, round(timeFontSize * 0.08));
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [_timer invalidate];
    _timer = [NSTimer scheduledTimerWithTimeInterval:1
                                              target:self
                                            selector:@selector(refreshClock:)
                                            userInfo:nil
                                             repeats:YES];
    [self refreshClock:nil];
}

- (void)viewDidDisappear:(BOOL)animated {
    [super viewDidDisappear:animated];
    [_timer invalidate];
    _timer = nil;
}

- (void)refreshClock:(id)sender {
    NSDate *now = NSDate.date;
    _timeLabel.text = [_timeFormatter stringFromDate:now];
    _dateLabel.text = [_dateFormatter stringFromDate:now];
    NSString *abbreviation = NSTimeZone.localTimeZone.abbreviation ?: @"Local";
    _zoneLabel.text = [NSString stringWithFormat:@"%@  •  %@", abbreviation, NSTimeZone.localTimeZone.name ?: @"Time Zone"];
}

- (void)workspaceApplyTheme {
    [super workspaceApplyTheme];
    NSDictionary<NSString *, UIColor *> *theme = self.workspaceTheme;
    UIColor *cardColor = theme[@"card"];
    // Render the hero card effectively opaque. At 0.88 the background gradient bled through the
    // card and dragged the readout's contrast below the descriptor's guarantee on darker themes;
    // the rest of the workspace's surfaces sit at ~0.95-0.98, so the clock was the odd one out.
    _heroCard.backgroundColor = [cardColor colorWithAlphaComponent:0.98];
    // The time is the focal readout and auto-shrinks in small windows, so hold it to a firmer
    // contrast floor than the shared 4.8 accent default. Re-correcting against the actual card
    // (not the averaged tool surface) keeps the date/zone legible on every theme too.
    _timeLabel.textColor = ISHWorkspaceThemeColorAdjustedForContrast(theme[@"accent"], cardColor, 5.5);
    _dateLabel.textColor = ISHWorkspaceThemeColorAdjustedForContrast(theme[@"primary"], cardColor, 7.0);
    _zoneLabel.textColor = ISHWorkspaceThemeColorAdjustedForContrast(theme[@"accentAlt"], cardColor, 4.6);
}

@end

@implementation WorkspaceInfoToolViewController {
    UIScrollView *_scrollView;
    UIStackView *_contentStack;
    UIStackView *_topRow;
    UIStackView *_bottomRow;
    WorkspaceGaugeView *_batteryGauge;
    UILabel *_batterySubtitle;
    WorkspaceGaugeView *_storageGauge;
    UILabel *_storageSubtitle;
    UILabel *_rootValueLabel;
    UILabel *_startupValueLabel;
    NSTimer *_timer;
}

- (UIView *)infoMetricCardWithTitle:(NSString *)title valueLabel:(UILabel * __strong *)valueLabel {
    UIView *card = [self workspaceThemeCardView];
    CGFloat stackInset = ISHWorkspaceUsesPhoneLayout() ? 6.0 : 7.0;
    CGFloat minimumHeight = ISHWorkspaceUsesPhoneLayout() ? 50.0 : 58.0;

    UIStackView *stack = [UIStackView new];
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 6;
    [card addSubview:stack];

    UILabel *titleLabel = [self workspaceThemeSecondaryLabelWithTextStyle:UIFontTextStyleCaption1 monospaced:NO];
    titleLabel.text = [title uppercaseString];
    titleLabel.font = [UIFont systemFontOfSize:8 weight:UIFontWeightSemibold];
    titleLabel.textAlignment = NSTextAlignmentCenter;

    UILabel *label = [self workspaceThemePrimaryLabelWithTextStyle:UIFontTextStyleTitle3 monospaced:NO];
    label.font = [UIFont systemFontOfSize:13 weight:UIFontWeightSemibold];
    label.textAlignment = NSTextAlignmentCenter;
    label.numberOfLines = 2;
    label.adjustsFontSizeToFitWidth = YES;
    label.minimumScaleFactor = 0.55;

    [stack addArrangedSubview:titleLabel];
    [stack addArrangedSubview:label];

    [NSLayoutConstraint activateConstraints:@[
        [card.heightAnchor constraintGreaterThanOrEqualToConstant:minimumHeight],
        [stack.topAnchor constraintEqualToAnchor:card.topAnchor constant:stackInset],
        [stack.leadingAnchor constraintEqualToAnchor:card.leadingAnchor constant:stackInset],
        [stack.trailingAnchor constraintEqualToAnchor:card.trailingAnchor constant:-stackInset],
        [stack.bottomAnchor constraintEqualToAnchor:card.bottomAnchor constant:-stackInset],
    ]];

    if (valueLabel != NULL)
        *valueLabel = label;
    return card;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Info";

    _scrollView = [UIScrollView new];
    _scrollView.translatesAutoresizingMaskIntoConstraints = NO;
    _scrollView.alwaysBounceVertical = YES;
    [self.toolContentView addSubview:_scrollView];

    _contentStack = [UIStackView new];
    _contentStack.translatesAutoresizingMaskIntoConstraints = NO;
    _contentStack.axis = UILayoutConstraintAxisVertical;
    _contentStack.spacing = 6;
    [_scrollView addSubview:_contentStack];

    _topRow = [UIStackView new];
    _topRow.axis = UILayoutConstraintAxisHorizontal;
    _topRow.spacing = 6;
    _topRow.distribution = UIStackViewDistributionFillEqually;
    [_topRow addArrangedSubview:[self workspaceGaugeTileWithIcon:@"battery.100" caption:@"Battery" subtitleLabel:&_batterySubtitle gauge:&_batteryGauge]];
    [_topRow addArrangedSubview:[self workspaceGaugeTileWithIcon:@"internaldrive" caption:@"Storage" subtitleLabel:&_storageSubtitle gauge:&_storageGauge]];

    _bottomRow = [UIStackView new];
    _bottomRow.axis = UILayoutConstraintAxisHorizontal;
    _bottomRow.spacing = 6;
    _bottomRow.distribution = UIStackViewDistributionFillEqually;
    [_bottomRow addArrangedSubview:[self workspaceStatTileWithIcon:@"folder" caption:@"Root" valueLabel:&_rootValueLabel]];
    [_bottomRow addArrangedSubview:[self workspaceStatTileWithIcon:@"bolt" caption:@"Startup" valueLabel:&_startupValueLabel]];

    [_contentStack addArrangedSubview:_topRow];
    [_contentStack addArrangedSubview:_bottomRow];

    [NSLayoutConstraint activateConstraints:@[
        [_scrollView.topAnchor constraintEqualToAnchor:self.toolContentView.topAnchor],
        [_scrollView.leadingAnchor constraintEqualToAnchor:self.toolContentView.leadingAnchor],
        [_scrollView.trailingAnchor constraintEqualToAnchor:self.toolContentView.trailingAnchor],
        [_scrollView.bottomAnchor constraintEqualToAnchor:self.toolContentView.bottomAnchor],

        [_contentStack.topAnchor constraintEqualToAnchor:_scrollView.contentLayoutGuide.topAnchor constant:6],
        [_contentStack.leadingAnchor constraintEqualToAnchor:_scrollView.frameLayoutGuide.leadingAnchor constant:6],
        [_contentStack.trailingAnchor constraintEqualToAnchor:_scrollView.frameLayoutGuide.trailingAnchor constant:-6],
        [_contentStack.bottomAnchor constraintEqualToAnchor:_scrollView.contentLayoutGuide.bottomAnchor constant:-6],
        [_contentStack.widthAnchor constraintEqualToAnchor:_scrollView.frameLayoutGuide.widthAnchor constant:-12],
    ]];

    [self refreshInfo:nil];
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    BOOL compactWidth = CGRectGetWidth(self.toolContentView.bounds) < 240;
    _topRow.axis = compactWidth ? UILayoutConstraintAxisVertical : UILayoutConstraintAxisHorizontal;
    _bottomRow.axis = compactWidth ? UILayoutConstraintAxisVertical : UILayoutConstraintAxisHorizontal;
    _topRow.distribution = compactWidth ? UIStackViewDistributionFill : UIStackViewDistributionFillEqually;
    _bottomRow.distribution = compactWidth ? UIStackViewDistributionFill : UIStackViewDistributionFillEqually;
    CGFloat spacing = ISHWorkspaceDensityValue(4, 8);
    _contentStack.spacing = spacing;
    _topRow.spacing = spacing;
    _bottomRow.spacing = spacing;
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    UIDevice.currentDevice.batteryMonitoringEnabled = YES;
    [_timer invalidate];
    _timer = [NSTimer scheduledTimerWithTimeInterval:1.0
                                              target:self
                                            selector:@selector(refreshInfo:)
                                            userInfo:nil
                                             repeats:YES];
    [self refreshInfo:nil];
}

- (void)viewDidDisappear:(BOOL)animated {
    [super viewDidDisappear:animated];
    [_timer invalidate];
    _timer = nil;
    UIDevice.currentDevice.batteryMonitoringEnabled = NO;
}

- (void)refreshInfo:(id)sender {
    if (UIDevice.currentDevice.batteryState == UIDeviceBatteryStateUnknown || UIDevice.currentDevice.batteryLevel < 0) {
        _batteryGauge.progress = 0.0;
        _batteryGauge.valueText = @"--";
        _batterySubtitle.text = @"Unavailable";
    } else {
        NSString *stateDescription = @"On battery";
        switch (UIDevice.currentDevice.batteryState) {
            case UIDeviceBatteryStateCharging: stateDescription = @"Charging"; break;
            case UIDeviceBatteryStateFull: stateDescription = @"Full"; break;
            case UIDeviceBatteryStateUnplugged: stateDescription = @"On battery"; break;
            case UIDeviceBatteryStateUnknown: break;
        }
        double level = MAX(0.0, MIN(1.0, (double) UIDevice.currentDevice.batteryLevel));
        _batteryGauge.progress = level;
        _batteryGauge.valueText = [NSString stringWithFormat:@"%ld%%", (long) llround(level * 100.0)];
        _batterySubtitle.text = stateDescription;
    }

    NSString *defaultRoot = Roots.instance.defaultRoot;
    _rootValueLabel.text = defaultRoot.length > 0 ? defaultRoot : @"Unavailable";

    NSDictionary<NSFileAttributeKey, id> *attributes =
        [NSFileManager.defaultManager attributesOfFileSystemForPath:NSHomeDirectory() error:nil];
    NSNumber *freeSize = attributes[NSFileSystemFreeSize];
    NSNumber *totalSize = attributes[NSFileSystemSize];
    if (freeSize != nil && totalSize != nil && totalSize.longLongValue > 0) {
        double used = (double) (totalSize.longLongValue - freeSize.longLongValue) / (double) totalSize.longLongValue;
        _storageGauge.progress = MAX(0.0, MIN(1.0, used));
        _storageGauge.valueText = [NSString stringWithFormat:@"%ld%%", (long) llround(used * 100.0)];
        _storageSubtitle.text = [NSString stringWithFormat:@"%@ free",
                                 [NSByteCountFormatter stringFromByteCount:freeSize.longLongValue countStyle:NSByteCountFormatterCountStyleFile]];
    } else {
        _storageGauge.progress = 0.0;
        _storageGauge.valueText = @"--";
        _storageSubtitle.text = @"Unavailable";
    }

    _startupValueLabel.text = ISHInitialWindowTitle();
}

- (void)workspaceApplyTheme {
    [super workspaceApplyTheme];
    _storageGauge.fillColor = self.workspaceTheme[@"accentAlt"];
}

@end

@implementation WorkspaceMonitorToolViewController {
    UIScrollView *_scrollView;
    UIStackView *_contentStack;
    UIStackView *_gaugeRow;
    UIStackView *_gaugeRow2;
    WorkspaceGaugeView *_cpuGauge;
    UILabel *_cpuSubtitle;
    WorkspaceGaugeView *_memoryGauge;
    UILabel *_memorySubtitle;
    WorkspaceGaugeView *_batteryGauge;
    UILabel *_batterySubtitle;
    WorkspaceGaugeView *_storageGauge;
    UILabel *_storageSubtitle;
    UILabel *_uptimeValueLabel;
    UILabel *_rootValueLabel;
    UILabel *_networkValueLabel;
    UILabel *_startupValueLabel;
    UILabel *_liveValueLabel;
    NSTimer *_timer;
    natural_t _previousCPUTicks[CPU_STATE_MAX];
    BOOL _hasPreviousCPUSample;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Monitor";

    _scrollView = [UIScrollView new];
    _scrollView.translatesAutoresizingMaskIntoConstraints = NO;
    _scrollView.alwaysBounceVertical = YES;
    [self.toolContentView addSubview:_scrollView];

    _contentStack = [UIStackView new];
    _contentStack.translatesAutoresizingMaskIntoConstraints = NO;
    _contentStack.axis = UILayoutConstraintAxisVertical;
    _contentStack.spacing = 8;
    [_scrollView addSubview:_contentStack];

    _gaugeRow = [UIStackView new];
    _gaugeRow.axis = UILayoutConstraintAxisHorizontal;
    _gaugeRow.spacing = 8;
    _gaugeRow.distribution = UIStackViewDistributionFillEqually;
    [_gaugeRow addArrangedSubview:[self workspaceGaugeTileWithIcon:@"cpu" caption:@"CPU" subtitleLabel:&_cpuSubtitle gauge:&_cpuGauge]];
    [_gaugeRow addArrangedSubview:[self workspaceGaugeTileWithIcon:@"memorychip" caption:@"Memory" subtitleLabel:&_memorySubtitle gauge:&_memoryGauge]];
    [_contentStack addArrangedSubview:_gaugeRow];

    _gaugeRow2 = [UIStackView new];
    _gaugeRow2.axis = UILayoutConstraintAxisHorizontal;
    _gaugeRow2.spacing = 8;
    _gaugeRow2.distribution = UIStackViewDistributionFillEqually;
    [_gaugeRow2 addArrangedSubview:[self workspaceGaugeTileWithIcon:@"battery.100" caption:@"Battery" subtitleLabel:&_batterySubtitle gauge:&_batteryGauge]];
    [_gaugeRow2 addArrangedSubview:[self workspaceGaugeTileWithIcon:@"internaldrive" caption:@"Storage" subtitleLabel:&_storageSubtitle gauge:&_storageGauge]];
    [_contentStack addArrangedSubview:_gaugeRow2];

    [_contentStack addArrangedSubview:[self workspaceRowsCardWithRows:@[
        [self workspaceIconRowWithIcon:@"clock" title:@"Uptime" valueLabel:&_uptimeValueLabel],
        [self workspaceIconRowWithIcon:@"folder" title:@"Root" valueLabel:&_rootValueLabel],
        [self workspaceIconRowWithIcon:@"network" title:@"Network" valueLabel:&_networkValueLabel],
        [self workspaceIconRowWithIcon:@"bolt" title:@"Startup" valueLabel:&_startupValueLabel],
        [self workspaceIconRowWithIcon:@"square.grid.2x2" title:@"Live" valueLabel:&_liveValueLabel],
    ]]];

    [NSLayoutConstraint activateConstraints:@[
        [_scrollView.topAnchor constraintEqualToAnchor:self.toolContentView.topAnchor],
        [_scrollView.leadingAnchor constraintEqualToAnchor:self.toolContentView.leadingAnchor],
        [_scrollView.trailingAnchor constraintEqualToAnchor:self.toolContentView.trailingAnchor],
        [_scrollView.bottomAnchor constraintEqualToAnchor:self.toolContentView.bottomAnchor],

        [_contentStack.topAnchor constraintEqualToAnchor:_scrollView.contentLayoutGuide.topAnchor constant:6],
        [_contentStack.leadingAnchor constraintEqualToAnchor:_scrollView.frameLayoutGuide.leadingAnchor constant:6],
        [_contentStack.trailingAnchor constraintEqualToAnchor:_scrollView.frameLayoutGuide.trailingAnchor constant:-6],
        [_contentStack.bottomAnchor constraintEqualToAnchor:_scrollView.contentLayoutGuide.bottomAnchor constant:-6],
        [_contentStack.widthAnchor constraintEqualToAnchor:_scrollView.frameLayoutGuide.widthAnchor constant:-12],
    ]];

    [self refreshMonitor:nil];

    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(monitorGaugeStyleDidChange)
                                               name:ISHWorkspaceGaugeStyleDidChangeNotification
                                             object:nil];
}

- (void)monitorGaugeStyleDidChange {
    // Ring vs bar gauges differ in height, so resize the window to the new style (deferred so the
    // gauges re-lay-out first) — keeps both gauge rows visible instead of clipping or floating.
    __weak typeof(self) weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        [(id)weakSelf.workspaceHostViewController autosizeMonitorWindow];
    });
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    UIDevice.currentDevice.batteryMonitoringEnabled = YES;
    [_timer invalidate];
    _timer = [NSTimer scheduledTimerWithTimeInterval:1.0
                                              target:self
                                            selector:@selector(refreshMonitor:)
                                            userInfo:nil
                                             repeats:YES];
    [self refreshMonitor:nil];
}

- (void)viewDidDisappear:(BOOL)animated {
    [super viewDidDisappear:animated];
    [_timer invalidate];
    _timer = nil;
    UIDevice.currentDevice.batteryMonitoringEnabled = NO;
}

- (UILabel *)monitorValueLabelWithMonospaced:(BOOL)monospaced {
    UILabel *label = monospaced
        ? [self workspaceThemePrimaryLabelWithTextStyle:UIFontTextStyleSubheadline monospaced:YES]
        : [self workspaceThemePrimaryLabelWithTextStyle:UIFontTextStyleSubheadline monospaced:NO];
    label.numberOfLines = 1;
    UIFont *font = nil;
    if (monospaced) {
        if (@available(iOS 13.0, *)) {
            font = [UIFont monospacedSystemFontOfSize:12 weight:UIFontWeightRegular];
        } else {
            font = [UIFont fontWithName:@"Menlo-Regular" size:12] ?: [UIFont systemFontOfSize:12];
        }
    } else {
        font = [UIFont preferredFontForTextStyle:UIFontTextStyleSubheadline];
    }
    label.font = font;
    return label;
}

- (UIView *)monitorCardWithContent:(UIView *)contentView {
    UIView *card = [self workspaceThemeCardView];

    contentView.translatesAutoresizingMaskIntoConstraints = NO;
    [card addSubview:contentView];
    [NSLayoutConstraint activateConstraints:@[
        [contentView.topAnchor constraintEqualToAnchor:card.topAnchor constant:6],
        [contentView.leadingAnchor constraintEqualToAnchor:card.leadingAnchor constant:6],
        [contentView.trailingAnchor constraintEqualToAnchor:card.trailingAnchor constant:-6],
        [contentView.bottomAnchor constraintEqualToAnchor:card.bottomAnchor constant:-6],
    ]];
    return card;
}

- (UIView *)monitorBarCardWithTitle:(NSString *)title
                         titleLabel:(UILabel * __strong *)titleLabel
                       progressView:(UIProgressView * __strong *)progressView
                        detailLabel:(UILabel * __strong *)detailLabel {
    UIStackView *stack = [UIStackView new];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 4;

    UILabel *headingLabel = [self monitorValueLabelWithMonospaced:NO];
    headingLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleSubheadline];
    headingLabel.text = title;
    [stack addArrangedSubview:headingLabel];

    UIProgressView *bar = [self workspaceThemeProgressView];
    [stack addArrangedSubview:bar];

    if (titleLabel != NULL)
        *titleLabel = headingLabel;
    if (progressView != NULL)
        *progressView = bar;
    if (detailLabel != NULL)
        *detailLabel = nil;
    return [self monitorCardWithContent:stack];
}

- (UIView *)monitorKeyValueRowWithTitle:(NSString *)title valueLabel:(UILabel * __strong *)valueLabel {
    UIStackView *row = [UIStackView new];
    row.axis = UILayoutConstraintAxisHorizontal;
    row.spacing = 4;
    row.alignment = UIStackViewAlignmentFirstBaseline;

    UILabel *titleLabel = [self monitorValueLabelWithMonospaced:NO];
    titleLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleCaption1];
    titleLabel.text = title;
    titleLabel.textColor = self.workspaceTheme[@"secondary"];

    UILabel *detail = [self monitorValueLabelWithMonospaced:YES];
    detail.textAlignment = NSTextAlignmentRight;

    [row addArrangedSubview:titleLabel];
    [row addArrangedSubview:detail];
    [titleLabel setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
    [detail setContentCompressionResistancePriority:UILayoutPriorityDefaultLow forAxis:UILayoutConstraintAxisHorizontal];

    if (valueLabel != NULL)
        *valueLabel = detail;
    return row;
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    BOOL compactWidth = CGRectGetWidth(self.toolContentView.bounds) < 240;
    UILayoutConstraintAxis gaugeAxis = compactWidth ? UILayoutConstraintAxisVertical : UILayoutConstraintAxisHorizontal;
    UIStackViewDistribution gaugeDist = compactWidth ? UIStackViewDistributionFill : UIStackViewDistributionFillEqually;
    _gaugeRow.axis = gaugeAxis;
    _gaugeRow.distribution = gaugeDist;
    _gaugeRow2.axis = gaugeAxis;
    _gaugeRow2.distribution = gaugeDist;
    _contentStack.spacing = ISHWorkspaceDensityValue(4, 8);
}

- (double)sampleSystemCPURatio {
    host_cpu_load_info_data_t cpuInfo;
    mach_msg_type_number_t cpuInfoCount = HOST_CPU_LOAD_INFO_COUNT;
    mach_port_t hostPort = mach_host_self();
    kern_return_t result = host_statistics(hostPort, HOST_CPU_LOAD_INFO, (host_info_t) &cpuInfo, &cpuInfoCount);
    mach_port_deallocate(mach_task_self(), hostPort);
    if (result != KERN_SUCCESS)
        return -1.0;

    uint64_t totalTicks = 0;
    uint64_t activeTicks = 0;
    for (NSUInteger index = 0; index < CPU_STATE_MAX; index++) {
        natural_t currentTicks = cpuInfo.cpu_ticks[index];
        natural_t previousTicks = _hasPreviousCPUSample ? _previousCPUTicks[index] : 0;
        uint64_t delta = _hasPreviousCPUSample ? (uint64_t) (currentTicks - previousTicks) : (uint64_t) currentTicks;
        totalTicks += delta;
        if (index != CPU_STATE_IDLE)
            activeTicks += delta;
        _previousCPUTicks[index] = currentTicks;
    }
    _hasPreviousCPUSample = YES;
    if (totalTicks == 0)
        return 0.0;
    return (double) activeTicks / (double) totalTicks;
}

- (void)refreshMonitor:(id)sender {
    double cpuRatio = [self sampleSystemCPURatio];
    if (cpuRatio >= 0.0) {
        _cpuGauge.progress = cpuRatio;
        _cpuGauge.valueText = [NSString stringWithFormat:@"%ld%%", (long) llround(cpuRatio * 100.0)];
        _cpuSubtitle.text = @"load";
    } else {
        _cpuGauge.progress = 0.0;
        _cpuGauge.valueText = @"--";
        _cpuSubtitle.text = @"n/a";
    }

    uint64_t footprint = 0;
    uint64_t physical = 0;
    BOOL hasMemory = ISHWorkspaceMemoryUsage(&footprint, NULL, &physical);
    if (hasMemory && physical > 0) {
        double memoryRatio = (double) footprint / (double) physical;
        _memoryGauge.progress = memoryRatio;
        _memoryGauge.valueText = [NSString stringWithFormat:@"%ld%%", (long) llround(memoryRatio * 100.0)];
        _memorySubtitle.text = @"in use";
    } else {
        _memoryGauge.progress = 0.0;
        _memoryGauge.valueText = @"--";
        _memorySubtitle.text = @"n/a";
    }

    if (UIDevice.currentDevice.batteryState == UIDeviceBatteryStateUnknown || UIDevice.currentDevice.batteryLevel < 0) {
        _batteryGauge.progress = 0.0;
        _batteryGauge.valueText = @"--";
        _batterySubtitle.text = @"n/a";
    } else {
        NSString *batteryState = @"on battery";
        switch (UIDevice.currentDevice.batteryState) {
            case UIDeviceBatteryStateCharging: batteryState = @"charging"; break;
            case UIDeviceBatteryStateFull: batteryState = @"full"; break;
            case UIDeviceBatteryStateUnplugged: batteryState = @"on battery"; break;
            case UIDeviceBatteryStateUnknown: break;
        }
        double level = MAX(0.0, MIN(1.0, (double) UIDevice.currentDevice.batteryLevel));
        _batteryGauge.progress = level;
        _batteryGauge.valueText = [NSString stringWithFormat:@"%ld%%", (long) llround(level * 100.0)];
        _batterySubtitle.text = batteryState;
    }

    NSDictionary<NSFileAttributeKey, id> *fsAttributes =
        [NSFileManager.defaultManager attributesOfFileSystemForPath:NSHomeDirectory() error:nil];
    NSNumber *freeSize = fsAttributes[NSFileSystemFreeSize];
    NSNumber *totalSize = fsAttributes[NSFileSystemSize];
    if (freeSize != nil && totalSize != nil && totalSize.longLongValue > 0) {
        double used = (double) (totalSize.longLongValue - freeSize.longLongValue) / (double) totalSize.longLongValue;
        _storageGauge.progress = MAX(0.0, MIN(1.0, used));
        _storageGauge.valueText = [NSString stringWithFormat:@"%ld%%", (long) llround(used * 100.0)];
        _storageSubtitle.text = [NSString stringWithFormat:@"%@ free",
                                 [NSByteCountFormatter stringFromByteCount:freeSize.longLongValue countStyle:NSByteCountFormatterCountStyleFile]];
    } else {
        _storageGauge.progress = 0.0;
        _storageGauge.valueText = @"--";
        _storageSubtitle.text = @"n/a";
    }

    NSUInteger sceneCount = 0;
    if (@available(iOS 13.0, *)) {
        sceneCount = UIApplication.sharedApplication.connectedScenes.count;
    }
    NSString *defaultRoot = Roots.instance.defaultRoot;
    _uptimeValueLabel.text = ISHWorkspaceDurationString(NSProcessInfo.processInfo.systemUptime);
    _rootValueLabel.text = defaultRoot.length > 0 ? defaultRoot : @"unavailable";
    _networkValueLabel.text = ISHWorkspacePrimaryNetworkLine();
    _startupValueLabel.text = ISHInitialWindowTitle();
    _liveValueLabel.text = [NSString stringWithFormat:@"%lu scenes · %lu terminals",
                            (unsigned long) sceneCount,
                            (unsigned long) [Terminal activeTerminals].count];
}

- (void)workspaceApplyTheme {
    [super workspaceApplyTheme];
    _memoryGauge.fillColor = self.workspaceTheme[@"accentAlt"];
    _storageGauge.fillColor = self.workspaceTheme[@"accentAlt"];
}

@end

@implementation WorkspaceNetworksToolViewController {
    UIScrollView *_scrollView;
    UIStackView *_contentStack;
    UILabel *_summaryLabel;
    UITextView *_textView;
    NSTimer *_timer;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Networks";

    _scrollView = [UIScrollView new];
    _scrollView.translatesAutoresizingMaskIntoConstraints = NO;
    _scrollView.alwaysBounceVertical = YES;
    [self.toolContentView addSubview:_scrollView];

    _contentStack = [UIStackView new];
    _contentStack.translatesAutoresizingMaskIntoConstraints = NO;
    _contentStack.axis = UILayoutConstraintAxisVertical;
    _contentStack.spacing = 8;
    [_scrollView addSubview:_contentStack];

    UIView *summaryCard = [self workspaceThemeCardView];
    UIStackView *summaryStack = [UIStackView new];
    summaryStack.translatesAutoresizingMaskIntoConstraints = NO;
    summaryStack.axis = UILayoutConstraintAxisVertical;
    summaryStack.spacing = 6;
    [summaryCard addSubview:summaryStack];
    UILabel *summaryTitle = [self workspaceThemeSecondaryLabelWithTextStyle:UIFontTextStyleCaption1 monospaced:NO];
    summaryTitle.text = @"CONNECTIVITY";
    summaryTitle.font = [UIFont systemFontOfSize:9 weight:UIFontWeightSemibold];
    _summaryLabel = [self workspaceThemeAccentLabelWithTextStyle:UIFontTextStyleHeadline monospaced:NO];
    _summaryLabel.numberOfLines = 0;
    [summaryStack addArrangedSubview:summaryTitle];
    [summaryStack addArrangedSubview:_summaryLabel];
    [NSLayoutConstraint activateConstraints:@[
        [summaryStack.topAnchor constraintEqualToAnchor:summaryCard.topAnchor constant:12],
        [summaryStack.leadingAnchor constraintEqualToAnchor:summaryCard.leadingAnchor constant:12],
        [summaryStack.trailingAnchor constraintEqualToAnchor:summaryCard.trailingAnchor constant:-12],
        [summaryStack.bottomAnchor constraintEqualToAnchor:summaryCard.bottomAnchor constant:-12],
    ]];

    UIView *detailsCard = [self workspaceThemeCardView];
    _textView = [self workspaceThemeTextView];
    [detailsCard addSubview:_textView];
    [NSLayoutConstraint activateConstraints:@[
        [detailsCard.heightAnchor constraintGreaterThanOrEqualToConstant:(ISHWorkspaceUsesPhoneLayout() ? 92.0 : 110.0)],
        [_textView.topAnchor constraintEqualToAnchor:detailsCard.topAnchor constant:8],
        [_textView.leadingAnchor constraintEqualToAnchor:detailsCard.leadingAnchor constant:8],
        [_textView.trailingAnchor constraintEqualToAnchor:detailsCard.trailingAnchor constant:-8],
        [_textView.bottomAnchor constraintEqualToAnchor:detailsCard.bottomAnchor constant:-8],
    ]];

    [_contentStack addArrangedSubview:summaryCard];
    [_contentStack addArrangedSubview:detailsCard];
    [NSLayoutConstraint activateConstraints:@[
        [_scrollView.topAnchor constraintEqualToAnchor:self.toolContentView.topAnchor],
        [_scrollView.leadingAnchor constraintEqualToAnchor:self.toolContentView.leadingAnchor],
        [_scrollView.trailingAnchor constraintEqualToAnchor:self.toolContentView.trailingAnchor],
        [_scrollView.bottomAnchor constraintEqualToAnchor:self.toolContentView.bottomAnchor],

        [_contentStack.topAnchor constraintEqualToAnchor:_scrollView.contentLayoutGuide.topAnchor constant:6],
        [_contentStack.leadingAnchor constraintEqualToAnchor:_scrollView.frameLayoutGuide.leadingAnchor constant:6],
        [_contentStack.trailingAnchor constraintEqualToAnchor:_scrollView.frameLayoutGuide.trailingAnchor constant:-6],
        [_contentStack.bottomAnchor constraintEqualToAnchor:_scrollView.contentLayoutGuide.bottomAnchor constant:-6],
        [_contentStack.widthAnchor constraintEqualToAnchor:_scrollView.frameLayoutGuide.widthAnchor constant:-12],
    ]];

    [self refreshNetworks:nil];
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [_timer invalidate];
    _timer = [NSTimer scheduledTimerWithTimeInterval:2.0
                                              target:self
                                            selector:@selector(refreshNetworks:)
                                            userInfo:nil
                                             repeats:YES];
    [self refreshNetworks:nil];
}

- (void)viewDidDisappear:(BOOL)animated {
    [super viewDidDisappear:animated];
    [_timer invalidate];
    _timer = nil;
}

- (void)refreshNetworks:(id)sender {
    NSString *summary = ISHWorkspaceNetworkSummaryText();
    NSArray<NSString *> *lines = [summary componentsSeparatedByString:@"\n"];
    _summaryLabel.text = lines.firstObject ?: @"Network unavailable";
    if (lines.count > 1) {
        _textView.text = [[lines subarrayWithRange:NSMakeRange(1, lines.count - 1)] componentsJoinedByString:@"\n"];
    } else {
        _textView.text = @"No active interfaces to display.";
    }
}

- (void)workspaceApplyTheme {
    [super workspaceApplyTheme];
    _summaryLabel.textColor = self.workspaceTheme[@"accentAlt"];
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    _contentStack.spacing = ISHWorkspaceDensityValue(4, 8);
}

@end

// Guest log reads run /bin/sh in the guest via run_guest_command_capture, which repoints
// the kernel's `current`, so they must run on a dedicated host thread (never a guest task
// thread). Serialize them on one queue so overlapping refreshes can't race.
static dispatch_queue_t ISHWorkspaceLogReaderQueue(void) {
    static dispatch_queue_t queue;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        queue = dispatch_queue_create("app.ish.workspace.logreader", DISPATCH_QUEUE_SERIAL);
    });
    return queue;
}

@implementation WorkspaceStatusToolViewController {
    UILabel *_sourceLabel;
    UITextView *_logTextView;
    NSTimer *_timer;
    NSUInteger _refreshGeneration;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Logs";

    UIView *card = [self workspaceThemeCardView];
    card.translatesAutoresizingMaskIntoConstraints = NO;
    [self.toolContentView addSubview:card];

    UIStackView *cardStack = [UIStackView new];
    cardStack.translatesAutoresizingMaskIntoConstraints = NO;
    cardStack.axis = UILayoutConstraintAxisVertical;
    cardStack.spacing = 6;
    [card addSubview:cardStack];

    [cardStack addArrangedSubview:[self workspaceTileHeaderRowWithIcon:@"doc.text" caption:@"RECENT LOGS" trailingLabel:&_sourceLabel]];
    _sourceLabel.textAlignment = NSTextAlignmentRight;

    // The log text view fills the (resizable) window below the fixed header and scrolls
    // internally, so resizing the window changes how much of the log is visible.
    _logTextView = [self workspaceThemeTextView];
    _logTextView.scrollEnabled = YES;
    _logTextView.alwaysBounceVertical = YES;
    [_logTextView setContentHuggingPriority:UILayoutPriorityDefaultLow forAxis:UILayoutConstraintAxisVertical];
    [_logTextView setContentCompressionResistancePriority:UILayoutPriorityDefaultLow forAxis:UILayoutConstraintAxisVertical];
    [cardStack addArrangedSubview:_logTextView];

    [NSLayoutConstraint activateConstraints:@[
        [card.topAnchor constraintEqualToAnchor:self.toolContentView.topAnchor constant:6],
        [card.leadingAnchor constraintEqualToAnchor:self.toolContentView.leadingAnchor constant:6],
        [card.trailingAnchor constraintEqualToAnchor:self.toolContentView.trailingAnchor constant:-6],
        [card.bottomAnchor constraintEqualToAnchor:self.toolContentView.bottomAnchor constant:-6],
        [cardStack.topAnchor constraintEqualToAnchor:card.topAnchor constant:10],
        [cardStack.leadingAnchor constraintEqualToAnchor:card.leadingAnchor constant:12],
        [cardStack.trailingAnchor constraintEqualToAnchor:card.trailingAnchor constant:-12],
        [cardStack.bottomAnchor constraintEqualToAnchor:card.bottomAnchor constant:-10],
    ]];

    _sourceLabel.text = @"reading…";
    _logTextView.text = @"Loading recent log entries…";
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [self refreshLogs];
    [_timer invalidate];
    _timer = [NSTimer scheduledTimerWithTimeInterval:20.0
                                              target:self
                                            selector:@selector(refreshLogs)
                                            userInfo:nil
                                             repeats:YES];
}

- (void)viewDidDisappear:(BOOL)animated {
    [super viewDidDisappear:animated];
    [_timer invalidate];
    _timer = nil;
}

- (void)refreshLogs {
    NSUInteger generation = ++_refreshGeneration;
    // Probe the conventional log locations across rootfs's (Alpine writes /var/log/messages,
    // Devuan/Debian write /var/log/syslog), then fall back to the newest non-empty file in
    // /var/log. Running it as a guest shell snippet keeps it rootfs-agnostic.
    static NSString *const script =
        @"for f in /var/log/messages /var/log/syslog /var/log/dmesg /var/log/auth.log; do "
        @"[ -s \"$f\" ] && { printf '== %s ==\\n' \"$f\"; tail -n 200 \"$f\"; exit 0; }; done; "
        @"for f in $(ls -1t /var/log 2>/dev/null); do p=\"/var/log/$f\"; "
        @"[ -f \"$p\" ] && [ -s \"$p\" ] && { printf '== %s ==\\n' \"$p\"; tail -n 200 \"$p\"; exit 0; }; done; "
        @"echo __NOLOGS__";
    dispatch_async(ISHWorkspaceLogReaderQueue(), ^{
        struct guest_command_result result;
        memset(&result, 0, sizeof(result));
        int rc = run_guest_command_capture(script.UTF8String, NULL, 6000, 64 * 1024, &result);
        BOOL launched = (rc >= 0 && result.launched);
        NSString *captured = nil;
        if (launched && result.output != NULL && result.output_len > 0)
            captured = [[NSString alloc] initWithBytes:result.output length:result.output_len encoding:NSUTF8StringEncoding];
        if (result.output != NULL)
            free(result.output);
        dispatch_async(dispatch_get_main_queue(), ^{
            if (generation != self->_refreshGeneration)
                return;
            [self applyLogText:captured launched:launched];
        });
    });
}

- (void)applyLogText:(NSString *)captured launched:(BOOL)launched {
    if (!launched) {
        _sourceLabel.text = @"guest";
        _logTextView.text = @"The guest system isn't running yet. Start a shell or console, then recent log lines will appear here.";
        return;
    }
    NSString *trimmed = [(captured ?: @"") stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if (trimmed.length == 0 || [trimmed containsString:@"__NOLOGS__"]) {
        _sourceLabel.text = @"/var/log";
        _logTextView.text = @"No populated log files under /var/log. Many minimal rootfs's don't run a syslog daemon by default.";
        return;
    }
    NSMutableArray<NSString *> *lines = [[trimmed componentsSeparatedByString:@"\n"] mutableCopy];
    NSString *source = @"/var/log";
    if (lines.count > 0 && [lines.firstObject hasPrefix:@"== "]) {
        source = [[[lines.firstObject stringByReplacingOccurrencesOfString:@"== " withString:@""]
                   stringByReplacingOccurrencesOfString:@" ==" withString:@""]
                  stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        [lines removeObjectAtIndex:0];
    }
    _sourceLabel.text = source;
    _logTextView.text = lines.count > 0 ? [lines componentsJoinedByString:@"\n"] : @"(log file is empty)";
}

@end
