# 6. Threaded code: the JIT that is not a JIT

There is one thing an iOS application may not do that matters more than all the
others: it may not write instructions into memory and then execute them. The
entitlement that permits it exists, and it is not available to ordinary App
Store apps.

That should be the end of any argument about running an emulator at a usable
speed. An interpreter loop — fetch the guest instruction, decode it, switch on
the opcode, execute — spends most of its time on the fetch and the switch rather
than on the work, and no amount of care makes that cheap. Emulators solve this
by compiling: translate a run of guest instructions into host instructions once,
then run the host instructions many times. Which requires writing instructions
into memory and then executing them.

iSH-AOK compiles anyway. This chapter is about the loophole, which is old,
completely legitimate, and has consequences that took years to work through.

## 6.1 The loophole

Here, in full, is the code generator:

```c
static void gen(struct gen_state *state, unsigned long thing) {
    assert(state->size <= state->capacity);
    if (state->size >= state->capacity) {
        state->capacity *= 2;
        struct jit_block *bigger_block = realloc(state->block,
                sizeof(struct jit_block) + state->capacity * sizeof(unsigned long));
        if (bigger_block == NULL) {
            if (state->oom_active)
                _longjmp(state->oom_recovery, 1);
            die("out of memory while jitting");
        }
        state->block = bigger_block;
    }
    assert(state->size < state->capacity);
    state->block->code[state->size++] = thing;
}
```

A bounds check, a `realloc`, and an assignment into `unsigned long code[]`. That
is the whole of it. Translating a guest instruction means calling this a few
times:

```c
gen(state, (unsigned long) gadget_amd64_jmp);
gen(state, (unsigned long) (target_ip | (1ull << 63)));
```

The values appended are the *addresses of functions* — small assembly routines,
one per operation, compiled from `jit/gadgets-aarch64/*.S` or
`jit/gadgets-x86_64/*.S` into the application binary, linked and code-signed at
build time like every other function in it — interleaved with the immediates
those routines need. The resulting array is data. The platform has no objection
to writing data.

Executing it means walking the array: load the next pointer, jump to it, and let
that routine end by doing the same thing again. This is *indirect threaded
code*, which Forth implementations were doing in the 1970s for reasons that had
nothing to do with code signing, and it turns out to be the technique that makes
an emulator viable on a platform designed to prevent them.

The vocabulary in this chapter follows the tree's: a **gadget** is one of those
pre-compiled assembly routines; a **block** is one `struct jit_block`, holding
the gadget-address array translated from one run of guest instructions; and
**dispatch** is the act of moving from one gadget to the next.

## 6.2 Dispatch is five instructions, and two of them are contentious

Every gadget ends by invoking one macro, `gret`, defined in
[jit/gadgets-aarch64/gadgets.h](../../jit/gadgets-aarch64/gadgets.h):

```asm
.macro gret pop=0
    ldr x8, [_ip, \pop*8]!
    dmb ishld
    add _ip, _ip, 8
    cbnz x8, 0f      /* null gadget safety: if non-null, execute normally */
    b jit_ret        /* null gadget: bail safely */
0:  br x8
.endm
```

`_ip` is `x28`, and it is the *gadget* program counter — a pointer into the
block's array, entirely separate from the guest's own instruction pointer. The
guest's registers live in host registers for the duration: on aarch64,
`eax` is `w20`, `ecx` is `w22`, `esp` is `w27`, and so on down the file, with
`_cpu` in `x1` and `_tlb` in `x2`. A gadget that adds two guest registers is
therefore an `add` between two host registers, and the surrounding machinery is
this macro.

The `cbnz`/`b jit_ret` pair is defensive: a null entry in the array means
something has gone wrong upstream, and branching to address zero produces a
crash report that names nothing. Bailing out produces one that names the
emulator.

And then there is `dmb ishld`, a memory barrier, on the hot path of every single
guest instruction. It looks exactly like the sort of thing that should be
optimized away — `ldr` followed by `dmb ishld` is what `ldar` means, and `ldar`
is one instruction instead of two.

