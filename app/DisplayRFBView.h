#import <MetalKit/MetalKit.h>
#import "DisplayRFBClient.h"

NS_ASSUME_NONNULL_BEGIN

// Renders a DisplayRFBClient's framebuffer via Metal (not CALayer.contents +
// CGImage -- that composites on the GPU too, but the per-frame CGImage/
// CGDataProvider upload path is opaque about when/whether it actually lands
// in an IOSurface; going through Metal explicitly guarantees the upload-to-
// texture and the compositing/scaling are both genuinely GPU-executed) and
// forwards touch/keyboard input to it as RFB Pointer/Key events. Replaces
// the WKWebView + vendored noVNC in DisplayViewController.
//
// Fully on-demand: no continuous render loop. -setNeedsDisplay (called by
// DisplayViewController from -rfbClientDidUpdateFramebuffer:) schedules
// exactly one -drawInMTKView: at the next vsync, coalescing any additional
// calls that land before then.
@interface DisplayRFBView : MTKView

// Set once, after the client has connected (framebufferWidth/Height must
// already be known). Reading pixels/sending input both go through this.
@property (nonatomic, nullable) DisplayRFBClient *rfbClient;

// When YES, -inputAccessoryView returns nil and the owner is expected to
// place -accessoryKeyStack in its own view hierarchy instead. Standalone
// Display mode does this: UIKit's keyboard-host window (which hosts a real
// inputAccessoryView) repositions the bar to clear the home indicator
// whenever the indicator becomes visible -- and never lowers it again once
// the indicator auto-hides -- so the only way to keep the strip genuinely
// flush at the physical bottom of a fullscreen surface is to own its
// placement outright.
@property (nonatomic) BOOL accessoryBarExternallyHosted;

// Taking keyboard focus brings up the software keyboard when no hardware one
// is attached, so a touch takes it only when it may: always with a hardware
// keyboard (keys need a first responder), otherwise only while Auto-Show
// Keyboard is on and the keyboard has not been put away from the menu
// (keyboardPutAway, cleared by the menu's Show Keyboard). Every tap used to
// take it, so on an iPad with no keyboard the software one came back on each
// click in the desktop.
@property (nonatomic) BOOL keyboardPutAway;
- (void)takeKeyboardFocusIfWanted;

// Frames straight from the guest for the desktop whose VNC port is `display`
// (wl-present, fs/virtgpu.h). While they flow, directFrames is YES: the view
// shows them instead of the RFB client's pixels, pauses the client's
// framebuffer updates (VNC then carries only input), maps the pointer to
// their size, and leaves the cursor to them (they have it drawn in). When the
// presenter goes away the view goes back to VNC's pixels.
- (void)startDirectFramesForDisplay:(uint32_t)display;
- (void)stopDirectFrames;
@property (nonatomic, readonly) BOOL directFrames;

// The accessory key row (esc/tab/ctrl/alt/super/arrows) alone, no container,
// lazily built. For accessoryBarExternallyHosted use: the owner embeds this
// in its own container view with its own constraints. Deliberately NOT the
// same UIInputView -inputAccessoryView vends -- allowsSelfSizing and
// safeAreaLayoutGuide-driven implicit sizing only work for a REAL
// inputAccessoryView hosted by UIKit; reusing that container as a plain
// subview left its height ambiguous, which silently broke touch delivery to
// the rest of the view underneath it (2026-07-24).
- (UIStackView *)accessoryKeyStack;

- (instancetype)initWithFrame:(CGRect)frameRect;

// Forward DisplayRFBClientDelegate's cursor callback here. Renders the
// cursor as a small overlay positioned at the last coordinates sent via
// touch input (RFB doesn't push cursor position, only shape/hotspot).
- (void)updateCursorWithWidth:(uint16_t)width height:(uint16_t)height
                      hotspotX:(uint16_t)hotspotX hotspotY:(uint16_t)hotspotY bgra:(NSData *)bgra;

@end

NS_ASSUME_NONNULL_END
