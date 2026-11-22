#!/bin/sh
# setup-native-wayland.sh
# ---------------------------------------------------------------------------
# Give a native-mode root a Wayland desktop.
#
# A native root has no distribution, so it has no compositor. Its desktop is
# installed into its companion filesystem -- Devuan 6 arm64, the one apt uses
# too (setup-companion.sh, which this runs first) -- and runs from there under
# emulation, while the booted root stays native. See
# /AOK/docs/native-mode.md.
#
# Run as root, in a native root:
#       sudo sh /AOK/tools/setup-native-wayland.sh
#
# Options:
#   --root NAME    use a Devuan arm64 filesystem that is already installed
#                  (Filesystems, or manage-roots.sh list) as the companion
#   --user LOGIN   give this account the companion; repeatable. The default
#                  is every account here with a uid of 1000 or more
#   --no-desktop   prepare the companion but do not install the desktop into
#                  it (the same as setup-companion.sh)
#   -h, --help     this text
#
# Safe to run again: what is done is kept.
#
# Afterwards: open the Wayland window. More desktops and programs go in the
# same way as on any root -- setup-wayfire.sh, setup-xfce.sh, setup-games.sh
# and select-desktop.sh all run inside the companion from here -- and
# anything else with sudo apt install, here or in the desktop's terminal.
# ---------------------------------------------------------------------------
set -eu

. /AOK/tools/wayland-root.sh

log()  { printf '\n\033[1;36m==>\033[0m \033[1m%s\033[0m\n' "$*"; }
note() { printf '    %s\n' "$*"; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

# The header comment, between its two rules.
usage() { awk '/^# ------/ { n++; next } n == 1 { sub(/^# ?/, ""); print }' "$0"; }

DESKTOP=1
# Everything else is setup-companion.sh's to read.
for arg in "$@"; do
    case "$arg" in
        --no-desktop) DESKTOP=0 ;;
        -h|--help) usage; exit 0 ;;
    esac
done
for arg in "$@"; do
    shift
    [ "$arg" = --no-desktop ] || set -- "$@" "$arg"
done

[ "$(id -u)" = 0 ] || die "needs root: sudo sh $0"
aok_in_native_root || die "this is for a native-mode root. On this one, run: sudo sh /AOK/tools/setup-wayland.sh"

sh /AOK/tools/setup-companion.sh "$@" || exit $?

aok_wayland_root_find || die "$AOK_WAYLAND_PROBLEM"
ROOT_NAME=$(aok_companion_root_name)

if [ "$DESKTOP" = 1 ]; then
    log "Installing the desktop into '$ROOT_NAME' (this is the slow part)"
    aok_companion_env
    "$AOK_WAYLAND_DIR/bin/sh" /AOK/tools/setup-wayland.sh || die "setup-wayland.sh failed inside '$ROOT_NAME' (see above). Run this again to retry; what is done is kept"
    log "Done"
    note "Open the Wayland window to start the desktop."
else
    log "Done"
    note "No desktop was installed. Next: sudo sh /AOK/tools/setup-wayland.sh"
fi
note "More programs: sudo apt install ..."
