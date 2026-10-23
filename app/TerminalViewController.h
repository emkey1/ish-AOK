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
// The session leader's pid, or 0 when there is no session. This is the ONLY
// identifier for a session that survives a suspend to disk: a Terminal's UUID
// dies with the process, and the restore hands out fresh pts numbers, but the
// checkpoint restores pids. The workspace records it in its saved layout so a
// window can ask for its own shell back.
@property (readonly) int sessionPid;
// The pseudo-terminal this window's own shell runs on, or nil when the window
// has none (it shows the console, or adopted a terminal it did not start).
// What the workspace records at suspend: the shell to ask for on resume is
// this one, whatever the window is displaying.
@property (readonly, nonatomic) Terminal *sessionTerminal;
// Set before a session starts to ask for a particular restored session (by the
// leader pid recorded in the saved layout). 0 means "any". Falls back to the
// next session in the queue when there is no such session -- see
// checkpoint_take_restored_session_for_pid.
@property (nonatomic) int desiredRestoredSessionPid;
// A window that had no shell of its own when the layout was saved (a System
// Console showing tty1 with nothing behind it) must not adopt a restored one
// on the way back: any session it took would be somebody else's, hidden
// behind the console it displays. The workspace sets this from its layout.
@property (nonatomic) BOOL declinesRestoredSession;
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
// The Cmd+= / Cmd+- / Cmd+0 actions. `command` is unused and may be nil. The
// workspace calls these when the chord is meant for a terminal window that does
// not hold first responder.
- (void)increaseFontSize:(UIKeyCommand *)command;
- (void)decreaseFontSize:(UIKeyCommand *)command;
- (void)resetFontSize:(UIKeyCommand *)command;
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
