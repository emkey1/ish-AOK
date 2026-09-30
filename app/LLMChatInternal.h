//
//  LLMChatInternal.h
//  iSH-AOK
//
//  Declarations shared by the Workspace LLM Chat's files, which were one
//  region of AboutViewController.m until 2026-09-28:
//
//    LLMChatSupport.m      destinations, saved chats, endpoints, HTTP and SSE
//    LLMChatTools.m        the tools the model can call, and their limits
//    LLMChatViewController.m  the chat screen itself
//    LLMChatSettings.m     LLM Settings, provider picker, chat and destination lists
//
//  Not a public interface: the app outside these files reaches the chat through
//  the factories in AboutViewController.h.
//

#import <UIKit/UIKit.h>
#import "AboutViewController.h"
#import "WorkspaceViewController.h"
#import "LLMChatPermissions.h"
#include <netdb.h>

// WorkspaceTextScalable: the Workspace window's Cmd+= / Cmd+- / Cmd+0. Shown
// modally from a terminal instead, nothing sets it and the chat stays at 1.0.
@interface LLMClientViewController : UIViewController <UITextViewDelegate, UITableViewDataSource, UITableViewDelegate, WorkspaceTextScalable>

@property (nonatomic, copy) NSString *initialPrompt;

@end

@interface LLMSettingsViewController : UITableViewController <WorkspaceTextScaledPage>
// Opened from a chat: no Models row, the chat's model button is where a
// model is chosen.
@property (nonatomic) BOOL hidesModels;
@end

// One destination: name, provider, server, model and key, plus picking the
// model from the server's list and a connection test. Each change is saved
// as it is made.
@interface LLMDestinationEditorViewController : UITableViewController <WorkspaceTextScaledPage>
@property (nonatomic, copy) NSDictionary<NSString *, NSString *> *destination;
@property (nonatomic, copy) void (^destinationSaved)(void);
@end

// The saved chats, newest first: switch, rename, delete. Presented modally
// (the chat is an embedded child view controller in Workspace mode, where a
// push has nowhere to go).
@interface LLMChatSessionListViewController : UITableViewController
@property (nonatomic, copy) NSString *currentSessionID;
@property (nonatomic, copy) void (^sessionSelected)(NSString *sessionID);
@end

// The chats whose agents are working, waiting for approval, or have an
// unread answer (LLMChatAgentList.m); picking one opens it.
@interface LLMAgentListViewController : UITableViewController
@property (nonatomic, copy) NSString *currentSessionID;
@property (nonatomic, copy) void (^agentSelected)(NSString *sessionID);
@end

@class ISHLLMToolContext, ISHLLMFileChange;

// The chat menu's "Changes…": the files the model changed, as diffs, each
// revertible. The chat does the reverting (it also tells the model), and
// calls `done` so the list and the open diff redraw.
@interface LLMChangesViewController : UITableViewController <WorkspaceTextScaledPage>
@property (nonatomic, strong) ISHLLMToolContext *toolContext;
@property (nonatomic, copy) void (^revertRequested)(ISHLLMFileChange *change, UIViewController *presenter, void (^done)(void));
@end

// LLM Settings -> Tool Permissions: allow/ask/deny per tool category, and the
// shell command rules. See LLMChatPermissions.h.
@interface LLMToolPermissionsViewController : UITableViewController <WorkspaceTextScaledPage>
@end

// LLM Settings -> MCP Servers: remote and in-guest MCP servers whose tools
// the chat offers. See LLMChatMCP.h.
@interface LLMMCPServersViewController : UITableViewController <WorkspaceTextScaledPage>
@end

// The saved destinations: select, edit, duplicate, delete, add from a preset.
// Reachable both from the chat's destination menu and from LLM Settings.
@interface LLMDestinationListViewController : UITableViewController <WorkspaceTextScaledPage>
@property (nonatomic, copy) void (^destinationsChanged)(void);
@end

