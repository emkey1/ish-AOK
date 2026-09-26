#!/bin/sh
# setup-wayland-extras.sh
# ---------------------------------------------------------------------------
# Optional programs for the Workspace "Display" applet's Wayland desktop, in
# three sets. Every program in them was tried on AOK: each starts in a few
# seconds, idles near 0% CPU and draws correctly over VNC. They show up in the
# desktop's Applications menu (right-click the desktop) under their category.
# Run /AOK/tools/setup-wayland.sh first; that installs the desktop itself.
#
#   games  sgt-puzzles (about 40 small puzzles), AisleRiot solitaire, Mines,
#          Chess with the GNU Chess engine, Sudoku and Reversi (iagno), plus
#          terminal games to play in foot: NetHack, bsdgames, nudoku and
#          moon-buggy
#   tools  fuzzel, which the root menu's Launcher and Alt+Shift+D open; mako,
#          which shows notify-send notifications; wl-clipboard (wl-copy,
#          wl-paste); grim and slurp for screenshots; swayimg for images;
#          zathura for PDFs; a text editor; pcmanfm for files; galculator;
#          and the dillo web browser
#   x11    Xwayland, xterm and the core X fonts that programs speaking only
#          X11 need, with xeyes, xclock and xcalc to try it on
#
# Run as root, naming one or more sets, or all of them:
#       sudo sh /AOK/tools/setup-wayland-extras.sh games tools
#       sudo sh /AOK/tools/setup-wayland-extras.sh all
#
# Package names differ between Devuan, Alpine and Arch, and not everything is
# packaged everywhere: Alpine has no sgt-puzzles, iagno, nudoku, moon-buggy or
# l3afpad (mousepad is its editor), and Arch has no nudoku or moon-buggy. When
# a package cannot be installed, the rest of its set still is, and the ones
# missing are listed at the end.
#
# Recommended packages are not installed on Devuan: with them, most GNOME
# games pull in a help browser and WebKit, about 450 MB. The icons GTK games
# draw need the SVG loader (librsvg2-common), which is in the games set, and
# X programs need the core fonts, which are in x11. Left out after trying
# them: SDL action games, which run but draw 2 to 6 frames a second over VNC.
# ---------------------------------------------------------------------------
set -u

log()  { printf '\n\033[1;36m==>\033[0m \033[1m%s\033[0m\n' "$*"; }
note() { printf '    %s\n' "$*"; }
die()  { printf 'setup-wayland-extras.sh: %s\n' "$*" >&2; exit 1; }

