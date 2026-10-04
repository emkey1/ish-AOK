//
//  TerminalView.m
//  iSH
//
//  Created by Theodore Dubois on 11/3/17.
//

#import "ScrollbarView.h"
#import "TerminalView.h"
#import "UserPreferences.h"
#import "UIApplication+OpenURL.h"
#import "NSObject+SaneKVO.h"
#import "Diagnostics.h"
#include <stdlib.h>
#include <string.h>

struct rowcol {
    int row;
    int col;
};

@interface WeakScriptMessageHandler : NSObject <WKScriptMessageHandler>
@property (weak) id <WKScriptMessageHandler> handler;
@end
@implementation WeakScriptMessageHandler
- (instancetype)initWithHandler:(id <WKScriptMessageHandler>)handler {
    if (self = [super init]) {
        self.handler = handler;
    }
    return self;
}
- (void)userContentController:(WKUserContentController *)userContentController didReceiveScriptMessage:(WKScriptMessage *)message {
    [self.handler userContentController:userContentController didReceiveScriptMessage:message];
}
@end

@interface AOKInputSlot : NSObject
// The bytes this press produced, or nil while they are still in flight.
@property (nonatomic) NSData *payload;
// What the press would type, used to tell this press's key command apart from
// a repeat of some other key arriving in the same handful of milliseconds.
@property (nonatomic, copy) NSString *input;
@end
@implementation AOKInputSlot
@end

// UITextInput's document, for the keyboard's purposes, is only the IME
// composition: the guest's line editor owns everything already typed, so there
// is nothing else the keyboard could usefully read or edit. Positions are UTF-16
// offsets into the composition.
@interface AOKTextPosition : UITextPosition
@property (nonatomic, readonly) NSInteger offset;
+ (instancetype)positionWithOffset:(NSInteger)offset;
@end
@implementation AOKTextPosition
+ (instancetype)positionWithOffset:(NSInteger)offset {
    AOKTextPosition *position = [self new];
    position->_offset = offset;
    return position;
}
@end

@interface AOKTextRange : UITextRange
@property (nonatomic, readonly) AOKTextPosition *startPosition;
@property (nonatomic, readonly) AOKTextPosition *endPosition;
+ (instancetype)rangeFrom:(NSInteger)start to:(NSInteger)end;
@end
@implementation AOKTextRange
+ (instancetype)rangeFrom:(NSInteger)start to:(NSInteger)end {
    AOKTextRange *range = [self new];
    range->_startPosition = [AOKTextPosition positionWithOffset:MIN(start, end)];
    range->_endPosition = [AOKTextPosition positionWithOffset:MAX(start, end)];
    return range;
}
- (UITextPosition *)start { return self.startPosition; }
- (UITextPosition *)end { return self.endPosition; }
- (BOOL)isEmpty { return self.startPosition.offset == self.endPosition.offset; }
@end

@interface TerminalView () {
    // Every key command the terminal claims; -keyCommands narrows it while an
    // IME composition is open.
    NSMutableArray<UIKeyCommand *> *_keyCommands;
}
@property (nonatomic) NSMutableArray *functionKeys;
@property ScrollbarView *scrollbarView;
@property (nonatomic) BOOL terminalFocused;

// The IME composition (pinyin, kana, ...) not yet committed, and the
// keyboard's selection inside it. Drawn over the cursor by term.js; only the
// committed text is sent to the pty.
@property (nonatomic, copy, nullable) NSString *composition;
@property (nonatomic) NSRange compositionSelection;
// The cell the composition starts in, in the web view's coordinates, as term.js
// last reported it. It follows the cursor while nothing is being composed.
@property (nonatomic) CGRect imeCellRect;

@property struct rowcol floatingCursor;
@property CGSize floatingCursorSensitivity;
@property CGSize actualFloatingCursorSensitivity;

@property (nonatomic) NSTimer *keyRepeatTimer;
@property (nonatomic, copy, nullable) NSString *keyRepeatText;

@property (nonatomic) NSMutableArray *pendingInput;
@property (nonatomic) AOKInputSlot *keyEventSlot;
@property (nonatomic) NSTimer *pendingInputTimeout;
@property (nonatomic) NSUInteger immediateKeyInputDepth;

@end

@implementation TerminalView
@synthesize inputDelegate;
@synthesize tokenizer;
@synthesize canBecomeFirstResponder;

- (void)awakeFromNib {
    [super awakeFromNib];
    self.inputAssistantItem.leadingBarButtonGroups = @[];
    self.inputAssistantItem.trailingBarButtonGroups = @[];

    ScrollbarView *scrollbarView = self.scrollbarView = [[ScrollbarView alloc] initWithFrame:self.bounds];
    scrollbarView.delegate = self;
    scrollbarView.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    scrollbarView.bounces = NO;
    [self addSubview:scrollbarView];

    UserPreferences *prefs = UserPreferences.shared;
    [prefs observe:@[@"capsLockMapping", @"optionMapping", @"backtickMapEscape", @"overrideControlSpace"]
           options:0 owner:self usingBlock:^(typeof(self) self) {
        dispatch_async(dispatch_get_main_queue(), ^{
            self->_keyCommands = nil;
        });
    }];
    [prefs observe:@[@"colorScheme", @"fontFamily", @"fontSize", @"lineHeight", @"theme", @"cursorStyle", @"blinkCursor"]
           options:0 owner:self usingBlock:^(typeof(self) self) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self _updateStyle];
        });
    }];

}

- (void)dealloc {
    if (_terminal != nil) {
        [_terminal removeObserver:self forKeyPath:@"loaded"];
        [self uninstallTerminalView];
        _terminal = nil;
    }
}

- (void)observeValueForKeyPath:(NSString *)keyPath ofObject:(id)object change:(NSDictionary<NSKeyValueChangeKey,id> *)change context:(void *)context {
    if (object == _terminal) {
        if (_terminal.loaded) {
            [self installTerminalView];
            [self _updateStyle];
        }
    }
}

static NSString *const HANDLERS[] = {@"syncFocus", @"focus", @"newScrollHeight", @"newScrollTop", @"openLink", @"findCount", @"imeRect"};

static BOOL ISHTerminalViewEventLogEnabled(void) {
    const char *enabled = getenv("ISH_TRACE_TERMINAL_LIFECYCLE");
    return enabled != NULL && enabled[0] != '\0' && strcmp(enabled, "0") != 0;
}

static void ISHRecordTerminalViewEvent(NSString *event, Terminal *terminal, NSDictionary<NSString *, id> *details) {
    NSMutableDictionary<NSString *, id> *payload = [NSMutableDictionary dictionaryWithDictionary:details ?: @{}];
    payload[@"terminalUUID"] = terminal.uuid.UUIDString ?: @"";
    payload[@"type"] = @(terminal.type);
    payload[@"number"] = @(terminal.number);
    [ISHDiagnosticsStore recordBreadcrumb:event details:payload];
    if (ISHTerminalViewEventLogEnabled()) {
        NSLog(@"%@ terminal=%@ type=%d num=%d details=%@",
              event, terminal.uuid.UUIDString ?: @"", terminal.type, terminal.number, payload);
    }
}

- (void)setTerminal:(Terminal *)terminal {
    if (_terminal == terminal)
        return;

    if (_terminal) {
        ISHRecordTerminalViewEvent(@"terminalView.setTerminal.detach", _terminal,
                                   @{@"reason": @"replace-terminal"} );
        // A composition was being typed into the terminal going away. Drop it
        // (it was never sent), and tell the keyboard its document changed so
        // it starts the next one fresh.
        if (self.composition != nil) {
            [self.inputDelegate textWillChange:self];
            [self setComposition:nil selection:NSMakeRange(0, 0)];
            [self.inputDelegate textDidChange:self];
        }
        [_terminal removeObserver:self forKeyPath:@"loaded"];
        [self uninstallTerminalView];
    }

    _terminal = terminal;
    if (_terminal == nil)
        return;

    ISHRecordTerminalViewEvent(@"terminalView.setTerminal.attach", _terminal,
                               @{@"loaded": _terminal.loaded ? @"yes" : @"no"} );
    [_terminal webView];
    [_terminal addObserver:self forKeyPath:@"loaded" options:NSKeyValueObservingOptionInitial context:nil];
    [self installTerminalView];
}

