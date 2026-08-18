#!/bin/sh
# native_zsh_fork_state.sh -- what must survive a native zsh subshell.
# Run inside the guest (device or CLI root):
#
#   /AOK/tests/native_zsh_fork_state.sh
#
# WHY THIS EXISTS. A native program is a C function on a guest task's thread
# inside one address space, so zsh cannot fork(). Every subshell -- $(...),
# a pipeline, ( ), &, <(...) -- is instead a RE-LAUNCH: the parent serialises
# its state into a script and spawns a fresh native zsh to run the command
# (deps/zsh/Src/aok_fork.c). That serialiser is the single largest source of
# behavioural difference from real zsh in this shell, and a differential probe
# against zsh 5.9 found 42 confirmed defects in it in one sweep.
#
# Each case below is a defect that was found, fixed, and must not come back.
# The oracle for every one of them is what /bin/zsh prints, not what looks
# reasonable -- several "obvious" expectations here are wrong (see FLOAT).
#
# A NOTE ON THE APP-KILLER CASES. Four of these used to take the WHOLE APP
# down -- SIGBUS, a segfault, or an unbounded hang -- because a native program
# shares the app's address space. If one of them regresses, this script does
# not fail: the app dies. That is not a flaw in the test, it is the reason the
# cases are worth keeping.

Z=/AOK/native/zsh
pass=0; fail=0

# ck NAME EXPECTED COMMAND -- runs COMMAND in a fresh native zsh, compares.
ck() {
    name=$1; want=$2; shift 2
    got=$("$Z" -f -c "$*" 2>&1 | tail -1)
    if [ "$got" = "$want" ]; then
        pass=$((pass+1)); printf '  ok    %s\n' "$name"
    else
        fail=$((fail+1)); printf '  FAIL  %-34s want [%s] got [%s]\n' "$name" "$want" "$got"
    fi
}

echo "== state crossing the boundary =="
# Each of these was dropped or mangled by the serialiser at some point.
ck scalar        hello    'v=hello; echo $(echo $v)'
ck array         "y 3"    'a=(x y z); echo $(echo ${a[2]} ${#a})'
ck assoc         v        'typeset -A m; m[k]=v; echo $(echo ${m[k]})'
# -H means "hide value in listings", and typeset -p honoured it to the letter:
# the value AND the -H itself were lost. compinit declares its tables this way,
# so every completion table emptied on the way across.
ck hidden-assoc  secret   'typeset -HA h; h[k]=secret; echo $(echo ${h[k]:-LOST})'
ck integer       43       'typeset -i n=42; echo $(echo $((n+1)))'
# FLOAT: 1.5000000000, not 1.5. That is what zsh -F prints; do not "fix" it.
ck float         1.5000000000 'typeset -F f=1.5; echo $(echo $f)'
# Negative zero: "%.17g" of -0.0 is "-0", which zsh arithmetic reads as unary
# minus applied to the INTEGER 0, losing the sign bit.
ck negative-zero -0.0000000000 'typeset -F z=-0.0; echo $(echo $z)'
ck readonly      fixed    'typeset -r r=fixed; echo $(echo $r)'
ck exported      exp      'export E=exp; echo $(echo $E)'
ck function      fn-ok    'f(){ echo fn-ok; }; echo $(f)'
# A function body crosses as TEXT and is re-parsed by the child. When options
# were emitted above the text, a body like this was a parse error under sh
# emulation and the whole state aborted there.
ck fn-glob       glob-ok  'g(){ case $1 in *.txt) echo glob-ok;; esac; }; echo $(g a.txt)'
ck alias         alias-ok 'alias hi="echo alias-ok"; echo $(hi)'
ck positional    p1-p2-2  'set -- p1 p2; echo $(echo $1-$2-$#)'
ck exit-status   rc=1     'false; echo $(echo rc=$?)'

echo "== shell options =="
# Options are read from opts[] in C, not from ${(kv)options} in the state
# script: the script runs inside `emulate -L zsh`, which resets the very
# options it would be reporting.
ck option-crosses active 'setopt extendedglob; echo $( [[ abc == a#bc ]] && echo active || echo INACTIVE )'

