#!/bin/sh
# setup-wayfire.sh
# ---------------------------------------------------------------------------
# Install Wayfire, a Wayland desktop composited on the device's GPU, and make
# it the desktop the Wayland applet starts. Installs:
#   wayfire   - the compositor: wobbly windows, a desktop cube, expo (every
#               desktop at once), animations
#   wf-shell  - its panel (wf-panel), wallpaper (wf-background) and dock
#   wcm       - the Wayfire Config Manager, a settings window for all of it
#   kclock    - KDE's Clock (time zones, timers, stopwatch, alarms), in the
#               panel's menu
# and, when they are missing, the base desktop (setup-wayland.sh: foot,
# wayvnc, wofi) and the GPU drivers (setup-gpu.sh).
#
# Wayfire draws everything with OpenGL ES, so it needs the GPU: iSH-AOK's
# render node, Mesa's Venus driver and zink, which means an aarch64 or x86_64
# root. And it will not run as root, on any Linux, so the Wayland window must
# open as your own account: Settings > Open Everything as Default User. A
# session without either starts the labwc desktop instead, and says why.
# Wayfire is like labwc in every way the app relies on (the app shows its
# frames directly, and wayvnc still works), so nothing else changes.
#
# Usage:
#   sudo sh /AOK/tools/setup-wayfire.sh               install, and start Wayfire from the next session
#   sudo sh /AOK/tools/setup-wayfire.sh --no-select   install only
#   sudo sh /AOK/tools/select-desktop.sh labwc        back to the default desktop
#
# Devuan 6 (Wayfire 0.9) runs it. Arch Linux ARM packages it (0.11) but its
# Mesa is too new for OpenGL ES on iSH-AOK's GPU (see below), and Alpine does
# not package it; setup-xfce.sh works on both.
# ---------------------------------------------------------------------------
set -u

log()  { printf '\n\033[1;36m==>\033[0m \033[1m%s\033[0m\n' "$*"; }
note() { printf '    %s\n' "$*"; }
die()  { printf 'setup-wayfire.sh: %s\n' "$*" >&2; exit 1; }

SELECT=1
for arg in "$@"; do
    case "$arg" in
        --no-select) SELECT=0 ;;
        -h|--help) sed -n '/^# Usage:/,/^# Devuan/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
        *) die "unknown option '$arg' (try --help)" ;;
    esac
done

[ "$(id -u)" = 0 ] || die "must run as root:  sudo sh $0 $*"

if command -v apk >/dev/null 2>&1 && ! command -v apt-get >/dev/null 2>&1; then
    die "Alpine does not package Wayfire. The Xfce desktop works here: sudo sh /AOK/tools/setup-xfce.sh"
fi
case "$(uname -m)" in
    x86_64|amd64|aarch64|arm64) : ;;
    *) die "Wayfire needs the GPU, and Mesa has no GPU (Venus) driver for $(uname -m) roots. setup-xfce.sh works here." ;;
esac
[ -c /dev/dri/renderD128 ] || die "no /dev/dri/renderD128: this iSH-AOK build has no GPU device, which Wayfire needs -- update the app"

# The base desktop: the terminal, the VNC bridge and the launcher Wayfire's
# Alt+Shift+D opens.
if ! command -v foot >/dev/null 2>&1 || ! command -v wayvnc >/dev/null 2>&1; then
    log "the base Wayland desktop is not installed yet -- running setup-wayland.sh first"
    sh /AOK/tools/setup-wayland.sh || die "setup-wayland.sh failed -- see above"
fi

# The GPU drivers. --check reports without installing.
if ! sh /AOK/tools/setup-gpu.sh --check >/dev/null 2>&1; then
    log "the GPU drivers are not installed yet -- running setup-gpu.sh first"
    sh /AOK/tools/setup-gpu.sh || die "setup-gpu.sh failed, and Wayfire cannot run without the GPU -- see above"
fi

