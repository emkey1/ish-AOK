#!/bin/sh
# checkpoint_leave_out_gpu.sh -- a save that leaves the GPU out.
#
# What stands behind /dev/dri/renderD128 and the Wayland view's input lives in
# the app and cannot go into an image. A plain save refuses ("a Wayland desktop
# or GPU program is running"); the app's background and "Suspend Anyway" saves
# pass CKPT_SAVE_LEAVE_OUT_GPU (the CLI: ISH_CHECKPOINT_LEAVE_OUT_GPU=1) and
# write everything else: the holder, its threads and its descendants are left
# out, and the rest comes back working.
#
#     tests/manual/checkpoint_leave_out_gpu.sh [root]
#
# The guest shell starts G, a subshell holding the render node with a child of
# its own, and K, an unrelated process. After the restore: G and its child are
# gone, K is alive, and the shell's wait for G returns instead of hanging.
set -e
REPO=$(cd "$(dirname "$0")/../.." && pwd)
ISH=${ISH:-$REPO/build/ish}
ROOT=${1:-$REPO/build/devuan-arm64-test}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/aok-gpuout.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
cat > "$WORK/probe.sh" <<'EOF'
( exec 5<>/dev/dri/renderD128; sleep 300 & sleep 300 ) &
G=$!
sleep 301 &
K=$!
touch /realmnt/ready
sleep 6
# Only the restored guest gets here: the save run is killed once the image exists.
if kill -0 $G 2>/dev/null; then echo "FAIL holder: still here"; else echo "OK holder: left out"; fi
if kill -0 $K 2>/dev/null; then echo "OK kept: alive"; else echo "FAIL kept: gone"; fi
n300=$(ps -e -o args= | grep -c '^sleep 300$' || true)
[ "$n300" -eq 0 ] && echo "OK descendants: left out" || echo "FAIL descendants: $n300 left"
wait $G
echo "OK wait: returned $?"
kill $K
EOF

save() {   # <extra env assignment or empty>
    rm -f "$WORK/img" "$WORK/img.log" "$WORK/ready"
    env $1 ISH_REAL_MNT=$WORK ISH_CHECKPOINT_AFTER="@$WORK/ready:$WORK/img" \
        "$ISH" -f "$ROOT" /bin/sh /realmnt/probe.sh < /dev/null > "$WORK/save.out" 2>&1 &
    saver=$!
    n=0
    while [ ! -s "$WORK/img.log" ] && kill -0 $saver 2>/dev/null && [ $n -lt 1200 ]; do
        sleep 0.1; n=$((n+1))
    done
    kill -9 $saver 2>/dev/null || true; wait $saver 2>/dev/null || true
}

fail=0
# The control: without the option, the save refuses and says why.
save ""
if [ -s "$WORK/img" ] || ! grep -q 'Wayland desktop or GPU program' "$WORK/img.log" 2>/dev/null; then
    echo "  FAIL    | a plain save did not refuse: $(cat "$WORK/img.log" 2>/dev/null)"; fail=1
else
    echo "  OK refused: $(cat "$WORK/img.log")"
fi

save ISH_CHECKPOINT_LEAVE_OUT_GPU=1
[ -s "$WORK/img" ] || { echo "FAIL: no image written: $(cat "$WORK/img.log" 2>/dev/null)"; exit 1; }
out=$(ISH_REAL_MNT=$WORK ISH_RESTORE="$WORK/img" perl -e 'alarm 60; exec @ARGV' "$ISH" -f "$ROOT" < /dev/null 2>&1 || true)
echo "$out" | grep -E '^(OK|FAIL)' | sed 's/^/  /'
for k in holder kept descendants wait; do
    echo "$out" | grep -q "^OK $k" || { echo "  FAIL    | $k"; fail=1; }
done
[ $fail -eq 0 ] || { echo "FAIL"; exit 1; }
echo "PASS: the GPU holder and its children were left out; the rest came back"