typedef NS_ENUM(NSInteger, ISHLLMToolRunDecision) {
    ISHLLMToolRunDecline = 0,
    ISHLLMToolRunOnce,
    ISHLLMToolRunAllowReply, // run this and auto-run the rest of this reply
    ISHLLMToolRunAllowChat,  // run this and auto-run for the rest of the chat
};

// Bounds for the user-configurable shell-tool limits. The floor and ceiling keep
// the tool loop functional rather than being safety policy: a command with no
// timeout wedges the serial tool queue forever, and an unbounded capture blows
// out the model's context window when the result is fed back.
static const NSInteger kISHLLMToolTimeoutMinSeconds = 5;
static const NSInteger kISHLLMToolTimeoutMaxSeconds = 900;
static const NSInteger kISHLLMToolOutputMinKB = 16;
static const NSInteger kISHLLMToolOutputMaxKB = 256;
// Below the floor a multi-step task (probe environment, install a package, use
// it) can't finish before the loop bails; above the ceiling a runaway loop with
// a bad model/prompt burns too many requests before the user notices.
static const NSInteger kISHLLMToolMaxRoundsMin = 4;
static const NSInteger kISHLLMToolMaxRoundsMax = 50;

// Every request resends the entire transcript, so without some limit a
// tool-heavy conversation keeps adding another full command's output on top
// of every prior one, forever -- a handful of rounds is enough to blow past
// most models' context windows. Only the most recent tool results (as many as
// fit under a token budget) keep their full content; older ones are
// compacted down to their one-line summary instead. The budget scales with
// the model's real context window when known (see probeContextWindowIfNeeded)
// so a small local model compacts aggressively and a large one doesn't
// discard useful history it didn't need to.
static const NSInteger kISHLLMToolContextDefaultBudgetTokens = 4000; // used only when the window size is unknown
static const CGFloat kISHLLMToolContextBudgetFraction = 0.5; // fraction of a KNOWN window reserved for tool-result content

// The raw-socket streamer above exists because ATS blocks plain http, so
// local Ollama/LM Studio endpoints have to be spoken to directly. https
// endpoints go through NSURLSession, which used to mean the completion-handler
// API and therefore no streaming at all: every hosted provider sat silent for
// the whole generation and then dumped the finished answer.
//
// A data-task delegate gives the same incremental Server-Sent Events the
// direct path parses, so both transports now stream. Everything before the
// first token is identical to the non-streaming path, and the fallbacks are
// the ones that already existed: a non-200, or a server that ignores
// "stream": true and answers with an ordinary JSON body, ends with zero
// chunks and the buffered body is handed to the normal response handler.
// See LLMChatSupport.m. Payloads arrive on the session's delegate queue.
@interface ISHLLMRawStreamDelegate : NSObject <NSURLSessionDataDelegate>
@property (nonatomic, copy) void (^payloadHandler)(NSString *payload);
@property (nonatomic, copy) void (^completionHandler)(NSData *body, NSInteger statusCode, NSError *error);
@end

@interface ISHLLMStreamingResponseDelegate : NSObject <NSURLSessionDataDelegate>
@property (nonatomic, copy) void (^chunkHandler)(NSString *chunk);
// receivedChunks is what tells the caller whether the reply already went on
// screen incrementally or still has to be parsed out of body.
@property (nonatomic, copy) void (^completionHandler)(BOOL receivedChunks, NSData *body, NSInteger statusCode, NSError *error);
@end

// A "destination" is one saved endpoint: provider + server URL + model + API
// key. The four UserPreferences scalars stay the single source of truth for
// everything that actually talks to a server -- selecting a destination just
// mirrors its values into them -- so no request path had to learn about
// destinations at all. The saved array only describes the set to pick from.
//
// Because the mirror is global, two live chat view controllers (a Workspace
// window and the terminal's modal) share one active destination: the last
// switch wins for both. That matches how the four scalars already behaved.
static NSString *const kISHLLMDestinationID = @"id";
static NSString *const kISHLLMDestinationName = @"name";
static NSString *const kISHLLMDestinationProvider = @"provider";
static NSString *const kISHLLMDestinationURL = @"url";
static NSString *const kISHLLMDestinationModel = @"model";
static NSString *const kISHLLMDestinationAPIKey = @"apiKey";

