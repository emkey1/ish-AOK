#!/bin/sh
# jit_fuse_ab.sh -- interleaved A/B of any /proc/ish/<arch>_jit_fuse bit, run
# inside the guest (on device or on a CLI root).
#
#   usage: jit_fuse_ab.sh <guest-arch> <fuse-bit> <reps> <workload>...
#   e.g.:  jit_fuse_ab.sh riscv64 retcache 5 shloop fib forkexec
#          jit_fuse_ab.sh arm64   bcond    5 shloop
#          jit_fuse_ab.sh riscv64 all      3 shloop        # whole arch on/off
#
# Every rule this encodes was learned by getting it wrong at least once:
#
# * BOTH ARMS EVERY REP, order alternating rep by rep. Running all of arm A then
#   all of arm B lets thermal drift or a background process land entirely on one
#   arm; a 2.9x intra-arm spread has been observed that way on a busy host.
# * A FRESH GUEST PROCESS PER TIMED RUN. These bits are consumed at TRANSLATION
#   time, so flipping one changes nothing about blocks that are already
#   compiled. A rep that reuses a warm process measures the previous arm.
# * THE MASK IS READ BACK AND PRINTED FOR EVERY SINGLE RUN, and a mismatch
#   aborts. The node is root-owned; a failed write is SILENT and leaves you
#   measuring one arm twice, which looks exactly like a clean negative. That
#   has already produced one worthless A/B.
# * THE MASK IS PUT BACK ON EVERY EXIT PATH, including Ctrl-C and an abort.
#   The script's normal operation leaves the knob flipped, and with the arms
#   alternating an odd rep count always ends on the OFF arm. A device was left
#   with retcache off on both arm64 and riscv64 this way, and nothing says so:
#   every later measurement on it silently ran against a crippled build.
# * REFUSES TO RUN ON AN UNOPTIMIZED BUILD. `uname -v` carries " unoptimized"
#   when the emulator was built -O0, where measurements are not merely slower
#   but wrong (an -O0 build once made the crypto accelerator look like a 2x
#   loss where -O2 shows a 14x win).
# * PRINTS THE SHELL AND LIBC BEHIND EACH WORKLOAD. dash-vs-busybox and
#   glibc-vs-musl differences have been mistaken for engine differences: a
#   mismatched pair once produced a "3.24x slower" cross-arch figure that was
#   really ~1.6-1.9x.
#
# Keep this and its binary in $HOME/bench, not /tmp: reinstalling the app wipes
# the guest /tmp. The script is also served read-only at /AOK/tests, so a fresh
# device only needs `cp /AOK/tests/jit_fuse_ab.sh ~/bench/`.
#
# The compute workloads need jit_bench.c built for the guest under test, kept as
# $HOME/bench/jitbench-<arch> and copied into the target root automatically:
#   cc -O2 -static -o ~/bench/jitbench-<arch> /AOK/tests/jit_bench.c
# shloop and forkexec need nothing, which is why they are the default pair for a
# guest with no compiler.

set -u

if [ $# -lt 4 ]; then
    sed -n '2,8p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
fi
arch=$1; bit=$2; reps=$3; shift 3

# Roots on the A9 rig; override for another device. NEVER trust a root's name
# for its architecture -- read the ELF e_machine of its busybox/dash instead
# (od -An -tx1 -j18 -N1 <bin>: 03=i386 3e=amd64 b7=aarch64 f3=riscv64).
: "${ISH_ROOT_RISCV64:=/AOK/roots/Devuan6-riscv64}"
: "${ISH_ROOT_I386:=/AOK/roots/Devuan6}"
: "${ISH_ROOT_AMD64:=/AOK/roots/Devuan6-x86_64}"

node=/proc/ish/${arch}_jit_fuse
[ -r "$node" ] || { echo "no such knob: $node" >&2; exit 1; }

# Put the mask back the way we found it, on EVERY exit path.
#
# This script's whole job is leaving the knob somewhere other than where it
# started, once per timed run, and it used to just stop -- so the mask kept
# whichever arm the last rep happened to end on. With the arms alternating by
# rep, an odd rep count always ends on the OFF arm, and a plain `retcache off`
# is invisible: nothing warns, the build simply runs slower forever. That is
# how a device was left with retcache disabled on BOTH arm64 and riscv64, so
# every measurement taken on it afterwards was against a crippled build.
#
# Snapshot the WHOLE mask rather than just $bit. `all` is a write-only
# pseudo-name -- it sets every family but never appears in the node's output --
# so there is no "previous value of all" to write back, and restoring `all=1`
# would switch on families that started off. Reading every "name on|off" line
# and writing every one back is exact for `all` and for any single bit, and it
# also repairs a run interrupted midway through a rep.
snapshot_mask() {
    awk '{printf "%s=%d ", $1, ($2 == "on") ? 1 : 0}' "$node"
}
orig_mask=$(snapshot_mask)
restored=0
restore_mask() {
    [ "$restored" = 0 ] || return 0
    restored=1
    [ -n "$orig_mask" ] || return 0
    echo "$orig_mask" | sudo tee "$node" >/dev/null 2>&1
    now=$(snapshot_mask)
    if [ "$now" != "$orig_mask" ]; then
        echo "WARNING: could not restore $node" >&2
        echo "  wanted: $orig_mask" >&2
        echo "  now:    $now" >&2
        echo "  fix by hand: echo \"$orig_mask\" | sudo tee $node" >&2
        return 1
    fi
    echo "restored $node: $orig_mask"
}
# INT/TERM do not exit on their own in POSIX sh, so say so explicitly; the EXIT
# trap then no-ops on the `restored` flag rather than restoring twice.
trap 'restore_mask; exit 130' INT
trap 'restore_mask; exit 143' TERM HUP
trap 'restore_mask' EXIT
if [ -z "$orig_mask" ]; then
    echo "WARNING: $node read back empty; cannot restore it afterwards" >&2
fi

case $arch in
arm64|aarch64) root="" ;;                 # the native guest, no chroot
riscv64)       root=$ISH_ROOT_RISCV64 ;;
i386)          root=$ISH_ROOT_I386 ;;
amd64)         root=$ISH_ROOT_AMD64 ;;
*) echo "unknown guest arch: $arch" >&2; exit 1 ;;
esac
if [ -n "$root" ] && [ ! -d "$root" ]; then
    echo "root for $arch not found: $root (set ISH_ROOT_$(echo "$arch" | tr a-z A-Z))" >&2
    exit 1
