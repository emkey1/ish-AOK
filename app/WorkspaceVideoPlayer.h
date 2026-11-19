//
//  WorkspaceVideoPlayer.h
//  AOK-iSH
//
//  A video player applet for Workspace mode, wrapping AVPlayerViewController
//  (scrubber/rate/AirPlay/fullscreen come for free). Realfs-backed files play
//  directly from their host URL; fakefs files are extracted to a temp file
//  first, with a progress/cancel overlay while that happens.
//

#import <UIKit/UIKit.h>
#import "WorkspaceViewController.h"

NS_ASSUME_NONNULL_BEGIN

@interface WorkspaceVideoPlayerToolViewController : WorkspaceThemedToolViewController <WorkspaceFileOpenable, WorkspaceStatefulTool>
@end

NS_ASSUME_NONNULL_END
