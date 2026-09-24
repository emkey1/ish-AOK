#!/bin/sh
# Build the iSH pixman-accelerator LD_PRELOAD shim in-guest and install it so
# the Wayland Display session picks it up automatically (start-wayland.sh
# checks for the installed .so and exports LD_PRELOAD when present).
#
# Requires: a C compiler. libpixman-1 itself (the runtime .so this shim
# interposes) is already a transitive dependency of cairo/GTK, so it's
# present on any rootfs that can run the Wayland stack at all. The system's
# pixman-1 development headers are used when installed (Alpine: pixman-dev;
# Debian/Devuan: libpixman-1-dev; Arch: pixman); otherwise the copy of
# pixman.h vendored beside the shim is, which the shim's include falls back
# to by itself.
set -e
DIR="$(dirname "$0")"
SRC="$DIR/ish_pixman_shim.c"
DEST="${1:-/usr/local/lib/ish-pixman}"
mkdir -p "$DEST"
cc -O2 -fPIC -shared -o "$DEST/libish-pixman.so" "$SRC" -I/usr/include/pixman-1 -ldl -lpthread
echo "installed shim: $DEST/libish-pixman.so"
echo "start-wayland.sh picks this up automatically on the next session."
