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

- **Layout Manager / Desktops** — manage and switch between virtual
  desktops; window arrangements are saved and restored per layout.
- **Launcher / Quick Actions** — shortcuts to common actions.
- **Sessions** — list and switch between running terminal sessions.
- **Browser** — an in-app web browser applet.
- **Music** — see below.
- **MotePad** — see below.
- **LLM Chat** — an OpenAI-compatible/on-device chat assistant; only shown
  if enabled in Settings. See [llm-chat.md](llm-chat.md).
- **Themes, Clock, Info, Monitor, Networks, Status, Storage, Filesystems,
  Settings, Diagnostics** — status/utility applets and the Filesystems
  screen described in [roots.md](roots.md).

Window arrangements can be bookmarked and saved, so a favorite layout of
terminals and applets can be recalled later.

## MotePad

A text editor applet with its own file browser (open/save), reading and
writing files through the guest filesystem. It has a fast path for
anything under [`/AOK/persist`](persist.md) (direct host file access, no
emulated-VFS overhead) and falls back to the ordinary emulated path for
files inside a guest root. All guest filesystem I/O is serialized on its
own queue so it won't contend with other guest activity.

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
