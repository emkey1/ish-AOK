// AArch64 guest AdvSIMD gadgets: two-register misc (integer + FP),
// across-lanes reductions, three-different (widening/narrowing), permute
// (UZP/TRN/ZIP), and their scalar forms. Ported from OpenMinis/ish-arm64
// gadgets-aarch64/math.S (make_twomis_native, make_int_vec_cmp_zero,
// make_fp_vec_two_reg_misc, make_fp_vec_cmp_zero, make_fp_vec_unary,
// make_across_*, make_narrow_vec, make_pairwise_addl/accl,
// make_widening_long, make_byte_vec_unary; GPLv3, see
// /CREDITS-aarch64.md), collapsed to parameterized macros. Coverage gaps
// in OpenMinis filled here: FCVTN/FCVTL, SQDMLAL/SQDMLSL/SQDMULL,
// SQABS/SQNEG, SUQADD/USQADD, REV16, URECPE/URSQRTE, FCVTXN, scalar
// pairwise FP reductions beyond FADDP, scalar SQXTN family.
//
// Q=0 arrangements zero the upper 64 bits of Vd on the host op, so a full
// `str q0` store is architecturally correct without masking. The "2"
// variants (XTN2, SADDL2, ADDHN2, ...) read or preserve half of Vd, so
// those load Vd first and let the host op do the merge.
//
// Code stream (one-source): [gadget][rd | rn<<8 | size<<16 | q<<18]
// Code stream (two-source): [gadget][rd | rn<<8 | rm<<16 | size<<24 | q<<26]

#include "gadgets.h"

// PMULL/PMULL2's 1Q form is in the AES/crypto extension, which Xcode's
// default -arch arm64 assembler target doesn't advertise (every host we
// run on has it). Enable it explicitly for this file.
.arch_extension aes

.text

// ---- two-reg misc, integer: Vd = op(Vn), per-size validity ---------------
.macro two_misc_int name, op, bv=1, hv=1, sv=1, dv=0
.gadget \name
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f          // rd
    ubfx x10, x8, #8, #5       // rn
    ubfx x12, x8, #16, #2      // size
    ubfx x13, x8, #18, #1      // q
    add x14, _cpu, #CPU_v0
    add x15, x14, x10, lsl #4
    ldr q0, [x15]
.if \bv
    cmp x12, #0
    b.eq \name\()_b
.endif
.if \hv
    cmp x12, #1
    b.eq \name\()_h
.endif
.if \sv
    cmp x12, #2
    b.eq \name\()_s
.endif
.if \dv
    \op v0.2d, v0.2d           // gen only emits size 3 with Q=1
.endif
    b \name\()_st
.if \bv
\name\()_b:
    cbz x13, 1f
    \op v0.16b, v0.16b
    b \name\()_st
1:  \op v0.8b, v0.8b
    b \name\()_st
.endif
.if \hv
\name\()_h:
    cbz x13, 2f
    \op v0.8h, v0.8h
    b \name\()_st
2:  \op v0.4h, v0.4h
    b \name\()_st
.endif
.if \sv
\name\()_s:
    cbz x13, 3f
    \op v0.4s, v0.4s
    b \name\()_st
3:  \op v0.2s, v0.2s
.endif
\name\()_st:
    add x15, x14, x9, lsl #4
    str q0, [x15]
    gret
.endm

// Compare-with-zero needs a "#0" suffix operand.
.macro two_misc_zero name, op
.gadget \name
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    ubfx x12, x8, #16, #2
    ubfx x13, x8, #18, #1
    add x14, _cpu, #CPU_v0
    add x15, x14, x10, lsl #4
    ldr q0, [x15]
    cmp x12, #0
    b.eq \name\()_b
    cmp x12, #1
    b.eq \name\()_h
    cmp x12, #2
    b.eq \name\()_s
    \op v0.2d, v0.2d, #0
    b \name\()_st
\name\()_b:
    cbz x13, 1f
    \op v0.16b, v0.16b, #0
    b \name\()_st
1:  \op v0.8b, v0.8b, #0
    b \name\()_st
\name\()_h:
    cbz x13, 2f
    \op v0.8h, v0.8h, #0
    b \name\()_st
2:  \op v0.4h, v0.4h, #0
    b \name\()_st
\name\()_s:
    cbz x13, 3f
    \op v0.4s, v0.4s, #0
    b \name\()_st
3:  \op v0.2s, v0.2s, #0
\name\()_st:
    add x15, x14, x9, lsl #4
    str q0, [x15]
    gret
.endm

two_misc_int vabs,   abs,   1,1,1,1
two_misc_int vneg,   neg,   1,1,1,1
two_misc_int vsqabs, sqabs, 1,1,1,1
two_misc_int vsqneg, sqneg, 1,1,1,1
two_misc_int vcls,   cls,   1,1,1,0
two_misc_int vclz,   clz,   1,1,1,0
two_misc_int vrev64, rev64, 1,1,1,0
two_misc_int vrev32, rev32, 1,1,0,0
two_misc_int vrev16, rev16, 1,0,0,0
two_misc_int vcnt,   cnt,   1,0,0,0
two_misc_int vnot,   not,   1,0,0,0
two_misc_int vrbit,  rbit,  1,0,0,0

two_misc_zero vcmeq0, cmeq
two_misc_zero vcmgt0, cmgt
two_misc_zero vcmge0, cmge
two_misc_zero vcmle0, cmle
two_misc_zero vcmlt0, cmlt

