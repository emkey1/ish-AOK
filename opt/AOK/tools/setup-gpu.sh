#!/bin/sh
# setup-gpu.sh
# ---------------------------------------------------------------------------
# Set a guest root up to draw with the device's GPU (iSH-AOK's render node,
# /dev/dri/renderD128: Mesa's Venus Vulkan driver in the guest, Metal on the
# host). Installs:
#   the Venus Vulkan driver and zink  - Vulkan on the GPU, and OpenGL on top
#                                       of it; the Wayland desktop then
#                                       composites on the GPU by itself
#   vulkan-tools                      - vulkaninfo, vkcube
#   mesa-utils                        - eglinfo, es2gears_wayland, glxgears
#   /usr/local/bin/gpu-run            - runs one OpenGL program on the GPU
# and, with --demos, glmark2 (Devuan and Arch; Alpine has none).
#
# Vulkan programs use the GPU with nothing more. OpenGL programs do not by
# default: on this GPU zink offers OpenGL 2.1 and OpenGL ES 2.0 where software
# rendering (llvmpipe) offers 4.5, so a program needing more than 2.1 would
# fail outright on the GPU rather than run slowly. `gpu-run PROGRAM` puts one
# program on the GPU; --gl-default makes the GPU the desktop's default for
# every OpenGL program it starts, for those who run older and simpler ones.
#
# Usage:
#   sudo sh /AOK/tools/setup-gpu.sh              install, then check
#   sudo sh /AOK/tools/setup-gpu.sh --demos      also glmark2
#   sudo sh /AOK/tools/setup-gpu.sh --gl-default OpenGL on the GPU by default
#   sudo sh /AOK/tools/setup-gpu.sh --gl-software  back to software OpenGL
#   sh /AOK/tools/setup-gpu.sh --check           report only (no root needed)
#
# Devuan 6, Alpine 3.24 and Arch Linux ARM carry the packages (Alpine 3.23
# has no Venus driver at all); aarch64 and
# x86_64 guests only -- Mesa has no Venus driver for riscv64 or i386 roots.
# ---------------------------------------------------------------------------
set -u

log()  { printf '\n\033[1;36m==>\033[0m \033[1m%s\033[0m\n' "$*"; }
note() { printf '    %s\n' "$*"; }
die()  { printf 'setup-gpu.sh: %s\n' "$*" >&2; exit 1; }

CONF=/etc/aok-gpu.conf
GPU_RUN=/usr/local/bin/gpu-run

usage() {
    sed -n '/^# Usage:/,/^# Devuan/p' "$0" | sed '$d; s/^# \{0,1\}//'
}

DEMOS=0
CHECK_ONLY=0
GL_CHOICE=""
for arg in "$@"; do
    case "$arg" in
        --demos) DEMOS=1 ;;
        --check) CHECK_ONLY=1 ;;
        --gl-default) GL_CHOICE=gpu ;;
        --gl-software) GL_CHOICE=software ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
done

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

# ---- checks ---------------------------------------------------------------

have_zink() {
    for zink in /usr/lib/*/dri/zink_dri.so /usr/lib/dri/zink_dri.so /usr/lib64/dri/zink_dri.so; do
        [ -e "$zink" ] && return 0
    done
    return 1
}

# Bounded: a wedged renderer makes Vulkan programs wait, and a check must not.
bounded() {
    if command -v timeout >/dev/null 2>&1; then
        timeout 30 "$@"
    else
        "$@"
    fi
}

CHECK_FAILED=0
GL_ON_GPU=0
mesa_version() {
    eglinfo -B -p surfaceless 2>/dev/null | sed -n 's/^OpenGL ES profile version: .*Mesa \([0-9][0-9.]*\).*/\1/p' | head -1
}
ok()   { printf '    \033[1;32mok\033[0m    %s\n' "$*"; }
bad()  { printf '    \033[1;31mno\033[0m    %s\n' "$*"; CHECK_FAILED=1; }
info() { printf '    --    %s\n' "$*"; }

