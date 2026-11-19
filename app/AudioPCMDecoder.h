//
//  AudioPCMDecoder.h
//  AOK-iSH
//
//  Uniform PCM decode interface for the Workspace audio player. Every supported
//  container (MP3/AAC/M4A/ALAC/WAV/AIFF/CAF/FLAC via AVFoundation, plus Ogg
//  Vorbis and Opus via the vendored libvorbisfile/libopusfile) is decoded to
//  linear PCM and handed to AVAudioEngine the same way. This keeps the playback
//  engine format-agnostic: it only ever schedules AVAudioPCMBuffers.
//
//  Public Apple APIs only. The Ogg/Opus backends use the open-source decoders
//  in deps/audiocodecs-static (no private interfaces — App Store safe).
//

#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>

NS_ASSUME_NONNULL_BEGIN

// Metadata keys returned by -metadata (all optional, all NSString unless noted).
extern NSString *const ISHAudioMetadataTitleKey;
extern NSString *const ISHAudioMetadataArtistKey;
extern NSString *const ISHAudioMetadataAlbumKey;
extern NSString *const ISHAudioMetadataArtworkKey;   // NSData (encoded image), optional

// A streaming decoder for a single audio file. Not thread-safe; the playback
// engine owns exactly one decoder for the current track and drives it from its
// own scheduling queue.
@protocol ISHPCMDecoder <NSObject>

// Canonical PCM format the decoder emits: 32-bit float, deinterleaved, at the
// source's native sample rate and channel count. The engine connects its player
// node using this format and lets AVAudioEngine handle sample-rate conversion to
// the output device.
@property (nonatomic, readonly) AVAudioFormat *processingFormat;

// Total length of the decoded stream, in source sample frames, or 0 if unknown
// (e.g. a malformed/streamed Ogg without a length). duration == frameLength / sampleRate.
@property (nonatomic, readonly) AVAudioFramePosition frameLength;
@property (nonatomic, readonly) double sampleRate;

// File tags, if any were parsed at open time. nil if none.
@property (nonatomic, readonly, nullable) NSDictionary<NSString *, id> *metadata;

// Decode the next chunk. Returns a buffer of up to `frames` frames in
// processingFormat, an empty buffer (frameLength 0) with *atEnd = YES at EOF, or
// nil on a hard decode error (*error set). The engine loops calling this until atEnd.
- (nullable AVAudioPCMBuffer *)readBufferWithFrameCapacity:(AVAudioFrameCount)frames
                                                    atEnd:(BOOL *)atEnd
                                                    error:(NSError **)error;

// Seek so the next readBuffer returns audio starting at `frame`. Returns NO if
// the container is not seekable. Safe to call mid-playback (engine stops
// scheduling, seeks, resumes).
- (BOOL)seekToFrame:(AVAudioFramePosition)frame;

// Release the underlying file/handles. Idempotent.
- (void)close;

@end

// Construct the right decoder for a host file URL, dispatched by extension and,
// on ambiguity, by sniffing magic bytes. Returns nil (with *error) for an
// unsupported or unreadable file. The URL must be host-readable — guest fakefs
// paths are resolved to a host/temp URL by AudioLibrary first.
FOUNDATION_EXPORT id<ISHPCMDecoder> _Nullable
ISHCreatePCMDecoderForFileURL(NSURL *fileURL, NSError **error);

// Lowercased extensions the player accepts. Used for directory scans and the
// "is this an audio file" check in the file picker.
FOUNDATION_EXPORT NSSet<NSString *> *ISHAudioSupportedExtensions(void);

NS_ASSUME_NONNULL_END
