#ifndef TLB_H
#define TLB_H

#include <string.h>
#include "emu/mmu.h"
#include "debug.h"

// 24 bytes, deliberately NOT padded to 32. Padding it looks like an obvious
// win and is not, so before trying it, know that it has been measured:
//
// Because the size is not a power of two, every gadget's TLB probe has to
// MULTIPLY the hash index to reach its entry (`mov w10, TLB_ENTRY_SIZE;
// madd x9,x9,x10,_tlb` on aarch64, `imull $TLB_ENTRY_SIZE` on x86_64), and
// that multiply sits in the dependency chain addr -> index -> entry ->
// compare, in front of every guest load and store. Padding to 32 turns it
// into one `add x9,_tlb,x9,lsl 5`. It also fixes a real cache detail: at 24
// bytes the entry starts cycle through 0,24,48,8,32,56,16,40 within a
// 64-byte line, so the ones at 48 and 56 SPAN two lines, and 25% of probes
// touch two lines to read `page` (+0) and `data_minus_addr` (+16).
//
// Both of those are true and neither is worth anything. Implemented in full
// (aligned(32) on the entry so the array base is aligned too, TLB_ENTRY_SHIFT
// emitted from jit/offsets.c, all six gadget sites converted, all four guests
// green on the memory/TLB tests) and measured on two microarchitectures and
// two workload shapes:
//
//   md5sum of 24MB, interleaved A/B, median of 8   (Apple Silicon host)
//     riscv64 guest  1699 -> 1688 ms   (+0.6%)
//     i386 guest      550 ->  553 ms   (-0.5%)   [converged reps 548-550 vs
//                                                 547-553: identical]
//     arm64 guest     427 ->  431 ms   (-0.9%)
//   dash sh-loop 20k, min of 5-8       (A9 / ARMv8.0, no LSE, iPad 5)
//     arm64 guest    3042 -> 3035 ms   (flat)
//     riscv64 guest  3588 -> 3596 ms   (flat)
//
// Flat everywhere, arguably a hair worse, for +8KB per address space. The
// reason the multiply is free: A9/Twister is a 3-wide OUT-OF-ORDER core, not
// in-order, so it overlaps the multiply with the surrounding gadget work; and
// a straddled entry costs nothing extra while the 24KB array stays cache
// resident. Reverted. Do not re-land this without a workload that actually
// shows a win -- and note that a shell loop and a hash of 24MB, on both an
// Apple Silicon and an ARMv8.0 core, already did not.
struct tlb_entry {
    page_t page;
    page_t page_if_writable;
    uintptr_t data_minus_addr;
};
#define TLB_BITS 10
#define TLB_SIZE (1 << TLB_BITS)
struct tlb {
    struct mmu *mmu;
    page_t dirty_page;
    uint64_t mem_changes;
    // this is basically one of the return values of tlb_handle_miss, tlb_{read,write}, and __tlb_{read,write}_cross_page
    // yes, this sucks
    guest_addr_t segfault_addr;
    // ISH_ARM64_WATCH_LO16 debug aid: guest pc of the store currently being
    // resolved, stashed by arm64_resolve_write_ptr (jit/guest-arm64/memory.S)
    // so tlb_write_ptr_slow can attribute watched stores to an instruction.
    guest_addr_t watch_ip;
    // ISH_ARM64_WATCH_VAL: the previous store resolved through this (per-
    // thread) tlb, read back on the next store to detect a poison value
    // having been written. prev_write_changes guards against the host page
    // having been freed by a mapping change in between.
    void *prev_write_ptr;
    guest_addr_t prev_write_addr;
    guest_addr_t prev_write_ip;
    uint64_t prev_write_changes;
    struct tlb_entry entries[TLB_SIZE];
};

#define TLB_INDEX(addr) ((((addr >> PAGE_BITS) ^ (addr >> (PAGE_BITS + TLB_BITS)))) & (TLB_SIZE - 1))
#define TLB_PAGE(addr) ((page_t) PAGE(addr) << PAGE_BITS)
#define TLB_PAGE_EMPTY 1
void tlb_refresh(struct tlb *tlb, struct mmu *mmu);
void tlb_free(struct tlb *tlb);
void tlb_flush(struct tlb *tlb);
void *tlb_handle_miss(struct tlb *tlb, guest_addr_t addr, int type);
void *tlb_write_ptr_slow(struct tlb *tlb, guest_addr_t addr);

// ISH_ARM64_WATCH_LO16=<hex> store-watchpoint debug aid: records every
// JIT store whose target's low 16 address bits fall in a small window
// around the given value (heap layout is stable in the low bits across
// runs even when the allocation base moves). Requires the write-
// revalidate funnel; arm64_watch_enabled() turns it on in mem_init.
bool arm64_watch_enabled(void);
void arm64_watch_dump(void);
uint64_t arm64_trace_ip_target(void);

