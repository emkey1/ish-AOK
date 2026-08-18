#!/bin/sh
# Host-side A/B for the fakefs escape migration (fs/fake-migrate.c).
#
# The v4/v5 passes rename a host entry when its *name* changes and skip any
# guest path that needs no escaping. That strands content: when a case-twin
# ancestor is renamed ("A" -> "%a"), entries a folding host had been reaching
# *through* that directory stay physically inside it while their own escaped
# path ("a/...") stops resolving. v6 puts them back.
#
# This rig manufactures the damage rather than waiting to meet it: it imports a
# rootfs, rewrites one subtree back into the pre-v4 on-disk format (raw guest
# names, case twins merged the way a case-insensitive host produced them), and
# replays the migration. Needs a case-insensitive host filesystem -- on a
# case-sensitive one the twins never merge and the whole thing passes
# vacuously, so it checks first.
#
#   tests/manual/fakefs_migrate_repair.sh <rootfs.tar[.gz|.xz]> [subtree] [builddir]
#
# subtree defaults to usr/share/terminfo, which is where this reliably bites:
# every ncurses build has the 8 colliding directory pairs A/a E/e L/l M/m N/n
# P/p Q/q X/x, so on an ArchLinuxARM aarch64 root 1069 of 2899 terminfo files
# went unreachable on upgrade, xterm-256color among them.
#
# Both spellings of the merged directory are replayed, because which one it ends
# up with is just which twin the extraction created first, and the repair used to
# handle only one of them. When the merged directory keeps the *uppercase* name
# the twin is found and everything is recovered; when it keeps the lowercase one
# the recovery had to look past an exact match to see the twin at all, and a
# readdir that returned the escaped spelling first made it give up. Running only
# the uppercase variant is what let that survive, so a pass here now means both.
set -e

TARBALL=${1:?usage: fakefs_migrate_repair.sh <rootfs.tar.gz> [subtree] [builddir]}
SUBTREE=${2:-usr/share/terminfo}
BUILD=${3:-build}
ISH=$BUILD/ish
FAKEFSIFY=$BUILD/tools/fakefsify
ROOT=$(mktemp -d)/root
trap 'rm -rf "$(dirname "$ROOT")"' EXIT

for f in "$ISH" "$FAKEFSIFY"; do
    [ -x "$f" ] || { echo "missing $f (build it first)"; exit 1; }
done

probe=$(dirname "$ROOT")/probe
mkdir -p "$probe" && : > "$probe/aa"
if [ ! -e "$probe/AA" ]; then
    echo "SKIP: $(dirname "$ROOT") is case-sensitive; this bug needs a folding host (APFS)"
    exit 0
fi

fail=0
for WINNER in upper lower; do
echo "--- merged directory keeps the $WINNER-case spelling ---"
rm -rf "$ROOT"

echo "importing $TARBALL"
"$FAKEFSIFY" "$TARBALL" "$ROOT" >/dev/null
# First boot absorbs the db_inode rebuild that a freshly imported root always
# triggers. It must happen while the root is still healthy: fakefs_rebuild
# drops any path whose host entry is missing, so on a damaged root it deletes
# exactly the rows the repair needs and there is nothing left to find.
"$ISH" -f "$ROOT" /bin/true 2>/dev/null || true
before=$("$ISH" -f "$ROOT" /bin/sh -c "find /$SUBTREE -type f 2>/dev/null | wc -l" 2>/dev/null | tr -d ' ')
echo "  $before files under /$SUBTREE"

echo "rewriting /$SUBTREE into the pre-v4 on-disk format"
python3 - "$ROOT/data/$SUBTREE" "$WINNER" <<'PY'
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

def downgrade(d):
    staged = []
    for i, n in enumerate(sorted(os.listdir(d))):
        tmp = os.path.join(d, '__stg_%d' % i)
        os.rename(os.path.join(d, n), tmp)
        staged.append((tmp, decode(n)))
    # Place in ASCII order of the guest name, which is tar order: the uppercase
    # twin lands first and keeps the host name, the lowercase one merges into
    # it and its files win the name conflicts, exactly as extraction did. With
    # "lower" the pair is placed the other way round, which is the same merge an
    # archive listing the lowercase twin first produces.
    if sys.argv[2] == 'lower':
        staged.sort(key=lambda t: (t[1].lower(), not t[1][:1].islower(), t[1]))
    else:
        staged.sort(key=lambda t: t[1])
    for tmp, dn in staged:
        dst = os.path.join(d, dn)
        if not os.path.lexists(dst):
            os.rename(tmp, dst)
        elif os.path.isdir(tmp) and os.path.isdir(dst):
            for c in os.listdir(tmp):
                cdst = os.path.join(dst, c)
                if os.path.lexists(cdst):
                    os.remove(cdst)
                os.rename(os.path.join(tmp, c), cdst)
            os.rmdir(tmp)
        else:
            os.remove(dst)
            os.rename(tmp, dst)
    for n in os.listdir(d):
        p = os.path.join(d, n)
        if os.path.isdir(p):
            downgrade(p)

downgrade(sys.argv[1])
PY
sqlite3 "$ROOT/meta.db" "pragma user_version = 3"

after=$("$ISH" -f "$ROOT" /bin/sh -c "find /$SUBTREE -type f 2>/dev/null | wc -l" 2>/dev/null | tr -d ' ')
echo "  $after files readable after the migration"

if [ "$after" -eq "$before" ]; then
    echo "  $WINNER: PASS ($before files, none stranded)"
else
    echo "  $WINNER: FAIL ($((before - after)) of $before files stranded by the migration)"
    fail=1
fi
done

if [ "$fail" -eq 0 ]; then
    echo "fakefs_migrate_repair: PASS (both spellings, none stranded)"
else
    echo "fakefs_migrate_repair: FAIL"
    exit 1
fi
