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
#   sudo sh /AOK/tools/select-desktop.sh --panel wf-panel|waybar
#                                              Wayfire's panel: wf-shell's own (with
#                                              its menu), or waybar with a wofi launcher
#   sudo sh /AOK/tools/select-desktop.sh --remove NAME
#                                              remove NAME's own packages (see
#                                              own_packages below); what the
#                                              desktops share stays installed
#
# setup-wayfire.sh offers both panels, and a faster wf-panel built from source
# (tools/wayland/build-wf-panel.sh), and sets the panel it installed.
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
# The packages that are a desktop's own, and that --remove takes out: what
# its setup script installed for it alone. What the desktops share -- foot,
# wayvnc, wofi, waybar, fonts, D-Bus, seatd, the GPU drivers, the menu's
# programs -- stays, and so does anything the user installed besides: a
# package goes only if nothing left installed depends on it (apt-get remove
# after a dry run takes nothing else, apk del, pacman --asdeps), and what it
# pulled in goes only if nothing else needs it either. Names are the
# same on Devuan, Alpine and Arch, except Wayfire's clock modules; xfce4 is a
# group on Arch, expanded there.
own_packages() {
    case "$1" in
        labwc) echo "labwc" ;;
        sway) echo "sway" ;;
        xfce) echo "xfce4 xfce4-terminal xfce4-appfinder xfce4-notifyd tumbler" ;;
        wayfire) echo "wayfire wf-shell wcm kclock qqc2-desktop-style" \
            "qml6-module-org-kde-desktop qml6-module-org-kde-coreaddons" \
            "qml6-module-org-kde-kirigamiaddons-delegates qml6-module-org-kde-kirigamiaddons-formcard" \
            "qml6-module-qtquick-dialogs qml6-module-qtquick-shapes qml6-module-qtquick-templates" ;;
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
current_panel() {
    v=$(sed -n 's/^AOK_WAYFIRE_PANEL=//p' "$CONF" 2>/dev/null | tail -n 1)
    case "$v" in waybar) echo waybar ;; *) echo wf-panel ;; esac
}
# Both settings, written together so that changing one keeps the other.
write_conf() {
    {
        echo "# Written by /AOK/tools/select-desktop.sh; start-wayland.sh reads it."
        echo "# One of: labwc sway wayfire xfce"
        echo "AOK_DESKTOP=$1"
        echo "# Wayfire's panel: wf-panel or waybar"
        echo "AOK_WAYFIRE_PANEL=$2"
    } > "$CONF" || die "cannot write $CONF"
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
    panel=$(current_panel)
    case "$panel" in
        wf-panel) [ -x /usr/local/bin/wf-panel ] && panel="wf-panel (the faster build in /usr/local/bin)" ;;
    esac
    echo "Wayfire's panel: $panel"
    if [ -n "${WAYLAND_COMPOSITOR_CMD:-}" ]; then
        echo "WAYLAND_COMPOSITOR_CMD is set ($WAYLAND_COMPOSITOR_CMD), and wins where it is set."
    fi
    exit 0
fi

case "$1" in
    -h|--help) sed -n '/^# Usage:/,/^# The choice/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
esac

# The lines of stdin that are not one of the words in $1 (with or without
# an architecture suffix).
not_in() {
    while read -r p; do
        case " $1 " in
            *" $p "*|*" ${p%%:*} "*) ;;
            *) echo "$p" ;;
        esac
    done
}

