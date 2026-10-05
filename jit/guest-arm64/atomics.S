// AArch64 guest atomic gadgets: LDXR/STXR (load/store exclusive) and CAS
// (LSE compare-and-swap). Independently written, not adapted from
// OpenMinis' equivalents. Semantics deliberately mirror this codebase's
// own interpreter handlers (emu/arm64_interp.c's exclusive/CAS blocks)
// exactly, including their documented limitations:
//
//   - The exclusive monitor is emulated via cpu->arm64_excl_addr/
//     arm64_excl_val (single-guest-thread-step semantics), NOT via host
//     ldxr/stxr. Host exclusives would be unsound here: gadget bodies
//     store to cpu_state between the guest's LDXR and STXR, and a host
//     store by the same PE is architecturally allowed to clear the local
//     monitor -- a livelock risk, not a correctness optimization.
//   - STXR succeeds iff the monitor address matches AND the memory value
//     still equals the value read by LDXR (a CAS-style check, same as
//     the interpreter). Cross-thread atomicity of the final store is not
//     modeled -- the same "differential-testing follow-up" caveat as the
//     interpreter's, inherited rather than widened.
//   - Acquire/release bits (LDAXR/STLXR's o0, CAS's A/R) are accepted
//     and ignored, matching the interpreter's decode: barriers aren't
//     modeled. (Non-exclusive LDAR/STLR never reach these gadgets at
//     all -- gen_step_arm64 lowers them to the plain single-register
//     load/store gadgets in memory.S, which is exactly their semantics
//     minus the unmodeled barrier.)
//
// Naturally-aligned atomics can never straddle a page (size <= 8 and the
// prep macros' boundary check is end-exclusive), so the crosspage paths
// below are only reachable for architecturally-misaligned atomics, which
// real hardware faults on anyway; like the interpreter, no alignment
// check is performed, and a misaligned atomic just takes the (slow but
// functionally consistent) crosspage path.
//
// Code stream: [gadget][rt | rn<<8 | rs<<16]. rs is unused by LDXR
// (gen_step_arm64 packs it anyway; the field is ignored here).

#include "gadgets.h"

.text

// ---- host atomics in the gadgets themselves ----
// Atomics are naturally aligned (a misaligned one is SIGBUS/BUS_ADRALN,
// INT_ALIGN), so one resolved page
// covers the access, and the host's own exclusives do the atomic part:
// ldaxr/stlxr loops -- ARMv8.0, so A10X hosts run them -- with nothing but
// register work between the pair.

