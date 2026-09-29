// Host unit test for the LLM Chat's stream assemblers (app/LLMChatStream.m).
// A streamed reply must end as exactly the message the same request would
// have returned without streaming: the tool loop acts on it, and for
// Anthropic its content blocks -- thinking signature included -- are sent
// back verbatim. Event sequences follow the documented formats, split at
// awkward places (inside a JSON string, inside a signature).
//
// Built by meson on macOS (`meson test -C build llm_stream`), or alone:
//   clang -fobjc-arc -framework Foundation -Iapp app/LLMChatStream.m \
//         app/LLMChatAnthropic.m tests/unit/llm_stream_test.m -o /tmp/t && /tmp/t

#import <Foundation/Foundation.h>
#import "LLMChatStream.h"
#import "LLMChatAnthropic.h"

static int failures = 0;

#define CHECK(cond, ...) do { \
    if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } \
} while (0)

static NSString *payload(id object) {
    NSData *data = [NSJSONSerialization dataWithJSONObject:object options:0 error:nil];
    return [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
}

static NSDictionary *chunk(NSDictionary *delta) {
    return @{@"choices": @[@{@"index": @0, @"delta": delta}]};
}

static void test_openai(void) {
    ISHLLMOpenAIStreamAssembler *a = [ISHLLMOpenAIStreamAssembler new];
    NSMutableString *shown = [NSMutableString string];
    NSArray *events = @[
        chunk(@{@"role": @"assistant", @"content": @""}),
        chunk(@{@"content": @"Let me "}),
        chunk(@{@"content": @"look."}),
        chunk(@{@"tool_calls": @[@{@"index": @0, @"id": @"call_1", @"type": @"function", @"function": @{@"name": @"read_file", @"arguments": @""}}]}),
        chunk(@{@"tool_calls": @[@{@"index": @0, @"function": @{@"arguments": @"{\"pa"}}]}),
        chunk(@{@"tool_calls": @[@{@"index": @1, @"id": @"call_2", @"function": @{@"name": @"grep", @"arguments": @"{\"pattern\""}}]}),
        chunk(@{@"tool_calls": @[@{@"index": @0, @"function": @{@"arguments": @"th\":\"a b.txt\"}"}}]}),
        chunk(@{@"tool_calls": @[@{@"index": @1, @"function": @{@"arguments": @":\"x\"}"}}]}),
        @{@"choices": @[@{@"index": @0, @"delta": @{}, @"finish_reason": @"tool_calls"}]},
    ];
    for (NSDictionary *event in events) {
        NSString *text = [a consumePayload:payload(event)];
        if (text != nil)
            [shown appendString:text];
    }
    NSDictionary *message = a.responseMessage;
    CHECK([shown isEqualToString:@"Let me look."] && [message[@"content"] isEqualToString:@"Let me look."], "text streamed and kept: %s", shown.UTF8String);
    NSArray *calls = message[@"tool_calls"];
    CHECK(calls.count == 2, "two tool calls");
    CHECK([calls[0][@"id"] isEqual:@"call_1"] && [calls[0][@"function"][@"name"] isEqual:@"read_file"] && [calls[0][@"function"][@"arguments"] isEqual:@"{\"path\":\"a b.txt\"}"], "call 1 assembled across interleaved fragments: %s", [calls[0][@"function"][@"arguments"] UTF8String]);
    CHECK([calls[1][@"function"][@"arguments"] isEqual:@"{\"pattern\":\"x\"}"], "call 2 assembled");

    // No index at all (some local servers): one call continued by fragments.
    ISHLLMOpenAIStreamAssembler *b = [ISHLLMOpenAIStreamAssembler new];
    [b consumePayload:payload(chunk(@{@"tool_calls": @[@{@"id": @"c9", @"function": @{@"name": @"glob", @"arguments": @"{\"pat"}}]}))];
    [b consumePayload:payload(chunk(@{@"tool_calls": @[@{@"function": @{@"arguments": @"tern\":\"*.c\"}"}}]}))];
    NSArray *bc = b.responseMessage[@"tool_calls"];
    CHECK(bc.count == 1 && [bc[0][@"function"][@"arguments"] isEqual:@"{\"pattern\":\"*.c\"}"], "index-less fragments join one call");

    ISHLLMOpenAIStreamAssembler *c = [ISHLLMOpenAIStreamAssembler new];
    [c consumePayload:@"not json"];
    CHECK(!c.sawEvents, "garbage is not an event");
    [c consumePayload:payload(@{@"error": @{@"message": @"rate limited"}})];
    CHECK([c.responseMessage[@"error"][@"message"] isEqual:@"rate limited"], "stream error surfaces");
}

static void test_anthropic(void) {
    ISHLLMAnthropicStreamAssembler *a = [ISHLLMAnthropicStreamAssembler new];
    NSMutableString *shown = [NSMutableString string];
    NSArray *events = @[
        @{@"type": @"message_start", @"message": @{@"id": @"msg_1", @"type": @"message", @"role": @"assistant", @"content": @[]}},
        @{@"type": @"content_block_start", @"index": @0, @"content_block": @{@"type": @"thinking", @"thinking": @"", @"signature": @""}},
        @{@"type": @"content_block_delta", @"index": @0, @"delta": @{@"type": @"thinking_delta", @"thinking": @"Look at "}},
        @{@"type": @"content_block_delta", @"index": @0, @"delta": @{@"type": @"thinking_delta", @"thinking": @"the dir."}},
        @{@"type": @"content_block_delta", @"index": @0, @"delta": @{@"type": @"signature_delta", @"signature": @"AbC"}},
        @{@"type": @"content_block_delta", @"index": @0, @"delta": @{@"type": @"signature_delta", @"signature": @"dEf=="}},
        @{@"type": @"content_block_stop", @"index": @0},
        @{@"type": @"content_block_start", @"index": @1, @"content_block": @{@"type": @"text", @"text": @""}},
        @{@"type": @"content_block_delta", @"index": @1, @"delta": @{@"type": @"text_delta", @"text": @"Check"}},
        @{@"type": @"content_block_delta", @"index": @1, @"delta": @{@"type": @"text_delta", @"text": @"ing."}},
        @{@"type": @"content_block_stop", @"index": @1},
        @{@"type": @"content_block_start", @"index": @2, @"content_block": @{@"type": @"tool_use", @"id": @"toolu_1", @"name": @"list_directory", @"input": @{}}},
        @{@"type": @"content_block_delta", @"index": @2, @"delta": @{@"type": @"input_json_delta", @"partial_json": @""}},
        @{@"type": @"content_block_delta", @"index": @2, @"delta": @{@"type": @"input_json_delta", @"partial_json": @"{\"path\": \"/ro"}},
        @{@"type": @"content_block_delta", @"index": @2, @"delta": @{@"type": @"input_json_delta", @"partial_json": @"ot\"}"}},
        @{@"type": @"content_block_stop", @"index": @2},
        @{@"type": @"message_delta", @"delta": @{@"stop_reason": @"tool_use"}, @"usage": @{@"output_tokens": @30}},
        @{@"type": @"message_stop"},
    ];
    for (NSDictionary *event in events) {
        NSString *text = [a consumePayload:payload(event)];
        if (text != nil)
            [shown appendString:text];
    }
    CHECK([shown isEqualToString:@"Checking."], "only answer text is shown, not thinking: %s", shown.UTF8String);
    CHECK(a.finished, "message_stop seen");
    NSDictionary *response = a.response;
    NSArray *content = response[@"content"];
    CHECK(content.count == 3, "three blocks");
    NSDictionary *thinking = @{@"type": @"thinking", @"thinking": @"Look at the dir.", @"signature": @"AbCdEf=="};
    NSDictionary *text = @{@"type": @"text", @"text": @"Checking."};
    NSDictionary *toolUse = @{@"type": @"tool_use", @"id": @"toolu_1", @"name": @"list_directory", @"input": @{@"path": @"/root"}};
    CHECK([content[0] isEqual:thinking], "thinking block rebuilt exactly: %s", payload(content[0]).UTF8String);
    CHECK([content[1] isEqual:text], "text block");
    CHECK([content[2] isEqual:toolUse], "tool_use input from partial JSON: %s", payload(content[2]).UTF8String);
    CHECK([response[@"stop_reason"] isEqual:@"tool_use"], "stop reason");

    // And it parses the way a non-streaming response does.
    NSDictionary *message = ISHLLMAnthropicMessageFromResponse(response, NULL, NULL);
    CHECK([message[@"content"] isEqual:@"Checking."] && [message[@"tool_calls"] count] == 1, "parses like a non-streaming response");

    // A tool_use with no input at all, and a stream that breaks off.
    ISHLLMAnthropicStreamAssembler *b = [ISHLLMAnthropicStreamAssembler new];
    [b consumePayload:payload(@{@"type": @"content_block_start", @"index": @0, @"content_block": @{@"type": @"tool_use", @"id": @"t", @"name": @"list_directory", @"input": @{}}})];
    [b consumePayload:payload(@{@"type": @"content_block_stop", @"index": @0})];
    NSDictionary *empty = @{};
    CHECK([b.response[@"content"][0][@"input"] isEqual:empty], "empty input is {}");
    CHECK(!b.finished, "no message_stop: not finished");

    ISHLLMAnthropicStreamAssembler *c = [ISHLLMAnthropicStreamAssembler new];
    [c consumePayload:payload(@{@"type": @"error", @"error": @{@"type": @"overloaded_error", @"message": @"Overloaded"}})];
    NSString *error = nil;
    CHECK(ISHLLMAnthropicMessageFromResponse(c.response, &error, NULL) == nil && [error containsString:@"overloaded_error"], "stream error event reported: %s", error.UTF8String);
}

int main(void) {
    @autoreleasepool {
        test_openai();
        test_anthropic();
    }
    if (failures == 0)
        printf("llm_stream: all passed\n");
    return failures == 0 ? 0 : 1;
}
