//
//  AudioPCMDecoder_AVF.m
//  AOK-iSH
//
//  Native-format backend: MP3/AAC/M4A/ALAC/WAV/AIFF/CAF/FLAC via the public
//  AVAudioFile API. AVAudioFile's processingFormat is already canonical 32-bit
//  float deinterleaved PCM at the source rate — exactly the engine's contract.
//

#import "AudioPCMDecoder_Internal.h"
#import <AVFoundation/AVFoundation.h>

@interface ISHAVFoundationDecoder : NSObject <ISHPCMDecoder>
@end

@implementation ISHAVFoundationDecoder {
    AVAudioFile *_file;
    NSDictionary *_metadata;
}

- (instancetype)initWithURL:(NSURL *)url error:(NSError **)error {
    self = [super init];
    if (self) {
        _file = [[AVAudioFile alloc] initForReading:url
                                       commonFormat:AVAudioPCMFormatFloat32
                                        interleaved:NO
                                              error:error];
        if (_file == nil) return nil;
        _metadata = [[self class] metadataForURL:url];
    }
    return self;
}

- (AVAudioFormat *)processingFormat { return _file.processingFormat; }
- (AVAudioFramePosition)frameLength { return _file.length; }
- (double)sampleRate { return _file.processingFormat.sampleRate; }
- (NSDictionary<NSString *, id> *)metadata { return _metadata; }

- (AVAudioPCMBuffer *)readBufferWithFrameCapacity:(AVAudioFrameCount)frames
                                            atEnd:(BOOL *)atEnd
                                            error:(NSError **)error {
    if (atEnd) *atEnd = NO;
    AVAudioPCMBuffer *buffer = [[AVAudioPCMBuffer alloc] initWithPCMFormat:_file.processingFormat
                                                            frameCapacity:frames];
    if (buffer == nil) return nil;
    if (![_file readIntoBuffer:buffer frameCount:frames error:error])
        return nil;
    if (atEnd && (buffer.frameLength < frames || _file.framePosition >= _file.length))
        *atEnd = YES;
    return buffer;
}

- (BOOL)seekToFrame:(AVAudioFramePosition)frame {
    if (frame < 0) frame = 0;
    if (frame > _file.length) frame = _file.length;
    @try { _file.framePosition = frame; }
    @catch (__unused NSException *e) { return NO; }
    return YES;
}

- (void)close { _file = nil; }

+ (NSDictionary *)metadataForURL:(NSURL *)url {
    AVURLAsset *asset = [AVURLAsset URLAssetWithURL:url options:nil];
    NSMutableDictionary *result = [NSMutableDictionary dictionary];
    NSString *(^stringFor)(NSString *) = ^NSString *(NSString *commonKey) {
        NSArray<AVMetadataItem *> *items = [AVMetadataItem metadataItemsFromArray:asset.commonMetadata
                                                                          withKey:commonKey
                                                                         keySpace:AVMetadataKeySpaceCommon];
        return items.firstObject.stringValue;
    };
    NSString *title = stringFor(AVMetadataCommonKeyTitle);
    NSString *artist = stringFor(AVMetadataCommonKeyArtist);
    NSString *album = stringFor(AVMetadataCommonKeyAlbumName);
    if (title.length) result[ISHAudioMetadataTitleKey] = title;
    if (artist.length) result[ISHAudioMetadataArtistKey] = artist;
    if (album.length) result[ISHAudioMetadataAlbumKey] = album;
    NSArray<AVMetadataItem *> *art = [AVMetadataItem metadataItemsFromArray:asset.commonMetadata
                                                                    withKey:AVMetadataCommonKeyArtwork
                                                                   keySpace:AVMetadataKeySpaceCommon];
    if ([art.firstObject.dataValue isKindOfClass:NSData.class])
        result[ISHAudioMetadataArtworkKey] = art.firstObject.dataValue;
    return result.count ? result : nil;
}

@end

id<ISHPCMDecoder> ISHCreateAVFoundationDecoder(NSURL *url, NSError **error) {
    return [[ISHAVFoundationDecoder alloc] initWithURL:url error:error];
}