fi

build=$(uname -v)
case $build in
*unoptimized*)
    echo "REFUSING: this emulator is an unoptimized build -- '$build'" >&2
    echo "  measurements on -O0 are not slow-but-proportional, they are wrong." >&2
    exit 1 ;;
esac

# Run a command in the guest under test: chroot for the non-native arches.
guest() {
    if [ -n "$root" ]; then sudo chroot "$root" "$@"; else "$@"; fi
}

# Stage the compute binary from its golden copy if the root lost it (a rebuilt
# root, a fresh image). /AOK/roots survives an app restart; a rebuild does not.
stage_bench() {
    if [ -n "$root" ]; then
        [ -x "$root/jitbench" ] && return 0
        if [ -x "$HOME/bench/jitbench-$arch" ]; then
            sudo cp "$HOME/bench/jitbench-$arch" "$root/jitbench" &&
                sudo chmod 755 "$root/jitbench" && return 0
        fi
    else
        [ -x "$HOME/bench/jitbench-$arch" ] && return 0
    fi
    return 1
}

bench() {
    if [ -n "$root" ]; then guest /jitbench "$@"; else "$HOME/bench/jitbench-$arch" "$@"; fi
}

set_arm() {
    echo "$bit=$1" | sudo tee "$node" >/dev/null 2>&1
    want=on; [ "$1" = 0 ] && want=off
    rb=$(grep "^$bit " "$node")
    if [ "$rb" != "$bit $want" ]; then
        echo "ABORT: asked $bit=$1, node reads '$rb' (write refused? need sudo?)" >&2
        exit 1
    fi
    echo "$rb"
}

work() {
    case $1 in
    shloop)   guest /bin/sh -c 'i=0; while [ $i -lt 20000 ]; do i=$((i+1)); done' ;;
    forkexec) guest /bin/sh -c 'i=0; while [ $i -lt 100 ]; do /bin/true; i=$((i+1)); done' ;;
    fib)      bench fib 35 >/dev/null ;;
    indirect) bench ind 60000000 >/dev/null ;;
    *) echo "unknown workload: $1" >&2; exit 1 ;;
    esac
}

echo "build : $build"
echo "guest : $arch   knob: $node   bit: $bit"
echo "root  : ${root:-<native>}   shell: $(guest /bin/sh -c 'readlink -f /bin/sh' 2>/dev/null)"

for w in "$@"; do
    case $w in
    fib|indirect)
        if ! stage_bench; then
            echo "=== $w: SKIP (no \$HOME/bench/jitbench-$arch; build it from /AOK/tests/jit_bench.c)"
            continue
        fi ;;
    esac
    echo "=== $w ($reps reps, interleaved, ms) ==="
    on_list=""; off_list=""; rep=0
    while [ "$rep" -lt "$reps" ]; do
        if [ $((rep % 2)) -eq 0 ]; then order="1 0"; else order="0 1"; fi
        on_ms=""; off_ms=""; on_rb=""; off_rb=""
        for a in $order; do
            rb=$(set_arm "$a")
            t0=$(date +%s%N)
            work "$w"
            t1=$(date +%s%N)
            ms=$(( (t1 - t0) / 1000000 ))
            if [ "$a" = 1 ]; then on_ms=$ms; on_rb=$rb; else off_ms=$ms; off_rb=$rb; fi
        done
        on_list="$on_list $on_ms"; off_list="$off_list $off_ms"
        d=$(awk "BEGIN{printf \"%+6.2f\", 100*($off_ms-$on_ms)/$off_ms}")
        echo "  rep $rep  on[$on_rb]=${on_ms}ms  off[$off_rb]=${off_ms}ms  delta=${d}%"
        rep=$((rep + 1))
    done
    echo "$on_list|$off_list" | awk -F'|' '{
        n = split($1, a, " "); split($2, b, " ");
        for (i = 1; i <= n; i++) for (j = i + 1; j <= n; j++) {
            if (a[j] < a[i]) { t = a[i]; a[i] = a[j]; a[j] = t }
            if (b[j] < b[i]) { t = b[i]; b[i] = b[j]; b[j] = t }
        }
        m = int((n + 1) / 2);
        printf "  median on=%dms off=%dms win=%+.2f%%   min on=%dms off=%dms win=%+.2f%%\n",
               a[m], b[m], 100 * (b[m] - a[m]) / b[m], a[1], b[1], 100 * (b[1] - a[1]) / b[1];
    }'
done
