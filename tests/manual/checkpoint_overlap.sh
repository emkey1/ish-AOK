#!/bin/sh
# checkpoint_overlap.sh -- two saves at once must not hurt the guest or the image.
#     tests/manual/checkpoint_overlap.sh [root]      (needs gcc in root)
# The app starts a save every time it goes to the background, so leaving, coming
# back and leaving again during a long save started a second. Its freeze succeeded
# at once, the first save's thaw then woke every task under it, and it walked tasks
# as they exited: a device crash in ckpt_save_task on 563, with no thread parked.
# Both also wrote the same temporary file.
#
# Runs ISH_CHECKPOINT_OVERLAP rounds of two external saves, the second starting
# while the first writes, then restores the image and checks its memory.
set -e
REPO=$(cd "$(dirname "$0")/../.." && pwd)
ISH=${ISH:-$REPO/build/ish}
ROOT=${1:-$REPO/build/alpine-arm64-324}
ROUNDS=${ROUNDS:-12}
GAP_MS=${GAP_MS:-10}
BALLAST_MB=${BALLAST_MB:-256}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/aok-overlap.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
cp "$REPO/tests/manual/checkpoint_overlap.c" "$WORK/"
ISH_REAL_MNT=$WORK "$ISH" -f "$ROOT" /bin/sh -c \
    'gcc -O1 -pthread -o /realmnt/probe /realmnt/checkpoint_overlap.c' \
    || { echo "FAIL: could not build the probe"; exit 1; }

ISH_REAL_MNT=$WORK ISH_CHECKPOINT_AFTER="@$WORK/ready:$WORK/img" \
    ISH_CHECKPOINT_OVERLAP="$ROUNDS:$GAP_MS" \
    "$ISH" -f "$ROOT" /realmnt/probe /realmnt/ready /realmnt/stop "$BALLAST_MB" \
    < /dev/null > "$WORK/save.out" 2>&1 &
saver=$!
want=$((ROUNDS * 2))
n=0
while [ "$(grep -c '^time' "$WORK/img.log" 2>/dev/null || true)" != "$want" ] &&
        kill -0 $saver 2>/dev/null && [ $n -lt 3000 ]; do
    sleep 0.1; n=$((n+1))
done
: > "$WORK/stop"
rc=0
wait $saver || rc=$?
grep -v '^time' "$WORK/img.log" 2>/dev/null | sort | uniq -c | sed 's/^/  log  | /' || true
tail -3 "$WORK/save.out" | sed 's/^/  save | /'
[ "$rc" -eq 0 ] || { echo "FAIL: the emulator died during the saves (exit $rc)"; exit 1; }
grep -q '^CORRUPT' "$WORK/save.out" && { echo "FAIL: the guest's memory changed"; exit 1; }
written=$(grep -c '^written' "$WORK/img.log" || true)
[ "$written" -eq "$want" ] || { echo "FAIL: $written of $want saves written"; exit 1; }
[ ! -e "$WORK/img.new" ] || { echo "FAIL: a temporary was left behind"; exit 1; }
# Each round logs its two saves in the order they ended, each with when it was asked
# for and when it was over. Without an overlap this tests nothing, so it has to
# have happened: the later one was asked for before the earlier one was done.
overlapped=$(awk '/^time/ { if (n % 2 == 0) { b1 = $2; e1 = $3 } else if ($2 < e1 && b1 < $3) c++; n++ } END { print c + 0 }' "$WORK/img.log")
echo "  $overlapped of $ROUNDS rounds overlapped"
[ "$overlapped" -gt 0 ] || { echo "FAIL: no two saves overlapped, so this tested nothing (raise BALLAST_MB)"; exit 1; }

rm -f "$WORK/stop" "$WORK/ready"
ISH_REAL_MNT=$WORK ISH_RESTORE="$WORK/img" "$ISH" -f "$ROOT" \
    < /dev/null > "$WORK/restore.out" 2>&1 &
restored=$!
n=0
while ! grep -q '^OK' "$WORK/restore.out" 2>/dev/null &&
        kill -0 $restored 2>/dev/null && [ $n -lt 600 ]; do
    sleep 0.1; n=$((n+1))
done
sleep 2
: > "$WORK/stop"
n=0
while kill -0 $restored 2>/dev/null && [ $n -lt 100 ]; do sleep 0.1; n=$((n+1)); done
kill -9 $restored 2>/dev/null || true
wait $restored 2>/dev/null || true
tail -3 "$WORK/restore.out" | sed 's/^/  back | /'
grep -q '^OK' "$WORK/restore.out" || { echo "FAIL: nothing ran after the restore"; exit 1; }
grep -q '^CORRUPT' "$WORK/restore.out" && { echo "FAIL: the image's memory is wrong"; exit 1; }
echo "PASS: $want overlapping saves written, and the image restores intact"
