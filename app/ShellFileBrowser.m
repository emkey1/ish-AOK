//
//  ShellFileBrowser.m
//  iSH-AOK
//

#import "ShellFileBrowser.h"
#import "GuestFileBridge.h"

NS_ASSUME_NONNULL_BEGIN

static NSString *const kShellFileBrowserCellReuseID = @"ShellFileBrowserCell";
static NSString *const kShellFileBrowserShowHiddenKey = @"ISHShellFileBrowserShowsHiddenFiles";

#pragma mark - Shell quoting

NSString *ISHShellQuoteArgument(NSString *argument) {
    if (argument.length == 0)
        return @"''";
    // Conservative allow-list. Everything outside it gets quoted, including
    // characters a shell would tolerate bare, because over-quoting only costs
    // a pair of quotes while under-quoting silently corrupts a command line.
    static NSCharacterSet *unsafe;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSString *safe = @"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_./-+=:,@%";
        unsafe = [[NSCharacterSet characterSetWithCharactersInString:safe] invertedSet];
    });
    if ([argument rangeOfCharacterFromSet:unsafe].location == NSNotFound)
        return argument;
    // Inside single quotes a shell honours no escapes at all, so a literal
    // single quote has to leave the quoted run, emit an escaped quote, and
    // start a new run: '\'' -- which is what this produces.
    NSString *escaped = [argument stringByReplacingOccurrencesOfString:@"'" withString:@"'\\''"];
    return [NSString stringWithFormat:@"'%@'", escaped];
}

#pragma mark - Browser

@interface ISHShellFileBrowserViewController () <UITableViewDataSource, UITableViewDelegate>
@end

@implementation ISHShellFileBrowserViewController {
    NSString *_path;
    NSArray<ISHGuestFileItem *> *_items;
    UITableView *_tableView;
    UIScrollView *_breadcrumbScroll;
    UIStackView *_breadcrumbStack;
    UILabel *_statusLabel;
    UIActivityIndicatorView *_spinner;
    // Bumped on every navigation so a slow listing that lands after the user
    // has already moved on is dropped instead of repainting the wrong folder.
    NSUInteger _loadGeneration;
    // The listing itself, so a superseded or abandoned one can be STOPPED and
    // not merely ignored. The generation guard above only discards the answer;
    // on a big directory the enumeration kept running regardless.
    ISHGuestFileOperationToken _listToken;
    BOOL _showsHiddenFiles;
}

#pragma mark Presentation

+ (void)presentFromViewController:(UIViewController *)presenter
                          ttyType:(int)ttyType
                        ttyNumber:(int)ttyNumber
                     fallbackPath:(nullable NSString *)fallbackPath
                         delegate:(nullable id<ISHShellFileBrowserDelegate>)delegate {
    ISHShellFileBrowserViewController *browser = [[self alloc] initWithPath:fallbackPath ?: @"/"];
    browser.delegate = delegate;
    UINavigationController *nav = [[UINavigationController alloc] initWithRootViewController:browser];
    nav.modalPresentationStyle = UIModalPresentationPageSheet;
    UISheetPresentationController *sheet = nav.sheetPresentationController;
    if (sheet != nil) {
        sheet.detents = @[[UISheetPresentationControllerDetent mediumDetent],
                          [UISheetPresentationControllerDetent largeDetent]];
        sheet.prefersGrabberVisible = YES;
        // Without this, a drag anywhere in the file list grows the sheet to
        // full height instead of scrolling the list, which makes the medium
        // detent -- the whole point of a browser you glance at over a live
        // terminal -- nearly impossible to stay in.
        sheet.prefersScrollingExpandsWhenScrolledToEdge = NO;
    }
    [presenter presentViewController:nav animated:YES completion:nil];

    // Ask where the shell is standing, and go there if the answer arrives
    // before the user has navigated somewhere themselves.
    [[ISHGuestFileBridge sharedBridge] foregroundDirectoryForTTYType:ttyType
                                                              number:ttyNumber
                                                          completion:^(NSString *guestPath) {
        if (guestPath != nil)
            [browser adoptInitialPath:guestPath];
    }];
}

- (instancetype)initWithPath:(NSString *)path {
    self = [super initWithNibName:nil bundle:nil];
    if (self != nil) {
        _path = [path copy];
        _items = @[];
        _showsHiddenFiles = [NSUserDefaults.standardUserDefaults boolForKey:kShellFileBrowserShowHiddenKey];
    }
    return self;
}

