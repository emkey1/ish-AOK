# The x87, for every engine that runs an x86 guest on AArch64: the 80-bit
# arithmetic, in integer registers. This file is the arithmetic alone and
# reaches nothing: no CPU state, no memory, no stream. x87-stack.S is the
# register stack and the status word, x87-reg.S every D8-DF instruction's
# register form, x87-trans.S the transcendentals.
#
# The routines share a convention:
#
#   A = x3 (significand) / w4 (sign << 15 | exponent): the first operand and
#       the result. B = x5 / w6: the second operand.
#   w7: what the operation raised, in status-word bits -- IE DE ZE OE UE PE SF
#       (0-6) -- plus C1 at bit 9 and C0 C2 C3 (bits 8, 10, 14) for compares.
#   w15: the control word.
#   x0, x5, x6, x8-x14, x16, x17 are scratch, and x30 (they are called). They
#   touch no other register (the transcendentals use x19 and put it back),
#   and leave nothing on the stack.
#
# The answers are real hardware's: AOK-Workstation's tests/manual/x86/x87_kat.c holds
# 122880 of them, from an AMD Ryzen.
#
# What a host gives the four files (AOK-Workstation's is jit/gadgets-aarch64/x87.S):
#
#   _cpu, _ip             its CPU-state and stream registers, by these names;
#                         neither may be x0, x3-x17 or x30
#   CPU_fp                the offset in the CPU state of the eight registers,
#                         16 bytes each: the significand, then sign and
#                         exponent. ST(i) is register (TOP + i) & 7
#   CPU_fcw               the offset of the control word, 16 bits
#   x87_ld_fsw REG        REG = the status word, TOP in bits 11-13
#   x87_st_fsw REG        the status word = REG (which may be wzr)
#   x87_ld_valid REG      REG = the abridged tag: bit i set when physical
#                         register i holds a value
#   x87_st_valid REG      the abridged tag = REG (which may be wzr)
#                         (these four may use x16 and x17, nothing else)
#   x87_wait              leave, raising #MF, if the status word's ES is set
#   x87_comi_flags        the guest's ZF, PF and CF from w9 (EFLAGS's bits);
#                         OF, SF and AF clear. May use w9-w11
#   .gadget NAME, gret_nomem N
#                         a gadget's head, and its end: on to the stream's
#                         next gadget, past N operand words
#   a flag macro          for x87_fcmov_all (x87-reg.S): DST = 1 if the
#                         guest's flag WHICH (c, z or p) is set, else 0. May
#                         use w9, w10 and DST
#
# A register form's operand word has its number in the low eight bits; the
# rest is the host's.

#define X87_EFL_CF 0x01
#define X87_EFL_PF 0x04
#define X87_EFL_ZF 0x40

#define X87_IE 0x01
#define X87_DE 0x02
#define X87_ZE 0x04
#define X87_OE 0x08
#define X87_UE 0x10
#define X87_PE 0x20
#define X87_SF 0x40
#define X87_C0 0x100
#define X87_C1 0x200
#define X87_C2 0x400
#define X87_C3 0x4000
#define X87_CC (X87_C0 | X87_C1 | X87_C2 | X87_C3)
// A memory operand that was a denormal float: DE, once the operation knows a
// NaN is not its answer (a NaN result raises no DE). Never stored.
#define X87_DEP 0x10000

# the x87's real indefinite: a negative quiet NaN
.macro x87_indef m, se
    mov \m, 0xc000000000000000
    mov \se, 0xffff
.endm

# \cls = class of (\m, \se): 0 zero, 1 normal, 2 denormal (pseudo-denormals
# too), 3 infinity, 4 QNaN, 5 SNaN, 6 unsupported (an unnormal, a
# pseudo-NaN or pseudo-infinity). \t is scratch.
.macro x87_class cls, m, se, t
    and \t, \se, 0x7fff
    cbnz \t, .Lc1\@
    mov \cls, 0
    cbz \m, .Lc9\@
    mov \cls, 2
    b .Lc9\@
.Lc1\@:
    add \t, \t, 1
    tbnz \t, 15, .Lc3\@
    mov \cls, 1
    tbnz \m, 63, .Lc9\@
    mov \cls, 6
    b .Lc9\@
.Lc3\@:
    mov \cls, 6
    tbz \m, 63, .Lc9\@
    mov \cls, 3
    tst \m, 0x7fffffffffffffff
    b.eq .Lc9\@
    mov \cls, 4
    tbnz \m, 62, .Lc9\@
    mov \cls, 5
.Lc9\@:
.endm

# The scale of an exponent field: a denormal's 0 means the same as 1.
.macro x87_effexp dst, se
    and \dst, \se, 0x7fff
    cmp \dst, 0
    csinc \dst, \dst, wzr, ne
.endm

# Precision control -> the significand bits rounding discards: 40 for single
# (PC 00), 11 for double (10), none for extended (11) and for the reserved 01.
.macro x87_extra dst
    ubfx w0, w15, 8, 2
    mov \dst, 0
    cmp w0, 2
    b.hi .Lx\@
    mov \dst, 11
    b.eq .Lx\@
    mov \dst, 0
    cbnz w0, .Lx\@
    mov \dst, 40
.Lx\@:
.endm

