#ifndef KERNEL_SWAP_H
#define KERNEL_SWAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Forward declaration at FILE scope. Without it the `struct mem *` in
// swap_direct_reclaim's prototype declares a type scoped to that prototype, and
// the definition in swap.c then has a conflicting one.
struct mem;

// ---------------------------------------------------------------------------
// The simulated-swap backing store: the file, the slot table, and the switch.
//
// docs/simulated_swap_plan.md sections 3.11 (storage) and 3.13 (settings).
// This module owns everything about WHERE an evicted frame's bytes go. It owns
// nothing about WHICH frame goes, or when -- the page-table side of the pager
// lives in emu/memory.c, which is where struct pt_entry and struct data are,
// and the two meet only across the handful of calls at the bottom of this file.
//
// That division is deliberate and was paid for. An earlier attempt put a second
// pager in this file with its own view of the page table, and the two owners of
// one piece of state produced measured silent corruption. There is exactly one
// implementation of the page-table transitions, in emu/memory.c, and exactly one
// implementation of the storage, here.
//
// SWAP SHIPS OFF. That is a product decision (section 3.13), not a staging
// step: paging guest memory writes to the device's flash and eats container
// space, so it is not a thing to turn on for somebody. Until a user enables it
// from Settings, nothing here allocates a byte or opens a file, and the guest
// sees byte-for-byte today's behaviour.
// ---------------------------------------------------------------------------

// ---- the switch -----------------------------------------------------------

// Is the pager on? False is the default and the shipping state. One relaxed
// atomic load, so a caller on a hot path may test it inline.
//
// This gates EVICTION only. The fault path must never consult it: a frame that
// went out before swap was turned off still has to be answerable until it is
// back, and refusing to answer would be a SIGBUS on good guest memory. See
// swap_disable().
bool swap_enabled(void);

// Turn the pager on with `bytes` of backing store, rounded DOWN to whole host
// frames. Returns 0, or a negative errno; on any failure nothing is left
// allocated and swap stays off.
//
// The size is the USER'S choice and is never derived silently from device RAM
// or free disk. ENOSPC while reserving means swap off, never a smaller area
// than was asked for -- a swap area that quietly shrank would make SwapTotal a
// number the user did not choose.
//
// Idempotent for the same size. A different size is a disable followed by an
// enable, so it pages everything back in first.
int swap_enable(uint64_t bytes);

// Turn the pager off: stop new eviction, fault every evicted page back into its
// address space, then release the slot table and truncate and close the file.
// Blocks until the pages are back.
//
// The page-in walk covers every LIVE task, so it can in principle miss an
// address space whose task was already exiting when the snapshot was taken. If
// anything is still out when it finishes, eviction stays off but THE FILE IS
// KEPT OPEN so those faults can still be answered -- a page whose bytes are in
// a file nobody can open any more is a SIGSEGV on good guest memory, for ever.
// swap_enable() then refuses until the last slot comes back.
void swap_disable(void);

// Called once at guest boot, after preferences are readable. Consults, in
// order: swap_set_preference() if the app has already called it, then
// ISH_GUEST_SWAP_MB for the CLI and Xcode runs. Turns nothing on unless one of
// them says a size, which is the default.
void swap_startup(void);

// The Settings toggle and size field, as the app's KVO observers see them.
//
// RECORDS ONLY, for the next swap_startup(). It does not allocate, reserve or
// open anything, and it does not touch a running pager in either direction --
// swap is sized once at launch and is deliberately not resizable in place
// (section 3.13). The observer that calls this fires on every launch, including
// ones that never boot a guest, and when it used to act instead of record, a
// simulator sitting on the rootfs picker with nothing installed had half a
// gigabyte of swap file open in its container.
void swap_set_preference(bool enabled, unsigned size_mb);

