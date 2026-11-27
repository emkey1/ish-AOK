#!/bin/sh
# aok-apt.sh [--root DIR] [--once] [--remove] [-q]
# ---------------------------------------------------------------------------
# Add AOK-Workstation's own apt repository to a Devuan 6 filesystem, or take
# it away again.
#
# The repository, https://emkey1.github.io/aok-apt (github.com/emkey1/aok-apt),
# holds packages built for Devuan 6 ("excalibur"), arm64 and amd64. Today
# that is PSCAL, as `pscal`: the pascal, clike, rea and aether languages,
# exsh and the PSCAL VM. Nothing is installed by adding the source; after
#       sudo apt update
#       sudo apt install pscal
#
# A native root's companion gets the source when setup-companion.sh prepares
# it, and an existing companion at its first `sudo apt` once this script is
# in the app (companion-run.sh runs it with --once). Any other Devuan 6
# filesystem, as root:
#       sudo sh /AOK/tools/aok-apt.sh
#
# What it writes into the filesystem:
#   /usr/share/keyrings/aok-apt.asc          the repository's public key
#   /etc/apt/sources.list.d/aok.sources      the source, trusted for that key only
#   /var/lib/aok/aok-apt                     a note that it was added once
#
# Options:
#   --root DIR   the filesystem to change. The default is this one, or in a
#                native root its companion.
#   --once       do nothing if the source was ever added here, even if it has
#                been removed since: removing it is a choice to respect
#   --remove     take the source and the key away. The note stays, so --once
#                does not bring them back.
#   -q           say nothing unless something changes or goes wrong
#   -h, --help   this text
#
# The key: ed25519 D127 47AE 9E6F 55A7 C139  BD6F E16C 5C31 AF98 5A69,
# "AOK-Workstation apt repository (github.com/emkey1/aok-apt)".
# ---------------------------------------------------------------------------
set -u

KEY=/AOK/tools/aok-apt.asc
URL=https://emkey1.github.io/aok-apt
SUITE=excalibur

ROOT=
ONCE=0
REMOVE=0
QUIET=0

say() { [ "$QUIET" = 1 ] || printf '%s\n' "$*"; }
die() { printf 'aok-apt.sh: %s\n' "$*" >&2; exit 1; }
usage() { awk '/^# ------/ { n++; next } n == 1 { sub(/^# ?/, ""); print }' "$0"; }

while [ $# -gt 0 ]; do
    case "$1" in
        --root) [ $# -ge 2 ] || die "--root needs a directory"; ROOT=$2; shift ;;
        --root=*) ROOT=${1#--root=} ;;
        --once) ONCE=1 ;;
        --remove) REMOVE=1 ;;
        -q|--quiet) QUIET=1 ;;
        -h|--help) usage; exit 0 ;;
        *) die "unknown option '$1' (--help lists them)" ;;
    esac
    shift
done

if [ -z "$ROOT" ]; then
    ROOT=/
    if [ -r /AOK/tools/companion-root.sh ]; then
        . /AOK/tools/companion-root.sh
        if aok_in_native_root; then
            aok_companion_root_find || die "$AOK_COMPANION_PROBLEM"
            ROOT=$AOK_COMPANION_DIR
        fi
    fi
fi
[ -d "$ROOT" ] || die "$ROOT is not a directory"
ROOT=${ROOT%/}
WHERE=${ROOT:-/}
[ -d "$ROOT/etc/apt" ] || die "$WHERE has no /etc/apt: it is not a Debian or Devuan filesystem"
[ "$(id -u)" = 0 ] || die "run it as root: sudo sh /AOK/tools/aok-apt.sh"

STAMP=$ROOT/var/lib/aok/aok-apt
SOURCES=$ROOT/etc/apt/sources.list.d/aok.sources
KEYRING=$ROOT/usr/share/keyrings/aok-apt.asc

if [ "$REMOVE" = 1 ]; then
    if [ -e "$SOURCES" ] || [ -e "$KEYRING" ]; then
        rm -f "$SOURCES" "$KEYRING"
        printf "Removed AOK-Workstation's package source from %s. Run: sudo apt update\n" "$WHERE"
    else
        say "AOK-Workstation's package source is not in $WHERE"
    fi
    mkdir -p "${STAMP%/*}" && : > "$STAMP"
    exit 0
fi

[ "$ONCE" = 1 ] && [ -e "$STAMP" ] && exit 0

# The packages are built for one release and two architectures.
codename=
if [ -r "$ROOT/etc/os-release" ]; then
    while IFS='=' read -r key value; do
        [ "$key" = VERSION_CODENAME ] || continue
        codename=$value
        codename=${codename#\"}
        codename=${codename%\"}
    done < "$ROOT/etc/os-release"
fi
case "$codename" in
    excalibur|trixie) ;;
    *)
        [ "$ONCE" = 1 ] && exit 0
        die "its packages are built for Devuan 6 (excalibur); $WHERE is ${codename:-not a release this can tell}"
        ;;
esac
arch=
in_dpkg=0
if [ -r "$ROOT/var/lib/dpkg/status" ]; then
    while IFS= read -r line; do
        case "$line" in
            'Package: dpkg') in_dpkg=1 ;;
            'Package: '*) in_dpkg=0 ;;
            'Architecture: '*) [ "$in_dpkg" = 1 ] && { arch=${line#Architecture: }; break; } ;;
        esac
    done < "$ROOT/var/lib/dpkg/status"
fi
case "$arch" in
    arm64|amd64) ;;
    *)
        [ "$ONCE" = 1 ] && exit 0
        die "its packages are built for arm64 and amd64; $WHERE is ${arch:-of an architecture this cannot tell}"
        ;;
esac

[ -r "$KEY" ] || die "$KEY is missing"
mkdir -p "${KEYRING%/*}" "${SOURCES%/*}" "${STAMP%/*}" || die "cannot write into $WHERE"
cp "$KEY" "$KEYRING" && chmod 644 "$KEYRING" || die "cannot write $KEYRING"
cat > "$SOURCES" <<EOF || die "cannot write $SOURCES"
# AOK-Workstation's own packages, built for Devuan 6: PSCAL is \`pscal\`.
# https://github.com/emkey1/aok-apt. Written by /AOK/tools/aok-apt.sh;
# \`sudo sh /AOK/tools/aok-apt.sh --remove\` takes it away again.
Types: deb
URIs: $URL
Suites: $SUITE
Components: main
Signed-By: /usr/share/keyrings/aok-apt.asc
EOF
chmod 644 "$SOURCES"
printf '%s\n' "added $(date -u '+%Y-%m-%d %H:%M:%S') UTC by /AOK/tools/aok-apt.sh" > "$STAMP"

printf "Added AOK-Workstation's package source to %s (PSCAL: sudo apt update, then sudo apt install pscal)\n" "$WHERE"
# https needs the CA certificates, which a minimal filesystem can lack.
if [ ! -s "$ROOT/etc/ssl/certs/ca-certificates.crt" ]; then
    printf '%s\n' "  It is served over https, and $WHERE has no CA certificates yet:" \
        "  sudo apt install ca-certificates" >&2
fi
exit 0