# x8:x9 >>= x12 (>= 1, any size), the bits shifted out leaving a sticky bit 0.
# Clobbers x12-x14.
.macro x87_srs128
    cmp x12, 128
    b.lo .Ls1\@
    orr x9, x8, x9
    cmp x9, 0
    cset x9, ne
    mov x8, 0
    b .Ls9\@
.Ls1\@:
    cmp x12, 64
    b.lo .Ls2\@
    sub x13, x12, 64
    mov x14, -1
    lsl x14, x14, x13
    bic x14, x8, x14
    orr x14, x14, x9
    lsr x9, x8, x13
    mov x8, 0
    cmp x14, 0
    cset x14, ne
    orr x9, x9, x14
    b .Ls9\@
.Ls2\@:
    neg x13, x12
    lsl x14, x9, x13
    lsr x9, x9, x12
    lsl x13, x8, x13
    orr x9, x9, x13
    lsr x8, x8, x12
    cmp x14, 0
    cset x14, ne
    orr x9, x9, x14
.Ls9\@:
.endm

# Round the 128-bit x8:x9 at bit w16 of x8 (the low w16 bits of x8 and all of
# x9 are discarded), in the control word's rounding mode, for sign w11.
# Out: x8 the kept bits (the discarded ones zero), w13 = inexact, w14 = rounded
# away from zero, w12 = the increment carried out of bit 63. x9 is untouched.
.macro x87_rk
    cbz w16, .Lrk0\@
    neg w12, w16
    lsl x13, x8, x12
    cmp x9, 0
    cset x14, ne
    orr x13, x13, x14
    mov x14, -1
    lsl x14, x14, x16
    and x8, x8, x14
    b .Lrk1\@
.Lrk0\@:
    mov x13, x9
.Lrk1\@:
    mov w12, 0
    mov w14, 0
    cbz x13, .Lrk9\@
    ubfx w12, w15, 10, 2
    cbz w12, .Lrkn\@
    cmp w12, 3
    mov w14, 0
    b.eq .Lrkc\@
    cmp w12, 2
    cset w14, eq
    eor w14, w14, w11
    b .Lrkc\@
.Lrkn\@:
    mov x12, 0x8000000000000000
    cmp x13, x12
    cset w14, hi
    b.ne .Lrkc\@
    lsr x14, x8, x16
    and w14, w14, 1
.Lrkc\@:
    mov w13, 1
    lsl x12, x14, x16
    adds x8, x8, x12
    cset w12, cs
.Lrk9\@:
.endm

# ---------------------------------------------------------------------------
# x87_round: the one rounding every arithmetic result goes through.
#
# In: x8:x9 = M (128 bits), the value M * 2^(x10 - bias - 127) -- x10 is the
# exponent M would have with bit 127 set, biased for the TARGET format; w11
# the sign (0/1); w16 the bits to discard (64 - precision); w17 the target's
# largest exponent field (0x7ffe extended, 0x7fe double, 0xfe single).
# Out: x8 the significand left-aligned (the integer bit at 63 for a normal),
# x10 the exponent field (0 for a denormal or zero, w17 + 1 for infinity).
# w7 gets PE, UE, OE and C1 (rounded away from zero).
#
# Tininess is decided after rounding with an unbounded exponent, as the x87
# does; a masked underflow is a tiny result that is also inexact. Masked
# overflow goes to infinity, or to the largest finite value when the rounding
# points back toward zero. Unmasked, an extended (register) result comes back
# with its exponent wrapped by 24576 instead; a narrower one is the masked
# value, which the store then drops.
# Clobbers x0, x5, x9, x12-x14.
# ---------------------------------------------------------------------------
x87_round:
    orr x0, x8, x9
    cbz x0, .Lr_zero
    clz x0, x8
    cbnz x8, 1f
    clz x0, x9
    add x0, x0, 64
1:  cbz x0, 3f
    cmp x0, 64
    b.lo 2f
    sub x12, x0, 64
    lsl x8, x9, x12
    mov x9, 0
    b 3f
2:  neg x12, x0
    lsr x12, x9, x12
    lsl x8, x8, x0
    orr x8, x8, x12
    lsl x9, x9, x0
3:  sub x10, x10, x0
    cmp x10, 0
    b.le .Lr_tiny
    cmp x10, x17
    b.gt .Lr_over
.Lr_round:
    x87_rk
    cbz w12, .Lr_flags
    mov x8, 0x8000000000000000
    add x10, x10, 1
    cmp x10, x17
    b.gt .Lr_over_rounded
.Lr_flags:
    cbz w13, 1f
    orr w7, w7, X87_PE
1:  bfi w7, w14, 9, 1
    ret

.Lr_zero:
    mov x8, 0
    mov x10, 0
    bic w7, w7, X87_C1
    ret

.Lr_tiny:
    tbnz w15, 4, 1f                 // UM: masked
    mov w0, 0x7ffe
    cmp w17, w0
    b.ne .Lr_unmasked_narrow
    // tininess is after rounding, unmasked too: just below the smallest
    // normal and rounding up into it is that normal, no UE
    cbnz x10, 4f
    mov x5, x8
    x87_rk
    mov x8, x5
    cbnz w12, .Lr_round
4:  mov x0, 24576
    add x10, x10, x0
    orr w7, w7, X87_UE
    cmp x10, 0
    b.gt .Lr_round
1:  // tiny, unless just below the smallest normal and rounding carries into it
    mov w0, 1
    cbnz x10, 2f
    mov x5, x8
    x87_rk
    mov x8, x5
    eor w0, w12, 1
