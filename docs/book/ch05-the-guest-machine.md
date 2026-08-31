# 5. The guest machine

A CPU, in this system, is a struct. Everything in this chapter is a
consequence of that sentence: what the struct contains, what it deliberately
does not contain, and the three or four places where the obvious way to
represent something turned out to cost more than the clever way.

## 5.1 Where the CPU lives

`struct cpu_state` is a member of `struct task`. Not a pointer to one — a
member. A guest process's registers live inside the same allocation as its file
descriptor table and its signal mask, and when `fork` allocates a new task it
gets a new CPU for free.

The struct holds four register files side by side, not in a union:

```c
union { struct { _REGX(a); _REGX(c); ... }; dword_t regs[8]; };  // i386
dword_t eip;
qword_t amd64_regs[amd64_reg_count];  qword_t amd64_rip;
qword_t arm64_regs[arm64_reg_count];  qword_t arm64_sp, arm64_pc;
qword_t riscv64_regs[riscv64_reg_count];  qword_t riscv64_pc;
```

An i386 task carries arm64's thirty-one general registers and riscv64's
thirty-two around with it, zeroed and untouched, for the life of the process.
That is a deliberate trade: a union would save a few hundred bytes per task and
would make every access go through a discriminator that the JIT would then have
to prove constant. Memory is not the scarce resource here; dispatch is. The
comment in the tree says as much, and Chapter 7 explains what "the guest ABI is
a field on the task" buys when four architectures have to coexist in one
address space.

The i386 file is the one piece with real structure to it, and it is structure
inherited from 1978: `_REGX(a)` expands to a union of `eax`, `ax`, `al` and
`ah`, so a gadget that writes the low byte of a register writes it directly
rather than masking and merging. This works because everything assumes little
endian, which the header says out loud — "as does literally everything".

The first member of the struct is a `struct mmu *`, and the second is a cycle
counter. Both are there because they are touched constantly and the offsets
matter: `jit/offsets.c` exports field offsets to the assembly gadgets, so the
layout of this struct is part of the JIT's ABI. Moving a field is not a
refactor, it is a change to compiled code that nothing will warn you about.

## 5.2 Flags are a promise to compute the answer later

The x86 flags register is the single most expensive thing about emulating x86
honestly. Almost every arithmetic instruction writes six flags, and almost
nothing ever reads them: the `add` in a loop body sets carry, parity, adjust,
zero, sign and overflow, and then the next instruction is another `add`.
Computing all six eagerly means doing several times more work than the guest
instruction itself.

So AOK does not compute them. It records the ingredients:

```c
byte_t cf;
byte_t of;
dword_t res, op1, op2;
union { struct { bitfield pf_res:1, zf_res:1, sf_res:1, af_ops:1; };
        byte_t flags_res; };
```

`res`, `op1` and `op2` are the result and operands of the last flag-setting
operation. `flags_res` is a four-bit ledger saying which flags are currently
*not* in the visible `eflags` word and must instead be derived — zero flag from
`res == 0`, sign flag from the top bit of `res`, parity from the low byte,
adjust from the operands. A guest that never reads the flags never pays for
them. A guest that reads one pays for that one, at the moment it asks.

Carry and overflow get their own byte-sized fields rather than living in the
bitfield, "for maximum efficiency" as the comment says: they are set by paths
that already have the value in a register, and a byte store is cheaper than a
read-modify-write of a packed word.

The arm64 guest went the other way, and the reasoning is worth following
because it is the same reasoning inverted. NZCV is stored packed into bits
31:28 of a single `dword_t`, matching the AArch64 PSTATE layout exactly, so a
gadget can move the guest's flags into the host's own condition flags with one
`msr nzcv, x` and read them back with one `mrs`. An earlier design decoded them
into four bools — one representation, no risk of the two drifting out of sync —
and it was reversed once the goal became gadget throughput rather than
interpreter clarity. The interpreter still pays a small decode cost per flag
touch, and that is fine, because the interpreter is not the performance path
(Chapter 7).

