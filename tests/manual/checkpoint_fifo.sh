#!/bin/sh
# checkpoint_fifo.sh -- see checkpoint_fifo.c.
#     tests/manual/checkpoint_fifo.sh [root]      (needs gcc in root)
set -e
REPO=$(cd "$(dirname "$0")/../.." && pwd)
ISH=${ISH:-$REPO/build/ish}
ROOT=${1:-$REPO/build/devuan-amd64-test}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/aok-fifo.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
cp "$REPO/tests/manual/checkpoint_fifo.c" "$WORK/"
ISH_REAL_MNT=$WORK "$ISH" -f "$ROOT" /bin/sh -c \
    'gcc -O1 -o /realmnt/probe /realmnt/checkpoint_fifo.c' < /dev/null \
    || { echo "FAIL: could not build the probe"; exit 1; }
# /run as a tmpfs, as the device has it; killed once the image exists, as iOS
# kills the app, so only the restored run reports.
ISH_REAL_MNT=$WORK ISH_CHECKPOINT_AFTER="3:$WORK/img" "$ISH" -f "$ROOT" /bin/sh -c \
    'mount -t tmpfs tmpfs /run && exec /realmnt/probe' < /dev/null > "$WORK/save.out" 2>&1 &
saver=$!
n=0; while [ ! -s "$WORK/img" ] && [ $n -lt 150 ]; do sleep 0.1; n=$((n+1)); done
kill -9 $saver 2>/dev/null || true; wait $saver 2>/dev/null || true
if [ ! -s "$WORK/img" ]; then
    sed 's/^/  save | /' "$WORK/save.out"
    echo "FAIL: no image written -- the checkpoint was refused"; exit 1
fi
out=$(ISH_REAL_MNT=$WORK ISH_RESTORE="$WORK/img" "$ISH" -f "$ROOT" < /dev/null 2>&1 || true)
echo "$out" | sed 's/^/  /'
fail=0
for k in INITCTL WRITER-FLAGS EPIPE; do
    echo "$out" | grep -q "^OK $k" || { echo "  FAIL    | $k"; fail=1; }
done
echo "$out" | grep -qxF 'CHILD-READ=[before|after|]' \
    || { echo "  FAIL    | BUFFER/JOIN: the reader did not get before|after|"; fail=1; }
[ $fail -eq 0 ] || { echo "FAIL"; exit 1; }
echo "PASS: named FIFOs came back as themselves, buffer and all"