2:  mov x12, 1
    sub x12, x12, x10
    x87_srs128
    x87_rk
    lsr x10, x8, 63                 // rounded up into the integer bit: the smallest normal
    cbz w13, 3f
    orr w7, w7, X87_PE
    cbz w0, 3f
    orr w7, w7, X87_UE
3:  bfi w7, w14, 9, 1
    ret

.Lr_over:
    tbnz w15, 3, .Lr_over_masked    // OM: masked
    mov w0, 0x7ffe
    cmp w17, w0
    b.ne .Lr_unmasked_narrow_o
    mov x0, 24576
    sub x10, x10, x0
    orr w7, w7, X87_OE
    cmp x10, x17
    b.le .Lr_round
    // too big even with the bias adjusted: infinity, whatever the rounding
    mov w0, (X87_OE | X87_PE | X87_C1)
    orr w7, w7, w0
    mov x8, 0x8000000000000000
    add x10, x17, 1
    ret
.Lr_over_masked:
    mov w0, (X87_OE | X87_PE)
    orr w7, w7, w0
    ubfx w0, w15, 10, 2
    cmp w0, 3
    b.eq 2f                         // chop: the largest finite value
    cbz w0, 1f                      // nearest: infinity
    cmp w0, 2
    cset w0, eq                     // 1 when rounding up
    cmp w0, w11
    b.eq 2f                         // up and negative, or down and positive
1:  mov x8, 0x8000000000000000
    add x10, x17, 1
    orr w7, w7, X87_C1
    ret
2:  mov x8, -1
    lsl x8, x8, x16
    mov x10, x17
    bic w7, w7, X87_C1
    ret
.Lr_over_rounded:
    tbnz w15, 3, .Lr_over_masked
    mov w0, 0x7ffe
    cmp w17, w0
    b.ne .Lr_unmasked_narrow_o
    mov x0, 24576
    sub x10, x10, x0
    orr w7, w7, X87_OE
    b .Lr_flags

# An unmasked underflow or overflow into a narrower format (a store): the
# flag alone, no PE -- the store is dropped, so the value does not matter.
.Lr_unmasked_narrow:
    orr w7, w7, X87_UE
    mov x8, 0
    mov x10, 0
    ret
.Lr_unmasked_narrow_o:
    orr w7, w7, X87_OE
    mov x8, 0
    mov x10, 0
    ret

# Pack the rounder's result as an extended value: A = x8, w11 << 15 | x10.
.macro x87_pack
    mov x3, x8
    orr w4, w10, w11, lsl 15
.endm

# Round x8:x9 (exponent x10, sign w11) into A at the precision control's width.
.macro x87_round_pc
    x87_extra w16
    mov w17, 0x7ffe
    bl x87_round
    x87_pack
.endm
# ... and at the full 64 bits, whatever precision control says.
.macro x87_round_64
    mov w16, 0
    mov w17, 0x7ffe
    bl x87_round
    x87_pack
.endm

# w12 = class of A, w13 = class of B. Clobbers w0.
x87_cls2:
    x87_class w12, x3, w4, w0
    x87_class w13, x5, w6, w0
    ret

# The NaN an operation on A and B gives, one of them at least a NaN and
# neither unsupported: the larger significand of the NaNs -- which makes a
# QNaN beat an SNaN -- quieted; IE for an SNaN. Classes in w12, w13.
x87_nan2:
    cmp w12, 5
    ccmp w13, 5, 4, ne
    b.ne 1f
    orr w7, w7, X87_IE
1:  cmp w12, 4
    b.lo 3f                         // only B is a NaN
    cmp w13, 4
    b.lo 4f                         // only A
    cmp x5, x3
    b.hi 3f
    b.lo 4f
    // equal significands: the positive one
    tbz w4, 15, 4f
3:  mov x3, x5
    mov w4, w6
4:  orr x3, x3, 0x4000000000000000
    ret

# Shared tails for routines that saved lr: A as it is, the indefinite with IE,
# or the NaN rule.
x87_ret_a:
    ldr lr, [sp], 16
    ret
x87_ret_invalid:
    orr w7, w7, X87_IE
    x87_indef x3, w4
    ldr lr, [sp], 16
    ret
x87_ret_nan:
    bl x87_nan2
    ldr lr, [sp], 16
    ret

# The checks every two-operand arithmetic operation starts with: unsupported
# is invalid, then the NaN rule, then DE for a denormal operand.
.macro x87_check2
    bl x87_cls2
    cmp w12, 6
    ccmp w13, 6, 4, ne
    b.eq x87_ret_invalid
    cmp w12, 4
    ccmp w13, 4, 2, lo
    b.hs x87_ret_nan
    tbnz w7, 16, .Lcd\@
    cmp w12, 2
    ccmp w13, 2, 4, ne
    b.ne .Lck\@
.Lcd\@:
    orr w7, w7, X87_DE
    tbz w15, 1, x87_ret_a           // DE unmasked: no operation at all
.Lck\@:
.endm

# ---------------------------------------------------------------------------
# A = A + B. (Subtraction flips a sign first.)
# ---------------------------------------------------------------------------
x87_add:
    mov w17, 0
