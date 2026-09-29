// Host unit test for the LLM Chat's Anthropic Messages API translation
// (app/LLMChatAnthropic.m). The API rejects a request whose tool results are
// split across turns, whose tool_result blocks follow text, or whose first
// turn is not the user's -- and a stable system prompt that is not cached,
// or a clock that sits before the cache breakpoint, costs money on every
// request without any error. Each of those is checked here.
//
// Built by meson on macOS (`meson test -C build llm_anthropic`), or alone:
//   clang -fobjc-arc -framework Foundation -Iapp app/LLMChatAnthropic.m \
//         tests/unit/llm_anthropic_test.m -o /tmp/t && /tmp/t

#import <Foundation/Foundation.h>
#import "LLMChatAnthropic.h"

static int failures = 0;

#define CHECK(cond, ...) do { \
    if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } \
} while (0)

static NSString *json(id object) {
    NSData *data = [NSJSONSerialization dataWithJSONObject:object options:NSJSONWritingSortedKeys error:nil];
    return [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
}

static NSDictionary *toolCall(NSString *identifier, NSString *name, NSString *arguments) {
    return @{@"id": identifier, @"type": @"function", @"function": @{@"name": name, @"arguments": arguments}};
}

static void test_system_and_caching(void) {
    NSDictionary *body = ISHLLMAnthropicRequestBody(@"claude-sonnet-5", 16000, @[
        @{@"role": @"system", @"content": @"Be brief."},
        @{@"role": @"user", @"content": @"hi"},
    ], @"Tool guidance.", @"The time is now.", nil);
    NSArray *system = body[@"system"];
    CHECK(system.count == 2, "stable and volatile system blocks: %s", json(system).UTF8String);
    CHECK([system[0][@"text"] isEqualToString:@"Be brief.\n\nTool guidance."], "chat system prompt first, then tool guidance: %s", [system[0][@"text"] UTF8String]);
    CHECK([system[0][@"cache_control"][@"type"] isEqual:@"ephemeral"], "stable system block carries the breakpoint");
    CHECK(system[1][@"cache_control"] == nil && [system[1][@"text"] isEqualToString:@"The time is now."], "clock after the breakpoint, uncached");
    CHECK([body[@"cache_control"][@"type"] isEqual:@"ephemeral"], "automatic caching for the conversation");
    CHECK([body[@"max_tokens"] integerValue] == 16000, "max_tokens");
    for (NSDictionary *turn in body[@"messages"])
        CHECK(![turn[@"role"] isEqual:@"system"], "no system role left in messages");
    CHECK(body[@"fallbacks"] == nil, "no fallbacks for sonnet");
}

static void test_tool_round(void) {
    NSArray *messages = @[
        @{@"role": @"user", @"content": @"list and read"},
        @{@"role": @"assistant", @"content": @"Looking.", @"tool_calls": @[
            toolCall(@"t1", @"list_directory", @"{\"path\":\"/root\"}"),
            toolCall(@"t2", @"read_file", @"{\"path\":\"a.txt\",\"limit\":5}"),
        ]},
        @{@"role": @"tool", @"tool_call_id": @"t1", @"name": @"list_directory", @"content": @"a.txt"},
        @{@"role": @"tool", @"tool_call_id": @"t2", @"name": @"read_file", @"content": @""},
        @{@"role": @"user", @"content": @"(I reverted your change.)"},
    ];
    NSDictionary *body = ISHLLMAnthropicRequestBody(@"claude-opus-5", 16000, messages, nil, nil, @[
        @{@"type": @"function", @"function": @{@"name": @"read_file", @"description": @"Read.", @"parameters": @{@"type": @"object", @"properties": @{@"path": @{@"type": @"string"}}, @"required": @[@"path"]}}},
    ]);
    NSArray *turns = body[@"messages"];
    CHECK(turns.count == 3, "user, assistant, one user turn of results: %s", json(turns).UTF8String);
    NSArray *assistant = turns[1][@"content"];
    CHECK([assistant[0][@"type"] isEqual:@"text"] && [assistant[1][@"type"] isEqual:@"tool_use"] && [assistant[2][@"type"] isEqual:@"tool_use"], "text then tool_use blocks");
    CHECK([assistant[2][@"input"][@"limit"] integerValue] == 5 && [assistant[2][@"input"][@"path"] isEqual:@"a.txt"], "arguments string became an input object");
    NSArray *results = turns[2][@"content"];
    CHECK(results.count == 3, "both results and the note in one user turn");
    CHECK([results[0][@"type"] isEqual:@"tool_result"] && [results[0][@"tool_use_id"] isEqual:@"t1"], "first result");
    CHECK([results[1][@"type"] isEqual:@"tool_result"] && [results[1][@"content"] isEqual:@"(no output)"], "empty result gets text");
    CHECK([results[2][@"type"] isEqual:@"text"], "text after the results");
    NSArray *tools = body[@"tools"];
    CHECK(tools.count == 1 && [tools[0][@"input_schema"][@"required"][0] isEqual:@"path"] && tools[0][@"function"] == nil, "tool as name/description/input_schema: %s", json(tools).UTF8String);
    CHECK([body[@"fallbacks"] isEqual:@"default"], "claude-opus-5 opts into fallbacks");
}

static void test_ordering_rules(void) {
    // Text already in a user turn, then tool results arriving: results first.
    NSDictionary *body = ISHLLMAnthropicRequestBody(@"m", 100, @[
        @{@"role": @"assistant", @"content": @"", @"tool_calls": @[toolCall(@"x", @"grep", @"{}")]},
        @{@"role": @"user", @"content": @"note"},
        @{@"role": @"tool", @"tool_call_id": @"x", @"content": @"r"},
    ], nil, nil, nil);
    NSArray *turns = body[@"messages"];
    CHECK([turns[0][@"role"] isEqual:@"user"], "a conversation opening with the assistant gets a user turn first");
    NSArray *last = turns.lastObject[@"content"];
    CHECK([last[0][@"type"] isEqual:@"tool_result"] && [last[1][@"type"] isEqual:@"text"], "tool_result moved before text: %s", json(last).UTF8String);
    CHECK([turns[1][@"content"] count] == 1 && [turns[1][@"content"][0][@"type"] isEqual:@"tool_use"], "empty assistant text dropped, tool_use kept");

    // Consecutive assistant turns merge; an empty user turn vanishes.
    body = ISHLLMAnthropicRequestBody(@"m", 100, @[
        @{@"role": @"user", @"content": @"q"},
        @{@"role": @"assistant", @"content": @"a"},
        @{@"role": @"assistant", @"content": @"(stopped)"},
        @{@"role": @"user", @"content": @""},
    ], nil, nil, nil);
    turns = body[@"messages"];
    CHECK(turns.count == 2 && [turns[1][@"content"] count] == 2, "assistant turns merged: %s", json(turns).UTF8String);
}

static void test_kept_content(void) {
    NSArray *kept = @[
        @{@"type": @"thinking", @"thinking": @"", @"signature": @"sig=="},
        @{@"type": @"tool_use", @"id": @"t9", @"name": @"glob", @"input": @{@"pattern": @"*.c"}},
    ];
    NSDictionary *body = ISHLLMAnthropicRequestBody(@"m", 100, @[
        @{@"role": @"user", @"content": @"q"},
        @{@"role": @"assistant", @"content": @"", @"tool_calls": @[toolCall(@"t9", @"glob", @"{}")], kISHLLMAnthropicContentKey: kept},
        @{@"role": @"tool", @"tool_call_id": @"t9", @"content": @"a.c"},
    ], nil, nil, nil);
    CHECK([json(body[@"messages"][1][@"content"]) isEqualToString:json(kept)], "kept content sent back unchanged, thinking signature and all");
}

static void test_responses(void) {
    NSString *error = nil, *note = nil;
    NSDictionary *message = ISHLLMAnthropicMessageFromResponse(@{
        @"type": @"message", @"role": @"assistant", @"stop_reason": @"tool_use",
        @"content": @[
            @{@"type": @"thinking", @"thinking": @"", @"signature": @"s"},
            @{@"type": @"text", @"text": @"Let me look."},
            @{@"type": @"tool_use", @"id": @"toolu_1", @"name": @"read_file", @"input": @{@"path": @"x/ü.txt", @"offset": @3}},
        ],
    }, &error, &note);
    CHECK(message != nil && error == nil, "tool_use response parsed");
    CHECK([message[@"content"] isEqual:@"Let me look."], "text only, no thinking");
    NSDictionary *call = [message[@"tool_calls"] firstObject];
    NSDictionary *arguments = [NSJSONSerialization JSONObjectWithData:[call[@"function"][@"arguments"] dataUsingEncoding:NSUTF8StringEncoding] options:0 error:nil];
    CHECK([call[@"id"] isEqual:@"toolu_1"] && [arguments[@"path"] isEqual:@"x/ü.txt"] && [arguments[@"offset"] integerValue] == 3, "tool_use became an OpenAI tool call");
    CHECK([message[kISHLLMAnthropicContentKey] count] == 3, "content kept for the next request");

    message = ISHLLMAnthropicMessageFromResponse(@{@"type": @"error", @"error": @{@"type": @"overloaded_error", @"message": @"Overloaded"}}, &error, NULL);
    CHECK(message == nil && [error containsString:@"overloaded_error"] && [error containsString:@"Overloaded"], "API error reported: %s", error.UTF8String);

    message = ISHLLMAnthropicMessageFromResponse(@{@"content": @[], @"stop_reason": @"refusal", @"stop_details": @{@"type": @"refusal", @"explanation": @"Not this."}}, &error, NULL);
    CHECK(message == nil && [error containsString:@"Not this."], "refusal reported, not shown as a reply: %s", error.UTF8String);

    message = ISHLLMAnthropicMessageFromResponse(@{@"content": @[@{@"type": @"text", @"text": @"partial"}], @"stop_reason": @"max_tokens"}, &error, &note);
    CHECK(message != nil && note.length > 0, "cut-off reply keeps its text and gets a note");

    message = ISHLLMAnthropicMessageFromResponse(@{@"content": @[
        @{@"type": @"fallback", @"from": @{@"model": @"claude-opus-5"}, @"to": @{@"model": @"claude-opus-4-8"}},
        @{@"type": @"text", @"text": @"answer"}], @"stop_reason": @"end_turn"}, &error, &note);
    CHECK([message[@"content"] isEqual:@"answer"] && note == nil, "fallback block skipped in the text");
}

static void test_fallback_models(void) {
    CHECK(ISHLLMAnthropicModelTakesFallbacks(@"claude-opus-5"), "opus 5");
    CHECK(ISHLLMAnthropicModelTakesFallbacks(@"claude-fable-5-1"), "fable 5.1");
    CHECK(!ISHLLMAnthropicModelTakesFallbacks(@"claude-opus-5-5"), "not opus 5.5");
    CHECK(!ISHLLMAnthropicModelTakesFallbacks(@"claude-sonnet-5"), "not sonnet 5");
}

int main(void) {
    @autoreleasepool {
        test_system_and_caching();
        test_tool_round();
        test_ordering_rules();
        test_kept_content();
        test_responses();
        test_fallback_models();
    }
    if (failures == 0)
        printf("llm_anthropic: all passed\n");
    return failures == 0 ? 0 : 1;
}
