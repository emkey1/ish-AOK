# companion-root.sh -- sourced, not run.
#
# A native-mode root has no distribution: no package manager, no compositor.
# It keeps a second installed filesystem for both -- its "companion", Devuan
# arm64, set up by setup-companion.sh -- while the booted root stays native.
# apt, the programs apt installs and the Wayland desktop all run out of the
# companion, under emulation. See /AOK/docs/native-companion.md.
#
# Which filesystem it is: the name in /etc/aok-companion-root, here in the
# native root. Rename the filesystem and that file is what to edit.
#
# How its programs run: a program named by its path in another root runs
# chrooted into that root (Settings -> Other Filesystems -> Programs From
# Other Roots -> Run Inside Their Root, the default), for any user, with
# /proc, /sys, /dev, /run and /AOK/native, /AOK/docs and /AOK/tools bound in
# -- and, because the companion's /etc/aok-binds asks for them, this root's
# /home and /tmp, its hosts, hostname and resolv.conf, and its time zone. A
# program started from a directory under /home or /tmp starts in that same
# directory.
#
# Builtins only: this is read by AOK-iSH's native dash and by a distribution's
# sh alike.

AOK_COMPANION_ROOT_FILE=${AOK_COMPANION_ROOT_FILE:-/etc/aok-companion-root}
AOK_COMPANION_ROOTS_DIR=${AOK_COMPANION_ROOTS_DIR:-/AOK/roots}
# The directories of the companion a command is looked for in, in order.
AOK_COMPANION_BIN_DIRS="usr/local/sbin usr/local/bin usr/sbin usr/bin sbin bin usr/games"
# What stopped a use of the companion, in words to show the user.
AOK_COMPANION_PROBLEM=
# Why: "none" (no companion recorded), "missing" (recorded, not there),
# "mode" (programs from other roots are not run inside their root).
AOK_COMPANION_PROBLEM_KIND=
AOK_COMPANION_DIR=

# True in a native-mode root. Inside the companion /etc/os-release is the
# distribution's, so a script handed off into it does not hand off again.
aok_in_native_root() {
    [ -r /etc/os-release ] || return 1
    while IFS= read -r _aok_l || [ -n "$_aok_l" ]; do
        [ "$_aok_l" = "ID=aok-native" ] && return 0
    done < /etc/os-release
    return 1
}

# The recorded name, or nothing.
aok_companion_root_name() {
    [ -r "$AOK_COMPANION_ROOT_FILE" ] || return 1
    _aok_n=
    while IFS= read -r _aok_l || [ -n "$_aok_l" ]; do
        case "$_aok_l" in ''|'#'*) continue ;; esac
        _aok_n=$_aok_l
        break
    done < "$AOK_COMPANION_ROOT_FILE"
    [ -n "$_aok_n" ] || return 1
    printf '%s\n' "$_aok_n"
}

# Sets AOK_COMPANION_DIR to the companion's directory when it is usable.
# Otherwise returns 1 with AOK_COMPANION_PROBLEM and _KIND saying why. Call it
# directly, not in $(...): it answers in variables.
aok_companion_root_find() {
    AOK_COMPANION_DIR=
    AOK_COMPANION_PROBLEM=
    AOK_COMPANION_PROBLEM_KIND=
    _aok_name=$(aok_companion_root_name) || {
        AOK_COMPANION_PROBLEM_KIND=none
        AOK_COMPANION_PROBLEM="A native filesystem has no packages of its own. Install its companion, a Devuan filesystem, with: sudo sh /AOK/tools/setup-companion.sh"
        return 1
    }
    _aok_dir="$AOK_COMPANION_ROOTS_DIR/$_aok_name"
    if [ ! -x "$_aok_dir/bin/sh" ]; then
        AOK_COMPANION_PROBLEM_KIND=missing
        AOK_COMPANION_PROBLEM="The companion filesystem '$_aok_name' is not at $_aok_dir (removed or renamed? other filesystems appear a second or two after boot). Put its name in $AOK_COMPANION_ROOT_FILE, or run: sudo sh /AOK/tools/setup-companion.sh"
        return 1
    fi
    _aok_mode=root
    if [ -r /proc/ish/foreign_exec ]; then
        IFS= read -r _aok_mode < /proc/ish/foreign_exec || _aok_mode=root
    fi
    if [ "$_aok_mode" != root ]; then
        AOK_COMPANION_PROBLEM_KIND=mode
        AOK_COMPANION_PROBLEM="The filesystem '$_aok_name' is used by running programs inside it, which needs Settings -> Other Filesystems -> Programs From Other Roots set to Run Inside Their Root (it is '$_aok_mode'). As root: echo root > /proc/ish/foreign_exec"
        return 1
    fi
    AOK_COMPANION_DIR=$_aok_dir
}

# The path of COMMAND in the companion at DIR, or nothing.
aok_companion_which() {
    for _aok_d in $AOK_COMPANION_BIN_DIRS; do
        if [ -x "$1/$_aok_d/$2" ] && [ ! -d "$1/$_aok_d/$2" ]; then
            printf '%s\n' "$1/$_aok_d/$2"
            return 0
        fi
    done
    return 1
}

# The environment a program of the companion expects: a distribution's PATH,
# with the native programs after it -- dpkg wants the sbin directories, and
# nothing of the distribution's should be shadowed.
aok_companion_env() {
    PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:/usr/games:/AOK/native
    export PATH
}
