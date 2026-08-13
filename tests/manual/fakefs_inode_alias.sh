#!/bin/sh
# Host-side rig for the fakefs inode-alias wedge (fs/fake-db.c, fs/fake.c,
# fs/fake-migrate.c).
#
# fakefs numbers inodes from a per-mount in-memory counter seeded once from
# max(stats.inode), so a second allocator on the same meta.db -- another ish
# process on the same root, or that root mounted twice -- hands out numbers the
# first one is still using. path_create's `insert into stats` then hit a
# primary-key constraint, which db_check_error only printk()s, and the
# `insert or replace into paths` below it ran anyway: the new path was left
# pointing at an inode that already belonged to another file, sharing its stat
# blob. When the two disagree about being a directory the entry becomes
# unremovable -- rmdir(2) gets ENOTDIR from the host, unlink(2) EISDIR from the
# metadata -- which is how a real root ended up with a gcc temporary,
# /tmp/ccKNBeAg.o, wearing the mode of the directory /rbind and aborting every
# in-guest build that reused the name.
#
# Reproducing that by racing two allocators is a coin flip, so this rig makes
# the end state directly: create a directory and a file, then point the file's
# paths row at the directory's inode, which is byte for byte what the lost race
# produced. Then check both cures:
#
#   1. rmdir() clears it (fs/fake.c), for a root that meets one at runtime.
#   2. The v7 migration repairs it at mount (fs/fake-migrate.c), for a root
#      already carrying the damage from an older build.
#
#   tests/manual/fakefs_inode_alias.sh [root] [builddir]
#
# Runs against a real fakefs root (default build/alpinex86); it creates and
# removes its own /tmp entries and restores the root's user_version on the way
# out, so it leaves nothing behind.
set -e

ROOT=${1:-build/alpinex86}
BUILD=${2:-build}
ISH=$BUILD/ish
DB=$ROOT/meta.db

[ -x "$ISH" ] || { echo "missing $ISH (build it first)"; exit 1; }
[ -f "$DB" ] || { echo "missing $DB (not a fakefs root?)"; exit 1; }
command -v sqlite3 >/dev/null 2>&1 || { echo "SKIP: no sqlite3 on the host"; exit 0; }

# paths.path is a BLOB column, so a bare `= 'text'` comparison never matches
# anything. Every lookup here has to cast.
q() { sqlite3 "$DB" "$1"; }

saved_version=$(q "pragma user_version;")
probe=/tmp/inode_alias_probe.$$
failures=0

# The version a fully migrated root lands on, read rather than written down.
# "already migrated, leave it alone" is a moving number: hard-coding it meant
# that adding version 8 turned every one of those into "run the newest pass on
# the next mount" and made the check below insist on a version the migration no
# longer stops at.
"$ISH" -f "$ROOT" /bin/true >/dev/null 2>&1 || true
CURRENT=$(q "pragma user_version;")

cleanup() {
    "$ISH" -f "$ROOT" /bin/sh -c "rm -rf $probe" >/dev/null 2>&1 || true
    q "pragma user_version = $saved_version;" || true
}
trap cleanup EXIT

note() { echo "  $*"; }
fail() { echo "FAIL: $*"; failures=$((failures + 1)); }

# Build the alias: $probe/twin is a real directory, $probe/victim a real file,
# and the victim's row is bent to the twin's inode so both read one stat blob.
manufacture() {
    "$ISH" -f "$ROOT" /bin/sh -c "rm -rf $probe; mkdir -p $probe && mkdir $probe/twin && : > $probe/victim" >/dev/null 2>&1
    twin_inode=$(q "select inode from paths where cast(path as text) = '$probe/twin';")
    [ -n "$twin_inode" ] || { fail "could not find the twin directory's inode"; return 1; }
    q "update paths set inode = $twin_inode where cast(path as text) = '$probe/victim';"
    victim_inode=$(q "select inode from paths where cast(path as text) = '$probe/victim';")
    [ "$victim_inode" = "$twin_inode" ] || { fail "could not alias the victim onto the twin"; return 1; }

    # The host entry must still be an ordinary file: that disagreement with the
    # metadata, which now says directory, IS the wedge.
    # $probe is lowercase ASCII, so its host name needs no escaping (fs/fake-path.h).
    host_victim="$ROOT/data$probe/victim"
    [ -f "$host_victim" ] || { fail "host entry $host_victim is not a regular file"; return 1; }
    return 0
}

