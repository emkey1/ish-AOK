#!/bin/sh
# Host-side A/B for the fakefs escape migration on a CASE-SENSITIVE host
# (fs/fake-migrate.c). The companion rig, fakefs_migrate_repair.sh, covers the
# folding host; this one covers the damage only a case-sensitive host can take,
# so between them they cover both kinds of host iSH-AOK stores a root on.
#
# v4/v5 resolve a DB path's current host entry by scanning its directory for a
# case-insensitive match and taking the first one readdir offers. A folding host
# has ONE entry per twin pair, so that is the only rule that can work there.
# iOS is case-sensitive: "A" and "a" are both really present, and whenever the
# lowercase twin came back first the pass decided a twin owned the file, left
# the real "A" alone and created an EMPTY "%a" beside it.
#
# The directory then holds two host entries decoding to one guest name, so
# readdir emits it twice -- which no real filesystem does -- and every lookup
# escapes to "%a", the empty one. Measured on an aarch64 Devuan root:
# /usr/share/terminfo listed "A" and "N" twice with all their entries
# unreachable, so TERM=Apple_Terminal could not be found.
#
#   tests/manual/fakefs_migrate_alias.sh <rootfs.tar[.gz|.xz]> [subtree] [builddir]
#
# Two arms, because they fail for different reasons and are fixed by different
# code:
#   prevention -- a pre-v4 root replayed through the whole migration. Catches
#                 dir_cache_find preferring readdir order over an exact match.
#                 Needs a twin pair to come back lowercase-first at least once,
#                 so it wants several pairs, and it is the only probabilistic
#                 part of this rig (with 8 pairs, missing it is 1 chance in 256).
#   repair     -- the wreckage manufactured directly and the root declared to be
#                 at user_version 7, which is where every already-damaged root
#                 in the field sits. Deterministic, and the arm that exercises
#                 the version 8 pass.
set -e

