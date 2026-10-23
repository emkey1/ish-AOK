#import "DisplayRFBView.h"
#import "BarButton.h"
#import "UserPreferences.h"
#import <GameController/GameController.h>
#include <stdatomic.h>
#include "fs/virtgpu.h"

NS_ASSUME_NONNULL_BEGIN

@interface DisplayRFBView ()
- (void)directFrameArrivedWithWidth:(uint32_t)width height:(uint32_t)height;
- (void)directFramesEnded;
@end

// Takes wl-present's frames on the guest thread presenting them, so it holds
// no strong reference to the view (whose last release there would run UIKit
// teardown off the main thread): the frames go into a texture of its own, and
// the view is told on the main thread.
@interface DisplayDirectFrames : NSObject {
  @public
    id<MTLDevice> _device;
    id<MTLCommandQueue> _queue;     // the view's: drawing then follows each blit
    id<MTLTexture> _Nullable _texture;  // @synchronized(self)
    uint32_t _display;
    atomic_bool _visible;
}
@property (weak, nullable) DisplayRFBView *view;
@end

@implementation DisplayDirectFrames

- (id<MTLTexture> _Nullable)texture {
    @synchronized (self) {
        return _texture;
    }
}

- (BOOL)presentFrame:(const struct virtgpu_frame *)frame {
    if (!atomic_load(&_visible))
        return NO;
    if (frame->width == 0 || frame->height == 0 || frame->width > 16384 || frame->height > 16384 ||
            frame->stride < frame->width * 4 || (size_t) frame->stride * frame->height > frame->size)
        return NO;
    // The shared memory itself, no copy: page-aligned and whole pages (see
    // struct virtgpu_frame), and alive until this returns.
    id<MTLBuffer> buffer = [_device newBufferWithBytesNoCopy:frame->pixels
                                                      length:frame->size
                                                     options:MTLResourceStorageModeShared
                                                 deallocator:nil];
    if (buffer == nil)
        return NO;
    id<MTLTexture> texture;
    BOOL whole = NO;
    @synchronized (self) {
        if (_texture == nil || _texture.width != frame->width || _texture.height != frame->height) {
            MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                                    width:frame->width
                                                                                                   height:frame->height
                                                                                                mipmapped:NO];
            descriptor.usage = MTLTextureUsageShaderRead;
            descriptor.storageMode = MTLStorageModePrivate;
            _texture = [_device newTextureWithDescriptor:descriptor];
            whole = YES;
        }
        texture = _texture;
    }
    if (texture == nil)
        return NO;
    NSUInteger x = 0, y = 0, w = frame->width, h = frame->height;
    if (!whole && frame->damage_width > 0 && frame->damage_height > 0 &&
            frame->damage_x >= 0 && frame->damage_y >= 0 &&
            (uint32_t) frame->damage_x < frame->width && (uint32_t) frame->damage_y < frame->height) {
        x = (NSUInteger) frame->damage_x;
        y = (NSUInteger) frame->damage_y;
        w = MIN((NSUInteger) frame->damage_width, frame->width - x);
        h = MIN((NSUInteger) frame->damage_height, frame->height - y);
    }
    id<MTLCommandBuffer> commandBuffer = [_queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [commandBuffer blitCommandEncoder];
    [blit copyFromBuffer:buffer
            sourceOffset:y * frame->stride + x * 4
       sourceBytesPerRow:frame->stride
     sourceBytesPerImage:frame->stride * h
              sourceSize:MTLSizeMake(w, h, 1)
               toTexture:texture
        destinationSlice:0
        destinationLevel:0
       destinationOrigin:MTLOriginMake(x, y, 0)];
    [blit endEncoding];
    [commandBuffer commit];
    [commandBuffer waitUntilCompleted];
    if (commandBuffer.status != MTLCommandBufferStatusCompleted)
        return NO; // e.g. the app went to the background: GPU work is refused there
    uint32_t width = frame->width, height = frame->height;
    dispatch_async(dispatch_get_main_queue(), ^{
        [self.view directFrameArrivedWithWidth:width height:height];
    });
    return YES;
}

- (void)clipboardText:(NSString *)text {
    dispatch_async(dispatch_get_main_queue(), ^{
        [self.view.rfbClient deliverServerCutText:text];
    });
}

- (void)presenterGone {
    @synchronized (self) {
        _texture = nil;
    }
    dispatch_async(dispatch_get_main_queue(), ^{
        [self.view directFramesEnded];
    });
}

@end

// virtgpu_present_fn; ctx is a DisplayDirectFrames the hook's registration
// holds a reference to.
static int display_direct_frame(const struct virtgpu_frame *frame, void *ctx) {
    DisplayDirectFrames *sink = (__bridge DisplayDirectFrames *) ctx;
    // A guest thread has no pool of its own: without one, every Metal object
    // made here would live until the thread exits.
    @autoreleasepool {
        if (frame == NULL) {
            [sink presenterGone];
            return 0;
        }
        return [sink presentFrame:frame] ? 0 : 1;
    }
}

static void display_direct_clipboard(const char *text, size_t len, void *ctx) {
    DisplayDirectFrames *sink = (__bridge DisplayDirectFrames *) ctx;
    @autoreleasepool {
        NSString *string = [[NSString alloc] initWithBytes:text length:len encoding:NSUTF8StringEncoding];
        if (string != nil)
            [sink clipboardText:string];
    }
}

static const struct virtgpu_present_ops display_direct_ops = {
    .frame = display_direct_frame,
    .clipboard = display_direct_clipboard,
};

@interface DisplayRFBView () <MTKViewDelegate, UIKeyInput>
// The texture the source has already uploaded this frame; what a mirror draws.
@property (nonatomic, readonly, nullable) id<MTLTexture> framebufferTexture;
- (void)drawTexture:(nullable id<MTLTexture>)texture;
- (void)adoptCursorStateFromMirrorSource;
// Shared body of both public initializers; nil source means "not a mirror".
- (instancetype)initWithFrame:(CGRect)frameRect mirrorSource:(nullable DisplayRFBView *)sourceView;
@end

@implementation DisplayRFBView {
    id<MTLCommandQueue> _commandQueue;
    id<MTLRenderPipelineState> _pipelineState;
    id<MTLTexture> _Nullable _texture;
    DisplayDirectFrames *_Nullable _direct;
    uint32_t _directWidth, _directHeight;
    BOOL _hasCursor;    // the RFB server gave a cursor shape to overlay
    BOOL _textureStale; // _texture missed updates while direct frames showed
    NSMutableArray<UIKeyCommand *> *_Nullable _keyCommands;
    // Hardware keys held down, by HID usage, with the keysym each went down
    // as: its release sends the same keysym whatever the modifiers are by then.
    NSMutableDictionary<NSNumber *, NSNumber *> *_Nullable _heldKeys;

    UIImageView *_Nullable _cursorView;
    CGSize _cursorImageSize;
    CGPoint _cursorHotspot;
    CGPoint _lastPointerViewPoint; // last touch location, in this view's own coordinate space
    BOOL _havePointerPoint;        // ...once there has been one

    // Touchpad mode (UserPreferences displayTouchpadMode, GH #628): fingers
    // move the pointer from where it is instead of putting it under them.
    // One gesture runs from the first finger down to the last one up.
    NSMutableArray<UITouch *> *_Nullable _tpTouches; // fingers down now
    CGPoint _tpLastCentroid;         // where their centre was at the last event
    NSTimeInterval _tpLastEventTime; // ...and when
    NSTimeInterval _tpBeganAt;       // the first finger's touch-down
    CGFloat _tpTravel;               // how far the centre has moved, in points
    NSUInteger _tpMaxFingers;        // the most fingers down at once
    BOOL _tpDragArmed;               // began right after a tap: moving it drags
    BOOL _tpDragging;                // the left button is held for that drag
    NSTimeInterval _tpLastTapAt;     // when the last one-finger tap ended
    CGFloat _tpScrollX, _tpScrollY;  // two-finger travel not yet sent as wheel steps
    // The same position in framebuffer coordinates. A mirror has no pointer
    // input of its own and a different size, so this is what it scales into
    // its own bounds to place the cursor overlay.
    CGPoint _lastPointerFramebufferPoint;
    CGSize _lastLaidOutSize; // for sizeDidChange

    // Accessory key strip shown above/instead of the soft keyboard (the Display
    // surface is its own first responder, so TerminalViewController's bar never
    // applies here). Modifier keys latch one-shot: they hold their keysym down
    // until the next key/character is sent, then release.
    UIInputView *_Nullable _accessoryBar;
    UIStackView *_Nullable _accessoryKeyStack; // externally-hosted equivalent of _accessoryBar; see -accessoryKeyStack
    NSArray<BarButton *> *_Nullable _accessoryModifierKeys;
    // Last value -inputAccessoryView would have returned nil-ness for, so
    // -hardwareKeyboardDidChange: can tell a real change from a no-op one.
    // See that method for why this matters.
    BOOL _accessoryBarHidden;
}

