#include "emu/cpu.h"
#include "emu/tlb.h"
#include "kernel/signal.h"
#include "kernel/task.h"
#include "kernel/resource_locking.h"

void tlb_refresh(struct tlb *tlb, struct mmu *mmu) {
    //mofify_critical_region_counter(current, 1, __FILE__, __LINE__); // WORKING ON -mke
    if (tlb->mmu == mmu && tlb->mem_changes == mmu->changes) {
        //mofify_critical_region_counter(current, -1, __FILE__, __LINE__);
        return;
    }
    tlb->mmu = mmu;
    tlb->dirty_page = TLB_PAGE_EMPTY;
    tlb->mem_changes = mmu->changes;
    tlb_flush(tlb);
    //mofify_critical_region_counter(current, -1, __FILE__, __LINE__);
}

void tlb_flush(struct tlb *tlb) {
    tlb->mem_changes = tlb->mmu->changes;
    for (unsigned i = 0; i < TLB_SIZE; i++)
        tlb->entries[i] = (struct tlb_entry) {.page = 1, .page_if_writable = 1};
}

void tlb_free(struct tlb *tlb) {
    ////mofify_critical_region_counter(current, 1, __FILE__, __LINE__);
    free(tlb);
    ////mofify_critical_region_counter(current, -1, __FILE__, __LINE__);
}

bool __tlb_read_cross_page(struct tlb *tlb, addr_t addr, char *value, unsigned size) {
    ////mofify_critical_region_counter(current, 1, __FILE__, __LINE__);
    char *ptr1 = __tlb_read_ptr(tlb, addr);
    if (ptr1 == NULL) {
        ////mofify_critical_region_counter(current, -1, __FILE__, __LINE__);
        return false;
    }
    char *ptr2 = __tlb_read_ptr(tlb, (PAGE(addr) + 1) << PAGE_BITS);
    if (ptr2 == NULL) {
        ////mofify_critical_region_counter(current, -1, __FILE__, __LINE__);
        return false;
    }
    size_t part1 = PAGE_SIZE - PGOFFSET(addr);
    assert(part1 < size);
    memcpy(value, ptr1, part1);
    memcpy(value + part1, ptr2, size - part1);
    ////mofify_critical_region_counter(current, -1, __FILE__, __LINE__);
    return true;
}

bool __tlb_write_cross_page(struct tlb *tlb, addr_t addr, const char *value, unsigned size) {
    ////mofify_critical_region_counter(current, 1, __FILE__, __LINE__);
    char *ptr1 = __tlb_write_ptr(tlb, addr);
    if (ptr1 == NULL) {
        ////mofify_critical_region_counter(current, -1, __FILE__, __LINE__);
        return false;
    }
    char *ptr2 = __tlb_write_ptr(tlb, (PAGE(addr) + 1) << PAGE_BITS);
    if (ptr2 == NULL) {
        ////mofify_critical_region_counter(current, -1, __FILE__, __LINE__);
        return false;
    }
    size_t part1 = PAGE_SIZE - PGOFFSET(addr);
    assert(part1 < size);
    memcpy(ptr1, value, part1);
    memcpy(ptr2, value + part1, size - part1);
    ////mofify_critical_region_counter(current, -1, __FILE__, __LINE__);
    return true;
}

__no_instrument void *tlb_handle_miss(struct tlb *tlb, addr_t addr, int type) {
    char *ptr = mmu_translate(tlb->mmu, TLB_PAGE(addr), type);
    if (tlb->mmu->changes != tlb->mem_changes)
        tlb_flush(tlb);
    if (ptr == NULL) {
        tlb->segfault_addr = addr;
        return NULL;
    }
    tlb->dirty_page = TLB_PAGE(addr);

    struct tlb_entry *tlb_ent = &tlb->entries[TLB_INDEX(addr)];
    tlb_ent->page = TLB_PAGE(addr);
    if (type == MEM_WRITE)
        tlb_ent->page_if_writable = tlb_ent->page;
    else
        // 1 is not a valid page so this won't look like a hit
        tlb_ent->page_if_writable = TLB_PAGE_EMPTY;
    tlb_ent->data_minus_addr = (uintptr_t) ptr - TLB_PAGE(addr);
    return (void *) (tlb_ent->data_minus_addr + addr);
}
