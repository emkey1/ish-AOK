#import "WorkspaceViewController.h"

#import "AboutViewController.h"
#import "Diagnostics.h"
#import "Roots.h"
#import "SceneDelegate.h"
#include "fs/devices.h"
#include "kernel/init.h"
#import "TerminalViewController.h"
#import "UserPreferences.h"
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <mach/mach.h>
#include <mach/task_info.h>
#include <net/if.h>

@class ISHWorkspaceContainedWindowView;

@interface WorkspaceViewController ()

@property (nonatomic, copy) NSString *initialToolIdentifier;
@property (nonatomic) BOOL didOpenInitialTool;
@property (nonatomic, strong) UIView *desktopSurfaceView;
@property (nonatomic, strong) NSMutableArray<UIView *> *desktopWindows;
@property (nonatomic) NSInteger desktopWindowCascadeIndex;
@property (nonatomic, weak) ISHWorkspaceContainedWindowView *dashboardWindow;
@property (nonatomic, weak) ISHWorkspaceContainedWindowView *dockWindow;
@property (nonatomic) CGSize dashboardExpandedSize;
@property (nonatomic) BOOL dashboardIsCompact;
@property (nonatomic, strong) UILabel *clockLabel;
@property (nonatomic, strong) UILabel *batteryLabel;
@property (nonatomic, strong) UILabel *rootLabel;
@property (nonatomic, strong) UILabel *storageLabel;
@property (nonatomic, strong) UILabel *startupPreferenceLabel;
@property (nonatomic, strong) UILabel *windowSummaryLabel;
@property (nonatomic, strong) UILabel *systemSummaryLabel;
@property (nonatomic, strong) UILabel *networkSummaryLabel;
@property (nonatomic, strong) UILabel *diagnosticsSummaryLabel;
@property (nonatomic, strong) UIButton *dockDashboardButton;
@property (nonatomic, strong) UIButton *dockUtilsButton;
@property (nonatomic, strong) UIButton *dockTerminalButton;
@property (nonatomic, strong) UIStackView *sceneWindowsStack;
@property (nonatomic, strong) UIStackView *activeTerminalsStack;
@property (nonatomic, strong) UILabel *breadcrumbsLabel;
@property (nonatomic, strong) UILabel *summaryLabel;
@property (nonatomic, strong) UIStackView *bodyStack;
@property (nonatomic, strong) UIStackView *leadingColumnStack;
@property (nonatomic, strong) UIStackView *trailingColumnStack;
@property (nonatomic, strong) UIView *statusCard;
@property (nonatomic, strong) UIView *actionsCard;
@property (nonatomic, strong) UIView *toolsCard;
@property (nonatomic, strong) UIView *windowCard;
@property (nonatomic, strong) UIView *systemCard;
@property (nonatomic, strong) UIView *networkCard;
@property (nonatomic, strong) UIView *terminalsCard;
@property (nonatomic, strong) UIView *eventsCard;
@property (nonatomic, strong) NSDateFormatter *timeFormatter;
@property (nonatomic, strong) NSTimer *clockTimer;

- (UISceneSession *)sceneSessionHostingTerminalUUID:(NSUUID *)terminalUUID API_AVAILABLE(ios(13.0));
- (BOOL)focusSceneSession:(UISceneSession *)sceneSession title:(NSString *)title API_AVAILABLE(ios(13.0));

@end

NSString *const ISHInitialWindowWorkspaceValue = @"workspace";
NSString *const ISHInitialWindowChooseFilesystemValue = @"choose-filesystem";
static NSString *const ISHWorkspaceToolClockIdentifier = @"clock";
static NSString *const ISHWorkspaceToolInfoIdentifier = @"info";
static NSString *const ISHWorkspaceToolMonitorIdentifier = @"monitor";
static NSString *const ISHWorkspaceToolNetworksIdentifier = @"networks";
static NSString *const ISHWorkspaceToolStatusIdentifier = @"status";
static NSString *const ISHWorkspaceToolFilesystemsIdentifier = @"filesystems";
static NSString *const ISHWorkspaceToolSettingsIdentifier = @"settings";
static NSString *const ISHWorkspaceToolDiagnosticsIdentifier = @"diagnostics";
static NSString *const ISHWorkspaceSavedLayoutDefaultsKey = @"ISHWorkspaceSavedLayout";
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

static CGRect ISHWorkspaceRectWithRoundedOriginPreservingSize(CGRect frame) {
    frame.origin.x = round(frame.origin.x);
    frame.origin.y = round(frame.origin.y);
    return frame;
}

@interface ISHWorkspaceContainedWindowView : UIView

@property (nonatomic) CGSize preferredSize;
@property (nonatomic) BOOL didApplyInitialFrame;
@property (nonatomic) BOOL draggable;
@property (nonatomic) BOOL resizable;
@property (nonatomic, strong) UIView *titleBarView;
@property (nonatomic, strong) UILabel *titleLabel;
@property (nonatomic, strong) UIButton *closeButton;
@property (nonatomic, strong) UIButton *utilityButton;
@property (nonatomic, strong) UIView *contentContainerView;
@property (nonatomic, strong) UIView *resizeHandleView;
@property (nonatomic, strong) NSLayoutConstraint *resizeHandleTopConstraint;
@property (nonatomic, strong) NSLayoutConstraint *resizeHandleBottomConstraint;
@property (nonatomic, strong) NSLayoutConstraint *resizeHandleTrailingConstraint;
@property (nonatomic, strong) NSLayoutConstraint *resizeHandleLeadingConstraint;
@property (nonatomic, copy, nullable) dispatch_block_t closeHandler;
@property (nonatomic, copy, nullable) dispatch_block_t utilityHandler;
@property (nonatomic, copy, nullable) dispatch_block_t didBecomeFrontmostHandler;
@property (nonatomic, weak) TerminalViewController *hostedTerminalViewController;
@property (nonatomic, copy) NSString *workspaceToolIdentifier;
@property (nonatomic, copy) NSString *workspaceTerminalRole;
@property (nonatomic) BOOL pinnedToLowerRight;
@property (nonatomic) BOOL resizeHandleAtTopRight;
@property (nonatomic) CGSize minimumSize;
@property (nonatomic) CGSize maximumSize;

- (instancetype)initWithTitle:(NSString *)title showsCloseButton:(BOOL)showsCloseButton;
- (void)setUtilityButtonTitle:(nullable NSString *)title handler:(nullable dispatch_block_t)handler;

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
    self.pinnedToLowerRight = NO;
    self.resizeHandleAtTopRight = NO;
    self.minimumSize = CGSizeMake(280, 180);
    self.maximumSize = CGSizeZero;

    UIView *panelView = [UIView new];
    panelView.translatesAutoresizingMaskIntoConstraints = NO;
    panelView.layer.cornerRadius = ISHWorkspaceWindowCornerRadius;
    panelView.layer.masksToBounds = YES;
    if (@available(iOS 13.0, *)) {
        panelView.backgroundColor = UIColor.secondarySystemBackgroundColor;
    } else {
        panelView.backgroundColor = UIColor.whiteColor;
    }
    [self addSubview:panelView];

    self.titleBarView = [UIView new];
    self.titleBarView.translatesAutoresizingMaskIntoConstraints = NO;
    if (@available(iOS 13.0, *)) {
        self.titleBarView.backgroundColor = [UIColor.secondarySystemBackgroundColor colorWithAlphaComponent:0.96];
    } else {
        self.titleBarView.backgroundColor = [UIColor colorWithWhite:0.94 alpha:1.0];
    }
    [panelView addSubview:self.titleBarView];

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
    [panelView addSubview:self.contentContainerView];

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
    [panelView addSubview:self.resizeHandleView];

    self.resizeHandleLeadingConstraint =
        [self.resizeHandleView.leadingAnchor constraintGreaterThanOrEqualToAnchor:panelView.leadingAnchor constant:12];
    self.resizeHandleTrailingConstraint =
        [self.resizeHandleView.trailingAnchor constraintEqualToAnchor:panelView.trailingAnchor constant:-12];
    self.resizeHandleTopConstraint =
        [self.resizeHandleView.topAnchor constraintEqualToAnchor:panelView.topAnchor constant:12];
    self.resizeHandleBottomConstraint =
        [self.resizeHandleView.bottomAnchor constraintEqualToAnchor:panelView.bottomAnchor constant:-12];

    [NSLayoutConstraint activateConstraints:@[
        [panelView.topAnchor constraintEqualToAnchor:self.topAnchor],
        [panelView.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
        [panelView.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
        [panelView.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],

        [self.titleBarView.topAnchor constraintEqualToAnchor:panelView.topAnchor],
        [self.titleBarView.leadingAnchor constraintEqualToAnchor:panelView.leadingAnchor],
        [self.titleBarView.trailingAnchor constraintEqualToAnchor:panelView.trailingAnchor],
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
        [self.contentContainerView.leadingAnchor constraintEqualToAnchor:panelView.leadingAnchor],
        [self.contentContainerView.trailingAnchor constraintEqualToAnchor:panelView.trailingAnchor],
        [self.contentContainerView.bottomAnchor constraintEqualToAnchor:panelView.bottomAnchor],

        [self.resizeHandleView.widthAnchor constraintEqualToConstant:20],
        [self.resizeHandleView.heightAnchor constraintEqualToConstant:20],
        self.resizeHandleLeadingConstraint,
        self.resizeHandleTrailingConstraint,
        self.resizeHandleBottomConstraint,
    ]];

    UIPanGestureRecognizer *panGestureRecognizer = [[UIPanGestureRecognizer alloc] initWithTarget:self action:@selector(handlePan:)];
    [self.titleBarView addGestureRecognizer:panGestureRecognizer];

    UIPanGestureRecognizer *resizeGestureRecognizer = [[UIPanGestureRecognizer alloc] initWithTarget:self action:@selector(handleResizePan:)];
    [self.resizeHandleView addGestureRecognizer:resizeGestureRecognizer];

    UITapGestureRecognizer *tapGestureRecognizer = [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(bringWindowToFront)];
    tapGestureRecognizer.cancelsTouchesInView = NO;
    [self addGestureRecognizer:tapGestureRecognizer];

    return self;
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

- (void)setUtilityButtonTitle:(NSString *)title handler:(dispatch_block_t)handler {
    self.utilityHandler = handler;
    BOOL visible = title.length > 0 && handler != nil;
    [self.utilityButton setTitle:title forState:UIControlStateNormal];
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
    if (!self.draggable || self.superview == nil)
        return;

    if (recognizer.state == UIGestureRecognizerStateBegan) {
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
    [recognizer setTranslation:CGPointZero inView:self.superview];
}

- (void)handleResizePan:(UIPanGestureRecognizer *)recognizer {
    if (!self.resizable || self.superview == nil)
        return;

    if (recognizer.state == UIGestureRecognizerStateBegan) {
        [self bringWindowToFront];
    }

    CGPoint translation = [recognizer translationInView:self.superview];
    CGRect frame = self.frame;
    CGFloat maxWidth = self.pinnedToLowerRight ? CGRectGetMaxX(frame) : CGRectGetWidth(self.superview.bounds) - CGRectGetMinX(frame);
    CGFloat maxHeight = self.pinnedToLowerRight ? CGRectGetMaxY(frame) : CGRectGetHeight(self.superview.bounds) - CGRectGetMinY(frame);
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
    if (self.pinnedToLowerRight) {
        frame.origin.x = CGRectGetWidth(self.superview.bounds) - CGRectGetWidth(frame);
        frame.origin.y = CGRectGetHeight(self.superview.bounds) - CGRectGetHeight(frame);
    }
    self.frame = CGRectIntegral(frame);
    self.preferredSize = frame.size;
    [recognizer setTranslation:CGPointZero inView:self.superview];
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

static CGSize ISHWorkspacePreferredToolContentSize(NSString *toolIdentifier) {
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolClockIdentifier])
        return CGSizeMake(200, 140);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolInfoIdentifier])
        return CGSizeMake(340, 210);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolMonitorIdentifier])
        return CGSizeMake(400, 240);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolNetworksIdentifier])
        return CGSizeMake(420, 260);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolStatusIdentifier])
        return CGSizeMake(720, 560);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolDiagnosticsIdentifier])
        return CGSizeMake(760, 700);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolFilesystemsIdentifier])
        return CGSizeMake(760, 720);
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolSettingsIdentifier])
        return CGSizeMake(760, 760);
    return CGSizeMake(720, 640);
}

