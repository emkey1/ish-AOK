#!/bin/sh
# start-wayland.sh
# ---------------------------------------------------------------------------
# Launch the headless Wayland desktop for the Workspace "Display" applet:
# labwc (compositor) + foot (first terminal) + wayvnc (VNC bridge target).
# Runs in the foreground as the session leader of a pseudo-terminal owned by
# DisplayViewController (the same pty-backed session mechanism
# TerminalViewController uses, minus the visible terminal UI); closing the
# applet hangs up that pty, which delivers SIGHUP here. TERM/INT are also
# trapped for when something signals this script directly (e.g. testing it
# by hand from a shell). Requires labwc, foot, wayvnc on PATH -- run
# /AOK/tools/setup-wayland.sh once first if they're missing.
#
# Env overrides (all optional):
#   WAYVNC_PORT       TCP port wayvnc listens on (default 5901; 5900 is
#                      commonly taken by other VNC/screen-sharing services)
#   WAYLAND_COMPOSITOR_CMD
#                      compositor invocation, overriding the desktop chosen
#                      in /etc/aok-desktop.conf (select-desktop.sh: labwc,
#                      the default; sway; wayfire, setup-wayfire.sh; xfce,
#                      setup-xfce.sh -- Xfce's session with labwc). cage is
#                      NOT supported here -- its virtual-keyboard keycodes
#                      are off by evdev's +8 offset and foot silently drops
#                      every key (see docs/historical/wayland_workspace_plan.md phase 0).
#                      labwc is a floating/stacking compositor (normal
#                      desktop-style windows, right-click menu) -- tried
#                      "sway" (tiling, keybinding-driven) for a while
#                      instead, since labwc's menu had been unreliable, but
#                      that turned out to be a real kernel signal/poll
#                      deadlock the menu's fork/exit pattern triggered (see
#                      project memory project_signalfd_sighand_poll_
#                      deadlock.md), now fixed -- and sway's tiling-by-
#                      default layout (every window fullscreen until you
#                      float/resize it) wasn't the desktop feel this applet
#                      wants anyway. "sway" still works as an alternative if
#                      you prefer it; both get installed by setup-wayland.sh.
#   ISH_DISPLAY_READY_FILE
#                      path this script writes "$WAYVNC_PORT\n" to once
#                      wayvnc is confirmed listening (default
#                      /tmp/ish-display.ready). DisplayViewController polls
#                      for this rather than trying to parse this script's
#                      pty-carried stdout, which goes through the terminal
#                      emulator's escape-sequence parser, not a plain pipe.
#                      Removed on exit and (defensively) at startup, in case
#                      a previous run was killed hard enough to skip cleanup.
#                      On any die() below, "$ISH_DISPLAY_READY_FILE.error" is
#                      written instead with the failure reason, so
#                      DisplayViewController's poll can surface the actual
#                      cause (compositor/foot/wayvnc crash, timeout, etc.)
#                      immediately instead of only ever reporting its own
#                      generic "timed out waiting" after the full deadline.
#
# The headless output starts at wlroots' default size (1280x720). Once the
# applet's native RFB client (DisplayRFBClient/DisplayRFBView) connects, it asks
# for a desktop the size of the surface showing it, and again whenever that
# surface is resized (RFB SetDesktopSize, which wayvnc forwards to the headless
# output). A server that refuses keeps 1280x720, scaled to fit. This script
# makes wayvnc refuse when its neatvnc is older than 0.9.2, which crashes when
# the desktop shrinks under a connected client (see the wayvnc launch below).
#
# A labwc setup gets seeded under $HOME/.config/labwc/ and
# $HOME/.local/share/themes/ (see below): a right-click root menu (New
# Terminal, Launcher, an Applications submenu grouping whatever is installed
# by category, Reconfigure, Exit), a handful of Alt-based keybindings for the
# same actions (DisplayRFBView.m forwards Alt/Alt+Shift+<letter> and Alt+Return
# as their own UIKeyCommands for this -- Control stays free for in-terminal
# Ctrl combos), and a themerc styled after the app's own Workspace "Graphite"
# palette (dark background, mint-accented active window/menu highlights)
# instead of labwc's plain stock look. The menu and keybindings are written
# when missing, or over an unedited default from an earlier version; a file
# the user has changed is kept.
# ---------------------------------------------------------------------------
set -u

log()  { printf '\n\033[1;36m==>\033[0m \033[1m%s\033[0m\n' "$*"; }
# Failure evidence written only under /tmp evaporates before anyone can read
# it: the next session's startup rm -f below wipes $ERROR_FILE, and a guest
# reboot wipes /tmp wholesale. die() therefore also appends the reason (plus
# the tail of $DEBUG_LOG when one exists -- that's where the compositor's own
# output lands) to a persistent per-user log, so a post-mortem from any
# terminal stays possible no matter how many sessions or reboots later.
PERSIST_LOG_DIR="/var/log"
[ -d "$PERSIST_LOG_DIR" ] && [ -w "$PERSIST_LOG_DIR" ] || PERSIST_LOG_DIR="${HOME:-/tmp}"
PERSIST_LOG="$PERSIST_LOG_DIR/ish-wayland-error.log"
die()  {
    printf 'start-wayland.sh: %s\n' "$*" >&2
    printf '%s\n' "$*" > "$ERROR_FILE" 2>/dev/null
    {
        printf '==== %s ====\n' "$(date 2>/dev/null || echo unknown-time)"
        printf '%s\n' "$*"
        if [ -n "${DEBUG_LOG:-}" ] && [ -s "$DEBUG_LOG" ]; then
            printf -- '-- last compositor/wayvnc output (%s) --\n' "$DEBUG_LOG"
            tail -n 40 "$DEBUG_LOG" 2>/dev/null
        fi
    } >> "$PERSIST_LOG" 2>/dev/null
    printf 'start-wayland.sh: failure appended to %s\n' "$PERSIST_LOG" >&2
    exit 1
}

# Whether Font Awesome is actually available to fontconfig. waybar's modules
# draw their icons from its private-use codepoints, so without it every icon is
# an empty box with the codepoint printed in it -- which is what a user saw on a
# panel reading "11% [F2DB]  13% [F059]  85% [F0A0]". setup-wayland.sh installs
# the font best-effort, and only waybar's OWN absence was ever checked, so a
# failed font install produced a panel full of boxes and said nothing at all.
aok_font_awesome_present() {
    if command -v fc-list >/dev/null 2>&1; then
        fc-list 2>/dev/null | grep -qi 'font *awesome'
    else
        # No fontconfig tools: look for the files the packages drop instead.
        find /usr/share/fonts /usr/local/share/fonts "$HOME/.local/share/fonts" \
            -iname '*awesome*' 2>/dev/null | grep -q .
    fi
}

# The package that carries it, named per distro so the advice is actionable.
aok_font_awesome_package() {
    if command -v apt-get >/dev/null 2>&1; then printf 'fonts-font-awesome'
    elif command -v pacman >/dev/null 2>&1; then printf 'otf-font-awesome'
    elif command -v apk >/dev/null 2>&1; then printf 'font-awesome'
    else printf 'the Font Awesome font'
    fi
}

WAYVNC_PORT="${WAYVNC_PORT:-5901}"
# Which desktop: labwc, sway, wayfire or xfce, as /etc/aok-desktop.conf says
# (select-desktop.sh writes it; setup-wayfire.sh and setup-xfce.sh choose
# theirs). WAYLAND_COMPOSITOR_CMD, the older knob, still wins, and its first
# word names the desktop. COMPOSITOR_BIN is the program that runs as the
# compositor -- labwc, under Xfce -- and COMPOSITOR_CMD its whole command,
# which for wayfire and xfce names files under $HOME and is completed once HOME
# is settled (below).
AOK_DESKTOP=labwc
if [ -n "${WAYLAND_COMPOSITOR_CMD:-}" ]; then
    AOK_DESKTOP=${WAYLAND_COMPOSITOR_CMD%% *}
else
    _wl_choice=$(sed -n 's/^AOK_DESKTOP=//p' /etc/aok-desktop.conf 2>/dev/null | tail -n 1)
    case "$_wl_choice" in labwc|sway|wayfire|xfce) AOK_DESKTOP=$_wl_choice ;; esac
fi
# Wayfire's panel: wf-shell's own wf-panel, or waybar with wofi as its launcher
# (select-desktop.sh --panel; setup-wayfire.sh offers both).
AOK_WAYFIRE_PANEL=wf-panel
case "$(sed -n 's/^AOK_WAYFIRE_PANEL=//p' /etc/aok-desktop.conf 2>/dev/null | tail -n 1)" in
    waybar) AOK_WAYFIRE_PANEL=waybar ;;
esac
case "$AOK_DESKTOP" in
    xfce) COMPOSITOR_BIN=labwc DESKTOP_BINS="labwc xfce4-session" DESKTOP_SETUP=setup-xfce.sh ;;
    wayfire) COMPOSITOR_BIN=wayfire DESKTOP_BINS=wayfire DESKTOP_SETUP=setup-wayfire.sh ;;
    *) COMPOSITOR_BIN=$AOK_DESKTOP DESKTOP_BINS=$AOK_DESKTOP DESKTOP_SETUP=setup-wayland.sh ;;
esac
COMPOSITOR_CMD="${WAYLAND_COMPOSITOR_CMD:-$COMPOSITOR_BIN}"
READY_FILE="${ISH_DISPLAY_READY_FILE:-/tmp/ish-display.ready}"
ERROR_FILE="$READY_FILE.error"

# ONE Wayland session at a time, and a second run refuses rather than takes
# over. Every session binds the same WAYVNC_PORT and the same $READY_FILE, and
# the stale-session sweep below kills labwc/foot/wayvnc by name, so a second
# run used to end whatever desktop was already on screen -- a second Workspace
# window's Wayland applet, or this script typed into the desktop's own foot,
# silently closed the one in use. So before anything below touches a shared
# name, look for a live session and stop if there is one.
#
# A live session is found in /proc, not in a lock file: /tmp and /run are
# wiped partway through guest boot, which is exactly when the applet starts
# the first session, and $HOME differs between a root session and an "Open
# Everything as Default User" one.
#
# A session that is SHUTTING DOWN is not a live one: closing the applet and
# opening it again hangs up the old session's pty and starts the new one at
# once, and the old one's cleanup() runs whenever the guest scheduler gets to
# it. cleanup() leaves /tmp/ish-display.closing.<pid> for exactly as long as it
# runs, and this waits for such a session to finish. A brief grace covers the
# moment between the hangup and cleanup() starting.
aok_ppid() { sed -n 's/^PPid:[[:space:]]*//p' "/proc/$1/status" 2>/dev/null; }
# Is pid $1 a shell running this script? argv[0] must be a shell, so vi or
# less looking at start-wayland.sh is not mistaken for a session.
aok_runs_start_wayland() {
    _wl_n=0
    _wl_hit=1
    while IFS= read -r _wl_arg; do
        if [ "$_wl_n" = 0 ]; then
            case "${_wl_arg##*/}" in sh|dash|bash|ash|busybox) ;; *) return 1 ;; esac
        else
            case "$_wl_arg" in *start-wayland.sh) _wl_hit=0 ;; esac
        fi
        _wl_n=$((_wl_n + 1))
    done <<AOK_CMDLINE_EOF