echo "== special parameters =="
# The serialiser used to skip every parameter zsh marks "special", which threw
# away this entire set. IFS is the pervasive one: without it every subshell
# word-splits with the default.
ck ifs           "a b"    'IFS=,; v="a,b"; echo $(echo ${=v})'
# $! read 0 in the child, so `kill $!` in a subshell became `kill 0` -- which
# killed the parent shell.
ck lastpid       pid-ok   'sleep 1 & echo $( [[ $! -gt 0 ]] && echo pid-ok || echo ZERO ); wait'
ck ppid          ppid-ok  'echo $( [[ $PPID -gt 0 ]] && echo ppid-ok || echo ZERO )'
ck subshell-lvl  1        'echo $(echo $ZSH_SUBSHELL)'
ck histsize      4321     'HISTSIZE=4321; echo $(echo $HISTSIZE)'
ck fpath         /zzz     'fpath=(/zzz $fpath); echo $(echo ${fpath[1]})'
ck pipestatus    1        'false | true; echo $(echo ${pipestatus[1]})'
# ${(q-)dirstack} in a double-quoted context joined the array into one word.
ck dirstack      2        'pushd /tmp >/dev/null; pushd / >/dev/null; echo $(echo ${#dirstack})'

echo "== fork shapes =="
ck cmdsubst      A=sub    'A=$(echo sub); echo A=$A'
ck nested        deep     'echo $(echo $(echo deep))'
ck pipeline      3        'printf "a\nb\nc\n" | wc -l | tr -d " "'
ck subshell-rc   rc=3     '( exit 3 ); echo rc=$?'
# The load-bearing one: a subshell must NOT write back into its parent.
ck isolation     outer    'v=outer; ( v=inner ); echo $v'
ck bg-wait       waited   'sleep 1 & wait; echo waited'
ck procsubst     psub     'echo $(cat <(echo psub))'
# A signal is not an answer to "did my child finish": EINTR from the reaping
# wait was read as "exec failed", giving 127 for a command that ran fine.
ck procsub-ext   3        'wc -c < <(echo hi) | tr -d " "'
ck wait-signal   143      'sleep 5 & p=$!; sleep 1; kill -TERM $p; wait $p; echo $?'

echo "== modules =="
# A module can own a READONLY parameter, and the child loads modules before it
# replays parameters -- so re-declaring ZFTP_SESSION aborted the whole state.
ck zftp-subshell 1        'zmodload zsh/zftp 2>/dev/null; echo $(zmodload -L | grep -c zftp)'

echo "== MULTIOS (two or more redirections on one descriptor) =="
# The byte pump is a child that is NOT a shell, so a re-launch cannot express it;
# it is a separate native program, zsh-multio, spawned as its own guest task.
# Before that existed, every one of these refused outright.
mkdir -p /tmp/aokmul 2>/dev/null
ck multios-out   "hi|hi" 'cd /tmp/aokmul; rm -f a b; echo hi > a > b; print "$(cat a)|$(cat b)"'
ck multios-in    "I1 I2" 'cd /tmp/aokmul; print I1 > i1; print I2 > i2; print $(cat < i1 < i2)'
# The one that mattered most: this used to write the file, return 0, print no
# warning, and hand the downstream command an EMPTY pipe. It must do both.
ck multios-pipe  "hi|hi" 'cd /tmp/aokmul; rm -f e; print "$(echo hi > e | cat)|$(cat e)"'
# A descriptor CLOSE is not a second redirection and must not trip the check.
ck multios-close "hi"    'cd /tmp/aokmul; rm -f f; echo hi > f 2>&-; cat f'

echo "== app-killers (these used to take the app down) =="
# The state script is zsh text the PARENT parses, so every word in it resolved
# against the parent's aliases, functions, builtins and reserved words. All
# four are user-settable and all four were fatal.
ck alias-emulate ok       'alias emulate="echo BROKEN"; echo $(echo ok)'
ck alias-zmodload ok      'alias zmodload="echo x"; echo $(echo ok)'
ck disable-print ok       'disable print; echo $(echo ok)'
ck fn-named-time ok       'time() { echo t }; echo $(echo ok)'
ck fn-named-print ok      'print() { echo p }; echo $(echo ok)'
# The subshell text was passed as `zsh -f -c <text>` with no --, so text
# starting with a dash was parsed as OPTIONS.
ck leading-dash  "[]"     'echo "[$( -e )]"'

echo
echo "  passed=$pass failed=$fail"
[ "$fail" -eq 0 ]
