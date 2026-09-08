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

That defence has three settings, and they are the shape of the first half of
this chapter: **refuse** the memory (13.2), **slow down** the thread taking it
(13.3), or **find some** by paging cold pages out to a swap area (13.4–13.6).
The last of those is the newest and largest piece of the subsystem, and it
exists because the first two both end in the guest not getting what it asked
for.

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

The most iOS-specific thing in this file is a check at the top of `mmap`,
`mremap` and `brk`:

```c
if (mem_growth_refused(len))
    return _ENOMEM;
```

Underneath it is `host_mem_headroom_low()`, and what that function consults has
grown three times, each time because the previous version was wrong on a real
device rather than in theory.

**It started as the app's own ceiling.** `os_proc_available_memory()` reports
what is left of the jetsam budget, and the guard refused growth within about
192 MB of it. The reasoning holds: UIKit, libobjc and the fakefs SQLite layer
keep allocating regardless of what the guest does, and an allocation failure in
*those* is a `NULL` dereference and a terminated app. A guest that gets `ENOMEM`
raises an exception or prints "out of memory"; all of that is recoverable, and
all of it beats the terminal disappearing.

**Then the machine's.** On a 3 GB iPhone SE the guard computed ~1.6 GB of its
own headroom and cheerfully allowed more while the *device* had 310 MB free. The
per-process limit on that phone is roughly the size of RAM, so obeying only it
means AOK is permitted to consume the entire machine — and the JetsamEvent shows
exactly that: largestProcess at 1.86 GB, 40 MB free device-wide, Apple's own
daemons dying around it. The host refused before we did, which is the wrong way
round. The machine's free memory now counts too.

**And only when the system agrees.** That test alone made the guest unusable.
iOS keeps free memory low *by design* — `available` counts inactive and
purgeable pages it reclaims without asking — so a small phone sits below any
useful floor for long stretches with nothing wrong, and the guest could not even
`exec` `sleep`. The machine's figure is therefore read only while iOS is itself
reporting pressure. The conjunction is a correction, not caution.

Two further corrections came out of the 554 cycle, and both are instructive
about what a guard is *for*.

> **The bug that taught us this**
>
> A device ran `stress --vm-bytes 5.5G`, and the log said:
>
> ```
> MEMORY PRESSURE CRITICAL (was normal): footprint 4915 MB, own headroom 1228 MB
> WARNING: 744(stress) mmap refused, low iOS memory headroom (len=0x140001000)
> WARNING: 745(zsh) mmap refused, low iOS memory headroom (len=0x100000)
> ```
>
> The first refusal is the guard working: 5 GB, correctly denied. The second is a
> shell being denied **one megabyte**, nine seconds later, with `stress` already
> dead and 1228 MB of the app's own headroom free. Every command after it failed;
> the guest was bricked until the app restarted.
>
> Two defects, and neither is the guard's *policy* being wrong.
>
> **The guard never saw the request size.** `host_mem_headroom_low()` answers "is
> the app near its ceiling", which is a property of the app; whether to refuse is
> a property of the request too, and every caller threw that away. So a 5 GB
> mapping and a 1 MiB one got the same verdict. Refusing the small one buys
> almost nothing — anonymous growth here is a lazy *reservation* that costs the
> app nothing until the pages are touched, and touching them is policed
> separately and better by Section 13.3 — while a guest that cannot obtain a
> megabyte cannot start a process, cannot `exec`, and cannot run the command that
> would fix it. `mem_growth_refused()` now exempts anything below a threshold
> scaled off the same floor it defends.
>
> **CRITICAL was a latch, not a measurement.** dispatch delivers memory pressure
> on a level *change*, so one CRITICAL notification stands until the system sends
> something else — and no "normal" ever followed. The notification now has to
> agree with a reading taken *now*, weighed against a larger floor so that
> CRITICAL still bites harder than warn. The iPhone SE case still refuses: there
> the machine had 40 MB free, which is under any floor.
>
> The generalisable form: **a notification used as a gate needs the correctness
> of a gate.** A latched value that is merely *reported* is a diagnostic; the
> moment something refuses work on it, staleness becomes a bug.