// The cwd lookup races the first listing. Only honour it while the browser is
// still showing the folder it opened on and the user has not navigated -- a
// late answer must never yank the ground out from under them.
- (void)adoptInitialPath:(NSString *)path {
    if (_loadGeneration > 1 || [path isEqualToString:_path])
        return;
    [self navigateToPath:path];
}

#pragma mark View lifecycle

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.systemBackgroundColor;
    self.navigationItem.leftBarButtonItem =
        [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemDone
                                                      target:self
                                                      action:@selector(dismissBrowser:)];
    [self buildBreadcrumbBar];
    [self buildTableView];
    [self buildToolbar];
    [self updateOverflowMenu];
    [self reload];
}

- (void)buildBreadcrumbBar {
    _breadcrumbScroll = [[UIScrollView alloc] init];
    _breadcrumbScroll.translatesAutoresizingMaskIntoConstraints = NO;
    _breadcrumbScroll.showsHorizontalScrollIndicator = NO;
    _breadcrumbScroll.alwaysBounceHorizontal = YES;
    [self.view addSubview:_breadcrumbScroll];

    _breadcrumbStack = [[UIStackView alloc] init];
    _breadcrumbStack.translatesAutoresizingMaskIntoConstraints = NO;
    _breadcrumbStack.axis = UILayoutConstraintAxisHorizontal;
    _breadcrumbStack.alignment = UIStackViewAlignmentCenter;
    _breadcrumbStack.spacing = 2;
    [_breadcrumbScroll addSubview:_breadcrumbStack];

    UIView *separator = [[UIView alloc] init];
    separator.translatesAutoresizingMaskIntoConstraints = NO;
    separator.backgroundColor = UIColor.separatorColor;
    [self.view addSubview:separator];

    UILayoutGuide *safe = self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [_breadcrumbScroll.topAnchor constraintEqualToAnchor:safe.topAnchor],
        [_breadcrumbScroll.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor],
        [_breadcrumbScroll.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor],
        [_breadcrumbScroll.heightAnchor constraintEqualToConstant:38],

        [_breadcrumbStack.topAnchor constraintEqualToAnchor:_breadcrumbScroll.topAnchor],
        [_breadcrumbStack.bottomAnchor constraintEqualToAnchor:_breadcrumbScroll.bottomAnchor],
        [_breadcrumbStack.leadingAnchor constraintEqualToAnchor:_breadcrumbScroll.leadingAnchor constant:12],
        [_breadcrumbStack.trailingAnchor constraintEqualToAnchor:_breadcrumbScroll.trailingAnchor constant:-12],
        [_breadcrumbStack.heightAnchor constraintEqualToAnchor:_breadcrumbScroll.heightAnchor],

        [separator.topAnchor constraintEqualToAnchor:_breadcrumbScroll.bottomAnchor],
        [separator.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
        [separator.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
        [separator.heightAnchor constraintEqualToConstant:1.0 / MAX(self.traitCollection.displayScale, 1.0)],
    ]];
}

- (void)buildTableView {
    _tableView = [[UITableView alloc] initWithFrame:CGRectZero style:UITableViewStylePlain];
    _tableView.translatesAutoresizingMaskIntoConstraints = NO;
    _tableView.dataSource = self;
    _tableView.delegate = self;
    _tableView.rowHeight = 52;
    [self.view addSubview:_tableView];

    UILayoutGuide *safe = self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [_tableView.topAnchor constraintEqualToAnchor:_breadcrumbScroll.bottomAnchor constant:1],
        [_tableView.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
        [_tableView.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
        [_tableView.bottomAnchor constraintEqualToAnchor:safe.bottomAnchor],
    ]];

    _spinner = [[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleMedium];
    _spinner.hidesWhenStopped = YES;
    _spinner.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:_spinner];
    [NSLayoutConstraint activateConstraints:@[
        [_spinner.centerXAnchor constraintEqualToAnchor:_tableView.centerXAnchor],
        [_spinner.centerYAnchor constraintEqualToAnchor:_tableView.centerYAnchor],
    ]];

    _statusLabel = [[UILabel alloc] init];
    _statusLabel.translatesAutoresizingMaskIntoConstraints = NO;
    _statusLabel.textAlignment = NSTextAlignmentCenter;
    _statusLabel.textColor = UIColor.secondaryLabelColor;
    _statusLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleSubheadline];
    _statusLabel.numberOfLines = 0;
    _statusLabel.hidden = YES;
    [self.view addSubview:_statusLabel];
    [NSLayoutConstraint activateConstraints:@[
        [_statusLabel.centerYAnchor constraintEqualToAnchor:_tableView.centerYAnchor],
        [_statusLabel.leadingAnchor constraintEqualToAnchor:_tableView.leadingAnchor constant:24],
        [_statusLabel.trailingAnchor constraintEqualToAnchor:_tableView.trailingAnchor constant:-24],
    ]];
}

