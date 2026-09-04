# Simulated swap for iSH-AOK: feasibility, recommended design, and plan

Date: 2026-09-02. Tree: `/Users/mke/git/ish-AOK`, branch `working`.
Line numbers were re-checked at `e12aeae36`; where a cited line moved since the
study began, the current number is used and the drift is noted.

Host used for every "Mac" number: this machine, macOS 26.5.2 (xnu-12377.121.10),
Apple silicon, 16 KiB host pages, APFS NVMe. Device numbers come from an M4 iPad
Pro (8 GB, 477 GB volume at 94-95% full) running iSH-AOK 553, reached over
`ssh m4pt`. Anything labelled **iOS-sourced** is read out of the
`apple-oss-distributions/xnu` sources (xnu-12377 generation) and has not been
executed on a device. Anything labelled **unverified** was neither measured nor
read out of primary source.

---

## 1. The answer

**Feasible, with caveats, and for a narrower purpose than the design study first
claimed.** AOK can page guest anonymous memory to storage and can present an
honest swap surface to the guest. The mechanism is the one the study converged
on: AOK itself evicts 16 KiB frames of guest anonymous memory to an unlinked,
preallocated file in `$TMPDIR`, marks the four guest pages behind them SWAPPED in
the page table, releases the host memory, and faults them back on demand,
reporting every guest-visible number from its own counters. Verification of the
seven load-bearing claims changed three of them, and the changes are real design
changes, not footnotes:

- **Eviction must protect what it releases.** `madvise(MADV_FREE_REUSABLE)`
  leaves the frame mapped, readable and byte-identical, so any pointer the design
  failed to account for writes into a released frame with no fault and no
  diagnostic, and the swap-in silently reverts the write. Measured. The fix is to
  follow the `MADV_FREE_REUSABLE` with `mprotect(PROT_NONE)` in production, not
  only under a test knob: it keeps the entire footprint drop, turns every stale
  access into SIGBUS at the guilty instruction, coalesces its `vm_map` entries
  back on restore, and costs 2.7 us per frame on eviction and 0.35 us on swap-in.
- **The aging clock cannot be lockless.** Page-table leaves really are immortal,
  but `struct data` is not: it is freed synchronously by every `munmap` under a
  write lock a lockless walker does not hold, at a measured 267-457 frees per
  second from one dull single-threaded guest. The clock hand must walk under
  `mem_read_lock_quiesce_aware` in bounded chunks, exactly as
  `collect_mem_page_stats` already does (fs/proc/root.c:400-418).
- **Eviction throughput is 100-200 MiB/s per victim process, not 1 GB/s.** The
  barrier is scheduler-bound, its cost is a per-busy-sibling scheduling latency,
  and each barrier also costs every sibling about 109 us of its own CPU that the
  original measurement never saw. The policy lever is batch size, not batch rate.

Together with the device I/O numbers (a cold 16 KiB read is 82-86 us at p50 and
110-152 us at p99, better than the Mac), that fixes what the feature is for. It
buys headroom for cold and idle guest memory: a large mostly-cold heap, a
background process tree, a build that has finished linking, the shared pages of a
process that has stopped. It does not make a hot working set larger than the
app's jetsam budget run well, and the report should never be read as promising
that. That is what swap does on a real system too.

Effort is **13-18 engineer-weeks** to a default-on, documented feature, up from
the 10-14 the synthesis estimated, because the three repairs, the exact-ownership
fix, the ledger-verification rule and two shipping bugs found along the way are
all new work. The first day was a device probe that could have killed the whole
family, and on 2026-09-03 it PASSED (see section 7, Day 1), and it now has to be run without a debugger attached, because the release
primitive silently does nothing while Xcode or Instruments holds a memory
snapshot of the region.

**Phase 0 ships alone and is worth doing whether or not swap is ever built.**

---

## 2. What verification changed

Seven claims were stated as load-bearing: if false, the verdict changes. Each was
attacked by three independent lenses (code, XNU/iOS platform, semantics and
performance). Three do not stand, two are contested, two stand.

| # | Claim (abbreviated) | Outcome | Effect on the design |
|---|---|---|---|
| C1 | `MADV_FREE_REUSABLE` on a 16 KiB frame raises `os_proc_available_memory()`; else `munmap`+`MAP_FIXED` does | **Contested** (1 of 3 refuted) | Mechanism holds and is source-verified through the arm64 pmap with no platform gate. Two changes: never trust `madvise()==0` as evidence of release, and gate eligibility on `struct data` fields rather than `P_ANONYMOUS` |
| C2 | Every host pointer is minted by `mem_ptr_nofault` under `mem->lock` and dropped before release, except `futex_load` | **Does not stand** (3 of 3) | Eviction gains a mandatory `mprotect(PROT_NONE)` companion; the pointer inventory becomes a CI check; two shipping bugs must be fixed first |
| C3 | The five `refcount` sites are the complete set where a `pt_entry` acquires or loses a `struct data` | **Stands** (0 of 3) | The site list is exhaustive. The `owners[2]` encoding is not exact and, without an overflow counter, is unsafe |
| C4 | Chunks and leaves are never freed before `mem_destroy`, so a lockless clock walk is safe | **Does not stand** (3 of 3) | The clock walks under the read lock in bounded chunks; kswapd never calls `mm_release` |
| C5 | The first store after any TLB fill takes the write-miss path, so a software dirty bit there is complete | **Contested** (1 of 3) | Hook on `type != MEM_READ`, not `MEM_WRITE`. One bypass exists and is a named precondition. The stake is corruption, not wear |
| C6 | The barrier costs tens of us, so 64-frame batches give 1 GB/s at under 10% stall | **Does not stand** (3 of 3) | Target 100-200 MiB/s per victim; scale batch size with thread count; direct reclaim needs a byte bound |
| C7 | A cold 16 KiB `pread` on device NAND is tens to low hundreds of us, not milliseconds | **Contested** (1 of 3), net strengthened | Now device-measured and good. Three new constraints: cap swap-out rate, watch swap-file fragmentation, keep every blocking thread at `QOS_CLASS_USER_INITIATED` |

### 2.1 C2: the released frame is not protected, and the failure is silent

Three lenses refuted C2, from three directions, and they agree.

The inventory half of the claim is close but not complete. Besides `futex_load`
(kernel/futex.c:206-211: `mem_ptr` at :207, `mem_read_unlock_quiesce_aware` at
:208, `*out = *ptr` at :211), the verification found:

- `mem_ptr` itself releases the read lock on its lazy, growsdown and COW upgrades
  (`read_to_write_lock` at emu/memory.c:1494, :1515, :1566), and
  `read_to_write_lock` releases the reader before waiting (util/rw_locks.h:238-244).
- The multi-pointer walks in kernel/user.c hold an already-minted write pointer
  live across a later `mem_ptr` that can drop that lock. `user_transform_two`
  resolves the write pointer at kernel/user.c:280 and the read pointer at :281
  and calls `fn(in_host, out_host, ...)` at :283 with both live; the same shape is
  in `user_transform_rect_two` (:351) and `user_transform_rect_three` (:400).
- `mem_ptr_fault` returns its pointer after `write_unlock` (emu/memory.c:1698-1700),
  safe only because its sole caller NULL-tests it (kernel/calls.c:5167-5171).
- The two COW source pointers compute `entry->data->data + entry->offset`
  directly and `memcpy` from them (emu/memory.c:1587, :1674).
- Three direct readers in the amd64 interpreter, debug-gated: emu/amd64_interp.c:450,
  :482, :922.
- The debug-gated `tlb->prev_write_ptr`, which survives the lock release and is
  guarded by the changes counter that the design says it never rests on.

Two other properties of the claim were also wrong: it is an instantaneous
statement used durationally (the design leans on it for the whole length of the
lock-free `pwrite`, during which a racing fault can republish the frame), and
"anywhere" is only true for a `struct data` owned by exactly one mem, because
`task_poke_shared_mem` skips tasks whose `other->mem != mem` (kernel/task.c:733-736).

The decisive finding is the platform one. AOK maps every guest anonymous page
host `PROT_READ|PROT_WRITE` (emu/memory.c pt_map_nothing) and host page-protection
mirroring is disabled twice over on Apple: `mem_can_mirror_host_page_protections`
returns `real_page_size == PAGE_SIZE`, which is 16384 != 4096, and
`mem_host_page_mirroring_enabled` additionally hard-codes it off under
`#if __APPLE__` (emu/memory.c:169-199). And XNU's `MADV_FREE_REUSABLE` does not
unmap, protect or zero anything: `vm_object_deactivate_pages(DEACTIVATE_REUSABLE)`
sets `m->vmp_reusable = TRUE` and bumps `object->reusable_page_count`
(exp/xnu/vm_object.c:2427-2436). So a stale pointer into a released frame
produces no fault at all.

Measured end to end on this Mac (`exp/c2plat/c2plat.txt` section B): a stale write
issued between the `pwrite` and the `madvise` lands with no fault; a stale read
during the swapped interval returns the correct old byte; a stale write after the
release lands and reads back; and after `MADV_FREE_REUSE` + `pread` both writes
are gone, byte-for-byte reverted. The log's own words: "a write by a missed holder
is SILENTLY LOST, with no fault and no diagnostic." The tree already carries the
scar of the weaker `munmap` version of this at emu/memory.c:1082-1096, which is
why `ISH_MEM_QUARANTINE` exists, and that knob is consulted only in
`pt_unmap_always_unlocked`'s last-reference free (:1173-1178) and so cannot reach a
swapped frame at all.

**The repair, measured.** Follow the `MADV_FREE_REUSABLE` with
`mprotect(PROT_NONE)` on the same 16 KiB, and reverse it on swap-in
(`mprotect(PROT_READ|PROT_WRITE)`, then `MADV_FREE_REUSE`, then `pread`). From
`exp/c2plat/c2plat5.txt`:

| | REUSABLE only | REUSABLE + PROT_NONE |
|---|---:|---:|
| footprint released, 4096 scattered frames | -67,108,864 B | -67,108,864 B |
| eviction cost per frame | 2.03 us | 4.72 us |
| swap-in cost per frame | 1.79 us | 2.14 us |
| `vm_map` entries while evicted | 2 | 8,193 |
| `vm_map` entries after restore | 2 | 2 |
| stale read of a released frame | returns old bytes, no fault | SIGBUS (10/10) |
| stale write of a released frame | lands, then silently reverted | SIGBUS (10/10) |
| swap-in correctness | 0/16384 bytes wrong | 0/16384 bytes wrong |

The order is forced: `MADV_FREE_REUSABLE` on a range that is already `PROT_NONE`
returns EPERM (`c2plat5.txt` section 2, errno 1). The entries coalesce fully on
restore, which `mmap(MAP_FIXED, PROT_NONE)` does not: 512 `MAP_FIXED` holes took a
mapping from 3 to 1027 entries and it stayed at 1027 after all 512 were restored
(`exp/c2plat/c2plat.txt` section E), while 256 `mprotect(PROT_NONE)` frames went
1 -> 512 -> 1. So `mprotect` is the right companion and also the right fallback if
REUSABLE turns out not to move the ledger on a device.

One measurement trap worth recording, because it will mislead the next person to
benchmark a release primitive: `mprotect(PROT_NONE)` over 512 MiB of dirty
anonymous memory drops **both** `phys_footprint` and `resident_size` by the full
512 MiB in 1.8 ms while freeing nothing. Host free memory does not move, nothing
is compressed, and all 131,072 pages read back correct afterwards
(`exp/c2plat/c2plat4.txt`). Both fields are pmap-derived. `mprotect` alone is not
a release primitive; it is only the guard rail around one.

### 2.2 C4: the clock hand must take the read lock

Both premises of C4 are true and were re-verified exhaustively by all three
lenses. Page-table leaves, chunks, the root array and both bitmaps are freed only
inside `mem_destroy` (emu/memory.c:616-622); `mem_pt_del` only stores
`entry->data = NULL` (:701); the bitmaps are set-only. `mm_retain` does hold
`mem_destroy` off, provided the retain is taken under `task->general_lock` as
every existing retainer does (fs/proc/pid.c:44-52, fs/proc/root.c:391-397).

The conclusion does not follow, because the walk the design specifies is not a
leaf-only walk. Synthesis section 2.4 lists as "checked locklessly by the scan":
`D->data`, `D->data != vdso_data`, `D->fd`, `D->cache_entry`, `D->shared_key`,
`D->frame_refs[f]`, and a host `mincore` on `D->data + (f<<14)`. Every one of those
dereferences `struct data`, which is freed synchronously by the live unmap path:
`pt_unmap_always_unlocked` calls `mem_pt_del` at emu/memory.c:1169, decrements at
:1170, `munmap(data->data, data->size)` at :1180, `free(data->host_page_prot)` at
:1196 and `free(data)` at :1197, under `mem->lock` write. A lockless walker holds
no read lock, is not waited for by `mem_write_lock_with_pokes`, and is not poked.

The two existing lockless walkers bound the safe envelope exactly, and the code
lens checked this before claiming a bug: `mem_mapped_page_count` (emu/memory.c:720-748)
only tests `entries[i].data != NULL` and never dereferences;
`mem_host_addr_to_guest` (:680-695) does dereference, but its only caller
`jit_translate_host_fault` runs on a faulting thread that is already inside
`read_lock(&mem->lock)` in `task_run_current`. There is no precedent in the tree
for a struct-data-dereferencing walk with no lock held.

Measured, and the numbers say this is not a tail case:

| measurement | value | source |
|---|---|---|
| `struct data` frees per lockless walk, one dull churning guest thread | 1.24-1.96 (267-457 frees/s) | `verifyC4sem/racewin.c` |
| faults dereferencing `struct data` in a 5 s writer/reader race | 42 | `exp/c4plat4.txt`, `c4plat5.txt` |
| leaf entries walked locklessly with zero faults from the leaf itself | 1,181,800 | same |
| candidates that passed a magic + `mincore` precondition but pointed at **another** mapping | 46 | same |
| ABA events: same `struct data *` value, different descriptor | 326 | same |
| `mmap(NULL,...)` returning a just-`munmap`'d VA | 2000/2000 | `exp/c4plat.txt` |

