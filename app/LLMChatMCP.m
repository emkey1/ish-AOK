//
//  LLMChatMCP.m
//  iSH-AOK
//
//  See LLMChatMCP.h. One ISHLLMMCPConnection per server, each on its own
//  serial queue, so a slow server holds up only its own calls; the manager
//  owns them and speaks for the chat.
//

#import "LLMChatMCP.h"
#import "LLMChatMCPCore.h"
#import "LLMKeychain.h"
#import "AppDelegate.h"
#include <poll.h>
#include <unistd.h>
#include "kernel/init.h"

NSString *const kISHLLMMCPKindRemote = @"remote";
NSString *const kISHLLMMCPKindGuest = @"guest";
static NSString *const kISHLLMMCPServersKey = @"LLM MCP Servers";

NSArray<NSDictionary<NSString *, id> *> *ISHLLMMCPServers(void) {
    id stored = [NSUserDefaults.standardUserDefaults objectForKey:kISHLLMMCPServersKey];
    NSMutableArray *servers = [NSMutableArray array];
    for (id entry in [stored isKindOfClass:NSArray.class] ? stored : @[]) {
        if ([entry isKindOfClass:NSDictionary.class] && [entry[@"id"] isKindOfClass:NSString.class])
            [servers addObject:entry];
    }
    return servers;
}

void ISHLLMSetMCPServers(NSArray<NSDictionary<NSString *, id> *> *servers) {
    NSMutableSet<NSString *> *gone = [NSMutableSet set];
    for (NSDictionary *old in ISHLLMMCPServers())
        [gone addObject:old[@"id"]];
    for (NSDictionary *server in servers)
        [gone removeObject:server[@"id"]];
    for (NSString *identifier in gone)
        ISHLLMSetMCPServerToken(identifier, nil);
    [NSUserDefaults.standardUserDefaults setObject:servers forKey:kISHLLMMCPServersKey];
}

// Tokens are secrets like API keys, and every defaults key is readable by
// the guest (see LLMKeychain.h), so they go in the Keychain.
NSString *ISHLLMMCPServerToken(NSString *serverID) {
    return ISHLLMKeychainRead([@"mcp:" stringByAppendingString:serverID]);
}

void ISHLLMSetMCPServerToken(NSString *serverID, NSString *token) {
    ISHLLMKeychainWrite([@"mcp:" stringByAppendingString:serverID], token);
}

static NSString *ISHLLMMCPString(NSDictionary *dictionary, NSString *key) {
    id value = dictionary[key];
    return [value isKindOfClass:NSString.class] ? value : @"";
}

#pragma mark - One server

@interface ISHLLMMCPConnection : NSObject
@property (nonatomic, copy, readonly) NSDictionary *config;
@property (nonatomic, copy, readonly) NSString *name;
@property (nonatomic, readonly) dispatch_queue_t queue;
@property (atomic, copy) NSArray<NSDictionary *> *tools;
@property (atomic, copy) NSDictionary<NSString *, NSString *> *nameMap;
@property (atomic) BOOL connected;
@end

@implementation ISHLLMMCPConnection {
    NSInteger _nextID;
    // remote
    NSURLSession *_session;
    NSString *_sessionID;
    // guest
    struct guest_process _process;
    BOOL _processRunning;
    NSMutableData *_stdoutBuffer;
    NSMutableString *_stderrTail;
    dispatch_source_t _stderrSource;
}

- (instancetype)initWithConfig:(NSDictionary *)config {
    if ((self = [super init])) {
        _config = [config copy];
        NSString *name = ISHLLMMCPString(config, @"name");
        _name = name.length > 0 ? name : @"server";
        _queue = dispatch_queue_create("aok.llm.mcp", DISPATCH_QUEUE_SERIAL);
        _nextID = 1;
        _stdoutBuffer = [NSMutableData data];
        _stderrTail = [NSMutableString string];
    }
    return self;
}

- (BOOL)isGuest {
    return [ISHLLMMCPString(_config, @"kind") isEqualToString:kISHLLMMCPKindGuest];
}

// MARK: Transports (on self.queue)

