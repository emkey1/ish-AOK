//
//  WorkspaceImageViewer.h
//  AOK-iSH
//
//  An image viewer applet for Workspace mode: pinch/double-tap zoom over a
//  jetsam-safe ImageIO downsampled decode, fit/actual-size toggle, and
//  prev/next navigation across sibling images in the same guest directory.
//

#import <UIKit/UIKit.h>
#import "WorkspaceViewController.h"

NS_ASSUME_NONNULL_BEGIN

@interface WorkspaceImageViewerToolViewController : WorkspaceThemedToolViewController <WorkspaceFileOpenable, WorkspaceStatefulTool>
@end

NS_ASSUME_NONNULL_END