The platform lens adds that Darwin offers the walker no safety net either. XNU's
`mincore` has no map-presence check at all and returns 0 (success) rather than an
error when its argument sanitiser rejects the request (`exp/xnu/kern_mman.c:1524-1670`,
:1553-1555); measured, it returns success with `vec=0x80` for NULL, for `0x5`, for
a kernel VA, and identically for an unmapped hole, a `munmap`'d range, and a
mapped-but-untouched page. On Linux `mincore` returns ENOMEM on a hole, which is
probably where the design's intuition came from. Combined with 2000/2000 VA reuse,
the usual outcome of following a stale `D->data` is a **silent read of a different
live mapping**, not a crash. And the barrier's stated re-validation ("same `data`
pointer") does not save it: libmalloc returns the same address for a new
`struct data` up to 20/20 times in free-then-alloc pairs.

**The repair.** The clock hand runs under `mem_read_lock_quiesce_aware`, in
bounded chunks (one leaf, or one 4 MiB granule, per acquisition, dropping and
re-taking between chunks). This is the pattern `collect_mem_page_stats` already
uses (fs/proc/root.c:400 lock, :418 unlock, with the `mm_retain` from
`task_snapshot_collect`). The read lock is shared with running guest threads, so
it does not stop them; it only delays a concurrent barrier writer, and only for
the length of one chunk. Nothing else in the design changes: the `accessed`/`age`
bytes still live in the leaf, and the barrier re-validation still runs.

The cost model in the synthesis was also wrong in its denominator. Because leaves
are immortal, the walk's cost is O(high-water populated 4 MiB granules), not
O(resident pages), and never decreases:

| measurement | value |
|---|---|
| 4 GiB address space, 512 KiB touched | 4.9-11.6 ms per pass |
| the same after unmapping all 4 GiB, 236 kB resident | 4.0-7.1 ms per pass, forever |
| 1 GiB clean case: base / mapped / after unmap | 104 us / 1371 us / 1171 us permanently |
| marginal per-entry cost | 4.07 ns (matches 3.9-4.8 ns while mapped) |
| per allocated pgdir chunk | ~38 us |
| clock hand RMW vs read-only walk | +10%, +26% with a concurrent stamper |

So budget **1.1-1.7 ms per GiB of ever-populated address space** per full pass,
not "3-5 ms per GiB of resident". At a 250 ms pass interval that is a fraction of
a percent of duty, and the read-modify-write of the clock hand is not the
expensive part. This cost is guest-visible today, not only under swap: the same
walk backs `/proc/<pid>/statm` (fs/proc/pid.c:398), `stat` (:196), `status` (:581)
and `ru_maxrss` (kernel/resource.c), so `top`/`htop`/`ps` refresh latency already
grows with a process's historical peak and never recovers.

One more repair from C4: **kswapd must never call `mm_release` directly.** If it
is the thread whose release hits zero it runs `mem_destroy` with `current == NULL`,
and the bounded-teardown guard at emu/memory.c:597-600 (`current != NULL &&
current->mm_teardown`) then feeds `dying = false` into fs/fuse.c:587, making a
FUSE flush wait unbounded, which is exactly the hang emu/memory.c:590-596 exists to
prevent. Hand the mm to a reaper, or give kswapd a synthetic `current`.

### 2.3 C6: throughput is 100-200 MiB/s, and the lever is batch size

All three lenses refuted C6, and the first refutation is arithmetic inside the
design itself. A frame is 16 KiB, so a 64-frame batch is 1 MiB, so "about 1 GB/s
of guest data per wall second" requires about 1000 barriers per second on one mem.
The same document caps a mem at one batch per 10 ms (synthesis section 2.10 limit
(d)), which is 100 MiB/s, and caps writes at 4096 MB/day, which 1 GB/s exhausts in
4.1 seconds. Claim C6 and section 2.10 cannot both be implemented.

The proxy the claim used is exact as far as it goes: `sys_mprotect_guest` really
is `mem_write_lock_with_pokes` plus `mem_changed` (kernel/mmap.c:870 and :876;
`pt_set_flags` ends in `mem_changed` at emu/memory.c:1395). But the barrier is
scheduler-bound, not work-bound: `mem_write_lock_with_pokes` (kernel/mmap.c:48-74)
pokes every sibling sharing the mem and then spins 256 `sched_yield` followed by
768 `nanosleep(lock_pause)` at 2 us each, and convergence needs every sibling
scheduled once. AOK runs one host thread per guest thread (kernel/task.c:1934-1996,
`QOS_CLASS_USER_INITIATED` at :1947) with no cap at the advertised CPU count, so a
100-thread guest process is 100 runnable host threads.

Measured, arm64 Alpine guest on this Mac (three independent runs, host loaded to
varying degrees, so absolute values are upper bounds; the shape is consistent
across all three lenses):

| busy siblings | median barrier | mean | p99 |
|---:|---:|---:|---:|
| 0 | 5-9 us | 6.3-12.2 us | -- |
| 1 | 25-33 us | 37.9-409 us | -- |
| 3 | 71-90 us | 75.8-201 us | -- |
| 6 | 148-159 us | 155-239 us | 1097 us |
| 12 | 335-519 us | 1117-1561 us | 11.9-16.7 ms |
| 24 | 187 us | -- | 20.2 ms |
| 96 | 3.66 ms | 7.57 ms | 60.5 ms |

The claim's own named falsifier ("milliseconds per barrier on a 100-thread
process") fires at 12 threads on a 10-core Mac, not at 100 on an A9.

Two costs the caller-side timer never saw:

- **The victim's own CPU.** Every batch calls `mem_changed()`, invalidating the
  whole 1024-entry software TLB of every thread of the mem (emu/memory.c:1459-1461;
  `tlb_flush` at emu/tlb.c:658-662). Measured at 3 threads with a 4 MiB working
  set: `mprotect` costs 111.6 us of extra victim CPU per thread per barrier, of
  which 77 us is TLB refill and 31 us is the quiesce machinery, against 34.6 us
  for `madvise(MADV_NORMAL)` (same barrier, no `mem_changed`) and 3.2 us for a
  `getppid` control. Corroborated independently: measured guest TLB miss cost is
  74 ns, times 1024 entries, is 76 us.
- **The blocked growers.** `mem_write_lock_with_pokes` takes `pt_alloc_lock`
  first (kernel/mmap.c:49) and holds it across the whole acquire and hold, and the
  pure-growth `mmap` fast path, explicitly "the musl mallocng hot path"
  (kernel/mmap.c:39-46, :337-349), takes the same mutex.

Measured directly at the required rate: 6 threads, 256-page batches, asking for
1000 barriers/s gave 24.5% / 30.3% / 47.4% duty across three runs, and only
336-541 barriers/s were achieved because barrier latency ate the inter-barrier
budget. A second lens, pacing to 954 barriers/s with 4 busy threads and tagging
each sample with host idle, saw a **minimum** duty of 12.2% at 60.5% host idle,
rising to 61.7% at 0.4% idle.

**The repair.** Batch size is nearly free and batch rate is not. With zero
siblings, 1 page under the lock is 4.8 us and 256 pages is 29.3 us, so 255 extra
page-table entries cost about 24 us; another lens measured 2-8 us for the same
delta at matched thread counts. So scale **frames per barrier** with the victim's
live thread count (64 at 1-2 threads, 512 at 8 or more), and state the throughput
target as **100-200 MiB/s per victim process**. A 512-frame batch cuts the barrier
count eightfold for tens of microseconds more hold time.

Independently, C7's device measurements say swap-out should be capped near
100 MiB/s anyway, because a 217 MiB/s writer took cold-read p99 from 129 us to
1101 us and max to 10.7 ms. The two constraints agree, which is reassuring.

Two further C6 findings that must be fixed:

- **Direct reclaim cannot restore the headroom it is called to restore.** Section
  2.10 bounds it to "about 20 ms of the calling mem's oldest candidates" while
  limit (d) imposes a 10 ms minimum inter-batch interval and a batch is 1 MiB, so
  the bound is 2 MiB, against a guard that fires when `os_proc_available_memory()`
  is under `ISH_GUEST_MEM_HEADROOM_MB` (default 192, platform/darwin.c:189-214).
  Direct reclaim needs a byte bound and must be exempt from the inter-batch
  interval.
- **The evictor's poke does nothing today.** `mem_write_lock_with_pokes` passes
  `current` (kernel/mmap.c:59, :72) and `task_poke_shared_mem` returns immediately
  for `task == NULL` (kernel/task.c:729-731), so a task-less evictor on a GCD or
  pthread worker would drain with no pokes at all. The two-line fix (skip by
  `pthread_equal(other->thread, pthread_self())` instead of by task identity) is
  already in the plan; it is a prerequisite, not a nicety.

### 2.4 C1: the mechanism holds; the observable does not

The source chain is verified end to end and has no platform gate:
`os_proc_available_memory()` is `memlimit - get_task_phys_footprint(task)`,
clamped at 0 (`exp/xnu/kern_memorystatus.c:9679-9717`, with a macOS-only early-out
at :9681-9685 that is why this Mac reads 0); `TASK_VM_INFO.phys_footprint` is the
identical call (`exp/xnu/task.c:6136-6137`), so every Mac `phys_footprint` delta is
byte-for-byte the `avail` delta a device would show; `madvise` reaches
`vm_map_reusable_pages` with no OS conditional (`exp/xnu/kern_mman.c:1409-1500`,
`vm_map.c:17111-17275`); the arm64 pmap debits `task_ledgers.phys_footprint`
inline by 16 KiB and credits it back on REUSE (`exp/xnu/pmap.c:8095-8117`). The
`exp/xnu/vm_map.c` copy is byte-identical to GitHub main. Measured on this Mac,
byte-exact and repeatedly: aligned 16 KiB REUSABLE is -16384; REUSE alone is
+16384 with no write needed; `mprotect(PROT_NONE)` and `munmap` and
`mmap(MAP_FIXED, PROT_NONE)` are -16384 in every state tested.

Two corrections matter for the implementation.

**The primitive is conditional and its failure is silent.** `vm_map.c:17190-17253`
returns `KERN_SUCCESS` with `kill_pages = -1`, bumping `vm.reusable_pages_shared`
and changing no ledger, whenever the VM object is COW-shared or shadowed. That
state is entered by `fork()`, by `mach_vm_read` / `mach_vm_read_overwrite` /
`mach_vm_copy` above 32 KiB on arm64, and by dirtying a `MAP_PRIVATE` file
mapping; it persists after the sharer is gone until a write fault collapses the
chain. Measured: a live `vm_read` copy gives ret 0 delta 0; a post-fork mapping
gives ret 0 delta 0 for the whole mapping; a dirtied 16 MiB private file mapping
gives ret 0 delta 0; a 4 KiB-page map is a silent no-op; a non-writable range is
EPERM. AOK itself never host-forks (`nlibc_fork` is ENOSYS, kernel/native_libc.c:2103)
and its own `pwrite`/`pread`/pipe/`F_NOCACHE` paths are clean (512 cycles of
`pwrite16K` + REUSABLE / REUSE + `pread` moved the ledger by exactly 8,388,608 with
`reusable_pages_shared` at 0). But **a debugger does**, so the day-1 probe reads a
false negative under Xcode or Instruments, and production must confirm the ledger
moved rather than trust `rc == 0`.

**The excluded file class is excluded for the wrong reason.** Synthesis section
2.8 justifies excluding file mappings with "on macOS they are not charged at all
(`exp/run1.txt` c1)", but c1 is `MAP_SHARED`. Guest-written `MAP_PRIVATE` file
pages, which is every relocated `.data`/`.got`/RELRO page of every ELF and library
(fs/real.c maps every guest file `MAP_PRIVATE|PROT_WRITE` on Apple), **are**
charged 1:1 to `phys_footprint`, measured at +16,777,216 for a 16 MiB private file
mapping and 513.2 MiB in `exp/run1.txt` d2. On iOS they are compressed, not paged.
REUSABLE cannot release them at all; only the `mprotect`/`MAP_FIXED` path can. So
the exclusion is right, the stated reason is wrong, and there is a real per-process
footprint class the design never reaches. Conversely, COW copies of such pages are
real anonymous host memory that the `P_ANONYMOUS` test needlessly excludes, because
both COW breaks inherit the source's flags (`entry->flags & ~P_COW`, emu/memory.c:1596
and :1682). Both are fixed by gating on `struct data` fields instead of the
guest-visible flag.

### 2.5 C3: the site list is exhaustive; the encoding is not

C3 survived all three lenses, and each of them ran the claim's own falsifier
repo-wide rather than inside `emu/memory.c`. Every write to `struct data::refcount`
in the tree is one of the five named sites plus the two rollbacks
(emu/memory.c:1057, :1060-1061, :1170, :1271, :1298, :1443, :1446, confirmed again
at HEAD). Every assignment of a `pt_entry`'s `data` field is emu/memory.c:701
(`mem_pt_del`, whose sole caller is :1169, inside site 2), :1065, :1272, :1299,
:1450. There is no structural copy of entries anywhere: leaves are `calloc`'d
(:142-143) and never `memcpy`'d, `mm_copy`'s wholesale `*new_mm = *mm`
(kernel/mmap.c:152) is repaired by `mem_init`'s fresh `calloc` of `pgdir_root`
(emu/memory.c:513-518) before the child is reachable, and `mem_destroy` cannot
strand a live entry because `mem_set_page_limit` runs only on an empty mem. The
only cross-mem entry point is `pt_copy_on_write` at :1450.

What fails is the `owners[2]` encoding with "overflow = sticky unevictable". It
was measured to trip in under a second of ordinary shell use: `sh -c '( sleep 3 ) &
( sleep 3 ) & sleep 1; cat /proc/$$/smaps'` shows `/bin/busybox` at 3 sharers, and
a plain 16 MiB anonymous heap with two non-exec children reports `sharers=3`. Once
tripped, the data is pinned for the life of the process even after the extra mems
are reaped. Worse, without a guard the scheme is **unsafe** rather than merely
conservative: with A in slot 0, B in slot 1 and C overflowed, A unmapping frees
slot 0 and the data then reads as exclusive to B while C still maps it.

The fix costs one counter at the same five sites: keep the two slots, add
`uint32_t overflow_entries`, and never free a slot while `overflow_entries != 0`.
Then `n_owners == 1 && owners[0].mem == M && overflow_entries == 0` is exact in
both directions and recovers milliseconds after a fork-and-exec. The hook itself is
cheap: measured 7.6 ns for lock plus two-slot compare plus two increments plus
unlock (5.4 ns for the mutex pair on 64 rotating stripes, 6.8 ns on one hot
stripe), which against a measured guest `fork+_exit+waitpid` marginal cost of
1.35-2.65 us per page is 0.6-1.1% of `fork`. An earlier estimate of "20-35% of
munmap" was 4x too pessimistic.