- (instancetype)initWithFrame:(CGRect)frameRect {
    return [self initWithFrame:frameRect mirrorSource:nil];
}

- (instancetype)initWithFrame:(CGRect)frameRect mirroringSourceView:(DisplayRFBView *)sourceView {
    return [self initWithFrame:frameRect mirrorSource:sourceView];
}

- (instancetype)initWithFrame:(CGRect)frameRect mirrorSource:(nullable DisplayRFBView *)sourceView {
    // The mirror must render on the SAME device as its source, or the source's
    // texture is not legal to sample from here.
    id<MTLDevice> device = sourceView != nil ? sourceView.device : MTLCreateSystemDefaultDevice();
    self = [super initWithFrame:frameRect device:device];
    if (self != nil) {
        _mirrorSourceView = sourceView;
        self.translatesAutoresizingMaskIntoConstraints = NO;
        self.delegate = self;
        // Fully on-demand rendering: draw only when a frame actually
        // arrived, not on a continuous display-link tick. setNeedsDisplay
        // coalesces any calls that land within the same vsync window into a
        // single -drawInMTKView:, which is the mechanism by which this
        // should beat noVNC's per-rect canvas putImageData in a WebView.
        self.enableSetNeedsDisplay = YES;
        self.paused = YES;
        self.framebufferOnly = YES;
        self.backgroundColor = UIColor.blackColor;
        self.layer.cornerRadius = 10.0;
        self.layer.masksToBounds = YES;
        self.multipleTouchEnabled = NO;
        // ...unless fingers are a touchpad, whose scroll and right-click take
        // two. The menu's toggle writes the preference; this follows it.
        [self applyTouchInputPreference];
        [NSNotificationCenter.defaultCenter addObserver:self
                                               selector:@selector(touchInputPreferenceDidChange:)
                                                   name:NSUserDefaultsDidChangeNotification
                                                 object:nil];
        // Without this, iOS reserves space above the accessory bar for its own
        // (never-shown, since we have no text field) predictive-text strip
        // whenever a hardware keyboard is attached, leaving a visible gap
        // between the accessory bar and the true bottom edge (same fix as
        // TerminalView.m, discovered independently here).
        self.inputAssistantItem.leadingBarButtonGroups = @[];
        self.inputAssistantItem.trailingBarButtonGroups = @[];
        // Trackpad/mouse movement without a button held never generates
        // touch events at all (only click-and-drag does, via iPadOS's
        // touch-compatibility synthesis) -- this is the only way to track
        // the remote cursor to plain hover movement.
        [self addGestureRecognizer:[[UIHoverGestureRecognizer alloc] initWithTarget:self action:@selector(handleHover:)]];

        // The accessory strip is suppressed while a hardware keyboard is attached and
        // the user asked for that (Settings -> External Keyboard); connect/disconnect
        // has to re-evaluate it or the decision goes stale (same lesson as the
        // terminal's bar, GH #520).
        if (@available(iOS 14.0, *)) {
            [NSNotificationCenter.defaultCenter addObserver:self
                                                   selector:@selector(hardwareKeyboardDidChange:)
                                                       name:GCKeyboardDidConnectNotification
                                                     object:nil];
            [NSNotificationCenter.defaultCenter addObserver:self
                                                   selector:@selector(hardwareKeyboardDidChange:)
                                                       name:GCKeyboardDidDisconnectNotification
                                                     object:nil];
        }

        _commandQueue = [device newCommandQueue];
        id<MTLLibrary> library = [device newDefaultLibrary];
        id<MTLFunction> vertexFn = [library newFunctionWithName:@"display_rfb_vertex"];
        id<MTLFunction> fragmentFn = [library newFunctionWithName:@"display_rfb_fragment"];
        // A nil function here (e.g. DisplayRFBShaders.metal not actually
        // compiled into this bundle's default.metallib) produces an invalid
        // MTLRenderPipelineDescriptor -- Metal API Validation treats that as
        // a hard abort, not a graceful nil+error return, so this must be
        // caught before ever calling newRenderPipelineStateWithDescriptor:.
        if (vertexFn != nil && fragmentFn != nil) {
            MTLRenderPipelineDescriptor *pipelineDescriptor = [MTLRenderPipelineDescriptor new];
            pipelineDescriptor.vertexFunction = vertexFn;
            pipelineDescriptor.fragmentFunction = fragmentFn;
            pipelineDescriptor.colorAttachments[0].pixelFormat = self.colorPixelFormat;
            NSError *error = nil;
            _pipelineState = [device newRenderPipelineStateWithDescriptor:pipelineDescriptor error:&error];
        }
        if (sourceView != nil) {
            // Purely a rendering surface: it lives on a non-interactive scene
            // that can never be key, so it can neither be first responder nor
            // receive touches -- and letting it try would only compete with
            // the source view for the pointer state they share.
            self.userInteractionEnabled = NO;
            // Rounded corners make sense inside a Workspace window; on a
            // display filled edge-to-edge they would just notch the desktop.
            self.layer.cornerRadius = 0.0;
            [self adoptCursorStateFromMirrorSource];
        }
    }
    return self;
}

// What a mirror draws: whatever this view is showing now -- the GPU's direct
// frames while they come, the RFB client's framebuffer otherwise.
- (nullable id<MTLTexture>)framebufferTexture {
    if (_directFrames)
        return [_direct texture];
    return _texture;
}

#pragma mark - MTKViewDelegate

- (void)mtkView:(MTKView *)view drawableSizeWillChange:(CGSize)size {
    (void) view;
    (void) size;
}

- (void)layoutSubviews {
    [super layoutSubviews];
    // The cursor overlay is placed in view coordinates scaled from the
    // framebuffer, so a size change (a resized Workspace window, or a mirror
    // taking on the external display's geometry) invalidates its frame.
    [self repositionCursor];
    if (!CGSizeEqualToSize(self.bounds.size, _lastLaidOutSize)) {
        _lastLaidOutSize = self.bounds.size;
        if (self.sizeDidChange != nil)
            self.sizeDidChange();
    }
}

