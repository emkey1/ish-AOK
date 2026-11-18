#!/bin/sh
# suspend_stays.sh -- a suspend the guest asks for stops the WHOLE machine at
# the image, and can do it without ending the process.
#
#     tests/manual/suspend_stays.sh [root]
#
# `echo suspend > /proc/ish/checkpoint` (all that /AOK/tools/suspend.sh does)
# writes the session image and stops the machine. Two things were wrong with
# how, and checkpoint_restore.sh could see neither, because it only watches the
# shell that asked:
#
#   1. Everything ELSE ran on. The save thawed the guest as soon as the image
#      was written, before its fsync and rename, and only then ended the
#      process -- so every other task kept going through the slowest part of
#      the save, writing to a root the image no longer described. The resumed
#      session then did that work a second time.
#
#   2. In the app the stop was exit(0), from inside the kernel. An app may not
#      terminate itself, so there the machine has to stop while the process
#      stays (checkpoint_suspended_hook). ISH_SUSPEND_STAYS is that ending on
#      the command line.
#
# The witness is a ticker: a second shell appending 1, 2, 3... to a file as
# fast as it can. Read from the HOST, through the fakefs data directory, so
# what says "stopped" is not the thing that did the stopping. A tick taken
# after the image shows up after the resume as a number written twice.
set -e

REPO=$(cd "$(dirname "$0")/../.." && pwd)
ISH=${ISH:-$REPO/build/ish}
ROOT=${1:-$REPO/build/devuan-arm64-test}
SH=/bin/dash
[ -d "$ROOT" ] || { echo "no root at $ROOT" >&2; exit 2; }
[ -x "$ISH" ] || { echo "build/ish first" >&2; exit 2; }

WORK=$(mktemp -d "${TMPDIR:-/tmp}/aok-suspend-stays.XXXXXX")
IMG=$WORK/session.img
MARK=$WORK/stopped
OUT=$WORK/out
TICKS_GUEST=/tmp/suspend-stays-ticks-$$
TICKS=$ROOT/data$TICKS_GUEST
pid=
cleanup() {
    [ -z "$pid" ] || kill -9 "$pid" 2>/dev/null || true
    rm -rf "$WORK" "$TICKS"
}
trap cleanup EXIT

# The guest. The asker waits until the ticker has plainly been counting, so a
# count that then stands still is a machine that stopped and not one that
# never started.
PROG='
T='"$TICKS_GUEST"'
: > $T
( i=0; while :; do i=$((i+1)); echo $i >> $T; done ) &
tick=$!
while [ "$(wc -l < $T)" -lt 200 ]; do sleep 0.05; done
echo "asker: before the suspend"
echo suspend > /proc/ish/checkpoint
echo "asker: after the suspend"
sleep 1
kill $tick
wait $tick 2>/dev/null
echo "asker: done"
'

ticks() { wc -l < "$TICKS" 2>/dev/null | tr -d ' '; }

# consecutive <leg>: the file holds 1..N, each once, in order.
consecutive() {
    bad=$(awk 'NR != $1 { print "line " NR " holds " $1; exit }' "$TICKS")
    [ -z "$bad" ] || {
        echo "FAIL: $1: the ticker's count is not 1..N ($bad) -- it ran on past the image"
        exit 1
    }
}

