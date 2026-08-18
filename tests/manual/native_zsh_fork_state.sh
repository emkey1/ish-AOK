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

echo "== recursion must not take the app down =="
# A native program runs on a guest task thread, whose stack is far smaller than
# the main thread's, while zsh's FUNCNEST default assumes otherwise. Unbounded
# recursion ran off the end and killed the WHOLE APP -- and zsh's own message
# ("increase FUNCNEST?") is exactly the wrong advice when the limit is the
# stack, so following it made things worse. doshfunc now checks real thread
# headroom. If one of these regresses the app dies and this script reports
# nothing at all, which is the point of keeping them.
# The top-level cases assert the MESSAGE, not a later command: real zsh also
# abandons the rest of a -c script after a nested-function error, so expecting
# anything printed afterwards would assert behaviour the oracle does not have.
# A message at all proves the guard fired rather than the app dying.
ck recurse-default   "r: maximum nested function level reached; increase FUNCNEST?" 'r(){ r; }; r'
ck recurse-nofuncnest "r: maximum nested function level reached; out of stack (raising FUNCNEST will not help)" 'FUNCNEST=99999; r(){ r; }; r'
# The guard must reach a RE-LAUNCHED child too -- a subshell is a fresh task on
# a fresh thread, so the bounds have to be worked out there as well.
ck recurse-subshell  "rc=1 ALIVE" 'FUNCNEST=99999; r(){ r; }; x=$(r) 2>/dev/null; print rc=$? ALIVE'
ck recurse-pipeline  ALIVE      'FUNCNEST=99999; r(){ r; }; r 2>/dev/null | cat; print ALIVE'
ck recurse-background ALIVE     'FUNCNEST=99999; r(){ r; }; r 2>/dev/null & wait; print ALIVE'
# ...while legal deep recursion still completes. The reserve costs ~6% of the
# usable depth, so this must stay well inside it.
ck recurse-legal-400 deep-ok    'f(){ (( $1 > 0 )) && f $(( $1 - 1 )) || print deep-ok }; f 400'

echo "== globbing across the boundary =="
# The re-launch hands the child ALREADY-EXPANDED words, so that a $( ) is not
# evaluated twice. They were taken one step too early -- before globbing -- so
# the child received a quoted literal '*'. `rm -f *` deleted NOTHING whenever it
# was not the last command in the shell, status 0, no diagnostic. Only external
# commands in non-final position were hit, which is how it survived a 1167-case
# differential sweep.
ck glob-not-last "a b c"  'rm -rf /tmp/aokg; mkdir -p /tmp/aokg; cd /tmp/aokg; touch a b c; /bin/echo *; true'
ck glob-rm-works "0"      'rm -rf /tmp/aokg2; mkdir -p /tmp/aokg2; cd /tmp/aokg2; touch x y; rm -f *; true; ls | wc -l | tr -d " "'
ck glob-noglob   "*"      'rm -rf /tmp/aokgn; mkdir -p /tmp/aokgn; cd /tmp/aokgn; touch a; setopt noglob; /bin/echo *; true'
# The double-evaluation guard the expanded-words design exists for: RAN once.
ck glob-eval-once "RAN a b c" 'rm -rf /tmp/aokge; mkdir -p /tmp/aokge; cd /tmp/aokge; touch a b c; /bin/echo $(echo RAN) *; true'

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

# ...AND THE SAME THING AGAIN WHEN THE COMMAND ALSO SUBSTITUTES SOMETHING.
#
# The pipeline element is told about its pipe in AOK_ZSH_PIPEIO, and the bits
# used to be taken by whichever execcmd_exec reached the descriptors first --
# which is NOT the same frame as the one that was entered first. Anything the
# argument expansion runs gets there sooner, and every re-launch runs the state
# serialiser, which is a zsh function this shell executes. So a substitution
# anywhere in the command stole the bits, dup'd the STATE PIPE instead of the
# pipeline's, and the file quietly replaced the pipe again -- the exact failure
# the channel exists to prevent, back for every command that substitutes.
# `echo hi > f | cat` worked and `echo $(echo hi) > f | cat` did not.
ck multios-cmdsubst  "hi|hi" 'cd /tmp/aokmul; rm -f e; print "$(echo $(echo hi) > e | cat)|$(cat e)"'
ck multios-backtick  "hi|hi" 'cd /tmp/aokmul; rm -f e; print "$(echo `echo hi` > e | cat)|$(cat e)"'
ck multios-procsub   "hi|hi" 'cd /tmp/aokmul; rm -f e; print "$(cat <(echo hi) > e | cat)|$(cat e)"'
# =(...) is the one that needs the bits passed on a SECOND time: it puts a name
# in the file list, havefiles() rules out the fake exec, so the element has to
# re-launch again and the grandchild is what actually runs the command.
ck multios-eqsub     "hi|hi" 'cd /tmp/aokmul; rm -f e; print "$(cat =(echo hi) > e | cat)|$(cat e)"'
# A container does NOT get the bits -- zsh gives the pipe to the group and the
# inner redirection replaces it for the inner command only, so cat gets nothing.
ck multios-group     "|hi"   'cd /tmp/aokmul; rm -f e; print "$({ echo $(echo hi) > e } | cat)|$(cat e)"'

