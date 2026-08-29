#!/bin/sh
# build.sh -- build (and optionally install) ktop from the read-only source
# shipped at /AOK/tools/ktop.
#
# /AOK is a read-only mount, so this copies the source to a writable work
# directory first (default /tmp/ktop-build), builds it there with make, and
# leaves the resulting binary at $WORK_DIR/ktop. Pass "install" to also copy
# it to $PREFIX/bin/ktop (default /usr/local/bin) so it's on PATH.
#
# Usage:
#   sh /AOK/tools/ktop/build.sh              # build only
#   sh /AOK/tools/ktop/build.sh install       # build + install
#
# Environment:
#   WORK_DIR   Work directory. Default: /tmp/ktop-build
#   PREFIX     Install prefix (used with "install"). Default: /usr/local
#   CC         Compiler passed to make. Default: cc
set -eu

SRC_DIR=$(dirname "$0")
WORK_DIR="${WORK_DIR:-/tmp/ktop-build}"
PREFIX="${PREFIX:-/usr/local}"
CC="${CC:-cc}"

mkdir -p "$WORK_DIR"
# The sources on /AOK are read-only, so the copies land read-only too and a
# SECOND run's cp cannot overwrite them: "cp: cannot create regular file
# '/tmp/ktop-build/ktop.c': Permission denied", and set -e stops there. That
# broke the two-step this script's own help suggests -- build once to try it,
# then run again with "install" -- so an already-installed ktop could not be
# replaced without deleting the work directory by hand. Remove first, and make
# the copies writable.
rm -f "$WORK_DIR/ktop.c" "$WORK_DIR/Makefile"
cp "$SRC_DIR/ktop.c" "$SRC_DIR/Makefile" "$WORK_DIR/"
chmod u+w "$WORK_DIR/ktop.c" "$WORK_DIR/Makefile"

echo "Building ktop in $WORK_DIR ..."
make -C "$WORK_DIR" CC="$CC"

echo "Built: $WORK_DIR/ktop"

if [ "${1:-}" = install ]; then
    # $PREFIX/bin is root-owned on every normal root, so an ordinary user got
    # a bare "make: *** [install] Error 1" here with nothing saying why or
    # what to do about it. Say it plainly instead, and do NOT reach for sudo
    # on the user's behalf.
    if [ "$(id -u)" != 0 ] && [ ! -w "$PREFIX/bin" ]; then
        echo "ktop: $PREFIX/bin is not writable by uid $(id -u)." >&2
        echo "The build succeeded -- run it from $WORK_DIR/ktop, or install with:" >&2
        echo "  sudo make -C $WORK_DIR install PREFIX=$PREFIX" >&2
        exit 1
    fi
    echo "Installing to $PREFIX/bin/ktop ..."
    make -C "$WORK_DIR" install PREFIX="$PREFIX"
    echo "Installed. Run: ktop"
else
    echo "Run it directly: $WORK_DIR/ktop"
    echo "Or install it to PATH: sh $0 install"
fi
