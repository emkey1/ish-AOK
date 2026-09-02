#!/bin/sh
# Run the guest regression suite across every guest root, and report totals.
#
# This is the release gate. The rule it exists to enforce is that a release
# runs the full suite on ALL FOUR architectures AND on real hardware -- and,
# since 552, on a glibc root as well as the musl ones.
#
# That last part is the lesson of the 552 cycle. build/alpine-*-test are four
# architectures but only ONE libc, and four kernel bugs shipped through that
# gap in a single release: glibc rewrites CLOCK_PROCESS_CPUTIME_ID into a
# dynamic clock id before the syscall while musl passes the constant, so
# timer_create, clock_nanosleep, clock_getcpuclockid and clock_getres(NULL)
# were all broken for glibc programs and green on every Alpine root. A fifth
# leg on build/devuan-arm64-test costs one more suite run and closes it.
#
# Usage:
#   tools/run-guest-gate.sh                 # every local root found, then e2e
#   tools/run-guest-gate.sh --no-e2e        # skip the end-to-end suite
#   tools/run-guest-gate.sh --only arm64    # one root, by name
#   tools/run-guest-gate.sh --parallel      # run the local legs at once
#   tools/run-guest-gate.sh --device m4pt   # also run it on a device over ssh
#
# --parallel runs the five local legs concurrently instead of one after
# another: roughly 45 minutes rather than three hours on an 8-core Mac, and the
# way this project has run multi-arch sweeps historically. The cost is load, and
# load makes the timing-sensitive tests flake -- so treat a failure under
# --parallel as a question, not an answer, and re-run that leg on a quiet
# machine before believing it.
#
# Exits non-zero if any leg reports a failure, so it can gate a release script.

set -u

cd "$(dirname "$0")/.." || exit 1
REPO=$(pwd)
LOGDIR=${GATE_LOGDIR:-$REPO/build/gate-logs}
ISH=$REPO/build/ish
RUN_E2E=1
PARALLEL=0
ONLY=
DEVICE=
DEVICE_PORT=${GATE_DEVICE_PORT:-1022}

while [ $# -gt 0 ]; do
    case "$1" in
        --no-e2e)  RUN_E2E=0 ;;
        --parallel|-j) PARALLEL=1 ;;
        --only)    shift; ONLY=${1:-} ;;
        --device)  shift; DEVICE=${1:-} ;;
        --port)    shift; DEVICE_PORT=${1:-1022} ;;
        -h|--help) sed -n '2,28p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

