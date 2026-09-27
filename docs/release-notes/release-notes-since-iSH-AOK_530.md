# Release Notes Since `builds/iSH-AOK_530`

These notes summarize changes from `builds/iSH-AOK_530` intended for `builds/iSH-AOK_531`.

## Highlights

- Large interactive-performance pass: typing, echo, and bulk terminal output are noticeably faster, and a heavy guest workload (notably `go build`/`go run`) no longer freezes the app's UI for seconds at a time.
- Fixed several on-device terminal rendering bugs that corrupted output under real programs — UTF-8 box-drawing glyphs, stale/duplicated regions, and `htop`'s CPU meters all render correctly now.
- Fixed CPU-count reporting: `htop` and `nproc` no longer report a single CPU on the 64-bit guest, while multi-threaded guest tools are deliberately given a smaller CPU count so they stop saturating every core and starving the UI.
- Fixed multiple amd64 (64-bit) emulation correctness bugs in instruction decode, the `LOCK` prefix path, and JIT dirty-page tracking.
- Security hardening: replaced unbounded `sprintf`/`vsprintf` with bounded equivalents across the codebase and fixed a unix-socket bind buffer overflow.
- Accessibility: added VoiceOver labels and selection state across Workspace controls, the browser toolbar, and the chat Send button.
- Networking and runtime regression fixes for `apt`, VNC, and procfs, plus local Bonjour hostname resolution.

## User-Facing Changes

### Interactive Performance and Responsiveness