# Wayfire composites with OpenGL ES, which reaches the GPU only through zink,
# and Mesa 25.2 and later refuse to start zink without robustness2's
# nullDescriptor -- which iSH-AOK's GPU (Venus on MoltenVK) does not offer
# yet. Such a root would install Wayfire and then only ever start labwc, so
# find out first. Devuan 6 (Mesa 25.0) works; Arch and Alpine 3.24 ship newer.
gles_renderer=$(LIBGL_ALWAYS_SOFTWARE=0 MESA_LOADER_DRIVER_OVERRIDE=zink \
    eglinfo -B -p surfaceless 2>/dev/null | sed -n 's/^OpenGL ES profile renderer: //p' | head -1)
case "$gles_renderer" in
    *zink*) : ;;
    *) die "Wayfire needs OpenGL ES on the GPU, and this root's Mesa cannot provide it (renderer: ${gles_renderer:-none}). Mesa 25.2 and later need a Vulkan feature iSH-AOK's GPU does not offer yet; Devuan 6 (Mesa 25.0) runs Wayfire. setup-xfce.sh works here." ;;
esac

PKGS="wayfire wf-shell wcm kclock"
# kclock's QML modules. Debian's kclock does not depend on them -- Plasma
# always has them -- and without org.kde.desktop it starts and opens no
# window at all ("module org.kde.desktop is not installed", bip 2026-10-02).
# Arch's packages depend on what they use, so it needs only the style.
CLOCK_DEB="qml6-module-org-kde-desktop qml6-module-org-kde-coreaddons qml6-module-org-kde-kirigamiaddons-delegates qml6-module-org-kde-kirigamiaddons-formcard qml6-module-qtquick-dialogs qml6-module-qtquick-shapes qml6-module-qtquick-templates"
CLOCK_ARCH="qqc2-desktop-style"
if command -v apt-get >/dev/null 2>&1; then
    PKGS="$PKGS $CLOCK_DEB"
    log "Devuan/Debian (apt) detected"
    SOURCES=/etc/apt/sources.list
    if [ -f "$SOURCES" ] && grep -q 'deb\.devuan\.org' "$SOURCES" 2>/dev/null; then
        note "pinning pkgmaster.devuan.org (deb.devuan.org's round-robin can be very slow)"
        sed -i 's/deb\.devuan\.org/pkgmaster.devuan.org/g' "$SOURCES"
    fi
    log "apt-get update"
    apt-get update || die "apt-get update failed -- check network/DNS (guest /etc/resolv.conf)"
    log "installing $PKGS"
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        -o Dpkg::Options::=--force-confdef -o Dpkg::Options::=--force-confold $PKGS \
        || die "apt-get install failed -- see output above"
elif command -v pacman >/dev/null 2>&1; then
    PKGS="$PKGS $CLOCK_ARCH"
    log "Arch (pacman) detected"
    log "pacman -Sy"
    pacman -Sy --noconfirm || die "pacman -Sy failed -- check network/DNS (guest /etc/resolv.conf)"
    log "installing $PKGS"
    pacman -S --needed --noconfirm $PKGS || die "pacman -S failed -- see output above"
else
    die "no supported package manager found (need apt-get or pacman)"
fi

for bin in wayfire wf-panel wf-background; do
    command -v "$bin" >/dev/null 2>&1 || die "install reported success but '$bin' is not on PATH"
done

log "done"
note "Wayfire $(wayfire --version 2>/dev/null | sed -n '1s/[- ].*//p') is installed."
if [ "$SELECT" = 1 ]; then
    sh /AOK/tools/select-desktop.sh wayfire >/dev/null || die "could not select it -- see select-desktop.sh"
    note "The Wayland window starts Wayfire from its next session: close it and open it again."
else
    note "To start it from the next session:  sudo sh /AOK/tools/select-desktop.sh wayfire"
fi
note "Wayfire will not run as root: the Wayland window needs Settings >"
note "Open Everything as Default User turned on, or it opens labwc instead."
note "Keys: Alt+Return terminal, Alt+Shift+D launcher, Alt+Shift+Q close,"
note "      Ctrl+Alt+Left/Right desktops, Alt+Shift+W all desktops (expo),"
note "      Ctrl+Alt+drag turns the desktop cube, Alt+drag moves a window."
note "Settings: wcm. Back to labwc:  sudo sh /AOK/tools/select-desktop.sh labwc"