The guard is still silent-by-default in one respect and deliberately not in
another. It logs, rate-limited to eight lines, because a runaway guest hammering
`mmap` would otherwise flood the log and push out whatever came before it — the
original version logged nothing at all, and a Wayland compositor's *next* window
simply failing to open, with nothing anywhere saying why, is what taught that
lesson. Any policy that changes behaviour based on state the user cannot see
needs to announce itself once, and only once.

`ISH_GUEST_MEM_HEADROOM_MB` sets the floor and `0` disables the guard entirely;
`ISH_GUEST_MEM_BUDGET_MB` invents a ceiling on a host that has none, which is
the only way to reach any of this code off a device.

## 13.3 When refusing is not enough: the fault-path throttle

The guard of Section 13.2 sees `mmap`. It does not see the memory actually being
*used*, because an anonymous mapping here is a reservation that costs nothing
until a page is touched (Chapter 5). A guest can therefore reserve politely,
pass every check, and then commit gigabytes one fault at a time without the
growth guards being consulted once.

`mem_fault_backpressure()` ([kernel/mmap.c](../../kernel/mmap.c)) is the answer,
and almost every line of it is a correction to a plausible simpler design.

**Is this address space actually growing?** The headroom reading is app-wide and
the sensor over-counts badly: the TLB is direct-mapped, so conflict misses and
post-flush re-misses look exactly like fresh commits. Without a growth test, a
process merely re-walking a working set it already owns accrues strikes and is
eventually killed for committing nothing. Residency is the honest question, and
only a space whose page count is rising is the one making things worse.

**Pay for the memory on the thread committing it.** A thread made to page
memory out before it may commit more is slowed by exactly the cost of the I/O it
is causing — which is what Linux does to a process in direct reclaim.

**But not more often than every 250 ms**, because reclaim takes the
address-space barrier and every sibling guest thread stops while it runs.

> **The bug that taught us this**
>
> On a Mac this is invisible. On a 3 GB iPhone SE it was tens of milliseconds of
> flash writes per 4 MiB committed, back to back, with every other guest thread
> parked — and the app stopped responding and iOS terminated it as *hung*. The
> clue was that this is not what a jetsam kill looks like: the window was still
> on screen. 250 ms against a ~40 ms barrier is a 16% duty cycle, so the guest
> runs 5/6 of the time and reclaim still proceeds an order of magnitude faster
> than a device guest commits.

**Brake unconditionally while headroom is low**, whether or not reclaim just
freed something. Making the brake conditional on reclaim *failing* was wrong in
a way only a slow device would have shown: the throttle's strength then came
entirely from how long the eviction I/O took, so on a Mac — where the swap file
is page cache and 2 MiB costs microseconds — a guest committing 1536 MiB was not
slowed at all, at a measured 297 MiB/s, while the same code on a phone braked
hard. **A backpressure mechanism whose strength is set by the host's I/O speed
is not a mechanism, it is an accident.**

**And finally, a kill.** Eight consecutive strikes — the space still growing,
the host with nothing left, the pager able to free none of it, braking having
failed to stop it — and the faulting process is sent `SIGKILL`. Linux answers
this with the OOM killer and so does AOK; one guest process dies instead of iOS
killing the app, which takes every other guest process and the user's session
with it. The *faulting* process, not a badness heuristic: it is the one asking
for memory that is not there, its death certainly helps, and picking another
would mean walking the task table under `pids_lock` at the worst possible
moment.

Only a reclaim that ran and freed nothing accrues a strike. A reclaim that
freed something means the pager is coping, and a probe that skipped reclaim
because it was too soon for the barrier proves nothing either way.

## 13.4 Swap: somewhere to put the cold pages

Refusing and throttling both end in the same place — the guest cannot have the
memory. Swap is the alternative: give the cold parts of a large, mostly idle
working set somewhere to live that is not RAM, and the working set can exist.

The area is a **file**, created at boot and immediately unlinked, so it can never
appear in the app's container and can never be swept into a backup. Its size is
the user's decision, taken in Settings; it is **off by default**, because paging
spends the write endurance of somebody's phone and nothing should start doing
that on their behalf. The command-line build has `ISH_GUEST_SWAP_MB` instead.

