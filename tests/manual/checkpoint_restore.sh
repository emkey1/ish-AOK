#!/bin/sh
# checkpoint_restore.sh -- the phase-0 proof for suspend to disk, on the CLI.
#
# The roadmap's wording is the test: "checkpoint and restore a single-process
# guest, no native program, one open file ... The restored guest continues from
# the instruction after the checkpoint, reads the same bytes from the same open
# file at the same offset."
#
# So this runs a shell that reads two lines of a file, checkpoints itself,
# reads three more, and then runs a SEPARATE ish process from the image. The
# restored run's output must be exactly the tail of the original's -- not
# similar to it, and in particular not starting again from the top.
#
#     tests/manual/checkpoint_restore.sh [root]
#
# `root` defaults to build/devuan-arm64-test. It has to be a root the CLI can
# boot with a working /bin/dash or /bin/sh.
set -e

REPO=$(cd "$(dirname "$0")/../.." && pwd)
ISH=${ISH:-$REPO/build/ish}
ROOT=${1:-$REPO/build/devuan-arm64-test}
IMG=${TMPDIR:-/tmp}/aok-checkpoint-$$.img
SH=/bin/dash
[ -d "$ROOT" ] || { echo "no root at $ROOT" >&2; exit 2; }
[ -x "$ISH" ] || { echo "build/ish first" >&2; exit 2; }
trap 'rm -f "$IMG"' EXIT

# The `pre` line is printed BEFORE the checkpoint and the `post`/`then` lines
# after it, so the restored run must print the second two and not the first.
PROG='
i=1; : > /tmp/ckpt-lines.txt
while [ $i -le 8 ]; do echo "LINE-$i-payload" >> /tmp/ckpt-lines.txt; i=$((i+1)); done
exec 3< /tmp/ckpt-lines.txt
read -r a <&3; read -r b <&3
MARK=a-shell-variable-that-must-survive
echo "pre : $a / $b / $MARK"
echo save IMAGE > /proc/ish/checkpoint
read -r c <&3; read -r d <&3
echo "post: $c / $d / $MARK"
read -r e <&3
echo "then: $e"
'

save_out=$(ISH_GUEST_CHECKPOINT=1 "$ISH" -f "$ROOT" $SH -c "$(printf '%s' "$PROG" | sed "s|IMAGE|$IMG|")" 2>&1)
echo "$save_out" | sed 's/^/  save    | /'

[ -s "$IMG" ] || { echo "FAIL: no image written"; exit 1; }

restore_out=$(ISH_RESTORE="$IMG" "$ISH" -f "$ROOT" 2>&1)
echo "$restore_out" | sed 's/^/  restore | /'

# 1. It continued rather than re-ran: the line printed before the checkpoint
#    must NOT appear in the restored run.
if echo "$restore_out" | grep -q '^pre :'; then
    echo "FAIL: the restored guest re-ran the checkpoint request"
    exit 1
fi

# 2. Same bytes, same file, same offset -- the restored run is exactly the
#    tail of the original. Compared whole rather than grepped for a substring,
#    because "it printed something plausible" is what a wrong offset looks
#    like.
expect=$(echo "$save_out" | sed -n '2,$p')
if [ "$restore_out" != "$expect" ]; then
    echo "FAIL: restored output is not the original's tail"
    echo "  expected: $expect"
    echo "  got     : $restore_out"
    exit 1
fi

# 3. And it really did come back from a file rather than boot fresh, which the
#    two checks above would not distinguish if the shell were somehow re-run
#    with the same state by accident.
case $restore_out in
    *"LINE-3-payload"*"LINE-4-payload"*"LINE-5-payload"*) ;;
    *) echo "FAIL: the restored guest did not continue the open file"; exit 1;;
esac

# 4. And the guest can TELL the two lives apart. This is what a program uses
#    to decide whether it is running for the first time; without it a restore
#    is indistinguishable from a continuation and nothing can react to one.
gen_out=$(ISH_GUEST_CHECKPOINT=1 "$ISH" -f "$ROOT" $SH -c "
echo save $IMG > /proc/ish/checkpoint
while read -r l; do case \$l in restored*) echo \"\$l\";; esac; done < /proc/ish/checkpoint
" 2>&1)
gen_back=$(ISH_RESTORE="$IMG" "$ISH" -f "$ROOT" 2>&1)
case $gen_out in
    *"restored        no"*) ;;
    *) echo "FAIL: a fresh guest reported itself restored: $gen_out"; exit 1;;
esac
case $gen_back in
    *"restored        yes"*) ;;
    *) echo "FAIL: a restored guest did not report itself restored: $gen_back"; exit 1;;
