#!/bin/sh
# checkpoint_failed_restore.sh -- a restore that fails part-way leaves nothing.
#
# The app boots in the same process when a resume fails, and that boot used to
# inherit the failed restore's debris: tasks that were built but never started,
# still holding what was rebuilt for them -- a listening socket bound on the
# host among it -- the image's tmpfses mounted over /run with the old session's
# pid files in them, and pid 1's descriptors. On device: no working terminal and
# no sshd until the app was restarted.
#
# So: a session with a tmpfs /run holding a stale pid file and a daemon
# listening on a port; a restore made to fail at the LAST task, after the
# daemon's has been rebuilt; and the command-line boot that follows
# (ISH_RESTORE_FALLBACK) checks that the image's /run, the pid file, the ghost
# daemon and its port are all gone.
#
#     tests/manual/checkpoint_failed_restore.sh [root]     (needs gcc in root)
set -e
REPO=$(cd "$(dirname "$0")/../.." && pwd)
ISH=${ISH:-$REPO/build/ish}
ROOT=${1:-$REPO/build/devuan-amd64-test}
PORT=18222
WORK=$(mktemp -d "${TMPDIR:-/tmp}/aok-failrest.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
cp "$REPO/tests/manual/checkpoint_failed_restore.c" "$WORK/"
ISH_REAL_MNT=$WORK "$ISH" -f "$ROOT" /bin/sh -c \
    'gcc -O1 -o /realmnt/lsn /realmnt/checkpoint_failed_restore.c' \
    || { echo "FAIL: could not build the helper"; exit 1; }

SAVE='mount -t tmpfs tmpfs /run
echo 4242 > /run/stale-daemon.pid
/realmnt/lsn listen '$PORT' & L=$!
sleep 40 & S=$!
echo "PIDS $L $S"
wait'
ISH_REAL_MNT=$WORK ISH_CHECKPOINT_AFTER="3:$WORK/img" "$ISH" -f "$ROOT" /bin/sh -c "$SAVE" \
    > "$WORK/save.out" 2>&1 &
saver=$!
n=0; while [ ! -s "$WORK/img" ] && [ $n -lt 100 ]; do sleep 0.1; n=$((n+1)); done
kill -9 $saver 2>/dev/null || true; wait $saver 2>/dev/null || true
[ -s "$WORK/img" ] || { echo "FAIL: no image written"; exit 1; }
set -- $(grep '^PIDS' "$WORK/save.out")
LSN=$2; SLEEPER=$3
[ -n "$SLEEPER" ] || { echo "FAIL: the session did not say its pids"; exit 1; }
LAST=$LSN; [ "$SLEEPER" -gt "$LAST" ] && LAST=$SLEEPER
[ "$LSN" -lt "$LAST" ] || { echo "FAIL: the daemon has to be built before the failure (pids $LSN $SLEEPER)"; exit 1; }

CHECK='echo "RUNMOUNT=$(grep -c " /run " /proc/mounts)"
echo "PIDFILE=$(test -e /run/stale-daemon.pid && echo present || echo gone)"
echo "GHOST=$(cat /proc/'$LSN'/comm 2>/dev/null || echo none)"
/realmnt/lsn probe '$PORT
out=$(ISH_REAL_MNT=$WORK ISH_RESTORE="$WORK/img" ISH_RESTORE_FALLBACK=1 \
      ISH_CHECKPOINT_TEST_FAIL_PID=$LAST "$ISH" -f "$ROOT" /bin/sh -c "$CHECK" 2>&1 || true)
echo "$out" | sed 's/^/  boot    | /'

fail=0
expect() {
    if echo "$out" | grep -qxF "$1"; then echo "  ok      | $1  ($2)"
    else echo "  FAIL    | want $1  ($2)"; fail=1; fi
}
echo "$out" | grep -q "test: failing the restore at pid $LAST" \
    || { echo "  FAIL    | the restore was not made to fail -- nothing was tested"; fail=1; }
expect 'RUNMOUNT=0'  "the image's tmpfs is not over /run"
expect 'PIDFILE=gone' "no stale pid file from the old session"
expect 'GHOST=none'  "no ghost of the daemon in the pid table"
expect 'BIND=ok'     "its port is free for the new boot"
[ $fail -eq 0 ] || { echo "FAIL"; exit 1; }
echo "PASS: the failed restore was undone before the boot"
