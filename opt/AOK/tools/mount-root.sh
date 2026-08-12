#!/bin/sh
# mount-root.sh
# ---------------------------------------------------------------------------
# Make one -- or every -- root exposed under /AOK/roots/* fully usable via
# chroot: bind-mount the live /proc, /sys, /dev and /dev/pts (and /run) from
# the currently-booted root into it, the same setup any Linux chroot needs
# for process/tty/device-aware tools (top, ps, free, tty, ...) to work. Also
# bind-mounts /AOK/tools in, so these same tools (including this script) are
# reachable from inside the chroot too.
#
# iSH-AOK has no mount or PID namespaces, so /proc et al. always reflect the
# one true kernel state regardless of which root you bind them into -- `top`
# run inside a chroot sees exactly the same processes as `top` run outside
# it. Each installed root other than the booted one is auto-exposed
# read-write at /AOK/roots/<name> at boot; this script just wires up the
# bind mounts a chroot needs on top of that.
#
# Usage:
#   mount-root.sh <name>              set up mounts, then chroot in (shell)
#   mount-root.sh <name> -- CMD...    set up mounts, then chroot in and run CMD
#   mount-root.sh all                 set up mounts for every /AOK/roots/* root
#   mount-root.sh all -- CMD...       set up mounts, then run CMD in each root in turn
#   mount-root.sh --unmount <name>    tear the bind mounts back down
#   mount-root.sh --unmount all       tear down every /AOK/roots/* root
#
# Examples:
#   mount-root.sh Devuan6-x86_64                  # interactive shell
#   mount-root.sh Devuan6-x86_64 -- top -bn1       # one-shot snapshot
#   mount-root.sh all -- uname -a                  # sanity-check every root
#   mount-root.sh --unmount all
#
# Must run as root (mount/chroot require it).
# ---------------------------------------------------------------------------
set -u

ROOTS_DIR=${ROOTS_DIR:-/AOK/roots}
BIND_DIRS="proc sys dev dev/pts run AOK/tools"

log()  { printf '\n\033[1;36m==>\033[0m \033[1m%s\033[0m\n' "$*"; }
note() { printf '    %s\n' "$*"; }
die()  { printf 'mount-root.sh: %s\n' "$*" >&2; exit 1; }

if [ "$(id -u)" != 0 ]; then
    die "must run as root:  sudo sh $0 ...  (or as root directly)"
fi

CHROOT=$(command -v chroot || true)
[ -n "$CHROOT" ] || for c in /usr/sbin/chroot /sbin/chroot; do
    [ -x "$c" ] && CHROOT="$c" && break
done
[ -n "$CHROOT" ] || die "no chroot binary found"

is_mounted() {
    # Exact-match the target field (2nd column) of /proc/mounts -- more
    # robust than substring grep against paths that happen to prefix one
    # another (e.g. /AOK/roots/foo vs /AOK/roots/foo-bar).
    awk -v t="$1" '$2 == t { found=1 } END { exit !found }' /proc/mounts
}

mount_one_dir() {
    src=$1 dst=$2
    mkdir -p "$dst" 2>/dev/null
    if is_mounted "$dst"; then
        note "already mounted: $dst"
        return 0
    fi
    if mount --bind "$src" "$dst" 2>/dev/null; then
        note "bind-mounted $src -> $dst"
    else
        note "WARNING: failed to bind-mount $src -> $dst"
    fi
}

umount_one_dir() {
    dst=$1
    if is_mounted "$dst"; then
        if umount "$dst" 2>/dev/null; then
            note "unmounted $dst"
        else
            note "WARNING: failed to unmount $dst (busy?)"
        fi
    fi
}

