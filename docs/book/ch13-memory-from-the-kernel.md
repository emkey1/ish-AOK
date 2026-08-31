# 13. Memory management from the kernel side

Chapter 5 described the machine's view of memory: page tables, a software TLB,
reservations that cost nothing until touched, and the flags that make a page
shared or private. This chapter is the other side of the same subject — the
contract userspace sees. `mmap`, `munmap`, `mprotect`, `mremap`, `brk`,
`madvise`, `memfd_create`, `msync`, `mlock`, `membarrier`: the calls a guest
allocator makes thousands of times a second, and whose exact behaviour at the
edges every language runtime quietly depends on.

The unusual thing about memory management here is who it is protecting. In a
normal kernel, the memory manager defends processes from each other and the
kernel from processes. Here there is one host process containing everything, and
a large part of what these calls do is defend *the application* from the guest
running inside it.

## 13.1 Validation is most of the code

Read `mmap_common_guest` and the striking thing is how much of it runs before
anything is mapped:

```c
if (len == 0)                                   return _EINVAL;
if (prot & ~P_RWX)                              return _EINVAL;
if ((flags & MMAP_PRIVATE) && (flags & MMAP_SHARED))  return _EINVAL;
if (!(flags & (MMAP_PRIVATE | MMAP_SHARED)))    return _EINVAL;
```

Then the address rules, which are asymmetric in a way worth knowing: a plain
address *hint* is rounded down to a page boundary rather than rejected, while
`MAP_FIXED` and `MAP_FIXED_NOREPLACE` demand an exactly page-aligned address.
And `MAP_FIXED_NOREPLACE` must **fail** rather than clobber or relocate, which
is the entire point of it — a caller using it is asking "is this range free?",
and an implementation that helpfully relocates has answered a different
question.

Then the file-backed path: the descriptor must exist (`EBADF`), and it must
support mapping at all (`ENODEV`).

`mremap` is the same pattern:

```c
if (flags & ~(MREMAP_MAYMOVE_ | MREMAP_FIXED_))       return _EINVAL;
if ((flags & MREMAP_FIXED_) && !(flags & MREMAP_MAYMOVE_))  return _EINVAL;
if ((flags & MREMAP_FIXED_) && PGOFFSET(new_addr) != 0)     return _EINVAL;
```

It is tempting to treat this as boilerplate. It is not, and the reason is that
allocators probe. glibc's and musl's allocators, and every language runtime with
its own heap, use these calls' error behaviour as a *feature test*: they attempt
something, look at the errno, and conclude what the kernel supports. A wrong
`EINVAL` does not produce a crash — it produces a runtime that has concluded
something false about its address space and takes a different path forever
after. Chapter 40 has a name for the general case; this is its most common
instance.

## 13.2 Refusing memory that exists

The most iOS-specific thing in this file is a check at the top of both `mmap`
and `brk`:

```c
if (host_mem_headroom_low()) {
    ...
    return _ENOMEM;
}
```

`host_mem_headroom_low()` consults `os_proc_available_memory()` — the jetsam
budget — and returns true when the application is within about 192 MB of the
ceiling at which iOS kills it. At that point the guest's `mmap` starts failing
even though the device has memory, because the alternative is worse: UIKit,
libobjc and the fakefs SQLite layer keep allocating regardless of what the guest
does, and an allocation failure in *those* is a `NULL` dereference and a
terminated app.

A guest that gets `ENOMEM` from `mmap` is a guest whose allocator raises an
exception, or whose thread creation fails, or which prints "out of memory". All
of those are recoverable, and all of them are better than the whole terminal
disappearing. The threshold is tunable with `ISH_GUEST_MEM_HEADROOM_MB`, and
setting it to 0 disables the guard.

> **The bug that taught us this**
>
> The guard was originally silent, and silence was the problem:
>
> > This guard fires silently otherwise — from the guest's point of view every
> > `mmap()` in the whole app just starts failing with a clean `ENOMEM`, with
> > nothing to explain why (e.g. a Wayland compositor's *next* window failing to
> > open, with no obvious cause, because its `wl_shm` buffer's mmap got refused
> > here).
>
> A correct refusal is still a refusal that has to be explainable. It now logs —
> rate-limited to eight lines, because a runaway guest hammering `mmap` under
> low headroom would otherwise flood the log and push out whatever came before
> it.
>
> The pattern generalizes: any policy that changes a system's behaviour based on
> state the user cannot see needs to announce itself once, and only once.

## 13.3 The growth fast path

Most `mmap` calls in a running system are boring: anonymous, not fixed, growing
the address space. That case has a property worth exploiting, and the reasoning
is a small gem of concurrency argument:

> A pure-growth mmap only adds never-before-mapped pages. Anonymous (no fd
> backing to wire up) and not `MAP_FIXED*` means `do_mmap` always lands on a hole
> — a non-fixed hint that collides is relocated to a fresh hole, so the whole
> target range is unmapped and `do_mmap` unmaps nothing. Adding hole pages needs
> no reader eviction: those pages were never mapped, so no sibling holds a TLB
> entry for them, and the page-table chunk/leaf publication is already atomic
> (acquire/release). Only writer-vs-writer exclusion is required.

The general case of a page-table mutation requires taking the memory write lock
*and* poking every sibling thread to drop its stale TLB entries (Chapter 5).
That is expensive and it is what made the `mprotect`-per-`pthread_create`
benchmark pathological. The pure-growth case skips all of it and holds only
`pt_alloc_lock`, because the pages being added cannot be in anybody's TLB — no
sibling ever had a translation for a page that was never mapped.

The escape hatch is `ISH_NO_MMAP_GROWTH_FAST`, and its presence is deliberate:
a fast path with a subtle correctness argument should be switchable at run time,
so that "is it this?" can be answered in one A/B rather than by reasoning.

## 13.4 `brk`, and a one-page off-by-one everybody meets

`brk` is the older, simpler heap interface, and its specification contains a
trap that every implementation rediscovers. The comment in `sys_brk_guest`
states it exactly:

> round up because of the definition of brk: "the first location after the end
> of the uninitialized data segment" (brk(2)). If the brk is 0x2000, page 0x2000
> shouldn't be mapped, but it should be if the brk is 0x2001.

The break is an *exclusive* bound. Off by one byte, and you map an extra page —
which is invisible until something depends on the page after the heap being
absent, at which point it is a very confusing bug.

`brk` also shares the jetsam backpressure of Section 13.2, and interacts with
the reservation mechanism of Chapter 5: the dynamic-PIE guests get a
`brk_reserve` range set aside by `exec`, tracked as a plain range rather than as
page-table entries, which `sys_brk_guest` claims prefixes of as the heap grows.
`pt_is_hole` and `pt_find_hole` treat that reservation as occupied, so nothing
else takes the space, and `fork` has nothing extra to copy.

## 13.5 `mremap`, and what has to survive a move

Moving a mapping is the most structurally invasive thing in this file: page
table entries move, the JIT blocks compiled from those pages must be
invalidated (Chapter 6), and every sibling's TLB must be flushed.

The subtlety is what the moved pages must keep. A `MAP_SHARED` mapping's
contents live in a `struct data` descriptor that other processes' page tables
also point at, and a `MAP_PRIVATE` page that has already been written has its
own copied data. So `mremap` moves the page-table entries and their descriptors
rather than rebuilding them:

> preserves both `MAP_SHARED` contents and any `MAP_PRIVATE`/COW data already
> written. Mirrors the fd path in `do_mmap()`.

`MREMAP_FIXED` additionally clears whatever is currently mapped at the
destination, which is what makes it dangerous and why it requires
`MREMAP_MAYMOVE` to be set alongside it — the API forces the caller to
acknowledge that a move can happen before letting them name where.

## 13.6 `madvise`: advice, and the parts that are not advice

Most of `madvise` is genuinely advisory and can be honestly implemented as a
no-op: `MADV_WILLNEED`, `MADV_SEQUENTIAL`, `MADV_RANDOM` describe intentions
that a system with no page cache of its own has no way to act on.

Two are not advice at all, and both had to be implemented:

- **`MADV_WIPEONFORK`** and its counterpart `MADV_KEEPONFORK` set and clear the
  `P_WIPEONFORK` flag on the page-table entries. A child of `fork` gets fresh
  zero pages in that range instead of inheriting the parent's data. Runtimes use
  it for per-thread state that must not be duplicated — a random-number pool, a
  cached pid — where inheriting is worse than losing.
- **`MADV_REMOVE`** punches a hole: the pages are dropped and subsequent reads
  see zeroes. That is a state change, not a hint.

The distinction to hold on to is the same as everywhere else in this book: a
call whose effect is observable must be implemented, and a call whose effect is
not may be ignored — but the *return value* has to be right either way, because
a caller that gets `EINVAL` from an advisory call concludes the feature is
missing.

## 13.7 `memfd` and sealing

Chapter 5 told the sealing story from the page-table side. From the syscall
side, `memfd_create` produces an anonymous file that can be mapped and passed
over a Unix socket, which is what `wl_shm` and every shared-memory IPC scheme in
modern Linux userspace is built on — one of the pieces that made the Wayland
work of Chapter 42 possible at all.

`F_ADD_SEALS` with `F_SEAL_WRITE` is the part with teeth, and the rule AOK
arrived at is worth restating because it is counterintuitive and was measured
rather than reasoned: the seal is refused with `EBUSY` while **any** shared
mapping of the memfd is live — not merely any *writable* one. A read-only shared
mapping blocks it, because it can be `mprotect`ed writable later; a writable
private mapping does not, because writes to it never reach the file. That is the
opposite of what the names suggest, which is why the conformance test checks the
whole matrix instead of the cases somebody expected to matter.