Two guests, two opposite representations, both chosen by measuring what the
translator can emit rather than what reads well in a header.

## 5.3 Memory is a page table AOK owns

The guest's address space is a two-level structure of `struct pt_entry`:

```c
struct pt_entry {
    struct data *data;      // the host memory behind this page
    size_t offset;          // where in that host allocation
    unsigned flags;         // P_READ | P_WRITE | P_COW | P_SHARED | ...
    struct list blocks[2];  // JIT blocks compiled from this page
};
```

The flags are the interesting field. `P_WRITE` and `P_COW` together do not mean
writable — `P_WRITABLE(flags)` is `flags & P_WRITE && !(flags & P_COW)`, which
is the entire copy-on-write mechanism expressed as a macro. `P_SHARED` marks a
`MAP_SHARED` mapping that must never be copied. `P_ANONYMOUS` marks a page that
came from `pt_map_nothing` rather than from a file. `P_GROWSDOWN` is a stack.
`P_WIPEONFORK` is `madvise(MADV_WIPEONFORK)`, which gives a child fresh zero
pages instead of the parent's data.

`P_READ` and `P_EXEC` are defined and ignored. That is not an oversight and it
is not free: it is why guest pages have no NX, which Chapter 13 covers as a
known gap rather than a bug to be fixed in passing.

The `blocks[2]` field is the seam between memory and the JIT. Every compiled
block records itself on the pages it was compiled from, so that unmapping,
protecting or writing to a page can find and invalidate exactly the translations
that depended on it. Chapter 6 picks that up.

Above the entries sit chunked page directories, an atomic pointer array, and a
set-only bitmap saying which roots have chunks at all — added because
`pt_find_hole` on a large, sparse amd64 address space spent its time linearly
probing 32 KiB of mostly-empty root pointers, and a bit-scan skips the empty
regions in bulk.

## 5.4 The TLB, and an optimization that was measured and rejected

