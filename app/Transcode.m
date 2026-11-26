//
//  Transcode.m
//  AOK-Workstation
//
//  The AVFoundation half of the `transcode` native program
//  (kernel/native_transcode.c; kernel/native_transcode.h has the split).
//  Decoding and encoding are AVAssetReader and AVAssetWriter, which on iOS run
//  on the media engine: the CPU only moves sample buffers between them.
//
//  Runs on the guest task's thread, synchronously. The job's callbacks are
//  called only from that thread, because they reach the guest through the
//  native libc shim, which knows the calling task by its thread.
//

#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#include <stdatomic.h>
#include "kernel/native_transcode.h"

static void TCSetError(struct transcode_job *job, NSString *what, NSError *error) {
    NSString *text = what;
    if (error != nil) {
        NSString *detail = error.localizedFailureReason ?: error.localizedDescription;
        NSError *under = error.userInfo[NSUnderlyingErrorKey];
        if (under != nil)
            detail = [detail stringByAppendingFormat:@" (%@ %ld)", under.domain, (long) under.code];
        text = [NSString stringWithFormat:@"%@: %@", what, detail];
    }
    strlcpy(job->error, text.UTF8String ?: "failed", sizeof(job->error));
}

// AVAsynchronousKeyValueLoading, waited for. The task's thread may block: it
// is not the main thread, and nothing here needs the main thread.
static BOOL TCLoad(id<AVAsynchronousKeyValueLoading> object, NSArray<NSString *> *keys, NSError **error) {
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    [object loadValuesAsynchronouslyForKeys:keys completionHandler:^{
        dispatch_semaphore_signal(done);
    }];
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    for (NSString *key in keys) {
        if ([object statusOfValueForKey:key error:error] != AVKeyValueStatusLoaded)
            return NO;
    }
    return YES;
}

static BOOL TCIsHDR(CMFormatDescriptionRef format) {
    if (format == NULL)
        return NO;
    CFTypeRef transfer = CMFormatDescriptionGetExtension(format, kCMFormatDescriptionExtension_TransferFunction);
    if (transfer == NULL)
        return NO;
    return CFEqual(transfer, kCMFormatDescriptionTransferFunction_ITU_R_2100_HLG) ||
        CFEqual(transfer, kCMFormatDescriptionTransferFunction_SMPTE_ST_2084_PQ);
}

// The source's colour description, in the writer's terms, or nil.
static NSDictionary *TCColorProperties(CMFormatDescriptionRef format) {
    if (format == NULL)
        return nil;
    id primaries = (__bridge id) CMFormatDescriptionGetExtension(format, kCMFormatDescriptionExtension_ColorPrimaries);
    id transfer = (__bridge id) CMFormatDescriptionGetExtension(format, kCMFormatDescriptionExtension_TransferFunction);
    id matrix = (__bridge id) CMFormatDescriptionGetExtension(format, kCMFormatDescriptionExtension_YCbCrMatrix);
    if (primaries == nil || transfer == nil || matrix == nil)
        return nil;
    return @{
        AVVideoColorPrimariesKey: primaries,
        AVVideoTransferFunctionKey: transfer,
        AVVideoYCbCrMatrixKey: matrix,
    };
}

static NSDictionary *TCRec709(void) {
    return @{
        AVVideoColorPrimariesKey: AVVideoColorPrimaries_ITU_R_709_2,
        AVVideoTransferFunctionKey: AVVideoTransferFunction_ITU_R_709_2,
        AVVideoYCbCrMatrixKey: AVVideoYCbCrMatrix_ITU_R_709_2,
    };
}

// One reader output feeding one writer input, on its own queue.
@interface TCPump : NSObject
@property (nonatomic) AVAssetReaderOutput *output;
@property (nonatomic) AVAssetWriterInput *input;
@property (nonatomic) BOOL isVideo;
@property (nonatomic) BOOL tracksProgress;  // its presentation times are the job's progress
@property (nonatomic) BOOL failed;
@end
@implementation TCPump
@end

static BOOL TCIsAAC(FourCharCode codec) {
    return codec == kAudioFormatMPEG4AAC || codec == kAudioFormatMPEG4AAC_HE ||
        codec == kAudioFormatMPEG4AAC_HE_V2 || codec == kAudioFormatMPEG4AAC_LD ||
        codec == kAudioFormatMPEG4AAC_ELD;
}