It is divided into fixed **slots** of 16 KiB — four guest pages — tracked by a
free bitmap and an allocation rover. Sixteen kilobytes rather than four because
the unit of eviction is a *frame*: paging out one guest page at a time would
multiply the bookkeeping by four and the I/O by rather more.

The guest sees it the way it sees anything else, which was a deliberate goal
rather than a nicety: `SwapTotal`/`SwapFree` in `/proc/meminfo` and `free`, the
`si`/`so` columns in `vmstat`, a real row in `/proc/swaps` naming a real block
device at `/dev/aokswap0`, and working `swapon(2)`/`swapoff(2)`. A guest that
cannot see its own swap would be a machine no Linux produces.

> **The bug that taught us this**
>
> Every one of those surfaces was checked against *a reading of* what Linux
> does. Two were wrong in exactly that way and were found only when someone ran
> the real tools beside them: `mlock` over `RLIMIT_MEMLOCK` returned `EPERM`
> where Linux has returned `ENOMEM` since 2.6.9, and a failed swap-in delivered
> `SIGSEGV` where Linux delivers `SIGBUS`. Both were one line.
>
> A third survived even that, until the 554 documentation audit: `swapon
> /dev/aokswap0` failed with `read swap header failed`. The header was perfect
> and the syscalls worked — but the *device* implemented no ioctls at all, and
> the tool asks it for its size before it will touch it. Read, write, lseek,
> a valid `SWAPSPACE2` signature and a `/proc/swaps` row, and it was still a
> block device the way a photograph of a door is a door. **Running the command a
> user would type is a different test from reading the code that serves it.**

## 13.5 Choosing what to evict, and getting it back

Eviction runs over one address space at a time and only over frames that space
**exclusively owns**. A frame reachable from two page tables — a forked family's
shared image — is left alone, because releasing it under one owner while the
other still holds entries into it is the whole class of bug this design is built
to avoid. That is also the largest thing swap does not yet do (Section 13.6).

Which frames are cold is decided by a **second-chance clock**: a sweep ages
entries, gives a recently-accessed one a second pass, and takes the ones that
survive two.

> **The bug that taught us this**
>
> The `accessed` byte is only written on a **TLB fill**. A page already in the
> TLB is touched by the guest without the page-table entry learning anything, so
> a sweep that merely reads the byte is blind to the hottest pages in the
> process. Measured: 506 of 1536 avoidable faults came from exactly that. The
> sweep now bumps `mem_changed` every pass, forcing the TLB to refill and the
> byte to mean something.

Evicting a frame is an ordering problem. The slot must be readable before any
entry says "go and read it", so the frame's slot is published first with release
ordering and the entries second. Then the host is given the page back, with two
calls in a fixed order: `madvise(MADV_FREE_REUSABLE)` *then*
`mprotect(PROT_NONE)`. The first is what moves the jetsam ledger; the second is
what turns a missed pointer into a `SIGBUS` at the guilty instruction instead of
silent corruption. `mprotect` first would be `EPERM`. `ISH_SWAP_NO_MADVISE` and
`ISH_SWAP_NO_MPROTECT` exist so that which of the two actually releases anything
can be answered by measurement rather than by argument.

Bringing a frame back is the asymmetric half, because a `fork` after an eviction
can create a second party where eviction had none. The rule that resolves it is
the design's largest correction, and it is worth stating as a slogan: **the slot
belongs to the frame, not to the entry.** A faulting entry only records that its
page is *not here*, which may already be stale — a sibling's fault, or the same
frame's fault in a forked sibling, can have brought it back. So swap-in asks the
frame where its bytes are, under a per-frame lock, and if the answer is "nowhere,
it is already home" it reads nothing at all. Re-reading there would put the
on-disk copy back over whatever the guest has written since.

