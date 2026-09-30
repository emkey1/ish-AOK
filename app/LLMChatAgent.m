//
//  LLMChatAgent.m
//  iSH-AOK
//
//  See LLMChatAgent.h. The reply loop here was the chat screen's until
//  2026-09-30; it moved so that it no longer ends with the window.
//

#import "LLMChatAgent.h"
#import "UserPreferences.h"
#import "GuestFileBridge.h"
#import "LLMChatAnthropic.h"
#import "LLMChatStream.h"
#import "LLMChatMCP.h"
#import "LLMChatMCPCore.h"
#if __has_include("libiSH_AOKApp-Swift.h")
#import "libiSH_AOKApp-Swift.h" // AOKFoundationModelsBridge
#endif
#include <sys/socket.h>

NSNotificationName const ISHLLMAgentStateDidChangeNotification = @"ISHLLMAgentStateDidChangeNotification";

static BOOL ISHLLMUsesAppleFoundationModelsForDestination(NSDictionary<NSString *, NSString *> *destination) {
    return [(ISHLLMStringValue(destination, kISHLLMDestinationProvider) ?: @"").lowercaseString containsString:@"foundation models"];
}

// How many of one chat's sub-agents may work at once.
static const NSUInteger kISHLLMMaxRunningSubagents = 4;

@interface ISHLLMAgent ()
- (void)approval:(ISHLLMAgentApproval *)approval resolvedWith:(ISHLLMToolRunDecision)decision;
- (void)attachToParent:(ISHLLMAgent *)parent;
@end

@implementation ISHLLMAgentApproval {
    void (^_completion)(ISHLLMToolRunDecision decision);
}

- (instancetype)initWithAgent:(ISHLLMAgent *)agent invocation:(ISHLLMToolInvocation *)invocation reason:(NSString *)reason
                   completion:(void (^)(ISHLLMToolRunDecision decision))completion {
    if ((self = [super init])) {
        _agent = agent;
        _invocation = invocation;
        _reason = [reason copy];
        _completion = [completion copy];
    }
    return self;
}

- (void)resolve:(ISHLLMToolRunDecision)decision {
    if (_resolved)
        return;
    _resolved = YES;
    void (^completion)(ISHLLMToolRunDecision) = _completion;
    _completion = nil;
    [_agent approval:self resolvedWith:decision];
    if (completion != nil)
        completion(decision);
}

@end

@implementation ISHLLMAgent {
    NSMutableArray<NSDictionary<NSString *, id> *> *_messages;
    NSHashTable<id<ISHLLMAgentObserver>> *_observers;
    NSInteger _viewers;
    BOOL _titleIsAutomatic;
    double _lastKnownSessionUpdate;

    // The reply in flight.
    BOOL _sending;
    BOOL _cancelled;
    NSURLSessionDataTask *_activeTask;
    NSURLSessionDataTask *_auxiliaryTask;
    int _activeStreamFD;
    NSDictionary<NSString *, NSString *> *_requestDestination; // snapshot for the reply in flight
    NSMutableArray<void (^)(void)> *_idleActions;
    NSMutableArray<NSString *> *_queuedPrompts;
    NSInteger _toolLoopCompactedRound;

    // Approvals.
    BOOL _autoRunCommandsThisReply;
    BOOL _autoRunCommandsThisChat;
    NSMutableDictionary<NSString *, NSNumber *> *_commandDecisionsThisReply; // Apple FM repeat guard

    // Context.
    NSString *_projectInstructions;
    NSString *_projectInstructionsSource;
    NSInteger _knownContextWindowTokens;
    NSString *_knownContextWindowProbeKey;
    BOOL _contextWindowProbeInFlight;

    NSString *_runningToolName;

    // Sub-agents.
    NSMutableArray<ISHLLMAgent *> *_subagents;
    NSMutableArray<void (^)(void)> *_resultWaiters; // agent_result calls waiting on a sub-agent
}

- (instancetype)initWithSessionEntry:(NSDictionary<NSString *, id> *)entry {
    if ((self = [super init])) {
        _messages = [NSMutableArray array];
        _observers = [NSHashTable weakObjectsHashTable];
        _idleActions = [NSMutableArray array];
        _queuedPrompts = [NSMutableArray array];
        _commandDecisionsThisReply = [NSMutableDictionary dictionary];
        _subagents = [NSMutableArray array];
        _resultWaiters = [NSMutableArray array];
        _sessionID = [ISHLLMStringValue(entry, @"id") copy];
        _shortID = [[_sessionID substringToIndex:MIN((NSUInteger) 8, _sessionID.length)] lowercaseString];
        _toolContext = [ISHLLMToolContext new];
        _toolContext.queue = dispatch_queue_create([[@"aok.llm.agent." stringByAppendingString:_shortID] UTF8String], DISPATCH_QUEUE_SERIAL);
        [self loadFromEntry:entry];
    }
    return self;
}

- (void)loadFromEntry:(NSDictionary<NSString *, id> *)entry {
    _title = [ISHLLMStringValue(entry, @"title") copy];
    _systemPrompt = [ISHLLMStringValue(entry, @"system") copy] ?: @"";
    _titleIsAutomatic = ![entry[@"titleIsCustom"] boolValue];
    _parentSessionID = [ISHLLMStringValue(entry, @"parent") copy];
    _lastKnownSessionUpdate = [entry[@"updated"] isKindOfClass:NSNumber.class] ? [entry[@"updated"] doubleValue] : 0.0;
    [_messages setArray:ISHLLMLoadSessionMessages(_sessionID)];
    NSString *workingDirectory = ISHLLMStringValue(entry, @"workingDirectory");
    _toolContext.workingDirectory = workingDirectory.length > 0 ? workingDirectory : ISHLLMAgentManager.shared.guestHomeDirectory;
}

- (NSArray<NSDictionary<NSString *, id> *> *)messages {
    return _messages;
}

- (NSArray<NSString *> *)queuedPrompts {
    return [_queuedPrompts copy];
}

- (NSArray<ISHLLMAgent *> *)subagents {
    return [_subagents copy];
}

#pragma mark - Destination

// The chat's saved destination, else the one selected in Settings. Read
// fresh each time, so an edit in Settings applies from the next reply.
- (NSDictionary<NSString *, NSString *> *)destination {
    NSString *destinationID = ISHLLMStringValue(ISHLLMSessionEntryWithID(_sessionID), @"destination");
    if (destinationID.length > 0) {
        for (NSDictionary<NSString *, NSString *> *destination in ISHLLMDestinations()) {
            if ([ISHLLMStringValue(destination, kISHLLMDestinationID) isEqualToString:destinationID])
                return destination;
        }
    }
    return ISHLLMActiveDestination();
}

- (NSDictionary<NSString *, NSString *> *)scopeDestination {
    return _requestDestination ?: [self destination];
}

// Every piece of the loop that builds or sends a request runs inside one of
// these, so the endpoint helpers see this chat's destination, not whichever
// one Settings has selected.
- (void)scoped:(dispatch_block_t)block {
    ISHLLMRunWithDestination([self scopeDestination], block);
}

- (void)onMain:(dispatch_block_t)block {
    dispatch_async(dispatch_get_main_queue(), ^{
        [self scoped:block];
    });
}

- (void)inBackground:(dispatch_block_t)block {
    NSDictionary<NSString *, NSString *> *destination = [self scopeDestination];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        ISHLLMRunWithDestination(destination, block);
    });
}

