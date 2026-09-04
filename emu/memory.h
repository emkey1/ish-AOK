#ifndef MEMORY_H
#define MEMORY_H

#include <stdatomic.h>
#include <unistd.h>
#include <stdbool.h>
#include <sched.h>
#include "emu/mmu.h"
#include "util/list.h"
#include "util/sync.h"
#include "misc.h"
#if ENGINE_JIT
struct jit;
#endif

struct pt_directory_chunk;

// A reserved-but-unmaterialised anonymous range: address space the guest has
// mapped, for which no page-table entries exist yet.
//
// Why: pt_map_nothing builds one struct pt_entry per page, ~65 bytes of host
// memory each, eagerly at mmap time. Measured on this build, host RSS while
// holding an untouched reservation is dead linear at ~16.6 MB per GiB -- 1.06
// GB for a 64 GiB reservation -- with no ceiling below the 256 TiB page limit,
// so one mmap can get the app OOM-killed. Linux makes the same call free.
//
// INVARIANT, and the reason this design is shaped the way it is: a reservation
// is NEVER split. Materialising a fault takes the whole prefix up to the end of
// the faulting chunk and trims the front, so a reservation only ever shrinks
// from the left or disappears. Anything that would punch a hole in one
// materialises it in full first, at a call site where mapping is safe. That
// removes slot exhaustion, partial-update atomicity, and the lock recursion
// that a splitting version has to get right.
//
// The cost of never splitting: a fault at the far end of a reservation
// materialises everything before it, i.e. exactly today's eager behaviour and
// no worse. The win is every case that reserves and touches little or nothing.
struct mem_lazy_map {
    page_t start, end;   // empty iff start >= end
    unsigned flags;      // pt flags the pages get when materialised
};
#define MEM_LAZY_MAX 32
// Below this the eager path is kept: ordinary programs touch 36-72% of what
// they map (measured: bash 36%, python3 52%, gcc 72%) over mappings of a few
// MB, where per-page faulting would cost more than the batch loop, and their
// eager cost is trivial anyway.
#define MEM_LAZY_MIN_PAGES ((64u * 1024 * 1024) >> PAGE_BITS)
// Granularity of a fault: materialise at least this much past the reservation
// start, so a sequential walk pays one fault per 2 MiB rather than per page.
#define MEM_LAZY_CHUNK_PAGES 512u

struct mem {
    _Atomic(struct pt_directory_chunk *) *pgdir_root;
    // Set-only bitmap (one bit per pgdir_root entry) of which roots have a
    // chunk. Page-table chunks are never freed until mem_destroy, so this only
    // ever gains bits. mem_next_allocated_leaf_base() bit-scans it to skip empty
    // roots in bulk instead of linearly probing the (32 KiB) pgdir_root array --
    // the dominant pt_find_hole cost on a large/sparse amd64 address space.
    _Atomic uint64_t *pgdir_root_bitmap;
    page_t page_limit;
    page_t mmap_floor;
    page_t mmap_ceiling;
    // [brk_reserve_start, brk_reserve_end) is address space set aside by
    // exec.c for future brk growth (arm64/riscv64 dynamic-PIE headroom) —
    // tracked as a plain range, NOT real page-table entries, so reserving it
    // is O(1) and a fork() has nothing extra to walk/copy. pt_is_hole/
    // pt_find_hole treat it as occupied; sys_brk_guest claims prefixes of it
    // by mapping real pages and advancing brk_reserve_start. Empty iff
    // brk_reserve_start >= brk_reserve_end (the initial all-zero state
    // qualifies, so no separate "has a reservation" flag is needed).
    page_t brk_reserve_start;
    page_t brk_reserve_end;

    // The main stack, for bounding its growth the way Linux's
    // expand_downwards() does. stack_top is the exclusive top page, recorded
    // by exec; stack_limit_pages is RLIMIT_STACK in pages. Either being 0
    // means "no bound known", which is the pre-exec state.
    //
    // Cached here rather than read from the task's rlimits at fault time, and
    // that is deliberate: the fault path already holds mem->lock, rlimit_get()
    // takes group->lock, and better than a hundred sites take group->lock
    // before touching guest memory. Taking them in that order here would
    // invert the nesting against every one of them.
    //
    // Atomic because setrlimit stores it from a different thread than the one
    // faulting. mm_copy copies the whole struct and then calls mem_init, which
    // clears these, so it restores them explicitly afterwards -- a fork's
    // child keeps its parent's stack and limit.
    page_t stack_top;
    _Atomic page_t stack_limit_pages;

