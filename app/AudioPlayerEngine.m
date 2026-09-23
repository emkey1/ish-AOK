//
//  AudioPlayerEngine.m
//  iSH-AOK
//

#import "AudioPlayerEngine.h"
#import "AudioPCMDecoder.h"
#import "AudioLibrary.h"
#import <AVFoundation/AVFoundation.h>
#import <MediaPlayer/MediaPlayer.h>
#import <UIKit/UIKit.h>
#include <stdatomic.h>

NSString *const ISHAudioPlayerStateDidChangeNotification = @"ISHAudioPlayerStateDidChangeNotification";
NSString *const ISHAudioPlayerTrackDidChangeNotification = @"ISHAudioPlayerTrackDidChangeNotification";
NSString *const ISHAudioPlayerQueueDidChangeNotification = @"ISHAudioPlayerQueueDidChangeNotification";

static NSString *const kAudioVolumeKey = @"ISHAudioPlayerVolume";
static NSString *const kAudioRepeatKey = @"ISHAudioPlayerRepeatMode";
static NSString *const kAudioShuffleKey = @"ISHAudioPlayerShuffle";

// Larger chunks + a deep in-flight cushion keep playback gapless on weak, CPU-
// contended devices (e.g. a 2-core A10 also running the x86 emulator): fewer
// scheduling events, and several seconds of audio buffered ahead of the play head
// so a transient CPU stall can't drain the queue.
static const AVAudioFrameCount kDecodeChunkFrames = 16384;
static const double kInFlightSeconds = 6.0;

#pragma mark - ISHAudioTrack

@implementation ISHAudioTrack

- (instancetype)initWithGuestPath:(NSString *)guestPath {
    self = [super init];
    if (self) {
        _guestPath = [guestPath copy];
        _title = [guestPath.lastPathComponent stringByDeletingPathExtension] ?: guestPath;
    }
    return self;
}

- (NSDictionary *)dictionaryRepresentation {
    NSMutableDictionary *dict = [NSMutableDictionary dictionary];
    dict[@"path"] = self.guestPath;
    if (self.title) dict[@"title"] = self.title;
    if (self.artist) dict[@"artist"] = self.artist;
    if (self.album) dict[@"album"] = self.album;
    if (self.duration > 0) dict[@"duration"] = @(self.duration);
    return dict;
}

+ (instancetype)trackWithDictionary:(NSDictionary *)dict {
    NSString *path = dict[@"path"];
    if (![path isKindOfClass:NSString.class] || path.length == 0)
        return nil;
    ISHAudioTrack *track = [[ISHAudioTrack alloc] initWithGuestPath:path];
    if ([dict[@"title"] isKindOfClass:NSString.class]) track.title = dict[@"title"];
    if ([dict[@"artist"] isKindOfClass:NSString.class]) track.artist = dict[@"artist"];
    if ([dict[@"album"] isKindOfClass:NSString.class]) track.album = dict[@"album"];
    if ([dict[@"duration"] isKindOfClass:NSNumber.class]) track.duration = [dict[@"duration"] doubleValue];
    return track;
}

@end

#pragma mark - ISHAudioPlayerEngine

@interface ISHAudioPlayerEngine ()
@property (nonatomic, strong) AVAudioEngine *avEngine;
@property (nonatomic, strong) AVAudioPlayerNode *playerNode;
@property (nonatomic, strong) dispatch_queue_t decodeQueue;
@property (nonatomic, strong) dispatch_queue_t sessionQueue;  // serializes the blocking AVAudioSession calls off the main thread
@property (nonatomic, strong, nullable) id<ISHPCMDecoder> decoder;

@property (nonatomic, strong) NSMutableArray<ISHAudioTrack *> *mutableQueue;
@property (nonatomic, assign) NSInteger currentIndex;
@property (nonatomic, assign) ISHAudioPlaybackState state;

// Bumped whenever the play head jumps (stop/seek/track change). Completion
// handlers and decode passes captured with an older generation no-op, which is
// how we keep the async buffer pipeline from acting on a stale track.
@property (nonatomic, assign) uint64_t generation;
@property (nonatomic, assign) AVAudioFramePosition inFlightFrames; // scheduled, not yet played
@property (nonatomic, assign) BOOL decoderAtEnd;

