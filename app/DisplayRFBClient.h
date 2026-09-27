#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>

NS_ASSUME_NONNULL_BEGIN

@class DisplayRFBClient;

// All delegate callbacks are delivered on the main queue.
@protocol DisplayRFBClientDelegate <NSObject>
- (void)rfbClientDidConnect:(DisplayRFBClient *)client;
// The framebuffer changed. Read it via -framebufferBytes, do whatever
// GPU upload you need synchronously, then call -acknowledgeFramebufferRead
// before returning -- the client won't let any more pixel data arrive on the
// wire (RFB is request/response: the server can't send an update we didn't
// ask for) until you do, so there is no risk of it rewriting the buffer out
// from under a upload you're still reading from. -dirtyRect (in framebuffer
// pixel coordinates) bounds everything that actually changed -- upload just
// that sub-region instead of the whole framebuffer.
- (void)rfbClientDidUpdateFramebuffer:(DisplayRFBClient *)client;
// The server-side cursor image changed (shape or hotspot) -- bgra is
// width*height*4 bytes, same BGRA8888 memory order as -framebufferBytes but
// with real alpha in the 4th byte (unlike the opaque framebuffer, which has
// meaningless padding there). Declaring Cursor pseudo-encoding support means
// the server composites the cursor client-side instead of redrawing it into
// the framebuffer on every pointer move, so this fires far less often than
// framebuffer updates would have. Position isn't pushed by the protocol --
// track it yourself from the pointer events you send.
- (void)rfbClient:(DisplayRFBClient *)client didUpdateCursorWithWidth:(uint16_t)width height:(uint16_t)height
         hotspotX:(uint16_t)hotspotX hotspotY:(uint16_t)hotspotY bgra:(NSData *)bgra;
- (void)rfbClient:(DisplayRFBClient *)client didReceiveServerCutText:(NSString *)text;
// Fires at most once per connection attempt: a handshake/protocol failure,
// or the connection dropping after having connected. Terminal -- make a new
// instance to reconnect.
- (void)rfbClient:(DisplayRFBClient *)client didFailWithMessage:(NSString *)message;
@end

// A minimal, hand-rolled RFB (VNC) 3.8 client purpose-built for our own
// wayvnc -- not general VNC interop. No auth (security type None only), raw
// encoding (0) only, and a client-dictated 32bpp BGRA8888 pixel format.
// Confirmed against neatvnc's source (server.c on_client_set_pixel_format +
// enc/raw.c) that an explicit true-colour SetPixelFormat request is honored
// verbatim and the raw encoder converts to it, so there's no need to parse
// or convert an arbitrary server-chosen format -- the bytes that arrive on
// the wire for a Raw rect are already BGRA8888, ready to feed straight into
// an MTLTexture with no per-pixel conversion.
//
// Replaces DisplayNetworkBridge + the WS/noVNC/WKWebView stack entirely:
// this connects straight to wayvnc's loopback TCP port (see
// docs/historical/wayland_workspace_plan.md and the harmonic-giggling-aho plan for why).
@interface DisplayRFBClient : NSObject

@property (nonatomic, weak, nullable) id<DisplayRFBClientDelegate> delegate;

@property (atomic, readonly) uint16_t framebufferWidth;
@property (atomic, readonly) uint16_t framebufferHeight;
@property (atomic, readonly, nullable) NSString *desktopName;

// Connects to 127.0.0.1:guestPort (wayvnc). Retries through the same
// loopback-.waiting-is-a-dead-end pattern DisplayNetworkBridge used (wayvnc
// may not be bound yet, or briefly drops its listen socket while its own
// Wayland connection settles).
- (void)connectToGuestPort:(uint16_t)guestPort;

- (void)disconnect;

// BGRA8888, row-major, framebufferWidth * framebufferHeight * 4 bytes. See
// the delegate doc above for the read/acknowledge contract. Returns NULL
// before the first successful connect.
- (nullable const uint8_t *)framebufferBytes;

// Bounding box (in framebuffer pixel coordinates) of everything that changed
// in the most recently completed update. Valid in the same window as
// -framebufferBytes.
@property (atomic, readonly) CGRect dirtyRect;

- (void)acknowledgeFramebufferRead;

#pragma mark - Input

- (void)sendPointerEventAtX:(uint16_t)x y:(uint16_t)y buttonMask:(uint8_t)buttonMask;
- (void)sendKeyEvent:(uint32_t)keysym down:(BOOL)down;
- (void)sendCtrlAltDel;
- (void)sendClientCutText:(NSString *)text;

#pragma mark - Desktop resize

// Ask the server to resize its desktop (RFB SetDesktopSize, client message
// 251). wayvnc implements this for headless outputs by resizing the actual
// compositor output via wlr-output-management -- verified live on-device
// (see docs/wayland_rotation_resize_plan.md): the labwc output really
// changes mode, maximized windows reflow, and the new size arrives back as
// a DesktopSize/ExtendedDesktopSize rect, which this client applies to its
// framebuffer. Purely advisory: when a server doesn't support it, or
// refuses, the framebuffer keeps its size. Not always
// sent at once: the client holds it until the first frame has arrived, any
// earlier request has been answered and no frame is on its way. Sooner crashes
// the wayvnc that Debian 13 and Devuan 6 ship, or garbles the stream (seen
// with wayvnc 0.10.0). Only the latest size asked for is kept, and after a
// "prohibited" answer none are sent.
- (void)requestDesktopSizeWidth:(uint16_t)width height:(uint16_t)height;

@end

NS_ASSUME_NONNULL_END
