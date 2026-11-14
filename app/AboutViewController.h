//
//  AboutViewController.h
//  iSH
//
//  Created by Theodore Dubois on 9/23/18.
//

#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

extern NSString *const kPreferenceOpenDiagnosticsOnLaunchKey;
extern UINavigationController *ISHCreateAboutNavigationController(BOOL recoveryMode, BOOL startInDiagnostics);
extern UIViewController *ISHCreateDiagnosticsViewController(void);
// Diagnostics for a workspace tool window: same screen, wrapped so its Share
// and Refresh buttons have a bar to live in. See the definition.
extern UIViewController *ISHCreateDiagnosticsNavigationController(void);
extern UIViewController *ISHCreateLLMClientViewController(void);
extern UIViewController *ISHCreateLLMClientViewControllerWithInitialPrompt(NSString *_Nullable initialPrompt);
extern UIViewController *ISHCreateLLMSettingsViewController(void);
// The page that installs a desktop for the Wayland display and chooses which
// one it starts (GH #639). Push it, or present it in a navigation controller.
extern UIViewController *ISHCreateDesktopSetupViewController(void);
// The Legal screen: licence notice and the GPL written offer. The offer has
// to accompany the binaries it covers, so it ships in the app rather than
// living on a website. See docs/gpl-source-offer.md.
extern UIViewController *ISHCreateLegalViewController(void);
extern BOOL ISHLLMClientEnabled(void);

typedef NS_ENUM(NSInteger, AOKLLMBackend) {
    AOKLLMBackendAppleFoundationModels,
    AOKLLMBackendBundledQwenTiny,
    AOKLLMBackendOpenAICompatibleEndpoint,
};

@interface AboutViewController : UITableViewController

@property BOOL recoveryMode;
@property BOOL startInDiagnostics;

@end

NS_ASSUME_NONNULL_END