- (NSString *)modelName {
    return [ISHLLMStringValue([self scopeDestination], kISHLLMDestinationModel) ?: @""
            stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
}

- (NSString *)apiKey {
    return ISHLLMStringValue([self scopeDestination], kISHLLMDestinationAPIKey) ?: @"";
}

- (void)useDestinationWithID:(NSString *)destinationID {
    ISHLLMUpdateSessionEntry(_sessionID, @{@"destination": destinationID ?: @""});
    [self notifyMetadata];
}

#pragma mark - Observers and status

- (void)addObserver:(id<ISHLLMAgentObserver>)observer {
    [_observers addObject:observer];
}

- (void)removeObserver:(id<ISHLLMAgentObserver>)observer {
    [_observers removeObject:observer];
}

- (void)beginViewing {
    _viewers++;
    [self markSeen];
}

- (void)endViewing {
    _viewers = MAX(0, _viewers - 1);
}

- (void)markSeen {
    if (!_finishedUnseen)
        return;
    _finishedUnseen = NO;
    [self postStateChange];
}

- (void)notifyMessages {
    for (id<ISHLLMAgentObserver> observer in _observers.allObjects) {
        if ([observer respondsToSelector:@selector(agentMessagesDidChange:)])
            [observer agentMessagesDidChange:self];
    }
}

- (void)notifyStreamingAtIndex:(NSUInteger)index {
    for (id<ISHLLMAgentObserver> observer in _observers.allObjects) {
        if ([observer respondsToSelector:@selector(agent:didUpdateStreamingMessageAtIndex:)])
            [observer agent:self didUpdateStreamingMessageAtIndex:index];
    }
}

- (void)notifyStatus {
    for (id<ISHLLMAgentObserver> observer in _observers.allObjects) {
        if ([observer respondsToSelector:@selector(agentStatusDidChange:)])
            [observer agentStatusDidChange:self];
    }
}

- (void)notifyMetadata {
    for (id<ISHLLMAgentObserver> observer in _observers.allObjects) {
        if ([observer respondsToSelector:@selector(agentMetadataDidChange:)])
            [observer agentMetadataDidChange:self];
    }
}

- (void)postStateChange {
    [self notifyStatus];
    [NSNotificationCenter.defaultCenter postNotificationName:ISHLLMAgentStateDidChangeNotification object:self];
    // A sub-agent's state shows in its parent's status bar.
    ISHLLMAgent *parent = _parent;
    if (parent != nil)
        [parent notifyStatus];
}

- (void)setPhase:(ISHLLMAgentPhase)phase detail:(NSString *)detail {
    BOOL changed = phase != _phase;
    if (changed)
        _phaseStarted = [NSDate date];
    _phase = phase;
    _phaseDetail = [detail copy];
    if (changed)
        [self postStateChange];
    else
        [self notifyStatus];
}

- (BOOL)isBusy {
    return _sending || _activeTask != nil || _activeStreamFD != 0;
}

- (NSString *)statusLine {
    switch (_phase) {
        case ISHLLMAgentPhaseIdle:
            if ([self modelName].length == 0 && !ISHLLMUsesAppleFoundationModelsForDestination([self destination]))
                return @"No model set";
            return _lastOutcome.length > 0 ? _lastOutcome : @"Ready";
        case ISHLLMAgentPhasePreparing: return @"Getting ready";
        case ISHLLMAgentPhaseContacting: return _round > 0 ? @"Waiting for the model" : @"Contacting the model";
        case ISHLLMAgentPhaseThinking: return @"Thinking";
        case ISHLLMAgentPhaseWriting: return @"Writing";
        case ISHLLMAgentPhaseTool: return [self runningToolTitle];
        case ISHLLMAgentPhaseApproval: return @"Needs your approval";
        case ISHLLMAgentPhaseWaitingForAgents: return @"Waiting for sub-agent";
        case ISHLLMAgentPhaseSummarizing: return @"Summarizing the conversation";
    }
    return @"";
}

- (NSString *)runningToolTitle {
    NSString *name = _runningToolName;
    if ([name isEqualToString:@"read_file"])
        return @"Reading file";
    if ([name isEqualToString:@"write_file"])
        return @"Writing file";
    if ([name isEqualToString:@"edit_file"])
        return @"Editing file";
    if ([name isEqualToString:@"list_directory"])
        return @"Listing directory";
    if ([name isEqualToString:@"glob"] || [name isEqualToString:@"grep"])
        return @"Searching";
    if ([name isEqualToString:@"todo_write"])
        return @"Updating the task list";
    if ([name isEqualToString:kISHLLMSpawnAgentTool])
        return @"Starting a sub-agent";
    if ([name hasPrefix:kISHLLMMCPToolPrefix])
        return @"Using MCP tool";
    return @"Running command";
}

- (ISHLLMAgentApproval *)pendingApprovalIncludingSubagents {
    if (_pendingApproval != nil && !_pendingApproval.resolved)
        return _pendingApproval;
    for (ISHLLMAgent *subagent in _subagents) {
        ISHLLMAgentApproval *approval = [subagent pendingApprovalIncludingSubagents];
        if (approval != nil)
            return approval;
    }
    return nil;
}

#pragma mark - Busy and idle

// The funnel every reply ends in.
- (void)setSending:(BOOL)sending {
    BOOL wasSending = _sending;
    _sending = sending;
    if (sending) {
        if (!wasSending) {
            _replyStarted = [NSDate date];
            _round = 0;
            _toolCallsThisReply = 0;
            _lastOutcome = nil;
        }
        [self setPhase:ISHLLMAgentPhaseContacting detail:nil];
        return;
    }
    _requestDestination = nil;
    _runningToolName = nil;
    if (wasSending) {
        _lastFinished = [NSDate date];
        if (_lastOutcome.length == 0)
            _lastOutcome = [self outcomeOfLastReply];
        if (_viewers == 0)
            _finishedUnseen = YES;
    }
    [self setPhase:ISHLLMAgentPhaseIdle detail:nil];
    [self postStateChange];
    if (_idleActions.count > 0 || _queuedPrompts.count > 0) {
        NSArray<void (^)(void)> *actions = [_idleActions copy];
        [_idleActions removeAllObjects];
        // A turn later: several paths call setSending:NO before appending the
        // reply they just received.
        dispatch_async(dispatch_get_main_queue(), ^{
            for (void (^action)(void) in actions)
                action();
            [self sendNextQueuedPrompt];
        });
    }
}

- (void)sendNextQueuedPrompt {
    if ([self isBusy] || _queuedPrompts.count == 0)
        return;
    NSString *prompt = _queuedPrompts.firstObject;
    [_queuedPrompts removeObjectAtIndex:0];
    [self sendPrompt:prompt];
}

- (NSString *)outcomeOfLastReply {
    NSString *last = [self lastAssistantText];
    if ([last hasSuffix:@"(stopped)"])
        return @"Stopped";
    if ([last hasPrefix:@"Request failed"] || [last hasPrefix:@"Unexpected response"] || [last hasPrefix:@"Invalid "])
        return @"Failed";
    return @"Done";
}

- (NSString *)lastAssistantText {
    for (NSDictionary<NSString *, id> *message in _messages.reverseObjectEnumerator) {
        NSString *role = ISHLLMStringValue(message, @"role");
        if ([role isEqualToString:@"user"])
            return @"";
        if ([role isEqualToString:@"assistant"]) {
            NSString *content = ISHLLMStringValue(message, @"content");
            if (content.length > 0)
                return content;
        }
    }
    return @"";
}

- (void)stop {
    [_queuedPrompts removeAllObjects];
    _cancelled = YES;
    [_activeTask cancel];
    [_auxiliaryTask cancel];
    if (_activeStreamFD > 0)
        shutdown(_activeStreamFD, SHUT_RDWR);
#if __has_include("libiSH_AOKApp-Swift.h")
    if (ISHLLMUsesAppleFoundationModelsForDestination([self scopeDestination]))
        [AOKFoundationModelsBridge cancelActiveRequest];
#endif
    // A question nobody will answer now; the loop sees _cancelled next.
    if (_pendingApproval != nil && !_pendingApproval.resolved)
        [_pendingApproval resolve:ISHLLMToolRunDecline];
    for (ISHLLMAgent *subagent in _subagents) {
        if (subagent.busy)
            [subagent stop];
    }
    // Waiting on sub-agents: nothing else will wake the loop.
    [self wakeResultWaiters];
}

- (void)stopThen:(void (^)(void))continuation {
    if (![self isBusy]) {
        continuation();
        return;
    }
    [_idleActions addObject:[continuation copy]];
    [self stop];
}

- (void)whenIdle:(void (^)(void))action {
    if (![self isBusy]) {
        action();
        return;
    }
    [_idleActions addObject:[action copy]];
}

#pragma mark - Messages

- (void)appendRole:(NSString *)role content:(NSString *)content {
    if (content.length == 0)
        return;
    [_messages addObject:@{@"role": role, @"content": content}];
    [self saveTranscript];
    [self notifyMessages];
}

- (void)appendLocalRole:(NSString *)role content:(NSString *)content {
    if (content.length == 0)
        return;
    [_messages addObject:@{@"role": role, @"content": content, @"local": @"1"}];
    [self saveTranscript];
    [self notifyMessages];
}

- (BOOL)messageIsLocalOnly:(NSDictionary<NSString *, id> *)message {
    if ([message[@"local"] isEqual:@"1"])
        return YES;
    NSString *role = ISHLLMStringValue(message, @"role");
    NSString *content = ISHLLMStringValue(message, @"content");
    if ([role isEqualToString:@"user"] && [content hasPrefix:@"/"])
        return YES;
    if ([role isEqualToString:@"assistant"]) {
        if ([content hasPrefix:@"Model set to "] || [content hasPrefix:@"Current model:"] ||
            [content hasPrefix:@"Model query failed:"] || [content hasPrefix:@"No models found"] ||
            [content hasPrefix:@"Invalid models URL"] || [content containsString:@" models returned by "])
            return YES;
    }
    return NO;
}

// Everything from the latest summary on; what came before it reaches the
// model only as that summary. The transcript on screen keeps all of it.
- (NSArray<NSDictionary<NSString *, id> *> *)messagesSentToModel {
    for (NSInteger i = (NSInteger) _messages.count - 1; i >= 0; i--) {
        if ([_messages[(NSUInteger) i][@"compacted"] isEqual:@"1"])
            return [_messages subarrayWithRange:NSMakeRange((NSUInteger) i, _messages.count - (NSUInteger) i)];
    }
    return _messages;
}

- (NSArray<NSDictionary<NSString *, id> *> *)providerMessages {
    // Which tool results are recent enough to send in full: walk them newest
    // to oldest, keeping full content until the running estimate would blow
    // the budget. Always keep at least the single most recent result in full.
    // Identity (not equality) keyed, since two results can have identical
    // content.
    NSInteger budget = [self toolResultContextBudgetTokens];
    NSMutableSet<NSValue *> *fullContentToolMessages = [NSMutableSet set];
    NSInteger runningTokens = 0;
    NSArray<NSDictionary<NSString *, id> *> *live = [self messagesSentToModel];
    for (NSDictionary<NSString *, id> *message in live.reverseObjectEnumerator) {
        if (![ISHLLMStringValue(message, @"role") isEqualToString:@"tool"])
            continue;
        NSInteger tokens = ISHLLMEstimateTokenCount(ISHLLMStringValue(message, @"content") ?: @"");
        if (runningTokens + tokens > budget && fullContentToolMessages.count > 0)
            break;
        runningTokens += tokens;
        [fullContentToolMessages addObject:[NSValue valueWithNonretainedObject:message]];
    }

    BOOL anthropic = ISHLLMUsesAnthropicAPI();
    NSMutableArray<NSDictionary<NSString *, id> *> *messages = [NSMutableArray array];
    if (_systemPrompt.length > 0)
        [messages addObject:@{@"role": @"system", @"content": _systemPrompt}];
    for (NSDictionary<NSString *, id> *message in live) {
        if ([self messageIsLocalOnly:message])
            continue;
        NSString *role = ISHLLMStringValue(message, @"role");
        if (role.length == 0)
            continue;
        NSString *content = ISHLLMStringValue(message, @"content") ?: @"";
        if ([role isEqualToString:@"tool"]) {
            NSString *toolCallID = ISHLLMStringValue(message, @"tool_call_id");
            if (toolCallID.length == 0)
                continue;
            NSString *sentContent = content;
            if (![fullContentToolMessages containsObject:[NSValue valueWithNonretainedObject:message]]) {
                NSString *summary = ISHLLMStringValue(message, @"summary");
                sentContent = [NSString stringWithFormat:@"(output omitted to save context: %@. Call the tool again if you need it.)",
                               summary.length > 0 ? summary : @"result compacted"];
            }
            [messages addObject:@{@"role": @"tool", @"tool_call_id": toolCallID, @"content": sentContent}];
            continue;
        }
        NSArray *toolCalls = [message[@"tool_calls"] isKindOfClass:NSArray.class] ? message[@"tool_calls"] : nil;
        if (toolCalls.count > 0) {
            NSMutableDictionary *entry = [@{@"role": role, @"content": content, @"tool_calls": toolCalls} mutableCopy];
            // Only the Anthropic translation reads it; an OpenAI-compatible
            // server could reject an unknown field.
            if (anthropic && [message[kISHLLMAnthropicContentKey] isKindOfClass:NSArray.class])
                entry[kISHLLMAnthropicContentKey] = message[kISHLLMAnthropicContentKey];
            [messages addObject:entry];
            continue;
        }
        if (content.length > 0)
            [messages addObject:@{@"role": role, @"content": content}];
    }
    return messages;
}

- (void)saveTranscript {
    if (_sessionID.length == 0)
        return;
    // A chat deleted from the list has had its file removed already; writing
    // it back would resurrect the transcript as an orphan.
    if (ISHLLMSessionEntryWithID(_sessionID) == nil)
        return;
    ISHLLMWriteSessionMessages(_sessionID, _messages);
    NSMutableDictionary<NSString *, id> *updates = [NSMutableDictionary dictionary];
    updates[@"updated"] = @(NSDate.date.timeIntervalSince1970);
    updates[@"count"] = @(_messages.count);
    BOOL titleChanged = NO;
    if (_titleIsAutomatic && _parentSessionID.length == 0) {
        NSString *derived = ISHLLMSessionTitleFromMessages(_messages);
        NSString *title = derived.length > 0 ? derived : @"New Chat";
        if (![title isEqualToString:_title]) {
            _title = [title copy];
            updates[@"title"] = title;
            titleChanged = YES;
        }
    }
    _lastKnownSessionUpdate = [updates[@"updated"] doubleValue];
    ISHLLMUpdateSessionEntry(_sessionID, updates);
    if (titleChanged)
        [self notifyMetadata];
}

- (void)reloadIfChangedOnDisk {
    if ([self isBusy])
        return;
    NSDictionary<NSString *, id> *entry = ISHLLMSessionEntryWithID(_sessionID);
    if (entry == nil)
        return;
    double updated = [entry[@"updated"] isKindOfClass:NSNumber.class] ? [entry[@"updated"] doubleValue] : 0.0;
    if (updated <= _lastKnownSessionUpdate)
        return;
    [self loadFromEntry:entry];
    [self notifyMessages];
    [self notifyMetadata];
}

- (void)setCustomTitle:(NSString *)title {
    NSString *trimmed = [title stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet] ?: @"";
    _titleIsAutomatic = trimmed.length == 0;
    _title = [trimmed.length > 0 ? trimmed : ISHLLMSessionTitleFromMessages(_messages) copy];
    ISHLLMUpdateSessionEntry(_sessionID, @{@"title": _title ?: @"", @"titleIsCustom": @(!_titleIsAutomatic)});
    [self notifyMetadata];
}

- (void)setSystemPrompt:(NSString *)systemPrompt {
    _systemPrompt = [[systemPrompt stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet] ?: @"" copy];
    ISHLLMUpdateSessionEntry(_sessionID, @{@"system": _systemPrompt});
    [self notifyMetadata];
}

- (NSString *)homeDirectory {
    return ISHLLMAgentManager.shared.guestHomeDirectory;
}

- (void)setWorkingDirectory:(NSString *)path {
    if (path.length == 0) {
        _toolContext.workingDirectory = [self homeDirectory];
        ISHLLMUpdateSessionEntry(_sessionID, @{@"workingDirectory": @""});
        [self appendLocalRole:@"assistant" content:[NSString stringWithFormat:@"Working directory: %@ (the home directory).", [self homeDirectory] ?: @"the home directory"]];
    } else {
        _toolContext.workingDirectory = path;
        ISHLLMUpdateSessionEntry(_sessionID, @{@"workingDirectory": path});
        [self appendLocalRole:@"assistant" content:[NSString stringWithFormat:@"Working directory: %@", path]];
    }
    [self notifyMetadata];
}

// A clean slate: the model starts over, so it must read again before
// writing, and chat-wide approvals are withdrawn.
- (void)clearMessages {
    [_activeTask cancel];
    if (_activeStreamFD > 0)
        shutdown(_activeStreamFD, SHUT_RDWR);
    _cancelled = NO;
    [self setSending:NO];
    [_messages removeAllObjects];
    _autoRunCommandsThisChat = NO;
    _autoRunCommandsThisReply = NO;
    [_commandDecisionsThisReply removeAllObjects];
    [_toolContext forgetReads];
    ISHLLMAgentManager.shared.guestEnvironmentNote = nil; // re-probe the guest on the next tool-enabled reply
    _lastOutcome = nil;
    [self saveTranscript];
    [self notifyMessages];
    [self notifyMetadata];
}

#pragma mark - Streaming into a message

- (void)updateStreamingPhaseForContent:(NSString *)content {
    BOOL open = NO;
    NSMutableArray<NSString *> *thoughts = [NSMutableArray array];
    (void) ISHLLMSplitThinkingFromContent(content ?: @"", thoughts, &open);
    ISHLLMAgentPhase phase = open ? ISHLLMAgentPhaseThinking : ISHLLMAgentPhaseWriting;
    if (phase != _phase)
        [self setPhase:phase detail:nil];
}

- (void)setStreamingAssistantContent:(NSString *)content atMessageIndex:(NSUInteger)index {
    if (index >= _messages.count)
        return;
    NSMutableDictionary<NSString *, id> *message = [_messages[index] mutableCopy];
    message[@"content"] = content ?: @"";
    _messages[index] = message;
    [self updateStreamingPhaseForContent:message[@"content"]];
    [self notifyStreamingAtIndex:index];
}

- (void)appendStreamingAssistantChunk:(NSString *)chunk toMessageAtIndex:(NSUInteger)index {
    if (index >= _messages.count || chunk.length == 0)
        return;
    NSMutableDictionary<NSString *, id> *message = [_messages[index] mutableCopy];
    NSString *content = ISHLLMStreamingAssistantContent([ISHLLMStringValue(message, @"content") ?: @"" stringByAppendingString:chunk]);
    message[@"content"] = content;
    _messages[index] = message;
    [self updateStreamingPhaseForContent:content];
    [self notifyStreamingAtIndex:index];
}

- (void)finalizeStreamingAssistantMessageAtIndex:(NSUInteger)index {
    if (index >= _messages.count)
        return;
    NSMutableDictionary<NSString *, id> *message = [_messages[index] mutableCopy];
    NSString *finalized = ISHLLMSanitizedAssistantContent(ISHLLMStringValue(message, @"content") ?: @"");
    if ([finalized isEqualToString:message[@"content"]])
        return;
    message[@"content"] = finalized;
    _messages[index] = message;
    [self notifyStreamingAtIndex:index];
}

- (NSUInteger)addStreamingPlaceholder {
    [_messages addObject:@{@"role": @"assistant", @"content": @""}];
    [self notifyMessages];
    return _messages.count - 1;
}

- (void)removeMessageAtIndex:(NSUInteger)index {
    if (index < _messages.count)
        [_messages removeObjectAtIndex:index];
}

#pragma mark - Context

- (NSInteger)contextWindowTokens {
    NSInteger setting = ISHLLMContextWindowSetting();
    return setting > 0 ? setting : _knownContextWindowTokens;
}

- (NSInteger)effectiveContextWindowTokens {
    NSInteger window = [self contextWindowTokens];
    return window > 0 ? window : kISHLLMFallbackContextWindowTokens;
}

- (NSInteger)estimatedContextTokens {
    __block NSInteger tokens = 0;
    [self scoped:^{
        tokens = ISHLLMEstimateMessagesTokenCount([self providerMessages]);
    }];
    return tokens;
}

- (NSInteger)toolResultContextBudgetTokens {
    if ([self contextWindowTokens] > 0)
        return MAX(1024, (NSInteger) ([self contextWindowTokens] * kISHLLMToolContextBudgetFraction));
    return kISHLLMToolContextDefaultBudgetTokens;
}

- (NSString *)contextWindowProbeKey {
    return [NSString stringWithFormat:@"%@|%@", [self modelName], ISHLLMModelsEndpoint()];
}

- (void)probeContextWindowIfNeeded {
    if (ISHLLMCurrentBackend() == AOKLLMBackendAppleFoundationModels)
        return;
    NSString *probeKey = [self contextWindowProbeKey];
    if (_contextWindowProbeInFlight || [probeKey isEqualToString:_knownContextWindowProbeKey])
        return;
    _contextWindowProbeInFlight = YES;
    NSString *model = [self modelName];
    ISHLLMFetchModelsDataAsync(^(NSData *data, NSInteger statusCode, NSError *error) {
        (void) statusCode;
        NSInteger tokens = error == nil ? ISHLLMContextWindowFromModelsResponse(data, model) : 0;
        dispatch_async(dispatch_get_main_queue(), ^{
            self->_contextWindowProbeInFlight = NO;
            self->_knownContextWindowProbeKey = probeKey; // tried, even if 0: don't hammer a provider that doesn't say
            self->_knownContextWindowTokens = tokens;
            [self notifyStatus];
        });
    });
}

- (BOOL)shouldCompactBeforeSending {
    if (ISHLLMUsesGeminiAPI() || ISHLLMCurrentBackend() == AOKLLMBackendAppleFoundationModels)
        return NO;
    return ISHLLMEstimateMessagesTokenCount([self providerMessages]) > (NSInteger) ([self effectiveContextWindowTokens] * 0.75);
}

- (NSString *)toolsSummaryText {
    __block NSString *text = nil;
    [self scoped:^{
        if (!UserPreferences.shared.llmToolsEnabled) {
            text = @"Tools are off: the model can only talk. Turn on Tools in LLM Settings to let it read, search and edit files and run commands.";
            return;
        }
        if (ISHLLMCurrentBackend() == AOKLLMBackendAppleFoundationModels) {
            text = @"Tools: shell commands only (the on-device model's context is too small for the file tools).";
            return;
        }
        if (ISHLLMUsesGeminiAPI()) {
            text = @"Tools are not available with the Gemini API.";
            return;
        }
        NSString *where = self->_toolContext.workingDirectory.length > 0 ? self->_toolContext.workingDirectory : @"your home directory";
        NSUInteger mcpServers = 0;
        for (NSDictionary *server in ISHLLMMCPServers())
            mcpServers += [server[@"enabled"] boolValue];
        NSString *mcp = mcpServers == 0 ? @"" : [NSString stringWithFormat:@" · MCP (%lu server%@): %@", (unsigned long) mcpServers,
                                                 mcpServers == 1 ? @"" : @"s", ISHLLMPermissionActionTitle(ISHLLMCategoryAction(ISHLLMToolCategoryMCP))];
        text = [NSString stringWithFormat:@"Tools: files and shell, working in %@.\nReading: %@ · Edits: %@ · Commands: %@%@\n%@/new starts a chat and /chats lists them; /agents shows the ones working; /compact summarizes; /undo reverts the last file change; /mcp lists MCP servers.",
                where,
                ISHLLMPermissionActionTitle(ISHLLMCategoryAction(ISHLLMToolCategoryRead)),
                ISHLLMPermissionActionTitle(ISHLLMCategoryAction(ISHLLMToolCategoryEdit)),
                ISHLLMPermissionActionTitle(ISHLLMCategoryAction(ISHLLMToolCategoryShell)), mcp,
                self->_parentSessionID.length == 0 ? @"The model can start sub-agents for independent tasks; they run in the background.\n" : @""];
    }];
    return text;
}

#pragma mark - Sending

// A reply's destination is fixed when it starts: a switch or a Settings
// edit meanwhile applies from the next one.
- (void)runEntryPoint:(dispatch_block_t)block {
    if (_requestDestination == nil)
        _requestDestination = [self destination];
    [self scoped:block];
    if (!_sending)
        _requestDestination = nil;
}

- (void)sendPrompt:(NSString *)prompt {
    if (prompt.length == 0)
        return;
    if ([self isBusy]) {
        [_queuedPrompts addObject:[prompt copy]];
        [self notifyStatus];
        return;
    }
    [self markSeen];
    [self runEntryPoint:^{
        [self sendPromptInScope:prompt];
    }];
}

- (void)sendPromptInScope:(NSString *)prompt {
    if ([prompt isEqualToString:@"/compact"]) {
        [self compact];
        return;
    }
    if ([prompt isEqualToString:@"/models"]) {
        [self appendLocalRole:@"user" content:prompt];
        [self queryModels];
        return;
    }
    if ([prompt isEqualToString:@"/model"] || [prompt hasPrefix:@"/model "]) {
        [self appendLocalRole:@"user" content:prompt];
        [self setAndLoadModelFromCommand:prompt];
        return;
    }
    if (ISHLLMCurrentBackend() == AOKLLMBackendAppleFoundationModels) {
        [self appendRole:@"user" content:prompt];
        [self sendPromptToAppleFoundationModels:prompt];
        return;
    }
    NSString *model = [self modelName];
    if (model.length == 0) {
        [self appendRole:@"assistant" content:@"Set an LLM model in Settings before sending a prompt."];
        return;
    }
    NSString *apiKey = [self apiKey];
    if (ISHLLMProviderRequiresAPIKey() && apiKey.length == 0) {
        [self appendRole:@"assistant" content:ISHLLMMissingAPIKeyMessage()];
        return;
    }
    [self appendRole:@"user" content:prompt];
    [self probeContextWindowIfNeeded];
    [self setSending:YES];
    // A conversation about to outgrow the model's window is summarised first,
    // as OpenCode does, rather than failing or losing its start.
    if ([self shouldCompactBeforeSending]) {
        [self summarizeConversationKeepingTrailing:1 then:^(__unused BOOL ok) {
            if (self->_cancelled) {
                self->_cancelled = NO;
                [self setSending:NO];
                return;
            }
            [self dispatchPromptWithModel:model apiKey:apiKey];
        }];
        return;
    }
    [self dispatchPromptWithModel:model apiKey:apiKey];
}

- (BOOL)toolsEnabled {
    return UserPreferences.shared.llmToolsEnabled;
}

- (void)dispatchPromptWithModel:(NSString *)model apiKey:(NSString *)apiKey {
    // Anthropic's Messages API always goes through the tool loop, with or
    // without tools: it is where the Messages translation lives.
    if (ISHLLMUsesAnthropicAPI() && ![self toolsEnabled]) {
        [self runToolLoopRound:0 model:model apiKey:apiKey];
        return;
    }
    if (!ISHLLMUsesGeminiAPI() && [self toolsEnabled]) {
        _autoRunCommandsThisReply = NO; // a new prompt re-arms confirmation for this reply
        [self setPhase:ISHLLMAgentPhasePreparing detail:nil];
        [self prepareGuestEnvironmentNoteThen:^{
            if (ISHLLMMCPServers().count > 0)
                [self setPhase:ISHLLMAgentPhasePreparing detail:@"connecting MCP servers"];
            // MCP servers' tools join the built-in ones; a server that could
            // not connect is named in the chat and left out.
            [ISHLLMMCPManager.shared prepareWithCompletion:^(NSArray<NSString *> *problems) {
                [self scoped:^{
                    if (problems.count > 0)
                        [self appendLocalRole:@"assistant" content:[problems componentsJoinedByString:@"\n"]];
                    [self runToolLoopRound:0 model:model apiKey:apiKey];
                }];
            }];
        }];
        return;
    }
    if (ISHLLMUsesGeminiAPI()) {
        [self sendGeminiRequest];
        return;
    }
    [self sendStreamingChatWithModel:model apiKey:apiKey];
}

- (void)sendGeminiRequest {
    NSURL *geminiURL = [NSURL URLWithString:ISHLLMGeminiGenerateEndpoint()];
    if (geminiURL == nil) {
        [self appendRole:@"assistant" content:@"Invalid Gemini server URL."];
        [self setSending:NO];
        return;
    }
    NSMutableArray<NSDictionary<NSString *, id> *> *contents = [NSMutableArray array];
    for (NSDictionary<NSString *, id> *message in [self providerMessages]) {
        NSString *messageRole = ISHLLMStringValue(message, @"role");
        if ([messageRole isEqualToString:@"system"])
            continue; // carried in system_instruction below, not as a turn
        NSString *role = [messageRole isEqualToString:@"assistant"] ? @"model" : @"user";
        NSString *content = ISHLLMStringValue(message, @"content") ?: @"";
        if (content.length > 0)
            [contents addObject:@{@"role": role, @"parts": @[@{@"text": content}]}];
    }
    NSMutableDictionary *body = [@{@"contents": contents} mutableCopy];
    if (_systemPrompt.length > 0)
        body[@"system_instruction"] = @{@"parts": @[@{@"text": _systemPrompt}]};
    NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:geminiURL];
    request.HTTPMethod = @"POST";
    [request setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
    request.HTTPBody = [NSJSONSerialization dataWithJSONObject:body options:0 error:nil];
    _activeTask = [NSURLSession.sharedSession dataTaskWithRequest:request completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
        [self onMain:^{
            [self handleGeminiResponseData:data response:response error:error];
        }];
    }];
    [_activeTask resume];
}

