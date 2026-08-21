# iSH-AOK Remote Differential Test Harness

A crash-resilient, differential test harness for the iSH-AOK x86 emulator. It
runs each test across every **(arch × engine)** the emulator offers, plus a
**native x86 oracle**, and flags any divergence or crash — then minimizes it to
a repro. Built to push the edges of the 32- and 64-bit JIT.

## Status

| Piece | State |
|---|---|
| `conductor.py` — 5 modes: `run` / `supervise` / `device` / `tier0` / `conform` | **working** |
| `native_job_control.py` — ^C / ^Z / fg / jobs / traps in a native shell, over a real pty | **working** |
| local-fakefs backend (host `./build/ish`, device-identical aarch64 gadgets) | **working** (primary) |
| Rosetta `arch -x86_64` + mint Lima-VM oracles (true i386 + real-Linux x86_64) | **working** |
| differential corpus — 12 families (ALU, adc/sbb-mem, shifts, sign-ext, mul/div, mxcsr, bit-ops, rep-string, sse-cvt, sse-shuffle, sse4, atomics) | **working** |
| **conformance corpus** (`conform`) — signal/syscall (`corpus_signal/`) + filesystem/VFS (`corpus_fs/`) families vs **real Linux** (mint), Rosetta excluded | **working** (all green) |
| Tier 0 — the 21 `tests/manual` self-check tests | **working** (20/20 i386, 21/21 amd64) |
| `mint:i386:jit` cell — iSH built in mint's VM (x86_64-host i386 JIT) | **working** |
| crash/hang classification + journal reconciliation (`supervise`) | **working** (local-validated) |
| device backend — ssh deploy/run + devicectl/notify recovery | **scaffold** (dry-run-verified; needs a live device) |
| `minimize` (case bisection) | basic |

## Quickstart

```bash
# from repo root, after `ninja -C build`
python3 tests/remote/conductor.py run                  # differential corpus, all cells
python3 tests/remote/conductor.py run --tests flags_alu --cells oracle,amd64:jit
python3 tests/remote/conductor.py tier0                # tests/manual self-check suite, per arch
python3 tests/remote/native_job_control.py build/alpine-arm64-test          # native bash, over a pty
python3 tests/remote/native_job_control.py build/alpine-arm64-test /AOK/native/zsh
python3 tests/remote/conductor.py conform              # signal/syscall conformance vs REAL LINUX (mint)
python3 tests/remote/conductor.py conform --tests sig_rt_order
python3 tests/remote/conductor.py supervise            # journaled batch (device crash model), local
python3 tests/remote/conductor.py minimize --test flags_alu --cell amd64:jit
python3 tests/remote/conductor.py run --cells mint:i386:jit   # corpus under iSH built in mint's VM
python3 tests/remote/conductor.py device --dry-run --device-host <ip>   # device backend (scaffold)
# results.json + built artifacts land in tests/remote/.work/
```

Requires: arm64 macOS (so `./build/ish` runs the **device-identical aarch64
gadgets**), `zig`, Rosetta (`arch -x86_64`), `build/ish`, `build/tools/fakefsify`.

## How it works

Each corpus test is compiled **three ways from one source**:

| Build | Command | Runs under |
|---|---|---|
| i386 ELF | `zig cc -target x86-linux-musl -static` | iSH i386 |
| x86_64 ELF | `zig cc -target x86_64-linux-musl -static` | iSH amd64 |
| x86_64 Mach-O | `cc -arch x86_64` | `arch -x86_64` (oracle) |

The test executes an x86 instruction via inline asm, captures the result and the
architecturally **defined** EFLAGS bits, and prints one canonical line per case.
It bakes in **no** expected answers — the oracle is ground truth.

**Cells** (engine selection is runtime, via `/proc/ish`, so one `ish` build
covers all of them):