// The AAC bitrate the system's encoder will take for this sample rate and
// channel count: the one asked for when it is allowed, else the highest
// allowed below it, else the lowest allowed. What it allows depends on both
// -- 128k is too much for mono at 22.05 kHz, which failed with -12651
// ("encoding parameters are not supported") -- so it is asked rather than
// worked out.
static long TCAACBitrate(double rate, int channels, long wanted) {
    AudioStreamBasicDescription in = {
        .mSampleRate = rate, .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked,
        .mBytesPerPacket = 4 * (UInt32) channels, .mFramesPerPacket = 1,
        .mBytesPerFrame = 4 * (UInt32) channels, .mChannelsPerFrame = (UInt32) channels, .mBitsPerChannel = 32,
    };
    AudioStreamBasicDescription out = {
        .mSampleRate = rate, .mFormatID = kAudioFormatMPEG4AAC,
        .mChannelsPerFrame = (UInt32) channels, .mFramesPerPacket = 1024,
    };
    AudioConverterRef converter = NULL;
    if (AudioConverterNew(&in, &out, &converter) != noErr)
        return wanted;
    UInt32 size = 0;
    AudioValueRange *ranges = NULL;
    if (AudioConverterGetPropertyInfo(converter, kAudioConverterApplicableEncodeBitRates, &size, NULL) == noErr &&
            size >= sizeof(AudioValueRange) && (ranges = malloc(size)) != NULL &&
            AudioConverterGetProperty(converter, kAudioConverterApplicableEncodeBitRates, &size, ranges) != noErr) {
        free(ranges);
        ranges = NULL;
    }
    AudioConverterDispose(converter);
    if (ranges == NULL)
        return wanted;
    long below = 0, lowest = 0;
    BOOL allowed = NO;
    for (size_t i = 0; i < size / sizeof(AudioValueRange); i++) {
        long lo = (long) ranges[i].mMinimum, hi = (long) ranges[i].mMaximum;
        if (wanted >= lo && wanted <= hi)
            allowed = YES;
        if (hi <= wanted && hi > below)
            below = hi;
        if (lowest == 0 || lo < lowest)
            lowest = lo;
    }
    free(ranges);
    if (allowed)
        return wanted;
    return below > 0 ? below : lowest > 0 ? lowest : wanted;
}

static BOOL TCIsSoundOnlyContainer(enum transcode_container container) {
    return container == TRANSCODE_M4A || container == TRANSCODE_CAF ||
        container == TRANSCODE_WAV || container == TRANSCODE_AIFF;
}

// A writer for the job's container, on a file in the temporary directory
// that job->out_host_path names.
static AVAssetWriter *TCMakeWriter(struct transcode_job *job, NSError **error) {
    AVFileType type;
    NSString *ext;
    switch (job->container) {
    case TRANSCODE_MOV: type = AVFileTypeQuickTimeMovie; ext = @"mov"; break;
    case TRANSCODE_M4A: type = AVFileTypeAppleM4A; ext = @"m4a"; break;
    case TRANSCODE_CAF: type = AVFileTypeCoreAudioFormat; ext = @"caf"; break;
    case TRANSCODE_WAV: type = AVFileTypeWAVE; ext = @"wav"; break;
    case TRANSCODE_AIFF: type = AVFileTypeAIFF; ext = @"aif"; break;
    default: type = AVFileTypeMPEG4; ext = @"mp4"; break;
    }
    NSString *outPath = [NSTemporaryDirectory() stringByAppendingPathComponent:
        [NSString stringWithFormat:@"%s%@.%@", TRANSCODE_TEMP_PREFIX, NSUUID.UUID.UUIDString, ext]];
    strlcpy(job->out_host_path, outPath.fileSystemRepresentation, sizeof(job->out_host_path));
    return [AVAssetWriter assetWriterWithURL:[NSURL fileURLWithPath:outPath] fileType:type error:error];
}