x87_add_f:                          // w17: bit 0 A's sign was flipped, bit 1 B's
    str lr, [sp, -16]!
    bl x87_cls2
    cmp w12, 6
    ccmp w13, 6, 4, ne
    b.eq x87_ret_invalid
    cmp w12, 4
    ccmp w13, 4, 2, lo
    b.hs 11f
    tbnz w7, 16, 15f
    cmp w12, 2
    ccmp w13, 2, 4, ne
    b.ne 12f
15: orr w7, w7, X87_DE
    tbz w15, 1, x87_ret_a           // DE unmasked: no operation at all
12: cmp w12, 3
    b.eq 1f
    cmp w13, 3
    b.eq 2f
    orr w0, w12, w13
    cbz w0, 3f
    // effective exponents, the larger in w10 with its operand in A
    x87_effexp w10, w4
    x87_effexp w12, w6
    cmp w10, w12
    b.hs 4f
    mov x0, x3
    mov x3, x5
    mov x5, x0
    mov w0, w4
    mov w4, w6
    mov w6, w0
    mov w0, w10
    mov w10, w12
    mov w12, w0
4:  sub w12, w10, w12
    mov x8, x5
    mov x9, 0
    cbz w12, 5f
    x87_srs128
5:  lsr w11, w4, 15
    eor w0, w4, w6
    tbnz w0, 15, 6f
    // same signs: add, a carry shifting the sum down one with its sticky bit
    adds x8, x3, x8
    b.cc 7f
    and x0, x9, 1
    extr x9, x8, x9, 1
    orr x9, x9, x0
    extr x8, xzr, x8, 1
    orr x8, x8, 0x8000000000000000
    add w10, w10, 1
    b 7f
6:  // opposite signs: the smaller magnitude from the larger
    cmp x3, x8
    b.hi 8f
    b.lo 9f
    cbz x9, 10f                     // exactly equal: zero
9:  sub x8, x8, x3                 // B's lo stays: A's is zero
    lsr w11, w6, 15
    b 7f
8:  negs x9, x9
    sbc x8, x3, x8
7:  sxtw x10, w10
    x87_round_pc
    b x87_ret_a
10: // x - x is +0, or -0 rounding down
    ubfx w0, w15, 10, 2
    cmp w0, 1
    cset w4, eq
    lsl w4, w4, 15
    mov x3, 0
    b x87_ret_a
1:  // A infinite: A, unless B is the opposite infinity
    cmp w13, 3
    b.ne x87_ret_a
    eor w0, w4, w6
    tbnz w0, 15, x87_ret_invalid
    b x87_ret_a
2:  mov x3, x5
    mov w4, w6
    b x87_ret_a
3:  // two zeros: the same sign, or +0 (-0 rounding down)
    eor w0, w4, w6
    tbz w0, 15, x87_ret_a
    b 10b
11: // a NaN keeps the sign it came with: undo a subtraction's flip
    tbz w17, 0, 13f
    eor w4, w4, 0x8000
13: tbz w17, 1, 14f
    eor w6, w6, 0x8000
14: b x87_ret_nan

x87_sub:
    eor w6, w6, 0x8000
    mov w17, 2
    b x87_add_f
x87_subr:
    eor w4, w4, 0x8000
    mov w17, 1
    b x87_add_f

# ---------------------------------------------------------------------------
# A = A * B
# ---------------------------------------------------------------------------
x87_mul:
    str lr, [sp, -16]!
    x87_check2
    eor w11, w4, w6
    ubfx w11, w11, 15, 1
    cmp w12, 3
    b.eq 1f
    cmp w13, 3
    b.eq 2f
    cmp w12, 0
    ccmp w13, 0, 4, ne
    b.eq 3f
    x87_effexp w10, w4
    x87_effexp w12, w6
    add w10, w10, w12
    mov w12, 16382
    sub w10, w10, w12               // ea + eb - 16383 + 1
    mul x9, x3, x5
    umulh x8, x3, x5
    sxtw x10, w10
    x87_round_pc
    b x87_ret_a
1:  cbz w13, x87_ret_invalid        // infinity * 0
    b 4f
2:  cbz w12, x87_ret_invalid
4:  mov x3, 0x8000000000000000
    mov w4, 0x7fff
    orr w4, w4, w11, lsl 15
    b x87_ret_a
3:  mov x3, 0
    lsl w4, w11, 15
    b x87_ret_a

# (x12:x13) / x5, x5 normalized and x12 < x5: quotient x12, remainder x13.
# Hacker's Delight's divlu. Clobbers x0, x8-x11, x14, x16, x17.
x87_divlu:
    lsr x0, x5, 32
    and x8, x5, 0xffffffff
    lsr x9, x13, 32
    and x16, x13, 0xffffffff
    udiv x17, x12, x0
    msub x14, x17, x0, x12
1:  lsr x10, x17, 32
    cbnz x10, 2f
    mul x10, x17, x8
    orr x11, x9, x14, lsl 32
    cmp x10, x11
    b.ls 3f
2:  sub x17, x17, 1
    add x14, x14, x0
    lsr x10, x14, 32
    cbz x10, 1b
3:  orr x11, x9, x12, lsl 32
    msub x13, x17, x5, x11
    udiv x12, x13, x0
    msub x14, x12, x0, x13
4:  lsr x10, x12, 32
    cbnz x10, 5f
    mul x10, x12, x8
    orr x11, x16, x14, lsl 32
    cmp x10, x11
    b.ls 6f
5:  sub x12, x12, 1
    add x14, x14, x0
    lsr x10, x14, 32
    cbz x10, 4b
