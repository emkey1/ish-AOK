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

@property (strong, nonatomic, nullable) UIWindow *window;
- (void)exitApp;

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
// That account's uid and primary gid (/etc/passwd fields 2 and 3), for handing files the
// GUI creates to the account the user's sessions run as. NO under the same conditions
// headlessCommandAccountName answers nil: preference off, or no such account.
+ (BOOL)headlessCommandAccountOwner:(NSInteger * _Nullable)uid gid:(NSInteger * _Nullable)gid;

+ (void)maybePresentStartupMessageOnViewController:(UIViewController * _Nonnull)vc;

- (void)refreshDnsConfiguration;

@end

extern NSString * _Nonnull const ProcessExitedNotification;

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

// ---- suspend to disk (kernel/checkpoint.c) --------------------------------
//
// Where a suspended session lives, and taking one on demand. The automatic
// half -- save on backgrounding, resume on launch -- needs no caller; these
// are for a user asking for it NOW, from the Workspace.
//
// ISHSuspendSessionSaveNow BLOCKS: it freezes every guest task, writes the
// image and thaws before returning. Call it off the main thread. It returns 0,
// or a guest _E* code with /proc/ish/checkpoint's last_refusal naming the
// cause. The guest is unharmed either way -- a checkpoint is a copy.
NSString *ISHSuspendSessionImagePath(void);
int ISHSuspendSessionSaveNow(void);

// Save the session and terminate the app, so the next launch resumes it. Does
// not return on success. A SAVE is a copy and the guest carries on; this is the
// departure.
int ISHSuspendSessionSuspendAndExit(void);

// ---- saved sessions -----------------------------------------------------
// Each entry: path, name, date, bytes, tasks, hostname, loadable. Newest first.
NSArray<NSDictionary *> *ISHSessionSlots(void);
// How many are worth keeping on this device right now, space considered.
NSUInteger ISHSessionSlotLimit(void);
// Which slot the next save writes to; nil resets to "pick one".
void ISHSessionSetCurrentSlot(NSString *_Nullable path);
// The slot this launch should resume, chosen before the guest boots. nil means
// start fresh, and the boot will not look for an image at all.
void ISHSessionSetResumeChoice(NSString *_Nullable path);
// YES while nothing has been chosen and there is something to choose between.
BOOL ISHSessionResumeChoicePending(void);
