// 64-bit little-endian PowerPC GUEST gadgets: AArch64 host assembly
// implementing ppc64 guest semantics (docs/ppc64le_guest_plan.md). The
// conventions are jit/guest-riscv64/gadgets.h's, and its rules apply here
// unchanged: jit/gadgets-aarch64/entry.S's jit_enter/gret/jit_exit, _cpu=x1,
// _tlb=x2, _ip=x28, never x18, guest registers in memory and reached by the
// BYTE OFFSET of their slot in cpu_state, which jit/gen_ppc64.c puts in the
// code stream. `(rA|0)` is the offset of ppc64_zero when rA is 0.
//
// A guest value lives in the x view of a host register; a word op computes in
// the w view and extends as the instruction says. The condition register is a
// word per field (emu/cpu.h), XER's SO, OV and CA a word each.

#include "../gadgets-generic.h"
#include "cpu-offsets.h"
#include "emu/interrupt.h"

_cpu    .req x1
_tlb    .req x2
_ip     .req x28

_tmp    .req x0
_tmp2   .req x8
_addr   .req x7
// What a load or store body keeps across read_prep/write_prep (frag.h):
// callee-saved, since prep may call into C.
_sav    .req x19
_savw   .req w19

.extern jit_ret
.extern jit_exit

.macro .gadget name
    .global NAME(gadget_ppc64_\()\name)
    .align 4
    NAME(gadget_ppc64_\()\name) :
.endm

// Same dispatch as jit/guest-riscv64/gadgets.h's gret (and arm64's), selected
// by the same -Darm64_gret option; the measurements are there.
.macro gret pop=0
#if defined(ISH_ARM64_GRET_LDAR)
.if \pop != 0
    add _ip, _ip, \pop*8
.endif
    ldar x9, [_ip]
    add _ip, _ip, 8
#else
    // The load and the add both start from the old _ip: one update of _ip a
    // gadget, not two. See the arm64 guest's gret (jit/guest-arm64/gadgets.h).
    ldr x9, [_ip, \pop*8]    // no barrier: see arm64_chain (guest-arm64 gadgets.h)
    add _ip, _ip, \pop*8 + 8
#endif
    cbnz x9, 0f
    b jit_ret
0:  br x9
.endm

// The dispatch for a gadget that has already moved _ip past its parameters
// AND past the next gadget's word, in the one `add` it makes on entry (see
// jit/guest-arm64/gadgets.h's gret_ahead): a gadget that can fault has to
// move _ip before its access, and with gret that made two updates of _ip a
// gadget. Such a gadget passes the distance as the `ahead` argument of
// read_prep/write_prep and their _bullshit halves, so that every call out of
// them is made with _ip where the slow paths expect it.
.macro gret_ahead
#if defined(ISH_ARM64_GRET_LDAR)
    sub x9, _ip, #8
    ldar x9, [x9]
