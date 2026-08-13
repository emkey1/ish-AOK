# iSH-AOK performance measurements, August 2026

All device numbers are the **A9 iPad** (iPad6,12, ARMv8.0, two homogeneous cores, iOS 16.7)
unless stated. That device is the reference rig because its two identical cores give ±0.5%
run-to-run spread, where an Apple Silicon Mac oscillates with P/E migration and this
particular Mac also sits at load ~35.

Every emulator build measured here was `buildtype=debugoptimized` (`optimization=2`) with
`log=''`. Both matter: an `-O0` build invalidates measurements (the guest prints
`" unoptimized"` in `uname -v`), and `log=strace` cost 15x on a syscall-heavy workload.

---

## 1. Four guests, same work, same root

Statically cross-compiled from one source (`/AOK/tools/iSH_benchmark.tgz`, `benchmark/bmm.c`
— an accumulate loop with an `asm` barrier so `-O2` cannot fold it), all four binaries run in
**one** root, so libc and distro are not variables. Only the emulator engine differs.

| seconds (lower better) | aarch64 | riscv64 | amd64 | i386 |
|---|---|---|---|---|
| integer | **20.4** | **20.3** | 66.2 | 94.9 |
| float | **40.8** | **40.8** | 160.9 | 313.3 |
| penalty vs aarch64 | — | 1.00x | 3.2x / 3.9x | 4.7x / **7.3x** |

- **riscv64 ties aarch64** on dense `-O2` code. See §3 for why this is not a paradox.
- **i386 float is the worst case anywhere at 7.3x** — x87 emulation. amd64 float is 3.9x
  because it uses SSE, which is why amd64 beats i386 by 1.4-1.9x overall despite having far
  fewer optimizations.
- Practical guidance: on ARM hardware prefer an arm64 (or riscv64) rootfs; if x86 is required
  prefer amd64 over i386.
- Cross-check: an earlier run with a *different distro per arch* (riscv64 on Alpine/musl, the
  rest on Devuan/glibc) landed within 1.2% of every number above, so that run was sound
  despite the mismatch.

## 2. Call-heavy shapes

`tests/manual/jit_bench.c`: `fib` is a call+return per node, `ind` is an indirect call per
iteration. These are what the return caches serve; `bmm` above has **no calls in its hot
loop** and cannot show them at all.

| median of 3, ms | fib 33 | ind 30M |
|---|---|---|
| aarch64 | **1041** | **1823** |
| riscv64 | 1383 | 2301 |
| i386 | 1052 | 8790 |
| amd64 | 2067 | 7643 |

## 3. The engine is dispatch-bound: ~6.8 ns per dispatch

This is the most useful single result, because it predicts the rest.

Neither guest runs natively — an aarch64 guest instruction is still ONE gadget dispatch, just
like a riscv64 one. Being the host's own ISA family makes each gadget's *body* trivial; it does
not avoid the dispatch. So cost tracks guest **instruction count**.

The hot loop, disassembled from the two static binaries:

    aarch64: add x1,x1,x0 / add x0,x0,#1 / cmp x0,x2 / b.ne   = 4 instructions
    riscv64: add a1,a1,a5 / addi a5,a5,1 / bne a5,a4,loop     = 3   (compare-and-branch)

aarch64 runs 33% more guest instructions for identical work, yet ties. Confirmed cause: the
arm64 compare+branch fusion collapses `cmp`+`b.ne` into one dispatch. Flipped at runtime via
`/proc/ish/arm64_jit_fuse`:

| | seconds |
|---|---|
| `bcond=1` | 20.435, 20.439 |
| `bcond=0` | 27.042, 27.234 |

**1.328x** against a predicted 4/3 = 1.333x. Which yields:

| | dispatches/iter | seconds | ns/dispatch |
|---|---|---|---|
| aarch64, bcond fused | 3 | 20.44 | 6.812 |
| aarch64, bcond OFF | 4 | 27.14 | 6.785 |
| riscv64 (native 3-insn) | 3 | 20.32 | 6.773 |

Identical to within **0.6%**. Count guest instructions in a hot loop, multiply by ~6.8 ns, and
you have the time on this device.

