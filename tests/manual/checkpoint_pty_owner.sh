#!/bin/sh
# checkpoint_pty_owner.sh -- a user's pty survives a checkpoint, owned by them.
# See checkpoint_pty_owner.c for the shape and the two bugs it pins.
#
#     tests/manual/checkpoint_pty_owner.sh [root]
#
# Uses the APP's save path (ISH_CHECKPOINT_AFTER, checkpoint_save_external).
# The root needs gcc: build/devuan-amd64-test by default.
set -e
REPO=$(cd "$(dirname "$0")/../.." && pwd)
ISH=${ISH:-$REPO/build/ish}
ROOT=${1:-$REPO/build/devuan-amd64-test}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/aok-ptyown.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
cp "$REPO/tests/manual/checkpoint_pty_owner.c" "$WORK/"
ISH_REAL_MNT=$WORK "$ISH" -f "$ROOT" /bin/sh -c \
    'gcc -O1 -o /realmnt/ptyown /realmnt/checkpoint_pty_owner.c' \
    || { echo "FAIL: could not build the probe"; exit 1; }
"$ISH" -f "$ROOT" /bin/sh -c 'rm -f /tmp/ptyown-child /tmp/ptyown-owner'

ISH_REAL_MNT=$WORK ISH_CHECKPOINT_AFTER="4:$WORK/img" "$ISH" -f "$ROOT" \
    /realmnt/ptyown > "$WORK/save.out" 2>&1 || true
[ -s "$WORK/img" ] || { echo "FAIL: no image written"; exit 1; }
# Only the restored run may leave the witnesses behind.
"$ISH" -f "$ROOT" /bin/sh -c 'rm -f /tmp/ptyown-child /tmp/ptyown-owner'

rc=0
ISH_REAL_MNT=$WORK ISH_RESTORE="$WORK/img" "$ISH" -f "$ROOT" \
    > "$WORK/restore.out" 2>&1 || rc=$?
sed 's/^/  restore | /' "$WORK/restore.out"
got=$("$ISH" -f "$ROOT" /bin/sh -c \
    'cat /tmp/ptyown-child /tmp/ptyown-owner 2>/dev/null; rm -f /tmp/ptyown-child /tmp/ptyown-owner')
echo "$got" | sed 's/^/  guest   | /'

fail=0
[ $rc -eq 0 ] || { echo "  FAIL    | the restore itself failed (exit $rc)"; fail=1; }
echo "$got" | grep -qx 'child-alive' \
    && echo "  ok      | the uid-1000 child came back and ran to the end" \
    || { echo "  FAIL    | the uid-1000 child did not survive the restore"; fail=1; }
echo "$got" | grep -qx 'OWNER=1000:5:620' \
    && echo "  ok      | the rebuilt slave is still the user's (1000:5, 0620)" \
    || { echo "  FAIL    | the rebuilt slave lost its owner (want OWNER=1000:5:620)"; fail=1; }
[ $fail -eq 0 ] || { echo "FAIL"; exit 1; }
echo "PASS: a user's pty came back theirs, and openable by them"
