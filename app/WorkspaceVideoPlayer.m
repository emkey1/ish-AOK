//
//  WorkspaceVideoPlayer.m
//  iSH-AOK
//

#import "WorkspaceVideoPlayer.h"
#import "GuestFileBridge.h"
#import <AVKit/AVKit.h>
#import <AVFoundation/AVFoundation.h>

static void *kWorkspaceVideoPlayerItemStatusContext = &kWorkspaceVideoPlayerItemStatusContext;

#pragma mark - Playlists

// One entry of an .m3u / .m3u8 playlist: the name to show and where it is -- a
// URL, or a guest path for a playlist that came from the guest filesystem.
@interface ISHVideoPlaylistEntry : NSObject
@property (nonatomic, copy) NSString *title;
@property (nonatomic, copy) NSString *location;
@end
@implementation ISHVideoPlaylistEntry
@end

// A playlist's text, read. Two different things share the extensions: an HLS
// playlist (#EXT-X- tags) is ONE stream, cut into segments, which AVPlayer plays
// from its URL; any other .m3u is a LIST of streams or files, one per line, with
// optional "#EXTINF:<duration> <attributes>,<title>" lines naming the next one --
// the shape of IPTV channel lists. `hls` says which.
static NSArray<ISHVideoPlaylistEntry *> *ISHVideoParsePlaylist(NSString *text, BOOL *hls) {
    *hls = NO;
    NSMutableArray<ISHVideoPlaylistEntry *> *entries = [NSMutableArray array];
    NSString *pendingTitle = nil;
    if ([text hasPrefix:@"\uFEFF"])
        text = [text substringFromIndex:1];
    for (NSString *raw in [text componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet]) {
        NSString *line = [raw stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        if (line.length == 0)
            continue;
        if ([line hasPrefix:@"#"]) {
            if ([line hasPrefix:@"#EXT-X-"]) {
                *hls = YES;
            } else if ([line hasPrefix:@"#EXTINF:"]) {
                // The title follows the first comma outside the attributes'
                // quotes: tvg-name="News, Sport" must not end it early.
                BOOL quoted = NO;
                pendingTitle = nil;
                for (NSUInteger i = 8; i < line.length; i++) {
                    unichar c = [line characterAtIndex:i];
                    if (c == '"') {
                        quoted = !quoted;
                    } else if (c == ',' && !quoted) {
                        NSString *title = [[line substringFromIndex:i + 1]
                            stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
                        pendingTitle = title.length > 0 ? title : nil;
                        break;
                    }
                }
            }
            continue;
        }
        ISHVideoPlaylistEntry *entry = [ISHVideoPlaylistEntry new];
        entry.location = line;
        entry.title = pendingTitle ?: line.lastPathComponent;
        pendingTitle = nil;
        [entries addObject:entry];
    }
    return entries;
}

static BOOL ISHVideoIsRemoteLocation(NSString *location) {
    NSString *lower = location.lowercaseString;
    return [lower hasPrefix:@"http://"] || [lower hasPrefix:@"https://"];
}

static BOOL ISHVideoIsPlaylistPath(NSString *path) {
    NSString *extension = path.pathExtension.lowercaseString;
    return [extension isEqualToString:@"m3u"] || [extension isEqualToString:@"m3u8"];
}

// A remote address worth reading as a playlist before playing: one ending in
// .m3u / .m3u8, or an IPTV-style link that names the format in its query
// (get.php?...&type=m3u_plus). Anything else goes to AVPlayer as it is.
static BOOL ISHVideoURLLooksLikePlaylist(NSURL *url) {
    return ISHVideoIsPlaylistPath(url.path) || [url.query.lowercaseString containsString:@"m3u"];
}

// The channel list: every entry, filtered by what is typed in the search field.
@interface ISHVideoPlaylistPicker : UITableViewController <UISearchResultsUpdating>
@property (nonatomic, copy) NSArray<ISHVideoPlaylistEntry *> *entries;
@property (nonatomic, copy) void (^onPick)(NSUInteger index);
@end

@implementation ISHVideoPlaylistPicker {
    NSArray<NSNumber *> *_shown;  // indexes into entries
    UISearchController *_search;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = [NSString stringWithFormat:NSLocalizedString(@"Channels (%lu)", @"Playlist screen title; %lu is the number of entries"), (unsigned long) self.entries.count];
    self.navigationItem.rightBarButtonItem =
        [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemClose
                                                      target:self action:@selector(close)];
    _search = [[UISearchController alloc] initWithSearchResultsController:nil];
    _search.searchResultsUpdater = self;
    _search.obscuresBackgroundDuringPresentation = NO;
    _search.searchBar.placeholder = NSLocalizedString(@"Search", @"Search field placeholder in the playlist screen");
    self.navigationItem.searchController = _search;
    self.navigationItem.hidesSearchBarWhenScrolling = NO;
    self.definesPresentationContext = YES;
    [self filter:nil];
}

- (void)close {
    [self dismissViewControllerAnimated:YES completion:nil];
}

- (void)filter:(NSString *)query {
    NSMutableArray<NSNumber *> *shown = [NSMutableArray array];
    [self.entries enumerateObjectsUsingBlock:^(ISHVideoPlaylistEntry *entry, NSUInteger i, BOOL *stop) {
        if (query.length == 0 || [entry.title localizedCaseInsensitiveContainsString:query])
            [shown addObject:@(i)];
    }];
    _shown = shown;
    [self.tableView reloadData];
}

- (void)updateSearchResultsForSearchController:(UISearchController *)searchController {
    [self filter:searchController.searchBar.text];
}

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    return (NSInteger) _shown.count;
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:@"entry"]
        ?: [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:@"entry"];
    ISHVideoPlaylistEntry *entry = self.entries[_shown[(NSUInteger) indexPath.row].unsignedIntegerValue];
    cell.textLabel.text = entry.title;
    cell.detailTextLabel.text = [entry.title isEqualToString:entry.location] ? nil : entry.location;
    cell.detailTextLabel.textColor = UIColor.secondaryLabelColor;
    return cell;
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    NSUInteger index = _shown[(NSUInteger) indexPath.row].unsignedIntegerValue;
    void (^onPick)(NSUInteger) = self.onPick;
    [self.presentingViewController dismissViewControllerAnimated:YES completion:^{
        if (onPick != nil)
            onPick(index);
    }];
}

