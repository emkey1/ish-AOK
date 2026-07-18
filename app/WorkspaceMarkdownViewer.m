//
//  WorkspaceMarkdownViewer.m
//  iSH-AOK
//

#import "WorkspaceMarkdownViewer.h"
#import "GuestFileBridge.h"
#import "MarkdownRenderer.h"

static const CGFloat kMarkdownViewerToolbarHeight = 44.0;
static const NSUInteger kMarkdownViewerMaxBytes = 4 * 1024 * 1024;  // 4 MiB; real markdown that size is a pathology

@interface WorkspaceMarkdownViewerToolViewController () <UITextViewDelegate>
@end

@implementation WorkspaceMarkdownViewerToolViewController {
    UIView *_toolbarView;
    UIButton *_backButton;
    UILabel *_titleLabel;
    UIButton *_editButton;
    UIButton *_reloadButton;
    UITextView *_textView;

    NSString *_currentPath;
    NSMutableArray<NSString *> *_backHistory;
    NSString *_rawMarkdown;    // nil while a status message (loading/error/empty) is showing instead
    NSString *_statusMessage;
    NSInteger _loadGeneration; // discards stale async reads
}

#pragma mark Lifecycle

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Markdown";
    _backHistory = [NSMutableArray array];

    [self buildToolbar];
    [self buildTextView];
    [self activateRegionConstraints];

    _statusMessage = @"Open a Markdown file from the File Manager.";
    [self updateContent];
}

#pragma mark View construction

