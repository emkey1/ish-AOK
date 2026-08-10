// RISC-V GUEST gadgets (AArch64 host assembly implementing riscv64 guest
// semantics). Follows jit/guest-arm64/gadgets.h's conventions exactly —
// read that header first; its rules (x18 ban, entry.S reuse, memory-based
// guest GPR access) all apply here and are not repeated in full.
//
// Reuses jit/gadgets-aarch64/entry.S's jit_enter/gret/jit_exit unmodified,
// same as the arm64 guest engine: _cpu=x1, _tlb=x2, _ip=x28. Guest GPRs are
// memory-based (ldr/str against cpu_state per touch). Unlike guest-arm64,
// operands are addressed by BYTE OFFSET into cpu_state (computed by
// gen_step_riscv64 from the register number), so one gadget serves all 32
// registers and the x0-is-zero rule costs nothing here: gen passes
// CPU_riscv64_zero_sink as the destination offset for rd==x0 and the
// riscv64_regs[0] slot (never written, always 0) as any x0 source.

#include "../gadgets-generic.h"
#include "cpu-offsets.h"
#include "emu/interrupt.h"

_cpu    .req x1
_tlb    .req x2
_ip     .req x28

_tmp    .req x0
_tmp2   .req x8
_addr   .req x7

.extern jit_ret
.extern jit_exit

.macro .gadget name
    .global NAME(gadget_riscv64_\()\name)
    .align 4
    NAME(gadget_riscv64_\()\name) :
.endm

// Same dispatch as jit/guest-arm64/gadgets.h's gret, selected by the same
// -Darm64_gret option; see the measurements and rationale there. Short version:
// dmb is the default because ldar costs ~1.7x on ARMv8.0 while only buying ~6%
// on Apple Silicon. Keep in sync if entry.S's contract changes.
// ⚠ MEASURED, and it does NOT match the arm64 result: the riscv64 guest is
// INSENSITIVE to this choice. A9 iPad, sh-loop 20k, same two builds, both
// verified from `uname -v` and from the disassembled dispatch tail:
//     riscv64   9998ms (dmb)  vs 10065ms (ldar)  -- flat, 0.7%
//     arm64     3089ms (dmb)  vs  5394ms (ldar)  -- 1.75x
// So the "it is a verbatim copy of the arm64 dispatcher, therefore it must
// benefit the same" reasoning that first landed this was wrong, and is recorded
// here because it is an inviting mistake: the instruction really is identical,
// but this engine is ~3.2x slower per unit of guest work than the arm64 one, so
// dispatch is a much smaller share of its time and the per-dispatch cost is
// diluted to nothing. Kept on dmb anyway -- it measures the same either way on
// ARMv8.0, and keeping both engines on one setting means one code path.
// If riscv64 dispatch ever gets cheap enough to matter, re-measure rather than
// assuming it now tracks arm64.
.macro gret pop=0
#if defined(ISH_ARM64_GRET_LDAR)
.if \pop != 0
    add _ip, _ip, \pop*8
.endif
    ldar x9, [_ip]
#else
    ldr x9, [_ip, \pop*8]!
    dmb ishld
#endif
    add _ip, _ip, 8
    cbnz x9, 0f
    b jit_ret
0:  br x9
.endm

// ---- TLB access machinery: verbatim copies of jit/guest-arm64/
// gadgets.h's read_prep/write_prep/write_done and save_c/restore_c
// (same _cpu/_tlb/_ip/_addr register convention), with the miss/
// crosspage helper symbols renamed to this port's riscv64_ copies
// (jit/guest-riscv64/memory.S) so the segfault paths write
// CPU_riscv64_pc. Every hard-won rule documented there applies.

// ---- Memory access (TLB fast path) --------------------------------------
// Adapted from jit/gadgets-aarch64/gadgets.h's own read_prep/write_prep —
// NOT from OpenMinis' version, which assumes TLB_BITS=13 and a 32-byte
// tlb_entry stride (`lsl x9,x9,#5`). This codebase's actual layout is
// TLB_BITS=10 (jit/offsets.c's new MACRO(TLB_BITS) emits the real value)
// and a 24-byte tlb_entry (three 8-byte fields, MACRO(TLB_ENTRY_SIZE)) —
// i386's own gadgets.h already gets this exactly right (`ubfx x9,_xaddr,
// 12,10` / `eor x9,x9,_xaddr,lsr 22` / `mov w10,TLB_ENTRY_SIZE; madd x9,
// x9,w10,_tlb`), so this macro is a straight copy of that proven-correct
// pattern with register names adjusted for this file's _addr (x7, not
// i386's w3/x3) and _tlb already pointing at tlb->entries (same
// convention, set up once in entry.S, not per-gadget).
.irp type, read,write

