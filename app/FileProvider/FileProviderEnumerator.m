//
//  FileProviderEnumerator.m
//  iSHFiles
//
//  Created by Theodore Dubois on 9/20/18.
//

#import <MobileCoreServices/MobileCoreServices.h>
#include <dirent.h>
#import "../AppGroup.h"
#import "FileProviderExtension.h"
#import "FileProviderEnumerator.h"
#import "FileProviderItem.h"
#import "NSError+ISHErrno.h"
#include "fs/fake-db.h"

static NSNumber *ISHFileProviderEnumeratorDurationMilliseconds(NSTimeInterval start) {
    return @((NSInteger) ((NSDate.date.timeIntervalSinceReferenceDate - start) * 1000.0));
}

@interface FileProviderEnumerator ()

@property FileProviderItem *item;
@property (weak) FileProviderExtension *extension;

@end

@implementation FileProviderEnumerator

- (instancetype)initWithItem:(FileProviderItem *)item extension:(FileProviderExtension *)extension {
    if (self = [super init]) {
        self.item = item;
        self.extension = extension;
    }
    return self;
}

- (void)enumerateItemsForObserver:(id<NSFileProviderEnumerationObserver>)observer startingAtPage:(NSFileProviderPage)page {
    NSLog(@"enumeration start %@", self.item.itemIdentifier);
    NSString *containerIdentifier = self.item.itemIdentifier ?: @"working-set";
    NSString *domainIdentifier = self.item.mountOwner.rootName ?: @"iSH-AOK";
    ISHFileProviderRecordBreadcrumb(@"fileprovider.enumerate.begin",
                                    @{@"container": containerIdentifier,
                                      @"domain": domainIdentifier});
    NSTimeInterval start = NSDate.date.timeIntervalSinceReferenceDate;
    // if we're asked to enumerate the working set
    if (self.item == nil) {
        ISHFileProviderRecordBreadcrumb(@"fileprovider.enumerate.end",
                                        @{@"container": containerIdentifier,
                                          @"domain": domainIdentifier,
                                          @"duration_ms": ISHFileProviderEnumeratorDurationMilliseconds(start),
                                          @"count": @0});
        [observer finishEnumeratingUpToPage:page];
        return;
    }
    // The synthetic domain root lists each installed root as a folder, plus a
    // "Persist" folder for /AOK/persist (shared across every root, and not
    // itself an installed root).
    if (self.item.mountOwner == nil) {
        NSMutableArray<FileProviderItem *> *items = [NSMutableArray new];
        NSArray<NSString *> *rootNames = [[self.extension installedRootNames] arrayByAddingObject:ISHFileProviderPersistRootName];
        for (NSString *rootName in rootNames) {
            NSError *err;
            ISHFileProviderMount *mount = [self.extension mountForRootName:rootName error:&err];
            if (mount == nil) {
                NSLog(@"skipping root %@: %@", rootName, err);
                continue;
            }
            FileProviderItem *rootItem = [[FileProviderItem alloc] initWithIdentifier:NSFileProviderRootContainerItemIdentifier mountOwner:mount error:&err];
            if (rootItem != nil)
                [items addObject:rootItem];
        }
        ISHFileProviderRecordBreadcrumb(@"fileprovider.enumerate.end",
                                        @{@"container": containerIdentifier,
                                          @"domain": domainIdentifier,
                                          @"duration_ms": ISHFileProviderEnumeratorDurationMilliseconds(start),
                                          @"count": @(items.count)});
        [observer didEnumerateItems:items];
        [observer finishEnumeratingUpToPage:nil];
        return;
    }
    // if we're asked to enumerate a file
    if (![self.item.typeIdentifier isEqualToString:(NSString *) kUTTypeFolder]) {
        NSLog(@"not enumerating a file (%@)", self.item.typeIdentifier);
        ISHFileProviderRecordBreadcrumb(@"fileprovider.enumerate.end",
                                        @{@"container": containerIdentifier,
                                          @"domain": domainIdentifier,
                                          @"duration_ms": ISHFileProviderEnumeratorDurationMilliseconds(start),
                                          @"count": @0,
                                          @"skipped": @YES});
        [observer finishEnumeratingUpToPage:page];
        return;
    }

    int rootLockFd = ISHAppGroupAcquireNamedLock(@"root", self.item.mountOwner.rootName ?: @"", NO, nil);

    NSError *error;
    int fd = [self.item openNewFDWithError:&error];
    if (fd == -1) {
        ISHAppGroupReleaseLock(rootLockFd);
        ISHFileProviderRecordBreadcrumb(@"fileprovider.enumerate.failed",
                                        @{@"container": containerIdentifier,
                                          @"domain": domainIdentifier,
                                          @"duration_ms": ISHFileProviderEnumeratorDurationMilliseconds(start),
                                          @"error": error.localizedDescription ?: @"unknown"});
        [observer finishEnumeratingWithError:error];
        return;
    }
    DIR *dir = fdopendir(fd);
    if (dir == NULL) {
        // fdopendir can fail (ENOMEM, or the directory raced out from under us
        // between openNewFDWithError: and here). readdir(NULL) is undefined
        // behavior and in practice faults inside the DIR's own mutex, and the
        // fd is a real leak with no DIR* to closedir() it through. Same guard
        // as -[FileProviderItem childItemCount].
        NSError *dirError = [NSError errorWithDomain:NSPOSIXErrorDomain code:errno userInfo:nil];
        close(fd);
        ISHAppGroupReleaseLock(rootLockFd);
        ISHFileProviderRecordBreadcrumb(@"fileprovider.enumerate.failed",
                                        @{@"container": containerIdentifier,
                                          @"domain": domainIdentifier,
                                          @"duration_ms": ISHFileProviderEnumeratorDurationMilliseconds(start),
                                          @"error": dirError.localizedDescription ?: @"fdopendir failed"});
        [observer finishEnumeratingWithError:dirError];
        return;
    }
    NSMutableArray<FileProviderItem *> *items = [NSMutableArray new];
    // Entries we could not build an item for. Counted and reported rather than
    // silently dropped -- a listing that is quietly short is its own bug.
    NSUInteger skipped = 0;
    NSError *firstSkipError = nil;
    NSString *firstSkippedName = nil;
    struct dirent *dirent;
    errno = 0;
    while ((dirent = readdir(dir))) {
        if (strcmp(dirent->d_name, ".") == 0 || strcmp(dirent->d_name, "..") == 0)
            continue;

        NSString *path = _item.path;
        // host entry names are in escaped on-disk form (fs/fake-path.h)
        NSString *name = ISHGuestPathForHostPath([NSString stringWithUTF8String:dirent->d_name]);
        // Join with an explicit leading slash so both a root folder (path == "")
        // and a sub-directory (path == "/bin") produce the slash-prefixed path
        // the fakefs db stores ("/bin", "/bin/ls"). -stringByAppendingPathComponent:
        // drops the leading slash on an empty receiver, which left a root's
        // direct children unresolvable (every lookup missed -> empty listing).
        NSString *childPath = [NSString stringWithFormat:@"%@/%@", path, name];
        db_begin_read(&_item.mount->db);
        inode_t inode = path_get_inode(&_item.mount->db, childPath.fileSystemRepresentation);
        db_commit(&_item.mount->db);
        NSString *childIdent;
        if (inode == 0) {
            childIdent = ISHFileProviderVirtualIdentifierForPath(childPath);
        } else {
            childIdent = [NSString stringWithFormat:@"%lu", (unsigned long) inode];
        }

        NSLog(@"returning %s %@", dirent->d_name, childIdent);
        FileProviderItem *item = [[FileProviderItem alloc] initWithIdentifier:childIdent mountOwner:_item.mountOwner error:&error];
        if (item == nil) {
            // Skip this entry, do not abandon the directory. Aborting here
            // discarded every item already collected, so a SINGLE unopenable
            // child made the whole folder show up empty in the Files app --
            // and it stayed empty on every later enumeration too, because the
            // offending file was still there. That is the "copy a file into
            // Persist and the folder goes blank until you delete it" report:
            // a read-only imported file (O_RDWR -> EACCES), a dangling
            // symlink, or a stale db row is enough to do it.
            //
            // One missing row is a far better failure than no rows. The
            // domain-root branch above already takes this view and continues
            // past a root it cannot mount; this loop was the odd one out.
            skipped++;
            if (firstSkipError == nil)
                firstSkipError = error;
            if (firstSkippedName == nil)
                firstSkippedName = name;
            NSLog(@"skipping unreadable entry %@: %@", childIdent, error.localizedDescription ?: @"unknown");
            error = nil;
            errno = 0;
            continue;
        }
        [items addObject:item];
        errno = 0;
    }
    if (errno != 0) {
        NSError *error = [NSError errorWithDomain:NSPOSIXErrorDomain code:errno userInfo:nil];
        NSLog(@"readdir returned %@", error);
        ISHAppGroupReleaseLock(rootLockFd);
        ISHFileProviderRecordBreadcrumb(@"fileprovider.enumerate.failed",
                                        @{@"container": containerIdentifier,
                                          @"domain": domainIdentifier,
                                          @"duration_ms": ISHFileProviderEnumeratorDurationMilliseconds(start),
                                          @"error": error.localizedDescription ?: @"unknown"});
        [observer finishEnumeratingWithError:error];
        closedir(dir);
        return;
    }

    closedir(dir);
    ISHAppGroupReleaseLock(rootLockFd);
    NSLog(@"returning %@", items);
    NSMutableDictionary<NSString *, id> *endCrumb = [NSMutableDictionary dictionaryWithDictionary:@{
        @"container": containerIdentifier,
        @"domain": domainIdentifier,
        @"duration_ms": ISHFileProviderEnumeratorDurationMilliseconds(start),
        @"count": @(items.count),
    }];
    if (skipped > 0) {
        endCrumb[@"skipped"] = @(skipped);
        endCrumb[@"skipped_first"] = firstSkippedName ?: @"?";
        endCrumb[@"skipped_error"] = firstSkipError.localizedDescription ?: @"unknown";
    }
    ISHFileProviderRecordBreadcrumb(@"fileprovider.enumerate.end", endCrumb);
    [observer didEnumerateItems:items];
    [observer finishEnumeratingUpToPage:nil];
}

- (void)enumerateChangesForObserver:(id<NSFileProviderChangeObserver>)observer fromSyncAnchor:(NSFileProviderSyncAnchor)anchor {
    NSLog(@"saying no file changes");
    // TODO implement by having the sync anchor be a serialized list of files
    [observer finishEnumeratingChangesUpToSyncAnchor:anchor moreComing:NO];
}

- (void)invalidate {
    // The system is done with this enumerator. Release the item, because a
    // FileProviderItem retains its mountOwner and therefore holds the fakefs
    // database open: while any enumerator lives, the extension's idle close has
    // nothing it can actually close, and the database survives until process
    // teardown, where sqlite3_close()'s WAL checkpoint runs into suspension and
    // earns a 0xdead10cc kill. This was an empty stub before, which is why the
    // mounts stayed open for the extension's whole lifetime.
    //
    // Safe even if the system ignores its own "no further calls" contract:
    // -enumerateItemsForObserver:startingAtPage: already treats a nil item as
    // the empty working set, and -enumerateChangesForObserver: never reads it.
    self.item = nil;
}

@end
