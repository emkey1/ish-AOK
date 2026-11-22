#!/bin/sh
# ssh-stash.sh -- carry a user's ~/.ssh from one AOK-iSH root to another.
#
# Each root has its own /etc/passwd, and the same person is a different uid in
# each (the PSCAL image's first login is 501, a native root's 1000, a distro's
# often 1000 or 1001), so the stash is keyed by LOGIN NAME. Ownership is put
# right from the restoring root's own /etc/passwd.
#
#   sudo sh /AOK/tools/ssh-stash.sh save    [LOGIN]   ~LOGIN/.ssh -> the stash
#   sudo sh /AOK/tools/ssh-stash.sh restore [LOGIN]   the stash -> ~LOGIN/.ssh
#   sudo sh /AOK/tools/ssh-stash.sh list
#
# LOGIN defaults to whoever ran sudo (SUDO_USER), then to the caller.
#
# Where: /AOK/fakefs/ssh/<login>, root-only (0700). /AOK/fakefs survives root
# switches, app updates and reinstalls like /AOK/persist does, but keeps real
# owners and modes -- /AOK/persist shows every file as the viewer's own, so
# every user of every root could read a private key kept there. That is also
# why this needs root: the stash is root's, whichever uid you are here.
#
# Merging, both ways: authorized_keys and known_hosts are combined line by line
# (nothing is dropped, nothing doubled). Any other file -- keys, config,
# config.d/* -- is copied where the other side has none; where both have one
# and they differ, `save` takes this root's (it is the newer word on what you
# use) and `restore` keeps this root's unless you add --force.
#
# Run as root from any of AOK-iSH's shells: native dash, the PSCAL image's sh,
# a distro's sh. Only POSIX sh and the commands every one of those has.
set -eu

STASH_ROOT=${AOK_SSH_STASH_DIR:-/AOK/fakefs/ssh}

usage() {
    sed -n '2,/^$/s/^# \{0,1\}//p' "$0" >&2
    exit 2
}

die() { echo "ssh-stash: $*" >&2; exit 1; }
say() { echo "  $*"; }

[ $# -ge 1 ] || usage
action=$1; shift
force=0
login=
for arg in "$@"; do
    case "$arg" in
        --force) force=1 ;;
        -h|--help) usage ;;
        -*) die "unknown option $arg" ;;
        *) [ -z "$login" ] || die "one login at a time"; login=$arg ;;
    esac
done

[ "$(id -u)" = 0 ] || die "needs root (the stash is root's): sudo sh $0 $action${login:+ $login}"

# A passwd field for LOGIN: 3 uid, 4 gid, 6 home. By name, never by uid.
passwd_field() {
    awk -F: -v u="$1" -v f="$2" '$1 == u { print $f; exit }' /etc/passwd
}

if [ "$action" = list ]; then
    [ -d "$STASH_ROOT" ] || { echo "nothing stashed ($STASH_ROOT does not exist)"; exit 0; }
    found=0
    for d in "$STASH_ROOT"/*; do
        [ -d "$d" ] || continue
        found=1
        name=${d##*/}
        files=$(cd "$d" && find . -type f | sed 's|^\./||' | sort | tr '\n' ' ')
        echo "$name: $files"
    done
    [ "$found" = 1 ] || echo "nothing stashed in $STASH_ROOT"
    exit 0
fi

case "$action" in save|restore) ;; *) usage ;; esac

if [ -z "$login" ]; then
    login=${SUDO_USER:-}
    [ -n "$login" ] || login=$(id -un)
fi
case "$login" in
    ''|*/*|.*) die "not a login name: '$login'" ;;
esac

uid=$(passwd_field "$login" 3)
gid=$(passwd_field "$login" 4)
home=$(passwd_field "$login" 6)
[ -n "$uid" ] || die "no login '$login' in this root's /etc/passwd"
[ -n "$home" ] || die "'$login' has no home directory in /etc/passwd"
dot="$home/.ssh"
stash="$STASH_ROOT/$login"

# Lines of FROM that TO lacks, appended to TO: authorized_keys, known_hosts.
merge_lines() {
    from=$1 to=$2
    added=0
    [ -f "$to" ] || : > "$to"
    while IFS= read -r line || [ -n "$line" ]; do
        [ -n "$line" ] || continue
        if ! grep -q -F -x -e "$line" "$to"; then
            printf '%s\n' "$line" >> "$to"
            added=$((added + 1))
        fi
    done < "$from"
    echo "$added"
}

# Copy one tree's files into another by these rules. PREFER is "from" when
# the source wins a conflict (save, restore --force), "to" when the
# destination keeps its own (restore).
copy_tree() {
    src=$1 dst=$2 prefer=$3
    copied=0 merged=0 kept=0
    list=$(cd "$src" && find . -type f | sed 's|^\./||')
    [ -n "$list" ] || { echo "0 0 0"; return 0; }
    # Paths with a newline in them are not something ~/.ssh holds; the loop
    # splits on lines.
    echo "$list" | {
        while IFS= read -r rel; do
            s="$src/$rel" d="$dst/$rel"
            case "$rel" in */*) mkdir -p "$dst/${rel%/*}" ;; esac
            case "${rel##*/}" in
                authorized_keys|authorized_keys2|known_hosts)
                    n=$(merge_lines "$s" "$d")
                    [ "$n" = 0 ] || merged=$((merged + n))
                    ;;
                *)
                    if [ ! -e "$d" ]; then
                        cat "$s" > "$d"; copied=$((copied + 1))
                    elif cmp -s "$s" "$d"; then
                        :
                    elif [ "$prefer" = from ]; then
                        cat "$s" > "$d"; copied=$((copied + 1))
                    else
                        say "kept this root's $rel (the stash's differs; --force takes the stash's)" >&2
                        kept=$((kept + 1))
                    fi
                    ;;
            esac
        done
        echo "$copied $merged $kept"
    } | tail -n 1
}

# 0700 directories, 0644 public keys, 0600 for everything else -- what ssh and
# sshd's StrictModes want, whatever the source's modes were.
fix_modes() {
    dir=$1
    find "$dir" -type d -exec chmod 700 {} \;
    find "$dir" -type f -exec chmod 600 {} \;
    find "$dir" -type f -name '*.pub' -exec chmod 644 {} \;
}

if [ "$action" = save ]; then
    [ -d "$dot" ] || die "$login has no $dot to save"
    mkdir -p "$STASH_ROOT"
    chmod 700 "$STASH_ROOT"
    chown 0:0 "$STASH_ROOT" 2>/dev/null || true
    mkdir -p "$stash"
    set -- $(copy_tree "$dot" "$stash" from)
    fix_modes "$stash"
    chown -R 0:0 "$stash" 2>/dev/null || true
    echo "saved $login's $dot to $stash: $1 file(s) copied, $2 line(s) merged"
    exit 0
fi

# restore
[ -d "$stash" ] || die "nothing stashed for $login (sudo sh $0 list)"
[ -d "$home" ] || die "$login's home $home does not exist"
mkdir -p "$dot"
if [ "$force" = 1 ]; then prefer=from; else prefer=to; fi
set -- $(copy_tree "$stash" "$dot" "$prefer")
fix_modes "$dot"
chown -R "$uid:$gid" "$dot"
echo "restored $login's ~/.ssh from $stash: $1 file(s) copied, $2 line(s) merged, $3 kept"
