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

#pragma mark - External display mirroring

// A second view on another screen showing the same framebuffer. Unlike the
// terminal, this view can't simply move: it IS the touch surface that
// generates the RFB pointer events, so it stays on the device and the mirror
// renders alongside it.
//
// The mirror does no protocol I/O at all -- it never reads the client's
// framebuffer and never acknowledges. That contract is deliberate: the read/
// acknowledge pair is what gates the next update onto the wire, so a second
// acknowledger would let the client start overwriting the buffer while the
// other view was still uploading from it. Instead the mirror draws the
// source's already-uploaded MTLTexture, which is safe to share because both
// views render on the same MTLDevice.
- (instancetype)initWithFrame:(CGRect)frameRect mirroringSourceView:(DisplayRFBView *)sourceView;

@property (nonatomic, weak, nullable, readonly) DisplayRFBView *mirrorSourceView; // set on the mirror
@property (nonatomic, weak, nullable) DisplayRFBView *mirrorView;                 // set on the source

// Re-place the cursor overlay against this view's current bounds. Called on
// the mirror by the source, whose pointer position it shares.
- (void)repositionCursor;

// Forward DisplayRFBClientDelegate's cursor callback here. Renders the
// cursor as a small overlay positioned at the last coordinates sent via
// touch input (RFB doesn't push cursor position, only shape/hotspot).
- (void)updateCursorWithWidth:(uint16_t)width height:(uint16_t)height
                      hotspotX:(uint16_t)hotspotX hotspotY:(uint16_t)hotspotY bgra:(NSData *)bgra;

@end

NS_ASSUME_NONNULL_END
