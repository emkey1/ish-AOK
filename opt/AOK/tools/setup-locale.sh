#!/bin/sh
# setup-locale.sh
# ---------------------------------------------------------------------------
# Give the guest the device's language. Every root starts in C.UTF-8, which
# shows Chinese (or any other) text fine but leaves every program speaking
# English. This makes a real locale the default:
#
#   - Devuan/Debian and Arch (glibc): installs `locales` where needed and
#     generates the locale; glibc needs the data before it will use it.
#   - Alpine (musl): installs musl-locales, which carries the C library's
#     messages and the `locale` command. musl needs no generated data.
#   - Everywhere: names it as the default for logins (PAM, ssh, busybox login,
#     zsh) and for the Wayland desktop, through /etc/aok-locale.
#
# Programs translate their own messages only when the distribution ships
# them. Devuan does, in the packages themselves: after this, `ls` errors and
# `date` come out in Chinese. Alpine mostly does not -- busybox has no
# translations, few packages have a *-lang subpackage, and musl-locales covers
# European languages only (no zh, ja or ko) -- so there the locale is named
# but most text stays English. For a Chinese system, use a Devuan root.
#
# Which locale: the first language in /proc/ish/languages (the device's
# Settings > General > Language & Region order), mapped to the matching glibc
# name -- zh-Hans-CN -> zh_CN.UTF-8, zh-Hant-TW -> zh_TW.UTF-8,
# ja-JP -> ja_JP.UTF-8 -- or the one named on the command line.
#
# Usage:
#   sudo sh /AOK/tools/setup-locale.sh             the device's language
#   sudo sh /AOK/tools/setup-locale.sh zh_TW       a locale you name (.UTF-8 is added;
#                                                  ja, zh-Hant, pt-BR work too)
#   sudo sh /AOK/tools/setup-locale.sh --fonts     also install CJK fonts for the desktop
#   sudo sh /AOK/tools/setup-locale.sh --reset     back to C.UTF-8
#   sh /AOK/tools/setup-locale.sh --show           print what it would set (no root needed)
#
# Takes effect in new logins and new desktop sessions; run `exec $SHELL -l`
# (or open a new terminal) to pick it up now.
# ---------------------------------------------------------------------------
set -u

PROG=setup-locale.sh
CONF=/etc/aok-locale
PROFILE=/etc/profile.d/01-aok-lang.sh
MARK_BEGIN='# >>> iSH-AOK setup-locale.sh >>>'
MARK_END='# <<< iSH-AOK setup-locale.sh <<<'

die() { echo "$PROG: $*" >&2; exit 1; }
note() { echo "$PROG: $*"; }

# A BCP 47 tag (zh-Hans-CN, en-GB, pt-BR, ja) as a glibc locale name.
tag_to_locale() {
    tag=$1
    lang=$(printf '%s' "${tag%%-*}" | tr 'A-Z' 'a-z')
    script=
    region=
    rest=${tag#"${tag%%-*}"}
    for part in $(printf '%s' "$rest" | tr -- '-_' '  '); do
        case $part in
            [A-Za-z][a-z][a-z][a-z]) script=$part ;;
            [A-Za-z][A-Za-z]) region=$(printf '%s' "$part" | tr 'a-z' 'A-Z') ;;
            419) [ "$lang" = es ] && region=MX ;;  # Latin America
        esac
    done
    case $lang in
        zh)
            # Script first: Traditional Chinese written in mainland China is
            # still Traditional. glibc has zh_CN, zh_SG, zh_TW and zh_HK.
            case $script in
                Hant) case $region in HK|MO) region=HK ;; *) region=TW ;; esac ;;
                Hans) case $region in SG) ;; *) region=CN ;; esac ;;
                *) case $region in TW|HK|SG|CN) ;; MO) region=HK ;; *) region=CN ;; esac ;;
            esac ;;
        "") return 1 ;;
    esac
    if [ -z "$region" ]; then
        case $lang in
            en) region=US ;; ja) region=JP ;; ko) region=KR ;; pt) region=BR ;;
            sv) region=SE ;; da) region=DK ;; cs) region=CZ ;; el) region=GR ;;
            uk) region=UA ;; he) region=IL ;; ar) region=SA ;; vi) region=VN ;;
            hi) region=IN ;; ms) region=MY ;; nb) region=NO ;; ca) region=ES ;;
            *) region=$(printf '%s' "$lang" | tr 'a-z' 'A-Z') ;;
        esac
    fi
    printf '%s_%s.UTF-8\n' "$lang" "$region"
}

