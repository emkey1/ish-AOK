# Release Notes Since `builds/iSH-AOK_543`

A shorter cycle than 543's systemd marathon (87 commits), but with two changes that touch everything: a JIT dispatch-loop fix that makes the emulator itself substantially faster on every guest architecture, and a host-native 2D graphics accelerator that takes the largest single cost out of the Wayland desktop. Alongside those, project CI is running again for the first time in about a year, and reviving it immediately paid for itself by exposing a silent memory-corruption bug that only ever manifested on x86_64 hosts. The rest of the cycle is the Wayland/Display applet growing into a real standalone mode, a filesystem fix for case- and Unicode-colliding file names on APFS, and the fixes behind issue #516.

## Highlights

- **The JIT stopped copying the whole CPU state out of its frame on every basic block.** Each frontend's dispatch loop did `*cpu = frame->cpu` after every `jit_enter` return, even though nothing between that copy and the next entry reads guest registers. On indirect-branch-heavy code, where the chain gadgets cannot link and the JIT returns to C at nearly every block, this was a startling share of total host time: a `sample` of a CPython 3.14 resolver-shaped benchmark under the arm64 guest put **~28% of host time in that one memcpy** (1755 of ~6000 samples). Syncing only when an interrupt is actually handed back gives, on -O2 CLI builds:
  - arm64 guest, CPython benchmark: 10.08s to 6.13s (**1.65x**; ~1.60x on device) (`9e8723b8`)
  - i386 guest, computed-goto bytecode VM: 1.23s to 0.62s (**1.99x**); in-guest `gcc -O2` compile 2.42s to 1.61s (**~1.58x**); fork+exec 1.26x (`68374669`)

  Removing the copy exposed why it looked load-bearing: it was also, by accident, the only thing clearing the sticky poke flag. Because `cpu->poked_ptr` points into `cpu->_poked` and the frame holds a whole-struct snapshot taken at entry, the copy wrote the snapshot's *stale* `_poked` over the live flag. A poke arriving mid-block (how a sibling thread forces a task out of the JIT for signal delivery or the memory-quiesce barrier) was therefore silently dropped, and a stale TRUE would re-arm the flag forever once the copy went away. `jit_frame_sync_out()` now preserves the live flag on every frontend (i386, amd64, arm64, riscv64), fixing the pre-existing poke loss as a correctness change in its own right (`8cf79974`). The amd64 and riscv64 loops still carry the per-block copy and are the obvious next win. Submitted upstream as PR #2767.
- **Host-native pixman accelerator for the Wayland desktop.** Phase 0 measurement found **~23.5% of interactive Wayland wall time** (labwc plus a redraw-heavy GTK app) inside raw pixman calls, well above the plan's GO threshold, unlike the parallel persistent-JIT-code-cache proposal, which measured 3-6% and was closed out as a no-go (`365cd259`, `72c66142`). What shipped:
  - `ISH_SYS_PIXOP` (syscall `0xacc1`), a paravirt accelerator running FILL/COPY/OVER for 32bpp `a8r8g8b8`/`x8r8g8b8` surfaces directly over the guest's own buffers, with new strided-2D direct-pointer primitives in `kernel/user.c` that keep the per-page-span resolve discipline and write-before-read COW ordering (`b2c97524`).
  - A guest-side `LD_PRELOAD` shim interposing pixman's public API, which falls through to real pixman the instant a call doesn't exactly match the accelerator's shape: mask, transform, repeat, filter, alpha-map, clip, wrong format, out of bounds, or accelerator unavailable (`c2f0d45a`).
  - v2 adds masked composite (`OVER_MASK_A8`, `db6c4d57`) and `x8r8g8b8` as a composite *destination* (`2f0c587b`); wired into Settings (`a693e2a4`).
  - **The iSH-private accelerator syscalls were never reachable from x86 guests, and probing for them was fatal** (`adb90999`). `ISH_SYS_AEAD` (`0xacc0`) and `ISH_SYS_PIXOP` sit above every real syscall range, so `handle_syscall_interrupt` has to intercept them before its table range check, but the intercept was gated on arm64/riscv64. On i386/amd64 both numbers fell through and got **SIGSYS, not ENOSYS**, which is what every guest-side consumer probes for. The pixman shim would therefore kill every process it was preloaded into on an x86 Wayland session, and the crypto provider died the same way. Both accelerators are ABI-neutral, so the intercept now runs for every ABI.
