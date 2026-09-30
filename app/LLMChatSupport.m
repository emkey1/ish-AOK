//
//  LLMChatSupport.m
//  iSH-AOK
//
//  The Workspace LLM Chat's non-UI plumbing: saved destinations and chats,
//  provider endpoints, and the HTTP/SSE transports.
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
#import "LLMChatAnthropic.h"
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

// Connect to one of the resolved addresses with a bounded timeout (default
// blocking connect can hang ~75s on an unreachable host). Returns a connected,
// blocking socket fd, or -1 with *errnoOut set to the last failure reason.
int ISHLLMConnectWithTimeout(struct addrinfo *results, int timeoutMs, int *errnoOut) {
    int lastErrno = ETIMEDOUT;
    for (struct addrinfo *addr = results; addr != NULL; addr = addr->ai_next) {
        int fd = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
        if (fd < 0) { lastErrno = errno; continue; }
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        if (connect(fd, addr->ai_addr, addr->ai_addrlen) == 0) {
            fcntl(fd, F_SETFL, flags);
            return fd;
        }
        if (errno != EINPROGRESS) { lastErrno = errno; close(fd); continue; }
        struct pollfd pfd = {.fd = fd, .events = POLLOUT};
        int pr = poll(&pfd, 1, timeoutMs);
        if (pr == 0) { lastErrno = ETIMEDOUT; close(fd); continue; }
        if (pr < 0) { lastErrno = errno; close(fd); continue; }
        int soError = 0;
        socklen_t soLen = sizeof(soError);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soError, &soLen) < 0 || soError != 0) {
            lastErrno = soError != 0 ? soError : errno;
            close(fd);
            continue;
        }
        fcntl(fd, F_SETFL, flags);
        return fd;
    }
    if (errnoOut != NULL)
        *errnoOut = lastErrno;
    return -1;
}

// Build a user-facing NSError for a failed connection that names the host:port and
// the reason, so timeouts/refusals are obvious instead of a bare errno.
NSError *ISHLLMConnectionError(NSString *host, NSString *port, int errnoValue) {
    NSString *reason = [NSString stringWithUTF8String:strerror(errnoValue)] ?: @"connection failed";
    NSString *message = [NSString stringWithFormat:@"Could not connect to %@:%@ — %@. The model server must be reachable from this device (check the URL/port, that the server is listening on all interfaces, and any VPN/firewall between them).", host, port, reason];
    return [NSError errorWithDomain:NSPOSIXErrorDomain code:errnoValue userInfo:@{NSLocalizedDescriptionKey: message}];
}

// Dedicated serial queue for guest-shell tool execution. run_guest_command_capture
// runs emulated guest code (spawns a task, manipulates kernel signal/`current`
// state) and must be kept OFF the shared libdispatch global pool that the HTTP
// requests run on -- otherwise running a tool command can leave a pooled worker
// thread in a state that wedges later network requests (observed as connect()
// timeouts after a tool ran). A private queue gives guest commands their own,
// isolated worker thread.
dispatch_queue_t ISHLLMGuestCommandQueue(void) {
    static dispatch_queue_t queue;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        queue = dispatch_queue_create("aok.llm.guest-shell", DISPATCH_QUEUE_SERIAL);
    });
    return queue;
}

NSURL *ISHLLMPersistDirectoryURL(void) {
    NSURL *containerURL = ContainerURL();
    if (containerURL == nil)
        return nil;
    return [[containerURL URLByAppendingPathComponent:@"AOK" isDirectory:YES]
            URLByAppendingPathComponent:@"persist" isDirectory:YES];
}

NSURL *ISHLLMTranscriptURL(void) {
    NSURL *directoryURL = ISHLLMPersistDirectoryURL();
    return [directoryURL URLByAppendingPathComponent:@"llm-chat.json" isDirectory:NO];
}

NSURL *ISHLLMExtractsDirectoryURL(void) {
    return [ISHLLMPersistDirectoryURL() URLByAppendingPathComponent:@"llm-extracts" isDirectory:YES];
}

NSString *ISHLLMSanitizeFilenameComponent(NSString *value) {
    NSMutableString *result = [NSMutableString string];
    NSCharacterSet *allowed = [NSCharacterSet characterSetWithCharactersInString:@"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-" ];
    for (NSUInteger i = 0; i < value.length; i++) {
        unichar ch = [value characterAtIndex:i];
        if ([allowed characterIsMember:ch])
            [result appendFormat:@"%C", ch];
    }
    return result.length > 0 ? result : @"snippet";
}

NSString *ISHLLMExtensionForFenceLanguage(NSString *language) {
    NSString *lower = language.lowercaseString;
    if ([lower isEqualToString:@"sh"] || [lower isEqualToString:@"bash"] || [lower isEqualToString:@"zsh"] || [lower isEqualToString:@"shell"])
        return @"sh";
    if ([lower isEqualToString:@"python"] || [lower isEqualToString:@"py"])
        return @"py";
    if ([lower isEqualToString:@"javascript"] || [lower isEqualToString:@"js"])
        return @"js";
    if ([lower isEqualToString:@"typescript"] || [lower isEqualToString:@"ts"])
        return @"ts";
    if ([lower isEqualToString:@"objective-c"] || [lower isEqualToString:@"objc"] || [lower isEqualToString:@"m"])
        return @"m";
    if ([lower isEqualToString:@"c"])
        return @"c";
    if ([lower isEqualToString:@"cpp"] || [lower isEqualToString:@"c++"])
        return @"cpp";
    return @"txt";
}

#pragma mark - Chat destinations


NSString *ISHLLMStringValue(NSDictionary *dictionary, NSString *key) {
    id value = dictionary[key];
    return [value isKindOfClass:NSString.class] ? value : @"";
}

// Snapshot of the live configuration as a destination entry.
NSDictionary<NSString *, NSString *> *ISHLLMDestinationFromCurrentPreferences(NSString *identifier, NSString *name) {
    UserPreferences *preferences = UserPreferences.shared;
    NSString *provider = preferences.llmProvider ?: @"Custom";
    return @{
        kISHLLMDestinationID: identifier.length > 0 ? identifier : NSUUID.UUID.UUIDString,
        kISHLLMDestinationName: name.length > 0 ? name : provider,
        kISHLLMDestinationProvider: provider,
        kISHLLMDestinationURL: preferences.llmServerURL ?: @"",
        kISHLLMDestinationModel: preferences.llmModel ?: @"",
        kISHLLMDestinationAPIKey: preferences.llmAPIKey ?: @"",
    };
}

// Reading accessor, with the upgrade seed: someone arriving from an older
// build has exactly one configured endpoint (the four scalars), so it becomes
// destination #1 and stays selected. Entries without a usable id are dropped
// rather than trusted, since the id is what selection and editing key on.
NSArray<NSDictionary<NSString *, NSString *> *> *ISHLLMDestinations(void) {
    NSMutableArray<NSDictionary<NSString *, NSString *> *> *valid = [NSMutableArray array];
    for (id entry in UserPreferences.shared.llmDestinations) {
        if ([entry isKindOfClass:NSDictionary.class] && ISHLLMStringValue(entry, kISHLLMDestinationID).length > 0)
            [valid addObject:entry];
    }
    if (valid.count == 0) {
        NSDictionary<NSString *, NSString *> *seeded = ISHLLMDestinationFromCurrentPreferences(nil, nil);
        [valid addObject:seeded];
        UserPreferences.shared.llmDestinations = valid;
        UserPreferences.shared.llmActiveDestinationID = seeded[kISHLLMDestinationID];
    }
    return valid;
}

// Index of the selected destination. A stale or deleted id resolves to the
// first entry instead of failing -- there is always exactly one active
// destination, and ISHLLMDestinations() guarantees the array is non-empty.
NSUInteger ISHLLMActiveDestinationIndex(void) {
    NSArray<NSDictionary<NSString *, NSString *> *> *destinations = ISHLLMDestinations();
    NSString *activeID = UserPreferences.shared.llmActiveDestinationID;
    if (activeID.length > 0) {
        for (NSUInteger i = 0; i < destinations.count; i++) {
            if ([ISHLLMStringValue(destinations[i], kISHLLMDestinationID) isEqualToString:activeID])
                return i;
        }
    }
    return 0;
}

NSDictionary<NSString *, NSString *> *ISHLLMActiveDestination(void) {
    return ISHLLMDestinations()[ISHLLMActiveDestinationIndex()];
}

NSString *ISHLLMDestinationDisplayName(NSDictionary<NSString *, NSString *> *destination) {
    NSString *name = ISHLLMStringValue(destination, kISHLLMDestinationName);
    if (name.length > 0)
        return name;
    NSString *provider = ISHLLMStringValue(destination, kISHLLMDestinationProvider);
    return provider.length > 0 ? provider : @"Destination";
}

// One-line "what does this destination point at" summary for pickers.
NSString *ISHLLMDestinationSubtitle(NSDictionary<NSString *, NSString *> *destination) {
    NSString *model = ISHLLMStringValue(destination, kISHLLMDestinationModel);
    NSString *provider = ISHLLMStringValue(destination, kISHLLMDestinationProvider);
    if ([provider.lowercaseString containsString:@"foundation models"])
        return model.length > 0 ? [@"On-device · " stringByAppendingString:model] : @"On-device";
    NSString *host = [NSURL URLWithString:ISHLLMStringValue(destination, kISHLLMDestinationURL)].host ?: @"";
    if (model.length > 0 && host.length > 0)
        return [NSString stringWithFormat:@"%@ · %@", model, host];
    if (model.length > 0)
        return model;
    return host.length > 0 ? host : @"Not configured";
}

