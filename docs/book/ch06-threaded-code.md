# 6. Threaded code: the JIT that is not a JIT

This chapter is the centre of the book. Everything in Part III and Part IV
describes a kernel that could, in principle, have been written for any emulator;
this is the part that explains why iSH-AOK exists at all on a platform that
forbids the technique its performance depends on.

It is also the chapter where the vocabulary has to be right, because the tree
uses words like *gadget* and *block* and *dispatch* with precise meanings, and
because the design only looks clever once you know what the alternatives were.
So the mechanism is held back for a few pages: first the terms, then the theory
of why emulators are slow and what is normally done about it, then what AOK
actually does.

## 6.1 A vocabulary

**Guest instruction.** One instruction of the emulated architecture — an i386
`add`, an arm64 `ldp`. The unit the guest's compiler emitted and the unit the
emulator must be faithful to.

**Basic block.** A run of guest instructions with one entry at the top and one
exit at the bottom: execution enters at the first instruction and leaves after
the last, with no branches into or out of the middle. Every emulator that
translates anything works in blocks, because a block is the largest unit whose
control flow is known statically.

**Interpreter.** A loop that reads one guest instruction, works out what it
means, does it, advances the guest program counter, and repeats. Simple,
obviously correct, and slow for reasons Section 6.2 makes precise.

**Translation.** Converting a block of guest instructions, once, into something
cheaper to execute repeatedly. **Translation time** is when that happens; **run
time** is every subsequent execution of the result. The distance between those
two clocks is the entire economy of this chapter: work moved from run time to
translation time is work paid once instead of a million times.

**Translation cache.** The table that maps a guest address to its already
translated form, so the second execution of a loop body does not re-translate
it. In AOK this is `struct jit`, and there is one per address space.

**Gadget.** AOK's name for one small routine, written in assembly, that performs
one piece of guest semantics — load a register, add two values, compute a memory
address, take a branch. Gadgets are ordinary functions in the application
binary: compiled by the normal toolchain, linked, and code-signed at build time
along with everything else.

**Block, in AOK's sense.** One `struct jit_block`: the translated form of one
basic block, holding an array of gadget addresses and inline operands.

**Dispatch.** Moving from one gadget to the next. This is the operation whose
cost dominates everything, and Section 6.7 measures it.

**Chaining.** Patching one translated block to jump directly to another, instead
of returning to the C loop that decides what to run next.

**W^X.** "Write xor execute": the policy that a page of memory may be writable
or executable but not both. It is a security measure — it prevents an attacker
who can write memory from executing what they wrote — and it is enforced for
third-party applications on iOS. It is also, precisely, the thing that makes a
conventional JIT impossible here.

## 6.2 Why interpreting is slow, in detail

The classic interpreter loop, in the shape every emulator starts with:

```c
for (;;) {
    uint32_t insn = read_guest_memory(pc);   // fetch
    switch (decode(insn)) {                  // decode + dispatch
        case OP_ADD: regs[rd] = regs[rn] + regs[rm]; break;
        case OP_LDR: regs[rt] = read_guest_memory(regs[rn] + imm); break;
        ...
    }
    pc += 4;
}
```

Ask what fraction of that is the guest's work. For `OP_ADD`, the guest's work is
one host `add`. Everything else — the fetch, the decode, the switch, the pc
update, the loop back-edge — is overhead, and it does not get smaller for
simple instructions. A ten-to-one ratio of overhead to work is normal, and the
overall slowdown of a careful interpreter against native execution is usually
quoted in the range of ten to a hundred times.

Two specific wastes are worth separating, because the fixes are different.

**Decode is repeated.** A loop body executing ten million times is decoded ten
million times, and the answer is the same every time. The instruction bytes did
not change; the emulator simply threw away what it learned.

**The dispatch is an unpredictable indirect branch.** A `switch` over opcodes
compiles to an indirect jump through a table. Modern processors predict indirect
branches by remembering where they went last time — and this one goes wherever
the *guest program* goes next, which is precisely what the predictor cannot
know. Every dispatch risks a misprediction and the pipeline flush that follows,
which on a deep out-of-order core costs more than the instruction being
emulated. Interpreter implementers have fought this for decades; replicating the
dispatch at the end of every case (so each opcode gets its own branch site with
its own prediction history) is a well-known partial remedy, and it is a hint of
where this chapter is going.