- (void)installTerminalView {
    UIView *superview = self.terminal.webView.superview;
    if (superview != nil) {
        NSAssert(superview == self.scrollbarView, @"installing terminal that is already installed elsewhere");
        ISHRecordTerminalViewEvent(@"terminalView.install.skip", self.terminal,
                                   @{@"reason": @"already-installed",
                                     @"sameSuperview": superview == self.scrollbarView ? @"yes" : @"no"} );
        return;
    }

    WKWebView *webView = _terminal.webView;
    ISHRecordTerminalViewEvent(@"terminalView.install.begin", _terminal,
                               @{@"loaded": _terminal.loaded ? @"yes" : @"no"} );
    _terminal.enableVoiceOverAnnounce = YES;
    webView.scrollView.scrollEnabled = NO;
    webView.scrollView.delaysContentTouches = NO;
    webView.scrollView.canCancelContentTouches = NO;
    webView.scrollView.panGestureRecognizer.enabled = NO;
    id <WKScriptMessageHandler> handler = [[WeakScriptMessageHandler alloc] initWithHandler:self];
    for (int i = 0; i < sizeof(HANDLERS)/sizeof(HANDLERS[0]); i++) {
        [webView.configuration.userContentController addScriptMessageHandler:handler name:HANDLERS[i]];
    }
    webView.frame = self.bounds;
    self.opaque = webView.opaque = NO;
    webView.backgroundColor = UIColor.clearColor;
    webView.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;

    self.scrollbarView.contentView = webView;
    [self.scrollbarView addSubview:webView];
    [self syncTerminalFocus];
    self.imeCellRect = CGRectZero;
    if (_terminal.loaded)
        [webView evaluateJavaScript:@"exports.layoutComposition()" completionHandler:nil];
    [self.terminal requestRefresh];
    ISHRecordTerminalViewEvent(@"terminalView.install.end", _terminal, nil);
}

- (void)uninstallTerminalView {
    // remove old terminal
    UIView *superview = _terminal.webView.superview;
    if (superview != self.scrollbarView) {
        NSAssert(superview == nil, @"uninstalling terminal that is installed elsewhere");
        ISHRecordTerminalViewEvent(@"terminalView.uninstall.skip", _terminal,
                                   @{@"reason": superview == nil ? @"already-detached" : @"installed-elsewhere"} );
        return;
    }

    ISHRecordTerminalViewEvent(@"terminalView.uninstall.begin", _terminal, nil);
    [_terminal.webView removeFromSuperview];
    self.scrollbarView.contentView = nil;
    for (int i = 0; i < sizeof(HANDLERS)/sizeof(HANDLERS[0]); i++) {
        [_terminal.webView.configuration.userContentController removeScriptMessageHandlerForName:HANDLERS[i]];
    }
    _terminal.enableVoiceOverAnnounce = NO;
    ISHRecordTerminalViewEvent(@"terminalView.uninstall.end", _terminal, nil);
}

#pragma mark Styling

- (void)_updateStyle {
    NSAssert(NSThread.isMainThread, @"This method needs to be called on the main thread");
    if (!self.terminal.loaded)
        return;
    UserPreferences *prefs = [UserPreferences shared];
    Palette *palette = prefs.palette;
    if (self.overrideAppearance != OverrideAppearanceNone) {
        palette = self.overrideAppearance == OverrideAppearanceLight ? prefs.theme.lightPalette : prefs.theme.darkPalette;
    }
    NSMutableDictionary<NSString *, id> *themeInfo = [@{
        @"fontFamily": prefs.fontFamily,
        @"fontSize": @(self.effectiveFontSize),
        @"lineHeight": prefs.lineHeight,
        @"foregroundColor": palette.foregroundColor,
        @"backgroundColor": palette.backgroundColor,
        @"blinkCursor": @(prefs.blinkCursor),
        @"cursorShape": prefs.htermCursorShape,
    } mutableCopy];
    // Ask the palette that is actually being rendered, not prefs.palette: with
    // an appearance override in play those are different objects, and testing
    // one while reading the other drops a light palette's overrides whenever
    // the dark half of the same theme happens not to define any.
    if (palette.colorPaletteOverrides) {
        themeInfo[@"colorPaletteOverrides"] = palette.colorPaletteOverrides;
    }
    NSString *json = [[NSString alloc] initWithData:[NSJSONSerialization dataWithJSONObject:themeInfo options:0 error:nil] encoding:NSUTF8StringEncoding];
    [self.terminal.webView evaluateJavaScript:[NSString stringWithFormat:@"exports.updateStyle(%@)", json] completionHandler:^(id result, NSError *error){
        [self updateFloatingCursorSensitivity];
    }];
}

- (void)setOverrideFontSize:(CGFloat)overrideFontSize {
    _overrideFontSize = overrideFontSize;
    [self _updateStyle];
}

- (void)setOverrideAppearance:(enum OverrideAppearance)overrideAppearance {
    _overrideAppearance = overrideAppearance;
    [self _updateStyle];
}

- (CGFloat)effectiveFontSize {
    if (self.overrideFontSize != 0)
        return self.overrideFontSize;
    return UserPreferences.shared.fontSize.doubleValue;
}

#pragma mark Focus and scrolling

- (void)setTerminalFocused:(BOOL)terminalFocused {
    _terminalFocused = terminalFocused;
    [self syncTerminalFocus];
}

- (void)syncTerminalFocus {
    if (!self.terminal.loaded)
        return;
    NSString *script = _terminalFocused ? @"exports.setFocused(true)" : @"exports.setFocused(false)";
    [self.terminal.webView evaluateJavaScript:script completionHandler:nil];
}

- (BOOL)becomeFirstResponder {
    self.terminalFocused = YES;
    [self.terminal requestRefresh];
    [self reloadInputViews];
    return [super becomeFirstResponder];
}
- (BOOL)resignFirstResponder {
    self.terminalFocused = NO;
    [self stopKeyRepeat];
    [self discardPendingInput];
    return [super resignFirstResponder];
}
- (void)windowDidBecomeKey:(NSNotification *)notif {
    self.terminalFocused = YES;
    [self.terminal requestRefresh];
}
- (void)windowDidResignKey:(NSNotification *)notif {
    self.terminalFocused = NO;
    [self stopKeyRepeat];
    [self discardPendingInput];
}

- (IBAction)loseFocus:(id)sender {
    [self resignFirstResponder];
}

- (void)willMoveToWindow:(UIWindow *)newWindow {
    NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
    if (self.window != nil) {
        [center removeObserver:self
                          name:UIWindowDidBecomeKeyNotification
                        object:self.window];
        [center removeObserver:self
                          name:UIWindowDidResignKeyNotification
                        object:self.window];
    }
    if (newWindow != nil) {
        [center addObserver:self
                   selector:@selector(windowDidBecomeKey:)
                       name:UIWindowDidBecomeKeyNotification
                     object:newWindow];
        [center addObserver:self
                   selector:@selector(windowDidResignKey:)
                       name:UIWindowDidResignKeyNotification
                     object:newWindow];
    }
}

- (void)userContentController:(WKUserContentController *)userContentController didReceiveScriptMessage:(WKScriptMessage *)message {
    if ([message.name isEqualToString:@"syncFocus"]) {
        self.terminalFocused = self.terminalFocused;
    } else if ([message.name isEqualToString:@"focus"]) {
        if (!self.isFirstResponder) {
            [self becomeFirstResponder];
        }
    } else if ([message.name isEqualToString:@"newScrollHeight"]) {
        self.scrollbarView.contentSize = CGSizeMake(0, [message.body doubleValue]);
    } else if ([message.name isEqualToString:@"newScrollTop"]) {
        CGFloat newOffset = [message.body doubleValue];
        if (self.scrollbarView.contentOffset.y == newOffset)
            return;
        [self.scrollbarView setContentOffset:CGPointMake(0, newOffset) animated:NO];
    } else if ([message.name isEqualToString:@"openLink"]) {
        [UIApplication openURL:message.body];
    } else if ([message.name isEqualToString:@"imeRect"]) {
        NSArray *rect = message.body;
        if ([rect isKindOfClass:NSArray.class] && rect.count == 4)
            self.imeCellRect = CGRectMake([rect[0] doubleValue], [rect[1] doubleValue],
                                          [rect[2] doubleValue], [rect[3] doubleValue]);
    } else if ([message.name isEqualToString:@"findCount"]) {
        NSArray *counts = message.body;
        if (self.findResultsDidChange != nil && [counts isKindOfClass:NSArray.class] && counts.count == 2) {
            self.findResultsDidChange([counts[0] integerValue], [counts[1] integerValue]);
        }
    }
}

- (void)scrollViewDidScroll:(UIScrollView *)scrollView {
    if (!self.terminal.loaded)
        return;
    [self.terminal.webView evaluateJavaScript:[NSString stringWithFormat:@"exports.newScrollTop(%f)", scrollView.contentOffset.y] completionHandler:nil];
}