Two more precisions the design must carry. `refcount` counts `pt_entry`s, not
mems, and `pt_dup`/`pt_move` inflate it within one mem, so the per-owner entry
count is load-bearing. And exclusive `struct data` is not the same as an exclusive
host frame: the vdso is one static array (kernel/vdso.c:8-11) that
`pt_map`'d into every 32-bit process at kernel/exec.c:948 gets its own `struct
data` per exec, each of which reads as "exclusive". In `build/ish` `vdso_data`
sits 11,804 bytes into a host page, so its frames also contain unrelated emulator
globals including a live lock. Three separate filters catch it, but none of them is
the ownership record, so the eligibility predicate has to be written down as a
precondition of the record's meaning, not as a filter someone can later relax.

### 2.6 C5: the funnel is real; the predicate and the stake were both wrong

The mechanism half of C5 was confirmed harder than the claim stated it, by three
different methods. `page_if_writable` is written in exactly four places tree-wide,
and `tlb_handle_miss` sets it to the page only for `MEM_WRITE` and to
`TLB_PAGE_EMPTY` otherwise (emu/tlb.c:713-717 at HEAD; the claim cited :516-520,
which moved). All five gadget sets compare it on the write path. One lens drove 15
store shapes through the real emulator under lldb; another built a guest-visible
instrument out of the fact that a COW break calls `task_count_minflt()` exactly
once per page and ran 32 operations across three guest ABIs. Every store shape
(plain, SIMD, pair, `dc zva`, `rep stosb`, `rep movsb`, `memcpy` destination,
`read(2)` destination, every atomic that stores) took exactly one `MEM_WRITE`
resolve; every load shape took none; an unaligned store across a page boundary took
two.

Three corrections:

1. **The predicate is `type != MEM_READ`, not `type == MEM_WRITE`.** A ptrace poke
   and a `/proc/<pid>/mem` write enter `mem_ptr_nofault` with `MEM_WRITE_PTRACE`
   (kernel/user.c:136; and `mem_ptr_nofault` at emu/memory.c:1474-1478 deliberately
   lets it past both protection gates), measured at delta 1 on two ABIs. The design
   body already says `type != MEM_READ`; only the claim's prose was wrong.
2. **There is one bypass.** `kernel/mmap.c` `MADV_REMOVE` zeroes the page in place
   with `memset((char *) pt->data->data + pt->offset, 0, PAGE_SIZE)` and calls no
   `mem_ptr` at all. It is gated on `P_SHARED`, which section 2.4 already excludes
   from eviction, so it is inert today. That makes it a **named precondition** of
   the design, not an absence, and it must be written down, because `MAP_SHARED`
   anonymous memory becoming evictable is exactly what phase 4 proposes.
3. **The stake is backwards.** The synthesis says an incomplete dirty bit costs
   NAND wear "not correctness". Following the design's own state machine, a
   `PT_CLEAN` frame is discarded with no write and restored from its slot, so a
   missed store is **silently reverted**: guest memory corruption, invisible until
   the guest reads stale data. XNU offers no backstop, since a write to a reusable
   page moves no accounting and takes no fault (`exp/run3.txt`). Extra writes are
   the consequence of the *opposite* error, an over-approximating dirty bit, which
   is the safe direction.

Two smaller notes. `arm64_cas`/`arm64_casp`/`x86_atomic_cas` resolve the write
pointer before knowing whether the compare succeeded, so a failing LSE `casal` and
a failing i386 `lock cmpxchgl` both mark the page dirty; the guest advertises
`atomics` in `/proc/cpuinfo`, so glibc outline-atomics, Go and Rust take that
path. And the swap-cache saving is per 16 KiB frame, not per 4 KiB page, so
"read-mostly data costs one write ever" holds only when all four guest pages of a
frame stay unwritten.

`ISH_FORCE_WRITE_REVALIDATE` (emu/memory.c:546-550) already forces every store
through `mem_ptr_nofault(MEM_WRITE)` and is a ready-made oracle for validating the
dirty bit: any frame marked CLEAN without the knob but RESIDENT with it is a missed
store path. It costs +91% user CPU on a busy-loop, so it is a test knob only.

### 2.7 C7: device-measured, and better than the Mac

C7 was the one claim that got *stronger* under verification, because two lenses
took it to the device instead of extrapolating. The claim's own evidence was bad
(two of the four cited numbers were sequential readahead-amortised scans, and one
was a warm read wearing a cold label because `F_NOCACHE` is a retention hint, not
`O_DIRECT`: identical file and offsets gave p50 2.29 us cached and 2.42 us with
`F_NOCACHE`). The replacement numbers are real.

**M4 iPad Pro, iSH-AOK 553, volume 94-95% full, one sample per 16 KiB slot in a
random permutation over a file far larger than RAM, page-cache hits separated out:**

| condition | n | p50 | p90 | p99 | p99.9 | max | over 1 ms |
|---|---:|---:|---:|---:|---:|---:|---:|
| idle, 16 KiB | 2702 | 86 us | 114 us | 152 us | 961 us | 2221 us | -- |
| idle, 4 KiB | 2687 | 82 us | 109 us | 131 us | 309 us | 1571 us | -- |
| idle, 16 KiB (second lens, 10 GiB file, 100% media) | 32768 | 82 us | 88 us | 110 us | 190 us | 556 us | 0.00% |
| concurrent writer at 6.2 MiB/s | 2735 | 82 us | 109 us | 129 us | 198 us | 280 us | -- |
| concurrent writer at 51.6 MiB/s | 2727 | 86 us | 111 us | 126 us | 176 us | 272 us | -- |
| concurrent writer at 217 MiB/s | 2848 | **132 us** | **367 us** | **1101 us** | 2447 us | **10,726 us** | -- |
| under 3 x 2 GiB fsynced writeback at 1.18 GB/s | 32768 | 111 us | 377 us | 416 us | 683 us | 52,978 us | 0.05% |
| hot control through the guest (AOK + fakefs + realfs overhead) | 20000 | 1.0 us | 2.0 us | 2.0 us | 8 us | 22-35 us | 0.00% |

**This Mac, same shape (native, and through the Alpine guest):** p50 106-118 us,
p99 232-4164 us, p99.9 1.1-9.8 ms, max 8.4-39.2 ms, 0.28-3.25% over 1 ms. So the
Mac is the pessimistic proxy, not the optimistic one, and the study's
"device unverified" hedge was pointing at the wrong risk. Read-size sweep on macOS
explains the shape: of about 115 us, roughly 75 us is the shared XNU plus
controller round trip and only about 40 us is media, which is why 4 KiB is not
cheaper than 16 KiB.

Three constraints fall out, none of which was in the design:

1. **Cap swap-out near 100 MiB/s.** A writer at 6 or 52 MiB/s costs nothing; at
   217 MiB/s p99 goes to 1.1 ms.
2. **Watch swap-file fragmentation.** `F_PUNCHHOLE` churn is what both Design B
   and Design C do for the life of the process. 60,000 punch-and-rewrite cycles on a
   1 GiB file took it from 2 extents to 55,197 (44,756 after refill) and p99 from
   212 us to 3.9 ms on macOS. Judge the slot allocator on resulting extent count,
   not only on space efficiency. Device magnitude unverified.
3. **Keep every thread a guest fault can block behind at `QOS_CLASS_USER_INITIATED`
   or above.** XNU maps thread QoS to the disk throttle tier
   (`exp/c7plat/xnu-thread_policy.c:96-101`: USER_INITIATED -> TIER0, UTILITY ->
   TIER1, BACKGROUND -> TIER2), and tiers 1-3 sleep I/O for 5-25 ms per throttle
   period (`debug.lowpri_throttle_tier{1,2,3}_io_period_ssd_msecs` = 5/15/25).
   Measured on macOS: p90 119 us at IMPORTANT versus 2888-2962 us at
   STANDARD/UTILITY/THROTTLE. AOK is safe today only by accident, because
   kernel/task.c:1947 sets `QOS_CLASS_USER_INITIATED` for an unrelated CPU-scheduling
   reason. That line needs a comment, and the design needs an explicit constraint,
   or a future "put swap I/O on a background queue" change degrades swap-in 25x at
   p90 with no test that catches it.

A fourth finding is load-bearing and was nearly missed. The one thing that moved
the tail by orders of magnitude was **writeback backlog**. A device fill of
1.5 GiB with a single trailing `fsync` produced a *hot* control (page-cache reads
that never touch media) with p99.9 of 17.7 ms and 0.51% over 1 ms, and a cold pass
in which 12% of media reads exceeded 5 ms. Both vanished completely once the fill
`fsync`ed every 256 MiB. So the design's `fsync`-every-64-MiB cadence is what keeps
the swap-in tail near 400 us instead of 20 ms, and that reason must be written down
next to the jetsam-writeback reason it was originally chosen for, or a later
optimisation that relaxes the cadence to save NAND writes reintroduces a 20 ms
swap-in tail.

Practical translation. At 82-86 us per 16 KiB quartet, a cold major fault is about
20.5 us per guest 4 KiB page with quartet locality and 82 us without, against
7.14 us for today's COW break: 3x to 12x, not a different universe. Single-threaded
restore of a fully swapped 512 MiB process is about 2.7 seconds, or about 0.35 s at
QD8 if the design prefetches (QD1 129 MiB/s -> QD8 512 MiB/s measured on macOS).
The thrash guard's 4096 pswpin pages/s is 1024 slot reads/s is 84 ms/s, about 8%
I/O duty, which is a sane trip point at this latency.

---

### 2.8 C8: the slot belongs to the frame, not to the page-table entry

**Found while building the foundation, 2026-09-04. It changes 3.3 and 3.5.**

Section 3.3 puts the slot number in `struct pt_entry` and has
`pt_dup`/`pt_move`/`pt_copy_on_write` copy it. That makes N independent copies
of one fact, and they go out of step the instant any one of them is published
resident on its own.

The reachable case is an `mremap` out of an evicted frame. `pt_move` copies
`data` and `offset` verbatim -- only the GUEST address changes -- so the moved
entry still describes the same host frame, and under 3.3 it also still holds
that frame's slot. Touching any sibling brings the frame back and publishes the
three entries that are still adjacent; the moved one is not adjacent, so it
stays SWAPPED holding the slot. The guest then writes the three siblings, and
the moved page's own fault re-reads the whole 16 KiB frame from the slot,
**silently reverting all three writes to their pre-eviction values**.

Measured, both directions, on `alpine-arm64-test` and `alpine-amd64-test`
(scratch `mrem.c` drives the ordering deterministically and single-threaded):

| build | result |
|---|---|
| slot kept after the frame returns (3.3's shape) | `MREMAP GAP REPRODUCED clobbered=3`, each page reverted to its pre-eviction value |
| slot owned by the frame | `clobbered=0`, `frames_cancelled 1` |

**The correction.** `struct data` gains `_Atomic uint32_t *frame_slot`, one
entry per host frame, `SWAP_SLOT_NONE` meaning "the bytes are in host memory".
That is the only place a slot is ever recorded. `pt_entry` keeps a
`swap_state` byte and nothing else, and that byte is now explicitly a
**conservative hint**: PT_SWAPPED over a resident frame is harmless and costs
one trip to the slow path, while PT_RESIDENT over a released frame is a host
fault in emulator C code and is what eviction's preconditions (exact ownership,
plus `frame_refs[f]` equalling the entries actually found) exist to prevent.

Swap-in then reads the FRAME's slot under a per-frame striped mutex, and
**cancels with no I/O** when it finds `SWAP_SLOT_NONE`. That single test
subsumes three of 3.5's five slot states: WRITING, CANCELLED and CACHED-on-
return all present as "the bytes are already here". READING becomes "another
thread holds the frame lock", which is what the mutex is for, so the condvar
goes too. It also makes the post-fork case correct rather than merely benign --
two address spaces holding entries into one released frame each fault under
only their own write lock, and the loser of that race now finds the slot
already cleared instead of pread-ing the frame a second time.

**What this constrains, and it is not only a performance matter.** Because the
per-entry byte is a hint, **it is not a swap-accounting source**. After a fork,
the address space that did not fault a frame back keeps its entries reading
SWAPPED over memory that is resident, so 3.12's `VmSwap` and `Rss = Size -
Swap` must be derived from the frame's slot table and never from the entries.
Counting entries instead would let the sum of per-process `VmSwap` exceed
`SwapTotal - SwapFree` the moment slots are actually freed -- memory counted in
nobody's Rss and in no slot. Linux closes that book with `SwapCached`; this
shape has nothing to close it with until 3.6's cache lands, which makes the
cache a correctness prerequisite for 3.12's identities rather than only the
performance extension the next paragraph describes. Every reader of the hint
that is not the fault path has already had to be corrected once:
`mem_resident_page_count` reported 8 MiB of resident memory as absent after a
fork, and `swap_frame_eligible` made a frame permanently unevictable. Both now
ask the frame.

**What this does not lose.** The swap cache of 3.6 -- a frame that is resident
AND whose on-disk copy is still valid, which is what makes a clean frame's
second eviction free -- is a strict extension of this shape, not a casualty of
it: add a per-frame state byte distinguishing "resident, slot still valid" from
"released", and keep the slot across a fault instead of clearing it. The dirty
bit that invalidates the cached copy is the same first-write signal 3.6
describes. What has gone is only the per-entry copy of the slot number, which
was never a cache and only ever a second source of truth.

## 3. The recommended design

Unchanged from the synthesis except where marked **[amended]**. Design B's pager
is the mechanism; Design C's guest contract, minus anything that needs a
guest-visible file; Design A's packer, chunked preallocation, coalesced punch,
exit truncate, pressure source and device probe.

### 3.1 Mechanism

Guest anonymous memory is born and lives exactly as today: one host
`mmap(MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE)` per mapping via `pt_map_nothing`,
lazy reservations for large anonymous maps, COW on fork. Under budget pressure a
`kswapd` host thread, plus bounded direct reclaim at the three headroom guards,
evicts cold **frames** to 16 KiB **slots** of one unlinked, preallocated file in
`$TMPDIR`. "Swapped" is a per-`pt_entry` state byte; `mem_ptr_nofault` returns NULL
for it, so every JIT, interpreter, HLE, accelerator, native-program and
syscall-side access faults exactly as an unbacked page does today. Swap-out takes
the process barrier only to flip four bytes and bump `mem_changed()`; the I/O and
the release run outside every address-space lock. Swap-in takes no barrier at all.
A software dirty bit falls out of the TLB's read-fill/write-miss asymmetry, giving
a real swap cache.

### 3.2 Granularity: the frame

The host releases, replaces, protects and re-accounts memory only in 16 KiB units
on Apple silicon. `munmap`/`mprotect` of 4 KiB is EINVAL, `madvise` rounds
outward, and a 4 KiB host `mmap` costs 16 KiB of footprint (measured: 4096 separate
4 KiB mappings, zero pairs sharing a host page, minimum gap exactly 16384, 16.3 KiB
resident per mapping, a 4.00x ratio).

A frame `(D, f)` is the set of `pt_entry`s with `entry->data == D` and
`entry->offset >> 14 == f`. Membership is by data offset, not guest adjacency,
because `pt_move` copies `offset` verbatim (emu/memory.c:1298-1302), so after a
partial `mremap` the four entries may be non-adjacent or one may be gone.
`struct data` gains `uint8_t *frame_refs`, one byte per 16 KiB of `size`,
maintained at the five refcount sites. A frame is evictable only when the entries
found equal `frame_refs[f]`.

**[amended]** A COW-broken singleton has `size == 4096`, so the eviction I/O must
clamp to `D->size` rather than always writing 16384. On this 16 KiB-page host a
4096-byte `mmap` happens to be readable, writable and REUSABLE-able across the full
16 KiB, so the current spelling is safe by luck of the length round-up, not by
design, and it is 4x the I/O per useful guest page. Phase 0's packer removes most
of these anyway.

### 3.3 Representation

The `pt_swap` union replaces the 32 vestigial `blocks[2]` bytes in
`struct pt_entry` (emu/memory.h:275-282), **unconditionally**, because the
interpreter-only build has no `blocks[]` at all:

```c
struct pt_swap {
    _Atomic uint8_t state; // PT_RESIDENT=0 (dirty/unknown), PT_CLEAN=1, PT_SWAPPED=2
    uint8_t age;           // clock age
    uint8_t accessed;      // stamped by mem_ptr_nofault, cleared by the clock hand
    uint8_t locked;        // mlock / mlockall pin
};
```

**[amended -- 2.8]** No `slot` field. The slot lives on the frame
(`struct data::frame_slot`), because a per-entry copy silently reverts writes
after an `mremap` out of an evicted frame; the measurement is in 2.8. `state`
here is a conservative hint, not the truth: it may say SWAPPED over a resident
frame, and must never say RESIDENT over a released one.

`state == 0` is the only state that exists today, so `calloc`'d leaves and every
existing path yield RESIDENT entries. `pt_map` zeroes the bytes explicitly, and
so does `mem_pt_del` -- both, because a leaf is never freed, so a `pt_entry` is
a slot that gets REUSED and a stale SWAPPED byte would make some future
mapping's fresh page read as absent to every engine at once.
`pt_dup`/`pt_move`/`pt_copy_on_write` copy the state, so an alias never claims
a released frame is resident.

Not `flags`: `pt_map` writes only `data`, `offset` and `flags` (emu/memory.c:1065-1067)
and several sites derive a new page's flags from a neighbour's, so a `P_SWAPPED`
bit would propagate into fresh zero pages through `MADV_DONTNEED` and would hand a
`P_WIPEONFORK` child the parent's slot. Not `data->data == NULL`, which is per-data
and already means PROT_NONE-unbacked.

`mem_ptr_nofault` (emu/memory.c:1465-1483) gains, after its existing tests: one
acquire byte load with `state == PT_SWAPPED -> NULL`, an `accessed = 1` store, and
`type != MEM_READ && state == PT_CLEAN -> state = PT_RESIDENT`. **[amended]**
`type != MEM_READ`, not `type == MEM_WRITE`, so ptrace pokes and
`/proc/<pid>/mem` writes are caught.

### 3.4 Swap-out: the only path that takes the barrier

Precondition, re-checked under the lock: frame `(D, f)` of mem `M`; all entries
RESIDENT or CLEAN; `D` exclusively owned by `M`; entries found equal
`frame_refs[f]`; no entry `locked` or `accessed` since the last clock pass; host
`mincore` says resident.

**[amended]** The eligibility test gates on `struct data` fields, not on the
guest-visible flag: `D->data != NULL && D->data != vdso_data && D->fd == NULL &&
D->cache_entry == NULL && D->shared_key == 0 && D->name == NULL &&
!(flags & P_SHARED)`. Testing `P_ANONYMOUS` needlessly excludes COW copies of
private file pages, which are real anonymous host memory AOK owns (both COW breaks
inherit `entry->flags & ~P_COW`, emu/memory.c:1596, :1682), and it does not by
itself exclude the vdso, which is one static host array under N nominally exclusive
`struct data`s.

1. `mem_write_lock_with_pokes(M)`, exported, task-less caller allowed. This also
   takes `pt_alloc_lock`, excluding the growth fast path.
2. Per frame in the batch: re-read the four entries, same `data`, same `offset`,
   same state; under `swap_lock` allocate a slot (or reuse the CACHED slot when all
   four are CLEAN with the same slot, in which case there is no I/O at all);
   `slot->state = WRITING`; store `PT_SWAPPED` and the slot into the four entries;
   `M->swapped_pages += 4`; pin `D->refcount`.
3. `mem_changed(M)`.
4. `mem_write_unlock_with_pokes_nodrain(M)`.
5. No lock held: `pwrite(fd, D->data + (f<<14), min(16384, D->size - (f<<14)),
   slot<<14)`.
6. Under `swap_lock`, if still WRITING: `madvise(frame, 16384, MADV_FREE_REUSABLE)`,
   then **[amended]** `mprotect(frame, 16384, PROT_NONE)`, in that order (the
   reverse returns EPERM). **[amended]** Verify the ledger actually moved: one
   `task_info(TASK_VM_INFO)` per 64-frame batch, expecting a 1 MiB drop; if it did
   not, that batch's frames are on a shared or shadowed object and must be released
   by `mprotect` alone, which works in every state tested. Then `ON_DISK`,
   `pswpout += 4`. If CANCELLED (a fault raced in), the frame is live again; free
   the slot unless all four entries are CLEAN. Unpin, deferring the free if zero.
   Every 64 MiB of `pwrite`, `fsync(fd)`; never `F_FULLFSYNC`.

**[amended]** Batch size scales with the victim's live thread count: 64 frames at
1-2 threads, 512 at 8 or more. Target 100-200 MiB/s per victim.

Step 5's safety argument must be stated correctly, because C2 showed the original
wording is instantaneous where the design uses it durationally: the write is safe
because the four entries say SWAPPED and the barrier retired every TLB entry, and
it stays safe across a racing fault because of the CANCELLED protocol and step 6's
"unless all four entries are CLEAN" guard. Write both halves down, or the next
person to touch it will drop the guard.

### 3.5 Swap-in: no barrier, no write lock

`swap_fault(M, entry)`:

1. **[amended -- 2.8]** Under the FRAME's striped mutex read
   `D->frame_slot[f]`. `SWAP_SLOT_NONE` -> the bytes are already here, so
   CANCEL: do no I/O at all and go to 5. Anything else -> the frame is out, and
   holding the mutex is what "READING" used to mean, so there is no condvar and
   no per-slot state machine. This subsumes 3.5's WRITING, CANCELLED and
   CACHED-on-return states into one test.

   The original text read: *"Under `swap_lock` read `slot->state`. WRITING or
   CANCELLED (bytes still in the frame) -> CANCELLED, go to 5. CACHED -> go to
   5 with `PT_CLEAN`. READING -> wait on the slot condvar with no mem lock
   held. ON_DISK -> READING, pin `D->refcount`, release `swap_lock`."*
2. **[amended]** `mprotect(frame, 16384, PROT_READ|PROT_WRITE)`, then
   `madvise(frame, 16384, MADV_FREE_REUSE)`. REUSE is mandatory: a write into a
   REUSABLE range without it is not re-charged until the pager next scans the page,
   at which point the footprint jumps up with no AOK action.
3. `pread(fd, frame, 16384, slot<<14)`. EIO -> slot BAD, entry stays SWAPPED,
   return -EIO; the caller delivers SIGBUS.
4. **[amended -- 2.8]** Still under the frame mutex, store `SWAP_SLOT_NONE`
   into `D->frame_slot[f]` BEFORE releasing it and before publishing any entry,
   so the next fault on any entry of this frame -- in this address space or a
   forked sibling's -- cancels instead of reading the slot a second time.
   (When 3.6's swap cache lands this becomes "mark the frame resident and keep
   the slot", and the clearing moves to the first write.)
5. Publish this mem's entry: release-store `PT_CLEAN`, `M->swapped_pages -= 1`,
   `task_count_majflt()`, `pgmajfault++`, `pswpin += 4` once per slot read. No
   `mem_changed` is needed, because no stale TLB entry can exist for a page whose
   fill returned NULL.

Hooks: a prologue in `mem_ptr_fault` before its raw `write_lock` at
emu/memory.c:1618, reached from `handle_page_fault_interrupt` (kernel/calls.c:5167-5171);
and a first branch in `mem_ptr` (emu/memory.c:1484) that drops the read lock, runs
`swap_fault`, re-takes it quiesce-aware and re-walks.

**[amended]** `mem_ptr_fault` needs the same contract, spelled out, not the one
phrase the plan gives it. It holds the exclusive `mem->lock` from its first line
and every guest thread of the process runs holding the read lock
(kernel/task.c:834), so a naive implementation freezes the whole process for the
length of the `pread` (82 us p50 on device, 683 us p99.9, 53 ms once in 32768).
Give it the same drop-run-retake-rewalk shape as `mem_ptr` and re-fetch
`mem_pt(mem, page)` after every resume point.

### 3.6 Dirty bit and swap cache

`tlb_handle_miss` sets `page_if_writable = page` only for a `MEM_WRITE` miss and
leaves it `TLB_PAGE_EMPTY` on a read fill (emu/tlb.c:713-717), and all five gadget
sets compare it on stores, so the first store to a page after any fill takes the
write-miss path into `mem_ptr_nofault` with `type != MEM_READ`. After a swap-in the
entry is `PT_CLEAN`; the first write-miss turns it RESIDENT and releases the cached
slot reference lazily. A `PT_CLEAN` frame is discarded with the release primitive
alone, with no write, and is reported as `SwapCached`. A never-swapped page is
RESIDENT ("dirty or unknown"), so a first eviction always writes.

**[amended]** Measured re-runs put a clean discard at 1-4 us, not 0.7 us, and a
`pwrite`+release at 8-34 us, so the swap cache is worth 4x to 8x, not 8x to 48x.
The saving is per 16 KiB frame, so it needs all four guest pages of a frame to stay
unwritten.

**[amended]** The one precondition: `MADV_REMOVE` in `kernel/mmap.c` zeroes a page
in place with no page-table consultation. It is `P_SHARED`-gated and shared frames
are not evictable, so it is inert, but it must be recorded as a precondition and
re-examined before phase 4 widens the eviction set to `MAP_SHARED` anonymous.

### 3.7 Aging: the clock

- **Signal**: the `accessed` byte store in `mem_ptr_nofault`, on every TLB fill and
  every syscall-side page touch, and nowhere on a hit (JIT read hits touch no C
  code).
- **Refresh**: while active, `kswapd` bumps `mem_changed(M)` once per pass so
  every thread's next block boundary flushes its TLB and re-stamps what it touches.
  **[amended]** That refresh costs about 4.3 us fixed plus 110-170 ns per page of
  hot working set, so about 144 us per thread per pass for a 4 MiB hot set, not the
  "about 1 us per thread" the synthesis budgeted. Still 0.03% duty at a 250 ms
  pass; the number matters only because it is a licence someone might use to run
  the clock faster.
- **Hand [amended]**: a chunked walk under `mem_read_lock_quiesce_aware`, one leaf
  or one 4 MiB granule per acquisition, never lockless, holding an `mm_retain` taken
  under `trylock(&task->general_lock)` in the `collect_mem_page_stats` pattern
  (fs/proc/root.c:392-418). Group entries into frames: any `accessed` clears all four
  and sets `age = 0`; else `age++`; `age >= 2` is a candidate; faulted-in frames
  start at `age = 0, accessed = 1` so they survive two passes. Budget 1.1-1.7 ms per
  GiB of ever-populated address space per pass.
- **[amended]** `kswapd` never calls `mm_release` directly.

### 3.8 Ownership

`struct data` gains `struct data_owner { struct mem *mem; uint32_t entries; }
owners[2]; uint8_t n_owners;` **[amended] plus `uint32_t overflow_entries`**, and
`frame_refs`, maintained at the five refcount sites under a striped spinlock.
Acquire: bump the matching slot, else take a free slot, else `overflow_entries++`.
Release: decrement the matching slot, freeing it only when it hits zero **and**
`overflow_entries == 0`; else `overflow_entries--`. Exclusive iff
`n_owners == 1 && owners[0].mem == M && overflow_entries == 0`. Without the
overflow counter the scheme is unsafe, not merely conservative, and ordinary shell
use reaches three owners in under a second.

**[amended]** The stripe lock's ordering is not "under the mapping mem's write lock
only": the pure-growth `mmap` fast path runs `pt_map` under `pt_alloc_lock` plus
the mem **read** lock (kernel/mmap.c:39-46, :337-349). What serialises it against
the swap barrier is `pt_alloc_lock`, which `mem_write_lock_with_pokes` takes first.

Phase 1 evicts exclusive data only. Excluded: the shared image of every
fork-without-exec family, `MAP_SHARED` anonymous, SysV shm, every file mapping, the
vdso, `data->data == NULL`, lazy reservations. Eligible: every never-forked heap,
every packed singleton, every stack page, brk, `MADV_DONTNEED` replacements, 2 MiB
lazy chunks. Phase 4 takes ordered barriers on every owner in ascending
`struct mem *` order.

### 3.9 Locking

Outermost first:

1. `mm` pin: `mm_retain` under `trylock(&task->general_lock)`. Never held while
   blocking. **[amended]** `mm_release` from the evictor goes to a reaper.
2. `mem->pt_alloc_lock` + `mem->lock` write via the exported
   `mem_write_lock_with_pokes`; swap-out only; never across I/O, never across a
   condvar wait, never while taking `pids_lock`. The unlock variant does not drain
   deferred fds.
3. `mem->lock` **read**, quiesce-aware: **[amended]** now used by the clock hand as
   well as by swap-in's inspection; dropped around I/O; re-taken with a full
   `mem_pt` re-walk and state/slot re-check.
4. `data_owner_lock[stripe]`: leaf.
5. `swap_lock`: one global mutex over the slot table, bitmap, counters and
   condvars; leaf; condvar waits only with no mem lock held.
6. `struct data` lifetime: a `refcount` pin taken before the mem lock is dropped,
   released after the I/O, with a deferred free beside `deferred_fds`.

Forbidden: `swap_lock` -> any mem lock; `data_owner_lock` -> anything; a barrier
waiting for anything but readers leaving at block boundaries.

**[amended]** Prerequisites, in order, before any swap code:
`futex_load`'s post-unlock dereference; the `kernel/user.c` multi-pointer walks;
`dump_addr_backing`'s lockless `struct data` read; and the two-line poke fix so a
task-less caller pokes everyone.

**[amended]** If the pointer inventory cannot be made airtight, holding the write
lock across the `pwrite` is a viable fallback the study never considered: measured,
that is 262 us per batch, 2.6% at 100 MiB/s, and the barrier is not the bottleneck
anyway (about 16 us of marginal sibling time per barrier).

### 3.10 Trigger, watermarks, pressure, suspension

- **Signal**: `os_proc_available_memory()` (platform/darwin.c:200) with the
  `avail == 0` split fixed: 0 at boot means "no information" and keeps today's rule
  (darwin.c:202-205); 0 later means at or over the limit, which is CRITICAL.
  **[amended]** That distinction is real: `kern_memorystatus.c:9707-9710` clamps the
  result at 0, and it also returns 0 when `isApp` is false. On macOS and the CLI, a
  new `ISH_GUEST_MEM_BUDGET_MB` makes `host_mem_avail()` = budget minus
  `task_info(TASK_VM_INFO).phys_footprint`, so all of it is testable on this Mac,
  where `host_mem_headroom_low()` returns false unconditionally today.
- **Watermarks** (MiB): `low` = `ISH_GUEST_MEM_HEADROOM_MB` (192);
  `high` = low + 128. `kswapd` runs passes while `avail < high`, stops at
  `high + 64`, sleeps on a 250 ms timer otherwise.
- **Direct reclaim** at the `mmap` guard and the `mremap` guard, both of which run
  before their barriers; at `brk`, **before** `mem_write_lock_with_pokes`, because
  the existing check runs under that lock and a reclaim that takes the barrier from
  there deadlocks on its own non-recursive `pt_alloc_lock`. **[amended]** Bound it
  by bytes, not by 20 ms, and exempt it from the inter-batch interval, or it can
  never restore a 192 MiB headroom.
- **Event kicks**: `DISPATCH_SOURCE_TYPE_MEMORYPRESSURE` (WARN and CRITICAL) and
  `UIApplicationDidReceiveMemoryWarningNotification`, both unused today, writing a
  diagnostics breadcrumb with `avail` and `phys_footprint`. Jetsam kills have no
  preceding breadcrumb today.
- **Rate limits**: a daily write budget (`ISH_GUEST_SWAP_WRITE_MB_PER_DAY`,
  default 4096) past which only CLEAN frames are discarded; a thrash guard pausing
  swap-out when `pswpin` exceeds 4096 pages/s for 2 s; no re-eviction of a frame
  faulted in within two passes; **[amended]** an inter-batch interval that applies
  to background reclaim only, with batch size as the primary lever.
- **[amended] QoS**: the evictor thread and every thread a guest fault can block
  behind must be `QOS_CLASS_USER_INITIATED` or above, for the disk throttle tier as
  much as for CPU scheduling.
- **Suspension**: a `swap_quiesce_begin(ms)` beside `fakefs_quiesce_begin` in the
  assertion-expiration handler: stop new batches, wait at most 500 ms for in-flight
  writes, report stragglers; lifted first on foreground and by the existing 5 s
  auto-lift. Raw pthread primitives, `current == NULL`, taken before any mem lock.
  No lock is ever taken on the swap file.

### 3.11 Storage

One process-lifetime file from `host_unlinked_tmpfd()` (`mkstemp` in `$TMPDIR`
then `unlink`), never in the App Group container, a root's `data/`, or Documents.
Reserved with `fcntl(F_PREALLOCATE)` plus `ftruncate` in 256 MiB chunks. ENOSPC at
enable means swap off, never a smaller area. Slot table of 16-byte entries, 1 MiB
per GiB of swap, plus a free bitmap; states FREE / WRITING / ON_DISK / READING /
CACHED / BAD. `F_PUNCHHOLE` only when the free pool drops below a threshold or at
`swapoff`, coalesced and off every lock. `ftruncate(fd, 0)` at clean exit so a
clean exit writes nothing. Page cache kept, with `fsync` every 64 MiB;
`ISH_GUEST_SWAP_NOCACHE` is a knob.

**[amended]** Three additions. The `fsync` cadence protects read latency as well as
bounding the jetsam writeback backlog, and both reasons must be recorded.
`F_PUNCHHOLE` churn is a read-latency hazard, so the slot allocator is judged on
resulting extent count. And the file's data-protection class should be set
deliberately rather than inherited: nothing in the tree sets any
`NSFileProtection` class today, so the file inherits the app default; if that ever
lands at `NSFileProtectionComplete` through a deployment or MDM choice, reads after
device lock fail and paged-out guest memory becomes unreadable while the app is
still alive. Unverified, reasoned from the mechanism.

### 3.12 Guest-visible surface

The rule: a kernel never reclaims anonymous pages without swap, so `SwapTotal > 0`
if and only if AOK itself moves pages to storage; and in every state the host's own
counters must not leak into the guest.

| surface | today | swap disabled | swap enabled |
|---|---|---|---|
| `SwapTotal`/`SwapFree`/`SwapCached` (fs/proc/root.c:490-491) | 0/0 | 0/0/0 | slots x 16 KiB / free / CACHED |
| `Swapins`/`Swapouts`/`MemShared` (root.c:483, 498-499) | XNU whole-machine counters; `MemShared` prints `usage.free` | removed | removed |
| `MemTotal`/`MemFree`/`MemAvailable` (root.c:478-480) | device RAM / host free list / 32-bit truncated | app budget / `max(0, avail - low)` / same | same |
| `pswpin`/`pgfault`/`pgmajfault` (root.c:531) | host counters, 0, 0 | 0 / minflt sum / 0 | AOK counters |
| `/proc/swaps` (root.c:609) | header only | header only | `/dev/aokswap0 partition <KiB> <used> -2`, and the node exists |
| `sysinfo` totalswap/freeswap, both layouts | 0 | 0 | live, `mem_unit`-scaled on i386 |
| `/proc/<pid>/status` | `VmRSS == VmSize`, `VmLck 0` | unchanged | `VmRSS` = mapped - swapped, `VmSwap`, `VmLck`, `VmHWM` |
| `/proc/<pid>/stat` fields 10-13 (fs/proc/pid.c:279-282) | four hard-coded `0l` | `task->minflt` live | plus `majflt` |
| `smaps` `Swap`/`Locked` | 0 | 0 | per-region counts; `Rss = Size - Swap` |
| `getrusage` `ru_majflt` (kernel/resource.c:276-278, "stays 0 on purpose") | 0 | 0 | `task->majflt`; the recorded rationale is exactly what stops being true |
| `mincore` | host `mincore` | unchanged | 0 for SWAPPED, else the host answer |
| `mlock`/`mlockall` (kernel/mmap.c:1138-1140, "advisory against swap, which iOS manages itself") | range check only | unchanged | real pins, `RLIMIT_MEMLOCK` enforced |
| `swapon`/`swapoff` (kernel/calls.c:1970-1971 `syscall_stub`) | ENOSYS | exist: EPERM without CAP_SYS_ADMIN; `/dev/aokswap0` activates or EBUSY; a regular file EINVAL | `swapoff` pages everything in |
| `/proc/ish/swap` (new) | -- | status | status plus test control |
| `ps` | -- | -- | pid 3 `[kswapd0]` |

`/dev/aokswap0` does not exist today: `dev_standard_nodes` and `dev_dynamic_nodes`
(fs/dev.c:24-56) list null/zero/full/random/urandom/kmsg/tty*/console/ptmx/tty0-7/
rtc0/fuse and clipboard/location/dsp/url, and nothing else. Linux never prints a
`/proc/swaps` path that does not resolve, so the node is part of the contract, not
decoration. `dev_open` already dispatches on `type == DEV_BLOCK ? block_devs :
char_devs` (fs/dev.c:58-59).

`MemTotal := the app's jetsam budget` ships in phase 0, before any pager, because
it turns a confirmed incident class into correct behaviour on its own: OpenJDK 21 in
the Alpine root chose `MaxHeapSize = 6442450944`, a quarter of this Mac's 24 GiB
`MemTotal`, and Node reported `totalmem 25769803776`. With `SwapTotal > 0` beside
`MemTotal = device RAM`, swap becomes the road to the same jetsam.