// x7 = the guest address of an atomic of x0 bytes (its alignment too) ->
// x7 = a writable host pointer and x27 = 0; or x27 = INT_ALIGN (misaligned)
// or INT_PF (unmapped, read-only, ...), the fault recorded in cpu_state.
// Clobbers x9-x11; a miss or a write the mmu wants revalidated takes
// tlb_write_ptr_slow, as the scalar stores do.
a64_atomic_wptr:
    sub x9, x0, #1
    tst x7, x9
    b.ne 8f
    ldr x10, [_tlb, #(-TLB_entries+TLB_mmu)]
    ldr x9, [_tlb, #(-TLB_entries+TLB_mem_changes)]
    ldr x11, [x10, #MMU_changes]
    cmp x9, x11
    b.ne 2f
    ldrb w10, [x10, #MMU_requires_write_revalidate]
    cbnz w10, 2f
    and x9, x7, #0xfffffffffffff000
    ubfx x10, x7, #12, #10
    eor x10, x10, x7, lsr #22
    and x10, x10, #(1 << TLB_BITS) - 1
    mov w11, #TLB_ENTRY_SIZE
    madd x10, x10, x11, _tlb
    ldr x11, [x10, #TLB_ENTRY_page_if_writable]
    cmp x9, x11
    b.ne 2f
    str x9, [_tlb, #(-TLB_entries+TLB_dirty_page)]
    ldr x11, [x10, #TLB_ENTRY_data_minus_addr]
    add x7, x11, x7
    mov x27, #0
    ret
2:  save_c
    sub x0, _tlb, #TLB_entries     // (x2 read before it is written)
    mov x1, x7
    bl NAME(tlb_write_ptr_slow)
    mov x27, x0
    restore_c
    cbz x27, 9f
    mov x7, x27
    mov x27, #0
    ret
9:  ldr x9, [_tlb, #(-TLB_entries+TLB_segfault_addr)]
    str x9, [_cpu, #CPU_segfault_addr]
    mov w9, #1
    strb w9, [_cpu, #CPU_segfault_was_write]
    mov x27, #INT_PF
    ret
8:  str x7, [_cpu, #CPU_segfault_addr]
    mov w9, #1
    strb w9, [_cpu, #CPU_segfault_was_write]
    mov x27, #INT_ALIGN
    ret
// the same for a load (LDXP: a read-only page will do)
a64_atomic_rptr:
    sub x9, x0, #1
    tst x7, x9
    b.ne 8f
    and x9, x7, #0xfffffffffffff000
    ubfx x10, x7, #12, #10
    eor x10, x10, x7, lsr #22
    and x10, x10, #(1 << TLB_BITS) - 1
    mov w11, #TLB_ENTRY_SIZE
    madd x10, x10, x11, _tlb
    ldr x11, [x10, #TLB_ENTRY_page]
    cmp x9, x11
    b.ne 2f
    ldr x11, [x10, #TLB_ENTRY_data_minus_addr]
    add x7, x11, x7
    mov x27, #0
    ret
2:  save_c
    sub x0, _tlb, #TLB_entries
    mov x1, x7
    mov x2, #MEM_READ
    bl NAME(tlb_handle_miss)
    mov x27, x0
    restore_c
    cbz x27, 9f
    mov x7, x27
    mov x27, #0
    ret
9:  ldr x9, [_tlb, #(-TLB_entries+TLB_segfault_addr)]
    str x9, [_cpu, #CPU_segfault_addr]
    strb wzr, [_cpu, #CPU_segfault_was_write]
    mov x27, #INT_PF
    ret
8:  str x7, [_cpu, #CPU_segfault_addr]
    strb wzr, [_cpu, #CPU_segfault_was_write]
    mov x27, #INT_ALIGN
    ret

// compare-and-swap of 1 << \sizelog bytes at host x7: expected x24,
// desired x25 -> x8 = the value seen (zero-extended), x9 = 1 if stored
.macro host_cas sizelog
.if \sizelog == 0
    uxtb w10, w24
.elseif \sizelog == 1
    uxth w10, w24
.elseif \sizelog == 2
    mov w10, w24
.else
    mov x10, x24
.endif
71:
.if \sizelog == 0
    ldaxrb w8, [x7]
    cmp w8, w10
.elseif \sizelog == 1
    ldaxrh w8, [x7]
    cmp w8, w10
.elseif \sizelog == 2
    ldaxr w8, [x7]
    cmp w8, w10
.else
    ldaxr x8, [x7]
    cmp x8, x10
.endif
    b.ne 72f
.if \sizelog == 0
    stlxrb w9, w25, [x7]
.elseif \sizelog == 1
    stlxrh w9, w25, [x7]
.elseif \sizelog == 2
    stlxr w9, w25, [x7]
.else
    stlxr w9, x25, [x7]
.endif
    cbnz w9, 71b
    mov x9, #1
    b 73f
72: clrex
    mov x9, #0
73:
.endm

// 16-byte compare-and-swap at host x7: expected x24:x25, desired x26:x12
// (lo:hi) -> x8:x10 the pair seen, x9 = 1 if stored. A pair is read
// atomically only by a store-exclusive that succeeds, so a mismatch
// stores back what it read.
.macro host_cas128
71: ldaxp x8, x10, [x7]
    cmp x8, x24
    ccmp x10, x25, #0, eq
    b.ne 72f
    stlxp w9, x26, x12, [x7]
    cbnz w9, 71b
    mov x9, #1
    b 73f
72: stlxp w9, x8, x10, [x7]
    cbnz w9, 71b
    mov x9, #0
73:
.endm

// LDXR: load, then arm the monitor with the address and observed value.
.macro ldxr_gadget name, bits, ldinsn, reg
.gadget \name
    ldr x8, [_ip]
    add _ip, _ip, #16         // params + trailing orig_ip (fault restart)
    and x20, x8, #0x1f        // rt
    ubfx x22, x8, #8, #5      // rn (31 = SP)

    add x9, _cpu, #CPU_x0
    cmp x22, #31
    b.eq 1f
    add x26, x9, x22, lsl #3
    ldr x21, [x26]
    b 2f
1:  ldr x21, [_cpu, #CPU_sp]
2:  mov x7, x21               // keep the guest address in x21: read_prep
                              // translates x7 in place, and the monitor
                              // must record the GUEST address
    read_prep \bits, \name
    \ldinsn \reg, [_addr]
    add x9, _cpu, #CPU_x0
    cmp x20, #31
    b.eq 3f
    add x26, x9, x20, lsl #3
    str x17, [x26]
3:  str x21, [_cpu, #CPU_excl_addr]
    str x17, [_cpu, #CPU_excl_val]
    gret
    read_bullshit \bits, \name
.endm

// STXR: fails fast (status 1, monitor cleared) if the monitor isn't
// armed for this exact address; otherwise re-reads memory through the
// write-validated pointer, compares against the armed value, and stores
// only on match. Rs gets 0 on success, 1 on failure. The monitor clears
// on every STXR, success or not (architected behavior, and what the
// interpreter does).
// STXR: host-atomic store-conditional. Fails fast (status 1) if the
// monitor isn't armed for this guest address; otherwise attempts a host
// CAS(addr: excl_val -> Rt) (host_cas). A won CAS is
// STXR success (status 0); a lost CAS (another agent changed memory)
// is failure, so the guest LL/SC loop retries. Replaces the old
// load-compare-store, which raced (two threads could both pass the
// compare and both store, losing an update) -- the arm64/atomics64.c
// stress mix and a threaded atomic counter both exposed it.
// [gadget][rt | rn<<8 | rs<<16][orig_ip]
.macro stxr_gadget name, sizelog
.gadget \name
    ldr x8, [_ip]
    ldr x19, [_ip, #8]       // orig_ip (fault restart), callee-saved
    add _ip, _ip, #16
    and x20, x8, #0x1f       // rt (value to store)
    ubfx x22, x8, #8, #5     // rn (31 = SP)
    ubfx x23, x8, #16, #5    // rs (status out)

    add x9, _cpu, #CPU_x0
    cmp x22, #31
    b.eq 1f
    add x10, x9, x22, lsl #3
    ldr x21, [x10]           // guest address
    b 2f
1:  ldr x21, [_cpu, #CPU_sp]
2:
    ldr x9, [_cpu, #CPU_excl_addr]
    cmp x9, x21
    b.ne stxr_fail_\name

    ldr x24, [_cpu, #CPU_excl_val] // expected (typed CAS truncates to width)
    mov x25, #0                    // desired (Rt), 31 = XZR = 0
    cmp x20, #31
    b.eq 3f
    add x10, _cpu, #CPU_x0
    add x10, x10, x20, lsl #3
    ldr x25, [x10]
3:
    mov x7, x21
    mov x0, #(1 << \sizelog)
    bl a64_atomic_wptr
    cbnz x27, stxr_pf_\name
    host_cas \sizelog
    eor x11, x9, #1                // stored -> status 0 (success); not -> 1
    b stxr_status_\name
stxr_fail_\name:
    mov x11, #1
stxr_status_\name:
    mov x9, #-1                    // monitor clears on any STXR
    str x9, [_cpu, #CPU_excl_addr]
    cmp x23, #31
    b.eq 4f
    add x9, _cpu, #CPU_x0
    add x9, x9, x23, lsl #3
    str x11, [x9]
4:  gret
stxr_pf_\name:
    str x19, [_cpu, #CPU_pc]
    mov w0, w27
    b jit_exit
.endm

ldxr_gadget ldxr8,   8, ldrb, w17
ldxr_gadget ldxr16, 16, ldrh, w17
ldxr_gadget ldxr32, 32, ldr,  w17
ldxr_gadget ldxr64, 64, ldr,  x17

stxr_gadget stxr8,  0
stxr_gadget stxr16, 1
stxr_gadget stxr32, 2
stxr_gadget stxr64, 3

// CAS: host-atomic compare-and-swap (host_cas). Rs always receives the
// observed old value (zero-extended). Was load-compare-store (same race
// as STXR). [gadget][rt | rn<<8 | rs<<16][orig_ip]
.macro cas_gadget name, sizelog
.gadget \name
    ldr x8, [_ip]
    ldr x19, [_ip, #8]       // orig_ip
    add _ip, _ip, #16
    and x20, x8, #0x1f       // rt (new/desired)
    ubfx x22, x8, #8, #5     // rn (31 = SP)
    ubfx x23, x8, #16, #5    // rs (expected in, old out)

    add x9, _cpu, #CPU_x0
    cmp x22, #31
    b.eq 1f
    add x10, x9, x22, lsl #3
    ldr x21, [x10]           // guest address
    b 2f
1:  ldr x21, [_cpu, #CPU_sp]
2:
    mov x24, #0              // expected (Rs), 31 = XZR
    cmp x23, #31
    b.eq 3f
    add x10, _cpu, #CPU_x0
    add x10, x10, x23, lsl #3
    ldr x24, [x10]
3:
    mov x25, #0             // desired (Rt)
    cmp x20, #31
    b.eq 4f
    add x10, _cpu, #CPU_x0
    add x10, x10, x20, lsl #3
    ldr x25, [x10]
4:
    mov x7, x21
    mov x0, #(1 << \sizelog)
    bl a64_atomic_wptr
    cbnz x27, cas_pf_\name
    host_cas \sizelog
    cmp x23, #31           // Rs=31 discards
    b.eq cas_done_\name
    add x9, _cpu, #CPU_x0
    str x8, [x9, x23, lsl #3]
cas_done_\name:
    gret
cas_pf_\name:
    str x19, [_cpu, #CPU_pc]
    mov w0, w27
    b jit_exit
.endm

cas_gadget cas8,  0
cas_gadget cas16, 1
cas_gadget cas32, 2
cas_gadget cas64, 3

# vim: ft=gas

// CLREX: drop the exclusive reservation. Was mis-decoded into the barrier
// space (0xd503305f matches the DMB/DSB mask with CRm free), so a signal-
// handler or context-switch CLREX left the monitor armed and a following
// STXR could spuriously SUCCEED — found by arm64/atomics64.c's clrex check.
.gadget clrex
    mov x8, #-1
    str x8, [_cpu, #CPU_excl_addr]
    gret

// one LSE read-modify-write of \sz bytes at host x7, operand x25, op x24
// (0 add 1 clr 2 eor 3 set 4 smax 5 smin 6 umax 7 umin 8 swp), the old
// value (zero-extended) in x8
.macro rmw_loop ldx, stx, r, sz
.if \sz == 1
    sxtb x13, w25
    uxtb w14, w25
.elseif \sz == 2
    sxth x13, w25
    uxth w14, w25
.elseif \sz == 4
    sxtw x13, w25
    mov w14, w25
.else
    mov x13, x25
    mov x14, x25
.endif
71: \ldx \r\()8, [x7]
    adr x12, 70f
    add x12, x12, x24, lsl #3
    br x12
70: add x10, x8, x25
    b 79f
    bic x10, x8, x25
    b 79f
    eor x10, x8, x25
    b 79f
    orr x10, x8, x25
    b 79f
    b 74f
    nop
    b 75f
    nop
    b 76f
    nop
    b 77f
    nop
    mov x10, x25
    b 79f
74:
.if \sz == 1
    sxtb x11, w8
.elseif \sz == 2
    sxth x11, w8
.elseif \sz == 4
    sxtw x11, w8
.else
    mov x11, x8
.endif
    cmp x11, x13
    csel x10, x8, x25, gt
    b 79f
75:
.if \sz == 1
    sxtb x11, w8
.elseif \sz == 2
    sxth x11, w8
.elseif \sz == 4
    sxtw x11, w8
.else
    mov x11, x8
.endif
    cmp x11, x13
    csel x10, x8, x25, lt
    b 79f
76: cmp x8, x14
    csel x10, x8, x25, hi
    b 79f
77: cmp x8, x14
    csel x10, x8, x25, lo
79: \stx w9, \r\()10, [x7]
    cbnz w9, 71b
.endm

// LSE atomic RMW (LDADD/LDCLR/LDEOR/LDSET/LDSMAX/LDSMIN/LDUMAX/LDUMIN
// and SWP), host-atomic (rmw_loop). The operand comes from Rs (31 = XZR =
// 0), the old value is written to Rt (31 = discard).
// [gadget][rt | rn<<8 | rs<<16 | size<<24 | op<<26][orig_ip]
.gadget lse_rmw
    ldr x8, [_ip]
    ldr x19, [_ip, #8]        // orig_ip (fault restart), callee-saved
    add _ip, _ip, #16
    and x20, x8, #0x1f        // rt (old out)
    ubfx x21, x8, #8, #5      // rn
    ubfx x22, x8, #16, #5     // rs (operand)
    ubfx x23, x8, #24, #2     // size log2: 0=B 1=H 2=W 3=X
    ubfx x24, x8, #26, #4     // op

    add x9, _cpu, #CPU_x0
    cmp x21, #31
    b.eq 1f
    add x10, x9, x21, lsl #3
    ldr x7, [x10]            // addr
    b 2f
1:  ldr x7, [_cpu, #CPU_sp]
2:
    mov x25, #0
    cmp x22, #31
    b.eq 3f
    add x10, x9, x22, lsl #3
    ldr x25, [x10]          // operand
3:
    mov x0, #1
    lsl x0, x0, x23         // size_bytes, the alignment
    bl a64_atomic_wptr
    cbnz x27, lse_rmw_fault
    cmp x23, #2
    b.eq 32f
    b.hi 64f
    cbz x23, 8f
    rmw_loop ldaxrh, stlxrh, w, 2
    b 99f
8:  rmw_loop ldaxrb, stlxrb, w, 1
    b 99f
32: rmw_loop ldaxr, stlxr, w, 4
    b 99f
64: rmw_loop ldaxr, stlxr, x, 8
99:

    cmp x20, #31          // Rt=31 discards the old value
    b.eq lse_rmw_done
    add x9, _cpu, #CPU_x0
    str x8, [x9, x20, lsl #3] // Wt zero-extends via the size-limited read
lse_rmw_done:
    gret

lse_rmw_fault:
    str x19, [_cpu, #CPU_pc]
    mov w0, w27
    b jit_exit

// CASP (LSE compare-and-swap pair): host-atomic 2*sz-byte CAS (host_cas
// on the W pair as one 8-byte value, host_cas128 on the X pair). Rs/Rs+1 always receive the observed old pair (there is no
// status register; the LL/SC-style status is CAS-implicit). gen_step_arm64
// guarantees Rs and Rt are even (odd encodings are UNDEFINED), so Rs/Rt
// themselves are never 31; Rs+1/Rt+1 can be 31 only for Rs/Rt = 30, where
// register 31 is XZR (reads 0, writes discarded).
// [gadget][rt | rn<<8 | rs<<16][orig_ip]
.macro casp_gadget name, sizelog
.gadget \name
    ldr x8, [_ip]
    ldr x19, [_ip, #8]       // orig_ip (fault restart), callee-saved
    add _ip, _ip, #16
    and x20, x8, #0x1f       // rt (desired lo; rt+1 = desired hi)
    ubfx x22, x8, #8, #5     // rn (31 = SP)
    ubfx x23, x8, #16, #5    // rs (expected in / old out; rs+1 = hi)

    add x9, _cpu, #CPU_x0
    cmp x22, #31
    b.eq 1f
    add x10, x9, x22, lsl #3
    ldr x21, [x10]           // guest address
    b 2f
1:  ldr x21, [_cpu, #CPU_sp]
2:
    ldr x24, [x9, x23, lsl #3]  // expected lo (Rs, even, never 31)
    add x11, x23, #1
    mov x25, #0
    cmp x11, #31
    b.eq 3f
    ldr x25, [x9, x11, lsl #3]  // expected hi (Rs+1; 31 = XZR)
3:
    ldr x26, [x9, x20, lsl #3]  // desired lo (Rt, even, never 31)
    add x11, x20, #1
    mov x12, #0
    cmp x11, #31
    b.eq 4f
    ldr x12, [x9, x11, lsl #3]  // desired hi (Rt+1; 31 = XZR)
4:
    mov x7, x21
    mov x0, #(2 << \sizelog)    // the whole pair's alignment
    bl a64_atomic_wptr
    cbnz x27, casp_fault_\name
.if \sizelog == 2                // the W pair: one 8-byte value, lo first
    mov w24, w24
    orr x24, x24, x25, lsl #32
    mov w26, w26
    orr x25, x26, x12, lsl #32
    host_cas 3
    lsr x9, x8, #32
    mov w8, w8
.else
    host_cas128
    mov x9, x10
.endif
    add x10, _cpu, #CPU_x0
    str x8, [x10, x23, lsl #3]  // old lo -> Rs
    add x11, x23, #1
    cmp x11, #31                // Rs+1 = 31 (Rs = 30) discards the hi half
    b.eq casp_done_\name
    str x9, [x10, x11, lsl #3]  // old hi -> Rs+1
casp_done_\name:
    gret
casp_fault_\name:
    str x19, [_cpu, #CPU_pc]
    mov w0, w27
    b jit_exit
.endm

casp_gadget casp32, 2
casp_gadget casp64, 3

// LDXP (load exclusive pair): the alignment check, the pair load (the X
// pair as two loads: a torn one fails the STXP's CAS, and the guest's LL/SC
// loop retries) and arming excl_addr/excl_val/excl_val_hi, then the
// two results into Rt/Rt2 (31 = discard; Rt==Rt2 is CONSTRAINED
// UNPREDICTABLE and simply stores hi last). LDAXP's acquire bit (o0) is
// accepted and ignored, same as every other atomic gadget here.
// [gadget][rt | rn<<8 | rt2<<16][orig_ip]
.macro ldxp_gadget name, sizelog
.gadget \name
    ldr x8, [_ip]
    ldr x19, [_ip, #8]       // orig_ip (fault restart), callee-saved
    add _ip, _ip, #16
    and x20, x8, #0x1f       // rt (lo out)
    ubfx x22, x8, #8, #5     // rn (31 = SP)
    ubfx x23, x8, #16, #5    // rt2 (hi out)

    add x9, _cpu, #CPU_x0
    cmp x22, #31
    b.eq 1f
    add x10, x9, x22, lsl #3
    ldr x21, [x10]           // guest address
    b 2f
1:  ldr x21, [_cpu, #CPU_sp]
2:
    mov x7, x21
    mov x0, #(2 << \sizelog)
    bl a64_atomic_rptr
    cbnz x27, ldxp_pf_\name
.if \sizelog == 2
    ldr x8, [x7]
    lsr x9, x8, #32
    mov w8, w8
.else
    ldr x8, [x7]                 // two 8-byte loads: a pair load that is
    ldr x9, [x7, #8]             // torn shows at STXP, whose CAS fails
.endif
    str x21, [_cpu, #CPU_excl_addr]
    str x8, [_cpu, #CPU_excl_val]
    str x9, [_cpu, #CPU_excl_val_hi]
    add x10, _cpu, #CPU_x0
    cmp x20, #31             // Rt = 31 discards the lo half
    b.eq 3f
    str x8, [x10, x20, lsl #3]
3:  cmp x23, #31             // Rt2 = 31 discards the hi half
    b.eq 4f
    str x9, [x10, x23, lsl #3]
4:  gret
ldxp_pf_\name:
    str x19, [_cpu, #CPU_pc]
    mov w0, w27
    b jit_exit
.endm

ldxp_gadget ldxp32, 2
ldxp_gadget ldxp64, 3

// STXP (store exclusive pair): the monitor check, a host-atomic pair CAS
// against the armed pair, and the monitor clear, here; the gadget takes the desired pair from Rt/Rt2 (31 = XZR = 0) and writes
// the returned status to Rs (0 = stored, 1 = failed; 31 = discard).
// STLXP's release bit (o0) is accepted and ignored. On a fault the
// monitor is preserved (helper contract) so the restarted instruction
// can succeed after e.g. a COW break — same as the stxr gadget.
// [gadget][rt | rn<<8 | rs<<16 | rt2<<24][orig_ip]
.macro stxp_gadget name, sizelog
.gadget \name
    ldr x8, [_ip]
    ldr x19, [_ip, #8]       // orig_ip (fault restart), callee-saved
    add _ip, _ip, #16
    and x20, x8, #0x1f       // rt (desired lo)
    ubfx x22, x8, #8, #5     // rn (31 = SP)
    ubfx x23, x8, #16, #5    // rs (status out)
    ubfx x24, x8, #24, #5    // rt2 (desired hi)

    add x9, _cpu, #CPU_x0
    cmp x22, #31
    b.eq 1f
    add x10, x9, x22, lsl #3
    ldr x21, [x10]           // guest address
    b 2f
1:  ldr x21, [_cpu, #CPU_sp]
2:
    mov x25, #0              // desired lo (Rt; 31 = XZR = 0)
    cmp x20, #31
    b.eq 3f
    ldr x25, [x9, x20, lsl #3]
3:  mov x26, #0              // desired hi (Rt2; 31 = XZR = 0)
    cmp x24, #31
    b.eq 4f
    ldr x26, [x9, x24, lsl #3]
4:
    mov x8, #1                   // status: failed unless stored
    ldr x9, [_cpu, #CPU_excl_addr]
    cmp x9, x21
    b.ne 6f
    mov x7, x21
    mov x0, #(2 << \sizelog)
    bl a64_atomic_wptr
    cbnz x27, stxp_pf_\name     // the monitor stays armed for the restart
    mov x12, x26                 // desired hi
    mov x26, x25                 // desired lo
    ldr x24, [_cpu, #CPU_excl_val]
    ldr x25, [_cpu, #CPU_excl_val_hi]
.if \sizelog == 2
    mov w24, w24
    orr x24, x24, x25, lsl #32
    mov w26, w26
    orr x25, x26, x12, lsl #32
    host_cas 3
.else
    host_cas128
.endif
    eor x8, x9, #1
6:  mov x9, #-1                  // any completed STXP clears the monitor
    str x9, [_cpu, #CPU_excl_addr]
    cmp x23, #31             // Rs = 31 discards the status
    b.eq 5f
    add x9, _cpu, #CPU_x0
    str x8, [x9, x23, lsl #3]
5:  gret
stxp_pf_\name:
    str x19, [_cpu, #CPU_pc]
    mov w0, w27
    b jit_exit
.endm

stxp_gadget stxp32, 2
stxp_gadget stxp64, 3