## 6.3 What a JIT does, and the door that is closed

The standard answer is to stop interpreting and start compiling. Translate a
basic block of guest code into a block of *host* instructions, write those
instructions into a page of memory, mark the page executable, and jump to it.
Decode happens once. The dispatch disappears entirely, because the host's own
program counter walks the translated code. A register allocator can keep guest
registers in host registers across many instructions; a peephole pass can fold
the flag computation nobody reads.

This is what QEMU does, what every JavaScript engine does, and what a
hypothetical iSH-AOK would do if it could. The cost is that it requires writing
instructions into memory and then executing them, which is the definition of a
W^X violation.

iOS grants that capability through an entitlement. WebKit has it, because a web
browser without a JavaScript JIT is not competitive. Applications running under
a debugger get it. Ordinary App Store applications do not, and no amount of
cleverness in the application gets it — the check is in the kernel, not in the
toolchain.

So: interpretation is available and slow. Compilation is fast and unavailable.
The interesting question is whether there is anything in between.

## 6.4 Threaded code

There is, and it is older than both.

Forth implementations in the 1970s faced a different constraint with the same
shape: they needed to represent a compiled program compactly on machines with
very little memory, and they could not afford a full native code generator. The
technique they arrived at is called **threaded code**, and its central idea is
this: represent the compiled program as a *list of addresses of routines*,
rather than as machine instructions.

A word in Forth compiles to a sequence of addresses. Executing it means walking
that sequence: load the next address, jump to it, and let the routine at that
address end by doing the same thing again — the operation Forth calls `NEXT`.
There is no dispatch table and no decode, because the "program" is already a
list of exactly the routines to run, in order.

The family has several members, and the differences matter here:

- **Direct threading.** The list holds the addresses of the routines themselves.
  `NEXT` is a load and an indirect jump. Fastest of the variants, and the one to
  keep in mind for the rest of this chapter.
- **Indirect threading.** The list holds addresses *of* addresses, buying a
  level of indirection that Forth used for data structures. One more load per
  dispatch.
- **Token threading.** The list holds small integers, mapped through a table to
  routine addresses. Most compact, slowest — it reintroduces the table lookup
  that direct threading removed.
- **Subroutine threading.** The list is made of actual `call` instructions to
  each routine, so the host's own program counter walks it. This is the fastest
  of all and, for our purposes, the one that is unavailable: emitting `call`
  instructions is emitting instructions.

What does threaded code buy relative to an interpreter? Decode is gone —
performed once, when the list was built. Specialization becomes possible: at
build-list time you know the operand sizes and the register numbers, so you can
select a routine specialized for exactly that case rather than one that branches
on it at run time. And the branch-prediction picture improves, because each
routine's terminating jump is its own branch site with its own history, rather
than one shared dispatch point that every opcode funnels through.

What does it not buy, relative to real compilation? The per-operation dispatch
remains: one load and one indirect jump for every operation, forever. There is
no register allocation across operations, no scheduling, no peephole
optimization over the host instruction stream, because there is no host
instruction stream to optimize — only a fixed set of routines chosen at build
time.

Threaded code is therefore the middle term exactly: it eliminates the
interpreter's repeated decode without requiring the compiler's runtime code
emission. Its historical motivation was code density on small machines. Its
motivation here is a code-signing policy on large ones. Same mechanism, entirely
different reason — which is a fairly common shape in systems programming, and
worth noticing when it happens.

## 6.5 The reveal: the code generator is one assignment

Here, in full, is iSH-AOK's code generator:

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

A bounds check, a `realloc`, and an assignment into `unsigned long code[]`, the
flexible array member at the end of `struct jit_block`. Translating a guest
instruction means calling it a few times:

```c
gen(state, (unsigned long) gadget_amd64_jmp);
gen(state, (unsigned long) (target_ip | (1ull << 63)));
```

