#define _GNU_SOURCE // for dladdr/Dl_info (refcount-underflow diagnostic)
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#define DEFAULT_CHANNEL memory
#include "debug.h"
#include "kernel/errno.h"
#include "kernel/signal.h"
#if __APPLE__
#include <mach/mach.h>
#endif
#include "emu/memory.h"
#include "fs/fd.h"
#include "emu/tlb.h"
#include "jit/jit.h"
#include "kernel/vdso.h"
#include "kernel/task.h"
#include "kernel/swap.h"
#include "fs/fd.h"
#include "fs/mmap_cache.h"
#include "kernel/calls.h"
#include "util/sync.h"
#include <dlfcn.h>

// Time to wait between non blocking lock attempts
struct timespec lock_pause = {0 /*secs*/, WAIT_SLEEP /*nanosecs*/};

// --- mem-quiesce barrier instrumentation (dumped at exit if ISH_QUIESCE_STATS) ---
// Counts the stop-the-world barrier that every mmap/munmap/mprotect/brk/fork
// triggers. Relaxed atomics: cheap, identical overhead on baseline and fixed
// builds, so the A/B comparison stays honest.
_Atomic long quiesce_barriers;     // mem_write_lock_with_pokes entries
_Atomic long quiesce_writer_naps;  // writer-side nanosleep iterations
_Atomic long quiesce_reader_naps;  // reader-side nanosleep iterations (both sites)
_Atomic long quiesce_poke_calls;   // task_poke_shared_mem invocations
_Atomic long quiesce_poke_noop;    // ...that did nothing (pids_lock trylock failed)
_Atomic long quiesce_pokes_sent;   // SIGUSR1s actually delivered to siblings
_Atomic long quiesce_pokes_skipped;// siblings skipped because parked in a syscall
_Atomic long quiesce_growth_fast;  // pure-growth mmaps that skipped the barrier entirely

void quiesce_stats_dump(const char *tag) {
    fprintf(stderr,
        "[quiesce %s] barriers=%ld growth_fast=%ld writer_naps=%ld reader_naps=%ld "
        "poke_calls=%ld poke_noop=%ld pokes_sent=%ld pokes_skipped=%ld\n",
        tag ? tag : "",
        atomic_load_explicit(&quiesce_barriers, memory_order_relaxed),
        atomic_load_explicit(&quiesce_growth_fast, memory_order_relaxed),
        atomic_load_explicit(&quiesce_writer_naps, memory_order_relaxed),
        atomic_load_explicit(&quiesce_reader_naps, memory_order_relaxed),
        atomic_load_explicit(&quiesce_poke_calls, memory_order_relaxed),
        atomic_load_explicit(&quiesce_poke_noop, memory_order_relaxed),
        atomic_load_explicit(&quiesce_pokes_sent, memory_order_relaxed),
        atomic_load_explicit(&quiesce_pokes_skipped, memory_order_relaxed));
}

// Quiesce parking lot (see memory.h's mem_quiesce_wait). No missed wakeup:
// the parker re-checks quiesce_requested UNDER the mutex, and the release
// side broadcasts UNDER the same mutex after decrementing — if the parker
// saw the count still up, the writer's broadcast necessarily happens after
// the parker is in pthread_cond_wait. SIGUSR1 pokes landing on a parked
// thread just bounce off pthread_cond_wait's internal restart (the re-check
// loop absorbs spurious wakeups either way).
void mem_quiesce_park(struct mem *mem) {
    pthread_mutex_lock(&mem->quiesce_park_lock);
    while (atomic_load_explicit(&mem->quiesce_requested, memory_order_acquire) > 0)
        pthread_cond_wait(&mem->quiesce_park_cond, &mem->quiesce_park_lock);
    pthread_mutex_unlock(&mem->quiesce_park_lock);
}

void mem_quiesce_wake_parked(struct mem *mem) {
    pthread_mutex_lock(&mem->quiesce_park_lock);
    pthread_cond_broadcast(&mem->quiesce_park_cond);
    pthread_mutex_unlock(&mem->quiesce_park_lock);
}

static bool amd64_jit_debug_enabled(void) {
    static int enabled = -1;
    if (enabled == -1)
        enabled = getenv("ISH_TRACE_AMD64_JIT") != NULL ? 1 : 0;
    return enabled == 1;
}

// increment the change count
static void mem_changed(struct mem *mem);
// Swap-in with the address-space write lock already held; defined with the rest
// of the pager prototype at the end of this file, declared here because
// mem_ptr_fault needs it and sits above it.
static int swap_fault_page_locked(struct mem *mem, page_t page);
static struct mmu_ops mem_mmu_ops;
static _Atomic uint64_t next_mem_change_id = 1;
#define PGDIR_ROOT_INDEX(page) ((page) >> (MEM_PTDIR_BITS + MEM_PGDIR_MID_BITS))
#define PGDIR_MID_INDEX(page) (((page) >> MEM_PTDIR_BITS) & (MEM_PGDIR_MID_SIZE - 1))
#define PGDIR_LEAF_INDEX(page) ((page) & (MEM_PTDIR_SIZE - 1))
#define PGDIR_LEAF_BASE(root, mid) ((((page_t) (root) << MEM_PGDIR_MID_BITS) | (page_t) (mid)) << MEM_PTDIR_BITS)

struct pt_directory_chunk {
    _Atomic(struct pt_entry *) leaves[MEM_PGDIR_MID_SIZE];
    // Set-only bitmap of which leaves[] slots are populated, scanned the same way
    // as the root bitmap: the mid directory is the second sparse level (8192
    // slots/chunk), and a high mapping (e.g. an amd64 PIE at mid ~5461) would
    // otherwise make mem_next_allocated_leaf_base probe thousands of empty slots.
    _Atomic uint64_t leaf_bitmap[MEM_PGDIR_MID_SIZE / 64];
};

static struct pt_directory_chunk *mem_pgdir_chunk_get(struct mem *mem, page_t page) {
    if (page >= mem->page_limit)
        return NULL;
    return atomic_load_explicit(&mem->pgdir_root[PGDIR_ROOT_INDEX(page)], memory_order_acquire);
}

static struct pt_directory_chunk *mem_pgdir_chunk_new(struct mem *mem, page_t page) {
    struct pt_directory_chunk *chunk = mem_pgdir_chunk_get(mem, page);
    if (chunk != NULL)
        return chunk;

    chunk = calloc(1, sizeof(*chunk));
    if (chunk == NULL)
        return NULL;
    page_t root = PGDIR_ROOT_INDEX(page);
    atomic_store_explicit(&mem->pgdir_root[root], chunk, memory_order_release);
    // Record the root in the scan bitmap (after publishing the chunk, so any
    // observer that sees the bit and then loads the entry finds it non-NULL).
    atomic_fetch_or_explicit(&mem->pgdir_root_bitmap[root / 64],
            (uint64_t) 1 << (root % 64), memory_order_release);
    return chunk;
}

static struct pt_entry *mem_pt_leaf_get(struct mem *mem, page_t page) {
    struct pt_directory_chunk *chunk = mem_pgdir_chunk_get(mem, page);
    if (chunk == NULL)
        return NULL;
    return atomic_load_explicit(&chunk->leaves[PGDIR_MID_INDEX(page)], memory_order_acquire);
}

static struct pt_entry *mem_pt_leaf_new(struct mem *mem, page_t page) {
    struct pt_directory_chunk *chunk = mem_pgdir_chunk_new(mem, page);
    if (chunk == NULL)
        return NULL;

    _Atomic(struct pt_entry *) *slot = &chunk->leaves[PGDIR_MID_INDEX(page)];
    struct pt_entry *entries = atomic_load_explicit(slot, memory_order_acquire);
    if (entries != NULL)
        return entries;

    entries = calloc(MEM_PTDIR_SIZE, sizeof(*entries));
    if (entries == NULL)
        return NULL;
    page_t mid = PGDIR_MID_INDEX(page);
    atomic_store_explicit(slot, entries, memory_order_release);
    atomic_fetch_or_explicit(&chunk->leaf_bitmap[mid / 64],
            (uint64_t) 1 << (mid % 64), memory_order_release);
    return entries;
}

static struct pt_entry *mem_pt_raw(struct mem *mem, page_t page) {
    if (page >= mem->page_limit)
        return NULL;
    struct pt_entry *entries = mem_pt_leaf_get(mem, page);
    if (entries == NULL)
        return NULL;
    return &entries[PGDIR_LEAF_INDEX(page)];
}

static bool mem_page_range_valid(struct mem *mem, page_t start, pages_t pages) {
    if (pages == 0)
        return true;
    if (start >= mem->page_limit)
        return false;
    return pages <= mem->page_limit - start;
}

static bool mem_can_mirror_host_page_protections(void) {
    // mprotect() works at host-page granularity. Guest page flags can only be
    // mirrored into host page protections when one guest page maps exactly one
    // host page. On iOS, host pages are commonly 16K while guest pages are 4K,
    // so the emulator must rely on guest page-table checks instead.
    return real_page_size == PAGE_SIZE;
}

static bool mem_host_page_mirroring_enabled(void) {
    static int enabled = -1;
    if (enabled == -1) {
        const char *forced = getenv("ISH_ENABLE_HOST_PAGE_MIRROR");
        const char *disabled = getenv("ISH_DISABLE_HOST_PAGE_MIRROR");
        if (forced != NULL && forced[0] != '\0' && forced[0] != '0') {
            enabled = 1;
        } else if (disabled != NULL && disabled[0] != '\0' && disabled[0] != '0') {
            enabled = 0;
        } else {
#if __APPLE__
            enabled = 0;
#else
            enabled = 1;
#endif
        }
    }
    return enabled == 1;
}

static bool mem_uses_host_page_mirroring(void) {
    return mem_can_mirror_host_page_protections() && mem_host_page_mirroring_enabled();
}

bool mem_host_page_mirroring_available(void) {
    return mem_uses_host_page_mirroring();
}

static void *mem_host_page_addr(struct pt_entry *entry) {
    if (entry == NULL || entry->data == NULL || entry->data->data == NULL)
        return NULL;
    void *data = (char *) entry->data->data + entry->offset;
    return (void *) ((uintptr_t) data & ~(real_page_size - 1));
}

static size_t mem_host_page_index(struct pt_entry *entry) {
    if (entry == NULL || entry->data == NULL || entry->data->data == NULL)
        return 0;
    uintptr_t base = (uintptr_t) entry->data->data;
    uintptr_t addr = (uintptr_t) entry->data->data + entry->offset;
    return (addr - base) / real_page_size;
}

// Debug bisect knob: keep mirroring "enabled" (host_page_prot tracking,
// requires_write_revalidate) but skip the actual host mprotect calls.
static bool mem_mirror_mprotect_disabled(void) {
    static int disabled = -1;
    if (disabled == -1) {
        const char *v = getenv("ISH_MIRROR_NO_MPROTECT");
        disabled = (v != NULL && v[0] != '\0' && v[0] != '0') ? 1 : 0;
    }
    return disabled == 1;
}

static int mem_mirror_host_page_protection(struct pt_entry *entry, int flags) {
    if (!mem_uses_host_page_mirroring())
        return 0;
    if (mem_mirror_mprotect_disabled())
        return 0;
    void *data = mem_host_page_addr(entry);
    if (data == NULL)
        return 0;
    uint8_t desired = P_READ;
    if (flags & P_WRITE)
        desired |= P_WRITE;
    if (entry->data->host_page_prot != NULL) {
        size_t idx = mem_host_page_index(entry);
        if (entry->data->host_page_prot[idx] == desired)
            return 0;
    }
    int prot = PROT_READ;
    if (flags & P_WRITE)
        prot |= PROT_WRITE;
    if (mprotect(data, real_page_size, prot) < 0)
        return errno_map();
    if (entry->data->host_page_prot != NULL) {
        size_t idx = mem_host_page_index(entry);
        entry->data->host_page_prot[idx] = desired;
    }
    return 0;
}

static int mem_ensure_host_writable(struct pt_entry *entry) {
    return mem_mirror_host_page_protection(entry, P_READ | P_WRITE);
}

// Lowest root index >= `from` whose pgdir_root entry has a chunk, read from the
// set-only bitmap so empty roots are skipped 64 at a time (one word, branch on
// zero) rather than probed one 8-byte pointer at a time across 32 KiB. Returns
// MEM_PGDIR_ROOT_SIZE when there is none.
static page_t mem_next_chunk_root(struct mem *mem, page_t from) {
    if (from >= MEM_PGDIR_ROOT_SIZE)
        return MEM_PGDIR_ROOT_SIZE;
    page_t w = from / 64;
    uint64_t bits = atomic_load_explicit(&mem->pgdir_root_bitmap[w], memory_order_acquire)
            & ~(((uint64_t) 1 << (from % 64)) - 1); // ignore roots below `from`
    while (bits == 0) {
        if (++w >= MEM_PGDIR_ROOT_SIZE / 64)
            return MEM_PGDIR_ROOT_SIZE;
        bits = atomic_load_explicit(&mem->pgdir_root_bitmap[w], memory_order_acquire);
    }
    return w * 64 + (page_t) __builtin_ctzll(bits);
}

// Lowest mid index >= `from` whose leaves[] slot is populated, per the chunk's
// leaf bitmap (same bulk-skip as mem_next_chunk_root). MEM_PGDIR_MID_SIZE if none.
static page_t mem_next_leaf_mid(struct pt_directory_chunk *chunk, page_t from) {
    if (from >= MEM_PGDIR_MID_SIZE)
        return MEM_PGDIR_MID_SIZE;
    page_t w = from / 64;
    uint64_t bits = atomic_load_explicit(&chunk->leaf_bitmap[w], memory_order_acquire)
            & ~(((uint64_t) 1 << (from % 64)) - 1);
    while (bits == 0) {
        if (++w >= MEM_PGDIR_MID_SIZE / 64)
            return MEM_PGDIR_MID_SIZE;
        bits = atomic_load_explicit(&chunk->leaf_bitmap[w], memory_order_acquire);
    }
    return w * 64 + (page_t) __builtin_ctzll(bits);
}

static page_t mem_next_allocated_leaf_base(struct mem *mem, page_t page) {
    if (page >= mem->page_limit)
        return BAD_PAGE;

    // Jump straight to the next root that actually has a chunk. `mid` only keeps
    // the page's offset when we land on the page's own root; any root we skip to
    // is searched from its first leaf.
    page_t want_root = PGDIR_ROOT_INDEX(page);
    page_t root = mem_next_chunk_root(mem, want_root);
    page_t mid = (root == want_root) ? PGDIR_MID_INDEX(page) : 0;
    for (; root < MEM_PGDIR_ROOT_SIZE;
            root = mem_next_chunk_root(mem, root + 1), mid = 0) {
        // Nothing at or beyond page_limit is mapped (a 32-bit address space only
        // populates root 0), so stop rather than walk the high directory.
        if (PGDIR_LEAF_BASE(root, 0) >= mem->page_limit)
            return BAD_PAGE;
        struct pt_directory_chunk *chunk =
            atomic_load_explicit(&mem->pgdir_root[root], memory_order_acquire);
        if (chunk == NULL)
            continue; // defensive: mem_pgdir_chunk_new publishes the chunk before setting the bit
        for (mid = mem_next_leaf_mid(chunk, mid); mid < MEM_PGDIR_MID_SIZE;
                mid = mem_next_leaf_mid(chunk, mid + 1)) {
            page_t base = PGDIR_LEAF_BASE(root, mid);
            if (base >= mem->page_limit)
                return BAD_PAGE;
            struct pt_entry *entries =
                atomic_load_explicit(&chunk->leaves[mid], memory_order_acquire);
            if (entries == NULL)
                continue;
            return base;
        }
    }
    return BAD_PAGE;
}

static page_t mem_next_mapped_page(struct mem *mem, page_t page) {
    if (page >= mem->page_limit)
        return BAD_PAGE;

    page_t leaf_base = page - PGDIR_LEAF_INDEX(page);
    while (leaf_base < mem->page_limit) {
        struct pt_entry *entries = mem_pt_leaf_get(mem, leaf_base);
        if (entries == NULL) {
            leaf_base = mem_next_allocated_leaf_base(mem, page);
            if (leaf_base == BAD_PAGE)
                return BAD_PAGE;
            entries = mem_pt_leaf_get(mem, leaf_base);
            if (entries == NULL) {
                page = leaf_base + MEM_PTDIR_SIZE;
                leaf_base = page;
                continue;
            }
        }

        int start_index = leaf_base == page - PGDIR_LEAF_INDEX(page) ?
            (int) PGDIR_LEAF_INDEX(page) : 0;
        for (int i = start_index; i < MEM_PTDIR_SIZE; i++) {
            if (entries[i].data == NULL)
                continue;
            page_t mapped = leaf_base + (page_t) i;
            return mapped < mem->page_limit ? mapped : BAD_PAGE;
        }

        page = leaf_base + MEM_PTDIR_SIZE;
        if (page >= mem->page_limit)
            break;
        leaf_base = mem_next_allocated_leaf_base(mem, page);
        if (leaf_base == BAD_PAGE)
            break;
    }
    return BAD_PAGE;
}

// mmap_min_addr: Linux refuses to map anything under this (and advertises the
// same 65536 in /proc/sys/vm/mmap_min_addr, which fs/proc/sys.c already
// reports), specifically so that a NULL dereference always faults.
#define MEM_MIN_MAP_PAGE ((page_t) (65536 >> PAGE_BITS))
// How far below a MAP_GROWSDOWN region a single fault may extend it. Linux
// bounds this by RLIMIT_STACK measured from the stack top (8 MiB by default --
// see kernel/init.c's init_rlimits); this is a deliberately looser constant so
// that a guest that raises its stack limit and touches deep into a big frame
// still behaves, while a wild pointer far below the stack does not.
#define MEM_GROWSDOWN_MAX_PAGES ((page_t) ((32 * 1024 * 1024) >> PAGE_BITS))

// Linux keeps a gap between the stack and whatever is mapped below it --
// stack_guard_gap, 256 pages, enforced by expand_downwards():
//
//     if (address - prev->vm_end < stack_guard_gap) return -ENOMEM;
//
// The bound above is on the DISTANCE TO THE NEIGHBOUR ABOVE, and a runaway
// recursion never trips it: each fault lands one page below the page the last
// fault mapped, so the gap is always one page and growth is always allowed.
// Nothing then stops the stack descending until it reaches whatever is mapped
// beneath it -- and at that point it does not fault at all. Those pages are
// present and writable, so the frames simply land IN the neighbouring mapping
// and quietly destroy it.
//
// On a 64-bit guest that costs nothing, because the stack is the lowest thing
// in a mostly empty address space. On i386 the address space is 4 GiB and
// crowded: the stack starts just under 0xffffe000 and musl's thread block sits
// around 0xf7ffc000, ~134 MiB below. gnulib's "checking for working
// sigaltstack" probe recurses without bound to provoke a stack overflow, and
// AOK walked the stack straight through that thread block. The SIGSEGV, when
// it finally came, was delivered correctly onto the alternate stack -- and the
// handler died on its first libc call, because the thread pointer it needed
// had been overwritten by the recursion. Measured: %gs:0 read 0xf7ffcc64
// before the fault and 0x00400080 inside the handler.
//
// That is what took iSH-AOK down during a Buildroot build (issue #521): the
// probe is the first thing in a gnulib configure that overflows a stack on
// purpose, and every package configured after it inherited the crash.
#define MEM_STACK_GUARD_GAP_PAGES ((page_t) ((1024 * 1024) >> PAGE_BITS))

static page_t next_mapped_page_with_reservation(struct mem *mem, page_t page);

