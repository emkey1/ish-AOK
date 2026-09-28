#import <UIKit/UIKit.h>
#import "WorkspaceViewController.h"

NS_ASSUME_NONNULL_BEGIN

// Workspace "Display" applet: a headless Wayland desktop (labwc + foot +
// wayvnc, launched via /AOK/tools/start-wayland.sh) shown through a native
// Metal-rendered RFB client (DisplayRFBClient/DisplayRFBView) connected
// straight to wayvnc's loopback TCP port. See docs/historical/wayland_workspace_plan.md and
// the harmonic-giggling-aho plan for why this replaced the original vendored
// noVNC-in-WKWebView + WebSocket bridge design.
//
// The guest session is owned the same way TerminalViewController owns its
// shell session -- a real pseudo-terminal via +[Terminal createPseudoTerminal:]
// -- but its Terminal (a plain hterm-less instance) is never shown; only the
// DisplayRFBView is user-visible. Closing the applet hangs up that pty
// (SIGHUP), which start-wayland.sh's trap tears the whole guest stack down on.
@interface DisplayViewController : WorkspaceThemedToolViewController

// YES when this controller IS the scene's root (the "Wayland Display" startup
// mode) rather than an applet window inside the Workspace. Adds the same
// lower-right menu pip the Workspace desktop has (Open Workspace / Settings /
// Reconnect) -- without it a broken Wayland stack would leave no way back to
// Settings or a terminal -- and extends the display surface to the physical
// bottom edge instead of stopping at the safe area. Set before the view loads.
@property (nonatomic) BOOL standaloneMode;

// Hands the running Wayland session to the next Wayland view to start, instead
// of ending it with this one: how the session moves between full screen and
// the Workspace's window. A no-op without a session.
- (void)parkSession;

@end

// The desktop size the applet asks the compositor for, given the size of the
// surface that shows it, in points (#483), and how many desktop pixels to ask
// for per point (1 unless the user raised it -- see displayDesktopScale).
// Scaled up uniformly so the short side is at least 480 and down so the long
// side is at most 4096 (the ceiling wins when both apply), then rounded to even
// dimensions. CGSizeZero for a surface under 1pt on either side.
CGSize DisplayDesktopSizeForViewSize(CGSize viewSize, CGFloat pixelsPerPoint);

NS_ASSUME_NONNULL_END