$(tr '\0' '\n' < "/proc/$1/cmdline" 2>/dev/null)
AOK_CMDLINE_EOF
    return $_wl_hit
}
# A session with no compositor under it that is past its start-up is not a
# desktop anyone can use: what is left of one whose compositor died -- a crash,
# or a resume that left the desktop out of the saved session (the GPU cannot be
# saved) while the script and something it waits on came back. It used to
# refuse every new desktop as "already running". The age keeps a session that
# is still starting, and has no compositor YET, from being taken for one.
aok_session_stale() {
    for _wl_c in /proc/[0-9]*; do
        [ "$(aok_ppid "${_wl_c#/proc/}")" = "$1" ] || continue
        case "$(cat "$_wl_c/comm" 2>/dev/null)" in labwc|sway|wayfire) return 1 ;; esac
    done
    _wl_hz=$(getconf CLK_TCK 2>/dev/null || echo 100)
    _wl_started=$(awk '{print $22}' "/proc/$1/stat" 2>/dev/null)
    _wl_up=$(awk -v hz="$_wl_hz" '{print int($1 * hz)}' /proc/uptime 2>/dev/null)
    [ -n "$_wl_started" ] && [ -n "$_wl_up" ] && [ $(( (_wl_up - _wl_started) / _wl_hz )) -ge 30 ]
}
# End a stale session: its children (wayvnc, tee, a foot) and then the script.
aok_end_stale_session() {
    printf 'start-wayland.sh: clearing a stale Wayland session (pid %s): its compositor is gone\n' "$1" >&2
    for _wl_c in /proc/[0-9]*; do
        [ "$(aok_ppid "${_wl_c#/proc/}")" = "$1" ] && kill -TERM "${_wl_c#/proc/}" 2>/dev/null
    done
    kill -TERM "$1" 2>/dev/null
}
# Pids of every OTHER session. This run is excluded, and so are its ancestors
# (the su/sh wrappers DisplayViewController starts it through) and its own
# subshells, whose cmdline fork copies unchanged -- including the $(...) this
# runs in.
aok_other_wayland_sessions() {
    _wl_mine=" "
    _wl_p=$$
    while [ -n "$_wl_p" ] && [ "$_wl_p" -gt 1 ]; do
        _wl_mine="$_wl_mine$_wl_p "
        _wl_p=$(aok_ppid "$_wl_p")
    done
    for _wl_file in $(grep -ls 'start-wayland\.sh' /proc/[0-9]*/cmdline 2>/dev/null); do
        _wl_pid=${_wl_file#/proc/}
        _wl_pid=${_wl_pid%/cmdline}
        case "$_wl_mine" in *" $_wl_pid "*) continue ;; esac
        aok_runs_start_wayland "$_wl_pid" || continue
        _wl_p=$(aok_ppid "$_wl_pid")
        _wl_own=0
        while [ -n "$_wl_p" ] && [ "$_wl_p" -gt 1 ]; do
            [ "$_wl_p" = "$$" ] && { _wl_own=1; break; }
            _wl_p=$(aok_ppid "$_wl_p")
        done
        [ "$_wl_own" = 1 ] && continue
        if aok_session_stale "$_wl_pid"; then
            aok_end_stale_session "$_wl_pid"
            continue
        fi
        printf '%s ' "$_wl_pid"
    done
}
_wl_others=$(aok_other_wayland_sessions)
_wl_waited=0
while [ -n "$_wl_others" ]; do
    _wl_closing=0
    for _wl_pid in $_wl_others; do
        [ -e "/tmp/ish-display.closing.$_wl_pid" ] && _wl_closing=1
    done
    # Quarter-second polls: 3 s of grace, or 20 s for one that is closing.
    if [ "$_wl_closing" = 1 ]; then
        [ "$_wl_waited" -lt 80 ] \
            || die "the previous Wayland session (pid ${_wl_others% }) is still shutting down after 20 seconds -- try again in a moment"
    elif [ "$_wl_waited" -ge 12 ]; then
        die "a Wayland session is already running (pid ${_wl_others% }). Only one Wayland desktop can run at a time: use the one that is open, or close it first."
    fi
    sleep 0.25
    _wl_waited=$((_wl_waited + 1))
    _wl_others=$(aok_other_wayland_sessions)
done

# The guest's early boot, the stage that wipes /tmp: Devuan's `rc S`, whose
# bootclean honours TMPTIME=0, or OpenRC's sysinit and boot runlevels. The app
# opens this session as soon as it launches, so on a slow device the compositor
# is up long before that cleanup runs (5th-generation iPad: labwc at 4 s, the
# cleanup at 26 s), and the cleanup deletes the compositor's /tmp/.X<n>-lock,
# its /tmp/.X11-unix/X<n> and the applet's ready file. Waiting the cleanup out
# before starting cost the desktop ~20 s there, so the session starts at once
# and repairs afterwards instead (aok_repair_after_early_boot, below). The
# runtime dir is under $HOME for the same reason (see WL_RUNTIME_BASE); X's
# paths cannot move.
aok_early_boot_running() {
    for _eb_cmdline in /proc/[0-9]*/cmdline; do
        _eb_args=$(tr '\0' ' ' < "$_eb_cmdline" 2>/dev/null) || continue
        case "$_eb_args" in
            *"/etc/init.d/rc S "*|*"/etc/init.d/rcS "*|*"openrc sysinit "*|*"openrc boot "*) return 0 ;;
        esac
    done
    return 1
}
# Started near boot? The app starts this session together with the guest, so
# init may not even have started its early boot yet: on a 5th-generation iPad
# this script was pid 3. The repair is armed by uptime, not by catching the
# early boot in the act.
UPTIME_AT_START=$(cut -d. -f1 /proc/uptime 2>/dev/null || echo 9999)

# The distro's env and sh, by path, for the commands whose pid this script
# tracks. native-links.sh puts SmallCLUE's env and dash first on PATH, and a
# native program cannot exec in place -- it runs the command as a child and
# waits -- so $! named the wrapper and not the compositor: the X lock (written
# with labwc's pid) never matched, the fallback looked at the wrapper's
# sockets, and no X program in the desktop had a DISPLAY.
AOK_ENV=/usr/bin/env
[ -x "$AOK_ENV" ] || AOK_ENV=env
AOK_SH=/bin/sh

rm -f "$READY_FILE" "$ERROR_FILE"

for bin in $DESKTOP_BINS foot wayvnc; do
    command -v "$bin" >/dev/null 2>&1 \
        || die "'$bin' not found -- run 'sudo sh /AOK/tools/$DESKTOP_SETUP' first"
done

# Clean up any compositor/foot/wayvnc a dead session left behind. The
# one-session check above has already refused, or waited out, every session
# whose script is still alive, so whatever is killed here is an orphan: its
# script was killed too hard to run cleanup(), and it still holds the fixed
# WAYVNC_PORT. Without this, overlapping instances fight over that port and
# pile up as unreaped zombies that drag the whole guest's scheduling down
# until the applet looks wedged. Both compositors are killed by name (not
# just $COMPOSITOR_CMD) in case WAYLAND_COMPOSITOR_CMD was switched between
# sessions and a stale instance of the *other* one is still around.
pkill -x sway 2>/dev/null
pkill -x labwc 2>/dev/null
pkill -x wayfire 2>/dev/null
# And what the other desktops start inside themselves: Wayfire's wf-shell, and
# Xfce's session and its panel, desktop and settings daemon.
for _wl_p in wf-panel wf-background wf-dock xfce4-session xfce4-panel xfdesktop xfsettingsd; do
    pkill -x "$_wl_p" 2>/dev/null
done
pkill -x foot 2>/dev/null
pkill -x wayvnc 2>/dev/null
pkill -x wofi 2>/dev/null
sleep 0.2

# (XDG_RUNTIME_DIR is set up below, after the HOME fixup it depends on.)

# DisplayViewController always execs this script with a hardcoded root envp
# (PATH/HOME=/root/TERM); when "Open Everything as Default User" is on, that
# exec is wrapped in `su - <user> -c '...'` (see DisplayGuestSessionCommand
# in DisplayViewController.m), which is *supposed* to reset HOME/SHELL/USER
# to the target account's own -- but observed on-device: HOME was still
# /root even though $(id -u) was the non-root default user, i.e. this
# rootfs's `su` doesn't fully reset the environment for the `-c` form. Don't
# trust the inherited environment; look up the actual running uid's account
# directly.
CURRENT_UID="$(id -u)"
PW_LINE="$(awk -F: -v uid="$CURRENT_UID" '$3 == uid { print; exit }' /etc/passwd 2>/dev/null)"
if [ -n "$PW_LINE" ]; then
    PW_HOME="$(printf '%s' "$PW_LINE" | cut -d: -f6)"
    [ -n "$PW_HOME" ] && export HOME="$PW_HOME"
fi
export HOME="${HOME:-/root}"
# A UTF-8 locale when nothing named one. A session started from a shell that
# never passed through login has no LANG, and every program in the desktop
# inherits that: btop refuses to start without UTF-8 (#620), and terminals and
# GTK programs mangle anything non-ASCII. LANG only; LC_ALL would outrank a
# per-category choice made later.
if [ -z "${LANG:-}${LC_ALL:-}" ]; then
    export LANG=C.UTF-8
fi
# SHELL: prefer bash outright rather than trusting the passwd entry's shell
# field, which is often stale or minimal -- the "Open Everything as Default
# User" account this su targets (AppDelegate.m's ProvisionDefaultUserAccount)
# used to hardcode /bin/sh, and that field is never rewritten once the
# account exists (by design, to preserve hand-edits), so a device where that
# account got created before that was fixed stays on /bin/sh forever even
# after upgrading. Confirmed on-device: a genuinely interactive `-sh` (dash),
# not a broken/non-interactive shell -- just not the bash a user typing at a
# terminal actually wants, hence "I have to manually run bash". This is what
# foot spawns its terminal shell from; check /bin/bash directly instead of
# re-deriving it from a field that's unreliable for this specific purpose.
if [ -x /bin/bash ]; then
    export SHELL=/bin/bash
elif [ -n "$PW_LINE" ]; then
    PW_SHELL="$(printf '%s' "$PW_LINE" | cut -d: -f7)"
    [ -n "$PW_SHELL" ] && export SHELL="$PW_SHELL"
fi

# The desktops whose command names files under $HOME, now that HOME is right.
# Wayfire gets its config by path, since 0.9 and 0.11 look in different places
# by default. Xfce is what `startxfce4 --wayland` runs: labwc with a config
# directory of Xfce's own, so the plain labwc desktop's menu, panel and
# autostart stay out of it, running xfce4-session for as long as the session
# lasts. That uses the session bus this script starts below, as
# dbus-run-session would.
if [ -z "${WAYLAND_COMPOSITOR_CMD:-}" ]; then
    case "$AOK_DESKTOP" in
        wayfire) COMPOSITOR_CMD="wayfire -c $HOME/.config/wayfire.ini" ;;
        xfce) COMPOSITOR_CMD="labwc --config-dir $HOME/.config/xfce4/labwc --session xfce4-session" ;;
    esac
fi
case "$AOK_DESKTOP" in
    xfce) export XDG_CURRENT_DESKTOP=XFCE XDG_SESSION_DESKTOP=xfce XDG_SESSION_TYPE=wayland ;;
    wayfire) export XDG_CURRENT_DESKTOP=wayfire XDG_SESSION_DESKTOP=wayfire XDG_SESSION_TYPE=wayland ;;
esac

# A runtime dir scoped to this invocation (pid-suffixed) avoids colliding
# with a leftover socket/lock from a prior run that didn't get torn down
# cleanly (observed during bring-up: a stale wayvnc control socket makes the
# next wayvnc refuse to start with "Another wayvnc process is already
# running").
#
# It lives under $HOME, NOT /tmp: this session typically starts early in
# guest boot (the Display applet opens it as soon as the app is up), and the
# guest's own init then wipes /tmp partway through boot (Debian/Devuan
# bootmisc.sh with the default TMPTIME=0 deletes everything in /tmp; Alpine's
# bootmisc does the same) -- observed on-device deleting the runtime dir out
# from under a LIVE session: already-connected clients (foot, wayvnc) kept
# their sockets and rendered fine, but every new client failed with
# "Failed to connect to Wayland display '/tmp/xdg-display-NN/wayland-0':
# No such file or directory" (e.g. firefox launched from the foot shell).
# The same applies to /run (init mounts a fresh tmpfs over it mid-boot) and
# /dev/shm (ditto), so a fakefs-backed home directory is the only location
# that reliably survives guest boot. Unix sockets work fine on fakefs, and
# socket traffic never touches the filesystem.
WL_RUNTIME_BASE="$HOME/.cache/ish-display"
# Sweep stale runtime dirs from prior sessions that died without cleanup()
# (app killed, crash): they're pid-suffixed so they never collide, but they'd
# otherwise accumulate forever now that boot doesn't clean them for us. The
# same goes for the session bus such a session started (below): it is found by
# its socket path, which nothing else uses.
pkill -f "dbus-daemon --session --fork --nopidfile --address=unix:path=$WL_RUNTIME_BASE/" 2>/dev/null
rm -rf "$WL_RUNTIME_BASE" 2>/dev/null
# Likewise a closing marker whose cleanup() was killed before it removed it.
# No other session is alive by now, so none of these is current, and guest
# pids restart at 1 every boot: a stale one would name a later live session.
rm -f /tmp/ish-display.closing.* 2>/dev/null
export XDG_RUNTIME_DIR="$WL_RUNTIME_BASE/$$"
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"

# A session D-Bus for this desktop. Nothing else starts one on these roots,
# which have no systemd user session, so GLib and Qt programs found no session
# bus at all: waybar stopped with "Cannot autolaunch D-Bus without X11
# $DISPLAY", and Qt apps such as Falkon could not connect (GH #485). Its socket
# lives in XDG_RUNTIME_DIR with the rest of the session, and cleanup() stops
# it. A bus the caller already has is kept, and a root without dbus-daemon
# goes without; setup-wayland.sh installs it.
DBUS_DAEMON_PID=""
if [ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ] && command -v dbus-daemon >/dev/null 2>&1; then
    session_bus_address="unix:path=$XDG_RUNTIME_DIR/bus"
    # --fork returns with the socket already in place (5 of 5 on Devuan), and
    # the daemon moves its own stdout to /dev/null, so the substitution ends
    # when the parent exits rather than waiting on the daemon.
    DBUS_DAEMON_PID="$(dbus-daemon --session --fork --nopidfile \
        --address="$session_bus_address" --print-pid)" || DBUS_DAEMON_PID=""
    if [ -n "$DBUS_DAEMON_PID" ]; then
        export DBUS_SESSION_BUS_ADDRESS="$session_bus_address"
        log "session D-Bus at $session_bus_address"
    else
        log "dbus-daemon did not start: programs that need a session bus will not find one"
    fi
fi

