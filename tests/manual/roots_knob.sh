#!/bin/sh
# roots_knob.sh -- regression test for /proc/ish/roots and manage-roots.sh.
#
# Two halves, because the feature has two halves that fail differently.
#
# 1. The knob, against the command-line build of ish. There is no root manager
#    there, so the whole surface must degrade to a clear message rather than
#    calling through the NULL bridge pointer. That is not hypothetical:
#    /proc/ish/documents took the entire emulator down exactly this way under
#    stress-ng --procfs, so this walks the file the way stress-ng would.
#
# 2. The protocol, against a stand-in knob. manage-roots.sh is the only writer
#    the app will ever have, so the bytes it emits ARE the interface, and they
#    are asserted here literally. A device is not needed and not used: what a
#    device would add is the app's side of the conversation, which is not what
#    breaks when someone edits the front end.
#
#   sh tests/manual/roots_knob.sh [rootfs.tar.gz] [builddir]
#
# With no tarball, the knob half is skipped and the protocol half still runs,
# so this is useful on any machine.

set -eu

HERE=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
SCRIPT=$HERE/../../opt/AOK/tools/manage-roots.sh
TARBALL=${1:-}
BUILD=${2:-build}

[ -f "$SCRIPT" ] || { echo "cannot find manage-roots.sh at $SCRIPT" >&2; exit 1; }

WORK=$(mktemp -d) || exit 1
trap 'rm -rf "$WORK"' EXIT INT TERM

PASS=0
FAIL=0
ok()  { PASS=$((PASS + 1)); }
bad() { FAIL=$((FAIL + 1)); printf '  FAIL %s\n' "$*"; }

want() {  # want <description> <expected> <actual>
    if [ "$2" = "$3" ]; then ok; else bad "$1
       expected: $2
       actual:   $3"; fi
}

wantin() {  # wantin <description> <pattern> <file>
    if grep -q -- "$2" "$3"; then ok; else
        bad "$1 (no '$2' in output)"
        sed 's/^/       | /' "$3"
    fi
}

wantnotin() {  # wantnotin <description> <pattern> <file>
    if grep -q -- "$2" "$3"; then
        bad "$1 (unwanted '$2' in output)"
        sed 's/^/       | /' "$3"
    else ok; fi
}

# ------------------------------------------------- 1. the knob, on the CLI build

if [ -n "$TARBALL" ] && [ -x "$BUILD/ish" ] && [ -x "$BUILD/tools/fakefsify" ]; then
    echo "1. /proc/ish/roots on the command-line build"
    ROOT=$WORK/root
    "$BUILD/tools/fakefsify" "$TARBALL" "$ROOT" >/dev/null 2>&1 || {
        echo "  could not import $TARBALL; skipping the knob half" >&2
        TARBALL=''
    }
fi

if [ -n "$TARBALL" ] && [ -d "${ROOT:-}" ]; then
    guest() { "$BUILD/ish" -f "$ROOT" /bin/sh -c "$1" 2>&1; }

    guest 'cat /proc/ish/roots' > "$WORK/read"
    wantin "reading it says the manager is unavailable" "job state=unavailable" "$WORK/read"

    # The entry must be root-writable and world-readable like the other knobs.
    guest 'ls -l /proc/ish/roots' > "$WORK/mode"
    wantin "mode is 0644" "^-rw-r--r--" "$WORK/mode"

    # A write with no manager behind it must fail cleanly, not crash and not
    # silently succeed.
    guest 'printf "op=default\nname=x\n" > /proc/ish/roots 2>/dev/null; echo "write=$?"' > "$WORK/write"
    wantin "writing without the app fails" "write=1" "$WORK/write"

    # The stress-ng shape: garbage, oversize, embedded NUL, empty, and a read
    # after each one. Surviving with the emulator still up is the whole point.
    guest '
        for junk in "" "garbage" "op=" "=value" "op=install