// A chat without tools: the reply streams straight into its bubble.
- (void)sendStreamingChatWithModel:(NSString *)model apiKey:(NSString *)apiKey {
    NSURL *url = [NSURL URLWithString:ISHLLMChatEndpoint()];
    if (url == nil) {
        [self appendRole:@"assistant" content:@"Invalid LLM server URL."];
        [self setSending:NO];
        return;
    }
    NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:url];
    request.HTTPMethod = @"POST";
    [request setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
    ISHLLMApplyAuthHeaders(request, apiKey);
    NSArray *messages = [self providerMessages];
    request.HTTPBody = [NSJSONSerialization dataWithJSONObject:@{@"model": model, @"messages": messages, @"stream": @YES, @"stop": @[@"<file_sep>"]} options:0 error:nil];

    if ([url.scheme.lowercaseString isEqualToString:@"http"]) {
        NSData *requestBody = request.HTTPBody;
        NSData *fallbackRequestBody = [NSJSONSerialization dataWithJSONObject:@{@"model": model, @"messages": messages, @"stream": @NO, @"stop": @[@"<file_sep>"]} options:0 error:nil];
        NSUInteger streamingIndex = [self addStreamingPlaceholder];
        [self inBackground:^{
            NSInteger statusCode = 0;
            NSError *directError = nil;
            __block BOOL receivedChunk = NO;
            BOOL streamed = ISHLLMDirectHTTPPostStreaming(url, requestBody, apiKey, ^(NSString *chunk) {
                receivedChunk = YES;
                [self onMain:^{
                    [self appendStreamingAssistantChunk:chunk toMessageAtIndex:streamingIndex];
                }];
            }, &self->_activeStreamFD, &statusCode, &directError);
            if (self->_cancelled) {
                [self onMain:^{
                    self->_cancelled = NO;
                    [self finalizeStreamingAssistantMessageAtIndex:streamingIndex];
                    self->_lastOutcome = @"Stopped";
                    [self setSending:NO];
                    [self saveTranscript];
                }];
                return;
            }
            NSData *responseBody = nil;
            if (!streamed || !receivedChunk)
                responseBody = ISHLLMDirectHTTPPost(url, fallbackRequestBody, apiKey, &statusCode, &directError);
            NSHTTPURLResponse *directResponse = statusCode > 0
                ? [[NSHTTPURLResponse alloc] initWithURL:url statusCode:statusCode HTTPVersion:@"HTTP/1.1" headerFields:nil] : nil;
            [self onMain:^{
                if (streamed && receivedChunk && directError == nil) {
                    [self finalizeStreamingAssistantMessageAtIndex:streamingIndex];
                    [self setSending:NO];
                    [self saveTranscript];
                    return;
                }
                [self removeMessageAtIndex:streamingIndex];
                [self handleLLMResponseData:responseBody response:directResponse error:directError];
            }];
        }];
        return;
    }

    // https: Server-Sent Events on an NSURLSession data task. If none arrive
    // (a non-200, or a server that ignores "stream": true), the buffered
    // response goes to the ordinary handler instead.
    NSUInteger streamingIndex = [self addStreamingPlaceholder];
    ISHLLMStreamingResponseDelegate *streamDelegate = [ISHLLMStreamingResponseDelegate new];
    streamDelegate.chunkHandler = ^(NSString *chunk) {
        [self onMain:^{
            [self appendStreamingAssistantChunk:chunk toMessageAtIndex:streamingIndex];
        }];
    };
    streamDelegate.completionHandler = ^(BOOL receivedChunks, NSData *responseBody, NSInteger statusCode, NSError *error) {
        [self onMain:^{
            self->_activeTask = nil;
            if (receivedChunks) {
                BOOL userStopped = self->_cancelled;
                self->_cancelled = NO;
                [self finalizeStreamingAssistantMessageAtIndex:streamingIndex];
                if (error != nil && !userStopped)
                    [self appendRole:@"assistant" content:[NSString stringWithFormat:@"(stream interrupted: %@)", error.localizedDescription]];
                if (userStopped)
                    self->_lastOutcome = @"Stopped";
                [self setSending:NO];
                [self saveTranscript];
                return;
            }
            [self removeMessageAtIndex:streamingIndex];
            if (error == nil && statusCode >= 400 && !self->_cancelled) {
                [self retryWithoutStreamingRequest:request];
                return;
            }
            NSHTTPURLResponse *streamResponse = statusCode > 0
                ? [[NSHTTPURLResponse alloc] initWithURL:url statusCode:statusCode HTTPVersion:@"HTTP/1.1" headerFields:nil] : nil;
            [self handleLLMResponseData:responseBody response:streamResponse error:error];
        }];
    };
    NSURLSession *streamSession = [NSURLSession sessionWithConfiguration:NSURLSessionConfiguration.defaultSessionConfiguration
                                                                delegate:streamDelegate delegateQueue:nil];
    _activeTask = [streamSession dataTaskWithRequest:request];
    [_activeTask resume];
}