// Runs every pump to the end, reporting progress and watching for a cancel,
// then finishes the file. 0 when it is written; 1 when cancelled and -1 with
// job->error set when it failed, both with the file removed.
static int TCPumpAll(struct transcode_job *job, AVAssetReader *reader, AVAssetWriter *writer,
                     NSArray<TCPump *> *pumps, double duration) {
    NSString *outPath = @(job->out_host_path);
    if (![reader startReading]) {
        TCSetError(job, @"cannot read the input", reader.error);
        return -1;
    }
    if (![writer startWriting]) {
        [reader cancelReading];
        TCSetError(job, @"cannot start the encoder", writer.error);
        return -1;
    }
    [writer startSessionAtSourceTime:kCMTimeZero];

    // Progress, as the presentation time of the last sample passed on by the
    // pump that tracks it, in microseconds. Written by its queue, read here.
    _Atomic int64_t *position = calloc(1, sizeof(*position));
    _Atomic bool *cancelled = calloc(1, sizeof(*cancelled));
    if (position == NULL || cancelled == NULL) {
        free(position);
        free(cancelled);
        [reader cancelReading];
        [writer cancelWriting];
        TCSetError(job, @"out of memory", nil);
        return -1;
    }
    dispatch_group_t group = dispatch_group_create();
    for (TCPump *pump in pumps) {
        dispatch_group_enter(group);
        dispatch_queue_t queue = dispatch_queue_create(pump.isVideo ? "transcode.video" : "transcode.audio",
                                                       DISPATCH_QUEUE_SERIAL);
        __block BOOL finished = NO;
        [pump.input requestMediaDataWhenReadyOnQueue:queue usingBlock:^{
            if (finished)
                return;
            while (pump.input.isReadyForMoreMediaData) {
                CMSampleBufferRef buffer = atomic_load(cancelled) ? NULL : [pump.output copyNextSampleBuffer];
                if (buffer == NULL) {
                    [pump.input markAsFinished];
                    finished = YES;
                    dispatch_group_leave(group);
                    return;
                }
                if (pump.tracksProgress) {
                    CMTime t = CMSampleBufferGetPresentationTimeStamp(buffer);
                    if (CMTIME_IS_NUMERIC(t))
                        atomic_store(position, (int64_t) (CMTimeGetSeconds(t) * 1e6));
                }
                BOOL ok = [pump.input appendSampleBuffer:buffer];
                CFRelease(buffer);
                if (!ok) {
                    pump.failed = YES;
                    atomic_store(cancelled, true);
                    [pump.input markAsFinished];
                    finished = YES;
                    dispatch_group_leave(group);
                    return;
                }
            }
        }];
    }

    BOOL userCancelled = NO;
    while (dispatch_group_wait(group, dispatch_time(DISPATCH_TIME_NOW, 500 * NSEC_PER_MSEC)) != 0) {
        if (job->progress != NULL && duration > 0)
            job->progress(atomic_load(position) / 1e6 / duration, job->ctx);
        if (!userCancelled && job->should_cancel != NULL && job->should_cancel(job->ctx)) {
            userCancelled = YES;
            atomic_store(cancelled, true);
            [reader cancelReading];
        }
    }
    BOOL anyFailed = NO;
    for (TCPump *pump in pumps)
        anyFailed |= pump.failed;
    free(position);
    free(cancelled);

    if (userCancelled || anyFailed || reader.status == AVAssetReaderStatusFailed) {
        NSError *readerError = reader.status == AVAssetReaderStatusFailed ? reader.error : nil;
        NSError *writerError = writer.status == AVAssetWriterStatusFailed ? writer.error : nil;
        if (reader.status == AVAssetReaderStatusReading)
            [reader cancelReading];
        [writer cancelWriting];
        [NSFileManager.defaultManager removeItemAtPath:outPath error:nil];
        job->out_host_path[0] = '\0';
        if (userCancelled)
            return 1;
        if (readerError != nil)
            TCSetError(job, @"decoding failed", readerError);
        else
            TCSetError(job, @"encoding failed", writerError);
        return -1;
    }

    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    [writer finishWritingWithCompletionHandler:^{
        dispatch_semaphore_signal(done);
    }];
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    if (writer.status != AVAssetWriterStatusCompleted) {
        TCSetError(job, @"encoding failed", writer.error);
        [NSFileManager.defaultManager removeItemAtPath:outPath error:nil];
        job->out_host_path[0] = '\0';
        return -1;
    }
    if (job->progress != NULL)
        job->progress(1, job->ctx);
    return 0;
}

// The bits per sample worth keeping from a source: 24 for a lossless or PCM
// source deeper than 16 bits (or in floating point), 16 for the rest.
static int TCSourceBits(const AudioStreamBasicDescription *asbd) {
    if (asbd == NULL)
        return 16;
    if (asbd->mFormatID == kAudioFormatLinearPCM)
        return (asbd->mFormatFlags & kAudioFormatFlagIsFloat) || asbd->mBitsPerChannel > 16 ? 24 : 16;
    if (asbd->mFormatID == kAudioFormatAppleLossless || asbd->mFormatID == kAudioFormatFLAC) {
        switch (asbd->mFormatFlags) {
        case kAppleLosslessFormatFlag_20BitSourceData:
        case kAppleLosslessFormatFlag_24BitSourceData:
        case kAppleLosslessFormatFlag_32BitSourceData:
            return 24;
        }
    }
    return 16;
}

