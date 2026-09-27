# Release Notes Since `builds/iSH-AOK_531`

These notes summarize changes from `builds/iSH-AOK_531` intended for `builds/iSH-AOK_532`.

This is a large release (214 commits). The headline is that the **64-bit (amd64) guest has gone from experimental to a genuinely usable, fast environment**, backed by a big JIT performance campaign and broad syscall coverage; alongside that, the kernel's OS semantics were systematically vetted against **real Linux** and dozens of nonconformances fixed.

## Highlights

- **64-bit (amd64) guest matured dramatically.** The amd64 JIT now reaches parity with i386 and adds large speedups (native gadgets for nearly every hot instruction; `gcc`/`g++` roughly **4× faster**; SHA-512/crypt roughly **3×**; page-batched `rep` string ops). Combined with much broader syscall coverage, real package installs now work on amd64 (e.g. `apt`/`dpkg`, `openssh-server`). On-device the full guest regression suite passes 32/33 on amd64.
- **OS semantics vetted against real Linux.** Six subsystems — signals, filesystem/VFS, process/thread lifecycle, time/clocks, sockets/IPC, and memory management — were checked differentially against a real Linux oracle, fixing ~40 behavioral nonconformances.
- **x86 vector ISA completed.** SSE3, SSSE3, SSE4.1, and SSE4.2 are now implemented in **both** the i386 JIT and the amd64 engine and advertised via CPUID, eliminating `SIGILL` in vectorized binaries (cmake, crypto, string-heavy code).
- **Real named-pipe (FIFO) support** plus tmpfs symlinks and many `/proc` and filesystem correctness fixes — resolves `telinit` aborts, a parallel `make -jN` jobserver hang, and BusyBox `ip addr`.
- **Two genuine crash/abort classes fixed across all engines:** `SAHF`/`LAHF` and `LOOP`/`LOOPE`/`LOOPNE`/`XLATB` were `SIGILL` even on i386, and a 32-bit `fcntl(F_GETLK)` path was silently overflowing the guest's stack.
- **Workspace UI redesign** and a substantially expanded **LLM Chat** client (guest-shell tool use, Markdown rendering).
- **Performance:** lower per-syscall locking overhead, faster `fork`, and a large reduction in the musl/Alpine task-switching stalls caused by mmap quiesce barriers.

## User-Facing Changes

### 64-bit (amd64) Guest — Performance

- Native JIT gadgets replaced C "bridge" calls for almost all hot operations: stack push/pop/call/ret/leave, conditional and indirect jumps, register and memory ALU (8/16/32/64-bit add/sub/cmp/and/or/xor/adc/sbb/inc/dec/not/neg/test), shifts and rotates (by imm, by CL, by 1), `imul`, `movzx`/`movsx`/`movsxd`, `MOV` reg↔mem (all widths), and flag ops (`stc`/`clc`/`cmc`/`cld`/`std`/`sahf`/`lahf`).
- `cc1` (the GCC compiler proper) now runs under the JIT instead of being forced to the interpreter — about **4× faster** `gcc`/`g++` compiles, byte-identical output.
- SHA-512 / crypt throughput improved ~3× via native rotate/shift/ALU gadgets and an SSE byte-scan path.
- `memcpy`/`memset` sped up with a page-batched fast path for forward `rep movs`/`rep stos`.
- The amd64 JIT is enabled by default on Apple-silicon (aarch64) hosts; it stays off on x86_64 CLI hosts.

### 64-bit (amd64) Guest — Correctness and Syscalls