> **The bug that taught us this**
>
> A frame's first guest page is found from its **data offset**, not from guest
> adjacency — "membership is by data offset, not guest adjacency". The prototype
> used `page - (page % pages_per_frame)`, which is the same thing only when a
> mapping's first guest page happens to be a multiple of four. The arm64 test
> root put the verifier's region at guest page `0x7fffbcf1c`, a multiple of 4, and
> it passed. The amd64 root put the identical region at `0x7ffffcf59`, which is
> 1 mod 4, so every base came out one page low: swap-in marked the last page of
> the *previous* frame resident while that frame was still released, and the next
> read of it took a host fault on a `PROT_NONE` page. Deterministic,
> single-threaded, and it killed the verifier with `SIGBUS` after three faults.

## 13.6 What swap costs, and what it refuses to spend

A pager on somebody's phone has obligations a pager on a server does not, and
most of the interesting logic in `kernel/swap.c` is about declining to work.

- **A write budget.** Eviction stops once a fixed number of bytes have been
  written inside a rolling 24-hour window. A runaway guest cannot quietly grind
  through the device's write endurance; `/proc/ish/swap` reports the window and
  what is left of it.
- **A thrash guard.** If pages come straight back after being evicted, paging is
  not helping and background reclaim pauses. The pause must **expire** rather
  than wait to be cleared by evidence: while it holds, nothing is evicted, so
  there is no evidence to be had, and it latched for the life of the process
  until a cooldown was added.
- **A suspension gate.** No new eviction I/O may start while iOS is suspending
  the app — a write in flight across suspension is the `0xdead10cc` class.
- **A release check.** If several sweeps free bytes without moving
  `phys_footprint`, the pager stops: a release that does not move the ledger
  jetsam kills on is not a release, it is I/O for nothing. `/proc/ish/swap`
  reports it as `release_works`.
- **Direct reclaim.** An allocation about to be refused first asks the pager to
  make room, so a large mostly-idle heap can exist at all. Without it the guard
  is the only answer and the answer is always no.
- **`kswapd`.** A background thread sheds cold pages *before* an allocation
  fails, because a caller that gives up on one `ENOMEM` never gets a second.

And two promises the pager owes the guest: memory the guest has `mlock`ed is
never evicted, and a slot that cannot be read back delivers `SIGBUS` with
`BUS_ADRERR` — the address is valid, the hardware could not deliver it — rather
than `SIGSEGV`.

**What it does not do yet.** Frames shared by a forked family stay resident, so
fork-heavy workloads benefit less than the totals suggest. And the per-frame lock
is held across the read that brings a frame back, which is a measured stall of
82 µs at p50 and 683 µs at p99.9 on device; the design that drops the lock across
the read and re-walks afterwards is written down and not yet built.

## 13.7 The growth fast path

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

## 13.8 Growing the stack downward, and the two bounds on it

