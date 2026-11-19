//
//  AudioPCMDecoder_Vorbis.m
//  AOK-iSH
//
//  Ogg Vorbis backend via the vendored libvorbisfile (open-source, public API
//  only). Real when deps/audiocodecs-static/audiocodecs.xcframework is built;
//  a nil-returning stub otherwise so the rest of the app still compiles/links.
//

#import "AudioPCMDecoder_Internal.h"

#if __has_include(<vorbis/vorbisfile.h>)
#define ISH_HAVE_VORBIS 1
#import <AVFoundation/AVFoundation.h>
#include <vorbis/vorbisfile.h>

BOOL ISHAudioVorbisAvailable(void) { return YES; }

@interface ISHVorbisDecoder : NSObject <ISHPCMDecoder>
@end

@implementation ISHVorbisDecoder {
    OggVorbis_File _vf;
    BOOL _open;
    AVAudioFormat *_format;
    AVAudioFramePosition _frameLength;
    double _sampleRate;
    int _channels;
    NSDictionary *_metadata;
}

- (instancetype)initWithURL:(NSURL *)url error:(NSError **)error {
    self = [super init];
    if (self) {
        if (ov_fopen(url.fileSystemRepresentation, &_vf) != 0) {
            if (error) *error = [NSError errorWithDomain:ISHAudioErrorDomain code:1
                                               userInfo:@{NSLocalizedDescriptionKey: @"Not a readable Ogg Vorbis file"}];
            return nil;
        }
        _open = YES;
        vorbis_info *vi = ov_info(&_vf, -1);
        if (vi == NULL || vi->channels < 1) {
            if (error) *error = [NSError errorWithDomain:ISHAudioErrorDomain code:2
                                               userInfo:@{NSLocalizedDescriptionKey: @"Invalid Vorbis stream"}];
            return nil;
        }
        _channels = vi->channels;
        _sampleRate = vi->rate;
        _frameLength = (AVAudioFramePosition)ov_pcm_total(&_vf, -1);
        _format = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:_sampleRate channels:_channels];
        _metadata = [self readComments];
    }
    return self;
}

- (AVAudioFormat *)processingFormat { return _format; }
- (AVAudioFramePosition)frameLength { return _frameLength; }
- (double)sampleRate { return _sampleRate; }
- (NSDictionary<NSString *, id> *)metadata { return _metadata; }

- (AVAudioPCMBuffer *)readBufferWithFrameCapacity:(AVAudioFrameCount)frames
                                            atEnd:(BOOL *)atEnd
                                            error:(NSError **)error {
    if (atEnd) *atEnd = NO;
    AVAudioPCMBuffer *buffer = [[AVAudioPCMBuffer alloc] initWithPCMFormat:_format frameCapacity:frames];
    if (buffer == nil) return nil;
    float *const *out = buffer.floatChannelData;
    AVAudioFrameCount written = 0;
    while (written < frames) {
        float **pcm = NULL;
        int bitstream = 0;
        long n = ov_read_float(&_vf, &pcm, (int)(frames - written), &bitstream);
        if (n <= 0) { if (atEnd) *atEnd = YES; break; }  // 0 = EOF, <0 = stream error (treat as end)
        for (int ch = 0; ch < _channels; ch++)
            memcpy(out[ch] + written, pcm[ch], (size_t)n * sizeof(float));
        written += (AVAudioFrameCount)n;
    }
    buffer.frameLength = written;
    return buffer;
}

- (BOOL)seekToFrame:(AVAudioFramePosition)frame {
    if (!_open) return NO;
    return ov_pcm_seek(&_vf, frame) == 0;
}

- (void)close {
    if (_open) { ov_clear(&_vf); _open = NO; }
}

- (void)dealloc { [self close]; }

- (NSDictionary *)readComments {
    vorbis_comment *vc = ov_comment(&_vf, -1);
    if (vc == NULL) return nil;
    NSMutableDictionary *result = [NSMutableDictionary dictionary];
    for (int i = 0; i < vc->comments; i++) {
        NSString *entry = [NSString stringWithUTF8String:vc->user_comments[i]];
        NSRange eq = [entry rangeOfString:@"="];
        if (eq.location == NSNotFound) continue;
        NSString *key = [entry substringToIndex:eq.location].uppercaseString;
        NSString *value = [entry substringFromIndex:eq.location + 1];
        if ([key isEqualToString:@"TITLE"]) result[ISHAudioMetadataTitleKey] = value;
        else if ([key isEqualToString:@"ARTIST"]) result[ISHAudioMetadataArtistKey] = value;
        else if ([key isEqualToString:@"ALBUM"]) result[ISHAudioMetadataAlbumKey] = value;
    }
    return result.count ? result : nil;
}

@end

id<ISHPCMDecoder> ISHCreateVorbisDecoder(NSURL *url, NSError **error) {
    return [[ISHVorbisDecoder alloc] initWithURL:url error:error];
}

#else  // codec headers not present

BOOL ISHAudioVorbisAvailable(void) { return NO; }

id<ISHPCMDecoder> ISHCreateVorbisDecoder(NSURL *url, NSError **error) {
    (void)url;
    if (error) *error = [NSError errorWithDomain:ISHAudioErrorDomain code:-1
                                       userInfo:@{NSLocalizedDescriptionKey: @"Ogg Vorbis support not built"}];
    return nil;
}

#endif
