#!/bin/sh
# suspend.sh -- save this iSH-AOK session, or check whether it can be saved.
#
#     /AOK/tools/suspend.sh              save the session and stop
#     /AOK/tools/suspend.sh --save FILE  save a copy and keep running
#     /AOK/tools/suspend.sh --status     what the last attempt did
#
# WHAT SUSPENDING DOES. Every process, its memory, its open files and its place
# in the process tree are written to one file, and the next launch resumes from
# it instead of booting. iOS ends this app routinely -- memory pressure, or a
# swipe away -- and without this that always loses the session.
#
# You do not normally have to run this. With "Suspend to Disk" switched on in
# Settings, iSH-AOK saves the session by itself whenever the app goes to the
# background, which is the case this exists for. This is the manual form, for
# when you want the session saved NOW.
#
# WHAT IT CANNOT SAVE, and it says so rather than saving something that will
# not come back:
#
#   - A socket, or any other descriptor with no rule for rebuilding it.
#     Regular files, directories, terminals, pipes and the standard streams all
#     have one.
#
# Then nothing is written, the session carries on untouched, and --status names
# the process and the reason.
#
# A native program does not stop a save: /AOK/native/zsh describes itself, and
# the others -- dash, which is `sh` on a root with the native links and /bin/sh
# on the native root -- are re-launched from their command line when the
# session comes back. This script copes with being run by one of those (see
# RESUMED below).
set -e

CONTROL=/proc/ish/checkpoint
SELF=$(readlink -f "$0" 2>/dev/null) || SELF=$0

# Run by a NATIVE shell, this script is not continued when the session comes
# back: the shell is re-launched from its command line, so the script runs
# again from the top -- and without this its first act on the resumed machine
# would be to ask for another suspend, and a resumed --save would save again
# with its output still pointing at the control file. The shell's descriptors
# do come back as they were at the save, so ask() leaves three for this to
# find: 9 open on this script, which is how a re-launched run knows it is one,
# and 7 and 6 holding the output and errors the redirect to the control file
# had set aside.
RESUMED=
if [ "$(readlink "/proc/$$/fd/9" 2>/dev/null)" = "$SELF" ]; then
    exec 1>&7 2>&6 7>&- 6>&- 9<&-
    RESUMED=1
fi

# ask <request>: write it to the control file.
ask() {
    native=
    case $(readlink "/proc/$$/exe" 2>/dev/null) in
        /AOK/native/*) native=1 ;;
    esac
    if [ -n "$native" ]; then
        exec 9<"$SELF"
        { exec 7>&1; } 2>/dev/null || exec 7>/dev/null
        { exec 6>&2; } 2>/dev/null || exec 6>/dev/null
    fi
    asked=0
    echo "$1" > "$CONTROL" 2>/dev/null || asked=$?
    [ -z "$native" ] || exec 9<&- 7>&- 6>&-
    return "$asked"
}

usage() {
    sed -n '2,/^set -e$/p' "$0" | sed 's/^# \{0,1\}//; /^set -e$/d'
}

status() {
    # The file is the report. Everything above the blank line is the inventory
    # -- what a checkpoint would have to deal with in this guest -- and what
    # follows it is what actually happened.
    cat "$CONTROL"
}

# One line of that report.
report() {
    status | sed -n "s/^$1 *//p"
}

# A write to the file only queues the save, which runs just after the write
# returns, so the write succeeding says nothing about the save. Past it, the
# report says. It lasts until the next save ends, which clears a refusal.
not_written() {
    why=$(report last_refusal)
    echo "${why:-another save was being written}" >&2
    report last_err | sed 's/^/error /' >&2
    exit 1
}

case ${1-} in
    --status|-s|status)
        status
        ;;
    --help|-h|help)
        usage
        ;;
    --save)
        if [ -z "${2-}" ]; then
            echo "suspend.sh: --save needs a file to write" >&2
            exit 2
        fi
        # A COPY: the guest carries on afterwards. The path is a HOST path --
        # somewhere on the device outside the guest filesystem -- because that
        # is where a session has to live to survive the app being killed.
        if [ -z "$RESUMED" ] && ! ask "save $2"; then
            echo "suspend.sh: refused. Why:" >&2
            report last_refusal >&2
            report last_err | sed 's/^/error /' >&2
            exit 1
        fi
        # A resumed copy of this session comes back here too, on a machine that
        # has made no saves yet, and it being here is the proof it was saved.
        if [ -n "$(report last_err)" ] ||
                { [ "$(report saves)" != 0 ] && [ "$(report last_path)" != "$2" ]; }; then
            echo "suspend.sh: refused. Why:" >&2
            not_written
        fi
        echo "saved to $2"
        ;;
    "")
        # A DEPARTURE: the image is written and the machine stops, so the next
        # launch resumes rather than boots.
        if [ -z "$RESUMED" ] && ! ask suspend; then
            echo "suspend.sh: refused." >&2
            echo "  Turn on Suspend to Disk in Settings, or on the command" >&2
            echo "  line run iSH-AOK with ISH_GUEST_CHECKPOINT=1 and a" >&2
            echo "  session file (ISH_SESSION=<path>)." >&2
            status | sed -n 's/^last_refusal */  /p' >&2
            exit 1
        fi
        # A suspend that worked stopped the machine just after the write, so only
        # two things get here: the resumed session, on a machine that has made no
        # saves yet, and a machine still running because the save was refused.
        if [ -n "$(report last_err)" ] || [ "$(report saves)" != 0 ]; then
            echo "suspend.sh: refused, and the session is still running. Why:" >&2
            not_written
        fi
        ;;
    *)
        echo "suspend.sh: unknown argument '$1'" >&2
        usage >&2
        exit 2
        ;;
esac
