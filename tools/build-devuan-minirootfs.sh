#!/bin/sh
# build-devuan-minirootfs.sh
# ---------------------------------------------------------------------------
# Build *minimal* Devuan 6 "Excalibur" rootfs tarballs for iSH-AOK, the Devuan
# analog of the bundled `alpine-minirootfs-3.23.3-x86{,_64}.tar.gz` images.
#
# Produces (in OUT_DIR, default = repo root):
#   devuan-minirootfs-6.0-x86.tar.gz      i386  (i686)
#   devuan-minirootfs-6.0-x86_64.tar.gz   amd64 (x86_64)
#   devuan-minirootfs-6.0-aarch64.tar.gz  arm64 (aarch64) -- matches the
#                                         alpine-minirootfs-*-aarch64 naming
#   devuan-minirootfs-6.0-riscv64.tar.gz  riscv64 (RV64GC)
#
# Strategy
# --------
# We bootstrap straight from the Devuan "merged" archive with `mmdebstrap
# --variant=minbase`, which yields a bare apt/dpkg/glibc/coreutils/bash system
# -- exactly like the Alpine minirootfs -- so that provision-ultimate-devuan.sh
# can do all the "ultimate terminal" heavy lifting on top.
#
# mmdebstrap runs inside a *matching-architecture* Debian container: on an
# amd64 host Docker emulates linux/386 and linux/arm64 via binfmt/qemu; on an
# arm64 host (this project's actual dev machine) linux/arm64 runs NATIVELY and
# only linux/386 and linux/amd64 need emulation. Either way, matching the
# container arch to the target rootfs arch means the package maintainer
# scripts run "native" to the (possibly emulated) container -- no fragile
# nested qemu-user binfmt setup. The Devuan archive keyring is lifted out of
# the official `dyne/devuan:excalibur` image so the Release signature verifies.
#
# This keeps the same provenance philosophy as the existing amd64 docker-export
# images (Linux-native uid/gid/perms in the tar) while being (a) genuinely
# minimal and (b) buildable for i386/arm64, which the amd64-only dyne image is
# not.
#
# Usage:
#   tools/build-devuan-minirootfs.sh                # all arches
#   ARCHES=i386 tools/build-devuan-minirootfs.sh    # just one
#   ARCHES=arm64 tools/build-devuan-minirootfs.sh   # just arm64
#   OUT_DIR=/tmp tools/build-devuan-minirootfs.sh   # elsewhere
#
# Tunables (env):
#   ARCHES         space list of deb arches to build   (default "i386 amd64 arm64 riscv64")
#   SUITE          Devuan suite                         (default excalibur)
#   VERSION        version string used in filenames     (default 6.0)
#   DEVUAN_MIRROR  archive mirror                        (default deb.devuan.org)
#   BUILD_IMAGE    Debian image that hosts mmdebstrap   (default debian:trixie)
#   OUT_DIR        where the .tar.gz land               (default repo root)
#   KEEP_APT_LISTS 1 to keep /var/lib/apt/lists (bigger) (default unset = strip)
#   EXTRA_PKGS     comma list appended to the bootstrap package set (default
#                  empty). Installed by mmdebstrap in the natively-run (or
#                  Docker-emulated) build container, which is minutes rather
#                  than the hours the same apt-get takes inside an iSH guest --
#                  e.g. EXTRA_PKGS=gcc,libc6-dev,make for a rootfs that can
#                  build the tests/manual regression suite on first boot.
#   SLOW_HASH      1 to keep Devuan's default yescrypt password hashing
#                  instead of downgrading to sha512crypt (default unset =
#                  downgrade). yescrypt is deliberately memory-hard, which is
#                  close to worst-case for a JIT-emulated guest -- measured
#                  ~1.15s of added login latency on fast Apple Silicon, and
#                  minutes on an older A10X. sha512crypt is pure ALU work and
#                  still a reasonable scheme for a personal terminal, so it's
#                  the default here; set SLOW_HASH=1 if you want the stronger
#                  (and slower-under-emulation) scheme instead.
# ---------------------------------------------------------------------------
set -eu

SUITE="${SUITE:-excalibur}"
VERSION="${VERSION:-6.0}"
DEVUAN_MIRROR="${DEVUAN_MIRROR:-http://deb.devuan.org/merged}"
BUILD_IMAGE="${BUILD_IMAGE:-debian:trixie}"
ARCHES="${ARCHES:-i386 amd64 arm64 riscv64}"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT_DIR="${OUT_DIR:-$REPO_ROOT}"
KEYRING_IMAGE="${KEYRING_IMAGE:-dyne/devuan:excalibur}"

