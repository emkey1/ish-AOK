//
//  LLMChatDiff.m
//  iSH-AOK
//
//  See LLMChatDiff.h. Myers' O(ND) algorithm ("An O(ND) Difference Algorithm
//  and Its Variations", 1986), keeping every V vector to walk the path back.
//  N and M are file line counts and D the number of differing lines, capped
//  by maxEdits, so memory is O(D^2) at worst.
//

#import "LLMChatDiff.h"

@implementation ISHLLMDiffLine

+ (instancetype)lineWithKind:(ISHLLMDiffLineKind)kind text:(NSString *)text {
    ISHLLMDiffLine *line = [ISHLLMDiffLine new];
    line->_kind = kind;
    line->_text = [text copy];
    return line;
}

@end

NSArray<NSString *> *ISHLLMDiffSplitLines(NSString *text) {
    if (text.length == 0)
        return @[];
    NSMutableArray<NSString *> *lines = [[text componentsSeparatedByString:@"\n"] mutableCopy];
    if ([text hasSuffix:@"\n"])
        [lines removeLastObject];
    return lines;
}

// One step of the edit script: equal, delete a line of `before`, or insert
// a line of `after`, with the line indexes in each.
typedef struct {
    char op; // '=', '-', '+'
    NSInteger a, b;
} ISHLLMDiffOp;

static NSArray<NSValue *> *ISHLLMEditScript(NSArray<NSString *> *a, NSArray<NSString *> *b, NSUInteger maxEdits, BOOL *gaveUp) {
    NSInteger n = (NSInteger) a.count, m = (NSInteger) b.count;
    NSInteger max = MIN(n + m, (NSInteger) maxEdits);
    NSInteger offset = max + 1;
    NSMutableArray<NSData *> *trace = [NSMutableArray array];
    NSMutableData *vData = [NSMutableData dataWithLength:(NSUInteger) (2 * max + 3) * sizeof(NSInteger)];
    NSInteger *v = vData.mutableBytes;
    NSInteger found = -1;
    for (NSInteger d = 0; d <= max && found < 0; d++) {
        [trace addObject:[vData copy]];
        for (NSInteger k = -d; k <= d; k += 2) {
            NSInteger x;
            if (k == -d || (k != d && v[offset + k - 1] < v[offset + k + 1]))
                x = v[offset + k + 1];
            else
                x = v[offset + k - 1] + 1;
            NSInteger y = x - k;
            while (x < n && y < m && [a[(NSUInteger) x] isEqualToString:b[(NSUInteger) y]]) {
                x++;
                y++;
            }
            v[offset + k] = x;
            if (x >= n && y >= m) {
                found = d;
                break;
            }
        }
    }
    if (found < 0) {
        *gaveUp = YES;
        return nil;
    }
    // Walk back through the saved V vectors from (n, m).
    NSMutableArray<NSValue *> *ops = [NSMutableArray array];
    NSInteger x = n, y = m;
    for (NSInteger d = found; d > 0; d--) {
        const NSInteger *pv = trace[(NSUInteger) d].bytes;
        NSInteger k = x - y;
        NSInteger prevK = (k == -d || (k != d && pv[offset + k - 1] < pv[offset + k + 1])) ? k + 1 : k - 1;
        NSInteger prevX = pv[offset + prevK];
        NSInteger prevY = prevX - prevK;
        while (x > prevX && y > prevY) {
            x--;
            y--;
            ISHLLMDiffOp op = {'=', x, y};
            [ops addObject:[NSValue valueWithBytes:&op objCType:@encode(ISHLLMDiffOp)]];
        }
        if (x == prevX) {
            y--;
            ISHLLMDiffOp op = {'+', x, y};
            [ops addObject:[NSValue valueWithBytes:&op objCType:@encode(ISHLLMDiffOp)]];
        } else {
            x--;
            ISHLLMDiffOp op = {'-', x, y};
            [ops addObject:[NSValue valueWithBytes:&op objCType:@encode(ISHLLMDiffOp)]];
        }
    }
    while (x > 0 && y > 0) {
        x--;
        y--;
        ISHLLMDiffOp op = {'=', x, y};
        [ops addObject:[NSValue valueWithBytes:&op objCType:@encode(ISHLLMDiffOp)]];
    }
    return [[ops reverseObjectEnumerator] allObjects];
}

