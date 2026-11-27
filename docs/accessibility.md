# AOK-Workstation accessibility

What AOK-Workstation does for accessibility on iPhone and iPad, how to set it up,
and what it does not do yet. This describes version 1.3, build 567 and later.
If something here is wrong for you, or something gets in your way, please
[open an issue](https://github.com/emkey1/AOK-Workstation/issues) or use **Settings →
Send Feedback** in the app.

## At a glance

| Feature | iPhone and iPad | |
|---|---|---|
| Dark Interface | Supported | The terminal is dark unless you choose otherwise. |
| Larger Text | Supported | The terminal's font goes from 1 to 72 points. Settings follows the system text size. |
| Differentiate Without Color Alone | Supported | A held key is outlined, not only shaded. |
| Reduced Motion | Supported | No parallax, spinning or moving backgrounds. |
| VoiceOver | Not yet claimed | The app's screens are labelled. Reading the terminal with VoiceOver has not been checked on current iOS ([below](#voiceover-and-voice-control)). |
| Voice Control | Not yet claimed | As for VoiceOver. |
| Sufficient Contrast | Not claimed for the default theme | Several built-in themes have high contrast ([below](#contrast-and-colour)). |
| Captions, Audio Descriptions | Not applicable | The app has no video or audio of its own. |

## Text size

**The terminal.** Settings → Appearance → **Font Size** sets the terminal's
text from 1 to 72 points (12 to start with), and **Line Height** spaces the
lines out. With a hardware keyboard, **⌘+** and **⌘−** make the text larger and
smaller and **⌘0** puts it back. Programs in the terminal then get more or fewer
rows and columns, as they would in a resized window.

**Settings.** Settings, Filesystems and Diagnostics follow the system text
size: iOS Settings → Accessibility → Display & Text Size → **Larger Text**,
including the larger accessibility sizes. Rows grow to fit, and long labels
wrap rather than being cut off. The size changes as soon as you change it, with
no need to reopen the screen.

**Workspace windows.** Inside the Workspace, each window has its own text size:
⌘+, ⌘− and ⌘0 with a hardware keyboard, from half size to three times.

## Dark and light

The terminal starts dark. Settings → Appearance → **Color Scheme** chooses
**Match System**, **Light** or **Dark**. Settings follows the system's
appearance, and the Workspace has dark themes of its own in its **Themes**
applet.

## Contrast and colour

The default theme, Solarized, gives ordinary terminal text 4.7:1 contrast
against its dark background (4.1:1 in its light form). The colours programs use
for output, such as blue for directories in `ls`, go as low as 3.2:1. For
stronger contrast, choose another theme in Settings → Appearance → **Theme**:

| Theme | Text | Program colours, lowest |
|---|---|---|
| iSH-Default | 21:1 (white on black, or black on white) | standard terminal colours |
| Catppuccin, dark | 11.3:1 | 7.1:1 |
| Tokyo Night, dark | 10.6:1 | 6.5:1 |
| Tokyo Night Storm | 9.0:1 | 5.5:1 |
| Dracula | 13.4:1 | 4.5:1 |

Themes with a light and a dark form switch with the Color Scheme setting. Any
theme can be duplicated there and its colours changed.

## Without relying on colour

The row of keys above the on-screen keyboard has keys that stay held after a
tap: **Control** in the terminal, and **ctrl**, **alt** and **❖** in the
desktop window. A held key is outlined as well as shaded, and the outline goes
when it is released. Choices in Settings are marked with a checkmark, not only a
colour.

## Motion

There is no parallax, no spinning, and nothing that moves on its own in the
app's screens. Animations are short fades and slides. The terminal's cursor
does not blink unless you turn on Settings → Appearance → **Blink Cursor**. A
program running in the terminal can still ask for blinking text; that is the
program's choice.

## Typing and keyboards

- **One finger at a time.** A modifier never needs two fingers: tap
  **Control** and then **C** for Ctrl-C. In the desktop window, **alt** and
  **❖** work the same way.
- **Your own key row.** Settings → **Keyboard Toolbar** reorders the keys above
  the on-screen keyboard, removes the ones you do not use, and adds keys that
  type whatever you give them
  ([how](https://github.com/emkey1/AOK-Workstation/blob/working/opt/AOK/docs/keyboard-toolbar.md)).
- **Hardware keyboards.** The terminal, the Workspace and its editor have
  keyboard shortcuts for most of what they do
  ([the full list](https://github.com/emkey1/AOK-Workstation/blob/working/opt/AOK/docs/keyboard-shortcuts.md)).
  Settings → **External Keyboard** can make Caps Lock act as Control or Escape,
  send Option as Meta, make the backtick key an Escape key, and hide the on-screen
  key row while a keyboard is attached.

## VoiceOver and Voice Control

The app's own screens (Settings, the Workspace and its applets, the file
manager, the AI chat and the rest) give every button and control a name, and
most also give a hint saying what it does. Selected items, and held keys, are
announced as selected.

The terminal passes its output to VoiceOver as it arrives, and it takes typing
through the standard iOS text input, the one VoiceOver, Voice Control and
dictation type into. That has not yet been tried from start to finish with
VoiceOver on iOS 17 or later, so the app does not claim VoiceOver or Voice
Control support yet. If you use either, we would like to hear how it goes.

The graphical desktop window shows its programs as a picture, so VoiceOver
cannot read inside it.

## Video and audio

The app makes no videos or sound of its own. Videos you open are played by the
system's video player, which offers the subtitle and audio tracks a file
contains.
