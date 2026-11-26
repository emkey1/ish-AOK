#!/bin/sh
# build-wf-panel.sh
# ---------------------------------------------------------------------------
# Build wf-shell's panel (wf-panel) from the distro's own source with one
# change, and install it as /usr/local/bin/wf-panel, ahead of the packaged one
# on PATH. Devuan/Debian only.
#
# The change: wf-panel 0.9 (and 0.10) throws away and rebuilds every button in
# its application menu each time the menu opens (WayfireMenu::on_popover_shown
# -> set_category("All") -> populate_menu_items). On a PC that is a blink; in
# AOK-Workstation, on an A10X iPad, it was ~3.5 s of CPU for ~100 applications, every
# single time (bip, 2026-10-03). Patched, the menu is rebuilt only when it is
# showing something other than "All" -- a category picked last time -- and
# otherwise just reopens. The list is still rebuilt when applications are
# installed or removed, as before.
#
# A second change: the clock. It set its label every second whatever its
# format showed, and GtkLabel lays out and redraws on every set_text, even of
# the same text -- so an idle desktop drew a panel frame, and the compositor
# a frame for the app, every second (wf-panel 0.64 s and wayfire 0.61 s of CPU
# in every 20 on an idle A10X, 2026-11-22). Patched, the label is set only
# when its text changes, and a clock whose format has no seconds wakes once a
# minute.
#
# Usage:
#   sudo sh /AOK/tools/wayland/build-wf-panel.sh            build and install
#   sudo sh /AOK/tools/wayland/build-wf-panel.sh --remove   back to the packaged wf-panel
#   sudo sh /AOK/tools/wayland/build-wf-panel.sh --out DIR  build only; leave wf-panel and
#                                                           wf-panel.source in DIR
#
# AOK-Workstation ships this build for Devuan 6 in /AOK/bundled, and setup-wayfire.sh
# links to it, so this is the fallback: for a wf-shell the bundled one was not
# built against. On an A10X iPad the compile ran out of memory; it needs a
# newer device (tools/build-bundled.sh builds the bundled ones on a Mac).
#
# Takes a while: the build dependencies (gtkmm and friends, which stay
# installed for the next rebuild) are installed and
# wf-panel is compiled under emulation -- minutes on an M-series iPad, much
# longer on older ones. After the distro updates wf-shell, run it again so the
# panel matches.
# ---------------------------------------------------------------------------
set -u

log()  { printf '\n\033[1;36m==>\033[0m \033[1m%s\033[0m\n' "$*"; }
note() { printf '    %s\n' "$*"; }
die()  { printf 'build-wf-panel.sh: %s\n' "$*" >&2; exit 1; }

# The binary, and the wf-shell version it was built from beside it (as in
# /AOK/bundled), with /usr/local/bin/wf-panel a link to it: start-wayland.sh
# reads the version through the link and falls back to the packaged panel once
# the distro's wf-shell has moved on.
BIN=/usr/local/bin/wf-panel
LIBBIN=/usr/local/lib/ish-wayland/wf-panel
STAMP=$LIBBIN.source
WORK=/usr/local/src/aok-wf-panel
SRCLIST=/etc/apt/sources.list.d/aok-wf-shell-src.list

[ "$(id -u)" = 0 ] || die "must run as root:  sudo sh $0 $*"

OUT=
case "${1:-}" in
    --remove)
        rm -f "$BIN" "$LIBBIN" "$STAMP"
        note "removed $BIN; the packaged wf-panel runs from the next session."
        exit 0 ;;
    --out)
        [ $# -ge 2 ] || die "--out needs a directory"
        OUT=$2
        mkdir -p "$OUT" || die "cannot create $OUT"
        OUT=$(cd "$OUT" && pwd) ;;
    "") ;;
    *) die "unknown option '$1' (try --remove or --out DIR)" ;;
esac

command -v apt-get >/dev/null 2>&1 || die "this builds from Debian/Devuan source; no apt-get here"
pkg_version=$(dpkg-query -W -f '${Version}' wf-shell 2>/dev/null) || pkg_version=
[ -n "$pkg_version" ] || die "wf-shell is not installed -- run setup-wayfire.sh first"

