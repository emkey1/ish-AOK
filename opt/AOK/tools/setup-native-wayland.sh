#!/bin/sh
# setup-native-wayland.sh
# ---------------------------------------------------------------------------
# Give a native-mode root a Wayland desktop.
#
# A native root has no distribution, so it has no compositor and no package
# manager to install one with. This installs a second filesystem -- Devuan 6
# arm64, named "Wayland" -- prepares it to work alongside this one, and
# installs the desktop into it. The booted root stays native; the desktop,
# and the graphical programs you add to it, run from the Wayland filesystem
# under emulation. See /AOK/docs/native-mode.md.
#
# Run as root, in a native root:
#       sudo sh /AOK/tools/setup-native-wayland.sh
#
# Options:
#   --root NAME    use a Devuan arm64 filesystem that is already installed
#                  (Filesystems, or manage-roots.sh list) instead of
#                  installing one
#   --user LOGIN   give this account the desktop; repeatable. The default is
#                  every account here with a uid of 1000 or more
#   --no-desktop   prepare the filesystem but do not install packages into it
#                  (run setup-wayland.sh later; it finds its way in)
#   -h, --help     this text
#
# What "prepares" means:
#   - /etc/aok-wayland-root here names the filesystem, for start-wayland.sh
#     and the other desktop scripts, which run themselves inside it;
#   - its /etc/aok-binds asks for this root's /home and /tmp to be shared in,
#     so the desktop sees your files and the app sees the session;
#   - each account gets the same name, uid, gid, home, shell and password
#     there, so a file in the shared /home has one owner, and sudo asks the
#     same question;
#   - its resolv.conf is this root's;
#   - /usr/sbin/policy-rc.d there says no to starting services, because that
#     filesystem is never booted while this is how it is used.
#
# Safe to run again: every step is skipped where it is already done, and a
# second run is how you add an account (--user) or finish an install that was
# interrupted.
#
# Afterwards: open the Wayland window. More desktops and programs go in the
# same way as on any root -- setup-wayfire.sh, setup-xfce.sh, setup-games.sh
# and select-desktop.sh all run inside the Wayland filesystem from here -- and
# anything else with apt, from the desktop's own terminal (sudo apt install
# ...) or from outside (sudo sh /AOK/tools/mount-root.sh Wayland -- apt ...).
# ---------------------------------------------------------------------------
set -eu

. /AOK/tools/wayland-root.sh

CATALOG_ID=devuan6arm64
DEFAULT_NAME=Wayland

log()  { printf '\n\033[1;36m==>\033[0m \033[1m%s\033[0m\n' "$*"; }
note() { printf '    %s\n' "$*"; }
warn() { printf '    \033[1;33m!\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

# The header comment, between its two rules.
usage() { awk '/^# ------/ { n++; next } n == 1 { sub(/^# ?/, ""); print }' "$0"; }

ROOT_NAME=
USERS=
DESKTOP=1
while [ $# -gt 0 ]; do
    case "$1" in
        --root) [ $# -ge 2 ] || die "--root needs a name"; ROOT_NAME=$2; shift ;;
        --user) [ $# -ge 2 ] || die "--user needs a login"; USERS="$USERS $2"; shift ;;
        --no-desktop) DESKTOP=0 ;;
        -h|--help) usage; exit 0 ;;
        *) die "unknown argument: $1 (try --help)" ;;
    esac
    shift
done

[ "$(id -u)" = 0 ] || die "needs root: sudo sh $0"
aok_in_native_root || die "this is for a native-mode root. On this one, run: sudo sh /AOK/tools/setup-wayland.sh"

# ---- the filesystem --------------------------------------------------------

EXPLICIT_ROOT=$ROOT_NAME
[ -n "$ROOT_NAME" ] || ROOT_NAME=$(aok_wayland_root_name 2>/dev/null) || ROOT_NAME=$DEFAULT_NAME
W="$AOK_WAYLAND_ROOTS_DIR/$ROOT_NAME"

roots_now() { ls "$AOK_WAYLAND_ROOTS_DIR" 2>/dev/null || true; }