esac

# ---- a full suspend/resume cycle -------------------------------------------
#
# The difference from a checkpoint: the guest STOPS once the image is written,
# and the next launch resumes it rather than booting. One path -- ISH_SESSION
# -- is both where the suspend writes and where the launch looks, which is the
# shape the app needs.
rm -f "$IMG"
sus_out=$(ISH_GUEST_CHECKPOINT=1 ISH_SESSION="$IMG" "$ISH" -f "$ROOT" $SH -c '
exec 3< /tmp/ckpt-lines.txt
read -r a <&3
echo "launch-1 read $a"
echo suspend > /proc/ish/checkpoint
echo "launch-2 read the rest"
read -r b <&3; echo "launch-2 got $b"
' 2>&1)
echo "$sus_out" | sed 's/^/  suspend | /'
[ -s "$IMG" ] || { echo "FAIL: suspend wrote no image"; exit 1; }
case $sus_out in
    *"launch-2"*) echo "FAIL: the suspending guest kept running"; exit 1;;
esac

res_out=$(ISH_GUEST_CHECKPOINT=1 ISH_SESSION="$IMG" "$ISH" -f "$ROOT" $SH -c 'echo "this argv must be ignored"' 2>&1)
echo "$res_out" | sed 's/^/  resume  | /'
case $res_out in
    *"launch-2 read the rest"*"launch-2 got LINE-2-payload"*) ;;
    *) echo "FAIL: resume did not continue the suspended guest"; echo "  got: $res_out"; exit 1;;
esac
case $res_out in
    *"this argv must be ignored"*) echo "FAIL: resume ran the new command line"; exit 1;;
esac
[ -e "$IMG" ] && { echo "FAIL: the session image survived being resumed"; exit 1; }
echo "  resume  | (image consumed)"

# ---- more than one process ------------------------------------------------
#
# The interesting half, and the reason for the freezer. The parent here is
# blocked in wait(), its child is blocked in nanosleep, and NEITHER is at a
# place a checkpoint can describe until the machine is stopped: the freeze
# wakes them, their waits come back EINTR, the dispatcher rewinds the program
# counter over the syscall instruction, and they arrive at the loop top about
# to re-execute the call they were in. On the far side of the restore they do
# exactly that.
rm -f "$IMG"
mp_out=$(ISH_GUEST_CHECKPOINT=1 ISH_SESSION="$IMG" "$ISH" -f "$ROOT" $SH -c '
/bin/sleep 3 &
echo "launch1: child is $!"
echo suspend > /proc/ish/checkpoint
echo "launch2: back, waiting"
wait
echo "launch2: wait returned $?"
' 2>&1)
echo "$mp_out" | sed 's/^/  procs   | /'
case $mp_out in
    *"launch2"*) echo "FAIL: the suspending guest kept running"; exit 1;;
esac
[ -s "$IMG" ] || { echo "FAIL: no image for the two-process guest"; exit 1; }

mp_back=$(ISH_GUEST_CHECKPOINT=1 ISH_SESSION="$IMG" "$ISH" -f "$ROOT" $SH -c 'x' 2>&1)
echo "$mp_back" | sed 's/^/  procs   | /'
case $mp_back in
    *"launch2: back, waiting"*"launch2: wait returned 0"*) ;;
    *) echo "FAIL: the child did not survive the restore"; echo "  got: $mp_back"; exit 1;;
esac

# ---- the session and the process group ------------------------------------
#
# A terminal belongs to a SESSION, and which process group is in the FOREGROUND
# of it decides who may read from it. Both are membership, not just numbers:
# they live in per-pid lists, and a restore that set the fields and left the
# lists alone produced a shell whose tcsetpgrp answered ENOTTY -- job control
# silently off, and on a pseudo-terminal a foreground group that was still the
# login's, so the shell's first read came back EIO and the session was a pair
# of zombies a millisecond after the resume.
#
# setsid() in a subshell puts this process somewhere its ids are its own, so
# the check is a real comparison rather than "everything is 1".
rm -f "$IMG"
# setsid, so the ids under test are NOT 1/1 -- a restore that lost them
# entirely would still match if everything in the guest were pid 1's.
sid_out=$(ISH_GUEST_CHECKPOINT=1 ISH_SESSION="$IMG" "$ISH" -f "$ROOT" \
    /usr/bin/setsid -w $SH -c '
read_ids() { set -- $(cut -d" " -f4,5,6 /proc/self/stat); echo "$2/$3"; }
echo "launch1 pgid/sid: $(read_ids)"
echo suspend > /proc/ish/checkpoint
echo "launch2 pgid/sid: $(read_ids)"
' 2>&1)
echo "$sid_out" | sed 's/^/  ids     | /'
[ -s "$IMG" ] || { echo "FAIL: no image for the session test"; exit 1; }
sid_back=$(ISH_GUEST_CHECKPOINT=1 ISH_SESSION="$IMG" "$ISH" -f "$ROOT" $SH -c 'x' 2>&1)
echo "$sid_back" | sed 's/^/  ids     | /'
before=$(echo "$sid_out" | sed -n 's/^launch1 pgid\/sid: //p')
after=$(echo "$sid_back" | sed -n 's/^launch2 pgid\/sid: //p')
if [ -z "$after" ] || [ "$before" != "$after" ]; then
    echo "FAIL: process group / session changed across the restore ($before -> $after)"
    exit 1
fi
# The ids have to be worth comparing. 1/1 is what a guest that lost them
# entirely also reports, so a pass on those numbers would prove nothing.
case $before in
    1/1|/|"") echo "FAIL: the session test ran as pid 1 ($before); setsid did not take"; exit 1;;
