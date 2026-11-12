//
//  WorkspaceTranscoder.m
//  iSH-AOK
//
//  The Video Converter applet. See WorkspaceTranscoder.h.
//
//  Three sources, each read where it is wherever that is possible:
//   - Photos, through PHPicker, which needs no library permission. The picker
//     hands over a copy, which is moved (not copied again) into the temporary
//     directory and removed when the job ends.
//   - The Files app, through a document picker, read in place under its
//     security scope.
//   - A guest file (the File Manager's Convert Video, `ws-transcode FILE`),
//     read from the host file that holds it (GuestFileBridge
//     -hostFileURLForGuestPath:), extracted only when there is none (tmpfs).
//
//  The output goes to Photos (a Photos source always; a guest source when
//  asked), beside the original (a guest source), or wherever the user picks
//  (a Files source: a picked file grants no access to its folder).
//

#import <AVFoundation/AVFoundation.h>
#import <CoreLocation/CoreLocation.h>
#import <Photos/Photos.h>
#import <PhotosUI/PhotosUI.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <stdatomic.h>
#import "WorkspaceTranscoder.h"
#import "GuestFileBridge.h"
#import "Transcode.h"

typedef NS_ENUM(NSInteger, TCSourceKind) {
    TCSourceNone,
    TCSourcePhotos,
    TCSourceFiles,
    TCSourceGuest,
};

// What the job's callbacks reach, shared between the worker thread and the
// main thread. A plain struct rather than the view controller: the worker
// must not touch UIKit, and must not keep the applet alive.
struct TCApplet {
    _Atomic bool cancel;
    _Atomic int64_t permille;
};

static void TCAppletProgress(double fraction, void *ctx) {
    struct TCApplet *a = ctx;
    atomic_store(&a->permille, (int64_t) (fraction * 1000));
}

static bool TCAppletShouldCancel(void *ctx) {
    struct TCApplet *a = ctx;
    return atomic_load(&a->cancel);
}

static NSString *TCSize(long long bytes) {
    return [NSByteCountFormatter stringFromByteCount:bytes countStyle:NSByteCountFormatterCountStyleFile];
}

// "+51.5007-000.1246+010.000/" -> a location.
static CLLocation *TCLocationFromISO6709(NSString *text) {
    if (text.length == 0)
        return nil;
    NSRegularExpression *re = [NSRegularExpression regularExpressionWithPattern:@"^([+-][0-9.]+)([+-][0-9.]+)([+-][0-9.]+)?" options:0 error:nil];
    NSTextCheckingResult *m = [re firstMatchInString:text options:0 range:NSMakeRange(0, text.length)];
    if (m == nil)
        return nil;
    double lat = [text substringWithRange:[m rangeAtIndex:1]].doubleValue;
    double lon = [text substringWithRange:[m rangeAtIndex:2]].doubleValue;
    double alt = [m rangeAtIndex:3].location != NSNotFound ? [text substringWithRange:[m rangeAtIndex:3]].doubleValue : 0;
    return [[CLLocation alloc] initWithCoordinate:CLLocationCoordinate2DMake(lat, lon) altitude:alt
                               horizontalAccuracy:0 verticalAccuracy:0 timestamp:NSDate.date];
}

@interface WorkspaceTranscoderToolViewController () <PHPickerViewControllerDelegate, UIDocumentPickerDelegate>
@end

@implementation WorkspaceTranscoderToolViewController {
    UIScrollView *_scroll;
    UIStackView *_stack;
    UIButton *_photosButton;
    UIButton *_filesButton;
    UILabel *_sourceLabel;
    UILabel *_sourceDetailLabel;
    UISegmentedControl *_codecControl;
    UISegmentedControl *_sizeControl;
    UISegmentedControl *_qualityControl;
    UILabel *_codecLabel, *_sizeLabel, *_qualityLabel;
    UIView *_photosRow;
    UILabel *_photosRowLabel;
    UISwitch *_photosSwitch;
    UILabel *_destinationLabel;
    UIButton *_convertButton;
    UIProgressView *_progressView;
    UILabel *_statusLabel;
    UIButton *_deleteOriginalButton;
    NSArray<UILabel *> *_secondaryLabels;

    TCSourceKind _sourceKind;
    NSString *_sourceName;
    NSURL *_sourceURL;              // what the encoder reads
    BOOL _sourceIsTemporary;        // ours to delete (a Photos copy, an extraction we made)
    BOOL _sourceIsScoped;           // a Files URL under security scope
    NSString *_guestPath;           // a guest source's path
    NSString *_assetIdentifier;     // a Photos source's asset
    NSUInteger _loadGeneration;

    struct TCApplet *_job;          // non-NULL while converting
    NSTimer *_progressTimer;
    NSURL *_pendingExport;          // a Files result waiting for the export picker
}

static NSArray<NSNumber *> *TCSizeLines(void) {
    return @[@0, @2160, @1080, @720, @480];
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = NSLocalizedString(@"Video Converter", @"Window title of the video converter applet");
    [self buildViews];
    [self workspaceApplyTheme];
    [self showNoSource];
}

// Closing the window cancels a conversion: the worker sees the flag, finds
// no applet to hand the result to, removes it and frees the shared state.
// Hiding it (another desktop) does not.
- (void)dealloc {
    if (_job != NULL) {
        atomic_store(&_job->cancel, true);
        UIApplication.sharedApplication.idleTimerDisabled = NO;
    }
    [_progressTimer invalidate];
    [self releaseSource];
}

