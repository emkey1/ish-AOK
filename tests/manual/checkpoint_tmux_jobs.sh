#!/bin/sh
# checkpoint_tmux_jobs.sh -- tmux panes whose shells are waiting on a job.
#     tests/manual/checkpoint_tmux_jobs.sh [root]     (needs tmux and watch)
#
# The shape reported from the iPad: a tmux session, native zsh in each pane,
# `watch` running in one and `ktop` in the other. After a resume the watch pane
# was at a prompt and ktop ignored `q`.
#
# A pane's pty is one the IMAGE owns -- tmux holds its master -- and the restore
# put back none of that terminal's state: not its foreground group, not the
# raw mode the program had set, not its size, and not the controlling terminal
# of anything in the session but the process that reopened it. A restored
# native shell waits for its foreground job before it starts
# (kernel/native.c), and it finds that job by the terminal's foreground group;
# with the shell's own group in front it waited for nothing, prompted, and
# took the pane from the job. ktop, in the background and in cooked mode,
# never saw its `q`.
#
# Asserted after the resume: each pane's terminal has the JOB in front, the
# job's processes have the pane as their controlling terminal, the waiting
# shells use no CPU and are not counted as running, `q` quits ktop and hands
# the pane back to a working shell, and ^C does the same for watch.
#
# And the server survives a signal. Its event loop (libevent) drains its
# signal pipe until EAGAIN; a restored pipe came back BLOCKING, so the first
# signal the server got after a resume left it in read() for good -- every
# pane deaf, every later tmux command hung. Reported from the iPad as "attach
# from the window, neither pane responds, and detaching wedges the shell".
set -e
REPO=$(cd "$(dirname "$0")/../.." && pwd)
ISH=${ISH:-$REPO/build/ish}
ROOT=${1:-$REPO/build/archarm64-test}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/aok-tmux-jobs.XXXXXX")
IMG=$WORK/img
# `|| true`: under set -e a failing kill (the guest already gone) ends the trap
# with status 1 -- the script "fails" after printing PASS, and the rm never runs.
trap 'kill -9 $(cat "$WORK/pid" 2>/dev/null) 2>/dev/null || true; rm -rf "$WORK"' EXIT

have=0
for p in "$ROOT/data/usr/bin/tmux" "$ROOT/data/usr/sbin/tmux" "$ROOT/usr/bin/tmux"; do
    [ -e "$p" ] && have=1
done
[ $have -eq 1 ] || { echo "checkpoint_tmux_jobs: SKIP (no tmux in $ROOT)"; exit 0; }

cat > "$WORK/launch.sh" <<'L'
export SHELL=/AOK/native/zsh
tmux new-session -d -s t -x 80 -y 24 >/dev/null 2>&1
tmux split-window -t t >/dev/null 2>&1
sleep 2
tmux send-keys -t t:0.0 'watch -n 1 date' Enter
tmux send-keys -t t:0.1 '/AOK/native/ktop' Enter
sleep 4
# Everything on stdout: the image is saved without the host mount.
report() {
  for p in 0 1; do
    sh=$(tmux display -p -t t:0.$p '#{pane_pid}')
    echo "$1 pane$p shell=$sh tpgid=$(awk '{print $8}' /proc/$sh/stat)"
  done
  ps -eo pid,pgid,tty,stat,comm | awk -v w="$1" '$5 ~ /^(watch|ktop|zsh|-zsh)$/ {print w, "ps", $0}'
}
report BEFORE
echo suspend > /proc/ish/checkpoint
sleep 4
report AFTER
for p in 0 1; do
  sh=$(tmux display -p -t t:0.$p '#{pane_pid}')
  a=$(awk '{print $14+$15}' /proc/$sh/stat); sleep 2; b=$(awk '{print $14+$15}' /proc/$sh/stat)
  echo "CPU pane$p $((b-a))"
done
tmux send-keys -t t:0.1 q
sleep 3
echo "KTOP-LEFT $(ps -eo comm | grep -c '^ktop$')"
tmux send-keys -t t:0.1 'echo PANE1-$((7*6))' Enter
tmux send-keys -t t:0.0 C-c
sleep 2
tmux send-keys -t t:0.0 'echo PANE0-$((6*7))' Enter
sleep 2
echo "PANE1-ECHO $(tmux capture-pane -p -t t:0.1 | grep -c '^PANE1-42')"
echo "PANE0-ECHO $(tmux capture-pane -p -t t:0.0 | grep -c '^PANE0-42')"
for p in 0 1; do
  sh=$(tmux display -p -t t:0.$p '#{pane_pid}')
  echo "FINAL pane$p shell=$sh tpgid=$(awk '{print $8}' /proc/$sh/stat)"