// May this unmapped page be materialized by extending a MAP_GROWSDOWN region
// (i.e. the stack) down onto it?
//
// Linux only extends a growsdown VMA for a fault *close below* it:
// expand_downwards() refuses anything under mmap_min_addr, and anything more
// than RLIMIT_STACK below the stack. iSH had no bound at all -- any unmapped
// page whose next mapped neighbour *anywhere* above it happened to be
// growsdown was mapped on demand. That is catastrophic in the 64-bit guest
// layouts, where the stack (~4 GiB) is the LOWEST mapping in the address space
// and the executable and every library sit far above it (0x7fff_....): every
// address below the stack -- the whole NULL page included -- silently
// allocated zero-filled memory instead of faulting. A guest NULL dereference
// read zeros and a guest NULL store SUCCEEDED, so instead of an immediate,
// obvious SIGSEGV at the offending instruction the guest wandered on with
// corrupted state and crashed somewhere else entirely.
//
// Found from a foot(1) render-worker crash on device: foot passed a NULL
// pixman_image_t to pixman_image_fill_rectangles(), and rather than faulting
// on the first field read, _pixman_image_validate() read a stale
// non-NULL "transform" out of the auto-mapped NULL page (written earlier by
// some other NULL store) and faulted three loads later on a garbage pointer,
// with nothing in the report pointing back at the actual NULL.
static bool mem_growsdown_allowed(struct mem *mem, page_t page) {
    if (page < MEM_MIN_MAP_PAGE)
        return false;
    page_t p = mem_next_mapped_page(mem, page + 1);
    if (p == BAD_PAGE || p >= mem->page_limit)
        return false;
    if (p - page > MEM_GROWSDOWN_MAX_PAGES)
        return false;
    struct pt_entry *next = mem_pt(mem, p);
    if (next == NULL || !(next->flags & P_GROWSDOWN))
        return false;

    // ...the stack may not grow past RLIMIT_STACK, which is Linux's other
    // half of this test:
    //
    //     if (size > rlimit(RLIMIT_STACK)) return -ENOMEM;   // size = vm_end - address
    //
    // Without it the guard gap alone bounds the stack by WHERE ITS NEIGHBOUR
    // HAPPENS TO BE, which on a 64-bit guest is nowhere near. Measured on an
    // otherwise idle arm64 guest: a runaway recursion drove the emulator to
    // 1.42 GB of resident memory before the descent reached anything, and on
    // an iPad that is not a fault, it is the app being killed by jetsam.
    // Linux would have stopped the same program at the default 8 MB. So this
    // is not only conformance -- it is what stops any guest program with an
    // unbounded recursion from taking the whole app down with it.
    //
    // Only the main stack is ever P_GROWSDOWN (kernel/mmap.c does not honour
    // MAP_GROWSDOWN), so stack_top is unambiguous and no VMA lookup is needed.
    page_t stack_limit = atomic_load(&mem->stack_limit_pages);
    if (stack_limit != 0 && mem->stack_top != 0 && page < mem->stack_top &&
            mem->stack_top - page > stack_limit)
        return false;

    // ...and nothing may occupy the guard gap below the page being claimed.
    // A neighbour inside the gap means this fault is the stack arriving at its
    // floor, and refusing it is what turns a silent descent into another
    // mapping into the SIGSEGV the guest is entitled to.
    //
    // The stack's OWN pages do not count as that neighbour, which is the
    // clause Linux spells out as !(prev->vm_flags & VM_GROWSDOWN). They turn
    // up here whenever a frame with a large local skips over a page on its way
    // down: the jumped-over page faults later with already-grown stack sitting
    // just below it, and refusing to fill that hole would kill a program doing
    // nothing wrong. Measured, before this clause existed: a plain `gcc
    // hello.c` died at page 0xffffb with the stack's own 0xffffa one page
    // under it.
    //
    // Hence a walk rather than a single lookup -- but one that costs a single
    // lookup in the ordinary case. The gap below a growing stack is empty, so
    // the first probe returns either BAD_PAGE or a page above `page` and the
    // loop breaks immediately; only hole-filling iterates, and only past the
    // few stack pages actually below the hole. The scan skips whole
    // unallocated page-table leaves, so an empty gap is not walked page by
    // page, and the loop is bounded by the gap regardless.
    //
    // ...with_reservation, not the bare page-table scan: a lazy anonymous
    // reservation and the brk headroom hold address space with no page-table
    // entries behind them. Growing the stack into either would map pages that
    // are already spoken for -- and a page both mapped and reserved is exactly
    // the state the lazy-mapping invariant forbids.
    page_t gap_bottom = page > MEM_STACK_GUARD_GAP_PAGES + MEM_MIN_MAP_PAGE
        ? page - MEM_STACK_GUARD_GAP_PAGES
        : MEM_MIN_MAP_PAGE;
    page_t scan = gap_bottom;
    while (scan < page) {
        page_t occupied = next_mapped_page_with_reservation(mem, scan);
        if (occupied == BAD_PAGE || occupied >= page)
            break;                      // open space the rest of the way down
        struct pt_entry *entry = mem_pt(mem, occupied);
        // A reservation has no page-table entry behind it and is never the
        // stack, so a NULL here is a blocker rather than something to skip.
        if (entry == NULL || !(entry->flags & P_GROWSDOWN))
            return false;
        scan = occupied + 1;
    }
    return true;
}

void mem_init(struct mem *mem) {
    mem->pgdir_root = calloc(MEM_PGDIR_ROOT_SIZE, sizeof(*mem->pgdir_root));
    if (mem->pgdir_root == NULL)
        die("calloc pgdir_root failed");
    mem->pgdir_root_bitmap = calloc(MEM_PGDIR_ROOT_SIZE / 64, sizeof(*mem->pgdir_root_bitmap));
    if (mem->pgdir_root_bitmap == NULL)
        die("calloc pgdir_root_bitmap failed");
    mem->page_limit = MEM_DEFAULT_PAGE_LIMIT;
    mem->mmap_floor = MEM_DEFAULT_MMAP_FLOOR;
    mem->mmap_ceiling = MEM_DEFAULT_MMAP_CEILING;
    // MUST be cleared here. mm_copy does `*new_mm = *mm` -- a whole-struct
    // copy -- and then calls this on the child, so without it the child
    // inherits the parent's reservations. pt_copy_on_write has meanwhile
    // materialised those same ranges into real COW entries, leaving the child
    // with a range that is both mapped AND reserved: a later fault
    // re-materialises over the copied pages with the reservation's original
    // flags. That broke every fork-heavy workload -- the e2e suite's gcc step
    // first.
    memset(mem->lazy, 0, sizeof(mem->lazy));
    mem->lazy_count = 0;
    // Same reason as mem->lazy above: mm_copy copies the whole struct and then
    // calls this on the child, so an inherited pointer here would be a double
    // free of the parent's array and a double close of its descriptors.
    mem->deferred_fds = NULL;
    mem->deferred_count = mem->deferred_cap = 0;
    pthread_mutex_init(&mem->deferred_lock, NULL);
    // Cleared for the same mm_copy reason as the two above, and restored by
    // mm_copy immediately after this returns -- see mem_set_stack_bounds.
    mem->stack_top = 0;
    atomic_init(&mem->stack_limit_pages, 0);
    atomic_init(&mem->quiesce_requested, 0);
    pthread_mutex_init(&mem->quiesce_park_lock, NULL);
    pthread_cond_init(&mem->quiesce_park_cond, NULL);
    mem->mmu.ops = &mem_mmu_ops;
    mem->mmu.requires_write_revalidate = mem_uses_host_page_mirroring() ||
        arm64_watch_enabled() ||
        // Debug bisect knob: force the JIT write slow path even with
        // mirroring off, to separate its effect from the mprotect mirroring.
        (getenv("ISH_FORCE_WRITE_REVALIDATE") != NULL);
#if ENGINE_JIT
    mem->mmu.jit = jit_new(&mem->mmu);
#endif
    // Seed each new address space with a unique change id so a per-thread TLB
    // flushes even if malloc reuses the same mmu address after exec/exit.
    atomic_store_explicit(&mem->mmu.changes,
            atomic_fetch_add_explicit(&next_mem_change_id, 1, memory_order_relaxed),
            memory_order_relaxed);
    wrlock_init(&mem->lock);
    pthread_mutex_init(&mem->pt_alloc_lock, NULL);
    strlcpy(mem->lock.lname, "mem", sizeof(mem->lock.lname));
    atomic_store_explicit(&mem->reference.count, 0, memory_order_relaxed);
    mem->reference.ready_to_be_freed = false;
}

void mem_destroy(struct mem *mem) {
    write_lock(&mem->lock);
#if ENGINE_JIT
    // Hold this across both the invalidation sweep below and jit_free --
    // dropping it in between would reopen the exact race it exists to close.
    // See jit_teardown_lock's comment for why full-teardown invalidation
    // needs this and ordinary partial munmap doesn't.
    //
    // Deliberately NEVER unlocked: jit_free frees the jit struct (the lock
    // lives inside it), so an unlock after jit_free is a use-after-free --
    // observed as an abort in jit_teardown_unlock on device, faulting at
    // jit + jetsam_lock's offset inside recycled memory. The pre-teardown-
    // lock code had the same "die locked" shape (jit_free acquired the
    // write lock and freed while holding it); by mem_destroy time the mm
    // refcount is zero, so no thread can legitimately be waiting on it.
    (void) jit_teardown_lock(mem->mmu.jit);
#endif
    pt_unmap_always(mem, 0, mem->page_limit);
    // The parked closes have to run somewhere, and this is the last chance:
    // nothing else will ever unlock this mem (the write lock above and the
    // JIT teardown lock are both deliberately never released -- see their
    // comments), so the usual drain in mem_write_unlock_with_pokes cannot
    // happen. Holding those locks across the closes is harmless here for the
    // reason they are held at all: this runs when the mm's refcount has hit
    // zero, so no thread can still be using this address space.
    //
    // What is NOT harmless is waiting forever. A ->close reached from here
    // may be a FUSE flush, and whatever would have answered it -- if it was a
    // thread of this process -- is already gone. mm_teardown makes that wait
    // bounded; `exiting` alone would not, because execve reaches here with
    // the task very much alive.
    bool saved_teardown = current != NULL && current->mm_teardown;
    if (current != NULL)
        current->mm_teardown = true;
    mem_close_deferred_fds(mem);
    if (current != NULL)
        current->mm_teardown = saved_teardown;
    pthread_mutex_destroy(&mem->deferred_lock);

#if ENGINE_JIT
    jit_free(mem->mmu.jit);
#endif
    for (size_t root = 0; root < MEM_PGDIR_ROOT_SIZE; root++) {
        struct pt_directory_chunk *chunk =
            atomic_load_explicit(&mem->pgdir_root[root], memory_order_acquire);
        if (chunk == NULL)
            continue;
        for (size_t mid = 0; mid < MEM_PGDIR_MID_SIZE; mid++) {
            struct pt_entry *entries =
                atomic_load_explicit(&chunk->leaves[mid], memory_order_acquire);
            free(entries);
        }
        free(chunk);
    }
    free(mem->pgdir_root);
    mem->pgdir_root = NULL;
    free(mem->pgdir_root_bitmap);
    mem->pgdir_root_bitmap = NULL;

    write_unlock_and_destroy(&mem->lock);
    pthread_mutex_destroy(&mem->pt_alloc_lock);
    pthread_mutex_destroy(&mem->quiesce_park_lock);
    pthread_cond_destroy(&mem->quiesce_park_cond);
}

void mem_set_page_limit(struct mem *mem, page_t limit) {
    if (limit > MEM_MAX_PAGE_LIMIT)
        limit = MEM_MAX_PAGE_LIMIT;
    mem->page_limit = limit;
}

void mem_set_mmap_window(struct mem *mem, page_t floor, page_t ceiling) {
    mem->mmap_floor = floor;
    mem->mmap_ceiling = ceiling;
}

// Records the main stack's extent, for the RLIMIT_STACK half of
// mem_growsdown_allowed. `limit_bytes` is RLIMIT_STACK's soft limit, with
// RLIM_INFINITY passed as 0 by the caller to mean "no rlimit bound" -- the
// guard gap still applies in that case. A `top` of 0 leaves the recorded top
// alone, so setrlimit can update the limit without knowing the layout.
void mem_set_stack_bounds(struct mem *mem, page_t top, uint64_t limit_bytes) {
    if (top != 0)
        mem->stack_top = top;
    atomic_store(&mem->stack_limit_pages, (page_t) (limit_bytes >> PAGE_BITS));
}

static struct pt_entry *mem_pt_new(struct mem *mem, page_t page) {
    if (page >= mem->page_limit)
        return NULL;
    struct pt_entry *entries = mem_pt_leaf_new(mem, page);
    if (entries == NULL)
        return NULL;
    return &entries[PGDIR_LEAF_INDEX(page)];
}

struct pt_entry *mem_pt(struct mem *mem, page_t page) {
    struct pt_entry *entry = mem_pt_raw(mem, page);
    if (entry == NULL || entry->data == NULL)
        return NULL;
    return entry;
}

// Reverse-map a host address that faulted during JIT execution back to the
// guest address it backs. Walks the address space's page table looking for the
// mapped guest page whose host backing range [data->data+offset, +PAGE_SIZE)
// contains host_addr; on success stores the guest address in *guest_out.
//
// Safe to call from a host signal / Mach-exception handler: it takes no locks
// and only performs the same atomic page-table loads the lockless proc/maps
// walkers already use. The fault this exists to translate -- a store to a
// past-EOF page of a file-backed mapping whose backing file was truncated --
// never mutates iSH's page table (only the host file shrinks), so the faulting
// entry is stable while we search for it.
bool mem_host_addr_to_guest(struct mem *mem, void *host_addr, guest_addr_t *guest_out) {
    if (mem == NULL || host_addr == NULL)
        return false;
    uintptr_t target = (uintptr_t) host_addr;
    for (page_t page = 0; page < mem->page_limit; mem_next_page(mem, &page)) {
        struct pt_entry *entry = mem_pt(mem, page);
        if (entry == NULL || entry->data == NULL || entry->data->data == NULL)
            continue;
        uintptr_t base = (uintptr_t) entry->data->data + entry->offset;
        if (target >= base && target < base + PAGE_SIZE) {
            *guest_out = ((guest_addr_t) page << PAGE_BITS) + (target - base);
            return true;
        }
    }
    return false;
}

static void mem_pt_del(struct mem *mem, page_t page) {
    struct pt_entry *entry = mem_pt_raw(mem, page);
    if (entry == NULL)
        return;
    // An entry with no data has no swap state either. This matters because the
    // entry is not freed, only emptied: leaves are immortal, so this slot comes
    // back as some future mapping's page. Leaving PT_SWAPPED behind would make
    // that future page read as evicted to every engine at once. pt_map
    // re-establishes the same invariant on the way in; this is the resting
    // state on the way out.
    atomic_store_explicit(&entry->swap_state, PT_RESIDENT, memory_order_relaxed);
    entry->data = NULL;
}

void mem_next_page(struct mem *mem, page_t *page) {
    (*page)++;
    if (*page >= mem->page_limit) {
        *page = mem->page_limit;
        return;
    }
    if (mem_pt_leaf_get(mem, *page) != NULL)
        return;
    page_t next = mem_next_allocated_leaf_base(mem, *page);
    if (next == BAD_PAGE) {
        *page = mem->page_limit;
        return;
    }
    *page = next;
}

// Count the pages with a live entry. Guest-visible through /proc/<pid>/stat,
// statm and status, and through ru_maxrss, so top/htop/ps pay this every
// refresh.
//
// The walk is driven by pgdir_root_bitmap and leaf_bitmap rather than by a
// linear probe of every slot, which is what it used to do. Page-table chunks
// and leaves are immortal (nothing below mem_destroy ever frees one), so the
// linear version's cost was set by the process's HIGH-WATER footprint and never
// came back down: measured at 4.9-11.6 ms per pass for a 4 GiB address space
// with 512 KiB touched, and still 4.0-7.1 ms per pass after all of it was
// unmapped, at 236 kB resident -- almost all of it spent loading the 64 KiB of
// leaves[] pointers behind each allocated chunk to find a handful of non-NULL
// ones. Reading 1 KiB of bitmap per chunk instead brings that to roughly 38 us
// per allocated chunk.
//
// The bitmaps are SET-ONLY: a bit says a leaf exists, never that anything in it
// is mapped. So this can skip empty regions in bulk but still has to look at
// every entry of every leaf it lands on, and the count it returns is unchanged.
//
// `resident_only` skips entries the pager has evicted; see
// mem_resident_page_count. One walk rather than two copies of it, because the
// bitmap skipping is the only interesting part and it should exist once.
static size_t mem_page_count_walk(struct mem *mem, bool resident_only) {
    if (mem == NULL)
        return 0;
    // Hoisted: a function call per live entry of the address space otherwise.
    const size_t frame = mem_frame_size();

    size_t count = 0;
    for (page_t root = mem_next_chunk_root(mem, 0); root < MEM_PGDIR_ROOT_SIZE;
            root = mem_next_chunk_root(mem, root + 1)) {
        // Roots ascend, so the first one whose lowest page is past the limit
        // ends the walk -- a 32-bit address space only ever populates root 0.
        if (PGDIR_LEAF_BASE(root, 0) >= mem->page_limit)
            return count;
        struct pt_directory_chunk *chunk =
            atomic_load_explicit(&mem->pgdir_root[root], memory_order_acquire);
        if (chunk == NULL)
            continue; // defensive: mem_pgdir_chunk_new publishes the chunk before setting the bit
        for (page_t mid = mem_next_leaf_mid(chunk, 0); mid < MEM_PGDIR_MID_SIZE;
                mid = mem_next_leaf_mid(chunk, mid + 1)) {
            struct pt_entry *entries =
                atomic_load_explicit(&chunk->leaves[mid], memory_order_acquire);
            if (entries == NULL)
                continue;
            page_t base = PGDIR_LEAF_BASE(root, mid);
            if (base >= mem->page_limit)
                return count;
            size_t limit = MEM_PTDIR_SIZE;
            if (base + MEM_PTDIR_SIZE > mem->page_limit)
                limit = (size_t) (mem->page_limit - base);
            for (size_t i = 0; i < limit; i++) {
                struct data *data = entries[i].data;
                if (data == NULL)
                    continue;
                if (resident_only &&
                        atomic_load_explicit(&entries[i].swap_state,
                                memory_order_relaxed) != PT_RESIDENT) {
                    // The entry only claims "not here", and that claim is a
                    // conservative HINT (struct pt_entry::swap_state). Ask the
                    // frame, which owns the slot: a sibling entry's fault --
                    // or a FORKED SIBLING ADDRESS SPACE's -- brings the frame
                    // back and publishes only its own entries, leaving these
                    // saying SWAPPED over memory that is resident and holding
                    // the right bytes.
                    //
                    // MEASURED: evict 8 MiB, fork, let the child read it all
                    // back, then sample the parent -- 2048 pages reported out
                    // while every one of those host frames was mprotect(RW)
                    // and resident. A residency figure that wrong is not a
                    // rounding error, it is the number /proc would print.
                    //
                    // A NULL frame_slot means the mapping was never evictable,
                    // so its bytes are in host memory by definition.
                    if (data->frame_slot != NULL &&
                            atomic_load_explicit(
                                &data->frame_slot[entries[i].offset / frame],
                                memory_order_relaxed) != SWAP_SLOT_NONE)
                        continue;   // really out
                    // NOT written back into the entry. This walk holds no mem
                    // lock -- proc_ish_update_swap_evict calls it after the
                    // barrier has been released -- and the evictor may be
                    // releasing that frame right now. PT_RESIDENT over a
                    // released frame is the one direction that must never
                    // happen. Reconciliation belongs where a write lock is
                    // held.
                }
                count++;
            }
        }
    }
    return count;
}

size_t mem_mapped_page_count(struct mem *mem) {
    return mem_page_count_walk(mem, false);
}

// Mapped pages minus the ones the pager has evicted.
//
// WHAT THIS IS. With the per-frame slot record in place, a page whose frame
// holds a slot is definitively not in host memory, which is the first real
// residency signal this tree has ever had. The per-entry swap_state byte is
// NOT that signal -- it is a conservative hint, and this walk uses it only to
// decide which entries are worth asking the frame about. Every "resident" figure AOK reports today --
// /proc/<pid>/status VmRSS and VmHWM, statm field 2, stat field 24, smaps and
// smaps_rollup Rss, getrusage's ru_maxrss, /proc/meminfo AnonPages and Mapped
// -- is mem_mapped_page_count, i.e. a count of mapped ADDRESS SPACE.
//
// WHAT IT STILL OVER-REPORTS, and nobody should mistake this for a true
// residency measure until it does not. AOK builds page-table entries EAGERLY at
// mmap() time: for file mappings in host_fd_mmap, and for anonymous mappings
// below the 64 MiB lazy threshold in pt_map_nothing. No pt_entry field records
// whether a page was ever TOUCHED, so an untouched mapping is counted here in
// full. Measured: an untouched 1 GiB file mapping takes VmRSS from 2124 kB to
// 1050700 kB. Only the lazy reservations above MEM_LAZY_MIN_PAGES escape it,
// and they escape it by having no entries at all rather than by being measured.
//
// WHY THAT MATTERS. docs/simulated_swap_plan.md section 11 records that
// "MemTotal := the app's jetsam budget" was deferred precisely on this: while
// MemTotal is the machine's RAM the over-reporting is invisible, but the moment
// MemTotal becomes a few hundred MiB every one of those figures can exceed it,
// which is a state Linux cannot produce. Bounding each surface against MemTotal
// independently was tried and rejected four times, because it replaces an
// impossible value with an inconsistent pair. Subtracting swapped pages is the
// half of the fix that can be built now; the other half is a touched bit, and
// until it exists this function is honestly named "resident" only relative to
// what the pager has taken away.
//
// Cost is mem_mapped_page_count's, plus one relaxed byte load per live entry.
// Relaxed is right: a page being evicted or faulted back concurrently may be
// counted either way, and no caller of a whole-address-space page count can
// mean anything more precise than that.
size_t mem_resident_page_count(struct mem *mem) {
    return mem_page_count_walk(mem, true);
}

