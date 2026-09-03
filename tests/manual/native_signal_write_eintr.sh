#!/bin/sh
# native_signal_write_eintr.sh -- when a signal may interrupt a transfer, and
# when it may not. Run inside the guest (device or CLI root):
#
#   /AOK/tests/native_signal_write_eintr.sh        # needs /AOK/native/bash
#
# WHY THIS EXISTS. Native bash intermittently printed
#
#   /tmp/g.sh: line 4: echo: write error: Interrupted system call
#
# about one run in eight (build 554). Three bytes going into an empty 64 KiB
# pipe cannot block, so nothing was entitled to interrupt them; two separate
# kernel defects conspired.
#
#   1. fs/real.c asked realfs_wait_writable() BEFORE attempting the write, and
#      that wait reports EINTR the moment a guest signal is pending. A transfer
#      that would have completed instantly was failed for a queued SIGCHLD.
#      Linux only lets a signal cut short an operation that genuinely blocks.
#
#   2. A native program's SA_RESTART was unreachable. The shim cannot give the
#      kernel a host function pointer, so it blocks the signal and leaves a
#      SIG_DFL placeholder in sighand->action -- flags and all. The kernel then
#      had nothing true to read, and signal_should_restart_syscall() answered
#      "do not restart" for every native program, forever.
#
# The two halves also disagreed with each other by construction: the wait side
# uses task_wake_blocked() (which subtracts the shim's held set, so a held
# signal counts as pending) while the restart side used the raw blocked set
# (where a held signal is invisible). Pending enough to interrupt, blocked
# enough not to restart.
#
# Every case below was verified against real bash on x86_64 Linux 6.12 before
# being written down, so these are Linux's answers, not this kernel's.

B=/AOK/native/bash
pass=0; fail=0

[ -x "$B" ] || { echo "native_signal_write_eintr: no $B -- build with -Dnative_bash=enabled"; exit 0; }

ck() {
    name=$1; want=$2; got=$3
    if [ "$got" = "$want" ]; then
        pass=$((pass+1)); printf '  ok    %s\n' "$name"
    else
        fail=$((fail+1)); printf '  FAIL  %-32s want [%s] got [%s]\n' "$name" "$want" "$got"
    fi
}

tmp=${TMPDIR:-/tmp}/nswe.$$
mkdir -p "$tmp" || exit 1
trap 'rm -rf "$tmp"' EXIT

echo "== a pending signal must not fail a write that cannot block =="
# The reported bug, made deterministic. `kill` queues the signal; bash's echo
# then flushes inside a host stdio callback, where signal delivery is
# deliberately deferred (nlibc_stdio_defer_fatal) -- so the signal is still
# sitting pending across the whole write, every single time.
#
# Piped, not to a terminal: the failing write was a real host pipe (fs/pipe.c
# -> realfs_write). A tty goes through fs/tty.c and never had the bug.
got=$("$B" -c 'trap : USR1; kill -USR1 $$; echo x; echo y' 2>&1 | cat | tr '\n' ' ')
ck pending-usr1-nonblocking "x y " "$got"

got=$("$B" -c 'x=$(echo hi); kill -CHLD $$; echo $x' 2>&1 | cat | tr '\n' ' ')
ck pending-chld-nonblocking "hi " "$got"

# The original reproducer, whole: 40 command substitutions, each forking a
# subshell whose exit queues a SIGCHLD into the next iteration's write.
cat > "$tmp/loop.sh" <<'SCRIPT'
i=0
while [ $i -lt 40 ]; do
  x=$(echo hi)
  i=$((i+1))
done
grep VmSize /proc/self/status > /dev/null
echo done
SCRIPT
got=$("$B" "$tmp/loop.sh" 2>&1 | cat | tr '\n' ' ')
ck cmdsubst-loop-40 "done " "$got"

echo "== SA_RESTART must restart a write that DOES block =="
# 300 KiB into a pipe whose reader sleeps first, so the write really is stuck
# with the pipe full while children exit underneath it. bash installs
# sigchld_handler with SA_RESTART (sig.c, set_signal_handler), so Linux hides
# the interruption completely and every byte arrives.
cat > "$tmp/chld.sh" <<'SCRIPT'
( sleep 1 ) &
( sleep 2 ) &
( sleep 3 ) &
i=0
while [ $i -lt 300 ]; do
  printf '%01000d' 0
  i=$((i+1))
done
SCRIPT
got=$("$B" "$tmp/chld.sh" 2>"$tmp/chld.err" | ( sleep 4; cat ) | wc -c | tr -d ' ')
ck sa-restart-blocking-write 300000 "$got"
ck sa-restart-no-diagnostic "" "$(cat "$tmp/chld.err")"

echo "== a handler WITHOUT SA_RESTART must still interrupt =="
# The other half, and the reason this is not fixed by restarting everything: a
# bash trap on USR1 carries no SA_RESTART, so a blocked read must come back and
# let the trap run. If it were restarted transparently the read would never
# return and this hangs -- which is what the timeout is here to turn into a
# failure rather than a wedged suite.
mkfifo "$tmp/f" 2>/dev/null || true
if [ -p "$tmp/f" ]; then
    got=$(timeout 15 "$B" -c '
        trap "echo trapped; exit 7" USR1
        me=$$
        ( sleep 1; kill -USR1 $me ) &
        read line < '"$tmp"'/f
        echo "read-returned-instead"' 2>&1 | tail -1)
    ck no-restart-breaks-blocked-read trapped "$got"
else
    echo "  skip  no-restart-breaks-blocked-read (no fifo support here)"
fi

echo
echo "  passed=$pass failed=$fail"
[ "$fail" -eq 0 ]
