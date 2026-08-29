//
//  WorkspaceFileManager.m
//  iSH-AOK
//

#import "WorkspaceFileManager.h"
#import "GuestFileBridge.h"
#import "AudioLibrary.h"
#import "AudioPlayerEngine.h"
#import "UserPreferences.h"

// Forward-declared: defined near the routing table below, used earlier by
// -iconForItem: to badge image/audio/video files distinctly.
static NSSet<NSString *> *ISHFileManagerImageExtensions(void);
static NSSet<NSString *> *ISHFileManagerVideoExtensions(void);

static const CGFloat kFileManagerSidebarWidth = 180.0;
static const CGFloat kFileManagerSidebarCollapseThreshold = 520.0;
static const CGFloat kFileManagerToolbarHeight = 44.0;
static const CGFloat kFileManagerStatusBarHeight = 28.0;
static const CGFloat kFileManagerDividerThickness = 1.0 / 3.0;  // hairline; UIView doesn't need device-scale awareness here

static NSString *const kFileManagerSortModeDefaultsKey = @"WorkspaceFileManagerSortMode";
static NSString *const kFileManagerShowHiddenDefaultsKey = @"WorkspaceFileManagerShowHidden";

static NSString *const kFileManagerCellReuseID = @"filemanager.row";
static NSString *const kFileManagerSidebarCellReuseID = @"filemanager.sidebar";

typedef NS_ENUM(NSInteger, WorkspaceFileManagerSortMode) {
    WorkspaceFileManagerSortName = 0,
    WorkspaceFileManagerSortSize = 1,
    WorkspaceFileManagerSortDate = 2,
    WorkspaceFileManagerSortKind = 3,
};

// Parses /etc/passwd content for the entry whose numeric UID field matches targetUID and
// returns its home-directory field (field 5, 0-indexed: name:passwd:uid:gid:gecos:home:shell).
// nil if the file couldn't be read/decoded or no entry matches.
static NSString *ISHHomeDirectoryForUID(NSData *passwdData, uid_t targetUID) {
    if (passwdData == nil)
        return nil;
    NSString *contents = [[NSString alloc] initWithData:passwdData encoding:NSUTF8StringEncoding];
    if (contents == nil)
        return nil;
    for (NSString *line in [contents componentsSeparatedByString:@"\n"]) {
        NSArray<NSString *> *fields = [line componentsSeparatedByString:@":"];
        if (fields.count < 6)
            continue;
        if ((uid_t)fields[2].integerValue == targetUID)
            return fields[5];
    }
    return nil;
}

@interface WorkspaceFileManagerToolViewController () <UITableViewDataSource, UITableViewDelegate>
@end

@implementation WorkspaceFileManagerToolViewController {
    UIView *_sidebarContainerView;
    UITableView *_sidebarTableView;
    NSLayoutConstraint *_sidebarWidthConstraint;
    BOOL _sidebarHidden;

    UIView *_dividerView;

    UIView *_toolbarView;
    UIButton *_backButton;
    UIButton *_forwardButton;
    UIButton *_upButton;
    UIScrollView *_pathScrollView;   // breadcrumb: one tappable button per path component
    UIStackView *_pathStack;
    UIButton *_moreButton;

    UITableView *_tableView;
    UIView *_statusBarView;
    UILabel *_statusLabel;

    NSString *_currentPath;
    NSMutableArray<NSString *> *_backHistory;
    NSMutableArray<NSString *> *_forwardHistory;

    NSArray<ISHGuestFileItem *> *_allItems;   // last listing, unfiltered/unsorted
    NSArray<ISHGuestFileItem *> *_items;      // filtered + sorted, what the table shows
    NSError *_loadError;
    BOOL _loading;
    NSInteger _loadGeneration;                // discards stale async listings
    // ...and this STOPS the superseded one. The generation guard only throws the
    // answer away; the enumeration kept running, holding the lane, while the
    // table sat with userInteractionEnabled = NO.
    ISHGuestFileOperationToken _listToken;
    int64_t _availableBytes;                  // statfs free space of the current mount, 0 = no figure

    WorkspaceFileManagerSortMode _sortMode;
    BOOL _showHidden;
    BOOL _navigationPinned;  // a restore or user action happened; the async default-directory kick must not override it

    NSString *_homeDirectoryPath;  // sidebar "Home" target; resolved from /etc/passwd, see -resolveHomeDirectoryPath
}

// Fixed, non-editable sidebar rows for v1 -- the plan's "user-editable Favorites" is a
// later-phase nicety; this covers the seed locations that matter today.
- (NSArray<NSDictionary *> *)sidebarRows {
    return @[
        @{@"title": @"Home", @"path": _homeDirectoryPath ?: @"/root", @"symbol": @"house"},
        @{@"title": @"Persist", @"path": @"/AOK/persist", @"symbol": @"externaldrive"},
        @{@"title": @"/tmp", @"path": @"/tmp", @"symbol": @"clock"},
        @{@"title": @"Root (/)", @"path": @"/", @"symbol": @"internaldrive"},
    ];
}

// New sessions log in as root, or as whatever account this rootfs has at UID 1000 when "Open
// Everything as Default User" is on (see +[AppDelegate defaultUserAccountName] -- no account is
// provisioned by iSH itself) -- match the sidebar's "Home" row to wherever that account's shell
// actually lands by reading its real entry out of /etc/passwd.
- (void)resolveHomeDirectoryPath {
    _homeDirectoryPath = @"/root";
    if (!UserPreferences.shared.shouldLoginAsDefaultUser)
        return;
    __weak typeof(self) weakSelf = self;
    [[ISHGuestFileBridge sharedBridge] readFileAtGuestPath:@"/etc/passwd" maxBytes:65536
                                                  completion:^(NSData *data, NSError *error) {
        typeof(self) strongSelf = weakSelf;
        if (strongSelf == nil) return;
        NSString *home = ISHHomeDirectoryForUID(data, ISHDefaultUserAccountUID);
        if (home.length == 0) return;
        strongSelf->_homeDirectoryPath = home;
        [strongSelf->_sidebarTableView reloadData];
    }];
}

#pragma mark Lifecycle

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"File Manager";

    _backHistory = [NSMutableArray array];
    _forwardHistory = [NSMutableArray array];
    _items = @[];
    _allItems = @[];

    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    _sortMode = (WorkspaceFileManagerSortMode)[defaults integerForKey:kFileManagerSortModeDefaultsKey];
    _showHidden = [defaults boolForKey:kFileManagerShowHiddenDefaultsKey];

    [self resolveHomeDirectoryPath];
    [self buildSidebar];
    [self buildDivider];
    [self buildToolbar];
    [self buildTableView];
    [self buildStatusBar];
    [self activateRegionConstraints];

    [self refreshMoreMenu];
    _currentPath = @"/";
    [self updateToolbarState];
    [self reload];

    // Start in /AOK/persist when it exists (matches MotePad's default-directory
    // convention) without blocking the first paint on the check.
    __weak typeof(self) weakSelf = self;
    [[ISHGuestFileBridge sharedBridge] statAtGuestPath:@"/AOK/persist" completion:^(ISHGuestFileItem *item, NSError *error) {
        typeof(self) strongSelf = weakSelf;
        if (strongSelf == nil) return;
        BOOL userAlreadyNavigated = strongSelf->_navigationPinned
            || strongSelf->_backHistory.count > 0
            || ![strongSelf->_currentPath isEqualToString:@"/"];
        if (userAlreadyNavigated) return;
        if (item != nil && item.kind == ISHGuestFileKindDirectory)
            [strongSelf navigateToPath:@"/AOK/persist" pushHistory:NO];
    }];
}

#pragma mark WorkspaceStatefulTool

