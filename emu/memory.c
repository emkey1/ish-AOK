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
#include "emu/tlb.h"
#include "jit/jit.h"
#include "kernel/vdso.h"
#include "kernel/task.h"
#include "fs/fd.h"
#include "fs/mmap_cache.h"
#include "util/sync.h"
#include <dlfcn.h>

// The Evil global lock.  Use sparingly or not at all
extern pthread_mutex_t multicore_lock;
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

extern bool doEnableExtraLocking;
extern pthread_mutex_t extra_lock;
extern dword_t extra_lock_pid;
extern const char extra_lock_comm;

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
            continue; // bitmap bit set just ahead of the chunk store (proc/maps race)
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
    return next != NULL && (next->flags & P_GROWSDOWN);
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

size_t mem_mapped_page_count(struct mem *mem) {
    if (mem == NULL)
        return 0;

    size_t count = 0;
    for (size_t root = 0; root < MEM_PGDIR_ROOT_SIZE; root++) {
        struct pt_directory_chunk *chunk =
            atomic_load_explicit(&mem->pgdir_root[root], memory_order_acquire);
        if (chunk == NULL)
            continue;
        for (size_t mid = 0; mid < MEM_PGDIR_MID_SIZE; mid++) {
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

    for (page_t page = start; page < start + pages; page++) {
        if (mem_pt(mem, page) != NULL)
            pt_unmap(mem, page, 1);
        data->refcount++;
        struct pt_entry *pt = mem_pt_new(mem, page);
        if (pt == NULL) {
            data->refcount--;
            if (data->refcount == 0)
                free(data);
            return _ENOMEM;
        }
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
                fd_close(data->fd);
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
    return pt_map(mem, start, pages, memory, 0, flags | P_ANONYMOUS);
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
        // If they later become accessible, allocate a real host page here.
        if (entry->data->data == NULL && (new_flags & P_RWX) != 0) {
            void *memory = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
            int err = pt_map(mem, page, 1, memory, 0, new_flags);
            if (err < 0)
                return err;
            continue;
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
        if (mem_pt(mem, page) == NULL)
            mem_lazy_fault(mem, page);
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
        if (mem_pt(mem, page) == NULL)
            pt_map_nothing(mem, page, 1, P_WRITE | P_GROWSDOWN);
        write_to_read_lock(&mem->lock);

        entry = mem_pt(mem, page);
    }

    if (entry != NULL && (type == MEM_WRITE || type == MEM_WRITE_PTRACE)) {
        // if page is unwritable, well tough luck
        if (type != MEM_WRITE_PTRACE && !(entry->flags & P_WRITE))
            return NULL;
        
        if (type == MEM_WRITE_PTRACE) {
            // TODO: Is P_WRITE really correct? The page shouldn't be writable without ptrace.
            entry->flags |= P_WRITE | P_COW;
        }
#if ENGINE_JIT
        // get rid of any compiled blocks in this page
        jit_invalidate_page(mem->mmu.jit, page);
#endif
        
        // if page is cow, ~~milk~~ copy it
        
        if (entry->flags & P_COW) {
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
            void *copy = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
            void *data = (char *) entry->data->data + entry->offset;

            if (copy == MAP_FAILED) {
                if (locked_general_lock)
                    unlock(&current->general_lock);
                write_to_read_lock(&mem->lock);
                return NULL;
            }
            memcpy(copy, data, PAGE_SIZE);
            pt_map(mem, page, 1, copy, 0, entry->flags &~ P_COW);
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
        pt_map_nothing(mem, page, 1, P_WRITE | P_GROWSDOWN);
        entry = mem_pt(mem, page);
    }

    if (entry != NULL && (type == MEM_WRITE || type == MEM_WRITE_PTRACE)) {
        if (type != MEM_WRITE_PTRACE && !(entry->flags & P_WRITE)) {
            write_unlock(&mem->lock);
            return NULL;
        }
        if (type == MEM_WRITE_PTRACE)
            entry->flags |= P_WRITE | P_COW;
#if ENGINE_JIT
        jit_invalidate_page(mem->mmu.jit, page);
#endif
        if (entry->flags & P_COW) {
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
                void *copy = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
                void *data = (char *) entry->data->data + entry->offset;
                if (copy == MAP_FAILED) {
                    if (locked_general_lock)
                        unlock(&current->general_lock);
                    write_unlock(&mem->lock);
                    return NULL;
                }
                memcpy(copy, data, PAGE_SIZE);
                pt_map(mem, page, 1, copy, 0, entry->flags & ~P_COW);
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
        return;
    }

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
            return;
        }
        if (write(fd, entry->data->data, PAGE_SIZE) < 0) {
            perror("write");
            return;
        }
    }
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