    // Where the last eviction sweep of this address space stopped, so the next
    // bounded one carries on rather than restarting. A guest page number; it
    // wraps to 0 at page_limit.
    //
    // Direct reclaim asks for a few MiB at a time, and without a cursor every
    // call would sweep from page 0 and evict the same first frames of the same
    // first mapping -- which the guest then faults straight back in, because
    // those are the pages it is using. The cursor is what makes repeated
    // bounded calls cover the address space.
    //
    // Read and written only under the address-space barrier, so it needs no
    // atomicity of its own. MUST be cleared in mem_init: mm_copy does a
    // whole-struct copy and then calls mem_init on the child, and this struct's
    // rule is that every field inherited that way is accounted for.
    page_t swap_hand;

    // Lazy anonymous reservations -- same idea as brk_reserve above (a plain
    // range, no page-table entries) but there can be several. See
    // struct mem_lazy_map's comment for the no-split invariant.
    struct mem_lazy_map lazy[MEM_LAZY_MAX];
    unsigned lazy_count;
    _Atomic int quiesce_requested;
    // Parking lot for quiesce waiters (mem_quiesce_park/mem_quiesce_wake_parked,
    // memory.c). Leaf lock: nothing else is ever taken under it.
    pthread_mutex_t quiesce_park_lock;
    pthread_cond_t quiesce_park_cond;

#if ENGINE_JIT
    struct jit *jit;
#endif
    struct mmu mmu;
    struct {
        atomic_int count; // If positive, don't delete yet, wait_to_delete
        bool ready_to_be_freed; // Should be false initially
    } reference;

    // Descriptors whose last reference was dropped by an unmap, parked until
    // the address-space lock is gone.
    //
    // Closing one runs the filesystem's ->close, and that is allowed to BLOCK
    // on a guest process: fusefs sends FUSE_FLUSH and waits for its daemon's
    // answer. Every unmap runs under this mem's write lock with the process's
    // other threads quiesced -- and when the daemon is one of those threads,
    // it can no longer answer. Doing the close here rather than there is what
    // keeps that from being a hang.
    //
    // Guarded by its own leaf lock so a deferral made under the write lock and
    // a drain made after it need not share one. Drained by
    // mem_close_deferred_fds(). MUST be cleared in mem_init: mm_copy does a
    // whole-struct copy and then calls it on the child (see mem->lazy).
    struct fd **deferred_fds;
    unsigned deferred_count, deferred_cap;
    pthread_mutex_t deferred_lock;

    wrlock_t lock;
    // Serializes every structural page-table mutation (map/unmap/protect/COW).
    // Evicting writers hold this *and* take `lock` in write mode + poke siblings.
    // The growth-mmap fast path holds ONLY this: adding never-mapped pages needs
    // no reader eviction (no stale TLB entry) and no rwlock (entry publication is
    // atomic), just mutual exclusion against a concurrent unmap freeing chunks.
    pthread_mutex_t pt_alloc_lock;
};

extern _Atomic long quiesce_reader_naps;

// Block until quiesce_requested drops to 0, sleeping on the mem's parking-lot
// condvar (no CPU burned). memory.c.
void mem_quiesce_park(struct mem *mem);
// Broadcast the parking lot after dropping quiesce_requested (the barrier
// writer's release side, mem_write_unlock_with_pokes). memory.c.
void mem_quiesce_wake_parked(struct mem *mem);