// Return the first page >= page with no mapping, or page_limit if every page
// up to the limit is mapped. Scans leaf entry arrays directly so walking a
// large contiguous mapped region doesn't redo the page-table descent per page.
static page_t mem_next_unmapped_page(struct mem *mem, page_t page) {
    while (page < mem->page_limit) {
        page_t leaf_base = page - PGDIR_LEAF_INDEX(page);
        struct pt_entry *entries = mem_pt_leaf_get(mem, leaf_base);
        if (entries == NULL)
            return page;
        for (int i = (int) PGDIR_LEAF_INDEX(page); i < MEM_PTDIR_SIZE; i++) {
            if (entries[i].data == NULL) {
                page_t unmapped = leaf_base + (page_t) i;
                return unmapped < mem->page_limit ? unmapped : mem->page_limit;
            }
        }
        page = leaf_base + MEM_PTDIR_SIZE;
    }
    return mem->page_limit;
}

// ---- lazy anonymous reservations ---------------------------------------
// See struct mem_lazy_map in emu/memory.h for the contract. The rule that
// makes this safe: a page inside a reservation has NO page-table entry, so
// every reader that reads "no entry" as "not mapped" must consult these ranges
// too. Those readers are the hole finder, the fault paths, and /proc/pid/maps.

static bool lazy_trace(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("ISH_LAZY_TRACE"); on = (e && *e == '1'); }
    return on == 1;
}
#define LAZY_TRACE(...) do { if (lazy_trace()) fprintf(stderr, "[lazy] " __VA_ARGS__); } while (0)

struct mem_lazy_map *mem_lazy_find(struct mem *mem, page_t page) {
    for (unsigned i = 0; i < mem->lazy_count; i++) {
        struct mem_lazy_map *l = &mem->lazy[i];
        if (l->start < l->end && page >= l->start && page < l->end)
            return l;
    }
    return NULL;
}

bool mem_lazy_overlaps(struct mem *mem, page_t start, page_t end) {
    for (unsigned i = 0; i < mem->lazy_count; i++) {
        struct mem_lazy_map *l = &mem->lazy[i];
        if (l->start < l->end && start < l->end && l->start < end)
            return true;
    }
    return false;
}

// True iff dropping [start, end) would leave a reservation on BOTH sides --
// the one case the no-split invariant forbids. Callers materialise instead.
bool mem_lazy_would_split(struct mem *mem, page_t start, page_t end) {
    for (unsigned i = 0; i < mem->lazy_count; i++) {
        struct mem_lazy_map *l = &mem->lazy[i];
        if (l->start < l->end && start > l->start && end < l->end)
            return true;
    }
    return false;
}

bool mem_lazy_reserve(struct mem *mem, page_t start, pages_t pages, unsigned flags) {
    if (pages < MEM_LAZY_MIN_PAGES)
        return false;
    // A new mapping REPLACES whatever occupied these pages, so existing
    // reservation coverage has to go first. Without this the range ends up
    // covered twice with different flags and mem_lazy_find returns whichever
    // came first -- which is how the JVM died: it reserves its heap PROT_NONE
    // and commits sub-ranges RW with MAP_FIXED, the commit added a second
    // overlapping reservation, and a later fault materialised the PROT_NONE
    // one over the committed pages. SEGV_ACCERR at the first write.
    if (mem->lazy_count != 0) {
        if (mem_lazy_would_split(mem, start, start + pages))
            mem_lazy_materialize_range(mem, start, start + pages);
        else
            mem_lazy_drop(mem, start, start + pages);
    }
    for (unsigned i = 0; i < MEM_LAZY_MAX; i++) {
        struct mem_lazy_map *l = &mem->lazy[i];
        if (i >= mem->lazy_count || l->start >= l->end) {
            l->start = start; l->end = start + pages; l->flags = flags;
            if (i >= mem->lazy_count)
                mem->lazy_count = i + 1;
            LAZY_TRACE("reserve [%llx,%llx) flags=%#x\n",
                       (unsigned long long) start, (unsigned long long) (start + pages), flags);
            return true;
        }
    }
    return false;   // table full: caller maps eagerly, i.e. today's behaviour
}

// Pure range math, no mapping. Safe to call with any lock held. Only whole
// coverage and front/back trims are possible here -- mem_lazy_would_split must
// have been checked by the caller.
void mem_lazy_drop(struct mem *mem, page_t start, page_t end) {
    for (unsigned i = 0; i < mem->lazy_count; i++) {
        struct mem_lazy_map *l = &mem->lazy[i];
        if (l->start >= l->end || end <= l->start || l->end <= start)
            continue;
        if (start <= l->start && end >= l->end)
            l->start = l->end = 0;
        else if (start <= l->start)
            l->start = end;
        else if (end >= l->end)
            l->end = start;
        // the both-sides case cannot occur; see mem_lazy_would_split
    }
}

void mem_lazy_materialize_range(struct mem *mem, page_t start, page_t end) {
    // Bounded by MEM_LAZY_MAX, never by the page count: this is called from
    // fork's COW pass with the WHOLE address space, and a per-page scan there
    // took a one-second compile to 216 seconds.
    for (unsigned i = 0; i < mem->lazy_count; i++) {
        struct mem_lazy_map *l = &mem->lazy[i];
        if (l->start >= l->end || end <= l->start || l->end <= start)
            continue;
        page_t s = l->start, e = l->end;
        unsigned flags = l->flags;
        LAZY_TRACE("materialize_range [%llx,%llx) for req [%llx,%llx) flags=%#x\n",
                   (unsigned long long) s, (unsigned long long) e,
                   (unsigned long long) start, (unsigned long long) end, flags);
        l->start = l->end = 0;          // clear BEFORE mapping: pt_map_nothing
                                        // must not see this range as reserved
        pt_map_nothing(mem, s, e - s, flags);
    }
}

// Fault handler. Materialises [l->start, end of the chunk holding `page`) and
// trims the front -- a reservation only ever shrinks from the left, never
// splits. Caller holds the write lock.
static bool mem_lazy_fault(struct mem *mem, page_t page) {
    struct mem_lazy_map *l = mem_lazy_find(mem, page);
    if (l == NULL)
        return false;
    page_t s = l->start;
    page_t e = page + MEM_LAZY_CHUNK_PAGES;
    e -= (e - l->start) % MEM_LAZY_CHUNK_PAGES;   // round up to a chunk edge
    if (e <= page)
        e = page + 1;
    if (e > l->end)
        e = l->end;
    unsigned flags = l->flags;
    LAZY_TRACE("fault page=%llx -> materialize [%llx,%llx) flags=%#x (res was [%llx,%llx))\n",
               (unsigned long long) page, (unsigned long long) s, (unsigned long long) e,
               flags, (unsigned long long) l->start, (unsigned long long) l->end);
    if (e >= l->end)
        l->start = l->end = 0;      // consumed entirely
    else
        l->start = e;               // front trim
    return pt_map_nothing(mem, s, e - s, flags) == 0;
}

// True iff `mem` currently has an active brk-headroom reservation (see
// struct mem's brk_reserve_start/end) that overlaps [start, start+pages).
// A plain range check -- O(1), no page-table walk -- since the reservation
// is bookkeeping, not real page-table entries.
static bool overlaps_brk_reservation(struct mem *mem, page_t start, page_t end) {
    return mem->brk_reserve_start < mem->brk_reserve_end &&
        start < mem->brk_reserve_end && mem->brk_reserve_start < end;
}

// mem_next_mapped_page/mem_next_unmapped_page wrappers that also treat the
// brk-headroom reservation range as occupied, for pt_find_hole's search --
// so a fresh mmap() can't land inside headroom that's earmarked for the
// heap to grow into later. Just range math on top of the real page-table
// scan (O(1) extra), so pt_find_hole keeps its usual cost even though the
// reservation itself has no page-table entries to walk.
static page_t next_mapped_page_with_reservation(struct mem *mem, page_t page) {
    page_t real = mem_next_mapped_page(mem, page);
    // Lazy reservations occupy address space with no page-table entries, just
    // like the brk headroom below, so a fresh mmap must not land inside one.
    for (unsigned i = 0; i < mem->lazy_count; i++) {
        struct mem_lazy_map *l = &mem->lazy[i];
        if (l->start >= l->end)
            continue;
        if (page >= l->start && page < l->end)
            return page;
        if (page <= l->start && l->start < real)
            real = l->start;
    }
    if (mem->brk_reserve_start < mem->brk_reserve_end) {
        if (page <= mem->brk_reserve_start && mem->brk_reserve_start < real)
            return mem->brk_reserve_start;
        if (page >= mem->brk_reserve_start && page < mem->brk_reserve_end)
            return page;
    }
    return real;
}

static page_t next_unmapped_page_with_reservation(struct mem *mem, page_t page) {
    for (unsigned i = 0; i < mem->lazy_count; i++) {
        struct mem_lazy_map *l = &mem->lazy[i];
        if (l->start < l->end && page >= l->start && page < l->end)
            page = l->end;
    }
    if (mem->brk_reserve_start < mem->brk_reserve_end &&
            page >= mem->brk_reserve_start && page < mem->brk_reserve_end)
        page = mem->brk_reserve_end;
    return mem_next_unmapped_page(mem, page);
}

page_t pt_find_hole(struct mem *mem, pages_t size) {
    if (size == 0 || mem->mmap_ceiling <= mem->mmap_floor)
        return BAD_PAGE;
    if (size > mem->mmap_ceiling - mem->mmap_floor)
        return BAD_PAGE;

    page_t best = BAD_PAGE;
    page_t prev_end = mem->mmap_floor;
    page_t page = next_mapped_page_with_reservation(mem, mem->mmap_floor);
    while (page != BAD_PAGE && page < mem->mmap_ceiling) {
        if (page > prev_end && page - prev_end >= size)
            best = page - size;

        page_t region_end = next_unmapped_page_with_reservation(mem, page + 1);
        if (region_end > mem->mmap_ceiling)
            region_end = mem->mmap_ceiling;
        prev_end = region_end;
        page = region_end >= mem->mmap_ceiling ? BAD_PAGE
             : next_mapped_page_with_reservation(mem, region_end);
    }
    if (mem->mmap_ceiling - prev_end >= size)
        best = mem->mmap_ceiling - size;
    return best;
}

bool pt_is_hole(struct mem *mem, page_t start, pages_t pages) {
    if (!mem_page_range_valid(mem, start, pages))
        return false;
    if (mem_lazy_overlaps(mem, start, start + pages))
        return false;
    if (overlaps_brk_reservation(mem, start, start + pages))
        return false;
    for (page_t page = start; page < start + pages; page++) {
        if (mem_pt(mem, page) != NULL)
            return false;
    }
    return true;
}

// ---- exact ownership of a struct data -------------------------------------
//
// docs/simulated_swap_plan.md section 3.8, with the two corrections section 2.5
// made to it. The question the pager must answer before it releases a host
// frame is "is this struct data reachable from exactly ONE address space, and
// is that address space the one I hold the barrier on?". Everything else about
// eviction is recoverable; getting this wrong releases a frame a sibling
// process still believes it owns, and on Apple that failure is silent.
//
// `refcount` cannot answer it, and the gate prototype's approximation
// (refcount == size / PAGE_SIZE) is only right for a mapping that never forked
// AND never had a page unmapped out of it, because refcount counts page-table
// ENTRIES with no record of whose they are. So each struct data carries the
// owners itself, maintained at the FIVE and only five sites where refcount
// changes -- pt_map, pt_unmap_always_unlocked, pt_dup, pt_move and
// pt_copy_on_write. That the list is complete is not an assumption: section 2.5
// re-derived it three times independently, over every write to
// data::refcount and every assignment of a pt_entry's data field in the tree,
// and closed the back doors (there is no structural copy of entries anywhere,
// mm_copy's whole-struct copy is repaired by mem_init's fresh pgdir_root
// before the child is reachable, and mem_set_page_limit only runs on an empty
// mem). A sixth site would silently break this; there must not be one.
//
// TWO RULES make the two-slots-plus-overflow encoding exact rather than merely
// conservative, and both were found by attacking the original design:
//
//  1. A slot is never freed while overflow_entries != 0. Without it: A in slot
//     0, B in slot 1, C overflowed; A unmaps, slot 0 is freed, and the data
//     reads exclusive to B while C still maps it. That is unsafe, not
//     conservative.
//  2. While overflow_entries != 0 a newcomer NEVER takes a free slot, it goes
//     to the overflow. Otherwise one mem can end up counted in both a slot and
//     the overflow, and the release side has no way to tell which to decrement.
//
// Together they make the encoding monotone: once anything overflows, the slots
// freeze and every acquire goes to the overflow, until the overflow drains to
// zero -- at which point every overflowed owner is provably gone, empty slots
// are swept, and the record is exact again. Ordinary shell use reaches three
// owners in under a second, so that recovery is the point: a fork-and-exec
// family stops being unevictable milliseconds after the exec, instead of
// staying pinned for the life of the process.
//
// Locking: a striped mutex, taken and released with nothing else held. It is a
// LEAF -- see section 3.9's ordering -- and deliberately not "the mapping mem's
// write lock", because the pure-growth mmap fast path runs pt_map under
// pt_alloc_lock plus the mem READ lock (kernel/mmap.c). What serialises that
// path against the swap barrier is pt_alloc_lock, which
// mem_write_lock_with_pokes takes first; this lock only has to make each
// record's own read-modify-write atomic. Measured cost of the whole hook (lock,
// slot compare, two increments, unlock): 7.6 ns, which is 0.6-1.1% of a guest
// fork.
#define DATA_OWNER_STRIPES 64
// Statically initialised rather than built in a constructor: PTHREAD_MUTEX_
// INITIALIZER is not all-zero on Darwin, and a constructor would put this
// path's correctness at the mercy of initialiser ordering.
#define DATA_OWNER_L1 PTHREAD_MUTEX_INITIALIZER
#define DATA_OWNER_L4 DATA_OWNER_L1, DATA_OWNER_L1, DATA_OWNER_L1, DATA_OWNER_L1
#define DATA_OWNER_L16 DATA_OWNER_L4, DATA_OWNER_L4, DATA_OWNER_L4, DATA_OWNER_L4
static pthread_mutex_t data_owner_locks[DATA_OWNER_STRIPES] = {
    DATA_OWNER_L16, DATA_OWNER_L16, DATA_OWNER_L16, DATA_OWNER_L16,
};
#undef DATA_OWNER_L16
#undef DATA_OWNER_L4
#undef DATA_OWNER_L1

static pthread_mutex_t *data_owner_lock_for(struct data *data) {
    // Mix two shifts: a struct data is malloc'd, so the low bits are alignment
    // padding and the allocator hands out runs whose next-higher bits move
    // together. One shift alone put whole mappings' worth of neighbours on the
    // same stripe.
    uintptr_t key = (uintptr_t) data;
    key = (key >> 4) ^ (key >> 12);
    return &data_owner_locks[key % DATA_OWNER_STRIPES];
}

size_t mem_frame_size(void) {
    return real_page_size > PAGE_SIZE ? real_page_size : PAGE_SIZE;
}

static size_t mem_frame_count(size_t size) {
    size_t frame = mem_frame_size();
    return (size + frame - 1) / frame;
}

// Adjust the per-frame entry counts for the `pages` entries covering data
// offsets [offset, offset + pages * PAGE_SIZE). Caller holds the stripe lock,
// or owns a struct data no other thread can reach yet (see data_owner_init).
//
// Counts saturate at 255 and, once saturated, are never changed again: the true
// count is unrecoverable at that point, and a frame whose count is unknown must
// read as unevictable forever rather than as evictable once by accident. 255
// entries in one 16 KiB frame is 64 address spaces sharing it, four guest pages
// each -- a fork family far past anything phase 1 would evict anyway, since it
// evicts exclusively-owned data only.
#define DATA_FRAME_REFS_SATURATED 255
static void data_frame_refs_add_locked(struct data *data, size_t offset,
                                       pages_t pages, int delta) {
    if (data->frame_refs == NULL || pages == 0)
        return;
    size_t frame = mem_frame_size();
    size_t frames = mem_frame_count(data->size);
    // Walk FRAMES, not pages: a run covering a whole frame contributes the same
    // amount to every page of it, so the four (or one) pages of a frame are one
    // add rather than four. Offsets are always guest-page multiples -- pt_map's
    // `offset` argument is host_fd_mmap's page-aligned `correction` (fs/real.c)
    // and zero everywhere else -- so a frame's share of the run is just how
    // many of the run's pages fall inside it.
    size_t first = offset;
    size_t last = offset + (size_t) (pages - 1) * PAGE_SIZE;   // inclusive
    for (size_t f = first / frame; f <= last / frame; f++) {
        if (f >= frames)
            return;     // past the end of the mapping; nothing to count
        size_t frame_start = f * frame;
        size_t frame_end = frame_start + frame;               // exclusive
        size_t lo = first > frame_start ? first : frame_start;
        size_t hi = last + PAGE_SIZE < frame_end ? last + PAGE_SIZE : frame_end;
        unsigned n = (unsigned) ((hi - lo) / PAGE_SIZE);
        uint8_t *slot = &data->frame_refs[f];
        // Saturated stays saturated: the true count is unrecoverable once it
        // does, and a frame whose count is unknown must read as unevictable for
        // ever rather than as evictable once by accident.
        if (*slot == DATA_FRAME_REFS_SATURATED)
            continue;
        if (delta > 0) {
            unsigned sum = *slot + n;
            *slot = sum >= DATA_FRAME_REFS_SATURATED ?
                    DATA_FRAME_REFS_SATURATED : (uint8_t) sum;
        } else {
            *slot = n >= *slot ? 0 : (uint8_t) (*slot - n);
        }
    }
}

// pages_t is 64-bit and an entry count is 32-bit, so clamp rather than
// truncate. A single mapping of more than 2^32 guest pages is 16 TiB and no
// guest ABI's page_limit reaches it, but a truncation here would be a wrap to a
// small number -- and a wrap is the one arithmetic error that can turn "shared"
// into "exclusive". Clamping can only ever over-count, which reads as
// not-exclusive, which refuses to evict.
static uint32_t data_owner_count(pages_t pages) {
    return pages > UINT32_MAX ? UINT32_MAX : (uint32_t) pages;
}

// Seed the records for a brand-new struct data whose entire mapping is about to
// be published into one mem. pt_map only.
//
// No lock, and that is safe for a reason worth stating rather than assuming:
// `data` is a local malloc that no page-table entry names yet, so no other
// thread has any way to reach it. The publication loop that follows is what
// makes it reachable, and every reader of these fields either holds that mem's
// lock or, in the evictor's case, the address-space barrier -- both of which
// order after the publication of the first entry. Taking the stripe lock here
// instead would hold a lock shared with 1/64 of every other mapping in the
// process across a 262144-iteration loop for a 1 GiB mmap.
static void data_owner_init(struct data *data, struct mem *mem, size_t offset, pages_t pages) {
    if (pages == 0)
        return;
    data->owners[0].mem = mem;
    data->owners[0].entries = data_owner_count(pages);
    data->n_owners = 1;
    data->overflow_entries = 0;
    data_frame_refs_add_locked(data, offset, pages, +1);
}

// Record that `pages` more page-table entries of `mem`, covering data offsets
// [offset, offset + pages * PAGE_SIZE), now point at `data`.
static void data_owner_acquire(struct data *data, struct mem *mem, size_t offset, pages_t pages) {
    if (data == NULL || pages == 0)
        return;
    pthread_mutex_t *stripe = data_owner_lock_for(data);
    pthread_mutex_lock(stripe);
    unsigned i;
    for (i = 0; i < data->n_owners; i++) {
        if (data->owners[i].mem == mem) {
            data->owners[i].entries += data_owner_count(pages);
            break;
        }
    }
    if (i == data->n_owners) {
        // Rule 2: while anything is overflowed, a newcomer goes to the
        // overflow even when a slot is free, so no mem is ever counted in both
        // a slot and the overflow.
        if (data->overflow_entries != 0 || data->n_owners >= 2) {
            data->overflow_entries += data_owner_count(pages);
        } else {
            data->owners[data->n_owners].mem = mem;
            data->owners[data->n_owners].entries = data_owner_count(pages);
            data->n_owners++;
        }
    }
    data_frame_refs_add_locked(data, offset, pages, +1);
    pthread_mutex_unlock(stripe);
}