static CGSize ISHWorkspacePreferredTerminalContentSize(void) {
    return CGSizeMake(900, 620);
}

static CGSize ISHWorkspaceCompactDashboardSize(void) {
    return CGSizeMake(440, 320);
}

static CGSize ISHWorkspacePreferredDockContentSize(void) {
    return CGSizeMake(360, 188);
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
        return @"System Status";
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolDiagnosticsIdentifier])
        return @"Diagnostics";
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolFilesystemsIdentifier])
        return @"Filesystems";
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolSettingsIdentifier])
        return @"Settings";
    return @"Window";
}

BOOL ISHShouldLaunchWorkspaceAtStartup(void) {
    NSString *initialWindow = [NSUserDefaults.standardUserDefaults stringForKey:kPreferenceInitialWindowKey];
    return [initialWindow isEqualToString:ISHInitialWindowWorkspaceValue];
}

static NSString *ISHInitialWindowTitle(void) {
    NSString *initialWindow = [NSUserDefaults.standardUserDefaults stringForKey:kPreferenceInitialWindowKey];
    if ([initialWindow isEqualToString:ISHInitialWindowWorkspaceValue])
        return @"Workspace";
    if ([initialWindow isEqualToString:ISHInitialWindowChooseFilesystemValue])
        return @"Choose Filesystem";
    if ([initialWindow isEqualToString:@"session-shell"])
        return @"Session Shell (pts/0)";
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

        NSString *interfaceName = [NSString stringWithUTF8String:cursor->ifa_name];
        NSString *address = [NSString stringWithUTF8String:addressBuffer];
        BOOL isLoopback = (cursor->ifa_flags & IFF_LOOPBACK) != 0;
        if (isLoopback) {
            if (loopbackLine == nil)
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

@interface WorkspaceClockToolViewController : UIViewController
@end

@interface WorkspaceInfoToolViewController : UIViewController
@end

@interface WorkspaceMonitorToolViewController : UIViewController
@end

@interface WorkspaceNetworksToolViewController : UIViewController
@end

@interface WorkspaceStatusToolViewController : UIViewController
@end

static UIViewController *ISHCreateWorkspaceToolViewController(NSString *toolIdentifier) {
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
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolFilesystemsIdentifier])
        return ISHCreateRootsViewController();
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolSettingsIdentifier]) {
        UINavigationController *navigationController = ISHCreateAboutNavigationController(NO, NO);
        return navigationController.topViewController;
    }
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolDiagnosticsIdentifier])
        return ISHCreateDiagnosticsViewController();
    return nil;
}

NSString *ISHWorkspaceToolIdentifierForViewController(UIViewController *viewController) {
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
    if ([viewController isKindOfClass:NSClassFromString(@"DiagnosticsViewController")])
        return ISHWorkspaceToolDiagnosticsIdentifier;
    if ([viewController isKindOfClass:NSClassFromString(@"AboutViewController")])
        return ISHWorkspaceToolSettingsIdentifier;
    if ([viewController isKindOfClass:NSClassFromString(@"RootsTableViewController")])
        return ISHWorkspaceToolFilesystemsIdentifier;
    return nil;
}

@implementation WorkspaceViewController

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

- (UIButton *)workspaceCompactActionButtonWithTitle:(NSString *)title selector:(SEL)selector {
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    button.translatesAutoresizingMaskIntoConstraints = NO;
    button.contentHorizontalAlignment = UIControlContentHorizontalAlignmentLeft;
    button.titleLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleSubheadline];
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
    button.contentEdgeInsets = UIEdgeInsetsMake(10, 12, 10, 12);
    button.contentHorizontalAlignment = UIControlContentHorizontalAlignmentCenter;
    button.contentVerticalAlignment = UIControlContentVerticalAlignmentCenter;
    button.titleLabel.numberOfLines = 2;
    button.titleLabel.textAlignment = NSTextAlignmentCenter;
    button.titleLabel.adjustsFontForContentSizeCategory = YES;
    button.layer.cornerRadius = 14;
    button.layer.borderWidth = 1;
    [button.heightAnchor constraintGreaterThanOrEqualToConstant:54].active = YES;
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

    UIFont *titleFont = [UIFont preferredFontForTextStyle:UIFontTextStyleSubheadline];
    UIFont *stateFont = [UIFont preferredFontForTextStyle:UIFontTextStyleCaption1];
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

