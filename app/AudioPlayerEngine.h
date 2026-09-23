//
//  AudioPlayerEngine.h
//  iSH-AOK
//
//  Native music playback for the Workspace audio player applet. Built on
//  AVAudioEngine + AVAudioPlayerNode (public AVFoundation), fed PCM by an
//  ISHPCMDecoder so it plays every supported format identically. Owns the
//  AVAudioSession (.playback, so audio continues when iSH-AOK is backgrounded or
//  the device is locked) and the lock-screen / Control Center integration via
//  MPNowPlayingInfoCenter + MPRemoteCommandCenter.
//
//  Singleton: there is one audio output for the app. The applet view controller
//  is just a remote control + observer; closing its window does not stop playback.
//

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

typedef NS_ENUM(NSInteger, ISHAudioPlaybackState) {
    ISHAudioPlaybackStateStopped = 0,
    ISHAudioPlaybackStateLoading,   // decoding/priming the current track
    ISHAudioPlaybackStatePlaying,
    ISHAudioPlaybackStatePaused,
};

typedef NS_ENUM(NSInteger, ISHAudioRepeatMode) {
    ISHAudioRepeatModeOff = 0,  // stop after the last track
    ISHAudioRepeatModeAll,      // wrap to the first track
    ISHAudioRepeatModeOne,      // loop the current track
};

// One queue entry. Identified by its guest path (e.g. "/AOK/persist/music/x.mp3"
// or "/root/music/y.ogg"); the host-readable URL and tags are filled lazily when
// the track is first loaded for playback (see AudioLibrary).
@interface ISHAudioTrack : NSObject
- (instancetype)initWithGuestPath:(NSString *)guestPath;
@property (nonatomic, copy, readonly) NSString *guestPath;
@property (nonatomic, copy) NSString *title;        // defaults to the file's basename
@property (nonatomic, copy, nullable) NSString *artist;
@property (nonatomic, copy, nullable) NSString *album;
@property (nonatomic, assign) NSTimeInterval duration;  // 0 until known
// Host URL the decoder reads from. For /AOK/persist/* this is the real backing
// file; for fakefs paths it's a temp extraction. Resolved on demand.
@property (nonatomic, strong, nullable) NSURL *resolvedFileURL;
// Plain-dictionary form for playlist persistence (JSON in /AOK/persist).
- (NSDictionary *)dictionaryRepresentation;
+ (nullable instancetype)trackWithDictionary:(NSDictionary *)dict;
@end

// Posted on the main thread. State/track/queue carry no userInfo; observers read
// the engine's properties. currentTime is NOT notified (it changes continuously)
// — the applet polls it via a display timer while visible.
extern NSString *const ISHAudioPlayerStateDidChangeNotification;
extern NSString *const ISHAudioPlayerTrackDidChangeNotification;   // currentTrack changed
extern NSString *const ISHAudioPlayerQueueDidChangeNotification;   // queue contents changed

@interface ISHAudioPlayerEngine : NSObject

+ (instancetype)sharedEngine;

// --- Queue ---
@property (nonatomic, copy, readonly) NSArray<ISHAudioTrack *> *queue;
@property (nonatomic, assign, readonly) NSInteger currentIndex;     // -1 when stopped/empty
@property (nonatomic, strong, readonly, nullable) ISHAudioTrack *currentTrack;

// Replace the queue and (optionally) begin playing at startIndex. Pass
// NSNotFound to load the queue without auto-playing.
- (void)setQueue:(NSArray<ISHAudioTrack *> *)tracks startIndex:(NSInteger)startIndex;
// Append without disturbing current playback.
- (void)enqueueTracks:(NSArray<ISHAudioTrack *> *)tracks;
- (void)removeTrackAtIndex:(NSInteger)index;
- (void)moveTrackAtIndex:(NSInteger)from toIndex:(NSInteger)to;
- (void)clearQueue;

// --- Transport ---
@property (nonatomic, assign, readonly) ISHAudioPlaybackState state;
- (void)playTrackAtIndex:(NSInteger)index;
- (void)play;
- (void)pause;
- (void)togglePlayPause;
- (void)stop;            // halts and resets to the start of the current track
- (void)next;            // honors shuffle/repeat
- (void)previous;        // restarts current if >3s in, else goes to prior track
- (void)seekToTime:(NSTimeInterval)time;

// --- Position (polled; not notified) ---
@property (nonatomic, assign, readonly) NSTimeInterval currentTime;
@property (nonatomic, assign, readonly) NSTimeInterval duration;

// --- Modes ---
@property (nonatomic, assign) ISHAudioRepeatMode repeatMode;
@property (nonatomic, assign) BOOL shuffleEnabled;
@property (nonatomic, assign) float volume;  // 0.0–1.0, persisted

@end

// True while the engine is playing, which is what keeps iSH-AOK running in the
// background (the audio background mode). The suspension guard in AppDelegate.m
// uses it, like ISHLocationKeepsAppAlive, to tell "still running" from "about to
// be suspended". Callable from any thread, and does not create the engine.
bool ISHAudioKeepsAppAlive(void);

NS_ASSUME_NONNULL_END
