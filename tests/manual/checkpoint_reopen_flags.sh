#!/bin/sh
# checkpoint_reopen_flags.sh -- see checkpoint_reopen_flags.c.
#     tests/manual/checkpoint_reopen_flags.sh [root]     (needs gcc in root)
set -e
REPO=$(cd "$(dirname "$0")/../.." && pwd)
ISH=${ISH:-$REPO/build/ish}
ROOT=${1:-$REPO/build/devuan-amd64-test}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/aok-reopen.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
cp "$REPO/tests/manual/checkpoint_reopen_flags.c" "$WORK/"
ISH_REAL_MNT=$WORK "$ISH" -f "$ROOT" /bin/sh -c \
    'gcc -O1 -o /realmnt/probe /realmnt/checkpoint_reopen_flags.c' \
    || { echo "FAIL: could not build the probe"; exit 1; }
"$ISH" -f "$ROOT" /bin/sh -c 'rm -f /tmp/ckrf-trunc /tmp/ckrf-excl-*'
ISH_REAL_MNT=$WORK ISH_CHECKPOINT_AFTER="2:$WORK/img" "$ISH" -f "$ROOT" /realmnt/probe \
    > "$WORK/save.out" 2>&1 &
saver=$!
# The image is renamed into place only once complete; kill the saving run the
# moment it is, as iOS would, so nothing after the checkpoint runs twice.
n=0; while [ ! -s "$WORK/img" ] && [ $n -lt 100 ]; do sleep 0.1; n=$((n+1)); done
kill -9 $saver 2>/dev/null || true; wait $saver 2>/dev/null || true
[ -s "$WORK/img" ] || { echo "FAIL: no image written"; exit 1; }
out=$(ISH_REAL_MNT=$WORK ISH_RESTORE="$WORK/img" "$ISH" -f "$ROOT" 2>&1 || true)
echo "$out" | sed 's/^/  restore | /'
fail=0
for want in 'TRUNC=[before|after|]' 'EXCL=[before|after|]' \
        'PIPE=[nonblock 1/1 blocking 0 empty-read -1/EAGAIN]'; do
    if echo "$out" | grep -qxF "$want"; then echo "  ok      | $want"
    else echo "  FAIL    | want $want"; fail=1; fi
done
[ $fail -eq 0 ] || { "$ISH" -f "$ROOT" /bin/sh -c 'rm -f /tmp/ckrf-trunc /tmp/ckrf-excl-*'; echo "FAIL: a restored descriptor lost or replayed its flags"; exit 1; }
"$ISH" -f "$ROOT" /bin/sh -c 'rm -f /tmp/ckrf-trunc /tmp/ckrf-excl-*'
echo "PASS: reopened descriptors kept their files intact, and pipes their flags"
