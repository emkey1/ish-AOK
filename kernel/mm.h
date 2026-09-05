#ifndef KERNEL_MM_H
#define KERNEL_MM_H

#include "kernel/abi.h"
#include "emu/memory.h"
#include "misc.h"

// uses mem.lock instead of having a lock of its own
struct mm {
    atomic_uint refcount;
    // A number that identifies this address space and is never reused, so code
    // holding a guest address can ask "is this still the space that address
    // meant something in?" without keeping the mm alive to compare pointers
    // against. A released mm is freed, and the allocator hands its address to
    // the next one; answering yes about the wrong space means reading or
    // unmapping memory that belongs to somebody else. The native marshalling
    // arena asks exactly this (kernel/native_syscall.c).
    uint64_t id;
    struct mem mem;
    struct list shm_regions;

    guest_addr_t vdso; // immutable
    guest_addr_t start_brk; // immutable
    guest_addr_t brk;

    // crap for procfs
    guest_addr_t argv_start;
    guest_addr_t argv_end;
    guest_addr_t env_start;
    guest_addr_t env_end;
    guest_addr_t auxv_start;
    guest_addr_t auxv_end;
    guest_addr_t stack_start;
    dword_t mlockall_flags;
    // Peak mapped-page count this address space has been observed at, for
    // getrusage's ru_maxrss. Sampled rather than maintained incrementally: the
    // page count comes from a page-table walk, and there is no single choke
    // point through which every mapping change passes. So it is a high-water
    // mark over the samples actually taken -- which for the consumers that
    // matter (a wait4 supervisor reading a child's usage, `time -v`) includes
    // the sample taken at exit.
    size_t rss_pages_hwm;
    // Memory backpressure state. On the mm, not the task, because the thing
    // being judged is the ADDRESS SPACE's growth: a four-thread filler shares
    // one mm, and per-task strikes would need 4x the overshoot before any one
    // counter reached the limit. Approximate by design -- these are a
    // heuristic's working state, and a lost update costs one probe.
    _Atomic unsigned fault_oom_strikes;
    _Atomic size_t fault_last_resident_pages;
    struct fd *exefile;
};

// Create a new address space
struct mm *mm_new(enum guest_abi abi);
// Clone (COW) the address space
struct mm *mm_copy(struct mm *mm);
// Increment the refcount
void mm_retain(struct mm *mem);
// Decrement the refcount, destroy everything in the space if 0
void mm_release(struct mm *mem);

// Called from handle_timer_interrupt, which task_run_current reaches only AFTER
// releasing mem->lock -- the one point where a guest thread executing code holds
// no lock and may block. Throttles a guest committing memory the mmap/mremap/brk
// growth guards never saw, and OOM-kills it if the host is out and the pager can
// free nothing. Defined in kernel/mmap.c.
void mem_fault_backpressure(void);

// Guest write misses between headroom probes in tlb_handle_miss. 1024 misses is
// up to 4 MiB of freshly touched pages, so the Mach trap in
// host_mem_headroom_low() is amortised and cannot itself become the cost of
// touching memory -- while still noticing a 240 MiB/s writer within ~17 ms.
#define MEM_HEADROOM_PROBE_MISSES 1024

// SysV shm bookkeeping hooks owned by kernel/ipc.c.
void ipc_mm_init(struct mm *mm);
void ipc_mm_copy(struct mm *dst, struct mm *src);
void ipc_mm_release(struct mm *mm);

#endif