- (void)buildToolbar {
    self.navigationController.toolbarHidden = NO;
    // Titles, not icons: "cd" and "insert the path" are verbs no glyph conveys.
    // (initWithTitle:image:target:action:menu: would give both, but it is iOS
    // 16+ and this app still supports 15.)
    UIBarButtonItem *cd = [[UIBarButtonItem alloc] initWithTitle:@"cd Here"
                                                            style:UIBarButtonItemStylePlain
                                                           target:self
                                                           action:@selector(changeDirectoryHere:)];
    UIBarButtonItem *insert = [[UIBarButtonItem alloc] initWithTitle:@"Insert Path"
                                                                style:UIBarButtonItemStylePlain
                                                               target:self
                                                               action:@selector(insertCurrentDirectoryPath:)];
    UIBarButtonItem *flex = [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemFlexibleSpace
                                                                          target:nil
                                                                          action:nil];
    self.toolbarItems = @[cd, flex, insert];
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    self.navigationController.toolbarHidden = NO;
}

- (void)viewDidDisappear:(BOOL)animated {
    [super viewDidDisappear:animated];
    // Only when the whole sheet is going away -- not when a child (Get Info,
    // a rename prompt) is covering us.
    if (self.isBeingDismissed || self.navigationController.isBeingDismissed) {
        // The sheet is gone; a listing still walking /usr/bin is pure waste and
        // it is holding the lane the next browser will want.
        [self cancelPendingListing];
        if ([self.delegate respondsToSelector:@selector(shellFileBrowserDidDismiss:)])
            [self.delegate shellFileBrowserDidDismiss:self];
    }
}

#pragma mark Navigation

- (void)navigateToPath:(NSString *)path {
    _path = [path copy];
    [self reload];
}

- (void)reload {
    _loadGeneration += 1;
    NSUInteger generation = _loadGeneration;
    NSString *path = _path;
    [self cancelPendingListing];
    self.title = path.lastPathComponent.length > 0 ? path.lastPathComponent : @"/";
    [self rebuildBreadcrumb];
    _statusLabel.hidden = YES;

    // Drop the old rows before the new listing arrives, and always spin.
    //
    // The title and breadcrumb above are updated synchronously, so leaving the
    // previous folder's rows on screen showed them UNDER A HEADER NAMING A
    // DIFFERENT DIRECTORY -- and they stayed live: a tap inserted a path from
    // the folder you had just left, and a long-press Delete confirmed against
    // it. That is a wrong-file hazard, not a cosmetic one.
    //
    // Spinning only when the list happened to be empty made the same moment
    // look like nothing was happening at all, which on a big directory reads
    // as a hang.
    _items = @[];
    [_tableView reloadData];
    [_spinner startAnimating];

    __weak typeof(self) weakSelf = self;
    _listToken = [[ISHGuestFileBridge sharedBridge] listDirectoryAtGuestPath:path completion:^(NSArray<ISHGuestFileItem *> *items, NSError *error) {
        typeof(self) self_ = weakSelf;
        if (self_ == nil || generation != self_->_loadGeneration)
            return;
        self_->_listToken = nil;
        [self_ applyListing:items error:error];
    }];
}

// Cancelling always completes the listing, with ISHGuestFileBridgeErrorCancelled.
// That completion is dropped by the generation guard above, since anything that
// cancels has already bumped the generation or is on its way out.
- (void)cancelPendingListing {
    if (_listToken == nil)
        return;
    [[ISHGuestFileBridge sharedBridge] cancelOperation:_listToken];
    _listToken = nil;
}

- (void)dealloc {
    [self cancelPendingListing];
}

