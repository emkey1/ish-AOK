//
//  LLMChatAnthropic.m
//  iSH-AOK
//
//  See LLMChatAnthropic.h.
//

#import "LLMChatAnthropic.h"

NSString *const kISHLLMAnthropicContentKey = @"anthropicContent";
NSString *const kISHLLMAnthropicVersion = @"2023-06-01";

BOOL ISHLLMAnthropicModelTakesFallbacks(NSString *model) {
    for (NSString *prefix in @[@"claude-opus-5", @"claude-fable-5-1", @"claude-mythos-5-1"]) {
        // claude-opus-5 but not claude-opus-5-5, which is a different model
        // with its own rules.
        if ([model isEqualToString:prefix])
            return YES;
    }
    return NO;
}

static NSString *ISHLLMString(id value) {
    return [value isKindOfClass:NSString.class] ? value : @"";
}

static NSDictionary *ISHLLMTextBlock(NSString *text) {
    return @{@"type": @"text", @"text": text};
}

// OpenAI tool_calls carry arguments as a JSON string (Ollama sends the
// object itself); tool_use wants the object.
static NSDictionary *ISHLLMToolUseBlock(NSDictionary *toolCall) {
    NSDictionary *function = [toolCall[@"function"] isKindOfClass:NSDictionary.class] ? toolCall[@"function"] : @{};
    id arguments = function[@"arguments"];
    NSDictionary *input = @{};
    if ([arguments isKindOfClass:NSDictionary.class]) {
        input = arguments;
    } else if ([arguments isKindOfClass:NSString.class] && [arguments length] > 0) {
        id parsed = [NSJSONSerialization JSONObjectWithData:[arguments dataUsingEncoding:NSUTF8StringEncoding] options:0 error:nil];
        if ([parsed isKindOfClass:NSDictionary.class])
            input = parsed;
    }
    return @{@"type": @"tool_use", @"id": ISHLLMString(toolCall[@"id"]), @"name": ISHLLMString(function[@"name"]), @"input": input};
}

NSDictionary *ISHLLMAnthropicRequestBody(NSString *model, NSUInteger maxTokens,
                                         NSArray<NSDictionary *> *messages,
                                         NSString *stableSystem, NSString *volatileSystem,
                                         NSArray<NSDictionary *> *tools) {
    NSMutableArray<NSString *> *systemTexts = [NSMutableArray array];
    // Each entry: @{@"role": ..., @"content": NSMutableArray of blocks}.
    NSMutableArray<NSMutableDictionary *> *turns = [NSMutableArray array];

    void (^appendBlocks)(NSString *, NSArray *) = ^(NSString *role, NSArray *blocks) {
        if (blocks.count == 0)
            return;
        NSMutableDictionary *last = turns.lastObject;
        if (last != nil && [last[@"role"] isEqualToString:role]) {
            NSMutableArray *content = last[@"content"];
            // In a user turn every tool_result must come before any text.
            BOOL addingResults = [blocks.firstObject[@"type"] isEqual:@"tool_result"];
            if (addingResults && [role isEqualToString:@"user"]) {
                NSUInteger firstText = [content indexOfObjectPassingTest:^BOOL(NSDictionary *block, __unused NSUInteger i, __unused BOOL *stop) {
                    return ![block[@"type"] isEqual:@"tool_result"];
                }];
                if (firstText != NSNotFound) {
                    [content insertObjects:blocks atIndexes:[NSIndexSet indexSetWithIndexesInRange:NSMakeRange(firstText, blocks.count)]];
                    return;
                }
            }
            [content addObjectsFromArray:blocks];
            return;
        }
        [turns addObject:[@{@"role": role, @"content": [blocks mutableCopy]} mutableCopy]];
    };

    for (NSDictionary *message in messages) {
        NSString *role = ISHLLMString(message[@"role"]);
        NSString *content = ISHLLMString(message[@"content"]);
        if ([role isEqualToString:@"system"]) {
            if (content.length > 0)
                [systemTexts addObject:content];
        } else if ([role isEqualToString:@"user"]) {
            if (content.length > 0)
                appendBlocks(@"user", @[ISHLLMTextBlock(content)]);
        } else if ([role isEqualToString:@"tool"]) {
            appendBlocks(@"user", @[@{@"type": @"tool_result",
                                      @"tool_use_id": ISHLLMString(message[@"tool_call_id"]),
                                      @"content": content.length > 0 ? content : @"(no output)"}]);
        } else if ([role isEqualToString:@"assistant"]) {
            NSArray *kept = [message[kISHLLMAnthropicContentKey] isKindOfClass:NSArray.class] ? message[kISHLLMAnthropicContentKey] : nil;
            if (kept.count > 0) {
                appendBlocks(@"assistant", kept);
                continue;
            }
            NSMutableArray *blocks = [NSMutableArray array];
            if (content.length > 0)
                [blocks addObject:ISHLLMTextBlock(content)];
            NSArray *toolCalls = [message[@"tool_calls"] isKindOfClass:NSArray.class] ? message[@"tool_calls"] : @[];
            for (id toolCall in toolCalls) {
                if ([toolCall isKindOfClass:NSDictionary.class])
                    [blocks addObject:ISHLLMToolUseBlock(toolCall)];
            }
            appendBlocks(@"assistant", blocks);
        }
    }
    // The API wants the conversation to open with the user.
    if (turns.count == 0 || ![turns.firstObject[@"role"] isEqualToString:@"user"])
        [turns insertObject:[@{@"role": @"user", @"content": [@[ISHLLMTextBlock(@"(continuing the conversation)")] mutableCopy]} mutableCopy] atIndex:0];

    NSMutableDictionary *body = [@{
        @"model": model,
        @"max_tokens": @(maxTokens),
        @"messages": turns,
        // Automatic caching of the growing conversation, on top of the
        // explicit breakpoint on the stable system text below.
        @"cache_control": @{@"type": @"ephemeral"},
    } mutableCopy];

    // Render order is tools, system, messages: a breakpoint on the last
    // stable system block caches the tools and that text together, and the
    // clock goes after it.
    NSMutableArray<NSString *> *stableParts = [systemTexts mutableCopy];
    if (stableSystem.length > 0)
        [stableParts addObject:stableSystem];
    NSMutableArray *system = [NSMutableArray array];
    if (stableParts.count > 0) {
        [system addObject:@{@"type": @"text", @"text": [stableParts componentsJoinedByString:@"\n\n"],
                            @"cache_control": @{@"type": @"ephemeral"}}];
    }
    if (volatileSystem.length > 0)
        [system addObject:ISHLLMTextBlock(volatileSystem)];
    if (system.count > 0)
        body[@"system"] = system;

    if (tools.count > 0) {
        NSMutableArray *converted = [NSMutableArray array];
        for (NSDictionary *tool in tools) {
            NSDictionary *function = [tool[@"function"] isKindOfClass:NSDictionary.class] ? tool[@"function"] : tool;
            NSString *name = ISHLLMString(function[@"name"]);
            if (name.length == 0)
                continue;
            NSDictionary *schema = [function[@"parameters"] isKindOfClass:NSDictionary.class] ? function[@"parameters"] : @{@"type": @"object", @"properties": @{}};
            [converted addObject:@{@"name": name, @"description": ISHLLMString(function[@"description"]), @"input_schema": schema}];
        }
        if (converted.count > 0)
            body[@"tools"] = converted;
    }
    if (ISHLLMAnthropicModelTakesFallbacks(model))
        body[@"fallbacks"] = @"default";
    return body;
}

