#!/bin/sh
# companion-run.sh COMMAND [ARGS...]
# ---------------------------------------------------------------------------
# Run a command of a native root's companion filesystem, from the native root.
#
# This is what /usr/bin/apt, apt-get, apt-cache, apt-mark, dpkg and dpkg-query
# are in a native-mode root: a native root has no package manager, so those
# names hand off to the companion's own (setup-companion.sh,
# /AOK/docs/native-companion.md). With no companion yet, apt and apt-get offer
# to install one.
#
# Everything else the companion has needs no wrapper: its bin directories
# follow this root's on PATH (/etc/profile.d/20-aok-companion.sh), and a
# program named by its path there runs inside it.
# ---------------------------------------------------------------------------
set -u

. /AOK/tools/companion-root.sh

die() { printf '%s\n' "$*" >&2; exit 1; }

[ $# -ge 1 ] || die "usage: companion-run.sh COMMAND [ARGS...]"
cmd=$1
shift

aok_in_native_root || die "$cmd: this is for a native-mode root"

if ! aok_companion_root_find; then
    [ "$AOK_COMPANION_PROBLEM_KIND" = none ] || die "$cmd: $AOK_COMPANION_PROBLEM"
    case "$cmd" in
        apt|apt-get) ;;
        *) die "$cmd: $AOK_COMPANION_PROBLEM" ;;
    esac
    cat >&2 <<'NOTE'
A native filesystem has no packages of its own. apt works in its companion:
a Devuan filesystem installed beside this one, where packages are kept and
run from under emulation, found by name from here. It is not installed yet
(a download of a few hundred MB).
NOTE
    [ "$(id -u)" = 0 ] || die "Install it with: sudo sh /AOK/tools/setup-companion.sh"
    [ -t 0 ] || die "Install it with: sh /AOK/tools/setup-companion.sh"
    printf 'Install it now? [y/N] ' >&2
    IFS= read -r answer || answer=
    case "$answer" in
        y|Y|yes|Yes|YES) ;;
        *) die "Not installed. Later: sudo sh /AOK/tools/setup-companion.sh" ;;
    esac
    sh /AOK/tools/setup-companion.sh || exit $?
    aok_companion_root_find || die "$cmd: $AOK_COMPANION_PROBLEM"
    # Nothing but the install was asked for.
    [ $# -gt 0 ] || exit 0
fi

path=$(aok_companion_which "$AOK_COMPANION_DIR" "$cmd") ||
    die "$cmd: not in the companion filesystem (${AOK_COMPANION_DIR##*/})"
# A companion set up before AOK-Workstation had its own package source gets it
# here, once, at its first apt as root (aok-apt.sh, which says so when it does).
case "$cmd" in
    apt|apt-get)
        [ "$(id -u)" = 0 ] && sh /AOK/tools/aok-apt.sh --root "$AOK_COMPANION_DIR" --once -q >&2
        ;;
esac
aok_companion_env
exec "$path" "$@"