This is direct threading, with inline operands: the array holds gadget
addresses interleaved with the immediates those gadgets consume. `gadget_amd64_jmp`
is the address of a function in the application binary, produced by the ordinary
build and signed with everything else. The array it is stored into is a `malloc`
allocation holding integers.

Nothing anywhere in this system is made executable that was not executable when
the app was signed. The platform's objection is to *creating* executable code;
AOK creates a list, and the list refers to code that already existed. The
`realloc` in the middle of that function is the entire "code cache management"
story, and there is no `mprotect` call in it because there is nothing to
protect.

That is the loophole, and it is not a trick played on the platform. Byte for
byte, the executable pages of iSH-AOK are exactly what Apple's build and
signing process produced. What varies at run time is the order in which they are
visited.

## 6.6 What a gadget actually looks like

Abstractions are easier to believe with an example. This is the arm64 guest's
logical-immediate gadget — `AND`, `ORR`, `EOR`, `ANDS` with an encoded bitmask
operand — lightly trimmed:

```asm
// Code stream: [gadget][rd | rn<<8 | opc<<16 | sf<<24][imm (64-bit)]

.gadget logical_imm
    ldr x8, [_ip]           // the packed operand word
    ldr x11, [_ip, #8]      // imm, already decoded at compile time
    add _ip, _ip, #16

    and x9, x8, #0x1f       // rd
    ubfx x10, x8, #8, #5    // rn
    ubfx x12, x8, #16, #2   // opc: 0=AND,1=ORR,2=EOR,3=ANDS
    ubfx x13, x8, #24, #1   // sf
    ...
```

Three things in that fragment carry the whole design.

**The operands are inline.** `_ip` is the gadget program counter — a pointer
into the block's array — and the gadget reads its own operands out of the array
immediately after itself, then steps past them. This is what "inline operands"
means concretely, and it is why the array holds `unsigned long` rather than a
struct: gadget addresses and operand words are the same kind of thing to the
walker.

**Work has moved to translation time.** The comment above the gadget in the tree
is explicit: ARM's bitmask immediate encoding (`immN:imms:immr` expanding to a
64-bit pattern, the operation the architecture manual calls `DecodeBitMasks`) is
decoded *once*, at translation time, by `gen_step_arm64` calling a shared decode
helper — and the resulting 64-bit pattern is packed straight into the code
stream. So this gadget never implements `DecodeBitMasks` at all, "unlike a real
hardware decoder". Every execution after the first gets the answer for free.
That is the translation-time/run-time economy of Section 6.1, in one instruction.

**The host does the semantics.** `ANDS`'s flag behaviour — N and Z from the
result, C and V always cleared — falls out of using the host's own native `ands`
instruction, because this is an arm64 guest on an arm64 host. No special-casing
is needed. This is exactly what Chapter 7 means when it says a matching ISA
family makes each *gadget body* cheaper.

Not every operation is worth writing in assembly. A gadget can call into C when
it needs to, through a pair of macros that save and restore the caller-saved
host registers around the call:

```asm
.gadget cpuid
    # regrettable
    save_c
    ...
    bl NAME(helper_cpuid)
    ...
    restore_c
    gret
```

The comment `# regrettable` is doing honest work there. `cpuid` is rare, its
semantics are a large table, and nobody wants that table in assembly — so it
bridges to C, at the cost of a save/restore around every execution.

The amd64 engine takes that trade much further, and the consequences show up in
Chapter 7's benchmark table. Its translator emits real assembly gadgets for the
shapes that run hot, and for the long tail it emits a bridge to a C helper that
re-decodes the instruction at run time. The tree is clear-eyed about what that
costs. Here is the note attached to the gadget that replaced one such bridge —
register-operand `INC`/`DEC`, which in long mode is *every* `incq`, because the
one-byte `0x40+r` encodings that used to be `INC` are the REX prefixes:

> Previously this bridged to `amd64_jit_ff_group`, which cost three things per
> execution: a `gadget_amd64_set_rip` dispatch (the helper re-decodes at
> `CPU_amd64_rip`, so the bridge emitter must flush the deferred rip), the
> bridge dispatch, and a C helper that re-walked prefixes and re-decoded the
> modrm the translator had already decoded. This gadget removes all three.

