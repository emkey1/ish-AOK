#!/bin/sh
# setup-wayland.sh
# ---------------------------------------------------------------------------
# Install a headless Wayland desktop (compositor + terminal + VNC bridge) into
# the current guest root, for the Workspace "Display" applet. Installs:
#   labwc    - wlroots-based Wayland compositor (headless backend, no
#              DRM/udev/seatd needed -- see start-wayland.sh); the default
#              compositor -- floating/stacking windows, right-click menu,
#              themed to match the app's own Workspace look
#   sway     - a second wlroots-based compositor, kept installed as a
#              WAYLAND_COMPOSITOR_CMD=sway alternative (tiling by default,
#              keybinding-driven instead of a menu)
#   wofi     - dmenu-style app launcher, invoked by sway's Alt+d binding
#              (only relevant if you switch to sway)
#   foot     - lightweight Wayland terminal, the default first app
#   wayvnc   - VNC server for wlroots compositors; the applet's native RFB
#              client connects to it over TCP
# Also installs (best-effort, won't block the above): htop, btop, mc,
# neovim, vim -- so the Display applet's Applications menu (list-apps.sh,
# see start-wayland.sh) isn't nearly empty on a fresh rootfs. That menu just
# reflects whatever's already installed with a .desktop file; with nothing
# beyond the required packages above it would only ever show foot's own
# three entries. And, also best-effort: dbus-daemon, for the session bus
# start-wayland.sh gives the desktop, and waybar with the Font Awesome icons
# its modules draw.
#
# Run as root:
#       sudo sh /AOK/tools/setup-wayland.sh
#   or  doas sh /AOK/tools/setup-wayland.sh
#
# Devuan (apt) and Alpine (apk) both run the desktop: Devuan 6 on amd64 and
# arm64, in the CLI harness and on-device, and Alpine 3.23 arm64 (labwc 0.9.2)
# start to finish in the harness, menu and X11 programs included. Arch (pacman)
# installs the same stack under the same names and its packages resolve, but
# nobody has run a session on it.
#
# amd64/x86_64 and arm64/aarch64 are the tested guest arches. The others may
# work -- the packages exist for them in Devuan -- but haven't been run, so
# this script warns rather than refuses.
# ---------------------------------------------------------------------------
set -u

log()  { printf '\n\033[1;36m==>\033[0m \033[1m%s\033[0m\n' "$*"; }
note() { printf '    %s\n' "$*"; }
die()  { printf 'setup-wayland.sh: %s\n' "$*" >&2; exit 1; }

[ "$(id -u)" = 0 ] || die "must run as root:  sudo sh $0"

ARCH="$(uname -m)"
case "$ARCH" in
    x86_64|amd64|aarch64|arm64) : ;;
    *) note "warning: guest arch '$ARCH' is untested for the Wayland stack;"
       note "proceeding anyway -- report back what breaks." ;;
esac

if command -v apt-get >/dev/null 2>&1; then
    log "Devuan/Debian (apt) detected"

    # deb.devuan.org's round-robin can land on a slow/unreliable mirror (seen:
    # <1KB/s, dropped connections mid-transfer); pin the master mirror instead
    # of gambling on the pool, matching the fix used during Wayland bring-up.
    SOURCES=/etc/apt/sources.list
    if [ -f "$SOURCES" ] && grep -q 'deb\.devuan\.org' "$SOURCES" 2>/dev/null; then
        note "pinning pkgmaster.devuan.org (deb.devuan.org's round-robin can be very slow)"
        sed -i 's/deb\.devuan\.org/pkgmaster.devuan.org/g' "$SOURCES"
    fi

    log "apt-get update"
    apt-get update || die "apt-get update failed -- check network/DNS (guest /etc/resolv.conf)"

    log "installing labwc, sway, wofi, foot, wayvnc, wlr-randr"
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        labwc sway wofi foot wayvnc wlr-randr \
        || die "apt-get install failed -- see output above"

elif command -v pacman >/dev/null 2>&1; then
    log "Arch (pacman) detected"
    # All five packages carry the same names in the Arch repos as in Devuan.
    # The Arch provisioner (provision-ultimate-archlinux.sh) has already
    # disabled pacman's Landlock sandbox and signature checking for iSH; if
    # this root wasn't provisioned by it, pacman -Sy may fail on those --
    # run the provisioner first in that case.
    log "pacman -Sy"
    pacman -Sy --noconfirm || die "pacman -Sy failed -- check network/DNS (guest /etc/resolv.conf)"
    log "installing labwc, sway, wofi, foot, wayvnc, wlr-randr"
    pacman -S --needed --noconfirm labwc sway wofi foot wayvnc wlr-randr \
        || die "pacman -S failed -- see output above"

