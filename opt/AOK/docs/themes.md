# Themes: the terminal's colours, and how to make your own

Settings → **Appearance** → **Theme**. Fourteen themes ship with the app, and
you can build your own from there.

The built-ins are:

| | |
|---|---|
| `iSH-Default` | the colours AOK starts with |
| `Amber`, `1337` | inherited from upstream iSH |
| `Catppuccin`, `Dracula`, `Everforest`, `Gruvbox`, `Kanagawa`, `Nord`, `One Dark`, `Rosé Pine`, `Solarized`, `Tokyo Night`, `Tokyo Night Storm` | well-known terminal palettes, taken from each project's own published values |

Those last ones are transcriptions, not approximations. People recognise these
palettes by sight, so an "almost Nord" would be worse than no Nord at all.

There is a fifteenth, `Hot Dog Stand`, which the list deliberately does not
show. It appears only if it is already your current theme. It is red on yellow.
You were not meant to find it here.

## What a theme actually is

A theme is a **name**, **one or two palettes**, and an optional **UI override**.

A palette is three colours — foreground, background, cursor — and an optional
override for the sixteen ANSI colours. The palette is what the terminal draws
with: it is handed to hterm, so it decides every colour you see, including the
ones programs pick themselves with SGR escapes.

Colours are hex, and the `#` is required: `#RGB`, `#RGBA`, `#RRGGBB` or
`#RRGGBBAA`. Nothing else is accepted — not `red`, not `rgb(255,0,0)`.

An override must define **all sixteen** ANSI colours or none. A partial list is
rejected rather than half-applied, because sixteen colours that half agree with
each other look like a rendering bug rather than a choice.

A theme's **two palettes** are how one theme covers both appearances: the light
palette and the dark palette. That is why `Solarized` can be one theme rather
than two — Solarized Light and Solarized Dark are the same theme wearing its two
palettes. Which one you get is decided by **Color Scheme**, below.

**UI Overrides** are separate from the palette. They decide whether the app's
own chrome — keyboard, status bar — goes against the grain of the current
scheme: *Use Dark UI for Light Color Scheme*, and *Use Light UI for Dark Color
Scheme*.

## Making one

There is no "new theme" button. You start from one that already exists:

- **Duplicate** — swipe left on any theme, including a built-in, and choose
  Duplicate. You get `<name>-1`. This is the normal way in, and starting from a
  palette that already works beats starting from black.
- **Import** — tap **Edit**; an *Import Theme* row appears at the bottom. It
  opens a file picker for a theme `.json`. This is for a theme somebody sent
  you, not for making one.

Then tap **Edit** and tap your copy to open it. Built-ins open read-only; only
your own copies are editable.

Inside the editor:

- **Name** it, and fill in the colours as hex.
- **Single Palette** — on means light and dark share one palette. Turn it
  **off** to get separate *Light Palette* and *Dark Palette* sections and have
  the terminal follow the system appearance.
- **UI Overrides** — the two chrome switches described above.

There is no Save button. Edits are written when you leave the screen, and only
if the whole theme is valid. A theme with an empty name or a half-typed hex
colour is discarded rather than stored broken — so leaving a half-finished
edit throws that screen's work away.

Your own themes persist across app updates. The built-ins are always present and
are not editable in place.

## The rest of the Appearance screen

| setting | what it does |
|---|---|
| **Font** and **Font Size** | the terminal typeface and its size |
| **Line Height** | vertical spacing, 0.70 to 1.30 in steps of 0.05 — see [proc-ish.md](proc-ish.md) |
| **Color Scheme** | which of the theme's palettes the terminal uses: Light, Dark, or follow the system |
| **Cursor Style** and **Blink Cursor** | shape, and whether it blinks |
| **Status Bar**, **Terminal Buttons** | whether those are shown |
| **Workspace Style**, **Desktops at Launch** | [Workspace mode](workspace.md) appearance |

Note that **Color Scheme** picks a palette, not an app appearance. Setting it to
Dark on a device running in light mode gives you a dark terminal with light
chrome around it — unless the theme's UI Overrides say otherwise.

## Not to be confused with

[Workspace](workspace.md) has its own, unrelated themes — Utils → Themes, or
`ws-themes` from the guest. Those colour the Workspace desktop and its cards.
They share nothing with terminal themes but the word.

## Reading your settings from the guest

Your current theme is readable from inside the guest:

```sh
cat /proc/ish/defaults/theme          # e.g. "Nord"
cat /proc/ish/defaults/font_size      # e.g. 15
cat /proc/ish/defaults/line_height    # e.g. 1
```

As root, those are also **writable** — see [proc-ish.md](proc-ish.md).