// The ONLY place a destination is written into the four scalars.
void ISHLLMActivateDestination(NSDictionary<NSString *, NSString *> *destination) {
    UserPreferences *preferences = UserPreferences.shared;
    preferences.llmActiveDestinationID = ISHLLMStringValue(destination, kISHLLMDestinationID);
    preferences.llmProvider = ISHLLMStringValue(destination, kISHLLMDestinationProvider) ?: @"Custom";
    preferences.llmServerURL = ISHLLMStringValue(destination, kISHLLMDestinationURL);
    preferences.llmModel = ISHLLMStringValue(destination, kISHLLMDestinationModel);
    preferences.llmAPIKey = ISHLLMStringValue(destination, kISHLLMDestinationAPIKey);
}

// The inverse, and the reason the saved set can't drift: every edit that
// writes one of the four scalars (the Settings rows, the provider preset
// picker, the model picker, the /model command) calls this straight after, so
// the active entry always describes the live configuration.
void ISHLLMSyncActiveDestinationFromPreferences(void) {
    NSMutableArray<NSDictionary<NSString *, NSString *> *> *destinations = [ISHLLMDestinations() mutableCopy];
    NSUInteger index = ISHLLMActiveDestinationIndex();
    NSDictionary<NSString *, NSString *> *existing = destinations[index];
    // A destination the user never renamed follows its provider; one they did
    // name keeps that name across a provider change.
    NSString *name = ISHLLMStringValue(existing, kISHLLMDestinationName);
    if (name.length == 0 || [name isEqualToString:ISHLLMStringValue(existing, kISHLLMDestinationProvider)])
        name = nil;
    destinations[index] = ISHLLMDestinationFromCurrentPreferences(ISHLLMStringValue(existing, kISHLLMDestinationID), name);
    UserPreferences.shared.llmDestinations = destinations;
    UserPreferences.shared.llmActiveDestinationID = destinations[index][kISHLLMDestinationID];
}

// Upsert by id; a destination not already in the array is appended.
void ISHLLMSaveDestination(NSDictionary<NSString *, NSString *> *destination) {
    NSString *identifier = ISHLLMStringValue(destination, kISHLLMDestinationID);
    if (identifier.length == 0)
        return;
    NSMutableArray<NSDictionary<NSString *, NSString *> *> *destinations = [ISHLLMDestinations() mutableCopy];
    for (NSUInteger i = 0; i < destinations.count; i++) {
        if ([ISHLLMStringValue(destinations[i], kISHLLMDestinationID) isEqualToString:identifier]) {
            destinations[i] = destination;
            UserPreferences.shared.llmDestinations = destinations;
            // Editing the active destination has to move the live scalars too,
            // or the chat keeps talking to the pre-edit endpoint.
            if (i == ISHLLMActiveDestinationIndex())
                ISHLLMActivateDestination(destination);
            return;
        }
    }
    [destinations addObject:destination];
    UserPreferences.shared.llmDestinations = destinations;
}

// Deleting the active destination selects a survivor; the last one can't be
// deleted, since "no destination" has no meaning for the four scalars.
BOOL ISHLLMDeleteDestinationWithID(NSString *identifier) {
    NSMutableArray<NSDictionary<NSString *, NSString *> *> *destinations = [ISHLLMDestinations() mutableCopy];
    if (destinations.count <= 1)
        return NO;
    BOOL wasActive = [ISHLLMStringValue(ISHLLMActiveDestination(), kISHLLMDestinationID) isEqualToString:identifier];
    NSUInteger removedIndex = NSNotFound;
    for (NSUInteger i = 0; i < destinations.count; i++) {
        if ([ISHLLMStringValue(destinations[i], kISHLLMDestinationID) isEqualToString:identifier]) {
            removedIndex = i;
            break;
        }
    }
    if (removedIndex == NSNotFound)
        return NO;
    [destinations removeObjectAtIndex:removedIndex];
    UserPreferences.shared.llmDestinations = destinations;
    if (wasActive)
        ISHLLMActivateDestination(destinations[removedIndex < destinations.count ? removedIndex : destinations.count - 1]);
    return YES;
}

#pragma mark - Chat sessions

// Chats are stored one file per session under /AOK/persist/llm-chats, with an
// index document beside them naming the sessions and which one is selected:
//
//   llm-chats/index.json          {version, active, sessions:[{id,title,...}]}
//   llm-chats/<session-id>.json   the message array, same shape as before
//
// Session files use the same writeToURL:/arrayWithContentsOfURL: mechanism
// the single transcript always used (a property list, despite the .json
// name) so the on-disk message format is unchanged and a migrated chat reads
// back exactly as it was written.
NSURL *ISHLLMSessionsDirectoryURL(void) {
    return [ISHLLMPersistDirectoryURL() URLByAppendingPathComponent:@"llm-chats" isDirectory:YES];
}

NSURL *ISHLLMSessionIndexURL(void) {
    return [ISHLLMSessionsDirectoryURL() URLByAppendingPathComponent:@"index.json" isDirectory:NO];
}

NSURL *ISHLLMSessionFileURL(NSString *sessionID) {
    NSString *component = ISHLLMSanitizeFilenameComponent(sessionID ?: @"");
    return [ISHLLMSessionsDirectoryURL() URLByAppendingPathComponent:[component stringByAppendingString:@".json"] isDirectory:NO];
}

// ContainerURL() is nil when the app group isn't configured, and every URL
// derived from it is then nil too. Rather than handing nil URLs to Foundation
// at four call sites, the chats simply live in memory for that launch.
BOOL ISHLLMSessionStorageAvailable(void) {
    return ISHLLMSessionsDirectoryURL() != nil;
}

void ISHLLMEnsureSessionsDirectory(void) {
    NSURL *directoryURL = ISHLLMSessionsDirectoryURL();
    if (directoryURL != nil)
        [NSFileManager.defaultManager createDirectoryAtURL:directoryURL withIntermediateDirectories:YES attributes:nil error:nil];
}

NSArray<NSDictionary<NSString *, id> *> *ISHLLMValidMessagesFromStoredArray(NSArray *stored) {
    NSMutableArray<NSDictionary<NSString *, id> *> *messages = [NSMutableArray array];
    for (id message in stored) {
        if ([message isKindOfClass:NSDictionary.class] && [message[@"role"] isKindOfClass:NSString.class] && [message[@"content"] isKindOfClass:NSString.class])
            [messages addObject:message];
    }
    return messages;
}

NSArray<NSDictionary<NSString *, id> *> *ISHLLMLoadSessionMessages(NSString *sessionID) {
    if (sessionID.length == 0 || !ISHLLMSessionStorageAvailable())
        return @[];
    NSArray *stored = [NSArray arrayWithContentsOfURL:ISHLLMSessionFileURL(sessionID)];
    return [stored isKindOfClass:NSArray.class] ? ISHLLMValidMessagesFromStoredArray(stored) : @[];
}

void ISHLLMWriteSessionMessages(NSString *sessionID, NSArray<NSDictionary<NSString *, id> *> *messages) {
    if (sessionID.length == 0 || !ISHLLMSessionStorageAvailable())
        return;
    ISHLLMEnsureSessionsDirectory();
    [messages writeToURL:ISHLLMSessionFileURL(sessionID) atomically:YES];
}

