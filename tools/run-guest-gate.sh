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
#   tools/run-guest-gate.sh --unpriv        # add an UNPRIVILEGED leg (see below)
#   tools/run-guest-gate.sh --device m4pt   # also run it on a device over ssh
#
# --unpriv adds one leg that runs the suite as an ordinary user instead of
# root. Every other leg runs the CLI as uid 0, where a parent directory is
# always writable and an ownership check always passes -- so a whole class of
# permission-ORDERING bugs is unreachable from the gate as it stands. Two of
# them shipped: `mkdir -p` reporting EACCES where Linux reports EEXIST (2026-08),
# and `rm -f` on a missing name reporting EACCES where Linux reports ENOENT
# (2026-09), the latter killing the entire suite on the first cache miss with no
# diagnostic. Neither was subtle; both were simply invisible. The tests that
# cover those now drop privileges in-process, which is the durable fix, and this
# leg is the belt to that pair of braces: it also catches a test that FAILS
# rather than skips when it cannot have privilege.
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
UNPRIV=0
ONLY=
DEVICE=
DEVICE_PORT=${GATE_DEVICE_PORT:-1022}

while [ $# -gt 0 ]; do
    case "$1" in
        --no-e2e)  RUN_E2E=0 ;;
        --parallel|-j) PARALLEL=1 ;;
        --unpriv|--unprivileged) UNPRIV=1 ;;
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

# The suite as an ordinary user. The guest CLI is always uid 0, so the drop has
# to happen inside the guest: make a user, make /tmp writable by it (some roots
# ship /tmp 0755), and su. `-s /bin/sh` because a freshly added user often has
# no shell set, and a uid well clear of 1000, which is usually taken.
#
# Tests needing privilege SKIP here, so the pass count is legitimately lower
# than a root leg's -- what this leg is looking for is a FAIL.
leg_run_unpriv() {
    "$ISH" -f "$2" /bin/sh -c '
        if ! id -u ishtester >/dev/null 2>&1; then
            adduser -D -u 1500 ishtester 2>/dev/null ||
                useradd -m -u 1500 ishtester 2>/dev/null || true
        fi
        id -u ishtester >/dev/null 2>&1 || { echo "cannot create an unprivileged user"; exit 1; }
        # Clear the scratch the ROOT legs left behind. /tmp is sticky, so an
        # unprivileged user cannot unlink a root-owned file there even when the
        # directory is 1777 -- and several tests use fixed /tmp paths and start
        # by unlinking their own leftovers. fcntl_lock is the sharp case: its
        # stale root-owned /tmp/fcntl_lock.done survives the unlink, the waiter
        # sees "done" immediately and reports "expected a conflict inside the
        # locked range", which looks like a locking regression and is a
        # permission on a leftover file. Safe here because this leg runs after
        # every root leg has finished.
        rm -rf /tmp/* /tmp/.[!.]* 2>/dev/null
        chmod 1777 /tmp
        # Its own work and cache directories. The default ones are left behind
        # root-owned by every other leg, and an unprivileged run cannot write
        # either -- the work dir fails outright, and the cache silently fails
        # every store (which is how the root-owned-cache trap was found).
        # A cwd the user owns. Several tests create scratch files RELATIVE to
        # cwd on purpose -- proc_pid_io wants storage I/O rather than tmpfs, so
        # it mkdtemps in place -- and the gate would otherwise leave them
        # standing in a root-owned "/", where they fail for want of a writable
        # directory rather than for anything about the kernel.
        rm -rf /tmp/unpriv-home
        mkdir -p /tmp/unpriv-home
        chown ishtester /tmp/unpriv-home 2>/dev/null || chown 1500 /tmp/unpriv-home
        su -s /bin/sh ishtester -c "
            cd /tmp/unpriv-home || exit 1
            HOME=/tmp/unpriv-home
            ISH_AOK_REGRESS_DIR=/tmp/ish-aok-regressions-unpriv
            ISH_AOK_REGRESS_CACHE=/tmp/ish-aok-regress-cache-unpriv
            export HOME ISH_AOK_REGRESS_DIR ISH_AOK_REGRESS_CACHE
            /AOK/tests/setup-regressions.sh --run" 2>&1
    ' > "$LOGDIR/$1.log" 2>&1
    echo $? > "$LOGDIR/$1.rc"
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

# The unprivileged leg, on the glibc root: it is the one whose /tmp and user
# tooling match a real device session, which is where this class was found.
if [ "$UNPRIV" -eq 1 ] && [ -z "$ONLY" ]; then
    unpriv_root=$REPO/build/devuan-arm64-test
    [ -d "$unpriv_root" ] || unpriv_root=$REPO/build/alpine-arm64-test
    if [ -d "$unpriv_root" ]; then
        printf '########## unprivileged (%s) ##########\n' "$(basename "$unpriv_root")"
        leg_run_unpriv unpriv "$unpriv_root"
        leg_report unpriv
    else
        echo "  unpriv: SKIPPED (no root to run it in)"
    fi
fi

if [ -n "$DEVICE" ]; then
    printf '########## device: %s ##########\n' "$DEVICE"
    # Under sudo, so every test actually runs: the ones needing privilege skip
    # otherwise. An unprivileged device run is worth doing too and is now clean
    # (the five tests that used to FAIL rather than skip were fixed in 554, one
    # of them a real socket-ownership bug) -- but it exercises strictly less, so
    # the gate's device leg takes the root path.
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