log()  { printf '\n\033[1;36m==>\033[0m \033[1m%s\033[0m\n' "$*"; }
note() { printf '    %s\n' "$*"; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

command -v docker >/dev/null 2>&1 || die "docker not found in PATH"
docker info >/dev/null 2>&1 || die "docker daemon is not running"

# --- output compression -----------------------------------------------------
# iSH-AOK imports via libarchive (archive_read_support_filter_all in
# tools/fakefs.c), which reads gzip/xz/zstd/bzip2 transparently, and the import
# decompresses *natively* and only once -- so the smallest file wins. xz -9e is
# ~39% smaller than gzip on this rootfs (30 MB vs 50 MB). Override with
# COMPRESS=zst (faster, ~32 MB) or COMPRESS=gz.
COMPRESS="${COMPRESS:-xz}"
case "$COMPRESS" in
    xz)  COMP_EXT="tar.xz";  COMP_TOOL="xz";   COMP_CMD="xz -9e -c" ;;
    zst) COMP_EXT="tar.zst"; COMP_TOOL="zstd"; COMP_CMD="zstd -19 -c" ;;
    gz)  COMP_EXT="tar.gz";  COMP_TOOL="gzip"; COMP_CMD="gzip -9 -c" ;;
    *)   die "unknown COMPRESS='$COMPRESS' (use one of: xz zst gz)" ;;
esac
command -v "$COMP_TOOL" >/dev/null 2>&1 || die "compressor '$COMP_TOOL' not found in PATH"

# i386 -> x86, amd64 -> x86_64, arm64 -> aarch64, riscv64 -> riscv64 (match the
# Alpine minirootfs filename convention -- see
# alpine-minirootfs-3.23.3-aarch64.tar.xz / -riscv64.tar.xz).
arch_suffix() {
    case "$1" in
        i386)  echo x86 ;;
        amd64) echo x86_64 ;;
        arm64) echo aarch64 ;;
        *)     echo "$1" ;;
    esac
}
# deb arch -> docker --platform value
arch_platform() {
    case "$1" in
        i386)    echo linux/386 ;;
        amd64)   echo linux/amd64 ;;
        arm64)   echo linux/arm64 ;;
        riscv64) echo linux/riscv64 ;;
        *)       die "unknown arch '$1'" ;;
    esac
}

WORK="$(mktemp -d "${TMPDIR:-/tmp}/devuan-minirootfs.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$OUT_DIR"

# --- Devuan archive keyring (extracted from the official Devuan image) --------
# `docker create` + `docker cp` instead of `docker run ... cat`: copying a file
# out of an image needs no process, and running one here meant running the
# image's ENTRYPOINT under emulation. dyne/devuan is amd64-only and its tini
# entrypoint aborts on an Apple Silicon host -- "[FATAL tini (1)]
# PR_SET_CHILD_SUBREAPER is unavailable on this platform" -- so every build on
# this project's actual dev machine died here before bootstrapping anything.
log "Fetching the Devuan archive keyring from $KEYRING_IMAGE"
docker pull --platform=linux/amd64 "$KEYRING_IMAGE" >/dev/null
keyring_cid="$(docker create --platform=linux/amd64 "$KEYRING_IMAGE")" \
    || die "could not create a container from $KEYRING_IMAGE"
docker cp "$keyring_cid:/usr/share/keyrings/devuan-archive-keyring.gpg" \
    "$WORK/devuan-archive-keyring.gpg" >/dev/null 2>&1 || true
docker rm "$keyring_cid" >/dev/null 2>&1 || true
[ -s "$WORK/devuan-archive-keyring.gpg" ] || die "could not extract the Devuan keyring"
note "keyring: $(wc -c < "$WORK/devuan-archive-keyring.gpg") bytes"

# Optionally drop the apt package lists to shrink the image (the guest will run
# `apt-get update` anyway, exactly like Alpine needs `apk update`).
CLEAN_HOOK='rm -rf "$1"/var/lib/apt/lists/* "$1"/var/cache/apt/archives/*.deb "$1"/var/cache/apt/*.bin 2>/dev/null || true'
[ "${KEEP_APT_LISTS:-}" = 1 ] && CLEAN_HOOK='true'

