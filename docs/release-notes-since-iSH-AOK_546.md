# Release Notes Since `builds/iSH-AOK_545`

Thirty-four commits, and unlike 545 this is not a CPU release. The centre of
gravity is the terminal: the tty layer had never seeded `c_cflag` at all, which
meant every tty came up reporting `B0` -- hang up -- and four commits put that
right, per driver, the way Linux does. Around that sit the two crashes that
Apple's reports showed actually hitting users on 545, a launch-path fix for a
watchdog kill that could stop the app starting at all, and the Workspace finally
becoming a place you can leave. Two pieces of harness rot are also fixed; both
had been quietly hiding test signal for weeks.

One feature was implemented and then pulled: external display support works, but
device testing found it fights iPadOS's own use of a second screen, and taking
away a working iPad workflow to add it was the wrong trade.

## Highlights

- **A fresh tty no longer claims to be hung up.** `tty_alloc` left
  `termios.cflags` at zero, so every tty reported `B0`, which in termios means
  hang up. Now seeded `B38400|CS8|CREAD` (`d532698d`), on the real-tty path too
  (`c4e9af90`), with `HUPCL` split per driver as Linux does -- `vt.c` takes the
  console default including `HUPCL`, `pty.c` overrides it for both ends
  (`2684bbd4`) -- and keyed off the device type rather than the driver identity,
  because the app drives its terminals through a driver of its own while
  registering them as pty slaves (`294ec60e`).
- **The top crasher on 545 is fixed** (`d93da4e0`). `-[Terminal syncWindowSize]`
  read its unowned `struct tty *` back-pointer three separate times; the
  emulator thread clears it during teardown, so the NULL check guarded only
  itself and `tty_set_winsize` could be handed NULL. Six devices across iOS 16.7
  to 26.5.2. The reported fault address is `offsetof(struct tty, winsize)`
  exactly.
- **The tty back-pointer is now a counted reference** (`7aadd5cb`). Reading it
  correctly is not enough -- `tty_release` frees the struct, so the value can go
  stale between read and use, and the same defect was in `-sendInput:` (every
  keystroke) and `-destroy`. New `tty_lookup_ref()`/`tty_put()` take a reference
  under `ttys_lock`, the lock the free is documented to hold. A lock could not be
  used: `tty_release` calls `ops->cleanup` -> `setTty:` -> `@synchronized(self)`
  while already holding `tty->lock`, so that order is an AB-BA.
- **Files extension no longer crashes on `readdir(NULL)`** (`0236b3c0`). The
  enumerator's `fdopendir` was unguarded, the same defect already fixed in
  `childItemCount` with a comment explaining exactly this failure. Seen on builds
  540 through 545.
- **The app can be killed at launch on a large filesystem** (`7ef8f91b`). The
  fakefs schema migration cost a full `readdir` of the parent directory for every
  path needing an escape -- and the gate is any uppercase letter or byte >= 0x80,
  so `README` and `Makefile` qualify, not just exotic names. It runs on the main
  thread inside `mount_root`, and RunningBoard killed the app before it finished
  (`0xdead10cc`, thread 0 in `readdir`). The query is `order by path`, so caching
  the current directory's listing collapses it to one `readdir` per directory:
  measured 1.60s -> 0.04s on a 24940-path root, with the syscall time going 1.56s
  -> 0.01s.