elif command -v apk >/dev/null 2>&1; then
    log "Alpine (apk) detected"
    note "Alpine was last run on 3.23 arm64 -- please report back what breaks."
    # font-dejavu: on Devuan and Arch the stack pulls in a text font, and on
    # Alpine nothing does. With only the icon font waybar brings below, every
    # window title, menu and foot terminal was drawn in Font Awesome.
    # wlr-randr sets the output scale the Display applet's UI-scale setting
    # asks for; labwc has no output configuration of its own.
    apk add labwc sway wofi foot wayvnc font-dejavu wlr-randr || die "apk add failed -- see output above"

else
    die "no supported package manager found (need apt-get, pacman, or apk)"
fi

for bin in labwc sway wofi foot wayvnc; do
    command -v "$bin" >/dev/null 2>&1 || die "install reported success but '$bin' is not on PATH"
done

# The desktop's session bus. start-wayland.sh starts one when dbus-daemon is
# installed, and GLib and Qt programs (waybar, Falkon) need it; the Alpine
# packages above do not pull it in. Best-effort, like the menu apps below:
# labwc, foot and wayvnc run without it. On Devuan it is dbus-daemon alone,
# since the dbus package would add the system bus and its init script.
if ! command -v dbus-daemon >/dev/null 2>&1; then
    log "installing dbus-daemon for the desktop's session bus (best-effort)"
    if command -v apt-get >/dev/null 2>&1; then
        DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends dbus-daemon
    elif command -v pacman >/dev/null 2>&1; then
        pacman -S --needed --noconfirm dbus
    elif command -v apk >/dev/null 2>&1; then
        apk add dbus
    fi
    # Judged by the result, not the exit status: apk exits 1 when any package
    # already in the root is broken, even though dbus itself installed.
    command -v dbus-daemon >/dev/null 2>&1 \
        || note "warning: dbus-daemon did not install -- programs that need a session bus will not find one"
fi

# waybar, the panel, and the Font Awesome icons its modules draw with (without
# the font every icon is an empty box). start-wayland.sh gives it a config for
# labwc on the first session. Best-effort like the rest: the desktop runs
# without it. The font's package name differs on each: fonts-font-awesome on
# Devuan, font-awesome on Alpine, otf-font-awesome on Arch.
log "installing waybar and its icon font (best-effort)"
if command -v apt-get >/dev/null 2>&1; then
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends waybar fonts-font-awesome
elif command -v pacman >/dev/null 2>&1; then
    pacman -S --needed --noconfirm waybar otf-font-awesome
elif command -v apk >/dev/null 2>&1; then
    apk add waybar font-awesome
fi
command -v waybar >/dev/null 2>&1 \
    || note "warning: waybar did not install -- the desktop runs without a panel"
# The font is the other half of a working panel, and its install is best-effort
# too. Only waybar's absence used to be checked, so a font that failed to install
# produced a panel of empty boxes and no warning.
if command -v waybar >/dev/null 2>&1; then
    if command -v fc-list >/dev/null 2>&1; then
        font_hits=$(fc-list 2>/dev/null | grep -ci 'font *awesome')
    else
        font_hits=$(find /usr/share/fonts -iname '*awesome*' 2>/dev/null | grep -c .)
    fi
    if [ "${font_hits:-0}" = 0 ]; then
        note "warning: waybar installed but Font Awesome did not -- every panel icon"
        note "         will be an empty box with a code in it. Install it by hand,"
        note "         then reopen the desktop."
    fi
fi

# Best-effort, not required: a renamed/missing package on some future
# Debian/Alpine release shouldn't block installing the actual Wayland stack
# above, which just finished and is already verified working.
MENU_APPS="htop btop mc neovim vim"
log "installing $MENU_APPS for the default Applications menu (best-effort)"
if command -v apt-get >/dev/null 2>&1; then
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends $MENU_APPS \
        || note "warning: some of these failed to install -- Applications menu will just show whichever succeeded"