- **CI is alive again, and reviving it found a real memory-corruption bug.** Every recent run had "failed" at exactly 24h0m with an empty `runner_name`: jobs were queued on runner labels GitHub retired (`ubuntu-20.04`, `macos-11`) and cancelled at the queue limit without ever starting, on workflow config untouched upstream since 2021 (`09c8b826`). Modernized, timeout-capped, and gated on unit tests, then on the e2e suite (`4c789ba3`, `d91ca1de`, `44db7b25`). The x86_64-only e2e failures it surfaced took five rounds of CI-driven bisection to unwind:
  - **`jit/gadgets-x86_64`: the write-prep gadget checked cross-page *after* bailing to `handle_miss`** (`6365b0a5`). `handle_miss` returns a host pointer valid for exactly one guest page, and the gadget's inline store then ran its full width from it, so for a page-crossing address the tail bytes landed in whatever host memory followed. On Linux hosts, mirroring routes *every* guest write through `handle_miss`, so every page-crossing store silently corrupted adjacent guest memory. Seen as guest `gcc` dying with input-dependent but deterministic ICEs and python2 segfaulting, on x86_64 hosts only; aarch64 gadgets have always dispatched cross-page first.
  - **`emu/memory`: `host_page_prot` was seeded read-write instead of unknown** (`eca8a981`), so the first promotion of a read-only file-backed page to writable matched the stale cache, skipped the real `mprotect`, and ended in a spurious guest SIGSEGV. Linux-hosts-only.
  - **`sendfile()` lost data on a short write** (`22a605de`): `fd_copy_range()` advanced the input position past a full bounce-buffer chunk, so when a pipe accepted only part of it the remainder vanished and the next call saw a false EOF. busybox `cat`/`tar` use sendfile, so any guest pipe copy of a file over 64K truncated. The same commit made the e2e suite runnable on macOS at all.
- **fakefs now escapes host file names, fixing silent data loss on APFS.** Guest files were stored on the host under their raw guest names, and APFS is case-insensitive: guest paths differing only in ASCII case collided in one host directory. Observed as apk's ncurses-terminfo install losing `/usr/share/terminfo/{a,e,l,m,n,p,q,x}` to their uppercase twins, with extraction failing ENOENT, `mkdir` returning EEXIST for a directory `ls` didn't show, and case-twin files silently sharing content (`075ee627`). APFS also folds Unicode case and normalization, so Cyrillic "Ф"/"ф" and NFC/NFD "café" collided too; the escape format was extended to cover every byte >= 0x80, making the on-disk form pure fold- and normalization-inert ASCII (`c26be8ad`). Metadata keeps unescaped guest paths, so only the host filesystem boundary translates, and versioned migrations (v4, then v5) convert existing roots in place, resumably, with best-effort handling for roots that had already collided. `tools/fakefs.c` had a second, independent normalizer via libarchive's Apple UTF-8-MAC conversion, fixed in the same change.
- **Issue #516 (Nix on aarch64) was three separate bugs.**
  - `sys_read_buf` called `generic_getpath` on **every successful read**, purely to feed two trace modes (one env-gated, one compiled off). On fakefs that is a SQLite query per `read()`, with WAL commit and SHM fcntl locking, so bulk I/O ran at database speed while pinning a core. `nix-env -iA nixpkgs.hello` sat "hung" at 100% CPU indefinitely; gated behind the trace modes, it completes in **8.6 seconds**. The write side needs the path for real work (IN_MODIFY), so that is now gated on a cheap `inotify_has_instances()` check instead (`36eab9c6`). This likely accounts for a share of *every* fakefs-bound workload's slowness since the tracing landed in the 528 era.
  - `open(2)` on `/proc/pid/ns/*` now yields a namespace fd instead of resolving the `mnt:[4026531840]` identity token as a path. nix >= 2.30 treats the ENOENT as fatal and aborts every command with "error: saving parent mount namespace" (`8cb6bcbc`).
  - `kill(-pgid)` on an all-zombie process group is success, not EPERM, since Linux counts a zombie as successfully signaled. nix hits this on every channel unpack (`54ffed9c`).

