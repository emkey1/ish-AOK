//
//  Terminal.h
//  iSH
//
//  Created by Theodore Dubois on 10/18/17.
//

#import <UIKit/UIKit.h>
#import <WebKit/WebKit.h>

struct tty;

@interface Terminal : NSObject

+ (Terminal *)terminalWithType:(int)type number:(int)number;
+ (NSArray<Terminal *> *)activeTerminals;
// Returns a strong struct tty and a Terminal that has a weak reference to the same tty
+ (Terminal *)createPseudoTerminal:(struct tty **)tty;

+ (Terminal *)terminalWithUUID:(NSUUID *)uuid;

// The factory kernel/checkpoint.c calls to give a session that came back from
// suspend-to-disk a terminal of its own. The kernel cannot make one: a
// pseudo-terminal's master side here is a Terminal, not a guest process.
// Installed into checkpoint_open_session_tty before the restore runs.
struct tty *ISHOpenTerminalForRestoredSession(void);

@property (readonly) NSUUID *uuid;
@property (readonly) int type;
@property (readonly) int number;
// The guest SESSION this terminal is carrying, or 0 when it has none.
//
// This is what a checkpoint keys a restored session on -- the restore reports
// a session's leader pid, and a session leader's pid IS its session id. Read
// from the tty rather than remembered by the window, because a window does not
// always start the session it shows: one that ADOPTS an existing terminal
// (reconnectSessionFromTerminalUUID:) never learned a pid at all, so the saved
// layout recorded nothing and every window fell back to queue order -- which
// is two terminals coming back with each other's shells.
@property (readonly) int guestSessionId;
// What this terminal has printed -- screen plus scrollback -- as plain text.
//
// Asynchronous because it crosses into the web view. The completion runs on the
// MAIN queue, so a caller that waits for it must not itself be on main.
- (void)fetchContentsWithCompletion:(void (^)(NSString *contents))completion;
// Put previously captured text back, before a session starts writing to it.
- (void)writeRestoredContents:(NSString *)contents;

+ (void)convertCommand:(NSArray<NSString *> *)command toArgs:(char *)argv limitSize:(size_t)maxSize;

- (int)sendOutput:(const void *)buf length:(int)len;
- (void)sendInput:(NSData *)input;

// Bracketed paste mode (DECSET 2004) as hterm currently has it, pushed up from
// the web view every time the guest turns it on or off. It says how to deliver
// a PASTE and nothing about ordinary keystrokes: those are typing, and typing
// is never bracketed.
@property (nonatomic) BOOL bracketedPasteEnabled;
- (void)requestRefresh;
// Re-measure the terminal against its current bounds; see -resyncSize.
- (void)resyncSize;
// Tell the tty an estimated size while the page has not loaded; see
// -presetSizeWithColumns:rows:. Ignored once the page has measured itself.
- (void)presetSizeWithColumns:(int)cols rows:(int)rows;
- (void)setPendingDestroyReason:(NSString *)reason;

- (NSString *)arrow:(char)direction;

// Make this terminal no longer be the singleton terminal with its type and number. Will happen eventually if all references go away, but sometimes you want it to happen now.
- (void)destroy;

@property (readonly) WKWebView *webView;
@property (nonatomic) BOOL enableVoiceOverAnnounce;
// Use KVO on this
@property (readonly) BOOL loaded;

@end

// Wraps `text` the way a terminal is meant to deliver a paste, given whether the
// guest currently has bracketed paste mode on, and appends the carriage return
// that RUNS the result when `execute` is set.
//
// More than a concatenation, in three ways. A newline inside pasted text is a
// carriage return on the command line, which is how hterm's own paste handler
// writes it. When the mode is on the payload is filtered of the control
// characters that could end the bracket early -- an \e[201~ smuggled inside a
// snippet would otherwise hand everything after it to the shell as typed input,
// which is a command the user did not choose to run -- keeping the same
// whitespace (\b \t \n \r) that xterm and hterm keep. And the execute
// newline goes OUTSIDE the closing bracket: inside, the shell takes it as
// literal text in the pasted block and the line never runs at all, which is the
// whole point of bracketing.
//
// Exposed for the unit test.
extern NSString *ISHPasteSequence(NSString *text, BOOL bracketed, BOOL execute);

extern NSNotificationName const TerminalLoadFailedNotification;
extern NSNotificationName const TerminalDidLoadNotification;
extern NSNotificationName const TerminalRegistryDidChangeNotification;

extern struct tty_driver ios_console_driver;

NSString *Terminal_debugReadRows(int type, int number, int maxRows);
NSString *Terminal_debugSendInputUTF8(int type, int number, const char *input);
int Terminal_debugSendInputUTF8Sync(int type, int number, const char *input);
