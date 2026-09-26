# The keyboard toolbar: arranging its keys

The row of keys above the on-screen keyboard is yours to arrange: **Settings →
Keyboard Toolbar**. Reorder the keys, remove the ones you never press, bring
them back, and add keys of your own that type whatever you give them. The bar
changes as you go; nothing needs restarting.

Until you change something, the bar is the one iSH-AOK has always had:
**Tab, Control, Escape and the arrow keys** at the left, and **- . / : ! |** in
the centre, of which only **.** and **/** show on an iPhone held upright.

## Two places on the bar

The screen has a section for each place a key can sit:

- **Left** keys run from the bar's left edge.
- **Center** keys sit between two equal spaces, so they stay centred however
  many there are.

Drag a key by its handle to move it, within a section or from one to the other.
The red button removes it. The app's own buttons at the right-hand end —
Settings, Files, Paste, Hide Keyboard and the rest — are not part of the list
and stay where they are, so Settings can never be arranged off its own bar.

## Narrow bars

An iPhone held upright has room for fewer keys. Tap a key and choose **Hide on
a Narrow Bar** to leave it off there; it still shows in landscape and on an
iPad. This is how - : ! | have always behaved. Choose **Show on a Narrow Bar**
to keep a key everywhere.

When the keys that are showing do not fit at full width, every key and button
narrows together rather than some disappearing.

## Custom keys

**Add Key… → Custom Key…** asks for two things: what the key **shows** (a
character or a short label, say `~` or `ll`) and what it **types**. The text
is typed exactly as written, except for these escapes:

| write | to type |
| --- | --- |
| `\e` | Escape |
| `\t` | Tab |
| `\n` | Return — so `ls -la\n` runs the command |
| `\r` | a carriage return |
| `\xHH` | the character with that hex code, up to `7F`: `\x03` is Control-C |
| `\\` | a backslash |

A backslash followed by anything else stands for itself. With **Control** on, a
custom key of one character sends that character's control code, as the
built-in keys do.

Tap a custom key's row to edit it or change where it shows. **Add Key…** also
lists any built-in key you have removed, to put it back where it came from.

**Reset to Default** puts the bar back as it came and removes your custom keys.

## Where it is kept

The arrangement is an app preference, `Toolbar Keys`, kept with the rest of
iSH-AOK's settings; it is not in the guest and is not part of a root. It covers
the terminal's bar. The Wayland display's key strip is separate.

See also [workspace.md](workspace.md) for the bar in Workspace windows, and
[snippets.md](snippets.md) for saved commands, which suit longer text than a
key does.