// May a guest process change the switch, through /proc/ish/swap?
//
// FALSE on any App Store launch, and that is the point: swap writes to the
// user's flash and eats container space, so turning it on is the user's
// decision in Settings and not something a guest process -- even guest root --
// can do on their behalf. It becomes true only when the launch itself offered
// guest control by setting ISH_GUEST_SWAP_MB, which is the CLI and Xcode-scheme
// path and is not reachable from an installed app.
//
// It exists because the alternative was a swapoff nothing could test. Every
// other way of exercising swap_disable() from the CLI meant either shipping a
// device-reachable control or testing a different code path than the one that
// ships.
bool swap_guest_control_allowed(void);

// ---- slots ----------------------------------------------------------------
//
// A slot is one host frame's worth of the file. Slot numbers are what
// struct data::frame_slot holds; SWAP_SLOT_NONE (0) is never allocated, so a
// calloc'd frame_slot array already reads as "these bytes are in host memory".

// Take a free slot. Returns 0 and stores the slot, or a negative errno --
// _ENOSPC when the area the user asked for is full, which is an ordinary
// condition and simply means this frame is not evicted.
int swap_slot_alloc(uint32_t *slot_out);

// Give one back. Safe with SWAP_SLOT_NONE, which does nothing, so a caller
// tearing down a mapping can hand over every frame's slot without testing.
void swap_slot_free(uint32_t slot);

// The I/O. Both take the whole frame; `len` is mem_frame_size() and is checked
// against the slot size the area was built with. Return 0 or a negative errno.
// Neither takes any lock the pager holds.
int swap_slot_write(uint32_t slot, const void *buf, size_t len);
int swap_slot_read(uint32_t slot, void *buf, size_t len);

// ---- direct reclaim -------------------------------------------------------

// Try to free `want_bytes` of host memory by evicting `mem`'s coldest frames.
// Returns bytes released; 0 whenever swap is off, which is the default.
//
// MUST be called with NO mem lock held: it takes the address-space barrier
// itself, and that barrier takes pt_alloc_lock, which is not recursive. At brk
// that means hoisting the call ABOVE mem_write_lock_with_pokes rather than
// putting it beside the guard it serves.
//
// Bounded by BYTES, not by time. The plan's original "about 20 ms of the
// calling mem's oldest candidates" works out to roughly 2 MiB against a guard
// that fires when the app is 192 MiB from its ceiling, so it could never
// restore the headroom it was called to restore. It is also not the whole
// deficit: at 100-200 MiB/s per process, reclaiming 192 MiB inside one mmap()
// would stall the caller for one to two seconds. So it reclaims a bounded slice
// and lets the allocation retry, which is the shape Linux's direct reclaim has
// for the same reason.
long swap_direct_reclaim(struct mem *mem, uint64_t want_bytes);

// ---- /dev/aokswap0, the area as a block device ---------------------------
// Size in bytes, 0 when there is no area. Same figure as swap_stats.total_bytes
// and therefore as /proc/meminfo's SwapTotal, on purpose.
uint64_t swap_area_bytes(void);
// Raw access to the area. A read past the end (or with no area at all) returns
// 0 = EOF, which is what an unbound Linux block device does. Negative returns
// are kernel/errno.h codes.
ssize_t swap_area_pread(void *buf, size_t len, off_t off);
ssize_t swap_area_pwrite(const void *buf, size_t len, off_t off);

// ---- background reclaim ---------------------------------------------------
//
// kswapd sheds cold memory BEFORE an allocation is about to fail, which is the
// whole point of having it: direct reclaim only runs when the guest is already
// at the guard, and on a device the thing being avoided is jetsam, which does
// not wait politely for an mmap. Started by swap_enable, stopped by
// swap_disable, and non-existent while swap is off.
//
// It runs on a host thread with NO `current`, and two things follow that are
// not incidental:
//
//  - the address-space barrier has to work for a task-less caller.
//    task_poke_shared_mem treats a NULL task as "no self to skip" rather than
//    "do nothing"; before that it poked nobody and the barrier fell through to
//    a blocking write_lock behind guest threads that were still executing.
//  - it must never run mm_release, because that can reach mem_destroy, and the
//    bounded-teardown guard keys on current->mm_teardown -- without it a FUSE
//    flush in an unmapped file's ->close waits for a guest process that will
//    never run again. So kswapd never retains an mm at all: it holds a TASK
//    reference from task_snapshot_collect, and do_exit blocks on exactly those
//    references before it releases the mm, so task->mm stays valid for as long
//    as the reference is held.

