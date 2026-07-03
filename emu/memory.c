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
#include "jit/jit.h"
#include "kernel/vdso.h"
#include "kernel/task.h"
#include "fs/fd.h"
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

static int mem_mirror_host_page_protection(struct pt_entry *entry, int flags) {
    if (!mem_uses_host_page_mirroring())
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
    atomic_init(&mem->quiesce_requested, 0);
    pthread_mutex_init(&mem->quiesce_park_lock, NULL);
    pthread_cond_init(&mem->quiesce_park_cond, NULL);
    mem->mmu.ops = &mem_mmu_ops;
    mem->mmu.requires_write_revalidate = mem_uses_host_page_mirroring();
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

page_t pt_find_hole(struct mem *mem, pages_t size) {
    if (size == 0 || mem->mmap_ceiling <= mem->mmap_floor)
        return BAD_PAGE;
    if (size > mem->mmap_ceiling - mem->mmap_floor)
        return BAD_PAGE;

    page_t best = BAD_PAGE;
    page_t prev_end = mem->mmap_floor;
    page_t page = mem_next_mapped_page(mem, mem->mmap_floor);
    while (page != BAD_PAGE && page < mem->mmap_ceiling) {
        if (page > prev_end && page - prev_end >= size)
            best = page - size;

        page_t region_end = mem_next_unmapped_page(mem, page + 1);
        if (region_end > mem->mmap_ceiling)
            region_end = mem->mmap_ceiling;
        prev_end = region_end;
        page = region_end >= mem->mmap_ceiling ? BAD_PAGE
             : mem_next_mapped_page(mem, region_end);
    }
    if (mem->mmap_ceiling - prev_end >= size)
        best = mem->mmap_ceiling - size;
    return best;
}

bool pt_is_hole(struct mem *mem, page_t start, pages_t pages) {
    if (!mem_page_range_valid(mem, start, pages))
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

    // If this fails, the munmap in pt_unmap would probably fail.
    assert(memory == NULL || (uintptr_t) memory % real_page_size == 0 || memory == vdso_data);

    struct data *data = malloc(sizeof(struct data));
    if (data == NULL)
        return _ENOMEM;
    *data = (struct data) {
        .data = memory,
        .size = pages * PAGE_SIZE + offset,
        .shared_key = 0,

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
        memset(data->host_page_prot, P_READ | P_WRITE, host_pages);
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
        if (mem_pt(mem, page) == NULL)
            return -1;
    return pt_unmap_always(mem, start, pages);
}

int pt_unmap_always(struct mem *mem, page_t start, pages_t pages) {
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
                int err = munmap(data->data, data->size);
                if (err != 0)
                    die("munmap(%p, %lu) failed: %s", data->data, data->size, strerror(errno));
            }
            if (data->fd != NULL) {
                fd_close(data->fd);
            }
            free(data->host_page_prot);
            free(data);
        }
    }
    mem_changed(mem);
    return 0;
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
    if (!mem_page_range_valid(src, start, pages) || !mem_page_range_valid(dst, start, pages))
        return -1;
    page_t end = start + pages;
    for (page_t page = mem_next_mapped_page(src, start);
         page != BAD_PAGE && page < end;
         page = mem_next_mapped_page(src, page + 1)) {
        struct pt_entry *entry = mem_pt(src, page);
        if (entry == NULL)
            continue;
        if (pt_unmap_always(dst, page, 1) < 0)
            return -1;
        if (!(entry->flags & P_SHARED))
            entry->flags |= P_COW;
        entry->data->refcount++;
        struct pt_entry *dst_entry = mem_pt_new(dst, page);
        if (dst_entry == NULL) {
            entry->data->refcount--;
            return -1;
        }
        dst_entry->data = entry->data;
        dst_entry->offset = entry->offset;
        dst_entry->flags = entry->flags;
    }
    mem_changed(src);
    mem_changed(dst);
    
    return 0;
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

    if (entry == NULL) {
        // page does not exist
        // look to see if the next VM region is willing to grow down
        page_t p = page + 1;
        p = mem_next_mapped_page(mem, p);
        if (p == BAD_PAGE || p >= mem->page_limit)
            return NULL;
        if (!(mem_pt(mem, p)->flags & P_GROWSDOWN))
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
    if (entry == NULL) {
        page_t p = page + 1;
        p = mem_next_mapped_page(mem, p);
        if (p == BAD_PAGE || p >= mem->page_limit) {
            write_unlock(&mem->lock);
            return NULL;
        }
        struct pt_entry *next = mem_pt(mem, p);
        if (next == NULL || !(next->flags & P_GROWSDOWN)) {
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
    if (mem == NULL)
        return;

    if (value != 1 && value != -1) {
        printk("ERROR: invalid mem refcount delta %d\n", value);
        return;
    }

    int old_count = atomic_load_explicit(&mem->reference.count, memory_order_relaxed);
    for (;;) {
        int new_count = old_count + value;
        if (new_count < 0) {
            void *caller = __builtin_return_address(0);
            Dl_info caller_info = {};
            const char *caller_name = "?";
            ptrdiff_t caller_offset = 0;
            if (caller != NULL && dladdr(caller, &caller_info) != 0 && caller_info.dli_sname != NULL) {
                caller_name = caller_info.dli_sname;
                caller_offset = (char *) caller - (char *) caller_info->dli_saddr;
            }
            printk("ERROR: Attempt to decrement mem reference count to be negative, ignoring(%d:%d) caller=%s+%td addr=%p mem=%p\n",
                   old_count, value, caller_name, caller_offset, caller, mem);
            return;
        }
        if (atomic_compare_exchange_weak_explicit(&mem->reference.count, &old_count, new_count,
                                                   memory_order_acq_rel, memory_order_relaxed))
            return;
    }
}

int mem_ref_cnt_get(struct mem *mem) {
    if (mem == NULL)
        return 0;
    int cnt = (int) atomic_load_explicit(&mem->reference.count, memory_order_relaxed);
    if ((cnt < 0) || (cnt > 1000))
        cnt = 0;
    return cnt;
}