## User-Facing Changes

### Wayland / Display

- **"Wayland Display" is now a real standalone startup mode** (`8a0d98a2`, `583093bf`): it boots straight into a fullscreen display as the scene root rather than a Workspace desktop with the applet opened, with a "Workspace" escape button (confirm dialog, tears the session down cleanly) so a broken guest Wayland stack cannot strand you away from Settings or a terminal. Scene-state restoration is ignored in both directions, so switching startup modes later cannot resurrect a stale scene shape. Plus a standalone menu pip and fullscreen display surface (`b27ae9ec`) and "Maximize Screen Space" (`03f5e8ca`).
- **Per-orientation compositor resolution** (`d60437ca`): instead of stretching a landscape canvas into a portrait viewport, the compositor output is actually resized via RFB `SetDesktopSize` (1280x720 and 720x1280, standalone mode). Advertising `DesktopSize`/`ExtendedDesktopSize` also fixes a real pre-existing fragility: *any* server-side resize under a live connection used to produce rects beyond the old framebuffer bounds and kill the session.
- **The accessory key strip** got its own strip for the Wayland surface (`29df4bed`) and then a long correctness pass: hardware-keyboard state synced off `GCKeyboard` connect/disconnect rather than soft-keyboard frame changes (`7b3aa522`), no force-hiding on iPad against the user's setting (`35e32cb1`), a real layout pass instead of re-dirtying every pass (`d909a791`), no reload on no-op notifications (`07cfcceb`), smart punctuation and the predictive-text strip suppressed in the RFB path (`9a311cd6`, `f1090ac2`), and the menu pip kept clear of the bar including after rotation (`71dceaaf`, `7f8604ac`). The final shape: **self-host the strip only while a hardware keyboard is attached** (`6e000aef`). Self-hosting existed to work around UIKit bumping a docked accessory view up to clear the home indicator and never lowering it, which only matters with no software keyboard to dock above; using it unconditionally meant the software keyboard never appeared at all. Plus a full resign/reclaim of first responder on mode switch (`7713e718`).
- **"Wayland session ended" with no explanation, on device, deterministically** turned out to be an unwritable `/tmp` (`4ba4dd50`). The rootfs's `/tmp` lacked world-write, so the `su`-launched default-user session got EACCES on every file the startup script touches. Fatal in two stacked ways: `spawn_logged` pipes every process through `tee -a $DEBUG_LOG`, so an unopenable log leaves the compositor's fifo readerless and labwc dies on SIGPIPE at first output; and on dash roots `:` is a POSIX *special builtin*, so its redirection error exits the whole script on the spot, silently. The applet now surfaces the real startup-failure reason (`6a483ee1`) and `die()` reasons persist past `/tmp` wipes (`aad11b0b`).
- **labwc's event loop spinning at ~3000 cycles/s** was a timerfd conformance gap: Linux resets a timerfd's expiration count on *every* `timerfd_settime`, so a disarm also clears poll/epoll readiness, but iSH only cleared it in `read()`. libwayland multiplexes all `wl_event_loop` timers onto one timerfd and disarms it without reading, so an unread expiration left the fd permanently readable: 123k `epoll_pwait` + `timerfd_settime` pairs in 40s, pinning a guest CPU (`4c1b74df`).
- `start-wayland.sh`/`setup-wayland.sh`: foot retried on the same startup race wayvnc already retries (`02cfc17f`, `6fc49f06`), windows no longer force-maximized on open (`c862512d`), Qt6 runtime installed for what wayvnc pulls in (`ad6612ad`).