source=nonsense" ; do
            printf "%s" "$junk" > /proc/ish/roots 2>/dev/null || true
            cat /proc/ish/roots > /dev/null || true
        done
        dd if=/dev/zero bs=1024 count=64 2>/dev/null > /proc/ish/roots 2>/dev/null || true
        printf "op=default\x00name=x\n" > /proc/ish/roots 2>/dev/null || true
        cat /proc/ish/roots > /dev/null
        echo "survived"
    ' > "$WORK/abuse"
    wantin "survives garbage, oversize and NUL-bearing writes" "survived" "$WORK/abuse"

    # And the front end, in the place it will actually run.
    guest 'sh /AOK/tools/manage-roots.sh list 2>&1; echo "exit=$?"' > "$WORK/cli"
    wantin "manage-roots.sh ships at /AOK/tools" "needs the iSH-AOK app" "$WORK/cli"
    wantin "and exits non-zero rather than pretending" "exit=1" "$WORK/cli"

    # A knob that accepts the open and then fails the write, which is what a
    # rejected command really looks like: the app returns an errno from the
    # write, not from the open. /dev/full does exactly that and needs no app.
    #
    # This is the shape that got through: the script used to pipe the command
    # into `cat > $KNOB`, and busybox cat exits 0 even when its write failed, so
    # every rejection was thrown away and the script reported success for a
    # command the app had refused. Proven right here: a redirect gives 1 where
    # cat gives 0.
    guest 'printf x > /dev/full 2>/dev/null; echo "redirect=$?"; printf x | cat > /dev/full 2>/dev/null; echo "cat=$?"' > "$WORK/full"
    wantin "a failing write is visible to a redirect" "redirect=1" "$WORK/full"
    wantin "and invisible to cat, which is why it is not used" "cat=0" "$WORK/full"

    # Pointed at a real sibling knob rather than /dev/full, whose reads never
    # end. i386_jit_fuse has exactly the shape needed and is not a stand-in for
    # anything: opening it works, reading it returns something finite that is
    # not a roots status, and writing a command it does not understand fails the
    # way a rejected roots command does.
    guest 'ISH_ROOTS_KNOB=/proc/ish/i386_jit_fuse sh /AOK/tools/manage-roots.sh default Whatever 2>&1; echo "exit=$?"' > "$WORK/reject"
    wantin "a rejected command is not reported as success" "exit=1" "$WORK/reject"
    wantnotin "and does not claim the switch happened" "boots at the next app launch" "$WORK/reject"

    # The reason the protocol has a `run` terminator at all: the shell's printf
    # issues one write(2) per line, so a command arrives in pieces. Demonstrated
    # on i386_jit_fuse, whose parser rejects a whole write atomically: writing
    # "addr=1\nbogus=1" leaves addr ON, which can only happen if the two lines
    # arrived as two separate writes and the first was applied on its own.
    guest '
        echo "addr=0" > /proc/ish/i386_jit_fuse
        printf "addr=1\nbogus=1\n" > /proc/ish/i386_jit_fuse 2>/dev/null || true
        grep "^addr " /proc/ish/i386_jit_fuse
    ' > "$WORK/split"
    wantin "a multi-line write really does arrive as several writes" "addr on" "$WORK/split"
else
    echo "1. /proc/ish/roots on the command-line build (skipped: no rootfs tarball given)"
fi

# ------------------------------------------------- 2. the protocol, on a stand-in

echo "2. the bytes manage-roots.sh sends"

KNOB=$WORK/knob
run() {  # run <args...> ; output in $WORK/out, status in $STATUS
    STATUS=0
    ISH_ROOTS_KNOB="$KNOB" sh "$SCRIPT" "$@" > "$WORK/out" 2>&1 || STATUS=$?
}

seed() { cat > "$KNOB"; }        # pretend to be the app's status
sent() { cat "$KNOB"; }          # what the script wrote over it

# A status with two roots, one catalog entry, and an idle job. Names deliberately
# include the one the app says boots next, to prove the marker follows the
# "default name=" line rather than position.
seed <<'STATUS'
root abi=aarch64
root default=0
root name=default
root abi=amd64
root default=1
root name=Devuan6-x86_64
available id=alpine3233_arm64
available abi=aarch64
available download=0
available label=Alpine 3.23.3 (arm64)
job state=idle
job op=
job percent=0
job result=
job name=
job message=
default name=Devuan6-x86_64
STATUS

# Asserted by meaning rather than by column, so tidying the layout later is not
# a test failure: every root appears once, with its own ABI, and the marker sits
# on the one the app said boots next.
run list
wantin "list shows a root with its ABI" "^default  *aarch64$" "$WORK/out"
wantin "list marks the root that boots next" "^Devuan6-x86_64  *amd64  (boots next)$" "$WORK/out"
want "list prints one line per root" "2" "$(wc -l < "$WORK/out" | tr -d ' ')"
wantnotin "list does not leak protocol fields" "default=" "$WORK/out"

run available
wantin "available shows the catalog id, its ABI and its label on one line" \
       "^alpine3233_arm64 *aarch64 *Alpine 3.23.3 (arm64)$" "$WORK/out"
wantnotin "available does not leak protocol fields" "download=" "$WORK/out"

