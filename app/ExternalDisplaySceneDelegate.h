//
//  ExternalDisplaySceneDelegate.h
//  iSH
//

#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

// Delegate for the UIWindowSceneSessionRoleExternalDisplayNonInteractive scene
// iOS creates when a display is attached over USB-C/HDMI (or AirPlay without
// mirroring). It hosts nothing of its own: it borrows the active terminal's
// rendering, and hands it back when the display goes away. The scene is
// non-interactive by definition -- it can never be key, so nothing in it can be
// first responder -- which is exactly why only the rendering moves over.
API_AVAILABLE(ios(13))
@interface ExternalDisplaySceneDelegate : UIResponder <UIWindowSceneDelegate>

@property (nonatomic, nullable) UIWindow *window;

@end

// Asks iOS 27 for the external display on viewController's behalf. An app built
// with the iOS 27 SDK no longer gets this scene from the Info.plist role: iOS 27
// presents it as a scene accessory that a view controller registers, and with
// none registered the display stays a mirror (black in the simulator). Earlier
// iOS still reads the plist role, so the two coexist.
//
// A registration counts only while its controller is in the window: a full-screen
// presentation over it takes the display away until it is dismissed, and a page
// sheet does not. So it belongs on a surface that owns a scene's content -- a
// terminal or Wayland display that is a scene's own, or a Workspace -- once that
// is on screen. Idempotent, and iPhone only, like the plist role.
void ISHRegisterExternalDisplayAccessory(UIViewController *viewController);

NS_ASSUME_NONNULL_END
