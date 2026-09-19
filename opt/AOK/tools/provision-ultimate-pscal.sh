#!/bin/sh
# provision-ultimate-pscal.sh
# ---------------------------------------------------------------------------
# Set up a freshly imported PSCAL rootfs running under iSH-AOK: login account,
# passwords, SSH identity and native links.
#
# This is NOT the apt/apk twin of provision-ultimate-devuan.sh. A PSCAL rootfs
# has no package manager and nothing to install -- it is a from-scratch image
# holding the five PSCAL frontends, SmallCLUE and OpenSSH, and it arrives with
# every account LOCKED. So this script configures rather than installs, and
# the three things it does are the three things that are otherwise a chore to
# redo by hand after every image update:
#
#   1. Give both accounts a password. The image ships every account locked, so
#      until this runs `su -` has nothing to check and `sudo` has nobody it can
#      authenticate: sudo asks the INVOKING user for their OWN password, and
#      su asks for the target's. Then authorise the login for sudo, which on
#      this image means putting it in `wheel` (`%wheel ALL=(ALL:ALL) ALL` is in
#      the shipped /etc/sudoers).
#   2. Create the login you actually use, with a password and its own
#      ~/.ssh/authorized_keys (the stock sshd_config points every user at
#      /root/.ssh/authorized_keys, which this corrects).
#   3. Keep the SSH identity across image updates. Host keys are regenerated
#      by /etc/service/sshd/run on a fresh image, so every re-import makes
#      your client cry MITM and makes you re-copy your authorized_keys. Both
#      are stashed under /AOK/persist, which survives a root switch, and
#      restored from there next time.
#
# So the second run of this, on the next image, is a handful of Return
# presses: everything it needs is already in /AOK/persist.
#
# PRIVACY, worth saying out loud: /AOK/persist is HOST-backed -- it lives in
# the app's own container, outside any Linux rootfs, and is reachable from
# iOS (Files.app, a backup) rather than only from the guest. Stashing host
# keys there puts SSH PRIVATE KEYS somewhere the guest does not control.
# That is the whole point -- nothing inside the rootfs can outlive the
# rootfs -- but it is your call, so the prompt asks, and PERSIST_SSH=0 skips
# it entirely (at the cost of a new host identity on every image).
#
# It is IDEMPOTENT: safe to run repeatedly. Run as root, from either copy:
#       sh /AOK/tools/provision-ultimate-pscal.sh    # served by iSH-AOK
#       provision-ultimate-pscal.sh                  # shipped in the image
# The /AOK copy is whatever the installed app carries, so it is the newer of
# the two on a current build and absent on a build older than this script;
# the image always has one that matches the image.
#
# When run on a terminal it PROMPTS. Pre-set any tunable via the environment
# to skip its prompt / run non-interactively:
#       TARGET_USER=mke            # primary login to set up (else prompted)
#       NEW_HOSTNAME=pscal-ish     # hostname to set (else prompted)
#       PERSIST_SSH=1              # 0 = never touch /AOK/persist
#       PASSWORD_AUTH=1            # 1 = sshd PasswordAuthentication yes
#       NATIVE_LINKS=0             # 1 = also run /AOK/tools/native-links.sh
#       AUTHORIZED_KEY=            # a public key line to install
#
# Passwords are never taken from the environment and never echoed: this hands
# off to `passwd`, which reads them from the terminal itself.
# ---------------------------------------------------------------------------
set -u

PERSIST_DIR=/AOK/persist/pscal

# The stash lives in iSH-AOK's own filesystem, which is a real host-backed
# mount the app makes at startup. Decide once, here, whether it can be written
# to, rather than letting every later mkdir and cp fail on its own -- and KEEP
# THE REASON, because "not writable" on a directory that ls shows as drwxrwxrwx
# is not a diagnosis, it is the start of one.
PERSIST_AVAILABLE=1
PERSIST_WHY=""
if [ ! -d /AOK/persist ]; then
    PERSIST_AVAILABLE=0
    if [ -e /AOK/persist ]; then
        PERSIST_WHY="/AOK/persist exists but is not a directory"
    elif [ -d /AOK ]; then
        PERSIST_WHY="/AOK/persist does not exist (an iSH-AOK that does not mount it?)"
    else
        PERSIST_WHY="there is no /AOK here at all"
    fi