- (void)applyListing:(nullable NSArray<ISHGuestFileItem *> *)items error:(nullable NSError *)error {
    [_spinner stopAnimating];
    if (error != nil) {
        _items = @[];
        [_tableView reloadData];
        _statusLabel.text = [NSString stringWithFormat:@"Can't open %@\n%@", _path, error.localizedDescription];
        _statusLabel.hidden = NO;
        return;
    }
    _items = [self sortedVisibleItems:items ?: @[]];
    [_tableView reloadData];
    if (_items.count == 0) {
        _statusLabel.text = _showsHiddenFiles ? @"Empty folder" : @"Empty folder\n(hidden files are not shown)";
        _statusLabel.hidden = NO;
    } else {
        _statusLabel.hidden = YES;
        [_tableView setContentOffset:CGPointZero animated:NO];
    }
}

- (NSArray<ISHGuestFileItem *> *)sortedVisibleItems:(NSArray<ISHGuestFileItem *> *)items {
    NSMutableArray<ISHGuestFileItem *> *visible = [NSMutableArray arrayWithCapacity:items.count];
    for (ISHGuestFileItem *item in items) {
        if (!_showsHiddenFiles && [item.name hasPrefix:@"."])
            continue;
        [visible addObject:item];
    }
    // Folders first, then case-insensitive by name -- the order every file
    // browser on this platform uses, and not the order the VFS yields.
    [visible sortUsingComparator:^NSComparisonResult(ISHGuestFileItem *a, ISHGuestFileItem *b) {
        BOOL aDir = a.kind == ISHGuestFileKindDirectory;
        BOOL bDir = b.kind == ISHGuestFileKindDirectory;
        if (aDir != bDir)
            return aDir ? NSOrderedAscending : NSOrderedDescending;
        return [a.name localizedStandardCompare:b.name];
    }];
    return visible;
}

- (void)rebuildBreadcrumb {
    for (UIView *view in _breadcrumbStack.arrangedSubviews.copy) {
        [_breadcrumbStack removeArrangedSubview:view];
        [view removeFromSuperview];
    }
    NSMutableArray<NSString *> *titles = [NSMutableArray arrayWithObject:@"/"];
    NSMutableArray<NSString *> *paths = [NSMutableArray arrayWithObject:@"/"];
    NSString *walk = @"/";
    for (NSString *component in _path.pathComponents) {
        if ([component isEqualToString:@"/"])
            continue;
        walk = [walk stringByAppendingPathComponent:component];
        [titles addObject:component];
        [paths addObject:walk];
    }
    for (NSUInteger i = 0; i < titles.count; i++) {
        if (i > 0) {
            UILabel *chevron = [[UILabel alloc] init];
            chevron.text = @"›";
            chevron.textColor = UIColor.tertiaryLabelColor;
            chevron.font = [UIFont systemFontOfSize:15];
            [_breadcrumbStack addArrangedSubview:chevron];
        }
        BOOL isLast = (i == titles.count - 1);
        UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
        [button setTitle:titles[i] forState:UIControlStateNormal];
        button.titleLabel.font = [UIFont systemFontOfSize:15
                                                   weight:isLast ? UIFontWeightSemibold : UIFontWeightRegular];
        [button setTitleColor:isLast ? UIColor.labelColor : UIColor.systemBlueColor
                     forState:UIControlStateNormal];
        button.accessibilityHint = isLast ? nil : @"Go to this folder";
        NSString *destination = paths[i];
        __weak typeof(self) weakSelf = self;
        [button addAction:[UIAction actionWithHandler:^(UIAction *action) {
            [weakSelf navigateToPath:destination];
        }] forControlEvents:UIControlEventPrimaryActionTriggered];
        [_breadcrumbStack addArrangedSubview:button];
    }
    // Show the deepest component, which is the one the user is standing in.
    [_breadcrumbScroll layoutIfNeeded];
    CGFloat overflow = _breadcrumbScroll.contentSize.width - _breadcrumbScroll.bounds.size.width;
    if (overflow > 0)
        [_breadcrumbScroll setContentOffset:CGPointMake(overflow, 0) animated:NO];
}

#pragma mark Overflow menu