- (UIStackView *)workspaceToolLauncherRowWithTitle:(NSString *)title
                                          subtitle:(NSString *)subtitle
                                    toolIdentifier:(NSString *)toolIdentifier {
    UIStackView *row = [UIStackView new];
    row.axis = UILayoutConstraintAxisHorizontal;
    row.spacing = 10;
    row.alignment = UIStackViewAlignmentCenter;

    UIStackView *labelStack = [UIStackView new];
    labelStack.axis = UILayoutConstraintAxisVertical;
    labelStack.spacing = 2;

    UILabel *titleLabel = [self workspaceLabelWithTextStyle:UIFontTextStyleBody monospaced:NO];
    titleLabel.text = title;
    UILabel *subtitleLabel = [self workspaceLabelWithTextStyle:UIFontTextStyleFootnote monospaced:NO];
    if (@available(iOS 13.0, *)) {
        subtitleLabel.textColor = UIColor.secondaryLabelColor;
    } else {
        subtitleLabel.textColor = UIColor.darkGrayColor;
    }
    subtitleLabel.text = subtitle;
    [labelStack addArrangedSubview:titleLabel];
    [labelStack addArrangedSubview:subtitleLabel];

    UIButton *hereButton = [self workspaceCompactActionButtonWithTitle:@"Here"
                                                              selector:@selector(openWorkspaceToolHereFromButton:)];
    hereButton.accessibilityIdentifier = toolIdentifier;
    UIButton *windowButton = [self workspaceCompactActionButtonWithTitle:@"Window"
                                                                selector:@selector(openWorkspaceToolWindowFromButton:)];
    windowButton.accessibilityIdentifier = toolIdentifier;

    [row addArrangedSubview:labelStack];
    [row addArrangedSubview:hereButton];
    [row addArrangedSubview:windowButton];
    [labelStack setContentHuggingPriority:UILayoutPriorityDefaultLow forAxis:UILayoutConstraintAxisHorizontal];
    [labelStack setContentCompressionResistancePriority:UILayoutPriorityDefaultLow forAxis:UILayoutConstraintAxisHorizontal];
    [hereButton setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
    [windowButton setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
    return row;
}

- (UIView *)workspaceCardWithContentStack:(UIStackView **)contentStackOut {
    UIView *card = [UIView new];
    card.translatesAutoresizingMaskIntoConstraints = NO;
    card.layer.cornerRadius = 18;
    card.layer.masksToBounds = NO;
    card.layer.shadowColor = UIColor.blackColor.CGColor;
    card.layer.shadowOpacity = 0.08;
    card.layer.shadowRadius = 18;
    card.layer.shadowOffset = CGSizeMake(0, 8);
    if (@available(iOS 13.0, *)) {
        card.backgroundColor = UIColor.secondarySystemBackgroundColor;
    } else {
        card.backgroundColor = [UIColor colorWithWhite:0.96 alpha:1.0];
    }

    UIStackView *contentStack = [UIStackView new];
    contentStack.axis = UILayoutConstraintAxisVertical;
    contentStack.spacing = 14;
    contentStack.translatesAutoresizingMaskIntoConstraints = NO;
    [card addSubview:contentStack];

    [NSLayoutConstraint activateConstraints:@[
        [contentStack.topAnchor constraintEqualToAnchor:card.topAnchor constant:18],
        [contentStack.leadingAnchor constraintEqualToAnchor:card.leadingAnchor constant:18],
        [contentStack.trailingAnchor constraintEqualToAnchor:card.trailingAnchor constant:-18],
        [contentStack.bottomAnchor constraintEqualToAnchor:card.bottomAnchor constant:-18],
    ]];

    if (contentStackOut != NULL)
        *contentStackOut = contentStack;
    return card;
}

- (CGRect)desktopUsableBounds {
    UIEdgeInsets insets = self.view.safeAreaInsets;
    CGRect bounds = self.desktopSurfaceView.bounds;
    return UIEdgeInsetsInsetRect(bounds, UIEdgeInsetsMake(insets.top, 0, 0, 0));
}

- (CGRect)desktopFrameForWindowWithPreferredSize:(CGSize)preferredSize {
    CGRect usableBounds = [self desktopUsableBounds];
    CGFloat width = MIN(preferredSize.width, CGRectGetWidth(usableBounds));
    CGFloat height = MIN(preferredSize.height, CGRectGetHeight(usableBounds));
    CGFloat offset = (CGFloat) (self.desktopWindowCascadeIndex % 6) * 28.0;
    CGFloat originX = CGRectGetMinX(usableBounds) + MAX(0, (CGRectGetWidth(usableBounds) - width) * 0.5) + offset;
    CGFloat originY = CGRectGetMinY(usableBounds) + MAX(0, (CGRectGetHeight(usableBounds) - height) * 0.16) + offset;
    originX = MIN(originX, CGRectGetMaxX(usableBounds) - width);
    originY = MIN(originY, CGRectGetMaxY(usableBounds) - height);
    self.desktopWindowCascadeIndex += 1;
    return CGRectIntegral(CGRectMake(originX, originY, width, height));
}

- (CGRect)clampedDesktopFrame:(CGRect)frame forWindow:(ISHWorkspaceContainedWindowView *)windowView {
    CGRect usableBounds = [self desktopUsableBounds];
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

    windowView.frame = [self clampedDesktopFrame:windowView.frame forWindow:windowView];
}

- (void)pinDesktopWindowToLowerRight:(ISHWorkspaceContainedWindowView *)windowView {
    if (windowView == nil || windowView.superview == nil)
        return;
    CGRect usableBounds = [self desktopUsableBounds];
    CGRect frame = windowView.frame;
    frame.origin.x = CGRectGetMaxX(usableBounds) - CGRectGetWidth(frame);
    frame.origin.y = CGRectGetMaxY(usableBounds) - CGRectGetHeight(frame);
    windowView.frame = ISHWorkspaceRectWithRoundedOriginPreservingSize(frame);
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
    width = MIN(width, CGRectGetWidth(usableBounds));
    height = MIN(height, CGRectGetHeight(usableBounds));

    CGRect frame = windowView.frame;
    frame.size = CGSizeMake(width, height);
    frame = [self clampedDesktopFrame:frame forWindow:windowView];
    windowView.preferredSize = frame.size;
    if (windowView.pinnedToLowerRight) {
        frame.origin.x = CGRectGetMaxX(usableBounds) - CGRectGetWidth(frame);
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

- (void)applyInitialFrameIfNeededToDesktopWindow:(ISHWorkspaceContainedWindowView *)windowView {
    if (windowView.didApplyInitialFrame || self.desktopSurfaceView.bounds.size.width <= 0 || self.desktopSurfaceView.bounds.size.height <= 0)
        return;
    windowView.frame = [self desktopFrameForWindowWithPreferredSize:windowView.preferredSize];
    windowView.didApplyInitialFrame = YES;
}

- (ISHWorkspaceContainedWindowView *)createDesktopWindowWithTitle:(NSString *)title
                                                    preferredSize:(CGSize)preferredSize
                                                 showsCloseButton:(BOOL)showsCloseButton {
    ISHWorkspaceContainedWindowView *windowView = [[ISHWorkspaceContainedWindowView alloc] initWithTitle:title
                                                                                         showsCloseButton:showsCloseButton];
    windowView.preferredSize = preferredSize;
    windowView.frame = CGRectIntegral(CGRectMake(0, 0,
                                                 MAX(1, preferredSize.width),
                                                 MAX(1, preferredSize.height)));
    [self.desktopSurfaceView addSubview:windowView];
    [self.desktopWindows addObject:windowView];
    [self applyInitialFrameIfNeededToDesktopWindow:windowView];
    [self.desktopSurfaceView bringSubviewToFront:windowView];
    __weak typeof(self) weakSelf = self;
    windowView.didBecomeFrontmostHandler = ^{
        [weakSelf refreshDockButtons];
    };
    return windowView;
}

- (void)attachViewController:(UIViewController *)viewController toDesktopWindow:(ISHWorkspaceContainedWindowView *)windowView {
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
        [strongViewController willMoveToParentViewController:nil];
        [strongViewController.view removeFromSuperview];
        [strongViewController removeFromParentViewController];
        [strongSelf.desktopWindows removeObject:strongWindowView];
        [strongWindowView removeFromSuperview];
        [strongSelf refreshDockButtons];
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
            @"compact": @(self.dashboardIsCompact),
            @"hidden": @(self.dashboardWindow.hidden),
            @"expandedSize": ISHWorkspaceSizeDescriptor(self.dashboardExpandedSize),
        };
    }

    if (windowView == self.dockWindow) {
        return @{
            @"kind": ISHWorkspaceSavedLayoutKindDock,
            @"size": ISHWorkspaceSizeDescriptor(windowView.bounds.size),
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
        return descriptor;
    }

    if (windowView.workspaceToolIdentifier.length > 0) {
        return @{
            @"kind": ISHWorkspaceSavedLayoutKindTool,
            @"frame": frameDescriptor,
            @"toolIdentifier": windowView.workspaceToolIdentifier,
        };
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
    CGSize expandedSize = ISHWorkspaceSizeFromDescriptor(descriptor[@"expandedSize"]);
    if (expandedSize.width > 0 && expandedSize.height > 0) {
        self.dashboardExpandedSize = expandedSize;
    } else if (self.dashboardWindow.bounds.size.width > 0 && self.dashboardWindow.bounds.size.height > 0) {
        self.dashboardExpandedSize = self.dashboardWindow.bounds.size;
    }

    self.dashboardIsCompact = [descriptor[@"compact"] boolValue];
    [self.dashboardWindow setUtilityButtonTitle:(self.dashboardIsCompact ? @"Full" : @"Mini")
                                        handler:self.dashboardWindow.utilityHandler];
    CGSize fallbackSize = self.dashboardWindow.bounds.size.width > 0
        ? self.dashboardWindow.bounds.size
        : self.dashboardWindow.preferredSize;
    [self applySavedFrameDescriptor:frameDescriptor toWindow:self.dashboardWindow fallbackSize:fallbackSize];
    self.dashboardWindow.hidden = [descriptor[@"hidden"] boolValue];
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
    UIViewController *viewController = ISHCreateWorkspaceToolViewController(toolIdentifier);
    if (viewController == nil)
        return nil;
    CGSize preferredSize = ISHWorkspacePreferredToolContentSize(toolIdentifier);
    viewController.preferredContentSize = preferredSize;
    ISHWorkspaceContainedWindowView *windowView =
        [self createDesktopWindowWithTitle:ISHWorkspaceToolTitle(toolIdentifier)
                             preferredSize:preferredSize
                          showsCloseButton:YES];
    windowView.workspaceToolIdentifier = toolIdentifier;
    if ([toolIdentifier isEqualToString:ISHWorkspaceToolClockIdentifier]) {
        windowView.resizable = YES;
        windowView.minimumSize = CGSizeMake(200, 140);
    } else if ([toolIdentifier isEqualToString:ISHWorkspaceToolInfoIdentifier]) {
        windowView.resizable = YES;
        windowView.minimumSize = CGSizeMake(260, 160);
    } else if ([toolIdentifier isEqualToString:ISHWorkspaceToolMonitorIdentifier]) {
        windowView.resizable = YES;
        windowView.minimumSize = CGSizeMake(280, 144);
    } else if ([toolIdentifier isEqualToString:ISHWorkspaceToolNetworksIdentifier]) {
        windowView.resizable = YES;
        windowView.minimumSize = CGSizeMake(320, 180);
    }
    [self attachViewController:viewController toDesktopWindow:windowView];
    [self refreshDockButtons];
    return windowView;
}

- (void)updateDashboardUtilityButton {
    if (self.dashboardWindow == nil)
        return;

    __weak typeof(self) weakSelf = self;
    NSString *title = self.dashboardIsCompact ? @"Full" : @"Mini";
    [self.dashboardWindow setUtilityButtonTitle:title handler:^{
        [weakSelf toggleDashboardCompactMode];
    }];
}

- (void)toggleDashboardCompactMode {
    ISHWorkspaceContainedWindowView *dashboardWindow = self.dashboardWindow;
    if (dashboardWindow == nil)
        return;

    if (self.dashboardIsCompact) {
        self.dashboardIsCompact = NO;
        [self updateDashboardUtilityButton];
        [self resizeDesktopWindow:dashboardWindow
                           toSize:self.dashboardExpandedSize
                         animated:YES];
        return;
    }

    self.dashboardExpandedSize = dashboardWindow.bounds.size;
    self.dashboardIsCompact = YES;
    [self updateDashboardUtilityButton];
    [self resizeDesktopWindow:dashboardWindow
                       toSize:ISHWorkspaceCompactDashboardSize()
                     animated:YES];
}

- (void)openDashboardWindow:(id)sender {
    if (self.dashboardWindow != nil) {
        self.dashboardWindow.hidden = NO;
        [self focusDesktopWindow:self.dashboardWindow];
    }
}

- (ISHWorkspaceContainedWindowView *)createDockWindow {
    CGSize preferredSize = ISHWorkspacePreferredDockContentSize();
    ISHWorkspaceContainedWindowView *windowView =
        [self createDesktopWindowWithTitle:@"Dock"
                             preferredSize:preferredSize
                          showsCloseButton:NO];
    self.dockWindow = windowView;
    windowView.draggable = NO;
    windowView.resizable = YES;
    windowView.resizeHandleAtTopRight = YES;
    windowView.minimumSize = CGSizeMake(250, 150);
    windowView.maximumSize = CGSizeMake(520, 280);
    windowView.pinnedToLowerRight = YES;

    UIStackView *stack = [UIStackView new];
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 10;
    [windowView.contentContainerView addSubview:stack];

    UIStackView *appsRow = [UIStackView new];
    appsRow.axis = UILayoutConstraintAxisHorizontal;
    appsRow.spacing = 10;
    appsRow.distribution = UIStackViewDistributionFillEqually;

    self.dockDashboardButton = [self workspaceDockTileButtonWithTitle:@"Dashboard"
                                                             selector:@selector(openOrFocusDashboardFromDock:)
                                                           identifier:@"dashboard"];
    self.dockUtilsButton = [self workspaceDockTileButtonWithTitle:@"Utils"
                                                         selector:@selector(toggleClockFromDock:)
                                                       identifier:@"utils"];
    UILongPressGestureRecognizer *utilsLongPressRecognizer =
        [[UILongPressGestureRecognizer alloc] initWithTarget:self action:@selector(handleUtilsDockLongPress:)];
    [self.dockUtilsButton addGestureRecognizer:utilsLongPressRecognizer];
    self.dockTerminalButton = [self workspaceDockTileButtonWithTitle:@"Terminal"
                                                            selector:@selector(openOrFocusTerminalFromDock:)
                                                          identifier:ISHWorkspaceTerminalRoleSessionShell];
    UILongPressGestureRecognizer *terminalLongPressRecognizer =
        [[UILongPressGestureRecognizer alloc] initWithTarget:self action:@selector(handleTerminalDockLongPress:)];
    [self.dockTerminalButton addGestureRecognizer:terminalLongPressRecognizer];

    [appsRow addArrangedSubview:self.dockDashboardButton];
    [appsRow addArrangedSubview:self.dockUtilsButton];
    [appsRow addArrangedSubview:self.dockTerminalButton];

    [stack addArrangedSubview:appsRow];

    [NSLayoutConstraint activateConstraints:@[
        [stack.topAnchor constraintEqualToAnchor:windowView.contentContainerView.topAnchor constant:14],
        [stack.leadingAnchor constraintEqualToAnchor:windowView.contentContainerView.leadingAnchor constant:16],
        [stack.trailingAnchor constraintEqualToAnchor:windowView.contentContainerView.trailingAnchor constant:-16],
        [stack.bottomAnchor constraintEqualToAnchor:windowView.contentContainerView.bottomAnchor constant:-14],
    ]];

    [self pinDesktopWindowToLowerRight:windowView];
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
    [NSUserDefaults.standardUserDefaults setObject:layout forKey:ISHWorkspaceSavedLayoutDefaultsKey];
}

- (void)restoreWorkspaceLayout:(id)sender {
    NSArray<NSDictionary<NSString *, id> *> *layout =
        [NSUserDefaults.standardUserDefaults arrayForKey:ISHWorkspaceSavedLayoutDefaultsKey];
    if (![layout isKindOfClass:NSArray.class] || layout.count == 0) {
        UIAlertController *alert =
            [UIAlertController alertControllerWithTitle:@"No Saved Layout"
                                                message:@"Save a workspace arrangement first, then restore it from here."
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
    if (dockDescriptor != nil && self.dockWindow != nil) {
        CGSize savedDockSize = ISHWorkspaceSizeFromDescriptor(dockDescriptor[@"size"]);
        if (savedDockSize.width > 0 && savedDockSize.height > 0) {
            [self resizeDesktopWindow:self.dockWindow toSize:savedDockSize animated:NO];
        } else {
            [self pinDesktopWindowToLowerRight:self.dockWindow];
        }
    }

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
            ISHWorkspaceContainedWindowView *windowView = [self openWorkspaceToolWindowWithIdentifier:toolIdentifier];
            [self applySavedFrameDescriptor:frameDescriptor
                                   toWindow:windowView
                               fallbackSize:ISHWorkspacePreferredToolContentSize(toolIdentifier)];
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
            }
        }
    }

    [self refreshWorkspaceStatus];
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
        if (windowView.hidden)
            continue;
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
    [self refreshDockButtons];
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
    ISHWorkspaceContainedWindowView *windowView =
        [self createDesktopWindowWithTitle:title
                             preferredSize:preferredSize
                          showsCloseButton:YES];
    windowView.hostedTerminalViewController = terminalViewController;
    windowView.resizable = YES;
    windowView.minimumSize = CGSizeMake(520, 340);
    [self attachViewController:terminalViewController toDesktopWindow:windowView];
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
    [self workspaceClearArrangedSubviewsFromStack:self.leadingColumnStack];
    [self workspaceClearArrangedSubviewsFromStack:self.trailingColumnStack];

    BOOL useTwoColumns = self.traitCollection.horizontalSizeClass == UIUserInterfaceSizeClassRegular;
    self.bodyStack.axis = useTwoColumns ? UILayoutConstraintAxisHorizontal : UILayoutConstraintAxisVertical;
    self.bodyStack.spacing = 20;
    self.bodyStack.alignment = UIStackViewAlignmentFill;
    self.bodyStack.distribution = useTwoColumns ? UIStackViewDistributionFillEqually : UIStackViewDistributionFill;

    if (useTwoColumns) {
        [self.leadingColumnStack addArrangedSubview:self.actionsCard];
        [self.leadingColumnStack addArrangedSubview:self.toolsCard];
        [self.leadingColumnStack addArrangedSubview:self.eventsCard];
        [self.trailingColumnStack addArrangedSubview:self.windowCard];
        [self.trailingColumnStack addArrangedSubview:self.systemCard];
        [self.trailingColumnStack addArrangedSubview:self.networkCard];
        [self.trailingColumnStack addArrangedSubview:self.terminalsCard];
        [self.bodyStack addArrangedSubview:self.leadingColumnStack];
        [self.bodyStack addArrangedSubview:self.trailingColumnStack];
    } else {
        [self.bodyStack addArrangedSubview:self.actionsCard];
        [self.bodyStack addArrangedSubview:self.toolsCard];
        [self.bodyStack addArrangedSubview:self.windowCard];
        [self.bodyStack addArrangedSubview:self.systemCard];
        [self.bodyStack addArrangedSubview:self.networkCard];
        [self.bodyStack addArrangedSubview:self.terminalsCard];
        [self.bodyStack addArrangedSubview:self.eventsCard];
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

    self.desktopSurfaceView = [UIView new];
    self.desktopSurfaceView.translatesAutoresizingMaskIntoConstraints = NO;
    if (@available(iOS 13.0, *)) {
        self.desktopSurfaceView.backgroundColor = [UIColor.systemGroupedBackgroundColor colorWithAlphaComponent:1.0];
    } else {
        self.desktopSurfaceView.backgroundColor = [UIColor colorWithWhite:0.92 alpha:1.0];
    }
    [self.view addSubview:self.desktopSurfaceView];

    [NSLayoutConstraint activateConstraints:@[
        [self.desktopSurfaceView.topAnchor constraintEqualToAnchor:self.view.topAnchor],
        [self.desktopSurfaceView.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
        [self.desktopSurfaceView.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
        [self.desktopSurfaceView.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor],
    ]];

    self.timeFormatter = [NSDateFormatter new];
    self.timeFormatter.dateStyle = NSDateFormatterMediumStyle;
    self.timeFormatter.timeStyle = NSDateFormatterMediumStyle;

    ISHWorkspaceContainedWindowView *dashboardWindow =
        [self createDesktopWindowWithTitle:@"Dashboard"
                             preferredSize:CGSizeMake(960, 760)
                          showsCloseButton:YES];
    self.dashboardWindow = dashboardWindow;
    self.dashboardExpandedSize = dashboardWindow.preferredSize;
    self.dashboardIsCompact = NO;
    dashboardWindow.draggable = YES;
    dashboardWindow.resizable = YES;
    dashboardWindow.minimumSize = CGSizeMake(420, 96);
    __weak typeof(self) weakSelf = self;
    dashboardWindow.closeHandler = ^{
        dashboardWindow.hidden = YES;
        [weakSelf refreshDockButtons];
    };
    [self updateDashboardUtilityButton];
    [self createDockWindow];

    UIScrollView *scrollView = [UIScrollView new];
    scrollView.translatesAutoresizingMaskIntoConstraints = NO;
    [dashboardWindow.contentContainerView addSubview:scrollView];

    UIStackView *contentStack = [UIStackView new];
    contentStack.axis = UILayoutConstraintAxisVertical;
    contentStack.spacing = 20;
    contentStack.translatesAutoresizingMaskIntoConstraints = NO;
    [scrollView addSubview:contentStack];

    self.clockLabel = [self workspaceLabelWithTextStyle:UIFontTextStyleTitle1 monospaced:YES];
    self.batteryLabel = [self workspaceLabelWithTextStyle:UIFontTextStyleBody monospaced:NO];
    self.rootLabel = [self workspaceLabelWithTextStyle:UIFontTextStyleBody monospaced:NO];
    self.storageLabel = [self workspaceLabelWithTextStyle:UIFontTextStyleBody monospaced:NO];
    self.startupPreferenceLabel = [self workspaceLabelWithTextStyle:UIFontTextStyleBody monospaced:NO];
    self.windowSummaryLabel = [self workspaceLabelWithTextStyle:UIFontTextStyleBody monospaced:NO];
    self.systemSummaryLabel = [self workspaceLabelWithTextStyle:UIFontTextStyleFootnote monospaced:NO];
    self.networkSummaryLabel = [self workspaceLabelWithTextStyle:UIFontTextStyleFootnote monospaced:NO];
    self.diagnosticsSummaryLabel = [self workspaceLabelWithTextStyle:UIFontTextStyleFootnote monospaced:NO];
    self.sceneWindowsStack = [UIStackView new];
    self.sceneWindowsStack.axis = UILayoutConstraintAxisVertical;
    self.sceneWindowsStack.spacing = 10;
    self.sceneWindowsStack.translatesAutoresizingMaskIntoConstraints = NO;
    self.breadcrumbsLabel = [self workspaceLabelWithTextStyle:UIFontTextStyleFootnote monospaced:NO];
    self.summaryLabel = [self workspaceLabelWithTextStyle:UIFontTextStyleFootnote monospaced:NO];
    self.summaryLabel.text = @"Widgets and layout stay native ARM. Terminal sessions remain guest-backed, and you can launch either console-focused or session-focused terminals from here.";

    self.activeTerminalsStack = [UIStackView new];
    self.activeTerminalsStack.axis = UILayoutConstraintAxisVertical;
    self.activeTerminalsStack.spacing = 10;
    self.activeTerminalsStack.translatesAutoresizingMaskIntoConstraints = NO;
    UIStackView *statusStack = nil;
    self.statusCard = [self workspaceCardWithContentStack:&statusStack];
    UILabel *headlineLabel = [self workspaceSectionTitle:@"Native workspace"];
    UILabel *statusSummaryLabel = [self workspaceLabelWithTextStyle:UIFontTextStyleSubheadline monospaced:NO];
    if (@available(iOS 13.0, *)) {
        statusSummaryLabel.textColor = UIColor.secondaryLabelColor;
    } else {
        statusSummaryLabel.textColor = UIColor.darkGrayColor;
    }
    statusSummaryLabel.text = @"A native ARM dashboard for windows, terminals, and support surfaces.";
    [statusStack addArrangedSubview:headlineLabel];
    [statusStack addArrangedSubview:self.clockLabel];
    [statusStack addArrangedSubview:statusSummaryLabel];
    [statusStack addArrangedSubview:self.batteryLabel];
    [statusStack addArrangedSubview:self.rootLabel];
    [statusStack addArrangedSubview:self.storageLabel];
    [statusStack addArrangedSubview:self.startupPreferenceLabel];

    UIStackView *actionsStack = nil;
    self.actionsCard = [self workspaceCardWithContentStack:&actionsStack];
    [actionsStack addArrangedSubview:[self workspaceSectionTitle:@"Terminals"]];
    [actionsStack addArrangedSubview:[self workspaceActionButtonWithTitle:@"Open System Console Here"
                                                                selector:@selector(openSystemConsoleHere:)]];
    [actionsStack addArrangedSubview:[self workspaceActionButtonWithTitle:@"Open Session Shell Here"
                                                                selector:@selector(openSessionShellHere:)]];
    [actionsStack addArrangedSubview:[self workspaceActionButtonWithTitle:@"Open Preferred Terminal Here"
                                                                selector:@selector(openTerminalHere:)]];

    UIStackView *toolsStack = nil;
    self.toolsCard = [self workspaceCardWithContentStack:&toolsStack];
    [toolsStack addArrangedSubview:[self workspaceSectionTitle:@"Native apps"]];
    [toolsStack addArrangedSubview:[self workspaceToolLauncherRowWithTitle:@"Clock"
                                                                  subtitle:@"Large clock view"
                                                            toolIdentifier:ISHWorkspaceToolClockIdentifier]];
    [toolsStack addArrangedSubview:[self workspaceToolLauncherRowWithTitle:@"Info"
                                                                  subtitle:@"Battery, root, and storage"
                                                            toolIdentifier:ISHWorkspaceToolInfoIdentifier]];
    [toolsStack addArrangedSubview:[self workspaceToolLauncherRowWithTitle:@"Monitor"
                                                                  subtitle:@"Compact live CPU, memory, and session monitor"
                                                            toolIdentifier:ISHWorkspaceToolMonitorIdentifier]];
    [toolsStack addArrangedSubview:[self workspaceToolLauncherRowWithTitle:@"Networks"
                                                                  subtitle:@"Interface and address summary"
                                                            toolIdentifier:ISHWorkspaceToolNetworksIdentifier]];
    [toolsStack addArrangedSubview:[self workspaceToolLauncherRowWithTitle:@"System Status"
                                                                  subtitle:@"Device, root, and session summary"
                                                            toolIdentifier:ISHWorkspaceToolStatusIdentifier]];
    [toolsStack addArrangedSubview:[self workspaceToolLauncherRowWithTitle:@"Filesystems"
                                                                  subtitle:@"Manage installed roots"
                                                            toolIdentifier:ISHWorkspaceToolFilesystemsIdentifier]];
    [toolsStack addArrangedSubview:[self workspaceToolLauncherRowWithTitle:@"Settings"
                                                                  subtitle:@"App configuration and preferences"
                                                            toolIdentifier:ISHWorkspaceToolSettingsIdentifier]];
    [toolsStack addArrangedSubview:[self workspaceToolLauncherRowWithTitle:@"Diagnostics"
                                                                  subtitle:@"Crash, MetricKit, and breadcrumb data"
                                                            toolIdentifier:ISHWorkspaceToolDiagnosticsIdentifier]];

    UIStackView *windowCardStack = nil;
    self.windowCard = [self workspaceCardWithContentStack:&windowCardStack];
    [windowCardStack addArrangedSubview:[self workspaceSectionTitle:@"Window overview"]];
    [windowCardStack addArrangedSubview:self.windowSummaryLabel];
    [windowCardStack addArrangedSubview:self.sceneWindowsStack];
    [windowCardStack addArrangedSubview:[self workspaceActionButtonWithTitle:@"Save Current Layout"
                                                                   selector:@selector(saveWorkspaceLayout:)]];
    [windowCardStack addArrangedSubview:[self workspaceActionButtonWithTitle:@"Restore Saved Layout"
                                                                   selector:@selector(restoreWorkspaceLayout:)]];
    [windowCardStack addArrangedSubview:[self workspaceActionButtonWithTitle:@"New Terminal Window"
                                                                   selector:@selector(openNewTerminalWindow:)]];
    [windowCardStack addArrangedSubview:[self workspaceActionButtonWithTitle:@"New Workspace Window"
                                                                   selector:@selector(openNewWorkspaceWindow:)]];

    UIStackView *systemCardStack = nil;
    self.systemCard = [self workspaceCardWithContentStack:&systemCardStack];
    [systemCardStack addArrangedSubview:[self workspaceSectionTitle:@"System snapshot"]];
    [systemCardStack addArrangedSubview:self.systemSummaryLabel];
    [systemCardStack addArrangedSubview:self.diagnosticsSummaryLabel];

    UIStackView *networkCardStack = nil;
    self.networkCard = [self workspaceCardWithContentStack:&networkCardStack];
    [networkCardStack addArrangedSubview:[self workspaceSectionTitle:@"Network"]];
    [networkCardStack addArrangedSubview:self.networkSummaryLabel];

    UIStackView *terminalsCardStack = nil;
    self.terminalsCard = [self workspaceCardWithContentStack:&terminalsCardStack];
    [terminalsCardStack addArrangedSubview:[self workspaceSectionTitle:@"Active terminals"]];
    [terminalsCardStack addArrangedSubview:self.activeTerminalsStack];

    UIStackView *eventsCardStack = nil;
    self.eventsCard = [self workspaceCardWithContentStack:&eventsCardStack];
    [eventsCardStack addArrangedSubview:[self workspaceSectionTitle:@"Recent events"]];
    [eventsCardStack addArrangedSubview:self.breadcrumbsLabel];
    [eventsCardStack addArrangedSubview:self.summaryLabel];

    self.bodyStack = [UIStackView new];
    self.bodyStack.translatesAutoresizingMaskIntoConstraints = NO;
    self.leadingColumnStack = [UIStackView new];
    self.leadingColumnStack.translatesAutoresizingMaskIntoConstraints = NO;
    self.leadingColumnStack.axis = UILayoutConstraintAxisVertical;
    self.leadingColumnStack.spacing = 20;
    self.trailingColumnStack = [UIStackView new];
    self.trailingColumnStack.translatesAutoresizingMaskIntoConstraints = NO;
    self.trailingColumnStack.axis = UILayoutConstraintAxisVertical;
    self.trailingColumnStack.spacing = 20;

    [contentStack addArrangedSubview:self.statusCard];
    [contentStack addArrangedSubview:self.bodyStack];
    [self rebuildWorkspaceColumns];

    UILayoutGuide *safeArea = dashboardWindow.contentContainerView.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [scrollView.topAnchor constraintEqualToAnchor:safeArea.topAnchor],
        [scrollView.leadingAnchor constraintEqualToAnchor:safeArea.leadingAnchor],
        [scrollView.trailingAnchor constraintEqualToAnchor:safeArea.trailingAnchor],
        [scrollView.bottomAnchor constraintEqualToAnchor:safeArea.bottomAnchor],

        [contentStack.topAnchor constraintEqualToAnchor:scrollView.topAnchor constant:24],
        [contentStack.leadingAnchor constraintEqualToAnchor:scrollView.leadingAnchor constant:20],
        [contentStack.trailingAnchor constraintEqualToAnchor:scrollView.trailingAnchor constant:-20],
        [contentStack.bottomAnchor constraintEqualToAnchor:scrollView.bottomAnchor constant:-24],
        [contentStack.widthAnchor constraintEqualToAnchor:scrollView.widthAnchor constant:-40],
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
                                           selector:@selector(refreshWorkspaceStatus)
                                               name:ISHDiagnosticsStoreDidUpdateNotification
                                             object:nil];
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(refreshWorkspaceStatus)
                                               name:NSUserDefaultsDidChangeNotification
                                             object:nil];
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(refreshWorkspaceStatus)
                                               name:UIDeviceBatteryLevelDidChangeNotification
                                             object:nil];
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(refreshWorkspaceStatus)
                                               name:UIDeviceBatteryStateDidChangeNotification
                                             object:nil];
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(refreshWorkspaceStatus)
                                               name:UIApplicationDidBecomeActiveNotification
                                             object:nil];
    if (@available(iOS 13.0, *)) {
        [NSNotificationCenter.defaultCenter addObserver:self
                                               selector:@selector(refreshWorkspaceStatus)
                                                   name:UISceneDidActivateNotification
                                                 object:nil];
        [NSNotificationCenter.defaultCenter addObserver:self
                                               selector:@selector(refreshWorkspaceStatus)
                                                   name:UISceneDidDisconnectNotification
                                                 object:nil];
    }

    [self refreshWorkspaceStatus];
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
    for (UIView *view in self.desktopWindows) {
        if (![view isKindOfClass:ISHWorkspaceContainedWindowView.class])
            continue;
        ISHWorkspaceContainedWindowView *windowView = (ISHWorkspaceContainedWindowView *) view;
        [self applyInitialFrameIfNeededToDesktopWindow:windowView];
        [self clampDesktopWindowToVisibleBounds:windowView];
        if (windowView.pinnedToLowerRight)
            [self pinDesktopWindowToLowerRight:windowView];
    }
}

- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:self];
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    UIDevice.currentDevice.batteryMonitoringEnabled = YES;
    [self refreshWorkspaceStatus];
    [self startClock];
}

- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    if (self.didOpenInitialTool || self.initialToolIdentifier.length == 0)
        return;
    self.didOpenInitialTool = YES;
    [self openWorkspaceToolWithIdentifier:self.initialToolIdentifier];
}

- (void)viewDidDisappear:(BOOL)animated {
    [super viewDidDisappear:animated];
    [self.clockTimer invalidate];
    self.clockTimer = nil;
    UIDevice.currentDevice.batteryMonitoringEnabled = NO;
}

- (void)startClock {
    [self.clockTimer invalidate];
    self.clockTimer = [NSTimer scheduledTimerWithTimeInterval:1
                                                       target:self
                                                     selector:@selector(refreshWorkspaceStatus)
                                                     userInfo:nil
                                                      repeats:YES];
}

- (void)refreshWorkspaceStatus {
    if (!NSThread.isMainThread) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self refreshWorkspaceStatus];
        });
        return;
    }

    self.clockLabel.text = [self.timeFormatter stringFromDate:NSDate.date];
    if (UIDevice.currentDevice.batteryState == UIDeviceBatteryStateUnknown || UIDevice.currentDevice.batteryLevel < 0) {
        self.batteryLabel.text = @"Battery: unavailable";
    } else {
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
        self.batteryLabel.text = [NSString stringWithFormat:@"Battery: %@ (%ld%%)", stateDescription, (long) percent];
    }
    NSString *defaultRoot = Roots.instance.defaultRoot;
    self.rootLabel.text = defaultRoot.length > 0
        ? [NSString stringWithFormat:@"Current root: %@", defaultRoot]
        : @"Current root: unavailable";

    NSDictionary<NSFileAttributeKey, id> *attributes =
        [NSFileManager.defaultManager attributesOfFileSystemForPath:NSHomeDirectory() error:nil];
    NSNumber *freeSize = attributes[NSFileSystemFreeSize];
    if (freeSize != nil) {
        NSString *formattedSize = [NSByteCountFormatter stringFromByteCount:freeSize.longLongValue
                                                                  countStyle:NSByteCountFormatterCountStyleFile];
        self.storageLabel.text = [NSString stringWithFormat:@"Free storage: %@", formattedSize];
    } else {
        self.storageLabel.text = @"Free storage: unavailable";
    }
    self.startupPreferenceLabel.text = [NSString stringWithFormat:@"Startup screen: %@", ISHInitialWindowTitle()];
    [self refreshWindowSummary];
    [self refreshSceneWindows];
    [self refreshSystemSummary];
    self.networkSummaryLabel.text = ISHWorkspaceNetworkSummaryText();

    NSArray<NSDictionary<NSString *, id> *> *breadcrumbs = [ISHDiagnosticsStore recentBreadcrumbsWithLimit:3];
    if (breadcrumbs.count == 0) {
        self.breadcrumbsLabel.text = @"Recent events: none";
    } else {
        NSMutableArray<NSString *> *lines = [NSMutableArray array];
        for (NSDictionary<NSString *, id> *entry in breadcrumbs) {
            NSString *event = entry[@"event"] ?: @"event";
            NSString *timestamp = entry[@"timestamp"] ?: @"";
            [lines addObject:[NSString stringWithFormat:@"%@  %@", timestamp, event]];
        }
        self.breadcrumbsLabel.text = [NSString stringWithFormat:@"Recent events:\n%@", [lines componentsJoinedByString:@"\n"]];
    }
    [self refreshActiveTerminals];
    [self refreshDockButtons];
}