else
    # NOT mkdir -p. /AOK itself is a read-only mount with the writable one
    # underneath it, and an iSH-AOK that answers EROFS rather than EEXIST for
    # an ancestor that is already there stops -p on /AOK -- a component nobody
    # asked it to create. The parent is guaranteed by the -d test above, so the
    # final component is the only one to make.
    PERSIST_WHY="$(mkdir "$PERSIST_DIR" 2>&1)"
    if [ -d "$PERSIST_DIR" ]; then
        PERSIST_WHY=""
    else
        PERSIST_AVAILABLE=0
        [ -n "$PERSIST_WHY" ] || PERSIST_WHY="mkdir $PERSIST_DIR failed without saying why"
    fi
fi

# ---- must be root --------------------------------------------------------
if [ "$(id -u)" != 0 ]; then
    echo "This script must run as root:  sudo sh $0" >&2
    echo "(on a fresh image sudo does not work yet -- log in as root instead)" >&2
    exit 1
fi

# ---- must be a PSCAL rootfs ----------------------------------------------
# Naming a wrong-rootfs mistake here beats rewriting /etc/passwd on a Devuan
# install because the wrong provisioner was pasted.
if [ ! -f /etc/pscal-release ] && [ ! -x /usr/bin/exsh ]; then
    echo "This does not look like a PSCAL rootfs (no /etc/pscal-release, no /usr/bin/exsh)." >&2
    echo "For Devuan/Alpine/Arch use the matching /AOK/tools/provision-ultimate-*.sh." >&2
    exit 1
fi

log()  { printf '\n\033[1;36m==>\033[0m \033[1m%s\033[0m\n' "$*"; }
note() { printf '    %s\n' "$*"; }
warn() { printf '    \033[1;33m!\033[0m %s\n' "$*"; }

# ---- config (env overrides; prompts interactively when run on a TTY) ------
NEW_HOSTNAME="${NEW_HOSTNAME:-}"
AUTHORIZED_KEY="${AUTHORIZED_KEY:-}"

# ask <var> <prompt> <default>: keep an env-provided value; else prompt on a
# TTY; else use the default (so piped/ssh runs never block).
ask() {
    eval "_cur=\${$1:-}"
    [ -n "$_cur" ] && return
    if [ -t 0 ]; then
        printf '%s [%s]: ' "$2" "$3"
        read _a || _a=""
        [ -n "$_a" ] || _a="$3"
    else
        _a="$3"
    fi
    eval "$1=\$_a"
}

# ask_yn <var> <prompt> <default 0|1>: same, for yes/no tunables.
ask_yn() {
    eval "_cur=\${$1:-}"
    [ -n "$_cur" ] && return
    if [ "$3" = 1 ]; then _d=Y/n; else _d=y/N; fi
    if [ -t 0 ]; then
        printf '%s [%s]: ' "$2" "$_d"
        read _a || _a=""
    else
        _a=""
    fi
    case "$_a" in
        [Yy]*) _v=1 ;;
        [Nn]*) _v=0 ;;
        *)     _v=$3 ;;
    esac
    eval "$1=\$_v"
}

# The placeholder login the image ships: uid 1000, a locked password and an
# EMPTY home directory. It is a name reserved for whoever installs this, not
# an account anyone uses -- claim it rather than leaving it as litter and
# putting the real login at 1001.
PLACEHOLDER_USER=username

DEF_USER="${TARGET_USER:-}"
if [ -z "$DEF_USER" ]; then
    DEF_USER="$(awk -F: -v ph="$PLACEHOLDER_USER" \
        '$3>=1000 && $3<65000 && $1!=ph {print $1; exit}' /etc/passwd)"
