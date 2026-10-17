// ppc64 guest core gadgets: the ways out of a block (an interrupt, `sc`, the
// branches that end one), constants, and the TLB helpers every memory gadget
// calls. Operands: gadgets.h.

#include "gadgets.h"

// Exit to the kernel with an interrupt code. [gadget][code][pc][addr]
.gadget interrupt
    ldr _tmp, [_ip]
    ldr x8, [_ip, #8]
    str x8, [_cpu, #CPU_ppc64_pc]
    ldr x8, [_ip, #16]
    str x8, [_cpu, #CPU_segfault_addr]
    strb wzr, [_cpu, #CPU_segfault_was_write]
    // An INT_GPF from here is a fetch from an unmapped page, and its address
    // is the fault's even when it is 0 (a call through NULL): reported, so
    // the kernel delivers SEGV_MAPERR at it rather than SI_KERNEL.
    cmp _tmp, #INT_GPF
    cset w8, eq
    strb w8, [_cpu, #CPU_segfault_reported]
    b jit_exit

// sc: [gadget][next_pc]. The pc is the instruction after, as Linux has it
// during a system call; a restart steps back over the 4 bytes.
.gadget sc
    ldr x8, [_ip]
    str x8, [_cpu, #CPU_ppc64_pc]
    mov w0, #INT_PPC64_SC
    b jit_exit

// ISH_JIT_PROFILE's count of this block's entries: [gadget][&counter]
.gadget block_count
    ldr x8, [_ip], #8
    ldr x9, [x8]
    add x9, x9, #1
    str x9, [x8]
    gret

// rd = a constant (li, lis, an addis of r0, a link value): [gadget][rd][value]
.gadget mov_const
    ldp x9, x10, [_ip]
    str x10, [_cpu, x9]
    gret 2

// A branch to a constant address, and the block's end when it is cut off at
// its size limit: [gadget][target], the target tagged with bit 63 until the
// frontend chains it to the block there (jit.c, as for riscv64).
.gadget b
    ldr _ip, [_ip]
    b ppc64_branch_dispatch

// bl: LR = the link, then as b. [gadget][link][target]
.gadget bl
    ldr x8, [_ip]
    str x8, [_cpu, #CPU_ppc64_lr]
    ldr _ip, [_ip, #8]
    b ppc64_branch_dispatch

.global ppc64_branch_dispatch
ppc64_branch_dispatch:
    tbz _ip, #63, ppc64_chain
.global ppc64_branch_unchained
ppc64_branch_unchained:
    and x8, _ip, #0x7fffffffffffffff
    str x8, [_cpu, #CPU_ppc64_pc]
    b jit_ret
// riscv64_chain's design: a chained edge enters the next block's code with its
// poke and budget checks.
.global ppc64_chain
ppc64_chain:
    ldr x8, [_cpu, #CPU_poked_ptr]
    ldrb w8, [x8]
    cbnz w8, ppc64_chain_poked
    ldr x8, [_cpu, #LOCAL_chain_budget]
    subs x8, x8, #1
    str x8, [_cpu, #LOCAL_chain_budget]
    b.le ppc64_chain_poked
    sub x8, _ip, #JIT_BLOCK_code
    str x8, [_cpu, #LOCAL_last_block]
#if !defined(ISH_ARM64_GRET_LDAR)
    dmb ishld
#endif
    gret
.global ppc64_chain_poked
ppc64_chain_poked:
    ldur x8, [_ip, #(JIT_BLOCK_addr - JIT_BLOCK_code)]
    str x8, [_cpu, #CPU_ppc64_pc]
    b jit_ret

// A branch to an address in a register (blr, bctr and their kin): the pc is
// that address, and the frontend finds the block. x8 holds it.
.global ppc64_jump_x8
ppc64_jump_x8:
    and x8, x8, #~3
    str x8, [_cpu, #CPU_ppc64_pc]
    b jit_ret

// ---- TLB helpers (jit/guest-riscv64/memory.S's, for ppc64's pc) ------------
//
// The fault-restart contract is riscv64's: every memory gadget's LAST code
// stream word is its instruction's guest address, and _ip is past it when a
// helper runs, so the segfault paths take the pc from [_ip, #-8].

.extern tlb_handle_miss
.extern tlb_write_ptr_slow
.extern __tlb_read_cross_page
.extern __tlb_write_cross_page



// Both segfault paths also rewind the guest PC to the FAULTING
// INSTRUCTION, read from the fixed [-8] slot of the just-consumed code
// stream: every memory-touching gadget's LAST parameter word is its
// instruction's guest address (emitted by gen_step_ppc64), and every
// gadget advances _ip past all its params before any prep macro can
// fault, so [_ip, #-8] is always that address here. Without this, an
// INT_PF resolved by the kernel (the everyday case: first write to a
// copy-on-write page after fork) resumed at the BLOCK-START pc,
// re-executing every instruction before the faulting one — real bug:
// busybox sh crashed post-fork because musl's __syscall_ret block did
// `mov x0,#-1` before the errno store, the store CoW-faulted, and the
// rerun computed the errno pointer from the clobbered x0. Re-executing
// just the faulting instruction is sound: its inputs are untouched at
// fault time (writeback in the load/store gadgets happens after the
// access).
.global ppc64_segfault_read
ppc64_segfault_read:
    ldur x9, [_ip, #-8]
    str x9, [_cpu, #CPU_ppc64_pc]
    ldr x8, [_tlb, #(-TLB_entries+TLB_segfault_addr)]
    str x8, [_cpu, #CPU_segfault_addr]
    strb wzr, [_cpu, #CPU_segfault_was_write]
    mov w0, #INT_PF
    b jit_exit

.global ppc64_segfault_write
ppc64_segfault_write:
    ldur x9, [_ip, #-8]
    str x9, [_cpu, #CPU_ppc64_pc]
    ldr x8, [_tlb, #(-TLB_entries+TLB_segfault_addr)]
    str x8, [_cpu, #CPU_segfault_addr]
    mov w9, #1
    strb w9, [_cpu, #CPU_segfault_was_write]
    mov w0, #INT_PF
    b jit_exit

// (Carried over from riscv64 with the comments that explain them.)
// Real bug caught by testing: these routines used x19 as their own scratch
// to stash the C call's return value across restore_c. But x19 (like
// x19-x26 generally) is exactly the register range ldp_gadget/stp_gadget
// (memory.S) hold their own live cross-call state in (rt/rt2/rn/mode/
// offset -- chosen specifically because x19-x28 are callee-saved and
// "guaranteed to survive any well-behaved C function call", per this
// file's own earlier header comment). These hand-written routines are not
// "well-behaved" in that sense: they clobber x19 without saving/restoring
// it, silently corrupting a caller's live offset (e.g. ldp/stp's
// pre/post-index displacement) on every miss/slow-path/crosspage call.
// restore_c only reloads x0-x18+lr, so x19 can't be used as scratch here
// either without permanently losing the caller's value. Fixed by using
// x27 instead -- unused by every gadget in this port so far, genuinely
// free for these routines' own internal use.
.global ppc64_handle_read_miss
ppc64_handle_read_miss:
    // The second chance (emu/tlb.h, TLB_VICTIMS): the page most recently
    // evicted from the table is the other of two pages that share an entry
    // and are used in turn. Found, it changes places with the one in the
    // entry, here, without the trip to C. A real miss pays the four
    // instructions down to the branch. x27 is this helper's own; x9-x11 are
    // the probe's scratch.
    ldur x11, [_tlb, #(TLB_victim - TLB_entries + TLB_ENTRY_page)]
    and x9, x7, #0xfffffffffffff000
    cmp x9, x11
    b.ne 1f
    eor x10, x7, x7, lsr #10
    ubfx x10, x10, #12, #10
    add x10, x10, x10, lsl #1
    add x10, _tlb, x10, lsl #3          // the page's entry
    sub x27, _tlb, #(TLB_entries - TLB_victim)
    .irp field, TLB_ENTRY_page, TLB_ENTRY_page_if_writable, TLB_ENTRY_data_minus_addr
    ldr x9, [x27, #\field]
    ldr x11, [x10, #\field]
    str x9, [x10, #\field]
    str x11, [x27, #\field]
    .endr
    add x7, x9, x7                      // x9: the found entry's data_minus_addr
    ret
1:
    save_c
    sub x0, _tlb, #TLB_entries
    mov x1, _addr
    mov x2, #MEM_READ
    bl NAME(tlb_handle_miss)
    mov x27, x0
    restore_c
    cbz x27, ppc64_segfault_read
    mov _addr, x27
    ret

.global ppc64_handle_write_miss
ppc64_handle_write_miss:
    // The second chance (emu/tlb.h, TLB_VICTIMS): the page most recently
    // evicted from the table is the other of two pages that share an entry
    // and are used in turn. Found, it changes places with the one in the
    // entry, here, without the trip to C. A real miss pays the four
    // instructions down to the branch. x27 is this helper's own; x9-x11 are
    // the probe's scratch.
    // (write_prep has already found the table current and no revalidation due.)
    ldur x11, [_tlb, #(TLB_victim - TLB_entries + TLB_ENTRY_page_if_writable)]
    and x9, x7, #0xfffffffffffff000
    cmp x9, x11
    b.ne 1f
    str x9, [_tlb, #(-TLB_entries+TLB_dirty_page)]
    eor x10, x7, x7, lsr #10
    ubfx x10, x10, #12, #10
    add x10, x10, x10, lsl #1
    add x10, _tlb, x10, lsl #3          // the page's entry
    sub x27, _tlb, #(TLB_entries - TLB_victim)
    .irp field, TLB_ENTRY_page, TLB_ENTRY_page_if_writable, TLB_ENTRY_data_minus_addr
    ldr x9, [x27, #\field]
    ldr x11, [x10, #\field]
    str x9, [x10, #\field]
    str x11, [x27, #\field]
    .endr
    add x7, x9, x7                      // x9: the found entry's data_minus_addr
    ret
1:
    save_c
    sub x0, _tlb, #TLB_entries
    mov x1, _addr
    mov x2, #MEM_WRITE
    bl NAME(tlb_handle_miss)
    mov x27, x0
    restore_c
    cbz x27, ppc64_segfault_write
    mov _addr, x27
    ret

.global ppc64_resolve_write_ptr
ppc64_resolve_write_ptr:
    save_c
    sub x0, _tlb, #TLB_entries
    mov x1, _addr
    bl NAME(tlb_write_ptr_slow)
    mov x27, x0
    restore_c
    cbz x27, ppc64_segfault_write
    mov _addr, x27
    ret

// Real bug caught by testing: _cpu is aliased to x1 (gadgets.h), the same
// TWO real register-aliasing bugs caught here, one by testing, one by
// the fix for the first: the AAPCS64 argument registers x0/x1/x2 ARE the
// gadget convention's _tmp/_cpu/_tlb, so argument setup order is load-
// bearing in both directions. The original wrote x1 (arg1 = addr) first,
// clobbering _cpu before arg2 (= _cpu + LOCAL_value) read it. The first
// "fix" moved the arg2 computation to the top -- which writes x2,
// clobbering _tlb before arg0 (= _tlb - TLB_entries) read it, sending a
// wild tlb pointer into __tlb_read_cross_page (caught by the page-
// straddling case in tests/arm64/ppc64_dpextra.s, host-crashing on
// tlb->mmu). The only safe order consumes x2 first and writes x1 last:
//   x0 <- f(x2)   (reads _tlb; x0/_tmp holds nothing live)
//   x2 <- f(x1)   (reads _cpu, overwrites _tlb -- already consumed)
//   x3 <- x27
//   x1 <- x7      (overwrites _cpu -- already consumed; x7/_addr last-read here)
// Also uses x27 (not x19) for the size argument gadgets.h's bullshit macro
// passes in -- same x19-corrupts-the-caller's-live-offset bug as the miss
// handlers above, just via argument-passing instead of return-stash.
//
// Serves BOTH reads and crosspage WRITES (i386's design, mirrored): for a
// write, this fills LOCAL_value with the current guest bytes, stashes the
// guest address in LOCAL_value_addr, and redirects _addr to the buffer so
// the gadget's store writes into it; write_done then detects the redirect
// and ppc64_crosspage_store (below) flushes the buffer back to guest
// memory at the stashed address.
.global ppc64_crosspage_load
ppc64_crosspage_load:
    save_c
    sub x0, _tlb, #TLB_entries
    mov x2, LOCAL_value
    add x2, _cpu, x2
    mov x3, x27             // size in bytes, set by the caller (gadgets.h's bullshit macro)
    mov x1, _addr
    bl NAME(__tlb_read_cross_page)
    mov x27, x0
    restore_c
    cbz x27, ppc64_segfault_read
    str _addr, [_cpu, #LOCAL_value_addr]
    mov _addr, LOCAL_value
    add _addr, _cpu, _addr
    ret

// Real bug caught by re-reading i386's flow (not by a failing test --
// nothing exercises crosspage writes yet): this used `mov x1, _addr` for
// the guest address, but by flush time _addr has been REDIRECTED to the
// LOCAL_value buffer by ppc64_crosspage_load above -- the guest address
// only survives in LOCAL_value_addr, which is where i386's own
// crosspage_store (jit/gadgets-aarch64/memory.S:138) reloads it from.
// LOCAL_value_addr was also widened to 64 bits (jit/frame.h) since arm64
// guest addresses don't fit the old 32-bit addr_t field.
.global ppc64_crosspage_store
ppc64_crosspage_store:
    save_c
    // Same argument-order constraint as ppc64_crosspage_load above:
    // consume x2 (_tlb) before writing it, write x1 (_cpu) last.
    sub x0, _tlb, #TLB_entries
    ldr x3, [_cpu, #LOCAL_value_addr] // guest addr, staged in x3 while _cpu is intact
    mov x2, LOCAL_value
    add x2, _cpu, x2
    mov x1, x3
    mov x3, x27             // size in bytes, set by the caller (gadgets.h's write_bullshit)
    bl NAME(__tlb_write_cross_page)
    mov x27, x0
    restore_c
    cbz x27, ppc64_segfault_write
    ret


