#!/bin/sh
# checkpoint_tmux_attached.sh -- a tmux client ATTACHED across suspend and resume.
#     tests/manual/checkpoint_tmux_attached.sh [root]     (needs tmux and watch)
#
# Reported from the iPad: after a resume, attach and detach from one terminal
# worked; attaching from another left both panes unresponsive, and detaching
# wedged the shell that tried. A restored pipe came back BLOCKING, and tmux's
# event loop (libevent) drains its non-blocking signal pipe until EAGAIN -- so
# the first signal a restored tmux process got left it in read() for good.
# Detaching is what sends one here: the attached client is a pane's process in
# an outer tmux, so its exit is SIGCHLD to that server.
#
# The attached client lives in a pane of a second, OUTER tmux, which gives it a
# terminal the script can type into. Asserted after the resume: the client
# still shows the panes updating, a key typed into it reaches ktop, a detach
# typed into it detaches it, and both servers keep answering.
set -e
REPO=$(cd "$(dirname "$0")/../.." && pwd)
ISH=${ISH:-$REPO/build/ish}
ROOT=${1:-$REPO/build/archarm64-test}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/aok-tmux-attached.XXXXXX")
IMG=$WORK/img
trap 'kill -9 $(cat "$WORK/pid" 2>/dev/null) 2>/dev/null || true; rm -rf "$WORK"' EXIT

have=0
for p in "$ROOT/data/usr/bin/tmux" "$ROOT/data/usr/sbin/tmux" "$ROOT/usr/bin/tmux"; do
    [ -e "$p" ] && have=1
done
[ $have -eq 1 ] || { echo "checkpoint_tmux_attached: SKIP (no tmux in $ROOT)"; exit 0; }

cat > "$WORK/launch.sh" <<'L'
export SHELL=/AOK/native/zsh
tmux -L inner new-session -d -s t -x 80 -y 24 >/dev/null 2>&1
tmux -L inner split-window -t t >/dev/null 2>&1      # ktop's pane is active
sleep 2
tmux -L inner send-keys -t t:0.0 'watch -n 1 date' Enter
tmux -L inner send-keys -t t:0.1 '/AOK/native/ktop' Enter
# env -u TMUX: tmux builds that refuse any nesting while $TMUX is set (Devuan's
# 3.5a does) would otherwise quit the attach at once.
tmux -L outer new-session -d -s o -x 100 -y 30 'env -u TMUX tmux -L inner attach -t t' >/dev/null 2>&1
sleep 5
echo "BEFORE clients=$(tmux -L inner list-clients 2>/dev/null | wc -l)"
echo suspend > /proc/ish/checkpoint
sleep 4
clock() { tmux -L outer capture-pane -p -t o | head -1 | grep -o '[0-9][0-9]:[0-9][0-9]:[0-9][0-9]' | head -1; }
a=$(clock); sleep 3; b=$(clock)
echo "CLOCK $a $b"
tmux -L outer send-keys -t o q
sleep 3
echo "KTOP-LEFT $(ps -eo comm | grep -c '^ktop$')"
tmux -L outer send-keys -t o C-b d
sleep 3
( tmux -L inner list-clients > /tmp/lc.out 2>&1; tmux -L inner ls > /tmp/ls.out 2>&1;
  echo done > /tmp/inner.done ) &
( tmux -L outer ls > /tmp/lo.out 2>&1; echo done > /tmp/outer.done ) &
sleep 4
[ -e /tmp/inner.done ] && echo "INNER answered clients=$(grep -c . /tmp/lc.out)" || echo "INNER wedged"
[ -e /tmp/outer.done ] && echo "OUTER answered" || echo "OUTER wedged"
echo DONE
kill -9 1
L

ISH_CLI_PTY=1 ISH_GUEST_CHECKPOINT=1 ISH_SESSION="$IMG" ISH_REAL_MNT="$WORK" \
    "$ISH" -f "$ROOT" /bin/sh /realmnt/launch.sh > "$WORK/out1" 2>&1 || true
[ -s "$IMG" ] || { echo "FAIL: suspend wrote no image"; tail -5 "$WORK/out1"; exit 1; }
( ISH_CLI_PTY=1 ISH_GUEST_CHECKPOINT=1 ISH_SESSION="$IMG" ISH_REAL_MNT="$WORK" \
    "$ISH" -f "$ROOT" /bin/sh -c x > "$WORK/out2" 2>&1 & echo $! > "$WORK/pid" )
n=0; while ! grep -q '^DONE' "$WORK/out2" 2>/dev/null && [ $n -lt 90 ]; do sleep 1; n=$((n+1)); done
for o in out1 out2; do tr -d '\r' < "$WORK/$o" > "$WORK/$o.txt" && mv "$WORK/$o.txt" "$WORK/$o"; done
grep -aE '^(BEFORE|CLOCK|KTOP|INNER|OUTER)' "$WORK/out1" "$WORK/out2" -h | sed 's/^/  /'
grep -q '^DONE' "$WORK/out2" || { echo "FAIL: the resumed guest never finished -- something wedged"; exit 1; }

fail=0
f() { echo "  FAIL    | $*"; fail=1; }
grep -qx 'BEFORE clients=1' "$WORK/out1" || f "no client was attached when the image was saved"
set -- $(sed -n 's/^CLOCK //p' "$WORK/out2")
[ -n "$1" ] && [ -n "$2" ] && [ "$1" != "$2" ] || f "watch did not update through the attached client ($1 -> $2)"
grep -qx 'KTOP-LEFT 0' "$WORK/out2" || f "q typed into the attached client did not reach ktop"
grep -qx 'INNER answered clients=0' "$WORK/out2" || f "the inner server wedged, or the client did not detach"
grep -qx 'OUTER answered' "$WORK/out2" || f "the outer server wedged when its pane's process left"
[ $fail -eq 0 ] || { echo "FAIL"; exit 1; }
echo "PASS: an attached tmux client came back working, and detached cleanly"