fi
[ -n "$DEF_USER" ] || DEF_USER="$(cat "$PERSIST_DIR/username" 2>/dev/null)"
[ -n "$DEF_USER" ] || DEF_USER=aok
DEF_HOSTNAME="$(cat "$PERSIST_DIR/hostname" 2>/dev/null)"
[ -n "$DEF_HOSTNAME" ] || DEF_HOSTNAME="$(cat /etc/hostname 2>/dev/null)"
[ -n "$DEF_HOSTNAME" ] || DEF_HOSTNAME=pscal-ish

ask TARGET_USER  "Primary login username to set up" "$DEF_USER"
ask NEW_HOSTNAME "Hostname"                         "$DEF_HOSTNAME"

if [ "$PERSIST_AVAILABLE" = 0 ]; then
    PERSIST_SSH=0
elif [ -n "$(ls -A "$PERSIST_DIR" 2>/dev/null)" ]; then
    note "found a previous setup in $PERSIST_DIR -- it will be reused"
fi
ask_yn PERSIST_SSH   "Keep the SSH identity in /AOK/persist across image updates" 1
if [ "$PERSIST_SSH" = 1 ] && [ "$PERSIST_AVAILABLE" = 0 ]; then
    PERSIST_SSH=0
fi
ask_yn PASSWORD_AUTH "Allow SSH password authentication"                          1
ask_yn NATIVE_LINKS  "Also link iSH-AOK's native programs into PATH"              0

case "$TARGET_USER" in
    ''|root|*[!a-z0-9_-]*)
        echo "Refusing '$TARGET_USER': pick a lowercase login name that is not root." >&2
        exit 1 ;;
esac

# ===========================================================================
log "Accounts"
# ===========================================================================

# Rewrite one of the account files through a temp file in the same directory:
# no sed -i (SmallCLUE's sed has no such flag), and a rename rather than a
# truncate-and-write so a failure half way leaves the original intact.
rewrite_file() {
    _dst="$1"; _mode="$2"
    cat > "$_dst.aok-new" || return 1
    chmod "$_mode" "$_dst.aok-new" || return 1
    mv "$_dst.aok-new" "$_dst"
}

user_exists() { awk -F: -v u="$1" '$1==u {found=1} END {exit !found}' /etc/passwd; }

# dir_is_empty: true when the path is absent, or is a directory with nothing
# in it. The image ships /home/username as an empty directory, so "no home"
# is not the test -- "nobody has put anything there" is.
dir_is_empty() {
    [ -d "$1" ] || return 0
    [ -z "$(ls -A "$1" 2>/dev/null)" ]
}

# Claim the shipped placeholder, keeping uid 1000, but ONLY while it is still
# untouched: a locked password and an empty home. Anything else means somebody
# is using it and it is not ours to rename.
if [ "$TARGET_USER" != "$PLACEHOLDER_USER" ] && user_exists "$PLACEHOLDER_USER" \
   && ! user_exists "$TARGET_USER" \
   && dir_is_empty "/home/$PLACEHOLDER_USER" \
   && awk -F: -v u="$PLACEHOLDER_USER" '$1==u && ($2=="*"||$2=="!") {ok=1} END {exit !ok}' /etc/shadow
then
    awk -F: -v OFS=: -v ph="$PLACEHOLDER_USER" -v u="$TARGET_USER" \
        '$1==ph {$1=u; $5=u",,,"; $6="/home/"u} {print}' /etc/passwd \
        | rewrite_file /etc/passwd 644
    awk -F: -v OFS=: -v ph="$PLACEHOLDER_USER" -v u="$TARGET_USER" \
        '$1==ph {$1=u} {print}' /etc/group | rewrite_file /etc/group 644
    awk -F: -v OFS=: -v ph="$PLACEHOLDER_USER" -v u="$TARGET_USER" \
        '$1==ph {$1=u} {print}' /etc/shadow | rewrite_file /etc/shadow 600
    [ -d "/home/$PLACEHOLDER_USER" ] && rmdir "/home/$PLACEHOLDER_USER" 2>/dev/null
    note "claimed the shipped placeholder login '$PLACEHOLDER_USER' as '$TARGET_USER' (uid 1000)"
