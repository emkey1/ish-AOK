# Keyboard shortcuts: every key combination the app and its tools answer to

Everything here needs a hardware keyboard. On an iPad, **hold Command** to see
the shortcuts that apply to whatever has focus: iPadOS lists them, and the list
changes as focus moves between a terminal, a file manager and MotePad.

One rule explains most of what follows. **Command belongs to the app; Control
and Option belong to Linux.** Command chords are app actions (zoom, find,
switch desktops) and never reach a program in the guest, so Command-C can never
turn into Control-C and interrupt something. Control and Option go to the
terminal, or to the Wayland desktop, as they would on a Linux machine.

In the tables, ⌘ is Command, ⌥ is Option, ⌃ is Control and ⇧ is Shift.

## Workspace: desktops and windows

These work anywhere in Workspace, whatever window has focus.

| Keys | Action |
|---|---|
| ⌘← / ⌘→ | previous / next desktop |
| ⌃⌥← / ⌃⌥→ | previous / next desktop |
| ⌃⌥1 … ⌃⌥9 | go to desktop 1 to 9 |
| ⌃Tab | next window on this desktop |
| ⌃⇧Tab | previous window on this desktop |
| ⌘+ (or ⌘=) / ⌘- / ⌘0 | larger / smaller / default text in the focused window |

Two exceptions:

- In a text view you are **editing** (MotePad, a prompt), ⌘← and ⌘→ move to the
  start and end of the line instead, as they do in every iPad text editor. ⌃⌥←
  and ⌃⌥→ still switch desktops there.
- Inside a **Wayland desktop** window, ⌃⌥ with an arrow or a digit goes to the
  Linux desktop and switches *its* desktops (see below). Outside it, the same
  keys switch Workspace's. One habit, two levels.

## The terminal

The same keys in Workspace's terminal windows and in the full-screen terminal.

| Keys | Action |
|---|---|
| ⌘+ (or ⌘=) / ⌘- / ⌘0 | larger / smaller / default font |
| ⌘F | find in scrollback |
| ⌘G / ⌘⇧G | find next / previous |
| Esc | close the find bar (while it is open) |
| ⌘⇧K | clear the scrollback |
| ⌘B | file browser ([file-browser.md](file-browser.md)) |
| ⌘J | snippets ([snippets.md](snippets.md)) |
| ⌘S | save the session to disk ([suspend.md](suspend.md)) |
| ⌘, | Settings |
| ⌘⌥⇧1 … ⌘⌥⇧6 | switch to tty1 to tty6 |
| ⌘⌥⇧7 | switch back to the session shell |

Workspace's terminal window also has tabs:

