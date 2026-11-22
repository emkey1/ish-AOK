# wayland-root.sh -- sourced by the desktop scripts, not run.
#
# A native-mode root has no compositor. Its desktop runs out of its companion
# filesystem (companion-root.sh, setup-companion.sh), while the booted root
# stays native. Every desktop script starts by calling aok_wayland_handoff,
# which does nothing on a distribution root and, in a native root, runs the
# same script again inside the companion:
#
#     /AOK/roots/<name>/bin/sh /AOK/tools/<script> ...
#
# The companion shares this root's /home and /tmp, so the session inside sees
# your files, and the app sees the session's /tmp/ish-display.ready.
#
# Builtins only until the hand-off: this is read by AOK-iSH's native dash and
# by a distribution's sh alike.

. /AOK/tools/companion-root.sh

# What stopped a hand-off, for the caller to show.
AOK_WAYLAND_PROBLEM=
AOK_WAYLAND_DIR=

# Sets AOK_WAYLAND_DIR to the companion's directory when it is usable.
# Otherwise returns 1 with AOK_WAYLAND_PROBLEM saying why, in words to show
# the user. Call it directly, not in $(...): it answers in variables.
aok_wayland_root_find() {
    AOK_WAYLAND_DIR=
    AOK_WAYLAND_PROBLEM=
    if aok_companion_root_find; then
        AOK_WAYLAND_DIR=$AOK_COMPANION_DIR
        return 0
    fi
    AOK_WAYLAND_PROBLEM=$AOK_COMPANION_PROBLEM
    [ "$AOK_COMPANION_PROBLEM_KIND" != none ] ||
        AOK_WAYLAND_PROBLEM="Native mode has no desktop of its own. Install one into its companion filesystem with: sudo sh /AOK/tools/setup-native-wayland.sh  (or Settings -> Wayland Desktop)"
    return 1
}

# Run SCRIPT (a name in /AOK/tools) inside DIR with the arguments that follow.
# Does not return.
aok_wayland_run_in() {
    _aok_dir=$1 _aok_script=$2
    shift 2
    aok_companion_env
    # Tells the script it was entered from a native root: the account's shell
    # is the one setup-companion.sh wrote, and the session starts at home.
    AOK_WAYLAND_FROM_NATIVE=1
    export AOK_WAYLAND_FROM_NATIVE
    exec "$_aok_dir/bin/sh" "/AOK/tools/$_aok_script" "$@"
}

# aok_wayland_handoff SCRIPT POLICY ARGS...
#
# On a distribution root: returns 0, and the caller carries on as it always
# did. In a native root with a usable companion: runs SCRIPT inside it and
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
    [ "$AOK_COMPANION_PROBLEM_KIND" = none ] || return 1
    sh /AOK/tools/setup-native-wayland.sh || exit $?
    [ "$_aok_script" = setup-wayland.sh ] && exit 0
    aok_wayland_root_find || return 1
    aok_wayland_run_in "$AOK_WAYLAND_DIR" "$_aok_script" "$@"
}
