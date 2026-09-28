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

    UIImageView *_Nullable _cursorView;
    CGSize _cursorImageSize;
    CGPoint _cursorHotspot;
    CGPoint _lastPointerViewPoint; // last touch location, in this view's own coordinate space

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
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    self = [super initWithFrame:frameRect device:device];
    if (self != nil) {
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
    }
    return self;
}

#pragma mark - MTKViewDelegate

- (void)mtkView:(MTKView *)view drawableSizeWillChange:(CGSize)size {
    (void) view;
    (void) size;
}

- (void)drawInMTKView:(MTKView *)view {
    if (_directFrames) {
        // An update already on its way when VNC was paused: dropped, but
        // acknowledged, since the client reads nothing more (clipboard,
        // cursor shapes) until it is.
        [_rfbClient acknowledgeFramebufferRead];
        id<MTLTexture> texture = [_direct texture];
        if (texture != nil)
            [self drawTexture:texture];
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
}

- (void)drawTexture:(id<MTLTexture>)texture {
    if (_pipelineState == nil)
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

- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *_Nullable)event {
    [self becomeFirstResponder];
    [self sendPointerEventFromTouches:touches down:YES];
}

- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *_Nullable)event {
    [self sendPointerEventFromTouches:touches down:YES];
}

- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *_Nullable)event {
    [self sendPointerEventFromTouches:touches down:NO];
}

- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *_Nullable)event {
    [self sendPointerEventFromTouches:touches down:NO];
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
    [self repositionCursor];
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
    self.cursorView.hidden = _directFrames; // direct frames have it drawn in
    [self repositionCursor];
}