#pragma mark Views

- (UILabel *)rowLabel:(NSString *)text {
    UILabel *label = [self workspaceThemeSecondaryLabelWithTextStyle:UIFontTextStyleFootnote monospaced:NO];
    label.text = text;
    return label;
}

- (UIButton *)sourceButton:(NSString *)title symbol:(NSString *)symbol action:(SEL)action {
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    [button setTitle:[@" " stringByAppendingString:title] forState:UIControlStateNormal];
    [button setImage:[UIImage systemImageNamed:symbol] forState:UIControlStateNormal];
    button.titleLabel.font = [UIFont systemFontOfSize:14 weight:UIFontWeightSemibold];
    [button addTarget:self action:action forControlEvents:UIControlEventTouchUpInside];
    return button;
}

- (void)buildViews {
    _scroll = [UIScrollView new];
    _scroll.translatesAutoresizingMaskIntoConstraints = NO;
    _scroll.alwaysBounceVertical = YES;
    [self.toolContentView addSubview:_scroll];

    _stack = [UIStackView new];
    _stack.translatesAutoresizingMaskIntoConstraints = NO;
    _stack.axis = UILayoutConstraintAxisVertical;
    _stack.spacing = 8;
    _stack.alignment = UIStackViewAlignmentFill;
    [_scroll addSubview:_stack];
    [NSLayoutConstraint activateConstraints:@[
        [_scroll.topAnchor constraintEqualToAnchor:self.toolContentView.topAnchor],
        [_scroll.leadingAnchor constraintEqualToAnchor:self.toolContentView.leadingAnchor],
        [_scroll.trailingAnchor constraintEqualToAnchor:self.toolContentView.trailingAnchor],
        [_scroll.bottomAnchor constraintEqualToAnchor:self.toolContentView.bottomAnchor],
        [_stack.topAnchor constraintEqualToAnchor:_scroll.contentLayoutGuide.topAnchor constant:10],
        [_stack.bottomAnchor constraintEqualToAnchor:_scroll.contentLayoutGuide.bottomAnchor constant:-14],
        [_stack.leadingAnchor constraintEqualToAnchor:_scroll.frameLayoutGuide.leadingAnchor constant:14],
        [_stack.trailingAnchor constraintEqualToAnchor:_scroll.frameLayoutGuide.trailingAnchor constant:-14],
    ]];

    _photosButton = [self sourceButton:NSLocalizedString(@"Photos", @"Video converter: pick a video from Photos")
                                symbol:@"photo.on.rectangle" action:@selector(photosTapped)];
    _photosButton.accessibilityHint = NSLocalizedString(@"Chooses a video from your photo library.", @"Accessibility hint for the video converter's Photos button");
    _filesButton = [self sourceButton:NSLocalizedString(@"Files", @"Video converter: pick a video from the Files app")
                               symbol:@"folder" action:@selector(filesTapped)];
    _filesButton.accessibilityHint = NSLocalizedString(@"Chooses a video from the Files app.", @"Accessibility hint for the video converter's Files button");
    UIStackView *bar = [[UIStackView alloc] initWithArrangedSubviews:@[_photosButton, _filesButton, [UIView new]]];
    bar.spacing = 16;
    [_stack addArrangedSubview:bar];

    UIView *card = [self workspaceThemeCardView];
    _sourceLabel = [self workspaceThemePrimaryLabelWithTextStyle:UIFontTextStyleHeadline monospaced:NO];
    _sourceLabel.numberOfLines = 2;
    _sourceLabel.lineBreakMode = NSLineBreakByTruncatingMiddle;
    _sourceDetailLabel = [self workspaceThemeSecondaryLabelWithTextStyle:UIFontTextStyleFootnote monospaced:NO];
    _sourceDetailLabel.numberOfLines = 0;
    UIStackView *cardStack = [[UIStackView alloc] initWithArrangedSubviews:@[_sourceLabel, _sourceDetailLabel]];
    cardStack.translatesAutoresizingMaskIntoConstraints = NO;
    cardStack.axis = UILayoutConstraintAxisVertical;
    cardStack.spacing = 4;
    [card addSubview:cardStack];
    [NSLayoutConstraint activateConstraints:@[
        [cardStack.topAnchor constraintEqualToAnchor:card.topAnchor constant:10],
        [cardStack.bottomAnchor constraintEqualToAnchor:card.bottomAnchor constant:-10],
        [cardStack.leadingAnchor constraintEqualToAnchor:card.leadingAnchor constant:12],
        [cardStack.trailingAnchor constraintEqualToAnchor:card.trailingAnchor constant:-12],
    ]];
    [_stack addArrangedSubview:card];
    [_stack setCustomSpacing:14 afterView:card];

    _codecLabel = [self rowLabel:NSLocalizedString(@"Format", @"Video converter: codec choice label")];
    _codecControl = [[UISegmentedControl alloc] initWithItems:@[@"HEVC", @"H.264"]];
    _codecControl.selectedSegmentIndex = 0;
    _codecControl.accessibilityLabel = NSLocalizedString(@"Format", @"Video converter: codec choice label");
    _codecControl.accessibilityHint = NSLocalizedString(@"HEVC makes smaller files; H.264 plays on more old devices.", @"Accessibility hint for the video converter's format choice");
    [_codecControl addTarget:self action:@selector(optionsChanged) forControlEvents:UIControlEventValueChanged];

    _sizeLabel = [self rowLabel:NSLocalizedString(@"Size", @"Video converter: picture size choice label")];
    _sizeControl = [[UISegmentedControl alloc] initWithItems:@[
        NSLocalizedString(@"Original", @"Video converter: keep the picture size"), @"4K", @"1080p", @"720p", @"480p"]];
    _sizeControl.selectedSegmentIndex = 0;
    _sizeControl.accessibilityLabel = NSLocalizedString(@"Size", @"Video converter: picture size choice label");
    _sizeControl.accessibilityHint = NSLocalizedString(@"Shrinks the picture; a smaller video is never made bigger.", @"Accessibility hint for the video converter's size choice");
    [_sizeControl addTarget:self action:@selector(optionsChanged) forControlEvents:UIControlEventValueChanged];

    _qualityLabel = [self rowLabel:NSLocalizedString(@"Quality", @"Video converter: quality choice label")];
    _qualityControl = [[UISegmentedControl alloc] initWithItems:@[
        NSLocalizedString(@"Smaller", @"Video converter quality: smallest file"),
        NSLocalizedString(@"Balanced", @"Video converter quality: default"),
        NSLocalizedString(@"Better", @"Video converter quality: best picture")]];
    _qualityControl.selectedSegmentIndex = 1;
    _qualityControl.accessibilityLabel = NSLocalizedString(@"Quality", @"Video converter: quality choice label");
    _qualityControl.accessibilityHint = NSLocalizedString(@"Trades file size against picture quality.", @"Accessibility hint for the video converter's quality choice");

    for (UIView *view in @[_codecLabel, _codecControl, _sizeLabel, _sizeControl, _qualityLabel, _qualityControl])
        [_stack addArrangedSubview:view];
    [_stack setCustomSpacing:2 afterView:_codecLabel];
    [_stack setCustomSpacing:2 afterView:_sizeLabel];
    [_stack setCustomSpacing:2 afterView:_qualityLabel];
    [_stack setCustomSpacing:12 afterView:_codecControl];
    [_stack setCustomSpacing:12 afterView:_sizeControl];
    [_stack setCustomSpacing:14 afterView:_qualityControl];

    _photosRowLabel = [self workspaceThemePrimaryLabelWithTextStyle:UIFontTextStyleSubheadline monospaced:NO];
    _photosRowLabel.text = NSLocalizedString(@"Also save to Photos", @"Video converter: switch label");
    _photosSwitch = [UISwitch new];
    _photosSwitch.accessibilityLabel = NSLocalizedString(@"Also save to Photos", @"Video converter: switch label");
    [_photosSwitch addTarget:self action:@selector(optionsChanged) forControlEvents:UIControlEventValueChanged];
    UIStackView *photosRow = [[UIStackView alloc] initWithArrangedSubviews:@[_photosRowLabel, _photosSwitch]];
    photosRow.alignment = UIStackViewAlignmentCenter;
    _photosRow = photosRow;
    [_stack addArrangedSubview:_photosRow];

    _destinationLabel = [self workspaceThemeSecondaryLabelWithTextStyle:UIFontTextStyleFootnote monospaced:NO];
    _destinationLabel.numberOfLines = 0;
    [_stack addArrangedSubview:_destinationLabel];
    [_stack setCustomSpacing:14 afterView:_destinationLabel];

    _convertButton = [UIButton buttonWithType:UIButtonTypeSystem];
    _convertButton.titleLabel.font = [UIFont systemFontOfSize:16 weight:UIFontWeightSemibold];
    _convertButton.layer.cornerRadius = 10;
    [_convertButton.heightAnchor constraintGreaterThanOrEqualToConstant:44].active = YES;
    [_convertButton addTarget:self action:@selector(convertTapped) forControlEvents:UIControlEventTouchUpInside];
    [_stack addArrangedSubview:_convertButton];

    _progressView = [self workspaceThemeProgressView];
    _progressView.hidden = YES;
    [_stack addArrangedSubview:_progressView];

    _statusLabel = [self workspaceThemeSecondaryLabelWithTextStyle:UIFontTextStyleFootnote monospaced:NO];
    _statusLabel.numberOfLines = 0;
    [_stack addArrangedSubview:_statusLabel];

    _deleteOriginalButton = [UIButton buttonWithType:UIButtonTypeSystem];
    [_deleteOriginalButton setTitle:NSLocalizedString(@"Delete Original from Photos…", @"Video converter button") forState:UIControlStateNormal];
    _deleteOriginalButton.accessibilityHint = NSLocalizedString(@"Asks to delete the original video; Photos keeps it in Recently Deleted for 30 days.", @"Accessibility hint for the video converter's delete-original button");
    _deleteOriginalButton.tintColor = UIColor.systemRedColor;
    _deleteOriginalButton.contentHorizontalAlignment = UIControlContentHorizontalAlignmentLeading;
    _deleteOriginalButton.hidden = YES;
    [_deleteOriginalButton addTarget:self action:@selector(deleteOriginalTapped) forControlEvents:UIControlEventTouchUpInside];
    [_stack addArrangedSubview:_deleteOriginalButton];

    _secondaryLabels = @[_codecLabel, _sizeLabel, _qualityLabel];
}