6:  orr x11, x16, x13, lsl 32
    msub x13, x12, x5, x11
    orr x12, x12, x17, lsl 32
    ret

# Normalize a finite nonzero significand \m with exponent field \se: \m
# shifted up to bit 63, \e (a w register) its effective exponent less the
# shift. Clobbers w0.
.macro x87_norm m, se, e
    x87_effexp \e, \se
    clz x0, \m
    lsl \m, \m, x0
    sub \e, \e, w0
.endm

# ---------------------------------------------------------------------------
# A = A / B
# ---------------------------------------------------------------------------
x87_div:
    str lr, [sp, -16]!
    bl x87_cls2
    cmp w12, 6
    ccmp w13, 6, 4, ne
    b.eq x87_ret_invalid
    cmp w12, 4
    ccmp w13, 4, 2, lo
    b.hs x87_ret_nan
    // a finite nonzero over zero is ZE, and that before any denormal
    cbnz w13, 21f
    cmp w12, 0
    ccmp w12, 3, 4, ne
    eor w11, w4, w6
    ubfx w11, w11, 15, 1
    b.ne 2f
21: tbnz w7, 16, 22f
    cmp w12, 2
    ccmp w13, 2, 4, ne
    b.ne 23f
22: orr w7, w7, X87_DE
    tbz w15, 1, x87_ret_a           // DE unmasked: no operation at all
23: eor w11, w4, w6
    ubfx w11, w11, 15, 1
    cmp w12, 3
    b.eq 1f
    cmp w13, 3
    b.eq 3f                         // finite / infinity: zero
    cbz w13, 2f                     // finite / 0
    cbz w12, 3f                     // 0 / finite: zero
    x87_norm x3, w4, w10
    x87_norm x5, w6, w12
    sub w10, w10, w12
    mov w12, 16383
    add w10, w10, w12
    sxtw x10, w10
    stp x10, x11, [sp, -16]!
    lsr x12, x3, 1
    lsl x13, x3, 63
    bl x87_divlu
    mov x6, x12
    mov x12, x13
    mov x13, 0
    bl x87_divlu
    cmp x13, 0
    cset x0, ne
    orr x9, x12, x0
    mov x8, x6
    ldp x10, x11, [sp], 16
    x87_round_pc
    b x87_ret_a
1:  cmp w13, 3
    b.eq x87_ret_invalid            // infinity / infinity
    b 4f
2:  cbz w12, x87_ret_invalid        // 0 / 0
    bic w7, w7, X87_DE              // a division by zero is ZE alone
    bic w7, w7, X87_DEP
    orr w7, w7, X87_ZE
4:  mov x3, 0x8000000000000000
    mov w4, 0x7fff
    orr w4, w4, w11, lsl 15
    b x87_ret_a
3:  mov x3, 0
    lsl w4, w11, 15
    b x87_ret_a

x87_divr:
    mov x0, x3
    mov x3, x5
    mov x5, x0
    mov w0, w4
    mov w4, w6
    mov w6, w0
    b x87_div

# ---------------------------------------------------------------------------
# A = sqrt(A). A negative operand (but -0) is invalid, and that before any
# denormal is noticed. C1 says the root rounded up, as for any arithmetic.
# ---------------------------------------------------------------------------
x87_sqrt:
    str lr, [sp, -16]!
    x87_class w12, x3, w4, w0
    cmp w12, 6
    b.eq x87_ret_invalid
    cmp w12, 4
    b.hs 1f
    cbz w12, x87_ret_a              // a zero: itself
    tbnz w4, 15, x87_ret_invalid    // negative
    cmp w12, 2
    b.ne 2f
    orr w7, w7, X87_DE
    tbz w15, 1, x87_ret_a
2:  cmp w12, 3
    b.eq x87_ret_a                  // +infinity
    x87_norm x3, w4, w10
    // the value is m * 2^t, t = e - 16446; take N = m << 64 or << 63 so the
    // exponent left over is even
    mov w0, 16446
    sub w10, w10, w0
    mov x9, 0
    mov x8, x3
    tbz w10, 0, 3f
    lsl x9, x3, 63
    lsr x8, x3, 1
    add w10, w10, 1
3:  sub w10, w10, 64
    // r = isqrt(N), N = x8:x9 in [2^126, 2^128): a double's root for 50-odd
    // bits, one Newton step (r + N / r) / 2 for the rest, then exact to +-1
    // by squaring. The host FPSR is put back, so its flags stay the guest's.
    stp x10, xzr, [sp, -16]!
    mrs x11, fpsr
    ucvtf d0, x8
    fsqrt d0, d0
    mov x12, 0x41f0000000000000     // 2^32
    fmov d1, x12
    fmul d0, d0, d1
    fcvtzu x14, d0                  // the estimate, near 2^63..2^64
    msr fpsr, x11
    orr x14, x14, 0x8000000000000000
    // N's top half all ones: the root is 2^64 - 1, and no 64-bit divisor is
    // above that half (x8 + 1 wrapped to 0, and the +-1 fixup below then
    // started 2^63 away -- a hang inside the gadget, which nothing can kill)
    cmn x8, 1
    b.eq 3f
    cmp x14, x8
    csinc x14, x14, x8, hi          // divlu wants the divisor above N's top half
    stp x8, x9, [sp, -16]!
    mov x16, x5                     // divlu divides by x5
    mov x5, x14
    mov x12, x8
    mov x13, x9
    mov x17, x14
    stp x16, x17, [sp, -16]!
    bl x87_divlu                    // x12 = N / r
    ldp x16, x17, [sp], 16
    mov x5, x16
    adds x14, x17, x12              // (r + N / r) / 2, 65 bits
    cset x0, cs
    extr x14, x0, x14, 1
    ldp x8, x9, [sp], 16
    b 4f
