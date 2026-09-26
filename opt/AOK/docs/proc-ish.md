# /proc/ish: asking the app about itself

`/proc/ish` is where the guest can see the app it is running inside — the build,
the host device, your settings, the JIT's state. It is AOK's own addition to
procfs; nothing on real Linux has it.

```sh
cat /proc/ish/version        # iSH-AOK 1.3 (556)
cat /proc/ish/host_info      # the Mac or iPad underneath: OS, release, hardware
cat /proc/ish/ips            # this device's network interfaces
cat /proc/ish/colors         # the 16 ANSI colours, drawn -- a quick theme check
cat /proc/ish/BAT0_capacity  # battery charge, 0-100
cat /proc/ish/BAT0_status    # Charging / Discharging / Full
cat /proc/ish/BAT0           # both of those plus low-power mode, one per line
cat /proc/ish/thermal_state  # nominal / fair / serious / critical
cat /proc/ish/timezone       # the device's time zone, e.g. Europe/London
cat /proc/ish/UIDevice       # the UIDevice the app sees: model, OS, orientation
cat /proc/ish/applets        # the Workspace applets that are open
cat /proc/ish/arch           # every process's guest architecture, one line each
cat /proc/ish/host_ports     # Mach port usage -- a leak diagnostic
```

The Workspace applets are app interface, not processes, so `ps` and `top` do
not show them. `applets` lists them instead: see
[workspace.md](workspace.md#procishapplets-what-is-open).

## Every process's architecture, and who may ask

AOK runs four guest architectures plus native host code at once, and `arch`
says which is which:

```sh
$ cat /proc/ish/arch
PID ARCH
1 aarch64
842 aarch64(n)
917 x86_64
```

One line per process (the thread-group leader): the machine name
`uname` reports inside it; for a program compiled into iSH-AOK and running as
host code, the host's machine marked `(n)` — `aarch64(n)` on every Apple
device (see [native-programs.md](native-programs.md)); or `-`
for a task caught without an address space. A zombie is listed with what it
ran when it began to exit, until it is reaped; before 557 zombies were left
out, and `ktop` showed `?` for them. This
exists because [ktop](ktop.md)'s ARCH column used to read the ELF header
behind `/proc/<pid>/exe`, and since another user's process became off-limits
to inspect (below), that returned "?" for anything not your own — real Linux
has no equivalent because it runs one architecture, so there was nowhere to
borrow the idea from.

**Another user's process is not yours to inspect**, the same way it is not on
Linux: `ptrace_may_access()` gates `/proc/<pid>/maps`, `smaps`, `mem`,
`environ`, `auxv`, `cwd`, `root`, `exe`, `fd`, `fdinfo` and `io`, and
`PTRACE_ATTACH`/`PTRACE_SEIZE`, on the caller's ids matching the target's real,
effective and saved ids, the target being dumpable, or the caller holding
`CAP_SYS_PTRACE` — a thread of your own process may always look. `status`,
`stat`, `cmdline` and `comm` stay public, which is what `arch` relies on. A
process that calls `PR_SET_DUMPABLE(0)` (as `ssh-agent` and `sshd` do, to keep
its memory away from the user's other processes) or that ran a set-id exec is
undumpable and gated even for its own uid.

`host_ports` is a different kind of diagnostic, for developers rather than
day-to-day use: the number of Mach ports this process holds and how many are
dead names (a right to something that no longer exists — a thread that already
exited, typically). iOS kills the app outright at a per-process port limit;
this file is how you catch a leak before that happens rather than after. A
host with no Mach ports (the Linux CLI build) says so.

## Battery, heat and the time zone

The `BAT0` files are AOK's own, older than the standard ones, and they keep
their format: the charge has two decimals (`83.00`). With no battery to report
— a Mac without one, or the command-line build — the state reads `Unknown` and
the charge is negative.

**Linux tools look in `/sys/class/power_supply` instead**, and the battery is
there too: `BAT0` with `capacity`, `status`, `present`, `type` and `uevent`,
and an `AC` adapter whose `online` says whether it is plugged in. That is where
waybar's battery module and btop look for it. Only what iOS reports is
published — a percentage and a charging state. There are no energy, voltage,
current or time-left files, because iOS has no such figures, and a number made
up to fill a file is worse than no file; htop's battery meter needs one of them
and shows N/A. With no battery, the directory is empty, as it is on a Linux
machine without one.

`thermal_state` is iOS's own coarse reading of how hot the device is running:
`nominal`, `fair`, `serious` or `critical`. iOS gives no temperature, so there
is none here. The command-line build has nothing to ask and says `unknown`.

`timezone` is the device's IANA zone name — the name to find under
`/usr/share/zoneinfo`. The guest's clock follows it; see
[roots.md](roots.md#the-time-zone-following-the-device). The command-line build
reports the host's zone, read from its `/etc/localtime`, and an empty line if
it cannot tell.

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

## Memory and swap

Four files describe what the emulator is doing about memory. They are read-only
diagnostics — nothing here changes behaviour except `swap`, and then only on a
build that offered guest control.

```sh
cat /proc/ish/swap          # the swap area: capacity, use, why it refuses
cat /proc/ish/mem_guard     # the jetsam guard: what it sees, what it refuses
cat /proc/ish/zswap         # compressed memory: what it holds, flash it saved
cat /proc/ish/mem_compress  # what compressing a given process would buy
```

`swap` is the pager's whole state in one screen. The lines worth knowing:

| line | what it tells you |
|---|---|
| `enabled` / `state` | whether there is an area at all, and why not if there isn't |
| `slots_total` / `slots_free` | capacity in 16 KiB slots, and how much is still free |
| `bytes_written` / `write_window` | what paging has cost so far, against the 24-hour budget |
| `budget_refusals` | evictions declined because that budget is spent |
| `kswapd` | the background sweeper: passes made, bytes reclaimed |
| `thrashing` | pages coming straight back, so reclaim has paused itself |
| `release_works` | whether releasing a page actually moves the memory the OS charges us for |
| `direct_reclaim` | bytes freed for an allocation that would otherwise have failed |
| `alloc_failures` / `no_area` | evictions refused because the area is full, or absent |
| `io_errors` | failed reads or writes against the area |

`zswap` is the compressed tier — see [swap.md](swap.md) for what it is and how
to turn it on. It answers "did it actually do anything", which a passing swap
test does not:

| line | what it tells you |
|---|---|
| `on` / `cap` | whether there is a pool, and how big you asked for |
| `objects` | frames held compressed right now |
| `pool` / `stored` / `original` | what it occupies, the compressed bytes in it, and what those frames used to occupy — `original` over `stored` is the ratio you are getting |
| `stores` / `loads` | frames kept in RAM, and faults served back out of it |
| `declined` | frames that would not compress, or arrived with the pool full, so they went to flash |
| `flash NOT written` | the wear saving, and it comes off the 24-hour write budget |

`mem_compress` is a measurement rather than a report: write a pid to it and it
walks that process's resident pages and says what compressing them would save,
at what cost per page. It exists because the host already compresses idle
memory for free — but that does not move `phys_footprint`, the ledger iOS kills
on, so only compression AOK does itself can help.

```sh
echo $$ > /proc/ish/mem_compress && cat /proc/ish/mem_compress
```

`mem_guard` answers the question "why did that allocation fail?". It prints the
machine's memory, this process's own ceiling and headroom, the system pressure
level, and then two verdicts: whether a large growth is being refused right now,
and whether the throttle is engaged. A small growth is always allowed — see
[swap.md](swap.md) and the note in `mem_growth_refused`.

Two more exist for development and are of no use in normal running:
`swap_evict`, which forces eviction of a named pid's address space, and
`mem_release_probe`, which measures whether releasing pages moves the host's
ledger at all. Both are writable only on a build that offered guest control.

## When something is not waking up

```sh
cat /proc/ish/wake_signals
```

A task parked in a blocking call is woken by a signal from the thread that wants
its attention. On Darwin that poke is occasionally swallowed in a way that
leaves the target permanently deaf, and this file counts how often that has been
detected and repaired — separately for a sleeping task and for one in
`poll`/`select`/`epoll`. It also reports the cap that bounds any wait, so a lost
wake costs at most that much latency rather than the rest of the process's life.

**Zero is the expected state, and a non-zero count is not by itself a fault** — a
repair means the mechanism worked. A number that *climbs* while guest processes
hang is the signal worth chasing.

## Things worth knowing

- **`/proc/ish` is the same in every root and every chroot.** AOK has no mount
  or PID namespaces, so there is one true kernel underneath everything — see
  [00-overview.md](00-overview.md).
- **`documents` gives the app's Documents directory** as a host path, which is
  what the Files app and iTunes file sharing see.
- **`colors` is a rendering test, not a list.** Reading it prints the sixteen
  ANSI colours as coloured text, so you can see at a glance what the current
  [theme](themes.md) does to them.