@end

#pragma mark - The player

@implementation WorkspaceVideoPlayerToolViewController {
    AVPlayerViewController *_playerViewController;
    UILabel *_statusLabel;

    UIView *_progressCard;
    UILabel *_progressLabel;
    UIProgressView *_progressView;
    UIButton *_cancelButton;

    UIStackView *_bar;
    UIButton *_openURLButton;
    UIButton *_channelsButton;

    NSString *_currentPath;     // the guest file playing, if it is one
    NSURL *_currentURL;         // the stream playing, if it is one
    NSString *_playlistSource;  // where the loaded playlist came from: a URL string or guest path
    NSArray<ISHVideoPlaylistEntry *> *_playlistEntries;
    NSInteger _playlistIndex;   // the entry playing, or -1
    ISHGuestFileExtractionToken _activeExtractionToken;
    AVPlayerItem *_observedItem;  // the one item we're KVO-observing "status" on, if any
    NSInteger _loadGeneration;    // discards stale async loads
}

#pragma mark Lifecycle

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = NSLocalizedString(@"Video", @"Window title of the video player");

    _playlistIndex = -1;
    [self buildBar];
    [self buildPlayer];
    [self buildStatusLabel];
    [self buildProgressCard];

    _statusLabel.text = NSLocalizedString(@"Open a video from the File Manager, or a stream with Open URL.", @"Video player empty-state text");
}

- (void)dealloc {
    // Stop a still-running extraction; without this, closing the window
    // mid-copy of a large video keeps the ioQueue copying to completion.
    if (_activeExtractionToken != nil)
        [ISHGuestFileBridge.sharedBridge cancelExtraction:_activeExtractionToken];
    [self removeItemObserverIfNeeded];
}

#pragma mark View construction

