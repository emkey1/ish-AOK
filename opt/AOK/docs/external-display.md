# External display

Plug an iPhone into a monitor — a USB-C or Lightning video adapter, or AirPlay
Screen Mirroring to a TV or a Mac — and iSH-AOK puts the terminal on the big
screen at the big screen's shape, while the phone keeps the keyboard. Without
this, every iOS app gets the same thing: a letterboxed copy of the phone's
screen.

**iPhone only. New in 563; on iOS 27 it works from 564.** On iOS 27, 563
gave only the mirror: iOS 27 changed how an app asks for the second screen,
and 563 asked the old way. 564 asks both ways. It is tested in the simulator
on iOS 26.5 and 27.0, not yet on a real display. If you try it, please say
what you saw on [#540](https://github.com/emkey1/ish-AOK/issues/540) —
working or not.

## What goes where

Connect the display and these move to it on their own; there is nothing to
turn on.

- **A terminal.** Its text is drawn on the external screen, sized to fill it.
  The phone shows a placeholder and keeps the keyboard, the keyboard bar and
  the touch input, so you type on the phone and read on the display.
- **The Workspace.** The frontmost terminal window is the one shown. Bring
  another terminal window to the front and the display follows it.
- **The Wayland Display.** It is **mirrored**, not moved: the desktop draws on
  both screens, with the cursor, and the phone stays the touch surface — tap,
  drag and touchpad mode work as before. While the mirror exists the desktop is
  sized to the external screen, so a landscape monitor gets a landscape
  desktop rather than the phone's portrait one.
- **Nothing to show.** A Workspace desktop with no terminal window open says
  *Open a terminal or the Wayland Display to use this screen.* on the display,
  rather than leaving it blank. Open one and it takes over.

Disconnect the display and everything comes back to the phone.

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
- **One thing at a time.** The display shows one terminal, or the Wayland
  Display, never both.
- **On iOS 27, only a terminal, the Workspace or the Wayland Display takes the
  screen.** While the app shows something else full screen — choosing a
  filesystem, or diagnostics — the display goes back to the mirror. Sheets
  such as Settings opened over a terminal leave it on the display.

## See also

- [workspace.md](workspace.md) — the Workspace and the Wayland desktop.
- [themes.md](themes.md) — the display uses the terminal's theme.