- (void)drawInMTKView:(MTKView *)view {
    DisplayRFBView *mirrorSource = self.mirrorSourceView;
    if (mirrorSource != nil) {
        // Draw the source's texture as-is. No client read and -- crucially --
        // no acknowledge: the source owns that handshake, and a second
        // acknowledge would release the client to overwrite its buffer while
        // the source was still uploading out of it.
        [self drawTexture:mirrorSource.framebufferTexture];
        return;
    }
    if (_directFrames) {
        // An update already on its way when VNC was paused: dropped, but
        // acknowledged, since the client reads nothing more (clipboard,
        // cursor shapes) until it is.
        [_rfbClient acknowledgeFramebufferRead];
        id<MTLTexture> texture = [_direct texture];
        if (texture != nil) {
            [self drawTexture:texture];
            // The same frame on the second screen, now, for the reason given
            // below the RFB upload.
            [self.mirrorView drawTexture:texture];
        }
        return;
    }
    DisplayRFBClient *client = _rfbClient;
    uint16_t width = client.framebufferWidth;
    uint16_t height = client.framebufferHeight;
    const uint8_t *bytes = client.framebufferBytes;
    if (client == nil || width == 0 || height == 0 || bytes == NULL)
        return;

    [self ensureTextureWithWidth:width height:height];

    // Upload only what actually changed -- client.dirtyRect is the bounding
    // box of every rect in the completed update, which for typical "mostly
    // static desktop, small localized change" traffic (cursor blink, a text
    // cursor, a small window redraw) is far smaller than the full
    // framebuffer. bytesPerRow stays the FULL row stride even though we're
    // uploading a sub-region -- replaceRegion reads regionWidth*4 bytes per
    // row and advances by bytesPerRow between rows, so this reads a strided
    // sub-rectangle straight out of the full buffer with no extra copy.
    CGRect dirty = CGRectIntegral(CGRectIntersection(client.dirtyRect, CGRectMake(0, 0, width, height)));
    if (_textureStale) {
        _textureStale = NO;
        dirty = CGRectMake(0, 0, width, height);
    }
    if (CGRectIsNull(dirty) || CGRectIsEmpty(dirty)) {
        // Nothing in the texture actually changed (e.g. an update that was
        // purely a cursor rect, handled separately as an overlay) -- no GPU
        // work needed, just keep the RFB pull loop going.
        [client acknowledgeFramebufferRead];
        return;
    }
    size_t stride = (size_t) width * 4;
    NSUInteger originX = (NSUInteger) dirty.origin.x;
    NSUInteger originY = (NSUInteger) dirty.origin.y;
    const uint8_t *regionStart = bytes + (size_t) originY * stride + (size_t) originX * 4;
    MTLRegion region = MTLRegionMake2D(originX, originY, (NSUInteger) dirty.size.width, (NSUInteger) dirty.size.height);
    [_texture replaceRegion:region mipmapLevel:0 withBytes:regionStart bytesPerRow:stride];
    // Safe the instant replaceRegion returns: Metal has its own copy of the
    // pixel data by then, so the client can start overwriting its buffer
    // with the next update immediately -- no need to wait for the encode/
    // present below to actually finish on the GPU.
    [client acknowledgeFramebufferRead];
    [self drawTexture:_texture];
    // Same frame, second screen -- drawn synchronously, NOT via setNeedsDisplay.
    // The two views share one texture, and the acknowledge above has already
    // released the client to send the next update: a mirror that waited for its own
    // vsync would sample a texture that was already part-way into the following
    // frame, which tears visibly (seen as a sheared image on a full-framebuffer
    // update). Encoding here keeps its frame identical to the one just drawn
    // locally.
    [self.mirrorView drawTexture:_texture];
}

- (void)drawTexture:(nullable id<MTLTexture>)texture {
    if (_pipelineState == nil || texture == nil)
        return; // shader pipeline failed to build; keep the RFB session alive without rendering

    id<CAMetalDrawable> drawable = self.currentDrawable;
    MTLRenderPassDescriptor *pass = self.currentRenderPassDescriptor;
    if (drawable == nil || pass == nil)
        return;
    id<MTLCommandBuffer> commandBuffer = [_commandQueue commandBuffer];
    id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:_pipelineState];
    [encoder setFragmentTexture:texture atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    [encoder endEncoding];
    [commandBuffer presentDrawable:drawable];
    [commandBuffer commit];
}

- (void)ensureTextureWithWidth:(uint16_t)width height:(uint16_t)height {
    if (_texture != nil && _texture.width == width && _texture.height == height)
        return;
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                            width:width
                                                                                           height:height
                                                                                        mipmapped:NO];
    descriptor.usage = MTLTextureUsageShaderRead;
    descriptor.storageMode = MTLStorageModeShared;
    _texture = [self.device newTextureWithDescriptor:descriptor];
}

#pragma mark - Direct frames

- (void)startDirectFramesForDisplay:(uint32_t)display {
    if (_direct != nil && _direct->_display == display)
        return;
    [self stopDirectFrames];
    DisplayDirectFrames *sink = [DisplayDirectFrames new];
    sink->_device = self.device;
    sink->_queue = _commandQueue;
    sink->_display = display;
    atomic_init(&sink->_visible, [self directVisible]);
    sink.view = self;
    _direct = sink;
    virtgpu_set_present_hook(display, &display_direct_ops, (__bridge_retained void *) sink);
    // Per scene: this app adopts UIScene, and one window may go to the
    // background while another stays.
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(directVisibilityChanged:)
                                               name:UISceneDidEnterBackgroundNotification object:nil];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(directVisibilityChanged:)
                                               name:UISceneWillEnterForegroundNotification object:nil];
}

- (void)stopDirectFrames {
    if (_direct == nil)
        return;
    [self detachDirectFrames];
    [self directFramesEnded];
}

- (void)detachDirectFrames {
    DisplayDirectFrames *sink = _direct;
    _direct = nil;
    [NSNotificationCenter.defaultCenter removeObserver:self name:UISceneDidEnterBackgroundNotification object:nil];
    [NSNotificationCenter.defaultCenter removeObserver:self name:UISceneWillEnterForegroundNotification object:nil];
    // Waits out a frame in progress; after it, nothing calls the sink again.
    virtgpu_clear_present_hook(sink->_display, (__bridge void *) sink);
    CFRelease((__bridge CFTypeRef) sink);
}

// Nobody sees frames with this window in the background (and Metal refuses
// the work when the whole app is): wl-present is told so, and captures less
// often.
- (BOOL)directVisible {
    UIWindowScene *scene = self.window.windowScene;
    return scene != nil && scene.activationState != UISceneActivationStateBackground;
}

- (void)directVisibilityChanged:(NSNotification *)note {
    if (_direct == nil || note.object != self.window.windowScene)
        return;
    BOOL background = [note.name isEqualToString:UISceneDidEnterBackgroundNotification];
    atomic_store(&_direct->_visible, !background);
}

- (void)didMoveToWindow {
    [super didMoveToWindow];
    if (_direct != nil)
        atomic_store(&_direct->_visible, [self directVisible]);
}

- (void)directFrameArrivedWithWidth:(uint32_t)width height:(uint32_t)height {
    if (_direct == nil)
        return; // stopped since
    _directWidth = width;
    _directHeight = height;
    if (!_directFrames) {
        _directFrames = YES;
        _rfbClient.framebufferUpdatesPaused = YES;
        _cursorView.hidden = YES;
        [self.mirrorView adoptCursorStateFromMirrorSource];
    }
    [self setNeedsDisplay];
}

- (void)directFramesEnded {
    if (!_directFrames)
        return;
    _directFrames = NO;
    _textureStale = YES;
    _rfbClient.framebufferUpdatesPaused = NO;
    if (_hasCursor) {
        _cursorView.hidden = NO;
        [self repositionCursor];
    }
    [self.mirrorView adoptCursorStateFromMirrorSource];
    [self setNeedsDisplay];
}

// The desktop's size in pixels: the direct frames' while they come, the RFB
// client's otherwise.
- (CGSize)desktopPixelSize {
    if (_directFrames && _directWidth != 0)
        return CGSizeMake(_directWidth, _directHeight);
    return CGSizeMake(_rfbClient.framebufferWidth, _rfbClient.framebufferHeight);
}

- (void)dealloc {
    if (_direct != nil)
        [self detachDirectFrames];
}

#pragma mark - Pointer input

static BOOL DisplayHardwareKeyboardAttached(void) {
    if (@available(iOS 14.0, *))
        return GCKeyboard.coalescedKeyboard != nil;
    return NO;
}

- (void)takeKeyboardFocusIfWanted {
    if (self.isFirstResponder)
        return;
    if (DisplayHardwareKeyboardAttached() ||
            (UserPreferences.shared.autoShowKeyboard && !self.keyboardPutAway))
        [self becomeFirstResponder];
}

// A touch is a click in the desktop, not a request to type: it takes focus
// only for a hardware keyboard's keys. Taking it otherwise brings up the
// software keyboard, which on an iPad with no keyboard came back on every
// click, Auto-Show or not. The software keyboard comes from Auto-Show when
// the display opens, or from the menu's Show Keyboard.
- (void)takeKeyboardFocusFromTouch {
    if (!self.isFirstResponder && DisplayHardwareKeyboardAttached())
        [self becomeFirstResponder];
}

- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *_Nullable)event {
    [self takeKeyboardFocusFromTouch];
    NSSet<UITouch *> *fingers = [self touchpadFingersIn:touches];
    if (fingers.count != 0)
        [self touchpadFingersDown:fingers];
    if (fingers.count != touches.count)
        [self sendPointerEventFromTouches:[self touches:touches without:fingers] down:YES];
}

- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *_Nullable)event {
    NSSet<UITouch *> *fingers = [self touchpadFingersIn:touches];
    if (fingers.count != 0)
        [self touchpadFingersMoved];
    if (fingers.count != touches.count)
        [self sendPointerEventFromTouches:[self touches:touches without:fingers] down:YES];
}

- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *_Nullable)event {
    NSSet<UITouch *> *fingers = [self touchpadFingersIn:touches];
    if (fingers.count != 0)
        [self touchpadFingersUp:fingers cancelled:NO];
    if (fingers.count != touches.count)
        [self sendPointerEventFromTouches:[self touches:touches without:fingers] down:NO];
}

- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *_Nullable)event {
    NSSet<UITouch *> *fingers = [self touchpadFingersIn:touches];
    if (fingers.count != 0)
        [self touchpadFingersUp:fingers cancelled:YES];
    if (fingers.count != touches.count)
        [self sendPointerEventFromTouches:[self touches:touches without:fingers] down:NO];
}

#pragma mark - Touchpad mode

// RFB's button mask: bits 0-2 are the left, middle and right buttons; 3 and 4
// are one step of the wheel up and down, 5 and 6 left and right.
enum {
    DisplayButtonLeft = 0x01,
    DisplayButtonMiddle = 0x02,
    DisplayButtonRight = 0x04,
    DisplayWheelUp = 0x08,
    DisplayWheelDown = 0x10,
    DisplayWheelLeft = 0x20,
    DisplayWheelRight = 0x40,
};

// How a gesture is read. Points are this view's.
static const CGFloat DisplayTouchpadTapSlop = 8.0;         // a tap moves less than this...
static const NSTimeInterval DisplayTouchpadTapTime = 0.35; // ...and is over within this
static const NSTimeInterval DisplayTouchpadDragWindow = 0.30; // a touch this soon after a tap drags
static const CGFloat DisplayTouchpadWheelStep = 14.0;      // two-finger travel per wheel step

// The pointer's travel for a finger's: one for one when slow, so a target can
// be approached exactly, and up to three times as far when fast, so the
// screen can be crossed in a stroke.
static CGFloat DisplayTouchpadGain(CGFloat pointsPerSecond) {
    const CGFloat slow = 120.0, fast = 1400.0;
    if (pointsPerSecond <= slow)
        return 1.0;
    if (pointsPerSecond >= fast)
        return 3.0;
    return 1.0 + 2.0 * (pointsPerSecond - slow) / (fast - slow);
}

// Fingers are touchpad input while the preference is on. A mouse or a
// trackpad's pointer already is one, and a pencil points at what it touches:
// both stay as they were.
- (NSSet<UITouch *> *)touchpadFingersIn:(NSSet<UITouch *> *)touches {
    if (!UserPreferences.shared.displayTouchpadMode && _tpTouches.count == 0)
        return [NSSet set];
    NSMutableSet<UITouch *> *fingers = [NSMutableSet set];
    for (UITouch *touch in touches) {
        if (touch.type == UITouchTypeDirect || [_tpTouches containsObject:touch])
            [fingers addObject:touch];
    }
    return fingers;
}

- (NSSet<UITouch *> *)touches:(NSSet<UITouch *> *)touches without:(NSSet<UITouch *> *)fingers {
    NSMutableSet<UITouch *> *rest = [touches mutableCopy];
    [rest minusSet:fingers];
    return rest;
}

// (Posted on whichever thread wrote the default.)
- (void)touchInputPreferenceDidChange:(NSNotification *)note {
    dispatch_async(dispatch_get_main_queue(), ^{
        [self applyTouchInputPreference];
    });
}

// Two-finger gestures need the second finger delivered; the direct mode's
// one-touch stream is what it has always been.
- (void)applyTouchInputPreference {
    BOOL touchpad = UserPreferences.shared.displayTouchpadMode;
    if (self.multipleTouchEnabled != touchpad)
        self.multipleTouchEnabled = touchpad;
}

- (CGPoint)touchpadCentroid {
    CGPoint sum = CGPointZero;
    for (UITouch *touch in _tpTouches) {
        CGPoint p = [touch locationInView:self];
        sum.x += p.x;
        sum.y += p.y;
    }
    NSUInteger n = MAX((NSUInteger) 1, _tpTouches.count);
    return CGPointMake(sum.x / n, sum.y / n);
}

// Where the pointer is, for a gesture that moves it from there: where the
// last event of any kind left it, or the middle of the desktop before any.
- (CGPoint)touchpadPointer {
    if (!_havePointerPoint)
        return CGPointMake(CGRectGetMidX(self.bounds), CGRectGetMidY(self.bounds));
    return _lastPointerViewPoint;
}

- (void)touchpadSendButtons:(uint8_t)buttons {
    [self sendPointerEventAtViewPoint:[self touchpadPointer] buttonMask:buttons];
}

- (void)touchpadClick:(uint8_t)button {
    uint8_t held = _tpDragging ? DisplayButtonLeft : 0;
    [self touchpadSendButtons:held | button];
    [self touchpadSendButtons:held];
}

- (void)touchpadFingersDown:(NSSet<UITouch *> *)fingers {
    if (_tpTouches == nil)
        _tpTouches = [NSMutableArray array];
    NSTimeInterval now = fingers.anyObject.timestamp;
    BOOL first = _tpTouches.count == 0;
    for (UITouch *touch in fingers) {
        if (![_tpTouches containsObject:touch])
            [_tpTouches addObject:touch];
    }
    if (first) {
        _tpBeganAt = now;
        _tpTravel = 0;
        _tpMaxFingers = 0;
        _tpScrollX = _tpScrollY = 0;
        _tpDragging = NO;
        // Tap, then touch again at once and move: the second touch drags
        // with the button held, as a laptop's touchpad has it.
        _tpDragArmed = _tpLastTapAt != 0 && now - _tpLastTapAt < DisplayTouchpadDragWindow;
    }
    _tpMaxFingers = MAX(_tpMaxFingers, _tpTouches.count);
    // A finger joining moves the centre without anything having moved.
    _tpLastCentroid = [self touchpadCentroid];
    _tpLastEventTime = now;
}

- (void)touchpadFingersMoved {
    if (_tpTouches.count == 0)
        return;
    CGPoint centroid = [self touchpadCentroid];
    NSTimeInterval now = _tpTouches.lastObject.timestamp;
    for (UITouch *touch in _tpTouches)
        now = MAX(now, touch.timestamp);
    CGFloat dx = centroid.x - _tpLastCentroid.x;
    CGFloat dy = centroid.y - _tpLastCentroid.y;
    CGFloat distance = hypot(dx, dy);
    if (distance == 0)
        return;
    NSTimeInterval elapsed = now - _tpLastEventTime;
    _tpLastCentroid = centroid;
    _tpLastEventTime = now;
    BOOL wasStill = _tpTravel < DisplayTouchpadTapSlop;
    _tpTravel += distance;

    if (_tpMaxFingers >= 2) {
        // Two fingers scroll, and the content follows them: down the screen
        // is toward the top of the document, which is the wheel turned up.
        _tpScrollX += dx;
        _tpScrollY += dy;
        while (fabs(_tpScrollY) >= DisplayTouchpadWheelStep) {
            BOOL down = _tpScrollY > 0;
            [self touchpadClick:down ? DisplayWheelUp : DisplayWheelDown];
            _tpScrollY -= down ? DisplayTouchpadWheelStep : -DisplayTouchpadWheelStep;
        }
        while (fabs(_tpScrollX) >= DisplayTouchpadWheelStep) {
            BOOL right = _tpScrollX > 0;
            [self touchpadClick:right ? DisplayWheelLeft : DisplayWheelRight];
            _tpScrollX -= right ? DisplayTouchpadWheelStep : -DisplayTouchpadWheelStep;
        }
        return;
    }

    // Within a tap's slop nothing moves, so that a tap lands where the
    // pointer was and not a few pixels off it.
    if (_tpTravel < DisplayTouchpadTapSlop)
        return;
    if (wasStill) {
        if (_tpDragArmed && !_tpDragging) {
            _tpDragging = YES;
            [self touchpadSendButtons:DisplayButtonLeft];
        }
        // The slop itself is not travel: start from here.
        return;
    }
    CGFloat gain = DisplayTouchpadGain(elapsed > 0 ? distance / elapsed : 0);
    CGPoint pointer = [self touchpadPointer];
    pointer.x = MAX(0.0, MIN(CGRectGetWidth(self.bounds) - 1, pointer.x + dx * gain));
    pointer.y = MAX(0.0, MIN(CGRectGetHeight(self.bounds) - 1, pointer.y + dy * gain));
    [self sendPointerEventAtViewPoint:pointer buttonMask:_tpDragging ? DisplayButtonLeft : 0];
}