// LLMChatSupport.m
BOOL ISHLLMDirectHTTPPostStreamingPayloads(NSURL *url, NSData *body, NSString *apiKey,
                                           NSDictionary<NSString *, NSString *> *extraHeaders,
                                           void (^payloadHandler)(NSString *payload),
                                           int *fdOut, NSInteger *statusCodeOut,
                                           NSMutableData *plainBody, NSError **errorOut);
int ISHLLMConnectWithTimeout(struct addrinfo *results, int timeoutMs, int *errnoOut);
NSError *ISHLLMConnectionError(NSString *host, NSString *port, int errnoValue);
dispatch_queue_t ISHLLMGuestCommandQueue(void);
// The destination requests on this thread go to; nil = the one selected in
// Settings (the global scalars). See ISHLLMRunWithDestination.
NSDictionary<NSString *, NSString *> *ISHLLMThreadDestination(void);
void ISHLLMRunWithDestination(NSDictionary<NSString *, NSString *> *destination, void (^block)(void));
NSString *ISHLLMCurrentServerURL(void);
NSString *ISHLLMCurrentModel(void);
NSString *ISHLLMCurrentAPIKey(void);
NSString *ISHLLMCurrentProvider(void);
NSDictionary<NSString *, id> *ISHLLMCreateBackgroundSession(NSString *title, NSDictionary<NSString *, id> *extra);
NSURL *ISHLLMPersistDirectoryURL(void);
NSURL *ISHLLMTranscriptURL(void);
NSURL *ISHLLMExtractsDirectoryURL(void);
NSString *ISHLLMSanitizeFilenameComponent(NSString *value);
NSString *ISHLLMExtensionForFenceLanguage(NSString *language);
NSString *ISHLLMStringValue(NSDictionary *dictionary, NSString *key);
NSDictionary<NSString *, NSString *> *ISHLLMDestinationFromCurrentPreferences(NSString *identifier, NSString *name);
NSArray<NSDictionary<NSString *, NSString *> *> *ISHLLMDestinations(void);
NSUInteger ISHLLMActiveDestinationIndex(void);
NSDictionary<NSString *, NSString *> *ISHLLMActiveDestination(void);
NSString *ISHLLMDestinationDisplayName(NSDictionary<NSString *, NSString *> *destination);
// How a destination is named everywhere it is shown with its model:
// "LM Studio · ornith-1.5-35b", or the name alone when the model adds nothing.
NSString *ISHLLMDestinationLabel(NSDictionary<NSString *, NSString *> *destination);
NSString *ISHLLMDestinationSubtitle(NSDictionary<NSString *, NSString *> *destination);
void ISHLLMActivateDestination(NSDictionary<NSString *, NSString *> *destination);
void ISHLLMSyncActiveDestinationFromPreferences(void);
void ISHLLMSaveDestination(NSDictionary<NSString *, NSString *> *destination);
BOOL ISHLLMDeleteDestinationWithID(NSString *identifier);
NSURL *ISHLLMSessionsDirectoryURL(void);
NSURL *ISHLLMSessionIndexURL(void);
NSURL *ISHLLMSessionFileURL(NSString *sessionID);
BOOL ISHLLMSessionStorageAvailable(void);
void ISHLLMEnsureSessionsDirectory(void);
NSArray<NSDictionary<NSString *, id> *> *ISHLLMValidMessagesFromStoredArray(NSArray *stored);
NSArray<NSDictionary<NSString *, id> *> *ISHLLMLoadSessionMessages(NSString *sessionID);
void ISHLLMWriteSessionMessages(NSString *sessionID, NSArray<NSDictionary<NSString *, id> *> *messages);
NSString *ISHLLMSessionTitleFromMessages(NSArray<NSDictionary<NSString *, id> *> *messages);
NSDictionary<NSString *, id> *ISHLLMNewSessionEntry(NSString *title);
NSDictionary<NSString *, id> *ISHLLMLoadSessionIndexDocument(void);
void ISHLLMWriteSessionIndex(NSArray<NSDictionary<NSString *, id> *> *sessions, NSString *activeID);
NSArray<NSDictionary<NSString *, id> *> *ISHLLMSessionEntries(void);
NSArray<NSDictionary<NSString *, id> *> *ISHLLMSessionEntriesByRecency(void);
NSString *ISHLLMActiveSessionID(void);
NSDictionary<NSString *, id> *ISHLLMSessionEntryWithID(NSString *sessionID);
void ISHLLMUpdateSessionEntry(NSString *sessionID, NSDictionary<NSString *, id> *updates);
void ISHLLMSetActiveSessionID(NSString *sessionID);
NSDictionary<NSString *, id> *ISHLLMCreateSession(NSString *title);
NSString *ISHLLMDeleteSession(NSString *sessionID);
NSString *ISHLLMChatEndpoint(void);
BOOL ISHLLMUsesAppleFoundationModels(void);
AOKLLMBackend ISHLLMCurrentBackend(void);
NSString *ISHLLMAppleFoundationModelsUnavailableMessage(void);
BOOL ISHLLMFoundationModelsReady(void);
BOOL ISHLLMUsesGeminiAPI(void);
// Anthropic's Messages API (provider name contains "anthropic", or the URL is
// api.anthropic.com). See LLMChatAnthropic.h for the translation.
BOOL ISHLLMUsesAnthropicAPI(void);
NSString *ISHLLMAnthropicMessagesEndpoint(void);
void ISHLLMApplyAuthHeaders(NSMutableURLRequest *request, NSString *apiKey);
NSURL *ISHLLMProbeURL(void);
NSDictionary *ISHLLMProbeBody(NSString *model, NSString *prompt, NSUInteger maxTokens);
NSData *ISHLLMAnthropicPost(NSDictionary *body, NSString *apiKey, NSInteger *statusCodeOut, NSError **errorOut);
NSString *ISHLLMGeminiGenerateEndpoint(void);
NSString *ISHLLMModelsEndpoint(void);
NSArray<NSDictionary<NSString *, NSString *> *> *ISHLLMProviderPresets(void);
NSString *ISHLLMCurrentAPIFormat(void);
BOOL ISHLLMProviderRequiresAPIKey(void);
NSString *ISHLLMMissingAPIKeyMessage(void);
NSArray<NSString *> *ISHLLMModelIdentifiersFromResponseData(NSData *data);
NSInteger ISHLLMContextWindowFromModelsResponse(NSData *data, NSString *modelID);
// LLM Settings -> Context Window: the model's usable window in tokens, set by
// the user; 0 = automatic (whatever the server's /models reports). Wins over
// the reported value: a proxy may report none, or a model may fall apart
// well before its advertised window.
NSInteger ISHLLMContextWindowSetting(void);
void ISHLLMSetContextWindowSetting(NSInteger tokens);
NSString *ISHLLMFormattedTokenCountShort(NSInteger tokens);
// What a chat is compacted against when neither the setting nor the server
// gives a window: small enough for the local models behind proxies that
// report nothing, which degrade into garbage well before 128K.
static const NSInteger kISHLLMFallbackContextWindowTokens = 65536;
NSString *ISHLLMSanitizedAssistantContent(NSString *content);
NSString *ISHLLMStreamingAssistantContent(NSString *content);
BOOL ISHLLMHideThinkingEnabled(void);
NSArray<NSValue *> *ISHLLMCodeSpanRanges(NSString *content);
NSTextCheckingResult *ISHLLMFirstTagOutsideCode(NSRegularExpression *regex,
                                                       NSString *content,
                                                       NSRange range,
                                                       NSArray<NSValue *> *codeRanges);