@property (nonatomic, assign) double seekBaseTime;   // wall time at node sampleTime 0
@property (nonatomic, assign) double pausedTime;     // currentTime captured while paused/stopped
@property (nonatomic, assign) double trackDuration;
@property (nonatomic, assign) double currentNodeSampleRate;

@property (nonatomic, assign) BOOL wasPlayingBeforeInterruption;
@property (nonatomic, strong) NSMutableArray<NSNumber *> *shuffleHistory;
@property (nonatomic, assign) NSInteger loadFailureStreak;  // consecutive undecodable tracks
@end

@implementation ISHAudioPlayerEngine

@synthesize repeatMode = _repeatMode;
@synthesize shuffleEnabled = _shuffleEnabled;
@synthesize volume = _volume;

+ (instancetype)sharedEngine {
    static ISHAudioPlayerEngine *shared;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ shared = [[ISHAudioPlayerEngine alloc] init]; });
    return shared;
}

- (instancetype)init {
    self = [super init];
    if (self) {
        _mutableQueue = [NSMutableArray array];
        _currentIndex = -1;
        _state = ISHAudioPlaybackStateStopped;
        _shuffleHistory = [NSMutableArray array];
        // USER_INTERACTIVE puts the audio supply ABOVE the emulator's guest-CPU
        // threads, which run at USER_INITIATED (kernel/task.c). At equal QoS the
        // decode queue tied with a busy guest (e.g. bash pegging a core at terminal
        // launch) and lost the core, starving the buffer; one level up, refills
        // preempt the guest so the queue stays full. The bursts are tiny, so this
        // doesn't meaningfully slow emulation.
        dispatch_queue_attr_t decodeAttr =
            dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0);
        _decodeQueue = dispatch_queue_create("app.ish.audio.decode", decodeAttr);
        _sessionQueue = dispatch_queue_create("app.ish.audio.session", DISPATCH_QUEUE_SERIAL);

        NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
        NSNumber *vol = [defaults objectForKey:kAudioVolumeKey];
        _volume = vol != nil ? vol.floatValue : 1.0f;
        _repeatMode = (ISHAudioRepeatMode)[defaults integerForKey:kAudioRepeatKey];
        _shuffleEnabled = [defaults boolForKey:kAudioShuffleKey];

        _avEngine = [[AVAudioEngine alloc] init];
        _playerNode = [[AVAudioPlayerNode alloc] init];
        [_avEngine attachNode:_playerNode];
        // Connect with a default format now; we reconnect per-track to the
        // decoder's processing format when a track loads.
        AVAudioFormat *defaultFormat = [_avEngine.mainMixerNode outputFormatForBus:0];
        [_avEngine connect:_playerNode to:_avEngine.mainMixerNode format:defaultFormat];
        _avEngine.mainMixerNode.outputVolume = _volume;

        [self configureAudioSession];
        [self setupRemoteCommands];
        [self observeSessionNotifications];
    }
    return self;
}

#pragma mark Session

- (void)configureAudioSession {
    // setCategory / setPreferredIOBufferDuration are synchronous calls into
    // mediaserverd and can block; run them off the main thread (Thread Performance
    // Checker flags them otherwise). Serialized on sessionQueue so the category is
    // set before the first activateSession.
    dispatch_async(self.sessionQueue, ^{
        NSError *error = nil;
        AVAudioSession *session = AVAudioSession.sharedInstance;
        // .playback keeps audio alive when backgrounded / screen locked (paired with
        // the "audio" UIBackgroundMode in Info.plist). Honors the silent switch the
        // way a music player should (it does not duck for it).
        if (![session setCategory:AVAudioSessionCategoryPlayback mode:AVAudioSessionModeDefault options:0 error:&error])
            NSLog(@"ISHAudio: setCategory failed: %@", error);
        // Larger hardware I/O buffer: playback doesn't need low latency, and a bigger
        // buffer gives the audio render thread more slack before it underruns.
        [session setPreferredIOBufferDuration:0.046 error:NULL];
    });
}

- (void)observeSessionNotifications {
    NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
    [center addObserver:self selector:@selector(handleInterruption:)
                   name:AVAudioSessionInterruptionNotification object:nil];
    [center addObserver:self selector:@selector(handleRouteChange:)
                   name:AVAudioSessionRouteChangeNotification object:nil];
}