- (void)send:(NSDictionary *)message timeout:(NSTimeInterval)timeout
    expectReplyTo:(NSInteger)identifier reply:(NSDictionary **)replyOut error:(NSString **)errorOut {
    if (self.isGuest)
        [self sendGuest:message timeout:timeout expectReplyTo:identifier reply:replyOut error:errorOut];
    else
        [self sendRemote:message timeout:timeout expectReplyTo:identifier reply:replyOut error:errorOut];
}

- (void)sendRemote:(NSDictionary *)message timeout:(NSTimeInterval)timeout
     expectReplyTo:(NSInteger)identifier reply:(NSDictionary **)replyOut error:(NSString **)errorOut {
    NSURL *url = [NSURL URLWithString:ISHLLMMCPString(_config, @"url")];
    if (url == nil || url.scheme.length == 0) {
        *errorOut = @"the server URL is not valid";
        return;
    }
    if (_session == nil) {
        NSURLSessionConfiguration *configuration = NSURLSessionConfiguration.ephemeralSessionConfiguration;
        configuration.timeoutIntervalForRequest = 600;
        _session = [NSURLSession sessionWithConfiguration:configuration];
    }
    NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:url];
    request.HTTPMethod = @"POST";
    request.timeoutInterval = timeout;
    [request setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
    [request setValue:@"application/json, text/event-stream" forHTTPHeaderField:@"Accept"];
    [request setValue:kISHLLMMCPProtocolVersion forHTTPHeaderField:@"MCP-Protocol-Version"];
    if (_sessionID.length > 0)
        [request setValue:_sessionID forHTTPHeaderField:@"Mcp-Session-Id"];
    NSString *token = ISHLLMMCPServerToken(ISHLLMMCPString(_config, @"id"));
    if (token.length > 0)
        [request setValue:[@"Bearer " stringByAppendingString:token] forHTTPHeaderField:@"Authorization"];
    request.HTTPBody = [NSJSONSerialization dataWithJSONObject:message options:NSJSONWritingWithoutEscapingSlashes error:nil];
    __block NSData *body = nil;
    __block NSHTTPURLResponse *http = nil;
    __block NSError *failure = nil;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    [[_session dataTaskWithRequest:request completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
        body = data;
        http = [response isKindOfClass:NSHTTPURLResponse.class] ? (NSHTTPURLResponse *) response : nil;
        failure = error;
        dispatch_semaphore_signal(done);
    }] resume];
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    if (failure != nil) {
        *errorOut = failure.localizedDescription;
        return;
    }
    NSString *session = [http valueForHTTPHeaderField:@"Mcp-Session-Id"];
    if (session.length > 0)
        _sessionID = session;
    if (http.statusCode == 401 || http.statusCode == 403) {
        *errorOut = [NSString stringWithFormat:@"HTTP %ld: the server wants a (different) token", (long) http.statusCode];
        return;
    }
    if (identifier == 0) // a notification: 202 Accepted and no body
        return;
    NSDictionary *reply = ISHLLMMCPResponseInBody(body ?: NSData.data, [http valueForHTTPHeaderField:@"Content-Type"], identifier);
    if (reply == nil) {
        NSString *text = body.length > 0 ? [[NSString alloc] initWithData:body encoding:NSUTF8StringEncoding] : @"";
        *errorOut = [NSString stringWithFormat:@"HTTP %ld with no reply%@", (long) http.statusCode,
                     text.length > 0 ? [@": " stringByAppendingString:[text substringToIndex:MIN(text.length, (NSUInteger) 200)]] : @""];
        return;
    }
    *replyOut = reply;
}