- **Workspace is no longer one-way (GH #546).** The Settings button always read
  "Workspace" and only ever went one direction, so the only way back to the shell
  was force-quitting. It also rebuilt the root controller on every switch,
  discarding the Workspace's open tool windows and layout. Both modes are now
  kept alive on the window and the button is labelled with its destination
  (`2dc72cef`).
- **UTS namespaces** (`3804e3ea`), so `unshare --uts` and per-container hostnames
  work, with `/proc/PID/ns/*` magic links answering `stat` and `access`
  (`2272e768`).
- **`in`/`out` raise `#GP`, not `SIGILL`** (`a2ba4b74`). Port I/O is ring-0; a
  user process faulting on it is a deliberate hypervisor-probe idiom, and the
  fault it expects is `SIGSEGV`. `lscpu` uses exactly that to detect VMware and
  does not handle `SIGILL`, so it died at startup on the amd64 guest.
- **Per-CPU topology and cache in sysfs** (`f9f79edd`), sourced from real host
  geometry, plus a perf-level tiering fix. Topology is deliberately flat: guest
  CPUs are host pthreads the scheduler migrates at will and `sched_setaffinity`
  is a no-op, so advertising clusters would invite placement decisions that
  cannot take effect.

## User-Facing Changes

### Emulation

- `fninit` implemented (`44f67e81`).
- `in`/`out` on both x86 frontends now raise `#GP` in user mode (`a2ba4b74`).
- HLE's libc-mapping test is memoized per mapping rather than per block
  (`f7273dd0`).
- A leftover amd64 zero-rip probe is gone from the task run loop (`57489351`).
- An x86_64-host build fix for a stray `.macro` (`797b781f`).

### Filesystem and kernel

- UTS namespaces (`3804e3ea`); `/proc/PID/ns/*` answers `stat`/`access`
  (`2272e768`).
- fakefs migration scans each directory once (`7ef8f91b`).
- `fs/path` uses `strrchr` for the parent-directory split (`1abe3add`).
- `uname -v` reports a real build identifier instead of the current clock
  (`fbb99c5e`). It formatted `time(NULL)`, so it changed every time you asked and
  told you nothing about which build a device was running. Now the app's version
  and build number on iOS, compile timestamp on the CLI.

### App

- The tty `c_cflag` series (`d532698d`, `c4e9af90`, `2684bbd4`, `294ec60e`).
- Workspace/Shell two-way switch (`2dc72cef`).
- Terminal winsize and tty-reference crash fixes (`d93da4e0`, `7aadd5cb`).
- FileProvider enumerator guard (`0236b3c0`).
- The terminal-switcher long press is advertised on the views that have it
  (`a9cab3ab`).
- LLM chat collapses `<think>` blocks behind a Thinking disclosure (`ccdf1ca2`).
- ktop truncates the command column and filters control bytes out of it
  (`0c685e64`). A newline in a process's argv ended the row mid-table and a
  carriage return rewrote it from column 0, so one process could corrupt the
  whole display.

## Validation

### Guest regression suites

- **CLI, four architectures concurrently:** i386 105 pass, amd64 105 pass, arm64
  103 pass, riscv64 96 pass. Zero failures.
- **Device (M4 iPad), four architectures concurrently via `/AOK/roots` chroots:**
  clean, with one caveat below.
- **e2e:** 7/7, up from 2/7 before the harness fix.
- `float80` still fails on Apple Silicon by design -- the 80-bit float library is
  x86-host-only.

### Harness rot fixed during the sweep

Both of these had been hiding signal, and both produced failures that looked like
product bugs:

- `sysfs_cpu_topology` counted a `processor` line *or* a `hart` line as a CPU
  (`f7999b2a`). Real riscv64 emits both per CPU and `fs/proc/root.c` matches that
  faithfully, so the test saw 8 CPUs against 4 online and failed. The emulator was
  right. That test shipped in `f9f79edd` **this release**, so it was red on
  riscv64 from the moment it landed.
- `tests/e2e/e2e.bash` provisioned its rootfs only when the directory was absent
  (`2927593d`), so an image whose `apk add` never landed was skipped forever
  after. The local image dated from May and had no toolchain at all, so every
  compile-based test failed with a bare "gcc: not found". Now re-provisions when
  the toolchain is missing.
- `at_empty_path` and `fsopen_move_mount` now skip rather than fail when not
  privileged (`0cb09141`). Run over ssh as a normal user they reported EPERM and
  EACCES, which reads like an AT_EMPTY_PATH or mount-API regression rather than
  the account the suite was launched under.

### App-level

- Workspace/Shell switching verified end to end on both iPad and iPhone
  simulators, including that the shell keeps its session across the round trip
  and the Workspace keeps its layout, and that both survive background/foreground.
- Migration correctness checked two ways: re-running it over an already-migrated
  tree changes nothing (27424 entries, identical listing), and a synthesized
  pre-migration tree of 60 raw-named files migrates with all 60 readable and
  byte-correct afterwards.

## Known Issues

- **`ptrace_group_stop` can time out under a 4-way device run.** It passes in
  ~1 second in isolation and concurrently on all four architectures, against a
  120-second watchdog; it only failed while four emulated toolchains were
  saturating the device during the build phase. Nothing in this release touches
  `ptrace.c`, `signal.c`, `group.c`, `poll.c` or `sync.h`. Set
  `ISH_TEST_WATCHDOG_SCALE=4` for the device pass. The 1s-vs->120s gap is wide
  enough that a starvation bug under saturation cannot be fully excluded.
- **External display support is not in this release.** See below.
- `^C`/`^Z` can echo as a placeholder glyph rather than `^C`/`^Z` on a tty whose
  `echoctl` is off. iSH seeds `ECHOCTL` on and echoes correctly in both modes --
  verified directly against the line discipline -- so this is the login path in
  that particular root clearing it. `stty echoctl` restores it. Not a regression:
  the tty work this release changed `cflags` only, never `lflags`.

## Maintainer Notes

- **External display support was implemented and then reverted** (`ea5985ec`,
  `57380ba6`, `cc0b5b21`, reverted in `ad602c7c`). Testing on an M4 iPad with a
  real monitor found it conflicts with how iPadOS already uses a second screen:
  the app would not open on the external display, resized oddly when dragged
  there, was dragged back when a second window opened on the built-in display,
  and opening a terminal applet disturbed the primary display. 545 declared no
  external-display scene, so an iPad got plain Stage Manager behaviour; the new
  `UIWindowSceneSessionRoleExternalDisplayNonInteractive` scene plausibly
  pre-empts that, though that cause is **not proven**. The design notes in
  `docs/external_display_plan.md` carry the findings and the shape of a fix.
  Revert the reverts to resume.
- **DerivedData corruption blocks CLI builds** and presents as a dependency
  failure rather than what it is. Symptom: `module file '...pcm' not found`, then
  `Internal inconsistency error: never received target ended message`. Deleting
  only `ExplicitPrecompiledModules/` makes it worse -- the build description still
  names the deleted artifacts, and the Xcode GUI then fails on `.scan` files. Wipe
  the whole `iSH-AOK-<hash>` DerivedData directory.
- `MakeXcodeAutoCompleteWork` now has `$(SRCROOT)` on its header search path
  (`ca0777a1`). It had none, so the indexer could not resolve `#include "debug.h"`
  in any of the 84 emulator sources it exists to index, and reported errors on a
  build that compiles fine.
- Privileged-port wildcard binds are loopback-only; documented (`9029596f`).

## Commit Range

`builds/iSH-AOK_545..builds/iSH-AOK_546` -- 34 commits.
