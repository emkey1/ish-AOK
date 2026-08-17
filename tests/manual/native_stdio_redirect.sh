#!/bin/sh
# native_stdio_redirect.sh -- a native program's buffered stdout must survive a
# redirection. Run inside the guest (device or CLI root):
#
#   /AOK/tests/native_stdio_redirect.sh
#
# WHAT BROKE. `zsh -f -c 'printf plain > file'` wrote an EMPTY file, and the
# bytes were not misdirected either -- nothing reached the terminal. `print` and
# `echo` through the same redirection were fine, which is what made it look like
# a printf bug; all three are one C function (bin_print).
#
# The cause was in the shim, not in zsh. kernel/native_libc.c built every FILE
# with funopen() and handed it BOTH a read and a write callback regardless of
# how the stream had been opened, so stdio marked each one __SRW -- "read/write,
# direction not yet decided" -- and left __SWR clear until the first write went
# through __swsetup(). fflush() is __sflush(), which opens with
#
#     if ((flags & __SWR) == 0) return 0;
#
# so a stream stdio does not believe has been written to cannot be flushed, and
# says it succeeded. Everything that writes calls __swsetup first EXCEPT putc()'s
# inline fast path, which stores straight into the buffer whenever _w > 0 -- and
# fpurge() sets _w to the buffer size without setting __SWR. zsh's redup() calls
# fpurge(stdout) on every redirection of fd 1, printf emits its literal text with
# putc(), and print reaches the buffer through fwrite(), which does call
# __swsetup. Hence one working and the other not.
#
# WHY THESE CASES. Every form below reaches the same fpurge-then-putc window by a
# different route, and each one was silently losing its output:
#
#   gt/ap/br   the ordinary redirections, including a compound command
#   e2         stderr, which has its own wrapper and its own fpurge in redup()
#   nl         a trailing newline, which does NOT rescue it -- zsh's init.c makes
#              stdout fully buffered with its own buffer, so nothing flushes on
#              '\n'. Anyone reaching for line buffering as the explanation should
#              look at this case first.
#   fmt        a real conversion, so the fprintf path is covered next to putc's
#   ex         `exec 1>file`, where the redirection outlives the command
#
# ORDER MATTERS, and not for style. __SWR, once set, stays set for the life of
# the stream, so the first case that reaches stdout through fwrite or fprintf
# cures every case after it within the same shell -- against the bug this exists
# to catch, `fmt` is that case. It is therefore placed last but one, after every
# putc-only case; `ex` has to come last because it redirects the rest of the
# script, so it rides on fmt's __SWR and is a coverage case rather than a
# diagnostic one. Moving fmt earlier does not break the test, it QUIETLY BLINDS
# it: on the build this was written against, `fmt` first turns five failures
# into zero. Standalone, one form per shell invocation, all seven fail.
#
# ONE INVOCATION PER SHELL, deliberately. A native program is a C function on a
# guest task's thread and its globals persist for the life of the app, so a
# second zsh (or bash) in the same session runs on the first one's state and
# warns that it will misbehave. Every case therefore rides in a single -c script,
# and `exec 1>` comes last because it redirects the rest of it.
#
# A shell that is not compiled into this build leaves its /AOK/native entry as a
# placeholder that exits 127; that is reported as SKIP, not failure.

set -u

dir=${TMPDIR:-/tmp}/native-stdio-redirect.$$
rm -rf "$dir"
mkdir -p "$dir" || { echo "cannot create $dir"; exit 1; }
trap 'rm -rf "$dir"' EXIT INT TERM

fail=0
ran=0

check() { # check <shell> <case> <expected>
    got=$(cat "$dir/$1.$2" 2>/dev/null)
    if [ "$got" = "$3" ]; then
        printf 'PASS  %-5s %-4s [%s]\n' "$1" "$2" "$got"
    else
        printf 'FAIL  %-5s %-4s expected [%s], got [%s]\n' "$1" "$2" "$3" "$got"
        fail=$((fail + 1))
    fi
}

check_all() { # check_all <shell>
    check "$1" gt  'gt'
    check "$1" ap  'ap'
    check "$1" br  'br'
    check "$1" e2  'e2'
    check "$1" nl  'nl'
    check "$1" fmt 'f-7'
    check "$1" ex  'ex'
    ran=$((ran + 1))
}

# Availability is decided AFTER the fact, from what the run left behind, because
# a probe run would itself be the one invocation this shell gets. When the
# program is not compiled in, /AOK/native/<name> is still a file -- a placeholder
# script that says so on stderr and creates nothing -- so reading it cannot tell
# the two apart, but its output can.
verdict() { # verdict <shell>
    if [ ! -e "/AOK/native/$1" ]; then
        printf 'SKIP  %-5s (no /AOK/native/%s in this build)\n' "$1" "$1"
    elif grep -q 'native dispatch unavailable' "$dir/$1.err" 2>/dev/null; then
        printf 'SKIP  %-5s (not compiled into this build)\n' "$1"
    else
        check_all "$1"
    fi
}

# The bodies differ only in zsh's `{ ... }` not needing the trailing semicolon
# bash requires; keeping them separate beats a shared string with a substitution
# in it, because the whole point is to run each shell's own parser.
if [ -e /AOK/native/zsh ]; then
    /AOK/native/zsh -f -c "
        printf gt    > $dir/zsh.gt
        printf ap   >> $dir/zsh.ap
        { printf br } > $dir/zsh.br
        { printf e2 >&2 } 2> $dir/zsh.e2
        printf 'nl\n' > $dir/zsh.nl
        printf '%s-%d\n' f 7 > $dir/zsh.fmt
        exec 1> $dir/zsh.ex
        printf ex
    " 2> "$dir/zsh.err"
fi
verdict zsh

if [ -e /AOK/native/bash ]; then
    /AOK/native/bash --norc -c "
        printf gt    > $dir/bash.gt
        printf ap   >> $dir/bash.ap
        { printf br ; } > $dir/bash.br
        { printf e2 >&2 ; } 2> $dir/bash.e2
        printf 'nl\n' > $dir/bash.nl
        printf '%s-%d\n' f 7 > $dir/bash.fmt
        exec 1> $dir/bash.ex
        printf ex
    " 2> "$dir/bash.err"
fi
verdict bash

if [ "$ran" -eq 0 ]; then
    echo 'no native shell in this build -- nothing tested'
    exit 0
fi
if [ "$fail" -ne 0 ]; then
    echo "$fail case(s) failed"
    exit 1
fi
echo 'all cases passed'
