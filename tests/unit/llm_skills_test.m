// Host unit test for the LLM Chat's Agent Skills support (app/LLMChatSkills.m):
// where it looks, how leniently it reads a SKILL.md's frontmatter (skills are
// written for other clients, whose parsers accept more than YAML does), which
// skill wins a name, and the list the model is given.
//
// Built by meson on macOS (`meson test -C build llm_skills`), or alone:
//   clang -fobjc-arc -framework Foundation -Iapp app/LLMChatSkills.m \
//         tests/unit/llm_skills_test.m -o /tmp/t && /tmp/t

#import <Foundation/Foundation.h>
#import "LLMChatSkills.h"

static int failures = 0;

#define CHECK(cond, ...) do { \
    if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } \
} while (0)

static NSString *paths(NSArray<NSDictionary<NSString *, id> *> *directories) {
    NSMutableArray<NSString *> *out = [NSMutableArray array];
    for (NSDictionary *d in directories)
        [out addObject:[NSString stringWithFormat:@"%@=%@", d[@"path"], d[@"scope"]]];
    return [out componentsJoinedByString:@" "];
}

static ISHLLMSkill *parse(NSString *text) {
    return ISHLLMParseSkill(text, @"/home/u/.agents/skills/folder/SKILL.md", @"folder", ISHLLMSkillScopeUser);
}

static void test_search_directories(void) {
    NSString *got = paths(ISHLLMSkillSearchDirectories(@"/home/u/proj/sub", @"/home/u"));
    NSString *want = @"/home/u/proj/sub/.agents/skills=0 /home/u/proj/sub/.claude/skills=0 "
                     @"/home/u/proj/.agents/skills=0 /home/u/proj/.claude/skills=0 "
                     @"/home/u/.agents/skills=1 /home/u/.claude/skills=1 /AOK/persist/skills=2";
    CHECK([got isEqualToString:want], "nested project: %s", got.UTF8String);

    got = paths(ISHLLMSkillSearchDirectories(@"/home/u/proj/sub/", @"/home/u/"));
    CHECK([got isEqualToString:want], "trailing slashes: %s", got.UTF8String);

    got = paths(ISHLLMSkillSearchDirectories(@"/home/u", @"/home/u"));
    CHECK([got isEqualToString:@"/home/u/.agents/skills=1 /home/u/.claude/skills=1 /AOK/persist/skills=2"],
          "working directory is home: %s", got.UTF8String);

    got = paths(ISHLLMSkillSearchDirectories(@"/tmp/x", @"/home/u"));
    CHECK([got isEqualToString:@"/tmp/x/.agents/skills=0 /tmp/x/.claude/skills=0 /tmp/.agents/skills=0 /tmp/.claude/skills=0 "
                               @"/home/u/.agents/skills=1 /home/u/.claude/skills=1 /AOK/persist/skills=2"],
          "outside home, stops before /: %s", got.UTF8String);

    got = paths(ISHLLMSkillSearchDirectories(@"/", @"/root"));
    CHECK([got isEqualToString:@"/root/.agents/skills=1 /root/.claude/skills=1 /AOK/persist/skills=2"], "at /: %s", got.UTF8String);

    got = paths(ISHLLMSkillSearchDirectories(nil, nil));
    CHECK([got isEqualToString:@"/AOK/persist/skills=2"], "nothing known: %s", got.UTF8String);

    got = paths(ISHLLMSkillSearchDirectories(@"relative/dir", @"also-relative"));
    CHECK([got isEqualToString:@"/AOK/persist/skills=2"], "relative paths ignored: %s", got.UTF8String);
}