Three costs, all of them re-doing work the translator had already done. That is
the same economy again, seen from the other side: a bridge to C is a decision to
keep paying at run time for something translation time already knew.

## 6.7 Dispatch is five instructions, and two of them are contentious

Every gadget ends by invoking one macro, `gret`:

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

That is `NEXT`. Load the next gadget address, step the gadget program counter,
and branch to it.

The guest's registers live in host registers for the duration of a block: on
aarch64, `eax` is `w20`, `ecx` is `w22`, `esp` is `w27`, `_ip` is `x28`, with
`_cpu` in `x1` and `_tlb` in `x2`. A gadget that adds two guest registers is an
`add` between two host registers plus this macro. The `load_regs` and
`save_regs` macros move the whole file in and out around block entry and exit.

The `cbnz`/`b jit_ret` pair is defensive. A null entry in the array means
something upstream went wrong, and branching to address zero produces a crash
report that names nothing at all; bailing out produces one that names the
emulator.

And then there is `dmb ishld`, a memory barrier, on the hot path of every guest
instruction. It looks exactly like the kind of thing that should be optimized
away: `ldr` followed by `dmb ishld` is what `ldar` means, and `ldar` is one
instruction instead of two.

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
> as a build option — `-Darm64_gret=dmb|ldar`, with `dmb` the default — and the
> numbers live in the header so the next person to notice the "obvious"
> simplification gets the answer without buying an iPad.
>
> The honest footnote, also in the tree: the fully-translated guests
> (`jit/guest-arm64/`) still dispatch via `ldar`, so the arm64 guest may be
> paying that same penalty on ARMv8.0 hardware. Nobody has A/B'd it on an
> ARMv8.0 device. It is written down as an open question rather than assumed
> either way.

## 6.8 Entering and leaving

`jit_enter` is assembly, and short enough to read whole: push the callee-saved
host registers, point `_ip` at `block->code`, point `_tlb` at the TLB's entry
array, `load_regs` to pull the guest's register file into host registers, and
`gret` into the first gadget. Execution does not return to C until something
forces it to.

The C side keeps a `struct jit_frame` alive across that call, and its layout is
part of the assembly ABI — the header says "keep in sync with asm", which is
carrying more weight than a comment usually does. The frame holds a working copy
of `struct cpu_state`, a `value[4]` buffer for cross-page accesses (32 bytes,
sized for the largest single access any gadget can produce, an arm64 `STP` of
two 128-bit Q registers; i386 and amd64 use at most 16 of it), a `last_block`
pointer, a chain budget, and the return cache.

Leaving happens for a small, enumerable set of reasons: the guest made a
syscall, the guest faulted, a timer or signal poked this CPU, the chain budget
expired, or another thread wants to free blocks. Everything else stays inside.

## 6.9 Finding the block

Guest execution is a loop in `cpu_step_to_interrupt` that repeatedly asks: what
is the block at this guest ip? There are three answers, in increasing order of
cost.

**A per-entry direct-mapped cache.** `cache[JIT_CACHE_SIZE]` — 1024 entries,
hashed from the ip, held in per-thread scratch. A hit is a load and two
compares, with no locks at all. It is per-*entry* rather than global on purpose:
the hot path must not touch shared state.

**The block hash table.** On a miss, take `jit->lock` and look the address up in
the jit's chained hash table, which resizes as the working set grows. There is
one jit per address space, so a `fork` shares translations until the address
spaces diverge and an `execve` starts over with a fresh one.

**Compilation.** On a miss there too, translate. Decoding runs forward from the
current ip until the block ends, which happens at a branch, at a page boundary —
a block may not span pages, because the second page might not be mapped and
because invalidation is tracked per page (Section 6.12) — or at an instruction
the translator declines to handle.