- Removed a thread-group mutex from the return path of every syscall and exception; multi-threaded guests no longer serialize on it once per syscall.
- Removed a per-write heap allocation from the terminal output path (`tty_write`), which sits in the hot path for prompts, command output, and echoed input.
- Rewrote the terminal output-encoding step to a single pass, eliminating a per-character Objective-C dispatch that dominated bulk-output throughput (`cat`, `ls`, build logs).
- Heavy guest workloads no longer make the app unresponsive:
  - Guest threads now run one quality-of-service band below the UI thread, so the interface can preempt guest compute.
  - The guest is reported a reduced CPU count for scheduler sizing (e.g. the Go runtime's `GOMAXPROCS`, `make -j$(nproc)`), so a build burst no longer spawns one saturating thread per host core. `/proc/cpuinfo` and `/proc/stat` still report the true core count.

### Terminal Rendering Correctness

- Fixed UTF-8 corruption in the on-screen terminal: output is now fed through hterm's streaming UTF-8 decoder, so multibyte glyphs that straddle two output chunks are no longer mangled into replacement characters.
- Fixed stale/partially-painted regions for rapidly-updating full-screen programs by forcing a coalesced viewport repaint after output.
- Fixed doubled/garbled regions (most visible in `htop`'s CPU meters) caused by the output watchdog re-sending in-flight data after a slow — but successful — webview write.
- Implemented `REP` (`CSI Pn b`, repeat preceding character) in the bundled terminal emulator. `ncurses` programs such as `htop` use it to fill runs; without it, meters and headers collapsed and misaligned. (Carried on the `emkey1/libapps` fork the submodule now points at.)
- Mounted `sysfs` on `/sys` at guest boot in the app (the CLI already did this). `htop` 3.4+ derives its CPU count from `/sys/devices/system/cpu`, so without it `htop` displayed a single CPU even when `/proc` was correct.

### CPU Reporting and Multicore Behavior

- `sched_getaffinity` now returns a kernel-accurate cpumask whose size is a multiple of the guest's `sizeof(long)`. The previous odd-sized return caused musl's `__get_nprocs` (used by `nproc` and `sysconf`) on the 64-bit guest to fall back to reporting one CPU.
- Net effect on device: `htop` shows the real core count (via sysfs) while `nproc` and the Go runtime see the reduced, scheduler-visible count — honest display with throttled parallelism.

### amd64 (64-bit) Emulation

- Fixed `modrm` decode for the no-base + `disp32` form (`mod=00`, `base=101`) in both the interpreter and JIT, so `r13`-as-base is no longer confused with a displess encoding.
- Fixed `LOCK`-prefix handling in the `xchg` helper and routed `LOCK not`/`neg` on memory operands to the interpreter, which parses the prefix correctly.
- Corrected an inverted address range in the `cc1` force-interpreter guard.
- JIT gadgets now track the dirty page only on writes (reads no longer clobber it) and revalidate writable TLB hits where host page mirroring requires it.
- Quieted experimental amd64 boot: diagnostic JIT-fallback and tracked-tty traces are now gated behind environment variables and off by default, so the log is clean on a normal boot.

### Security Hardening

- Replaced unbounded `sprintf`/`vsprintf` with `snprintf`/`vsnprintf` across the codebase (including `fs/pty.c`, `fs/proc/pid.c`, `fs/sock.c`), using structural buffer sizes and clamping lengths used in subsequent calculations. This closes buffer overflows reachable through attacker-influenced process names, pids, fds, and socket addresses.
- Fixed an unbounded `sprintf` into `sun_path` during unix-socket binding; the path is now bounded and returns `ENAMETOOLONG` rather than overflowing.

### Accessibility (VoiceOver)

- Added accessibility labels and selected-state traits to Workspace tabs, themes, and window controls.
- Added labels to the symbol-only browser toolbar buttons (`<`, `>`, reload, `+`, `×`), with the reload control announcing "Reload"/"Stop" by load state.
- The chat Send button now updates its accessibility label for its loading state.

### Networking

- Added local Bonjour (`.local`) hostname resolution.
- Fixed runtime regressions affecting `apt` networking and VNC, and a separate VNC/procfs regression.

### Filesystem and Runtime Correctness

- Fixed setuid-root `exec` to restore the full capability set and set the saved-set-uid correctly.
- Fixed five correctness bugs in the SQLite-backed fake filesystem.
- `futex` now reports a successful wake (returns 0) when the wait list is emptied after an `ETIMEDOUT` slice, instead of spuriously timing out.
- Minor path-handling optimizations in `tmpfs_getpath` and `sys_getcwd_common`.

## Known Issues

- The reduced scheduler-visible CPU count means `nproc` (and therefore the Go runtime and `make -j$(nproc)`) report fewer CPUs than `/proc/cpuinfo`/`htop` on iOS. This is intentional — it keeps the UI responsive under heavy guest load — but is an intentional inconsistency between the two counts.
- The 64-bit (amd64) guest boot remains experimental.

## Maintainer Notes

- `builds/iSH-AOK_530` points to commit `bdf44174`.
- `builds/iSH-AOK_531` should point to the current release-candidate commit after this note is committed and tagged.
- The terminal `REP` fix lives in the `emkey1/libapps` fork (commit `d1b1ab1c` on branch `ish-aok`); the submodule URL and pin were updated accordingly in `02c5a9c0`. A fresh `git clone --recurse-submodules` pulls it automatically.
- Release validation status at tag time:
  - Guest regression suite (`/AOK/tests`) passed 13/13 on device (atomics, signal core/restart/realtime/altstack/poll, eventfd, futex, process lifecycle, pthread sync).
  - Synthetic Go release-smoke exercised cold/warm `go build`, `go test`, and `go run` through the freshly built emulator.
  - Manual on-device verification of the terminal-rendering and CPU-count fixes on both the i386 and experimental amd64 roots.

## Commit Range

- `fc58a49b` app: mount sysfs on /sys at guest boot
- `e055bbf6` resource: return a kernel-accurate sched_getaffinity cpumask size
- `c326d7b4` amd64: gate boot-time diagnostic traces behind env vars
- `508cb401` cpu: report fewer CPUs to guest scheduler sizing than to /proc/cpuinfo
- `4ca96334` task: lower guest thread QoS so heavy guest load doesn't starve the UI
- `02c5a9c0` deps: point libapps submodule at emkey1 fork with REP fix
- `b9aeda89` app: stop terminal output watchdog from duplicating in-flight data
- `5fa1f803` app: fix terminal UTF-8 corruption and stale-region rendering
- `024243c4` app: encode terminal output literal in one pass
- `8a83cdfc` tty: avoid per-write malloc in tty_write OPOST path
- `28501aec` task: read group->stopped locklessly on the interrupt-return fast path
- `6d7088e8` fakefs: fix five correctness bugs in fake.c
- `1136f4ce` exec: fix setuid-root exec to restore full caps and correct saved-set-uid
- `12ad6244` futex: return 0 (woken) when list_null is true after ETIMEDOUT slice
- `16342682` jit gadgets: only track dirty_page on writes, revalidate writable TLB hits
- `3534e668` amd64: fix modrm no-base decode, LOCK prefix handling, cc1 interp range
- `6ae6ec1a` Palette: dynamically update Send button accessibility label
- `0f56a990` Bolt: hoist `pwd` string length in `sys_getcwd_common`
- `df419f45` Add local Bonjour hostname resolution
- `cc96caa2` Palette: add accessibility labels to symbol-only browser buttons
- `208bff42` More attempts to improve stability
- `f09caa07` Palette: add accessibility labels to Workspace window controls
- `b7ab9f67` Sentinel: fix buffer overflow in unix socket binding
- `dbbaef0e` fix vnc and procfs runtime regressions
- `13887699` Bolt: optimize redundant strlen in tmpfs_getpath
- `384e1340` Palette: indicate selected state for workspace tabs and themes
- `7ec875d8` Sentinel: fix widespread buffer overflows by replacing sprintf with snprintf
- `a733d2a0` fix runtime regressions across apt network and vnc