- (void)refreshSceneWindows {
    for (UIView *subview in self.sceneWindowsStack.arrangedSubviews) {
        [self.sceneWindowsStack removeArrangedSubview:subview];
        [subview removeFromSuperview];
    }

    if (@available(iOS 13.0, *)) {
        UIWindowScene *currentWindowScene = self.view.window.windowScene;
        NSArray<UIScene *> *connectedScenes =
            [UIApplication.sharedApplication.connectedScenes.allObjects sortedArrayUsingComparator:^NSComparisonResult(UIScene *left, UIScene *right) {
            if (left == currentWindowScene)
                return NSOrderedAscending;
            if (right == currentWindowScene)
                return NSOrderedDescending;
            return [left.session.persistentIdentifier compare:right.session.persistentIdentifier];
        }];

        if (connectedScenes.count == 0) {
            UILabel *emptyLabel = [self workspaceLabelWithTextStyle:UIFontTextStyleFootnote monospaced:NO];
            emptyLabel.text = @"No live windows detected.";
            [self.sceneWindowsStack addArrangedSubview:emptyLabel];
            return;
        }

        for (UIScene *scene in connectedScenes) {
            UIStackView *row = [UIStackView new];
            row.axis = UILayoutConstraintAxisHorizontal;
            row.spacing = 10;
            row.alignment = UIStackViewAlignmentCenter;

            UILabel *label = [self workspaceLabelWithTextStyle:UIFontTextStyleFootnote monospaced:NO];
            NSString *role = ISHWorkspaceSceneRoleDescription(scene.session);
            NSString *state = ISHWorkspaceSceneActivationDescription(scene);
            NSString *identifier = scene.session.persistentIdentifier ?: @"";
            if (identifier.length > 8)
                identifier = [identifier substringFromIndex:identifier.length - 8];

            NSMutableArray<NSString *> *parts = [NSMutableArray arrayWithObjects:role, state, nil];
            NSString *terminalUUID = scene.session.stateRestorationActivity.userInfo[ISHSceneTerminalUUIDUserInfoKey];
            if (terminalUUID.length > 0) {
                Terminal *terminal = [Terminal terminalWithUUID:[[NSUUID alloc] initWithUUIDString:terminalUUID]];
                NSString *terminalLabel = terminal != nil ? ISHWorkspaceTerminalDisplayName(terminal) : @"Detached terminal";
                [parts addObject:terminalLabel];
            }
            NSString *currentMarker = scene == currentWindowScene ? @"Current window" : [NSString stringWithFormat:@"Scene %@", identifier];
            label.text = [NSString stringWithFormat:@"%@\n%@", currentMarker, [parts componentsJoinedByString:@"  |  "]];

            UIButton *focusButton = [UIButton buttonWithType:UIButtonTypeSystem];
            focusButton.titleLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleSubheadline];
            focusButton.accessibilityIdentifier = scene.session.persistentIdentifier;
            [focusButton setTitle:(scene == currentWindowScene ? @"Here" : @"Focus") forState:UIControlStateNormal];
            focusButton.enabled = scene != currentWindowScene;
            [focusButton addTarget:self action:@selector(focusExistingSceneFromButton:) forControlEvents:UIControlEventTouchUpInside];

            [row addArrangedSubview:label];
            [row addArrangedSubview:focusButton];
            [label setContentHuggingPriority:UILayoutPriorityDefaultLow forAxis:UILayoutConstraintAxisHorizontal];
            [label setContentCompressionResistancePriority:UILayoutPriorityDefaultLow forAxis:UILayoutConstraintAxisHorizontal];
            [focusButton setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
            [self.sceneWindowsStack addArrangedSubview:row];
        }
        return;
    } else {
        UILabel *legacyLabel = [self workspaceLabelWithTextStyle:UIFontTextStyleFootnote monospaced:NO];
        legacyLabel.text = @"Live window enumeration requires iOS 13 scene APIs.";
        [self.sceneWindowsStack addArrangedSubview:legacyLabel];
        return;
    }
}

- (void)refreshWindowSummary {
    NSMutableArray<NSString *> *lines = [NSMutableArray array];
    NSUInteger terminalCount = [Terminal activeTerminals].count;
    [lines addObject:[NSString stringWithFormat:@"Guest terminals: %lu", (unsigned long) terminalCount]];

    if (@available(iOS 13.0, *)) {
        NSUInteger workspaceSceneCount = 0;
        NSUInteger terminalSceneCount = 0;
        for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
            if (![scene isKindOfClass:UIScene.class])
                continue;
            NSString *role = ISHWorkspaceSceneRoleDescription(scene.session);
            if ([role isEqualToString:@"Workspace"]) {
                workspaceSceneCount += 1;
            } else if ([role isEqualToString:@"Terminal"]) {
                terminalSceneCount += 1;
            }
        }
        [lines addObject:[NSString stringWithFormat:@"Workspace windows: %lu", (unsigned long) workspaceSceneCount]];
        [lines addObject:[NSString stringWithFormat:@"Terminal windows: %lu", (unsigned long) terminalSceneCount]];
        NSString *currentRole = self.view.window.windowScene != nil
            ? ISHWorkspaceSceneRoleDescription(self.view.window.windowScene.session)
            : @"Unknown";
        [lines addObject:[NSString stringWithFormat:@"Current window: %@", currentRole]];
    } else {
        [lines addObject:@"Workspace windows: 1"];
        [lines addObject:@"Terminal windows: 1"];
        [lines addObject:@"Current window: Workspace"];
    }

    self.windowSummaryLabel.text = [lines componentsJoinedByString:@"\n"];
}