if [ ! -x "$W/bin/sh" ]; then
    [ -z "$EXPLICIT_ROOT" ] || die "no filesystem named '$ROOT_NAME' at $W (sudo sh /AOK/tools/manage-roots.sh list)"
    log "Installing a Devuan arm64 filesystem for the desktop"
    [ -e /proc/ish/roots ] || die "this build cannot install filesystems (no /proc/ish/roots). Install a Devuan arm64 one yourself and name it: $0 --root NAME"
    before=$(roots_now)
    sh /AOK/tools/manage-roots.sh install "$CATALOG_ID" || die "the install failed (see above); nothing else was changed"
    # A catalogue install names the root itself: it is the one that was not
    # there before. Other roots can take a moment to be mounted.
    installed=
    tries=0
    while [ -z "$installed" ] && [ "$tries" -lt 30 ]; do
        for r in $(roots_now); do
            case " $(echo $before) " in *" $r "*) ;; *) installed=$r ;; esac
        done
        [ -n "$installed" ] || { sleep 1; tries=$((tries + 1)); }
    done
    [ -n "$installed" ] || die "the filesystem was installed but did not appear under $AOK_WAYLAND_ROOTS_DIR; relaunch AOK-iSH and run this again with --root NAME"
    if [ "$installed" != "$ROOT_NAME" ]; then
        if sh /AOK/tools/manage-roots.sh rename "$installed" "$ROOT_NAME" -q; then
            note "named it '$ROOT_NAME'"
        else
            warn "could not rename '$installed' to '$ROOT_NAME'; using it as '$installed'"
            ROOT_NAME=$installed
        fi
    fi
    W="$AOK_WAYLAND_ROOTS_DIR/$ROOT_NAME"
    tries=0
    while [ ! -x "$W/bin/sh" ] && [ "$tries" -lt 30 ]; do sleep 1; tries=$((tries + 1)); done
    [ -x "$W/bin/sh" ] || die "'$ROOT_NAME' is installed but $W is not there yet; relaunch AOK-iSH and run this again"
else
    log "Using the filesystem '$ROOT_NAME'"
fi

[ -x "$W/usr/bin/apt-get" ] || die "'$ROOT_NAME' has no apt-get: the desktop setup here is for a Devuan (or Debian) filesystem"

# ---- wiring ----------------------------------------------------------------

log "Wiring '$ROOT_NAME' to this root"

printf '# The filesystem this native root'"'"'s Wayland desktop runs from\n# (setup-native-wayland.sh, /AOK/tools/wayland-root.sh). One name.\n%s\n' \
    "$ROOT_NAME" > "$AOK_WAYLAND_ROOT_FILE"
chmod 644 "$AOK_WAYLAND_ROOT_FILE"
note "$AOK_WAYLAND_ROOT_FILE names it"

for want in home tmp; do
    if ! grep -q -x "$want" "$W/etc/aok-binds" 2>/dev/null; then
        [ -s "$W/etc/aok-binds" ] || printf '%s\n' \
            '# Directories of the booted root to share into this one when it is' \
            '# entered from there (AOK-iSH: kernel/foreign_exec.c, mount-root.sh).' \
            '# One a line, of: home tmp mnt media srv.' > "$W/etc/aok-binds"
        printf '%s\n' "$want" >> "$W/etc/aok-binds"
    fi
done
chmod 644 "$W/etc/aok-binds"
note "its /etc/aok-binds shares this root's /home and /tmp"

if [ -s /etc/resolv.conf ]; then
    rm -f "$W/etc/resolv.conf"
    cat /etc/resolv.conf > "$W/etc/resolv.conf"
    note "resolv.conf copied"
else
    warn "this root has no /etc/resolv.conf; package downloads will fail until it does"
fi

if [ ! -e "$W/usr/sbin/policy-rc.d" ]; then
    printf '%s\n' '#!/bin/sh' \
        '# Written by setup-native-wayland.sh: this filesystem is entered from a' \
        '# native root, not booted, so package scripts must not start services.' \
        '# Delete this file if you boot it on its own.' \
        'exit 101' > "$W/usr/sbin/policy-rc.d"
    chmod 755 "$W/usr/sbin/policy-rc.d"
    note "package scripts will not start services there (policy-rc.d)"
fi

# The mode that makes any of this work. Said now, not at the first session.
if ! aok_wayland_root_find; then
    die "$AOK_WAYLAND_PROBLEM"
fi
arch=$("$W/bin/sh" -c 'dpkg --print-architecture' 2>/dev/null) || arch=
case "$arch" in
    arm64) ;;
    '') die "could not run a program from '$ROOT_NAME' ($W/bin/sh)" ;;
    *) warn "'$ROOT_NAME' is $arch, not arm64: it will work, more slowly" ;;
esac

# ---- accounts --------------------------------------------------------------

# A field of LOGIN's line in FILE (passwd, group or shadow layout).
field_of() { awk -F: -v u="$2" -v f="$3" '$1 == u { print $f; exit }' "$1"; }
# The name that has ID in column 3 of FILE.
name_of_id() { awk -F: -v i="$2" '$3 == i { print $1; exit }' "$1"; }

if [ -z "$USERS" ]; then
    USERS=$(awk -F: '$3 >= 1000 && $3 < 65000 { print $1 }' /etc/passwd)
fi