echo "== process substitution: a temporary name per call =="
# The app's mktemp fills the XXXXXX from rand(), and rand() in a re-launched
# child replays the sequence its parent just used; its retry after a collision
# then rewrites a template it has already overwritten and returns NULL. So
# gettempname returned NULL, getoutputfile returned NULL, and the WHOLE WORD
# disappeared -- no message, status 0. Five substitutions produced two words.
ck procsub-five-names "5" 'print -rl -- =(echo 1) =(echo 2) =(echo 3) =(echo 4) =(echo 5) | sort -u | wc -l | tr -d " "'
ck procsub-five-read  "12345" 'cat =(echo 1) =(echo 2) =(echo 3) =(echo 4) =(echo 5) | tr -d "\n"'
ck procsub-in-child   "12" 'print -r -- "$(cat =(echo 1) =(echo 2) | tr -d "\n")"'
# The same failure one level down: with a redirection the command could not
# fake-exec, so it was re-launched and the child's name collided with the
# parent's. cat lost its argument, read its own stdin, and the file was empty.
ck procsub-redirected "X" 'cd /tmp/aokmul; rm -f p1; cat =(echo X) > p1; cat p1'
ck procsub-assigned   "X" 'cd /tmp/aokmul; rm -f p2; V=1 cat =(echo X) > p2; cat p2'

echo "== the words a re-launch hands over, when it also has a redirection =="
# A command with a redirection used to be re-launched as SOURCE TEXT, so the
# child ran every expansion a second time -- `/bin/echo $(cmd) > f` ran cmd
# twice -- and `cat =(echo X) > h` never terminated at all, because each child
# made another temp file and re-launched for the same reason.
ck redir-eval-once  "1" 'rm -rf /tmp/aokre; mkdir -p /tmp/aokre; cd /tmp/aokre; /bin/echo $(print -n x >> c; print v) > o; true; wc -c < c | tr -d " "'
ck redir-words-ok   "v" 'rm -rf /tmp/aokre2; mkdir -p /tmp/aokre2; cd /tmp/aokre2; /bin/echo $(print v) > o; true; cat o'
ck assign-eval-once "1" 'rm -rf /tmp/aokre3; mkdir -p /tmp/aokre3; cd /tmp/aokre3; V=1 /bin/echo $(print -n x >> c; print v) > o; true; wc -c < c | tr -d " "'
# The assignment must still reach the command, and must still not stick.
ck assign-reaches   "1" 'rm -rf /tmp/aokre4; mkdir -p /tmp/aokre4; cd /tmp/aokre4; V=1 /usr/bin/printenv V > o; true; cat o'
ck assign-not-stuck "[]" 'V=1 /bin/echo hi > /dev/null; print "[$V]"'

echo "== a re-launched element still reports its own write errors =="
# `save[1] == -2` is upstream asking "am I a forked child about to exit?" -- a
# fork does not save its descriptors, so a write error there is reported, while
# the main shell, which will restore fd 1, swallows it. A re-launched element
# is that child but saves like a main shell, so the diagnostic was thrown away
# and `print` wrote nowhere, said nothing and exited 0.
ck writeerr-pipe   "zsh:1: write error: bad file descriptor" 'cd /tmp/aokmul; rm -f w1; print a > w1 >&- | cat'
ck writeerr-noredir "zsh:1: write error: bad file descriptor" 'print a >&- | cat'
ck writeerr-async  "zsh:1: write error: bad file descriptor" 'print a >&- & wait'
# ...and the shell that is going to carry on still swallows it, as zsh does.
ck writeerr-quiet  "[]" 'cd /tmp/aokmul; rm -f w2; print "[$(print a > w2 >&- 2>&1)]"'

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