# resume <leg> <ticks at the stop>: the session comes back and carries on.
resume() {
    back=$(ISH_GUEST_CHECKPOINT=1 ISH_SESSION="$IMG" "$ISH" -f "$ROOT" $SH -c 'x' < /dev/null 2>&1)
    echo "$back" | sed 's/^/  resume  | /'
    n_after=$(printf '%s\n' "$back" | grep -c 'asker: after the suspend' || true)
    [ "$n_after" = 1 ] || { echo "FAIL: $1: the resumed shell did not continue after its suspend exactly once ($n_after)"; exit 1; }
    case $back in
        *"asker: before the suspend"*) echo "FAIL: $1: the resumed guest re-ran its request"; exit 1;;
    esac
    case $back in
        *"asker: done"*) ;;
        *) echo "FAIL: $1: the resumed shell did not finish"; exit 1;;
    esac
    # Reached: the ticker really did run again, or "no number twice" below
    # would be true of a ticker that never came back.
    now=$(ticks)
    [ "$now" -gt "$2" ] || { echo "FAIL: $1: the ticker did not carry on after the resume ($2 then, $now now)"; exit 1; }
    consecutive "$1"
    echo "  resume  | $2 ticks at the stop, $now at the end, every number once"
}

# ---- the CLI's ending: the process exits, and nothing ran past the image ----
echo "  ---- stops at the image (the process ends) ----"
rm -f "$IMG" "$TICKS"
sus=$(ISH_GUEST_CHECKPOINT=1 ISH_SESSION="$IMG" "$ISH" -f "$ROOT" $SH -c "$PROG" < /dev/null 2>&1)
echo "$sus" | sed 's/^/  suspend | /'
[ -s "$IMG" ] || { echo "FAIL: suspend wrote no image"; exit 1; }
case $sus in
    *"asker: after the suspend"*) echo "FAIL: the shell that asked ran past its own suspend"; exit 1;;
esac
at_stop=$(ticks)
[ "$at_stop" -ge 200 ] || { echo "FAIL: the ticker was not counting before the suspend ($at_stop)"; exit 1; }
resume "process ends" "$at_stop"

# ---- the app's ending: the machine stops and the process stays --------------
echo "  ---- stops at the image (the process stays) ----"
rm -f "$IMG" "$TICKS" "$MARK"
ISH_GUEST_CHECKPOINT=1 ISH_SESSION="$IMG" ISH_SUSPEND_STAYS="$MARK" \
    "$ISH" -f "$ROOT" $SH -c "$PROG" < /dev/null > "$OUT" 2>&1 &
pid=$!
waited=0
while [ ! -s "$MARK" ]; do
    if ! kill -0 "$pid" 2>/dev/null; then
        sed 's/^/  suspend | /' "$OUT"
        pid=
        echo "FAIL: the process ended instead of stopping the machine"
        exit 1
    fi
    waited=$((waited + 1))
    [ "$waited" -lt 600 ] || { sed 's/^/  suspend | /' "$OUT"; echo "FAIL: no suspend within 60 s"; exit 1; }
    sleep 0.1
done
[ -s "$IMG" ] || { echo "FAIL: told the machine had stopped, but there is no image"; exit 1; }
[ "$(cat "$MARK")" = "$IMG" ] || { echo "FAIL: the hook was given $(cat "$MARK"), not $IMG"; exit 1; }
at_stop=$(ticks)
[ "$at_stop" -ge 200 ] || { echo "FAIL: the ticker was not counting before the suspend ($at_stop)"; exit 1; }
# Long against the ticker, which writes thousands a second: a machine still
# running moves the count in the first few milliseconds of this.
sleep 2
later=$(ticks)
[ "$later" = "$at_stop" ] || { echo "FAIL: the guest ran on behind the suspend ($at_stop ticks, then $later)"; exit 1; }
kill -0 "$pid" 2>/dev/null || { pid=; echo "FAIL: the process ended after stopping the machine"; exit 1; }
sed 's/^/  suspend | /' "$OUT"
if grep -q 'asker: after the suspend' "$OUT"; then
    echo "FAIL: the shell that asked ran past its own suspend"
    exit 1
fi
echo "  suspend | $at_stop ticks, the same 2 s later, and the process is still there"
# How the app goes: closed by the person, which is a SIGKILL with no warning.
kill -9 "$pid"
wait "$pid" 2>/dev/null || true
pid=
resume "process stays" "$at_stop"

echo "PASS: suspend_stays"
