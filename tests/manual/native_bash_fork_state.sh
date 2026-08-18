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

echo "== recursion must not take the app down =="
# bash is the worse case of the two shells: FUNCNEST is UNSET by default, so
# bash's own limit never fires and the stack guard is the ONLY thing between a
# runaway recursive function and the end of a guest task thread's stack --
# which, for a native program, is the end of the APP. If one of these regresses
# the app dies and this script reports nothing at all, which is the point.
# These assert the MESSAGE, not a later command: the guard ends the -c script,
# so expecting anything printed afterwards would assert behaviour the shell does
# not have. A message at all proves the guard fired instead of the app dying --
# and the app dying is what this case exists to catch.
ck recurse-unbounded "environment: line 1: r: maximum function nesting level exceeded (out of stack)" 'r(){ r; }; r'
ck recurse-funcnest  "environment: line 1: r: maximum function nesting level exceeded (out of stack)" 'FUNCNEST=5000; r(){ r; }; r'
ck recurse-subshell   "rc=1 alive" 'r(){ r; }; x=$(r) 2>/dev/null; echo rc=$? alive'
ck recurse-legal-400  deep-ok  'f(){ [ $1 -gt 0 ] && f $(($1-1)) || echo deep-ok; }; f 400'
# bash's own FUNCNEST must still work where it applies.
ck funcnest-still-works 1 'FUNCNEST=10; r(){ r; }; r 2>&1 | grep -c "exceeded (10)"'

echo "== the locale comes from the guest, not the host =="
# setlocale(cat, "") means "take it from the environment", and a native program
# is a function call inside the app -- so the C library resolved it against the
# HOST's environment, which on a device is whatever iOS is set to. Native and
# emulated bash then disagreed about what a character IS, and length, case and
# collation all follow from that. Asserted as AGREEMENT with the guest's own
# bash rather than a fixed number, so the case holds whatever locale the guest
# is actually in.
ck locale-agrees agree 'n=$(/AOK/native/bash -c "e=\$(printf \\303\\251); echo \${#e}"); g=$(/bin/bash -c "e=\$(printf \\303\\251); echo \${#e}"); [ "$n" = "$g" ] && echo agree || echo "differ n=$n g=$g"'

echo "== state crossing =="
ck var               outer  'v=outer; ( v=inner ); echo $v'
ck function          fn-ok  'f(){ echo fn-ok; }; echo $(f)'
ck exported          exp    'export E=exp; echo $(echo $E)'

echo
echo "  passed=$pass failed=$fail"
[ "$fail" -eq 0 ]
