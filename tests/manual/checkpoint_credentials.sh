#!/bin/sh
# checkpoint_credentials.sh -- what a restored process is, beyond its uids.
#     tests/manual/checkpoint_credentials.sh [root]
#
# The image carried the uids and gids and nothing else of a process's
# identity, so the rest came back empty or as the RESTORING task's:
#   - its executable: /proc/<pid>/exe came back empty, so readlink of it failed
#     and ktop, which reads the ELF header through that link, showed "?" as the
#     architecture of every restored process;
#   - root's capabilities, for a uid-1000 shell as much as for init: a restore
#     was a privilege escalation (measured: CapEff 0 before, 3fffffffff after);
#   - root's supplementary groups, which are none: the shell lost its own.
# The witness is an unprivileged shell -- su to the root's first account with a
# uid of 1000 or more -- which starts with no capabilities and its own groups.
# It is saved from outside mid-loop and reports the same things before and
# after.
set -e
REPO=$(cd "$(dirname "$0")/../.." && pwd)
ISH=${ISH:-$REPO/build/ish}
ROOT=${1:-$REPO/build/alpine-arm64-test}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/aok-ckcreds.XXXXXX")
IMG=$WORK/img
trap 'rm -rf "$WORK"' EXIT

cat > "$WORK/witness.sh" <<'W'
show() {
    exe=$(readlink /proc/$$/exe 2>/dev/null || echo none)
    arch=$(/AOK/native/ktop -b -n 1 2>/dev/null | awk -v p=$$ '$1 == p { for (i = 1; i <= NF; i++) if ($i ~ /^(arm64|x86_64|x86|riscv64|\?)$/) { print $i; exit } }')
    cap=$(awk '/^CapEff/ {print $2}' /proc/$$/status)
    groups=$(awk '/^Groups/ {$1 = ""; print}' /proc/$$/status | tr -s ' ' | sed 's/^ //')
    echo "$1 uid=$(id -u) exe=$exe arch=${arch:-none} cap=$cap groups=[$groups]"
}
show BEFORE
i=0; while [ $i -lt 6 ]; do sleep 1; i=$((i+1)); done
show AFTER
W
ISH_REAL_MNT=$WORK "$ISH" -f "$ROOT" /bin/sh -c 'cp /realmnt/witness.sh /tmp/witness.sh; chmod 755 /tmp/witness.sh' < /dev/null

user=$(awk -F: '$3 >= 1000 && $3 < 60000 { print $1; exit }' "$ROOT/data/etc/passwd" 2>/dev/null)
[ -n "$user" ] || { echo "checkpoint_credentials: SKIP (no account with uid >= 1000 in $ROOT)"; exit 0; }
ISH_CHECKPOINT_AFTER="3:$IMG" "$ISH" -f "$ROOT" /bin/sh -c \
    "su -s /bin/sh $user -c /tmp/witness.sh" < /dev/null > "$WORK/save.out" 2>&1 || true
[ -s "$IMG" ] || { echo "FAIL: no image written"; cat "$WORK/save.out"; exit 1; }
ISH_RESTORE="$IMG" "$ISH" -f "$ROOT" < /dev/null > "$WORK/restore.out" 2>&1 &
pid=$!
n=0; while kill -0 $pid 2>/dev/null && [ $n -lt 150 ]; do sleep 0.1; n=$((n+1)); done
kill -9 $pid 2>/dev/null || true
sed 's/^/  saved    | /' "$WORK/save.out"
sed 's/^/  restored | /' "$WORK/restore.out"

before=$(sed -n 's/^BEFORE //p' "$WORK/save.out")
after=$(sed -n 's/^AFTER //p' "$WORK/restore.out")
fail=0
f() { echo "  FAIL    | $*"; fail=1; }
[ -n "$before" ] || f "the saving run printed nothing"
[ -n "$after" ] || f "the restored process did not finish"
case $before in *"exe=none"*|*"arch=?"*|*"arch=none"*) f "the witness itself is broken before any restore: $before";; esac
case $before in *"cap=0000000000000000"*) ;; *) f "an unprivileged shell should start with no capabilities: $before";; esac
[ "$after" = "$before" ] || f "the restored process is not what was saved: [$before] -> [$after]"
[ $fail -eq 0 ] || { echo "FAIL"; exit 1; }
echo "PASS: a restored process keeps its executable, its capabilities and its groups"