// SUQADD/USQADD: Vd = sat(Vd + Vn) — accumulating two-misc.
.macro two_misc_acc name, op
.gadget \name
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    ubfx x12, x8, #16, #2
    ubfx x13, x8, #18, #1
    add x14, _cpu, #CPU_v0
    add x15, x14, x9, lsl #4
    ldr q0, [x15]              // Vd (accumulator)
    add x16, x14, x10, lsl #4
    ldr q1, [x16]
    cmp x12, #0
    b.eq \name\()_b
    cmp x12, #1
    b.eq \name\()_h
    cmp x12, #2
    b.eq \name\()_s
    \op v0.2d, v1.2d
    b \name\()_st
\name\()_b:
    cbz x13, 1f
    \op v0.16b, v1.16b
    b \name\()_st
1:  \op v0.8b, v1.8b
    b \name\()_st
\name\()_h:
    cbz x13, 2f
    \op v0.8h, v1.8h
    b \name\()_st
2:  \op v0.4h, v1.4h
    b \name\()_st
\name\()_s:
    cbz x13, 3f
    \op v0.4s, v1.4s
    b \name\()_st
3:  \op v0.2s, v1.2s
\name\()_st:
    // Q=0 host ops zero v0's upper 64 bits, matching the architecture.
    str q0, [x15]
    gret
.endm

two_misc_acc vsuqadd, suqadd
two_misc_acc vusqadd, usqadd

// ---- narrowing (XTN/SQXTN/UQXTN/SQXTUN): size names both widths ---------
// The Q=1 "2" variant inserts into Vd's upper half, preserving the lower
// half, so Vd is loaded first for both variants (harmless for Q=0).
.macro narrow_vec name, op
.gadget \name
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    ubfx x12, x8, #16, #2      // size (of the NARROW elements)
    ubfx x13, x8, #18, #1      // q: 0=\op, 1=\op\()2
    add x14, _cpu, #CPU_v0
    add x15, x14, x9, lsl #4
    ldr q0, [x15]              // Vd
    add x16, x14, x10, lsl #4
    ldr q1, [x16]              // Vn
    cmp x12, #0
    b.eq \name\()_b
    cmp x12, #1
    b.eq \name\()_h
    cbz x13, 3f
    \op\()2 v0.4s, v1.2d
    b \name\()_st
3:  \op v0.2s, v1.2d
    b \name\()_st
\name\()_b:
    cbz x13, 1f
    \op\()2 v0.16b, v1.8h
    b \name\()_st
1:  \op v0.8b, v1.8h
    b \name\()_st
\name\()_h:
    cbz x13, 2f
    \op\()2 v0.8h, v1.4s
    b \name\()_st
2:  \op v0.4h, v1.4s
\name\()_st:
    str q0, [x15]
    gret
.endm

narrow_vec vxtn,    xtn
narrow_vec vsqxtn,  sqxtn
narrow_vec vuqxtn,  uqxtn
narrow_vec vsqxtun, sqxtun

// SHLL/SHLL2: widen + left shift by exactly the source element size.
.gadget vshll
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    ubfx x12, x8, #16, #2
    ubfx x13, x8, #18, #1
    add x14, _cpu, #CPU_v0
    add x15, x14, x10, lsl #4
    ldr q0, [x15]
    cmp x12, #0
    b.eq 1f
    cmp x12, #1
    b.eq 2f
    cbz x13, 3f
    shll2 v0.2d, v0.4s, #32
    b 9f
3:  shll v0.2d, v0.2s, #32
    b 9f
1:  cbz x13, 4f
    shll2 v0.8h, v0.16b, #8
    b 9f
4:  shll v0.8h, v0.8b, #8
    b 9f
2:  cbz x13, 5f
    shll2 v0.4s, v0.8h, #16
    b 9f
5:  shll v0.4s, v0.4h, #16
9:  add x15, x14, x9, lsl #4
    str q0, [x15]
    gret

// Pairwise add-long: dest has half the lanes at double width.
.macro pairwise_addl name, op, acc=0
.gadget \name
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    ubfx x12, x8, #16, #2
    ubfx x13, x8, #18, #1
    add x14, _cpu, #CPU_v0
    add x15, x14, x9, lsl #4
.if \acc
    ldr q0, [x15]              // Vd (accumulator for SADALP/UADALP)
.endif
    add x16, x14, x10, lsl #4
    ldr q1, [x16]
    cmp x12, #0
    b.eq \name\()_b
    cmp x12, #1
    b.eq \name\()_h
    cbz x13, 3f
    \op v0.2d, v1.4s
    b \name\()_st
3:  \op v0.1d, v1.2s
    b \name\()_st
\name\()_b:
    cbz x13, 1f
    \op v0.8h, v1.16b
    b \name\()_st
1:  \op v0.4h, v1.8b
    b \name\()_st
\name\()_h:
    cbz x13, 2f
    \op v0.4s, v1.8h
    b \name\()_st
2:  \op v0.2s, v1.4h
\name\()_st:
    str q0, [x15]
    gret
.endm

pairwise_addl vsaddlp, saddlp
pairwise_addl vuaddlp, uaddlp
pairwise_addl vsadalp, sadalp, 1
pairwise_addl vuadalp, uadalp, 1

