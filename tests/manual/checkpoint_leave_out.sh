#!/bin/sh
# checkpoint_leave_out.sh -- a save that leaves the GPU out.
#
# What stands behind /dev/dri/renderD128 and the Wayland view's input lives in
# the app and cannot go into an image, and nor can a memfd past 64 MB (wayvnc's
# screen buffers). A plain save refuses; the app's background and "Suspend
# Anyway" saves pass CKPT_SAVE_LEAVE_OUT (the CLI: ISH_CHECKPOINT_LEAVE_OUT=1)
# and write everything else: each such process, its threads and its
# descendants are left out, and the rest comes back working.
#
#     tests/manual/checkpoint_leave_out.sh [root]
#
# The guest shell starts G, a subshell holding the render node with a child of
# its own; M, a process holding an 80 MB memfd; and K, an unrelated process.
# And J, a job shaped like the Wayland desktop session: a setsid shell whose
# process group holds a GPU subshell and a plain `sleep 302` that holds
# nothing. The whole group goes, not only the holder -- a kept wayvnc and
# start-wayland.sh used to linger after a resume and refuse every new
# desktop as "already running".
# After the restore: G, its child, M and all of J are gone, K is alive, and
# the shell's waits return instead of hanging.  (Needs gcc in the root.)
set -e
REPO=$(cd "$(dirname "$0")/../.." && pwd)
ISH=${ISH:-$REPO/build/ish}
ROOT=${1:-$REPO/build/devuan-arm64-test}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/aok-gpuout.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
cat > "$WORK/bigmemfd.c" <<'EOF'
#define _GNU_SOURCE
#include <sys/mman.h>
#include <unistd.h>
int main(void) {
    int fd = memfd_create("big", 0);
    if (fd < 0 || ftruncate(fd, 80 << 20) != 0)
        return 1;
    pwrite(fd, "x", 1, (80 << 20) - 1);
    for (;;)
        pause();
}
EOF
ISH_REAL_MNT=$WORK "$ISH" -f "$ROOT" /bin/sh -c 'gcc -O1 -o /realmnt/bigmemfd /realmnt/bigmemfd.c' \
    || { echo "FAIL: could not build the memfd holder"; exit 1; }
cat > "$WORK/probe.sh" <<'EOF'
( exec 5<>/dev/dri/renderD128; sleep 300 & sleep 300 ) &
G=$!
/realmnt/bigmemfd &
M=$!
setsid sh -c '( exec 5<>/dev/dri/renderD128; sleep 303 ) & sleep 302 & wait' &
J=$!
sleep 301 &
K=$!
# J's children exist before the checkpoint, or the save lands first and the
# restored shell starts them afresh.
n=0
while [ "$(ps -e -o args= | grep -c -E '^sleep 30[23]$')" -lt 2 ] && [ $n -lt 100 ]; do
    sleep 0.1; n=$((n+1))
done
touch /realmnt/ready
sleep 6
# Only the restored guest gets here: the save run is killed once the image exists.
if kill -0 $G 2>/dev/null; then echo "FAIL holder: still here"; else echo "OK holder: left out"; fi
if kill -0 $K 2>/dev/null; then echo "OK kept: alive"; else echo "FAIL kept: gone"; fi
n300=$(ps -e -o args= | grep -c '^sleep 300$' || true)
[ "$n300" -eq 0 ] && echo "OK descendants: left out" || echo "FAIL descendants: $n300 left"
if kill -0 $M 2>/dev/null; then echo "FAIL memfd: still here"; else echo "OK memfd: left out"; fi
n302=$(ps -e -o args= | grep -c -E '^sleep 30[23]$' || true)
[ "$n302" -eq 0 ] && echo "OK group: left out whole" || echo "FAIL group: $n302 of the job left"
wait $G
echo "OK wait: returned $?"
wait $M
echo "OK waitm: returned $?"
wait $J
echo "OK waitj: returned $?"
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

save ISH_CHECKPOINT_LEAVE_OUT=1
[ -s "$WORK/img" ] || { echo "FAIL: no image written: $(cat "$WORK/img.log" 2>/dev/null)"; exit 1; }
out=$(ISH_REAL_MNT=$WORK ISH_RESTORE="$WORK/img" perl -e 'alarm 60; exec @ARGV' "$ISH" -f "$ROOT" < /dev/null 2>&1 || true)
echo "$out" | grep -E '^(OK|FAIL)' | sed 's/^/  /'
for k in holder memfd group kept descendants wait waitm waitj; do
    echo "$out" | grep -q "^OK $k" || { echo "  FAIL    | $k"; fail=1; }
done
[ $fail -eq 0 ] || { echo "FAIL"; exit 1; }
echo "PASS: the GPU holder, its children and the big memfd were left out; the rest came back"