- (void)updateOverflowMenu {
    __weak typeof(self) weakSelf = self;
    UIAction *newFolder = [UIAction actionWithTitle:@"New Folder"
                                              image:[UIImage systemImageNamed:@"folder.badge.plus"]
                                         identifier:nil
                                            handler:^(UIAction *action) { [weakSelf promptNewFolder]; }];
    UIAction *hidden = [UIAction actionWithTitle:@"Show Hidden Files"
                                           image:[UIImage systemImageNamed:@"eye"]
                                      identifier:nil
                                         handler:^(UIAction *action) { [weakSelf toggleShowsHiddenFiles]; }];
    hidden.state = _showsHiddenFiles ? UIMenuElementStateOn : UIMenuElementStateOff;
    UIAction *refresh = [UIAction actionWithTitle:@"Refresh"
                                            image:[UIImage systemImageNamed:@"arrow.clockwise"]
                                       identifier:nil
                                          handler:^(UIAction *action) { [weakSelf reload]; }];
    UIMenu *menu = [UIMenu menuWithTitle:@"" children:@[newFolder, hidden, refresh]];
    self.navigationItem.rightBarButtonItem =
        [[UIBarButtonItem alloc] initWithImage:[UIImage systemImageNamed:@"ellipsis.circle"]
                                          menu:menu];
}

- (void)toggleShowsHiddenFiles {
    _showsHiddenFiles = !_showsHiddenFiles;
    [NSUserDefaults.standardUserDefaults setBool:_showsHiddenFiles forKey:kShellFileBrowserShowHiddenKey];
    [self updateOverflowMenu];
    [self reload];
}

#pragma mark Terminal verbs

- (void)dismissBrowser:(id)sender {
    [self dismissViewControllerAnimated:YES completion:nil];
}

- (void)sendToTerminal:(NSString *)text dismiss:(BOOL)dismiss {
    [self.delegate shellFileBrowser:self insertText:text];
    if (dismiss)
        [self dismissViewControllerAnimated:YES completion:nil];
}

- (void)changeDirectoryHere:(id)sender {
    [self sendToTerminal:[NSString stringWithFormat:@"cd %@\n", ISHShellQuoteArgument(_path)] dismiss:YES];
}

- (void)insertCurrentDirectoryPath:(id)sender {
    [self sendToTerminal:ISHShellQuoteArgument(_path) dismiss:YES];
}

- (void)insertPathOfItem:(ISHGuestFileItem *)item {
    // A trailing space: a path is almost always one argument among several,
    // and typing the separator by hand right after a tap defeats the point.
    [self sendToTerminal:[ISHShellQuoteArgument(item.guestPath) stringByAppendingString:@" "] dismiss:YES];
}

#pragma mark File operations

- (void)promptNewFolder {
    __weak typeof(self) weakSelf = self;
    [self promptForName:@"New Folder"
                message:[NSString stringWithFormat:@"Create a folder in %@", _path]
            initialText:@"untitled folder"
             actionName:@"Create"
             completion:^(NSString *name) {
        typeof(self) self_ = weakSelf;
        if (self_ == nil)
            return;
        NSString *destination = [self_->_path stringByAppendingPathComponent:name];
        [[ISHGuestFileBridge sharedBridge] createDirectoryAtGuestPath:destination completion:^(BOOL ok, NSError *error) {
            if (!ok)
                [weakSelf presentError:error title:@"Couldn't Create Folder"];
            else
                [weakSelf reload];
        }];
    }];
}

// "notes.txt" -> "notes copy.txt", then "notes copy 2.txt". Picked against the
// names already on screen, so it does not silently overwrite a sibling -- the
// bridge's copy has no O_EXCL and would happily clobber one.
- (NSString *)duplicateNameFor:(ISHGuestFileItem *)item {
    NSString *ext = item.name.pathExtension;
    NSString *base = item.name.stringByDeletingPathExtension;
    NSMutableSet<NSString *> *taken = [NSMutableSet set];
    for (ISHGuestFileItem *sibling in _items)
        [taken addObject:sibling.name];
    for (NSUInteger n = 1; n < 1000; n++) {
        NSString *stem = (n == 1) ? [base stringByAppendingString:@" copy"]
                                  : [NSString stringWithFormat:@"%@ copy %lu", base, (unsigned long) n];
        NSString *candidate = ext.length > 0 ? [stem stringByAppendingPathExtension:ext] : stem;
        if (![taken containsObject:candidate])
            return candidate;
    }
    return [base stringByAppendingFormat:@" copy %@", [NSUUID UUID].UUIDString];
}

- (void)duplicateItem:(ISHGuestFileItem *)item {
    NSString *destination = [[item.guestPath stringByDeletingLastPathComponent]
                             stringByAppendingPathComponent:[self duplicateNameFor:item]];
    __weak typeof(self) weakSelf = self;
    [[ISHGuestFileBridge sharedBridge] copyItemAtGuestPath:item.guestPath
                                              toGuestPath:destination
                                               completion:^(BOOL ok, NSError *error) {
        if (!ok)
            [weakSelf presentError:error title:@"Couldn't Duplicate"];
        else
            [weakSelf reload];
    }];
}

