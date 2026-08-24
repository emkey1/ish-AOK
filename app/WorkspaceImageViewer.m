//
//  WorkspaceImageViewer.m
//  iSH-AOK
//

#import "WorkspaceImageViewer.h"
#import "GuestFileBridge.h"
#import <ImageIO/ImageIO.h>

static const CGFloat kImageViewerToolbarHeight = 44.0;
static const NSUInteger kImageViewerMaxBytes = 64 * 1024 * 1024;  // 64 MiB
static const CGFloat kImageViewerMinPixelBudget = 512.0;
static const CGFloat kImageViewerMaxPixelBudget = 4096.0;

static NSSet<NSString *> *ISHImageViewerSupportedExtensions(void) {
    static NSSet<NSString *> *set;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        set = [NSSet setWithArray:@[@"png", @"jpg", @"jpeg", @"gif", @"bmp", @"tiff", @"tif", @"heic", @"heif", @"webp", @"ico"]];
    });
    return set;
}

@interface WorkspaceImageViewerToolViewController () <UIScrollViewDelegate>
@end

@implementation WorkspaceImageViewerToolViewController {
    UIView *_toolbarView;
    UIButton *_prevButton;
    UIButton *_nextButton;
    UILabel *_titleLabel;
    UIButton *_zoomToggleButton;
    UIButton *_shareButton;
    UIButton *_reloadButton;

    UIScrollView *_scrollView;
    UIImageView *_imageView;
    NSLayoutConstraint *_imageFitWidthConstraint;
    NSLayoutConstraint *_imageFitHeightConstraint;
    NSLayoutConstraint *_imageActualWidthConstraint;
    NSLayoutConstraint *_imageActualHeightConstraint;
    UILabel *_statusLabel;

    NSString *_currentPath;
    NSArray<NSString *> *_siblingPaths;  // sorted image paths in the current directory, for prev/next
    CGSize _originalPixelSize;            // true pixel dimensions, for the title -- may exceed the decoded bitmap
    BOOL _showingActualSize;
    NSString *_statusMessage;
    NSInteger _loadGeneration;            // discards stale async loads
    // ...and these stop them. Flipping through a folder of 60 MiB photos with
    // only the generation guard left every skipped image still being read out of
    // the guest, and the sibling scan still walking the directory.
    ISHGuestFileOperationToken _readToken;
    ISHGuestFileOperationToken _siblingsToken;
    CGSize _lastLayoutSize;               // detects real window resizes in viewDidLayoutSubviews
}

#pragma mark Lifecycle

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Image";

    [self buildToolbar];
    [self buildImageArea];
    [self buildStatusLabel];
    [self activateRegionConstraints];

    _statusMessage = @"Open an image from the File Manager.";
    [self updateStatusLabel];
}

// Workspace windows resize continuously; the fit-mode constraints tie the
// image size to the scroll view's frame, and re-solving them under a live
// zoomScale > 1 makes the zoomed view jump unpredictably. Reset to the
// un-zoomed fit on a real size change (zooming itself doesn't change our
// bounds, so this never fires mid-pinch).
- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    CGSize size = self.view.bounds.size;
    if (CGSizeEqualToSize(size, _lastLayoutSize))
        return;
    _lastLayoutSize = size;
    if (_scrollView.zoomScale != _scrollView.minimumZoomScale)
        [_scrollView setZoomScale:_scrollView.minimumZoomScale animated:NO];
    [self centerImage];
}

// Key commands are collected along the first-responder chain, and nothing in
// this applet takes text input -- participate directly so the arrows work.
- (BOOL)canBecomeFirstResponder {
    return YES;
}

- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    [self becomeFirstResponder];
}

- (NSArray<UIKeyCommand *> *)keyCommands {
    UIKeyCommand *prev = [UIKeyCommand keyCommandWithInput:UIKeyInputLeftArrow modifierFlags:0 action:@selector(navigatePrev)];
    prev.discoverabilityTitle = @"Previous Image";
    UIKeyCommand *next = [UIKeyCommand keyCommandWithInput:UIKeyInputRightArrow modifierFlags:0 action:@selector(navigateNext)];
    next.discoverabilityTitle = @"Next Image";
    // Without this the iOS 15+ focus engine consumes unmodified arrows before
    // they reach key commands, so prev/next would silently never fire.
    prev.wantsPriorityOverSystemBehavior = YES;
    next.wantsPriorityOverSystemBehavior = YES;
    return @[prev, next];
}