static NSString *TCAudioCodecName(FourCharCode codec) {
    switch (codec) {
    case kAudioFormatMPEG4AAC: return @"AAC";
    case kAudioFormatMPEG4AAC_HE: case kAudioFormatMPEG4AAC_HE_V2: return @"HE-AAC";
    case kAudioFormatMPEG4AAC_LD: case kAudioFormatMPEG4AAC_ELD: return @"AAC-LD";
    case kAudioFormatAppleLossless: return @"ALAC";
    case kAudioFormatFLAC: return @"FLAC";
    case kAudioFormatMPEGLayer3: return @"MP3";
    case kAudioFormatLinearPCM: return @"PCM";
    case kAudioFormatOpus: return @"Opus";
    case kAudioFormatAC3: return @"AC-3";
    case kAudioFormatEnhancedAC3: return @"E-AC-3";
    default: return @"audio";
    }
}

// The input's tags in the common key space, which the writer turns into the
// container's own (iTunes atoms in an .m4a): an MP3's ID3 frames cannot be
// written there as they are, and a video's QuickTime keys keep its date and
// place this way.
static NSArray<AVMetadataItem *> *TCCommonMetadata(AVAsset *asset) {
    NSMutableArray<AVMetadataItem *> *items = [NSMutableArray array];
    for (AVMetadataItem *item in asset.commonMetadata) {
        if (item.commonKey == nil || item.value == nil)
            continue;
        AVMutableMetadataItem *copy = [AVMutableMetadataItem metadataItem];
        copy.keySpace = AVMetadataKeySpaceCommon;
        copy.key = item.commonKey;
        copy.value = item.value;
        copy.dataType = item.dataType;
        copy.extendedLanguageTag = item.extendedLanguageTag;
        [items addObject:copy];
    }
    return items;
}

