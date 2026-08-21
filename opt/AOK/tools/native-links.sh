#!/bin/sh
# Link iSH-AOK's native programs into a bin directory so they run natively:
# SmallCLUE's applets, and the standalone programs beside it in /AOK/native --
# helix (`hx`), bash and zsh.
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
# Usage:
#   sh /AOK/tools/native-links.sh [options] [directory]
#
#   --list     show what would happen, change nothing
#   --remove   remove links pointing anywhere into /AOK/native
#   --all      include applets that do not work in this build (see EXCLUDED)
#   --force    replace files that are not our own symlinks
#   --no-shell leave the UID 1000 login shell alone
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
# Which of them becomes the login shell. Empty means "decide below": prefer bash
# when it is there, otherwise zsh. That ordering keeps this script doing exactly
# what it always did on a build that HAS bash -- which is the default build, since
# -Dnative_bash is `auto' and resolves to ON whenever deps/bash is checked out --
# while making a build configured with -Dnative_bash=disabled, where
# /AOK/native/bash does not exist at all because linking it would put GPLv3 in the
# binary, switch to zsh instead of silently switching nothing.
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

# Applets deliberately NOT linked. Linking a broken one is worse than leaving
# it alone: the distro's working command gets shadowed by one that errors, and
# the failure surfaces somewhere unrelated.
#
# THIS LIST WENT BADLY STALE ONCE. It was written when much of the native libc
# shim was missing, and kept entries long after the shim grew spawn, signals,
# wait and job control -- 52 entries where a measurement found a handful.
#
# tools/native-applet-audit.py, which walks SmallCLUE's sources for calls the
# shim answers with ENOSYS, is NOT the authority here and has drifted further
# from one: it flags find, rm, time, timeout, watch, xargs, init and runit, and
# every one of those was then measured working. The reason is structural -- it
# follows helpers transitively and cannot see #if defined(PSCAL_TARGET_IOS), so
# the one fork() left in the tree (inside an iOS-only watch helper) taints
# everything that can reach the applet table. It also cleared `env`, whose
# inability to exec once took PSCAL's harness from 217 passing to zero. Treat
# it as a hint about where to LOOK, never as an answer.
#
# So the entries below are what was MEASURED by running each applet, not what
# the audit predicted. Re-measure rather than re-reason when this is revisited.
#
#   broken here      script needs the PSCAL app's terminal-capture hooks and
#                    creates no pty of its own; mount, umount and passwd have
#                    their real bodies inside #if defined(__linux__), and a
#                    native program is compiled for the HOST; vproc-test says
#                    it is iOS-only; version reports the embedding app's
#                    marketing version, which AOK has none of
#   loops or blocks  init, runit, watch all WORK -- init runs /etc/rc and reaps,
#                    runit starts its services, watch repeats -- and that is
#                    exactly why they are not linked: two are supervisors that
#                    never return and the third repeats until interrupted
#   misleading       halt, poweroff and reboot print "System halt requested"
#                    and return 0, having halted nothing: their body is an
#                    exit(0), and a native program's exit is a return into the
#                    kernel, not the end of anything. Shadowing sysvinit's
#                    halt with a no-op that reports success is the worst kind
#                    of entry to link
#   system state     mknod, mdev, chroot, su, sudo -- these work, and
#                    shadowing them is still all risk: this sudo runs the
#                    command with no authentication at all
#   not commands     smallclue, smallclue-help, licenses
#   pagers           less and more. `apt search maria` wedged the app every
#                    time, and removing the less symlink cured it -- confirmed
#                    on device. The two backtraces agree on the shape: apt
#                    blocked reading four bytes from a descriptor, and
#                    SmallCLUE's less blocked in pagerCollectLines reading
#                    stdin that never reaches EOF. It spools the WHOLE stream
#                    to a temp file before drawing anything, where real less
#                    paints the first screen at once, so a producer that waits
#                    on its pager before closing waits forever.
#
#                    Excluded rather than fixed because the exact trigger is
#                    not characterised yet: forcing PAGER at SmallCLUE's less
#                    by hand COMPLETED, so it is something about how apt
#                    invokes a pager it found on PATH, not collect-all alone.
#                    Until that is understood, a pager that can hang the app
#                    has no business shadowing the distro's. `more` goes with
#                    it -- same code path, same risk, and nothing has proved it
#                    safe either.
#
# Three entries left this list after being fixed rather than reclassified, which
# is the outcome to aim for: ipaddr (the shim's getifaddrs is real now -- the
# host's interfaces ARE the guest's, and /proc/net/dev was already built from
# them), kill (which now takes -0, -s SIG and a signal by name or number), and
# dmesg -- whose __linux__ test was answering the wrong question, since AOK's
# guest IS Linux and now answers klogctl through the shim.
#
# Absent from this list on purpose, because they are handled by PROBED below
# rather than hardcoded: everything whose availability depends on what this
# particular build has compiled in.
EXCLUDED="chroot halt init less licenses mdev mknod more mount passwd
poweroff reboot runit script smallclue smallclue-help su sudo umount
version vproc-test watch"

# Availability-gated applets: present in every build, working only in some.
# These are the ones that made the list stale, because whether they work is a
# property of the BUILD rather than of the applet -- ssh needs the vendored
# OpenSSH tree, tar and gzip need zlib, git needs libgit2, curl and wget need
# libcurl, micro and vi need their embedded editors.
#
# So they are not guessed at: each is run once, and skipped only if it reports
# that it is not in this build. A build that gains ssh starts linking ssh with
# no edit here, which is the property this list was missing.
#
# md5sum/sha1sum/sha256sum are still probed although they now work everywhere
# AOK builds: they are compiled only where CommonCrypto exists, which is the
# same kind of build-time fact as the rest of this list.
PROBED="ssh scp sftp ssh-keygen ssh-copy-id rsync git tar gzip gunzip zcat
md5sum sha1sum sha256sum micro vi nextvi curl wget"

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
    return 1
}

