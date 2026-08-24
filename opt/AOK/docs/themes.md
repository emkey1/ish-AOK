# Themes: the terminal's colours, and how to make your own

Settings → **Appearance** → **Theme**. Fifteen themes ship with the app, and you
can build your own from there.

The built-ins are:

| | |
|---|---|
| `iSH-Default` | the colours AOK starts with |
| `Amber`, `1337`, `Hot Dog Stand` | inherited from upstream iSH |
| `Catppuccin`, `Dracula`, `Everforest`, `Gruvbox`, `Kanagawa`, `Nord`, `One Dark`, `Rosé Pine`, `Solarized`, `Tokyo Night`, `Tokyo Night Storm` | well-known terminal palettes, taken from each project's own published values |

Those last ones are transcriptions, not approximations. People recognise these
palettes by sight, so an "almost Nord" would be worse than no Nord at all.

## What a theme actually is

A theme is a **name** plus a **palette**, where a palette is three colours —
foreground, background, cursor — and an optional override for the sixteen ANSI
colours. The palette is what the terminal draws with: it is handed to hterm, so
it decides every colour you see, including the ones programs pick themselves
with SGR escapes.

A theme can carry **one palette, or two**. With two, the first is used in light
appearance and the second in dark, and the terminal follows the system. That is
why `Solarized` can be one theme rather than two: Solarized Light and Solarized
Dark are the same theme wearing its two palettes.

An override must define **all sixteen** ANSI colours or none. A partial list is
rejected rather than half-applied, because sixteen colours that half agree with
each other look like a rendering bug rather than a choice.

## Making one

Settings → Appearance → Theme → **Edit**, then **+**.

```sh
# Nothing here is a file you can edit from the guest -- themes live in the
# app's preferences, not in the rootfs. See "Reading your settings" below.
```

- **Name** it, then fill in the colours as hex.
- **Duplicate** an existing theme first if you want a starting point close to
  something that already works. That is usually easier than starting from black.
- Turn on the **separate light and dark palettes** switch if you want the theme
  to follow the system appearance; leave it off for a theme that looks the same
  either way.
- A theme that is not yet valid — an empty name, a half-typed hex colour — is
  simply not saved. You can leave the editor at any point; an incomplete theme
  is discarded rather than stored broken, and nothing traps you on the screen.

Your own themes persist across app updates. The built-ins are always present and
are not editable in place — duplicate one and edit the copy.

## The rest of the Appearance screen

| setting | what it does |
|---|---|
| **Font** and **Font Size** | the terminal typeface and its size |
| **Line Height** | vertical spacing, 0.70 to 1.30 in steps of 0.05 — see [proc-ish.md](proc-ish.md) |
| **Color Scheme** | whether the app's own chrome follows light, dark, or the system |
| **Cursor Style** and **Blink Cursor** | shape, and whether it blinks |
| **Status Bar**, **Terminal Buttons** | whether those are shown |
| **Workspace Style**, **Desktops at Launch** | [Workspace mode](workspace.md) appearance |

## Reading your settings from the guest

Your current theme is readable from inside the guest:

```sh
cat /proc/ish/defaults/theme          # e.g. "Nord"
cat /proc/ish/defaults/font_size      # e.g. 15
cat /proc/ish/defaults/line_height    # e.g. 1
```

Those are **read-only** — a window onto the app's preferences, not a way to
change them. See [proc-ish.md](proc-ish.md).