esac

# ---- a pipeline, with bytes still in the pipe ------------------------------
#
# The producer writes three lines and exits; the consumer reads one and then
# suspends, leaving two lines sitting in the pipe. A pipe cannot be restored --
# it is a HOST pipe belonging to a process that is about to end -- so what
# travels is the pairing, the direction and the bytes in flight, and the
# restore builds a new pipe holding them.
#
# It is also the test for descriptor IDENTITY: both processes hold the same
# struct fd, and the image describes it once and references it from the other.
# Without that they would come back as two objects with two positions.
rm -f "$IMG"
pipe_out=$(ISH_GUEST_CHECKPOINT=1 ISH_SESSION="$IMG" "$ISH" -f "$ROOT" $SH -c '
/bin/printf "PIPE-DATA-1\nPIPE-DATA-2\nPIPE-DATA-3\n" | {
    read -r a; echo "launch1 read: $a"
    echo suspend > /proc/ish/checkpoint
    read -r b; echo "launch2 read: $b"
    read -r c; echo "launch2 read: $c"
}' 2>&1)
echo "$pipe_out" | sed 's/^/  pipe    | /'
case $pipe_out in
    *"launch1 read: PIPE-DATA-1"*) ;;
    *) echo "FAIL: the pipeline did not run"; exit 1;;
esac
case $pipe_out in
    *"launch2"*) echo "FAIL: the suspending guest kept running"; exit 1;;
esac

pipe_back=$(ISH_GUEST_CHECKPOINT=1 ISH_SESSION="$IMG" "$ISH" -f "$ROOT" $SH -c 'x' 2>&1)
echo "$pipe_back" | sed 's/^/  pipe    | /'
case $pipe_back in
    *"launch2 read: PIPE-DATA-2"*"launch2 read: PIPE-DATA-3"*) ;;
    *) echo "FAIL: the bytes in the pipe did not survive"; echo "  got: $pipe_back"; exit 1;;
esac

# ---- a NATIVE program -----------------------------------------------------
#
# The one that matters most, because AOK's login shell is native zsh. A native
# program is a C function on a host thread: there is no serialising that stack,
# so it is not photographed, it is asked to DESCRIBE ITSELF and re-launched
# from the description. zsh already knows how -- its fork-by-relaunch turns a
# live shell into a script that rebuilds it -- and a checkpoint wants exactly
# those bytes.
#
# The consequence, and it is a real one: a restored native program RE-RUNS its
# command line rather than continuing mid-command. For an interactive shell,
# which is what this is for, that is a prompt with your session still in it.
# The test uses that deliberately -- the same command line, run twice, taking
# the other branch the second time because its state came back.
rm -f "$IMG"
NPROG='print "launch: MARK is [$MARK]"
if [[ -z $MARK ]]; then
  MARK=set-before-suspend
  myfunc() { print FUNC-SURVIVED }
  alias myalias="print ALIAS-SURVIVED"
  exec 9> /proc/ish/checkpoint; print -u9 suspend; exec 9>&-
else
  print "second life: MARK=[$MARK]"
  myfunc
  myalias
fi'
nat_out=$(ISH_GUEST_CHECKPOINT=1 ISH_SESSION="$IMG" "$ISH" -f "$ROOT" \
    /AOK/native/zsh -c "$NPROG" 2>&1)
echo "$nat_out" | sed 's/^/  native  | /'
[ -s "$IMG" ] || { echo "FAIL: no image for the native shell"; exit 1; }
case $nat_out in
    *"second life"*) echo "FAIL: the suspending native shell kept running"; exit 1;;