#pragma mark View construction

- (void)buildToolbar {
    _toolbarView = [UIView new];
    _toolbarView.translatesAutoresizingMaskIntoConstraints = NO;
    [self.toolContentView addSubview:_toolbarView];

    _prevButton = [self toolbarIconButtonNamed:@"chevron.left" action:@selector(navigatePrev)];
    _prevButton.accessibilityLabel = @"Previous Image";
    _nextButton = [self toolbarIconButtonNamed:@"chevron.right" action:@selector(navigateNext)];
    _nextButton.accessibilityLabel = @"Next Image";
    _prevButton.enabled = NO;
    _nextButton.enabled = NO;

    _titleLabel = [UILabel new];
    _titleLabel.translatesAutoresizingMaskIntoConstraints = NO;
    _titleLabel.font = [UIFont systemFontOfSize:13.0 weight:UIFontWeightMedium];
    _titleLabel.lineBreakMode = NSLineBreakByTruncatingTail;
    _titleLabel.textAlignment = NSTextAlignmentCenter;
    _titleLabel.text = @"Image";
    [_titleLabel setContentCompressionResistancePriority:UILayoutPriorityDefaultLow forAxis:UILayoutConstraintAxisHorizontal];

    _zoomToggleButton = [UIButton buttonWithType:UIButtonTypeSystem];
    _zoomToggleButton.translatesAutoresizingMaskIntoConstraints = NO;
    _zoomToggleButton.titleLabel.font = [UIFont systemFontOfSize:12.0 weight:UIFontWeightSemibold];
    [_zoomToggleButton addTarget:self action:@selector(toggleZoomMode) forControlEvents:UIControlEventTouchUpInside];
    _zoomToggleButton.enabled = NO;
    [self updateZoomToggleTitle];

    _shareButton = [self toolbarIconButtonNamed:@"square.and.arrow.up" action:@selector(shareCurrentImage)];
    _shareButton.accessibilityLabel = @"Share";
    _shareButton.enabled = NO;
    _reloadButton = [self toolbarIconButtonNamed:@"arrow.clockwise" action:@selector(reload)];
    _reloadButton.accessibilityLabel = @"Reload";
    _reloadButton.enabled = NO;

    UIStackView *stack = [[UIStackView alloc] initWithArrangedSubviews:@[
        _prevButton, _nextButton, _titleLabel, _zoomToggleButton, _shareButton, _reloadButton,
    ]];
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    stack.axis = UILayoutConstraintAxisHorizontal;
    stack.alignment = UIStackViewAlignmentCenter;
    stack.spacing = 8.0;
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

- (void)buildImageArea {
    _scrollView = [UIScrollView new];
    _scrollView.translatesAutoresizingMaskIntoConstraints = NO;
    _scrollView.delegate = self;
    _scrollView.minimumZoomScale = 1.0;
    _scrollView.maximumZoomScale = 4.0;
    _scrollView.backgroundColor = UIColor.clearColor;
    [self.toolContentView addSubview:_scrollView];

    UITapGestureRecognizer *doubleTap = [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(handleDoubleTap:)];
    doubleTap.numberOfTapsRequired = 2;
    [_scrollView addGestureRecognizer:doubleTap];

    _imageView = [UIImageView new];
    _imageView.translatesAutoresizingMaskIntoConstraints = NO;
    _imageView.contentMode = UIViewContentModeScaleAspectFit;
    _imageView.userInteractionEnabled = YES;
    [_scrollView addSubview:_imageView];

    // "Fit" pins the image view to the scroll view's own frame (the image
    // scales down to fit inside via aspect-fit content mode). "Actual size"
    // instead gives it fixed constants equal to the decoded bitmap's point
    // size. Only one pair is ever active; -applyZoomMode: toggles between them
    // (a relative-vs-absolute constraint pair can't be expressed as one
    // constraint whose multiplier changes at runtime).
    _imageFitWidthConstraint = [_imageView.widthAnchor constraintEqualToAnchor:_scrollView.frameLayoutGuide.widthAnchor];
    _imageFitHeightConstraint = [_imageView.heightAnchor constraintEqualToAnchor:_scrollView.frameLayoutGuide.heightAnchor];
    _imageActualWidthConstraint = [_imageView.widthAnchor constraintEqualToConstant:0];
    _imageActualHeightConstraint = [_imageView.heightAnchor constraintEqualToConstant:0];

    [NSLayoutConstraint activateConstraints:@[
        [_imageView.topAnchor constraintEqualToAnchor:_scrollView.contentLayoutGuide.topAnchor],
        [_imageView.leadingAnchor constraintEqualToAnchor:_scrollView.contentLayoutGuide.leadingAnchor],
        [_imageView.trailingAnchor constraintEqualToAnchor:_scrollView.contentLayoutGuide.trailingAnchor],
        [_imageView.bottomAnchor constraintEqualToAnchor:_scrollView.contentLayoutGuide.bottomAnchor],
    ]];
    _imageFitWidthConstraint.active = YES;
    _imageFitHeightConstraint.active = YES;
}

- (void)buildStatusLabel {
    _statusLabel = [UILabel new];
    _statusLabel.translatesAutoresizingMaskIntoConstraints = NO;
    _statusLabel.numberOfLines = 0;
    _statusLabel.textAlignment = NSTextAlignmentCenter;
    _statusLabel.font = [UIFont systemFontOfSize:15.0];
    [self.toolContentView addSubview:_statusLabel];
}

- (void)activateRegionConstraints {
    UIView *content = self.toolContentView;
    [NSLayoutConstraint activateConstraints:@[
        [_toolbarView.topAnchor constraintEqualToAnchor:content.topAnchor],
        [_toolbarView.leadingAnchor constraintEqualToAnchor:content.leadingAnchor],
        [_toolbarView.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [_toolbarView.heightAnchor constraintEqualToConstant:kImageViewerToolbarHeight],

        [_scrollView.topAnchor constraintEqualToAnchor:_toolbarView.bottomAnchor],
        [_scrollView.leadingAnchor constraintEqualToAnchor:content.leadingAnchor],
        [_scrollView.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [_scrollView.bottomAnchor constraintEqualToAnchor:content.bottomAnchor],

        [_statusLabel.centerXAnchor constraintEqualToAnchor:_scrollView.centerXAnchor],
        [_statusLabel.centerYAnchor constraintEqualToAnchor:_scrollView.centerYAnchor],
        [_statusLabel.leadingAnchor constraintGreaterThanOrEqualToAnchor:content.leadingAnchor constant:20],
        [_statusLabel.trailingAnchor constraintLessThanOrEqualToAnchor:content.trailingAnchor constant:-20],
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
    // Also gives letterboxed (non-fitting) images a neutral backdrop instead
    // of the gradient showing through.
    self.toolContentView.backgroundColor = [theme[@"card"] colorWithAlphaComponent:0.95];
    _titleLabel.textColor = theme[@"primary"];
    _statusLabel.textColor = theme[@"secondary"];
    for (UIButton *button in @[_prevButton, _nextButton, _shareButton, _reloadButton])
        button.tintColor = theme[@"accent"];
    [_zoomToggleButton setTitleColor:theme[@"accent"] forState:UIControlStateNormal];
}

#pragma mark WorkspaceFileOpenable

- (void)workspaceOpenFileAtGuestPath:(NSString *)guestPath {
    [self loadPath:guestPath];
}

#pragma mark WorkspaceStatefulTool

- (nullable NSDictionary<NSString *, id> *)workspaceToolStateForSaving {
    return _currentPath.length > 0 ? @{@"path": _currentPath} : nil;
}

- (void)workspaceRestoreToolState:(NSDictionary<NSString *, id> *)state {
    NSString *path = [state[@"path"] isKindOfClass:NSString.class] ? state[@"path"] : nil;
    if (path.length > 0)
        [self loadPath:path];  // a since-deleted file shows the load error, honestly
}

#pragma mark Navigation

- (void)reload {
    if (_currentPath != nil) [self loadPath:_currentPath];
}

- (void)navigatePrev {
    NSUInteger index = [_siblingPaths indexOfObject:_currentPath];
    if (index == NSNotFound || index == 0) return;
    [self loadPath:_siblingPaths[index - 1]];
}

- (void)navigateNext {
    NSUInteger index = [_siblingPaths indexOfObject:_currentPath];
    if (index == NSNotFound || index + 1 >= _siblingPaths.count) return;
    [self loadPath:_siblingPaths[index + 1]];
}

- (void)updatePrevNextButtons {
    NSUInteger index = [_siblingPaths indexOfObject:_currentPath];
    _prevButton.enabled = (index != NSNotFound && index > 0);
    _nextButton.enabled = (index != NSNotFound && index + 1 < _siblingPaths.count);
}

- (void)loadSiblingsForCurrentDirectory {
    NSString *directory = _currentPath.stringByDeletingLastPathComponent;
    if (directory.length == 0) directory = @"/";
    // Guarded by the generation like the image read is. Without it a scan
    // cancelled by the NEXT loadPath: would land afterward and clear the token
    // that load had just stored, leaving its own scan uncancellable -- and it
    // would also overwrite _siblingPaths with the directory we had left.
    NSInteger generation = _loadGeneration;
    __weak typeof(self) weakSelf = self;
    _siblingsToken = [ISHGuestFileBridge.sharedBridge listDirectoryAtGuestPath:directory completion:^(NSArray<ISHGuestFileItem *> *items, NSError *error) {
        typeof(self) strongSelf = weakSelf;
        if (strongSelf == nil || strongSelf->_loadGeneration != generation) return;
        strongSelf->_siblingsToken = nil;
        if (items == nil) return;
        NSMutableArray<NSString *> *imagePaths = [NSMutableArray array];
        for (ISHGuestFileItem *item in items) {
            if (item.kind != ISHGuestFileKindRegular) continue;
            if ([ISHImageViewerSupportedExtensions() containsObject:item.name.pathExtension.lowercaseString])
                [imagePaths addObject:item.guestPath];
        }
        [imagePaths sortUsingSelector:@selector(caseInsensitiveCompare:)];
        strongSelf->_siblingPaths = imagePaths;
        [strongSelf updatePrevNextButtons];
    }];
}

#pragma mark Loading

// Both the sibling scan and the image read belong to one displayed image, so
// they are cancelled together whenever a new one starts or the viewer goes away.
// Their completions are guarded by _loadGeneration and a nil check, so a
// cancelled one lands harmlessly.
- (void)cancelPendingBridgeWork {
    if (_siblingsToken != nil) {
        [ISHGuestFileBridge.sharedBridge cancelOperation:_siblingsToken];
        _siblingsToken = nil;
    }
    if (_readToken != nil) {
        [ISHGuestFileBridge.sharedBridge cancelOperation:_readToken];
        _readToken = nil;
    }
}

- (void)dealloc {
    [self cancelPendingBridgeWork];
}

- (void)loadPath:(NSString *)path {
    [self cancelPendingBridgeWork];  // whatever the last image was doing, stop it
    _currentPath = path;
    _titleLabel.text = path.lastPathComponent;
    _reloadButton.enabled = YES;
    _shareButton.enabled = NO;
    _zoomToggleButton.enabled = NO;
    _imageView.image = nil;
    _originalPixelSize = CGSizeZero;
    _statusMessage = @"Loading…";
    [self updateStatusLabel];

    NSInteger generation = ++_loadGeneration;
    [self loadSiblingsForCurrentDirectory];  // after the bump: it reads the new generation

    __weak typeof(self) weakSelf = self;
    _readToken = [ISHGuestFileBridge.sharedBridge readFileAtGuestPath:path maxBytes:kImageViewerMaxBytes
                                                completion:^(NSData *data, NSError *error) {
        typeof(self) strongSelf = weakSelf;
        if (strongSelf == nil || strongSelf->_loadGeneration != generation)
            return;  // a newer navigation superseded this read
        strongSelf->_readToken = nil;
        if (data == nil) {
            strongSelf->_statusMessage = [strongSelf messageForReadError:error];
            [strongSelf updateStatusLabel];
            return;
        }
        // Capture the pixel budget here on main (view bounds and screen
        // scale are UIKit main-thread state), then decode off-main -- ImageIO
        // decode is a blocking CPU operation.
        size_t maxPixelSize = [strongSelf currentDecodePixelBudget];
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            [strongSelf decodeAndDisplayData:data maxPixelSize:maxPixelSize generation:generation];
        });
    }];
}

// Main thread only. ~2x the window's screen-pixel size, clamped to a sane
// range, so a huge photo can never balloon into a full-resolution UIImage.
- (size_t)currentDecodePixelBudget {
    CGFloat screenScale = UIScreen.mainScreen.scale;
    CGFloat maxViewDimension = MAX(self.view.bounds.size.width, self.view.bounds.size.height);
    if (maxViewDimension <= 0) maxViewDimension = 512.0;  // view not laid out yet; fall back to a sane budget
    return (size_t)MIN(MAX(maxViewDimension * screenScale * 2.0, kImageViewerMinPixelBudget), kImageViewerMaxPixelBudget);
}

- (NSString *)messageForReadError:(NSError *)error {
    if ([error.domain isEqualToString:ISHGuestFileErrorDomain] && error.code == ISHGuestFileBridgeErrorTooLarge)
        return @"This file is larger than 64 MB and can’t be displayed here.";
    return error.localizedDescription.length ? error.localizedDescription : @"Couldn’t open this file.";
}

// Runs on a background queue. Decodes at the caller-captured pixel budget so
// a huge photo can never balloon into a full-resolution UIImage and trigger
// jetsam -- this is a correctness requirement, not an optimization. "Actual
// size" below therefore shows the decoded bitmap's own size, which for
// anything under the budget (the common case) is the image's true size; the
// title bar's pixel-dimension readout still reflects the file's real
// dimensions, read separately and cheaply from the image source's
// properties without a full decode.
- (void)decodeAndDisplayData:(NSData *)data maxPixelSize:(size_t)maxPixelSize generation:(NSInteger)generation {
    CGImageSourceRef source = CGImageSourceCreateWithData((__bridge CFDataRef)data, NULL);
    if (source == NULL) {
        [self finishDecodeOnMainQueueWithImage:nil pixelSize:CGSizeZero generation:generation
                                   errorMessage:@"This doesn’t look like an image iSH-AOK can display."];
        return;
    }

    NSDictionary *properties = (__bridge_transfer NSDictionary *)CGImageSourceCopyPropertiesAtIndex(source, 0, NULL);
    CGFloat pixelWidth = [properties[(__bridge NSString *)kCGImagePropertyPixelWidth] doubleValue];
    CGFloat pixelHeight = [properties[(__bridge NSString *)kCGImagePropertyPixelHeight] doubleValue];

    NSDictionary *thumbnailOptions = @{
        (__bridge NSString *)kCGImageSourceCreateThumbnailFromImageAlways: @YES,
        (__bridge NSString *)kCGImageSourceThumbnailMaxPixelSize: @(maxPixelSize),
        (__bridge NSString *)kCGImageSourceCreateThumbnailWithTransform: @YES,  // respect EXIF orientation
        (__bridge NSString *)kCGImageSourceShouldCacheImmediately: @YES,
    };
    CGImageRef thumbnail = CGImageSourceCreateThumbnailAtIndex(source, 0, (__bridge CFDictionaryRef)thumbnailOptions);
    CFRelease(source);
    if (thumbnail == NULL) {
        [self finishDecodeOnMainQueueWithImage:nil pixelSize:CGSizeZero generation:generation
                                   errorMessage:@"Couldn’t decode this image."];
        return;
    }
    UIImage *image = [UIImage imageWithCGImage:thumbnail];
    CGImageRelease(thumbnail);
    [self finishDecodeOnMainQueueWithImage:image pixelSize:CGSizeMake(pixelWidth, pixelHeight)
                                 generation:generation errorMessage:nil];
}

- (void)finishDecodeOnMainQueueWithImage:(nullable UIImage *)image pixelSize:(CGSize)pixelSize
                               generation:(NSInteger)generation errorMessage:(nullable NSString *)errorMessage {
    __weak typeof(self) weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        typeof(self) strongSelf = weakSelf;
        if (strongSelf == nil || strongSelf->_loadGeneration != generation)
            return;  // a newer navigation superseded this decode
        if (image == nil) {
            strongSelf->_statusMessage = errorMessage;
            [strongSelf updateStatusLabel];
            return;
        }
        strongSelf->_statusMessage = nil;
        strongSelf->_originalPixelSize = pixelSize;
        [strongSelf displayImage:image];
    });
}

- (void)displayImage:(UIImage *)image {
    _imageView.image = image;
    _imageActualWidthConstraint.constant = image.size.width;
    _imageActualHeightConstraint.constant = image.size.height;
    _showingActualSize = NO;
    [self applyZoomMode];
    _shareButton.enabled = YES;
    _zoomToggleButton.enabled = YES;
    [self updateStatusLabel];
    [self updateTitleLabel];
}

- (void)updateTitleLabel {
    NSString *name = _currentPath.lastPathComponent ?: @"";
    if (_originalPixelSize.width > 0 && _originalPixelSize.height > 0)
        _titleLabel.text = [NSString stringWithFormat:@"%@ — %.0f × %.0f", name, _originalPixelSize.width, _originalPixelSize.height];
    else
        _titleLabel.text = name;
}

- (void)updateStatusLabel {
    _statusLabel.hidden = (_statusMessage == nil);
    _statusLabel.text = _statusMessage;
}

#pragma mark Zoom

- (void)toggleZoomMode {
    _showingActualSize = !_showingActualSize;
    [self applyZoomMode];
    [self updateZoomToggleTitle];
}

- (void)updateZoomToggleTitle {
    [_zoomToggleButton setTitle:(_showingActualSize ? @"Fit" : @"100%") forState:UIControlStateNormal];
    _zoomToggleButton.accessibilityLabel = _showingActualSize ? @"Fit to screen" : @"View actual size";
}

- (void)applyZoomMode {
    BOOL actual = _showingActualSize;
    _imageFitWidthConstraint.active = !actual;
    _imageFitHeightConstraint.active = !actual;
    _imageActualWidthConstraint.active = actual;
    _imageActualHeightConstraint.active = actual;
    [_scrollView setZoomScale:1.0 animated:NO];
    [self.toolContentView layoutIfNeeded];
    [self centerImage];
}

- (void)handleDoubleTap:(UITapGestureRecognizer *)gesture {
    if (_scrollView.zoomScale > _scrollView.minimumZoomScale) {
        [_scrollView setZoomScale:_scrollView.minimumZoomScale animated:YES];
    } else {
        CGPoint point = [gesture locationInView:_imageView];
        CGFloat targetScale = _scrollView.maximumZoomScale;
        CGSize size = CGSizeMake(_scrollView.bounds.size.width / targetScale, _scrollView.bounds.size.height / targetScale);
        CGRect zoomRect = CGRectMake(point.x - size.width / 2, point.y - size.height / 2, size.width, size.height);
        [_scrollView zoomToRect:zoomRect animated:YES];
    }
}

- (nullable UIView *)viewForZoomingInScrollView:(UIScrollView *)scrollView {
    return _imageView;
}

- (void)scrollViewDidZoom:(UIScrollView *)scrollView {
    [self centerImage];
}

// UIScrollView doesn't center content that's smaller than its bounds on its
// own; nudge it via contentInset, the standard technique for this.
- (void)centerImage {
    CGSize boundsSize = _scrollView.bounds.size;
    CGRect frame = _imageView.frame;
    CGFloat insetX = MAX((boundsSize.width - frame.size.width) / 2.0, 0);
    CGFloat insetY = MAX((boundsSize.height - frame.size.height) / 2.0, 0);
    _scrollView.contentInset = UIEdgeInsetsMake(insetY, insetX, insetY, insetX);
}

#pragma mark Sharing

- (void)shareCurrentImage {
    if (_currentPath == nil) return;
    __weak typeof(self) weakSelf = self;
    [ISHGuestFileBridge.sharedBridge extractToTempFileAtGuestPath:_currentPath progress:nil
        completion:^(NSURL *fileURL, NSError *error) {
        typeof(self) strongSelf = weakSelf;
        if (strongSelf == nil) return;
        if (fileURL == nil) {
            [strongSelf presentSimpleAlertWithTitle:@"Can’t Share" message:error.localizedDescription];
            return;
        }
        UIActivityViewController *activity = [[UIActivityViewController alloc] initWithActivityItems:@[fileURL] applicationActivities:nil];
        UIPopoverPresentationController *popover = activity.popoverPresentationController;
        if (popover != nil) {
            popover.sourceView = strongSelf->_shareButton;
            popover.sourceRect = strongSelf->_shareButton.bounds;
        }
        [strongSelf presentViewController:activity animated:YES completion:nil];
    }];
}

- (void)presentSimpleAlertWithTitle:(NSString *)title message:(nullable NSString *)message {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:title message:message
                                                              preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}

@end
