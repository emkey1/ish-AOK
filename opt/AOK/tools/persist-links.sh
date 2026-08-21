#!/bin/sh
# Link what is in /AOK/persist/bin into a directory the distro already has on
# PATH -- /usr/local/bin by default.
#
# Why this is needed at all, given /AOK/persist/bin IS first on the default
# PATH: that default is the one iSH-AOK hands to sessions it starts itself.
# A login that gets its environment from somewhere else -- sshd, `su -`, cron,
# a service started by init -- reads PATH from /etc/profile or a compiled-in
# default, and will not have it. Linking into /usr/local/bin makes the same
# programs reachable from every one of those.
#
# Use a SYMlink, never a hard link: /AOK/persist is its own filesystem, so `ln`
# across it fails with EXDEV.
#
# The links point at /AOK/persist/bin, which survives root switches, app
# updates and reinstalls -- but /usr/local/bin belongs to the ROOT you are in.
# Install a fresh root and the links are gone while their targets are not, so
# re-running this is the expected thing to do, not a sign something broke.
#
# Usage:
#   sh /AOK/tools/persist-links.sh [options] [directory]
#
#   --list     show what would happen, change nothing
#   --remove   remove links pointing into /AOK/persist/bin
#   --force    replace files that are not our own symlinks
#   --help
#
# --list is a dry run rather than a mode of its own, so it composes with the
# others: `--list --remove` prints what a removal would unlink and unlinks
# nothing. Holding both in one MODE variable is what made that combination
# delete instead of preview -- last flag won, and the preview arm inside the
# removal loop could never be reached.
set -eu

SOURCE=/AOK/persist/bin
TARGET_DIR=/usr/local/bin
MODE=link          # link | remove -- what this run is for
DRY_RUN=0          # --list: print, change nothing, in either mode
FORCE=0

usage() {
    echo "Link /AOK/persist/bin into a directory already on PATH."
    echo
    echo "Usage: sh /AOK/tools/persist-links.sh [options] [directory]"
    echo "       (defaults to /usr/local/bin)"
    echo
    echo "  --list     show what would happen, change nothing"
    echo "  --remove   remove links pointing into $SOURCE"
    echo "  --force    replace files that are not our own symlinks"
    echo "  --help"
    exit "${1:-0}"
}

while [ $# -gt 0 ]; do
    case "$1" in
        --list) DRY_RUN=1 ;;
        --remove) MODE=remove ;;
        --force) FORCE=1 ;;
        -h|--help) usage 0 ;;
        -*) echo "$0: unknown option $1" >&2; usage 1 ;;
        *) TARGET_DIR="$1" ;;
    esac
    shift
done

if [ ! -d "$SOURCE" ]; then
    echo "$0: $SOURCE does not exist -- this iSH-AOK has no /AOK/persist mounted" >&2
    exit 1
fi

# Ownership test, used by both --remove and the replace check: a symlink that
# resolves to something inside $SOURCE is ours to manage, anything else is not.
# `readlink` is avoided deliberately -- it may itself be one of the links this
# script installs -- so this uses -ef and the shell alone, the same reasoning
# as native-links.sh.
link_is_ours() {
    [ -L "$1" ] || return 1
    for _s in "$SOURCE"/*; do
        [ -e "$_s" ] || continue
        [ "$1" -ef "$_s" ] && return 0
    done
    return 1
}

if [ "$MODE" = remove ]; then
    removed=0
    for f in "$TARGET_DIR"/*; do
        link_is_ours "$f" || continue
        if [ "$DRY_RUN" -eq 1 ]; then
            echo "  would unlink $f"
        else
            rm -f "$f"
        fi
        removed=$((removed + 1))
    done
    if [ "$DRY_RUN" -eq 1 ]; then
        echo "would remove $removed link(s) from $TARGET_DIR"
    else
        echo "removed $removed link(s) from $TARGET_DIR"
    fi
    exit 0
fi

[ "$DRY_RUN" -eq 1 ] || mkdir -p "$TARGET_DIR"

linked=0; skipped=0; blocked=0; empty=1
for src in "$SOURCE"/*; do
    # An unmatched glob leaves the pattern itself; -e sorts that out and also
    # skips a dangling link.
    [ -e "$src" ] || continue
    empty=0
    # Directories are not commands. /AOK/persist/lib and etc are siblings of
    # this directory rather than inside it, but somebody may still have made
    # one here.
    [ -d "$src" ] && continue
    if [ ! -x "$src" ]; then
        [ "$DRY_RUN" -eq 1 ] && echo "  skipping $src (not executable)"
        skipped=$((skipped + 1))
        continue
    fi

    name=${src##*/}
    dest="$TARGET_DIR/$name"

    if [ -L "$dest" ] && [ "$dest" -ef "$src" ]; then
        skipped=$((skipped + 1))   # already ours and already right
        continue
    fi
    if [ -e "$dest" ] || [ -L "$dest" ]; then
        # Ours but pointing elsewhere in $SOURCE: correct it without --force.
        # Somebody else's file: leave it, because shadowing a distro command
        # from a directory this script does not own is exactly the failure
        # native-links.sh had to be walked back from.
        if ! link_is_ours "$dest" && [ "$FORCE" -eq 0 ]; then
            [ "$DRY_RUN" -eq 1 ] && echo "  would NOT replace $dest (exists; --force to override)"
            blocked=$((blocked + 1))
            continue
        fi
    fi

    if [ "$DRY_RUN" -eq 1 ]; then
        echo "  would link $dest -> $src"
    else
        ln -sf "$src" "$dest"
    fi
    linked=$((linked + 1))
done

if [ "$empty" -eq 1 ]; then
    echo "$SOURCE is empty -- nothing to link"
    exit 0
fi

if [ "$DRY_RUN" -eq 1 ]; then
    echo "would link $linked, leave $blocked in place, skip $skipped"
else
    echo "linked $linked into $TARGET_DIR ($skipped already or skipped, $blocked left in place)"
    [ "$blocked" -gt 0 ] && echo "  $blocked existing command(s) left alone; --force to replace, --list to see them"
fi
exit 0
