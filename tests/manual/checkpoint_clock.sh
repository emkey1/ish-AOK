#!/bin/sh
# checkpoint_clock.sh -- the guest's clocks across a checkpoint; see
# checkpoint_clock.c for what is checked and why.
#
#     tests/manual/checkpoint_clock.sh [root]
#
# One leg per save path: the app's (ISH_CHECKPOINT_AFTER, from a host thread,
# the saver killed the moment the image exists, the way iOS kills the app) and
# the guest's own `echo suspend > /proc/ish/checkpoint`. Between the save and
# the restore the machine stays stopped for STOP_SECONDS (default 3), which
# CLOCK_BOOTTIME must count and CLOCK_MONOTONIC must not. Both need gcc in the
# root. A third leg is the reported case itself, where the root has python3:
# a Python time.sleep -- clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME) --
# asleep across a suspend (checkpoint_clock.py).
set -e
REPO=$(cd "$(dirname "$0")/../.." && pwd)
ISH=${ISH:-$REPO/build/ish}
ROOT=${1:-$REPO/build/devuan-amd64-test}
STOP=${STOP_SECONDS:-3}
[ -d "$ROOT" ] || { echo "no root at $ROOT" >&2; exit 2; }
[ -x "$ISH" ] || { echo "build/ish first" >&2; exit 2; }
WORK=$(mktemp -d "${TMPDIR:-/tmp}/aok-clock.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
cp "$REPO/tests/manual/checkpoint_clock.c" "$WORK/"

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

CHECKS="wake uptime monotonic monotonic-raw boottime order btime starttime condvar
nanosleep-monotonic nanosleep-boottime timerfd-boottime-restored
timerfd-monotonic-restored timerfd-boottime-new posix-timer-boottime
posix-timer-monotonic"
fail=0
legs=0

# 0: graded. 1: failed. 4: the stop did not land inside the sleep under test.
grade() {
    label=$1; file=$2
    sed "s/^/  $label restored | /" "$file"
    grep -q '^INCONCLUSIVE' "$file" && return 4
    bad=0
    for k in $CHECKS; do
        grep -q "^OK $k:" "$file" || { echo "  FAIL    | $label: $k"; bad=1; }
    done
    grep -q '^CLOCK-PROBE-DONE failures=0' "$file" ||
        { echo "  FAIL    | $label: the restored probe did not finish clean"; bad=1; }
    return $bad
}

have_gcc=0
if ISH_REAL_MNT=$WORK "$ISH" -f "$ROOT" /bin/sh -c \
        'command -v gcc >/dev/null && gcc -O1 -pthread -o /realmnt/probe /realmnt/checkpoint_clock.c' \
        < /dev/null; then
    have_gcc=1
else
    echo "  SKIP    | no gcc in $ROOT: the probe legs are not run"
fi

if [ $have_gcc = 1 ]; then
    # ---- the app's path ----------------------------------------------------
    # The external save lands wherever the probe happens to be; nearly always
    # inside one of its 1 s sleeps, and the probe says so when it was not.
    attempt=1
    while :; do
        rm -f "$WORK/img"
        ISH_REAL_MNT=$WORK ISH_CHECKPOINT_AFTER="8:$WORK/img" "$ISH" -f "$ROOT" \
            /realmnt/probe after < /dev/null > "$WORK/save.out" 2>&1 &
        saver=$!
        n=0; while [ ! -s "$WORK/img" ] && [ $n -lt 300 ]; do sleep 0.1; n=$((n+1)); done
        kill -9 $saver 2>/dev/null || true; wait $saver 2>/dev/null || true
        if [ ! -s "$WORK/img" ]; then
            sed 's/^/  after saved    | /' "$WORK/save.out"
            echo "  FAIL    | after: no image written"; fail=1; break
        fi
        sed 's/^/  after saved    | /' "$WORK/save.out"
        sleep "$STOP"
        run_guest "$WORK/after.out" env ISH_REAL_MNT="$WORK" ISH_RESTORE="$WORK/img" "$ISH" -f "$ROOT"
        set +e; grade after "$WORK/after.out"; rc=$?; set -e
        if [ $rc = 4 ] && [ $attempt -lt 3 ]; then
            echo "  (the save missed the sleep; again)"; attempt=$((attempt+1)); continue
        fi
        [ $rc = 0 ] || fail=1
        legs=$((legs+1))
        break
    done

    # ---- the guest's own suspend ---------------------------------------------
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
fi

# ---- the reported case: Python's time.sleep across a suspend ---------------
if ISH_REAL_MNT=$WORK "$ISH" -f "$ROOT" /bin/sh -c 'command -v python3 >/dev/null' < /dev/null; then
    cp "$REPO/tests/manual/checkpoint_clock.py" "$WORK/"
    rm -f "$WORK/py.img"
    run_guest "$WORK/py-save.out" env ISH_REAL_MNT="$WORK" ISH_GUEST_CHECKPOINT=1 \
        ISH_SESSION="$WORK/py.img" "$ISH" -f "$ROOT" /bin/sh -c 'exec python3 /realmnt/checkpoint_clock.py'
    sed 's/^/  python saved   | /' "$WORK/py-save.out"
    if [ ! -s "$WORK/py.img" ]; then
        echo "  FAIL    | python: no image written"; fail=1
    else
        sleep "$STOP"
        run_guest "$WORK/py.out" env ISH_REAL_MNT="$WORK" ISH_SESSION="$WORK/py.img" "$ISH" -f "$ROOT"
        sed 's/^/  python restored | /' "$WORK/py.out"
        grep -q '^PY-RESULT OK' "$WORK/py.out" ||
            { echo "  FAIL    | python: time.sleep did not wake on time after the restore"; fail=1; }
        legs=$((legs+1))
    fi
else
    echo "  SKIP    | no python3 in $ROOT: the time.sleep leg is not run"
fi

[ $legs -gt 0 ] || { echo "FAIL: nothing ran (the root has neither gcc nor python3)"; exit 1; }
[ $fail -eq 0 ] || { echo "FAIL"; exit 1; }
echo "PASS: the guest's clocks went on across a checkpoint -- MONOTONIC from its saved value, BOOTTIME and uptime counting the stop -- and every absolute deadline woke on time ($legs legs)"
