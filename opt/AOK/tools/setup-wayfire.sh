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
# render node, Mesa's Venus driver and zink. Mesa has Venus only for aarch64
# and x86_64 -- but the driver a process loads is of the PROCESS's
# architecture, not the root's, and iSH-AOK runs an arm64 program in a root
# of any architecture. So on a Devuan root that is not arm64, Wayfire, its
# shell and its settings window are installed as arm64 through multiarch,
# with arm64's Mesa: an arm64 Wayfire composites on the GPU in a riscv64 or
# i386 root as it does in an arm64 one, and runs as the fastest guest on an
# x86_64 one. The terminal, VNC bridge and X server stay the root's own.
# And it will not run as root, on any Linux, so the Wayland window must
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
#               0.7 s. Prebuilt in /AOK/bundled for Devuan 6, as arm64 (the
#               fastest guest), and linked on a root of any architecture --
#               with arm64 libraries through multiarch on one that is not
#               arm64; built from source for a wf-shell it does not match.
#       waybar  waybar, with an Apps button that opens wofi's application
#               list, as the labwc desktop's panel is. No build.
#       both    the two, starting with fast; swap whenever you like with
#               select-desktop.sh --panel wf-panel|waybar
#       stock   the packaged wf-panel as it is
#     Asked when run in a terminal without --panel; stock otherwise.
#   sudo sh /AOK/tools/select-desktop.sh --panel waybar   change the panel later
#   sudo sh /AOK/tools/select-desktop.sh labwc        back to the default desktop
#
# Devuan 6 (Wayfire 0.9) runs it, on any architecture (arm64 Wayfire through
# multiarch where the root is not arm64). Arch Linux ARM packages it (0.11) but its
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
# Wayfire as arm64 on a Devuan root of another architecture (see above).
ARM64_MODE=0
if command -v dpkg >/dev/null 2>&1; then
    [ "$(dpkg --print-architecture)" = arm64 ] || ARM64_MODE=1
else
    case "$(uname -m)" in
        x86_64|amd64|aarch64|arm64) : ;;
        *) die "Wayfire needs the GPU, and Mesa has no GPU (Venus) driver for $(uname -m) roots. setup-xfce.sh works here." ;;
    esac
fi
[ -c /dev/dri/renderD128 ] || die "no /dev/dri/renderD128: this iSH-AOK build has no GPU device, which Wayfire needs -- update the app"

# The base desktop: the terminal, the VNC bridge and the launcher Wayfire's
# Alt+Shift+D opens.
if ! command -v foot >/dev/null 2>&1 || ! command -v wayvnc >/dev/null 2>&1; then
    log "the base Wayland desktop is not installed yet -- running setup-wayland.sh first"
    sh /AOK/tools/setup-wayland.sh || die "setup-wayland.sh failed -- see above"
fi

# The GPU drivers. --check reports without installing. Not in arm64 mode:
# setup-gpu.sh installs the root's own architecture's, and Wayfire's are
# arm64's, installed with it below.
if [ "$ARM64_MODE" = 1 ]; then
    :
elif ! sh /AOK/tools/setup-gpu.sh --check >/dev/null 2>&1; then
    log "the GPU drivers are not installed yet -- running setup-gpu.sh first"
    sh /AOK/tools/setup-gpu.sh || die "setup-gpu.sh failed, and Wayfire cannot run without the GPU -- see above"
fi