- (void)workspaceApplyTheme {
    [super workspaceApplyTheme];
    // The base class applies the theme from its own -viewDidLoad, before
    // -buildViews has made anything to colour; -viewDidLoad applies it again.
    if (_stack == nil)
        return;
    NSDictionary<NSString *, UIColor *> *theme = self.workspaceTheme;
    self.toolContentView.backgroundColor = [theme[@"card"] colorWithAlphaComponent:0.95];
    UIColor *accent = theme[@"accent"];
    _photosButton.tintColor = accent;
    _filesButton.tintColor = accent;
    _photosSwitch.onTintColor = accent;
    for (UISegmentedControl *control in @[_codecControl, _sizeControl, _qualityControl])
        control.selectedSegmentTintColor = [accent colorWithAlphaComponent:0.35];
    [self updateConvertButton];
}

#pragma mark State

- (BOOL)converting {
    return _job != NULL;
}

- (void)releaseSource {
    if (_sourceIsScoped)
        [_sourceURL stopAccessingSecurityScopedResource];
    if (_sourceIsTemporary && _sourceURL != nil)
        [NSFileManager.defaultManager removeItemAtURL:_sourceURL error:nil];
    _sourceURL = nil;
    _sourceIsScoped = NO;
    _sourceIsTemporary = NO;
}