- (void)refreshSystemSummary {
    NSMutableArray<NSString *> *systemLines = [NSMutableArray array];
    NSString *version = [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"] ?: @"?";
    NSString *build = [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleVersion"] ?: @"?";
    [systemLines addObject:[NSString stringWithFormat:@"App: %@ (%@)", version, build]];
    [systemLines addObject:[NSString stringWithFormat:@"Device: %@ / iOS %@",
                            UIDevice.currentDevice.model ?: @"Unknown",
                            UIDevice.currentDevice.systemVersion ?: @"?"]];
    [systemLines addObject:[NSString stringWithFormat:@"Installed roots: %lu",
                            (unsigned long) Roots.instance.roots.count]];
    self.systemSummaryLabel.text = [systemLines componentsJoinedByString:@"\n"];

    NSArray<NSDictionary<NSString *, id> *> *payloads = [ISHDiagnosticsStore recentMetricKitPayloadsWithLimit:2];
    if (payloads.count == 0) {
        self.diagnosticsSummaryLabel.text = @"Diagnostics: no recent MetricKit payloads";
        return;
    }

    NSDictionary<NSString *, id> *latestPayload = payloads.firstObject;
    NSString *filename = latestPayload[@"filename"] ?: @"payload.json";
    NSString *receivedAt = latestPayload[@"receivedAt"] ?: @"recently";
    NSArray<NSDictionary<NSString *, id> *> *summaries = latestPayload[@"summaries"];
    NSString *topSummary = @"no summaries";
    if ([summaries isKindOfClass:NSArray.class] && summaries.count > 0) {
        NSDictionary<NSString *, id> *entry = summaries.firstObject;
        NSString *kind = entry[@"kind"] ?: @"diagnostic";
        NSString *signal = entry[@"signal"] ?: @"";
        topSummary = signal.length > 0 ? [NSString stringWithFormat:@"%@ / signal %@", kind, signal] : kind;
    }
    self.diagnosticsSummaryLabel.text =
        [NSString stringWithFormat:@"Diagnostics: %lu recent payload%@\nLatest: %@ (%@)\nTop summary: %@",
                                   (unsigned long) payloads.count,
                                   payloads.count == 1 ? @"" : @"s",
                                   filename,
                                   receivedAt,
                                   topSummary];
}

- (void)refreshActiveTerminals {
    for (UIView *subview in self.activeTerminalsStack.arrangedSubviews) {
        [self.activeTerminalsStack removeArrangedSubview:subview];
        [subview removeFromSuperview];
    }

    NSArray<Terminal *> *activeTerminals = [Terminal activeTerminals];
    if (activeTerminals.count == 0) {
        UILabel *emptyLabel = [UILabel new];
        emptyLabel.numberOfLines = 0;
        emptyLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleSubheadline];
        if (@available(iOS 13.0, *)) {
            emptyLabel.textColor = UIColor.secondaryLabelColor;
        } else {
            emptyLabel.textColor = UIColor.darkGrayColor;
        }
        emptyLabel.text = @"No terminals are active yet.";
        [self.activeTerminalsStack addArrangedSubview:emptyLabel];
        return;
    }

    for (Terminal *terminal in activeTerminals) {
        UIStackView *row = [UIStackView new];
        row.axis = UILayoutConstraintAxisHorizontal;
        row.spacing = 10;
        row.alignment = UIStackViewAlignmentCenter;

        UILabel *label = [UILabel new];
        label.numberOfLines = 0;
        label.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
        if (@available(iOS 13.0, *)) {
            label.textColor = UIColor.labelColor;
        } else {
            label.textColor = UIColor.blackColor;
        }
        label.text = ISHWorkspaceTerminalDisplayName(terminal);

        UIButton *hereButton = [self terminalActionButtonWithTitle:@"Here"
                                                          selector:@selector(openExistingTerminalHereFromButton:)
                                                      terminalUUID:terminal.uuid];
        UIButton *windowButton = [self terminalActionButtonWithTitle:@"Window"
                                                            selector:@selector(openExistingTerminalInNewWindowFromButton:)
                                                        terminalUUID:terminal.uuid];

        [row addArrangedSubview:label];
        [row addArrangedSubview:hereButton];
        [row addArrangedSubview:windowButton];
        [label setContentHuggingPriority:UILayoutPriorityDefaultLow forAxis:UILayoutConstraintAxisHorizontal];
        [label setContentCompressionResistancePriority:UILayoutPriorityDefaultLow forAxis:UILayoutConstraintAxisHorizontal];
        [hereButton setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
        [windowButton setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];

        [self.activeTerminalsStack addArrangedSubview:row];
    }
}

- (void)refreshDockButtons {
    ISHWorkspaceContainedWindowView *frontmostWindow = [self frontmostDesktopWindowExcludingDock];

    BOOL dashboardVisible = self.dashboardWindow != nil && !self.dashboardWindow.hidden;
    NSString *dashboardState = dashboardVisible
        ? (frontmostWindow == self.dashboardWindow ? @"Front" : @"Focus")
        : @"Show";
    [self configureDockTileButton:self.dockDashboardButton
                            title:@"Dashboard"
                            state:dashboardState
                           active:dashboardVisible
                        frontmost:frontmostWindow == self.dashboardWindow];

    ISHWorkspaceContainedWindowView *clockWindow = [self desktopWindowForToolIdentifier:ISHWorkspaceToolClockIdentifier];
    [self configureDockTileButton:self.dockUtilsButton
                            title:@"Utils"
                            state:(clockWindow != nil ? @"Close Clock" : @"Open Clock")
                           active:clockWindow != nil
                        frontmost:frontmostWindow == clockWindow];

    ISHWorkspaceContainedWindowView *shellWindow = [self desktopWindowForTerminalRole:ISHWorkspaceTerminalRoleSessionShell];
    ISHWorkspaceContainedWindowView *frontmostTerminalWindow = [self frontmostDesktopTerminalWindow];
    BOOL hasTerminalWindow = frontmostTerminalWindow != nil;
    BOOL frontmostIsTerminal = frontmostTerminalWindow != nil && frontmostWindow == frontmostTerminalWindow;
    NSString *terminalState = @"Open Shell";
    if (shellWindow != nil) {
        terminalState = frontmostWindow == shellWindow ? @"Front" : @"Focus Shell";
    } else if (hasTerminalWindow) {
        terminalState = frontmostIsTerminal ? @"Front" : @"Focus";
    }
    [self configureDockTileButton:self.dockTerminalButton
                            title:@"Terminal"
                            state:terminalState
                           active:hasTerminalWindow
                        frontmost:frontmostIsTerminal];
}

- (UIButton *)terminalActionButtonWithTitle:(NSString *)title
                                   selector:(SEL)selector
                               terminalUUID:(NSUUID *)terminalUUID {
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    button.titleLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleSubheadline];
    button.accessibilityIdentifier = terminalUUID.UUIDString;
    [button setTitle:title forState:UIControlStateNormal];
    [button addTarget:self action:selector forControlEvents:UIControlEventTouchUpInside];
    return button;
}

- (void)openWorkspaceToolWithIdentifier:(NSString *)toolIdentifier {
    [self openWorkspaceToolWindowWithIdentifier:toolIdentifier];
}

- (void)openWorkspaceToolHereFromButton:(UIButton *)sender {
    NSString *toolIdentifier = sender.accessibilityIdentifier;
    if (toolIdentifier.length == 0)
        return;
    [self openWorkspaceToolWithIdentifier:toolIdentifier];
}

- (void)openWorkspaceToolWindowFromButton:(UIButton *)sender {
    NSString *toolIdentifier = sender.accessibilityIdentifier;
    if (toolIdentifier.length == 0) {
        [self presentSceneActivationError:nil title:@"Unable to open app window"];
        return;
    }
    [self requestSceneWithActivityType:ISHSceneActivityTypeWorkspace
                                 title:@"Unable to open app window"
                              userInfo:@{ISHSceneWorkspaceToolUserInfoKey: toolIdentifier}];
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

- (void)openOrFocusDashboardFromDock:(id)sender {
    [self openDashboardWindow:sender];
}

- (void)openOrFocusWorkspaceToolFromDock:(UIButton *)sender {
    NSString *toolIdentifier = sender.accessibilityIdentifier;
    if (toolIdentifier.length == 0)
        return;

    ISHWorkspaceContainedWindowView *existingWindow = [self desktopWindowForToolIdentifier:toolIdentifier];
    if (existingWindow != nil) {
        [self focusDesktopWindow:existingWindow];
        return;
    }
    [self openWorkspaceToolWithIdentifier:toolIdentifier];
}

- (void)openOrFocusTerminalFromDock:(UIButton *)sender {
    (void) sender;
    ISHWorkspaceContainedWindowView *shellWindow = [self desktopWindowForTerminalRole:ISHWorkspaceTerminalRoleSessionShell];
    if (shellWindow != nil) {
        [self focusDesktopWindow:shellWindow];
        return;
    }

    ISHWorkspaceContainedWindowView *frontmostTerminalWindow = [self frontmostDesktopTerminalWindow];
    if (frontmostTerminalWindow != nil) {
        [self focusDesktopWindow:frontmostTerminalWindow];
        return;
    }

    [self openTerminalHerePreferringConsole:NO];
}

- (NSArray<NSDictionary<NSString *, NSString *> *> *)dockUtilityToolDescriptors {
    return @[
        @{@"title": @"Clock", @"identifier": ISHWorkspaceToolClockIdentifier},
        @{@"title": @"Info", @"identifier": ISHWorkspaceToolInfoIdentifier},
        @{@"title": @"Monitor", @"identifier": ISHWorkspaceToolMonitorIdentifier},
        @{@"title": @"Networks", @"identifier": ISHWorkspaceToolNetworksIdentifier},
        @{@"title": @"System Status", @"identifier": ISHWorkspaceToolStatusIdentifier},
        @{@"title": @"Filesystems", @"identifier": ISHWorkspaceToolFilesystemsIdentifier},
        @{@"title": @"Settings", @"identifier": ISHWorkspaceToolSettingsIdentifier},
        @{@"title": @"Diagnostics", @"identifier": ISHWorkspaceToolDiagnosticsIdentifier},
    ];
}

- (void)handleUtilsDockLongPress:(UILongPressGestureRecognizer *)recognizer {
    if (recognizer.state != UIGestureRecognizerStateBegan)
        return;
    [self presentUtilsDockActionsFromView:recognizer.view];
}

- (void)presentUtilsDockActionsFromView:(UIView *)sourceView {
    UIAlertController *sheet =
        [UIAlertController alertControllerWithTitle:@"Native Apps"
                                            message:@"Open or focus a native workspace tool."
                                     preferredStyle:UIAlertControllerStyleActionSheet];

    for (NSDictionary<NSString *, NSString *> *descriptor in [self dockUtilityToolDescriptors]) {
        NSString *toolIdentifier = descriptor[@"identifier"];
        NSString *title = descriptor[@"title"];
        ISHWorkspaceContainedWindowView *existingWindow = [self desktopWindowForToolIdentifier:toolIdentifier];
        NSString *actionTitle = existingWindow != nil
            ? [NSString stringWithFormat:@"Focus %@", title]
            : [NSString stringWithFormat:@"Open %@", title];
        [sheet addAction:[UIAlertAction actionWithTitle:actionTitle
                                                  style:UIAlertActionStyleDefault
                                                handler:^(__unused UIAlertAction *action) {
            if (existingWindow != nil) {
                [self focusDesktopWindow:existingWindow];
            } else {
                [self openWorkspaceToolWithIdentifier:toolIdentifier];
            }
        }]];
    }

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

- (void)openClockTool:(id)sender {
    [self openWorkspaceToolWithIdentifier:ISHWorkspaceToolClockIdentifier];
}

- (void)openInfoTool:(id)sender {
    [self openWorkspaceToolWithIdentifier:ISHWorkspaceToolInfoIdentifier];
}

- (void)openMonitorTool:(id)sender {
    [self openWorkspaceToolWithIdentifier:ISHWorkspaceToolMonitorIdentifier];
}

- (void)openNetworksTool:(id)sender {
    [self openWorkspaceToolWithIdentifier:ISHWorkspaceToolNetworksIdentifier];
}

- (void)openSystemStatusTool:(id)sender {
    [self openWorkspaceToolWithIdentifier:ISHWorkspaceToolStatusIdentifier];
}

- (void)openFilesystems:(id)sender {
    [self openWorkspaceToolWithIdentifier:ISHWorkspaceToolFilesystemsIdentifier];
}

- (void)openSettings:(id)sender {
    [self openWorkspaceToolWithIdentifier:ISHWorkspaceToolSettingsIdentifier];
}

- (void)openSystemConsoleHere:(id)sender {
    [self openTerminalHerePreferringConsole:YES];
}

- (void)openSessionShellHere:(id)sender {
    [self openTerminalHerePreferringConsole:NO];
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

    NSString *title = preferConsole ? @"System Console" : @"Session Shell";
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
        windowView.titleLabel.text = ISHWorkspaceTerminalDisplayName(terminalViewController.terminal);
    }
    [self refreshDockButtons];
}

- (void)openTerminalHere:(id)sender {
    [self openTerminalHerePreferringConsole:[self shouldPreferConsoleForPreferredLaunch]];
}

- (BOOL)shouldPreferConsoleForPreferredLaunch {
    NSString *initialWindow = [NSUserDefaults.standardUserDefaults stringForKey:kPreferenceInitialWindowKey];
    return ![initialWindow isEqualToString:@"session-shell"];
}

- (void)openTerminalHerePreferringConsole:(BOOL)preferConsole {
    [self openDesktopTerminalHerePreferringConsole:preferConsole
                                     reuseExisting:YES
                                   trackPrimaryRole:YES];
}

- (void)openExistingTerminalHereFromButton:(UIButton *)sender {
    NSUUID *terminalUUID = [[NSUUID alloc] initWithUUIDString:sender.accessibilityIdentifier];
    if (terminalUUID == nil) {
        [self presentSceneActivationError:nil];
        return;
    }
    [self openExistingTerminalHereWithUUID:terminalUUID];
}

- (UISceneSession *)sceneSessionHostingTerminalUUID:(NSUUID *)terminalUUID {
    NSString *terminalUUIDString = terminalUUID.UUIDString;
    if (terminalUUIDString.length == 0)
        return nil;
    for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
        NSString *sceneTerminalUUID = scene.session.stateRestorationActivity.userInfo[ISHSceneTerminalUUIDUserInfoKey];
        if ([sceneTerminalUUID isEqualToString:terminalUUIDString])
            return scene.session;
    }
    return nil;
}

- (BOOL)focusSceneSession:(UISceneSession *)sceneSession title:(NSString *)title {
    if (sceneSession == nil)
        return NO;
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

- (void)focusExistingSceneFromButton:(UIButton *)sender {
    if (@available(iOS 13.0, *)) {
        NSString *identifier = sender.accessibilityIdentifier;
        if (identifier.length == 0) {
            [self presentSceneActivationError:nil title:@"Unable to focus window"];
            return;
        }
        UISceneSession *targetSession = nil;
        for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
            if ([scene.session.persistentIdentifier isEqualToString:identifier]) {
                targetSession = scene.session;
                break;
            }
        }
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
        return;
    }
    [self presentSceneActivationError:nil title:@"Unable to focus window"];
}

- (void)openExistingTerminalInNewWindowFromButton:(UIButton *)sender {
    NSUUID *terminalUUID = [[NSUUID alloc] initWithUUIDString:sender.accessibilityIdentifier];
    if (terminalUUID == nil) {
        [self presentSceneActivationError:nil title:@"Unable to open terminal window"];
        return;
    }
    if (@available(iOS 13.0, *)) {
        UISceneSession *existingSession = [self sceneSessionHostingTerminalUUID:terminalUUID];
        if ([self focusSceneSession:existingSession title:@"Unable to open terminal window"])
            return;
    }
    ISHWorkspaceContainedWindowView *containedWindow = [self desktopWindowHostingTerminalUUID:terminalUUID];
    if (containedWindow != nil) {
        [self focusDesktopWindow:containedWindow];
        return;
    }
    Terminal *terminal = [Terminal terminalWithUUID:terminalUUID];
    if (terminal == nil) {
        [self presentSceneActivationError:nil title:@"Unable to open terminal window"];
        return;
    }
    if (terminal.webView.superview != nil) {
        [self presentSceneActivationError:nil title:@"Terminal already open in another window"];
        return;
    }
    [self requestSceneWithActivityType:ISHSceneActivityTypeTerminal
                                 title:@"Unable to open terminal window"
                              userInfo:@{ISHSceneTerminalUUIDUserInfoKey: terminalUUID.UUIDString}];
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

- (void)openNewTerminalWindow:(id)sender {
    [self requestSceneWithActivityType:ISHSceneActivityTypeTerminal
                               title:@"Unable to open terminal"
                            userInfo:nil];
}

- (void)openNewWorkspaceWindow:(id)sender {
    [self requestSceneWithActivityType:ISHSceneActivityTypeWorkspace
                               title:@"Unable to open workspace"
                            userInfo:nil];
}

- (void)requestSceneWithActivityType:(NSString *)activityType
                               title:(NSString *)title
                            userInfo:(NSDictionary<NSString *, id> *)userInfo {
    if (@available(iOS 13.0, *)) {
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

@implementation WorkspaceClockToolViewController {
    UILabel *_timeLabel;
    UILabel *_dateLabel;
    UIStackView *_stackView;
    NSTimer *_timer;
    NSDateFormatter *_timeFormatter;
    NSDateFormatter *_dateFormatter;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Clock";
    if (@available(iOS 13.0, *)) {
        self.view.backgroundColor = UIColor.systemBackgroundColor;
    } else {
        self.view.backgroundColor = UIColor.whiteColor;
    }

    _timeFormatter = [NSDateFormatter new];
    _timeFormatter.timeStyle = NSDateFormatterMediumStyle;
    _timeFormatter.dateStyle = NSDateFormatterNoStyle;
    _dateFormatter = [NSDateFormatter new];
    _dateFormatter.dateStyle = NSDateFormatterFullStyle;
    _dateFormatter.timeStyle = NSDateFormatterNoStyle;

    _stackView = [UIStackView new];
    _stackView.axis = UILayoutConstraintAxisVertical;
    _stackView.spacing = 18;
    _stackView.translatesAutoresizingMaskIntoConstraints = NO;
    _stackView.alignment = UIStackViewAlignmentCenter;
    [self.view addSubview:_stackView];

    _timeLabel = [UILabel new];
    _timeLabel.numberOfLines = 1;
    _timeLabel.adjustsFontSizeToFitWidth = YES;
    _timeLabel.minimumScaleFactor = 0.5;
    _timeLabel.font = [UIFont monospacedDigitSystemFontOfSize:40 weight:UIFontWeightBold];
    if (@available(iOS 13.0, *)) {
        _timeLabel.textColor = UIColor.labelColor;
    } else {
        _timeLabel.textColor = UIColor.blackColor;
    }

    _dateLabel = [UILabel new];
    _dateLabel.numberOfLines = 0;
    _dateLabel.textAlignment = NSTextAlignmentCenter;
    _dateLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleTitle3];
    if (@available(iOS 13.0, *)) {
        _dateLabel.textColor = UIColor.secondaryLabelColor;
    } else {
        _dateLabel.textColor = UIColor.darkGrayColor;
    }

    [_stackView addArrangedSubview:_timeLabel];
    [_stackView addArrangedSubview:_dateLabel];

    [NSLayoutConstraint activateConstraints:@[
        [_stackView.centerXAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.centerXAnchor],
        [_stackView.centerYAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.centerYAnchor],
        [_stackView.leadingAnchor constraintGreaterThanOrEqualToAnchor:self.view.safeAreaLayoutGuide.leadingAnchor constant:24],
        [_stackView.trailingAnchor constraintLessThanOrEqualToAnchor:self.view.safeAreaLayoutGuide.trailingAnchor constant:-24],
    ]];

    [self refreshClock:nil];
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];

    CGRect bounds = UIEdgeInsetsInsetRect(self.view.safeAreaLayoutGuide.layoutFrame, UIEdgeInsetsMake(12, 16, 12, 16));
    CGFloat width = MAX(1, CGRectGetWidth(bounds));
    CGFloat height = MAX(1, CGRectGetHeight(bounds));
    CGFloat timeFontSize = MIN(width * 0.22, height * 0.34);
    timeFontSize = MIN(MAX(timeFontSize, 24), 56);
    CGFloat dateFontSize = MIN(width * 0.08, height * 0.13);
    dateFontSize = MIN(MAX(dateFontSize, 12), 24);

    _timeLabel.font = [UIFont monospacedDigitSystemFontOfSize:timeFontSize weight:UIFontWeightBold];
    _dateLabel.font = [UIFont systemFontOfSize:dateFontSize weight:UIFontWeightRegular];
    _stackView.spacing = MAX(10, round(timeFontSize * 0.35));
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
}

@end

@implementation WorkspaceInfoToolViewController {
    UILabel *_batteryLabel;
    UILabel *_rootLabel;
    UILabel *_storageLabel;
    UILabel *_startupLabel;
    NSTimer *_timer;
}

- (UILabel *)infoLabel {
    UILabel *label = [UILabel new];
    label.translatesAutoresizingMaskIntoConstraints = NO;
    label.numberOfLines = 0;
    label.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    if (@available(iOS 13.0, *)) {
        label.textColor = UIColor.labelColor;
    } else {
        label.textColor = UIColor.blackColor;
    }
    return label;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Info";
    if (@available(iOS 13.0, *)) {
        self.view.backgroundColor = UIColor.systemBackgroundColor;
    } else {
        self.view.backgroundColor = UIColor.whiteColor;
    }

    UIStackView *stack = [UIStackView new];
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 14;
    [self.view addSubview:stack];

    UILabel *titleLabel = [UILabel new];
    titleLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];
    titleLabel.text = @"Current workspace info";
    if (@available(iOS 13.0, *)) {
        titleLabel.textColor = UIColor.labelColor;
    } else {
        titleLabel.textColor = UIColor.blackColor;
    }

    _batteryLabel = [self infoLabel];
    _rootLabel = [self infoLabel];
    _storageLabel = [self infoLabel];
    _startupLabel = [self infoLabel];

    [stack addArrangedSubview:titleLabel];
    [stack addArrangedSubview:_batteryLabel];
    [stack addArrangedSubview:_rootLabel];
    [stack addArrangedSubview:_storageLabel];
    [stack addArrangedSubview:_startupLabel];

    [NSLayoutConstraint activateConstraints:@[
        [stack.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor constant:18],
        [stack.leadingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.leadingAnchor constant:18],
        [stack.trailingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.trailingAnchor constant:-18],
        [stack.bottomAnchor constraintLessThanOrEqualToAnchor:self.view.safeAreaLayoutGuide.bottomAnchor constant:-18],
    ]];

    [self refreshInfo:nil];
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
        _batteryLabel.text = @"Battery: unavailable";
    } else {
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
        _batteryLabel.text = [NSString stringWithFormat:@"Battery: %@ (%ld%%)", stateDescription, (long) percent];
    }

    NSString *defaultRoot = Roots.instance.defaultRoot;
    _rootLabel.text = defaultRoot.length > 0
        ? [NSString stringWithFormat:@"Current root: %@", defaultRoot]
        : @"Current root: unavailable";

    NSDictionary<NSFileAttributeKey, id> *attributes =
        [NSFileManager.defaultManager attributesOfFileSystemForPath:NSHomeDirectory() error:nil];
    NSNumber *freeSize = attributes[NSFileSystemFreeSize];
    if (freeSize != nil) {
        NSString *formattedSize = [NSByteCountFormatter stringFromByteCount:freeSize.longLongValue
                                                                  countStyle:NSByteCountFormatterCountStyleFile];
        _storageLabel.text = [NSString stringWithFormat:@"Free storage: %@", formattedSize];
    } else {
        _storageLabel.text = @"Free storage: unavailable";
    }

    _startupLabel.text = [NSString stringWithFormat:@"Startup screen: %@", ISHInitialWindowTitle()];
}