// A sound-only result: an audio container, or an input with no picture.
static int TCRunAudio(struct transcode_job *job, AVURLAsset *asset, AVAssetTrack *audio, BOOL inputHasVideo) {
    NSError *error = nil;
    job->audio_only = true;
    if (!inputHasVideo && (job->codec_set || job->max_lines > 0 || job->video_bitrate > 0)) {
        TCSetError(job, @"the input has no video: -s, -b and -c hevc/h264 are for video", nil);
        return -1;
    }
    if (!inputHasVideo && job->no_audio) {
        TCSetError(job, @"the input has no video: -n would leave nothing", nil);
        return -1;
    }
    if (audio == nil) {
        TCSetError(job, inputHasVideo ? @"the input has no audio track"
                                      : @"the input has no video or audio track", nil);
        return -1;
    }
    if (!TCLoad(audio, @[@"formatDescriptions", @"estimatedDataRate"], &error) ||
            !TCLoad(asset, @[@"commonMetadata"], &error)) {
        TCSetError(job, @"cannot read the audio track", error);
        return -1;
    }
    double duration = CMTimeGetSeconds(asset.duration);
    if (!(duration > 0))
        duration = 0;
    CMFormatDescriptionRef format = (__bridge CMFormatDescriptionRef) audio.formatDescriptions.firstObject;
    const AudioStreamBasicDescription *asbd = format ? CMAudioFormatDescriptionGetStreamBasicDescription(format) : NULL;
    FourCharCode source = format ? CMFormatDescriptionGetMediaSubType(format) : 0;
    enum transcode_container container = job->container;

    // The codec, and whether the source can go in as it is.
    enum transcode_audio_codec codec = job->audio_codec;
    if (container == TRANSCODE_WAV || container == TRANSCODE_AIFF)
        codec = TRANSCODE_AUDIO_PCM;
    if (codec == TRANSCODE_AUDIO_AUTO) {
        BOOL alacFits = container == TRANSCODE_M4A || container == TRANSCODE_CAF || container == TRANSCODE_MOV;
        codec = source == kAudioFormatAppleLossless && alacFits ? TRANSCODE_AUDIO_ALAC : TRANSCODE_AUDIO_AAC;
    }
    BOOL copy = (codec == TRANSCODE_AUDIO_AAC && TCIsAAC(source) && job->audio_bitrate == 0) ||
        (codec == TRANSCODE_AUDIO_ALAC && source == kAudioFormatAppleLossless);
    job->lossless = codec != TRANSCODE_AUDIO_AAC;

    AVAssetReader *reader = [AVAssetReader assetReaderWithAsset:asset error:&error];
    if (reader == nil) {
        TCSetError(job, @"cannot read the input", error);
        return -1;
    }
    AVAssetWriter *writer = TCMakeWriter(job, &error);
    if (writer == nil) {
        TCSetError(job, @"cannot start the encoder", error);
        return -1;
    }
    if (container != TRANSCODE_WAV && container != TRANSCODE_AIFF)
        writer.metadata = TCCommonMetadata(asset);

    AVAssetReaderTrackOutput *audioOut = nil;
    AVAssetWriterInput *audioIn = nil;
    NSString *note;
    if (copy) {
        audioOut = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:audio outputSettings:nil];
        audioIn = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeAudio outputSettings:nil
                                                   sourceFormatHint:format];
        if (![reader canAddOutput:audioOut] || ![writer canAddInput:audioIn])
            copy = NO;
        note = [NSString stringWithFormat:@"%@ %.0f kbit/s, copied", TCAudioCodecName(source),
                audio.estimatedDataRate / 1000];
    }
    if (!copy) {
        // Mono stays mono; anything wider is mixed down to stereo, as a
        // video's audio is.
        int channels = asbd != NULL && asbd->mChannelsPerFrame == 1 ? 1 : 2;
        double sourceRate = asbd != NULL && asbd->mSampleRate > 0 ? asbd->mSampleRate : 44100;
        double rate = sourceRate;
        long bitrate = 0;
        int bits = TCSourceBits(asbd);
        if (codec == TRANSCODE_AUDIO_AAC) {
            // AAC's own rates; above 48 kHz it goes down to the one of the
            // same family.
            static const double aacRates[] = {8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000};
            BOOL valid = NO;
            for (size_t i = 0; i < sizeof(aacRates) / sizeof(aacRates[0]); i++)
                valid |= rate == aacRates[i];
            if (!valid)
                rate = rate > 48000 && fmod(rate, 48000) == 0 ? 48000 : 44100;
            bitrate = TCAACBitrate(rate, channels, job->audio_bitrate > 0 ? job->audio_bitrate : 128000);
        }
        AudioChannelLayout layout = {
            .mChannelLayoutTag = channels == 1 ? kAudioChannelLayoutTag_Mono : kAudioChannelLayoutTag_Stereo,
        };
        NSData *layoutData = [NSData dataWithBytes:&layout length:sizeof(layout)];
        audioOut = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:audio outputSettings:@{
            AVFormatIDKey: @(kAudioFormatLinearPCM),
            AVSampleRateKey: @(rate),
            AVNumberOfChannelsKey: @(channels),
            AVChannelLayoutKey: layoutData,
            AVLinearPCMBitDepthKey: @32,
            AVLinearPCMIsFloatKey: @YES,
            AVLinearPCMIsBigEndianKey: @NO,
            AVLinearPCMIsNonInterleaved: @NO,
        }];
        NSMutableDictionary *settings = [@{
            AVSampleRateKey: @(rate),
            AVNumberOfChannelsKey: @(channels),
            AVChannelLayoutKey: layoutData,
        } mutableCopy];
        NSString *what;
        switch (codec) {
        case TRANSCODE_AUDIO_ALAC:
            settings[AVFormatIDKey] = @(kAudioFormatAppleLossless);
            settings[AVEncoderBitDepthHintKey] = @(bits);
            what = [NSString stringWithFormat:@"ALAC %d-bit", bits];
            break;
        case TRANSCODE_AUDIO_PCM:
            settings[AVFormatIDKey] = @(kAudioFormatLinearPCM);
            settings[AVLinearPCMBitDepthKey] = @(bits);
            settings[AVLinearPCMIsFloatKey] = @NO;
            settings[AVLinearPCMIsBigEndianKey] = @(container == TRANSCODE_AIFF);
            settings[AVLinearPCMIsNonInterleaved] = @NO;
            what = [NSString stringWithFormat:@"PCM %d-bit", bits];
            break;
        default:
            settings[AVFormatIDKey] = @(kAudioFormatMPEG4AAC);
            settings[AVEncoderBitRateKey] = @(bitrate);
            what = [NSString stringWithFormat:@"AAC %ld kbit/s", bitrate / 1000];
            break;
        }
        audioIn = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeAudio outputSettings:settings];
        if (![reader canAddOutput:audioOut] || ![writer canAddInput:audioIn]) {
            TCSetError(job, [NSString stringWithFormat:@"cannot make %@ at %.1f kHz from this audio", what, rate / 1000], nil);
            return -1;
        }
        note = [NSString stringWithFormat:@"%@, %@, %g kHz", what, channels == 1 ? @"mono" : @"stereo", rate / 1000];
        if (TCIsAAC(source) && codec == TRANSCODE_AUDIO_AAC)
            note = [note stringByAppendingString:@", re-encoded"];
    }
    audioOut.alwaysCopiesSampleData = NO;
    audioIn.expectsMediaDataInRealTime = NO;
    [reader addOutput:audioOut];
    [writer addInput:audioIn];
    TCPump *pump = [TCPump new];
    pump.output = audioOut;
    pump.input = audioIn;
    pump.tracksProgress = YES;

    int res = TCPumpAll(job, reader, writer, @[pump], duration);
    if (res != 0)
        return res;
    NSString *summary = inputHasVideo ? [note stringByAppendingString:@", the video's sound only"] : note;
    strlcpy(job->summary, summary.UTF8String, sizeof(job->summary));
    return 0;
}