guest_type() {
    "$ISH" -f "$ROOT" /bin/sh -c "stat -c %F '$1' 2>/dev/null" 2>/dev/null | tr -d '\r'
}
guest_inode() {
    "$ISH" -f "$ROOT" /bin/sh -c "stat -c %i '$1' 2>/dev/null" 2>/dev/null | tr -d '\r'
}

echo "== part 1: rmdir clears an alias met at runtime"
q "pragma user_version = $CURRENT;"   # already migrated, so only the runtime path can help
if manufacture; then
    seen=$(guest_type "$probe/victim")
    note "victim reports as: $seen (host entry is a regular file)"
    [ "$seen" = "directory" ] || fail "expected the guest to see a directory, got '$seen'"

    out=$("$ISH" -f "$ROOT" /bin/sh -c "rmdir $probe/victim 2>&1 && echo RMDIR_OK" 2>&1 | tr -d '\r')
    case "$out" in
        *RMDIR_OK*) note "rmdir cleared it" ;;
        *) fail "rmdir could not clear the alias: $out" ;;
    esac

    still=$(guest_type "$probe/victim")
    [ -z "$still" ] || fail "the entry survived rmdir, still reports as '$still'"

    # The twin must be untouched -- it was the legitimate owner of that blob.
    twin_seen=$(guest_type "$probe/twin")
    [ "$twin_seen" = "directory" ] || fail "the twin directory was damaged, reports as '$twin_seen'"

    # And the name has to be usable again; that is what gcc needs.
    reuse=$("$ISH" -f "$ROOT" /bin/sh -c "echo hi > $probe/victim && cat $probe/victim" 2>&1 | tr -d '\r')
    [ "$reuse" = "hi" ] || fail "the name was not reusable after rmdir: $reuse"
fi

echo "== part 2: the v7 migration repairs a root that already carries one"
if manufacture; then
    q "pragma user_version = 6;"   # let the v7 pass run on the next mount
    "$ISH" -f "$ROOT" /bin/true >/dev/null 2>&1 || true
    [ "$(q 'pragma user_version;')" = "$CURRENT" ] || fail "the migration did not reach version $CURRENT"

    seen=$(guest_type "$probe/victim")
    note "victim reports as: $seen"
    [ "$seen" = "regular empty file" ] || fail "expected a regular empty file after repair, got '$seen'"

    v_ino=$(guest_inode "$probe/victim")
    t_ino=$(guest_inode "$probe/twin")
    note "victim inode $v_ino, twin inode $t_ino"
    [ -n "$v_ino" ] && [ "$v_ino" != "$t_ino" ] || fail "the repair left both paths on inode $v_ino"

    twin_seen=$(guest_type "$probe/twin")
    [ "$twin_seen" = "directory" ] || fail "the twin directory was damaged, reports as '$twin_seen'"
    [ "$t_ino" = "$twin_inode" ] || fail "the twin lost its own inode ($twin_inode -> $t_ino)"

    rm_out=$("$ISH" -f "$ROOT" /bin/sh -c "rm -f $probe/victim && echo RM_OK" 2>&1 | tr -d '\r')
    case "$rm_out" in
        *RM_OK*) note "repaired entry removes normally" ;;
        *) fail "could not remove the repaired entry: $rm_out" ;;
    esac
fi

