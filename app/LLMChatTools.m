//
//  LLMChatTools.m
//  iSH-AOK
//
//  The tools the chat's model can call in the guest, and their limits.
//

#import "AboutViewController.h"
#import "AppDelegate.h"
#import "CurrentRoot.h"
#import "AppGroup.h"
#import "UserPreferences.h"
#import "UIViewController+Extras.h"
#import "WorkspaceViewController.h"
#import "MarkdownRenderer.h"
#import "LLMChatInternal.h"
#if __has_include("libiSH_AOKApp-Swift.h")
#import "libiSH_AOKApp-Swift.h" // AOKFoundationModelsBridge (Swift, iOS 26+ FoundationModels wrapper)
#endif
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include "kernel/init.h" // run_guest_command_capture (guest-shell tool)

// MARK: - Guest-shell tool support (OpenAI-compatible function calling)


// No tokenizer is available client-side, so approximate English text at ~4
// characters/token -- a standard rule of thumb. Real tokenizers vary
// +/-30% depending on the model and content, but this is only used to size
// compaction and a status display, not anything that needs to be exact.
NSInteger ISHLLMEstimateTokenCount(NSString *text) {
    return text.length > 0 ? (NSInteger) ((text.length + 3) / 4) : 0;
}

NSInteger ISHLLMEstimateMessagesTokenCount(NSArray<NSDictionary<NSString *, id> *> *messages) {
    NSInteger total = 0;
    for (NSDictionary<NSString *, id> *message in messages) {
        NSString *content = [message[@"content"] isKindOfClass:NSString.class] ? message[@"content"] : @"";
        total += ISHLLMEstimateTokenCount(content) + 4; // + rough per-message role/framing overhead
        NSArray *toolCalls = [message[@"tool_calls"] isKindOfClass:NSArray.class] ? message[@"tool_calls"] : nil;
        for (NSDictionary *toolCall in toolCalls) {
            NSDictionary *function = [toolCall[@"function"] isKindOfClass:NSDictionary.class] ? toolCall[@"function"] : nil;
            NSString *arguments = [function[@"arguments"] isKindOfClass:NSString.class] ? function[@"arguments"] : @"";
            total += ISHLLMEstimateTokenCount(arguments) + 8; // + rough per-call framing overhead
        }
    }
    return total;
}

NSString *ISHLLMFormattedTokenCount(NSInteger tokens) {
    if (tokens >= 1000)
        return [NSString stringWithFormat:@"%.1fK", tokens / 1000.0];
    return [NSString stringWithFormat:@"%ld", (long) tokens];
}

NSInteger ISHLLMToolTimeoutSeconds(void) {
    return MAX(kISHLLMToolTimeoutMinSeconds, MIN(kISHLLMToolTimeoutMaxSeconds, UserPreferences.shared.llmToolTimeoutSeconds));
}

NSInteger ISHLLMToolOutputLimitKB(void) {
    return MAX(kISHLLMToolOutputMinKB, MIN(kISHLLMToolOutputMaxKB, UserPreferences.shared.llmToolOutputLimitKB));
}

NSInteger ISHLLMToolMaxRounds(void) {
    return MAX(kISHLLMToolMaxRoundsMin, MIN(kISHLLMToolMaxRoundsMax, UserPreferences.shared.llmToolMaxRounds));
}

NSString *ISHLLMToolTimeoutTitle(NSInteger seconds) {
    if (seconds % 60 == 0 && seconds >= 60)
        return [NSString stringWithFormat:@"%ld min", (long) (seconds / 60)];
    return [NSString stringWithFormat:@"%lds", (long) seconds];
}

// The single tool exposed to the model: run a command in the iSH Linux shell and
// return its combined stdout+stderr. This makes "web search" just `curl`/`wget`
// in the environment iSH already is, with no extra API key.
NSArray<NSDictionary<NSString *, id> *> *ISHLLMChatToolDefinitions(void) {
    // Rebuilt per request, so the description tracks the current "Open
    // Everything as Default User" state: commands run as that account (via su
    // and its login shell) when the setting is on, as root via /bin/sh -c
    // otherwise. Only the preference is consulted here -- this runs on the
    // main thread while composing the request, and resolving the actual
    // account name means a guest-VFS read of /etc/passwd (the per-session
    // environment note, built on the guest command queue, carries the exact
    // name).
    NSString *identityNote = UserPreferences.shared.shouldLoginAsDefaultUser
        ? @"Commands run as the unprivileged default user account (not root) when this filesystem has one. "
        : @"";
    return @[@{
        @"type": @"function",
        @"function": @{
            @"name": @"run_shell",
            @"description": [NSString stringWithFormat:@"Run a command in the local iSH Linux shell and return its combined stdout and stderr. %@Use this to fetch web pages or APIs, read files, or run any Linux command available in this environment. The userland varies by distro -- it may be a minimal BusyBox/Alpine system or a full Debian/Devuan/glibc one -- so use the tools that are actually present (a per-session environment note lists what was detected) and try an alternative if a command reports 'not found'. Output is capped at %ld KB and the command is killed after %ld seconds.", identityNote, (long) ISHLLMToolOutputLimitKB(), (long) ISHLLMToolTimeoutSeconds()],
            @"parameters": @{
                @"type": @"object",
                @"properties": @{
                    @"command": @{
                        @"type": @"string",
                        @"description": @"The shell command line to execute, e.g. curl -fsSL 'https://wttr.in/Paris?format=3' (or wget -qO- on BusyBox systems)",
                    },
                },
                @"required": @[@"command"],
            },
        },
    }];
}