- (void)setKeyboardAppearance:(UIKeyboardAppearance)keyboardAppearance {
    BOOL needsFirstResponderDance = self.isFirstResponder && _keyboardAppearance != keyboardAppearance;
    _keyboardAppearance = keyboardAppearance;
    if (needsFirstResponderDance) {
        // Deferred to the next runloop turn: this setter is reached from
        // TerminalViewController's UserPreferences KVO handler inside a
        // UIView animation block, whose call stack is not under our control
        // -- in a multi-window Workspace, that can land while UIKit's own
        // keyboard-scene machinery (_UIRemoteKeyboards) is already mid-
        // transition for a DIFFERENT terminal window. Calling
        // resignFirstResponder/becomeFirstResponder synchronously there
        // re-enters UIKit's keyboard/input-view teardown from inside itself,
        // observed on device as an EXC_BAD_ACCESS deep in CoreAutoLayout's
        // _switchToLayoutEngine: (recursing through its own block ~7 times)
        // during -[TerminalView resignFirstResponder]. Running the dance on
        // a fresh runloop turn keeps it off any in-progress UIKit call stack.
        dispatch_async(dispatch_get_main_queue(), ^{
            [self resignFirstResponder];
            [self becomeFirstResponder];
        });
    }
    if (keyboardAppearance == UIKeyboardAppearanceLight) {
        self.scrollbarView.indicatorStyle = UIScrollViewIndicatorStyleBlack;
    } else {
        self.scrollbarView.indicatorStyle = UIScrollViewIndicatorStyleWhite;
    }
}

#pragma mark Keyboard input ordering

// The keyboard reaches the tty by two routes with different latencies. A
// UIKeyCommand is dispatched straight off the key event -- measured 0.3 ms
// after the press -- while an ordinary printable character arrives at
// -insertText: through UIKit's text-input pipeline, measured ~5 ms after the
// press (25 ms for the first keystroke after an idle spell, and further behind
// once that pipeline has a backlog). Nothing sequenced the two, so a key
// command could overtake text typed before it. That is what put h/j/k/l ahead
// of typed characters until they were taken off the key-command path; arrows,
// Tab, Esc, the function keys and the Ctrl chords are still key commands and
// kept the same head start.
//
// -pressesBegan: sees every hardware key, in the order the user pressed them,
// before either route runs, so it is the one place that knows the true order.
// Each press that can produce input reserves a slot; whichever route produces
// the bytes fills that press's slot; slots are emitted strictly from the front.
// A lone keystroke -- overwhelmingly the common case -- fills the only slot and
// flushes with no added latency, and input with no press behind it (the
// on-screen keyboard, paste, the arrow bar, key repeat) has no slot to wait
// behind and goes straight out.
//
// Two invariants hold this together. Every submission is emitted exactly once:
// a slot is filled by one submission and removed when emitted, and a filled
// slot is always drained -- by the flush after every fill, by the timeout
// below, or by -discardPendingInput. And every uncertain case degrades to
// sending immediately, which is precisely the behaviour before any of this
// existed, so a mis-identified slot can only fail to improve the ordering, not
// lose or duplicate a keystroke.

// How long a reservation may hold the queue before it is written off. It has to
// clear the worst text-path latency (25 ms, cold) by a wide margin, because
// giving up early is what reintroduces the reordering. It only costs anything
// when a press produces no input at all -- a chord that belongs to another
// responder, a keystroke swallowed into an IME composition.
static const NSTimeInterval kPendingInputTimeout = 0.075;

- (AOKInputSlot *)reserveInputSlotForInput:(NSString *)input {
    if (self.pendingInput == nil)
        self.pendingInput = [NSMutableArray new];
    AOKInputSlot *slot = [AOKInputSlot new];
    slot.input = input;
    [self.pendingInput addObject:slot];
    [self armPendingInputTimeout];
    return slot;
}

- (void)armPendingInputTimeout {
    if (self.pendingInputTimeout != nil)
        return;
    __weak typeof(self) weakSelf = self;
    NSTimer *timer = [NSTimer timerWithTimeInterval:kPendingInputTimeout repeats:NO block:^(NSTimer *t) {
        typeof(self) strongSelf = weakSelf;
        strongSelf.pendingInputTimeout = nil;
        // Write off the reservations at the front that never produced anything,
        // and let whatever queued up behind them through.
        while (strongSelf.pendingInput.count > 0 && ((AOKInputSlot *) strongSelf.pendingInput.firstObject).payload == nil)
            [strongSelf.pendingInput removeObjectAtIndex:0];
        [strongSelf flushPendingInput];
    }];
    [NSRunLoop.mainRunLoop addTimer:timer forMode:NSRunLoopCommonModes];
    self.pendingInputTimeout = timer;
}

- (void)cancelPendingInputTimeout {
    [self.pendingInputTimeout invalidate];
    self.pendingInputTimeout = nil;
}

- (void)flushPendingInput {
    while (self.pendingInput.count > 0) {
        AOKInputSlot *head = self.pendingInput.firstObject;
        if (head.payload == nil)
            break;
        [self.pendingInput removeObjectAtIndex:0];
        if (head.payload.length > 0)
            [self.terminal sendInput:head.payload];
    }
    if (self.pendingInput.count == 0)
        [self cancelPendingInputTimeout];
    else
        [self armPendingInputTimeout];
}

// Focus is going away: emit what is already in hand, in order, and write off
// reservations whose bytes this view will never see.
- (void)discardPendingInput {
    NSMutableArray *kept = [NSMutableArray new];
    for (AOKInputSlot *slot in self.pendingInput)
        if (slot.payload != nil)
            [kept addObject:slot];
    self.pendingInput = kept;
    self.keyEventSlot = nil;
    [self flushPendingInput];
}

// Every byte the keyboard produces leaves through here.
- (void)submitInput:(NSData *)data {
    if (data == nil)
        return;
    AOKInputSlot *slot = nil;
    if (self.immediateKeyInputDepth > 0) {
        // Produced synchronously by the key event being dispatched right now,
        // so it belongs to that press's reservation and to no other. A key
        // repeat arrives here with no press behind it and no slot claimed, and
        // goes straight out.
        slot = self.keyEventSlot;
        self.keyEventSlot = nil;
    } else {
        // Came back through the text pipeline, so it belongs to the oldest
        // press still waiting for its bytes.
        for (AOKInputSlot *candidate in self.pendingInput) {
            if (candidate.payload == nil) {
                slot = candidate;
                break;
            }
        }
        if (slot == self.keyEventSlot)
            self.keyEventSlot = nil;
    }
    if (slot == nil || slot.payload != nil) {
        if (data.length > 0)
            [self.terminal sendInput:data];
        return;
    }
    slot.payload = data;
    [self flushPendingInput];
}

#pragma mark Keyboard Input

// implementing these makes a keyboard pop up when this view is first responder

- (void)insertText:(NSString *)text {
    if (self.composition != nil)
        [self setComposition:nil selection:NSMakeRange(0, 0)];
    if (text == nil)
        return;

    if (self.controlKey.highlighted)
        self.controlKey.selected = YES;
    if (self.controlKey.selected) {
        if (!self.controlKey.highlighted)
            self.controlKey.selected = NO;
        // ASCII only: insertControlChar: takes a char, and a committed 中
        // (U+4E2D) truncated to one would arrive as Ctrl-M.
        if (text.length == 1 && [text characterAtIndex:0] < 0x80)
            return [self insertControlChar:[text characterAtIndex:0]];
    }

    text = [text stringByReplacingOccurrencesOfString:@"\n" withString:@"\r"];
    NSData *data = [text dataUsingEncoding:NSUTF8StringEncoding];
    [self submitInput:data];
}

// This method is used with text that requires no further processing; like the escape sequences from function keys
- (void)insertRawText:(NSString *)text {
    NSData *data = [text dataUsingEncoding:NSUTF8StringEncoding];
    [self submitInput:data];
}

- (void)insertControlChar:(char)ch {
    if (strchr(controlKeys, ch) != NULL) {
        if (ch == ' ') ch = '\0';
        if (ch == '2') ch = '@';
        if (ch == '6') ch = '^';
        if (ch != '\0')
            ch = toupper(ch) ^ 0x40;
        [self submitInput:[NSData dataWithBytes:&ch length:1]];
    }
}


- (NSString *)setControlChar:(char)ch {
    if (strchr(controlKeys, ch) != NULL) {
        if (ch == ' ') ch = '\0';
        if (ch == '2') ch = '@';
        if (ch == '6') ch = '^';
        if (ch != '\0')
            ch = toupper(ch) ^ 0x40;
    } else {
        ch = '\0';
    }
    return [NSString stringWithFormat:@"%c", ch];
}