- (BOOL)startGuestProcess:(NSString **)errorOut {
    NSString *command = ISHLLMMCPString(_config, @"command");
    if (command.length == 0) {
        *errorOut = @"no command set";
        return NO;
    }
    // Same account the chat's shell tool uses ("Open Everything as Default
    // User"), and a login-style environment through su when it is not root.
    NSString *account = [AppDelegate headlessCommandAccountName];
    int rc = guest_process_spawn_user(account.UTF8String, command.UTF8String, NULL, &_process);
    if (rc < 0) {
        *errorOut = [NSString stringWithFormat:@"could not start `%@` in the guest (error %d)", command, rc];
        return NO;
    }
    _processRunning = YES;
    // Keep draining stderr (a full pipe would stall the server) and keep the
    // tail of it for error messages.
    int stderrFD = _process.stderr_fd;
    _stderrSource = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, (uintptr_t) stderrFD, 0, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
    NSMutableString *tail = _stderrTail;
    dispatch_source_t source = _stderrSource;
    dispatch_source_set_event_handler(_stderrSource, ^{
        char buffer[1024];
        ssize_t n = read(stderrFD, buffer, sizeof(buffer));
        if (n <= 0) {
            dispatch_source_cancel(source);
            return;
        }
        NSString *text = [[NSString alloc] initWithBytes:buffer length:(NSUInteger) n encoding:NSUTF8StringEncoding] ?: @"";
        @synchronized (tail) {
            [tail appendString:text];
            if (tail.length > 2000)
                [tail deleteCharactersInRange:NSMakeRange(0, tail.length - 2000)];
        }
    });
    dispatch_resume(_stderrSource);
    return YES;
}

- (NSString *)stderrTail {
    @synchronized (_stderrTail) {
        return [[_stderrTail copy] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    }
}

- (BOOL)writeGuestMessage:(NSDictionary *)message {
    NSMutableData *line = [[NSJSONSerialization dataWithJSONObject:message options:NSJSONWritingWithoutEscapingSlashes error:nil] mutableCopy];
    [line appendBytes:"\n" length:1];
    const uint8_t *bytes = line.bytes;
    size_t remaining = line.length;
    while (remaining > 0) {
        ssize_t n = write(_process.stdin_fd, bytes, remaining);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return NO;
        bytes += n;
        remaining -= (size_t) n;
    }
    return YES;
}

// stdio servers speak one JSON-RPC message per line on stdout.
- (void)sendGuest:(NSDictionary *)message timeout:(NSTimeInterval)timeout
    expectReplyTo:(NSInteger)identifier reply:(NSDictionary **)replyOut error:(NSString **)errorOut {
    if (!_processRunning && ![self startGuestProcess:errorOut])
        return;
    if (![self writeGuestMessage:message]) {
        *errorOut = [self processEndedMessage];
        [self stopGuestProcess];
        return;
    }
    if (identifier == 0)
        return;
    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:timeout];
    while (YES) {
        NSRange newline = [_stdoutBuffer rangeOfData:[NSData dataWithBytes:"\n" length:1] options:0 range:NSMakeRange(0, _stdoutBuffer.length)];
        if (newline.location != NSNotFound) {
            NSData *lineData = [_stdoutBuffer subdataWithRange:NSMakeRange(0, newline.location)];
            [_stdoutBuffer replaceBytesInRange:NSMakeRange(0, NSMaxRange(newline)) withBytes:NULL length:0];
            id parsed = lineData.length > 0 ? [NSJSONSerialization JSONObjectWithData:lineData options:0 error:nil] : nil;
            if (![parsed isKindOfClass:NSDictionary.class])
                continue; // a server's stray log line on stdout
            NSDictionary *incoming = parsed;
            if (incoming[@"method"] != nil && incoming[@"id"] != nil) {
                // The server asks us something: answer a ping, decline the
                // rest (roots/list, sampling, ...), so it does not wait on an
                // answer forever.
                if ([incoming[@"method"] isEqual:@"ping"])
                    [self writeGuestMessage:@{@"jsonrpc": @"2.0", @"id": incoming[@"id"], @"result": @{}}];
                else
                    [self writeGuestMessage:@{@"jsonrpc": @"2.0", @"id": incoming[@"id"],
                                              @"error": @{@"code": @-32601, @"message": @"Method not found"}}];
                continue;
            }
            NSDictionary *reply = ISHLLMMCPResponseInBody(lineData, nil, identifier);
            if (reply != nil) {
                *replyOut = reply;
                return;
            }
            continue;
        }
        NSTimeInterval left = [deadline timeIntervalSinceNow];
        if (left <= 0) {
            *errorOut = [NSString stringWithFormat:@"no reply within %.0f seconds%@", timeout, [self stderrSuffix]];
            return;
        }
        struct pollfd pfd = {.fd = _process.stdout_fd, .events = POLLIN};
        int ready = poll(&pfd, 1, (int) MIN(left * 1000, 1000));
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready <= 0)
            continue;
        char buffer[16384];
        ssize_t n = read(_process.stdout_fd, buffer, sizeof(buffer));
        if (n <= 0) {
            *errorOut = [self processEndedMessage];
            [self stopGuestProcess];
            return;
        }
        [_stdoutBuffer appendBytes:buffer length:(NSUInteger) n];
    }
}

