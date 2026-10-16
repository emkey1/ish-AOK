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
# arm64, in the CLI harness and on-device, Alpine 3.23 arm64 (labwc 0.9.2)
# start to finish in the harness, menu and X11 programs included, and Alpine
# 3.24 arm64 in an AOK VM machine (the user, 2026-10-14). Arch (pacman)
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

# apk add, with every package downloaded whole before any is installed.
# apk-tools 3 (Alpine 3.23+) streams each package from the mirror straight into
# extraction, so the connection is read only as fast as files are written. Under
# emulation that is slow, and if the app is suspended mid-install it stops
# altogether; a mirror drops a connection left unread for a few minutes
# (measured: a reader paused for 3 minutes lost the rest of a 68 MB transfer),
# and apk reports that as "I/O error" on whatever it was extracting.
# --cache-predownload fetches the whole transaction into the cache first
# (/var/cache/apk when /etc/apk/cache is not set up) and installs from there; a
# failed download stops the run before anything is installed, and a retry keeps
# what already arrived. Up to three tries. The downloaded packages are deleted
# afterwards unless the system keeps an apk cache of its own. apk-tools 2 has no
# such option and streams as it always did.
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

    # The desktop runs the wayvnc in /AOK/bundled (0.10.2; Devuan's 0.9.1 can
    # die when the app takes a GPU desktop over -- see start-wayland.sh), and
    # it needs these libraries. The packaged wayvnc above stays as the
    # fallback for a root the bundle was not built for.
    vnc_depends="/AOK/bundled/devuan6-$(uname -m)/wayvnc.depends"
    if [ -r "$vnc_depends" ]; then
        log "installing the libraries of the bundled wayvnc"
        # shellcheck disable=SC2046 # one package name per line
        DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
            $(sed -n 's/^\([a-z0-9][a-z0-9.+-]*\)$/\1/p' "$vnc_depends") \
            || note "could not install them; the desktop uses Devuan's wayvnc instead"
    fi

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
    note "Alpine was last run on 3.24 arm64 -- please report back what breaks."
    # font-dejavu: on Devuan and Arch the stack pulls in a text font, and on
    # Alpine nothing does. With only the icon font waybar brings below, every
    # window title, menu and foot terminal was drawn in Font Awesome.
    # wlr-randr sets the output scale the Display applet's UI-scale setting
    # asks for; labwc has no output configuration of its own.
    log "installing labwc, sway, wofi, foot, wayvnc, wlr-randr"
    apk_add labwc sway wofi foot wayvnc font-dejavu wlr-randr || die "apk add failed -- see output above"

else
    die "no supported package manager found (need apt-get, pacman, or apk)"
fi

for bin in labwc sway wofi foot wayvnc; do
    command -v "$bin" >/dev/null 2>&1 || die "install reported success but '$bin' is not on PATH"
done

# Not under iSH-AOK: a machine with a kernel and a display of its own (a
# virtual machine), where start-wayland.sh puts the desktop on that display
# instead of serving it to the app. The compositor then needs a seat to take
# the card and the input devices through, which is seatd; a login that is to
# start the desktop has to be in seatd's group.
IN_AOK=0
[ -d /proc/ish ] && IN_AOK=1
if [ "$IN_AOK" = 0 ]; then
    log "a machine of its own: seatd, for the compositor to take the display"
    if command -v apt-get >/dev/null 2>&1; then
        DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends seatd \
            || note "warning: seatd did not install; only root will be able to start the desktop"
        update-rc.d seatd defaults >/dev/null 2>&1
        service seatd start >/dev/null 2>&1 || true
        seat_group=video
    elif command -v pacman >/dev/null 2>&1; then
        pacman -S --needed --noconfirm seatd || note "warning: seatd did not install"
        seat_group=seat
    else
        apk_add seatd || note "warning: seatd did not install"
        rc-update add seatd default >/dev/null 2>&1
        rc-service seatd start >/dev/null 2>&1 || true
        seat_group=seat
    fi
    # The keyboard and the pointer: libinput takes only the input devices a
    # udev daemon has recorded, and AOK VM's base machines run none (busybox's
    # init on Alpine, and Devuan's root as iSH-AOK ships it has no eudev).
    if command -v apt-get >/dev/null 2>&1; then
        DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends eudev >/dev/null 2>&1 \
            || note "warning: eudev did not install; the desktop may find no keyboard or pointer"
    elif command -v apk >/dev/null 2>&1; then
        apk_add eudev || note "warning: eudev did not install; the desktop may find no keyboard or pointer"
    fi
    # `desktop`: the desktop on this machine's display, as the Ultimate machines
    # have it. It starts udevd and seatd itself where nothing else has (AOK
    # VM's Alpine machines boot with busybox's init, no OpenRC to run them).
    # `desktop soft`: composited without the GPU, for a display whose GL path
    # does not show.
    mkdir -p /usr/local/bin
    cat > /usr/local/bin/desktop <<'D'