- (nullable NSDictionary<NSString *, id> *)workspaceToolStateForSaving {
    return _currentPath.length > 0 ? @{@"path": _currentPath} : nil;
}

- (void)workspaceRestoreToolState:(NSDictionary<NSString *, id> *)state {
    NSString *path = [state[@"path"] isKindOfClass:NSString.class] ? state[@"path"] : nil;
    if (path.length == 0)
        return;
    // Pin even when the restored path is "/": the user's saved location must
    // win over the async default-directory kick above, whatever it was. A
    // path that no longer exists just shows the listing's errno empty state.
    _navigationPinned = YES;
    [self navigateToPath:path pushHistory:NO];
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    BOOL hideSidebar = self.view.bounds.size.width < kFileManagerSidebarCollapseThreshold;
    if (hideSidebar == _sidebarHidden)
        return;
    _sidebarHidden = hideSidebar;
    _sidebarContainerView.hidden = hideSidebar;
    _sidebarWidthConstraint.constant = hideSidebar ? 0 : kFileManagerSidebarWidth;
}

#pragma mark Hardware keyboard

// Key commands are collected along the first-responder chain, and this
// applet has no text input to anchor it there (MotePad's shortcuts work
// because its UITextView is first responder) -- so the view controller
// itself participates: it takes first responder when its window appears and
// again on any row tap, and yields naturally when the user focuses a
// terminal or another editor.
- (BOOL)canBecomeFirstResponder {
    return YES;
}

- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    [self becomeFirstResponder];
}

// Only shortcuts that don't require row selection -- the tap-to-open
// interaction model has no persistent selection to operate on. Selection-
// dependent shortcuts (return-to-open, cmd-delete, space preview) arrive
// with multi-select.
- (NSArray<UIKeyCommand *> *)keyCommands {
    UIKeyCommand *up = [UIKeyCommand keyCommandWithInput:UIKeyInputUpArrow modifierFlags:UIKeyModifierCommand action:@selector(navigateUp)];
    up.discoverabilityTitle = @"Enclosing Folder";
    UIKeyCommand *back = [UIKeyCommand keyCommandWithInput:@"[" modifierFlags:UIKeyModifierCommand action:@selector(navigateBack)];
    back.discoverabilityTitle = @"Back";
    UIKeyCommand *forward = [UIKeyCommand keyCommandWithInput:@"]" modifierFlags:UIKeyModifierCommand action:@selector(navigateForward)];
    forward.discoverabilityTitle = @"Forward";
    UIKeyCommand *hidden = [UIKeyCommand keyCommandWithInput:@"." modifierFlags:UIKeyModifierCommand | UIKeyModifierShift action:@selector(toggleHiddenFiles)];
    hidden.discoverabilityTitle = @"Show/Hide Hidden Files";
    UIKeyCommand *refresh = [UIKeyCommand keyCommandWithInput:@"r" modifierFlags:UIKeyModifierCommand action:@selector(reload)];
    refresh.discoverabilityTitle = @"Refresh";
    UIKeyCommand *newWindow = [UIKeyCommand keyCommandWithInput:@"n" modifierFlags:UIKeyModifierCommand action:@selector(openNewWindowHere)];
    newWindow.discoverabilityTitle = @"New File Manager Window";
    return @[up, back, forward, hidden, refresh, newWindow];
}

- (void)openNewWindowHere {
    // The routing entry point opens a fresh (non-singleton) window and
    // delivers the path; our dir-aware open then navigates straight into it.
    [self.workspaceHostViewController openWorkspaceToolWithIdentifier:@"filemanager" fileGuestPath:_currentPath];
}

- (void)toggleHiddenFiles {
    _showHidden = !_showHidden;
    [NSUserDefaults.standardUserDefaults setBool:_showHidden forKey:kFileManagerShowHiddenDefaultsKey];
    [self applyFilterAndSort];
    [self refreshMoreMenu];
}

#pragma mark View construction

- (void)buildSidebar {
    _sidebarContainerView = [UIView new];
    _sidebarContainerView.translatesAutoresizingMaskIntoConstraints = NO;
    _sidebarContainerView.clipsToBounds = YES;
    [self.toolContentView addSubview:_sidebarContainerView];

    _sidebarTableView = [[UITableView alloc] initWithFrame:CGRectZero style:UITableViewStylePlain];
    _sidebarTableView.translatesAutoresizingMaskIntoConstraints = NO;
    _sidebarTableView.dataSource = self;
    _sidebarTableView.delegate = self;
    _sidebarTableView.rowHeight = 36.0;
    [_sidebarContainerView addSubview:_sidebarTableView];

    [NSLayoutConstraint activateConstraints:@[
        [_sidebarTableView.topAnchor constraintEqualToAnchor:_sidebarContainerView.topAnchor],
        [_sidebarTableView.leadingAnchor constraintEqualToAnchor:_sidebarContainerView.leadingAnchor],
        [_sidebarTableView.trailingAnchor constraintEqualToAnchor:_sidebarContainerView.trailingAnchor],
        [_sidebarTableView.bottomAnchor constraintEqualToAnchor:_sidebarContainerView.bottomAnchor],
    ]];
}

- (void)buildDivider {
    _dividerView = [UIView new];
    _dividerView.translatesAutoresizingMaskIntoConstraints = NO;
    [self.toolContentView addSubview:_dividerView];
}

- (void)buildToolbar {
    _toolbarView = [UIView new];
    _toolbarView.translatesAutoresizingMaskIntoConstraints = NO;
    [self.toolContentView addSubview:_toolbarView];

    _backButton = [self toolbarIconButtonNamed:@"chevron.left" action:@selector(navigateBack)];
    _backButton.accessibilityLabel = @"Back";
    _forwardButton = [self toolbarIconButtonNamed:@"chevron.right" action:@selector(navigateForward)];
    _forwardButton.accessibilityLabel = @"Forward";
    _upButton = [self toolbarIconButtonNamed:@"arrow.up" action:@selector(navigateUp)];
    _upButton.accessibilityLabel = @"Up";
    _moreButton = [self toolbarIconButtonNamed:@"ellipsis.circle" action:nil];
    _moreButton.accessibilityLabel = @"More Actions";
    _moreButton.showsMenuAsPrimaryAction = YES;

    // Finder-style breadcrumb: one button per path component inside a
    // horizontal scroll view, so deep paths scroll rather than squeezing the
    // nav buttons out. Buttons are rebuilt on every navigation.
    _pathScrollView = [UIScrollView new];
    _pathScrollView.translatesAutoresizingMaskIntoConstraints = NO;
    _pathScrollView.showsHorizontalScrollIndicator = NO;
    _pathScrollView.showsVerticalScrollIndicator = NO;
    [_pathScrollView setContentHuggingPriority:UILayoutPriorityDefaultLow forAxis:UILayoutConstraintAxisHorizontal];
    [_pathScrollView setContentCompressionResistancePriority:UILayoutPriorityDefaultLow forAxis:UILayoutConstraintAxisHorizontal];

    _pathStack = [UIStackView new];
    _pathStack.translatesAutoresizingMaskIntoConstraints = NO;
    _pathStack.axis = UILayoutConstraintAxisHorizontal;
    _pathStack.alignment = UIStackViewAlignmentCenter;
    _pathStack.spacing = 2.0;
    [_pathScrollView addSubview:_pathStack];

    [NSLayoutConstraint activateConstraints:@[
        [_pathStack.leadingAnchor constraintEqualToAnchor:_pathScrollView.contentLayoutGuide.leadingAnchor],
        [_pathStack.trailingAnchor constraintEqualToAnchor:_pathScrollView.contentLayoutGuide.trailingAnchor],
        [_pathStack.topAnchor constraintEqualToAnchor:_pathScrollView.contentLayoutGuide.topAnchor],
        [_pathStack.bottomAnchor constraintEqualToAnchor:_pathScrollView.contentLayoutGuide.bottomAnchor],
        [_pathStack.heightAnchor constraintEqualToAnchor:_pathScrollView.frameLayoutGuide.heightAnchor],
        [_pathScrollView.heightAnchor constraintEqualToConstant:kFileManagerToolbarHeight - 12.0],
    ]];

    UIStackView *stack = [[UIStackView alloc] initWithArrangedSubviews:@[_backButton, _forwardButton, _upButton, _pathScrollView, _moreButton]];
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    stack.axis = UILayoutConstraintAxisHorizontal;
    stack.alignment = UIStackViewAlignmentCenter;
    stack.spacing = 10.0;
    [_toolbarView addSubview:stack];

    [NSLayoutConstraint activateConstraints:@[
        [stack.leadingAnchor constraintEqualToAnchor:_toolbarView.leadingAnchor constant:10],
        [stack.trailingAnchor constraintEqualToAnchor:_toolbarView.trailingAnchor constant:-10],
        [stack.centerYAnchor constraintEqualToAnchor:_toolbarView.centerYAnchor],
    ]];
}