// URECPE/URSQRTE (32-bit lanes only).
.gadget vurecpe
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    ubfx x13, x8, #18, #1
    add x14, _cpu, #CPU_v0
    add x15, x14, x10, lsl #4
    ldr q0, [x15]
    cbz x13, 1f
    urecpe v0.4s, v0.4s
    b 2f
1:  urecpe v0.2s, v0.2s
2:  add x15, x14, x9, lsl #4
    str q0, [x15]
    gret

.gadget vursqrte
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    ubfx x13, x8, #18, #1
    add x14, _cpu, #CPU_v0
    add x15, x14, x10, lsl #4
    ldr q0, [x15]
    cbz x13, 1f
    ursqrte v0.4s, v0.4s
    b 2f
1:  ursqrte v0.2s, v0.2s
2:  add x15, x14, x9, lsl #4
    str q0, [x15]
    gret

// ---- two-reg misc, FP: sz selects .2s/.4s/.2d (sz=1 requires Q=1) --------
// Code stream: [gadget][rd | rn<<8 | sz<<16 | q<<18]
.macro two_misc_fp name, op
.gadget \name
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    ubfx x12, x8, #16, #1      // sz
    ubfx x13, x8, #18, #1      // q
    add x14, _cpu, #CPU_v0
    add x15, x14, x10, lsl #4
    ldr q0, [x15]
    cbnz x12, 1f
    cbz x13, 2f
    \op v0.4s, v0.4s
    b 3f
2:  \op v0.2s, v0.2s
    b 3f
1:  \op v0.2d, v0.2d
3:  add x15, x14, x9, lsl #4
    str q0, [x15]
    gret
.endm

.macro two_misc_fp_zero name, op
.gadget \name
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    ubfx x12, x8, #16, #1
    ubfx x13, x8, #18, #1
    add x14, _cpu, #CPU_v0
    add x15, x14, x10, lsl #4
    ldr q0, [x15]
    cbnz x12, 1f
    cbz x13, 2f
    \op v0.4s, v0.4s, #0.0
    b 3f
2:  \op v0.2s, v0.2s, #0.0
    b 3f
1:  \op v0.2d, v0.2d, #0.0
3:  add x15, x14, x9, lsl #4
    str q0, [x15]
    gret
.endm

two_misc_fp vfabs,    fabs
two_misc_fp vfneg,    fneg
two_misc_fp vfsqrt,   fsqrt
two_misc_fp vfrecpe,  frecpe
two_misc_fp vfrsqrte, frsqrte
two_misc_fp vscvtf,   scvtf
two_misc_fp vucvtf,   ucvtf
two_misc_fp vfcvtzs,  fcvtzs
two_misc_fp vfcvtzu,  fcvtzu
two_misc_fp vfcvtns,  fcvtns
two_misc_fp vfcvtnu,  fcvtnu
two_misc_fp vfcvtms,  fcvtms
two_misc_fp vfcvtmu,  fcvtmu
two_misc_fp vfcvtps,  fcvtps
two_misc_fp vfcvtpu,  fcvtpu
two_misc_fp vfcvtas,  fcvtas
two_misc_fp vfcvtau,  fcvtau
two_misc_fp vfrintn,  frintn
two_misc_fp vfrinta,  frinta
two_misc_fp vfrintp,  frintp
two_misc_fp vfrintm,  frintm
two_misc_fp vfrintx,  frintx
two_misc_fp vfrintz,  frintz
two_misc_fp vfrinti,  frinti

two_misc_fp_zero vfcmeq0, fcmeq
two_misc_fp_zero vfcmge0, fcmge
two_misc_fp_zero vfcmgt0, fcmgt
two_misc_fp_zero vfcmle0, fcmle
two_misc_fp_zero vfcmlt0, fcmlt

// FCVTN/FCVTN2 (double -> single, narrow) and FCVTXN/FCVTXN2 (round-to-
// odd variant); q picks the 2-form. Absent from OpenMinis.
.macro fcvt_narrow name, op
.gadget \name
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    ubfx x13, x8, #18, #1
    add x14, _cpu, #CPU_v0
    add x15, x14, x9, lsl #4
    ldr q0, [x15]              // Vd (2-form preserves the low half)
    add x16, x14, x10, lsl #4
    ldr q1, [x16]
    cbz x13, 1f
    \op\()2 v0.4s, v1.2d
    b 2f
1:  \op v0.2s, v1.2d
2:  str q0, [x15]
    gret
.endm

fcvt_narrow vfcvtn,  fcvtn
fcvt_narrow vfcvtxn, fcvtxn

// FCVTL/FCVTL2 (single -> double, lengthen); q picks the high half source.
.gadget vfcvtl
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    ubfx x13, x8, #18, #1
    add x14, _cpu, #CPU_v0
    add x15, x14, x10, lsl #4
    ldr q0, [x15]
    cbz x13, 1f
    fcvtl2 v0.2d, v0.4s
    b 2f
1:  fcvtl v0.2d, v0.2s
2:  add x15, x14, x9, lsl #4
    str q0, [x15]
    gret

