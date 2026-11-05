# The x87's register forms: every D8-DF instruction whose operands are
# registers of the stack, the constants, the stack's own instructions. They
# touch nothing but the x87 state (FCOMI the flags, which the host writes,
# and FCMOV reads them, through the host's macro). See x87-arith.S.

# FWAIT, and FNOP, which waits and does nothing else.
.gadget x87_fwait
    x87_wait
    gret_nomem 1

# ---------------------------------------------------------------------------
# Register forms. These touch nothing but the x87 state (FCOMI the flags,
# FCMOV reads them), so i386 and amd64 share them, and the amd64 register
# cache stays live across them.
# ---------------------------------------------------------------------------

# [s | d << 3 | pop << 6]: ST(d) = ST(d) op ST(s), popping after.
.macro x87_rr name, core
.gadget x87_\name\()_rr
    x87_wait
    x87_begin
    ldr w0, [_ip]
    ubfx w0, w0, 3, 3
    ldr w10, [_ip]
    and w10, w10, 7
    bl x87_get2
    cbnz w0, 1f
    bl \core
1:  ldr w0, [_ip]
    bl x87_put
    gret_nomem 1
.endm
x87_rr add, x87_add
x87_rr sub, x87_sub
x87_rr subr, x87_subr
x87_rr mul, x87_mul
x87_rr div, x87_div
x87_rr divr, x87_divr

# [i | pops << 6]: compare ST(0) with ST(i), then pop 0-2 times.
.macro x87_com_rr name, signalling
.gadget x87_\name\()_rr
    x87_wait
    x87_begin
    mov w0, 0
    ldr w10, [_ip]
    and w10, w10, 7
    bl x87_get2
    mov w12, 3
    cbnz w0, 1f
    mov w0, \signalling
    bl x87_cmp
    mov w12, w0
1:  ldr w0, [_ip]
    ubfx w0, w0, 6, 2
    bl x87_put_cc
    gret_nomem 1
.endm
x87_com_rr com, 1
x87_com_rr ucom, 0

# FTST: ST(0) against +0, signalling.
.gadget x87_ftst
    x87_wait
    x87_begin
    mov w0, 0
    bl x87_get1
    mov w12, 3
    cbnz w0, 1f
    mov x5, 0
    mov w6, 0
    mov w0, 1
    bl x87_cmp
    mov w12, w0
1:  mov w0, 0
    bl x87_put_cc
    gret_nomem 1

# [i | pop << 6]: FCOMI/FUCOMI: ZF PF CF from the compare of ST(0) with ST(i)
# (000 greater, 001 less, 100 equal, 111 unordered), OF SF AF cleared -- the
# true flags, in eflags and the CF/OF bytes, with none left lazy. An unmasked
# IE still sets them (unordered) but does not pop. C1 stays as it was unless a
# stack fault clears it, as hardware does; the SDM says it is always cleared.
.macro x87_comi_rr name, signalling
.gadget x87_\name\()_rr
    x87_wait
    x87_begin
    mov w0, 0
    ldr w10, [_ip]
    and w10, w10, 7
    bl x87_get2
    mov w12, 3
    cbnz w0, 1f
    mov w0, \signalling
    bl x87_cmp
    mov w12, w0
1:  // w12: 0 >, 1 <, 2 =, 3 unordered -> CF (bit 0), PF (2), ZF (6)
    mov w9, 0
    cmp w12, 1
    b.ne 2f
    mov w9, 1
2:  cmp w12, 2
    b.ne 21f
    mov w9, X87_EFL_ZF
21: cmp w12, 3
    b.ne 22f
    mov w9, (X87_EFL_ZF | X87_EFL_PF | X87_EFL_CF)
22: x87_comi_flags
3:  ldr w0, [_ip]
    ubfx w0, w0, 6, 1
    // pop (the condition codes stay as they were)
    x87_ld_fsw w8
    cbz w0, 4f
    bic w10, w7, w15
    tst w10, (X87_IE | X87_DE)
    b.ne 4f
    ubfx w9, w8, 11, 3
    x87_ld_valid w11
    mov w12, 1
    lsl w12, w12, w9
    bic w11, w11, w12
    x87_st_valid w11
    add w9, w9, 1
    bfi w8, w9, 11, 3
4:  bic w7, w7, X87_C1
    tst w7, X87_SF
    b.ne 5f
    and w10, w8, X87_C1
    orr w7, w7, w10
5:  bl x87_put_fsw
    gret_nomem 1
.endm
x87_comi_rr comi, 1
x87_comi_rr ucomi, 0

