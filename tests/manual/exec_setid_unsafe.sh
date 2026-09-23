#!/bin/sh
# exec_setid_unsafe.sh -- a set-id exec under a tracer or no_new_privs, against
# what Linux does.
#     tests/manual/exec_setid_unsafe.sh [root]
#
# Runs exec_setid_unsafe/probe.c in the guest three ways -- "nosudo" and
# "user" as uid 1000, "root" as root -- over the set-id copies Linux's run
# used, and compares every row with exec_setid_unsafe/linux-6.12.txt. A row
# names the case and what the exec'd image got: ids, gids, the AT_* values,
# capabilities, no_new_privs, the parent-death signal and whether seteuid(0)
# works.
#
# Two differences are not behaviour and are normalised: AOK's full
# capability mask is 38 bits (3fffffffff) where 6.12's is 41 (1ffffffffff),
# and the N rows were recorded before the probe printed its pdeath field.
#
# One is a known gap and does not fail the run: R7. AOK's current_capable()
# counts an effective uid of 0 as holding every capability, whatever the
# effective set says, so root that has dropped CAP_SETUID from it is not
# downgraded as Linux downgrades it.
#
# Needs gcc in the root. Measured 2026-09-23 on devuan-amd64-test: 19 of 51
# rows matched before the fix, 50 after.
set -e
REPO=$(cd "$(dirname "$0")/../.." && pwd)
ISH=${ISH:-$REPO/build/ish}
ROOT=${1:-$REPO/build/devuan-amd64-test}
SRC=$REPO/tests/manual/exec_setid_unsafe
WORK=$(mktemp -d "${TMPDIR:-/tmp}/aok-setid.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
cp "$SRC/probe.c" "$SRC/runas.c" "$WORK/"

cat > "$WORK/guest.sh" <<'G'
set -e
W=/tmp/exec_setid_unsafe; rm -rf $W; mkdir -p $W/d; cd $W
command -v gcc >/dev/null || { echo NOGCC > /realmnt/out-status; exit 0; }
gcc -O1 -o probe /realmnt/probe.c
gcc -O1 -o runas /realmnt/runas.c
chmod 755 $W
mk() { cp probe d/$1; chown $2 d/$1; chmod $3 d/$1; }
mk plain 1000:1000 755; mk suid-own 1000:1000 4755
mk sgid-grp 1000:44 2755; mk sgid-grp-noxgrp 1000:44 2745
./runas 1000 1000 44 ./probe nosudo $W/d > /realmnt/out-nosudo 2>&1 || true
mk suid-root 0:0 4755; mk sgid-root 0:0 2755; mk sgid-root-noxgrp 0:0 2745
mk suid-sgid-root 0:0 6755; mk suid-1000 1000:1000 4755; mk suid-nobody 65534:65534 4755
./runas 1000 1000 44 ./probe user $W/d > /realmnt/out-user 2>&1 || true
./probe root $W/d > /realmnt/out-root 2>&1 || true
rm -rf $W
echo OK > /realmnt/out-status
G
ISH_REAL_MNT=$WORK "$ISH" -f "$ROOT" /bin/sh /realmnt/guest.sh < /dev/null > "$WORK/guest.log" 2>&1 || true
case $(cat "$WORK/out-status" 2>/dev/null) in
    OK) ;;
    NOGCC) echo "exec_setid_unsafe: SKIP (no gcc in $ROOT)"; exit 0 ;;
    *) echo "exec_setid_unsafe: FAIL (the guest run did not finish)"; cat "$WORK/guest.log"; exit 1 ;;
esac

cat "$WORK/out-nosudo" "$WORK/out-user" "$WORK/out-root" > "$WORK/aok.txt"
awk -v known=R7 '
    function norm(line, label) {
        gsub(/[ \t]+/, " ", line)
        gsub(/3fffffffff/, "1ffffffffff", line)
        if (label ~ /^N/) sub(/ pdeath [0-9]+/, "", line)
        return line
    }
    FNR == NR { if ($1 ~ /^[NUR][0-9]+$/) want[$1] = norm($0, $1); next }
    $1 ~ /^[NUR][0-9]+$/ { got[$1] = norm($0, $1) }
    END {
        for (k in want) {
            total++
            if (got[k] == want[k]) { same++; continue }
            if (k == known) { gap++; continue }
            printf("  FAIL %s\n    linux: %s\n    aok:   %s\n", k, want[k], (k in got) ? got[k] : "(missing)")
            bad++
        }
        printf("exec_setid_unsafe: %d of %d rows as Linux, %d known gap, %d wrong\n", same, total, gap, bad)
        exit bad != 0
    }' "$SRC/linux-6.12.txt" "$WORK/aok.txt" && echo "exec_setid_unsafe: PASS" || { echo "exec_setid_unsafe: FAIL"; exit 1; }
