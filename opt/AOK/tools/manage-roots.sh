#!/bin/sh
# manage-roots.sh
# ---------------------------------------------------------------------------
# Install root filesystems and choose which one boots, from a shell. This is
# the command-line half of the app's Filesystems screen: the same catalog, the
# same importer, the same "Default Root" setting, reachable over ssh or from a
# script on a device whose screen you are not looking at.
#
# Companion to mount-root.sh, which does the other thing: that one chroots into
# a root you already have, this one adds roots and picks the one that boots.
#
# Usage:
#   manage-roots.sh list                    installed roots, and which boots next
#   manage-roots.sh available               root filesystems you can install
#   manage-roots.sh install <id>            install one from the catalog
#   manage-roots.sh install <archive> <name>    install a tarball on this device
#   manage-roots.sh install <url> <name>    download a tarball and install it
#   manage-roots.sh default <name>          choose the root that boots next
#   manage-roots.sh rename <name> <new>     rename an installed root
#   manage-roots.sh remove <name> --yes     delete an installed root
#   manage-roots.sh cancel                  stop the running install
#   manage-roots.sh status                  what the current job is doing
#
# Options:
#   --default      after installing, make the new root the one that boots
#   --exit-app     when done, quit iSH-AOK so the next launch boots the choice
#   --yes          required by remove, which does not ask twice
#   -q, --quiet    no progress lines
#   -h, --help     this text
#
# Examples:
#   manage-roots.sh available
#   manage-roots.sh install alpine3233_arm64 --default --exit-app
#   manage-roots.sh install /AOK/persist/roots/mine.tar.gz "My Root"
#   manage-roots.sh install https://example.org/rootfs.tar.gz Experiment
#   manage-roots.sh default Devuan6-x86_64 --exit-app
#
# Switching takes effect at the next app launch, because the root to boot is
# read once when the app starts. Nothing here disturbs the running guest, so
# you can install a root from inside the root you are using.
#
# Needs root, because /proc/ish/roots is root-writable, and needs the iSH-AOK
# app: there is no root manager in the command-line build.
# ---------------------------------------------------------------------------
set -eu

# The knob is overridable so tests/manual/roots_knob.sh can point this at a
# stand-in file and check the exact bytes that reach the app, without a device.
KNOB=${ISH_ROOTS_KNOB:-/proc/ish/roots}

QUIET=0
SET_DEFAULT=0
EXIT_APP=0
CONFIRMED=0
ARGS=''

say()  { [ "$QUIET" -eq 1 ] || printf '%s\n' "$*"; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }

usage() { sed -n '3,42p' "$0" | sed 's/^# \{0,1\}//'; }

# Root names are restricted to [A-Za-z0-9._-] by the app, but the paths and
# URLs handled here are not: an archive under a directory with a space in it is
# perfectly ordinary. Positional arguments are therefore read out of "$@" rather
# than flattened into a string, and every value reaches the knob on a line of
# its own where nothing can re-split it.
positional() {  # positional <n> ; nth non-option argument
    n=$1; shift
    i=0
    for a in "$@"; do
        case $a in
            --default|--exit-app|--yes|-q|--quiet) continue ;;
        esac
        i=$((i + 1))
        [ "$i" -eq "$n" ] && { printf '%s' "$a"; return 0; }
    done
    return 1
}

for a in "$@"; do
    case $a in
        --default)   SET_DEFAULT=1 ;;
        --exit-app)  EXIT_APP=1 ;;
        --yes)       CONFIRMED=1 ;;
        -q|--quiet)  QUIET=1 ;;
        -h|--help)   usage; exit 0 ;;
    esac
done
[ $# -gt 0 ] || { usage; exit 1; }
ARGS=$1

[ -e "$KNOB" ] || die "$KNOB does not exist; this build of iSH-AOK has no root manager"

status() { cat "$KNOB"; }

# field <kind> <key> ; the value of the first "<kind> <key>=..." line
field() {
    status | while IFS= read -r line; do
        case $line in
            "$1 $2="*) printf '%s' "${line#"$1 $2="}"; break ;;
        esac
    done
}

# Two things this has to get right, both learned the hard way on a device.
#
# Written with a plain redirect, never `cat`: busybox cat exits 0 even when its
# write(2) failed, so piping into it threw away every rejection the knob makes
# and this script reported success for commands the app had refused. The shell's
# own printf reports the failure.
#
# And terminated with `run`, because that same printf issues one write(2) per
# line: the knob receives this command in pieces and commits it when `run`
# arrives. The failure status therefore lands on the write that carries `run`,
# which is the last one, so printf still sees it.
send() {  # send <command text>
    if ! printf '%s\nrun\n' "$1" > "$KNOB" 2>/dev/null; then
        # The knob reports why it said no in the same place a failed job does,
        # so there is one place to look rather than an errno and a shrug.
        message=$(field job message)
        die "${message:-the root manager rejected that}"
    fi
}