@end

@implementation WorkspaceMonitorToolViewController {
    UIScrollView *_scrollView;
    UIStackView *_contentStack;
    UIProgressView *_cpuProgressView;
    UILabel *_cpuTitleLabel;
    UIProgressView *_memoryProgressView;
    UILabel *_memoryTitleLabel;
    UILabel *_uptimeValueLabel;
    UILabel *_batteryValueLabel;
    UILabel *_diskValueLabel;
    UILabel *_rootValueLabel;
    UILabel *_liveValueLabel;
    UILabel *_networkValueLabel;
    NSTimer *_timer;
    natural_t _previousCPUTicks[CPU_STATE_MAX];
    BOOL _hasPreviousCPUSample;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Monitor";
    if (@available(iOS 13.0, *)) {
        self.view.backgroundColor = UIColor.systemBackgroundColor;
    } else {
        self.view.backgroundColor = UIColor.whiteColor;
    }

    _scrollView = [UIScrollView new];
    _scrollView.translatesAutoresizingMaskIntoConstraints = NO;
    _scrollView.alwaysBounceVertical = YES;
    [self.view addSubview:_scrollView];

    _contentStack = [UIStackView new];
    _contentStack.translatesAutoresizingMaskIntoConstraints = NO;
    _contentStack.axis = UILayoutConstraintAxisVertical;
    _contentStack.spacing = 8;
    [_scrollView addSubview:_contentStack];

    [_contentStack addArrangedSubview:[self monitorBarCardWithTitle:@"CPU"
                                                         titleLabel:&_cpuTitleLabel
                                                       progressView:&_cpuProgressView
                                                        detailLabel:NULL]];
    [_contentStack addArrangedSubview:[self monitorBarCardWithTitle:@"Memory"
                                                         titleLabel:&_memoryTitleLabel
                                                       progressView:&_memoryProgressView
                                                        detailLabel:NULL]];

    UIStackView *factsStack = [UIStackView new];
    factsStack.axis = UILayoutConstraintAxisVertical;
    factsStack.spacing = 6;
    [factsStack addArrangedSubview:[self monitorKeyValueRowWithTitle:@"Live" valueLabel:&_liveValueLabel]];
    [factsStack addArrangedSubview:[self monitorKeyValueRowWithTitle:@"Uptime" valueLabel:&_uptimeValueLabel]];
    [factsStack addArrangedSubview:[self monitorKeyValueRowWithTitle:@"Battery" valueLabel:&_batteryValueLabel]];
    [factsStack addArrangedSubview:[self monitorKeyValueRowWithTitle:@"Storage" valueLabel:&_diskValueLabel]];
    [factsStack addArrangedSubview:[self monitorKeyValueRowWithTitle:@"Root" valueLabel:&_rootValueLabel]];
    [factsStack addArrangedSubview:[self monitorKeyValueRowWithTitle:@"Network" valueLabel:&_networkValueLabel]];
    [_contentStack addArrangedSubview:[self monitorCardWithContent:factsStack]];

    [NSLayoutConstraint activateConstraints:@[
        [_scrollView.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor],
        [_scrollView.leadingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.leadingAnchor],
        [_scrollView.trailingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.trailingAnchor],
        [_scrollView.bottomAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.bottomAnchor],

        [_contentStack.topAnchor constraintEqualToAnchor:_scrollView.contentLayoutGuide.topAnchor constant:10],
        [_contentStack.leadingAnchor constraintEqualToAnchor:_scrollView.frameLayoutGuide.leadingAnchor constant:10],
        [_contentStack.trailingAnchor constraintEqualToAnchor:_scrollView.frameLayoutGuide.trailingAnchor constant:-10],
        [_contentStack.bottomAnchor constraintEqualToAnchor:_scrollView.contentLayoutGuide.bottomAnchor constant:-10],
        [_contentStack.widthAnchor constraintEqualToAnchor:_scrollView.frameLayoutGuide.widthAnchor constant:-20],
    ]];

    self.navigationItem.rightBarButtonItem =
        [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemRefresh
                                                      target:self
                                                      action:@selector(refreshMonitor:)];
    [self refreshMonitor:nil];
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
    UILabel *label = [UILabel new];
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
    if (@available(iOS 13.0, *)) {
        label.textColor = UIColor.labelColor;
    } else {
        label.textColor = UIColor.blackColor;
    }
    return label;
}