| Cell | Selection | Role |
|---|---|---|
| `oracle` | native x86_64 (Rosetta Mach-O) | x86_64 ground truth |
| `mint:x86_64` | x86_64 ELF in mint's Lima Linux VM | real-Linux x86_64 truth |
| `mint:i386` | i386 ELF in mint's Lima Linux VM | **true i386 truth** |
| `amd64:interp` | `echo 0 > /proc/ish/amd64_jit` | amd64 interp |
| `amd64:jit` | `echo 1 > /proc/ish/amd64_jit` | amd64 JIT |
| `i386:jit` | default | i386 JIT |
| `i386:no_cache` | `…/i386_no_cache_comm` | force fresh gadget regen |
| `i386:single_step` | `…/i386_single_step_comm` | one-instruction blocks |
| `mint:i386:jit` | iSH built in mint's VM (`gadgets-x86_64`) | x86_64-host i386 JIT — codegen independent of the M5/device aarch64 gadgets |
| `device:*` | iSH on a real device over ssh:1022 | the actual target (scaffold) |

The last two are **opt-in** (not in the default set): add `--cells mint:i386:jit`
or use the `device` subcommand explicitly.

**i386 has no interpreter**, so its ground truth is (a) the 3-mode self-diff
(`jit ≡ no_cache ≡ single_step`, which needs no oracle), (b) the x86_64 oracle
for width-agnostic values, and (c) the **`mint:i386`** Lima-VM oracle — true
i386 Linux ground truth. The M5 can't provide that itself (macOS dropped 32-bit;
Rosetta is x86_64-only), so the Intel laptop "mint" runs the *identical* static
i386 ELF in its x86_64 Linux VM (kernel IA32 compat; no 32-bit userspace needed)
via the read-only virtiofs mount of its home — a plain `scp` makes the binary
instantly executable in the VM. mint cells auto-skip when it's offline, so the
M5 runs standalone. Config: `ISH_MINT_HOST` (mint), `ISH_LIMA_INSTANCE` (ish),
`ISH_MINT_BINDIR` (`.ish-oracle/bin`).

**Comparison is key-based**, not `diff`: each line is `(key=op+width+operands) →
(value=res+flags)`. Cells are compared per key, so an i386 cell legitimately
omitting 64-bit lines is not a false positive. Per-op **defined-flag masks**
ensure undefined bits (e.g. AF after a logical op) are never compared.

## Conformance mode (`conform`) — signal/syscall semantics vs real Linux

The CPU-instruction corpus uses Rosetta as an oracle because it faithfully runs
x86 *instructions*. For **OS / signal / syscall semantics** that is the wrong
oracle: a macOS Mach-O has Darwin behavior and lacks Linux-only APIs (signalfd,
real-time signals, `CLD_*` codes). iSH emulates *Linux*, so the ground truth is
a **real Linux kernel** — mint's Lima VM (`mint:x86_64` / `mint:i386`).

`conform` therefore builds the corpora in `corpus_signal/` and `corpus_fs/` as
**only the two Linux musl ELFs** (no Mach-O), runs every iSH cell plus the mint
cells, and key-compares with **mint as the oracle — Rosetta excluded**. Each test prints
`<key> res=<value>` lines of *behavioral* state (si_code, signo, delivery order,
errno symbol, masks) — arch-independent, never raw pointers. Same key-based
compare, same crash classification. Without mint reachable it degrades to an
iSH-only self-consistency run (arch × engine), which is *not* a conformance
check — the divergences only show against real Linux.

> **cmpxchg lesson** (see `project_remote_diff_harness`): assert only
> spec-guaranteed invariants. Real-time signal order (lowest-numbered first,
> same-number FIFO) is POSIX-guaranteed and asserted; *standard*-signal
> cross-number order is unspecified, so the tests never scramble/assert it. One
> intentional iSH deviation (i386 runs every handler on the altstack regardless
> of `SA_ONSTACK`) is documented and the relevant check is gated to amd64.

The signal families (`sig_si_code`, `sig_rt_order`, `sig_deliver_order`,
`sig_block_pending`, `sig_sigtimedwait`, `sig_child`, `sig_signalfd`,
`sig_altstack`, `sig_restart`) found and locked regressions for five real Linux
nonconformances — see the commit that introduced them.