// Wait out a writer holding the mem-quiesce barrier. sched_yield() for a short
// burst hands the CPU straight to the writer so the brief common case (a small
// page-table edit) clears in microseconds; past that the waiter PARKS on the
// mem's condvar until the writer's release broadcast. The old scheme kept
// yielding for 256 spins and then nanosleep-polled — under a barrier STORM
// (thread benchmark: one mprotect per pthread_create, with dozens of sibling
// threads alive) that put every parked sibling in a sched_yield hot loop,
// monopolizing the host scheduler so hard that freshly-created threads never
// got scheduled to run and exit — the pileup fed itself until the app wedged
// (main thread starved mid-open()). Parked threads now cost nothing, however
// many pile up. `spins` is threaded across both the inner and outer waits of
// a single acquire.
#define MEM_QUIESCE_SPIN_YIELDS 32
static inline void mem_quiesce_wait(struct mem *mem, int *spins) {
    if ((*spins)++ < MEM_QUIESCE_SPIN_YIELDS) {
        sched_yield();
    } else {
        atomic_fetch_add_explicit(&quiesce_reader_naps, 1, memory_order_relaxed);
        mem_quiesce_park(mem);
    }
}

static inline void mem_read_lock_quiesce_aware(struct mem *mem) {
    if (mem == NULL)
        return;
    int spins = 0;
    while (true) {
        while (atomic_load_explicit(&mem->quiesce_requested, memory_order_acquire) > 0)
            mem_quiesce_wait(mem, &spins);
        read_lock(&mem->lock);
        if (atomic_load_explicit(&mem->quiesce_requested, memory_order_acquire) == 0)
            return;
        read_unlock(&mem->lock);
        mem_quiesce_wait(mem, &spins);
    }
}

static inline void mem_read_unlock_quiesce_aware(struct mem *mem) {
    if (mem != NULL)
        read_unlock(&mem->lock);
}

#define MEM_DEFAULT_PAGE_LIMIT ((page_t) 1 << 20)
#define MEM_DEFAULT_MMAP_FLOOR ((page_t) 0x40000)
#define MEM_DEFAULT_MMAP_CEILING ((page_t) 0xf7ffe)
#define MEM_PTDIR_BITS 10
#define MEM_PTDIR_SIZE (1 << MEM_PTDIR_BITS)
#define MEM_PGDIR_MID_BITS 13
#define MEM_PGDIR_MID_SIZE (1 << MEM_PGDIR_MID_BITS)
#define MEM_PGDIR_ROOT_BITS 12
#define MEM_PGDIR_ROOT_SIZE (1 << MEM_PGDIR_ROOT_BITS)
#define MEM_MAX_PAGE_LIMIT ((page_t) 1 << (MEM_PTDIR_BITS + MEM_PGDIR_MID_BITS + MEM_PGDIR_ROOT_BITS))

// Initialize the address space
void mem_init(struct mem *mem);
// Close the descriptors an unmap parked. MUST be called with no address-space
// lock held; see struct mem's deferred_fds.
void mem_close_deferred_fds(struct mem *mem);
// Uninitialize the address space
void mem_destroy(struct mem *mem);
void mem_set_page_limit(struct mem *mem, page_t limit);
void mem_set_mmap_window(struct mem *mem, page_t floor, page_t ceiling);
void mem_set_stack_bounds(struct mem *mem, page_t top, uint64_t limit_bytes);
// Return the pagetable entry for the given page
struct pt_entry *mem_pt(struct mem *mem, page_t page);
// Lazy anonymous reservations; see struct mem_lazy_map above.
struct mem_lazy_map *mem_lazy_find(struct mem *mem, page_t page);
bool mem_lazy_overlaps(struct mem *mem, page_t start, page_t end);
bool mem_lazy_reserve(struct mem *mem, page_t start, pages_t pages, unsigned flags);
// Materialise every reservation overlapping [start, end), IN FULL. Maps, so it
// must not be called with the JIT invalidate lock held.
void mem_lazy_materialize_range(struct mem *mem, page_t start, page_t end);
// Drop reservation coverage of [start, end) without mapping. Only valid when
// the range does not punch a hole in a reservation; callers use
// mem_lazy_would_split() first.
void mem_lazy_drop(struct mem *mem, page_t start, page_t end);
bool mem_lazy_would_split(struct mem *mem, page_t start, page_t end);
// Increment *page, skipping over unallocated page directories. Intended to be
// used as the incremenent in a for loop to traverse mappings.
void mem_next_page(struct mem *mem, page_t *page);
// Pages of this address space with a live page-table entry. NOT a residency
// measure -- see the definition, and mem_resident_page_count below.
size_t mem_mapped_page_count(struct mem *mem);
// Pages with a live entry whose contents are actually in host memory right
// now, i.e. mem_mapped_page_count minus the pages the pager has evicted.
//
// This is the first residency signal the tree has ever had, and it is still
// only a partial one, in ONE direction: it over-reports, because AOK builds
// page-table entries eagerly at mmap() time and no field records whether a
// page was ever touched. It does not under-report -- the walk asks each
// frame's own slot rather than the entry's conservative swap_state hint,
// precisely so that a frame a forked sibling faulted back is not counted as
// absent. See the definition, and why the over-report is what blocks MemTotal.
size_t mem_resident_page_count(struct mem *mem);
void *mem_ptr_fault(struct mem *mem, guest_addr_t addr, int type);
// Reverse-map a faulting host address to the guest address it backs (for host
// SIGBUS -> guest SIGBUS translation of file-backed-mmap truncation faults).
// Lockless / signal-handler-safe. Returns true and fills *guest_out on a hit.
bool mem_host_addr_to_guest(struct mem *mem, void *host_addr, guest_addr_t *guest_out);