# apt-get source and build-dep need source entries, which these roots do not
# carry: one for the distro's own archive, for the length of the build.
cleanup() { rm -f "$SRCLIST"; }
trap cleanup EXIT INT TERM HUP
sed -n 's/^deb \([^#]*\)$/deb-src \1/p' /etc/apt/sources.list 2>/dev/null | head -n 1 > "$SRCLIST"
[ -s "$SRCLIST" ] || die "found no 'deb' line in /etc/apt/sources.list to take the source from"
log "apt-get update (with the source archive)"
apt-get update >/dev/null || die "apt-get update failed -- check network/DNS"

# wf-shell's build dependencies, less one: wayfire-dev, whose libseat-dev
# needs libsystemd-dev, which Devuan's elogind conflicts with -- `apt-get
# build-dep wf-shell` would remove elogind. The panel does not use it: meson
# only asks for its pkg-config file, and a stand-in (below) answers that.
log "installing what wf-panel builds with"
DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    dpkg-dev patch g++ meson pkgconf wayland-protocols libwayland-dev libgtkmm-3.0-dev \
    libgtk-layer-shell-dev libdbusmenu-gtk3-dev libpulse-dev libasound2-dev libwf-config-dev \
    || die "could not install the build dependencies -- see above"

log "fetching wf-shell $pkg_version's source"
mkdir -p "$WORK" && cd "$WORK" || die "cannot use $WORK"
rm -rf wf-shell-* wf-shell_*
apt-get source "wf-shell=$pkg_version" >/dev/null 2>&1 || apt-get source wf-shell \
    || die "apt-get source wf-shell failed"
src=$(ls -d wf-shell-*/ 2>/dev/null | head -n 1)
[ -n "$src" ] && cd "$src" || die "no source tree after apt-get source"

log "patching the menu to reopen instead of rebuilding"
menu=src/panel/widgets/menu.cpp
grep -q '^    set_category("All");$' "$menu" \
    || die "$menu has changed shape (no 'set_category(\"All\")' line to patch) -- this wf-shell needs a new patch"
sed -i 's/^    set_category("All");$/    if (category != "All")\n        set_category("All");/' "$menu"
grep -q 'if (category != "All")' "$menu" || die "the patch did not apply"

log "patching the clock to redraw only when its text changes"
clock=src/panel/widgets/clock.cpp
grep -q '^    label.set_text(text.substr(i));$' "$clock" \
    || die "$clock has changed shape -- this wf-shell needs a new clock patch"
patch -p1 --forward >/dev/null <<'PATCH' || die "the clock patch did not apply"
--- a/src/panel/widgets/clock.cpp
+++ b/src/panel/widgets/clock.cpp
@@ -1,7 +1,41 @@
 #include <glibmm.h>
 #include <iostream>
+#include <cstring>
 #include "clock.hpp"
 
+/* AOK-Workstation: whether a GDateTime format shows seconds -- %S, %s, %T, %r, %c,
+ * %X or %f, after any of the flags g_date_time_format takes. */
+static bool format_shows_seconds(const std::string& format)
+{
+    for (size_t i = 0; i < format.size(); i++)
+    {
+        if (format[i] != '%')
+        {
+            continue;
+        }
+
+        size_t j = i + 1;
+        while (j < format.size() && std::strchr("-_0^#EO:", format[j]))
+        {
+            j++;
+        }
+
+        if (j >= format.size())
+        {
+            break;
+        }
+
+        if (std::strchr("SsTrcXf", format[j]))
+        {
+            return true;
+        }
+
+        i = j;
+    }
+
+    return false;
+}
+
 void WayfireClock::init(Gtk::HBox *container)
 {
     button = std::make_unique<WayfireMenuButton>("panel");
@@ -20,15 +54,50 @@
 
     container->pack_start(*button, false, false);
 
-    timeout = Glib::signal_timeout().connect_seconds(
-        sigc::mem_fun(this, &WayfireClock::update_label), 1);
+    schedule_update(false);
 
     // initially set font
     set_font();
 
     font.set_callback([=] () { set_font(); });
+    format.set_callback([=] ()
+    {
+        update_label();
+        schedule_update(true);
+    });
 }
 
