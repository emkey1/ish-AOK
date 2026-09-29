//
//  LLMChatPermissions.m
//  iSH-AOK
//
//  See LLMChatPermissions.h.
//

#import "LLMChatPermissions.h"

NSString *const kISHLLMShellRulePattern = @"pattern";
NSString *const kISHLLMShellRuleAction = @"action";

static NSString *const kISHLLMPermissionReadKey = @"LLM Tool Permission Read";
static NSString *const kISHLLMPermissionEditKey = @"LLM Tool Permission Edit";
static NSString *const kISHLLMPermissionShellKey = @"LLM Tool Permission Shell";
static NSString *const kISHLLMPermissionMCPKey = @"LLM Tool Permission MCP";
static NSString *const kISHLLMShellRulesKey = @"LLM Tool Shell Rules";

NSString *ISHLLMPermissionActionTitle(ISHLLMPermissionAction action) {
    switch (action) {
        case ISHLLMPermissionAllow: return @"Allow";
        case ISHLLMPermissionAsk: return @"Ask";
        case ISHLLMPermissionDeny: return @"Deny";
    }
    return @"Ask";
}

NSString *ISHLLMToolCategoryTitle(ISHLLMToolCategory category) {
    switch (category) {
        case ISHLLMToolCategoryRead: return @"Read Files";
        case ISHLLMToolCategoryEdit: return @"Edit Files";
        case ISHLLMToolCategoryShell: return @"Shell Commands";
        case ISHLLMToolCategoryMCP: return @"MCP Tools";
    }
    return @"";
}

static NSString *ISHLLMCategoryKey(ISHLLMToolCategory category) {
    switch (category) {
        case ISHLLMToolCategoryRead: return kISHLLMPermissionReadKey;
        case ISHLLMToolCategoryEdit: return kISHLLMPermissionEditKey;
        case ISHLLMToolCategoryShell: return kISHLLMPermissionShellKey;
        case ISHLLMToolCategoryMCP: return kISHLLMPermissionMCPKey;
    }
    return kISHLLMPermissionShellKey;
}

static BOOL ISHLLMValidAction(id value) {
    if (![value isKindOfClass:NSNumber.class])
        return NO;
    NSInteger n = [value integerValue];
    return n >= ISHLLMPermissionAllow && n <= ISHLLMPermissionDeny;
}

ISHLLMPermissionAction ISHLLMCategoryAction(ISHLLMToolCategory category) {
    id value = [NSUserDefaults.standardUserDefaults objectForKey:ISHLLMCategoryKey(category)];
    if (ISHLLMValidAction(value))
        return (ISHLLMPermissionAction) [value integerValue];
    // Reading is what the model does most and changes nothing; before these
    // settings existed every read went through a confirmed shell command.
    return category == ISHLLMToolCategoryRead ? ISHLLMPermissionAllow : ISHLLMPermissionAsk;
}

void ISHLLMSetCategoryAction(ISHLLMToolCategory category, ISHLLMPermissionAction action) {
    [NSUserDefaults.standardUserDefaults setInteger:action forKey:ISHLLMCategoryKey(category)];
}

NSArray<NSDictionary<NSString *, id> *> *ISHLLMDefaultShellRules(void) {
    // Commands that only look. A piece that redirects into a file, or a line
    // with a command substitution, still asks (see ISHLLMEvaluateShellCommand),
    // so "cat x > y" and "echo $(rm -rf ~)" are not covered by these.
    NSArray<NSString *> *readOnly = @[
        @"ls *", @"pwd", @"cat *", @"head *", @"tail *", @"wc *", @"grep *",
        @"uname *", @"whoami", @"id *", @"date *", @"which *", @"command -v *",
        @"file *", @"stat *", @"du *", @"df *", @"ps *", @"echo *", @"printf *",
        @"git status *", @"git diff *", @"git log *", @"git show *", @"git branch *",
    ];
    NSMutableArray *rules = [NSMutableArray array];
    for (NSString *pattern in readOnly)
        [rules addObject:@{kISHLLMShellRulePattern: pattern, kISHLLMShellRuleAction: @(ISHLLMPermissionAllow)}];
    return rules;
}

