# Workspace: a native multi-window desktop for iSH-AOK

Workspace is a native, in-app multi-window "desktop" environment layered
on top of the terminal — a windowing system with a dock, multiple virtual
Desktops, and draggable/resizable applet windows. Open it from the
floating Workspace button in a terminal session, or from the "Switch
Terminal" menu.

Workspace windows are built entirely in native UIKit — it is **not** a
Wayland or X11 guest display. Only actual terminal sessions inside it run
guest Linux processes; the window chrome, dock, and most applets are pure
native code. That's also why Workspace works on iPhone, not just iPad: it
doesn't depend on iOS Scenes or Stage Manager multi-window support.

## Reaching the applets

Which controls you get depends on the **Workspace Style** preference. The
default, **Modern**, has no dock: tap the ☰ button at the bottom-right, or
long-press the bare desktop (two fingers works anywhere, even over a window),
and pick **Utilities…**. **Classic** shows a dock instead, with two tiles —
**Terminal** and **Utils** — that you long-press for the same menus. Either way,
Terminal lists your sessions and Utils lists every applet, in five groups:

- **Workspace** — Layout Manager, Desktops, Launcher, Quick Actions, Browser,
  Music, MotePad, File Manager, Sessions, Themes, and LLM Chat when it is
  enabled in Settings (see [llm-chat.md](llm-chat.md)).
- **Media** — Markdown, Image Viewer, Video Player, and Wayland (see below).
- **Status** — Clock, Monitor, Networks, Logs.
- **Storage** — Storage, and Boot Images, which is the Filesystems screen
  described in [roots.md](roots.md).
- **Support** — Settings, Diagnostics.

The menu **Utilities…** sits in — the ☰ button, the desktop long press, and
each window's own ☰ all raise it — also has **Snippets…**, your library of
saved command lines. In a plain terminal those hang off a long press on the
Paste key, or Cmd-J, both of which want the keyboard up; here they are one tap
from anywhere on the desktop. A snippet goes to the terminal window whose ☰ you
used, or to the frontmost one when you opened the menu from the desktop or the
corner button — and that window comes to the front before the list appears, so
you can see where the text is about to land. With no terminal window open the
list still opens, to read and edit; it says so at the top, and a tap opens the
snippet rather than inserting it.

A terminal window has its own way in as well: the **Switch Terminal** button,
the one showing overlapping rectangles, lists **Snippets…** too. That is the
route to use with a hardware keyboard, which hides the accessory bar the Paste
key lives in — the same button then floats over the bottom-right of the
terminal. Cmd-J opens the list from the keyboard.

Window arrangements can be bookmarked and saved, so a favorite layout of
terminals and applets can be recalled later.

## MotePad

A text editor applet with its own file browser (open/save), reading and
writing files through the guest filesystem. It has a fast path for
anything under [`/AOK/persist`](persist.md) (direct host file access, no
emulated-VFS overhead) and falls back to the ordinary emulated path for
files inside a guest root. All guest filesystem I/O is serialized on its
own queue so it won't contend with other guest activity.

It also has a terminal half: `motepad` is a native program with the same
modeless design, and inside Workspace `motepad somefile` hands the file to
*this* applet rather than editing in the terminal. See
[motepad.md](motepad.md).

## Music

An audio player applet with two kinds of sources:

- `/AOK/persist/music` — the default library location; because
  `/AOK/persist` is a real host directory, the player can read files there
  directly via AVFoundation with no copying.
- Any other guest path (for example `/root/music` inside an installed
  root) — read through the emulated filesystem and extracted to a
  temporary file, since that data lives inside a root's own SQLite-backed
  store rather than on the host directly.

Playlists are saved as JSON under `/AOK/persist/playlists`.

## The Wayland applet

The **Wayland** applet is the one window here whose contents are drawn by guest
programs rather than by UIKit. A wlroots compositor, a terminal and a VNC server
run as ordinary processes inside your root, and the applet is a native RFB
client connected to them over localhost. It can also be the window the app opens
on, rather than the terminal.

It needs those programs installed in the guest first, and two scripts do that:

```sh
sudo sh /AOK/tools/setup-wayland.sh   # once: labwc, sway, wofi, foot, wayvnc, waybar
sh /AOK/tools/start-wayland.sh        # the applet runs this for you
```

