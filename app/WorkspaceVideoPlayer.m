//
//  WorkspaceVideoPlayer.m
//  iSH-AOK
//

#import "WorkspaceVideoPlayer.h"
#import "GuestFileBridge.h"
#import <AVKit/AVKit.h>
#import <AVFoundation/AVFoundation.h>

static void *kWorkspaceVideoPlayerItemStatusContext = &kWorkspaceVideoPlayerItemStatusContext;

@implementation WorkspaceVideoPlayerToolViewController {
    AVPlayerViewController *_playerViewController;
    UILabel *_statusLabel;

    UIView *_progressCard;
    UILabel *_progressLabel;
    UIProgressView *_progressView;
    UIButton *_cancelButton;

    NSString *_currentPath;
    ISHGuestFileExtractionToken _activeExtractionToken;
    AVPlayerItem *_observedItem;  // the one item we're KVO-observing "status" on, if any
    NSInteger _loadGeneration;    // discards stale async loads
}

#pragma mark Lifecycle

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Video";

    [self buildPlayer];
    [self buildStatusLabel];
    [self buildProgressCard];

    _statusLabel.text = @"Open a video from the File Manager.";
}

- (void)dealloc {
    // Stop a still-running extraction; without this, closing the window
    // mid-copy of a large video keeps the ioQueue copying to completion.
    if (_activeExtractionToken != nil)
        [ISHGuestFileBridge.sharedBridge cancelExtraction:_activeExtractionToken];
    [self removeItemObserverIfNeeded];
}

#pragma mark View construction

- (void)buildPlayer {
    _playerViewController = [AVPlayerViewController new];
    [self addChildViewController:_playerViewController];
    _playerViewController.view.translatesAutoresizingMaskIntoConstraints = NO;
    _playerViewController.view.hidden = YES;  // shown once a player item is actually ready
    [self.toolContentView addSubview:_playerViewController.view];
    [NSLayoutConstraint activateConstraints:@[
        [_playerViewController.view.topAnchor constraintEqualToAnchor:self.toolContentView.topAnchor],
        [_playerViewController.view.leadingAnchor constraintEqualToAnchor:self.toolContentView.leadingAnchor],
        [_playerViewController.view.trailingAnchor constraintEqualToAnchor:self.toolContentView.trailingAnchor],
        [_playerViewController.view.bottomAnchor constraintEqualToAnchor:self.toolContentView.bottomAnchor],
    ]];
    [_playerViewController didMoveToParentViewController:self];
}

- (void)buildStatusLabel {
    _statusLabel = [UILabel new];
    _statusLabel.translatesAutoresizingMaskIntoConstraints = NO;
    _statusLabel.numberOfLines = 0;
    _statusLabel.textAlignment = NSTextAlignmentCenter;
    _statusLabel.font = [UIFont systemFontOfSize:15.0];
    [self.toolContentView addSubview:_statusLabel];
    [NSLayoutConstraint activateConstraints:@[
        [_statusLabel.centerXAnchor constraintEqualToAnchor:self.toolContentView.centerXAnchor],
        [_statusLabel.centerYAnchor constraintEqualToAnchor:self.toolContentView.centerYAnchor],
        [_statusLabel.leadingAnchor constraintGreaterThanOrEqualToAnchor:self.toolContentView.leadingAnchor constant:20],
        [_statusLabel.trailingAnchor constraintLessThanOrEqualToAnchor:self.toolContentView.trailingAnchor constant:-20],
    ]];
}

