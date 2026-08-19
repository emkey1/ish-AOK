#!/bin/sh
# Run bash's mkbuiltins and put its output where meson asked for it.
#
# mkbuiltins has no -o: it derives the output name from the input and writes it
# into the current directory. meson needs the file at a path it chose, and runs
# every custom_target from the build root -- so several .def files generating at
# once would race to write into the same directory. Each invocation therefore
# gets a scratch directory of its own and the result is moved into place.
#
# Two modes, matching the two things bash's own Makefile uses mkbuiltins for:
#
#   one   <mkbuiltins> <defdir> <input.def> <output.c>
#         the per-builtin C file, from the .def.c rule
#   table <mkbuiltins> <defdir> <output-builtins.c> <output-builtext.h> <defs...>
#         the dispatch table and its externs, from the builtins.c rule
#
# -D <defdir> tells mkbuiltins where the .def files live, which is how it finds
# the sources when the build tree is not the source tree.
set -eu

# meson hands paths relative to the build directory, and everything below runs
# from a scratch directory, so they have to be resolved first -- otherwise
# ../deps/bash/... is looked up one level above the wrong place.
abspath() {
    case "$1" in
    /*) printf '%s\n' "$1" ;;
    *)  printf '%s/%s\n' "$PWD" "$1" ;;
    esac
}

mode=$1; mkbuiltins=$(abspath "$2"); defdir=$3; shift 3

scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT

case "$mode" in
one)
    input=$(abspath "$1"); output=$(abspath "$2")
    base=$(basename "$input" .def)
    (cd "$scratch" && "$mkbuiltins" -D "$defdir" "$input")
    mv "$scratch/$base.c" "$output"
    ;;
table)
    out_c=$(abspath "$1"); out_h=$(abspath "$2"); shift 2
    # The .def arguments are relative too. Rotated through the positional
    # parameters rather than joined into a string and re-split with
    # `set -- $defs`: word splitting destroys any path containing a space, and
    # $PWD is the build directory -- which is under "Application Support"
    # whenever DerivedData is pointed there, as a headless simulator build
    # does. That failed with "/Users/mke/Library/Application: No such file or
    # directory" and nothing naming the script.
    count=$#
    while [ "$count" -gt 0 ]; do
        d=$1; shift
        set -- "$@" "$(abspath "$d")"
        count=$((count - 1))
    done
    # -noproduction: emit only the table and the externs, not the per-builtin
    # files, which the `one` mode above generates individually.
    (cd "$scratch" && "$mkbuiltins" -externfile builtext.h -structfile builtins.c \
        -noproduction -D "$defdir" "$@")
    mv "$scratch/builtins.c" "$out_c"
    mv "$scratch/builtext.h" "$out_h"
    ;;
*)
    echo "run-mkbuiltins.sh: unknown mode '$mode'" >&2
    exit 2
    ;;
esac
