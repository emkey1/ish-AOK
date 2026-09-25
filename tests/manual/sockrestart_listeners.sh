#!/bin/sh
# sockrestart_listeners.sh -- guest listening sockets across the app's
# suspend/resume socket cycle (fs/sockrestart.c), driven from the CLI.
#
# ISH_SOCKRESTART_AFTER=1 runs the app's save (sockrestart_on_suspend) and
# rebuild (sockrestart_on_resume) from a host thread one second in.
# ISH_SOCKRESTART_TEST_DESTROY kills the listeners in between, since nothing on
# a Mac does:
#
#   (unset)      nothing is destroyed: the resume the app runs when it was
#                backgrounded and never suspended. Nothing may be rebuilt, and
#                a connection waiting in a listener's queue must still be there.
#   1            every listener replaced by a fresh unbound socket. Any host.
#   defunct      what iOS does to a suspended app. Darwin only: TCP listeners
#                are defunct, AF_UNIX ones are exempt and must be left alone.
#   defunct-all  the same, with the AF_UNIX listeners made defunct as well.
#
# Every case checks the probe's verdict AND how many listeners the resume
# rebuilt, so a case cannot pass because nothing happened.
#
# Not covered: a server blocked in accept() under =1. The fresh socket makes
# that accept() fail with EINVAL before the rebuild runs, where a defunct
# listener keeps it waiting (the defunct cases cover the shape).
#
#     tests/manual/sockrestart_listeners.sh [root]
#
# `root` defaults to build/devuan-arm64-test and needs a C compiler. $ISH
# overrides the binary (default build/ish).
set -u
REPO=$(cd "$(dirname "$0")/../.." && pwd)
ISH=${ISH:-$REPO/build/ish}
ROOT=${1:-$REPO/build/devuan-arm64-test}
[ -d "$ROOT" ] || { echo "no root at $ROOT" >&2; exit 2; }
[ -x "$ISH" ] || { echo "no ish at $ISH" >&2; exit 2; }
WORK=$(mktemp -d "${TMPDIR:-/tmp}/aok-sockrestart.XXXXXX") || exit 2
trap 'rm -rf "$WORK"' EXIT

cp "$REPO/tests/manual/sockrestart_listeners.c" "$WORK/"
ISH_REAL_MNT=$WORK "$ISH" -f "$ROOT" /bin/sh -c \
    'cc -O1 -o /realmnt/probe /realmnt/sockrestart_listeners.c' < /dev/null \
    || { echo "FAIL: could not build the probe"; exit 1; }

failures=0
fail() {
    echo "FAIL: $*"
    failures=$((failures + 1))
}

# run_case DESTROY MODE REBUILT -- DESTROY is "-" for unset.
run_case() {
    destroy=$1 mode=$2 want=$3
    out=$WORK/$destroy-$mode.out
    if [ "$destroy" = - ]; then
        env -u ISH_SOCKRESTART_TEST_DESTROY ISH_SOCKRESTART_AFTER=1 ISH_REAL_MNT=$WORK \
            "$ISH" -f "$ROOT" /realmnt/probe "$mode" < /dev/null > "$out" 2>&1 &
    else
        env ISH_SOCKRESTART_TEST_DESTROY=$destroy ISH_SOCKRESTART_AFTER=1 ISH_REAL_MNT=$WORK \
            "$ISH" -f "$ROOT" /realmnt/probe "$mode" < /dev/null > "$out" 2>&1 &
    fi
    pid=$!
    n=0
    while kill -0 $pid 2>/dev/null && [ $n -lt 400 ]; do sleep 0.1; n=$((n + 1)); done
    if kill -0 $pid 2>/dev/null; then
        kill -9 $pid 2>/dev/null
        wait $pid 2>/dev/null
        fail "destroy=$destroy $mode: hung for 40s"
        sed 's/^/    | /' "$out"
        return
    fi
    wait $pid 2>/dev/null
    ok=1
    grep -qx "PASS $mode" "$out" || ok=0
    grep -qx "sockrestart: rebuilt $want listener(s)" "$out" || ok=0
    if [ $ok = 1 ]; then
        echo "ok: destroy=$destroy $mode (rebuilt $want)"
    else
        fail "destroy=$destroy $mode, wanted PASS and rebuilt $want"
        sed 's/^/    | /' "$out"
    fi
}

while read -r destroy mode want; do
    [ -n "$destroy" ] && run_case "$destroy" "$mode" "$want"
done <<EOF
- survive 0
- abstract 0
- pending 0
- accept 0
- poll 0
- epollet 0
- relisten 0
- mixed 0
- tcp 0
- tcp-pending 0
- tcp-accept 0
- tcp-epollet 0
1 survive 1
1 abstract 1
1 poll 1
1 epollet 1
1 relisten 1
1 mixed 2
1 tcp 1
1 tcp-epollet 1
EOF

if [ "$(uname -s)" = Darwin ]; then
    while read -r destroy mode want; do
        [ -n "$destroy" ] && run_case "$destroy" "$mode" "$want"
    done <<EOF
defunct pending 0
defunct accept 0
defunct mixed 1
defunct tcp-accept 1
defunct tcp-epollet 1
defunct-all survive 1
defunct-all abstract 1
defunct-all accept 1
defunct-all poll 1
defunct-all epollet 1
defunct-all relisten 1
defunct-all mixed 2
EOF
else
    echo "skip: the defunct cases need Darwin"
fi

if [ $failures -ne 0 ]; then
    echo "FAIL: $failures case(s)"
    exit 1
fi
echo "PASS: listeners serve after the cycle, the dead ones rebuilt and the live ones untouched"