# zh_CN, zh_CN.utf8, zh_CN.UTF-8 -> zh_CN.UTF-8; C.UTF-8 stays as it is. A
# name with no region (ja) or in BCP 47 form (zh-Hant, pt-BR) is mapped as a
# device language would be.
normalise() {
    case $1 in
        C|C.*|POSIX) echo C.UTF-8 ;;
        *_*) printf '%s.UTF-8\n' "${1%%.*}" ;;
        *) tag_to_locale "${1%%.*}" ;;
    esac
}

device_locale() {
    [ -r /proc/ish/languages ] || return 1
    first=$(sed -n '1p' /proc/ish/languages 2>/dev/null)
    [ -n "$first" ] || return 1
    tag_to_locale "$first"
}

FONTS=0
RESET=0
SHOW=0
WANT=
while [ $# -gt 0 ]; do
    case $1 in
        --fonts) FONTS=1 ;;
        --reset) RESET=1 ;;
        --show) SHOW=1 ;;
        -h|--help) sed -n '2,/^# ----.*$/{/^# ----/d;s/^# \{0,1\}//;p;}' "$0"; exit 0 ;;
        -*) die "unknown option $1 (see --help)" ;;
        *) WANT=$1 ;;
    esac
    shift
done

if [ "$RESET" = 1 ]; then
    LOCALE=C.UTF-8
elif [ -n "$WANT" ]; then
    LOCALE=$(normalise "$WANT")
else
    LOCALE=$(device_locale) || die "no device language in /proc/ish/languages; name a locale, e.g. $PROG zh_CN"
fi

if [ "$SHOW" = 1 ]; then
    [ -r /proc/ish/languages ] && sed 's/^/device language: /' /proc/ish/languages | head -3
    echo "locale: $LOCALE"
    [ -r "$CONF" ] && echo "current default: $(cat "$CONF")"
    exit 0
fi

[ "$(id -u)" = 0 ] || die "run as root (sudo sh /AOK/tools/$PROG ...)"

if [ -r /etc/alpine-release ]; then
    DISTRO=alpine
elif command -v apt-get >/dev/null 2>&1; then
    DISTRO=debian
elif command -v pacman >/dev/null 2>&1; then
    DISTRO=arch
else
    DISTRO=other
fi

# --- the locale's data ------------------------------------------------------
enable_glibc_locale() {
    gen=/etc/locale.gen
    name=${1%.UTF-8}
    [ -f "$gen" ] || : > "$gen"
    # "# zh_CN.UTF-8 UTF-8" -> "zh_CN.UTF-8 UTF-8"; added when it is not listed.
    if grep -q "^[#[:space:]]*$name\.UTF-8[[:space:]]\{1,\}UTF-8" "$gen"; then
        sed -i "s/^[#[:space:]]*\($name\.UTF-8[[:space:]]\{1,\}UTF-8\)/\1/" "$gen"
    else
        echo "$name.UTF-8 UTF-8" >> "$gen"
    fi
    if ! locale-gen; then
        die "locale-gen failed for $1 (is it a real locale? see /usr/share/i18n/SUPPORTED)"
    fi
}

