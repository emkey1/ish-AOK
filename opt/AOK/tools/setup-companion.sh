#!/bin/sh
# setup-companion.sh
# ---------------------------------------------------------------------------
# Give a native-mode root its companion: the filesystem apt and the Wayland
# desktop work in.
#
# A native root has no distribution, so it has no package manager. This
# installs a second filesystem -- Devuan 6 arm64, named "Devuan" -- and
# prepares it to work alongside this one. The booted root stays native; what
# apt installs lives in the companion and runs from there under emulation,
# found by name because the companion's bin directories follow this root's on
# PATH. See /AOK/docs/native-companion.md.
#
# Run as root, in a native root:
#       sudo sh /AOK/tools/setup-companion.sh
# `sudo apt install ...` offers to run it the first time, and
# setup-native-wayland.sh runs it before installing a desktop.
#
# Options:
#   --root NAME    use a Devuan arm64 filesystem that is already installed
#                  (Filesystems, or manage-roots.sh list) instead of
#                  installing one
#   --user LOGIN   give this account the companion; repeatable. The default
#                  is every account here with a uid of 1000 or more
#   -h, --help     this text
#
# What "prepares" means:
#   - /etc/aok-companion-root here names the filesystem, for apt, the
#     desktop scripts and your PATH (/etc/profile.d/20-aok-companion.sh);
#   - its /etc/aok-binds asks for this root's /home and /tmp, its hosts,
#     hostname and resolv.conf, and its time zone, each time it is entered:
#     its programs see your files, and the same system you do;
#   - each account gets the same name, uid, gid, home, shell and password
#     there, so a file in the shared /home has one owner, and sudo asks the
#     same question;
#   - /usr/sbin/policy-rc.d there says no to starting services, because that
#     filesystem is never booted while this is how it is used;
#   - its package lists are fetched (apt-get update) if it has none.
#
# Safe to run again: every step is skipped where it is already done, and a
# second run is how you add an account (--user) or finish an install that was
# interrupted.
#
# Afterwards: sudo apt install <package>, and run what it installed by name
# (in a new login, so PATH has the companion on it). A desktop:
# sudo sh /AOK/tools/setup-native-wayland.sh.
# ---------------------------------------------------------------------------
set -eu

. /AOK/tools/companion-root.sh

CATALOG_ID=devuan6arm64
DEFAULT_NAME=Devuan

log()  { printf '\n\033[1;36m==>\033[0m \033[1m%s\033[0m\n' "$*"; }
note() { printf '    %s\n' "$*"; }
warn() { printf '    \033[1;33m!\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

# The header comment, between its two rules.
usage() { awk '/^# ------/ { n++; next } n == 1 { sub(/^# ?/, ""); print }' "$0"; }

ROOT_NAME=
USERS=
while [ $# -gt 0 ]; do
    case "$1" in
        --root) [ $# -ge 2 ] || die "--root needs a name"; ROOT_NAME=$2; shift ;;
        --user) [ $# -ge 2 ] || die "--user needs a login"; USERS="$USERS $2"; shift ;;
        -h|--help) usage; exit 0 ;;
        *) die "unknown argument: $1 (try --help)" ;;
    esac
    shift
done

[ "$(id -u)" = 0 ] || die "needs root: sudo sh $0"
aok_in_native_root || die "this is for a native-mode root. This one has its own package manager."

# ---- the filesystem --------------------------------------------------------

EXPLICIT_ROOT=$ROOT_NAME
[ -n "$ROOT_NAME" ] || ROOT_NAME=$(aok_companion_root_name 2>/dev/null) || ROOT_NAME=$DEFAULT_NAME
W="$AOK_COMPANION_ROOTS_DIR/$ROOT_NAME"

roots_now() { ls "$AOK_COMPANION_ROOTS_DIR" 2>/dev/null || true; }

if [ ! -x "$W/bin/sh" ]; then
    [ -z "$EXPLICIT_ROOT" ] || die "no filesystem named '$ROOT_NAME' at $W (sudo sh /AOK/tools/manage-roots.sh list)"
    log "Installing a Devuan arm64 filesystem as this root's companion"
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
    [ -n "$installed" ] || die "the filesystem was installed but did not appear under $AOK_COMPANION_ROOTS_DIR; relaunch AOK-Workstation and run this again with --root NAME"
    if [ "$installed" != "$ROOT_NAME" ]; then
        if sh /AOK/tools/manage-roots.sh rename "$installed" "$ROOT_NAME" -q; then
            note "named it '$ROOT_NAME'"
        else
            warn "could not rename '$installed' to '$ROOT_NAME'; using it as '$installed'"
            ROOT_NAME=$installed
        fi
    fi
    W="$AOK_COMPANION_ROOTS_DIR/$ROOT_NAME"
    tries=0
    while [ ! -x "$W/bin/sh" ] && [ "$tries" -lt 30 ]; do sleep 1; tries=$((tries + 1)); done
    [ -x "$W/bin/sh" ] || die "'$ROOT_NAME' is installed but $W is not there yet; relaunch AOK-Workstation and run this again"