NSString *ISHLLMSplitThinkingFromContent(NSString *content,
                                                NSMutableArray<NSString *> *thoughts,
                                                BOOL *openOut);
NSData *ISHLLMDirectHTTPPost(NSURL *url, NSData *body, NSString *apiKey, NSInteger *statusCodeOut, NSError **errorOut);
NSData *ISHLLMDirectHTTPGet(NSURL *url, NSString *apiKey, NSInteger *statusCodeOut, NSError **errorOut);
void ISHLLMFetchModelsDataAsync(void (^completion)(NSData *data, NSInteger statusCode, NSError *error));
NSString *ISHLLMContentFromStreamingPayload(NSString *payload);
BOOL ISHLLMDirectHTTPPostStreaming(NSURL *url,
                                          NSData *body,
                                          NSString *apiKey,
                                          void (^chunkHandler)(NSString *chunk),
                                          int *fdOut,
                                          NSInteger *statusCodeOut,
                                          NSError **errorOut);

// LLMChatTools.m
NSInteger ISHLLMEstimateTokenCount(NSString *text);
NSInteger ISHLLMEstimateMessagesTokenCount(NSArray<NSDictionary<NSString *, id> *> *messages);
NSString *ISHLLMFormattedTokenCount(NSInteger tokens);
NSInteger ISHLLMToolTimeoutSeconds(void);
NSInteger ISHLLMToolOutputLimitKB(void);
NSInteger ISHLLMToolMaxRounds(void);
NSString *ISHLLMToolTimeoutTitle(NSInteger seconds);
NSString *ISHLLMToolCallID(NSDictionary *toolCall);
NSString *ISHLLMToolCallName(NSDictionary *toolCall);
NSString *ISHLLMToolCallCommand(NSDictionary *toolCall);
NSDictionary *ISHLLMToolCallArguments(NSDictionary *toolCall);
NSArray<NSDictionary *> *ISHLLMValidToolCalls(NSDictionary *message);
NSData *ISHLLMSynchronousChatPost(NSURL *url, NSData *body, NSString *apiKey,
                                         NSInteger *statusCodeOut, NSError **errorOut);