done
# SIGCHLD: the server's handler only reaps, so it is harmless -- except to a
# server whose signal pipe blocks.
kill -CHLD "$(tmux display -p '#{pid}')"
sleep 1
( tmux ls > /tmp/ls.out 2>&1; echo done > /tmp/ls.done ) &
sleep 4
if [ -e /tmp/ls.done ]; then echo "SERVER answered"; else echo "SERVER wedged"; fi
echo DONE
kill -9 1
L

ISH_CLI_PTY=1 ISH_GUEST_CHECKPOINT=1 ISH_SESSION="$IMG" ISH_REAL_MNT="$WORK" \
    "$ISH" -f "$ROOT" /bin/sh /realmnt/launch.sh > "$WORK/out1" 2>&1 || true
[ -s "$IMG" ] || { echo "FAIL: suspend wrote no image"; tail -5 "$WORK/out1"; exit 1; }
( ISH_CLI_PTY=1 ISH_GUEST_CHECKPOINT=1 ISH_SESSION="$IMG" ISH_REAL_MNT="$WORK" \
    "$ISH" -f "$ROOT" /bin/sh -c x > "$WORK/out2" 2>&1 & echo $! > "$WORK/pid" )
n=0; while ! grep -q '^DONE' "$WORK/out2" 2>/dev/null && [ $n -lt 90 ]; do sleep 1; n=$((n+1)); done
# The CLI's terminal ends lines with \r\n.
for o in out1 out2; do tr -d '\r' < "$WORK/$o" > "$WORK/$o.txt" && mv "$WORK/$o.txt" "$WORK/$o"; done
grep -aE '^(BEFORE|AFTER)' "$WORK/out1" "$WORK/out2" -h | sed 's/^/  /'
grep -aE '^(CPU|KTOP|PANE|FINAL|SERVER)' "$WORK/out2" | sed 's/^/  /'
grep -q '^DONE' "$WORK/out2" || { echo "FAIL: the resumed guest never finished"; exit 1; }

fail=0
f() { echo "  FAIL    | $*"; fail=1; }
# The pgid of the pane's job, from the AFTER ps lines: the non-login zsh (the
# job's own process) and the program under it share it.
for p in 0 1; do
    sh=$(sed -n "s/^AFTER pane$p shell=\([0-9]*\) .*/\1/p" "$WORK/out2")
    tp=$(sed -n "s/^AFTER pane$p shell=[0-9]* tpgid=\([0-9]*\)/\1/p" "$WORK/out2")
    [ -n "$sh" ] && [ -n "$tp" ] || { f "pane$p: no report"; continue; }
    [ "$tp" != "$sh" ] || f "pane$p: the shell ($sh) has the terminal, not its job"
done
for prog in watch ktop; do
    line=$(grep -a "^AFTER ps .* $prog\$" "$WORK/out2" | head -1)
    [ -n "$line" ] || { f "$prog did not come back"; continue; }
    pg=$(echo "$line" | awk '{print $4}'); tty=$(echo "$line" | awk '{print $5}')
    st=$(echo "$line" | awk '{print $6}')
    [ "$tty" != "?" ] || f "$prog has no controlling terminal"
    case $st in *+*) ;; *) f "$prog's group $pg is not in its terminal's foreground ($st)";; esac
done
grep -a '^AFTER ps .* -zsh$' "$WORK/out2" | while read -r _ _ pid pg tty st rest; do
    case $st in R*) echo "  FAIL    | waiting shell $pid reads as running ($st)"; echo x > "$WORK/rfail";; esac
done
[ -e "$WORK/rfail" ] && fail=1
for p in 0 1; do
    c=$(sed -n "s/^CPU pane$p //p" "$WORK/out2")
    [ "${c:-99}" -le 1 ] || f "pane$p's waiting shell used $c ticks in 2 s"
done
grep -qx 'SERVER answered' "$WORK/out2" || f "the tmux server stopped answering after a signal"
grep -qx 'KTOP-LEFT 0' "$WORK/out2" || f "q did not quit ktop"
grep -qx 'PANE1-ECHO 1' "$WORK/out2" || f "pane1's shell did not come back after ktop"
grep -qx 'PANE0-ECHO 1' "$WORK/out2" || f "pane0's shell did not come back after watch"
for p in 0 1; do
    sh=$(sed -n "s/^FINAL pane$p shell=\([0-9]*\) .*/\1/p" "$WORK/out2")
    tp=$(sed -n "s/^FINAL pane$p shell=[0-9]* tpgid=\([0-9]*\)/\1/p" "$WORK/out2")
    [ -n "$sh" ] && [ "$tp" = "$sh" ] || f "pane$p: the shell did not get its terminal back ($tp)"
done
[ $fail -eq 0 ] || { echo "FAIL"; exit 1; }
echo "PASS: panes came back with their jobs in front, and their shells after"