| Keys | Action |
|---|---|
| ⌘T | new tab |
| ⌘W | close the tab |
| ⌘⇧] / ⌘⇧[ | next / previous tab (also ⌘} / ⌘{) |
| ⌘1 … ⌘9 | go to tab 1 to 9 |

### What the terminal sends to Linux

Control with a letter sends that control character (⌃C is interrupt, ⌃D end of
input, ⌃Z suspend, and so on). The arrows, Home, End, Page Up, Page Down and F1
to F12 send the escape sequences an xterm sends, including their Shift, Control
and Option forms, so ⌃← and ⌃→ move by word in shells and editors that expect
them.

**Settings → External Keyboard** changes a few of these:

| Setting | Effect |
|---|---|
| Map Caps Lock: Control / Escape / None | Caps Lock acts as Control, or as Escape. Since iOS 13.4 the same can be done for every app in iOS Settings → General → Keyboard → Hardware Keyboard → Modifier Keys |
| Option → Meta | Option-letter sends Escape then the letter, which is what Emacs, readline (⌥B, ⌥F, ⌥D) and most terminal programs expect as Meta. Off, Option types the accented characters it types everywhere else |
| Backtick → Escape | the ` key sends Escape, for keyboards without one |
| Send ctrl-space to terminal | ⌃Space reaches the terminal (Emacs's set-mark) instead of switching keyboard layouts |
| Extra keys: Hide with external keyboard | hides the key row above the keyboard; ⌘, still opens Settings |

The key row above the on-screen keyboard is configurable too: see
[keyboard-toolbar.md](keyboard-toolbar.md).

## MotePad (the Workspace editor)

| Keys | Action |
|---|---|
| ⌘N | new |
| ⌘O | open |
| ⌘S | save |
| ⌘⇧S | save as |
| ⌘P | print |
| ⌘⇧P | page setup |
| ⌘W | close |
| ⌘Q | quit MotePad |
| ⌘F | find |
| ⌘G | find next |
| ⌘⇧F | replace |
| ⌘L | go to line |
| ⌘D | insert the date and time |
| ⌘⌥W | word wrap on / off |
| ⌘T | fonts |
| ⌘⌥S | status bar on / off |
| ⌘⌥L | line numbers on / off |
| ⌘? | MotePad help |
| ⌘Z / ⌘⇧Z, ⌘X / ⌘C / ⌘V, ⌘A | undo / redo, cut / copy / paste, select all |
| ⌃⌥F / ⌃⌥E / ⌃⌥O / ⌃⌥V / ⌃⌥H | open the File / Edit / Format / View / Help menu |

## motepad (the terminal editor)

The terminal half of MotePad uses Control keys, since Command never reaches a
program in the terminal ([motepad.md](motepad.md)):

| Keys | Action |
|---|---|
| ⌃S | save |
| ⌃Q | quit; with unsaved changes the first press asks, the second discards |
| ⌃F | find (forward from the cursor, wrapping) |
| ⌃G | go to line |
| ⌃K | delete the current line |
| ⌃A, Home / ⌃E, End | start / end of line |
| Esc | cancel the find or go-to-line prompt |

## File manager

| Keys | Action |
|---|---|
| ⌘↑ | enclosing folder |
| ⌘[ / ⌘] | back / forward |
| ⌘⇧. | show / hide hidden files |
| ⌘R | refresh |
| ⌘N | new file manager window here |

## Image viewer

| Keys | Action |
|---|---|
| ← / → | previous / next image in the folder |

## LLM Chat

| Keys | Action |
|---|---|
| Return | send the prompt |
| ⇧Return | new line in the prompt |

## The Wayland desktop

A Wayland desktop window passes every key to Linux except Command chords, so
labwc and the programs on it get Control, Option (as Alt) and Shift as they
would on a PC. The window's own key row has a ❖ key for Super, which a Mac
keyboard has no key for.

⌘+ (or ⌘=), ⌘- and ⌘0 are the one exception: they are passed on as ⌃+, ⌃- and
⌃0, the zoom keys of foot and most Linux programs. No other Command chord is
translated, so ⌘C stays copy and never becomes ⌃C.

These are labwc's keys as `start-wayland.sh` sets them up (in
`~/.config/labwc/rc.xml`, yours to change). Alt is Option:

| Keys | Action |
|---|---|
| ⌥Return | new terminal (foot) |
| ⌥Tab | next window |
| ⌥⇧Q | close the window |
| ⌥⇧D | application launcher |
| ⌃⌥← / ⌃⌥→ | previous / next desktop (four, wrapping) |
| ⌃⌥⇧← / ⌃⌥⇧→ | move the window to the previous / next desktop |
| ⌃⌥1 … ⌃⌥4 | go to desktop 1 to 4 |
| ⌥⇧R | reload labwc's configuration |
| ⌥⇧E | leave the desktop |

In foot, the terminal, ⌃⇧C and ⌃⇧V copy and paste, as foot does everywhere.

## Programs with their own keys

- **ktop** — `P` `M` `T` `N` sort, `k` sends a signal, `q` quits: the full
  table is in [ktop.md](ktop.md).
- **vi** (Nextvi), **hx** (Helix) and the distro's own editors and pagers keep
  their usual keys; nothing in the app changes them, apart from the terminal
  options above (Option → Meta, Caps Lock).