## 4. What the fusion + return-cache work is worth

One build, one filesystem, arms interleaved rep by rep, mask read back on every run
(`/proc/ish/{i386,arm64,riscv64,amd64}_jit_fuse`, `all=1` vs `all=0`).

| guest | shape | ON | OFF | faster by |
|---|---|---|---|---|
| aarch64 | fib 33 | 1063 | 1638 | **35.1%** (1.54x) |
| aarch64 | ind 30M | 1846 | 7799 | **76.3%** (4.22x) |
| riscv64 | fib 33 | 1246 | 1851 | **32.7%** (1.49x) |
| riscv64 | ind 30M | 2293 | 8167 | **71.9%** (3.56x) |
| i386 | fib 33 | 1052 | 1505 | 30.1% (1.43x) |
| i386 | ind 30M | 8804 | 11689 | 24.7% (1.33x) |
| amd64 | fib / ind | 2009 / 7940 | 2000 / 7840 | ~0% |
| aarch64 | bmm integer | 20.65 s | 27.24 s | 24.2% |
| riscv64 | bmm integer | 20.49 s | 20.39 s | ~0% |

- **Indirect dispatch is the headline: 4.2x on aarch64, 3.6x on riscv64.** That is
  function-pointer-heavy code — vtables, interpreters, callbacks — i.e. most real software.
- **amd64 is flat** because `incdec_reg` is its only lever. It is now the weakest engine and
  holds the most remaining headroom.
- riscv64's `bmm` is flat while aarch64's gains 24.2% — the §3 mechanism exactly: arm64's
  fusion buys what RISC-V gets from its ISA.
- This is a FLOOR on "improvement this cycle", not a ceiling: it cannot switch off the
  per-syscall memset removal, the riscv64 per-block sync fix, or the arm64 dispatch change.

## 5. Host-side wins (all four engines)

Per-syscall scratch zeroing removed (`711b48f3`): `cpu_step_to_interrupt` was zeroing ~40 KB
(an 8 KB block-lookup cache + a 32 KB return cache) on **every entry**, and an entry happens
once per guest syscall. Measured on an idle bare-metal AMD host, 8 interleaved pairs, i386:

| workload | before | after | |
|---|---|---|---|
| `dd bs=1`, 150k syscalls | 1264 ms | **920 ms** | **-27.2%**, distributions disjoint |
| 300x process spawn | 2989 | 2559 | ~-14% |
| 300x read 1 MB | 3700 | 3254 | ~-12% |
| 200k arithmetic loop | 50524 | 50760 | **+0.5% slower** |

The compute regression is real and disjoint: `jit_frame` holds `cpu`, which every gadget
dereferences, and it moved from stack to heap. Fixable by keeping the frame on the stack and
persisting only the two big arrays.

## 6. Memory, per guest thread

Measured on an idle host with a static thread-spawner: peak RSS scales at **~564 KB per guest
thread** (44.5 MB at 50 threads, 242 MB at 400). The persistent JIT scratch is 40.5 KB of
that — 7.2%. So `bmt`'s 10,000-thread test needs ~5.4 GB and cannot pass on a 2 GB device
regardless of emulator changes.

## Method notes worth keeping

- **Interleave arms rep by rep.** Sequential arms let cache warmth and drift fake a 2x result;
  one such run reported a flat change as "75-120% slower".
- **Print the read-back state on every rep.** A `/proc` write that fails is silent (the node is
  root-owned) and looks exactly like a clean negative result.
- **Self time, not subtree.** `sample` emits a call tree; counting raw node values makes every
  ancestor read ~100%. An amd64 cost reported as 41.6% was an inclusive subtree; self was ~12%.
- **Check which build produced a signal.** A "defined but not used" warning came from the
  x86_64 host build where the aarch64 paths are `#if`'d out; the function was live on aarch64.
- **A benchmark must prove it did the work.** Print an iteration count. Two runs here measured
  a `chroot` failure path (~520 ms against a real ~6500 ms) and a loop that never executed
  (112 ms against 3042 ms) — both looked like wins.