# Inlined rather than read out of the file header with sed: `sed` is itself an
# applet this script links, so --help would break after installation. Same
# reason the applet list is parsed with builtins.
usage() {
    echo "Link iSH-AOK's native programs into a bin directory so they run"
    echo "natively: SmallCLUE's applets, plus hx, bash and zsh from /AOK/native."
    echo
    echo "Usage: sh /AOK/tools/native-links.sh [options] [directory]"
    echo "       (defaults to /usr/local/native-bin, put first on PATH unless --no-path)"
    echo
    echo "  --list     show what would happen, change nothing"
    echo "  --remove   remove links pointing anywhere into /AOK/native"
    echo "  --all      include applets that do not work in this build"
    echo "  --force    replace files that are not our own symlinks"
    echo "  --no-shell leave the UID 1000 login shell alone"
    echo "  --shell S  which native shell to switch to: bash, zsh, or a path"
    echo "  --no-path  do not put the link directory on PATH"
    echo "  --help"
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
        rm -f /etc/passwd.aok-new
        return 1
    fi
    cp /etc/passwd /etc/passwd.aok-bak 2>/dev/null || :
    printf '%s' "$new" > /etc/passwd || return 1
    rm -f /etc/passwd.aok-new
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
                if rm -f "$PATH_FILE" 2>/dev/null && [ ! -f "$PATH_FILE" ]; then
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
                     done < "$zfile" > "$tmp"; } 2>/dev/null && mv "$tmp" "$zfile" 2>/dev/null; then
                    # A file left empty was one this script created. Only
                    # considered once the rewrite actually landed, or a failed
                    # run could delete a file it never managed to touch.
                    [ -s "$zfile" ] || rm -f "$zfile"
                    echo "  removed the PATH block from $zfile"
                else
                    rm -f "$tmp" 2>/dev/null || :
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
        [ -d "$zdir" ] || mkdir -p "$zdir" 2>/dev/null || true
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
        "")   if [ -x "$NATIVE_BASH" ]; then NATIVE_SHELL=$NATIVE_BASH
              else NATIVE_SHELL=$NATIVE_ZSH; fi ;;
        *)    echo "$0: --shell wants bash, zsh or an absolute path" >&2; exit 1 ;;
    esac
}

apply_shell() {
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
        [ "$DRY_RUN" -eq 1 ] || rm -f "$SHELL_STATE"
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
    for f in "$TARGET_DIR"/*; do
        # `readlink` is itself an applet this script may have linked, so avoid
        # it: link_is_native compares what the paths resolve to, using the
        # shell alone, and covers the standalone programs as well as SmallCLUE.
        link_is_native "$f" || continue
        if [ "$DRY_RUN" -eq 1 ]; then
            echo "  would unlink $f"
        elif rm -f "$f" 2>/dev/null && [ ! -e "$f" ] && [ ! -L "$f" ]; then
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
        echo "would remove $removed link(s) from $TARGET_DIR"
    else
        echo "removed $removed link(s) from $TARGET_DIR"
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

[ "$DRY_RUN" -eq 1 ] || mkdir -p "$TARGET_DIR"

linked=0; skipped=0; excluded=0; blocked=0
pruned=0
for applet in $APPLETS; do
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
        stale="$TARGET_DIR/$applet"
        if [ -L "$stale" ] && [ "$stale" -ef "$NATIVE" ]; then
            if [ "$DRY_RUN" -eq 1 ]; then
                echo "  would unlink $stale (now excluded)"
            else
                rm -f "$stale"
                echo "  unlinked $stale (now excluded)"
            fi
            pruned=$((pruned + 1))
        fi
        continue
    fi
    dest="$TARGET_DIR/$applet"

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
        ln -sf "$NATIVE" "$dest"
    fi
    linked=$((linked + 1))
done

# The standalone programs. Same rules as the applets -- never replace
# something that is not ours without --force, idempotent when the link is
# already right -- but each points at its own file rather than at $NATIVE.
programs=0
for prog in $NATIVE_ALL; do
    skip=0
    for e in $PROGRAMS_EXCLUDED; do
        [ "$prog" = "$e" ] && skip=1
    done
    [ "$skip" -eq 1 ] && continue

    src="$NATIVE_DIR/$prog"
    dest="$TARGET_DIR/$prog"

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
        ln -sf "$src" "$dest"
    fi
    programs=$((programs + 1))
done

if [ "$DRY_RUN" -eq 1 ]; then
    echo "would link $linked applet(s) and $programs program(s), leave $blocked in place, skip $excluded excluded, $skipped already linked, unlink $pruned now-excluded"
else
    echo "linked $linked applet(s) and $programs program(s) into $TARGET_DIR ($skipped already, $blocked left in place, $excluded excluded, $pruned stale removed)"
    [ "$blocked" -gt 0 ] && echo "  $blocked existing command(s) left alone; --force to replace, --list to see them"
fi

# PATH and the login shell last, so a failure in either cannot leave the links
# half-done -- and so their messages are the ones still on screen, since they
# are the changes that affect the next login rather than the next command.
[ "$DO_PATH" -eq 1 ] && apply_path
[ "$DO_SHELL" -eq 1 ] && apply_shell
exit 0
