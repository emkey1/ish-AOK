//
//  WorkspaceMarkdownViewer.h
//  AOK-Workstation
//
//  A read-only Markdown reader applet for Workspace mode: renders a guest
//  file with the shared MarkdownRenderer, follows relative document links
//  in place (with back history), and opens http(s) links externally.
//

#import <UIKit/UIKit.h>
#import "WorkspaceViewController.h"

NS_ASSUME_NONNULL_BEGIN

@interface WorkspaceMarkdownViewerToolViewController : WorkspaceThemedToolViewController <WorkspaceFileOpenable, WorkspaceStatefulTool, WorkspaceTextScalable>
@end

NS_ASSUME_NONNULL_END
