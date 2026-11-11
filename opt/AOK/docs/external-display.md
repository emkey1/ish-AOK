# External display

Plug an iPhone into a monitor — a USB-C or Lightning video adapter, or AirPlay
Screen Mirroring to a TV or a Mac — and iSH-AOK puts the terminal on the big
screen at the big screen's shape, while the phone keeps the keyboard. Without
this, every iOS app gets the same thing: a letterboxed copy of the phone's
screen.

**iPhone only. New in 563; on iOS 27 it works from 564.** On iOS 27, 563
gave only the mirror: iOS 27 changed how an app asks for the second screen,
and 563 asked the old way. 564 asks both ways, and a Workspace terminal
window now reaches the display too (in 563 it never did). Testers have since
run the terminal on real displays with 564 — over USB-C, Thunderbolt and
AirPlay, on iOS 27. The Wayland mirror in 564 showed only its first frame and
then stopped; 565 fixes that, tested in the simulator on iOS 27.0 but not yet
on a real display. If you try it, please say what you saw on [#540](https://github.com/emkey1/ish-AOK/issues/540) —
working or not.

## What goes where

Connect the display and these move to it on their own; there is nothing to
turn on.

- **A terminal.** Its text is drawn on the external screen, sized to fill it.
  The phone shows a placeholder and keeps the keyboard, the keyboard bar and
  the touch input, so you type on the phone and read on the display.
- **The Workspace.** The frontmost terminal window is the one shown, at the
  tab it is showing. Bring another terminal window to the front, or pick
  another tab, and the display follows.
- **The Wayland Display.** It is **mirrored**, not moved: the desktop draws on
  both screens, with the cursor, and the phone stays the touch surface — tap,
  drag and touchpad mode work as before. While the mirror exists the desktop is
  sized to the external screen, so a landscape monitor gets a landscape
  desktop rather than the phone's portrait one.
- **Nothing to show.** A Workspace desktop with no terminal window open says
  *Open a terminal or the Wayland Display to use this screen.* on the display,
  rather than leaving it blank. Open one and it takes over.

Disconnect the display and everything comes back to the phone.

## The Wayland desktop on a monitor

The desktop is a set of guest programs, so it has to be installed once, in the
filesystem you use:

1. **Install a desktop.** **Settings → Wayland Desktop → Install a Desktop…**,
   then tap one — labwc is the light one. It takes several minutes; leave
   iSH-AOK open until it finishes. [workspace.md](workspace.md) describes the
   choices.
2. **Open the Wayland Display.** Any of: long-press the app's icon on the Home
   Screen and pick **Wayland Display**; set **Settings → Startup Mode →
   Wayland Display** to open on it every time; or, in the Workspace, open the
   **Wayland** applet from the dock or the Launcher.
3. **Connect the display**, before or after. The desktop appears on it once
   the phone's view says *Connected*.
4. **Turn on Touchpad Mode** in the view's **···** menu. Out of the box a
   finger puts the pointer where it touches the phone, which only works while
   you are looking at the phone; as a touchpad, you drag on the phone and
   watch the pointer on the monitor. The gestures are listed in
   [workspace.md](workspace.md).

A hardware keyboard paired with the phone types into the desktop. While the
monitor is attached, the phone's own view shows the same desktop squeezed to
the phone's shape — it is still the touch surface, so that is expected.

## iPad

iPads are left as they were in 562. An iPad with **Stage Manager** and an
external display already gets real, resizable iSH-AOK windows on the monitor
through iPadOS itself; an iPad without it gets the letterboxed mirror. The
563 behaviour is declared for iPhone only, because the first attempt at this
feature (546) fought Stage Manager on an iPad and was taken out.

## Things worth knowing

- **AirPlay Screen Mirroring counts; macOS iPhone Mirroring does not.** Screen
  Mirroring from the phone's Control Center to an Apple TV, a smart TV or a Mac
  with AirPlay Receiver on is an external display to the app. The iPhone
  Mirroring app on a Mac only remote-controls the phone, and gives apps no
  second screen.
- **The display shows; it does not take input.** iOS gives an app this kind of
  screen as non-interactive, so a mouse or touch on the monitor does nothing —
  use the phone, or a hardware keyboard paired with it.
- **Text size is the terminal's.** ⌘+ (or ⌘=), ⌘- and ⌘0 on a hardware
  keyboard make the text on the display larger, smaller or the default, as
  does **Settings → Appearance → Font Size**. On a high-resolution monitor the text can
  start out small. It is the same setting the phone uses, so it stays when you
  unplug — except in a Workspace terminal window, which keeps its own size.
- **One thing at a time.** The display shows one terminal, or the Wayland
  Display, never both.
- **On iOS 27, only a terminal, the Workspace or the Wayland Display takes the
  screen.** While the app shows something else full screen — choosing a
  filesystem, or diagnostics — the display goes back to the mirror. Sheets
  such as Settings opened over a terminal leave it on the display.

## See also

- [workspace.md](workspace.md) — the Workspace and the Wayland desktop.
- [themes.md](themes.md) — the display uses the terminal's theme.