- (void)retryWithoutStreamingRequest:(NSURLRequest *)streamingRequest {
    NSMutableURLRequest *request = [streamingRequest mutableCopy];
    NSMutableDictionary *body = [[NSJSONSerialization JSONObjectWithData:streamingRequest.HTTPBody ?: NSData.data options:0 error:nil] mutableCopy];
    if (![body isKindOfClass:NSMutableDictionary.class]) {
        [self appendRole:@"assistant" content:@"Could not encode the request."];
        [self setSending:NO];
        return;
    }
    body[@"stream"] = @NO;
    request.HTTPBody = [NSJSONSerialization dataWithJSONObject:body options:0 error:nil];
    [self setPhase:ISHLLMAgentPhaseContacting detail:@"retrying without streaming"];
    _activeTask = [NSURLSession.sharedSession dataTaskWithRequest:request completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
        [self onMain:^{
            [self handleLLMResponseData:data response:response error:error];
        }];
    }];
    [_activeTask resume];
}

- (BOOL)handleUserCancelledError:(NSError *)error {
    if (error == nil || !(error.domain == NSURLErrorDomain && error.code == NSURLErrorCancelled) || !_cancelled)
        return NO;
    _cancelled = NO;
    [self appendRole:@"assistant" content:@"(stopped)"];
    return YES;
}

- (void)handleLLMResponseData:(NSData *)data response:(NSURLResponse *)response error:(NSError *)error {
    _activeTask = nil;
    if ([self handleUserCancelledError:error]) {
        [self setSending:NO];
        return;
    }
    if (error != nil) {
        [self appendRole:@"assistant" content:[NSString stringWithFormat:@"Request failed: %@", error.localizedDescription]];
        [self setSending:NO];
        return;
    }
    NSHTTPURLResponse *http = [response isKindOfClass:NSHTTPURLResponse.class] ? (NSHTTPURLResponse *) response : nil;
    id json = data.length > 0 ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
    NSString *content = nil;
    if ([json isKindOfClass:NSDictionary.class]) {
        NSDictionary *dict = json;
        NSArray *choices = [dict[@"choices"] isKindOfClass:NSArray.class] ? dict[@"choices"] : nil;
        NSDictionary *choice = choices.count > 0 && [choices[0] isKindOfClass:NSDictionary.class] ? choices[0] : nil;
        NSDictionary *message = [choice[@"message"] isKindOfClass:NSDictionary.class] ? choice[@"message"] : nil;
        content = ISHLLMSanitizedAssistantContent(ISHLLMStringValue(message, @"content") ?: @"");
        if (content.length == 0 && [dict[@"error"] isKindOfClass:NSDictionary.class])
            content = ISHLLMStringValue(dict[@"error"], @"message");
    }
    if (content.length == 0) {
        NSString *raw = data.length > 0 ? ([[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] ?: @"") : @"";
        content = [NSString stringWithFormat:@"Unexpected response%@%@", http != nil ? [NSString stringWithFormat:@" (%ld)", (long) http.statusCode] : @"", raw.length > 0 ? [@": " stringByAppendingString:raw] : @"."];
    }
    [self appendRole:@"assistant" content:ISHLLMSanitizedAssistantContent(content)];
    [self setSending:NO];
}

- (void)handleGeminiResponseData:(NSData *)data response:(NSURLResponse *)response error:(NSError *)error {
    _activeTask = nil;
    if ([self handleUserCancelledError:error]) {
        [self setSending:NO];
        return;
    }
    if (error != nil) {
        [self appendRole:@"assistant" content:[NSString stringWithFormat:@"Request failed: %@", error.localizedDescription]];
        [self setSending:NO];
        return;
    }
    NSHTTPURLResponse *http = [response isKindOfClass:NSHTTPURLResponse.class] ? (NSHTTPURLResponse *) response : nil;
    id json = data.length > 0 ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
    NSString *content = nil;
    if ([json isKindOfClass:NSDictionary.class]) {
        NSDictionary *dict = json;
        NSArray *candidates = [dict[@"candidates"] isKindOfClass:NSArray.class] ? dict[@"candidates"] : nil;
        NSDictionary *candidate = candidates.count > 0 && [candidates[0] isKindOfClass:NSDictionary.class] ? candidates[0] : nil;
        NSDictionary *candidateContent = [candidate[@"content"] isKindOfClass:NSDictionary.class] ? candidate[@"content"] : nil;
        NSArray *parts = [candidateContent[@"parts"] isKindOfClass:NSArray.class] ? candidateContent[@"parts"] : nil;
        NSMutableString *text = [NSMutableString string];
        for (id part in parts) {
            if ([part isKindOfClass:NSDictionary.class] && [part[@"text"] isKindOfClass:NSString.class])
                [text appendString:part[@"text"]];
        }
        content = ISHLLMSanitizedAssistantContent(text);
        if (content.length == 0 && [dict[@"error"] isKindOfClass:NSDictionary.class])
            content = ISHLLMStringValue(dict[@"error"], @"message");
    }
    if (content.length == 0) {
        NSString *raw = data.length > 0 ? ([[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] ?: @"") : @"";
        content = [NSString stringWithFormat:@"Unexpected Gemini response%@%@", http != nil ? [NSString stringWithFormat:@" (%ld)", (long) http.statusCode] : @"", raw.length > 0 ? [@": " stringByAppendingString:raw] : @"."];
    }
    [self appendRole:@"assistant" content:ISHLLMSanitizedAssistantContent(content)];
    [self setSending:NO];
}

#pragma mark - /models and /model

- (void)queryModels {
    if ([self isBusy])
        return;
    [self runEntryPoint:^{
        if (ISHLLMCurrentBackend() == AOKLLMBackendAppleFoundationModels) {
            NSString *model = [self modelName];
            [self appendLocalRole:@"assistant" content:[NSString stringWithFormat:@"Current on-device model: %@\n%@", model.length > 0 ? model : @"system-language-model", ISHLLMAppleFoundationModelsUnavailableMessage()]];
            return;
        }
        NSURL *url = [NSURL URLWithString:ISHLLMModelsEndpoint()];
        if (url == nil) {
            [self appendLocalRole:@"assistant" content:@"Invalid models URL."];
            return;
        }
        NSString *apiKey = [self apiKey];
        if (ISHLLMProviderRequiresAPIKey() && apiKey.length == 0) {
            [self appendLocalRole:@"assistant" content:ISHLLMMissingAPIKeyMessage()];
            return;
        }
        [self setSending:YES];
        NSString *endpoint = ISHLLMModelsEndpoint();
        if ([url.scheme.lowercaseString isEqualToString:@"http"]) {
            [self inBackground:^{
                NSInteger statusCode = 0;
                NSError *error = nil;
                NSData *data = ISHLLMDirectHTTPGet(url, apiKey, &statusCode, &error);
                [self onMain:^{
                    [self appendModelListFromData:data statusCode:statusCode error:error endpoint:endpoint];
                }];
            }];
            return;
        }
        NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:url];
        ISHLLMApplyAuthHeaders(request, apiKey);
        self->_auxiliaryTask = [NSURLSession.sharedSession dataTaskWithRequest:request completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
            NSHTTPURLResponse *http = [response isKindOfClass:NSHTTPURLResponse.class] ? (NSHTTPURLResponse *) response : nil;
            [self onMain:^{
                [self appendModelListFromData:data statusCode:http.statusCode error:error endpoint:endpoint];
            }];
        }];
        [self->_auxiliaryTask resume];
    }];
}