Honest documented deviations: `SwapFree` moves in 16 KiB steps while `VmSwap`
moves in 4 KiB steps; `SwapCached` counts frames; and XNU may still compress the
app underneath, invisibly, exactly as a hypervisor would.

### 3.13 Settings

**Swap ships OFF and stays off unless the user turns it on.** That is a product
decision taken on 2026-09-03, not a staging step: paging guest memory writes to
the device's flash and consumes container space, so it is not a thing to enable
on someone's behalf. The size is the user's choice too, offered beside the
toggle rather than derived silently from device RAM or free disk. Section 3.12's
"swap disabled" column is therefore the DEFAULT state of the guest surface, not
a fallback, and it has to stay byte-identical to today.

`UserPreferences`: `shouldEnableSwap` (`enable_swap`, **default off**) and
`swapSizeMB` (`swap_size_mb`, no silent default -- the UI asks), with
`registerDefaults` entries, friendly names in the
`/proc/ish/defaults` mapping, and KVO observers into kernel globals following the
`doEnableMulticore` pattern; effective at next launch, scriptable from the guest.
Env for CLI and Xcode runs only: `ISH_GUEST_SWAP_MB`, `ISH_GUEST_MEM_BUDGET_MB`,
`ISH_GUEST_SWAP_WRITE_MB_PER_DAY`, `ISH_GUEST_SWAP_NOCACHE`, alongside the existing
`ISH_GUEST_MEM_HEADROOM_MB`. Docs compiled into the app under `opt/AOK/docs/`, plus
a book section and release notes.