#else
    ldr x9, [_ip, #-8]
#endif
    cbnz x9, 0f
    b jit_ret
0:  br x9
.endm

.macro bl_ahead target, ahead
    .if \ahead
    sub _ip, _ip, #\ahead
    .endif
    bl \target
    .if \ahead
    add _ip, _ip, #\ahead
    .endif
.endm

// ---- TLB access machinery: verbatim copies of jit/guest-arm64/
// gadgets.h's read_prep/write_prep/write_done and save_c/restore_c
// (same _cpu/_tlb/_ip/_addr register convention), with the miss/
// crosspage helper symbols renamed to this port's ppc64_ copies
// (jit/guest-ppc64/core.S) so the segfault paths write
// CPU_ppc64_pc. Every hard-won rule documented there applies.

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

.macro \type\()_prep size, id, ahead=0
    // An access that crosses into the next page takes crosspage_load_\id --
    // BOTH types, i386's own design (jit/gadgets-aarch64/gadgets.h): for a
    // WRITE that straddles a page boundary, ppc64_crosspage_load fills
    // LOCAL_value with the current guest bytes, stashes the guest address
    // in LOCAL_value_addr, and redirects _addr to the LOCAL_value buffer;
    // the gadget's store instruction then writes into the buffer, and
    // write_done (below) detects _addr==LOCAL_value and calls
    // ppc64_crosspage_store to flush the buffer back to guest memory
    // using the stashed address. (Branching writes straight to
    // crosspage_store_\id flushes the still-uninitialized buffer and skips
    // the store: it was tried.)
    //
    // For 2, 4 and 8 bytes the crossing test is folded into the TLB compare:
    // the tag is compared with the address's page AND its low alignment bits,
    // so an aligned access -- which cannot cross -- hits in one compare, and
    // anything unaligned leaves by handle_miss_\id, which sorts out whether it
    // crosses, is merely unaligned inside its page (and probes again), or is a
    // real miss. That is three instructions and a branch off nearly every
    // load and store; one byte cannot cross and needs no test at all. A pair
    // (16 bytes, aligned to 8) can cross while aligned, so it keeps the
    // explicit test.
    .if \size > 64
    and w9, w7, #0xfff
    cmp x9, #(0x1000-(\size/8))
    b.hi crosspage_load_\id
    .endif
    .ifc \type,write
        ldr x10, [_tlb, #(-TLB_entries+TLB_mmu)]
        ldr x9, [_tlb, #(-TLB_entries+TLB_mem_changes)]
        ldr x11, [x10, #MMU_changes]
        cmp x9, x11
        b.ne slow_write_\id
        ldrb w10, [x10, #MMU_requires_write_revalidate]
        cbnz w10, slow_write_\id   // out of line (_bullshit): the store falls through
    .endif
    // 64-bit page math throughout: arm64 guest addresses are 48-bit, and a
    // 32-bit page compare would treat any two pages 4GiB apart as aliases.
    .if \size > 64 || \size == 8
    and x9, x7, #0xfffffffffffff000
    .else
    and x9, x7, #(0xfffffffffffff000 | (\size/8 - 1))
    .endif
    // TLB_INDEX (emu/tlb.h), page bits 0-9 xor bits 10-19, and the entry's
    // address, without the mask, the constant and the multiply the first
    // version of this took: xor the address with itself shifted and the
    // index is one field of the result; 24 bytes an entry is times three,
    // times eight.
    .if TLB_ENTRY_SIZE != 24 || TLB_BITS != 10
    .error "the TLB probe below is written for 1024 entries of 24 bytes"
    .endif
    eor x10, x7, x7, lsr #10
    ubfx x10, x10, #12, #10
    add x10, x10, x10, lsl #1
    add x10, _tlb, x10, lsl #3
    .ifc \type,read
        ldr x11, [x10, #TLB_ENTRY_page]
    .else
        ldr x11, [x10, #TLB_ENTRY_page_if_writable]
    .endif
    cmp x9, x11
    b.ne handle_miss_\id
    .ifc \type,write
        // (x9 is the page itself here: the compare passed)
        str x9, [_tlb, #(-TLB_entries+TLB_dirty_page)]
    .endif
    ldr x11, [x10, #TLB_ENTRY_data_minus_addr]
    add x7, x11, x7, uxtx
back_\id:
.endm

.macro \type\()_bullshit size, id, ahead=0
handle_miss_\id :
    .if \size > 8 && \size <= 64
    // Not a hit as probed (see \type\()_prep): crossing, unaligned, or missing.
    and w9, w7, #0xfff
    cmp x9, #(0x1000-(\size/8))
    b.hi crosspage_load_\id
    tst x7, #(\size/8 - 1)
    b.eq 1f
    // Unaligned inside its page: the same entry, compared with the page alone.
    and x9, x7, #0xfffffffffffff000
    cmp x9, x11
    b.ne 1f
    .ifc \type,write
        str x9, [_tlb, #(-TLB_entries+TLB_dirty_page)]
    .endif
    ldr x11, [x10, #TLB_ENTRY_data_minus_addr]
    add x7, x11, x7, uxtx
    b back_\id
1:
    .endif
    bl_ahead ppc64_handle_\type\()_miss, \ahead
    b back_\id
.ifc \type,write
slow_write_\id :
    bl_ahead ppc64_resolve_write_ptr, \ahead
    b back_\id
.endif
crosspage_load_\id :
    mov x27, #(\size/8)
    bl_ahead ppc64_crosspage_load, \ahead
    b back_\id
.ifc \type,write
crosspage_store_\id :
    mov x27, #(\size/8)
    bl_ahead ppc64_crosspage_store, \ahead
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

