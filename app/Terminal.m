//
//  Terminal.m
//  iSH
//
//  Created by Theodore Dubois on 10/18/17.
//

#import "Terminal.h"
#import "DelayedUITask.h"
#import "Diagnostics.h"
#import "UserPreferences.h"
#include "LinuxInterop.h"
#include "fs/dev.h"
#include "fs/devices.h"
#include "fs/tty.h"
#include "fs/devices.h"
#include "util/ro_locks.h"
#include <stdlib.h>
#include <string.h>

extern struct tty_driver ios_pty_driver;

#if !ISH_LINUX
typedef struct tty *tty_t;
#else
typedef struct linux_tty *tty_t;
#endif

NSNotificationName const TerminalLoadFailedNotification = @"TerminalLoadFailedNotification";
NSNotificationName const TerminalDidLoadNotification = @"TerminalDidLoadNotification";
NSNotificationName const TerminalRegistryDidChangeNotification = @"TerminalRegistryDidChangeNotification";

@interface Terminal () <WKScriptMessageHandler, WKNavigationDelegate> {
#if !ISH_LINUX
    lock_t _dataLock;
    cond_t _dataConsumed;
#endif
}

@property BOOL loaded;
@property BOOL didReportLoadFailure;
@property (nonatomic) tty_t tty;
// lock with dataLock for !linux and @synchronized(self) for linux
@property (nonatomic) NSMutableData *pendingData;
// sending output is an asynchronous thing due to javascript, this is used to ensure it doesn't happen twice at once
@property (nonatomic) BOOL outputInProgress;
@property (nonatomic) NSData *inFlightData;
@property (nonatomic) NSUInteger outputGeneration;
@property (nonatomic) CFTimeInterval outputStartedAt;

@property DelayedUITask *refreshTask;
@property DelayedUITask *scrollToBottomTask;

@property BOOL applicationCursor;

@property NSNumber *terminalsKey;
@property NSUUID *uuid;
@property (nonatomic, copy) NSString *pendingDestroyReason;

@end

@interface CustomWebView : WKWebView
@end
@implementation CustomWebView
- (BOOL)becomeFirstResponder {
    if (@available(iOS 13.4, *)) {
        return [super becomeFirstResponder];
    }
    return NO;
}

- (BOOL)canPerformAction:(SEL)action withSender:(id)sender {
    if (action == @selector(copy:) || action == @selector(paste:)) {
        return NO;
    }
    return [super canPerformAction:action withSender:sender];
}

- (UIView *)snapshotViewAfterScreenUpdates:(BOOL)afterUpdates {
    if (self.window == nil || self.hidden || self.alpha <= 0.0) {
        return [super snapshotViewAfterScreenUpdates:YES];
    }
    return [super snapshotViewAfterScreenUpdates:afterUpdates];
}

- (UIView *)resizableSnapshotViewFromRect:(CGRect)rect
                       afterScreenUpdates:(BOOL)afterUpdates
                            withCapInsets:(UIEdgeInsets)capInsets {
    if (self.window == nil || self.hidden || self.alpha <= 0.0) {
        return [super resizableSnapshotViewFromRect:rect
                                 afterScreenUpdates:YES
                                      withCapInsets:capInsets];
    }
    return [super resizableSnapshotViewFromRect:rect
                             afterScreenUpdates:afterUpdates
                                  withCapInsets:capInsets];
}
@end

@implementation Terminal
@synthesize webView = _webView;

static const int BUF_SIZE = 1<<14;

static NSMapTable<NSNumber *, Terminal *> *terminals;
static NSMapTable<NSUUID *, Terminal *> *terminalsByUUID;

static NSString *ISHJavaScriptLiteralForTerminalData(NSData *data) {
    const unsigned char *bytes = data.bytes;
    NSUInteger len = data.length;
    if (len == 0)
        return @"";
    // This runs on every screen refresh and dominates bulk-output throughput.
    // The previous version did an Objective-C method dispatch (and, for every
    // printable byte, a format-string parse via appendFormat:@"%c") per byte.
    // Build the whole escaped literal in one C buffer instead and make a single
    // NSString. Worst case is 4 output chars ("\xNN") per input byte. Every
    // byte emitted is ASCII, so NSASCIIStringEncoding is lossless.
    char *out = malloc(len * 4 + 1);
    if (out == NULL)
        return @"";
    static const char hex[] = "0123456789abcdef";
    size_t o = 0;
    for (NSUInteger i = 0; i < len; i++) {
        unsigned char byte = bytes[i];
        switch (byte) {
            case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
            case '"':  out[o++] = '\\'; out[o++] = '"';  break;
            case '\r': out[o++] = '\\'; out[o++] = 'r';  break;
            case '\n': out[o++] = '\\'; out[o++] = 'n';  break;
            case '\t': out[o++] = '\\'; out[o++] = 't';  break;
            default:
                if (byte >= 0x20 && byte <= 0x7e) {
                    out[o++] = (char) byte;
                } else {
                    // Prompts and line editing emit raw escape/control bytes.
                    out[o++] = '\\';
                    out[o++] = 'x';
                    out[o++] = hex[byte >> 4];
                    out[o++] = hex[byte & 0xf];
                }
                break;
        }
    }
    NSString *literal = [[NSString alloc] initWithBytes:out length:o encoding:NSASCIIStringEncoding];
    free(out);
    return literal ?: @"";
}