### 3.14 Idle cost

- JIT gadget hot path: untouched. No field of `struct tlb_entry`, `struct tlb` or
  `struct mmu` changes, and `jit/offsets.c` names no `pt_entry`.
- Per TLB miss: one acquire byte load and compare, one byte store, one more
  compare on writes. Zero on a hit. A TLB write hit is 12-20 ns and a
  miss-and-refill is 110-190 ns, so this is under 5% of a miss.
- Per `pt_map`/`pt_unmap` page: an owner-slot compare and a `frame_refs` increment
  under an uncontended stripe, measured at 7.6 ns for the whole hook, which is
  0.6-1.1% of `fork`.
- Memory: `pt_entry` stays 56 bytes; `struct data` gains about 28 bytes plus 64 B
  per MiB of mapping; the slot table is 1 MiB per GiB of swap and only when enabled.
  Page-table leaves already cost 56 B per 4 KiB guest page (1.37% of mapped size)
  and are never freed before `mem_destroy`.
- Swap disabled: no thread, no file, no slot table, `SwapTotal 0`.
- Enabled but idle: a 250 ms timer and one `os_proc_available_memory()` call, or
  one `task_info` at about 2 us on macOS. No walks, no `mem_changed` ticks, no I/O.

### 3.15 Failure modes

1. **The release primitive does not move the ledger on a device.** Fall back to
   `mprotect(PROT_NONE)`, which released 16 KiB byte-exact in every state tested,
   coalesces on restore, and works on the shared and shadowed objects REUSABLE
   skips. If neither moves `avail`, the AOK-paging family is dead as specified.
2. **The release primitive silently does nothing** on a shared or shadowed object.
   Detected by the per-batch ledger check; that batch falls back to `mprotect`.
3. **A missed pointer holder.** Now loud: SIGBUS at the guilty instruction, because
   the released frame is `PROT_NONE`. Previously silent data loss.
4. **Swap I/O error.** `pwrite` EIO leaves the frame resident and the slot BAD;
   `pread` EIO delivers SIGBUS to the faulting process. Disk-full cannot happen
   after preallocation succeeded.
5. **Thrash.** High `pswpin`, everything slow. The thrash guard pauses swap-out and
   the guards refuse growth, because thrash plus a shrinking budget ends in jetsam
   anyway. AOK has no OOM killer and does not invent one.
6. **Barrier storms** on a many-threaded process: bounded by scaling batch size
   rather than batch rate, by the inter-batch interval on background reclaim, and by
   the poke helper's `trylock(&pids_lock)` give-up path. That last one bounds the
   signal flood but makes that particular barrier slower; it is a cost transfer, not
   a bound on barrier cost, and the design should stop citing it as one.
7. **Long read-lock holders** delay eviction; the barrier's 1024-attempt loop then
   blocking `write_lock` tolerates them.
8. **`vm_map` entry growth** while frames are out: 8193 entries for 4096 evicted
   frames on macOS, fully coalescing on restore. The macOS ceiling is at least
   131,072 entries with no failure; the iOS ceiling is **unverified** and is the one
   number that could make the PROT_NONE companion unusable.
9. **Host SIGBUS**: impossible by construction. The guest never maps the swap file,
   and a swapped page is refused by the page table before any host pointer exists.
   The alternative aborts the app, because a host fault outside guest execution is
   untranslatable (jit/jit.c:729-742).
10. **iOS 18's system-wide compressor-space-shortage kill** is invisible to
    `avail`. Swap reduces the app's compressed share but cannot remove the risk.
11. **Suspension mid-write**: one 16 KiB write; no lock on the file.

---

## 4. The measurements that matter

### 4.1 Release and restore primitives (Mac-measured; XNU source is iOS-sourced)