- Implemented real `copy_file_range` and `sendfile` (actual data copy), and OFD locks (`F_OFD_GETLK`/`SETLK`/`SETLKW`); fixed the amd64 `struct flock` marshalling and `F_SETLKW64` blocking that had deadlocked `dpkg`/`apt` (ca-certificates install).
- Wired `fchownat(AT_EMPTY_PATH)`, `name_to_handle_at`/`open_by_handle_at`, `vhangup`, `clock_settime`, `fchmod`, `fadvise64`, and `pidfd_open`/`io_uring`/`seccomp`/`membarrier` stubs so they return proper errnos instead of `SIGSYS`.
- Fixed 64-bit pointer/argument truncation across the syscall boundary, the `sysinfo(2)` struct layout, and dynamic-PIE placement so the `brk` heap has room (git no longer hits a tiny brk cap).
- Implemented `cmpxchg16b`, `LDMXCSR`/`STMXCSR`, `LFENCE`/`MFENCE`/`SFENCE`, `FWAIT`; treat `RDSSPD`/`RDSSPQ` (CET shadow-stack reads) as NOPs rather than `SIGILL` (had broken all C++ exceptions / static binaries).
- Fixed numerous decode/flag bugs: `modrm` no-base `disp32` form, `cbw` sign-extension, 32/64-bit `mul`/`imul` high half and CF/OF, `shl`/`sar`/`rol`/`ror` edge cases, and `adc`/`sbb` AF/OF with carry-in.

### CPU Emulation Correctness (both engines)

- Implemented `SAHF`/`LAHF` (0x9E/0x9F) and `LOOP`/`LOOPE`/`LOOPNE` (0xE0–0xE2) + `XLATB` (0xD7) — these were `SIGILL` on **every** engine, including i386.
- i386 JIT: raise `#DE` on divide-by-zero / `idiv` overflow; fix 2-op `imul` CF/OF for 8/16-bit operands; decode the 0x66 operand-size prefix after `rep`/`repnz` (fixes a `rep movsw`/`stosw` `SIGILL`).
- An unimplemented vector op now raises `#UD` instead of aborting the emulator.

### x86 Vector (SSE) Completeness

- SSE3, SSSE3, SSE4.1, and SSE4.2 implemented in both engines (crc32, `pcmp*str*`, `pshufb`, `pmovsx`/`pmovzx`, `ptest`, `popcnt`, blends, `pmulld`, `dpps`/`dppd`, `mpsadbw`, `phminposuw`, horizontal add/sub, etc.) and advertised via CPUID; `/proc/cpuinfo` reflects them.
- i386 JIT gained packed/scalar double SSE2 ops and conversions; fixed an in-place `shufps`/`shufpd` lane-corruption bug and SSE min/max `+0`/`-0`/NaN tie-break; amd64 gained the missing packed/scalar SSE floating-point ops and out-of-range `cvtt*` → integer-indefinite.

### OS-Semantics Conformance (vs real Linux)

- **Signals:** realtime delivery order, `SI_TKILL`, `SIGCHLD`/`waitid` `CLD_*` codes, amd64 64-bit `sigtimedwait` width, stop reporting.
- **Filesystem/VFS:** `O_NOFOLLOW`→`ELOOP`, `rmdir`/`mkdir` not following a final symlink, `unlink(dir)`→`EISDIR`, lexical `..` through a missing component, trailing-slash `O_CREAT`→`EISDIR`, tiny-buffer `getdents`→`EINVAL`, `RENAME_NOREPLACE`, `F_DUPFD` negative-arg, `/proc/self`, and `ENAMETOOLONG` (not `EFAULT`) for over-`PATH_MAX` paths.
- **Process/thread:** `setpgid` validation, `waitpid`/`wait4`/`waitid` flag handling and `WCONTINUED`.
- **Time/clocks:** `getitimer`, amd64 `timer_settime`/`gettime` + `timerfd` 32-bit-width bug (timer never fired), `nanosleep` remainder on `EINTR`, `CLOCK_TAI`, clock validation.
- **Sockets/IPC:** `eventfd` semaphore mode, `poll` neg/closed fds, `epoll` self-add/bad-op, `SO_ACCEPTCONN`, AF_UNIX no-dest, datagram `MSG_TRUNC`, pipe/FIFO poll readability.
- **Memory:** `PROT_NONE` now enforced on **reads**, `MAP_FIXED_NOREPLACE`→`EEXIST`, mmap flag/alignment validation, `madvise`/`msync`/`mincore` validation, and `MADV_DONTNEED` actually zeroing anonymous pages.

### Filesystem, FIFOs, and /proc

