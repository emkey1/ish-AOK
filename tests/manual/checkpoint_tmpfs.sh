#!/bin/sh
# checkpoint_tmpfs.sh -- a tmpfs is RAM with a path, so the image has to carry
# it or a restore hands the session back onto an empty /run.
#
# The bug this pins: a process whose stdin was /run/pacct_source came back to
# ENOENT because nothing outside the image remembered /run at all. Same for
# every pid file, lock file and AF_UNIX name a restored session expects to
# still be there.
#
# So this mounts a tmpfs, fills it with one of each kind of thing that lives
# in a real /run -- a file with contents, a nested directory, a symlink, a
# fifo, a mode that is not the default, a non-root owner -- checkpoints, and
# then checks a SEPARATE ish process restored from the image sees all of it.
#
#     tests/manual/checkpoint_tmpfs.sh [root]
set -e

REPO=$(cd "$(dirname "$0")/../.." && pwd)
ISH=${ISH:-$REPO/build/ish}
ROOT=${1:-$REPO/build/devuan-arm64-test}
IMG=${TMPDIR:-/tmp}/aok-ckpt-tmpfs-$$.img
SH=/bin/dash
[ -d "$ROOT" ] || { echo "no root at $ROOT" >&2; exit 2; }
[ -x "$ISH" ] || { echo "build/ish first" >&2; exit 2; }
trap 'rm -f "$IMG"' EXIT

PROG='
mount -t tmpfs tmpfs /run || { echo "FAIL: could not mount tmpfs on /run"; exit 1; }
mkdir -p /run/nested/deeper
echo pacct-source-payload > /run/nested/deeper/data.txt
printf "no-trailing-newline" > /run/short
ln -s nested/deeper/data.txt /run/alias
mkfifo /run/apipe
chmod 0710 /run/nested
chown 1000:1001 /run/short
# The descriptor that started all this: a process holding a /run file open
# across the checkpoint. It is reopened BY PATH on the far side, so it is the
# one thing that cannot work unless the tree is back before the fd loop runs.
exec 3< /run/nested/deeper/data.txt
echo "pre : $(cat /run/nested/deeper/data.txt)"
echo save IMAGE > /proc/ish/checkpoint
# Everything from here runs twice: once in the guest that saved the image and
# once in the guest restored FROM it. Only the second run is checked.
echo "R-data=$(cat /run/nested/deeper/data.txt 2>&1)"
echo "R-short=$(cat /run/short 2>&1)"
echo "R-link=$(readlink /run/alias 2>&1)"
echo "R-fifo=$(test -p /run/apipe && echo yes || echo no)"
echo "R-mode=$(stat -c %a /run/nested 2>&1)"
echo "R-own=$(stat -c %u:%g /run/short 2>&1)"
read -r held <&3 || held="fd-3-was-lost"
echo "R-fd=$held"
'

save_out=$(ISH_GUEST_CHECKPOINT=1 "$ISH" -f "$ROOT" $SH -c "$(printf '%s' "$PROG" | sed "s|IMAGE|$IMG|")" 2>&1)
echo "$save_out" | sed 's/^/  save    | /'
[ -s "$IMG" ] || { echo "FAIL: no image written"; exit 1; }

restore_out=$(ISH_RESTORE="$IMG" "$ISH" -f "$ROOT" 2>&1 || true)
echo "$restore_out" | sed 's/^/  restore | /'

fail=0
expect() {
    if echo "$restore_out" | grep -qxF "$1"; then
        echo "  ok      | $1"
    else
        echo "  FAIL    | expected: $1"
        fail=1
    fi
}
expect 'R-data=pacct-source-payload'
expect 'R-short=no-trailing-newline'
expect 'R-link=nested/deeper/data.txt'
expect 'R-fifo=yes'
expect 'R-mode=710'
expect 'R-own=1000:1001'
expect 'R-fd=pacct-source-payload'
# The restore has to mount a tmpfs before it fills one. Without that it writes
# the session's /run into the ROOTFS, where it outlives the next boot -- which
# looks identical above and is a different, worse bug. A fresh guest is the
# control: it mounts nothing, so anything still visible here was made permanent.
leaked=$("$ISH" -f "$ROOT" $SH -c 'ls /run' 2>&1 | grep -cE '^(nested|short|alias|apipe)$' || true)
if [ "$leaked" = 0 ]; then
    echo "  ok      | nothing leaked into the on-disk /run"
else
    echo "  FAIL    | $leaked entries were written to the rootfs, not a tmpfs"
    fail=1
fi


[ $fail -eq 0 ] || { echo "FAIL: /run did not come back intact"; exit 1; }
echo "PASS: the tmpfs came back with its contents, modes and ownership"