// ---- across-lanes reductions ---------------------------------------------
// Result is a scalar in Vd (upper bits zeroed by the host op).
// Code stream: [gadget][rd | rn<<8 | size<<16 | q<<18]
.macro across_int name, op, dest_b, dest_h, dest_s
.gadget \name
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    ubfx x12, x8, #16, #2
    ubfx x13, x8, #18, #1
    add x14, _cpu, #CPU_v0
    add x15, x14, x10, lsl #4
    ldr q0, [x15]
    cmp x12, #0
    b.eq \name\()_b
    cmp x12, #1
    b.eq \name\()_h
    // size 2: only the 4s form exists (2s is reserved for reductions
    // except a couple of pairwise aliases the decoder rejects anyway)
    \op \dest_s\()0, v0.4s
    b \name\()_st
\name\()_b:
    cbz x13, 1f
    \op \dest_b\()0, v0.16b
    b \name\()_st
1:  \op \dest_b\()0, v0.8b
    b \name\()_st
\name\()_h:
    cbz x13, 2f
    \op \dest_h\()0, v0.8h
    b \name\()_st
2:  \op \dest_h\()0, v0.4h
\name\()_st:
    add x15, x14, x9, lsl #4
    str q0, [x15]
    gret
.endm

// same-width reductions (ADDV, xMAXV/xMINV): dest element = source element
across_int vaddv,  addv,  b, h, s
across_int vsmaxv, smaxv, b, h, s
across_int vsminv, sminv, b, h, s
across_int vumaxv, umaxv, b, h, s
across_int vuminv, uminv, b, h, s
// widening reductions: dest is double-width
across_int vsaddlv, saddlv, h, s, d
across_int vuaddlv, uaddlv, h, s, d

// FP across-lanes: only the .4s form exists in AArch64.
.macro across_fp name, op
.gadget \name
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    add x14, _cpu, #CPU_v0
    add x15, x14, x10, lsl #4
    ldr q0, [x15]
    \op s0, v0.4s
    add x15, x14, x9, lsl #4
    str q0, [x15]
    gret
.endm

across_fp vfmaxv,   fmaxv
across_fp vfminv,   fminv
across_fp vfmaxnmv, fmaxnmv
across_fp vfminnmv, fminnmv

// ---- permute: UZP1/UZP2/TRN1/TRN2/ZIP1/ZIP2 ------------------------------
// Plain three-same-shaped operand flow; all sizes, .2d needs Q=1.
.macro permute_vec name, op
.gadget \name
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    ubfx x11, x8, #16, #5
    ubfx x12, x8, #24, #2
    ubfx x13, x8, #26, #1
    add x14, _cpu, #CPU_v0
    add x15, x14, x10, lsl #4
    ldr q0, [x15]
    add x16, x14, x11, lsl #4
    ldr q1, [x16]
    cmp x12, #0
    b.eq \name\()_b
    cmp x12, #1
    b.eq \name\()_h
    cmp x12, #2
    b.eq \name\()_s
    \op v0.2d, v0.2d, v1.2d
    b \name\()_st
\name\()_b:
    cbz x13, 1f
    \op v0.16b, v0.16b, v1.16b
    b \name\()_st
1:  \op v0.8b, v0.8b, v1.8b
    b \name\()_st
\name\()_h:
    cbz x13, 2f
    \op v0.8h, v0.8h, v1.8h
    b \name\()_st
2:  \op v0.4h, v0.4h, v1.4h
    b \name\()_st
\name\()_s:
    cbz x13, 3f
    \op v0.4s, v0.4s, v1.4s
    b \name\()_st
3:  \op v0.2s, v0.2s, v1.2s
\name\()_st:
    add x15, x14, x9, lsl #4
    str q0, [x15]
    gret
.endm

permute_vec vuzp1, uzp1
permute_vec vuzp2, uzp2
permute_vec vtrn1, trn1
permute_vec vtrn2, trn2
permute_vec vzip1, zip1
permute_vec vzip2, zip2

// ---- three-different: widening/narrowing two-source ops ------------------
// L-forms (SADDL &c): dest double-width; Q picks which source half, spelled
// by the "2" mnemonic. Code stream: [gadget][rd|rn<<8|rm<<16|size<<24|q<<26]
.macro three_diff_l name, op, acc=0
.gadget \name
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    ubfx x11, x8, #16, #5
    ubfx x12, x8, #24, #2
    ubfx x13, x8, #26, #1
    add x14, _cpu, #CPU_v0
    add x15, x14, x9, lsl #4
.if \acc
    ldr q0, [x15]              // Vd (accumulator: xMLAL/xMLSL/xABAL)
.endif
    add x16, x14, x10, lsl #4
    ldr q1, [x16]
    add x16, x14, x11, lsl #4
    ldr q2, [x16]
    cmp x12, #0
    b.eq \name\()_b
    cmp x12, #1
    b.eq \name\()_h
    cbz x13, 3f
    \op\()2 v0.2d, v1.4s, v2.4s
    b \name\()_st
3:  \op v0.2d, v1.2s, v2.2s
    b \name\()_st
\name\()_b:
    cbz x13, 1f
    \op\()2 v0.8h, v1.16b, v2.16b
    b \name\()_st
1:  \op v0.8h, v1.8b, v2.8b
    b \name\()_st
\name\()_h:
    cbz x13, 2f
    \op\()2 v0.4s, v1.8h, v2.8h
    b \name\()_st
2:  \op v0.4s, v1.4h, v2.4h
\name\()_st:
    str q0, [x15]
    gret
.endm