static BOOL TerminalShouldMirrorDebugOutput(Terminal *terminal) {
    if (terminal == nil || terminal.type != 136 || terminal.number != 1)
        return NO;
    const char *enabled = getenv("ISH_DEBUG_MIRROR_TTY1_OUTPUT");
    return enabled != NULL && enabled[0] != '\0' && strcmp(enabled, "0") != 0;
}

static void TerminalDebugMirrorOutput(Terminal *terminal, const void *buf, int len) {
    if (!TerminalShouldMirrorDebugOutput(terminal) || buf == NULL || len <= 0)
        return;
    @autoreleasepool {
        NSData *data = [NSData dataWithBytes:buf length:(NSUInteger) len];
        NSString *escaped = ISHJavaScriptLiteralForTerminalData(data);
        fprintf(stderr, "ish-tty1-output: \"%s\"\n", escaped.UTF8String ?: "");
        fflush(stderr);
    }
}

static NSString *ISHTerminalTypeString(int type) {
    if (type == TTY_CONSOLE_MAJOR)
        return @"console";
    if (type == TTY_PSEUDO_SLAVE_MAJOR)
        return @"pty-slave";
    if (type == TTY_PSEUDO_MASTER_MAJOR)
        return @"pty-master";
    return [NSString stringWithFormat:@"%d", type];
}

static BOOL ISHTerminalLifecycleLogEnabled(void) {
    const char *enabled = getenv("ISH_TRACE_TERMINAL_LIFECYCLE");
    return enabled != NULL && enabled[0] != '\0' && strcmp(enabled, "0") != 0;
}

static CFTimeInterval ISHTerminalNowMonotonic(void) {
    return CACurrentMediaTime();
}

static const CFTimeInterval ISHTerminalOutputWatchdogSeconds = 1.5;

static NSString *ISHStringFromBOOL(BOOL value) {
    return value ? @"yes" : @"no";
}

static NSString *TerminalDebugReadRowsForTerminal(Terminal *terminal, int maxRows) {
    if (terminal == nil)
        return @"<no-terminal>";

    WKWebView *webView = terminal.webView;
    if (webView == nil)
        return @"<no-webview>";

    __block NSString *output = @"<pending>";
    __block BOOL done = NO;
    int rows = maxRows > 0 ? maxRows : 0;
    NSString *script = [NSString stringWithFormat:@"term.getRowsText(Math.max(0,term.getRowCount()-%d), term.getRowCount())", rows];
    void (^readBlock)(void) = ^{
        [webView evaluateJavaScript:script completionHandler:^(id result, NSError *error) {
            if ([result isKindOfClass:NSString.class]) {
                output = result;
            } else if (error != nil) {
                output = error.localizedDescription ?: @"<error>";
            } else {
                output = @"<no-output>";
            }
            done = YES;
        }];
    };

    if (NSThread.isMainThread) {
        readBlock();
    } else {
        dispatch_async(dispatch_get_main_queue(), readBlock);
    }

    while (!done) {
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
    }
    return output;
}

__attribute__((used))
NSString *Terminal_debugReadRows(int type, int number, int maxRows) {
    @autoreleasepool {
        return TerminalDebugReadRowsForTerminal([Terminal terminalWithType:type number:number], maxRows);
    }
}

__attribute__((used))
NSString *Terminal_debugSendInputUTF8(int type, int number, const char *input) {
    @autoreleasepool {
        Terminal *terminal = [Terminal terminalWithType:type number:number];
        if (terminal == nil)
            return @"<no-terminal>";
        if (input == NULL)
            return @"<null-input>";

        NSString *string = [NSString stringWithUTF8String:input];
        if (string == nil)
            return @"<invalid-utf8>";

        NSData *data = [string dataUsingEncoding:NSUTF8StringEncoding];
        [terminal sendInput:data];
        return @"ok";
    }
}

