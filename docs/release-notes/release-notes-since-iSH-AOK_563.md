# Release Notes Since `builds/iSH-AOK_562`

40 commits. This build puts the terminal on an external display for
iPhone users (#540), untested on real hardware. It fixes 562's one crash, at
launch when resuming a session that had `/dev/location` open. It also fixes
a native zsh that could not write after resuming a session it had suspended,
and btop taking a whole core while its network box is shown. Shortcuts' Run
Command handles big output, output that is not UTF-8, and the exit status on
roots with a real init. Native programs no longer share a time zone, a random
number stream or a few other pieces of C library state.

## Before you update

**Saved sessions from 562 will not restore.** An image is tied to the build
that saved it. The guest boots fresh, and the resume picker marks such an
image "can't be restored".

## External display on iPhone (#540) — untested on hardware

Connect an iPhone to a monitor, with a USB-C or Lightning video adapter or
with AirPlay Screen Mirroring to a TV or a Mac, and the terminal moves to the
big screen at its shape. The phone keeps the keyboard, the keyboard bar and
touch. Until now every app got a letterboxed copy of the phone's screen.

- **A terminal** fills the external screen; you type on the phone and read on
  the display.
- **The Workspace** shows its frontmost terminal window there, and follows
  when you bring another to the front. With no terminal window open, the
  display says what to open.
- **The Wayland Display** is mirrored, with its cursor, and the desktop is
  sized to the external screen while the mirror exists. The phone stays the
  touch surface, touchpad mode included.
- **Disconnect** and everything comes back to the phone.

**Nobody has tried this on a real display yet.** It was built and run in the
simulator, but no iPhone and monitor were at hand before the release. If you
try it, please say what you saw on
[#540](https://github.com/emkey1/ish-AOK/issues/540), working or not.
**iPads are unchanged**: with Stage Manager they already get real iSH-AOK
windows on a monitor, and the first attempt at this (546) was taken out after
it fought Stage Manager. `/AOK/docs/external-display.md` has the details.

## Crash fixes

- **Resuming a session with `/dev/location` open crashed at launch**, every
  time, on 562. That is the session #631's keep-alive
  (`cat /dev/location > /dev/null &`) leaves behind. The restore reopened the
  device on the main thread, which waited on itself. Reproduced with the
  reporter's stack in a simulator; it resumes now.
- **A launch could be ended by iOS while it opened the root's database**
  (0xdead10cc, seen once on 561). The boot now runs under a background-task
  assertion, as the app's other mounts already did. Not reproduced.
- **A crash before the guest had booted**: a list of processes was walked
  before anything had initialised it. A walk over a list that was never set
  up is now empty.

## Suspend to disk

- **A native zsh restored after `echo suspend > /proc/ish/checkpoint` could
  not write.** That command, the one `/AOK/docs/suspend.md` gives, leaves the
  shell in the middle of its own redirection when the session is saved: stdout
  on the checkpoint file, the terminal parked aside until the `echo` finishes.
  A restored zsh is relaunched at its prompt and never finished it, so every
  command said `write error: invalid argument`. The session now records each
  descriptor where the shell's own prompt would have it.
- **A restored zsh no longer gains a descriptor at every resume.** Each
  relaunch kept the last one's internal descriptors (its input, its terminal)
  beside the ones it opened itself.

## Shortcuts: Run Command

- **Output over 256 KB** failed the action ("killed by signal 9"). The command
  now runs to its end with its real exit status, and the action returns the
  first 256 KB with `[iSH-AOK: output truncated at 256 KB]` on a line of its
  own.
- **Output with one byte that was not UTF-8** came back as empty text. Each
  bad byte is now shown as �, and the rest of the output is kept.
- **"Fail on Non-Zero Exit" passed failing commands** on any root with a real
  init, because init reaped the command before the action could read its exit
  status. The status is now recorded by whoever reaps it. When there is
  truly none to be had, the action fails rather than vouching.

`/AOK/docs/shortcuts.md` describes the action as it now behaves.

## Native programs

- **Each native program reads its own time zone.** Native programs in two
  roots with different `/etc/localtime` could briefly share one zone, and a
  `date` straight after the file was replaced showed the old one.
- **Each has its own `rand()` stream**, so `RANDOM=42` in native zsh repeats
  its numbers again while other zshes run. The same goes for `inet_ntoa`'s
  buffer and the multibyte conversion state, which were one per app.
- **SmallCLUE**: `wc -m` counts a character split across two reads once; the
  `less`, `more` and `md` prompt stays on one line on a phone; `top -b` leaves
  a blank line between frames; `base64` is about 30 times faster on large
  input; `smallclue <typo>` gives a one-line hint; and `wget` wipes its
  password buffer when it is done.

## Kernel and conformance

- **btop no longer takes a whole core with its network box shown** (reported
  on Discord, Alpine 3.24 arm64). Reading one of
  `/sys/class/net/<if>/statistics/` cost about 140 lookups of the host's
  interface list, and btop reads two per interface every second. On a Mac
  with 33 interfaces, btop with the box went from 89% of a core to 3.5%; the
  counters are still read fresh every time. `ip addr` is also faster.
- **On amd64, a 64-bit result just under 4 GiB is not an error.** An `lseek`
  landing in the last 4095 bytes below 4 GiB returned -1 with a nonsense
  errno. ppc64le had the same fault.
- **A call through a NULL function pointer** on arm64, riscv64 and i386 now
  reports `SIGSEGV` with `SEGV_MAPERR` and address 0, as Linux does, instead
  of `SI_KERNEL`.
- **x87: a load of an 80-bit value reads its 10 bytes, not 16.** On x86_64
  Linux hosts (the CLI) a value near the end of a page could fault the
  emulator. Apple hosts had already stopped using that path.

## The app

- **VoiceOver** labels and hints for the bar buttons of the desktop setup,
  chats, chat destinations, file browser, snippets and settings windows, the
  snippet editor's Cancel and Save, and the LLM Chat file-change Revert
  button, in all nine languages (from the bot PRs #654, #658 and #659).

## Testing

The full regression suite on the five release roots — Alpine 3.24 i386,
amd64, arm64 and riscv64, and Devuan 6 arm64 with glibc — on this build:

| root | M5 Mac (command line) | iPad simulator (the app) |
|---|---|---|
| i386 | 457 passed | 458 passed |
| amd64 | 477 passed | 478 passed |
| arm64 (musl) | 395 passed | 396 passed |
| riscv64 | 387 passed | 388 passed |
| arm64 (Devuan, glibc) | 395 passed | 396 passed |

No failures in either column.

**No real device ran the suite for this build**: the maintainer was
travelling without one. The second column is the app itself, built for the
simulator, with each root booted as the running root under its own init, in
a simulated iPad Pro (M5, iOS 27.0) on the M5 Mac
(`tools/sim-suite.sh`). That checks the app's own kernel, JIT and boot, but
not iOS's memory limits, thermal state or suspension on real hardware. The
ppc64le root was not run for this build. The e2e suite passed 7 of 7
from a freshly built test filesystem.

## Known issues

- **The external display is untested on hardware** (see above).
- **Sandboxes cannot reach the network** through a veth: there is no
  forwarding or bridge yet.
- **ssh sessions still drop when the device locks**
  ([#631](https://github.com/emkey1/ish-AOK/issues/631)): iOS takes a
  suspended app's connections.
- **A native program's own threads share one task** and are not stopped
  when the program ends.
- **Xfce draws no desktop backdrop**: icons on black.
- **On distribution roots, `who` misses the app terminal's own login.**
- **ppc64le is experimental**: no speed work yet, and no POWER9 instructions.
