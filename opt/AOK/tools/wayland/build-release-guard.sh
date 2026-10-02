#!/bin/sh
# Build ish_wl_release_guard (see the .c) in the guest and install it where
# start-wayland.sh finds it: /usr/local/lib/ish-wayland by default, or the
# directory given. Needs only a C compiler: the libwayland ABI it uses is
# declared in the source.
set -e
DIR="$(dirname "$0")"
DEST="${1:-/usr/local/lib/ish-wayland}"
mkdir -p "$DEST"
cc -O2 -fPIC -shared -o "$DEST/libish-wl-release-guard.so.tmp" "$DIR/ish_wl_release_guard.c" -ldl -lpthread
mv "$DEST/libish-wl-release-guard.so.tmp" "$DEST/libish-wl-release-guard.so"
echo "installed: $DEST/libish-wl-release-guard.so"
