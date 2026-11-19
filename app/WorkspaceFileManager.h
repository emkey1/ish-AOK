//
//  WorkspaceFileManager.h
//  AOK-iSH
//
//  A Finder-modeled file manager applet for Workspace mode: sidebar of fixed
//  locations, a native-scrolling file list, breadcrumb-less path label with
//  back/forward/up navigation, and New Folder / Rename / Duplicate / Delete /
//  Get Info via a row context menu. Built entirely on GuestFileBridge.
//

#import <UIKit/UIKit.h>
#import "WorkspaceViewController.h"

NS_ASSUME_NONNULL_BEGIN

@interface WorkspaceFileManagerToolViewController : WorkspaceThemedToolViewController <WorkspaceFileOpenable, WorkspaceStatefulTool>
@end

NS_ASSUME_NONNULL_END
