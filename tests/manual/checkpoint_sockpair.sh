#!/bin/sh
# checkpoint_sockpair.sh -- see checkpoint_sockpair.c.
#     tests/manual/checkpoint_sockpair.sh [root]      (needs gcc in root)
set -e
REPO=$(cd "$(dirname "$0")/../.." && pwd)
ISH=${ISH:-$REPO/build/ish}
ROOT=${1:-$REPO/build/devuan-amd64-test}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/aok-sockpair.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
export ISH_FORCE_SEQPACKET_EPERM=1          # refuse SEQPACKET, as iOS does
cp "$REPO/tests/manual/checkpoint_sockpair.c" "$WORK/"
ISH_REAL_MNT=$WORK "$ISH" -f "$ROOT" /bin/sh -c \
    'gcc -O1 -o /realmnt/probe /realmnt/checkpoint_sockpair.c' < /dev/null \
    || { echo "FAIL: could not build the probe"; exit 1; }
ISH_REAL_MNT=$WORK ISH_CHECKPOINT_AFTER="3:$WORK/img" "$ISH" -f "$ROOT" /bin/sh -c \
    'mount -t tmpfs tmpfs /run && exec /realmnt/probe' < /dev/null > "$WORK/save.out" 2>&1 &
saver=$!
# The run that saved goes on to the end: the save read the pairs' queues and
# must have left them as they were.
n=0; while kill -0 $saver 2>/dev/null && [ $n -lt 300 ]; do sleep 0.1; n=$((n+1)); done
kill -9 $saver 2>/dev/null || true; wait $saver 2>/dev/null || true
if [ ! -s "$WORK/img" ]; then
    sed 's/^/  save | /' "$WORK/save.out"; echo "FAIL: no image written"; exit 1
fi
out=$(ISH_REAL_MNT=$WORK ISH_RESTORE="$WORK/img" "$ISH" -f "$ROOT" < /dev/null 2>&1 || true)
sed 's/^/  saved    | /' "$WORK/save.out"
echo "$out" | sed 's/^/  restored | /'
fail=0
for k in LISTENER QUEUED-DGRAM QUEUED-STREAM QUEUED-FULL QUEUED-BIG SOCKETPAIR CONNECTION; do
    grep -q "^OK $k" "$WORK/save.out" || { echo "  FAIL    | $k, in the run that saved"; fail=1; }
    echo "$out" | grep -q "^OK $k" || { echo "  FAIL    | $k, restored"; fail=1; }
done
grep -qxF 'CHILD-GOT=[pong]' "$WORK/save.out" || { echo "  FAIL    | the saving run's child never heard back"; fail=1; }
echo "$out" | grep -qxF 'CHILD-GOT=[pong]' || { echo "  FAIL    | the restored child never heard back"; fail=1; }
[ $fail -eq 0 ] || { echo "FAIL"; exit 1; }
echo "PASS: local sockets came back listening, connected, and holding what they held"