.macro \type\()_prep size, id
    and w9, w7, #0xfff
    cmp x9, #(0x1000-(\size/8))
    // BOTH types branch to crosspage_load_\id -- this is i386's own design
    // (jit/gadgets-aarch64/gadgets.h line 48), deliberately: for a WRITE
    // that straddles a page boundary, riscv64_crosspage_load fills
    // LOCAL_value with the current guest bytes, stashes the guest address
    // in LOCAL_value_addr, and redirects _addr to the LOCAL_value buffer;
    // the gadget's store instruction then writes into the buffer, and
    // write_done (below) detects _addr==LOCAL_value and calls
    // riscv64_crosspage_store to flush the buffer back to guest memory
    // using the stashed address. A previous revision of this port
    // misdiagnosed this shared branch target as a label-collision bug and
    // "fixed" it by branching writes directly to crosspage_store_\id --
    // which flushes the still-uninitialized buffer and skips the actual
    // store entirely. Corrected by re-reading i386's flow end-to-end
    // (memory.S's crosspage_load/crosspage_store pair) rather than
    // reasoning about the prep macro in isolation.
    b.hi crosspage_load_\id
    .ifc \type,write
        ldr x10, [_tlb, #(-TLB_entries+TLB_mmu)]
        ldr x9, [_tlb, #(-TLB_entries+TLB_mem_changes)]
        ldr x11, [x10, #MMU_changes]
        cmp x9, x11
        b.ne slow_write_\id
        ldrb w10, [x10, #MMU_requires_write_revalidate]
        cbz w10, fast_write_\id
slow_write_\id :
        bl riscv64_resolve_write_ptr
        b back_\id
fast_write_\id :
    .endif
    // 64-bit page math throughout: this macro was copied from the i386
    // gadget set, whose w-register page compare and unmasked `eor ..,
    // lsr #22` index are only correct for 32-bit guest addresses. arm64
    // guest addresses are 48-bit: the unmasked eor indexed OUTSIDE the
    // 1024-entry TLB array (ld crashed the whole app dereferencing a
    // garbage data_minus_addr), and the 32-bit page compare would treat
    // any two pages 4GiB apart as aliases — silent wrong-memory access.
    and x9, x7, #0xfffffffffffff000
    .ifc \type,write
        str x9, [_tlb, #(-TLB_entries+TLB_dirty_page)]
    .endif
    ubfx x10, x7, #12, #10
    eor x10, x10, x7, lsr #22
    and x10, x10, #(1 << TLB_BITS) - 1
    mov w11, #TLB_ENTRY_SIZE
    madd x10, x10, x11, _tlb
    .ifc \type,read
        ldr x11, [x10, #TLB_ENTRY_page]
    .else
        ldr x11, [x10, #TLB_ENTRY_page_if_writable]
    .endif
    cmp x9, x11
    b.ne handle_miss_\id
    ldr x11, [x10, #TLB_ENTRY_data_minus_addr]
    add x7, x11, x7, uxtx
back_\id:
.endm

.macro \type\()_bullshit size, id
handle_miss_\id :
    bl riscv64_handle_\type\()_miss
    b back_\id
crosspage_load_\id :
    mov x27, #(\size/8)
    bl riscv64_crosspage_load
    b back_\id
.ifc \type,write
crosspage_store_\id :
    mov x27, #(\size/8)
    bl riscv64_crosspage_store
    b back_write_done_\id
.endif
.endm

.endr

.macro write_done size, id
    mov x9, LOCAL_value
    add x9, _cpu, x9
    cmp x9, x7
    b.eq crosspage_store_\id
back_write_done_\id :
.endm


.macro save_c
    stp x0, x1, [sp, -0xa0]!
    stp x2, x3, [sp, 0x10]
    stp x4, x5, [sp, 0x20]
    stp x6, x7, [sp, 0x30]
    stp x8, x9, [sp, 0x40]
    stp x10, x11, [sp, 0x50]
    stp x12, x13, [sp, 0x60]
    stp x14, x15, [sp, 0x70]
    stp x16, x17, [sp, 0x80]
    stp x18, lr, [sp, 0x90]
.endm

.macro restore_c
    ldp x18, lr, [sp, 0x90]
    ldp x16, x17, [sp, 0x80]
    ldp x14, x15, [sp, 0x70]
    ldp x12, x13, [sp, 0x60]
    ldp x10, x11, [sp, 0x50]
    ldp x8, x9, [sp, 0x40]
    ldp x6, x7, [sp, 0x30]
    ldp x4, x5, [sp, 0x20]
    ldp x2, x3, [sp, 0x10]
    ldp x0, x1, [sp], 0xa0
.endm

