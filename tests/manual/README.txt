Focused iSH-AOK guest regression suite

These sources are exposed inside the guest at /AOK/tests.

Quick start:
  sh /AOK/tests/setup-regressions.sh --install-deps --run

If a C toolchain is already present:
  sh /AOK/tests/setup-regressions.sh --run

Verbose mode:
  sh /AOK/tests/setup-regressions.sh --run -v

Simple amd64 JIT timing benchmark inside the guest:
  sh /AOK/tests/x86/amd64_jit_guest_bench.sh
  For short commands, use -n to amplify timing differences, e.g.:
  sh /AOK/tests/x86/amd64_jit_guest_bench.sh -n 5

Host-side amd64 GAS encoding probe:
  tests/manual/x86/amd64_gas_probe.sh -r /path/to/amd64-root-with-binutils
  This compares suspect GNU as mnemonic encodings against paired .byte
  encodings and falls back to one-file-per-case when as aborts early.

Layout:
  *.c                  Portable tests, built and run on every guest arch.
  x86/                 x86-only tests (lock-prefixed inline asm, amd64 JIT
                       benchmarks/probes). Built on i386/x86_64 guests.
  arm64/               AArch64-only tests. Built on aarch64 guests.

Focused tests (x86/, i386 + x86_64 guests):
  amd64_regress.c      amd64 cross-page write, exec loader, fcntl race, and cc1 stress
  atomics32.c          Combined atomic probe with single-case and stress checks
  atomic_xadd32.c      lock xaddl coverage
  atomic_cmpxchg32.c   lock cmpxchgl coverage
  atomic_cmpxchg8b.c   lock cmpxchg8b coverage
  atomic_logic32.c     lock orl/andl/xorl coverage

Focused tests (arm64/, aarch64 guests):
  atomics64.c          LDXR/STXR + LDAXR/STLXR exclusives (8-64 bit), CLREX,
                       LDAR/STLR, __atomic builtins, HWCAP-gated LSE, and a
                       multithreaded stress mix (fetch_add/or, cmpxchg retry,
                       acquire/release message passing)
  arm64_regress.c      One check per real arm64-JIT bug class: ldp32 upper-half
                       zeroing, EOR/BSL vector decode, >1-page straight-line
                       blocks, 48-bit TLB aliasing, high-pointer syscall args,
                       raw-brk heap growth, MRS (CNTVCT/CNTFRQ/NZCV/ID regs),
                       CRC32 known-answer, FRECPE/CMxx-zero scalars
  vector_smoke.c       NEON intrinsics vs volatile scalar reference loops:
                       three-same int, saturating, pairwise, across-lanes,
                       widening/narrowing, shifts, permute (zip/uzp/ext/tbl),
                       two-reg misc, FP arithmetic/compares/converts/FMA
  ands_bcond_fusion.c  ANDS+B.cond gadget fusion (jit/guest-arm64/control.S
                       fused_andsi/fused_andsr): all 14 conditions x
                       {imm,reg} x {32,64-bit} x {TST,normal-Rd}, checking
                       branch-taken outcome against the real ARM64
                       condition truth table (with C=V=0 forced, per ANDS),
                       NZCV correctness, result-register write (skipped for
                       TST/Rd=31), and scratch-register leakage via
                       canaries. Portable host-native AArch64 -- can be
                       compiled and run directly on an Apple Silicon Mac to
                       validate the test itself before running it under the
                       JIT. Compare against ISH_ARM64_NO_FUSE=1 to isolate
                       a failure to the fused gadgets specifically.

Portable focused tests (all guest arches):
  signal_core.c        Core signal delivery, wait, and signalfd coverage
  signal_restart.c     SA_RESTART behavior for read and waitpid
  signal_realtime.c    sigqueue and realtime queued-signal coverage
  signal_altstack.c    sigaltstack and SA_ONSTACK coverage
  signal_poll.c        poll/select/pselect signal interruption coverage
  signal_child_burst.c A shell reaping a burst of near-simultaneous child exits
                       (SIGCHLD) must never get stuck forever in sigsuspend()
  eventfd_interrupt.c  eventfd read/poll interruption via the generic wait path
  timerfd_settime_readiness.c
                       timerfd_settime resets the expiration counter (disarm/
                       re-arm clears poll/epoll readiness; libwayland timer-heap
                       pattern that spun labwc's event loop)
  pixman_accel.c       ISH_SYS_PIXOP (0xacc1) pixman accelerator differential
                       test: FILL/COPY/OVER bit-exact vs real pixman (dlopen'd
                       oracle) across tight/offset/padded/multi-page/full-frame
                       geometries + fuzz; decline paths (overlap, oversized,
                       misaligned stride). SKIPs if no libpixman-1 on the
                       rootfs, or if built/run without the accelerator (needs
                       ISH_PIX_ACCEL=1)
  futex_core.c         FUTEX_WAIT/FUTEX_WAKE timeout, wake, and signal coverage
  process_lifecycle.c  fork/exec/vfork/wait and signal inheritance coverage
  pthread_sync.c       mutex/condvar/rwlock/timed wait and pthread_once coverage

All focused tests accept -v or --verbose. Without it they print only failures
plus the final PASS/FAIL line for each test.