// One tappable button per path component ("/" plus each directory), the last
// one rendered as the current location. Buttons carry their target path in a
// UIAction, so there's nothing to look up on tap.
- (void)rebuildBreadcrumb {
    for (UIView *subview in _pathStack.arrangedSubviews.copy) {
        [_pathStack removeArrangedSubview:subview];
        [subview removeFromSuperview];
    }

    NSDictionary<NSString *, UIColor *> *theme = self.workspaceTheme;
    NSMutableArray<NSString *> *prefixes = [NSMutableArray arrayWithObject:@"/"];
    NSMutableArray<NSString *> *titles = [NSMutableArray arrayWithObject:@"/"];
    NSString *running = @"";
    for (NSString *component in _currentPath.pathComponents) {
        if ([component isEqualToString:@"/"])
            continue;
        running = [running stringByAppendingFormat:@"/%@", component];
        [prefixes addObject:running];
        [titles addObject:component];
    }

    for (NSUInteger i = 0; i < prefixes.count; i++) {
        BOOL isLast = (i == prefixes.count - 1);
        if (i > 0) {
            UILabel *separator = [UILabel new];
            separator.text = @"›";
            separator.font = [UIFont systemFontOfSize:12.0];
            separator.textColor = theme[@"secondary"];
            [_pathStack addArrangedSubview:separator];
        }
        UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
        button.titleLabel.font = [UIFont systemFontOfSize:13.0 weight:(isLast ? UIFontWeightSemibold : UIFontWeightRegular)];
        [button setTitle:titles[i] forState:UIControlStateNormal];
        [button setTitleColor:(isLast ? theme[@"primary"] : theme[@"accent"]) forState:UIControlStateNormal];
        button.enabled = !isLast;  // the current location isn't a link anywhere
        button.accessibilityHint = isLast ? nil : @"Go to this folder";
        NSString *targetPath = prefixes[i];
        __weak typeof(self) weakSelf = self;
        [button addAction:[UIAction actionWithHandler:^(UIAction *action) {
            [weakSelf navigateToPath:targetPath pushHistory:YES];
        }] forControlEvents:UIControlEventTouchUpInside];
        [_pathStack addArrangedSubview:button];
    }

    // Deep paths overflow leftward; keep the current location in view.
    [_pathScrollView layoutIfNeeded];
    CGFloat overflow = _pathStack.bounds.size.width - _pathScrollView.bounds.size.width;
    if (overflow > 0)
        [_pathScrollView setContentOffset:CGPointMake(overflow, 0) animated:NO];
}

- (UIButton *)toolbarIconButtonNamed:(NSString *)symbolName action:(nullable SEL)action {
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    button.translatesAutoresizingMaskIntoConstraints = NO;
    [button setImage:[UIImage systemImageNamed:symbolName] forState:UIControlStateNormal];
    if (action != NULL)
        [button addTarget:self action:action forControlEvents:UIControlEventTouchUpInside];
    [button.widthAnchor constraintGreaterThanOrEqualToConstant:28].active = YES;
    return button;
}

- (void)buildTableView {
    _tableView = [[UITableView alloc] initWithFrame:CGRectZero style:UITableViewStylePlain];
    _tableView.translatesAutoresizingMaskIntoConstraints = NO;
    _tableView.dataSource = self;
    _tableView.delegate = self;
    _tableView.backgroundColor = UIColor.clearColor;
    [self.toolContentView addSubview:_tableView];
}

- (void)buildStatusBar {
    _statusBarView = [UIView new];
    _statusBarView.translatesAutoresizingMaskIntoConstraints = NO;
    [self.toolContentView addSubview:_statusBarView];

    _statusLabel = [UILabel new];
    _statusLabel.translatesAutoresizingMaskIntoConstraints = NO;
    _statusLabel.font = [UIFont systemFontOfSize:11.0];
    [_statusBarView addSubview:_statusLabel];

    [NSLayoutConstraint activateConstraints:@[
        [_statusLabel.leadingAnchor constraintEqualToAnchor:_statusBarView.leadingAnchor constant:10],
        [_statusLabel.centerYAnchor constraintEqualToAnchor:_statusBarView.centerYAnchor],
    ]];
}

