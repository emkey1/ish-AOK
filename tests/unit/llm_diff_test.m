// Host unit test for the LLM Chat's line diff (app/LLMChatDiff.m), which
// the chat's change review shows and its revert relies on being honest
// about. patch(1) is the oracle: every diff produced for a random pair of
// files must turn the first into the second, and must be as small as
// `diff -d` (minimal) says the change is.
//
// Built by meson on macOS (`meson test -C build llm_diff`), or alone:
//   clang -fobjc-arc -framework Foundation -Iapp app/LLMChatDiff.m \
//         tests/unit/llm_diff_test.m -o /tmp/t && /tmp/t

#import <Foundation/Foundation.h>
#import "LLMChatDiff.h"

static int failures = 0;

#define CHECK(cond, ...) do { \
    if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } \
} while (0)

static uint64_t rng_state = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(uint32_t n) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (uint32_t) (rng_state % n);
}

static NSString *run(NSString *tool, NSArray<NSString *> *args, int *status) {
    NSTask *task = [NSTask new];
    task.launchPath = tool;
    task.arguments = args;
    NSPipe *pipe = [NSPipe pipe];
    task.standardOutput = pipe;
    task.standardError = pipe;
    [task launch];
    NSData *data = [pipe.fileHandleForReading readDataToEndOfFile];
    [task waitUntilExit];
    if (status != NULL)
        *status = task.terminationStatus;
    return [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] ?: @"";
}

static NSString *joined(NSArray<NSString *> *lines) {
    return lines.count == 0 ? @"" : [[lines componentsJoinedByString:@"\n"] stringByAppendingString:@"\n"];
}

// Counts the -/+ lines diff -d reports, the minimal size of the change.
static NSUInteger minimal_changes(NSString *dir, NSString *before, NSString *after) {
    NSString *a = [dir stringByAppendingPathComponent:@"ma"], *b = [dir stringByAppendingPathComponent:@"mb"];
    [before writeToFile:a atomically:NO encoding:NSUTF8StringEncoding error:nil];
    [after writeToFile:b atomically:NO encoding:NSUTF8StringEncoding error:nil];
    NSString *out = run(@"/usr/bin/diff", @[@"-d", @"-U0", a, b], NULL);
    NSUInteger n = 0;
    for (NSString *line in [out componentsSeparatedByString:@"\n"]) {
        if (([line hasPrefix:@"-"] && ![line hasPrefix:@"---"]) || ([line hasPrefix:@"+"] && ![line hasPrefix:@"+++"]))
            n++;
    }
    return n;
}

static void check_pair(NSString *dir, NSArray<NSString *> *before, NSArray<NSString *> *after, NSUInteger context, int round) {
    NSArray<ISHLLMDiffLine *> *diff = ISHLLMUnifiedDiff(before, after, context, 100000);
    CHECK(diff != nil, "round %d: gave up", round);
    if (diff == nil)
        return;
    NSString *a = [dir stringByAppendingPathComponent:@"a"];
    NSString *p = [dir stringByAppendingPathComponent:@"p.diff"];
    [joined(before) writeToFile:a atomically:NO encoding:NSUTF8StringEncoding error:nil];
    NSString *patchText = [NSString stringWithFormat:@"--- a\n+++ a\n%@", ISHLLMDiffText(diff)];
    [patchText writeToFile:p atomically:NO encoding:NSUTF8StringEncoding error:nil];
    int status = 0;
    NSString *out = diff.count == 0 ? @"" : run(@"/usr/bin/patch", @[@"-s", @"-F0", @"-N", a, p], &status);
    NSString *result = [NSString stringWithContentsOfFile:a encoding:NSUTF8StringEncoding error:nil] ?: @"";
    CHECK(status == 0 && [result isEqualToString:joined(after)], "round %d (context %lu): patch did not reproduce the new file (status %d) %s\n%s",
          round, (unsigned long) context, status, out.UTF8String, patchText.UTF8String);
    NSUInteger removed = 0, added = 0;
    ISHLLMDiffCounts(diff, &removed, &added);
    NSUInteger minimal = minimal_changes(dir, joined(before), joined(after));
    CHECK(removed + added == minimal, "round %d: %lu changed lines, diff -d says %lu", round, (unsigned long) (removed + added), (unsigned long) minimal);
}

int main(void) {
    @autoreleasepool {
        NSString *dir = [NSTemporaryDirectory() stringByAppendingPathComponent:[NSString stringWithFormat:@"llm-diff-test-%d", getpid()]];
        [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];

        // Fixed cases first: the edges.
        check_pair(dir, @[], @[@"a"], 3, -1);
        check_pair(dir, @[@"a"], @[], 3, -2);
        check_pair(dir, @[@"a", @"b", @"c"], @[@"a", @"x", @"c"], 3, -3);
        check_pair(dir, @[@"a", @"b", @"c"], @[@"a", @"b", @"c", @"d"], 0, -4);
        check_pair(dir, @[@"x", @"a", @"b"], @[@"a", @"b"], 1, -5);
        CHECK(ISHLLMUnifiedDiff(@[@"a", @"b"], @[@"a", @"b"], 3, 100).count == 0, "equal files give an empty diff");

        NSArray<NSString *> *words = @[@"alpha", @"beta", @"gamma", @"delta", @"eps", @"}", @"{", @""];
        for (int round = 0; round < 150; round++) {
            NSMutableArray<NSString *> *before = [NSMutableArray array];
            NSUInteger n = rnd(40);
            for (NSUInteger i = 0; i < n; i++)
                [before addObject:words[rnd((uint32_t) words.count)]];
            NSMutableArray<NSString *> *after = [before mutableCopy];
            NSUInteger edits = rnd(6);
            for (NSUInteger e = 0; e < edits; e++) {
                uint32_t what = rnd(3);
                if (what == 0 || after.count == 0)
                    [after insertObject:words[rnd((uint32_t) words.count)] atIndex:rnd((uint32_t) after.count + 1)];
                else if (what == 1)
                    [after removeObjectAtIndex:rnd((uint32_t) after.count)];
                else
                    after[rnd((uint32_t) after.count)] = words[rnd((uint32_t) words.count)];
            }
            check_pair(dir, before, after, rnd(4), round);
        }

        // Give-up: a rewrite larger than maxEdits.
        NSMutableArray *big1 = [NSMutableArray array], *big2 = [NSMutableArray array];
        for (int i = 0; i < 200; i++) {
            [big1 addObject:[NSString stringWithFormat:@"a%d", i]];
            [big2 addObject:[NSString stringWithFormat:@"b%d", i]];
        }
        CHECK(ISHLLMUnifiedDiff(big1, big2, 3, 50) == nil, "a 400-line rewrite gives up at 50 edits");
        CHECK(ISHLLMUnifiedDiff(big1, big2, 3, 1000) != nil, "and succeeds at 1000");

        [NSFileManager.defaultManager removeItemAtPath:dir error:nil];
    }
    if (failures == 0)
        printf("llm_diff: all passed\n");
    return failures == 0 ? 0 : 1;
}