- (void)deleteBackward {
    [self insertText:@"\x7f"];
}

- (BOOL)hasText {
    return YES; // it's always ok to send a "delete"
}

#pragma mark IME Input and Selection

- (void)setComposition:(nullable NSString *)composition selection:(NSRange)selection {
    if (composition.length == 0)
        composition = nil;
    BOOL changed = !(composition == self.composition || [composition isEqualToString:self.composition]);
    _composition = [composition copy];
    _compositionSelection = composition == nil ? NSMakeRange(0, 0) : selection;
    if (!changed || !self.terminal.loaded)
        return;
    NSData *json = [NSJSONSerialization dataWithJSONObject:composition ?: @""
                                                   options:NSJSONWritingFragmentsAllowed error:nil];
    if (json == nil)
        return;
    NSString *script = [NSString stringWithFormat:@"exports.setComposition(%@)",
                        [[NSString alloc] initWithData:json encoding:NSUTF8StringEncoding]];
    [self.terminal.webView evaluateJavaScript:script completionHandler:nil];
}

- (void)setMarkedText:(nullable NSString *)markedText selectedRange:(NSRange)selectedRange {
    [self setComposition:markedText selection:selectedRange];
}

- (void)unmarkText {
    NSString *text = self.composition;
    [self setComposition:nil selection:NSMakeRange(0, 0)];
    if (text != nil)
        [self insertText:text];
}

- (NSInteger)documentLength {
    return self.composition.length;
}

- (NSInteger)clampOffset:(NSInteger)offset {
    return MAX(0, MIN(offset, self.documentLength));
}

- (UITextRange *)markedTextRange {
    if (self.composition == nil)
        return nil;
    return [AOKTextRange rangeFrom:0 to:self.documentLength];
}

// Besides the IME, this keeps "speak selection" from failing to get the current
// selection and falling back on calling copy:.
- (UITextRange *)selectedTextRange {
    NSRange selection = self.compositionSelection;
    return [AOKTextRange rangeFrom:[self clampOffset:selection.location]
                                to:[self clampOffset:NSMaxRange(selection)]];
}

- (void)setSelectedTextRange:(UITextRange *)selectedTextRange {
    if (self.composition == nil || ![selectedTextRange isKindOfClass:AOKTextRange.class])
        return;
    AOKTextRange *range = (AOKTextRange *) selectedTextRange;
    NSInteger start = [self clampOffset:range.startPosition.offset];
    NSInteger end = [self clampOffset:range.endPosition.offset];
    _compositionSelection = NSMakeRange(start, end - start);
}

- (NSString *)textInRange:(UITextRange *)range {
    if (![range isKindOfClass:AOKTextRange.class])
        return nil;
    AOKTextRange *r = (AOKTextRange *) range;
    NSInteger start = [self clampOffset:r.startPosition.offset];
    NSInteger end = [self clampOffset:r.endPosition.offset];
    return [self.composition ?: @"" substringWithRange:NSMakeRange(start, end - start)];
}

- (void)replaceRange:(UITextRange *)range withText:(NSString *)text {
    // The only text there is to replace is the composition, so replacing any of
    // it commits the replacement, like choosing a candidate.
    [self insertText:text];
}

- (UITextPosition *)beginningOfDocument {
    return [AOKTextPosition positionWithOffset:0];
}

- (UITextPosition *)endOfDocument {
    return [AOKTextPosition positionWithOffset:self.documentLength];
}

- (nullable UITextRange *)textRangeFromPosition:(UITextPosition *)fromPosition toPosition:(UITextPosition *)toPosition {
    if (![fromPosition isKindOfClass:AOKTextPosition.class] || ![toPosition isKindOfClass:AOKTextPosition.class])
        return nil;
    return [AOKTextRange rangeFrom:((AOKTextPosition *) fromPosition).offset
                                to:((AOKTextPosition *) toPosition).offset];
}

- (nullable UITextPosition *)positionFromPosition:(UITextPosition *)position offset:(NSInteger)offset {
    if (![position isKindOfClass:AOKTextPosition.class])
        return nil;
    NSInteger result = ((AOKTextPosition *) position).offset + offset;
    if (result < 0 || result > self.documentLength)
        return nil;
    return [AOKTextPosition positionWithOffset:result];
}

- (nullable UITextPosition *)positionFromPosition:(UITextPosition *)position inDirection:(UITextLayoutDirection)direction offset:(NSInteger)offset {
    // One line, left to right.
    switch (direction) {
        case UITextLayoutDirectionRight: return [self positionFromPosition:position offset:offset];
        case UITextLayoutDirectionLeft: return [self positionFromPosition:position offset:-offset];
        default: return nil;
    }
}

- (NSComparisonResult)comparePosition:(UITextPosition *)position toPosition:(UITextPosition *)other {
    NSInteger a = [position isKindOfClass:AOKTextPosition.class] ? ((AOKTextPosition *) position).offset : 0;
    NSInteger b = [other isKindOfClass:AOKTextPosition.class] ? ((AOKTextPosition *) other).offset : 0;
    return a < b ? NSOrderedAscending : a > b ? NSOrderedDescending : NSOrderedSame;
}

- (NSInteger)offsetFromPosition:(UITextPosition *)from toPosition:(UITextPosition *)toPosition {
    NSInteger a = [from isKindOfClass:AOKTextPosition.class] ? ((AOKTextPosition *) from).offset : 0;
    NSInteger b = [toPosition isKindOfClass:AOKTextPosition.class] ? ((AOKTextPosition *) toPosition).offset : 0;
    return b - a;
}

- (nullable UITextPosition *)positionWithinRange:(UITextRange *)range farthestInDirection:(UITextLayoutDirection)direction {
    if (direction == UITextLayoutDirectionLeft || direction == UITextLayoutDirectionUp)
        return range.start;
    return range.end;
}

- (nullable UITextRange *)characterRangeByExtendingPosition:(UITextPosition *)position inDirection:(UITextLayoutDirection)direction {
    if (![position isKindOfClass:AOKTextPosition.class])
        return nil;
    NSInteger offset = [self clampOffset:((AOKTextPosition *) position).offset];
    if (direction == UITextLayoutDirectionLeft || direction == UITextLayoutDirectionUp)
        return [AOKTextRange rangeFrom:0 to:offset];
    return [AOKTextRange rangeFrom:offset to:self.documentLength];
}

- (NSWritingDirection)baseWritingDirectionForPosition:(UITextPosition *)position inDirection:(UITextStorageDirection)direction {
    return NSWritingDirectionLeftToRight;
}
- (void)setBaseWritingDirection:(NSWritingDirection)writingDirection forRange:(UITextRange *)range {
}

#pragma mark IME Geometry

// Terminal columns the composition takes up to offset, counted as hterm's
// lib.wc lays it out in term.js: East Asian wide and fullwidth characters (and
// anything astral, which is mostly emoji and CJK extensions) take two cells.
- (NSInteger)compositionColumnsBefore:(NSInteger)offset {
    NSString *text = self.composition;
    if (text == nil || offset <= 0)
        return 0;
    __block NSInteger columns = 0;
    [text enumerateSubstringsInRange:NSMakeRange(0, MIN(offset, (NSInteger) text.length))
                             options:NSStringEnumerationByComposedCharacterSequences
                          usingBlock:^(NSString *ch, NSRange range, NSRange enclosing, BOOL *stop) {
        UTF32Char c = [ch characterAtIndex:0];
        if (CFStringIsSurrogateHighCharacter(c) && ch.length > 1)
            c = CFStringGetLongCharacterForSurrogatePair(c, [ch characterAtIndex:1]);
        BOOL wide = (c >= 0x1100 && c <= 0x115f) || (c >= 0x2e80 && c <= 0xa4cf && c != 0x303f) ||
            (c >= 0xac00 && c <= 0xd7a3) || (c >= 0xf900 && c <= 0xfaff) ||
            (c >= 0xfe30 && c <= 0xfe4f) || (c >= 0xff00 && c <= 0xff60) ||
            (c >= 0xffe0 && c <= 0xffe6) || c >= 0x10000;
        columns += wide ? 2 : 1;
    }];
    return columns;
}

// The terminal cells from offset `from` to `to` of the composition, in this
// view's coordinates. Without a composition both are 0 and this is the cursor.
- (CGRect)rectFromOffset:(NSInteger)from toOffset:(NSInteger)to {
    CGRect cell = self.imeCellRect;
    WKWebView *webView = self.terminal.webView;
    if (CGRectIsEmpty(cell) || webView == nil)
        return CGRectZero;
    NSInteger startCol = [self compositionColumnsBefore:from];
    NSInteger endCol = [self compositionColumnsBefore:to];
    CGRect rect = CGRectMake(cell.origin.x + cell.size.width * startCol, cell.origin.y,
                             cell.size.width * MAX(endCol - startCol, 0), cell.size.height);
    return [self convertRect:rect fromView:webView];
}