#define BYTES_ROUND_DOWN(bytes) (PAGE(bytes) << PAGE_BITS)
#define BYTES_ROUND_UP(bytes) (PAGE_ROUND_UP(bytes) << PAGE_BITS)

#define LEAK_DEBUG 0

struct mmap_cache_entry;

// One address space that reaches a struct data, and how many of that address
// space's page-table entries do. See struct data::owners for the encoding and
// emu/memory.c's "exact ownership" section for why it is shaped this way.
struct data_owner {
    struct mem *mem;
    uint32_t entries;
};

struct data {
    // NOT immutable, whatever this field used to claim. pt_map sets it when the
    // struct is built, but a MAP_SHARED anonymous region reserved with no host
    // backing starts out NULL here, and mem_materialize_shared_data
    // (emu/memory.c) fills it in later, under a private static lock of its own.
    //
    // The writer does hold a mem write lock -- its OWN: the only path in is
    // pt_set_flags, called by sys_mprotect_guest under
    // mem_write_lock_with_pokes. That lock excludes nothing that matters here,
    // because one struct data is reachable from several address spaces at once:
    // pt_copy_on_write points the child's page-table entry at the parent's
    // struct on every fork. A thread walking a forked sibling's page table
    // under THAT mem's read lock is not held off at all. Hence the private
    // lock, which is the only thing that covers the transition.
    //
    // What is guaranteed: the value moves one way only, NULL -> non-NULL, and
    // never changes again. So a reader that has already loaded a non-NULL
    // pointer holds one that stays valid for as long as its own reference to
    // this struct does.
    //
    // What is NOT guaranteed, and what a design must not assume: that a reader
    // which sees NULL will still see NULL a moment later, or that holding the
    // lock of the mem being walked orders this field in any way at all.
    // Deciding anything on "still unbacked" means going through
    // mem_materialize_shared_data.
    void *data;
    size_t size; // immutable: pt_map sets it, nothing ever changes it
    atomic_uint refcount;

