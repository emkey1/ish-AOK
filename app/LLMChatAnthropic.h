//
//  LLMChatAnthropic.h
//  iSH-AOK
//
//  The LLM Chat's translation to and from Anthropic's Messages API
//  (POST /v1/messages). The chat keeps its conversation in the OpenAI chat
//  completions shape -- role user/assistant/tool/system, tool_calls with JSON
//  string arguments -- and this is the one place that knows the Anthropic
//  shape: tool_use / tool_result content blocks, a top-level system, tools
//  with input_schema, prompt caching.
//
//  Foundation only, tested on the host (tests/unit/llm_anthropic_test.m).
//

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

// On an assistant message, the response's own content blocks, kept so the
// next request can send them back unchanged: thinking blocks (and their
// signatures) must be returned exactly as received when a tool loop
// continues on the same model.
extern NSString *const kISHLLMAnthropicContentKey;

extern NSString *const kISHLLMAnthropicVersion; // anthropic-version header

// Whether a model takes the server-side refusal fallback (`fallbacks:
// "default"` with the server-side-fallback-2026-07-01 beta): Claude Opus 5
// and Claude Fable 5.1 / Mythos 5.1.
BOOL ISHLLMAnthropicModelTakesFallbacks(NSString *model);

// The request body.
//   messages     the conversation in the chat's shape; "system" entries
//                anywhere are gathered into the top-level system
//   stableSystem extra system text that stays the same between requests
//                (tool guidance, environment, AGENTS.md); cached
//   volatileSystem system text that changes every request (the clock); sent
//                after the cache breakpoint so it does not invalidate it
//   tools        OpenAI-style function tool definitions, or nil
// Consecutive messages of one role are merged (tool results become one user
// turn, as the API requires), an assistant turn with a kept content array is
// sent verbatim, and the conversation always starts with a user turn.
NSDictionary *ISHLLMAnthropicRequestBody(NSString *model, NSUInteger maxTokens,
                                         NSArray<NSDictionary *> *messages,
                                         NSString *_Nullable stableSystem,
                                         NSString *_Nullable volatileSystem,
                                         NSArray<NSDictionary *> *_Nullable tools);

// A response turned into the chat's assistant message: "content" (the text
// blocks joined), "tool_calls" (OpenAI style, arguments as a JSON string) if
// the model called tools, and kISHLLMAnthropicContentKey. nil with *errorOut
// for an API error, a refusal, or an unexpected shape; *noteOut gets a short
// remark worth showing (the reply was cut off at max_tokens).
NSDictionary *_Nullable ISHLLMAnthropicMessageFromResponse(NSDictionary *response,
                                                           NSString *_Nullable *_Nullable errorOut,
                                                           NSString *_Nullable *_Nullable noteOut);

NS_ASSUME_NONNULL_END