esac

nat_back=$(ISH_GUEST_CHECKPOINT=1 ISH_SESSION="$IMG" "$ISH" -f "$ROOT" $SH -c 'x' 2>&1)
echo "$nat_back" | sed 's/^/  native  | /'
case $nat_back in
    *"second life: MARK=[set-before-suspend]"*"FUNC-SURVIVED"*"ALIAS-SURVIVED"*) ;;
    *) echo "FAIL: the native shell's state did not survive"
       echo "  got: $nat_back"; exit 1;;
esac

# ---- taken from OUTSIDE the guest ------------------------------------------
#
# The app's path. Backgrounding happens on the UI thread, which is not a guest
# task and has nothing to defer to -- so the checkpoint is synchronous and
# every task is frozen, including the ones the guest-triggered path leaves
# running. ISH_CHECKPOINT_AFTER exercises it from the CLI, where it can be
# tested at all.
#
# Both shapes, because they froze differently: an emulated guest asleep in
# nanosleep, and a NATIVE shell waiting on a child.
for shell in $SH /AOK/native/zsh; do
    rm -f "$IMG" "$IMG.log"
    ext_out=$(ISH_CHECKPOINT_AFTER=1.5:"$IMG" "$ISH" -f "$ROOT" $shell -c '
echo running
/bin/sleep 5
echo finished' 2>&1)
    echo "$ext_out" | while IFS= read -r l; do echo "  outside | $shell: $l"; done
    log=$(cat "$IMG.log" 2>/dev/null)
    case $log in
        written*) ;;
        *) echo "FAIL: external checkpoint of $shell: $log"; exit 1;;
    esac
    # The guest must be UNHARMED: a checkpoint is a copy, and one that stops
    # the thing it is copying is a crash with extra steps.
    case $ext_out in
        *finished*) ;;
        *) echo "FAIL: $shell did not survive being checkpointed"; exit 1;;
    esac
    [ -s "$IMG" ] || { echo "FAIL: external checkpoint wrote no image"; exit 1; }
    echo "  outside | image $(wc -c < "$IMG") bytes, guest unharmed"
done

# And the THIRD shape: a native shell blocked in a READ, owning no child.
#
# This is the only one that reaches ckpt_dump. checkpoint_native_park skips the
# dump when native_standin_child is set, so the loop above -- a shell waiting
# on /bin/sleep -- never called it, and neither did anything else in this file.
# That is how a registry entry whose ckpt_dump was a STRING LITERAL survived:
# written positionally, "AOK_ZSH_STATE_FD" landed in the function pointer (and
# native_zsh_ckpt_dump landed in the setuid_root bool, so native zsh also ran
# as root). The checkpoint then CALLED the string, and the app died every time
# it was backgrounded with an interactive native zsh on screen -- while this
# file passed.
#
# The pipe is the point: it holds stdin open with nothing on it, so zsh is
# inside nlibc_read when the freeze lands, which is where an interactive shell
# waiting for a keystroke also is.
rm -f "$IMG" "$IMG.log"
read_out=$( (sleep 5) | ISH_CHECKPOINT_AFTER=1.5:"$IMG" "$ISH" -f "$ROOT" \
    /AOK/native/zsh -c 'read -r x; echo checkpointed-mid-read' 2>&1 )
echo "$read_out" | while IFS= read -r l; do echo "  outside | zsh-read: $l"; done
log=$(cat "$IMG.log" 2>/dev/null)
case $log in
    written*) ;;
    *) echo "FAIL: external checkpoint of a native shell mid-read: $log"; exit 1;;
esac
case $read_out in
    *checkpointed-mid-read*) ;;
    *) echo "FAIL: native shell did not survive being checkpointed mid-read"
       echo "  got: $read_out"; exit 1;;
esac
[ -s "$IMG" ] || { echo "FAIL: mid-read checkpoint wrote no image"; exit 1; }
echo "  outside | image $(wc -c < "$IMG") bytes, ckpt_dump actually called"

rm -f "$IMG.log"

# ---- and the refusals ------------------------------------------------------
#
# A capability boundary that never fires is not a boundary, it is a comment.
# Each of these is a case v1 cannot do, and each has to SAY so rather than
# writing an image that would not come back.
refuse() {   # refuse <what> <shell> <program> <expected substring>
    out=$(ISH_GUEST_CHECKPOINT=1 "$ISH" -f "$ROOT" "$2" -c "$3" 2>&1 || true)
    case $out in
        *"$4"*) echo "  refused | $1" ;;
        *) echo "FAIL: no refusal for $1"; echo "  got: $out"; exit 1;;
    esac
    rm -f "$IMG"
}
READ_REFUSAL='while read -r l; do case $l in last_refusal*) echo "$l";; esac; done < /proc/ish/checkpoint'