export WLR_BACKENDS=headless
export WLR_LIBINPUT_NO_DEVICES=1
# A headless output can never scan a client's buffer out directly, yet
# wlroots' scene (0.18) tries it every frame for a fullscreen window: it sends
# the surface dmabuf feedback with a scanout tranche, the test commit fails,
# and the render path sends feedback without one. The two alternate, so both
# go out every frame, each with a new format-table descriptor. Mesa's EGL
# surface keeps its feedback object on a queue it never dispatches once zink
# presents through Vulkan, so those descriptors pile up in the client: a
# fullscreen Doom gained ~5 a second and hit its 1024 limit ("dup failed: Too
# many open files", then VK_ERROR_TOO_MANY_OBJECTS on the next swapchain) on
# a 5th-generation iPad, 2026-09-30. Windowed, it stayed at 20.
export WLR_SCENE_DISABLE_DIRECT_SCANOUT=1
# Which renderer the compositor uses. With the GPU render node (/dev/dri/
# renderD128, #484) and the guest's Mesa able to use it -- the Venus Vulkan
# driver and zink -- it composites on the host GPU: wlroots' Vulkan renderer,
# output buffers from GBM, and client buffers taken as dma-bufs with no copy.
# The session profiled with ~40% less CPU for the same frames that way.
# Otherwise, and whenever that fails to bring up an output (below), it
# renders in software (pixman). WLR_RENDERER set by the caller wins, and
# ISH_DISPLAY_GPU=0 keeps it in software. On the GPU the app also takes the
# frames directly (wl-present, below); ISH_DISPLAY_DIRECT=0 leaves that to
# wayvnc.
wl_gpu_usable() {
    [ -c /dev/dri/renderD128 ] || return 1
    ls /usr/share/vulkan/icd.d/virtio_icd*.json >/dev/null 2>&1 || return 1
    for zink in /usr/lib/*/dri/zink_dri.so /usr/lib/dri/zink_dri.so /usr/lib64/dri/zink_dri.so; do
        [ -e "$zink" ] && return 0
    done
    return 1
}
WL_GPU_COMPOSITOR=0
if [ -z "${WLR_RENDERER:-}" ] && [ "${ISH_DISPLAY_GPU:-1}" != 0 ] && wl_gpu_usable; then
    WL_GPU_COMPOSITOR=1
fi
# Two things Wayfire cannot do, where the session is the plain labwc desktop
# instead, and says why. It draws everything with OpenGL ES, so it cannot run
# in software here: wlroots' GLES renderer needs a GPU device, and pixman has
# no GL. And it will not run as root: after dropping privileges it checks that
# setuid(0) fails, and refuses to start when it succeeds ("Unable to drop
# root"), as on any Linux. The Wayland window is a root session unless
# Settings > Open Everything as Default User is on.
wl_wayfire_cannot=""
if [ "$AOK_DESKTOP" = wayfire ] && [ -z "${WAYLAND_COMPOSITOR_CMD:-}" ]; then
    if [ "$CURRENT_UID" = 0 ]; then
        wl_wayfire_cannot="Wayfire does not run as root -- turn on Settings > Open Everything as Default User"
    elif [ "$WL_GPU_COMPOSITOR" = 0 ]; then
        wl_wayfire_cannot="Wayfire needs the GPU (setup-gpu.sh), which this session cannot use"
    fi
fi
if [ -n "$wl_wayfire_cannot" ]; then
    log "warning: $wl_wayfire_cannot; starting labwc instead"
    AOK_DESKTOP=labwc COMPOSITOR_BIN=labwc COMPOSITOR_CMD=labwc
    command -v labwc >/dev/null 2>&1 || die "$wl_wayfire_cannot, and labwc is not installed to fall back to"
fi
export WLR_RENDERER="${WLR_RENDERER:-pixman}"
# There's no real GPU/DRM device here (matches labwc's own harmless
# "drmGetDevices2 failed: No such file or directory" at startup). GTK3
# apps generally cope via cairo software rendering by default, but GTK4's
# GskRenderer tries GL/Vulkan first -- confirmed on-device: gnome-calculator
# hit "MESA: error: ZINK: vkCreateInstance failed (VK_ERROR_INCOMPATIBLE_
# DRIVER)" and only rendered once forced to cairo/software. Exported here
# (not just set for wayvnc/foot individually) so every client an interactive
# session launches -- typically from a shell inside foot -- inherits them
# without the user needing to know this environment has no GPU.
export GSK_RENDERER=cairo
# OpenGL programs render in software unless /etc/aok-gpu.conf (written by
# /AOK/tools/setup-gpu.sh --gl-default) says AOK_GL=gpu: zink on this GPU is
# OpenGL 2.1 / ES 2.0 against llvmpipe's 4.5, so the GPU cannot be everyone's
# default. A caller's own LIBGL_ALWAYS_SOFTWARE wins either way.
if [ -z "${LIBGL_ALWAYS_SOFTWARE+set}" ] && [ -f /etc/aok-gpu.conf ] &&
   grep -q '^AOK_GL=gpu' /etc/aok-gpu.conf 2>/dev/null && wl_gpu_usable; then
    export LIBGL_ALWAYS_SOFTWARE=0 MESA_LOADER_DRIVER_OVERRIDE=zink
fi
export LIBGL_ALWAYS_SOFTWARE="${LIBGL_ALWAYS_SOFTWARE-1}"
# Debian/Devuan firefox-esr routes its Wayland connection through a bundled
# "wayland-proxy-compositor" shim by default. Under iSH that relay corrupts
# the stream: firefox dies at startup with "Wayland protocol error:
# wl_display#1: error 0: invalid object 0" and then spins in its crash path
# (~38k aborts/sec, pinning the emulator). With the proxy disabled, the same
# firefox speaks flawless Wayland straight to labwc (verified on-device with
# WAYLAND_DEBUG=1: full registry/shm/output/seat startup, zero errors; the
# same stack on real Linux is clean either way, so the underlying bug is in
# how iSH's unix-socket layer handles the proxy's relay pattern -- tracked
# separately; this export is the workaround until that's root-caused).
export MOZ_DISABLE_WAYLAND_PROXY=1
# GTK programs, waybar among them, look for the AT-SPI accessibility bus at
# startup and warn when there is none ("AT-SPI: Error retrieving accessibility
# bus address"). Nothing here can use it: no screen reader reaches a desktop
# drawn over VNC. Installing at-spi2-core instead would add a bus launcher to
# every session for nothing.
export NO_AT_BRIDGE=1
# GTK 4 ignores NO_AT_BRIDGE and starts the accessibility bus anyway, which is
# at-spi-bus-launcher and its registry daemon, about 20 MB, beside every GTK 4
# program (gnome-mines, gnome-chess).
export GTK_A11Y=none
# SDL tries X11 before Wayland whenever DISPLAY is set, so SDL games ran behind
# Xwayland; they run as Wayland clients instead. X11 stays second, for an SDL
# built without Wayland. SDL 2 reads SDL_VIDEODRIVER and SDL 3 SDL_VIDEO_DRIVER;
# both take a comma-separated list.
export SDL_VIDEODRIVER=wayland,x11
export SDL_VIDEO_DRIVER=wayland,x11
# Debian and Devuan install games in /usr/games, which their /etc/profile puts
# on PATH only for users other than root, and the app starts this script with
# a fixed PATH without it: game entries in the Applications menu did nothing, and
# gnome-chess found no chess engine (gnuchess is /usr/games/gnuchess).
for games_dir in /usr/local/games /usr/games; do
    case ":$PATH:" in
        *":$games_dir:"*) ;;
        *) PATH="$PATH:$games_dir" ;;
    esac
done

# pkexec, for this desktop only. Programs that need root from a menu --
# Synaptic's entry runs synaptic-pkexec, which is `pkexec synaptic` -- ask
# polkit, and polkit asks an authentication agent registered for the caller's
# login session. There are no login sessions here (no elogind), so no agent
# can register, lxpolkit included (tried), and pkexec gives up: "Error
# creating textual authentication agent" (#620: Synaptic "refuses to start").
# This one, first on the session's PATH, asks for the password with sudo in a
# terminal and starts the program with this display, then closes the
# terminal. The wrappers find it because they call pkexec by name. Root runs
# the program straight away, as polkit would allow.
AOK_SESSION_BIN="$WL_RUNTIME_BASE/bin"
mkdir -p "$AOK_SESSION_BIN"
cat > "$AOK_SESSION_BIN/pkexec" <<'PKEXEC_EOF'
#!/bin/sh
# Written by /AOK/tools/start-wayland.sh for the Wayland desktop (see there).
user=root
while [ $# -gt 0 ]; do
    case "$1" in
        --user|-u) user=${2:-root}; shift 2 ;;
        --user=*) user=${1#--user=}; shift ;;
        --disable-internal-agent|--keep-cwd) shift ;;
        --help|--version) exec /usr/bin/pkexec "$@" ;;
        --) shift; break ;;
        *) break ;;
    esac
done
[ $# -gt 0 ] || exec /usr/bin/pkexec
# The distro's sudo, or iSH-AOK's own on a root that has none.
AOK_PKEXEC_SUDO=$(command -v sudo 2>/dev/null || echo /AOK/native/sudo)
export AOK_PKEXEC_SUDO
if [ "$(id -u)" = 0 ] && [ "$user" = root ]; then
    exec "$@"
fi
# Started as root on this display: sudo resets the environment, so the
# display variables go through env explicitly, and HOME is the target's.
as_user() {
    target_home=$(getent passwd "$user" 2>/dev/null | cut -d: -f6)
    "$AOK_PKEXEC_SUDO" -u "$user" env HOME="${target_home:-/root}" LANG="${LANG:-C.UTF-8}" \
        WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-}" XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-}" \
        DISPLAY="${DISPLAY:-}" "$@"
}
if [ -t 0 ]; then
    as_user "$@"
    exit $?
fi
# From a menu there is nothing to type a password into: a terminal asks,
# starts the program in its own session so closing the terminal leaves it
# running, and closes -- unless sudo refused, which stays on screen.
command -v foot >/dev/null 2>&1 || exec /usr/bin/pkexec "$@"
export AOK_PKEXEC_USER="$user"
export AOK_PKEXEC_DETACH="${0%/*}/aok-detach"
# sudo in the foreground, where it can ask; aok-detach starts the program in
# a session of its own and returns, so the terminal can close.
exec foot --title "Administrator password" sh -c '
    user=$AOK_PKEXEC_USER
    target_home=$(getent passwd "$user" 2>/dev/null | cut -d: -f6)
    printf "%s needs %s rights.\n" "${1##*/}" "$user"
    "$AOK_PKEXEC_SUDO" -u "$user" env HOME="${target_home:-/root}" LANG="${LANG:-C.UTF-8}" \
        WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-}" XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-}" \
        DISPLAY="${DISPLAY:-}" "$AOK_PKEXEC_DETACH" "$@"
    status=$?
    if [ "$status" -ne 0 ]; then
        printf "\nCould not start %s (%s). Press Enter to close.\n" "$1" "$status"
        read -r _
    fi
' pkexec "$@"
PKEXEC_EOF
cat > "$AOK_SESSION_BIN/aok-detach" <<'DETACH_EOF'
#!/bin/sh
# Written by /AOK/tools/start-wayland.sh: run a program in a session of its
# own and return at once, so the terminal that started it can close.
[ $# -gt 0 ] || exit 2
command -v "$1" >/dev/null 2>&1 || { echo "$1: not found" >&2; exit 127; }
# The new session has to exist BEFORE this returns. Backgrounding
# `setsid PROGRAM` raced the terminal: sudo returned, the terminal closed,
# and its hangup killed the program before setsid had run -- under emulation,
# every time. So setsid runs in the foreground; the shell it starts is already
# in the new session when it puts the program in the background. Without
# setsid, SIGHUP is ignored before the fork, which is just as race-free.
if command -v setsid >/dev/null 2>&1; then
    setsid sh -c '"$@" </dev/null >/dev/null 2>&1 &' sh "$@"
else
    trap '' HUP
    "$@" </dev/null >/dev/null 2>&1 &
fi
exit 0
DETACH_EOF
chmod 755 "$AOK_SESSION_BIN/pkexec" "$AOK_SESSION_BIN/aok-detach"
# The terminal a desktop's launcher opens a terminal program in (Terminal=true:
# btop, htop, mc). GLib's launcher -- wf-panel's menu, and anything else on
# GIO -- asks for xdg-terminal-exec first and, without it, falls to
# x-terminal-emulator, which Debian points at lxterm: an X11 terminal, which
# needs Xwayland's DISPLAY, and btop from Wayfire's menu never appeared
# ("xterm: Xt error: Can't open display:", bip 2026-10-02). foot is the
# desktop's own terminal. A real xdg-terminal-exec, if installed, wins.
rm -f "$AOK_SESSION_BIN/xdg-terminal-exec"
if ! command -v xdg-terminal-exec >/dev/null 2>&1 && command -v foot >/dev/null 2>&1; then
    cat > "$AOK_SESSION_BIN/xdg-terminal-exec" <<'TERMEXEC_EOF'
#!/bin/sh
# Written by /AOK/tools/start-wayland.sh: the desktop's terminal for
# launchers (the xdg-terminal-exec convention: the command, if any, follows).
[ "$1" = "-e" ] && shift
exec foot "$@"
TERMEXEC_EOF
    chmod 755 "$AOK_SESSION_BIN/xdg-terminal-exec"
fi
case ":$PATH:" in
    *":$AOK_SESSION_BIN:"*) ;;
    *) PATH="$AOK_SESSION_BIN:$PATH" ;;
esac
export PATH

# Pixman accelerator (kernel/ish_accel_pix.c via ISH_SYS_PIXOP): loaded only
# if setup-wayland.sh's best-effort build actually produced the shim AND the
# session hasn't explicitly opted out. Every interposed call in the shim
# falls through to real pixman whenever the accelerator turns out to be
# unavailable (ISH_PIX_ACCEL off, or an emulator build without it) -- so
# exporting this is always safe even if the host-side toggle is off; it
# just makes acceleration possible when it's on, never required.
#
# /AOK/bundled carries both shims prebuilt for this root's libc and CPU
# (tools/build-bundled.sh), and they come first: they need no compiler and
# update with the app. A local build (setup-wayland.sh, or the release
# guard's per-user one below) is the fallback for a root they do not cover.
AOK_BUNDLED_LIBC=glibc
ls /lib/ld-musl-*.so.1 >/dev/null 2>&1 && AOK_BUNDLED_LIBC=musl
case "$(uname -m)" in
    aarch64|arm64) AOK_BUNDLED_ARCH=aarch64 ;;
    x86_64|amd64) AOK_BUNDLED_ARCH=x86_64 ;;
    i[3-6]86) AOK_BUNDLED_ARCH=i386 ;;   # AOK's x86 guest says i686
    *) AOK_BUNDLED_ARCH=$(uname -m) ;;
esac
AOK_BUNDLED_LIBS=/AOK/bundled/$AOK_BUNDLED_LIBC-$AOK_BUNDLED_ARCH
if [ -z "${ISH_PIXMAN_SHIM:-}" ] && [ -f "$AOK_BUNDLED_LIBS/libish-pixman.so" ]; then
    ISH_PIXMAN_SHIM=$AOK_BUNDLED_LIBS/libish-pixman.so
fi
ISH_PIXMAN_SHIM="${ISH_PIXMAN_SHIM:-/usr/local/lib/ish-pixman/libish-pixman.so}"
if [ "${ISH_WAYLAND_DISABLE_PIXMAN_SHIM:-0}" != "1" ] && [ -f "$ISH_PIXMAN_SHIM" ]; then
    export LD_PRELOAD="$ISH_PIXMAN_SHIM${LD_PRELOAD:+:$LD_PRELOAD}"
fi

# The wl_buffer release guard (tools/wayland/ish_wl_release_guard.c): wlroots
# releases a shm buffer once per commit, and a GTK 3 program that committed
# one twice -- wf-panel, opening its menu on a desktop whose frames came late
# -- aborted on the second release (bip, 2026-10-02). setup-wayland.sh
# installs it; a desktop set up before that gets it built here, once, into
# the user's cache, when there is a compiler. Without either the session runs
# as it always has. ISH_WAYLAND_DISABLE_RELEASE_GUARD=1 leaves it out.
ISH_RELEASE_GUARD=$AOK_BUNDLED_LIBS/libish-wl-release-guard.so
[ -f "$ISH_RELEASE_GUARD" ] || ISH_RELEASE_GUARD=/usr/local/lib/ish-wayland/libish-wl-release-guard.so
if [ ! -f "$ISH_RELEASE_GUARD" ]; then
    ISH_RELEASE_GUARD="$HOME/.cache/ish-wayland/libish-wl-release-guard.so"
    if [ "${ISH_WAYLAND_DISABLE_RELEASE_GUARD:-0}" != "1" ] && command -v cc >/dev/null 2>&1 \
            && { [ ! -f "$ISH_RELEASE_GUARD" ] \
                 || [ /AOK/tools/wayland/ish_wl_release_guard.c -nt "$ISH_RELEASE_GUARD" ]; }; then
        log "building the Wayland buffer release guard"
        sh /AOK/tools/wayland/build-release-guard.sh "$HOME/.cache/ish-wayland" >/dev/null 2>&1 \
            || log "warning: the release guard did not build; GTK programs may abort on a double buffer release"
    fi
fi
if [ "${ISH_WAYLAND_DISABLE_RELEASE_GUARD:-0}" != "1" ] && [ -f "$ISH_RELEASE_GUARD" ]; then
    export LD_PRELOAD="$ISH_RELEASE_GUARD${LD_PRELOAD:+:$LD_PRELOAD}"
fi

COMPOSITOR_PID=""
FOOT_PID=""
REPAIR_PID=""
TEE_PIDS=""
WAYVNC_PID=""
PANEL_PID=""
PRESENT_PID=""

cleanup() {
    trap - TERM INT HUP EXIT
    # Tells a session starting now that this one is going, not staying, so it
    # waits instead of refusing (see the one-session check near the top).
    # `true`, not `:`: a failed redirection on a special builtin ends dash
    # there and then, and a stale marker of another uid's (guest pids restart
    # every boot, so a root session's can carry this pid) made every default-
    # user session exit here, leaving its compositor, wayvnc and foot running.
    { true > "/tmp/ish-display.closing.$$"; } 2>/dev/null
    rm -f "$READY_FILE"
    [ -n "$PANEL_PID" ] && kill "$PANEL_PID" 2>/dev/null
    # The early-boot repair (below) polls for up to a minute or more, and the
    # `wait` here waited for it: a session closed soon after boot sat in
    # cleanup until it finished, and the next one said the previous was still
    # shutting down. It has nothing left to repair once the session ends.
    [ -n "$REPAIR_PID" ] && kill "$REPAIR_PID" 2>/dev/null
    [ -n "$PRESENT_PID" ] && kill "$PRESENT_PID" 2>/dev/null
    [ -n "$WAYVNC_PID" ] && kill "$WAYVNC_PID" 2>/dev/null
    [ -n "$FOOT_PID" ] && kill "$FOOT_PID" 2>/dev/null
    [ -n "$COMPOSITOR_PID" ] && kill "$COMPOSITOR_PID" 2>/dev/null
    for pid in $PANEL_PID $PRESENT_PID $WAYVNC_PID $FOOT_PID $COMPOSITOR_PID $REPAIR_PID; do
        wait "$pid" 2>/dev/null
    done
    # Each log's tee ends when the last process writing to it has gone, and a
    # daemon the desktop started can keep the compositor's output open long
    # after the desktop is gone: gpg-agent, which xfce4-session launches,
    # held this cleanup indefinitely (1 run in 4), and every new session then
    # waited for it and gave up. A moment to drain, then they are stopped.
    sleep 0.5
    [ -n "$TEE_PIDS" ] && kill $TEE_PIDS 2>/dev/null
    wait 2>/dev/null
    [ -n "$DBUS_DAEMON_PID" ] && kill "$DBUS_DAEMON_PID" 2>/dev/null
    rm -rf "$XDG_RUNTIME_DIR"
    rm -f "/tmp/ish-display.closing.$$"
    exit 0
}
trap cleanup TERM INT HUP EXIT