3:  mov x14, -1
    // too big: down; too small: up
4:  mul x13, x14, x14
    umulh x12, x14, x14
    cmp x9, x13
    sbcs xzr, x8, x12               // N >= r^2 ?
    b.hs 5f
    sub x14, x14, 1
    b 4b
5:  adds x16, x14, 1                // (r + 1)^2 > N ?
    b.cs 6f
    mul x0, x16, x16
    umulh x17, x16, x16
    cmp x9, x0
    sbcs xzr, x8, x17
    b.lo 6f
    mov x14, x16
    mov x12, x17
    mov x13, x0
    b 5b
6:  // the remainder N - r^2
    subs x9, x9, x13
    sbc x8, x8, x12
    mov x13, x14
    ldp x10, xzr, [sp], 16
    // the bits past the root: above a half when the remainder exceeds the
    // root, never exactly a half, nonzero when the remainder is
    mov x0, 0
    cmp x8, 0
    ccmp x9, x13, 2, eq             // remainder > root?
    b.hi 7f
    orr x17, x8, x9
    cmp x17, 0
    cset x0, ne
    b 8f
7:  mov x0, 0xc000000000000000
8:  mov x8, x13
    mov x9, x0
    asr w10, w10, 1                 // root * 2^(t'/2) = M * 2^(t'/2 - 64)
    mov w0, 16446
    add w10, w10, w0
    sxtw x10, w10
    mov w11, 0
    x87_round_pc
    b x87_ret_a
1:  // a NaN: quieted, IE for an SNaN
    cmp w12, 5
    b.ne 9f
    orr w7, w7, X87_IE
9:  orr x3, x3, 0x4000000000000000
    b x87_ret_a

# ---------------------------------------------------------------------------
# Compare A with B. w0 in: 1 signalling (any NaN invalid: FCOM, FCOMI, FTST),
# 0 quiet (only an SNaN: FUCOM, FUCOMI). w0 out: 0 greater, 1 less, 2 equal,
# 3 unordered. Raises IE and DE.
# ---------------------------------------------------------------------------
x87_cmp:
    mov w8, w0
    x87_class w12, x3, w4, w0
    x87_class w13, x5, w6, w0
    cmp w12, 6
    ccmp w13, 6, 4, ne
    b.eq 9f
    cmp w12, 4
    ccmp w13, 4, 2, lo
    b.hs 8f
    tbnz w7, 16, 10f
    cmp w12, 2
    ccmp w13, 2, 4, ne
    b.ne 1f
10: orr w7, w7, X87_DE              // (unmasked, the compare still sets its codes)
1:  orr w0, w12, w13
    cbz w0, 6f                      // two zeros: equal
    eor w0, w4, w6
    tbz w0, 15, 2f
    // opposite signs: the positive one is greater
    tbnz w4, 15, 5f
    b 4f
2:  x87_effexp w9, w4
    x87_effexp w10, w6
    cbnz w12, 21f
    mov w9, 0                       // a zero is below every denormal
21: cbnz w13, 22f
    mov w10, 0
22: cmp w9, w10
    b.ne 3f
    cmp x3, x5
    b.eq 6f
3:  // b.hi: |A| > |B|
    cset w0, hi
    tbz w4, 15, 31f
    eor w0, w0, 1                   // both negative
31: cbnz w0, 4f
5:  mov w0, 1
    ret
4:  mov w0, 0
    ret
6:  mov w0, 2
    ret
8:  // a NaN: unordered; invalid if it signals, or if this compare does
    cmp w12, 5
    ccmp w13, 5, 4, ne
    cset w0, eq
    orr w0, w0, w8
    cbz w0, 7f
9:  orr w7, w7, X87_IE
7:  mov w0, 3
    ret

# ---------------------------------------------------------------------------
# Conversions into B (x5/w6), exact. A float's denormal raises DE; its SNaN
# stays signalling, for the operation to find (FLD quiets it itself).
# ---------------------------------------------------------------------------
x87_ld_f32:                         // w0
    ubfx w8, w0, 23, 8
    and w9, w0, 0x7fffff
    lsr w6, w0, 31
    lsl w6, w6, 15
    cbz w8, 2f
    lsl x5, x9, 40
    orr x5, x5, 0x8000000000000000
    cmp w8, 0xff
    b.eq 1f
    mov w0, 16256
    add w8, w8, w0
    orr w6, w6, w8
    ret
1:  mov w8, 0x7fff
    orr w6, w6, w8
    ret
2:  mov x5, 0
    cbz w9, 3f
    orr w7, w7, X87_DEP
    lsl x5, x9, 40
    clz x0, x5
    lsl x5, x5, x0
    mov w8, 16257
    sub w8, w8, w0
    orr w6, w6, w8
3:  ret