### Kernel and filesystem

- **Signals:** `SA_RESTART` restart decisions now also scan `sighand->queue`, not just the thread queue. Process/group-directed signals (SIGCHLD on child exit, `kill(-pgid, …)`, `kill(-1, …)`) live there and were invisible to the check, turning what should be a transparent restart into a real EINTR surfacing into the guest (`5c781d16`). The `foot` EINTR crash needed a second fix: `realfs_wait_readable`/`writable`'s `sigunwind_start()` branch declared EINTR on any host SIGUSR1 poke without rechecking whether the guest actually has an observable signal, unlike the two correct EINTR sites a few lines below in the same loop (`a5eeca28`).
- **JIT fault handling:** unmapped guest-execution faults (the stale-TLB race) are now retried on the i386 (`55afb69d`), amd64 and riscv64 (`27c4afa1`) frontends, not just arm64. The x86 frontends were assumed covered by the INT_GPF retry dance, but that requires a nonzero `segfault_addr` and the unmapped-fault path hands it exactly 0, so an i386 guest hit by the race got a spurious SIGSEGV at address 0.
- **Sockets:** real multicast group membership (`IP`/`IPV6_ADD_MEMBERSHIP`, `915386a6`), previously an unconditional no-op, so nothing that needs to *receive* multicast (avahi/mDNS, SSDP/UPnP discovery) ever worked; Linux's 1-byte form of `IP_MULTICAST_TTL`/`IP_MULTICAST_LOOP` accepted (`e44e7473`); `setsockopt(IPV6_RECVERR)` no longer fails when the helper socket cannot open (`d8ba2eeb`).
- `fs/aok.c`: unsafe `strcpy` calls replaced (`0d5f1741`).

### App UI

- **Files integration:** `/AOK/persist`, the one directory shared by every booted root, is now browsable as a "Persist" folder alongside the installed roots (`1c26ae32`); `loadToURL:`/`saveFromURL:` guarded against a nil backing URL (`6baa7cc4`).
- **Workspace windows pop back after a transient surface shrink instead of staying clamped** (`72a0c328`). The clamp into `desktopUsableBounds` was one-way, so a window shrunk to fit a smaller surface never grew back. Two casualties on iPad: Cmd-Tabbing out makes iOS run snapshot layout passes at the *other* orientation's size, clamping a full-width landscape window to portrait width off-screen (on a 13" iPad, 1024/1366, exactly the reported "about 75% of screen width"); and landscape to portrait to landscape did the same in plain sight to the Desktops and Launcher windows.
- Re-importing an exported filesystem no longer gives a misleading name error (`b8950201`); official roots are preferred over experimental ones as the fallback default (`c2099e05`); CLI guests get a default `PATH`/`HOME`/`TERM` (`b419cbe2`); `NSBonjourServices` declared alongside the local-network usage string (`dba3448a`).
- LLM chat: configurable tool-loop round cap (`082cf8f2`, previously hardcoded at 6); bubble-width cap dropped below required priority (`2cc4d08f`).
- `ktop` ships prebuilt aarch64 binaries and auto-rebuilds on change (`e7a8b10b`).
- Accessibility hint for the terminal switcher long-press (`48c66856`).

### Performance (smaller items)

- `fakefs_iterate` and `fakefs_readdir` string operations optimized (`aceeb42e`, `1f5f1878`); last-slash search in path handling optimized (`46bf5fb4`).

## Validation