# Every subprocess's stdout/stderr also goes to this file (in addition to
# >&2, the pty DisplayViewController owns, which nothing else can tail) --
# without it, a crash right after launch (e.g. wayvnc/foot dying before
# their first line of output reaches the pty) leaves no way to diagnose
# *why* short of re-running this script by hand over SSH.
#
# It lives under $HOME and /tmp/ish-wayland-debug.log points at it: a session
# started early in boot had its log deleted by the boot's /tmp cleanup along
# with the evidence of why wayvnc died (5th-generation iPad, 2026-10-01), and
# it is outside $WL_RUNTIME_BASE because each session (a Reconnect included)
# empties that. The previous session's log is kept beside it as .prev.
DEBUG_LOG="${ISH_DISPLAY_DEBUG_LOG:-}"
if [ -z "$DEBUG_LOG" ]; then
    DEBUG_LOG="${HOME:-/tmp}/.cache/ish-wayland-debug.log"
    mkdir -p "${DEBUG_LOG%/*}" 2>/dev/null
    [ -s "$DEBUG_LOG" ] && mv -f "$DEBUG_LOG" "$DEBUG_LOG.prev" 2>/dev/null
    # -sf, not -sfn: SmallCLUE's ln (first on PATH after native-links.sh)
    # rejected -n until smallclue's GNU-compatible ln, and the target is a
    # file, so -n changes nothing here anyway.
    ln -sf "$DEBUG_LOG" /tmp/ish-wayland-debug.log 2>/dev/null
fi
# An unwritable $DEBUG_LOG is FATAL downstream, not cosmetic: spawn_logged
# pipes every process through `tee -a $DEBUG_LOG`, so if tee can't open the
# log it exits, the fifo loses its reader, and the compositor dies on SIGPIPE
# at its first line of output. Observed on-device: a stale ROOT-owned log in
# sticky /tmp (left by an earlier root-run session) that a default-user
# session couldn't truncate -- the whole desktop died of a log file. Fall
# back to a path this uid owns, then to /dev/null (losing the log but never
# the session).
# `true`, not `:`: a redirection error on a POSIX *special* builtin (which
# `:` is) makes dash exit the whole script on the spot -- the probe must be a
# regular builtin so EACCES just fails the command and the fallback runs.
if ! { true > "$DEBUG_LOG"; } 2>/dev/null; then
    DEBUG_LOG="${HOME:-/tmp}/ish-wayland-debug.log"
    { true > "$DEBUG_LOG"; } 2>/dev/null || DEBUG_LOG=/dev/null
fi

# wlroots allocates the seat keymap via shm_open() under /dev/shm. If that
# fails (missing dir, or a wrong-mode /dev/shm that non-root can't write
# to -- seen on-device before the app-side boot repair enforced 1777),
# labwc starts up apparently fine but with a NULL keymap, then exit()s the
# instant the first keyboard attaches (wayvnc's virtual keyboard, i.e. the
# moment a VNC client connects) -- a confusing delayed death. Catch it here
# with a clear message instead.
shm_probe="/dev/shm/.wl-start-probe.$$"
if ! ( : > "$shm_probe" ) 2>/dev/null; then
    die "/dev/shm is not writable by uid $(id -u) -- POSIX shm (keymaps, wl_shm buffers) cannot work. Reboot the app (the boot repair fixes /dev/shm's mode) or run: sudo chmod 1777 /dev/shm"
fi
rm -f "$shm_probe"

# The same kind of trap for X: wlroots binds the Xwayland display's socket in
# /tmp/.X11-unix, which Linux boot scripts create world-writable and sticky
# (Devuan's x11-common does). Where nothing has, wlroots creates it with the
# session's umask, so a root session left it 0755, and a later session as the
# default user could bind no display at all: wlroots gave up after X0-X32,
# labwc exited with "cannot create xwayland server", and the desktop never
# started. A root session makes the directory 1777 before the compositor
# starts. Another user cannot change a directory root owns, so that session
# stops here with the fix rather than with labwc's exit.
X11_SOCKET_DIR=/tmp/.X11-unix
if [ "$CURRENT_UID" = 0 ]; then
    mkdir -p "$X11_SOCKET_DIR" 2>/dev/null
    chmod 1777 "$X11_SOCKET_DIR" 2>/dev/null
elif [ -d "$X11_SOCKET_DIR" ] && ! [ -w "$X11_SOCKET_DIR" ]; then
    die "$X11_SOCKET_DIR is not writable by uid $CURRENT_UID, so $COMPOSITOR_CMD cannot start Xwayland and would exit -- a root session created it; run: sudo chmod 1777 $X11_SOCKET_DIR"
fi

# Seed a minimal sway config + a wofi-driven app launcher, for anyone who
# sets WAYLAND_COMPOSITOR_CMD=sway instead of the labwc default below. sway
# has no menu system at all; everything is a keybinding, dispatched straight
# to sway's own IPC socket instead of round-tripping through a spawned
# XML-emitting subprocess whose output gets parsed at click time -- useful
# if labwc's menu ever regresses again, but it also tiles by default (every
# window fullscreen until floated), which is why labwc is the default now.
# $mod is Alt (Mod1): Control has to stay free for in-terminal Ctrl combos
# (Ctrl+C etc.), so a window-manager modifier that also used Control would
# collide with those.
if [ "$AOK_DESKTOP" = sway ]; then
    mkdir -p "$HOME/.config/sway"
    # Regenerated every session start, like list-apps.sh below -- it's a
    # generated helper, not something a user would hand-edit. Builds a
    # Name\tExec map from the same apt-installed .desktop files (single awk
    # pass per file, no XML escaping needed since wofi's dmenu mode is just
    # plain-text lines in and a plain-text choice out).
    cat > "$HOME/.config/sway/app-launcher.sh" <<'APP_LAUNCHER_EOF'
#!/bin/sh
set -u
MAP_FILE="$(mktemp)"
trap 'rm -f "$MAP_FILE"' EXIT INT TERM

# The same entries list-apps.sh offers, overrides included (see there).
local_apps=/usr/local/share/applications
user_apps=$HOME/.local/share/applications
for d in /usr/share/applications "$local_apps" "$user_apps"; do
  for f in "$d"/*.desktop; do
    [ -f "$f" ] || continue
    b=${f##*/}
    [ "$d" != "$user_apps" ] && [ -f "$user_apps/$b" ] && continue
    [ "$d" = /usr/share/applications ] && [ -f "$local_apps/$b" ] && continue
    awk '
        /^NoDisplay=true/ { hidden=1 }
        /^Hidden=true/ { hidden=1 }
        /^Name=/ && name == "" { name = substr($0, 6) }
        /^Exec=/ && execline == "" { execline = substr($0, 6) }
        END {
            if (hidden || name == "" || execline == "") exit
            # The second copy of this rule -- list-apps.sh has the same one,
            # with the reasoning (manages_the_desktop). An entry asking a file
            # manager to take over the desktop, or to configure the desktop it
            # would be managing, cannot work when the compositor draws it:
            # pcmanfm answers --desktop-pref with "Desktop manager is not
            # active." and --desktop would fight sway for the desktop.
            if (execline ~ /(^| )--(desktop-pref|wallpaper-mode|set-wallpaper|desktop)( |=|$)/) exit
            gsub(/%[a-zA-Z]/, "", execline)
            printf "%s\t%s\n", name, execline
        }
    ' "$f" >> "$MAP_FILE"
  done
done

[ -s "$MAP_FILE" ] || exit 0

chosen_name="$(cut -f1 "$MAP_FILE" | wofi --show dmenu --prompt Applications)"
[ -n "$chosen_name" ] || exit 0

exec_line="$(awk -F'\t' -v name="$chosen_name" '$1 == name { print $2; exit }' "$MAP_FILE")"
[ -n "$exec_line" ] || exit 0

exec sh -c "$exec_line"
APP_LAUNCHER_EOF
    chmod +x "$HOME/.config/sway/app-launcher.sh"
fi
if [ "$AOK_DESKTOP" = sway ] && [ ! -f "$HOME/.config/sway/config" ]; then
    cat > "$HOME/.config/sway/config" <<SWAY_CONFIG_EOF
# Generated by start-wayland.sh (first run only -- hand edits are preserved
# on later sessions since this file is only seeded when missing).
set \$mod Mod1

# No bar: single-output headless desktop, nothing to show a taskbar for,
# and it avoids needing i3status/swaybar installed at all.

# Unlike labwc, sway does not auto-enable a headless output that has no
# matching config: confirmed on-device, sway logged "Could not find config
# for output HEADLESS-1" and then never created wayland-0 at all (our own
# 10s startup timeout was what actually killed the session, not sway
# itself dying). The wildcard matches whatever wlroots names the sole
# headless output, so this doesn't depend on that name staying "HEADLESS-1"
# across wlroots/sway versions. 1280x720 matches wlroots' headless-backend
# default, i.e. no different from what labwc got for free.
output * mode 1280x720

# sway tiles by default (i3-style): with only one window open, a tiling
# layout stretches it to fill the entire output -- confirmed on-device,
# foot opened full-screen with no way to resize it. labwc was a floating
# compositor (normal desktop-style windows), so this is a real paradigm
# difference, not a bug. Float everything instead so windows open at their
# own natural/preferred size and get a draggable border for resizing.
# Deliberately NOT relying on floating_modifier+drag (the usual i3/sway
# way to move/resize by holding \$mod anywhere on the window) -- there's no
# way to forward "hold Alt continuously while touch-dragging" through this
# applet's input model (Alt is only ever sent as part of one atomic
# UIKeyCommand action, never as a held state alongside a separate pointer
# gesture) -- so plain border-dragging, which needs no modifier at all, is
# what actually has to work here.
for_window [app_id=".*"] floating enable
for_window [class=".*"] floating enable

bindsym \$mod+Return exec foot
bindsym \$mod+d exec $HOME/.config/sway/app-launcher.sh
bindsym \$mod+Shift+q kill
bindsym \$mod+Shift+r reload
# No confirmation dialog (sway's default binds this through swaynag) --
# keeps it a single reliable keypress instead of needing a second,
# precisely-targeted click on a popup over VNC.
bindsym \$mod+Shift+e exec swaymsg exit
SWAY_CONFIG_EOF
fi

# Seed labwc's setup: without this, labwc falls back to its compiled-in
# root menu, which is just "Reconfigure"/"Exit" and gives no way to launch
# anything beyond the one foot window this script starts. The
# "Applications" entry is a labwc pipemenu -- labwc runs list-apps.sh and
# expects an <openbox_pipe_menu> XML fragment on stdout, regenerated every
# time that submenu opens -- so whatever gets apt-installed later shows up
# automatically, no menu.xml edits needed.
#
# Root-caused on-device (m4pt, /tmp/ish-wayland-debug.log): labwc enforces a
# hard PIPEMENU_TIMEOUT_IN_MS of 4000ms (src/menu/menu.c upstream) and kills
# the pipemenu process if it's not done by then -- "[pipemenu N] timeout
# reached, killing list-apps.sh". The one-awk-process-per-.desktop-file
# version (already a big improvement over the original ~9-forks-per-file
# version, and what fixed a real kernel signal/poll deadlock its fork/exit
# burst triggered, see project memory project_signalfd_sighand_poll_
# deadlock.md) still ran fine standalone (~0.35s for 11 files) but
# apparently not always fast enough once labwc's own event loop is also
# busy servicing the click that opened the menu. A single awk invocation
# processing every .desktop file in one pass (FNR==1 marks a new file,
# flushing the previous one's entry) cuts this to exactly one `sh` + one
# `awk` process total regardless of app count, measured at ~0.065s for the
# same 11 files (~5x faster, far more consistent run to run) -- comfortably
# inside the timeout even under load. Also switched Execute's command from
# a nested <command> child element to the command="" attribute form, matching
# every example in labwc's own docs (both plain and forms of "&", "<", ">",
# and now quote marks too need escaping in attribute values, unlike element
# text).
#
# Also root-caused on-device: btop/htop/neovim/mc/mcedit installed but not
# launching from the menu at all. Their .desktop files (unlike foot's) all
# declare Terminal=true -- the freedesktop spec's way of saying "this is a
# TUI app, wrap it in a terminal emulator before running it", since a bare
# curses/ncurses program has no pty/window of its own to render into.
# list-apps.sh was ignoring that field entirely and running the raw Exec=
# command directly, so anything with Terminal=true silently did nothing
# (no error, no output, since there was no terminal for one to appear in).
# Fixed by parsing Terminal=true and prefixing "foot " onto the command in
# that case; confirmed on-device that `foot btop` spawns a real foot window
# with btop correctly attached to its own pty.
#
# The Applications menu is grouped into submenus by each program's first
# freedesktop main category (Games, Graphics, Internet, ...), sorted by name.
# One flat list outgrew the screen: labwc menus do not scroll, and with the
# extras installed only 29 of 96 entries fitted a 720-pixel desktop
# (sgt-puzzles alone adds about 40). So a submenu holds at most 24 entries, and a
# longer group is split into parts named by their first letters, "Games (A–L)".
# Still one awk process for every .desktop file. Only the [Desktop Entry]
# group is read (a [Desktop Action] group has its own Name= and Exec=), and
# entries that are not applications, or that OnlyShowIn/NotShowIn keep off this
# desktop, are left out.
if [ "$AOK_DESKTOP" = labwc ]; then
    mkdir -p "$HOME/.config/labwc"
    # Regenerated on every session start (not gated on "doesn't already
    # exist" like menu.xml below) -- it's a generated helper, not something
    # a user would hand-edit, and it needs to actually pick up fixes.
    cat > "$HOME/.config/labwc/list-apps.sh" <<'LIST_APPS_EOF'
#!/bin/sh
# The menu's entries, by the freedesktop rule: a file of the same name in
# /usr/local/share/applications or ~/.local/share/applications replaces the
# packaged one (the user's above both), and entries only there count too.
# setup-games.sh hides a game whose data is not installed that way -- Chocolate
# Heretic, Hexen and Strife need commercial IWADs, and without them each one
# opened a "No IWAD was found" error box. No forks: this has labwc's 4 s.
local_apps=/usr/local/share/applications
user_apps=$HOME/.local/share/applications
set --
for d in /usr/share/applications "$local_apps" "$user_apps"; do
    for f in "$d"/*.desktop; do
        [ -f "$f" ] || continue
        b=${f##*/}
        [ "$d" != "$user_apps" ] && [ -f "$user_apps/$b" ] && continue
        [ "$d" = /usr/share/applications ] && [ -f "$local_apps/$b" ] && continue
        set -- "$@" "$f"
    done
done
echo '<openbox_pipe_menu>'
# The panel's switch (panel.sh), labelled by whether waybar is running now.
if command -v waybar >/dev/null 2>&1; then
    if pgrep -u "$(id -u)" -x waybar >/dev/null 2>&1; then panel_label="Hide Panel"; else panel_label="Show Panel"; fi
    printf '<item label="%s"><action name="Execute" command="%s/.config/labwc/panel.sh toggle"/></item>\n<separator/>\n' \
        "$panel_label" "$HOME"
