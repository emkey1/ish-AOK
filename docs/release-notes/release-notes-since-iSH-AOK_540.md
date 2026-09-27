# Release Notes Since `builds/iSH-AOK_540`

This cycle is dominated by three hard-won correctness fixes across the JIT/CPU emulation layer — an arm64 stale-JIT-translation bug that crashed `npm`/`node` roughly half the time, an i386 spurious-SIGSEGV race on COW-raced atomics, and an amd64 `POP r/m64` bug that corrupted `sigsetjmp`'s return address — plus a redesign of the arm64/riscv64 `brk` headroom mechanism that fixed a real heap-expand failure and then, in fixing it, uncovered and fixed a ~65x fork/boot slowdown its own first attempt had introduced. It also includes an end-to-end root-cause investigation into a "gdb is totally unusable" report, which turned up five distinct arch-specific bugs across arm64 and amd64 (none of them about filesystem type, the original report's guess). Also in this range: two separate app-wide-crash hardening fixes (a malformed `mount -t fake` call from any guest process with root could previously take down the entire app), a real inotify use-after-free found chasing an elusive `stress-ng --sockabuse` wedge, a whole-filesystem deadlock in `fs/fake` that was intermittently breaking the Wayland applet's connection, a `/proc/stat` counter bug that crashed `ktop` under fork churn, riscv64 `ptrace`/`strace` support, `NETLINK_AUDIT` implementation, a long-stalled futex `SA_RESTART` lost-wake fix finally merged, a batch of socket/epoll conformance fixes, a real Dependabot security fix, new experimental Arch Linux downloadable roots (which surfaced and fixed a real termios2 ioctl gap affecting any modern-glibc guest, and were later repaired after a pruning pass accidentally broke `/sbin/init` on both images), a `/proc/sys/kernel/random` gap that spammed every new Arch shell with an error, and several Workspace/Wayland reliability and keyboard-input fixes.

## Highlights

