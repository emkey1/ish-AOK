//
//  LLMChatSkills.m
//  AOK-Workstation
//
//  See LLMChatSkills.h. The model of how a client takes up skills is
//  agentskills.io's integration guide: list name, description and location
//  up front, let the model read the SKILL.md itself, accept the frontmatter
//  other clients accept, and on a clash of names the nearer skill wins.
//

#import "LLMChatSkills.h"

NSString *const kISHLLMSharedSkillsDirectory = @"/AOK/persist/skills";
const NSUInteger kISHLLMSkillCatalogBudget = 12000;

static NSString *const kISHLLMSkillsEnabledKey = @"LLM Skills Enabled";

// The specification's limit for a description. A longer one is cut rather
// than refused: it was written for some client that took it.
static const NSUInteger kISHLLMSkillDescriptionMax = 1024;

@implementation ISHLLMSkill

- (instancetype)init {
    if ((self = [super init])) {
        _name = @"";
        _skillDescription = @"";
        _location = @"";
    }
    return self;
}

- (NSString *)description {
    return [NSString stringWithFormat:@"<skill %@ at %@%@>", _name, _location,
            _notOfferedReason != nil ? [@": " stringByAppendingString:_notOfferedReason] : @""];
}

@end

#pragma mark - Where to look

// Path arithmetic only: these are guest paths, and -stringByStandardizingPath
// would consult the host's filesystem.
static NSString *ISHLLMSkillsTrimSlash(NSString *_Nullable path) {
    if (path.length == 0 || ![path hasPrefix:@"/"])
        return nil;
    while (path.length > 1 && [path hasSuffix:@"/"])
        path = [path substringToIndex:path.length - 1];
    return path;
}

NSArray<NSDictionary<NSString *, id> *> *ISHLLMSkillSearchDirectories(NSString *workingDirectory, NSString *homeDirectory) {
    NSMutableArray<NSDictionary<NSString *, id> *> *directories = [NSMutableArray array];
    NSMutableSet<NSString *> *seen = [NSMutableSet set];
    void (^add)(NSString *, ISHLLMSkillScope) = ^(NSString *path, ISHLLMSkillScope scope) {
        if ([seen containsObject:path])
            return;
        [seen addObject:path];
        [directories addObject:@{@"path": path, @"scope": @(scope)}];
    };
    NSString *home = ISHLLMSkillsTrimSlash(homeDirectory);
    NSString *directory = ISHLLMSkillsTrimSlash(workingDirectory);
    while (directory.length > 0 && ![directory isEqualToString:@"/"] && ![directory isEqualToString:home]) {
        add([directory stringByAppendingPathComponent:@".agents/skills"], ISHLLMSkillScopeProject);
        add([directory stringByAppendingPathComponent:@".claude/skills"], ISHLLMSkillScopeProject);
        directory = directory.stringByDeletingLastPathComponent;
    }
    if (home.length > 0 && ![home isEqualToString:@"/"]) {
        add([home stringByAppendingPathComponent:@".agents/skills"], ISHLLMSkillScopeUser);
        add([home stringByAppendingPathComponent:@".claude/skills"], ISHLLMSkillScopeUser);
    }
    add(kISHLLMSharedSkillsDirectory, ISHLLMSkillScopeShared);
    return directories;
}

#pragma mark - Frontmatter

static BOOL ISHLLMSkillsIndented(NSString *line) {
    return [line hasPrefix:@" "] || [line hasPrefix:@"\t"];
}

static BOOL ISHLLMSkillsBlank(NSString *line) {
    return [line stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet].length == 0;
}

