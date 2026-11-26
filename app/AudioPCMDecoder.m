//
//  AudioPCMDecoder.m
//  AOK-Workstation
//
//  Format detection + dispatch to the right decoder backend. Everything reduces
//  to an ISHPCMDecoder so the engine never special-cases a format.
//

#import "AudioPCMDecoder.h"
#import "AudioPCMDecoder_Internal.h"

NSString *const ISHAudioMetadataTitleKey = @"title";
NSString *const ISHAudioMetadataArtistKey = @"artist";
NSString *const ISHAudioMetadataAlbumKey = @"album";
NSString *const ISHAudioMetadataArtworkKey = @"artwork";
NSString *const ISHAudioErrorDomain = @"ISHAudioErrorDomain";

// Native AVFoundation handles these directly.
static NSSet<NSString *> *AVFExtensions(void) {
    static NSSet *set;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        set = [NSSet setWithArray:@[@"mp3", @"m4a", @"mp4", @"aac", @"adts", @"alac",
                                    @"wav", @"wave", @"aif", @"aiff", @"aifc", @"caf", @"flac"]];
    });
    return set;
}

NSSet<NSString *> *ISHAudioSupportedExtensions(void) {
    static NSSet *set;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSMutableSet *all = [AVFExtensions() mutableCopy];
        // Advertise Ogg/Opus only when their decoders are actually built in, so
        // the picker doesn't list files the player can't open.
        if (ISHAudioVorbisAvailable()) [all addObjectsFromArray:@[@"ogg", @"oga"]];
        if (ISHAudioOpusAvailable()) [all addObject:@"opus"];
        if (ISHAudioVorbisAvailable() || ISHAudioOpusAvailable()) [all addObject:@"ogg"];
        set = [all copy];
    });
    return set;
}

// Sniff an Ogg stream's first page to tell Opus from Vorbis (a .ogg file can
// carry either). Returns "opus", "vorbis", or nil if not recognizably Ogg.
static NSString *SniffOggCodec(NSURL *url) {
    NSFileHandle *handle = [NSFileHandle fileHandleForReadingFromURL:url error:NULL];
    if (handle == nil) return nil;
    NSData *head = [handle readDataOfLength:128];
    [handle closeFile];
    if (head.length < 4) return nil;
    const char *bytes = head.bytes;
    if (memcmp(bytes, "OggS", 4) != 0) return nil;
    NSData *opusMagic = [@"OpusHead" dataUsingEncoding:NSASCIIStringEncoding];
    NSData *vorbisMagic = [@"vorbis" dataUsingEncoding:NSASCIIStringEncoding];
    if ([head rangeOfData:opusMagic options:0 range:NSMakeRange(0, head.length)].location != NSNotFound)
        return @"opus";
    if ([head rangeOfData:vorbisMagic options:0 range:NSMakeRange(0, head.length)].location != NSNotFound)
        return @"vorbis";
    return nil;
}

id<ISHPCMDecoder> ISHCreatePCMDecoderForFileURL(NSURL *fileURL, NSError **error) {
    NSString *ext = fileURL.pathExtension.lowercaseString;

    if ([AVFExtensions() containsObject:ext])
        return ISHCreateAVFoundationDecoder(fileURL, error);

    if ([ext isEqualToString:@"opus"])
        return ISHCreateOpusDecoder(fileURL, error);

    if ([ext isEqualToString:@"ogg"] || [ext isEqualToString:@"oga"]) {
        NSString *codec = SniffOggCodec(fileURL);
        if ([codec isEqualToString:@"opus"]) return ISHCreateOpusDecoder(fileURL, error);
        if ([codec isEqualToString:@"vorbis"]) return ISHCreateVorbisDecoder(fileURL, error);
        // Unknown Ogg payload — try Vorbis, then Opus.
        id<ISHPCMDecoder> d = ISHCreateVorbisDecoder(fileURL, NULL);
        if (d) return d;
        return ISHCreateOpusDecoder(fileURL, error);
    }

    // Unknown extension: let AVFoundation try (it sniffs container magic).
    return ISHCreateAVFoundationDecoder(fileURL, error);
}