root_path() {
    name=$1
    case "$name" in
        */*|.|..) die "invalid root name: $name" ;;
    esac
    path="$ROOTS_DIR/$name"
    [ -d "$path" ] || die "no such root under $ROOTS_DIR: $name (see: ls $ROOTS_DIR)"
    echo "$path"
}

list_roots() {
    for entry in "$ROOTS_DIR"/*; do
        [ -d "$entry" ] || continue
        basename "$entry"
    done
}

setup_mounts() {
    root=$1
    log "Setting up $root"
    for d in $BIND_DIRS; do
        mount_one_dir "/$d" "$root/$d"
    done
}

teardown_mounts() {
    root=$1
    log "Tearing down $root"
    # Reverse order: dev/pts before dev, etc.
    for d in AOK/tools run dev/pts dev sys proc; do
        umount_one_dir "$root/$d"
    done
}

usage() {
    cat <<'EOF' >&2
Usage:
  mount-root.sh <name>              set up mounts, then chroot in (shell)
  mount-root.sh <name> -- CMD...    set up mounts, then chroot in and run CMD
  mount-root.sh all                 set up mounts for every /AOK/roots/* root
  mount-root.sh all -- CMD...       set up mounts, then run CMD in each root in turn
  mount-root.sh --unmount <name>    tear the bind mounts back down
  mount-root.sh --unmount all       tear down every /AOK/roots/* root

Examples:
  mount-root.sh Devuan6-x86_64
  mount-root.sh Devuan6-x86_64 -- top -bn1
  mount-root.sh all -- uname -a
  mount-root.sh --unmount all
EOF
    exit "${1:-1}"
}

[ $# -ge 1 ] || usage

UNMOUNT=0
if [ "$1" = "--unmount" ] || [ "$1" = "-u" ]; then
    UNMOUNT=1
    shift
fi
[ $# -ge 1 ] || usage

TARGET=$1
shift

# Keep the command as "$@", NOT flattened into a string. It used to be `CMD=$*`
# and then an UNQUOTED $CMD at the chroot call, which re-split on whitespace and
# threw the caller's quoting away: `mount-root.sh R -- sh -c "apt-get update"`
# ran `sh -c apt-get`, so apt got NO arguments and printed its own help. Silent,
# and it reads as the command misbehaving rather than as this script dropping
# quotes. "$@" survives the `for name` loop below untouched, so nothing needs
# saving or re-parsing.
HAVE_CMD=0
if [ $# -gt 0 ]; then
    [ "$1" = "--" ] || die "expected -- before a command (got: $1)"
    shift
    HAVE_CMD=1
fi

if [ "$TARGET" = all ]; then
    NAMES=$(list_roots)
    [ -n "$NAMES" ] || die "no roots found under $ROOTS_DIR (is anything else installed besides the booted root?)"
else
    NAMES=$TARGET
    root_path "$TARGET" >/dev/null
fi

if [ "$UNMOUNT" = 1 ]; then
    for name in $NAMES; do
        teardown_mounts "$(root_path "$name")"
    done
    exit 0
fi

for name in $NAMES; do
    setup_mounts "$(root_path "$name")"
done

if [ "$TARGET" = all ]; then
    if [ "$HAVE_CMD" = 1 ]; then
        for name in $NAMES; do
            root=$(root_path "$name")
            log "$name: $*"
            "$CHROOT" "$root" "$@"
        done
    else
        log "Mounts are ready for every root under $ROOTS_DIR:"
        for name in $NAMES; do
            note "$name  ->  chroot $ROOTS_DIR/$name /bin/sh"
        done
        note "(or re-run: mount-root.sh <name> [-- CMD...])"
    fi
    exit 0
fi

root=$(root_path "$TARGET")
if [ "$HAVE_CMD" = 1 ]; then
    log "$TARGET: $*"
    exec "$CHROOT" "$root" "$@"
else
    SHELL_BIN=/bin/sh
    [ -x "$root/bin/bash" ] && SHELL_BIN=/bin/bash
    log "Entering $TARGET ($SHELL_BIN) -- exit to return"
    exec "$CHROOT" "$root" "$SHELL_BIN" -l
fi