TARBALL=${1:?usage: fakefs_migrate_alias.sh <rootfs.tar.gz> [subtree] [builddir]}
SUBTREE=${2:-usr/share/terminfo}
BUILD=${3:-build}
ISH=$BUILD/ish
FAKEFSIFY=$BUILD/tools/fakefsify
# Honours TMPDIR explicitly: on macOS the root has to be put on a case-sensitive
# volume for this to test anything, and that is how you point it at one.
WORK=$(mktemp -d "${TMPDIR:-/tmp}/fakefs_migrate_alias.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

for f in "$ISH" "$FAKEFSIFY"; do
    [ -x "$f" ] || { echo "missing $f (build it first)"; exit 1; }
done

mkdir -p "$WORK/probe" && : > "$WORK/probe/aa"
if [ -e "$WORK/probe/AA" ]; then
    echo "SKIP: $WORK is case-insensitive; this bug needs a case-sensitive host"
    echo "  (make one with: hdiutil create -size 2g -fs 'Case-sensitive APFS' \\"
    echo "                    -volname ishcs -type SPARSE ishcs && hdiutil attach ishcs.sparseimage,"
    echo "   then run with TMPDIR=/Volumes/ishcs)"
    exit 0
fi

echo "importing $TARBALL"
mkdir -p "$WORK/stage"
tar xf "$TARBALL" -C "$WORK/stage"

# Use the tarball's own case twins when it has them; ncurses ships 8 pairs under
# usr/share/terminfo. A minirootfs has no terminfo at all, so plant the same
# shape rather than passing vacuously.
twins=$(ls "$WORK/stage/$SUBTREE" 2>/dev/null | tr 'A-Z' 'a-z' | sort | uniq -d | wc -l | tr -d ' ')
if [ "$twins" -lt 2 ]; then
    echo "  $SUBTREE has no case twins; planting a terminfo-shaped subtree"
    for L in A B C D E F G H I J K L M N O P Q R S T; do
        l=$(echo "$L" | tr 'A-Z' 'a-z')
        mkdir -p "$WORK/stage/$SUBTREE/$L" "$WORK/stage/$SUBTREE/$l"
        echo "upper-$L" > "$WORK/stage/$SUBTREE/$L/${L}term"
        echo "deep-$L"  > "$WORK/stage/$SUBTREE/$L/${L}sub_dir_marker"
        mkdir -p "$WORK/stage/$SUBTREE/$L/${L}sub"
        echo "nested-$L" > "$WORK/stage/$SUBTREE/$L/${L}sub/${L}nested"
        echo "lower-$l" > "$WORK/stage/$SUBTREE/$l/${l}nsi"
    done
fi
tar czf "$WORK/fixture.tar.gz" -C "$WORK/stage" .

# Decode a host on-disk name back to the guest name, the shell-side twin of
# fake_path_from_host() in fs/fake-path.h.
cat > "$WORK/names.py" <<'PY'
import os, sys
def decode(name):
    out, i = [], 0
    while i < len(name):
        c = name[i]
        if c == '%' and i + 1 < len(name):
            n = name[i + 1]
            if 'a' <= n <= 'z':
                out.append(n.upper()); i += 2; continue
            if n == '%':
                out.append('%'); i += 2; continue
            if '0' <= n <= '7' and i + 2 < len(name):
                h = name[i + 2]
                if h in '0123456789abcdef':
                    out.append(chr(0x80 | ((ord(n) - 48) << 4) | int(h, 16))); i += 3; continue
        out.append(c); i += 1
    return ''.join(out)
def encode(name):
    out = []
    for ch in name:
        b = ord(ch)
        if 'A' <= ch <= 'Z':
            out.append('%' + ch.lower())
        elif ch == '%':
            out.append('%%')
        elif b >= 0x80:
            out.append('%%%d%x' % ((b & 0x7f) >> 4, b & 0xf))
        else:
            out.append(ch)
    return ''.join(out)
PY

fixture() {
    rm -rf "$WORK/root"
    "$FAKEFSIFY" "$WORK/fixture.tar.gz" "$WORK/root" >/dev/null
    # First boot absorbs the db_inode rebuild a freshly imported root triggers.
    # It has to happen while the root is still healthy: fakefs_rebuild drops any
    # path whose host entry is missing, which on a damaged root is exactly the
    # rows the repair would have used.
    "$ISH" -f "$WORK/root" /bin/true 2>/dev/null || true
}

# Everything the guest can read under the subtree, so a repair that quietly
# drops a file is caught as surely as one that leaves a duplicate.
inventory() {
    "$ISH" -f "$WORK/root" /bin/sh -c \
        "find /$SUBTREE -type f -exec cat {} + 2>/dev/null | sort" 2>/dev/null
}

dups() {
    "$ISH" -f "$WORK/root" /bin/sh -c \
        "find /$SUBTREE -type d 2>/dev/null | while read -r d; do ls -a \"\$d\" | sort | uniq -d | sed \"s|^|\$d/|\"; done" 2>/dev/null
}

fail=0
check() {
    want=$1; got=$2; what=$3
    if [ "$want" = "$got" ]; then
        echo "  ok: $what"
    else
        echo "  FAIL: $what (want [$want], got [$got])"
        fail=1
    fi
}

# The inventory is compared as a file rather than through process substitution,
# which /bin/sh is not obliged to have.
compare_inventory() {
    inventory > "$WORK/after.txt"
    check "$BEFORE_N" "$(grep -c . < "$WORK/after.txt" || true)" "every file readable"
    check "" "$(diff "$WORK/before.txt" "$WORK/after.txt" | head -5 | tr '\n' ' ')" "contents unchanged"
}

fixture
inventory > "$WORK/before.txt"
BEFORE_N=$(grep -c . < "$WORK/before.txt" || true)
echo "  $BEFORE_N files readable under /$SUBTREE"
[ "$BEFORE_N" -gt 0 ] || { echo "FAIL: nothing under /$SUBTREE to test with"; exit 1; }

# ---------------------------------------------------------------- prevention
echo "prevention: replaying a pre-v4 root through the migration"
python3 - "$WORK/root/data/$SUBTREE" "$WORK/names.py" <<'PY'
import os, sys
exec(open(sys.argv[2]).read())
# Rewrite the subtree into the pre-v4 on-disk format: raw guest names. On a
# case-sensitive host the twins do not merge, so both spellings survive -- which
# is the state the device roots were imported in.
def downgrade(d):
    for n in list(os.listdir(d)):
        p, dn = os.path.join(d, n), decode(n)
        if dn != n:
            os.rename(p, os.path.join(d, dn))
            p = os.path.join(d, dn)
        if os.path.isdir(p):
            downgrade(p)
downgrade(sys.argv[1])
PY
sqlite3 "$WORK/root/meta.db" "pragma user_version = 3"
"$ISH" -f "$WORK/root" /bin/true 2>/dev/null || true
check "" "$(dups | tr '\n' ' ' | sed 's/ *$//')" "no duplicate readdir names"
compare_inventory

# -------------------------------------------------------------------- repair
echo "repair: the wreckage of an old build, replayed from user_version 7"
fixture
python3 - "$WORK/root/data/$SUBTREE" "$WORK/names.py" "$WORK/root/meta.db" "/$SUBTREE" > "$WORK/damage.txt" <<'PY'
import os, sqlite3, sys
exec(open(sys.argv[2]).read())
root, db, guest_root = sys.argv[1], sqlite3.connect(sys.argv[3]), sys.argv[4]

# Exactly what migrate_twin_fallback left behind for an uppercase directory
# whose lowercase twin readdir happened to return first: the real entry keeps
# its pre-escaping name and an EMPTY directory takes the escaped one. Everything
# inside stays in the format that predates the escaping, because the v4 walk
# does parents first and skipped the whole subtree once it made that call.
def unescape_tree(d):
    for n in list(os.listdir(d)):
        p, dn = os.path.join(d, n), decode(n)
        if dn != n:
            os.rename(p, os.path.join(d, dn))
            p = os.path.join(d, dn)
        if os.path.isdir(p):
            unescape_tree(p)

damaged = 0
for host in sorted(os.listdir(root)):
    guest = decode(host)
    if not guest[:1].isupper() or not os.path.isdir(os.path.join(root, host)):
        continue
    os.rename(os.path.join(root, host), os.path.join(root, guest))
    unescape_tree(os.path.join(root, guest))
    os.mkdir(os.path.join(root, host))
    # ...and then v7 deleted the rows of everything inside: their host entry is
    # missing and their parent -- that same empty directory -- opens fine, which
    # is its stale-row condition exactly. So the files have no metadata either,
    # and fakefs_readdir hides any entry that has none.
    #
    # Matched in Python, on the raw bytes. paths.path is a blob, and SQL LIKE is
    # ASCII case-INSENSITIVE by default -- "/usr/share/terminfo/A/%" also matches
    # every row under the lowercase twin, which is the half of the pair this
    # damage is supposed to leave alone.
    prefix = (guest_root + "/" + guest + "/").encode()
    doomed = [row[0] for row in db.execute("select path from paths")
              if bytes(row[0]).startswith(prefix)]
    for p in doomed:
        db.execute("delete from paths where path = ?", (p,))
    damaged += 1
db.commit()
db.execute("pragma user_version = 7")
db.commit()
db.close()
print("%d" % damaged)
PY
DAMAGED=$(cat "$WORK/damage.txt")
echo "  manufactured $DAMAGED aliased directories"
# Checked on the host: any ish invocation runs the migration, so there is no way
# to look at the damaged root from the guest without repairing it first. Without
# this the arm below could pass by testing an undamaged root.
[ "$DAMAGED" -gt 0 ] || { echo "  FAIL: nothing was damaged, so the repair arm proves nothing"; exit 1; }

"$ISH" -f "$WORK/root" /bin/true 2>/dev/null || true
check "" "$(dups | tr '\n' ' ' | sed 's/ *$//')" "no duplicate readdir names"
compare_inventory

if [ "$fail" -eq 0 ]; then
    echo "fakefs_migrate_alias: PASS"
else
    echo "fakefs_migrate_alias: FAIL"
    exit 1
fi
