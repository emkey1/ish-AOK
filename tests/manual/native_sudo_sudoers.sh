#!/bin/sh
# native_sudo_sudoers.sh -- the native sudo reads sudoers.d however the main
# file spells its include.
#     tests/manual/native_sudo_sudoers.sh
#
# sudo 1.9.1 added "@includedir" / "@include" beside the old '#' spellings, and
# current distributions ship the '@' form: Alpine 3.23's stock /etc/sudoers
# ends in "@includedir /etc/sudoers.d". /AOK/native/sudo read only the '#'
# form, so on a root provisioned by /AOK/tools/provision-ultimate-alpine.sh --
# which puts "%wheel ALL=(ALL) ALL" in /etc/sudoers.d/wheel -- a user in wheel
# was told "not allowed" while the distro's own sudo matched the same rule.
# Found on a device, 2026-09-26.
#
# Builds a throwaway root from the tracked Alpine tarball (no network, and the
# shared test roots are never touched), then asks the native sudo, for each
# spelling, whether mke (in wheel) and bob (not) may run commands. The control
# case has no include line at all: mke must be refused there, or the other
# cases prove nothing about the include.
set -e
REPO=$(cd "$(dirname "$0")/../.." && pwd)
ISH=${ISH:-$REPO/build/ish}
FAKEFSIFY=${FAKEFSIFY:-$REPO/build/tools/fakefsify}
TARBALL=$REPO/alpine-minirootfs-3.23.3-aarch64.tar.xz
WORK=$(mktemp -d "${TMPDIR:-/tmp}/aok-native-sudo.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
"$FAKEFSIFY" "$TARBALL" "$WORK/root" > "$WORK/fakefsify.log" 2>&1 || {
    echo "native_sudo_sudoers: FAIL (fakefsify)"; cat "$WORK/fakefsify.log"; exit 1; }
mkdir "$WORK/mnt"

cat > "$WORK/mnt/guest.sh" <<'G'
set -e
[ -x /AOK/native/sudo ] || { echo NONATIVE > /realmnt/out-status; exit 0; }
adduser -D mke; adduser mke wheel; adduser -D bob
mkdir -p /etc/sudoers.d; chmod 0750 /etc/sudoers.d
echo '%wheel ALL=(ALL) ALL' > /etc/sudoers.d/wheel; chmod 0440 /etc/sudoers.d/wheel
ask() {  # ask <case> <user>: one line, the case, the user and the verdict
    if su -s /bin/sh "$2" -c '/AOK/native/sudo -n -l' > /tmp/ans 2>&1; then v=allowed; else v=refused; fi
    grep -q 'may run' /tmp/ans || [ $v = refused ] || v="allowed-but-said:$(cat /tmp/ans)"
    echo "$1 $2 $v" >> /realmnt/out
}
for spelling in none '@includedir /etc/sudoers.d' '#includedir /etc/sudoers.d' \
                '@include /etc/sudoers.d/wheel' '#include /etc/sudoers.d/wheel'; do
    printf 'root ALL=(ALL:ALL) ALL\n' > /etc/sudoers
    [ "$spelling" = none ] || printf '%s\n' "$spelling" >> /etc/sudoers
    chmod 0440 /etc/sudoers
    name=$(echo "$spelling" | cut -d' ' -f1)
    ask "$name" mke; ask "$name" bob
done
echo OK > /realmnt/out-status
G
ISH_REAL_MNT=$WORK/mnt "$ISH" -f "$WORK/root" /bin/sh /realmnt/guest.sh < /dev/null > "$WORK/guest.log" 2>&1 || true
case $(cat "$WORK/mnt/out-status" 2>/dev/null) in
    OK) ;;
    NONATIVE) echo "native_sudo_sudoers: SKIP (this build has no /AOK/native/sudo)"; exit 0 ;;
    *) echo "native_sudo_sudoers: FAIL (the guest run did not finish)"; cat "$WORK/guest.log"; exit 1 ;;
esac

cat > "$WORK/want" <<'W'
none mke refused
none bob refused
@includedir mke allowed
@includedir bob refused
#includedir mke allowed
#includedir bob refused
@include mke allowed
@include bob refused
#include mke allowed
#include bob refused
W
if diff -u "$WORK/want" "$WORK/mnt/out"; then
    echo "native_sudo_sudoers: PASS (10 cases)"
else
    echo "native_sudo_sudoers: FAIL"; exit 1
fi
