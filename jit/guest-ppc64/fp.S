// ppc64 guest floating point, VSX and VMX gadgets.
//
// An FPR is its VSR's high doubleword; the streams carry that slot's offset
// (an FP operand) or the whole VSR's (a vector one). FPSCR follows
// emu/fpenv.c: RN is the host FPCR's rounding mode and NI its FZ, and the
// exceptions gather in the host FPSR, to be folded into FPSCR when it is
// read (fpscr_take) or the guest leaves the JIT. FPRF is not kept by the
// arithmetic, which nothing in a Linux userland reads; the compares keep
// FPCC.

#include "gadgets.h"

.text

#define FPSCR_FX     0x80000000
#define FPSCR_FEX    0x40000000
#define FPSCR_VX     0x20000000
#define FPSCR_VXALL  0x01f80700

// ---- arithmetic: [frt][fra][frb] ----------------------------------------------
.macro f3_in
    ldp x9, x10, [_ip]
    ldr x11, [_ip, #16]
    ldr d0, [_cpu, x10]
    ldr d1, [_cpu, x11]
.endm
// With NaN operands POWER gives the first of them, A before B, quieted; the
// host prefers a signalling NaN wherever it is. nan_first \r, \x: \r is
// \x quieted if \x is a NaN (the add raises nothing a NaN operand of the op
// did not already).
.macro nan_first r, x
    fcmp \x, \x
    b.vc 9f
    fadd \r, \x, \x
9:
.endm
.macro f3 name, op, nanfix=1
.gadget \name
    f3_in
    \op d2, d0, d1
    .if \nanfix
    nan_first d2, d0
    .endif
    str d2, [_cpu, x9]
    gret 3
.endm
// The single-precision ops: in double and rounded to single, which for +,
// -, *, / and sqrt of singles is the correctly rounded single (53 >= 2*24+2).
.macro f3s name, op
.gadget \name
    f3_in
    \op d2, d0, d1
    nan_first d2, d0
    fcvt s2, d2
    fcvt d2, s2
    str d2, [_cpu, x9]
    gret 3
.endm
f3 fadd, fadd
f3 fsub, fsub
f3 fmul, fmul
f3 fdiv, fdiv
f3s fadds, fadd
f3s fsubs, fsub
f3s fmuls, fmul
f3s fdivs, fdiv
// xsmaxdp, xsmindp: a QNaN operand gives the other, as fmaxnm
f3 xsmaxdp, fmaxnm, 0
f3 xsmindp, fminnm, 0

// [frt][frb]
.macro f1 name, op, single=0
.gadget \name
    ldp x9, x10, [_ip]
    ldr d0, [_cpu, x10]
    \op
    .if \single
    fcvt s0, d0
    fcvt d0, s0
    .endif
    str d0, [_cpu, x9]
    gret 2
.endm
.macro op_fsqrt
    fsqrt d0, d0
.endm
// fre, frsqrte: an estimate may be the exact value
.macro op_fre
    fmov d1, #1.0
    fdiv d0, d1, d0
.endm
.macro op_frsqrte
    fsqrt d0, d0
    fmov d1, #1.0
    fdiv d0, d1, d0
.endm
.macro op_frsp
    fcvt s0, d0
    fcvt d0, s0
.endm
.macro op_frin
    frinta d0, d0
.endm
.macro op_friz
    frintz d0, d0
.endm
.macro op_frip
    frintp d0, d0
.endm
.macro op_frim
    frintm d0, d0
.endm
.macro op_fric
    frinti d0, d0
.endm
.macro op_fcfid
    fmov x12, d0
    scvtf d0, x12
.endm
.macro op_fcfidu
    fmov x12, d0
    ucvtf d0, x12
.endm
.macro op_fcfids
    fmov x12, d0
    scvtf s0, x12
    fcvt d0, s0
.endm
.macro op_fcfidus
    fmov x12, d0
    ucvtf s0, x12
    fcvt d0, s0
.endm
// The conversions to integers saturate as the host's do; a NaN gives the
// most negative value (0 for an unsigned one, as the host gives too). A word
// result is sign-extended into the doubleword (the ISA leaves the high half
// undefined).
.macro nan_min reg, min
    fcmp d0, d0
    b.vc 1f
    mov \reg, \min
1:
.endm
.macro op_fctidz
    fcvtzs x12, d0
    nan_min x12, #0x8000000000000000
    fmov d0, x12
.endm
.macro op_fctid
    frinti d0, d0
    fcvtzs x12, d0
    nan_min x12, #0x8000000000000000
    fmov d0, x12
.endm
.macro op_fctiwz
    fcvtzs w12, d0
    nan_min w12, #0x80000000
    sxtw x12, w12
    fmov d0, x12
.endm
.macro op_fctiw
    frinti d0, d0
    fcvtzs w12, d0
    nan_min w12, #0x80000000
    sxtw x12, w12
    fmov d0, x12
.endm
.macro op_fctiduz
    fcvtzu x12, d0
    fmov d0, x12
.endm
.macro op_fctidu
    frinti d0, d0
    fcvtzu x12, d0
    fmov d0, x12
.endm
.macro op_fctiwuz
    fcvtzu w12, d0
    fmov d0, x12
.endm
.macro op_fctiwu
    frinti d0, d0
    fcvtzu w12, d0
    fmov d0, x12
.endm
f1 fsqrt, op_fsqrt
f1 fsqrts, op_fsqrt, 1
f1 fre, op_fre
f1 fres, op_fre, 1
f1 frsqrte, op_frsqrte
f1 frsqrtes, op_frsqrte, 1
f1 frsp, op_frsp
f1 frin, op_frin
f1 friz, op_friz
f1 frip, op_frip
f1 frim, op_frim
f1 fric, op_fric
f1 fcfid, op_fcfid
f1 fcfidu, op_fcfidu
f1 fcfids, op_fcfids
f1 fcfidus, op_fcfidus
f1 fctid, op_fctid
f1 fctidz, op_fctidz
f1 fctiw, op_fctiw
f1 fctiwz, op_fctiwz
f1 fctidu, op_fctidu
f1 fctiduz, op_fctiduz
f1 fctiwu, op_fctiwu
f1 fctiwuz, op_fctiwuz

// The sign ops, on the bits (no exception, a NaN's payload kept): [frt][frb]
.macro fbits name, op
.gadget \name
    ldp x9, x10, [_ip]
    ldr x10, [_cpu, x10]
    \op
    str x10, [_cpu, x9]
    gret 2
.endm
.macro op_fmr
.endm
.macro op_fneg
    eor x10, x10, #0x8000000000000000
.endm
.macro op_fabs
    and x10, x10, #0x7fffffffffffffff
.endm
.macro op_fnabs
    orr x10, x10, #0x8000000000000000
.endm
fbits fmr, op_fmr
fbits fneg, op_fneg
fbits fabs, op_fabs
fbits fnabs, op_fnabs
// fcpsgn frt, fra, frb: frb with fra's sign. [frt][fra][frb]
.gadget fcpsgn
    ldp x9, x10, [_ip]
    ldr x11, [_ip, #16]
    ldr x10, [_cpu, x10]
    ldr x11, [_cpu, x11]
    and x10, x10, #0x8000000000000000
    and x11, x11, #0x7fffffffffffffff
    orr x10, x10, x11
    str x10, [_cpu, x9]
    gret 3

// The multiply-adds: [frt][fra][frc][frb], frt = fra * frc +- frb. fnmadd and
// fnmsub negate the rounded result, but not a NaN, as POWER does (the
// host's fnmadd would round the negated value, which differs in the
// directed rounding modes).
.macro fma name, op, neg, single
.gadget \name
    ldp x9, x10, [_ip]
    ldp x11, x12, [_ip, #16]
    ldr d0, [_cpu, x10]
    ldr d1, [_cpu, x11]
    ldr d2, [_cpu, x12]
    \op d3, d0, d1, d2
    .if \neg
    fcmp d3, d3
    b.vs 1f
    fneg d3, d3
1:
    .endif
    // NaN operands: A's, then B's (the addend), then C's
    fcmp d1, d1
    b.vc 3f
    fadd d3, d1, d1
3:  fcmp d2, d2
    b.vc 4f
    fadd d3, d2, d2
4:  fcmp d0, d0
    b.vc 5f
    fadd d3, d0, d0
5:
    .if \single
    fcvt s3, d3
    fcvt d3, s3
    .endif
    str d3, [_cpu, x9]
    gret 4
.endm
// a*c + b, a*c - b
.macro op_madd d, a, c, b
    fmadd \d, \a, \c, \b
.endm
// (b negated first, unless it is a NaN: the host's fnmsub negates a NaN
// addend, and POWER passes a NaN through as it is)
.macro op_msub d, a, c, b
    fcmp \b, \b
    b.vs 2f
    fneg \b, \b
2:  fmadd \d, \a, \c, \b
.endm
fma fmadd, op_madd, 0, 0
fma fmsub, op_msub, 0, 0
fma fnmadd, op_madd, 1, 0
fma fnmsub, op_msub, 1, 0
fma fmadds, op_madd, 0, 1
fma fmsubs, op_msub, 0, 1
fma fnmadds, op_madd, 1, 1
fma fnmsubs, op_msub, 1, 1

// fsel frt, fra, frc, frb: frc if fra >= 0 (and not a NaN), else frb.
// [frt][fra][frc][frb]
.gadget fsel
    ldp x9, x10, [_ip]
    ldp x11, x12, [_ip, #16]
    ldr d0, [_cpu, x10]
    ldr d1, [_cpu, x11]
    ldr d2, [_cpu, x12]
    fcmp d0, #0.0
    fcsel d0, d1, d2, ge
    str d0, [_cpu, x9]
    gret 4

// fcmpu, fcmpo (and xscmpudp, xscmpodp): [crf][fra][frb]. The field is
// FL FG FE FU (8 4 2 1); FPSCR's FPCC gets the same.
.gadget fcmpu
    f3_in
    fcmp d0, d1
    mov w12, #8
    mov w13, #4
    csel w12, w12, w13, mi
    mov w13, #2
    csel w12, w13, w12, eq
    mov w13, #1
    csel w12, w13, w12, vs
    str w12, [_cpu, x9]
    ldr w13, [_cpu, #CPU_ppc64_fpscr]
    bfi w13, w12, #12, #4
    str w13, [_cpu, #CPU_ppc64_fpscr]
    gret 3

// ---- FPSCR -------------------------------------------------------------------
// The host's flags into FPSCR, and cleared (emu/fpenv.c ppc64_fold): w13 the
// FPSCR after.
.macro fpscr_take
    mrs x10, fpsr
    and x11, x10, #0x1f
    bic x10, x10, #0x1f
    bic x10, x10, #0x80
    msr fpsr, x10
    mov w12, #0
    tbz x11, #0, 1f             // IOC: VX, VXSOFT
    orr w12, w12, #0x20000000
    orr w12, w12, #0x400
1:  tbz x11, #1, 1f             // DZC: ZX
    orr w12, w12, #0x04000000
1:  tbz x11, #2, 1f             // OFC: OX
    orr w12, w12, #0x10000000
1:  tbz x11, #3, 1f             // UFC: UX
    orr w12, w12, #0x08000000
1:  tbz x11, #4, 1f             // IXC: XX
    orr w12, w12, #0x02000000
1:  ldr w13, [_cpu, #CPU_ppc64_fpscr]
    bics wzr, w12, w13          // new exceptions: FX
    b.eq 1f
    orr w12, w12, #FPSCR_FX
1:  orr w13, w13, w12
.endm
// w13 written to FPSCR as it is (VX and FEX summarised) and its RN and NI
// into the host's FPCR.
.macro fpscr_put
    mov w10, #0x0700
    movk w10, #0x01f8, lsl #16  // FPSCR_VXALL
    tst w13, w10
    bic w13, w13, #(FPSCR_VX | FPSCR_FEX)
    mov w10, #FPSCR_VX
    csel w10, w10, wzr, ne
    orr w13, w13, w10
    lsr w10, w13, #22           // VX OX UX ZX XX against VE OE UE ZE XE
    and w10, w10, w13
    tst w10, #0xf8
    mov w10, #FPSCR_FEX
    csel w10, w10, wzr, ne
    orr w13, w13, w10
    str w13, [_cpu, #CPU_ppc64_fpscr]
    and w10, w13, #3            // RN 0 1 2 3: FPCR.RMode 0 3 1 2
    lsl w10, w10, #2
    mov w11, #0x2130
    lsr w11, w11, w10
    and w11, w11, #3
    mrs x10, fpcr
    bfi x10, x11, #22, #2
    ubfx w11, w13, #2, #1       // NI: FZ
    bfi x10, x11, #24, #1
    msr fpcr, x10
.endm

// mffs frt: [frt]
.gadget mffs
    fpscr_take
    str w13, [_cpu, #CPU_ppc64_fpscr]
    ldr x9, [_ip]
    mov w13, w13
    str x13, [_cpu, x9]
    gret 1
// mtfsf FLM, frb (and L, W): [frb][the FPSCR bits it writes]
.gadget mtfsf
    fpscr_take
    ldp x9, x10, [_ip]
    ldr x9, [_cpu, x9]
    bic w13, w13, w10
    and w9, w9, w10
    orr w13, w13, w9
    fpscr_put
    gret 2
// mtfsfi, mtfsb0, mtfsb1: [the bits cleared][the bits set]
.gadget mtfsfi
    fpscr_take
    ldp x9, x10, [_ip]
    bic w13, w13, w9
    orr w13, w13, w10
    fpscr_put
    gret 2
// mcrfs BF, BFA: [crf][field shift]; the field is copied and its exception
// bits cleared (the summaries are recomputed)
.gadget mcrfs
    fpscr_take
    ldp x9, x10, [_ip]
    lsr w11, w13, w10
    and w11, w11, #0xf
    str w11, [_cpu, x9]
    mov w12, #0xf
    lsl w12, w12, w10
    mov w14, #0x0700            // only exception bits are cleared
    movk w14, #0x9ff8, lsl #16
    and w12, w12, w14
    bic w13, w13, w12
    fpscr_put
    gret 2
// A floating-point record form: CR1 = FX FEX VX OX
.gadget cr1
    fpscr_take
    str w13, [_cpu, #CPU_ppc64_fpscr]
    lsr w13, w13, #28
    mov x9, #CPU_ppc64_cr
    add x9, _cpu, x9
    str w13, [x9, #4]
    gret

// ---- moves between GPRs and VSRs ------------------------------------------------
// mtvsrd (mtfprd): [xt dw0][ra]; mtvsrwa, mtvsrwz: the word extended
.gadget mtvsrd
    ldp x9, x10, [_ip]
    ldr x10, [_cpu, x10]
    str x10, [_cpu, x9]
    gret 2
.gadget mtvsrwa
    ldp x9, x10, [_ip]
    ldrsw x10, [_cpu, x10]
    str x10, [_cpu, x9]
    gret 2
.gadget mtvsrwz
    ldp x9, x10, [_ip]
    ldr w10, [_cpu, x10]
    str x10, [_cpu, x9]
    gret 2
// mfvsrd: [ra][xs dw0]; mfvsrwz: the low word of doubleword 0
.gadget mfvsrwz
    ldp x9, x10, [_ip]
    ldr w10, [_cpu, x10]
    str x10, [_cpu, x9]
    gret 2

// ---- single <-> double without rounding or signalling (xscvdpspn, xscvspdpn)
// xscvdpspn xt, xb: the single in words 0 and 1. [xt][xb dw0]
.gadget xscvdpspn
    ldp x9, x10, [_ip]
    ldr x19, [_cpu, x10]
    ubfx x12, x19, #52, #11
    cmp w12, #0x7ff
    b.eq 1f
    fmov d0, x19
    fcvt s0, d0
    fmov w19, s0
    b 2f
1:  lsr x12, x19, #32
    and w12, w12, #0x80000000
    orr w12, w12, #0x7f800000
    ubfx x13, x19, #29, #23
    orr w19, w12, w13
2:  mov w19, w19
    orr x19, x19, x19, lsl #32
    str x19, [_cpu, x9]
    gret 2
// xscvdpsp: the same, rounded in the current mode (a signalling NaN quieted)
.gadget xscvdpsp
    ldp x9, x10, [_ip]
    ldr d0, [_cpu, x10]
    fcvt s0, d0
    fmov w11, s0
    orr x11, x11, x11, lsl #32
    str x11, [_cpu, x9]
    gret 2
// xscvspdpn, xscvspdp: word 0's single as a double. [xt dw0][xb dw0]
.gadget xscvspdpn
    ldp x9, x10, [_ip]
    add x10, x10, #4
    ldr w11, [_cpu, x10]
    ubfx w12, w11, #23, #8
    cmp w12, #0xff
    b.eq 1f
    fmov s0, w11
    fcvt d0, s0
    fmov x11, d0
    b 2f
1:  and x12, x11, #0x80000000
    lsl x12, x12, #32
    orr x12, x12, #0x7ff0000000000000
    ubfx x13, x11, #0, #23
    orr x11, x12, x13, lsl #29
2:  str x11, [_cpu, x9]
    gret 2
.gadget xscvspdp
    ldp x9, x10, [_ip]
    add x10, x10, #4
    ldr s0, [_cpu, x10]
    fcvt d0, s0
    str d0, [_cpu, x9]
    gret 2

// ---- 128-bit vector ops: [vt][va][vb] ----------------------------------------
.macro v3_in
    ldp x9, x10, [_ip]
    ldr x11, [_ip, #16]
    ldr q0, [_cpu, x10]
    ldr q1, [_cpu, x11]
.endm
.macro v3 name, insn, arr
.gadget \name
    v3_in
    \insn v0.\arr, v0.\arr, v1.\arr
    str q0, [_cpu, x9]
    gret 3
.endm
// the same with the operands swapped: vt = op(vb, va)
.macro v3r name, insn, arr
.gadget \name
    v3_in
    \insn v0.\arr, v1.\arr, v0.\arr
    str q0, [_cpu, x9]
    gret 3
.endm
v3 vand, and, 16b
v3 vor, orr, 16b
v3 vxor, eor, 16b
v3 vandc, bic, 16b
v3 vorc, orn, 16b
.gadget vnor
    v3_in
    orr v0.16b, v0.16b, v1.16b
    mvn v0.16b, v0.16b
    str q0, [_cpu, x9]
    gret 3
.gadget vnand
    v3_in
    and v0.16b, v0.16b, v1.16b
    mvn v0.16b, v0.16b
    str q0, [_cpu, x9]
    gret 3
.gadget veqv
    v3_in
    eor v0.16b, v0.16b, v1.16b
    mvn v0.16b, v0.16b
    str q0, [_cpu, x9]
    gret 3
v3 vaddubm, add, 16b
v3 vadduhm, add, 8h
v3 vadduwm, add, 4s
v3 vaddudm, add, 2d
v3 vsububm, sub, 16b
v3 vsubuhm, sub, 8h
v3 vsubuwm, sub, 4s
v3 vsubudm, sub, 2d
v3 vmaxub, umax, 16b
v3 vmaxuh, umax, 8h
v3 vmaxuw, umax, 4s
v3 vmaxsb, smax, 16b
v3 vmaxsh, smax, 8h
v3 vmaxsw, smax, 4s
v3 vminub, umin, 16b
v3 vminuh, umin, 8h
v3 vminuw, umin, 4s
v3 vminsb, smin, 16b
v3 vminsh, smin, 8h
v3 vminsw, smin, 4s
v3 vaddubs, uqadd, 16b
v3 vadduhs, uqadd, 8h
v3 vadduws, uqadd, 4s
v3 vaddsbs, sqadd, 16b
v3 vaddshs, sqadd, 8h
v3 vaddsws, sqadd, 4s
v3 vsububs, uqsub, 16b
v3 vsubuhs, uqsub, 8h
v3 vsubuws, uqsub, 4s
v3 vsubsbs, sqsub, 16b
v3 vsubshs, sqsub, 8h
v3 vsubsws, sqsub, 4s
v3 vavgub, urhadd, 16b
v3 vavguh, urhadd, 8h
v3 vavguw, urhadd, 4s
v3 vavgsb, srhadd, 16b
v3 vavgsh, srhadd, 8h
v3 vavgsw, srhadd, 4s
v3 vmuluwm, mul, 4s
v3 vcmpequb, cmeq, 16b
v3 vcmpequh, cmeq, 8h
v3 vcmpequw, cmeq, 4s
v3 vcmpequd, cmeq, 2d
v3 vcmpgtub, cmhi, 16b
v3 vcmpgtuh, cmhi, 8h
v3 vcmpgtuw, cmhi, 4s
v3 vcmpgtud, cmhi, 2d
v3 vcmpgtsb, cmgt, 16b
v3 vcmpgtsh, cmgt, 8h
v3 vcmpgtsw, cmgt, 4s
v3 vcmpgtsd, cmgt, 2d
// element shifts by the low bits of each element of vb
.macro vshift name, arr, bits, left, signed
.gadget \name
    v3_in
    movi v2.\arr, #(\bits - 1)
    and v1.16b, v1.16b, v2.16b
    .if !\left
    neg v1.\arr, v1.\arr
    .endif
    .if \signed
    sshl v0.\arr, v0.\arr, v1.\arr
    .else
    ushl v0.\arr, v0.\arr, v1.\arr
    .endif
    str q0, [_cpu, x9]
    gret 3
.endm
vshift vslb, 16b, 8, 1, 0
vshift vslh, 8h, 16, 1, 0
vshift vslw, 4s, 32, 1, 0
vshift vsrb, 16b, 8, 0, 0
vshift vsrh, 8h, 16, 0, 0
vshift vsrw, 4s, 32, 0, 0
vshift vsrab, 16b, 8, 0, 1
vshift vsrah, 8h, 16, 0, 1
vshift vsraw, 4s, 32, 0, 1
// (movi .2d cannot hold 63: the doubleword shifts load it)
.macro vshiftd name, left, signed
.gadget \name
    v3_in
    mov x12, #63
    dup v2.2d, x12
    and v1.16b, v1.16b, v2.16b
    .if !\left
    neg v1.2d, v1.2d
    .endif
    .if \signed
    sshl v0.2d, v0.2d, v1.2d
    .else
    ushl v0.2d, v0.2d, v1.2d
    .endif
    str q0, [_cpu, x9]
    gret 3
.endm
vshiftd vsld, 1, 0
vshiftd vsrd, 0, 0
vshiftd vsrad, 0, 1
// the rotates: left by each element's low bits
.macro vrot name, arr, bits
.gadget \name
    v3_in
    movi v2.\arr, #(\bits - 1)
    and v1.16b, v1.16b, v2.16b
    ushl v3.\arr, v0.\arr, v1.\arr
    movi v4.\arr, #\bits
    sub v4.\arr, v1.\arr, v4.\arr
    ushl v0.\arr, v0.\arr, v4.\arr
    orr v0.16b, v0.16b, v3.16b
    str q0, [_cpu, x9]
    gret 3
.endm
vrot vrlb, 16b, 8
vrot vrlh, 8h, 16
vrot vrlw, 4s, 32

// The merges. Big-endian element 0 is the host's last lane, so vmrgh's
// [a0 b0 a1 b1] is zip2(b, a) and vmrgl's zip1(b, a).
v3r vmrghb, zip2, 16b
v3r vmrghh, zip2, 8h
v3r vmrghw, zip2, 4s
v3r vmrglb, zip1, 16b
v3r vmrglh, zip1, 8h
v3r vmrglw, zip1, 4s
// xxmrghw, xxmrglw are vmrghw, vmrglw on any VSR

// The selects and permutes ----------------------------------------------------
// xxsel xt, xa, xb, xc: (xa & ~xc) | (xb & xc). [xt][xa][xb][xc]; vsel too
.gadget xxsel
    ldp x9, x10, [_ip]
    ldp x11, x12, [_ip, #16]
    ldr q0, [_cpu, x10]
    ldr q1, [_cpu, x11]
    ldr q2, [_cpu, x12]
    bsl v2.16b, v1.16b, v0.16b
    str q2, [_cpu, x9]
    gret 4
// vperm vt, va, vb, vc: big-endian byte i of vt is byte (vc[i] & 31) of va||vb.
// With the host's {vb, va} as tbl's table, that byte is index 31 - (c & 31).
// [vt][va][vb][vc]
.gadget vperm
    ldp x9, x10, [_ip]
    ldp x11, x12, [_ip, #16]
    ldr q1, [_cpu, x10]
    ldr q0, [_cpu, x11]
    ldr q2, [_cpu, x12]
    mvn v2.16b, v2.16b
    movi v3.16b, #31
    and v2.16b, v2.16b, v3.16b
    tbl v2.16b, {v0.16b, v1.16b}, v2.16b
    str q2, [_cpu, x9]
    gret 4
// xxpermdi and its aliases (xxswapd, xxspltd, xxmrghd, xxmrgld): doubleword 0
// from one slot, doubleword 1 from another, each a doubleword's offset.
// [xt][dw0 source][dw1 source]
.gadget xxpermdi
    ldp x9, x10, [_ip]
    ldr x11, [_ip, #16]
    ldr x10, [_cpu, x10]
    ldr x11, [_cpu, x11]
    add x9, x9, _cpu
    stp x11, x10, [x9]
    gret 3
// xxsldwi xt, xa, xb, shw (1-3; 0 is a copy) and vsldoi (bytes 1-15): bytes
// sh to sh + 15 of a||vb, the host's ext(vb, va, 16 - sh). [vt][va][vb]
.macro vsldoi_n n
.gadget vsldoi\n
    v3_in
    ext v0.16b, v1.16b, v0.16b, #(16 - \n)
    str q0, [_cpu, x9]
    gret 3
.endm
.irp n, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
vsldoi_n \n
.endr
// A copy of a VSR: [vt][vb]
.gadget vmov
    ldp x9, x10, [_ip]
    ldr q0, [_cpu, x10]
    str q0, [_cpu, x9]
    gret 2
// A constant (vspltis*, xxspltib, xxlxor of a register with itself):
// [vt][low][high]
.gadget vconst
    ldp x9, x10, [_ip]
    ldr x11, [_ip, #16]
    add x9, x9, _cpu
    stp x10, x11, [x9]
    gret 3
// The splats of one element: [vt][the element's offset]
.gadget vspltb
    ldp x9, x10, [_ip]
    ldr b0, [_cpu, x10]
    dup v0.16b, v0.b[0]
    str q0, [_cpu, x9]
    gret 2
.gadget vsplth
    ldp x9, x10, [_ip]
    ldr h0, [_cpu, x10]
    dup v0.8h, v0.h[0]
    str q0, [_cpu, x9]
    gret 2
.gadget vspltw
    ldp x9, x10, [_ip]
    ldr s0, [_cpu, x10]
    dup v0.4s, v0.s[0]
    str q0, [_cpu, x9]
    gret 2
// xvnegdp, xvabsdp, xvnabsdp, xvcpsgndp-free sign ops on both doublewords
.macro vsign name, op
.gadget \name
    ldp x9, x10, [_ip]
    ldr q0, [_cpu, x10]
    movi v1.2d, #0
    fneg v1.2d, v1.2d               // the sign bits
    \op v0.16b, v0.16b, v1.16b
    str q0, [_cpu, x9]
    gret 2
.endm
vsign xvnegdp, eor
vsign xvabsdp, bic
vsign xvnabsdp, orr
// A vector compare's record form: CR6 = 8 if every element compared true
// (all ones), 2 if none did. [vt]
.gadget cr6
    ldr x9, [_ip]
    ldr q0, [_cpu, x9]
    uminv b1, v0.16b
    umaxv b2, v0.16b
    fmov w10, s1
    fmov w11, s2
    cmp w10, #0xff
    mov w12, #8
    csel w12, w12, wzr, eq
    cmp w11, #0
    mov w13, #2
    csel w13, w13, wzr, eq
    orr w12, w12, w13
    mov x9, #CPU_ppc64_cr
    add x9, _cpu, x9
    str w12, [x9, #24]
    gret 1

// vgbbd vt, vb: each doubleword an 8x8 bit matrix, transposed (bit j of byte
// i to bit i of byte j). Big-endian numbering reverses both the bytes and the
// bits, so it is the same transpose of the host's doubleword. [vt][vb]
.macro transpose8 x, t, m1, m2, m3
    eor \t, \x, \x, lsr #7
    and \t, \t, \m1
    eor \x, \x, \t
    eor \x, \x, \t, lsl #7
    eor \t, \x, \x, lsr #14
    and \t, \t, \m2
    eor \x, \x, \t
    eor \x, \x, \t, lsl #14
    eor \t, \x, \x, lsr #28
    and \t, \t, \m3
    eor \x, \x, \t
    eor \x, \x, \t, lsl #28
.endm
.gadget vgbbd
    ldp x9, x10, [_ip]
    add x10, x10, _cpu
    ldp x11, x12, [x10]
    adr x13, ppc64_transpose_masks
    ldp x14, x15, [x13]
    ldr x16, [x13, #16]
    transpose8 x11, x13, x14, x15, x16
    transpose8 x12, x13, x14, x15, x16
    add x9, x9, _cpu
    stp x11, x12, [x9]
    gret 2
.balign 8
ppc64_transpose_masks:
    .quad 0x00aa00aa00aa00aa, 0x0000cccc0000cccc, 0x00000000f0f0f0f0

// vslo, vsro: the whole vector shifted left or right by octets, the count in
// bits 121:124 of vb (big-endian), its last byte's bits 3:6; vsl, vsr: by
// bits, the count in its last byte's low 3 bits. [vt][va][vb]
.gadget vslo
    v3_in
    umov w12, v1.b[0]
    ubfx w12, w12, #3, #4
    b ppc64_vshift_bytes_left
.gadget vsro
    v3_in
    umov w12, v1.b[0]
    ubfx w12, w12, #3, #4
    b ppc64_vshift_bytes_right
// v0 shifted by w12 bytes through a zero-padded copy on the stack, stored to [x9]
ppc64_vshift_bytes_left:
    movi v2.16b, #0
    stp q2, q0, [sp, #-32]!
    mov x13, sp
    sub x13, x13, x12
    ldr q0, [x13, #16]
    add sp, sp, #32
    str q0, [_cpu, x9]
    gret 3
ppc64_vshift_bytes_right:
    movi v2.16b, #0
    stp q0, q2, [sp, #-32]!
    mov x13, sp
    add x13, x13, x12
    ldr q0, [x13]
    add sp, sp, #32
    str q0, [_cpu, x9]
    gret 3
.gadget vsl
    v3_in
    umov w12, v1.b[0]
    and w12, w12, #7
    add x10, x10, _cpu
    ldp x10, x11, [x10]             // va as two halves (low, high)
    lsl x13, x11, x12
    neg w14, w12
    lsr x15, x10, x14
    cmp w12, #0
    csel x15, xzr, x15, eq
    orr x11, x13, x15
    lsl x10, x10, x12
    add x9, x9, _cpu
    stp x10, x11, [x9]
    gret 3
.gadget vsr
    v3_in
    umov w12, v1.b[0]
    and w12, w12, #7
    add x10, x10, _cpu
    ldp x10, x11, [x10]
    lsr x13, x10, x12
    neg w14, w12
    lsl x15, x11, x14
    cmp w12, #0
    csel x15, xzr, x15, eq
    orr x10, x13, x15
    lsr x11, x11, x12
    add x9, x9, _cpu
    stp x10, x11, [x9]
    gret 3

// mfvscr vt: VSCR in the last word, the rest zero. mtvscr vb. [v]
.gadget mfvscr
    ldr x9, [_ip]
    ldr w10, [_cpu, #CPU_ppc64_vscr]
    add x9, x9, _cpu
    stp x10, xzr, [x9]
    gret 1
.gadget mtvscr
    ldr x9, [_ip]
    ldr w10, [_cpu, x9]
    str w10, [_cpu, #CPU_ppc64_vscr]
    gret 1

// vbpermq vt, va, vb: bit i of a 16-bit mask is the bit of va that vb's byte
// i numbers (big-endian, 0 the most significant; 128 or more gives 0); the
// mask is doubleword 0's low 16 bits, and the rest is zero. Host bit 127 - n
// is big-endian bit n, and big-endian byte i of vb is host byte 15 - i.
// [vt][va][vb]
.gadget vbpermq
    ldp x9, x10, [_ip]
    ldr x11, [_ip, #16]
    add x10, x10, _cpu
    ldp x12, x13, [x10]             // va: low, high
    add x11, x11, _cpu              // vb's bytes
    mov x14, #0                     // the mask
    mov x15, #15                    // big-endian byte i = 15 - x15
1:  ldrb w16, [x11, x15]            // host byte x15: vb byte 15 - x15
    tbnz w16, #7, 2f                // 128 or more: 0
    mov w17, #127
    sub w17, w17, w16               // the host bit
    lsr x10, x12, x17
    subs w17, w17, #64
    lsr x8, x13, x17
    csel x10, x8, x10, ge
    and x10, x10, #1
    lsl x10, x10, x15               // mask bit 15 - i = host byte number
    orr x14, x14, x10
2:  subs x15, x15, #1
    b.ge 1b
    add x9, x9, _cpu
    stp xzr, x14, [x9]
    gret 3

// ---- VSX vector floating point ------------------------------------------------
// Lanes are lanes: an element-wise op is the host's on the whole register.
// [xt][xa][xb]
// \r's lanes where \x is a NaN become \x quieted (the first NaN operand is
// the result, as for the scalar ops' nan_first); \q holds the quiet bits.
.macro vquiet q, arr
    .ifc \arr,4s
    movi \q\().4s, #0x40, lsl #16
    .else
    mov x13, #0x0008000000000000
    dup \q\().2d, x13
    .endif
.endm
.macro vnan_first r, x, q, t1, t2, arr
    orr \t1\().16b, \x\().16b, \q\().16b
    fcmeq \t2\().\arr, \x\().\arr, \x\().\arr
    bif \r\().16b, \t1\().16b, \t2\().16b
.endm
.macro vf3 name, insn, arr, nanfix=1
.gadget \name
    v3_in
    \insn v2.\arr, v0.\arr, v1.\arr
    .if \nanfix
    vquiet v5, \arr
    vnan_first v2, v0, v5, v6, v7, \arr
    .endif
    str q2, [_cpu, x9]
    gret 3
.endm
vf3 xvadddp, fadd, 2d
vf3 xvaddsp, fadd, 4s
vf3 xvsubdp, fsub, 2d
vf3 xvsubsp, fsub, 4s
vf3 xvmuldp, fmul, 2d
vf3 xvmulsp, fmul, 4s
vf3 xvdivdp, fdiv, 2d
vf3 xvdivsp, fdiv, 4s
vf3 xvmaxdp, fmaxnm, 2d, 0
vf3 xvmaxsp, fmaxnm, 4s, 0
vf3 xvmindp, fminnm, 2d, 0
vf3 xvminsp, fminnm, 4s, 0
vf3 xvcmpeqdp, fcmeq, 2d, 0
vf3 xvcmpeqsp, fcmeq, 4s, 0
vf3 xvcmpgtdp, fcmgt, 2d, 0
vf3 xvcmpgtsp, fcmgt, 4s, 0
vf3 xvcmpgedp, fcmge, 2d, 0
vf3 xvcmpgesp, fcmge, 4s, 0
// xvcpsgn: xa's signs on xb's magnitudes
.macro vcpsgn name, arr
.gadget \name
    v3_in
    movi v2.2d, #0
    fneg v2.\arr, v2.\arr               // the sign bits
    bsl v2.16b, v0.16b, v1.16b
    str q2, [_cpu, x9]
    gret 3
.endm
vcpsgn xvcpsgndp, 2d
vcpsgn xvcpsgnsp, 4s

// \r negated where it is not a NaN: a negated multiply-add negates its
// addend or its result, never a NaN's sign (and -(a*b - c) is not c - a*b
// when they are equal: the zero's sign differs).
.macro vnn_neg r, t1, t2, arr
    fcmeq \t1\().\arr, \r\().\arr, \r\().\arr
    fneg \t2\().\arr, \r\().\arr
    bit \r\().16b, \t2\().16b, \t1\().16b
.endm

// The multiply-adds. A form: xt = xa * xb +- xt; M form: xt = xa * xt +- xb.
// msub is a*b - c, nmadd -(a*b + c), nmsub -(a*b - c). [xt][xa][xb]
.macro vfma name, form, kind, arr
.gadget \name
    v3_in
    ldr q2, [_cpu, x9]                  // xt
    .ifc \form,a
    mov v3.16b, v2.16b                  // the addend: xt
    mov v4.16b, v1.16b                  // the multiplier: xb
    .else
    mov v3.16b, v1.16b                  // the addend: xb
    mov v4.16b, v2.16b                  // the multiplier: xt
    .endif
    mov v7.16b, v3.16b
    .ifc \kind,madd
    fmla v3.\arr, v0.\arr, v4.\arr
    .endif
    .ifc \kind,nmadd
    fmla v3.\arr, v0.\arr, v4.\arr
    vnn_neg v3, v5, v6, \arr
    .endif
    .ifc \kind,msub
    vnn_neg v3, v5, v6, \arr
    fmla v3.\arr, v0.\arr, v4.\arr
    .endif
    .ifc \kind,nmsub
    vnn_neg v3, v5, v6, \arr
    fmla v3.\arr, v0.\arr, v4.\arr
    vnn_neg v3, v5, v6, \arr
    .endif
    // NaN operands: xa's, then the addend's, then the other multiplier's
    vquiet v16, \arr
    vnan_first v3, v4, v16, v5, v6, \arr
    vnan_first v3, v7, v16, v5, v6, \arr
    vnan_first v3, v0, v16, v5, v6, \arr
    str q3, [_cpu, x9]
    gret 3
.endm
.irp kind, madd, msub, nmadd, nmsub
vfma xv\kind\()adp, a, \kind, 2d
vfma xv\kind\()mdp, m, \kind, 2d
vfma xv\kind\()asp, a, \kind, 4s
vfma xv\kind\()msp, m, \kind, 4s
.endr

// One operand: [xt][xb]
.macro vf1 name, insn, arr
.gadget \name
    ldp x9, x10, [_ip]
    ldr q0, [_cpu, x10]
    \insn v0.\arr, v0.\arr
    str q0, [_cpu, x9]
    gret 2
.endm
vf1 xvsqrtdp, fsqrt, 2d
vf1 xvsqrtsp, fsqrt, 4s
vf1 xvabssp, fabs, 4s
vf1 xvnegsp, fneg, 4s
vf1 xvrdpi, frinta, 2d
vf1 xvrdpiz, frintz, 2d
vf1 xvrdpip, frintp, 2d
vf1 xvrdpim, frintm, 2d
vf1 xvrdpic, frinti, 2d
vf1 xvrspi, frinta, 4s
vf1 xvrspiz, frintz, 4s
vf1 xvrspip, frintp, 4s
vf1 xvrspim, frintm, 4s
vf1 xvrspic, frinti, 4s
vf1 xvcvsxwsp, scvtf, 4s
vf1 xvcvuxwsp, ucvtf, 4s
vf1 xvcvspuxws, fcvtzu, 4s
vf1 xvcvsxddp, scvtf, 2d
vf1 xvcvuxddp, ucvtf, 2d
vf1 xvcvdpuxds, fcvtzu, 2d
.gadget xvnabssp
    ldp x9, x10, [_ip]
    ldr q0, [_cpu, x10]
    fabs v0.4s, v0.4s
    fneg v0.4s, v0.4s
    str q0, [_cpu, x9]
    gret 2
// the estimates, exact
.macro vrecip name, arr, sqrt
.gadget \name
    ldp x9, x10, [_ip]
    ldr q0, [_cpu, x10]
    .if \sqrt
    fsqrt v0.\arr, v0.\arr
    .endif
    fmov v1.\arr, #1.0
    fdiv v0.\arr, v1.\arr, v0.\arr
    str q0, [_cpu, x9]
    gret 2
.endm
vrecip xvredp, 2d, 0
vrecip xvresp, 4s, 0
vrecip xvrsqrtedp, 2d, 1
vrecip xvrsqrtesp, 4s, 1
// Between words and doublewords: the words that pair with doublewords are
// big-endian words 0 and 2, the host's lanes 3 and 1.
// xvcvspdp, xvcvsxwdp, xvcvuxwdp: those words widened
.gadget xvcvspdp
    ldp x9, x10, [_ip]
    ldr q0, [_cpu, x10]
    uzp2 v0.4s, v0.4s, v0.4s
    fcvtl v0.2d, v0.2s
    str q0, [_cpu, x9]
    gret 2
.gadget xvcvsxwdp
    ldp x9, x10, [_ip]
    ldr q0, [_cpu, x10]
    uzp2 v0.4s, v0.4s, v0.4s
    sxtl v0.2d, v0.2s
    scvtf v0.2d, v0.2d
    str q0, [_cpu, x9]
    gret 2
.gadget xvcvuxwdp
    ldp x9, x10, [_ip]
    ldr q0, [_cpu, x10]
    uzp2 v0.4s, v0.4s, v0.4s
    uxtl v0.2d, v0.2s
    ucvtf v0.2d, v0.2d
    str q0, [_cpu, x9]
    gret 2
// xvcvdpsp, xvcvdpsxws: narrowed into those words (each also in the word
// beside it, which the ISA leaves undefined)
.gadget xvcvdpsp
    ldp x9, x10, [_ip]
    ldr q0, [_cpu, x10]
    fcvtn v0.2s, v0.2d
    zip1 v0.4s, v0.4s, v0.4s
    str q0, [_cpu, x9]
    gret 2
// The signed conversions to integers give a NaN the most negative value, as
// the scalar ones do (the host gives 0); the unsigned give it 0, as the host.
.macro nan_min_v v, t1, t2, arr
    movi \t2\().2d, #0
    fneg \t2\().\arr, \t2\().\arr       // the sign bit alone: the most negative
    bif \v\().16b, \t2\().16b, \t1\().16b
.endm
.macro vcvt_s name, arr
.gadget \name
    ldp x9, x10, [_ip]
    ldr q0, [_cpu, x10]
    fcmeq v1.\arr, v0.\arr, v0.\arr
    fcvtzs v0.\arr, v0.\arr
    nan_min_v v0, v1, v2, \arr
    str q0, [_cpu, x9]
    gret 2
.endm
vcvt_s xvcvspsxws, 4s
vcvt_s xvcvdpsxds, 2d
.gadget xvcvdpsxws
    ldp x9, x10, [_ip]
    ldr q0, [_cpu, x10]
    fcmeq v1.2d, v0.2d, v0.2d
    fcvtzs v0.2d, v0.2d
    nan_min_v v0, v1, v2, 2d
    sqxtn v0.2s, v0.2d
    zip1 v0.4s, v0.4s, v0.4s
    str q0, [_cpu, x9]
    gret 2
.gadget xvcvdpuxws
    ldp x9, x10, [_ip]
    ldr q0, [_cpu, x10]
    fcvtzu v0.2d, v0.2d
    uqxtn v0.2s, v0.2d
    zip1 v0.4s, v0.4s, v0.4s
    str q0, [_cpu, x9]
    gret 2
// xvcvspsxds, xvcvspuxds: the singles of words 0 and 2, as xvcvspdp takes
// them, to doublewords (exactly through double)
.gadget xvcvspsxds
    ldp x9, x10, [_ip]
    ldr q0, [_cpu, x10]
    uzp2 v0.4s, v0.4s, v0.4s
    fcvtl v0.2d, v0.2s
    fcmeq v1.2d, v0.2d, v0.2d
    fcvtzs v0.2d, v0.2d
    nan_min_v v0, v1, v2, 2d
    str q0, [_cpu, x9]
    gret 2
.gadget xvcvspuxds
    ldp x9, x10, [_ip]
    ldr q0, [_cpu, x10]
    uzp2 v0.4s, v0.4s, v0.4s
    fcvtl v0.2d, v0.2s
    fcvtzu v0.2d, v0.2d
    str q0, [_cpu, x9]
    gret 2
// xvcvsxdsp, xvcvuxdsp: doublewords to singles in words 0 and 2, each
// rounded once (through double it would be twice)
.macro vcvt_dsp name, cvt
.gadget \name
    ldp x9, x10, [_ip]
    ldr q0, [_cpu, x10]
    mov x11, v0.d[0]
    mov x12, v0.d[1]
    \cvt s1, x11
    \cvt s2, x12
    mov v1.s[1], v2.s[0]
    zip1 v0.4s, v1.4s, v1.4s
    str q0, [_cpu, x9]
    gret 2
.endm
vcvt_dsp xvcvsxdsp, scvtf
vcvt_dsp xvcvuxdsp, ucvtf

// ---- more VMX integer ops ---------------------------------------------------
// The even and odd multiplies: big-endian even elements are the host's odd
// lanes (uzp2), odd ones its even lanes (uzp1); the products widen into the
// element twice the size, in order. [vt][va][vb]
.macro vmul_eo name, uzp, arr, half, mul, wide
.gadget \name
    v3_in
    \uzp v0.\arr, v0.\arr, v0.\arr
    \uzp v1.\arr, v1.\arr, v1.\arr
    \mul v0.\wide, v0.\half, v1.\half
    str q0, [_cpu, x9]
    gret 3
.endm
vmul_eo vmuleub, uzp2, 16b, 8b, umull, 8h
vmul_eo vmuloub, uzp1, 16b, 8b, umull, 8h
vmul_eo vmulesb, uzp2, 16b, 8b, smull, 8h
vmul_eo vmulosb, uzp1, 16b, 8b, smull, 8h
vmul_eo vmuleuh, uzp2, 8h, 4h, umull, 4s
vmul_eo vmulouh, uzp1, 8h, 4h, umull, 4s
vmul_eo vmulesh, uzp2, 8h, 4h, smull, 4s
vmul_eo vmulosh, uzp1, 8h, 4h, smull, 4s
vmul_eo vmuleuw, uzp2, 4s, 2s, umull, 2d
vmul_eo vmulouw, uzp1, 4s, 2s, umull, 2d
vmul_eo vmulesw, uzp2, 4s, 2s, smull, 2d
vmul_eo vmulosw, uzp1, 4s, 2s, smull, 2d

// The packs: vt's big-endian first half from va, so the host's low half is
// vb narrowed and its high half va. (VSCR[SAT] is not kept.)
.macro vpack name, op, lo, hi, wide
.gadget \name
    v3_in
    \op v2.\lo, v1.\wide
    \op\()2 v2.\hi, v0.\wide
    str q2, [_cpu, x9]
    gret 3
.endm
vpack vpkuhum, xtn, 8b, 16b, 8h
vpack vpkuwum, xtn, 4h, 8h, 4s
vpack vpkudum, xtn, 2s, 4s, 2d
vpack vpkshss, sqxtn, 8b, 16b, 8h
vpack vpkswss, sqxtn, 4h, 8h, 4s
vpack vpksdss, sqxtn, 2s, 4s, 2d
vpack vpkshus, sqxtun, 8b, 16b, 8h
vpack vpkswus, sqxtun, 4h, 8h, 4s
vpack vpksdus, sqxtun, 2s, 4s, 2d
vpack vpkuhus, uqxtn, 8b, 16b, 8h
vpack vpkuwus, uqxtn, 4h, 8h, 4s
vpack vpkudus, uqxtn, 2s, 4s, 2d

// The unpacks of vb: the big-endian high half is the host's high half
// (sxtl2), the low the low (sxtl). [vt][vb]
.macro vunpack name, op, wide, narrow
.gadget \name
    ldp x9, x10, [_ip]
    ldr q1, [_cpu, x10]
    \op v0.\wide, v1.\narrow
    str q0, [_cpu, x9]
    gret 2
.endm
vunpack vupkhsb, sxtl2, 8h, 16b
vunpack vupklsb, sxtl, 8h, 8b
vunpack vupkhsh, sxtl2, 4s, 8h
vunpack vupklsh, sxtl, 4s, 4h
vunpack vupkhsw, sxtl2, 2d, 4s
vunpack vupklsw, sxtl, 2d, 2s

// vpopcnt[bhwd], vclz[bhw] of vb. [vt][vb]
.gadget vpopcntb
    ldp x9, x10, [_ip]
    ldr q0, [_cpu, x10]
    cnt v0.16b, v0.16b
    str q0, [_cpu, x9]
    gret 2
.gadget vpopcnth
    ldp x9, x10, [_ip]
    ldr q0, [_cpu, x10]
    cnt v0.16b, v0.16b
    uaddlp v0.8h, v0.16b
    str q0, [_cpu, x9]
    gret 2
.gadget vpopcntw
    ldp x9, x10, [_ip]
    ldr q0, [_cpu, x10]
    cnt v0.16b, v0.16b
    uaddlp v0.8h, v0.16b
    uaddlp v0.4s, v0.8h
    str q0, [_cpu, x9]
    gret 2
.gadget vpopcntd
    ldp x9, x10, [_ip]
    ldr q0, [_cpu, x10]
    cnt v0.16b, v0.16b
    uaddlp v0.8h, v0.16b
    uaddlp v0.4s, v0.8h
    uaddlp v0.2d, v0.4s
    str q0, [_cpu, x9]
    gret 2
vf1 vclzb, clz, 16b
vf1 vclzh, clz, 8h
vf1 vclzw, clz, 4s
.gadget vclzd
    ldp x9, x10, [_ip]
    add x10, x10, _cpu
    ldp x11, x12, [x10]
    clz x11, x11
    clz x12, x12
    add x9, x9, _cpu
    stp x11, x12, [x9]
    gret 2

// vadduqm, vsubuqm: the 128-bit sum and difference
.gadget vadduqm
    ldp x9, x10, [_ip]
    ldr x11, [_ip, #16]
    add x10, x10, _cpu
    add x11, x11, _cpu
    ldp x12, x13, [x10]
    ldp x14, x15, [x11]
    adds x12, x12, x14
    adc x13, x13, x15
    add x9, x9, _cpu
    stp x12, x13, [x9]
    gret 3
.gadget vsubuqm
    ldp x9, x10, [_ip]
    ldr x11, [_ip, #16]
    add x10, x10, _cpu
    add x11, x11, _cpu
    ldp x12, x13, [x10]
    ldp x14, x15, [x11]
    subs x12, x12, x14
    sbc x13, x13, x15
    add x9, x9, _cpu
    stp x12, x13, [x9]
    gret 3
// vaddcuw, vsubcuw: each word's carry out (no-borrow for the subtract)
.gadget vaddcuw
    v3_in
    add v2.4s, v0.4s, v1.4s
    cmhi v2.4s, v0.4s, v2.4s
    ushr v2.4s, v2.4s, #31
    str q2, [_cpu, x9]
    gret 3
.gadget vsubcuw
    v3_in
    cmhs v2.4s, v0.4s, v1.4s
    ushr v2.4s, v2.4s, #31
    str q2, [_cpu, x9]
    gret 3
// vmrgew [a0 b0 a2 b2], vmrgow [a1 b1 a3 b3]: trn2(b, a), trn1(b, a)
v3r vmrgew, trn2, 4s
v3r vmrgow, trn1, 4s
// the doubleword max and min, which NEON has no instruction for
.macro vminmaxd name, cmp, first
.gadget \name
    v3_in
    .if \first
    \cmp v2.2d, v0.2d, v1.2d
    .else
    \cmp v2.2d, v1.2d, v0.2d
    .endif
    bsl v2.16b, v0.16b, v1.16b
    str q2, [_cpu, x9]
    gret 3
.endm
vminmaxd vmaxsd, cmgt, 1
vminmaxd vmaxud, cmhi, 1
vminmaxd vminsd, cmgt, 0
vminmaxd vminud, cmhi, 0
// vrld: each doubleword rotated left by vb's low 6 bits there
.gadget vrld
    v3_in
    mov x12, #63
    dup v2.2d, x12
    and v1.16b, v1.16b, v2.16b
    ushl v3.2d, v0.2d, v1.2d
    mov x12, #64
    dup v2.2d, x12
    sub v4.2d, v1.2d, v2.2d
    ushl v0.2d, v0.2d, v4.2d
    orr v0.16b, v0.16b, v3.16b
    str q0, [_cpu, x9]
    gret 3

// vsumsws: big-endian word 3 (the host's lane 0) = va's four words + vb's
// word 3, saturated; the rest zero
.gadget vsumsws
    v3_in
    saddlv d2, v0.4s
    fmov x12, d2
    smov x13, v1.s[0]
    add x12, x12, x13
    mov x13, #0x7fffffff
    cmp x12, x13
    csel x12, x12, x13, le
    mov x13, #-0x80000000
    cmp x12, x13
    csel x12, x12, x13, ge
    mov w12, w12
    add x9, x9, _cpu
    stp x12, xzr, [x9]
    gret 3
// vsum2sws: big-endian words 1 and 3 (host lanes 2 and 0) = each pair of
// va's words + vb's word there, saturated; words 0 and 2 zero
.gadget vsum2sws
    v3_in
    saddlp v2.2d, v0.4s
    sxtl v3.2d, v1.2s                   // vb's lanes 0 and 2, widened...
    uzp1 v4.4s, v1.4s, v1.4s
    sxtl v3.2d, v4.2s                   // ...lanes 0 and 2 to doublewords
    add v2.2d, v2.2d, v3.2d
    sqxtn v2.2s, v2.2d
    uxtl v2.2d, v2.2s                   // each word in the low half of its doubleword
    str q2, [_cpu, x9]
    gret 3
// vsum4ubs, vsum4sbs, vsum4shs: each word of vb + the sum of va's bytes or
// halfwords in it, saturated
.gadget vsum4ubs
    v3_in
    uaddlp v0.8h, v0.16b
    uaddlp v0.4s, v0.8h
    uqadd v0.4s, v0.4s, v1.4s
    str q0, [_cpu, x9]
    gret 3
.gadget vsum4sbs
    v3_in
    saddlp v0.8h, v0.16b
    saddlp v0.4s, v0.8h
    sqadd v0.4s, v0.4s, v1.4s
    str q0, [_cpu, x9]
    gret 3
.gadget vsum4shs
    v3_in
    saddlp v0.4s, v0.8h
    sqadd v0.4s, v0.4s, v1.4s
    str q0, [_cpu, x9]
    gret 3

// ---- VMX floating point ---------------------------------------------------------
// The VMX (AltiVec) float instructions round to nearest whatever FPSCR[RN]
// says, flush denormal operands and results to zero while VSCR[NJ] is set
// (as Linux starts every program), and leave FPSCR's flags alone: around the
// op, FPCR is the guest's with RMode nearest and FZ = NJ, and the FPSR comes
// back as it was. x14-x16 carry that across the op; each msr is skipped when
// nothing changed, since they cost.
.macro vmx_fp_begin
    mrs x15, fpcr
    mrs x16, fpsr
    ldr w13, [_cpu, #CPU_ppc64_vscr]
    ubfx w13, w13, #16, #1
    and x14, x15, #~(7 << 22)           // RMode (nearest is 0) and FZ
    orr x14, x14, x13, lsl #24
    cmp x14, x15
    b.eq 81f
    msr fpcr, x14
81:
.endm
.macro vmx_fp_end
    cmp x14, x15
    b.eq 82f
    msr fpcr, x15
82: mrs x13, fpsr
    cmp x13, x16
    b.eq 83f
    msr fpsr, x16
83:
.endm

// [vt][va][vb]
.macro vmx3 name, insn, nanfix=1
.gadget \name
    v3_in
    vmx_fp_begin
    \insn v2.4s, v0.4s, v1.4s
    .if \nanfix
    vquiet v5, 4s
    vnan_first v2, v0, v5, v6, v7, 4s   // (fcmeq raises Invalid on an SNaN)
    .endif
    vmx_fp_end
    str q2, [_cpu, x9]
    gret 3
.endm
vmx3 vaddfp, fadd
vmx3 vsubfp, fsub
vmx3 vmaxfp, fmax
vmx3 vminfp, fmin
vmx3 vcmpeqfp, fcmeq, 0
vmx3 vcmpgefp, fcmge, 0
vmx3 vcmpgtfp, fcmgt, 0
// vcmpbfp: each element's bit 0 says va > vb, bit 1 va < -vb (both for a
// NaN); a record form's CR6 then says "all in bounds" in its EQ bit, which
// is what the cr6 gadget makes of an all-zero result.
.gadget vcmpbfp
    v3_in
    vmx_fp_begin
    fcmge v2.4s, v1.4s, v0.4s           // va <= vb
    fneg v3.4s, v1.4s
    fcmge v4.4s, v0.4s, v3.4s           // va >= -vb
    vmx_fp_end
    movi v5.4s, #0x80, lsl #24
    bic v2.16b, v5.16b, v2.16b
    movi v5.4s, #0x40, lsl #24
    bic v4.16b, v5.16b, v4.16b
    orr v0.16b, v2.16b, v4.16b
    str q0, [_cpu, x9]
    gret 3

// [vt][vb]
.macro vmx2 name, insn
.gadget \name
    ldp x9, x10, [_ip]
    ldr q0, [_cpu, x10]
    vmx_fp_begin
    \insn v0.4s, v0.4s
    vmx_fp_end
    str q0, [_cpu, x9]
    gret 2
.endm
vmx2 vrfin, frintn
vmx2 vrfiz, frintz
vmx2 vrfip, frintp
vmx2 vrfim, frintm
// vrefp, vrsqrtefp: estimates, given exactly (as qemu does)
.gadget vrefp
    ldp x9, x10, [_ip]
    ldr q1, [_cpu, x10]
    vmx_fp_begin
    fmov v0.4s, #1.0
    fdiv v0.4s, v0.4s, v1.4s
    vmx_fp_end
    str q0, [_cpu, x9]
    gret 2
.gadget vrsqrtefp
    ldp x9, x10, [_ip]
    ldr q1, [_cpu, x10]
    vmx_fp_begin
    fsqrt v1.4s, v1.4s
    fmov v0.4s, #1.0
    fdiv v0.4s, v0.4s, v1.4s
    vmx_fp_end
    str q0, [_cpu, x9]
    gret 2

// vcfsx, vcfux: [vt][vb][2^-UIM's single bits]: the integer converted, then
// scaled (exactly, but for a result the scaling makes denormal)
.macro vmx_cf name, cvt
.gadget \name
    ldp x9, x10, [_ip]
    ldr x11, [_ip, #16]
    ldr q0, [_cpu, x10]
    dup v1.4s, w11
    vmx_fp_begin
    \cvt v0.4s, v0.4s
    fmul v0.4s, v0.4s, v1.4s
    vmx_fp_end
    str q0, [_cpu, x9]
    gret 3
.endm
vmx_cf vcfsx, scvtf
vmx_cf vcfux, ucvtf
// vctsxs, vctuxs: [vt][vb][2^UIM's double bits]: scaled in double (exact),
// truncated to a 64-bit integer, then saturated to the word: a NaN gives 0.
// (VSCR[SAT] is not kept, as for the integer saturating ops.)
.macro vmx_ct name, narrow, narrow2
.gadget \name
    ldp x9, x10, [_ip]
    ldr x11, [_ip, #16]
    ldr q0, [_cpu, x10]
    dup v4.2d, x11
    vmx_fp_begin
    fcvtl v2.2d, v0.2s
    fcvtl2 v3.2d, v0.4s
    fmul v2.2d, v2.2d, v4.2d
    fmul v3.2d, v3.2d, v4.2d
    fcvtzs v2.2d, v2.2d
    fcvtzs v3.2d, v3.2d
    vmx_fp_end
    \narrow v0.2s, v2.2d
    \narrow2 v0.4s, v3.2d
    str q0, [_cpu, x9]
    gret 3
.endm
vmx_ct vctsxs, sqxtn, sqxtn2
vmx_ct vctuxs, sqxtun, sqxtun2

// vexptefp, vlogefp: estimates (the ISA asks 1/16 relative and 1/32 absolute
// error), computed in double and so good to the last bit or so. qemu's own
// exp2 is a Taylor series about 0 and drifts far off it for large operands.
.p2align 3
ppc64_vmx_exp2_tab:
    .double -160.0, 130.0
    // 2^f = e^(f ln 2) on [0, 1): (ln 2)^k / k!, k = 9 down to 0
    .double 1.0178086009239696e-07, 1.3215486790144305e-06, 1.5252733804059838e-05
    .double 0.00015403530393381606, 0.0013333558146428441, 0.009618129107628477
    .double 0.055504108664821576, 0.2402265069591007, 0.6931471805599453, 1.0
ppc64_vmx_log2_tab:
    .quad 0x000fffffffffffff, 0x3ff0000000000000   // the mantissa, 1.0
    .double 1.4142135623730951, 0.5
    // ln m = 2 atanh s, s = (m-1)/(m+1): 1/(2k+1), k = 4 down to 0
    .double 0.1111111111111111, 0.14285714285714285, 0.2, 0.3333333333333333, 1.0
    .double 2.8853900817779268                    // 2 / ln 2

// \x (a 2d register) = 2^\x: clamped to [-160, 130], where the float
// narrowing gives 0 and infinity; n = floor, f = x - n; 2^f by its series,
// 2^n made from its bits. A NaN goes through as a NaN. Uses v20-v23, x12, x13.
.macro exp2_2d x
    adr x12, ppc64_vmx_exp2_tab
    ld1r {v20.2d}, [x12], #8
    fmax \x\().2d, \x\().2d, v20.2d
    ld1r {v20.2d}, [x12], #8
    fmin \x\().2d, \x\().2d, v20.2d
    frintm v21.2d, \x\().2d
    fsub v22.2d, \x\().2d, v21.2d
    fcvtzs v21.2d, v21.2d
    mov x13, #1023
    dup v23.2d, x13
    add v21.2d, v21.2d, v23.2d
    shl v21.2d, v21.2d, #52
    ld1r {v23.2d}, [x12], #8
    .rept 9
    ld1r {v20.2d}, [x12], #8
    fmla v20.2d, v23.2d, v22.2d
    mov v23.16b, v20.16b
    .endr
    fmul \x\().2d, v23.2d, v21.2d
.endm
.gadget vexptefp
    ldp x9, x10, [_ip]
    ldr q0, [_cpu, x10]
    vmx_fp_begin
    fcvtl v2.2d, v0.2s
    fcvtl2 v3.2d, v0.4s
    exp2_2d v2
    exp2_2d v3
    fcvtn v0.2s, v2.2d
    fcvtn2 v0.4s, v3.2d
    vmx_fp_end
    str q0, [_cpu, x9]
    gret 2

// \x (a 2d register, positive and finite) = log2 \x: x = 2^e m with m in
// [sqrt(2)/2, sqrt(2)), and ln m by its atanh series. Uses v20-v27, x12, x13.
.macro log2_2d x
    adr x12, ppc64_vmx_log2_tab
    ushr v20.2d, \x\().2d, #52
    mov x13, #1023
    dup v21.2d, x13
    sub v20.2d, v20.2d, v21.2d          // e
    ld1r {v21.2d}, [x12], #8
    and v22.16b, \x\().16b, v21.16b
    ld1r {v21.2d}, [x12], #8
    orr v22.16b, v22.16b, v21.16b       // m in [1, 2)
    ld1r {v21.2d}, [x12], #8
    fcmgt v23.2d, v22.2d, v21.2d        // past sqrt(2): halve it
    sub v20.2d, v20.2d, v23.2d
    ld1r {v21.2d}, [x12], #8
    fmul v24.2d, v22.2d, v21.2d
    bsl v23.16b, v24.16b, v22.16b
    scvtf v20.2d, v20.2d
    fmov v24.2d, #1.0
    fsub v25.2d, v23.2d, v24.2d
    fadd v26.2d, v23.2d, v24.2d
    fdiv v25.2d, v25.2d, v26.2d         // s
    fmul v26.2d, v25.2d, v25.2d         // s^2
    ld1r {v27.2d}, [x12], #8
    .rept 4
    ld1r {v21.2d}, [x12], #8
    fmla v21.2d, v27.2d, v26.2d
    mov v27.16b, v21.16b
    .endr
    fmul v25.2d, v25.2d, v27.2d         // ln m / 2
    ld1r {v21.2d}, [x12], #8
    fmla v20.2d, v25.2d, v21.2d
    mov \x\().16b, v20.16b
.endm
// The special operands as qemu's float32_log2 has them: +-0 (and, with NJ, a
// denormal) gives -inf, any operand with its sign set (a NaN included) the
// default NaN, +inf itself, a NaN itself made quiet.
.gadget vlogefp
    ldp x9, x10, [_ip]
    ldr q0, [_cpu, x10]
    vmx_fp_begin
    fcvtl v2.2d, v0.2s
    fcvtl2 v3.2d, v0.4s
    log2_2d v2
    log2_2d v3
    fcvtn v1.2s, v2.2d
    fcvtn2 v1.4s, v3.2d
    fcmeq v7.4s, v0.4s, #0.0            // zero (flushed, with NJ)
    vmx_fp_end
    mov w13, #0xff800000
    dup v3.4s, w13
    mov v2.16b, v7.16b
    bsl v2.16b, v3.16b, v1.16b          // -inf
    mov w13, #0x7f800000
    dup v3.4s, w13
    cmeq v4.4s, v0.4s, v3.4s
    bsl v4.16b, v3.16b, v2.16b          // +inf
    and v5.16b, v0.16b, v3.16b
    cmeq v5.4s, v5.4s, v3.4s            // the exponent all ones
    mov w13, #0x007fffff
    dup v6.4s, w13
    and v6.16b, v0.16b, v6.16b
    cmtst v6.4s, v6.4s, v6.4s           // a mantissa
    and v5.16b, v5.16b, v6.16b          // a NaN
    mov w13, #0x00400000
    dup v6.4s, w13
    orr v6.16b, v0.16b, v6.16b
    bsl v5.16b, v6.16b, v4.16b          // the NaN, quiet
    cmlt v6.4s, v0.4s, #0
    bic v6.16b, v6.16b, v7.16b          // the sign, and not zero
    mov w13, #0x7fc00000
    dup v3.4s, w13
    bsl v6.16b, v3.16b, v5.16b
    str q6, [_cpu, x9]
    gret 2

// vmaddfp vt = va * vc + vb, vnmsubfp vt = -(va * vc - vb), fused, a NaN
// never negated: [vt][va][vb][vc]
// NaN operands: va's, then vb's (the addend, kept in v7), then vc's
.macro vmx_nan3
    vquiet v16, 4s
    vnan_first v1, v2, v16, v5, v6, 4s
    vnan_first v1, v7, v16, v5, v6, 4s
    vnan_first v1, v0, v16, v5, v6, 4s
.endm
.gadget vmaddfp
    ldp x9, x10, [_ip]
    ldp x11, x12, [_ip, #16]
    ldr q0, [_cpu, x10]
    ldr q1, [_cpu, x11]
    ldr q2, [_cpu, x12]
    mov v7.16b, v1.16b
    vmx_fp_begin
    fmla v1.4s, v0.4s, v2.4s
    vmx_nan3
    vmx_fp_end
    str q1, [_cpu, x9]
    gret 4
.gadget vnmsubfp
    ldp x9, x10, [_ip]
    ldp x11, x12, [_ip, #16]
    ldr q0, [_cpu, x10]
    ldr q1, [_cpu, x11]
    ldr q2, [_cpu, x12]
    vmx_fp_begin
    mov v7.16b, v1.16b
    vnn_neg v1, v3, v4, 4s
    fmla v1.4s, v0.4s, v2.4s
    vnn_neg v1, v3, v4, 4s
    vmx_nan3
    vmx_fp_end
    str q1, [_cpu, x9]
    gret 4