- (void)touchpadFingersUp:(NSSet<UITouch *> *)fingers cancelled:(BOOL)cancelled {
    NSTimeInterval now = fingers.anyObject.timestamp;
    for (UITouch *touch in fingers)
        [_tpTouches removeObject:touch];
    if (_tpTouches.count != 0) {
        _tpLastCentroid = [self touchpadCentroid];
        return;
    }
    // The last finger is up: the gesture is over.
    if (_tpDragging) {
        _tpDragging = NO;
        [self touchpadSendButtons:0];
        _tpLastTapAt = 0;
        return;
    }
    BOOL tap = !cancelled && _tpTravel < DisplayTouchpadTapSlop && now - _tpBeganAt < DisplayTouchpadTapTime;
    if (!tap) {
        _tpLastTapAt = 0;
        return;
    }
    if (_tpMaxFingers >= 3) {
        [self touchpadClick:DisplayButtonMiddle];
        _tpLastTapAt = 0;
    } else if (_tpMaxFingers == 2) {
        [self touchpadClick:DisplayButtonRight];
        _tpLastTapAt = 0;
    } else {
        // One finger: a click where the pointer is. A second tap right after
        // it is a second click (a double click); a touch that moves is a drag.
        [self touchpadClick:DisplayButtonLeft];
        _tpLastTapAt = now;
    }
}

- (void)sendPointerEventFromTouches:(NSSet<UITouch *> *)touches down:(BOOL)down {
    UITouch *touch = touches.anyObject;
    if (touch == nil)
        return;
    [self sendPointerEventAtViewPoint:[touch locationInView:self] buttonMask:down ? 0x01 : 0x00];
}

// Shared by both actual touch (finger down/drag) and hover (trackpad/mouse
// moved without a button held -- see -handleHover: below). Without the
// hover path, a connected trackpad/mouse only ever updates the remote
// pointer on click-and-drag (iPadOS synthesizes touch events for that), so
// just moving the pointer around does nothing at all -- the remote cursor
// sits wherever the last click left it instead of tracking live movement.
- (void)sendPointerEventAtViewPoint:(CGPoint)point buttonMask:(uint8_t)buttonMask {
    CGSize desktop = [self desktopPixelSize];
    uint16_t fbWidth = (uint16_t) desktop.width;
    uint16_t fbHeight = (uint16_t) desktop.height;
    if (_rfbClient == nil || fbWidth == 0 || fbHeight == 0)
        return;
    CGSize viewSize = self.bounds.size;
    if (viewSize.width <= 0 || viewSize.height <= 0)
        return;
    CGFloat scaleX = (CGFloat) fbWidth / viewSize.width;
    CGFloat scaleY = (CGFloat) fbHeight / viewSize.height;
    uint16_t x = (uint16_t) MAX(0.0, MIN((CGFloat) (fbWidth - 1), point.x * scaleX));
    uint16_t y = (uint16_t) MAX(0.0, MIN((CGFloat) (fbHeight - 1), point.y * scaleY));
    [_rfbClient sendPointerEventAtX:x y:y buttonMask:buttonMask];
    _lastPointerViewPoint = point;
    _havePointerPoint = YES;
    _lastPointerFramebufferPoint = CGPointMake(x, y);
    [self repositionCursor];
    [self.mirrorView repositionCursor];
}

- (void)handleHover:(UIHoverGestureRecognizer *)recognizer {
    if (recognizer.state != UIGestureRecognizerStateChanged && recognizer.state != UIGestureRecognizerStateBegan)
        return;
    [self sendPointerEventAtViewPoint:[recognizer locationInView:self] buttonMask:0x00];
}

#pragma mark - Cursor overlay

// RFB doesn't push cursor position, only shape/hotspot (via the Cursor
// pseudo-encoding) -- position is tracked here from the same touch-driven
// pointer events we send to the server, so the overlay follows wherever we
// last told the server the pointer is.

- (UIImageView *)cursorView {
    if (_cursorView == nil) {
        _cursorView = [[UIImageView alloc] initWithFrame:CGRectZero];
        _cursorView.hidden = YES;
        _cursorView.userInteractionEnabled = NO; // never steals touches meant for the desktop
        [self addSubview:_cursorView];
    }
    return _cursorView;
}

- (void)updateCursorWithWidth:(uint16_t)width height:(uint16_t)height
                      hotspotX:(uint16_t)hotspotX hotspotY:(uint16_t)hotspotY bgra:(NSData *)bgra {
    if (width == 0 || height == 0) {
        _hasCursor = NO;
        _cursorView.hidden = YES;
        return;
    }
    if (bgra.length != (NSUInteger) width * height * 4)
        return;
    CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef provider = CGDataProviderCreateWithCFData((__bridge CFDataRef) bgra);
    CGImageRef image = CGImageCreate(width, height, 8, 32, (size_t) width * 4, colorSpace,
                                      kCGBitmapByteOrder32Little | kCGImageAlphaPremultipliedFirst,
                                      provider, NULL, false, kCGRenderingIntentDefault);
    CGColorSpaceRelease(colorSpace);
    CGDataProviderRelease(provider);
    if (image == NULL)
        return;
    self.cursorView.image = [UIImage imageWithCGImage:image];
    CGImageRelease(image);
    _cursorImageSize = CGSizeMake(width, height);
    _cursorHotspot = CGPointMake(hotspotX, hotspotY);
    _hasCursor = YES;
    // Direct frames have it drawn in -- the source's, for a mirror, whose own
    // view never receives any.
    DisplayRFBView *shown = self.mirrorSourceView ?: self;
    self.cursorView.hidden = shown->_directFrames;
    [self repositionCursor];
    // The shape only changes occasionally, so a mirror that missed the last
    // one would sit cursorless until the pointer happened to enter something
    // that changes it -- hand every update straight on.
    [self.mirrorView updateCursorWithWidth:width height:height hotspotX:hotspotX hotspotY:hotspotY bgra:bgra];
}

// A mirror created mid-session missed every cursor update so far; copy the
// current shape over rather than waiting for the next one.
- (void)adoptCursorStateFromMirrorSource {
    DisplayRFBView *source = self.mirrorSourceView;
    if (source == nil || source->_cursorView == nil || source->_cursorView.image == nil)
        return;
    self.cursorView.image = source->_cursorView.image;
    _cursorImageSize = source->_cursorImageSize;
    _cursorHotspot = source->_cursorHotspot;
    self.cursorView.hidden = source->_cursorView.hidden;
    [self repositionCursor];
}

- (void)repositionCursor {
    if (_cursorView == nil || _cursorView.hidden)
        return;
    DisplayRFBView *source = self.mirrorSourceView ?: self;
    CGSize desktop = [source desktopPixelSize];
    uint16_t fbWidth = (uint16_t) desktop.width;
    uint16_t fbHeight = (uint16_t) desktop.height;
    CGSize viewSize = self.bounds.size;
    if (fbWidth == 0 || fbHeight == 0 || viewSize.width <= 0 || viewSize.height <= 0)
        return;
    CGFloat scaleX = viewSize.width / (CGFloat) fbWidth;
    CGFloat scaleY = viewSize.height / (CGFloat) fbHeight;
    // Locally the touch point is used directly, which is finer-grained than
    // the integer framebuffer coordinate it was quantized to. A mirror has no
    // touch of its own, so it scales that framebuffer coordinate into its own
    // (differently sized) bounds instead.
    CGPoint pointer = source == self
        ? _lastPointerViewPoint
        : CGPointMake(source->_lastPointerFramebufferPoint.x * scaleX,
                      source->_lastPointerFramebufferPoint.y * scaleY);
    CGSize viewImageSize = CGSizeMake(_cursorImageSize.width * scaleX, _cursorImageSize.height * scaleY);
    CGPoint origin = CGPointMake(pointer.x - _cursorHotspot.x * scaleX,
                                  pointer.y - _cursorHotspot.y * scaleY);
    _cursorView.frame = CGRectMake(origin.x, origin.y, viewImageSize.width, viewImageSize.height);
}