- (void)appendModelListFromData:(NSData *)data statusCode:(NSInteger)statusCode error:(NSError *)error endpoint:(NSString *)endpoint {
    _auxiliaryTask = nil;
    _lastOutcome = @"Ready";
    [self setSending:NO];
    if (error != nil) {
        [self appendLocalRole:@"assistant" content:[NSString stringWithFormat:@"Model query failed: %@", error.localizedDescription]];
        return;
    }
    NSArray<NSString *> *models = ISHLLMModelIdentifiersFromResponseData(data);
    if (models.count == 0) {
        NSString *raw = data.length > 0 ? ([[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] ?: @"") : @"";
        NSString *message = statusCode > 0 ? [NSString stringWithFormat:@"No models found. HTTP %ld", (long) statusCode] : @"No models found.";
        if (raw.length > 0)
            message = [message stringByAppendingFormat:@"\n%@", raw.length > 480 ? [raw substringToIndex:480] : raw];
        [self appendLocalRole:@"assistant" content:message];
        return;
    }
    NSUInteger limit = MIN(models.count, (NSUInteger) 80);
    NSMutableString *message = [NSMutableString stringWithFormat:@"%lu models returned by %@:", (unsigned long) models.count, endpoint];
    for (NSUInteger i = 0; i < limit; i++)
        [message appendFormat:@"\n- %@", models[i]];
    if (models.count > limit)
        [message appendFormat:@"\nShowing first %lu of %lu.", (unsigned long) limit, (unsigned long) models.count];
    [self appendLocalRole:@"assistant" content:message];
}

// /model <name>: sets this chat's destination's model (a destination
// setting, as the Settings row is) and sends it a one-token warm-up.
- (void)setAndLoadModelFromCommand:(NSString *)command {
    NSString *model = [[command substringFromIndex:@"/model".length] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if (model.length == 0) {
        NSString *current = [self modelName].length > 0 ? [self modelName] : @"not set";
        [self appendLocalRole:@"assistant" content:[NSString stringWithFormat:@"Current model: %@\nUsage: /model <model-name>", current]];
        return;
    }
    NSMutableDictionary<NSString *, NSString *> *destination = [[self destination] mutableCopy];
    destination[kISHLLMDestinationModel] = model;
    ISHLLMSaveDestination(destination);
    _requestDestination = destination;
    [self notifyMetadata];
    [self scoped:^{
        if (ISHLLMCurrentBackend() == AOKLLMBackendAppleFoundationModels) {
            [self appendLocalRole:@"assistant" content:[NSString stringWithFormat:@"Model set to %@. Apple Foundation Models uses the system on-device model when available. %@", model, ISHLLMAppleFoundationModelsUnavailableMessage()]];
            return;
        }
        NSString *apiKey = [self apiKey];
        if (ISHLLMProviderRequiresAPIKey() && apiKey.length == 0) {
            [self appendLocalRole:@"assistant" content:[NSString stringWithFormat:@"Model set to %@. %@", model, ISHLLMMissingAPIKeyMessage()]];
            return;
        }
        NSURL *url = ISHLLMProbeURL();
        if (url == nil) {
            [self appendLocalRole:@"assistant" content:[NSString stringWithFormat:@"Model set to %@, but the provider URL is invalid.", model]];
            return;
        }
        NSData *bodyData = [NSJSONSerialization dataWithJSONObject:ISHLLMProbeBody(model, @"Reply with ok.", 1) options:0 error:nil];
        [self setSending:YES];
        if ([url.scheme.lowercaseString isEqualToString:@"http"]) {
            [self inBackground:^{
                NSInteger statusCode = 0;
                NSError *error = nil;
                NSData *data = ISHLLMDirectHTTPPost(url, bodyData, apiKey, &statusCode, &error);
                [self onMain:^{
                    [self appendModelLoadResultWithModel:model data:data statusCode:statusCode error:error];
                }];
            }];
            return;
        }
        NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:url];
        request.HTTPMethod = @"POST";
        [request setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
        ISHLLMApplyAuthHeaders(request, apiKey);
        request.HTTPBody = bodyData;
        self->_auxiliaryTask = [NSURLSession.sharedSession dataTaskWithRequest:request completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
            NSHTTPURLResponse *http = [response isKindOfClass:NSHTTPURLResponse.class] ? (NSHTTPURLResponse *) response : nil;
            [self onMain:^{
                [self appendModelLoadResultWithModel:model data:data statusCode:http.statusCode error:error];
            }];
        }];
        [self->_auxiliaryTask resume];
    }];
}

- (void)appendModelLoadResultWithModel:(NSString *)model data:(NSData *)data statusCode:(NSInteger)statusCode error:(NSError *)error {
    _auxiliaryTask = nil;
    _lastOutcome = @"Ready";
    [self setSending:NO];
    if (error != nil) {
        [self appendLocalRole:@"assistant" content:[NSString stringWithFormat:@"Model set to %@, but load failed: %@", model, error.localizedDescription]];
        return;
    }
    if (statusCode >= 200 && statusCode < 300) {
        [self appendLocalRole:@"assistant" content:[NSString stringWithFormat:@"Model set to %@. Provider accepted a warm-up request.", model]];
        return;
    }
    NSString *raw = data.length > 0 ? ([[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] ?: @"") : @"";
    NSString *message = [NSString stringWithFormat:@"Model set to %@, but provider returned HTTP %ld.", model, (long) statusCode];
    if (raw.length > 0)
        message = [message stringByAppendingFormat:@"\n%@", raw.length > 480 ? [raw substringToIndex:480] : raw];
    [self appendLocalRole:@"assistant" content:message];
}

#pragma mark - Apple Foundation Models

#if __has_include("libiSH_AOKApp-Swift.h")
// The bridge's handler is process-wide: reinstalled for every request, so
// the tool calls of the chat asking now reach it.
- (void)installAppleFoundationModelsShellHandler {
    __weak typeof(self) weakSelf = self;
    AOKFoundationModelsBridge.shellCommandHandler = ^(NSString *command, void (^handlerCompletion)(NSString *)) {
        dispatch_async(dispatch_get_main_queue(), ^{
            typeof(self) self = weakSelf;
            if (self == nil) {
                handlerCompletion(@"The chat closed before this command could run.");
                return;
            }
            [self runAppleFoundationModelsShellCommand:command completion:handlerCompletion];
        });
    };
}

// FoundationModels has been seen to call a tool twice for one logical call;
// _commandDecisionsThisReply remembers this reply's answer per command.
- (void)runAppleFoundationModelsShellCommand:(NSString *)command completion:(void (^)(NSString *))completion {
    void (^recordAndComplete)(NSString *, NSString *) = ^(NSString *resultText, NSString *summary) {
        [self->_messages addObject:@{
            @"role": @"tool",
            @"name": @"run_shell",
            @"content": resultText ?: @"",
            @"summary": summary.length > 0 ? summary : (resultText ?: @""),
        }];
        [self notifyMessages];
        [self saveTranscript];
        completion(resultText ?: @"");
    };
    NSDictionary *toolCall = @{@"id": NSUUID.UUID.UUIDString, @"function": @{@"name": @"run_shell", @"arguments": @{@"command": command ?: @""}}};
    ISHLLMToolInvocation *invocation = [ISHLLMToolInvocation invocationWithToolCall:toolCall context:_toolContext];
    if (command.length > 0) {
        NSNumber *priorDecision = _commandDecisionsThisReply[command];
        if (priorDecision != nil) {
            if (priorDecision.integerValue == ISHLLMToolRunDecline) {
                recordAndComplete(@"The user declined to run this command.", @"declined by user (repeat request)");
            } else {
                [self setRunningTool:invocation];
                ISHLLMRunToolInvocation(invocation, _toolContext, recordAndComplete);
            }
            return;
        }
    }
    [self performToolInvocation:invocation decision:^(BOOL approved) {
        if (command.length > 0)
            self->_commandDecisionsThisReply[command] = @(approved ? ISHLLMToolRunOnce : ISHLLMToolRunDecline);
    } completion:recordAndComplete];
}
#endif

// One flat prompt of recent turns: this backend keeps no history between
// calls and has a ~4K-token window.
- (NSString *)appleFoundationModelsPromptWithHistory {
    NSMutableArray<NSString *> *turns = [NSMutableArray array];
    for (NSDictionary<NSString *, id> *message in _messages) {
        if ([self messageIsLocalOnly:message])
            continue;
        NSString *role = ISHLLMStringValue(message, @"role");
        NSString *content = ISHLLMStringValue(message, @"content");
        if (content.length == 0 || [role isEqualToString:@"tool"] || [role isEqualToString:@"system"])
            continue;
        [turns addObject:[NSString stringWithFormat:@"%@: %@", [role isEqualToString:@"assistant"] ? @"Assistant" : @"User", content]];
    }
    NSUInteger budget = 6000;
    NSMutableArray<NSString *> *kept = [NSMutableArray array];
    NSUInteger total = 0;
    for (NSString *turn in turns.reverseObjectEnumerator) {
        total += turn.length;
        if (total > budget && kept.count > 0)
            break;
        [kept insertObject:turn atIndex:0];
    }
    if (_systemPrompt.length > 0)
        [kept insertObject:[NSString stringWithFormat:@"Instructions: %@", _systemPrompt] atIndex:0];
    return [kept componentsJoinedByString:@"\n\n"];
}

- (void)sendPromptToAppleFoundationModels:(NSString *)prompt {
    if (!ISHLLMFoundationModelsReady()) {
        [self appendRole:@"assistant" content:ISHLLMAppleFoundationModelsUnavailableMessage()];
        return;
    }
#if __has_include("libiSH_AOKApp-Swift.h")
    BOOL toolsEnabled = [self toolsEnabled];
    if (toolsEnabled) {
        _autoRunCommandsThisReply = NO;
        [_commandDecisionsThisReply removeAllObjects];
        [self installAppleFoundationModelsShellHandler];
    }
    void (^startRequest)(void) = ^{
        NSString *instructions = [@"The prompt is this conversation so far, formatted as alternating \"User:\"/\"Assistant:\" turns. Continue it naturally as the Assistant, responding only to the latest User message -- the earlier turns are context, not something to repeat back."
            stringByAppendingString:toolsEnabled ? [@" " stringByAppendingString:ISHLLMToolSystemNote(ISHLLMAgentManager.shared.guestEnvironmentNote, self->_toolContext.workingDirectory, NO)] : @""];
        NSString *promptWithHistory = [self appleFoundationModelsPromptWithHistory];
        [self setSending:YES];
        NSUInteger streamingIndex = [self addStreamingPlaceholder];
        [AOKFoundationModelsBridge streamResponseToPrompt:promptWithHistory.length > 0 ? promptWithHistory : prompt
                                              instructions:instructions
                                              toolsEnabled:toolsEnabled
                                                 onPartial:^(NSString *partial) {
            dispatch_async(dispatch_get_main_queue(), ^{
                [self setStreamingAssistantContent:partial atMessageIndex:streamingIndex];
            });
        }
                                                completion:^(NSString *finalText, NSString *errorMessage) {
            dispatch_async(dispatch_get_main_queue(), ^{
                if (self->_cancelled) {
                    self->_cancelled = NO;
                    self->_lastOutcome = @"Stopped";
                    [self finalizeStreamingAssistantMessageAtIndex:streamingIndex];
                } else if (finalText.length == 0 && errorMessage.length > 0) {
                    [self setStreamingAssistantContent:[NSString stringWithFormat:@"Request failed: %@", errorMessage] atMessageIndex:streamingIndex];
                } else {
                    [self setStreamingAssistantContent:ISHLLMSanitizedAssistantContent(finalText ?: @"") atMessageIndex:streamingIndex];
                }
                [self setSending:NO];
                [self saveTranscript];
            });
        }];
    };
    if (toolsEnabled)
        [self prepareGuestEnvironmentNoteThen:startRequest];
    else
        startRequest();
#else
    [self appendRole:@"assistant" content:ISHLLMAppleFoundationModelsUnavailableMessage()];
#endif
}

#pragma mark - The tool loop

// The distro/tool probe runs once for the guest, in the background, never
// holding up a request; AGENTS.md is read again for every prompt.
- (void)prepareGuestEnvironmentNoteThen:(void (^)(void))continuation {
    ISHLLMAgentManager *manager = ISHLLMAgentManager.shared;
    if (manager.guestEnvironmentNote == nil) {
        manager.guestEnvironmentNote = @""; // in flight
        dispatch_async(ISHLLMGuestCommandQueue(), ^{
            NSString *home = nil;
            NSString *note = ISHLLMDetectGuestEnvironmentNote(&home);
            dispatch_async(dispatch_get_main_queue(), ^{
                if (note.length > 0)
                    manager.guestEnvironmentNote = note;
                if (home.length > 0) {
                    manager.guestHomeDirectory = home;
                    if (self->_toolContext.workingDirectory.length == 0)
                        self->_toolContext.workingDirectory = home;
                }
            });
        });
    }
    [self loadProjectInstructionsThen:continuation];
}

- (void)loadProjectInstructionsThen:(void (^)(void))continuation {
    NSString *workingDirectory = _toolContext.workingDirectory;
    __block BOOL finished = NO;
    void (^finish)(NSString *, NSString *, BOOL) = ^(NSString *text, NSString *source, BOOL loaded) {
        if (finished)
            return;
        finished = YES;
        if (loaded) {
            self->_projectInstructions = text;
            self->_projectInstructionsSource = source;
        }
        [self scoped:continuation];
    };
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSString *source = nil;
        NSString *text = ISHLLMLoadProjectInstructions(workingDirectory, &source);
        dispatch_async(dispatch_get_main_queue(), ^{
            finish(text, source, YES);
        });
    });
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        finish(nil, nil, NO);
    });
}