// Explicit region anchors rather than a fill-distribution UIStackView: UITableView
// has no reliable intrinsic content size, and a stack view's "give the ambiguous
// child the remaining space" behavior is exactly the kind of thing that's easy to
// get subtly wrong without being able to see it render. Every region here is
// pinned by hand instead, matching how every other applet in this file pins its
// outer scroll/content views.
- (void)activateRegionConstraints {
    UIView *content = self.toolContentView;
    _sidebarWidthConstraint = [_sidebarContainerView.widthAnchor constraintEqualToConstant:kFileManagerSidebarWidth];
    [NSLayoutConstraint activateConstraints:@[
        [_sidebarContainerView.topAnchor constraintEqualToAnchor:content.topAnchor],
        [_sidebarContainerView.leadingAnchor constraintEqualToAnchor:content.leadingAnchor],
        [_sidebarContainerView.bottomAnchor constraintEqualToAnchor:content.bottomAnchor],
        _sidebarWidthConstraint,

        [_dividerView.topAnchor constraintEqualToAnchor:content.topAnchor],
        [_dividerView.bottomAnchor constraintEqualToAnchor:content.bottomAnchor],
        [_dividerView.leadingAnchor constraintEqualToAnchor:_sidebarContainerView.trailingAnchor],
        [_dividerView.widthAnchor constraintEqualToConstant:kFileManagerDividerThickness],

        [_toolbarView.topAnchor constraintEqualToAnchor:content.topAnchor],
        [_toolbarView.leadingAnchor constraintEqualToAnchor:_dividerView.trailingAnchor],
        [_toolbarView.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [_toolbarView.heightAnchor constraintEqualToConstant:kFileManagerToolbarHeight],

        [_tableView.topAnchor constraintEqualToAnchor:_toolbarView.bottomAnchor],
        [_tableView.leadingAnchor constraintEqualToAnchor:_dividerView.trailingAnchor],
        [_tableView.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [_tableView.bottomAnchor constraintEqualToAnchor:_statusBarView.topAnchor],

        [_statusBarView.leadingAnchor constraintEqualToAnchor:_dividerView.trailingAnchor],
        [_statusBarView.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [_statusBarView.bottomAnchor constraintEqualToAnchor:content.bottomAnchor],
        [_statusBarView.heightAnchor constraintEqualToConstant:kFileManagerStatusBarHeight],
    ]];
}

#pragma mark Theme

- (void)workspaceApplyTheme {
    [super workspaceApplyTheme];
    // The base class invokes this from ITS viewDidLoad, before ours has
    // built any views -- an @[] literal of nil ivars would throw. Same guard
    // the Music applet uses for the same reason.
    if (_toolbarView == nil)
        return;
    NSDictionary<NSString *, UIColor *> *theme = self.workspaceTheme;
    // primary/secondary/accent are only guaranteed to contrast against the
    // theme's card surfaces (they're programmatically contrast-adjusted
    // against them, see ISHWorkspaceThemeDescriptorFromEditablePalette) --
    // never against backgroundTop/backgroundBottom directly. Every other
    // applet in this file renders on an opaque workspaceThemeCardView; this
    // one and the other new viewers were the only ones putting text right on
    // the transparent gradient backdrop, which is exactly what made rows
    // unreadable in some themes (dark text in a theme whose gradient reads
    // dark, or vice versa) despite the theme system's contrast guarantees.
    self.toolContentView.backgroundColor = [theme[@"card"] colorWithAlphaComponent:0.95];
    [self rebuildBreadcrumb];  // its buttons bake theme colors at build time
    _statusLabel.textColor = theme[@"secondary"];
    _dividerView.backgroundColor = theme[@"stroke"];
    _tableView.separatorColor = theme[@"stroke"];
    _sidebarTableView.separatorColor = theme[@"stroke"];
    _sidebarTableView.backgroundColor = [theme[@"cardAlt"] colorWithAlphaComponent:0.95];
    for (UIButton *button in @[_backButton, _forwardButton, _upButton, _moreButton])
        button.tintColor = theme[@"accent"];
    [_tableView reloadData];
    [_sidebarTableView reloadData];
    [self updateEmptyState];  // its label bakes theme colors at creation; rebuild so it tracks the new theme
}

#pragma mark Navigation

- (void)navigateToPath:(NSString *)path pushHistory:(BOOL)pushHistory {
    if (pushHistory && _currentPath != nil && ![_currentPath isEqualToString:path]) {
        [_backHistory addObject:_currentPath];
        [_forwardHistory removeAllObjects];
    }
    _currentPath = path;
    [self updateToolbarState];
    [self reload];
}

- (void)navigateBack {
    if (_backHistory.count == 0) return;
    NSString *target = _backHistory.lastObject;
    [_backHistory removeLastObject];
    [_forwardHistory addObject:_currentPath];
    _currentPath = target;
    [self updateToolbarState];
    [self reload];
}

- (void)navigateForward {
    if (_forwardHistory.count == 0) return;
    NSString *target = _forwardHistory.lastObject;
    [_forwardHistory removeLastObject];
    [_backHistory addObject:_currentPath];
    _currentPath = target;
    [self updateToolbarState];
    [self reload];
}

- (void)navigateUp {
    if ([_currentPath isEqualToString:@"/"]) return;
    NSString *parent = _currentPath.stringByDeletingLastPathComponent;
    if (parent.length == 0) parent = @"/";
    [self navigateToPath:parent pushHistory:YES];
}

- (void)updateToolbarState {
    _backButton.enabled = _backHistory.count > 0;
    _forwardButton.enabled = _forwardHistory.count > 0;
    _upButton.enabled = ![_currentPath isEqualToString:@"/"];
    [self rebuildBreadcrumb];
}

#pragma mark WorkspaceFileOpenable

// Opening a directory shows that directory; opening a file reveals its
// containing folder. The kind check is async (a stat through the bridge),
// which is fine -- navigation lands a beat later.
- (void)workspaceOpenFileAtGuestPath:(NSString *)guestPath {
    __weak typeof(self) weakSelf = self;
    [ISHGuestFileBridge.sharedBridge statAtGuestPath:guestPath completion:^(ISHGuestFileItem *item, NSError *error) {
        typeof(self) strongSelf = weakSelf;
        if (strongSelf == nil) return;
        strongSelf->_navigationPinned = YES;  // an explicit open always outranks the default-directory kick
        if (item != nil && item.kind == ISHGuestFileKindDirectory) {
            [strongSelf navigateToPath:item.guestPath pushHistory:YES];
            return;
        }
        NSString *directory = guestPath.stringByDeletingLastPathComponent;
        if (directory.length == 0) directory = @"/";
        [strongSelf navigateToPath:directory pushHistory:YES];
    }];
}

#pragma mark Loading

- (void)reload {
    NSInteger generation = ++_loadGeneration;
    [self cancelPendingListing];
    [self setLoading:YES];
    __weak typeof(self) weakSelf = self;
    _listToken = [[ISHGuestFileBridge sharedBridge] listDirectoryAtGuestPath:_currentPath
                                                      completion:^(NSArray<ISHGuestFileItem *> *items, NSError *error) {
        typeof(self) strongSelf = weakSelf;
        if (strongSelf == nil || strongSelf->_loadGeneration != generation)
            return;  // a newer navigation superseded this listing
        strongSelf->_listToken = nil;
        strongSelf->_loadError = error;
        strongSelf->_allItems = items ?: @[];
        [strongSelf setLoading:NO];  // after the assignments: this recomputes the empty state from them
        [strongSelf applyFilterAndSort];
    }];
    [[ISHGuestFileBridge sharedBridge] filesystemStatusAtGuestPath:_currentPath
                                                         completion:^(int64_t availableBytes, int64_t totalBytes, NSError *error) {
        typeof(self) strongSelf = weakSelf;
        if (strongSelf == nil || strongSelf->_loadGeneration != generation)
            return;
        strongSelf->_availableBytes = (error == nil) ? availableBytes : 0;
        [strongSelf updateStatusLabel];
    }];
}

// The cancelled listing still completes, with ISHGuestFileBridgeErrorCancelled;
// the generation guard drops it, because everything that cancels has already
// bumped the generation or is being torn down.
- (void)cancelPendingListing {
    if (_listToken == nil)
        return;
    [ISHGuestFileBridge.sharedBridge cancelOperation:_listToken];
    _listToken = nil;
}

- (void)dealloc {
    [self cancelPendingListing];
}

- (void)setLoading:(BOOL)loading {
    _loading = loading;
    _tableView.userInteractionEnabled = !loading;
    if (loading) {
        UIActivityIndicatorView *spinner = [[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleMedium];
        [spinner startAnimating];
        _tableView.backgroundView = spinner;
    } else {
        [self updateEmptyState];
    }
}

- (void)applyFilterAndSort {
    NSArray<ISHGuestFileItem *> *filtered = _allItems;
    if (!_showHidden) {
        filtered = [filtered filteredArrayUsingPredicate:[NSPredicate predicateWithBlock:
            ^BOOL(ISHGuestFileItem *item, NSDictionary *bindings) {
                return ![item.name hasPrefix:@"."];
            }]];
    }
    _items = [self sortedItems:filtered];
    [_tableView reloadData];
    [self updateEmptyState];
    [self updateStatusLabel];
}

- (NSArray<ISHGuestFileItem *> *)sortedItems:(NSArray<ISHGuestFileItem *> *)items {
    NSComparator comparator;
    switch (_sortMode) {
        case WorkspaceFileManagerSortSize:
            comparator = ^NSComparisonResult(ISHGuestFileItem *a, ISHGuestFileItem *b) {
                if (a.size != b.size) return a.size < b.size ? NSOrderedDescending : NSOrderedAscending;
                return [a.name caseInsensitiveCompare:b.name];
            };
            break;
        case WorkspaceFileManagerSortDate:
            comparator = ^NSComparisonResult(ISHGuestFileItem *a, ISHGuestFileItem *b) {
                NSDate *ad = a.modificationDate ?: NSDate.distantPast;
                NSDate *bd = b.modificationDate ?: NSDate.distantPast;
                NSComparisonResult result = [bd compare:ad];
                return result != NSOrderedSame ? result : [a.name caseInsensitiveCompare:b.name];
            };
            break;
        case WorkspaceFileManagerSortKind:
            comparator = ^NSComparisonResult(ISHGuestFileItem *a, ISHGuestFileItem *b) {
                if (a.kind != b.kind) return a.kind < b.kind ? NSOrderedAscending : NSOrderedDescending;
                return [a.name caseInsensitiveCompare:b.name];
            };
            break;
        case WorkspaceFileManagerSortName:
        default:
            comparator = ^NSComparisonResult(ISHGuestFileItem *a, ISHGuestFileItem *b) {
                return [a.name caseInsensitiveCompare:b.name];
            };
            break;
    }
    return [items sortedArrayUsingComparator:comparator];
}

- (void)updateEmptyState {
    if (_loading) return;
    if (_items.count > 0) {
        _tableView.backgroundView = nil;
        return;
    }
    UILabel *label = [UILabel new];
    label.textAlignment = NSTextAlignmentCenter;
    label.numberOfLines = 0;
    label.textColor = self.workspaceTheme[@"secondary"] ?: UIColor.secondaryLabelColor;
    label.font = [UIFont systemFontOfSize:15.0];
    if (_loadError != nil)
        label.text = _loadError.localizedDescription.length ? _loadError.localizedDescription : @"Couldn’t load this folder.";
    else if (![ISHGuestFileBridge.sharedBridge isGuestAvailable])
        label.text = @"The guest filesystem isn’t ready yet.";
    else
        label.text = @"This folder is empty.";
    _tableView.backgroundView = label;
}

- (void)updateStatusLabel {
    NSString *countText = _items.count == 1 ? @"1 item" : [NSString stringWithFormat:@"%lu items", (unsigned long)_items.count];
    // 0 means "the filesystem reports no figure" (proc, devpts), not "full".
    if (_availableBytes > 0)
        countText = [countText stringByAppendingFormat:@" · %@ available", [self formattedSize:(unsigned long long)_availableBytes]];
    _statusLabel.text = countText;
}

#pragma mark UITableViewDataSource / UITableViewDelegate

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    return (tableView == _sidebarTableView) ? (NSInteger)[self sidebarRows].count : (NSInteger)_items.count;
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    if (tableView == _sidebarTableView)
        return [self sidebarCellForRowAtIndexPath:indexPath inTableView:tableView];
    return [self fileCellForRowAtIndexPath:indexPath inTableView:tableView];
}

- (UITableViewCell *)sidebarCellForRowAtIndexPath:(NSIndexPath *)indexPath inTableView:(UITableView *)tableView {
    UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:kFileManagerSidebarCellReuseID];
    if (cell == nil)
        cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:kFileManagerSidebarCellReuseID];
    NSDictionary *row = [self sidebarRows][(NSUInteger)indexPath.row];
    cell.textLabel.text = row[@"title"];
    cell.textLabel.font = [UIFont systemFontOfSize:13.0];
    cell.textLabel.textColor = self.workspaceTheme[@"primary"];
    cell.imageView.image = [UIImage systemImageNamed:row[@"symbol"]];
    cell.imageView.tintColor = self.workspaceTheme[@"accent"];
    cell.backgroundColor = UIColor.clearColor;
    cell.accessoryType = UITableViewCellAccessoryNone;
    return cell;
}

- (UITableViewCell *)fileCellForRowAtIndexPath:(NSIndexPath *)indexPath inTableView:(UITableView *)tableView {
    UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:kFileManagerCellReuseID];
    if (cell == nil)
        cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:kFileManagerCellReuseID];
    if ((NSUInteger)indexPath.row >= _items.count)
        return cell;

    ISHGuestFileItem *item = _items[(NSUInteger)indexPath.row];
    BOOL openable = [self itemIsOpenable:item];

    cell.textLabel.text = item.name;
    cell.textLabel.textColor = openable ? self.workspaceTheme[@"primary"] : self.workspaceTheme[@"secondary"];
    cell.detailTextLabel.text = [self subtitleForItem:item];
    cell.detailTextLabel.textColor = self.workspaceTheme[@"secondary"];
    cell.imageView.image = [self iconForItem:item];
    cell.imageView.tintColor = self.workspaceTheme[@"accent"];
    cell.backgroundColor = UIColor.clearColor;  // UITableViewCell defaults to opaque white; without this the theme's light-on-dark text is nearly invisible
    cell.accessoryType = (item.kind == ISHGuestFileKindDirectory) ? UITableViewCellAccessoryDisclosureIndicator : UITableViewCellAccessoryNone;
    cell.selectionStyle = openable ? UITableViewCellSelectionStyleDefault : UITableViewCellSelectionStyleNone;
    return cell;
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    [self becomeFirstResponder];  // re-anchor keyboard shortcuts here after the user worked elsewhere
    if (tableView == _sidebarTableView) {
        NSDictionary *row = [self sidebarRows][(NSUInteger)indexPath.row];
        [self navigateToPath:row[@"path"] pushHistory:YES];
        return;
    }
    if ((NSUInteger)indexPath.row >= _items.count) return;
    ISHGuestFileItem *item = _items[(NSUInteger)indexPath.row];
    if (![self itemIsOpenable:item]) return;
    if (item.kind == ISHGuestFileKindDirectory) {
        [self navigateToPath:item.guestPath pushHistory:YES];
        return;
    }
    [self openFileItem:item];
}

