//
//  LLMChatAgent.h
//  iSH-AOK
//
//  A chat's agent: its messages and the whole reply loop (streaming, tool
//  calls, permissions, compaction, sub-agents), apart from any window. The
//  chat screen is a view of one agent; closing it or switching to another
//  chat leaves the agent working, and several can work at once, each against
//  its own destination and with its own tool queue.
//
//  Anything the loop needs from the user -- a tool call to approve -- waits
//  in `pendingApproval` until a chat screen showing that agent (or its
//  parent) puts it up, so a background agent never pops up over another
//  chat. ISHLLMAgentManager finds the agents that want attention.
//
//  Main thread only.
//

#import <Foundation/Foundation.h>
#import "LLMChatInternal.h"

@class ISHLLMAgent;

typedef NS_ENUM(NSInteger, ISHLLMAgentPhase) {
    ISHLLMAgentPhaseIdle,
    ISHLLMAgentPhasePreparing,        // environment probe, AGENTS.md, MCP servers
    ISHLLMAgentPhaseContacting,       // request sent, nothing back yet
    ISHLLMAgentPhaseThinking,         // reasoning streaming in
    ISHLLMAgentPhaseWriting,          // the answer streaming in
    ISHLLMAgentPhaseTool,             // a tool running
    ISHLLMAgentPhaseApproval,         // a tool call waiting for the user
    ISHLLMAgentPhaseWaitingForAgents, // agent_result waiting on a sub-agent
    ISHLLMAgentPhaseSummarizing,      // compaction
};

// One tool call waiting for the user's answer.
@interface ISHLLMAgentApproval : NSObject
@property (nonatomic, weak, readonly) ISHLLMAgent *agent;
@property (nonatomic, strong, readonly) ISHLLMToolInvocation *invocation;
@property (nonatomic, copy, readonly) NSString *reason;
@property (nonatomic, readonly) BOOL resolved;
// The screen that has it up right now, so a second window does not put up
// the same question; cleared when that screen goes away.
@property (nonatomic, weak) id presenter;
- (void)resolve:(ISHLLMToolRunDecision)decision;
@end

@protocol ISHLLMAgentObserver <NSObject>
@optional
- (void)agentMessagesDidChange:(ISHLLMAgent *)agent;
// A streaming reply grew; only its (trailing) row needs redrawing.
- (void)agent:(ISHLLMAgent *)agent didUpdateStreamingMessageAtIndex:(NSUInteger)index;
- (void)agentStatusDidChange:(ISHLLMAgent *)agent;
// Title, system prompt, destination or working directory.
- (void)agentMetadataDidChange:(ISHLLMAgent *)agent;
@end

// Posted (object: the agent) whenever an agent starts or finishes a reply,
// changes phase, or gains or loses a pending approval: for the status bar's
// "other agents" line and the lists.
extern NSNotificationName const ISHLLMAgentStateDidChangeNotification;

@interface ISHLLMAgent : NSObject

@property (nonatomic, copy, readonly) NSString *sessionID;
@property (nonatomic, copy, readonly) NSString *title;
@property (nonatomic, copy, readonly) NSString *systemPrompt;
@property (nonatomic, readonly) NSArray<NSDictionary<NSString *, id> *> *messages;
@property (nonatomic, strong, readonly) ISHLLMToolContext *toolContext;

// A sub-agent's parent (nil for a chat of its own), and a parent's
// sub-agents, oldest first.
@property (nonatomic, weak, readonly) ISHLLMAgent *parent;
@property (nonatomic, copy, readonly) NSString *parentSessionID;
@property (nonatomic, readonly) NSArray<ISHLLMAgent *> *subagents;
// The short id the parent's model uses for it.
@property (nonatomic, copy, readonly) NSString *shortID;

