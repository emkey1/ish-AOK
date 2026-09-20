#!/bin/sh
# checkpoint_tmux.sh -- a tmux session survives suspend and resume.
#
# tmux is the hard case for the checkpoint, and it is hard in two independent
# ways that both had to be fixed before any of this passed:
#
#   * its server listens on an AF_UNIX socket. Unix sockets used to come back
#     hung up on the reasoning that it cost "that socket rather than the
#     session" -- but for tmux the socket IS the session, and the server exited
#     within a dozen syscalls of resuming.
#   * its panes run on a pty whose MASTER tmux itself holds. The restore used
#     to treat every pts as a terminal for the UI to adopt, so a pane came back
#     on a fresh window of its own while tmux's master pointed at nothing.
#
# The pane here COUNTS, and the check is that the count ADVANCED across the
# restore. A surviving server and a surviving scrollback would both pass a
# weaker test while the pane sat dead on a pty going nowhere.
set -e
REPO=$(cd "$(dirname "$0")/../.." && pwd)
ISH=${ISH:-$REPO/build/ish}
ROOT=${1:-$REPO/build/devuan-arm64-test}
IMG=${TMPDIR:-/tmp}/aok-ckpt-tmux-$$.img
WORK=${TMPDIR:-/tmp}/aok-ckpt-tmux-work-$$
rm -rf "$WORK"; mkdir -p "$WORK"
trap 'rm -rf "$WORK" "$IMG" "$IMG.log"' EXIT

# A fakefs root keeps the guest's files under data/; a plain directory root
# does not. Look in both rather than guess.
have_tmux=no
for p in "$ROOT/data/usr/bin/tmux" "$ROOT/data/bin/tmux" \
         "$ROOT/usr/bin/tmux" "$ROOT/bin/tmux"; do
    [ -e "$p" ] && have_tmux=yes && break
done
if [ "$have_tmux" = no ]; then
    echo "checkpoint_tmux: SKIP (no tmux in $ROOT)"
    exit 0
fi

# The pane program, in a file: quoting it through two shells and a C string is
# how the first version of this silently tested nothing.
cat > "$WORK/pane.sh" <<'PANE'
i=0
while true; do
    i=$((i + 1))
    echo "tick-$i"
    sleep 1
done
PANE

cat > "$WORK/launch1.sh" <<'L1'
tmux new-session -d -s t "sh /realmnt/pane.sh" >/dev/null 2>&1
sleep 3
# Everything reported on STDOUT, nothing through /realmnt: the image is saved
# WITHOUT the host mount, so a resumed guest writing there writes nowhere.
echo "before: $(tmux capture-pane -p -t t 2>/dev/null | grep -c tick-)"
echo suspend > /proc/ish/checkpoint
sleep 5
echo "ls: $(tmux ls 2>&1)"
echo "last: $(tmux capture-pane -p -t t 2>/dev/null | grep tick- | tail -1)"
kill -9 1
L1

rm -f "$IMG" "$IMG.log"
ISH_CLI_PTY=1 ISH_GUEST_CHECKPOINT=1 ISH_SESSION="$IMG" ISH_REAL_MNT="$WORK" \
    "$ISH" -f "$ROOT" /bin/sh /realmnt/launch1.sh >"$WORK/out1" 2>&1 || true
[ -s "$IMG" ] || { echo "FAIL: suspend wrote no image"; exit 1; }
case $(cat "$WORK/out1" 2>/dev/null) in
    *"ls: "*) echo "FAIL: the suspending guest kept running"; exit 1;;
esac
before=$(sed -n 's/^before: //p' "$WORK/out1" | tr -dc '0-9')
[ -n "$before" ] || before=0
echo "  suspend | image $(wc -c < "$IMG") bytes, pane had $before lines"

( ISH_CLI_PTY=1 ISH_GUEST_CHECKPOINT=1 ISH_SESSION="$IMG" ISH_REAL_MNT="$WORK" \
    "$ISH" -f "$ROOT" /bin/sh -c x >"$WORK/out2" 2>&1 & echo $! > "$WORK/pid" )
i=0
while [ $i -lt 60 ]; do
    # Wait for the LAST line the resumed script writes, not the first: the
    # guest is killed as soon as this loop ends, and breaking on RESUMED killed
    # it before `tmux ls` had written its answer.
    grep -q '^last: ' "$WORK/out2" 2>/dev/null && break
    i=$((i + 1)); sleep 1
done
kill -9 "$(cat "$WORK/pid")" 2>/dev/null || true

grep -q '^ls: ' "$WORK/out2" || { echo "FAIL: the image did not resume"; grep -av syscall "$WORK/out2" | tail -5 | sed 's/^/  /'; exit 1; }
sed -n 's/^ls: /  resume  | /p;s/^last: /  resume  | last /p' "$WORK/out2"

case $(grep '^ls: ' "$WORK/out2") in
    *"t: 1 windows"*) ;;
    *) echo "FAIL: the tmux session did not survive"; grep '^ls: ' "$WORK/out2"; exit 1;;
esac

last=$(sed -n 's/^last: //p' "$WORK/out2" | tr -dc '0-9')
[ -n "$last" ] || { echo "FAIL: the pane produced no output after the restore"; exit 1; }
# ADVANCED, not merely present: a dead pane on a dead pty would still have its
# scrollback.
[ "$last" -gt "$before" ] || {
    echo "FAIL: the pane stopped counting across the restore (before $before, after $last)"
    exit 1
}
echo "  resume  | pane advanced $before -> $last, so its pty still carries data"
echo "checkpoint_tmux: PASS"
