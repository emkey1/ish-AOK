# Release Notes Since `builds/iSH-AOK_544`

Forty-seven commits, and the centre of gravity is the x86 CPU itself. The bulk of
the cycle goes to closing the AVX gap reported as GH #525 -- VEX and EVEX
decoding, AVX2, AVX-512 opmasks and predication, BMI1/BMI2, AES/CLMUL/GFNI/VNNI
and FMA on the amd64 guest, plus VEX for the 32-bit guest, which is JIT-only and
had to decode it at codegen time. Around that sits a batch of x87 correctness
work, three decode-table holes on i386 that had been raising SIGILL on ordinary
instructions since forever, and the first real permission enforcement the
filesystem layer has ever had. One memory-management change is quietly the most
consequential in the release: iSH no longer auto-maps the NULL page, so a guest
NULL dereference finally faults instead of silently reading zero.

## Highlights

- **The AVX gap is closed (GH #525).** The reported binary went from unusable to
  100% covered, in stages: VEX/EVEX decode and core AVX2 (`3752346e`), shifts,
  shuffles, packs and permutes (`1419d254`), AES/CLMUL/GFNI/VNNI/FMA and
  packed/scalar FP (`506881c4`), BMI1/BMI2 (`a36a1731`), AVX-512 opmask registers
  and EVEX predication (`4911c298`), registers 16-31 with compare-to-mask
  (`d644e626`), then the long tail from 93.9% to 99.9% (`1b313cb6`) and finally
  to 100% (`18974749`). The wide-vector register state landed first
  (`2febb587`), and the instruction semantics were split into an
  arch-independent module (`a79e9c0b`) so the 32-bit guest could reuse them.
- **AVX/VEX for the i386 guest** (`48066c26`). i386 is JIT-only -- there is no
  interpreter to bail to -- so VEX is decoded at codegen time and executed
  through the vec-helper gadgets. That made a different set of cases interesting
  than on amd64: 256-bit memory operands (new `vec_helper_read256`/`write256`
  instantiations on both host backends), `VZEROUPPER`, which has no ModRM byte
  at all and desynchronizes the whole decoder if you consume one for it, and a
  GPR destination for `VPMOVMSKB`, where the helper's destination pointer aims
  into `cpu->xmm[]` and the runtime has to notice and write `cpu->regs[]`.
- **x87 correctness.** `fsincos` was missing from both the i386 decode table and
  the amd64 interpreter while every neighbouring `D9 F8..FF` opcode was present
  (`a122473b`); `fnstsw` to memory was likewise absent though `fnstsw ax` was
  there (`57badd19`). Precision control was parsed out of the control word and
  then never read by anything, so everything ran at a 64-bit significand and
  glibc's 53-bit `sin`/`cos` came back wrong in the low digits (`c891f92a`).
  Worst of the batch: **`FCMOVcc` read `cpu->zf`/`cpu->pf` directly instead of
  through the ZF/PF macros** (`9c67c0fd`) -- those are lazy, so while the
  producing instruction's result still sits in `cpu->res` the raw fields hold
  whatever was last materialized. glibc applies `sin`'s sign with
  `and $0x2,%ecx; fchs; fcmove %st(1),%st`, an `fcmov` on a ZF set one
  instruction earlier, so results came back with exactly the right magnitude and
  the wrong sign. `d80d6077` adds the PE exception flag and the C1 rounding
  indicator.
- **Three i386 decode-table holes.** `LOOP` (`51ce33d1`) and `LOOPE`/`LOOPNE`
  (`ff3f66e1`) were absent although `jcxz` was present, and the six packed and
  unpacked BCD adjusts -- `aaa`, `aas`, `daa`, `das`, `aam`, `aad` -- were absent
  entirely (`a9a462a9`). All of them raised SIGILL on real programs. Two details
  differential testing caught that the SDM's pseudo-code gets wrong: `aaa`/`aas`
  adjust `AX`, not `AL`, so writing them as two 8-bit steps loses the carry out
  of AL; and the flags the SDM calls "undefined" are set anyway on real silicon.
- **A guest NULL dereference now faults** (`d5dece75`). `mem_ptr()` extended a
  growsdown region onto an unmapped page whenever the next mapped page anywhere
  above it happened to be growsdown -- with no distance bound and no
  `mmap_min_addr` floor, both of which Linux's `expand_downwards()` has. In every
  64-bit guest layout the stack is the *lowest* mapping in the address space, so
  the scan reached it from anywhere below and every address under it, the whole
  NULL page included, silently allocated a zero-filled page. A guest NULL read
  returned 0 and a guest NULL store *succeeded*. i386 was accidentally immune.
  This is a large correctness win, and it has a visible consequence: latent NULL
  dereferences in guest software that used to limp along now produce a proper
  SIGSEGV. See Known Issues.
- **A deadlock that wedged the entire emulator.** `pidfd_poll()` takes
  `pids_lock`, and `poll_scan_ready_locked()` calls it while holding
  `poll->lock`; meanwhile `do_exit()` held `pids_lock` and called
  `poll_wakeup()`, which wants `poll->lock`. Opposite orders on the same pair,
  so an ordinary sequence -- a process exiting while a sibling sits in
  `epoll_wait` on a pidfd for it -- could stop the whole guest (`0b738e76`).
  This is what `fs/poll.h` forbids in as many words, and the same bug the
  signalfd path had with `sighand->lock`; `poll_wakeup_trylock()` already
  existed for it and pidfd simply was not using it. Found only because the
  device seized during the release sweep: around thirty threads were stacked on
  `pids_lock` -- systemd, dbus-broker, labwc, sshd, the app's own UI thread --
  and it had to be read out of an lldb backtrace, because nothing can report a
  test result once scheduling stops.
- **The filesystem enforces permissions.** Neither `generic_setattrat` nor
  `generic_fsetattr` checked anything, so any process could chmod, chown or
  truncate a file it did not own (`4f4b0f01`, upstream ish-app/ish#2197).
  Separately the sticky bit was unenforced, so an unprivileged process could
  unlink or rename another user's files out of a world-writable `/tmp` -- the
  entire reason a sticky `/tmp` is safe (`8360c379`).

## User-Facing Changes

### Emulation

- AVX, AVX2, AVX-512, BMI1/BMI2, AES, CLMUL, GFNI, VNNI and FMA on the amd64
  guest; AVX/VEX on the i386 guest.
- CPUID leaf 7 and leaf 0x0D (XSAVE state enumeration), and XGETBV on both x86
  guests (`5d49de54`, `84d5633c`). This is groundwork for making the vector
  work reachable by feature detection and is deliberately **inert** in this
  release -- see Known Issues. JIT-side only: the amd64 interpreter is being
  retired, and implementing it there would have made XGETBV work today solely
  through the interpreter fallback and then vanish with it.
- x87: `fsincos`, `fnstsw` to memory, precision control, correct `FCMOVcc`
  condition evaluation, `fsin`/`fcos` C2 semantics, PE and C1 reporting.
- i386: `LOOP`, `LOOPE`/`LOOPZ`, `LOOPNE`/`LOOPNZ`, and the six BCD adjusts.
- A JIT gadget fix so `loope`/`loopne` test ZF through `do_jump` rather than
  reading the eflags bit directly, which is wrong whenever ZF is still deferred
  (`2583d324`).

### Filesystem and kernel

- `truncate(2)` returned EBADF on *every* ABI, because `sys_truncate64_guest`
  passed a NULL dirfd to `generic_setattrat`, which since the AT_PWD work means
  "bad dirfd"; i386 syscall 92 was also unwired (`4d8a6a18`).
- `adjtimex` was wired only into the asm-generic table; i386 [124] and amd64
  [159] were both missing (`295d1a27`, upstream ish-app/ish#1322).
- `/proc/PID/fd/N` now resolves through symlink indirection, which fixes
  `/dev/stdin` (`5221d61f`).
- A `poll_fds` list read raced a concurrent fd close (`717e6d3d`).
- A 32-bit syscall argument's upper half can hold legal junk, which was being
  turned into SIGSYS (`8a7a067d`).
- **`ftruncate` takes its permission from the descriptor, not the inode**
  (`f7d16937`) -- see Validation, this was found during the release sweep.
- A fresh pty no longer inherits packet mode from a recycled `struct tty`
  (`fe4705c3`).
- `fs/sock` warns when a wildcard privileged-port bind silently downgrades to
  loopback-only (`f955e990`).
- **`pidfd_notify_exit` no longer takes `poll->lock` under `pids_lock`**
  (`0b738e76`), the whole-emulator deadlock above, with a deliberate repro in
  `pidfd_epoll_deadlock.c` (`db68d942`).

### App

- The Display applet tolerates a resize settling across more than one update
  (`d44f5971`) and gained a Hide Keyboard action in the standalone menu pip
  (`5f217088`).
- The app no longer aborts when the roots directory is unavailable (`bf495e0e`).
- The amd64 JIT Settings toggle is retired (`21180ab9`).
- `ptraceomatic` reports why a ptrace request failed (`0d8b4704`).
- `setup-wayland.sh` installs the pixman development headers the accelerator
  shim needs (`47703fce`). It checked for a compiler but not for `pixman.h`, so
  on a fresh rootfs the shim build died after the one check it made had already
  passed -- and the no-compiler message sent you straight into that second
  error. Reported from a Devuan arm64 install.

## Validation

Full sweep for this release. Everything below was run against a `-O2`,
logging-disabled build, which is closer to what ships than the default dev
configuration.

### Guest regression suites

All four guest architectures were run concurrently from one host against the
same binary, then the device separately.

| Target | PASS | FAIL | SKIP |
|---|---|---|---|
| i386 (alpinei386) | 101 | **0** | 2 |
| amd64 (alpine64) | 101 | **0** | 4 |
| arm64 (alpine-arm64-test) | 100 | **0** | 2 |
| riscv64 (alpine-riscv64-test) | 93 | **0** | 2 |
| Device, native Arch Linux ARM aarch64, as root | 99 | 1 (flake) | 1 |

The skips are legitimate: `pixman_accel` skips unless the accelerator is built
in and `ISH_PIX_ACCEL=1` is set, and `bcd_adjust` skips on amd64 because the BCD
adjusts are invalid in 64-bit mode. Re-run with the accelerator enabled,
`pixman_accel` passes on i386, amd64 and arm64.

The device run was made from the sources shipped in the app bundle rather than a
copy, which also verified the manifest fix below end to end.

The device's one failure is a flake, not a regression: `ptrace_group_stop` timed
out once inside the full-suite run, then passed 3/3 run on its own on the same
device and same build, and passes on all four CLI architectures. An earlier
device run this cycle -- same code apart from `fe4705c3` -- was 100 PASS / 0 FAIL
with that test passing. It is timing sensitivity under suite load, in the same
family as `getrusage_group` below.

### CPU work against real silicon

The point of the mint oracle is that a test proves something only if it also
passes on real hardware. `x87_fpu`, `x86_loop`, `bcd_adjust` and `avx32_smoke`
were each built as static i386 musl binaries *inside* the alpinei386 guest and
then that same binary was run on the oracle's real Intel silicon: all four pass
there and under iSH, so the expectations are hardware truth rather than
self-fulfilling.

`avx_regress` passes in full on the amd64 guest, and was additionally built as a
static amd64 binary in the guest and run on the oracle: **52 checks pass, 0 fail
on real Intel silicon**, after which it takes SIGILL at the first AVX-512
instruction because the oracle has no AVX-512. So the AVX/AVX2 half of that test
is now hardware-validated; the AVX-512 third is not, and cannot be here. See
Known Issues.

### Bugs found and fixed during the sweep

- **A whole-emulator deadlock, found on device** (`0b738e76`). The pidfd/epoll
  lock inversion described above. It is worth being precise about how this was
  caught, because it is the argument for running the device leg at all: the CLI
  suite was green on all four architectures and would have shipped it. The
  device wedged mid-run instead -- unresponsive to ssh and in the UI -- and the
  cycle was read out of an lldb thread backtrace. A/B afterwards was
  unambiguous: `tests/manual/pidfd_epoll_deadlock.c` hangs to its watchdog
  (`rc=142`, SIGALRM) on the unfixed build and passes on the fixed one, and the
  device gave an accidental A/B of its own -- the build from before the fix
  seized part-way through the suite, the build from after it completed all 101
  tests under the same load.

- **`ftruncate` returned EACCES through a writable descriptor** (`f7d16937`).
  The permission enforcement added this cycle ran the *inode* check on the
  fd-based `ftruncate`, where Linux checks only that the file was opened for
  writing. This broke the Wayland desktop outright: wlroots allocates every
  keymap by creating a `/dev/shm` file 0600, reopening it read-only,
  `fchmod()`ing it to 000 so nobody can reopen it, then `ftruncate()`ing through
  the descriptor it still holds. That returned EACCES, so the keyboard kept a
  NULL keymap and labwc then called `xkb_state_get_keymap(NULL)` and took
  SIGSEGV before ever creating its Wayland socket. Traced on device with
  `strace`; fixed, and covered by the new `ftruncate_fd_mode` test. Confirmed
  fixed end to end on device: labwc now starts, and `start-wayland.sh` reaches
  `READY 5901` with foot on the first attempt.
- **Tests added this cycle were not shipped to the device** (`9faa14e1`).
  `file_perms.c`, `syscall_wiring.c` and the three x86 CPU tests were wired into
  the runner but never added to `fs/aok-tests.manifest`, so the on-device suite
  hit `cc1: fatal error: /AOK/tests/file_perms.c: No such file or directory` and,
  because the build loop aborts, everything after it never ran either. The
  device suite was unusable on that build, not merely incomplete. All 113 tests
  in `all_tests` now resolve to a manifest entry, and the check is one line of
  Python worth re-running when tests are added.
- **`avx32_smoke` was never run** (`6006b92c`). The i386 half of the AVX work had
  a dedicated test that appeared nowhere in `setup-regressions.sh`; its header
  documented a manual build recipe instead. Now wired in for the i386 guest,
  with the freestanding build line it needs.
- **A test asserted something untrue on 32-bit musl** (`6a00683c`).
  `syscall_wiring` checked `timex.tick != 0` after a raw `adjtimex`, which fails
  on any 32-bit ABI whose libc has a 64-bit `time_t`, because libc's `struct
  timex` is then the 64-bit layout while raw syscall 124 is the legacy one.
  Confirmed a libc property and not an emulator bug by running the identical
  static i386 binary on the oracle's real x86_64 kernel, where it fails the same
  assertion.

## Known Issues

- **CPUID advertises none of the new vector support**, still. The enumeration
  itself now exists -- leaf 7, leaf 0x0D and XGETBV all landed this cycle
  (`5d49de54`, `84d5633c`) -- but the advertisement is held behind
  `CPUID_ADVERTISE_VECTOR_STATE`, which is 0. Leaf 1 ECX is byte-identical to
  544 (`0x00980201`) and leaf 7 reads zero, so nothing a guest can observe
  through feature detection has changed. That is deliberate, for the reason
  below. Before this cycle there was no leaf 7 at all and leaf 1 reported AVX,
  XSAVE and OSXSAVE clear. Measured in-guest
  against real Intel, which reports `avx=1 xsave=1 osxsave=1` and a populated
  leaf 7. This is defensible rather than an oversight: `XSAVE`/`XGETBV`/`XCR0`
  are not implemented at all (only legacy `FXSAVE` is), and glibc gates its AVX
  paths on OSXSAVE plus an `XGETBV` check, so advertising AVX without them would
  be a lie. The consequence is worth stating plainly: software that
  runtime-dispatches on CPUID -- glibc string ifuncs, codecs, OpenSSL, numpy --
  will not select the new AVX paths. The new code is reached by binaries that
  use those instructions unconditionally, which is exactly the GH #525 case.

  There is a second, harder reason not to just flip the bit, recorded at
  `emu/cpu.h:437`: the wide state (`xmm_ext`, `ymm_hi`, `zmm_hi`, `avx512_k`) is
  deliberately not threaded through ptrace GETFPREGS, signal-frame construction
  or clone. `kernel/signal.c`'s `amd64_fpstate_` is `static_assert`ed to exactly
  512 bytes -- the legacy FXSAVE layout, which carries only the low 128 bits of
  `xmm[0-15]`. Today that is the "narrow, known gap" the comment describes,
  because little uses AVX. Advertise AVX and it stops being narrow: glibc would
  start selecting AVX string routines, those run inside signal handlers too, and
  a handler clobbering `ymm_hi` has nowhere to restore it from, because it was
  never saved. Real Linux avoids this by saving YMM/ZMM in an XSAVE-format
  fpstate. So the order of work is: XSAVE/XGETBV plus leaf 0xD state enumeration,
  extend the signal frame and ptrace regsets, *then* advertise leaf 7 and AVX.
  Flipping the CPUID bits first would trade a dormant gap for silent register
  corruption in any program that takes a signal mid-AVX.
- **The AVX-512 implementation has not been validated against real silicon.**
  The available oracle is a Coffee Lake i9-9980HK: AVX, AVX2, BMI1/2, FMA, AES
  and PCLMULQDQ, but no AVX-512 -- and no GFNI, VNNI or VAES either, so
  `506881c4`'s GFNI/VNNI work is equally unattested. Of `avx_regress`'s 30 test
  functions and ~106 assertion sites, six functions and ~38 sites (about a third)
  sit behind `__attribute__((target("avx512...")))` with no runtime CPUID check,
  so on the oracle the binary simply dies: 52 checks pass, then SIGILL. Those
  sections are validated against the SDM and the GH #525 target binary only.

  The obvious repair -- gate them on `__builtin_cpu_supports("avx512f")` so the
  test degrades instead of trapping -- does not work while the CPUID issue above
  stands, and the two are coupled. glibc's `__cpu_indicator_init` checks the
  maximum basic leaf before querying leaf 7; iSH reports 1, so it never queries,
  and `avx512f` reads false. The gate would therefore skip the AVX-512 tests
  *under iSH*, which is precisely where they need to run. Until CPUID grows leaf
  7, a runtime gate has to key off something else (an env var, or a SIGILL probe).
- **Latent guest NULL dereferences now crash.** This is the intended consequence
  of `d5dece75`, but it means a guest program that used to appear to work can now
  die with SIGSEGV at a small address. Worth noting how both such crashes so far
  have ended: the foot/pixman one that motivated the change, and the labwc one
  found in this sweep, each looked like a guest bug and each turned out to be an
  iSH bug once the NULL was traced back to where it came from. Treat a new
  `si_addr` near zero as a lead worth pulling, not as somebody else's problem.
- **Two tests are load-sensitive and can fail spuriously in a full run.**
  `getrusage_group` compares process cputime against `RUSAGE_SELF` and can fail
  when all four architectures' suites share one host; it passes when its own
  suite runs alone. `ptrace_group_stop` timed out once in the device suite and
  then passed 3/3 in isolation on the same build. Neither is a regression, but
  both mean a single red line in a sweep is worth re-running on its own before
  it is believed. `ISH_TEST_WATCHDOG_SCALE` widens every watchdog for exactly
  this case and was set to 4 for the concurrent sweep.
- The `float80` unit test fails on Apple Silicon by design -- it uses
  `long double` arithmetic as its oracle, and `long double` is 64-bit there. It
  is meaningful only on the x86_64 CI leg, which is where CI runs it.
- The local `e2e` suite fails for an environmental reason, not an emulator one:
  its setup step installs `build-base` and `python2`/`python3` into the test
  filesystem it creates, but that step is skipped when `e2e_out/testfs` already
  exists. The reused one here predates it, so every toolchain-dependent case
  (`fpu`, `hello`, `net_regress`) reports `gcc: not found`. Delete
  `e2e_out/testfs` to force a fresh setup. Note that CI does not cover this: the
  ci.yml runs on the branch are all upstream PR branches, so nothing gates a
  push to `working`.
- `tools/fakefsify` will not link on an arm64 Mac whose Homebrew `libarchive` is
  the x86_64 one. The `ish` target itself is unaffected; build it directly.

## Maintainer Notes

- **Run the on-device suite from a filesystem that is not tmpfs.** `proc_pid_io`,
  `taskstats_genl` and `fifo_open_creat_deadlock` all fail from a `/tmp` cwd --
  the first two because `write_bytes` only counts realfs/fakefs as "disk", the
  third because the FIFO has to be on fakefs to exercise the case. All three pass
  from `/home/mke`. This cost a full device cycle to rediscover.
- **Keep the device awake for the whole device run.** iOS suspends the app when
  the screen locks, which kills the suite wherever it happens to be -- one run
  this cycle died at 99 of 113 tests built, with nothing in the log to say why
  beyond it simply stopping. The log lives on the real filesystem and survives,
  but `/tmp` does not: a suspend/relaunch cycle clears it, so write run logs and
  `ISH_AOK_REGRESS_DIR` somewhere under `$HOME`, not `/tmp`, or a completed run
  can vanish before it is read.
- **Adding a test means touching three places**, not one: the source, the
  `all_tests` list in `setup-regressions.sh`, and `fs/aok-tests.manifest`. Miss
  the third and it works on the CLI and hard-fails the whole device run. All 113
  tests in `all_tests` now resolve to a manifest entry.
- The `ISH_REAL_MNT` harness still avoids any tarball/fakefsify cycle:
  ```
  ISH_REAL_MNT=$PWD/tests/manual ./build-rel/ish -f build/<root> /bin/sh -c \
    'sh /realmnt/setup-regressions.sh --src /realmnt --run'
  ```
  Add `--only <name>` for a single test.
- The amd64 suite takes markedly longer than the others to build, almost all of
  it compiling `avx_regress.c` under emulation. Budget for it when running the
  four concurrently.
- Device rig: `ssh -p 1022 m4pt`, passwordless sudo. Build and install with
  `xcodebuild -scheme iSH -configuration Release -destination 'id=<udid>'` then
  `xcrun devicectl device install app`; `xcrun devicectl list devices` shows the
  udid.

## Commit Range

`builds/iSH-AOK_544..builds/iSH-AOK_545`

```
db68d942 tests: cover the pidfd/epoll AB-BA deadlock
0b738e76 kernel/pidfd: don't take poll->lock while holding pids_lock
84d5633c emu/amd64: move XGETBV off the interpreter and into the JIT
47703fce tools/setup-wayland: install the pixman headers the shim needs
5d49de54 emu: implement the CPUID and XGETBV half of AVX feature reporting
0219dfe5 docs: add iSH-AOK 545 release notes
fe4705c3 tty: a fresh pty must not inherit packet mode from a recycled struct
f88c116f build: bump project version to 545
f7d16937 fs: ftruncate takes its permission from the fd, not the inode
9faa14e1 fs: ship the tests added this cycle to the device
6006b92c tests: run avx32_smoke from the regression suite
6a00683c tests/syscall_wiring: don't assert timex fields when libc's layout differs
bf495e0e app/Roots: don't abort the app when the roots directory is unavailable
0d8b4704 tools/ptraceomatic: report why a ptrace request failed
d80d6077 emu/fpu: report the PE exception flag and the C1 rounding indicator
18974749 emu/amd64: complete AVX coverage of the GH #525 binary (100%)
a0578993 tests: add regression coverage for the x87, loop, BCD, perms and wiring fixes
1b313cb6 emu/amd64: close the AVX long tail -- 93.9% -> 99.9% of the GH #525 binary
d644e626 emu/amd64: AVX-512 registers 16-31, compare-to-mask, and a RIP-relative fix
4911c298 emu/amd64: implement AVX-512 opmask registers and EVEX predication (GH #525)
48066c26 i386: implement AVX/VEX for the 32-bit guest (GH #525)
a79e9c0b emu/avx: split AVX instruction semantics into an arch-independent module
2583d324 jit/amd64: test ZF through do_jump in the loope/loopne gadget
9c67c0fd emu/fpu: read the real flag values in fcmovcc
c891f92a emu/fpu: honour x87 precision control
ff3f66e1 emu: implement the i386 LOOPE/LOOPZ and LOOPNE/LOOPNZ instructions
a36a1731 emu/amd64: implement BMI1/BMI2 and the F3 0F 7E movq form (GH #525)
57badd19 emu/fpu: implement fnstsw to memory (DD /7)
506881c4 emu/amd64: add AES, CLMUL, GFNI, VNNI, FMA and packed/scalar FP (GH #525)
1419d254 emu/amd64: expand AVX2 coverage to shifts, shuffles, packs and permutes (GH #525)
3752346e emu/amd64: decode VEX/EVEX and implement core AVX2 instructions (GH #525)
8360c379 fs: enforce the sticky bit on unlink, rmdir and rename
4f4b0f01 fs: enforce ownership on chmod/chown and write permission on truncate
2febb587 emu/cpu: add AVX/AVX-512 wide-vector register storage (GH #525)
a9a462a9 emu: implement the i386 packed-decimal instructions
51ce33d1 emu: implement the i386 LOOP instruction
5221d61f fs: resolve /proc/PID/fd/N through symlink indirection (fixes /dev/stdin)
a122473b emu/fpu: implement fsincos, and give fsin/fcos their C2 semantics
295d1a27 kernel: wire adjtimex into the i386 and amd64 syscall tables
4d8a6a18 fs: fix truncate(2) returning EBADF on every ABI, and wire i386 syscall 92
5f217088 app/Display: add a Hide Keyboard action to the standalone menu pip
d44f5971 app/Display: tolerate a resize settling across more than one update
f955e990 fs/sock: warn when a wildcard privileged-port bind silently downgrades to loopback-only
21180ab9 app: retire the amd64 JIT Settings toggle
717e6d3d fs/poll: fix unsynchronized poll_fds list read racing concurrent fd close
8a7a067d kernel: don't SIGSYS legal junk in the upper half of a 32-bit syscall arg
d5dece75 mm: stop auto-mapping the NULL page via unbounded MAP_GROWSDOWN growth
```