# --remove NAME: the desktop's own packages, unwanted; then the choice moves
# to a desktop still installed if it was this one.
remove_desktop() {
    d=$1
    own_packages "$d" >/dev/null || die "unknown desktop '$d' -- choose labwc, sway, wayfire or xfce"
    [ "$(id -u)" = 0 ] || die "must run as root:  sudo sh $0 --remove $d"
    if [ "$d" = labwc ] && command -v xfce4-session >/dev/null 2>&1; then
        die "Xfce runs on labwc, so labwc stays while Xfce is installed -- remove Xfce first"
    fi
    pkgs=
    if command -v apt-get >/dev/null 2>&1; then
        # Each name in every architecture it is installed in (Wayfire goes in
        # as arm64 on other roots).
        for p in $(own_packages "$d"); do
            pkgs="$pkgs $(dpkg-query -W -f '${binary:Package} ${db:Status-Abbrev}\n' "$p" 2>/dev/null \
                | awk '$2 ~ /^ii/ { print $1 }')"
        done
        pkgs=$(echo $pkgs)
        [ -n "$pkgs" ] || die "none of $d's packages are installed"
        # Removed by name, since apt's autoremove keeps whatever another
        # package only suggests (waybar suggests sway) -- but only if that
        # takes nothing else with it: a program that depends on one of them
        # would go too, so then nothing is removed.
        # shellcheck disable=SC2086 # one word per package
        others=$(apt-get -s remove $pkgs 2>/dev/null | sed -n 's/^Remv \([^ ]*\) .*/\1/p' | not_in "$pkgs")
        if [ -n "$others" ]; then
            die "removing $d would also remove $(echo $others), which depend on it -- nothing was removed"
        fi
        echo "==> removing $pkgs"
        # shellcheck disable=SC2086
        DEBIAN_FRONTEND=noninteractive apt-get remove -y $pkgs || die "apt-get remove failed -- see output above"
        echo "==> removing what nothing else needs"
        DEBIAN_FRONTEND=noninteractive apt-get autoremove -y || die "apt-get autoremove failed -- see output above"
    elif command -v apk >/dev/null 2>&1; then
        for p in $(own_packages "$d"); do
            grep -qx "$p" /etc/apk/world 2>/dev/null && pkgs="$pkgs $p"
        done
        pkgs=$(echo $pkgs)
        [ -n "$pkgs" ] || die "none of $d's packages are installed"
        echo "==> removing $pkgs, and what nothing else needs"
        # shellcheck disable=SC2086
        apk del $pkgs || die "apk del failed -- see output above"
    elif command -v pacman >/dev/null 2>&1; then
        for p in $(own_packages "$d"); do
            if pacman -Qq "$p" >/dev/null 2>&1; then
                pkgs="$pkgs $p"
            else
                pkgs="$pkgs $(pacman -Qgq "$p" 2>/dev/null | tr '\n' ' ')"
            fi
        done
        pkgs=$(echo $pkgs)
        [ -n "$pkgs" ] || die "none of $d's packages are installed"
        echo "==> no longer wanted: $pkgs"
        # shellcheck disable=SC2086
        pacman -D --asdeps $pkgs >/dev/null || die "pacman -D failed -- see output above"
        orphans=$(pacman -Qdtq 2>/dev/null)
        if [ -n "$orphans" ]; then
            echo "==> removing what nothing else needs"
            # shellcheck disable=SC2086
            pacman -Rns --noconfirm $orphans || die "pacman -Rns failed -- see output above"
        fi
    else
        die "no supported package manager found (need apt-get, apk or pacman)"
    fi
    if [ "$d" = wayfire ]; then
        # The faster wf-panel setup-wayfire.sh linked or built.
        rm -f /usr/local/bin/wf-panel /usr/local/lib/ish-wayland/wf-panel /usr/local/lib/ish-wayland/wf-panel.source
    fi
    if installed "$d"; then
        echo "$d is still installed: something left installed needs its packages."
        exit 1
    fi
    echo "$d is removed."
    if [ "$(current)" = "$d" ] || ! installed "$(current)"; then
        for next in labwc sway xfce wayfire; do
            installed "$next" || continue
            write_conf "$next" "$(current_panel)"
            echo "The Wayland desktop will start $next from the next session."
            exit 0
        done
        write_conf labwc "$(current_panel)"
        echo "No desktop is left installed; sudo sh /AOK/tools/setup-wayland.sh installs one again."
    fi
    exit 0
}

name=
panel=
while [ $# -gt 0 ]; do
    case "$1" in
        --remove)
            [ $# -eq 2 ] || die "--remove takes one desktop: labwc, sway, wayfire or xfce"
            remove_desktop "$2" ;;
        --remove=*) [ $# -eq 1 ] || die "--remove takes one desktop"; remove_desktop "${1#--remove=}" ;;
        --panel)
            [ $# -ge 2 ] || die "--panel needs wf-panel or waybar"
            panel=$2
            shift 2 ;;
        --panel=*) panel=${1#--panel=}; shift ;;
        -*) die "unknown option '$1' (try --help)" ;;
        *) [ -z "$name" ] || die "one desktop at a time"; name=$1; shift ;;
    esac
done
[ -n "$name$panel" ] || die "nothing to set (try --help)"

if [ -n "$name" ]; then
    needs "$name" >/dev/null 2>&1 || die "unknown desktop '$name' -- choose labwc, sway, wayfire or xfce"
    installed "$name" || die "$name is not installed -- run: sudo sh /AOK/tools/$(setup_script "$name")"
fi
case "$panel" in
    "") ;;
    wf-panel) command -v wf-panel >/dev/null 2>&1 || die "wf-panel is not installed -- run: sudo sh /AOK/tools/setup-wayfire.sh" ;;
    waybar)
        command -v waybar >/dev/null 2>&1 || die "waybar is not installed -- run: sudo sh /AOK/tools/setup-wayfire.sh --panel waybar"
        command -v wofi >/dev/null 2>&1 || die "wofi (its launcher) is not installed -- run: sudo sh /AOK/tools/setup-wayland.sh" ;;
    *) die "unknown panel '$panel' -- choose wf-panel or waybar" ;;
esac
[ "$(id -u)" = 0 ] || die "must run as root:  sudo sh $0 ${name}${panel:+ --panel $panel}"

write_conf "${name:-$(current)}" "${panel:-$(current_panel)}"
[ -n "$name" ] && echo "The Wayland desktop will start $name from the next session."
[ -n "$panel" ] && echo "Wayfire's panel will be $panel from the next session."
echo "Close the Wayland window and open it again to switch."
