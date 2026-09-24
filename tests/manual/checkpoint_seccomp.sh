#!/bin/sh
# checkpoint_seccomp.sh -- a sandboxed process stays sandboxed across a restore.
#     tests/manual/checkpoint_seccomp.sh [root]
#
# The image did not carry a process's seccomp mode, its filters, or whether it
# was dumpable, so a restore brought a sandboxed process back unconfined and
# open to its user's ptrace and /proc -- with nothing to tell it so. The
# witness (checkpoint_seccomp/witness.c) installs a filter that makes getppid
# fail EPERM, shares it with a second thread, makes itself undumpable, and is
# saved from outside mid-sleep. It must report the same state after the
# restore, and a TSYNC filter installed afterwards must still reach the other
# thread: that works only if the two threads share their filters again rather
# than holding equal copies (TSYNC asks about identity).
set -e
REPO=$(cd "$(dirname "$0")/../.." && pwd)
ISH=${ISH:-$REPO/build/ish}
ROOT=${1:-$REPO/build/alpine-arm64-test}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/aok-ckseccomp.XXXXXX")
IMG=$WORK/img
trap 'rm -rf "$WORK"' EXIT

cp "$REPO/tests/manual/checkpoint_seccomp/witness.c" "$WORK/witness.c"
ISH_REAL_MNT=$WORK "$ISH" -f "$ROOT" /bin/sh -c \
    'cc -O2 -pthread -o /tmp/ckseccomp-witness /realmnt/witness.c' < /dev/null
ISH_CHECKPOINT_AFTER="3:$IMG" "$ISH" -f "$ROOT" /tmp/ckseccomp-witness \
    < /dev/null > "$WORK/save.out" 2>&1 || true
[ -s "$IMG" ] || { echo "FAIL: no image written"; cat "$WORK/save.out"; exit 1; }
ISH_RESTORE="$IMG" "$ISH" -f "$ROOT" < /dev/null > "$WORK/restore.out" 2>&1 &
pid=$!
n=0; while kill -0 $pid 2>/dev/null && [ $n -lt 150 ]; do sleep 0.1; n=$((n+1)); done
kill -9 $pid 2>/dev/null || true
sed 's/^/  saved    | /' "$WORK/save.out"
sed 's/^/  restored | /' "$WORK/restore.out"

before=$(sed -n 's/^BEFORE //p' "$WORK/save.out")
after=$(sed -n 's/^AFTER //p' "$WORK/restore.out")
tsync=$(sed -n 's/^TSYNC //p' "$WORK/restore.out")
fail=0
f() { echo "  FAIL    | $*"; fail=1; }
[ -n "$before" ] || f "the saving run printed nothing"
[ -n "$after" ] || f "the restored process did not finish"
[ "$before" = "seccomp=2 filters=1 getppid_errno=1 dumpable=0" ] ||
    f "the witness itself is broken before any restore: $before"
[ "$after" = "$before" ] || f "the restored process is not what was saved: [$before] -> [$after]"
[ "$tsync" = "0" ] || f "TSYNC after the restore returned $tsync, not 0: the threads no longer share their filters"
[ $fail -eq 0 ] || { echo "FAIL"; exit 1; }
echo "PASS: a restored process keeps its seccomp filters, shared, and stays undumpable"