Full guest regression suite, run concurrently on all four guest architectures on the CLI harness at tag time:

| Guest | Result |
|-------|--------|
| i386 | 93 PASS / 0 FAIL |
| amd64 | 94 PASS / 0 FAIL |
| arm64 | 96 PASS / 0 FAIL |
| riscv64 | 89 PASS / 0 FAIL |

Each of the four skips `pixman_accel`, which needs the accelerator explicitly enabled. Run separately with `ISH_PIX_ACCEL=1` it PASSes on **i386, amd64 and arm64**, each a differential against a real `dlopen`'d libpixman. The i386 and amd64 results are the ones that matter for `adb90999`: they are the two ABIs whose accelerator intercept was missing, and i386 is the case its commit message calls out specifically (the request struct's leading u32 pair has to land at the same offsets under i386's 4-byte `long long` alignment as under any 64-bit ABI). The riscv64 test rootfs has no libpixman installed, so it skips there for environmental reasons rather than emulator ones.

On device (iPad, Arch aarch64, build 2026-07-25 11:45, which postdates every emulator commit in this cycle), two suites run concurrently against the one emulator process, which is the load pattern that surfaced blockers in the 543 sweep:

| Device run | Result |
|------------|--------|
| Native Arch aarch64, as uid 1000 | 91 PASS / 2 FAIL (both root-only, see below) |
| Devuan6-arm64 chroot, concurrent with the above | 97 PASS / 0 FAIL |
| Native Arch aarch64, re-run as root, alone | **96 PASS / 0 FAIL** |

The two failures in the uid-1000 run are privilege artifacts, not emulator bugs: `netlink_audit` (AUDIT_GET needs CAP_AUDIT_CONTROL) and `oom_score_adj` (a negative adjustment needs CAP_SYS_RESOURCE). The clean root re-run confirms it. Both now emit the same SKIP marker their siblings already used (`ambient_caps: SKIP (needs root)`, `chroot_getcwd: SKIP (not privileged: euid=1000)`) when the privilege is absent, so an unprivileged device run no longer reports two failures a maintainer has to re-verify by hand (`a3a56540`); they still run for real as root, verified on the arm64 guest. Running the device suite as root (`sudo sh /AOK/tests/setup-regressions.sh --run`) remains the recommended form.

Cross-architecture device coverage was not possible this cycle: the non-aarch64 roots under `/AOK/roots` (`Alpine3.23.3`, `ArchLinux-x86_64`, `Alpine3.23.3-riscv64`, `Devuan6-riscv64`) are empty stubs rather than installed roots, so only the aarch64 roots can be chrooted into. The four-architecture coverage above therefore comes from the CLI harness. Reinstalling those roots would restore true cross-arch device testing.

CI is green on the release candidate (run 30169732364): `build-linux (clang)`, `build-linux (gcc)`, and `build-mac` all pass, which covers the `float80` and `riscv64_decode` unit tests and the e2e suite on x86_64. Note that `float80_test` cannot pass on an Apple Silicon host and is not expected to: it unions `float80` with `long double` and uses `long double` arithmetic as its oracle, but arm64 macOS has a 64-bit `long double` (`LDBL_MANT_DIG` 53). It is meaningful only on the x86_64 CI leg, which is where CI runs it.

## Known Issues