if [ "$LOCALE" != C.UTF-8 ]; then
    case $DISTRO in
        debian)
            if ! command -v locale-gen >/dev/null 2>&1; then
                note "installing locales"
                DEBIAN_FRONTEND=noninteractive apt-get install -y locales ||
                    { apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y locales; } ||
                    die "could not install the locales package"
            fi
            if [ -f /usr/share/i18n/SUPPORTED ] &&
               ! grep -q "^${LOCALE%.UTF-8}\.UTF-8 UTF-8" /usr/share/i18n/SUPPORTED; then
                die "$LOCALE is not a locale glibc knows (see /usr/share/i18n/SUPPORTED)"
            fi
            enable_glibc_locale "$LOCALE"
            [ "$FONTS" = 1 ] && { DEBIAN_FRONTEND=noninteractive apt-get install -y fonts-noto-cjk || note "fonts-noto-cjk did not install"; } ;;
        arch)
            enable_glibc_locale "$LOCALE"
            [ "$FONTS" = 1 ] && { pacman -S --noconfirm --needed noto-fonts-cjk || note "noto-fonts-cjk did not install"; } ;;
        alpine)
            note "installing musl-locales"
            apk add musl-locales musl-locales-lang || note "musl-locales did not install; the locale is still named"
            [ "$FONTS" = 1 ] && { apk add font-noto-cjk || note "font-noto-cjk did not install"; } ;;
        *)
            note "unknown distribution: naming $LOCALE without installing anything" ;;
    esac
fi

# --- the default ------------------------------------------------------------
# One file says which locale; everything below reads it, so --reset or a
# second run changes one place. LANG only, never LC_ALL: LC_ALL would outrank
# any per-category variable a user sets (LC_TIME and so on).
if [ "$LOCALE" = C.UTF-8 ]; then
    rm -f "$CONF" "$PROFILE"
else
    echo "$LOCALE" > "$CONF"
    chmod 0644 "$CONF"
    mkdir -p /etc/profile.d
    cat > "$PROFILE" <<'EOF'
# Written by /AOK/tools/setup-locale.sh: the locale it set up, for logins that
# came up in the C.UTF-8 default (the app's own, or none). A LANG that names
# something else -- forwarded by ssh, set by PAM -- wins.
if [ -r /etc/aok-locale ]; then
    case "${LANG:-}" in
        ""|C|POSIX|C.UTF-8|C.utf8) read -r LANG < /etc/aok-locale && export LANG ;;
    esac
fi
EOF
    chmod 0644 "$PROFILE"
fi

# PAM logins (Devuan's login, su, sshd with UsePAM) read /etc/default/locale.
if [ -d /etc/default ] && { [ "$DISTRO" = debian ] || [ -f /etc/default/locale ]; }; then
    if command -v update-locale >/dev/null 2>&1; then
        update-locale LANG="$LOCALE" 2>/dev/null || printf 'LANG=%s\n' "$LOCALE" > /etc/default/locale
    else
        printf 'LANG=%s\n' "$LOCALE" > /etc/default/locale
    fi
fi
# Arch's equivalent.
if [ "$DISTRO" = arch ]; then
    printf 'LANG=%s\n' "$LOCALE" > /etc/locale.conf
fi

# zsh reads none of /etc/profile.d. The block goes in the global zshenv the
# app's own locale line uses: /etc/zsh/zshenv when the distribution has one,
# else /etc/zshenv (where native zsh looks).
zshenv=/etc/zshenv
[ -f /etc/zsh/zshenv ] && zshenv=/etc/zsh/zshenv
if [ -f "$zshenv" ] || command -v zsh >/dev/null 2>&1 || [ -x /AOK/native/zsh ]; then
    tmp=$(mktemp) || die "mktemp failed"
    [ -f "$zshenv" ] && sed "/^$MARK_BEGIN\$/,/^$MARK_END\$/d" "$zshenv" > "$tmp"
    if [ "$LOCALE" != C.UTF-8 ]; then
        {
            echo "$MARK_BEGIN"
            echo '[ -r /etc/aok-locale ] && case "${LANG:-}" in ""|C|POSIX|C.UTF-8|C.utf8) read -r LANG < /etc/aok-locale && export LANG ;; esac'
            echo "$MARK_END"
        } >> "$tmp"
    fi
    cat "$tmp" > "$zshenv"
    rm -f "$tmp"
    chmod 0644 "$zshenv"
fi

if [ "$LOCALE" = C.UTF-8 ]; then
    note "default locale is C.UTF-8 again"
else
    note "default locale is now $LOCALE (new logins and desktop sessions; 'exec \$SHELL -l' for this one)"
fi
