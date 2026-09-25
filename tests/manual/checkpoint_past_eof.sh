#!/bin/sh
# checkpoint_past_eof.sh -- see checkpoint_past_eof.c.
#     tests/manual/checkpoint_past_eof.sh [root]      (needs gcc in root)
#
# Two legs, one per save path: the guest's own `save` through
# /proc/ish/checkpoint, and the app's, from outside the guest
# (ISH_CHECKPOINT_AFTER, checkpoint_save_external). Before the fix both killed
# ish with SIGBUS while writing the image, so "no image written" is the failure
# this exists to catch.
set -e
REPO=$(cd "$(dirname "$0")/../.." && pwd)
ISH=${ISH:-$REPO/build/ish}
ROOT=${1:-$REPO/build/devuan-amd64-test}
[ -d "$ROOT" ] || { echo "no root at $ROOT" >&2; exit 2; }
[ -x "$ISH" ] || { echo "build/ish first" >&2; exit 2; }
WORK=$(mktemp -d "${TMPDIR:-/tmp}/aok-past-eof.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
cp "$REPO/tests/manual/checkpoint_past_eof.c" "$WORK/"
ISH_REAL_MNT=$WORK "$ISH" -f "$ROOT" /bin/sh -c \
    'gcc -O1 -o /realmnt/probe /realmnt/checkpoint_past_eof.c' < /dev/null \
    || { echo "FAIL: could not build the probe"; exit 1; }

fail=0
# $1 label, $2 the saving run's output, then the restore is run and graded.
grade() {
    label=$1; saved=$2
    sed "s/^/  $label saved    | /" "$saved"
    if [ ! -s "$WORK/img" ]; then
        echo "  FAIL    | $label: no image written"; fail=1; return
    fi
    out=$(ISH_REAL_MNT=$WORK ISH_RESTORE="$WORK/img" "$ISH" -f "$ROOT" < /dev/null 2>&1 || true)
    echo "$out" | sed "s/^/  $label restored | /"
    case $out in
        *A-BEFORE-SAVE*) echo "  FAIL    | $label: the restored guest re-ran rather than continued"; fail=1 ;;
    esac
    case $out in
        *"life: restored"*"RESULT: PASS"*) ;;
        *) echo "  FAIL    | $label: the restored run did not pass"; fail=1 ;;
    esac
}

# ---- the guest's own save ---------------------------------------------------
# The image path is the HOST's: the save writes it from outside the guest's
# filesystem.
rm -f "$WORK/img"
ISH_REAL_MNT=$WORK ISH_GUEST_CHECKPOINT=1 "$ISH" -f "$ROOT" /realmnt/probe "$WORK/img" \
    < /dev/null > "$WORK/save.out" 2>&1 || true
grep -q '^life: original' "$WORK/save.out" && grep -q '^RESULT: PASS' "$WORK/save.out" \
    || { echo "  FAIL    | guest: the saving run did not pass"; fail=1; }
grade guest "$WORK/save.out"

# ---- from outside the guest -------------------------------------------------
# Killed once the image exists, as iOS kills the app; the saving run's own
# checks are the guest leg's.
rm -f "$WORK/img" "$WORK/img.log"
ISH_REAL_MNT=$WORK ISH_CHECKPOINT_AFTER="3:$WORK/img" "$ISH" -f "$ROOT" \
    /realmnt/probe --sleep 8 < /dev/null > "$WORK/ext.out" 2>&1 &
saver=$!
n=0; while [ ! -s "$WORK/img.log" ] && kill -0 $saver 2>/dev/null && [ $n -lt 200 ]; do
    sleep 0.1; n=$((n+1)); done
kill -9 $saver 2>/dev/null || true; wait $saver 2>/dev/null || true
case $(cat "$WORK/img.log" 2>/dev/null) in
    written*) ;;
    *) echo "  FAIL    | outside: the save said: $(cat "$WORK/img.log" 2>/dev/null)"; fail=1 ;;
esac
grade outside "$WORK/ext.out"

[ $fail -eq 0 ] || { echo "FAIL"; exit 1; }
echo "PASS: a file mapping longer than its file was saved and came back, from inside the guest and from outside it"