- (CGRect)caretRectForPosition:(UITextPosition *)position {
    NSInteger offset = [position isKindOfClass:AOKTextPosition.class] ? [self clampOffset:((AOKTextPosition *) position).offset] : 0;
    CGRect rect = [self rectFromOffset:offset toOffset:offset];
    if (rect.size.height > 0)
        rect.size.width = 2;
    return rect;
}

- (CGRect)firstRectForRange:(UITextRange *)range {
    if (![range isKindOfClass:AOKTextRange.class])
        return [self rectFromOffset:0 toOffset:self.documentLength];
    AOKTextRange *r = (AOKTextRange *) range;
    CGRect rect = [self rectFromOffset:[self clampOffset:r.startPosition.offset]
                              toOffset:[self clampOffset:r.endPosition.offset]];
    if (rect.size.height > 0 && rect.size.width == 0)
        rect.size.width = self.imeCellRect.size.width;
    return rect;
}

- (NSArray<UITextSelectionRect *> *)selectionRectsForRange:(UITextRange *)range {
    return @[];
}

- (nullable UITextPosition *)closestPositionToPoint:(CGPoint)point {
    return self.endOfDocument;
}
- (nullable UITextPosition *)closestPositionToPoint:(CGPoint)point withinRange:(UITextRange *)range {
    return range.end;
}
- (nullable UITextRange *)characterRangeAtPoint:(CGPoint)point {
    return nil;
}

// The composition is drawn by term.js in the terminal's own font and colours,
// underlined, so UIKit's attributes for it are not used.
- (NSDictionary<NSAttributedStringKey,id> *)markedTextStyle {
    return nil;
}
- (void)setMarkedTextStyle:(NSDictionary<NSAttributedStringKey,id> *)markedTextStyle {
}

- (id)insertDictationResultPlaceholder {
    return @"";
}
- (void)removeDictationResultPlaceholder:(id)placeholder willInsertResult:(BOOL)willInsertResult {
}

#pragma mark Keyboard Actions

- (void)paste:(id)sender {
    NSString *string = UIPasteboard.generalPasteboard.string;
    if (string == nil)
        return;
    // Deliberately not -insertText:, which is the TYPING path. A guest that
    // asked for bracketed paste is asking to be able to tell a paste from
    // typing, and it only can if the payload arrives wrapped -- otherwise a
    // multi-line clipboard runs as a sequence of commands the moment it lands.
    // -insertRawText: keeps the input-slot ordering that the Cmd-V keypress has
    // already reserved, which is why the bytes still go out through the view
    // rather than straight to the terminal.
    [self insertRawText:ISHPasteSequence(string, self.terminal.bracketedPasteEnabled, NO)];
    // And take keyboard focus back if the paste came while the WEB VIEW had it.
    // A long press or a drag-selection makes the web view first responder, and
    // its edit menu's Paste, Cmd-V and the Paste key still end up here: the web
    // view cannot paste into read-only text, so UIKit passes paste: up the
    // responder chain to this view. The text went out and focus stayed in the
    // web view, where typed keys reach nothing -- the pasted command sat at the
    // prompt and Return did nothing until a click gave the terminal focus back
    // (#603).
    if (!self.isFirstResponder)
        [self becomeFirstResponder];
}

- (void)copy:(id)sender {
    [self.terminal.webView evaluateJavaScript:@"exports.copy()" completionHandler:nil];
}

- (void)clearScrollback:(UIKeyCommand *)command {
    [self.terminal.webView evaluateJavaScript:@"exports.clearScrollback()" completionHandler:nil];
}

#pragma mark Scrollback search

- (void)findOpen {
    [self.terminal.webView evaluateJavaScript:@"exports.findOpen()" completionHandler:nil];
}

- (void)findSetText:(NSString *)text {
    // Round-trip through JSON rather than interpolating: a search query is
    // arbitrary user text and will contain quotes, backslashes and newlines.
    NSData *json = [NSJSONSerialization dataWithJSONObject:@[text ?: @""] options:0 error:NULL];
    if (json == nil)
        return;
    NSString *literal = [[NSString alloc] initWithData:json encoding:NSUTF8StringEncoding];
    NSString *script = [NSString stringWithFormat:@"exports.findSetText(%@[0])", literal];
    [self.terminal.webView evaluateJavaScript:script completionHandler:nil];
}

- (void)findNext {
    [self.terminal.webView evaluateJavaScript:@"exports.findNext()" completionHandler:nil];
}

- (void)findPrevious {
    [self.terminal.webView evaluateJavaScript:@"exports.findPrevious()" completionHandler:nil];
}

- (void)findClose {
    [self.terminal.webView evaluateJavaScript:@"exports.findClose()" completionHandler:nil];
}

#pragma mark Floating cursor

- (void)updateFloatingCursorSensitivity {
    [self.terminal.webView evaluateJavaScript:@"exports.getCharacterSize()" completionHandler:^(NSArray *charSizeRaw, NSError *error) {
        if (error != nil) {
            NSLog(@"error getting character size: %@", error);
            return;
        }
        CGSize charSize = CGSizeMake([charSizeRaw[0] doubleValue], [charSizeRaw[1] doubleValue]);
        double sensitivity = 0.5;
        self.floatingCursorSensitivity = CGSizeMake(charSize.width / sensitivity, charSize.height / sensitivity);
    }];
}

- (struct rowcol)rowcolFromPoint:(CGPoint)point {
    CGSize sensitivity = self.actualFloatingCursorSensitivity;
    return (struct rowcol) {
        .row = (int) (-point.y / sensitivity.height),
        .col = (int) (point.x / sensitivity.width),
    };
}

- (void)beginFloatingCursorAtPoint:(CGPoint)point {
    self.actualFloatingCursorSensitivity = self.floatingCursorSensitivity;
    self.floatingCursor = [self rowcolFromPoint:point];
}

- (void)updateFloatingCursorAtPoint:(CGPoint)point {
    struct rowcol newPos = [self rowcolFromPoint:point];
    int rowDiff = newPos.row - self.floatingCursor.row;
    int colDiff = newPos.col - self.floatingCursor.col;
    NSMutableString *arrows = [NSMutableString string];
    for (int i = 0; i < abs(rowDiff); i++) {
        [arrows appendString:[self.terminal arrow:rowDiff > 0 ? 'A': 'B']];
    }
    for (int i = 0; i < abs(colDiff); i++) {
        [arrows appendString:[self.terminal arrow:colDiff > 0 ? 'C': 'D']];
    }
    [self insertText:arrows];
    self.floatingCursor = newPos;
}

- (void)endFloatingCursor {
    self.floatingCursor = (struct rowcol) {};
}

#pragma mark Keyboard Traits

- (UITextSmartDashesType)smartDashesType API_AVAILABLE(ios(11)) {
    return UITextSmartDashesTypeNo;
}
- (UITextSmartQuotesType)smartQuotesType API_AVAILABLE(ios(11)) {
    return UITextSmartQuotesTypeNo;
}
- (UITextSmartInsertDeleteType)smartInsertDeleteType API_AVAILABLE(ios(11)) {
    return UITextSmartInsertDeleteTypeNo;
}
- (UITextAutocapitalizationType)autocapitalizationType {
    return UITextAutocapitalizationTypeNone;
}
- (UITextAutocorrectionType)autocorrectionType {
    return UITextAutocorrectionTypeNo;
}
// Apparently required on iOS 15+: https://stackoverflow.com/a/72359764
- (UITextSpellCheckingType)spellCheckingType {
    return UITextSpellCheckingTypeNo;
}

#pragma mark Hardware Keyboard

- (void)handleKeyCommand:(UIKeyCommand *)command {
    // Everything this produces comes straight off the key event being
    // dispatched now, so it takes the reservation -pressesBegan: just made for
    // that press -- but only if the reservation really is this key's. UIKit
    // repeats a held key by calling here again with no press in between, and
    // that repeat must not take a reservation belonging to a key someone typed
    // a millisecond ago. If it does not match, drop the claim and let this go
    // straight out, exactly as it did before any of this existed.
    NSString *owner = self.keyEventSlot.input;
    if (owner == nil || command.input == nil ||
        [owner caseInsensitiveCompare:command.input] != NSOrderedSame)
        self.keyEventSlot = nil;
    self.immediateKeyInputDepth++;
    [self dispatchKeyCommand:command];
    self.immediateKeyInputDepth--;
}