> **The bug that taught us this**
>
> It was tried. Commit `861da1d1` made the change; `95140ab7` reverted it. The
> `ldar` version is correct, and on Apple silicon it is faster — 4.3% on an i386
> shell loop.
>
> On an A9 iPad it was a **2.04x regression**: two builds differing only in that
> change, running the same i386 chroot `sh` loop of 20,000 iterations, measured
> 8,422 ms with `ldr`+`dmb` and 17,203 ms with `ldar`. Apple's cores retire
> `ldar` nearly free. The A9 evidently implements load-acquire far more
> conservatively, and this sequence runs once per guest instruction, so a
> per-dispatch acquire is brutal there.
>
> Old devices are the point of this project, so they win. The choice is exposed
> as a build option — `-Darm64_gret=dmb|ldar` — with `dmb` the default, and the
> comment in the header records the numbers so that the next person to notice
> the "obvious" simplification gets the answer without buying an iPad.
>
> The honest footnote, which is also in the tree: the fully-translated guests
> (`jit/guest-arm64/`) still dispatch via `ldar`, so the arm64 guest may be
> paying that same penalty on ARMv8.0 hardware. Nobody has A/B'd it on an
> ARMv8.0 device. It is written down as an open question rather than assumed
> either way.

## 6.3 Entering and leaving

`jit_enter` is assembly, in [jit/gadgets-aarch64/entry.S](../../jit/gadgets-aarch64/entry.S),
and it is short enough to read whole: push the callee-saved registers, point
`_ip` at `block->code`, point `_tlb` at the TLB's entry array, `load_regs` to
pull the guest's register file into host registers, and `gret` into the first
gadget. Execution does not return to C until something forces it to.

The C side keeps a `struct jit_frame` alive across that call, and its layout is
part of the assembly ABI — the header says "keep in sync with asm", which is
doing more work than a comment usually does. The frame holds a working copy of
`struct cpu_state`, a `value[4]` buffer for cross-page accesses (32 bytes,
sized for the largest single access any gadget can produce, which is an arm64
`STP` of two 128-bit Q registers; i386 and amd64 use at most 16 of it), a
`last_block` pointer, a chain budget, and the return cache.

Leaving happens for one of a small number of reasons: the guest made a syscall,
the guest faulted, a timer or signal poked this CPU, the chain budget expired,
or another thread wants to free blocks. Everything else stays inside.

## 6.4 Finding the block

Guest execution is a loop in `cpu_step_to_interrupt` that asks, over and over,
"what is the block at this guest ip?" There are three answers, in increasing
order of cost.

**A per-entry direct-mapped cache.** `cache[JIT_CACHE_SIZE]` — 1024 entries,
hashed from the ip, held in per-thread scratch. A hit is a load and two
compares, with no locks at all. It is per-*entry* rather than global on purpose:
the hot path must not touch shared state.

**The block hash table.** On a miss, take `jit->lock`, and look the address up
in the jit's chained hash table, which resizes as the working set grows. There
is one jit per address space (`struct jit` hangs off `struct mmu`), so a `fork`
shares translations until the address spaces diverge and an `execve` starts
over with a fresh one.

**Compilation.** On a miss there too, translate: decode guest instructions from
the current ip until the block ends — at a branch, at a page boundary, or at
something the translator declines to handle — appending gadget addresses as it
goes.

Note what happens around that third case in the code: the jit lock and the
jetsam read lock are *released* before `jit_block_compile` runs and re-acquired
after. Compilation allocates memory, and under iOS's debug malloc (guard pages,
scribbling) allocation is slow enough that holding a lock across it starved
other threads and could chain-deadlock between goroutines contending malloc's
own zone locks. The general rule that produced: never hold a JIT lock across an
allocation.

## 6.5 Chaining, and the budget that makes loops safe

A block that ends in a direct branch does not need to go back to C at all. Each
block records `jump_ip[2]` — pointers to the ip slots in its own final gadget —
and when the target block is known, those slots are patched to point straight at
it. `jumps_from[2]` records the reverse edges so that freeing a block can unlink
its predecessors.