- (void)showNoSource {
    _sourceLabel.text = NSLocalizedString(@"No video chosen", @"Video converter: empty state title");
    _sourceDetailLabel.text = NSLocalizedString(@"Choose one from Photos or Files, or use Convert Video on a file in the File Manager. The conversion runs on this device's hardware video encoder.", @"Video converter: empty state text");
    [self optionsChanged];
}

// The last conversion's result belongs to the last video.
- (void)clearResult {
    _statusLabel.text = nil;
    _progressView.hidden = YES;
    _deleteOriginalButton.hidden = YES;
}

- (void)setSourceKind:(TCSourceKind)kind name:(NSString *)name url:(NSURL *)url {
    _sourceKind = kind;
    _sourceName = name;
    _sourceURL = url;
    _sourceLabel.text = name;
    [self clearResult];
    _photosSwitch.on = kind == TCSourcePhotos;
    [self describeSource];
    [self optionsChanged];
}

// Size, duration, picture and codec, read off the file.
- (void)describeSource {
    NSURL *url = _sourceURL;
    NSUInteger generation = _loadGeneration;
    _sourceDetailLabel.text = NSLocalizedString(@"Reading…", @"Video converter: reading the chosen video");
    NSNumber *size = nil;
    [url getResourceValue:&size forKey:NSURLFileSizeKey error:nil];
    AVURLAsset *asset = [AVURLAsset URLAssetWithURL:url options:nil];
    __weak typeof(self) weakSelf = self;
    [asset loadValuesAsynchronouslyForKeys:@[@"duration", @"tracks"] completionHandler:^{
        NSString *text;
        AVAssetTrack *video = [asset tracksWithMediaType:AVMediaTypeVideo].firstObject;
        if (video == nil) {
            text = NSLocalizedString(@"This file has no video that iOS can read.", @"Video converter: unreadable source");
        } else {
            CGSize n = CGSizeApplyAffineTransform(video.naturalSize, video.preferredTransform);
            double seconds = CMTimeGetSeconds(asset.duration);
            NSDateComponentsFormatter *f = [NSDateComponentsFormatter new];
            f.allowedUnits = seconds >= 3600 ? (NSCalendarUnitHour | NSCalendarUnitMinute | NSCalendarUnitSecond)
                                             : (NSCalendarUnitMinute | NSCalendarUnitSecond);
            f.zeroFormattingBehavior = NSDateComponentsFormatterZeroFormattingBehaviorPad;
            CMFormatDescriptionRef format = (__bridge CMFormatDescriptionRef) video.formatDescriptions.firstObject;
            FourCharCode codec = format ? CMFormatDescriptionGetMediaSubType(format) : 0;
            NSString *codecName = codec == kCMVideoCodecType_HEVC ? @"HEVC" : codec == kCMVideoCodecType_H264 ? @"H.264" : @"video";
            text = [NSString stringWithFormat:@"%@ · %@ · %.0f×%.0f %@ · %.1f Mbit/s",
                    size ? TCSize(size.longLongValue) : @"?",
                    isfinite(seconds) ? [f stringFromTimeInterval:seconds] : @"?",
                    fabs(n.width), fabs(n.height), codecName, video.estimatedDataRate / 1e6];
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            typeof(self) strongSelf = weakSelf;
            if (strongSelf == nil || strongSelf->_loadGeneration != generation)
                return;
            strongSelf->_sourceDetailLabel.text = text;
        });
    }];
}

- (void)optionsChanged {
    BOOL guest = _sourceKind == TCSourceGuest;
    _photosRow.hidden = !guest;
    NSString *dest = nil;
    switch (_sourceKind) {
    case TCSourceNone:
        break;
    case TCSourcePhotos:
        dest = NSLocalizedString(@"The converted video is added to Photos, with the original's date and place. The original stays until you delete it.", @"Video converter: where a Photos source's result goes");
        break;
    case TCSourceFiles:
        dest = NSLocalizedString(@"You choose where to save the converted video when it is done.", @"Video converter: where a Files source's result goes");
        break;
    case TCSourceGuest:
        dest = [NSString stringWithFormat:NSLocalizedString(@"Saved beside the original as %@.", @"Video converter: where a guest file's result goes; %@ is the file name"),
                [self guestOutputPath].lastPathComponent];
        break;
    }
    _destinationLabel.text = dest;
    _destinationLabel.hidden = dest == nil;
    [self updateConvertButton];
}