- (nullable UIContextMenuConfiguration *)tableView:(UITableView *)tableView
             contextMenuConfigurationForRowAtIndexPath:(NSIndexPath *)indexPath
                                                  point:(CGPoint)point {
    if (tableView == _sidebarTableView) return nil;
    if ((NSUInteger)indexPath.row >= _items.count) return nil;
    ISHGuestFileItem *item = _items[(NSUInteger)indexPath.row];
    __weak typeof(self) weakSelf = self;
    return [UIContextMenuConfiguration configurationWithIdentifier:nil previewProvider:nil
        actionProvider:^UIMenu *(NSArray<UIMenuElement *> *suggestedActions) {
        typeof(self) strongSelf = weakSelf;
        if (strongSelf == nil) return nil;
        return [strongSelf contextMenuForItem:item];
    }];
}

- (UIMenu *)contextMenuForItem:(ISHGuestFileItem *)item {
    NSMutableArray<UIMenuElement *> *actions = [NSMutableArray array];
    __weak typeof(self) weakSelf = self;

    if ([self itemIsOpenable:item]) {
        [actions addObject:[UIAction actionWithTitle:@"Open" image:[UIImage systemImageNamed:@"arrow.up.forward.square"]
                                           identifier:nil handler:^(UIAction *action) {
            typeof(self) strongSelf = weakSelf;
            if (strongSelf == nil) return;
            if (item.kind == ISHGuestFileKindDirectory) [strongSelf navigateToPath:item.guestPath pushHistory:YES];
            else [strongSelf openFileItem:item];
        }]];
    }
    if (item.kind == ISHGuestFileKindRegular) {
        [actions addObject:[UIAction actionWithTitle:@"Duplicate" image:[UIImage systemImageNamed:@"plus.square.on.square"]
                                           identifier:nil handler:^(UIAction *action) {
            [weakSelf duplicateItem:item];
        }]];
    }
    if (item.kind == ISHGuestFileKindDirectory) {
        [actions addObject:[UIAction actionWithTitle:@"Add Folder to Music" image:[UIImage systemImageNamed:@"music.note.list"]
                                           identifier:nil handler:^(UIAction *action) {
            [weakSelf addFolderToMusic:item];
        }]];
    }
    [actions addObject:[UIAction actionWithTitle:@"Rename…" image:[UIImage systemImageNamed:@"pencil"]
                                       identifier:nil handler:^(UIAction *action) {
        [weakSelf promptRenameItem:item];
    }]];
    [actions addObject:[UIAction actionWithTitle:@"Get Info" image:[UIImage systemImageNamed:@"info.circle"]
                                       identifier:nil handler:^(UIAction *action) {
        [weakSelf presentInfoForItem:item];
    }]];
    UIAction *deleteAction = [UIAction actionWithTitle:@"Delete" image:[UIImage systemImageNamed:@"trash"]
                                             identifier:nil handler:^(UIAction *action) {
        [weakSelf confirmDeleteItem:item];
    }];
    deleteAction.attributes = UIMenuElementAttributesDestructive;
    [actions addObject:deleteAction];
    return [UIMenu menuWithTitle:@"" children:actions];
}