Forward chaining is easy. Backward chaining — a loop — is where it gets
interesting, because a chained loop *never returns to C*. The cycle counter
never advances, so the guest is never preempted; pokes are never checked, so
signals and timers never arrive; and a thread that wants to free blocks waits
forever for a reader that has no reason to stop.

The answer is a budget, in the frame:

```asm
    ldr x8, [_cpu, LOCAL_chain_budget]
    subs x8, x8, 1
    str x8, [_cpu, LOCAL_chain_budget]
    b.le poke
```

8,192 chained dispatches, reset before every `jit_enter`, decremented on each
block-to-block chain, and on expiry the chain exits to C so the cycle counter
fires, pokes are seen, and lock waiters get their turn. The arm64, riscv64 and
amd64 engines all use it.

The i386 engine does not have one, and so it forbids backward links outright and
pays an exit-to-C round trip on every iteration of every loop. That is a real,
measurable cost that the newer engines do not pay, sitting in the tree next to
the mechanism that fixes it — a good example of how a system of this age carries
its own history in its performance profile.

## 6.6 The return cache, and an optimization that can fail silently

Indirect branches cannot be chained, because the target is not known until
runtime. A `ret` is the overwhelmingly common case, and a return cache handles
it: a 4,096-entry table in the frame, keyed by guest address, mapping a return
target to the block that starts there. The gadget hashes the target, probes the
table, validates the entry, and dispatches without leaving assembly.

Two different hash functions live in [jit/frame.h](../../jit/frame.h), and the
reason is worth the detour. i386 uses a plain bitfield of the address, bits 4
through 15. The link-register engines — arm64's `br`/`blr`/`ret`, riscv64's
`jalr` — cannot, because their code addresses are 2- and 4-byte aligned, so
i386's scheme would put every call site within a single 16-byte window into the
same bucket, and in those guests a 16-byte window is a run of consecutive
instructions. They use instead:

```c
#define LINKREG_RET_CACHE_HASH(ip) \
    ((size_t) ((((ip) ^ ((ip) >> 12)) >> 1) & (JIT_RETURN_CACHE_SIZE - 1)))
```

which the gadgets spell as `eor xN, target, target, lsr #12` followed by
`ubfx xN, xN, #1, #12`.

> **The bug that taught us this**
>
> Those two spellings — the C macro and the assembly — must match bit for bit,
> and the comment in the header says exactly what happens when they do not:
>
> A one-bit disagreement is **not a correctness bug**. Entries self-validate, so
> a wrong bucket simply never hits, the lookup falls through to the slow path,
> and the guest computes the right answer. What you get is a silent 0% hit rate.
>
> Which is indistinguishable from "the optimization did not pay off". Every
> correctness test still passes. The benchmark shows nothing. The feature is
> quietly dead and the evidence says it was never worth much.
>
> The same failure shape bit the frame layout: the cache size is hardcoded in
> the gadgets as a 12-bit extract (`ubfx ..., 12`, `andl $0xfff`), and shrinking
> the array to 64 entries without editing every gadget indexed past the end of
> the frame — at which point "the guest silently produced nothing".
>
> The lesson generalizes past this table, and Chapter 36 makes it a rule: an
> optimization whose failure mode is *silence* needs a counter that proves it
> is working, not just a benchmark that says it did not help.

## 6.7 Invalidation, and the free list that stands in for RCU

Translations depend on guest memory. Guest memory changes: a program writes to
its own code, `munmap` removes a page, a copy-on-write fault re-backs one,
`execve` replaces everything.

The dependency is recorded where it is cheapest to find. Every `struct pt_entry`
carries `struct list blocks[2]` (Chapter 5), and every block records itself on
the pages it was compiled from, so `jit_invalidate_page` can walk exactly the
translations that page produced. `jit_invalidate_range` and
`jit_invalidate_all` are the coarser hammers, and memory.c calls them in concert
with the mmu change counter.

Invalidating is not freeing, because another thread may be executing the block
right now. The invalidated block is disconnected — unlinked from the hash table,
its predecessors' jump slots restored to their original values — marked
`is_jetsam`, and put on a list:

```c
// list of jit_blocks that should be freed soon (at the next RCU grace
// period, if we had such a thing)
struct list jetsam;
```

There is no RCU, so the grace period is built by hand. `jetsam_lock` is a
reader-writer lock that every executing thread holds for read while inside
`jit_enter`. A thread that wants to free sets `write_wanted` first, which every
running engine checks at each block boundary — that is what `jit_ret_chain` is
doing when it loads `CPU_poked_ptr` — so readers exit promptly instead of being
waited out. Then the writer takes the lock for write, frees the jetsam list,
bumps `cleanup_seq`, and drops it. Any thread holding a `last_block` pointer
compares `cleanup_seq` against its own sample and discards a stale pointer
rather than following it.

Teardown is the one asymmetric case, and it is asymmetric because of a bug.
`jit_teardown_lock` acquires the write lock and **never releases it** — because
`jit_free` frees the struct the lock lives inside, so an unlock after `jit_free`
is a use-after-free. That happened. The two safe shapes are "free while holding"
and "never acquired", and the function's contract says so in the header.

## 6.8 What happens when compilation runs out of memory

The JIT lives inside an application with a memory budget that the operating
system enforces by killing it (Chapter 1). `jit_block_compile` returning NULL is
therefore an expected condition, not an assertion, and the response is a ladder:

1. Free the jetsam list — blocks already invalidated and merely awaiting a grace
   period — and retry.
2. Flush the entire cache for this address space, `printk` that it happened, and
   retry.
3. Give up on this task specifically: `printk`, and return `INT_GPF`, which
   kills the guest process rather than the app.

Inside `gen()` itself the same problem is handled with the `_longjmp` seen at the
top of this chapter, which unwinds out of a half-built block without leaking it.

Every step is logged, because a JIT that quietly discards its cache under memory
pressure looks exactly like a JIT that is inexplicably slow.

## 6.9 Crash recovery, and a 35% measurement

Translated code faults. Sometimes the guest genuinely dereferenced a bad
pointer, and that must become a guest `SIGSEGV` with an accurate `si_addr`.
Sometimes the fault is the emulator's own: a host address went stale under a
concurrent mapping change on another thread.

Each task thread installs a Mach exception handler for `EXC_BAD_ACCESS`, scoped
to that thread, once per thread lifetime. The handler redirects the faulting
thread to `jit_crash_fn`, which releases whichever JIT lock was registered and
`siglongjmp`s back to a landing pad in `cpu_step_to_interrupt`. On the CLI,
where the Mach handler is not active, a POSIX `SIGBUS` handler translates the
same events.

Two details in that path are worth pulling out, because both are the kind of
thing that only shows up under measurement.

The first is the `sigsetjmp(jit_crash_unwind_buf, 0)`. That zero means "do not
save the signal mask", and it is not a style choice: the Mach handler redirects
the thread with `thread_set_state` without touching the mask, so there is
nothing to restore. Saving it cost a `sigprocmask` and `sigaltstack` pair on
**every block-dispatch entry**, which measured at roughly **35% of host time for
a syscall-heavy guest**. A guest syscall means leaving translated code and
coming back, so "per entry" is very nearly "per syscall".

The second is the stale-TLB heal. If a host fault reverse-maps to no guest page
at all, this thread's cached host translation went stale under a concurrent
COW, `mmap` or `munmap` — the guest page is usually still valid under new
backing. Delivering `SIGSEGV` there kills an innocent guest process, and with
the meaningless `si_addr` of 0 that this path produces. So instead: flush the
TLB, clear the caches, and re-execute from the faulting ip, capped at eight
retries so a genuine repeat cannot spin. A transient race heals invisibly, and
a genuinely bad guest address refaults through the normal translation path as a
clean guest `SIGSEGV` with a real address.

## 6.10 Fusion, and why the switches are readable at runtime

Dispatch costs about 6.8 nanoseconds, and it costs that whether the gadget
underneath is an `add` or a `div`. The direct consequence — pursued properly in
Chapter 38 — is that the way to go faster is to dispatch fewer times, which
means one gadget doing what two used to.