[ -x "$ISH" ] || { echo "no build/ish -- run: ninja -C build ish" >&2; exit 1; }
mkdir -p "$LOGDIR" || exit 1
# Stale logs from a previous run are worse than none: a summary that greps the
# directory will happily report yesterday's failures as today's.
rm -f "$LOGDIR"/*.log "$LOGDIR"/*.rc

fail_total=0
legs_run=0

# Is this leg wanted, and does its root exist?
leg_wanted() {
    [ -d "$2" ] || return 1
    if [ -n "$ONLY" ] && [ "$ONLY" != "$1" ]; then return 1; fi
    return 0
}

# The suite run itself, with its exit status left in $LOGDIR/<name>.rc so the
# parallel path can score it after waiting.
leg_run() {
    "$ISH" -f "$2" /bin/sh -c '/AOK/tests/setup-regressions.sh --run 2>&1' \
        > "$LOGDIR/$1.log" 2>&1
    echo $? > "$LOGDIR/$1.rc"
}

# Score a finished leg from its log. Separated from leg_run so it is identical
# whether the leg ran alone or alongside four others.
leg_report() {
    name=$1
    log=$LOGDIR/$name.log
    rc=$(cat "$LOGDIR/$name.rc" 2>/dev/null || echo 1)
    # The runner stops at the first BUILD failure, so a short log is a leg that
    # never really ran -- count it as a failure rather than a clean sweep.
    p=$(grep -cE '^[a-z0-9_]+: PASS$' "$log")
    f=$(grep -cE '^[a-z0-9_]+: FAIL' "$log")
    s=$(grep -cE ': SKIP' "$log")
    if [ "$p" -eq 0 ] && [ "$f" -eq 0 ]; then
        echo "  $name: NOTHING RAN (rc=$rc) -- almost certainly a build failure:"
        grep -E 'error:' "$log" | head -3
        f=1
    fi
    echo "  $name  PASS=$p FAIL=$f SKIP=$s  (rc=$rc)"
    [ "$f" -gt 0 ] && grep -E '^[a-z0-9_]+: FAIL' "$log"
    fail_total=$((fail_total + f))
    legs_run=$((legs_run + 1))
    return 0
}

# One leg, start to finish, the way it has always worked.
run_leg() {
    leg_wanted "$1" "$2" || return 0
    printf '########## %s ##########\n' "$1"
    leg_run "$1" "$2"
    leg_report "$1"
}

echo "logs: $LOGDIR"
echo

LEG_NAMES="arm64 i386 amd64 riscv64 devuan-arm64"
leg_root() {
    case "$1" in
        arm64)        echo "$REPO/build/alpine-arm64-test" ;;
        i386)         echo "$REPO/build/alpine-i386-test" ;;
        amd64)        echo "$REPO/build/alpine-amd64-test" ;;
        riscv64)      echo "$REPO/build/alpine-riscv64-test" ;;
        devuan-arm64) echo "$REPO/build/devuan-arm64-test" ;;
    esac
}

if [ "$PARALLEL" -eq 1 ]; then
    started=
    for name in $LEG_NAMES; do
        root=$(leg_root "$name")
        leg_wanted "$name" "$root" || continue
        printf '########## %s (started) ##########\n' "$name"
        leg_run "$name" "$root" &
        started="$started $name"
    done
    wait
    for name in $started; do
        printf '########## %s ##########\n' "$name"
        leg_report "$name"
    done
else

# The four architectures, on musl.
run_leg arm64   "$REPO/build/alpine-arm64-test"
run_leg i386    "$REPO/build/alpine-i386-test"
run_leg amd64   "$REPO/build/alpine-amd64-test"
run_leg riscv64 "$REPO/build/alpine-riscv64-test"

# ...and the same kernel against a different libc. Not optional at release
# time: see the header. Needs a toolchain in the root --
#   build/ish -f build/devuan-arm64-test /bin/sh -c \
#       'apt-get update && apt-get install -y --no-install-recommends gcc libc6-dev'
run_leg devuan-arm64 "$REPO/build/devuan-arm64-test"

fi

if [ -n "$DEVICE" ]; then
    printf '########## device: %s ##########\n' "$DEVICE"
    # Under sudo: the suite expects to be root, as the app's own sessions are.
    # An unprivileged run fails a batch of tests for reasons that have nothing
    # to do with the kernel under test.
    ssh -p "$DEVICE_PORT" "$DEVICE" \
        'sudo sh -c "/AOK/tests/setup-regressions.sh --run"' \
        > "$LOGDIR/device.log" 2>&1
    rc=$?
    p=$(grep -cE '^[a-z0-9_]+: PASS$' "$LOGDIR/device.log")
    f=$(grep -cE '^[a-z0-9_]+: FAIL' "$LOGDIR/device.log")
    s=$(grep -cE ': SKIP' "$LOGDIR/device.log")
    echo "  device  PASS=$p FAIL=$f SKIP=$s  (rc=$rc)"
    [ "$f" -gt 0 ] && grep -E '^[a-z0-9_]+: FAIL' "$LOGDIR/device.log"
    fail_total=$((fail_total + f))
    legs_run=$((legs_run + 1))
fi

if [ "$RUN_E2E" -eq 1 ] && [ -z "$ONLY" ]; then
    echo
    echo '########## e2e ##########'
    # From scratch: a stale e2e_out silently reuses a tree the current build
    # never produced.
    rm -rf "$REPO/build/e2e_out"
    # Capture the status of MESON, not of the tail at the end of the pipe.
    # `if meson test ... | tail -6` tests tail's exit status, which is always 0,
    # so a failed e2e leg never reached fail_total and the script signed off
    # "GATE CLEAN" with a red leg on the screen above it. That is the one thing
    # a release gate must never do.
    meson test -C "$REPO/build" e2e > "$LOGDIR/e2e.log" 2>&1
    e2e_rc=$?
    tail -6 "$LOGDIR/e2e.log"
    if [ "$e2e_rc" -ne 0 ]; then
        # A missing build/tools/fakefsify is a build-dir that never had a bare
        # `ninja -C build` run in it, not a regression -- say so, because the
        # meson output alone reads like a test failure.
        if grep -q "fakefsify: No such file" "$LOGDIR/e2e.log"; then
            echo "  e2e: build/tools/fakefsify missing -- run a BARE 'ninja -C build' once"
        fi
        fail_total=$((fail_total + 1))
    fi
    legs_run=$((legs_run + 1))
fi

echo
echo "=================================================="
if [ "$fail_total" -eq 0 ]; then
    echo "GATE CLEAN -- $legs_run leg(s), no failures"
else
    echo "GATE FAILED -- $fail_total failure(s) across $legs_run leg(s)"
fi
echo "=================================================="
[ "$fail_total" -eq 0 ]
