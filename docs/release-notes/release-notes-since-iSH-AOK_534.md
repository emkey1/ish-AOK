# Release Notes Since `builds/iSH-AOK_534`

These notes summarize changes from `builds/iSH-AOK_534` intended for `builds/iSH-AOK_535`.

This is a **major release** (82 commits). The headline is a brand-new **AArch64 (arm64) guest architecture** — iSH-AOK can now run real arm64 Linux binaries via a from-scratch JIT gadget port, validated end-to-end with a bundled Alpine arm64 root and real on-device boots. Alongside it: a new native **MotePad** text-editor Workspace applet, a hand-rolled locking rewrite that fixes a macOS `pthread_rwlock` lost-wakeup bug behind several device wedges, and a broad stability sweep closing out crashes found during pre-release review and live device testing (including one that traced back to a thread-storm benchmark exhausting the app's own memory).

## Highlights

- **New: AArch64 guest support.** iSH-AOK can now boot and run real **arm64 Linux** binaries — a complete from-scratch JIT gadget port (decoder, register file, syscall table, signal delivery, SIMD/crypto/atomics) adapted from OpenMinis' groundwork. Busybox, gcc, cargo, go, and Alpine's package tooling all run; a bundled **Alpine 3.21.4 aarch64** root ships in the app for immediate on-device testing. This is new, actively-hardened code — see Known Issues.
- **New: Workspace "MotePad" text editor.** A native UIKit text editor applet — line-number gutter, in-window menu bar, Print support, and a guest-filesystem browser (host `/AOK/persist` or any root's fakefs) — with async I/O, atomic saves, and autosave drafts that survive a background kill.
- **Fixed: macOS `pthread_rwlock` lost-wakeup.** Root-caused the recurring on-device fork/thread/signal wedges seen in `bmt`, `cargo`, `cmake`, and stress tests: Apple's `pthread_rwlock` can silently miss a wakeup. Replaced with a hand-rolled mutex+condvar lock across the emulator's memory subsystem.
- **Fixed: thread-storm exhausting the app's own memory.** A 10,000-thread stress benchmark could run the host process out of memory faster than iOS could react, corrupting UIKit (`EXC_BAD_ACCESS` inside `libobjc`) and leaving "ghost" guest tasks with no backing host thread. The emulator now refuses guest memory growth (`mmap`/`brk`/`mremap` return `ENOMEM`) once the app nears its jetsam budget, and a failed thread-create cleanly unwinds the guest `clone()` with `EAGAIN` instead of leaking a broken task.
- **Fixed: signal delivery to a blocked `poll()`/`select()`.** A signal meant to interrupt a blocked wait could arrive after the call's own timeout and be lost, returning `0` instead of `EINTR`. Fixed via a dedicated notify-pipe wake, independent of the host `SIGUSR1` channel TLB-shootdown pokes also use.

## User-Facing Changes

### AArch64 Guest Architecture (new)

- Full JIT gadget port for arm64 guests: integer/logical/branch instruction families, AdvSIMD vector and scalar floating point, shift-immediate and by-element-multiply space, LD1/ST1, and a complete exclusive-atomics family (LL/SC, CAS, **CASP** 128-bit compare-and-swap, **LDXP/STXP** 128-bit load/store-exclusive-pair) — all genuinely cross-thread atomic, and disassembly-verified to stay within the **ARMv8.0 baseline instruction set** so devices back to early 64-bit hardware keep working.
- Hardware crypto extension (AES/SHA-1/SHA-256/SHA-512/SHA-3) and CRC32, advertised via `AT_HWCAP`, with **soft C fallbacks** on hosts that lack the silicon extension (override with `ISH_ARM64_FORCE_SOFT_CRYPTO`).
- EL0 cache-maintenance instructions (`IC IVAU`, `DC CVAU`/`CVAC`/`CVAP`/`CVADP`/`CIVAC`) — used by runtimes' `__clear_cache` after generating code at runtime — are handled as safe no-ops; `syslog-ng`'s crash-loop on this instruction is fixed.
- Full syscall-table parity sweep (OpenMinis-parity syscall implementations, 64-bit pointer-truncation audit, xattr family, `munlock`, `openat2`/`fcntl` flag translation) so aarch64 syscalls don't silently corrupt data or `SIGSYS`-kill the guest.
- Two performance passes: an `-O0` hot-path pass (fast load/store, mov-wide folding, instruction fusion) and a follow-up round (`ldar` dispatch, compare+branch fusion, block chaining) that made arm64 the fastest guest architecture in local benchmarks.
- A bundled **Alpine 3.21.4 aarch64** minirootfs ships in the app for immediate testing; visible as a normal root-import choice.

### Workspace — MotePad (new)

- A native UIKit text editor applet: line-number gutter, in-window menu bar with full keyboard shortcuts, Print support, Find/Go-to-Line, and a guest-filesystem file browser.
- Browses and edits files on the host (`/AOK/persist`) or inside any root's emulated filesystem via a borrowed VFS task context (the same mechanism the Music applet uses).
- All guest/host file I/O runs on a background queue with a loading indicator — opening a large file or a busy emulator no longer freezes the app.
- Saves are atomic (temp file + rename), so a failed save (out of disk space, etc.) cannot destroy the file being edited.
- Debounced autosave drafts, stored outside the guest filesystem, restore unsaved text after a background jetsam kill.
- Symlinked directories and files (e.g. a merged-usr root's `/bin`, `/lib`) are browsable and openable, not dead ends.

### Stability

- **Thread-storm / memory exhaustion (new this release).** See Highlights. The emulator now pushes back on guest memory growth as the app nears its jetsam limit (tunable via `ISH_GUEST_MEM_HEADROOM_MB`, default 192 MiB; `0` disables), and a host thread-creation failure during `clone()` unwinds cleanly instead of leaking a task with no backing thread.
- **`pthread_rwlock` lost-wakeup (new this release).** See Highlights — this was the underlying cause of several previously-mysterious device wedges under `fork`/thread/signal load.
- **`poll`/`select` signal delivery (new this release).** See Highlights.
- Fixed a `clone()` error-path bug where a failing thread creation (bad `CLONE_SETTLS`/`CLONE_*_SETTID` pointer) could leave a freed thread-group object still linked into the live pid-rooted lists — a use-after-free.
- Fixed a socket-pair address truncation on arm64 (48 bits truncated to 32) that produced spurious `EBADF` from Rust programs.
- Fixed an `epoll_event` struct-layout mismatch on arm64 (16-byte aligned, not x86's packed 12 bytes) that `SIGSEGV`'d Go binaries.

### Diagnostics

- Crash reports for the visible terminal session no longer show a misleading guest process name (e.g. `login-459`) on the main UIKit thread — the main thread is never renamed, only worker threads running guest tasks are.
- `getsockopt(SO_BINDTODEVICE)` now matches real Linux (success, empty name) instead of `ENOPROTOOPT`, which stopped OpenSSH's VRF probe from logging spurious "Protocol not available" errors on every SSH session.

## Known Issues

- **AArch64 is new and under active hardening.** It has been validated against real Alpine and Devuan userspaces (busybox, gcc, cargo, go, apk) and, for the atomics work specifically, differentially verified against real Apple Silicon — but it has not seen the length of real-world exposure the i386/amd64 engines have. Expect rougher edges than the established architectures for at least the next few releases.
- **`signal_poll` can fail under heavy concurrent load** (long-standing, not new in 535). Under many simultaneous emulated processes, a signal meant to interrupt a blocked `poll()`/`select()` can still occasionally race its own timeout, because guest signal delivery and inter-CPU TLB-invalidation pokes share the host `SIGUSR1` channel. Reproduces only under deliberate concurrency (~3 of 4 simultaneous instances); sequential/single-process runs (how the regression suite runs) pass reliably.
- **The futex `SA_RESTART` lost-wake issue remains open** (deferred, real-software-immune; cannot be reproduced on the macOS CLI, only on a loaded device).
- The jetsam memory-headroom guard (192 MiB default) is new this release; if it proves too tight or too generous for a given workload, tune with `ISH_GUEST_MEM_HEADROOM_MB` rather than filing it as a regression.

## Maintainer Notes

- **AArch64 build config.** The new `jit/guest-arm64/` gadget sources are gated to `jit_cpu_family == 'aarch64'` builds via a new `ISH_JIT_ARM64_GUEST` define; non-arm64 CLI hosts (x86_64 macOS, the Lima Linux test VM) build and link cleanly again with a clear `die()` if an arm64 guest binary is ever run on one.
- **CASP/LDXP/STXP implementation notes.** All three land as genuine host-atomic operations (not stubs), verified via disassembly to stay within ARMv8.0 baseline (no LSE-only mnemonics reach the host unconditionally), and differentially tested against real Apple Silicon. 16-byte atomic *loads* specifically use two 8-byte `ldar`s rather than a compiler-lowered `ldxp`/`stxp` loop, because that lowering performs a spurious store back — which faults on a read-only guest page.
- **Locking rewrite scope.** The `pthread_rwlock` replacement (hand-rolled mutex+condvar) covers the emulator's memory-subsystem lock; `jetsam_lock` deliberately stays a raw lock (unrelated failure mode). Fork/signal lock paths were separately hardened against the mem-quiesce stop-the-world barrier used by the new lock's parking mechanism.
- **Test additions.** `tests/manual/` gained an `arm64/` subdirectory (mirroring the existing x86 layout) with a dedicated atomics suite (`atomics64.c`, including a cross-thread 128-bit counter scenario) and an aarch64 raw-syscall shim for `futex_core`.
- **Dependency bump.** `faraday` 1.10.5 → 1.10.6 (CVE-2026-54297, high).
- Most of this release's emulator-level fixes were validated on an arm64 host and differentially verified where noted (real Apple Silicon for atomics, a real-Linux Lima VM oracle for the socket-option fix); all have since been confirmed on-device by the maintainer, and the guest-side regression suite (`/AOK/tests`) has passed.

## Commit Range

```
03b616ab fs/sock: getsockopt(SO_BINDTODEVICE) returns empty like Linux, not ENOPROTOOPT
227a8ae7 mem: refuse guest memory growth when the app nears its jetsam budget
c9e9fec9 kernel: unwind clone() when the host thread can't be created; keep guest names off the main thread
b657048e xcode: add CFBundleDisplayName/app-category Info.plist keys; re-normalize pbxproj comments
d5994c92 arm64 guest: LDXP/STXP (load/store exclusive pair) — complete the exclusive family
bf8ffda3 app: convert keyboard frame through the window, not a UIScreen space
c21e33ba arm64 JIT: EL0 cache-maintenance ops (IC IVAU, DC clean-by-VA) as no-ops
219ea41a arm64 JIT: CASP, atomic alignment faults, fetch-fault SIGSEGV, fusion cap, host gating
34d31d81 app/workspace: MotePad hardening — async I/O, atomic saves, drafts, FR handoff
ff183874 arm64 guest: xattr native dispatch, munlock table entry, open-flag translation
469773d6 app/workspace: fix Monitor card clipping its last row
781ccdeb app/workspace: refresh MotePad doc title on load/save
1946bdf0 app/workspace: center MotePad popup menu rows for even padding
fc152748 app/workspace: fix MotePad popup menu top padding
70cb7d05 app/workspace: custom compact MotePad menus (content-sized popover)
86a6881c app/workspace: drop leading icons from MotePad menus
33335ced app/workspace: render MotePad menu shortcuts as title text
1e438a1d app/workspace: MotePad menus show shortcut glyphs + full item set
844be80b app/workspace: MotePad keyboard shortcuts
d8b8c0c7 app/workspace: add MotePad text-editor applet
4f616d9b xcode: add Xcode Cloud manifest + normalize pbxproj reference comments
d1906b26 tests/arm64: commit the crypto-extension differential test asset
bc1200df build(deps): bump faraday 1.10.5 -> 1.10.6 (CVE-2026-54297, high)
a0f3b6cb Merge branch 'aarch64' into working — AArch64 guest port (mostly functional)
6b987cc5 arm64: silence deliberate-ENOSYS pidfd/io_uring stubs (go build ERROR spam)
c7c7e436 arm64: epoll_event is 16-byte aligned, not x86's packed 12 (go build SIGSEGV)
b56e36b7 arm64: socketpair sockets_addr was truncated to 32 bits (rust EBADF)
9638d210 kernel: harden fork/signal lock paths against the mem-lock barrier
6099c419 util: replace mem rwlock with a hand-rolled mutex+condvar (macOS lost-wakeup)
11d0a235 arm64: restore orig X0 on syscall restart (strace cmake libuv fd>=0 abort)
d03cfe62 arm64 guest: unsigned rounding FP->int conversions (FCVT{N,P,M,A}U)
54a97453 arm64 guest: ORR/BIC (vector, immediate) — cargo SIGILL
c0d31fe5 arm64 JIT: 64-bit interrupt gadget + loud rejects — fix SIGILL pc reporting
723d4b5e workspace: don't clamp window frames to an unlaid-out desktop surface
77db95e7 arm64 guest: FP<->fixed-point conversions (SCVTF/UCVTF/FCVTZS/FCVTZU #fbits)
5db778be mem quiesce: park waiters on a condvar — fix bmt thread-storm wedging the app
3242c19b arm64 guest: soft crypto fallbacks — CRC32/SHA512 on hosts without the extensions
bc9aa4af arm64 JIT: -O0 hot-path pass — fast load/store, mov-wide folding, idiom fusion
90ccefbc tests: aarch64 raw-syscall shim for futex_core
7dc4c888 arm64 guest: LSE atomics + make LL/SC and CAS genuinely cross-thread atomic
c647f497 arm64 guest: crypto extension (AES/SHA1/SHA256/SHA512/SHA3) + AT_HWCAP advertising
680feeb6 arm64 guest: complete vector/scalar shift-immediate space + by-element multiplies
db8ddb9e amd64 JIT: native SSE FP long-tail — 17x on double matmul (was crawling through the interp bridge)
aad2fb04 tests: arch-specific /AOK/tests subdirs + new arm64 suite (which immediately caught a real CLREX bug)
b3ebd43a arm64 guest: OpenMinis-parity syscall sweep — implement preadv/pwritev + 11 more, clean errnos for the rest
875db8d4 arm64 guest: wire fadvise64 (223) + aarch64-format /proc/cpuinfo
6862696c arm64 guest: audit + fix 64-bit pointer truncation across the syscall surface
52de3855 arm64 guest: fix empty dmesg (syslog buffer pointer truncation)
2812fc8c arm64 guest: enable crc/aes assembler extensions explicitly for Xcode builds
7fcb889d arm64 guest: gcc now compiles+runs — two-reg-misc/three-diff/across/permute/copy/struct-mem port + 3 critical latent bugs
bd00533e arm64 guest: port AdvSIMD three-same family (vector+scalar, int+FP) from OpenMinis
ef34ec10 arm64 guest: fix truncate/faccessat/open-flags native dispatch
7ed5a133 aarch64: translate open(2) flags — fixes on-device apk "Symbolic link loop"
b04153b6 aarch64: AdvSIMD LD1/ST1 (load/store multiple) — fixes on-device cd crash
177b2d7a aarch64: fix on-device login — socket-family pointer truncation + LDPSW
5ea7eddd aarch64: AdvSIMD vector shift-by-immediate — fixes on-device getty loop
d9f67093 app: bundle the Alpine 3.21.4 aarch64 minirootfs for on-device testing
cf875921 aarch64: perf round 2 — ldar dispatch, cmp+branch fusion, loop chaining
48c76a6e aarch64: fast-path ALU gadgets — specialization pass, slice 1
ba50b3c1 aarch64: block chaining — arm64 becomes the fastest guest
082ff68e aarch64: scalar floating point — busybox awk fully works
c4310ab7 aarch64: fix branch-wide i386/amd64 SIGILL — uninitialized state->arm64
2c8b85d3 aarch64: arm64 signal delivery + mid-block fault restart
414256fe aarch64: Phase D part 1 + syscall layer — real Alpine busybox runs
4bff3def aarch64: JIT gadget port Phase C part 3 — integer core completion
3975fa3a aarch64: JIT gadget port Phase C part 2 — DP_REG (shifted register)
4f39487f aarch64: JIT gadget port Phase C part 1 — Logical (immediate)
61dee547 aarch64: JIT gadget port Phase B — branches, ADD/SUB-imm, LDP/STP
f690ee27 aarch64: JIT gadget port Phase A — real gadgets, validated end-to-end
6acd6a19 aarch64: direction change — pack NZCV, pivot to porting the JIT gadget set
a7d500e9 aarch64: add Logical (immediate) support, informed by real-rootfs testing
d3327bbf aarch64: patch 5 — ELF loading + real execution, exit criteria verified
b9084fdc aarch64: patch 4 — AArch64 syscall table
9b61ed56 aarch64: fix arm64 stack placement — keep it low, like amd64
e1bc4320 aarch64: patch 3 — AArch64 decoder and interpreter core (scope-cut)
cffc912d aarch64: patch 2 — AArch64 register file in struct cpu_state
b69c29ec aarch64: confirm jit/guest-arm64/ as the gadget dir name
381448a0 aarch64: patch 1 — ABI scaffolding for GUEST_ABI_ARM64
7da0d16f aarch64: add guest port plan
34b31d27 fs/poll: fix poll/select returning 0 instead of EINTR under signal load
04a9bf54 deps/audio: add Headers/opus to HEADER_SEARCH_PATHS so the Opus decoder compiles
30598c7d deps: build + link audiocodecs.xcframework (Ogg Vorbis + Opus); fix build script for modern Xcode
```