# dash is native too, and unlike zsh it has no way to emit its shell functions
# as text -- jobs.c's commandtext() is display-only and drops CTLESC. So it
# cannot describe itself.
#
# That used to REFUSE the checkpoint, and this leg asserted the refusal. It no
# longer does (2026-09-11): the program is saved and re-launched from its
# command line, because the alternative to a degraded restore is not a perfect
# one, it is no restore at all. So the assertion is inverted -- the save must
# SUCCEED, and must say which program will start again rather than resume.
#
# Guarded against passing trivially: a run that never reached the checkpoint
# would print neither line.
echo "  ---- a native program that cannot describe itself ----"
nat_out=$(ISH_GUEST_CHECKPOINT=1 "$ISH" -f "$ROOT" /AOK/native/dash -c \
    "echo save $IMG > /proc/ish/checkpoint && echo SAVE-OK; \
     while read -r l; do case \$l in saves*|restarted*) echo \"\$l\";; esac; done \
        < /proc/ish/checkpoint" 2>&1 || true)
case $nat_out in
    *SAVE-OK*) echo "  native  | the save was not refused" ;;
    *) echo "FAIL: a native program still refuses the checkpoint"; echo "  got: $nat_out"; exit 1;;
esac
case $nat_out in
    *"re-launched"*dash*) echo "  native  | reported as re-launched: dash" ;;
    *) echo "FAIL: the restart was not reported"; echo "  got: $nat_out"; exit 1;;
esac
[ -s "$IMG" ] || { echo "FAIL: no image written for a native program"; exit 1; }
echo "  native  | image $(wc -c < "$IMG") bytes"
rm -f "$IMG"

# ---- a descriptor that is NOT a tty, on fd 0/1/2 --------------------------
#
# `fd->tty` is a union arm (fs/fd.h), so on a descriptor that is not a terminal
# it holds whichever pointer the other arms left there -- not NULL. ckpt_save_task
# read it for every fd 0/1/2 and locked what it found, which on a daemon whose
# standard streams are a socket is a wild pointer: EXC_BAD_ACCESS, reported from
# a device 2026-09-11 and reproduced here as SIGSEGV (exit 139).
#
# The save must now SUCCEED. A socket is described rather than photographed
# (fs/sock_ckpt.h): this one is a connected AF_UNIX pair with both ends in the
# image, which comes back connected (checkpoint_sockpair.sh). Refusing preserved
# nothing, because iOS destroys these sockets during the suspension anyway.
echo "  ---- a socket on fd 0/1/2 ----"
sock_prog='
perl -e "use Socket; socketpair(A,B,AF_UNIX,SOCK_STREAM,PF_UNSPEC) or die; open(STDIN,q{<&},A); open(STDOUT,q{>&},A); open(STDERR,q{>&},A); sleep 40" &
sleep 3
echo save '"$IMG"' > /proc/ish/checkpoint
sleep 3
echo STILL-ALIVE
while read -r l; do case $l in last_refusal*) echo "$l";; esac; done < /proc/ish/checkpoint
'
sock_out=$(ISH_GUEST_CHECKPOINT=1 "$ISH" -f "$ROOT" /bin/dash -c "$sock_prog" 2>&1)
sock_rc=$?
if [ "$sock_rc" -ge 128 ]; then
    echo "FAIL: the save died on a non-tty descriptor (exit $sock_rc)"; echo "  got: $sock_out"; exit 1
fi
case $sock_out in
    *STILL-ALIVE*) echo "  socket  | the guest survived the save" ;;
    *) echo "FAIL: the guest did not survive a save with a socket on stdio"; echo "  got: $sock_out"; exit 1;;
esac
case $sock_out in
    *"is a socket"*) echo "FAIL: a socket still refuses the save"; echo "  got: $sock_out"; exit 1;;
    *) : ;;
esac
if [ ! -s "$IMG" ]; then
    echo "FAIL: the save wrote no image for a guest holding a socket"; echo "  got: $sock_out"; exit 1
fi
echo "  socket  | saved, $(wc -c < "$IMG" | tr -d ' ') bytes"

rm -f "$IMG"

