#include "jit/jit.h"
#include "jit/frame.h"
#include "emu/cpu.h"
#include "emu/tlb.h"
#include "emu/mmu.h"

#define TLB_ENTRY_SIZE sizeof(struct tlb_entry)

void cpu() {
    OFFSET(CPU, cpu_state, eax);
    OFFSET(CPU, cpu_state, ebx);
    OFFSET(CPU, cpu_state, ecx);
    OFFSET(CPU, cpu_state, edx);
    OFFSET(CPU, cpu_state, esi);
    OFFSET(CPU, cpu_state, edi);
    OFFSET(CPU, cpu_state, ebp);
    OFFSET(CPU, cpu_state, esp);
    OFFSET(CPU, cpu_state, ax);
    OFFSET(CPU, cpu_state, bx);
    OFFSET(CPU, cpu_state, cx);
    OFFSET(CPU, cpu_state, dx);
    OFFSET(CPU, cpu_state, si);
    OFFSET(CPU, cpu_state, di);
    OFFSET(CPU, cpu_state, bp);
    OFFSET(CPU, cpu_state, sp);
    OFFSET(CPU, cpu_state, eip);
    OFFSET(CPU, cpu_state, amd64_regs);
    OFFSET(CPU, cpu_state, amd64_rip);
    OFFSET(CPU, cpu_state, gs);
    OFFSET(CPU, cpu_state, tls_ptr);

    OFFSET(CPU, cpu_state, eflags);
    OFFSET(CPU, cpu_state, of);
    OFFSET(CPU, cpu_state, cf);
    OFFSET(CPU, cpu_state, res);
    OFFSET(CPU, cpu_state, op1);
    OFFSET(CPU, cpu_state, op2);
    OFFSET(CPU, cpu_state, flags_res);
    OFFSET(CPU, cpu_state, df_offset);
    OFFSET(CPU, cpu_state, fsw);
    OFFSET(CPU, cpu_state, xmm);
    MACRO(PF_RES);
    MACRO(ZF_RES);
    MACRO(SF_RES);
    MACRO(AF_OPS);
    MACRO(PF_FLAG);
    MACRO(AF_FLAG);
    MACRO(ZF_FLAG);
    MACRO(SF_FLAG);
    MACRO(DF_FLAG);

    OFFSET(LOCAL, jit_frame, bp);
    OFFSET(LOCAL, jit_frame, value);
    OFFSET(LOCAL, jit_frame, value_addr);
    OFFSET(LOCAL, jit_frame, last_block);
    OFFSET(LOCAL, jit_frame, chain_budget);
    OFFSET(LOCAL, jit_frame, ret_cache);
    OFFSET(CPU, cpu_state, segfault_addr);
    OFFSET(CPU, cpu_state, segfault_was_write);
    OFFSET(CPU, cpu_state, poked_ptr);
    MACRO(MEM_READ);
    MACRO(MEM_WRITE);

    OFFSET(JIT_BLOCK, jit_block, addr);
    OFFSET(JIT_BLOCK, jit_block, code);
    // Read (at code-relative negative offset) by gadget_riscv64_jalr_cached:
    // an invalidated-but-not-yet-freed block must not be dispatched out of the
    // return cache, which unlike a chained edge has nothing to unpatch it.
    OFFSET(JIT_BLOCK, jit_block, is_jetsam);

    OFFSET(TLB, tlb, entries);
    OFFSET(TLB, tlb, mmu);
    OFFSET(TLB, tlb, mem_changes);
    OFFSET(TLB, tlb, dirty_page);
    OFFSET(TLB, tlb, segfault_addr);
    OFFSET(TLB, tlb, watch_ip);
    OFFSET(MMU, mmu, changes);
    OFFSET(MMU, mmu, requires_write_revalidate);
    OFFSET(TLB_ENTRY, tlb_entry, page);
    OFFSET(TLB_ENTRY, tlb_entry, page_if_writable);
    OFFSET(TLB_ENTRY, tlb_entry, data_minus_addr);
    MACRO(TLB_ENTRY_SIZE);
    MACRO(TLB_BITS);
    MACRO(PAGE_BITS);
}

// AArch64 guest register file, for jit/guest-arm64/'s ported gadgets
// (aarch64_guest_plan.md's JIT-gadget-port direction change). Symbol names
// intentionally match this codebase's own OFFSET(CPU, cpu_state, ...)
// convention (CPU_arm64_regs etc), not OpenMinis' bare CPU_x0/CPU_sp/
// CPU_pc/CPU_nzcv naming (their struct is arm64-only so has no collision
// risk; this codebase's struct is shared across i386/amd64/arm64, and
// CPU_sp/CPU_pc would collide with i386's own `sp` field and nothing-named-
// pc respectively if used bare). jit/guest-arm64/gadgets.h defines
// CPU_x0/CPU_sp/CPU_pc/CPU_nzcv as aliases to these, once, in one place,
// rather than renaming every reference across the ~22K lines of ported
// gadget assembly.
void arm64() {
    OFFSET(CPU, cpu_state, arm64_regs);
    OFFSET(CPU, cpu_state, arm64_sp);
    OFFSET(CPU, cpu_state, arm64_pc);
    OFFSET(CPU, cpu_state, arm64_nzcv);
    OFFSET(CPU, cpu_state, arm64_excl_addr);
    OFFSET(CPU, cpu_state, arm64_excl_val);
    OFFSET(CPU, cpu_state, arm64_excl_val_hi);
    OFFSET(CPU, cpu_state, arm64_tpidr);
    OFFSET(CPU, cpu_state, arm64_v);
    OFFSET(CPU, cpu_state, arm64_fpsr);
    OFFSET(CPU, cpu_state, arm64_fpcr);
}

// Same prefixed-name rule as arm64() above; jit/guest-riscv64/gadgets.h
// will alias bare names (CPU_x10 etc.) onto these in one place.
void riscv64() {
    OFFSET(CPU, cpu_state, riscv64_regs);
    OFFSET(CPU, cpu_state, riscv64_zero_sink);
    OFFSET(CPU, cpu_state, riscv64_pc);
    OFFSET(CPU, cpu_state, riscv64_res_addr);
    OFFSET(CPU, cpu_state, riscv64_res_val);
    OFFSET(CPU, cpu_state, riscv64_f);
    OFFSET(CPU, cpu_state, riscv64_fcsr);
}
