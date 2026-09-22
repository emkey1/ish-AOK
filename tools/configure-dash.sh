#!/bin/sh
# Configure dash the way iSH-AOK's native build needs it. Run from deps/dash.
#
#     cd deps/dash && ../../tools/configure-dash.sh
#
# Configure ONLY, then build iSH-AOK. The five generated sources are committed
# in emkey1/dash, and they carry the thread-local conversion; config.h is not,
# and neither is anything else configure writes -- the fork ignores all of it,
# so a configured tree stays clean. Do not follow it with dash's own `make`:
# from a clean checkout it fails (the standalone link lacks aok_fork.c), and on
# the way it can regenerate nodes.c from nodes.c.pat without __thread. CI runs
# exactly this step.
#
# WHY A CONFIGURED TREE AT ALL, rather than compiling the sources straight from
# a pristine checkout: dash generates five of the objects it links --
# builtins.c, init.c, nodes.c, signames.c and syntax.c -- from its own helper
# programs, plus token.h and the BUILT_SOURCES headers. meson does not
# reimplement that; the tree is prepared once with dash's own build and meson
# compiles what comes out. Same arrangement as deps/zsh, and for the same
# reason (see meson.build's note on zsh being a CONFIGURED tree).
#
# NO libedit. dash's line editing is optional and off by default, and it must
# stay off here: iSH-AOK's native programs reach the terminal through the
# shim's tty handling, and a second line editor inside the shell would be
# competing for the same descriptor. The interactive editing a user wants is
# zsh's; dash is here to be /bin/sh.
#
# --disable-fnmatch and --disable-glob are NOT passed. dash's bundled versions
# exist for platforms whose libc ones are broken, and using the shim's means one
# implementation of the pattern rules rather than two that can disagree.
set -e

if [ ! -f ./configure ] || [ ! -f ./src/aok_mksignames.c ]; then
    echo "$0: run this from deps/dash (a tree with aok_mksignames.c in src/)" >&2
    exit 1
fi

# The guard meson also applies, repeated here so the failure lands at the step
# that would bake the file in rather than at the next configure.
if [ -f ./src/mksignames.c ]; then
    echo "$0: src/mksignames.c is present. It is GPL-2+ and its output is" >&2
    echo "    linked; iSH-AOK removed it deliberately. It has probably come" >&2
    echo "    back through a merge from Debian -- remove it again." >&2
    exit 1
fi

./configure "$@"