## 13.8 The gap: `PROT_EXEC` is never enforced

This is the largest known hole in AOK's memory model, and the way it is recorded
in the tree is a model for how to document one.

`emu/memory.h` says "P_READ and P_EXEC are ignored for now", and `P_EXEC` really
is ignored: it is stored, printed in `/proc/<pid>/maps`, reconstructed by
`mremap`, and never once consulted. Measured against Linux 6.12, with an arm64
`mov w0,#42; ret` written into a `PROT_READ|PROT_WRITE` page:

| | Linux | AOK |
|---|---|---|
| call into a never-`PROT_EXEC` page | SIGSEGV | returns 42 |
| `mprotect(PROT_READ)` over a `PROT_EXEC` page, then call | SIGSEGV | returns 42 |

So every guest `.data` and `.bss` page is executable, and any guest JIT's own
W^X discipline is decorative.

**The grading.** It is a mitigation gap rather than a hole: exploiting it
requires a separate memory-corruption bug in guest software. Nothing about AOK
becomes reachable that was not already reachable; what is lost is a layer that
would have made a guest-side bug harder to turn into execution.

**Why it is not fixed, stated as design rather than as an apology.** The
instruction-fetch path has no access type of its own — `emu/tlb.h` fills the TLB
for a fetch with `MEM_READ` — so there is nothing for a permission check to hang
off. Two designs were considered:

*A TLB bit.* Add a `page_if_executable` tag beside `page` and
`page_if_writable`, so a fetch checks a third tag exactly as a write checks the
second. This is the obvious shape, and it grows the emulator's hottest data
structure by half. Chapter 5 has the measurement that makes this a hard no: the
same structure was already grown once, from 24 bytes to 32, in a change that was
implemented in full, benchmarked on two microarchitectures, and reverted for
buying nothing. Rejected on cost.

*Check once per compiled block, invalidate on revoke.* This is the right shape
and is nearly free. `jit_block_compile_common` runs once per block, so the check
lands exactly where Linux's fault-on-fetch would; and `jit_invalidate_page` —
which already exists for self-modifying code — handles the revoke half when
`pt_set_flags` clears `P_EXEC`.

The obstacle is fault *delivery*. `jit_block_compile` returning NULL already
means out of memory, and every dispatch loop responds to that by flushing the
entire JIT, retrying, and then killing the task with a "JIT OOM" message
(Chapter 6). A non-executable page needs a **distinct** signal threaded out, so
the loop raises `INT_PF` with the faulting address instead — and there are four
dispatch loops, one per guest, each with its own OOM ladder and crash-unwind
structure, plus the interpreter build's own path.

The verdict in `docs/TODO.md`:

> That is a contained project rather than a patch, and it touches the one path
> where a mistake stops every guest from running. Worth doing deliberately, with
> its own before/after benchmark run, rather than folded into a conformance
> sweep.

Three things make that entry worth imitating: the gap is *measured* against a
real Linux rather than assumed; the severity is *graded* rather than asserted;
and the two candidate designs are written down with the specific reason each was
or was not taken, so the next person starts from the second design rather than
rediscovering the first.

## 13.9 What this layer is actually defending

Put the chapter together and the priorities are visible in the order the checks
run.

The guest is not being protected from itself. `PROT_EXEC` is unenforced, there
are no memory namespaces, and a guest process that corrupts its own heap is on
its own — the same as on Linux, minus one mitigation.

What is being protected is the *application*. Reservations are bounded so a
64 GiB `mmap` cannot consume a gigabyte of host RSS. Growth is refused before
the jetsam budget is exhausted so UIKit keeps working. Page-table mutations take
a barrier that parks siblings rather than spinning them, so a `mprotect` storm
cannot starve the main thread. Every one of those exists because the failure it
prevents is not "the guest program crashes" but "the terminal disappears".

That is the inversion at the centre of this whole project, and memory management
is where it is most visible: the kernel's most important client is the process
it is running inside.

---

*Anchors:* [kernel/mmap.c](../../kernel/mmap.c), [kernel/memfd.c](../../kernel/memfd.c),
[emu/memory.c](../../emu/memory.c), [emu/memory.h](../../emu/memory.h),
[emu/tlb.h](../../emu/tlb.h), [platform/platform.h](../../platform/platform.h)
(`host_mem_headroom_low`), [docs/TODO.md](../../docs/TODO.md) ("PROT_EXEC is
never enforced"), `tests/manual/mmap_shared_integrity.c`.

*Story:* the silent headroom guard — every `mmap` in the application beginning
to fail with a clean `ENOMEM` and nothing anywhere saying why, diagnosed from a
Wayland compositor's next window simply not opening.
