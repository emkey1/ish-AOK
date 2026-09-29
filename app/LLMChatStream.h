//
//  LLMChatStream.h
//  iSH-AOK
//
//  Streamed replies put back together, so the tool loop can show a reply as
//  it arrives and still end with exactly the message a non-streaming request
//  returns. One assembler per wire format; feed it each SSE `data:` payload
//  in order.
//
//  Foundation only, tested on the host (tests/unit/llm_stream_test.m).
//

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

// OpenAI chat completions with "stream": true: text in choices[0].delta
// .content, tool calls in delta.tool_calls fragments keyed by index (the id
// and name arrive once, the arguments in pieces).
@interface ISHLLMOpenAIStreamAssembler : NSObject
// The visible text this payload adds, or nil.
- (nullable NSString *)consumePayload:(NSString *)payload;
@property (nonatomic, readonly) BOOL sawEvents;
// {"role": "assistant", "content": ..., "tool_calls": [...]}, shaped like a
// non-streaming response's choices[0].message; an "error" payload makes it
// {"error": ...} instead.
- (NSDictionary *)responseMessage;
@property (nonatomic, readonly, nullable) NSDictionary *error;
@end

// Anthropic Messages with "stream": true: message_start, then per content
// block content_block_start / _delta / _stop, message_delta with the stop
// reason, message_stop. Blocks are rebuilt exactly -- thinking text and
// signature, tool_use input from its partial JSON -- because they are sent
// back verbatim on the next request.
@interface ISHLLMAnthropicStreamAssembler : NSObject
- (nullable NSString *)consumePayload:(NSString *)payload;
@property (nonatomic, readonly) BOOL sawEvents;
// A dictionary in the shape of a non-streaming Messages response ("content",
// "stop_reason", "stop_details", or "error"), for
// ISHLLMAnthropicMessageFromResponse.
- (NSDictionary *)response;
// message_stop arrived: a stream that ends without it was cut off.
@property (nonatomic, readonly) BOOL finished;
@end

NS_ASSUME_NONNULL_END