__attribute__((used))
int Terminal_debugSendInputUTF8Sync(int type, int number, const char *input) {
    @autoreleasepool {
        Terminal *terminal = [Terminal terminalWithType:type number:number];
        if (terminal == nil)
            return -1;
        if (input == NULL)
            return -2;
        if (terminal.tty == NULL)
            return -3;

        size_t length = strlen(input);
        char *copy = NULL;
        if (length > 0) {
            copy = malloc(length);
            if (copy == NULL)
                return -4;
            memcpy(copy, input, length);
            uint8_t first = (uint8_t) copy[0];
            [terminal recordLifecycleEvent:@"terminal.debugSendInputSync"
                                   details:@{@"bytes": @(length),
                                             @"firstByte": [NSString stringWithFormat:@"%#x", first]}];
        }

#if !ISH_LINUX
        tty_input(terminal.tty, copy, length, 0);
        free(copy);
#else
        sync_do_in_workqueue(^(void (^done)(void)) {
            terminal.tty->ops->send_input(terminal.tty, copy, length);
            free(copy);
            done();
        });
#endif
        return 0;
    }
}

static void NotifyTerminalRegistryChanged(void) {
    dispatch_async(dispatch_get_main_queue(), ^{
        [NSNotificationCenter.defaultCenter postNotificationName:TerminalRegistryDidChangeNotification object:nil];
    });
}

- (instancetype)initWithType:(int)type number:(int)num {
    @synchronized (Terminal.class) {
        self.terminalsKey = @(dev_make(type, num));
        Terminal *terminal = [terminals objectForKey:self.terminalsKey];
        if (terminal)
            return terminal;

        if (self = [super init]) {
            self.pendingData = [[NSMutableData alloc] initWithCapacity:BUF_SIZE];
            self.refreshTask = [[DelayedUITask alloc] initWithTarget:self action:@selector(refresh)];
            self.scrollToBottomTask = [[DelayedUITask alloc] initWithTarget:self action:@selector(scrollToBottom)];
#if !ISH_LINUX
            lock_init(&_dataLock, "datalock\0");
            cond_init(&_dataConsumed);
#endif

            [terminals setObject:self forKey:self.terminalsKey];
            self.uuid = [NSUUID UUID];
            [terminalsByUUID setObject:self forKey:self.uuid];
            NotifyTerminalRegistryChanged();
        }
        return self;
    }
}

- (void)reportTerminalLoadFailure:(NSError *)error {
    if (self.didReportLoadFailure)
        return;
    self.didReportLoadFailure = YES;
    [ISHDiagnosticsStore recordBreadcrumb:@"terminal.loadFailure"
                                  details:@{@"terminalUUID": self.uuid.UUIDString ?: @"",
                                            @"error": error.localizedDescription ?: @"unknown"}];
    NSLog(@"Terminal %@ failed to load: %@",
          self.uuid.UUIDString ?: @"(unknown)",
          error.localizedDescription ?: @"unknown error");
    dispatch_async(dispatch_get_main_queue(), ^{
        [NSNotificationCenter.defaultCenter postNotificationName:TerminalLoadFailedNotification
                                                          object:self
                                                        userInfo:error != nil ? @{@"error": error} : @{}];
    });
}

- (void)resetOutputStateAndRequeueInFlightDataLocked {
    NSData *retryData = self.inFlightData;
    if (retryData.length > 0) {
        NSMutableData *restored = [[NSMutableData alloc] initWithCapacity:retryData.length + self.pendingData.length];
        [restored appendData:retryData];
        [restored appendData:self.pendingData];
        self.pendingData = restored;
    }
    self.inFlightData = nil;
    self.outputInProgress = NO;
    self.outputStartedAt = 0;
    self.outputGeneration++;
}

- (void)recoverTerminalWebViewWithReason:(NSString *)reason error:(NSError *)error {
    dispatch_async(dispatch_get_main_queue(), ^{
        NSUInteger pendingBytes = 0;
#if !ISH_LINUX
        lock(&self->_dataLock, 0);
        [self resetOutputStateAndRequeueInFlightDataLocked];
        pendingBytes = self.pendingData.length;
        unlock(&self->_dataLock);
#else
        @synchronized (self) {
            [self resetOutputStateAndRequeueInFlightDataLocked];
            pendingBytes = self.pendingData.length;
        }
#endif
        [self recordLifecycleEvent:@"terminal.webview.recover"
                           details:@{@"reason": reason ?: @"unknown",
                                     @"pendingBytes": @(pendingBytes),
                                     @"error": error.localizedDescription ?: @"unknown"}];
        self.loaded = NO;
        self.didReportLoadFailure = NO;
        WKWebView *oldWebView = _webView;
        oldWebView.navigationDelegate = nil;
        [oldWebView stopLoading];
        [oldWebView removeFromSuperview];
        _webView = nil;
        [self webView];
    });
}

- (void)recordLifecycleEvent:(NSString *)event details:(NSDictionary<NSString *, id> *)details {
    NSMutableDictionary<NSString *, id> *payload = [NSMutableDictionary dictionaryWithDictionary:details ?: @{}];
    payload[@"terminalUUID"] = self.uuid.UUIDString ?: @"";
    payload[@"type"] = ISHTerminalTypeString(self.type);
    payload[@"number"] = @(self.number);
    payload[@"loaded"] = ISHStringFromBOOL(self.loaded);
    [ISHDiagnosticsStore recordBreadcrumb:event details:payload];
    if (ISHTerminalLifecycleLogEnabled()) {
        NSLog(@"%@ %@ type=%@ num=%d loaded=%@ details=%@",
              event, self.uuid.UUIDString ?: @"", ISHTerminalTypeString(self.type), self.number,
              ISHStringFromBOOL(self.loaded), payload);
    }
}