check() {
    log "checking"
    if [ -c /dev/dri/renderD128 ]; then
        ok "the GPU device, /dev/dri/renderD128"
    else
        bad "no /dev/dri/renderD128 -- this iSH-AOK build has no GPU device (update the app)"
        return
    fi
    if ls /usr/share/vulkan/icd.d/virtio_icd*.json >/dev/null 2>&1; then
        ok "Mesa's Venus Vulkan driver"
    else
        bad "Mesa's Venus Vulkan driver is not installed"
    fi
    if have_zink; then
        ok "zink (OpenGL on Vulkan)"
    else
        bad "zink is not installed"
    fi
    if command -v vulkaninfo >/dev/null 2>&1; then
        gpu=$(bounded vulkaninfo --summary 2>/dev/null | sed -n 's/^[[:space:]]*deviceName[[:space:]]*= //p' | grep -m1 Venus)
        if [ -n "$gpu" ]; then
            ok "Vulkan sees $gpu"
        else
            bad "vulkaninfo lists no Venus device"
        fi
    else
        info "vulkaninfo is not installed; Vulkan not tried"
    fi
    if command -v eglinfo >/dev/null 2>&1; then
        # The RENDERER says whether zink is drawing: when zink cannot start,
        # Mesa quietly falls back to llvmpipe, whose context reported OpenGL
        # 4.6 and passed this check as "OpenGL through zink".
        egl_out=$(LIBGL_ALWAYS_SOFTWARE=0 MESA_LOADER_DRIVER_OVERRIDE=zink bounded eglinfo -B -p surfaceless 2>&1)
        gl=$(printf '%s\n' "$egl_out" |
             sed -n 's/^OpenGL ES profile version: //p; s/^OpenGL compatibility profile version: //p' | head -2 | tr '\n' ';')
        renderer=$(printf '%s\n' "$egl_out" | sed -n 's/^OpenGL ES profile renderer: //p' | head -1)
        case "$renderer" in
            *zink*)
                GL_ON_GPU=1
                ok "OpenGL through zink: ${gl%;}" ;;
            *)
                if printf '%s\n' "$egl_out" | grep -q 'nullDescriptor'; then
                    # Not something this script can install: Mesa 25.2 and
                    # later refuse to start zink without robustness2's
                    # nullDescriptor, which the GPU path (Venus on MoltenVK,
                    # on Metal) does not offer yet. Vulkan is unaffected.
                    info "OpenGL cannot use the GPU with this Mesa ($(mesa_version)): its zink requires a"
                    info "  Vulkan feature (nullDescriptor) iSH-AOK's GPU does not offer yet. OpenGL"
                    info "  programs run in software; Vulkan programs use the GPU. Devuan 6's Mesa 25.0"
                    info "  still runs OpenGL on the GPU."
                else
                    bad "zink gave no OpenGL context on the GPU (renderer: ${renderer:-none})"
                fi ;;
        esac
    else
        info "eglinfo is not installed; OpenGL not tried"
    fi
    if [ -x "$GPU_RUN" ]; then
        ok "$GPU_RUN"
    else
        bad "$GPU_RUN is not installed"
    fi
    if [ -f "$CONF" ] && grep -q '^AOK_GL=gpu' "$CONF"; then
        info "OpenGL programs in the desktop use the GPU by default ($CONF)"
    else
        if [ "$GL_ON_GPU" = 1 ]; then
            info "OpenGL programs in the desktop use software by default; gpu-run PROGRAM uses the GPU"
        else
            info "OpenGL programs in the desktop use software"
        fi
    fi
}

if [ "$CHECK_ONLY" = 1 ]; then
    check
    exit "$CHECK_FAILED"
fi

# ---- install --------------------------------------------------------------

[ "$(id -u)" = 0 ] || die "must run as root:  sudo sh $0 $*"
[ -c /dev/dri/renderD128 ] || die "no /dev/dri/renderD128: this iSH-AOK build has no GPU device -- update the app"

ARCH="$(uname -m)"
case "$ARCH" in
    x86_64|amd64|aarch64|arm64) : ;;
    *) die "Mesa has no Venus (GPU) driver for $ARCH guests; programs here render in software" ;;
esac

if command -v apt-get >/dev/null 2>&1; then
    log "Devuan/Debian (apt) detected"
    SOURCES=/etc/apt/sources.list
    if [ -f "$SOURCES" ] && grep -q 'deb\.devuan\.org' "$SOURCES" 2>/dev/null; then
        note "pinning pkgmaster.devuan.org (deb.devuan.org's round-robin can be very slow)"
        sed -i 's/deb\.devuan\.org/pkgmaster.devuan.org/g' "$SOURCES"
    fi
    log "apt-get update"
    apt-get update || die "apt-get update failed -- check network/DNS (guest /etc/resolv.conf)"
    PKGS="mesa-vulkan-drivers libgl1-mesa-dri libegl-mesa0 vulkan-tools mesa-utils"
    [ "$DEMOS" = 1 ] && PKGS="$PKGS glmark2-wayland glmark2-es2-wayland"
    log "installing $PKGS"
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends $PKGS \
        || die "apt-get install failed -- see output above"
