#!/bin/sh
# suspend_script.sh -- opt/AOK/tools/suspend.sh says what happened to the save.
#     tests/manual/suspend_script.sh [root]      (needs unshare and dash in root)
# A write to /proc/ish/checkpoint only queues the save, which runs just after the
# write returns, so the write succeeding says nothing about it. --save printed
# "saved to FILE" for a save that was refused and wrote nothing, and a refused
# suspend ended silently with status 0. A resumed session's shell comes back
# inside the same script, so that must not read as a refusal.
#
# The script is run as the docs say, by its path. A bare `sh` on a root with the
# native links is native dash, which a resume re-launches from the top instead of
# continuing: `sh suspend.sh` there would suspend again on every resume.
#
# SUSPEND_SH names another copy of the script, for the control: the old one fails
# the two refusal legs.
set -e
REPO=$(cd "$(dirname "$0")/../.." && pwd)
ISH=${ISH:-$REPO/build/ish}
ROOT=${1:-$REPO/build/devuan-arm64-test}
SUSPEND_SH=${SUSPEND_SH:-$REPO/opt/AOK/tools/suspend.sh}
SH=/bin/dash
[ -d "$ROOT" ] || { echo "no root at $ROOT" >&2; exit 2; }
WORK=$(mktemp -d "${TMPDIR:-/tmp}/aok-suspend.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
cp "$SUSPEND_SH" "$WORK/suspend.sh"
chmod +x "$WORK/suspend.sh"

fail=0
# guest <NAME=value env, or empty> <program>
guest() {
    env $1 ISH_GUEST_CHECKPOINT=1 ISH_REAL_MNT="$WORK" "$ISH" -f "$ROOT" $SH -c "$2" < /dev/null 2>&1 || true
}
# want <leg> <output> <pattern...>: each pattern in the output, or !pattern not in it
want() {
    leg=$1 out=$2
    shift 2
    for p in "$@"; do
        case $p in
            !*) case $out in *"${p#!}"*) echo "  FAIL | $leg: has '${p#!}'"; echo "$out" | sed 's/^/       /'; fail=1 ;; esac ;;
            *)  case $out in *"$p"*) ;; *) echo "  FAIL | $leg: no '$p'"; echo "$out" | sed 's/^/       /'; fail=1 ;; esac ;;
        esac
    done
}
HOLD='unshare -m sleep 30 & k=$!; sleep 1;'
RELEASE='kill $k'

# Nothing in the way.
out=$(guest "" "/realmnt/suspend.sh --save $WORK/saved; echo rc=\$?")
want "--save" "$out" "saved to $WORK/saved" "rc=0" "!refused"
[ -s "$WORK/saved" ] || { echo "  FAIL | --save: no image"; fail=1; }

# A process no image can carry, refused only once the save runs.
out=$(guest "" "$HOLD /realmnt/suspend.sh --save $WORK/refused; echo rc=\$?; $RELEASE")
want "--save refused" "$out" "refused" "namespace of its own" "rc=1" "!saved to"
[ ! -e "$WORK/refused" ] || { echo "  FAIL | --save refused: an image exists"; fail=1; }
out=$(guest "ISH_SESSION=$WORK/sus-refused" "$HOLD /realmnt/suspend.sh; echo rc=\$?; $RELEASE")
want "suspend refused" "$out" "refused" "namespace of its own" "rc=1"
[ ! -e "$WORK/sus-refused" ] || { echo "  FAIL | suspend refused: an image exists"; fail=1; }

# A copy, resumed: the shell comes back inside the script.
out=$(guest "" "/realmnt/suspend.sh --save $WORK/copy; echo rc=\$?")
want "--save for a copy" "$out" "saved to $WORK/copy" "rc=0"
out=$(guest "ISH_RESTORE=$WORK/copy" "echo this-argv-must-be-ignored")
want "--save, resumed" "$out" "saved to $WORK/copy" "rc=0" "!refused" "!this-argv"

# A suspend, then two resumes: each comes back in the script and carries on.
S="ISH_SESSION=$WORK/session"
TWICE='/realmnt/suspend.sh; echo first=$?; /realmnt/suspend.sh; echo second=$?'
out=$(guest "$S" "$TWICE")
want "suspend" "$out" "!first=" "!refused"
[ -s "$WORK/session" ] || { echo "  FAIL | suspend: no image"; fail=1; }
out=$(guest "$S" "$TWICE")
want "suspend, resumed" "$out" "first=0" "!second=" "!refused"
out=$(guest "$S" "$TWICE")
want "suspend, resumed again" "$out" "second=0" "!refused"

# The same under a NATIVE shell, which is what a bare `sh` is on a root with the
# native links and what /bin/sh is on the native root. A resume re-launches it
# from its command line, so the script runs again from the top: it must know it
# has been resumed rather than suspend again, and must put back the output its
# own redirect to the control file was holding.
NSH=/AOK/native/sh
if ISH_REAL_MNT="$WORK" "$ISH" -f "$ROOT" $SH -c "[ -x $NSH ]" < /dev/null 2>/dev/null; then
    NS="ISH_SESSION=$WORK/nsession"
    NTWICE="$NSH /realmnt/suspend.sh; echo first=\$?; $NSH /realmnt/suspend.sh; echo second=\$?"
    out=$(guest "$NS" "$NTWICE")
    want "native sh: suspend" "$out" "!first=" "!refused"
    [ -s "$WORK/nsession" ] || { echo "  FAIL | native sh: suspend: no image"; fail=1; }
    out=$(guest "$NS" "$NTWICE")
    want "native sh: suspend, resumed" "$out" "first=0" "!second=" "!refused" "!I/O error"
    out=$(guest "$NS" "$NTWICE")
    want "native sh: suspend, resumed again" "$out" "second=0" "!refused" "!I/O error"

    out=$(guest "" "$NSH /realmnt/suspend.sh --save $WORK/ncopy; echo rc=\$?")
    want "native sh: --save" "$out" "saved to $WORK/ncopy" "rc=0" "!refused"
    before=$(ls -l "$WORK/ncopy")
    sleep 1
    out=$(guest "ISH_RESTORE=$WORK/ncopy" "echo this-argv-must-be-ignored")
    want "native sh: --save, resumed" "$out" "saved to $WORK/ncopy" "rc=0" "!refused" "!I/O error" "!this-argv"
    # The resumed copy must not have saved again over the image it came from.
    [ "$(ls -l "$WORK/ncopy")" = "$before" ] || { echo "  FAIL | native sh: --save, resumed: saved again"; fail=1; }
else
    echo "  SKIP | no $NSH in $ROOT"
fi

[ "$fail" -eq 0 ] && echo "suspend_script: PASS" || { echo "suspend_script: FAIL"; exit 1; }