- (void)repositionCursor {
    if (_cursorView == nil || _cursorView.hidden)
        return;
    CGSize desktop = [self desktopPixelSize];
    uint16_t fbWidth = (uint16_t) desktop.width;
    uint16_t fbHeight = (uint16_t) desktop.height;
    CGSize viewSize = self.bounds.size;
    if (fbWidth == 0 || fbHeight == 0 || viewSize.width <= 0 || viewSize.height <= 0)
        return;
    CGFloat scaleX = viewSize.width / (CGFloat) fbWidth;
    CGFloat scaleY = viewSize.height / (CGFloat) fbHeight;
    CGSize viewImageSize = CGSizeMake(_cursorImageSize.width * scaleX, _cursorImageSize.height * scaleY);
    CGPoint origin = CGPointMake(_lastPointerViewPoint.x - _cursorHotspot.x * scaleX,
                                  _lastPointerViewPoint.y - _cursorHotspot.y * scaleY);
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

- (nullable NSArray<UIKeyCommand *> *)keyCommands {
    if (_keyCommands != nil)
        return _keyCommands;
    _keyCommands = [NSMutableArray new];
    [self addSpecialKeyWithInput:UIKeyInputUpArrow keysym:0xFF52];
    [self addSpecialKeyWithInput:UIKeyInputDownArrow keysym:0xFF54];
    [self addSpecialKeyWithInput:UIKeyInputLeftArrow keysym:0xFF51];
    [self addSpecialKeyWithInput:UIKeyInputRightArrow keysym:0xFF53];
    [self addSpecialKeyWithInput:UIKeyInputEscape keysym:0xFF1B];
    [self addSpecialKeyWithInput:@"\t" keysym:0xFF09];
    // The arrows under every mix of Shift, Control and Option -- selection
    // and word moves in programs, and the desktop's Ctrl+Alt+Left/Right
    // (switch desktops) and Ctrl+Alt+Shift+Left/Right (take the window along).
    // Like every modified key, they never arrive at all without a key command
    // of their own. Cmd+arrows stay with Workspace's Desktops around the view.
    static const UIKeyModifierFlags arrowModifiers[] = {
        UIKeyModifierShift, UIKeyModifierControl, UIKeyModifierAlternate,
        UIKeyModifierShift | UIKeyModifierControl, UIKeyModifierShift | UIKeyModifierAlternate,
        UIKeyModifierControl | UIKeyModifierAlternate,
        UIKeyModifierShift | UIKeyModifierControl | UIKeyModifierAlternate,
    };
    NSArray<NSArray *> *arrows = @[@[UIKeyInputUpArrow, @0xFF52], @[UIKeyInputDownArrow, @0xFF54],
                                   @[UIKeyInputLeftArrow, @0xFF51], @[UIKeyInputRightArrow, @0xFF53]];
    for (NSArray *arrow in arrows)
        for (size_t i = 0; i < sizeof(arrowModifiers) / sizeof(arrowModifiers[0]); i++)
            [self addModifiedKeyWithInput:arrow[0] keysym:[arrow[1] unsignedIntValue] modifierFlags:arrowModifiers[i]];
    // Ctrl+<key> (Ctrl+C, Ctrl+D, Ctrl+Z, ...): not covered by UIKeyInput
    // at all -- iOS only routes plain character insertion through
    // -insertText:, not modified combinations, so without an explicit
    // UIKeyCommand per key these are silently swallowed before ever
    // reaching the RFB session. Besides the letters, cover digits and the
    // characters terminal font zooming uses: foot binds font-increase to
    // Control+plus/Control+equal, font-decrease to Control+minus, and
    // font-reset to Control+0 by default ("+" is registered as its own
    // input alongside "=" because a hardware keyboard reports the shifted
    // character itself for Ctrl+Shift+=; every keysym here equals its
    // ASCII value, so handleControlKeyCommand needs no special cases).
    static const char *controlLetters = "abcdefghijklmnopqrstuvwxyz0123456789=+-_";
    for (size_t i = 0; controlLetters[i] != '\0'; i++) {
        NSString *letter = [NSString stringWithFormat:@"%c", controlLetters[i]];
        UIKeyCommand *command = [UIKeyCommand keyCommandWithInput:letter
                                                    modifierFlags:UIKeyModifierControl
                                                           action:@selector(handleControlKeyCommand:)];
        if (@available(iOS 15, *))
            command.wantsPriorityOverSystemBehavior = YES;
        [_keyCommands addObject:command];
    }
    // Alt+<letter> and Alt+Shift+<letter>: sway's $mod is Alt (Mod1), not
    // Control -- Control has to stay free for the terminal/app-level Ctrl
    // combos above (Ctrl+C etc.), so a window-manager modifier would collide
    // with those if it also used Control. Same UIKeyInput gap as Control:
    // iOS never routes modified combinations through -insertText:.
    for (size_t i = 0; controlLetters[i] != '\0'; i++) {
        NSString *letter = [NSString stringWithFormat:@"%c", controlLetters[i]];
        UIKeyCommand *altCommand = [UIKeyCommand keyCommandWithInput:letter
                                                       modifierFlags:UIKeyModifierAlternate
                                                              action:@selector(handleAltKeyCommand:)];
        UIKeyCommand *altShiftCommand = [UIKeyCommand keyCommandWithInput:letter
                                                            modifierFlags:UIKeyModifierAlternate | UIKeyModifierShift
                                                                   action:@selector(handleAltShiftKeyCommand:)];
        if (@available(iOS 15, *)) {
            altCommand.wantsPriorityOverSystemBehavior = YES;
            altShiftCommand.wantsPriorityOverSystemBehavior = YES;
        }
        [_keyCommands addObject:altCommand];
        [_keyCommands addObject:altShiftCommand];
    }
    // Ctrl+Alt+<letter or digit>: the desktop's Ctrl+Alt+1-4 (go to that
    // desktop), and whatever else programs bind there.
    static const char *controlAltKeys = "abcdefghijklmnopqrstuvwxyz0123456789";
    for (size_t i = 0; controlAltKeys[i] != '\0'; i++)
        [self addModifiedKeyWithInput:[NSString stringWithFormat:@"%c", controlAltKeys[i]]
                               keysym:(uint32_t) controlAltKeys[i]
                        modifierFlags:UIKeyModifierControl | UIKeyModifierAlternate];
    // Cmd+= / Cmd++ / Cmd+- / Cmd+0: the Apple-conventional zoom chords.
    // Terminal apps in the guest only understand the Ctrl forms (foot's
    // font-increase/decrease/reset are Control+equal/plus/minus/0), so these
    // reuse handleControlKeyCommand, which wraps the key in a guest Ctrl
    // press. Deliberately NOT extended to Cmd+<letter>: translating Cmd+C
    // into guest Ctrl+C would turn a reflexive "copy" into SIGINT.
    static const char *commandZoomKeys = "=+-0";
    for (size_t i = 0; commandZoomKeys[i] != '\0'; i++) {
        NSString *key = [NSString stringWithFormat:@"%c", commandZoomKeys[i]];
        UIKeyCommand *zoomCommand = [UIKeyCommand keyCommandWithInput:key
                                                        modifierFlags:UIKeyModifierCommand
                                                               action:@selector(handleControlKeyCommand:)];
        if (@available(iOS 15, *))
            zoomCommand.wantsPriorityOverSystemBehavior = YES;
        [_keyCommands addObject:zoomCommand];
    }
    // Alt+Return: sway's new-terminal binding.
    UIKeyCommand *altReturn = [UIKeyCommand keyCommandWithInput:@"\r"
                                                  modifierFlags:UIKeyModifierAlternate
                                                         action:@selector(handleAltKeyCommand:)];
    if (@available(iOS 15, *))
        altReturn.wantsPriorityOverSystemBehavior = YES;
    [_keyCommands addObject:altReturn];
    return _keyCommands;
}

- (void)handleControlKeyCommand:(UIKeyCommand *)command {
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

// -input's "\r" (Alt+Return) needs the Return keysym, not the literal
// carriage-return character value; everything else here is a plain letter,
// where the X11 keysym equals its ASCII value.
static uint32_t DisplayRFBKeysymForKeyCommandInput(NSString *input) {
    if ([input isEqualToString:@"\r"])
        return 0xFF0D; // Return
    return (uint32_t) [input characterAtIndex:0];
}

- (void)handleAltKeyCommand:(UIKeyCommand *)command {
    NSString *input = command.input;
    if (input.length == 0 || _rfbClient == nil)
        return;
    [self releaseLatchedAccessoryModifiers];
    static const uint32_t keysymAltL = 0xFFE9;
    uint32_t keysym = DisplayRFBKeysymForKeyCommandInput(input);
    [_rfbClient sendKeyEvent:keysymAltL down:YES];
    [_rfbClient sendKeyEvent:keysym down:YES];
    [_rfbClient sendKeyEvent:keysym down:NO];
    [_rfbClient sendKeyEvent:keysymAltL down:NO];
}

- (void)handleAltShiftKeyCommand:(UIKeyCommand *)command {
    NSString *input = command.input;
    if (input.length == 0 || _rfbClient == nil)
        return;
    [self releaseLatchedAccessoryModifiers];
    static const uint32_t keysymAltL = 0xFFE9;
    static const uint32_t keysymShiftL = 0xFFE1;
    uint32_t keysym = DisplayRFBKeysymForKeyCommandInput(input);
    // A letter goes as its uppercase keysym, the symbol the key makes with Shift
    // held. wayvnc presses the key at the level that makes the keysym it is sent;
    // for a lowercase letter that is the unshifted level, so it sent the key with
    // Shift and Alt lifted. labwc never saw Alt+Shift, none of its Alt+Shift
    // bindings (close, reconfigure, exit, the launcher) fired, and the letter was
    // typed into the focused window instead.
    if (keysym >= 'a' && keysym <= 'z')
        keysym -= 'a' - 'A';
    [_rfbClient sendKeyEvent:keysymAltL down:YES];
    [_rfbClient sendKeyEvent:keysymShiftL down:YES];
    [_rfbClient sendKeyEvent:keysym down:YES];
    [_rfbClient sendKeyEvent:keysym down:NO];
    [_rfbClient sendKeyEvent:keysymShiftL down:NO];
    [_rfbClient sendKeyEvent:keysymAltL down:NO];
}

// Mirrors TerminalView's addFunctionKey: pattern (stashing the payload via
// propertyList: rather than an associated object) but targets an RFB keysym
// instead of a terminal escape sequence.
- (void)addSpecialKeyWithInput:(NSString *)input keysym:(uint32_t)keysym {
    UIKeyCommand *command = [UIKeyCommand commandWithTitle:@""
                                                      image:nil
                                                     action:@selector(handleSpecialKeyCommand:)
                                                      input:input
                                              modifierFlags:0
                                               propertyList:@(keysym)];
    if (@available(iOS 15, *))
        command.wantsPriorityOverSystemBehavior = YES;
    [_keyCommands addObject:command];
}

// A key under modifiers, sent as the guest would see it typed: the modifiers
// down, the key, the modifiers up.
- (void)addModifiedKeyWithInput:(NSString *)input keysym:(uint32_t)keysym modifierFlags:(UIKeyModifierFlags)flags {
    UIKeyCommand *command = [UIKeyCommand commandWithTitle:@""
                                                      image:nil
                                                     action:@selector(handleModifiedKeyCommand:)
                                                      input:input
                                              modifierFlags:flags
                                               propertyList:@(keysym)];
    if (@available(iOS 15, *))
        command.wantsPriorityOverSystemBehavior = YES;
    [_keyCommands addObject:command];
}

- (void)handleModifiedKeyCommand:(UIKeyCommand *)command {
    NSNumber *keysymNumber = command.propertyList;
    if (![keysymNumber isKindOfClass:NSNumber.class] || _rfbClient == nil)
        return;
    [self releaseLatchedAccessoryModifiers];
    uint32_t modifiers[3];
    int count = 0;
    if (command.modifierFlags & UIKeyModifierControl)
        modifiers[count++] = 0xFFE3; // Control_L
    if (command.modifierFlags & UIKeyModifierAlternate)
        modifiers[count++] = 0xFFE9; // Alt_L
    if (command.modifierFlags & UIKeyModifierShift)
        modifiers[count++] = 0xFFE1; // Shift_L
    for (int i = 0; i < count; i++)
        [_rfbClient sendKeyEvent:modifiers[i] down:YES];
    [_rfbClient sendKeyEvent:keysymNumber.unsignedIntValue down:YES];
    [_rfbClient sendKeyEvent:keysymNumber.unsignedIntValue down:NO];
    for (int i = count - 1; i >= 0; i--)
        [_rfbClient sendKeyEvent:modifiers[i] down:NO];
}

- (void)handleSpecialKeyCommand:(UIKeyCommand *)command {
    NSNumber *keysymNumber = command.propertyList;
    if (![keysymNumber isKindOfClass:NSNumber.class] || _rfbClient == nil)
        return;
    uint32_t keysym = keysymNumber.unsignedIntValue;
    [_rfbClient sendKeyEvent:keysym down:YES];
    [_rfbClient sendKeyEvent:keysym down:NO];
    [self releaseLatchedAccessoryModifiers];
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
    BarButton *ctrlKey = [self accessoryModifierKeyWithTitle:@"ctrl" label:@"Control" keysym:kKeysymControlL];
    BarButton *altKey = [self accessoryModifierKeyWithTitle:@"alt" label:@"Alt" keysym:kKeysymAltL];
    BarButton *superKey = [self accessoryModifierKeyWithTitle:@"❖" label:@"Super" keysym:kKeysymSuperL];
    _accessoryModifierKeys = @[ctrlKey, altKey, superKey];

    UIStackView *stack = [[UIStackView alloc] initWithArrangedSubviews:@[
        [self accessoryKeyWithTitle:@"esc" label:@"Escape" keysym:kKeysymEscape],
        [self accessoryKeyWithTitle:@"⇥" label:@"Tab" keysym:kKeysymTab],
        ctrlKey,
        altKey,
        superKey,
        [self accessoryKeyWithTitle:@"←" label:@"Left arrow" keysym:kKeysymLeft],
        [self accessoryKeyWithTitle:@"↑" label:@"Up arrow" keysym:kKeysymUp],
        [self accessoryKeyWithTitle:@"↓" label:@"Down arrow" keysym:kKeysymDown],
        [self accessoryKeyWithTitle:@"→" label:@"Right arrow" keysym:kKeysymRight],
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
