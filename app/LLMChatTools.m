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
#import "GuestFileBridge.h"

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

static NSDictionary *ISHLLMFunctionTool(NSString *name, NSString *description,
                                        NSDictionary *properties, NSArray<NSString *> *required) {
    return @{
        @"type": @"function",
        @"function": @{
            @"name": name,
            @"description": description,
            @"parameters": @{@"type": @"object", @"properties": properties, @"required": required},
        },
    };
}

static NSDictionary *ISHLLMStringParameter(NSString *description) {
    return @{@"type": @"string", @"description": description};
}

static NSDictionary *ISHLLMIntegerParameter(NSString *description) {
    return @{@"type": @"integer", @"description": description};
}

static NSDictionary *ISHLLMBooleanParameter(NSString *description) {
    return @{@"type": @"boolean", @"description": description};
}

// The tools offered to the model, after OpenCode's set: a shell, and file
// tools that do the common jobs without shell quoting (an edit is an exact
// string replacement, the form models are trained to produce). run_shell
// keeps its name so saved chats replay.
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
    NSString *pathNote = @"Absolute, or relative to the working directory.";
    return @[
        ISHLLMFunctionTool(@"run_shell",
            [NSString stringWithFormat:@"Run a command line in the local iSH-AOK Linux shell, starting in the working directory, and return its combined stdout and stderr. %@Use it to run programs, build and test, fetch web pages or APIs (curl/wget), or install packages. Prefer read_file, edit_file, write_file, grep and glob for working with files. The userland varies by distro -- it may be a minimal BusyBox/Alpine system or a full Debian/Devuan/glibc one -- so use the tools that are actually present (a per-session environment note lists what was detected) and try an alternative if a command reports 'not found'. Output is capped at %ld KB and the command is killed after %ld seconds.", identityNote, (long) ISHLLMToolOutputLimitKB(), (long) ISHLLMToolTimeoutSeconds()],
            @{@"command": ISHLLMStringParameter(@"The shell command line to execute, e.g. curl -fsSL 'https://wttr.in/Paris?format=3' (or wget -qO- on BusyBox systems)")},
            @[@"command"]),
        ISHLLMFunctionTool(@"read_file",
            @"Read a text file. Returns its lines numbered from 1, a tab after each number (the numbers are not part of the file). Reads up to 2000 lines from `offset`; lines longer than 2000 characters are cut short. Read a file before editing or overwriting it.",
            @{@"path": ISHLLMStringParameter(pathNote),
              @"offset": ISHLLMIntegerParameter(@"The line number to start at (1-based). Optional."),
              @"limit": ISHLLMIntegerParameter(@"How many lines to read. Optional, at most 2000.")},
            @[@"path"]),
        ISHLLMFunctionTool(@"write_file",
            @"Create a file, or replace an existing file's whole content. Creates missing parent directories. An existing file must have been read with read_file first. For a change to part of a file, use edit_file instead.",
            @{@"path": ISHLLMStringParameter(pathNote),
              @"content": ISHLLMStringParameter(@"The complete new content of the file.")},
            @[@"path", @"content"]),
        ISHLLMFunctionTool(@"edit_file",
            @"Replace exact text in a file. old_string must match the file exactly, including whitespace and indentation (copy it from read_file output, without the line numbers), and must occur exactly once unless replace_all is true; include enough surrounding lines to make it unique. The file must have been read with read_file first.",
            @{@"path": ISHLLMStringParameter(pathNote),
              @"old_string": ISHLLMStringParameter(@"The text to replace."),
              @"new_string": ISHLLMStringParameter(@"The text to put in its place."),
              @"replace_all": ISHLLMBooleanParameter(@"Replace every occurrence instead of exactly one. Optional.")},
            @[@"path", @"old_string", @"new_string"]),
        ISHLLMFunctionTool(@"list_directory",
            @"List a directory's entries: subdirectories end in /, files show their size, symbolic links show their target.",
            @{@"path": ISHLLMStringParameter(@"The directory. Optional; defaults to the working directory.")},
            @[]),
        ISHLLMFunctionTool(@"glob",
            @"Find files by name pattern, e.g. \"**/*.c\" or \"src/**/*.h\". A pattern with no / matches file names in any directory. * does not cross /, ** does, {a,b} is either. Skips .git and node_modules. Returns paths relative to the search directory, at most 200.",
            @{@"pattern": ISHLLMStringParameter(@"The glob pattern."),
              @"path": ISHLLMStringParameter(@"The directory to search. Optional; defaults to the working directory.")},
            @[@"pattern"]),
        ISHLLMFunctionTool(@"todo_write",
            @"Keep a short task list for multi-step work, shown back to you with every change: pass the whole list each time, with each item's status. Use it for tasks of three or more steps; mark one item in_progress while you work on it and completed as soon as it is done.",
            @{@"todos": @{@"type": @"array", @"description": @"The complete list, in order.",
                          @"items": @{@"type": @"object",
                                      @"properties": @{@"content": ISHLLMStringParameter(@"What to do."),
                                                       @"status": @{@"type": @"string", @"enum": @[@"pending", @"in_progress", @"completed"]}},
                                      @"required": @[@"content", @"status"]}}},
            @[@"todos"]),
        ISHLLMFunctionTool(@"grep",
            @"Search file contents for a regular expression (extended syntax), recursively. Returns matching lines as path:line:text, at most 100. Skips .git and node_modules.",
            @{@"pattern": ISHLLMStringParameter(@"The regular expression."),
              @"path": ISHLLMStringParameter(@"A directory or file to search. Optional; defaults to the working directory."),
              @"include": ISHLLMStringParameter(@"Only search files whose name matches this glob, e.g. \"*.c\". Optional."),
              @"ignore_case": ISHLLMBooleanParameter(@"Match case-insensitively. Optional.")},
            @[@"pattern"]),
    ];
}

NSString *ISHLLMToolCallID(NSDictionary *toolCall) {
    return [toolCall[@"id"] isKindOfClass:NSString.class] ? toolCall[@"id"] : nil;
}

NSString *ISHLLMToolCallName(NSDictionary *toolCall) {
    NSDictionary *function = [toolCall[@"function"] isKindOfClass:NSDictionary.class] ? toolCall[@"function"] : nil;
    return [function[@"name"] isKindOfClass:NSString.class] ? function[@"name"] : nil;
}

// The OpenAI tool-call "arguments" field is a JSON *string*. Some local
// servers (Ollama's OpenAI endpoint among them) send the object itself.
NSDictionary *ISHLLMToolCallArguments(NSDictionary *toolCall) {
    NSDictionary *function = [toolCall[@"function"] isKindOfClass:NSDictionary.class] ? toolCall[@"function"] : nil;
    id arguments = function[@"arguments"];
    if ([arguments isKindOfClass:NSDictionary.class])
        return arguments;
    if (![arguments isKindOfClass:NSString.class] || [arguments length] == 0)
        return @{};
    NSData *data = [arguments dataUsingEncoding:NSUTF8StringEncoding];
    id json = data != nil ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
    return [json isKindOfClass:NSDictionary.class] ? json : @{};
}

