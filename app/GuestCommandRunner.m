//
//  GuestCommandRunner.m
//  iSH-AOK
//

#import <UIKit/UIKit.h>
#import <os/lock.h>
#import "GuestCommandRunner.h"
#import "AppDelegate.h"
#include "kernel/init.h"
#include "fs/fake-db.h"

// Shortcuts commands run under the native zsh: it starts in host time rather
// than emulated time, which matters for an intent the user is waiting on.
static const char *const kISHShortcutShellPath = "/AOK/native/zsh";
// Ceiling matches the LLM tool's kISHLLMToolOutputMaxKB; Shortcuts hands the
// result to actions like Show Result that cope fine with text this size.
static const NSInteger kISHShortcutOutputLimitKB = 256;

@interface ISHGuestCommandOutcome ()
@property (nonatomic, readwrite) BOOL launched;
@property (nonatomic, readwrite) BOOL exited;
@property (nonatomic, readwrite) int exitCode;
@property (nonatomic, readwrite) int termSignal;
@property (nonatomic, readwrite) BOOL timedOut;
@property (nonatomic, readwrite) BOOL truncated;
@property (nonatomic, readwrite, copy) NSString *output;
@property (nonatomic, readwrite, copy) NSString *shell;
@property (nonatomic, readwrite, copy, nullable) NSString *failureReason;
@end

@implementation ISHGuestCommandOutcome
- (instancetype)init {
    if (self = [super init]) {
        _output = @"";
        _shell = @"";
    }
    return self;
}
@end

// Dedicated serial queue, same rationale as ISHLLMGuestCommandQueue in
// AboutViewController.m: run_guest_command_capture repoints the kernel's
// `current` and must stay off the shared libdispatch global pool, or a pooled
// worker can be left in a state that wedges later network requests.
static dispatch_queue_t ISHShortcutCommandQueue(void) {
    static dispatch_queue_t queue;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        queue = dispatch_queue_create("aok.shortcuts.guest-shell", DISPATCH_QUEUE_SERIAL);
    });
    return queue;
}

@implementation ISHGuestCommandRunner

+ (void)runCommand:(NSString *)command
    timeoutSeconds:(NSInteger)timeoutSeconds
        completion:(void (^)(ISHGuestCommandOutcome *outcome))completion {
    // An intent can background-launch the app; the assertion keeps iOS from
    // suspending us mid-command. Begin/end are thread-safe.
    UIApplication *app = UIApplication.sharedApplication;
    __block UIBackgroundTaskIdentifier assertion = UIBackgroundTaskInvalid;
    __block os_unfair_lock assertionLock = OS_UNFAIR_LOCK_INIT;
    void (^endAssertion)(void) = ^{
        os_unfair_lock_lock(&assertionLock);
        UIBackgroundTaskIdentifier claimed = assertion;
        assertion = UIBackgroundTaskInvalid;
        os_unfair_lock_unlock(&assertionLock);
        if (claimed != UIBackgroundTaskInvalid)
            [app endBackgroundTask:claimed];
    };
    assertion = [app beginBackgroundTaskWithName:@"shortcuts-run-command" expirationHandler:endAssertion];

    dispatch_async(ISHShortcutCommandQueue(), ^{
        ISHGuestCommandOutcome *outcome = [self runCommandOnQueue:command timeoutSeconds:timeoutSeconds];
        // Re-arm the suspend guard BEFORE dropping our assertion: if the app is
        // still backgrounded, whatever comes after us must not be a suspension
        // that lands while a fakefs transaction holds a SQLite lock
        // (0xdead10cc). The guard's expiration handler re-quiesces on cue; if
        // the original scene-driven guard is still armed this is a no-op.
        dispatch_async(dispatch_get_main_queue(), ^{
            if (UIApplication.sharedApplication.applicationState == UIApplicationStateBackground)
                ISHSuspendGuardEnterBackground();
            endAssertion();
        });
        completion(outcome);
    });
}

// Runs on ISHShortcutCommandQueue only.
+ (ISHGuestCommandOutcome *)runCommandOnQueue:(NSString *)command timeoutSeconds:(NSInteger)timeoutSeconds {
    ISHGuestCommandOutcome *outcome = [ISHGuestCommandOutcome new];
    outcome.shell = @(kISHShortcutShellPath);

    intptr_t bootErr = [AppDelegate ensureBooted];
    if (bootErr < 0) {
        outcome.failureReason = [NSString stringWithFormat:@"The guest system could not boot (error %ld). Open iSH-AOK once to finish setup.", (long) bootErr];
        return outcome;
    }

    // A background-launched intent can arrive with the suspension quiesce gate
    // engaged (the suspend guard freezes fakefs before iOS suspends the app).
    // New transactions would park on it until the 5 s max-hold timer fires;
    // lift it up front instead, exactly as ISHSuspendGuardEnterForeground does.
    // Idempotent when the gate is not engaged.
    fakefs_quiesce_end();

    struct guest_command_result result;
    int rc = run_guest_command_capture_shell(kISHShortcutShellPath, command.UTF8String, NULL,
                                             (int) (timeoutSeconds * 1000),
                                             (size_t) kISHShortcutOutputLimitKB * 1024, &result);
    if (rc < 0) {
        // The native zsh is expected on every root (/AOK is app-bundled), but
        // fall back rather than fail if it cannot exec.
        outcome.shell = @"/bin/sh";
        rc = run_guest_command_capture_shell(NULL, command.UTF8String, NULL,
                                             (int) (timeoutSeconds * 1000),
                                             (size_t) kISHShortcutOutputLimitKB * 1024, &result);
    }
    if (rc < 0) {
        outcome.failureReason = [NSString stringWithFormat:@"Could not start the command (error %d). Is the guest system booted?", rc];
        return outcome;
    }

    outcome.launched = result.launched != 0;
    outcome.exited = result.exited != 0;
    outcome.exitCode = result.exit_code;
    outcome.termSignal = result.term_signal;
    outcome.timedOut = result.timed_out != 0;
    outcome.truncated = result.truncated != 0;
    if (result.output != NULL && result.output_len > 0) {
        outcome.output = [[NSString alloc] initWithBytes:result.output
                                                  length:result.output_len
                                                encoding:NSUTF8StringEncoding] ?: @"";
    }
    free(result.output);
    if (!outcome.launched && outcome.failureReason == nil)
        outcome.failureReason = @"The command could not be started.";
    return outcome;
}

@end
