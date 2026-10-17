// ppc64 guest integer gadgets: arithmetic, logic, shifts and rotates,
// compares, the condition register and XER, and moves between registers.
//
// Streams: [gadget][rd][ra][rb] (rr), [gadget][rd][ra][imm] (ri) and
// [gadget][rd][ra] (r1), each register the offset of its slot. A logical op's
// "rd" is the instruction's RA and its "ra" RS, as in qemu-aok's gadgets this
// file is widened from (emkey1/qemu target/ppc/tcg/aok/gadgets.S). A record
// form (Rc=1) is its op's gadget and then cr0 (below). Word ops (the w forms)
// compute in the w view; what they leave in the upper half is what POWER8
// leaves there.

#include "gadgets.h"

.text

.macro rr_in
    ldp x9, x10, [_ip]
    ldr x11, [_ip, #16]
    ldr x10, [_cpu, x10]
    ldr x11, [_cpu, x11]
.endm
.macro ri_in
    ldp x9, x10, [_ip]
    ldr x11, [_ip, #16]
    ldr x10, [_cpu, x10]
.endm
.macro r1_in
    ldp x9, x10, [_ip]
    ldr x10, [_cpu, x10]
.endm
.macro out3
    str x10, [_cpu, x9]
    gret 3
.endm
.macro out2
    str x10, [_cpu, x9]
    gret 2
.endm

// CA = the host's C after adds/adcs, or after subs/sbcs (PowerPC's carry out
// of rB + ~rA + 1 is the host's not-borrow C).
.macro ca_out
    cset w12, cs
    str w12, [_cpu, #CPU_ppc64_ca]
.endm
.macro ca_in
    ldr w12, [_cpu, #CPU_ppc64_ca]
    cmp w12, #1                 // C = CA
.endm

// ---- two-register ops ------------------------------------------------------
.macro rr_gadget name
.gadget \name
    rr_in
    op_\name
    out3
.endm
.macro ri_gadget name, op
.gadget \name
    ri_in
    op_\op
    out3
.endm
.macro r1_gadget name
.gadget \name
    r1_in
    op_\name
    out2
.endm

.macro op_add
    add x10, x10, x11
.endm
.macro op_subf
    sub x10, x11, x10
.endm
.macro op_and
    and x10, x10, x11
.endm
.macro op_andc
    bic x10, x10, x11
.endm
.macro op_or
    orr x10, x10, x11
.endm
.macro op_orc
    orn x10, x10, x11
.endm
.macro op_xor
    eor x10, x10, x11
.endm
.macro op_nand
    and x10, x10, x11
    mvn x10, x10
.endm
.macro op_nor
    orr x10, x10, x11
    mvn x10, x10
.endm
.macro op_eqv
    eon x10, x10, x11
.endm
.macro op_mulld
    mul x10, x10, x11
.endm
.macro op_mulhd
    smulh x10, x10, x11
.endm
.macro op_mulhdu
    umulh x10, x10, x11
.endm
// mullw: the 64-bit product of the low words, signed
.macro op_mullw
    smull x10, w10, w11
.endm
// mulhw, mulhwu: the high word of the low words' product, in the low half
// and zero-extended (the high half is undefined; this is what qemu gives)
.macro op_mulhw
    smull x10, w10, w11
    lsr x10, x10, #32
.endm
.macro op_mulhwu
    umull x10, w10, w11
    lsr x10, x10, #32
.endm
// The divides. By zero the result is undefined; this gives the dividend (its
// low word for the word forms), as qemu does, where the host would give 0.
// The signed overflow gives the dividend on both.
.macro op_divd
    sdiv x12, x10, x11
    cmp x11, #0
    csel x10, x10, x12, eq
.endm
.macro op_divdu
    udiv x12, x10, x11
    cmp x11, #0
    csel x10, x10, x12, eq
.endm
.macro op_divw
    sdiv w12, w10, w11
    cmp w11, #0
    csel w10, w10, w12, eq
.endm
.macro op_divwu
    udiv w12, w10, w11
    cmp w11, #0
    csel w10, w10, w12, eq
.endm
// modsd, modud, modsw, moduw (ISA 3.0, offered by no AT_HWCAP2 here, but
// compilers targeting POWER9 emit them)
.macro op_modsd
    sdiv x12, x10, x11
    msub x10, x12, x11, x10
.endm
.macro op_modud
    udiv x12, x10, x11
    msub x10, x12, x11, x10
.endm
.macro op_modsw
    sdiv w12, w10, w11
    msub w10, w12, w11, w10
    sxtw x10, w10
.endm
.macro op_moduw
    udiv w12, w10, w11
    msub w10, w12, w11, w10
.endm
// sld, srd: rB's low 7 bits; 64 or more gives 0
.macro op_sld
    lsl x12, x10, x11
    tst x11, #0x40
    csel x10, xzr, x12, ne
.endm
.macro op_srd
    lsr x12, x10, x11
    tst x11, #0x40
    csel x10, xzr, x12, ne
.endm
// slw, srw: rB's low 6 bits on the low word; 32 or more gives 0
.macro op_slw
    lsl w12, w10, w11
    tst x11, #0x20
    csel w10, wzr, w12, ne
.endm
.macro op_srw
    lsr w12, w10, w11
    tst x11, #0x20
    csel w10, wzr, w12, ne
.endm
// srad: CA = rS negative and a 1 shifted out; 64 or more shifts everything
.macro op_srad
    and x13, x11, #0x7f
    cmp x13, #63
    mov x14, #63
    csel x13, x13, x14, ls         // a shift of 63 gives the same as more
    asr x12, x10, x13
    mov x14, #1
    lsl x14, x14, x13
    sub x14, x14, #1
    tst x11, #0x40
    csinv x14, x14, xzr, eq        // 64+: every bit is shifted out
    tst x10, x14
    cset w15, ne
    cmp x10, #0
    csel w15, w15, wzr, lt
    str w15, [_cpu, #CPU_ppc64_ca]
    mov x10, x12
.endm
// sraw: the same on the low word, the result sign-extended
.macro op_sraw
    sxtw x10, w10
    and x13, x11, #0x3f
    cmp x13, #31
    mov x14, #31
    csel x13, x13, x14, ls
    asr x12, x10, x13
    mov x14, #1
    lsl x14, x14, x13
    sub x14, x14, #1
    tst x11, #0x20
    csinv x14, x14, xzr, eq
    tst x10, x14
    cset w15, ne
    cmp x10, #0
    csel w15, w15, wzr, lt
    str w15, [_cpu, #CPU_ppc64_ca]
    mov x10, x12
.endm
// cmpb: each byte 0xff where rS's and rB's are equal
.macro op_cmpb
    fmov d0, x10
    fmov d1, x11
    cmeq v0.8b, v0.8b, v1.8b
    fmov x10, d0
.endm

rr_gadget add
rr_gadget subf
rr_gadget and
rr_gadget andc
rr_gadget or
rr_gadget orc
rr_gadget xor
rr_gadget nand
rr_gadget nor
rr_gadget eqv
rr_gadget mulld
rr_gadget mulhd
rr_gadget mulhdu
rr_gadget mullw
rr_gadget mulhw
rr_gadget mulhwu
rr_gadget divd
rr_gadget divdu
rr_gadget divw
rr_gadget divwu
rr_gadget modsd
rr_gadget modud
rr_gadget modsw
rr_gadget moduw
rr_gadget sld
rr_gadget srd
rr_gadget slw
rr_gadget srw
rr_gadget srad
rr_gadget sraw
rr_gadget cmpb

// immediates: addi (rA != 0; with rA 0 it is mov_const), the logical ones,
// mulli
ri_gadget addi, add
ri_gadget andi, and
ri_gadget ori, or
ri_gadget xori, xor
ri_gadget mulli, mulld

// ---- carries ---------------------------------------------------------------
.gadget addc
    rr_in
    adds x10, x10, x11
    ca_out
    out3
.gadget adde
    rr_in
    ca_in
    adcs x10, x10, x11
    ca_out
    out3
.gadget subfc
    rr_in
    subs x10, x11, x10
    ca_out
    out3
.gadget subfe
    rr_in
    ca_in
    sbcs x10, x11, x10
    ca_out
    out3
// addic, subfic: [rd][ra][imm]
.gadget addic
    ri_in
    adds x10, x10, x11
    ca_out
    out3
.gadget subfic
    ri_in
    subs x10, x11, x10
    ca_out
    out3
// addze, addme, subfze, subfme: [rd][ra]
.gadget addze
    r1_in
    ca_in
    adcs x10, x10, xzr
    ca_out
    out2
.gadget addme
    r1_in
    ca_in
    mov x11, #-1
    adcs x10, x10, x11
    ca_out
    out2
.gadget subfze
    r1_in
    ca_in
    sbcs x10, xzr, x10
    ca_out
    out2
.gadget subfme
    r1_in
    ca_in
    mov x11, #-1
    sbcs x10, x11, x10
    ca_out
    out2

// ---- the OE forms ----------------------------------------------------------
// XER[OV] is the signed overflow of the operation, and SO collects it. (OV32
// is ISA 3.0's; a POWER8 leaves it alone, and so does this.) Streams as the
// plain forms'.
.macro ov_store
    str w14, [_cpu, #CPU_ppc64_ov]
    ldr w13, [_cpu, #CPU_ppc64_so]
    orr w13, w13, w14
    str w13, [_cpu, #CPU_ppc64_so]
.endm
.macro ov_out
    cset w14, vs
    ov_store
.endm
.macro rr_o name, ca, op:vararg
.gadget \name
    rr_in
    .if \ca == 2
    ca_in
    .endif
    \op
    .if \ca
    ca_out
    .endif
    ov_out
    out3
.endm
.macro r1_o name, op:vararg
.gadget \name
    r1_in
    ca_in
    \op
    ca_out
    ov_out
    out2
.endm
rr_o addo, 0, adds x10, x10, x11
rr_o subfo, 0, subs x10, x11, x10
rr_o addco, 1, adds x10, x10, x11
rr_o subfco, 1, subs x10, x11, x10
rr_o addeo, 2, adcs x10, x10, x11
rr_o subfeo, 2, sbcs x10, x11, x10
r1_o addzeo, adcs x10, x10, xzr
r1_o subfzeo, sbcs x10, xzr, x10
.gadget addmeo
    r1_in
    ca_in
    mov x11, #-1
    adcs x10, x10, x11
    ca_out
    ov_out
    out2
.gadget subfmeo
    r1_in
    ca_in
    mov x11, #-1
    sbcs x10, x11, x10
    ca_out
    ov_out
    out2
.gadget nego
    r1_in
    negs x10, x10
    ov_out
    out2
// mulldo: the 128-bit product does not fit in 64
.gadget mulldo
    rr_in
    mul x12, x10, x11
    smulh x13, x10, x11
    cmp x13, x12, asr #63
    cset w14, ne
    mov x10, x12
    ov_store
    out3
// mullwo: the product of the words does not fit in 32
.gadget mullwo
    rr_in
    smull x10, w10, w11
    cmp x10, w10, sxtw
    cset w14, ne
    ov_store
    out3
// the divides: by zero, or the most negative by -1 (the signed ones)
.gadget divdo
    rr_in
    mov x13, #0x8000000000000000
    cmp x10, x13
    ccmn x11, #1, #0, eq
    cset w14, eq
    cmp x11, #0
    cset w15, eq
    orr w14, w14, w15
    op_divd
    ov_store
    out3
.gadget divduo
    rr_in
    cmp x11, #0
    cset w14, eq
    op_divdu
    ov_store
    out3
.gadget divwo
    rr_in
    mov w13, #0x80000000
    cmp w10, w13
    ccmn w11, #1, #0, eq
    cset w14, eq
    cmp w11, #0
    cset w15, eq
    orr w14, w14, w15
    op_divw
    ov_store
    out3
.gadget divwuo
    rr_in
    cmp w11, #0
    cset w14, eq
    op_divwu
    ov_store
    out3

// ---- the extended divides (ISA 2.06) ---------------------------------------
// divweu, divwe: rA's low word times 2^32 over rB's low word; divdeu, divde:
// rA times 2^64 over rB. A quotient that does not fit (or a divisor of 0) is
// undefined; this gives 0, as qemu does, and w14 says it overflowed (for the
// OE forms). x3-x6 are scratch: these call nothing.
.macro op_divweu
    lsl x12, x10, #32
    mov w11, w11
    udiv x13, x12, x11
    lsr x15, x13, #32
    cmp x11, #0
    cset w14, eq
    cmp x15, #0
    csinc w14, w14, wzr, eq
    cmp w14, #0
    csel x10, x13, xzr, eq
.endm
.macro op_divwe
    lsl x12, x10, #32           // (int64) rA << 32
    sxtw x11, w11
    mov x15, #0x8000000000000000
    cmp x12, x15
    ccmn x11, #1, #0, eq
    cset w14, eq
    cmp x11, #0
    csinc w14, w14, wzr, ne
    sdiv x13, x12, x11
    cmp x13, w13, sxtw
    csinc w14, w14, wzr, eq
    cmp w14, #0
    csel x10, x13, xzr, eq
.endm
// x10 = (x10 * 2^64) / x11, unsigned, by long division (x10 < x11, or it
// overflows); w14 = overflow
.macro udiv128_hi
    cmp x10, x11
    cset w14, hs                // rA >= rB, rB == 0 included
    b.hs 3f
    mov x13, x10                // the remainder
    mov x12, #0                 // the quotient
    mov x15, #64
1:  adds x13, x13, x13          // C: the bit shifted out of the remainder
    ccmp x13, x11, #2, cc       // with it, the remainder is past rB anyway
    csel x3, x11, xzr, hs
    cset x4, hs
    sub x13, x13, x3
    orr x12, x4, x12, lsl #1
    subs x15, x15, #1
    b.ne 1b
    mov x10, x12
    b 4f
3:  mov x10, #0
4:
.endm
.macro op_divdeu
    udiv128_hi
.endm
.macro op_divde
    eor x5, x10, x11            // the quotient's sign, bit 63
    cmp x10, #0
    cneg x10, x10, lt
    cmp x11, #0
    cneg x11, x11, lt
    udiv128_hi
    cbnz w14, 6f
    tbnz x5, #63, 5f
    tbz x10, #63, 6f            // positive: below 2^63
    b 7f
5:  neg x10, x10                // negative: down to -2^63
    cbz x10, 6f
    tbnz x10, #63, 6f
7:  mov w14, #1
    mov x10, #0
6:
.endm
.irp name, divweu, divwe, divdeu, divde
.gadget \name
    rr_in
    op_\name
    out3
.gadget \name\()o
    rr_in
    op_\name
    ov_store
    out3
.endr

// ---- one-register ops -------------------------------------------------------
.macro op_neg
    neg x10, x10
.endm
.macro op_extsb
    sxtb x10, w10
.endm
.macro op_extsh
    sxth x10, w10
.endm
.macro op_extsw
    sxtw x10, w10
.endm
.macro op_cntlzw
    clz w10, w10
.endm
.macro op_cntlzd
    clz x10, x10
.endm
.macro op_cnttzw
    rbit w10, w10
    clz w10, w10
.endm
.macro op_cnttzd
    rbit x10, x10
    clz x10, x10
.endm
// popcntb: each byte's count in that byte
.macro op_popcntb
    fmov d0, x10
    cnt v0.8b, v0.8b
    fmov x10, d0
.endm
// popcntw: each word's count in that word
.macro op_popcntw
    fmov d0, x10
    cnt v0.8b, v0.8b
    uaddlp v0.4h, v0.8b
    uaddlp v0.2s, v0.4h
    fmov x10, d0
.endm
.macro op_popcntd
    fmov d0, x10
    cnt v0.8b, v0.8b
    addv b0, v0.8b
    fmov x10, d0
.endm
// mr (or rA,rS,rS), mflr, mtctr and the rest of the plain moves
.macro op_mov
.endm
r1_gadget neg
r1_gadget extsb
r1_gadget extsh
r1_gadget extsw
r1_gadget cntlzw
r1_gadget cntlzd
r1_gadget cnttzw
r1_gadget cnttzd
r1_gadget popcntb
r1_gadget popcntw
r1_gadget popcntd
r1_gadget mov

// ---- shifts and rotates by an immediate -------------------------------------
// sradi, srawi: [rd][rs][sh]; CA as srad's
.gadget sradi
    ri_in
    asr x12, x10, x11
    mov x14, #1
    lsl x14, x14, x11
    sub x14, x14, #1
    tst x10, x14
    cset w15, ne
    cmp x10, #0
    csel w15, w15, wzr, lt
    str w15, [_cpu, #CPU_ppc64_ca]
    mov x10, x12
    out3
.gadget srawi
    ri_in
    sxtw x10, w10
    asr x12, x10, x11
    mov x14, #1
    lsl x14, x14, x11
    sub x14, x14, #1
    tst x10, x14
    cset w15, ne
    cmp x10, #0
    csel w15, w15, wzr, lt
    str w15, [_cpu, #CPU_ppc64_ca]
    mov x10, x12
    out3

// rldicl, rldicr, rldic, sldi, srdi and the rest of the rotate-and-mask
// doublewords: rA = rotl64(rS, sh) & mask. [rd][rs][64 - sh][mask]
.gadget rotd
    ldp x9, x10, [_ip]
    ldp x11, x12, [_ip, #16]
    ldr x10, [_cpu, x10]
    ror x10, x10, x11
    and x10, x10, x12
    str x10, [_cpu, x9]
    gret 4
// rldcl, rldcr: the rotation from rB's low 6 bits. [rd][rs][rb][mask]
.gadget rotd_r
    ldp x9, x10, [_ip]
    ldp x11, x12, [_ip, #16]
    ldr x10, [_cpu, x10]
    ldr x11, [_cpu, x11]
    neg x11, x11
    ror x10, x10, x11
    and x10, x10, x12
    str x10, [_cpu, x9]
    gret 4
// rldimi: rA = (rotl64(rS, sh) & mask) | (rA & ~mask). [rd][rs][64 - sh][mask]
.gadget rotd_ins
    ldp x9, x10, [_ip]
    ldp x11, x12, [_ip, #16]
    ldr x10, [_cpu, x10]
    ldr x13, [_cpu, x9]
    ror x10, x10, x11
    and x10, x10, x12
    bic x13, x13, x12
    orr x10, x10, x13
    str x10, [_cpu, x9]
    gret 4
// rlwinm and kin: the low word rotated, repeated in both halves, & mask.
// [rd][rs][32 - sh][mask], mask 64 bits (it reaches the high half when MB > ME)
.gadget rotw
    ldp x9, x10, [_ip]
    ldp x11, x12, [_ip, #16]
    ldr x10, [_cpu, x10]
    ror w10, w10, w11
    orr x10, x10, x10, lsl #32
    and x10, x10, x12
    str x10, [_cpu, x9]
    gret 4
// rlwnm: the rotation from rB's low 5 bits. [rd][rs][rb][mask]
.gadget rotw_r
    ldp x9, x10, [_ip]
    ldp x11, x12, [_ip, #16]
    ldr x10, [_cpu, x10]
    ldr x11, [_cpu, x11]
    neg w11, w11
    ror w10, w10, w11
    orr x10, x10, x10, lsl #32
    and x10, x10, x12
    str x10, [_cpu, x9]
    gret 4
// rlwimi. [rd][rs][32 - sh][mask]
.gadget rotw_ins
    ldp x9, x10, [_ip]
    ldp x11, x12, [_ip, #16]
    ldr x10, [_cpu, x10]
    ldr x13, [_cpu, x9]
    ror w10, w10, w11
    orr x10, x10, x10, lsl #32
    and x10, x10, x12
    bic x13, x13, x12
    orr x10, x10, x13
    str x10, [_cpu, x9]
    gret 4

// ---- compares ---------------------------------------------------------------
// The CR field's word from the host flags of a compare: LT 8, GT 4, EQ 2, and
// SO from XER. \lt and \gt are the host conditions.
.macro cr_from_flags lt, gt
    mov w12, #8
    mov w13, #4
    csel w12, w12, w13, \lt
    mov w13, #2
    csel w12, w13, w12, eq
    ldr w13, [_cpu, #CPU_ppc64_so]
    orr w12, w12, w13
    str w12, [_cpu, x9]
.endm
// [crf][ra][rb] or [crf][ra][imm]; crf is the offset of the field's word
.macro cmp_gadget name, in, w, lt, gt, ext
.gadget \name
    \in
    .if \w
    \ext x10, w10
    .ifc \in,rr_in
    \ext x11, w11
    .endif
    .endif
    cmp x10, x11
    cr_from_flags \lt, \gt
    gret 3
.endm
.macro uxtw_ dst, src
    mov \src, \src
.endm
cmp_gadget cmpd, rr_in, 0, lt, gt
cmp_gadget cmpw, rr_in, 1, lt, gt, sxtw
cmp_gadget cmpld, rr_in, 0, lo, hi
cmp_gadget cmplw, rr_in, 1, lo, hi, uxtw_
cmp_gadget cmpdi, ri_in, 0, lt, gt
cmp_gadget cmpwi, ri_in, 1, lt, gt, sxtw
cmp_gadget cmpldi, ri_in, 0, lo, hi
cmp_gadget cmplwi, ri_in, 1, lo, hi, uxtw_

// A record form's CR0 from the result it wrote: [rd]
.gadget cr0
    ldr x9, [_ip]
    ldr x10, [_cpu, x9]
    mov x9, #CPU_ppc64_cr
    cmp x10, #0
    cr_from_flags lt, gt
    gret 1

// isel rD, (rA|0), rB, BI: [rd][ra][rb][crf][bit mask]
.gadget isel
    ldp x9, x10, [_ip]
    ldp x11, x12, [_ip, #16]
    ldr x13, [_ip, #32]
    ldr w12, [_cpu, x12]
    ldr x10, [_cpu, x10]
    ldr x11, [_cpu, x11]
    tst w12, w13
    csel x10, x10, x11, ne
    str x10, [_cpu, x9]
    gret 5

// ---- the condition register as one ------------------------------------------
// mfcr (and mfocrf: the other fields are undefined, and these are right): [rd]
.gadget mfcr
    ldr x9, [_ip]
    mov x10, #CPU_ppc64_cr
    add x10, _cpu, x10
    ldp w11, w12, [x10]
    ldp w13, w14, [x10, #8]
    lsl w11, w11, #28
    orr w11, w11, w12, lsl #24
    orr w11, w11, w13, lsl #20
    orr w11, w11, w14, lsl #16
    ldp w12, w13, [x10, #16]
    ldp w14, w15, [x10, #24]
    orr w11, w11, w12, lsl #12
    orr w11, w11, w13, lsl #8
    orr w11, w11, w14, lsl #4
    orr w11, w11, w15
    str x11, [_cpu, x9]
    gret 1
// mtcrf FXM, rS (and mtocrf): [rs][fxm]; field n follows FXM's bit 7 - n
.gadget mtcrf
    ldp x9, x10, [_ip]
    ldr x9, [_cpu, x9]
    mov x11, #CPU_ppc64_cr
    add x11, _cpu, x11
    .irp n, 0, 1, 2, 3, 4, 5, 6, 7
    tbz x10, #(7 - \n), 1f
    ubfx w12, w9, #(28 - 4 * \n), #4
    str w12, [x11, #(4 * \n)]
1:
    .endr
    gret 2
// mcrf BF, BFA: [bf][bfa]
.gadget mcrf
    ldp x9, x10, [_ip]
    ldr w10, [_cpu, x10]
    str w10, [_cpu, x9]
    gret 2
// The CR bit ops, crand BT, BA, BB and kin: [bt][bt mask][ba][ba mask][bb][bb mask],
// each bit a field's word and the bit's value in it.
.macro crop name, op
.gadget \name
    ldp x9, x10, [_ip]
    ldp x11, x12, [_ip, #16]
    ldp x13, x14, [_ip, #32]
    ldr w11, [_cpu, x11]
    ldr w13, [_cpu, x13]
    tst w11, w12
    cset w11, ne
    tst w13, w14
    cset w13, ne
    \op w11, w11, w13
    ldr w15, [_cpu, x9]
    bic w15, w15, w10
    tst w11, #1
    csel w10, w10, wzr, ne
    orr w15, w15, w10
    str w15, [_cpu, x9]
    gret 6
.endm
.macro eon_ d, a, b
    eon \d, \a, \b
.endm
.macro nand_ d, a, b
    and \d, \a, \b
    mvn \d, \d
.endm
.macro nor_ d, a, b
    orr \d, \a, \b
    mvn \d, \d
.endm
.macro andc_ d, a, b
    bic \d, \a, \b
.endm
.macro orc_ d, a, b
    orn \d, \a, \b
.endm
crop crand, and
crop cror, orr
crop crxor, eor
crop creqv, eon_
crop crnand, nand_
crop crnor, nor_
crop crandc, andc_
crop crorc, orc_

// ---- XER, the time base -------------------------------------------------------
// mfxer: [rd]
.gadget mfxer
    ldr x9, [_ip]
    ldr w10, [_cpu, #CPU_ppc64_so]
    ldr w11, [_cpu, #CPU_ppc64_ov]
    lsl w10, w10, #31
    orr w10, w10, w11, lsl #30
    ldr w11, [_cpu, #CPU_ppc64_ca]
    orr w10, w10, w11, lsl #29
    ldr w11, [_cpu, #CPU_ppc64_ov32]
    orr w10, w10, w11, lsl #19
    ldr w11, [_cpu, #CPU_ppc64_ca32]
    orr w10, w10, w11, lsl #18
    ldr w11, [_cpu, #CPU_ppc64_xer_rest]
    orr w10, w10, w11
    str x10, [_cpu, x9]
    gret 1
// mtxer: [rs]
.gadget mtxer
    ldr x9, [_ip]
    ldr x10, [_cpu, x9]
    ubfx w11, w10, #31, #1
    str w11, [_cpu, #CPU_ppc64_so]
    ubfx w11, w10, #30, #1
    str w11, [_cpu, #CPU_ppc64_ov]
    ubfx w11, w10, #29, #1
    str w11, [_cpu, #CPU_ppc64_ca]
    ubfx w11, w10, #19, #1
    str w11, [_cpu, #CPU_ppc64_ov32]
    ubfx w11, w10, #18, #1
    str w11, [_cpu, #CPU_ppc64_ca32]
    and w10, w10, #0x1fffffff
    and w10, w10, #0xfff3ffff
    str w10, [_cpu, #CPU_ppc64_xer_rest]
    gret 1
// mfspr rT, 777: AOK_VCLOCK, which only the vDSO reads (vdso/ppc64/vdso.S).
// rT names a clock, and is replaced by kernel/time.c's vdso_clock_ns of it:
// the reading clock_gettime(2) would give, in ns, or a negative value for
// "make the system call". [rt]
.gadget vclock
    ldr x19, [_ip]                  // rt's offset, callee-saved across the call
    ldr w0, [_cpu, x19]             // the clock: rT's low word
    save_c                          // stores x0, and leaves it as the argument
    bl NAME(vdso_clock_ns)
    mov x20, x0                     // x0 itself is restored by restore_c
    restore_c
    str x20, [_cpu, x19]
    gret 1

// mftb, mfspr 268/269: the host's virtual counter, whose frequency
// /proc/cpuinfo's "timebase" line gives. [rd]; mftbu takes the high word.
.gadget mftb
    ldr x9, [_ip]
    mrs x10, cntvct_el0
    str x10, [_cpu, x9]
    gret 1
.gadget mftbu
    ldr x9, [_ip]
    mrs x10, cntvct_el0
    lsr x10, x10, #32
    str x10, [_cpu, x9]
    gret 1

// ---- conditional traps ----------------------------------------------------------
// tw, twi, td, tdi: [ra][rb or imm][TO][pc]. A TO bit that holds (16: a < b,
// 8: a > b, 4: a == b, 2: a <u b, 1: a >u b) traps: SIGTRAP at the
// instruction, as Linux's program check delivers it.
.macro trap_gadget name, w, imm
.gadget \name
    ldp x9, x10, [_ip]
    ldr x11, [_ip, #16]
    ldr x9, [_cpu, x9]
    .if !\imm
    ldr x10, [_cpu, x10]
    .endif
    .if \w
    sxtw x9, w9
    sxtw x10, w10
    .endif
    mov w12, #0
    cmp x9, x10
    mov w13, #16
    csel w13, w13, wzr, lt
    orr w12, w12, w13
    mov w13, #8
    csel w13, w13, wzr, gt
    orr w12, w12, w13
    mov w13, #4
    csel w13, w13, wzr, eq
    orr w12, w12, w13
    .if \w
    cmp w9, w10
    .endif
    mov w13, #2
    csel w13, w13, wzr, lo
    orr w12, w12, w13
    mov w13, #1
    csel w13, w13, wzr, hi
    orr w12, w12, w13
    tst w12, w11
    b.ne 1f
    gret 4
1:  ldr x8, [_ip, #24]
    str x8, [_cpu, #CPU_ppc64_pc]
    mov w0, #INT_BREAKPOINT
    b jit_exit
.endm
trap_gadget tw, 1, 0
trap_gadget twi, 1, 1
trap_gadget td, 0, 0
trap_gadget tdi, 0, 1

// bpermd rA, rS, rB: bit 7 - i of rA (host numbering) is the bit of rB that
// rS's big-endian byte i (host byte 7 - i) numbers, big-endian, 0 the most
// significant; 64 or more gives 0. [rd][rs][rb]
.gadget bpermd
    rr_in
    mov x12, #0
    mov x13, #7                     // host byte of rS
1:  lsl x14, x13, #3
    lsr x14, x10, x14
    and x14, x14, #0xff
    cmp x14, #64
    b.hs 2f
    mov x15, #63
    sub x15, x15, x14
    lsr x15, x11, x15
    and x15, x15, #1
    lsl x15, x15, x13               // rA bit 7 - i is host byte number 7 - i
    orr x12, x12, x15
2:  subs x13, x13, #1
    b.ge 1b
    mov x10, x12
    out3