- (void)updateConvertButton {
    BOOL busy = [self converting];
    NSString *title = busy ? NSLocalizedString(@"Cancel", @"Video converter: stop the conversion")
                           : NSLocalizedString(@"Convert", @"Video converter: start the conversion");
    [_convertButton setTitle:title forState:UIControlStateNormal];
    _convertButton.accessibilityHint = busy
        ? NSLocalizedString(@"Stops the conversion and discards its file.", @"Accessibility hint for the video converter's Cancel button")
        : NSLocalizedString(@"Converts the chosen video with these settings.", @"Accessibility hint for the video converter's Convert button");
    _convertButton.enabled = busy || _sourceURL != nil;
    UIColor *accent = self.workspaceTheme[@"accent"] ?: self.view.tintColor;
    _convertButton.backgroundColor = [(busy ? UIColor.systemRedColor : accent) colorWithAlphaComponent:_convertButton.enabled ? 0.2 : 0.08];
    _convertButton.tintColor = busy ? UIColor.systemRedColor : accent;
    _photosButton.enabled = !busy;
    _filesButton.enabled = !busy;
    _codecControl.enabled = !busy;
    _sizeControl.enabled = !busy;
    _qualityControl.enabled = !busy;
    _photosSwitch.enabled = !busy;
}

- (NSString *)guestOutputPath {
    NSString *dir = _guestPath.stringByDeletingLastPathComponent;
    NSString *base = _guestPath.lastPathComponent.stringByDeletingPathExtension;
    return [dir stringByAppendingPathComponent:[base stringByAppendingString:@"-converted.mp4"]];
}

#pragma mark Sources

- (void)photosTapped {
    PHPickerConfiguration *config = [[PHPickerConfiguration alloc] initWithPhotoLibrary:PHPhotoLibrary.sharedPhotoLibrary];
    config.filter = PHPickerFilter.videosFilter;
    config.selectionLimit = 1;
    // The original file, not a re-encoded one iOS made for compatibility.
    config.preferredAssetRepresentationMode = PHPickerConfigurationAssetRepresentationModeCurrent;
    PHPickerViewController *picker = [[PHPickerViewController alloc] initWithConfiguration:config];
    picker.delegate = self;
    [self presentViewController:picker animated:YES completion:nil];
}

- (void)picker:(PHPickerViewController *)picker didFinishPicking:(NSArray<PHPickerResult *> *)results {
    [picker dismissViewControllerAnimated:YES completion:nil];
    PHPickerResult *result = results.firstObject;
    if (result == nil)
        return;
    NSItemProvider *provider = result.itemProvider;
    NSString *type = [provider hasItemConformingToTypeIdentifier:UTTypeMovie.identifier] ? UTTypeMovie.identifier
                                                                                          : provider.registeredTypeIdentifiers.firstObject;
    if (type == nil)
        return;
    [self releaseSource];
    NSUInteger generation = ++_loadGeneration;
    _sourceKind = TCSourceNone;
    _sourceLabel.text = provider.suggestedName ?: NSLocalizedString(@"Video", @"Video converter: an unnamed video");
    _sourceDetailLabel.text = NSLocalizedString(@"Getting the video from Photos…", @"Video converter: waiting for Photos (it may download from iCloud)");
    [self clearResult];
    [self optionsChanged];
    NSString *assetIdentifier = result.assetIdentifier;
    __weak typeof(self) weakSelf = self;
    [provider loadFileRepresentationForTypeIdentifier:type completionHandler:^(NSURL *url, NSError *error) {
        // The file is gone when this returns: move it somewhere of ours. A
        // move, so a large video costs no second copy.
        NSURL *kept = nil;
        NSError *moveError = error;
        if (url != nil) {
            NSString *ext = url.pathExtension.length ? url.pathExtension : @"mov";
            NSString *name = [NSString stringWithFormat:@"%sin-%@.%@", TRANSCODE_TEMP_PREFIX, NSUUID.UUID.UUIDString, ext];
            kept = [NSURL fileURLWithPath:[NSTemporaryDirectory() stringByAppendingPathComponent:name]];
            if (![NSFileManager.defaultManager moveItemAtURL:url toURL:kept error:&moveError] &&
                    ![NSFileManager.defaultManager copyItemAtURL:url toURL:kept error:&moveError])
                kept = nil;
        }
        NSString *displayName = url.lastPathComponent;
        dispatch_async(dispatch_get_main_queue(), ^{
            typeof(self) strongSelf = weakSelf;
            if (strongSelf == nil || strongSelf->_loadGeneration != generation) {
                if (kept != nil)
                    [NSFileManager.defaultManager removeItemAtURL:kept error:nil];
                return;
            }
            if (kept == nil) {
                strongSelf->_sourceDetailLabel.text = moveError.localizedDescription ?: NSLocalizedString(@"Photos did not provide the video.", @"Video converter: Photos load failed");
                return;
            }
            strongSelf->_assetIdentifier = assetIdentifier;
            strongSelf->_guestPath = nil;
            strongSelf->_sourceIsTemporary = YES;
            [strongSelf setSourceKind:TCSourcePhotos name:provider.suggestedName ?: displayName url:kept];
        });
    }];
}

