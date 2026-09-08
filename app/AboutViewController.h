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
