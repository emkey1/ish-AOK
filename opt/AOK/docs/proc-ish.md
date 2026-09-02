# /proc/ish: asking the app about itself

`/proc/ish` is where the guest can see the app it is running inside — the build,
the host device, your settings, the JIT's state. It is AOK's own addition to
procfs; nothing on real Linux has it.

```sh
cat /proc/ish/version        # iSH-AOK 1.3 (553)
cat /proc/ish/host_info      # the Mac or iPad underneath: OS, release, hardware
cat /proc/ish/ips            # this device's network interfaces
cat /proc/ish/colors         # the 16 ANSI colours, drawn -- a quick theme check
cat /proc/ish/BAT0_capacity  # battery charge, 0-100
cat /proc/ish/BAT0_status    # Charging / Discharging / Full
cat /proc/ish/BAT0           # both of those plus low-power mode, one per line
cat /proc/ish/UIDevice       # the UIDevice the app sees: model, OS, orientation
```

## Your settings, from the guest

`/proc/ish/defaults/` is a directory, one entry per app preference:

```sh
cat /proc/ish/defaults/theme         # "Nord"
cat /proc/ish/defaults/font_size     # 15
cat /proc/ish/defaults/line_height   # 1
ls  /proc/ish/defaults               # everything available
```

Values come out as JSON: strings quoted, numbers bare, booleans as `true` or
`false`.

The entries are `0444 root:root`, so an ordinary user can read them and gets
`EACCES` on write. **Root can write them**, and the change takes effect live and
persists, exactly as though you had used Settings:

```sh
echo '"Nord"' > /proc/ish/defaults/theme    # a string value, so JSON-quoted
echo 16 > /proc/ish/defaults/font_size
```

The value goes through the same validation the Settings screen uses, so a
rejected value fails the write rather than wedging the app. Removing an entry
resets that preference to its default:

```sh
rm /proc/ish/defaults/font_size
```

If **Open Everything as Default User** is on you are not root, and these
become read-only unless you `sudo`. Reading is the common case anyway: a script
that needs to know how the app is configured — which theme is active, whether an
accelerator is on, what the font size is.

The names are lower-cased and underscored versions of the Settings labels, so
`hide_extra_keys_with_external_keyboard` is the switch of that name. **One
exception**: the switch above is still `login_as_default_user` here, because the
Settings label was renamed once its reach grew beyond the login shell and the
stored name stayed put. A few notable ones:

| entry | what it reflects |
|---|---|
| `theme`, `font_family`, `font_size`, `line_height` | [Appearance](themes.md) |
| `enable_hle`, `enable_crypto_accel`, `enable_pix_accel` | the optional accelerators |
| `enable_multicore`, `enable_extralocking` | emulator behaviour |
| `launch_command`, `boot_command` | what a session starts |
| `llm_*` | the [LLM client](llm-chat.md) |

### Line Height

`line_height` is worth a note because it is the one appearance setting whose
purpose is not obvious. It is a multiplier from **0.70 to 1.30**, in steps of
0.05, default 1. It exists to close the one- or two-pixel band that can appear
above block glyphs — the characters box-drawing and TUI programs use — where a
row of blocks shows hairline gaps instead of a solid field. Nudging it just
below 1 usually removes them. Settings → Appearance → Line Height.

That 0.70-1.30 is what the stepper offers. Writing `line_height` directly
accepts anything above 0.5 and up to 2 -- the floor exists because below about
half the measured height even capitals are cut. The stepper is the range worth
using; the validator is only a guard against nonsense.

## Writable entries

Most of `/proc/ish` is read-only. The exceptions:

| entry | mode | what it does |
|---|---|---|
| `workspace` | `0666` | ask the app to open a [Workspace](workspace.md) tool; writable by an ordinary user, because opening a window is not an administrative act |
| `roots` | `0644` | the installed [root filesystems](roots.md); root-only, because switching them is |
| `amd_jit`, `amd64_jit`, `<arch>_jit_fuse` | `0644` | JIT engine and instruction-fusion switches, per guest architecture |
| `i386_no_cache_comm`, `i386_single_step_comm` | `0644` | i386 debugging aids, named for the process they apply to |

The `*_jit_fuse` entries report which fusions are on and let you turn one off
while you are bisecting a suspected JIT bug:

```sh
cat /proc/ish/arm64_jit_fuse
# bcond on
# ldst on
# ldcmp on
# retcache on
```

## Things worth knowing

- **`/proc/ish` is the same in every root and every chroot.** AOK has no mount
  or PID namespaces, so there is one true kernel underneath everything — see
  [00-overview.md](00-overview.md).
- **`documents` gives the app's Documents directory** as a host path, which is
  what the Files app and iTunes file sharing see.
- **`colors` is a rendering test, not a list.** Reading it prints the sixteen
  ANSI colours as coloured text, so you can see at a glance what the current
  [theme](themes.md) does to them.
