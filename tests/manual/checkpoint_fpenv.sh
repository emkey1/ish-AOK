#!/bin/sh
# checkpoint_fpenv.sh -- a process keeps its floating-point environment across
# a checkpoint and restore.
#     tests/manual/checkpoint_fpenv.sh [root]
#
# The rounding mode and the sticky exception flags live in cpu_state (MXCSR and
# the x87 words, FPCR/FPSR, fcsr) and are installed on the host FPU each time
# guest code runs (emu/fpenv.c). The witness sets FE_UPWARD, raises
# FE_DIVBYZERO, reports both and 1/3 computed in that mode, and is saved from
# outside mid-sleep. The restored copy must report exactly the same: the image
# carries the state, and the new host thread that runs the restored task must
# put it back on its own FPU -- float80's x87 mode is per host thread too.
set -e
REPO=$(cd "$(dirname "$0")/../.." && pwd)
ISH=${ISH:-$REPO/build/ish}
ROOT=${1:-$REPO/build/alpine-arm64-test}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/aok-ckfpenv.XXXXXX")
IMG=$WORK/img
trap 'rm -rf "$WORK"' EXIT

cp "$REPO/tests/manual/checkpoint_fpenv/witness.c" "$WORK/witness.c"
ISH_REAL_MNT=$WORK "$ISH" -f "$ROOT" /bin/sh -c \
    'cc -O2 -o /tmp/ckfpenv-witness /realmnt/witness.c -lm' < /dev/null
ISH_CHECKPOINT_AFTER="3:$IMG" "$ISH" -f "$ROOT" /tmp/ckfpenv-witness \
    < /dev/null > "$WORK/save.out" 2>&1 || true
[ -s "$IMG" ] || { echo "FAIL: no image written"; cat "$WORK/save.out"; exit 1; }
ISH_RESTORE="$IMG" "$ISH" -f "$ROOT" < /dev/null > "$WORK/restore.out" 2>&1 &
pid=$!
n=0; while kill -0 $pid 2>/dev/null && [ $n -lt 200 ]; do sleep 0.1; n=$((n+1)); done
kill -9 $pid 2>/dev/null || true
sed 's/^/  saved    | /' "$WORK/save.out"
sed 's/^/  restored | /' "$WORK/restore.out"

before=$(sed -n 's/^BEFORE //p' "$WORK/save.out")
after=$(sed -n 's/^AFTER //p' "$WORK/restore.out")
fail=0
f() { echo "  FAIL    | $*"; fail=1; }
[ -n "$before" ] || f "the saving run printed nothing"
[ -n "$after" ] || f "the restored process did not finish"
[ "$before" = "mode=0x800 flags=0x4 third=3fd5555555555556" ] ||
    [ "$before" = "mode=0x400000 flags=0x2 third=3fd5555555555556" ] ||
    [ "$before" = "mode=0x3 flags=0x8 third=3fd5555555555556" ] ||
    f "the witness itself is wrong before any restore: $before"
[ "$after" = "$before" ] || f "the restored process is not what was saved: [$before] -> [$after]"
[ $fail -eq 0 ] || { echo "FAIL"; exit 1; }
echo "PASS: a restored process keeps its rounding mode and exception flags"