#pragma mark - Keyboard input

- (BOOL)canBecomeFirstResponder {
    return YES;
}

- (BOOL)hasText {
    return YES;
}

// Smart punctuation/autocorrect must be off, exactly like TerminalView: the
// iOS keyboard otherwise rewrites what the user typed BEFORE it reaches
// -insertText: -- most damagingly "--" becomes an em-dash (U+2014), which has
// no Latin-1 keysym and silently vanished in the guest, so any --long-option
// typed into the Wayland desktop lost its dashes ("systemctl enable --now"
// arrived as "enable now"). A lone "-" was never converted, which made the
// symptom look bizarrely selective.
- (UITextSmartDashesType)smartDashesType {
    return UITextSmartDashesTypeNo;
}
- (UITextSmartQuotesType)smartQuotesType {
    return UITextSmartQuotesTypeNo;
}
- (UITextSmartInsertDeleteType)smartInsertDeleteType {
    return UITextSmartInsertDeleteTypeNo;
}
- (UITextAutocapitalizationType)autocapitalizationType {
    return UITextAutocapitalizationTypeNone;
}
- (UITextAutocorrectionType)autocorrectionType {
    return UITextAutocorrectionTypeNo;
}
// Apparently required on iOS 15+ in addition to autocorrectionType (same
// note as TerminalView): https://stackoverflow.com/a/72359764
- (UITextSpellCheckingType)spellCheckingType {
    return UITextSpellCheckingTypeNo;
}
// Follow the app theme like the terminal does, so the soft keyboard matches
// the accessory strip's key styling.
- (UIKeyboardAppearance)keyboardAppearance {
    return UserPreferences.shared.keyboardAppearance;
}

- (void)insertText:(NSString *)text {
    if (_rfbClient == nil)
        return;
    for (NSUInteger i = 0; i < text.length; i++) {
        unichar ch = [text characterAtIndex:i];
        // Typographic characters the iOS text system can still hand us even
        // with the smart-punctuation traits off (dictation, pasted-by-iOS
        // autofill, a hardware keyboard's Option layer): fold them back to
        // the ASCII the guest can actually type. An em-dash reconstructs the
        // "--" it was made from.
        if (ch == 0x2014 /* em-dash */) {
            [_rfbClient sendKeyEvent:'-' down:YES];
            [_rfbClient sendKeyEvent:'-' down:NO];
            [_rfbClient sendKeyEvent:'-' down:YES];
            [_rfbClient sendKeyEvent:'-' down:NO];
            continue;
        }
        if (ch == 0x2013 /* en-dash */)
            ch = '-';
        else if (ch == 0x2018 || ch == 0x2019) /* curly single quotes */
            ch = '\'';
        else if (ch == 0x201C || ch == 0x201D) /* curly double quotes */
            ch = '"';
        else if (ch == 0x00A0) /* non-breaking space */
            ch = ' ';
        // X11 keysyms equal the Unicode code point for the printable
        // Latin-1 range, which covers normal typing; anything outside that
        // range is out of scope for v1 (matches typing through a US/Latin-1
        // layout, same practical coverage noVNC's default input path had).
        uint32_t keysym = ch == '\n' ? 0xFF0D /* Return */ : (uint32_t) ch;
        [_rfbClient sendKeyEvent:keysym down:YES];
        [_rfbClient sendKeyEvent:keysym down:NO];
    }
    [self releaseLatchedAccessoryModifiers];
}

- (void)deleteBackward {
    if (_rfbClient == nil)
        return;
    [_rfbClient sendKeyEvent:0xFF08 down:YES]; // BackSpace
    [_rfbClient sendKeyEvent:0xFF08 down:NO];
    [self releaseLatchedAccessoryModifiers];
}

#pragma mark - Hardware keyboard

// A hardware keyboard's keys go to the guest as they are pressed and released,
// modifiers included: a game reads a held arrow as movement, Ctrl alone as fire
// and Shift alone as run, none of which -insertText: or a key command can say
// (each sends a press and its release at once, and neither sees a bare
// modifier). The presses are not passed on, so neither of those paths sees
// them too. Command chords stay with iPadOS and Workspace.

static uint32_t DisplayRFBKeysymForHIDUsage(UIKeyboardHIDUsage usage) {
    switch (usage) {
        case UIKeyboardHIDUsageKeyboardReturnOrEnter: return 0xFF0D;
        case UIKeyboardHIDUsageKeypadEnter: return 0xFF8D;
        case UIKeyboardHIDUsageKeyboardEscape: return 0xFF1B;
        case UIKeyboardHIDUsageKeyboardDeleteOrBackspace: return 0xFF08;
        case UIKeyboardHIDUsageKeyboardTab: return 0xFF09;
        case UIKeyboardHIDUsageKeyboardCapsLock: return 0xFFE5;
        case UIKeyboardHIDUsageKeyboardDeleteForward: return 0xFFFF;
        case UIKeyboardHIDUsageKeyboardInsert: return 0xFF63;
        case UIKeyboardHIDUsageKeyboardHome: return 0xFF50;
        case UIKeyboardHIDUsageKeyboardEnd: return 0xFF57;
        case UIKeyboardHIDUsageKeyboardPageUp: return 0xFF55;
        case UIKeyboardHIDUsageKeyboardPageDown: return 0xFF56;
        case UIKeyboardHIDUsageKeyboardLeftArrow: return 0xFF51;
        case UIKeyboardHIDUsageKeyboardUpArrow: return 0xFF52;
        case UIKeyboardHIDUsageKeyboardRightArrow: return 0xFF53;
        case UIKeyboardHIDUsageKeyboardDownArrow: return 0xFF54;
        case UIKeyboardHIDUsageKeyboardLeftControl: return 0xFFE3;
        case UIKeyboardHIDUsageKeyboardRightControl: return 0xFFE4;
        case UIKeyboardHIDUsageKeyboardLeftShift: return 0xFFE1;
        case UIKeyboardHIDUsageKeyboardRightShift: return 0xFFE2;
        case UIKeyboardHIDUsageKeyboardLeftAlt: return 0xFFE9;
        case UIKeyboardHIDUsageKeyboardRightAlt: return 0xFFEA;
        default: break;
    }
    if (usage >= UIKeyboardHIDUsageKeyboardF1 && usage <= UIKeyboardHIDUsageKeyboardF12)
        return 0xFFBE + (uint32_t) (usage - UIKeyboardHIDUsageKeyboardF1);
    return 0;
}

static BOOL DisplayRFBUsageIsModifier(UIKeyboardHIDUsage usage) {
    return usage >= UIKeyboardHIDUsageKeyboardLeftControl && usage <= UIKeyboardHIDUsageKeyboardRightGUI;
}

// The keysym a key goes down as. Printable keys go as the character they make
// with the modifiers held -- the uppercase letter or "!" with Shift -- since the
// compositor presses a key at the level that makes the keysym it is sent, and
// lifts the held modifiers when that level does not match: a lowercase letter
// under Alt+Shift reached labwc as a bare letter. With Control or Option held, the character iOS
// reports is a control code or an Option-layer symbol, so take the key's plain
// character and apply Shift to letters only.
static uint32_t DisplayRFBKeysymForKey(UIKey *key) {
    uint32_t keysym = DisplayRFBKeysymForHIDUsage(key.keyCode);
    if (keysym != 0)
        return keysym;
    BOOL plain = !(key.modifierFlags & (UIKeyModifierControl | UIKeyModifierAlternate));
    NSString *text = plain ? key.characters : key.charactersIgnoringModifiers;
    if (text.length != 1)
        return 0;
    unichar ch = [text characterAtIndex:0];
    if (!plain && (key.modifierFlags & UIKeyModifierShift) && ch >= 'a' && ch <= 'z')
        ch -= 'a' - 'A';
    if (ch < 0x20 || ch == 0x7F)
        return 0;
    // Latin-1 keysyms are the code point; the rest are 0x01000000 + it.
    return ch <= 0xFF ? (uint32_t) ch : 0x01000000u + ch;
}