echo "== part 3: the migration also splits two real directories off one inode"
# The collision's other shape. Neither entry is wedged -- each agrees with its
# own host directory -- but (st_dev, st_ino) stops identifying a file, and
# link(2) will not hard-link a directory, so the pair cannot be honest.
"$ISH" -f "$ROOT" /bin/sh -c "rm -rf $probe; mkdir -p $probe/twin $probe/other" >/dev/null 2>&1
twin_inode=$(q "select inode from paths where cast(path as text) = '$probe/twin';")
if [ -n "$twin_inode" ]; then
    q "update paths set inode = $twin_inode where cast(path as text) = '$probe/other';"
    [ "$(guest_inode "$probe/other")" = "$(guest_inode "$probe/twin")" ] ||
        fail "could not put both directories on one inode"

    q "pragma user_version = 6;"
    "$ISH" -f "$ROOT" /bin/true >/dev/null 2>&1 || true

    a=$(guest_inode "$probe/twin")
    b=$(guest_inode "$probe/other")
    note "twin inode $a, other inode $b"
    [ -n "$a" ] && [ -n "$b" ] && [ "$a" != "$b" ] || fail "both directories still share inode $a"
    [ "$(guest_type "$probe/twin")" = "directory" ] || fail "the twin stopped being a directory"
    [ "$(guest_type "$probe/other")" = "directory" ] || fail "the other stopped being a directory"
    # Both must still work as directories.
    ok=$("$ISH" -f "$ROOT" /bin/sh -c ": > $probe/twin/a && : > $probe/other/b && echo BOTH_OK" 2>&1 | tr -d '\r')
    case "$ok" in
        *BOTH_OK*) note "both directories still usable" ;;
        *) fail "a split directory stopped working: $ok" ;;
    esac
fi

echo "== part 4: two allocators on one root must not hand out the same inode"
# The mechanism itself, rather than its end state. Two ish processes on one
# meta.db each seed next_inode from the same max(stats.inode) and then allocate
# from their own in-memory copy, so before path_create learned to notice the
# primary-key constraint they marched through the same numbers together -- the
# damaged root that started all this had contiguous runs of unrelated
# temporaries paired off exactly this way.
#
# Both sides make DIRECTORIES on purpose: two regular files sharing an inode
# are indistinguishable from an ordinary hard link, while two directories
# cannot be hard-linked at all, so any sharing here is unambiguous.
q "pragma user_version = $CURRENT;"
"$ISH" -f "$ROOT" /bin/sh -c "rm -rf $probe; mkdir -p $probe/a $probe/b" >/dev/null 2>&1
racer() {
    "$ISH" -f "$ROOT" /bin/sh -c \
        "i=0; while [ \$i -lt 120 ]; do mkdir $probe/$1/d\$i 2>/dev/null; i=\$((i+1)); done" >/dev/null 2>&1
}
racer a & p1=$!
racer b & p2=$!
wait "$p1" || true
wait "$p2" || true

made=$(q "select count(*) from paths where cast(path as text) like '$probe/%';")
note "created $made entries across two concurrent ish processes"
if [ "$made" -lt 100 ]; then
    note "SKIP: the two processes did not get far enough to race (only $made entries)"
else
    dupes=$(q "select count(*) from (select inode from paths
                where cast(path as text) like '$probe/%' group by inode having count(*) > 1);")
    [ "$dupes" = "0" ] || fail "$dupes inodes were handed out twice by concurrent allocators"

    # And nothing may collide with the rest of the root either.
    outside=$(q "select count(*) from paths p where cast(p.path as text) like '$probe/%'
                and exists (select 1 from paths o where o.inode = p.inode
                            and cast(o.path as text) not like '$probe/%');")
    [ "$outside" = "0" ] || fail "$outside new entries landed on an inode already in use elsewhere"
fi

if [ "$failures" -ne 0 ]; then
    echo "fakefs_inode_alias.sh: FAIL failures=$failures"
    exit 1
fi
echo "fakefs_inode_alias.sh: PASS"