run status
wantin "status reports the job state" "state:   idle" "$WORK/out"
wantnotin "status does not run fields together" "idle op=" "$WORK/out"

# A job the app accepted and then failed. Caught in the simulator: the app used
# to pack several key=values onto the state line, so "result=error" was not the
# whole value of any line, the front end never found it, and a failed install
# printed the failure and then exited 0. Anything scripting this would have
# carried on as though it had a new root.
#
# Asserted through `status` rather than by running an install, because an
# install writes to the knob first and a file, unlike the real thing, does not
# keep serving a status after being written to. What broke was the front end's
# ability to find `result` at all, and this pins exactly that.
seed <<'STATUS'
root abi=aarch64
root default=0
root name=default
job state=done
job op=install
job percent=40
job result=error
job name=Experiment
job message=the download failed
default name=default
STATUS
run status
wantin "a failed job's result is found, not lost in a packed line" "^result:  error$" "$WORK/out"
wantin "and its reason is reported" "the download failed" "$WORK/out"
wantin "the job's own name is reported" "name:    Experiment" "$WORK/out"

# Each command below rewrites the knob, so the seed is restored first and the
# assertion is on the exact bytes the app would receive.
protocol() {  # protocol <description> <expected bytes> <args...>
    description=$1; expected=$2; shift 2
    seed <<'STATUS'
root abi=aarch64
root default=0
root name=default
job state=done
job op=
job percent=100
job result=ok
job name=
job message=done
default name=default
STATUS
    run "$@"
    want "$description" "$expected" "$(sent)"
}

protocol "default sends op and name on separate lines" \
    "op=default
name=Devuan6-x86_64
run" \
    default Devuan6-x86_64

protocol "a catalog install sends no name" \
    "op=install
source=catalog
id=alpine3233_arm64
run" \
    install alpine3233_arm64

protocol "a URL install carries the URL verbatim" \
    "op=install
source=url
url=https://example.org/a b/rootfs.tar.gz?x=1
name=Experiment
run" \
    install "https://example.org/a b/rootfs.tar.gz?x=1" Experiment

protocol "remove sends the confirmation the knob requires" \
    "op=remove
name=default
confirm=yes
run" \
    remove default --yes

protocol "rename sends both names" \
    "op=rename
name=default
to=renamed
run" \
    rename default renamed

# A path install needs the file to exist, so it gets its own case.
: > "$WORK/my root.tar.gz"
protocol "a path install carries a path containing a space" \
    "op=install
source=path
path=$WORK/my root.tar.gz
name=Mine
run" \
    install "$WORK/my root.tar.gz" Mine

echo "3. what it refuses"

seed <<'STATUS'
root abi=aarch64
root default=0
root name=default
job state=idle
job op=
job percent=0
job result=
job name=
job message=
default name=default
STATUS

run remove default
if [ "$STATUS" -ne 0 ]; then ok; else bad "remove without --yes should fail"; fi
wantin "remove without --yes explains itself" "add --yes" "$WORK/out"
wantnotin "remove without --yes sends nothing" "op=remove" "$KNOB"

run install https://example.org/rootfs.tar.gz
if [ "$STATUS" -ne 0 ]; then ok; else bad "a URL install with no name should fail"; fi
wantin "a URL install with no name says so" "needs a name" "$WORK/out"

run install /nonexistent/rootfs.tar.gz Name
if [ "$STATUS" -ne 0 ]; then ok; else bad "installing a missing file should fail"; fi
wantin "a missing archive is named" "no such file" "$WORK/out"

run install alpine3233_arm64 SomeName
if [ "$STATUS" -ne 0 ]; then ok; else bad "a catalog install with a name should fail"; fi
wantin "a catalog install rejects a name argument" "names the root itself" "$WORK/out"

run nonsense
if [ "$STATUS" -ne 0 ]; then ok; else bad "an unknown command should fail"; fi

# The knob's own refusal has to reach the user: the app reports why it said no
# in the job message, and losing that is how a clear error becomes "it failed".
cat > "$KNOB" <<'STATUS'
job state=done
job op=default
job percent=0
job result=error
job name=
job message=no such root: Nope
default name=default
STATUS
chmod 0444 "$KNOB"
run default Nope
chmod 0644 "$KNOB"
if [ "$STATUS" -ne 0 ]; then ok; else bad "a rejected write should fail"; fi
wantin "the knob's reason is what gets printed" "no such root: Nope" "$WORK/out"

echo ""
if [ "$FAIL" -eq 0 ]; then
    echo "PASS ($PASS checks)"
    exit 0
fi
echo "FAIL ($FAIL of $((PASS + FAIL)) checks)"
exit 1