- (void)handleInterruption:(NSNotification *)note {
    NSInteger type = [note.userInfo[AVAudioSessionInterruptionTypeKey] integerValue];
    if (type == AVAudioSessionInterruptionTypeBegan) {
        self.wasPlayingBeforeInterruption = (self.state == ISHAudioPlaybackStatePlaying);
        if (self.wasPlayingBeforeInterruption)
            [self pause];
    } else if (type == AVAudioSessionInterruptionTypeEnded) {
        NSInteger options = [note.userInfo[AVAudioSessionInterruptionOptionKey] integerValue];
        if ((options & AVAudioSessionInterruptionOptionShouldResume) && self.wasPlayingBeforeInterruption)
            [self play];
    }
}

- (void)handleRouteChange:(NSNotification *)note {
    NSInteger reason = [note.userInfo[AVAudioSessionRouteChangeReasonKey] integerValue];
    // Headphones unplugged etc. — pause, matching iOS music apps.
    if (reason == AVAudioSessionRouteChangeReasonOldDeviceUnavailable && self.state == ISHAudioPlaybackStatePlaying)
        [self pause];
}

- (BOOL)activateSession {
    NSError *error = nil;
    if (![AVAudioSession.sharedInstance setActive:YES error:&error]) {
        NSLog(@"ISHAudio: setActive failed: %@", error);
        return NO;
    }
    return YES;
}

#pragma mark Queue management

- (NSArray<ISHAudioTrack *> *)queue { return [self.mutableQueue copy]; }

- (ISHAudioTrack *)currentTrack {
    if (self.currentIndex < 0 || (NSUInteger)self.currentIndex >= self.mutableQueue.count)
        return nil;
    return self.mutableQueue[self.currentIndex];
}

- (void)setQueue:(NSArray<ISHAudioTrack *> *)tracks startIndex:(NSInteger)startIndex {
    self.mutableQueue = [tracks mutableCopy] ?: [NSMutableArray array];
    [self.shuffleHistory removeAllObjects];
    [self postNotification:ISHAudioPlayerQueueDidChangeNotification];
    if (startIndex != NSNotFound && startIndex >= 0 && (NSUInteger)startIndex < self.mutableQueue.count)
        [self playTrackAtIndex:startIndex];
    else {
        self.currentIndex = self.mutableQueue.count > 0 ? 0 : -1;
        [self postNotification:ISHAudioPlayerTrackDidChangeNotification];
    }
}

- (void)enqueueTracks:(NSArray<ISHAudioTrack *> *)tracks {
    if (tracks.count == 0) return;
    [self.mutableQueue addObjectsFromArray:tracks];
    if (self.currentIndex < 0) self.currentIndex = 0;
    [self postNotification:ISHAudioPlayerQueueDidChangeNotification];
}

- (void)removeTrackAtIndex:(NSInteger)index {
    if (index < 0 || (NSUInteger)index >= self.mutableQueue.count) return;
    BOOL removingCurrent = (index == self.currentIndex);
    [self.mutableQueue removeObjectAtIndex:index];
    if (index < self.currentIndex)
        self.currentIndex--;
    else if (removingCurrent) {
        if ((NSUInteger)self.currentIndex >= self.mutableQueue.count)
            self.currentIndex = self.mutableQueue.count - 1;
        if (self.currentIndex >= 0 && self.state == ISHAudioPlaybackStatePlaying)
            [self playTrackAtIndex:self.currentIndex];
        else
            [self stop];
    }
    [self postNotification:ISHAudioPlayerQueueDidChangeNotification];
}

- (void)moveTrackAtIndex:(NSInteger)from toIndex:(NSInteger)to {
    if (from < 0 || (NSUInteger)from >= self.mutableQueue.count) return;
    if (to < 0 || (NSUInteger)to >= self.mutableQueue.count) return;
    ISHAudioTrack *track = self.mutableQueue[from];
    [self.mutableQueue removeObjectAtIndex:from];
    [self.mutableQueue insertObject:track atIndex:to];
    if (from == self.currentIndex) self.currentIndex = to;
    else if (from < self.currentIndex && to >= self.currentIndex) self.currentIndex--;
    else if (from > self.currentIndex && to <= self.currentIndex) self.currentIndex++;
    [self postNotification:ISHAudioPlayerQueueDidChangeNotification];
}

- (void)clearQueue {
    [self stop];
    [self.mutableQueue removeAllObjects];
    self.currentIndex = -1;
    [self postNotification:ISHAudioPlayerQueueDidChangeNotification];
    [self postNotification:ISHAudioPlayerTrackDidChangeNotification];
}