fi
if [ $# -gt 0 ]; then
    awk -v max_items=24 '
        function xml(s) {
            gsub(/&/, "\\&amp;", s); gsub(/</, "\\&lt;", s); gsub(/>/, "\\&gt;", s); gsub(/"/, "\\&quot;", s)
            return s
        }
        # Exec= without its field codes (%f, %U, ...), and %% as %.
        function strip_field_codes(s,    out, i, c) {
            out = ""
            while ((i = index(s, "%")) > 0) {
                out = out substr(s, 1, i - 1)
                c = substr(s, i + 1, 1)
                if (c == "%") out = out "%"
                s = substr(s, i + 2)
            }
            return out s
        }
        # An entry that asks a file manager to take over the desktop, or to
        # configure the desktop it would be managing. labwc draws the desktop
        # here and nothing runs `pcmanfm --desktop`, so these cannot work:
        # pcmanfm answers --desktop-pref, --wallpaper-mode and --set-wallpaper
        # with a modal "Desktop manager is not active." (src/pcmanfm.c), and
        # --desktop itself would try to take the desktop labwc is drawing.
        #
        # Neither existing filter catches it. Its Categories carry
        # DesktopSettings, but dropping that whole category would take
        # lxappearance with it, which sets the GTK theme and does work here;
        # and its NotShowIn names GNOME, XFCE, KDE and MATE -- no wlroots
        # compositor -- so the standard key says nothing about this desktop.
        # setup-wayland-extras.sh installs pcmanfm in its "tools" set, so the
        # entry is present on any root that ran it.
        function manages_the_desktop(cmd) {
            return cmd ~ /(^| )--(desktop-pref|wallpaper-mode|set-wallpaper|desktop)( |=|$)/
        }
        # Whether a ;-separated OnlyShowIn/NotShowIn list names this desktop.
        function names_this_desktop(list,    n, i, names) {
            n = split(list, names, ";")
            for (i = 1; i <= n; i++)
                if (names[i] != "" && (names[i] in this_desktop)) return 1
            return 0
        }
        function section_for(categories,    n, i, names) {
            n = split(categories, names, ";")
            for (i = 1; i <= n; i++)
                if (names[i] in section_of) return section_of[names[i]]
            return "Other"
        }
        # A part of a split submenu is named by the first letters of its first
        # and last entries, when both are plain letters or digits.
        function initial(s,    c) {
            c = substr(s, 1, 1)
            return c ~ /^[A-Za-z0-9]$/ ? toupper(c) : ""
        }
        function finish(    s, n) {
            if (type != "Application" || hidden || name == "" || execline == "") return
            if (only_show_in != "" && !names_this_desktop(only_show_in)) return
            if (not_show_in != "" && names_this_desktop(not_show_in)) return
            if (manages_the_desktop(execline)) return
            execline = strip_field_codes(execline)
            if (terminal == "true") execline = "foot " execline
            s = section_for(categories)
            n = ++count[s]
            label[s, n] = name
            command[s, n] = execline
        }
        BEGIN {
            section_of["AudioVideo"] = "Multimedia"; section_of["Audio"] = "Multimedia"
            section_of["Video"] = "Multimedia"; section_of["Development"] = "Development"
            section_of["Education"] = "Education"; section_of["Game"] = "Games"
            section_of["Graphics"] = "Graphics"; section_of["Network"] = "Internet"
            section_of["Office"] = "Office"; section_of["Science"] = "Science"
            section_of["Settings"] = "Settings"; section_of["System"] = "System"
            section_of["Utility"] = "Accessories"
            sections = split("Accessories Development Education Games Graphics Internet Multimedia Office Science Settings System Other", section_order, " ")
            desktops = ENVIRON["XDG_CURRENT_DESKTOP"]
            if (desktops == "") desktops = "labwc:wlroots"
            n = split(desktops, names, ":")
            for (i = 1; i <= n; i++) this_desktop[names[i]] = 1
        }
        FNR == 1 {
            if (NR > 1) finish()
            group = ""; type = ""; hidden = 0; name = ""; execline = ""; terminal = ""
            categories = ""; only_show_in = ""; not_show_in = ""
        }
        { sub(/\r$/, "") }
        /^\[/ { group = $0; next }
        group != "[Desktop Entry]" { next }
        /^Type=/ { type = substr($0, 6) }
        /^NoDisplay=true/ { hidden = 1 }
        /^Hidden=true/ { hidden = 1 }
        /^Name=/ && name == "" { name = substr($0, 6) }
        /^Exec=/ && execline == "" { execline = substr($0, 6) }
        /^Terminal=true/ { terminal = "true" }
        /^Categories=/ { categories = substr($0, 12) }
        /^OnlyShowIn=/ { only_show_in = substr($0, 12) }
        /^NotShowIn=/ { not_show_in = substr($0, 11) }
        END {
            if (NR > 0) finish()
            menus = 0
            for (o = 1; o <= sections; o++) {
                s = section_order[o]
                n = count[s] + 0
                if (n == 0) continue
                # Insertion sort by name, ignoring case.
                for (i = 2; i <= n; i++) {
                    l = label[s, i]; c = command[s, i]; key = tolower(l)
                    for (j = i - 1; j >= 1 && tolower(label[s, j]) > key; j--) {
                        label[s, j + 1] = label[s, j]; command[s, j + 1] = command[s, j]
                    }
                    label[s, j + 1] = l; command[s, j + 1] = c
                }
                parts = int((n + max_items - 1) / max_items)
                per_part = int((n + parts - 1) / parts)
                # Parts are named by letters only when every part gets a
                # different name that way; otherwise they are numbered.
                numbered = 0
                for (p = 1; p <= parts && parts > 1; p++) {
                    first = (p - 1) * per_part + 1
                    last = first + per_part - 1
                    if (last > n) last = n
                    a = initial(label[s, first]); b = initial(label[s, last])
                    part_name[p] = a == b ? a : a "–" b
                    if (a == "" || b == "") numbered = 1
                    for (q = 1; q < p; q++)
                        if (part_name[q] == part_name[p]) numbered = 1
                }
                for (p = 1; p <= parts; p++) {
                    first = (p - 1) * per_part + 1
                    last = first + per_part - 1
                    if (last > n) last = n
                    title = s
                    if (parts > 1) title = s " (" (numbered ? p " of " parts : part_name[p]) ")"
                    printf "<menu id=\"apps-%d\" label=\"%s\">\n", ++menus, xml(title)
                    for (i = first; i <= last; i++)
                        printf "<item label=\"%s\"><action name=\"Execute\" command=\"%s\"/></item>\n", xml(label[s, i]), xml(command[s, i])
                    print "</menu>"
                }
            }
        }
    ' "$@"
fi
echo '</openbox_pipe_menu>'
LIST_APPS_EOF
    chmod +x "$HOME/.config/labwc/list-apps.sh"

    # The panel's switch, behind the Applications menu's Show/Hide Panel. The
    # choice lasts: hiding it leaves panel-off behind, and a session started with
    # that file present does not start waybar.
    cat > "$HOME/.config/labwc/panel.sh" <<'PANEL_EOF'
#!/bin/sh
off="$HOME/.config/labwc/panel-off"
running() { pgrep -u "$(id -u)" -x waybar >/dev/null 2>&1; }
case "${1:-toggle}" in
    toggle)
        if running; then
            pkill -u "$(id -u)" -x waybar
            : > "$off"
        else
            rm -f "$off"
            exec waybar
        fi
        ;;
    start)
        [ -e "$off" ] && exit 0
        running && exit 0
        exec waybar
        ;;
esac
PANEL_EOF
    chmod +x "$HOME/.config/labwc/panel.sh"

    # The launcher behind the root menu's Launcher item and Alt+Shift+D: fuzzel,
    # which setup-wayland-extras.sh installs with its tools, or else wofi, which
    # setup-wayland.sh installs. Both search the installed applications and start
    # the one picked, in foot when it is a terminal program.
    cat > "$HOME/.config/labwc/launcher.sh" <<'LAUNCHER_EOF'
#!/bin/sh
if command -v fuzzel >/dev/null 2>&1; then
    exec fuzzel --terminal=foot
elif command -v wofi >/dev/null 2>&1; then
    exec wofi --show drun --term=foot
fi
LAUNCHER_EOF
    chmod +x "$HOME/.config/labwc/launcher.sh"
fi

# menu.xml and rc.xml below are defaults: written when missing, and never over a
# file the user has edited. A default that an earlier version of this script
# wrote and nobody has touched since is replaced by the current one, though, so
# additions such as the Launcher reach desktops set up before them. Those old
# defaults are recognised by cksum, computed with the home directory written as
# ~ because menu.xml names files in it; "crc:size" values in $2. The new content
# is read from stdin.
config_cksum() {
    awk -v home="$HOME" '
        home != "" { while ((i = index($0, home)) > 0) $0 = substr($0, 1, i - 1) "~" substr($0, i + length(home)) }
        { print }' "$1" | cksum | awk '{ print $1 ":" $2 }'
}
install_default_config() {
    config_target="$1"
    config_new="$config_target.new.$$"
    # Temporaries earlier sessions left behind: every default user's session
    # left one while smallclue's mv refused `mv -f` (fixed in smallclue).
    for config_stale in "$config_target".new.*; do
        [ -e "$config_stale" ] && rm -f "$config_stale"
    done
    cat > "$config_new" || { rm -f "$config_new"; return 1; }
    if [ -e "$config_target" ]; then
        config_current="$(config_cksum "$config_target")"
        config_replace=0
        if [ -n "$config_current" ] && [ "$config_current" != "$(config_cksum "$config_new")" ]; then
            for config_old in $2; do
                [ "$config_old" = "$config_current" ] && config_replace=1
            done
        fi
        if [ "$config_replace" = 0 ]; then
            rm -f "$config_new"
            return 0
        fi
        log "replacing $config_target, an unedited default from an earlier version of this script, with the current one"
    fi
    mv -f "$config_new" "$config_target"
}

if [ "$AOK_DESKTOP" = labwc ]; then
    # The one earlier default menu.xml (6b57294a to 583ce462).
    install_default_config "$HOME/.config/labwc/menu.xml" "2481222110:490" <<MENU_EOF
<?xml version="1.0" encoding="UTF-8"?>
<openbox_menu>
  <menu id="root-menu" label="Root">
    <item label="New Terminal">
      <action name="Execute"><command>foot</command></action>
    </item>
    <item label="Launcher">
      <action name="Execute"><command>$HOME/.config/labwc/launcher.sh</command></action>
    </item>
    <separator/>
    <menu id="apps-pipemenu" label="Applications" execute="$HOME/.config/labwc/list-apps.sh"/>
    <separator/>
    <item label="Reconfigure">
      <action name="Reconfigure"/>
    </item>
    <item label="Exit">
      <action name="Exit"/>
    </item>
  </menu>
</openbox_menu>
MENU_EOF
fi

# labwc's stock look is plain gray -- give it a theme instead, in the same
# dark/mint palette as the app's own Workspace "Graphite" theme (see
# ISHWorkspaceBuiltInThemePalette in WorkspaceViewController.m) so the
# Display applet's windows and menu feel like part of the same app rather
# than a bare stock Linux desktop dropped in unstyled. labwc reads themes
# via the same lookup XDG-desktop themes use (openbox-compatible themerc
# format): $XDG_DATA_HOME/themes/<name>/openbox-3/themerc, referenced by
# name from rc.xml's <theme> below. Unrecognized/misspelled keys are just
# ignored by labwc rather than failing to parse, so this is low-risk to
# get slightly wrong.
if [ "$AOK_DESKTOP" = labwc ] && [ ! -f "$HOME/.local/share/themes/iSH-Workspace/openbox-3/themerc" ]; then
    mkdir -p "$HOME/.local/share/themes/iSH-Workspace/openbox-3"
    cat > "$HOME/.local/share/themes/iSH-Workspace/openbox-3/themerc" <<'THEMERC_EOF'
window.active.title.bg.color: #2C374C
window.active.label.text.color: #F0F4FF
window.active.border.color: #70E8CC
window.active.button.unpressed.fg.color: #F0F4FF
window.active.border.width: 2
window.inactive.title.bg.color: #1A2232
window.inactive.label.text.color: #B6C2DA
window.inactive.border.color: #1A2232
window.inactive.button.unpressed.fg.color: #B6C2DA
window.inactive.border.width: 1
menu.items.bg.color: #0F1420
menu.items.text.color: #F0F4FF
menu.items.active.bg.color: #2C374C
menu.items.active.text.color: #70E8CC
menu.title.bg.color: #2C374C
menu.title.text.color: #F0F4FF
menu.border.color: #70E8CC
menu.border.width: 1
THEMERC_EOF
fi

# rc.xml: without this, labwc runs on its compiled-in defaults (fine, but
# gives up the theme above -- it's only referenced here -- and there's no
# keyboard equivalent of the menu actions, matching what sway's Alt
# keybindings already gave that setup). Alt+Return/Tab/Shift+Q/Shift+R/
# Shift+E mirror the same actions the right-click menu already offers,
# using the same Alt-based scheme DisplayRFBView.m forwards (Control stays
# free for in-terminal Ctrl combos). Alt+Shift+D opens the launcher, sway's
# $mod+d with Shift added: plain Alt+D would take kill-word from readline in
# every terminal. labwc expands ~ in an Execute command.
#
# Four desktops, since labwc starts with one. Ctrl+Alt+Left/Right switches
# (labwc's own convention; Cmd+arrows belong to Workspace's Desktops around
# the window, and Alt+digit is readline's digit-argument in every terminal),
# with Shift taking the focused window along, and Ctrl+Alt+1-4 goes straight
# to one.
if [ "$AOK_DESKTOP" = labwc ]; then
    # The earlier defaults: 2c7e6aa7 to 02cfc17f, 6fc49f06 (it maximized every
    # window), c862512d to 583ce462, and the one-desktop default before the
    # desktop keys.
    install_default_config "$HOME/.config/labwc/rc.xml" "1331651033:528 1672038269:1609 240916693:1110 3367275241:1236" <<'RC_XML_EOF'
<?xml version="1.0"?>
<labwc_config>
  <theme>
    <name>iSH-Workspace</name>
  </theme>
  <!-- Deliberately NO windowRules forcing Maximize: a wildcard Maximize-on-
       open rule briefly lived here to hide the visual gap a small floating
       window left on the stretched landscape canvas, but per-orientation
       output resizing (DisplayRFBClient's SetDesktopSize support, see
       docs/wayland_rotation_resize_plan.md) fixed the aspect mismatch that
       made that gap confusing, and forcing EVERY app fullscreen was wrong
       for normal desktop use (user-reported). Windows open floating at
       their natural size, like any stock labwc desktop. -->
  <desktops number="4" popupTime="500" />
  <keyboard>
    <keybind key="A-Return">
      <action name="Execute"><command>foot</command></action>
    </keybind>
    <keybind key="A-Tab">
      <action name="NextWindow"/>
    </keybind>
    <keybind key="A-S-q">
      <action name="Close"/>
    </keybind>
    <keybind key="A-S-r">
      <action name="Reconfigure"/>
    </keybind>
    <keybind key="A-S-e">
      <action name="Exit"/>
    </keybind>
    <keybind key="A-S-d">
      <action name="Execute"><command>~/.config/labwc/launcher.sh</command></action>
    </keybind>
    <keybind key="C-A-Left">
      <action name="GoToDesktop" to="left" wrap="yes"/>
    </keybind>
    <keybind key="C-A-Right">
      <action name="GoToDesktop" to="right" wrap="yes"/>
    </keybind>
    <keybind key="C-A-S-Left">
      <action name="SendToDesktop" to="left" wrap="yes"/>
    </keybind>
    <keybind key="C-A-S-Right">
      <action name="SendToDesktop" to="right" wrap="yes"/>
    </keybind>
    <keybind key="C-A-1">
      <action name="GoToDesktop" to="1"/>
    </keybind>
    <keybind key="C-A-2">
      <action name="GoToDesktop" to="2"/>
    </keybind>
    <keybind key="C-A-3">
      <action name="GoToDesktop" to="3"/>
    </keybind>
    <keybind key="C-A-4">
      <action name="GoToDesktop" to="4"/>
    </keybind>
  </keyboard>
</labwc_config>
RC_XML_EOF
fi

# Wayfire's config (setup-wayfire.sh), on its first session: the plugins that
# make it worth running on the GPU -- wobbly windows, the desktop cube, expo,
# animations -- and the same Alt keys as the labwc desktop above, since
# Wayfire's own defaults are on the Super key, which the app keeps for itself.
# wf-shell's panel and wallpaper start through the autostart plugin; the first
# terminal is started below, as for every desktop. Written only when missing:
# edit it freely, or with wcm (the Wayfire Config Manager).
if [ "$AOK_DESKTOP" = wayfire ]; then
    # Earlier defaults, from before the panel moved out of autostart.
    install_default_config "$HOME/.config/wayfire.ini" "2333436050:1405 3256557696:1516" <<'WAYFIRE_INI_EOF'
# Written by /AOK/tools/start-wayland.sh on the first Wayfire session. Edit
# freely, or with wcm (the Wayfire Config Manager): it is only written when
# there is none. Keys use Alt, as the labwc desktop does: the app keeps the
# Command (Super) key for itself.
[core]
plugins = alpha animate autostart command cube decoration expo fast-switcher foreign-toplevel grid gtk-shell move oswitch place resize switcher vswitch wayfire-shell window-rules wm-actions wobbly zoom
xwayland = true
vwidth = 4
vheight = 1
close_top_view = <alt> <shift> KEY_Q

[autostart]
# wf-shell's wallpaper. Its panel is started by start-wayland.sh once the
# desktop is up: one started this early was never drawn.
autostart_wf_shell = false
background = wf-background

[command]
binding_terminal = <alt> KEY_ENTER
command_terminal = foot
binding_launcher = <alt> <shift> KEY_D
command_launcher = wofi --show drun
binding_logout = <alt> <shift> KEY_E
command_logout = wayland-logout

[switcher]
next_view = <alt> KEY_TAB
prev_view = <alt> <shift> KEY_TAB

[vswitch]
binding_left = <ctrl> <alt> KEY_LEFT
binding_right = <ctrl> <alt> KEY_RIGHT
with_win_left = <ctrl> <alt> <shift> KEY_LEFT
with_win_right = <ctrl> <alt> <shift> KEY_RIGHT

[expo]
# Every desktop at once; click one to go there.
toggle = <alt> <shift> KEY_W

[cube]
# Hold Ctrl+Alt and drag to turn the desktops as a cube.
activate = <ctrl> <alt> BTN_LEFT

[move]
activate = <alt> BTN_LEFT

[resize]
activate = <alt> BTN_RIGHT

[wobbly]
friction = 3.0
spring_k = 8.0
WAYFIRE_INI_EOF
    # wf-shell's panel hides itself until the pointer reaches the top edge,
    # once the wayfire-shell plugin gives it the means to; on a touch screen
    # that is a panel nobody finds. Kept in view here.
    install_default_config "$HOME/.config/wf-shell.ini" "" <<'WF_SHELL_INI_EOF'
# Written by /AOK/tools/start-wayland.sh on the first Wayfire session; edit
# freely. Options: wf-shell's own documentation, or wcm.
[panel]
autohide = false
position = top
WF_SHELL_INI_EOF
fi

# Xfce's labwc (setup-xfce.sh): its own config directory, as startxfce4 makes
# one. Xfce's sample rc.xml binds the Super key, which the app keeps, so this
# one carries the labwc desktop's Alt keys instead, with Xfce's terminal and
# app finder. And its environment file: Xfce's sample sets the keyboard layout
# to "en", which is no layout -- startxfce4 rewrites it from the console's, and
# labwc otherwise logs a keymap failure and falls back to "us".
if [ "$AOK_DESKTOP" = xfce ]; then
    mkdir -p "$HOME/.config/xfce4/labwc"
    xfce_term=xfce4-terminal
    command -v "$xfce_term" >/dev/null 2>&1 || xfce_term=foot
    install_default_config "$HOME/.config/xfce4/labwc/rc.xml" "" <<XFCE_RC_XML_EOF
<?xml version="1.0"?>
<!-- Written by /AOK/tools/start-wayland.sh on the first Xfce session; edit
     freely. labwc is Xfce's compositor here; Xfce draws the panel and desktop. -->
<labwc_config>
  <theme>
    <name>Adwaita</name>
    <cornerRadius>8</cornerRadius>
    <font name="sans" size="10"/>
  </theme>
  <desktops number="4" />
  <keyboard>
    <keybind key="A-Return">
      <action name="Execute"><command>$xfce_term</command></action>
    </keybind>
    <keybind key="A-Tab">
      <action name="NextWindow"/>
    </keybind>
    <keybind key="A-S-q">
      <action name="Close"/>
    </keybind>
    <keybind key="A-S-d">
      <action name="Execute"><command>xfce4-appfinder</command></action>
    </keybind>
    <keybind key="A-S-e">
      <action name="Exit"/>
    </keybind>
    <keybind key="C-A-Left">
      <action name="GoToDesktop" to="left" wrap="yes"/>
    </keybind>
    <keybind key="C-A-Right">
      <action name="GoToDesktop" to="right" wrap="yes"/>
    </keybind>
    <keybind key="C-A-S-Left">
      <action name="SendToDesktop" to="left" wrap="yes"/>
    </keybind>
    <keybind key="C-A-S-Right">
      <action name="SendToDesktop" to="right" wrap="yes"/>
    </keybind>
  </keyboard>
</labwc_config>
XFCE_RC_XML_EOF
    if [ ! -e "$HOME/.config/xfce4/labwc/environment" ]; then
        if [ -r /usr/share/xfce4/labwc/labwc-environment ]; then
            sed 's/^XKB_DEFAULT_LAYOUT=en$/XKB_DEFAULT_LAYOUT=us/' \
                /usr/share/xfce4/labwc/labwc-environment > "$HOME/.config/xfce4/labwc/environment"
        else
            printf 'XKB_DEFAULT_LAYOUT=us\n' > "$HOME/.config/xfce4/labwc/environment"
        fi
    fi
fi

# waybar, when it is installed: a config for labwc on the first session that
# has none (the user's own, config or config.jsonc, always wins). Debian's
# default in /etc/xdg/waybar is written for sway. Under labwc its five sway
# modules find no sway socket and turn themselves off, and its pulseaudio, mpd,
# power-profiles-daemon and media modules want servers these roots do not run.
# This keeps Debian's formats and icons for the modules that have something to
# show here, except two icons: Debian's font package is Font Awesome 4.7, which
# lacks the ethernet and charging-station glyphs Debian's config uses, so those
# are sitemap (U+F0E8) and bolt (U+F0E7), present in 4.7 and in the 7.x Alpine
# and Arch ship. The icons need Font Awesome, which setup-wayland.sh installs
# with waybar.
if [ "$AOK_DESKTOP" = labwc ] && command -v waybar >/dev/null 2>&1 \
        && [ ! -e "$HOME/.config/waybar/config.jsonc" ] && [ ! -e "$HOME/.config/waybar/config" ]; then
    mkdir -p "$HOME/.config/waybar"
    cat > "$HOME/.config/waybar/config.jsonc" <<'WAYBAR_CONFIG_EOF'
// -*- mode: jsonc -*-
// Written by /AOK/tools/start-wayland.sh on the first labwc session.
// Edit freely: it is only written when there is no waybar config yet.
{
    "spacing": 4,
    "modules-left": ["wlr/taskbar"],
    "modules-center": ["clock"],
    "modules-right": ["cpu", "memory", "disk", "network", "battery", "tray"],
    "wlr/taskbar": {
        "format": "{icon} {title:.24}",
        "icon-size": 16,
        "on-click": "activate",
        "on-click-middle": "close"
    },
    "clock": {
        "tooltip-format": "<big>{:%Y %B}</big>\n<tt><small>{calendar}</small></tt>",
        "format-alt": "{:%Y-%m-%d}"
    },
    "cpu": {
        "format": "{usage}% ",
        "tooltip": false
    },
    "memory": {
        "format": "{}% "
    },
    "disk": {
        "format": "{percentage_used}% ",
        "path": "/"
    },
    "network": {
        "format-wifi": "{essid} ({signalStrength}%) ",
        "format-ethernet": "{ipaddr}/{cidr} ",
        "tooltip-format": "{ifname} via {gwaddr} ",
        "format-linked": "{ifname} (No IP) ",
        "format-disconnected": "Disconnected ⚠",
        "format-alt": "{ifname}: {ipaddr}/{cidr}"
    },
    "battery": {
        "states": {
            "warning": 30,
            "critical": 15
        },
        "format": "{capacity}% {icon}",
        "format-full": "{capacity}% {icon}",
        "format-charging": "{capacity}% ",
        "format-plugged": "{capacity}% ",
        "format-icons": ["", "", "", "", ""]
    },
    "tray": {
        "spacing": 10
    }
}
WAYBAR_CONFIG_EOF
fi
# waybar as Wayfire's panel (AOK_WAYFIRE_PANEL=waybar) has a config of its own,
# so a labwc config beside it is left alone: the same modules, plus what labwc
# has elsewhere -- an Apps button that opens wofi's application list, since
# Wayfire's desktop has no menu of its own. Written when missing; edit freely.
WAYFIRE_WAYBAR_CONFIG="$HOME/.config/waybar/wayfire.jsonc"
if [ "$AOK_DESKTOP" = wayfire ] && [ "$AOK_WAYFIRE_PANEL" = waybar ] \
        && command -v waybar >/dev/null 2>&1 && [ ! -e "$WAYFIRE_WAYBAR_CONFIG" ]; then
    mkdir -p "$HOME/.config/waybar"
    cat > "$WAYFIRE_WAYBAR_CONFIG" <<'WAYFIRE_WAYBAR_EOF'
// -*- mode: jsonc -*-
// Written by /AOK/tools/start-wayland.sh: waybar as Wayfire's panel
// (select-desktop.sh --panel waybar). Edit freely: it is only written when
// missing.
{
    "spacing": 4,
    "modules-left": ["custom/apps", "wlr/taskbar"],
    "modules-center": ["clock"],
    "modules-right": ["cpu", "memory", "battery", "tray"],
    "custom/apps": {
        "format": " Apps",
        "tooltip": false,
        "on-click": "pkill -x wofi || wofi --show drun --term=foot"
    },
    "wlr/taskbar": {
        "format": "{icon} {title:.24}",
        "icon-size": 16,
        "on-click": "activate",
        "on-click-middle": "close"
    },
    "clock": {
        "tooltip-format": "<big>{:%Y %B}</big>\n<tt><small>{calendar}</small></tt>",
        "format-alt": "{:%Y-%m-%d}"
    },
    "cpu": {
        "format": "{usage}% ",
        "tooltip": false
    },
    "memory": {
        "format": "{}% "
    },
    "battery": {
        "states": {
            "warning": 30,
            "critical": 15
        },
        "format": "{capacity}% {icon}",
        "format-full": "{capacity}% {icon}",
        "format-charging": "{capacity}% ",
        "format-plugged": "{capacity}% ",
        "format-icons": ["", "", "", "", ""]
    },
    "tray": {
        "spacing": 10
    }
}
WAYFIRE_WAYBAR_EOF
fi
# And a style that puts the text font first. waybar's default style.css asks for
# FontAwesome before any text font. Alpine and Arch ship Font Awesome 7 with a
# fontconfig alias from that name, and Font Awesome 6 and later draw letters and
# digits as icons at their ASCII code points, so the whole panel read in icon
# capitals with no punctuation: the clock said 1623, the address 192168815124.
# Naming Font Awesome after sans-serif did not help, since the alias binds it
# more strongly than sans-serif's own match; with sans-serif alone the text is
# the text font, and the icons still come from Font Awesome, the one font with
# glyphs at their code points. Debian's Font Awesome 4.7 has no letters, so the
# panel there looks the same either way. Written only when there is no user
# style; the system style is imported, so it keeps up with the installed waybar.
if { [ "$AOK_DESKTOP" = labwc ] || { [ "$AOK_DESKTOP" = wayfire ] && [ "$AOK_WAYFIRE_PANEL" = waybar ]; }; } \
        && command -v waybar >/dev/null 2>&1 \
        && [ ! -e "$HOME/.config/waybar/style.css" ] && [ -r /etc/xdg/waybar/style.css ]; then
    mkdir -p "$HOME/.config/waybar"
    cat > "$HOME/.config/waybar/style.css" <<'WAYBAR_STYLE_EOF'
/* Written by /AOK/tools/start-wayland.sh when there was no waybar style yet:
   waybar's own style, with the text font instead of Font Awesome first (Font
   Awesome 6 and later would draw every letter as an icon). Edit freely. */
@import url("file:///etc/xdg/waybar/style.css");

* {
    font-family: sans-serif;
}
WAYBAR_STYLE_EOF
fi

# Captures the PID of the actual program, not a `cmd | tee` pipeline's last
# stage -- verified empirically: `$!` after `cmd | tee &` is tee's pid in
# both bash and dash, and killing that pid does NOT kill `cmd` (it keeps
# running with its output silently discarded once tee's read end closes).
# cleanup() below relies on COMPOSITOR_PID/WAYVNC_PID/FOOT_PID actually
# being the real processes to signal them -- with the old `| tee` pipeline
# form, `kill "$COMPOSITOR_PID"` was killing tee, not the compositor, and
# whether the compositor happened to exit anyway depended on it dying from
# the broken pipe (SIGPIPE) or the pty hangup's SIGHUP reaching the whole
# process group by luck -- which apparently differs between labwc and sway,
# since this surfaced as Reconnect/reopen hanging or racing a still-alive
# stale session only after the switch to sway. Routing each process's
# output through its own named FIFO into a separate `tee` reader instead
# means `$!` right after launching the real command (no pipe on that line)
# is correct, fixing this deterministically for every compositor, not just
# whichever one happens to die from a broken pipe. Must be called directly
# (not via `$(...)`), which would put the backgrounded jobs in a subshell
# and reparent them away before this script's own `wait` calls can see them.
# Sets $SPAWN_PID for the caller to read immediately after the call.
spawn_logged() {
    name="$1"; shift
    fifo="$XDG_RUNTIME_DIR/$name.fifo"
    rm -f "$fifo"
    mkfifo "$fifo"
    tee -a "$DEBUG_LOG" >&2 < "$fifo" &
    TEE_PIDS="$TEE_PIDS $!"
    "$@" > "$fifo" 2>&1 &
    SPAWN_PID=$!
}

# Marks when the compositor started, for telling its X display lock from locks
# earlier sessions left behind (see the DISPLAY lookup below).
COMPOSITOR_START_MARK="$XDG_RUNTIME_DIR/compositor-started"
: > "$COMPOSITOR_START_MARK"

# The GPU compositor's environment, for the compositor process alone.
# - Its output buffers come from GBM, which must load zink: Mesa's software
#   fallback for this device reports buffers without a modifier, and wlroots'
#   Vulkan renderer takes none but LINEAR. Mesa picks the driver per program
#   (drirc "dri_driver"), so a private drirc directory -- Mesa's own defaults
#   plus that one rule -- gives zink to the compositor and not to the programs
#   it starts: zink here offers only OpenGL 2.1 and GLES 2.0 (MoltenVK lacks
#   what GL 3 needs), where llvmpipe offers 4.5.
# - Xwayland gets zink the same way, for its glamor: without it Xwayland has
#   no DRI3, and an X11 OpenGL program (Extreme Tux Racer, anything on SFML or
#   GLEW) cannot reach the GPU at all -- it runs in software, or not at all
#   when it asked for zink.
# - LIBGL_ALWAYS_SOFTWARE would put zink on a CPU device; the compositor's
#   children therefore start without it, and Mesa falls back to software GL
#   for them by itself.
wl_gpu_drirc() {
    dir="$XDG_RUNTIME_DIR/drirc.d"
    mkdir -p "$dir" || return 1
    for conf in /usr/share/drirc.d/*.conf; do
        [ -e "$conf" ] && ln -sf "$conf" "$dir/"
    done
    cat > "$dir/99-ish-aok-compositor.conf" <<'DRIRC_EOF'
<?xml version="1.0" standalone="yes"?>
<driconf>
    <device driver="loader" kernel_driver="virtio_gpu">
        <application name="labwc" executable="labwc">
            <option name="dri_driver" value="zink" />
        </application>
        <application name="sway" executable="sway">
            <option name="dri_driver" value="zink" />
        </application>
        <application name="wayfire" executable="wayfire">
            <option name="dri_driver" value="zink" />
        </application>
        <application name="Xwayland" executable="Xwayland">
            <option name="dri_driver" value="zink" />
        </application>
    </device>
</driconf>
DRIRC_EOF
    printf '%s\n' "$dir"
}

# Start the compositor; on the GPU, fall back to software when it cannot
# bring up its output (an allocator or swapchain failure leaves a compositor
# running with nothing to show).
start_compositor() {
    if [ "$WL_GPU_COMPOSITOR" = 1 ] && drirc_dir=$(wl_gpu_drirc); then
        log "starting $COMPOSITOR_CMD (headless, composited on the GPU)"
        log_mark=$(wc -c < "$DEBUG_LOG" 2>/dev/null || echo 0)
        # wlroots' Vulkan renderer, except for Wayfire, which only has GLES:
        # zink on the same Vulkan device, found by the render node's path
        # because the headless backend has no DRM device to offer.
        gpu_renderer="WLR_RENDERER=vulkan"
        [ "$AOK_DESKTOP" = wayfire ] &&
            gpu_renderer="WLR_RENDERER=gles2 WLR_RENDER_DRM_DEVICE=/dev/dri/renderD128"
        spawn_logged compositor "$AOK_ENV" -u LIBGL_ALWAYS_SOFTWARE \
            $gpu_renderer DRIRC_CONFIGDIR="$drirc_dir" $COMPOSITOR_CMD
        COMPOSITOR_PID=$SPAWN_PID
        # The output's first commit, where a failure shows, follows the
        # socket closely: wait for the socket, then a little longer.
        i=0
        while [ $i -lt 100 ]; do
            kill -0 "$COMPOSITOR_PID" 2>/dev/null || break
            ls "$XDG_RUNTIME_DIR"/wayland-* >/dev/null 2>&1 && break
            sleep 0.1
            i=$((i + 1))
        done
        sleep 1.5
        if kill -0 "$COMPOSITOR_PID" 2>/dev/null &&
           ! tail -c +$((log_mark + 1)) "$DEBUG_LOG" 2>/dev/null |
             grep -qE 'failed test|Failed to commit frame|unable to create (allocator|renderer)|Failed to create (renderer|allocator)|Could not initialize'; then
            return 0
        fi
        kill "$COMPOSITOR_PID" 2>/dev/null
        wait "$COMPOSITOR_PID" 2>/dev/null
        [ "$AOK_DESKTOP" = wayfire ] \
            && die "Wayfire could not composite on the GPU (see $DEBUG_LOG) -- 'sudo sh /AOK/tools/select-desktop.sh labwc' goes back to the default desktop"
        log "warning: $COMPOSITOR_CMD could not composite on the GPU -- falling back to software (see $DEBUG_LOG)"
        rm -f "$XDG_RUNTIME_DIR"/wayland-*
        WL_GPU_COMPOSITOR=0
    fi
    log "starting $COMPOSITOR_CMD (headless)"
    spawn_logged compositor $COMPOSITOR_CMD
    COMPOSITOR_PID=$SPAWN_PID
}
start_compositor

# Wait for the compositor to create its Wayland socket rather than a fixed
# sleep -- labwc under JIT on a loaded device can take longer than the ~1s
# it takes on a native host. Discover the actual socket name instead of
# assuming "wayland-0": confirmed on-device (sway 1.10.1) a compositor that
# started and ran perfectly healthy can still name its socket "wayland-1" --
# wl_display_add_socket_auto() increments past "wayland-0" if it can't bind
# that specific name for any reason (even just a stale leftover socket file
# with no live listener), and there's no guarantee it won't happen here too.
# Hardcoding "wayland-0" made this loop wait forever for a file that was
# never going to appear and time out declaring a running compositor "dead".
i=0
WAYLAND_SOCKET_NAME=""
while [ -z "$WAYLAND_SOCKET_NAME" ] && [ $i -lt 100 ]; do
    kill -0 "$COMPOSITOR_PID" 2>/dev/null || die "$COMPOSITOR_CMD exited before creating its Wayland socket -- see $DEBUG_LOG"
    for candidate in "$XDG_RUNTIME_DIR"/wayland-*; do
        [ -S "$candidate" ] || continue
        WAYLAND_SOCKET_NAME=$(basename "$candidate")
        break
    done
    [ -n "$WAYLAND_SOCKET_NAME" ] && break
    sleep 0.1
    i=$((i + 1))
done
[ -n "$WAYLAND_SOCKET_NAME" ] || die "$COMPOSITOR_CMD did not create a wayland-* socket within 10s"
export WAYLAND_DISPLAY="$WAYLAND_SOCKET_NAME"

# If something still sits between this script and the compositor (a wrapper
# that runs its command as a child -- see AOK_ENV), track the compositor
# itself: the X lock names it, and its sockets are the ones looked at below.
_comp_name=${COMPOSITOR_CMD%% *}
_comp_name=${_comp_name##*/}
if [ "$(cat /proc/"$COMPOSITOR_PID"/comm 2>/dev/null)" != "$_comp_name" ]; then
    for _c in /proc/[0-9]*; do
        [ "$(aok_ppid "${_c#/proc/}")" = "$COMPOSITOR_PID" ] || continue
        [ "$(cat "$_c/comm" 2>/dev/null)" = "$_comp_name" ] || continue
        COMPOSITOR_PID=${_c#/proc/}
        break
    done
fi

# The socket *file* existing doesn't mean labwc's event loop is actually
# accepting Wayland client connections yet -- observed on-device: wayvnc
# started immediately after the socket appeared and still hit "failed to
# connect to wayland; no compositor running?" (from wayvnc's own log), i.e.
# a real connect()-level race, not just a slow client. A short grace period
# here is cheaper and more robust than trying to detect "actually ready"
# any more precisely.
sleep 0.3

# ISH_DISPLAY_UI_SCALE: how big things should LOOK, which is a separate question
# from how many pixels the desktop has. The applet asks for the pixel count over
# RFB (SetDesktopSize); this sets the scale the compositor reports to its
# clients, so a desktop with three times the pixels can either fit three times
# as much at a third the size (scale 1) or draw the same layout sharply
# (scale 3). Applied before wayvnc starts, so the first frame it serves is
# already the right shape.
#
# wlr-randr is the standard way to set it on a running wlroots compositor --
# labwc has no output configuration of its own. It arrives in the environment
# rather than as an argument because the applet also launches this through
# `su -`, and the assignment rides inside that command string.
if [ -n "${ISH_DISPLAY_UI_SCALE:-}" ] && [ "$ISH_DISPLAY_UI_SCALE" != "1" ]; then
    if command -v wlr-randr >/dev/null 2>&1; then
        # First column of the first line is the output name (HEADLESS-1 here).
        scale_output=$(wlr-randr 2>/dev/null | awk 'NF && $1 !~ /^ / { print $1; exit }')
        if [ -z "$scale_output" ]; then
            log "warning: wlr-randr listed no output; leaving the scale alone"
        elif wlr-randr --output "$scale_output" --scale "$ISH_DISPLAY_UI_SCALE" >/dev/null 2>&1; then
            log "output $scale_output scaled to ${ISH_DISPLAY_UI_SCALE}x"
        else
            log "warning: wlr-randr could not set scale $ISH_DISPLAY_UI_SCALE on $scale_output"
        fi
    else
        log "warning: a UI scale of ${ISH_DISPLAY_UI_SCALE}x was asked for, but wlr-randr"
        log "         is not installed -- re-run /AOK/tools/setup-wayland.sh to add it."
    fi
fi

# wayvnc gets its own retry loop on top of the grace sleep above: the same
# labwc-not-quite-ready race can still occasionally lose even with the
# sleep (JIT-emulation timing is not consistent run to run), and wayvnc
# fails fast (connection refused) rather than hanging, so a few cheap
# retries turn an intermittent failure into a reliable success instead of a
# full die()/Reconnect round trip. foot is deliberately started only AFTER
# this loop confirms wayvnc is actually listening -- that's empirical proof
# labwc is truly ready, which sidesteps the same race for foot too rather
# than needing a second, duplicated retry loop (found on-device: giving
# only wayvnc a retry loop while starting foot at the same fixed point as
# before just moved the race onto foot -- it died with the identical
# "no compositor running" race on its one and only attempt).
wayvnc_is_listening() {
    kill -0 "$WAYVNC_PID" 2>/dev/null \
        && awk -v port="$hex_port" '$2 ~ (":" port "$") && $4 == "0A" { found=1 } END { exit !found }' /proc/net/tcp 2>/dev/null
}

# Desktop resizing is switched off for neatvnc before 0.9.2, which is what
# Debian 13 and Devuan 6 ship (0.9.1). It encodes a client's pending damage
# against the current frame without clamping it to that frame's size (fixed
# upstream in 0.9.2, "server: Clamp damage to fb size"). When the desktop
# shrinks while anything on it is still changing, damage recorded against the
# old, bigger frame gets encoded against the new one, the raw encoder reads
# past the end of it, and wayvnc dies with SIGSEGV. No sequencing on the
# client's side prevents that: it reproduced on native Linux with the same
# packages. --disable-resizing makes wayvnc refuse SetDesktopSize, so the
# applet keeps the default desktop, scaled to fit, as it did before resizing
# existed. A version that cannot be read keeps resizing.
neatvnc_lacks_damage_clamp() {
    printf '%s\n' "$1" | awk -F. '
        $1 !~ /^[0-9]+$/ || $2 !~ /^[0-9]+/ { exit 1 }
        { exit !($1 + 0 == 0 && ($2 + 0 < 9 || ($2 + 0 == 9 && $3 + 0 < 2))) }'
}
# ISH_DISPLAY_MAX_FPS: wayvnc captures and encodes the whole framebuffer in
# software, and hands its rate limit to the compositor's screencopy as well, so
# this bounds BOTH. The default is 30, which is affordable at one pixel per
# point and is not at 2x or 3x -- four and nine times the bytes per frame. The
# applet works the number out from the resolution the user chose and passes it
# here; unset, wayvnc keeps its own default and nothing changes.
WAYVNC_FPS_ARG=""
case "${ISH_DISPLAY_MAX_FPS:-}" in
    "") : ;;
    *[!0-9]*) log "warning: ignoring non-numeric ISH_DISPLAY_MAX_FPS='$ISH_DISPLAY_MAX_FPS'" ;;
    0) : ;;
    *) WAYVNC_FPS_ARG="--max-fps=$ISH_DISPLAY_MAX_FPS"
       log "capping wayvnc at ${ISH_DISPLAY_MAX_FPS} fps for the chosen resolution" ;;