# [i]: FLD ST(i)
.gadget x87_fld_rr
    x87_wait
    x87_begin
    ldr w0, [_ip]
    bl x87_get1
    bl x87_push
    gret_nomem 1

# [i | pop << 6 | quiet << 7]: FST/FSTP ST(i): ST(i) = ST(0), a copy (an SNaN
# is no exception here), then pop. quiet is FSTP1 (D9 D8+i), the reserved
# alias, which on an empty ST(0) neither underflows nor writes: it only pops
# (an AMD Ryzen; FSTP8 and FSTP9 underflow as FSTP does).
.gadget x87_fst_rr
    x87_wait
    x87_begin
    mov w0, 0
    bl x87_get1
    cbz w0, 1f
    ldr w0, [_ip]
    tbz w0, 7, 1f
    mov w7, 0
    bl x87_pop1
    gret_nomem 1
1:  ldr w0, [_ip]
    lsl w0, w0, 3
    ldr w10, [_ip]
    and w10, w10, 0x40
    orr w0, w0, w10
    bl x87_put
    gret_nomem 1

# [i]: FXCH: an empty register taking part is the indefinite (IE, SF).
.gadget x87_fxch
    x87_wait
    x87_begin
    x87_ld_fsw w8
    x87_ld_valid w9
    ubfx w8, w8, 11, 3
    ldr w10, [_ip]
    add w10, w10, w8
    and w10, w10, 7
    add x12, _cpu, CPU_fp
    add x13, x12, x10, lsl 4
    add x12, x12, x8, lsl 4
    ldr x3, [x12]
    ldrh w4, [x12, 8]
    ldr x5, [x13]
    ldrh w6, [x13, 8]
    lsr w11, w9, w8
    tbnz w11, 0, 1f
    mov w7, (X87_IE | X87_SF)
    x87_indef x3, w4
1:  lsr w11, w9, w10
    tbnz w11, 0, 2f
    mov w7, (X87_IE | X87_SF)
    x87_indef x5, w6
2:  bic w11, w7, w15
    tbnz w11, 0, 3f
    str x5, [x12]
    strh w6, [x12, 8]
    str x3, [x13]
    strh w4, [x13, 8]
    mov w11, 1
    lsl w12, w11, w8
    lsl w11, w11, w10
    orr w9, w9, w11
    orr w9, w9, w12
    x87_st_valid w9
3:  x87_ld_fsw w8
    bl x87_put_fsw
    gret_nomem 1

# [i | pop << 6]: FFREE/FFREEP ST(i): tag it empty, then pop.
.gadget x87_ffree
    x87_wait
    x87_ld_fsw w8
    x87_ld_valid w11
    ubfx w9, w8, 11, 3
    ldr w10, [_ip]
    add w12, w10, w9
    and w12, w12, 7
    mov w13, 1
    lsl w12, w13, w12
    bic w11, w11, w12
    tbz w10, 6, 1f
    lsl w12, w13, w9
    bic w11, w11, w12
    add w9, w9, 1
    bfi w8, w9, 11, 3
    x87_st_fsw w8
1:  x87_st_valid w11
    gret_nomem 1

# FCHS, FABS: the sign bit only (no exception for any NaN), C1 clear.
.macro x87_sign name, op, imm
.gadget x87_\name
    x87_wait
    x87_begin
    mov w0, 0
    bl x87_get1
    cbnz w0, 1f
    \op w4, w4, \imm
1:  mov w0, 0
    bl x87_put
    gret_nomem 1
.endm
x87_sign fchs, eor, 0x8000
x87_sign fabs, and, 0x7fff

# FXAM: C3 C2 C0 the class of ST(0) -- 000 unsupported, 001 NaN, 010
# normal, 011 infinity, 100 zero, 101 empty, 110 denormal -- and C1 its sign.
.gadget x87_fxam
    x87_wait
    x87_ld_fsw w8
    x87_ld_valid w9
    ubfx w10, w8, 11, 3
    add x12, _cpu, CPU_fp
    add x12, x12, x10, lsl 4
    ldr x3, [x12]
    ldrh w4, [x12, 8]
    mov w11, X87_CC
    bic w8, w8, w11
    lsr w11, w4, 15
    bfi w8, w11, 9, 1
    lsr w9, w9, w10
    mov w13, (X87_C3 | X87_C0)
    tbz w9, 0, 2f
    x87_class w12, x3, w4, w0
    adr x13, x87_fxam_cc
    ldrh w13, [x13, x12, lsl 1]
2:  orr w8, w8, w13
    x87_st_fsw w8
    gret_nomem 1
x87_fxam_cc:
    .hword X87_C3, X87_C2, (X87_C3 | X87_C2), (X87_C2 | X87_C0), X87_C0, X87_C0, 0
    .balign 4

