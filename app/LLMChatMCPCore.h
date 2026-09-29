//
//  LLMChatMCPCore.h
//  iSH-AOK
//
//  The transport-independent half of the LLM Chat's MCP client (Model Context
//  Protocol, JSON-RPC 2.0): building requests, finding the reply to one in
//  what a server sent, and translating tools and results between MCP and the
//  chat's OpenAI-style function tools.
//
//  Foundation only, tested on the host (tests/unit/llm_mcp_test.m). The
//  transports -- Streamable HTTP, and stdio to a server running in the guest
//  -- are in LLMChatMCP.m.
//

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

extern NSString *const kISHLLMMCPProtocolVersion;
// Every MCP tool the model sees is named mcp__<server>__<tool>.
extern NSString *const kISHLLMMCPToolPrefix;

NSDictionary *ISHLLMMCPRequest(NSInteger identifier, NSString *method, NSDictionary *_Nullable params);
NSDictionary *ISHLLMMCPNotification(NSString *method, NSDictionary *_Nullable params);
NSDictionary *ISHLLMMCPInitializeParams(void);

// The response to request `identifier` among the messages in `body`: a
// single JSON object, a JSON array (a batch), or a text/event-stream whose
// `data:` lines carry messages. nil if it is not there. Server requests and
// notifications that arrive alongside are skipped.
NSDictionary *_Nullable ISHLLMMCPResponseInBody(NSData *body, NSString *_Nullable contentType, NSInteger identifier);

// A reply's result, or nil with *errorOut set from its JSON-RPC error.
NSDictionary *_Nullable ISHLLMMCPResult(NSDictionary *response, NSString *_Nullable *_Nullable errorOut);

// A name safe for every provider's tool-name rules (letters, digits, _ and -,
// at most 64 characters).
NSString *ISHLLMMCPToolName(NSString *serverName, NSString *toolName);

// tools/list's `tools` as OpenAI function tools named by ISHLLMMCPToolName;
// nameMapOut maps each of those names back to the server's own tool name.
NSArray<NSDictionary *> *ISHLLMMCPFunctionTools(NSString *serverName, NSArray *tools,
                                                NSDictionary<NSString *, NSString *> *_Nullable *_Nullable nameMapOut);

// tools/call's result as text for the model: text content joined, other
// content types named, structuredContent as JSON when there is no text.
// *isErrorOut reports the result's isError.
NSString *ISHLLMMCPResultText(NSDictionary *result, BOOL *_Nullable isErrorOut);

NS_ASSUME_NONNULL_END