x87_ld_f64:                         // x0
    ubfx x8, x0, 52, 11
    and x9, x0, 0xfffffffffffff
    lsr x6, x0, 63
    lsl w6, w6, 15
    cbz w8, 2f
    lsl x5, x9, 11
    orr x5, x5, 0x8000000000000000
    cmp w8, 0x7ff
    b.eq 1f
    mov w0, 15360
    add w8, w8, w0
    orr w6, w6, w8
    ret
1:  mov w8, 0x7fff
    orr w6, w6, w8
    ret
2:  mov x5, 0
    cbz x9, 3f
    orr w7, w7, X87_DEP
    lsl x5, x9, 11
    clz x0, x5
    lsl x5, x5, x0
    mov w8, 15361
    sub w8, w8, w0
    orr w6, w6, w8
3:  ret

x87_ld_int:                         // x0, signed
    mov x5, 0
    mov w6, 0
    cbz x0, 1f
    lsr x8, x0, 63
    lsl w6, w8, 15
    cmp x0, 0
    cneg x0, x0, lt
    clz x8, x0
    lsl x5, x0, x8
    mov w9, 16446
    sub w9, w9, w8
    orr w6, w6, w9
1:  ret

# FBLD's packed decimal (x0 the low 16 digits, w9 the high two and the sign
# byte) into B: exact, -0 kept. Each digit is taken at its value, as hardware
# reads the invalid ones too.
x87_ld_bcd:
    mov x10, 0
    ubfx w8, w9, 4, 4
    mov x11, 10
    madd x10, x10, x11, x8
    and w8, w9, 0xf
    madd x10, x10, x11, x8
    mov w12, 60
1:  lsr x8, x0, x12
    and x8, x8, 0xf
    madd x10, x10, x11, x8
    subs w12, w12, 4
    b.ge 1b
    mov x5, 0
    ubfx w6, w9, 15, 1
    lsl w6, w6, 15
    cbz x10, 2f
    clz x8, x10
    lsl x5, x10, x8
    mov w11, 16446
    sub w11, w11, w8
    orr w6, w6, w11
2:  ret

# ---------------------------------------------------------------------------
# Conversions out of A, rounding in the control word's mode.
# ---------------------------------------------------------------------------

# A -> an IEEE single (w0) or double (x0): w16 = 40 or 11, w17 = 0xfe or 0x7fe.
# NaNs keep their payload's top bits and are quieted (IE for an SNaN); an
# unsupported value is invalid and gives the default NaN. A denormal source
# is no DE here (a store has no #D). Clobbers x5, x8-x14.
x87_to_ieee:
    str lr, [sp, -16]!
    x87_class w12, x3, w4, w0
    lsr w11, w4, 15
    cmp w12, 6
    b.eq 5f
    cmp w12, 3
    b.eq 3f
    b.hi 4f
    cbz w12, 2f
    x87_effexp w10, w4
    mov x8, x3
    mov x9, 0
    // rebias: extended -> the target's
    mov w0, 16383 - 127
    mov w5, 16383 - 1023
    cmp w17, 0xfe
    csel w0, w0, w5, eq
    sub w10, w10, w0
    sxtw x10, w10
    bl x87_round
    b 6f
2:  mov x8, 0
    mov x10, 0
    b 6f
3:  mov x8, 0
    add x10, x17, 1
    b 6f
4:  // a NaN
    cmp w12, 5
    b.ne 41f
    orr w7, w7, X87_IE
41: orr x8, x3, 0x4000000000000000
    add x10, x17, 1
    mov x9, -1
    lsl x9, x9, x16
    and x8, x8, x9
    b 6f
5:  orr w7, w7, X87_IE
    mov w11, 1
    mov x8, 0xc000000000000000
    add x10, x17, 1
6:  // pack: sign, exponent field, the fraction below the integer bit
    lsl x8, x8, 1
    lsr x8, x8, 1
    lsr x8, x8, x16
    cmp w17, 0xfe
    b.ne 7f
    orr w0, w8, w10, lsl 23
    orr w0, w0, w11, lsl 31
    ldr lr, [sp], 16
    ret
7:  orr x0, x8, x10, lsl 52
    orr x0, x0, x11, lsl 63
    ldr lr, [sp], 16
    ret

# A -> a w0-bit signed integer (16, 32 or 64) in x0. Out of range, a NaN, an
# infinity or an unsupported value is invalid and gives the integer
# indefinite (the most negative value), with nothing said about rounding.
# Clobbers x5, x8-x14, x16.
x87_to_int:
    str lr, [sp, -16]!
    mov w5, w0
    x87_class w12, x3, w4, w0
    lsr w11, w4, 15
    cmp w12, 3
    b.hs 9f
    mov x0, 0
    cbz w12, 8f
    x87_effexp w10, w4
    mov w0, 16383 + 63
    sub w12, w0, w10                // the shift that leaves the units at bit 0 of x8
    cmp w12, 0
    b.lt 9f
    mov x8, x3
    mov x9, 0
    cbz w12, 1f
    sxtw x12, w12
    x87_srs128
1:  mov w16, 0
    x87_rk
    cbnz w12, 9f                    // rounded past 2^64
    sub w0, w5, 1
    mov x9, 1
    lsl x9, x9, x0                  // 2^(width - 1)
    sub x9, x9, 1
    add x9, x9, x11                 // the largest magnitude this sign holds
    cmp x8, x9
    b.hi 9f
    cbz w13, 2f
    orr w7, w7, X87_PE
2:  bfi w7, w14, 9, 1
    cmp w11, 0
    cneg x0, x8, ne
