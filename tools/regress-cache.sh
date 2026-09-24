#!/bin/sh
# Keep a copy of the guest test cache on this Mac, and seed a device from it.
#
#     tools/regress-cache.sh pull <ssh args...>    device -> this Mac
#     tools/regress-cache.sh push <ssh args...>    this Mac -> device
#
# e.g. tools/regress-cache.sh pull -p 1022 169.254.109.106
#
# The cache is what tests/manual/setup-regressions.sh compiles each guest test
# into: one executable per test per toolchain, named
# <test>.<toolchain key>.<source hash>. A hit needs both hashes to match, so
# caches from different devices, roots and builds merge safely: an entry for a
# toolchain or a source that no longer exists is just never used. On a device
# it is /AOK/fakefs/regress-cache. Here it is $ISH_AOK_REGRESS_STORE, by
# default ~/.cache/ish-aok-regress-cache. ISH_AOK_REGRESS_DEVICE_DIR moves the
# device end, for trying a push without touching the real one.
#
# Why keep one here at all: compiling the suite on a device is SLOW. The i386
# root alone took two hours on the M4 iPad (556, 2026-09-24), and a new device,
# or a device whose cache got wiped, would pay that for every root. With this,
# it pays nothing for the tests it has seen before.
#
# Neither direction overwrites an entry already there: equal names are equal
# binaries, and a push must never clobber a cache a run is writing into.
set -eu

store=${ISH_AOK_REGRESS_STORE:-$HOME/.cache/ish-aok-regress-cache}
device_dir=${ISH_AOK_REGRESS_DEVICE_DIR:-/AOK/fakefs/regress-cache}

usage() {
    echo "usage: $0 pull|push <ssh args...>" >&2
    exit 2
}
[ $# -ge 2 ] || usage
direction=$1
shift

mkdir -p "$store"
before=$(ls "$store" | wc -l | tr -d ' ')

case $direction in
    pull)
        # Unpack beside the store, then add only what is missing.
        stage=$(mktemp -d "${TMPDIR:-/tmp}/regress-cache-pull.XXXXXX")
        trap 'rm -rf "$stage"' EXIT
        ssh "$@" "cd $device_dir && tar cf - ." | tar xf - -C "$stage"
        rm -f "$stage"/.probe* "$stage"/.regress-cache-probe
        cp -pn "$stage"/* "$store"/ 2>/dev/null || true
        after=$(ls "$store" | wc -l | tr -d ' ')
        echo "pulled: $(ls "$stage" | wc -l | tr -d ' ') on the device, $((after - before)) new here, $after in $store"
        ;;
    push)
        # The device's cache dir is root-owned after a root run, so use sudo
        # when it is not writable (the devices here have passwordless sudo).
        # Unpack to a temp dir there too, and cp -n: GNU tar's -k errors on
        # every file that exists, and busybox's differs again.
        # --no-xattrs/--no-mac-metadata and COPYFILE_DISABLE: macOS tar would
        # otherwise attach com.apple.provenance to every entry, and the
        # device's GNU tar prints a warning per file.
        COPYFILE_DISABLE=1 tar --no-xattrs --no-mac-metadata -cf - -C "$store" . | ssh "$@" "
            set -e
            s=; [ -w $device_dir ] || [ -w /AOK/fakefs ] || s='sudo -n'
            [ -d $device_dir ] && [ ! -w $device_dir ] && s='sudo -n'
            t=\$(mktemp -d)
            tar xf - -C \"\$t\"
            \$s mkdir -p $device_dir
            n0=\$(ls $device_dir | wc -l)
            \$s cp -pn \"\$t\"/* $device_dir/ 2>/dev/null || true
            \$s chmod 1777 $device_dir 2>/dev/null || true
            rm -rf \"\$t\"
            echo \"pushed: \$((\$(ls $device_dir | wc -l) - n0)) new on the device, \$(ls $device_dir | wc -l) there now\""
        ;;
    *)
        usage
        ;;
esac