That is instruction fusion, and each guest has its own families:

| guest | families |
|---|---|
| i386 | `ADDR`, `MOVMR`, `LEA`, `ALU`, `PUSHPOP` |
| arm64 | `BCOND` (compare + branch), `LDST` (load/store RMW), `LDCMP` (load + compare), `RETCACHE` |
| riscv64 | `FOLD` (`lui`/`auipc` + `addi`/load), `JAL` (link write + branch in one), `RETCACHE` |
| amd64 | `INCDEC_REG` |

Both sides of every fusion live in the same binary: clearing a bit makes `gen`
emit the unfused multi-dispatch expansion instead. And the bits are readable and
writable at runtime, through `/proc/ish/<arch>_jit_fuse`:

```sh
cat /proc/ish/arm64_jit_fuse       # one "name on|off" line per family
echo retcache=0 > /proc/ish/arm64_jit_fuse
echo all=1 > /proc/ish/riscv64_jit_fuse
```

The reasoning behind that design is the most transferable thing in this chapter,
and it is spelled out in [jit/jit.h](../../jit/jit.h). Making the switches
environment variables would have been easier. But an environment variable can
only be changed by relaunching, which on a device means killing the app — and
with it the ssh session you were measuring through, and the guest's `/tmp` — and
it cannot be read back to prove it took effect. Those are two ways to silently
produce a meaningless "flat" A/B result, and both had already happened.

With a proc node, the value `gen.c` consults and the value `cat` prints are the
same variable, so a read-back is proof of plumbing *by construction*, and
flipping arms costs a file write instead of a launch cycle. That makes
rep-level interleaving possible, which is the only way to keep thermal drift and
host load out of a result in the 1–2% range.

One rule comes with it: the bits are consumed at translation time, so a change
affects newly compiled blocks only. Already-translated blocks keep what they
were built with. A freshly `exec`'d process gets a fresh address space and
therefore a fresh jit, so running each benchmark repetition as its own process
is enough to pick up a change — and nothing sweeps live blocks on a proc write,
deliberately, since that would buy nothing for measurement and is a concurrency
hazard.

`tests/manual/jit_fuse_ab.sh` automates an interleaved A/B and restores the mask
when it exits.

## 6.11 What it adds up to

A guest instruction costs one dispatch — a load, a barrier, a compare, a branch,
and whatever the gadget itself does — plus, for memory operations, a TLB probe.
There is no register allocator, no peephole optimizer over host code, no
scheduling. There cannot be: the host instructions were all chosen at build
time, and the only decisions available at translation time are *which* gadgets
and *how many*.

That constraint sets the shape of every performance chapter that follows.
Fusion reduces the count. The return cache and chaining remove exits. High-level
emulation (Chapter 8) removes whole *functions* worth of dispatches at once. And
native programs (Part V) remove the guest instructions entirely, by not being
guest instructions.

It also sets the shape of the correctness chapters, because the thing that makes
this design tractable — every host instruction is fixed at build time — is also
what makes a mistake in it so quiet. A wrong gadget address is a crash you can
find. A wrong *hash* is a feature that silently never works, and a wrong
ordering at a page boundary is a corruption that appears once every few million
instructions. Chapter 9 is about how any of this gets proven.

---

*Anchors:* [jit/gen.c](../../jit/gen.c), [jit/jit.c](../../jit/jit.c),
[jit/jit.h](../../jit/jit.h), [jit/frame.h](../../jit/frame.h),
[jit/gadgets-aarch64/gadgets.h](../../jit/gadgets-aarch64/gadgets.h),
[jit/gadgets-aarch64/entry.S](../../jit/gadgets-aarch64/entry.S),
[jit/offsets.c](../../jit/offsets.c), [emu/memory.h](../../emu/memory.h),
`tests/manual/jit_fuse_ab.sh`, commits `861da1d1` and `95140ab7`.

*Story:* `ldar` versus `ldr`+`dmb ishld` — one instruction fewer, 4.3% faster on
Apple silicon, and a 2.04x regression on an A9 iPad (8,422 ms → 17,203 ms on the
same shell loop).