// The inverse. `mem` may legitimately have no slot: it is then one of the
// overflowed owners, which are counted but not named.
static void data_owner_release(struct data *data, struct mem *mem, size_t offset, pages_t pages) {
    if (data == NULL || pages == 0)
        return;
    pthread_mutex_t *stripe = data_owner_lock_for(data);
    pthread_mutex_lock(stripe);
    unsigned i;
    for (i = 0; i < data->n_owners; i++)
        if (data->owners[i].mem == mem)
            break;
    if (i < data->n_owners) {
        // The clamps are defensive, not expected: a release with no matching
        // acquire would mean a sixth refcount site exists. Saturating beats
        // wrapping a uint32 into "billions of owners", which reads as
        // permanently-not-exclusive and would be invisible.
        if (data->owners[i].entries >= data_owner_count(pages))
            data->owners[i].entries -= data_owner_count(pages);
        else
            data->owners[i].entries = 0;
    } else if (data->overflow_entries >= data_owner_count(pages)) {
        data->overflow_entries -= data_owner_count(pages);
    } else {
        data->overflow_entries = 0;
    }
    // Rule 1: empty slots are swept only once the overflow is empty, because
    // only then is every unnamed owner provably gone. This is also what makes
    // the record recover: the sweep is how a forked-and-exec'd mapping becomes
    // exclusive again.
    if (data->overflow_entries == 0) {
        unsigned kept = 0;
        for (unsigned j = 0; j < data->n_owners; j++)
            if (data->owners[j].entries != 0)
                data->owners[kept++] = data->owners[j];
        data->n_owners = (uint8_t) kept;
    }
    data_frame_refs_add_locked(data, offset, pages, -1);
    pthread_mutex_unlock(stripe);
}

bool data_is_exclusive_to(struct data *data, struct mem *mem) {
    if (data == NULL || mem == NULL)
        return false;
    pthread_mutex_t *stripe = data_owner_lock_for(data);
    pthread_mutex_lock(stripe);
    bool exclusive = data->overflow_entries == 0 &&
                     data->n_owners == 1 &&
                     data->owners[0].mem == mem &&
                     data->owners[0].entries != 0;
    pthread_mutex_unlock(stripe);
    return exclusive;
}

int data_frame_ref_count(struct data *data, size_t offset) {
    if (data == NULL || data->frame_refs == NULL)
        return -1;
    size_t f = offset / mem_frame_size();
    if (f >= mem_frame_count(data->size))
        return -1;
    pthread_mutex_t *stripe = data_owner_lock_for(data);
    pthread_mutex_lock(stripe);
    uint8_t count = data->frame_refs[f];
    pthread_mutex_unlock(stripe);
    return count == DATA_FRAME_REFS_SATURATED ? -1 : (int) count;
}

// A run of consecutive page-table entries of ONE struct data at CONSECUTIVE
// data offsets, so the ownership record is updated once per run instead of once
// per page.
//
// It exists because data_owner_acquire/release take the struct data's stripe
// mutex, and the four sites that maintain the record are all per-page loops
// over whole address spaces -- every munmap, every exec teardown, every process
// exit, every fork, every mremap. One lock and one unlock per page of those is
// a cost paid by every guest whether or not swap is ever enabled.
//
// MEASURED, interleaved A/B, median of 7 on a quiet machine: mmap+munmap of a
// 512 MiB file mapping on a guest with swap off and nothing ever evicted.
//
//   6.2 ms  without the ownership records at all
//   8.3 ms  with them, updated per page       (+34%, 16 ns a page -- one
//                                              uncontended lock/unlock pair)
//   6.5 ms  with them, updated per run        (+5%)
//
// The residual is the per-page run bookkeeping and two callocs per mapping, on
// what is deliberately an extreme case: 131072 pages torn down at once.
//
// The offset test is not decoration. pt_move copies `offset` verbatim, so
// guest-adjacent entries are not necessarily offset-adjacent, and
// data_frame_refs_add_locked walks a run by stepping the offset -- a run that
// lied about contiguity would count the wrong frames.
//
// Deferring the update is safe in both directions, and for different reasons.
// A deferred RELEASE leaves the record over-counting, which reads as "shared",
// which refuses to evict. A deferred ACQUIRE would leave it under-counting,
// which is the dangerous direction -- but the only site that acquires across
// address spaces is pt_copy_on_write, which runs under the SOURCE's
// mem_write_lock_with_pokes barrier for its whole loop, and whose destination
// is a child mm no task can reach yet. So no evictor can observe either.
struct data_owner_run {
    struct data *data;
    size_t offset;
    pages_t pages;
};

static void data_owner_run_flush(struct data_owner_run *run, struct mem *mem, int delta) {
    if (run->pages == 0)
        return;
    if (delta > 0)
        data_owner_acquire(run->data, mem, run->offset, run->pages);
    else
        data_owner_release(run->data, mem, run->offset, run->pages);
    run->pages = 0;
}

static void data_owner_run_add(struct data_owner_run *run, struct mem *mem,
                               struct data *data, size_t offset, int delta) {
    if (run->pages != 0 &&
            (run->data != data ||
             offset != run->offset + (size_t) run->pages * PAGE_SIZE))
        data_owner_run_flush(run, mem, delta);
    if (run->pages == 0) {
        run->data = data;
        run->offset = offset;
    }
    run->pages++;
}

int pt_map(struct mem *mem, page_t start, pages_t pages, void *memory, size_t offset, unsigned flags) {
    if (!mem_page_range_valid(mem, start, pages))
        return _ENOMEM;
    if (memory == MAP_FAILED)
        return errno_map();

    // Anything mapped here stops being a lazy reservation, or the range ends
    // up BOTH mapped and reserved -- and a later fault elsewhere in that
    // reservation would re-materialise over these pages with the
    // reservation's original flags.
    //
    // That is exactly how the JVM died: it reserves its heap PROT_NONE and
    // commits sub-ranges with MAP_FIXED PROT_READ|WRITE, and a subsequent
    // fault reset those committed pages to PROT_NONE. The crash log said
    // SEGV_ACCERR -- mapped, wrong permission -- which is what pointed here.
    //
    // The materialise branch re-enters pt_map exactly once: it clears the
    // reservation before mapping, so the inner call finds nothing to drop.
    if (mem->lazy_count != 0) {
        if (mem_lazy_would_split(mem, start, start + pages))
            mem_lazy_materialize_range(mem, start, start + pages);
        else
            mem_lazy_drop(mem, start, start + pages);
    }

    // If this fails, the munmap in pt_unmap would probably fail.
    assert(memory == NULL || (uintptr_t) memory % real_page_size == 0 || memory == vdso_data);

    struct data *data = malloc(sizeof(struct data));
    if (data == NULL)
        return _ENOMEM;
    *data = (struct data) {
        .data = memory,
        .size = pages * PAGE_SIZE + offset,
        .shared_key = 0,
        .cache_entry = NULL,

#if LEAK_DEBUG
        .pid = current ? current->pid : 0,
        .dest = start << PAGE_BITS,
#endif
    };
    if (mem_uses_host_page_mirroring() && memory != NULL && memory != vdso_data) {
        size_t host_pages = (data->size + real_page_size - 1) / real_page_size;
        data->host_page_prot = malloc(host_pages);
        if (data->host_page_prot == NULL) {
            free(data);
            return _ENOMEM;
        }
        // 0 = unknown: the memory arriving here does NOT have a uniform host
        // protection -- anonymous maps are PROT_READ|PROT_WRITE but
        // host_fd_mmap file maps carry the guest's (often read-only)
        // protection. This array only exists to skip redundant mprotects, so
        // seed it with a value that can never match a real request and let
        // the first mem_mirror_host_page_protection call set the truth.
        // (Seeding P_READ|P_WRITE made mem_ensure_host_writable skip the
        // mprotect that would have actually granted write on a read-only
        // file mapping, leaving the guest to fault forever on a page its
        // own page tables call writable.)
        memset(data->host_page_prot, 0, host_pages);
    }
    // One byte per host frame, counting the entries that point into it -- see
    // struct data::frame_refs. Allocated for anything that has host memory to
    // release; NULL for the two cases that never can (an unbacked PROT_NONE
    // reservation, and the vdso's static array), where "unknown" is also the
    // right answer.
    //
    // A failure here is NOT an mmap failure. host_page_prot above is different:
    // its absence would silently mis-mirror protections. frame_refs' absence
    // only makes the mapping unevictable, so failing the guest's mmap over it
    // would trade a small feature for a large one.
    if (memory != NULL && memory != vdso_data) {
        size_t frames = mem_frame_count(data->size);
        data->frame_refs = calloc(frames, 1);
        // The frame's own record of where its bytes are. All-zero IS the
        // correct initial state, because SWAP_SLOT_NONE is 0 and the allocator
        // never hands out slot 0 -- so calloc alone says "every frame of this
        // mapping is in host memory", with no loop over the frames on a path
        // that runs whether or not swap is ever enabled.
        data->frame_slot = calloc(frames, sizeof(*data->frame_slot));
        // The two are used as a pair and are meaningless apart: frame_refs
        // says a frame may be released, frame_slot says where its bytes went.
        // Half of the pair is worse than neither, so a partial allocation
        // drops both and the mapping is simply never evicted.
        if (data->frame_refs == NULL || data->frame_slot == NULL) {
            free(data->frame_refs);
            free(data->frame_slot);
            data->frame_refs = NULL;
            data->frame_slot = NULL;
        }
    }

    // Allocate every page-table leaf the range needs BEFORE publishing a single
    // entry. This is what makes the loop below infallible, and it is the last
    // thing in this function that can fail.
    //
    // Publication has to be infallible rather than reversible, because a
    // rollback that unpublished entries would break the safety argument the
    // pure-growth mmap fast path rests on. mem_growth_lock (kernel/mmap.c)
    // holds only pt_alloc_lock and the mem READ lock, so the sibling threads
    // keep running and keep resolving guest addresses through this page table
    // while pt_map runs; what makes that safe is stated there, and it is that
    // growth only ever publishes new chunks and entries and frees nothing.
    // Removing an entry already published would violate exactly that: a sibling
    // can have resolved it into a host pointer a microsecond earlier, and
    // pt_map_nothing munmaps the host range on the error return. The sibling
    // would not even be told to re-resolve, because emu/tlb.h only flushes a
    // cached translation when mmu.changes moves. (The pre-change code was safe
    // here only because it leaked: it left the entries published and munmapped
    // nothing.)
    //
    // Allocating leaves is the one page-table mutation that path already
    // permits. Leaves and chunks are immortal -- mem_destroy is the only place
    // in this file that frees one -- and mem_pt_leaf_new hands back an existing
    // leaf instead of replacing it, so this pre-pass is idempotent and the
    // mem_pt_new below is just a re-fetch. An empty leaf left behind when this
    // fails is harmless: the bitmaps are set-only and every walker already
    // tolerates an all-NULL leaf.
    //
    // One call per LEAF, not per page: mem_pt_new allocates the whole
    // 1024-entry leaf at once, so touching every page here would add a third
    // page-table descent per page to a loop that already does two, for no
    // effect after the first page of each leaf. A 1 GiB anonymous mmap is
    // 262144 pages and 256 leaves.
    for (page_t page = start; page < start + pages;
            page = (page | (MEM_PTDIR_SIZE - 1)) + 1) {
        if (mem_pt_new(mem, page) == NULL) {
            // Nothing was published, so this is the complete teardown: fd, name
            // and cache_entry are filled in by the caller only AFTER a
            // successful return, and `memory` stays the caller's (see the
            // ownership contract in emu/memory.h). host_page_prot in particular
            // was leaked by the old rollback, which called free(data) alone.
            free(data->host_page_prot);
            free(data->frame_refs);
            free(data->frame_slot);
            free(data);
            return _ENOMEM;
        }
    }

    // Ownership site 1 of 5. Before the publication loop, not inside it: `data`
    // is still a local nothing else can reach, so this needs no lock (see
    // data_owner_init), and the loop below cannot fail -- every allocation this
    // function can fail on is already done.
    data_owner_init(data, mem, offset, pages);

    for (page_t page = start; page < start + pages; page++) {
        if (mem_pt(mem, page) != NULL)
            pt_unmap(mem, page, 1);
        data->refcount++;
        // Cannot be NULL: both of mem_pt_new's failure conditions are already
        // excluded. The page is below page_limit, because mem_page_range_valid
        // checked the whole range at the top and page_limit is set once when
        // the mm is created (kernel/mmap.c, mm_new/mm_copy) and never moves.
        // And its leaf exists, because the pre-pass allocated one per leaf and
        // nothing frees a leaf while the mem is alive -- the pt_unmap just
        // above releases entries, never the leaf that holds them.
        struct pt_entry *pt = mem_pt_new(mem, page);
        // Not an assert: assert compiles out under NDEBUG, and the comment
        // above is a list of invariants a future change could break. A release
        // build that broke one would get a NULL dereference on the next line
        // instead of a diagnosable stop naming the page.
        if (pt == NULL)
            die("pt_map: leaf vanished at page %llx after the pre-pass allocated it",
                (unsigned long long) page);
        // Swap state is reset EXPLICITLY, which docs/simulated_swap_plan.md
        // section 3.3 requires and which the gate prototype did not do. A leaf
        // is calloc'd once and never freed (nothing below mem_destroy frees
        // one), so a pt_entry is a slot that gets REUSED: unmap a page that had
        // been evicted, map something new at the same guest address, and the
        // fresh page would inherit PT_SWAPPED and read as absent to every
        // engine at once. mem_pt_del clears it too; both, because this one is
        // the invariant the entry is published with and that one is the
        // invariant an unmapped entry rests at.
        atomic_store_explicit(&pt->swap_state, PT_RESIDENT, memory_order_relaxed);
        pt->data = data;
        pt->offset = ((page - start) << PAGE_BITS) + offset;
        pt->flags = flags;
    }
    mem_changed(mem);
    return 0;
}

int pt_unmap(struct mem *mem, page_t start, pages_t pages) {
    if (!mem_page_range_valid(mem, start, pages))
        return -1;
    for (page_t page = start; page < start + pages; page++)
        if (mem_pt(mem, page) == NULL && mem_lazy_find(mem, page) == NULL)
            return -1;
    return pt_unmap_always(mem, start, pages);
}

// ISH_MEM_QUARANTINE=1: when guest page data is released, mprotect it
// PROT_NONE and keep the address range, instead of munmapping it.
//
// This exists because of how the failure looks WITHOUT it. Task threads take
// their 4 MB stacks from the same host arena this memory goes back to
// (kernel/task.c, pthread_attr_setstacksize), so a page AOK unmaps can come
// straight back as the next thread's `_pthread` struct. A write through a
// mem_ptr() result that outlived its mapping therefore does not fault -- it
// quietly overwrites a live thread's bookkeeping, and the bill arrives later,
// on a different thread, inside pthread_exit. See the pread_stack_thread_race
// entry in docs/TODO.md for the crash report that led here.
//
// With the knob on, that write faults at the instruction that makes it, while
// the guilty frames are still on the stack. It leaks address space by design,
// so it is for a debugging run and nothing else.
static bool mem_quarantine_freed_pages(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *v = getenv("ISH_MEM_QUARANTINE");
        enabled = (v != NULL && *v != '\0' && *v != '0') ? 1 : 0;
    }
    return enabled == 1;
}

// Core of pt_unmap_always, without the jetsam_lock exclusion -- callers that
// already know no sibling thread can be executing JIT code in `mem` (e.g.
// mm_copy building a brand-new, not-yet-started child mm; see
// pt_copy_on_write) can use this directly to avoid a lock acquire/release
// per page. Callers for a live, potentially-multithreaded mem must go
// through pt_unmap_always instead.
// Park a descriptor for mem_close_deferred_fds. See struct mem's deferred_fds.
//
// The drain happens at the next mem_write_unlock_with_pokes, or at
// mem_destroy, whichever comes first -- so a descriptor parked by a path that
// releases the write lock some other way (the COW break in mem_ptr upgrades
// and downgrades in place, and cannot drain while still holding the read
// lock) waits for the process's next address-space change. That is a delayed
// close, never a lost one: teardown always drains.
static void mem_defer_fd_close(struct mem *mem, struct fd *fd) {
    pthread_mutex_lock(&mem->deferred_lock);
    if (mem->deferred_count == mem->deferred_cap) {
        unsigned cap = mem->deferred_cap == 0 ? 4 : mem->deferred_cap * 2;
        struct fd **grown = realloc(mem->deferred_fds, cap * sizeof(*grown));
        if (grown == NULL) {
            // Out of memory for a four-pointer array means the process is
            // finished anyway. Close it here rather than leak the descriptor;
            // the hang this defers is the lesser risk at that point.
            pthread_mutex_unlock(&mem->deferred_lock);
            fd_close(fd);
            return;
        }
        mem->deferred_fds = grown;
        mem->deferred_cap = cap;
    }
    mem->deferred_fds[mem->deferred_count++] = fd;
    pthread_mutex_unlock(&mem->deferred_lock);
}

void mem_close_deferred_fds(struct mem *mem) {
    // Take the whole list before closing anything: a ->close can itself unmap
    // (a filesystem freeing a mapping of its own), and would otherwise be
    // appending to the array being walked.
    pthread_mutex_lock(&mem->deferred_lock);
    struct fd **fds = mem->deferred_fds;
    unsigned count = mem->deferred_count;
    mem->deferred_fds = NULL;
    mem->deferred_count = mem->deferred_cap = 0;
    pthread_mutex_unlock(&mem->deferred_lock);
    for (unsigned i = 0; i < count; i++)
        fd_close(fds[i]);
    free(fds);
}

// The last page-table entry of a mapping has gone: give the host memory back
// and free the struct.
static void data_destroy(struct mem *mem, struct data *data) {
    // vdso wasn't allocated with mmap, it's just in our data segment
    if (data->data != NULL && data->data != vdso_data) {
        if (mem_quarantine_freed_pages()) {
            // Debug mode: keep the range reserved and make it fault rather
            // than handing it back. See the helper above.
            if (mprotect(data->data, data->size, PROT_NONE) != 0)
                die("quarantine mprotect(%p, %lu) failed: %s",
                    data->data, data->size, strerror(errno));
        } else {
            int err = munmap(data->data, data->size);
            if (err != 0)
                die("munmap(%p, %lu) failed: %s", data->data, data->size, strerror(errno));
        }
    }
    if (data->fd != NULL) {
        // The last reference to this mapping is going away; if it was holding
        // a memfd's write seal off, stop holding it.
        if (data->memfd_shared_mapped)
            memfd_mapping_released(data->fd);
        // NOT fd_close: this runs under the address-space write lock with the
        // other threads quiesced, and a ->close may block on one of them. See
        // struct mem's deferred_fds.
        mem_defer_fd_close(mem, data->fd);
    }
    mmap_cache_unregister(data->cache_entry);
    // Every frame of this mapping that was still out has just lost its last
    // page-table entry, so nothing can ever fault it back. Its slot would
    // otherwise be leaked for the life of the process -- and the slot pool is a
    // fixed size the user chose, so leaking there is a pager that quietly stops
    // being able to evict.
    if (data->frame_slot != NULL) {
        size_t frames = mem_frame_count(data->size);
        for (size_t f = 0; f < frames; f++) {
            uint32_t slot = atomic_load_explicit(&data->frame_slot[f],
                    memory_order_relaxed);
            if (slot != SWAP_SLOT_NONE)
                swap_slot_free(slot);
        }
    }
    free(data->host_page_prot);
    free(data->frame_refs);
    free(data->frame_slot);
    free(data);
}