# [k]: FLD1, FLDL2T, FLDL2E, FLDPI, FLDLG2, FLDLN2, FLDZ. The irrational ones
# round in the rounding mode: truncated, plus one when rounding up -- or to
# nearest when the bits past 64 are above a half (all but log2(10)'s).
.gadget x87_fldc
    x87_wait
    x87_begin
    ldr w0, [_ip]
    and w0, w0, 7
    adr x8, x87_consts
    add x8, x8, x0, lsl 4
    ldr x3, [x8]
    ldrh w4, [x8, 8]
    ldrh w9, [x8, 10]               // 1 if nearest rounds up
    ubfx w10, w15, 10, 2
    cmp w10, 2
    b.eq 1f                         // up
    cbnz w10, 2f
    add x3, x3, x9                  // nearest
    b 2f
1:  ldrh w9, [x8, 12]               // 1 if inexact
    add x3, x3, x9
2:  bl x87_push
    gret_nomem 1
x87_consts:
    .quad 0x8000000000000000
    .hword 0x3fff, 0, 0, 0
    .quad 0xd49a784bcd1b8afe
    .hword 0x4000, 0, 1, 0
    .quad 0xb8aa3b295c17f0bb
    .hword 0x3fff, 1, 1, 0
    .quad 0xc90fdaa22168c234
    .hword 0x4000, 1, 1, 0
    .quad 0x9a209a84fbcff798
    .hword 0x3ffd, 1, 1, 0
    .quad 0xb17217f7d1cf79ab
    .hword 0x3ffe, 1, 1, 0
    .quad 0
    .hword 0, 0, 0, 0

# FINCSTP, FDECSTP: TOP +-1, the tags untouched, C1 clear.
.gadget x87_fincstp
    x87_wait
    x87_ld_fsw w8
    ubfx w9, w8, 11, 3
    add w9, w9, 1
    bfi w8, w9, 11, 3
    bic w8, w8, X87_C1
    x87_st_fsw w8
    gret_nomem 1
.gadget x87_fdecstp
    x87_wait
    x87_ld_fsw w8
    ubfx w9, w8, 11, 3
    sub w9, w9, 1
    bfi w8, w9, 11, 3
    bic w8, w8, X87_C1
    x87_st_fsw w8
    gret_nomem 1

# FNCLEX: the exception flags, SF, ES and B. FNINIT: control word 037F,
# status 0 (TOP 0), every register empty.
.gadget x87_fnclex
    x87_ld_fsw w8
    and w8, w8, 0x7f00
    x87_st_fsw w8
    gret_nomem 1
.gadget x87_fninit
    mov w8, 0x37f
    strh w8, [_cpu, CPU_fcw]
    x87_st_fsw wzr
    x87_st_valid wzr
    gret_nomem 1

# One-operand arithmetic on ST(0).
.macro x87_r1 name, core
.gadget x87_\name
    x87_wait
    x87_begin
    mov w0, 0
    bl x87_get1
    cbnz w0, 1f
    bl \core
1:  mov w0, 0
    bl x87_put
    gret_nomem 1
.endm
x87_r1 fsqrt, x87_sqrt
x87_r1 frndint, x87_rndint

# FSCALE: ST(0) = ST(0) * 2^trunc(ST(1)).
.gadget x87_fscale
    x87_wait
    x87_begin
    mov w0, 0
    mov w10, 1
    bl x87_get2
    cbnz w0, 1f
    bl x87_scale
1:  mov w0, 0
    bl x87_put
    gret_nomem 1

# ---------------------------------------------------------------------------
# FXTRACT: ST(0) = its exponent (as a value), then push its significand (the
# exponent field 3FFF, the sign kept). Zero: the exponent is -infinity and ZE;
# infinity: +infinity and the infinity; a NaN goes to both.
# ---------------------------------------------------------------------------
.gadget x87_fxtract
    x87_wait
    x87_begin
    mov w0, 0
    bl x87_get1
    cbnz w0, 8f
    x87_class w12, x3, w4, w0
    cmp w12, 6
    b.eq 7f
    cmp w12, 4
    b.hs 6f
    cmp w12, 3
    b.eq 5f
    cbz w12, 4f
    cmp w12, 2
    b.ne 1f
    orr w7, w7, X87_DE
1:  x87_norm x3, w4, w10
    mov w0, 16383
    sub w0, w10, w0
    sxtw x0, w0
    and w4, w4, 0x8000
    mov w10, 0x3fff
    orr w4, w4, w10
    mov x13, x3
    mov w14, w4
    bl x87_ld_int                   // the exponent -> B
    mov x3, x13
    mov w4, w14
    b 9f