esac

WAYVNC_RESIZE_ARG=""
NEATVNC_VERSION="$(wayvnc -V 2>/dev/null | awk -F': *' '$1 == "neatvnc" { print $2; exit }')"
if neatvnc_lacks_damage_clamp "$NEATVNC_VERSION" \
        && wayvnc --help 2>&1 | grep -q -- '--disable-resizing'; then
    WAYVNC_RESIZE_ARG="--disable-resizing"
    log "neatvnc $NEATVNC_VERSION is older than 0.9.2: desktop resizing is off"
fi

hex_port=$(printf '%04X' "$WAYVNC_PORT")
wayvnc_attempt=1
while true; do
    log "starting wayvnc on :$WAYVNC_PORT (attempt $wayvnc_attempt)"
    # $WAYVNC_RESIZE_ARG is empty or one word, so it is left unquoted.
    spawn_logged "wayvnc-attempt$wayvnc_attempt" wayvnc $WAYVNC_RESIZE_ARG $WAYVNC_FPS_ARG 127.0.0.1 "$WAYVNC_PORT"
    WAYVNC_PID=$SPAWN_PID

    # Confirm wayvnc is both still alive AND actually bound/listening before
    # declaring ready -- checking liveness alone races wayvnc's own startup:
    # `kill -0` succeeds the instant fork() returns, long before wayvnc has
    # opened its listening socket. /proc/net/tcp is checked directly (no
    # `ss`/`netstat` dependency) for a LISTEN (0A) entry on WAYVNC_PORT.
    i=0
    wayvnc_listening=0
    while [ $i -lt 150 ]; do
        wayvnc_is_listening && { wayvnc_listening=1; break; }
        kill -0 "$WAYVNC_PID" 2>/dev/null || break
        i=$((i + 1))
        sleep 0.1
    done

    if [ "$wayvnc_listening" = 1 ]; then
        # A single instantaneous LISTEN read isn't enough: the same
        # labwc-not-quite-ready race can let wayvnc bind its VNC socket
        # first, then fail its *internal* reconnect to WAYLAND_DISPLAY a
        # moment later and drop back off LISTEN while it retries that --
        # observed on-device: the applet's bridge got ECONNREFUSED
        # connecting seconds after this exact check had already passed.
        # Recheck after a short settle window instead of trusting one
        # instantaneous read.
        sleep 1.0
        wayvnc_is_listening && break
        wayvnc_listening=0
    fi

    [ "$wayvnc_attempt" -ge 4 ] && die "wayvnc never reached a stable listening state on :$WAYVNC_PORT after $wayvnc_attempt attempts -- see $DEBUG_LOG"
    kill "$WAYVNC_PID" 2>/dev/null
    wayvnc_attempt=$((wayvnc_attempt + 1))
    sleep 0.3