- (UIView *)monitorCardWithContent:(UIView *)contentView {
    UIView *card = [UIView new];
    card.translatesAutoresizingMaskIntoConstraints = NO;
    if (@available(iOS 13.0, *)) {
        card.backgroundColor = UIColor.secondarySystemBackgroundColor;
    } else {
        card.backgroundColor = [UIColor colorWithWhite:0.96 alpha:1.0];
    }
    card.layer.cornerRadius = 10;

    contentView.translatesAutoresizingMaskIntoConstraints = NO;
    [card addSubview:contentView];
    [NSLayoutConstraint activateConstraints:@[
        [contentView.topAnchor constraintEqualToAnchor:card.topAnchor constant:8],
        [contentView.leadingAnchor constraintEqualToAnchor:card.leadingAnchor constant:8],
        [contentView.trailingAnchor constraintEqualToAnchor:card.trailingAnchor constant:-8],
        [contentView.bottomAnchor constraintEqualToAnchor:card.bottomAnchor constant:-8],
    ]];
    return card;
}

- (UIView *)monitorBarCardWithTitle:(NSString *)title
                         titleLabel:(UILabel * __strong *)titleLabel
                       progressView:(UIProgressView * __strong *)progressView
                        detailLabel:(UILabel * __strong *)detailLabel {
    UIStackView *stack = [UIStackView new];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 5;

    UILabel *headingLabel = [self monitorValueLabelWithMonospaced:NO];
    headingLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleSubheadline];
    headingLabel.text = title;
    [stack addArrangedSubview:headingLabel];

    UIProgressView *bar = [[UIProgressView alloc] initWithProgressViewStyle:UIProgressViewStyleDefault];
    if (@available(iOS 13.0, *)) {
        bar.trackTintColor = UIColor.tertiarySystemFillColor;
        bar.progressTintColor = UIColor.systemGreenColor;
    }
    bar.transform = CGAffineTransformMakeScale(1.0, 1.35);
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
    row.spacing = 6;
    row.alignment = UIStackViewAlignmentFirstBaseline;

    UILabel *titleLabel = [self monitorValueLabelWithMonospaced:NO];
    titleLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleCaption1];
    titleLabel.text = title;
    if (@available(iOS 13.0, *)) {
        titleLabel.textColor = UIColor.secondaryLabelColor;
    } else {
        titleLabel.textColor = UIColor.darkGrayColor;
    }

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

    uint64_t footprint = 0;
    uint64_t physical = 0;
    BOOL hasMemory = ISHWorkspaceMemoryUsage(&footprint, NULL, &physical);
    double memoryRatio = (hasMemory && physical > 0) ? ((double) footprint / (double) physical) : 0.0;

    NSString *storageString = [ISHWorkspaceStorageSummaryText() stringByReplacingOccurrencesOfString:@"Free storage: "
                                                                                          withString:@""];
    NSString *defaultRoot = Roots.instance.defaultRoot;
    NSString *networkLine = ISHWorkspacePrimaryNetworkLine();

    NSUInteger sceneCount = 0;
    if (@available(iOS 13.0, *)) {
        sceneCount = UIApplication.sharedApplication.connectedScenes.count;
    }

    NSUInteger terminalCount = [Terminal activeTerminals].count;

    if (cpuRatio >= 0.0) {
        _cpuProgressView.progress = (float) cpuRatio;
        _cpuTitleLabel.text = [NSString stringWithFormat:@"CPU  %ld%%", (long) llround(cpuRatio * 100.0)];
    } else {
        _cpuProgressView.progress = 0.0f;
        _cpuTitleLabel.text = @"CPU  unavailable";
    }

    if (hasMemory) {
        _memoryProgressView.progress = (float) memoryRatio;
        _memoryTitleLabel.text = [NSString stringWithFormat:@"Memory  %ld%%", (long) llround(memoryRatio * 100.0)];
    } else {
        _memoryProgressView.progress = 0.0f;
        _memoryTitleLabel.text = @"Memory  unavailable";
    }

    _uptimeValueLabel.text = ISHWorkspaceDurationString(NSProcessInfo.processInfo.systemUptime);
    _batteryValueLabel.text = ISHWorkspaceBatterySummaryText();
    _diskValueLabel.text = storageString;
    _rootValueLabel.text = defaultRoot.length > 0 ? defaultRoot : @"unavailable";
    _liveValueLabel.text = [NSString stringWithFormat:@"%lu scenes   %lu roots   %lu terminals",
                            (unsigned long) sceneCount,
                            (unsigned long) Roots.instance.roots.count,
                            (unsigned long) terminalCount];
    _networkValueLabel.text = networkLine;
}

@end

@implementation WorkspaceNetworksToolViewController {
    UITextView *_textView;
    NSTimer *_timer;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Networks";
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
        _textView.font = [UIFont monospacedSystemFontOfSize:13 weight:UIFontWeightRegular];
    } else {
        _textView.backgroundColor = UIColor.whiteColor;
        _textView.textColor = UIColor.blackColor;
        _textView.font = [UIFont fontWithName:@"Menlo-Regular" size:13] ?: [UIFont systemFontOfSize:13];
    }
    [self.view addSubview:_textView];

    self.navigationItem.rightBarButtonItem =
        [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemRefresh
                                                      target:self
                                                      action:@selector(refreshNetworks:)];
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
    _textView.text = ISHWorkspaceNetworkSummaryText();
}

@end

@implementation WorkspaceStatusToolViewController {
    UITextView *_textView;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"System Status";
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
        _textView.font = [UIFont monospacedSystemFontOfSize:13 weight:UIFontWeightRegular];
    } else {
        _textView.backgroundColor = UIColor.whiteColor;
        _textView.textColor = UIColor.blackColor;
        _textView.font = [UIFont fontWithName:@"Menlo-Regular" size:13] ?: [UIFont systemFontOfSize:13];
    }
    [self.view addSubview:_textView];

    self.navigationItem.rightBarButtonItem =
        [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemRefresh
                                                      target:self
                                                      action:@selector(refreshStatus:)];
    [self refreshStatus:nil];
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [self refreshStatus:nil];
}

- (void)refreshStatus:(id)sender {
    _textView.text = ISHWorkspaceSystemStatusText();
}

@end
