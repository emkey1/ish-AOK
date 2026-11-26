//
//  AudioPCMDecoder_Opus.m
//  AOK-Workstation
//
//  Opus backend via the vendored libopusfile (open-source, public API only).
//  Opus always decodes at 48 kHz. op_read_float yields interleaved float, which
//  we de-interleave into the engine's standard deinterleaved buffer.
//

#import "AudioPCMDecoder_Internal.h"

#if __has_include(<opusfile.h>)
#define ISH_HAVE_OPUS 1
#import <AVFoundation/AVFoundation.h>
#include <opusfile.h>
#elif __has_include(<opus/opusfile.h>)
#define ISH_HAVE_OPUS 1
#import <AVFoundation/AVFoundation.h>
#include <opus/opusfile.h>
#endif

#ifdef ISH_HAVE_OPUS

static const double kOpusSampleRate = 48000.0;  // Opus is always 48 kHz

BOOL ISHAudioOpusAvailable(void) { return YES; }

@interface ISHOpusDecoder : NSObject <ISHPCMDecoder>
@end

@implementation ISHOpusDecoder {
    OggOpusFile *_of;
    AVAudioFormat *_format;
    AVAudioFramePosition _frameLength;
    int _channels;
    NSDictionary *_metadata;
    float *_interleaved;
    AVAudioFrameCount _interleavedCapacity;
}

- (instancetype)initWithURL:(NSURL *)url error:(NSError **)error {
    self = [super init];
    if (self) {
        int err = 0;
        _of = op_open_file(url.fileSystemRepresentation, &err);
        if (_of == NULL) {
            if (error) *error = [NSError errorWithDomain:ISHAudioErrorDomain code:1
                                               userInfo:@{NSLocalizedDescriptionKey: @"Not a readable Opus file"}];
            return nil;
        }
        _channels = op_channel_count(_of, -1);
        if (_channels < 1) {
            if (error) *error = [NSError errorWithDomain:ISHAudioErrorDomain code:2
                                               userInfo:@{NSLocalizedDescriptionKey: @"Invalid Opus stream"}];
            return nil;
        }
        _frameLength = (AVAudioFramePosition)op_pcm_total(_of, -1);
        _format = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:kOpusSampleRate channels:_channels];
        _metadata = [self readTags];
    }
    return self;
}

- (AVAudioFormat *)processingFormat { return _format; }
- (AVAudioFramePosition)frameLength { return _frameLength; }
- (double)sampleRate { return kOpusSampleRate; }
- (NSDictionary<NSString *, id> *)metadata { return _metadata; }

- (AVAudioPCMBuffer *)readBufferWithFrameCapacity:(AVAudioFrameCount)frames
                                            atEnd:(BOOL *)atEnd
                                            error:(NSError **)error {
    if (atEnd) *atEnd = NO;
    AVAudioPCMBuffer *buffer = [[AVAudioPCMBuffer alloc] initWithPCMFormat:_format frameCapacity:frames];
    if (buffer == nil) return nil;

    if (_interleaved == NULL || _interleavedCapacity < frames) {
        free(_interleaved);
        _interleaved = malloc((size_t)frames * _channels * sizeof(float));
        _interleavedCapacity = frames;
        if (_interleaved == NULL) return nil;
    }

    float *const *out = buffer.floatChannelData;
    AVAudioFrameCount written = 0;
    while (written < frames) {
        int want = (int)(frames - written);
        int n = op_read_float(_of, _interleaved, want * _channels, NULL);
        if (n <= 0) { if (atEnd) *atEnd = YES; break; }  // 0 = EOF, <0 = error (treat as end)
        for (int frame = 0; frame < n; frame++)
            for (int ch = 0; ch < _channels; ch++)
                out[ch][written + frame] = _interleaved[frame * _channels + ch];
        written += (AVAudioFrameCount)n;
    }
    buffer.frameLength = written;
    return buffer;
}

- (BOOL)seekToFrame:(AVAudioFramePosition)frame {
    if (_of == NULL) return NO;
    return op_pcm_seek(_of, frame) == 0;
}

- (void)close {
    if (_of) { op_free(_of); _of = NULL; }
    free(_interleaved); _interleaved = NULL; _interleavedCapacity = 0;
}

- (void)dealloc { [self close]; }

- (NSDictionary *)readTags {
    const OpusTags *tags = op_tags(_of, -1);
    if (tags == NULL) return nil;
    NSMutableDictionary *result = [NSMutableDictionary dictionary];
    void (^pull)(const char *, NSString *) = ^(const char *key, NSString *outKey) {
        const char *v = opus_tags_query(tags, key, 0);
        if (v) result[outKey] = [NSString stringWithUTF8String:v];
    };
    pull("TITLE", ISHAudioMetadataTitleKey);
    pull("ARTIST", ISHAudioMetadataArtistKey);
    pull("ALBUM", ISHAudioMetadataAlbumKey);
    return result.count ? result : nil;
}

@end

id<ISHPCMDecoder> ISHCreateOpusDecoder(NSURL *url, NSError **error) {
    return [[ISHOpusDecoder alloc] initWithURL:url error:error];
}

#else  // codec headers not present

BOOL ISHAudioOpusAvailable(void) { return NO; }

id<ISHPCMDecoder> ISHCreateOpusDecoder(NSURL *url, NSError **error) {
    (void)url;
    if (error) *error = [NSError errorWithDomain:ISHAudioErrorDomain code:-1
                                       userInfo:@{NSLocalizedDescriptionKey: @"Opus support not built"}];
    return nil;
}

#endif
