#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "kernel/swap.h"
#include "kernel/errno.h"
#include "kernel/task.h"
#include "kernel/mm.h"
#include "emu/memory.h"
#include "util/sync.h"
#include "misc.h"

// fs/real.h is not included here on purpose -- one declaration is a better
// trade than the whole filesystem header. host_unlinked_tmpfd creates a file in
// TMPDIR and unlinks it immediately, which is the shape the pager wants: the
// swap file must never be visible in the container, backed up, or reachable
// through the File Provider (section 3.11).
int host_unlinked_tmpfd(void);

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
//
// One lock over the whole module. It is a LEAF: nothing else is taken under it,
// and it is NEVER held across I/O. swap_slot_write and swap_slot_read do their
// pread/pwrite outside it, which they can because a slot's owner is the frame
// that holds it and no two frames hold the same slot; swap_enable builds and
// reserves the area outside it and publishes the result under it; and the
// teardown paths detach the descriptor under it and ftruncate/close outside
// (swap_file_detach_locked and swap_file_dispose).
//
// The lock ordering that matters is the one this module does NOT participate
// in. The pager calls swap_slot_alloc under the address-space barrier and
// swap_slot_free under a mem write lock plus the per-frame lock, so this lock
// sits below all of them. It must therefore never call back into emu/memory.c
// or take a task lock while held. swap_disable is the exception and is written
// to drop it before it walks anything.
static lock_t swap_lock = LOCK_INITIALIZER;

// Serialises the whole of an enable against the whole of a disable, which
// swap_lock cannot: swap_disable has to DROP swap_lock to walk address spaces
// (it takes task and address-space locks, which sit above it), and a
// swap_enable arriving in that window used to create a file that the disable
// then closed, leaving swap_on true over no file at all -- a pager reporting
// itself on with nothing behind it.
//
// Strictly above swap_lock and above the address-space barrier. Nothing takes
// it from below: its only holders are swap_enable and swap_disable, reached
// from guest boot, the app's preferences, and a write to /proc/ish/swap, none
// of which hold a mem lock.
static lock_t swap_config_lock = LOCK_INITIALIZER;

// Read on the eviction path without the lock, so atomic. Eviction only; the
// fault path deliberately does not consult it (see the header).
static _Atomic bool swap_on;

static int swap_fd = -1;
static uint64_t swap_slot_size;        // one host frame
static uint32_t swap_slot_count;       // slots in the area, INCLUDING slot 0
static uint32_t swap_slots_used;       // allocated, i.e. out on disk
static uint64_t *swap_free_bitmap;     // 1 = free
static uint32_t swap_alloc_rover;      // where the last search stopped

static _Atomic uint64_t swap_stat_alloc_fail;
// Evictions refused because there was no swap area at all, as opposed to an
// area that is full. Distinct counters because they mean different things: one
// says the user did not ask for swap, the other says they asked for too little.
static _Atomic uint64_t swap_stat_no_area;
static _Atomic uint64_t swap_stat_io_errors;
static _Atomic uint64_t swap_stat_bytes_written;
// Bytes released by direct reclaim, i.e. by a guest allocation that would
// otherwise have been refused. Reported separately from total eviction because
// it is the number that says whether the feature is doing its job.
static _Atomic uint64_t swap_stat_direct_bytes;

// Set at startup when ISH_GUEST_SWAP_MB was present, which is the CLI and
// Xcode-scheme path. See swap_guest_control_allowed().
static _Atomic bool swap_guest_control;

// The app's Settings, remembered even when they arrive before swap_startup().
static _Atomic bool swap_pref_seen;
static _Atomic bool swap_pref_enabled;
static _Atomic unsigned swap_pref_size_mb;

bool swap_enabled(void) {
    return atomic_load_explicit(&swap_on, memory_order_relaxed);
}