NSDictionary *ISHLLMAnthropicMessageFromResponse(NSDictionary *response, NSString **errorOut, NSString **noteOut) {
    if (errorOut != NULL)
        *errorOut = nil;
    if (noteOut != NULL)
        *noteOut = nil;
    NSDictionary *error = [response[@"error"] isKindOfClass:NSDictionary.class] ? response[@"error"] : nil;
    if (error != nil || [response[@"type"] isEqual:@"error"]) {
        if (errorOut != NULL) {
            NSString *type = ISHLLMString(error[@"type"]);
            NSString *message = ISHLLMString(error[@"message"]);
            *errorOut = [NSString stringWithFormat:@"Anthropic API error%@: %@", type.length > 0 ? [NSString stringWithFormat:@" (%@)", type] : @"",
                         message.length > 0 ? message : @"no message"];
        }
        return nil;
    }
    NSArray *content = [response[@"content"] isKindOfClass:NSArray.class] ? response[@"content"] : nil;
    NSString *stopReason = ISHLLMString(response[@"stop_reason"]);
    if (content == nil) {
        if (errorOut != NULL)
            *errorOut = @"Unexpected response from the Anthropic API (no content).";
        return nil;
    }
    if ([stopReason isEqualToString:@"refusal"]) {
        NSDictionary *details = [response[@"stop_details"] isKindOfClass:NSDictionary.class] ? response[@"stop_details"] : nil;
        NSString *explanation = ISHLLMString(details[@"explanation"]);
        if (errorOut != NULL)
            *errorOut = explanation.length > 0 ? [@"The model declined this request: " stringByAppendingString:explanation] : @"The model declined this request.";
        return nil;
    }
    NSMutableArray<NSString *> *texts = [NSMutableArray array];
    NSMutableArray *toolCalls = [NSMutableArray array];
    for (id block in content) {
        if (![block isKindOfClass:NSDictionary.class])
            continue;
        NSString *type = ISHLLMString(block[@"type"]);
        if ([type isEqualToString:@"text"]) {
            NSString *text = ISHLLMString(block[@"text"]);
            if (text.length > 0)
                [texts addObject:text];
        } else if ([type isEqualToString:@"tool_use"]) {
            id input = block[@"input"] ?: @{};
            NSData *json = [NSJSONSerialization isValidJSONObject:input] ? [NSJSONSerialization dataWithJSONObject:input options:0 error:nil] : nil;
            [toolCalls addObject:@{
                @"id": ISHLLMString(block[@"id"]),
                @"type": @"function",
                @"function": @{@"name": ISHLLMString(block[@"name"]),
                               @"arguments": json != nil ? [[NSString alloc] initWithData:json encoding:NSUTF8StringEncoding] : @"{}"},
            }];
        }
        // thinking, redacted_thinking and fallback blocks are not shown;
        // they travel back to the API in kISHLLMAnthropicContentKey.
    }
    if ([stopReason isEqualToString:@"max_tokens"] && noteOut != NULL)
        *noteOut = @"(The reply was cut off at the output limit.)";
    NSMutableDictionary *message = [@{
        @"role": @"assistant",
        @"content": [texts componentsJoinedByString:@"\n\n"],
        kISHLLMAnthropicContentKey: content,
    } mutableCopy];
    if (toolCalls.count > 0)
        message[@"tool_calls"] = toolCalls;
    return message;
}