- (BOOL)forwardPresses:(NSSet<UIPress *> *)presses down:(BOOL)down {
    if (_rfbClient == nil)
        return NO;
    BOOL all = YES;
    for (UIPress *press in presses) {
        UIKey *key = press.key;
        if (key == nil || (key.modifierFlags & UIKeyModifierCommand) ||
            key.keyCode == UIKeyboardHIDUsageKeyboardLeftGUI || key.keyCode == UIKeyboardHIDUsageKeyboardRightGUI) {
            all = NO;
            continue;
        }
        NSNumber *usage = @(key.keyCode);
        if (down) {
            uint32_t keysym = DisplayRFBKeysymForKey(key);
            if (keysym == 0) {
                all = NO;
                continue;
            }
            if (_heldKeys == nil)
                _heldKeys = [NSMutableDictionary new];
            _heldKeys[usage] = @(keysym);
            [_rfbClient sendKeyEvent:keysym down:YES];
            if (!DisplayRFBUsageIsModifier(key.keyCode))
                [self releaseLatchedAccessoryModifiers];
        } else {
            NSNumber *keysym = _heldKeys[usage];
            if (keysym == nil) {
                all = NO;
                continue;
            }
            [_heldKeys removeObjectForKey:usage];
            [_rfbClient sendKeyEvent:keysym.unsignedIntValue down:NO];
        }
    }
    return all;
}

// Anything still held goes up: focus moving away mid-press would otherwise
// leave a key down in the guest (a game running forever).
- (void)releaseHeldKeys {
    NSDictionary<NSNumber *, NSNumber *> *held = _heldKeys;
    _heldKeys = nil;
    if (_rfbClient == nil)
        return;
    for (NSNumber *keysym in held.allValues)
        [_rfbClient sendKeyEvent:keysym.unsignedIntValue down:NO];
}

- (void)pressesBegan:(NSSet<UIPress *> *)presses withEvent:(nullable UIPressesEvent *)event {
    if (![self forwardPresses:presses down:YES])
        [super pressesBegan:presses withEvent:event];
}

- (void)pressesEnded:(NSSet<UIPress *> *)presses withEvent:(nullable UIPressesEvent *)event {
    if (![self forwardPresses:presses down:NO])
        [super pressesEnded:presses withEvent:event];
}

- (void)pressesCancelled:(NSSet<UIPress *> *)presses withEvent:(nullable UIPressesEvent *)event {
    if (![self forwardPresses:presses down:NO])
        [super pressesCancelled:presses withEvent:event];
}

- (BOOL)resignFirstResponder {
    [self releaseHeldKeys];
    return [super resignFirstResponder];
}

- (nullable NSArray<UIKeyCommand *> *)keyCommands {
    if (_keyCommands != nil)
        return _keyCommands;
    _keyCommands = [NSMutableArray new];
    // Cmd+= / Cmd++ / Cmd+- / Cmd+0: the Apple-conventional zoom chords.
    // Terminal apps in the guest only understand the Ctrl forms (foot's
    // font-increase/decrease/reset are Control+equal/plus/minus/0), so these
    // wrap the key in a guest Ctrl press. Deliberately NOT extended to
    // Cmd+<letter>: translating Cmd+C into guest Ctrl+C would turn a reflexive
    // "copy" into SIGINT. Every other key reaches the guest through
    // -pressesBegan: as it is pressed and released; a key command here would
    // take its press first.
    static const char *commandZoomKeys = "=+-0";
    for (size_t i = 0; commandZoomKeys[i] != '\0'; i++) {
        NSString *key = [NSString stringWithFormat:@"%c", commandZoomKeys[i]];
        UIKeyCommand *zoomCommand = [UIKeyCommand keyCommandWithInput:key
                                                        modifierFlags:UIKeyModifierCommand
                                                               action:@selector(handleZoomKeyCommand:)];
        if (@available(iOS 15, *))
            zoomCommand.wantsPriorityOverSystemBehavior = YES;
        [_keyCommands addObject:zoomCommand];
    }
    return _keyCommands;
}

- (void)handleZoomKeyCommand:(UIKeyCommand *)command {
    NSString *input = command.input;
    if (input.length == 0 || _rfbClient == nil)
        return;
    // Release any bar-latched modifier first so this chord's own wrap doesn't
    // stack on (and then half-release) an already-held modifier.
    [self releaseLatchedAccessoryModifiers];
    static const uint32_t keysymControlL = 0xFFE3;
    uint32_t keysym = (uint32_t) [input characterAtIndex:0];
    [_rfbClient sendKeyEvent:keysymControlL down:YES];
    [_rfbClient sendKeyEvent:keysym down:YES];
    [_rfbClient sendKeyEvent:keysym down:NO];
    [_rfbClient sendKeyEvent:keysymControlL down:NO];
}

#pragma mark - Accessory key strip

// Keysyms for the bar's keys. Super_L is included because Wayland compositors
// (labwc, sway alternates) bind menus/shortcuts to Super, which no iOS soft
// keyboard can produce at all.
static const uint32_t kKeysymEscape = 0xFF1B;
static const uint32_t kKeysymTab = 0xFF09;
static const uint32_t kKeysymControlL = 0xFFE3;
static const uint32_t kKeysymAltL = 0xFFE9;
static const uint32_t kKeysymSuperL = 0xFFEB;
static const uint32_t kKeysymLeft = 0xFF51;
static const uint32_t kKeysymUp = 0xFF52;
static const uint32_t kKeysymDown = 0xFF54;
static const uint32_t kKeysymRight = 0xFF53;

- (UIView *_Nullable)inputAccessoryView {
    // Externally hosted (standalone Display mode): the owner places
    // -accessoryKeyStack itself and manages its visibility -- handing a real
    // accessory view to UIKit's keyboard host too would fight that placement.
    if (self.accessoryBarExternallyHosted)
        return nil;
    if (@available(iOS 14.0, *)) {
        if (GCKeyboard.coalescedKeyboard != nil && UserPreferences.shared.hideExtraKeysWithExternalKeyboard)
            return nil;
    }
    return [self accessoryBar];
}

- (void)hardwareKeyboardDidChange:(NSNotification *)notification {
    // GCKeyboard notifications are not guaranteed to arrive on the main queue.
    dispatch_async(dispatch_get_main_queue(), ^{
        // Externally hosted: the owner observes the same notifications and
        // toggles the strip's visibility itself; reloadInputViews would only
        // churn the (nil) inputAccessoryView.
        if (self.accessoryBarExternallyHosted)
            return;
        BOOL hidden = NO;
        if (@available(iOS 14.0, *)) {
            hidden = GCKeyboard.coalescedKeyboard != nil && UserPreferences.shared.hideExtraKeysWithExternalKeyboard;
        }
        // Bluetooth keyboards nap and reconnect on keypress -- observed to
        // coincide with exactly the moment a user starts typing/interacting,
        // which fires GCKeyboardDidConnectNotification again even though the
        // keyboard was already connected and -inputAccessoryView's return
        // value hasn't actually changed. Unconditionally reloading on every
        // one of those was visibly yanking the accessory bar to a different
        // resting position (and dragging the menu pip's covered-state
        // tracking along with it) for no reason. Mirrors the exact guard
        // TerminalViewController's _updateStyleFromPreferences: already has
        // (only reload when inputAccessoryView would actually differ).
        if (hidden == _accessoryBarHidden)
            return;
        _accessoryBarHidden = hidden;
        if (self.isFirstResponder)
            [self reloadInputViews];
    });
}