The filesystem families (`fs_path`, `fs_openflags`, `fs_dup`, `fs_dirent`,
`fs_rename_link`, `fs_stat`, `fs_dupfd_range`, `fs_proc`) cover the VFS pin-list
(path resolution incl. `.`/`..`/symlink loops/`O_NOFOLLOW`/trailing slash,
dup/dup2/dup3 + `O_CLOEXEC`, stat/statx, readdir order + `d_type`,
rename/link/unlink + `AT_*` and `renameat2` flags, `/proc` consistency) and
found+locked nine real Linux nonconformances (O_NOFOLLOW ignored, rmdir/mkdir
following a final symlink, `unlink(dir)` leaking the host EPERM, lexical `..`
through a missing component, trailing-slash `O_CREAT`, getdents tiny-buffer EOF,
RENAME_NOREPLACE rejected, `F_DUPFD` negative-arg abort, `/proc/self` trailing
slash). Mirrored as the self-checking `tests/manual/fs_conformance.c` Tier-0 gate.
Known deferred gap: an over-PATH_MAX path returns EFAULT instead of ENAMETOOLONG
(`fs_path` asserts only "is an error").

## Crash resilience

A JIT bug kills the whole emulator (host SIGSEGV/SIGILL/abort). That is the
primary signal this harness exists to catch.

- **local-fakefs:** the host `ish` process *is* the device. A signal exit →
  `CRASH`; a timeout → `HANG`; both are captured with stderr. One crashing case
  never blocks the rest (and `minimize` bisects `--case` to the culprit).