// A one-line label for a chat, taken from its first real user turn -- the
// same thing the user would have typed as a title. Slash commands and the
// terminal-context prompts are skipped; those name the tool, not the chat.
NSString *ISHLLMSessionTitleFromMessages(NSArray<NSDictionary<NSString *, id> *> *messages) {
    for (NSDictionary<NSString *, id> *message in messages) {
        if (![ISHLLMStringValue(message, @"role") isEqualToString:@"user"])
            continue;
        NSString *content = [ISHLLMStringValue(message, @"content") stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        if (content.length == 0 || [content hasPrefix:@"/"])
            continue;
        NSString *firstLine = [content componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet].firstObject ?: content;
        firstLine = [firstLine stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        if (firstLine.length == 0)
            continue;
        if (firstLine.length > 42)
            firstLine = [[firstLine substringToIndex:41] stringByAppendingString:@"…"];
        return firstLine;
    }
    return @"";
}

NSDictionary<NSString *, id> *ISHLLMNewSessionEntry(NSString *title) {
    NSTimeInterval now = NSDate.date.timeIntervalSince1970;
    return @{
        @"id": NSUUID.UUID.UUIDString,
        @"title": title.length > 0 ? title : @"New Chat",
        @"created": @(now),
        @"updated": @(now),
        @"destination": ISHLLMStringValue(ISHLLMActiveDestination(), kISHLLMDestinationID),
        @"system": @"",
        @"count": @0,
    };
}

// The index, rebuilt from whatever is on disk and always valid: at least one
// session, a selected id that exists, and (on first run in this build) the
// pre-sessions transcript carried in as the first chat. The legacy
// llm-chat.json is copied, never moved -- an older build reopened afterwards
// still finds the chat where it left it.

NSDictionary<NSString *, id> *ISHLLMLoadSessionIndexDocument(void) {
    NSDictionary *stored = ISHLLMSessionStorageAvailable() ? [NSDictionary dictionaryWithContentsOfURL:ISHLLMSessionIndexURL()] : nil;
    NSMutableArray<NSDictionary<NSString *, id> *> *sessions = [NSMutableArray array];
    if ([stored isKindOfClass:NSDictionary.class] && [stored[@"sessions"] isKindOfClass:NSArray.class]) {
        for (id entry in stored[@"sessions"]) {
            if ([entry isKindOfClass:NSDictionary.class] && ISHLLMStringValue(entry, @"id").length > 0)
                [sessions addObject:entry];
        }
    }
    NSString *activeID = [stored isKindOfClass:NSDictionary.class] ? ISHLLMStringValue(stored, @"active") : @"";
    BOOL synthesized = NO;

    if (sessions.count == 0) {
        NSArray *legacy = ISHLLMSessionStorageAvailable() ? [NSArray arrayWithContentsOfURL:ISHLLMTranscriptURL()] : nil;
        NSArray<NSDictionary<NSString *, id> *> *legacyMessages = [legacy isKindOfClass:NSArray.class] ? ISHLLMValidMessagesFromStoredArray(legacy) : @[];
        NSMutableDictionary<NSString *, id> *entry = [ISHLLMNewSessionEntry(ISHLLMSessionTitleFromMessages(legacyMessages)) mutableCopy];
        if (legacyMessages.count > 0) {
            entry[@"count"] = @(legacyMessages.count);
            ISHLLMWriteSessionMessages(entry[@"id"], legacyMessages);
        }
        [sessions addObject:entry];
        activeID = entry[@"id"];
        synthesized = YES;
    }

    BOOL activeExists = NO;
    for (NSDictionary<NSString *, id> *entry in sessions) {
        if ([ISHLLMStringValue(entry, @"id") isEqualToString:activeID]) {
            activeExists = YES;
            break;
        }
    }
    if (!activeExists) {
        activeID = ISHLLMStringValue(sessions.firstObject, @"id");
        synthesized = YES;
    }
    // Anything invented here MUST be written back before returning. This
    // function is a read accessor called several times per switch, and each
    // call re-derives what it did not find -- so a migration that stayed in
    // memory would mint a new chat id (and a new copy of the migrated
    // transcript) on every single call, leaving the user on an empty chat with
    // their old conversation stranded in orphan files.
    if (synthesized)
        ISHLLMWriteSessionIndex(sessions, activeID);
    return @{@"version": @1, @"active": activeID, @"sessions": sessions};
}

void ISHLLMWriteSessionIndex(NSArray<NSDictionary<NSString *, id> *> *sessions, NSString *activeID) {
    if (!ISHLLMSessionStorageAvailable())
        return;
    ISHLLMEnsureSessionsDirectory();
    [@{@"version": @1, @"active": activeID ?: @"", @"sessions": sessions ?: @[]} writeToURL:ISHLLMSessionIndexURL() atomically:YES];
}

NSArray<NSDictionary<NSString *, id> *> *ISHLLMSessionEntries(void) {
    return ISHLLMLoadSessionIndexDocument()[@"sessions"];
}

// Newest activity first, which is the order a chat list is useful in.
NSArray<NSDictionary<NSString *, id> *> *ISHLLMSessionEntriesByRecency(void) {
    return [ISHLLMSessionEntries() sortedArrayUsingComparator:^NSComparisonResult(NSDictionary<NSString *, id> *a, NSDictionary<NSString *, id> *b) {
        double left = [a[@"updated"] isKindOfClass:NSNumber.class] ? [a[@"updated"] doubleValue] : 0.0;
        double right = [b[@"updated"] isKindOfClass:NSNumber.class] ? [b[@"updated"] doubleValue] : 0.0;
        if (left == right)
            return NSOrderedSame;
        return left > right ? NSOrderedAscending : NSOrderedDescending;
    }];
}

NSString *ISHLLMActiveSessionID(void) {
    return ISHLLMLoadSessionIndexDocument()[@"active"];
}

NSDictionary<NSString *, id> *ISHLLMSessionEntryWithID(NSString *sessionID) {
    for (NSDictionary<NSString *, id> *entry in ISHLLMSessionEntries()) {
        if ([ISHLLMStringValue(entry, @"id") isEqualToString:sessionID])
            return entry;
    }
    return nil;
}

// Merge `updates` into one session entry in place, leaving the rest alone.
void ISHLLMUpdateSessionEntry(NSString *sessionID, NSDictionary<NSString *, id> *updates) {
    if (sessionID.length == 0)
        return;
    NSDictionary<NSString *, id> *document = ISHLLMLoadSessionIndexDocument();
    NSMutableArray<NSDictionary<NSString *, id> *> *sessions = [document[@"sessions"] mutableCopy];
    for (NSUInteger i = 0; i < sessions.count; i++) {
        if (![ISHLLMStringValue(sessions[i], @"id") isEqualToString:sessionID])
            continue;
        NSMutableDictionary<NSString *, id> *entry = [sessions[i] mutableCopy];
        [entry addEntriesFromDictionary:updates];
        sessions[i] = entry;
        ISHLLMWriteSessionIndex(sessions, document[@"active"]);
        return;
    }
}

void ISHLLMSetActiveSessionID(NSString *sessionID) {
    NSDictionary<NSString *, id> *document = ISHLLMLoadSessionIndexDocument();
    ISHLLMWriteSessionIndex(document[@"sessions"], sessionID);
}

NSDictionary<NSString *, id> *ISHLLMCreateSession(NSString *title) {
    NSDictionary<NSString *, id> *document = ISHLLMLoadSessionIndexDocument();
    NSMutableArray<NSDictionary<NSString *, id> *> *sessions = [document[@"sessions"] mutableCopy];
    NSDictionary<NSString *, id> *entry = ISHLLMNewSessionEntry(title);
    [sessions addObject:entry];
    ISHLLMWriteSessionIndex(sessions, entry[@"id"]);
    return entry;
}

// A chat started by an agent (a sub-agent's): added to the index without
// becoming the selected chat, with `extra` merged into its entry.
NSDictionary<NSString *, id> *ISHLLMCreateBackgroundSession(NSString *title, NSDictionary<NSString *, id> *extra) {
    NSDictionary<NSString *, id> *document = ISHLLMLoadSessionIndexDocument();
    NSMutableArray<NSDictionary<NSString *, id> *> *sessions = [document[@"sessions"] mutableCopy];
    NSMutableDictionary<NSString *, id> *entry = [ISHLLMNewSessionEntry(title) mutableCopy];
    [entry addEntriesFromDictionary:extra ?: @{}];
    [sessions addObject:entry];
    ISHLLMWriteSessionIndex(sessions, ISHLLMStringValue(document, @"active"));
    return entry;
}

// Deleting the last chat leaves an empty one rather than no chat at all --
// the client always has somewhere to put the next message.
NSString *ISHLLMDeleteSession(NSString *sessionID) {
    NSDictionary<NSString *, id> *document = ISHLLMLoadSessionIndexDocument();
    NSMutableArray<NSDictionary<NSString *, id> *> *sessions = [document[@"sessions"] mutableCopy];
    NSUInteger removedIndex = NSNotFound;
    for (NSUInteger i = 0; i < sessions.count; i++) {
        if ([ISHLLMStringValue(sessions[i], @"id") isEqualToString:sessionID]) {
            removedIndex = i;
            break;
        }
    }
    if (removedIndex == NSNotFound)
        return ISHLLMStringValue(document, @"active");
    [sessions removeObjectAtIndex:removedIndex];
    if (ISHLLMSessionStorageAvailable())
        [NSFileManager.defaultManager removeItemAtURL:ISHLLMSessionFileURL(sessionID) error:nil];
    if (sessions.count == 0)
        [sessions addObject:ISHLLMNewSessionEntry(nil)];
    NSString *activeID = ISHLLMStringValue(document, @"active");
    if ([activeID isEqualToString:sessionID])
        activeID = ISHLLMStringValue(sessions[removedIndex < sessions.count ? removedIndex : sessions.count - 1], @"id");
    ISHLLMWriteSessionIndex(sessions, activeID);
    return activeID;
}

// MARK: The destination a request goes to
//
// Several chats can be mid-reply at once, each on its own destination, while
// the four global scalars describe only the one selected in Settings. A
// running agent therefore sets its destination on the thread for as long as
// it builds or sends a request (ISHLLMRunWithDestination), and every helper
// below reads through these four instead of UserPreferences.

static NSString *const kISHLLMThreadDestinationKey = @"ISHLLMThreadDestination";

NSDictionary<NSString *, NSString *> *ISHLLMThreadDestination(void) {
    return NSThread.currentThread.threadDictionary[kISHLLMThreadDestinationKey];
}

void ISHLLMRunWithDestination(NSDictionary<NSString *, NSString *> *destination, void (^block)(void)) {
    NSMutableDictionary *threadDictionary = NSThread.currentThread.threadDictionary;
    id previous = threadDictionary[kISHLLMThreadDestinationKey];
    if (destination != nil)
        threadDictionary[kISHLLMThreadDestinationKey] = destination;
    else
        [threadDictionary removeObjectForKey:kISHLLMThreadDestinationKey];
    block();
    if (previous != nil)
        threadDictionary[kISHLLMThreadDestinationKey] = previous;
    else
        [threadDictionary removeObjectForKey:kISHLLMThreadDestinationKey];
}

static NSString *ISHLLMScopedValue(NSString *key, NSString *global) {
    NSDictionary<NSString *, NSString *> *destination = ISHLLMThreadDestination();
    if (destination == nil)
        return global ?: @"";
    return ISHLLMStringValue(destination, key) ?: @"";
}

NSString *ISHLLMCurrentServerURL(void) { return ISHLLMScopedValue(kISHLLMDestinationURL, UserPreferences.shared.llmServerURL); }
NSString *ISHLLMCurrentModel(void) { return ISHLLMScopedValue(kISHLLMDestinationModel, UserPreferences.shared.llmModel); }
NSString *ISHLLMCurrentAPIKey(void) { return ISHLLMScopedValue(kISHLLMDestinationAPIKey, UserPreferences.shared.llmAPIKey); }
NSString *ISHLLMCurrentProvider(void) {
    NSString *provider = ISHLLMScopedValue(kISHLLMDestinationProvider, UserPreferences.shared.llmProvider);
    return provider.length > 0 ? provider : @"Custom";
}

NSString *ISHLLMChatEndpoint(void) {
    NSString *base = [ISHLLMCurrentServerURL() stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if (base.length == 0)
        base = @"http://localhost:11434/v1";
    while ([base hasSuffix:@"/"])
        base = [base substringToIndex:base.length - 1];
    if ([base hasSuffix:@"/chat/completions"])
        return base;
    return [base stringByAppendingString:@"/chat/completions"];
}

BOOL ISHLLMUsesAppleFoundationModels(void) {
    return [ISHLLMCurrentProvider().lowercaseString containsString:@"foundation models"];
}

AOKLLMBackend ISHLLMCurrentBackend(void) {
    return ISHLLMUsesAppleFoundationModels()
        ? AOKLLMBackendAppleFoundationModels
        : AOKLLMBackendOpenAICompatibleEndpoint;
}

// Status/explanation text for the Apple Foundation Models provider. When the
// Swift bridge is compiled in (libiSH_AOKApp-Swift.h present) this reflects
// the live SystemLanguageModel.default.availability on this device/OS; older
// builds without FoundationModels.framework linked fall back to a static
// explanation.
NSString *ISHLLMAppleFoundationModelsUnavailableMessage(void) {
#if __has_include("libiSH_AOKApp-Swift.h")
    return [AOKFoundationModelsBridge availabilityDescription];
#else
    return @"Apple Foundation Models is selected, but this build cannot call it yet. The current SDK does not expose FoundationModels.framework, and the app still targets older iOS/iPadOS. When built with the iOS 26 SDK, this provider should use Apple's on-device Foundation Models backend with no server URL or API key.";
#endif
}

BOOL ISHLLMFoundationModelsReady(void) {
#if __has_include("libiSH_AOKApp-Swift.h")
    return [AOKFoundationModelsBridge currentAvailability] == AOKFoundationModelAvailabilityAvailable;
#else
    return NO;
#endif
}

BOOL ISHLLMUsesGeminiAPI(void) {
    if (ISHLLMUsesAppleFoundationModels())
        return NO;
    NSString *provider = ISHLLMCurrentProvider().lowercaseString;
    NSString *host = [NSURL URLWithString:ISHLLMCurrentServerURL()].host.lowercaseString ?: @"";
    return [provider containsString:@"gemini"] || [host containsString:@"generativelanguage.googleapis.com"];
}

BOOL ISHLLMUsesAnthropicAPI(void) {
    if (ISHLLMUsesAppleFoundationModels())
        return NO;
    NSString *provider = ISHLLMCurrentProvider().lowercaseString;
    NSString *host = [NSURL URLWithString:ISHLLMCurrentServerURL()].host.lowercaseString ?: @"";
    return [provider containsString:@"anthropic"] || [host isEqualToString:@"api.anthropic.com"];
}

// The same headers as ISHLLMApplyAuthHeaders, as raw HTTP/1.1 lines, for
// the hand-rolled socket requests to plain-http servers.
static NSString *ISHLLMRawAuthHeaders(NSString *apiKey) {
    if (ISHLLMUsesAnthropicAPI())
        return [NSString stringWithFormat:@"%@anthropic-version: %@\r\n",
                apiKey.length > 0 ? [NSString stringWithFormat:@"x-api-key: %@\r\n", apiKey] : @"", kISHLLMAnthropicVersion];
    return apiKey.length > 0 ? [NSString stringWithFormat:@"Authorization: Bearer %@\r\n", apiKey] : @"";
}

NSString *ISHLLMAnthropicMessagesEndpoint(void) {
    NSString *base = [ISHLLMCurrentServerURL() stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if (base.length == 0)
        base = @"https://api.anthropic.com/v1";
    while ([base hasSuffix:@"/"])
        base = [base substringToIndex:base.length - 1];
    if ([base hasSuffix:@"/messages"])
        return base;
    return [base stringByAppendingString:@"/messages"];
}

// Every provider's authentication in one place: Anthropic takes x-api-key
// and a version header, Gemini its key in the URL, everything else a Bearer.
void ISHLLMApplyAuthHeaders(NSMutableURLRequest *request, NSString *apiKey) {
    if (ISHLLMUsesAnthropicAPI()) {
        if (apiKey.length > 0)
            [request setValue:apiKey forHTTPHeaderField:@"x-api-key"];
        [request setValue:kISHLLMAnthropicVersion forHTTPHeaderField:@"anthropic-version"];
        return;
    }
    if (apiKey.length > 0 && !ISHLLMUsesGeminiAPI())
        [request setValue:[@"Bearer " stringByAppendingString:apiKey] forHTTPHeaderField:@"Authorization"];
}

// A Messages API call. Its own session, because a long reply (or a model
// that thinks first) can go quiet for longer than the shared session's
// 60-second idle timeout. Blocks; call from a background queue.
NSData *ISHLLMAnthropicPost(NSDictionary *body, NSString *apiKey, NSInteger *statusCodeOut, NSError **errorOut) {
    static NSURLSession *session;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSURLSessionConfiguration *configuration = NSURLSessionConfiguration.defaultSessionConfiguration;
        configuration.timeoutIntervalForRequest = 600;
        configuration.timeoutIntervalForResource = 1800;
        session = [NSURLSession sessionWithConfiguration:configuration];
    });
    NSURL *url = [NSURL URLWithString:ISHLLMAnthropicMessagesEndpoint()];
    NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:url];
    request.HTTPMethod = @"POST";
    [request setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
    ISHLLMApplyAuthHeaders(request, apiKey);
    if (body[@"fallbacks"] != nil)
        [request setValue:@"server-side-fallback-2026-07-01" forHTTPHeaderField:@"anthropic-beta"];
    request.HTTPBody = [NSJSONSerialization dataWithJSONObject:body options:0 error:nil];
    __block NSData *resultData = nil;
    __block NSInteger statusCode = 0;
    __block NSError *resultError = nil;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    [[session dataTaskWithRequest:request completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
        resultData = data;
        resultError = error;
        if ([response isKindOfClass:NSHTTPURLResponse.class])
            statusCode = ((NSHTTPURLResponse *) response).statusCode;
        dispatch_semaphore_signal(done);
    }] resume];
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    if (statusCodeOut != NULL)
        *statusCodeOut = statusCode;
    if (errorOut != NULL)
        *errorOut = resultError;
    return resultData;
}

// A tiny request that proves the destination answers ("Test Connection",
// "/model"), in the shape the current API format takes.
NSURL *ISHLLMProbeURL(void) {
    if (ISHLLMUsesGeminiAPI())
        return [NSURL URLWithString:ISHLLMGeminiGenerateEndpoint()];
    if (ISHLLMUsesAnthropicAPI())
        return [NSURL URLWithString:ISHLLMAnthropicMessagesEndpoint()];
    return [NSURL URLWithString:ISHLLMChatEndpoint()];
}

NSDictionary *ISHLLMProbeBody(NSString *model, NSString *prompt, NSUInteger maxTokens) {
    if (ISHLLMUsesGeminiAPI())
        return @{@"contents": @[@{@"role": @"user", @"parts": @[@{@"text": prompt}]}]};
    if (ISHLLMUsesAnthropicAPI())
        return @{@"model": model, @"max_tokens": @(MAX(maxTokens, (NSUInteger) 16)), @"messages": @[@{@"role": @"user", @"content": prompt}]};
    return @{@"model": model, @"messages": @[@{@"role": @"user", @"content": prompt}], @"stream": @NO, @"max_tokens": @(maxTokens)};
}

NSString *ISHLLMGeminiGenerateEndpoint(void) {
    NSString *base = [ISHLLMCurrentServerURL() stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if (base.length == 0)
        base = @"https://generativelanguage.googleapis.com/v1beta";
    while ([base hasSuffix:@"/"])
        base = [base substringToIndex:base.length - 1];
    NSString *model = [ISHLLMCurrentModel() stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if (![model hasPrefix:@"models/"])
        model = [@"models/" stringByAppendingString:model];
    NSString *encodedModel = [model stringByAddingPercentEncodingWithAllowedCharacters:NSCharacterSet.URLPathAllowedCharacterSet] ?: model;
    NSString *endpoint = [base stringByAppendingFormat:@"/%@:generateContent", encodedModel];
    NSString *apiKey = [ISHLLMCurrentAPIKey() stringByAddingPercentEncodingWithAllowedCharacters:NSCharacterSet.URLQueryAllowedCharacterSet] ?: @"";
    return apiKey.length > 0 ? [endpoint stringByAppendingFormat:@"?key=%@", apiKey] : endpoint;
}

NSString *ISHLLMModelsEndpoint(void) {
    if (ISHLLMUsesGeminiAPI()) {
        NSString *base = [ISHLLMCurrentServerURL() stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        if (base.length == 0)
            base = @"https://generativelanguage.googleapis.com/v1beta";
        while ([base hasSuffix:@"/"])
            base = [base substringToIndex:base.length - 1];
        NSString *apiKey = [ISHLLMCurrentAPIKey() stringByAddingPercentEncodingWithAllowedCharacters:NSCharacterSet.URLQueryAllowedCharacterSet] ?: @"";
        NSString *endpoint = [base stringByAppendingString:@"/models"];
        return apiKey.length > 0 ? [endpoint stringByAppendingFormat:@"?key=%@", apiKey] : endpoint;
    }
    NSString *base = [ISHLLMCurrentServerURL() stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if (base.length == 0)
        base = @"https://openrouter.ai/api/v1";
    while ([base hasSuffix:@"/"])
        base = [base substringToIndex:base.length - 1];
    if ([base hasSuffix:@"/chat/completions"])
        base = [base substringToIndex:base.length - @"/chat/completions".length];
    if ([base hasSuffix:@"/models"])
        return base;
    return [base stringByAppendingString:@"/models"];
}

NSArray<NSDictionary<NSString *, NSString *> *> *ISHLLMProviderPresets(void) {
    return @[
        @{@"name": @"Apple Foundation Models", @"url": @"", @"model": @"system-language-model", @"format": @"Apple on-device Foundation Models"},
        @{@"name": @"OpenRouter Free", @"url": @"https://openrouter.ai/api/v1", @"model": @"openrouter/free", @"format": @"OpenAI-compatible chat completions"},
        @{@"name": @"Groq Llama", @"url": @"https://api.groq.com/openai/v1", @"model": @"llama-3.1-8b-instant", @"format": @"OpenAI-compatible chat completions"},
        @{@"name": @"Anthropic Claude", @"url": @"https://api.anthropic.com/v1", @"model": @"claude-opus-5", @"format": @"Anthropic Messages"},
        @{@"name": @"Gemini Flash", @"url": @"https://generativelanguage.googleapis.com/v1beta", @"model": @"gemini-2.5-flash", @"format": @"Google Gemini generateContent"},
        @{@"name": @"LM Studio", @"url": @"http://127.0.0.1:1234/v1", @"model": @"local-model", @"format": @"OpenAI-compatible chat completions"},
        @{@"name": @"Ollama", @"url": @"http://127.0.0.1:11434/v1", @"model": @"llama3.2", @"format": @"OpenAI-compatible chat completions"},
        @{@"name": @"OpenAI", @"url": @"https://api.openai.com/v1", @"model": @"gpt-4o-mini", @"format": @"OpenAI-compatible chat completions"},
        @{@"name": @"Custom", @"url": @"", @"model": @"", @"format": @"OpenAI-compatible chat completions"},
    ];
}

NSString *ISHLLMCurrentAPIFormat(void) {
    if (ISHLLMUsesAppleFoundationModels())
        return @"Apple on-device Foundation Models";
    if (ISHLLMUsesGeminiAPI())
        return @"Google Gemini generateContent";
    if (ISHLLMUsesAnthropicAPI())
        return @"Anthropic Messages";
    return @"OpenAI-compatible chat completions";
}

BOOL ISHLLMProviderRequiresAPIKey(void) {
    if (ISHLLMUsesAppleFoundationModels())
        return NO;
    NSString *provider = ISHLLMCurrentProvider().lowercaseString;
    NSString *host = [NSURL URLWithString:ISHLLMCurrentServerURL()].host.lowercaseString ?: @"";
    return [provider containsString:@"openrouter"] || [provider containsString:@"openai"] || ISHLLMUsesAnthropicAPI() ||
        [provider containsString:@"groq"] || [provider containsString:@"gemini"] ||
        [host containsString:@"openrouter.ai"] || [host containsString:@"api.openai.com"] ||
        [host containsString:@"api.groq.com"] || [host containsString:@"generativelanguage.googleapis.com"];
}

NSString *ISHLLMMissingAPIKeyMessage(void) {
    return @"This provider requires an API key. Add it in LLM Settings -> API Key. OpenRouter free, Groq, Gemini, and OpenAI all require API keys.";
}

NSArray<NSString *> *ISHLLMModelIdentifiersFromResponseData(NSData *data) {
    id json = data.length > 0 ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
    if (![json isKindOfClass:NSDictionary.class])
        return @[];
    NSArray *models = ISHLLMUsesGeminiAPI() ? json[@"models"] : json[@"data"];
    if (![models isKindOfClass:NSArray.class])
        return @[];
    NSMutableArray<NSString *> *ids = [NSMutableArray array];
    for (id model in models) {
        if (![model isKindOfClass:NSDictionary.class])
            continue;
        if (ISHLLMUsesGeminiAPI()) {
            NSArray *methods = model[@"supportedGenerationMethods"];
            if ([methods isKindOfClass:NSArray.class] && ![methods containsObject:@"generateContent"])
                continue;
            NSString *name = [model[@"name"] isKindOfClass:NSString.class] ? model[@"name"] : nil;
            if ([name hasPrefix:@"models/"])
                name = [name substringFromIndex:@"models/".length];
            if (name.length > 0)
                [ids addObject:name];
        } else if ([model[@"id"] isKindOfClass:NSString.class]) {
            [ids addObject:model[@"id"]];
        }
    }
    return [ids sortedArrayUsingSelector:@selector(localizedCaseInsensitiveCompare:)];
}

// Best-effort context-window (max input tokens) lookup for one model entry in
// a /models (or Gemini ListModels) response. There's no standard field for
// this -- OpenRouter uses "context_length" (sometimes only nested under
// "top_provider"), Groq uses "context_window", vLLM's OpenAI-compatible
// server adds "max_model_len", Gemini's ListModels uses "inputTokenLimit".
// Plain OpenAI/Ollama don't expose it at all. Returns 0 if the model or a
// usable field can't be found -- callers must treat that as "unknown", not 0.
static NSString *const kISHLLMContextWindowKey = @"LLM Context Window Tokens";

NSInteger ISHLLMContextWindowSetting(void) {
    return MAX(0, [NSUserDefaults.standardUserDefaults integerForKey:kISHLLMContextWindowKey]);
}

void ISHLLMSetContextWindowSetting(NSInteger tokens) {
    if (tokens > 0)
        [NSUserDefaults.standardUserDefaults setInteger:tokens forKey:kISHLLMContextWindowKey];
    else
        [NSUserDefaults.standardUserDefaults removeObjectForKey:kISHLLMContextWindowKey];
}

NSInteger ISHLLMContextWindowFromModelsResponse(NSData *data, NSString *modelID) {
    if (modelID.length == 0)
        return 0;
    id json = data.length > 0 ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
    if (![json isKindOfClass:NSDictionary.class])
        return 0;
    NSArray *models = ISHLLMUsesGeminiAPI() ? json[@"models"] : json[@"data"];
    if (![models isKindOfClass:NSArray.class])
        return 0;
    for (id entry in models) {
        if (![entry isKindOfClass:NSDictionary.class])
            continue;
        NSDictionary *model = entry;
        NSString *entryID;
        if (ISHLLMUsesGeminiAPI()) {
            entryID = [model[@"name"] isKindOfClass:NSString.class] ? model[@"name"] : nil;
            if ([entryID hasPrefix:@"models/"])
                entryID = [entryID substringFromIndex:@"models/".length];
        } else {
            entryID = [model[@"id"] isKindOfClass:NSString.class] ? model[@"id"] : nil;
        }
        if (entryID.length == 0 || ![entryID isEqualToString:modelID])
            continue;
        for (NSString *key in @[@"context_length", @"context_window", @"max_model_len", @"max_context_length", @"n_ctx", @"max_input_tokens"]) {
            id value = model[key];
            if ([value isKindOfClass:NSNumber.class] && [value integerValue] > 0)
                return [value integerValue];
        }
        NSDictionary *topProvider = [model[@"top_provider"] isKindOfClass:NSDictionary.class] ? model[@"top_provider"] : nil;
        id nestedLength = topProvider[@"context_length"];
        if ([nestedLength isKindOfClass:NSNumber.class] && [nestedLength integerValue] > 0)
            return [nestedLength integerValue];
        if (ISHLLMUsesGeminiAPI()) {
            id inputLimit = model[@"inputTokenLimit"];
            if ([inputLimit isKindOfClass:NSNumber.class] && [inputLimit integerValue] > 0)
                return [inputLimit integerValue];
        }
        return 0; // matched the model but no known context-size field
    }
    return 0;
}

NSString *ISHLLMSanitizedAssistantContent(NSString *content) {
    NSRange fileSeparator = [content rangeOfString:@"<file_sep>"];
    if (fileSeparator.location != NSNotFound)
        content = [content substringToIndex:fileSeparator.location];
    return [content stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
}

// Streaming-safe accumulator. Unlike ISHLLMSanitizedAssistantContent, this must
// NOT trim trailing whitespace: it runs on the whole running message after every
// streamed delta, so a token-boundary space that lands at the tail of the buffer
// between two deltas (e.g. delta "Hello! " followed by "How") would be stripped,
// gluing the next word on ("Hello!How", "helpyou"). We only cut at the <file_sep>
// stop marker and trim *leading* whitespace, which is idempotent once real text
// has arrived and never touches interior or trailing spaces. Call
// ISHLLMSanitizedAssistantContent once at end-of-stream for the final cleanup.
NSString *ISHLLMStreamingAssistantContent(NSString *content) {
    NSRange fileSeparator = [content rangeOfString:@"<file_sep>"];
    if (fileSeparator.location != NSNotFound)
        content = [content substringToIndex:fileSeparator.location];
    NSCharacterSet *whitespace = NSCharacterSet.whitespaceAndNewlineCharacterSet;
    NSUInteger start = 0;
    while (start < content.length && [whitespace characterIsMember:[content characterAtIndex:start]])
        start++;
    return start > 0 ? [content substringFromIndex:start] : content;
}

BOOL ISHLLMHideThinkingEnabled(void) {
    return UserPreferences.shared.llmHideThinking;
}

// Ranges of `content` that are literal code -- fenced blocks and inline
// backtick spans. Tags inside them are being *talked about* (ask a model how
// this very feature works and it will happily print a <think> tag in an
// example), so they must not be mistaken for the model thinking. An unclosed
// fence runs to the end of the text, which is also right mid-stream.
NSArray<NSValue *> *ISHLLMCodeSpanRanges(NSString *content) {
    static NSRegularExpression *fenceMarker;
    static NSRegularExpression *inlineSpan;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        fenceMarker = [NSRegularExpression regularExpressionWithPattern:@"^[ \\t]*```" options:NSRegularExpressionAnchorsMatchLines error:NULL];
        inlineSpan = [NSRegularExpression regularExpressionWithPattern:@"`[^`\\n]*`" options:0 error:NULL];
    });
    NSMutableArray<NSValue *> *ranges = [NSMutableArray array];
    if (fenceMarker == nil || inlineSpan == nil)
        return ranges;
    NSRange whole = NSMakeRange(0, content.length);
    NSArray<NSTextCheckingResult *> *fences = [fenceMarker matchesInString:content options:0 range:whole];
    for (NSUInteger i = 0; i < fences.count; i += 2) {
        NSUInteger start = fences[i].range.location;
        NSUInteger end = i + 1 < fences.count ? NSMaxRange(fences[i + 1].range) : content.length;
        [ranges addObject:[NSValue valueWithRange:NSMakeRange(start, end - start)]];
    }
    NSUInteger fencedCount = ranges.count;
    for (NSTextCheckingResult *match in [inlineSpan matchesInString:content options:0 range:whole]) {
        BOOL insideFence = NO;
        for (NSUInteger i = 0; i < fencedCount && !insideFence; i++)
            insideFence = NSLocationInRange(match.range.location, ranges[i].rangeValue);
        if (!insideFence)
            [ranges addObject:[NSValue valueWithRange:match.range]];
    }
    return ranges;
}

NSTextCheckingResult *ISHLLMFirstTagOutsideCode(NSRegularExpression *regex,
                                                       NSString *content,
                                                       NSRange range,
                                                       NSArray<NSValue *> *codeRanges) {
    __block NSTextCheckingResult *found = nil;
    [regex enumerateMatchesInString:content options:0 range:range usingBlock:^(NSTextCheckingResult *match, __unused NSMatchingFlags flags, BOOL *stop) {
        if (match == nil)
            return;
        for (NSValue *value in codeRanges) {
            NSRange code = value.rangeValue;
            if (match.range.location >= code.location && NSMaxRange(match.range) <= NSMaxRange(code))
                return;
        }
        found = match;
        *stop = YES;
    }];
    return found;
}

// Splits an assistant message into the part meant for the reader and the
// model's chain-of-thought. Reasoning models (DeepSeek-R1, Qwen3, gpt-oss,
// magistral, ...) emit the latter inline, wrapped in <think>...</think>; some
// spell the tag <thinking>, <thought> or <reasoning>. Returns the visible text
// with the blocks removed; each thought is appended to `thoughts` in order, and
// `*openOut` is YES when the text ends inside an unterminated block -- i.e. the
// model is thinking right now, mid-stream.
//
// Two shapes matter beyond the obvious one:
//   - Only a closing tag. Several servers apply a chat template that already
//     opens <think> for the model, so the reply starts mid-thought and the
//     first tag seen is </think>. Treated as an implicit open, but only while
//     nothing visible has been emitted yet, so a stray </think> further down a
//     normal answer stays literal text rather than eating the whole reply.
//   - No trailing trim. This runs on the accumulating buffer after every
//     streamed delta, so trimming the tail would eat token-boundary spaces
//     (see ISHLLMStreamingAssistantContent); only the leading edge is trimmed.
NSString *ISHLLMSplitThinkingFromContent(NSString *content,
                                                NSMutableArray<NSString *> *thoughts,
                                                BOOL *openOut) {
    if (openOut != NULL)
        *openOut = NO;
    if (content.length == 0)
        return content ?: @"";
    static NSRegularExpression *openTag;
    static NSRegularExpression *closeTag;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        NSRegularExpressionOptions options = NSRegularExpressionCaseInsensitive;
        openTag = [NSRegularExpression regularExpressionWithPattern:@"<(think|thinking|thought|reasoning)>" options:options error:NULL];
        closeTag = [NSRegularExpression regularExpressionWithPattern:@"</(think|thinking|thought|reasoning)>" options:options error:NULL];
    });
    if (openTag == nil || closeTag == nil)
        return content;

    NSArray<NSValue *> *codeRanges = ISHLLMCodeSpanRanges(content);
    NSMutableString *visible = [NSMutableString string];
    NSUInteger cursor = 0;
    while (cursor < content.length) {
        NSRange remaining = NSMakeRange(cursor, content.length - cursor);
        NSTextCheckingResult *open = ISHLLMFirstTagOutsideCode(openTag, content, remaining, codeRanges);
        NSTextCheckingResult *close = ISHLLMFirstTagOutsideCode(closeTag, content, remaining, codeRanges);
        if (open == nil && close == nil) {
            [visible appendString:[content substringFromIndex:cursor]];
            break;
        }
        // Implicit open (see above): a close tag ahead of any open tag, with
        // nothing shown yet, means the whole prefix was the model thinking.
        if (open == nil || (close != nil && close.range.location < open.range.location)) {
            if (visible.length > 0) {
                // Mid-answer stray close tag: leave it alone as literal text.
                [visible appendString:[content substringWithRange:NSMakeRange(cursor, NSMaxRange(close.range) - cursor)]];
                cursor = NSMaxRange(close.range);
                continue;
            }
            NSString *thought = [content substringWithRange:NSMakeRange(cursor, close.range.location - cursor)];
            thought = [thought stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
            if (thought.length > 0)
                [thoughts addObject:thought];
            cursor = NSMaxRange(close.range);
            continue;
        }
        [visible appendString:[content substringWithRange:NSMakeRange(cursor, open.range.location - cursor)]];
        NSUInteger bodyStart = NSMaxRange(open.range);
        NSRange afterOpen = NSMakeRange(bodyStart, content.length - bodyStart);
        NSTextCheckingResult *end = ISHLLMFirstTagOutsideCode(closeTag, content, afterOpen, codeRanges);
        NSString *thought = end != nil
            ? [content substringWithRange:NSMakeRange(bodyStart, end.range.location - bodyStart)]
            : [content substringFromIndex:bodyStart];
        thought = [thought stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        if (thought.length > 0)
            [thoughts addObject:thought];
        if (end == nil) {
            if (openOut != NULL)
                *openOut = YES;
            break;
        }
        cursor = NSMaxRange(end.range);
    }

    // Leading-only trim: idempotent once real text arrives (the blank line a
    // model leaves after </think> would otherwise open every reply).
    NSCharacterSet *whitespace = NSCharacterSet.whitespaceAndNewlineCharacterSet;
    NSUInteger start = 0;
    while (start < visible.length && [whitespace characterIsMember:[visible characterAtIndex:start]])
        start++;
    return start > 0 ? [visible substringFromIndex:start] : visible;
}

NSData *ISHLLMDirectHTTPPost(NSURL *url, NSData *body, NSString *apiKey, NSInteger *statusCodeOut, NSError **errorOut) {
    NSString *host = url.host;
    if (host.length == 0) {
        if (errorOut != nil)
            *errorOut = [NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorBadURL userInfo:nil];
        return nil;
    }

    NSString *portString = url.port != nil ? url.port.stringValue : @"80";
    struct addrinfo hints = {0};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    struct addrinfo *results = NULL;
    int gai = getaddrinfo(host.UTF8String, portString.UTF8String, &hints, &results);
    if (gai != 0) {
        if (errorOut != nil) {
            NSString *message = [NSString stringWithUTF8String:gai_strerror(gai)] ?: @"Name lookup failed";
            *errorOut = [NSError errorWithDomain:NSPOSIXErrorDomain code:gai userInfo:@{NSLocalizedDescriptionKey: message}];
        }
        return nil;
    }

    int connectErrno = 0;
    int fd = ISHLLMConnectWithTimeout(results, 15000, &connectErrno);
    freeaddrinfo(results);
    if (fd < 0) {
        if (errorOut != nil)
            *errorOut = ISHLLMConnectionError(host, portString, connectErrno);
        return nil;
    }

    NSString *path = url.path.length > 0 ? url.path : @"/";
    if (url.query.length > 0)
        path = [path stringByAppendingFormat:@"?%@", url.query];
    NSString *hostHeader = host;
    if (url.port != nil)
        hostHeader = [hostHeader stringByAppendingFormat:@":%@", url.port];
    NSMutableString *headers = [NSMutableString stringWithFormat:
        @"POST %@ HTTP/1.1\r\n"
        @"Host: %@\r\n"
        @"Content-Type: application/json\r\n"
        @"Content-Length: %lu\r\n"
        @"Connection: close\r\n",
        path, hostHeader, (unsigned long) body.length];
    [headers appendString:ISHLLMRawAuthHeaders(apiKey)];
    [headers appendString:@"\r\n"];

    NSMutableData *requestData = [NSMutableData dataWithData:[headers dataUsingEncoding:NSUTF8StringEncoding]];
    [requestData appendData:body];
    const uint8_t *bytes = requestData.bytes;
    size_t remaining = requestData.length;
    while (remaining > 0) {
        ssize_t sent = send(fd, bytes, remaining, 0);
        if (sent <= 0) {
            int savedErrno = errno;
            close(fd);
            if (errorOut != nil)
                *errorOut = [NSError errorWithDomain:NSPOSIXErrorDomain code:savedErrno userInfo:nil];
            return nil;
        }
        bytes += sent;
        remaining -= (size_t) sent;
    }

    NSMutableData *responseData = [NSMutableData data];
    uint8_t buffer[8192];
    for (;;) {
        ssize_t nread = recv(fd, buffer, sizeof(buffer), 0);
        if (nread < 0) {
            int savedErrno = errno;
            close(fd);
            if (errorOut != nil)
                *errorOut = [NSError errorWithDomain:NSPOSIXErrorDomain code:savedErrno userInfo:nil];
            return nil;
        }
        if (nread == 0)
            break;
        [responseData appendBytes:buffer length:(NSUInteger) nread];
    }
    close(fd);

    NSData *separator = [@"\r\n\r\n" dataUsingEncoding:NSUTF8StringEncoding];
    NSRange separatorRange = [responseData rangeOfData:separator options:0 range:NSMakeRange(0, responseData.length)];
    if (separatorRange.location == NSNotFound) {
        if (errorOut != nil)
            *errorOut = [NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorCannotParseResponse userInfo:nil];
        return nil;
    }

    NSData *headerData = [responseData subdataWithRange:NSMakeRange(0, separatorRange.location)];
    NSString *headerText = [[NSString alloc] initWithData:headerData encoding:NSISOLatin1StringEncoding] ?: @"";
    NSArray<NSString *> *headerLines = [headerText componentsSeparatedByString:@"\r\n"];
    NSArray<NSString *> *statusParts = [headerLines.firstObject componentsSeparatedByString:@" "];
    if (statusParts.count >= 2 && statusCodeOut != NULL)
        *statusCodeOut = statusParts[1].integerValue;

    NSUInteger bodyOffset = NSMaxRange(separatorRange);
    return [responseData subdataWithRange:NSMakeRange(bodyOffset, responseData.length - bodyOffset)];
}

NSData *ISHLLMDirectHTTPGet(NSURL *url, NSString *apiKey, NSInteger *statusCodeOut, NSError **errorOut) {
    NSString *host = url.host;
    if (host.length == 0) {
        if (errorOut != nil)
            *errorOut = [NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorBadURL userInfo:nil];
        return nil;
    }
    NSString *portString = url.port != nil ? url.port.stringValue : @"80";
    struct addrinfo hints = {0};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    struct addrinfo *results = NULL;
    int gai = getaddrinfo(host.UTF8String, portString.UTF8String, &hints, &results);
    if (gai != 0) {
        if (errorOut != nil) {
            NSString *message = [NSString stringWithUTF8String:gai_strerror(gai)] ?: @"Name lookup failed";
            *errorOut = [NSError errorWithDomain:NSPOSIXErrorDomain code:gai userInfo:@{NSLocalizedDescriptionKey: message}];
        }
        return nil;
    }
    int connectErrno = 0;
    int fd = ISHLLMConnectWithTimeout(results, 15000, &connectErrno);
    freeaddrinfo(results);
    if (fd < 0) {
        if (errorOut != nil)
            *errorOut = ISHLLMConnectionError(host, portString, connectErrno);
        return nil;
    }
    NSString *path = url.path.length > 0 ? url.path : @"/";
    if (url.query.length > 0)
        path = [path stringByAppendingFormat:@"?%@", url.query];
    NSString *hostHeader = host;
    if (url.port != nil)
        hostHeader = [hostHeader stringByAppendingFormat:@":%@", url.port];
    NSMutableString *requestText = [NSMutableString stringWithFormat:
        @"GET %@ HTTP/1.1\r\n"
        @"Host: %@\r\n"
        @"Accept: application/json\r\n"
        @"Connection: close\r\n",
        path, hostHeader];
    [requestText appendString:ISHLLMRawAuthHeaders(apiKey)];
    [requestText appendString:@"\r\n"];
    NSData *requestData = [requestText dataUsingEncoding:NSUTF8StringEncoding];
    const uint8_t *bytes = requestData.bytes;
    size_t remaining = requestData.length;
    while (remaining > 0) {
        ssize_t sent = send(fd, bytes, remaining, 0);
        if (sent <= 0) {
            int savedErrno = errno;
            close(fd);
            if (errorOut != nil)
                *errorOut = [NSError errorWithDomain:NSPOSIXErrorDomain code:savedErrno userInfo:nil];
            return nil;
        }
        bytes += sent;
        remaining -= (size_t) sent;
    }
    NSMutableData *responseData = [NSMutableData data];
    uint8_t buffer[8192];
    for (;;) {
        ssize_t nread = recv(fd, buffer, sizeof(buffer), 0);
        if (nread < 0) {
            int savedErrno = errno;
            close(fd);
            if (errorOut != nil)
                *errorOut = [NSError errorWithDomain:NSPOSIXErrorDomain code:savedErrno userInfo:nil];
            return nil;
        }
        if (nread == 0)
            break;
        [responseData appendBytes:buffer length:(NSUInteger) nread];
    }
    close(fd);
    NSData *separator = [@"\r\n\r\n" dataUsingEncoding:NSUTF8StringEncoding];
    NSRange separatorRange = [responseData rangeOfData:separator options:0 range:NSMakeRange(0, responseData.length)];
    if (separatorRange.location == NSNotFound)
        return responseData;
    NSData *headerData = [responseData subdataWithRange:NSMakeRange(0, separatorRange.location)];
    NSString *headerText = [[NSString alloc] initWithData:headerData encoding:NSISOLatin1StringEncoding] ?: @"";
    NSArray<NSString *> *statusParts = [[headerText componentsSeparatedByString:@"\r\n"].firstObject componentsSeparatedByString:@" "];
    if (statusParts.count >= 2 && statusCodeOut != NULL)
        *statusCodeOut = statusParts[1].integerValue;
    NSUInteger bodyOffset = NSMaxRange(separatorRange);
    return [responseData subdataWithRange:NSMakeRange(bodyOffset, responseData.length - bodyOffset)];
}

// Fetch ISHLLMModelsEndpoint() and hand the raw response to `completion` on an
// unspecified background thread (callers hop to main themselves) -- used for
// the silent context-window probe below. http endpoints use the raw-socket
// path (ATS blocks plaintext http via NSURLSession); https uses NSURLSession.
void ISHLLMFetchModelsDataAsync(void (^completion)(NSData *data, NSInteger statusCode, NSError *error)) {
    NSURL *url = [NSURL URLWithString:ISHLLMModelsEndpoint()];
    if (url == nil) {
        completion(nil, 0, [NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorBadURL userInfo:nil]);
        return;
    }
    NSString *apiKey = ISHLLMCurrentAPIKey();
    NSDictionary<NSString *, NSString *> *destination = ISHLLMThreadDestination();
    if ([url.scheme.lowercaseString isEqualToString:@"http"]) {
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
            __block NSInteger statusCode = 0;
            __block NSError *error = nil;
            __block NSData *data = nil;
            ISHLLMRunWithDestination(destination, ^{
                data = ISHLLMDirectHTTPGet(url, apiKey, &statusCode, &error);
            });
            completion(data, statusCode, error);
        });
        return;
    }
    NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:url];
    ISHLLMApplyAuthHeaders(request, apiKey);
    NSURLSessionDataTask *task = [NSURLSession.sharedSession dataTaskWithRequest:request completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
        NSHTTPURLResponse *http = [response isKindOfClass:NSHTTPURLResponse.class] ? (NSHTTPURLResponse *) response : nil;
        completion(data, http.statusCode, error);
    }];
    [task resume];
}