| operation | result | source |
|---|---|---|
| `MADV_FREE_REUSABLE`, one aligned 16 KiB frame | `phys_footprint` -16,384 B, synchronous | reusable_probe, run7 A1 |
| `MADV_FREE_REUSE` alone, no write | +16,384 B | run7 A3 |
| write into a REUSABLE'd frame with no REUSE | +0 now, re-charged later by the pager | run7 A6, run3 |
| `MADV_FREE_REUSABLE` over 512 MiB | footprint -536,870,912 B, internal -536,870,912, reusable +536,870,912 | c2plat4 |
| REUSABLE on a PROT_NONE range | -1, EPERM | c2plat5 section 2 |
| REUSABLE on a post-fork mapping | rc 0, delta 0 | run7 C4-D1, reusable_probe (c) |
| REUSABLE on a dirtied MAP_PRIVATE file mapping | rc 0, delta 0 | reusable_probe (b) |
| REUSABLE with a live `vm_read` copy outstanding | rc 0, delta 0 | probe1 F1 |
| `mprotect(PROT_NONE)` 512 MiB | footprint and resident both -512 MiB, **frees nothing**, all pages read back correct | c2plat4 |
| `mmap(MAP_FIXED, PROT_NONE)` one frame | -16,384 B in every state | run7 B4, c1 probe (e) |
| REUSABLE only, 4096 scattered frames | 2.03 us out, 1.79 us in, 2 `vm_map` entries | c2plat5 section 3 |
| REUSABLE + `mprotect(PROT_NONE)`, same | 4.72 us out, 2.14 us in, 8193 entries out, 2 after restore | c2plat5 section 3 |
| stale read/write of a REUSABLE-only frame | no fault; correct old bytes; write silently reverted | c2plat section B |
| stale read/write of a REUSABLE+PROT_NONE frame | SIGBUS 10/10 both | c2plat5 section 1 |
| `munmap`+`MAP_FIXED` fragmentation | 3 -> 1027 entries for 512 holes, still 1027 after restoring all | c2plat section E |
| `mprotect` fragmentation | 1 -> 512 -> 1 | c2plat4 |
| bulk REUSABLE | 0.117 us/frame | probe1 A4 |
| host free memory after REUSABLE | moves only after ~500 ms (task ledger is synchronous, system relief is not) | probe1 B |

### 4.2 Swap I/O (device-measured on M4 iPad Pro; Mac shown for contrast)

| | device p50 | device p90 | device p99 | device p99.9 | device max |
|---|---:|---:|---:|---:|---:|
| cold random 16 KiB `pread`, idle | 82-86 us | 88-114 us | 110-152 us | 190-961 us | 556-2221 us |
| cold random 4 KiB `pread`, idle | 82-87 us | 96-109 us | 131-141 us | 309-363 us | 684-1571 us |
| same, writer at 217 MiB/s | 132 us | 367 us | 1101 us | 2447 us | 10,726 us |
| same, 1.18 GB/s sustained fsynced writeback | 111 us | 377 us | 416 us | 683 us | 52,978 us |
| hot page-cache read through AOK's guest path | 1.0 us | 2.0 us | 2.0 us | 8 us | 22-35 us |
| **this Mac**, cold random 16 KiB | 106-118 us | 122-191 us | 232-4164 us | 1.1-9.8 ms | 8.4-39.2 ms |

Write side, measured on this Mac: `pwrite` 16 KiB 4.1 us; `pwrite` + release 5.6 us
(256 MiB in 93 ms, 2754 MiB/s); `F_PUNCHHOLE` 6.0 us; `fsync` 0.9 ms per 256 MiB.
Warm restore (`MADV_FREE_REUSE` + `pread`) 2.7 us, 16384 frames in 44.6 ms, 0 of
65536 pages wrong.

Fragmentation, macOS: 60,000 punch-and-rewrite cycles took a 1 GiB file from 2
extents to 55,197 (44,756 after refill) and p99 from 212 us to 3.9 ms.
I/O tier, macOS: p90 119 us at TIER0 versus 2888-2962 us at tiers 1-3.

### 4.3 The barrier (Mac-measured, arm64 Alpine guest; device unmeasured)

| busy siblings | median | mean | p99 |
|---:|---:|---:|---:|
| 0 | 5-9 us | 6.3-12.2 us | -- |
| 1 | 25-33 us | 37.9-409 us | -- |
| 3 | 71-90 us | 75.8-201 us | -- |
| 6 | 148-159 us | 155-239 us | 1.1 ms |
| 12 | 335-519 us | 1.1-1.6 ms | 11.9-16.7 ms |
| 96 | 3.66 ms | 7.57 ms | 60.5 ms |

Extra victim CPU per thread per barrier: 111.6 us (77 us TLB refill, 31 us quiesce
machinery), against 34.6 us for the same barrier without `mem_changed`.
Batch-size marginal cost: 1 page 4.8 us versus 256 pages 29.3 us with no siblings.
Duty at 1000 barriers/s with 6 threads: 24.5% / 30.3% / 47.4%.
Best duty ever observed at 954 barriers/s: 12.2%, at 60.5% host idle.
Barrier fall-through floor: the 768 `nanosleep(2 us)` phase costs 5.6-48.9 ms,
because timer coalescing makes a 2 us sleep cost 4-6 us on XNU.

### 4.4 Page-table walk and footprint (Mac-measured)

| measurement | value |
|---|---|
| clock-hand walk, marginal per entry | 4.07 ns |
| 1 GiB touched: base / mapped / after unmap | 104 us / 1371 us / 1171 us permanently |
| 4 GiB space, 512 KiB touched | 4.9-11.6 ms per pass, 4.0-7.1 ms after unmapping all |
| per allocated pgdir chunk | ~38 us |
| clock RMW vs read-only walk | +10%, +26% under a concurrent stamper |
| COW-broken guest page host residency | 16.3 KiB per 4 KiB guest page (4.00x) |
| child rewriting all 256 MiB of a 256 MiB parent | emulator footprint 1300 MB, 9498 VM_ALLOCATE regions |
| COW break cost today | 7.14 us per page |
| owner hook (lock + compare + two increments + unlock) | 7.6 ns |
| guest `fork+_exit+waitpid`, marginal | 1.35 us/page at 16 MiB, 2.65 us/page at 64 MiB |
| `struct data` frees per lockless walk, one churning thread | 1.24-1.96 (267-457/s) |
| `mmap(NULL,...)` reusing a just-`munmap`'d VA | 2000/2000 |
| `sizeof(struct pt_entry)` / leaf / chunk / pgdir_root | 56 / 57,344 / 66,560 / 32,768 B |

---

## 5. Verified claims

| # | Claim | Verdict | What survives |
|---|---|---|---|
| C1 | `MADV_FREE_REUSABLE` raises `avail`; else `munmap`+`MAP_FIXED` | Contested (1/3 refuted) | The mechanism, source-verified with no platform gate and measured byte-exact on the same arm64 pmap code the device runs. Not the unconditional form: the primitive is a silent no-op on shared or shadowed objects, and the named observable (`os_proc_available_memory()`) has never been seen to move anywhere, because macOS returns 0 |
| C2 | Every host pointer minted under `mem->lock` and dropped before release, except `futex_load` | **Does not stand** (3/3) | Nothing in `fs/` can construct a pointer into guest memory, so the host kernel is never handed a guest address in a blocking syscall; the Mach exception server thread reads no guest memory; JIT blocks hold no host pointers. The universal quantifier is false at five to seven sites, the statement is instantaneous not durational, and it needs exclusive ownership. On Apple a violation is completely silent |
| C3 | The five refcount sites are complete | **Stands** (0/3) | Exhaustively true, re-derived three times independently, with the back doors (struct copies, `mm_copy`, page-limit shrink, `mem_destroy` reach) each closed. The `owners[2]` encoding needs an overflow counter to be exact, and to be safe |
| C4 | Chunks and leaves immortal + `mm_retain`, so a lockless clock walk is safe | **Does not stand** (3/3) | Both premises true. A lockless walk may read leaf-resident fields, which is what `mem_mapped_page_count` does today; it may not dereference `entry->data`. On Darwin the failure is a silent read of a recycled VA, not a crash |
| C5 | The first store after a fill takes the write-miss path, so the dirty bit is complete | Contested (1/3 refuted) | The funnel, confirmed three ways including 32 guest-visible operations across three ABIs. The predicate is `type != MEM_READ`; there is one `P_SHARED`-gated bypass; the stake is corruption, not wear |
| C6 | Barrier tens of us, so 64-frame batches give 1 GB/s at under 10% stall | **Does not stand** (3/3) | The proxy is exact and the barrier is cheap at low thread counts. It is scheduler-bound, its cost scales with busy siblings, the victim's own CPU bill was never measured, and 1 GB/s contradicts the design's own rate limit |
| C7 | A cold 16 KiB `pread` on device NAND is tens to low hundreds of us | Contested (1/3 refuted), net stronger | Now device-measured at 82-86 us p50 and 110-152 us p99, better than the Mac. "Tens of microseconds" never happens; the floor is 67-70 us. Three new operating constraints |

---

## 6. Rejected alternatives

- **Design A's balancer as the mechanism** (migrate whole `struct data`s into a
  `MAP_SHARED` pool that XNU pages). It was the only design exercised end to end in
  the real emulator, and its ledger claim is the best established. It loses on three
  measured facts: XNU's syncer rewrites every dirty pool page within about 30 s with
  no pressure at all (`designA/probe-idle.txt`: 0 MiB of blocks at t=25 s, 248 MiB at
  t=30 s), and the write budget can only stop migrate-in; it cannot observe its own
  refaults, so no `majflt`, no `pswpin`, and no thrash guard is possible; and there
  is no AOK-side fallback if dirty external pages turn out to be charged on iOS.
  Kept from it: the packer, chunked `F_PREALLOCATE`, coalesced async punch, exit
  truncate, the memory-pressure source, the device probe and the interposer rig.
- **Pure Design A** (all anonymous memory file-backed from birth). A itself measured
  and rejected this on wear: 1085 MB written versus 26 MB for five gcc compiles.
- **Design C's pager choices**: a barrier per swap-in (a process-wide stall per
  major fault, unnecessary because no TLB entry can exist for a swapped page); swap
  state in `flags` (it propagates through `MADV_DONTNEED`, `P_WIPEONFORK` and both
  COW copies); `F_NOCACHE` by default (throws away a free swap cache, and, as the
  verification showed, does not even produce a cold read); the swap file in the
  root's `data/` under the App Group container (File Provider exposure, backup,
  host truncation to SIGBUS, cleartext guest memory persisted); kswapd as a borrowed
  `init`; a staging pool sized against the wrong batch. Kept: the guest contract,
  the PROT_NONE reservation rule, the synthetic kthread, the test registration
  checklist.
- **`data->data == NULL` as the swapped state.** It is per-data and already means
  PROT_NONE-unbacked (emu/memory.c:1480-1481), and `mem_ptr_fault` has no recovery
  branch for it.
- **Per-4 KiB eviction.** The host cannot release, replace or protect a single
  4 KiB guest page: `munmap`/`mprotect` of 4 KiB is EINVAL, `madvise` rounds outward,
  and a 4 KiB `mmap` costs 16 KiB.
- **`mprotect` plus fault-and-repair.** Host page protections cannot be mirrored on
  Apple (16 KiB versus 4 KiB, and a hard `#if __APPLE__` disable at
  emu/memory.c:187-189), and a host fault outside guest execution aborts the app.
  Eviction must be purely exclusion-based, which is precisely why the PROT_NONE
  companion is a guard rail and not a mechanism.
- **`MADV_PAGEOUT`.** ENOTSUP for userland (errno 45).
- **Relying on XNU's compressor or purgeable-volatile memory.**
  `internal_compressed` is inside `phys_footprint` (658 MiB compressed, footprint
  unchanged); purgeable-volatile memory is purged by the kernel at will.
- **Advertising XNU's paging as guest swap.** No honest `SwapFree` or used figure
  can be computed; the only defensible values are 0 and 0.
- **Guest-file `swapon` in v1.** Separable, and its exposure (File Provider, backup,
  `/etc/fstab` boot changes, SIGBUS on host truncation) is not worth carrying before
  the pager is proven. Deferred to an optional later phase.
- **One host file per page, or per-page fakefs operations.** fakefs metadata is a
  saturating SQLite lock; a single preallocated file is the only viable shape.
- **[new] A lockless clock hand.** Rejected by C4; the read-lock chunked walk costs a
  fraction of a percent of duty and removes an entire class of use-after-free.
- **[new] `mmap(MAP_FIXED, PROT_NONE)` as the PROT_NONE companion.** It fragments
  the `vm_map` permanently (3 to 1027 entries, still 1027 after restoring every
  hole), while `mprotect` coalesces fully. Keep `MAP_FIXED` only as the last-resort
  fallback if `mprotect` turns out not to release on a device.

---

## 7. Implementation plan

Effort assumes one engineer who knows the memory core.

### Day 1: the device probe -- **RUN 2026-09-03, PASSED**

Built as `/proc/ish/mem_release_probe` (commit `65bda10bf`, the engine in
`platform/darwin.c`) and run on the M4 iPad against iSH-AOK 553. **The decisive
platform assumption of this whole document is no longer unverified.**

| case | device result |
|---|---|
| `MADV_FREE_REUSABLE`, 4096 frames | footprint -63,537,152 -- MOVED |
| `mmap(MAP_FIXED)` fallback | -67,141,680 -- MOVED |
| `REUSABLE` + `mprotect(PROT_NONE)`, 2048 alternating | **-33,554,432 byte-exact** -- MOVED, and the drop held |
| dirtied `MAP_PRIVATE` file control | no-op until `mprotect`, as designed |
| `vm_map` entries | 2805 -> 6900 (+4095) -> 2805, fully coalesced on restore |
| evict / restore cost | 2.53 us / 0.93 us per frame |
| app's real jetsam ceiling | 6,442,450,944 bytes, floor 201,326,592 |

Three of section 8's open risks close on that run. **Risk 1** (does the ledger
move on a device) is answered yes for both primitives. **Risk 3** (the unknown
iOS `vm_map` entry ceiling, which could have made the mandatory `PROT_NONE`
companion unusable) was not hit at 4095 extra entries and they were all
recovered. And `os_proc_available_memory()` was observed to move, which section
4.1 recorded as never having been seen anywhere, so **risk 2**'s first half is
answered too.