static NSString *ISHLLMSkillsTrim(NSString *text) {
    return [text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
}

static NSString *ISHLLMSkillsCollapseWhitespace(NSString *text) {
    NSArray<NSString *> *words = [text componentsSeparatedByCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    return [[words filteredArrayUsingPredicate:[NSPredicate predicateWithFormat:@"length > 0"]] componentsJoinedByString:@" "];
}

// A | or > block: the continuation lines less their common indent, kept as
// lines (|) or folded into one (>, where a blank line is a line break).
static NSString *ISHLLMSkillsBlockScalar(BOOL literal, NSArray<NSString *> *lines) {
    NSUInteger indent = NSUIntegerMax;
    for (NSString *line in lines) {
        if (ISHLLMSkillsBlank(line))
            continue;
        NSUInteger n = 0;
        while (n < line.length && ([line characterAtIndex:n] == ' ' || [line characterAtIndex:n] == '\t'))
            n++;
        indent = MIN(indent, n);
    }
    if (indent == NSUIntegerMax)
        return @"";
    NSMutableArray<NSString *> *stripped = [NSMutableArray array];
    for (NSString *line in lines)
        [stripped addObject:line.length >= indent ? [line substringFromIndex:indent] : @""];
    if (literal)
        return ISHLLMSkillsTrim([stripped componentsJoinedByString:@"\n"]);
    NSMutableString *folded = [NSMutableString string];
    for (NSString *line in stripped) {
        if (ISHLLMSkillsBlank(line)) {
            [folded appendString:@"\n"];
        } else {
            if (folded.length > 0 && ![folded hasSuffix:@"\n"])
                [folded appendString:@" "];
            [folded appendString:line];
        }
    }
    return ISHLLMSkillsTrim(folded);
}

// A quoted value, which may run on over the continuation lines (YAML folds
// them with spaces). Takes what lies between the opening quote and the last
// matching one.
static NSString *ISHLLMSkillsQuotedScalar(NSString *rest, NSArray<NSString *> *continuation) {
    unichar quote = [rest characterAtIndex:0];
    NSMutableArray<NSString *> *pieces = [NSMutableArray arrayWithObject:rest];
    for (NSString *line in continuation)
        [pieces addObject:ISHLLMSkillsTrim(line)];
    NSString *joined = [pieces componentsJoinedByString:@" "];
    NSString *quoteString = [NSString stringWithCharacters:&quote length:1];
    NSRange close = [joined rangeOfString:quoteString options:NSBackwardsSearch];
    NSString *inner = close.location > 0 ? [joined substringWithRange:NSMakeRange(1, close.location - 1)]
                                         : [joined substringFromIndex:1];
    if (quote == '\'')
        return [inner stringByReplacingOccurrencesOfString:@"''" withString:@"'"];
    NSMutableString *unescaped = [NSMutableString string];
    for (NSUInteger i = 0; i < inner.length; i++) {
        unichar c = [inner characterAtIndex:i];
        if (c == '\\' && i + 1 < inner.length) {
            unichar next = [inner characterAtIndex:++i];
            switch (next) {
                case 'n': [unescaped appendString:@"\n"]; break;
                case 't': [unescaped appendString:@"\t"]; break;
                default: [unescaped appendFormat:@"%C", next]; break;
            }
        } else {
            [unescaped appendFormat:@"%C", c];
        }
    }
    return unescaped;
}

// A plain value: the rest of the line, colons and all, and any indented
// lines after it, up to a " #" comment.
static NSString *ISHLLMSkillsPlainScalar(NSString *rest, NSArray<NSString *> *continuation) {
    NSMutableArray<NSString *> *pieces = [NSMutableArray array];
    if (rest.length > 0)
        [pieces addObject:rest];
    for (NSString *line in continuation) {
        if (!ISHLLMSkillsBlank(line))
            [pieces addObject:ISHLLMSkillsTrim(line)];
    }
    NSString *value = [pieces componentsJoinedByString:@" "];
    if ([value hasPrefix:@"#"])
        return @"";
    NSRange comment = [value rangeOfString:@" #"];
    if (comment.location != NSNotFound)
        value = [value substringToIndex:comment.location];
    return ISHLLMSkillsTrim(value);
}

static NSString *ISHLLMSkillsScalar(NSString *rest, NSArray<NSString *> *continuation) {
    if (rest.length > 0) {
        unichar first = [rest characterAtIndex:0];
        if (first == '|' || first == '>')
            return ISHLLMSkillsBlockScalar(first == '|', continuation);
        if (first == '"' || first == '\'')
            return ISHLLMSkillsQuotedScalar(rest, continuation);
    }
    return ISHLLMSkillsPlainScalar(rest, continuation);
}

// Top-level keys only, lower-cased; nested maps (metadata:) are passed over
// as the continuation of their key.
static NSDictionary<NSString *, NSString *> *ISHLLMSkillsFrontmatterValues(NSArray<NSString *> *lines) {
    NSMutableDictionary<NSString *, NSString *> *values = [NSMutableDictionary dictionary];
    NSUInteger i = 0;
    while (i < lines.count) {
        NSString *line = lines[i];
        NSRange colon = [line rangeOfString:@":"];
        if (ISHLLMSkillsBlank(line) || ISHLLMSkillsIndented(line) || [line hasPrefix:@"#"] || colon.location == NSNotFound || colon.location == 0) {
            i++;
            continue;
        }
        NSString *key = ISHLLMSkillsTrim([line substringToIndex:colon.location]).lowercaseString;
        NSString *rest = ISHLLMSkillsTrim([line substringFromIndex:NSMaxRange(colon)]);
        NSUInteger j = i + 1;
        while (j < lines.count && (ISHLLMSkillsBlank(lines[j]) || ISHLLMSkillsIndented(lines[j])))
            j++;
        NSArray<NSString *> *continuation = [lines subarrayWithRange:NSMakeRange(i + 1, j - i - 1)];
        if (values[key] == nil)
            values[key] = ISHLLMSkillsScalar(rest, continuation);
        i = j;
    }
    return values;
}

static BOOL ISHLLMSkillsTrue(NSString *_Nullable value) {
    NSString *v = value.lowercaseString;
    return [v isEqualToString:@"true"] || [v isEqualToString:@"yes"] || [v isEqualToString:@"on"];
}

ISHLLMSkill *ISHLLMParseSkill(NSString *text, NSString *location, NSString *folderName, ISHLLMSkillScope scope) {
    ISHLLMSkill *skill = [ISHLLMSkill new];
    skill.location = location;
    skill.scope = scope;
    skill.name = folderName;
    NSString *normalized = [[text stringByReplacingOccurrencesOfString:@"\r\n" withString:@"\n"]
                            stringByReplacingOccurrencesOfString:@"\r" withString:@"\n"];
    if ([normalized hasPrefix:@"﻿"])
        normalized = [normalized substringFromIndex:1];
    NSArray<NSString *> *lines = [normalized componentsSeparatedByString:@"\n"];
    NSUInteger close = NSNotFound;
    if (lines.count > 0 && [ISHLLMSkillsTrim(lines[0]) isEqualToString:@"---"]) {
        for (NSUInteger i = 1; i < lines.count; i++) {
            NSString *line = ISHLLMSkillsTrim(lines[i]);
            if ([line isEqualToString:@"---"] || [line isEqualToString:@"..."]) {
                close = i;
                break;
            }
        }
    }
    if (close == NSNotFound) {
        skill.notOfferedReason = NSLocalizedString(@"no frontmatter", @"/skills: why a skill is not offered; its SKILL.md does not start with a --- block");
        return skill;
    }
    NSDictionary<NSString *, NSString *> *values = ISHLLMSkillsFrontmatterValues([lines subarrayWithRange:NSMakeRange(1, close - 1)]);
    NSString *name = ISHLLMSkillsCollapseWhitespace(values[@"name"] ?: @"");
    if (name.length > 0)
        skill.name = name;
    NSString *description = ISHLLMSkillsCollapseWhitespace(values[@"description"] ?: @"");
    if (description.length > kISHLLMSkillDescriptionMax)
        description = [[description substringToIndex:kISHLLMSkillDescriptionMax] stringByAppendingString:@"…"];
    skill.skillDescription = description;
    skill.disableModelInvocation = ISHLLMSkillsTrue(values[@"disable-model-invocation"]);
    if (description.length == 0)
        skill.notOfferedReason = NSLocalizedString(@"no description", @"/skills: why a skill is not offered; its frontmatter has no description");
    return skill;
}

#pragma mark - Which are offered, and the list

static NSString *ISHLLMSkillsXMLEscape(NSString *text) {
    return [[[text stringByReplacingOccurrencesOfString:@"&" withString:@"&amp;"]
             stringByReplacingOccurrencesOfString:@"<" withString:@"&lt;"]
            stringByReplacingOccurrencesOfString:@">" withString:@"&gt;"];
}

static NSString *ISHLLMSkillCatalogEntry(ISHLLMSkill *skill) {
    return [NSString stringWithFormat:@"<skill><name>%@</name><description>%@</description><location>%@</location></skill>\n",
            ISHLLMSkillsXMLEscape(skill.name), ISHLLMSkillsXMLEscape(skill.skillDescription), ISHLLMSkillsXMLEscape(skill.location)];
}

NSArray<ISHLLMSkill *> *ISHLLMResolveSkills(NSArray<ISHLLMSkill *> *found, NSUInteger budgetCharacters) {
    NSMutableDictionary<NSString *, ISHLLMSkill *> *byName = [NSMutableDictionary dictionary];
    NSMutableArray<ISHLLMSkill *> *offered = [NSMutableArray array];
    NSUInteger used = 0;
    BOOL full = NO;
    for (ISHLLMSkill *skill in found) {
        if (skill.notOfferedReason != nil)
            continue;
        // The nearer skill owns the name even when it is not listed itself,
        // so a project can switch off a skill of the same name.
        NSString *key = skill.name.lowercaseString;
        ISHLLMSkill *first = byName[key];
        if (first != nil) {
            skill.notOfferedReason = [NSString stringWithFormat:NSLocalizedString(@"%@ comes first", @"/skills: why a skill is not offered; %@ is the path of another SKILL.md with the same name"), first.location];
            continue;
        }
        byName[key] = skill;
        if (skill.disableModelInvocation) {
            skill.notOfferedReason = NSLocalizedString(@"disable-model-invocation is set", @"/skills: why a skill is not offered; the frontmatter key is quoted literally");
            continue;
        }
        NSUInteger cost = ISHLLMSkillCatalogEntry(skill).length;
        if (full || used + cost > budgetCharacters) {
            full = YES;
            skill.notOfferedReason = NSLocalizedString(@"the list is full", @"/skills: why a skill is not offered; the list sent to the model reached its size limit");
            continue;
        }
        used += cost;
        [offered addObject:skill];
    }
    return offered;
}

NSString *ISHLLMSkillCatalog(NSArray<ISHLLMSkill *> *offered) {
    if (offered.count == 0)
        return nil;
    NSMutableString *catalog = [NSMutableString stringWithString:
        @"Skills: each one below holds instructions for one kind of task. When a task matches a skill's description, "
        @"read its SKILL.md with read_file before you start, then follow it; read a skill only when its task comes up. "
        @"Paths in a skill are relative to the folder its SKILL.md is in: use absolute paths in tool calls.\n<available_skills>\n"];
    for (ISHLLMSkill *skill in offered)
        [catalog appendString:ISHLLMSkillCatalogEntry(skill)];
    [catalog appendString:@"</available_skills>"];
    return catalog;
}

#pragma mark - The setting

BOOL ISHLLMSkillsEnabled(void) {
    id value = [NSUserDefaults.standardUserDefaults objectForKey:kISHLLMSkillsEnabledKey];
    return value == nil || [value boolValue];
}

void ISHLLMSetSkillsEnabled(BOOL enabled) {
    [NSUserDefaults.standardUserDefaults setBool:enabled forKey:kISHLLMSkillsEnabledKey];
}
