//
//  GuestCommandRunner.h
//  iSH-AOK
//
//  Headless one-shot guest command execution for App Intents (Shortcuts).
//

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

// Outcome of one guest command run. Mirrors struct guest_command_result
// (kernel/init.h) with boot/launch failures folded in, so the Swift intent
// deals with exactly one object.
@interface ISHGuestCommandOutcome : NSObject
@property (nonatomic, readonly) BOOL launched;    // the command started in the guest
@property (nonatomic, readonly) BOOL exited;      // exitCode is valid
@property (nonatomic, readonly) int exitCode;
@property (nonatomic, readonly) int termSignal;   // terminating signal when exited == NO
@property (nonatomic, readonly) BOOL timedOut;
@property (nonatomic, readonly) BOOL truncated;   // output hit the size cap
@property (nonatomic, readonly, copy) NSString *output; // merged stdout+stderr
@property (nonatomic, readonly, copy) NSString *shell;  // shell that ran (or was last attempted)
// Human-readable reason when launched == NO (boot failure, exec failure, ...).
@property (nonatomic, readonly, copy, nullable) NSString *failureReason;
@end

// Runs `<shell> -c command` headlessly in the booted guest, /AOK/native/zsh
// first with a /bin/sh fallback if the native shell cannot start. Safe to call
// from any thread and while the app is background-launched for an intent: the
// runner boots the guest if needed, holds a background-task assertion for the
// duration, lifts the fakefs quiesce gate before running, and re-arms the
// suspend guard afterwards when the app is still backgrounded. The completion
// block is invoked exactly once, on an arbitrary background queue.
@interface ISHGuestCommandRunner : NSObject
+ (void)runCommand:(NSString *)command
    timeoutSeconds:(NSInteger)timeoutSeconds
        completion:(void (^)(ISHGuestCommandOutcome *outcome))completion;
@end

NS_ASSUME_NONNULL_END