echo "== the EXIT trap fires where zsh fires it, and nowhere else =="
# A re-launched child is a main shell, so it leaves through zexit() and zexit()
# fires the EXIT trap. A FORKED child mostly does not: `( )`, a pipeline
# element and a background job leave through _realexit(), and only the
# substitution sites run their body with execode(..., exiting, ...). So an
# inherited TRAPEXIT fired at EVERY fork site -- one firing per subshell more
# than zsh 5.9. Counted rather than watched, because a $( ) captures the
# trap's stdout and "no output" proves nothing.
XT=/tmp/aokxt
ck exittrap-parens    0 ": > $XT; TRAPEXIT(){ print -r -- X >> $XT }; ( true ); unfunction TRAPEXIT; print -r -- \$(wc -l < $XT | tr -d ' ')"
ck exittrap-pipe      0 ": > $XT; TRAPEXIT(){ print -r -- X >> $XT }; { true } | cat; unfunction TRAPEXIT; print -r -- \$(wc -l < $XT | tr -d ' ')"
ck exittrap-async     0 ": > $XT; TRAPEXIT(){ print -r -- X >> $XT }; { true } & wait; unfunction TRAPEXIT; print -r -- \$(wc -l < $XT | tr -d ' ')"
ck exittrap-cmdsubst  1 ": > $XT; TRAPEXIT(){ print -r -- X >> $XT }; v=\$(true); unfunction TRAPEXIT; print -r -- \$(wc -l < $XT | tr -d ' ')"
ck exittrap-nested    2 ": > $XT; TRAPEXIT(){ print -r -- X >> $XT }; v=\$(w=\$(true); true); unfunction TRAPEXIT; print -r -- \$(wc -l < $XT | tr -d ' ')"
# A cmdsubst INSIDE a ( ) fires nothing: the ( ) child has no exit trap left to
# hand on, so the whole subtree is quiet.
ck exittrap-under-sub 0 ": > $XT; TRAPEXIT(){ print -r -- X >> $XT }; ( v=\$(true) ); unfunction TRAPEXIT; print -r -- \$(wc -l < $XT | tr -d ' ')"
# ...and the substitution child must not exec its last command away, or it
# leaves without running the trap it still owes. Clause (2) of the fake-exec
# test counts SIGEXIT even though the child inherited it.
ck exittrap-external  1 ": > $XT; TRAPEXIT(){ print -r -- X >> $XT }; v=\$(/bin/echo a); unfunction TRAPEXIT; print -r -- \$(wc -l < $XT | tr -d ' ')"
# The FUNCTION still crosses even where the trap does not: zsh leaves TRAPEXIT
# in shfunctab and only takes the arming off it, which is why `unfunction`
# there answers "no such hash table element" while `functions` prints the body.
ck exittrap-defined   1 "TRAPEXIT(){ : }; ( print -r -- \${+functions[TRAPEXIT]} )"
ck exittrap-disarmed  1 "TRAPEXIT(){ : }; ( unfunction TRAPEXIT 2>/dev/null; print -r -- \$? )"

echo "== disable crosses with the DEFINITION, not just the bit =="
# `alias -L`, `alias -sL` and the `${(@k)functions}` loop all skip a DISABLED
# node by design, so the state emitted `builtin disable -a -- hi` for an alias
# whose definition it had never emitted -- and the line then failed in the
# child with `no such hash table element`. A disable the parent could undo with
# one `enable` was, in every subshell, an object that no longer existed.
ck disable-alias    "hi='print -r -- A'" "alias hi='print -r -- A'; disable -a hi; print -r -- \$( disable -a )"
ck disable-suffix   "txt='print -r --'" "alias -s txt='print -r --'; disable -s txt; print -r -- \$( disable -s )"
ck disable-function "FN"                'fn(){ print -r -- FN }; disable -f fn; print -r -- $( enable -f fn; fn )'
ck disable-galias   "GG='| cat'"        "alias -g GG='| cat'; disable -a GG; print -r -- \$( disable -a )"
# Two levels down, and with the re-enable in the grandchild.
ck disable-deep     "FN"                'fn(){ print -r -- FN }; disable -f fn; print -r -- $( print -r -- $( enable -f fn; fn ) )'