done

# The X display of the compositor's Xwayland. labwc gives it to what labwc starts
# (the menu, keybindings, autostart), but foot and the panel are started here, so
# an X program run in the first terminal said "Can't open display". wlroots
# claims the display while the compositor starts up, which wayvnc connecting has
# shown is over: it binds the X sockets and writes the compositor's pid into
# /tmp/.X<n>-lock, which is how that display is told apart from another
# session's. Guest pids start again at 1 with every boot, though, and a lock an
# earlier session left behind can name the same pid: a default-user session
# took :0 from a stale root lock while its own display was :2. So only a lock
# written since the compositor started counts. Xwayland itself starts on the
# first X client. No lock names the compositor when it has no Xwayland, and
# DISPLAY stays unset.
for x_lock in /tmp/.X*-lock; do
    [ -f "$x_lock" ] || continue
    [ "$COMPOSITOR_START_MARK" -nt "$x_lock" ] && continue
    x_lock_pid=""
    # The pid is right-aligned in ten columns with no newline: read strips the
    # padding and returns nonzero at the missing newline, having set the value.
    read -r x_lock_pid < "$x_lock" 2>/dev/null
    [ "$x_lock_pid" = "$COMPOSITOR_PID" ] || continue
    x_display="${x_lock#/tmp/.X}"
    export DISPLAY=":${x_display%-lock}"
    X_LOCK_PATH="$x_lock"
    cp "$x_lock" "$XDG_RUNTIME_DIR/x-lock.copy" 2>/dev/null
    log "X programs use display $DISPLAY (Xwayland starts on the first one)"
    break
