//
//  ISHFileProviderDomainCleanup.h
//  AOK-iSH
//
//  Shared by the app (Roots.m) and the File Provider extension
//  (FileProviderExtension.m), which are separate targets. Header-only so that
//  neither target needs another source file.
//

#import <Foundation/Foundation.h>
#import <FileProvider/FileProvider.h>

NS_ASSUME_NONNULL_BEGIN

// YES when this process is AOK running on a Mac ("Designed for iPad"), in the
// extension as well as the app: both are iOS binaries hosted by macOS. Cheap
// either way. On a Mac, Foundation answers isiOSAppOnMac from the dyld platform.
// On an iPhone or iPad, isiOSAppOnMac is a constant NO and isMacCatalystApp
// returns NO (build 554 on an iOS 27.0 iPad took the non-Mac path). dyld's
// platform is no test on its own: it is PLATFORM_IOS on the Mac and on the
// device alike.
//
// On a Mac the extension cannot work at all. It subclasses
// NSFileProviderExtension, the classic API, which the SDK marks
// API_UNAVAILABLE(macos). macOS treats every domain as replicated, and when
// fileproviderd asks the extension to serve one, FileProvider.framework finds a
// principal class that is not an NSFileProviderReplicatedExtension and aborts
// the process with __FILEPROVIDER_BAD_EXTENSION__.
static inline BOOL ISHFileProviderRunningOnMac(void) {
    NSProcessInfo *info = NSProcessInfo.processInfo;
    return info.isiOSAppOnMac || info.isMacCatalystApp;
}

// Removes every domain registered for this provider and calls `completion`
// once, on a private queue, after every removal has answered (unless `listed`
// stops it; see below). The summary is small and property-list typed so it can
// go straight into a breadcrumb:
//
//   domains          how many getDomains returned
//   removed          how many are gone
//   preserved        removals that kept user data somewhere
//   preservedAs      the names of the folders that data went to (absent if
//                    none; names only, since a full path names the user)
//   errors           removals that failed outright (absent if none)
//   preserveRefused  preserving removals refused, then retried plainly
//   listError        getDomains failed (absent if it did not)
//
// The arrays are sorted, so the same outcome gives an equal summary whatever
// order the removals answered in.
//
// `listed`, if not nil, is called once when getDomains answers, on the queue it
// answers on, before any removal is requested. It gets the part of the summary
// known by then: domains, and listError if there is one. It returns whether to
// go on. NO stops there: no removal is requested and `completion` is never
// called, since a summary of removals nobody attempted would be a false record.
// A caller that stops waiting for `completion` returns NO once it has, so that
// no removal it did not see listed is ever requested.
//
// On a Mac a removal first asks fileproviderd to keep dirty user data -- a
// domain left by an older build is a Finder location its user may have dropped
// files into, none of which the extension ever accepted -- and only falls back
// to the plain removal (what builds 550-554 always did) if that is refused.
// That mode is macOS-only in the SDK, hence the raw value; iOS always uses the
// plain removal.
static inline void ISHFileProviderRemoveRegisteredDomains(BOOL (^_Nullable listed)(NSDictionary<NSString *, id> *listing),
                                                          void (^completion)(NSDictionary<NSString *, id> *summary)) {
    [NSFileProviderManager getDomainsWithCompletionHandler:^(NSArray<NSFileProviderDomain *> *domains, NSError *listError) {
        NSMutableDictionary<NSString *, id> *summary = [NSMutableDictionary dictionary];
        NSMutableArray<NSString *> *errors = [NSMutableArray array];
        NSMutableArray<NSString *> *preserveRefused = [NSMutableArray array];
        NSMutableArray<NSString *> *preservedAs = [NSMutableArray array];
        __block NSUInteger removed = 0;
        __block NSUInteger preserved = 0;
        summary[@"domains"] = @(domains.count);
        if (listError != nil)
            summary[@"listError"] = listError.localizedDescription ?: @"unknown";
        if (listed != nil && !listed(summary.copy))
            return;

        dispatch_queue_t queue = dispatch_queue_create("app.ish.iSH-AOK.FileProviderDomainCleanup", DISPATCH_QUEUE_SERIAL);
        dispatch_group_t group = dispatch_group_create();
        void (^plainRemove)(NSFileProviderDomain *) = ^(NSFileProviderDomain *domain) {
            [NSFileProviderManager removeDomain:domain completionHandler:^(NSError *removeError) {
                dispatch_async(queue, ^{
                    if (removeError != nil)
                        [errors addObject:removeError.localizedDescription ?: @"unknown"];
                    else
                        removed++;
                    dispatch_group_leave(group);
                });
            }];
        };

        for (NSFileProviderDomain *domain in domains) {
            dispatch_group_enter(group);
            if (@available(iOS 16.0, *)) {
                if (ISHFileProviderRunningOnMac()) {
                    // NSFileProviderDomainRemovalModePreserveDirtyUserData
                    const NSFileProviderDomainRemovalMode preserveDirtyUserData = (NSFileProviderDomainRemovalMode) 1;
                    [NSFileProviderManager removeDomain:domain mode:preserveDirtyUserData
                                      completionHandler:^(NSURL *preservedLocation, NSError *removeError) {
                        dispatch_async(queue, ^{
                            if (removeError == nil) {
                                removed++;
                                if (preservedLocation != nil) {
                                    preserved++;
                                    [preservedAs addObject:preservedLocation.lastPathComponent ?: @"unknown"];
                                }
                                dispatch_group_leave(group);
                                return;
                            }
                            // Not an error yet: only the plain removal's
                            // outcome decides whether the domain is gone.
                            [preserveRefused addObject:removeError.localizedDescription ?: @"unknown"];
                            plainRemove(domain);
                        });
                    }];
                    continue;
                }
            }
            plainRemove(domain);
        }

        dispatch_group_notify(group, queue, ^{
            summary[@"removed"] = @(removed);
            summary[@"preserved"] = @(preserved);
            if (preservedAs.count != 0)
                summary[@"preservedAs"] = [preservedAs sortedArrayUsingSelector:@selector(compare:)];
            if (errors.count != 0)
                summary[@"errors"] = [errors sortedArrayUsingSelector:@selector(compare:)];
            if (preserveRefused.count != 0)
                summary[@"preserveRefused"] = [preserveRefused sortedArrayUsingSelector:@selector(compare:)];
            completion(summary.copy);
        });
    }];
}

NS_ASSUME_NONNULL_END
