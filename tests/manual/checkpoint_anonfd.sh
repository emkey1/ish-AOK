#!/bin/sh
# checkpoint_anonfd.sh -- see checkpoint_anonfd.c.
#     tests/manual/checkpoint_anonfd.sh [root]      (needs gcc in root)
# On the app's save path (ISH_CHECKPOINT_AFTER), killed the moment the image
# exists, the way iOS kills the app -- so only the restored run checks.
set -e
REPO=$(cd "$(dirname "$0")/../.." && pwd)
ISH=${ISH:-$REPO/build/ish}
ROOT=${1:-$REPO/build/devuan-amd64-test}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/aok-anonfd.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
cp "$REPO/tests/manual/checkpoint_anonfd.c" "$WORK/"
ISH_REAL_MNT=$WORK "$ISH" -f "$ROOT" /bin/sh -c \
    'gcc -O1 -o /realmnt/probe /realmnt/checkpoint_anonfd.c' \
    || { echo "FAIL: could not build the probe"; exit 1; }
# stdin from /dev/null throughout: a broken restore can hand a descriptor the
# CLI's own stdin, and a read on that must end, not wait for a terminal.
ISH_REAL_MNT=$WORK ISH_CHECKPOINT_AFTER="2:$WORK/img" "$ISH" -f "$ROOT" /realmnt/probe \
    < /dev/null > "$WORK/save.out" 2>&1 &
saver=$!
n=0; while [ ! -s "$WORK/img" ] && [ $n -lt 100 ]; do sleep 0.1; n=$((n+1)); done
kill -9 $saver 2>/dev/null || true; wait $saver 2>/dev/null || true
[ -s "$WORK/img" ] || { echo "FAIL: no image written"; exit 1; }
out=$(ISH_REAL_MNT=$WORK ISH_RESTORE="$WORK/img" "$ISH" -f "$ROOT" < /dev/null 2>&1 || true)
echo "$out" | sed 's/^/  /'
fail=0
for k in epoll eventfd signalfd timerfd inotify memfd pidfd pidfd-gone; do
    echo "$out" | grep -q "^OK $k" || { echo "  FAIL    | $k did not work after the restore"; fail=1; }
done
[ $fail -eq 0 ] || { echo "FAIL"; exit 1; }
echo "PASS: every descriptor with no file behind it came back working"
