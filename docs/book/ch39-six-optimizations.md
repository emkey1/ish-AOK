# 39. Six optimizations, in full

Chapter 38 established the model. This chapter is six investigations that used
it, told in the same shape each time — symptom, first hypothesis, what settled
it, what was actually wrong, and the number afterwards.

They are laid out together because the pattern across them is worth more than
any one of them. **Four of the six had a wrong first hypothesis**, and in every
one of those the thing that corrected it was a measurement of a *component*
rather than of the whole.

| | symptom | first hypothesis | actually |
|---|---|---|---|
| 1 | (none — an obvious simplification) | `ldar` replaces `ldr`+`dmb` | 2.04x regression on ARMv8.0 |
| 2 | data movement is slow | native will beat translated | the translated code was already good |
| 3 | (none — model-driven) | fewer dispatches, less time | correct, and 4.2x on indirect |
| 4 | shells are slow | re-launch is too expensive | forking was never emulated |
| 5 | guest `gcc` ICEs on CI | a toolchain problem in the guest | `sendfile` truncating at 64 KB |
| 6 | the file manager freezes | it needs concurrency | it needs two queues, not more threads |

## 39.1 The barrier that looked redundant

**Symptom:** none. This one starts from reading code — `ldr` followed by
`dmb ishld` is what `ldar` means, on the hot path of every guest instruction
(Chapter 6).

**How it was settled:** implemented, then A/B'd on two microarchitectures.

**Actually:** correct, and 4.3% *faster* on Apple silicon. And a **2.04x
regression** on an A9 iPad — 8,422 ms against 17,203 ms on the same shell loop.
Apple's cores retire `ldar` nearly free; the A9 implements load-acquire far more
conservatively, and this sequence runs once per guest instruction.

**Fix:** revert, expose the choice as `-Darm64_gret=dmb|ldar` with `dmb` as the
default, and write the numbers into the header.

**The lesson:** an obvious simplification has to be measured on the *oldest*
device in the support matrix, not the fastest one to hand. Old devices are the
point of this project, so they win — and the numbers in the comment are what
stop the change being re-proposed every year.

## 39.2 The optimization that was slower

**Symptom:** guests spend their time in `memcpy` and `strlen`.

**Hypothesis:** recognize those functions and run host-native code instead
(Chapter 8). Native beats translated.

**How it was settled:** measured, and the first implementation was **about twice
as slow as plain emulation**.

**Actually:** it staged data through a 256-byte stack buffer using `tlb_read` and
`tlb_write` — while the code it replaced, the JIT's *translated* `memcpy`, was
already writing host memory directly with NEON, because the guest's own `memcpy`
is NEON code and the memory gadgets resolve host pointers.

**Fix:** resolve a direct host pointer per guest page and run one native
operation per in-page span.

**Result:** 1.23x at 256 B, 3.16x at 4 KB, 7.17x at 64 KB, 6.68x at 1 MB. On
real workloads: `sort` +22%, `base64` +5%, `wc` +3%, and neutral on
arithmetic-bound work.

**The lesson:** "native code beats translated code" is an assumption, not a
fact. The translated code here was the output of a good translator over
hand-tuned assembly. Beating it required being native **and doing less work**.

## 39.3 The optimization built with its own measuring device

**Symptom:** none. This one comes from the model: if cost is dispatches times a
constant, one gadget doing the work of two is proportionally faster.

**How it was settled:** by making the fusion families runtime-togglable through
`/proc/ish/<arch>_jit_fuse` *before* measuring anything — so that arms could be
interleaved rep by rep, and the mask read back on every run.

**Result:** 35% on a call-heavy shape, **76% (4.22x) on indirect dispatch**,
24% on a fused compare-and-branch loop. And ~0% on amd64, whose only fusion
lever is `incdec_reg`.