Note what surrounds that third case in the code: the jit lock and the jetsam
read lock are *released* before `jit_block_compile` runs and re-acquired after.
Compilation allocates, and under iOS's debug malloc (guard pages, scribbling)
allocation is slow enough that holding a lock across it starved other threads,
and could chain-deadlock between threads contending malloc's own zone locks. The
rule that came out of it: never hold a JIT lock across an allocation.

## 6.10 Chaining, and the budget that makes loops safe

A block ending in a direct branch need not return to C at all. Each block
records `jump_ip[2]` — pointers to the ip slots in its own final gadget — and
when the target block is known those slots are patched to point straight at it.
`jumps_from[2]` records the reverse edges, so that freeing a block can unlink
its predecessors.

Forward chaining is easy. Backward chaining — a loop — is where it gets
interesting, because a chained loop *never returns to C*. The cycle counter
never advances, so the guest is never preempted; pokes are never checked, so
signals and timers never arrive; and a thread that wants to free blocks waits
forever on a reader with no reason to stop.

The answer is a budget, kept in the frame:

```asm
    ldr x8, [_cpu, LOCAL_chain_budget]
    subs x8, x8, 1
    str x8, [_cpu, LOCAL_chain_budget]
    b.le poke
```

8,192 chained dispatches, reset before every `jit_enter`, decremented on each
block-to-block chain; on expiry the chain exits to C so the cycle counter fires,
pokes are seen, and lock waiters get their turn. Every engine uses it.

The i386 engine gained backward-edge linking in August 2026 for exactly this
reason — an unlinked backward edge did not merely round-trip to C, it re-took
`jit->lock` on every loop iteration only to rediscover the edge and refuse it,
which put `pthread_mutex_lock` in the profile of a pure-compute guest loop.
`ISH_I386_NOBACKCHAIN=1` restores the old forward-only behaviour for bisection.

## 6.11 The return cache, and an optimization that can fail silently

Indirect branches cannot be chained, because the target is not known until run
time. A `ret` is the overwhelmingly common case, and a return cache handles it:
a 4,096-entry table in the frame, keyed by guest address, mapping a return
target to the block that starts there. The gadget hashes the target, probes the
table, validates the entry, and dispatches without ever leaving assembly.

Two different hash functions live in `jit/frame.h`, and the reason is worth the
detour. i386 uses a plain bitfield of the address, bits 4 through 15. The
link-register engines — arm64's `br`/`blr`/`ret`, riscv64's `jalr` — cannot,
because their code addresses are 2- and 4-byte aligned, so i386's scheme would
drop every call site within a single 16-byte window into one bucket, and in
those guests a 16-byte window is a run of consecutive instructions. They use:

```c
#define LINKREG_RET_CACHE_HASH(ip) \
    ((size_t) ((((ip) ^ ((ip) >> 12)) >> 1) & (JIT_RETURN_CACHE_SIZE - 1)))
```

which the gadgets spell as `eor xN, target, target, lsr #12` followed by
`ubfx xN, xN, #1, #12`.

> **The bug that taught us this**
>
> Those two spellings — the C macro and the assembly — must match bit for bit,
> and the header says exactly what happens when they do not.
>
> A one-bit disagreement is **not a correctness bug**. Entries self-validate, so
> a wrong bucket simply never hits, the lookup falls through to the slow path,
> and the guest computes the right answer. What you get is a silent 0% hit rate.
>
> Which is indistinguishable from "the optimization did not pay off". Every
> correctness test still passes. The benchmark shows nothing. The feature is
> quietly dead, and the evidence says it was never worth much.
>
> The same shape bit the frame layout: the cache size is hardcoded in the
> gadgets as a 12-bit extract (`ubfx ..., 12`, `andl $0xfff`), and shrinking the
> array to 64 entries without editing every gadget indexed past the end of the
> frame — at which point "the guest silently produced nothing".
>
> The lesson generalizes past this table, and Chapter 36 makes it a rule: an
> optimization whose failure mode is *silence* needs a counter that proves it is
> working, not just a benchmark that says it did not help.

## 6.12 Invalidation, and the free list that stands in for RCU

Translations depend on guest memory, and guest memory changes: a program writes
to its own code, `munmap` removes a page, a copy-on-write fault re-backs one,
`execve` replaces everything.