three_diff_l vsaddl, saddl
three_diff_l vuaddl, uaddl
three_diff_l vssubl, ssubl
three_diff_l vusubl, usubl
three_diff_l vsabdl, sabdl
three_diff_l vuabdl, uabdl
three_diff_l vsmull_d, smull
three_diff_l vumull_d, umull
three_diff_l vsabal, sabal, 1
three_diff_l vuabal, uabal, 1
three_diff_l vsmlal, smlal, 1
three_diff_l vumlal, umlal, 1
three_diff_l vsmlsl, smlsl, 1
three_diff_l vumlsl, umlsl, 1

// SQDMULL/SQDMLAL/SQDMLSL: h/s sources only (no byte form). Absent from
// OpenMinis' three-different table.
.macro three_diff_l_hs name, op, acc=0
.gadget \name
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    ubfx x11, x8, #16, #5
    ubfx x12, x8, #24, #2
    ubfx x13, x8, #26, #1
    add x14, _cpu, #CPU_v0
    add x15, x14, x9, lsl #4
.if \acc
    ldr q0, [x15]
.endif
    add x16, x14, x10, lsl #4
    ldr q1, [x16]
    add x16, x14, x11, lsl #4
    ldr q2, [x16]
    cmp x12, #1
    b.eq \name\()_h
    cbz x13, 3f
    \op\()2 v0.2d, v1.4s, v2.4s
    b \name\()_st
3:  \op v0.2d, v1.2s, v2.2s
    b \name\()_st
\name\()_h:
    cbz x13, 2f
    \op\()2 v0.4s, v1.8h, v2.8h
    b \name\()_st
2:  \op v0.4s, v1.4h, v2.4h
\name\()_st:
    str q0, [x15]
    gret
.endm

three_diff_l_hs vsqdmull, sqdmull
three_diff_l_hs vsqdmlal, sqdmlal, 1
three_diff_l_hs vsqdmlsl, sqdmlsl, 1

// W-forms (SADDW &c): Vd = Vn(wide) op widen(Vm).
.macro three_diff_w name, op
.gadget \name
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    ubfx x11, x8, #16, #5
    ubfx x12, x8, #24, #2
    ubfx x13, x8, #26, #1
    add x14, _cpu, #CPU_v0
    add x15, x14, x10, lsl #4
    ldr q0, [x15]              // Vn (already wide)
    add x16, x14, x11, lsl #4
    ldr q1, [x16]              // Vm (narrow)
    cmp x12, #0
    b.eq \name\()_b
    cmp x12, #1
    b.eq \name\()_h
    cbz x13, 3f
    \op\()2 v0.2d, v0.2d, v1.4s
    b \name\()_st
3:  \op v0.2d, v0.2d, v1.2s
    b \name\()_st
\name\()_b:
    cbz x13, 1f
    \op\()2 v0.8h, v0.8h, v1.16b
    b \name\()_st
1:  \op v0.8h, v0.8h, v1.8b
    b \name\()_st
\name\()_h:
    cbz x13, 2f
    \op\()2 v0.4s, v0.4s, v1.8h
    b \name\()_st
2:  \op v0.4s, v0.4s, v1.4h
\name\()_st:
    add x15, x14, x9, lsl #4
    str q0, [x15]
    gret
.endm

three_diff_w vsaddw, saddw
three_diff_w vuaddw, uaddw
three_diff_w vssubw, ssubw
three_diff_w vusubw, usubw

// Narrow-high forms (ADDHN &c): Vd(narrow) = high_half(Vn op Vm); the "2"
// variant writes Vd's upper half preserving the lower, so Vd is loaded.
.macro three_diff_n name, op
.gadget \name
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    ubfx x11, x8, #16, #5
    ubfx x12, x8, #24, #2
    ubfx x13, x8, #26, #1
    add x14, _cpu, #CPU_v0
    add x15, x14, x9, lsl #4
    ldr q0, [x15]              // Vd
    add x16, x14, x10, lsl #4
    ldr q1, [x16]
    add x16, x14, x11, lsl #4
    ldr q2, [x16]
    cmp x12, #0
    b.eq \name\()_b
    cmp x12, #1
    b.eq \name\()_h
    cbz x13, 3f
    \op\()2 v0.4s, v1.2d, v2.2d
    b \name\()_st
3:  \op v0.2s, v1.2d, v2.2d
    b \name\()_st
\name\()_b:
    cbz x13, 1f
    \op\()2 v0.16b, v1.8h, v2.8h
    b \name\()_st
1:  \op v0.8b, v1.8h, v2.8h
    b \name\()_st
\name\()_h:
    cbz x13, 2f
    \op\()2 v0.8h, v1.4s, v2.4s
    b \name\()_st
2:  \op v0.4h, v1.4s, v2.4s
\name\()_st:
    str q0, [x15]
    gret
.endm

three_diff_n vaddhn,  addhn
three_diff_n vraddhn, raddhn
three_diff_n vsubhn,  subhn
three_diff_n vrsubhn, rsubhn

// PMULL/PMULL2: polynomial widening multiply. size=0 -> 8H from 8B/16B;
// size=3 -> 1Q from 1D/2D (FEAT_PMULL, present on all Apple silicon).
.gadget vpmull
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    ubfx x11, x8, #16, #5
    ubfx x12, x8, #24, #2
    ubfx x13, x8, #26, #1
    add x14, _cpu, #CPU_v0
    add x15, x14, x10, lsl #4
    ldr q0, [x15]
    add x16, x14, x11, lsl #4
    ldr q1, [x16]
    cmp x12, #0
    b.eq 1f
    cbz x13, 2f
    pmull2 v0.1q, v0.2d, v1.2d
    b 9f
