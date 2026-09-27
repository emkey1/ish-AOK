# Release Notes Since `builds/iSH-AOK_537`

These notes summarize changes from `builds/iSH-AOK_537` intended for `builds/iSH-AOK_538`.

This is the largest release in the project's recent history — 125 commits. The headline is a complete **fourth guest architecture: riscv64 (RV64GC)**, built up in fourteen incremental patches from "first guest instruction executes" through a working package manager. Alongside it, a large stress-ng-driven bug hunt closed out crashes, hangs, and deadlocks across sockets, signals, and the filesystem; the tracked FIXME catalog (#423) was closed with nine real fixes; JIT gadget-fusion landed measurable perf wins on i386, amd64, and arm64; and a run of tmpfs/mmap/memfd fixes made POSIX shared memory actually work.

A second wave, added after the initial 538 cut during real-device multi-arch regression testing, fixed three of the release's most impactful remaining bugs: a **stale-TLB use-after-free after execve** on arm64/riscv64 (#469), a compiler-miscompilation bug that hung the i386 guest test suite (#462), and — found only after #469 turned out NOT to be the same bug — a separate, longer-standing **use-after-free/double-free in the `clone()` error path** that intermittently corrupted the heap and crashed the whole app (#463). It also closed out riscv64's remaining rough edges (signal delivery, `SA_RESTART`, `epoll`) enough for **syslog-ng to run correctly on riscv64 for the first time**, and reworked `ktop` into an htop-style tool with real per-CPU accounting behind it. All three crash/hang fixes were confirmed clean under a deliberately adversarial 4-way-concurrent (i386+amd64+arm64+riscv64 simultaneously) on-device regression run — the exact stress profile that used to reliably kill the app.

## Highlights

