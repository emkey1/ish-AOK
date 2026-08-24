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

## What's in the dock

The dock itself has two tiles: **Terminal** and **Utils**. Long-press either for
its menu — Terminal lists your sessions, Utils lists every applet, in five
groups:

- **Workspace** — Layout Manager, Desktops, Launcher, Quick Actions, Browser,
  Music, MotePad, File Manager, Sessions, Themes, and LLM Chat when it is
  enabled in Settings (see [llm-chat.md](llm-chat.md)).
- **Media** — Markdown, Image Viewer, Video Player, and Wayland (see below).
- **Status** — Clock, Monitor, Networks, Logs.
- **Storage** — Storage, and Boot Images, which is the Filesystems screen
  described in [roots.md](roots.md).
- **Support** — Settings, Diagnostics.

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
sudo sh /AOK/tools/setup-wayland.sh   # once: labwc, sway, wofi, foot, wayvnc
sh /AOK/tools/start-wayland.sh        # the applet runs this for you
```

`labwc` is the default compositor and `foot` the first app; `sway` is installed
as a `WAYLAND_COMPOSITOR_CMD=sway` alternative. `start-wayland.sh` also honours
`WAYVNC_PORT` and `ISH_DISPLAY_READY_FILE`.

Two caveats worth knowing before you start. Only **amd64/x86_64** guests have
been bring-up-tested — the packages exist for the other architectures in Devuan
and may well work, but nobody has run them. And Devuan (apt) and Arch (pacman)
install the same stack under the same package names, while Alpine (apk) is a
documented follow-up rather than a supported path.

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
`0644`. Reading it works
for anyone, writing to it needs root. The app logs you in as root, so this is
usually invisible; it is worth knowing if you run a `ws-*` launcher as the
UID 1000 user and get `Permission denied` from a command that works fine in your
own terminal.

One thing the return value does *not* tell you. A successful write means
**accepted**, not on screen. UIKit cannot be touched from a guest task's thread,
so the presenting is handed to the main queue and the write returns — and it has
to, because blocking a guest write on the UI queue is how you deadlock a
terminal that is itself being drawn by that UI. If Workspace goes away between
your write and the main thread getting to it, the request is dropped.