- (void)filesTapped {
    UIDocumentPickerViewController *picker =
        [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[UTTypeMovie] asCopy:NO];
    picker.delegate = self;
    picker.allowsMultipleSelection = NO;
    [self presentViewController:picker animated:YES completion:nil];
}

- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    if (_pendingExport != nil) {
        [self exportFinished:YES];
        return;
    }
    NSURL *url = urls.firstObject;
    if (url == nil)
        return;
    [self releaseSource];
    ++_loadGeneration;
    _sourceIsScoped = [url startAccessingSecurityScopedResource];
    _assetIdentifier = nil;
    _guestPath = nil;
    [self setSourceKind:TCSourceFiles name:url.lastPathComponent url:url];
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)controller {
    if (_pendingExport != nil)
        [self exportFinished:NO];
}

- (void)workspaceOpenFileAtGuestPath:(NSString *)guestPath {
    if ([self converting])
        return;
    [self releaseSource];
    NSUInteger generation = ++_loadGeneration;
    _sourceKind = TCSourceNone;
    _sourceLabel.text = guestPath.lastPathComponent;
    _sourceDetailLabel.text = NSLocalizedString(@"Reading…", @"Video converter: reading the chosen video");
    [self clearResult];
    [self optionsChanged];
    __weak typeof(self) weakSelf = self;
    [ISHGuestFileBridge.sharedBridge hostFileURLForGuestPath:guestPath completion:^(NSURL *hostURL, NSError *error) {
        typeof(self) strongSelf = weakSelf;
        if (strongSelf == nil || strongSelf->_loadGeneration != generation)
            return;
        if (hostURL != nil) {
            strongSelf->_guestPath = guestPath;
            strongSelf->_assetIdentifier = nil;
            [strongSelf setSourceKind:TCSourceGuest name:guestPath.lastPathComponent url:hostURL];
            return;
        }
        if (error != nil) {
            strongSelf->_sourceDetailLabel.text = error.localizedDescription;
            return;
        }
        // No host file of its own (tmpfs): a copy is the only way in.
        [ISHGuestFileBridge.sharedBridge extractToTempFileAtGuestPath:guestPath progress:nil completion:^(NSURL *fileURL, NSError *extractError) {
            typeof(self) s = weakSelf;
            if (s == nil || s->_loadGeneration != generation)
                return;
            if (fileURL == nil) {
                s->_sourceDetailLabel.text = extractError.localizedDescription;
                return;
            }
            s->_guestPath = guestPath;
            s->_assetIdentifier = nil;
            [s setSourceKind:TCSourceGuest name:guestPath.lastPathComponent url:fileURL];
        }];
    }];
}

#pragma mark Converting

- (void)convertTapped {
    if ([self converting]) {
        atomic_store(&_job->cancel, true);
        _statusLabel.text = NSLocalizedString(@"Cancelling…", @"Video converter status");
        return;
    }
    if (_sourceURL == nil)
        return;

    struct TCApplet *shared = calloc(1, sizeof(*shared));
    struct transcode_job *job = calloc(1, sizeof(*job));
    char *inPath = strdup(_sourceURL.fileSystemRepresentation);
    if (shared == NULL || job == NULL || inPath == NULL) {
        free(shared); free(job); free(inPath);
        return;
    }
    job->in_host_path = inPath;
    job->codec = _codecControl.selectedSegmentIndex == 1 ? TRANSCODE_H264 : TRANSCODE_HEVC;
    job->max_lines = TCSizeLines()[(NSUInteger) MAX(0, _sizeControl.selectedSegmentIndex)].intValue;
    static const double scales[] = {0.6, 1.0, 1.6};
    job->rate_scale = scales[MIN(2, MAX(0, _qualityControl.selectedSegmentIndex))];
    job->audio_bitrate = 128000;
    job->progress = TCAppletProgress;
    job->should_cancel = TCAppletShouldCancel;
    job->ctx = shared;
    _job = shared;

    NSDate *start = NSDate.date;
    long long inSize = 0;
    NSNumber *size = nil;
    if ([_sourceURL getResourceValue:&size forKey:NSURLFileSizeKey error:nil])
        inSize = size.longLongValue;
    _progressView.hidden = NO;
    _progressView.progress = 0;
    _deleteOriginalButton.hidden = YES;
    _statusLabel.text = NSLocalizedString(@"Converting…", @"Video converter status");
    [self updateConvertButton];
    // A conversion stops when iOS suspends the app, and an idle lock is the
    // usual way it gets suspended.
    UIApplication.sharedApplication.idleTimerDisabled = YES;
    __weak typeof(self) weakSelf = self;
    _progressTimer = [NSTimer scheduledTimerWithTimeInterval:0.25 repeats:YES block:^(NSTimer *timer) {
        typeof(self) strongSelf = weakSelf;
        if (strongSelf == nil || strongSelf->_job != shared) { [timer invalidate]; return; }
        double fraction = atomic_load(&shared->permille) / 1000.0;
        strongSelf->_progressView.progress = (float) fraction;
        if (!atomic_load(&shared->cancel)) {
            NSTimeInterval t = -start.timeIntervalSinceNow;
            NSString *left = fraction > 0.02
                ? [NSString stringWithFormat:NSLocalizedString(@", about %.0f s left", @"Video converter: time remaining; %.0f is seconds"), t / fraction - t]
                : @"";
            strongSelf->_statusLabel.text = [NSString stringWithFormat:NSLocalizedString(@"Converting… %.0f%%%@", @"Video converter progress; %.0f is the percentage, %@ the time remaining or nothing"), fraction * 100, left];
        }
    }];

    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        int res = ISHTranscodeRun(job);
        NSString *outPath = job->out_host_path[0] ? @(job->out_host_path) : nil;
        NSString *summary = @(job->summary);
        NSString *error = @(job->error);
        free((void *) job->in_host_path);
        free(job);
        dispatch_async(dispatch_get_main_queue(), ^{
            typeof(self) strongSelf = weakSelf;
            if (strongSelf == nil || strongSelf->_job != shared) {
                // The applet closed: nothing will save the result.
                if (outPath != nil)
                    [NSFileManager.defaultManager removeItemAtPath:outPath error:nil];
                free(shared);
                return;
            }
            strongSelf->_job = NULL;
            free(shared);
            [strongSelf->_progressTimer invalidate];
            strongSelf->_progressTimer = nil;
            UIApplication.sharedApplication.idleTimerDisabled = NO;
            [strongSelf jobEnded:res output:outPath summary:summary error:error
                         seconds:-start.timeIntervalSinceNow inputSize:inSize];
        });
    });
}