One caveat that does not change the verdict: the probe reported a debugger
attached. That state can only cause FALSE NEGATIVES -- a shadowed VM object
makes `REUSABLE` a silent no-op -- so it cannot manufacture a MOVED. It likely
explains why case (a) released 94.7% of its region while the `mprotect` case was
byte-exact. Re-run detached for a clean figure.

What the probe still does not answer is listed under "what it cannot answer" in
its own output: the two-minute `st_blocks` watch, a forced jetsam to read the
reason strings, and `limit_bytes_remaining` while backgrounded.

The original specification of this probe follows, for the record.

An env-gated change of about 60 lines plus a guest program. Touch 2 GiB; print
`os_proc_available_memory()` and `phys_footprint` from a debug `/proc/ish` file;
then measure each of:

(a) `MADV_FREE_REUSABLE` on every 16 KiB frame;
(b) the same followed by `mprotect(PROT_NONE)`;
(c) `mprotect(PROT_NONE)` alone;
(d) `munmap` + `mmap(MAP_FIXED, PROT_NONE)`;
(e) **[new]** the same four on a dirtied `MAP_PRIVATE` **file** mapping, so a
    zero-return-with-zero-delta is recognised as the shadow branch rather than as
    "iOS ignores REUSABLE";
(f) the `vm_map` entry count while 4096 frames are out and after restoring them;
(g) `st_blocks` of an unlinked file for two minutes with no pressure;
(h) the jetsam reason strings seen after a deliberate over-commit, and
    `limit_bytes_remaining` read once while backgrounded.

**[new] Run it with no debugger and no Instruments attached**, because a live
`mach_vm_read` copy of the region makes REUSABLE a silent no-op, and key the
results on `task_vm_info.phys_footprint`, not on `avail`, which clamps at 0 both
below and above the limit. Size the touch so the process stays under its limit.

Kill criterion: none of (a), (b), (c), (d) moves `phys_footprint`. Secondary kill:
the `vm_map` entry count in (f) hits a device ceiling.

### Phase 0: ships alone, whether or not swap is ever built (2-3 weeks)

- The singleton packer (`pt_map_into`, a per-mem 16 KiB slab replacing the four
  `mmap(PAGE_SIZE)` sites at emu/memory.c:1586, :1672, :1520, :1630, :1379-1381),
  which recovers three quarters of the measured 4x COW footprint amplification.
- **[new]** `futex_load`'s post-unlock dereference (kernel/futex.c:206-211).
- **[new]** The `kernel/user.c` multi-pointer walks (:280-283, :351, :400): re-mint
  every earlier span pointer after each `mem_ptr`, or restart the span when
  `mmu.changes` moved.
- **[new]** `dump_addr_backing` (kernel/calls.c:5670): take
  `mem_read_lock_quiesce_aware` as the neighbouring helpers already do.
- **[new]** `mm_copy` (kernel/mmap.c:178): propagate `pt_copy_on_write`'s return so
  `fork()` yields ENOMEM instead of a truncated child.
- **[new]** `pt_map`'s OOM rollback (emu/memory.c:1058-1063): `munmap` the host
  range and free `host_page_prot`, and unwind the pages already published.
- `MemAvailable`'s 32-bit truncation; drop the host counter leaks
  (fs/proc/root.c:483, :498-499, :531); `stat`'s four hard-coded fault fields
  (fs/proc/pid.c:279-282) and the `mem_ptr_fault` lazy-path minflt gap.
- `MemTotal := budget` behind its own knob with the `avail == 0` split.
- `ISH_GUEST_MEM_BUDGET_MB` so the headroom guard fires on this Mac.
- The memory-pressure dispatch source and memory-warning observer writing
  breadcrumbs.
- Hoist the `brk` direct-reclaim point above the barrier.
- **[new]** A comment at kernel/task.c:1947 recording that the QoS choice also
  selects the disk throttle tier.
- Four-arch gate on the Mac. Regression guard: with the packer off, today's
  `fp-*.txt` figures must reproduce exactly.

### Phase 1 gate -- **RUN 2026-09-03 on the M4 iPad, PASSED**

Built as `/proc/ish/swap_evict` (commit `6d8a4268d`) and measured on device
against iSH-AOK 553, 64 MiB region, guest frozen so nothing of its own could
fault a page back:

| | device |
|---|---|
| frames evicted / faulted back | 8214 / 8198 |
| ledger drop | -67,469,312, and it HOLDS after the barrier releases |
| `task_vm_info.reusable` | 81 MB |
| corrupted pages, 3 reader threads | **0** |
| `madvise` / `mprotect` failures | 0 / 0 |

**The device settled the one thing macOS could not.** On this Mac the pair
`MADV_FREE_REUSABLE` + `mprotect(PROT_NONE)` drops `phys_footprint` while
leaving `reusable` at 0, which cannot distinguish a real release from merely
hiding the pages from the ledger -- and on macOS that distinction has no
consequence, so it cannot be resolved there at all. On iOS the same pair
populates `reusable` AND drops the ledger. The pages are genuinely handed
back with the PROT_NONE guard rail still in place, so the memory saving and
the stale-pointer protection are not in tension. Since iOS derives remaining
headroom as limit minus footprint, that drop is 67 MB of real jetsam headroom.

Diagnostic knobs `ISH_SWAP_NO_MADVISE` and `ISH_SWAP_NO_MPROTECT` (host
environment, so the Xcode scheme on a device) separate the two calls if this
ever needs re-establishing.

Hypotheses eliminated on the way to that, recorded so nobody repeats them:
`MAP_NORESERVE`, the `fd=0` anonymous mmap, guest faults undoing the release,
and an uninitialised `task_info` struct with an unchecked reply count. That
last was a real bug in the measurement helper, fixed, and not the cause.

The original specification of this gate follows, for the record.

### Phase 1 gate: the CLI prototype (1 week, scratch branch)

`pt_swap` bytes; the `mem_ptr_nofault` test; a `swap_evict_frame`/`swap_fault`
pair for exclusive anonymous frames; prologues in `mem_ptr` and `mem_ptr_fault`;
the exported barrier with the poke fix; a `/proc/ish/swap` control that evicts every
eligible frame of a pid; **[new]** the REUSABLE-then-`mprotect(PROT_NONE)` release
and its inverse, on by default; **[new]** the per-batch ledger check; **[new]** the
chunked read-lock clock hand.

Guest test on arm64 and amd64: fill 512 MiB with a position-dependent pattern,
four threads reading random pages while the host script evicts repeatedly and
samples footprint, verify the checksum and `ru_majflt > 0`.

Pass: footprint drops at least 90% of evicted bytes within 100 ms; zero corruption
over 100 cycles; warm fault p99 under 200 us; barrier p99 under 1 ms with 8 threads.
**[amended]** Add a cold bar and name the host: cold swap-in p99 under 500 us and
p99.9 under 2 ms **evaluated on device**, with the Mac informational, because the
original "p99 under 1 ms on NAND" gate names no host and would fail on this Mac in
roughly half of the measured passes while passing comfortably on the device.

Kill criteria: footprint does not drop with either primitive; barrier p99 above
1 ms at 8 threads after batch-size scaling.

### Phase 1: pager core (4-5 weeks, about 2,000 LOC)

`kernel/swap.c`/`.h`: file lifecycle with chunked preallocation, slot table,
bitmap, condvars, `swap_evict_frames`, `swap_fault`, `kswapd` with the chunked
read-lock clock, watermarks, byte-bounded direct reclaim, pressure kicks, write
budget, thrash guard, counters, `/proc/ish/swap`. Owner tracking with the overflow
counter and `frame_refs` at the five refcount sites. The `mem_ptr`/`mem_ptr_fault`
prologues with full re-walks. Deferred `struct data` free. `mem_resident_page_count`.
The exported `_nodrain` barrier and the pthread-identity poke skip. The macOS budget
knob. Batch size scaled to thread count. Exclusive-only eviction, CLI-testable end
to end under `ISH_GUEST_MEM_BUDGET_MB=768` against a 1 GiB workload.

**[new]** A CI check that maintains the pointer inventory: a grep allow-list over
`mem_ptr`, `mem_ptr_nofault`, `__tlb_*_ptr`, `tlb_write_ptr_slow`, `data_minus_addr`
and `->data->data` outside `emu/` and `jit/`, plus a full-suite soak with the
PROT_NONE release forced on. The inventory went stale within five commits during
this study, when `emu/tlb.c` gained four new direct-pointer minting sites; a document
cannot hold this invariant.

### Phase 1 as built (2026-09-04)

Landed on `working`, each piece gated on all five guest roots and reviewed
adversarially. Commits in order: `69558a762`, `d26478f7e`, `57e760c9e`,
`cf49fdac3`, `66b074d89`, `4495bcc1f`, `613742a9a`, `0cf326b8d`, `434b7bbec`.

**Done.** Exact ownership records and per-frame entry counts on `struct data`;
the frame-owned slot (section 2.8) replacing the per-entry copy; the slot
allocator, file lifecycle and swapon/swapoff over one preallocated unlinked
file; `swap_enabled()` off by default with the app's Settings switch and size
wired through `swap_set_preference`/`swap_startup`; direct reclaim at all three
growth guards; the second-chance aging clock with its TLB refresh;
`mem_resident_page_count`; the suspension gate; the 24-hour write budget;
`/proc/ish/swap`; and the system-wide and per-process guest surfaces
(`meminfo`, `vmstat`, `sysinfo(2)`, `status`, `statm`, `stat`, `smaps`).

`kswapd` and the thrash guard landed too (`2eedfeb9b`), which took the barrier's
task-less-caller support with them -- `task_poke_shared_mem` treated a NULL
`task` as "do nothing" rather than "no self to skip", so a task-less barrier
poked nobody and fell through to a blocking `write_lock`.

**Still open from this phase.** The low-latency swap-in that drops the lock
across the `pread` (section 3.5's drop-run-retake-rewalk, still a stall of 82 us
p50 / 683 us p99.9 on device); and the CI pointer-inventory check.

**Corrections this phase forced on the design.** Section 2.8 (the slot belongs
to the frame) is the largest. Beyond it: the aging clock's `accessed` byte is
only written on a TLB FILL, so the sweep must bump `mem_changed` every pass or
it is blind to the hottest pages in the process -- 506 of 1536 avoidable faults
came from exactly that; direct reclaim must be allowed to lower the age bar,
because the clock needs two passes before anything is a candidate and a caller
that gives up on one `ENOMEM` never gets a second; and the thrash guard's pause
must EXPIRE rather than wait to be cleared by evidence, because while it holds
nothing is evicted and so there is no evidence to be had -- it latched for the
life of the process until a cooldown was added.

### Phase 2: surfaces, contract, app (2-3 weeks, about 700 LOC)

Every row of the surface table: meminfo, vmstat, `/proc/swaps`, `sysinfo` in both
layouts, status, statm, stat, smaps, smaps_rollup, `ru_majflt`, `mincore`, real
`mlock` with `RLIMIT_MEMLOCK`; the `/dev/aokswap0` block node; `swapon`/`swapoff` on
arm64 224/225 (shared by riscv64), removal from the asm-generic native ENOSYS list,
new i386 87/115 and amd64 167/168 slots; SIGBUS on `-EIO`; `[kswapd0]`; preferences,
friendly names and KVO; the suspend gate; the settings row; pbxproj and meson; docs.
`swapon` errno tests as an unprivileged user, since the CLI runs as uid 0. Surface
conformance against a real Linux box with `free`, `vmstat 1`, `top`, `htop`, procps
and busybox variants, `swapon --show`, `time -v`, and ktop's swap meter.

### Phase 3: device validation (2 weeks)

The four-arch suite on Mac and device per the release checklist; a 24-hour soak with
a low write cap while watching Xcode Organizer's disk-writes metric;
background/foreground cycles during eviction; a jetsam-report audit after a
deliberate over-commit; `make -j` of a mid-size project and a JVM with a 3 GiB heap
on the 16 GB iPad and on a small-RAM phone. Measure cold swap-in latency, barrier
cost at realistic thread counts, `vm_map` entry ceilings, and the entitled limit at
launch. **[new]** Measure swap-file extent count after the soak.
**Do NOT flip `enable_swap` on by default** -- an earlier draft of this plan said
to if the numbers held, and that is superseded by the decision recorded in 3.13.
What phase 3 decides is whether the feature is fit to OFFER, not whether to
enable it for people.

### Phase 4: multi-owner eviction (2 weeks)

Ordered barriers on every owner; `MAP_SHARED` anonymous becomes evictable, which
requires revisiting the `MADV_REMOVE` bypass; shm and file-backed stay excluded. The
least-tested lock pattern in the tree; ships behind the four-arch gate with a Python
fork-pool stress.

### Phase 5: gated extras (1-2 weeks each, optional)

(a) Background trim at the suspend gate, only if phase 3 shows footprint matters in
the idle band. (b) Design C's root-file `swapon` layer, only if users ask. (c) Zero
frame detection or a compressed tier, as a second design.

**Total: 13-18 engineer-weeks** to a shippable, documented, default-on feature
(phases 0 through 3), plus 2 for phase 4. The riskiest day is day 1 on the device;
the riskiest week is the first of phase 1.

---

## 8. Open risks

1. **The device ledger.** Whether any of the four release primitives moves
   `phys_footprint` inside a real iOS app has never been observed. The XNU path has
   no platform gate and the arm64 pmap debit is source-verified, but the observable
   itself is unmeasured everywhere. Day 1 decides it.
2. **Whether `os_proc_available_memory()` is non-zero in the portal-signed shipping
   build.** The 539 notes record that the increased-memory-limit entitlement's effect
   was never independently verified. If it is always 0 the trigger must fall back to
   `phys_footprint` against a configured budget, which is what the macOS path already
   does.
3. **The iOS `vm_map` entry ceiling.** macOS took 131,072 PROT_NONE holes with no
   failure. The device number is unknown and is the one thing that could make the
   PROT_NONE companion unusable in production, which would put the design back on a
   release primitive whose violations are silent.
4. **Barrier cost on older hardware and on 100-thread guests.** Measured to 96
   threads on a 10-core Mac (3.7 ms median, 60 ms p99); unmeasured on any device, and
   a device has 2 to 6 cores that the app shares with the system, and eviction runs
   precisely when the system is under pressure.
5. **The `mem_ptr(MEM_READ)` contract change.** Its audit is enforced by running the
   suite with the PROT_NONE release, not by inspection. A production miss is now a
   SIGBUS rather than a silent lost store, which is the right failure, but the audit
   still has to be complete enough that the SIGBUS never fires in a user's hands.