#pragma mark Transport

- (void)playTrackAtIndex:(NSInteger)index {
    if (index < 0 || (NSUInteger)index >= self.mutableQueue.count) return;
    self.currentIndex = index;
    [self postNotification:ISHAudioPlayerTrackDidChangeNotification];
    [self loadAndPlayCurrentTrackFromTime:0 autoPlay:YES];
}

- (void)play {
    if (!NSThread.isMainThread) { dispatch_async(dispatch_get_main_queue(), ^{ [self play]; }); return; }
    if (self.state == ISHAudioPlaybackStatePlaying) return;
    if (self.currentTrack == nil) {
        if (self.mutableQueue.count > 0) [self playTrackAtIndex:MAX(self.currentIndex, 0)];
        return;
    }
    if (self.decoder == nil) {
        // Resumed from a cold/persisted state — reload at the paused position.
        [self loadAndPlayCurrentTrackFromTime:self.pausedTime autoPlay:YES];
        return;
    }
    // Set state optimistically so a pause() that races the (async) activation still
    // takes effect, then do the blocking AVAudioSession activation + engine start
    // off the main thread to avoid the UI-hang risk.
    [self setState:ISHAudioPlaybackStatePlaying];
    dispatch_async(self.sessionQueue, ^{
        BOOL ok = [self activateSession];
        if (ok && !self.avEngine.isRunning) {
            NSError *error = nil;
            ok = [self.avEngine startAndReturnError:&error];
            if (!ok) NSLog(@"ISHAudio: engine start failed: %@", error);
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            if (!ok) {
                if (self.state == ISHAudioPlaybackStatePlaying) [self setState:ISHAudioPlaybackStatePaused];
                return;
            }
            if (self.state != ISHAudioPlaybackStatePlaying) return;  // paused while activating
            [self.playerNode play];
            [self updateNowPlayingInfo];
        });
    });
}

- (void)pause {
    if (!NSThread.isMainThread) { dispatch_async(dispatch_get_main_queue(), ^{ [self pause]; }); return; }
    if (self.state != ISHAudioPlaybackStatePlaying) return;
    self.pausedTime = self.currentTime;
    [self.playerNode pause];
    [self setState:ISHAudioPlaybackStatePaused];
    [self updateNowPlayingInfo];
}

- (void)togglePlayPause {
    if (self.state == ISHAudioPlaybackStatePlaying) [self pause]; else [self play];
}

- (void)stop {
    self.generation++;
    [self.playerNode stop];
    [self teardownDecoder];
    self.inFlightFrames = 0;
    self.decoderAtEnd = NO;
    self.pausedTime = 0;
    self.seekBaseTime = 0;
    [self setState:ISHAudioPlaybackStateStopped];
    [self updateNowPlayingInfo];
}

- (void)next {
    NSInteger nextIndex = [self indexAfterCurrentAdvancing:YES];
    if (nextIndex < 0) { [self stop]; return; }
    [self playTrackAtIndex:nextIndex];
}

- (void)previous {
    // Restart current track if we're more than 3s in (standard music-player behavior).
    if (self.currentTime > 3.0) { [self seekToTime:0]; return; }
    NSInteger prevIndex = [self indexAfterCurrentAdvancing:NO];
    if (prevIndex < 0) { [self seekToTime:0]; return; }
    [self playTrackAtIndex:prevIndex];
}

- (void)seekToTime:(NSTimeInterval)time {
    if (self.decoder == nil) { self.pausedTime = time; return; }
    if (self.trackDuration > 0) time = MAX(0, MIN(time, self.trackDuration));
    BOOL wasPlaying = (self.state == ISHAudioPlaybackStatePlaying);
    self.generation++;
    [self.playerNode stop];
    self.inFlightFrames = 0;
    self.decoderAtEnd = NO;
    self.seekBaseTime = time;
    self.pausedTime = time;
    AVAudioFramePosition targetFrame = (AVAudioFramePosition)llround(time * self.currentNodeSampleRate);
    uint64_t gen = self.generation;
    dispatch_async(self.decodeQueue, ^{
        if (gen != self.generation) return;
        [self.decoder seekToFrame:targetFrame];
        [self fillBuffersForGeneration:gen];
        dispatch_async(dispatch_get_main_queue(), ^{
            if (gen != self.generation) return;
            if (wasPlaying) {
                if (!self.avEngine.isRunning) [self.avEngine startAndReturnError:nil];
                [self.playerNode play];
            }
            [self updateNowPlayingInfo];
        });
    });
}