- (nullable UISwipeActionsConfiguration *)tableView:(UITableView *)tableView
     trailingSwipeActionsConfigurationForRowAtIndexPath:(NSIndexPath *)indexPath {
    if (tableView == _sidebarTableView) return nil;
    if ((NSUInteger)indexPath.row >= _items.count) return nil;
    ISHGuestFileItem *item = _items[(NSUInteger)indexPath.row];
    __weak typeof(self) weakSelf = self;
    UIContextualAction *delete = [UIContextualAction contextualActionWithStyle:UIContextualActionStyleDestructive
                                                                          title:@"Delete"
                                                                        handler:^(UIContextualAction *action, UIView *sourceView, void (^completionHandler)(BOOL)) {
        [weakSelf confirmDeleteItem:item];
        completionHandler(YES);  // dismiss the swipe row now; the confirm alert handles the actual delete
    }];
    return [UISwipeActionsConfiguration configurationWithActions:@[delete]];
}

#pragma mark Row presentation helpers

- (BOOL)itemIsOpenable:(ISHGuestFileItem *)item {
    return item.kind == ISHGuestFileKindDirectory || item.kind == ISHGuestFileKindRegular;
}

- (nullable UIImage *)iconForItem:(ISHGuestFileItem *)item {
    NSString *symbolName;
    if (item.kind == ISHGuestFileKindDirectory) {
        symbolName = @"folder";
    } else if (item.kind == ISHGuestFileKindRegular) {
        NSString *ext = item.name.pathExtension.lowercaseString;
        if (ext.length > 0 && [ISHFileManagerImageExtensions() containsObject:ext])
            symbolName = @"photo";
        else if (ext.length > 0 && [ISHFileManagerVideoExtensions() containsObject:ext])
            symbolName = @"film";
        else if ([ISHAudioLibrary isSupportedAudioFileName:item.name])
            symbolName = @"music.note";
        else
            symbolName = @"doc.text";
    } else {
        symbolName = @"questionmark.square";
    }
    return [UIImage systemImageNamed:symbolName];
}

- (nullable NSString *)subtitleForItem:(ISHGuestFileItem *)item {
    if (item.isBrokenSymlink)
        return [NSString stringWithFormat:@"Broken alias → %@", item.symlinkTarget];
    if (item.kind == ISHGuestFileKindDirectory)
        return item.isSymlink ? [NSString stringWithFormat:@"Alias → %@", item.symlinkTarget] : nil;
    NSString *size = [self formattedSize:item.size];
    NSString *date = item.modificationDate ? [self formattedDate:item.modificationDate] : nil;
    return date ? [NSString stringWithFormat:@"%@ · %@", size, date] : size;
}

- (NSString *)formattedSize:(unsigned long long)size {
    static NSByteCountFormatter *formatter;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        formatter = [NSByteCountFormatter new];
        formatter.countStyle = NSByteCountFormatterCountStyleFile;
    });
    return [formatter stringFromByteCount:(long long)size];
}

- (NSString *)formattedDate:(NSDate *)date {
    static NSDateFormatter *formatter;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        formatter = [NSDateFormatter new];
        formatter.dateStyle = NSDateFormatterShortStyle;
        formatter.timeStyle = NSDateFormatterShortStyle;
    });
    return [formatter stringFromDate:date];
}

- (NSString *)kindDisplayNameForItem:(ISHGuestFileItem *)item {
    switch (item.kind) {
        case ISHGuestFileKindDirectory: return @"Folder";
        case ISHGuestFileKindRegular: return @"File";
        case ISHGuestFileKindSymlink: return @"Broken Alias";
        case ISHGuestFileKindFIFO: return @"Named Pipe";
        case ISHGuestFileKindSocket: return @"Socket";
        case ISHGuestFileKindCharDevice: return @"Character Device";
        case ISHGuestFileKindBlockDevice: return @"Block Device";
        case ISHGuestFileKindOther: default: return @"Item";
    }
}

- (NSString *)formattedPosixMode:(mode_t)mode {
    char permissions[10];
    static const char kPermissionChars[] = "rwxrwxrwx";
    for (int i = 0; i < 9; i++)
        permissions[i] = (mode & (1 << (8 - i))) ? kPermissionChars[i] : '-';
    permissions[9] = '\0';
    return [NSString stringWithFormat:@"%s (%o)", permissions, mode & 07777];
}

#pragma mark Opening files

// Markdown and image extensions get their dedicated viewers. Other known-text
// extensions skip the content sniff below and route straight to MotePad.
// Known-binary extensions with no viewer (archives/office/fonts) always
// report no route rather than guessing from content.
static NSSet<NSString *> *ISHFileManagerMarkdownExtensions(void) {
    static NSSet<NSString *> *set;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ set = [NSSet setWithArray:@[@"md", @"markdown"]]; });
    return set;
}

// Matches WorkspaceImageViewer.m's ISHImageViewerSupportedExtensions() --
// deliberately excludes svg (not rasterizable by the ImageIO decode path the
// viewer uses) and stays routed to nil until the Browser applet is wired for
// file-open.
static NSSet<NSString *> *ISHFileManagerImageExtensions(void) {
    static NSSet<NSString *> *set;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        set = [NSSet setWithArray:@[@"png", @"jpg", @"jpeg", @"gif", @"bmp", @"tiff", @"tif", @"heic", @"heif", @"webp", @"ico"]];
    });
    return set;
}

// The video player doesn't pre-filter by extension internally (it hands
// anything to AVPlayer and surfaces whatever real error AVFoundation
// reports), but the file manager still needs a list to decide whether a tap
// should open the player at all -- includes containers AVFoundation may not
// actually support (mkv/webm) so those get a real in-player error instead of
// a flat "no viewer" from here.
static NSSet<NSString *> *ISHFileManagerVideoExtensions(void) {
    static NSSet<NSString *> *set;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ set = [NSSet setWithArray:@[@"mp4", @"mov", @"m4v", @"avi", @"mkv", @"webm"]]; });
    return set;
}

static NSSet<NSString *> *ISHFileManagerKnownTextExtensions(void) {
    static NSSet<NSString *> *set;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        set = [NSSet setWithArray:@[
            @"txt", @"log", @"conf", @"cfg", @"config", @"json", @"yaml", @"yml",
            @"xml", @"csv", @"tsv", @"sh", @"bash", @"zsh", @"c", @"h", @"cpp", @"cc", @"hpp", @"m", @"mm",
            @"py", @"rb", @"js", @"ts", @"jsx", @"tsx", @"go", @"rs", @"java", @"swift", @"ini", @"toml",
            @"gitignore", @"env", @"plist", @"properties", @"gradle", @"make", @"mk", @"cmake", @"rst",
            @"tex", @"diff", @"patch",
        ]];
    });
    return set;
}