- (void)dispatchKeyCommand:(UIKeyCommand *)command {
    NSString *key = command.input;
    if (@available(iOS 13.0, *)) {
        if ( command.propertyList != nil ) {
            [self insertRawText:command.propertyList];
            return;
        }
    }
    if (command.modifierFlags == 0) {
        if ([key isEqualToString:@"`"] && UserPreferences.shared.backtickMapEscape)
            key = UIKeyInputEscape;
        if ([key isEqualToString:UIKeyInputEscape])
            key = @"\x1b";
        else if ([key isEqualToString:UIKeyInputUpArrow])
            key = [self.terminal arrow:'A'];
        else if ([key isEqualToString:UIKeyInputDownArrow])
            key = [self.terminal arrow:'B'];
        else if ([key isEqualToString:UIKeyInputLeftArrow])
            key = [self.terminal arrow:'D'];
        else if ([key isEqualToString:UIKeyInputRightArrow])
            key = [self.terminal arrow:'C'];
        [self insertText:key];
    } else if (command.modifierFlags & UIKeyModifierShift) {
        [self insertText:[key uppercaseString]];
    } else if (command.modifierFlags & UIKeyModifierAlternate) {
        [self insertText:[@"\x1b" stringByAppendingString:key]];
    } else if (command.modifierFlags & UIKeyModifierAlphaShift) {
        [self handleCapsLockWithCommand:command];
    } else if (command.modifierFlags & UIKeyModifierControl || command.modifierFlags & UIKeyModifierAlphaShift) {
        if (key.length == 0)
            return;
        if ([key isEqualToString:@"2"])
            key = @"@";
        else if ([key isEqualToString:@"6"])
            key = @"^";
        else if ([key isEqualToString:@"-"])
            key = @"_";
        [self insertControlChar:[key characterAtIndex:0]];
    }
}

static const char *alphabet = "abcdefghijklmnopqrstuvwxyz";
static const char *controlKeys = "abcdefghijklmnopqrstuvwxyz@^26-=[]\\ ";
static const char *metaKeys = "abcdefghijklmnopqrstuvwxyz0123456789-=[]\\;',./";
// Held-key repeat for the vi movement keys is synthesised in -pressesBegan:
// (see -startKeyRepeatForPresses:). These four used to be registered as bare
// UIKeyCommands purely to borrow UIKit's key-command repeat, which put them on
// a different delivery path from every other printable character -- see the
// comment on -startKeyRepeatForPresses: for why that reordered typed text.
static const char *viRepeatKeys = "hjkl";

- (NSArray<UIKeyCommand *> *)keyCommands {
    NSArray<UIKeyCommand *> *commands = [self allKeyCommands];
    // While a composition is open, the arrows, Return, Esc, Tab and the rest
    // belong to the input method: they pick a candidate, commit the pinyin or
    // cancel it. Claimed here (with priority over system behaviour) they would
    // go to the guest as escape sequences instead and leave the composition
    // stuck. Command and Control chords are never the IME's, so they stay.
    if (self.composition != nil) {
        NSPredicate *chords = [NSPredicate predicateWithBlock:^BOOL(UIKeyCommand *command, NSDictionary *bindings) {
            return (command.modifierFlags & (UIKeyModifierCommand | UIKeyModifierControl)) != 0;
        }];
        return [commands filteredArrayUsingPredicate:chords];
    }
    return commands;
}

- (NSArray<UIKeyCommand *> *)allKeyCommands {
    if (_keyCommands != nil)
        return _keyCommands;
    _keyCommands = [NSMutableArray new];
    [self addKeys:controlKeys withModifiers:UIKeyModifierControl];

    if (@available(iOS 13.4, *)) {
        [self addFunctionKey:UIKeyInputUpArrow withName:@"Up" withNormalEscapeSequence:@"\x1b[A" withShiftEscapeSequence:@"\x1b[1;2A" withControlEscapeSequence:@"\x1b[1;5A" withAltEscapeSequence:@"\x1b[1;3A"];
        [self addFunctionKey:UIKeyInputDownArrow withName:@"Down" withNormalEscapeSequence:@"\x1b[B" withShiftEscapeSequence:@"\x1b[1;2B" withControlEscapeSequence:@"\x1b[1;5B" withAltEscapeSequence:@"\x1b[1;3B"];
        [self addFunctionKey:UIKeyInputRightArrow withName:@"Right" withNormalEscapeSequence:@"\x1b[C" withShiftEscapeSequence:@"\x1b[1;2C" withControlEscapeSequence:@"\x1b[1;5C" withAltEscapeSequence:@"\x1b[1;3C"];
        [self addFunctionKey:UIKeyInputLeftArrow withName:@"Left" withNormalEscapeSequence:@"\x1b[D" withShiftEscapeSequence:@"\x1b[1;2D" withControlEscapeSequence:@"\x1b[1;5D" withAltEscapeSequence:@"\x1b[1;3D"];
        [self addFunctionKey:@"\t" withName:@"Tab" withNormalEscapeSequence:@"\t" withShiftEscapeSequence:@"\x1b[Z" withControlEscapeSequence:NULL withAltEscapeSequence:@"\x1b\t"];

        [self addFunctionKey:UIKeyInputEscape withName:@"Esc" withNormalEscapeSequence:@"\x1b" withShiftEscapeSequence:NULL withControlEscapeSequence:NULL withAltEscapeSequence:NULL];
        // Navigation keys

        /*
         * Now UIKey equivalent for Ins/help keys presumably because Apple Keyboards don't have Ins key :-(  Have to handle these in pressesbegan
        [self addFunctionKey:UIKeyInputInsert withNormalEscapeSequence:@"\x1b[2~" withShiftEscapeSequence:@"\x1b[2;2~" withControlEscapeSequence:@"\x1b[2;5~"];
        [self addFunctionKey:UIKeyInputHelp withNormalEscapeSequence:@"\x1b[2~" withShiftEscapeSequence:@"\x1b[2;2~" withControlEscapeSequence:@"\x1b[2;5~"];
         */
        //if (@available(iOS 15.0, *)) {
        //    [self addFunctionKey:UIKeyInputDelete withName:@"Del" withNormalEscapeSequence:@"\x1b[3~" withShiftEscapeSequence:@"\x1b[3;2~" withControlEscapeSequence:@"\x1b[3;5~"];
       // } // This breaks the Del key.
        [self addFunctionKey:UIKeyInputPageUp withName:@"PgUp" withNormalEscapeSequence:@"\x1b[5~" withShiftEscapeSequence:@"\x1b[5;2~" withControlEscapeSequence:@"\x1b[5;5~" withAltEscapeSequence:@"\x1b[5;3~"];
        [self addFunctionKey:UIKeyInputPageDown withName:@"PgDn" withNormalEscapeSequence:@"\x1b[6~" withShiftEscapeSequence:@"\x1b[6;2~" withControlEscapeSequence:@"\x1b[6;5~" withAltEscapeSequence:@"\x1b[6;3~"];
        [self addFunctionKey:UIKeyInputHome withName:@"Home" withNormalEscapeSequence:@"\x1bOH" withShiftEscapeSequence:@"\x1b[1;2H" withControlEscapeSequence:@"\x1b[1;5H" withAltEscapeSequence:@"\x1b[1;3H"];
        [self addFunctionKey:UIKeyInputEnd withName:@"End" withNormalEscapeSequence:@"\x1bOF" withShiftEscapeSequence:@"\x1b[1;2F" withControlEscapeSequence:@"\x1b[1;5F" withAltEscapeSequence:@"\x1b[1;3F"];
        // Function keys
        [self addFunctionKey:UIKeyInputF1 withName:@"F1" withNormalEscapeSequence:@"\x1bOP" withShiftEscapeSequence:@"\x1b[1;2P" withControlEscapeSequence:@"\x1b[1;5P" withAltEscapeSequence:@"\x1bO3P"];
        [self addFunctionKey:UIKeyInputF2 withName:@"F2" withNormalEscapeSequence:@"\x1bOQ" withShiftEscapeSequence:@"\x1b[1;2Q" withControlEscapeSequence:@"\x1b[1;5Q" withAltEscapeSequence:@"\x1bO3Q"];
        [self addFunctionKey:UIKeyInputF3 withName:@"F3" withNormalEscapeSequence:@"\x1bOR" withShiftEscapeSequence:@"\x1b[1;2R" withControlEscapeSequence:@"\x1b[1;5R" withAltEscapeSequence:@"\x1bO3R"];
        [self addFunctionKey:UIKeyInputF4 withName:@"F4" withNormalEscapeSequence:@"\x1bOS" withShiftEscapeSequence:@"\x1b[1;2S" withControlEscapeSequence:@"\x1b[1;5S" withAltEscapeSequence:@"\x1bO3S"];
        [self addFunctionKey:UIKeyInputF5 withName:@"F5" withNormalEscapeSequence:@"\x1b[15~" withShiftEscapeSequence:@"\x1b[15;2~" withControlEscapeSequence:@"\x1b[15;5~" withAltEscapeSequence:@"\x1b[15;3~"];
        // Yes, @"\x1b[16~" is missing; it is meant to be
        [self addFunctionKey:UIKeyInputF6 withName:@"F6" withNormalEscapeSequence:@"\x1b[17~" withShiftEscapeSequence:@"\x1b[17;2~" withControlEscapeSequence:@"\x1b[17;5~" withAltEscapeSequence:@"\x1b[17;3~"];
        [self addFunctionKey:UIKeyInputF7 withName:@"F7" withNormalEscapeSequence:@"\x1b[18~" withShiftEscapeSequence:@"\x1b[18;2~" withControlEscapeSequence:@"\x1b[18;5~" withAltEscapeSequence:@"\x1b[18;3~"];
        [self addFunctionKey:UIKeyInputF8 withName:@"F8" withNormalEscapeSequence:@"\x1b[19~" withShiftEscapeSequence:@"\x1b[19;2~" withControlEscapeSequence:@"\x1b[19;5~" withAltEscapeSequence:@"\x1b[19;3~"];
        [self addFunctionKey:UIKeyInputF9 withName:@"F9" withNormalEscapeSequence:@"\x1b[20~" withShiftEscapeSequence:@"\x1b[20;2~" withControlEscapeSequence:@"\x1b[20;5~" withAltEscapeSequence:@"\x1b[20;3~"];
        [self addFunctionKey:UIKeyInputF10 withName:@"F10" withNormalEscapeSequence:@"\x1b[21~" withShiftEscapeSequence:@"\x1b[21;2~" withControlEscapeSequence:@"\x1b[21;5~" withAltEscapeSequence:@"\x1b[21;3~"];
        // Yes, @"\x1b[22~" is missing; it is meant to be
        [self addFunctionKey:UIKeyInputF11 withName:@"F11" withNormalEscapeSequence:@"\x1b[23~" withShiftEscapeSequence:@"\x1b[23;2~" withControlEscapeSequence:@"\x1b[23;5~" withAltEscapeSequence:@"\x1b[23;3~"];
        [self addFunctionKey:UIKeyInputF12 withName:@"F12" withNormalEscapeSequence:@"\x1b[24~" withShiftEscapeSequence:@"\x1b[24;2~" withControlEscapeSequence:@"\x1b[24;5~" withAltEscapeSequence:@"\x1b[24;3~"];
    } else {
        for (NSString *specialKey in @[UIKeyInputEscape, UIKeyInputUpArrow, UIKeyInputDownArrow,
                                   UIKeyInputLeftArrow, UIKeyInputRightArrow, @"\t"]) {
        [self addKey:specialKey withModifiers:0];
        }
    }
    if (UserPreferences.shared.capsLockMapping != CapsLockMapNone) {
        if (@available(iOS 13, *)); else {
            [self addKeys:controlKeys withModifiers:UIKeyModifierAlphaShift];
            [self addKeys:alphabet withModifiers:0];
            [self addKeys:alphabet withModifiers:UIKeyModifierShift];
            [self addKey:@"" withModifiers:UIKeyModifierAlphaShift]; // otherwise tap of caps lock can switch layouts
        }
    }
    if (UserPreferences.shared.optionMapping == OptionMapEsc) {
        [self addKeys:metaKeys withModifiers:UIKeyModifierAlternate];
    }
    if (UserPreferences.shared.backtickMapEscape) {
        [self addKey:@"`" withModifiers:0];
    }
    [_keyCommands addObject:[UIKeyCommand keyCommandWithInput:@"k"
                                                modifierFlags:UIKeyModifierCommand|UIKeyModifierShift
                                                       action:@selector(clearScrollback:)
                                         discoverabilityTitle:NSLocalizedString(@"Clear Scrollback", @"Hardware keyboard shortcut title")]];
    // Suspend to disk. Not implemented here: it goes up the responder chain to
    // TerminalViewController, which owns the save.
    //
    // This is the ONLY way to reach it with a hardware keyboard attached. The
    // accessory bar carries the button, and a hardware keyboard is exactly when
    // that bar is not on screen -- which is also exactly the setup someone
    // leaves running for hours and most wants to save. Registered even when the
    // preference is off, so holding Command advertises that the feature exists;
    // the handler explains rather than doing nothing.
    //
    // Command is free here: iSH-AOK sends Control and Alt to the guest and
    // keeps Command for the app, which is why Clear Scrollback could take
    // Command-Shift-K above.
    [_keyCommands addObject:[UIKeyCommand keyCommandWithInput:@"s"
                                                modifierFlags:UIKeyModifierCommand
                                                       action:@selector(saveSessionFromKeyCommand:)
                                         discoverabilityTitle:NSLocalizedString(@"Save Session", @"Hardware keyboard shortcut title")]];

    return _keyCommands;
}

