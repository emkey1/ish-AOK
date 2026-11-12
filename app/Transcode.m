//
//  Transcode.m
//  iSH-AOK
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
@property (nonatomic) BOOL failed;
@end
@implementation TCPump
@end

static int TCRun(struct transcode_job *job) {
    @autoreleasepool {
        NSError *error = nil;
        NSURL *inURL = [NSURL fileURLWithPath:@(job->in_host_path)];
        AVURLAsset *asset = [AVURLAsset URLAssetWithURL:inURL
                                                options:@{AVURLAssetPreferPreciseDurationAndTimingKey: @YES}];
        if (!TCLoad(asset, @[@"tracks", @"duration", @"metadata"], &error)) {
            TCSetError(job, @"cannot read the input as a video", error);
            return -1;
        }
        AVAssetTrack *video = [asset tracksWithMediaType:AVMediaTypeVideo].firstObject;
        AVAssetTrack *audio = job->no_audio ? nil : [asset tracksWithMediaType:AVMediaTypeAudio].firstObject;
        if (video == nil) {
            TCSetError(job, @"the input has no video track", nil);
            return -1;
        }
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
            bitrate = lround(outWidth * outHeight * fps * (hevc ? 0.05 : 0.08));
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

        NSString *ext = job->mov ? @"mov" : @"mp4";
        NSString *outPath = [NSTemporaryDirectory() stringByAppendingPathComponent:
            [NSString stringWithFormat:@"transcode-%@.%@", NSUUID.UUID.UUIDString, ext]];
        strlcpy(job->out_host_path, outPath.fileSystemRepresentation, sizeof(job->out_host_path));
        AVAssetWriter *writer = [AVAssetWriter assetWriterWithURL:[NSURL fileURLWithPath:outPath]
                                                         fileType:job->mov ? AVFileTypeQuickTimeMovie : AVFileTypeMPEG4
                                                            error:&error];
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
        NSMutableArray<TCPump *> *pumps = [NSMutableArray arrayWithObject:videoPump];

        // Audio: copied as it is when the container takes it, otherwise
        // re-encoded as AAC (newer iPhones record spatial audio, which an
        // MPEG-4 file cannot carry as it is).
        NSString *audioNote = @"no audio";
        if (audio != nil) {
            CMFormatDescriptionRef audioFormat =
                (__bridge CMFormatDescriptionRef) audio.formatDescriptions.firstObject;
            AVAssetReaderTrackOutput *audioOut = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:audio
                                                                                            outputSettings:nil];
            AVAssetWriterInput *audioIn = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeAudio
                                                                             outputSettings:nil
                                                                           sourceFormatHint:audioFormat];
            BOOL copied = audioFormat != NULL && [reader canAddOutput:audioOut] && [writer canAddInput:audioIn];
            if (copied) {
                audioNote = @"audio copied";
            } else {
                double rate = 48000;
                const AudioStreamBasicDescription *asbd =
                    audioFormat ? CMAudioFormatDescriptionGetStreamBasicDescription(audioFormat) : NULL;
                if (asbd != NULL && (asbd->mSampleRate == 44100 || asbd->mSampleRate == 48000))
                    rate = asbd->mSampleRate;
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
                    AVEncoderBitRateKey: @(job->audio_bitrate),
                }];
                if (![reader canAddOutput:audioOut] || ![writer canAddInput:audioIn]) {
                    TCSetError(job, @"cannot carry this video's audio (-n drops it)", nil);
                    return -1;
                }
                audioNote = [NSString stringWithFormat:@"audio AAC %ld kbit/s", job->audio_bitrate / 1000];
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

        // Progress, as the presentation time of the last video frame passed
        // on, in microseconds. Written by the video queue, read here.
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
                    if (pump.isVideo) {
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

__attribute__((constructor)) static void TCInstall(void) {
    transcode_hook = TCRun;
}