- (NSInteger)indexAfterCurrentAdvancing:(BOOL)forward {
    NSInteger count = (NSInteger)self.mutableQueue.count;
    if (count == 0) return -1;
    if (self.repeatMode == ISHAudioRepeatModeOne) return self.currentIndex;

    if (self.shuffleEnabled) {
        if (forward) {
            if (count == 1) return self.repeatMode == ISHAudioRepeatModeOff ? -1 : 0;
            [self.shuffleHistory addObject:@(self.currentIndex)];
            NSInteger pick;
            do { pick = arc4random_uniform((uint32_t)count); } while (pick == self.currentIndex && count > 1);
            return pick;
        } else {
            if (self.shuffleHistory.count > 0) {
                NSInteger prev = self.shuffleHistory.lastObject.integerValue;
                [self.shuffleHistory removeLastObject];
                return prev;
            }
            return -1;
        }
    }

    NSInteger next = self.currentIndex + (forward ? 1 : -1);
    if (next >= count) return self.repeatMode == ISHAudioRepeatModeAll ? 0 : -1;
    if (next < 0) return self.repeatMode == ISHAudioRepeatModeAll ? count - 1 : -1;
    return next;
}

#pragma mark Decoding pipeline

- (void)loadAndPlayCurrentTrackFromTime:(double)startTime autoPlay:(BOOL)autoPlay {
    ISHAudioTrack *track = self.currentTrack;
    if (track == nil) return;

    self.generation++;
    uint64_t gen = self.generation;
    [self.playerNode stop];
    [self teardownDecoder];
    self.inFlightFrames = 0;
    self.decoderAtEnd = NO;
    self.seekBaseTime = startTime;
    self.pausedTime = startTime;
    [self setState:ISHAudioPlaybackStateLoading];

    dispatch_async(self.decodeQueue, ^{
        if (gen != self.generation) return;

        // Resolve the guest path to a host-readable URL (host-direct for
        // /AOK/persist, VFS extraction otherwise) off the main thread.
        NSURL *url = track.resolvedFileURL;
        if (url == nil) {
            NSError *resolveError = nil;
            url = [ISHAudioLibrary.sharedLibrary resolvePlayableURLForGuestPath:track.guestPath error:&resolveError];
            if (url == nil) {
                NSLog(@"ISHAudio: cannot resolve %@: %@", track.guestPath, resolveError);
                dispatch_async(dispatch_get_main_queue(), ^{ if (gen == self.generation) [self handleLoadFailure]; });
                return;
            }
            track.resolvedFileURL = url;
        }

        NSError *decodeError = nil;
        id<ISHPCMDecoder> decoder = ISHCreatePCMDecoderForFileURL(url, &decodeError);
        if (decoder == nil) {
            NSLog(@"ISHAudio: cannot decode %@: %@", url.lastPathComponent, decodeError);
            dispatch_async(dispatch_get_main_queue(), ^{ if (gen == self.generation) [self handleLoadFailure]; });
            return;
        }

        double sampleRate = decoder.sampleRate;
        double duration = sampleRate > 0 ? (double)decoder.frameLength / sampleRate : 0;
        [self applyMetadata:decoder.metadata toTrack:track duration:duration];

        if (startTime > 0)
            [decoder seekToFrame:(AVAudioFramePosition)llround(startTime * sampleRate)];

        dispatch_async(dispatch_get_main_queue(), ^{
            if (gen != self.generation) { [decoder close]; return; }
            self.decoder = decoder;
            self.loadFailureStreak = 0;
            self.trackDuration = duration;
            self.currentNodeSampleRate = sampleRate;
            // Reconnect the player node to this track's processing format; the
            // mixer resamples to the output device.
            [self.avEngine connect:self.playerNode to:self.avEngine.mainMixerNode format:decoder.processingFormat];
            [self postNotification:ISHAudioPlayerTrackDidChangeNotification];

            dispatch_async(self.decodeQueue, ^{
                if (gen != self.generation) return;
                [self fillBuffersForGeneration:gen];
                dispatch_async(dispatch_get_main_queue(), ^{
                    if (gen != self.generation) return;
                    if (autoPlay) [self play];
                    else { self.pausedTime = startTime; [self setState:ISHAudioPlaybackStatePaused]; }
                    [self updateNowPlayingInfo];
                });
            });
        });
    });
}