done
# The lock can already be gone when the session started before the guest's
# boot cleaned /tmp. The listening X sockets survive that (the abstract one is
# what clients connect to first), so the display is found from them: socket
# inodes against /proc/net/unix. Whose sockets: the compositor's, or its
# Xwayland's -- Wayfire starts Xwayland at once and hands it the listening
# sockets (-listenfd), so they are no longer in the compositor's table at all.
# Looking only there left a Wayfire session on bip (whose boot cleaned /tmp
# first) with no DISPLAY for foot or the panel, and every X program started
# from the panel's menu -- Dillo -- said "Can't open display". wlroots
# double-forks Xwayland, so it is init's child, not the compositor's: it is
# found as this user's Xwayland that started after the compositor did (an
# older one is a dead session's).
if [ -z "${DISPLAY:-}" ] && [ -r /proc/net/unix ]; then
    _x_inodes=" "
    _x_pids="$COMPOSITOR_PID"
    _x_since=$(awk '{ print $22 }' "/proc/$COMPOSITOR_PID/stat" 2>/dev/null)
    for _c in /proc/[0-9]*; do
        [ "$(cat "$_c/comm" 2>/dev/null)" = Xwayland ] || continue
        [ "$(awk '/^Uid:/ { print $2 }' "$_c/status" 2>/dev/null)" = "$CURRENT_UID" ] || continue
        [ "$(awk '{ print $22 }' "$_c/stat" 2>/dev/null)" -ge "${_x_since:-0}" ] 2>/dev/null || continue
        _x_pids="$_x_pids ${_c#/proc/}"
    done
    for _p in $_x_pids; do
        for _fd in /proc/"$_p"/fd/*; do
            _t=$(readlink "$_fd" 2>/dev/null) || continue
            # [ escaped: unescaped, dash reads it as a bracket expression and
            # strips nothing, so no inode ever matched and this never worked.
            case "$_t" in socket:\[*\]) _t=${_t#socket:\[}; _x_inodes="$_x_inodes${_t%]} " ;; esac
        done
    done
    _x_num=$(awk -v inodes="$_x_inodes" '$8 ~ /^@?\/tmp\/\.X11-unix\/X[0-9]+$/ && index(inodes, " " $7 " ") {
        sub(/.*X/, "", $8); print $8; exit }' /proc/net/unix 2>/dev/null)
    if [ -n "$_x_num" ]; then
        export DISPLAY=":$_x_num"
        X_LOCK_PATH="/tmp/.X$_x_num-lock"
        printf '%10d\n' "$COMPOSITOR_PID" > "$XDG_RUNTIME_DIR/x-lock.copy" 2>/dev/null
        log "X programs use display $DISPLAY (found from the compositor's socket)"
    fi
fi

# foot isn't load-bearing for the applet's own readiness (wayvnc is what the
# bridge connects to), so a foot that dies here leaves a perfectly "Connected"
# session with a silently empty desktop -- no error anywhere, since wayvnc
# already proved labwc's socket is live and accepting *a* client. Turns out
# that's not proof labwc is ready for the *next* one: this is the exact same
# "labwc accepted wayvnc's connection but isn't fully ready for a new
# client's surface yet" race the wayvnc retry loop above was built for, just
# narrower and easier to lose (observed on-device as an intermittent "no
# terminal on startup" with labwc/wayvnc/avahi/sshd all otherwise healthy --
# no foot thread AND no leftover tee reader for it, i.e. it started, emitted
# some output, then fully exited moments later). The old check here was a
# single 0.2s sleep + one kill -0, far too narrow a window to catch a death
# that lands just after it. Give foot the same watch-then-retry treatment.
# The ready file comes only after this watch, and that matters beyond foot:
# written straight after starting foot, the applet connected and wl-present
# told wayvnc to detach while wayvnc was still on its first capture, and
# wayvnc 0.9.1 crashed (a NULL write in libwayland-client's object map, the
# connection it had just dropped) -- 5th-generation iPad, 2026-10-01.
#
# Under Wayfire the first terminal opens maximized. Its panel starts later
# (below), and Wayfire places a window by the work area at the moment it
# opens and never moves a floating one when a panel later reserves the top:
# foot's title bar sat under wf-panel (bip, 2026-10-02; Wayfire on Linux
# does the same). A maximized window is fitted to the work area again when
# the panel arrives. foot's default size is larger than an iPad's screen at
# 2x scale anyway.
foot_args=
[ "$AOK_DESKTOP" = wayfire ] && foot_args=--maximized
foot_attempt=1
while true; do
    log "starting foot (attempt $foot_attempt)"
    spawn_logged "foot-attempt$foot_attempt" foot $foot_args
    FOOT_PID=$SPAWN_PID

    i=0
    foot_died=0
    while [ $i -lt 20 ]; do
        kill -0 "$FOOT_PID" 2>/dev/null || { foot_died=1; break; }
        i=$((i + 1))
        sleep 0.1
    done
    [ "$foot_died" = 0 ] && break

    # 4 attempts (the wayvnc loop's own budget) turned out not to be enough:
    # on-device, a full cold boot's systemd/avahi/sshd/dbus-broker churn can
    # keep colliding with this exact race for several seconds straight, and
    # each failed attempt only costs the crash's own near-instant death time
    # (well under the 2s watch window above) plus this backoff, so a much
    # larger budget is cheap in the case that matters (still succeeds fast
    # once the collision stops) and just means a slower failure in the case
    # that doesn't.
    [ "$foot_attempt" -ge 15 ] && die "foot kept exiting right after starting (attempt $foot_attempt) -- see $DEBUG_LOG"
    foot_attempt=$((foot_attempt + 1))
    sleep 0.3
done



# On the GPU, wl-present (compiled into iSH-AOK) hands each frame the
# compositor draws straight to the app, and takes the app's pointer, keyboard,
# clipboard and resizes to the compositor itself. Once the app is showing its
# frames it prints "detach": wayvnc, which otherwise screen-copies for as long
# as the app is connected, lets go of the compositor, and nothing copies or
# encodes frames on the CPU. It prints "attach" as it ends, and wayvnc is
# attached again however it ends, so the app's VNC connection -- kept up
# throughout -- can take over. Without it, or with a software compositor
# (whose buffers the app cannot take), the app uses wayvnc as before.
if [ "$WL_GPU_COMPOSITOR" = 1 ] && [ "${ISH_DISPLAY_DIRECT:-1}" != 0 ] && [ -x /AOK/native/wl-present ]; then
    log "presenting the desktop to the app directly"
    spawn_logged wl-present "$AOK_ENV" WAYVNC_PORT="$WAYVNC_PORT" "$AOK_SH" -c '
        /AOK/native/wl-present | while read -r cmd; do
            case "$cmd" in
                detach) wayvncctl detach ;;
                attach) wayvncctl attach "$WAYLAND_DISPLAY" ;;
            esac >/dev/null 2>&1
        done
        wayvncctl attach "$WAYLAND_DISPLAY" >/dev/null 2>&1'
    PRESENT_PID=$SPAWN_PID
fi

echo "READY $WAYVNC_PORT"
# The applet's whole handshake is this file appearing; if the write fails
# (e.g. a stale root-owned file this uid can't overwrite in sticky /tmp),
# a perfectly healthy stack would sit invisible until the applet's 45s
# timeout tears it down. Fail loudly instead -- die() persists the reason.
{ printf '%s\n' "$WAYVNC_PORT" > "$READY_FILE"; } 2>/dev/null \
    || die "cannot write $READY_FILE (stale file owned by another uid?) -- the applet cannot see this session"

# waybar, the panel, starts with the desktop: after the ready file, so its GTK
# startup never delays the applet connecting. panel.sh leaves it off when the
# user hid it (Applications > Hide Panel), and it is skipped when the user's own
# labwc autostart starts waybar, which would make two.
if [ "$AOK_DESKTOP" = labwc ] && command -v waybar >/dev/null 2>&1 \
        && [ -x "$HOME/.config/labwc/panel.sh" ] \
        && ! grep -qs waybar "$HOME/.config/labwc/autostart"; then
    if ! aok_font_awesome_present; then
        log "warning: Font Awesome is not installed, so the panel's icons will"
        log "         draw as empty boxes with a code in them. Install it with"
        log "         your package manager ($(aok_font_awesome_package)) and"
        log "         reopen the desktop, or re-run /AOK/tools/setup-wayland.sh."
    fi
    log "starting the panel (waybar)"
    spawn_logged panel "$HOME/.config/labwc/panel.sh" start
    PANEL_PID=$SPAWN_PID
fi

# Wayfire's panel, like waybar for labwc above: started late. Started by
# Wayfire's own autostart, or straight after the UI scale is set, it was never
# drawn (M4, 2026-10-02): Wayfire applies an output's new scale asynchronously,
# and a panel that connects while the change is still arriving keeps a surface
# that is never shown. Restarted later, the same panel in the same environment
# is drawn. A wayfire.ini that starts wf-panel itself is left to do so.
# Or waybar, when select-desktop.sh --panel waybar chose it (and it is there).
if [ "$AOK_DESKTOP" = wayfire ] && [ "$AOK_WAYFIRE_PANEL" = waybar ] && ! command -v waybar >/dev/null 2>&1; then
    log "warning: waybar was chosen as Wayfire's panel but is not installed; starting wf-panel"
    log "         (sudo sh /AOK/tools/setup-wayfire.sh --panel waybar installs it)"
    AOK_WAYFIRE_PANEL=wf-panel
fi
if [ "$AOK_DESKTOP" = wayfire ] && [ "$AOK_WAYFIRE_PANEL" = waybar ]; then
    log "starting the panel (waybar)"
    spawn_logged panel waybar -c "$WAYFIRE_WAYBAR_CONFIG"
    PANEL_PID=$SPAWN_PID
elif [ "$AOK_DESKTOP" = wayfire ] && command -v wf-panel >/dev/null 2>&1 \
        && ! grep -qs '^[^#]*wf-panel' "$HOME/.config/wayfire.ini" \
        && ! grep -qs '^autostart_wf_shell *= *true' "$HOME/.config/wayfire.ini"; then
    # The faster wf-panel (setup-wayfire.sh --panel fast) is built against one
    # wf-shell release, which wf-panel.source beside it names; after the
    # distro moves on, the packaged one is the safe panel.
    panel_bin=wf-panel
    fast_panel=$(readlink -f /usr/local/bin/wf-panel 2>/dev/null)
    if [ -n "$fast_panel" ] && [ -f "$fast_panel.source" ] && [ -x /usr/bin/wf-panel ] \
            && [ "$(cat "$fast_panel.source")" != "$(dpkg-query -W -f '${Version}' wf-shell 2>/dev/null)" ]; then
        log "warning: the faster wf-panel was built for wf-shell $(cat "$fast_panel.source"),"
        log "         not the installed one; starting the packaged wf-panel"
        log "         (sudo sh /AOK/tools/setup-wayfire.sh --panel fast updates it)"
        panel_bin=/usr/bin/wf-panel
    fi
    log "starting the panel (wf-panel)"
    spawn_logged panel "$panel_bin"
    PANEL_PID=$SPAWN_PID
fi

# The repair the early-boot comment above promises: once the guest's boot has
# cleaned /tmp, put back the compositor's X lock (aok-sdl-game and later
# sessions find the display by it) and the applet's ready file (a Reconnect
# polls for it). Only for a session that started near boot (UPTIME_AT_START).
aok_repair_after_early_boot() {
    # Wait for the early boot to show up and finish -- or, when none shows up
    # within a minute (a root without one), repair anyway, which is harmless.
    _rp_waited=0
    _rp_seen=0
    while [ "$_rp_waited" -lt 600 ]; do
        if aok_early_boot_running; then
            _rp_seen=1
        elif [ "$_rp_seen" = 1 ] || [ "$_rp_waited" -ge 60 ]; then
            break
        fi
        sleep 1
        _rp_waited=$((_rp_waited + 1))
    done
    kill -0 "$COMPOSITOR_PID" 2>/dev/null || return 0
    if [ -n "${X_LOCK_PATH:-}" ] && [ ! -e "$X_LOCK_PATH" ] && [ -s "$XDG_RUNTIME_DIR/x-lock.copy" ]; then
        cp "$XDG_RUNTIME_DIR/x-lock.copy" "$X_LOCK_PATH" 2>/dev/null && chmod 444 "$X_LOCK_PATH" 2>/dev/null
    fi
    if [ -z "${ISH_DISPLAY_DEBUG_LOG:-}" ] && [ ! -e /tmp/ish-wayland-debug.log ]; then
        ln -sf "$DEBUG_LOG" /tmp/ish-wayland-debug.log 2>/dev/null
    fi
    if [ ! -e "$READY_FILE" ] && [ ! -e "/tmp/ish-display.closing.$$" ]; then
        printf '%s\n' "$WAYVNC_PORT" > "$READY_FILE" 2>/dev/null
    fi
}
if [ "$UPTIME_AT_START" -lt 300 ] 2>/dev/null; then
    aok_repair_after_early_boot &
    REPAIR_PID=$!
fi

# The session is over when the compositor is. This was `wait` on the
# compositor, foot AND wayvnc, which returns only when all three have gone:
# labwc crashed on the M4 with wayvnc still up, and the applet said "running
# and connected" over a desktop that no longer existed. Exiting runs cleanup
# (the EXIT trap), which takes wayvnc and the rest down, so the applet sees the
# session end and can start a new one. foot closing is not the end of anything.
#
# Polled, through a waited-for sleep rather than a plain one: `wait` lets the
# HUP/TERM traps run at once, and it reaps the compositor, so `kill -0` stops
# finding a zombie.
while kill -0 "$COMPOSITOR_PID" 2>/dev/null; do
    sleep 2 &
    wait $! 2>/dev/null
done
echo "start-wayland: $COMPOSITOR_CMD (pid $COMPOSITOR_PID) exited; ending the session" >&2