2:  pmull v0.1q, v0.1d, v1.1d
    b 9f
1:  cbz x13, 3f
    pmull2 v0.8h, v0.16b, v1.16b
    b 9f
3:  pmull v0.8h, v0.8b, v1.8b
9:  add x15, x14, x9, lsl #4
    str q0, [x15]
    gret

// ---- scalar two-reg misc --------------------------------------------------
// Code stream: [gadget][rd | rn<<8 | size<<16]
.macro scalar_misc_d name, op
.gadget \name
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    add x14, _cpu, #CPU_v0
    add x15, x14, x10, lsl #4
    ldr d0, [x15]
    \op d0, d0
    add x15, x14, x9, lsl #4
    str q0, [x15]
    gret
.endm

scalar_misc_d sabs_d, abs
scalar_misc_d sneg_d, neg

.macro scalar_misc_zero name, op
.gadget \name
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    add x14, _cpu, #CPU_v0
    add x15, x14, x10, lsl #4
    ldr d0, [x15]
    \op d0, d0, #0
    add x15, x14, x9, lsl #4
    str q0, [x15]
    gret
.endm

scalar_misc_zero scmeq0, cmeq
scalar_misc_zero scmgt0, cmgt
scalar_misc_zero scmge0, cmge
scalar_misc_zero scmle0, cmle
scalar_misc_zero scmlt0, cmlt

// SQABS/SQNEG scalar: all element sizes.
.macro scalar_misc_sat name, op
.gadget \name
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    ubfx x12, x8, #16, #2
    add x14, _cpu, #CPU_v0
    add x15, x14, x10, lsl #4
    ldr q0, [x15]
    cmp x12, #0
    b.eq \name\()_b
    cmp x12, #1
    b.eq \name\()_h
    cmp x12, #2
    b.eq \name\()_s
    \op d0, d0
    b \name\()_st
\name\()_b:
    \op b0, b0
    b \name\()_st
\name\()_h:
    \op h0, h0
    b \name\()_st
\name\()_s:
    \op s0, s0
\name\()_st:
    add x15, x14, x9, lsl #4
    str q0, [x15]
    gret
.endm

scalar_misc_sat ssqabs, sqabs
scalar_misc_sat ssqneg, sqneg

// Scalar SUQADD/USQADD (accumulating, all sizes).
.macro scalar_misc_sat_acc name, op
.gadget \name
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    ubfx x12, x8, #16, #2
    add x14, _cpu, #CPU_v0
    add x15, x14, x9, lsl #4
    ldr q0, [x15]              // Vd
    add x16, x14, x10, lsl #4
    ldr q1, [x16]
    cmp x12, #0
    b.eq \name\()_b
    cmp x12, #1
    b.eq \name\()_h
    cmp x12, #2
    b.eq \name\()_s
    \op d0, d1
    b \name\()_st
\name\()_b:
    \op b0, b1
    b \name\()_st
\name\()_h:
    \op h0, h1
    b \name\()_st
\name\()_s:
    \op s0, s1
\name\()_st:
    str q0, [x15]
    gret
.endm

scalar_misc_sat_acc ssuqadd, suqadd
scalar_misc_sat_acc susqadd, usqadd

// Scalar saturating narrows: SQXTN/UQXTN/SQXTUN (b/h/s results).
.macro scalar_misc_narrow name, op
.gadget \name
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    ubfx x12, x8, #16, #2      // size of the NARROW result
    add x14, _cpu, #CPU_v0
    add x15, x14, x10, lsl #4
    ldr q0, [x15]
    cmp x12, #0
    b.eq \name\()_b
    cmp x12, #1
    b.eq \name\()_h
    \op s0, d0
    b \name\()_st
\name\()_b:
    \op b0, h0
    b \name\()_st
\name\()_h:
    \op h0, s0
\name\()_st:
    add x15, x14, x9, lsl #4
    str q0, [x15]
    gret
.endm

scalar_misc_narrow ssqxtn,  sqxtn
scalar_misc_narrow suqxtn,  uqxtn
scalar_misc_narrow ssqxtun, sqxtun

// Scalar FP two-reg misc: FRECPE/FRSQRTE/FRECPX (cc1 startup hits FRECPE),
// the vector-encoded scalar conversions, and FCVTXN.
// Code stream: [gadget][rd | rn<<8 | sz<<16]
.macro scalar_misc_fp name, op
.gadget \name
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    ubfx x12, x8, #16, #1
    add x14, _cpu, #CPU_v0
    add x15, x14, x10, lsl #4
    ldr q0, [x15]
    cbnz x12, 1f
    \op s0, s0
    b 2f
1:  \op d0, d0
2:  add x15, x14, x9, lsl #4
    str q0, [x15]
    gret
.endm