- (WKWebView *)webView {
    if (_webView == nil) {
        WKWebViewConfiguration *config = [WKWebViewConfiguration new];
        [config.userContentController addScriptMessageHandler:self name:@"load"];
        [config.userContentController addScriptMessageHandler:self name:@"log"];
        [config.userContentController addScriptMessageHandler:self name:@"sendInput"];
        [config.userContentController addScriptMessageHandler:self name:@"resize"];
        [config.userContentController addScriptMessageHandler:self name:@"propUpdate"];
        // Make the web view really big so that if a program tries to write to the terminal before it's displayed, the text probably won't wrap too badly.
        CGRect webviewSize = CGRectMake(0, 0, 10000, 10000);
        _webView = [[CustomWebView alloc] initWithFrame:webviewSize configuration:config];
        _webView.navigationDelegate = self;
        _webView.scrollView.scrollEnabled = NO;
        NSURL *xtermHtmlFile = [NSBundle.mainBundle URLForResource:@"term" withExtension:@"html"];
        if (xtermHtmlFile == nil) {
            NSError *error = [NSError errorWithDomain:NSCocoaErrorDomain
                                                 code:NSFileNoSuchFileError
                                             userInfo:@{NSLocalizedDescriptionKey: @"missing bundled terminal UI"}];
            [self reportTerminalLoadFailure:error];
            [_webView loadHTMLString:@"<html><body style='background:black;color:white;font-family:-apple-system;padding:1rem'>Terminal UI failed to load.</body></html>"
                             baseURL:nil];
            return _webView;
        }
        NSURL *readAccessURL = xtermHtmlFile.URLByDeletingLastPathComponent ?: NSBundle.mainBundle.resourceURL ?: xtermHtmlFile;
        [_webView loadFileURL:xtermHtmlFile allowingReadAccessToURL:readAccessURL];
    }
    return _webView;
}

#if !ISH_LINUX
+ (Terminal *)createPseudoTerminal:(struct tty **)tty {
    *tty = pty_open_fake(&ios_pty_driver);
    if (IS_ERR(*tty))
        return nil;
    return (__bridge Terminal *) (*tty)->data;
}
#endif

- (void)setTty:(tty_t)tty {
    @synchronized (self) {
        _tty = tty;
    }
    dispatch_async(dispatch_get_main_queue(), ^{
        [self syncWindowSize];
    });
}

- (void)userContentController:(WKUserContentController *)userContentController didReceiveScriptMessage:(WKScriptMessage *)message {
    if ([message.name isEqualToString:@"load"]) {
        self.loaded = YES;
        self.didReportLoadFailure = NO;
        [self recordLifecycleEvent:@"terminal.webview.load"
                           details:@{@"script": @"load"}];
        if (ISHTerminalLifecycleLogEnabled()) {
            NSLog(@"Terminal %@ finished loading terminal UI", self.uuid.UUIDString ?: @"(unknown)");
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            [NSNotificationCenter.defaultCenter postNotificationName:TerminalDidLoadNotification
                                                              object:self];
        });
        [self syncWindowSize];
        [self.refreshTask schedule];
        // make sure this setting works if it's set before loading
        self.enableVoiceOverAnnounce = self.enableVoiceOverAnnounce;
    } else if ([message.name isEqualToString:@"sendInput"]) {
        NSData *data = [message.body dataUsingEncoding:NSUTF8StringEncoding];
        [self recordLifecycleEvent:@"terminal.webview.sendInput"
                           details:@{@"bytes": @(data.length)}];
        [self sendInput:data];
    } else if ([message.name isEqualToString:@"resize"]) {
        [self recordLifecycleEvent:@"terminal.webview.resize" details:nil];
        [self syncWindowSize];
    } else if ([message.name isEqualToString:@"propUpdate"]) {
        [self setValue:message.body[1] forKey:message.body[0]];
    }
}

- (void)webView:(WKWebView *)webView didFailNavigation:(WKNavigation *)navigation withError:(NSError *)error {
    [self reportTerminalLoadFailure:error];
}

- (void)webView:(WKWebView *)webView didFailProvisionalNavigation:(WKNavigation *)navigation withError:(NSError *)error {
    [self reportTerminalLoadFailure:error];
}