// arm64-guest host-atomic CAS pair helper (emu/tlb.c), shared by the JIT
// casp gadgets (jit/guest-arm64/atomics.S) and the interpreter
// (emu/arm64_interp.c). sz = bytes per register half (4 or 8);
// expected/desired/old_out are {lo, hi}. Returns 0 / INT_PF / INT_GPF
// (alignment; see arm64_atomic_alignment_fault).
struct cpu_state;
int arm64_casp(struct cpu_state *cpu, struct tlb *tlb, guest_addr_t addr,
               unsigned sz, const uint64_t expected[2], const uint64_t desired[2],
               uint64_t old_out[2], uint32_t *swapped);

// arm64-guest exclusive-pair helpers (emu/tlb.c), shared by the JIT
// ldxp/stxp gadgets (jit/guest-arm64/atomics.S) and the interpreter
// (emu/arm64_interp.c). sz = bytes per register half (4 or 8). arm64_ldxp
// loads {lo, hi} into val_out and arms the exclusive monitor; arm64_stxp
// resolves the monitor into a host-atomic pair CAS (via arm64_casp) and
// writes the LL/SC status (0 = stored, 1 = failed) to *status_out.
// Both return 0 / INT_PF / INT_GPF (alignment = total pair size).
int arm64_ldxp(struct cpu_state *cpu, struct tlb *tlb, guest_addr_t addr,
               unsigned sz, uint64_t val_out[2]);
int arm64_stxp(struct cpu_state *cpu, struct tlb *tlb, guest_addr_t addr,
               unsigned sz, uint64_t desired_lo, uint64_t desired_hi,
               uint32_t *status_out);

forceinline __no_instrument void *__tlb_read_ptr(struct tlb *tlb, guest_addr_t addr) {
    if (unlikely(tlb->mem_changes != atomic_load_explicit(&tlb->mmu->changes, memory_order_relaxed)))
        tlb_flush(tlb);
    struct tlb_entry entry = tlb->entries[TLB_INDEX(addr)];
    if (entry.page == TLB_PAGE(addr)) {
        void *address = (void *) (entry.data_minus_addr + addr);
        posit(address != NULL);
        return address;
    }
    return tlb_handle_miss(tlb, addr, MEM_READ);
}
bool __tlb_read_cross_page(struct tlb *tlb, guest_addr_t addr, char *out, unsigned size);
forceinline __no_instrument bool tlb_read(struct tlb *tlb, guest_addr_t addr, void *out, unsigned size) {
    if (PGOFFSET(addr) > PAGE_SIZE - size)
        return __tlb_read_cross_page(tlb, addr, out, size);
    void *ptr = __tlb_read_ptr(tlb, addr);
    if (ptr == NULL)
        return false;
    memcpy(out, ptr, size);
    return true;
}

forceinline __no_instrument void *__tlb_write_ptr(struct tlb *tlb, guest_addr_t addr) {
    if (unlikely(tlb->mem_changes != atomic_load_explicit(&tlb->mmu->changes, memory_order_relaxed)))
        tlb_flush(tlb);
    struct tlb_entry *entry = &tlb->entries[TLB_INDEX(addr)];
    if (entry->page_if_writable == TLB_PAGE(addr)) {
        if (unlikely(tlb->mmu->requires_write_revalidate)) {
            // Host page protections are process-global. If host mirroring is
            // active, a cached writable hit must be revalidated before use.
            void *page_ptr = mmu_translate(tlb->mmu, TLB_PAGE(addr), MEM_WRITE);
            if (page_ptr == NULL) {
                entry->page_if_writable = TLB_PAGE_EMPTY;
                return NULL;
            }
            entry->data_minus_addr = (uintptr_t) page_ptr - TLB_PAGE(addr);
        }
        tlb->dirty_page = TLB_PAGE(addr);
        void *address = (void *) (entry->data_minus_addr + addr);
        posit(address != NULL);
        return address;
    }
    return tlb_handle_miss(tlb, addr, MEM_WRITE);
}
bool __tlb_write_cross_page(struct tlb *tlb, guest_addr_t addr, const char *value, unsigned size);
forceinline __no_instrument bool tlb_write(struct tlb *tlb, guest_addr_t addr, const void *value, unsigned size) {
    if (PGOFFSET(addr) > PAGE_SIZE - size)
        return __tlb_write_cross_page(tlb, addr, value, size);
    void *ptr = __tlb_write_ptr(tlb, addr);
    if (ptr == NULL)
        return false;
    memcpy(ptr, value, size);
    return true;
}

#endif