# busybox-static (~2 MB) gives the bare image the interactive/monitoring tools a
# Devuan minbase lacks (ps/top/watch/free/wget/less/...) -- usable offline,
# before provision-ultimate-devuan.sh runs apt. We symlink ONLY those gap
# applets into /usr/bin: never the coreutils/dash/bash/sysvinit names, and never
# update-alternatives-managed names (vi/editor/pager) which would fight a later
# vim/less install. Because Excalibur is usr-merged, dpkg cleanly replaces each
# symlink with the real binary when its package is installed (procps/psmisc/
# wget/less), so busybox is a self-healing fallback, never a shadow. Set
# BUSYBOX_APPLETS="" to skip the symlinks (busybox is still callable as
# `busybox <applet>`); set BUSYBOX=0 to omit busybox entirely.
BUSYBOX="${BUSYBOX:-1}"
BUSYBOX_APPLETS="${BUSYBOX_APPLETS-top watch ps free vmstat pgrep pkill killall wget less}"
# logsave: initscripts' checkroot.sh/checkfs.sh persist fsck output through
# /sbin/logsave (split out of e2fsprogs into its own package in Debian 13/
# Devuan Excalibur). Without it every boot prints the cosmetic-but-alarming
# "Checking file systems...Cannot persist the following output on disc ...
# failed!" from mount-functions.sh's logsave_best_effort().
#
# vim-tiny: the bare minbase + busybox-static image had NO command named `vi`
# at all -- busybox-static does compile a vi applet, but the customize hook
# below deliberately never symlinks it (see its comment) to avoid contending
# with update-alternatives, so it was reachable only via `busybox vi`, not
# plain `vi`. vim-tiny is Debian's real minimal-vi package: it registers
# /usr/bin/vi through update-alternatives properly on install (auto mode, no
# manual symlink to fight), and provision-ultimate-devuan.sh's later `apt-get
# install vim` cleanly takes over the same alternative at a higher priority
# (verified: `update-alternatives: using /usr/bin/vim.basic to provide
# /usr/bin/vi ... in auto mode`, no warnings). ~2.1 MB installed.
if [ "$BUSYBOX" = 1 ]; then
    INCLUDE_PKGS="ca-certificates,devuan-keyring,busybox-static,logsave,vim-tiny"
    # Only symlink applets the target busybox actually provides (it's the same
    # arch as the build container, so it runs) -- a symlink to a non-compiled
    # applet would print "applet not found" and look broken. Never overwrite an
    # existing target file.
    BBOX_HOOK='[ -e "$1/bin/busybox" ] || exit 0; avail=$("$1/bin/busybox" --list 2>/dev/null); for ap in '"$BUSYBOX_APPLETS"'; do printf "%s\n" "$avail" | grep -qx "$ap" || { echo "    busybox: no applet $ap, skipping" >&2; continue; }; [ -e "$1/usr/bin/$ap" ] || [ -e "$1/bin/$ap" ] || ln -s /bin/busybox "$1/usr/bin/$ap"; done'
else
    INCLUDE_PKGS="ca-certificates,devuan-keyring,logsave,vim-tiny"
    BBOX_HOOK='true'
fi
[ -n "${EXTRA_PKGS:-}" ] && INCLUDE_PKGS="$INCLUDE_PKGS,$EXTRA_PKGS"

# Devuan/Debian's shadow package defaults new password hashes to yescrypt --
# deliberately memory-hard (its whole point: resist GPU/ASIC cracking), which
# makes it one of the worst possible workloads for a JIT-emulated guest: every
# pseudo-random scratch-buffer access pays the emulator's address-translation
# overhead, on top of already-slower host silicon on older devices. Measured
# on a fast Apple Silicon Mac, a yescrypt login added ~1.15s over key-based
# auth; on an A10X iPad this stretched into minutes. sha512crypt is still a
# perfectly reasonable scheme for a personal terminal environment (thousands
# of SHA-512 rounds, no deliberate memory-hardness) and is pure ALU work, so
# it doesn't hit the same emulation penalty. Both the PAM line AND
# login.defs need patching: pam_unix.so's own argument (used by anything
# going through PAM -- login, sshd, sudo) takes precedence over login.defs,
# but login.defs is what non-PAM tools (useradd, chpasswd's libc path) fall
# back to, so leaving it at YESCRYPT would silently reintroduce yescrypt for
# any user/password created outside the PAM stack. Set SLOW_HASH=1 to keep
# the yescrypt default (e.g. if you want the stronger scheme and don't mind
# the emulation cost).
SLOW_HASH="${SLOW_HASH:-0}"
if [ "$SLOW_HASH" = 1 ]; then
    CRYPT_HOOK='true'
else
    CRYPT_HOOK='f="$1/etc/pam.d/common-password"; [ -f "$f" ] && sed -i "s/\bpam_unix\.so \(.*\)\byescrypt\b/pam_unix.so \1sha512/" "$f"; f="$1/etc/login.defs"; [ -f "$f" ] && sed -i "s/^ENCRYPT_METHOD[[:space:]]\+YESCRYPT/ENCRYPT_METHOD SHA512/" "$f"; true'
fi

