#!/bin/sh
# checkpoint_threads.sh -- see checkpoint_threads.c.
#     tests/manual/checkpoint_threads.sh [root]      (needs gcc in root)
set -e
REPO=$(cd "$(dirname "$0")/../.." && pwd)
ISH=${ISH:-$REPO/build/ish}
ROOT=${1:-$REPO/build/devuan-amd64-test}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/aok-threads.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
cp "$REPO/tests/manual/checkpoint_threads.c" "$WORK/"
ISH_REAL_MNT=$WORK "$ISH" -f "$ROOT" /bin/sh -c \
    'gcc -O1 -pthread -o /realmnt/probe /realmnt/checkpoint_threads.c' < /dev/null \
    || { echo "FAIL: could not build the probe"; exit 1; }
# Twice: as pid 1, which the restore rebuilds in the entry point's own task,
# and as a shell's child, which it builds from nothing -- two different paths
# to the same thread group.
fail=0
run() {
    label=$1; checks=$2; want_exit=$3; shift 3
    rm -f "$WORK/img" "$WORK/save.out"
    ISH_REAL_MNT=$WORK ISH_CHECKPOINT_AFTER="3:$WORK/img" "$ISH" -f "$ROOT" \
        "$@" < /dev/null > "$WORK/save.out" 2>&1 &
    saver=$!
    # The run that saved goes on to the end too: saving must not disturb it.
    n=0; while kill -0 $saver 2>/dev/null && [ $n -lt 400 ]; do sleep 0.1; n=$((n+1)); done
    kill -9 $saver 2>/dev/null || true; wait $saver 2>/dev/null || true
    if [ ! -s "$WORK/img" ]; then
        sed "s/^/  $label saved | /" "$WORK/save.out"
        echo "  FAIL    | $label: no image written"; fail=1; return
    fi
    out=$(ISH_REAL_MNT=$WORK ISH_RESTORE="$WORK/img" "$ISH" -f "$ROOT" < /dev/null 2>&1 || true)
    sed "s/^/  $label saved    | /" "$WORK/save.out"
    echo "$out" | sed "s/^/  $label restored | /"
    for k in $checks; do
        grep -q "^OK $k" "$WORK/save.out" || { echo "  FAIL    | $label $k, in the run that saved"; fail=1; }
        echo "$out" | grep -q "^OK $k" || { echo "  FAIL    | $label $k, restored"; fail=1; }
    done
    grep -qx THREADS-DONE "$WORK/save.out" || { echo "  FAIL    | $label: the saving run did not finish"; fail=1; }
    echo "$out" | grep -qx THREADS-DONE || { echo "  FAIL    | $label: the restored run did not finish"; fail=1; }
    if [ -n "$want_exit" ]; then
        echo "$out" | grep -qx "PROBE-EXIT=$want_exit" || { echo "  FAIL    | $label: its shell never saw it exit $want_exit"; fail=1; }
    fi
}
THREADED="ONE-PROCESS SHARED-MEMORY SIGMASK SHARED-FDS SHARED-CWD FUTEX-HANDOFF JOIN"
run pid1 "$THREADED" "" /realmnt/probe
run child "$THREADED" 0 /bin/sh -c '/realmnt/probe; echo "PROBE-EXIT=$?"'
run departed DEPARTED-SAME-PROCESS 3 /bin/sh -c '/realmnt/probe departed; echo "PROBE-EXIT=$?"'
run zombie ZOMBIE-REAPED 0 /bin/sh -c '/realmnt/probe zombie; echo "PROBE-EXIT=$?"'
[ $fail -eq 0 ] || { echo "FAIL"; exit 1; }
echo "PASS: threads came back as one process -- as pid 1, as a child, after their leader exited -- and a zombie came back to be reaped"