NSArray<NSDictionary<NSString *, id> *> *ISHLLMShellRules(void) {
    id stored = [NSUserDefaults.standardUserDefaults objectForKey:kISHLLMShellRulesKey];
    if (![stored isKindOfClass:NSArray.class])
        return ISHLLMDefaultShellRules();
    NSMutableArray *rules = [NSMutableArray array];
    for (id rule in stored) {
        if (![rule isKindOfClass:NSDictionary.class])
            continue;
        NSString *pattern = rule[kISHLLMShellRulePattern];
        if (![pattern isKindOfClass:NSString.class] || pattern.length == 0 || !ISHLLMValidAction(rule[kISHLLMShellRuleAction]))
            continue;
        [rules addObject:@{kISHLLMShellRulePattern: pattern, kISHLLMShellRuleAction: rule[kISHLLMShellRuleAction]}];
    }
    return rules;
}

void ISHLLMSetShellRules(NSArray<NSDictionary<NSString *, id> *> *rules) {
    if (rules == nil)
        [NSUserDefaults.standardUserDefaults removeObjectForKey:kISHLLMShellRulesKey];
    else
        [NSUserDefaults.standardUserDefaults setObject:rules forKey:kISHLLMShellRulesKey];
}

void ISHLLMAddShellRule(NSString *pattern, ISHLLMPermissionAction action) {
    NSMutableArray *rules = [ISHLLMShellRules() mutableCopy];
    for (NSInteger i = (NSInteger) rules.count - 1; i >= 0; i--) {
        if ([rules[i][kISHLLMShellRulePattern] isEqualToString:pattern])
            [rules removeObjectAtIndex:i];
    }
    [rules insertObject:@{kISHLLMShellRulePattern: pattern, kISHLLMShellRuleAction: @(action)} atIndex:0];
    ISHLLMSetShellRules(rules);
}

// Classic two-pointer glob with backtracking to the last star; linear in
// practice, and the patterns are short.
static BOOL ISHLLMWildcardMatchExact(NSString *pattern, NSString *text) {
    NSUInteger p = 0, t = 0, star = NSNotFound, mark = 0;
    NSUInteger plen = pattern.length, tlen = text.length;
    while (t < tlen) {
        unichar pc = p < plen ? [pattern characterAtIndex:p] : 0;
        if (p < plen && (pc == '?' || pc == [text characterAtIndex:t])) {
            p++;
            t++;
        } else if (p < plen && pc == '*') {
            star = p++;
            mark = t;
        } else if (star != NSNotFound) {
            p = star + 1;
            t = ++mark;
        } else {
            return NO;
        }
    }
    while (p < plen && [pattern characterAtIndex:p] == '*')
        p++;
    return p == plen;
}

BOOL ISHLLMWildcardMatch(NSString *pattern, NSString *text) {
    if (ISHLLMWildcardMatchExact(pattern, text))
        return YES;
    // "ls *" also means a bare "ls", which is how people read it.
    if ([pattern hasSuffix:@" *"])
        return [[pattern substringToIndex:pattern.length - 2] isEqualToString:text];
    return NO;
}