scalar_misc_fp sfrecpe,  frecpe
scalar_misc_fp sfrsqrte, frsqrte
scalar_misc_fp sfrecpx,  frecpx
scalar_misc_fp sscvtf,   scvtf
scalar_misc_fp sucvtf,   ucvtf
scalar_misc_fp sfcvtzs,  fcvtzs
scalar_misc_fp sfcvtzu,  fcvtzu
scalar_misc_fp sfcvtns,  fcvtns
scalar_misc_fp sfcvtnu,  fcvtnu
scalar_misc_fp sfcvtms,  fcvtms
scalar_misc_fp sfcvtmu,  fcvtmu
scalar_misc_fp sfcvtps,  fcvtps
scalar_misc_fp sfcvtpu,  fcvtpu
scalar_misc_fp sfcvtas,  fcvtas
scalar_misc_fp sfcvtau,  fcvtau

.gadget sfcvtxn
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    add x14, _cpu, #CPU_v0
    add x15, x14, x10, lsl #4
    ldr d0, [x15]
    fcvtxn s0, d0
    add x15, x14, x9, lsl #4
    str q0, [x15]
    gret

// Scalar FP compare with zero: [gadget][rd | rn<<8 | sz<<16]
.macro scalar_misc_fp_zero name, op
.gadget \name
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    ubfx x12, x8, #16, #1
    add x14, _cpu, #CPU_v0
    add x15, x14, x10, lsl #4
    ldr q0, [x15]
    cbnz x12, 1f
    \op s0, s0, #0.0
    b 2f
1:  \op d0, d0, #0.0
2:  add x15, x14, x9, lsl #4
    str q0, [x15]
    gret
.endm

scalar_misc_fp_zero sfcmeq0, fcmeq
scalar_misc_fp_zero sfcmge0, fcmge
scalar_misc_fp_zero sfcmgt0, fcmgt
scalar_misc_fp_zero sfcmle0, fcmle
scalar_misc_fp_zero sfcmlt0, fcmlt

// Scalar FP pairwise reductions: FADDP/FMAXP/FMINP/FMAXNMP/FMINNMP
// (Sd from Vn.2S / Dd from Vn.2D). [gadget][rd | rn<<8 | sz<<16]
.macro scalar_fp_pairwise name, op
.gadget \name
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    ubfx x12, x8, #16, #1
    add x14, _cpu, #CPU_v0
    add x15, x14, x10, lsl #4
    ldr q0, [x15]
    cbnz x12, 1f
    \op s0, v0.2s
    b 2f
1:  \op d0, v0.2d
2:  add x15, x14, x9, lsl #4
    str q0, [x15]
    gret
.endm

scalar_fp_pairwise sfaddp,   faddp
scalar_fp_pairwise sfmaxp,   fmaxp
scalar_fp_pairwise sfminp,   fminp
scalar_fp_pairwise sfmaxnmp, fmaxnmp
scalar_fp_pairwise sfminnmp, fminnmp

// ---- copy group: INS (element), DUP (element) ----------------------------
// The lane indexes are immediates in the host ISA, so these work through
// the V-register file in memory instead: byte-exact element copies.
// INS Vd.Ts[i1], Vn.Ts[i2]. [gadget][rd | rn<<8 | esize_log2<<16 |
// i1<<18 (4b) | i2<<22 (4b)]
.gadget ins_elem
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    ubfx x12, x8, #16, #2      // esize_log2
    ubfx x13, x8, #18, #4      // i1 (dest lane)
    ubfx x14, x8, #22, #4      // i2 (source lane)
    add x15, _cpu, #CPU_v0
    lsl x14, x14, x12          // i2 byte offset
    lsl x13, x13, x12          // i1 byte offset
    add x16, x15, x10, lsl #4
    add x16, x16, x14          // &Vn.b[i2 << esize_log2]
    add x15, x15, x9, lsl #4
    add x15, x15, x13          // &Vd.b[i1 << esize_log2]
    cmp x12, #0
    b.eq 1f
    cmp x12, #1
    b.eq 2f
    cmp x12, #2
    b.eq 3f
    ldr x8, [x16]
    str x8, [x15]
    gret
1:  ldrb w8, [x16]
    strb w8, [x15]
    gret
2:  ldrh w8, [x16]
    strh w8, [x15]
    gret
3:  ldr w8, [x16]
    str w8, [x15]
    gret

// DUP Vd.T, Vn.Ts[i]: load the element into lane 0 of a scratch register,
// then host-dup from the constant index 0. [gadget][rd | rn<<8 |
// esize_log2<<16 | i<<18 (4b) | q<<22]
.gadget dup_elem
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    ubfx x12, x8, #16, #2
    ubfx x13, x8, #18, #4
    ubfx x14, x8, #22, #1      // q
    add x15, _cpu, #CPU_v0
    lsl x13, x13, x12          // lane byte offset
    add x16, x15, x10, lsl #4
    add x16, x16, x13          // &Vn element
    cmp x12, #0
    b.eq 1f
    cmp x12, #1
    b.eq 2f
    cmp x12, #2
    b.eq 3f
    ldr d0, [x16]              // esize 8: only .2d exists (q=1)
    dup v0.2d, v0.d[0]
    b 9f
1:  ldr b0, [x16]
    cbz x14, 4f
    dup v0.16b, v0.b[0]
    b 9f
4:  dup v0.8b, v0.b[0]
    b 9f
2:  ldr h0, [x16]
    cbz x14, 5f
    dup v0.8h, v0.h[0]
    b 9f
5:  dup v0.4h, v0.h[0]
    b 9f
3:  ldr s0, [x16]
    cbz x14, 6f
    dup v0.4s, v0.s[0]
    b 9f