else
    log "Using the filesystem '$ROOT_NAME'"
fi

[ -x "$W/usr/bin/apt-get" ] || die "'$ROOT_NAME' has no apt-get: the companion has to be a Devuan (or Debian) filesystem"

# ---- wiring ----------------------------------------------------------------

log "Wiring '$ROOT_NAME' to this root"

printf '# The companion filesystem of this native root: where apt and the Wayland\n# desktop work (setup-companion.sh, /AOK/tools/companion-root.sh). One name.\n%s\n' \
    "$ROOT_NAME" > "$AOK_COMPANION_ROOT_FILE"
chmod 644 "$AOK_COMPANION_ROOT_FILE"
note "$AOK_COMPANION_ROOT_FILE names it"

[ -s "$W/etc/aok-binds" ] || printf '%s\n' \
    '# What of the booted root to share into this one when it is entered from' \
    '# there (AOK-Workstation: kernel/foreign_exec.c, mount-root.sh). One a line, of:' \
    '#   home tmp mnt media srv                      directories' \
    '#   etc/hosts etc/hostname etc/resolv.conf      files' \
    '#   timezone                                    /etc/localtime names the same zone' > "$W/etc/aok-binds"
for want in home tmp etc/hosts etc/hostname etc/resolv.conf timezone; do
    grep -q -x "$want" "$W/etc/aok-binds" 2>/dev/null || printf '%s\n' "$want" >> "$W/etc/aok-binds"
done
chmod 644 "$W/etc/aok-binds"
note "its /etc/aok-binds shares this root's /home and /tmp, its hosts, hostname,"
note "resolv.conf and time zone"
[ -s /etc/resolv.conf ] || warn "this root has no /etc/resolv.conf; package downloads will fail until it does"

if [ ! -e "$W/usr/sbin/policy-rc.d" ]; then
    printf '%s\n' '#!/bin/sh' \
        '# Written by setup-companion.sh: this filesystem is entered from a' \
        '# native root, not booted, so package scripts must not start services.' \
        '# Delete this file if you boot it on its own.' \
        'exit 101' > "$W/usr/sbin/policy-rc.d"
    chmod 755 "$W/usr/sbin/policy-rc.d"
    note "package scripts will not start services there (policy-rc.d)"
fi

# The mode that makes any of this work. Said now, not at the first session.
if ! aok_companion_root_find; then
    die "$AOK_COMPANION_PROBLEM"
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
    # The same supplementary groups, where the companion has them.
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

# sudo run inside the companion (a desktop's terminal) is AOK-Workstation's native
# one, reading the companion's sudoers. A fresh Devuan has none, and none
# means nobody.
if [ ! -e "$W/etc/sudoers" ] && [ -r /etc/sudoers ]; then
    cat /etc/sudoers > "$W/etc/sudoers"
    chmod 440 "$W/etc/sudoers"
    note "sudoers copied (the companion had none)"
fi

# ---- the shell inside it ---------------------------------------------------

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
# AOK-Workstation's native programs, after this filesystem's own commands.
if [ -d /AOK/native ]; then
    case ":$PATH:" in
        *":/AOK/native:"*) ;;
        *) PATH="$PATH:/AOK/native"; export PATH ;;
    esac
fi
EOF
    chmod 644 "$W/etc/profile.d/10-aok-native.sh"
fi
note "a shell inside it is your shell, with the native programs on PATH"

# ---- the package lists -----------------------------------------------------

# A filesystem straight from the catalogue has never run apt: with no lists,
# `apt install anything` answers "Unable to locate package". Fetched here,
# once, so the first install works; a later `apt update` is the user's.
has_lists() {
    for f in "$W"/var/lib/apt/lists/*_Packages*; do
        [ -e "$f" ] && return 0
    done
    return 1
}
if ! has_lists; then
    log "Fetching the package lists (apt-get update)"
    aok_companion_env
    "$W/usr/bin/apt-get" update ||
        warn "apt-get update failed -- check the network, then run: sudo apt update"
fi

log "Done"
note "companion: $ROOT_NAME  ($W)"
note "Install packages with: sudo apt install <package>"
note "What they install runs by name in a new login (PATH picks the companion up"
note "there); until then: $W/usr/bin/<command>"