require_app() {
    state=$(field job state)
    [ "$state" = "unavailable" ] && die "root management needs the iSH-AOK app; this is the command-line build"
    return 0
}

# Waits out a running job, printing progress as it changes. The job is the
# app's, not ours: killing this script leaves the install running, and running
# the script again picks the progress back up.
wait_for_job() {
    last=''
    while :; do
        state=$(field job state)
        [ "$state" = "running" ] || break
        line="$(field job percent)% $(field job message)"
        [ "$line" = "$last" ] || { say "  $line"; last=$line; }
        sleep 1
    done
    [ "$(field job result)" = "error" ] && die "$(field job message)"
    return 0
}

cmd_list() {
    require_app
    default=$(field default name)
    # One key per line, and `name` is the last of each record, so the fields are
    # held until the name they belong to arrives.
    status | while IFS= read -r line; do
        case $line in
            "root abi="*) abi=${line#root abi=} ;;
            "root name="*)
                name=${line#root name=}
                if [ "$name" = "$default" ]; then mark='  (boots next)'; else mark=''; fi
                printf '%-24s %s%s\n' "$name" "${abi:-unknown}" "$mark" ;;
        esac
    done
}

cmd_available() {
    require_app
    download=0
    status | while IFS= read -r line; do
        case $line in
            "available id="*)       id=${line#available id=} ;;
            "available abi="*)      abi=${line#available abi=} ;;
            "available download="*) download=${line#available download=} ;;
            "available label="*)
                if [ "$download" = "1" ]; then mark='  (downloads first)'; else mark=''; fi
                printf '%-22s %-8s %s%s\n' "${id:-?}" "${abi:-unknown}" \
                       "${line#available label=}" "$mark" ;;
        esac
    done
}

set_default_root() {  # set_default_root <name>
    send "op=default
name=$1"
    say "$1 boots at the next app launch"
}

exit_app() {
    say "quitting iSH-AOK; launch it again to boot the new root"
    send "op=exit-app"
}

case $ARGS in
    list)      cmd_list ;;
    available) cmd_available ;;
    status)
        require_app
        say "state:   $(field job state)"
        say "op:      $(field job op)"
        say "name:    $(field job name)"
        say "percent: $(field job percent)"
        # Whether the last job failed is the one thing worth knowing after the
        # fact, so it is shown rather than left for the message to imply.
        say "result:  $(field job result)"
        say "message: $(field job message)"
        ;;
    cancel)
        require_app
        send "op=cancel"
        say "asked the running job to stop"
        ;;
    install)
        require_app
        what=$(positional 2 "$@") || die "install needs a catalog id, a file, or a URL (try: $0 available)"
        name=$(positional 3 "$@") || name=''
        case $what in
            http://*|https://*)
                [ -n "$name" ] || die "installing from a URL needs a name: $0 install <url> <name>"
                send "op=install
source=url
url=$what
name=$name" ;;
            /*)
                [ -f "$what" ] || die "no such file: $what"
                [ -n "$name" ] || die "installing from a file needs a name: $0 install <archive> <name>"
                send "op=install
source=path
path=$what
name=$name" ;;
            *)
                [ -n "$name" ] && die "a catalog install names the root itself; drop the name argument"
                send "op=install
source=catalog
id=$what" ;;
        esac
        say "installing ..."
        wait_for_job
        say "$(field job message)"
        # The name is whatever the app settled on, which for a catalog install
        # it chose itself.
        [ "$SET_DEFAULT" -eq 1 ] && set_default_root "$(field job name)"
        ;;
    default)
        require_app
        name=$(positional 2 "$@") || die "default needs a root name (try: $0 list)"
        set_default_root "$name"
        ;;
    rename)
        require_app
        name=$(positional 2 "$@") || die "rename needs a root name"
        to=$(positional 3 "$@") || die "rename needs a new name"
        send "op=rename
name=$name
to=$to"
        wait_for_job
        say "renamed $name to $to"
        ;;
    remove)
        require_app
        name=$(positional 2 "$@") || die "remove needs a root name"
        [ "$CONFIRMED" -eq 1 ] || die "removing $name destroys it; add --yes if that is what you want"
        send "op=remove
name=$name
confirm=yes"
        wait_for_job
        say "removed $name"
        ;;
    *) die "unknown command: $ARGS (try --help)" ;;
esac

[ "$EXIT_APP" -eq 1 ] && exit_app
exit 0
