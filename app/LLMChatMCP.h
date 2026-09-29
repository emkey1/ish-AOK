//
//  LLMChatMCP.h
//  iSH-AOK
//
//  MCP servers as tool sources for the LLM Chat. Two kinds:
//    remote  -- a Streamable HTTP endpoint (https://…/mcp), with an optional
//               bearer token kept in the Keychain;
//    guest   -- a stdio server started as a command inside the AOK guest
//               (npx …, uvx …, python3 server.py), talking over its
//               stdin/stdout through guest_process_spawn_user (kernel/init.h),
//               as the chat's tool account.
//  The model sees each server tool as mcp__<server>__<tool>; calls go
//  through the chat's permissions as the "MCP Tools" category.
//

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

// Server settings, in NSUserDefaults ("LLM MCP Servers"): dictionaries with
// "id", "name", "kind" ("remote" or "guest"), "url", "command", "enabled".
extern NSString *const kISHLLMMCPKindRemote;
extern NSString *const kISHLLMMCPKindGuest;
NSArray<NSDictionary<NSString *, id> *> *ISHLLMMCPServers(void);
void ISHLLMSetMCPServers(NSArray<NSDictionary<NSString *, id> *> *servers);
NSString *_Nullable ISHLLMMCPServerToken(NSString *serverID);
void ISHLLMSetMCPServerToken(NSString *serverID, NSString *_Nullable token);

@interface ISHLLMMCPManager : NSObject
+ (instancetype)shared;
// Connects every enabled server that is not connected yet (reconnecting any
// whose settings changed), then completes on main with one line per server
// that failed just now. A server that failed is left alone for five minutes
// unless its settings change or a Check succeeds. Cheap when everything is up.
- (void)prepareWithCompletion:(void (^)(NSArray<NSString *> *problems))completion;
// The connected servers' tools, as OpenAI function tools. Main thread.
- (NSArray<NSDictionary *> *)toolDefinitions;
- (BOOL)knowsTool:(NSString *)exposedName;
// "server: tool" for a tool name the model used.
- (NSString *)describeTool:(NSString *)exposedName;
// Blocks: call off the main thread. Returns the text for the model.
- (NSString *)callTool:(NSString *)exposedName arguments:(NSDictionary *)arguments
               isError:(BOOL *_Nullable)isErrorOut summary:(NSString *_Nullable *_Nullable)summaryOut;
// Connects one server on its own (for Settings' "Check"), completing on main
// with its tool names or an error.
- (void)checkServer:(NSDictionary<NSString *, id> *)server
         completion:(void (^)(NSArray<NSString *> *_Nullable toolNames, NSString *_Nullable error))completion;
// Drops every connection (stops guest servers); the next prepare reconnects.
- (void)disconnectAll;
@end

NS_ASSUME_NONNULL_END