- **One unexplained whole-emulator wedge under heavy concurrent load, not root-caused.** Observed once on device during this cycle's release testing, with a live Wayland session running while two full regression suites (the native one plus a Devuan6-arm64 chroot one) ran concurrently against the same emulator process. sshd stopped answering, an established ssh session dropped, and the Wayland desktop locked; the app had to be restarted by hand. No crash log, so it was a hang rather than a crash. Both suites had already completed and exited 0 before it happened, so guest execution and signal delivery were working for a long stretch under that same load. Not reproduced since, and a subsequent full device suite run as root is clean (96 PASS / 0 FAIL). Two candidate explanations, both consistent with the symptom and distinguishable only from a backtrace of the wedged process:
  - This cycle's JIT frame-sync changes. The device runs the aarch64 guest frontend that `9e8723b8` changed, a poke is how a sibling thread forces a task out of the JIT for signal delivery and the memory-quiesce barrier, and `8cf79974` exists precisely because a mishandled poke flag can livelock a task. A remaining narrow race there would stall signal delivery across many tasks, which is what was seen.
  - A poll/sighand lock-order deadlock, a recurring class in this codebase. `signalfd_epoll_deadlock` was originally found from an on-device backtrace taken during a labwc Wayland session under load, which is close to this scenario.

  If it recurs, attach lldb from Xcode and capture `thread backtrace all` before restarting: tasks parked in the JIT with a poke pending indicates the first, a cycle across `sighand->lock` and `poll->lock` the second. The load involved was deliberately adversarial and heavier than normal use.