fi

if ! user_exists "$TARGET_USER"; then
    NEW_UID="$(awk -F: '$3>=1000 && $3<65000 {if ($3+0>m) m=$3+0} END {print (m?m+1:1000)}' /etc/passwd)"
    printf '%s:x:%s:%s:%s,,,:/home/%s:/usr/bin/exsh\n' \
        "$TARGET_USER" "$NEW_UID" "$NEW_UID" "$TARGET_USER" "$TARGET_USER" >> /etc/passwd
    printf '%s:x:%s:\n' "$TARGET_USER" "$NEW_UID" >> /etc/group
    printf '%s:*:::::::\n' "$TARGET_USER" >> /etc/shadow
    note "created login '$TARGET_USER' (uid $NEW_UID)"
fi

# sudo authorisation. SUDO_ASKS records whether the rule that ended up
# covering this login is the password-asking kind, so the summary can say so
# and give the line that changes it.
SUDO_ASKS=0
#
# Newer images ship /etc/sudoers and a wheel group; older ones predate sudo
# reading a policy at all, and on those sudo now refuses everybody. Create what
# is missing rather than telling someone to go and write it, so re-running this
# after an iSH-AOK update is enough on a root that is already installed.
if [ ! -f /etc/sudoers ]; then
    mkdir -p /etc/sudoers.d
    chmod 750 /etc/sudoers.d
    cat > /etc/sudoers <<'SUDOERS'
# Who may run what, as whom. SmallCLUE's sudo implements a subset of
# sudoers(5): user and %group entries, host lists, (runas) specs,
# NOPASSWD/PASSWD, ALL or an explicit list of absolute command paths,
# #includedir, and last-match-wins. Aliases, negation and globs are NOT
# implemented and a line using them is skipped rather than guessed at.
#
# Two forms, and the difference is the whole choice:
#
#   someone ALL=(ALL:ALL) ALL             asks for SOMEONE'S OWN password
#                                         (never root's -- that is su's question)
#   someone ALL=(ALL:ALL) NOPASSWD: ALL   never asks
#
# There is no credential cache yet, so the first form asks EVERY time rather
# than once per terminal the way sudo elsewhere does.
root   ALL=(ALL:ALL) ALL
%wheel ALL=(ALL:ALL) ALL

#includedir /etc/sudoers.d
SUDOERS
    chmod 440 /etc/sudoers
    note "created /etc/sudoers (root, and anyone in wheel)"
fi
if ! awk -F: '$1=="wheel"{f=1} END{exit !f}' /etc/group; then
    printf 'wheel:x:10:\n' >> /etc/group
    note "created the wheel group"
fi

# The whole job is then putting this login in wheel -- appended to the member
# list rather than replacing it, and only when it is not already there.
if [ -f /etc/sudoers ] && awk -F: '$1=="wheel"{f=1} END{exit !f}' /etc/group; then
    if ! awk -F: -v u="$TARGET_USER" '$1=="wheel" {n=split($4,m,","); for(i=1;i<=n;i++) if (m[i]==u) f=1} END{exit !f}' /etc/group; then
        awk -F: -v OFS=: -v u="$TARGET_USER" \
            '$1=="wheel" {$4 = ($4=="" ? u : $4","u)} {print}' /etc/group \
            | rewrite_file /etc/group 644
        note "added $TARGET_USER to wheel (sudo: %wheel ALL=(ALL:ALL) ALL)"
        SUDO_ASKS=1
    else
        note "$TARGET_USER is already in wheel"
    fi
