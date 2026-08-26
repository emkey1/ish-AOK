//
//  AppDelegate.h
//  iSH
//
//  Created by Theodore Dubois on 10/17/17.
//

#import <UIKit/UIKit.h>

FOUNDATION_EXPORT void ISHScheduleLaunchJournalCompletion(NSDictionary<NSString *, id> * _Nullable details);

struct task;

@interface AppDelegate : UIResponder <UIApplicationDelegate>

@property (strong, nonatomic) UIWindow *window;
- (void)exitApp;

#if !ISH_LINUX
+ (intptr_t)bootError;
+ (NSString * _Nonnull)descriptionForISHErrno:(intptr_t)err;
+ (NSString * _Nullable)bootFailureTitle;
+ (NSString * _Nullable)bootFailureMessage;
+ (NSString * _Nullable)bootFailureOverlayText;
+ (BOOL)bootUsesConsoleSessionFallback;
+ (intptr_t)ensureBooted;
+ (BOOL)pushUsableInitTaskAsCurrent:(struct task * _Nullable * _Nonnull)previousCurrent;
+ (void)popCurrentTask:(struct task * _Nullable)previousCurrent;
// The account name whose /etc/passwd UID matches ISHDefaultUserAccountUID (see
// UserPreferences.h), or nil if this rootfs has no such account. "Open Everything as Default
// User" targets whatever's actually there instead of a name iSH provisions itself.
+ (NSString * _Nullable)defaultUserAccountName;
// The account the headless command surfaces (LLM chat's run_shell tool, the Shortcuts Run
// Command intent) should run commands as: the default-user account when "Open Everything as
// Default User" is on and this rootfs actually has one, else nil for root. Callers hand the
// name to run_guest_command_capture_user; a nil keeps the plain root capture path.
+ (NSString * _Nullable)headlessCommandAccountName;
#endif

+ (void)maybePresentStartupMessageOnViewController:(UIViewController *)vc;

#if !ISH_LINUX
- (void)refreshDnsConfiguration;
#endif

@end

#if !ISH_LINUX
extern NSString *const ProcessExitedNotification;
#else
extern NSString *const KernelPanicNotification;
#endif

// Suspension handling for the fakefs; implemented in AppDelegate.m.
//
// These must be driven from the SCENE delegate. This app adopts UIScene, and
// UIKit does not call applicationDidEnterBackground: / willEnterForeground: on
// the application delegate in a scene-based app, so hanging the work off those
// leaves it silently never running.
//
// EnterBackground takes a background-task assertion whose expiration handler is
// the closest thing iOS gives to an "about to be suspended" signal, and that is
// where the fakefs is quiesced so no SQLite lock is held across suspension
// (RUNNINGBOARD 0xdead10cc). EnterForeground lifts the gate. Both are safe to
// call repeatedly.
void ISHSuspendGuardEnterBackground(void);
void ISHSuspendGuardEnterForeground(void);