- **device (scaffold — dry-run-verified, needs a live device):** a JIT bug
  there also kills sshd and drops the connection. The recovery model (the local
  `supervise` mode already validates the journal half):
  - `guest_supervisor.c` forks each test as a child and writes a
    `SUPER-START …` / `SUPER-END …` **journal** (to stdout *and* an fsync'd file)
    plus a **heartbeat** to the guest fs (the fakefs SQLite / real dir persists
    across an app restart, so the journal survives the ssh stream truncating);
  - the conductor streams the journal; loss of heartbeat → probe port 1022;
  - on confirmed crash it reads the journal — the id with `SUPER-START` but no
    `SUPER-END` is the crasher — pulls forensics (iOS crash report, app log,
    `[amd64-jit] bad-*` diagnostics), then **recovers**: auto-relaunch via
    `xcrun devicectl` (USB-tethered), or notify + poll port 1022 (ssh-only);
  - the crasher is quarantined and the suite resumes; a later isolated-repro
    pass re-runs it with `ISH_TRACE_*` on and minimizes it.

Backends sit behind one interface (`launch / kill / is_up / pull_crashlog`).
The iOS **Simulator is intentionally unsupported** — the default
case-insensitive macOS volume corrupts Linux rootfs paths.

## Corpus

Two test styles flow through the same matrix:

- **Differential** (`run`, JIT-edge): `corpus/*.c` via `diff_common.h` print a
  canonical `result+flags` line per case; require byte-identical across cells +
  oracle. Twelve families so far, each guarding a bug class this project has hit:
  - `flags_alu` — integer ALU result + EFLAGS exactness
  - `adc_sbb_mem` — carry-in adc/sbb on the **native memory-operand gadget** (not
    just the bridged register path)
  - `shifts` — shl/shr/sar/rol/ror CF/OF at counts 0 / 1 / ≥ width
  - `sext` — sign/zero-extension (movsx/movzx/cbw/cwde/cdqe)
  - `muldiv` — mul/imul/div/idiv, high half + `#DE`
  - `sse_mxcsr` — ldmxcsr/stmxcsr control-word round-trip (amd64)
  - `bit_ops` — bt/bts/btr/btc (CF) and bsf/bsr (ZF, undefined-dest)
  - `rep_string` — rep movs/stos + repe/repne cmps/scas; DF fwd/back, cross-page
    spans (the batch fast path), ECX=0, 16-bit forms, early-out ECX + flags
  - `sse_cvt` — SSE/SSE2 add/sub/mul/div/sqrt/min/max (scalar + packed) and the
    conversions (cvtsi2/cvtt*2si/cvtss2sd/cvtsd2ss/cvtdq2ps); NaN results and
    out-of-range float→int are canonicalized so the documented arm64-vs-x86 NaN
    sign / saturation differences don't false-diverge
  - `sse_shuffle` — shufps/shufpd, unpck[lh]p[sd], pshufd/pshuflw/pshufhw, and the
    sign-mask extracts movmskps/movmskpd/pmovmskb; pure lane moves, byte-exact
  - `sse4` — SSSE3 / SSE4.1 three-byte ops (0F 38 / 0F 3A): pinsr/pextr (d/b/w),
    extractps, palignr, pshufb, pabs, pmovsx/pmovzx (all 12), pmulld/pmuldq,
    pcmpeqq/pcmpgtq, packusdw, pmin/pmax (sb/uw/sd/ud), ptest (ZF/CF), the imm and
    XMM0-variable blends, and round{ps,pd,ss,sd} (finite inputs). Found the i386
    JIT had **no three-byte opcode map at all** (guest `pinsrd` → SIGILL, crashed
    cmake); the amd64 engine had the same gap (no 0F 3A handler, partial 0F 38).
    Both fixed — all 8 cells (i386 ×3, amd64 ×2, oracle, mint ×2) agree byte-exact.
  - `atomics` — lock xadd / cmpxchg / cmpxchg8b / cmpxchg16b / lock add·and·or·
    xor·sub·inc·dec / xchg; result + ZF (cmpxchg arith sub-flags masked — see the
    oracle-fidelity note below)
- **Self-checking** (`tier0`): the 21 `tests/manual/*.c` tests (atomics, futex,
  signals, ptrace, epoll, fcntl/OFD, copy_file_range, pidfd, …) built static and
  run under iSH per arch, gated on `^<name>: PASS$`. No oracle — functional
  regression, not differential.

Still planned: **control-flow/SMC**.

This run the harness found and fixed **12 amd64/i386 JIT flag & result bugs** —
adc/sbb carry-in AF/OF (interp *and* the native gadget); rol/ror CF on full
turns; ror-by-1 OF; sar CF past width; cbw sign-extend; 32-bit mul/imul high
half; 2-op imul w64 overflow; i386 16-bit imul; i386 div/idiv `#DE`; missing
amd64 LDMXCSR/STMXCSR; and i386 min/max returning the wrong operand on the
`+0`/`-0` (and NaN) tie — plus **implemented amd64 `cmpxchg16b`** (the 128-bit
compare-exchange was silently running as cmpxchg8b, mangling the result). Each
confirmed against the oracle (Rosetta + real-Intel mint) and validated to green.

### Oracle fidelity: oracles can disagree

`atomics` surfaced a case where the two oracles disagree. After `cmpxchg`, real
Intel (the `mint` cell), iSH, and the Intel SDM (`CMP accumulator, dest`) all set
CF/PF/AF/SF/OF from `acc − dest`, while Rosetta uses `dest − acc`. This is **not
necessarily a Rosetta bug** — CPUs carry errata and generational/vendor quirks,
and an emulator may faithfully model a different part — but it does show the
arithmetic sub-flags of `cmpxchg` are not a dependable cross-implementation
invariant. Treating *either* single oracle as absolute ground truth would have
meant "correcting" iSH to match it; the disagreement only became visible because
a second, real-silicon oracle (mint) sits alongside Rosetta. The test now
compares only ZF (the order-symmetric flag) for cmpxchg. The episode is the
clearest argument for the second oracle — not to crown a winner, but to flag
where "ground truth" is implementation-defined.

## First validated finding (worked example)

On its first run the harness flagged 214 divergent keys, all `adc`/`sbb` with
carry-in = 1, on **amd64 only** (i386 matched the oracle exactly). Root cause:
`amd64_set_adc_flags` / `amd64_set_sbb_flags` pre-folded the carry into
`rhs_with_carry = rhs + carry_in` and fed it to the **carry-less** AF/OF
formulas; the folded carry ripples past bit 3 (`0x7f+1=0x80`), corrupting the
bit-4 XOR (AF) and the signed-overflow test (OF). Fixed by using the original
`rhs` for OF and the carry-aware nibble form for AF. Harness verdict after the
fix: all cells agree. (The native *memory-operand* adc/sbb gadget had the same
anti-pattern; `adc_sbb_mem` was added to cover it and it too is now fixed.)

## Layout

```
tests/remote/
  conductor.py          orchestrator: run / supervise / device / tier0
  guest_supervisor.c    in-guest batch runner (journal for device crash recovery)
  corpus/
    diff_common.h       differential harness: flag capture, engine self-select,
                        --case/--seed/--list, canonical emit
    flags_alu.c adc_sbb_mem.c shifts.c sext.c muldiv.c sse_mxcsr.c bit_ops.c
  .work/                build artifacts + results.json (gitignored)
  README.md             this file
```
