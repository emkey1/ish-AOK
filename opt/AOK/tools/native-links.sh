#!/bin/sh
# Link iSH-AOK's native programs into a bin directory so they run natively:
# SmallCLUE's applets, and the standalone programs beside it in /AOK/native --
# zsh, dash (also linked as `sh`), helix (`hx`), ktop, motepad, the bmm/bmt
# benchmarks, and the setuid-root su, sudo and passwd. (bash, when a build
# enables it; builds from 556 on do not.)
#
# /AOK/native/smallclue is compiled into iSH-AOK and runs as host code rather
# than translated guest instructions, so it costs the same on every guest
# architecture. Like any multicall binary it picks its applet from argv[0], so
# a symlink named `wc` runs the wc applet.
#
# The standalone programs are not applets: each is its own binary, so its link
# points at its own file and argv[0] selects nothing. Which of them exist is a
# build property -- `hx` is in the app build and absent from a CLI build
# configured without -Dnative_helix -- so they are enumerated from /AOK/native
# rather than named here. See PROGRAMS_EXCLUDED for the three that are not
# commands.
#
# Use a SYMlink, never a hard link: /AOK is its own filesystem, so `ln` across
# it fails with EXDEV.
#
# Default target is /usr/local/native-bin, and this script puts that directory
# FIRST on PATH, via /etc/profile.d, unless you pass --no-path.
#
# That default was reversed deliberately, and the reasoning it reversed is still
# worth keeping in view: shadowing means every link takes precedence over the
# distro's command, and SmallCLUE's applets are SMALLER implementations rather
# than drop-in replacements. Excluding the applets that cannot work at all is not
# enough on its own, because the incompatibilities are per-flag: PSCAL's harness
# once died on `grep -q', which SmallCLUE's grep did not support at the time. No
# audit of the SOURCES finds that class, since grep is plainly present and works,
# just not with that one flag. (`grep -q' itself works now -- the shape of the
# problem is what matters, not that example.)
#
# What changed is the other half: the EXCLUDED list below is now derived from
# tools/native-applet-audit.py rather than guessed, so an applet that cannot work
# is not linked in the first place. If something still turns out to be shadowed
# badly:
#
#   sh /AOK/tools/native-links.sh --no-path   # link, but leave PATH alone
#   sh /AOK/tools/native-links.sh --remove    # take the links back out
#
# The links stay usable by full path either way:
#
#   /usr/local/native-bin/wc -l file          # one command, no PATH needed
#
# Pass /usr/local/bin explicitly if you want the links ahead of the distro's own
# /usr/local/bin entries too.
#
# Every link also goes into /usr/local/bin, whatever the directory (unless
# --target-only). profile.d only reaches login shells, and native commands are
# run from everywhere else too -- sshd's `ssh host cmd`, cron, init scripts --
# whose PATH is the distro's compiled-in one, with /usr/local/bin ahead of
# /bin. That includes `sh`, so a bare `sh` means native dash there; #!/bin/sh
# scripts are untouched, since a shebang names /bin/sh itself. It also means
# those contexts -- package maintainer scripts among them -- get SmallCLUE's
# sed, grep and awk ahead of the distro's; see the caution above, and
# --target-only if that bites. Nothing there that is not ours is replaced
# without --force.
#
# Usage:
#   sh /AOK/tools/native-links.sh [options] [directory]
#
#   --list     show what would happen, change nothing
#   --remove   remove links pointing anywhere into /AOK/native
#   --all      include applets that do not work in this build (see EXCLUDED)
#   --force    replace files that are not our own symlinks
#   --no-shell leave the UID 1000 login shell alone
#   --no-sh    link dash, but do not make `sh` mean it
#   --target-only  link into the target directory only, not /usr/local/bin too
#   --shell S  which native shell to switch to: bash, zsh or an absolute path
#   --help
#
# It also switches the UID 1000 user's login shell to a native shell, because
# that is the other half of "make this install use the native code": the links
# above only matter for commands the shell RUNS, and the shell itself is where
# a session actually spends its time -- interpretation is 16.5x faster natively
# (docs/bash_native_plan.md). --no-shell skips it, --remove puts it back.
set -eu

NATIVE=/AOK/native/smallclue
NATIVE_DIR=/AOK/native
NATIVE_BASH=/AOK/native/bash
NATIVE_ZSH=/AOK/native/zsh
NATIVE_DASH=/AOK/native/dash
# Native dash by its other name: kernel/native.c dispatches both to dash, and
# as `sh` it runs in POSIX mode the way /bin/sh would.
NATIVE_SH=/AOK/native/sh
# Where every link also goes, in addition to the target directory (see the
# header): the directory every default PATH has ahead of /bin.
ALSO_DIR=/usr/local/bin