bool swap_guest_control_allowed(void) {
    return atomic_load_explicit(&swap_guest_control, memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// The free bitmap
// ---------------------------------------------------------------------------

#define SWAP_BITS_PER_WORD 64

static bool swap_bit_test(uint32_t i) {
    return (swap_free_bitmap[i / SWAP_BITS_PER_WORD] >>
            (i % SWAP_BITS_PER_WORD)) & 1;
}
static void swap_bit_set(uint32_t i) {
    swap_free_bitmap[i / SWAP_BITS_PER_WORD] |=
        (uint64_t) 1 << (i % SWAP_BITS_PER_WORD);
}
static void swap_bit_clear(uint32_t i) {
    swap_free_bitmap[i / SWAP_BITS_PER_WORD] &=
        ~((uint64_t) 1 << (i % SWAP_BITS_PER_WORD));
}

// ---------------------------------------------------------------------------
// The file
// ---------------------------------------------------------------------------

// Reserve `bytes` of real blocks, so that a later pwrite cannot fail with
// ENOSPC halfway through an eviction that has already marked its entries
// absent. F_PREALLOCATE is the Darwin spelling; ftruncate alone would give a
// sparse file that reports the right size and has no blocks behind it.
//
// F_ALLOCATEALL rather than F_ALLOCATECONTIG: contiguity is a nice-to-have that
// fails outright on a fragmented volume, and section 3.11's concern is extent
// COUNT after churn, which the slot allocator's reuse policy governs far more
// than the initial layout does.
static int swap_reserve(int fd, uint64_t bytes) {
#ifdef F_PREALLOCATE
    fstore_t store = {
        .fst_flags = F_ALLOCATEALL,
        .fst_posmode = F_PEOFPOSMODE,
        .fst_offset = 0,
        .fst_length = (off_t) bytes,
    };
    if (fcntl(fd, F_PREALLOCATE, &store) != 0) {
        // Not fatal on its own: the ftruncate below still sets the size, and a
        // filesystem that cannot preallocate (or a Linux host, where this fcntl
        // does not exist) is not a reason to refuse swap. What it costs is the
        // guarantee that a later write has somewhere to go, so ENOSPC becomes
        // an I/O error at eviction instead of a refusal at enable.
        //
        // Recorded rather than silently accepted, because the failure it stops
        // being able to prevent is the one that matters.
        return errno == ENOSPC ? _ENOSPC : 0;
    }
#endif
    (void) bytes;
    return 0;
}

// Detach the area from the module under swap_lock and hand the descriptor and
// bitmap back to the caller, which disposes of them with the lock DROPPED.
//
// The split exists because ftruncate and close are I/O, and this module's whole
// lock story is that swap_lock is a leaf that is never held across I/O -- a
// claim that was not actually true while the close path did both under it.
static int swap_file_detach_locked(uint64_t **bitmap_out) {
    int fd = swap_fd;
    *bitmap_out = swap_free_bitmap;
    swap_fd = -1;
    swap_free_bitmap = NULL;
    swap_slot_count = 0;
    swap_slots_used = 0;
    swap_alloc_rover = 0;
    swap_slot_size = 0;
    return fd;
}

static void swap_file_dispose(int fd, uint64_t *bitmap) {
    if (fd >= 0) {
        // Truncate before closing: the file is already unlinked, so this is
        // what actually hands the blocks back rather than waiting for the
        // process to exit. A clean exit should leave nothing behind.
        if (ftruncate(fd, 0) != 0)
            /* nothing useful to do; the unlink still frees it at exit */;
        close(fd);
    }
    free(bitmap);
}

// ---------------------------------------------------------------------------
// Enable and disable
// ---------------------------------------------------------------------------

static void swap_disable_locked(void);

static int swap_enable_locked(uint64_t bytes) {
    uint64_t frame = mem_frame_size();
    if (frame == 0)
        return _EINVAL;
    // Rounded DOWN, and slot 0 is reserved, so the user gets whole frames of
    // usable area and never more than they asked for.
    uint64_t usable_slots = bytes / frame;
    if (usable_slots == 0)
        return _EINVAL;
    // +1 for the reserved slot 0. UINT32_MAX slots is 64 TiB; the clamp is
    // here so the arithmetic below cannot wrap rather than because anyone will
    // reach it.
    if (usable_slots >= UINT32_MAX - 1)
        return _EINVAL;
    uint32_t slots = (uint32_t) usable_slots + 1;

    lock(&swap_lock, 0);
    if (swap_fd >= 0) {
        bool same_size = swap_slot_count == slots && swap_slot_size == frame;
        bool draining = !atomic_load_explicit(&swap_on, memory_order_relaxed);
        unlock(&swap_lock);
        if (same_size && !draining) {
            return 0;               // idempotent
        } else if (draining) {
            // A previous swap_disable could not get everything back, so the
            // file is still answering faults. Re-enabling over it would hand
            // out slots that those frames still name.
            return _EBUSY;
        }
        // A different size is a disable then an enable, and the disable has to
        // happen with swap_lock dropped because it walks address spaces. The
        // config lock is still held throughout, so nothing else can enable in
        // the gap.
        swap_disable_locked();
        lock(&swap_lock, 0);
        if (swap_fd >= 0) {
            unlock(&swap_lock);
            return _EBUSY;          // still draining after the disable
        }
    }

    unlock(&swap_lock);

    // Built with swap_lock DROPPED: creating and reserving the area is
    // mkstemp + F_PREALLOCATE + ftruncate over as much as 16 GiB, and holding
    // a leaf lock that every fault path takes across that would stall the whole
    // guest. Nothing else can be enabling concurrently -- swap_config_lock is
    // held for the whole of this function -- and the state is published under
    // swap_lock at the end.
    uint64_t total = (uint64_t) slots * frame;
    uint32_t words = (slots + SWAP_BITS_PER_WORD - 1) / SWAP_BITS_PER_WORD;
    uint64_t *bitmap = calloc(words, sizeof(*bitmap));
    if (bitmap == NULL)
        return _ENOMEM;

    int fd = host_unlinked_tmpfd();
    if (fd < 0) {
        free(bitmap);
        return fd;
    }
    int err = swap_reserve(fd, total);
    if (err == 0 && ftruncate(fd, (off_t) total) != 0)
        err = errno == ENOSPC ? _ENOSPC : _EIO;
    if (err != 0) {
        close(fd);
        free(bitmap);
        return err;                 // never a smaller area than was asked for
    }

    lock(&swap_lock, 0);
    swap_fd = fd;
    swap_slot_size = frame;
    swap_slot_count = slots;
    swap_slots_used = 0;
    swap_free_bitmap = bitmap;
    // Every slot free except 0, which is SWAP_SLOT_NONE and must never be
    // handed out.
    for (uint32_t i = 1; i < slots; i++)
        swap_bit_set(i);
    swap_alloc_rover = 1;
    atomic_store_explicit(&swap_stat_bytes_written, 0, memory_order_relaxed);
    atomic_store_explicit(&swap_on, true, memory_order_release);
    unlock(&swap_lock);
    printk("swap: enabled, %llu MB in %u slots of %llu bytes\n",
           (unsigned long long) (total / (1024 * 1024)),
           slots - 1, (unsigned long long) frame);
    return 0;
}

static void swap_disable_locked(void) {
    // Stop new eviction FIRST and outside everything else, so the walk below
    // is chasing a set that can only shrink.
    atomic_store_explicit(&swap_on, false, memory_order_release);

    lock(&swap_lock, 0);
    bool anything_to_do = swap_fd >= 0;
    unlock(&swap_lock);
    if (!anything_to_do)
        return;

    // Bring every evicted page back, one address space at a time.
    //
    // EVERY TASK, not just thread-group leaders. Summing leaders is the right
    // shape for counting address spaces once (fs/proc/root.c's
    // collect_mem_page_stats does exactly that), but this is not counting: an
    // address space has to be REACHED, and a thread group whose leader called
    // pthread_exit() has a live mm with no live leader. Missing one means
    // swapoff never brings its pages back, so swap_slots_used never reaches
    // zero, so the file is kept for ever and swap_enable refuses with EBUSY
    // from then on. Leaders-only is therefore wrong in the one direction that
    // matters, and duplicates are merely wasted work.
    //
    // Duplicates ARE the common case -- every thread of a group names the same
    // mm -- so they are filtered by mm pointer rather than paid for: the walk
    // takes the address-space barrier, which stops every thread of that
    // process, and doing it once per thread would be quadratic in an
    // 8-thread guest.
    //
    // The trylock-and-skip is the pattern collect_mem_page_stats documents at
    // length: do_exit spins in exit_wait_backoff() while holding general_lock,
    // and the snapshot reference we hold keeps that loop alive, so a blocking
    // lock here deadlocks. A task we cannot lock is mid-exit and its address
    // space is about to be torn down, which frees its slots anyway.
    struct task_snapshot snapshot = {0};
    if (task_snapshot_collect(&snapshot, false) == 0) {
        struct mm **seen = calloc(snapshot.count, sizeof(*seen));
        unsigned seen_count = 0;
        for (unsigned i = 0; i < snapshot.count; i++) {
            struct task *task = snapshot.tasks[i];
            if (trylock(&task->general_lock) != 0)
                continue;
            struct mm *mm = task->mm;
            if (mm != NULL)
                mm_retain(mm);
            unlock(&task->general_lock);
            if (mm == NULL)
                continue;
            bool already = false;
            if (seen != NULL) {
                for (unsigned j = 0; j < seen_count && !already; j++)
                    already = seen[j] == mm;
                if (!already)
                    seen[seen_count++] = mm;
            }
            // A failed calloc only costs repeated work, never correctness, so
            // it does not fail the swapoff.
            if (!already)
                swap_fault_mem_all(&mm->mem);
            mm_release(mm);
        }
        free(seen);
        task_snapshot_release(&snapshot);
    }

    lock(&swap_lock, 0);
    if (swap_slots_used != 0) {
        // Something is still out: an address space whose task was already
        // exiting when the snapshot was taken, or one that could not be locked.
        // KEEP THE FILE. Those frames are mprotect(PROT_NONE) with their bytes
        // only in this file, and closing it turns their next touch into a
        // permanent SIGSEGV on memory the guest mapped correctly.
        unlock(&swap_lock);
        printk("swap: disabled, but %u slots are still out; keeping the file "
               "open so those faults can still be answered\n", swap_slots_used);
        return;
    }
    uint64_t *bitmap = NULL;
    int fd = swap_file_detach_locked(&bitmap);
    unlock(&swap_lock);
    swap_file_dispose(fd, bitmap);
    printk("swap: disabled\n");
}

int swap_enable(uint64_t bytes) {
    lock(&swap_config_lock, 0);
    int err = swap_enable_locked(bytes);
    unlock(&swap_config_lock);
    return err;
}

void swap_disable(void) {
    lock(&swap_config_lock, 0);
    swap_disable_locked();
    unlock(&swap_config_lock);
}

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------

int swap_slot_alloc(uint32_t *slot_out) {
    lock(&swap_lock, 0);
    // The switch is tested HERE as well as at the top of a sweep. A sweep can
    // span a swapoff -- it walks a whole address space under the barrier, and
    // the swapoff is walking the others -- and a slot handed out after the
    // swapoff has already visited this address space would never be brought
    // back by it. swap_slots_used would then never reach zero, the file would
    // be kept for ever, and swap_enable would refuse with EBUSY from then on.
    if (!atomic_load_explicit(&swap_on, memory_order_acquire) ||
            swap_fd < 0 || swap_free_bitmap == NULL) {
        unlock(&swap_lock);
        // Counted, because "there was no area to evict into" and "the area the
        // user chose is full" are different things the /proc report should not
        // conflate -- and the first used to be invisible in every counter.
        atomic_fetch_add_explicit(&swap_stat_no_area, 1, memory_order_relaxed);
        return _ENOSPC;
    }
    // A rover rather than a scan from 0: reusing the slot that came free most
    // recently keeps the working set of the file small, which is what section
    // 3.11 cares about (extent count after churn), and makes the common
    // allocate-free-allocate cycle O(1).
    uint32_t start = swap_alloc_rover;
    if (start == 0 || start >= swap_slot_count)
        start = 1;
    uint32_t i = start;
    do {
        if (swap_bit_test(i)) {
            swap_bit_clear(i);
            swap_slots_used++;
            swap_alloc_rover = i + 1 >= swap_slot_count ? 1 : i + 1;
            unlock(&swap_lock);
            *slot_out = i;
            return 0;
        }
        i = i + 1 >= swap_slot_count ? 1 : i + 1;
    } while (i != start);
    unlock(&swap_lock);
    // The area the user asked for is full. Not an error: the caller simply
    // does not evict this frame.
    atomic_fetch_add_explicit(&swap_stat_alloc_fail, 1, memory_order_relaxed);
    return _ENOSPC;
}

void swap_slot_free(uint32_t slot) {
    if (slot == SWAP_SLOT_NONE)
        return;
    lock(&swap_lock, 0);
    if (swap_free_bitmap == NULL || slot >= swap_slot_count) {
        unlock(&swap_lock);
        return;
    }
    if (!swap_bit_test(slot)) {
        swap_bit_set(slot);
        if (swap_slots_used > 0)
            swap_slots_used--;
        // Hand it back to the rover, so the next eviction reuses these blocks
        // rather than extending the file's live footprint.
        swap_alloc_rover = slot;
    }
    bool drained = swap_slots_used == 0 &&
        !atomic_load_explicit(&swap_on, memory_order_relaxed) && swap_fd >= 0;
    int fd = -1;
    uint64_t *bitmap = NULL;
    if (drained) {
        // The last frame a stopped pager still had out has come home. Now the
        // file can go, which is what makes swap_disable's "keeping the file
        // open" state temporary rather than permanent.
        fd = swap_file_detach_locked(&bitmap);
    }
    unlock(&swap_lock);
    if (drained) {
        // Outside the lock: this is a caller on the fault path, and it must not
        // hold a leaf lock through an ftruncate.
        swap_file_dispose(fd, bitmap);
        printk("swap: the last outstanding slot came back; file released\n");
    }
}

// The descriptor is read under the lock and used outside it. That is safe
// because the file is only ever closed when no slot is out (swap_slot_free's
// drain, or swap_disable finding used == 0), and holding a slot is precisely
// what a caller of these two functions is doing.
static int swap_fd_for_slot(uint32_t slot, off_t *at_out, size_t len) {
    lock(&swap_lock, 0);
    int fd = swap_fd;
    bool ok = fd >= 0 && slot != SWAP_SLOT_NONE && slot < swap_slot_count &&
              len == swap_slot_size;
    off_t at = ok ? (off_t) slot * (off_t) swap_slot_size : 0;
    unlock(&swap_lock);
    if (!ok)
        return -1;
    *at_out = at;
    return fd;
}

int swap_slot_write(uint32_t slot, const void *buf, size_t len) {
    off_t at;
    int fd = swap_fd_for_slot(slot, &at, len);
    if (fd < 0)
        return _EIO;
    size_t done = 0;
    while (done < len) {
        ssize_t n = pwrite(fd, (const char *) buf + done, len - done,
                           at + (off_t) done);
        if (n <= 0) {
            atomic_fetch_add_explicit(&swap_stat_io_errors, 1, memory_order_relaxed);
            return _EIO;
        }
        done += (size_t) n;
    }
    atomic_fetch_add_explicit(&swap_stat_bytes_written, len, memory_order_relaxed);
    return 0;
}

int swap_slot_read(uint32_t slot, void *buf, size_t len) {
    off_t at;
    int fd = swap_fd_for_slot(slot, &at, len);
    if (fd < 0)
        return _EIO;
    size_t done = 0;
    while (done < len) {
        ssize_t n = pread(fd, (char *) buf + done, len - done, at + (off_t) done);
        if (n <= 0) {
            atomic_fetch_add_explicit(&swap_stat_io_errors, 1, memory_order_relaxed);
            return _EIO;
        }
        done += (size_t) n;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Direct reclaim
// ---------------------------------------------------------------------------

// The slice one call will try for. Small enough that an mmap() does not stall
// for a second (100-200 MiB/s per process, so 32 MiB is 160-320 ms at worst),
// large enough to be worth the barrier it costs -- the barrier is scheduler
// bound and also costs every sibling thread of the process about 109 us of its
// own CPU, so reclaiming 64 KiB per acquisition would be nearly all overhead.
#define SWAP_RECLAIM_MIN_BYTES (4ull * 1024 * 1024)
#define SWAP_RECLAIM_MAX_BYTES (32ull * 1024 * 1024)

long swap_direct_reclaim(struct mem *mem, uint64_t want_bytes) {
    // Off is the default, and in it this is one relaxed atomic load on the
    // guest's mmap path and nothing else.
    if (!swap_enabled() || mem == NULL)
        return 0;
    if (want_bytes < SWAP_RECLAIM_MIN_BYTES)
        want_bytes = SWAP_RECLAIM_MIN_BYTES;
    if (want_bytes > SWAP_RECLAIM_MAX_BYTES)
        want_bytes = SWAP_RECLAIM_MAX_BYTES;
    long released = swap_evict_bytes(mem, want_bytes);
    if (released > 0)
        atomic_fetch_add_explicit(&swap_stat_direct_bytes, (uint64_t) released,
                memory_order_relaxed);
    return released > 0 ? released : 0;
}

// ---------------------------------------------------------------------------
// Preferences
// ---------------------------------------------------------------------------

// RECORDS ONLY. Nothing here allocates, reserves or opens anything; only
// swap_startup() acts, and only on a launch that is actually going to run a
// guest.
//
// That separation is not tidiness. This is a KVO observer on the app's
// preferences, registered with NSKeyValueObservingOptionInitial, so it fires on
// every launch whether or not a guest ever boots. MEASURED when it did act
// here: an iPhone 17 simulator sitting on the Filesystems picker with no rootfs
// installed -- a launch that reached RecordBootFailure(_ENOENT) and never
// started a guest -- had a 536887296-byte swap file open in its container,
// because the observer had already called swap_enable(). lsof named it.
//
// It is also what section 3.13 asks for: swap is sized once at launch and is
// deliberately not resizable in place, so a preference changed now is a
// preference for the NEXT launch. A running pager is left alone, in both
// directions -- turning the switch off does not tear down an area whose frames
// the guest is still faulting on.
void swap_set_preference(bool enabled, unsigned size_mb) {
    atomic_store_explicit(&swap_pref_enabled, enabled, memory_order_relaxed);
    atomic_store_explicit(&swap_pref_size_mb, size_mb, memory_order_relaxed);
    atomic_store_explicit(&swap_pref_seen, true, memory_order_release);
}

void swap_startup(void) {
    if (atomic_load_explicit(&swap_pref_seen, memory_order_acquire)) {
        if (!atomic_load_explicit(&swap_pref_enabled, memory_order_relaxed))
            return;
        unsigned mb = atomic_load_explicit(&swap_pref_size_mb, memory_order_relaxed);
        if (mb == 0) {
            // Refused rather than guessed, and said out loud. The UI is
            // required to ask for a size; picking one here would be AOK
            // deciding how much of the user's flash to write to. "Asked for
            // with no size" and "asked for and could not start" have to stay
            // distinguishable, which is why the app hands both halves across
            // rather than folding them into one flag.
            printk("swap: enabled in Settings but no size chosen; staying off\n");
            return;
        }
        int err = swap_enable((uint64_t) mb * 1024 * 1024);
        if (err < 0)
            printk("swap: enable failed (%d)\n", err);
        return;
    }
    // The CLI and Xcode-scheme path. Nothing on this branch is reachable from
    // an App Store install, where the preference above is the only way in.
    const char *mb = getenv("ISH_GUEST_SWAP_MB");
    if (mb == NULL || mb[0] == '\0')
        return;
    // The launch offered guest control, so /proc/ish/swap becomes writable for
    // root. Set even when the size below turns out to be 0 or garbage: what it
    // records is that this is a CLI or Xcode run, not that swap came up.
    atomic_store_explicit(&swap_guest_control, true, memory_order_relaxed);
    long want = strtol(mb, NULL, 10);
    if (want <= 0)
        return;
    int err = swap_enable((uint64_t) want * 1024 * 1024);
    if (err < 0)
        printk("swap: ISH_GUEST_SWAP_MB=%s refused (%d)\n", mb, err);
}

// ---------------------------------------------------------------------------
// Counters
// ---------------------------------------------------------------------------

void swap_get_stats(struct swap_stats *out) {
    memset(out, 0, sizeof(*out));
    lock(&swap_lock, 0);
    out->slot_size = swap_slot_size;
    // Slot 0 is not usable area and is not counted, so SwapTotal is exactly
    // what the user asked for (rounded down to frames).
    out->slots_total = swap_slot_count > 0 ? swap_slot_count - 1 : 0;
    out->slots_free = out->slots_total - swap_slots_used;
    out->total_bytes = out->slots_total * swap_slot_size;
    out->free_bytes = out->slots_free * swap_slot_size;
    // Both read under the lock, and both from the same sample: reading
    // `enabled` after dropping it let /proc/ish/swap print a pair that was
    // never true at once -- "on" beside a slot count from before an enable, or
    // "off, draining" beside the totals of an area that had just been created.
    out->enabled = atomic_load_explicit(&swap_on, memory_order_relaxed);
    out->draining = swap_fd >= 0 && !out->enabled;
    unlock(&swap_lock);
    out->alloc_failures = atomic_load_explicit(&swap_stat_alloc_fail, memory_order_relaxed);
    out->no_area = atomic_load_explicit(&swap_stat_no_area, memory_order_relaxed);
    out->direct_reclaim_bytes = atomic_load_explicit(&swap_stat_direct_bytes, memory_order_relaxed);
    out->io_errors = atomic_load_explicit(&swap_stat_io_errors, memory_order_relaxed);
    out->bytes_written = atomic_load_explicit(&swap_stat_bytes_written, memory_order_relaxed);
}

size_t swap_status_text(char *buf, size_t size) {
    if (buf == NULL || size == 0)
        return 0;
    struct swap_stats s;
    swap_get_stats(&s);
    int n = snprintf(buf, size,
        "enabled          %s\n"
        "state            %s\n"
        "slot_size        %llu\n"
        "slots_total      %llu\n"
        "slots_free       %llu\n"
        "total_bytes      %llu\n"
        "free_bytes       %llu\n"
        "bytes_written    %llu\n"
        "direct_reclaim   %llu  (bytes freed for an allocation that would have failed)\n"
        "alloc_failures   %llu  (evictions refused, the area is full)\n"
        "no_area          %llu  (evictions refused, there is no area)\n"
        "io_errors        %llu\n",
        s.enabled ? "yes" : "no",
        s.enabled ? "on" : (s.draining ? "off, draining" : "off"),
        (unsigned long long) s.slot_size,
        (unsigned long long) s.slots_total,
        (unsigned long long) s.slots_free,
        (unsigned long long) s.total_bytes,
        (unsigned long long) s.free_bytes,
        (unsigned long long) s.bytes_written,
        (unsigned long long) s.direct_reclaim_bytes,
        (unsigned long long) s.alloc_failures,
        (unsigned long long) s.no_area,
        (unsigned long long) s.io_errors);
    if (n < 0)
        return 0;
    return (size_t) n >= size ? size - 1 : (size_t) n;
}