- Real FIFO (named-pipe) support for tmpfs and the SQLite-backed fakefs; `/run/initctl` is now a real FIFO, fixing a `telinit` `SIGABRT`.
- tmpfs gained symlinks, `umount`, and `fsetattr` (fd-based `fchmod`/`fchown`); fixed five correctness bugs in the fake filesystem and a locale-independent UTF-8 archive import.
- `make -jN` no longer hangs on its host-FIFO jobserver (Darwin `poll()` never reports `POLLHUP`).
- `/proc` improvements: named cgroup hierarchies in `/proc/<pid>/cgroup`, guest (not host) load average in `/proc/loadavg`, synthesized `console=` on `/proc/cmdline`, and a Linux-compat shim for `/proc/net`.
- BusyBox `ip addr` works over emulated netlink sockets (bare `write`/`read` path + `SIOCGIFTXQLEN`).

### Performance and Responsiveness

- Cut the mmap quiesce-barrier overhead that made musl/Alpine task switching slow (sched_yield backoff, fewer wasted SIGUSR1 pokes).
- Removed a CAS retry loop and a per-syscall signal-mask save from the syscall hot path; bounded the page-directory scan for ~1.7× faster `fork`.

### Workspace

- Redesigned status applets (Info merged into Monitor, gauge-style option, IPv4-first networks, a Logs view), with phone/narrow-window scaling and theme-contrast fixes; Settings opens in a proper tool window; the Workspace button opens full-screen in the current scene.

### LLM Chat

- Guest-shell tool use (run commands in the iSH shell, confirmed per command), Markdown rendering of assistant replies, a Workspace tile, networking robustness, and a fix for spaces dropped at streaming token boundaries.

### Terminal

- Standardized the guest `TERM` on `screen-256color` (vt102 fixed up at exec time); settings gear and hide-keyboard moved into the accessory bar; iPhone settings gear floats above the keyboard.

### Other Fixes

- `epoll_wait(-1)` no longer returns a spurious `0` (was aborting libuv/cmake); `EPOLLONESHOT` re-arming via `EPOLL_CTL_MOD` wakes a blocked `epoll_wait`.
- pty masters waiting on a blocking read are now woken on slave close/hangup (fixes `tmux`/`script` exit wedges).
- `sigusr1_handler` is async-signal-safe against lazy TLV allocation (fixed an app crash).
- ptrace group-stop reporting so `strace -f` follows forks and threads.
- Filesystem names with spaces/unsafe characters are rejected (and bundled-root import names kept valid for guest init).

## Known Issues

- **`futex_core` "signal restart" is flaky on-device under load** (~75–85% on a busy device, both i386 and amd64; ~0% on a quiet device, and not reproducible on the macOS CLI). A `FUTEX_WAKE` that races an `SA_RESTART` signal can be lost while the waiter is mid-restart, so the restarted wait runs to its timeout. This is **deferred**: it is real-software-immune (every libc synchronization primitive changes the futex word before waking, which degrades a lost wake to a harmless re-check; real Linux passes the test because its syscall restart is microseconds). Tracked for a post-release fix.
- The 64-bit (amd64) guest, while now broadly functional, is still newer than the i386 guest and should be treated as the less-seasoned of the two.

## Maintainer Notes

- `builds/iSH-AOK_530` points to `bdf44174`; `builds/iSH-AOK_531` points to `a3fdfb6b`. `builds/iSH-AOK_532` should point to this release-notes commit.
- `CURRENT_PROJECT_VERSION` bumped 531 → 532 (the four app configs; the secondary target remains at 529, as in 531).
- The amd64 native-ALU work was developed on `amd64-byte-alu` and merged into `working` in `136f6993` (validated bit-exact vs a Rosetta x86 oracle and a real-Intel Linux VM; `amd64_regress` clean).
- A differential JIT/conformance test harness was added under `tests/remote/` (host + Rosetta + real-Linux "mint" oracles), and the `/AOK/tests` guest suite is now generated at build time from `fs/aok-tests.manifest`. A `/AOK/tools` provisioner was also added.
- Release validation at tag time:
  - On-device guest regression suite (`/AOK/tests`) on **both** roots: **32/33** on i386 (i686) and **32/33** on amd64 (x86_64) Alpine 3.23.3 — the only failure on each is the known `futex_core` flake above.
  - The i386 `fcntl_lock`/`fcntl_ofd` crash fix verified on-device (0/10 failures); amd64 was never affected.
  - amd64 instruction emulation validated bit-exact against Rosetta and a real-Intel Linux VM across the `tests/remote` corpus; OS semantics validated against the same Linux VM.