// Passes since the last one that did any work, for /proc/ish/swap.
struct swap_kswapd_stats {
    uint64_t passes, reclaim_passes, bytes_reclaimed;
    uint64_t thrash_backoffs;
    bool running, thrashing;
};

// ---- suspension -----------------------------------------------------------

// Called beside fakefs_quiesce_begin in the app's suspension handler: stop
// starting new eviction I/O and wait up to `timeout_ms` for anything already in
// flight to finish. Returns true if everything drained in time.
//
// Raw pthread primitives, because of where it is called from: an expiring
// background-task assertion on an arbitrary queue, with no `current` task, and
// it must take no mem lock -- a guest thread holding one may be exactly what it
// is waiting for.
//
// It stops EVICTION only. Faults are left alone: nothing is running guest code
// while suspended, so no fault can start, and one already in flight has to be
// allowed to finish or the frame it is restoring stays PROT_NONE with its bytes
// only on disk.
bool swap_quiesce_begin(int timeout_ms);
void swap_quiesce_end(void);

// ---- counters -------------------------------------------------------------
//
// Every guest-visible swap number will come from here, and from nothing the
// host says about itself. A kernel never reclaims anonymous pages without swap,
// so SwapTotal > 0 if and only if AOK itself moves pages to storage.
struct swap_stats {
    uint64_t total_bytes;      // what the user asked for, rounded to frames
    uint64_t free_bytes;       // FREE slots
    uint64_t slot_size;        // one host frame
    uint64_t slots_total, slots_free;
    uint64_t alloc_failures;   // evictions refused because the area was full
    uint64_t no_area;          // evictions refused because there was no area
    uint64_t direct_reclaim_bytes;  // freed for allocations that would have failed
    // GUEST 4 KiB pages moved, which is what /proc/vmstat's pswpin and pswpout
    // count -- four per host frame on Apple silicon. Monotonic since boot, like
    // every /proc/vmstat counter, so turning swap off and on again does not
    // reset them; bytes_written above is the per-area figure the write budget
    // will use and is reset with the area.
    uint64_t pswpin_pages, pswpout_pages;
    // Frames that are resident AND still have a valid copy on disk, so a second
    // eviction would need no write. Stays 0 until the pager has section 3.6's
    // clean state, and 0 is then a measurement rather than a placeholder: with
    // no clean state there really are no cached frames.
    uint64_t cached_bytes;
    uint64_t io_errors;
    uint64_t bytes_written;    // since enable
    // The rolling 24-hour write window. Paging writes to the user's flash, and
    // an app that writes gigabytes a day to it is one Apple's disk-writes
    // instrumentation notices and users feel. The threshold is folklore -- 1 GB
    // a day is the quoted figure -- so this is measured and enforced rather
    // than trusted.
    uint64_t write_budget_bytes, written_window_bytes;
    uint64_t budget_refusals;  // evictions refused because the window is spent
    bool enabled;
    bool draining;             // disabled, but slots are still out
    bool quiesced;             // suspension gate held: no new eviction I/O
    bool kswapd_running;
    bool thrashing;            // background reclaim paused: pages come straight back
    bool release_ineffective;  // reclaim paused: released pages stay in the footprint
    uint64_t kswapd_passes, kswapd_reclaimed_bytes, thrash_backoffs;
    uint64_t ledger_backoffs, ledger_refused;
    unsigned release_verdict;  // 0 not measured, 1 ledger moved, 2 it did not
};
void swap_get_stats(struct swap_stats *out);

// Human-readable status for /proc/ish/swap. Always NUL-terminates and never
// writes more than `size` bytes; returns the length left in the buffer.
size_t swap_status_text(char *buf, size_t size);

#endif