6. **The pin/unpin pairing and the slot state machine** on every early return of
   `swap_fault` and the batch loop. Designed, not prototyped.
7. **`swap_lock` is one global mutex.** Under thrash every swap-in and every
   `pt_unmap` of a swapped page contends on it. Acceptable for v1, but the C6
   measurements say stripe it before any batch rate above about 100/s.
8. **Apple's disk-writes exception threshold** is folklore (1 GB/day is the quoted
   figure) and the 4 GiB/day default cap may be wrong in either direction. The
   24-hour soak measures it.
9. **iOS 18's system-wide compressor-space-shortage kill** is invisible to `avail`.
10. **Whether RunningBoard assigns this app a distinct inactive limit** (XNU's
    default is the same limit as foreground) and whether footprint orders kills
    within the idle band. Decides whether the background trim is worth its writes.
11. **Fork-without-exec families** keep their shared image resident until phase 4.
12. **`RLIMIT_MEMLOCK` enforcement with real `mlock`** is a behaviour change for
    unprivileged guests that silently over-lock today.
13. **Swap-file data protection class.** Not set anywhere in the tree today, so the
    file inherits the app default. Unverified.
14. **The 16 KiB unit** means one hot 4 KiB guest page pins three cold neighbours,
    and packing does not change that.
15. **Dirty `MAP_PRIVATE` file pages** are a per-process footprint class the design
    can never release with REUSABLE, only with `mprotect`. Every relocated `.data`,
    `.got` and RELRO page of every ELF and library is in it.

---

## 9. Defects found along the way

These are real problems in today's tree, found while verifying the design. Each is
independent of whether swap is ever built. Line numbers are at `e12aeae36`.

**High**

1. **`futex_load` dereferences after the unlock.** kernel/futex.c:206-211:
   `mem_ptr` at :207, `mem_read_unlock_quiesce_aware` at :208, `*out = *ptr` at :211.
   A sibling's `munmap` frees the backing at emu/memory.c:1180-1197 in that window,
   so this is a use-after-free today, feeding the `FUTEX_WAIT` value check (a lost
   wakeup, i.e. a hang with no crash report). The comment at :195-205 justifies the
   lock choice, not the placement of the dereference. Fix: move `*out = *ptr;` above
   the unlock. `futex_wake_op` at :741 and the variant at :799 call the same helper.

2. **`kernel/user.c`'s multi-pointer walks hold a stale pointer across a lock
   drop.** `user_transform_two` resolves the write pointer at kernel/user.c:280,
   then calls `mem_ptr(mem, ip, MEM_READ)` at :281, which can drop the read lock on
   its lazy, growsdown or COW upgrade (emu/memory.c:1494, :1515, :1566, via
   `read_to_write_lock`, util/rw_locks.h:238-244). A sibling `munmap` can take the
   barrier in that window and free the backing, so the `fn(in_host, out_host, ...)`
   at :283 writes into unmapped or reused host memory. `user_transform_rect_two`
   (:351) and `user_transform_rect_three` (:400) have the same shape. Reachable
   through the accelerator ioctls. `tests/manual/pread_stack_thread_race.c` already
   documents this crash shape and suspects the upgrade.

3. **`mm_copy` discards `pt_copy_on_write`'s return value.** kernel/mmap.c:178.
   `pt_copy_on_write` returns -1 and breaks out of its loop when `mem_pt_new` fails
   (emu/memory.c:1445-1449), i.e. when a page-table leaf or chunk `calloc` fails.
   `mm_copy` returns `new_mm` regardless and kernel/fork.c only checks for NULL, so
   `fork()` reports success with a child whose page table holds only a prefix of the
   parent's address space. On i386 the stack is at the top of the address space, so
   the dropped pages are essentially always stack and high libraries and the child
   SIGSEGVs on its first instruction with no diagnostic. Directly relevant here:
   `mem_pt_new` failing **is** the out-of-memory state swap exists to survive, and
   AOK's current response to it is a corrupted child rather than `fork() -> ENOMEM`.

4. **`dump_addr_backing` is a lockless `struct data` use-after-free on a
   guest-reachable path.** kernel/calls.c:5670, called at :5188 on every unresolved
   arm64 page fault with no mem lock held; it reads `pt->data->name`, `->fd` and
   `->file_offset` and calls `generic_getpath` on the fd. A concurrent sibling
   `munmap` frees that `struct data` at emu/memory.c:1197. Fix: take
   `mem_read_lock_quiesce_aware` as the neighbouring helpers at :5467-5474 do.

5. **amd64 `lock cmpxchg` with a failing compare does not request write
   permission; i386 does.** Measured on both roots with the same source: i386
   `lock cmpxchgl` with a failing compare breaks COW (minflt delta 1) and SIGSEGVs on
   a `PROT_READ` page; amd64 does neither, while a plain store on the same amd64 page
   does SIGSEGV. The instruction is genuinely LOCK-prefixed in the guest binary
   (`f0 / 48 0f b1 31`). x86 `CMPXCHG` writes its destination unconditionally, so a
   locked `CMPXCHG` against a read-only page must fault; AOK's own i386
   implementation is the in-tree oracle and agrees. So the amd64 path is not reaching
   `x86_atomic_cas` (emu/tlb.c:490), which calls `tlb_write_ptr_slow` before the
   compare. Guest-visible: a program probing page writability with `lock cmpxchg`
   sees a read-only page as writable. **The atomicity consequence is unverified** (a
   CAS retry loop is self-correcting, so a four-thread increment test proves
   nothing), but the permission divergence is verified. Repro under
   `build/alpine-i386-test` versus `build/alpine-amd64-test`.

**Medium**

6. **`pt_map`'s allocation-failure rollback leaks the host mapping.**
   emu/memory.c:1058-1063 drops `refcount` to 0 and calls `free(data)` without
   `munmap(data->data, data->size)` and without `free(data->host_page_prot)`, which is
   `malloc`'d at :1036. Compare the correct teardown at :1180-1197. Because no entry
   points at that data any more, `pt_unmap`'s `munmap` can never run, so the host
   mapping leaks permanently. A failure on a later page instead returns `_ENOMEM`
   with the earlier pages left published and no unwind, so the guest gets ENOMEM plus
   a range it can neither see nor free.

7. **`mem_coredump` writes the wrong page.** emu/memory.c:1789:
   `write(fd, entry->data->data, PAGE_SIZE)` omits `+ entry->offset`, and `pt_map`
   sets a per-page offset at :1066, so every page of a multi-page mapping is dumped as
   that mapping's **first** page: a 1 MiB mapping writes page 0 into 256 different
   file offsets. It also walks and reads the page table with no `mem->lock` held.
   Dead today (no caller, no header declaration), which is exactly why it will
   mislead whoever revives it, and a swap implementer is the likeliest person to.
   Compare :1587 and :1674, which get the offset right.

8. **The `/proc` page-count walk's cost is set by a process's historical peak and
   never recovers.** Because leaves are immortal, `mem_mapped_page_count`'s cost is
   O(high-water populated 4 MiB granules). Measured: a 4 GiB address space with
   512 KiB touched costs 4.9-11.6 ms per pass, and after unmapping all of it, 4.0-7.1
   ms per pass forever at 236 kB resident. It also ignores the `pgdir_root_bitmap` and
   `leaf_bitmap` it maintains, which `mem_next_allocated_leaf_base` (emu/memory.c:301-330)
   exists to use, costing about 38 us per allocated pgdir chunk. This is guest-visible
   today: `/proc/<pid>/stat`, `statm`, `status` and `ru_maxrss` all pay it, so
   `top`/`htop`/`ps` refresh latency grows with a process's peak and never comes back.

9. **`tlb->dirty_page` is a dead store on the hottest path in the emulator.**
   Written unconditionally by every guest store (emu/tlb.c:709, emu/tlb.h:170, and the
   write prep in all four gadget sets plus the hand-rolled SIMD path), while its only
   readers are `tools/ptraceomatic.c` and `tools/unicornomatic.c`, neither of which is
   referenced by `meson.build`, the Xcode project, or CI. Every shipping build pays one
   wasted store per guest store.

10. **`emu/memory.h:243` documents `void *data; // immutable`, and it is not.**
    `mem_materialize_shared_data` writes it at emu/memory.c:1345 under a private
    static lock and no mem lock, while the struct is reachable from several mems. The
    transition is one-way NULL to non-NULL so existing readers are safe, but the
    synthesis leans on the comment as a guarantee.

**Low, or process**

11. **`kernel/mmap.c`'s comment "Once quiesce_requested is up, no new reader can
    enter" is not true of every reader.** `trylockw` (util/rw_locks.h:270-283) never
    sets `writers_waiting`, so during the barrier's whole spin the writer is invisible
    to the reader predicates, and four read sites do not consult `quiesce_requested`
    (kernel/calls.c:5229, :5468, :5595 and jit/jit.c:891's `trylockr` on the `#GP`
    retry path). No livelock, because those paths are short, but the comment should
    not be read as a guarantee.

12. **The pointer-holder inventory cannot be maintained by a document.**
    `emu/tlb.c` gained four new direct-pointer minting sites
    (`x86_atomic_rmw`/`_cas`/`_cas16b`/`_xchg` via `tlb_write_ptr_slow`) in the five
    commits between the tree two verification lenses read and the tree the third read.
    They happen to be safe. A whole-tree universal quantifier that four new sites can
    invalidate in a week needs a CI check.

13. **`F_NOCACHE` has been used throughout the swap experiments as though it
    produced a cold read, and it does not.** Identical file, identical offsets:
    cached path p50 2.29 us, `F_NOCACHE` p50 2.42 us. It is a retention hint, not
    `O_DIRECT`; pages already in the UBC are still served from it. This corrupted at
    least two numbers in the design documents, including one labelled "pread 16K
    F_NOCACHE (uncached) mean=1.2us", which is 13 GB/s and physically impossible from
    NAND. Future swap benchmarks must make files cold with `msync(MS_INVALIDATE)` on a
    `MAP_SHARED` mapping or with a working set exceeding RAM, and must report
    distributions rather than means.

14. **`kernel/mmap.c:1138-1140` justifies `mlock` being a no-op with "a lock is
    advisory against swap, which iOS manages itself".** Once AOK itself pages guest
    memory, `mlock` becomes a promise AOK breaks. The comment is a promissory note the
    design has to redeem, and it is on the phase 2 list.

15. **Two unchased observations.** On the amd64 guest, `prefetchw` and/or `clflush`
    appear to kill the process (probably SIGILL on an unimplemented opcode);
    `prefetchw` is what GCC emits for `__builtin_prefetch(p, 1)` and `-march=znver*`.
    Which of the two was not isolated, so this is **unverified**. And
    `ISH_QUIESCE_STATS` is read in `main.c` `cli_halt` but produced no output for
    `./build/ish -f <root> /bin/sh -c ...`, so the CLI's `-c` path may not reach the
    halt hook.

---

## 10. What this changes about the pitch

The honest one-sentence description of the feature, after verification, is:

> iSH-AOK can page cold guest anonymous memory out to storage at about 100 to
> 200 MiB per second per process and fault it back in about 85 microseconds, so a
> guest can hold considerably more memory than iOS will let the app keep resident,
> as long as most of it is cold.

Not: "the guest can use more memory than the app has". A working set that exceeds
the budget will thrash, and AOK will say so through honest `pswpin`/`pswpout`
counters, which is more than it can do today. That is a real capability and it is
worth building. It is a smaller capability than the study's headline throughput
number implied, and the difference is exactly the gap between a barrier measured
from the caller's side on an idle machine and a barrier measured from the victim's
side under load.

---

## 11. Phase 0 as built, and what was deferred

Recorded 2026-09-03, after building Phase 0 against this plan.

**Landed.** The parts that stand on their own and do not change what MemTotal
means: the `MemAvailable` truncation fix and the Darwin speculative
double-count, the Linux `get_mem_usage` fixes (five of nine fields were
uninitialised, so guests were reading host stack garbage, and the kB figures
were being stored where callers read bytes), the removal of the host-wide
counter leaks, `ISH_GUEST_MEM_BUDGET_MB`, and the per-task resource-counter
reset on fork.

**Deferred: `MemTotal := the app budget`.** Four review rounds rejected it, and
each round found the same class of problem one surface further out. The change
is not the size this plan assumed.

The reason is structural rather than a matter of care. AOK builds page-table
entries EAGERLY at `mmap()` time, for file mappings in `host_fd_mmap` and for
anonymous mappings below the 64 MiB lazy threshold in `pt_map_nothing`, and no
`pt_entry` field records whether a page was ever touched. So every "resident"
figure AOK reports is really a count of mapped address space. While MemTotal is
the machine's RAM that over-reporting is invisible, because the machine is
large. The moment MemTotal becomes the app's budget, every one of those figures
can exceed it, and each is a state Linux cannot produce:

- `/proc/meminfo` AnonPages, Mapped and Shmem, which count every entry.
- `/proc/<pid>/status` VmRSS and VmHWM, statm field 2, stat field 24. Measured:
  an untouched 1 GiB file mapping took VmRSS from 2124 kB to 1050700 kB.
- `smaps` and `smaps_rollup` Rss, which Linux derives from the same counter as
  VmRSS and which therefore must agree with it.
- `getrusage`'s `ru_maxrss`, bounded in kernel/resource.c against the HOST
  machine's RAM.

Bounding each one against MemTotal is what the rejected rounds tried. It
removes the impossible value and replaces it with an inconsistent pair, because
two surfaces bounded independently disagree with each other.

**The prerequisite is `mem_resident_page_count`,** which section 3.3 already
schedules as phase 1 work in emu/memory.c: a per-entry state byte, which the
pager needs anyway to know what it may evict. With a real resident count every
one of the figures above becomes a measurement rather than a bound, and they
agree by construction because they share a source. Sequence the MemTotal change
after it, not before.

Two things to carry forward regardless:

- The fork-inheritance family is wider than the one counter fixed here.
  `group->rusage` is not reset in `tgroup_copy` (kernel/fork.c), and
  `ru_maxrss` is re-derived from `mm->rss_pages_hwm` so the reset in
  `task_create_` does not reach it. Both are pre-existing and both are visible
  through `getrusage` and `times(2)` today.
- `kernel/mmap.c`'s comment on the headroom guard still says "No-op outside
  iOS", which stopped being true when `ISH_GUEST_MEM_BUDGET_MB` landed.
