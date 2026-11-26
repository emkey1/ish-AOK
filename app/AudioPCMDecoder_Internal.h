//
//  AudioPCMDecoder_Internal.h
//  AOK-Workstation
//
//  Backend constructors used by the factory in AudioPCMDecoder.m. Each backend
//  always exports its constructor symbol so the app links cleanly; the Ogg/Opus
//  ones return nil (with an "unsupported" error) when their vendored decoder
//  headers aren't present, and become real once deps/audiocodecs-static is built.
//

#import "AudioPCMDecoder.h"

NS_ASSUME_NONNULL_BEGIN

extern NSString *const ISHAudioErrorDomain;

// Native formats via public AVFoundation (AVAudioFile): MP3/AAC/M4A/ALAC/WAV/AIFF/CAF/FLAC.
id<ISHPCMDecoder> _Nullable ISHCreateAVFoundationDecoder(NSURL *url, NSError **error);

// Ogg Vorbis / Opus via the vendored open-source decoders. Available only when
// deps/audiocodecs-static/audiocodecs.xcframework has been built; otherwise nil.
id<ISHPCMDecoder> _Nullable ISHCreateVorbisDecoder(NSURL *url, NSError **error);
id<ISHPCMDecoder> _Nullable ISHCreateOpusDecoder(NSURL *url, NSError **error);

// YES if the Ogg/Opus backends were compiled in (codec xcframework present).
BOOL ISHAudioVorbisAvailable(void);
BOOL ISHAudioOpusAvailable(void);

NS_ASSUME_NONNULL_END