- **riscv64 (RV64GC) guest architecture — new, JIT-gadget-only, alongside i386/amd64/arm64.** Full integer ALU, M/A/F/D extensions, RVC compressed-instruction expansion, TLB-backed loads/stores with mid-block fault restart, signal delivery/`rt_sigreturn`, `/proc/cpuinfo` + `AT_HWCAP`, and a vendor/user extension hook. Busybox `sh` runs, then `awk`, then `apk` (a real RSA-verify bug was found and fixed along the way), then `chronyd`'s fused-multiply-add/`fclass` blocker was closed. An Alpine 3.23.3 riscv64 rootfs is downloadable in-app.
- **A large stress-ng bug hunt** found and fixed a long list of crashes, hangs, and unkillable deadlocks across sockets, signals, tmpfs, and `/proc` — including a `sigsetjmp`-in-a-function UB bug that could silently deafen a thread to its own poke signal, a self-deadlock in `socketpair()`'s error path, a POSIX-timer double-free/UAF across `fork()`, and a SIGSTOP/SIGCONT race that could wedge a task in group-stop forever.
- **FIXME catalog #423 closed**: `CLONE_PIDFD` + `pidfd_open`/`pidfd_send_signal`, `FUTEX_WAKE_OP`, `MREMAP_FIXED`, `setitimer(ITIMER_VIRTUAL/PROF)`, thread-group-wide `getrusage(RUSAGE_SELF)`, a tty `ECHOKE` bit-position bug plus accurate pty `POLLOUT`, `CLONE_NEW*` → `EPERM`/`CLONE_PARENT`, and a couple of stale-comment/O(n²) cleanups.
- **tmpfs and shared-memory now actually work for their intended purpose.** `mmap()` on `/tmp`, `/run`, `/dev/shm` files previously returned `ENODEV` (breaking POSIX shared memory outright); `memfd_create()` had the identical gap. Both are now backed by an unlinked host temp file on first mmap. `tmpfs` also gained `statfs` (fixing 0-block `/tmp` reports), real rename, dot-entry `readdir`, and fixes for two out-of-bounds-write concurrency bugs.
- **`/proc` and `iotop` groundwork**: per-task I/O accounting (`/proc/<pid>/io`), a `NETLINK_GENERIC` taskstats implementation with blkio delay accounting (iotop now runs end to end), `/proc/vmstat` and `/proc/diskstats` backed by real data, `/proc/<pid>/task/<tid>` thread enumeration, `/proc/<pid>/ns/*`, real read-write sysctls, and per-VMA `smaps`/`smaps_rollup` with cross-process Pss.
- **JIT gadget-fusion perf wins**: i386 cmp/test+jcc fusion, amd64 cached-cmp+jcc fusion, and arm64 adrp+add→adr fusion all landed as dedicated single gadgets instead of two dispatch hops.
- **Correctness fixes with real-world impact**: an amd64 RIP-relative `imul r,m,imm` immediate-fetch-order bug that broke modern static glibc binaries; a `chroot`-escaping symlink bug plus an unenforced parent-directory write-permission check; `getcwd()` now correctly rebases to the process's chroot root; `ifconfig`/`ip` fixed for 64-bit guests (wrong `struct ifreq`/`ifconf` layout was silently corrupting output); a `fakefsify` bug that mis-encoded device nodes (`/dev/null` et al.) on macOS-built rootfs images; and a `sendmsg()` control-buffer bug that rejected any unpadded final `cmsg` (breaking single-fd `SCM_RIGHTS` sends).
- **App/UX**: other installed roots are now exposed at `/AOK/roots` for `chroot`-ing between guest architectures on one booted image (plus a `mount-root.sh` helper and a `ktop` that shows each process's guest ISA), Workspace desktop switching via Cmd+Arrow and a long-press arrow-key menu, several VoiceOver/accessibility passes over Palette and Workspace UI, and a fixed gear-icon long-press that had been shadowing the hidden debug panel.
- **A stale-TLB use-after-free after execve, fixed (#469).** On arm64/riscv64, concurrent `fork()`+`execve()` (e.g. `cargo`'s parallel `rustc` invocations, or a compiler's own fork+exec of its linker) could leave a task's JIT translating through a freed `struct mm` — guest `SIGILL` at `pc=0`, or, at higher concurrency, a host crash of the whole app. Root cause: a per-thread TLB refresh guard compared only a change counter, not the mmu pointer, and freed/new mm counters could numerically collide. Verified via A/B: unfixed crashed 17/38 trials at 8-way concurrency; fixed, 0/90.
- **A `clone()` error-path use-after-free/double-free, fixed (#463).** A `struct mm`/`fs`/`files`/`sighand`-corrupting app crash, first seen as heap-corruption inside `mm_copy()` and initially (incorrectly) suspected to share #469's root cause — direct evidence disproved that (the repro test never calls `execve()`, so #469's mechanism couldn't be involved) and pointed to a genuinely separate bug: `copy_task()`'s `clone()` error path released a half-created task's resources while the task was still visible in the pid table and reachable by concurrent `/proc/<pid>/*` readers on another thread, without holding the lock those readers use — a reader could retain an `mm` just as its refcount hit zero, walk its already-freed page tables, and trigger a double-free that corrupted the allocator's freelist. Only reproduced on-device (where `/proc` scanners like `ktop`/`ps`/daemons are always running) until a local repro was built by adding scanner processes to the CLI test rig. Fix: detach each resource pointer from the task under its readers' lock before releasing it, matching the invariant `exec.c`/`exit.c` already followed. Verified via a widened-race-window A/B rig: 7/7 crashes unfixed, 8/8 clean fixed.
- **i386 `futex_core` test-suite hang, fixed (#462).** The regression suite's own `futex_core` test hung the whole i386 guest suite forever on `FUTEX_WAKE_OP`. Root cause was a miscompilation in the test's own inline-asm syscall wrapper (a gcc register-aliasing hazard), not an iSH bug — corrected argument marshalling; test-only change.
- **riscv64 hardening**: fixed `SA_RESTART` register corruption (arm64's syscall-restart dispatch table was used unconditionally for riscv64 tasks too, corrupting the resumed PC/registers) and a signal-delivery bug reading a stale/truncated `cpu.esp` snapshot instead of the live riscv64 stack pointer (could kill a task with "failed to install riscv64 frame"). Also fixed `epoll_event`'s guest-ABI marshalling, which used x86's packed layout instead of the correct aligned one on riscv64, corrupting every delivered event's `data` pointer — together these were the root causes of syslog-ng spinning at 50-65% CPU with zero log throughput (and periodically crashing) on a riscv64 root; syslog-ng now runs correctly end to end on riscv64 for the first time.
- **`ktop` reworked into an htop-style tool**: colored per-CPU/memory/swap meter bars, a cursor-selectable scrolling process list, sort hotkeys, and `kill`, still with zero dependencies beyond libc + `/proc`. It now also correctly labels riscv64 processes (was previously showing "?"), runs on the alternate screen buffer so quitting restores the terminal cleanly, and its per-CPU meters now reflect real per-CPU accounting (see below) instead of an even split of the total.
- **`/proc/stat` per-CPU accounting fixed**: the `cpuN` lines were the process-wide CPU total divided evenly across every emulated CPU, so every per-CPU meter in `top`/`htop`/`ktop` read identically regardless of actual load. Each guest task's real measured CPU time is now charged to a stable virtual-CPU slot, so a workload pegging one core's worth of work now shows up on one meter instead of smeared thinly across all of them.

## User-Facing Changes

### riscv64 guest architecture (new)

A full RV64GC port targeting the aarch64 host, built the same way the aarch64 port was: JIT-gadget-only, no interpreter, landed as a sequence of staged, individually-verified patches on the `riscv` branch and merged into `working`.

- ABI scaffolding, CPU state, and ELF detection (patches 1-2): `GUEST_ABI_RISCV64`, Sv39 address space, `kernel/abi/riscv64.h`, `riscv64_regs[32]` with `x0` hardwired to zero.
- A decoder header with an RVC (compressed-instruction) expander, verified against 67 `llvm-mc`-generated expansion vectors (patch 3) — the verification harness caught a real 3-bit-vs-2-bit immediate field bug (`c.sdsp`) on its first run.
- Syscall dispatch shared with arm64's asm-generic table (patch 4), since the two ABIs use identical numbering.
- The gadget engine itself, in five slices (patch 5): minimal engine and first guest code (a), full integer ALU/M-extension/branches/JALR (b), TLB-backed loads and stores with mid-block fault restart (c), the A extension plus `clone()` child-register plumbing — busybox `sh` runs here (d), and scalar F/D arithmetic/conversions/FP CSRs — `awk` works here (e).
- Signal delivery and `rt_sigreturn` (patch 6), `/proc/cpuinfo` + `AT_HWCAP` (patch 7), and a vendor/user extension decode-miss registry for custom-opcode-space instructions (patch 5b).
- Real bugs found and fixed by running actual programs: a `JALR rd==rs1` aliasing bug that silently corrupted RSA signature verification inside `apk`'s trust check (`a66a42c0`); an `O_*` flag mistranslation that turned every `openat()` through a symlink into a spurious `ELOOP` (`081e38b8`, the same bug class the arm64 port hit); and the fused-multiply-add family plus `fclass` — the first instructions `chronyd`'s NTP filter math actually needed (`b0d085cd`).
- App integration: a downloadable Alpine 3.23.3 riscv64 rootfs, and a riscv64 subtitle in the filesystem chooser.

### JIT / CPU emulation

- **i386**: fused `cmp`/`test` + `jcc` into a single gadget on aarch64 hosts (`c5e87a01`).
- **amd64**: fused cached `cmp` + `jcc` into one gadget (`1fb3611f`); fixed a RIP-relative `imul r,m,imm` bug where the effective address was computed before the trailing immediate was consumed, reading it short by the immediate's size — this broke `exec` of modern (glibc 2.42) static binaries in a binary-layout-dependent way (`70b69b18`).
- **arm64**: folded `adrp`+`add` page-address pairs into a single `adr` gadget (`3e323315`).
- **memory**: host `SIGBUS` on a file-backed guest mmap whose backing file gets truncated smaller under a live mapping (e.g. by a racing thread) used to kill the whole emulator process; it's now caught and translated into a proper guest `SIGBUS` via a reverse host-to-guest address walk, working on both the CLI (POSIX handler) and on-device (Mach exception redirect) (`b45b7a59`).

### Stability — stress-ng bug hunt

A sustained pass driven by differential testing against real Linux under `stress-ng`, closing crashes and hangs that previously took down or wedged the whole process:

- `signal_stop_cont`: a rapid SIGSTOP-then-SIGCONT pair could leave a stop signal queued that re-stopped a task after the continue already lifted the group-stop wait — implemented Linux's `prepare_signal()` stop/cont cancellation (`5c0d6062`).
- `accept4()` no longer force-restarts over a pending guest signal, which had swallowed a stress-ng watchdog's one-shot `SIGALRM` and hung the whole run (`5c0d6062`).
- Fixed a `sigsetjmp`-in-a-function UB bug in the socket blocking-syscall path (the same class of bug the poll/select fix in 537 addressed) that could leave a thread permanently deaf to its own `SIGUSR1` poke (`5aa184dd`).
- Fixed a self-deadlock in `socketpair()`'s fd-exhaustion error path, wildcard-address (`0.0.0.0`/`::`) `sendto`/`connect` now resolves to loopback like Linux instead of failing on Darwin, and `kill()` on an exited-but-unreaped zombie now succeeds silently like Linux instead of `ESRCH` (`5aa184dd`).
- `copy_file_range()` on a socket/pipe now returns `EINVAL` immediately instead of blocking forever on a read nobody will ever satisfy (`b5447af5`).
- `fchdir()` on a non-directory fd now returns `ENOTDIR` instead of pinning it as cwd — a socket pinned this way was the entire cause of `stress-ng --sockabuse` taking 15-20 minutes instead of ~5 seconds (`889a9837`).
- A POSIX-timer double-free/UAF across `fork()`: timers are not inherited on Linux, but `tgroup_copy()` shallow-copied the parent's live timer pointers, so a child deleting its inherited id could free a timer the parent still owned (`096bb915`).
- `setsockopt`/`getsockopt` no longer size a stack VLA directly from an unvalidated guest `optlen`, which could blow the stack guard page with a ~4GB allocation (`38251dcc`).
- `fd-direct futime` for `utimensat(fd, NULL)` on sockets/pipes/unlinked files, and `path_normalize` no longer asserts on a socket used as a dirfd (`df071e93`).
- An out-of-range `clone()`/`clone3()` `exit_signal` no longer aborts the process; `CLONE_IO` is accepted as a no-op; extended syscall tables through the post-5.x range so unimplemented modern syscalls get a quiet `ENOSYS` instead of crashing on garbage argument registers; implemented aarch64-guest half-precision `FCVT` (was `SIGILL`); fixed a lock-ordering deadlock in `signalfd` delivery; replaced an incorrect `assert` in `connect()` that crashed on ordinary `SOCK_DGRAM` reconnection (`1732cb78`).
- tmpfs: fixed two out-of-bounds-write concurrency bugs from unsynchronized `fd->offset` reads and a readdir use-after-free, plus VFS conformance gaps — dot entries, real `rename()`, directory `lseek`/`seekdir` (`3ba82b05`).
- Several NULL-pointer / unsigned-underflow crashes: `/proc/<pid>/mem` and unset app callbacks called through NULL function pointers (`5ffc5e34`, `1c697c68`); `tmpfs` pread-past-EOF and shrink-`ftruncate` unsigned-arithmetic underflows; a NULL dirfd hitting an assert instead of `EBADF`; an over-strict clock-type assert in `clock_nanosleep`/timer paths (`4f93d51f`); and an `open()` symlink race that hit an assert instead of returning `ELOOP` (`d1a6987e`).

### Filesystem / mmap / shared memory

- `mmap()` on tmpfs files (`/tmp`, `/run`, `/dev/shm`) — previously `ENODEV` for every case, breaking POSIX shared memory entirely — now migrates the backing to an unlinked host temp file on first mmap (`eb6493cc`).
- `memfd_create()` had the identical `ENODEV` mmap gap; fixed the same way, and `MFD_CLOEXEC` was wrongly aliased to `O_CLOEXEC_` instead of its own `0x0001` flag value (`45fbe02d`).
- `memfd` file seals (`F_ADD_SEALS`/`F_GET_SEALS`) implemented — `fcntl(F_ADD_SEALS)` previously returned `EINVAL` unconditionally (`41459f5c`).
- tmpfs `statfs` implemented, fixing 0-block reports for `/tmp`, `/run`, `/dev/shm` that made anything checking free space before writing treat the mount as full (`9e0388c2`).
- Each mount now gets a unique anonymous `st_dev` — previously every virtual mount (tmpfs/proc/devpts/sysfs) reported `st_dev=0`, so `df` matched the wrong mount and `find -xdev`/`du -x` couldn't tell mounts apart (`c0616694`).
- `chroot` symlink escape fixed: an absolute symlink target inside a `chroot` was re-anchored at the real filesystem root instead of the chroot root; also fixed the unenforced parent-directory write-permission check that let any user create/rename/delete entries in a directory they had no write access to (`f72c3150`).
- `getcwd()` now rebases to the process's chroot root instead of returning the raw mount-absolute path (`5fb2f6d3`); `readlink()` of `/proc/*/{cwd,root,exe,fd/N}` for chroot-unreachable targets now matches real Linux's `d_path()` "(unreachable)" form instead of `ENOENT` (`e23240c4`).
- `fakefsify` no longer leaks the host's native `dev_t` encoding into fakefs device nodes — on macOS this had silently corrupted `/dev/null` and other device nodes in any rootfs image built there (`e0926b75`); a separate `fakefsify` fix corrected a root-inode missing `S_IFDIR` on rootless tarballs that broke `dpkg -i`/`apt-get install` (`f5f14ed9`).
- `sendmsg()` control-buffer parsing now accepts an unpadded final `cmsg` (callers that set `msg_controllen = CMSG_LEN(n)` instead of the aligned `CMSG_SPACE(n)` previously got a spurious `EINVAL` on any single-fd `SCM_RIGHTS` send) (`d9f817f2`).
- `ifconfig`/`ip` fixed for 64-bit guests: `struct ifreq`/`ifconf` layout (padding, pointer width) now varies correctly by guest word size, `SIOCGIFADDR`/`DSTADDR`/`BRDADDR`/`NETMASK`/`METRIC`/`MTU`/`HWADDR` ioctls added, `/proc/net/if_inet6` now emits real per-interface addresses instead of a loopback placeholder, and `/proc/net/dev` now matches Linux's spacing so byte counters ≥ 8 digits don't glue onto the interface name (`4fe1da7c`).
- amd64 `preadv2`/`pwritev2` (syscalls 327/328) implemented — previously entirely missing from the amd64 table, causing a hard "missing syscall" `SIGSYS` (`gpg` hit this in normal use). An offset of `-1` is handled per the syscall's semantics (use and advance the current file position, like plain `preadv`/`pwritev`); `RWF_*` flags are accepted but ignored, same tradeoff as the existing `fadvise64` stub. Verified against a real x86_64 binary round-tripping data through both syscalls.

### /proc and iotop

- Per-task I/O accounting and `/proc/<pid>/io`, with exited-thread counters rolled into thread-group totals (`a005405a`).
- `NETLINK_GENERIC` + a taskstats genl family implementation with blkio delay accounting — `iotop` now works end to end, reading real per-process disk rates (`58917c55`).
- `/proc/vmstat` and `/proc/diskstats`/`iostat` backed by real free/active/inactive page counts and read/write op counters instead of zero stubs (`11510818`).
- Top-5 `/proc` gaps: real `/proc/<pid>/task/<tid>` thread enumeration, `/proc/<pid>/ns/*` symlinks, real read-write sysctls under `vm/`, `net/core/`, `net/ipv4/`, `kernel/`, `fs/`, per-VMA `smaps`/`smaps_rollup` with cross-process Pss, and real `/proc/meminfo` `AnonPages`/`Mapped`/`Shmem` plus `/proc/stat` `ctxt` (`57ddd8f8`). A follow-up fixed a self-referential `task/<tid>` directory that made `htop`'s thread scanner loop forever and leak file descriptors until it hit the ulimit.

### FIXME catalog #423 (closed)

A tracked backlog of documented-but-unresolved FIXMEs, closed with real implementations, each independently validated against real Linux (mint oracle) with a new regression test:

- `CLONE_PIDFD` + `pidfd_open`/`pidfd_send_signal` (`5c90cdb8`)
- `FUTEX_WAKE_OP` (`22e28fe6`)
- `MREMAP_FIXED`, plus removal of a redundant `mremap` shrink-path busy-wait (`c4d68313`)
- `setitimer(ITIMER_VIRTUAL/PROF)` via a periodic 20ms CPU-time sampler thread (`e888ee72`)
- Thread-group-wide `getrusage(RUSAGE_SELF)` — previously only the calling host thread's usage was reported, massively undercounting multithreaded processes (`859098c5`)
- tty `ECHOKE` bit was defined at the wrong bit position (aliasing `ECHONL`), plain-`ECHOK` kill-echo now matches Linux's non-erasing behavior, and pty `POLLOUT` is now accurate rather than always-true (`f35cbe87`)
- `CLONE_NEW*` namespace flags now return `EPERM` (matching an unprivileged caller on real Linux) instead of a generic unimplemented-flag `EINVAL`; `CLONE_PARENT` implemented (`897283e8`)
- A stale DOS-vector comment in `sys_write_common` removed (the fix it described had already landed years earlier) (`a0511317`)
- Four Tier 4 cleanup items: a stale trailing-slash+`O_CREAT` FIXME documented as inapplicable, an inaccurate mount-flags comment corrected, duplicated unix-socket bind-name release logic extracted into a shared helper, and an O(n²) poll fd-merge scan replaced with a sort+merge pass (`3367a3d6`)
- Also in this pass: `fcntl` locks on inode-less fds (e.g. pty-synthesized fds) now report unlocked/granted instead of crashing on a NULL inode dereference, and `getrusage(RUSAGE_THREAD)` is now recognized as a distinct target (`2b95bf5c`)

### App / Workspace / UX

- Other installed roots are exposed at `/AOK/roots` for `chroot`-ing between guest architectures on one booted image, mounted read-write in the background off the app-launch path to avoid tripping iOS's launch watchdog (`24f3a9b4`); a new `mount-root.sh` bind-mounts `/proc`, `/sys`, `/dev`, etc. into an exposed root for a usable `chroot`, and `ktop` is a `top(1)` clone with an extra ARCH column showing each process's guest ISA (`a677087b`).
- Workspace: Cmd+Left/Right switches desktops, long-pressing the terminal accessory bar's arrow key opens a "Switch Desktop" menu, and browser tool windows gained double-tap-title-bar-to-maximize (`6f838533`).
- Fixed the gear-icon long-press gesture shadowing the storyboard's 5-second Debug Panel Trigger gesture on the same button, which made the Secret Advanced Debugging Options panel unreachable outside crash recovery (`53dde9bd`).
- The app now bundles only the arm64 rootfs archives and downloads x86/x86_64 filesystems on demand, shrinking the IPA (`fdf4fdee`); the bundled arm64 Alpine minirootfs was bumped to 3.23.3 (`055bab36`); provisioned rootfs images now default to the `C.UTF-8` locale so UTF-8-aware tools don't mangle non-ASCII output (`a3dbfd94`).
- Several accessibility passes on Palette and Workspace: VoiceOver labels on the About screen's prompt field, the Workspace browser toolbar (address bar, home, go), text fields across theme settings, and an accessibility trait for the active desktop (`2c0d4e5f`, `0c8f561d`, `9f96199a`, `cfff066c`).
- `iputils-ping` added for the `ping` command (`b40a3737`).
- Perf: cached `getenv`/avoided repeated `strlen` on hot `stat` and `readlink` paths, and an O(N²) `strcat` in `/proc/<pid>/cgroup` replaced with tracked-length `memcpy` (`59fa7712`, `91206435`, `8c3c9f66`).

### Build / CI

- Per-guest-arch selection via a new `guest_archs` meson option — any of i386/amd64/arm64/riscv64 (host-permitting) can be disabled at configure time; disabling arm64 additionally drops its interpreter/JIT sources for a smaller CLI binary (`3f56ddf0`).
- Fixed the Apple Silicon Homebrew `libarchive` include-dir fallback that aborted the whole Xcode meson build phase on ARM Macs (`c0863c7d`), and patched historical release tags to pick up the same fix when CI rebuilds them (`db4f1259`).
- Fixed a vDSO cross-build failure from Homebrew's `llvm` formula not bundling `lld` (`2de1688c`).
- Moved CI to the macos-15 runner / newest available Xcode after an aarch64 gadget assembly file hit a GAS macro-local-label parsing bug on the previous default toolchain (`367fac56`).
- CI now generates `hterm_all.js` before building instead of assuming it already exists (`8ddfc0f4`).
- Added workflows publishing unsigned dev/release IPAs to GitHub Releases for resideloading via AltStore/SideStore/Sideloadly (`909218ba`).
- `meson: link -latomic for aarch64-guest 128-bit CASP on Linux/gcc` — a clean Linux build failed to link with an undefined `__atomic_compare_exchange_16` reference; Apple clang always inlines it, so macOS builds were unaffected (`91f84ccd`).

## Known Issues

- The futex `SA_RESTART` lost-wake issue remains open (deferred; real-software-immune, device-only repro; an earlier attempted fix was reverted). This is a distinct bug from the now-fixed i386 `futex_core` test hang (#462, root-caused as a test-harness miscompilation, not a futex/SA_RESTART bug at all).
- `NETLINK_AUDIT` remains unimplemented ("Failed to connect to audit daemon" from anything that probes it) — no fix landed in this range.
- `--sockabuse` can still hang on a second, less-understood `recvfrom` wedge distinct from the signal-mask bug fixed in `5aa184dd` (isolated repros of the same scenario behave correctly; left open pending a cleaner reproduction — see `b5447af5`'s commit body).
- Two build/devuan fakefs images already built with the old `fakefsify` (`build/devuan-x86`, `build/devuan-x86_64`) still carry the bad `dev_t` encoding from `e0926b75` and need regenerating or in-guest `mknod` repair; only future imports are fixed automatically.
- AArch64 (and now riscv64) remain the newest guest engines and are still being hardened; expect rougher edges than the established i386/amd64 engines.
- `concurrent_exec_tlb.c`, the new regression test for the #469 UAF fix, hasn't yet run as part of an on-device suite pass (the on-device test bundle predates it); it has been verified locally (0/90 crashes fixed vs. 17/38 unfixed at 8-way concurrency) and should be included in the on-device suite's test bundle going forward.
- `ptrace_group_stop` timed out once on arm64 during a deliberate 4-way-concurrent (all archs at once) on-device run — "no group-stop report / never resumed." Not reproduced in isolation yet; likely load-induced (9 emulated CPUs carrying four simultaneous compiler-heavy suites) rather than a real regression, but flagged for a follow-up solo re-run before ruling that out for certain.
- A `ktop` (guest tool) crash was observed under the same 4-way-concurrent run's extreme `/proc` scanning load — a read page fault inside `ktop`'s own code, not an emulator crash (the app survived). Likely a race in `ktop`'s `/proc` scanning under unusually heavy, fast process churn; not yet root-caused, deferred as a follow-up.

- Crash-log diagnostics: `dump_stack()`/`dump_mem()` were i386-only (read `cpu.esp`/`.ebp`/`.eip` unconditionally), so a page-fault crash dump on an amd64/arm64/riscv64 task printed garbage SP/FP/PC and truncated with "Unable to read memory" instead of a real stack dump. Now ABI-aware and 64-bit-address-safe; no behavior change to fault handling/signal delivery itself, which was already correct.

## Maintainer Notes

- `CURRENT_PROJECT_VERSION` is 538 across the four main-target build configs in `iSH-AOK.xcodeproj/project.pbxproj`; the secondary (autocomplete-dummy) target's four configs remain frozen at 529, per existing convention. `builds/iSH-AOK_538` (annotated tag, matching prior releases) has been moved forward several times during this release's testing to track the second and third waves of fixes below; it now points at `c03a7bc6`.
- CLI regression pass at initial tag time (local, Apple-silicon host): clean `ninja -C build ish` build. Guest suite (`tests/manual/setup-regressions.sh`) run to completion on both an amd64 root (`50/50 PASS`, including the x86 atomics and `amd64_regress`) and an i386 root (`41/41 PASS`, atomics included). `futex_core`'s `FUTEX_WAIT` timeout hung on this local CLI/macOS/i386 setup at the time — later root-caused as #462 (see below), not the environmental read originally recorded here. Along the way, found and fixed two guest-regression-suite-only bugs (test harness, not emulator): `memfd_mmap.c` wasn't emitting the `finish_suite()` PASS marker the runner greps for (a fully-passing run reported FAIL), and `pidfd_clone.c` was missing the same `SYS_pidfd_open`/`SYS_pidfd_send_signal` libc-header fallbacks `pidfd_open.c` already had, failing to build on an older musl. Both fixed in `6af4bbd0`. Also separately verified all 8 hand-assembled `tests/riscv64/*.s` smoke tests (cross-assembled locally with Homebrew clang + `ld.lld`, no riscv64 toolchain needed) plus both states of the `riscv64_vendor_ext` opt-in gate. The `at_empty_path`/`random_seed`/`pty_line_discipline` failures seen on the (very old, Alpine 3.11.2) i386 test root turned out to be root-image gaps (ancient musl predating `fchmodat2`'s `AT_EMPTY_PATH` support; missing `/dev/urandom`/`/dev/random`/`/dev/ptmx` device nodes) — not iSH bugs; both pass once the standard `/dev` nodes exist. Host `meson test` `float80`/`e2e` fail for the same pre-existing environmental reasons as prior releases (no `gcc` in this shell's `PATH` for `e2e`); the new `riscv64_decode` test passes. The `go_release_smoke.sh` tier-3 soak test did not complete locally in the time budgeted (Go builds under CLI emulation are slow); not run to a conclusion, not treated as a gate.
- **Second-wave real-device regression pass** (M4 iPad, one arch at a time via `/AOK/roots` chroots, per [[feedback_multiarch_concurrent_chroot_testing]]): i386 and arm64 suites were rerun end to end against the `#469`/`#462`-fixed build. arm64: **52/52 PASS**, including `clone_error_cleanup` — which nonetheless went on to crash the app again in a later run (see #463 below; the earlier clean pass was a non-deterministic miss, not evidence the bug was absent). i386: `futex_core` reproduced the hang once more pre-fix (killed through to let the suite continue), then passed cleanly post-fix with no manual intervention needed.
- **Third-wave: #463 found, fixed, and validated under deliberately adversarial concurrency.** After the second wave, `clone_error_cleanup` crashed the app again during i386 verification testing — traced to an unrelated overlapping-suite mistake exposing #463 (see above), not a regression. Once #463 was fixed (`c03a7bc6`), the fix was validated by intentionally running **all four architectures concurrently** (i386 + amd64 + arm64 + riscv64, the latter on the host root directly, the rest via `/AOK/roots` chroots) — the exact stress profile that had reliably killed the app before. Result: the app survived the full run with no crash. Per-arch results: i386 53/53 PASS, amd64 52/52 PASS, arm64 51/52 PASS (one `ptrace_group_stop` timeout, not reproduced in isolation yet — likely load-induced given the 4-way CPU oversubscription, flagged for a follow-up solo re-run rather than treated as a release blocker), riscv64 48/49 (the one "failure" is `chroot_getcwd` correctly `SKIP`ping for a non-privileged user but being misreported as FAIL by the harness — pre-existing issue #465, not a real failure). A `ktop` (guest tool, not the emulator) crash was also observed under this run's extreme `/proc` scanning churn — isolated to the guest process, app unaffected, deferred as a follow-up.
- The riscv64 port is staged as 14 sequential, individually-buildable-and-testable patches on the `riscv` branch, merged into `working` once each slice landed — worth reading `74320323` (the port plan) and the patch-1-through-7 commits in order for anyone extending it further (V extension, bitmanip, a real interpreter fallback are explicitly out of scope so far). The second wave's `epoll_event`/`SA_RESTART`/signal-SP fixes close out the last known riscv64-specific correctness gaps found by running real daemons (syslog-ng) rather than just busybox/apk.
- Two commits in this range (`d9f817f2` cmsg padding, `41459f5c` memfd seals) were motivated by Wayland-client (`libwayland`/`wl_shm`) compatibility testing, but they are general POSIX conformance fixes with no Wayland-specific code attached — no Wayland/VNC feature has landed on `working`.
- A `Filling more holes` commit (`4595e96f`) has no commit body beyond its subject; if anything in it needs calling out specifically for users, check its diff before finalizing external-facing notes.
- Several CI/tooling fixes in this range landed as accidental duplicate commits (`f4e20149`/`8ddfc0f4`, `e3fb5704`/`367fac56`, `db4f1259`/`65ed3032`, `c0863c7d`/`4e3e7d9e`, `2de1688c`/`87f1a21c`, `909218ba`/`82b8259b`) — likely two parallel sessions landing the same fix; harmless (idempotent), but worth a follow-up housekeeping pass to confirm nothing diverged between the pairs.
- The second- and third-wave fixes were found and landed via real-device multi-arch regression testing (not the original CLI-only pass at initial tag time) — see `project_mm_copy_heap_corruption.md` and `project_arm64_concurrent_exec_sigill.md` in project memory for the fuller investigation history behind #469 and #463, and GitHub issues #462/#463/#469 for the canonical writeups.
- **Correction to earlier speculation**: an earlier pass of these notes (and an issue comment) speculated that #469's fix "very likely also" resolved the #463 `mm_copy` heap-corruption crash, since both manifested as mm-related memory corruption. That was wrong — #463's repro (`clone_error_cleanup.c`) never calls `execve()`, so #469's execve-triggered TLB-staleness mechanism could not have been involved, and the app crashed via #463 again after #469 was already fixed and deployed. The two are unrelated bugs that happened to share a superficial crash signature; #463 required its own separate root-cause investigation and fix (`c03a7bc6`).

## Commit Range
```
7226a0d8 kernel: implement amd64 preadv2/pwritev2 (syscalls 327/328)
91cdbec5 kernel: make dump_stack/dump_mem ABI-aware for 64-bit guests
9f441f22 docs: fold the #463 fix and 4-way concurrent validation into the release notes
c03a7bc6 fork: detach task resources before releasing them on the clone() error path (#463)
6ac74f7e ktop: fix -Wformat-truncation warnings on gcc (Devuan riscv64 build)
902e61ae tests: fix i386 raw_syscall6 register-aliasing bug that wedged futex_core (#462)
08a080b2 ktop: restore the terminal properly on quit and on fatal signals
f1ba5ce2 proc/stat: real per-CPU accounting instead of an even split of the total
84510825 ktop: htop-style interactive mode (meters, cursor, sort keys, kill)
41c1f258 signal: read the live riscv64 SP for altstack/frame placement, not cpu.esp
bcfe68c6 jit: refresh the per-thread TLB on mmu pointer change, not just counter drift (#469)
adf90f8f ktop: label riscv64 binaries in the ARCH column
67f80196 riscv64: marshal epoll_event with the aligned layout, not x86's packed one
d2671c11 fs/poll: gate emulated-fd notify poke on actual readiness
dcc76333 kernel/fs_info: guard fs_info_copy against fs_info_new() OOM failure
aaeaa849 riscv64: fix SA_RESTART register corruption and futex_core's errno double-decode
348aac44 tests/manual: add wayland_scm_shm regression test
ca966cf5 docs: add iSH-AOK 538 release notes
6af4bbd0 tests/manual: fix memfd_mmap PASS marker and pidfd_clone SYS_pidfd_* fallbacks
4fe1da7c fs/proc, fs/sock: fix ifconfig/ip guest-arch layout bugs and if_inet6/dev stats
41459f5c memfd: implement file seals (F_ADD_SEALS/F_GET_SEALS)
d9f817f2 fs/sock: accept unpadded final cmsg in sendmsg control buffers
e0926b75 fakefsify: fix host dev_t encoding leaking into fakefs rdev
0f6c0e52 riscv64: vendor/user extension hook + /AOK/docs (plan patch 5b)
0cd3c036 fs/aok: teach setup-ish-benchmark.sh's no-make fallback the -O2 pair
b3fd2712 iSH_benchmark: explicit -O0/-O2 variants; barrier keeps -O2 loops real
69c79b61 amd64 frontend: gate cc1_trace like i386; xcode-meson.sh: ISH_MESON_BUILDTYPE override
1fb3611f amd64: fuse cached cmp + jcc into one gadget
c5e87a01 i386: fuse cmp/test + jcc into one gadget on aarch64 hosts
3e323315 arm64 guest: fold adrp+add page-address pairs into one adr gadget
f821f312 fs/aok-tests: add signal_stop_cont.c and utimensat_fd.c to the manifest
b0d085cd riscv64: fused multiply-add family and fclass — chronyd's blocker
c919916c app: riscv64 subtitle in the filesystem chooser
c82f82ee xcode-meson.sh: survive options added after a build dir was configured
d3555ec9 riscv64: /proc/cpuinfo riscv format and AT_HWCAP (plan patch 7)
72fff472 app: Alpine 3.23.3 riscv64 downloadable rootfs
a66a42c0 riscv64: fix JALR rd==rs1 aliasing — apk works (RSA verify was corrupted)
081e38b8 riscv64: fix O_ flag mistranslation, real hwprobe/flush_icache table entries
1db47313 riscv64: scalar F/D arithmetic, conversions, FP CSRs — awk works (plan patch 5, slice e)
ea0cc857 riscv64: signal delivery and rt_sigreturn (plan patch 6)
4423bc87 riscv64: A extension, FP loads/stores, clone child regs — busybox sh runs (plan patch 5, slice d)
3cf9a432 riscv64: loads and stores through the TLB with fault restart (plan patch 5, slice c)
d72895aa riscv64: full integer ALU, M extension, branches, JALR (plan patch 5, slice b)
18f2b856 riscv64: minimal gadget engine — first guest code runs (plan patch 5, slice a)
1be54cef Merge branch 'working' into riscv
31e195c4 riscv64: syscall dispatch via the shared asm-generic table (plan patch 4)
a677087b opt/AOK/tools: add mount-root.sh chroot helper and ktop
24f3a9b4 app+fs: expose other installed roots at /AOK/roots for chrooting
e23240c4 kernel/fs: fix readlink() of /proc/*/{cwd,root,exe,fd/N} for chroot-unreachable targets
8ddfc0f4 ci: generate hterm_all.js before building
f4e20149 ci: generate hterm_all.js before building
e3fb5704 ci: use macos-15 runner and select newest Xcode
367fac56 ci: use macos-15 runner and select newest Xcode
db4f1259 ci: patch pre-fix release tags for Apple Silicon Homebrew paths
65ed3032 ci: patch pre-fix release tags for Apple Silicon Homebrew paths
c0863c7d tools: fix libarchive include dir on Apple Silicon Homebrew
34408f6c riscv64: decoder header with RVC expander, llvm-mc-verified (plan patch 3)
4e3e7d9e tools: fix libarchive include dir on Apple Silicon Homebrew
6a6f6784 riscv64: CPU state, offsets, ecall interrupt, TLS/exec plumbing (plan patch 2)
756ddf3a riscv64: ABI scaffolding for a fourth guest architecture (plan patch 1)
87f1a21c ci: fix vDSO cross-build missing lld on CI runners
2de1688c ci: fix vDSO cross-build missing lld on CI runners
84884c71 riscv64 plan: add vendor/user extension hook (patch 5b)
3f56ddf0 build: per-guest-arch selection via guest_archs meson option
909218ba ci: publish unsigned dev and release IPAs to GitHub Releases
82b8259b ci: publish unsigned dev and release IPAs to GitHub Releases
74320323 riscv64: add guest port plan
055bab36 app: bump bundled arm64 Alpine minirootfs to 3.23.3
5fb2f6d3 kernel/fs: rebase getcwd() to the process chroot root
a3dbfd94 AOK/tools: default provisioned rootfs locale to C.UTF-8
fdf4fdee app: bundle only arm64 rootfs, download x86/x86_64 filesystems on demand
45fbe02d kernel/memfd: back memfds with an unlinked host temp file so mmap works
b45b7a59 emu/jit: translate host SIGBUS on truncated file-backed mmap to guest SIGBUS
5c0d6062 Fix stress-ng --syscall and --schedmix hangs; gate hot-path wait traces
5ffc5e34 fs/proc: don't call through a NULL ->show / app callback on read
3ba82b05 fs/tmp: fix tmpfs concurrency crashes and VFS conformance gaps
c0616694 fs: give each mount a unique anonymous st_dev (fixes df matching wrong mount)
f9229f7f kernel/log: move the dprintf log handler fd from 666 to 555
70b69b18 emu/amd64: fix RIP-relative imul r,m,imm resolving before the immediate
d99cc5fe tests: emit the harness PASS marker from newest regression tests
9e0388c2 fs/tmp: implement statfs for tmpfs (fixes 0-block /tmp, /run, /dev/shm)
eb6493cc fs/tmp: implement mmap for tmpfs files (fixes ENODEV on /tmp, /run, /dev/shm)
f5f14ed9 tools/fakefsify: fix root inode missing S_IFDIR on rootless tarballs
58917c55 net/fs: NETLINK_GENERIC taskstats + blkio delay accounting; iotop works
a005405a fs/proc: per-task I/O accounting and /proc/<pid>/io (iotop groundwork)
096bb915 kernel/fork: don't inherit POSIX timers across fork() (double-free/UAF)
1c697c68 fs/proc: don't call through unset /proc/ish app callbacks in CLI builds
4f93d51f fs/kernel: fix emulator crashes found by the stress-ng os/filesystem sweep
889a9837 fs: fchdir() on a non-directory returns ENOTDIR instead of pinning it as cwd
b5447af5 fs: reject copy_file_range() on non-regular-file fds, matching Linux
d1a6987e fs: don't abort the app when an open() symlink race slips past path_normalize
5aa184dd sock/signal: fix three bugs that wedged or failed stress-ng's network sweep
df071e93 fs: fd-direct futime for utimensat(fd, NULL); don't abort on socket dirfd
38251dcc fs/sock: reject oversized setsockopt/getsockopt optlen before the VLA
1732cb78 Fix stress-ng-found bugs: signal SIGSYS crash, JIT SIGILL, two unkillable deadlocks, one crash
57ddd8f8 proc: implement top-5 /proc gaps, fix 4 device-found bugs, add cross-process smaps Pss
91f84ccd meson: link -latomic for aarch64-guest 128-bit CASP on Linux/gcc
4595e96f Filling more holes
11510818 fs/proc: back /proc/vmstat and diskstats/iostat with real data
f72c3150 fs: fix chroot symlink escape and unenforced parent-dir write permission
0cb3edf9 Merge fixme-setitimer-virtprof: ITIMER_VIRTUAL/PROF via periodic sampling (#423)
e888ee72 Implement setitimer(ITIMER_VIRTUAL/PROF) via periodic CPU-time sampling (#423)
a86508d0 Merge fixme-clone-pidfd: CLONE_PIDFD + pidfd_open/send_signal (#423)
5c90cdb8 Implement CLONE_PIDFD, pidfd_open, pidfd_send_signal (#423 Tier 1b)
453539f5 Merge fixme-clone-flags: CLONE_NEW* EPERM + CLONE_PARENT (#423)
897283e8 clone: EPERM for namespace flags, implement CLONE_PARENT (#423 Tier 1b)
7aa441cd Merge fixme-futex-wake-op: implement FUTEX_WAKE_OP (#423)
22e28fe6 futex: implement FUTEX_WAKE_OP (#423)
607acda4 Merge fixme-mremap-fixed: MREMAP_FIXED + drop redundant busy-wait (#423)
c4d68313 mmap: implement MREMAP_FIXED, drop redundant mremap busy-wait (#423)
d8d0a571 Merge fixme-tty-echo-pollout: ECHOKE bit fix, kill-echo, pty POLLOUT (#423)
f35cbe87 tty: fix ECHOKE bit position, implement plain-ECHOK kill echo, accurate pty POLLOUT (#423)
2bb7c0e9 Merge fixme-readv-writev-vectorize: fix stale DOS-vector FIXME (#423)
a0511317 fs.c: fix stale DOS-vector FIXME in sys_write_common (#423)
3d9164e9 Merge fixme-getrusage-selfwide: sum usage across thread group for RUSAGE_SELF (#423)
859098c5 Sum usage across the whole thread group for getrusage(RUSAGE_SELF) (#423)
a3931fc3 Merge fixme-batch-a-cleanup: resolve four Tier 4 FIXME items (#423)
6f838533 Workspace: Cmd+Arrow desktop switching, long-press arrow key menu, browser double-tap zoom
3367a3d6 Resolve four Tier 4 FIXME catalog items (issue #423)
2b95bf5c fs/lock: treat fcntl locks on inode-less fds as unlocked/granted kernel/resource: recognize RUSAGE_THREAD as a distinct getrusage target
2c0d4e5f Palette: add accessibility label to About prompt field; drop dup address label (#457)
0c8f561d Palette: Improve accessibility of text fields (#456)
9f96199a Palette: [UX improvement] Add accessibility labels to Workspace browser toolbar elements (#426)
cfff066c Palette: Add accessibility trait for active desktop (#433)
59fa7712 Bolt: Cache getenv and avoid strlen in hot stat paths (#442)
91206435 Cache strlen(target) to avoid redundant calculation in proc_readlink (#419)
8c3c9f66 Bolt: Optimize O(N^2) strcat in proc_pid_cgroup_show (#454)
53dde9bd app: fix gear-icon long-press being shadowed by terminal switcher
b40a3737 Add iputils-ping for ping command
```