elif command -v pacman >/dev/null 2>&1; then
    pacman -S --needed --noconfirm $MENU_APPS \
        || note "warning: some of these failed to install -- Applications menu will just show whichever succeeded"
elif command -v apk >/dev/null 2>&1; then
    apk add $MENU_APPS \
        || note "warning: some of these failed to install -- Applications menu will just show whichever succeeded"
fi

# wayvnc -> neatvnc -> ffmpeg -> v4l-utils is a real dependency chain
# (confirmed on-device via `pacman -Qi ffmpeg`/`v4l-utils`): installing
# wayvnc above can transitively pull in v4l-utils, which ships two Qt6 GUI
# tools (qv4l2, qvidcap) with their own .desktop files -- so they show up in
# the Display applet's Applications menu (list-apps.sh) looking like normal
# entries. Every distro's v4l-utils package lists Qt6 as only an OPTIONAL
# dependency for those two tools specifically, so the package manager never
# installs it automatically, and this headless setup has no Xwayland to fall
# back on either -- without this, those two menu entries are silently broken
# from a fresh install (dynamic linker error; no window, no visible error at
# all, since their .desktop files set Terminal=false). Gated on qv4l2/
# qvidcap actually being present so this doesn't pull in Qt6 on every guest
# regardless of whether anything in the menu needs it.
if command -v qv4l2 >/dev/null 2>&1 || command -v qvidcap >/dev/null 2>&1; then
    QT_RUNTIME=""
    if command -v apt-get >/dev/null 2>&1; then
        # Debian/Devuan's qt6-wayland pulls in libqt6core6t64/gui/widgets etc.
        # as real Depends (unlike Arch, there's no separate runtime metapackage).
        QT_RUNTIME="qt6-wayland"
    elif command -v pacman >/dev/null 2>&1; then
        QT_RUNTIME="qt6-base qt6-5compat qt6-wayland"
    elif command -v apk >/dev/null 2>&1; then
        QT_RUNTIME="qt6-qtbase qt6-qtwayland"
    fi
    if [ -n "$QT_RUNTIME" ]; then
        log "installing $QT_RUNTIME (best-effort) -- v4l-utils pulled in qv4l2/qvidcap, whose Qt6 runtime is only an optional dependency"
        if command -v apt-get >/dev/null 2>&1; then
            DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends $QT_RUNTIME \
                || note "warning: Qt6 runtime install failed -- qv4l2/qvidcap in the Applications menu will still be broken"
        elif command -v pacman >/dev/null 2>&1; then
            pacman -S --needed --noconfirm $QT_RUNTIME \
                || note "warning: Qt6 runtime install failed -- qv4l2/qvidcap in the Applications menu will still be broken"
        elif command -v apk >/dev/null 2>&1; then
            apk add $QT_RUNTIME \
                || note "warning: Qt6 runtime install failed -- qv4l2/qvidcap in the Applications menu will still be broken"
        fi
    fi
fi

# Best-effort, not required: the pixman accelerator (kernel/ish_accel_pix.c)
# is an off-by-default host feature the emulator may not even have compiled
# in (ISH_PIX_ACCEL / the app toggle) -- building this shim just makes
# start-wayland.sh able to load it WHEN it's available; if there is no
# compiler, or the build fails for any other reason, that's a pure perf miss,
# never a functional one (start-wayland.sh only sets LD_PRELOAD if the .so
# actually got built here). The pixman development headers are NOT needed:
# the shim falls back to a copy of pixman.h vendored beside it.
log "building pixman accelerator shim (best-effort)"
if command -v cc >/dev/null 2>&1 || command -v gcc >/dev/null 2>&1; then
    if sh /AOK/tools/pixman/build-shim.sh 2>&1; then
        :
    else
        note "warning: pixman shim build failed -- Wayland sessions will just run without the accelerator"
    fi
else
    note "no C compiler found -- skipping the pixman accelerator shim (Wayland sessions run without it)."
    note "  to get it, install a compiler and rerun this script:"
    note "    Devuan/Debian: apt install gcc"
    note "    Arch:          pacman -S gcc"
    note "    Alpine:        apk add build-base"
fi

log "done"
note "labwc, sway, wofi, foot, and wayvnc are installed."
note "The Display applet will launch the stack automatically from here on."
note "Games, desktop tools and X11 support are optional extras:"
note "  sudo sh /AOK/tools/setup-wayland-extras.sh games tools x11"
