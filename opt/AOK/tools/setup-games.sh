#!/bin/sh
# setup-games.sh
# ---------------------------------------------------------------------------
# Install a set of classic games that run well in the iSH-AOK Wayland desktop,
# and set each one up to start the way it works best here:
#
#   Freedoom (Phase 1 and 2) on Chocolate Doom    freedoom1, freedoom2
#   Beneath a Steel Sky (ScummVM, freeware)       sky
#
# Every game is started through /usr/local/bin/aok-sdl-game (a symlink per
# game, found on PATH before /usr/games, and used by the desktop menu too),
# which plays sound through the app's /dev/dsp and draws on the GPU for the
# games listed in /etc/aok-games.conf, in software for the rest.
#
# The GPU is used where it works: an arm64 or amd64 root, the GPU device, and
# a GPU zink can draw on (A9 and newer; the A10X needs MoltenVK 67d3f726).
# --gpu and --software override that. Freedoom's timedemo: 280 fps on an M4's
# GPU against under 35 in software; 79 fps on an A10X's against under 10.
# ScummVM's intro used 26% of a CPU on the GPU against 70% in software (M5).
#
# Usage:
#   sudo sh /AOK/tools/setup-games.sh              install, then check
#   sudo sh /AOK/tools/setup-games.sh --gpu        Doom on the GPU regardless
#   sudo sh /AOK/tools/setup-games.sh --software   everything in software
#   sudo sh /AOK/tools/setup-games.sh --debs DIR   use the .deb files in DIR
#                                                  before downloading any
#   sh /AOK/tools/setup-games.sh --check           report only (no root needed)
#
# Devuan (apt) only for now; Alpine and Arch lack Beneath a Steel Sky's data
# package, and nobody has run the games there.
# ---------------------------------------------------------------------------
set -u

log()  { printf '\n\033[1;36m==>\033[0m \033[1m%s\033[0m\n' "$*"; }
note() { printf '    %s\n' "$*"; }
die()  { printf 'setup-games.sh: %s\n' "$*" >&2; exit 1; }

CONF=/etc/aok-games.conf
WRAPPER=/usr/local/bin/aok-sdl-game
WRAPPER_SRC="$(dirname "$0")/aok-sdl-game"   # /AOK/tools, beside this script

# Packages, the /usr/games names that go through the wrapper, and the ones
# that draw on the GPU when it works.
PKGS="chocolate-doom freedoom scummvm beneath-a-steel-sky"
WRAPPED="chocolate-doom doom freedoom1 freedoom2 scummvm sky"
GPU_GAMES="chocolate-doom doom freedoom1 freedoom2 scummvm sky"

usage() {
    sed -n '/^# Usage:/,/^# Devuan/p' "$0" | sed '$d; s/^# \{0,1\}//'
}

CHECK_ONLY=0
GL_CHOICE=auto
GL_FORCED=0
DEBS=""
while [ $# -gt 0 ]; do
    case "$1" in
        --check) CHECK_ONLY=1 ;;
        --gpu) GL_CHOICE=gpu; GL_FORCED=1 ;;
        --software) GL_CHOICE=software; GL_FORCED=1 ;;
        --debs) [ $# -ge 2 ] || { usage >&2; exit 2; }; DEBS=$2; shift ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
    shift
done

# The host's chip, e.g. "M4" or "A10X", from /proc/ish/host_info's
# "Host Architecture: arm64e(M4)".
host_chip() {
    sed -n 's/^Host Architecture: [^(]*(\(.*\))$/\1/p' /proc/ish/host_info 2>/dev/null
}

# Why the GPU cannot draw these games here; nothing when it can.
gpu_blocker() {
    case "$(uname -m)" in
        x86_64|amd64|aarch64|arm64) : ;;
        *) echo "Mesa has no GPU driver for $(uname -m) guests"; return ;;
    esac
    [ -c /dev/dri/renderD128 ] || { echo "this iSH-AOK build has no GPU device"; return; }
    case "$(host_chip)" in
        A7*|A8*) echo "zink cannot draw on the $(host_chip)'s GPU"; return ;;
    esac
}

# zink gives an OpenGL context (it can still fail at the first draw on a GPU
# gpu_blocker does not know about; --software is the way out).
zink_context() {
    command -v eglinfo >/dev/null 2>&1 || return 1
    if command -v timeout >/dev/null 2>&1; then _t="timeout 30"; else _t=""; fi
    LIBGL_ALWAYS_SOFTWARE=0 MESA_LOADER_DRIVER_OVERRIDE=zink $_t eglinfo -B -p surfaceless 2>/dev/null |
        grep -q 'profile version:.*Mesa'
}

CHECK_FAILED=0
ok()   { printf '    \033[1;32mok\033[0m    %s\n' "$*"; }
bad()  { printf '    \033[1;31mno\033[0m    %s\n' "$*"; CHECK_FAILED=1; }
info() { printf '    --    %s\n' "$*"; }