- (void)buildProgressCard {
    _progressCard = [self workspaceThemeCardView];
    _progressCard.hidden = YES;
    [self.toolContentView addSubview:_progressCard];

    _progressLabel = [self workspaceThemeSecondaryLabelWithTextStyle:UIFontTextStyleFootnote monospaced:NO];
    _progressLabel.textAlignment = NSTextAlignmentCenter;
    _progressLabel.text = @"Preparing video…";

    _progressView = [self workspaceThemeProgressView];

    _cancelButton = [UIButton buttonWithType:UIButtonTypeSystem];
    _cancelButton.translatesAutoresizingMaskIntoConstraints = NO;
    [_cancelButton setTitle:@"Cancel" forState:UIControlStateNormal];
    _cancelButton.accessibilityHint = @"Cancels the video extraction process.";
    [_cancelButton addTarget:self action:@selector(cancelButtonTapped) forControlEvents:UIControlEventTouchUpInside];

    UIStackView *stack = [[UIStackView alloc] initWithArrangedSubviews:@[_progressLabel, _progressView, _cancelButton]];
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 10.0;
    stack.alignment = UIStackViewAlignmentCenter;
    [_progressCard addSubview:stack];

    [NSLayoutConstraint activateConstraints:@[
        [stack.topAnchor constraintEqualToAnchor:_progressCard.topAnchor constant:16],
        [stack.leadingAnchor constraintEqualToAnchor:_progressCard.leadingAnchor constant:16],
        [stack.trailingAnchor constraintEqualToAnchor:_progressCard.trailingAnchor constant:-16],
        [stack.bottomAnchor constraintEqualToAnchor:_progressCard.bottomAnchor constant:-16],
        [_progressView.widthAnchor constraintEqualToConstant:220],

        [_progressCard.centerXAnchor constraintEqualToAnchor:self.toolContentView.centerXAnchor],
        [_progressCard.centerYAnchor constraintEqualToAnchor:self.toolContentView.centerYAnchor],
        [_progressCard.widthAnchor constraintLessThanOrEqualToAnchor:self.toolContentView.widthAnchor constant:-40],
    ]];
}

#pragma mark Theme

- (void)workspaceApplyTheme {
    [super workspaceApplyTheme];
    // The progress card/label/progress view are all base-class factory
    // products, already auto-tracked and recolored by super's implementation.
    // primary/secondary/accent are only guaranteed to contrast against the
    // theme's card surfaces, never the raw gradient backdrop -- give the
    // content area an opaque card backing so the status label (and any
    // letterboxing around a loaded video) reads correctly. Harmless once a
    // video is playing, since the player view then covers the whole area.
    self.toolContentView.backgroundColor = [self.workspaceTheme[@"card"] colorWithAlphaComponent:0.95];
    _statusLabel.textColor = self.workspaceTheme[@"secondary"];
}

#pragma mark WorkspaceFileOpenable

- (void)workspaceOpenFileAtGuestPath:(NSString *)guestPath {
    [self loadPath:guestPath];
}

#pragma mark WorkspaceStatefulTool

// Which file was open, so a resumed workspace does not come back with an empty
// player. Same shape as the image viewer: the PATH travels, not the media --
// the file is in the guest filesystem, which the checkpoint restores anyway.
//
// Playback POSITION is deliberately not carried: it lives in an AVPlayerItem
// that does not exist yet at restore time, and reopening at the start of the
// file is a normal thing for a player to do, where jumping to a stale offset in
// a file that may have changed is not.
- (nullable NSDictionary<NSString *, id> *)workspaceToolStateForSaving {
    return _currentPath.length > 0 ? @{@"path": _currentPath} : nil;
}

- (void)workspaceRestoreToolState:(NSDictionary<NSString *, id> *)state {
    NSString *path = [state[@"path"] isKindOfClass:NSString.class] ? state[@"path"] : nil;
    if (path.length > 0)
        [self loadPath:path];  // a since-deleted file shows the load error, honestly
}

#pragma mark Loading

- (void)reload {
    if (_currentPath != nil) [self loadPath:_currentPath];
}

- (void)loadPath:(NSString *)path {
    [self cancelExtraction];
    [self removeItemObserverIfNeeded];
    _currentPath = path;
    _playerViewController.player = nil;
    _playerViewController.view.hidden = YES;
    _progressCard.hidden = YES;
    _statusLabel.hidden = NO;
    _statusLabel.text = @"Loading…";

    NSInteger generation = ++_loadGeneration;
    __weak typeof(self) weakSelf = self;
    _activeExtractionToken = [ISHGuestFileBridge.sharedBridge extractToTempFileAtGuestPath:path
        progress:^(int64_t written, int64_t total) {
            typeof(self) strongSelf = weakSelf;
            if (strongSelf == nil || strongSelf->_loadGeneration != generation) return;
            [strongSelf updateProgressWritten:written total:total];
        }
        completion:^(NSURL *fileURL, NSError *error) {
            typeof(self) strongSelf = weakSelf;
            if (strongSelf == nil || strongSelf->_loadGeneration != generation)
                return;  // a newer navigation superseded this load
            strongSelf->_activeExtractionToken = nil;
            strongSelf->_progressCard.hidden = YES;
            if (fileURL == nil) {
                strongSelf->_statusLabel.hidden = NO;
                strongSelf->_statusLabel.text = [strongSelf messageForLoadError:error];
                return;
            }
            [strongSelf playFileAtURL:fileURL];
        }];

    // Realfs files resolve instantly (no progress callback ever fires) --
    // only reveal the progress card if we're still waiting a moment later,
    // so the common fast path doesn't flash it uselessly.
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.25 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        typeof(self) strongSelf = weakSelf;
        if (strongSelf == nil || strongSelf->_loadGeneration != generation) return;
        if (strongSelf->_activeExtractionToken != nil) {
            strongSelf->_progressCard.hidden = NO;
            strongSelf->_statusLabel.hidden = YES;
        }
    });
}