- (UIInputView *)accessoryBar {
    if (_accessoryBar != nil)
        return _accessoryBar;

    UIInputView *bar = [[UIInputView alloc] initWithFrame:CGRectMake(0, 0, 0, 56)
                                           inputViewStyle:UIInputViewStyleKeyboard];
    bar.allowsSelfSizing = YES;
    bar.translatesAutoresizingMaskIntoConstraints = YES;
    bar.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;

    UIStackView *stack = [self _buildAccessoryKeyStack];
    [bar addSubview:stack];
    // Pinned to the input view's safe-area guide so the keys stay clear of the home
    // indicator when the bar sits alone at the bottom (hardware keyboard attached);
    // allowsSelfSizing lets these constraints determine the bar's height. This
    // self-sizing behavior is specific to a REAL inputAccessoryView being
    // hosted by UIKit's keyboard window -- see -accessoryKeyStack for the
    // externally-hosted equivalent, which does NOT reuse this container.
    NSArray<NSLayoutConstraint *> *stackConstraints = @[
        [stack.leadingAnchor constraintEqualToAnchor:bar.safeAreaLayoutGuide.leadingAnchor constant:8],
        [stack.trailingAnchor constraintEqualToAnchor:bar.safeAreaLayoutGuide.trailingAnchor constant:-8],
        [stack.topAnchor constraintEqualToAnchor:bar.safeAreaLayoutGuide.topAnchor constant:6],
        [stack.bottomAnchor constraintEqualToAnchor:bar.safeAreaLayoutGuide.bottomAnchor constant:-6],
        [stack.heightAnchor constraintEqualToConstant:44],
    ];
    [NSLayoutConstraint activateConstraints:stackConstraints];

    _accessoryBar = bar;
    return bar;
}

// The button row alone, no container -- shared by -accessoryBar (embedded in
// a real UIInputView, pinned to ITS safeAreaLayoutGuide, self-sized via
// allowsSelfSizing) and -accessoryKeyStack (embedded directly by the owner
// when externally hosted, with no self-sizing trick involved: allowsSelfSizing
// and safeAreaLayoutGuide-driven implicit-height layout are UIInputView/real-
// accessory-hosting behaviors that don't apply to a view added as a plain
// subview -- reusing that same UIInputView as a dangling ordinary subview
// left its height ambiguous, which is what silently broke touch delivery to
// DisplayRFBView underneath it (2026-07-24, see project notes) once it
// stopped being an actual inputAccessoryView. The externally-hosted path
// gets an unambiguous, fully-constrained layout instead (see the owner's
// container setup).
//
// Known minor limitation: this creates a SEPARATE set of buttons (and
// overwrites _accessoryModifierKeys) each time it's called for a NEW
// container. DisplayViewController now toggles between -accessoryBar and
// -accessoryKeyStack live as a hardware keyboard connects/disconnects, so
// after both have been built at least once, _accessoryModifierKeys always
// points at whichever was built LAST, not necessarily whichever is
// currently visible -- a modifier key's latched/highlighted visual state
// could theoretically go stale across a mode switch that happens to land
// mid-latch. Cosmetic only (the RFB modifier keysym itself isn't affected,
// since -releaseLatchedAccessoryModifiers releases by iterating whatever
// _accessoryModifierKeys currently points to); not fixed here to avoid
// scope creep on top of the presentation-mode fix this comment lives next
// to. Fix properly by tracking modifier keys per-stack if this is ever
// visibly hit.
- (UIStackView *)_buildAccessoryKeyStack {
    BarButton *ctrlKey = [self accessoryModifierKeyWithTitle:@"ctrl" label:NSLocalizedString(@"Control", @"Accessibility label for the Control key on the Wayland keyboard bar") keysym:kKeysymControlL];
    BarButton *altKey = [self accessoryModifierKeyWithTitle:@"alt" label:NSLocalizedString(@"Alt", @"Accessibility label for the Alt key on the Wayland keyboard bar") keysym:kKeysymAltL];
    BarButton *superKey = [self accessoryModifierKeyWithTitle:@"❖" label:NSLocalizedString(@"Super", @"Accessibility label for the Super (Windows/logo) key on the Wayland keyboard bar") keysym:kKeysymSuperL];
    _accessoryModifierKeys = @[ctrlKey, altKey, superKey];

    UIStackView *stack = [[UIStackView alloc] initWithArrangedSubviews:@[
        [self accessoryKeyWithTitle:@"esc" label:NSLocalizedString(@"Escape", @"Accessibility label for the Escape key") keysym:kKeysymEscape],
        [self accessoryKeyWithTitle:@"⇥" label:NSLocalizedString(@"Tab", @"Accessibility label for the Tab key") keysym:kKeysymTab],
        ctrlKey,
        altKey,
        superKey,
        [self accessoryKeyWithTitle:@"←" label:NSLocalizedString(@"Left arrow", @"Accessibility label for an arrow key") keysym:kKeysymLeft],
        [self accessoryKeyWithTitle:@"↑" label:NSLocalizedString(@"Up arrow", @"Accessibility label for an arrow key") keysym:kKeysymUp],
        [self accessoryKeyWithTitle:@"↓" label:NSLocalizedString(@"Down arrow", @"Accessibility label for an arrow key") keysym:kKeysymDown],
        [self accessoryKeyWithTitle:@"→" label:NSLocalizedString(@"Right arrow", @"Accessibility label for an arrow key") keysym:kKeysymRight],
    ]];
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    stack.axis = UILayoutConstraintAxisHorizontal;
    stack.distribution = UIStackViewDistributionFillEqually;
    stack.spacing = 6;
    return stack;
}

- (UIStackView *)accessoryKeyStack {
    if (_accessoryKeyStack != nil)
        return _accessoryKeyStack;
    _accessoryKeyStack = [self _buildAccessoryKeyStack];
    return _accessoryKeyStack;
}

// BarButton does its setup in awakeFromNib (it has only ever been built from the
// terminal storyboard); replicate that configuration for programmatic use.
- (BarButton *)accessoryKeyBaseWithTitle:(NSString *)title label:(NSString *)label keysym:(uint32_t)keysym {
    BarButton *key = [BarButton buttonWithType:UIButtonTypeCustom];
    [key setTitle:title forState:UIControlStateNormal];
    key.titleLabel.font = [UIFont systemFontOfSize:18];
    key.layer.cornerRadius = 5;
    key.layer.shadowOffset = CGSizeMake(0, 1);
    key.layer.shadowOpacity = 0.4;
    key.layer.shadowRadius = 0;
    key.accessibilityLabel = label;
    key.accessibilityTraits |= UIAccessibilityTraitKeyboardKey;
    key.tag = (NSInteger) keysym;
    key.keyAppearance = UserPreferences.shared.keyboardAppearance;
    return key;
}

- (BarButton *)accessoryKeyWithTitle:(NSString *)title label:(NSString *)label keysym:(uint32_t)keysym {
    BarButton *key = [self accessoryKeyBaseWithTitle:title label:label keysym:keysym];
    [key addTarget:self action:@selector(accessoryKeyPressed:) forControlEvents:UIControlEventPrimaryActionTriggered];
    return key;
}

- (BarButton *)accessoryModifierKeyWithTitle:(NSString *)title label:(NSString *)label keysym:(uint32_t)keysym {
    BarButton *key = [self accessoryKeyBaseWithTitle:title label:label keysym:keysym];
    key.toggleable = YES;
    [key addTarget:self action:@selector(accessoryModifierToggled:) forControlEvents:UIControlEventPrimaryActionTriggered];
    return key;
}

- (void)accessoryKeyPressed:(BarButton *)sender {
    if (_rfbClient == nil)
        return;
    uint32_t keysym = (uint32_t) sender.tag;
    // Any latched modifiers are still held down here, so this composes with them
    // (Ctrl then Left = Ctrl+Left); they release afterwards, one-shot.
    [_rfbClient sendKeyEvent:keysym down:YES];
    [_rfbClient sendKeyEvent:keysym down:NO];
    [self releaseLatchedAccessoryModifiers];
}

- (void)accessoryModifierToggled:(BarButton *)sender {
    if (_rfbClient == nil)
        return;
    BOOL latch = !sender.selected;
    sender.selected = latch;
    [_rfbClient sendKeyEvent:(uint32_t) sender.tag down:latch];
}

// One-shot semantics (matching the terminal bar's Control key): the latched
// modifier keysyms stay held in the RFB session until the next real key or
// character goes through, then release. Also called from the hardware-keyboard
// combo handlers so a latched bar modifier can't linger under a hardware chord.
- (void)releaseLatchedAccessoryModifiers {
    for (BarButton *key in _accessoryModifierKeys) {
        if (!key.selected)
            continue;
        key.selected = NO;
        [_rfbClient sendKeyEvent:(uint32_t) key.tag down:NO];
    }
}

@end

NS_ASSUME_NONNULL_END
