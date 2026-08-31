# 38. Where the time actually goes

Most performance writing about emulators is vague, because the honest answer is
usually "it depends". This chapter is not vague, because in August 2026 somebody
measured the thing properly and came away with a model that *predicts*:

**Count the guest instructions in a hot loop, multiply by about 6.8 nanoseconds,
and you have the time.**

That sentence is the most useful result in the project, and the rest of this
chapter is how it was established, what it explains, and where it stops.

## 38.1 The reference rig is an old iPad, on purpose

All device numbers come from an **A9 iPad** — iPad6,12, ARMv8.0, two homogeneous
cores, iOS 16.7. Not because it is representative, and certainly not because it
is fast.

Because it is *quiet*:

> That device is the reference rig because its two identical cores give ±0.5%
> run-to-run spread, where an Apple Silicon Mac oscillates with P/E migration
> and this particular Mac also sits at load ~35.

This is worth stating as a general principle, because it is counter-intuitive:
**a heterogeneous-core machine is a worse measuring instrument than an old
homogeneous one.** A modern Mac will migrate a benchmark between performance and
efficiency cores mid-run, and the resulting spread swamps the 1–2% effects that
most of this work produces.

Two build conditions are equally load-bearing, and both are stated up front:
`buildtype=debugoptimized`, because an `-O0` build invalidates measurements
(Chapter 34); and `log=''`, because `log=strace` costs **15x** on a
syscall-heavy workload.

## 38.2 Four guests, one root, one source

The cleanest possible comparison: one C source, statically cross-compiled four
ways, all four binaries run in **one** root — so libc and distribution are not
variables and only the engine differs.

| seconds (lower is better) | aarch64 | riscv64 | amd64 | i386 |
|---|--:|--:|--:|--:|
| integer | **20.4** | **20.3** | 66.2 | 94.9 |
| float | **40.8** | **40.8** | 160.9 | 313.3 |
| penalty vs aarch64 | — | 1.00x | 3.2x / 3.9x | 4.7x / **7.3x** |

Three readings:

**i386 float at 7.3x is the worst case anywhere in the system**, and it is x87
emulation — the software 80-bit arithmetic of Chapter 5. amd64's float is 3.9x
rather than 7.3x because it uses SSE, which is most of why amd64 beats i386
overall despite having far fewer optimizations.

**riscv64 ties aarch64**, which looks like a paradox and is the subject of the
next section.

**The practical guidance falls straight out**: on ARM hardware prefer an arm64
or riscv64 rootfs, and if x86 is required prefer amd64 over i386.

There is also a small piece of methodological honesty attached. An earlier run
had used a *different distribution per architecture*, which is a real
confounder — and rather than discard it, it was cross-checked against this
clean run and landed within 1.2% of every number. The flawed experiment was
retroactively validated rather than deleted or quietly relied upon.

## 38.3 Deriving 6.8 nanoseconds

Here is the argument, and it is a proof rather than an assertion.

The benchmark's hot loop, disassembled from the two static binaries:

```
aarch64: add x1,x1,x0 / add x0,x0,#1 / cmp x0,x2 / b.ne     = 4 instructions
riscv64: add a1,a1,a5 / addi a5,a5,1 / bne a5,a4,loop       = 3 (compare-and-branch)
```

aarch64 executes **33% more guest instructions for identical work** — and ties.
That should be impossible if cost tracks instruction count.

The resolution is arm64's compare-and-branch fusion, which collapses `cmp` +
`b.ne` into a single dispatch (Chapter 6). And because the fusion bits are
runtime-togglable through `/proc/ish/arm64_jit_fuse` — which is exactly why they
were built that way — the hypothesis can be tested directly:

| | seconds |
|---|--:|
| `bcond=1` | 20.435, 20.439 |
| `bcond=0` | 27.042, 27.234 |

**1.328x measured, against a predicted 4/3 = 1.333x.**

Which converts three independent configurations into three estimates of the same
constant:

| | dispatches/iteration | seconds | ns/dispatch |
|---|--:|--:|--:|
| aarch64, `bcond` fused | 3 | 20.44 | 6.812 |
| aarch64, `bcond` off | 4 | 27.14 | 6.785 |
| riscv64 (native 3-instruction loop) | 3 | 20.32 | 6.773 |

Identical to within **0.6%**.

Two different guest architectures, two different fusion states, one constant.
That is what makes it a model rather than a benchmark result: it predicted the
1.333x before the A/B was run, and it explains why an arm64 guest on an arm64
host is not fast (Chapter 7) without any appeal to hand-waving about "emulation
overhead".

## 38.4 What fusion and the return caches are worth

One build, one filesystem, arms interleaved rep by rep, and the mask read back
on every run:

| guest | shape | ON | OFF | faster by |
|---|---|--:|--:|--:|
| aarch64 | fib 33 | 1063 | 1638 | **35.1%** (1.54x) |
| aarch64 | ind 30M | 1846 | 7799 | **76.3%** (4.22x) |
| riscv64 | fib 33 | 1246 | 1851 | 32.7% (1.49x) |
| riscv64 | ind 30M | 2293 | 8167 | 71.9% (3.56x) |
| i386 | fib 33 | 1052 | 1505 | 30.1% (1.43x) |
| i386 | ind 30M | 8804 | 11689 | 24.7% (1.33x) |
| amd64 | fib / ind | 2009 / 7940 | 2000 / 7840 | ~0% |
| aarch64 | `bmm` integer | 20.65 s | 27.24 s | 24.2% |
| riscv64 | `bmm` integer | 20.49 s | 20.39 s | ~0% |

**Indirect dispatch is the headline: 4.2x on aarch64.** That is
function-pointer-heavy code — vtables, interpreters, callbacks — which is to say
most real software, and it is why the return cache of Chapter 6 earns the
complexity it carries.