NSArray<NSString *> *ISHLLMSplitShellCommand(NSString *command, BOOL *writesFileOut, BOOL *substitutesOut) {
    NSMutableArray<NSString *> *pieces = [NSMutableArray array];
    NSMutableString *current = [NSMutableString string];
    BOOL writesFile = NO, substitutes = NO;
    BOOL inSingle = NO, inDouble = NO;
    NSUInteger length = command.length;
    void (^finishPiece)(void) = ^{
        NSString *piece = [current stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        if (piece.length > 0)
            [pieces addObject:piece];
        [current setString:@""];
    };
    for (NSUInteger i = 0; i < length; i++) {
        unichar c = [command characterAtIndex:i];
        unichar next = i + 1 < length ? [command characterAtIndex:i + 1] : 0;
        if (inSingle) {
            if (c == '\'')
                inSingle = NO;
            [current appendFormat:@"%C", c];
            continue;
        }
        if (c == '\\' && i + 1 < length) {
            [current appendFormat:@"%C%C", c, next];
            i++;
            continue;
        }
        if (c == '`' || (c == '$' && next == '('))
            substitutes = YES;
        if (inDouble) {
            if (c == '"')
                inDouble = NO;
            [current appendFormat:@"%C", c];
            continue;
        }
        switch (c) {
            case '\'':
                inSingle = YES;
                break;
            case '"':
                inDouble = YES;
                break;
            case ';':
            case '\n':
            case '|':
                finishPiece();
                if (c == '|' && next == '|')
                    i++;
                continue;
            case '&':
                // >& and &> are redirections, not separators.
                if (i > 0 && [command characterAtIndex:i - 1] == '>')
                    break;
                if (next == '>')
                    break;
                finishPiece();
                if (next == '&')
                    i++;
                continue;
            case '>': {
                // A descriptor move (2>&1, >&2) writes no file; neither does
                // /dev/null. Anything else after > or >> is a file.
                NSUInteger j = i + 1;
                if (j < length && [command characterAtIndex:j] == '>')
                    j++;
                if (j < length && [command characterAtIndex:j] == '&')
                    break;
                while (j < length && [command characterAtIndex:j] == ' ')
                    j++;
                NSString *rest = [command substringFromIndex:j];
                if (![rest hasPrefix:@"/dev/null"])
                    writesFile = YES;
                break;
            }
            default:
                break;
        }
        [current appendFormat:@"%C", c];
    }
    finishPiece();
    if (writesFileOut != NULL)
        *writesFileOut = writesFile;
    if (substitutesOut != NULL)
        *substitutesOut = substitutes;
    return pieces;
}

// Whether one piece redirects into a file; the splitter reports it for the
// whole line, and a rule allows per piece.
static BOOL ISHLLMPieceWritesFile(NSString *piece) {
    BOOL writes = NO;
    ISHLLMSplitShellCommand(piece, &writes, NULL);
    return writes;
}

ISHLLMPermissionAction ISHLLMEvaluateShellCommand(NSString *command,
                                                  NSArray<NSDictionary<NSString *, id> *> *rules,
                                                  ISHLLMPermissionAction fallback,
                                                  NSString **reasonOut) {
    BOOL substitutes = NO;
    NSArray<NSString *> *pieces = ISHLLMSplitShellCommand(command, NULL, &substitutes);
    ISHLLMPermissionAction result = pieces.count == 0 ? fallback : ISHLLMPermissionAllow;
    NSString *reason = nil;
    BOOL decided = NO;
    for (NSString *piece in pieces) {
        ISHLLMPermissionAction action = fallback;
        NSString *pieceReason = nil; // the category setting: nothing to explain
        BOOL fromRule = NO;
        for (NSDictionary<NSString *, id> *rule in rules) {
            NSString *pattern = rule[kISHLLMShellRulePattern];
            if ([pattern isKindOfClass:NSString.class] && ISHLLMWildcardMatch(pattern, piece)) {
                action = (ISHLLMPermissionAction) [rule[kISHLLMShellRuleAction] integerValue];
                pieceReason = [NSString stringWithFormat:@"the rule \u201c%@\u201d (%@)", pattern, ISHLLMPermissionActionTitle(action)];
                fromRule = YES;
                break;
            }
        }
        // The rule does not cover this piece after all, so it gets what an
        // unmatched piece would: the category's own answer.
        if (fromRule && action == ISHLLMPermissionAllow && fallback != ISHLLMPermissionAllow
            && (substitutes || ISHLLMPieceWritesFile(piece))) {
            action = fallback;
            pieceReason = substitutes
                ? @"a command substitution, which no rule can see into"
                : @"a redirect into a file, which a rule does not cover";
        }
        if (!decided || action > result) {
            result = MAX(result, action);
            reason = pieceReason;
            decided = YES;
        }
    }
    if (reasonOut != NULL)
        *reasonOut = reason;
    return result;
}

BOOL ISHLLMPathIsInside(NSString *path, NSString *directory) {
    if (directory.length == 0 || path.length == 0)
        return NO;
    NSString *dir = [directory hasSuffix:@"/"] && directory.length > 1 ? [directory substringToIndex:directory.length - 1] : directory;
    if ([path isEqualToString:dir] || [dir isEqualToString:@"/"])
        return YES;
    return [path hasPrefix:[dir stringByAppendingString:@"/"]];
}

ISHLLMPermissionAction ISHLLMEvaluatePathAccess(ISHLLMToolCategory category,
                                                NSString *path,
                                                NSString *workingDirectory,
                                                ISHLLMPermissionAction categoryAction,
                                                NSString **reasonOut) {
    NSString *reason = nil;
    ISHLLMPermissionAction action = categoryAction;
    if (category == ISHLLMToolCategoryEdit && action == ISHLLMPermissionAllow && !ISHLLMPathIsInside(path, workingDirectory)) {
        action = ISHLLMPermissionAsk;
        reason = [NSString stringWithFormat:@"the file being outside the working directory %@", workingDirectory];
    }
    if (reasonOut != NULL)
        *reasonOut = reason;
    return action;
}

// Commands whose second word is the real verb, so an "always allow" for
// `git status` becomes "git status *" rather than "git *" (which would also
// allow `git push --force`). The same idea as OpenCode's command arity table.
static NSSet<NSString *> *ISHLLMTwoWordCommands(void) {
    static NSSet<NSString *> *set;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        set = [NSSet setWithArray:@[@"git", @"apk", @"apt", @"apt-get", @"dpkg", @"npm", @"pnpm", @"yarn",
                                    @"cargo", @"go", @"pip", @"pip3", @"docker", @"kubectl", @"rc-service",
                                    @"service", @"systemctl", @"make", @"gh", @"brew", @"opkg", @"pacman"]];
    });
    return set;
}