- (void)webViewWebContentProcessDidTerminate:(WKWebView *)webView {
    self.loaded = NO;
    [self recordLifecycleEvent:@"terminal.webContentProcessTerminated"
                       details:@{@"webViewHidden": ISHStringFromBOOL(webView.isHidden)}];
    NSError *error = [NSError errorWithDomain:WKErrorDomain
                                         code:WKErrorWebContentProcessTerminated
                                     userInfo:@{NSLocalizedDescriptionKey: @"terminal web content process terminated"}];
    [self reportTerminalLoadFailure:error];
    [self recoverTerminalWebViewWithReason:@"webContentProcessTerminated" error:error];
}

- (void)syncWindowSize {
    if (!self.loaded)
        return;
    [self.webView evaluateJavaScript:@"exports.getSize()" completionHandler:^(NSArray<NSNumber *> *dimensions, NSError *error) {
        if (error != nil || dimensions.count < 2)
            return;
        int cols = dimensions[0].intValue;
        int rows = dimensions[1].intValue;
        // Read the unowned back-pointer once. ios_tty_cleanup clears it from
        // the emulator thread, so re-reading self.tty for each use means the
        // NULL check guards nothing: the pointer could still be live here and
        // NULL by the time it is used.
        //
        // Read it rather than holding @synchronized(self) across tty->lock:
        // tty_release calls ops->cleanup (-> setTty: -> @synchronized(self))
        // with tty->lock already held, so taking those two in the other order
        // here would be an AB-BA deadlock.
        tty_t tty;
        @synchronized (self) {
            tty = self->_tty;
        }
        if (tty == NULL)
            return;
#if !ISH_LINUX
        // Not enough on its own: tty_release frees the struct, so even a
        // correctly-read pointer can go stale before it is dereferenced. Re-find
        // the tty under ttys_lock -- the lock that free is required to hold --
        // and hold a reference for as long as it is used. The value read above
        // is passed only as an identity token (compared, never dereferenced),
        // so a recycled pty number can't point this at another terminal's tty.
        tty = tty_lookup_ref(self.type, self.number, tty);
        if (tty == NULL)
            return;
        lock(&tty->lock, 0);
        tty_set_winsize(tty, (struct winsize_) {.col = cols, .row = rows});
        unlock(&tty->lock);
        tty_put(tty);
#else
        async_do_in_workqueue(^{
            tty->ops->resize(tty, cols, rows);
        });
#endif
    }];
}

- (void)setEnableVoiceOverAnnounce:(BOOL)enableVoiceOverAnnounce {
    _enableVoiceOverAnnounce = enableVoiceOverAnnounce;
    if (!self.loaded)
        return;
    [self.webView evaluateJavaScript:[NSString stringWithFormat:@"term.setAccessibilityEnabled(%@)",
                                      enableVoiceOverAnnounce ? @"true" : @"false"]
                   completionHandler:nil];
}

- (int)sendOutput:(const void *)buf length:(int)len {
    TerminalDebugMirrorOutput(self, buf, len);
#if !ISH_LINUX
    lock(&_dataLock, 0);
    if (!NSThread.isMainThread) {
        if (!self.loaded) {
            // Hidden/background consoles (for example tty2-tty6 getty instances
            // started by init) may never get a web view to drain them. Keep a
            // bounded tail of recent output instead of blocking guest writers
            // forever once the buffer fills.
            if (len > BUF_SIZE) {
                buf = (const char *) buf + (len - BUF_SIZE);
                len = BUF_SIZE;
                [_pendingData setLength:0];
            } else {
                NSUInteger needed = (NSUInteger) len;
                NSUInteger available = _pendingData.length >= BUF_SIZE ? 0 : (NSUInteger) (BUF_SIZE - _pendingData.length);
                if (needed > available) {
                    NSUInteger discard = MIN(_pendingData.length, needed - available);
                    [_pendingData replaceBytesInRange:NSMakeRange(0, discard) withBytes:NULL length:0];
                }
            }
        } else {
            // The main thread is the only one that can unblock this, so sleeping
            // here would be a deadlock. The only reason for this to be called on
            // the main thread is if input is echoed.
            while (_pendingData.length > BUF_SIZE)
                wait_for_ignore_signals(&_dataConsumed, &_dataLock, NULL);
        }
    }
    [_pendingData appendBytes:buf length:(NSUInteger) len];
    [self.refreshTask schedule];
    unlock(&_dataLock);
#else
    @synchronized (self) {
        int room = [self roomForOutput];
        if (len > room)
            len = room;
        if (len > 0) {
            [_pendingData appendBytes:buf length:(NSUInteger) len];
            [_refreshTask schedule];
        }
    }
#endif
    return len;
}

#if ISH_LINUX
- (int)roomForOutput {
    @synchronized (self) {
        if (_pendingData.length > BUF_SIZE)
            return 0;
        return BUF_SIZE - (int) _pendingData.length;
    }
}
#endif

