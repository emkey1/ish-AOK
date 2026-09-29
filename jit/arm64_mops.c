// FEAT_MOPS (ARMv8.8): the CPY/CPYF memcpy/memmove and SET memset
// instructions, for the arm64 JIT's mops gadget (jit/guest-arm64/control.S).
//
// Each operation is a prologue/main/epilogue triple, e.g.
//     cpyfp [x0]!, [x1]!, x2!
//     cpyfm [x0]!, [x1]!, x2!
//     cpyfe [x0]!, [x1]!, x2!
// and the architecture lets the prologue do any amount of the work, so here
// it does all of it, a host page span at a time, and the main and epilogue
// find Xn = 0 and do nothing. glibc 2.41's __memcpy_mops/__memmove_mops/
// __memset_mops are exactly such triples; one guest instruction then replaces
// a loop of dozens per 64 bytes, each of which costs a dispatch.
//
// What software sees is only the end state, the same under both of the
// architecture's register options: Xd and (for a copy) Xs advanced by the
// original Xn, Xn = 0. NZCV is set as option A leaves it after a forward
// operation (0000); nothing reads it but a MOPS exception handler, and this
// implementation never raises one.
//
// A fault part way leaves the registers describing what is left and reports
// the instruction that was running, so a handler that fixes the page and
// returns restarts it and it finishes the rest -- as the hardware's own
// progress-in-registers design intends. A forward copy advances Xd/Xs and
// shrinks Xn; a backward one (CPY with dst above an overlapping src) copies
// from the end, so it shrinks Xn only, and what is left is the prefix, whose
// source the tail copy did not touch.
//
// Memory is reached through the TLB one page span at a time, like the HLE
// copies (jit/hle.c). A host fault on a file page past EOF is taken by the
// JIT's host fault handlers as for any guest access (jit_translate_host_fault).

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "emu/cpu.h"
#include "emu/tlb.h"
#include "emu/interrupt.h"
#include "jit/arm64_mops.h"

static inline uint64_t mops_to_page_end(guest_addr_t addr) {
    return PAGE_SIZE - (addr & (PAGE_SIZE - 1));
}

static int mops_fault(struct cpu_state *cpu, struct tlb *tlb, bool was_write) {
    cpu->segfault_addr = tlb->segfault_addr;
    cpu->segfault_was_write = was_write;
    return INT_PF;
}

// Forward copy of regs[n] bytes from regs[s] to regs[d], advancing all three
// per span.
static int mops_copy_forward(struct cpu_state *cpu, struct tlb *tlb,
        unsigned d, unsigned s, unsigned n) {
    uint64_t *r = cpu->arm64_regs;
    while (r[n] != 0) {
        guest_addr_t dst = r[d], src = r[s];
        uint64_t span = r[n];
        uint64_t sp = mops_to_page_end(src), dp = mops_to_page_end(dst);
        if (span > sp) span = sp;
        if (span > dp) span = dp;
        void *sh = __tlb_read_ptr(tlb, src);
        if (sh == NULL)
            return mops_fault(cpu, tlb, false);
        void *dh = __tlb_write_ptr(tlb, dst);
        if (dh == NULL)
            return mops_fault(cpu, tlb, true);
        // memmove: the spans may overlap (a forward copy is correct for any
        // dst below src, overlapping or not).
        memmove(dh, sh, span);
        r[d] = dst + span;
        r[s] = src + span;
        r[n] -= span;
    }
    return 0;
}