NSString *ISHLLMSuggestedShellRule(NSString *command) {
    BOOL writes = NO, substitutes = NO;
    NSArray<NSString *> *pieces = ISHLLMSplitShellCommand(command, &writes, &substitutes);
    if (pieces.count != 1 || writes || substitutes)
        return nil;
    NSMutableArray<NSString *> *words = [NSMutableArray array];
    for (NSString *word in [pieces[0] componentsSeparatedByCharactersInSet:NSCharacterSet.whitespaceCharacterSet]) {
        if (word.length > 0)
            [words addObject:word];
    }
    if (words.count == 0)
        return nil;
    // A leading VAR=value or a quoted word makes the "command" ambiguous.
    NSString *first = words[0];
    if ([first containsString:@"="] || [first rangeOfCharacterFromSet:[NSCharacterSet characterSetWithCharactersInString:@"'\"*?\\$"]].location != NSNotFound)
        return nil;
    if (words.count >= 2 && [ISHLLMTwoWordCommands() containsObject:first] && ![words[1] hasPrefix:@"-"])
        return [NSString stringWithFormat:@"%@ %@ *", first, words[1]];
    return [first stringByAppendingString:@" *"];
}

// Glob to regular expression, gitignore-style: * and ? stay inside one path
// component, ** crosses them, {a,b} is alternation, [...] passes through.
NSRegularExpression *ISHLLMGlobRegex(NSString *glob) {
    NSMutableString *pattern = [NSMutableString stringWithString:@"^"];
    NSUInteger length = glob.length;
    NSInteger braceDepth = 0;
    for (NSUInteger i = 0; i < length; i++) {
        unichar c = [glob characterAtIndex:i];
        if (c == '*') {
            BOOL doubleStar = i + 1 < length && [glob characterAtIndex:i + 1] == '*';
            if (doubleStar) {
                BOOL slashAfter = i + 2 < length && [glob characterAtIndex:i + 2] == '/';
                [pattern appendString:slashAfter ? @"(?:.*/)?" : @".*"];
                i += slashAfter ? 2 : 1;
            } else {
                [pattern appendString:@"[^/]*"];
            }
        } else if (c == '?') {
            [pattern appendString:@"[^/]"];
        } else if (c == '{') {
            braceDepth++;
            [pattern appendString:@"(?:"];
        } else if (c == '}' && braceDepth > 0) {
            braceDepth--;
            [pattern appendString:@")"];
        } else if (c == ',' && braceDepth > 0) {
            [pattern appendString:@"|"];
        } else if (c == '[') {
            NSRange close = [glob rangeOfString:@"]" options:0 range:NSMakeRange(i + 1, length - i - 1)];
            if (close.location == NSNotFound) {
                [pattern appendString:@"\\["];
            } else {
                NSString *set = [glob substringWithRange:NSMakeRange(i + 1, close.location - i - 1)];
                if ([set hasPrefix:@"!"])
                    set = [@"^" stringByAppendingString:[set substringFromIndex:1]];
                [pattern appendFormat:@"[%@]", set];
                i = close.location;
            }
        } else {
            [pattern appendString:[NSRegularExpression escapedPatternForString:[NSString stringWithCharacters:&c length:1]]];
        }
    }
    [pattern appendString:@"$"];
    return [NSRegularExpression regularExpressionWithPattern:pattern options:0 error:NULL];
}
