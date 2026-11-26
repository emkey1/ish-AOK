//
//  AudioLibrary.h
//  AOK-Workstation
//
//  Bridges the audio player to the filesystem. Two sources, per the design:
//
//    1. The default music folder /AOK/persist/music — a realfs mount backed by
//       the AppGroup container, so it survives root switches and its files are
//       directly host-readable (AVFoundation opens them with no copy).
//
//    2. Any other guest path the user points at (e.g. /root/music inside an
//       Alpine root). Those live in the per-root SQLite fakefs and are NOT host
//       files, so they're read through the emulated VFS (generic_open) and
//       extracted to a temp file the decoder can open.
//
//  Also owns playlist persistence (JSON files under /AOK/persist/playlists).
//

#import <Foundation/Foundation.h>
#import "AudioPlayerEngine.h"

NS_ASSUME_NONNULL_BEGIN

@interface ISHAudioLibrary : NSObject

+ (instancetype)sharedLibrary;

// Guest path "/AOK/persist/music" and its host-backed URL (created if missing).
@property (nonatomic, copy, readonly) NSString *musicGuestPath;
@property (nonatomic, strong, readonly, nullable) NSURL *musicDirectoryURL;

// Scan the default music folder (recursively) for supported audio files.
- (NSArray<ISHAudioTrack *> *)scanDefaultMusicDirectory;

// Scan an arbitrary guest directory through the VFS (works for fakefs roots too).
// Returns tracks with guestPath set; URLs/tags resolve lazily on play.
- (NSArray<ISHAudioTrack *> *)scanGuestDirectoryAtPath:(NSString *)guestPath;

// Resolve a guest path to a host-readable URL the decoder can open. Paths under
// /AOK/persist map straight to the backing host file; anything else is read via
// generic_open and written to a temp file (cached for the session). Returns nil
// (with *error) if the path is missing or unreadable.
- (nullable NSURL *)resolvePlayableURLForGuestPath:(NSString *)guestPath
                                             error:(NSError **)error;

// True if `name` ends in a supported audio extension (case-insensitive).
+ (BOOL)isSupportedAudioFileName:(NSString *)name;

// --- Playlists (persisted as /AOK/persist/playlists/<name>.json) ---
- (NSArray<NSString *> *)playlistNames;
- (NSArray<ISHAudioTrack *> *)tracksForPlaylistNamed:(NSString *)name;
- (void)savePlaylistNamed:(NSString *)name tracks:(NSArray<ISHAudioTrack *> *)tracks;
- (void)deletePlaylistNamed:(NSString *)name;

@end

NS_ASSUME_NONNULL_END