static NSSet<NSString *> *ISHFileManagerKnownBinaryExtensions(void) {
    static NSSet<NSString *> *set;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        set = [NSSet setWithArray:@[
            @"svg",
            @"zip", @"tar", @"gz", @"bz2", @"xz", @"7z", @"rar",
            @"pdf", @"doc", @"docx", @"xls", @"xlsx", @"ppt", @"pptx",
            @"so", @"dylib", @"dll", @"exe", @"o", @"a", @"bin",
            @"db", @"sqlite", @"sqlite3",
            @"ttf", @"otf", @"woff", @"woff2",
            @"html", @"htm",
        ]];
    });
    return set;
}

// The tool identifiers are hardcoded rather than referencing e.g.
// ISHWorkspaceToolMotePadIdentifier (those constants have internal linkage in
// WorkspaceViewController.m) -- these identifier strings are already a durable
// contract (they round-trip through NSUserDefaults for saved window layouts),
// so a literal here carries no more fragility than that existing constraint.
- (void)determineOpenRouteForItem:(ISHGuestFileItem *)item completion:(void (^)(NSString * _Nullable toolIdentifier))completion {
    NSString *ext = item.name.pathExtension.lowercaseString;
    if (ext.length > 0 && [ISHFileManagerMarkdownExtensions() containsObject:ext]) {
        completion(@"markdown");
        return;
    }
    if (ext.length > 0 && [ISHFileManagerImageExtensions() containsObject:ext]) {
        completion(@"imageviewer");
        return;
    }
    if (ext.length > 0 && [ISHFileManagerVideoExtensions() containsObject:ext]) {
        completion(@"videoplayer");
        return;
    }
    if ([ISHAudioLibrary isSupportedAudioFileName:item.name]) {
        completion(@"audio");
        return;
    }
    if (ext.length > 0 && [ISHFileManagerKnownTextExtensions() containsObject:ext]) {
        completion(@"motepad");
        return;
    }
    if (ext.length > 0 && [ISHFileManagerKnownBinaryExtensions() containsObject:ext]) {
        completion(nil);  // no viewer for this type yet
        return;
    }
    [ISHGuestFileBridge.sharedBridge readFileAtGuestPath:item.guestPath maxBytes:8192
                                               completion:^(NSData *data, NSError *error) {
        if (data == nil) { completion(nil); return; }
        completion([self dataLooksTextual:data] ? @"motepad" : nil);
    }];
}

- (BOOL)dataLooksTextual:(NSData *)data {
    const uint8_t *bytes = data.bytes;
    for (NSUInteger i = 0; i < data.length; i++) {
        if (bytes[i] == 0) return NO;
    }
    return YES;
}

// Single-quotes PATH for safe use as one shell word, escaping any embedded single quotes
// (foo'bar -> 'foo'\''bar'). Guest paths can contain spaces and other shell metacharacters.
static NSString *ISHShellQuotedPath(NSString *path) {
    NSString *escaped = [path stringByReplacingOccurrencesOfString:@"'" withString:@"'\\''"];
    return [NSString stringWithFormat:@"'%@'", escaped];
}

- (void)openFileItem:(ISHGuestFileItem *)item {
    // A regular file with any execute bit set is a program, not a document -- run it in a new
    // terminal (like double-clicking an executable in a real desktop file manager) instead of
    // routing it through the viewer/tool machinery below, which has nothing that can run it.
    if (item.kind == ISHGuestFileKindRegular && (item.posixMode & 0111) != 0) {
        [self.workspaceHostViewController launchTerminalWithCommand:ISHShellQuotedPath(item.guestPath)
                                                                title:item.name];
        return;
    }
    __weak typeof(self) weakSelf = self;
    [self determineOpenRouteForItem:item completion:^(NSString *toolIdentifier) {
        typeof(self) strongSelf = weakSelf;
        if (strongSelf == nil) return;
        if (toolIdentifier == nil) {
            [strongSelf presentSimpleAlertWithTitle:@"Can’t Open"
                                             message:[NSString stringWithFormat:@"There’s no app to open “%@” yet.", item.name]];
            return;
        }
        [strongSelf.workspaceHostViewController openWorkspaceToolWithIdentifier:toolIdentifier fileGuestPath:item.guestPath];
    }];
}

#pragma mark File operations

- (BOOL)validateItemName:(NSString *)name {
    if (name.length == 0) return NO;
    if ([name containsString:@"/"]) return NO;
    if ([name isEqualToString:@"."] || [name isEqualToString:@".."]) return NO;
    return YES;
}