NSString *ISHLLMContentFromStreamingPayload(NSString *payload) {
    NSData *data = [payload dataUsingEncoding:NSUTF8StringEncoding];
    if (data.length == 0)
        return nil;
    id json = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
    if (![json isKindOfClass:NSDictionary.class])
        return nil;
    NSArray *choices = json[@"choices"];
    NSDictionary *choice = choices.count > 0 && [choices[0] isKindOfClass:NSDictionary.class] ? choices[0] : nil;
    NSDictionary *delta = [choice[@"delta"] isKindOfClass:NSDictionary.class] ? choice[@"delta"] : nil;
    NSString *content = [delta[@"content"] isKindOfClass:NSString.class] ? delta[@"content"] : nil;
    if (content.length == 0) {
        NSDictionary *message = [choice[@"message"] isKindOfClass:NSDictionary.class] ? choice[@"message"] : nil;
        content = [message[@"content"] isKindOfClass:NSString.class] ? message[@"content"] : nil;
    }
    return content;
}

// fdOut, if non-NULL, receives the connected socket fd for as long as this
// call is blocked in send()/recv() -- so a caller on another thread can
// shutdown() it to unblock a cancelled request. Always left at 0 on return.
BOOL ISHLLMDirectHTTPPostStreamingPayloads(NSURL *url, NSData *body, NSString *apiKey,
                                           NSDictionary<NSString *, NSString *> *extraHeaders,
                                           void (^payloadHandler)(NSString *payload),
                                           int *fdOut, NSInteger *statusCodeOut,
                                           NSMutableData *plainBody, NSError **errorOut);