// Status, for the status bar and the lists.
@property (nonatomic, readonly, getter=isBusy) BOOL busy;
@property (nonatomic, readonly) ISHLLMAgentPhase phase;
@property (nonatomic, copy, readonly) NSString *phaseDetail; // the command, file, tool or sub-agent
@property (nonatomic, strong, readonly) NSDate *replyStarted;
@property (nonatomic, strong, readonly) NSDate *phaseStarted;
@property (nonatomic, readonly) NSInteger round;              // tool-loop round of this reply
@property (nonatomic, readonly) NSInteger toolCallsThisReply;
@property (nonatomic, strong, readonly) ISHLLMAgentApproval *pendingApproval;
// A reply finished while no screen was showing this agent.
@property (nonatomic, readonly) BOOL finishedUnseen;
@property (nonatomic, copy, readonly) NSString *lastOutcome;  // "Done", "Stopped", "Failed: …"
@property (nonatomic, strong, readonly) NSDate *lastFinished;

- (NSString *)statusLine;                 // the phase in words ("Running command", "Ready")
- (NSDictionary<NSString *, NSString *> *)destination; // what the next request goes to
- (NSString *)modelName;
- (NSInteger)contextWindowTokens;         // the setting or the server's; 0 unknown
- (NSInteger)effectiveContextWindowTokens; // with the fallback applied
- (NSInteger)estimatedContextTokens;
- (NSString *)toolsSummaryText;
- (NSString *)homeDirectory;
// This agent's pending approval, or else the first of its sub-agents'.
- (ISHLLMAgentApproval *)pendingApprovalIncludingSubagents;

- (void)addObserver:(id<ISHLLMAgentObserver>)observer;
- (void)removeObserver:(id<ISHLLMAgentObserver>)observer;
// A screen showing this agent; while any is, a finished reply is not "unseen".
- (void)beginViewing;
- (void)endViewing;

// A prompt, or /models, /model <name>, /compact. Sent while a reply is
// still coming in, it waits in `queuedPrompts` and goes when that reply
// ends; Stop drops the queue.
- (void)sendPrompt:(NSString *)prompt;
@property (nonatomic, readonly) NSArray<NSString *> *queuedPrompts;
- (void)stop;
// Stops, and runs `continuation` once the reply in flight has landed (at
// once if nothing is).
- (void)stopThen:(void (^)(void))continuation;
- (void)clearMessages;
- (void)compact;
- (void)queryModels;
- (void)appendLocalRole:(NSString *)role content:(NSString *)content;
- (void)appendRole:(NSString *)role content:(NSString *)content;

- (void)setCustomTitle:(NSString *)title; // empty = back to automatic
- (void)setSystemPrompt:(NSString *)systemPrompt;
- (void)setWorkingDirectory:(NSString *)path; // nil or empty = the home directory
- (void)useDestinationWithID:(NSString *)destinationID;
// Reverts a file change and, when it did, tells the model.
- (void)revertChange:(ISHLLMFileChange *)change force:(BOOL)force
          completion:(void (^)(BOOL reverted, BOOL changedSince, NSString *message))completion;
// Re-reads the transcript if another process wrote it since (idle only).
- (void)reloadIfChangedOnDisk;
@end

@interface ISHLLMAgentManager : NSObject
+ (instancetype)shared;
// The agent for a saved chat, loaded on first use; nil if there is no such chat.
- (ISHLLMAgent *)agentForSessionID:(NSString *)sessionID;
- (ISHLLMAgent *)existingAgentForSessionID:(NSString *)sessionID;
// Every loaded agent that is working, waiting for approval, or has an
// answer nobody has looked at, most urgent first.
- (NSArray<ISHLLMAgent *> *)agentsWantingAttention;
// A chat was deleted: stop its agent (and sub-agents) and drop it.
- (void)forgetSessionID:(NSString *)sessionID;

// The guest, not any one chat: probed once, shared by every agent.
@property (nonatomic, copy) NSString *guestEnvironmentNote;
@property (nonatomic, copy) NSString *guestHomeDirectory;
@end
