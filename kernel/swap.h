#ifndef KERNEL_SWAP_H
#define KERNEL_SWAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
    uint64_t io_errors;
    uint64_t bytes_written;    // since enable, for the write-budget work
    bool enabled;
    bool draining;             // disabled, but slots are still out
};
void swap_get_stats(struct swap_stats *out);

// Human-readable status for /proc/ish/swap. Always NUL-terminates and never
// writes more than `size` bytes; returns the length left in the buffer.
size_t swap_status_text(char *buf, size_t size);

#endif