build_one() {  # <deb-arch>
    arch="$1"
    suffix="$(arch_suffix "$arch")"
    platform="$(arch_platform "$arch")"
    out="$OUT_DIR/devuan-minirootfs-$VERSION-$suffix.$COMP_EXT"

    log "Building $arch ($platform) -> ${out##*/}"
    # mmdebstrap writes an uncompressed tar inside the container; we compress it
    # ($COMPRESS) on the host for the smallest possible bundle.
    docker run --rm --privileged --platform="$platform" \
        -e SUITE="$SUITE" -e ARCH="$arch" -e MIRROR="$DEVUAN_MIRROR" \
        -e CLEAN_HOOK="$CLEAN_HOOK" -e BBOX_HOOK="$BBOX_HOOK" -e CRYPT_HOOK="$CRYPT_HOOK" \
        -e INCLUDE_PKGS="$INCLUDE_PKGS" \
        -v "$WORK:/work" \
        "$BUILD_IMAGE" bash -euc '
            export DEBIAN_FRONTEND=noninteractive
            apt-get update -qq
            apt-get install -y -qq --no-install-recommends mmdebstrap >/dev/null
            # mmdebstrap drives apt with a Pre-Install-Pkgs hook ("cat >&14")
            # that redirects to a 2-digit fd. The container /bin/sh is dash,
            # which cannot parse ">&14" ("Bad fd number"); bash can. Point
            # /bin/sh at bash so the apt hook works (chroot maintainer scripts
            # run the *target* /bin/sh, so this only affects the outer hook).
            ln -sf /bin/bash /bin/sh
            : "${ARCH:?}" "${SUITE:?}" "${MIRROR:?}"
            echo "    mmdebstrap $(mmdebstrap --version 2>/dev/null | head -1)"
            mmdebstrap \
                --mode=root \
                --variant=minbase \
                --format=tar \
                --components=main \
                --arch="$ARCH" \
                --keyring=/work/devuan-archive-keyring.gpg \
                --include="$INCLUDE_PKGS" \
                --aptopt="Acquire::Check-Valid-Until \"false\"" \
                --customize-hook="$BBOX_HOOK" \
                --customize-hook="$CRYPT_HOOK" \
                --customize-hook="$CLEAN_HOOK" \
                "$SUITE" "/work/rootfs-$ARCH.tar" \
                "deb $MIRROR $SUITE main"
        '

    [ -s "$WORK/rootfs-$arch.tar" ] || die "mmdebstrap produced no tar for $arch"
    $COMP_CMD "$WORK/rootfs-$arch.tar" > "$out"
    rm -f "$WORK/rootfs-$arch.tar"

    # --- sanity: read os-release + arch back out of the finished tarball ------
    # tar -xO without an explicit filter: bsdtar/libarchive auto-detects xz/gz/zst.
    note "wrote $out ($(ls -lh "$out" | awk '{print $5}'))"
    osline="$(tar -xOf "$out" ./usr/lib/os-release 2>/dev/null | grep -E '^PRETTY_NAME=' || true)"
    dpkgarch="$(tar -xOf "$out" ./var/lib/dpkg/arch 2>/dev/null | head -1 || true)"
    note "  ${osline:-<no os-release>}"
    note "  dpkg arch: ${dpkgarch:-<unknown>}"
    [ "$dpkgarch" = "$arch" ] || note "  WARNING: dpkg arch '$dpkgarch' != requested '$arch'"
    cryptline="$(tar -xOf "$out" ./etc/login.defs 2>/dev/null | grep -E '^ENCRYPT_METHOD' || true)"
    note "  ${cryptline:-<no ENCRYPT_METHOD line>}"
    if [ "$SLOW_HASH" != 1 ]; then
        case "$cryptline" in
            *SHA512*) ;;
            *) note "  WARNING: expected ENCRYPT_METHOD SHA512 (CRYPT_HOOK may not have run)" ;;
        esac
    fi
}

for a in $ARCHES; do
    build_one "$a"
done

log "Done"
note "Images in $OUT_DIR:"
for a in $ARCHES; do
    f="$OUT_DIR/devuan-minirootfs-$VERSION-$(arch_suffix "$a").$COMP_EXT"
    [ -f "$f" ] && note "  $(ls -lh "$f" | awk '{print $5"  "$NF}')"
done
cat <<EOF

    Next:
      * Smoke-test locally:
          (cd build && ./tools/fakefsify $OUT_DIR/devuan-minirootfs-$VERSION-x86.$COMP_EXT devuan-x86)
          ./build/ish -f build/devuan-x86 /bin/sh
      * Provision into a full system from inside the guest:
          sh /AOK/tools/provision-ultimate-devuan.sh
EOF
