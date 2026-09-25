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
# terminal the script can type into. Asserted after the resume: the client is
# still attached and still shows the panes updating, a key typed into it
# reaches ktop, a detach typed into it detaches it, and both servers keep
# answering.
#
# The inner server holds the client's terminal -- the slave of a pty whose
# master is the OUTER server's -- and it is the older server, so the image
# builds it first. That slave came back on /dev/null while its master did not
# exist yet: the server read EOF from it and dropped the client at once.
#
# A second scenario attaches the client on a SESSION terminal instead -- the
# CLI's pty session, the app's window -- which the image does not own. The
# server holds that terminal too (a client hands it over), and the server is
# older than the session, so the image names the server's descriptor before
# the terminal exists at all. It used to come back as the server's own
# standard streams: the server drew every pane to the wrong place. Asserted:
# after the resume the server's descriptor is the client's own terminal, with
# the flags it had.
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
echo "AFTER clients=$(tmux -L inner list-clients 2>/dev/null | wc -l)"
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
grep -aE '^(BEFORE|AFTER|CLOCK|KTOP|INNER|OUTER)' "$WORK/out1" "$WORK/out2" -h | sed 's/^/  /'
grep -q '^DONE' "$WORK/out2" || { echo "FAIL: the resumed guest never finished -- something wedged"; exit 1; }

fail=0
f() { echo "  FAIL    | $*"; fail=1; }
grep -qx 'BEFORE clients=1' "$WORK/out1" || f "no client was attached when the image was saved"
# Before the checks below, which a client that died at the resume would pass:
# it is not a client to list, and its exit empties the outer server as a
# detach does.
grep -qx 'AFTER clients=1' "$WORK/out2" || f "the attached client did not survive the resume"
set -- $(sed -n 's/^CLOCK //p' "$WORK/out2")
[ -n "$1" ] && [ -n "$2" ] && [ "$1" != "$2" ] || f "watch did not update through the attached client ($1 -> $2)"
grep -qx 'KTOP-LEFT 0' "$WORK/out2" || f "q typed into the attached client did not reach ktop"
grep -qx 'INNER answered clients=0' "$WORK/out2" || f "the inner server wedged, or the client did not detach"
grep -qx 'OUTER answered' "$WORK/out2" || f "the outer server wedged when its pane's process left"

# ---- attached on a session terminal ---------------------------------------
cat > "$WORK/launch2.sh" <<'L'
export SHELL=/AOK/native/zsh
export TERM=xterm-256color
tmux -L w new-session -d -s t -x 80 -y 24 >/dev/null 2>&1
tmux -L w send-keys -t t 'watch -n 1 date' Enter
sleep 2
report() {
  srv=$(tmux -L w display -p '#{pid}')
  cl=$(tmux -L w list-clients -F '#{client_pid}' | head -1)
  mine=$(readlink /proc/$cl/fd/0)
  for fd in /proc/$srv/fd/*; do
    case $(readlink $fd) in /dev/pts/*)
      n=${fd##*/}
      same=0; [ "$(readlink $fd)" = "$mine" ] && same=1
      echo "$1 SERVER-TTY fd=$n same-as-client=$same $(grep '^flags' /proc/$srv/fdinfo/$n | tr -s ' \t' ' ')";;
    esac
  done
}
(
  sleep 4
  report BEFORE
  echo suspend > /proc/ish/checkpoint
  sleep 4
  report AFTER
  kill -CHLD "$(tmux -L w display -p '#{pid}')"
  sleep 1
  echo "AFTER answered=$(tmux -L w ls 2>/dev/null | grep -c '^t:')"
  echo WDONE
  sleep 1
  kill -9 1
) &
exec env -u TMUX tmux -L w attach -t t
L
rm -f "$IMG"
ISH_CLI_PTY=1 ISH_GUEST_CHECKPOINT=1 ISH_SESSION="$IMG" ISH_REAL_MNT="$WORK" \
    "$ISH" -f "$ROOT" /bin/sh /realmnt/launch2.sh < /dev/null > "$WORK/w1" 2>&1 &
wp=$!
n=0; while kill -0 $wp 2>/dev/null && [ $n -lt 60 ]; do sleep 1; n=$((n+1)); done
kill -9 $wp 2>/dev/null || true
if [ ! -s "$IMG" ]; then
    f "the session-terminal scenario wrote no image"
else
    ( ISH_CLI_PTY=1 ISH_GUEST_CHECKPOINT=1 ISH_SESSION="$IMG" ISH_REAL_MNT="$WORK" \
        "$ISH" -f "$ROOT" /bin/sh -c x < /dev/null > "$WORK/w2" 2>&1 & echo $! > "$WORK/pid" )
    n=0; while ! grep -aq 'WDONE' "$WORK/w2" 2>/dev/null && [ $n -lt 60 ]; do sleep 1; n=$((n+1)); done
    kill -9 "$(cat "$WORK/pid")" 2>/dev/null || true
    # The client's display is on the same terminal as these lines: keep only them.
    for o in w1 w2; do
        tr -d '\r' < "$WORK/$o" | grep -ao '\(BEFORE\|AFTER\) [A-Za-z-]*[ =][^[:cntrl:]]*' > "$WORK/$o.txt" || true
    done
    sed 's/^/  window  | /' "$WORK/w1.txt" "$WORK/w2.txt"
    before=$(grep -m1 '^BEFORE SERVER-TTY' "$WORK/w1.txt" | sed 's/^BEFORE //')
    after=$(grep -m1 '^AFTER SERVER-TTY' "$WORK/w2.txt" | sed 's/^AFTER //')
    [ -n "$before" ] || f "no client was attached on the session terminal at the save"
    case $before in *same-as-client=1*) ;; *) f "before the save the server's terminal was not the client's ($before)";; esac
    [ -n "$after" ] || f "after the resume the server holds no descriptor on the client's terminal"
    [ "$after" = "$before" ] || f "the server's terminal descriptor changed across the resume: [$before] -> [$after]"
    grep -qx 'AFTER answered=1' "$WORK/w2.txt" || f "the server stopped answering after a signal"
fi

[ $fail -eq 0 ] || { echo "FAIL"; exit 1; }
echo "PASS: an attached tmux client came back working and detached cleanly, and a server's hold on a session terminal came back on it"
