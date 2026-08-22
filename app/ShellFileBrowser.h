//
//  ShellFileBrowser.h
//  iSH-AOK
//
//  A file browser for terminal mode: a sheet over the terminal, opened with a
//  key on the keyboard bar (or Cmd-B), so browsing the guest filesystem no
//  longer means leaving for Workspace mode and driving the full Finder-style
//  applet by touch.
//
//  Deliberately NOT the Workspace file manager in a smaller frame. This one is
//  built around the terminal underneath it: it opens on the shell's own working
//  directory, and its primary verbs put text on the command line -- insert a
//  path at the cursor, cd into a directory -- rather than opening documents in
//  native viewers. File management (new folder, rename, delete) is here too,
//  but it is the secondary half; Workspace mode remains the full applet.
//
//  Built on ISHGuestFileBridge like every other native applet, so it inherits
//  the same async rule: no guest-VFS call ever happens on the main thread.
//

#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

@class ISHShellFileBrowserViewController;

@protocol ISHShellFileBrowserDelegate <NSObject>

// Put text on the terminal's command line exactly as if it had been typed.
// The browser never appends a newline itself: a caller that wants the line
// executed (the cd verb) passes one, and the delegate stays a dumb pipe.
- (void)shellFileBrowser:(ISHShellFileBrowserViewController *)browser
              insertText:(NSString *)text;

@optional

// The sheet has gone away. The presenter uses this to put keyboard focus back
// on the terminal, which it gave up while the sheet was open.
- (void)shellFileBrowserDidDismiss:(ISHShellFileBrowserViewController *)browser;

@end

@interface ISHShellFileBrowserViewController : UIViewController

// Presents the browser as a sheet over `presenter`, starting in the working
// directory of the foreground process group of tty (ttyType, ttyNumber) -- so
// it opens where the shell is standing. Falls back to `fallbackPath` (then to
// "/") when the guest is not booted or the tty has no foreground group yet.
+ (void)presentFromViewController:(UIViewController *)presenter
                          ttyType:(int)ttyType
                        ttyNumber:(int)ttyNumber
                     fallbackPath:(nullable NSString *)fallbackPath
                         delegate:(nullable id<ISHShellFileBrowserDelegate>)delegate;

@property (nonatomic, weak) id<ISHShellFileBrowserDelegate> delegate;

@end

// Quotes a guest path for a POSIX shell command line. Returns the path
// unchanged when every character is safe, and single-quoted (with embedded
// single quotes escaped the '\'' way) otherwise. Exposed for the unit test.
extern NSString *ISHShellQuoteArgument(NSString *argument);

NS_ASSUME_NONNULL_END