- (void)promptRenameItem:(ISHGuestFileItem *)item {
    __weak typeof(self) weakSelf = self;
    [self promptForName:@"Rename"
                message:item.name
            initialText:item.name
             actionName:@"Rename"
             completion:^(NSString *name) {
        typeof(self) self_ = weakSelf;
        if (self_ == nil || [name isEqualToString:item.name])
            return;
        NSString *destination = [[item.guestPath stringByDeletingLastPathComponent] stringByAppendingPathComponent:name];
        [[ISHGuestFileBridge sharedBridge] moveItemAtGuestPath:item.guestPath
                                                    toGuestPath:destination
                                                     completion:^(BOOL ok, NSError *error) {
            if (!ok)
                [weakSelf presentError:error title:@"Couldn't Rename"];
            else
                [weakSelf reload];
        }];
    }];
}

- (void)confirmDeleteItem:(ISHGuestFileItem *)item {
    BOOL isDirectory = item.kind == ISHGuestFileKindDirectory && !item.isSymlink;
    NSString *message = isDirectory
        ? [NSString stringWithFormat:@"Delete “%@” and everything in it? This can't be undone.", item.name]
        : [NSString stringWithFormat:@"Delete “%@”? This can't be undone.", item.name];
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Delete"
                                                                   message:message
                                                            preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    __weak typeof(self) weakSelf = self;
    [alert addAction:[UIAlertAction actionWithTitle:@"Delete" style:UIAlertActionStyleDestructive handler:^(UIAlertAction *action) {
        [[ISHGuestFileBridge sharedBridge] removeItemAtGuestPath:item.guestPath
                                                        recursive:isDirectory
                                                       completion:^(BOOL ok, NSError *error) {
            if (!ok)
                [weakSelf presentError:error title:@"Couldn't Delete"];
            else
                [weakSelf reload];
        }];
    }]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)showInfoForItem:(ISHGuestFileItem *)item {
    NSMutableString *body = [NSMutableString string];
    [body appendFormat:@"%@\n\n", item.guestPath];
    [body appendFormat:@"Kind: %@\n", [self kindDescriptionForItem:item]];
    if (item.kind != ISHGuestFileKindDirectory)
        [body appendFormat:@"Size: %@\n", [self formattedSize:item.size]];
    [body appendFormat:@"Permissions: %@\n", [self modeStringForItem:item]];
    [body appendFormat:@"Owner: %u:%u\n", (unsigned)item.uid, (unsigned)item.gid];
    if (item.modificationDate != nil)
        [body appendFormat:@"Modified: %@\n", [NSDateFormatter localizedStringFromDate:item.modificationDate
                                                                              dateStyle:NSDateFormatterMediumStyle
                                                                              timeStyle:NSDateFormatterShortStyle]];
    if (item.symlinkTarget != nil)
        [body appendFormat:@"Symlink to: %@\n", item.symlinkTarget];
    if (item.realfsURL != nil)
        [body appendString:@"Stored outside the rootfs (persists across reinstalls)\n"];

    UIAlertController *alert = [UIAlertController alertControllerWithTitle:item.name
                                                                   message:body
                                                            preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"Copy Path" style:UIAlertActionStyleDefault handler:^(UIAlertAction *action) {
        UIPasteboard.generalPasteboard.string = item.guestPath;
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Done" style:UIAlertActionStyleCancel handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)promptForName:(NSString *)title
              message:(nullable NSString *)message
          initialText:(NSString *)initialText
           actionName:(NSString *)actionName
           completion:(void (^)(NSString *name))completion {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:title
                                                                   message:message
                                                            preferredStyle:UIAlertControllerStyleAlert];
    // Hold the FIELD, not the alert. Capturing `alert` in a handler the alert
    // itself owns would leak it; going weak instead would leave the read
    // racing the dismissal that triggered it. A text field owns neither, so a
    // strong capture of it is both cycle-free and alive when the handler runs.
    __block UITextField *nameField = nil;
    [alert addTextFieldWithConfigurationHandler:^(UITextField *field) {
        field.text = initialText;
        field.autocapitalizationType = UITextAutocapitalizationTypeNone;
        field.autocorrectionType = UITextAutocorrectionTypeNo;
        field.clearButtonMode = UITextFieldViewModeWhileEditing;
        nameField = field;
    }];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    __weak typeof(self) weakSelf = self;
    [alert addAction:[UIAlertAction actionWithTitle:actionName style:UIAlertActionStyleDefault handler:^(UIAlertAction *action) {
        NSString *name = nameField.text ?: @"";
        name = [name stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        // "/" would silently write somewhere else entirely, so reject rather
        // than sanitise: a name the user did not type is worse than an error.
        if (name.length == 0 || [name containsString:@"/"] ||
                [name isEqualToString:@"."] || [name isEqualToString:@".."]) {
            [weakSelf presentMessage:@"That name can't be used. Names can't be empty or contain “/”."
                               title:title];
            return;
        }
        completion(name);
    }]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)presentError:(nullable NSError *)error title:(NSString *)title {
    [self presentMessage:error.localizedDescription ?: @"The operation failed." title:title];
}

- (void)presentMessage:(NSString *)message title:(NSString *)title {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:title
                                                                   message:message
                                                            preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}

#pragma mark Row presentation

- (nullable UIImage *)iconForItem:(ISHGuestFileItem *)item {
    NSString *symbol;
    switch (item.kind) {
        case ISHGuestFileKindDirectory: symbol = @"folder"; break;
        case ISHGuestFileKindRegular:
            symbol = (item.posixMode & 0111) != 0 ? @"terminal" : @"doc.text";
            break;
        case ISHGuestFileKindSymlink: symbol = @"questionmark.folder"; break;
        case ISHGuestFileKindFIFO: symbol = @"arrow.left.arrow.right.square"; break;
        case ISHGuestFileKindSocket: symbol = @"network"; break;
        case ISHGuestFileKindCharDevice:
        case ISHGuestFileKindBlockDevice: symbol = @"cpu"; break;
        default: symbol = @"questionmark.square"; break;
    }
    return [UIImage systemImageNamed:symbol];
}

- (NSString *)kindDescriptionForItem:(ISHGuestFileItem *)item {
    switch (item.kind) {
        case ISHGuestFileKindDirectory: return item.isSymlink ? @"Folder alias" : @"Folder";
        case ISHGuestFileKindRegular: return (item.posixMode & 0111) != 0 ? @"Executable" : @"File";
        case ISHGuestFileKindSymlink: return @"Broken alias";
        case ISHGuestFileKindFIFO: return @"Named pipe";
        case ISHGuestFileKindSocket: return @"Socket";
        case ISHGuestFileKindCharDevice: return @"Character device";
        case ISHGuestFileKindBlockDevice: return @"Block device";
        default: return @"Other";
    }
}

- (NSString *)formattedSize:(unsigned long long)size {
    return [NSByteCountFormatter stringFromByteCount:(long long)size countStyle:NSByteCountFormatterCountStyleFile];
}

- (NSString *)modeStringForItem:(ISHGuestFileItem *)item {
    mode_t mode = item.posixMode;
    char text[10];
    static const char *bits = "rwxrwxrwx";
    for (int i = 0; i < 9; i++)
        text[i] = (mode & (1 << (8 - i))) != 0 ? bits[i] : '-';
    text[9] = '\0';
    return [NSString stringWithFormat:@"%s (%04o)", text, (unsigned)(mode & 07777)];
}

- (nullable NSString *)subtitleForItem:(ISHGuestFileItem *)item {
    if (item.isBrokenSymlink)
        return [NSString stringWithFormat:@"Broken alias → %@", item.symlinkTarget];
    if (item.isSymlink)
        return [NSString stringWithFormat:@"→ %@", item.symlinkTarget];
    if (item.kind == ISHGuestFileKindDirectory)
        return nil;
    if (item.kind != ISHGuestFileKindRegular)
        return [self kindDescriptionForItem:item];
    return [self formattedSize:item.size];
}

#pragma mark UITableViewDataSource / UITableViewDelegate

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    return (NSInteger)_items.count;
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:kShellFileBrowserCellReuseID];
    if (cell == nil)
        cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle
                                       reuseIdentifier:kShellFileBrowserCellReuseID];
    if ((NSUInteger)indexPath.row >= _items.count)
        return cell;
    ISHGuestFileItem *item = _items[(NSUInteger)indexPath.row];
    cell.textLabel.text = item.name;
    cell.textLabel.font = [UIFont monospacedSystemFontOfSize:15 weight:UIFontWeightRegular];
    cell.detailTextLabel.text = [self subtitleForItem:item];
    cell.detailTextLabel.textColor = UIColor.secondaryLabelColor;
    cell.imageView.image = [self iconForItem:item];
    cell.imageView.tintColor = item.kind == ISHGuestFileKindDirectory
        ? UIColor.systemBlueColor
        : UIColor.secondaryLabelColor;
    cell.accessoryType = item.kind == ISHGuestFileKindDirectory
        ? UITableViewCellAccessoryDisclosureIndicator
        : UITableViewCellAccessoryNone;
    return cell;
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    if ((NSUInteger)indexPath.row >= _items.count)
        return;
    ISHGuestFileItem *item = _items[(NSUInteger)indexPath.row];
    if (item.kind == ISHGuestFileKindDirectory) {
        [self navigateToPath:item.guestPath];
        return;
    }
    // Tapping a file puts it on the command line. That is the verb this whole
    // sheet exists for -- everything else is one long-press away.
    [self insertPathOfItem:item];
}

- (nullable UIContextMenuConfiguration *)tableView:(UITableView *)tableView
     contextMenuConfigurationForRowAtIndexPath:(NSIndexPath *)indexPath
                                         point:(CGPoint)point {
    if ((NSUInteger)indexPath.row >= _items.count)
        return nil;
    ISHGuestFileItem *item = _items[(NSUInteger)indexPath.row];
    __weak typeof(self) weakSelf = self;
    return [UIContextMenuConfiguration configurationWithIdentifier:nil
                                                   previewProvider:nil
                                                    actionProvider:^UIMenu *(NSArray<UIMenuElement *> *suggested) {
        NSMutableArray<UIMenuElement *> *actions = [NSMutableArray array];
        [actions addObject:[UIAction actionWithTitle:@"Insert Path"
                                               image:[UIImage systemImageNamed:@"text.cursor"]
                                          identifier:nil
                                             handler:^(UIAction *action) { [weakSelf insertPathOfItem:item]; }]];
        if (item.kind == ISHGuestFileKindDirectory) {
            [actions addObject:[UIAction actionWithTitle:@"cd Here"
                                                   image:[UIImage systemImageNamed:@"arrow.turn.down.right"]
                                              identifier:nil
                                                 handler:^(UIAction *action) {
                [weakSelf sendToTerminal:[NSString stringWithFormat:@"cd %@\n", ISHShellQuoteArgument(item.guestPath)]
                                 dismiss:YES];
            }]];
        }
        [actions addObject:[UIAction actionWithTitle:@"Copy Path"
                                               image:[UIImage systemImageNamed:@"doc.on.doc"]
                                          identifier:nil
                                             handler:^(UIAction *action) {
            UIPasteboard.generalPasteboard.string = item.guestPath;
        }]];
        [actions addObject:[UIAction actionWithTitle:@"Rename…"
                                               image:[UIImage systemImageNamed:@"pencil"]
                                          identifier:nil
                                             handler:^(UIAction *action) { [weakSelf promptRenameItem:item]; }]];
        if (item.kind == ISHGuestFileKindRegular && !item.isSymlink) {
            // Regular files only: the bridge's copy does not do directories,
            // and offering a verb that fails is worse than not offering it.
            [actions addObject:[UIAction actionWithTitle:@"Duplicate"
                                                   image:[UIImage systemImageNamed:@"plus.square.on.square"]
                                              identifier:nil
                                                 handler:^(UIAction *action) { [weakSelf duplicateItem:item]; }]];
        }
        [actions addObject:[UIAction actionWithTitle:@"Get Info"
                                               image:[UIImage systemImageNamed:@"info.circle"]
                                          identifier:nil
                                             handler:^(UIAction *action) { [weakSelf showInfoForItem:item]; }]];
        UIAction *delete = [UIAction actionWithTitle:@"Delete"
                                               image:[UIImage systemImageNamed:@"trash"]
                                          identifier:nil
                                             handler:^(UIAction *action) { [weakSelf confirmDeleteItem:item]; }];
        delete.attributes = UIMenuElementAttributesDestructive;
        [actions addObject:delete];
        return [UIMenu menuWithTitle:item.name children:actions];
    }];
}

@end

NS_ASSUME_NONNULL_END