- (void)jobEnded:(int)res output:(NSString *)outPath summary:(NSString *)summary error:(NSString *)error
         seconds:(NSTimeInterval)seconds inputSize:(long long)inSize {
    [self updateConvertButton];
    if (res != 0) {
        _progressView.hidden = YES;
        if (outPath != nil)
            [NSFileManager.defaultManager removeItemAtPath:outPath error:nil];
        _statusLabel.text = res == 1 ? NSLocalizedString(@"Cancelled.", @"Video converter status")
                                     : [NSString stringWithFormat:NSLocalizedString(@"Conversion failed: %@", @"Video converter error; %@ is the reason"), error];
        return;
    }
    _progressView.progress = 1;
    NSNumber *size = nil;
    [[NSURL fileURLWithPath:outPath] getResourceValue:&size forKey:NSURLFileSizeKey error:nil];
    long long outSize = size.longLongValue;
    NSString *result = [NSString stringWithFormat:NSLocalizedString(@"%@ → %@ in %.1f s (%@).", @"Video converter result: input size, output size, seconds, details"),
                        TCSize(inSize), TCSize(outSize), seconds, summary];
    if (outSize > inSize && inSize > 0)
        result = [result stringByAppendingFormat:@" %@", NSLocalizedString(@"The result is larger than the original: it was already compact. Try a smaller size or quality.", @"Video converter: output grew")];
    _statusLabel.text = result;
    [self saveOutput:outPath result:result];
}

- (void)saveOutput:(NSString *)outPath result:(NSString *)result {
    NSURL *outURL = [NSURL fileURLWithPath:outPath];
    __weak typeof(self) weakSelf = self;
    switch (_sourceKind) {
    case TCSourcePhotos: {
        [self saveToPhotos:outURL move:YES then:^(BOOL ok, NSString *message) {
            typeof(self) s = weakSelf;
            if (s == nil) return;
            s->_statusLabel.text = [result stringByAppendingFormat:@"\n%@", message];
            s->_deleteOriginalButton.hidden = !ok || s->_assetIdentifier == nil;
        }];
        break;
    }
    case TCSourceFiles: {
        _pendingExport = outURL;
        UIDocumentPickerViewController *picker = [[UIDocumentPickerViewController alloc] initForExportingURLs:@[outURL] asCopy:NO];
        picker.delegate = self;
        [self presentViewController:picker animated:YES completion:nil];
        break;
    }
    case TCSourceGuest: {
        NSString *dest = [self guestOutputPath];
        BOOL alsoPhotos = _photosSwitch.on;
        NSData *data = [NSData dataWithContentsOfURL:outURL options:NSDataReadingMappedAlways error:nil];
        if (data == nil) {
            _statusLabel.text = [result stringByAppendingFormat:@"\n%@", NSLocalizedString(@"Could not read the converted video.", @"Video converter error")];
            [NSFileManager.defaultManager removeItemAtURL:outURL error:nil];
            break;
        }
        [ISHGuestFileBridge.sharedBridge writeData:data toGuestPath:dest completion:^(BOOL ok, NSError *writeError) {
            typeof(self) s = weakSelf;
            NSString *message = ok ? [NSString stringWithFormat:NSLocalizedString(@"Saved as %@.", @"Video converter: saved to a guest path; %@ is the path"), dest]
                                   : [NSString stringWithFormat:NSLocalizedString(@"Could not save %@: %@", @"Video converter: guest save failed; %1$@ path, %2$@ reason"), dest, writeError.localizedDescription];
            if (ok && alsoPhotos && s != nil) {
                [s saveToPhotos:outURL move:YES then:^(BOOL photosOK, NSString *photosMessage) {
                    typeof(self) s2 = weakSelf;
                    if (s2 != nil)
                        s2->_statusLabel.text = [result stringByAppendingFormat:@"\n%@ %@", message, photosMessage];
                }];
                return;
            }
            [NSFileManager.defaultManager removeItemAtURL:outURL error:nil];
            if (s != nil)
                s->_statusLabel.text = [result stringByAppendingFormat:@"\n%@", message];
        }];
        break;
    }
    case TCSourceNone: {
        [NSFileManager.defaultManager removeItemAtURL:outURL error:nil];
        break;
    }
    }
}