**amd64 is flat**, because `incdec_reg` is its only fusion lever. The
document's conclusion is worth quoting because it is a roadmap rather than a
complaint: "it is now the weakest engine and holds the most remaining headroom."

**riscv64's `bmm` is flat while aarch64's gains 24.2%** — Section 38.3's
mechanism exactly. arm64's fusion buys what RISC-V gets for free from its ISA.

And a framing note that most benchmark tables omit: this is a **floor** on
"improvement this cycle, not a ceiling", because the toggle cannot switch off
the other work done in the same period.

## 38.5 A win, and the regression published beside it

The per-syscall scratch zeroing is the largest host-side win of the cycle.
`cpu_step_to_interrupt` was zeroing about **40 KB on every entry** — an 8 KB
block-lookup cache plus a 32 KB return cache — and an entry happens once per
guest syscall.

| workload | before | after | |
|---|--:|--:|---|
| `dd bs=1`, 150k syscalls | 1264 ms | **920 ms** | **−27.2%**, distributions disjoint |
| 300× process spawn | 2989 | 2559 | ~−14% |
| 300× read 1 MB | 3700 | 3254 | ~−12% |
| 200k arithmetic loop | 50524 | 50760 | **+0.5% slower** |

That last row is why this section exists. The regression is real, disjoint, and
explained: `jit_frame` holds `cpu`, which every gadget dereferences, and it
moved from the stack to the heap. The fix is named — keep the frame on the stack
and persist only the two big arrays — and has not been done.

A −27% headline with a +0.5% regression printed underneath it, with the
mechanism and the remedy, is the standard this book keeps holding things to. It
is also the only way the next person can tell whether their compute workload got
slower for a reason.

## 38.6 Memory, and a test that cannot pass

Peak RSS scales at about **564 KB per guest thread** — 44.5 MB at 50 threads,
242 MB at 400. The persistent JIT scratch is 40.5 KB of that, or 7.2%.

Which produces a useful kind of conclusion:

> `bmt`'s 10,000-thread test needs ~5.4 GB and **cannot pass on a 2 GB device
> regardless of emulator changes**.

That is not a bug and not a target. It is arithmetic, and knowing it stops
somebody spending a week optimizing toward a number that is unreachable for
reasons that have nothing to do with the code.

## 38.7 Method notes, which are the transferable part

The numbers above will age. These will not.

**Interleave arms rep by rep.** Sequential arms let cache warmth and thermal
drift fake a result — "one such run reported a flat change as 75–120% slower".

**Print the read-back state on every rep.** A `/proc` write that fails is silent,
because the node is root-owned, and a silently-unapplied toggle "looks exactly
like a clean negative result". This is the same rule as Chapter 36's: prove the
instrument before believing the measurement. It is also the original design
reason for the fusion knobs being readable at all (Chapter 6).

**Self time, not subtree.** `sample` emits a call tree, and counting raw node
values makes every ancestor read about 100%. An amd64 cost reported as 41.6% was
an inclusive subtree; its self time was ~12%.

**Check which build produced a signal.** A diagnostic from one build directory
attributed to another is a whole afternoon.

## 38.8 Where the non-CPU time goes

Dispatch explains compute. It does not explain a system, and the other half is
locks and filesystems.

Chapter 16's profile: `inodes_lock` at 74–77% duty with 880 ms of aggregate wait
on an open-heavy workload, held by `generic_openat` across a stat, a host open
and an fstat. Chapter 17's: the fakefs metadata mutex at 78% duty **under a
single thread**, which is why concurrent metadata work measured slower than
sequential and why the fix was per-thread connections rather than parallelism.

And Chapter 7's thread benchmark is the control that ties the two halves
together: 5,000 `clone`s and joins land within 4.5% across all four guests,
because that time is spent in shared kernel code rather than in translation.
When a workload is syscall-bound, the guest architecture stops mattering and
everything in Parts III and IV starts to.

## 38.9 What the model tells you to do

If cost is guest instruction count times a constant, there are exactly three
ways to go faster, and every optimization in this book is one of them:

**Fewer dispatches per guest instruction.** Fusion (Chapter 6). Worth 24–76%
depending on shape, and amd64 has barely started.

**Fewer guest instructions.** High-level emulation replaces a whole libc
function with one call (Chapter 8); native programs remove the program's
instruction stream entirely (Part V); accelerators remove a library's
(Chapter 33).

**Fewer exits from translated code.** Block chaining and the return caches
(Chapter 6), and the per-syscall scratch zeroing above, which made each exit
cheaper rather than rarer.

What the model also says is where *not* to look. It does not matter how
complicated a guest instruction is, so micro-optimizing a gadget's body buys
almost nothing. It does not matter which ARM core you are on for the dispatch
cost, within a factor. And it explains, in one number, why the same effort spent
on the amd64 engine's fusion would be worth more today than any amount of
further work on arm64's.

---

*Anchors:* [docs/perf_benchmarks_2026_08.md](../../docs/perf_benchmarks_2026_08.md),
[docs/performance-optimizations-2026-07.md](../../docs/performance-optimizations-2026-07.md),
[docs/guest_architecture_benchmarks.md](../../docs/guest_architecture_benchmarks.md),
[docs/jit_gadget_perf_plan.md](../../docs/jit_gadget_perf_plan.md),
[opt/AOK/docs/benchmarks.md](../../opt/AOK/docs/benchmarks.md),
`tests/manual/jit_bench.c`, `tests/manual/jit_fuse_ab.sh`,
[util/lockstats.c](../../util/lockstats.c), [kernel/native_bench.c](../../kernel/native_bench.c).
