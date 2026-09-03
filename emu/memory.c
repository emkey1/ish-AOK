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
#include "emu/memory.h"
#include "fs/fd.h"
#include "emu/tlb.h"
#include "jit/jit.h"
#include "kernel/vdso.h"
#include "kernel/task.h"
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
size_t mem_mapped_page_count(struct mem *mem) {
    if (mem == NULL)
        return 0;

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
                if (entries[i].data != NULL)
                    count++;
            }
        }
    }
    return count;
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
            free(data);
            return _ENOMEM;
        }
    }

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

static int pt_unmap_always_unlocked(struct mem *mem, page_t start, pages_t pages) {
    if (!mem_page_range_valid(mem, start, pages))
        return -1;
    page_t end = start + pages;
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
        mem_pt_del(mem, page);
        if (--data->refcount == 0) {
            // vdso wasn't allocated with mmap, it's just in our data segment
            if (data->data != NULL && data->data != vdso_data) {
                if (mem_quarantine_freed_pages()) {
                    // Debug mode: keep the range reserved and make it fault
                    // rather than handing it back. See the helper above.
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
                // The last reference to this mapping is going away; if it was
                // holding a memfd's write seal off, stop holding it.
                if (data->memfd_shared_mapped)
                    memfd_mapping_released(data->fd);
                // NOT fd_close: this runs under the address-space write lock
                // with the other threads quiesced, and a ->close may block on
                // one of them. See struct mem's deferred_fds.
                mem_defer_fd_close(mem, data->fd);
            }
            mmap_cache_unregister(data->cache_entry);
            free(data->host_page_prot);
            free(data);
        }
    }
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
    for (; mapped < pages; mapped++) {
        struct pt_entry *src = mem_pt(mem, old_start + mapped);
        struct pt_entry *dst = mem_pt_new(mem, new_start + mapped);
        if (dst == NULL) {
            pt_unmap_always(mem, new_start, mapped);
            return _ENOMEM;
        }
        src->data->refcount++;
        dst->data = src->data;
        dst->offset = src->offset;
        dst->flags = src->flags;
    }
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
    for (; mapped < pages; mapped++) {
        struct pt_entry *src = mem_pt(mem, old_start + mapped);
        struct pt_entry *dst = mem_pt_new(mem, new_start + mapped);
        if (dst == NULL) {
            pt_unmap_always(mem, new_start, mapped);
            return _ENOMEM;
        }
        src->data->refcount++;
        dst->data = src->data;
        dst->offset = src->offset;
        dst->flags = src->flags;
    }

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
    return e->data->data != NULL;
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
    for (page_t page = mem_next_mapped_page(src, start);
         page != BAD_PAGE && page < end;
         page = mem_next_mapped_page(src, page + 1)) {
        struct pt_entry *entry = mem_pt(src, page);
        if (entry == NULL)
            continue;
        if (pt_unmap_always_unlocked(dst, page, 1) < 0) {
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
            if (pt_map_nothing(dst, page, 1, entry->flags & ~P_COW) < 0) {
                ret = -1;
                break;
            }
            continue;
        }
        if (!(entry->flags & P_SHARED))
            entry->flags |= P_COW;
        entry->data->refcount++;
        struct pt_entry *dst_entry = mem_pt_new(dst, page);
        if (dst_entry == NULL) {
            entry->data->refcount--;
            ret = -1;
            break;
        }
        dst_entry->data = entry->data;
        dst_entry->offset = entry->offset;
        dst_entry->flags = entry->flags;
    }
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