// Settle one run of unmapped entries: release their ownership record, then drop
// their refcount references, and tear the mapping down if that was the last of
// them.
//
// THE ORDER IS THE WHOLE POINT, and getting it wrong is a use-after-free that
// only a shared mapping can reach. `refcount` is what keeps `data` alive, and a
// run holds `pages` of those references until this runs -- so the record must
// be settled BEFORE they are given up. Dropping them first leaves a window in
// which ANOTHER address space's unmap of the same struct data takes the count
// to zero and frees it, and this thread then reads a freed struct.
//
// MEASURED: with the release deferred past the decrement, the arm64 regression
// suite died 6 times in 20 in mem_break_cow_group -> pt_map -> pt_unmap with a
// corrupted malloc freelist; under guard malloc the same run faults directly in
// data_owner_release on the freed struct. HEAD and the uncoalesced version are
// 0 in 20.
static void data_unmap_run_settle(struct data_owner_run *run, struct mem *mem) {
    if (run->pages == 0)
        return;
    struct data *data = run->data;
    size_t offset = run->offset;
    pages_t pages = run->pages;
    data_owner_run_flush(run, mem, -1);

    // Reclaim the slots of any frame this run just emptied -- BEFORE the
    // refcount references are given up, and that ordering is the whole point.
    //
    // The mapping may well survive: a frame whose last page-table entry just
    // went can never be faulted back, because there is no entry left to fault,
    // so its slot would sit allocated until the whole struct data is freed,
    // which for a long-lived mapping is never. The slot pool is a fixed size
    // the user chose, so leaking into it is a pager that quietly stops being
    // able to evict, and a partial munmap, an mremap that shrinks and
    // MADV_DONTNEED over part of a mapping all reach it -- as glibc's
    // malloc_trim, jemalloc's purge and Go's runtime all do routinely.
    //
    // Doing it AFTER the refcount drop, which is where this started, is a
    // use-after-free: once our references are gone another address space
    // unmapping the same struct data can take the count to zero and free it,
    // and the frame walk then reads a freed struct. Same trap as the one
    // data_unmap_run_settle exists to avoid, one line further down. It cost a
    // SIGSEGV in the arm64 regression suite.
    //
    // The frame counts are already correct here, because data_owner_run_flush
    // above is what decremented them. A count of zero says no address space has
    // an entry in this frame, so no fault can be racing us for it, and the
    // exchange makes the free happen once even if two address spaces empty
    // different frames of the same mapping at the same moment.
    if (data->frame_slot != NULL) {
        size_t frame = mem_frame_size();
        size_t last = offset + (size_t) (pages - 1) * PAGE_SIZE;
        for (size_t f = offset / frame; f <= last / frame; f++) {
            if (data_frame_ref_count(data, f * frame) != 0)
                continue;
            uint32_t slot = atomic_exchange_explicit(&data->frame_slot[f],
                    SWAP_SLOT_NONE, memory_order_acq_rel);
            if (slot != SWAP_SLOT_NONE)
                swap_slot_free(slot);
        }
    }

    if (atomic_fetch_sub(&data->refcount, (unsigned) pages) == (unsigned) pages)
        data_destroy(mem, data);
}

static int pt_unmap_always_unlocked(struct mem *mem, page_t start, pages_t pages) {
    if (!mem_page_range_valid(mem, start, pages))
        return -1;
    page_t end = start + pages;
    // Ownership site 2 of 5, coalesced -- see struct data_owner_run. The
    // refcount decrements are coalesced with it, and must be: see
    // data_unmap_run_settle.
    struct data_owner_run run = {};
    for (page_t page = mem_next_mapped_page(mem, start);
         page != BAD_PAGE && page < end;
         page = mem_next_mapped_page(mem, page + 1)) {
        struct pt_entry *pt = mem_pt(mem, page);
        if (pt == NULL)
            continue;
#if ENGINE_JIT
        jit_invalidate_page(mem->mmu.jit, page);
#endif
        struct data *data = pt->data;
        // Read the offset before mem_pt_del: it is the frame this entry was
        // counted against, and mem_pt_del is entitled to clear anything it
        // likes out of the entry.
        size_t owner_offset = pt->offset;
        mem_pt_del(mem, page);
        // A discontinuity ends the run, and ending it settles it -- the run
        // owns those refcount references until then.
        if (run.pages != 0 &&
                (run.data != data ||
                 owner_offset != run.offset + (size_t) run.pages * PAGE_SIZE))
            data_unmap_run_settle(&run, mem);
        if (run.pages == 0) {
            run.data = data;
            run.offset = owner_offset;
        }
        run.pages++;
    }
    data_unmap_run_settle(&run, mem);
    mem_changed(mem);
    return 0;
}

int pt_unmap_always(struct mem *mem, page_t start, pages_t pages) {
    // Reservations are handled HERE, outside the JIT invalidate lock taken
    // below, because materialising maps and pt_map_nothing takes that same
    // lock. A hole punched through the middle of a reservation is the one
    // case the no-split invariant forbids, so materialise it in full and let
    // the ordinary path below tear it down.
    if (mem_lazy_would_split(mem, start, start + pages))
        mem_lazy_materialize_range(mem, start, start + pages);
    else
        mem_lazy_drop(mem, start, start + pages);
#if ENGINE_JIT
    // Exclude CLONE_VM sibling threads still executing chained JIT code
    // before disconnecting blocks below -- see jit_invalidate_lock's
    // comment for the SIGBUS-during-munmap this closes.
    bool jit_locked = jit_invalidate_lock(mem->mmu.jit);
#endif
    int ret = pt_unmap_always_unlocked(mem, start, pages);
#if ENGINE_JIT
    if (jit_locked)
        jit_invalidate_unlock(mem->mmu.jit);
#endif
    return ret;
}

int pt_map_nothing(struct mem *mem, page_t start, pages_t pages, unsigned flags) {
    if (pages == 0) return 0;
    if (pages > SIZE_MAX / PAGE_SIZE)
        return _ENOMEM;
    if ((flags & P_RWX) == 0)
        return pt_map(mem, start, pages, NULL, 0, flags | P_ANONYMOUS);
    int mmap_flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_NORESERVE
    // Guest anonymous mappings are often just virtual reservations
    // (for example PROT_NONE arenas). Do not force the host to reserve
    // backing store up front for the entire range.
    mmap_flags |= MAP_NORESERVE;
#endif
    void *memory = mmap(NULL, pages * PAGE_SIZE,
            PROT_READ | PROT_WRITE, mmap_flags, 0, 0);
    int err = pt_map(mem, start, pages, memory, 0, flags | P_ANONYMOUS);
    // pt_map only takes ownership of `memory` when it succeeds (emu/memory.h),
    // so give the host mapping back here or it is leaked for the life of the
    // process. MAP_FAILED is the one case with nothing to release: pt_map
    // turned it into an errno without ever looking at it.
    //
    // Safe to munmap even on the growth fast path, where sibling threads are
    // running concurrently under the read lock, precisely because pt_map fails
    // only before it publishes anything (see the leaf pre-pass there): no
    // page-table entry ever named this range, so no sibling can be holding a
    // pointer into it.
    if (err < 0 && memory != MAP_FAILED)
        munmap(memory, pages * PAGE_SIZE);
    return err;
}

// Alias a range at a second address: same backing, same offsets, both live.
// This is pt_move without the unmap of the source -- which is exactly what
// mremap's old_len == 0 form asks for, and Linux allows only for a shared
// mapping, because aliasing a private one would silently break its privacy.
//
// The caller is responsible for that check; this routine will alias anything
// it is handed.
int pt_dup(struct mem *mem, page_t old_start, page_t new_start, pages_t pages) {
    if (!mem_page_range_valid(mem, old_start, pages) ||
            !mem_page_range_valid(mem, new_start, pages))
        return _ENOMEM;
    if (!pt_is_hole(mem, new_start, pages))
        return _ENOMEM;
    for (page_t page = old_start; page < old_start + pages; page++)
        if (mem_pt(mem, page) == NULL)
            return _EFAULT;

    pages_t mapped = 0;
    struct data_owner_run run = {};
    for (; mapped < pages; mapped++) {
        struct pt_entry *src = mem_pt(mem, old_start + mapped);
        struct pt_entry *dst = mem_pt_new(mem, new_start + mapped);
        if (dst == NULL) {
            // Settle the run BEFORE the unmap: pt_unmap_always releases what
            // this loop acquired, and it cannot release a record that has not
            // been written yet.
            data_owner_run_flush(&run, mem, +1);
            pt_unmap_always(mem, new_start, mapped);
            return _ENOMEM;
        }
        src->data->refcount++;
        // Ownership site 3 of 5. Same mem, so this only ever bumps the existing
        // slot -- but it is a real second entry against the same frame, which is
        // exactly the aliasing frame_refs exists to see. Without it, a data
        // aliased twice inside ONE address space still reads as exclusive, and
        // the evictor would release a frame with a live second entry pointing
        // at it. (Today pt_dup is reached only for P_SHARED mappings, which are
        // never evicted -- that is a coincidence of the caller in kernel/mmap.c,
        // not a property of this function, which "will alias anything it is
        // handed".) Coalesced -- see struct data_owner_run.
        data_owner_run_add(&run, mem, src->data, src->offset, +1);
        dst->data = src->data;
        dst->offset = src->offset;
        dst->flags = src->flags;
        // The alias must carry the swap state, or one of the two entries would
        // claim a PROT_NONE frame is resident and a read through it would take
        // a HOST fault in emulator C code, where nothing turns it into a guest
        // signal. The SLOT is not carried, because the slot is not the entry's
        // to carry -- it belongs to the frame both entries now describe (struct
        // data::frame_slot).
        atomic_store_explicit(&dst->swap_state,
                atomic_load_explicit(&src->swap_state, memory_order_acquire),
                memory_order_release);
    }
    data_owner_run_flush(&run, mem, +1);
    mem_changed(mem);
    return 0;
}

int pt_move(struct mem *mem, page_t old_start, page_t new_start, pages_t pages) {
    if (!mem_page_range_valid(mem, old_start, pages) ||
            !mem_page_range_valid(mem, new_start, pages))
        return _ENOMEM;
    if (!pt_is_hole(mem, new_start, pages))
        return _ENOMEM;
    for (page_t page = old_start; page < old_start + pages; page++)
        if (mem_pt(mem, page) == NULL)
            return _EFAULT;

    pages_t mapped = 0;
    struct data_owner_run run = {};
    for (; mapped < pages; mapped++) {
        struct pt_entry *src = mem_pt(mem, old_start + mapped);
        struct pt_entry *dst = mem_pt_new(mem, new_start + mapped);
        if (dst == NULL) {
            // Settle the run BEFORE the unmap, which releases what this loop
            // acquired and cannot release a record not yet written.
            data_owner_run_flush(&run, mem, +1);
            pt_unmap_always(mem, new_start, mapped);
            return _ENOMEM;
        }
        src->data->refcount++;
        // Ownership site 4 of 5. The matching release comes from the pt_unmap
        // of the source below, or from the pt_unmap_always on the failure path.
        // Coalesced -- see struct data_owner_run.
        data_owner_run_add(&run, mem, src->data, src->offset, +1);
        dst->data = src->data;
        dst->offset = src->offset;
        dst->flags = src->flags;
        // Carried for the same reason as in pt_dup: a moved entry describes the
        // same host frame -- pt_move copies `data` and `offset` verbatim, so
        // only the GUEST address changed -- and it must agree with the source
        // about whether that frame is currently released.
        //
        // This is the case that made the slot move to the frame. When the
        // entry carried its own copy of the slot, a page mremap'd out of an
        // evicted frame kept that copy, its three siblings were published
        // resident by an ordinary fault, the guest wrote them, and then the
        // moved page's own fault re-read the whole frame from the slot and
        // reverted those writes. Now the frame's first fault clears
        // frame_slot, and this entry's later fault finds it already resident
        // and does nothing but publish.
        atomic_store_explicit(&dst->swap_state,
                atomic_load_explicit(&src->swap_state, memory_order_acquire),
                memory_order_release);
    }
    data_owner_run_flush(&run, mem, +1);

    int err = pt_unmap(mem, old_start, pages);
    if (err < 0) {
        pt_unmap_always(mem, new_start, pages);
        return err;
    }
    mem_changed(mem);
    return 0;
}