    // ---- exact ownership (docs/simulated_swap_plan.md section 3.8) --------
    //
    // Which address spaces reach this struct data, and with how many entries
    // each. `refcount` cannot answer that: it counts page-table ENTRIES, so a
    // 4-page mapping forked once and an 8-page mapping never forked both read
    // 8. The pager has to know the difference, because releasing a host frame
    // that a SECOND address space still believes it owns hands that process a
    // PROT_NONE page it never asked for.
    //
    // Two slots plus an overflow COUNT, and the count is not optional: with A
    // in slot 0, B in slot 1 and C overflowed, A unmapping would free slot 0
    // and the data would then read as exclusive to B while C still maps it.
    // Ordinary shell use reaches three owners in under a second
    // (`sh -c '( sleep 3 ) & ( sleep 3 ) & sleep 1; cat /proc/$$/smaps'` shows
    // /bin/busybox at 3 sharers), so this is the common case and not a corner.
    //
    // Exclusive to M iff n_owners == 1 && owners[0].mem == M &&
    // overflow_entries == 0 -- exact in both directions, and it recovers
    // milliseconds after a fork-and-exec rather than pinning the mapping for
    // the life of the process. Ask through data_is_exclusive_to(), which takes
    // the stripe lock these fields live under.
    //
    // NOTE, and it is a precondition rather than something these records can
    // express: an exclusive struct data is not the same thing as an exclusive
    // host frame. The vdso is one static array (kernel/vdso.c) that every
    // 32-bit exec pt_map()s into its own struct data, so N of them read as
    // exclusive while sharing one host page -- which in build/ish also holds
    // unrelated emulator globals, including a live lock. Eviction excludes it
    // by identity, separately from these records.
    struct data_owner owners[2];
    uint32_t overflow_entries;
    uint8_t n_owners;

    // One byte per host frame (16 KiB on Apple silicon, so four guest pages)
    // of `size`: how many page-table entries, across every address space,
    // point into that frame. Maintained at the same five sites as refcount.
    //
    // Frame membership is by DATA OFFSET, not by guest adjacency: pt_move
    // copies `offset` verbatim, so after a partial mremap the four entries
    // sharing a host frame can sit at unrelated guest addresses, or one can be
    // gone. A frame is releasable only when the entries the evictor actually
    // found equal frame_refs[f]; without that the test is "safe by luck of the
    // aliasing rules" rather than by measurement.
    //
    // NULL means "unknown", which reads as not evictable: a failed allocation
    // here must not fail the guest's mmap. Counts saturate at 255 and are
    // never decremented again once they do -- see data_frame_refs_add_locked.
    uint8_t *frame_refs;

    // One slot number per host frame of `size`, or SWAP_SLOT_NONE when the
    // frame's bytes are in host memory. THIS IS THE ONLY PLACE A SWAP SLOT IS
    // RECORDED, and that single-ownership is the point rather than a tidiness
    // preference.
    //
    // The slot used to live in each pt_entry, copied to every alias by pt_dup,
    // pt_move and pt_copy_on_write. That made N independent copies of one
    // fact, and they went out of step the moment any of them was published
    // resident on its own: an mremap out of an evicted frame left the moved
    // entry still holding the slot, so touching it re-read the whole 16 KiB
    // frame from disk OVER bytes its three siblings had been written since --
    // a silent revert, reproduced deterministically and single-threaded by
    // scratch mrem.c. A frame is what the host releases and what a slot holds,
    // so the frame is what owns the slot.
    //
    // Read on the swap-in path with only the frame lock (see swap_frame_lock_
    // for), which is why it is atomic; written under that lock, and by the
    // evictor under the address-space barrier. NULL alongside a NULL
    // frame_refs means this mapping can never be evicted, so the question
    // never arises.
    _Atomic uint32_t *frame_slot;

    uintptr_t shared_key;
    uint8_t *host_page_prot; // cached mirrored host protections, one per host page
    // Set only for never-writable file-backed mappings (fs/mmap_cache.h) --
    // lets /proc/<pid>/smaps see cross-process sharing this refcount can't.
    struct mmap_cache_entry *cache_entry;

    // This mapping was counted as a live SHARED mapping of its memfd
    // (kernel/memfd.c), which is what holds F_SEAL_WRITE off. Recorded here
    // because the count has to come off when the mapping does, and that
    // happens here and nowhere else.
    bool memfd_shared_mapped;

    // for display in /proc/pid/maps
    struct fd *fd;
    size_t file_offset;
    const char *name;