- (NSString *)stderrSuffix {
    NSString *tail = self.stderrTail;
    return tail.length > 0 ? [@". Its last output: " stringByAppendingString:tail] : @"";
}

- (NSString *)processEndedMessage {
    // Give the stderr drain a moment to collect the reason it died.
    usleep(200 * 1000);
    return [NSString stringWithFormat:@"the server process ended%@", [self stderrSuffix]];
}

- (void)stopGuestProcess {
    if (!_processRunning)
        return;
    if (_stderrSource != nil) {
        dispatch_source_cancel(_stderrSource);
        _stderrSource = nil;
    }
    guest_process_stop(&_process);
    _processRunning = NO;
    [_stdoutBuffer setLength:0];
}

// MARK: Protocol (on self.queue)

- (NSDictionary *)call:(NSString *)method params:(NSDictionary *)params timeout:(NSTimeInterval)timeout error:(NSString **)errorOut {
    NSInteger identifier = _nextID++;
    NSDictionary *reply = nil;
    NSString *error = nil;
    [self send:ISHLLMMCPRequest(identifier, method, params) timeout:timeout expectReplyTo:identifier reply:&reply error:&error];
    if (error != nil) {
        *errorOut = error;
        return nil;
    }
    NSDictionary *result = ISHLLMMCPResult(reply, &error);
    if (result == nil)
        *errorOut = error;
    return result;
}

- (BOOL)connect:(NSString **)errorOut {
    [self disconnect];
    // A first `npx -y …` downloads the package before it answers.
    NSTimeInterval timeout = self.isGuest ? 180 : 30;
    NSString *error = nil;
    NSDictionary *initialized = [self call:@"initialize" params:ISHLLMMCPInitializeParams() timeout:timeout error:&error];
    if (initialized == nil) {
        *errorOut = [@"initialize: " stringByAppendingString:error ?: @"failed"];
        [self disconnect];
        return NO;
    }
    NSString *ignored = nil;
    [self send:ISHLLMMCPNotification(@"notifications/initialized", nil) timeout:10 expectReplyTo:0 reply:NULL error:&ignored];
    NSMutableArray *tools = [NSMutableArray array];
    NSString *cursor = nil;
    for (int page = 0; page < 20; page++) {
        NSDictionary *listed = [self call:@"tools/list" params:cursor.length > 0 ? @{@"cursor": cursor} : @{} timeout:30 error:&error];
        if (listed == nil) {
            *errorOut = [@"tools/list: " stringByAppendingString:error ?: @"failed"];
            [self disconnect];
            return NO;
        }
        [tools addObjectsFromArray:[listed[@"tools"] isKindOfClass:NSArray.class] ? listed[@"tools"] : @[]];
        cursor = [listed[@"nextCursor"] isKindOfClass:NSString.class] ? listed[@"nextCursor"] : nil;
        if (cursor.length == 0)
            break;
    }
    NSDictionary *map = nil;
    self.tools = ISHLLMMCPFunctionTools(self.name, tools, &map);
    self.nameMap = map;
    self.connected = YES;
    return YES;
}

