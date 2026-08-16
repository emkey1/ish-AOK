#!/bin/sh
# Differential test: the shim's getopt against the HOST's own.
#
# The shim replaced libSystem's getopt because its state is one copy per
# process and a native program is one function on a task's thread (see the
# option-parsing section of kernel/native_libc.c). That fixes the sharing; it
# says nothing about whether the PARSE still matches. A getopt that differs in
# some corner is worse than a shared one, because it fails quietly on an
# unusual flag instead of loudly.
#
# So this pulls the marked region straight out of kernel/native_libc.c -- the
# shipped bytes, not a copy that can drift -- links it beside the host's getopt
# in one program, and runs the same argv through both, comparing every step:
# the return, optarg, optind, optopt, and the final (permuted) argv.
#
# Usage: tools/difftest-native-getopt.sh [random-case-count]

set -e
root=$(cd "$(dirname "$0")/.." && pwd)
out=${TMPDIR:-/tmp}/aok-getopt-difftest.$$
mkdir -p "$out"
trap 'rm -rf "$out"' EXIT

# The region, verbatim. A missing marker is a hard failure rather than an empty
# test that reports success.
sed -n '/>>> native getopt: BEGIN/,/<<< native getopt: END/p' \
    "$root/kernel/native_libc.c" > "$out/region.c"
if ! grep -q 'nlibc_getopt_long_only' "$out/region.c"; then
    echo "difftest-native-getopt: markers not found in kernel/native_libc.c" >&2
    exit 2
fi

cat > "$out/prelude.h" <<'EOF'
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
/* The two shim calls the region reaches for. Outside the app, the guest's
 * stderr is the host's, and the guest's environment is the host's. */
#define nlibc_stderr() stderr
#define nlibc_getenv   getenv
EOF

cat > "$out/shim.c" <<'EOF'
#include "prelude.h"
#include "region.c"
EOF

cc -std=c11 -Wall -Wno-unused-function -I"$out" -c "$out/shim.c" -o "$out/shim.o"
cc -std=c11 -Wall -I"$out" -c "$root/tools/difftest_native_getopt.c" \
    -o "$out/driver.o"
cc "$out/shim.o" "$out/driver.o" -o "$out/difftest"

"$out/difftest" "${1:-20000}"