    // jit/hle.c memoizes here whether this mapping is a libc it can attach
    // to, and which parsed module it is. Resolving that costs a path lookup
    // (a SQLite round trip on a fakefs root) plus an ELF parse, and block
    // translation asks the question on every fingerprint-table miss -- i.e.
    // for essentially every block of every process. 0 = not resolved yet;
    // see hle_mapping_module() for the encoding.
    atomic_uint hle_memo;
#if LEAK_DEBUG
    int pid;
    guest_addr_t dest;
#endif
};
// Swap state of one guest page. PROTOTYPE NOTE: docs/simulated_swap_plan.md
// section 3.3 puts these bytes inside the vestigial blocks[] so pt_entry stays
// 56 bytes; this gate build adds them alongside instead, which is 8 bytes per
// guest page (about 0.2% of mapped size) and not what should ship. Folding them
// into the union is mechanical and belongs with the pager core.
//
// PT_RESIDENT is 0 so a calloc'd leaf and every existing pt_map path yield a
// resident entry with no extra work, which is what keeps swap-off byte
// identical to today.
#define PT_RESIDENT 0
#define PT_SWAPPED  1

// "This frame's bytes are in host memory", in struct data::frame_slot. ZERO,
// so that pt_map's calloc of that array already says it about every frame and
// no initialisation loop is needed on a path that runs whether or not swap is
// ever enabled. The allocator simply never hands out slot 0, which wastes one
// 16 KiB frame of a file that is unlinked at creation.
#define SWAP_SLOT_NONE 0u

struct pt_entry {
    struct data *data;
    size_t offset;
    unsigned flags;
    // Read on the fault path by mem_ptr_nofault, written under the
    // address-space write lock by the evictor and by swap-in. Atomic because
    // the read happens under the READ lock, concurrently with a writer that
    // has quiesced the other threads but not this one's inspection.
    //
    // A CONSERVATIVE HINT, not the truth: the truth about whether a frame is
    // in host memory is struct data::frame_slot, which the frame owns. PT_
    // SWAPPED here may be stale -- a sibling entry's fault, or a forked
    // sibling ADDRESS SPACE's, can bring the frame back without touching this
    // one -- and on the fault path that costs nothing but a trip to the slow
    // path, which finds the frame already resident and just publishes the
    // entry (a "cancelled" fault). The other direction is the one that must
    // never happen: PT_RESIDENT over a released frame is a HOST fault in
    // emulator C code. Eviction is what guarantees it cannot, by refusing any
    // frame whose entries it did not all find (frame_refs) in an address space
    // that is not the only one reaching the mapping (owners).
    //
    // THE RULE THAT FOLLOWS, and it is not optional: anything that needs to
    // know whether bytes are in host memory must ask the FRAME. Only the fault
    // path may read this byte alone, because being wrong there costs a slow
    // path and nothing else. Two callers learned this the hard way and both
    // now ask the frame -- mem_resident_page_count, which reported 8 MiB of
    // resident memory as absent after a fork, and swap_frame_eligible, where a
    // stale hint made a frame permanently unevictable.
    _Atomic uint8_t swap_state;
#if ENGINE_JIT
    struct list blocks[2];
#endif
};
// page flags
// P_READ and P_EXEC are ignored for now
#define P_READ (1 << 0)
#define P_WRITE (1 << 1)
#undef P_EXEC // defined in sys/proc.h on darwin
#define P_EXEC (1 << 2)
#define P_RWX (P_READ | P_WRITE | P_EXEC)
#define P_GROWSDOWN (1 << 3)
#define P_COW (1 << 4)
#define P_WRITABLE(flags) (flags & P_WRITE && !(flags & P_COW))

// mapping was created with pt_map_nothing
#define P_ANONYMOUS (1 << 6)
// mapping was created with MAP_SHARED, should not CoW
#define P_SHARED (1 << 7)
// madvise(MADV_WIPEONFORK): a child of fork() gets fresh zero pages here
// instead of inheriting the parent's data. Cleared by MADV_KEEPONFORK.
#define P_WIPEONFORK (1 << 8)

bool pt_is_hole(struct mem *mem, page_t start, pages_t pages);
page_t pt_find_hole(struct mem *mem, pages_t size);