The dependency is recorded where it is cheapest to find. Every `struct pt_entry`
carries `struct list blocks[2]` (Chapter 5), and every block records itself on
the pages it was compiled from, so `jit_invalidate_page` can walk exactly the
translations that page produced. `jit_invalidate_range` and `jit_invalidate_all`
are the coarser hammers, and `memory.c` calls them in concert with the mmu
change counter.

Invalidating is not freeing, because another thread may be executing the block
right now. The invalidated block is disconnected — unlinked from the hash table,
its predecessors' jump slots restored to their original values — marked
`is_jetsam`, and put on a list whose declaration is admirably direct:

```c
// list of jit_blocks that should be freed soon (at the next RCU grace
// period, if we had such a thing)
struct list jetsam;
```

There is no RCU, so the grace period is built by hand. `jetsam_lock` is a
reader-writer lock that every executing thread holds for read while inside
`jit_enter`. A thread that wants to free sets `write_wanted` first, and every
running engine checks it at each block boundary — that is what `jit_ret_chain`
is doing when it loads `CPU_poked_ptr` — so readers exit promptly instead of
being waited out. Then the writer takes the lock for write, frees the jetsam
list, bumps `cleanup_seq`, and drops it. Any thread holding a `last_block`
pointer compares `cleanup_seq` against its own sample and discards a stale
pointer rather than following it.

Teardown is the one asymmetric case, and it is asymmetric because of a bug.
`jit_teardown_lock` acquires the write lock and **never releases it** — because
`jit_free` frees the struct the lock lives inside, so an unlock after `jit_free`
is a use-after-free. That happened. The two safe shapes are "free while holding"
and "never acquired", and the function's contract says so in the header.

## 6.13 What happens when compilation runs out of memory

The JIT lives inside an application with a memory budget the operating system
enforces by killing it (Chapter 1). `jit_block_compile` returning NULL is
therefore an expected condition rather than an assertion, and the response is a
ladder:

1. Free the jetsam list — blocks already invalidated and merely awaiting a grace
   period — and retry.
2. Flush the entire cache for this address space, `printk` that it happened, and
   retry.
3. Give up on this task specifically: `printk`, and return `INT_GPF`, which
   kills the guest process rather than the app.

Inside `gen()` the same problem is handled by the `_longjmp` visible at the top
of this chapter, which unwinds out of a half-built block without leaking it.

Every step is logged, because a JIT that quietly discards its cache under
memory pressure looks exactly like a JIT that is inexplicably slow.

## 6.14 Crash recovery, and a 35% measurement

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

Two details in that path are worth pulling out, because both only show up under
measurement.

The first is `sigsetjmp(jit_crash_unwind_buf, 0)`. That zero means "do not save
the signal mask", and it is not a style choice: the Mach handler redirects the
thread with `thread_set_state` without touching the mask, so there is nothing to
restore. Saving it cost a `sigprocmask` and `sigaltstack` pair on **every
block-dispatch entry**, which measured at roughly **35% of host time for a
syscall-heavy guest**. A guest syscall means leaving translated code and coming
back, so "per entry" is very nearly "per syscall".

The second is the stale-TLB heal. If a host fault reverse-maps to no guest page
at all, this thread's cached host translation went stale under a concurrent COW,
`mmap` or `munmap`, and the guest page is usually still valid under new backing.
Delivering `SIGSEGV` there kills an innocent guest process, and with the
meaningless `si_addr` of 0 that this path produces. So instead: flush the TLB,
clear the caches, and re-execute from the faulting ip, capped at eight retries
so a genuine repeat cannot spin. A transient race heals invisibly, and a
genuinely bad guest address refaults through the normal translation path as a
clean guest `SIGSEGV` with a real address.

## 6.15 Fusion, and why the switches are readable at run time

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

The amd64 entry is the gadget from Section 6.6 — the one that replaced a bridge
costing three dispatches and a re-decode. Fusion and de-bridging are the same
move seen from two angles: both remove run-time work that translation time had
already done.

