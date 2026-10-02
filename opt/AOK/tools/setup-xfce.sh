#!/bin/sh
# setup-xfce.sh
# ---------------------------------------------------------------------------
# Install the Xfce 4.20 desktop and make it the desktop the Wayland applet
# starts. Xfce runs as a Wayland session with labwc as its compositor, the way
# `startxfce4 --wayland` runs it: Xfce's panel (applications menu, desktops,
# clock), its desktop with icons and wallpaper, the Thunar file manager and the
# settings manager, with labwc drawing the windows. (The desktop's wallpaper
# does not draw yet: xfdesktop paints no backdrop on this headless output, so
# the desktop is black behind its icons -- see docs/TODO.md.) Installs:
#   xfce4            - the session, panel, desktop, Thunar and settings
#   xfce4-terminal   - Xfce's terminal (Alt+Return, and the dock's)
#   xfce4-appfinder  - the application finder (Alt+Shift+D)
#   xfce4-notifyd    - notifications
#   an SVG loader    - many of Xfce's icons, and its wallpapers, are SVG
# and, when it is missing, the base desktop (setup-wayland.sh: labwc, foot,
# wayvnc). Xfce's own window manager, xfwm4, is X11-only and is not used.
#
# The GPU is not required. With it (setup-gpu.sh) labwc composites on the GPU
# and the app shows its frames directly, as for the plain labwc desktop.
#
# Usage:
#   sudo sh /AOK/tools/setup-xfce.sh               install, and start Xfce from the next session
#   sudo sh /AOK/tools/setup-xfce.sh --no-select   install only
#   sudo sh /AOK/tools/select-desktop.sh labwc     back to the default desktop
#
# Devuan 6, Alpine and Arch Linux ARM all carry Xfce 4.20.
# ---------------------------------------------------------------------------
set -u

log()  { printf '\n\033[1;36m==>\033[0m \033[1m%s\033[0m\n' "$*"; }
note() { printf '    %s\n' "$*"; }
die()  { printf 'setup-xfce.sh: %s\n' "$*" >&2; exit 1; }

# apk add, downloading every package before installing any: apk-tools 3
# streams packages into extraction, and a mirror drops a reader stalled for a
# few minutes (see setup-wayland.sh, which this copies).
apk_add() {
    _apk_opts=""
    _apk_purge=""
    case "$(apk --version 2>/dev/null)" in
        "apk-tools 3."*)
            _apk_opts="--cache-predownload"
            [ -e /etc/apk/cache ] || _apk_purge=/var/cache/apk
            ;;
    esac
    _apk_try=1
    while :; do
        apk add $_apk_opts "$@"
        _apk_rc=$?
        [ "$_apk_rc" -eq 0 ] && break
        [ "$_apk_try" -ge 3 ] && break
        _apk_try=$((_apk_try + 1))
        note "apk add failed; trying again ($_apk_try of 3)"
    done
    [ -n "$_apk_purge" ] && rm -f "$_apk_purge"/*.apk
    return "$_apk_rc"
}

SELECT=1
for arg in "$@"; do
    case "$arg" in
        --no-select) SELECT=0 ;;
        -h|--help) sed -n '/^# Usage:/,/^# Devuan/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
        *) die "unknown option '$arg' (try --help)" ;;
    esac
done

[ "$(id -u)" = 0 ] || die "must run as root:  sudo sh $0 $*"

# The base desktop: labwc, which is Xfce's compositor here, and the terminal
# and VNC bridge every desktop session starts.
if ! command -v labwc >/dev/null 2>&1 || ! command -v foot >/dev/null 2>&1 \
        || ! command -v wayvnc >/dev/null 2>&1; then
    log "the base Wayland desktop is not installed yet -- running setup-wayland.sh first"
    sh /AOK/tools/setup-wayland.sh || die "setup-wayland.sh failed -- see above"
fi

if command -v apt-get >/dev/null 2>&1; then
    log "Devuan/Debian (apt) detected"
    SOURCES=/etc/apt/sources.list
    if [ -f "$SOURCES" ] && grep -q 'deb\.devuan\.org' "$SOURCES" 2>/dev/null; then
        note "pinning pkgmaster.devuan.org (deb.devuan.org's round-robin can be very slow)"
        sed -i 's/deb\.devuan\.org/pkgmaster.devuan.org/g' "$SOURCES"
    fi
    log "apt-get update"
    apt-get update || die "apt-get update failed -- check network/DNS (guest /etc/resolv.conf)"
    # librsvg2-common and tumbler are only Recommends of xfdesktop4, which
    # --no-install-recommends skips: the SVG loader draws Xfce's SVG icons and
    # wallpapers, and tumbler makes the file manager's thumbnails.
    PKGS="xfce4 xfce4-terminal xfce4-appfinder xfce4-notifyd librsvg2-common tumbler xdg-user-dirs"
    log "installing $PKGS"
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        -o Dpkg::Options::=--force-confdef -o Dpkg::Options::=--force-confold $PKGS \
        || die "apt-get install failed -- see output above"
elif command -v pacman >/dev/null 2>&1; then
    log "Arch (pacman) detected"
    log "pacman -Sy"
    pacman -Sy --noconfirm || die "pacman -Sy failed -- check network/DNS (guest /etc/resolv.conf)"
    # xfce4 is a group; --noconfirm takes all of it.
    PKGS="xfce4 xfce4-notifyd librsvg"
    log "installing $PKGS"
    pacman -S --needed --noconfirm $PKGS || die "pacman -S failed -- see output above"
elif command -v apk >/dev/null 2>&1; then
    log "Alpine (apk) detected"
    PKGS="xfce4 xfce4-terminal xfce4-appfinder xfce4-notifyd adwaita-icon-theme librsvg font-dejavu"
    log "installing $PKGS"
    apk_add $PKGS || die "apk add failed -- see output above"
else
    die "no supported package manager found (need apt-get, pacman, or apk)"
fi

for bin in xfce4-session xfce4-panel xfdesktop xfsettingsd; do
    command -v "$bin" >/dev/null 2>&1 || die "install reported success but '$bin' is not on PATH"
done
# Xfce's Wayland session needs labwc's --session (0.7.2 and later).
labwc --help 2>&1 | grep -q -- '--session' \
    || die "this labwc has no --session option (it needs 0.7.2 or later), which Xfce's Wayland session uses"

log "done"
note "Xfce is installed."
if [ "$SELECT" = 1 ]; then
    sh /AOK/tools/select-desktop.sh xfce >/dev/null || die "could not select it -- see select-desktop.sh"
    note "The Wayland window starts Xfce from its next session: close it and open it again."
else
    note "To start it from the next session:  sudo sh /AOK/tools/select-desktop.sh xfce"
fi
note "Keys: Alt+Return terminal, Alt+Shift+D app finder, Alt+Shift+Q close,"
note "      Ctrl+Alt+Left/Right desktops, Alt+Shift+E ends the session."
note "Back to labwc:  sudo sh /AOK/tools/select-desktop.sh labwc"