// Runs on the serial decode queue. Tops up the player node so ~kInFlightSeconds
// of audio is always scheduled ahead of the play head.
- (void)fillBuffersForGeneration:(uint64_t)gen {
    if (gen != self.generation || self.decoder == nil) return;
    AVAudioFramePosition target = (AVAudioFramePosition)(kInFlightSeconds * self.currentNodeSampleRate);

    while (!self.decoderAtEnd && self.inFlightFrames < target) {
        if (gen != self.generation) return;
        BOOL atEnd = NO;
        NSError *error = nil;
        AVAudioPCMBuffer *buffer = [self.decoder readBufferWithFrameCapacity:kDecodeChunkFrames atEnd:&atEnd error:&error];
        if (buffer == nil) { self.decoderAtEnd = YES; break; }
        if (buffer.frameLength == 0) { if (atEnd) self.decoderAtEnd = YES; if (atEnd) break; else continue; }

        AVAudioFrameCount frames = buffer.frameLength;
        self.inFlightFrames += frames;
        if (atEnd) self.decoderAtEnd = YES;

        __weak typeof(self) weakSelf = self;
        [self.playerNode scheduleBuffer:buffer completionHandler:^{
            // Fires when the buffer has been rendered. Bounce to the decode queue
            // so all bookkeeping stays serialized with fill passes.
            dispatch_async(weakSelf.decodeQueue, ^{
                typeof(self) strongSelf = weakSelf;
                if (strongSelf == nil || gen != strongSelf.generation) return;
                strongSelf.inFlightFrames -= frames;
                if (strongSelf.decoderAtEnd && strongSelf.inFlightFrames <= 0) {
                    dispatch_async(dispatch_get_main_queue(), ^{
                        if (gen == strongSelf.generation) [strongSelf handleTrackEnded];
                    });
                } else {
                    [strongSelf fillBuffersForGeneration:gen];
                }
            });
        }];
    }
}

- (void)handleTrackEnded {
    if (self.repeatMode == ISHAudioRepeatModeOne) { [self loadAndPlayCurrentTrackFromTime:0 autoPlay:YES]; return; }
    [self next];
}

// Auto-advance past an undecodable track, but stop once we've failed on every
// track in the queue (so repeat-all over an all-broken queue can't busy-loop).
- (void)handleLoadFailure {
    self.loadFailureStreak++;
    if (self.mutableQueue.count == 0 || self.loadFailureStreak >= (NSInteger)self.mutableQueue.count) {
        self.loadFailureStreak = 0;
        [self stop];
        return;
    }
    [self next];
}

- (void)teardownDecoder {
    [self.decoder close];
    self.decoder = nil;
}

#pragma mark Position

- (NSTimeInterval)currentTime {
    if (self.decoder == nil) return self.pausedTime;
    if (self.state != ISHAudioPlaybackStatePlaying) return self.pausedTime;
    AVAudioTime *nodeTime = self.playerNode.lastRenderTime;
    if (nodeTime == nil || !nodeTime.isSampleTimeValid) return self.pausedTime;
    AVAudioTime *playerTime = [self.playerNode playerTimeForNodeTime:nodeTime];
    if (playerTime == nil || !playerTime.isSampleTimeValid) return self.pausedTime;
    return self.seekBaseTime + (double)playerTime.sampleTime / playerTime.sampleRate;
}

- (NSTimeInterval)duration { return self.trackDuration; }

#pragma mark Modes

- (void)setVolume:(float)volume {
    _volume = MAX(0, MIN(1, volume));
    self.avEngine.mainMixerNode.outputVolume = _volume;
    [NSUserDefaults.standardUserDefaults setFloat:_volume forKey:kAudioVolumeKey];
}
- (float)volume { return _volume; }

- (void)setRepeatMode:(ISHAudioRepeatMode)repeatMode {
    _repeatMode = repeatMode;
    [NSUserDefaults.standardUserDefaults setInteger:repeatMode forKey:kAudioRepeatKey];
}
- (ISHAudioRepeatMode)repeatMode { return _repeatMode; }

