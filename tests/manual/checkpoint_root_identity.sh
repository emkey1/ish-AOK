#!/bin/sh
# checkpoint_root_identity.sh -- a saved session belongs to the root it was
# saved on (GH #607).
#     tests/manual/checkpoint_root_identity.sh [rootfs tarball]
#
# The image recorded no root at all, so it resumed on whichever root was
# mounted: saved on Devuan and resumed on Alpine, the "Devuan" shell read
# Alpine's /etc/os-release, and a descriptor open for writing would have
# written into Alpine's files.
#
# A root is its data directory's host inode (checkpoint_root_identity), so:
#   another root      refused, and the image is left for its own root
#   a COPY of it      refused too -- same contents and the same name, but
#                     another filesystem from the moment it was made
#   the root renamed  resumes: a rename is still the same files
# The rename leg runs LAST, on the image both refusals were given: a refusal
# that damaged or consumed the image fails it.
set -e
REPO=$(cd "$(dirname "$0")/../.." && pwd)
ISH=${ISH:-$REPO/build/ish}
FAKEFSIFY=${FAKEFSIFY:-$REPO/build/tools/fakefsify}
TARBALL=${1:-$REPO/alpine-minirootfs-3.23.3-aarch64.tar.xz}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/aok-rootid.XXXXXX")
IMG=$WORK/img
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/one" "$WORK/other" "$WORK/copy"
"$FAKEFSIFY" "$TARBALL" "$WORK/one/root-a" > /dev/null
"$FAKEFSIFY" "$TARBALL" "$WORK/other/root-b" > /dev/null

# Saved from outside, 2 s in, while the loop runs; the run that saved goes on
# to the end. The token is the saved process's own: only that process, brought
# back, can print it after the loop.
ISH_CHECKPOINT_AFTER="2:$IMG" "$ISH" -f "$WORK/one/root-a" /bin/sh -c \
    'tok=$(head -c 6 /dev/urandom | od -An -tx1 | tr -d " \n"); echo "BEFORE $tok"
     i=0; while [ $i -lt 5 ]; do sleep 1; i=$((i+1)); done
     echo "AFTER $tok"' < /dev/null > "$WORK/save.out" 2>&1 || true
[ -s "$IMG" ] || { echo "FAIL: no image written"; cat "$WORK/save.out"; exit 1; }
tok=$(sed -n 's/^BEFORE //p' "$WORK/save.out")
[ -n "$tok" ] || { echo "FAIL: the saving run printed no token"; cat "$WORK/save.out"; exit 1; }

cp -c -R "$WORK/one/root-a" "$WORK/copy/root-a" 2>/dev/null || cp -R "$WORK/one/root-a" "$WORK/copy/root-a"

fail=0
f() { echo "  FAIL    | $*"; fail=1; }
resume() {   # <label> <root>; prints the run's output, returns its status
    ISH_RESTORE="$IMG" "$ISH" -f "$2" < /dev/null > "$WORK/$1.out" 2>&1 &
    pid=$!
    n=0; while kill -0 $pid 2>/dev/null && [ $n -lt 150 ]; do sleep 0.1; n=$((n+1)); done
    kill -9 $pid 2>/dev/null || true
    st=0; wait $pid 2>/dev/null || st=$?
    sed "s/^/  $1 | /" "$WORK/$1.out"
    return $st
}
refused() {   # <label> <root>
    if resume "$1" "$2"; then
        f "$1: the resume on $2 succeeded"
    fi
    grep -q "^AFTER " "$WORK/$1.out" && f "$1: the saved process ran on $2"
    grep -q 'saved on the root "root-a", not on this one' "$WORK/$1.out" ||
        f "$1: no refusal naming the root"
    [ -s "$IMG" ] || f "$1: the refusal took the image with it"
}
refused other "$WORK/other/root-b"
refused copy "$WORK/copy/root-a"
mv "$WORK/one/root-a" "$WORK/one/renamed"
resume renamed "$WORK/one/renamed" || true
grep -qx "AFTER $tok" "$WORK/renamed.out" ||
    f "renamed: the session did not come back on its own root, renamed"
[ $fail -eq 0 ] || { echo "FAIL"; exit 1; }
echo "PASS: a session resumed only on the root it was saved on -- refused on another root and on a copy, back on its own root after a rename"