- **Root-caused and fixed "gdb debugging is totally unusable" end-to-end on arm64, and substantially on amd64.** The investigation turned up five distinct bugs, none of them about filesystem type (fakefs vs realfs, the original report's guess) — all guest-architecture-specific:
  - `P_EXEC` was never set on any ELF-loaded segment (`kernel/exec.c` checked the ELF program header's writable bit but never its executable bit), so every process's own code showed as non-executable in `/proc/pid/maps`. Harmless to actual execution (the fault path doesn't enforce it) but misleading to any tool, like gdb, that reads the reported permissions.
  - The real root cause of "breakpoints in a PIE binary silently never trigger": `/proc/<pid>/mem` truncated the (genuinely 64-bit) file offset to a 32-bit type before translating it. gdb prefers writing breakpoints through `/proc/pid/mem`; since PIE binaries load at high (>4GB) addresses, every breakpoint write landed at the wrong (truncated) address — but read back "correctly" from that same wrong address, so gdb never saw an error. The debuggee just silently ran to completion every time. Non-PIE binaries happened to link low enough to dodge this by pure coincidence, which is exactly why the bug looked like an all-or-nothing "gdb is broken" problem rather than a narrow one.
  - Once that was fixed, breakpoints correctly trapped — but a debugger's *own* internal "restore the original instruction and step over the breakpoint" sequence (which every debugger does, even just to cleanly report the very first hit) silently ran the entire rest of the program instead of one instruction. Root cause: arm64's JIT dispatch never checked the single-step flag at all — amd64's JIT gets single-step almost for free by falling back to its plain interpreter, but arm64 has no interpreter fallback (JIT-gadget-only), so nobody had implemented single-step there. New `cpu_single_step_arm64` compiles and executes exactly one guest instruction as a private, throwaway JIT block.
  - Separately on amd64: `int3` (`0xcc`, the actual x86 software breakpoint instruction, and what gdb's own startup self-test uses to probe the host) was entirely unimplemented in the amd64 interpreter, raising `SIGILL` instead of `SIGTRAP` — this alone broke every amd64 gdb breakpoint, not just the self-test. Fixed alongside a related gap where reading the (nonexistent, since iSH has no hardware breakpoint support) x86 debug registers returned an I/O error instead of a truthful zero.
  - Verified end-to-end against real gdb on arm64: `break`/`run`/`print`/`next`/`step`/`continue` on a PIE binary now produces output identical to real Linux, plus 600+ consecutive single-instruction steps through musl's dynamic linker startup with zero crashes. On amd64, breakpoints now correctly trap with proper arguments and backtraces; a further, narrower "`next`/`step` right after a breakpoint hit" issue was found but not root-caused, and is tracked separately (issue #503) rather than left silently unmentioned.
- **arm64: root-caused and fixed the `npm`/`node` crash that hit roughly half of all installs.** The guest was executing a **stale JIT translation** of a recycled code address — the old function's compiled code running against the new function's stack frames — via three compounding gaps: `IC IVAU` (the guest's own self-modifying-code cache-invalidation instruction) was wrongly treated as a no-op; invalidating a block never bumped the generation counter other threads' dispatch caches check, so they kept running jetsam'd code until an unrelated OOM flush; and `LDAR`/`STLR` (load-acquire/store-release) lowered to plain loads/stores with no host memory-ordering barrier, letting V8's own concurrent-compiler/GC object handoff be observed out of order on this weakly-ordered host. riscv64 got the equivalent fix for `FENCE`/`FENCE.I`. Verified 20/20 clean `npm install -g @anthropic-ai/claude-code` runs on a baseline that crashed ~50% of the time before.
- **i386: fixed a spurious SIGSEGV on LOCK-prefixed atomics under concurrent `fork()`.** After any thread forks, the whole process's shared pages become copy-on-write; the GPF fault-restart handler re-checked "does this address need a page fault?" with a lock-free read even when the faulting gadget had already recorded the exact address — a sibling thread breaking the COW in that window made the check conclude no fault was needed, delivering a real SIGSEGV instead of just restarting the (idempotent) instruction. Fixed by routing straight to the authoritative locked resolver whenever the gadget already knows the fault address.
- **amd64: fixed `POP r/m64` corrupting `sigsetjmp`'s saved return address.** The instruction pops the stack (committing the SP advance) and then writes the popped value to memory; if that destination write needed a page fault-in, the code rewound the instruction pointer to retry but never rewound the already-advanced stack pointer, so the retry popped from the wrong slot. musl's `sigsetjmp` uses exactly this instruction, and the resulting garbage return address is what produced a deterministic `signal_child_burst` crash (issue #487).
- **arm64/riscv64 `brk` headroom: fixed a real heap-expand failure, then fixed the fix.** A dynamic-PIE process's 1 GiB of intended heap headroom was never actually reserved in the page table, only implied by address placement — a later `mmap()` landing in that gap could shrink `brk()`'s real ceiling well short of 1 GiB (`nix`'s "Failed to expand heap" on riscv64, issue #480). The first fix reserved the headroom as real placeholder page-table entries, which fixed the heap-expand case but made every `fork()` of a dynamic-PIE process (i.e. nearly every shell command on these guests) walk and copy-on-write all 262,144 of them — a ~65x fork/boot slowdown, reported as "booting is SUPER slow." Redesigned as a lightweight address-range reservation on `struct mem` instead: O(1) to reserve, nothing for `fork()` to copy, still fully backing the real heap-expand case.
- **Two guest-triggerable whole-app crashes fixed.** `mount -t fake <source> <target>` with a source path that doesn't match this project's own root-installation conventions (wrong basename, or a corrupt/malformed metadata database) previously hit an `assert()`/`die()` that aborted the entire host process — killing every running guest task in the session — reachable by any guest process with root via a raw `mount(2)` syscall. Both now fail the one `mount()` call with an error instead.
- **A real use-after-free fixed in the inotify fd-lifecycle path**, found chasing (but not confirmed as the cause of) the residual `stress-ng --sockabuse` wedge tracked in issue #467. A concurrent `close()` of an inotify fd could win the race to free it while a snapshot walk still held a raw pointer to it, "resurrecting" a dying object; reproduced a heap-corruption crash within 15-90s of `stress-ng --inotify` combined with `--chmod`/`--chown`, fixed with a CAS-based retain that fails instead of resurrecting.
- **A whole-filesystem deadlock fixed in `fs/fake`** that was intermittently breaking the Wayland applet's connection (and any other FIFO-heavy workload): opening an *existing* FIFO without `O_NONBLOCK` blocks in the host `openat()` until a peer opens the other end, but a recent TOCTOU fix had made that same open path hold the fakefs database's write-transaction lock across the call — so a `cmd > fifo` writer that reached the open first blocked the *entire emulator* while holding that lock, wedging every fakefs operation in the process behind it. Fixed by skipping the transaction for an open-of-an-existing-FIFO, which creates nothing and so needs no atomicity with the metadata write.
- **`ktop` crash under fork churn fixed** (and hardened independently): `/proc/stat`'s per-CPU counters could tick backward — a task exiting mid-read could be double-counted, and a freshly-forked child briefly inherited its parent's accumulated CPU time before its own thread existed — and `ktop`'s meter math didn't expect that, signed-overflowing its way off the top of the guest stack.
- **riscv64 gets real `ptrace`/`strace` support** (issue #464): tracees previously fell through to an i386-era register layout that doesn't exist on either real architecture, so `strace -f` decoded every syscall as syscall 0 and then crashed its own tracer.
- **`NETLINK_AUDIT` implemented** (issue #468): `socket(AF_NETLINK, SOCK_RAW, NETLINK_AUDIT)` previously failed outright, which surfaced to users as "Failed to connect to audit daemon: Protocol not supported" from PAM, `login`, `su`, `useradd`, and similar tools that probe it via `libaudit`. Now implemented to mirror a kernel built with auditing disabled: status queries and user messages are accepted and ACKed, nothing is actually recorded.
- **A long-stalled futex fix finally merged**: `FUTEX_WAKE` landing in the narrow window while a `FUTEX_WAIT` is dequeued for an `SA_RESTART` signal restart was silently lost, making the restarted wait block for its full timeout instead of returning immediately. Fixes issue #466.
- **A real Dependabot security fix**: bumped `fastlane` to 2.237.0, which resolves `excon` to 1.6.0, fixing a moderate-severity header-redaction gap in redirect handling (GHSA-48rx-c7pg-q66r / CVE-2026-54171). `fastlane` 2.235.0 directly pinned an `excon` ceiling that blocked any fix; 2.237.0 relaxed it, so bumping fastlane alone was sufficient.
- **New experimental Arch Linux downloadable roots** (x86_64 and ARM64/aarch64) surfaced a real, arch-independent bug: `login` on a modern glibc/util-linux build checks `isatty()` via the newer `TCGETS2` termios2 ioctl, which iSH-AOK had never implemented (only the legacy `TCGETS`) — every login attempt failed immediately with "FATAL: bad tty" before ever reaching PAM. Fixed by implementing the `TCGETS2`/`TCSETS2`/`TCSETSW2`/`TCSETSF2` family; this affects any modern-glibc guest on any architecture, not just Arch. Both minirootfs images were later found quietly broken by a prior pruning pass that removed `/usr/lib/systemd` (and, with it, the real binary `/sbin/init` symlinked into it, since iSH doesn't run systemd as PID 1) — surfacing live as an empty Workspace Terminal launcher list and various `ttyname()`-dependent tool failures; both images re-extracted and re-uploaded with `usr/lib/systemd` restored.
- **A batch of socket/epoll conformance fixes**, mostly surfaced chasing the #467 investigation above: `fcntl(F_SETOWN/F_GETOWN)` was entirely unimplemented (EINVAL instead of success); `epoll_ctl` had no cyclic-nesting check (silently allowed what Linux rejects with `ELOOP`) and no `EPOLLEXCLUSIVE` validation; `getpeername`/`getsockname` with a small or zero-length buffer (both valid per POSIX) read uninitialized stack garbage on the generic path and could underflow-overflow a stack buffer on the AF_UNIX path — a real, reproducible crash, not just a wrong-errno bug; and socket syscalls on a non-socket fd returned `EBADF` instead of `ENOTSOCK`. `syncfs(2)` is also now implemented (community-contributed, issue #488).

## User-Facing Changes

### Debugging (gdb)

- `kernel/exec.c`: `P_EXEC` now set on executable ELF segments, so `/proc/pid/maps` correctly reports `r-xp` instead of `r--p` for code (`d9701556`).
- `fs/proc/pid.c`: fixed `/proc/<pid>/mem` truncating 64-bit addresses to 32 bits — the actual root cause of PIE-binary breakpoints silently never triggering (`5ac55d6b`).
- `jit/jit.c`: implemented arm64 `PTRACE_SINGLESTEP` (new `cpu_single_step_arm64`), previously a complete no-op that behaved like `PTRACE_CONT`. Fixes issue #501 (`63006ee7`).
- `emu/amd64_interp.c`: implemented `int3` (`0xcc`), previously entirely missing and raising `SIGILL` instead of `SIGTRAP` — broke every amd64 gdb breakpoint (`46f64a02`).
- `kernel/ptrace.c`: `PTRACE_PEEKUSER` on amd64's debug-register range now returns 0 instead of `EIO`, matching "no hardware breakpoints ever armed" instead of erroring (`83c53bd6`).

### JIT / CPU emulation

- arm64/riscv64 stale-JIT-translation fixes for the `npm`/`node` crash (see Highlights): `IC IVAU`/`FENCE.I` now actually invalidate, block invalidation now propagates to every thread's dispatch cache, `LDAR`/`STLR` get real host memory barriers (`5e06ab0e`).
- i386 spurious-SIGSEGV-on-COW-race fix, issue #477 (`270710cf`).
- amd64 `POP r/m64` RSP-rollback fix, issue #487 (`d0635475`).
- `kernel/signal`: blocked synchronous fault signals (SIGSEGV/SIGILL/SIGBUS/SIGFPE/SIGTRAP/SIGSYS) with a custom handler installed are now force-defaulted and unblocked before delivery, matching Linux's `force_sig` — previously such a fault re-executed forever at 100% CPU, unkillable from inside the guest. Found chasing a V8-abort-path wedge on arm64 (`e5482332`).
- `jit_invalidate_range`'s page-block-list walk is now bounded against list corruption (containment + diagnostic, not a root-cause fix for whatever can corrupt the list under heavy arm64 JIT churn) (`8a41d04a`).
- New arm64-only crash diagnostics: name the file/region backing a faulting PC, a store watchpoint, and a traced-instruction probe, all env-gated with no cost when unused; an `ISH_ARM64_NO_FUSE` knob to bisect a suspected miscompile against the JIT's lookahead-fusion passes (`4e1aaeef`, `adc2348f`, `4a71f7df`).

### Memory / brk

- arm64/riscv64 `brk` headroom redesign fixing both the original heap-expand bug (issue #480) and the ~65x fork/boot slowdown its first fix introduced (see Highlights) (`fded0817`, `b33b3b33`).

### Filesystem / procfs

- `fs/fake`: fixed the FIFO-open-vs-db-lock whole-process deadlock (see Highlights) (`06582ffe`).
- Two guest-triggerable whole-app crashes hardened into ordinary syscall errors: a malformed `mount -t fake` source (`f2021a9b`) and a corrupt/invalid fakefs metadata database (`92afca56`).
- `/proc/<pid>/mounts` added (previously only `mountinfo` existed at the per-pid level) — needed by any distro whose `/etc/mtab` symlinks to `../proc/self/mounts`, including the new Arch Linux roots (`8d91ec6b`).
- `tty`: implemented the `TCGETS2`/`TCSETS2`/`TCSETSW2`/`TCSETSF2` termios2 ioctl family (see Highlights) (`7310f6a2`).
- `/proc/sys/kernel/random/{uuid,boot_id}` implemented — `bash` >= 5.1 and various distro rc scripts read these unconditionally for `$RANDOM` seeding; missing entirely before, spamming every new Arch shell with a "No such file or directory" error on stderr (`26738bd2`).

### Networking

- `NETLINK_AUDIT` implemented, issue #468 (`a8b848c3`).
- Socket/epoll conformance batch (see Highlights): `F_SETOWN`/`F_GETOWN` (`1ee4a515`), epoll cyclic-nesting `ELOOP` (`d890dab7`), `EPOLLEXCLUSIVE` validation (`2073b263`), `getpeername`/`getsockname` small-buffer fix — a real crash, not just a wrong errno (`35545064`), `EBADF`/`ENOTSOCK` distinction on socket syscalls (`d3b7c784`).
- `syncfs(2)` implemented, issue #488, community-contributed (`91266af4`).
- A real use-after-free fixed in the inotify fd-lifecycle path, found chasing issue #467 (see Highlights) (`8d230858`).
- futex `SA_RESTART` lost-wake fix, issue #466, cherry-picked from a long-stalled branch (`cd85d48c`).

### Process / signals

- `kernel/signal`: force-default for blocked synchronous traps (see JIT/CPU section above) (`e5482332`).
- App: the guest launch command no longer respawns in a tight, CPU-burning loop after repeated instant exits (e.g. a stock PAM `login` refusing root on the app's pty) — after 3 consecutive sub-2s exits, auto-restart stops and a failure overlay points at Settings → Launch Command instead of silently spinning in the background (`a03e2bf5`).

### riscv64

- Real `ptrace`/`strace` support, issue #464 (`09b2396f`, `8da77028`).

### Security

- Bumped `fastlane` to 2.237.0 / `excon` to 1.6.0, fixing GitHub Dependabot alert #18 (GHSA-48rx-c7pg-q66r / CVE-2026-54171) (`4e609d9f`).

### Test harness / tooling

- `setup-regressions.sh`'s PASS/FAIL marker no longer misreports a test that prints trailing text after PASS, or one that legitimately SKIPs, as a failure — issue #465 (`e38aab4c`).
- `ptrace_group_stop`'s fixed 15-second watchdog is now a generous, env-scalable `alarm(test_watchdog_secs(120))` — issue #478, confirmed as timeout-tuning (the handshake itself completes in tens of milliseconds even under heavy host oversubscription), not a lost-wakeup bug (`cf4a238e`).
- `/proc/stat` cpuN-counter monotonicity fix underlying the `ktop` crash (see Highlights), plus independent `ktop` hardening against the same class of bad input (`f2d7b6d7`, `e4ce12f6`).

### Roots

- New downloadable roots: PSCAL + SmallCLUE (arm64, pure glibc userland, no Alpine/Devuan/GPL base) (`28070fc5`); experimental Arch Linux x86_64 and ARM64/aarch64 (pruned minirootfs images, real util-linux `login` + PAM, no distro-specific patching) (`689fc912`, `4d19ba01`); a new `provision-ultimate-archlinux.sh` "ultimate terminal" provisioning script mirroring the existing Alpine/Devuan ones, including a direct-daemon-launch workaround for Arch shipping no non-systemd init (`939a2f29`).
- `provision-ultimate-archlinux.sh` fixed live from an on-device user report the same day it shipped: `rsyslog` isn't in Arch's official repos (swapped for `syslog-ng`), and `pacman` aborting the whole batch transaction over that one missing package left a stale `db.lck` that made every subsequent per-package retry fail with a lock error indistinguishable, once stderr was discarded, from "package doesn't exist" — so definitely-real packages like `bash` and `coreutils` were reported as unavailable. Fixed the masked stderr, the stale-lock cleanup, and a `$?`-through-a-pipe exit-status bug introduced while fixing the first two (`192d68e9`).
- Devuan minirootfs: added `logsave` (fixes cosmetic "Cannot persist fsck output" boot noise) and `vim-tiny` (there was previously no `vi` at all reachable outside `busybox vi`); all four bundled/repo-tracked architecture images rebuilt (`82fbd8b0`, `26d81e8f`).
- Both Arch Linux minirootfs images (x86_64, aarch64) repaired: `usr/lib/systemd` restored after a prior pruning pass left `/sbin/init` a dangling symlink, silently degrading boot to a shared-console fallback (see Highlights) (`cee8a3a6`).

### Arch Linux provisioning reliability

- `provision-ultimate-archlinux.sh` and friends: interactive hostname prompt added, matching the existing timezone/user prompts (`7b7b099f`); pacman's Landlock sandbox disabled (unsupported by iSH's kernel) (`7b7b099f`); pacman signature verification disabled (a fresh Arch keyring can't populate under iSH's fragile gpg-agent support; HTTPS-only mirrors remain the transport) (`6ec8af98`); waits for `/etc/resolv.conf` and retries `pacman -Sy` once on transient DNS failure during early boot (`1f85b0e9`); creates the `/dev/fd`/`/dev/std{in,out,err}` symlinks Arch normally provisions via systemd-tmpfiles, which iSH never runs (`5b66db66`); force-reinstalls `glibc`/`linux-api-headers` to self-heal a prior pruning pass that had stripped `/usr/include` entirely from the aarch64 image (`15cfcc25`); generates missing sshd host keys at service-start time, since Arch normally generates them via a systemd unit iSH never runs (`f3d40b8b`).

### Workspace / Wayland / tools

- `fs/boot`: `/dev/shm` mode is now force-repaired to `1777` even when the directory already exists, not just on first creation — a root whose tarball shipped it with the wrong mode silently broke Wayland's keyboard-attach path (wlroots couldn't allocate its keymap shm file, and the failure didn't surface until the first VNC client actually attached a keyboard, seconds after the session otherwise looked healthy) (`3eaab1fe`).
- Wayland's runtime directory moved out of `/tmp` (the guest's own init wipes `/tmp` about a minute into boot, killing a live session's socket directory out from under it) to a fakefs-backed path under `$HOME` (`ae359bec`).
- Firefox's bundled `wayland-proxy-compositor` shim is now disabled for Wayland sessions — it corrupts the protocol stream under this emulator's unix-socket relay handling, sending Firefox into a crash-loop tight enough to drop ssh sessions; the underlying socket-relay bug is tracked separately (`93b36cf0`).
- Keyboard input: Ctrl+digits/`=`/`+`/`-`/`_` and Cmd+`=`/`+`/`-`/`0` now reach the Wayland session (previously swallowed by iOS before reaching RFB) — most visibly `foot`'s font-zoom bindings, which simply did nothing before (`eee6ed46`, `7d7ac905`).
- Workspace: Ctrl+Tab / Ctrl+Shift+Tab now cycle applet windows on the active Desktop, activating each in turn through the same path a tap uses (`cbdc6532`) — enabled by fixing two focus bugs found while building it: `TerminalView` was unconditionally claiming Ctrl+Tab (a NULL escape-sequence registration that silently ate the chord) and MotePad never reclaimed keyboard focus when cycled back to (`cf50c0e7`, `33f92d4a`).
- The kernel hostname is now seeded from `/etc/hostname` before boot's init scripts run, rather than staying `localhost` for a minute or more — most visibly, the Wayland applet's `foot` terminal starts seconds after boot and previously baked the wrong hostname into its prompt for the shell's whole lifetime (`77d66f65`).
- Added an "Auto-Show Keyboard" toggle to the Workspace menu — terminals previously always grabbed the keyboard on load/focus/tab-switch whether wanted or not; now optional (default unchanged), with direct taps still focusing manually (`b01586de`).
- `ktop`: `1` now collapses the per-cpu meters into a single combined "Avg" summary, mirroring real `top`'s toggle (reversed, since ktop defaults to per-cpu) — useful on many-core hosts or narrow terminals (`1bcc3259`).

### Community contributions

- Buffer-overflow hardening in `/proc/pid.c` (bounds-checked copy; reviewed as legitimate hardening though not currently reachable with attacker-controlled data), issue #489 (`b82704bc`).
- Three accessibility-labeling improvements (Workspace reload button, icon-only audio controls, long-press action hints), issues #458/#461/#490 (`261e7a4d`, `7b35c1c1`, `15f5bbc0`).
- `syncfs(2)` implementation, issue #488 (`91266af4`).

## Known Issues

- **Issue #503** (amd64 gdb `next`/`step` crashes with `SIGILL` immediately after a breakpoint hit): found while verifying the amd64 gdb fixes above, reproduced on both PIE and static binaries (rules out any PIE-relocation involvement), but not root-caused. A hand-rolled ptrace reproducer mimicking gdb's own breakpoint-restore-and-singlestep sequence works correctly, so the gap is specifically between that and whatever gdb's actual internal sequence does differently.
- **Issue #467** (`stress-ng --sockabuse` residual `recvfrom` wedge) is closed as presumed-resolved via cumulative hardening, not a confirmed direct fix — it was never caught live, only known from a release-notes mention and a commit body. Three sessions of escalating reproduction attempts (isolated `--sockabuse` up to 24 workers/10 minutes, then a full concurrent run exercising ~250 stressor types simultaneously) never reproduced it, but the investigation did find and fix a real inotify use-after-free and several socket/epoll conformance bugs in the same subsystems. If it recurs on-device, please reopen with whatever evidence is available.
- The Devuan `dev_t` device-node encoding bug affecting `build/devuan-x86`/`build/devuan-x86_64` (fixed for future `fakefsify` imports only) should be resolved by this cycle's full image rebuild (`26d81e8f`), but that rebuild wasn't specifically re-verified against the original bug report — worth a follow-up check before relying on it.
- AArch64 and riscv64 remain the newest guest engines and are still being hardened; expect rougher edges than the established i386/amd64 engines.
- Arch Linux support remains explicitly experimental: real util-linux `login` + PAM with none of this project's distro-specific patching (e.g. the Devuan/Alpine password-hash and init workarounds), and no systemd PID 1 — this cycle's fixes make the *rootfs* boot correctly and provisioning more reliable, but don't change that systemd itself likely can't run as PID 1 under iSH (cgroups/netlink/mount-namespace gaps, not investigated).

## Maintainer Notes

- **Ran the full guest regression suite across all four architectures concurrently**, twice this cycle, on the local CLI harness (i386/`alpinex86`, amd64/`alpine64`, arm64/`alpine-arm64-test`, riscv64/`alpine-riscv64-test`), matching the project's standard concurrent multi-arch release-testing procedure. First pass: i386 and arm64 clean; amd64 had one known environment-only failure (`random_seed` — this test image lacks `/dev/random`/`/dev/urandom` device nodes, not a regression); riscv64 initially showed a `ptrace_group_stop` timeout traced to stale, unrelated CPU-hogging processes left over from an earlier session starving the run for CPU — killed them and re-ran clean. Second pass (gdb fixes): a similar stale, day-old hung process was found and killed before starting, avoiding a repeat of the same contamination trap — worth remembering as a recurring gotcha for this harness, not a one-off.
- `CURRENT_PROJECT_VERSION` bumped to 542 across all eight build configs in `iSH-AOK.xcodeproj/project.pbxproj` — the four real app-target configs plus the four the `PBXProject` object's own project-level default config, which every other target silently inherits (kept in sync going forward, rather than left stale as in prior cycles).
- CLI build verified clean both sessions (`ninja -C build`, no warnings beyond a pre-existing benign duplicate-library linker warning). `meson test`'s `float80`/`e2e` suites fail in this sandbox for pre-existing environment reasons unrelated to this release (no host `gcc`/`cc1` — macOS `gcc` is a clang shim — and a truncated QEMU test archive); not a regression.
- On-device `/AOK/tests` run on all installed roots and app rebuild/reach-device validation of this cycle's Wayland/Workspace/keyboard-input and gdb fixes remain the maintainer's tag-time step, as usual — this session's testing was all CLI-side.
- The extensive arm64 JIT diagnostic infrastructure added while chasing the `npm`/`node` crash (fault memdumps, a store watchpoint, a traced-instruction probe, the `ISH_ARM64_NO_FUSE` knob) is dev-facing debugging tooling, not user-facing behavior — left in place (env-gated, zero cost when unused) since it's generically useful for the next hard-to-reproduce arm64 JIT bug, not removed as one-off scaffolding.
- The gdb investigation's methodology is worth calling out for future hard-to-diagnose emulator bugs: rather than trying to read gdb's own C++ internals, a minimal hand-rolled `PTRACE_TRACEME` + `execve` C reproducer (mirroring exactly what a debugger does at the ptrace level) isolated "is this a gdb bug or an iSH bug" at each layer far faster, and a git-stash A/B directly confirmed the debug-register fix was not the cause of the separate #503 issue rather than leaving that as a guess.
- Several small bot-authored PRs (Palette/Sentinel) were reviewed and merged this cycle alongside the main development work; each carries its own review note in its commit message rather than being re-litigated here.

## Commit Range
```
4856c976 docs: add iSH-AOK 542 release notes
983661a3 build: bump project version to 542
83c53bd6 kernel/ptrace: PEEKUSER on amd64 debug-register range returns 0, not EIO
46f64a02 emu/amd64: implement int3 (0xcc), the standard x86 software breakpoint
63006ee7 jit/arm64: implement PTRACE_SINGLESTEP (#501)
5ac55d6b fs/proc/pid: fix /proc/<pid>/mem truncating 64-bit addresses to 32 bits
d9701556 kernel/exec: set P_EXEC on PT_LOAD segments marked executable
f3d40b8b tools: generate missing sshd host keys in start-aok-services
1bcc3259 ktop: '1' collapses the per-cpu meters into one overall-load summary
15cfcc25 tools: force-reinstall glibc/linux-api-headers in the ultimate Arch provisioner
5b66db66 tools: create /dev/fd symlinks in the ultimate Arch provisioner
6ec8af98 tools: disable pacman signature verification (gpg keyring unusable under iSH)
b01586de app: add Auto-Show Keyboard toggle to the Workspace menu
1f85b0e9 tools: wait for resolv.conf and retry pacman -Sy on transient DNS failure
7b7b099f tools: prompt for hostname in ultimate provisioners; disable pacman Landlock sandbox
26738bd2 procfs: implement /proc/sys/kernel/random/{uuid,boot_id}
cee8a3a6 Roots: fix both Arch minirootfs images -- restore usr/lib/systemd
4e609d9f deps: bump fastlane to 2.237.0, resolving excon to 1.6.0
7933671d docs: add iSH-AOK 541 release notes
8f5ad6d6 build: bump project version to 541
192d68e9 provision-ultimate-archlinux.sh: fix package list and retry-loop bugs
939a2f29 tools: add provision-ultimate-archlinux.sh
4d19ba01 Roots: add experimental Arch Linux ARM (aarch64) downloadable root
7310f6a2 tty: implement TCGETS2/TCSETS2/TCSETSW2/TCSETSF2
689fc912 Roots: add experimental Arch Linux x86_64 downloadable root
92afca56 fake-migrate/fake-rebuild: don't abort the app on a bad fakefs database
8d230858 inotify: fix UAF where fd_retain could resurrect a dying fd
f2021a9b fake.c: don't abort the whole app on a malformed fakefs mount source
91266af4 kernel: implement syncfs(2) syscall (#488)
d3b7c784 sock: distinguish EBADF from ENOTSOCK in socket syscalls
2073b263 epoll: enforce EPOLLEXCLUSIVE EINVAL rules on epoll_ctl
35545064 sock: fix getpeername/getsockname with a small or zero-length buffer
d890dab7 epoll: detect cyclic epoll nesting on EPOLL_CTL_ADD, return ELOOP
1ee4a515 fs: implement fcntl F_SETOWN/F_GETOWN
15f5bbc0 🎨 Palette: Add accessibility hints for long-press actions (#490)
b82704bc 🛡️ Sentinel: [CRITICAL] Fix buffer overflow in /proc/pid.c (#489)
7b35c1c1 🎨 Palette: Expand accessibility labels for icon-only audio controls (#461)
261e7a4d 🎨 Palette: [UX improvement] Workspace reload button accessibility label (#458)
d0635475 amd64: fix RSP rollback on faulted POP r/m64 destination write (#487)
8d91ec6b procfs: add /proc/<pid>/mounts (was only mountinfo)
cf4a238e tests: generous env-scalable watchdog for ptrace_group_stop (#478)
270710cf kernel: fix spurious i386 SIGSEGV on a COW-raced write/read fault (#477)
5e06ab0e jit: stale-translation fixes — IC IVAU/FENCE.I invalidation, cache seq, LDAR/STLR barriers
adc2348f debug: arm64 fault memdumps + store watchpoint + traced-instruction probe
a03e2bf5 app: stop respawning the launch command in a tight loop after repeated instant exits
4a71f7df jit/arm64: ISH_ARM64_NO_FUSE debug knob to disable lookahead fusions
4e1aaeef kernel: name the file/region backing a faulting guest PC (arm64)
8a41d04a jit: bound jit_invalidate_range's page-list walk against a cyclic list
e5482332 kernel/signal: force-default blocked synchronous traps like Linux force_sig
e38aab4c tests: setup-regressions.sh grep marker accepts SKIP and PASS-with-suffix
8da77028 tests: add riscv64/ptrace_regset.c, wire up the arch subdirectory
09b2396f kernel/ptrace: wire up GUEST_ABI_RISCV64 regset marshalling
26d81e8f tools/build-devuan-minirootfs: add vim-tiny, rebuild all 4 arch images
82fbd8b0 tools/build-devuan-minirootfs: include logsave (checkfs boot noise)
a8b848c3 net: implement NETLINK_AUDIT — audit_open() works, user messages ACKed
b33b3b33 emu/memory,exec,mmap: track brk headroom as a range, not page-table entries
cd85d48c kernel/futex: recover the lost wake across an SA_RESTART restart window
33f92d4a workspace: generic WorkspaceFocusable protocol, MotePad reclaims focus on cycle
cf50c0e7 app: fix Ctrl+Tab black hole in TerminalView; give MotePad initial focus
cbdc6532 workspace: Ctrl+Tab / Ctrl+Shift+Tab cycle applet windows on the active Desktop
77d66f65 app: seed the kernel hostname from /etc/hostname at boot, not a minute in
7d7ac905 app/DisplayRFBView: accept Cmd+=/+/-/0 as zoom chords in the Wayland applet
eee6ed46 app/DisplayRFBView: pass Ctrl+digits/=/+/-/_ through to the Wayland session
93b36cf0 opt/AOK/tools/start-wayland.sh: disable firefox's wayland-proxy shim in sessions
ae359bec opt/AOK/tools/start-wayland.sh: move XDG_RUNTIME_DIR out of /tmp (guest boot wipes it)
3eaab1fe fs/boot: enforce /dev/shm mode 1777 on existing dirs (Wayland session died on keyboard attach)
06582ffe fs/fake: don't hold the db write transaction across a blocking FIFO open
e4ce12f6 opt/AOK/tools/ktop: survive backward cpu counters; fix meter clamp overflow
f2d7b6d7 kernel: keep /proc/stat cpuN counters monotonic (ktop crash under fork churn)
b5abe1b6 app: update PSCAL root download size (sshd added, ~7MB not ~6MB)
28070fc5 app: add PSCAL + SmallCLUE (arm64) as a downloadable root
fded0817 exec/mmap: reserve arm64/riscv64 brk headroom for real, not just an address gap
1bb42d22 release-notes: rename to match build-release-ipa.yml's PREV_N convention
```