- Buildroot `make` crashes iSH-AOK at "checking for working sigaltstack" (issue #521), new this cycle, not yet root-caused.
- Some Qt applications (Falkon) cannot connect to the session bus (issue #485).
- Node.js is slow on aarch64 (issue #509).
- gdb `next`/`step` immediately after a breakpoint hit on amd64/arm64 can still misbehave (issue #503, carried forward).
- riscv64 `sudo` "no gadget 00000000" (PC jump to zero) remains unreproduced (carried forward).
- The amd64 and riscv64 JIT dispatch loops still copy the whole `cpu_state` out of the frame per block; the arm64/i386 hoist has not yet been ported to them.
- systemd as PID 1 remains supported-but-experimental on the Arch roots; Alpine (OpenRC) and Devuan (sysvinit) roots use their own inits.

## Maintainer Notes

- **The accelerator syscalls have three wiring points, not one.** `adb90999` is the cautionary tale: an iSH-private syscall number above the real ranges needs its intercept in `handle_syscall_interrupt` for *every* ABI, or the guest gets SIGSYS. And SIGSYS is not ENOSYS: any guest-side paravirt probe must survive a SIGSYS death, not just an error return, or the probe itself becomes fatal.
- Pixman accelerator differential testing lives in `tests/manual/pixman_accel.c`, validated against a real `dlopen`'d libpixman (FILL/COPY/OVER/OVER_MASK_A8, offset and padded and multi-page geometries, both source and destination formats, ~90 random-fuzz rounds). `e536ad04` made the probe SIGSYS-safe and flagged tests that report no PASS/FAIL marker at all, since a test that dies without a marker used to read as absence rather than failure.
- fakefs on-disk names are now escaped (`fs/fake-path.h`). Anything reaching around the fakefs layer to touch host files directly must escape first. Non-ASCII names triple in length, so names over roughly 85 CJK characters now fail with a clean `ENAMETOOLONG` rather than silently colliding.
- CI runs on `ubuntu-24.04`/`macos-15` with a 90-minute timeout and a concurrency group, and now runs on pull requests against *every* target branch. PRs against `working` (i.e. all of them) previously got no checks at all.
- `ISH_JIT_TIMING` (`365cd259`) is the Phase 0 measurement harness built for the code-cache proposal; the no-go result and its reasoning are recorded in the plan doc (`72c66142`). Plans for both proposals are in `docs/` (`07ca9d1f`).
- The default project-level `CURRENT_PROJECT_VERSION` in `app/Project.xcconfig` had been stale at 529 since before the 534 bump. Debug builds inherit it (only the Release/DebugLinux/ReleaseLinux configs set it in the pbxproj), so Debug builds on device were reporting a five-release-old build number. Now synced to 544 along with the eight pbxproj sites.

## Commit Range

```
38a47217 gitignore: ignore the Xcode plugin-cache dirs that land in the project root
1c26ae32 app/FileProvider: expose /AOK/persist as a "Persist" folder in Files
68374669 jit/i386: stop syncing the whole cpu_state out of the frame per block
e536ad04 tests: SIGSYS-safe pixman probe, and flag a test that reports no marker
adb90999 kernel: wire the iSH-private accelerator syscalls for the x86 guest ABIs
8cf79974 jit: never write a stale poke flag over the live one on frame sync-out
9e8723b8 jit/arm64: stop syncing the whole cpu_state out of the frame per block
ff57b0a6 app/Display: remove the dead "Show Keyboard" accessory key
7713e718 app/Display: fully resign/reclaim first responder on accessory mode switch
6e000aef app/Display: only self-host the accessory strip while a hardware keyboard is attached
a036550b app/Display: add a manual "Show Keyboard" key to the accessory strip
785c89c3 app/Display: fix touch-swallowing regression from the self-hosted accessory strip; hardware-aware auto-show
f4aaab2a app/Display: expose Auto-Show Keyboard in the Wayland pip menu
62114a3f app/Display: host the accessory strip ourselves; stop fighting UIKit's keyboard host
c862512d opt/AOK/tools/start-wayland.sh: stop forcing every window to maximize on open
d60437ca app/Display: per-orientation compositor resolution via RFB SetDesktopSize
e39bdeba docs: scope plan for per-orientation Wayland output resize
07cfcceb app/Display: don't reload the accessory bar on every no-op GCKeyboard notification
03f5e8ca app/Display: wire up "Maximize Screen Space" for standalone Wayland mode
a5eeca28 fs/real: realfs_wait_readable/writable's sigunwind_start() EINTR must recheck signal_pending
7f8604ac app/Display: fix menu pip staying covered by the accessory bar after rotation
6fc49f06 opt/AOK/tools/start-wayland.sh: maximize windows on open; widen foot's retry budget
ad6612ad opt/AOK/tools/setup-wayland.sh: install Qt6 runtime for the qv4l2/qvidcap wayvnc pulls in
5c781d16 kernel/signal: check sighand->queue too when deciding SA_RESTART restart
02cfc17f opt/AOK/tools/start-wayland.sh: retry foot on the same startup race wayvnc already retries
f1090ac2 app/Display: suppress the predictive-text strip in DisplayRFBView's accessory bar
915386a6 fs/sock: implement real multicast group membership (IP/IPV6_ADD_MEMBERSHIP)
46bf5fb4 ⚡ Bolt: Optimize finding the last slash in path (#522)
dba3448a app: declare NSBonjourServices alongside the existing local-network usage string
71dceaaf app/Wayland display: keep the menu pip clear of the accessory keyboard bar
2cc4d08f app/LLM chat: drop the bubble-width cap below required priority
2f0c587b kernel/pixman: support x8r8g8b8 as the composite destination format
a693e2a4 app: wire the pixman accelerator into Settings
4ba4dd50 wayland: fix the default-user session dying of an unwritable /tmp
082cf8f2 LLM chat: make the tool-loop round cap configurable
a44850f4 docs: record OVER_MASK_A8 completion + surface the x8r8g8b8-dst gap
db6c4d57 pixman accelerator v2: OVER_MASK_A8 (masked composite) support
f7b25959 docs: record pixman accelerator Phase 2 completion + v2 roadmap
c2f0d45a pixman accelerator Phase 2: guest-side LD_PRELOAD delivery shim
a1e19c07 docs: record pixman accelerator Phase 0+1 completion in the plan
b2c97524 kernel: pixman accelerator Phase 1 -- ISH_SYS_PIXOP (0xacc1)
aad11b0b start-wayland.sh: persist die() reasons past /tmp wipes
72c66142 docs: record Phase 0 NO-GO result in the JIT code-cache plan
365cd259 jit: add ISH_JIT_TIMING, Phase 0 measurement for the code-cache proposal
6a483ee1 app/Display: surface the real startup-failure reason, not bare "Wayland session ended"
29df4bed app/Display: give the Wayland surface its own accessory key strip
07ca9d1f docs: implementation plans for the persistent JIT code cache and pixman accelerator
b27ae9ec app/Display: standalone menu pip + fullscreen display surface
9a311cd6 app/Display: disable iOS smart punctuation in the RFB keyboard path
e44e7473 sock: accept Linux's 1-byte form of IP_MULTICAST_TTL / IP_MULTICAST_LOOP
4c1b74df kernel: timerfd_settime resets the expiration counter (clears readiness)
583093bf app: make the Wayland Display startup mode standalone, no Workspace shell
72a0c328 workspace: windows pop back after a transient surface shrink instead of staying clamped
d909a791 app: run the accessory bar's real layout pass; stop re-dirtying it every pass
8a0d98a2 app: add Wayland Display as a startup mode
35e32cb1 app: stop force-hiding the accessory bar on iPad regardless of the user's setting
7b3aa522 app: sync hasExternalKeyboard off GCKeyboard connect/disconnect, not just soft-keyboard frame changes
6baa7cc4 app/FileProvider: guard loadToURL:/saveFromURL: against a nil backing URL
44db7b25 ci: gate merges on the e2e suite; retire the debugging scaffolds
6365b0a5 jit/gadgets-x86_64: check cross-page BEFORE bailing to handle_miss
eca8a981 emu/memory: seed host_page_prot as unknown, not read-write
9852fa96 emu/memory: add mirror-bisect debug knobs; ci: 2x2 the cc1 ICE
f7c4747e ci: bisect the cc1 ICE against host-page mirroring
27c4afa1 jit: retry unmapped guest-execution faults on the amd64 and riscv64 frontends
41e2fca8 ci: e2e-debug round 4 -- ptraceomatic on the small cc1 ICE repro
477cdbb3 ci: e2e-debug round 3 -- prebuilt CPU-conformance probe + ptraceomatic
55afb69d jit: retry unmapped guest-execution faults on the i386 frontend too
d8ba2eeb fs/sock: don't fail setsockopt(IPV6_RECVERR) when the helper socket can't open
59f35c23 ci: add e2e-debug workflow for the x86_64-only guest failures
d91ca1de ci: gate merges on the e2e suite; add sendfile-to-pipe regression test
22a605de e2e: make the suite pass locally on macOS; fix sendfile data loss
c26be8ad fs/fake: escape non-ASCII bytes too, fixing Unicode case/normalization collisions
4c789ba3 ci: drop the dead kernel=linux legs; gate on unit tests, e2e informational
075ee627 fs/fake: escape host file names so guest case-twins stop colliding on APFS
e7a8b10b opt/AOK/tools/ktop: ship prebuilt aarch64 binaries, auto-rebuild on change
09c8b826 ci: revive the CI workflow after a year of 24-hour queue deaths
1f5f1878 I've gone ahead and optimized the string comparisons in `fakefs_readdir` for you. (#518)
aceeb42e ⚡ Bolt: [performance improvement] Optimize string operations in fakefs_iterate (#515)
48c66856 🎨 Palette: Add accessibilityHint for terminal switcher long-press (#508)
0d5f1741 🛡️ Sentinel: Replace unsafe strcpy calls with strncpy in fs/aok.c (#517)
b419cbe2 main: give CLI guests a default PATH/HOME/TERM environment
c2099e05 app: prefer official roots over experimental ones as fallback default
36eab9c6 kernel/fs: stop paying a fakefs SQLite lookup on every read and write
54ffed9c kernel: kill(-pgid) on an all-zombie process group is success, not EPERM
8cb6bcbc fs/proc: open(2) on /proc/pid/ns/* magic links yields a namespace fd
b8950201 app: fix misleading name error when re-importing an exported filesystem
9813acf6 docs/app: update fork docs and links for iSH-AOK
```
