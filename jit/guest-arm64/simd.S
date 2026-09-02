// AArch64 guest SIMD/FP gadgets, phase D minimal subset: the vector
// loads/stores and register moves musl's memset/memcpy (and
// compiler-generated struct zeroing: `movi v0.2d, #0; stp q0, q0`)
// actually execute. Independently written, not adapted from OpenMinis.
// Blocker-driven like the rest of the port -- CMEQ/ADDP/scan-style
// vector arithmetic waits until a real binary demands it.
//
// Guest V registers live in cpu_state.arm64_v (32 x 16 bytes, offset
// CPU_v0, 16-byte stride -- note lsl #4, not the GPR file's lsl #3).
//
// Host SIMD register discipline: gadgets only use v30/v31, and only
// between a completed read_prep/write_prep and the corresponding
// architectural load/store -- never live across anything that can call
// into C (AAPCS64 preserves only the LOW 64 bits of v8-v15; there is NO
// fully callee-saved 128-bit register, so "don't hold vector state
// across calls" is the only sound rule). For stores this means the
// value is loaded from cpu_state AFTER write_prep, not before --
// opposite of the integer store gadgets' ordering, same soundness
// argument.
//
// Unlike the GPR file, SIMD register number 31 is a real register (no
// ZR/SP aliasing), so none of these gadgets special-case 31.

#include "gadgets.h"

.text

// ---- Single vector/FP register load/store -------------------------------
// Same code-stream format and addressing families as memory.S's integer
// single-register gadgets (the shared ldst_single_setup/-writeback
// macros do all the work): [rt | rn<<8 | mode<<16][offset word].
// Scalar sub-128-bit loads zero the rest of the V register slot, per
// the architecture (a write to Bt/Ht/St/Dt zeroes the upper bits).
.macro vload_single name, bits, ldinsn, vreg
.gadget \name
    ldst_single_setup
    read_prep \bits, \name
    movi v31.2d, #0
    \ldinsn \vreg, [_addr]
    add x9, _cpu, #CPU_v0
    add x9, x9, x20, lsl #4
    str q31, [x9]
    ldst_single_writeback
    read_bullshit \bits, \name
.endm

.macro vstore_single name, bits, stinsn, vreg
.gadget \name
    ldst_single_setup
    write_prep \bits, \name
    add x9, _cpu, #CPU_v0
    add x9, x9, x20, lsl #4
    ldr q31, [x9]
    \stinsn \vreg, [_addr]
    write_done \bits, \name
    ldst_single_writeback
    write_bullshit \bits, \name
.endm

vload_single vload8,     8, ldr, b31
vload_single vload16,   16, ldr, h31
vload_single vload32,   32, ldr, s31
vload_single vload64,   64, ldr, d31
vload_single vload128, 128, ldr, q31
vstore_single vstore8,     8, str, b31
vstore_single vstore16,   16, str, h31
vstore_single vstore32,   32, str, s31
vstore_single vstore64,   64, str, d31
vstore_single vstore128, 128, str, q31