- (void)disconnect {
    self.connected = NO;
    self.tools = @[];
    self.nameMap = @{};
    [self stopGuestProcess];
    // Tell a remote server the session is over (the spec's DELETE), without
    // waiting for its answer; a server that does not keep sessions says 405.
    NSURL *url = [NSURL URLWithString:ISHLLMMCPString(_config, @"url") ?: @""];
    if (_sessionID.length > 0 && url != nil) {
        NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:url];
        request.HTTPMethod = @"DELETE";
        request.timeoutInterval = 10;
        [request setValue:_sessionID forHTTPHeaderField:@"Mcp-Session-Id"];
        [request setValue:kISHLLMMCPProtocolVersion forHTTPHeaderField:@"MCP-Protocol-Version"];
        NSString *token = ISHLLMMCPServerToken(ISHLLMMCPString(_config, @"id"));
        if (token.length > 0)
            [request setValue:[@"Bearer " stringByAppendingString:token] forHTTPHeaderField:@"Authorization"];
        [[NSURLSession.sharedSession dataTaskWithRequest:request] resume];
    }
    _sessionID = nil;
    [_session invalidateAndCancel];
    _session = nil;
}

@end

#pragma mark - Manager

@implementation ISHLLMMCPManager {
    NSMutableDictionary<NSString *, ISHLLMMCPConnection *> *_connections; // server id -> connection; main thread
    // A server that failed to connect is not retried for a while with the
    // same settings, so one that hangs does not hold up every message:
    // server id -> @{config, date, error}. Main thread.
    NSMutableDictionary<NSString *, NSDictionary *> *_failures;
}

static const NSTimeInterval kISHLLMMCPRetryInterval = 300;

+ (instancetype)shared {
    static ISHLLMMCPManager *manager;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        manager = [ISHLLMMCPManager new];
    });
    return manager;
}

- (instancetype)init {
    if ((self = [super init])) {
        _connections = [NSMutableDictionary dictionary];
        _failures = [NSMutableDictionary dictionary];
    }
    return self;
}

- (void)prepareWithCompletion:(void (^)(NSArray<NSString *> *))completion {
    NSMutableArray<NSString *> *problems = [NSMutableArray array];
    NSMutableSet<NSString *> *wanted = [NSMutableSet set];
    dispatch_group_t group = dispatch_group_create();
    for (NSDictionary *server in ISHLLMMCPServers()) {
        if (![server[@"enabled"] boolValue])
            continue;
        NSString *identifier = server[@"id"];
        [wanted addObject:identifier];
        ISHLLMMCPConnection *connection = _connections[identifier];
        if (connection != nil && [connection.config isEqualToDictionary:server] && connection.connected)
            continue;
        NSDictionary *failure = _failures[identifier];
        if (failure != nil && [failure[@"config"] isEqualToDictionary:server] &&
            -[failure[@"date"] timeIntervalSinceNow] < kISHLLMMCPRetryInterval) {
            continue; // reported when it failed
        }
        [_failures removeObjectForKey:identifier];
        if (connection != nil)
            dispatch_async(connection.queue, ^{ [connection disconnect]; });
        connection = [[ISHLLMMCPConnection alloc] initWithConfig:server];
        @synchronized (self) {
            _connections[identifier] = connection;
        }
        dispatch_group_async(group, connection.queue, ^{
            NSString *error = nil;
            if (![connection connect:&error]) {
                [connection disconnect];
                NSString *reason = error ?: NSLocalizedString(@"could not connect", @"chat note, MCP server failed to connect");
                @synchronized (problems) {
                    [problems addObject:[NSString stringWithFormat:NSLocalizedString(@"MCP server \"%@\": %@", @"chat note; first %@ is the MCP server name, second the reason"), connection.name, reason]];
                }
                dispatch_async(dispatch_get_main_queue(), ^{
                    self->_failures[identifier] = @{@"config": server, @"date": [NSDate date], @"error": reason};
                });
            }
        });
    }
    // Servers removed or turned off since last time.
    for (NSString *identifier in _connections.allKeys) {
        if (![wanted containsObject:identifier]) {
            ISHLLMMCPConnection *connection = _connections[identifier];
            dispatch_async(connection.queue, ^{ [connection disconnect]; });
            @synchronized (self) {
                [_connections removeObjectForKey:identifier];
            }
        }
    }
    dispatch_group_notify(group, dispatch_get_main_queue(), ^{
        completion([problems copy]);
    });
}