BOOL ISHLLMDirectHTTPPostStreaming(NSURL *url,
                                          NSData *body,
                                          NSString *apiKey,
                                          void (^chunkHandler)(NSString *chunk),
                                          int *fdOut,
                                          NSInteger *statusCodeOut,
                                          NSError **errorOut) {
    return ISHLLMDirectHTTPPostStreamingPayloads(url, body, apiKey, nil, ^(NSString *payload) {
        NSString *content = ISHLLMContentFromStreamingPayload(payload);
        if (content.length > 0 && chunkHandler != nil)
            chunkHandler(content);
    }, fdOut, statusCodeOut, nil, errorOut);
}

// The streaming POST underneath: every SSE `data:` payload goes to
// payloadHandler as it arrives ("[DONE]" is dropped), extraHeaders are sent
// as they are, and plainBody (optional) collects the whole response body, for
// a server that answered with plain JSON instead of an event stream.
BOOL ISHLLMDirectHTTPPostStreamingPayloads(NSURL *url, NSData *body, NSString *apiKey,
                                           NSDictionary<NSString *, NSString *> *extraHeaders,
                                           void (^payloadHandler)(NSString *payload),
                                           int *fdOut, NSInteger *statusCodeOut,
                                           NSMutableData *plainBody, NSError **errorOut) {
    NSString *host = url.host;
    if (host.length == 0) {
        if (errorOut != nil)
            *errorOut = [NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorBadURL userInfo:nil];
        return NO;
    }

    NSString *portString = url.port != nil ? url.port.stringValue : @"80";
    struct addrinfo hints = {0};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    struct addrinfo *results = NULL;
    int gai = getaddrinfo(host.UTF8String, portString.UTF8String, &hints, &results);
    if (gai != 0) {
        if (errorOut != nil) {
            NSString *message = [NSString stringWithUTF8String:gai_strerror(gai)] ?: @"Name lookup failed";
            *errorOut = [NSError errorWithDomain:NSPOSIXErrorDomain code:gai userInfo:@{NSLocalizedDescriptionKey: message}];
        }
        return NO;
    }

    int connectErrno = 0;
    int fd = ISHLLMConnectWithTimeout(results, 15000, &connectErrno);
    freeaddrinfo(results);
    if (fd < 0) {
        if (errorOut != nil)
            *errorOut = ISHLLMConnectionError(host, portString, connectErrno);
        return NO;
    }
    if (fdOut != NULL)
        *fdOut = fd;

    NSString *path = url.path.length > 0 ? url.path : @"/";
    if (url.query.length > 0)
        path = [path stringByAppendingFormat:@"?%@", url.query];
    NSString *hostHeader = host;
    if (url.port != nil)
        hostHeader = [hostHeader stringByAppendingFormat:@":%@", url.port];
    NSMutableString *headers = [NSMutableString stringWithFormat:
        @"POST %@ HTTP/1.0\r\n"
        @"Host: %@\r\n"
        @"Content-Type: application/json\r\n"
        @"Accept: text/event-stream\r\n"
        @"Content-Length: %lu\r\n",
        path, hostHeader, (unsigned long) body.length];
    [headers appendString:ISHLLMRawAuthHeaders(apiKey)];
    for (NSString *name in extraHeaders)
        [headers appendFormat:@"%@: %@\r\n", name, extraHeaders[name]];
    [headers appendString:@"\r\n"];

    NSMutableData *requestData = [NSMutableData dataWithData:[headers dataUsingEncoding:NSUTF8StringEncoding]];
    [requestData appendData:body];
    const uint8_t *bytes = requestData.bytes;
    size_t remaining = requestData.length;
    while (remaining > 0) {
        ssize_t sent = send(fd, bytes, remaining, 0);
        if (sent <= 0) {
            int savedErrno = errno;
            if (fdOut != NULL) *fdOut = 0;
            close(fd);
            if (errorOut != nil)
                *errorOut = [NSError errorWithDomain:NSPOSIXErrorDomain code:savedErrno userInfo:nil];
            return NO;
        }
        bytes += sent;
        remaining -= (size_t) sent;
    }

    NSMutableData *bufferedData = [NSMutableData data];
    NSMutableString *eventBuffer = [NSMutableString string];
    BOOL parsedHeaders = NO;
    uint8_t buffer[4096];
    for (;;) {
        ssize_t nread = recv(fd, buffer, sizeof(buffer), 0);
        if (nread < 0) {
            int savedErrno = errno;
            if (fdOut != NULL) *fdOut = 0;
            close(fd);
            if (errorOut != nil)
                *errorOut = [NSError errorWithDomain:NSPOSIXErrorDomain code:savedErrno userInfo:nil];
            return NO;
        }
        if (nread == 0)
            break;
        [bufferedData appendBytes:buffer length:(NSUInteger) nread];

        if (!parsedHeaders) {
            NSData *separator = [@"\r\n\r\n" dataUsingEncoding:NSUTF8StringEncoding];
            NSRange separatorRange = [bufferedData rangeOfData:separator options:0 range:NSMakeRange(0, bufferedData.length)];
            if (separatorRange.location == NSNotFound)
                continue;
            NSData *headerData = [bufferedData subdataWithRange:NSMakeRange(0, separatorRange.location)];
            NSString *headerText = [[NSString alloc] initWithData:headerData encoding:NSISOLatin1StringEncoding] ?: @"";
            NSArray<NSString *> *statusParts = [[headerText componentsSeparatedByString:@"\r\n"].firstObject componentsSeparatedByString:@" "];
            if (statusParts.count >= 2 && statusCodeOut != NULL)
                *statusCodeOut = statusParts[1].integerValue;
            NSUInteger bodyOffset = NSMaxRange(separatorRange);
            NSData *remainingBody = [bufferedData subdataWithRange:NSMakeRange(bodyOffset, bufferedData.length - bodyOffset)];
            [bufferedData setData:remainingBody];
            parsedHeaders = YES;
        }

        NSString *text = [[NSString alloc] initWithData:bufferedData encoding:NSUTF8StringEncoding];
        if (text.length == 0)
            continue;
        [plainBody appendData:bufferedData];
        [bufferedData setLength:0];
        [eventBuffer appendString:text];
        for (;;) {
            NSRange newlineRange = [eventBuffer rangeOfString:@"\n"];
            if (newlineRange.location == NSNotFound)
                break;
            NSString *line = [[eventBuffer substringToIndex:newlineRange.location] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
            [eventBuffer deleteCharactersInRange:NSMakeRange(0, NSMaxRange(newlineRange))];
            if (![line hasPrefix:@"data:"])
                continue;
            NSString *payload = [[line substringFromIndex:5] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
            if ([payload isEqualToString:@"[DONE]"])
                continue;
            if (payloadHandler != nil)
                payloadHandler(payload);
        }
    }
    if (fdOut != NULL) *fdOut = 0;
    close(fd);
    return YES;
}

// MARK: - Streaming over https (NSURLSession)


@implementation ISHLLMStreamingResponseDelegate {
    NSMutableData *_pending;      // bytes not yet split into complete SSE lines
    NSMutableData *_body;         // whole response, kept only until streaming is confirmed
    NSInteger _statusCode;
    BOOL _receivedChunks;
}

- (instancetype)init {
    self = [super init];
    if (self != nil) {
        _pending = [NSMutableData data];
        _body = [NSMutableData data];
    }
    return self;
}

- (void)URLSession:(NSURLSession *)session
          dataTask:(NSURLSessionDataTask *)dataTask
didReceiveResponse:(NSURLResponse *)response
 completionHandler:(void (^)(NSURLSessionResponseDisposition))completionHandler {
    (void) session;
    (void) dataTask;
    if ([response isKindOfClass:NSHTTPURLResponse.class])
        _statusCode = ((NSHTTPURLResponse *) response).statusCode;
    completionHandler(NSURLSessionResponseAllow);
}

- (void)URLSession:(NSURLSession *)session dataTask:(NSURLSessionDataTask *)dataTask didReceiveData:(NSData *)data {
    (void) session;
    (void) dataTask;
    [_body appendData:data];
    // Only a 200 carries an event stream; an error body is JSON to be parsed
    // whole by the caller, so don't try to read deltas out of it.
    if (_statusCode != 200)
        return;
    [_pending appendData:data];
    [self drainCompleteLines];
}

// SSE frames are line-oriented and a chunk can split mid-line, so only whole
// lines are consumed and the remainder stays buffered for the next callback.
- (void)drainCompleteLines {
    while (YES) {
        const char newline = '\n';
        NSRange lineBreak = [_pending rangeOfData:[NSData dataWithBytes:&newline length:1]
                                          options:0
                                            range:NSMakeRange(0, _pending.length)];
        if (lineBreak.location == NSNotFound)
            return;
        NSData *lineData = [_pending subdataWithRange:NSMakeRange(0, lineBreak.location)];
        [_pending replaceBytesInRange:NSMakeRange(0, NSMaxRange(lineBreak)) withBytes:NULL length:0];
        NSString *line = [[NSString alloc] initWithData:lineData encoding:NSUTF8StringEncoding];
        line = [line stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        if (![line hasPrefix:@"data:"])
            continue; // comments, event: lines and blank separators
        NSString *payload = [[line substringFromIndex:5] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        if ([payload isEqualToString:@"[DONE]"])
            continue; // end-of-stream marker, not a delta
        NSString *content = ISHLLMContentFromStreamingPayload(payload);
        if (content.length == 0)
            continue;
        if (!_receivedChunks) {
            _receivedChunks = YES;
            _body = nil; // streaming confirmed; stop keeping a second copy of the text
        }
        if (self.chunkHandler != nil)
            self.chunkHandler(content);
    }
}

- (void)URLSession:(NSURLSession *)session task:(NSURLSessionTask *)task didCompleteWithError:(NSError *)error {
    (void) task;
    if (self.completionHandler != nil)
        self.completionHandler(_receivedChunks, _body, _statusCode, error);
    self.chunkHandler = nil;
    self.completionHandler = nil;
    // NSURLSession holds its delegate strongly until it is invalidated, so a
    // session left alive here would leak this object and, through the
    // handlers, the view controller.
    [session finishTasksAndInvalidate];
}

@end

// The payload-level twin of ISHLLMStreamingResponseDelegate, for the tool
// loop: it hands on every SSE `data:` payload rather than extracting chat
// completions text, and always keeps the whole body so a server that
// answered with plain JSON can still be read.
@implementation ISHLLMRawStreamDelegate {
    NSMutableData *_pending;
    NSMutableData *_body;
    NSInteger _statusCode;
}

- (instancetype)init {
    if ((self = [super init])) {
        _pending = [NSMutableData data];
        _body = [NSMutableData data];
    }
    return self;
}

- (void)URLSession:(NSURLSession *)session dataTask:(NSURLSessionDataTask *)dataTask
didReceiveResponse:(NSURLResponse *)response completionHandler:(void (^)(NSURLSessionResponseDisposition))completionHandler {
    (void) session;
    (void) dataTask;
    if ([response isKindOfClass:NSHTTPURLResponse.class])
        _statusCode = ((NSHTTPURLResponse *) response).statusCode;
    completionHandler(NSURLSessionResponseAllow);
}

- (void)URLSession:(NSURLSession *)session dataTask:(NSURLSessionDataTask *)dataTask didReceiveData:(NSData *)data {
    (void) session;
    (void) dataTask;
    [_body appendData:data];
    if (_statusCode != 200)
        return;
    [_pending appendData:data];
    while (YES) {
        const char newline = '\n';
        NSRange lineBreak = [_pending rangeOfData:[NSData dataWithBytes:&newline length:1] options:0 range:NSMakeRange(0, _pending.length)];
        if (lineBreak.location == NSNotFound)
            return;
        NSData *lineData = [_pending subdataWithRange:NSMakeRange(0, lineBreak.location)];
        [_pending replaceBytesInRange:NSMakeRange(0, NSMaxRange(lineBreak)) withBytes:NULL length:0];
        NSString *line = [[[NSString alloc] initWithData:lineData encoding:NSUTF8StringEncoding] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        if (![line hasPrefix:@"data:"])
            continue;
        NSString *payload = [[line substringFromIndex:5] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        if ([payload isEqualToString:@"[DONE]"])
            continue;
        if (self.payloadHandler != nil)
            self.payloadHandler(payload);
    }
}

- (void)URLSession:(NSURLSession *)session task:(NSURLSessionTask *)task didCompleteWithError:(NSError *)error {
    (void) task;
    if (self.completionHandler != nil)
        self.completionHandler(_body, _statusCode, error);
    self.payloadHandler = nil;
    self.completionHandler = nil;
    [session finishTasksAndInvalidate];
}

@end