# The standalone native programs -- everything in /AOK/native that is NOT
# SmallCLUE -- get linked too. They are whole programs rather than applets of a
# multicall binary, so each link points at its own file instead of at
# $NATIVE, and argv[0] selects nothing.
#
# Enumerated from the directory rather than listed here, for the same reason
# the applet list is read out of the binary: which ones exist is a property of
# the BUILD. helix is the case that proves it -- `hx` is in the app build and
# absent from a CLI build configured without -Dnative_helix, and a hardcoded
# name would be wrong in one of them.
#
# Not linked, and neither is a judgement about whether it works:
#   rust-probe   a diagnostic that exercises the Rust/kqueue path, not a
#                command anybody types
#   zsh-multio   an internal variant of zsh, not a second shell
#   smallclue    the multicall binary itself; its applets are linked by name
#                further down, and a `smallclue` link would just be the banner
PROGRAMS_EXCLUDED="smallclue rust-probe zsh-multio"
# Which of them becomes the login shell. Empty means "decide below", and the
# decision is resolve_native_shell's: prefer zsh when it is there, otherwise
# bash. It is written out there rather than here, so read it there -- this
# comment said the opposite of the code for a while, which is exactly what a
# second copy of a decision buys you.
#
# The ordering used to be the other way round. bash is disabled by default (it
# is GPLv3, see docs/historical/shell_transition_plan.md), so preferring it would keep handing
# new installs the shell that is going away; bash stays reachable with
# --shell bash for as long as it exists.
SHELL_WANT=
TARGET_DIR=/usr/local/native-bin
# MODE is what the run is FOR; DRY_RUN is whether it touches anything. Two
# variables rather than one, so --list composes with --remove instead of
# racing it: `--list --remove` prints what a removal would take back and takes
# nothing back. Held in one variable, the last flag simply won.
MODE=link          # link | remove
DRY_RUN=0          # --list: print, change nothing, in either mode
INCLUDE_ALL=0
FORCE=0
DO_SHELL=1
DO_PATH=1
DO_SH=1
DO_ALSO=1
# Where the previous shell is remembered so --remove can restore it. In /etc
# because that is where the thing it describes lives, and because /usr/local
# may be a different filesystem.
SHELL_STATE=/etc/aok-native-shell.prev
# Delimits the block this script owns inside a zsh startup file that may not be
# ours, so --remove takes back exactly what it added.
ZSH_MARK_BEGIN='# >>> iSH-AOK native-bin >>>'
ZSH_MARK_END='# <<< iSH-AOK native-bin <<<'
# The PATH snippet. profile.d rather than a user dotfile: it is system state
# this script owns, so --remove can delete the whole file and be sure it has
# left nothing behind, which editing someone's .bashrc could never promise.
PATH_FILE=/etc/profile.d/05-aok-native-bin.sh

# zsh NEVER reads /etc/profile or /etc/profile.d. Its global files are zshenv,
# zprofile, zshrc, zlogin -- so a user whose login shell is zsh got the links
# and never got them on PATH, and `md` (or any other applet) answered "command
# not found" with a perfectly good symlink sitting in the link directory. That
# is not a corner case: this script's own --shell zsh puts people there.
#
# Which file, per the probe AOK's native zsh actually does (deps/zsh
# Src/aok_fork.c aok_source_global): /etc/zsh/<name> when that file exists,
# otherwise the compiled-in /etc/<name>. Matched here so the snippet lands
# where zsh will look, and NEVER creating a second file that would shadow one
# the distro already ships -- the probe is per-file and takes the /etc/zsh one.
# Builtin-only, for the reason the APPLETS parser below spells out: this script
# puts native commands on PATH, so it must not then depend on which grep or awk
# that PATH resolves to.
zsh_block_present() {
    [ -f "$1" ] || return 1
    while IFS= read -r zline || [ -n "$zline" ]; do
        [ "$zline" = "$ZSH_MARK_BEGIN" ] && return 0
    done < "$1"
    return 1
}

zsh_path_file() {
    if [ -f /etc/zsh/zprofile ]; then echo /etc/zsh/zprofile
    elif [ -f /etc/zprofile ]; then echo /etc/zprofile
    elif [ -d /etc/zsh ]; then echo /etc/zsh/zprofile
    else echo /etc/zprofile
    fi
}

# Applets deliberately NOT linked, and the ones whose availability is probed:
# the lists and the measurements behind them live in native-applets.conf,
# beside this script, because the native-mode root provisioner
# (kernel/native_root.c) reads the same file. Sourced from where this script
# is, falling back to /AOK/tools, so a copy run from elsewhere still finds it.
NATIVE_APPLETS_CONF="${0%/*}/native-applets.conf"
[ -r "$NATIVE_APPLETS_CONF" ] || NATIVE_APPLETS_CONF=/AOK/tools/native-applets.conf
if [ ! -r "$NATIVE_APPLETS_CONF" ]; then
    echo "native-links.sh: cannot read native-applets.conf" >&2
    exit 1
fi
# shellcheck source=native-applets.conf
. "$NATIVE_APPLETS_CONF"

# What to run to make an applet own up. --version for almost everything, but
# not for curl and wget: they parse it as an option, print usage, and look
# perfectly healthy right up until a transfer reports that libcurl is absent.
# So they are probed with a real fetch from an address on this machine that
# nothing listens on -- no network required, instant either way, and a build
# WITH libcurl fails at connect rather than at "unavailable".
probe_args() {
    case "$1" in
        curl) echo "-o /dev/null http://127.0.0.1:1/" ;;
        wget) echo "-O /dev/null http://127.0.0.1:1/" ;;
        # OpenSSH spells it -V, and answers --version with a getopt complaint on
        # stderr. Harmless to the probe, which only looks for the stub's words,
        # but it leaked "ssh: illegal option -- r" onto the terminal of anyone
        # running this script.
        # The ssh family shares no version flag: -V is a validity interval to
        # ssh-keygen, nothing to sftp, and ssh-copy-id has none. Bare usage is
        # what the probe wants -- but NOT for ssh-keygen, which with no
        # arguments starts GENERATING A KEY and blocks on a prompt. A probe must
        # not have side effects; give it a read-only failure instead.
        ssh|scp|sftp|ssh-copy-id) echo "" ;;
        ssh-keygen) echo "-l -f /nonexistent-probe" ;;
        *) echo "--version" ;;
    esac
}