`labwc` is the default compositor and `foot` the first app; `sway` is installed
as a `WAYLAND_COMPOSITOR_CMD=sway` alternative. `start-wayland.sh` also honours
`WAYVNC_PORT` and `ISH_DISPLAY_READY_FILE`.

The desktop gets its own session D-Bus when `dbus-daemon` is installed, which
`setup-wayland.sh` also does. Everything started inside it finds that bus
through `DBUS_SESSION_BUS_ADDRESS`, so programs that need one, such as waybar
and Qt apps, work without you starting a bus or setting `DISPLAY`. On a root set
up before this, install it once with `sudo apt install dbus-daemon` (Devuan) or
`sudo apk add dbus` (Alpine), then reopen the applet.

`waybar`, a panel along the top of the desktop, is installed too, with the Font
Awesome font its icons use: a taskbar, the clock, CPU, memory, disk, network and
battery. It starts with the desktop. To turn it off, right-click the desktop and
pick **Applications › Hide Panel**; it stays off in later sessions until
**Show Panel** turns it back on. If your own `~/.config/labwc/autostart` starts
waybar, the desktop leaves that to it.

The first session after it is installed writes a config that suits labwc to
`~/.config/waybar/config.jsonc`. It is written only when you have no waybar config
of your own, and it is yours to edit. Debian's default in `/etc/xdg/waybar` is
written for sway, and several of its modules switch themselves off here. A
`style.css` is written beside it the same way: waybar's own style, with the text
font ahead of Font Awesome, since the Font Awesome 7 that Alpine and Arch ship
would otherwise draw every letter as an icon. Two log lines are expected and
harmless: `basic_string::_M_create`, a waybar 0.12 bug that native Linux prints
too, and a warning that it cannot reach the system bus, which it only uses to
notice suspend. On a root set up before this, install it with
`sudo apt install waybar fonts-font-awesome` (Devuan) or
`sudo apk add waybar font-awesome` (Alpine).

Right-click the desktop for its menu. **New Terminal** opens foot, **Launcher**
searches the installed programs, and **Applications** has a submenu for each
category: Games, Graphics, Internet and so on. labwc menus do not scroll, so a
category with more than 24 programs is split into parts named by their first
letters, such as **Games (A–N)**. The same actions have keys: Alt+Return opens a
terminal, Alt+Shift+D the launcher, Alt+Tab switches windows, Alt+Shift+Q closes
one, Alt+Shift+R reloads labwc's settings and Alt+Shift+E ends the session.

The menu and the keys live in `~/.config/labwc/menu.xml` and `rc.xml`, which are
yours to edit: a file you have changed is never replaced. One still exactly as an
earlier AOK wrote it is brought up to date, which is how an existing desktop gets
the Launcher.

More programs that run well on this desktop come in three optional sets:

```sh
sudo sh /AOK/tools/setup-wayland-extras.sh games   # puzzles, solitaire, Mines, Chess, terminal games
sudo sh /AOK/tools/setup-wayland-extras.sh tools   # launcher, notifications, clipboard, screenshots, viewers, editor, files, browser
sudo sh /AOK/tools/setup-wayland-extras.sh x11     # Xwayland, xterm and the X fonts, for X11-only programs
```

Name several sets at once, or `all`. The Launcher is fuzzel once **tools** is
installed, and wofi before. Program names differ between distributions and a
few programs are missing from Alpine and Arch; the script installs what each has
and lists what it could not. X11 programs run from the menu and from the first
terminal alike, since the session sets `DISPLAY`; Xwayland starts with the first
one, which takes a few seconds. SDL games are told to use Wayland, and
`/usr/games`, where Devuan puts games, is on `PATH`.

Two caveats worth knowing before you start. It has been run on **amd64** and
**arm64** guests — the packages exist for the other architectures in Devuan and
may well work, but nobody has run them. And it has been run on **Devuan** (apt)
and **Alpine** (apk); **Arch** (pacman) installs the same stack under the same
names, and its packages resolve, but no one has run a session on it.

You may also come across `wayland_workspace_plan.md` in the project's design
docs. That is the forward design document this applet came out of; where it and
the shipped applet disagree, the applet is right.

## `/proc/ish/workspace`: asking the app to open something

Everything above is driven from the screen. `/proc/ish/workspace` is the other
direction — the file a guest process reads to find out whether it is running
under Workspace, and writes to ask the app to put something on screen. The
`ws-*` launchers in [`/AOK/persist/bin`](persist.md) and `motepad`'s handoff are
both just users of it.