NSString *ISHLLMRunGuestShellCommand(NSString *command, NSString *workingDirectory, NSString **summaryOut);
// homeOut: the command account's $HOME, the default working directory.
NSString *ISHLLMDetectGuestEnvironmentNote(NSString **homeOut);
// fileTools: the full tool set (OpenAI-compatible), or run_shell alone (Apple FM).
NSString *ISHLLMToolSystemNote(NSString *environmentNote, NSString *workingDirectory, BOOL fileTools);
// The same note in its two parts: everything but the clock (stable, so it can
// be cached), and the clock sentence (new every request).
NSString *ISHLLMToolSystemNoteWithoutClock(NSString *environmentNote, NSString *workingDirectory, BOOL fileTools);
NSString *ISHLLMClockNote(void);
// A guest path made absolute against workingDirectory and normalised
// lexically (".", "..", "//"; "~" is the tool account's home).
NSString *ISHLLMResolveGuestPath(NSString *raw, NSString *workingDirectory);
// POSIX single-quoting for a guest shell command line.
NSString *ISHLLMShellQuote(NSString *text);

// One file change the model made, kept so the user can review and revert it.
@interface ISHLLMFileChange : NSObject
@property (nonatomic, copy) NSString *path;
@property (nonatomic, copy) NSString *toolName;   // write_file or edit_file
@property (nonatomic, copy) NSData *before;       // nil when the file was created (or too large to keep)
@property (nonatomic, copy) NSData *after;
@property (nonatomic, copy) NSDate *date;
@property (nonatomic) BOOL created;
@property (nonatomic) BOOL revertible;
@property (atomic) BOOL reverted;
@end

