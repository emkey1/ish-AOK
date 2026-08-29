//
//  AppGroup.h
//  iSH
//
//  Created by Theodore Dubois on 2/28/20.
//

#import <Foundation/Foundation.h>

NSURL *ContainerURL(void);
// NO when ContainerURL() is this app's own container rather than the shared app
// group one, which is what a build re-signed without the App Group entitlement
// gets. Anything that has to be visible from another process -- the File
// Provider extension above all -- is unavailable in that state.
BOOL ContainerIsSharedAppGroup(void);
// The host directory mounted at /AOK/roots, under which every installed root
// other than the booted one is exposed as /AOK/roots/<name>. Lives here rather
// than next to its siblings in AppDelegate.m because both the boot that
// creates those mounts and the rename/delete that tear them down (Roots.m)
// need it. Distinct from AOK/persist/roots, the cached-archive directory.
NSURL *ISHRootsExposureDirectoryURL(void);
int ISHAppGroupAcquireNamedLock(NSString *category, NSString *name, BOOL exclusive, NSError **error);
int ISHAppGroupTryAcquireNamedLock(NSString *category, NSString *name, BOOL exclusive, NSError **error);
void ISHAppGroupReleaseLock(int fd);
