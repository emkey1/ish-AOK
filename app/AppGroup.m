//
//  AppGroup.m
//  iSH
//
//  Created by Theodore Dubois on 2/28/20.
//

#import "AppGroup.h"
#import <Foundation/Foundation.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#import <mach-o/dyld.h>
#import <mach-o/loader.h>
#import <mach-o/getsect.h>

#define CSMAGIC_EMBEDDED_SIGNATURE 0xfade0cc0
#define CSMAGIC_EMBEDDED_ENTITLEMENTS 0xfade7171

struct cs_blob_index {
    uint32_t type;
    uint32_t offset;
};

struct cs_superblob {
    uint32_t magic;
    uint32_t length;
    uint32_t count;
    struct cs_blob_index index[];
};

struct cs_entitlements {
    uint32_t magic;
    uint32_t length;
    char entitlements[];
};

static const struct mach_header_64 *MainExecutableHeader(void) {
    NSString *expectedPath = NSBundle.mainBundle.executablePath.stringByResolvingSymlinksInPath;
    if (expectedPath == nil)
        return NULL;

    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        const char *imageName = _dyld_get_image_name(i);
        if (imageName == NULL)
            continue;
        NSString *loadedPath = [[NSString stringWithUTF8String:imageName] stringByResolvingSymlinksInPath];
        if ([loadedPath isEqualToString:expectedPath])
            return (const struct mach_header_64 *) _dyld_get_image_header(i);
    }

    return (const struct mach_header_64 *) _dyld_get_image_header(0);
}

static NSDictionary *AppEntitlements(void) {
    static NSDictionary *entitlements = nil;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        const struct mach_header_64 *header = MainExecutableHeader();
        if (header == NULL)
            return;
        
        // Check for entitlements in the __entitlements section (common in simulators)
        size_t entitlements_size;
        char *entitlements_data = (char *) getsectiondata(header, "__TEXT", "__entitlements", &entitlements_size);
        if (entitlements_data != NULL) {
            NSData *data = [NSData dataWithBytesNoCopy:entitlements_data length:entitlements_size freeWhenDone:NO];
            NSError *error = nil;
            entitlements = [NSPropertyListSerialization propertyListWithData:data options:NSPropertyListImmutable format:nil error:&error];
            if (entitlements == nil) {
                NSLog(@"Failed to parse entitlements: %@", error);
                return;
            }
            return;
        }
        
        // Code for fetching entitlements from the code signature
        struct load_command *lc = (void *) (header + 1);
        struct linkedit_data_command *cs_lc = NULL;
        for (uint32_t i = 0; i < header->ncmds; i++) {
            if (lc->cmd == LC_CODE_SIGNATURE) {
                cs_lc = (void *) lc;
                break;
            }
            lc = (void *) ((char *) lc + lc->cmdsize);
        }
        if (cs_lc == NULL)
            return;

        NSFileHandle *fileHandle = [NSFileHandle fileHandleForReadingFromURL:NSBundle.mainBundle.executableURL error:nil];
        if (fileHandle == nil)
            return;
        [fileHandle seekToFileOffset:cs_lc->dataoff];
        NSData *csData = [fileHandle readDataOfLength:cs_lc->datasize];
        [fileHandle closeFile];
        if (csData.length < sizeof(struct cs_superblob))
            return;
        const struct cs_superblob *cs = csData.bytes;
        if (ntohl(cs->magic) != CSMAGIC_EMBEDDED_SIGNATURE)
            return;

        NSData *entitlementsData = nil;
        for (uint32_t i = 0; i < ntohl(cs->count); i++) {
            size_t blob_offset = ntohl(cs->index[i].offset);
            if (blob_offset > csData.length || csData.length - blob_offset < sizeof(struct cs_entitlements))
                return;
            struct cs_entitlements *ents = (void *) ((char *) cs + blob_offset);

            // Read the magic number in a way that does not assume alignment
            uint32_t magic;
            memcpy(&magic, &ents->magic, sizeof(uint32_t));
            uint32_t length;
            memcpy(&length, &ents->length, sizeof(uint32_t));
            length = ntohl(length);
            if (length < offsetof(struct cs_entitlements, entitlements) || blob_offset + length > csData.length)
                return;
            if (ntohl(magic) == CSMAGIC_EMBEDDED_ENTITLEMENTS) {
                entitlementsData = [NSData dataWithBytes:ents->entitlements length:length - offsetof(struct cs_entitlements, entitlements)];
                break; // Entitlements found
            }
        }
        
        if (entitlementsData == nil)
            return;

        NSError *serializationError = nil;
        entitlements = [NSPropertyListSerialization propertyListWithData:entitlementsData options:NSPropertyListImmutable format:nil error:&serializationError];
        if (entitlements == nil) {
            NSLog(@"Failed to parse entitlements: %@", serializationError);
            return;
        }
    });
    return entitlements;
}