// Map memory + offset into fake memory, unmapping existing mappings.
//
// Takes ownership of `memory` ONLY when it returns 0. It is then freed with
// munmap(memory, pages * PAGE_SIZE + offset) once the last page referring to it
// is unmapped.
//
// That length is in GUEST pages and is routinely not a whole number of HOST
// pages -- on Apple silicon a one-page mapping munmaps 4096 bytes of a 16 KiB
// host page. The host rounds the length up and releases the whole host page,
// which is correct only because no other mapping can be living in it: the host
// never places two mmap()s inside one host page (measured on this Mac: 4096
// separate 4 KiB mappings, no two sharing a host page, minimum gap exactly
// 16384). The host-page packing in emu/memory.c relies on that -- it puts
// several guest pages of ONE struct data in one host page, never two struct
// datas -- so a change that started sub-allocating host pages across mappings
// would have to fix this teardown first.
//
// On EVERY error return, `memory` is still the caller's, and pt_map does NOT
// munmap it -- the caller must. That split is not a stylistic choice:
// kernel/ipc.c's shmat already unmaps the segment itself when pt_map fails, and
// munmapping here as well would be a double munmap of an address range the host
// is free to have handed to something else in between.
//
// Every error return happens BEFORE the first page-table entry is published:
// pt_map allocates all the page-table leaves the range needs up front, which
// leaves the publication loop with no failure path. So an error means no entry
// ever named `memory`, no entry that existed before was disturbed, and the
// caller's munmap cannot race a sibling thread that resolved a pointer into the
// range. That ordering is load-bearing on the pure-growth mmap fast path, which
// runs pt_map with only the mem READ lock held; see the pre-pass comment in
// pt_map for the full argument.
//
// Two caveats on that ownership split. `memory` may legitimately be NULL (the
// PROT_NONE branch of pt_map_nothing) or vdso_data (kernel/exec.c), and neither
// is ever munmapped: pt_unmap_always_unlocked skips both, so a caller passing
// one has nothing to release on an error return either.
//
// And on any path that gets past the two validation checks at the top, an error
// return does not undo one thing: a lazy anonymous reservation overlapping the
// range has already been dropped or materialised, because that happens up front
// too. Long-standing, and unrelated to `memory`.
int pt_map(struct mem *mem, page_t start, pages_t pages, void *memory, size_t offset, unsigned flags);
// Map empty space into fake memory
int pt_map_nothing(struct mem *mem, page_t page, pages_t pages, unsigned flags);
// Move an existing mapped range into a hole.
int pt_move(struct mem *mem, page_t old_start, page_t new_start, pages_t pages);
// Like pt_move, but leaves the source mapped: both addresses end up aliasing
// the same pages. Only valid for shared mappings -- see the definition.
int pt_dup(struct mem *mem, page_t old_start, page_t new_start, pages_t pages);
// Unmap fake memory, return -1 if any part of the range isn't mapped and 0 otherwise
int pt_unmap(struct mem *mem, page_t start, pages_t pages);
// like pt_unmap but doesn't care if part of the range isn't mapped
int pt_unmap_always(struct mem *mem, page_t start, pages_t pages);
// Set the flags on memory
int pt_set_flags(struct mem *mem, page_t start, pages_t pages, int flags);
// Copy pages from src memory to dst memory using copy-on-write
int pt_copy_on_write(struct mem *src, struct mem *dst, page_t start, page_t pages);

// Is `data` reachable from exactly one address space, and is that `mem`?
// Exact in both directions -- see struct data::owners. Takes the striped
// ownership lock, which is a leaf: nothing else may be taken under it, and it
// may be taken with or without any mem lock held.
bool data_is_exclusive_to(struct data *data, struct mem *mem);
// How many page-table entries, across every address space, point into the
// host frame containing `offset`. -1 when that is not known (no frame_refs
// array, or the count saturated), which every caller must read as "do not
// touch this frame".
int data_frame_ref_count(struct data *data, size_t offset);
// Size of a host frame: the smallest unit the host will actually release,
// protect or replace. real_page_size where that is larger than a guest page
// (16 KiB on Apple silicon, so four guest pages), else PAGE_SIZE.
size_t mem_frame_size(void);

// Must call with mem read-locked.
void *mem_ptr(struct mem *mem, guest_addr_t addr, int type);
int mem_segv_reason(struct mem *mem, guest_addr_t addr);

// Reference counting is important
void mem_ref_cnt_mod(struct mem *mem, int value);
int mem_ref_cnt_get(struct mem *mem);