// Give a shared, still-unbacked struct data its host memory -- once, for every
// mapper at the same time.
//
// A MAP_SHARED PROT_NONE anonymous region is reserved with data == NULL and one
// struct data that every mapper's page-table entry points at. The first side to
// make it accessible used to allocate a page for ITSELF, ending the sharing at
// the exact moment the mapping first became usable: two processes that
// mprotected such a region after fork each got a private page and never saw
// each other's writes again. Doing it here, in the shared struct, keeps every
// mapper resolving to the same memory.
//
// Its own lock, because the struct is reachable from more than one process's
// page tables while each of those processes holds only its OWN memory lock.
static int mem_materialize_shared_data(struct data *data) {
    static lock_t shared_materialize_lock = LOCK_INITIALIZER;
    lock(&shared_materialize_lock, 0);
    int err = 0;
    if (data->data == NULL) {
        void *memory = mmap(NULL, data->size, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (memory == MAP_FAILED) {
            err = errno_map();
        } else {
            // Every guest process is a thread of one host process, so host
            // MAP_PRIVATE memory is already common to all of them -- what makes
            // this shared is that they reach it through the same struct data.
            if (mem_uses_host_page_mirroring()) {
                size_t host_pages = (data->size + real_page_size - 1) / real_page_size;
                data->host_page_prot = calloc(host_pages, 1);
                // A NULL host_page_prot only costs the protection cache, which
                // mem_mirror_host_page_protection already tolerates.
            }
            data->data = memory;
        }
    }
    unlock(&shared_materialize_lock);
    return err;
}

// ---- host-page packing --------------------------------------------------
//
// A guest page is 4 KiB. A host page on Apple silicon is 16 KiB, and the host
// charges a whole host page for a 4 KiB mapping: 4096 separate 4 KiB mmaps land
// with a minimum gap of exactly 16384, no two share a host page, and each costs
// 16.3 KiB resident (measured on this Mac; docs/simulated_swap_plan.md section
// 3.2). That is a 4.00x amplification of every guest page that gets a host
// allocation to itself.
//
// Five call sites in this file used to take exactly one guest page at a time:
// the copy-on-write break in mem_ptr and again in mem_ptr_fault, the
// MAP_GROWSDOWN stack extension in both, and the PROT_NONE-to-accessible commit
// in pt_set_flags. Measured before this change, on an arm64 Alpine guest that
// touches a 256 MiB private anonymous mapping, forks, and has the child rewrite
// every page of it: 1280 MB of untagged VM_ALLOCATE across 9500 host VM
// regions, for 512 MiB of guest data. 1024 MB of that is the child's 65536
// single-page mmaps at 16 KiB each; the same run with the grouping below costs
// 512 MB across 2477 regions. It is also faster, because three faults in four
// stop happening: on a 64 MiB version of the same test, interleaved A/B with
// seven samples each, the child's copy-on-write break went from a median of
// 3.03 us to 0.93 us per guest page.
//
// The allocation cannot be made smaller, so stop making one per guest page.
// Each of those sites now materialises every guest page that shares the
// faulting page's HOST page and is eligible for exactly the same treatment, in
// one pt_map: one host page, up to four guest pages, one struct data, four
// page-table entries at offsets 0, 4096, 8192 and 12288.
//
// Why a group of CONTIGUOUS guest pages, and not a slab allocator handing 4 KiB
// chunks of a host page to unrelated guest pages -- which is the shape the swap
// study (docs/simulated_swap_plan.md section 7, Phase 0) proposed. struct
// data::refcount counts page-table ENTRIES, and fs/proc/pid.c reads that as a
// sharer count: proc_smaps_region divides it by the region's page count and
// documents the invariant it depends on, that an unshared N-page region has
// refcount N. Four unrelated pages sharing one struct data breaks that -- a
// one-page region would report refcount 4, i.e. an exclusively-owned page shown
// as shared four ways with a quarter of its Pss, and the .data page a forked
// child writes to is exactly such a region. proc_maps_dump merges neighbours
// that share a struct data, so two adjacent unrelated pages that happened to
// land in one slab would also print as one region where there are two. Repairing
// either would mean changing fs/proc/pid.c. A group keeps both true by
// construction, because a group's entries ARE one contiguous region of one
// mapping, which is what refcount has always counted.
//
// The other properties this shape keeps, each of which a slab would have had to
// re-earn: teardown is unchanged (the last entry to go drops the refcount to 0
// and pt_unmap_always_unlocked munmaps data->data with data->size, exactly as
// for any other multi-page mapping), fork is unchanged (pt_copy_on_write shares
// the struct and bumps the refcount per entry), MADV_DONTNEED is unchanged (its
// pt_map_nothing over part of a group drops those entries and leaves the host
// page mapped for the siblings that survive -- it never hands back memory a
// live page is still using), and there is no new allocator state and so no new
// lock and no new lock ordering.

// Guest pages per host page, or 1 when there is nothing to pack: a host page no
// larger than a guest page (Linux x86-64) already fills exactly, and a ratio
// that is not a power of two would break the group-start mask below.
static pages_t mem_host_page_group(void) {
    if (real_page_size <= PAGE_SIZE || real_page_size % PAGE_SIZE != 0)
        return 1;
    size_t ratio = real_page_size / PAGE_SIZE;
    if ((ratio & (ratio - 1)) != 0)
        return 1;
    return (pages_t) ratio;
}

// First guest page of the host page `page` lives in.
static page_t mem_host_page_group_start(page_t page, pages_t group) {
    return page & ~(page_t) (group - 1);
}

static bool mem_page_packing_enabled(void) {
    static int disabled = -1;
    if (disabled < 0) {
        // ISH_MEM_NO_PAGE_PACKING=1 restores one mmap per guest page at these
        // sites, so an A/B run measures the packing and nothing else.
        const char *v = getenv("ISH_MEM_NO_PAGE_PACKING");
        disabled = (v != NULL && *v != '\0' && *v != '0') ? 1 : 0;
    }
    if (disabled == 1 || mem_host_page_group() == 1)
        return false;
    // ISH_MEM_QUARANTINE keeps a released page's address range and mprotects it
    // PROT_NONE so a stale host pointer faults at the instruction that makes the
    // write (see mem_quarantine_freed_pages). That only works while a released
    // guest page owns its host page: a page released out of the middle of a
    // group leaves the host page mapped for its live siblings, and mprotect
    // cannot protect 4 KiB of a 16 KiB host page. So the debug knob wins, and
    // with it on every one of these sites is a singleton again, exactly as
    // before -- a quarantine run is a debugging run and nothing else.
    return !mem_quarantine_freed_pages();
}

// May `p` join a copy-on-write group whose faulting page has flags `flags`?
//
// Flag EQUALITY, not a subset test, and that is what keeps the group invisible
// to the guest: pt_map takes one flags value for the whole range, so a
// neighbour may only join if it would have ended up with exactly those flags
// anyway. P_COW is part of the comparison, so only pages already waiting to be
// copied join. P_WRITE is too, so a read-only private page is never made
// private early, and a MEM_WRITE_PTRACE poke -- which adds P_WRITE|P_COW to the
// faulting entry alone -- always ends up a group of one, which is what a
// debugger's poke of a single page should be.
static bool mem_cow_group_member(struct mem *mem, page_t p, unsigned flags) {
    struct pt_entry *e = mem_pt(mem, p);
    if (e == NULL || e->flags != flags)
        return false;
    // A PROT_NONE anonymous reservation has no host backing to copy from. It
    // cannot reach here with P_RWX-equal flags, but mem_break_cow_group's memcpy
    // would dereference NULL if it did.
    if (e->data->data == NULL)
        return false;
    // Nor can an evicted page join, for the harder version of the same reason:
    // its host frame is mprotect(PROT_NONE), and mem_break_cow_group memcpy()s
    // straight out of `data->data + offset` with no page-table consultation, so
    // including one is a HOST fault inside emulator C code where the JIT's
    // crash recovery is not armed.
    //
    // The faulting page itself is always resident by the time the group is
    // formed -- both callers swap it in first -- but a neighbour need not be.
    // The group is built from GUEST page alignment (`page & ~(group-1)`) while
    // a host frame is aligned on the DATA OFFSET, and those differ whenever a
    // mapping's first guest page is not a multiple of four, which is the
    // ordinary case on the amd64 test root. So the group can straddle two host
    // frames, only one of which the fault brought back. Reachable by evicting a
    // page and then forking, which leaves it P_COW and SWAPPED at once -- that
    // ordering is exercised by the fork test, though this particular
    // straddling neighbour was not isolated, so this is a guard placed by
    // inspection rather than one a failure demanded.
    return atomic_load_explicit(&e->swap_state, memory_order_acquire) == PT_RESIDENT;
}

// Break copy-on-write for `page`, and for every guest page sharing its host
// page that is eligible for the identical break. Returns 0, or a negative errno
// with nothing changed.
//
// Caller holds the mem write lock and has already established that `page` is
// mapped, P_COW, and writable for this access type. Both COW sites in this file
// (mem_ptr's lock-upgrade path and mem_ptr_fault's) call this with those
// preconditions met; one function rather than two, because they were two copies
// of the same dozen lines and this would otherwise be written twice.
static int mem_break_cow_group(struct mem *mem, page_t page) {
    struct pt_entry *entry = mem_pt(mem, page);
    if (entry == NULL || entry->data->data == NULL)
        return _EFAULT;
    unsigned flags = entry->flags;

    page_t first = page, last = page;
    // P_ANONYMOUS only, and that restriction is measured rather than cautious.
    // Grouping anonymous pages costs nothing in the region list: proc_maps_dump
    // already merges adjacent anonymous entries with equal permissions whether
    // or not they share a struct data, so it is byte-identical either way
    // (verified by diffing /proc/self/maps and smaps_rollup of a forked child
    // that breaks COW in three shapes, packed against unpacked, on all five test
    // roots -- the only lines that ever differ are the [stack] extent and the
    // smaps_rollup totals that follow from it, which come from the growsdown
    // grouping below and not from here).
    //
    // What that argument does NOT buy is an invariant, and the distinction
    // matters. A group's entries are one contiguous region of one mapping AT
    // CREATION. Afterwards mprotect or mremap can split a region away from its
    // struct data while refcount still counts every entry, and then
    // proc_smaps_region's refcount/region_pages sharer estimate is wrong.
    // Measured: a forked child that writes p[0] of a 16 KiB-aligned window and
    // then mprotect(p, 4096, PROT_READ) gets smaps `r--s` with Pss 1 kB and
    // Shared_Dirty 4 kB, on a page it exclusively owns, while /proc/maps says
    // `r--p` -- a maps/smaps disagreement Linux never produces.
    //
    // That is a PRE-EXISTING defect this widens rather than a new one: the same
    // output already appears for a one-page mprotect split of any ordinary
    // multi-page anonymous mapping from pt_map_nothing, with grouping off. The
    // real repair is to make that sharer estimate count the entries of the
    // REGION rather than the refcount of the whole struct data, in
    // fs/proc/pid.c. TODO, and it fixes both instances at once.
    //
    // It is still the reason to group rather than to slab-allocate: a slab
    // breaks the same invariant the moment the group is made, unconditionally,
    // for pages that were never related to each other.
    //
    // A PRIVATE FILE mapping is the case where it would show, and what it would
    // show is an existing defect made wider rather than a new one: AOK's COW
    // break gives the copy a struct data with no fd and no name, so a broken
    // page of a file mapping already prints in /proc/maps as a nameless region
    // splitting the file's own. Grouping would take the name off up to three
    // more pages per host page -- pages the guest never wrote. Measured on a
    // 32-page private mapping of /bin/busybox with every fourth page written
    // after a fork: 16 alternating named/nameless regions became 8 nameless
    // ones. Linux prints one named region for all of it, so neither is right;
    // the repair is to carry fd, name and file_offset across the break, and
    // until that happens this stays out of it. The memory at stake is small --
    // the writable file-backed segments of an executable and its libraries,
    // tens of pages per process, against the hundreds of megabytes of anonymous
    // COW this is for.
    if (mem_page_packing_enabled() && (flags & P_ANONYMOUS)) {
        pages_t group = mem_host_page_group();
        page_t base = mem_host_page_group_start(page, group);
        while (first > base && mem_cow_group_member(mem, first - 1, flags))
            first--;
        while (last + 1 < base + group && mem_cow_group_member(mem, last + 1, flags))
            last++;
    }
    pages_t pages = (pages_t) (last - first + 1);
    size_t bytes = (size_t) pages * PAGE_SIZE;

    void *copy = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
    if (copy == MAP_FAILED)
        return errno_map();
    // Every source is read BEFORE pt_map publishes anything. pt_map unmaps the
    // old entry of each page as it goes, and that can drop the last reference to
    // the very struct data being copied from -- for a group whose pages all come
    // from one parent mapping, unmapping page `first` can munmap the memory page
    // `first + 1` still needs to be read out of.
    for (page_t p = first; p <= last; p++) {
        struct pt_entry *src = mem_pt(mem, p);
        memcpy((char *) copy + ((size_t) (p - first) << PAGE_BITS),
               (char *) src->data->data + src->offset, PAGE_SIZE);
    }
    int err = pt_map(mem, first, pages, copy, 0, flags & ~P_COW);
    if (err < 0) {
        // pt_map maps nothing when it fails, so the copy is still ours to
        // release, and the pages it was for are all still copy-on-write.
        munmap(copy, bytes);
        return err;
    }
    return 0;
}

// Extend a MAP_GROWSDOWN region onto `page`, and onto the rest of its host page
// where that is allowed. Caller holds the mem write lock and has already
// checked mem_growsdown_allowed(mem, page).
//
// The care this needs that the COW break does not: mapping a page the guest has
// not touched must never pre-empt a SIGSEGV it was entitled to. So a page below
// the faulting one joins only if it satisfies mem_growsdown_allowed on its own
// account -- which is the test that enforces mmap_min_addr, RLIMIT_STACK and
// the stack guard gap, and is exactly what a later fault on that page would
// have had to pass. RLIMIT_STACK in particular still bounds the stack at the
// same guest ADDRESS it did before, because that test is on the address and not
// on a page count.
//
// This is the one packed site the guest can see, and what it sees is a [stack]
// region in /proc/maps whose low edge is up to three guest pages below the
// deepest address actually touched, with Rss and Anonymous 12 kB higher to
// match (measured on the glibc guest: one 4 kB step, ffffd000 -> ffffc000).
// That is not a state Linux cannot produce -- a 16 KiB-page arm64 Linux grows
// its stack in 16 KiB steps for the same reason -- and the alternative is
// keeping the 4x amplification on the one mapping every process has: measured
// on a recursion 1400 frames deep, 21 MB of host memory for the stack against
// 5408 KB with the grouping, across 195 VM regions against 54.
static void mem_map_growsdown_group(struct mem *mem, page_t page) {
    page_t first = page, last = page;
    if (mem_page_packing_enabled()) {
        pages_t group = mem_host_page_group();
        page_t base = mem_host_page_group_start(page, group);
        while (first > base && mem_pt(mem, first - 1) == NULL &&
                mem_lazy_find(mem, first - 1) == NULL &&
                mem_growsdown_allowed(mem, first - 1))
            first--;
        // Upwards is the hole case: a frame with a large local skipped over a
        // page on its way down, and the stack is already mapped above it. Those
        // pages are closer to the stack than the faulting one, so the growsdown
        // test they would face is the one it has already passed.
        //
        // page_limit is checked explicitly because mem_pt returns NULL for a
        // page ABOVE the limit exactly as it does for an unmapped one, and
        // walking into that would hand pt_map a range it rejects -- turning a
        // stack growth that has already been allowed into a failed fault.
        while (last + 1 < base + group && last + 1 < mem->page_limit &&
                mem_pt(mem, last + 1) == NULL &&
                mem_lazy_find(mem, last + 1) == NULL)
            last++;
    }
    // A lazy reservation must never overlap a mapped page (see struct
    // mem_lazy_map), which is why the walks above skip reserved pages rather
    // than letting pt_map drop or materialise a reservation for the sake of
    // three stack pages.
    pt_map_nothing(mem, first, (pages_t) (last - first + 1), P_WRITE | P_GROWSDOWN);
}

int pt_set_flags(struct mem *mem, page_t start, pages_t pages, int flags) {
    // mprotect rewrites per-page flags, so materialise any overlapping reservation first rather than
    // teaching this path about them. Iterates reservations, never pages.
    mem_lazy_materialize_range(mem, start, start + pages);

    if (!mem_page_range_valid(mem, start, pages))
        return _ENOMEM;
    for (page_t page = start; page < start + pages; page++)
        if (mem_pt(mem, page) == NULL)
            return _ENOMEM;
    for (page_t page = start; page < start + pages; page++) {
        struct pt_entry *entry = mem_pt(mem, page);
        int old_flags = entry->flags;
        int keep_flags = old_flags & ~(P_READ | P_WRITE | P_EXEC);
        int new_flags = keep_flags | flags;

        // Reserve guest PROT_NONE anonymous mappings without host backing.
        // If they later become accessible, allocate a real host page here --
        // except for a SHARED one, whose backing belongs to every mapper and
        // so has to be filled in on the struct they share. See above.
        if (entry->data->data == NULL && (new_flags & P_RWX) != 0) {
            if (entry->flags & P_SHARED) {
                int err = mem_materialize_shared_data(entry->data);
                if (err < 0)
                    return err;
                // ...then fall through to the ordinary flag update below.
            } else {
                // Commit the whole host page's worth of pages that this same
                // mprotect is about to give identical flags, rather than one
                // guest page per host page -- see the host-page packing comment
                // above. Upwards only: every page below this one in the range
                // has already been through the loop.
                //
                // These pages are always P_ANONYMOUS, which is what keeps the
                // group out of /proc/maps: a NULL data->data can only come from
                // pt_map_nothing's PROT_NONE branch, and that is the one call
                // in the tree that passes pt_map a NULL `memory`, always with
                // P_ANONYMOUS forced on.
                page_t last = page;
                if (mem_page_packing_enabled()) {
                    pages_t group = mem_host_page_group();
                    page_t group_end = mem_host_page_group_start(page, group) + group;
                    if (group_end > start + pages)
                        group_end = start + pages;
                    while (last + 1 < group_end) {
                        struct pt_entry *next = mem_pt(mem, last + 1);
                        // Equal old flags means equal new flags (new_flags is a
                        // pure function of them and of `flags`), and it also
                        // means not P_SHARED, since this branch only runs for a
                        // page that is not.
                        if (next == NULL || next->flags != (unsigned) old_flags ||
                                next->data->data != NULL)
                            break;
                        last++;
                    }
                }
                pages_t group_pages = (pages_t) (last - page + 1);
                size_t bytes = (size_t) group_pages * PAGE_SIZE;
                void *memory = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
                int err = pt_map(mem, page, group_pages, memory, 0, new_flags);
                if (err < 0) {
                    // Ownership only transfers on success -- see pt_map_nothing.
                    if (memory != MAP_FAILED)
                        munmap(memory, bytes);
                    return err;
                }
                // pt_map already wrote new_flags for every page of the group, so
                // resume past it rather than reprocessing them.
                page = last;
                continue;
            }
        }

        entry->flags = new_flags;
        if (((new_flags ^ old_flags) & (P_READ | P_WRITE))) {
            int err = mem_mirror_host_page_protection(entry, new_flags);
            if (err < 0)
                return err;
        }
    }
    mem_changed(mem);
    return 0;
}

int pt_copy_on_write(struct mem *src, struct mem *dst, page_t start, page_t pages) {
    // fork's COW pass walks real entries, so materialise any overlapping reservation first rather than
    // teaching this path about them. Iterates reservations, never pages.
    mem_lazy_materialize_range(src, start, start + pages);

    if (!mem_page_range_valid(src, start, pages) || !mem_page_range_valid(dst, start, pages))
        return -1;
    page_t end = start + pages;
    // dst is the freshly mem_init'd child of a not-yet-started fork(): its
    // jit was just allocated and no thread has ever executed in it, so the
    // per-page unmaps below need no jetsam_lock exclusion (nothing could be
    // racing them). Calling the locked pt_unmap_always here instead made
    // every single fork() take/release the jetsam write lock once per
    // mapped page of the PARENT's entire address space -- for a typical
    // dynamically-linked process, thousands of uncontended lock cycles per
    // fork, on a path (systemd forking constantly during boot) hot enough
    // to shift boot timing and plausibly help surface unrelated races.
    int ret = 0;
    struct data_owner_run run = {};
    for (page_t page = mem_next_mapped_page(src, start);
         page != BAD_PAGE && page < end;
         page = mem_next_mapped_page(src, page + 1)) {
        struct pt_entry *entry = mem_pt(src, page);
        if (entry == NULL)
            continue;
        if (pt_unmap_always_unlocked(dst, page, 1) < 0) {
            data_owner_run_flush(&run, dst, +1);
            ret = -1;
            break;
        }
        // MADV_WIPEONFORK: the child gets fresh zero pages rather than the
        // parent's data. The point is a page holding something the parent
        // must not leak across a fork -- a PRNG state, a key -- so silently
        // inheriting it, which is what happened before, is the failure the
        // caller asked to be protected from.
        if (entry->flags & P_WIPEONFORK) {
            // The ATTRIBUTE is inherited -- Linux copies vm_flags, so a
            // grandchild is wiped too. Measured.
            //
            // This page is NOT shared with the parent, so it ends any run:
            // the child gets its own fresh mapping here and the next shared
            // page is not offset-contiguous with the last one.
            data_owner_run_flush(&run, dst, +1);
            if (pt_map_nothing(dst, page, 1, entry->flags & ~P_COW) < 0) {
                ret = -1;
                break;
            }
            continue;
        }
        if (!(entry->flags & P_SHARED))
            entry->flags |= P_COW;
        entry->data->refcount++;
        // Ownership site 5 of 5, and the only one that ever adds a SECOND
        // address space to a struct data -- section 2.5 re-derived that this is
        // the sole cross-mem entry point. Everything the pager refuses to evict
        // because it is shared becomes shared here.
        //
        // Coalesced -- see struct data_owner_run, which also says why
        // deferring an ACQUIRE is safe here specifically.
        data_owner_run_add(&run, dst, entry->data, entry->offset, +1);
        struct pt_entry *dst_entry = mem_pt_new(dst, page);
        if (dst_entry == NULL) {
            entry->data->refcount--;
            // This page never made it, so take it back off the run rather
            // than acquiring and releasing separately -- nothing has been
            // recorded for it yet.
            run.pages--;
            data_owner_run_flush(&run, dst, +1);
            ret = -1;
            break;
        }
        dst_entry->data = entry->data;
        dst_entry->offset = entry->offset;
        dst_entry->flags = entry->flags;
        // The child inherits the swap state, because it inherits the frame. If
        // it did not, the child would read a PROT_NONE frame as resident and
        // take a host fault.
        //
        // Both address spaces now hold entries into one released frame, and
        // either may fault it back. That is safe because neither holds a slot
        // of its own to fault it back FROM: the slot is the frame's, the frame
        // lock serialises the two faults, and whichever loses the race finds
        // frame_slot already SWAP_SLOT_NONE and simply publishes its entry.
        atomic_store_explicit(&dst_entry->swap_state,
                atomic_load_explicit(&entry->swap_state, memory_order_acquire),
                memory_order_release);
    }
    data_owner_run_flush(&run, dst, +1);
    mem_changed(src);
    mem_changed(dst);
    return ret;
}

static void mem_changed(struct mem *mem) {
    atomic_fetch_add_explicit(&mem->mmu.changes, 1, memory_order_relaxed);
}

// This version will return NULL instead of making necessary pagetable changes.
// Used by the emulator to avoid deadlocks.
static void *mem_ptr_nofault(struct mem *mem, guest_addr_t addr, int type) {
    struct pt_entry *entry = mem_pt(mem, PAGE(addr));
    if (entry == NULL)
        return NULL;
    // An evicted page reads as ABSENT to everything, and this one test is what
    // makes that true for every engine at once: the JIT and interpreters reach
    // here through tlb_handle_miss -> mmu_translate, the syscall side through
    // mem_ptr, and the fault handler through mem_ptr_fault. There is no second
    // place to teach.
    //
    // Deliberately BEFORE the protection tests below. An evicted frame is
    // mprotect(PROT_NONE) on the host, so a read that got past this would take
    // a host fault in kernel C code, where the JIT's crash recovery is not
    // armed and nothing turns it into a guest signal.
    if (atomic_load_explicit(&entry->swap_state, memory_order_acquire) != PT_RESIDENT)
        return NULL;
    // PROT_NONE (no access bits set) must fault on EVERY access, including
    // reads, even when the page still has live host backing -- e.g. an RW page
    // later mprotect'd to PROT_NONE keeps its data pointer but must no longer be
    // readable. Without this, guard pages (stack guards, sanitizer/JIT/runtime
    // PROT_NONE regions) silently allowed reads where real Linux faults.
    // MEM_WRITE_PTRACE intentionally bypasses guest protections (debugger poke).
    if (type != MEM_WRITE_PTRACE && (entry->flags & P_RWX) == 0)
        return NULL;
    if (type == MEM_WRITE && !P_WRITABLE(entry->flags))
        return NULL;
    if (entry->data->data == NULL)
        return NULL;
    return entry->data->data + entry->offset + PGOFFSET(addr);
}

void *mem_ptr(struct mem *mem, guest_addr_t addr, int type) {
    // Bring an evicted frame back before anything else looks at the entry.
    // Ahead of the old_ptr snapshot below deliberately: that snapshot feeds an
    // assert at the end of this function, and taking it while the page is still
    // evicted would compare a NULL from before the fault against a real pointer
    // from after it.
    {
        struct pt_entry *swapped = mem_pt(mem, PAGE(addr));
        if (swapped != NULL &&
            atomic_load_explicit(&swapped->swap_state, memory_order_acquire) != PT_RESIDENT)
            swap_fault_page(mem, PAGE(addr));   // NULL on failure is handled below
    }
    void *old_ptr = mem_ptr_nofault(mem, addr, type); // just for an assert

    page_t page = PAGE(addr);
    struct pt_entry *entry = mem_pt(mem, page);

    if (entry == NULL && mem_lazy_find(mem, page) != NULL) {
        // Lazy reservation: materialise on first touch. Same lock dance as
        // growsdown below -- the upgrade drops the read lock, so re-check
        // under the write lock before mapping.
        read_to_write_lock(&mem->lock);
        if (mem_pt(mem, page) == NULL) {
            mem_lazy_fault(mem, page);
            // A reservation becoming a real page is exactly a minor fault:
            // resolved from memory, nothing read from storage.
            task_count_minflt();
        }
        write_to_read_lock(&mem->lock);
        entry = mem_pt(mem, page);
    }

    if (entry == NULL) {
        // page does not exist
        // look to see if the next VM region is willing to grow down onto it
        if (!mem_growsdown_allowed(mem, page))
            return NULL;

        // Changing memory maps must be done with the write lock. But this is
        // called with the read lock.
        // This locking stuff is copy/pasted for all the code in this function
        // which changes memory maps.
        read_to_write_lock(&mem->lock);
        // The upgrade drops the read lock before taking the write lock, so
        // another thread faulting the same growsdown page can map it first.
        // Mapping again would discard anything that thread already wrote.
        if (mem_pt(mem, page) == NULL) {
            mem_map_growsdown_group(mem, page);
            task_count_minflt();
        }
        write_to_read_lock(&mem->lock);

        entry = mem_pt(mem, page);
    }

    if (entry != NULL && (type == MEM_WRITE || type == MEM_WRITE_PTRACE)) {
        // if page is unwritable, well tough luck
        if (type != MEM_WRITE_PTRACE && !(entry->flags & P_WRITE))
            return NULL;
        
        if (type == MEM_WRITE_PTRACE) {
            // A debugger's poke has to be able to write a page the tracee has
            // marked read-only, hence P_WRITE.
            //
            // P_COW, though, only for a PRIVATE page. Copying a SHARED one
            // gives this process a private duplicate and quietly severs it
            // from everybody else: the poke never reached the file, and every
            // store the TRACEE itself made afterwards was lost too, with no
            // error anywhere. On Linux the poke lands in the shared page and
            // the mapping keeps its file. Measured.
            entry->flags |= P_WRITE;
            if (!(entry->flags & P_SHARED))
                entry->flags |= P_COW;
        }
#if ENGINE_JIT
        // get rid of any compiled blocks in this page
        jit_invalidate_page(mem->mmu.jit, page);
#endif
        
        // if page is cow, ~~milk~~ copy it
        
        if (entry->flags & P_COW) {
            // Breaking a copy-on-write page is a minor fault, and after a fork
            // it is most of the process's fault count.
            task_count_minflt();
            bool locked_general_lock = false;
            // Some callers, including do_exit() via clear_tid, already hold
            // general_lock. Re-locking it here self-deadlocks while trying to
            // resolve the final COW write into user memory.
            if (current != NULL && !pthread_equal(current->general_lock.owner, pthread_self())) {
                lock(&current->general_lock, 0);  // prevent elf_exec from doing mm_release while we are in flight
                locked_general_lock = true;
            }
            read_to_write_lock(&mem->lock);
            entry = mem_pt(mem, page);
            if (entry == NULL) {
                if (locked_general_lock)
                    unlock(&current->general_lock);
                write_to_read_lock(&mem->lock);
                return NULL;
            }
            if (type != MEM_WRITE_PTRACE && !(entry->flags & P_WRITE)) {
                if (locked_general_lock)
                    unlock(&current->general_lock);
                write_to_read_lock(&mem->lock);
                return NULL;
            }
            if (!(entry->flags & P_COW)) {
                if (locked_general_lock)
                    unlock(&current->general_lock);
                write_to_read_lock(&mem->lock);
                goto done_write_fault;
            }
            // Residency is re-checked HERE, after the upgrade, and not only in
            // the prologue at the top of this function. read_to_write_lock
            // releases the reader before it waits, and the evictor takes that
            // write lock -- so a page this function swapped in under the read
            // lock can be gone again by the time control reaches this line.
            // mem_break_cow_group then memcpy()s out of `data->data + offset`
            // with no page-table consultation, which on a released frame is a
            // HOST fault inside emulator C code, where the JIT's crash recovery
            // is not armed and nothing turns it into a guest signal.
            //
            // Today's eviction filter refuses P_COW frames, so nothing can
            // actually take this window -- which is precisely why it is worth
            // closing now. Section 2.4 of the plan argues the pager should
            // eventually evict COW copies of private file pages, and the day
            // that filter widens this becomes a live host crash that looks like
            // an emulator bug.
            if (atomic_load_explicit(&entry->swap_state, memory_order_acquire) != PT_RESIDENT &&
                    swap_fault_page_locked(mem, page) < 0) {
                if (locked_general_lock)
                    unlock(&current->general_lock);
                write_to_read_lock(&mem->lock);
                return NULL;
            }
            entry = mem_pt(mem, page);
            if (entry == NULL) {
                if (locked_general_lock)
                    unlock(&current->general_lock);
                write_to_read_lock(&mem->lock);
                return NULL;
            }
            // Copies this page and, on a 16 KiB-page host, the rest of its host
            // page: see the host-page packing comment above mem_break_cow_group.
            if (mem_break_cow_group(mem, page) < 0) {
                // The COW break did not happen, so there is no private page to
                // hand back, and returning the still-shared one would let the
                // guest's write land in the parent's (or a sibling's) memory.
                if (locked_general_lock)
                    unlock(&current->general_lock);
                write_to_read_lock(&mem->lock);
                return NULL;
            }
            if (locked_general_lock)
                unlock(&current->general_lock);
            write_to_read_lock(&mem->lock);
            
        }
        
    }

done_write_fault:
    if (entry != NULL && type != MEM_WRITE_PTRACE && (entry->flags & P_WRITE)) {
        int host_err = mem_ensure_host_writable(entry);
        if (host_err < 0)
            return NULL;
    }
    void *ptr = mem_ptr_nofault(mem, addr, type);
    assert(old_ptr == NULL || old_ptr == ptr || type == MEM_WRITE_PTRACE);
    return ptr;
}

void *mem_ptr_fault(struct mem *mem, guest_addr_t addr, int type) {
    page_t page = PAGE(addr);
    write_lock(&mem->lock);

    struct pt_entry *entry = mem_pt(mem, page);
    if (entry == NULL && mem_lazy_find(mem, page) != NULL) {
        mem_lazy_fault(mem, page);      // write lock already held
        entry = mem_pt(mem, page);
    }
    if (entry == NULL) {
        if (!mem_growsdown_allowed(mem, page)) {
            write_unlock(&mem->lock);
            return NULL;
        }
        mem_map_growsdown_group(mem, page);
        entry = mem_pt(mem, page);
    }

    // An evicted page must be faulted back here too, not only in mem_ptr: this
    // is the path handle_page_fault_interrupt takes, which is how a JIT or
    // interpreter access arrives.
    //
    // UNDER THE WRITE LOCK THIS FUNCTION ALREADY HOLDS, and that placement is
    // the whole point. The first version did it in a read-locked prologue and
    // then dropped the read lock to take the write lock -- and the evictor
    // takes the write lock, so it fits exactly in that gap. The page arrived
    // resident and was evicted again before this function looked at it, so
    // mem_ptr_nofault at the bottom returned NULL and the guest took a SIGSEGV
    // on a page that is perfectly well mapped.
    //
    // MEASURED. With the prototype's eviction scan slow enough that only one
    // sweep ever landed, the window never opened. Making the scan skip
    // unallocated directories (see swap_evict_mem) let a second sweep run while
    // the guest was faulting pages back in, and the arm64 verifier died with
    // SIGSEGV after 2064 evictions and 1656 faults. Every run since this moved
    // has passed -- five guest roots by three test shapes, plus repeated arm64
    // and amd64 batches. The mechanism above is an argument from the lock
    // protocol rather than an isolated repro; the observation is that the
    // failure appeared with the window open and has not been seen with it
    // closed.
    //
    // It must also come BEFORE the copy-on-write break below: mem_break_cow_group
    // memcpy()s straight out of `data->data + offset`, with no page-table
    // consultation, so a released frame there is a HOST fault inside emulator C
    // code, which nothing turns into a guest signal.
    //
    // The cost is that the pread runs with the address space exclusively
    // locked, which is 82 us at p50 on device and 683 us at p99.9 -- every
    // thread of the process stalls for it. Section 3.5 of the plan is explicit
    // that the shipping version has to drop, run, re-take and RE-WALK instead,
    // re-fetching mem_pt after every resume point. That is pager-core work; the
    // prototype takes the stall and stays correct.
    if (entry != NULL &&
            atomic_load_explicit(&entry->swap_state, memory_order_acquire) != PT_RESIDENT) {
        if (swap_fault_page_locked(mem, page) < 0) {
            write_unlock(&mem->lock);
            return NULL;    // caller turns this into a guest SIGBUS
        }
        entry = mem_pt(mem, page);
        if (entry == NULL) {
            write_unlock(&mem->lock);
            return NULL;
        }
    }

    if (entry != NULL && (type == MEM_WRITE || type == MEM_WRITE_PTRACE)) {
        if (type != MEM_WRITE_PTRACE && !(entry->flags & P_WRITE)) {
            write_unlock(&mem->lock);
            return NULL;
        }
        if (type == MEM_WRITE_PTRACE) {
            // Same as the other write-fault path above: never COW a shared
            // page, or the poke detaches the tracee from its own mapping.
            entry->flags |= P_WRITE;
            if (!(entry->flags & P_SHARED))
                entry->flags |= P_COW;
        }
#if ENGINE_JIT
        jit_invalidate_page(mem->mmu.jit, page);
#endif
        if (entry->flags & P_COW) {
            // Breaking a copy-on-write page is a minor fault, and after a fork
            // it is most of the process's fault count.
            task_count_minflt();
            bool locked_general_lock = false;
            if (current != NULL && !pthread_equal(current->general_lock.owner, pthread_self())) {
                lock(&current->general_lock, 0);
                locked_general_lock = true;
            }
            entry = mem_pt(mem, page);
            if (entry == NULL) {
                if (locked_general_lock)
                    unlock(&current->general_lock);
                write_unlock(&mem->lock);
                return NULL;
            }
            if (type != MEM_WRITE_PTRACE && !(entry->flags & P_WRITE)) {
                if (locked_general_lock)
                    unlock(&current->general_lock);
                write_unlock(&mem->lock);
                return NULL;
            }
            if (entry->flags & P_COW) {
                // Same group break as mem_ptr's path above.
                if (mem_break_cow_group(mem, page) < 0) {
                    // No private page means no writable pointer.
                    if (locked_general_lock)
                        unlock(&current->general_lock);
                    write_unlock(&mem->lock);
                    return NULL;
                }
                entry = mem_pt(mem, page);
            }
            if (locked_general_lock)
                unlock(&current->general_lock);
        }
    }

    if (entry != NULL && type != MEM_WRITE_PTRACE && (entry->flags & P_WRITE)) {
        int host_err = mem_ensure_host_writable(entry);
        if (host_err < 0) {
            write_unlock(&mem->lock);
            return NULL;
        }
    }

    void *ptr = mem_ptr_nofault(mem, addr, type);
    write_unlock(&mem->lock);
    return ptr;
}

static void *mem_mmu_translate(struct mmu *mmu, guest_addr_t addr, int type) {
    struct mem *mem = container_of(mmu, struct mem, mmu);
    void *ptr = mem_ptr_nofault(mem, addr, type);
    if (ptr == NULL && type == MEM_READ && current != NULL &&
            current->abi == GUEST_ABI_AMD64 && amd64_jit_debug_enabled()) {
        enum { AMD64_JIT_TRANSLATE_TRACE_BUDGET = 64 };
        static unsigned amd64_jit_translate_trace_count;
        if (amd64_jit_translate_trace_count < AMD64_JIT_TRANSLATE_TRACE_BUDGET) {
            amd64_jit_translate_trace_count++;
            struct pt_entry *entry = mem_pt(mem, PAGE(addr));
            if (entry == NULL) {
                fprintf(stderr,
                        "[amd64-jit] mmu miss addr=%#llx page=%#llx page_limit=%#llx mmap=[%#llx,%#llx) mem=%p current_mem=%p no-entry\n",
                        (unsigned long long) addr,
                        (unsigned long long) PAGE(addr),
                        (unsigned long long) mem->page_limit,
                        (unsigned long long) mem->mmap_floor,
                        (unsigned long long) mem->mmap_ceiling,
                        (void *) mem,
                        current != NULL ? (void *) current->mem : NULL);
            } else {
                fprintf(stderr,
                        "[amd64-jit] mmu miss addr=%#llx page=%#llx flags=%#x data=%p off=%zu type=%d mem=%p current_mem=%p\n",
                        (unsigned long long) addr,
                        (unsigned long long) PAGE(addr),
                        entry->flags,
                        entry->data != NULL ? entry->data->data : NULL,
                        entry->offset,
                        type,
                        (void *) mem,
                        current != NULL ? (void *) current->mem : NULL);
            }
        }
    }
    if (ptr == NULL || type != MEM_WRITE)
        return ptr;

    struct pt_entry *entry = mem_pt(mem, PAGE(addr));
    if (entry == NULL)
        return NULL;
    if (mem_ensure_host_writable(entry) < 0)
        return NULL;
    return ptr;
}

static struct mmu_ops mem_mmu_ops = {
    .translate = mem_mmu_translate,
};

int mem_segv_reason(struct mem *mem, guest_addr_t addr) {
    struct pt_entry *pt = mem_pt(mem, PAGE(addr));
    if (pt == NULL)
        return SEGV_MAPERR_;
    return SEGV_ACCERR_;
}

size_t real_page_size;
__attribute__((constructor)) static void get_real_page_size(void) {
    real_page_size = sysconf(_SC_PAGESIZE);
}

void mem_coredump(struct mem *mem, const char *file) {
    int fd = open(file, O_CREAT | O_RDWR | O_TRUNC, 0666);
    if (fd < 0) {
        perror("open");
        return;
    }
    if (ftruncate(fd, 0xffffffff) < 0) {
        perror("ftruncate");
        close(fd);
        return;
    }

    // The walk reads entry->data and then dereferences it, and a sibling's
    // munmap frees that struct data (pt_unmap_always_unlocked) under the write
    // lock, so this needs the read lock for exactly the reason the /proc
    // walkers take it. Quiesce-aware, or a writer already inside the
    // mem-quiesce barrier is left waiting on a reader it cannot evict.
    //
    // Two things a reviver has to know, because the lock is then held across an
    // unbounded run of lseek()/write() calls -- the shape this codebase has
    // been bitten by before (mem_defer_fd_close exists because pt_unmap held
    // this lock across a guest fd's ->close). First, why it is tolerable here:
    // `fd` is a HOST descriptor, opened at the top of this function, so the
    // writes go straight to the host filesystem and can never block waiting on
    // a guest process the way a guest ->close can. Second, the precondition:
    // the caller must not already hold mem->lock in write mode, or this
    // deadlocks outright. A crash handler is the likeliest reviver, and a crash
    // handler is exactly the code that might already be inside the write lock.
    mem_read_lock_quiesce_aware(mem);
    int pages = 0;
    for (page_t page = mem_next_mapped_page(mem, 0);
         page != BAD_PAGE && page < mem->page_limit;
         page = mem_next_mapped_page(mem, page + 1)) {
        struct pt_entry *entry = mem_pt(mem, page);
        if (entry == NULL)
            continue;
        pages++;
        if (entry->data->data == NULL)
            continue;
        if (lseek(fd, page << PAGE_BITS, SEEK_SET) < 0) {
            perror("lseek");
            break;
        }
        // entry->offset is this page's byte offset INTO the mapping's host
        // memory, set per page by pt_map. Writing data->data without it dumped
        // the mapping's FIRST page for every page of the mapping: a 1 MiB
        // mapping wrote page 0 into 256 different file offsets. mem_ptr and the
        // COW break (both in this file) get this right; this one did not, and
        // it has no caller, so nothing was going to notice until somebody
        // revived it.
        if (write(fd, (char *) entry->data->data + entry->offset, PAGE_SIZE) < 0) {
            perror("write");
            break;
        }
    }
    mem_read_unlock_quiesce_aware(mem);
    printk("WARNING: dumped %d pages\n", pages);
    close(fd);
}

void mem_ref_cnt_mod(struct mem *mem, int value) { // value should only be -1 or 1.
    // Keep track of how many threads are referencing this mem. Maintained
    // unconditionally (see task_ref_cnt_mod): mm_release gates teardown on
    // this count, and the old doEnableExtraLocking preference could be
    // toggled mid-run, leaving the count permanently imbalanced.
    if(mem == NULL) {
            return;
    }

    if (value != 1 && value != -1) {
        printk("ERROR: invalid mem refcount delta %d\n", value);
        return;
    }

    int old_count = atomic_load_explicit(&mem->reference.count, memory_order_relaxed);
    do {
        if((old_count + value) < 0) { // Prevent the count from going negative.
            void *caller = __builtin_return_address(0);
            Dl_info caller_info = {};
            const char *caller_name = "?";
            ptrdiff_t caller_offset = 0;
            if (caller != NULL && dladdr(caller, &caller_info) != 0 && caller_info.dli_sname != NULL) {
                caller_name = caller_info.dli_sname;
                caller_offset = (char *) caller - (char *) caller_info.dli_saddr;
            }
            printk("ERROR: Attempt to decrement mem reference count to be negative, ignoring(%d:%d) caller=%s+%td addr=%p mem=%p\n",
                   old_count, value, caller_name, caller_offset, caller, mem);
            return;
        }
    } while (!atomic_compare_exchange_weak_explicit(&mem->reference.count, &old_count, old_count + value,
                                                    memory_order_acq_rel, memory_order_relaxed));
}

int mem_ref_cnt_get(struct mem *mem) {
    if (mem == NULL) return 0;
    int cnt = atomic_load_explicit(&mem->reference.count, memory_order_acquire);
    if((cnt < 0) || ( cnt > 1000)) // Stupid kluge while I fix this brain damage
        cnt = 0;
    return cnt;
}

// ---- Phase 1 gate prototype: evict a guest frame and fault it back ---------
//
// See the header for what this is and, more importantly, what it is not. The
// one property it exists to establish is that an evicted frame FAULTS on a
// stale access rather than returning the bytes that used to be there. That is
// the failure the C2 refutation of this design was about, and on Apple it is
// completely silent without help: MADV_FREE_REUSABLE leaves the frame mapped,
// readable and byte-identical, so a missed pointer holder reads plausible old
// data and a write is silently reverted by the swap-in. So the release is
// REUSABLE *followed by mprotect(PROT_NONE)*, in production and not only under
// a debug knob, which the day-1 device probe measured as both holding the
// footprint drop and costing 2.53 us per frame.

// Storage -- the file, the slot bitmap and the on/off switch -- is
// kernel/swap.c's. This file owns the page table and the frames, and the two
// meet only at swap_slot_alloc/free/read/write.
//
// That line is where it is because of what happened when it was not drawn. An
// earlier attempt put a second pager in kernel/swap.c with its own view of the
// page table; two owners of one piece of state produced measured silent
// corruption. And the prototype's own storage, a lazily-opened file behind an
// unlocked `if (fd < 0)`, let two concurrent evictions create two files and
// interleave slots across them, so the guest read ZEROS for pages it had
// written. One owner each, and a narrow interface between them.
// ATOMIC, all of them. Every evictor and every faulting guest thread bumps
// these, and two evictions of two processes run concurrently -- the barrier
// each takes is its own address space's. Plain increments here quietly
// under-reported, which for a diagnostic whose whole job is to say whether the
// release did anything is not a cosmetic problem.
static _Atomic unsigned long swap_stat_evicted, swap_stat_faulted;
static _Atomic unsigned long swap_stat_out, swap_stat_in;
// Faults that found the frame already back and did no I/O at all. A high ratio
// to frames evicted means either healthy sharing (a forked sibling faulted it
// first) or that eviction is choosing frames that are not cold.
static _Atomic unsigned long swap_stat_cancelled;
static _Atomic unsigned long swap_stat_adv_fail, swap_stat_prot_fail;
static _Atomic unsigned long swap_stat_ledger_refused;
static int swap_stat_adv_errno, swap_stat_prot_errno;
unsigned long swap_prototype_ledger_refused(void) { return swap_stat_ledger_refused; }
void swap_prototype_failures(unsigned long *adv, unsigned long *prot, int *adv_e, int *prot_e) {
    if (adv) *adv = swap_stat_adv_fail;
    if (prot) *prot = swap_stat_prot_fail;
    if (adv_e) *adv_e = swap_stat_adv_errno;
    if (prot_e) *prot_e = swap_stat_prot_errno;
}

// A frame is one HOST page: the unit the host will actually release. On Apple
// silicon that is 16 KiB, so four guest pages, which is why the eviction below
// only ever considers four consecutive guest pages at a time.
//
// One definition, shared with the frame_refs bookkeeping in the ownership
// section above -- these two must never be able to disagree about which entries
// share a frame, or the count and the release would be measuring different
// things.
static size_t swap_frame_size(void) {
    return mem_frame_size();
}
static size_t swap_pages_per_frame(void) {
    return swap_frame_size() / PAGE_SIZE;
}

// Is this guest page the base of a frame this prototype may evict?
//
// The restrictions are deliberately narrow, because a gate that evicts more
// than it can reason about proves less: anonymous private only, and the whole
// frame must be four consecutive guest pages of ONE struct data at consecutive
// offsets. What changed since the gate is the two tests that used to be
// approximations:
//
//  - EXCLUSIVITY. The gate asked whether refcount equalled the mapping's own
//    page count, which is right only for a mapping that never forked and never
//    had a page unmapped out of it. It now asks the owner records, which answer
//    exactly (struct data::owners). That is strictly broader as well as
//    strictly safer: a partially-unmapped mapping used to be refused for no
//    reason.
//  - FRAME OCCUPANCY. The four entries found must be ALL the entries pointing
//    into this host frame, and frame_refs is what makes that a measurement.
//    Without it the four-consecutive-pages test is safe only by coincidence of
//    who calls pt_dup today.
//
// Exclusive struct data does NOT imply an exclusive host frame, so the vdso is
// excluded by identity and not by ownership: kernel/vdso.c's static array gets
// its OWN struct data on every 32-bit exec (kernel/exec.c), each of which reads
// as exclusive, while all of them share one host page -- which in build/ish
// also holds unrelated emulator globals, including a live lock. Section 2.5
// makes that a written-down precondition of what the ownership record means
// rather than a filter someone may later relax.
//
// The remaining struct-data exclusions are section 3.4's, gating on fields
// rather than on the guest-visible flag: a mapping with an fd, a
// never-writable-file cache entry, a shared key or a name is not AOK's
// anonymous memory to release. (Section 2.4 argues P_ANONYMOUS should
// eventually go too, because it needlessly excludes COW copies of private file
// pages, which ARE real anonymous host memory -- both COW breaks inherit
// `entry->flags & ~P_COW`. Widening the set is a pager-core decision with its
// own measurements; it is not a change to make while the gate is the only
// regression test.)
static bool swap_frame_eligible(struct mem *mem, page_t base, struct data **data_out,
                                size_t *offset_out) {
    size_t per = swap_pages_per_frame();
    struct data *data = NULL;
    size_t first_offset = 0;
    for (size_t i = 0; i < per; i++) {
        struct pt_entry *pt = mem_pt(mem, base + i);
        if (pt == NULL || pt->data == NULL || pt->data->data == NULL)
            return false;
        if (pt->data->data == vdso_data)
            return false;
        if (pt->data->fd != NULL || pt->data->cache_entry != NULL ||
                pt->data->shared_key != 0 || pt->data->name != NULL)
            return false;
        if (!(pt->flags & P_ANONYMOUS) || (pt->flags & P_SHARED) || (pt->flags & P_COW))
            return false;
        if (i == 0) {
            data = pt->data;
            first_offset = pt->offset;
            // The frame must sit on a host-page boundary inside the mapping, or
            // releasing it would take bytes that belong to a neighbour.
            if ((((uintptr_t) data->data + first_offset) & (swap_frame_size() - 1)) != 0)
                return false;
            if (first_offset + swap_frame_size() > data->size)
                return false;
            if (!data_is_exclusive_to(data, mem))
                return false;   // reachable from a second address space
            // Every entry pointing into this frame must be one of the `per`
            // this loop is about to find. -1 is "not known", which has to read
            // as "do not touch it".
            if (data_frame_ref_count(data, first_offset) != (int) per)
                return false;
            // No frame_slot means the frame has nowhere to record where its
            // bytes went, so it can never be brought back. Allocation failure
            // in pt_map drops the pair together, so this and frame_refs are
            // never separately absent -- the test is here because the swap-in
            // path dereferences it and must not have to wonder.
            if (data->frame_slot == NULL)
                return false;
            // Is the frame OUT already? Asked of the frame, not of the entries'
            // swap_state bytes, which are a conservative hint: after a forked
            // sibling faults a frame back, this address space's entries can
            // stay PT_SWAPPED over memory that is resident and holding the
            // right bytes, and a hint test here would make that frame
            // unevictable for the life of the process -- measured as two full
            // sweeps releasing nothing at all. The frame's slot is the truth.
            if (atomic_load_explicit(&data->frame_slot[first_offset / swap_frame_size()],
                        memory_order_acquire) != SWAP_SLOT_NONE)
                return false;
        } else {
            if (pt->data != data || pt->offset != first_offset + i * PAGE_SIZE)
                return false;
        }
    }
    *data_out = data;
    *offset_out = first_offset;
    return true;
}

// The per-frame lock, striped. It serialises the transition of ONE frame
// between "in host memory" and "in slot N", and is held across the pread that
// brings a frame back.
//
// A separate stripe set from the ownership locks, deliberately: those are taken
// on every mmap and munmap of every mapping and must stay short, while this one
// is held for the length of a disk read. Both are leaves -- nothing is ever
// taken under either -- so there is no order between them to get wrong.
//
// It is needed because after a fork, two address spaces hold entries into one
// released frame and each faults under only its OWN write lock. Without this
// they would both mprotect, both madvise and both pread the same host memory,
// and the second would overwrite whatever the first had already handed back to
// its guest.
#define SWAP_FRAME_STRIPES 64
#define SWAP_FRAME_L1 PTHREAD_MUTEX_INITIALIZER
#define SWAP_FRAME_L4 SWAP_FRAME_L1, SWAP_FRAME_L1, SWAP_FRAME_L1, SWAP_FRAME_L1
#define SWAP_FRAME_L16 SWAP_FRAME_L4, SWAP_FRAME_L4, SWAP_FRAME_L4, SWAP_FRAME_L4
static pthread_mutex_t swap_frame_locks[SWAP_FRAME_STRIPES] = {
    SWAP_FRAME_L16, SWAP_FRAME_L16, SWAP_FRAME_L16, SWAP_FRAME_L16,
};
#undef SWAP_FRAME_L16
#undef SWAP_FRAME_L4
#undef SWAP_FRAME_L1

static pthread_mutex_t *swap_frame_lock_for(struct data *data, size_t frame_index) {
    uintptr_t key = (uintptr_t) data;
    key = (key >> 4) ^ (key >> 12) ^ (frame_index * 2654435761u);
    return &swap_frame_locks[key % SWAP_FRAME_STRIPES];
}

// Evict one frame. Everything happens under the caller's write lock, including
// the write to the swap file. The shipping design (section 2.4) does the I/O
// outside the barrier, which it can because the entries already read as absent
// by then; doing it inside is slower and obviously correct, and correctness is
// what this stage is measuring.
static bool swap_evict_frame(struct mem *mem, page_t base, struct data *data, size_t offset) {
    size_t frame = swap_frame_size();
    char *host = (char *) data->data + offset;

    uint32_t slot;
    if (swap_slot_alloc(&slot) < 0)
        return false;   // the area the user asked for is full; keep the frame
    if (swap_slot_write(slot, host, frame) < 0) {
        // Nothing has been published yet, so the frame is untouched and the
        // slot has no owner. Give it straight back rather than leaking it --
        // the slot pool is a fixed size the user chose, and a leak there is a
        // pager that quietly stops being able to evict.
        swap_slot_free(slot);
        return false;
    }

    // The frame records where its bytes went; the entries only record that
    // they are not here. Order matters: the slot has to be readable before any
    // entry says "go and read it", so it is published first and the entries
    // second, with release ordering on each store.
    //
    // No frame lock is taken. Eviction runs under the address-space barrier of
    // the ONLY address space that reaches this mapping (data_is_exclusive_to
    // was just checked), and every reader of frame_slot is a fault in some
    // address space that reaches it -- so there is no second party to race
    // with. Swap-in is the asymmetric case, because a fork after eviction can
    // create one.
    atomic_store_explicit(&data->frame_slot[offset / frame], slot,
            memory_order_release);
    for (size_t i = 0; i < swap_pages_per_frame(); i++) {
        struct pt_entry *pt = mem_pt(mem, base + i);
        atomic_store_explicit(&pt->swap_state, PT_SWAPPED, memory_order_release);
    }
    mem_changed(mem);

    // Now give the host its page back, and make a stale pointer FAULT. Both
    // calls, in this order: REUSABLE is what moves the ledger, PROT_NONE is
    // what turns a missed pointer holder into a SIGBUS at the guilty
    // instruction instead of silent corruption. mprotect first would be EPERM.
    //
    // MEASURED, and it is the reason the knobs above exist. Run with only the
    // madvise and task_vm_info.reusable rises by exactly the region size
    // (67108864 for a 64 MiB region). Run with both, as production does, and
    // reusable reads 0 while phys_footprint drops by the same amount either
    // way. So the PROT_NONE is not free: it takes the pages back out of the
    // reusable state that madvise just put them in.
    //
    // That leaves a question this prototype cannot settle on macOS, where
    // phys_footprint has no consequence: does the pair still let the kernel
    // RECLAIM the pages, or does it only stop the ledger counting them? The
    // two are indistinguishable here and are very much not on iOS, where that
    // ledger is what jetsam kills on. The day-1 device probe measured the pair
    // releasing byte-exact and returning every vm_map entry on restore, which
    // says the pair is fine there -- but that probe restored immediately, so it
    // never asked whether the pages stay reclaimable while held. Settle it on
    // the device with ISH_SWAP_NO_MPROTECT=1 as the control.
    // Diagnostic knobs: which of the two calls actually releases anything?
    // ISH_SWAP_NO_MADVISE=1 leaves only the mprotect, ISH_SWAP_NO_MPROTECT=1
    // leaves only the madvise. A release that only moves the ledger under
    // PROT_NONE, with the `reusable` counter never rising, is not a release at
    // all -- the pages are still resident and the ledger is just not counting
    // protected ones.
    static int no_madvise = -1, no_mprotect = -1;
    if (no_madvise < 0) {
        const char *a = getenv("ISH_SWAP_NO_MADVISE");
        const char *m = getenv("ISH_SWAP_NO_MPROTECT");
        no_madvise = (a != NULL && a[0] == '1') ? 1 : 0;
        no_mprotect = (m != NULL && m[0] == '1') ? 1 : 0;
    }
    int adv = no_madvise ? 0 : madvise(host, frame, MADV_FREE_REUSABLE);
    int adv_errno = adv == 0 ? 0 : errno;
    int prot = no_mprotect ? 0 : mprotect(host, frame, PROT_NONE);
    int prot_errno = prot == 0 ? 0 : errno;
    if (adv != 0 && swap_stat_adv_fail == 0)
        swap_stat_adv_errno = adv_errno;
    if (prot != 0 && swap_stat_prot_fail == 0)
        swap_stat_prot_errno = prot_errno;
    if (adv != 0) swap_stat_adv_fail++;
    if (prot != 0) swap_stat_prot_fail++;

    swap_stat_evicted++;
    swap_stat_out += frame;
    return true;
}

// Prototype diagnostic: the ledger, read the same way the day-1 probe reads it.
// Measured INSIDE the barrier and around the release loop, because measuring it
// from outside cannot tell "the release did nothing" apart from "something
// faulted it straight back".
static unsigned long long swap_footprint_now(void) {
#if __APPLE__
    // Zeroed, and the returned count checked, both for the reason
    // platform/darwin.c's read_host_mem() spells out: phys_footprint arrived in
    // revision 1 of this struct, a kernel older than the SDK fills fewer fields
    // and says so in count, and the rest is then whatever the caller left on
    // the stack. The first version of this helper did neither and produced a
    // measurement that disagreed with footprint(1), vmmap and ps by 5x while
    // looking entirely plausible.
    task_vm_info_data_t info = {};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t) &info, &count) != KERN_SUCCESS)
        return 0;
    if (count < TASK_VM_INFO_REV1_COUNT)
        return 0;   // no phys_footprint in this reply; 0 means "did not measure"
    return info.phys_footprint;