- (BOOL)offersAgentTools {
    return _parentSessionID.length == 0;
}

- (NSArray<NSDictionary *> *)toolDefinitions {
    NSArray *tools = ISHLLMChatToolDefinitions();
    if ([self offersAgentTools])
        tools = [tools arrayByAddingObjectsFromArray:ISHLLMAgentToolDefinitions()];
    return tools;
}

- (NSString *)systemNoteWithClock:(BOOL)clock {
    NSString *note = clock
        ? ISHLLMToolSystemNote(ISHLLMAgentManager.shared.guestEnvironmentNote, _toolContext.workingDirectory, YES)
        : ISHLLMToolSystemNoteWithoutClock(ISHLLMAgentManager.shared.guestEnvironmentNote, _toolContext.workingDirectory, YES);
    if (_projectInstructions.length > 0)
        note = [note stringByAppendingFormat:@"\n\nProject instructions from %@ -- follow them:\n\n%@", _projectInstructionsSource, _projectInstructions];
    if (_parentSessionID.length > 0)
        note = [note stringByAppendingString:@"\n\nYou are a sub-agent: another agent gave you the task in the first message and will read only your final reply. Do the task, then reply with a complete, self-contained report of what you did and found."];
    return note;
}

- (void)finishWithStop {
    _cancelled = NO;
    _lastOutcome = @"Stopped";
    [self appendRole:@"assistant" content:@"(stopped)"];
    [self setSending:NO];
    [self saveTranscript];
}

- (void)runToolLoopRound:(NSInteger)round model:(NSString *)model apiKey:(NSString *)apiKey {
    if (_cancelled) {
        [self finishWithStop];
        return;
    }
    NSURL *url = [NSURL URLWithString:ISHLLMChatEndpoint()];
    if (url == nil) {
        [self appendRole:@"assistant" content:@"Invalid LLM server URL."];
        [self setSending:NO];
        return;
    }
    if (round >= ISHLLMToolMaxRounds()) {
        [self appendRole:@"assistant" content:[NSString stringWithFormat:@"Stopped after %ld tool calls in a row (adjustable in Settings as \"Tool Call Rounds\"). Send another message to continue.", (long) ISHLLMToolMaxRounds()]];
        [self setSending:NO];
        [self saveTranscript];
        return;
    }
    _round = round;
    // One prompt can run dozens of tool rounds, so the window is checked
    // between rounds too, not only before the prompt is sent.
    if (round == 0)
        _toolLoopCompactedRound = 0;
    if (round > 0 && _toolLoopCompactedRound != round && [self shouldCompactBeforeSending]) {
        _toolLoopCompactedRound = round;
        [self summarizeConversationKeepingTrailing:[self currentToolRoundMessageCount] then:^(__unused BOOL ok) {
            [self runToolLoopRound:round model:model apiKey:apiKey];
        }];
        return;
    }
    [self setPhase:ISHLLMAgentPhaseContacting detail:nil];
    if (ISHLLMUsesAnthropicAPI()) {
        BOOL tools = [self toolsEnabled];
        // Streamed, so a long answer shows as it is written; 32000 leaves
        // room for thinking that a non-streaming request's timeout would not.
        NSDictionary *body = ISHLLMAnthropicRequestBody(model, 32000, [self providerMessages], tools ? [self systemNoteWithClock:NO] : nil,
                                                        ISHLLMClockNote(), tools ? [self toolDefinitions] : nil);
        [self streamRoundToURL:[NSURL URLWithString:ISHLLMAnthropicMessagesEndpoint()] body:body anthropic:YES round:round model:model apiKey:apiKey];
        return;
    }
    NSMutableArray<NSDictionary<NSString *, id> *> *messages = [NSMutableArray array];
    NSString *systemNote = [self systemNoteWithClock:YES];
    if (systemNote.length > 0)
        [messages addObject:@{@"role": @"system", @"content": systemNote}];
    [messages addObjectsFromArray:[self providerMessages]];
    NSDictionary *body = @{
        @"model": model,
        @"messages": messages,
        @"stream": @NO,
        @"tools": [self toolDefinitions],
        @"stop": @[@"<file_sep>"],
    };
    if ([NSJSONSerialization dataWithJSONObject:body options:0 error:nil] == nil) {
        [self appendRole:@"assistant" content:@"Could not encode the request."];
        [self setSending:NO];
        return;
    }
    [self streamRoundToURL:url body:body anthropic:NO round:round model:model apiKey:apiKey];
}

- (NSUInteger)currentToolRoundMessageCount {
    NSArray<NSDictionary<NSString *, id> *> *sent = [self providerMessages];
    for (NSUInteger count = 1; count <= sent.count; count++) {
        NSDictionary *message = sent[sent.count - count];
        if ([message[@"role"] isEqual:@"assistant"] && [message[@"tool_calls"] isKindOfClass:NSArray.class] && [message[@"tool_calls"] count] > 0)
            return count;
        if ([message[@"role"] isEqual:@"user"])
            break;
    }
    return 0;
}

- (void)sendRoundWithoutStreamingToURL:(NSURL *)url body:(NSDictionary *)body anthropic:(BOOL)anthropic
                                 round:(NSInteger)round model:(NSString *)model apiKey:(NSString *)apiKey {
    NSMutableDictionary *plain = [body mutableCopy];
    [plain removeObjectForKey:@"stream"];
    if (!anthropic)
        plain[@"stream"] = @NO;
    NSData *bodyData = [NSJSONSerialization dataWithJSONObject:plain options:0 error:nil];
    [self inBackground:^{
        NSInteger statusCode = 0;
        NSError *error = nil;
        NSData *data = anthropic ? ISHLLMAnthropicPost(plain, apiKey, &statusCode, &error)
                                 : ISHLLMSynchronousChatPost(url, bodyData, apiKey, &statusCode, &error);
        [self onMain:^{
            [self handleToolRoundData:data statusCode:statusCode error:error round:round model:model apiKey:apiKey];
        }];
    }];
}

// One round, streamed into a placeholder bubble; the assembled message then
// takes the same path a non-streaming response does.
- (void)streamRoundToURL:(NSURL *)url body:(NSDictionary *)body anthropic:(BOOL)anthropic
                   round:(NSInteger)round model:(NSString *)model apiKey:(NSString *)apiKey {
    NSMutableDictionary *streamed = [body mutableCopy];
    streamed[@"stream"] = @YES;
    NSData *bodyData = [NSJSONSerialization dataWithJSONObject:streamed options:0 error:nil];
    NSUInteger placeholder = [self addStreamingPlaceholder];

    ISHLLMOpenAIStreamAssembler *openAI = anthropic ? nil : [ISHLLMOpenAIStreamAssembler new];
    ISHLLMAnthropicStreamAssembler *claude = anthropic ? [ISHLLMAnthropicStreamAssembler new] : nil;
    void (^payloadHandler)(NSString *) = ^(NSString *payload) {
        NSString *text = anthropic ? [claude consumePayload:payload] : [openAI consumePayload:payload];
        if (text.length == 0)
            return;
        dispatch_async(dispatch_get_main_queue(), ^{
            [self appendStreamingAssistantChunk:text toMessageAtIndex:placeholder];
        });
    };
    // Runs on the transport's thread, after the last payload.
    void (^finished)(NSData *, NSInteger, NSError *) = ^(NSData *plainBody, NSInteger statusCode, NSError *error) {
        BOOL sawEvents = anthropic ? claude.sawEvents : openAI.sawEvents;
        NSData *assembled = nil;
        if (sawEvents) {
            NSDictionary *response = anthropic ? claude.response : ({
                NSDictionary *message = openAI.responseMessage;
                message[@"error"] != nil ? @{@"error": message[@"error"]} : @{@"choices": @[@{@"message": message}]};
            });
            assembled = [NSJSONSerialization dataWithJSONObject:response options:0 error:nil];
            if (anthropic && !claude.finished && error == nil)
                error = [NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorNetworkConnectionLost
                                        userInfo:@{NSLocalizedDescriptionKey: @"the reply stream ended early"}];
        }
        [self onMain:^{
            self->_activeTask = nil;
            NSString *partial = placeholder < self->_messages.count ? ISHLLMStringValue(self->_messages[placeholder], @"content") : @"";
            [self removeMessageAtIndex:placeholder];
            if (self->_cancelled) {
                // What arrived before Stop stays.
                self->_cancelled = NO;
                self->_lastOutcome = @"Stopped";
                [self appendRole:@"assistant" content:partial.length > 0 ? [partial stringByAppendingString:@"\n\n(stopped)"] : @"(stopped)"];
                [self setSending:NO];
                [self saveTranscript];
                return;
            }
            if (!sawEvents && error == nil && statusCode >= 400) {
                [self notifyMessages];
                [self sendRoundWithoutStreamingToURL:url body:body anthropic:anthropic round:round model:model apiKey:apiKey];
                return;
            }
            [self handleToolRoundData:sawEvents ? assembled : plainBody statusCode:statusCode error:error round:round model:model apiKey:apiKey];
        }];
    };

    NSMutableDictionary<NSString *, NSString *> *extraHeaders = [NSMutableDictionary dictionary];
    if (anthropic && body[@"fallbacks"] != nil)
        extraHeaders[@"anthropic-beta"] = @"server-side-fallback-2026-07-01";
    if ([url.scheme.lowercaseString isEqualToString:@"http"]) {
        [self inBackground:^{
            NSInteger statusCode = 0;
            NSError *error = nil;
            NSMutableData *plainBody = [NSMutableData data];
            ISHLLMDirectHTTPPostStreamingPayloads(url, bodyData, apiKey, extraHeaders, payloadHandler,
                                                  &self->_activeStreamFD, &statusCode, plainBody, &error);
            finished(plainBody, statusCode, error);
        }];
        return;
    }
    NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:url];
    request.HTTPMethod = @"POST";
    [request setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
    [request setValue:@"text/event-stream" forHTTPHeaderField:@"Accept"];
    ISHLLMApplyAuthHeaders(request, apiKey);
    for (NSString *name in extraHeaders)
        [request setValue:extraHeaders[name] forHTTPHeaderField:name];
    request.HTTPBody = bodyData;
    ISHLLMRawStreamDelegate *delegate = [ISHLLMRawStreamDelegate new];
    delegate.payloadHandler = payloadHandler;
    delegate.completionHandler = finished;
    NSURLSessionConfiguration *configuration = NSURLSessionConfiguration.defaultSessionConfiguration;
    // A model can think for minutes before the first byte of its answer.
    configuration.timeoutIntervalForRequest = 600;
    NSURLSession *session = [NSURLSession sessionWithConfiguration:configuration delegate:delegate delegateQueue:nil];
    _activeTask = [session dataTaskWithRequest:request];
    [_activeTask resume];
}

