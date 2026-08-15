#!/bin/sh
# Link SmallCLUE's applets into a bin directory so they run natively.
#
# /AOK/native/smallclue is compiled into iSH-AOK and runs as host code rather
# than translated guest instructions, so it costs the same on every guest
# architecture. Like any multicall binary it picks its applet from argv[0], so
# a symlink named `wc` runs the wc applet.
#
# Use a SYMlink, never a hard link: /AOK is its own filesystem, so `ln` across
# it fails with EXDEV.
#
# Default target is /usr/local/native-bin, which is deliberately NOT on PATH.
#
# Linking into /usr/local/bin was the obvious design and it is wrong: that
# directory precedes /usr/bin, so every link shadows the distro's command, and
# SmallCLUE's applets are SMALLER implementations rather than drop-in
# replacements. Excluding the applets that cannot work at all is not enough,
# because the incompatibilities are per-flag: with the links installed,
# PSCAL's harness died on `grep -q`, which SmallCLUE's grep does not support.
# No audit of the sources finds that -- grep is present and works, just not
# with that flag.
#
# So the default is a directory you opt into:
#
#   PATH=/usr/local/native-bin:$PATH        # this shell only
#   /usr/local/native-bin/wc -l file        # one command
#
# Pass /usr/local/bin explicitly if you want the shadowing, and expect scripts
# that rely on GNU extensions to break.
#
# Usage:
#   sh /AOK/tools/native-links.sh [options] [directory]
#
#   --list     show what would happen, change nothing
#   --remove   remove links pointing at /AOK/native/smallclue
#   --all      include applets that do not work in this build (see EXCLUDED)
#   --force    replace files that are not our own symlinks
#   --help
set -eu

NATIVE=/AOK/native/smallclue
TARGET_DIR=/usr/local/native-bin
MODE=link
INCLUDE_ALL=0
FORCE=0

# Applets deliberately NOT linked by default. Linking a broken one is worse
# than leaving it alone: the distro's working command gets shadowed by one that
# errors, and the failure surfaces somewhere unrelated. This list is derived,
# not guessed -- see tools/native-applet-audit.py, which walks SmallCLUE's
# sources and flags every applet whose handler can reach a call the shim
# answers with ENOSYS, following helper functions transitively.
#
# It was written by hand first and that was not good enough: `env` was missed,
# and since the PSCAL test harness runs `env ... bash ...`, installing these
# links took its suite from 217 passing to zero. env has to exec.
#
# Three groups:
#   missing dependency  -- report "not built into this iSH-AOK": tar/gzip/zcat
#                          (zlib), the checksum trio (OpenSSL), the ssh family
#                          and rsync (vendored OpenSSH), git (libgit2)
#   unimplemented call  -- reach exec/spawn/wait/ioctl/select/glob and fail
#                          with "Function not implemented", or silently do
#                          nothing, which is worse (xargs)
#   not worth shadowing -- system state and non-commands
#
# The audit is deliberately conservative: an applet is excluded if ANY path
# through it reaches an unimplemented call, even a rare one. `find` works fine
# until -exec; a command that fails only on some flags is a nastier trap than
# one that is simply absent. Use --all to link everything anyway.
EXCLUDED="awk chroot env find git gunzip gzip halt init kill less licenses md
md5sum mdev micro mknod more mount nextvi nohup passwd poweroff reboot resize
rm rsync runit scp script sftp sha1sum sha256sum smallclue smallclue-help ssh
ssh-copy-id ssh-keygen stty su sudo tar telnet time timeout top touch umount
version vi vproc-test watch xargs zcat"

