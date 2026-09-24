#ifndef EMU_CPU_MEM_H
#define EMU_CPU_MEM_H

#include "misc.h"
#include <stdatomic.h>

// Guest page numbers are internal MM bookkeeping. Keep them wide enough for
// future 64-bit address spaces even while guest-visible addr_t stays 32-bit.
typedef qword_t page_t;
typedef qword_t pages_t;
#define BAD_PAGE ((page_t) -1)

#ifndef __KERNEL__
#define PAGE_BITS 12
#undef PAGE_SIZE // defined in system headers somewhere
#define PAGE_SIZE (1 << PAGE_BITS)
#define PAGE(addr) ((addr) >> PAGE_BITS)
#define PGOFFSET(addr) ((addr) & (PAGE_SIZE - 1))
// bytes MUST be unsigned if you would like this to overflow to zero
#define PAGE_ROUND_UP(bytes) (PAGE((bytes) + PAGE_SIZE - 1))
#endif

struct mmu {
    struct mmu_ops *ops;
    struct jit *jit;
    // Bumped on every page-table change; each per-thread TLB compares its cached
    // copy and flushes on mismatch. Atomic (relaxed) because the lock-free
    // growth-mmap fast path bumps it without the write lock while readers
    // concurrently load it in the TLB hot path. Relaxed is sufficient: ordering
    // of the actual entry writes is provided by the rwlock (evicting writers) or
    // by app-level synchronization when a freshly-mmap'd pointer is shared.
    _Atomic uint64_t changes;
    bool requires_write_revalidate;
};

#define MEM_READ 0
#define MEM_WRITE 1
// Whether `page` may be executed: mapped (or reserved) with PROT_EXEC. A page
// neither mapped nor reserved answers true -- a fetch from it is an unmapped
// access, reported as one by the read that follows. Defined in emu/memory.c.
struct mmu;
bool mmu_page_executable(struct mmu *mmu, page_t page);
#define MEM_WRITE_PTRACE 2

struct mmu_ops {
    // type is MEM_READ or MEM_WRITE
    void *(*translate)(struct mmu *mmu, guest_addr_t addr, int type);
};

static inline void *mmu_translate(struct mmu *mmu, guest_addr_t addr, int type) {
    return mmu->ops->translate(mmu, addr, type);
}

#endif