# The stubs' own words, from kernel/smallclue_glue.c and the nextvi/micro
# stubs. Matched loosely because they are diagnostics rather than an API --
# three different spellings for one idea, which is why this is a case and not
# a comparison. "not enabled in this build" is git's, and its absence here is
# why git used to be linked into a build that had no libgit2.
probe_missing() {
    # shellcheck disable=SC2046
    case "$("$NATIVE" "$1" $(probe_args "$1") 2>&1 | head -2)" in
        *"not built into this iSH-AOK"*|\
        *"unavailable in this build"*|\
        *"not enabled in this build"*) return 0 ;;
    esac
    return 1
}

# Every file in /AOK/native, so the ownership test below can recognise a link
# this script made to ANY of them, not just to SmallCLUE. Populated even in
# --remove mode: that is the mode that most needs to know what is ours.
NATIVE_ALL=
if [ -d "$NATIVE_DIR" ]; then
    for np in "$NATIVE_DIR"/*; do
        [ -f "$np" ] || continue
        NATIVE_ALL="$NATIVE_ALL ${np##*/}"
    done
fi

# True when $1 is a symlink that resolves to something in /AOK/native -- which
# is what "this script made it" means now that the links have more than one
# target. `readlink` is itself an applet this script links, so this uses -ef
# against the enumerated names and stays builtin-only.
link_is_native() {
    [ -L "$1" ] || return 1
    for _np in $NATIVE_ALL; do
        [ "$1" -ef "$NATIVE_DIR/$_np" ] && return 0
    done
    link_is_dangling_native "$1"
}

