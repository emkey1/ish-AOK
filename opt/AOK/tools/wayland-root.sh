# wayland-root.sh -- sourced by the desktop scripts, not run.
#
# A native-mode root has no distribution, so no compositor. Its desktop runs
# out of a second installed root, the "Wayland root" -- Devuan arm64, set up by
# setup-native-wayland.sh -- while the booted root stays native. Every desktop
# script starts by calling aok_wayland_handoff, which does nothing on a
# distribution root and, in a native root, runs the same script again inside
# the Wayland root:
#
#     /AOK/roots/<name>/bin/sh /AOK/tools/<script> ...
#
# A program named by its path in another root runs chrooted into that root
# (Settings -> Other Filesystems -> Programs From Other Roots -> Run Inside
# Their Root, the default), for any user, with /proc, /sys, /dev, /run and
# /AOK/native, /AOK/docs and /AOK/tools bound in -- and, because the Wayland
# root's /etc/aok-binds asks for them, this root's /home and /tmp. So the
# session inside sees your files, and the app sees the session's
# /tmp/ish-display.ready.
#
# Which root it is: the name in /etc/aok-wayland-root, here in the native
# root. Rename the root and that file is what to edit.
#
# Builtins only until the hand-off: this is read by AOK-iSH's native dash and
# by a distribution's sh alike.

AOK_WAYLAND_ROOT_FILE=${AOK_WAYLAND_ROOT_FILE:-/etc/aok-wayland-root}
AOK_WAYLAND_ROOTS_DIR=${AOK_WAYLAND_ROOTS_DIR:-/AOK/roots}
# What stopped a hand-off, for the caller to show.
AOK_WAYLAND_PROBLEM=

# True in a native-mode root. Inside the Wayland root /etc/os-release is the
# distribution's, so a script that has been handed off does not hand off again.
aok_in_native_root() {
    [ -r /etc/os-release ] || return 1
    while IFS= read -r _aok_l || [ -n "$_aok_l" ]; do
        [ "$_aok_l" = "ID=aok-native" ] && return 0
    done < /etc/os-release
    return 1
}

# The recorded name, or nothing.
aok_wayland_root_name() {
    [ -r "$AOK_WAYLAND_ROOT_FILE" ] || return 1
    _aok_n=
    while IFS= read -r _aok_l || [ -n "$_aok_l" ]; do
        case "$_aok_l" in ''|'#'*) continue ;; esac
        _aok_n=$_aok_l
        break
    done < "$AOK_WAYLAND_ROOT_FILE"
    [ -n "$_aok_n" ] || return 1
    printf '%s\n' "$_aok_n"
}

# Sets AOK_WAYLAND_DIR to the Wayland root's directory when it is usable.
# Otherwise returns 1 with AOK_WAYLAND_PROBLEM saying why, in words to show
# the user. Call it directly, not in $(...): it answers in variables.
AOK_WAYLAND_DIR=
aok_wayland_root_find() {
    AOK_WAYLAND_DIR=
    AOK_WAYLAND_PROBLEM=
    _aok_name=$(aok_wayland_root_name) || {
        AOK_WAYLAND_PROBLEM="Native mode has no desktop of its own. Install one into a second filesystem with: sudo sh /AOK/tools/setup-native-wayland.sh  (or Settings -> Wayland Desktop)"
        return 1
    }
    _aok_dir="$AOK_WAYLAND_ROOTS_DIR/$_aok_name"
    if [ ! -x "$_aok_dir/bin/sh" ]; then
        AOK_WAYLAND_PROBLEM="The Wayland filesystem '$_aok_name' is not at $_aok_dir (removed or renamed? other filesystems appear a second or two after boot). Put its name in $AOK_WAYLAND_ROOT_FILE, or run: sudo sh /AOK/tools/setup-native-wayland.sh"
        return 1
    fi
    _aok_mode=root
    if [ -r /proc/ish/foreign_exec ]; then
        IFS= read -r _aok_mode < /proc/ish/foreign_exec || _aok_mode=root
    fi
    if [ "$_aok_mode" != root ]; then
        AOK_WAYLAND_PROBLEM="The desktop runs inside the filesystem '$_aok_name', which needs Settings -> Other Filesystems -> Programs From Other Roots set to Run Inside Their Root (it is '$_aok_mode'). As root: echo root > /proc/ish/foreign_exec"
        return 1
    fi
    AOK_WAYLAND_DIR=$_aok_dir
}

# As root, keep the Wayland root's resolv.conf the booted root's. On a device
# its first server is AOK-iSH's relay (127.0.0.53), which follows the network
# and answers in every root, so a copy stays right; this covers the rest.
aok_wayland_refresh() {
    [ "$(id -u)" = 0 ] || return 0
    [ -s /etc/resolv.conf ] || return 0
    cmp -s /etc/resolv.conf "$1/etc/resolv.conf" 2>/dev/null && return 0
    rm -f "$1/etc/resolv.conf" 2>/dev/null
    cat /etc/resolv.conf > "$1/etc/resolv.conf" 2>/dev/null || true
}

# Run SCRIPT (a name in /AOK/tools) inside DIR with the arguments that follow.
# Does not return. PATH is a distribution's, with the native programs after it:
# dpkg wants the sbin directories, and nothing here should be shadowed.
aok_wayland_run_in() {
    _aok_dir=$1 _aok_script=$2
    shift 2
    aok_wayland_refresh "$_aok_dir"
    PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:/AOK/native
    export PATH
    # Tells the script it was entered from a native root: the account's shell
    # is the one setup-native-wayland.sh wrote, and the chroot left it at /.
    AOK_WAYLAND_FROM_NATIVE=1
    export AOK_WAYLAND_FROM_NATIVE
    exec "$_aok_dir/bin/sh" "/AOK/tools/$_aok_script" "$@"
}

# aok_wayland_handoff SCRIPT POLICY ARGS...
#
# On a distribution root: returns 0, and the caller carries on as it always
# did. In a native root with a usable Wayland root: runs SCRIPT inside it and
# does not return. With none:
#   POLICY=install  installs one first (setup-native-wayland.sh, which also
#                   runs setup-wayland.sh inside it), then runs SCRIPT there;
#   POLICY=report   returns 1, AOK_WAYLAND_PROBLEM saying what to do.
aok_wayland_handoff() {
    aok_in_native_root || return 0
    _aok_script=$1 _aok_policy=$2
    shift 2
    if aok_wayland_root_find; then
        aok_wayland_run_in "$AOK_WAYLAND_DIR" "$_aok_script" "$@"
    fi
    [ "$_aok_policy" = install ] || return 1
    # Only "there is none yet" is something an install fixes.
    aok_wayland_root_name >/dev/null 2>&1 && return 1
    sh /AOK/tools/setup-native-wayland.sh || exit $?
    [ "$_aok_script" = setup-wayland.sh ] && exit 0
    aok_wayland_root_find || return 1
    aok_wayland_run_in "$AOK_WAYLAND_DIR" "$_aok_script" "$@"
}