6:  dup v0.2s, v0.s[0]
9:  add x15, x15, x9, lsl #4
    str q0, [x15]
    gret

// DUP <scalar>d, Vn.Ts[i] (a.k.a. MOV): element -> scalar, upper zeroed.
// [gadget][rd | rn<<8 | esize_log2<<16 | i<<18 (4b)]
.gadget dup_elem_scalar
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    ubfx x12, x8, #16, #2
    ubfx x13, x8, #18, #4
    add x15, _cpu, #CPU_v0
    lsl x13, x13, x12          // lane byte offset
    add x16, x15, x10, lsl #4
    add x16, x16, x13
    mov x8, #0
    cmp x12, #0
    b.eq 1f
    cmp x12, #1
    b.eq 2f
    cmp x12, #2
    b.eq 3f
    ldr x8, [x16]
    b 9f
1:  ldrb w8, [x16]
    b 9f
2:  ldrh w8, [x16]
    b 9f
3:  ldr w8, [x16]
9:  add x15, x15, x9, lsl #4
    str x8, [x15]
    str xzr, [x15, #8]
    gret

// EXT Vd.16B, Vn.16B, Vm.16B, #imm: concatenate Vn:Vm in the frame's
// LOCAL_value scratch (the first 32 bytes of it) and reload 16 (or 8) bytes
// at the runtime byte offset.
// [gadget][rd | rn<<8 | rm<<16 | imm<<24 (4b) | q<<28]
.gadget ext
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    ubfx x11, x8, #16, #5
    ubfx x12, x8, #24, #4      // imm (byte offset)
    ubfx x13, x8, #28, #1      // q
    add x14, _cpu, #CPU_v0
    add x15, x14, x10, lsl #4
    add x16, x14, x11, lsl #4
    mov x17, LOCAL_value
    add x17, _cpu, x17
    cbz x13, 1f
    ldr q0, [x15]
    ldr q1, [x16]
    stp q0, q1, [x17]
    ldr q0, [x17, x12]
    b 9f
1:  // 8-byte form: concat low halves, take 8 bytes at imm, zero upper
    ldr d0, [x15]
    ldr d1, [x16]
    stp d0, d1, [x17]
    ldr d0, [x17, x12]
9:  add x15, x14, x9, lsl #4
    str q0, [x15]
    gret

// TBL/TBX Vd.T, {Vn..Vn+len-1}.16B, Vm.T — host register lists are fixed
// names, so branch on len; the list regs load into v1-v4, indexes into
// v5. TBX preserves out-of-range dest bytes (dest preloaded).
// [gadget][rd | rn<<8 | rm<<16 | len<<24 (2b: regs-1) | q<<26]
.macro tbl_gadget name, op, loads_dest
.gadget \name
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    ubfx x11, x8, #16, #5
    ubfx x12, x8, #24, #2      // len-1
    ubfx x13, x8, #26, #1      // q
    add x14, _cpu, #CPU_v0
.if \loads_dest
    add x15, x14, x9, lsl #4
    ldr q0, [x15]
.endif
    add x16, x14, x11, lsl #4
    ldr q5, [x16]              // indexes
    // load v1..v(len), consecutive mod 32
    add x16, x14, x10, lsl #4
    ldr q1, [x16]
    cbz x12, \name\()_go1
    add x10, x10, #1
    and x10, x10, #31
    add x16, x14, x10, lsl #4
    ldr q2, [x16]
    cmp x12, #1
    b.eq \name\()_go2
    add x10, x10, #1
    and x10, x10, #31
    add x16, x14, x10, lsl #4
    ldr q3, [x16]
    cmp x12, #2
    b.eq \name\()_go3
    add x10, x10, #1
    and x10, x10, #31
    add x16, x14, x10, lsl #4
    ldr q4, [x16]
    cbz x13, 4f
    \op v0.16b, {v1.16b, v2.16b, v3.16b, v4.16b}, v5.16b
    b \name\()_st
4:  \op v0.8b, {v1.16b, v2.16b, v3.16b, v4.16b}, v5.8b
    b \name\()_st
\name\()_go1:
    cbz x13, 1f
    \op v0.16b, {v1.16b}, v5.16b
    b \name\()_st
1:  \op v0.8b, {v1.16b}, v5.8b
    b \name\()_st
\name\()_go2:
    cbz x13, 2f
    \op v0.16b, {v1.16b, v2.16b}, v5.16b
    b \name\()_st
2:  \op v0.8b, {v1.16b, v2.16b}, v5.8b
    b \name\()_st
\name\()_go3:
    cbz x13, 3f
    \op v0.16b, {v1.16b, v2.16b, v3.16b}, v5.16b
    b \name\()_st
3:  \op v0.8b, {v1.16b, v2.16b, v3.16b}, v5.8b
\name\()_st:
    add x15, x14, x9, lsl #4
    str q0, [x15]
    gret
.endm

tbl_gadget vtbl, tbl, 0
tbl_gadget vtbx, tbx, 1

// Scalar integer ADDP: Dd = Vn.2D[0] + Vn.2D[1]. [gadget][rd | rn<<8]
.gadget saddp_d
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    add x14, _cpu, #CPU_v0
    add x15, x14, x10, lsl #4
    ldr q0, [x15]
    addp d0, v0.2d
    add x15, x14, x9, lsl #4
    str q0, [x15]
    gret

# vim: ft=gas