log "Accounts"
made=0
for login in $USERS; do
    uid=$(field_of /etc/passwd "$login" 3)
    [ -n "$uid" ] || { warn "no account '$login' here; skipped"; continue; }
    gid=$(field_of /etc/passwd "$login" 4)
    home=$(field_of /etc/passwd "$login" 6)
    line=$(awk -F: -v u="$login" '$1 == u { print; exit }' /etc/passwd)

    there_uid=$(field_of "$W/etc/passwd" "$login" 3)
    if [ -n "$there_uid" ]; then
        if [ "$there_uid" = "$uid" ]; then
            note "$login (uid $uid): already there"
        else
            warn "$login is uid $there_uid in '$ROOT_NAME' and $uid here: the shared /home will not be theirs there. Fix one side (usermod -u) and run this again"
            continue
        fi
    else
        other=$(name_of_id "$W/etc/passwd" "$uid")
        if [ -n "$other" ]; then
            warn "uid $uid is '$other' in '$ROOT_NAME'; $login was not added there"
            continue
        fi
        printf '%s\n' "$line" >> "$W/etc/passwd"
        made=$((made + 1))
        note "$login (uid $uid): added, home $home"
    fi

    # Their own group, by gid, unless that gid is someone's there already.
    gname=$(name_of_id /etc/group "$gid")
    if [ -n "$gname" ] && [ -z "$(name_of_id "$W/etc/group" "$gid")" ] \
            && [ -z "$(field_of "$W/etc/group" "$gname" 3)" ]; then
        printf '%s:x:%s:\n' "$gname" "$gid" >> "$W/etc/group"
    fi
    # The password, so sudo and a lock screen ask for the one they know.
    hash=$(field_of /etc/shadow "$login" 2)
    if [ -f "$W/etc/shadow" ] && [ -z "$(field_of "$W/etc/shadow" "$login" 1)" ]; then
        printf '%s:%s:::::::\n' "$login" "${hash:-!}" >> "$W/etc/shadow"
        chmod 640 "$W/etc/shadow" 2>/dev/null || true
    fi
    # The same supplementary groups, where the Wayland root has them.
    for g in $(awk -F: -v u="$login" '{ n = split($4, m, ","); for (i = 1; i <= n; i++) if (m[i] == u) print $1 }' /etc/group); do
        [ -n "$(field_of "$W/etc/group" "$g" 3)" ] || continue
        if ! awk -F: -v g="$g" -v u="$login" '$1 == g { n = split($4, m, ","); for (i = 1; i <= n; i++) if (m[i] == u) f = 1 } END { exit !f }' "$W/etc/group"; then
            awk -F: -v OFS=: -v g="$g" -v u="$login" '$1 == g { $4 = ($4 == "" ? u : $4 "," u) } { print }' \
                "$W/etc/group" > "$W/etc/group.aok-new" && cat "$W/etc/group.aok-new" > "$W/etc/group"
            rm -f "$W/etc/group.aok-new"
        fi
    done
done
[ "$made" -gt 0 ] || note "(nothing to add)"

# sudo in the desktop's terminal is AOK-iSH's native one, reading the Wayland
# root's sudoers. A fresh Devuan has none, and none means nobody.
if [ ! -e "$W/etc/sudoers" ] && [ -r /etc/sudoers ]; then
    cat /etc/sudoers > "$W/etc/sudoers"
    chmod 440 "$W/etc/sudoers"
    note "sudoers copied (the Wayland filesystem had none)"
fi

# ---- the shell in the desktop's terminal -----------------------------------

# The accounts' shell is native zsh, which reads /etc/zprofile and /etc/zshrc
# of the root it runs in. Devuan ships neither without its own zsh.
for f in zprofile zshrc; do
    if [ ! -e "$W/etc/$f" ] && [ ! -e "$W/etc/zsh/$f" ] && [ -r "/etc/$f" ]; then
        cat "/etc/$f" > "$W/etc/$f"
        chmod 644 "$W/etc/$f"
    fi
done
mkdir -p "$W/etc/profile.d"
if [ ! -e "$W/etc/profile.d/10-aok-native.sh" ]; then
    cat > "$W/etc/profile.d/10-aok-native.sh" <<'EOF'
# AOK-iSH's native programs, after this filesystem's own commands.
if [ -d /AOK/native ]; then
    case ":$PATH:" in
        *":/AOK/native:"*) ;;
        *) PATH="$PATH:/AOK/native"; export PATH ;;
    esac
fi
EOF
    chmod 644 "$W/etc/profile.d/10-aok-native.sh"
fi
note "the desktop's terminal gets your shell, with the native programs on PATH"

# ---- the desktop -----------------------------------------------------------

if [ "$DESKTOP" = 1 ]; then
    log "Installing the desktop into '$ROOT_NAME' (this is the slow part)"
    PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:/AOK/native
    export PATH
    "$W/bin/sh" /AOK/tools/setup-wayland.sh || die "setup-wayland.sh failed inside '$ROOT_NAME' (see above). Run this again to retry; what is done is kept"
fi

log "Done"
note "filesystem: $ROOT_NAME  ($W)"
if [ "$DESKTOP" = 1 ]; then
    note "Open the Wayland window to start the desktop."
else
    note "No packages were installed. Next: sudo sh /AOK/tools/setup-wayland.sh"
fi
note "More programs: sudo apt install ... in the desktop's terminal, or"
note "    sudo sh /AOK/tools/mount-root.sh $ROOT_NAME -- apt install ..."
