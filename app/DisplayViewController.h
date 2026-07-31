#import <UIKit/UIKit.h>
#import "ISHExternalDisplayContent.h"
#import "WorkspaceViewController.h"

NS_ASSUME_NONNULL_BEGIN

// Workspace "Display" applet: a headless Wayland desktop (labwc + foot +
// wayvnc, launched via /AOK/tools/start-wayland.sh) shown through a native
// Metal-rendered RFB client (DisplayRFBClient/DisplayRFBView) connected
// straight to wayvnc's loopback TCP port. See wayland_workspace_plan.md and
// the harmonic-giggling-aho plan for why this replaced the original vendored
// noVNC-in-WKWebView + WebSocket bridge design.
//
// The guest session is owned the same way TerminalViewController owns its
// shell session -- a real pseudo-terminal via +[Terminal createPseudoTerminal:]
// -- but its Terminal (a plain hterm-less instance) is never shown; only the
// DisplayRFBView is user-visible. Closing the applet hangs up that pty
// (SIGHUP), which start-wayland.sh's trap tears the whole guest stack down on.
// ISHExternalDisplayContent: the desktop is *mirrored* to an external display
// rather than moved there. Its Metal view is also the touch surface producing
// the RFB pointer events, so it has to stay where the fingers are; a second
// view on the external screen renders the same framebuffer.
@interface DisplayViewController : WorkspaceThemedToolViewController <ISHExternalDisplayContent>

// YES when this controller IS the scene's root (the "Wayland Display" startup
// mode) rather than an applet window inside the Workspace. Adds the same
// lower-right menu pip the Workspace desktop has (Open Workspace / Settings /
// Reconnect) -- without it a broken Wayland stack would leave no way back to
// Settings or a terminal -- and extends the display surface to the physical
// bottom edge instead of stopping at the safe area. Set before the view loads.
@property (nonatomic) BOOL standaloneMode;

@end

NS_ASSUME_NONNULL_END