Everything above grows the address space upward. The stack grows the other way,
and it is the one mapping a guest extends by *faulting below it*: a page under a
`P_GROWSDOWN` region is materialized on demand rather than refused.
`mem_growsdown_allowed` ([emu/memory.c:434](../../emu/memory.c#L434)) decides
which faults get that treatment, and it used to have almost no opinion — any
unmapped page whose next mapped neighbour anywhere above it happened to be
growsdown was mapped on demand.

That is worse than it sounds, in both guest widths. On a 64-bit guest the stack
is the *lowest* mapping in the address space, so every address below it — the
NULL page included — quietly allocated zero-filled memory instead of faulting.
A guest NULL store succeeded, and the crash arrived later, somewhere unrelated.
On i386 the address space is crowded: the stack starts just under `0xffffe000`
and musl's thread block sits about 134 MiB below it, so a runaway recursion
walked straight through that thread block. gnulib's "checking for working
sigaltstack" probe recurses without bound on purpose to provoke an overflow;
the `SIGSEGV` was delivered correctly onto the alternate stack, and the handler
then died on its first libc call, because the thread pointer it needed had been
overwritten on the way down. Every package configured after that one inherited
the crash, which is what took a Buildroot build down (issue #521).

Two Linux checks now bound the descent.

- **The guard gap.** Linux keeps `stack_guard_gap` — 256 pages — between the
  stack and whatever is mapped below it, and `expand_downwards` refuses a fault
  that would close it. AOK walks that gap rather than probing a single page,
  because the stack's *own* pages turn up inside it whenever a frame with a
  large local skips over a page on the way down; that hole must stay fillable,
  which is the clause Linux spells as `!(prev->vm_flags & VM_GROWSDOWN)`. The
  walk counts lazy reservations too (Chapter 5), since growing into one would
  leave a page both mapped and reserved — the state the lazy-mapping invariant
  forbids.
- **`RLIMIT_STACK`.** The gap alone bounds the stack by *where its neighbour
  happens to be*, which on a 64-bit guest is nowhere near. `exec`
  ([kernel/exec.c:966](../../kernel/exec.c#L966)) records the stack top and the
  soft limit — 8 MiB by default — and a fault more than that many pages below
  the top is refused. Measured before this existed: a runaway
  recursion drove the emulator to 1.42 GB resident before the descent reached
  anything, and on an iPad that is not a fault, it is jetsam killing the app.

`RLIM_INFINITY` passes through as "no bound known", and the guard gap still
applies. Two things change for a user: a runaway recursion now faults instead
of silently overwriting its neighbour, and `ulimit -s` actually bounds the guest
stack where it previously did nothing. `tests/manual/stack_guard_gap.c` is the
regression.

## 13.9 `brk`, and a one-page off-by-one everybody meets

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

## 13.10 `mremap`, and what has to survive a move

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

## 13.11 `madvise`: advice, and the parts that are not advice

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

## 13.12 `memfd` and sealing

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

## 13.13 The gap: `PROT_EXEC` is never enforced

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

## 13.14 What this layer is actually defending

Put the chapter together and the priorities are visible in the order the checks
run.

The guest is not being protected from itself. `PROT_EXEC` is unenforced, there
are no memory namespaces, and a guest process that corrupts its own heap is on
its own — the same as on Linux, minus one mitigation.

What is being protected is the *application*. Reservations are bounded so a
64 GiB `mmap` cannot consume a gigabyte of host RSS. Growth is refused before
the jetsam budget is exhausted so UIKit keeps working. A thread committing
memory the growth guards never saw is braked, and killed if braking does not
stop it, so that one guest process dies rather than the app. Cold pages are
written to a swap area — with a write budget, a thrash guard and a suspension
gate, because the device belongs to somebody. Page-table mutations take a
barrier that parks siblings rather than spinning them, so a `mprotect` storm
cannot starve the main thread. Every one of those exists because the failure it
prevents is not "the guest program crashes" but "the terminal disappears".

That is the inversion at the centre of this whole project, and memory management
is where it is most visible: the kernel's most important client is the process
it is running inside.

---

*Anchors:* [kernel/mmap.c](../../kernel/mmap.c) (`mem_growth_refused`,
`mem_fault_backpressure`), [kernel/memfd.c](../../kernel/memfd.c),
[kernel/swap.c](../../kernel/swap.c), [emu/memory.c](../../emu/memory.c)
(`swap_evict_frame`, `swap_fault_page_locked`),
[emu/memory.h](../../emu/memory.h), [emu/tlb.h](../../emu/tlb.h),
[fs/dev.c](../../fs/dev.c) (`/dev/aokswap0`),
[kernel/exec.c](../../kernel/exec.c) (`mem_set_stack_bounds`),
[platform/platform.h](../../platform/platform.h) (`host_mem_headroom_low`,
`host_mem_pressure_level`), [docs/simulated_swap_plan.md](../../docs/simulated_swap_plan.md),
[docs/TODO.md](../../docs/TODO.md) ("PROT_EXEC is never enforced"),
`tests/manual/mmap_shared_integrity.c`, `tests/manual/stack_guard_gap.c`,
`tests/manual/mem_guard_small_growth.c`. Deeper treatment for users:
[opt/AOK/docs/swap.md](../../opt/AOK/docs/swap.md).

*Story:* the silent headroom guard — every `mmap` in the application beginning
to fail with a clean `ENOMEM` and nothing anywhere saying why, diagnosed from a
Wayland compositor's next window simply not opening. And its sequel two years
later: the same guard, now loud, refusing a shell one megabyte because it had
never been told how much was being asked for.