echo "== disable -p: the pattern characters cross too =="
# `disable -p` has no node in any table -- its state is pattern.c's
# zpc_disables[] -- so nothing emitted it, and a parent that had switched `*`
# off had every subshell switch it back on and expand the word.
ck disable-p-star  "/etc/hos*"  "setopt nonomatch; disable -p '*'; print -r -- \$( print -r -- /etc/hos* )"
ck disable-p-list  "'*'"        "disable -p '*'; print -r -- \$( disable -p )"
ck disable-p-many  "'?' '*'"    "disable -p '*' '?'; print -r -- \$( disable -p )"
ck disable-p-hash  "no"         "setopt extendedglob; disable -p '#'; print -r -- \$( [[ aab == a#b ]] && print -r -- yes || print -r -- no )"

echo "== getopts keeps its place inside a clustered option =="
# optcind is how far getopts has read into `-abc`, and it is a bare int in
# builtin.c with no parameter to write it into -- so the child restarted the
# cluster and handed back an option the parent had already consumed.
ck getopts-cluster  "b" 'set -- -abc x; getopts abc o >/dev/null; print -r -- $( getopts abc p >/dev/null; print -r -- $p )'
ck getopts-third    "c" 'set -- -abc x; getopts abc o >/dev/null; getopts abc o >/dev/null; print -r -- $( getopts abc p >/dev/null; print -r -- $p )'
ck getopts-optarg   "b/val" 'set -- -ab val; getopts "ab:" o >/dev/null; print -r -- $( getopts "ab:" p >/dev/null; print -r -- "$p/$OPTARG" )'
ck getopts-pipe     "b" 'set -- -abc x; getopts abc o >/dev/null; { getopts abc p >/dev/null; print -r -- $p } | cat'

echo "== bindkey keymaps cross, as a difference from the defaults =="
# Widgets crossed (`zle -lL`) but keymaps did not, so a key the parent REBOUND
# read as its default in the child. Replaying `bindkey -L` would have cost 400
# lines and 14 KB on every subshell of any shell that has loaded zle, nearly
# all of it default -- so the parent snapshots the keymaps when zle creates
# them and emits only what has changed since. A shell that bound nothing pays
# nothing, which is what bindkey-quiet measures.
ck bindkey-rebound  '"^X^T" beep'  'zmodload zsh/zle; bindkey -e; bindkey "^X^T" beep; print -r -- $( bindkey "^X^T" )'
ck bindkey-widget   '"^X^Q" w'     'zmodload zsh/zle; bindkey -e; w(){ : }; zle -N w; bindkey "^X^Q" w; print -r -- $( bindkey "^X^Q" )'
ck bindkey-newmap   '"a" beep'     'zmodload zsh/zle; bindkey -N mymap; bindkey -M mymap a beep; print -r -- $( bindkey -M mymap a )'
ck bindkey-removed  '"^A" undefined-key' 'zmodload zsh/zle; bindkey -e; bindkey -r "^A"; print -r -- $( bindkey "^A" )'
ck bindkey-string   '"^X^S" "hi"'  'zmodload zsh/zle; bindkey -e; bindkey -s "^X^S" hi; print -r -- $( bindkey "^X^S" )'
ck bindkey-alias    '"a" beep'     'zmodload zsh/zle; bindkey -N m1; bindkey -M m1 a beep; bindkey -A m1 m2; print -r -- $( bindkey -M m2 a )'
ck bindkey-deep     '"^X^T" beep'  'zmodload zsh/zle; bindkey -e; bindkey "^X^T" beep; print -r -- $( print -r -- $( bindkey "^X^T" ) )'
# The cost, measured rather than asserted: with zle loaded and nothing rebound
# the state carries no bindkey lines at all, and one rebind carries one line.
ck bindkey-cost-0   "0" 'zmodload zsh/zle; bindkey -e; export AOK_ZSH_DUMP_STATE=1; { v=$(true) } 2>/tmp/aokbk; unset AOK_ZSH_DUMP_STATE; print -r -- $(grep -c "^builtin bindkey" /tmp/aokbk)'
ck bindkey-cost-1   "1" 'zmodload zsh/zle; bindkey -e; bindkey "^X^T" beep; export AOK_ZSH_DUMP_STATE=1; { v=$(true) } 2>/tmp/aokbk; unset AOK_ZSH_DUMP_STATE; print -r -- $(grep -c "^builtin bindkey" /tmp/aokbk)'