static int TCRun(struct transcode_job *job) {
    @autoreleasepool {
        NSError *error = nil;
        NSURL *inURL = [NSURL fileURLWithPath:@(job->in_host_path)];
        AVURLAsset *asset = [AVURLAsset URLAssetWithURL:inURL
                                                options:@{AVURLAssetPreferPreciseDurationAndTimingKey: @YES}];
        if (!TCLoad(asset, @[@"tracks", @"duration", @"metadata"], &error)) {
            TCSetError(job, @"cannot read the input as a video or audio file", error);
            return -1;
        }
        AVAssetTrack *video = [asset tracksWithMediaType:AVMediaTypeVideo].firstObject;
        AVAssetTrack *firstAudio = [asset tracksWithMediaType:AVMediaTypeAudio].firstObject;
        if (video == nil || TCIsSoundOnlyContainer(job->container))
            return TCRunAudio(job, asset, firstAudio, video != nil);
        if (job->audio_codec != TRANSCODE_AUDIO_AUTO) {
            TCSetError(job, @"-c aac, alac and pcm make an audio file: name the output .m4a, .caf or .wav", nil);
            return -1;
        }
        AVAssetTrack *audio = job->no_audio ? nil : firstAudio;
        if (!TCLoad(video, @[@"naturalSize", @"preferredTransform", @"nominalFrameRate",
                             @"estimatedDataRate", @"formatDescriptions"], &error)) {
            TCSetError(job, @"cannot read the video track", error);
            return -1;
        }
        if (audio != nil && !TCLoad(audio, @[@"formatDescriptions"], &error)) {
            TCSetError(job, @"cannot read the audio track", error);
            return -1;
        }
        double duration = CMTimeGetSeconds(asset.duration);
        if (!(duration > 0))
            duration = 0;

        CMFormatDescriptionRef format = (__bridge CMFormatDescriptionRef) video.formatDescriptions.firstObject;
        BOOL hevc = job->codec == TRANSCODE_HEVC;
        BOOL hdr = TCIsHDR(format);
        // H.264 has no HDR here, so an HDR source going to H.264 is tone
        // mapped to SDR by a video composition. One going to HEVC stays HDR,
        // in 10 bits with the source's colour description.
        BOOL toneMap = hdr && !hevc;

        CGSize natural = video.naturalSize;
        CGAffineTransform transform = video.preferredTransform;
        AVMutableVideoComposition *composition = nil;
        if (toneMap) {
            if (!TCLoad(asset, @[@"tracks"], &error)) {
                TCSetError(job, @"cannot read the input", error);
                return -1;
            }
            composition = [AVMutableVideoComposition videoCompositionWithPropertiesOfAsset:asset];
            composition.colorPrimaries = AVVideoColorPrimaries_ITU_R_709_2;
            composition.colorTransferFunction = AVVideoTransferFunction_ITU_R_709_2;
            composition.colorYCbCrMatrix = AVVideoYCbCrMatrix_ITU_R_709_2;
            // The composition renders the picture upright, so the writer
            // gets no transform.
            natural = composition.renderSize;
            transform = CGAffineTransformIdentity;
        }
        double width = fabs(natural.width), height = fabs(natural.height);
        if (width < 2 || height < 2) {
            TCSetError(job, @"the video has no picture size", nil);
            return -1;
        }
        double scale = 1;
        double shorter = MIN(width, height);
        if (job->max_lines > 0 && shorter > job->max_lines)
            scale = job->max_lines / shorter;
        long outWidth = lround(width * scale / 2) * 2;
        long outHeight = lround(height * scale / 2) * 2;

        double fps = video.nominalFrameRate > 0 ? video.nominalFrameRate : 30;
        long bitrate = job->video_bitrate;
        BOOL automatic = bitrate == 0;
        if (automatic) {
            // About what the phone's own camera spends per pixel at its
            // efficient settings: HEVC needs roughly two thirds of H.264.
            bitrate = lround(outWidth * outHeight * fps * (hevc ? 0.05 : 0.08) *
                             (job->rate_scale > 0 ? job->rate_scale : 1));
            // Re-encoding cannot add detail, so never spend more than the
            // source did: that would only make the file bigger.
            if (video.estimatedDataRate > 0 && bitrate > video.estimatedDataRate)
                bitrate = lround(video.estimatedDataRate);
            if (bitrate < 100000)
                bitrate = 100000;
        }

        AVAssetReader *reader = [AVAssetReader assetReaderWithAsset:asset error:&error];
        if (reader == nil) {
            TCSetError(job, @"cannot read the input", error);
            return -1;
        }

        BOOL mov = job->container == TRANSCODE_MOV;
        AVAssetWriter *writer = TCMakeWriter(job, &error);
        if (writer == nil) {
            TCSetError(job, @"cannot start the encoder", error);
            return -1;
        }
        writer.shouldOptimizeForNetworkUse = YES;
        // Creation date, location and the rest, so that the result still
        // sorts and maps where the original did.
        writer.metadata = asset.metadata;

        // Video.
        BOOL tenBit = hdr && hevc;
        NSDictionary *pixels = @{
            (id) kCVPixelBufferPixelFormatTypeKey: @(tenBit ? kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange
                                                            : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
        };
        AVAssetReaderOutput *videoOut;
        if (composition != nil) {
            AVAssetReaderVideoCompositionOutput *out =
                [AVAssetReaderVideoCompositionOutput assetReaderVideoCompositionOutputWithVideoTracks:@[video]
                                                                                       videoSettings:pixels];
            out.videoComposition = composition;
            videoOut = out;
        } else {
            videoOut = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:video outputSettings:pixels];
        }
        videoOut.alwaysCopiesSampleData = NO;
        if (![reader canAddOutput:videoOut]) {
            TCSetError(job, @"cannot decode this video", nil);
            return -1;
        }
        [reader addOutput:videoOut];

        NSString *profile;
        if (hevc)
            profile = tenBit ? @"HEVC_Main10_AutoLevel" : @"HEVC_Main_AutoLevel";
        else
            profile = AVVideoProfileLevelH264HighAutoLevel;
        NSMutableDictionary *compression = [@{
            AVVideoAverageBitRateKey: @(bitrate),
            AVVideoExpectedSourceFrameRateKey: @(lround(fps)),
            AVVideoMaxKeyFrameIntervalDurationKey: @2,
            AVVideoProfileLevelKey: profile,
        } mutableCopy];
        NSMutableDictionary *videoSettings = [@{
            AVVideoCodecKey: hevc ? AVVideoCodecTypeHEVC : AVVideoCodecTypeH264,
            AVVideoWidthKey: @(outWidth),
            AVVideoHeightKey: @(outHeight),
            AVVideoScalingModeKey: AVVideoScalingModeResizeAspect,
            AVVideoCompressionPropertiesKey: compression,
        } mutableCopy];
        NSDictionary *color = toneMap ? TCRec709() : TCColorProperties(format);
        if (color != nil)
            videoSettings[AVVideoColorPropertiesKey] = color;
        AVAssetWriterInput *videoIn = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo
                                                                         outputSettings:videoSettings];
        videoIn.expectsMediaDataInRealTime = NO;
        videoIn.transform = transform;
        if (![writer canAddInput:videoIn]) {
            TCSetError(job, [NSString stringWithFormat:@"this device cannot encode %@ at %ldx%ld",
                             hevc ? @"HEVC" : @"H.264", outWidth, outHeight], nil);
            return -1;
        }
        [writer addInput:videoIn];

        TCPump *videoPump = [TCPump new];
        videoPump.output = videoOut;
        videoPump.input = videoIn;
        videoPump.isVideo = YES;
        videoPump.tracksProgress = YES;
        NSMutableArray<TCPump *> *pumps = [NSMutableArray arrayWithObject:videoPump];

        // Audio: copied as it is when the container takes it, otherwise
        // re-encoded as AAC (newer iPhones record spatial audio, which an
        // MPEG-4 file cannot carry as it is). See `copyable` below.
        NSString *audioNote = @"no audio";
        if (audio != nil) {
            CMFormatDescriptionRef audioFormat =
                (__bridge CMFormatDescriptionRef) audio.formatDescriptions.firstObject;
            AVAssetReaderTrackOutput *audioOut = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:audio
                                                                                            outputSettings:nil];
            AVAssetWriterInput *audioIn = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeAudio
                                                                             outputSettings:nil
                                                                           sourceFormatHint:audioFormat];
            // QuickTime carries any audio. MPEG-4 is given only AAC as it
            // is: AVAssetWriter also accepts linear PCM there (a Live
            // Photo's audio), and many players then cannot play the file.
            FourCharCode audioCodec = audioFormat ? CMFormatDescriptionGetMediaSubType(audioFormat) : 0;
            BOOL copyable = mov || TCIsAAC(audioCodec);
            BOOL copied = audioFormat != NULL && copyable &&
                [reader canAddOutput:audioOut] && [writer canAddInput:audioIn];
            if (copied) {
                audioNote = @"audio copied";
            } else {
                double rate = 48000;
                const AudioStreamBasicDescription *asbd =
                    audioFormat ? CMAudioFormatDescriptionGetStreamBasicDescription(audioFormat) : NULL;
                if (asbd != NULL && (asbd->mSampleRate == 44100 || asbd->mSampleRate == 48000))
                    rate = asbd->mSampleRate;
                long audioRate = TCAACBitrate(rate, 2, job->audio_bitrate > 0 ? job->audio_bitrate : 128000);
                AudioChannelLayout stereo = {.mChannelLayoutTag = kAudioChannelLayoutTag_Stereo};
                NSData *layout = [NSData dataWithBytes:&stereo length:sizeof(stereo)];
                audioOut = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:audio outputSettings:@{
                    AVFormatIDKey: @(kAudioFormatLinearPCM),
                    AVSampleRateKey: @(rate),
                    AVNumberOfChannelsKey: @2,
                    AVChannelLayoutKey: layout,
                }];
                audioIn = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeAudio outputSettings:@{
                    AVFormatIDKey: @(kAudioFormatMPEG4AAC),
                    AVSampleRateKey: @(rate),
                    AVNumberOfChannelsKey: @2,
                    AVChannelLayoutKey: layout,
                    AVEncoderBitRateKey: @(audioRate),
                }];
                if (![reader canAddOutput:audioOut] || ![writer canAddInput:audioIn]) {
                    TCSetError(job, @"cannot carry this video's audio (-n drops it)", nil);
                    return -1;
                }
                audioNote = [NSString stringWithFormat:@"audio AAC %ld kbit/s", audioRate / 1000];
            }
            audioOut.alwaysCopiesSampleData = NO;
            audioIn.expectsMediaDataInRealTime = NO;
            [reader addOutput:audioOut];
            [writer addInput:audioIn];
            TCPump *audioPump = [TCPump new];
            audioPump.output = audioOut;
            audioPump.input = audioIn;
            [pumps addObject:audioPump];
        }

        int res = TCPumpAll(job, reader, writer, pumps, duration);
        if (res != 0)
            return res;

        NSString *summary = [NSString stringWithFormat:@"%@%@ %ldx%ld, %.1f Mbit/s%@, %@",
                             hevc ? @"HEVC" : @"H.264", tenBit ? @" HDR" : @"",
                             outWidth, outHeight, bitrate / 1e6,
                             automatic ? @" (auto)" : @"", audioNote];
        if (toneMap)
            summary = [summary stringByAppendingString:@", HDR mapped to SDR"];
        strlcpy(job->summary, summary.UTF8String, sizeof(job->summary));
        return 0;
    }
}

int ISHTranscodeRun(struct transcode_job *job) {
    return TCRun(job);
}

__attribute__((constructor)) static void TCInstall(void) {
    transcode_hook = TCRun;
    // A job's output lives in the temporary directory until it is copied
    // out. One the app died during leaves its file behind, and a video's
    // worth of space is worth taking back: nothing can be running yet.
    @autoreleasepool {
        NSString *dir = NSTemporaryDirectory();
        for (NSString *name in [NSFileManager.defaultManager contentsOfDirectoryAtPath:dir error:nil]) {
            if ([name hasPrefix:@TRANSCODE_TEMP_PREFIX])
                [NSFileManager.defaultManager removeItemAtPath:[dir stringByAppendingPathComponent:name] error:nil];
        }
    }
}