# Wayfire composites with OpenGL ES, which reaches the GPU only through zink,
# and Mesa 25.2 and later refuse to start zink without robustness2's
# nullDescriptor -- which iSH-AOK's GPU (Venus on MoltenVK) does not offer
# yet. Such a root would install Wayfire and then only ever start labwc, so
# find out first. Devuan 6 (Mesa 25.0) works; Arch and Alpine 3.24 ship newer.
# (In arm64 mode the arm64 Mesa's version is checked after it is installed.)
gles_renderer=zink
[ "$ARM64_MODE" = 1 ] || gles_renderer=$(LIBGL_ALWAYS_SOFTWARE=0 MESA_LOADER_DRIVER_OVERRIDE=zink \
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
    if [ "$ARM64_MODE" = 1 ]; then
        # Wayfire's own packages and the drivers its process loads, as arm64;
        # kclock and its modules stay the root's own (it is only a program the
        # menu starts). A native wayfire from an earlier run is replaced.
        log "this is a $(dpkg --print-architecture) root: Wayfire goes in as arm64 (Debian multiarch), with arm64's GPU drivers"
        dpkg --print-foreign-architectures | grep -qx arm64 || dpkg --add-architecture arm64 \
            || die "could not add arm64 as a foreign architecture"
        PKGS="$(echo "$PKGS" | sed 's/wayfire wf-shell wcm //') wayfire:arm64 wf-shell:arm64 wcm:arm64"
        PKGS="$PKGS mesa-vulkan-drivers:arm64 libgl1-mesa-dri:arm64 libegl-mesa0:arm64 libvulkan1:arm64 librsvg2-common:arm64"
        # A root with no wayvnc bundled for its own architecture (i386,
        # riscv64) runs the arm64 one from /AOK/bundled: its libraries too.
        if [ ! -r "/AOK/bundled/devuan6-$(uname -m)/wayvnc.depends" ] \
                && [ -r /AOK/bundled/devuan6-aarch64/wayvnc.depends ]; then
            PKGS="$PKGS $(sed -n 's/^\([a-z0-9][a-z0-9.+-]*\)$/\1:arm64/p' /AOK/bundled/devuan6-aarch64/wayvnc.depends | tr '\n' ' ')"
        fi
    fi
    log "apt-get update"
    apt-get update || die "apt-get update failed -- check network/DNS (guest /etc/resolv.conf)"
    log "installing $PKGS"
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        -o Dpkg::Options::=--force-confdef -o Dpkg::Options::=--force-confold $PKGS \
        || die "apt-get install failed -- see output above"
    if [ "$ARM64_MODE" = 1 ]; then
        # The zink rule (above), for the Mesa Wayfire will load.
        mesa_arm64=$(dpkg-query -W -f '${Version}' libegl-mesa0:arm64 2>/dev/null)
        case "$mesa_arm64" in
            2[0-4].*|25.[01].*|25.[01]) note "arm64 Mesa $mesa_arm64: OpenGL ES on the GPU" ;;
            *) die "arm64 Mesa ${mesa_arm64:-(none)} is 25.2 or later, whose zink needs a Vulkan feature iSH-AOK's GPU does not offer yet; Wayfire cannot composite on it" ;;
        esac
    fi
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
# The faster wf-panel comes prebuilt in /AOK/bundled for Devuan 6's wf-shell,
# built for arm64 -- the guest that runs fastest -- and is linked in on a root
# of ANY architecture: iSH-AOK runs an arm64 program in an x86_64 or riscv64
# root as readily as in an arm64 one, given arm64 libraries to load. On a
# Devuan root that is not arm64 those come from Debian multiarch: arm64 is
# added as a foreign architecture and the packages in wf-panel.depends are
# installed for it (their arm64 loader included), beside the root's own. A
# wf-shell it was not built for compiles one here instead (which needs a lot
# of memory -- an A10X iPad ran out).
bundled_panel=/AOK/bundled/devuan6-aarch64/wf-panel
# The arm64 libraries the bundled panel needs, on a root of another
# architecture. Returns nonzero when they could not be installed.
bundled_panel_libs() {
    root_arch=$(dpkg --print-architecture 2>/dev/null) || return 1
    [ "$root_arch" = arm64 ] && return 0
    [ -r "$bundled_panel.depends" ] || return 1
    log "the bundled wf-panel is arm64: adding arm64 libraries to this $root_arch root (Debian multiarch)"
    dpkg --print-foreign-architectures | grep -qx arm64 || dpkg --add-architecture arm64 || return 1
    apt-get update || return 1
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        -o Dpkg::Options::=--force-confdef -o Dpkg::Options::=--force-confold \
        $(sed -n 's/^\([a-z0-9][a-z0-9.+-]*\)$/\1:arm64/p' "$bundled_panel.depends")
}
case "$PANEL" in
    fast|both)
        wf_shell_version=$(dpkg-query -W -f '${Version}' wf-shell 2>/dev/null) || wf_shell_version=
        if [ -x "$bundled_panel" ] && [ -n "$wf_shell_version" ] \
                && [ "$(cat "$bundled_panel.source" 2>/dev/null)" = "$wf_shell_version" ] \
                && bundled_panel_libs; then
            log "linking the faster wf-panel from /AOK/bundled"
            rm -f /usr/local/lib/ish-wayland/wf-panel /usr/local/lib/ish-wayland/wf-panel.source
            mkdir -p /usr/local/bin
            ln -sf "$bundled_panel" /usr/local/bin/wf-panel || die "could not link /usr/local/bin/wf-panel"
        elif command -v apt-get >/dev/null 2>&1; then
            if [ "$(cat "$bundled_panel.source" 2>/dev/null)" != "$wf_shell_version" ]; then
                note "the bundled faster wf-panel is for wf-shell $(cat "$bundled_panel.source" 2>/dev/null || echo '(none)'),"
                note "this root has ${wf_shell_version:-none}: building one from source"
            else
                note "the bundled faster wf-panel's arm64 libraries did not install: building one from source"
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
