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

NS_ASSUME_NONNULL_END
