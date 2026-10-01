//
//  WorkspaceTestHooks.h
//
//  A remote control for the Workspace, for tests/workspace/restore_test.sh.
//  Off unless the app is launched with ISH_WORKSPACE_TEST=1 in its environment
//  (SIMCTL_CHILD_ISH_WORKSPACE_TEST=1 from simctl), which nothing but that
//  harness sets.
//

#import <Foundation/Foundation.h>

// Call once at launch; does nothing unless ISH_WORKSPACE_TEST is set.
void ISHWorkspaceTestHooksStart(void);