elif [ -f /etc/sudoers ]; then
    # No wheel group to join, so say so in the one place sudo will look.
    if [ ! -f "/etc/sudoers.d/10-$TARGET_USER" ]; then
        mkdir -p /etc/sudoers.d
        printf '%s ALL=(ALL:ALL) ALL\n' "$TARGET_USER" > "/etc/sudoers.d/10-$TARGET_USER"
        chmod 440 "/etc/sudoers.d/10-$TARGET_USER"
        note "authorised $TARGET_USER in /etc/sudoers.d/10-$TARGET_USER"
        SUDO_ASKS=1
    fi
else
    warn "no /etc/sudoers on this image -- sudo will refuse everyone until one exists"
fi

USER_UID="$(awk -F: -v u="$TARGET_USER" '$1==u {print $3}' /etc/passwd)"
USER_GID="$(awk -F: -v u="$TARGET_USER" '$1==u {print $4}' /etc/passwd)"
USER_HOME="$(awk -F: -v u="$TARGET_USER" '$1==u {print $6}' /etc/passwd)"
[ -n "$USER_HOME" ] || USER_HOME="/home/$TARGET_USER"

mkdir -p "$USER_HOME/.ssh"
chmod 755 "$USER_HOME"
chmod 700 "$USER_HOME/.ssh"
chown -R "$USER_UID:$USER_GID" "$USER_HOME" 2>/dev/null || true
chmod 600 /etc/shadow 2>/dev/null || true

# Passwords last, because passwd is interactive: everything that can be done
# without the operator is already done by the time it asks.
# `locked` is the state the image ships in -- '*' or '!' in the shadow field.
locked() { awk -F: -v u="$1" '$1==u && ($2=="*"||$2=="!"||$2=="") {y=1} END {exit !y}' /etc/shadow; }

if locked root; then
    if [ -t 0 ]; then
        log "Set the ROOT password (su asks for it; sudo asks for yours)"
        passwd root || warn "root password unchanged -- sudo and su will keep refusing"
    else
        warn "root is still locked and there is no terminal to ask on; sudo and su will refuse"
    fi
else
    note "root already has a password"
fi

if locked "$TARGET_USER"; then
    if [ -t 0 ]; then
        log "Set the password for '$TARGET_USER' (sudo will ask for THIS one)"
        passwd "$TARGET_USER" || warn "$TARGET_USER left without a password (key login still works)"
    else
        warn "$TARGET_USER is still locked and there is no terminal to ask on"
    fi
else
    note "$TARGET_USER already has a password"
fi

# ===========================================================================
log "SSH identity"
# ===========================================================================
mkdir -p /etc/ssh
if [ "$PERSIST_AVAILABLE" = 0 ]; then
    warn "not using /AOK/persist -- nothing will be kept across images"
    warn "reason: $PERSIST_WHY"
fi

# Host keys. Restoring them is what stops the client shouting REMOTE HOST
# IDENTIFICATION HAS CHANGED after every image update; /etc/service/sshd/run
# generates a fresh set only when /etc/ssh/ssh_host_rsa_key is absent, so
# putting the old ones back before sshd starts is all it takes.
RESTORED=0
GENERATED=0
for t in rsa ecdsa ed25519; do
    k="/etc/ssh/ssh_host_${t}_key"
    p="$PERSIST_DIR/ssh_host_${t}_key"
    if [ ! -f "$k" ] && [ "$PERSIST_SSH" = 1 ] && [ -f "$p" ]; then
        cp "$p" "$k" && chmod 600 "$k"
        [ -f "$p.pub" ] && cp "$p.pub" "$k.pub" && chmod 644 "$k.pub"
        RESTORED=$((RESTORED + 1))
    fi
    if [ ! -f "$k" ]; then
        ssh-keygen -t "$t" -f "$k" -N "" -q >/dev/null 2>&1 \
            && GENERATED=$((GENERATED + 1))
    fi
    # Copy out unconditionally, not just for the ones just made: a host key
    # that predates this script (hand-copied from another rootfs, say) is
    # exactly the identity worth keeping, and it is not in the stash yet.
    if [ "$PERSIST_SSH" = 1 ] && [ -f "$k" ]; then
        cp "$k" "$p" 2>/dev/null && chmod 600 "$p" 2>/dev/null
        [ -f "$k.pub" ] && cp "$k.pub" "$p.pub" 2>/dev/null
    fi