Both sides of every fusion live in the same binary: clearing a bit makes `gen`
emit the unfused multi-dispatch expansion instead. And the bits are readable and
writable at run time, through `/proc/ish/<arch>_jit_fuse`:

```sh
cat /proc/ish/arm64_jit_fuse       # one "name on|off" line per family
echo retcache=0 > /proc/ish/arm64_jit_fuse
echo all=1 > /proc/ish/riscv64_jit_fuse
```

The reasoning behind that design is the most transferable thing in this chapter,
and it is spelled out in `jit/jit.h`. Making the switches environment variables
would have been easier. But an environment variable can only be changed by
relaunching, which on a device means killing the app — and with it the ssh
session you were measuring through, and the guest's `/tmp` — and it cannot be
read back to prove it took effect. Those are two ways to silently produce a
meaningless "flat" A/B result, and both had already happened.

With a proc node, the value `gen.c` consults and the value `cat` prints are the
same variable, so a read-back is proof of plumbing *by construction*, and
flipping arms costs a file write instead of a launch cycle. That makes
rep-level interleaving possible, which is the only way to keep thermal drift and
host load out of a result in the 1–2% range.

One rule comes with it: the bits are consumed at translation time, so a change
affects newly compiled blocks only. Already-translated blocks keep what they
were built with. A freshly `exec`'d process gets a fresh address space and
therefore a fresh jit, so running each benchmark repetition as its own process
picks up the change — and nothing sweeps live blocks on a proc write,
deliberately, since that would buy nothing for measurement and is a concurrency
hazard.

`tests/manual/jit_fuse_ab.sh` automates an interleaved A/B and restores the mask
when it exits.

## 6.16 The cost model, and what follows from it

Put the three approaches side by side, per guest instruction:

| | decode | dispatch | cross-instruction optimization |
|---|---|---|---|
| interpreter | every execution | one shared indirect branch | none |
| threaded code | once, at translation | one indirect branch per gadget | only what fusion buys |
| native codegen | once, at translation | none | register allocation, scheduling, peepholes |

AOK sits in the middle row, and every performance decision in this book follows
from that placement. There is no register allocator, no peephole pass over host
code, and no scheduling — there cannot be, because the host instructions were
all chosen at build time. The only decisions available at translation time are
*which* gadgets and *how many*.

So the optimizations are the ones that change those two quantities. Fusion
reduces the count directly. Chaining and the return cache remove exits back to
C. High-level emulation (Chapter 8) removes whole *functions* worth of
dispatches at a stroke. Native programs (Part V) remove the guest instructions
altogether, by not being guest instructions in the first place.

The same placement sets the shape of the correctness chapters. What makes this
design tractable — every host instruction fixed at build time — is also what
makes mistakes in it quiet. A wrong gadget address is a crash you can find. A
wrong *hash* is a feature that silently never works. A wrong ordering at a page
boundary is a corruption that shows up once every few million instructions.
Chapter 9 is about how any of this gets proven.

---

*Anchors:* [jit/gen.c](../../jit/gen.c), [jit/jit.c](../../jit/jit.c),
[jit/jit.h](../../jit/jit.h), [jit/frame.h](../../jit/frame.h),
[jit/gadgets-aarch64/gadgets.h](../../jit/gadgets-aarch64/gadgets.h),
[jit/gadgets-aarch64/entry.S](../../jit/gadgets-aarch64/entry.S),
[jit/gadgets-aarch64/misc.S](../../jit/gadgets-aarch64/misc.S),
[jit/gadgets-aarch64/math.S](../../jit/gadgets-aarch64/math.S),
[jit/guest-arm64/logical.S](../../jit/guest-arm64/logical.S),
[jit/offsets.c](../../jit/offsets.c), [emu/memory.h](../../emu/memory.h),
`tests/manual/jit_fuse_ab.sh`, commits `861da1d1` and `95140ab7`.

*Story:* `ldar` versus `ldr`+`dmb ishld` — one instruction fewer, 4.3% faster on
Apple silicon, and a 2.04x regression on an A9 iPad (8,422 ms → 17,203 ms on the
same shell loop).