// ---- LDR (literal, SIMD&FP): V=1 counterpart of memory.S's load_literal.
// The address is compile-time known (PC + imm19*4, computed by
// gen_step_arm64): same code-stream format, [gadget][rt][absolute address].
// Compilers emit this constantly for FP constant pools (e.g. loading a
// literal double) -- LuaJIT's interpreter/mcode hit it immediately on
// startup and, with no gadget to decode it, took a guest SIGILL (the
// nvim-on-arm64-guest crash-on-launch bug). Unlike the GPR form, register
// 31 needs no ZR-discard special case: the SIMD register file has no ZR/SP
// aliasing (see this file's header comment), so there's nothing to skip.
.macro vload_literal name, bits, ldinsn, vreg
.gadget \name
    ldr x20, [_ip]            // rt
    ldr x7, [_ip, #8]         // absolute guest address
    add _ip, _ip, #24         // rt + address + trailing orig_ip (fault restart)
    read_prep \bits, \name
    movi v31.2d, #0
    \ldinsn \vreg, [_addr]
    add x9, _cpu, #CPU_v0
    add x9, x9, x20, lsl #4
    str q31, [x9]
    gret
    read_bullshit \bits, \name
.endm

vload_literal vload_lit_s,   32, ldr, s31
vload_literal vload_lit_d,   64, ldr, d31
vload_literal vload_lit_q,  128, ldr, q31

// ---- Vector/FP register pair load/store (LDP/STP, V=1) ------------------
// Same code-stream format as memory.S's integer pair gadgets:
// [rt | rt2<<8 | rn<<16 | mode<<24 (1=post,2=offset,3=pre)][scaled offset].
// The 256-bit Q-pair case is the widest crosspage access this guest can
// stage into jit_frame.value; the i386 guest's 512-byte FXSAVE is what
// actually sizes that buffer.
.macro vldp_gadget name, bits, bytes, ldinsn, vreg1, vreg2
.gadget \name
    ldr x8, [_ip]
    ldr x19, [_ip, #8]
    add _ip, _ip, #24         // params + offset + trailing orig_ip (fault restart)
    and x20, x8, #0x1f        // vt
    ubfx x21, x8, #8, #5      // vt2
    ubfx x22, x8, #16, #5     // rn (31 = SP)
    ubfx x23, x8, #24, #2     // mode

    add x9, _cpu, #CPU_x0
    cmp x22, #31
    b.eq 1f
    add x26, x9, x22, lsl #3
    ldr x24, [x26]
    b 2f
1:  ldr x24, [_cpu, #CPU_sp]
2:  cmp x23, #1
    b.eq 3f
    add x7, x24, x19
    b 4f
3:  mov x7, x24
4:
    read_prep \bits, \name
    movi v30.2d, #0
    movi v31.2d, #0
    \ldinsn \vreg1, [_addr]
    \ldinsn \vreg2, [_addr, #\bytes]
    add x9, _cpu, #CPU_v0
    add x26, x9, x20, lsl #4
    str q30, [x26]
    add x26, x9, x21, lsl #4
    str q31, [x26]

    cmp x23, #2
    b.eq 6f
    add x25, x24, x19
    cmp x22, #31
    b.eq 5f
    add x9, _cpu, #CPU_x0
    add x26, x9, x22, lsl #3
    str x25, [x26]
    b 6f
5:  str x25, [_cpu, #CPU_sp]
6:  gret
    read_bullshit \bits, \name
.endm

.macro vstp_gadget name, bits, bytes, stinsn, vreg1, vreg2
.gadget \name
    ldr x8, [_ip]
    ldr x19, [_ip, #8]
    add _ip, _ip, #24         // params + offset + trailing orig_ip (fault restart)
    and x20, x8, #0x1f        // vt
    ubfx x21, x8, #8, #5      // vt2
    ubfx x22, x8, #16, #5     // rn
    ubfx x23, x8, #24, #2     // mode

    add x9, _cpu, #CPU_x0
    cmp x22, #31
    b.eq 1f
    add x26, x9, x22, lsl #3
    ldr x24, [x26]
    b 2f
1:  ldr x24, [_cpu, #CPU_sp]
2:  cmp x23, #1
    b.eq 3f
    add x7, x24, x19
    b 4f
3:  mov x7, x24
4:
    write_prep \bits, \name
    add x9, _cpu, #CPU_v0     // values loaded AFTER prep -- see file header
    add x26, x9, x20, lsl #4
    ldr q30, [x26]
    add x26, x9, x21, lsl #4
    ldr q31, [x26]
    \stinsn \vreg1, [_addr]
    \stinsn \vreg2, [_addr, #\bytes]
    write_done \bits, \name

    cmp x23, #2
    b.eq 6f
    add x25, x24, x19
    cmp x22, #31
    b.eq 5f
    add x9, _cpu, #CPU_x0
    add x26, x9, x22, lsl #3
    str x25, [x26]
    b 6f
5:  str x25, [_cpu, #CPU_sp]
6:  gret
    write_bullshit \bits, \name
.endm

vldp_gadget vldp32,   64,  4, ldr, s30, s31
vldp_gadget vldp64,  128,  8, ldr, d30, d31
vldp_gadget vldp128, 256, 16, ldr, q30, q31
vstp_gadget vstp32,   64,  4, str, s30, s31
vstp_gadget vstp64,  128,  8, str, d30, d31
vstp_gadget vstp128, 256, 16, str, q30, q31

// ---- DUP (general register to all vector lanes) --------------------------
// Code stream: [gadget][vd | rn<<8 | size<<16 (0=B,1=H,2=S,3=D) | q<<19]
// Writes to a sub-128-bit arrangement (q=0) zero the upper half, which
// the host dup does natively.
.gadget dup_gen
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f         // vd
    ubfx x10, x8, #8, #5      // rn (GPR; 31 = ZR)
    ubfx x11, x8, #16, #2     // size
    ubfx x12, x8, #19, #1     // q

    add x13, _cpu, #CPU_x0
    cmp x10, #31
    b.eq 1f
    add x14, x13, x10, lsl #3
    ldr x15, [x14]
    b 2f
1:  mov x15, #0
2:
    cmp x11, #0
    b.eq dup_b
    cmp x11, #1
    b.eq dup_h
    cmp x11, #2
    b.eq dup_s
    dup v31.2d, x15           // size D is 128-bit only (Q=0 is unallocated)
    b dup_store
dup_b:
    cbz x12, 3f
    dup v31.16b, w15
    b dup_store
3:  dup v31.8b, w15
    b dup_store
dup_h:
    cbz x12, 4f
    dup v31.8h, w15
    b dup_store
4:  dup v31.4h, w15
    b dup_store
dup_s:
    cbz x12, 5f
    dup v31.4s, w15
    b dup_store
5:  dup v31.2s, w15
dup_store:
    add x13, _cpu, #CPU_v0
    add x13, x13, x9, lsl #4
    str q31, [x13]
    gret

// ---- Vector constant (MOVI/MVNI/FMOV-immediate) --------------------------
// The modified-immediate expansion (AdvSIMDExpandImm) happens at compile
// time in gen_step_arm64; this gadget just writes a 128-bit constant.
// Code stream: [gadget][vd][lo64][hi64]
.gadget vconst
    ldr x9, [_ip]
    ldr x10, [_ip, #8]
    ldr x11, [_ip, #16]
    add _ip, _ip, #24
    and x9, x9, #0x1f
    add x13, _cpu, #CPU_v0
    add x13, x13, x9, lsl #4
    stp x10, x11, [x13]
    gret

// ORR/BIC (vector, immediate): the read-modify-write cousins of MOVI in
// the modified-immediate family (cargo's core-arch probe SIGILL'd on
// `orr v0.2s, #0x10, lsl #16` = 0x0f005600). gen_step_arm64 expands the
// abc:defgh/cmode pattern to the replicated 64-bit constant and passes a
// high-half keep-mask: ~0 for the Q form, 0 for the 64-bit form (whose
// architected write ZEROES the upper half -- OR-with-0 would wrongly
// preserve it). Code stream: [gadget][rd][imm64][himask]
.gadget vorr_imm
    ldr x9, [_ip]
    ldr x10, [_ip, #8]        // expanded immediate
    ldr x11, [_ip, #16]       // high-half keep-mask (Q ? ~0 : 0)
    add _ip, _ip, #24
    and x9, x9, #0x1f
    add x13, _cpu, #CPU_v0
    add x13, x13, x9, lsl #4
    ldp x14, x15, [x13]
    orr x14, x14, x10
    orr x15, x15, x10
    and x15, x15, x11         // Q=0: architected zero of the upper half
    stp x14, x15, [x13]
    gret

.gadget vbic_imm
    ldr x9, [_ip]
    ldr x10, [_ip, #8]
    ldr x11, [_ip, #16]
    add _ip, _ip, #24
    and x9, x9, #0x1f
    add x13, _cpu, #CPU_v0
    add x13, x13, x9, lsl #4
    ldp x14, x15, [x13]
    bic x14, x14, x10
    bic x15, x15, x10
    and x15, x15, x11
    stp x14, x15, [x13]
    gret

# vim: ft=gas

// ---- Vector element <-> general register moves ---------------------------
// UMOV/SMOV/INS and the scalar FMOV general<->FP forms all reduce to an
// integer load/store against the V-register slot in cpu_state at
// byteoff = index << size -- no host vector registers involved at all.
// gen_step_arm64 computes the byte offset at compile time.

// Element -> GPR (UMOV/SMOV/FMOV-to-general).
// Code stream: [gadget][rd | vn<<8 | byteoff<<16 | size<<24 | sext<<26 | sf<<27]
.gadget vext_to_gpr
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f         // rd (GPR; 31 = ZR discard)
    ubfx x10, x8, #8, #5      // vn
    ubfx x11, x8, #16, #4     // byte offset within the 16-byte slot
    ubfx x12, x8, #24, #2     // size (log2 bytes)
    ubfx x13, x8, #26, #1     // sign-extend (SMOV)
    ubfx x14, x8, #27, #1     // sf (64-bit destination)

    add x15, _cpu, #CPU_v0
    add x15, x15, x10, lsl #4
    add x15, x15, x11
    cmp x12, #0
    b.eq vext_b
    cmp x12, #1
    b.eq vext_h
    cmp x12, #2
    b.eq vext_s
    ldr x16, [x15]            // D: always a plain 64-bit read
    b vext_store
vext_b:
    cbz x13, 1f
    cbz x14, 2f
    ldrsb x16, [x15]
    b vext_store
2:  ldrsb w16, [x15]
    b vext_store
1:  ldrb w16, [x15]
    b vext_store
vext_h:
    cbz x13, 3f
    cbz x14, 4f
    ldrsh x16, [x15]
    b vext_store
4:  ldrsh w16, [x15]
    b vext_store
3:  ldrh w16, [x15]
    b vext_store
vext_s:
    cbz x13, 5f
    ldrsw x16, [x15]          // SMOV Xd, Vn.S[i] (Wd form is unallocated)
    b vext_store
5:  ldr w16, [x15]
vext_store:
    cmp x9, #31
    b.eq 6f
    add x15, _cpu, #CPU_x0
    add x15, x15, x9, lsl #3
    str x16, [x15]
6:  gret

// GPR -> element, PRESERVING the other lanes (INS Vd.T[i], Rn).
// Code stream: [gadget][vd | rn<<8 | byteoff<<16 | size<<24]
.gadget vins_gpr
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f         // vd
    ubfx x10, x8, #8, #5      // rn (GPR; 31 = ZR)
    ubfx x11, x8, #16, #4     // byte offset
    ubfx x12, x8, #24, #2     // size

    add x13, _cpu, #CPU_x0
    cmp x10, #31
    b.eq 1f
    add x14, x13, x10, lsl #3
    ldr x16, [x14]
    b 2f
1:  mov x16, #0
2:  add x15, _cpu, #CPU_v0
    add x15, x15, x9, lsl #4
    add x15, x15, x11
    cmp x12, #0
    b.eq 3f
    cmp x12, #1
    b.eq 4f
    cmp x12, #2
    b.eq 5f
    str x16, [x15]
    b 6f
3:  strb w16, [x15]
    b 6f
4:  strh w16, [x15]
    b 6f
5:  str w16, [x15]
6:  gret

// GPR -> vector scalar, ZEROING the rest (FMOV Sd, Wn / FMOV Dd, Xn --
// a scalar FP write architecturally clears the remaining bits, unlike
// INS). Code stream: [gadget][vd | rn<<8 | size<<16 (2=S, 3=D)]
.gadget vmov_from_gpr
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    ubfx x11, x8, #16, #2

    add x13, _cpu, #CPU_x0
    cmp x10, #31
    b.eq 1f
    add x14, x13, x10, lsl #3
    ldr x16, [x14]
    b 2f
1:  mov x16, #0
2:  add x15, _cpu, #CPU_v0
    add x15, x15, x9, lsl #4
    stp xzr, xzr, [x15]
    cmp x11, #3
    b.eq 3f
    str w16, [x15]
    b 4f
3:  str x16, [x15]
4:  gret

// ---- AdvSIMD three-same bitwise: AND/BIC/ORR/ORN/EOR/BSL/BIT/BIF ---------
// Whole-register bitwise ops (element size irrelevant); includes the
// vector MOV alias (ORR with Rn==Rm), which is how compilers copy V
// registers. BSL/BIT/BIF additionally read Rd (insert semantics), which
// the host instruction handles natively. Q=0 forms zero the upper half,
// which host ops on the .8b arrangement do architecturally.
// Code stream: [gadget][rd | rn<<8 | rm<<16]
.macro vbitwise name, op, arr
.gadget \name
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    ubfx x11, x8, #16, #5
    add x12, _cpu, #CPU_v0
    add x13, x12, x9, lsl #4
    ldr q29, [x13]             // current Rd (BSL/BIT/BIF read it; harmless otherwise)
    add x14, x12, x10, lsl #4
    ldr q30, [x14]
    add x14, x12, x11, lsl #4
    ldr q31, [x14]
    \op v29\arr, v30\arr, v31\arr
    str q29, [x13]
    gret
.endm

// The 2-operand forms overwrite v29 outright; the 3-with-insert forms
// (bsl/bit/bif) use v29 as the architected Rd input/output.
.macro vbitwise2 name, op, arr
.gadget \name
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    ubfx x11, x8, #16, #5
    add x12, _cpu, #CPU_v0
    add x14, x12, x10, lsl #4
    ldr q30, [x14]
    add x14, x12, x11, lsl #4
    ldr q31, [x14]
    \op v29\arr, v30\arr, v31\arr
    add x13, x12, x9, lsl #4
    str q29, [x13]
    gret
.endm

vbitwise2 vand_16b, and, .16b
vbitwise2 vand_8b,  and, .8b
vbitwise2 vbic_16b, bic, .16b
vbitwise2 vbic_8b,  bic, .8b
vbitwise2 vorr_16b, orr, .16b
vbitwise2 vorr_8b,  orr, .8b
vbitwise2 vorn_16b, orn, .16b
vbitwise2 vorn_8b,  orn, .8b
vbitwise2 veor_16b, eor, .16b
vbitwise2 veor_8b,  eor, .8b
vbitwise vbsl_16b, bsl, .16b
vbitwise vbsl_8b,  bsl, .8b
vbitwise vbit_16b, bit, .16b
vbitwise vbit_8b,  bit, .8b
vbitwise vbif_16b, bif, .16b
vbitwise vbif_8b,  bif, .8b

// ---- AdvSIMD scalar shift by immediate (D size): SHL/USHR/SSHR ----------
// musl's strtod builds the double's exponent field with `shl d, d, #52`.
// The shift amount is a compile-time field but a runtime parameter to the
// shared gadget, so the shift runs as an integer register-amount shift on
// the low 64 bits of the slot (bit-identical to the scalar SIMD op), and
// the store zeroes the upper half like the architected scalar write.
// Code stream: [gadget][rd | rn<<8 | shift<<16 | op<<24 (0=shl,1=ushr,2=sshr)]
.gadget vshift_d
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    ubfx x11, x8, #16, #6
    ubfx x13, x8, #24, #2
    add x12, _cpu, #CPU_v0
    add x14, x12, x10, lsl #4
    ldr x16, [x14]            // low 64 bits of Vn
    cmp x13, #1
    b.lo 1f
    b.eq 2f
    asr x16, x16, x11
    b 3f
1:  lsl x16, x16, x11
    b 3f
2:  lsr x16, x16, x11
3:  add x14, x12, x9, lsl #4
    stp x16, xzr, [x14]       // scalar D write zeroes the upper half
    gret

// ---- AdvSIMD vector shift by immediate: SSHR/USHR/SHL + SSHLL/USHLL -----
// (Found as an on-device crash-loop: Alpine getty executes
// `ushll v31.2d, v31.2s, #0` — the UXTL alias.) The host's immediate-form
// shifts can't take a runtime amount, but the REGISTER-form vector
// shifts (sshl/ushl) shift each lane by a per-lane amount from a
// register, with negative amounts shifting right — so one gadget per
// (arrangement, signedness) covers SSHR (sshl, negative), USHR (ushl,
// negative), and SHL (ushl, positive), with gen_step_arm64 encoding the
// direction in the sign of the streamed amount.
// Code stream: [gadget][rd | rn<<8][amount (signed)]
.macro vshift_gadget name, shiftop, arr, dupw
.gadget \name
    ldr x8, [_ip]
    ldr x9, [_ip, #8]
    add _ip, _ip, #16
    and x10, x8, #0x1f
    ubfx x11, x8, #8, #5
    add x12, _cpu, #CPU_v0
    add x13, x12, x11, lsl #4
    ldr q30, [x13]
    dup v29\arr, \dupw
    \shiftop v30\arr, v30\arr, v29\arr
    add x13, x12, x10, lsl #4
    str q30, [x13]
    gret
.endm

vshift_gadget vshl_s_8b,  sshl, .8b,  w9
vshift_gadget vshl_s_16b, sshl, .16b, w9
vshift_gadget vshl_s_4h,  sshl, .4h,  w9
vshift_gadget vshl_s_8h,  sshl, .8h,  w9
vshift_gadget vshl_s_2s,  sshl, .2s,  w9
vshift_gadget vshl_s_4s,  sshl, .4s,  w9
vshift_gadget vshl_s_2d,  sshl, .2d,  x9
vshift_gadget vshl_u_8b,  ushl, .8b,  w9
vshift_gadget vshl_u_16b, ushl, .16b, w9
vshift_gadget vshl_u_4h,  ushl, .4h,  w9
vshift_gadget vshl_u_8h,  ushl, .8h,  w9
vshift_gadget vshl_u_2s,  ushl, .2s,  w9
vshift_gadget vshl_u_4s,  ushl, .4s,  w9
vshift_gadget vshl_u_2d,  ushl, .2d,  x9

// SSHLL/USHLL (and the SXTL/UXTL #0 aliases): widen with sxtl/uxtl
// (fixed-immediate #0 forms), then apply the runtime shift with the
// register-form left shift on the widened arrangement.
// Code stream: [gadget][rd | rn<<8][amount]
.macro vshll_gadget name, xtl, srcarr, dstarr, dupw
.gadget \name
    ldr x8, [_ip]
    ldr x9, [_ip, #8]
    add _ip, _ip, #16
    and x10, x8, #0x1f
    ubfx x11, x8, #8, #5
    add x12, _cpu, #CPU_v0
    add x13, x12, x11, lsl #4
    ldr q30, [x13]
    \xtl v30\dstarr, v30\srcarr
    dup v29\dstarr, \dupw
    ushl v30\dstarr, v30\dstarr, v29\dstarr
    add x13, x12, x10, lsl #4
    str q30, [x13]
    gret
.endm

vshll_gadget vshll_s_8b,  sxtl,  .8b, .8h, w9
vshll_gadget vshll_s2_16b, sxtl2, .16b, .8h, w9
vshll_gadget vshll_s_4h,  sxtl,  .4h, .4s, w9
vshll_gadget vshll_s2_8h,  sxtl2, .8h, .4s, w9
vshll_gadget vshll_s_2s,  sxtl,  .2s, .2d, x9
vshll_gadget vshll_s2_4s,  sxtl2, .4s, .2d, x9
vshll_gadget vshll_u_8b,  uxtl,  .8b, .8h, w9
vshll_gadget vshll_u2_16b, uxtl2, .16b, .8h, w9
vshll_gadget vshll_u_4h,  uxtl,  .4h, .4s, w9
vshll_gadget vshll_u2_8h,  uxtl2, .8h, .4s, w9
vshll_gadget vshll_u_2s,  uxtl,  .2s, .2d, x9
vshll_gadget vshll_u2_4s,  uxtl2, .4s, .2d, x9

// ---- AdvSIMD LD1/ST1 (load/store multiple, contiguous) ------------------
// The interleaving LD2/3/4 forms are NOT handled here (different
// semantics); gen only routes the contiguous LD1/ST1 opcodes (1-4 regs)
// to this gadget. All the page-crossing work is in the C helper
// arm64_vldst_multi (emu/tlb.c); this gadget just marshals args, handles
// the fault return, and does the post-index writeback.
//
// Code stream: [gadget][packed][orig_ip]
//   packed: rt(0:4) | rn(5:9) | count(10:12) | q(13) | is_load(14) |
//           mode(15:16) | rm(17:21)
//   mode: 0 = no writeback, 1 = post-index by total bytes (imm form),
//         2 = post-index by Xrm (register form)
.extern arm64_vldst_multi

.gadget ld1st1_multi
    ldr x8, [_ip]
    ldr x19, [_ip, #8]         // orig_ip (fault restart), callee-saved
    add _ip, _ip, #16
    and x20, x8, #0x1f         // rt
    ubfx x21, x8, #5, #5       // rn
    ubfx x22, x8, #10, #3      // count
    ubfx x23, x8, #13, #1      // q
    ubfx x24, x8, #14, #1      // is_load
    ubfx x25, x8, #15, #2      // mode
    ubfx x26, x8, #17, #5      // rm

    // base address (rn: 31 = SP), into x7
    add x9, _cpu, #CPU_x0
    cmp x21, #31
    b.eq 1f
    add x10, x9, x21, lsl #3
    ldr x7, [x10]
    b 2f
1:  ldr x7, [_cpu, #CPU_sp]
2:
    // regbytes = q ? 16 : 8
    mov x11, #8
    cbz x23, 3f
    mov x11, #16
3:
    save_c                     // saves x0-x18+lr; x7/x11 spill+restore too
    // AAPCS64 args. Set the non-conflicting ones first (from callee-saved
    // sources), then chain x0<-x1(cpu), x1<-x2(tlb), x2<-x7(addr): x1 and
    // x2 are both source and destination, so this exact order is the only
    // safe one (same hazard as the crosspage helpers).
    mov x3, x20                // rt
    mov x4, x22                // count
    mov x5, x11                // regbytes
    mov x6, x24                // is_load
    mov x0, x1                 // cpu  (x1 still intact)
    sub x1, x2, #TLB_entries   // real `struct tlb *` (_tlb points at tlb->entries)
    mov x2, x7                 // addr
    bl NAME(arm64_vldst_multi)
    mov x27, x0                // stash result (INT_NONE=0 / INT_PF) in callee-saved
    restore_c

    cbnz x27, ld1st1_fault

    // writeback
    cbz x25, ld1st1_done
    add x9, _cpu, #CPU_x0
    cmp x21, #31
    b.eq 4f
    add x10, x9, x21, lsl #3
    ldr x13, [x10]
    b 5f
4:  ldr x13, [_cpu, #CPU_sp]
5:  cmp x25, #2
    b.eq 6f
    mul x14, x22, x11          // mode 1: += count * regbytes
    add x13, x13, x14
    b 7f
6:  cmp x26, #31              // mode 2: += Xrm (rm=31 -> +0, defensive)
    b.eq 7f
    add x10, x9, x26, lsl #3
    ldr x14, [x10]
    add x13, x13, x14
7:  cmp x21, #31
    b.eq 8f
    add x10, x9, x21, lsl #3
    str x13, [x10]
    b ld1st1_done
8:  str x13, [_cpu, #CPU_sp]
ld1st1_done:
    gret

ld1st1_fault:
    str x19, [_cpu, #CPU_pc]   // rewind PC to this instruction (idempotent retry)
    mov w0, w27                // INT_PF
    b jit_exit

// AdvSIMD structured load/store: interleaved LD2-4/ST2-4 (multiple),
// single-lane LD1-4/ST1-4, and LD1R-LD4R replicate — all through the
// crosspage-safe arm64_vldst_struct C helper (emu/tlb.c). Same shape as
// ld1st1_multi above. Code stream:
//   [gadget][rt | rn<<5 | count<<10 | q<<13 | is_load<<14 | mode<<15 |
//            rm<<17 | kind<<22 | esize_log2<<24 | lane<<26][orig_ip]
.gadget ldst_struct
    ldr x8, [_ip]
    ldr x19, [_ip, #8]         // orig_ip (fault restart), callee-saved
    add _ip, _ip, #16
    and x20, x8, #0x1f         // rt
    ubfx x21, x8, #5, #5       // rn
    ubfx x22, x8, #10, #3      // count
    ubfx x23, x8, #13, #1      // q
    ubfx x24, x8, #14, #1      // is_load
    ubfx x25, x8, #15, #2      // mode
    ubfx x26, x8, #17, #5      // rm
    ubfx x12, x8, #22, #2      // kind
    ubfx x13, x8, #24, #2      // esize_log2
    ubfx x14, x8, #26, #4      // lane

    // base address (rn: 31 = SP), into x7
    add x9, _cpu, #CPU_x0
    cmp x21, #31
    b.eq 1f
    add x10, x9, x21, lsl #3
    ldr x7, [x10]
    b 2f
1:  ldr x7, [_cpu, #CPU_sp]
2:
    // spec = count | esize_log2<<4 | q<<6 | lane<<8 | kind<<12 | is_load<<14
    orr x11, x22, x13, lsl #4
    orr x11, x11, x23, lsl #6
    orr x11, x11, x14, lsl #8
    orr x11, x11, x12, lsl #12
    orr x11, x11, x24, lsl #14
    save_c
    mov x3, x20                // rt
    mov x4, x11                // spec
    mov x0, x1                 // cpu
    sub x1, x2, #TLB_entries   // struct tlb *
    mov x2, x7                 // addr
    bl NAME(arm64_vldst_struct)
    mov x27, x0                // result (0 / INT_PF), callee-saved
    restore_c

    cbnz x27, ldst_struct_fault

    // writeback
    cbz x25, ldst_struct_done
    // recompute total bytes (x22/x23/x12/x13 are callee-saved and intact)
    cmp x12, #1
    b.ne 7f
    mov x15, #8
    cbz x23, 8f
    mov x15, #16
8:  mul x15, x22, x15
    b 9f
7:  mov x15, #1
    lsl x15, x15, x13
    mul x15, x22, x15
9:
    add x9, _cpu, #CPU_x0
    cmp x21, #31
    b.eq 10f
    add x10, x9, x21, lsl #3
    ldr x16, [x10]
    b 11f
10: ldr x16, [_cpu, #CPU_sp]
11: cmp x25, #2
    b.eq 12f
    add x16, x16, x15          // mode 1: += total bytes
    b 13f
12: cmp x26, #31               // mode 2: += Xrm
    b.eq 13f
    add x10, x9, x26, lsl #3
    ldr x15, [x10]
    add x16, x16, x15
13: cmp x21, #31
    b.eq 14f
    add x10, x9, x21, lsl #3
    str x16, [x10]
    b ldst_struct_done
14: str x16, [_cpu, #CPU_sp]
ldst_struct_done:
    gret

ldst_struct_fault:
    str x19, [_cpu, #CPU_pc]
    mov w0, w27
    b jit_exit