done
[ "$RESTORED"  -gt 0 ] && note "restored $RESTORED host key(s) from $PERSIST_DIR"
[ "$GENERATED" -gt 0 ] && note "generated $GENERATED new host key(s)"
if [ "$PERSIST_SSH" = 1 ]; then
    note "host keys stashed in $PERSIST_DIR (host-backed, survives a root switch)"
fi

# authorized_keys: the stash first, then the environment, then the operator.
AUTH_FILE="$USER_HOME/.ssh/authorized_keys"
if [ -z "$AUTHORIZED_KEY" ] && [ "$PERSIST_SSH" = 1 ] && [ -f "$PERSIST_DIR/authorized_keys" ] \
   && [ ! -s "$AUTH_FILE" ]; then
    cp "$PERSIST_DIR/authorized_keys" "$AUTH_FILE"
    note "restored authorized_keys from $PERSIST_DIR"
fi
if [ -z "$AUTHORIZED_KEY" ] && [ ! -s "$AUTH_FILE" ] && [ -t 0 ]; then
    printf '    Paste a public key for %s (blank to skip): ' "$TARGET_USER"
    read AUTHORIZED_KEY || AUTHORIZED_KEY=""
fi
if [ -n "$AUTHORIZED_KEY" ]; then
    case "$AUTHORIZED_KEY" in
        ssh-*|ecdsa-*|sk-*)
            # Appended, not overwritten: a second device gets a second key,
            # and re-running this must never throw the first one away.
            if ! grep -qF "$AUTHORIZED_KEY" "$AUTH_FILE" 2>/dev/null; then
                printf '%s\n' "$AUTHORIZED_KEY" >> "$AUTH_FILE"
                note "added a public key to $AUTH_FILE"
            fi ;;
        *)  warn "that does not look like a public key line; skipped" ;;
    esac
fi
if [ -s "$AUTH_FILE" ]; then
    chmod 600 "$AUTH_FILE"
    chown "$USER_UID:$USER_GID" "$AUTH_FILE" 2>/dev/null || true
    [ "$PERSIST_SSH" = 1 ] && cp "$AUTH_FILE" "$PERSIST_DIR/authorized_keys" 2>/dev/null
else
    warn "no authorized_keys for $TARGET_USER -- password login only"
fi

# ---- sshd_config ---------------------------------------------------------
# set_sshd_option replaces the first setting of a keyword and appends it if the
# file never mentions it. Keywords are matched case-insensitively because
# sshd reads them that way, and only the FIRST setting counts to sshd, so
# rewriting that one is what actually changes behaviour.
set_sshd_option() {
    _k="$1"; _v="$2"; _f=/etc/ssh/sshd_config
    [ -f "$_f" ] || return 0
    awk -v k="$_k" -v v="$_v" '
        tolower($1)==tolower(k) { if (!seen) { print k " " v; seen=1 } ; next }
        { print }
        END { if (!seen) print k " " v }
    ' "$_f" | rewrite_file "$_f" 644
}

# The shipped file says `AuthorizedKeysFile /root/.ssh/authorized_keys`: an
# absolute path, so EVERY user's keys are looked for in root's file. That is
# why a key copied to ~/.ssh worked only when it was also in root's, and it
# means one file authorises logins as anybody. Make it per-user.
set_sshd_option AuthorizedKeysFile ".ssh/authorized_keys"
if [ "$PASSWORD_AUTH" = 1 ]; then
    set_sshd_option PasswordAuthentication yes
    note "sshd: password authentication enabled"
else
    set_sshd_option PasswordAuthentication no
    note "sshd: keys only"
fi
note "sshd: AuthorizedKeysFile is now per-user (.ssh/authorized_keys)"