#else
    return 0;
#endif
}
static unsigned long long swap_fp_before, swap_fp_after, swap_fp_post;
unsigned long long swap_footprint_live(void) { return swap_footprint_now(); }
// The other ledger fields, so a disagreement with an external tool can be
// attributed to a specific one rather than argued about.
void swap_footprint_detail(unsigned long long *internal, unsigned long long *resident,
                           unsigned long long *reusable, unsigned long long *compressed) {
#if __APPLE__
    task_vm_info_data_t i = {};
    mach_msg_type_number_t c = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t) &i, &c) == KERN_SUCCESS &&
        c >= TASK_VM_INFO_REV1_COUNT) {
        if (internal) *internal = i.internal;
        if (resident) *resident = i.resident_size;
        if (reusable) *reusable = i.reusable;
        if (compressed) *compressed = i.compressed;
        return;
    }
#endif
    if (internal) *internal = 0;
    if (resident) *resident = 0;
    if (reusable) *reusable = 0;
    if (compressed) *compressed = 0;
}
void swap_prototype_ledger(unsigned long long *before, unsigned long long *after,
                           unsigned long long *post) {
    if (before) *before = swap_fp_before;
    if (after) *after = swap_fp_after;
    if (post) *post = swap_fp_post;
}

long swap_evict_mem(struct mem *mem) {
    if (mem == NULL)
        return _EINVAL;
    // The switch is consulted HERE and nowhere on the fault path. Turning swap
    // off has to stop new eviction at once, but a frame that went out before it
    // was turned off still has to be answerable until it is back -- refusing
    // there would be a SIGBUS on memory the guest mapped correctly.
    if (!swap_enabled())
        return _ENODEV;
    size_t per = swap_pages_per_frame();
    long released = 0;

    mem_write_lock_pokes_external(mem);
    swap_fp_before = swap_footprint_now();
    // The step on a miss is mem_next_page, not page + 1, and the difference is
    // not a micro-optimisation: page_limit is 2^35 pages for a 64-bit guest and
    // the mappings live near the top of it, so a linear probe walks tens of
    // billions of empty pages. Measured on the arm64 test root before this
    // change: 7.5 minutes of pure scanning to evict a 16 MiB region, which made
    // the gate's own regression test impractical to re-run. mem_next_page
    // bit-scans the pgdir_root and leaf bitmaps and never skips a page that has
    // a leaf, so it cannot miss an eligible frame.
    //
    // The real pager does not scan at all -- section 3.7's clock hand walks
    // allocated leaves in bounded chunks under mem_read_lock_quiesce_aware --
    // so this stays a prototype's whole-address-space sweep, just not a
    // pathological one.
    for (page_t page = 0; page + per <= mem->page_limit; ) {
        struct data *data;
        size_t offset;
        if (swap_frame_eligible(mem, page, &data, &offset) &&
            swap_evict_frame(mem, page, data, offset)) {
            released += (long) swap_frame_size();
            page += per;
        } else {
            mem_next_page(mem, &page);
        }
    }
    swap_fp_after = swap_footprint_now();
    // Trust the ledger, not the return codes. Both calls above can report
    // success and release nothing -- MADV_FREE_REUSABLE does exactly that on a
    // copy-on-write shared or shadowed VM object, which is the failure mode
    // section 4.1 of the plan warns about and which an attached debugger
    // induces. A release that did not move the ledger is a release that did not
    // happen, and the caller has to be able to see that.
    if (swap_fp_before != 0 && swap_fp_after != 0 && released > 0) {
        long long moved = (long long) swap_fp_before - (long long) swap_fp_after;
        // Under a tenth of what was released is not a rounding difference.
        if (moved < released / 10)
            swap_stat_ledger_refused++;
    }
    mem_write_unlock_pokes_external(mem);
    // Third sample, after the barrier is gone. If the drop is visible at
    // swap_fp_after but not here, the undo is something the release path does,
    // not a guest fault -- which is a different bug entirely.
    swap_fp_post = swap_footprint_now();
    return released;
}