static void test_parse(void) {
    ISHLLMSkill *s = parse(@"---\nname: pdf-processing\ndescription: Extract PDF text. Use when handling PDFs.\n---\n# PDF\nbody\n");
    CHECK([s.name isEqualToString:@"pdf-processing"], "name: %s", s.name.UTF8String);
    CHECK([s.skillDescription isEqualToString:@"Extract PDF text. Use when handling PDFs."], "description: %s", s.skillDescription.UTF8String);
    CHECK(s.notOfferedReason == nil, "plain skill offered");
    CHECK(s.scope == ISHLLMSkillScopeUser && [s.location hasSuffix:@"/folder/SKILL.md"], "scope and location kept");

    s = parse(@"---\nname: x\ndescription: Use this skill when: the user asks about PDFs\n---\n");
    CHECK([s.skillDescription isEqualToString:@"Use this skill when: the user asks about PDFs"], "unquoted colon: %s", s.skillDescription.UTF8String);

    s = parse(@"---\nname: \"quoted: name\"\ndescription: \"Say \\\"hi\\\": then stop\"\n---\n");
    CHECK([s.name isEqualToString:@"quoted: name"], "double-quoted name: %s", s.name.UTF8String);
    CHECK([s.skillDescription isEqualToString:@"Say \"hi\": then stop"], "double-quoted escapes: %s", s.skillDescription.UTF8String);

    s = parse(@"---\nname: x\ndescription: 'It''s here: really'\n---\n");
    CHECK([s.skillDescription isEqualToString:@"It's here: really"], "single-quoted: %s", s.skillDescription.UTF8String);

    s = parse(@"---\nname: x\ndescription: >\n  Folded across\n  two lines.\n\n  New paragraph.\nlicense: MIT\n---\n");
    CHECK([s.skillDescription isEqualToString:@"Folded across two lines. New paragraph."], "folded block: %s", s.skillDescription.UTF8String);

    s = parse(@"---\nname: x\ndescription: |-\n    Literal\n    lines\n---\n");
    CHECK([s.skillDescription isEqualToString:@"Literal lines"], "literal block, collapsed for the list: %s", s.skillDescription.UTF8String);

    s = parse(@"---\nname: x\ndescription: Runs on\n  over the next line\n---\n");
    CHECK([s.skillDescription isEqualToString:@"Runs on over the next line"], "plain continuation: %s", s.skillDescription.UTF8String);

    s = parse(@"---\nname: x\ndescription:\n  All on the next line\n---\n");
    CHECK([s.skillDescription isEqualToString:@"All on the next line"], "value on the next line: %s", s.skillDescription.UTF8String);

    s = parse(@"---\nname: x\nmetadata:\n  description: nested, not this\n  author: y\ndescription: top level\n---\n");
    CHECK([s.skillDescription isEqualToString:@"top level"], "nested map passed over: %s", s.skillDescription.UTF8String);

    s = parse(@"---\nname: x\ndescription: Does things # a comment\n---\n");
    CHECK([s.skillDescription isEqualToString:@"Does things"], "comment cut: %s", s.skillDescription.UTF8String);
    s = parse(@"---\nname: x\ndescription: C# and F# helpers\n---\n");
    CHECK([s.skillDescription isEqualToString:@"C# and F# helpers"], "# inside a word kept: %s", s.skillDescription.UTF8String);

    s = parse(@"﻿---\r\nname: crlf\r\ndescription: Windows line ends\r\n---\r\nbody\r\n");
    CHECK([s.name isEqualToString:@"crlf"] && [s.skillDescription isEqualToString:@"Windows line ends"], "BOM and CRLF: %s / %s", s.name.UTF8String, s.skillDescription.UTF8String);

    s = parse(@"---\nNAME: upper\nDescription: Keys in any case\n---\n");
    CHECK([s.name isEqualToString:@"upper"] && [s.skillDescription isEqualToString:@"Keys in any case"], "key case: %s", s.name.UTF8String);

    s = parse(@"---\ndescription: No name given\n---\n");
    CHECK([s.name isEqualToString:@"folder"] && s.notOfferedReason == nil, "missing name takes the folder's: %s", s.name.UTF8String);

    s = parse(@"---\nname: x\n---\nbody\n");
    CHECK(s.notOfferedReason != nil, "no description is not offered");
    s = parse(@"---\nname: x\ndescription: \"\"\n---\n");
    CHECK(s.notOfferedReason != nil, "empty description is not offered");
    s = parse(@"# Just markdown\nname: x\ndescription: y\n");
    CHECK(s.notOfferedReason != nil, "no frontmatter is not offered");
    s = parse(@"---\nname: x\ndescription: never closed\n");
    CHECK(s.notOfferedReason != nil, "unclosed frontmatter is not offered");

    s = parse(@"---\nname: x\ndescription: y\ndisable-model-invocation: true\n---\n");
    CHECK(s.disableModelInvocation, "disable-model-invocation read");
    s = parse(@"---\nname: x\ndescription: y\ndisable-model-invocation: false\n---\n");
    CHECK(!s.disableModelInvocation, "disable-model-invocation false");

    NSString *longDescription = [@"" stringByPaddingToLength:3000 withString:@"word " startingAtIndex:0];
    s = parse([NSString stringWithFormat:@"---\nname: x\ndescription: %@\n---\n", longDescription]);
    CHECK(s.skillDescription.length == 1025 && [s.skillDescription hasSuffix:@"…"], "long description cut to 1024: %lu", (unsigned long) s.skillDescription.length);
}

