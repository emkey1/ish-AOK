#!/bin/sh
# checkpoint_tty_descriptions.sh -- see checkpoint_tty_descriptions.c.
#     tests/manual/checkpoint_tty_descriptions.sh [root]     (needs gcc in root)
#
# Runs on the CLI's pseudo-terminal session (ISH_CLI_PTY), which restores
# session terminals the way the app does: a new pty per session.
set -e
REPO=$(cd "$(dirname "$0")/../.." && pwd)
ISH=${ISH:-$REPO/build/ish}
ROOT=${1:-$REPO/build/devuan-amd64-test}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/aok-ttydesc.XXXXXX")
IMG=$WORK/img
trap 'kill -9 $(cat "$WORK/pid" 2>/dev/null) 2>/dev/null || true; rm -rf "$WORK"' EXIT
cp "$REPO/tests/manual/checkpoint_tty_descriptions.c" "$WORK/"
ISH_REAL_MNT=$WORK "$ISH" -f "$ROOT" /bin/sh -c \
    'gcc -O1 -o /realmnt/probe /realmnt/checkpoint_tty_descriptions.c' < /dev/null \
    || { echo "FAIL: could not build the probe"; exit 1; }
fail=0
deferred=0
run() {
    order=$1
    rm -f "$IMG"
    ISH_CLI_PTY=1 ISH_GUEST_CHECKPOINT=1 ISH_SESSION="$IMG" ISH_REAL_MNT="$WORK" \
        "$ISH" -f "$ROOT" /realmnt/probe $order < /dev/null > "$WORK/out1" 2>&1 || true
    [ -s "$IMG" ] || { echo "  FAIL    | $order: suspend wrote no image"; tr -d '\r' < "$WORK/out1" | tail -3; fail=1; return; }
    ( ISH_CLI_PTY=1 ISH_GUEST_CHECKPOINT=1 ISH_CHECKPOINT_DEBUG=1 ISH_SESSION="$IMG" \
        ISH_REAL_MNT="$WORK" "$ISH" -f "$ROOT" /bin/sh -c x < /dev/null > "$WORK/out2" 2>&1 &
      echo $! > "$WORK/pid" )
    n=0; while ! grep -q 'DESCRIPTIONS-DONE' "$WORK/out2" 2>/dev/null && [ $n -lt 60 ]; do sleep 1; n=$((n+1)); done
    kill -9 "$(cat "$WORK/pid")" 2>/dev/null || true
    tr -d '\r' < "$WORK/out2" > "$WORK/out2.txt"
    grep -aE '^(D1|D2|D3)' "$WORK/out2.txt" | sed "s/^/  $order | /"
    grep -q 'not back yet' "$WORK/out2.txt" && { deferred=1; echo "  $order | (the daemon's terminal was rebuilt after it)"; }
    grep -q 'DESCRIPTIONS-DONE' "$WORK/out2.txt" || { echo "  FAIL    | $order: the restored probe never finished"; fail=1; return; }
    for want in 'D1 tty=1 nonblock=1 fd1-shares=1' \
                'D3 tty=1 nonblock=0 same-terminal=1' \
                'D2 tty=1 nonblock=1 same-terminal=1'; do
        grep -qx "$want" "$WORK/out2.txt" || { echo "  FAIL    | $order: want: $want"; fail=1; }
    done
}
run early
run late
# One of the two orders must have named D2 before its terminal existed, or the
# waiting path was never exercised and this proves less than it says.
[ $deferred -eq 1 ] || { echo "  FAIL    | neither order deferred the daemon's descriptor"; fail=1; }
[ $fail -eq 0 ] || { echo "FAIL"; exit 1; }
echo "PASS: each description of a terminal came back on it, with its own flags"