NSArray<ISHLLMDiffLine *> *ISHLLMUnifiedDiff(NSArray<NSString *> *before, NSArray<NSString *> *after,
                                             NSUInteger context, NSUInteger maxEdits) {
    BOOL gaveUp = NO;
    NSArray<NSValue *> *script = ISHLLMEditScript(before, after, maxEdits, &gaveUp);
    if (gaveUp)
        return nil;
    NSUInteger count = script.count;
    ISHLLMDiffOp *ops = calloc(count ? count : 1, sizeof(ISHLLMDiffOp));
    for (NSUInteger i = 0; i < count; i++)
        [script[i] getValue:&ops[i]];

    NSMutableArray<ISHLLMDiffLine *> *out = [NSMutableArray array];
    NSUInteger i = 0;
    while (i < count) {
        // Next change.
        NSUInteger change = i;
        while (change < count && ops[change].op == '=')
            change++;
        if (change == count)
            break;
        // The hunk runs until `2 * context` equal lines separate it from the
        // next change, or the end.
        NSUInteger start = change > context ? change - context : 0;
        start = MAX(start, i);
        NSUInteger end = change;
        NSUInteger equalRun = 0;
        while (end < count) {
            if (ops[end].op == '=') {
                equalRun++;
                if (equalRun > 2 * context)
                    break;
            } else {
                equalRun = 0;
            }
            end++;
        }
        // Trim trailing context to `context` lines.
        NSUInteger trailing = 0;
        while (end > change && ops[end - 1].op == '=' && trailing < equalRun) {
            end--;
            trailing++;
        }
        NSUInteger keep = MIN(context, trailing);
        end += keep;

        NSInteger aStart = -1, bStart = -1, aLen = 0, bLen = 0;
        for (NSUInteger j = start; j < end; j++) {
            if (ops[j].op != '+') {
                if (aStart < 0)
                    aStart = ops[j].a;
                aLen++;
            }
            if (ops[j].op != '-') {
                if (bStart < 0)
                    bStart = ops[j].b;
                bLen++;
            }
        }
        // An empty side is reported at the line before it, as diff(1) does.
        if (aStart < 0)
            aStart = ops[start].a;
        if (bStart < 0)
            bStart = ops[start].b;
        NSString *header = [NSString stringWithFormat:@"@@ -%ld,%ld +%ld,%ld @@",
                            (long) (aLen > 0 ? aStart + 1 : aStart), (long) aLen,
                            (long) (bLen > 0 ? bStart + 1 : bStart), (long) bLen];
        [out addObject:[ISHLLMDiffLine lineWithKind:ISHLLMDiffLineHunkHeader text:header]];
        for (NSUInteger j = start; j < end; j++) {
            if (ops[j].op == '=')
                [out addObject:[ISHLLMDiffLine lineWithKind:ISHLLMDiffLineContext text:before[(NSUInteger) ops[j].a]]];
            else if (ops[j].op == '-')
                [out addObject:[ISHLLMDiffLine lineWithKind:ISHLLMDiffLineRemoved text:before[(NSUInteger) ops[j].a]]];
            else
                [out addObject:[ISHLLMDiffLine lineWithKind:ISHLLMDiffLineAdded text:after[(NSUInteger) ops[j].b]]];
        }
        i = end;
    }
    free(ops);
    return out;
}

void ISHLLMDiffCounts(NSArray<ISHLLMDiffLine *> *diff, NSUInteger *removed, NSUInteger *added) {
    NSUInteger r = 0, a = 0;
    for (ISHLLMDiffLine *line in diff) {
        r += line.kind == ISHLLMDiffLineRemoved;
        a += line.kind == ISHLLMDiffLineAdded;
    }
    if (removed != NULL)
        *removed = r;
    if (added != NULL)
        *added = a;
}

NSString *ISHLLMDiffText(NSArray<ISHLLMDiffLine *> *diff) {
    NSMutableString *text = [NSMutableString string];
    for (ISHLLMDiffLine *line in diff) {
        switch (line.kind) {
            case ISHLLMDiffLineHunkHeader: [text appendFormat:@"%@\n", line.text]; break;
            case ISHLLMDiffLineContext: [text appendFormat:@" %@\n", line.text]; break;
            case ISHLLMDiffLineRemoved: [text appendFormat:@"-%@\n", line.text]; break;
            case ISHLLMDiffLineAdded: [text appendFormat:@"+%@\n", line.text]; break;
        }
    }
    return text;
}
