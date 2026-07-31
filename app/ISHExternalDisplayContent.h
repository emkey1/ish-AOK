//
//  ISHExternalDisplayContent.h
//  iSH
//

#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

// Posted (object: the adopter) when a surface becomes able to drive an external
// display -- a terminal attaching its session, a Wayland display finishing its
// RFB connect. An external scene can connect long before either happens, so it
// listens for this to know when there is finally something to show.
extern NSString *const ISHExternalDisplayContentDidBecomeAvailableNotification;

// A surface that can put its rendering on an external display while input
// stays on the device. ExternalDisplaySceneDelegate finds and drives whatever
// is frontmost through this protocol without caring which surface it got.
//
// The two adopters get there differently, because what carries their input
// differs. A terminal *relocates*: keystrokes never touched its webView, so
// the webView can leave outright and the TerminalView keeps typing. The
// Wayland display *mirrors*: its Metal view is also the touch surface that
// generates RFB pointer events, so it has to stay put, and a second view
// renders the same framebuffer over there.
@protocol ISHExternalDisplayContent <NSObject>

// NO when there is nothing to show yet -- no session, no connection -- which
// tells the caller to leave the display idle and try again later rather than
// treat it as taken.
- (BOOL)relocateContentToExternalView:(UIView *)hostView;

// Takes the content back, but only if it is still on hostView. iOS can stand
// up a replacement external scene before tearing the old one down, and the
// departing scene must not undo what the new one just did.
- (void)restoreContentFromExternalView:(UIView *)hostView;

@property (nonatomic, readonly) BOOL rendersOnExternalDisplay;

@end

NS_ASSUME_NONNULL_END