NSArray<NSString *> *CurrentAppGroups(void) {
    return AppEntitlements()[@"com.apple.security.application-groups"];
}

NSURL *ContainerURL(void) {
    NSString *appGroup = CurrentAppGroups()[0];
    return [NSFileManager.defaultManager containerURLForSecurityApplicationGroupIdentifier:appGroup];
}

NSURL *ISHRootsExposureDirectoryURL(void) {
    NSURL *containerURL = ContainerURL();
    if (containerURL == nil)
        return nil;
    return [[containerURL URLByAppendingPathComponent:@"AOK" isDirectory:YES]
            URLByAppendingPathComponent:@"roots" isDirectory:YES];
}

static NSString *ISHAppGroupLockSafeComponent(NSString *value) {
    if (value.length == 0)
        return @"default";
    NSCharacterSet *allowed = [NSCharacterSet characterSetWithCharactersInString:
                               @"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-"];
    NSMutableString *safe = [NSMutableString stringWithCapacity:value.length];
    for (NSUInteger i = 0; i < value.length; i++) {
        unichar ch = [value characterAtIndex:i];
        if ([allowed characterIsMember:ch]) {
            [safe appendFormat:@"%C", ch];
        } else {
            [safe appendFormat:@"_%02x", (unsigned) ch & 0xff];
        }
    }
    return safe;
}

static int ISHAppGroupAcquireNamedLockInternal(NSString *category, NSString *name, BOOL exclusive, BOOL nonBlocking, NSError **error) {
    NSURL *containerURL = ContainerURL();
    if (containerURL == nil) {
        if (error != nil) {
            *error = [NSError errorWithDomain:NSPOSIXErrorDomain
                                         code:ENOENT
                                     userInfo:@{NSLocalizedDescriptionKey: @"Missing app group container"}];
        }
        return -1;
    }

    NSURL *locksDirectoryURL = [containerURL URLByAppendingPathComponent:@"Locks" isDirectory:YES];
    if (![NSFileManager.defaultManager createDirectoryAtURL:locksDirectoryURL
                                withIntermediateDirectories:YES
                                                 attributes:nil
                                                      error:error]) {
        return -1;
    }

    NSString *safeCategory = ISHAppGroupLockSafeComponent(category ?: @"shared");
    NSString *safeName = ISHAppGroupLockSafeComponent(name ?: @"default");
    NSURL *lockURL = [locksDirectoryURL URLByAppendingPathComponent:
                      [NSString stringWithFormat:@"%@-%@.lock", safeCategory, safeName]
                                                 isDirectory:NO];

    int fd = open(lockURL.fileSystemRepresentation, O_CREAT | O_RDWR | O_CLOEXEC, 0666);
    if (fd < 0) {
        if (error != nil)
            *error = [NSError errorWithDomain:NSPOSIXErrorDomain code:errno userInfo:nil];
        return -1;
    }

    int operation = exclusive ? LOCK_EX : LOCK_SH;
    if (nonBlocking)
        operation |= LOCK_NB;
    if (flock(fd, operation) < 0) {
        int flockErrno = errno;
        close(fd);
        if (error != nil)
            *error = [NSError errorWithDomain:NSPOSIXErrorDomain code:flockErrno userInfo:nil];
        return -1;
    }
    return fd;
}

int ISHAppGroupAcquireNamedLock(NSString *category, NSString *name, BOOL exclusive, NSError **error) {
    return ISHAppGroupAcquireNamedLockInternal(category, name, exclusive, NO, error);
}

// Like ISHAppGroupAcquireNamedLock, but returns -1/EWOULDBLOCK immediately
// instead of blocking when the lock is already held elsewhere (e.g. the
// FileProvider extension mid-operation on that root). Used for opportunistic
// mounts -- e.g. exposing a non-booted root under /AOK/roots -- where losing
// the race just means skipping that mount, not stalling app boot.
int ISHAppGroupTryAcquireNamedLock(NSString *category, NSString *name, BOOL exclusive, NSError **error) {
    return ISHAppGroupAcquireNamedLockInternal(category, name, exclusive, YES, error);
}

void ISHAppGroupReleaseLock(int fd) {
    if (fd < 0)
        return;
    flock(fd, LOCK_UN);
    close(fd);
}