8:  ldr lr, [sp], 16
    ret
9:  orr w7, w7, X87_IE
    mov w8, (X87_PE | X87_C1)
    bic w7, w7, w8
    sub w0, w5, 1
    mov x8, 1
    lsl x0, x8, x0
    neg x0, x0
    ldr lr, [sp], 16
    ret

# A -> 18-digit packed decimal: x0 the low 16 digits, w9 the high two and
# the sign byte. Out of range or not a number: invalid, and the decimal
# indefinite (FFFF C000 0000 0000 0000).
x87_to_bcd:
    str lr, [sp, -16]!
    mov w0, 64
    bl x87_to_int
    tbnz w7, 0, 9f                  // invalid (IE could only come from here)
    cmp x0, 0
    cneg x10, x0, lt
    mov x11, 0xffff                 // 999999999999999999 = 0x0de0b6b3a763ffff
    movk x11, 0xa763, lsl 16
    movk x11, 0xb6b3, lsl 32
    movk x11, 0x0de0, lsl 48
    cmp x10, x11
    b.hi 8f
    // digits, low first
    mov x0, 0
    mov x13, 10
    mov w12, 0
1:  udiv x14, x10, x13
    msub x8, x14, x13, x10
    lsl x8, x8, x12
    orr x0, x0, x8
    mov x10, x14
    add w12, w12, 4
    cmp w12, 64
    b.lo 1b
    udiv x14, x10, x13
    msub x8, x14, x13, x10          // digit 16
    and w9, w8, 0xf
    udiv x10, x14, x13
    msub x8, x10, x13, x14          // digit 17
    orr w9, w9, w8, lsl 4
    lsr w11, w4, 15                 // the sign (x11 held the bound)
    orr w9, w9, w11, lsl 15
    ldr lr, [sp], 16
    ret
8:  orr w7, w7, X87_IE
    mov w8, (X87_PE | X87_C1)
    bic w7, w7, w8
9:  mov x0, 0xc000000000000000
    mov w9, 0xffff
    ldr lr, [sp], 16
    ret

# ---------------------------------------------------------------------------
# FRNDINT: A rounded to an integer in the rounding mode, at 64 bits.
# ---------------------------------------------------------------------------
x87_rndint:
    str lr, [sp, -16]!
    x87_class w12, x3, w4, w0
    cmp w12, 6
    b.eq x87_ret_invalid
    cmp w12, 4
    b.hs 7f
    cmp w12, 3
    b.eq x87_ret_a
    cbz w12, x87_ret_a
    cmp w12, 2
    b.ne 1f
    orr w7, w7, X87_DE
    tbz w15, 1, x87_ret_a
1:  x87_effexp w10, w4
    mov w0, 16383 + 63
    subs w12, w0, w10
    b.le x87_ret_a                  // no fraction bits
    lsr w11, w4, 15
    mov x8, x3
    mov x9, 0
    sxtw x12, w12
    mov x5, x12
    x87_srs128
    mov w16, 0
    x87_rk
    cbz w13, 2f
    orr w7, w7, X87_PE
2:  bfi w7, w14, 9, 1
    lsl w4, w11, 15
    // the integer, renormalized (a carry out of bit 63 is 2^64)
    cbz w12, 3f
    mov x3, 0x8000000000000000
    mov w0, 16383 + 64
    orr w4, w4, w0
    b x87_ret_a
3:  mov x3, 0
    cbz x8, x87_ret_a
    clz x0, x8
    lsl x3, x8, x0
    mov w9, 16383 + 63
    sub w9, w9, w0
    orr w4, w4, w9
    b x87_ret_a
7:  cmp w12, 5
    b.ne 8f
    orr w7, w7, X87_IE
8:  orr x3, x3, 0x4000000000000000
    b x87_ret_a

# ---------------------------------------------------------------------------
# FSCALE: A * 2^trunc(B), rounded at 64 bits whatever precision control says.
# ---------------------------------------------------------------------------
x87_scale:
    str lr, [sp, -16]!
    x87_check2
    cmp w13, 3
    b.eq 5f
    cmp w12, 3
    b.eq x87_ret_a
    cbz w12, x87_ret_a
    // n = trunc(B), held to +-65536 (anything past that over- or underflows)
    mov x9, 0
    cbz w13, 2f
    x87_effexp w10, w6
    mov w0, 16383
    subs w10, w10, w0               // B's unbiased exponent
    b.lt 2f
    mov x9, 65536
    cmp w10, 16
    b.gt 1f
    mov w0, 63
    sub w0, w0, w10
    lsr x9, x5, x0
1:  tbz w6, 15, 2f
    neg x9, x9
2:  x87_effexp w10, w4
    sxtw x10, w10
    add x10, x10, x9
    lsr w11, w4, 15
    mov x8, x3
    mov x9, 0
    x87_round_64
    b x87_ret_a
5:  // B infinite: A to infinity or zero, but 0 * 2^inf and inf * 2^-inf are invalid
    tbnz w6, 15, 6f
    cbz w12, x87_ret_invalid
    and w4, w4, 0x8000
    mov w0, 0x7fff
    orr w4, w4, w0
    mov x3, 0x8000000000000000
    b x87_ret_a
6:  cmp w12, 3
    b.eq x87_ret_invalid
    and w4, w4, 0x8000
    mov x3, 0
    b x87_ret_a