echo "== user math functions cross, and still evaluate in the child =="
# Reported as a re-launch defect: `functions -M mf 0 0 impl; echo $(( mf() ))'
# answers 0 in the parent and 1 in a re-launched child. Real, but NOT a fork
# bug -- it reproduces with a registration made entirely INSIDE the child, and
# in that shape the implementation never produces a return value at all, so
# what zsh answers is unspecified: /bin/zsh 5.9 gives 4621819117588971520 for
# `mf() + 10' (the bit pattern of the double 10.0) and `0.' for `2 * mf()'.
# Both shells agree exactly on the arity table in the parent -- 0 0 -> 0,
# 0 1 -> 0, 1 1 -> 1 -- so there is nothing here to assert but the supported
# usage, which is what these cases pin.
ck mathfn-crosses   "25"  'sq() { REPLY=$(( $1 * $1 )) }; functions -M msq 1 1 sq; print $( print $(( msq(5) )) )'
ck mathfn-in-expr   "26"  'sq() { REPLY=$(( $1 * $1 )) }; functions -M msq 1 1 sq; print $( print $(( msq(5) + 1 )) )'
ck mathfn-two-arg   "5"   'add() { REPLY=$(( $1 + $2 )) }; functions -M madd 2 2 add; print $( print $(( madd(2,3) )) )'
ck mathfn-nested    "9"   'sq() { REPLY=$(( $1 * $1 )) }; add() { REPLY=$(( $1 + $2 )) }; functions -M msq 1 1 sq; functions -M madd 2 2 add; print $( print $(( msq(madd(1,2)) )) )'
ck mathfn-pipe      "25"  'sq() { REPLY=$(( $1 * $1 )) }; functions -M msq 1 1 sq; print $(( msq(5) )) | cat'

echo "== zstyle patterns are compiled EAGERLY, so they must be replayed late =="
# A zstyle's context pattern is compiled at `zstyle' time, not at first use, so
# replaying the `zstyle -L' lines inline compiled them under the zsh -f defaults
# the child still had. Every one of these answered MISS where zsh 5.9 answers.
ck zstyle-extglob  "v1" 'setopt extendedglob; zstyle ":t:a#" s v1; print $( unset v; zstyle -s ":t:aaa" s v; print -n ${v:-MISS} )'
ck zstyle-sub      "v1" 'setopt extendedglob; zstyle ":t:a#" s v1; ( unset v; zstyle -s ":t:aaa" s v; print -n ${v:-MISS} )'
ck zstyle-pipe     "v1" 'setopt extendedglob; zstyle ":t:a#" s v1; { unset v; zstyle -s ":t:aaa" s v; print -n ${v:-MISS} } | cat'
ck zstyle-kshglob  "v2" 'setopt kshglob; zstyle ":t:@(a|b)" s v2; print $( unset v; zstyle -s ":t:a" s v; print -n ${v:-MISS} )'
# The default-syntax patterns always crossed; they are here so a regression in
# the deferral shows up as a difference between these and the ones above.
ck zstyle-plain    "v3" 'zstyle ":t:a*" s v3; print $( unset v; zstyle -s ":t:axx" s v; print -n ${v:-MISS} )'
ck zstyle-count    "2"  'zstyle ":a:*" s1 x; zstyle ":b:*" s2 y; print $( zstyle -L | wc -l | tr -d " " )'
# A shell that has never used zstyle must not pay for any of it.
# The bracket keeps this case from matching ITSELF: the shell serialises its own
# -c text into ZSH_EXECUTION_STRING, so a literal name here appears in the dump
# and the first version of this case counted its own command line.
ck zstyle-no-cost  "0"  'export AOK_ZSH_DUMP_STATE=1; { v=$(true) } 2>/tmp/aokzs; unset AOK_ZSH_DUMP_STATE; print -r -- $(grep -c "__aok_zs[t]" /tmp/aokzs)'

echo
echo "  passed=$pass failed=$fail"
[ "$fail" -eq 0 ]