NSString *ISHLLMToolCallID(NSDictionary *toolCall) {
    return [toolCall[@"id"] isKindOfClass:NSString.class] ? toolCall[@"id"] : nil;
}

NSString *ISHLLMToolCallName(NSDictionary *toolCall) {
    NSDictionary *function = [toolCall[@"function"] isKindOfClass:NSDictionary.class] ? toolCall[@"function"] : nil;
    return [function[@"name"] isKindOfClass:NSString.class] ? function[@"name"] : nil;
}

// The OpenAI tool-call "arguments" field is a JSON *string*; pull the command out.
NSString *ISHLLMToolCallCommand(NSDictionary *toolCall) {
    NSDictionary *function = [toolCall[@"function"] isKindOfClass:NSDictionary.class] ? toolCall[@"function"] : nil;
    NSString *arguments = [function[@"arguments"] isKindOfClass:NSString.class] ? function[@"arguments"] : nil;
    if (arguments.length == 0)
        return nil;
    NSData *data = [arguments dataUsingEncoding:NSUTF8StringEncoding];
    id json = data != nil ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
    if (![json isKindOfClass:NSDictionary.class])
        return nil;
    NSString *command = [json[@"command"] isKindOfClass:NSString.class] ? json[@"command"] : nil;
    if (command.length == 0)
        command = [json[@"cmd"] isKindOfClass:NSString.class] ? json[@"cmd"] : nil;
    return command.length > 0 ? command : nil;
}

// Pick out the well-formed tool calls (those with an id) from a response message.
NSArray<NSDictionary *> *ISHLLMValidToolCalls(NSDictionary *message) {
    NSArray *toolCalls = [message[@"tool_calls"] isKindOfClass:NSArray.class] ? message[@"tool_calls"] : nil;
    NSMutableArray<NSDictionary *> *valid = [NSMutableArray array];
    for (id toolCall in toolCalls) {
        if ([toolCall isKindOfClass:NSDictionary.class] && ISHLLMToolCallID(toolCall).length > 0)
            [valid addObject:toolCall];
    }
    return valid;
}

// Synchronous chat POST that works for both http (ATS-blocked, so hand-rolled
// socket) and https (NSURLSession). Blocks; call from a background queue.
NSData *ISHLLMSynchronousChatPost(NSURL *url, NSData *body, NSString *apiKey,
                                         NSInteger *statusCodeOut, NSError **errorOut) {
    if ([url.scheme.lowercaseString isEqualToString:@"http"])
        return ISHLLMDirectHTTPPost(url, body, apiKey, statusCodeOut, errorOut);

    __block NSData *resultData = nil;
    __block NSInteger statusCode = 0;
    __block NSError *resultError = nil;
    dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);
    NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:url];
    request.HTTPMethod = @"POST";
    [request setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
    if (apiKey.length > 0)
        [request setValue:[@"Bearer " stringByAppendingString:apiKey] forHTTPHeaderField:@"Authorization"];
    request.HTTPBody = body;
    NSURLSessionDataTask *task = [NSURLSession.sharedSession dataTaskWithRequest:request completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
        resultData = data;
        resultError = error;
        if ([response isKindOfClass:NSHTTPURLResponse.class])
            statusCode = ((NSHTTPURLResponse *) response).statusCode;
        dispatch_semaphore_signal(semaphore);
    }];
    [task resume];
    dispatch_semaphore_wait(semaphore, DISPATCH_TIME_FOREVER);
    if (statusCodeOut != NULL)
        *statusCodeOut = statusCode;
    if (errorOut != NULL)
        *errorOut = resultError;
    return resultData;
}