// Bring one evicted frame back, with the address-space WRITE lock already held.
//
// The write lock is what makes this correct rather than merely usual: the
// evictor takes the same lock, so while it is held no frame of this mem can be
// released, and the entry this function inspects, refills and publishes cannot
// change underneath it. Every caller that instead drops a lock and re-takes one
// has to re-walk and re-check afterwards, which is why mem_ptr_fault calls this
// form directly.
static int swap_fault_page_locked(struct mem *mem, page_t page) {
    int err = 0;
    struct pt_entry *pt = mem_pt(mem, page);
    if (pt != NULL &&
        atomic_load_explicit(&pt->swap_state, memory_order_acquire) == PT_SWAPPED) {
        struct data *data = pt->data;
        size_t frame = swap_frame_size();
        size_t offset = pt->offset & ~(frame - 1);
        char *host = (char *) data->data + offset;
        size_t f = offset / frame;

        // Ask the FRAME where its bytes are, not the entry. The entry only
        // said "not here", which may be stale -- a sibling entry's fault, or
        // the same frame's fault in a forked sibling process, can have brought
        // it back since. Under the frame lock so the answer cannot change
        // while it is being acted on.
        pthread_mutex_t *fl = swap_frame_lock_for(data, f);
        pthread_mutex_lock(fl);
        uint32_t slot = data->frame_slot == NULL ? SWAP_SLOT_NONE :
                atomic_load_explicit(&data->frame_slot[f], memory_order_acquire);

        if (slot == SWAP_SLOT_NONE) {
            // CANCELLED: the frame is already in host memory, so there is
            // nothing to read and -- this is the part that matters -- nothing
            // to overwrite. Re-reading the slot here is exactly the silent
            // revert that made the slot the frame's property: it would put the
            // on-disk copy back over whatever the guest has written through a
            // sibling entry since the frame returned.
            swap_stat_cancelled++;
        } else if (mprotect(host, frame, PROT_READ | PROT_WRITE) != 0) {
            // Reverse of the eviction, in reverse order: make it writable, tell
            // the host we want the page back, then refill it.
            err = _EIO;
        } else {
            madvise(host, frame, MADV_FREE_REUSE);
            err = swap_slot_read(slot, host, frame);
            if (err == 0) {
                // The frame is home. Clear the slot BEFORE the lock is dropped
                // and before any entry is published, so that the next fault on
                // any entry of this frame -- in this address space or a forked
                // sibling's -- takes the cancelled path above instead of
                // reading the slot a second time.
                atomic_store_explicit(&data->frame_slot[f], SWAP_SLOT_NONE,
                        memory_order_release);
                // Only THEN give the slot back. The other order would let
                // another frame's eviction take this slot and overwrite it
                // while entries here still named it.
                swap_slot_free(slot);
                swap_stat_faulted++;
                swap_stat_in += frame;
            }
        }
        pthread_mutex_unlock(fl);

        if (err == 0) {
            // Publish every page of the frame, not just the faulting one: they
            // were evicted together and their bytes are all back.
            //
            // The frame's first guest page is found from the DATA OFFSET, not
            // from the guest page number. Section 3.2 states the rule -- "a
            // frame (D, f) is the set of pt_entrys with entry->data == D and
            // entry->offset >> 14 == f; membership is by data offset, not guest
            // adjacency" -- and the gate prototype broke it with
            // `page - (page % pages_per_frame)`, which is only the same thing
            // when the mapping's first guest page happens to be a multiple of
            // four.
            //
            // MEASURED, and it is why this is not a theoretical tidy-up. The
            // arm64 test root put the verifier's 16 MiB region at guest page
            // 0x7fffbcf1c, a multiple of 4, and it passed. The amd64 root put
            // the identical region at 0x7ffffcf59, which is 1 mod 4, so every
            // base came out one page LOW: the swap-in marked the last page of
            // the PREVIOUS frame resident while that frame was still released,
            // and the next read of it took a host fault on a PROT_NONE page.
            // Deterministic, single-threaded, and it killed the verifier with
            // SIGBUS after three faults.
            size_t page_in_frame = (pt->offset & (frame - 1)) / PAGE_SIZE;
            page_t base = page - (page_t) page_in_frame;
            for (size_t i = 0; i < swap_pages_per_frame(); i++) {
                struct pt_entry *e = mem_pt(mem, base + i);
                // Same mapping AND the exact offset this position in the
                // frame must have. Testing only "same frame" would publish a
                // neighbour an mremap moved in from elsewhere, calling bytes
                // resident that this pread never wrote.
                if (e != NULL && e->data == data && e->offset == offset + i * PAGE_SIZE)
                    atomic_store_explicit(&e->swap_state, PT_RESIDENT, memory_order_release);
            }
            // An entry an mremap moved out of this frame is not adjacent any
            // more, so this loop does not reach it and it stays PT_SWAPPED.
            // That is now a hint that is merely stale rather than a bug: its
            // own fault takes the cancelled path above, reads nothing, and
            // publishes just itself. It used to be the silent-revert case,
            // because the entry carried a copy of the slot and would refill
            // the whole frame from it -- over three siblings the guest had
            // written since. scratch mrem.c drives exactly that ordering.
        }
    }
    return err;
}

int swap_fault_page(struct mem *mem, page_t page) {
    // Called with the READ lock held. Upgrade, because bringing a frame back
    // changes host protections and the page table, then drop back to a reader
    // the way every other fault path here does.
    //
    // read_to_write_lock releases the reader before it waits (util/rw_locks.h),
    // so the caller's view of the page table is stale on return and it MUST
    // re-walk. mem_ptr does: its prologue runs before it resolves anything, and
    // for the read case it holds the read lock continuously from here to
    // mem_ptr_nofault, which the evictor cannot get past. A caller that cannot
    // hold the lock across both should call swap_fault_page_locked instead.
    read_to_write_lock(&mem->lock);
    int err = swap_fault_page_locked(mem, page);
    write_to_read_lock(&mem->lock);
    return err;
}

void swap_prototype_stats(unsigned long *evicted_frames, unsigned long *faulted_frames,
                          unsigned long *bytes_out, unsigned long *bytes_in) {
    if (evicted_frames) *evicted_frames = swap_stat_evicted;
    if (faulted_frames) *faulted_frames = swap_stat_faulted;
    if (bytes_out) *bytes_out = swap_stat_out;
    if (bytes_in) *bytes_in = swap_stat_in;
}

unsigned long swap_prototype_cancelled(void) { return swap_stat_cancelled; }

// Bring EVERY evicted frame of one address space back. This is what swapoff
// means: stop using the file, and make sure nothing still needs it.
//
// Under the same barrier eviction takes, for the same reason -- it changes host
// protections and page-table state for an address space whose threads are
// running guest code, and a plain write_lock can stall behind a guest busy-loop
// indefinitely.
//
// Returns the number of frames still out afterwards, which should be zero. It
// can be non-zero honestly: a frame whose entries were all unmapped while it
// was out has no entry left to fault, and its slot is freed by data_destroy
// rather than here.
long swap_fault_mem_all(struct mem *mem) {
    if (mem == NULL)
        return 0;
    long still_out = 0;
    mem_write_lock_pokes_external(mem);
    for (page_t page = 0; page < mem->page_limit; ) {
        struct pt_entry *pt = mem_pt(mem, page);
        if (pt != NULL && pt->data != NULL &&
                atomic_load_explicit(&pt->swap_state, memory_order_acquire) != PT_RESIDENT) {
            if (swap_fault_page_locked(mem, page) < 0)
                still_out++;
        }
        // mem_next_page, not page + 1: page_limit is 2^35 pages for a 64-bit
        // guest, so a linear probe walks tens of billions of empty pages.
        mem_next_page(mem, &page);
    }
    mem_write_unlock_pokes_external(mem);
    return still_out;
}