// What one chat's tool calls share. Main thread only; the tools copy what
// they need before leaving it.
@interface ISHLLMToolContext : NSObject
// Where relative paths start and run_shell runs. nil until the chat has one
// (its saved setting, or the account's home once the environment probe says).
@property (nonatomic, copy) NSString *workingDirectory;
// write_file and edit_file refuse a file the model has not read in this chat,
// or one that changed since it did, so a stale picture is never written back
// over someone else's change. Keyed by absolute path.
- (void)recordReadOfPath:(NSString *)path size:(unsigned long long)size modified:(NSDate *)modified;
- (NSString *)stalenessProblemForPath:(NSString *)path size:(unsigned long long)size modified:(NSDate *)modified;
- (void)forgetReads;
// The file changes made in this chat, oldest first (16 MB of history).
@property (nonatomic, readonly) NSArray<ISHLLMFileChange *> *changes;
- (void)recordChange:(ISHLLMFileChange *)change;
// The model's todo_write list: dictionaries with "content" and "status".
@property (nonatomic, copy) NSArray<NSDictionary *> *todos;
// Where this chat's tools run: a serial queue of its own, so one chat's long
// command does not hold up another's. nil = ISHLLMGuestCommandQueue().
@property (nonatomic, strong) dispatch_queue_t queue;
@end

// One tool call from the model, parsed and checked, ready to confirm and run.
@interface ISHLLMToolInvocation : NSObject
@property (nonatomic, copy, readonly) NSString *callID;
@property (nonatomic, copy, readonly) NSString *name;
@property (nonatomic, readonly) ISHLLMToolCategory category;
@property (nonatomic, copy, readonly) NSString *command;   // run_shell
@property (nonatomic, copy, readonly) NSString *path;      // file tools: absolute
@property (nonatomic, copy, readonly) NSDictionary *arguments;
// Set when the call cannot run as asked (unknown tool, missing argument);
// this text goes back to the model as the result, nothing runs.
@property (nonatomic, copy, readonly) NSString *problem;
+ (instancetype)invocationWithToolCall:(NSDictionary *)toolCall context:(ISHLLMToolContext *)context;
// The rules' answer; reasonOut names what decided it.
- (ISHLLMPermissionAction)permissionWithReason:(NSString **)reasonOut;
- (NSString *)confirmationTitle;
- (NSString *)confirmationMessage;
@end

NSArray<NSDictionary<NSString *, id> *> *ISHLLMChatToolDefinitions(void);
// spawn_agent and agent_result, which the agent runs itself (LLMChatAgent.m).
extern NSString *const kISHLLMSpawnAgentTool;
extern NSString *const kISHLLMAgentResultTool;
NSArray<NSDictionary<NSString *, id> *> *ISHLLMAgentToolDefinitions(void);
// Runs an invocation (whose problem is nil) off the main thread and completes
// on main with the text for the model and a one-line summary for compaction.
void ISHLLMRunToolInvocation(ISHLLMToolInvocation *invocation, ISHLLMToolContext *context,
                             void (^completion)(NSString *result, NSString *summary));
// The nearest AGENTS.md (or CLAUDE.md) at or above workingDirectory, or nil.
// Blocks on the guest file bridge: call on ISHLLMGuestCommandQueue().
NSString *ISHLLMLoadProjectInstructions(NSString *workingDirectory, NSString **sourceOut);
// Puts the file back as it was before `change` (removes it if the change
// created it). Refuses, with changedSince, when the file is no longer what
// the change left, unless force. Completes on main.
void ISHLLMRevertFileChange(ISHLLMFileChange *change, ISHLLMToolContext *context, BOOL force,
                            void (^completion)(BOOL reverted, BOOL changedSince, NSString *message));
// One short line per call ("$ ls -la", "Read src/main.c"), for the transcript.
NSArray<NSString *> *ISHLLMToolCallDescriptions(NSArray *toolCalls);

// LLMChatChanges.m
// "edited · −3 +5 · 14:02", with " · reverted" once it is.
NSString *ISHLLMChangeSummary(ISHLLMFileChange *change);

// LLMChatSettings.m
void ISHConfigureLLMSettingsNavigationController(UINavigationController *navigationController);
NSString *ISHLLMDestinationNameForID(NSString *destinationID);
NSString *ISHLLMRelativeDateDescription(double timestamp);

