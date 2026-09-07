#!/bin/sh
# install-argv-capture.sh -- put tools/argv_capture_guest.c in front of the
# commands a failing login MOTD ran, INSIDE the guest.
#
# For the open argv-corruption report: a login MOTD on the 5th-gen iPad ran
#
#     command df -h / 2>/dev/null | awk 'NR==2{print $3" / "$2" ("$5")"}'
#
# and df behaved as though it had been handed neither `-h' nor `/' but awk's
# program text, with its stderr escaping the 2>/dev/null. It has not recurred.
# This installs the capture wrapper so that the next occurrence is recorded
# rather than remembered, with the argv AND the descriptor wiring that the
# report needs and a screenshot cannot give.
#
# Run it IN THE GUEST, as root:
#
#     sh install-argv-capture.sh install     # build, install, start recording
#     sh install-argv-capture.sh off         # stop recording, leave installed
#     sh install-argv-capture.sh on          # resume recording
#     sh install-argv-capture.sh uninstall   # remove everything
#     sh install-argv-capture.sh status      # what is installed and recording
#
# argv_capture_guest.c must be beside this script, or named in $SRC.

set -e

SRC=${SRC:-$(dirname "$0")/argv_capture_guest.c}
BIN=/usr/local/lib/aok-argv-capture
DIR=/usr/local/bin
LOG=/var/log/aok-argv-capture.log
FLAG=/var/log/aok-argv-capture.on

# The MOTD's own command set. `command df' and the rest are resolved through
# PATH, and /usr/local/bin precedes /usr/bin on this guest, so a copy here is
# what the shell finds. sh/dash are deliberately NOT wrapped: they are on the
# path of everything, including this script.
WATCH=${WATCH:-"df awk uptime free sed"}

die() { echo "install-argv-capture: $*" >&2; exit 1; }

# /usr/local/bin has to come FIRST, or the wrapper is installed somewhere the
# shell will never look and the whole exercise records nothing. Checked rather
# than assumed, because "the instrument was never on the path" is exactly the
# kind of silent nothing that reads as "the bug did not recur".
check_path() {
    for d in $(echo "$PATH" | tr ':' ' '); do
        if [ "$d" = "$DIR" ]; then
            return 0                    # found first: the wrappers will be used
        fi
        if [ "$d" = /usr/bin ] || [ "$d" = /bin ]; then
            break                       # real commands come first: warn
        fi
    done
    echo "WARNING: $DIR does not precede /usr/bin on this PATH:" >&2
    echo "  PATH=$PATH" >&2
    echo "  The wrappers will NOT be found. Check the login shell's PATH with:" >&2
    echo "    sudo su - -c 'echo \$PATH'" >&2
}

case "${1:-status}" in
install)
    [ "$(id -u)" = 0 ] || die "must be root"
    [ -f "$SRC" ] || die "no source at $SRC (set SRC=)"
    command -v cc >/dev/null 2>&1 || command -v gcc >/dev/null 2>&1 \
        || die "no C compiler in the guest; install gcc or build it elsewhere"
    CC=$(command -v cc 2>/dev/null || command -v gcc)

    mkdir -p "$(dirname "$BIN")"
    "$CC" -Wall -Wextra -O2 -o "$BIN" "$SRC" || die "compile failed"

    # Symlinks rather than copies: one binary, and `ls -l' in /usr/local/bin
    # then says plainly that these are not the real commands.
    for c in $WATCH; do
        if [ -e "$DIR/$c" ] && [ ! -L "$DIR/$c" ]; then
            echo "skipping $c: $DIR/$c exists and is not our symlink" >&2
            continue
        fi
        ln -sf "$BIN" "$DIR/$c"
        echo "wrapped $c"
    done

    : > "$FLAG"
    # 0644, so only root's logins are recorded. That is the case under
    # investigation -- the report is a `sudo su -' MOTD, and root's login shell
    # is the native bash this is about. A non-root login will find the log
    # unwritable and, being fail-open, will simply run the command and record
    # nothing. To widen it to a normal user's logins as well, chmod 0666 the
    # log and accept a world-writable file for as long as this is installed.
    touch "$LOG"; chmod 0644 "$LOG"
    check_path
    echo "recording to $LOG"
    echo "Reproduce with an interactive login -- the MOTD only runs for one:"
    echo "    sudo su -"
    ;;
on)
    [ "$(id -u)" = 0 ] || die "must be root"
    : > "$FLAG"; echo "recording ON"
    ;;
off)
    [ "$(id -u)" = 0 ] || die "must be root"
    rm -f "$FLAG"; echo "recording OFF (wrappers still installed)"
    ;;
uninstall)
    [ "$(id -u)" = 0 ] || die "must be root"
    for c in $WATCH; do
        # Only ever remove a link that points at our own binary.
        if [ -L "$DIR/$c" ] && [ "$(readlink "$DIR/$c")" = "$BIN" ]; then
            rm -f "$DIR/$c"; echo "unwrapped $c"
        fi
    done
    rm -f "$FLAG" "$BIN"
    echo "removed; $LOG kept"
    ;;
status)
    echo "binary   : $([ -x "$BIN" ] && echo "$BIN" || echo "not installed")"
    echo "recording: $([ -f "$FLAG" ] && echo on || echo off)"
    printf 'wrapped  :'
    for c in $WATCH; do
        [ -L "$DIR/$c" ] && [ "$(readlink "$DIR/$c" 2>/dev/null)" = "$BIN" ] \
            && printf ' %s' "$c"
    done
    echo
    echo "log      : $LOG ($([ -f "$LOG" ] && wc -l < "$LOG" || echo 0) lines)"
    ;;
*)
    die "usage: $0 {install|on|off|uninstall|status}"
    ;;
esac