- (void)addKeys:(const char *)keys withModifiers:(UIKeyModifierFlags)modifiers {
    for (size_t i = 0; keys[i] != '\0'; i++) {
        // Bolt: Optimize single-character NSString creation.
        // initWithBytes is significantly faster than parsing a format string
        // via stringWithFormat: in a tight initialization loop.
        NSString *keyStr = [[NSString alloc] initWithBytes:&keys[i] length:1 encoding:NSUTF8StringEncoding];
        if (!keyStr) {
            keyStr = [NSString stringWithFormat:@"%c", keys[i]];
        }
        [self addKey:keyStr withModifiers:modifiers];
    }
}

- (void)addKey:(NSString *)key withModifiers:(UIKeyModifierFlags)modifiers {
    UIKeyCommand *command = [UIKeyCommand keyCommandWithInput:key
                                                modifierFlags:modifiers
                                                       action:@selector(handleKeyCommand:)];
    if (@available(iOS 15, *)) {
        command.wantsPriorityOverSystemBehavior = YES;
    }

    [_keyCommands addObject:command];
}

- (void)addFunctionKey:(NSString *)keyName withName:(NSString *)keyTitle withNormalEscapeSequence:(NSString *)normalEscapeSequence withShiftEscapeSequence:(NSString *)shiftEscapeSequence withControlEscapeSequence:(NSString *)controlEscapeSequence withAltEscapeSequence:(NSString *)altEscapeSequence API_AVAILABLE(ios(13.4)) {

    UIKeyCommand *command;

    if ([keyName isEqualToString:UIKeyInputUpArrow] ||
            [keyName isEqualToString:UIKeyInputDownArrow] ||
            [keyName isEqualToString:UIKeyInputLeftArrow] ||
            [keyName isEqualToString:UIKeyInputRightArrow]) {
        command = [UIKeyCommand keyCommandWithInput:keyName
                                      modifierFlags:0
                                             action:@selector(handleKeyCommand:)];
    } else {
        command = [UIKeyCommand commandWithTitle: @"" image: nil action:@selector(handleKeyCommand:) input: keyName modifierFlags:0 propertyList:normalEscapeSequence];
    }
    if (@available(iOS 15, *)) {
        command.wantsPriorityOverSystemBehavior = YES;
    }
    [_keyCommands addObject:command];

    // Only register the modifier variants that actually have an escape
    // sequence to send. A NULL sequence used to be registered anyway, and
    // handleKeyCommand silently ignores a nil propertyList on a modified
    // key -- so the terminal CLAIMED the chord (first responder wins over
    // ancestors in UIKeyCommand dispatch) and then did nothing with it,
    // black-holing it for the whole app. Concretely: Tab's control
    // sequence is NULL, so Ctrl+Tab died here whenever a terminal had
    // focus, which broke the Workspace's Ctrl+Tab window cycling
    // ("worked once" -- until the cycle handed focus to a terminal).
    // Skipping the dead registrations lets those chords bubble up the
    // responder chain to whoever can actually handle them.
    if (shiftEscapeSequence != NULL) {
        command = [UIKeyCommand commandWithTitle: @"" image: nil action:@selector(handleKeyCommand:) input: keyName modifierFlags:UIKeyModifierShift propertyList:shiftEscapeSequence];
        if (@available(iOS 15, *)) {
            command.wantsPriorityOverSystemBehavior = YES;
        }
        [_keyCommands addObject:command];
    }

    if (controlEscapeSequence != NULL) {
        command = [UIKeyCommand commandWithTitle: @"" image: nil action:@selector(handleKeyCommand:) input: keyName modifierFlags:UIKeyModifierControl propertyList:controlEscapeSequence];
        if (@available(iOS 15, *)) {
            command.wantsPriorityOverSystemBehavior = YES;
        }
        [_keyCommands addObject:command];
    }
    if (altEscapeSequence != NULL) {
        command = [UIKeyCommand commandWithTitle: @"" image: nil action:@selector(handleKeyCommand:) input: keyName modifierFlags:UIKeyModifierAlternate propertyList:altEscapeSequence];
        if (@available(iOS 15, *)) {
            command.wantsPriorityOverSystemBehavior = YES;
        }
        [_keyCommands addObject:command];
    }
}