- (void)sendInput:(NSData *)input {
    tty_t tty;
    @synchronized (self) {
        tty = self->_tty;
    }
    if (tty == NULL)
        return;
    if (input.length > 0) {
        uint8_t first = ((const uint8_t *) input.bytes)[0];
        [self recordLifecycleEvent:@"terminal.sendInput"
                           details:@{@"bytes": @(input.length),
                                     @"firstByte": [NSString stringWithFormat:@"%#x", first]}];
    }
#if !ISH_LINUX
    // Hold a reference across tty_input rather than dereferencing the unowned
    // back-pointer, which tty_release can free between the check above and the
    // call (see -syncWindowSize). A reference rather than holding ttys_lock
    // across the call: this runs for every keystroke, and tty_input does line
    // discipline and signal delivery.
    tty = tty_lookup_ref(self.type, self.number, tty);
    if (tty != NULL)
        tty_input(tty, input.bytes, input.length, 0);
#else
    async_do_in_workqueue(^{
        NSData *inputRef = input;
        self.tty->ops->send_input(self.tty, inputRef.bytes, inputRef.length);
    });
#endif
    if (self.loaded) {
        [self.webView evaluateJavaScript:@"exports.setUserGesture()" completionHandler:nil];
        [self.scrollToBottomTask schedule];
    }
#if !ISH_LINUX
    // Last, and after every use of self: if this drops the final reference,
    // tty_release runs ios_tty_cleanup, whose CFBridgingRelease releases the
    // tty's own strong reference to this Terminal.
    tty_put(tty);
#endif
}

- (void)requestRefresh {
    [self.refreshTask schedule];
    if (self.loaded)
        [self.scrollToBottomTask schedule];
}

- (void)scrollToBottom {
    if (!self.loaded)
        return;
    [self.webView evaluateJavaScript:@"exports.scrollToBottom()" completionHandler:nil];
}

- (NSString *)arrow:(char)direction {
    return [NSString stringWithFormat:@"\x1b%c%c", self.applicationCursor ? 'O' : '[', direction];
}

- (void)refresh {
    if (!self.loaded)
        return;

#if !ISH_LINUX
    lock(&_dataLock, 0);
    CFTimeInterval now = ISHTerminalNowMonotonic();
    if (_outputInProgress) {
        if (_outputStartedAt > 0 && now - _outputStartedAt > ISHTerminalOutputWatchdogSeconds) {
            // Watchdog: a previous evaluateJavaScript write has not reported
            // completion within the timeout. Crucially, do NOT re-queue the
            // in-flight data. evaluateJavaScript cannot be cancelled, so a write
            // that is merely slow (not failed) may still apply after we give up
            // waiting. Re-queuing it would then draw the same bytes a second time,
            // and a partial-redraw TUI like htop never rewrites cells it believes
            // are already correct -- so the duplicate produces doubled / garbled
            // regions (most visible in the fastest-updating area, e.g. the CPU
            // meters) that persist until a full repaint (^L). If the slow write did
            // apply, its bytes are already on screen; if the webview is genuinely
            // wedged, re-queuing to it would not have helped. Either way, just drop
            // the in-flight tracking and let the next batch proceed.
            NSUInteger droppedBytes = self.inFlightData.length;
            self.inFlightData = nil;
            _outputInProgress = NO;
            _outputStartedAt = 0;
            self.outputGeneration++;
            [self recordLifecycleEvent:@"terminal.output.watchdog"
                               details:@{@"pendingBytes": @(_pendingData.length),
                                         @"inFlightBytes": @(droppedBytes)}];
        } else {
            [self.refreshTask schedule];
            unlock(&_dataLock);
            return;
        }
    }
    NSData *data = _pendingData;
    _pendingData = [[NSMutableData alloc] initWithCapacity:BUF_SIZE];
    _outputInProgress = YES;
    self.inFlightData = data;
    _outputStartedAt = now;
    NSUInteger generation = ++self.outputGeneration;
    notify(&self->_dataConsumed);
    unlock(&_dataLock);
#else
    NSData *data;
    NSUInteger generation;
    @synchronized (self) {
        if (_outputInProgress) {
            CFTimeInterval now = ISHTerminalNowMonotonic();
            if (_outputStartedAt > 0 && now - _outputStartedAt > ISHTerminalOutputWatchdogSeconds) {
                // See the !ISH_LINUX branch above: do NOT re-queue the in-flight
                // data. evaluateJavaScript can't be cancelled, so a merely-slow
                // write may still apply; re-sending it would draw the bytes twice
                // and corrupt partial-redraw TUIs (doubled/garbled regions that
                // persist until ^L).
                NSUInteger droppedBytes = self.inFlightData.length;
                self.inFlightData = nil;
                _outputInProgress = NO;
                _outputStartedAt = 0;
                self.outputGeneration++;
                [self recordLifecycleEvent:@"terminal.output.watchdog"
                                   details:@{@"pendingBytes": @(_pendingData.length),
                                             @"inFlightBytes": @(droppedBytes)}];
            } else {
                [self.refreshTask schedule];
                return;
            }
        }
        data = _pendingData;
        _pendingData = [[NSMutableData alloc] initWithCapacity:BUF_SIZE];
        _outputInProgress = YES;
        self.inFlightData = data;
        _outputStartedAt = ISHTerminalNowMonotonic();
        generation = ++self.outputGeneration;
        if (self->_tty)
            async_do_in_irq(^{
                self->_tty->ops->can_output(self->_tty);
            });
    }
#endif

    if (data.length == 0) {
#if !ISH_LINUX
        lock(&_dataLock, 0);
        if (self.outputGeneration == generation) {
            _outputInProgress = NO;
            self.inFlightData = nil;
            _outputStartedAt = 0;
        }
        unlock(&_dataLock);
#else
        @synchronized (self) {
            if (self.outputGeneration == generation) {
                _outputInProgress = NO;
                self.inFlightData = nil;
                _outputStartedAt = 0;
            }
        }
#endif
        return;
    }

    NSString *dataString = ISHJavaScriptLiteralForTerminalData(data);
    NSString *jsToEvaluate = [NSString stringWithFormat:@"exports.write(\"%@\")", dataString];
    [self.webView evaluateJavaScript:jsToEvaluate completionHandler:^(id result, NSError *error) {
#if !ISH_LINUX
        lock(&self->_dataLock, 0);
        if (self.outputGeneration == generation) {
            self->_outputInProgress = NO;
            self.inFlightData = nil;
            self->_outputStartedAt = 0;
        }
        unlock(&self->_dataLock);
#else
        bool hasPendingData;
        @synchronized (self) {
            if (self.outputGeneration == generation) {
                self->_outputInProgress = NO;
                self.inFlightData = nil;
                self->_outputStartedAt = 0;
            }
            hasPendingData = self->_pendingData.length > 0;
        }
        if (self->_tty != NULL) {
            async_do_in_irq(^{
                if (self->_tty != NULL)
                    self->_tty->ops->can_output(self->_tty);
            });
        }
        if (hasPendingData)
            [self.refreshTask schedule];
#endif
        if (error != nil) {
            NSLog(@"error sending bytes to the terminal: %@", error);
            [self recordLifecycleEvent:@"terminal.output.writeFailed"
                               details:@{@"bytes": @(data.length),
                                         @"error": error.localizedDescription ?: @"unknown"}];
            [self recoverTerminalWebViewWithReason:@"writeFailed" error:error];
            return;
        }
#if !ISH_LINUX
        lock(&self->_dataLock, 0);
        bool hasPendingData = self->_pendingData.length > 0;
        unlock(&self->_dataLock);
        if (hasPendingData)
            [self.refreshTask schedule];
#endif
    }];
}