## Commit Range

73e9b42e build: bump project version to 532
20c6e337 Revert futex SA_RESTART lost-wake fix attempt + diagnostic (deferred as known-flaky)
464ce71b kernel/futex: honor a FUTEX_WAKE that races a signal (SA_RESTART lost wake)
e20528ba kernel/futex: add ISH_TRACE_FUTEX diagnostic for the SA_RESTART lost-wake race
5bedbcc3 fs/fd: stop fcntl flock marshalling from overflowing the guest struct (i386 crash)
6d30204b aok-tests: skip amd64_regress on i686 guests (x86-64-only asm)
e0d7fadb aok-tests: bundle the 10 regression sources missing from /AOK/tests
9427c4af random: seed via /dev/{u,}random ioctls + /proc/sys/kernel/random
dee657f7 Terminal bar: settings gear in the Workspace bar + centered . and / keys
ebd1b457 LLM Chat: render assistant replies as Markdown
551d8d8e Terminal: put hide-keyboard + settings in the accessory bar on all devices
136f6993 Merge branch amd64-byte-alu into working
7ad11c09 tests/manual: add memchurn.c, the musl mmap-churn task-switch repro
6292e6b9 gitignore: ignore .cache/ and *.o; drop accidentally-committed log.o
7bac5f4d opt/AOK: install cmake in provision-ultimate-alpine
dc7077e2 app/Roots: keep bundled amd64 root importName valid for guest init
344a7722 kernel/hostinfo: add recent devices and A10/A10X Fusion core topology
28d5c1e9 fs/real: unwedge make -jN jobserver on Darwin FIFOs
d004a714 app: define anchorPopoverForAlertController:toSource: in shared category
2b3ad3d2 Terminal: float the Settings gear above the keyboard on iPhone
3b01527e Workspace Logs: register an iPad minimum size so the resize handle shows
9661ad2d Workspace Logs: fill the resizable window and scroll internally
fc707ada Workspace: remove the Processes applet
74dcb1d8 Settings 'Workspace' button: open full screen in the current scene
ed325075 Workspace status: merge Info into Monitor, IPv4-first networks, Logs view
cf2e9fb1 Workspace status applets: scale down further (smaller gauges, tiles, fonts)
54c90d14 Workspace status applets: scale down on phones and narrow windows
0e578b6b Workspace status applets: GUI redesign + gauge-style preference (Info, Monitor)
b3856cf3 Workspace tools: apply theme after subclass views exist (fixes invisible Clock text)
41a1ec14 Workspace Clock: fix low-contrast readout on darker themes
c75f03cf Workspace Settings: open in a tool window with a working Done button
e57c4db2 emu: complete SSE3/SSSE3/SSE4.1/SSE4.2 and advertise them via CPUID
c040f774 Workspace themes: fix focused-window contrast (Aurora & Graphite)
16825ab9 app: don't show the LLM server URL in the empty chat placeholder
6190a52f app: LLM Chat tool-use UX, workspace tile, and networking robustness
5ad01bbc jit/amd64: native register adc/sbb (reg-reg + reg-imm, 32/64)
2b70f2d0 jit/amd64: native 32/64-bit NOT/NEG (0xf7 /2,/3, register)
f88a1515 emu/jit: implement LOOP/LOOPE/LOOPNE (0xe0-e2) + XLATB (0xd7) — were SIGILL
21658431 jit/amd64: native 16-bit ALU arith reg-imm/reg-reg (ADD/SUB/CMP)
a616fdd5 emu/jit: implement SAHF/LAHF (0x9e/0x9f) — were SIGILL on every engine
efe4c16a jit/amd64: native byte (8-bit) ALU gadgets (the cc1 hot path)
40e21e1a app/kernel: add guest-shell tool use to the LLM Chat client
539cc80f app: fix LLM Chat streaming dropping spaces at token boundaries
3558ee46 emu/amd64: skip per-block frontend debug machinery when disabled
61105d11 emu/amd64: page-batched fast path for forward REP movs/stos
85e56e72 emu/amd64: skip per-write debug trace probes when disabled
0187119d emu/kernel: cut mem-quiesce + mmap overhead (musl/Alpine task-switching)
aa528c92 app: fix Palette override validation accepting invalid colors
1be4f376 app: fix Solarized default theme rendering gray/bright text invisible
482009b9 kernel: fix amd64 "needs full-width args" SIGSYS for seccomp (317) and membarrier (324)
31221c69 aokfs: add linux-headers to the /AOK/tools provisioner
cf60d966 aokfs: ship a /AOK/tools provisioner; generalize the gen-aokfs generator
71be198d kernel: stub i386 io_uring_setup/enter/register (425-427) as silent ENOSYS
156c6bd8 emu: implement SSSE3/SSE4.1 three-byte (0F 38/0F 3A) ops for i386 + amd64
44e6729b net: make BusyBox `ip addr` work over emulated netlink sockets
22018644 amd64: flush stale frontend JIT cache after jetsam free (SMP use-after-free)
af9b8909 cli: add ISH_MULTICORE env toggle to match the device's threading
0e2e8aa7 kernel: don't leak a spurious EINTR from wait4/waitpid on a host poke
00884881 amd64: handle clock_settime (227) natively so a high timespec ptr can't SIGSYS
894fe884 amd64: treat RDSSPD/RDSSPQ (CET shadow-stack read) as a NOP, not SIGILL
be030bfb mem: fix 9 real Linux nonconformances found by differential vs mint
ed09c2e8 sockets: fix 9 real Linux nonconformances found by differential vs mint
08c9fe01 time: fix 7 real Linux nonconformances found by differential vs mint
208d3757 fs: return ENAMETOOLONG (not EFAULT) for over-PATH_MAX guest paths
5c8afcd0 process: fix 4 real Linux nonconformances found by differential vs mint
4afa463b app: fix generic_renameat callers for the new flags arg
e6f0ce15 fs: fix 9 real Linux VFS nonconformances found by differential vs mint
ad8cd963 signals: stop SIGCHLD carries CLD_STOPPED + stop signal, not SIGINFO_NIL
a659a41f signals: fix 5 real Linux nonconformances found by differential vs mint
b928a274 emu/cpuid.h: advertise cmpxchg16b (cx16) for the amd64 guest
aeb78409 docs: reframe the cmpxchg oracle-disagreement note (Rosetta isn't necessarily wrong)
a8951357 emu/amd64_interp.c: implement cmpxchg16b; add atomics corpus family
41cadcda mmap: implement MADV_DONTNEED (destructive zero of anonymous pages)
cb70ea19 tests/remote: sse_shuffle corpus family (lane shuffles + sign masks)
a8c0001e emu/vec.c: fix i386 SSE min/max +0/-0 (and NaN) tie-break; add sse_cvt corpus
50ebcc52 tests/remote: rep_string corpus family (movs/stos/cmps/scas)
e65b209d tests/remote: bit_ops corpus family + README refresh
c81d0b56 tests/remote: tier0 -- run the tests/manual self-check suite under iSH
7781cf47 tests/remote: mint x86_64-host i386-JIT cell (+ i386 div operand guard)
8778b4b1 jit: fix x86_64 (non-aarch64) build -- guard the amd64 JIT codegen
d46e414e tests/remote: device backend scaffold (ssh deploy/run + crash recovery)
f9b8a868 tests/remote: guest supervisor + journaled batch run (device crash attribution)
e39e722b emu/amd64: implement LDMXCSR/STMXCSR (0F AE /2,/3)
4945ae0b jit/amd64: native 2-op imul reg,rm (0F AF), fixing the f2cf6452 cache-dirty bug
b8aee24e jit/i386: raise #DE on divide-by-zero and idiv overflow
597e6858 jit/i386: fix 2-op imul CF/OF for 8/16-bit operands
63867f21 jit/amd64: native movzx/movsx with a memory operand (0F B6/B7/BE/BF)
9a6154d6 emu/amd64: fix 2-op imul CF/OF for 64-bit operands
ad18f22c emu/amd64: fix cbw sign-extension and 32-bit mul/imul high half
eaa6c25a tests/remote: differential JIT test harness (host + mint oracle)
dd09148e emu/amd64: fix shl/sar/rol/ror flag edge cases
187d755e jit/amd64: fix adc/sbb AF in the native memory-operand gadgets
2827afd1 emu/amd64: fix adc/sbb AF and OF flags with carry-in
e3bf3fc3 jit/amd64: don't force cc1 into the interpreter by default (~4x faster gcc/g++)
f1536990 tty: wake blocking pty readers/writers on slave close and hangup
8e516b64 emu/vec: return x86 integer-indefinite for out-of-range cvtt* conversions
7bbd5a50 exec,app: standardize guest TERM on screen-256color (vt102 exec-time fixup)
1bb718b8 emu/amd64: implement missing packed/scalar SSE floating-point ops
a0e20b8c emu/vec: fix shufps/shufpd in-place lane corruption
55b5d7bc jit/i386: implement packed double/single SSE2 ops and conversions
66a77d5f epoll: don't return spurious 0 from infinite epoll_wait
d569642f jit/i386: decode 0x66 operand-size prefix after rep/repnz
eb2d7612 jit/i386: page-batched rep movs/stos fast path
cdd136bf exec/amd64: load dynamic PIE at low base so the brk heap has room
9f05510f jit/amd64: native shift/rotate reg,CL (0xd3) and reg,1 (0xd1)
3e2b0459 jit/amd64: native cld/std (0xfc/0xfd) via the shared gadgets
370a6742 signal: make sigusr1_handler async-signal-safe vs lazy TLV malloc
c9c1b0c2 jit/amd64: native stc/clc/cmc (0xf8/0xf9/0xf5)
3314aecd jit/amd64: native inc/dec [mem] (0xff /0,/1, RMW, CF-preserving)
f8dab36e jit/amd64: native high-reg ADD/SUB/CMP reg,imm (0x81/0x83 /0,/5,/7)
633ee4ec jit/amd64: native high-reg ADD/SUB/CMP reg,reg -- ~1.8x more SHA-512
5607df33 jit/amd64: native high-reg SHL/SHR/SAR reg,imm8 (0xc1 /4,/5,/7)
7c067542 jit/amd64: native ROL/ROR reg,imm8 (0xc1 /0,/1) -- ~1.5x SHA-512/crypt
8cf6a918 jit/amd64: native SSE byte-scan gadgets (pcmpeqb/psubb/movd/pmovmskb)
d2eba2bf jit/amd64: native cmp r/m,imm (0x80/0x81/0x83 /7) -- crypt/login hot path
418d1eef emu/amd64: interp delegates missing SSE2 packed ops to the vec bridge
e4e7a03d jit/amd64: native imm-to-mem logic (or/and/xor [mem],imm, 0x81/0x83 /1,/4,/6)
7b73e05b jit/amd64: native imm-to-mem add/sub ([mem] op= imm, 0x81/0x83 /0,/5)
d0e8d144 jit/amd64: native adc/sbb with a memory operand (0x11/0x13/0x19/0x1b)
f5ddcafa fs/real: fix inflated statfs free space (f_bsize was Darwin iosize)
9cbdda49 jit/amd64: native MOVSXD reg64<-[mem]32 (movslq, 0x63 REX.W)
d9d42f8d jit/amd64: native op-store/RMW logic + TEST (or/and/xor [mem],reg, test [mem],reg)
af47f01d jit/amd64: native op-store/RMW arith (add/sub/cmp [mem],reg, 0x01/0x29/0x39)
aa8d445e jit/amd64: native load-op logic (or/and/xor reg,[mem], 0x0b/0x23/0x33)
0db15773 jit/amd64: native load-op arith (add/sub/cmp reg,[mem], 0x03/0x2b/0x3b)
e2ae8e3e kernel: amd64 pidfd_open/pidfd_send_signal/io_uring stubs -- no SIGSYS
f2713ebb jit/amd64: native MOV reg<->mem byte forms (0x88/0x8a, mod!=3)
d8996516 kernel: implement copy_file_range and sendfile (real data copy)
ea3c8cd0 kernel: amd64 sendfile (64-bit count) no SIGSYS; wire vhangup stub
4370b820 jit/amd64: native MOV reg<->mem, word forms (0x89/0x8b, mod!=3)
4592da7b kernel: amd64 name_to_handle_at/open_by_handle_at -> EOPNOTSUPP stub
55ddf2f0 kernel: amd64 copy_file_range -- don't SIGSYS on 64-bit len, return ENOSYS
fff46527 jit/amd64: native memory-indirect call/jmp (0xff /2,/4, mod!=3)
37bab16c jit/amd64: native register-indirect call/jmp (0xff /2,/4, mod==3)
cdc79bdc kernel/fs: handle AT_EMPTY_PATH in fchownat (fchown-by-fd)
269ce9a6 jit/amd64: native conditional jumps (jcc 0x70-0x7f, 0x0f 80-8f)
1fb0a1a9 jit/amd64: native ret (0xc3)
67d74b72 jit/amd64: native call rel32 (0xe8)
d001544f jit/amd64: native leave gadget (0xc9)
409bb0b0 jit/amd64: native push imm gadget (0x68/0x6a)
f28f8b1f jit/amd64: native push/pop reg gadgets (0x50-0x5f)
6766c227 fs: implement fcntl open file description locks (F_OFD_GETLK/SETLK/SETLKW)
98b30f88 jit/aarch64: fix amd64 cached_logic_reg_reg using clobbered size reg for flags
01bd6b92 jit/amd64: per-instruction-range force-interp for exact-gadget bisection
b708ed94 fs: fix amd64 fcntl POSIX-lock marshalling, F_SETLKW64 blocking, exit wakeup
a8205693 jit/amd64: env-driven RIP-range force-interp for bisecting JIT bugs
e9644dcd tools/ptraceomatic: amd64 guest support (single-step + register compare)
c26782d7 fs/proc/net: Linux compat shim for BSD if_data/AF_LINK
9d213784 fs/proc: guard remaining net/if_var.h includes for Linux
62e22a4a Restore Linux buildability: guard macOS-only code, add missing includes
2b2fc580 main: default amd64 JIT off on non-aarch64 CLI hosts
9199012b fs/sock.c: include <signal.h> for sigemptyset/sigaddset
f2cf6452 jit/amd64: keep imul reg,rm (0F AF) bridged — imul_reg gadget has a latent bug
0a2afd1a jit/amd64: native imul reg,rm,imm gadget (69/6B reg form)
0d2eec77 jit/amd64: route accumulator-imm ALU ops to native gadgets (no bridge)
567fc966 mmap: ignore P_COW in mremap uniform-flags check (the real apt update fix)
1a20f385 fakefs: locale-independent UTF-8 archive import
5c51f043 ios: single Files provider domain, cached-roots install, host-file provisioning
793262a7 mmap: support mremap grow of file-backed mappings
3c874ac3 amd64: fix 64-bit pointer/arg truncation across syscalls
97f40635 amd64: fix sysinfo(2) struct layout
570cc949 jit/amd64: native pxor reg-reg (66 0F EF)
4d907c9e jit/amd64: native movdqa/movdqu 128-bit memory load/store (66/F3 0F 6F/7F)
88c069e6 amd64: enable the host JIT by default
d5ddcc45 jit/amd64: native SSE 128-bit memory store (0F 29 movaps / 0F 11 movups)
5634626a jit/amd64: native SSE 128-bit memory load (0F 28 movaps / 0F 10 movups)
cf867b45 jit/amd64: native cvtsi2sd reg-reg gadget (F2 0F 2A) — bmmc64 now 100% native
6f3d4ece jit/amd64: native addsd/mulsd reg-reg gadgets (F2 0F 58/59)
561f4a93 amd64: fix fchmod (91) arg-count + add fadvise64 (221) stub
c6914b3b jit/amd64: native psllq/psrlq imm gadgets (66 0F 73 /6,/2)
f9aca86c jit/amd64: native xorps/xorpd reg-reg gadget (0F 57)
d63174ad jit/amd64: native psubq reg-reg gadget (66 0F FB)
c83a35df jit/amd64: native paddq reg-reg gadget (66 0F D4) + lane64 binop macro
e0af3d75 jit/amd64: native movdqa reg-reg gadget (66 0F 6F) — first SSE bridge eliminated
d898ecf4 jit/amd64: strip per-block JIT overhead — gate the varargs debug call + pre-filter trace strcmps
2187d707 amd64 JIT: instrument the JIT->interp transitions (fallback + vector-bridge histograms)
4fd5b645 emu/amd64: pre-filter the per-instruction cc1-trace strcmp in the interpreter
99ed31c3 kernel/abi: scalar guest_abi_user_addr_max to skip struct-by-value on the hot validation path
401a16f1 emu/amd64: implement SSE4.1 pmulld and pcmpeqq in the interpreter
d387aee4 kernel/task: drop the CAS retry loop in modify_locks_held_count (per-syscall lock overhead)
70a94c87 emu/amd64: implement SSE4.1 blendvps/blendvpd in the interpreter
e34a0458 Track app/DiagnosticsBridge.h (was silently gitignored)
598fb293 emu/amd64: implement SSSE3 pshufb and SSE4.1 pblendvb in the interpreter
1fc4dc22 emu/amd64: implement SSE4.1/4.2 pmovsx/pmovzx, popcnt, ptest in the interpreter
d46ede6d jit: drop per-syscall signal-mask save in JIT crash-recovery sigsetjmp (~35% of syscall overhead)
22be4736 kernel/exit: CLI mirrors init's exit status instead of self-SIGKILL (fixes 137)
fc5a9bcb emu/amd64: implement LFENCE/MFENCE/SFENCE (0F AE /5,/6,/7)
cbffec34 jit: skip the per-block cpu_state snapshot unless the cc1 trace is on
90cbe3c6 emu/amd64: implement FWAIT/WAIT (0x9b) as a no-op
0dfc051a emu/memory: bound the page-directory scan by page_limit (1.7x faster fork)
b37eb673 app: boot/init console TERM=linux so checkroot setterm --msg works
09d47ade fs/fake: back initctl with the real FIFO, not the read=0/write=discard stub
d8d30aab fs: real FIFO (named pipe) support for tmpfs; fixes telinit SIGABRT
870c7df8 fs/tty, Terminal: batch canonical echo and drop throwaway NSData
e2660c47 fs/proc: report mounted named cgroup hierarchies in /proc/<pid>/cgroup
d95f5738 kernel: report the guest load average in /proc/loadavg, not the host's
5f8c4775 app/Roots: reject filesystem names with spaces or unsafe characters
e4986c94 fs/tty: accept TIOCCONS as a no-op instead of ENOTTY
8fc0e741 fs/proc: synthesize console= on /proc/cmdline so tools can find the console
bb51bde3 fs/tmp: implement symlinks in tmpfs
e473463a jit/gen: raise #UD instead of aborting on an unimplemented vector op
b95ce562 fs/real: return EINVAL instead of aborting on unknown attr type
5df68fc5 fs/tmp: implement tmpfs_umount instead of aborting
a072ef36 fs/tmp: implement fsetattr so fd-based fchmod/fchown work on tmpfs
84ef2f81 build: bump project version to 531; scheme env tweaks; x86_64 boot banner
504dff2d fs/aok: generate the /AOK/tests table at build time from a manifest
2733862d fs/poll: EPOLLONESHOT disarms (not unregisters) the fd, so MOD can re-arm it
78c147a8 kernel/exit: don't re-report PTRACE_EVENT_EXIT on re-entry (fixes crash)
3e5903a0 fs/poll: wake a blocked epoll_wait when an emulated fd is armed via ADD/MOD
60c1988f kernel/exit: report traced thread (non-leader) stops to the tracer under __WALL
39639e8b kernel/ptrace: deliver injected signals once, report group-stops to the tracer
cbbfd835 fs/sock: let a real syslogd receive on /dev/log
0abf3a9d fs: don't hold inodes_lock across a blocking FIFO open
b2a17b96 build: silence ~550 compiler warnings (clang)
9770b8fb fs: fix fcntl F_GETLK error check against unsigned return