extern size_t real_page_size;

// Whether guest page protection changes (e.g. mprotect promoting a page to
// writable) can be reflected into real host mprotect() calls. False whenever
// a host mprotect() can't safely target a single guest page (host/guest page
// size mismatch, or disabled outright -- see mem_host_page_mirroring_enabled
// in memory.c). Callers that create a host mapping less permissive than a
// guest page might later become must pre-grant the wider permission up front
// when this is false, since there's no way to promote it after the fact.
bool mem_host_page_mirroring_available(void);

// ---- Phase 1 gate prototype: evict a guest frame and fault it back ---------
//
// docs/simulated_swap_plan.md section 7 "Phase 1 gate". This is the smallest
// thing that can answer the gate's question -- can a frame be released, does
// the footprint actually drop, and does a stale access FAULT rather than
// quietly return the old bytes -- and it is deliberately not the pager. There
// is no aging, no kswapd, no watermarks, no guest-visible surface and no
// multi-owner eviction; section 7 puts all of those in the core phase.
//
// OFF unless asked: no swap file, no slot table, no kswapd thread and no I/O
// until swap_evict_mem() is called, which only /proc/ish/swap_evict does. That
// is the shipping default too -- swap is opt-in from Settings, and with it off
// the guest must see byte-for-byte today's behaviour.
//
// The one thing that is NOT conditional is the per-mapping bookkeeping the
// pager needs to have been correct all along: struct data's owner records and
// frame_refs. Section 3.14 budgets exactly that ("struct data gains about 28
// bytes plus 64 B per MiB of mapping ... swap disabled: no thread, no file, no
// slot table"), because the records have to be right from the first mmap of a
// launch, and whether swap is enabled is a launch-time decision. When the
// enable flag lands it should gate pt_map's frame_refs calloc, which is the
// only allocation of the three.

// Evict every eligible frame of `mem`. Returns bytes released, or a negative
// errno. Takes the address-space barrier itself; must be called with no mem
// lock held.
long swap_evict_mem(struct mem *mem);
// The same, but stopping once `want_bytes` have been released, and RESUMING
// where the last sweep of this address space stopped rather than restarting at
// page 0. Returns bytes released, or a negative errno.
long swap_evict_bytes(struct mem *mem, uint64_t want_bytes);
// The address-space barrier, from kernel/mmap.c (see its definition for why a
// plain write_lock is not enough: it quiesces the other threads of this mem).
void mem_write_lock_pokes_external(struct mem *mem);
void mem_write_unlock_pokes_external(struct mem *mem);
// Page one evicted guest page back in, with the mem READ lock held; it upgrades
// internally and hands the read lock back. Returns 0, or a negative errno.
//
// The upgrade RELEASES the reader before it waits, so a caller that cannot then
// hold one lock continuously through to the access must not use this form --
// the evictor fits in that gap. mem_ptr_fault therefore calls the write-locked
// core directly (static, in memory.c); mem_ptr uses this one because it holds
// the read lock from here to mem_ptr_nofault.
int swap_fault_page(struct mem *mem, page_t page);
// Counters for the /proc/ish report.
unsigned long swap_prototype_ledger_refused(void);
void swap_prototype_failures(unsigned long *adv_fail, unsigned long *prot_fail, int *adv_errno, int *prot_errno);
unsigned long long swap_footprint_live(void);
void swap_footprint_detail(unsigned long long *internal, unsigned long long *resident,
                           unsigned long long *reusable, unsigned long long *compressed);
void swap_prototype_ledger(unsigned long long *before, unsigned long long *after,
                           unsigned long long *post);
void swap_prototype_stats(unsigned long *evicted_frames, unsigned long *faulted_frames,
                          unsigned long *bytes_out, unsigned long *bytes_in);
// Faults that found the frame already resident and did no I/O.
unsigned long swap_prototype_cancelled(void);
// Bring every evicted frame of `mem` back, taking the address-space barrier
// itself. Returns how many frames could not be brought back, which should be 0.
// This is what kernel/swap.c's swap_disable() calls on each live address space.
long swap_fault_mem_all(struct mem *mem);

#endif