check() {
    log "checking"
    for name in $WRAPPED; do
        if [ -e "/usr/games/$name" ]; then
            if [ "$(readlink "/usr/local/bin/$name" 2>/dev/null)" = aok-sdl-game ]; then
                ok "$name"
            else
                bad "$name is installed but does not go through $WRAPPER"
            fi
        else
            bad "$name is not installed"
        fi
    done
    [ -x "$WRAPPER" ] && ok "$WRAPPER" || bad "$WRAPPER is not installed"
    if [ -w /dev/dsp ] || [ -c /dev/dsp ]; then
        ok "sound: /dev/dsp"
    else
        info "no /dev/dsp (the command-line build has none); the games play silently"
    fi
    gpu=""
    [ -r "$CONF" ] && gpu=$(. "$CONF"; echo "${AOK_GAMES_GPU:-}")
    if [ -n "$gpu" ]; then
        info "on the GPU: $gpu"
    else
        info "every game draws in software"
    fi
}

if [ "$CHECK_ONLY" = 1 ]; then
    check
    exit "$CHECK_FAILED"
fi

# ---- install --------------------------------------------------------------

[ "$(id -u)" = 0 ] || die "must run as root:  sudo sh $0"
command -v apt-get >/dev/null 2>&1 || die "only Devuan/Debian (apt) roots are supported so far"
[ -r "$WRAPPER_SRC" ] || die "$WRAPPER_SRC is missing -- update the app"

if [ "$GL_CHOICE" = auto ]; then
    why=$(gpu_blocker)
    if [ -n "$why" ]; then
        GL_CHOICE=software
        log "drawing in software: $why"
    else
        GL_CHOICE=gpu
        log "drawing on the GPU ($(host_chip))"
    fi
fi

MESA="libgl1-mesa-dri libegl-mesa0"
[ "$GL_CHOICE" = gpu ] && MESA="$MESA mesa-vulkan-drivers mesa-utils"

SOURCES=/etc/apt/sources.list
if [ -f "$SOURCES" ] && grep -q 'deb\.devuan\.org' "$SOURCES" 2>/dev/null; then
    note "pinning pkgmaster.devuan.org (deb.devuan.org's round-robin can be very slow)"
    sed -i 's/deb\.devuan\.org/pkgmaster.devuan.org/g' "$SOURCES"
fi
if [ -n "$DEBS" ]; then
    [ -d "$DEBS" ] || die "--debs $DEBS: no such directory"
    log "using the .deb files in $DEBS"
    cp "$DEBS"/*.deb /var/cache/apt/archives/ 2>/dev/null || note "(none found)"
fi

# A stalled mirror connection is common from a device; retry rather than fail.
APT_OPTS="-o Acquire::Retries=5 -o Acquire::http::Timeout=30"
log "apt-get update"
apt-get $APT_OPTS update || die "apt-get update failed -- check network/DNS (guest /etc/resolv.conf)"
log "installing $PKGS $MESA"
DEBIAN_FRONTEND=noninteractive apt-get $APT_OPTS install -y --no-install-recommends \
    -o Dpkg::Options::=--force-confold $PKGS $MESA ||
    die "apt-get install failed -- see output above"

# iOS pauses the GPU while the app is in the background, and a check made then
# times out: try a few times before settling for software.
if [ "$GL_CHOICE" = gpu ] && [ "$GL_FORCED" = 0 ]; then
    tries=1
    until zink_context; do
        if [ "$tries" -ge 3 ]; then
            GL_CHOICE=software
            log "zink gave no OpenGL context: drawing in software"
            note "If iSH-AOK was in the background just now, the GPU was paused; run this"
            note "again with the app in front, or with --gpu to skip the check."
            break
        fi
        tries=$((tries + 1))
        note "zink gave no OpenGL context; trying again ($tries of 3)"
        sleep 5
    done
fi

log "installing $WRAPPER"
mkdir -p /usr/local/bin
cp "$WRAPPER_SRC" "$WRAPPER" && chmod 755 "$WRAPPER" || die "could not write $WRAPPER"
for name in $WRAPPED; do
    [ -e "/usr/games/$name" ] || continue
    ln -sf aok-sdl-game "/usr/local/bin/$name"
    # The desktop menu reads only /usr/share/applications; an entry naming
    # /usr/games/<game> outright would skip the wrapper.
    for f in /usr/share/applications/*.desktop; do
        [ -f "$f" ] && grep -q "^Exec=/usr/games/$name\( \|$\)" "$f" &&
            sed -i "s|^Exec=/usr/games/$name|Exec=$name|" "$f"
    done
done

log "writing $CONF"
if [ "$GL_CHOICE" = gpu ]; then gpu_list=$GPU_GAMES; else gpu_list=""; fi
{
    echo "# Written by /AOK/tools/setup-games.sh; /usr/local/bin/aok-sdl-game reads it."
    echo "# The games that draw on the GPU (zink); the rest draw in software."
    echo "AOK_GAMES_GPU=\"$gpu_list\""
} > "$CONF"

check

log "done"
if [ "$CHECK_FAILED" = 0 ]; then
    note "Start them from the desktop's menu, or in a terminal there:"
    note "  freedoom1    freedoom2    sky"
    note "Doom: arrow keys move and turn, Ctrl fires, Space opens doors, Shift runs,"
    note "      Alt+arrows strafe, 1-7 pick a weapon, Tab shows the map, Esc the menu."
    note "Beneath a Steel Sky: left-click walks and looks, right-click uses; F5 menu."
else
    note "Something above is missing; see the lines marked 'no'."
    exit 1
fi
