#!/bin/sh
# select-desktop.sh
# ---------------------------------------------------------------------------
# Choose which desktop the Wayland applet starts (start-wayland.sh reads the
# choice from /etc/aok-desktop.conf each time the desktop opens):
#
#   labwc    the default: floating windows, right-click menu, waybar panel
#            (setup-wayland.sh)
#   sway     tiling, keyboard-driven (setup-wayland.sh)
#   wayfire  composited on the GPU: wobbly windows, desktop cube, expo,
#            wf-shell panel and dock (setup-wayfire.sh; needs the GPU)
#   xfce     the Xfce 4.20 desktop -- panel, desktop icons, Thunar, settings
#            -- with labwc as its compositor (setup-xfce.sh)
#
# Usage:
#   sh /AOK/tools/select-desktop.sh            show the choice, and what is installed
#   sudo sh /AOK/tools/select-desktop.sh NAME  start NAME from the next session
#
# The choice applies the next time the Wayland window opens; a desktop that is
# open now keeps running as it is. WAYLAND_COMPOSITOR_CMD, set in the
# environment start-wayland.sh runs in, still overrides it.
# ---------------------------------------------------------------------------
set -u

CONF=/etc/aok-desktop.conf

die() { printf 'select-desktop.sh: %s\n' "$*" >&2; exit 1; }

# The programs a desktop needs before it can be chosen.
needs() {
    case "$1" in
        labwc) echo "labwc" ;;
        sway) echo "sway" ;;
        wayfire) echo "wayfire" ;;
        xfce) echo "labwc xfce4-session xfce4-panel xfdesktop" ;;
        *) return 1 ;;
    esac
}
setup_script() {
    case "$1" in
        labwc|sway) echo setup-wayland.sh ;;
        *) echo "setup-$1.sh" ;;
    esac
}
installed() {
    for bin in $(needs "$1") foot wayvnc; do
        command -v "$bin" >/dev/null 2>&1 || return 1
    done
}
current() {
    v=$(sed -n 's/^AOK_DESKTOP=//p' "$CONF" 2>/dev/null | tail -n 1)
    needs "$v" >/dev/null 2>&1 && echo "$v" || echo labwc
}

if [ $# -eq 0 ]; then
    cur=$(current)
    echo "The Wayland desktop starts: $cur"
    for d in labwc sway wayfire xfce; do
        if installed "$d"; then state=installed; else state="not installed (sudo sh /AOK/tools/$(setup_script "$d"))"; fi
        mark=" "
        [ "$d" = "$cur" ] && mark="*"
        printf '  %s %-8s %s\n' "$mark" "$d" "$state"
    done
    if [ -n "${WAYLAND_COMPOSITOR_CMD:-}" ]; then
        echo "WAYLAND_COMPOSITOR_CMD is set ($WAYLAND_COMPOSITOR_CMD), and wins where it is set."
    fi
    exit 0
fi

case "$1" in
    -h|--help) sed -n '/^# Usage:/,/^# The choice/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
esac

name=$1
needs "$name" >/dev/null 2>&1 || die "unknown desktop '$name' -- choose labwc, sway, wayfire or xfce"
installed "$name" || die "$name is not installed -- run: sudo sh /AOK/tools/$(setup_script "$name")"
[ "$(id -u)" = 0 ] || die "must run as root:  sudo sh $0 $name"

{
    echo "# Written by /AOK/tools/select-desktop.sh; start-wayland.sh reads it."
    echo "# One of: labwc sway wayfire xfce"
    echo "AOK_DESKTOP=$name"
} > "$CONF" || die "cannot write $CONF"
echo "The Wayland desktop will start $name from the next session."
echo "Close the Wayland window and open it again to switch."
