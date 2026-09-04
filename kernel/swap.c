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
// and it is never held across I/O -- swap_slot_write and swap_slot_read do
// their pread/pwrite outside it, which they can because a slot's owner is the
// frame that holds it and no two frames hold the same slot.
//
// The lock ordering that matters is the one this module does NOT participate
// in. The pager calls swap_slot_alloc under the address-space barrier and
// swap_slot_free under a mem write lock plus the per-frame lock, so this lock
// sits below all of them. It must therefore never call back into emu/memory.c
// or take a task lock while held. swap_disable is the exception and is written
// to drop it before it walks anything.
static lock_t swap_lock = LOCK_INITIALIZER;

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
static _Atomic uint64_t swap_stat_io_errors;
static _Atomic uint64_t swap_stat_bytes_written;

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

static void swap_file_close_locked(void) {
    if (swap_fd >= 0) {
        // Truncate before closing: the file is already unlinked, so this is
        // what actually hands the blocks back rather than waiting for the
        // process to exit. A clean exit should leave nothing behind.
        if (ftruncate(swap_fd, 0) != 0)
            /* nothing useful to do; the unlink still frees it at exit */;
        close(swap_fd);
        swap_fd = -1;
    }
    free(swap_free_bitmap);
    swap_free_bitmap = NULL;
    swap_slot_count = 0;
    swap_slots_used = 0;
    swap_alloc_rover = 0;
}

// ---------------------------------------------------------------------------
// Enable and disable
// ---------------------------------------------------------------------------

int swap_enable(uint64_t bytes) {
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
        // happen with the lock dropped because it walks address spaces.
        swap_disable();
        lock(&swap_lock, 0);
        if (swap_fd >= 0) {
            unlock(&swap_lock);
            return _EBUSY;          // still draining after the disable
        }
    }

    uint64_t total = (uint64_t) slots * frame;
    uint32_t words = (slots + SWAP_BITS_PER_WORD - 1) / SWAP_BITS_PER_WORD;
    uint64_t *bitmap = calloc(words, sizeof(*bitmap));
    if (bitmap == NULL) {
        unlock(&swap_lock);
        return _ENOMEM;
    }

    int fd = host_unlinked_tmpfd();
    if (fd < 0) {
        free(bitmap);
        unlock(&swap_lock);
        return fd;
    }
    int err = swap_reserve(fd, total);
    if (err == 0 && ftruncate(fd, (off_t) total) != 0)
        err = errno == ENOSPC ? _ENOSPC : _EIO;
    if (err != 0) {
        close(fd);
        free(bitmap);
        unlock(&swap_lock);
        return err;                 // never a smaller area than was asked for
    }

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

void swap_disable(void) {
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
    // The trylock-and-skip is the pattern collect_mem_page_stats (fs/proc/root.c)
    // documents at length: do_exit spins in exit_wait_backoff() while holding
    // general_lock, and the snapshot reference we hold keeps that loop alive, so
    // a blocking lock here deadlocks. A task we cannot lock is mid-exit and its
    // address space is about to be torn down, which frees its slots anyway.
    struct task_snapshot snapshot = {0};
    if (task_snapshot_collect(&snapshot, true) == 0) {
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
            swap_fault_mem_all(&mm->mem);
            mm_release(mm);
        }
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
    swap_file_close_locked();
    unlock(&swap_lock);
    printk("swap: disabled\n");
}

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------

int swap_slot_alloc(uint32_t *slot_out) {
    lock(&swap_lock, 0);
    if (swap_fd < 0 || swap_free_bitmap == NULL) {
        unlock(&swap_lock);
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
    if (drained) {
        // The last frame a stopped pager still had out has come home. Now the
        // file can go, which is what makes swap_disable's "keeping the file
        // open" state temporary rather than permanent.
        swap_file_close_locked();
    }
    unlock(&swap_lock);
    if (drained)
        printk("swap: the last outstanding slot came back; file released\n");
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
// Preferences
// ---------------------------------------------------------------------------

void swap_set_preference(bool enabled, unsigned size_mb) {
    atomic_store_explicit(&swap_pref_enabled, enabled, memory_order_relaxed);
    atomic_store_explicit(&swap_pref_size_mb, size_mb, memory_order_relaxed);
    atomic_store_explicit(&swap_pref_seen, true, memory_order_release);
    if (!enabled) {
        swap_disable();
        return;
    }
    if (size_mb == 0) {
        // Refused rather than guessed. The UI is required to ask for a size;
        // picking one here would be AOK choosing how much of the user's flash
        // to write to.
        printk("swap: enable requested with no size; ignoring\n");
        return;
    }
    int err = swap_enable((uint64_t) size_mb * 1024 * 1024);
    if (err < 0)
        printk("swap: enable failed (%d)\n", err);
}

void swap_startup(void) {
    if (atomic_load_explicit(&swap_pref_seen, memory_order_acquire)) {
        if (atomic_load_explicit(&swap_pref_enabled, memory_order_relaxed)) {
            unsigned mb = atomic_load_explicit(&swap_pref_size_mb, memory_order_relaxed);
            if (mb != 0)
                swap_enable((uint64_t) mb * 1024 * 1024);
        }
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
    out->draining = swap_fd >= 0 &&
        !atomic_load_explicit(&swap_on, memory_order_relaxed);
    unlock(&swap_lock);
    out->enabled = swap_enabled();
    out->alloc_failures = atomic_load_explicit(&swap_stat_alloc_fail, memory_order_relaxed);
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
        "alloc_failures   %llu  (evictions refused, the area is full)\n"
        "io_errors        %llu\n",
        s.enabled ? "yes" : "no",
        s.enabled ? "on" : (s.draining ? "off, draining" : "off"),
        (unsigned long long) s.slot_size,
        (unsigned long long) s.slots_total,
        (unsigned long long) s.slots_free,
        (unsigned long long) s.total_bytes,
        (unsigned long long) s.free_bytes,
        (unsigned long long) s.bytes_written,
        (unsigned long long) s.alloc_failures,
        (unsigned long long) s.io_errors);
    if (n < 0)
        return 0;
    return (size_t) n >= size ? size - 1 : (size_t) n;
}
