#!/bin/sh
# native_bash_fork_state.sh -- what must survive a native bash subshell.
# Run inside the guest (device or CLI root):
#
#   /AOK/tests/native_bash_fork_state.sh
#
# WHY THIS EXISTS, and why it is separate from the zsh one. Native bash uses
# the same fork-by-re-launch design (deps/bash/aok_fork.c) but reaps its
# children differently, so the two shells fail in different ways from the same
# kernel change. The case that motivated this file proves the point:
#
#   kernel/fork.c gave a native-spawned task SIGCHLD as its exit signal. It had
#   to -- a native ZSH waits for a job by sleeping in sigsuspend until its
#   handler reaps, so with no exit signal the child became a zombie and the
#   shell hung forever on its first external command. bash does not wait that
#   way, but it does install sigchld_handler, and waitchld() reaps with
#   waitpid(-1, WNOHANG) keeping a status only for pids in its OWN jobs table.
#   A re-launch child is not one, so the status was discarded, the explicit
#   waitpid returned ECHILD, and `x=$(sh -c "exit 6"); echo $?` printed 0.
#
# So a fix that is REQUIRED by one native shell silently broke the other, and
# nothing in the zsh suite could have caught it. That is the class this file
# guards: kernel-level changes whose blast radius crosses shells.

B=/AOK/native/bash
pass=0; fail=0

ck() {
    name=$1; want=$2; shift 2
    got=$("$B" -c "$*" 2>&1 | tail -1)
    if [ "$got" = "$want" ]; then
        pass=$((pass+1)); printf '  ok    %s\n' "$name"
    else
        fail=$((fail+1)); printf '  FAIL  %-30s want [%s] got [%s]\n' "$name" "$want" "$got"
    fi
}

echo "== command substitution must carry the exit status =="
# The regression above. An EXTERNAL command is the case that broke; a builtin
# never spawns a task for the handler to steal, so test both.
ck cmdsubst-external 6      'x=$(sh -c "exit 6"); echo $?'
ck cmdsubst-builtin  1      'x=$(false); echo $?'
ck cmdsubst-value    hi     'x=$(echo hi); echo $x'
ck cmdsubst-nested   deep   'echo $(echo $(echo deep))'

echo "== the shapes that were NOT broken, so they stay that way =="
# Each of these collects a status by a different route than $( ) does.
ck subshell-rc       6      '( sh -c "exit 6" ); echo $?'
ck direct-external   6      'sh -c "exit 6"; echo $?'
ck pipestatus        6      'sh -c "exit 6" | cat; echo ${PIPESTATUS[0]}'
ck wait-status       4      'sh -c "exit 4" & wait $!; echo $?'

echo "== job control still works =="
ck bg-wait           ok     'sleep 1 & wait; echo ok'
ck lastpid           ok     'sleep 1 & [ -n "$!" ] && echo ok; wait'
ck jobs-builtin      ok     'sleep 1 & jobs >/dev/null && echo ok; wait'

echo "== state crossing =="
ck var               outer  'v=outer; ( v=inner ); echo $v'
ck function          fn-ok  'f(){ echo fn-ok; }; echo $(f)'
ck exported          exp    'export E=exp; echo $(echo $E)'

echo
echo "  passed=$pass failed=$fail"
[ "$fail" -eq 0 ]