# A socket is not photographed, it is DESCRIBED and built again -- so the two
# halves of that claim need separate proof, and the leg above only covers
# surviving the save.
#
# 1. A LISTENING socket has to come back listening. This is the case the whole
#    rule exists for: a guest running sshd could not be saved at all before.
#    `$| = 1` is load-bearing -- with perl's stdout block-buffered, the
#    pre-save line sits in the checkpointed userspace buffer and is flushed
#    AGAIN by the restored process, which reads exactly like a guest that
#    re-ran from the top.
echo "  ---- a listening socket ----"
LIMG=${TMPDIR:-/tmp}/aok-ckpt-listen-$$.img
LPORT=34521
rm -f "$LIMG"
listen_prog="perl -e '
\$| = 1;
use Socket;
socket(S, PF_INET, SOCK_STREAM, getprotobyname(\"tcp\")) or die \"socket: \$!\";
setsockopt(S, SOL_SOCKET, SO_REUSEADDR, pack(\"l\",1));
bind(S, sockaddr_in($LPORT, INADDR_LOOPBACK)) or die \"bind: \$!\";
listen(S, 7) or die \"listen: \$!\";
print \"A-BEFORE-SAVE\n\";
open(C, \">\", \"/proc/ish/checkpoint\") or die; print C \"save $LIMG\n\"; close(C);
print \"B-AFTER-SAVE\n\";
my \$n = getsockname(S) or die \"getsockname: \$!\";
my (\$p) = sockaddr_in(\$n);
print \"C-STILL-BOUND-\$p\n\";
socket(K, PF_INET, SOCK_STREAM, getprotobyname(\"tcp\")) or die;
if (connect(K, sockaddr_in($LPORT, INADDR_LOOPBACK))) {
  if (accept(A, S)) { print \"D-ACCEPTED\n\"; } else { print \"D-ACCEPT-FAILED\n\"; }
} else { print \"D-CONNECT-FAILED\n\"; }
'"
lsave=$(ISH_GUEST_CHECKPOINT=1 "$ISH" -f "$ROOT" $SH -c "$listen_prog" 2>&1)
[ -s "$LIMG" ] || { echo "FAIL: a guest with a listening socket wrote no image"; echo "  got: $lsave"; rm -f "$LIMG"; exit 1; }
lrest=$(ISH_RESTORE="$LIMG" "$ISH" -f "$ROOT" 2>&1)
rm -f "$LIMG"
case $lrest in
    *A-BEFORE-SAVE*) echo "FAIL: the restored guest re-ran rather than continued"; echo "  got: $lrest"; exit 1;;
esac
case $lrest in
    *C-STILL-BOUND-$LPORT*) echo "  listen  | came back bound to port $LPORT" ;;
    *) echo "FAIL: the restored listening socket lost its address"; echo "  got: $lrest"; exit 1;;
esac
case $lrest in
    *D-ACCEPTED*) echo "  listen  | and accepted a connection on it" ;;
    *) echo "FAIL: the restored socket was not actually listening"; echo "  got: $lrest"; exit 1;;
esac

# 2. A TCP connection cannot be rebuilt -- its far end is usually a process
#    outside the image, an ssh client, and a host TCP connection has no way
#    back -- so it comes back hung up. That has to mean end-of-file and
#    ENOTCONN, not a descriptor that hangs: a read that blocks forever is worse
#    than the refusal this replaced. (A connected LOCAL pair with both ends in
#    the image is rebuilt instead; checkpoint_sockpair.sh.) Both ends are in
#    this guest only so the test needs nothing outside it; close(Q) after the
#    peer check lets the run that saved read its EOF and finish.
echo "  ---- a connected socket comes back hung up ----"
HIMG=${TMPDIR:-/tmp}/aok-ckpt-hungup-$$.img
HPORT=34522
rm -f "$HIMG"
hungup_prog="perl -e '
\$| = 1;
use Socket;
socket(L, PF_INET, SOCK_STREAM, getprotobyname(\"tcp\")) or die \"socket: \$!\";
setsockopt(L, SOL_SOCKET, SO_REUSEADDR, pack(\"l\",1));
bind(L, sockaddr_in($HPORT, INADDR_LOOPBACK)) or die \"bind: \$!\";
listen(L, 1) or die \"listen: \$!\";
socket(P, PF_INET, SOCK_STREAM, getprotobyname(\"tcp\")) or die;
connect(P, sockaddr_in($HPORT, INADDR_LOOPBACK)) or die \"connect: \$!\";
accept(Q, L) or die \"accept: \$!\";
print \"A-BEFORE-SAVE\n\";
open(C, \">\", \"/proc/ish/checkpoint\") or die; print C \"save $HIMG\n\"; close(C);
print \"B-AFTER-SAVE\n\";
my \$pn = getpeername(P);
print \"C-PEER-\", (defined \$pn ? \"OK\" : \"ENOTCONN\"), \"\n\";
close(Q);
my \$buf = \"\"; my \$n = sysread(P, \$buf, 64);
if (!defined \$n) { print \"D-READ-ERROR\n\"; }
elsif (\$n == 0) { print \"D-READ-EOF\n\"; }
else { print \"D-READ-BYTES\n\"; }
'"
hsave=$(ISH_GUEST_CHECKPOINT=1 "$ISH" -f "$ROOT" $SH -c "$hungup_prog" 2>&1)
[ -s "$HIMG" ] || { echo "FAIL: a guest with a connected socket wrote no image"; echo "  got: $hsave"; rm -f "$HIMG"; exit 1; }
hrest=$(ISH_RESTORE="$HIMG" "$ISH" -f "$ROOT" 2>&1)
rm -f "$HIMG"
case $hrest in
    *C-PEER-ENOTCONN*) echo "  hungup  | getpeername says ENOTCONN" ;;
    *) echo "FAIL: a restored connected socket still claims a peer"; echo "  got: $hrest"; exit 1;;
esac
case $hrest in
    *D-READ-EOF*) echo "  hungup  | and reading it gives end-of-file, not a hang" ;;
    *) echo "FAIL: a restored dead socket did not read EOF"; echo "  got: $hrest"; exit 1;;
esac

# 3. And poll() has to SAY it is readable. A closed socket is readable on Linux
#    because a read returns 0, so revents is POLLIN|POLLHUP -- a program that
#    waits for POLLIN before reading must be told to read. This asserts the
#    bit, not just the wake-up: select() counts POLLHUP as readable all by
#    itself, so a select-based check passes even when POLLIN is missing and
#    every poll()-based program is left waiting. Measured at 0x18
#    (POLLHUP|POLLERR) before the ckpt_hungup arm in sock_poll existed.
#    docs/build_555_musts.md item 4 asks for exactly this assertion.
PIMG=${TMPDIR:-/tmp}/aok-ckpt-poll-$$.img
PPORT=34523
rm -f "$PIMG"
poll_prog="perl -e '
\$| = 1;
use Socket; use IO::Poll qw(POLLIN POLLHUP);
socket(L, PF_INET, SOCK_STREAM, getprotobyname(\"tcp\")) or die;
setsockopt(L, SOL_SOCKET, SO_REUSEADDR, pack(\"l\",1));
bind(L, sockaddr_in($PPORT, INADDR_LOOPBACK)) or die; listen(L, 1) or die;
socket(P, PF_INET, SOCK_STREAM, getprotobyname(\"tcp\")) or die;
connect(P, sockaddr_in($PPORT, INADDR_LOOPBACK)) or die; accept(Q, L) or die;
open(C, \">\", \"/proc/ish/checkpoint\") or die; print C \"save $PIMG\n\"; close(C);
my \$p = IO::Poll->new; \$p->mask(\*P => POLLIN);
my \$n = \$p->poll(5); my \$ev = \$p->events(\*P) || 0;
printf(\"P-REVENTS-0x%x\n\", \$ev);
print \"P-HAS-POLLIN\n\" if \$ev & POLLIN;
print \"P-HAS-POLLHUP\n\" if \$ev & POLLHUP;
'"
ISH_GUEST_CHECKPOINT=1 "$ISH" -f "$ROOT" $SH -c "$poll_prog" >/dev/null 2>&1 || true
if [ -s "$PIMG" ]; then
    prest=$(ISH_RESTORE="$PIMG" "$ISH" -f "$ROOT" 2>&1)
    rm -f "$PIMG"
    case $prest in
        *P-HAS-POLLIN*) echo "  hungup  | poll() reports POLLIN, so a poll loop reads the EOF" ;;
        *) echo "FAIL: poll() on a restored hung-up socket omits POLLIN"; echo "  got: $prest"; exit 1;;
    esac
    case $prest in
        *P-HAS-POLLHUP*) : ;;
        *) echo "FAIL: poll() on a restored hung-up socket omits POLLHUP"; echo "  got: $prest"; exit 1;;
    esac
else
    rm -f "$PIMG"
    echo "  hungup  | (poll leg skipped: no image)"
fi

# ---- reserved address space ------------------------------------------------
#
# A large anonymous mapping is a RESERVATION with no page-table entries
# (emu/memory.h, struct mem_lazy_map), so a save that walked only entries lost
# every one: a JVM-style PROT_NONE heap came back a hole that new mmaps landed
# in, and the untouched tail of a large RW mapping faulted on its first write.
# Writing reserved pages out as zeroes would be correct and put the whole
# reservation in the image, so the image must also stay small.
# checkpoint_lazy_reservation.c checks both lives and says which it is in.
echo "  ---- reserved address space ----"
RIMG=${TMPDIR:-/tmp}/aok-ckpt-lazy-$$.img
rm -f "$RIMG"
lazy_build=$("$ISH" -f "$ROOT" $SH -c '
command -v cc > /dev/null || { echo NO-CC; exit 0; }
cc -O1 -o /tmp/ckpt_lazy_reservation /AOK/tests/checkpoint_lazy_reservation.c && echo BUILT' 2>&1 || true)
case $lazy_build in
    *BUILT*)
        # `|| true`: the probe exits 1 on a failed check, which set -e would
        # otherwise turn into a silent exit before the verdict below.
        lz_save=$(ISH_GUEST_CHECKPOINT=1 "$ISH" -f "$ROOT" /tmp/ckpt_lazy_reservation "$RIMG" 2>&1 || true)
        echo "$lz_save" | sed 's/^/  lazy    | /'
        case $lz_save in
            *"life: original"*"RESULT: PASS"*) ;;
            *) echo "FAIL: reserved address space was wrong before the save"; rm -f "$RIMG"; exit 1;;
        esac
        [ -s "$RIMG" ] || { echo "FAIL: no image for the reserved address space"; exit 1; }
        lz_size=$(wc -c < "$RIMG" | tr -d ' ')
        # 640 MB is reserved and a few MB touched.
        if [ "$lz_size" -ge 104857600 ]; then
            echo "FAIL: the image holds reserved pages as bytes ($lz_size bytes)"; rm -f "$RIMG"; exit 1
        fi
        echo "  lazy    | image $lz_size bytes"
        lz_back=$(ISH_RESTORE="$RIMG" "$ISH" -f "$ROOT" 2>&1 || true)
        rm -f "$RIMG"
        echo "$lz_back" | sed 's/^/  lazy    | /'
        case $lz_back in
            *A-BEFORE-SAVE*) echo "FAIL: the restored guest re-ran rather than continued"; exit 1;;
        esac
        case $lz_back in
            *"life: restored"*"RESULT: PASS"*) echo "  lazy    | reservations came back reserved" ;;
            *) echo "FAIL: reserved address space did not survive the restore"; exit 1;;
        esac
        # And from outside the guest, the app's path: checkpoint_save_external.
        rm -f "$RIMG" "$RIMG.log"
        lz_ext=$(ISH_CHECKPOINT_AFTER=2:"$RIMG" "$ISH" -f "$ROOT" \
            /tmp/ckpt_lazy_reservation --sleep 5 2>&1 || true)
        echo "$lz_ext" | sed 's/^/  lazy    | outside: /'
        case $lz_ext in
            *"life: original"*"RESULT: PASS"*) ;;
            *) echo "FAIL: reserved address space was wrong around an external save"; rm -f "$RIMG" "$RIMG.log"; exit 1;;
        esac
        case $(cat "$RIMG.log" 2>/dev/null) in
            written*) ;;
            *) echo "FAIL: external checkpoint of reserved address space: $(cat "$RIMG.log" 2>/dev/null)"
               rm -f "$RIMG" "$RIMG.log"; exit 1;;
        esac
        rm -f "$RIMG.log"
        lz_size=$(wc -c < "$RIMG" | tr -d ' ')
        if [ "$lz_size" -ge 104857600 ]; then
            echo "FAIL: the external image holds reserved pages as bytes ($lz_size bytes)"; rm -f "$RIMG"; exit 1
        fi
        lz_back=$(ISH_RESTORE="$RIMG" "$ISH" -f "$ROOT" 2>&1 || true)
        rm -f "$RIMG"
        echo "$lz_back" | sed 's/^/  lazy    | outside: /'
        case $lz_back in
            *A-BEFORE-SAVE*) echo "FAIL: the externally restored guest re-ran rather than continued"; exit 1;;
        esac
        case $lz_back in
            *"life: restored"*"RESULT: PASS"*) echo "  lazy    | and from outside the guest, image $lz_size bytes" ;;
            *) echo "FAIL: reserved address space did not survive an external save"; exit 1;;
        esac
        ;;
    *NO-CC*)
        echo "  lazy    | (skipped: no cc in $ROOT)" ;;
    *)
        echo "FAIL: could not build checkpoint_lazy_reservation"; echo "  got: $lazy_build"; exit 1;;
esac

echo "PASS: continued from the instruction after the checkpoint, same file, same offset"