4:  orr w7, w7, X87_ZE              // zero: -infinity, then the zero
    mov x5, 0x8000000000000000
    mov w6, 0xffff
    b 9f
5:  mov x5, 0x8000000000000000      // infinity: +infinity, then itself
    mov w6, 0x7fff
    b 9f
6:  cmp w12, 5
    b.ne 61f
    orr w7, w7, X87_IE
61: orr x3, x3, 0x4000000000000000
    mov x5, x3
    mov w6, w4
    b 9f
7:  orr w7, w7, X87_IE              // unsupported: the indefinite, twice
8:  x87_indef x3, w4
    mov x5, x3
    mov w6, w4
9:  // a full stack: the push overflows, and the exponent's register gets the
    // indefinite too
    x87_ld_fsw w8
    x87_ld_valid w11
    ubfx w9, w8, 11, 3
    sub w10, w9, 1
    and w10, w10, 7
    lsr w11, w11, w10
    tbz w11, 0, 11f
    x87_indef x5, w6
    mov w7, (X87_IE | X87_SF | X87_C1)
11: // unmasked IE, DE or ZE: nothing changes
    bic w10, w7, w15
    tst w10, (X87_IE | X87_DE | X87_ZE)
    b.ne 10f
    add x10, _cpu, CPU_fp
    add x10, x10, x9, lsl 4
    str x5, [x10]
    strh w6, [x10, 8]
    x87_ld_valid w11
    mov w10, 1
    lsl w10, w10, w9
    orr w11, w11, w10
    x87_st_valid w11
    bl x87_push
    gret_nomem 1
10: x87_ld_fsw w8
    bl x87_put_fsw
    gret_nomem 1

# ---------------------------------------------------------------------------
# FPREM (w0 = 0: the quotient truncated) and FPREM1 (1: rounded to nearest,
# IEEE's remainder): A = A - Q * B, exactly, Q's low three bits to C0 C3 C1
# and C2 clear -- or, for an exponent difference d of 64 or more, a partial
# remainder with C2 set: A reduced by a multiple of B * 2^(d - N), where an
# AMD Ryzen takes N = 32 + d mod 32 (leaving a difference that is a multiple
# of 32). A value that comes back as it went in is still canonical: a
# pseudo-denormal's exponent field 0 becomes 1.
# ---------------------------------------------------------------------------
x87_prem:
    str lr, [sp, -16]!
    mov w17, w0
    x87_check2
    cmp w12, 3
    b.eq x87_ret_invalid            // infinity rem anything
    cbz w13, x87_ret_invalid        // anything rem 0
    cmp w13, 3
    b.eq 21f                        // finite rem infinity: A, Q = 0
    cbz w12, 20f                    // 0 rem finite: 0
    lsr w11, w4, 15
    x87_norm x3, w4, w10
    x87_norm x5, w6, w12
    subs w13, w10, w12              // d
    b.lt 30f
    mov w16, 0                      // the quotient's low bits
    cmp w13, 64
    b.ge 40f
    // r = ma, q = 0; d + 1 quotient bits, each a compare and a subtract, the
    // remainder doubled between them (a carry out of it means r >= B)
    mov x8, x3
    mov x9, 0
1:  cmp x8, x5
    cset x14, hs
    b.lo 2f
    sub x8, x8, x5
2:  orr x9, x14, x9, lsl 1
    cbz w13, 3f
    sub w13, w13, 1
    adds x8, x8, x8
    b.cc 1b
    sub x8, x8, x5
    mov x14, 1
    b 2b
3:  // FPREM1: round the quotient to nearest, even on a tie
    cbz w17, 4f
    adds x14, x8, x8
    b.cs 31f
    cmp x14, x5
    b.hi 31f
    b.lo 4f
    tbz x9, 0, 4f
31: add x9, x9, 1
    sub x8, x5, x8
    eor w11, w11, 1
4:  // condition codes: C0 = q2, C3 = q1, C1 = q0; C2 = 0
    ubfx w14, w9, 2, 1
    lsl w16, w14, 8                 // C0
    ubfx w14, w9, 1, 1
    orr w16, w16, w14, lsl 14       // C3
    and w14, w9, 1
    orr w16, w16, w14, lsl 9        // C1
    // the remainder r * 2^(eb - 16383 - 63), exact
    sxtw x10, w12
    mov x9, 0
    mov x0, x16
    stp x0, xzr, [sp, -16]!
    x87_round_64
    ldp x0, xzr, [sp], 16
    cbnz x3, 5f
    lsl w4, w11, 15                 // a zero remainder keeps A's sign