**The lesson:** the measurement surface is part of the feature. An environment
variable would have required relaunching the app to change — killing the ssh
session being measured through — and could not be read back to prove it took
effect. Both of those had already produced meaningless "flat" results. Building
the toggle first is what made the 1.328x prediction of Chapter 38 checkable at
all.

## 39.4 The design that was chosen by measuring its alternative

**Symptom:** shells are slow, and shells are what people use.

**Hypothesis:** compiling bash in natively is attractive but `fork` is
impossible, and the re-launch workaround will be too expensive to be worth it.

**How it was settled:** by measuring the *pieces* rather than the design. A
guest `fork` costs ~2.5 ms — and that cost is AOK's `sys_clone`, native C in the
emulator, which was never being emulated at all. There was no 40x penalty to
recover.

**Actually:** interpretation was the whole cost, at 38–46x, and forking was
nearly free. So re-launch — spawning a fresh shell at ~1.6 ms and handing it the
parent's state — costs about what bash already pays.

**And then the follow-on**, which is the same lesson twice: a subshell cost
8.3 ms, and *all three* obvious suspects measured wrong. The spawn plus a full
native bash startup together were 1.8 ms. Growing the state script 2.7x added
0.7 ms. The cost was 85 per-line `2>/dev/null` redirections, each an open, a
dup2 and a close through the shim — 120 such lines took 9.2 ms; the same lines
under one `exec` redirection took 1.4 ms.

**Result:** 11.2 ms per subshell start down to 2.8 ms, reaching parity with the
emulated shell at forking while keeping 46x on everything interpreted.

**The lesson:** measure the components before choosing the design, and again
after it works. Both times, the number that mattered belonged to something
nobody had suspected.

## 39.5 The bug that was found by chasing throughput

**Symptom:** guest `gcc` internal compiler errors and Python segfaults, on CI.

**Hypothesis:** something wrong with the toolchain in the guest.

**Actually:** `fd_copy_range` — the shared engine behind `sendfile` and
`copy_file_range` — dropped the read-but-unwritten tail of its 64 KB bounce
buffer on a short write, which a pipe produces routinely, and advanced the input
offset past bytes that had never been delivered. The caller saw a clean EOF.

busybox's `cat` and `tar` both use `sendfile`, so **any guest pipe copy of a
file larger than 64 KB silently truncated** — and a truncated archive produces
corrupted source files, and corrupted source files crash compilers.

**Fix:** drain each chunk with a write loop, and rewind the input by whatever is
left over.

**The lesson:** this is in a performance chapter because it was found by
performance work, and it is the only correctness bug in the set. It is also the
clearest example of a symptom arriving three layers from its cause — Chapter 16's
MariaDB crash and Chapter 18's empty btop panels are the same shape. **Ask
whether the symptom's layer is the cause's layer**, especially when the symptom
is in well-tested third-party software.

## 39.6 The fix that was almost the same bug again

**Symptom:** the file manager "completely unresponsive trying to do anything
with the folder, including just viewing it".

**Hypothesis:** the file bridge is single-threaded; make it concurrent.

**How it was settled:** the lock profile. AOK's fakefs metadata mutex already
saturates under a *single* thread at 78% duty, and parallel metadata work
measures slower (Chapter 17). Concurrency would have made every number worse.

**Actually:** a latency-class problem, not a throughput one. One serial queue
carried directory listings, `stat`, `mkdir`, rename, delete, whole-file reads and
writes, chunked extraction and cross-backend copies — so a `readdir` of twelve
entries queued behind a 300 MB transfer. And `-setLoading:` disables user
interaction for the whole load, so the folder was not merely stale but
untappable.

**Fix:** two **serial** lanes, interactive and bulk, with the lane chosen at
enqueue time.

What makes this the best of the six is what happened next: **three bugs were
found in the first draft of the fix, by testing it.** The sharpest one:

> The claim-conflict rule started as "equal, or one is an ancestor of the
> other". `/` is an ancestor of everything, so a copy into `/tmp/foo` conflicted
> with a listing of `/` — **any transfer anywhere would have pushed every listing
> of `/` onto the bulk lane, which is the exact blocking being removed.**