+/* AOK-Workstation: wake when the label can change, not every second. A clock without
+ * seconds is woken at the start of the next minute; one with them, every
+ * second as before. Each wake schedules the next, so a wake that comes a
+ * little early only means another one shortly after. */
+void WayfireClock::schedule_update(bool replace)
+{
+    if (replace)
+    {
+        timeout.disconnect();
+    }
+
+    auto slot = sigc::mem_fun(this, &WayfireClock::on_timeout);
+    if (format_shows_seconds((std::string)format))
+    {
+        timeout = Glib::signal_timeout().connect_seconds(slot, 1);
+        return;
+    }
+
+    auto now = Glib::DateTime::create_now_local();
+    double into_minute = now.get_seconds(); /* 0 to 59.999... */
+    unsigned ms = (unsigned)((60.0 - into_minute) * 1000.0) + 20;
+    timeout = Glib::signal_timeout().connect(slot, ms);
+}
+
+bool WayfireClock::on_timeout()
+{
+    update_label();
+    schedule_update(false);
+    return false; /* schedule_update set up the next one */
+}
+
 void WayfireClock::on_calendar_shown()
 {
     auto now = Glib::DateTime::create_now_local();
@@ -55,7 +124,15 @@
         i++;
     }
 
-    label.set_text(text.substr(i));
+    /* AOK-Workstation: GtkLabel lays out and redraws on every set_text, even of the
+     * same text, and every redraw is a frame the compositor has to put on
+     * screen. */
+    auto shown = text.substr(i);
+    if (label.get_text() != shown)
+    {
+        label.set_text(shown);
+    }
+
     return 1;
 }
 
--- a/src/panel/widgets/clock.hpp
+++ b/src/panel/widgets/clock.hpp
@@ -18,6 +18,8 @@
 
     void set_font();
     void on_calendar_shown();
+    void schedule_update(bool replace);
+    bool on_timeout();
 
   public:
     void init(Gtk::HBox *container) override;
PATCH
grep -q 'format_shows_seconds' "$clock" || die "the clock patch did not apply"

# --prefix=/usr, though the binary goes to /usr/local/bin: the panel looks
# for its option metadata under the prefix it was built with
# (METADATA_DIR), and the packaged metadata is in /usr/share/wayfire.
log "building wf-panel (this takes a while)"
rm -rf build
pcdir=$(mktemp -d)
printf 'Name: wayfire\nDescription: stand-in for building wf-panel\nVersion: %s\n' \
    "$(dpkg-query -W -f '${Version}' wayfire 2>/dev/null | sed 's/-.*//')" > "$pcdir/wayfire.pc"
PKG_CONFIG_PATH="$pcdir${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}" \
    meson setup build --prefix=/usr --buildtype=release >/dev/null || die "meson setup failed -- see above"
rm -rf "$pcdir"
# Two jobs, not one per CPU: each gtkmm translation unit wants hundreds of MB.
ninja -C build -j 2 src/panel/wf-panel || die "the build failed -- see above"
strip build/src/panel/wf-panel 2>/dev/null

if [ -n "$OUT" ]; then
    install -m 0755 build/src/panel/wf-panel "$OUT/wf-panel" || die "could not copy to $OUT"
    printf '%s\n' "$pkg_version" > "$OUT/wf-panel.source"
    cd / && rm -rf "$WORK"
    log "built: $OUT/wf-panel (wf-shell $pkg_version)"
    exit 0
fi
mkdir -p "$(dirname "$LIBBIN")"
install -m 0755 build/src/panel/wf-panel "$LIBBIN" || die "could not install $LIBBIN"
printf '%s\n' "$pkg_version" > "$STAMP"
ln -sf "$LIBBIN" "$BIN"
cd / && rm -rf "$WORK"

log "done"
note "Installed $BIN (wf-shell $pkg_version, menu and clock patched). It runs from the"
note "next Wayfire session. Back to the packaged one: sudo sh $0 --remove"
