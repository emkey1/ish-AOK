#!/bin/sh
# build-wayvnc.sh
# ---------------------------------------------------------------------------
# Build wayvnc 0.10.2 (with neatvnc 1.0.2 and aml 1.0.0, private to it) for
# /AOK/bundled. Devuan/Debian only.
#
# Why: the Display applet's GPU desktop detaches wayvnc while the app shows the
# compositor's frames itself (wl-present), and wayvnc 0.9.1 -- what Devuan 6
# and Debian 13 ship -- can die on that detach. It frees its buffer pool while
# neatvnc still holds a frame from it, and the frame's release writes into the
# freed pool ("malloc_consolidate(): unaligned fastbin chunk detected"); a
# resize just before the detach, which is how every session starts, makes it
# likely. Reproduced on the M4 iPad (2026-10-03): 0.9.1 died in 5 of 15
# rounds, 0.10.2 in none of 35. wayvnc 0.10.0 fixed it upstream (2897d15,
# 5d7784d), and 0.10 needs neatvnc 1.x and aml 1.x, which Devuan does not have.
#
# The build is for 127.0.0.1 only, as start-wayland.sh runs it: no TLS, no
# authentication, no PAM, no H.264. JPEG and GBM (dmabuf capture) are kept, as
# the packaged wayvnc has them.
#
# Usage:
#   sh /AOK/tools/wayland/build-wayvnc.sh --out DIR
#       Builds and leaves in DIR: wayvnc, wayvncctl, libneatvnc.so.1,
#       libaml.so.1 (the two programs find the libraries beside them),
#       wayvnc.source (the versions) and wayvnc.depends (the Debian packages
#       its other libraries come from). Installs build dependencies, so run it
#       as root. tools/build-bundled.sh runs it on a Mac, in the CLI's Devuan
#       roots; start-wayland.sh uses the result from /AOK/bundled.
# ---------------------------------------------------------------------------
set -u

log()  { printf '\n\033[1;36m==>\033[0m \033[1m%s\033[0m\n' "$*"; }
die()  { printf 'build-wayvnc.sh: %s\n' "$*" >&2; exit 1; }

AML_TAG=v1.0.0
NEATVNC_TAG=v1.0.2
WAYVNC_TAG=v0.10.2

[ "${1:-}" = --out ] && [ $# -ge 2 ] || die "usage: build-wayvnc.sh --out DIR"
OUT=$2
[ "$(id -u)" = 0 ] || die "must run as root (it installs build dependencies)"
command -v apt-get >/dev/null 2>&1 || die "Devuan/Debian only"

log "installing build dependencies"
apt-get update -q || true
DEBIAN_FRONTEND=noninteractive apt-get install -y -q --no-install-recommends \
    build-essential meson ninja-build pkg-config git ca-certificates patchelf \
    libpixman-1-dev zlib1g-dev libturbojpeg0-dev libdrm-dev libgbm-dev \
    libjansson-dev libxkbcommon-dev libwayland-dev wayland-protocols \
    || die "apt-get install failed"

WORK=$(mktemp -d /tmp/build-wayvnc.XXXXXX) || die "no work directory"
trap 'rm -rf "$WORK"' EXIT INT TERM
PREFIX=$WORK/prefix
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"

# build NAME URL TAG [meson options...]
build() {
    _name=$1; _url=$2; _tag=$3; shift 3
    log "building $_name $_tag"
    git -c advice.detachedHead=false clone -q --depth 1 --branch "$_tag" "$_url" "$WORK/$_name" \
        || die "could not clone $_name $_tag"
    ( cd "$WORK/$_name" \
        && meson setup build --prefix="$PREFIX" --libdir=lib --buildtype=release "$@" \
        && ninja -C build && ninja -C build install ) >"$WORK/$_name.log" 2>&1 \
        || { tail -30 "$WORK/$_name.log"; die "$_name $_tag failed to build"; }
}

build aml https://github.com/any1/aml.git "$AML_TAG"
build neatvnc https://github.com/any1/neatvnc.git "$NEATVNC_TAG" \
    -Dtls=disabled -Dnettle=disabled -Dh264=disabled -Djpeg=enabled -Dgbm=enabled \
    -Dexamples=false -Dbenchmarks=false -Dtests=false
build wayvnc https://github.com/any1/wayvnc.git "$WAYVNC_TAG" \
    -Dpam=disabled -Dman-pages=disabled -Dscreencopy-dmabuf=enabled -Dtests=false

log "collecting into $OUT"
mkdir -p "$OUT" || die "cannot make $OUT"
cp "$PREFIX/bin/wayvnc" "$PREFIX/bin/wayvncctl" "$OUT/" || die "no programs built"
cp -L "$PREFIX/lib/libneatvnc.so.1" "$PREFIX/lib/libaml.so.1" "$OUT/" || die "no libraries built"
for f in wayvnc wayvncctl libneatvnc.so.1 libaml.so.1; do
    strip --strip-unneeded "$OUT/$f"
    # The libraries beside the programs, wherever /AOK/bundled is read from.
    patchelf --set-rpath '$ORIGIN' "$OUT/$f" || die "patchelf failed on $f"
done
# Their licences (ISC), which travel with the binaries.
{
    echo "The wayvnc in this directory is built from these three projects (opt/AOK/tools/wayland/build-wayvnc.sh)."
    for _p in aml neatvnc wayvnc; do
        echo; echo "==== $_p (https://github.com/any1/$_p)"; cat "$WORK/$_p/COPYING"
    done
} > "$OUT/wayvnc.COPYING"
printf 'wayvnc %s neatvnc %s aml %s\n' "${WAYVNC_TAG#v}" "${NEATVNC_TAG#v}" "${AML_TAG#v}" > "$OUT/wayvnc.source"
# The Debian packages of every other library they load: what a root installs
# (natively, or as arm64 through multiarch on a root of another architecture).
for f in wayvnc wayvncctl libneatvnc.so.1 libaml.so.1; do
    readelf -d "$OUT/$f" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p'
done | grep -v -x -e libneatvnc.so.1 -e libaml.so.1 | sort -u | while read -r l; do
    dpkg -S "*/$l" 2>/dev/null | head -n 1 | cut -d: -f1
done | sort -u > "$OUT/wayvnc.depends"
"$OUT/wayvnc" -V || die "the collected wayvnc does not run"
log "done"