- (void)exportFinished:(BOOL)saved {
    // initForExportingURLs:asCopy:NO moves the file when saved; when not, it
    // is still ours.
    if (!saved)
        [NSFileManager.defaultManager removeItemAtURL:_pendingExport error:nil];
    _pendingExport = nil;
    NSString *line = saved ? NSLocalizedString(@"Saved.", @"Video converter: export finished")
                           : NSLocalizedString(@"Not saved.", @"Video converter: export cancelled");
    _statusLabel.text = [_statusLabel.text stringByAppendingFormat:@"\n%@", line];
}

// The original's date and place, so the copy sorts and maps where it did.
- (void)saveToPhotos:(NSURL *)url move:(BOOL)move then:(void (^)(BOOL ok, NSString *message))then {
    AVURLAsset *asset = [AVURLAsset URLAssetWithURL:url options:nil];
    [asset loadValuesAsynchronouslyForKeys:@[@"creationDate", @"metadata"] completionHandler:^{
        NSDate *date = asset.creationDate.dateValue;
        CLLocation *location = nil;
        for (AVMetadataItem *item in asset.metadata) {
            // QuickTime's ISO 6709 key in a .mov, the ISO user data 'loci' in
            // an .mp4; AVFoundation gives both as the ISO 6709 string.
            if ([item.identifier isEqualToString:AVMetadataIdentifierQuickTimeMetadataLocationISO6709] ||
                    [item.identifier hasSuffix:@"/loci"]) {
                location = TCLocationFromISO6709(item.stringValue);
                if (location != nil)
                    break;
            }
        }
        [PHPhotoLibrary requestAuthorizationForAccessLevel:PHAccessLevelAddOnly handler:^(PHAuthorizationStatus status) {
            if (status != PHAuthorizationStatusAuthorized && status != PHAuthorizationStatusLimited) {
                if (move)
                    [NSFileManager.defaultManager removeItemAtURL:url error:nil];
                dispatch_async(dispatch_get_main_queue(), ^{
                    then(NO, NSLocalizedString(@"Not saved: iSH-AOK may not add to Photos (Settings > iSH-AOK > Photos).", @"Video converter: Photos access refused"));
                });
                return;
            }
            [PHPhotoLibrary.sharedPhotoLibrary performChanges:^{
                PHAssetCreationRequest *request = [PHAssetCreationRequest creationRequestForAsset];
                PHAssetResourceCreationOptions *options = [PHAssetResourceCreationOptions new];
                options.shouldMoveFile = move;
                [request addResourceWithType:PHAssetResourceTypeVideo fileURL:url options:options];
                if (date != nil)
                    request.creationDate = date;
                if (location != nil)
                    request.location = location;
            } completionHandler:^(BOOL success, NSError *error) {
                if (move)
                    [NSFileManager.defaultManager removeItemAtURL:url error:nil];
                dispatch_async(dispatch_get_main_queue(), ^{
                    then(success, success ? NSLocalizedString(@"Added to Photos.", @"Video converter: saved to Photos")
                                          : [NSString stringWithFormat:NSLocalizedString(@"Not added to Photos: %@", @"Video converter: Photos save failed; %@ is the reason"), error.localizedDescription]);
                });
            }];
        }];
    }];
}

- (void)deleteOriginalTapped {
    NSString *identifier = _assetIdentifier;
    if (identifier == nil)
        return;
    __weak typeof(self) weakSelf = self;
    // Deleting needs read-write access, which picking never asked for. iOS
    // shows its own confirmation, and the video waits in Recently Deleted.
    [PHPhotoLibrary requestAuthorizationForAccessLevel:PHAccessLevelReadWrite handler:^(PHAuthorizationStatus status) {
        PHAsset *asset = nil;
        if (status == PHAuthorizationStatusAuthorized || status == PHAuthorizationStatusLimited)
            asset = [PHAsset fetchAssetsWithLocalIdentifiers:@[identifier] options:nil].firstObject;
        if (asset == nil) {
            dispatch_async(dispatch_get_main_queue(), ^{
                typeof(self) s = weakSelf;
                if (s == nil) return;
                s->_statusLabel.text = [s->_statusLabel.text stringByAppendingFormat:@"\n%@",
                    NSLocalizedString(@"iSH-AOK cannot see that video in your library (Settings > iSH-AOK > Photos allows full access). You can delete it in Photos.", @"Video converter: cannot delete the original")];
            });
            return;
        }
        [PHPhotoLibrary.sharedPhotoLibrary performChanges:^{
            [PHAssetChangeRequest deleteAssets:@[asset]];
        } completionHandler:^(BOOL success, NSError *error) {
            dispatch_async(dispatch_get_main_queue(), ^{
                typeof(self) s = weakSelf;
                if (s == nil || !success) return;
                s->_deleteOriginalButton.hidden = YES;
                s->_assetIdentifier = nil;
                s->_statusLabel.text = [s->_statusLabel.text stringByAppendingFormat:@"\n%@",
                    NSLocalizedString(@"Original deleted (it stays in Recently Deleted for 30 days).", @"Video converter: original deleted")];
            });
        }];
    }];
}

@end