Read it for the answer:

```sh
$ cat /proc/ish/workspace
hosted=1
tools=motepad,filemanager,markdown,imageviewer,videoplayer,audio,browser,llm,...
verbs=open
```

`hosted=0` is a complete answer rather than an error — it is what a plain
terminal session, and the whole command-line build, honestly are, and it comes
with a `reason=` line saying which. Read this *first* and fall back, rather than
writing a request nobody is there to answer:

```sh
case "$(head -1 /proc/ish/workspace 2>/dev/null)" in
    hosted=1) printf 'open markdown %s\n' "$PWD/README.md" > /proc/ish/workspace ;;
    *)        less README.md ;;
esac
```

That is the same test the shipped `ws-*` launchers make, down to the
`2>/dev/null` — an older build has no such file at all, and a missing one should
land in the fallback branch rather than on the terminal as an error.

Writing takes one verb, `open <tool> [path]`:

```sh
echo "open motepad /AOK/persist/notes.txt" > /proc/ish/workspace
echo "open filemanager /etc"               > /proc/ish/workspace
echo "open clock"                          > /proc/ish/workspace
```

The narrowness is the point, because this is a guest asking the app to act. The
tool must be on the list the app publishes in `tools=`, not any string a guest
can construct; an unknown one is refused with `EINVAL` from the `write` itself,
so a script gets an error it can branch on rather than silence from a queue it
cannot see. A write with no Workspace to receive it is `EOPNOTSUPP`, which is a
different answer from "I did not understand you". The path is **not** split on
whitespace, so a filename with spaces in it needs no quoting here — but it must
be **absolute**. A relative path has no meaning by the time the request reaches
the app: the guest's working directory is not the app's, and the process may be
gone before the window appears. `motepad` and the `ws-*` launchers resolve yours
for you.

The file is `0666` and owned by root — unlike `/proc/ish/roots`, which is
`0644`. Reading *and writing* work for any uid, and that asymmetry is
deliberate: managing roots is administrative, opening a window is not, so a
`ws-*` launcher run as the UID 1000 user works exactly as it does in a root
terminal.

One thing the return value does *not* tell you. A successful write means
**accepted**, not on screen. UIKit cannot be touched from a guest task's thread,
so the presenting is handed to the main queue and the write returns — and it has
to, because blocking a guest write on the UI queue is how you deadlock a
terminal that is itself being drawn by that UI. If Workspace goes away between
your write and the main thread getting to it, the request is dropped.

## `/proc/ish/applets`: what is open

The applets are not processes, and `ps`, `top`, `htop` and
[`ktop`](ktop.md) do not show them. File Manager, MotePad, LLM Chat, Music,
Settings, the Wayland display and the rest are iOS interface running inside
the app. They have no pid, no address space, and nothing a signal could reach.
A guest process shows up in those tools because it *is* one. Giving an applet a
made-up pid would put an entry in every process list that `kill -9` could not
stop, which no real system does, and every tool would pass the lie along.
What a guest runs *inside* a terminal window is a process, and it shows up as
usual.

To see which applets are open, read `/proc/ish/applets`:

```sh
$ cat /proc/ish/applets
ID TOOL DESKTOP STATE AGE TITLE
3 workspaces * shown 54 Desktops
5 clock 1 shown 3 Clock
6 motepad 1 shown 2 MotePad
7 audio 1 front 1 Music
```

Switch to Desktop 2 and the three on Desktop 1 read `hidden`. The Desktops
applet stays `shown`, because every Desktop shares it.

One line per applet, in the order they were opened:

- `ID` is a number no other window this run has had, so two reads can be
  matched up. It does not survive a relaunch.
- `TOOL` is the name `/proc/ish/workspace` uses for it.
- `DESKTOP` is numbered as the Desktops applet numbers them, from 1. `*` marks
  the Launcher and the Desktops applet, which every Desktop shares.
- `STATE` is `front` for the window on top of the Desktop you are looking at,
  `shown` for the rest of what is on screen, and `hidden` for one open on
  another Desktop.
- `AGE` is seconds since the window opened.
- `TITLE` is the window's title and runs to the end of the line, because
  titles have spaces in them. `awk '{print $2}'` gets the tools.

Terminal windows are not listed, because their programs are already in `ps`.
With nothing open, or with no Workspace at all (a plain terminal, or the
command-line build), the file is the header alone. It is read-only. Closing an
applet is done on screen, not with a signal.