- (void)keyCommandTriggered:(UIKeyCommand *)sender {
    dispatch_async(dispatch_get_main_queue(), ^{
        [self handleKeyCommand:sender];
    });
}

- (void)handleCapsLockWithCommand:(UIKeyCommand *)command {
    CapsLockMapping target = UserPreferences.shared.capsLockMapping;
    NSString *newInput = command.input ? command.input : @"";
    UIKeyModifierFlags flags = command.modifierFlags;
    flags ^= UIKeyModifierAlphaShift;
    if(target == CapsLockMapEscape) {
        newInput = UIKeyInputEscape;
    } else if(target == CapsLockMapControl) {
        if([newInput length] == 0) {
            return;
        }
        flags |= UIKeyModifierControl;
    } else {
        return;
    }

    UIKeyCommand *newCommand = [UIKeyCommand keyCommandWithInput:newInput
                                                   modifierFlags:flags
                                                          action:@selector(keyCommandTriggered:)];
    [self handleKeyCommand:newCommand];
}

// UIKit repeats a held key only for keys claimed by a UIKeyCommand, so h/j/k/l
// used to be registered as bare key commands just to get vi movement to repeat.
// That bought repeat at the cost of ordering: a key command is dispatched
// straight off the key event (~0.3 ms after the press), while an ordinary
// printable character reaches -insertText: through UIKit's text-input pipeline
// (~5 ms after the press, and further behind once that pipeline has a backlog).
// Two delivery paths with nothing sequencing them, so h/j/k/l overtook the
// characters typed before them -- "abcdefghijklmnopqrst" reached the tty as
// "abcdhejkflgimnopqrst", with only h, j, k and l out of place. The window is
// about 5 ms, so it takes a machine to type into it, but the terminal has no
// business reordering anything it is handed.
//
// Repeat is driven from the press stream instead: the first character of a hold
// travels the same in-order path as every other letter, and only the repeats are
// synthesised here. Timings match what UIKit's key-command repeat did (a 0.4 s
// delay, then ~0.1 s per repeat), measured against the previous build.
static const NSTimeInterval kKeyRepeatDelay = 0.4;
static const NSTimeInterval kKeyRepeatInterval = 0.1;

- (void)stopKeyRepeat {
    [self.keyRepeatTimer invalidate];
    self.keyRepeatTimer = nil;
    self.keyRepeatText = nil;
}

- (void)startKeyRepeatForPresses:(NSSet<UIPress *> *)presses API_AVAILABLE(ios(13.4)) {
    // Any new press ends the previous key's repeat, which is what a real
    // keyboard does when you roll from one key on to the next.
    [self stopKeyRepeat];
    // A key typed into a composition is the input method's, not hjkl.
    if (presses.count != 1 || self.composition != nil)
        return;
    UIKey *key = presses.anyObject.key;
    if (key == nil || key.modifierFlags != 0)
        return;
    NSString *characters = key.charactersIgnoringModifiers;
    if (characters.length != 1)
        return;
    unichar c = [characters characterAtIndex:0];
    if (c == 0 || c > 0x7f || strchr(viRepeatKeys, (char) c) == NULL)
        return;
    self.keyRepeatText = characters;
    // Repeats at kKeyRepeatInterval, with the first one held off until
    // kKeyRepeatDelay so a normal tap never produces a second character.
    __weak typeof(self) weakSelf = self;
    NSTimer *timer = [NSTimer timerWithTimeInterval:kKeyRepeatInterval repeats:YES block:^(NSTimer *t) {
        typeof(self) strongSelf = weakSelf;
        NSString *text = strongSelf.keyRepeatText;
        // The press may have opened a composition after all (a Pinyin
        // hardware keyboard turns a letter into pinyin); repeating it into the
        // guest behind the IME's back would be wrong.
        if (strongSelf == nil || text == nil || strongSelf.composition != nil) {
            [t invalidate];
            return;
        }
        // The sink the key-command handler used, so a synthesised repeat is
        // indistinguishable from the one UIKit used to deliver (including the
        // on-screen Ctrl modifier, if it happens to be armed).
        [strongSelf insertText:text];
    }];
    timer.fireDate = [NSDate dateWithTimeIntervalSinceNow:kKeyRepeatDelay];
    timer.tolerance = kKeyRepeatInterval / 4;
    // Common modes: a repeat must keep going while the scroll view is tracking,
    // which is exactly when the default mode stops servicing timers.
    [NSRunLoop.mainRunLoop addTimer:timer forMode:NSRunLoopCommonModes];
    self.keyRepeatTimer = timer;
}

- (void)pressesBegan:(NSSet<UIPress *> *)presses withEvent:(UIPressesEvent *)event {
    if (@available(iOS 13.4, *)) {
        [self startKeyRepeatForPresses:presses];
        // Reserve each press's place in the input stream before anything can
        // produce bytes for it. Skipped for presses that cannot reach the tty,
        // so they never hold the queue: a modifier on its own produces no
        // characters, and a Command chord is always somebody else's (the
        // terminal only ever acts on shift/alt/caps/control).
        self.keyEventSlot = nil;
        for (UIPress *press in presses) {
            UIKey *key = press.key;
            if (key == nil || key.characters.length == 0)
                continue;
            // Typed into an open composition: the IME takes it, and nothing
            // reaches the tty until a candidate is committed.
            if (self.composition != nil)
                continue;
            if (key.modifierFlags & UIKeyModifierCommand)
                continue;
            self.keyEventSlot = [self reserveInputSlotForInput:key.charactersIgnoringModifiers];
        }
    }
    bool handled = false;
    UIKeyModifierFlags modifier;

    self.immediateKeyInputDepth++;
    if (@available(iOS 13.4, *)) {
        // this is all to handle Ins/Help key as Apple don't support that key using UIKey interface
        UIKey *key;

        for (UIPress *aPress in presses) {
            key = aPress.key;
            handled = false;
            // Use of UIKeyboardHID was introduced in 13.4

            // ignore modifier keys by themselves
            if ( key.keyCode == UIKeyboardHIDUsageKeyboardLeftShift || key.keyCode == UIKeyboardHIDUsageKeyboardLeftControl || key.keyCode == UIKeyboardHIDUsageKeyboardLeftAlt || key.keyCode == UIKeyboardHIDUsageKeyboardRightShift || key.keyCode == UIKeyboardHIDUsageKeyboardRightControl || key.keyCode == UIKeyboardHIDUsageKeyboardRightAlt || key.keyCode == UIKeyboardHIDUsageKeyboardRightGUI ) {
                continue;
            }

            modifier = key.modifierFlags;
            if ( modifier & UIKeyModifierNumericPad ) {
                modifier &= ~UIKeyModifierNumericPad;
            }
            if ( modifier & UIKeyModifierAlphaShift) {
                modifier &= ~UIKeyModifierAlphaShift;
            }
            if ( key.keyCode == UIKeyboardHIDUsageKeyboardInsert || key.keyCode == UIKeyboardHIDUsageKeyboardHelp ) {
                if ( modifier == 0) {
                    [self insertRawText:@"\x1b[2~"];
                    handled = true;
                    break;
                }
                if ( modifier & UIKeyModifierShift )  {
                    [self insertRawText:@"\x1b[2;2~"];
                    handled = true;
                    break;
                }
                if ( modifier & UIKeyModifierControl )  {
                    [self insertRawText:@"\x1b[2;5~"];
                    handled = true;
                    break;
                }
            }
        }
    }
    self.immediateKeyInputDepth--;
    if ( !handled) {
       return [super pressesBegan:presses withEvent:event];
    }
}

- (void)pressesEnded:(NSSet<UIPress *> *)presses withEvent:(UIPressesEvent *)event {
    [self stopKeyRepeat];
}

- (void)pressesCancelled:(NSSet<UIPress *> *)presses withEvent:(UIPressesEvent *)event {
    [self stopKeyRepeat];
}

// conforming to UITextInput makes this view default to being an accessibility element, which blocks selecting anything in it
- (BOOL)isAccessibilityElement { return NO; }

@end
