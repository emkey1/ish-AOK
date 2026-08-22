#!/bin/bash
# Round-trip ISHShellQuoteArgument (app/ShellFileBrowser.m) through a real
# shell.
#
# The terminal file browser puts guest paths on the user's command line, so a
# quoting bug does not merely look wrong -- it runs something the user did not
# type. "/tmp/semi;colon && rm -rf /" is a legal filename.
#
# The function under test is EXTRACTED FROM THE SHIPPING SOURCE at run time
# rather than copied here, so this check cannot drift from the code it grades.
# And each case is graded by asking /bin/sh to echo the quoted string back:
# an assertion against an expected string only tests the author's idea of the
# right answer, while the shell is the actual authority.
#
# Host-side check (needs clang + Foundation), so it does not run under the
# guest test tiers. Run it after touching the quoting:
#     tools/check-shell-quoting.sh

set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_file="$repo/app/ShellFileBrowser.m"

if [ ! -f "$source_file" ]; then
    echo "check-shell-quoting: no $source_file" >&2
    exit 1
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

awk '/^NSString \*ISHShellQuoteArgument/,/^}$/' "$source_file" > "$work/extracted.inc"
if ! grep -q 'ISHShellQuoteArgument' "$work/extracted.inc"; then
    echo "check-shell-quoting: could not extract ISHShellQuoteArgument from $source_file" >&2
    echo "  (did the function signature change? this check greps for it at column 0)" >&2
    exit 1
fi

cat > "$work/main.m" <<'EOF'
#import <Foundation/Foundation.h>
#include <stdio.h>

#include "extracted.inc"

// printf %s rather than echo: echo mangles backslashes on some shells, which
// would make this report a quoting bug that isn't there.
static NSString *roundTrip(NSString *input) {
    NSString *cmd = [NSString stringWithFormat:@"printf '%%s' %@", ISHShellQuoteArgument(input)];
    FILE *p = popen(cmd.UTF8String, "r");
    if (p == NULL) return nil;
    NSMutableData *out = [NSMutableData data];
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), p)) > 0)
        [out appendBytes:buf length:n];
    if (pclose(p) != 0) return nil;
    return [[NSString alloc] initWithData:out encoding:NSUTF8StringEncoding];
}

static NSString *readable(NSString *s) {
    return [[s stringByReplacingOccurrencesOfString:@"\n" withString:@"\\n"]
                stringByReplacingOccurrencesOfString:@"\t" withString:@"\\t"];
}

int main(void) {
    @autoreleasepool {
        NSArray<NSString *> *cases = @[
            // Ordinary paths must come back UNQUOTED: a browser that quotes
            // every path turns a readable command line into noise.
            @"/home/pi",
            @"/AOK/persist/bin/motepad",
            @"relative/path.txt",
            @"-rf",
            @"--flag=value",
            @"a@b%c,d:e+f=g",
            // Legal filenames that are also shell syntax.
            @"/home/pi/my file.txt",
            @"/home/pi/don't panic.txt",
            @"/home/pi/it's a $HOME `thing`",
            @"/tmp/a\"b\"c",
            @"/tmp/semi;colon && rm -rf /",
            @"/tmp/pipe | cat",
            @"/tmp/redirect > out",
            @"/tmp/sub$(id)",
            @"/tmp/new\nline",
            @"/tmp/tab\there",
            @"/tmp/star*glob?[abc]",
            @"/tmp/back\\slash",
            @"/tmp/(paren)",
            @"/tmp/{brace}",
            @"/tmp/~tilde",
            @"/tmp/#hash",
            @"/tmp/!bang",
            // Non-ASCII must survive byte-identically.
            @"/tmp/emoji-\U0001F600",
            @"/tmp/üñïçø∂é",
            // Degenerate: empty, and runs of the one character that cannot be
            // escaped inside single quotes.
            @"",
            @"'",
            @"''",
            @"'''",
        ];
        int failures = 0;
        for (NSString *input in cases) {
            NSString *quoted = ISHShellQuoteArgument(input);
            NSString *back = roundTrip(input);
            BOOL ok = back != nil && [back isEqualToString:input];
            if (!ok) {
                failures++;
                printf("FAIL  in=%s\n      quoted=%s\n      back=%s\n",
                       readable(input).UTF8String, readable(quoted).UTF8String,
                       back == nil ? "(shell failed)" : readable(back).UTF8String);
            }
        }
        printf("shell quoting: %d case(s), %d failure(s)\n", (int)cases.count, failures);
        return failures == 0 ? 0 : 1;
    }
}
EOF

clang -fobjc-arc -framework Foundation -I"$work" "$work/main.m" -o "$work/quotetest"
"$work/quotetest"
