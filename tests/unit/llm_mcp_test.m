// Host unit test for the transport-independent MCP client code
// (app/LLMChatMCPCore.m): finding a reply among what a server sent (JSON,
// a batch, or an event stream with notifications and server requests mixed
// in), tool names every provider accepts, and results turned into text.
//
// Built by meson on macOS (`meson test -C build llm_mcp`), or alone:
//   clang -fobjc-arc -framework Foundation -Iapp app/LLMChatMCPCore.m \
//         tests/unit/llm_mcp_test.m -o /tmp/t && /tmp/t

#import <Foundation/Foundation.h>
#import "LLMChatMCPCore.h"

static int failures = 0;

#define CHECK(cond, ...) do { \
    if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } \
} while (0)

static NSData *data(NSString *text) {
    return [text dataUsingEncoding:NSUTF8StringEncoding];
}

static void test_find_reply(void) {
    NSDictionary *r = ISHLLMMCPResponseInBody(data(@"{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{\"ok\":true}}"), @"application/json", 3);
    CHECK([r[@"result"][@"ok"] boolValue], "plain JSON reply");
    CHECK(ISHLLMMCPResponseInBody(data(@"{\"jsonrpc\":\"2.0\",\"id\":4,\"result\":{}}"), @"application/json", 3) == nil, "another id is not the reply");
    r = ISHLLMMCPResponseInBody(data(@"[{\"jsonrpc\":\"2.0\",\"method\":\"notifications/progress\"},{\"jsonrpc\":\"2.0\",\"id\":7,\"result\":{\"n\":1}}]"), nil, 7);
    CHECK([r[@"result"][@"n"] integerValue] == 1, "reply inside a batch");
    NSString *stream = @"event: message\n"
        "data: {\"jsonrpc\":\"2.0\",\"method\":\"notifications/message\",\"params\":{}}\n\n"
        "data: {\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"roots/list\"}\n\n"
        "id: 1\ndata: {\"jsonrpc\":\"2.0\",\n"
        "data:  \"id\":9,\"result\":{\"tools\":[]}}\n\n";
    r = ISHLLMMCPResponseInBody(data(stream), @"text/event-stream; charset=utf-8", 9);
    CHECK(r != nil && [r[@"result"][@"tools"] isKindOfClass:NSArray.class], "reply in an event stream, after a notification and a server request with the same id, split over two data lines");
    r = ISHLLMMCPResponseInBody(data(@"data: {\"jsonrpc\":\"2.0\",\"id\":\"5\",\"result\":{}}\r\n\r\n"), @"text/event-stream", 5);
    CHECK(r != nil, "CRLF stream and a string id");
    NSString *error = nil;
    CHECK(ISHLLMMCPResult(@{@"id": @1, @"error": @{@"code": @-32601, @"message": @"Method not found"}}, &error) == nil && [error containsString:@"Method not found"] && [error containsString:@"-32601"], "error reply: %s", error.UTF8String);
    CHECK(ISHLLMMCPResult(@{@"id": @1, @"result": @{@"a": @1}}, &error) != nil && error == nil, "result reply");
}

static void test_names(void) {
    CHECK([ISHLLMMCPToolName(@"GitHub", @"search_issues") isEqualToString:@"mcp__GitHub__search_issues"], "plain");
    CHECK([ISHLLMMCPToolName(@"my server", @"read.file") isEqualToString:@"mcp__my_server__read_file"], "spaces and dots become _: %s", ISHLLMMCPToolName(@"my server", @"read.file").UTF8String);
    NSString *longName = ISHLLMMCPToolName(@"a-very-long-server-name-that-goes-on-and-on", @"a_tool_with_a_long_name_too");
    CHECK(longName.length <= 64 && [longName hasSuffix:@"__a_tool_with_a_long_name_too"], "long names keep the tool part: %s", longName.UTF8String);
    NSDictionary *map = nil;
    NSArray *tools = ISHLLMMCPFunctionTools(@"docs", @[
        @{@"name": @"search", @"description": @"Search the docs.", @"inputSchema": @{@"type": @"object", @"properties": @{@"q": @{@"type": @"string"}}}},
        @{@"name": @"no-schema"},
        @{@"description": @"nameless"},
    ], &map);
    CHECK(tools.count == 2, "nameless tool skipped");
    CHECK([tools[0][@"function"][@"name"] isEqual:@"mcp__docs__search"] && [tools[0][@"function"][@"parameters"][@"properties"][@"q"] isKindOfClass:NSDictionary.class], "schema passed through");
    CHECK([tools[1][@"function"][@"parameters"][@"type"] isEqual:@"object"], "missing schema becomes an empty object schema");
    CHECK([map[@"mcp__docs__no-schema"] isEqual:@"no-schema"], "name map back to the server's name");
}

static void test_results(void) {
    BOOL isError = YES;
    NSString *text = ISHLLMMCPResultText(@{@"content": @[@{@"type": @"text", @"text": @"one"}, @{@"type": @"image", @"mimeType": @"image/png", @"data": @"AAAA"}, @{@"type": @"text", @"text": @"two"}]}, &isError);
    CHECK([text isEqualToString:@"one\n[image content, image/png]\ntwo"] && !isError, "text joined, image named: %s", text.UTF8String);
    text = ISHLLMMCPResultText(@{@"content": @[@{@"type": @"text", @"text": @"boom"}], @"isError": @YES}, &isError);
    CHECK(isError && [text isEqualToString:@"boom"], "isError reported");
    text = ISHLLMMCPResultText(@{@"content": @[], @"structuredContent": @{@"sum": @5}}, NULL);
    CHECK([text isEqualToString:@"{\"sum\":5}"], "structured content when there is no text: %s", text.UTF8String);
    text = ISHLLMMCPResultText(@{@"content": @[@{@"type": @"resource", @"resource": @{@"uri": @"file:///a", @"text": @"body"}}]}, NULL);
    CHECK([text isEqualToString:@"body"], "embedded resource text");
    CHECK([ISHLLMMCPResultText(@{}, NULL) length] > 0, "empty result still says something");
}

int main(void) {
    @autoreleasepool {
        test_find_reply();
        test_names();
        test_results();
        CHECK([ISHLLMMCPRequest(2, @"tools/list", nil)[@"id"] isEqual:@2] && [ISHLLMMCPNotification(@"notifications/initialized", nil)[@"id"] == nil ? @YES : @NO boolValue], "request has an id, notification none");
    }
    if (failures == 0)
        printf("llm_mcp: all passed\n");
    return failures == 0 ? 0 : 1;
}