# Inlined rather than read out of the file header with sed: `sed` is itself an
# applet this script links, so --help would break after installation. Same
# reason the applet list is parsed with builtins.
usage() {
    echo "Link SmallCLUE's applets into a bin directory so they run natively."
    echo
    echo "Usage: sh /AOK/tools/native-links.sh [options] [directory]"
    echo "       (defaults to /usr/local/native-bin, deliberately not on PATH)"
    echo
    echo "  --list     show what would happen, change nothing"
    echo "  --remove   remove links pointing at /AOK/native/smallclue"
    echo "  --all      include applets that do not work in this build"
    echo "  --force    replace files that are not our own symlinks"
    echo "  --help"
    echo
    echo "Applets with missing dependencies or no working implementation are"
    echo "skipped by default: linking one would shadow a working command with"
    echo "an error. Use --list to see what would change."
    exit "${1:-0}"
}

while [ $# -gt 0 ]; do
    case "$1" in
        --list) MODE=list ;;
        --remove) MODE=remove ;;
        --all) INCLUDE_ALL=1 ;;
        --force) FORCE=1 ;;
        -h|--help) usage 0 ;;
        -*) echo "$0: unknown option $1" >&2; usage 1 ;;
        *) TARGET_DIR="$1" ;;
    esac
    shift
done

if [ ! -x "$NATIVE" ]; then
    echo "$0: $NATIVE not found -- this iSH-AOK has no native SmallCLUE" >&2
    exit 1
fi

is_excluded() {
    [ "$INCLUDE_ALL" -eq 1 ] && return 1
    for e in $EXCLUDED; do
        [ "$1" = "$e" ] && return 0
    done
    return 1
}

# --remove: only ever unlink a symlink that points at the native binary, so a
# real file that happens to share a name is never touched.
if [ "$MODE" = remove ]; then
    removed=0
    for f in "$TARGET_DIR"/*; do
        [ -L "$f" ] || continue
        # `readlink` is itself an applet this script may have linked, so avoid
        # it: -ef compares what the paths resolve to, using the shell alone.
        [ "$f" -ef "$NATIVE" ] || continue
        rm -f "$f"
        removed=$((removed + 1))
    done
    echo "removed $removed link(s) from $TARGET_DIR"
    exit 0
fi

# The applet list comes from the binary, not from a list in here, so it tracks
# whatever this build actually contains.
#
# Parsed with shell builtins only. Using awk here was a bug: once this script
# had installed the links, `awk` resolved to the NATIVE awk, which reads the
# banner differently, and the second run could no longer find any applets. A
# script that installs commands onto PATH must not then depend on that PATH.
APPLETS=$(
    "$NATIVE" 2>&1 | while IFS= read -r line; do
        case "$line" in
            "  "[a-z[]*)
                word=${line#  }       # strip the two-space indent
                word=${word%% *}      # first field only
                [ -n "$word" ] && printf '%s ' "$word"
                ;;
        esac
    done
)
if [ -z "$APPLETS" ]; then
    echo "$0: could not read the applet list from $NATIVE" >&2
    exit 1
fi

[ "$MODE" = list ] || mkdir -p "$TARGET_DIR"

linked=0; skipped=0; excluded=0; blocked=0
for applet in $APPLETS; do
    if is_excluded "$applet"; then
        excluded=$((excluded + 1))
        continue
    fi
    dest="$TARGET_DIR/$applet"

    if [ -L "$dest" ] && [ "$dest" -ef "$NATIVE" ]; then
        skipped=$((skipped + 1))   # already ours; idempotent
        continue
    fi
    if [ -e "$dest" ] || [ -L "$dest" ]; then
        if [ "$FORCE" -eq 0 ]; then
            [ "$MODE" = list ] && echo "  would NOT replace $dest (exists; --force to override)"
            blocked=$((blocked + 1))
            continue
        fi
    fi

    if [ "$MODE" = list ]; then
        echo "  would link $dest -> $NATIVE"
    else
        ln -sf "$NATIVE" "$dest"
    fi
    linked=$((linked + 1))
done

if [ "$MODE" = list ]; then
    echo "would link $linked, leave $blocked in place, skip $excluded excluded, $skipped already linked"
else
    echo "linked $linked into $TARGET_DIR ($skipped already, $blocked left in place, $excluded excluded)"
    [ "$blocked" -gt 0 ] && echo "  $blocked existing command(s) left alone; --force to replace, --list to see them"
fi
exit 0
