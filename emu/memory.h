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
// Uninitialize the address space
void mem_destroy(struct mem *mem);
void mem_set_page_limit(struct mem *mem, page_t limit);
void mem_set_mmap_window(struct mem *mem, page_t floor, page_t ceiling);
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
size_t mem_mapped_page_count(struct mem *mem);
void *mem_ptr_fault(struct mem *mem, guest_addr_t addr, int type);
// Reverse-map a faulting host address to the guest address it backs (for host
// SIGBUS -> guest SIGBUS translation of file-backed-mmap truncation faults).
// Lockless / signal-handler-safe. Returns true and fills *guest_out on a hit.
bool mem_host_addr_to_guest(struct mem *mem, void *host_addr, guest_addr_t *guest_out);

#define BYTES_ROUND_DOWN(bytes) (PAGE(bytes) << PAGE_BITS)
#define BYTES_ROUND_UP(bytes) (PAGE_ROUND_UP(bytes) << PAGE_BITS)

#define LEAK_DEBUG 0

struct mmap_cache_entry;

struct data {
    void *data; // immutable
    size_t size; // also immutable
    atomic_uint refcount;
    uintptr_t shared_key;
    uint8_t *host_page_prot; // cached mirrored host protections, one per host page
    // Set only for never-writable file-backed mappings (fs/mmap_cache.h) --
    // lets /proc/<pid>/smaps see cross-process sharing this refcount can't.
    struct mmap_cache_entry *cache_entry;

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
struct pt_entry {
    struct data *data;
    size_t offset;
    unsigned flags;
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

// Map memory + offset into fake memory, unmapping existing mappings. Takes
// ownership of memory. It will be freed with:
// munmap(memory, pages * PAGE_SIZE)
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

#endif