- (NSArray<NSDictionary *> *)toolDefinitions {
    NSMutableArray *all = [NSMutableArray array];
    for (ISHLLMMCPConnection *connection in _connections.allValues) {
        if (connection.connected)
            [all addObjectsFromArray:connection.tools];
    }
    return all;
}

- (ISHLLMMCPConnection *)connectionForTool:(NSString *)exposedName {
    @synchronized (self) {
        for (ISHLLMMCPConnection *connection in _connections.allValues) {
            if (connection.nameMap[exposedName] != nil)
                return connection;
        }
    }
    return nil;
}

- (BOOL)knowsTool:(NSString *)exposedName {
    return [self connectionForTool:exposedName] != nil;
}

- (NSString *)describeTool:(NSString *)exposedName {
    ISHLLMMCPConnection *connection = [self connectionForTool:exposedName];
    if (connection == nil) {
        // Not connected (a chat reopened after launch): split the exposed
        // name, mcp__<server>__<tool>, as well as it can be.
        NSString *rest = [exposedName hasPrefix:kISHLLMMCPToolPrefix] ? [exposedName substringFromIndex:kISHLLMMCPToolPrefix.length] : exposedName;
        NSRange split = [rest rangeOfString:@"__"];
        return split.location == NSNotFound ? rest
            : [NSString stringWithFormat:@"%@: %@", [rest substringToIndex:split.location], [rest substringFromIndex:NSMaxRange(split)]];
    }
    return [NSString stringWithFormat:@"%@: %@", connection.name, connection.nameMap[exposedName]];
}

- (NSString *)callTool:(NSString *)exposedName arguments:(NSDictionary *)arguments isError:(BOOL *)isErrorOut summary:(NSString **)summaryOut {
    ISHLLMMCPConnection *connection = [self connectionForTool:exposedName];
    if (connection == nil) {
        if (isErrorOut != NULL)
            *isErrorOut = YES;
        if (summaryOut != NULL)
            *summaryOut = @"unknown MCP tool";
        return [NSString stringWithFormat:@"No connected MCP server offers %@.", exposedName];
    }
    NSString *toolName = connection.nameMap[exposedName];
    __block NSString *text = nil;
    __block BOOL isError = NO;
    // Calls on one server take turns on its queue; this thread waits.
    dispatch_sync(connection.queue, ^{
        NSString *error = nil;
        NSDictionary *result = connection.connected
            ? [connection call:@"tools/call" params:@{@"name": toolName, @"arguments": arguments ?: @{}} timeout:600 error:&error]
            : nil;
        if (result == nil) {
            isError = YES;
            text = [NSString stringWithFormat:@"The MCP server \"%@\" failed: %@", connection.name, error ?: @"not connected"];
            return;
        }
        text = ISHLLMMCPResultText(result, &isError);
    });
    if (isErrorOut != NULL)
        *isErrorOut = isError;
    if (summaryOut != NULL)
        *summaryOut = [NSString stringWithFormat:@"%@%@, %lu bytes", [self describeTool:exposedName], isError ? @" error" : @"", (unsigned long) text.length];
    return text;
}

- (void)checkServer:(NSDictionary *)server completion:(void (^)(NSArray<NSString *> *, NSString *))completion {
    ISHLLMMCPConnection *connection = [[ISHLLMMCPConnection alloc] initWithConfig:server];
    dispatch_async(connection.queue, ^{
        NSString *error = nil;
        BOOL ok = [connection connect:&error];
        NSMutableArray *names = [NSMutableArray array];
        for (NSString *exposed in connection.nameMap)
            [names addObject:connection.nameMap[exposed]];
        [connection disconnect];
        dispatch_async(dispatch_get_main_queue(), ^{
            if (ok && [server[@"id"] isKindOfClass:NSString.class])
                [self->_failures removeObjectForKey:server[@"id"]];
            completion(ok ? [names sortedArrayUsingSelector:@selector(compare:)] : nil, ok ? nil : error);
        });
    });
}

- (void)disconnectAll {
    @synchronized (self) {
        for (ISHLLMMCPConnection *connection in _connections.allValues)
            dispatch_async(connection.queue, ^{ [connection disconnect]; });
        [_connections removeAllObjects];
    }
    [_failures removeAllObjects];
}

@end