// Run one command in the guest and format stdout+stderr plus a status note for
// feeding back to the model. summaryOut (optional) gets a compact one-line status
// for the transcript, so the verbose output stays out of the user's view while the
// model still receives the full result. Blocks; call from a background queue (the
// primitive repoints the kernel's `current`, so it must not run on a guest task
// thread).
NSString *ISHLLMRunGuestShellCommand(NSString *command, NSString **summaryOut) {
    NSInteger timeoutSeconds = ISHLLMToolTimeoutSeconds();
    NSInteger outputLimitKB = ISHLLMToolOutputLimitKB();
    // "Open Everything as Default User": tool commands run as the same account
    // the user's own workspace terminals sign in as, via su (see
    // run_guest_command_capture_user). nil account = the plain root path.
    NSString *account = [AppDelegate headlessCommandAccountName];
    struct guest_command_result result;
    int rc = account != nil
        ? run_guest_command_capture_user(account.UTF8String, command.UTF8String, NULL,
                                         (int) (timeoutSeconds * 1000), (size_t) outputLimitKB * 1024, &result)
        : run_guest_command_capture(command.UTF8String, NULL,
                                    (int) (timeoutSeconds * 1000), (size_t) outputLimitKB * 1024, &result);
    if (rc < 0) {
        if (summaryOut != NULL)
            *summaryOut = @"failed to start";
        // Never fall back to running as root here: the failure is reported
        // instead, naming the setting that chose the su path (mirrors the
        // Display applet's Wayland-session failure guidance).
        if (account != nil)
            return [NSString stringWithFormat:@"Could not start the command as user \"%@\" (error %d). "
                    @"The \"Open Everything as Default User\" setting runs commands via /bin/su -- "
                    @"if su is missing or that account cannot log in, disable the setting or fix the account.",
                    account, rc];
        return [NSString stringWithFormat:@"Could not start the command (error %d). Is the guest system booted?", rc];
    }

    NSString *captured = @"";
    if (result.output != NULL && result.output_len > 0)
        captured = [[NSString alloc] initWithBytes:result.output length:result.output_len encoding:NSUTF8StringEncoding] ?: @"";
    NSMutableArray<NSString *> *notes = [NSMutableArray array];
    if (result.timed_out)
        [notes addObject:[NSString stringWithFormat:@"timed out after %lds", (long) timeoutSeconds]];
    if (result.truncated)
        [notes addObject:[NSString stringWithFormat:@"output truncated to %ld KB", (long) outputLimitKB]];
    if (result.exited)
        [notes addObject:[NSString stringWithFormat:@"exit code %d", result.exit_code]];
    else if (result.term_signal)
        [notes addObject:[NSString stringWithFormat:@"killed by signal %d", result.term_signal]];

    if (summaryOut != NULL) {
        NSMutableArray<NSString *> *bits = [NSMutableArray array];
        if (result.exited)
            [bits addObject:[NSString stringWithFormat:@"exit %d", result.exit_code]];
        else if (result.term_signal)
            [bits addObject:[NSString stringWithFormat:@"signal %d", result.term_signal]];
        if (result.timed_out)
            [bits addObject:@"timed out"];
        [bits addObject:result.output_len > 0
            ? [NSString stringWithFormat:@"%zu bytes%@", result.output_len, result.truncated ? @"+" : @""]
            : @"no output"];
        *summaryOut = [bits componentsJoinedByString:@" · "];
    }
    free(result.output);

    NSMutableString *out = [NSMutableString stringWithString:captured.length > 0 ? captured : @"(no output)"];
    if (notes.count > 0) {
        if (![out hasSuffix:@"\n"])
            [out appendString:@"\n"];
        [out appendFormat:@"[%@]", [notes componentsJoinedByString:@", "]];
    }
    return out;
}