- (void)updateProgressWritten:(int64_t)written total:(int64_t)total {
    _progressView.progress = (total > 0) ? (float)((double)written / (double)total) : 0.0;
    static NSByteCountFormatter *formatter;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ formatter = [NSByteCountFormatter new]; });
    _progressLabel.text = (total > 0)
        ? [NSString stringWithFormat:@"Preparing video… %@ of %@", [formatter stringFromByteCount:written], [formatter stringFromByteCount:total]]
        : @"Preparing video…";
}

- (void)cancelButtonTapped {
    [self cancelExtraction];
    _progressCard.hidden = YES;
    _statusLabel.hidden = NO;
    _statusLabel.text = @"Cancelled.";
}

- (void)cancelExtraction {
    if (_activeExtractionToken != nil) {
        [ISHGuestFileBridge.sharedBridge cancelExtraction:_activeExtractionToken];
        _activeExtractionToken = nil;
    }
}

- (NSString *)messageForLoadError:(NSError *)error {
    if ([error.domain isEqualToString:ISHGuestFileErrorDomain] && error.code == ISHGuestFileBridgeErrorCancelled)
        return @"Cancelled.";
    return error.localizedDescription.length ? error.localizedDescription : @"Couldn’t open this video.";
}

// Rather than pre-filtering by extension, this hands anything to AVPlayer and
// surfaces whatever AVFoundation itself reports -- a genuinely unsupported
// container (mkv/webm and friends) fails with a real, accurate error instead
// of a guessed one, and nothing here has to maintain a codec support list.
//
// Audio/video coexistence needs no explicit "pause Music" call: AVPlayer
// activates the shared AVAudioSession when it starts playing, which posts
// AVAudioSessionInterruptionNotification to other active sessions --
// AudioPlayerEngine.m already observes that notification and pauses itself
// on .began (see its -handleInterruption:), so Music yields automatically.
- (void)playFileAtURL:(NSURL *)url {
    AVPlayerItem *item = [AVPlayerItem playerItemWithURL:url];
    _observedItem = item;
    [item addObserver:self forKeyPath:@"status" options:NSKeyValueObservingOptionNew context:kWorkspaceVideoPlayerItemStatusContext];

    AVPlayer *player = [AVPlayer playerWithPlayerItem:item];
    _playerViewController.player = player;
    _playerViewController.view.hidden = NO;
    _statusLabel.hidden = YES;
    [player play];
}

- (void)removeItemObserverIfNeeded {
    if (_observedItem != nil) {
        [_observedItem removeObserver:self forKeyPath:@"status" context:kWorkspaceVideoPlayerItemStatusContext];
        _observedItem = nil;
    }
}

- (void)observeValueForKeyPath:(nullable NSString *)keyPath ofObject:(nullable id)object
                         change:(nullable NSDictionary<NSKeyValueChangeKey, id> *)change context:(nullable void *)context {
    if (context != kWorkspaceVideoPlayerItemStatusContext) {
        [super observeValueForKeyPath:keyPath ofObject:object change:change context:context];
        return;
    }
    AVPlayerItem *item = (AVPlayerItem *)object;
    if (item.status != AVPlayerItemStatusFailed)
        return;
    __weak typeof(self) weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        typeof(self) strongSelf = weakSelf;
        if (strongSelf == nil || item != strongSelf->_observedItem)
            return;  // superseded by a newer load
        strongSelf->_playerViewController.view.hidden = YES;
        strongSelf->_playerViewController.player = nil;
        strongSelf->_statusLabel.hidden = NO;
        strongSelf->_statusLabel.text = item.error.localizedDescription.length
            ? item.error.localizedDescription
            : @"This video format isn’t supported.";
    });
}

@end