Every guest load and store goes through a software TLB. It is per-thread, it
lives on the task thread's stack (`struct tlb tlb = {}` in
[kernel/task.c:796](../../kernel/task.c#L796)), it has 1024 entries, and an
entry is three words:

```c
struct tlb_entry {
    page_t page;              // tag for a read
    page_t page_if_writable;  // tag for a write; differs when the page is COW
    uintptr_t data_minus_addr;  // add the guest address, get the host pointer
};
```

The third field is the trick. Rather than storing the host page base and making
every hit compute `base + offset`, the entry stores host-base-minus-guest-base,
so a hit is a tag compare and a single add. Two tags rather than one means a
read hit and a write hit are separate questions, which is exactly what
copy-on-write needs: a COW page is readable through the TLB and takes a miss on
every write, without a flag test in the hot path.

Coherency is a counter. `struct mmu` holds `_Atomic uint64_t changes`, bumped on
every page-table change; each TLB caches its last-seen value and flushes itself
when the two differ. That is a global flush rather than a shootdown, and it is
correct for the same reason it is cheap: the array is 24 KB and refilling it is
a series of ordinary misses.

> **Measure it yourself — and then believe the measurement**
>
> `struct tlb_entry` is 24 bytes, and deliberately not padded to 32. Padding it
> looks like an obvious win, twice over. Because 24 is not a power of two, every
> gadget's probe has to *multiply* the hash index to reach its entry, and that
> multiply sits in the dependency chain `addr → index → entry → compare` in
> front of every guest memory access; padding turns it into a shifted add. And
> at 24 bytes the entries cycle through byte offsets 0, 24, 48, 8, 32, 56, 16,
> 40 within a 64-byte cache line, so two of every eight span two lines and 25%
> of probes touch two lines to read `page` and `data_minus_addr`.
>
> Both facts are true. Neither is worth anything. The change was implemented in
> full — `aligned(32)`, a `TLB_ENTRY_SHIFT` emitted from `jit/offsets.c`, all
> six gadget sites converted, all four guests green — and measured on two
> microarchitectures and two workload shapes:
>
> ```
> md5sum of 24 MB, interleaved A/B, median of 8 (Apple silicon)
>   riscv64 guest  1699 → 1688 ms   (+0.6%)
>   i386 guest      550 →  553 ms   (−0.5%)
>   arm64 guest     427 →  431 ms   (−0.9%)
> dash sh-loop 20k, min of 5–8 (A9 / ARMv8.0, iPad 5)
>   arm64 guest    3042 → 3035 ms   (flat)
>   riscv64 guest  3588 → 3596 ms   (flat)
> ```
>
> Flat everywhere, arguably a hair worse, for 8 KB more stack per thread. The
> multiply is free because even the A9 is a three-wide out-of-order core that
> overlaps it with surrounding gadget work, and a straddled entry costs nothing
> while the whole 24 KB array is cache resident. Reverted, with the numbers
> written into [emu/tlb.h](../../emu/tlb.h) so the next person to have the idea
> gets the answer before spending the day.

## 5.5 Lazy anonymous mmap: reservations, not page tables

`mmap(NULL, 64ULL << 30, PROT_NONE, MAP_ANONYMOUS|MAP_NORESERVE, ...)` is free
on Linux. It is a promise about address space, and the kernel writes down a VMA
and nothing else.

AOK's original implementation of that call built one `struct pt_entry` per
page, eagerly, at mmap time. That is roughly 65 bytes of host memory per 4 KB of
guest address space — measured, host RSS while merely *holding* an untouched
reservation grows dead linear at about 16.6 MB per GiB, so 1.06 GB for a 64 GiB
reservation. A single guest `mmap` could get the entire application
jetsam-killed without the guest ever touching a byte. Language runtimes reserve
address space like this as a matter of course.

The fix is a second representation that costs nothing: a *reservation*, a plain
address range recorded in a fixed array on `struct mem`.

```c
struct mem_lazy_map { page_t start, end; unsigned flags; };
#define MEM_LAZY_MAX 32
```

The design's whole character comes from one invariant, stated in capitals in
the header: **a reservation is never split.** Materializing a fault takes the
entire prefix up to the end of the faulting chunk and trims the front, so a
reservation only ever shrinks from the left or vanishes. Anything that would
punch a hole in one — an `munmap` in the middle, an `mprotect` of a subrange —
materializes it in full first, at a call site where mapping is safe.

That invariant is doing a lot of work. Without it you need slot allocation when
a split runs the array out of entries, atomicity for a partial update, and a
lock-ordering story for the recursion a split provokes. With it, the failure
modes disappear: the worst case is a fault at the far end of a reservation
materializing everything before it, which is precisely the old eager behavior
and no worse.

Two constants set the boundaries, both from measurement rather than taste.
`MEM_LAZY_MIN_PAGES` is 64 MB: below that the eager path is kept, because
ordinary programs touch 36–72% of what they map — bash 36%, python3 52%, gcc
72%, measured — over mappings of a few megabytes, where per-page faulting would
cost more than the batch loop and the eager cost is trivial anyway.
`MEM_LAZY_CHUNK_PAGES` is 512, so a sequential walk through a reservation pays
one fault per 2 MB rather than one per page.

The same idea appears once more in a simpler form: `brk_reserve_start` and
`brk_reserve_end` set aside headroom for `brk` growth on the dynamic-PIE guests
as a plain range, so reserving it is O(1) and `fork` has nothing extra to walk.
`pt_is_hole` and `pt_find_hole` treat it as occupied; `sys_brk_guest` claims
prefixes of it by mapping real pages and advancing the start.

The rule that falls out, and that the header states as an invariant: **a page is
either mapped or reserved, never both, and a reservation is never split.**

## 5.6 Sharing, copying, and three ways a shared mapping stopped being shared

Copy-on-write is `P_COW`, and the write fault that clears it is the only place
a private page's `data` is replaced. `P_SHARED` is the flag that says this must
never happen. The interesting failures are all cases where one of those two
statements was locally true and globally wrong.

A single commit in August 2026 fixed three of them, and they make a good set
because each fails silently in a different way.

**A ptrace poke made the tracee's shared page private.** `PTRACE_POKEDATA` set
`P_COW` alongside `P_WRITE` without testing `P_SHARED`, so the page was copied
and the tracee was switched to a private duplicate. The poke never reached the
file. Worse, every store the *tracee itself* made afterwards was lost too, and
nothing returned an error anywhere. `P_COW` now means what the write fault
always meant by it: this page is private.

**The first `mprotect` ended the sharing.** A `PROT_NONE` `MAP_SHARED`
anonymous region is reserved with no host backing and one descriptor that every
mapper's page-table entry points at. The first process to `mprotect` it
accessible called `pt_map`, which built a *new* descriptor — so the sharing
ended at the exact moment the mapping first became usable. Two processes that
mprotected such a region after forking never saw each other's writes again,
while doing the mprotect *before* the fork worked fine. That asymmetry is why
this looked like a fork bug for as long as it did. The backing is now filled
into the shared descriptor under its own lock, because the struct is reachable
from several processes' page tables while each of them holds only its own memory
lock.

**`memfd` sealing lied.** `F_SEAL_WRITE` was granted while a shared mapping was
live, which is worse than not implementing sealing at all: `F_GET_SEALS`
reported a guarantee nothing was keeping, stores through the live mapping still
landed, and a receiver who mapped the "sealed" fd afterwards saw the mutations.
This is the failure mode Chapter 40 calls a capability lie — a state a real
system never produces, so nothing downstream is written to survive it. Live
shared mappings are now counted per memfd and the seal returns `EBUSY` while any
exists.

The last one carries a second lesson about how the criterion was chosen. The
seal is blocked by *any* shared mapping, not by any *writable* one, because
that is what Linux does when you actually test it: a read-only shared mapping
blocks the seal, since it can be mprotected writable later, and a writable
private mapping does not. That is the opposite of what the names suggest, which
is exactly why the test checks the whole matrix rather than the case somebody
reasoned their way to.

The test is `tests/manual/mmap_shared_integrity.c`: 25 checks, failing in seven
places on the parent commit, passing here, and — the part that makes it evidence
rather than opinion — passing when compiled and run on Linux.

## 5.7 Locking a page table that four threads are walking

`struct mem` carries two locks that do different jobs.

`lock` is a reader-writer lock over the page tables. A writer that changes an
existing mapping must also evict readers: any thread with a stale TLB entry has
to be poked, which means taking the lock in write mode and sending each sibling
a signal. `pt_alloc_lock` is the cheaper path — a plain mutex serializing
structural mutations — and the growth-mmap fast path holds only that one,
because adding never-before-mapped pages cannot produce a stale TLB entry and
entry publication is atomic.

Then there is quiescence, which is where an apparently polite implementation
turned into a denial of service.

Readers waiting for a barrier writer used to `sched_yield()` for 256 spins and
then poll with `nanosleep`. Under a storm of barriers — the thread benchmark
issues one `mprotect` per `pthread_create`, with dozens of siblings alive —
every parked sibling sat in a `sched_yield` hot loop, monopolizing the host
scheduler so thoroughly that freshly created threads never got scheduled to run
and exit. The pileup fed itself, and the application wedged with its main thread
starved in the middle of an `open()`.

Now a waiter yields 32 times, on the theory that the common case is a small
page-table edit that clears in microseconds and handing the CPU straight to the
writer is the fastest thing available, and then it *parks* on a condition
variable until the writer's release broadcast. Parked threads cost nothing,
however many pile up.

The general shape recurs in Chapter 36: a spin loop is a bet that the wait is
short, and when the bet is wrong the failure is not slowness, it is starvation
somewhere else entirely.

## 5.8 Floating point, and the reference that does not exist

x87 arithmetic is 80-bit, and no modern host has to provide that format. AOK
implements it in software:

```c
typedef struct {
    uint64_t signif;
    union { uint16_t signExp; struct { unsigned exp:15; unsigned sign:1; }; };
} float80;
```

with the full set of operations — add, subtract, multiply, divide, compare,
round, the transcendentals, and the classification predicates (`f80_isnan`,
`f80_isdenormal`, `f80_is_supported`) that x87 semantics depend on.

The awkward part is testing it. `float80-test.c` compares the implementation
against the host's own `long double`, which is the 80-bit format on x86_64 and
is not on Apple silicon. On an ARM host there is no reference to compare
against, so the test skips — and a skipped test is exactly as informative as it
sounds. This is one of the few places in the tree where the primary development
machine cannot validate the code, and the answer is Chapter 35's Linux CI,
which is also an x86_64 host and runs it in full.

## 5.9 Vectors

`union mm_reg mm[8]` and `union xmm_reg xmm[16]` cover MMX and SSE. The arm64
guest's `V0`–`V31` reuse `union xmm_reg` directly rather than duplicating an
identical 128-bit union under a new name.

AVX is the more interesting piece, because of where it lives. The semantics of
every VEX and EVEX instruction are implemented in `emu/avx.c` over *flat byte
buffers*: the functions know nothing about CPU state, the TLB, modrm, or which
guest is executing. That leaves exactly one implementation of each
instruction's meaning, shared by two front-ends that decode very differently —
the amd64 interpreter, which decodes and executes directly, and i386, which is
JIT-only, decodes at translation time in `jit/gen.c`, and reaches the same
functions through vector-helper gadgets.

The lane rule is the part worth remembering when reading that code: helpers
that are lane-local (shuffles, packs, unpacks) iterate 128-bit lanes
internally, and the ones that deliberately cross lanes — the permutes — say so
at their definition. Getting that boundary wrong produces results that are
correct for 128-bit vectors and wrong for 256, which is the single most common
way an AVX implementation is subtly broken.

## 5.10 What the guest machine is

Add it up and the emulated machine is: a struct with four register files, a
lazily-evaluated flags word, a page table whose entries carry both permissions
and the compiled code that depends on them, a per-thread 24 KB software TLB, two
representations of address space so that the empty one is free, and a software
implementation of a floating-point format nobody makes hardware for any more.

Nothing in that list is exotic. What is unusual is how much of the design is
about *not* materializing things: not computing flags, not building page tables
for untouched reservations, not padding a struct that profiling says gains
nothing, not splitting a range because splitting is where the bugs live. The
guest machine is defined as much by what it declines to allocate as by what it
holds.

The next chapter is about the other half of the engine — what actually executes
against this state, and why a JIT that is forbidden from writing instructions
turns out to be fast enough to run Alpine.

---

*Anchors:* [emu/cpu.h](../../emu/cpu.h), [emu/memory.h](../../emu/memory.h),
[emu/memory.c](../../emu/memory.c), [emu/mmu.h](../../emu/mmu.h),
[emu/tlb.h](../../emu/tlb.h), [emu/tlb.c](../../emu/tlb.c),
[emu/float80.h](../../emu/float80.h), [emu/avx.h](../../emu/avx.h),
[kernel/task.h](../../kernel/task.h), [kernel/task.c:796](../../kernel/task.c#L796),
`tests/manual/mmap_shared_integrity.c`.

*Story:* three ways a `MAP_SHARED` mapping stopped being shared — a ptrace poke
that privatized the tracee's page, an `mprotect` that ended the sharing at the
moment the mapping became usable, and a `memfd` seal that reported a guarantee
nothing was keeping.