5:  mov w8, (X87_C0 | X87_C1 | X87_C2 | X87_C3)
    bic w7, w7, w8
    orr w7, w7, w0
    b x87_ret_a
21: x87_effexp w10, w4              // A, canonical
    lsr w11, w4, 15
22: mov x8, x3
    mov x9, 0
    sxtw x10, w10
    x87_round_64
20: mov w8, (X87_C0 | X87_C1 | X87_C2 | X87_C3)
    bic w7, w7, w8
    b x87_ret_a
30: // |A| < |B| by exponent: A, but FPREM1 rounds a quotient over a half to 1
    cbz w17, 22b
    cmn w13, 1
    b.ne 22b
    cmp x3, x5
    b.ls 22b                        // a half exactly: Q = 0, even
    // A - B with B one binade up: 2 mb - ma (65 bits) at A's scale, the
    // sign flipped
    adds x8, x5, x5
    cset x9, cs
    subs x8, x8, x3
    sbc x9, x9, xzr
    mov x0, x8
    mov x8, x9
    mov x9, x0
    add w10, w10, 64
    eor w11, w11, 1
    sxtw x10, w10
    x87_round_64
    mov w8, (X87_C0 | X87_C1 | X87_C2 | X87_C3)
    bic w7, w7, w8
    orr w7, w7, X87_C1              // Q = 1
    b x87_ret_a
40: // partial: reduce by B * 2^(d - N), N = 32 + d mod 32: N more steps from ma
    and w14, w13, 31
    add w14, w14, 32                // N
    sub w13, w13, w14
    add w12, w12, w13               // the scale the remainder ends at
    mov w13, w14
    mov x8, x3
    mov x9, 0
41: cmp x8, x5
    b.lo 42f
    sub x8, x8, x5
42: cbz w13, 43f
    sub w13, w13, 1
    adds x8, x8, x8
    b.cc 41b
    sub x8, x8, x5
    b 42b
43: sxtw x10, w12
    mov x9, 0
    x87_round_64
    cbnz x3, 44f
    lsl w4, w11, 15
44: mov w8, (X87_C0 | X87_C1 | X87_C2 | X87_C3)
    bic w7, w7, w8
    orr w7, w7, X87_C2
    b x87_ret_a

# FPREM, FPREM1: ST(0) rem ST(1). The condition codes come from w7.
.macro x87_prem_g name, ieee
.gadget x87_\name
    x87_wait
    x87_begin
    mov w0, 0
    mov w10, 1
    bl x87_get2
    cbnz w0, 1f
    mov w0, \ieee
    bl x87_prem
1:  x87_ld_fsw w8
    bic w10, w7, w15
    tbnz w10, 0, 2f                 // unmasked IE: nothing
    tst w10, X87_DE
    b.ne 2f
    mov w9, (X87_C0 | X87_C2 | X87_C3)
    bic w8, w8, w9
    and w9, w7, w9
    orr w8, w8, w9
    ubfx w9, w8, 11, 3
    add x10, _cpu, CPU_fp
    add x10, x10, x9, lsl 4
    str x3, [x10]
    strh w4, [x10, 8]
    x87_ld_valid w11
    mov w10, 1
    lsl w10, w10, w9
    orr w11, w11, w10
    x87_st_valid w11
2:  bl x87_put_fsw
    gret_nomem 1
.endm
x87_prem_g fprem, 0
x87_prem_g fprem1, 1


.macro x87_fcmov gname, flag, f1, f2, neg
.gadget \gname
    x87_wait
    \flag w11, \f1
    .ifnb \f2
        \flag w12, \f2
        orr w11, w11, w12
    .endif
    .if \neg
        eor w11, w11, 1
    .endif
    x87_begin
    mov w0, 0
    ldr w10, [_ip]
    mov w14, w11                    // x87_get2 keeps x14
    bl x87_get2
    cbnz w0, 1f
    cbz w14, 1f
    mov x3, x5
    mov w4, w6
1:  mov w0, 0
    bl x87_put
    gret_nomem 1
.endm
.macro x87_fcmov_all pfx, flag
    x87_fcmov \pfx\()b, \flag, c,, 0
    x87_fcmov \pfx\()e, \flag, z,, 0
    x87_fcmov \pfx\()be, \flag, c, z, 0
    x87_fcmov \pfx\()u, \flag, p,, 0
    x87_fcmov \pfx\()nb, \flag, c,, 1
    x87_fcmov \pfx\()ne, \flag, z,, 1
    x87_fcmov \pfx\()nbe, \flag, c, z, 1
    x87_fcmov \pfx\()nu, \flag, p,, 1
.endm