A correctness rule that would have faithfully re-created the original bug. The
rule is now: same path, a path and its parent directory, and — for a recursive
delete alone — a subtree and anything inside it.

Two more decisions from the same work deserve keeping.

**The ordering question was settled by reading every call site**, not by
reasoning about queues. Callers do write-then-reload and expect to see the
result — and every one of them issues the reload *from inside the mutation's
completion block*, so the happens-before relationship is the completion's, not
the queue's, and splitting the queue cannot break it.

**And the unused hook was deleted.** An `ioQueue` property existed so callers
could order their own work behind pending operations; it had no users, and it is
gone from the header "so nobody can rebuild a dependency on a total order that
no longer exists". Removing the ability to depend on a guarantee you have just
stopped providing is the difference between a fix and a future bug.

The invariant that ties it together — **the interactive lane never waits** — is
also what makes the design deadlock-free: an interactive operation that would
have to wait is re-routed onto the bulk lane behind the work it must follow,
and the bulk lane may block because it is already slow. No cycle.

## 39.7 What the six have in common

**Four of six had a wrong first hypothesis**, and the wrong hypothesis was
always the *intuitive* one: the obvious simplification, the obvious speedup, the
obvious cause, the obvious fix.

**Three of six had their fix in a different place from their symptom.** `gcc`
crashing was a copy engine. A frozen file browser was queue topology. A slow
shell was a state script's stderr redirection.

**Two of six were made cheap by a measurement surface built in advance** — the
fusion toggles and the lock statistics. The others needed an instrument built on
the spot, which is most of what made them expensive.

**Two of six produced a negative result worth keeping**: HLE's first
implementation and the `ldar` revert. Both are recorded *in the source*, with
numbers, next to the code somebody would have to change to re-propose them. That
is why they stayed decided.

**All six end with a number**, and in four cases the number lives in a comment
in the tree rather than in a document, which is the difference between a result
and a rediscovery.

## 39.8 The rules this leaves

1. **Measure the components before choosing the design.** Chapter 24's whole
   architecture came from discovering that the thing everyone assumed was
   expensive was free.
2. **Build the A/B surface into the feature.** If flipping the arm requires a
   relaunch or cannot be read back, the result will eventually be meaningless
   and nobody will know which time.
3. **Interleave, read back, and use the quiet machine.** Sequential arms fake
   results; a silently-failed toggle looks like a clean negative; a
   heterogeneous-core host adds more noise than the effect.
4. **Publish the regression.** Chapter 38's −27.2% has a +0.5% printed
   underneath it with the mechanism and the remedy.
5. **Record the negative result where the next person will be standing.** Not in
   a document — in the header they would have to edit.
6. **Ask whether the symptom's layer is the cause's layer.** When the symptom is
   inside well-tested third-party software, it usually is not.

That last one is not really a performance rule, which is fitting. Half of this
chapter's investigations began as performance work and ended somewhere else
entirely — and the one that ended in a data-loss bug was the most valuable of
the six.

---

*Anchors:* [docs/perf_benchmarks_2026_08.md](../../docs/perf_benchmarks_2026_08.md),
[docs/performance-optimizations-2026-07.md](../../docs/performance-optimizations-2026-07.md),
[docs/bash_native_plan.md](../../docs/bash_native_plan.md),
[docs/guest_file_bridge_lanes.md](../../docs/guest_file_bridge_lanes.md),
[jit/gadgets-aarch64/gadgets.h](../../jit/gadgets-aarch64/gadgets.h),
[jit/hle.c](../../jit/hle.c), [kernel/fs.c](../../kernel/fs.c) (`fd_copy_range`),
[app/GuestFileBridge.m](../../app/GuestFileBridge.m),
[docs/TODO.md](../../docs/TODO.md), commits `861da1d1`, `95140ab7`, `643ea9eb`.
