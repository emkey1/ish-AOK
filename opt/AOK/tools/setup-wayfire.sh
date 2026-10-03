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
#   sudo sh /AOK/tools/setup-wayfire.sh --panel P     and the panel: P is one of
#       fast    wf-panel with a menu that reopens instead of rebuilding itself
#               (tools/wayland/build-wf-panel.sh). The packaged wf-panel
#               takes 3-6 s to open its menu on an A10X iPad, this one
#               0.7 s. Prebuilt in /AOK/bundled for Devuan 6 on arm64 (the
#               fastest guest) and linked; built from source elsewhere, or
#               for a wf-shell the bundled one does not match.
#       waybar  waybar, with an Apps button that opens wofi's application
#               list, as the labwc desktop's panel is. No build.
#       both    the two, starting with fast; swap whenever you like with
#               select-desktop.sh --panel wf-panel|waybar
#       stock   the packaged wf-panel as it is
#     Asked when run in a terminal without --panel; stock otherwise.
#   sudo sh /AOK/tools/select-desktop.sh --panel waybar   change the panel later
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
PANEL=
while [ $# -gt 0 ]; do
    case "$1" in
        --no-select) SELECT=0 ;;
        --panel) [ $# -ge 2 ] || die "--panel needs fast, waybar, both or stock"; PANEL=$2; shift ;;
        --panel=*) PANEL=${1#--panel=} ;;
        -h|--help) sed -n '/^# Usage:/,/^# Devuan/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
        *) die "unknown option '$1' (try --help)" ;;
    esac
    shift
done
case "$PANEL" in
    ""|fast|waybar|both|stock) ;;
    *) die "unknown panel '$PANEL' -- choose fast, waybar, both or stock" ;;
esac

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

# The panel. Asked in a terminal; without one, and without --panel, the
# packaged wf-panel, as before.
if [ -z "$PANEL" ]; then
    PANEL=stock
    if [ -t 0 ] && [ -t 1 ]; then
        echo
        echo "Which panel should Wayfire have?"
        echo "  1) fast    wf-panel whose menu reopens at once (prebuilt; the packaged"
        echo "             one takes seconds per open on older iPads)"
        echo "  2) waybar  waybar with an Apps button opening wofi (no build)"
        echo "  3) both    fast and waybar; swap any time with"
        echo "             sudo sh /AOK/tools/select-desktop.sh --panel wf-panel|waybar"
        echo "  4) stock   the packaged wf-panel as it is"
        printf 'Choose 1-4 [4]: '
        read -r answer || answer=
        case "$answer" in
            1|fast) PANEL=fast ;;
            2|waybar) PANEL=waybar ;;
            3|both) PANEL=both ;;
            *) PANEL=stock ;;
        esac
    fi
fi
case "$PANEL" in
    waybar|both)
        if ! command -v waybar >/dev/null 2>&1 || ! command -v wofi >/dev/null 2>&1; then
            log "installing waybar and wofi"
            if command -v apt-get >/dev/null 2>&1; then
                DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends waybar wofi fonts-font-awesome
            else
                pacman -S --needed --noconfirm waybar wofi otf-font-awesome
            fi || die "could not install waybar -- see above"
        fi ;;
esac
# The faster wf-panel comes prebuilt in /AOK/bundled for Devuan 6's wf-shell
# on arm64 and is linked in; other architectures, and a wf-shell it was not
# built for, compile it here (which needs a lot of memory -- an A10X iPad ran
# out). x86_64 is not bundled: arm64 is the guest that runs fastest, and
# Wayland on an x86_64 root is the exception.
case "$(uname -m)" in
    aarch64|arm64) bundled_panel=/AOK/bundled/devuan6-aarch64/wf-panel ;;
    *) bundled_panel= ;;
esac
case "$PANEL" in
    fast|both)
        wf_shell_version=$(dpkg-query -W -f '${Version}' wf-shell 2>/dev/null) || wf_shell_version=
        if [ -n "$bundled_panel" ] && [ -x "$bundled_panel" ] && [ -n "$wf_shell_version" ] \
                && [ "$(cat "$bundled_panel.source" 2>/dev/null)" = "$wf_shell_version" ]; then
            log "linking the faster wf-panel from /AOK/bundled"
            rm -f /usr/local/lib/ish-wayland/wf-panel /usr/local/lib/ish-wayland/wf-panel.source
            mkdir -p /usr/local/bin
            ln -sf "$bundled_panel" /usr/local/bin/wf-panel || die "could not link /usr/local/bin/wf-panel"
        elif command -v apt-get >/dev/null 2>&1; then
            if [ -n "$bundled_panel" ]; then
                note "the bundled faster wf-panel is for wf-shell $(cat "$bundled_panel.source" 2>/dev/null || echo '(none)'),"
                note "this root has ${wf_shell_version:-none}: building one from source"
            else
                note "no faster wf-panel is bundled for $(uname -m): building one from source"
            fi
            log "building the faster wf-panel (tools/wayland/build-wf-panel.sh)"
            sh /AOK/tools/wayland/build-wf-panel.sh \
                || die "the faster wf-panel did not build -- see above; the packaged one still works"
        else
            note "the faster wf-panel is built from Debian/Devuan source; this root keeps the packaged one"
        fi ;;
esac
case "$PANEL" in
    waybar) PANEL_CHOICE=waybar ;;
    *) PANEL_CHOICE=wf-panel ;;
esac

log "done"
note "Wayfire $(wayfire --version 2>/dev/null | sed -n '1s/[- ].*//p') is installed."
if [ "$SELECT" = 1 ]; then
    sh /AOK/tools/select-desktop.sh wayfire --panel "$PANEL_CHOICE" >/dev/null \
        || die "could not select it -- see select-desktop.sh"
    note "The Wayland window starts Wayfire from its next session: close it and open it again."
else
    sh /AOK/tools/select-desktop.sh --panel "$PANEL_CHOICE" >/dev/null \
        || die "could not set the panel -- see select-desktop.sh"
    note "To start it from the next session:  sudo sh /AOK/tools/select-desktop.sh wayfire"
fi
note "Panel: $PANEL_CHOICE$( [ "$PANEL_CHOICE" = wf-panel ] && [ -x /usr/local/bin/wf-panel ] && echo ' (the faster build)')."
if [ "$PANEL" = both ]; then
    note "Swap panels: sudo sh /AOK/tools/select-desktop.sh --panel waybar (or wf-panel)"
fi
note "Wayfire will not run as root: the Wayland window needs Settings >"
note "Open Everything as Default User turned on, or it opens labwc instead."
note "Keys: Alt+Return terminal, Alt+Shift+D launcher, Alt+Shift+Q close,"
note "      Ctrl+Alt+Left/Right desktops, Alt+Shift+W all desktops (expo),"
note "      Ctrl+Alt+drag turns the desktop cube, Alt+drag moves a window."
note "Settings: wcm. Back to labwc:  sudo sh /AOK/tools/select-desktop.sh labwc"
