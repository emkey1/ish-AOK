//
//  LLMChatMCPCore.m
//  iSH-AOK
//
//  See LLMChatMCPCore.h.
//

#import "LLMChatMCPCore.h"

NSString *const kISHLLMMCPProtocolVersion = @"2025-06-18";
NSString *const kISHLLMMCPToolPrefix = @"mcp__";

NSDictionary *ISHLLMMCPRequest(NSInteger identifier, NSString *method, NSDictionary *params) {
    NSMutableDictionary *request = [@{@"jsonrpc": @"2.0", @"id": @(identifier), @"method": method} mutableCopy];
    if (params != nil)
        request[@"params"] = params;
    return request;
}

NSDictionary *ISHLLMMCPNotification(NSString *method, NSDictionary *params) {
    NSMutableDictionary *notification = [@{@"jsonrpc": @"2.0", @"method": method} mutableCopy];
    if (params != nil)
        notification[@"params"] = params;
    return notification;
}

NSDictionary *ISHLLMMCPInitializeParams(void) {
    NSString *version = [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"] ?: @"1";
    return @{
        @"protocolVersion": kISHLLMMCPProtocolVersion,
        @"capabilities": @{},
        @"clientInfo": @{@"name": @"iSH-AOK LLM Chat", @"version": version},
    };
}

static NSDictionary *ISHLLMMCPMatch(id message, NSInteger identifier) {
    if ([message isKindOfClass:NSArray.class]) {
        for (id each in message) {
            NSDictionary *found = ISHLLMMCPMatch(each, identifier);
            if (found != nil)
                return found;
        }
        return nil;
    }
    if (![message isKindOfClass:NSDictionary.class])
        return nil;
    // A response has an id and a result or an error, and no method; a server
    // request has both an id and a method.
    id messageID = message[@"id"];
    if (message[@"method"] != nil || messageID == nil)
        return nil;
    if (([messageID isKindOfClass:NSNumber.class] && [messageID integerValue] == identifier) ||
        ([messageID isKindOfClass:NSString.class] && [messageID integerValue] == identifier && [messageID length] > 0))
        return message;
    return nil;
}

NSDictionary *ISHLLMMCPResponseInBody(NSData *body, NSString *contentType, NSInteger identifier) {
    if (body.length == 0)
        return nil;
    if ([contentType.lowercaseString containsString:@"text/event-stream"]) {
        NSString *text = [[NSString alloc] initWithData:body encoding:NSUTF8StringEncoding] ?: @"";
        // An event's data may span several data: lines, joined by newlines.
        NSMutableString *data = [NSMutableString string];
        NSArray<NSString *> *lines = [[text stringByReplacingOccurrencesOfString:@"\r\n" withString:@"\n"] componentsSeparatedByString:@"\n"];
        for (NSUInteger i = 0; i <= lines.count; i++) {
            NSString *line = i < lines.count ? lines[i] : @"";
            if ([line hasPrefix:@"data:"]) {
                NSString *part = [line substringFromIndex:5];
                if ([part hasPrefix:@" "])
                    part = [part substringFromIndex:1];
                if (data.length > 0)
                    [data appendString:@"\n"];
                [data appendString:part];
            } else if (line.length == 0 && data.length > 0) {
                id message = [NSJSONSerialization JSONObjectWithData:[data dataUsingEncoding:NSUTF8StringEncoding] options:0 error:nil];
                NSDictionary *found = ISHLLMMCPMatch(message, identifier);
                if (found != nil)
                    return found;
                [data setString:@""];
            }
        }
        return nil;
    }
    id message = [NSJSONSerialization JSONObjectWithData:body options:0 error:nil];
    return ISHLLMMCPMatch(message, identifier);
}

NSDictionary *ISHLLMMCPResult(NSDictionary *response, NSString **errorOut) {
    if (errorOut != NULL)
        *errorOut = nil;
    NSDictionary *error = [response[@"error"] isKindOfClass:NSDictionary.class] ? response[@"error"] : nil;
    if (error != nil) {
        if (errorOut != NULL) {
            NSString *message = [error[@"message"] isKindOfClass:NSString.class] ? error[@"message"] : @"error";
            *errorOut = [NSString stringWithFormat:@"%@ (code %@)", message, error[@"code"] ?: @"?"];
        }
        return nil;
    }
    NSDictionary *result = [response[@"result"] isKindOfClass:NSDictionary.class] ? response[@"result"] : nil;
    if (result == nil && errorOut != NULL)
        *errorOut = @"the reply had no result";
    return result;
}

static NSString *ISHLLMMCPSanitize(NSString *text) {
    NSMutableString *out = [NSMutableString string];
    for (NSUInteger i = 0; i < text.length; i++) {
        unichar c = [text characterAtIndex:i];
        BOOL ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        [out appendString:ok ? [NSString stringWithCharacters:&c length:1] : @"_"];
    }
    return out;
}

NSString *ISHLLMMCPToolName(NSString *serverName, NSString *toolName) {
    NSString *server = ISHLLMMCPSanitize(serverName.length > 0 ? serverName : @"server");
    NSString *tool = ISHLLMMCPSanitize(toolName);
    NSString *name = [NSString stringWithFormat:@"%@%@__%@", kISHLLMMCPToolPrefix, server, tool];
    if (name.length <= 64)
        return name;
    // Keep the tool's own name whole where possible: shorten the server part.
    NSInteger room = 64 - (NSInteger) kISHLLMMCPToolPrefix.length - 2 - (NSInteger) tool.length;
    if (room >= 3)
        return [NSString stringWithFormat:@"%@%@__%@", kISHLLMMCPToolPrefix, [server substringToIndex:(NSUInteger) MIN(room, (NSInteger) server.length)], tool];
    return [name substringToIndex:64];
}

NSArray<NSDictionary *> *ISHLLMMCPFunctionTools(NSString *serverName, NSArray *tools, NSDictionary<NSString *, NSString *> **nameMapOut) {
    NSMutableArray *functions = [NSMutableArray array];
    NSMutableDictionary *map = [NSMutableDictionary dictionary];
    for (id tool in tools) {
        if (![tool isKindOfClass:NSDictionary.class])
            continue;
        NSString *name = [tool[@"name"] isKindOfClass:NSString.class] ? tool[@"name"] : nil;
        if (name.length == 0)
            continue;
        NSString *exposed = ISHLLMMCPToolName(serverName, name);
        if (map[exposed] != nil)
            continue; // two tools that sanitise alike: keep the first
        map[exposed] = name;
        NSString *description = [tool[@"description"] isKindOfClass:NSString.class] ? tool[@"description"] : @"";
        NSDictionary *schema = [tool[@"inputSchema"] isKindOfClass:NSDictionary.class] ? tool[@"inputSchema"] : @{@"type": @"object", @"properties": @{}};
        [functions addObject:@{
            @"type": @"function",
            @"function": @{
                @"name": exposed,
                @"description": [NSString stringWithFormat:@"(MCP server \"%@\") %@", serverName, description],
                @"parameters": schema,
            },
        }];
    }
    if (nameMapOut != NULL)
        *nameMapOut = map;
    return functions;
}

NSString *ISHLLMMCPResultText(NSDictionary *result, BOOL *isErrorOut) {
    if (isErrorOut != NULL)
        *isErrorOut = [result[@"isError"] boolValue];
    NSMutableArray<NSString *> *parts = [NSMutableArray array];
    for (id item in [result[@"content"] isKindOfClass:NSArray.class] ? result[@"content"] : @[]) {
        if (![item isKindOfClass:NSDictionary.class])
            continue;
        NSString *type = [item[@"type"] isKindOfClass:NSString.class] ? item[@"type"] : @"";
        if ([type isEqualToString:@"text"] && [item[@"text"] isKindOfClass:NSString.class]) {
            [parts addObject:item[@"text"]];
        } else if ([type isEqualToString:@"resource"] && [item[@"resource"] isKindOfClass:NSDictionary.class]) {
            NSDictionary *resource = item[@"resource"];
            NSString *text = [resource[@"text"] isKindOfClass:NSString.class] ? resource[@"text"] : nil;
            [parts addObject:text ?: [NSString stringWithFormat:@"[resource %@]", resource[@"uri"] ?: @""]];
        } else if ([type isEqualToString:@"resource_link"]) {
            [parts addObject:[NSString stringWithFormat:@"[resource link %@]", item[@"uri"] ?: @""]];
        } else if (type.length > 0) {
            [parts addObject:[NSString stringWithFormat:@"[%@ content, %@]", type, item[@"mimeType"] ?: @"not shown"]];
        }
    }
    if (parts.count == 0 && result[@"structuredContent"] != nil && [NSJSONSerialization isValidJSONObject:@[result[@"structuredContent"]]]) {
        NSData *json = [NSJSONSerialization dataWithJSONObject:result[@"structuredContent"] options:NSJSONWritingSortedKeys | NSJSONWritingWithoutEscapingSlashes error:nil];
        [parts addObject:[[NSString alloc] initWithData:json encoding:NSUTF8StringEncoding] ?: @""];
    }
    return parts.count > 0 ? [parts componentsJoinedByString:@"\n"] : @"(the tool returned no content)";
}