elif command -v pacman >/dev/null 2>&1; then
    log "Arch (pacman) detected"
    log "pacman -Sy"
    pacman -Sy --noconfirm || die "pacman -Sy failed -- check network/DNS (guest /etc/resolv.conf)"
    PKGS="vulkan-virtio mesa vulkan-tools mesa-utils"
    [ "$DEMOS" = 1 ] && PKGS="$PKGS glmark2"
    log "installing $PKGS"
    pacman -S --needed --noconfirm $PKGS || die "pacman -S failed -- see output above"
elif command -v apk >/dev/null 2>&1; then
    log "Alpine (apk) detected"
    # Alpine 3.23 and older build Mesa without Venus: there is no
    # mesa-vulkan-virtio to install, in any repository, and apk said only
    # that the package was not found (Discord, 2026-10-02). 3.24 has it.
    alpine_release=$(cat /etc/alpine-release 2>/dev/null)
    case "$alpine_release" in
        3.[0-9].*|3.1[0-9].*|3.2[0-3].*)
            die "Alpine $alpine_release has no GPU (Venus) driver: its Mesa is built without it. Alpine 3.24 and later, Devuan 6 and Arch have it -- install one of those in Settings > Filesystems." ;;
    esac
    # vulkan-loader by name: on Alpine neither the Venus driver nor
    # vulkan-tools depends on it (Devuan's and Arch's packages pull theirs
    # in), and without it vulkaninfo found no Vulkan at all and zink fell
    # back to software -- reported on Discord from Alpine 3.24, 2026-10-02.
    PKGS="mesa-vulkan-virtio vulkan-loader mesa-dri-gallium mesa-egl vulkan-tools mesa-utils"
    [ "$DEMOS" = 1 ] && note "Alpine packages no glmark2; es2gears_wayland (mesa-utils) is the demo here"
    log "installing $PKGS"
    apk_add $PKGS || die "apk add failed -- see output above"
else
    die "no supported package manager found (need apt-get, pacman, or apk)"
fi

# One OpenGL program on the GPU. Vulkan programs need none of this.
log "installing $GPU_RUN"
mkdir -p "$(dirname "$GPU_RUN")"
cat > "$GPU_RUN" <<'GPU_RUN_EOF'
#!/bin/sh
# gpu-run PROGRAM [ARG...] -- run an OpenGL program on the GPU (iSH-AOK),
# through zink on the Venus Vulkan driver. Installed by
# /AOK/tools/setup-gpu.sh. zink here offers OpenGL 2.1 / OpenGL ES 2.0;
# a program that needs more reports that it cannot get a context.
if [ $# -eq 0 ]; then
    echo "usage: gpu-run PROGRAM [ARG...]" >&2
    exit 2
fi
export LIBGL_ALWAYS_SOFTWARE=0
export MESA_LOADER_DRIVER_OVERRIDE=zink
# SDL programs pick X11 when DISPLAY is set; Wayland is the direct path.
if [ -n "${WAYLAND_DISPLAY:-}" ] && [ -z "${SDL_VIDEODRIVER:-}" ]; then
    export SDL_VIDEODRIVER=wayland
fi
exec "$@"
GPU_RUN_EOF
chmod 755 "$GPU_RUN"

# The desktop's default for OpenGL programs (start-wayland.sh reads it).
if [ -n "$GL_CHOICE" ]; then
    log "OpenGL programs in the desktop: $GL_CHOICE by default"
    {
        echo "# Written by /AOK/tools/setup-gpu.sh; start-wayland.sh reads it."
        echo "# AOK_GL=gpu: OpenGL programs in the Wayland desktop use the GPU (zink,"
        echo "# OpenGL 2.1 / ES 2.0). AOK_GL=software: llvmpipe (OpenGL 4.5)."
        echo "AOK_GL=$GL_CHOICE"
    } > "$CONF"
    note "takes effect in the next desktop session (close the Wayland window and reopen it)"
fi

check

log "done"
if [ "$CHECK_FAILED" = 0 ]; then
    note "Vulkan programs use the GPU as they are:   vkcube --wsi wayland"
    if [ "$GL_ON_GPU" = 1 ]; then
        note "OpenGL programs, one at a time:            gpu-run es2gears_wayland"
        [ "$DEMOS" = 1 ] && command -v glmark2-wayland >/dev/null 2>&1 &&
            note "benchmark (software, then the GPU):        glmark2-wayland; gpu-run glmark2-wayland"
    else
        note "OpenGL programs run in software here (see above); gpu-run changes nothing."
    fi
    note "The Wayland desktop composites on the GPU by itself from its next session."
else
    note "Something above is missing; see the lines marked 'no'."
    exit 1
fi