// Open URL, and Channels once a playlist with more than one entry is loaded.
// The window's own menu is the Workspace menu, so the player's actions live
// here, above the picture, where they stay reachable while it plays.
- (void)buildBar {
    _openURLButton = [self barButtonWithTitle:NSLocalizedString(@"Open URL", @"Video player toolbar button") symbol:@"link" action:@selector(openURLTapped)];
    _openURLButton.accessibilityHint = NSLocalizedString(@"Plays a video stream or an .m3u playlist from a web address.", @"Accessibility hint for the Open URL button");
    _channelsButton = [self barButtonWithTitle:NSLocalizedString(@"Channels", @"Video player toolbar button: list playlist entries") symbol:@"list.bullet" action:@selector(channelsTapped)];
    _channelsButton.accessibilityHint = NSLocalizedString(@"Lists the entries of the loaded playlist.", @"Accessibility hint for the Channels button");
    _channelsButton.hidden = YES;
    UIView *spacer = [UIView new];
    _bar = [[UIStackView alloc] initWithArrangedSubviews:@[_openURLButton, _channelsButton, spacer]];
    _bar.translatesAutoresizingMaskIntoConstraints = NO;
    _bar.axis = UILayoutConstraintAxisHorizontal;
    _bar.spacing = 16.0;
    [self.toolContentView addSubview:_bar];
    [NSLayoutConstraint activateConstraints:@[
        [_bar.topAnchor constraintEqualToAnchor:self.toolContentView.topAnchor constant:4],
        [_bar.leadingAnchor constraintEqualToAnchor:self.toolContentView.leadingAnchor constant:12],
        [_bar.trailingAnchor constraintEqualToAnchor:self.toolContentView.trailingAnchor constant:-12],
        [_bar.heightAnchor constraintEqualToConstant:32],
    ]];
}

- (UIButton *)barButtonWithTitle:(NSString *)title symbol:(NSString *)symbol action:(SEL)action {
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    [button setTitle:[@" " stringByAppendingString:title] forState:UIControlStateNormal];
    [button setImage:[UIImage systemImageNamed:symbol] forState:UIControlStateNormal];
    button.titleLabel.font = [UIFont systemFontOfSize:14 weight:UIFontWeightSemibold];
    [button addTarget:self action:action forControlEvents:UIControlEventTouchUpInside];
    return button;
}