NSString *ISHLLMToolCallCommand(NSDictionary *toolCall) {
    NSDictionary *json = ISHLLMToolCallArguments(toolCall);
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
// Guest output is bytes. A command that prints Latin-1, or output cut at the
// size cap in the middle of a UTF-8 sequence, used to decode to nothing at
// all; keep what is readable instead.
static NSString *ISHLLMStringFromGuestBytes(const char *bytes, size_t length) {
    if (bytes == NULL || length == 0)
        return @"";
    for (size_t trim = 0; trim <= 3 && trim < length; trim++) {
        NSString *text = [[NSString alloc] initWithBytes:bytes length:length - trim encoding:NSUTF8StringEncoding];
        if (text != nil)
            return text;
    }
    return [[NSString alloc] initWithBytes:bytes length:length encoding:NSISOLatin1StringEncoding] ?: @"";
}

// Runs `command` as the account the chat's tools run as ("Open Everything as
// Default User", or root), starting in workingDirectory. Returns the
// primitive's result code; *accountOut names the account (nil = root).
static int ISHLLMCaptureGuestCommand(NSString *command, NSString *workingDirectory, NSInteger timeoutSeconds,
                                     size_t maxOutput, struct guest_command_result *result, NSString **accountOut) {
    // Start where the file tools resolve relative paths, so "make" and
    // read_file("Makefile") mean the same directory. A missing directory is
    // reported rather than silently running somewhere else.
    if (workingDirectory.length > 0)
        command = [NSString stringWithFormat:@"cd %@ || exit 1\n%@", ISHLLMShellQuote(workingDirectory), command];
    // "Open Everything as Default User": tool commands run as the same account
    // the user's own workspace terminals sign in as, via su (see
    // run_guest_command_capture_user). nil account = the plain root path.
    NSString *account = [AppDelegate headlessCommandAccountName];
    if (accountOut != NULL)
        *accountOut = account;
    return account != nil
        ? run_guest_command_capture_user(account.UTF8String, command.UTF8String, NULL,
                                         (int) (timeoutSeconds * 1000), maxOutput, result)
        : run_guest_command_capture(command.UTF8String, NULL,
                                    (int) (timeoutSeconds * 1000), maxOutput, result);
}

static NSString *ISHLLMCommandStartFailure(NSString *account, int rc) {
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

NSString *ISHLLMRunGuestShellCommand(NSString *command, NSString *workingDirectory, NSString **summaryOut) {
    NSInteger timeoutSeconds = ISHLLMToolTimeoutSeconds();
    NSInteger outputLimitKB = ISHLLMToolOutputLimitKB();
    NSString *account = nil;
    struct guest_command_result result;
    int rc = ISHLLMCaptureGuestCommand(command, workingDirectory, timeoutSeconds, (size_t) outputLimitKB * 1024, &result, &account);
    if (rc < 0) {
        if (summaryOut != NULL)
            *summaryOut = @"failed to start";
        return ISHLLMCommandStartFailure(account, rc);
    }

    NSString *captured = ISHLLMStringFromGuestBytes(result.output, result.output_len);
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
NSString *ISHLLMDetectGuestEnvironmentNote(NSString **homeOut) {
    const char *probe =
        ". /etc/os-release 2>/dev/null; "
        "printf 'distro=%s %s\\n' \"${NAME:-Linux}\" \"${VERSION_ID:-}\"; "
        "printf 'home=%s\\n' \"${HOME:-}\"; "
        "for t in curl wget jq python3 python git make gcc cc vi vim nano tar unzip ssh nc ss netstat ip ifconfig ps top rg apk apt apt-get dpkg rc-service rc-status service systemctl crontab; do "
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
        if ([line hasPrefix:@"home="] && homeOut != NULL && [line substringFromIndex:5].length > 1)
            *homeOut = [line substringFromIndex:5];
        if ([line hasPrefix:@"distro="])
            distro = [[line substringFromIndex:7] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        else if ([line hasPrefix:@"have="])
            [tools addObject:[line substringFromIndex:5]];
    }

    BOOL hasCurl = [tools containsObject:@"curl"];
    BOOL hasWget = [tools containsObject:@"wget"];
    NSMutableString *note = [NSMutableString stringWithString:@"About this iSH-AOK Linux guest:"];
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
// The one sentence of the system note that changes every request. Kept
// separate so a provider with prompt caching (Anthropic) can place it after
// the cache breakpoint instead of invalidating the whole prefix each time.
NSString *ISHLLMClockNote(void) {
    NSDateFormatter *formatter = [[NSDateFormatter alloc] init];
    formatter.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
    formatter.timeZone = [NSTimeZone timeZoneWithAbbreviation:@"UTC"];
    formatter.dateFormat = @"EEEE yyyy-MM-dd HH:mm:ss";
    return [NSString stringWithFormat:@"The current date and time is %@ UTC -- treat this as the authoritative clock and convert to other time zones from it, rather than guessing or reusing a time from an earlier message.",
            [formatter stringFromDate:[NSDate date]]];
}

NSString *ISHLLMToolSystemNoteWithoutClock(NSString *environmentNote, NSString *workingDirectory, BOOL fileTools) {
    NSMutableString *note = [NSMutableString string];
    if (fileTools)
        [note appendFormat:@"You can work in this iSH-AOK Linux guest with tools: read_file, write_file, edit_file, list_directory, glob and grep for files, and run_shell for commands (combined stdout+stderr, capped at %ld KB, killed after %lds). Prefer the file tools to shell commands for reading, searching and changing files. ",
            (long) ISHLLMToolOutputLimitKB(), (long) ISHLLMToolTimeoutSeconds()];
    else
        [note appendFormat:@"You can run shell commands in this iSH-AOK Linux guest with the run_shell tool; it returns combined stdout+stderr (capped at %ld KB, killed after %lds). ",
            (long) ISHLLMToolOutputLimitKB(), (long) ISHLLMToolTimeoutSeconds()];
    if (environmentNote.length > 0)
        [note appendFormat:@"%@ ", environmentNote];
    if (workingDirectory.length > 0)
        [note appendFormat:@"The working directory is %@: run_shell starts there and relative paths in the file tools resolve against it. ", workingDirectory];
    return [note stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
}

NSString *ISHLLMToolSystemNote(NSString *environmentNote, NSString *workingDirectory, BOOL fileTools) {
    return [NSString stringWithFormat:@"%@ %@", ISHLLMToolSystemNoteWithoutClock(environmentNote, workingDirectory, fileTools), ISHLLMClockNote()];
}

// MARK: - File tools and the tool context

NSString *ISHLLMShellQuote(NSString *text) {
    return [NSString stringWithFormat:@"'%@'", [text stringByReplacingOccurrencesOfString:@"'" withString:@"'\\''"]];
}

// Limits for the file tools, after OpenCode's read tool. A read is whole-file
// through the bridge, so the byte cap is also what edit_file can open.
static const NSUInteger kISHLLMReadMaxBytes = 8 * 1024 * 1024;
static const NSInteger kISHLLMReadDefaultLines = 2000;
static const NSUInteger kISHLLMReadMaxLineLength = 2000;
static const NSUInteger kISHLLMListMaxEntries = 1000;
static const NSUInteger kISHLLMGlobMaxResults = 200;
static const NSUInteger kISHLLMGrepMaxMatches = 100;
static const NSUInteger kISHLLMGrepMaxLineLength = 300;
// find/grep listings are filtered here, not shown whole, so they get more
// room than a run_shell result before being cut off.
static const size_t kISHLLMSearchCaptureBytes = 4 * 1024 * 1024;

@implementation ISHLLMFileChange
@end

// What the change review keeps, oldest dropped first: every change's before
// and after content is held in memory while the chat is open.
static const NSUInteger kISHLLMChangeHistoryBytes = 16 * 1024 * 1024;

@implementation ISHLLMToolContext {
    NSMutableDictionary<NSString *, NSArray *> *_reads; // path -> @[size, mtime or NSNull]
    NSArray<NSDictionary *> *_todos;
    NSMutableArray<ISHLLMFileChange *> *_changes;
}

- (instancetype)init {
    if ((self = [super init])) {
        _reads = [NSMutableDictionary dictionary];
        _changes = [NSMutableArray array];
    }
    return self;
}

- (NSArray<ISHLLMFileChange *> *)changes {
    @synchronized (self) {
        return [_changes copy];
    }
}

- (void)recordChange:(ISHLLMFileChange *)change {
    @synchronized (self) {
        [_changes addObject:change];
        NSUInteger total = 0;
        for (ISHLLMFileChange *each in _changes)
            total += each.before.length + each.after.length;
        while (total > kISHLLMChangeHistoryBytes && _changes.count > 1) {
            total -= _changes[0].before.length + _changes[0].after.length;
            [_changes removeObjectAtIndex:0];
        }
    }
}

- (void)recordReadOfPath:(NSString *)path size:(unsigned long long)size modified:(NSDate *)modified {
    @synchronized (self) {
        _reads[path] = @[@(size), modified ?: (id) NSNull.null];
    }
}

- (NSString *)stalenessProblemForPath:(NSString *)path size:(unsigned long long)size modified:(NSDate *)modified {
    NSArray *stamp;
    @synchronized (self) {
        stamp = _reads[path];
    }
    if (stamp == nil)
        return [NSString stringWithFormat:@"%@ has not been read in this chat. Read it with read_file first, so the change is made against its current content.", path];
    BOOL sameSize = [stamp[0] unsignedLongLongValue] == size;
    BOOL sameTime = [stamp[1] isEqual:NSNull.null] ? modified == nil : (modified != nil && [stamp[1] isEqualToDate:modified]);
    if (!sameSize || !sameTime)
        return [NSString stringWithFormat:@"%@ has changed since it was last read. Read it again with read_file before changing it.", path];
    return nil;
}

- (void)forgetReads {
    @synchronized (self) {
        [_reads removeAllObjects];
    }
}

- (NSArray<NSDictionary *> *)todos {
    @synchronized (self) {
        return _todos ?: @[];
    }
}

- (void)setTodos:(NSArray<NSDictionary *> *)todos {
    @synchronized (self) {
        _todos = [todos copy];
    }
}

@end

// Where a chat starts before the environment probe reports the real $HOME.
static NSString *ISHLLMDefaultWorkingDirectory(void) {
    NSString *account = [AppDelegate headlessCommandAccountName];
    return account != nil ? [@"/home/" stringByAppendingString:account] : @"/root";
}

// Lexical normalisation of a guest path: absolute, no ".", "..", or repeated
// slashes. Not NSString's stringByStandardizingPath, which expands ~ to the
// HOST home and consults the host filesystem.
NSString *ISHLLMResolveGuestPath(NSString *raw, NSString *workingDirectory) {
    NSString *path = raw;
    if ([path isEqualToString:@"~"] || [path hasPrefix:@"~/"]) {
        NSString *home = [AppDelegate headlessCommandAccountName] != nil ? ISHLLMDefaultWorkingDirectory() : @"/root";
        path = [home stringByAppendingString:[path substringFromIndex:1]];
    }
    if (![path hasPrefix:@"/"])
        path = [NSString stringWithFormat:@"%@/%@", workingDirectory.length > 0 ? workingDirectory : ISHLLMDefaultWorkingDirectory(), path];
    NSMutableArray<NSString *> *parts = [NSMutableArray array];
    for (NSString *part in [path componentsSeparatedByString:@"/"]) {
        if (part.length == 0 || [part isEqualToString:@"."])
            continue;
        if ([part isEqualToString:@".."]) {
            [parts removeLastObject];
            continue;
        }
        [parts addObject:part];
    }
    return [@"/" stringByAppendingString:[parts componentsJoinedByString:@"/"]];
}

static NSString *ISHLLMStringArgument(NSDictionary *arguments, NSString *key) {
    id value = arguments[key];
    return [value isKindOfClass:NSString.class] ? value : nil;
}

static NSInteger ISHLLMIntegerArgument(NSDictionary *arguments, NSString *key, NSInteger fallback) {
    id value = arguments[key];
    if ([value isKindOfClass:NSNumber.class])
        return [value integerValue];
    if ([value isKindOfClass:NSString.class] && [value length] > 0)
        return [value integerValue];
    return fallback;
}

static BOOL ISHLLMBoolArgument(NSDictionary *arguments, NSString *key) {
    id value = arguments[key];
    if ([value isKindOfClass:NSNumber.class])
        return [value boolValue];
    if ([value isKindOfClass:NSString.class])
        return [value isEqualToString:@"true"] || [value isEqualToString:@"1"];
    return NO;
}

static NSString *ISHLLMPreviewLines(NSString *text, NSUInteger maxLines, NSString *prefix) {
    NSArray<NSString *> *lines = [text componentsSeparatedByString:@"\n"];
    NSMutableArray<NSString *> *shown = [NSMutableArray array];
    for (NSUInteger i = 0; i < lines.count && i < maxLines; i++) {
        NSString *line = lines[i];
        if (line.length > 120)
            line = [[line substringToIndex:120] stringByAppendingString:@"…"];
        [shown addObject:[prefix stringByAppendingString:line]];
    }
    if (lines.count > maxLines)
        [shown addObject:[NSString stringWithFormat:@"%@… %lu more lines", prefix, (unsigned long) (lines.count - maxLines)]];
    return [shown componentsJoinedByString:@"\n"];
}

static NSUInteger ISHLLMLineCount(NSString *text) {
    if (text.length == 0)
        return 0;
    NSUInteger count = [text componentsSeparatedByString:@"\n"].count;
    return [text hasSuffix:@"\n"] ? count - 1 : count;
}

@implementation ISHLLMToolInvocation {
    NSString *_workingDirectory;
}

+ (NSSet<NSString *> *)knownTools {
    static NSSet<NSString *> *set;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        set = [NSSet setWithArray:@[@"run_shell", @"read_file", @"write_file", @"edit_file", @"list_directory", @"glob", @"grep", @"todo_write"]];
    });
    return set;
}

+ (instancetype)invocationWithToolCall:(NSDictionary *)toolCall context:(ISHLLMToolContext *)context {
    ISHLLMToolInvocation *invocation = [ISHLLMToolInvocation new];
    invocation->_callID = ISHLLMToolCallID(toolCall) ?: @"";
    invocation->_name = ISHLLMToolCallName(toolCall) ?: @"";
    invocation->_arguments = ISHLLMToolCallArguments(toolCall);
    invocation->_workingDirectory = context.workingDirectory ?: ISHLLMDefaultWorkingDirectory();
    [invocation parse];
    return invocation;
}

- (void)parse {
    NSString *name = _name;
    if (![ISHLLMToolInvocation.knownTools containsObject:name]) {
        _category = ISHLLMToolCategoryShell;
        _problem = [NSString stringWithFormat:@"There is no tool named '%@'. The tools are: %@.", name.length > 0 ? name : @"(unnamed)",
                    [[ISHLLMToolInvocation.knownTools.allObjects sortedArrayUsingSelector:@selector(compare:)] componentsJoinedByString:@", "]];
        return;
    }
    if ([name isEqualToString:@"run_shell"]) {
        _category = ISHLLMToolCategoryShell;
        _command = ISHLLMStringArgument(_arguments, @"command") ?: ISHLLMStringArgument(_arguments, @"cmd");
        if (_command.length == 0)
            _problem = @"run_shell needs a non-empty \"command\".";
        return;
    }
    if ([name isEqualToString:@"todo_write"]) {
        _category = ISHLLMToolCategoryRead;
        if (![_arguments[@"todos"] isKindOfClass:NSArray.class])
            _problem = @"todo_write needs \"todos\", an array of {content, status}.";
        return;
    }
    BOOL edits = [name isEqualToString:@"write_file"] || [name isEqualToString:@"edit_file"];
    _category = edits ? ISHLLMToolCategoryEdit : ISHLLMToolCategoryRead;
    NSString *rawPath = ISHLLMStringArgument(_arguments, @"path") ?: ISHLLMStringArgument(_arguments, @"file_path");
    BOOL pathRequired = [name isEqualToString:@"read_file"] || edits;
    if (rawPath.length == 0) {
        if (pathRequired) {
            _problem = [NSString stringWithFormat:@"%@ needs a \"path\".", name];
            return;
        }
        rawPath = _workingDirectory;
    }
    _path = ISHLLMResolveGuestPath(rawPath, _workingDirectory);
    if ([name isEqualToString:@"write_file"] && ISHLLMStringArgument(_arguments, @"content") == nil)
        _problem = @"write_file needs \"content\" (use an empty string for an empty file).";
    else if ([name isEqualToString:@"edit_file"] && (ISHLLMStringArgument(_arguments, @"old_string") == nil || ISHLLMStringArgument(_arguments, @"new_string") == nil))
        _problem = @"edit_file needs \"old_string\" and \"new_string\".";
    else if ([name isEqualToString:@"edit_file"] && ISHLLMStringArgument(_arguments, @"old_string").length == 0)
        _problem = @"edit_file needs a non-empty \"old_string\". To create a file, use write_file.";
    else if (([name isEqualToString:@"glob"] || [name isEqualToString:@"grep"]) && ISHLLMStringArgument(_arguments, @"pattern").length == 0)
        _problem = [NSString stringWithFormat:@"%@ needs a non-empty \"pattern\".", name];
}

- (ISHLLMPermissionAction)permissionWithReason:(NSString **)reasonOut {
    // The task list lives in the chat and touches nothing else.
    if ([_name isEqualToString:@"todo_write"]) {
        if (reasonOut != NULL)
            *reasonOut = nil;
        return ISHLLMPermissionAllow;
    }
    ISHLLMPermissionAction categoryAction = ISHLLMCategoryAction(_category);
    if (_category == ISHLLMToolCategoryShell)
        return ISHLLMEvaluateShellCommand(_command ?: @"", ISHLLMShellRules(), categoryAction, reasonOut);
    return ISHLLMEvaluatePathAccess(_category, _path ?: @"/", _workingDirectory, categoryAction, reasonOut);
}

- (NSString *)confirmationTitle {
    if ([_name isEqualToString:@"run_shell"])
        return @"Run shell command?";
    if ([_name isEqualToString:@"write_file"])
        return @"Write file?";
    if ([_name isEqualToString:@"edit_file"])
        return @"Edit file?";
    if ([_name isEqualToString:@"read_file"])
        return @"Read file?";
    if ([_name isEqualToString:@"list_directory"])
        return @"List directory?";
    return @"Search files?";
}

- (NSString *)confirmationMessage {
    if ([_name isEqualToString:@"run_shell"])
        return [NSString stringWithFormat:@"The model wants to run this in the iSH-AOK shell, in %@:\n\n%@", _workingDirectory, _command];
    if ([_name isEqualToString:@"write_file"]) {
        NSString *content = ISHLLMStringArgument(_arguments, @"content") ?: @"";
        return [NSString stringWithFormat:@"%@\n%lu lines, %lu bytes\n\n%@", _path,
                (unsigned long) ISHLLMLineCount(content), (unsigned long) [content lengthOfBytesUsingEncoding:NSUTF8StringEncoding],
                ISHLLMPreviewLines(content, 12, @"")];
    }
    if ([_name isEqualToString:@"edit_file"]) {
        NSString *oldText = ISHLLMStringArgument(_arguments, @"old_string") ?: @"";
        NSString *newText = ISHLLMStringArgument(_arguments, @"new_string") ?: @"";
        return [NSString stringWithFormat:@"%@%@\n\n%@\n%@", _path,
                ISHLLMBoolArgument(_arguments, @"replace_all") ? @" (every occurrence)" : @"",
                ISHLLMPreviewLines(oldText, 8, @"− "), ISHLLMPreviewLines(newText, 8, @"+ ")];
    }
    NSString *pattern = ISHLLMStringArgument(_arguments, @"pattern");
    return pattern.length > 0 ? [NSString stringWithFormat:@"%@ in %@", pattern, _path] : _path;
}

@end

NSArray<NSString *> *ISHLLMToolCallDescriptions(NSArray *toolCalls) {
    NSMutableArray<NSString *> *lines = [NSMutableArray array];
    for (NSDictionary *toolCall in toolCalls) {
        if (![toolCall isKindOfClass:NSDictionary.class])
            continue;
        NSString *name = ISHLLMToolCallName(toolCall) ?: @"tool";
        NSDictionary *arguments = ISHLLMToolCallArguments(toolCall);
        NSString *line;
        if ([name isEqualToString:@"todo_write"]) {
            NSArray *todos = [arguments[@"todos"] isKindOfClass:NSArray.class] ? arguments[@"todos"] : @[];
            NSUInteger done = 0;
            for (NSDictionary *todo in todos)
                done += [todo isKindOfClass:NSDictionary.class] && [todo[@"status"] isEqual:@"completed"];
            [lines addObject:[NSString stringWithFormat:@"todo: %lu of %lu done", (unsigned long) done, (unsigned long) todos.count]];
            continue;
        }
        if ([name isEqualToString:@"run_shell"]) {
            NSString *command = ISHLLMToolCallCommand(toolCall) ?: @"";
            NSString *firstLine = [command componentsSeparatedByString:@"\n"].firstObject ?: @"";
            if (firstLine.length > 100 || [command containsString:@"\n"])
                firstLine = [[firstLine substringToIndex:MIN(firstLine.length, (NSUInteger) 100)] stringByAppendingString:@" …"];
            line = [@"$ " stringByAppendingString:firstLine];
        } else {
            NSDictionary<NSString *, NSString *> *verbs = @{@"read_file": @"read", @"write_file": @"write", @"edit_file": @"edit",
                                                            @"list_directory": @"list", @"glob": @"glob", @"grep": @"grep"};
            NSString *verb = verbs[name] ?: name;
            NSString *pattern = ISHLLMStringArgument(arguments, @"pattern");
            NSString *path = ISHLLMStringArgument(arguments, @"path") ?: ISHLLMStringArgument(arguments, @"file_path");
            if (pattern.length > 0)
                line = path.length > 0 ? [NSString stringWithFormat:@"%@ %@ in %@", verb, pattern, path] : [NSString stringWithFormat:@"%@ %@", verb, pattern];
            else
                line = path.length > 0 ? [NSString stringWithFormat:@"%@ %@", verb, path] : verb;
        }
        [lines addObject:line];
    }
    return lines;
}

// MARK: Synchronous bridge calls, for the guest command queue

// The bridge completes on the main thread; these wait for it from the chat's
// serial guest queue, which is never the main thread, so the wait cannot
// deadlock and tool calls keep their order against run_shell commands.
static ISHGuestFileItem *ISHLLMStat(NSString *path, NSError **errorOut) {
    __block ISHGuestFileItem *result = nil;
    __block NSError *resultError = nil;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    [ISHGuestFileBridge.sharedBridge statAtGuestPath:path completion:^(ISHGuestFileItem *item, NSError *error) {
        result = item;
        resultError = error;
        dispatch_semaphore_signal(done);
    }];
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    if (errorOut != NULL)
        *errorOut = resultError;
    return result;
}

static NSData *ISHLLMReadWhole(NSString *path, NSUInteger maxBytes, NSError **errorOut) {
    __block NSData *result = nil;
    __block NSError *resultError = nil;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    [ISHGuestFileBridge.sharedBridge readFileAtGuestPath:path maxBytes:maxBytes completion:^(NSData *data, NSError *error) {
        result = data;
        resultError = error;
        dispatch_semaphore_signal(done);
    }];
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    if (errorOut != NULL)
        *errorOut = resultError;
    return result;
}

static BOOL ISHLLMWriteWhole(NSData *data, NSString *path, NSError **errorOut) {
    __block BOOL result = NO;
    __block NSError *resultError = nil;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    [ISHGuestFileBridge.sharedBridge writeData:data toGuestPath:path completion:^(BOOL ok, NSError *error) {
        result = ok;
        resultError = error;
        dispatch_semaphore_signal(done);
    }];
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    if (errorOut != NULL)
        *errorOut = resultError;
    return result;
}

static BOOL ISHLLMMakeDirectory(NSString *path, NSError **errorOut) {
    __block BOOL result = NO;
    __block NSError *resultError = nil;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    [ISHGuestFileBridge.sharedBridge createDirectoryAtGuestPath:path completion:^(BOOL ok, NSError *error) {
        result = ok;
        resultError = error;
        dispatch_semaphore_signal(done);
    }];
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    if (errorOut != NULL)
        *errorOut = resultError;
    return result;
}

static NSArray<ISHGuestFileItem *> *ISHLLMListWhole(NSString *path, NSError **errorOut) {
    __block NSArray<ISHGuestFileItem *> *result = nil;
    __block NSError *resultError = nil;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    [ISHGuestFileBridge.sharedBridge listDirectoryAtGuestPath:path completion:^(NSArray<ISHGuestFileItem *> *items, NSError *error) {
        result = items;
        resultError = error;
        dispatch_semaphore_signal(done);
    }];
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    if (errorOut != NULL)
        *errorOut = resultError;
    return result;
}

static NSString *ISHLLMFileError(NSString *path, NSError *error) {
    NSString *what = error.localizedDescription.length > 0 ? error.localizedDescription : @"failed";
    return [NSString stringWithFormat:@"Error: %@: %@", path, what];
}

// The bridge acts as the guest's init task (root). When the chat's commands
// run as the default user, a file tool must not reach what that user could
// not: this is the plain owner/group/other mode check, which leaves out
// supplementary groups and ACLs, so it can refuse something the user's own
// shell could do -- never the other way round.
static NSString *ISHLLMAccessProblem(ISHGuestFileItem *item, int want, NSString *path) {
    NSInteger uid = 0, gid = 0;
    if (![AppDelegate headlessCommandAccountOwner:&uid gid:&gid] || uid == 0)
        return nil;
    mode_t mode = item.posixMode;
    int bits = item.uid == (uid_t) uid ? (mode >> 6) & 7 : (item.gid == (gid_t) gid ? (mode >> 3) & 7 : mode & 7);
    if ((bits & want) == want)
        return nil;
    return [NSString stringWithFormat:@"Error: %@: permission denied for user \"%@\" (owner %u, group %u, mode %03o). "
            @"The chat's tools run as that account (\"Open Everything as Default User\").",
            path, [AppDelegate headlessCommandAccountName], item.uid, item.gid, (unsigned) (mode & 07777)];
}

// MARK: The tools

static NSString *ISHLLMDecodeText(NSData *data, BOOL *wasUTF8) {
    NSString *text = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
    if (wasUTF8 != NULL)
        *wasUTF8 = text != nil;
    return text ?: [[NSString alloc] initWithData:data encoding:NSISOLatin1StringEncoding];
}

static BOOL ISHLLMLooksBinary(NSData *data) {
    NSUInteger n = MIN(data.length, (NSUInteger) 8192);
    return n > 0 && memchr(data.bytes, 0, n) != NULL;
}

static NSString *ISHLLMReadFileTool(ISHLLMToolInvocation *invocation, ISHLLMToolContext *context, NSString **summaryOut) {
    NSString *path = invocation.path;
    NSError *error = nil;
    ISHGuestFileItem *item = ISHLLMStat(path, &error);
    if (item == nil) {
        *summaryOut = @"not found";
        return ISHLLMFileError(path, error);
    }
    if (item.kind == ISHGuestFileKindDirectory) {
        *summaryOut = @"directory";
        return [NSString stringWithFormat:@"Error: %@ is a directory. Use list_directory to see what is in it.", path];
    }
    NSString *denied = ISHLLMAccessProblem(item, 4, path);
    if (denied != nil) {
        *summaryOut = @"permission denied";
        return denied;
    }
    if (item.size > kISHLLMReadMaxBytes) {
        *summaryOut = @"too large";
        return [NSString stringWithFormat:@"Error: %@ is %.1f MB, more than read_file opens (%lu MB). Use grep to find the part you need, or run_shell with sed -n 'FIRST,LASTp'.",
                path, item.size / 1048576.0, (unsigned long) (kISHLLMReadMaxBytes / 1048576)];
    }
    NSData *data = ISHLLMReadWhole(path, kISHLLMReadMaxBytes, &error);
    if (data == nil) {
        *summaryOut = @"read failed";
        return ISHLLMFileError(path, error);
    }
    if (ISHLLMLooksBinary(data)) {
        *summaryOut = @"binary";
        return [NSString stringWithFormat:@"%@ is a binary file (%lu bytes); read_file shows text only. Use run_shell (e.g. file, hexdump -C | head) to inspect it.",
                path, (unsigned long) data.length];
    }
    [context recordReadOfPath:path size:item.size modified:item.modificationDate];
    BOOL utf8 = YES;
    NSString *text = ISHLLMDecodeText(data, &utf8);
    if (text.length == 0) {
        *summaryOut = @"empty";
        return [NSString stringWithFormat:@"%@ is empty.", path];
    }
    NSMutableArray<NSString *> *lines = [[text componentsSeparatedByString:@"\n"] mutableCopy];
    if ([text hasSuffix:@"\n"])
        [lines removeLastObject];
    NSInteger total = (NSInteger) lines.count;
    NSInteger offset = MAX((NSInteger) 1, ISHLLMIntegerArgument(invocation.arguments, @"offset", 1));
    NSInteger limit = ISHLLMIntegerArgument(invocation.arguments, @"limit", kISHLLMReadDefaultLines);
    limit = MAX((NSInteger) 1, MIN(limit, kISHLLMReadDefaultLines));
    if (offset > total) {
        *summaryOut = @"past end";
        return [NSString stringWithFormat:@"%@ has %ld lines; offset %ld is past the end.", path, (long) total, (long) offset];
    }
    NSInteger last = MIN(total, offset + limit - 1);
    NSMutableString *out = [NSMutableString string];
    NSUInteger longLines = 0;
    for (NSInteger i = offset; i <= last; i++) {
        NSString *line = lines[(NSUInteger) i - 1];
        if ([line hasSuffix:@"\r"])
            line = [line substringToIndex:line.length - 1];
        if (line.length > kISHLLMReadMaxLineLength) {
            line = [[line substringToIndex:kISHLLMReadMaxLineLength] stringByAppendingString:@"… (line cut short)"];
            longLines++;
        }
        [out appendFormat:@"%6ld\t%@\n", (long) i, line];
    }
    if (!utf8)
        [out appendString:@"(Not valid UTF-8; shown as Latin-1.)\n"];
    if (offset > 1 || last < total)
        [out appendFormat:@"(Lines %ld-%ld of %ld. Use offset to read more.)\n", (long) offset, (long) last, (long) total];
    *summaryOut = [NSString stringWithFormat:@"read lines %ld-%ld of %ld", (long) offset, (long) last, (long) total];
    return out;
}

// mkdir -p through the bridge, stopping at the first component that exists.
static NSString *ISHLLMEnsureParentDirectory(NSString *path) {
    NSString *parent = [path stringByDeletingLastPathComponent];
    NSMutableArray<NSString *> *missing = [NSMutableArray array];
    NSString *probe = parent;
    ISHGuestFileItem *existing = nil;
    while (probe.length > 0) {
        NSError *error = nil;
        existing = ISHLLMStat(probe, &error);
        if (existing != nil)
            break;
        if (error.code != -2) // anything but ENOENT is a real failure
            return ISHLLMFileError(probe, error);
        [missing insertObject:probe atIndex:0];
        if ([probe isEqualToString:@"/"])
            break;
        probe = [probe stringByDeletingLastPathComponent];
    }
    if (existing == nil)
        return [NSString stringWithFormat:@"Error: no existing directory above %@.", path];
    if (existing.kind != ISHGuestFileKindDirectory)
        return [NSString stringWithFormat:@"Error: %@ is not a directory.", probe];
    NSString *denied = ISHLLMAccessProblem(existing, 3, probe);
    if (denied != nil)
        return denied;
    for (NSString *directory in missing) {
        NSError *error = nil;
        if (!ISHLLMMakeDirectory(directory, &error))
            return ISHLLMFileError(directory, error);
    }
    return nil;
}

static NSString *ISHLLMWriteFileTool(ISHLLMToolInvocation *invocation, ISHLLMToolContext *context, NSString **summaryOut) {
    NSString *path = invocation.path;
    NSString *content = ISHLLMStringArgument(invocation.arguments, @"content") ?: @"";
    NSError *error = nil;
    ISHGuestFileItem *item = ISHLLMStat(path, &error);
    BOOL exists = item != nil;
    NSData *previous = nil;
    if (!exists && error.code != -2) {
        *summaryOut = @"failed";
        return ISHLLMFileError(path, error);
    }
    if (exists) {
        if (item.kind == ISHGuestFileKindDirectory) {
            *summaryOut = @"directory";
            return [NSString stringWithFormat:@"Error: %@ is a directory.", path];
        }
        NSString *problem = ISHLLMAccessProblem(item, 2, path) ?: [context stalenessProblemForPath:path size:item.size modified:item.modificationDate];
        if (problem != nil) {
            *summaryOut = @"refused";
            return problem;
        }
        // Kept so the user can review and revert the overwrite.
        previous = item.size <= kISHLLMReadMaxBytes ? ISHLLMReadWhole(path, kISHLLMReadMaxBytes, NULL) : nil;
    } else {
        NSString *problem = ISHLLMEnsureParentDirectory(path);
        if (problem != nil) {
            *summaryOut = @"failed";
            return problem;
        }
    }
    NSData *data = [content dataUsingEncoding:NSUTF8StringEncoding] ?: NSData.data;
    if (!ISHLLMWriteWhole(data, path, &error)) {
        *summaryOut = @"write failed";
        return ISHLLMFileError(path, error);
    }
    ISHGuestFileItem *written = ISHLLMStat(path, NULL);
    if (written != nil)
        [context recordReadOfPath:path size:written.size modified:written.modificationDate];
    ISHLLMFileChange *change = [ISHLLMFileChange new];
    change.path = path;
    change.toolName = @"write_file";
    change.created = !exists;
    change.revertible = !exists || previous != nil;
    change.before = previous;
    change.after = data;
    change.date = [NSDate date];
    [context recordChange:change];
    *summaryOut = [NSString stringWithFormat:@"%@ %lu bytes", exists ? @"overwrote" : @"created", (unsigned long) data.length];
    return [NSString stringWithFormat:@"%@ %@ (%lu lines, %lu bytes).", exists ? @"Overwrote" : @"Created", path,
            (unsigned long) ISHLLMLineCount(content), (unsigned long) data.length];
}

static NSArray<NSValue *> *ISHLLMOccurrences(NSString *haystack, NSString *needle) {
    NSMutableArray<NSValue *> *ranges = [NSMutableArray array];
    NSRange search = NSMakeRange(0, haystack.length);
    while (search.length > 0) {
        NSRange found = [haystack rangeOfString:needle options:NSLiteralSearch range:search];
        if (found.location == NSNotFound)
            break;
        [ranges addObject:[NSValue valueWithRange:found]];
        NSUInteger next = NSMaxRange(found);
        search = NSMakeRange(next, haystack.length - next);
    }
    return ranges;
}

static NSUInteger ISHLLMLineNumberAt(NSString *text, NSUInteger location) {
    NSUInteger line = 1;
    for (NSUInteger i = 0; i < location && i < text.length; i++) {
        if ([text characterAtIndex:i] == '\n')
            line++;
    }
    return line;
}

static NSString *ISHLLMNumberedExcerpt(NSString *text, NSUInteger firstLine, NSUInteger lastLine) {
    NSArray<NSString *> *lines = [text componentsSeparatedByString:@"\n"];
    NSMutableString *out = [NSMutableString string];
    for (NSUInteger n = MAX((NSUInteger) 1, firstLine); n <= lastLine && n <= lines.count; n++) {
        NSString *line = lines[n - 1];
        if (line.length > 240)
            line = [[line substringToIndex:240] stringByAppendingString:@"…"];
        [out appendFormat:@"%6lu\t%@\n", (unsigned long) n, line];
    }
    return out;
}

static NSString *ISHLLMEditFileTool(ISHLLMToolInvocation *invocation, ISHLLMToolContext *context, NSString **summaryOut) {
    NSString *path = invocation.path;
    NSString *oldText = ISHLLMStringArgument(invocation.arguments, @"old_string");
    NSString *newText = ISHLLMStringArgument(invocation.arguments, @"new_string");
    BOOL replaceAll = ISHLLMBoolArgument(invocation.arguments, @"replace_all");
    if ([oldText isEqualToString:newText]) {
        *summaryOut = @"no change";
        return @"Error: old_string and new_string are the same, so there is nothing to change.";
    }
    NSError *error = nil;
    ISHGuestFileItem *item = ISHLLMStat(path, &error);
    if (item == nil) {
        *summaryOut = @"not found";
        return error.code == -2
            ? [NSString stringWithFormat:@"Error: %@ does not exist. Use write_file to create it.", path]
            : ISHLLMFileError(path, error);
    }
    if (item.kind == ISHGuestFileKindDirectory) {
        *summaryOut = @"directory";
        return [NSString stringWithFormat:@"Error: %@ is a directory.", path];
    }
    NSString *problem = ISHLLMAccessProblem(item, 6, path) ?: [context stalenessProblemForPath:path size:item.size modified:item.modificationDate];
    if (problem != nil) {
        *summaryOut = @"refused";
        return problem;
    }
    if (item.size > kISHLLMReadMaxBytes) {
        *summaryOut = @"too large";
        return [NSString stringWithFormat:@"Error: %@ is too large for edit_file.", path];
    }
    NSData *data = ISHLLMReadWhole(path, kISHLLMReadMaxBytes, &error);
    if (data == nil) {
        *summaryOut = @"read failed";
        return ISHLLMFileError(path, error);
    }
    BOOL utf8 = NO;
    NSString *text = ISHLLMDecodeText(data, &utf8);
    if (!utf8 || ISHLLMLooksBinary(data)) {
        *summaryOut = @"not text";
        return [NSString stringWithFormat:@"Error: %@ is not UTF-8 text, so edit_file cannot change it safely.", path];
    }
    NSArray<NSValue *> *matches = ISHLLMOccurrences(text, oldText);
    // A CRLF file read through read_file shows no \r, so the model's
    // old_string has bare newlines; match it the way the file spells them.
    if (matches.count == 0 && [text containsString:@"\r\n"] && [oldText containsString:@"\n"] && ![oldText containsString:@"\r\n"]) {
        oldText = [oldText stringByReplacingOccurrencesOfString:@"\n" withString:@"\r\n"];
        newText = [newText stringByReplacingOccurrencesOfString:@"\n" withString:@"\r\n"];
        matches = ISHLLMOccurrences(text, oldText);
    }
    if (matches.count == 0) {
        *summaryOut = @"no match";
        NSMutableString *message = [NSMutableString stringWithFormat:@"Error: old_string was not found in %@. It must match exactly, including whitespace and indentation; copy it from read_file output without the line numbers.", path];
        NSString *firstLine = [[oldText componentsSeparatedByString:@"\n"].firstObject stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        if (firstLine.length >= 4) {
            NSRange near = [text rangeOfString:firstLine options:NSLiteralSearch];
            if (near.location != NSNotFound)
                [message appendFormat:@" Its first line does appear at line %lu, so the difference is in the lines after it or in the whitespace.", (unsigned long) ISHLLMLineNumberAt(text, near.location)];
        }
        return message;
    }
    if (matches.count > 1 && !replaceAll) {
        *summaryOut = @"ambiguous";
        NSMutableArray<NSString *> *lineNumbers = [NSMutableArray array];
        for (NSValue *match in matches) {
            if (lineNumbers.count == 10)
                break;
            [lineNumbers addObject:[NSString stringWithFormat:@"%lu", (unsigned long) ISHLLMLineNumberAt(text, match.rangeValue.location)]];
        }
        return [NSString stringWithFormat:@"Error: old_string occurs %lu times in %@ (at lines %@%@). Include more of the surrounding text so it matches exactly once, or set replace_all to change every one.",
                (unsigned long) matches.count, path, [lineNumbers componentsJoinedByString:@", "], matches.count > 10 ? @", …" : @""];
    }
    NSUInteger firstLocation = matches.firstObject.rangeValue.location;
    NSString *updated = replaceAll
        ? [text stringByReplacingOccurrencesOfString:oldText withString:newText options:NSLiteralSearch range:NSMakeRange(0, text.length)]
        : [text stringByReplacingCharactersInRange:matches.firstObject.rangeValue withString:newText];
    NSData *updatedData = [updated dataUsingEncoding:NSUTF8StringEncoding];
    if (!ISHLLMWriteWhole(updatedData, path, &error)) {
        *summaryOut = @"write failed";
        return ISHLLMFileError(path, error);
    }
    ISHGuestFileItem *written = ISHLLMStat(path, NULL);
    if (written != nil)
        [context recordReadOfPath:path size:written.size modified:written.modificationDate];
    ISHLLMFileChange *change = [ISHLLMFileChange new];
    change.path = path;
    change.toolName = @"edit_file";
    change.revertible = YES;
    change.before = data;
    change.after = updatedData;
    change.date = [NSDate date];
    [context recordChange:change];
    NSUInteger startLine = ISHLLMLineNumberAt(updated, firstLocation);
    NSUInteger newLines = MAX((NSUInteger) 1, [newText componentsSeparatedByString:@"\n"].count);
    NSInteger delta = (NSInteger) ISHLLMLineCount(updated) - (NSInteger) ISHLLMLineCount(text);
    *summaryOut = [NSString stringWithFormat:@"edited, %lu replacement%@, %+ld lines", (unsigned long) (replaceAll ? matches.count : 1),
                   (replaceAll ? matches.count : 1) == 1 ? @"" : @"s", (long) delta];
    NSUInteger excerptEnd = MIN(startLine + newLines + 2, startLine + 40);
    return [NSString stringWithFormat:@"Edited %@: replaced %lu occurrence%@. The changed region now reads:\n%@",
            path, (unsigned long) (replaceAll ? matches.count : 1), (replaceAll ? matches.count : 1) == 1 ? @"" : @"s",
            ISHLLMNumberedExcerpt(updated, startLine > 3 ? startLine - 3 : 1, excerptEnd)];
}

static NSString *ISHLLMListDirectoryTool(ISHLLMToolInvocation *invocation, NSString **summaryOut) {
    NSString *path = invocation.path;
    NSError *error = nil;
    ISHGuestFileItem *item = ISHLLMStat(path, &error);
    if (item == nil) {
        *summaryOut = @"not found";
        return ISHLLMFileError(path, error);
    }
    if (item.kind != ISHGuestFileKindDirectory) {
        *summaryOut = @"not a directory";
        return [NSString stringWithFormat:@"Error: %@ is not a directory. Use read_file to read it.", path];
    }
    NSString *denied = ISHLLMAccessProblem(item, 5, path);
    if (denied != nil) {
        *summaryOut = @"permission denied";
        return denied;
    }
    NSArray<ISHGuestFileItem *> *items = ISHLLMListWhole(path, &error);
    if (items == nil) {
        *summaryOut = @"failed";
        return ISHLLMFileError(path, error);
    }
    if (items.count == 0) {
        *summaryOut = @"empty";
        return [NSString stringWithFormat:@"%@ is empty.", path];
    }
    NSMutableString *out = [NSMutableString stringWithFormat:@"%@:\n", path];
    NSUInteger shown = 0;
    for (ISHGuestFileItem *entry in items) {
        if (shown == kISHLLMListMaxEntries)
            break;
        if (entry.symlinkTarget != nil)
            [out appendFormat:@"%@%@ -> %@\n", entry.name, entry.kind == ISHGuestFileKindDirectory ? @"/" : @"", entry.symlinkTarget];
        else if (entry.kind == ISHGuestFileKindDirectory)
            [out appendFormat:@"%@/\n", entry.name];
        else if (entry.kind == ISHGuestFileKindRegular)
            [out appendFormat:@"%@  %llu\n", entry.name, entry.size];
        else
            [out appendFormat:@"%@\n", entry.name];
        shown++;
    }
    if (items.count > shown)
        [out appendFormat:@"(%lu more entries not shown.)\n", (unsigned long) (items.count - shown)];
    *summaryOut = [NSString stringWithFormat:@"%lu entries", (unsigned long) items.count];
    return out;
}

static NSString *ISHLLMSearchCommandOutput(NSString *command, NSString *workingDirectory, int *exitCodeOut, BOOL *truncatedOut, NSString **failureOut) {
    struct guest_command_result result;
    NSString *account = nil;
    int rc = ISHLLMCaptureGuestCommand(command, workingDirectory, ISHLLMToolTimeoutSeconds(), kISHLLMSearchCaptureBytes, &result, &account);
    if (rc < 0) {
        *failureOut = ISHLLMCommandStartFailure(account, rc);
        return nil;
    }
    NSString *output = ISHLLMStringFromGuestBytes(result.output, result.output_len);
    *exitCodeOut = result.exited ? result.exit_code : -1;
    *truncatedOut = result.truncated || result.timed_out;
    if (result.timed_out)
        *failureOut = [NSString stringWithFormat:@"(the search was stopped after %lds; results are partial)", (long) ISHLLMToolTimeoutSeconds()];
    free(result.output);
    return output;
}

static NSString *const kISHLLMPruneClause = @"\\( -name .git -o -name node_modules \\) -prune -o";

static NSString *ISHLLMGlobTool(ISHLLMToolInvocation *invocation, NSString **summaryOut) {
    NSString *glob = ISHLLMStringArgument(invocation.arguments, @"pattern");
    if ([glob hasPrefix:@"./"])
        glob = [glob substringFromIndex:2];
    NSRegularExpression *regex = ISHLLMGlobRegex(glob);
    if (regex == nil) {
        *summaryOut = @"bad pattern";
        return [NSString stringWithFormat:@"Error: could not understand the glob pattern %@.", glob];
    }
    BOOL matchNameOnly = ![glob containsString:@"/"];
    NSString *command = [NSString stringWithFormat:@"cd %@ || exit 2\nfind . %@ \\( -type f -o -type l \\) -print 2>/dev/null",
                         ISHLLMShellQuote(invocation.path), kISHLLMPruneClause];
    int exitCode = 0;
    BOOL truncated = NO;
    NSString *failure = nil;
    NSString *output = ISHLLMSearchCommandOutput(command, nil, &exitCode, &truncated, &failure);
    if (output == nil) {
        *summaryOut = @"failed";
        return failure;
    }
    if (exitCode == 2) {
        *summaryOut = @"no directory";
        return [NSString stringWithFormat:@"Error: %@ is not a directory that can be searched.\n%@", invocation.path, output];
    }
    NSMutableArray<NSString *> *found = [NSMutableArray array];
    for (NSString *line in [output componentsSeparatedByString:@"\n"]) {
        if (line.length < 3 || ![line hasPrefix:@"./"])
            continue;
        NSString *relative = [line substringFromIndex:2];
        NSString *subject = matchNameOnly ? relative.lastPathComponent : relative;
        if ([regex firstMatchInString:subject options:0 range:NSMakeRange(0, subject.length)] != nil)
            [found addObject:relative];
    }
    [found sortUsingSelector:@selector(compare:)];
    if (found.count == 0) {
        *summaryOut = @"no files";
        return [NSString stringWithFormat:@"No files matching %@ in %@.%@", glob, invocation.path, failure != nil ? [@" " stringByAppendingString:failure] : @""];
    }
    NSMutableString *out = [NSMutableString stringWithFormat:@"%lu file%@ matching %@ in %@:\n", (unsigned long) found.count, found.count == 1 ? @"" : @"s", glob, invocation.path];
    for (NSUInteger i = 0; i < found.count && i < kISHLLMGlobMaxResults; i++)
        [out appendFormat:@"%@\n", found[i]];
    if (found.count > kISHLLMGlobMaxResults)
        [out appendFormat:@"(%lu more not shown; narrow the pattern or the directory.)\n", (unsigned long) (found.count - kISHLLMGlobMaxResults)];
    if (truncated)
        [out appendString:failure ?: @"(the directory listing was too large to search completely; narrow the directory.)"];
    *summaryOut = [NSString stringWithFormat:@"%lu files", (unsigned long) found.count];
    return out;
}

static NSString *ISHLLMGrepTool(ISHLLMToolInvocation *invocation, NSString **summaryOut) {
    NSString *pattern = ISHLLMStringArgument(invocation.arguments, @"pattern");
    NSString *include = ISHLLMStringArgument(invocation.arguments, @"include");
    BOOL ignoreCase = ISHLLMBoolArgument(invocation.arguments, @"ignore_case");
    NSString *p = ISHLLMShellQuote(pattern);
    NSString *caseFlag = ignoreCase ? @" -i" : @"";
    NSString *rgInclude = include.length > 0 ? [@" -g " stringByAppendingString:ISHLLMShellQuote(include)] : @"";
    NSString *findInclude = include.length > 0 ? [@" -name " stringByAppendingString:ISHLLMShellQuote(include)] : @"";
    NSString *target = ISHLLMShellQuote(invocation.path);
    // ripgrep when the guest has it (faster, and it honours .gitignore, as
    // OpenCode's grep does); otherwise find + grep, which BusyBox has too.
    // A malformed pattern must say so: through find -exec it would read as
    // "no matches" (find exits 1 either way, and its stderr is discarded).
    NSString *command = [NSString stringWithFormat:
        @"echo x | grep -E -e %@ 2>&1 >/dev/null; if [ $? -ge 2 ]; then exit 3; fi\n"
        @"if [ -d %@ ]; then cd %@ || exit 2\n"
        @"  if command -v rg >/dev/null 2>&1; then rg -n --no-heading --color never%@%@ -e %@ .\n"
        @"  else find . %@ -type f%@ -exec grep -snH%@ -E -e %@ {} + 2>/dev/null; fi\n"
        @"elif [ -e %@ ]; then grep -nH%@ -E -e %@ %@\n"
        @"else echo 'No such file or directory'; exit 2; fi",
        p, target, target, caseFlag, rgInclude, p,
        kISHLLMPruneClause, findInclude, caseFlag, p,
        target, caseFlag, p, target];
    int exitCode = 0;
    BOOL truncated = NO;
    NSString *failure = nil;
    NSString *output = ISHLLMSearchCommandOutput(command, nil, &exitCode, &truncated, &failure);
    if (output == nil) {
        *summaryOut = @"failed";
        return failure;
    }
    NSMutableArray<NSString *> *matches = [NSMutableArray array];
    for (NSString *raw in [output componentsSeparatedByString:@"\n"]) {
        if (raw.length == 0)
            continue;
        NSString *line = [raw hasPrefix:@"./"] ? [raw substringFromIndex:2] : raw;
        if (line.length > kISHLLMGrepMaxLineLength)
            line = [[line substringToIndex:kISHLLMGrepMaxLineLength] stringByAppendingString:@"…"];
        [matches addObject:line];
    }
    if (exitCode == 3) {
        *summaryOut = @"bad pattern";
        return [NSString stringWithFormat:@"Error: %@ is not a valid extended regular expression: %@", pattern, output];
    }
    // grep and rg exit 1 for "no match" and 2 for a real error (a missing
    // path, an unreadable file); an error's message is the output.
    if (exitCode == 2 && matches.count <= 3) {
        *summaryOut = @"error";
        return [NSString stringWithFormat:@"Error searching %@: %@", invocation.path, output.length > 0 ? output : @"grep failed"];
    }
    if (matches.count == 0) {
        *summaryOut = @"no matches";
        return [NSString stringWithFormat:@"No matches for %@ in %@.%@", pattern, invocation.path, failure != nil ? [@" " stringByAppendingString:failure] : @""];
    }
    NSMutableString *out = [NSMutableString string];
    for (NSUInteger i = 0; i < matches.count && i < kISHLLMGrepMaxMatches; i++)
        [out appendFormat:@"%@\n", matches[i]];
    if (matches.count > kISHLLMGrepMaxMatches)
        [out appendFormat:@"(%lu more matches not shown; narrow the pattern, path or include.)\n", (unsigned long) (matches.count - kISHLLMGrepMaxMatches)];
    if (truncated)
        [out appendString:failure ?: @"(output was too large to collect completely; narrow the search.)"];
    *summaryOut = [NSString stringWithFormat:@"%lu matches", (unsigned long) matches.count];
    return out;
}

static NSString *ISHLLMTodoWriteTool(ISHLLMToolInvocation *invocation, ISHLLMToolContext *context, NSString **summaryOut) {
    NSMutableArray<NSDictionary *> *todos = [NSMutableArray array];
    for (id item in invocation.arguments[@"todos"]) {
        if (![item isKindOfClass:NSDictionary.class])
            continue;
        NSString *content = ISHLLMStringArgument(item, @"content");
        NSString *status = ISHLLMStringArgument(item, @"status");
        if (content.length == 0)
            continue;
        if (![@[@"pending", @"in_progress", @"completed"] containsObject:status])
            status = @"pending";
        [todos addObject:@{@"content": content, @"status": status}];
    }
    context.todos = todos;
    NSMutableString *out = [NSMutableString string];
    NSUInteger done = 0;
    for (NSDictionary *todo in todos) {
        NSString *status = todo[@"status"];
        done += [status isEqualToString:@"completed"];
        NSString *box = [status isEqualToString:@"completed"] ? @"[x]" : ([status isEqualToString:@"in_progress"] ? @"[>]" : @"[ ]");
        [out appendFormat:@"%@ %@\n", box, todo[@"content"]];
    }
    *summaryOut = [NSString stringWithFormat:@"todo %lu/%lu done", (unsigned long) done, (unsigned long) todos.count];
    return out.length > 0 ? out : @"The task list is empty.";
}

// Project instructions, as OpenCode reads them: the nearest AGENTS.md at or
// above the working directory (CLAUDE.md where there is none), whole up to
// 32 KB. Blocks on the bridge; call on the guest command queue.
NSString *ISHLLMLoadProjectInstructions(NSString *workingDirectory, NSString **sourceOut) {
    if (workingDirectory.length == 0 || ![ISHGuestFileBridge.sharedBridge isGuestAvailable])
        return nil;
    NSString *directory = workingDirectory;
    while (YES) {
        for (NSString *name in @[@"AGENTS.md", @"CLAUDE.md"]) {
            NSString *path = [directory isEqualToString:@"/"] ? [@"/" stringByAppendingString:name] : [directory stringByAppendingPathComponent:name];
            ISHGuestFileItem *item = ISHLLMStat(path, NULL);
            if (item == nil || item.kind != ISHGuestFileKindRegular || ISHLLMAccessProblem(item, 4, path) != nil)
                continue;
            NSData *data = ISHLLMReadWhole(path, 32 * 1024, NULL);
            NSString *text = data != nil ? ISHLLMDecodeText(data, NULL) : nil;
            if (text.length == 0)
                continue;
            if (sourceOut != NULL)
                *sourceOut = path;
            return text;
        }
        if ([directory isEqualToString:@"/"] || directory.length == 0)
            return nil;
        directory = [directory stringByDeletingLastPathComponent];
    }
}

static BOOL ISHLLMRemoveFile(NSString *path, NSError **errorOut) {
    __block BOOL result = NO;
    __block NSError *resultError = nil;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    [ISHGuestFileBridge.sharedBridge removeItemAtGuestPath:path recursive:NO completion:^(BOOL ok, NSError *error) {
        result = ok;
        resultError = error;
        dispatch_semaphore_signal(done);
    }];
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    if (errorOut != NULL)
        *errorOut = resultError;
    return result;
}

void ISHLLMRevertFileChange(ISHLLMFileChange *change, ISHLLMToolContext *context, BOOL force,
                            void (^completion)(BOOL reverted, BOOL changedSince, NSString *message)) {
    dispatch_async(ISHLLMGuestCommandQueue(), ^{
        BOOL reverted = NO, changedSince = NO;
        NSString *message;
        NSString *path = change.path;
        NSError *error = nil;
        ISHGuestFileItem *item = ISHLLMStat(path, &error);
        NSData *current = item != nil && item.size <= kISHLLMReadMaxBytes ? ISHLLMReadWhole(path, kISHLLMReadMaxBytes, NULL) : nil;
        if (!change.revertible) {
            message = [NSString stringWithFormat:@"%@ was too large to keep a copy of, so this change cannot be reverted.", path];
        } else if (!force && ![current isEqualToData:change.after]) {
            changedSince = YES;
            message = item == nil
                ? [NSString stringWithFormat:@"%@ no longer exists.", path]
                : [NSString stringWithFormat:@"%@ has changed since this edit. Reverting would also throw away the later changes.", path];
        } else if (change.created) {
            reverted = item == nil || ISHLLMRemoveFile(path, &error);
            message = reverted ? [NSString stringWithFormat:@"Removed %@, which the model had created.", path] : ISHLLMFileError(path, error);
        } else {
            reverted = ISHLLMWriteWhole(change.before ?: NSData.data, path, &error);
            message = reverted ? [NSString stringWithFormat:@"Restored %@ to how it was before the change.", path] : ISHLLMFileError(path, error);
        }
        if (reverted)
            change.reverted = YES;
        dispatch_async(dispatch_get_main_queue(), ^{
            completion(reverted, changedSince, message);
        });
    });
}

void ISHLLMRunToolInvocation(ISHLLMToolInvocation *invocation, ISHLLMToolContext *context,
                             void (^completion)(NSString *result, NSString *summary)) {
    dispatch_async(ISHLLMGuestCommandQueue(), ^{
        NSString *summary = nil;
        NSString *result;
        NSString *name = invocation.name;
        if (invocation.problem != nil) {
            result = invocation.problem;
            summary = @"not run";
        } else if ([name isEqualToString:@"todo_write"]) {
            result = ISHLLMTodoWriteTool(invocation, context, &summary);
        } else if ([name isEqualToString:@"run_shell"]) {
            result = ISHLLMRunGuestShellCommand(invocation.command, context.workingDirectory, &summary);
        } else if (![ISHGuestFileBridge.sharedBridge isGuestAvailable]) {
            result = @"Error: the guest system is not running, so its files cannot be reached.";
            summary = @"guest not running";
        } else if ([name isEqualToString:@"read_file"]) {
            result = ISHLLMReadFileTool(invocation, context, &summary);
        } else if ([name isEqualToString:@"write_file"]) {
            result = ISHLLMWriteFileTool(invocation, context, &summary);
        } else if ([name isEqualToString:@"edit_file"]) {
            result = ISHLLMEditFileTool(invocation, context, &summary);
        } else if ([name isEqualToString:@"list_directory"]) {
            result = ISHLLMListDirectoryTool(invocation, &summary);
        } else if ([name isEqualToString:@"glob"]) {
            result = ISHLLMGlobTool(invocation, &summary);
        } else {
            result = ISHLLMGrepTool(invocation, &summary);
        }
        // The same cap run_shell's output has: a tool result is resent with
        // every later request until it is compacted.
        NSUInteger cap = (NSUInteger) ISHLLMToolOutputLimitKB() * 1024;
        if (result.length > cap)
            result = [[result substringToIndex:cap] stringByAppendingFormat:@"\n[output truncated to %ld KB]", (long) ISHLLMToolOutputLimitKB()];
        dispatch_async(dispatch_get_main_queue(), ^{
            completion(result ?: @"", summary ?: @"");
        });
    });
}