- (void)buildToolbar {
    _toolbarView = [UIView new];
    _toolbarView.translatesAutoresizingMaskIntoConstraints = NO;
    [self.toolContentView addSubview:_toolbarView];

    _backButton = [self toolbarIconButtonNamed:@"chevron.left" action:@selector(navigateBack)];
    _backButton.accessibilityLabel = @"Back";
    _backButton.enabled = NO;

    _titleLabel = [UILabel new];
    _titleLabel.translatesAutoresizingMaskIntoConstraints = NO;
    _titleLabel.font = [UIFont systemFontOfSize:13.0 weight:UIFontWeightMedium];
    _titleLabel.lineBreakMode = NSLineBreakByTruncatingTail;
    _titleLabel.textAlignment = NSTextAlignmentCenter;
    _titleLabel.text = @"Markdown";
    [_titleLabel setContentCompressionResistancePriority:UILayoutPriorityDefaultLow forAxis:UILayoutConstraintAxisHorizontal];

    _editButton = [self toolbarIconButtonNamed:@"pencil" action:@selector(openInMotePad)];
    _editButton.accessibilityLabel = @"Edit";
    _editButton.enabled = NO;
    _reloadButton = [self toolbarIconButtonNamed:@"arrow.clockwise" action:@selector(reload)];
    _reloadButton.accessibilityLabel = @"Reload";
    _reloadButton.enabled = NO;

    UIStackView *stack = [[UIStackView alloc] initWithArrangedSubviews:@[_backButton, _titleLabel, _editButton, _reloadButton]];
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

- (UIButton *)toolbarIconButtonNamed:(NSString *)symbolName action:(SEL)action {
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    button.translatesAutoresizingMaskIntoConstraints = NO;
    [button setImage:[UIImage systemImageNamed:symbolName] forState:UIControlStateNormal];
    [button addTarget:self action:action forControlEvents:UIControlEventTouchUpInside];
    [button.widthAnchor constraintGreaterThanOrEqualToConstant:28].active = YES;
    return button;
}

- (void)buildTextView {
    _textView = [UITextView new];
    _textView.translatesAutoresizingMaskIntoConstraints = NO;
    _textView.editable = NO;
    _textView.selectable = YES;
    _textView.delegate = self;
    _textView.backgroundColor = UIColor.clearColor;
    _textView.textContainerInset = UIEdgeInsetsMake(12, 12, 12, 12);
    // The renderer already turns [text](url) into NSLinkAttributeName runs; a
    // second, independent data-detector pass over the same text would double
    // up link recognition and can disagree with the renderer's own ranges.
    _textView.dataDetectorTypes = UIDataDetectorTypeNone;
    [self.toolContentView addSubview:_textView];
}

// Explicit anchors, not a stack view: same reasoning as the file manager's
// table view -- a UITextView filling "the rest of the space" needs pinned
// top/leading/trailing/bottom anchors, not stack-view fill guessing.
- (void)activateRegionConstraints {
    UIView *content = self.toolContentView;
    [NSLayoutConstraint activateConstraints:@[
        [_toolbarView.topAnchor constraintEqualToAnchor:content.topAnchor],
        [_toolbarView.leadingAnchor constraintEqualToAnchor:content.leadingAnchor],
        [_toolbarView.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [_toolbarView.heightAnchor constraintEqualToConstant:kMarkdownViewerToolbarHeight],

        [_textView.topAnchor constraintEqualToAnchor:_toolbarView.bottomAnchor],
        [_textView.leadingAnchor constraintEqualToAnchor:content.leadingAnchor],
        [_textView.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [_textView.bottomAnchor constraintEqualToAnchor:content.bottomAnchor],
    ]];
}

#pragma mark Theme

- (void)workspaceApplyTheme {
    [super workspaceApplyTheme];
    // The base class invokes this from ITS viewDidLoad, before ours has
    // built any views -- an @[] literal of nil ivars would throw.
    if (_toolbarView == nil)
        return;
    NSDictionary<NSString *, UIColor *> *theme = self.workspaceTheme;
    // primary/secondary/accent are only guaranteed to contrast against the
    // theme's card surfaces (programmatically contrast-adjusted against
    // them), never against the raw gradient backdrop -- give the content
    // area an opaque card backing rather than rendering text straight onto
    // the transparent gradient, matching every other applet in this file.
    self.toolContentView.backgroundColor = [theme[@"card"] colorWithAlphaComponent:0.95];
    _titleLabel.textColor = theme[@"primary"];
    for (UIButton *button in @[_backButton, _editButton, _reloadButton])
        button.tintColor = theme[@"accent"];
    [self updateContent];  // re-render markdown with the new theme's colors baked in
}

#pragma mark WorkspaceFileOpenable

- (void)workspaceOpenFileAtGuestPath:(NSString *)guestPath {
    [_backHistory removeAllObjects];
    [self loadPath:guestPath];
}

#pragma mark WorkspaceStatefulTool

- (nullable NSDictionary<NSString *, id> *)workspaceToolStateForSaving {
    return _currentPath.length > 0 ? @{@"path": _currentPath} : nil;
}

- (void)workspaceRestoreToolState:(NSDictionary<NSString *, id> *)state {
    NSString *path = [state[@"path"] isKindOfClass:NSString.class] ? state[@"path"] : nil;
    if (path.length > 0)
        [self workspaceOpenFileAtGuestPath:path];  // a since-deleted file shows the load error, honestly
}

#pragma mark Navigation

- (void)navigateBack {
    if (_backHistory.count == 0) return;
    NSString *target = _backHistory.lastObject;
    [_backHistory removeLastObject];
    [self loadPath:target];
}

// Resolves a relative markdown link against the current file's directory and
// navigates the reader to it in place; the back button retraces the trail.
- (void)navigateToRelativePath:(NSString *)relativePath {
    if (_currentPath == nil) return;
    NSString *directory = _currentPath.stringByDeletingLastPathComponent;
    if (directory.length == 0) directory = @"/";
    NSString *resolved = [relativePath hasPrefix:@"/"] ? relativePath
                                                        : [directory stringByAppendingPathComponent:relativePath];
    resolved = resolved.stringByStandardizingPath;
    [_backHistory addObject:_currentPath];
    [self loadPath:resolved];
}

- (void)reload {
    if (_currentPath != nil) [self loadPath:_currentPath];
}

- (void)openInMotePad {
    if (_currentPath == nil) return;
    [self.workspaceHostViewController openWorkspaceToolWithIdentifier:@"motepad" fileGuestPath:_currentPath];
}

#pragma mark Loading

- (void)loadPath:(NSString *)path {
    _currentPath = path;
    _titleLabel.text = path.lastPathComponent;
    _backButton.enabled = _backHistory.count > 0;
    _editButton.enabled = YES;
    _reloadButton.enabled = YES;
    _rawMarkdown = nil;
    _statusMessage = @"Loading…";
    [self updateContent];

    NSInteger generation = ++_loadGeneration;
    __weak typeof(self) weakSelf = self;
    [ISHGuestFileBridge.sharedBridge readFileAtGuestPath:path maxBytes:kMarkdownViewerMaxBytes
                                                completion:^(NSData *data, NSError *error) {
        typeof(self) strongSelf = weakSelf;
        if (strongSelf == nil || strongSelf->_loadGeneration != generation)
            return;  // a newer navigation superseded this read
        if (data == nil) {
            strongSelf->_rawMarkdown = nil;
            strongSelf->_statusMessage = [strongSelf messageForReadError:error];
            [strongSelf updateContent];
            return;
        }
        NSString *text = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding]
                       ?: [[NSString alloc] initWithData:data encoding:NSISOLatin1StringEncoding]
                       ?: @"";
        strongSelf->_rawMarkdown = text;
        strongSelf->_statusMessage = nil;
        [strongSelf updateContent];
    }];
}

- (NSString *)messageForReadError:(NSError *)error {
    if ([error.domain isEqualToString:ISHGuestFileErrorDomain] && error.code == ISHGuestFileBridgeErrorTooLarge)
        return @"This file is larger than 4 MB and can’t be displayed here.";
    return error.localizedDescription.length ? error.localizedDescription : @"Couldn’t open this file.";
}

- (void)updateContent {
    if (_statusMessage != nil) {
        NSDictionary<NSString *, UIColor *> *theme = self.workspaceTheme;
        _textView.attributedText = [[NSAttributedString alloc] initWithString:_statusMessage attributes:@{
            NSFontAttributeName: [UIFont systemFontOfSize:15.0],
            NSForegroundColorAttributeName: theme[@"secondary"] ?: UIColor.secondaryLabelColor,
        }];
        return;
    }
    _textView.attributedText = [self renderedMarkdown];
}

- (NSAttributedString *)renderedMarkdown {
    NSDictionary<NSString *, UIColor *> *theme = self.workspaceTheme;
    UIFont *baseFont = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    UIColor *textColor = theme[@"primary"] ?: UIColor.labelColor;
    UIColor *secondaryColor = theme[@"secondary"] ?: UIColor.secondaryLabelColor;
    UIColor *codeBg = [theme[@"cardAlt"] colorWithAlphaComponent:0.5] ?: UIColor.tertiarySystemFillColor;
    UIColor *linkColor = theme[@"accent"] ?: UIColor.linkColor;
    return ISHMarkdownAttributedStringFromMarkdown(_rawMarkdown ?: @"", baseFont, textColor, secondaryColor, codeBg, linkColor);
}

#pragma mark UITextViewDelegate

- (BOOL)textView:(UITextView *)textView shouldInteractWithURL:(NSURL *)URL
          inRange:(NSRange)characterRange interaction:(UITextItemInteraction)interaction {
    NSString *scheme = URL.scheme.lowercaseString;
    if ([scheme isEqualToString:@"http"] || [scheme isEqualToString:@"https"]) {
        // Opens in the system browser rather than the in-app Browser applet:
        // there's no existing "open this URL in Browser" entry point, and
        // adding cross-applet API surface just for this felt like scope
        // creep for a reader whose primary job is showing local documents.
        [UIApplication.sharedApplication openURL:URL options:@{} completionHandler:nil];
        return NO;
    }
    // No scheme: a path relative to the current file. Strip any #fragment
    // (an anchor within the target document, meaningless to our loader) and
    // percent-decode -- iOS 17+ leniently percent-encodes spaces at parse
    // time, and authors may pre-encode, but the guest path wants raw bytes.
    // Only follow it in-reader if it looks like another markdown document;
    // anything else (an image reference, say -- inline image rendering isn't
    // implemented yet) gets a clear "can't open this" instead of silently
    // doing nothing.
    NSString *relativePath = URL.relativeString;
    NSRange fragmentMarker = [relativePath rangeOfString:@"#"];
    if (fragmentMarker.location != NSNotFound)
        relativePath = [relativePath substringToIndex:fragmentMarker.location];
    relativePath = relativePath.stringByRemovingPercentEncoding ?: relativePath;
    if (relativePath.length == 0)
        return NO;  // pure-anchor link like "#section": nowhere to navigate
    NSString *ext = relativePath.pathExtension.lowercaseString;
    if (ext.length == 0 || [ext isEqualToString:@"md"] || [ext isEqualToString:@"markdown"]) {
        [self navigateToRelativePath:relativePath];
    } else {
        [self presentSimpleAlertWithTitle:@"Can’t Open"
                                   message:[NSString stringWithFormat:@"“%@” can’t be opened from here yet.", relativePath]];
    }
    return NO;
}

- (void)presentSimpleAlertWithTitle:(NSString *)title message:(NSString *)message {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:title message:message
                                                              preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}

@end