static ISHLLMSkill *make(NSString *name, NSString *location, NSString *description) {
    return ISHLLMParseSkill([NSString stringWithFormat:@"---\nname: %@\ndescription: %@\n---\n", name, description],
                            location, name, ISHLLMSkillScopeProject);
}

static void test_resolve(void) {
    ISHLLMSkill *near = make(@"review", @"/p/.agents/skills/review/SKILL.md", @"Project review");
    ISHLLMSkill *far = make(@"Review", @"/home/u/.claude/skills/review/SKILL.md", @"User review");
    ISHLLMSkill *other = make(@"pdf", @"/home/u/.agents/skills/pdf/SKILL.md", @"PDFs");
    ISHLLMSkill *broken = parse(@"---\nname: broken\n---\n");
    NSArray<ISHLLMSkill *> *offered = ISHLLMResolveSkills(@[near, far, broken, other], kISHLLMSkillCatalogBudget);
    CHECK(offered.count == 2 && offered[0] == near && offered[1] == other, "first of a name wins, order kept: %lu", (unsigned long) offered.count);
    CHECK([far.notOfferedReason containsString:near.location], "shadowed one names the winner: %s", far.notOfferedReason.UTF8String);
    CHECK(broken.notOfferedReason != nil, "parse problem stays");

    ISHLLMSkill *hidden = make(@"deploy", @"/p/.agents/skills/deploy/SKILL.md", @"Deploys");
    hidden.disableModelInvocation = YES;
    ISHLLMSkill *userDeploy = make(@"deploy", @"/home/u/.agents/skills/deploy/SKILL.md", @"User deploy");
    offered = ISHLLMResolveSkills(@[hidden, userDeploy], kISHLLMSkillCatalogBudget);
    CHECK(offered.count == 0, "a hidden nearer skill still owns its name");
    CHECK(hidden.notOfferedReason != nil && userDeploy.notOfferedReason != nil, "both explained");

    ISHLLMSkill *a = make(@"a", @"/s/a/SKILL.md", [@"" stringByPaddingToLength:200 withString:@"a" startingAtIndex:0]);
    ISHLLMSkill *b = make(@"b", @"/s/b/SKILL.md", [@"" stringByPaddingToLength:200 withString:@"b" startingAtIndex:0]);
    ISHLLMSkill *c = make(@"c", @"/s/c/SKILL.md", [@"" stringByPaddingToLength:200 withString:@"c" startingAtIndex:0]);
    ISHLLMSkill *tiny = make(@"d", @"/s/d/SKILL.md", @"d");
    offered = ISHLLMResolveSkills(@[a, b, c, tiny], 600);
    CHECK(offered.count == 2, "budget fits two: %lu", (unsigned long) offered.count);
    CHECK(c.notOfferedReason != nil && tiny.notOfferedReason != nil, "once full, later ones are left out too");
}

static void test_catalog(void) {
    CHECK(ISHLLMSkillCatalog(@[]) == nil, "no skills, no section");
    ISHLLMSkill *s = make(@"x<y>", @"/s/x/SKILL.md", @"Tom & Jerry <script>");
    NSString *catalog = ISHLLMSkillCatalog(@[s]);
    CHECK([catalog hasPrefix:@"Skills:"], "section opens with its instructions");
    CHECK([catalog containsString:@"read_file"], "says how to load one");
    CHECK([catalog containsString:@"<name>x&lt;y&gt;</name>"], "name escaped: %s", catalog.UTF8String);
    CHECK([catalog containsString:@"Tom &amp; Jerry &lt;script&gt;"], "description escaped");
    CHECK([catalog containsString:@"<location>/s/x/SKILL.md</location>"], "location given");
    CHECK([catalog hasSuffix:@"</available_skills>"], "section closed");
}

static void test_setting(void) {
    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    id saved = [defaults objectForKey:@"LLM Skills Enabled"];
    [defaults removeObjectForKey:@"LLM Skills Enabled"];
    CHECK(ISHLLMSkillsEnabled(), "on by default");
    ISHLLMSetSkillsEnabled(NO);
    CHECK(!ISHLLMSkillsEnabled(), "turned off");
    ISHLLMSetSkillsEnabled(YES);
    CHECK(ISHLLMSkillsEnabled(), "turned on");
    if (saved != nil)
        [defaults setObject:saved forKey:@"LLM Skills Enabled"];
    else
        [defaults removeObjectForKey:@"LLM Skills Enabled"];
}

int main(void) {
    @autoreleasepool {
        test_search_directories();
        test_parse();
        test_resolve();
        test_catalog();
        test_setting();
    }
    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("llm_skills: all passed\n");
    return 0;
}