# apk add, with every package downloaded whole before any is installed.
# apk-tools 3 (Alpine 3.23+) streams each package from the mirror straight into
# extraction, so the connection is read only as fast as files are written. Under
# emulation that is slow, and if the app is suspended mid-install it stops
# altogether; a mirror drops a connection left unread for a few minutes
# (measured: a reader paused for 3 minutes lost the rest of a 68 MB transfer),
# and apk reports that as "I/O error" on whatever it was extracting.
# --cache-predownload fetches the whole transaction into the cache first
# (/var/cache/apk when /etc/apk/cache is not set up) and installs from there; a
# failed download stops the run before anything is installed, and a retry keeps
# what already arrived. Up to three tries. The downloaded packages are deleted
# afterwards unless the system keeps an apk cache of its own. apk-tools 2 has no
# such option and streams as it always did.
apk_add() {
    _apk_opts=""
    _apk_purge=""
    case "$(apk --version 2>/dev/null)" in
        "apk-tools 3."*)
            _apk_opts="--cache-predownload"
            [ -e /etc/apk/cache ] || _apk_purge=/var/cache/apk
            ;;
    esac
    _apk_try=1
    while :; do
        apk add $_apk_opts "$@"
        _apk_rc=$?
        [ "$_apk_rc" -eq 0 ] && break
        [ "$_apk_try" -ge 3 ] && break
        _apk_try=$((_apk_try + 1))
        note "apk add failed; trying again ($_apk_try of 3)"
    done
    [ -n "$_apk_purge" ] && rm -f "$_apk_purge"/*.apk
    return "$_apk_rc"
}

usage() {
    cat <<'USAGE_EOF'
usage: sudo sh /AOK/tools/setup-wayland-extras.sh SET...

  games   card, board and puzzle games, and terminal games to play in foot
  tools   launcher, notifications, clipboard, screenshots, image and PDF
          viewers, text editor, file manager, calculator, web browser
  x11     Xwayland, xterm and the X fonts, for programs that only speak X11
  all     all three
USAGE_EOF
}

[ $# -gt 0 ] || { usage >&2; exit 2; }
WANT_GAMES=0
WANT_TOOLS=0
WANT_X11=0
for set_name in "$@"; do
    case "$set_name" in
        games) WANT_GAMES=1 ;;
        tools) WANT_TOOLS=1 ;;
        x11) WANT_X11=1 ;;
        all) WANT_GAMES=1; WANT_TOOLS=1; WANT_X11=1 ;;
        -h|--help|help) usage; exit 0 ;;
        *) usage >&2; die "unknown set '$set_name'" ;;
    esac
done

[ "$(id -u)" = 0 ] || die "must run as root:  sudo sh $0 $*"
command -v labwc >/dev/null 2>&1 \
    || note "labwc is not installed: these are for the desktop /AOK/tools/setup-wayland.sh installs"

if command -v apt-get >/dev/null 2>&1; then
    PM=apt
    GAMES="sgt-puzzles aisleriot gnome-mines gnome-chess gnuchess iagno gnome-sudoku librsvg2-common
           bsdgames nethack-console nudoku moon-buggy"
    TOOLS="fuzzel mako-notifier libnotify-bin wl-clipboard grim slurp swayimg zathura zathura-pdf-poppler
           l3afpad pcmanfm galculator dillo"
    X11="xwayland xterm xfonts-base x11-apps"
elif command -v pacman >/dev/null 2>&1; then
    PM=pacman
    GAMES="puzzles aisleriot gnome-mines gnome-chess gnuchess iagno gnome-sudoku librsvg
           bsd-games nethack"
    TOOLS="fuzzel mako libnotify wl-clipboard grim slurp swayimg zathura zathura-pdf-poppler
           l3afpad pcmanfm galculator dillo"
    X11="xorg-xwayland xterm xorg-fonts-misc xorg-fonts-alias-misc xorg-xeyes xorg-xclock xorg-xcalc"
elif command -v apk >/dev/null 2>&1; then
    PM=apk
    GAMES="aisleriot gnome-mines gnome-chess gnuchess gnome-sudoku librsvg
           bsd-games nethack"
    TOOLS="fuzzel mako libnotify wl-clipboard grim slurp swayimg zathura zathura-pdf-mupdf
           mousepad pcmanfm galculator dillo"
    X11="xwayland xterm font-misc-misc font-cursor-misc font-alias xeyes xclock xcalc"
else
    die "no supported package manager found (need apt-get, pacman, or apk)"
fi

pm_install() {
    case "$PM" in
        apt)
            DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
                -o Dpkg::Options::=--force-confdef -o Dpkg::Options::=--force-confold "$@"
            ;;
        pacman) pacman -S --needed --noconfirm "$@" ;;
        apk) apk_add "$@" ;;
    esac
}

# Judged by what is installed afterwards, not by exit status: apk exits 1 when
# any package already in the root is broken, even though the new ones installed.
pm_has() {
    case "$PM" in
        apt) [ "$(dpkg-query -W -f='${db:Status-Status}' "$1" 2>/dev/null)" = installed ] ;;
        pacman) pacman -Q "$1" >/dev/null 2>&1 ;;
        apk) apk info -e "$1" >/dev/null 2>&1 ;;
    esac
}

NOT_INSTALLED=""

# A set goes in as one transaction. A name the distribution does not carry
# fails the whole transaction with all three package managers, so whatever is
# still missing afterwards is tried again one package at a time, and the rest
# of the set arrives anyway.
install_set() {
    set_label="$1"
    shift
    log "installing $set_label: $*"
    pm_install "$@"
    set_missing=""
    for pkg in "$@"; do
        pm_has "$pkg" || set_missing="$set_missing $pkg"
    done
    [ -n "$set_missing" ] || return 0
    note "not installed:$set_missing -- trying each on its own"
    for pkg in $set_missing; do
        pm_install "$pkg"
        pm_has "$pkg" || NOT_INSTALLED="$NOT_INSTALLED $pkg"
    done
}

log "updating the package index"
case "$PM" in
    apt) apt-get update ;;
    pacman) pacman -Sy --noconfirm ;;
    apk) apk update ;;
esac || die "updating the package index failed -- check network/DNS (guest /etc/resolv.conf)"

# The lists above are left unquoted on purpose: one word per package.
[ "$WANT_GAMES" = 1 ] && install_set games $GAMES
[ "$WANT_TOOLS" = 1 ] && install_set tools $TOOLS
[ "$WANT_X11" = 1 ] && install_set x11 $X11

log "done"
if [ -n "$NOT_INSTALLED" ]; then
    note "could not install:$NOT_INSTALLED"
    note "(see the package manager's output above; everything else is installed)"
fi
note "Programs with a menu entry are in the desktop's Applications menu, by category."
if [ "$WANT_GAMES" = 1 ]; then
    note "Terminal games run in foot: nethack, and on Devuan nudoku and moon-buggy."
    note "bsdgames adds more commands, such as adventure, robots and worm."
fi
if [ "$WANT_TOOLS" = 1 ]; then
    note "The root menu's Launcher and Alt+Shift+D open fuzzel. notify-send starts mako itself."
fi
if [ "$WANT_X11" = 1 ]; then
    note "An X program starts Xwayland the first time, which takes a few seconds."
fi
exit 0