// Backward copy, for CPY with dst above an overlapping src: from the end,
// shrinking Xn only; Xd and Xs advance once everything is done.
static int mops_copy_backward(struct cpu_state *cpu, struct tlb *tlb,
        unsigned d, unsigned s, unsigned n) {
    uint64_t *r = cpu->arm64_regs;
    uint64_t total = r[n];
    while (r[n] != 0) {
        guest_addr_t se = r[s] + r[n], de = r[d] + r[n];
        uint64_t so = se & (PAGE_SIZE - 1), doff = de & (PAGE_SIZE - 1);
        if (so == 0) so = PAGE_SIZE;
        if (doff == 0) doff = PAGE_SIZE;
        uint64_t span = r[n];
        if (span > so) span = so;
        if (span > doff) span = doff;
        void *sh = __tlb_read_ptr(tlb, se - span);
        if (sh == NULL)
            return mops_fault(cpu, tlb, false);
        void *dh = __tlb_write_ptr(tlb, de - span);
        if (dh == NULL)
            return mops_fault(cpu, tlb, true);
        memmove(dh, sh, span);
        r[n] -= span;
    }
    r[d] += total;
    r[s] += total;
    return 0;
}

static int mops_set(struct cpu_state *cpu, struct tlb *tlb,
        unsigned d, unsigned n, uint8_t c) {
    uint64_t *r = cpu->arm64_regs;
    while (r[n] != 0) {
        guest_addr_t dst = r[d];
        uint64_t span = r[n], dp = mops_to_page_end(dst);
        if (span > dp) span = dp;
        void *dh = __tlb_write_ptr(tlb, dst);
        if (dh == NULL)
            return mops_fault(cpu, tlb, true);
        memset(dh, c, span);
        r[d] = dst + span;
        r[n] -= span;
    }
    return 0;
}

int arm64_mops(struct cpu_state *cpu, struct tlb *tlb, uint32_t insn) {
    unsigned d = insn & 0x1f, n = (insn >> 5) & 0x1f, s = (insn >> 16) & 0x1f;
    unsigned op1 = (insn >> 22) & 3;
    bool is_set = op1 == 3;
    // Xn is the byte count; the architecture takes it as signed and caps it,
    // so a "negative" count means nothing to do. (Option B's negative
    // intermediate counts never exist here: the prologue finishes.)
    if ((int64_t) cpu->arm64_regs[n] <= 0) {
        cpu->arm64_regs[n] = 0;
    } else if (is_set) {
        uint8_t c = s == 31 ? 0 : (uint8_t) cpu->arm64_regs[s];
        int err = mops_set(cpu, tlb, d, n, c);
        if (err)
            return err;
    } else {
        bool forward_only = !((insn >> 26) & 1); // CPYF*
        guest_addr_t dst = cpu->arm64_regs[d], src = cpu->arm64_regs[s];
        int err;
        if (forward_only || dst <= src || dst - src >= cpu->arm64_regs[n])
            err = mops_copy_forward(cpu, tlb, d, s, n);
        else
            err = mops_copy_backward(cpu, tlb, d, s, n);
        if (err)
            return err;
    }
    cpu->arm64_nzcv = 0;
    return 0;
}

// ISH_MOPS=0 (or `echo 0 > /proc/ish/arm64_mops`) hides MOPS from AT_HWCAP2,
// ID_AA64ISAR2 and /proc/cpuinfo for programs started afterwards, so glibc
// goes back to its NEON memcpy -- for A/B measurement, or if a program ever
// trips over it. The instructions still execute either way, so a process
// that already chose them keeps working.
static atomic_int mops_advertised = -1;

bool arm64_mops_advertised(void) {
#ifdef ISH_JIT_ARM64_GUEST
    int advertised = atomic_load_explicit(&mops_advertised, memory_order_relaxed);
    if (advertised < 0) {
        const char *env = getenv("ISH_MOPS");
        int from_env = !(env != NULL && env[0] == '0');
        atomic_compare_exchange_strong(&mops_advertised, &advertised, from_env);
        advertised = atomic_load_explicit(&mops_advertised, memory_order_relaxed);
    }
    return advertised;
#else
    return false; // no arm64-guest JIT on this host, so nothing decodes them
#endif
}

void arm64_mops_set_advertised(bool advertised) {
    atomic_store_explicit(&mops_advertised, advertised, memory_order_relaxed);
}
