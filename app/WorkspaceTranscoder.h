//
//  WorkspaceTranscoder.h
//  iSH-AOK
//
//  The Video Converter applet: the Workspace front end of
//  /AOK/native/transcode. It converts a video from Photos, from the Files app
//  or from the guest on the hardware encoder (app/Transcode.m), and saves the
//  result to Photos or beside the original.
//

#import <UIKit/UIKit.h>
#import "WorkspaceViewController.h"

NS_ASSUME_NONNULL_BEGIN

@interface WorkspaceTranscoderToolViewController : WorkspaceThemedToolViewController <WorkspaceFileOpenable>
@end

NS_ASSUME_NONNULL_END
