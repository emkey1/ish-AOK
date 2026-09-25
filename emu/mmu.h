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
// The same answer as a P_EXEC bit, with the page's P_SHARED bit beside it:
// what tlb_handle_miss needs to decide whether a store is a code write.
unsigned mmu_page_code_flags(struct mmu *mmu, page_t page);
// A debugger's forced access -- PTRACE_PEEK/POKE*, /proc/<pid>/mem -- which
// the protection check lets through for that one access (Linux's FOLL_FORCE):
// see mem_write_way in emu/memory.c. Only ever passed to mem_ptr.
#define MEM_WRITE_PTRACE 2
#define MEM_READ_PTRACE 3

struct mmu_ops {
    // type is MEM_READ or MEM_WRITE
    void *(*translate)(struct mmu *mmu, guest_addr_t addr, int type);
};

static inline void *mmu_translate(struct mmu *mmu, guest_addr_t addr, int type) {
    return mmu->ops->translate(mmu, addr, type);
}

#endif