+ (void)convertCommand:(NSArray<NSString *> *)command toArgs:(char *)argv limitSize:(size_t)maxSize {
    char *p = argv;
    for (NSString *cmd in command) {
        const char *c = cmd.UTF8String;
        // Save space for the final NUL byte in argv
        while (p < argv + maxSize - 1 && (*p++ = *c++));
        // If we reach the end of the buffer, the last string still needs to be
        // NUL terminated
        *p = '\0';
    }
    // Add the final NUL byte to argv
    *++p = '\0';
}

+ (Terminal *)terminalWithType:(int)type number:(int)number {
    return [[Terminal alloc] initWithType:type number:number];
}

+ (NSArray<Terminal *> *)activeTerminals {
    @synchronized (Terminal.class) {
        NSMutableArray<Terminal *> *active = [NSMutableArray array];
        NSEnumerator<Terminal *> *enumerator = terminals.objectEnumerator;
        for (Terminal *terminal in enumerator) {
            if (terminal != nil)
                [active addObject:terminal];
        }
        [active sortUsingComparator:^NSComparisonResult(Terminal *lhs, Terminal *rhs) {
            NSInteger lhsRank = lhs.type == TTY_CONSOLE_MAJOR ? 0 : (lhs.type == TTY_PSEUDO_SLAVE_MAJOR ? 1 : 2);
            NSInteger rhsRank = rhs.type == TTY_CONSOLE_MAJOR ? 0 : (rhs.type == TTY_PSEUDO_SLAVE_MAJOR ? 1 : 2);
            if (lhsRank < rhsRank)
                return NSOrderedAscending;
            if (lhsRank > rhsRank)
                return NSOrderedDescending;
            if (lhs.type < rhs.type)
                return NSOrderedAscending;
            if (lhs.type > rhs.type)
                return NSOrderedDescending;
            if (lhs.number < rhs.number)
                return NSOrderedAscending;
            if (lhs.number > rhs.number)
                return NSOrderedDescending;
            return NSOrderedSame;
        }];
        return active;
    }
}

+ (Terminal *)terminalWithUUID:(NSUUID *)uuid {
    @synchronized (Terminal.class) {
        return [terminalsByUUID objectForKey:uuid];
    }
}