# ===========================================================================
log "System"
# ===========================================================================
if [ -n "$NEW_HOSTNAME" ]; then
    printf '%s\n' "$NEW_HOSTNAME" > /etc/hostname
    hostname "$NEW_HOSTNAME" 2>/dev/null || true
    # /etc/hosts must follow, or every command that resolves its own hostname
    # waits for a DNS timeout first.
    if ! grep -q "[[:space:]]$NEW_HOSTNAME\$" /etc/hosts 2>/dev/null; then
        printf '127.0.0.1\t%s\n' "$NEW_HOSTNAME" >> /etc/hosts
    fi
    note "hostname set to $NEW_HOSTNAME"
fi

if [ "$NATIVE_LINKS" = 1 ] && [ -f /AOK/tools/native-links.sh ]; then
    sh /AOK/tools/native-links.sh
fi

# Remember the choices so the next image can offer them back.
if [ "$PERSIST_SSH" = 1 ]; then
    printf '%s\n' "$TARGET_USER" > "$PERSIST_DIR/username" 2>/dev/null || true
    [ -n "$NEW_HOSTNAME" ] && \
        printf '%s\n' "$NEW_HOSTNAME" > "$PERSIST_DIR/hostname" 2>/dev/null || true
fi

# ---- restart sshd so the config above is what is actually running --------
# runit re-runs /etc/service/sshd/run when the supervised process dies, so
# killing sshd IS the restart. No sv/svc applet in this image to ask nicely.
# The pid is resolved FIRST and checked for being a number, because `kill` here
# reads an empty argument as 0 -- which signals the whole process group, this
# script included. An untested version of this line killed itself with SIGHUP.
SSHD_PID=""
[ -f /var/run/sshd.pid ] && SSHD_PID="$(cat /var/run/sshd.pid 2>/dev/null)"
case "$SSHD_PID" in
    ''|*[!0-9]*) SSHD_PID="$(ps 2>/dev/null | awk '$1 ~ /^[0-9]+$/ && /[s]shd -D/ {print $1; exit}')" ;;
esac
case "$SSHD_PID" in
    ''|0|*[!0-9]*) SSHD_PID="" ;;
esac

if [ -f /etc/ssh/sshd.disable ]; then
    warn "sshd is disabled (/etc/ssh/sshd.disable); remove that file to enable it"
elif [ -n "$SSHD_PID" ] && kill "$SSHD_PID" 2>/dev/null; then
    # Killing the listener IS the restart: runit re-runs /etc/service/sshd/run
    # as soon as the supervised process dies, and there is no sv applet here to
    # ask more politely. Sessions already established are their own processes
    # and are not affected -- including the one you may be reading this over.
    note "sshd restarted (runit brings it straight back)"
else
    note "sshd was not running; runit starts it at boot"
fi

# The image's /etc/profile prints a "set this machine up" hint to root until
# this file exists. Written last, so a run that died half way still nags.
: > /etc/pscal-provisioned 2>/dev/null || true

# ===========================================================================
log "Done"
# ===========================================================================
note "login:    $TARGET_USER  (uid $USER_UID, home $USER_HOME, shell /usr/bin/exsh)"
if [ "$SUDO_ASKS" = 1 ]; then
    note "sudo:     asks $TARGET_USER for their OWN password, every time"
    note "          (su asks for root's; they are different questions)"
    note "          never ask instead:  echo '$TARGET_USER ALL=(ALL:ALL) NOPASSWD: ALL' > /etc/sudoers.d/20-$TARGET_USER"
fi
note "hostname: $(cat /etc/hostname 2>/dev/null)"
if [ "$PERSIST_SSH" = 1 ]; then
    note "stash:    $PERSIST_DIR -- re-run this after the next image update and"
    note "          the host keys, your authorized_keys and this username come back"
else
    note "stash:    disabled; the next image gets a new SSH host identity"
fi
locked root && warn "root is STILL locked, so 'su -' cannot work until it has a password"
locked "$TARGET_USER" && warn "$TARGET_USER has no password, so sudo cannot authenticate them"
printf '\n'
