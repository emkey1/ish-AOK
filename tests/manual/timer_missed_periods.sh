#!/bin/sh
# timer_missed_periods.sh -- the host-stop leg of timer_missed_periods.c: a 1ms
# timerfd and a 1ms POSIX timer must count every period that went by while the
# emulator itself was stopped, and stay on their grid.
#
#     tests/manual/timer_missed_periods.sh [root]
#
# iOS suspending the app stops every host thread, the timers' own among them,
# which no guest can do to itself: a guest's SIGSTOP leaves the timer threads
# running (the probe's other scenarios cover what a guest can do). So this
# stops the emulator from outside, kill -STOP for 300ms, while the probe
# sleeps. The probe says whether the stop landed. The root needs a compiler.
set -e
REPO=$(cd "$(dirname "$0")/../.." && pwd)
ISH=${ISH:-$REPO/build/ish}
ROOT=${1:-$REPO/build/alpine-arm64-test}
[ -d "$ROOT" ] || { echo "no root at $ROOT" >&2; exit 2; }
[ -x "$ISH" ] || { echo "build/ish first" >&2; exit 2; }
WORK=$(mktemp -d "${TMPDIR:-/tmp}/aok-missed.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
cp "$REPO/tests/manual/timer_missed_periods.c" "$REPO/tests/manual/test_common.h" "$WORK/"

if ! ISH_REAL_MNT=$WORK "$ISH" -f "$ROOT" /bin/sh -c \
        'cc -O2 -pthread -o /realmnt/probe /realmnt/timer_missed_periods.c -lrt' \
        < /dev/null; then
    echo "FAIL: could not build the probe in $ROOT"
    exit 1
fi

ISH_REAL_MNT=$WORK "$ISH" -f "$ROOT" /realmnt/probe host-stop -v < /dev/null \
    > "$WORK/out" 2>&1 &
pid=$!
n=0
while ! grep -q HOST-STOP-READY "$WORK/out" && kill -0 $pid 2>/dev/null && [ $n -lt 600 ]; do
    sleep 0.05; n=$((n+1))
done
if grep -q HOST-STOP-READY "$WORK/out"; then
    sleep 0.1
    kill -STOP $pid
    sleep 0.3
    kill -CONT $pid
fi
# A watchdog, as macOS has no timeout(1).
n=0
while kill -0 $pid 2>/dev/null && [ $n -lt 300 ]; do sleep 0.1; n=$((n+1)); done
if kill -0 $pid 2>/dev/null; then
    kill -9 $pid 2>/dev/null || true
    echo "WATCHDOG: killed after 30 s" >> "$WORK/out"
fi
wait $pid 2>/dev/null || true
sed 's/^/  /' "$WORK/out"
grep -q '^timer_missed_periods host-stop: PASS' "$WORK/out" || { echo "FAIL"; exit 1; }
echo "PASS: timers counted the periods the emulator was stopped for, on their grid"