- (void)buildPlayer {
    _playerViewController = [AVPlayerViewController new];
    [self addChildViewController:_playerViewController];
    _playerViewController.view.translatesAutoresizingMaskIntoConstraints = NO;
    _playerViewController.view.hidden = YES;  // shown once a player item is actually ready
    [self.toolContentView addSubview:_playerViewController.view];
    [NSLayoutConstraint activateConstraints:@[
        [_playerViewController.view.topAnchor constraintEqualToAnchor:_bar.bottomAnchor constant:4],
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
    _progressLabel.text = NSLocalizedString(@"Preparing video…", @"Video player progress text");

    _progressView = [self workspaceThemeProgressView];

    _cancelButton = [UIButton buttonWithType:UIButtonTypeSystem];
    _cancelButton.translatesAutoresizingMaskIntoConstraints = NO;
    [_cancelButton setTitle:NSLocalizedString(@"Cancel", @"Button that cancels preparing a video") forState:UIControlStateNormal];
    _cancelButton.accessibilityHint = NSLocalizedString(@"Cancels the video extraction process.", @"Accessibility hint for the Cancel button");
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
    _openURLButton.tintColor = self.workspaceTheme[@"accent"];
    _channelsButton.tintColor = self.workspaceTheme[@"accent"];
}

#pragma mark WorkspaceFileOpenable

// ws-videoplayer passes a stream's address through here as well.
- (void)workspaceOpenFileAtGuestPath:(NSString *)guestPath {
    if (ISHVideoIsRemoteLocation(guestPath))
        [self openURLString:guestPath];
    else
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
//
// A playlist travels as its source and the entry that was playing; a stream as
// its address. A restored playlist does not raise the channel list -- that is
// for when someone opens one.
- (nullable NSDictionary<NSString *, id> *)workspaceToolStateForSaving {
    if (_playlistSource.length > 0)
        return @{@"playlist": _playlistSource, @"entry": @(_playlistIndex)};
    if (_currentURL != nil)
        return @{@"url": _currentURL.absoluteString};
    return _currentPath.length > 0 ? @{@"path": _currentPath} : nil;
}

- (void)workspaceRestoreToolState:(NSDictionary<NSString *, id> *)state {
    NSString *playlist = [state[@"playlist"] isKindOfClass:NSString.class] ? state[@"playlist"] : nil;
    NSString *url = [state[@"url"] isKindOfClass:NSString.class] ? state[@"url"] : nil;
    NSString *path = [state[@"path"] isKindOfClass:NSString.class] ? state[@"path"] : nil;
    if (playlist.length > 0) {
        NSInteger entry = [state[@"entry"] isKindOfClass:NSNumber.class] ? [state[@"entry"] integerValue] : -1;
        [self loadPlaylistFrom:playlist playEntry:entry];
    } else if (url.length > 0) {
        [self openURLString:url];
    } else if (path.length > 0) {
        [self loadPath:path];  // a since-deleted file shows the load error, honestly
    }
}

#pragma mark Loading

- (void)reload {
    if (_currentPath != nil) [self loadMediaAtGuestPath:_currentPath];
}

// Everything playing or loading stops, and the status line says `text`.
- (void)resetToStatus:(NSString *)text {
    [self cancelExtraction];
    [self removeItemObserverIfNeeded];
    _loadGeneration++;
    _playerViewController.player = nil;
    _playerViewController.view.hidden = YES;
    _progressCard.hidden = YES;
    _statusLabel.hidden = NO;
    _statusLabel.text = text;
}

- (void)forgetPlaylist {
    _playlistSource = nil;
    _playlistEntries = nil;
    _playlistIndex = -1;
    _channelsButton.hidden = YES;
}

// A guest file: a playlist is read and listed, anything else extracted and played.
- (void)loadPath:(NSString *)path {
    if (ISHVideoIsPlaylistPath(path)) {
        [self loadPlaylistFrom:path playEntry:-1];
        return;
    }
    [self forgetPlaylist];
    [self loadMediaAtGuestPath:path];
}

- (void)loadMediaAtGuestPath:(NSString *)path {
    [self cancelExtraction];
    [self removeItemObserverIfNeeded];
    _currentPath = path;
    _currentURL = nil;
    _playerViewController.player = nil;
    _playerViewController.view.hidden = YES;
    _progressCard.hidden = YES;
    _statusLabel.hidden = NO;
    _statusLabel.text = NSLocalizedString(@"Loading…", @"Video player status text");

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
            [strongSelf playMediaAtURL:fileURL];
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
        ? [NSString stringWithFormat:NSLocalizedString(@"Preparing video… %@ of %@", @"Video player progress text; byte counts so far and in total"), [formatter stringFromByteCount:written], [formatter stringFromByteCount:total]]
        : NSLocalizedString(@"Preparing video…", @"Video player progress text");
}

- (void)cancelButtonTapped {
    [self cancelExtraction];
    _progressCard.hidden = YES;
    _statusLabel.hidden = NO;
    _statusLabel.text = NSLocalizedString(@"Cancelled.", @"Video player status text");
}

- (void)cancelExtraction {
    if (_activeExtractionToken != nil) {
        [ISHGuestFileBridge.sharedBridge cancelExtraction:_activeExtractionToken];
        _activeExtractionToken = nil;
    }
}

- (NSString *)messageForLoadError:(NSError *)error {
    if ([error.domain isEqualToString:ISHGuestFileErrorDomain] && error.code == ISHGuestFileBridgeErrorCancelled)
        return NSLocalizedString(@"Cancelled.", @"Video player status text");
    return error.localizedDescription.length ? error.localizedDescription : NSLocalizedString(@"Couldn’t open this video.", @"Video player error text");
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
- (void)playMediaAtURL:(NSURL *)url {
    AVPlayerItem *item = [AVPlayerItem playerItemWithURL:url];
    _observedItem = item;
    [item addObserver:self forKeyPath:@"status" options:NSKeyValueObservingOptionNew context:kWorkspaceVideoPlayerItemStatusContext];

    AVPlayer *player = [AVPlayer playerWithPlayerItem:item];
    _playerViewController.player = player;
    _playerViewController.view.hidden = NO;
    _statusLabel.hidden = YES;
    [player play];
}

#pragma mark Streams and playlists

- (void)openURLTapped {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:NSLocalizedString(@"Open URL", @"Alert title")
        message:NSLocalizedString(@"A video, an HLS stream (.m3u8), or an .m3u playlist.", @"Open URL alert message")
        preferredStyle:UIAlertControllerStyleAlert];
    NSString *clip = UIPasteboard.generalPasteboard.hasStrings ? UIPasteboard.generalPasteboard.string : nil;
    [alert addTextFieldWithConfigurationHandler:^(UITextField *field) {
        field.placeholder = @"https://";
        field.keyboardType = UIKeyboardTypeURL;
        field.autocapitalizationType = UITextAutocapitalizationTypeNone;
        field.autocorrectionType = UITextAutocorrectionTypeNo;
        field.clearButtonMode = UITextFieldViewModeWhileEditing;
        if (ISHVideoIsRemoteLocation(clip ?: @""))
            field.text = [clip stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    }];
    __weak typeof(alert) weakAlert = alert;
    [alert addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"Cancel", @"Alert button") style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"Open", @"Alert button that opens the URL") style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction *action) {
        NSString *text = [weakAlert.textFields.firstObject.text
            stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        if (text.length > 0)
            [self openURLString:text];
    }]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)openURLString:(NSString *)text {
    NSURL *url = [NSURL URLWithString:text];
    if (url == nil || !ISHVideoIsRemoteLocation(text)) {
        [self resetToStatus:NSLocalizedString(@"That is not an http:// or https:// address.", @"Video player error text")];
        return;
    }
    if (ISHVideoURLLooksLikePlaylist(url)) {
        [self loadPlaylistFrom:url.absoluteString playEntry:-1];
        return;
    }
    [self forgetPlaylist];
    [self playRemoteURL:url];
}

- (void)playRemoteURL:(NSURL *)url {
    [self resetToStatus:NSLocalizedString(@"Loading…", @"Video player status text")];
    _currentPath = nil;
    _currentURL = url;
    [self playMediaAtURL:url];
}

// Read a playlist -- from a URL or a guest file -- and act on what it is: an
// HLS playlist plays as the one stream it is, a list with one entry plays
// that, and a longer list is offered as channels (and entry `play`, if one is
// given, starts).
- (void)loadPlaylistFrom:(NSString *)source playEntry:(NSInteger)play {
    [self resetToStatus:NSLocalizedString(@"Reading playlist…", @"Video player status text")];
    [self forgetPlaylist];
    NSInteger generation = _loadGeneration;
    __weak typeof(self) weakSelf = self;
    void (^done)(NSString *, NSError *) = ^(NSString *text, NSError *error) {
        dispatch_async(dispatch_get_main_queue(), ^{
            typeof(self) strongSelf = weakSelf;
            if (strongSelf == nil || strongSelf->_loadGeneration != generation)
                return;
            [strongSelf playlistText:text error:error from:source playEntry:play];
        });
    };
    if (ISHVideoIsRemoteLocation(source)) {
        NSURLRequest *request = [NSURLRequest requestWithURL:[NSURL URLWithString:source]
                                                 cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
                                             timeoutInterval:30];
        [[NSURLSession.sharedSession dataTaskWithRequest:request
            completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
            NSInteger status = [response isKindOfClass:NSHTTPURLResponse.class]
                ? ((NSHTTPURLResponse *) response).statusCode : 200;
            if (error == nil && (status < 200 || status >= 300))
                error = [NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorBadServerResponse
                    userInfo:@{NSLocalizedDescriptionKey:
                        [NSString stringWithFormat:NSLocalizedString(@"The server answered %ld.", @"Video player error; %ld is an HTTP status code"), (long) status]}];
            done(data != nil ? [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding]
                                   ?: [[NSString alloc] initWithData:data encoding:NSISOLatin1StringEncoding]
                             : nil,
                 error);
        }] resume];
        return;
    }
    _activeExtractionToken = [ISHGuestFileBridge.sharedBridge extractToTempFileAtGuestPath:source
        progress:nil
        completion:^(NSURL *fileURL, NSError *error) {
            typeof(self) strongSelf = weakSelf;
            if (strongSelf != nil)
                strongSelf->_activeExtractionToken = nil;
            NSData *data = fileURL != nil ? [NSData dataWithContentsOfURL:fileURL] : nil;
            done(data != nil ? [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding]
                                   ?: [[NSString alloc] initWithData:data encoding:NSISOLatin1StringEncoding]
                             : nil,
                 error);
        }];
}

- (void)playlistText:(NSString *)text error:(NSError *)error from:(NSString *)source playEntry:(NSInteger)play {
    BOOL remote = ISHVideoIsRemoteLocation(source);
    if (text == nil) {
        [self resetToStatus:error.localizedDescription.length > 0 ? error.localizedDescription
                                                                   : NSLocalizedString(@"Couldn’t read the playlist.", @"Video player error text")];
        return;
    }
    BOOL hls = NO;
    NSArray<ISHVideoPlaylistEntry *> *entries = ISHVideoParsePlaylist(text, &hls);
    if (hls) {
        // An HLS playlist is one stream, and AVPlayer reads it from its URL,
        // fetching the segments it names relative to that URL. A copy in the
        // guest filesystem has no URL its segments can be fetched against.
        if (remote)
            [self playRemoteURL:[NSURL URLWithString:source]];
        else
            [self resetToStatus:NSLocalizedString(@"This is an HLS stream. Open its web address with Open URL.", @"Video player status text")];
        return;
    }
    // Where each entry is: a URL as written or resolved against a remote
    // playlist's own address; a guest path as written or against a guest
    // playlist's directory.
    for (ISHVideoPlaylistEntry *entry in entries) {
        if (ISHVideoIsRemoteLocation(entry.location))
            continue;
        if (remote) {
            NSURL *resolved = [NSURL URLWithString:entry.location relativeToURL:[NSURL URLWithString:source]];
            if (resolved != nil)
                entry.location = resolved.absoluteString;
        } else if (![entry.location hasPrefix:@"/"]) {
            entry.location = [[source.stringByDeletingLastPathComponent
                stringByAppendingPathComponent:entry.location] stringByStandardizingPath];
        }
    }
    if (entries.count == 0) {
        // Not a list after all -- an IPTV link can answer with the stream itself.
        if (remote)
            [self playRemoteURL:[NSURL URLWithString:source]];
        else
            [self resetToStatus:NSLocalizedString(@"This playlist lists nothing to play.", @"Video player status text")];
        return;
    }
    _playlistSource = source;
    _playlistEntries = entries;
    _channelsButton.hidden = entries.count < 2;
    if (play >= 0 && (NSUInteger) play < entries.count) {
        [self playEntry:(NSUInteger) play];
    } else if (entries.count == 1) {
        [self playEntry:0];
    } else {
        _statusLabel.text = [NSString stringWithFormat:NSLocalizedString(@"%lu entries. Choose one with Channels.", @"Video player status text; %lu is the number of playlist entries"),
                                                       (unsigned long) entries.count];
        [self channelsTapped];
    }
}

- (void)playEntry:(NSUInteger)index {
    if (index >= _playlistEntries.count)
        return;
    _playlistIndex = (NSInteger) index;
    ISHVideoPlaylistEntry *entry = _playlistEntries[index];
    if (ISHVideoIsRemoteLocation(entry.location)) {
        NSURL *url = [NSURL URLWithString:entry.location];
        if (url != nil)
            [self playRemoteURL:url];
        else
            [self resetToStatus:NSLocalizedString(@"This entry’s address is not valid.", @"Video player error text")];
    } else {
        [self loadMediaAtGuestPath:entry.location];
    }
}

- (void)channelsTapped {
    if (_playlistEntries.count == 0)
        return;
    ISHVideoPlaylistPicker *picker = [[ISHVideoPlaylistPicker alloc] initWithStyle:UITableViewStylePlain];
    picker.entries = _playlistEntries;
    __weak typeof(self) weakSelf = self;
    picker.onPick = ^(NSUInteger index) {
        [weakSelf playEntry:index];
    };
    UINavigationController *navigation = [[UINavigationController alloc] initWithRootViewController:picker];
    navigation.modalPresentationStyle = UIModalPresentationFormSheet;
    [self presentViewController:navigation animated:YES completion:nil];
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
            : NSLocalizedString(@"This video or stream can’t be played.", @"Video player error text");
    });
}

@end
