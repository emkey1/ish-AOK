#!/bin/sh
# checkpoint_timers.sh -- timers, pending signals and interrupted sleeps across
# a checkpoint; see checkpoint_timers.c for what is checked and why.
#
#     tests/manual/checkpoint_timers.sh [root]
#
# One leg per save path: the app's (ISH_CHECKPOINT_AFTER, from a host thread,
# the saver killed the moment the image exists, the way iOS kills the app) and
# the guest's own `echo suspend > /proc/ish/checkpoint`. Between the save and
# the restore the machine stays stopped for STOP_SECONDS (default 3), which
# CLOCK_BOOTTIME and an absolute CLOCK_REALTIME arming must count and
# everything on CLOCK_MONOTONIC must not. The root needs gcc.
set -e
REPO=$(cd "$(dirname "$0")/../.." && pwd)
ISH=${ISH:-$REPO/build/ish}
ROOT=${1:-$REPO/build/devuan-amd64-test}
STOP=${STOP_SECONDS:-3}
[ -d "$ROOT" ] || { echo "no root at $ROOT" >&2; exit 2; }
[ -x "$ISH" ] || { echo "build/ish first" >&2; exit 2; }
WORK=$(mktemp -d "${TMPDIR:-/tmp}/aok-timers.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
cp "$REPO/tests/manual/checkpoint_timers.c" "$WORK/"

# Run a guest in the background with a watchdog: macOS has no timeout(1), and
# a restore that never finishes must be a FAIL, not a hang. Output to $1.
run_guest() {
    out=$1; shift
    "$@" < /dev/null > "$out" 2>&1 &
    pid=$!
    n=0
    while kill -0 $pid 2>/dev/null && [ $n -lt 900 ]; do sleep 0.1; n=$((n+1)); done
    if kill -0 $pid 2>/dev/null; then
        kill -9 $pid 2>/dev/null || true
        echo "WATCHDOG: killed after 90 s" >> "$out"
    fi
    wait $pid 2>/dev/null || true
}

# The checks every architecture runs. poll, select and epoll_wait are x86-only
# syscalls; the probe runs them where they exist and grades them like the rest.
CHECKS="alarm posix-monotonic posix-realtime-relative posix-realtime-absolute
posix-boottime posix-periodic posix-sigev-none posix-process-cpu itimer-real
posix-thread-cpu itimer-prof pending-queued pending-timer-overrun pending-kill
sleep-nanosleep sleep-clock_nanosleep-monotonic sleep-clock_nanosleep-realtime
sleep-clock_nanosleep-boottime sleep-ppoll sleep-pselect6 sleep-epoll_pwait
never-posix never-timerfd never-sleep"
fail=0
legs=0

# 0: graded. 1: failed. 4: the stop did not land inside the waits under test.
grade() {
    label=$1; file=$2; checks=${3:-$CHECKS}
    sed "s/^/  $label restored | /" "$file"
    grep -q '^INCONCLUSIVE' "$file" && return 4
    bad=0
    for k in $checks; do
        grep -q "^OK $k:" "$file" || { echo "  FAIL    | $label: $k"; bad=1; }
    done
    grep -q '^FAIL ' "$file" && { echo "  FAIL    | $label: a check failed (above)"; bad=1; }
    grep -q '^TIMERS-PROBE-DONE failures=0' "$file" ||
        { echo "  FAIL    | $label: the restored probe did not finish clean"; bad=1; }
    return $bad
}

if ! ISH_REAL_MNT=$WORK "$ISH" -f "$ROOT" /bin/sh -c \
        'command -v gcc >/dev/null && gcc -O1 -pthread -o /realmnt/probe /realmnt/checkpoint_timers.c -lrt' \
        < /dev/null; then
    echo "FAIL: could not build the probe in $ROOT (it needs gcc)"
    exit 1
fi

# ---- the app's path ----------------------------------------------------------
# The external save lands wherever the probe happens to be; nearly always well
# inside every wait, and the probe says so when it was not.
attempt=1
while :; do
    rm -f "$WORK/img"
    ISH_REAL_MNT=$WORK ISH_CHECKPOINT_AFTER="8:$WORK/img" "$ISH" -f "$ROOT" \
        /realmnt/probe after < /dev/null > "$WORK/save.out" 2>&1 &
    saver=$!
    n=0; while [ ! -s "$WORK/img" ] && [ $n -lt 300 ]; do sleep 0.1; n=$((n+1)); done
    kill -9 $saver 2>/dev/null || true; wait $saver 2>/dev/null || true
    sed 's/^/  after saved    | /' "$WORK/save.out"
    if [ ! -s "$WORK/img" ]; then
        echo "  FAIL    | after: no image written"; fail=1; break
    fi
    sleep "$STOP"
    run_guest "$WORK/after.out" env ISH_REAL_MNT="$WORK" ISH_RESTORE="$WORK/img" "$ISH" -f "$ROOT"
    set +e; grade after "$WORK/after.out"; rc=$?; set -e
    if [ $rc = 4 ] && [ $attempt -lt 3 ]; then
        echo "  (the save missed the waits; again)"; attempt=$((attempt+1)); continue
    fi
    [ $rc = 0 ] || fail=1
    legs=$((legs+1))
    break
done

# ---- the guest's own suspend -------------------------------------------------
rm -f "$WORK/session.img"
run_guest "$WORK/suspend-save.out" env ISH_REAL_MNT="$WORK" ISH_GUEST_CHECKPOINT=1 \
    ISH_SESSION="$WORK/session.img" "$ISH" -f "$ROOT" /realmnt/probe suspend
sed 's/^/  suspend saved  | /' "$WORK/suspend-save.out"
if [ ! -s "$WORK/session.img" ]; then
    echo "  FAIL    | suspend: no image written"; fail=1
else
    sleep "$STOP"
    run_guest "$WORK/suspend.out" env ISH_REAL_MNT="$WORK" ISH_SESSION="$WORK/session.img" \
        "$ISH" -f "$ROOT"
    set +e; grade suspend "$WORK/suspend.out"; rc=$?; set -e
    [ $rc = 0 ] || fail=1
    legs=$((legs+1))
fi

# ---- a save that lands while an expiry is being delivered -------------------
# Every expiry held in delivery for 0.8 s in the saving run
# (ISH_TEST_TIMER_FIRE_DELAY_MS), and the suspend asked for inside the hold:
# reading a timer has to wait the delivery out, or the expiry is in neither the
# timer nor the signal queue. See race_main in the probe.
rm -f "$WORK/race.img"
run_guest "$WORK/race-save.out" env ISH_REAL_MNT="$WORK" ISH_GUEST_CHECKPOINT=1 \
    ISH_TEST_TIMER_FIRE_DELAY_MS=800 ISH_SESSION="$WORK/race.img" "$ISH" -f "$ROOT" \
    /realmnt/probe race
sed 's/^/  race saved     | /' "$WORK/race-save.out"
if [ ! -s "$WORK/race.img" ]; then
    echo "  FAIL    | race: no image written"; fail=1
else
    run_guest "$WORK/race.out" env ISH_REAL_MNT="$WORK" ISH_SESSION="$WORK/race.img" \
        "$ISH" -f "$ROOT"
    set +e; grade race "$WORK/race.out" "race-posix race-itimer"; rc=$?; set -e
    [ $rc = 0 ] || fail=1
    legs=$((legs+1))
fi

[ $fail -eq 0 ] || { echo "FAIL"; exit 1; }
echo "PASS: timers, pending signals and interrupted sleeps kept their deadlines across a checkpoint ($legs legs)"
