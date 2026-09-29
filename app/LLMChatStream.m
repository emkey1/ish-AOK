//
//  LLMChatStream.m
//  iSH-AOK
//
//  See LLMChatStream.h.
//

#import "LLMChatStream.h"

static NSDictionary *ISHLLMParsePayload(NSString *payload) {
    NSData *data = [payload dataUsingEncoding:NSUTF8StringEncoding];
    id json = data.length > 0 ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
    return [json isKindOfClass:NSDictionary.class] ? json : nil;
}

static NSString *ISHLLMStreamString(id value) {
    return [value isKindOfClass:NSString.class] ? value : nil;
}

@implementation ISHLLMOpenAIStreamAssembler {
    NSMutableString *_content;
    NSMutableDictionary<NSNumber *, NSMutableDictionary *> *_calls; // index -> {id, name, arguments}
    NSMutableArray<NSNumber *> *_order;
}

- (instancetype)init {
    if ((self = [super init])) {
        _content = [NSMutableString string];
        _calls = [NSMutableDictionary dictionary];
        _order = [NSMutableArray array];
    }
    return self;
}

- (NSString *)consumePayload:(NSString *)payload {
    NSDictionary *json = ISHLLMParsePayload(payload);
    if (json == nil)
        return nil;
    _sawEvents = YES;
    if ([json[@"error"] isKindOfClass:NSDictionary.class]) {
        _error = json[@"error"];
        return nil;
    }
    NSArray *choices = [json[@"choices"] isKindOfClass:NSArray.class] ? json[@"choices"] : nil;
    NSDictionary *choice = [choices.firstObject isKindOfClass:NSDictionary.class] ? choices.firstObject : nil;
    NSDictionary *delta = [choice[@"delta"] isKindOfClass:NSDictionary.class] ? choice[@"delta"] : nil;
    for (id fragment in [delta[@"tool_calls"] isKindOfClass:NSArray.class] ? delta[@"tool_calls"] : @[]) {
        if (![fragment isKindOfClass:NSDictionary.class])
            continue;
        // Some servers omit "index" when there is one call at a time.
        NSNumber *index = [fragment[@"index"] isKindOfClass:NSNumber.class] ? fragment[@"index"] : @(_order.count > 0 && ISHLLMStreamString(fragment[@"id"]).length == 0 ? _order.lastObject.integerValue : (NSInteger) _order.count);
        NSMutableDictionary *call = _calls[index];
        if (call == nil) {
            call = [@{@"id": @"", @"name": @"", @"arguments": [NSMutableString string]} mutableCopy];
            _calls[index] = call;
            [_order addObject:index];
        }
        NSString *identifier = ISHLLMStreamString(fragment[@"id"]);
        if (identifier.length > 0)
            call[@"id"] = identifier;
        NSDictionary *function = [fragment[@"function"] isKindOfClass:NSDictionary.class] ? fragment[@"function"] : nil;
        NSString *name = ISHLLMStreamString(function[@"name"]);
        if (name.length > 0)
            call[@"name"] = name;
        id arguments = function[@"arguments"];
        if ([arguments isKindOfClass:NSString.class])
            [call[@"arguments"] appendString:arguments];
        else if ([arguments isKindOfClass:NSDictionary.class]) {
            NSData *data = [NSJSONSerialization dataWithJSONObject:arguments options:0 error:nil];
            [call[@"arguments"] setString:[[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] ?: @""];
        }
    }
    NSString *text = ISHLLMStreamString(delta[@"content"]);
    if (text.length > 0) {
        [_content appendString:text];
        return text;
    }
    return nil;
}

- (NSDictionary *)responseMessage {
    if (_error != nil)
        return @{@"error": _error};
    NSMutableDictionary *message = [@{@"role": @"assistant", @"content": [_content copy]} mutableCopy];
    NSMutableArray *toolCalls = [NSMutableArray array];
    for (NSNumber *index in _order) {
        NSDictionary *call = _calls[index];
        NSString *arguments = [call[@"arguments"] length] > 0 ? [call[@"arguments"] copy] : @"{}";
        [toolCalls addObject:@{@"id": call[@"id"], @"type": @"function",
                               @"function": @{@"name": call[@"name"], @"arguments": arguments}}];
    }
    if (toolCalls.count > 0)
        message[@"tool_calls"] = toolCalls;
    return message;
}

@end

@implementation ISHLLMAnthropicStreamAssembler {
    NSMutableDictionary<NSNumber *, NSMutableDictionary *> *_blocks;
    NSMutableDictionary<NSNumber *, NSMutableString *> *_partialJSON;
    NSString *_stopReason;
    NSDictionary *_stopDetails;
    NSDictionary *_error;
}

- (instancetype)init {
    if ((self = [super init])) {
        _blocks = [NSMutableDictionary dictionary];
        _partialJSON = [NSMutableDictionary dictionary];
    }
    return self;
}

- (NSString *)consumePayload:(NSString *)payload {
    NSDictionary *event = ISHLLMParsePayload(payload);
    if (event == nil)
        return nil;
    _sawEvents = YES;
    NSString *type = ISHLLMStreamString(event[@"type"]);
    NSNumber *index = [event[@"index"] isKindOfClass:NSNumber.class] ? event[@"index"] : nil;
    if ([type isEqualToString:@"content_block_start"] && index != nil) {
        NSDictionary *block = [event[@"content_block"] isKindOfClass:NSDictionary.class] ? event[@"content_block"] : @{};
        _blocks[index] = [block mutableCopy];
        // A tool_use starts with "input": {} and fills in by JSON fragments.
        if ([block[@"type"] isEqual:@"tool_use"])
            _partialJSON[index] = [NSMutableString string];
        NSString *text = ISHLLMStreamString(block[@"text"]);
        return text.length > 0 ? text : nil;
    }
    if ([type isEqualToString:@"content_block_delta"] && index != nil) {
        NSMutableDictionary *block = _blocks[index];
        NSDictionary *delta = [event[@"delta"] isKindOfClass:NSDictionary.class] ? event[@"delta"] : @{};
        NSString *deltaType = ISHLLMStreamString(delta[@"type"]);
        if ([deltaType isEqualToString:@"text_delta"]) {
            NSString *text = ISHLLMStreamString(delta[@"text"]) ?: @"";
            block[@"text"] = [ISHLLMStreamString(block[@"text"]) ?: @"" stringByAppendingString:text];
            return text.length > 0 ? text : nil;
        }
        if ([deltaType isEqualToString:@"input_json_delta"])
            [_partialJSON[index] appendString:ISHLLMStreamString(delta[@"partial_json"]) ?: @""];
        else if ([deltaType isEqualToString:@"thinking_delta"])
            block[@"thinking"] = [ISHLLMStreamString(block[@"thinking"]) ?: @"" stringByAppendingString:ISHLLMStreamString(delta[@"thinking"]) ?: @""];
        else if ([deltaType isEqualToString:@"signature_delta"])
            block[@"signature"] = [ISHLLMStreamString(block[@"signature"]) ?: @"" stringByAppendingString:ISHLLMStreamString(delta[@"signature"]) ?: @""];
        return nil;
    }
    if ([type isEqualToString:@"content_block_stop"] && index != nil) {
        NSMutableString *json = _partialJSON[index];
        if (json != nil) {
            id input = json.length > 0 ? [NSJSONSerialization JSONObjectWithData:[json dataUsingEncoding:NSUTF8StringEncoding] options:0 error:nil] : nil;
            _blocks[index][@"input"] = [input isKindOfClass:NSDictionary.class] ? input : @{};
            [_partialJSON removeObjectForKey:index];
        }
        return nil;
    }
    if ([type isEqualToString:@"message_delta"]) {
        NSDictionary *delta = [event[@"delta"] isKindOfClass:NSDictionary.class] ? event[@"delta"] : @{};
        if (ISHLLMStreamString(delta[@"stop_reason"]).length > 0)
            _stopReason = delta[@"stop_reason"];
        if ([delta[@"stop_details"] isKindOfClass:NSDictionary.class])
            _stopDetails = delta[@"stop_details"];
        return nil;
    }
    if ([type isEqualToString:@"message_stop"])
        _finished = YES;
    if ([type isEqualToString:@"error"])
        _error = [event[@"error"] isKindOfClass:NSDictionary.class] ? event[@"error"] : @{@"message": @"stream error"};
    return nil;
}

- (NSDictionary *)response {
    if (_error != nil)
        return @{@"type": @"error", @"error": _error};
    NSMutableArray *content = [NSMutableArray array];
    for (NSNumber *index in [_blocks.allKeys sortedArrayUsingSelector:@selector(compare:)]) {
        NSMutableDictionary *block = _blocks[index];
        // A tool_use cut off before its stop still needs an input object.
        if ([block[@"type"] isEqual:@"tool_use"] && _partialJSON[index] != nil) {
            id input = [NSJSONSerialization JSONObjectWithData:[_partialJSON[index] dataUsingEncoding:NSUTF8StringEncoding] options:0 error:nil];
            block[@"input"] = [input isKindOfClass:NSDictionary.class] ? input : @{};
        }
        [content addObject:[block copy]];
    }
    NSMutableDictionary *response = [@{@"type": @"message", @"role": @"assistant", @"content": content,
                                       @"stop_reason": _stopReason ?: @""} mutableCopy];
    if (_stopDetails != nil)
        response[@"stop_details"] = _stopDetails;
    return response;
}

@end