- (void)setShuffleEnabled:(BOOL)shuffleEnabled {
    _shuffleEnabled = shuffleEnabled;
    [self.shuffleHistory removeAllObjects];
    [NSUserDefaults.standardUserDefaults setBool:shuffleEnabled forKey:kAudioShuffleKey];
}
- (BOOL)shuffleEnabled { return _shuffleEnabled; }

#pragma mark Now Playing / remote controls

- (void)setupRemoteCommands {
    MPRemoteCommandCenter *center = MPRemoteCommandCenter.sharedCommandCenter;
    __weak typeof(self) weakSelf = self;
    [center.playCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(__unused MPRemoteCommandEvent *e) {
        [weakSelf play]; return MPRemoteCommandHandlerStatusSuccess; }];
    [center.pauseCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(__unused MPRemoteCommandEvent *e) {
        [weakSelf pause]; return MPRemoteCommandHandlerStatusSuccess; }];
    [center.togglePlayPauseCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(__unused MPRemoteCommandEvent *e) {
        [weakSelf togglePlayPause]; return MPRemoteCommandHandlerStatusSuccess; }];
    [center.nextTrackCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(__unused MPRemoteCommandEvent *e) {
        [weakSelf next]; return MPRemoteCommandHandlerStatusSuccess; }];
    [center.previousTrackCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(__unused MPRemoteCommandEvent *e) {
        [weakSelf previous]; return MPRemoteCommandHandlerStatusSuccess; }];
    center.changePlaybackPositionCommand.enabled = YES;
    [center.changePlaybackPositionCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent *e) {
        if ([e isKindOfClass:MPChangePlaybackPositionCommandEvent.class]) {
            [weakSelf seekToTime:((MPChangePlaybackPositionCommandEvent *)e).positionTime];
            return MPRemoteCommandHandlerStatusSuccess;
        }
        return MPRemoteCommandHandlerStatusCommandFailed;
    }];
}

- (void)updateNowPlayingInfo {
    ISHAudioTrack *track = self.currentTrack;
    MPNowPlayingInfoCenter *center = MPNowPlayingInfoCenter.defaultCenter;
    if (track == nil) { center.nowPlayingInfo = nil; return; }
    NSMutableDictionary *info = [NSMutableDictionary dictionary];
    info[MPMediaItemPropertyTitle] = track.title ?: @"";
    if (track.artist) info[MPMediaItemPropertyArtist] = track.artist;
    if (track.album) info[MPMediaItemPropertyAlbumTitle] = track.album;
    if (self.trackDuration > 0) info[MPMediaItemPropertyPlaybackDuration] = @(self.trackDuration);
    info[MPNowPlayingInfoPropertyElapsedPlaybackTime] = @(self.currentTime);
    info[MPNowPlayingInfoPropertyPlaybackRate] = @(self.state == ISHAudioPlaybackStatePlaying ? 1.0 : 0.0);
    center.nowPlayingInfo = info;
}

#pragma mark Helpers

- (void)applyMetadata:(NSDictionary *)metadata toTrack:(ISHAudioTrack *)track duration:(double)duration {
    if (duration > 0) track.duration = duration;
    if (![metadata isKindOfClass:NSDictionary.class]) return;
    NSString *title = metadata[ISHAudioMetadataTitleKey];
    NSString *artist = metadata[ISHAudioMetadataArtistKey];
    NSString *album = metadata[ISHAudioMetadataAlbumKey];
    if ([title isKindOfClass:NSString.class] && title.length > 0) track.title = title;
    if ([artist isKindOfClass:NSString.class] && artist.length > 0) track.artist = artist;
    if ([album isKindOfClass:NSString.class] && album.length > 0) track.album = album;
}

static atomic_bool ish_audio_playing = false;

bool ISHAudioKeepsAppAlive(void) {
    return atomic_load(&ish_audio_playing);
}

- (void)setState:(ISHAudioPlaybackState)state {
    if (_state == state) return;
    _state = state;
    atomic_store(&ish_audio_playing, state == ISHAudioPlaybackStatePlaying);
    [self postNotification:ISHAudioPlayerStateDidChangeNotification];
}

- (void)postNotification:(NSString *)name {
    if (NSThread.isMainThread)
        [NSNotificationCenter.defaultCenter postNotificationName:name object:self];
    else
        dispatch_async(dispatch_get_main_queue(), ^{
            [NSNotificationCenter.defaultCenter postNotificationName:name object:self];
        });
}

- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:self];
}

@end