- (void)promptNewFolder {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"New Folder" message:nil
                                                              preferredStyle:UIAlertControllerStyleAlert];
    [alert addTextFieldWithConfigurationHandler:^(UITextField *textField) {
        textField.text = @"untitled folder";
        textField.placeholder = @"Name";
    }];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    __weak typeof(self) weakSelf = self;
    __weak UIAlertController *weakAlert = alert;
    [alert addAction:[UIAlertAction actionWithTitle:@"Create" style:UIAlertActionStyleDefault handler:^(UIAlertAction *action) {
        typeof(self) strongSelf = weakSelf;
        if (strongSelf == nil) return;
        NSString *name = [weakAlert.textFields.firstObject.text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        if (![strongSelf validateItemName:name]) {
            [strongSelf presentSimpleAlertWithTitle:@"Invalid Name" message:@"That name isn’t valid."];
            return;
        }
        NSString *path = [strongSelf->_currentPath stringByAppendingPathComponent:name];
        [ISHGuestFileBridge.sharedBridge createDirectoryAtGuestPath:path completion:^(BOOL ok, NSError *error) {
            typeof(self) strongSelf2 = weakSelf;
            if (strongSelf2 == nil) return;
            if (!ok) {
                [strongSelf2 presentSimpleAlertWithTitle:@"Couldn’t Create Folder" message:error.localizedDescription];
                return;
            }
            [strongSelf2 reload];
        }];
    }]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)promptRenameItem:(ISHGuestFileItem *)item {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Rename" message:nil
                                                              preferredStyle:UIAlertControllerStyleAlert];
    [alert addTextFieldWithConfigurationHandler:^(UITextField *textField) {
        textField.text = item.name;
        textField.placeholder = @"Name";
    }];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    __weak typeof(self) weakSelf = self;
    __weak UIAlertController *weakAlert = alert;
    [alert addAction:[UIAlertAction actionWithTitle:@"Rename" style:UIAlertActionStyleDefault handler:^(UIAlertAction *action) {
        typeof(self) strongSelf = weakSelf;
        if (strongSelf == nil) return;
        NSString *newName = [weakAlert.textFields.firstObject.text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        if (![strongSelf validateItemName:newName] || [newName isEqualToString:item.name])
            return;
        NSString *destinationPath = [item.guestPath.stringByDeletingLastPathComponent stringByAppendingPathComponent:newName];
        [ISHGuestFileBridge.sharedBridge moveItemAtGuestPath:item.guestPath toGuestPath:destinationPath
                                                    completion:^(BOOL ok, NSError *error) {
            typeof(self) strongSelf2 = weakSelf;
            if (strongSelf2 == nil) return;
            if (!ok) {
                [strongSelf2 presentSimpleAlertWithTitle:@"Couldn’t Rename" message:error.localizedDescription];
                return;
            }
            [strongSelf2 reload];
        }];
    }]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (NSString *)uniqueDuplicateNameForItem:(ISHGuestFileItem *)item {
    NSString *base = item.name.stringByDeletingPathExtension;
    NSString *ext = item.name.pathExtension;
    NSSet<NSString *> *existingNames = [NSSet setWithArray:[_allItems valueForKey:@"name"]];
    NSString *candidate = ext.length ? [[base stringByAppendingString:@" copy"] stringByAppendingPathExtension:ext]
                                      : [base stringByAppendingString:@" copy"];
    NSUInteger suffix = 2;
    while ([existingNames containsObject:candidate]) {
        NSString *stem = [NSString stringWithFormat:@"%@ copy %lu", base, (unsigned long)suffix];
        candidate = ext.length ? [stem stringByAppendingPathExtension:ext] : stem;
        suffix++;
    }
    return candidate;
}

- (void)duplicateItem:(ISHGuestFileItem *)item {
    if (item.kind != ISHGuestFileKindRegular) {
        [self presentSimpleAlertWithTitle:@"Can’t Duplicate" message:@"Duplicating folders isn’t supported yet."];
        return;
    }
    NSString *destinationPath = [_currentPath stringByAppendingPathComponent:[self uniqueDuplicateNameForItem:item]];
    __weak typeof(self) weakSelf = self;
    [ISHGuestFileBridge.sharedBridge copyItemAtGuestPath:item.guestPath toGuestPath:destinationPath
                                                completion:^(BOOL ok, NSError *error) {
        typeof(self) strongSelf = weakSelf;
        if (strongSelf == nil) return;
        if (!ok) {
            [strongSelf presentSimpleAlertWithTitle:@"Couldn’t Duplicate" message:error.localizedDescription];
            return;
        }
        [strongSelf reload];
    }];
}

// scanGuestDirectoryAtPath: is a blocking VFS walk (borrows the guest task
// context, like every other GuestFileBridge/AudioLibrary operation) -- run it
// off the main thread, matching how AudioPlayerEngine.m itself resolves
// tracks on its own decode queue rather than main.
- (void)addFolderToMusic:(ISHGuestFileItem *)item {
    __weak typeof(self) weakSelf = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSArray<ISHAudioTrack *> *tracks = [ISHAudioLibrary.sharedLibrary scanGuestDirectoryAtPath:item.guestPath];
        dispatch_async(dispatch_get_main_queue(), ^{
            typeof(self) strongSelf = weakSelf;
            if (strongSelf == nil) return;
            if (tracks.count == 0) {
                [strongSelf presentSimpleAlertWithTitle:@"No Audio Found"
                                                 message:[NSString stringWithFormat:@"“%@” doesn’t contain any supported audio files.", item.name]];
                return;
            }
            [ISHAudioPlayerEngine.sharedEngine enqueueTracks:tracks];
            [strongSelf presentSimpleAlertWithTitle:@"Added to Music"
                                             message:[NSString stringWithFormat:@"Added %lu track%@ to the Music queue.",
                                                       (unsigned long)tracks.count, tracks.count == 1 ? @"" : @"s"]];
        });
    });
}

- (void)confirmDeleteItem:(ISHGuestFileItem *)item {
    BOOL isDirectory = item.kind == ISHGuestFileKindDirectory;
    UIAlertController *alert = [UIAlertController
        alertControllerWithTitle:[NSString stringWithFormat:@"Delete “%@”?", item.name]
                          message:(isDirectory ? @"This will permanently delete the folder and everything inside it."
                                                : @"This will permanently delete the file.")
                   preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    __weak typeof(self) weakSelf = self;
    [alert addAction:[UIAlertAction actionWithTitle:@"Delete" style:UIAlertActionStyleDestructive handler:^(UIAlertAction *action) {
        typeof(self) strongSelf = weakSelf;
        if (strongSelf == nil) return;
        [ISHGuestFileBridge.sharedBridge removeItemAtGuestPath:item.guestPath recursive:YES completion:^(BOOL ok, NSError *error) {
            typeof(self) strongSelf2 = weakSelf;
            if (strongSelf2 == nil) return;
            if (!ok) {
                [strongSelf2 presentSimpleAlertWithTitle:@"Couldn’t Delete" message:error.localizedDescription];
                return;
            }
            [strongSelf2 reload];
        }];
    }]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)presentInfoForItem:(ISHGuestFileItem *)item {
    NSMutableString *message = [NSMutableString string];
    [message appendFormat:@"Kind: %@\n", [self kindDisplayNameForItem:item]];
    if (item.kind != ISHGuestFileKindDirectory)
        [message appendFormat:@"Size: %@\n", [self formattedSize:item.size]];
    if (item.modificationDate != nil)
        [message appendFormat:@"Modified: %@\n", [self formattedDate:item.modificationDate]];
    [message appendFormat:@"Permissions: %@\n", [self formattedPosixMode:item.posixMode]];
    [message appendFormat:@"Owner: %u:%u\n", item.uid, item.gid];
    if (item.isSymlink)
        [message appendFormat:@"%@ → %@\n", (item.isBrokenSymlink ? @"Broken link" : @"Alias"), item.symlinkTarget];
    [message appendFormat:@"Path: %@", item.guestPath];
    [self presentSimpleAlertWithTitle:item.name message:message];
}

- (void)presentSimpleAlertWithTitle:(NSString *)title message:(nullable NSString *)message {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:title
                                                                     message:message.length ? message : nil
                                                              preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}

#pragma mark More menu (New Folder / Sort / Hidden files / Refresh)

- (void)refreshMoreMenu {
    _moreButton.menu = [self buildMoreMenu];
}

- (UIMenu *)buildMoreMenu {
    __weak typeof(self) weakSelf = self;

    UIAction *newFolder = [UIAction actionWithTitle:@"New Folder" image:[UIImage systemImageNamed:@"folder.badge.plus"]
                                          identifier:nil handler:^(UIAction *action) {
        [weakSelf promptNewFolder];
    }];

    NSArray<NSNumber *> *sortModes = @[@(WorkspaceFileManagerSortName), @(WorkspaceFileManagerSortSize),
                                        @(WorkspaceFileManagerSortDate), @(WorkspaceFileManagerSortKind)];
    NSArray<NSString *> *sortTitles = @[@"Name", @"Size", @"Date Modified", @"Kind"];
    NSMutableArray<UIAction *> *sortActions = [NSMutableArray array];
    for (NSUInteger i = 0; i < sortModes.count; i++) {
        WorkspaceFileManagerSortMode mode = (WorkspaceFileManagerSortMode)sortModes[i].integerValue;
        UIAction *sortAction = [UIAction actionWithTitle:sortTitles[i] image:nil identifier:nil handler:^(UIAction *action) {
            typeof(self) strongSelf = weakSelf;
            if (strongSelf == nil) return;
            strongSelf->_sortMode = mode;
            [NSUserDefaults.standardUserDefaults setInteger:mode forKey:kFileManagerSortModeDefaultsKey];
            [strongSelf applyFilterAndSort];
            [strongSelf refreshMoreMenu];
        }];
        sortAction.state = (_sortMode == mode) ? UIMenuElementStateOn : UIMenuElementStateOff;
        [sortActions addObject:sortAction];
    }
    UIMenu *sortMenu = [UIMenu menuWithTitle:@"Sort By" image:[UIImage systemImageNamed:@"arrow.up.arrow.down"]
                                   identifier:nil options:0 children:sortActions];

    UIAction *hiddenToggle = [UIAction actionWithTitle:@"Show Hidden Files" image:nil identifier:nil
                                                handler:^(UIAction *action) {
        [weakSelf toggleHiddenFiles];
    }];
    hiddenToggle.state = _showHidden ? UIMenuElementStateOn : UIMenuElementStateOff;

    UIAction *refresh = [UIAction actionWithTitle:@"Refresh" image:[UIImage systemImageNamed:@"arrow.clockwise"]
                                        identifier:nil handler:^(UIAction *action) {
        [weakSelf reload];
    }];

    return [UIMenu menuWithTitle:@"" children:@[newFolder, sortMenu, hiddenToggle, refresh]];
}

@end
