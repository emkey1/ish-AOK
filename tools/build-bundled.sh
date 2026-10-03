#!/bin/sh
# tools/build-bundled.sh -- build what /AOK/bundled serves, on a Mac.
#
# /AOK/bundled holds programs and libraries for the guest that iSH-AOK builds
# ahead of time, so that setup scripts can link them in instead of compiling on
# the device: wf-panel's build ran an A10X iPad out of memory, and the shims
# want a compiler most roots lack. They are built here, in the CLI's guest
# roots, with the same scripts a device would run (opt/AOK/tools), and the
# results are committed under opt/AOK/bundled/ and listed in
# fs/aok-bundled.manifest.
#
#   tools/build-bundled.sh [TARGET...]     default: all of them
#
# Targets, and the root each is built in (override with the variable):
#   devuan6-aarch64   wf-panel            DEVUAN_ARM64_ROOT   (build/devuan-arm64-desk)
#   glibc-aarch64     the two shims       DEVUAN_ARM64_ROOT
#   glibc-x86_64      the two shims       DEVUAN_X86_64_ROOT
#   musl-aarch64      the two shims       ALPINE_ARM64_ROOT   (build/alpine-arm64-324)
#   musl-x86_64       the two shims       ALPINE_X86_64_ROOT  (build/alpine-amd64-test)
#
# The shims are libish-wl-release-guard.so (tools/wayland) and libish-pixman.so
# (tools/pixman). wf-panel is wf-shell's, patched (tools/wayland/build-wf-panel.sh),
# and is only good for the wf-shell version in its wf-panel.source: rerun this
# when Devuan updates wf-shell. It is built for arm64 only, the guest that runs
# fastest; the shims are small enough to carry for x86_64 too. ISH names the
# CLI binary (default build/ish); it must be built from this tree, since the
# guest scripts come from its /AOK/tools. The roots need network for apt.
set -eu

top=$(cd "$(dirname "$0")/.." && pwd)
ISH=${ISH:-$top/build/ish}
DEVUAN_ARM64_ROOT=${DEVUAN_ARM64_ROOT:-$top/build/devuan-arm64-desk}
DEVUAN_X86_64_ROOT=${DEVUAN_X86_64_ROOT:-$top/build/devuan-amd64-test}
ALPINE_ARM64_ROOT=${ALPINE_ARM64_ROOT:-$top/build/alpine-arm64-324}
ALPINE_X86_64_ROOT=${ALPINE_X86_64_ROOT:-$top/build/alpine-amd64-test}
OUT=$top/opt/AOK/bundled

[ -x "$ISH" ] || { echo "no CLI binary at $ISH (set ISH=)" >&2; exit 1; }

# guest ROOT TARGET SCRIPT: run SCRIPT (sh) in ROOT with $OUT/TARGET as /realmnt.
guest() {
    mkdir -p "$OUT/$2"
    echo "== $2 (in $1)"
    ISH_REAL_MNT="$OUT/$2" "$ISH" -f "$1" /bin/sh -c "$3"
}

shims='set -e
command -v cc >/dev/null 2>&1 || { if command -v apk >/dev/null; then apk add -q build-base; else DEBIAN_FRONTEND=noninteractive apt-get install -y -q --no-install-recommends gcc libc6-dev; fi; }
sh /AOK/tools/wayland/build-release-guard.sh /tmp/aok-bundled
sh /AOK/tools/pixman/build-shim.sh /tmp/aok-bundled
for f in libish-wl-release-guard.so libish-pixman.so; do
    strip --strip-unneeded /tmp/aok-bundled/$f 2>/dev/null || true
    cp /tmp/aok-bundled/$f /realmnt/$f
done
rm -rf /tmp/aok-bundled'

panel='set -e
command -v wf-panel >/dev/null 2>&1 || { apt-get update -q && DEBIAN_FRONTEND=noninteractive apt-get install -y -q --no-install-recommends wf-shell; }
sh /AOK/tools/wayland/build-wf-panel.sh --out /realmnt'

targets=${*:-"devuan6-aarch64 glibc-aarch64 glibc-x86_64 musl-aarch64 musl-x86_64"}
for t in $targets; do
    case "$t" in
        devuan6-aarch64) guest "$DEVUAN_ARM64_ROOT" "$t" "$panel" ;;
        glibc-aarch64)   guest "$DEVUAN_ARM64_ROOT" "$t" "$shims" ;;
        glibc-x86_64)    guest "$DEVUAN_X86_64_ROOT" "$t" "$shims" ;;
        musl-aarch64)    guest "$ALPINE_ARM64_ROOT" "$t" "$shims" ;;
        musl-x86_64)     guest "$ALPINE_X86_64_ROOT" "$t" "$shims" ;;
        *) echo "unknown target $t" >&2; exit 1 ;;
    esac
done
ls -la "$OUT"/*/