- (void)handleToolRoundData:(NSData *)data statusCode:(NSInteger)statusCode error:(NSError *)error round:(NSInteger)round model:(NSString *)model apiKey:(NSString *)apiKey {
    if (_cancelled) {
        [self finishWithStop];
        return;
    }
    if (error != nil) {
        [self appendRole:@"assistant" content:[NSString stringWithFormat:@"Request failed: %@", error.localizedDescription]];
        [self setSending:NO];
        return;
    }
    id json = data.length > 0 ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
    NSDictionary *dict = [json isKindOfClass:NSDictionary.class] ? json : nil;
    NSArray *choices = [dict[@"choices"] isKindOfClass:NSArray.class] ? dict[@"choices"] : nil;
    NSDictionary *choice = choices.count > 0 && [choices[0] isKindOfClass:NSDictionary.class] ? choices[0] : nil;
    NSDictionary *message = [choice[@"message"] isKindOfClass:NSDictionary.class] ? choice[@"message"] : nil;
    NSString *anthropicNote = nil;
    if (ISHLLMUsesAnthropicAPI() && dict != nil) {
        NSString *anthropicError = nil;
        message = ISHLLMAnthropicMessageFromResponse(dict, &anthropicError, &anthropicNote);
        if (message == nil) {
            [self appendRole:@"assistant" content:anthropicError ?: @"Unexpected response from the Anthropic API."];
            [self setSending:NO];
            [self saveTranscript];
            return;
        }
    }
    if (message == nil) {
        NSString *errorMessage = [dict[@"error"] isKindOfClass:NSDictionary.class] ? ISHLLMStringValue(dict[@"error"], @"message") : nil;
        if (errorMessage.length == 0) {
            NSString *raw = data.length > 0 ? ([[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] ?: @"") : @"";
            errorMessage = [NSString stringWithFormat:@"Unexpected response%@%@",
                statusCode > 0 ? [NSString stringWithFormat:@" (%ld)", (long) statusCode] : @"",
                raw.length > 0 ? [@": " stringByAppendingString:(raw.length > 400 ? [raw substringToIndex:400] : raw)] : @"."];
        }
        [self appendRole:@"assistant" content:errorMessage];
        [self setSending:NO];
        return;
    }

    NSString *content = ISHLLMStringValue(message, @"content") ?: @"";
    NSArray<NSDictionary *> *toolCalls = ISHLLMValidToolCalls(message);
    if (toolCalls.count == 0) {
        NSString *finalText = ISHLLMSanitizedAssistantContent(content);
        if (anthropicNote.length > 0)
            finalText = finalText.length > 0 ? [finalText stringByAppendingFormat:@"\n\n%@", anthropicNote] : anthropicNote;
        [self appendRole:@"assistant" content:finalText.length > 0 ? finalText : @"(The model returned an empty response.)"];
        [self setSending:NO];
        [self saveTranscript];
        return;
    }
    NSMutableDictionary *turn = [@{
        @"role": @"assistant",
        @"content": ISHLLMSanitizedAssistantContent(content),
        @"tool_calls": toolCalls,
    } mutableCopy];
    // Sent back verbatim next round: thinking blocks must return unchanged.
    if ([message[kISHLLMAnthropicContentKey] isKindOfClass:NSArray.class])
        turn[kISHLLMAnthropicContentKey] = message[kISHLLMAnthropicContentKey];
    [_messages addObject:turn];
    [self notifyMessages];
    [self saveTranscript];
    [self runToolCalls:toolCalls index:0 round:round model:model apiKey:apiKey];
}

- (void)runToolCalls:(NSArray<NSDictionary *> *)toolCalls index:(NSUInteger)index round:(NSInteger)round model:(NSString *)model apiKey:(NSString *)apiKey {
    if (_cancelled) {
        [self finishWithStop];
        return;
    }
    if (index >= toolCalls.count) {
        [self runToolLoopRound:round + 1 model:model apiKey:apiKey];
        return;
    }
    ISHLLMToolInvocation *invocation = [ISHLLMToolInvocation invocationWithToolCall:toolCalls[index] context:_toolContext];
    [self performToolInvocation:invocation decision:nil completion:^(NSString *resultText, NSString *summary) {
        [self scoped:^{
            [self->_messages addObject:@{
                @"role": @"tool",
                @"tool_call_id": invocation.callID,
                @"name": invocation.name.length > 0 ? invocation.name : @"run_shell",
                @"content": resultText ?: @"",
                @"summary": summary.length > 0 ? summary : (resultText ?: @""),
            }];
            [self notifyMessages];
            [self saveTranscript];
            if (self->_cancelled) {
                [self finishWithStop];
                return;
            }
            [self runToolCalls:toolCalls index:index + 1 round:round model:model apiKey:apiKey];
        }];
    }];
}

- (void)setRunningTool:(ISHLLMToolInvocation *)invocation {
    _runningToolName = [invocation.name copy];
    _toolCallsThisReply++;
    NSDictionary *call = @{@"id": invocation.callID ?: @"", @"function": @{@"name": invocation.name ?: @"", @"arguments": invocation.arguments ?: @{}}};
    NSString *detail = ISHLLMToolCallDescriptions(@[call]).firstObject;
    // setPhase: only posts on a change; the tool changed even if the phase did not.
    _phase = ISHLLMAgentPhaseIdle;
    [self setPhase:ISHLLMAgentPhaseTool detail:detail];
}

// Every tool call from either backend: the permission rules, then the
// user's answer when they say ask, then the tool. `decision` (optional)
// hears whether it ran, for the Apple FM repeat guard.
- (void)performToolInvocation:(ISHLLMToolInvocation *)invocation
                     decision:(void (^)(BOOL approved))decision
                   completion:(void (^)(NSString *result, NSString *summary))completion {
    ISHLLMToolContext *context = _toolContext;
    void (^run)(void) = ^{
        [self setRunningTool:invocation];
        if ([invocation.name isEqualToString:kISHLLMSpawnAgentTool] || [invocation.name isEqualToString:kISHLLMAgentResultTool]) {
            [self runAgentTool:invocation completion:completion];
            return;
        }
        ISHLLMRunToolInvocation(invocation, context, completion);
    };
    if (invocation.problem != nil) {
        run();
        return;
    }
    NSString *reason = nil;
    ISHLLMPermissionAction action = [invocation permissionWithReason:&reason];
    if (action == ISHLLMPermissionDeny) {
        if (decision != nil)
            decision(NO);
        completion([NSString stringWithFormat:@"Not run: the user's tool permissions refuse this (%@). Do not try to reach the same result another way; tell the user what you needed instead.",
                    reason ?: [NSString stringWithFormat:@"%@ is set to Deny", ISHLLMToolCategoryTitle(invocation.category)]],
                   @"refused by permissions");
        return;
    }
    if (action == ISHLLMPermissionAllow || _autoRunCommandsThisChat || _autoRunCommandsThisReply) {
        if (decision != nil)
            decision(YES);
        run();
        return;
    }
    ISHLLMAgentApproval *approval = [[ISHLLMAgentApproval alloc] initWithAgent:self invocation:invocation reason:reason completion:^(ISHLLMToolRunDecision choice) {
        if (decision != nil)
            decision(choice != ISHLLMToolRunDecline);
        if (choice == ISHLLMToolRunDecline) {
            completion(invocation.category == ISHLLMToolCategoryShell ? @"The user declined to run this command." : @"The user declined this tool call.",
                       @"declined by user");
            return;
        }
        if (choice == ISHLLMToolRunAllowReply)
            self->_autoRunCommandsThisReply = YES;
        else if (choice == ISHLLMToolRunAllowChat)
            self->_autoRunCommandsThisChat = YES;
        run();
    }];
    _pendingApproval = approval;
    NSDictionary *call = @{@"id": invocation.callID ?: @"", @"function": @{@"name": invocation.name ?: @"", @"arguments": invocation.arguments ?: @{}}};
    [self setPhase:ISHLLMAgentPhaseApproval detail:ISHLLMToolCallDescriptions(@[call]).firstObject];
}

- (void)approval:(ISHLLMAgentApproval *)approval resolvedWith:(ISHLLMToolRunDecision)decision {
    (void) decision;
    if (_pendingApproval == approval)
        _pendingApproval = nil;
    [self postStateChange];
}

#pragma mark - Sub-agents

- (ISHLLMAgent *)subagentWithShortID:(NSString *)shortID {
    NSString *wanted = [shortID.lowercaseString stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    for (ISHLLMAgent *subagent in _subagents) {
        if ([subagent.shortID isEqualToString:wanted] || [subagent.sessionID.lowercaseString isEqualToString:wanted])
            return subagent;
    }
    return nil;
}

- (void)attachToParent:(ISHLLMAgent *)parent {
    _parent = parent;
}

// The final report: the sub-agent's last answer since its task was given.
- (NSString *)finalReport {
    NSString *last = [self lastAssistantText];
    return last.length > 0 ? last : @"(The sub-agent gave no answer.)";
}

- (void)runAgentTool:(ISHLLMToolInvocation *)invocation completion:(void (^)(NSString *result, NSString *summary))completion {
    NSDictionary *arguments = invocation.arguments;
    if (![self offersAgentTools]) {
        completion(@"Sub-agents cannot start or query agents of their own. Do the task yourself.", @"not available to sub-agents");
        return;
    }
    if ([invocation.name isEqualToString:kISHLLMSpawnAgentTool]) {
        NSUInteger running = 0;
        for (ISHLLMAgent *subagent in _subagents)
            running += subagent.busy;
        if (running >= kISHLLMMaxRunningSubagents) {
            completion([NSString stringWithFormat:@"Not started: %lu sub-agents are already working, the most at once. Collect a result with agent_result first.", (unsigned long) running],
                       @"too many sub-agents");
            return;
        }
        NSString *description = [ISHLLMStringValue(arguments, @"description") stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        if (description.length == 0)
            description = @"sub-task";
        NSString *prompt = ISHLLMStringValue(arguments, @"prompt") ?: @"";
        NSDictionary<NSString *, id> *entry = ISHLLMCreateBackgroundSession([@"↳ " stringByAppendingString:description], @{
            @"parent": _sessionID,
            @"titleIsCustom": @YES,
            @"destination": ISHLLMStringValue([self scopeDestination], kISHLLMDestinationID) ?: @"",
            @"workingDirectory": _toolContext.workingDirectory ?: @"",
        });
        ISHLLMAgent *subagent = [ISHLLMAgentManager.shared agentForSessionID:ISHLLMStringValue(entry, @"id")];
        if (subagent == nil) {
            completion(@"Could not start the sub-agent.", @"failed to start");
            return;
        }
        [subagent attachToParent:self];
        // Allowing every tool call in this chat covers the work it hands out.
        subagent->_autoRunCommandsThisChat = _autoRunCommandsThisChat;
        [_subagents addObject:subagent];
        [subagent sendPrompt:prompt];
        [self postStateChange];
        completion([NSString stringWithFormat:@"Started sub-agent %@ (\"%@\"). It works in the background; call agent_result with id \"%@\" for its report.",
                    subagent.shortID, description, subagent.shortID],
                   [NSString stringWithFormat:@"started sub-agent %@", subagent.shortID]);
        return;
    }

    ISHLLMAgent *subagent = [self subagentWithShortID:ISHLLMStringValue(arguments, @"id") ?: @""];
    if (subagent == nil) {
        completion([NSString stringWithFormat:@"No sub-agent of this chat has the id \"%@\".", ISHLLMStringValue(arguments, @"id") ?: @""], @"unknown sub-agent");
        return;
    }
    BOOL wait = arguments[@"wait"] == nil || [arguments[@"wait"] boolValue];
    if (!subagent.busy || !wait) {
        if (subagent.busy) {
            completion([NSString stringWithFormat:@"Sub-agent %@ is still working (%@, %ld tool calls so far).", subagent.shortID, [subagent statusLine].lowercaseString, (long) subagent.toolCallsThisReply],
                       @"still working");
            return;
        }
        completion([NSString stringWithFormat:@"Sub-agent %@ finished (%@). Its report:\n\n%@", subagent.shortID, subagent.lastOutcome ?: @"Done", [subagent finalReport]],
                   [NSString stringWithFormat:@"report from sub-agent %@", subagent.shortID]);
        return;
    }
    [self setPhase:ISHLLMAgentPhaseWaitingForAgents detail:subagent.title];
    __block BOOL answered = NO;
    void (^answer)(void) = ^{
        if (answered)
            return;
        answered = YES;
        if (self->_cancelled) {
            completion(@"(stopped while waiting for the sub-agent)", @"stopped");
            return;
        }
        completion([NSString stringWithFormat:@"Sub-agent %@ finished (%@). Its report:\n\n%@", subagent.shortID, subagent.lastOutcome ?: @"Done", [subagent finalReport]],
                   [NSString stringWithFormat:@"report from sub-agent %@", subagent.shortID]);
    };
    [_resultWaiters addObject:[answer copy]];
    [subagent whenIdle:^{
        [self->_resultWaiters removeObject:answer];
        answer();
    }];
}

// Stop: wake every agent_result that is waiting.
- (void)wakeResultWaiters {
    NSArray<void (^)(void)> *waiters = [_resultWaiters copy];
    [_resultWaiters removeAllObjects];
    for (void (^waiter)(void) in waiters)
        waiter();
}

#pragma mark - Compaction

- (void)compact {
    if ([self isBusy])
        return;
    [self runEntryPoint:^{
        if (ISHLLMUsesGeminiAPI() || ISHLLMCurrentBackend() == AOKLLMBackendAppleFoundationModels) {
            [self appendLocalRole:@"assistant" content:@"Summarizing needs an OpenAI-compatible or Anthropic destination."];
            return;
        }
        [self setSending:YES];
        [self summarizeConversationKeepingTrailing:0 then:^(BOOL ok) {
            self->_cancelled = NO;
            self->_lastOutcome = ok ? @"Summarized" : @"Ready";
            [self setSending:NO];
        }];
    }];
}

// Asks the model for a summary of the history it is sent, then records it
// as a message marked "compacted": from then on the model gets the summary in
// place of everything before it. keepTrailing: how many of the newest
// messages stay after the summary -- the prompt about to be sent, or inside
// a tool loop the round's calls and their results.
- (void)summarizeConversationKeepingTrailing:(NSUInteger)keepTrailing then:(void (^)(BOOL ok))continuation {
    NSMutableArray<NSDictionary<NSString *, id> *> *history = [[self providerMessages] mutableCopy];
    NSUInteger kept = MIN(keepTrailing, history.count);
    [history removeObjectsInRange:NSMakeRange(history.count - kept, kept)];
    NSUInteger conversational = 0;
    for (NSDictionary *message in history)
        conversational += ![message[@"role"] isEqual:@"system"];
    if (conversational < 2) {
        if (keepTrailing == 0)
            [self appendLocalRole:@"assistant" content:@"Nothing to summarize yet."];
        continuation(NO);
        return;
    }
    NSURL *url = [NSURL URLWithString:ISHLLMChatEndpoint()];
    NSString *model = [self modelName];
    NSString *apiKey = [self apiKey];
    [history addObject:@{@"role": @"user", @"content":
        @"Summarize this conversation so that it can be continued from the summary alone: what the user wants, "
        @"decisions made, files read or changed and their current state, commands run and results that still matter, "
        @"and what remains to do. Be complete but brief. Reply with the summary only."}];
    BOOL anthropic = ISHLLMUsesAnthropicAPI();
    NSDictionary *anthropicBody = anthropic ? ISHLLMAnthropicRequestBody(model ?: @"", 16000, history, nil, nil, nil) : nil;
    NSData *body = [NSJSONSerialization dataWithJSONObject:@{@"model": model ?: @"", @"messages": history, @"stream": @NO} options:0 error:nil];
    if (url == nil || body == nil) {
        [self appendLocalRole:@"assistant" content:@"Could not summarize: invalid server URL or request."];
        continuation(NO);
        return;
    }
    [self setPhase:ISHLLMAgentPhaseSummarizing detail:nil];
    NSUInteger insertAt = _messages.count;
    for (NSUInteger skipped = 0; skipped < keepTrailing && insertAt > 0; ) {
        insertAt--;
        if (![_messages[insertAt][@"local"] isEqual:@"1"])
            skipped++;
    }
    [self inBackground:^{
        NSInteger statusCode = 0;
        NSError *error = nil;
        NSData *data = anthropic ? ISHLLMAnthropicPost(anthropicBody, apiKey, &statusCode, &error)
                                 : ISHLLMSynchronousChatPost(url, body, apiKey, &statusCode, &error);
        id json = data.length > 0 ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
        NSArray *choices = [json isKindOfClass:NSDictionary.class] && [json[@"choices"] isKindOfClass:NSArray.class] ? json[@"choices"] : nil;
        NSDictionary *message = choices.count > 0 && [choices[0] isKindOfClass:NSDictionary.class] ? choices[0][@"message"] : nil;
        if (anthropic && [json isKindOfClass:NSDictionary.class])
            message = ISHLLMAnthropicMessageFromResponse(json, NULL, NULL);
        NSString *content = [message isKindOfClass:NSDictionary.class] ? ISHLLMStringValue(message, @"content") : nil;
        NSString *summary = [ISHLLMSanitizedAssistantContent(content ?: @"") stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        [self onMain:^{
            if (summary.length == 0) {
                NSString *why = error.localizedDescription ?: (statusCode > 0 ? [NSString stringWithFormat:@"HTTP %ld", (long) statusCode] : @"empty reply");
                [self appendLocalRole:@"assistant" content:[NSString stringWithFormat:@"Could not summarize the conversation (%@); the full history is still sent.", why]];
                continuation(NO);
                return;
            }
            NSDictionary *entry = @{
                @"role": @"user",
                @"content": [@"Summary of the conversation so far (the messages before this are no longer sent to the model):\n\n" stringByAppendingString:summary],
                @"compacted": @"1",
            };
            [self->_messages insertObject:entry atIndex:MIN(insertAt, self->_messages.count)];
            [self saveTranscript];
            [self notifyMessages];
            continuation(YES);
        }];
    }];
}

#pragma mark - Reverting a change

- (void)revertChange:(ISHLLMFileChange *)change force:(BOOL)force
          completion:(void (^)(BOOL reverted, BOOL changedSince, NSString *message))completion {
    ISHLLMRevertFileChange(change, _toolContext, force, ^(BOOL reverted, BOOL changedSince, NSString *message) {
        if (reverted) {
            // As the user's own message, so the model knows its change is gone
            // and re-reads the file before touching it again.
            [self->_messages addObject:@{@"role": @"user",
                                         @"content": [NSString stringWithFormat:@"(I reverted your change to %@. %@)", change.path, message]}];
            [self saveTranscript];
            [self notifyMessages];
        }
        completion(reverted, changedSince, message);
    });
}

@end

@implementation ISHLLMAgentManager {
    NSMutableDictionary<NSString *, ISHLLMAgent *> *_agents;
}

+ (instancetype)shared {
    static ISHLLMAgentManager *manager;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        manager = [ISHLLMAgentManager new];
    });
    return manager;
}

- (instancetype)init {
    if ((self = [super init]))
        _agents = [NSMutableDictionary dictionary];
    return self;
}

- (ISHLLMAgent *)existingAgentForSessionID:(NSString *)sessionID {
    return sessionID.length > 0 ? _agents[sessionID] : nil;
}

- (ISHLLMAgent *)agentForSessionID:(NSString *)sessionID {
    if (sessionID.length == 0)
        return nil;
    ISHLLMAgent *agent = _agents[sessionID];
    if (agent != nil)
        return agent;
    NSDictionary<NSString *, id> *entry = ISHLLMSessionEntryWithID(sessionID);
    if (entry == nil)
        return nil;
    agent = [[ISHLLMAgent alloc] initWithSessionEntry:entry];
    _agents[sessionID] = agent;
    // A sub-agent loaded on its own (its chat opened from the list after a
    // relaunch) finds its parent if that is loaded; it cannot be waited on
    // across a relaunch anyway.
    NSString *parentID = agent.parentSessionID;
    ISHLLMAgent *parent = parentID.length > 0 ? _agents[parentID] : nil;
    if (parent != nil)
        [agent attachToParent:parent];
    return agent;
}

- (NSArray<ISHLLMAgent *> *)agentsWantingAttention {
    NSMutableArray<ISHLLMAgent *> *approval = [NSMutableArray array];
    NSMutableArray<ISHLLMAgent *> *running = [NSMutableArray array];
    NSMutableArray<ISHLLMAgent *> *finished = [NSMutableArray array];
    for (ISHLLMAgent *agent in _agents.allValues) {
        if (agent.pendingApproval != nil)
            [approval addObject:agent];
        else if (agent.busy)
            [running addObject:agent];
        else if (agent.finishedUnseen)
            [finished addObject:agent];
    }
    NSComparator byStart = ^NSComparisonResult(ISHLLMAgent *a, ISHLLMAgent *b) {
        return [b.replyStarted ?: NSDate.distantPast compare:a.replyStarted ?: NSDate.distantPast];
    };
    [approval sortUsingComparator:byStart];
    [running sortUsingComparator:byStart];
    [finished sortUsingComparator:^NSComparisonResult(ISHLLMAgent *a, ISHLLMAgent *b) {
        return [b.lastFinished ?: NSDate.distantPast compare:a.lastFinished ?: NSDate.distantPast];
    }];
    return [[approval arrayByAddingObjectsFromArray:running] arrayByAddingObjectsFromArray:finished];
}

- (void)forgetSessionID:(NSString *)sessionID {
    ISHLLMAgent *agent = _agents[sessionID];
    if (agent == nil)
        return;
    [agent stop];
    [_agents removeObjectForKey:sessionID];
    [NSNotificationCenter.defaultCenter postNotificationName:ISHLLMAgentStateDidChangeNotification object:agent];
}

@end