- (void)setPendingDestroyReason:(NSString *)reason {
    _pendingDestroyReason = [reason copy];
}

- (int)type {
    return dev_major((dev_t_) self.terminalsKey.unsignedIntValue);
}

- (int)number {
    return dev_minor((dev_t_) self.terminalsKey.unsignedIntValue);
}

- (void)destroy {
    tty_t tty;
    @synchronized (self) {
        tty = self->_tty;
    }
#if !ISH_LINUX
    // Own a reference for as long as this method touches the tty. Everything
    // below dereferences it, and tty_release can free it concurrently -- see
    // -syncWindowSize for why the unowned back-pointer alone isn't enough.
    tty = tty_lookup_ref(self.type, self.number, tty);
#endif
    NSString *reason = self.pendingDestroyReason ?: @"unspecified";
    self.pendingDestroyReason = nil;
    NSMutableDictionary<NSString *, id> *details = [NSMutableDictionary dictionaryWithDictionary:@{
        @"reason": reason,
        @"ttyAttached": ISHStringFromBOOL(tty != NULL),
    }];
#if !ISH_LINUX
    if (tty != NULL) {
        details[@"ttyHungUp"] = ISHStringFromBOOL(tty->hung_up);
        details[@"ttyEverOpened"] = ISHStringFromBOOL(tty->ever_opened);
        details[@"ttySession"] = @(tty->session);
        details[@"ttyFgGroup"] = @(tty->fg_group);
    }
#endif
    [self recordLifecycleEvent:@"terminal.destroy" details:details];
    if (tty != NULL) {
#if !ISH_LINUX
        if (tty != NULL) {
            lock(&tty->lock, 0);
            [self recordLifecycleEvent:@"terminal.destroy.hangup"
                               details:@{@"reason": reason,
                                         @"ttyHungUpBefore": ISHStringFromBOOL(tty->hung_up)}];
            tty_hangup(tty);
            unlock(&tty->lock);
        }
#else
        tty->ops->hangup(tty);
#endif
    }
    @synchronized (Terminal.class) {
        [terminals removeObjectForKey:self.terminalsKey];
        if (self.uuid != nil)
            [terminalsByUUID removeObjectForKey:self.uuid];
    }
    NotifyTerminalRegistryChanged();
#if !ISH_LINUX
    // Drop the reference taken above, last and after every use of self: if this
    // drops the final reference, tty_release runs ios_tty_cleanup, whose
    // CFBridgingRelease releases the tty's own strong reference to this
    // Terminal -- which for a terminal being destroyed may well be the last one.
    tty_put(tty);
#endif
}

+ (void)initialize {
    if (self == Terminal.class) {
        terminals = [NSMapTable strongToWeakObjectsMapTable];
        terminalsByUUID = [NSMapTable strongToWeakObjectsMapTable];
    }
}

@end

#if ISH_LINUX
nsobj_t Terminal_terminalWithType_number(int type, int number) {
    return CFBridgingRetain([Terminal terminalWithType:type number:number]);
}
int Terminal_sendOutput_length(nsobj_t _self, const char *data, int size) {
    return [(__bridge Terminal *) _self sendOutput:data length:size];
}
int Terminal_roomForOutput(nsobj_t _self) {
    return [(__bridge Terminal *) _self roomForOutput];
}
void Terminal_setLinuxTTY(nsobj_t _self, struct linux_tty *tty) {
    return [(__bridge Terminal *) _self setTty:tty];
}
#endif

#if !ISH_LINUX
static int ios_tty_init(struct tty *tty) {
    // This is called with ttys_lock but that results in deadlock since the main thread can also acquire ttys_lock. So release it.
    unlock(&ttys_lock);
    void (^init_block)(void) = ^{
        Terminal *terminal = [Terminal terminalWithType:tty->type number:tty->num];
        tty->data = (void *) CFBridgingRetain(terminal);
        terminal.tty = tty;
    };
    if ([NSThread isMainThread])
        init_block();
    else
        dispatch_sync(dispatch_get_main_queue(), init_block);

    lock(&ttys_lock, 0);
    return 0;
}

static int ios_tty_write(struct tty *tty, const void *buf, size_t len, bool blocking) {
    Terminal *terminal = (__bridge Terminal *) tty->data;
    return [terminal sendOutput:buf length:(int) len];
}

static void ios_tty_cleanup(struct tty *tty) {
    Terminal *terminal = CFBridgingRelease(tty->data);
    tty->data = NULL;
    terminal.tty = NULL;
}

struct tty_driver_ops ios_tty_ops = {
    .init = ios_tty_init,
    .write = ios_tty_write,
    .cleanup = ios_tty_cleanup,
};
DEFINE_TTY_DRIVER(ios_console_driver, &ios_tty_ops, TTY_CONSOLE_MAJOR, 64);
struct tty_driver ios_pty_driver = {.ops = &ios_tty_ops};
#endif