# A link into /AOK/native whose target this build no longer has -- `bash`, on
# any install linked before 556 dropped native bash. -ef cannot see it, since
# there is nothing to compare, so its text is read instead, with the distro's
# readlink (READLINK, set below; the check is skipped until it is).
link_is_dangling_native() {
    [ -L "$1" ] && [ ! -e "$1" ] && [ -n "${READLINK:-}" ] || return 1
    case "$("$READLINK" "$1" 2>/dev/null)" in
        "$NATIVE_DIR"/*) return 0 ;;
    esac
    return 1
}

# The distro's own rm, ln, mkdir, cp and mv, by absolute path. This script puts
# SmallCLUE's versions of all five first on every PATH, /usr/local/bin
# included, and a script that installs commands must not then run on them --
# the same rule the applet-list parser and the passwd code keep. SmallCLUE's rm
# once globbed its arguments, which left a `[` link that --remove could not
# take away. Not `command -p`: dash's default path has /usr/local/bin in it.
host_tool() {
    for _d in /bin /usr/bin /sbin /usr/sbin; do
        if [ -x "$_d/$1" ] && ! link_is_native "$_d/$1"; then
            printf '%s' "$_d/$1"
            return 0
        fi
    done
    printf '%s' "$1"
}
RM=$(host_tool rm)
LN=$(host_tool ln)
MKDIR=$(host_tool mkdir)
CP=$(host_tool cp)
MV=$(host_tool mv)
READLINK=$(host_tool readlink)

# Link $2 -> $1 under the same rules as the loops below: made when absent,
# left alone when already right, repointed when it is a link of ours to
# something else in /AOK/native, and anything else kept unless --force. Sets
# LINK_RESULT to linked, already or blocked for the caller's counts.
link_native_file() {
    _src=$1; _dest=$2
    if [ -L "$_dest" ] && [ "$_dest" -ef "$_src" ]; then
        LINK_RESULT=already
        return 0
    fi
    if [ -e "$_dest" ] || [ -L "$_dest" ]; then
        if ! link_is_native "$_dest" && [ "$FORCE" -eq 0 ]; then
            if [ "$DRY_RUN" -eq 1 ]; then
                echo "  would NOT replace $_dest (exists; --force to override)"
            else
                echo "  left $_dest alone (not ours; --force to replace it)"
            fi
            LINK_RESULT=blocked
            return 0
        fi
    fi
    if [ "$DRY_RUN" -eq 1 ]; then
        echo "  would link $_dest -> $_src"
    else
        [ -d "${_dest%/*}" ] || "$MKDIR" -p "${_dest%/*}"
        if ! "$LN" -sf "$_src" "$_dest"; then
            LINK_RESULT=blocked
            return 0
        fi
        echo "  linked $_dest -> $_src"
    fi
    LINK_RESULT=linked
}

# $ALSO_DIR, when it applies: it exists, it is not the target directory, and
# --target-only was not given. With an argument, prints that name inside it;
# without, the directory. Fails, printing nothing, when it does not apply.
also_dir() {
    [ "$DO_ALSO" -eq 1 ] || return 1
    [ -d "$ALSO_DIR" ] || return 1
    [ "$TARGET_DIR" -ef "$ALSO_DIR" ] && return 1
    if [ $# -gt 0 ]; then echo "$ALSO_DIR/$1"; else echo "$ALSO_DIR"; fi
}

# Inlined rather than read out of the file header with sed: `sed` is itself an
# applet this script links, so --help would break after installation. Same
# reason the applet list is parsed with builtins.
usage() {
    echo "Link iSH-AOK's native programs into a bin directory so they run"
    echo "natively: SmallCLUE's applets, plus the programs beside it in /AOK/native."
    echo
    echo "Usage: sh /AOK/tools/native-links.sh [options] [directory]"
    echo "       (defaults to /usr/local/native-bin, put first on PATH unless --no-path)"
    echo
    echo "  --list     show what would happen, change nothing"
    echo "  --remove   remove links pointing anywhere into /AOK/native"
    echo "  --all      include applets that do not work in this build"
    echo "  --force    replace files that are not our own symlinks"
    echo "  --no-shell leave the UID 1000 login shell alone"
    echo "  --no-sh    link dash, but do not make \`sh\` mean it"
    echo "  --target-only  link into the target directory only, not $ALSO_DIR too"
    echo "  --shell S  which native shell to switch to: bash, zsh, or a path"
    echo "  --no-path  do not put the link directory on PATH"
    echo "  --help"
    echo
    echo "Everything is also linked into $ALSO_DIR, which every default PATH"
    echo "has ahead of /bin -- sshd, cron and init scripts included -- unless"
    echo "--target-only is given. A bare \`sh\` then means native dash everywhere."
    echo
    echo "The UID 1000 user's login shell is switched unless --no-shell is"
    echo "given; --remove restores whatever it was before. Without --shell the"
    echo "choice is $NATIVE_BASH when present, otherwise $NATIVE_ZSH."
    echo
    echo "Applets with missing dependencies or no working implementation are"
    echo "skipped by default: linking one would shadow a working command with"
    echo "an error. Use --list to see what would change."
    exit "${1:-0}"
}

while [ $# -gt 0 ]; do
    case "$1" in
        --list) DRY_RUN=1 ;;
        --remove) MODE=remove ;;
        --all) INCLUDE_ALL=1 ;;
        --force) FORCE=1 ;;
        --no-shell) DO_SHELL=0 ;;
        --shell) [ $# -ge 2 ] || { echo "$0: --shell needs a value" >&2; exit 1; }
                 SHELL_WANT=$2; shift ;;
        --shell=*) SHELL_WANT=${1#--shell=} ;;
        --no-path) DO_PATH=0 ;;
        --no-sh) DO_SH=0 ;;
        --target-only) DO_ALSO=0 ;;
        -h|--help) usage 0 ;;
        -*) echo "$0: unknown option $1" >&2; usage 1 ;;
        *) TARGET_DIR="$1" ;;
    esac
    shift
done

if [ ! -x "$NATIVE" ]; then
    echo "$0: $NATIVE not found -- this iSH-AOK has no native SmallCLUE" >&2
    exit 1
fi

# ---------------------------------------------------------------- login shell
#
# /etc/passwd is read and rewritten with shell builtins, for the same reason
# the applet list is parsed that way: `sed` and `awk` are applets this script
# links, so after an install into a directory on PATH they are no longer the
# implementations this script was written against. A script that installs
# commands must not then depend on them.
#
# The rewrite is not atomic -- there is no builtin rename -- so the original is
# written to /etc/passwd.aok-bak FIRST. Recovering a mangled passwd matters
# more here than elsewhere: get it wrong and nobody can log in, which is
# exactly how an earlier round of this work locked a test account out.

uid1000_field() {
    # $1 = field number. Prints the field for the first UID 1000 line.
    while IFS=: read -r u p uid gid gecos home sh; do
        [ "${uid:-}" = 1000 ] || continue
        case "$1" in
            1) printf '%s' "$u" ;;
            7) printf '%s' "${sh:-}" ;;
        esac
        return 0
    done < /etc/passwd
    return 1
}

# Rewrite the UID 1000 line's shell field to $1. Every other line is passed
# through byte for byte.
set_uid1000_shell() {
    want=$1
    new=
    while IFS= read -r line || [ -n "$line" ]; do
        case "$line" in
            *:*:1000:*)
                # Re-split this one line only; the rest are untouched.
                oldifs=$IFS; IFS=:
                # shellcheck disable=SC2086
                set -f; set -- $line; set +f
                IFS=$oldifs
                if [ "$#" -ge 7 ] && [ "$3" = 1000 ]; then
                    line="$1:$2:$3:$4:$5:$6:$want"
                fi
                ;;
        esac
        new="$new$line
"
    done < /etc/passwd

    [ -n "$new" ] || { echo "$0: refusing to write an empty /etc/passwd" >&2; return 1; }
    printf '%s' "$new" > /etc/passwd.aok-new || return 1
    # Same number of lines in and out, or something went wrong and we stop.
    n_old=0; while IFS= read -r _l; do n_old=$((n_old + 1)); done < /etc/passwd
    n_new=0; while IFS= read -r _l; do n_new=$((n_new + 1)); done < /etc/passwd.aok-new
    if [ "$n_old" != "$n_new" ]; then
        echo "$0: /etc/passwd rewrite changed the line count ($n_old -> $n_new); not applying" >&2
        "$RM" -f /etc/passwd.aok-new
        return 1
    fi
    "$CP" /etc/passwd /etc/passwd.aok-bak 2>/dev/null || :
    printf '%s' "$new" > /etc/passwd || return 1
    "$RM" -f /etc/passwd.aok-new
    return 0
}

# PUTTING THE LINKS FIRST REVERSES THIS SCRIPT'S ORIGINAL DEFAULT, and the
# reason for that default is worth keeping in view rather than deleting: these
# applets are SMALLER implementations, not drop-in replacements, and the
# incompatibilities are per-flag. PSCAL's harness once died on `grep -q`, which
# SmallCLUE's grep did not support -- a failure that no audit of the sources
# finds, because grep is present and works, just not with that flag.
#
# What makes it the right default now is the other half: the EXCLUDED list is
# derived from tools/native-applet-audit.py rather than guessed, so an applet
# that cannot work is not linked in the first place. If something does turn out
# to be shadowed badly, --no-path leaves PATH alone and --remove takes it back
# out; the links themselves stay useful by full path either way.
apply_path() {
    zfile=$(zsh_path_file)
    if [ "$MODE" = remove ]; then
        if [ -f "$PATH_FILE" ]; then
            if [ "$DRY_RUN" -eq 1 ]; then
                echo "  would remove $PATH_FILE (PATH reverts at next login)"
            else
                if "$RM" -f "$PATH_FILE" 2>/dev/null && [ ! -f "$PATH_FILE" ]; then
                    echo "  removed $PATH_FILE (PATH reverts at next login)"
                else
                    echo "  could NOT remove $PATH_FILE (need root?)" >&2
                fi
            fi
        fi
        # Only ever the block this script wrote: the file may be the distro's.
        if zsh_block_present "$zfile"; then
            if [ "$DRY_RUN" -eq 1 ]; then
                echo "  would remove the PATH block from $zfile"
            else
                tmp=$zfile.aok.$$
                skip=0
                # Every step here can fail on a file this user does not own,
                # and saying so is the whole point: the message used to print
                # unconditionally, so a run that changed nothing reported
                # having removed the block. That is worse than the failure --
                # it sends you looking somewhere else for the problem.
                if { while IFS= read -r zline || [ -n "$zline" ]; do
                        case "$zline" in
                            "$ZSH_MARK_BEGIN") skip=1; continue ;;
                            "$ZSH_MARK_END")   skip=0; continue ;;
                        esac
                        [ "$skip" -eq 1 ] || printf '%s\n' "$zline"
                     done < "$zfile" > "$tmp"; } 2>/dev/null && "$MV" "$tmp" "$zfile" 2>/dev/null; then
                    # A file left empty was one this script created. Only
                    # considered once the rewrite actually landed, or a failed
                    # run could delete a file it never managed to touch.
                    [ -s "$zfile" ] || "$RM" -f "$zfile"
                    echo "  removed the PATH block from $zfile"
                else
                    "$RM" -f "$tmp" 2>/dev/null || :
                    echo "  could NOT edit $zfile (need root?); its PATH block is still there" >&2
                fi
            fi
        fi
        return 0
    fi
    if [ "$DRY_RUN" -eq 1 ]; then
        [ -f "$PATH_FILE" ] && echo "  $PATH_FILE already present" \
                            || echo "  would put $TARGET_DIR first on PATH via $PATH_FILE"
        if zsh_block_present "$zfile"; then
            echo "  $zfile already sources it (zsh)"
        else
            echo "  would make zsh read it too, via $zfile"
        fi
        return 0
    fi
    [ -d /etc/profile.d ] || { echo "  no /etc/profile.d; leaving PATH alone"; return 0; }
    # Guarded so re-logging in, or sourcing profile twice, cannot stack the
    # directory onto PATH over and over.
    cat > "$PATH_FILE" <<PATHEOF
# Added by native-links.sh. Puts iSH-AOK's native applet links ahead of the
# distro's commands, so they run as host code instead of being translated.
# Remove with: sh /AOK/tools/native-links.sh --remove
case ":\$PATH:" in
    *":$TARGET_DIR:"*) ;;
    *) PATH="$TARGET_DIR:\$PATH" ; export PATH ;;
esac
PATHEOF
    echo "  put $TARGET_DIR first on PATH via $PATH_FILE (takes effect at next login)"

    # The zsh side is a two-line shim rather than a copy of the logic above, so
    # there stays exactly one definition of what goes on PATH.
    if zsh_block_present "$zfile"; then
        echo "  $zfile already sources it (zsh)"
    else
        zdir=${zfile%/*}
        [ -d "$zdir" ] || "$MKDIR" -p "$zdir" 2>/dev/null || true
        {
            echo "$ZSH_MARK_BEGIN"
            echo "# zsh does not read /etc/profile.d; source the snippet that does."
            echo "[ -r $PATH_FILE ] && . $PATH_FILE"
            echo "$ZSH_MARK_END"
        } >> "$zfile" && echo "  made zsh read it too, via $zfile" \
                      || echo "  could not write $zfile; zsh will not see the links on PATH"
    fi
}

# Resolve SHELL_WANT to a path. A bare name picks the matching native shell; a
# path is taken as given, so an install can point at something this script has
# never heard of.
resolve_native_shell() {
    case "$SHELL_WANT" in
        bash) NATIVE_SHELL=$NATIVE_BASH ;;
        zsh)  NATIVE_SHELL=$NATIVE_ZSH ;;
        /*)   NATIVE_SHELL=$SHELL_WANT ;;
        # zsh first now, and deliberately. This used to prefer bash whenever it
        # was present, which would keep handing new installs the shell that is
        # being removed in 556. bash remains reachable with --shell bash for as
        # long as it exists.
        "")   if [ -x "$NATIVE_ZSH" ]; then NATIVE_SHELL=$NATIVE_ZSH
              else NATIVE_SHELL=$NATIVE_BASH; fi ;;
        *)    echo "$0: --shell wants bash, zsh or an absolute path" >&2; exit 1 ;;
    esac
}

# ---- native bash is going away, and a login shell that does not exist locks
# ---- the account out -------------------------------------------------------
#
# bash is GPLv3, so an App Store build cannot contain it; native bash is
# disabled by default starting in 556 (docs/historical/shell_transition_plan.md).
# Anyone whose login shell is
# /AOK/native/bash would find, on that upgrade, that their shell is simply gone
# -- which is not a degraded session, it is no session.
#
# So any passwd entry naming a native bash is converted to the GUEST's own bash,
# which is a different binary at a different path and is not going anywhere.
#
# BY PATH AND FOR EVERY ENTRY, not just uid 1000. set_uid1000_shell exists
# because that is the account this script normally manages, but whoever set a
# native shell could have set it for any account, and the one that locks out is
# whichever one they log in as.
guest_bash_path() {
    for _c in /bin/bash /usr/bin/bash /usr/local/bin/bash; do
        if [ -x "$_c" ]; then printf '%s' "$_c"; return 0; fi
    done
    # No guest bash installed. /bin/sh is the one shell that always exists and
    # always runs, and a working shell the user did not choose beats a missing
    # one they did.
    printf '%s' /bin/sh
}

is_native_bash() {
    # Both spellings: the path this script writes, and the linked one someone
    # may have set by hand.
    case "$1" in
        "$NATIVE_BASH"|"$TARGET_DIR/bash") return 0 ;;
        *) return 1 ;;
    esac
}

convert_native_bash_shells() {
    _to=$(guest_bash_path)
    _hits=0
    _new=
    while IFS= read -r line || [ -n "$line" ]; do
        case "$line" in
            *:*:*:*:*:*:*)
                oldifs=$IFS; IFS=:
                # shellcheck disable=SC2086
                set -f; set -- $line; set +f
                IFS=$oldifs
                if [ "$#" -ge 7 ] && is_native_bash "$7"; then
                    if [ "$DRY_RUN" -eq 1 ]; then
                        echo "  would convert $1's shell: $7 -> $_to"
                    else
                        echo "  converted $1's shell: $7 -> $_to"
                    fi
                    line="$1:$2:$3:$4:$5:$6:$_to"
                    _hits=$((_hits + 1))
                fi
                ;;
        esac
        _new="$_new$line
"
    done < /etc/passwd

    [ "$_hits" -gt 0 ] || return 0
    [ "$DRY_RUN" -eq 1 ] && return 0
    [ -n "$_new" ] || { echo "$0: refusing to write an empty /etc/passwd" >&2; return 1; }
    printf '%s' "$_new" > /etc/passwd.aok-new || return 1
    # Same discipline as set_uid1000_shell: a line-count change means something
    # went wrong, and a mangled passwd is the failure that locks everyone out.
    n_old=0; while IFS= read -r _l; do n_old=$((n_old + 1)); done < /etc/passwd
    n_new=0; while IFS= read -r _l; do n_new=$((n_new + 1)); done < /etc/passwd.aok-new
    if [ "$n_old" != "$n_new" ]; then
        echo "$0: /etc/passwd rewrite changed the line count ($n_old -> $n_new); not applying" >&2
        "$RM" -f /etc/passwd.aok-new
        return 1
    fi
    "$CP" /etc/passwd /etc/passwd.aok-bak 2>/dev/null || :
    printf '%s' "$_new" > /etc/passwd || return 1
    "$RM" -f /etc/passwd.aok-new
    return 0
}

apply_shell() {
    # Before anything else this function does: an entry pointing at a native
    # bash is a lockout waiting for the 556 upgrade, whatever else is asked for.
    convert_native_bash_shells
    resolve_native_shell
    user=$(uid1000_field 1) || { echo "  no UID 1000 user; leaving shells alone"; return 0; }
    cur=$(uid1000_field 7)

    if [ "$MODE" = remove ]; then
        if [ ! -f "$SHELL_STATE" ]; then
            echo "  no saved shell for $user; leaving $cur alone"
            return 0
        fi
        prev=
        while IFS= read -r l; do prev=$l; done < "$SHELL_STATE"
        [ -n "$prev" ] || { echo "  saved shell for $user is empty; leaving $cur alone"; return 0; }
        if [ "$cur" = "$prev" ]; then
            echo "  $user already uses $prev"
        elif [ "$DRY_RUN" -eq 1 ]; then
            echo "  would restore $user's shell: $cur -> $prev"
        elif set_uid1000_shell "$prev"; then
            echo "  restored $user's shell: $cur -> $prev"
        fi
        # The saved shell is what a real --remove consumes; a preview must
        # leave it behind or the run that follows has nothing to restore from.
        [ "$DRY_RUN" -eq 1 ] || "$RM" -f "$SHELL_STATE"
        return 0
    fi

    if [ "$cur" = "$NATIVE_SHELL" ]; then
        echo "  $user already uses $NATIVE_SHELL"
        return 0
    fi
    # Never point a login shell at something that will not run. This is the one
    # failure here that locks the user out rather than merely annoying them.
    if [ ! -x "$NATIVE_SHELL" ]; then
        echo "  $NATIVE_SHELL not present; leaving $user's shell as $cur" >&2
        return 0
    fi
    if [ "$DRY_RUN" -eq 1 ]; then
        echo "  would set $user's shell: $cur -> $NATIVE_SHELL"
        return 0
    fi
    if set_uid1000_shell "$NATIVE_SHELL"; then
        printf '%s\n' "$cur" > "$SHELL_STATE"
        echo "  set $user's shell: $cur -> $NATIVE_SHELL (--remove restores it)"
        # Some tools refuse a shell that is not listed here (chsh, and a few
        # ftp/mail daemons). Appending is harmless when it is already present.
        if [ -f /etc/shells ]; then
            found=0
            while IFS= read -r l; do [ "$l" = "$NATIVE_SHELL" ] && found=1; done < /etc/shells
            [ "$found" = 0 ] && printf '%s\n' "$NATIVE_SHELL" >> /etc/shells
        fi
    fi
}

is_excluded() {
    [ "$INCLUDE_ALL" -eq 1 ] && return 1
    for e in $EXCLUDED; do
        [ "$1" = "$e" ] && return 0
    done
    for e in $PROBED; do
        if [ "$1" = "$e" ]; then
            probe_missing "$1" && return 0
            return 1
        fi
    done
    return 1
}

# --remove: only ever unlink a symlink that points at the native binary, so a
# real file that happens to share a name is never touched.
if [ "$MODE" = remove ]; then
    removed=0
    failed=0
    for f in "$TARGET_DIR"/* $(also_dir >/dev/null && echo "$ALSO_DIR"/*); do
        # `readlink` is itself an applet this script may have linked, so avoid
        # it: link_is_native compares what the paths resolve to, using the
        # shell alone, and covers the standalone programs as well as SmallCLUE.
        link_is_native "$f" || continue
        if [ "$DRY_RUN" -eq 1 ]; then
            echo "  would unlink $f"
        elif "$RM" -f "$f" 2>/dev/null && [ ! -e "$f" ] && [ ! -L "$f" ]; then
            :
        else
            # Counting a removal that did not happen is how "removed 115
            # link(s)" gets printed by a run that changed nothing, which sends
            # the reader looking anywhere but here. Count what actually went.
            failed=$((failed + 1))
            continue
        fi
        removed=$((removed + 1))
    done
    if [ "$DRY_RUN" -eq 1 ]; then
        echo "would remove $removed link(s) from $TARGET_DIR$(also_dir >/dev/null && echo " and $ALSO_DIR")"
    else
        echo "removed $removed link(s) from $TARGET_DIR$(also_dir >/dev/null && echo " and $ALSO_DIR")"
        if [ "$failed" -gt 0 ]; then
            echo "  $failed link(s) could NOT be removed (need root?)" >&2
            REMOVE_FAILED=1
        fi
    fi
    [ "$DO_PATH" -eq 1 ] && apply_path
    [ "$DO_SHELL" -eq 1 ] && apply_shell
    exit "${REMOVE_FAILED:-0}"
fi

# The applet list comes from the binary, not from a list in here, so it tracks
# whatever this build actually contains.
#
# Parsed with shell builtins only. Using awk here was a bug: once this script
# had installed the links, `awk` resolved to the NATIVE awk, which reads the
# banner differently, and the second run could no longer find any applets. A
# script that installs commands onto PATH must not then depend on that PATH.
#
# A function rather than the loop written straight inside `APPLETS=$( ... )`,
# which is what it used to be. bash 3.2 -- still what macOS ships as /bin/bash,
# so still what a `bash native-links.sh` or a `sh -n` lint run on a Mac uses --
# counts parentheses naively inside $( ), takes the `)` that ends a case PATTERN
# for the one that ends the substitution, and rejects the whole file at the
# following `;;'. Nothing here is bash-specific and no guest shell has the bug
# (ash, dash, zsh and ksh all parse it), but a script nobody can lint is one
# whose next real syntax error goes unnoticed. Moving the case out of the
# substitution costs nothing and parses everywhere.
applet_list() {
    "$NATIVE" 2>&1 | while IFS= read -r line; do
        case "$line" in
            "  "[a-z[]*)
                word=${line#  }       # strip the two-space indent
                word=${word%% *}      # first field only
                [ -n "$word" ] && printf '%s ' "$word"
                ;;
        esac
    done
}
APPLETS=$(applet_list)
if [ -z "$APPLETS" ]; then
    echo "$0: could not read the applet list from $NATIVE" >&2
    exit 1
fi

linked=0; skipped=0; excluded=0; blocked=0
pruned=0
programs=0

# Every link, into one directory: the applets, then the standalone programs.
# Run for the target directory and then for $ALSO_DIR (see the header).
link_all_into() {
    LINK_DIR=$1
    [ "$DRY_RUN" -eq 1 ] || "$MKDIR" -p "$LINK_DIR"
    # Links to a native program this build no longer has point at nothing.
    for stale in "$LINK_DIR"/*; do
        link_is_dangling_native "$stale" || continue
        if [ "$DRY_RUN" -eq 1 ]; then
            echo "  would unlink $stale (its program is gone)"
        else
            "$RM" -f "$stale"
            echo "  unlinked $stale (its program is gone)"
        fi
        pruned=$((pruned + 1))
    done
    for applet in $APPLETS; do
        # `sh` is native dash's when there is one ($NATIVE_SH), not an applet's.
        if [ "$applet" = sh ] && [ -x "$NATIVE_SH" ]; then
            continue
        fi
        if is_excluded "$applet"; then
            excluded=$((excluded + 1))
            # PRUNE a link this script made before the applet was excluded. Without
            # this, exclusions only ever applied to installs that had never run the
            # script: an older version linked dmesg, dmesg was later found not to
            # work here and added to EXCLUDED, and every subsequent run skipped it
            # and left the broken link in place -- shadowing the distro's dmesg,
            # which works. Reported on an install whose dmesg link was made on
            # 2026-08-16.
            #
            # Same ownership test --remove uses: only ever unlink a symlink that
            # resolves to the native binary, so a real file of the same name, or
            # somebody else's link, is never touched.
            stale="$LINK_DIR/$applet"
            if [ -L "$stale" ] && [ "$stale" -ef "$NATIVE" ]; then
                if [ "$DRY_RUN" -eq 1 ]; then
                    echo "  would unlink $stale (now excluded)"
                else
                    "$RM" -f "$stale"
                    echo "  unlinked $stale (now excluded)"
                fi
                pruned=$((pruned + 1))
            fi
            continue
        fi
        dest="$LINK_DIR/$applet"

        if [ -L "$dest" ] && [ "$dest" -ef "$NATIVE" ]; then
            skipped=$((skipped + 1))   # already ours; idempotent
            continue
        fi
        if [ -e "$dest" ] || [ -L "$dest" ]; then
            if [ "$FORCE" -eq 0 ]; then
                [ "$DRY_RUN" -eq 1 ] && echo "  would NOT replace $dest (exists; --force to override)"
                blocked=$((blocked + 1))
                continue
            fi
        fi

        if [ "$DRY_RUN" -eq 1 ]; then
            echo "  would link $dest -> $NATIVE"
        else
            "$LN" -sf "$NATIVE" "$dest"
        fi
        linked=$((linked + 1))
    done

    # The standalone programs. Same rules as the applets -- never replace
    # something that is not ours without --force, idempotent when the link is
    # already right -- but each points at its own file rather than at $NATIVE.
    for prog in $NATIVE_ALL; do
        skip=0
        for e in $PROGRAMS_EXCLUDED; do
            [ "$prog" = "$e" ] && skip=1
        done
        [ "$prog" = sh ] && [ "$DO_SH" -eq 0 ] && skip=1
        [ "$skip" -eq 1 ] && continue

        src="$NATIVE_DIR/$prog"
        dest="$LINK_DIR/$prog"

        if [ -L "$dest" ] && [ "$dest" -ef "$src" ]; then
            skipped=$((skipped + 1))   # already ours and already right
            continue
        fi
        # A link of ours pointing somewhere else in /AOK/native is ours to correct
        # -- an applet link left by an older run whose name a program has since
        # taken, say -- and is repointed without needing --force.
        if [ -e "$dest" ] || [ -L "$dest" ]; then
            if ! link_is_native "$dest" && [ "$FORCE" -eq 0 ]; then
                [ "$DRY_RUN" -eq 1 ] && echo "  would NOT replace $dest (exists; --force to override)"
                blocked=$((blocked + 1))
                continue
            fi
        fi

        if [ "$DRY_RUN" -eq 1 ]; then
            echo "  would link $dest -> $src"
        else
            "$LN" -sf "$src" "$dest"
        fi
        programs=$((programs + 1))
    done
}

link_all_into "$TARGET_DIR"
also_dir >/dev/null && link_all_into "$ALSO_DIR"

# --no-sh takes back an `sh` a previous run made to mean native dash; one that
# is somebody else's is not ours to touch.
if [ "$DO_SH" -eq 0 ]; then
    for d in "$TARGET_DIR/sh" $(also_dir sh); do
        if [ -L "$d" ] && { [ "$d" -ef "$NATIVE_SH" ] || [ "$d" -ef "$NATIVE_DASH" ]; }; then
            if [ "$DRY_RUN" -eq 1 ]; then
                echo "  would unlink $d (--no-sh)"
            else
                "$RM" -f "$d" && echo "  unlinked $d (--no-sh)"
            fi
        fi
    done
fi

where="$TARGET_DIR"
also_dir >/dev/null && where="$TARGET_DIR and $ALSO_DIR"
if [ "$DRY_RUN" -eq 1 ]; then
    echo "would link $linked applet(s) and $programs program(s) into $where, leave $blocked in place, skip $excluded excluded, $skipped already linked, unlink $pruned now-excluded"
else
    echo "linked $linked applet(s) and $programs program(s) into $where ($skipped already, $blocked left in place, $excluded excluded, $pruned stale removed)"
    [ "$blocked" -gt 0 ] && echo "  $blocked existing command(s) left alone; --force to replace, --list to see them"
fi

# PATH and the login shell last, so a failure in either cannot leave the links
# half-done -- and so their messages are the ones still on screen, since they
# are the changes that affect the next login rather than the next command.
[ "$DO_PATH" -eq 1 ] && apply_path
[ "$DO_SHELL" -eq 1 ] && apply_shell
exit 0