#!/bin/sh
# desktop -- the Wayland desktop on this machine's display (setup-wayland.sh).
# desktop soft: composited without the GPU.
[ "$1" = soft ] && { shift; export WLR_RENDERER=pixman; }
if [ "$(id -u)" = 0 ]; then
    # udev, for libinput to find the keyboard and the pointer
    if ! pidof udevd systemd-udevd >/dev/null 2>&1; then
        for u in /sbin/udevd /usr/sbin/udevd /lib/systemd/systemd-udevd /usr/lib/systemd/systemd-udevd; do
            [ -x "$u" ] || continue
            "$u" --daemon && udevadm trigger --action=add && udevadm settle --timeout=5
            break
        done
    fi
    # seatd, for the compositor to take the display and the input devices
    if command -v seatd >/dev/null 2>&1 && ! pidof seatd >/dev/null 2>&1; then
        seatd -g SEAT_GROUP >/dev/null 2>&1 &
        n=0; while [ ! -S /run/seatd.sock ] && [ $n -lt 20 ]; do sleep 0.1; n=$((n + 1)); done
    fi
elif ! pidof seatd >/dev/null 2>&1; then
    echo "desktop: seatd is not running; run desktop as root once, or start seatd (seatd -g SEAT_GROUP &)" >&2
fi
exec sh /AOK/tools/start-wayland.sh "$@"
D
    sed -i "s/SEAT_GROUP/$seat_group/g" /usr/local/bin/desktop
    chmod 755 /usr/local/bin/desktop
    for _u in $(awk -F: '$3 >= 1000 && $3 < 2000 { print $1 }' /etc/passwd); do
        if command -v usermod >/dev/null 2>&1; then
            usermod -a -G "$seat_group" "$_u" 2>/dev/null
        else
            addgroup "$_u" "$seat_group" 2>/dev/null
        fi && note "$_u may start the desktop (group $seat_group)"
    done
fi

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
        apk_add dbus
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
    apk_add waybar font-awesome
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
    apk_add $MENU_APPS \
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
            apk_add $QT_RUNTIME \
                || note "warning: Qt6 runtime install failed -- qv4l2/qvidcap in the Applications menu will still be broken"
        fi
    fi
fi

# Both shims below come prebuilt in /AOK/bundled for glibc and musl on all four
# guest architectures (tools/build-bundled.sh), and start-wayland.sh loads
# those first; they are only built here for a root those do not cover.
case "$(uname -m)" in
    aarch64|arm64|x86_64|amd64|i[3-6]86|riscv64) shims_bundled=1 ;;
    *) shims_bundled=0 ;;
esac

# Best-effort too: the wl_buffer release guard (tools/wayland/
# ish_wl_release_guard.c), which keeps GTK 3 programs -- wf-panel's menu
# among them -- from aborting when a buffer they committed twice is released
# twice. Nothing but a compiler is needed; without one, start-wayland.sh
# tries again per user, and a session runs without it as it always has.
log "building the Wayland buffer release guard (best-effort)"
if [ "$IN_AOK" = 0 ] && [ ! -f /AOK/tools/wayland/build-release-guard.sh ]; then
    note "its source is not on this machine (/AOK/tools/wayland); skipped"
elif [ "$IN_AOK" = 1 ] && [ "$shims_bundled" = 1 ]; then
    note "prebuilt in /AOK/bundled for this root; nothing to build"
elif command -v cc >/dev/null 2>&1; then
    sh /AOK/tools/wayland/build-release-guard.sh 2>&1 \
        || note "warning: release guard build failed -- GTK programs may abort on a double buffer release"
else
    note "no C compiler found -- skipping the release guard."
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
if [ "$IN_AOK" = 0 ]; then
    note "not under iSH-AOK: there is no accelerator to reach"
elif [ "$shims_bundled" = 1 ]; then
    note "prebuilt in /AOK/bundled for this root; nothing to build"
elif command -v cc >/dev/null 2>&1 || command -v gcc >/dev/null 2>&1; then
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
if [ "$IN_AOK" = 1 ]; then
    note "The Display applet will launch the stack automatically from here on."
else
    note "Start the desktop on this machine's display with:  desktop"
fi
note "Games, desktop tools and X11 support are optional extras:"
note "  sudo sh /AOK/tools/setup-wayland-extras.sh games tools x11"
note "Fuller desktops, in place of labwc: setup-wayfire.sh (on the GPU) and setup-xfce.sh"