// Probe the guest once per chat session to learn the distro and which common
// tools are installed, so the tool guidance is distro-accurate (iSH-AOK runs
// Alpine/BusyBox, Debian/Devuan, and others). Returns a system-message note, or
// nil if the guest could not be probed. This is a fixed, read-only probe -- not a
// model-generated command -- so it runs without the per-command confirmation.
NSString *ISHLLMDetectGuestEnvironmentNote(void) {
    const char *probe =
        ". /etc/os-release 2>/dev/null; "
        "printf 'distro=%s %s\\n' \"${NAME:-Linux}\" \"${VERSION_ID:-}\"; "
        "for t in curl wget jq python3 python git make gcc cc vi vim nano tar unzip ssh nc ss netstat ip ifconfig ps top apk apt apt-get dpkg rc-service rc-status service systemctl crontab; do "
        "command -v \"$t\" >/dev/null 2>&1 && printf 'have=%s\\n' \"$t\"; done; "
        "[ -d /etc/init.d ] && printf 'have=/etc/init.d\\n'";
    // Probe as the same account tool commands will run as, so the detected
    // tools/PATH match what run_shell actually sees.
    NSString *account = [AppDelegate headlessCommandAccountName];
    struct guest_command_result result;
    int rc = account != nil
        ? run_guest_command_capture_user(account.UTF8String, probe, NULL, 10000, 16 * 1024, &result)
        : run_guest_command_capture(probe, NULL, 10000, 16 * 1024, &result);
    if (rc < 0)
        return nil;
    NSString *raw = (result.output != NULL && result.output_len > 0)
        ? ([[NSString alloc] initWithBytes:result.output length:result.output_len encoding:NSUTF8StringEncoding] ?: @"")
        : @"";
    free(result.output);
    if (raw.length == 0)
        return nil;

    NSString *distro = nil;
    NSMutableArray<NSString *> *tools = [NSMutableArray array];
    for (NSString *line in [raw componentsSeparatedByString:@"\n"]) {
        if ([line hasPrefix:@"distro="])
            distro = [[line substringFromIndex:7] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        else if ([line hasPrefix:@"have="])
            [tools addObject:[line substringFromIndex:5]];
    }

    BOOL hasCurl = [tools containsObject:@"curl"];
    BOOL hasWget = [tools containsObject:@"wget"];
    NSMutableString *note = [NSMutableString stringWithFormat:
        @"You can run shell commands in this iSH Linux guest with the run_shell tool; it returns combined stdout+stderr (capped at %ld KB, killed after %lds).",
        (long) ISHLLMToolOutputLimitKB(), (long) ISHLLMToolTimeoutSeconds()];
    if (account != nil)
        [note appendFormat:@" Commands run as the unprivileged user \"%@\" (the app's \"Open Everything as "
         @"Default User\" setting), not root -- expect permission errors from root-only operations "
         @"(package installs, service control) and say so rather than retrying.", account];
    if (distro.length > 0)
        [note appendFormat:@" Detected distro: %@.", distro];
    if (hasCurl && hasWget)
        [note appendString:@" Both curl and wget are installed (e.g. curl -fsSL URL or wget -qO- URL)."];
    else if (hasCurl)
        [note appendString:@" curl is installed (e.g. curl -fsSL URL); wget was not found."];
    else if (hasWget)
        [note appendString:@" wget is installed (e.g. wget -qO- URL); curl was not found."];
    else
        [note appendString:@" Neither curl nor wget was detected; use another approach for HTTP if needed."];

    // iSH-AOK never has systemd -- Alpine boots OpenRC (rc-service/rc-status),
    // Devuan boots sysvinit (service, /etc/init.d/<name>). Say this plainly so
    // the model doesn't burn a turn discovering that systemctl is absent.
    if ([tools containsObject:@"rc-service"]) {
        [note appendString:@" Init system: OpenRC. Manage services with rc-service <name> status|start|stop and see everything running with rc-status."];
    } else if ([tools containsObject:@"service"] || [tools containsObject:@"/etc/init.d"]) {
        [note appendString:@" Init system: sysvinit. Manage services with service <name> status|start|stop, or directly via /etc/init.d/<name> status|start|stop; list scripts with ls /etc/init.d."];
    } else if (![tools containsObject:@"systemctl"]) {
        [note appendString:@" No init/service manager tooling (rc-service/service/systemctl) was detected in PATH."];
    }
    if ([tools containsObject:@"apk"])
        [note appendString:@" Package manager: apk (e.g. apk info, apk add <pkg> as root)."];
    else if ([tools containsObject:@"apt"] || [tools containsObject:@"apt-get"] || [tools containsObject:@"dpkg"])
        [note appendString:@" Package manager: apt/dpkg (e.g. dpkg -l, apt list --installed, apt install <pkg> as root)."];
    if (tools.count > 0)
        [note appendFormat:@" Detected tools: %@.", [tools componentsJoinedByString:@", "]];
    [note appendString:@" This list is not exhaustive -- absence above doesn't mean a tool is missing, only that it wasn't in the fixed probe list. Use only tools you've confirmed are present; if a command reports 'not found', try an alternative or check with command -v first."];
    return note;
}

// Build the tool-loop system message: the cached distro/tool note plus a fresh
// current-time anchor. The time anchor matters because time/date questions
// otherwise lead the model to guess or reuse a stale timestamp from an earlier
// turn (especially when a network tool call fails).
NSString *ISHLLMToolSystemNote(NSString *environmentNote) {
    NSDateFormatter *formatter = [[NSDateFormatter alloc] init];
    formatter.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
    formatter.timeZone = [NSTimeZone timeZoneWithAbbreviation:@"UTC"];
    formatter.dateFormat = @"EEEE yyyy-MM-dd HH:mm:ss";
    NSString *now = [formatter stringFromDate:[NSDate date]];
    NSMutableString *note = [NSMutableString string];
    if (environmentNote.length > 0)
        [note appendString:environmentNote];
    if (note.length > 0)
        [note appendString:@" "];
    [note appendFormat:@"The current date and time is %@ UTC -- treat this as the authoritative clock and convert to other time zones from it, rather than guessing or reusing a time from an earlier message.", now];
    return note;
}
