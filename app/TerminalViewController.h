//
//  ViewController.h
//  iSH
//
//  Created by Theodore Dubois on 10/17/17.
//

#import <UIKit/UIKit.h>
#import "ISHExternalDisplayContent.h"
#import "Terminal.h"

typedef NS_ENUM(NSInteger, ISHFreshSessionTerminalDisplayMode) {
    ISHFreshSessionTerminalDisplayModeAuto = 0,
    ISHFreshSessionTerminalDisplayModeSessionShell,
    ISHFreshSessionTerminalDisplayModeSystemConsole,
};

@interface TerminalViewController : UIViewController <ISHExternalDisplayContent>

@property (nonatomic) Terminal *terminal;
@property (nonatomic) ISHFreshSessionTerminalDisplayMode freshSessionTerminalDisplayMode;

- (void)startNewSession;
- (void)showSystemConsoleForCurrentSession;
- (void)showSessionShellForCurrentSession;
- (void)reconnectSessionFromTerminalUUID:(NSUUID *)uuid;
- (void)focusTerminal;
@property (readonly) NSUUID *sessionTerminalUUID; // 0 means invalid
@property UISceneSession *sceneSession API_AVAILABLE(ios(13.0));
@property (nonatomic) BOOL showsWorkspaceDashboardButton;
@property (nonatomic) BOOL embeddedInWorkspaceWindow;
// Session Shell surfaces always sign in as root, regardless of the "Login as
// Default User" preference. Set by the workspace host on windows opened in
// (or restored into) the Session Shell role; the main full-screen terminal's
// session (the "Session Shell (pts/1)" surface) is root unconditionally.
@property (nonatomic) BOOL alwaysLoginAsRoot;
// Per-window terminal font size (workspace desktop windows). 0 = track the
// global UserPreferences fontSize. Forwards to the terminal view; used by the
// workspace host to persist/restore a window's size in the saved layout.
@property (nonatomic) CGFloat overrideFontSize;
// Set by the workspace host for an embedded terminal. Invoked (on the main queue) when the
// shell session ends, so the host closes the contained window instead of the default relaunch
// of the login shell. Left nil for the main full-screen terminal, which keeps relaunching.
// (No nullability qualifier: this header isn't audited for nullability, and adding one here
// would trip -Wnullability-completeness on the other unannotated pointers.)
@property (nonatomic, copy) dispatch_block_t workspaceSessionDidEndHandler;

// Ends the live shell session (hangs up its pty) without relaunching. Used by the workspace
// host when its window is closed via the × so the shell doesn't keep running headless. Safe to
// call repeatedly; a no-op once the session is already gone.
- (void)disposeSessionForWorkspaceClose;

// ISHExternalDisplayContent: relocating hands the terminal's rendering to a
// view living on the external screen; this controller keeps the keyboard, the
// accessory bar and first responder, so input is unaffected.

@end

extern struct tty_driver ios_tty_driver;
