#include "gadgets.h"
#include "amd64-mem.h"
#include "emu/interrupt.h"

# The x87, for the i386 and amd64 guests alike: the 80-bit arithmetic, the
# register stack and its tags, the status and control words, and every D8-DF
# instruction but the transcendentals -- here, in integer arithmetic, rather
# than in C (emu/fpu.c and emu/float80.c are the interpreter's).
#
# State: cpu->fp[8] (16 bytes each: the 64-bit significand, then sign and
# exponent), TOP in fsw bits 11-13, the abridged tag in cpu->x87_valid (bit i
# set when PHYSICAL register i holds a value), fcw. ST(i) is fp[(TOP + i) & 7].
#
# The core routines below share a convention, so the register-form gadgets
# can be the same code for both guests and the memory forms can differ only in
# how they reach memory:
#
#   A = x3 (significand) / w4 (sign << 15 | exponent): the first operand and
#       the result. B = x5 / w6: the second operand.
#   w7: what the operation raised, in status-word bits -- IE DE ZE OE UE PE SF
#       (0-6) -- plus C1 at bit 9 and C0 C2 C3 (bits 8, 10, 14) for compares.
#   w15: the control word.
#   x0, x5, x6, x8-x14, x16, x17 are scratch. x1 (cpu), x2 (tlb), x3, x4, x7,
#   x15, x19 and x20-x28 (the guests' registers) are preserved; so is the
#   stack, which nothing leaves anything on across a possible fault exit.
#
# The answers are real hardware's: tests/manual/x86/x87_kat.c holds 122880 of
# them, from an AMD Ryzen.

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
    mov x0, 24576
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

# ---------------------------------------------------------------------------
# Stack access. x87_slot: x9 = &ST(w0) for TOP in w8. Clobbers w0.
# ---------------------------------------------------------------------------
.macro x87_slot dst, idx, top
    add \idx, \idx, \top
    and \idx, \idx, 7
    add \dst, _cpu, CPU_fp
    add \dst, \dst, \idx, uxtw 4
.endm

# The start of every operation: the control word, and nothing raised yet.
.macro x87_begin
    ldrh w15, [_cpu, CPU_fcw]
    mov w7, 0
.endm

# A = ST(w0), B = ST(w10): both must hold values, else stack underflow --
# IE and SF, C1 clear, and the indefinite for a result. w0 out: 0, or 1 on
# underflow. Clobbers x8-x13.
x87_get2:
    ldrh w8, [_cpu, CPU_fsw]
    ldrb w9, [_cpu, CPU_x87_valid]
    ubfx w8, w8, 11, 3
    add w0, w0, w8
    and w0, w0, 7
    add w10, w10, w8
    and w10, w10, 7
    add x12, _cpu, CPU_fp
    add x13, x12, x10, lsl 4
    add x12, x12, x0, lsl 4
    ldr x3, [x12]
    ldrh w4, [x12, 8]
    ldr x5, [x13]
    ldrh w6, [x13, 8]
    lsr w0, w9, w0
    lsr w10, w9, w10
    and w0, w0, w10
    tbz w0, 0, x87_underflow
    mov w0, 0
    ret
x87_underflow:
    mov w0, (X87_C1 | X87_DE | X87_PE | X87_UE | X87_OE | X87_ZE)
    bic w7, w7, w0
    mov w0, (X87_IE | X87_SF)
    orr w7, w7, w0
    x87_indef x3, w4
    mov w0, 1
    ret

# A = ST(w0) (one operand). Same contract.
x87_get1:
    ldrh w8, [_cpu, CPU_fsw]
    ldrb w9, [_cpu, CPU_x87_valid]
    ubfx w8, w8, 11, 3
    add w0, w0, w8
    and w0, w0, 7
    add x12, _cpu, CPU_fp
    add x12, x12, x0, lsl 4
    ldr x3, [x12]
    ldrh w4, [x12, 8]
    lsr w0, w9, w0
    tbz w0, 0, x87_underflow
    mov w0, 0
    ret

# The status word after an operation: fsw w8 gains the exceptions in w7 and
# takes C1 from it; ES and B follow any unmasked exception. Stored.
x87_put_fsw:
    and w10, w7, 0x7f
    orr w8, w8, w10
    ubfx w10, w7, 9, 1
    bfi w8, w10, 9, 1
    and w10, w8, 0x3f
    bic w10, w10, w15
    cbz w10, 1f
    mov w10, 0x8080
    orr w8, w8, w10
1:  strh w8, [_cpu, CPU_fsw]
    ret

# ST(w0 bits 3-5) = A, then pop if bit 6, then the status word -- unless an
# unmasked IE, DE or ZE, which leave the registers and TOP as they were.
x87_put:
    ldrh w8, [_cpu, CPU_fsw]
    bic w10, w7, w15
    tst w10, (X87_IE | X87_DE | X87_ZE)
    b.ne x87_put_fsw
    ubfx w9, w8, 11, 3
    ubfx w10, w0, 3, 3
    x87_slot x11, w10, w9
    str x3, [x11]
    strh w4, [x11, 8]
    ldrb w11, [_cpu, CPU_x87_valid]
    mov w12, 1
    lsl w12, w12, w10
    orr w11, w11, w12
    tbz w0, 6, 1f
    mov w12, 1
    lsl w12, w12, w9
    bic w11, w11, w12
    add w9, w9, 1
    bfi w8, w9, 11, 3
1:  strb w11, [_cpu, CPU_x87_valid]
    b x87_put_fsw

# Pop w0 registers (0-2), then the status word with the compare's condition
# codes: C0 C2 C3 from the compare's result in w12 (0 >, 1 <, 2 =, 3
# unordered), C1 clear. An unmasked IE or DE still sets the codes, but pops
# nothing (an AMD Ryzen).
x87_put_cc:
    ldrh w8, [_cpu, CPU_fsw]
    bic w10, w7, w15
    tst w10, (X87_IE | X87_DE)      // unmasked: the codes, but no pops
    b.eq 6f
    mov w0, 0
6:  mov w10, (X87_C0 | X87_C2 | X87_C3)
    cmp w12, 1
    b.hi 1f
    mov w10, X87_C0
    b.eq 1f
    mov w10, 0
1:  cmp w12, 2
    b.ne 2f
    mov w10, X87_C3
2:  mov w11, X87_CC
    bic w8, w8, w11
    orr w8, w8, w10
    ldrb w11, [_cpu, CPU_x87_valid]
    ubfx w9, w8, 11, 3
    cbz w0, 4f
5:  mov w12, 1
    lsl w12, w12, w9
    bic w11, w11, w12
    add w9, w9, 1
    and w9, w9, 7
    subs w0, w0, 1
    b.ne 5b
    bfi w8, w9, 11, 3
    strb w11, [_cpu, CPU_x87_valid]
4:  bic w7, w7, X87_C1
    b x87_put_fsw

# Push A: the register below TOP must be empty, else stack overflow -- IE and
# SF, C1 set, the indefinite pushed. An unmasked IE (overflow or the load's)
# pushes nothing; a load's DE, unmasked, still loads.
x87_push:
    ldrh w8, [_cpu, CPU_fsw]
    ldrb w11, [_cpu, CPU_x87_valid]
    ubfx w9, w8, 11, 3
    sub w9, w9, 1
    and w9, w9, 7
    lsr w10, w11, w9
    tbz w10, 0, 1f
    mov w7, (X87_IE | X87_SF | X87_C1)
    x87_indef x3, w4
1:  bic w10, w7, w15
    tbnz w10, 0, x87_put_fsw
    add x10, _cpu, CPU_fp
    add x10, x10, w9, uxtw 4
    str x3, [x10]
    strh w4, [x10, 8]
    mov w10, 1
    lsl w10, w10, w9
    orr w11, w11, w10
    strb w11, [_cpu, CPU_x87_valid]
    bfi w8, w9, 11, 3
    b x87_put_fsw

# ---------------------------------------------------------------------------
# #MF. Every x87 instruction but FNINIT, FNCLEX, FNSTSW, FNSTCW, FNSTENV,
# FNSAVE, FXSAVE and FXRSTOR waits: with ES set -- an unmasked exception an
# earlier instruction left pending -- it faults before it runs, INT_MF at
# that instruction (kernel/calls.c makes it Linux's SIGFPE).
#
# A register form's word is [imm | flags << 8 | ip << 16]: bit 8, the amd64
# register cache is live (store it); bit 9, the guest is amd64; ip, the
# instruction. A memory form has its instruction in the stream already
# (i386's [orig_ip]) or in cpu_state (amd64's rip, flushed).
# ---------------------------------------------------------------------------
.macro x87_wait
    ldrh w8, [_cpu, CPU_fsw]
    tbz w8, 7, .Lw\@
    b x87_mf_rr                     // (out of a tbnz's reach from most gadgets)
.Lw\@:
.endm
.macro x87m_wait arch
    ldrh w8, [_cpu, CPU_fsw]
    tbz w8, 7, .Lw\@
    .ifc \arch,i386
        b x87_mf_m_i386
    .else
        b x87_mf_m_amd64
    .endif
.Lw\@:
.endm
x87_mf_rr:
    ldr x8, [_ip]
    tbz x8, 8, 1f
    add x9, _cpu, CPU_amd64_regs
    str x20, [x9, 0]
    str x22, [x9, 8]
    str x23, [x9, 16]
    str x21, [x9, 24]
    str x27, [x9, 32]
    str x26, [x9, 40]
    str x24, [x9, 48]
    str x25, [x9, 56]
1:  lsr x9, x8, 16
    tbz x8, 9, 2f
    str x9, [_cpu, CPU_amd64_rip]
2:  mov eip, w9
    mov x0, INT_MF
    b jit_exit
x87_mf_m_i386:
    ldr x8, [_ip]
    mov eip, w8
    mov x0, INT_MF
    b jit_exit
x87_mf_m_amd64:
    mov x0, INT_MF
    b jit_exit

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
# IE still sets them (unordered) but does not pop.
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
    mov w9, ZF_FLAG
21: cmp w12, 3
    b.ne 22f
    mov w9, (ZF_FLAG | PF_FLAG | 1)
22: ldr w10, [_cpu, CPU_eflags]
    mov w11, (ZF_FLAG | PF_FLAG | SF_FLAG | AF_FLAG | 1)
    orr w11, w11, (1 << 11)
    bic w10, w10, w11
    orr w10, w10, w9
    str w10, [_cpu, CPU_eflags]
    and w9, w9, 1
    strb w9, [_cpu, CPU_cf]
    strb wzr, [_cpu, CPU_of]
    strb wzr, [_cpu, CPU_flags_res]
3:  ldr w0, [_ip]
    ubfx w0, w0, 6, 1
    // pop (the condition codes stay as they were)
    ldrh w8, [_cpu, CPU_fsw]
    cbz w0, 4f
    bic w10, w7, w15
    tst w10, (X87_IE | X87_DE)
    b.ne 4f
    ubfx w9, w8, 11, 3
    ldrb w11, [_cpu, CPU_x87_valid]
    mov w12, 1
    lsl w12, w12, w9
    bic w11, w11, w12
    strb w11, [_cpu, CPU_x87_valid]
    add w9, w9, 1
    bfi w8, w9, 11, 3
4:  bic w7, w7, X87_C1
    bl x87_put_fsw
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
    ldrh w8, [_cpu, CPU_fsw]
    ldrb w9, [_cpu, CPU_x87_valid]
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
    strb w9, [_cpu, CPU_x87_valid]
3:  ldrh w8, [_cpu, CPU_fsw]
    bl x87_put_fsw
    gret_nomem 1

# [i | pop << 6]: FFREE/FFREEP ST(i): tag it empty, then pop.
.gadget x87_ffree
    x87_wait
    ldrh w8, [_cpu, CPU_fsw]
    ldrb w11, [_cpu, CPU_x87_valid]
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
    strh w8, [_cpu, CPU_fsw]
1:  strb w11, [_cpu, CPU_x87_valid]
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
    ldrh w8, [_cpu, CPU_fsw]
    ldrb w9, [_cpu, CPU_x87_valid]
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
    strh w8, [_cpu, CPU_fsw]
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
    ldrh w8, [_cpu, CPU_fsw]
    ubfx w9, w8, 11, 3
    add w9, w9, 1
    bfi w8, w9, 11, 3
    bic w8, w8, X87_C1
    strh w8, [_cpu, CPU_fsw]
    gret_nomem 1
.gadget x87_fdecstp
    x87_wait
    ldrh w8, [_cpu, CPU_fsw]
    ubfx w9, w8, 11, 3
    sub w9, w9, 1
    bfi w8, w9, 11, 3
    bic w8, w8, X87_C1
    strh w8, [_cpu, CPU_fsw]
    gret_nomem 1

# FNCLEX: the exception flags, SF, ES and B. FNINIT: control word 037F,
# status 0 (TOP 0), every register empty.
.gadget x87_fnclex
    ldrh w8, [_cpu, CPU_fsw]
    and w8, w8, 0x7f00
    strh w8, [_cpu, CPU_fsw]
    gret_nomem 1
.gadget x87_fninit
    mov w8, 0x37f
    strh w8, [_cpu, CPU_fcw]
    strh wzr, [_cpu, CPU_fsw]
    strb wzr, [_cpu, CPU_x87_valid]
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
    ldrh w8, [_cpu, CPU_fsw]
    ldrb w11, [_cpu, CPU_x87_valid]
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
    ldrb w11, [_cpu, CPU_x87_valid]
    mov w10, 1
    lsl w10, w10, w9
    orr w11, w11, w10
    strb w11, [_cpu, CPU_x87_valid]
    bl x87_push
    gret_nomem 1
10: ldrh w8, [_cpu, CPU_fsw]
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
1:  ldrh w8, [_cpu, CPU_fsw]
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
    ldrb w11, [_cpu, CPU_x87_valid]
    mov w10, 1
    lsl w10, w10, w9
    orr w11, w11, w10
    strb w11, [_cpu, CPU_x87_valid]
2:  bl x87_put_fsw
    gret_nomem 1
.endm
x87_prem_g fprem, 0
x87_prem_g fprem1, 1

# ---------------------------------------------------------------------------
# FCMOVcc ST(0), ST(i) [i]. The i386 guest's flags may still be lazy (in
# CPU_res, per flags_res); the amd64 guest's ZF and PF are in eflags.
# ---------------------------------------------------------------------------
.macro x87_flag_i386 dst, which
    .ifc \which,c
        ldrb \dst, [_cpu, CPU_cf]
        cmp \dst, 0
        cset \dst, ne
    .endif
    .ifc \which,z
        ldrb w9, [_cpu, CPU_flags_res]
        ldr w10, [_cpu, CPU_eflags]
        ubfx \dst, w10, 6, 1
        tbz w9, 1, .Lfz\@
        ldr w10, [_cpu, CPU_res]
        cmp w10, 0
        cset \dst, eq
.Lfz\@:
    .endif
    .ifc \which,p
        ldrb w9, [_cpu, CPU_flags_res]
        ldr w10, [_cpu, CPU_eflags]
        ubfx \dst, w10, 2, 1
        tbz w9, 0, .Lfp\@
        ldr w10, [_cpu, CPU_res]
        eor w10, w10, w10, lsr 4
        eor w10, w10, w10, lsr 2
        eor w10, w10, w10, lsr 1
        mvn w10, w10
        and \dst, w10, 1
.Lfp\@:
    .endif
.endm
.macro x87_flag_amd64 dst, which
    .ifc \which,c
        ldrb \dst, [_cpu, CPU_cf]
        cmp \dst, 0
        cset \dst, ne
    .endif
    .ifc \which,z
        ldr \dst, [_cpu, CPU_eflags]
        ubfx \dst, \dst, 6, 1
    .endif
    .ifc \which,p
        ldr \dst, [_cpu, CPU_eflags]
        ubfx \dst, \dst, 2, 1
    .endif
.endm

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
x87_fcmov_all x87_fcmov, x87_flag_i386
x87_fcmov_all amd64_x87_fcmov, x87_flag_amd64

# FNSTSW AX on amd64: the register cache is flushed, so straight to RAX.
.gadget amd64_x87_fnstsw_ax
    ldrh w8, [_cpu, CPU_fsw]
    add x9, _cpu, CPU_amd64_regs
    strh w8, [x9]
    gret_nomem

# ---------------------------------------------------------------------------
# Memory forms, one macro for both guests. i386: g_addr left the guest
# address in _addr and the stream is [orig_ip]. amd64: the stream is [meta]
# [disp][next_ip], the register cache and rip flushed. A read leaves the host
# pointer in x9; a write's is kept in x19 while the value is worked out, and
# the store and its cross-page flush come before any x87 state changes, so a
# fault leaves the instruction to run again from the start.
# ---------------------------------------------------------------------------
.macro x87m_gadget arch, name
    .ifc \arch,i386
        .gadget x87m_\name
    .else
        .gadget amd64_x87m_\name
    .endif
.endm
.macro x87m_ret arch
    .ifc \arch,i386
        gret 1
    .else
        gret 3
    .endif
.endm
.macro x87m_rprep arch, bits, id
    .ifc \arch,i386
        read_prep \bits, \id
        mov x9, _xaddr
    .else
        ldr x8, [_ip]
        ldr x9, [_ip, 8]
        amd64_vmem_addr
        amd64_vread_prep (\bits/8), \id
    .endif
.endm
.macro x87m_rtail arch, bits, id
    .ifc \arch,i386
        read_bullshit \bits, \id
    .else
        amd64_vread_bullshit (\bits/8), \id
    .endif
.endm
.macro x87m_wprep arch, bits, id
    .ifc \arch,i386
        write_prep \bits, \id
        mov x19, _xaddr
    .else
        ldr x8, [_ip]
        ldr x9, [_ip, 8]
        amd64_vmem_addr
        amd64_vwrite_prep (\bits/8), \id
        mov x19, x9
    .endif
.endm
# The store is done: flush a cross-page one. w7 and w15 ride in w12/w13,
# which the flush's C call preserves.
.macro x87m_wdone arch, bits, id
    mov w12, w7
    mov w13, w15
    .ifc \arch,i386
        mov _xaddr, x19
        write_done \bits, \id
    .else
        mov x9, x19
        amd64_vwrite_done (\bits/8), \id
    .endif
    mov w7, w12
    mov w15, w13
.endm
.macro x87m_wtail arch, bits, id
    .ifc \arch,i386
        write_bullshit \bits, \id
    .else
        amd64_vwrite_bullshit (\bits/8), \id
    .endif
.endm

# Pop once, then the status word.
x87_pop1:
    ldrh w8, [_cpu, CPU_fsw]
    ldrb w11, [_cpu, CPU_x87_valid]
    ubfx w9, w8, 11, 3
    mov w12, 1
    lsl w12, w12, w9
    bic w11, w11, w12
    strb w11, [_cpu, CPU_x87_valid]
    add w9, w9, 1
    bfi w8, w9, 11, 3
    b x87_put_fsw

# ES and B in fsw w8 recomputed from its flags and the masks in w15: what
# FLDCW, FLDENV, FRSTOR, FXRSTOR and FNSTENV leave.
.macro x87_es_update
    mov w10, 0x8080
    bic w8, w8, w10
    and w11, w8, 0x3f
    bic w11, w11, w15
    cbz w11, .Les\@
    orr w8, w8, w10
.Les\@:
.endm

# Arithmetic and compares with a memory operand: ST(0) op m.
.macro x87m_load_kind kind
    .ifc \kind,f32
        ldr w0, [x9]
    .endif
    .ifc \kind,f64
        ldr x0, [x9]
    .endif
    .ifc \kind,i16
        ldrsh x0, [x9]
    .endif
    .ifc \kind,i32
        ldrsw x0, [x9]
    .endif
.endm
.macro x87m_arith arch, kind, bits, ld, op, core
x87m_gadget \arch, \op\()_\kind
    x87m_wait \arch
    x87m_rprep \arch, \bits, \arch\()_\op\()_\kind
    x87m_load_kind \kind
    x87_begin
    bl \ld
    mov w0, 0
    bl x87_get1
    cbnz w0, 1f
    bl \core
1:  mov w0, 0
    bl x87_put
    x87m_ret \arch
    x87m_rtail \arch, \bits, \arch\()_\op\()_\kind
.endm
.macro x87m_com arch, kind, bits, ld, op, pops
x87m_gadget \arch, \op\()_\kind
    x87m_wait \arch
    x87m_rprep \arch, \bits, \arch\()_\op\()_\kind
    x87m_load_kind \kind
    x87_begin
    bl \ld
    mov w0, 0
    bl x87_get1
    mov w12, 3
    cbnz w0, 1f
    mov w0, 1
    bl x87_cmp
    mov w12, w0
1:  mov w0, \pops
    bl x87_put_cc
    x87m_ret \arch
    x87m_rtail \arch, \bits, \arch\()_\op\()_\kind
.endm
.macro x87m_arith_all arch, kind, bits, ld
    x87m_arith \arch, \kind, \bits, \ld, add, x87_add
    x87m_arith \arch, \kind, \bits, \ld, mul, x87_mul
    x87m_com \arch, \kind, \bits, \ld, com, 0
    x87m_com \arch, \kind, \bits, \ld, comp, 1
    x87m_arith \arch, \kind, \bits, \ld, sub, x87_sub
    x87m_arith \arch, \kind, \bits, \ld, subr, x87_subr
    x87m_arith \arch, \kind, \bits, \ld, div, x87_div
    x87m_arith \arch, \kind, \bits, \ld, divr, x87_divr
.endm

# Loads: push the converted value. FLD m32/m64 quiet an SNaN, with IE.
.macro x87m_fld_float arch, kind, bits, ld
x87m_gadget \arch, fld_\kind
    x87m_wait \arch
    x87m_rprep \arch, \bits, \arch\()_fld_\kind
    x87m_load_kind \kind
    x87_begin
    bl \ld
    tbz w7, 16, 2f
    orr w7, w7, X87_DE
2:  x87_class w12, x5, w6, w0
    cmp w12, 5
    b.ne 1f
    orr w7, w7, X87_IE
    orr x5, x5, 0x4000000000000000
1:  mov x3, x5
    mov w4, w6
    bl x87_push
    x87m_ret \arch
    x87m_rtail \arch, \bits, \arch\()_fld_\kind
.endm
.macro x87m_fild arch, kind, bits, ldi
x87m_gadget \arch, fild_\kind
    x87m_wait \arch
    x87m_rprep \arch, \bits, \arch\()_fild_\kind
    \ldi x0, [x9]
    x87_begin
    bl x87_ld_int
    mov x3, x5
    mov w4, w6
    bl x87_push
    x87m_ret \arch
    x87m_rtail \arch, \bits, \arch\()_fild_\kind
.endm
.macro x87m_loads arch
    x87m_fld_float \arch, f32, 32, x87_ld_f32
    x87m_fld_float \arch, f64, 64, x87_ld_f64
    x87m_fild \arch, i16, 16, ldrsh
    x87m_fild \arch, i32, 32, ldrsw
    x87m_fild \arch, i64, 64, ldr
x87m_gadget \arch, fld_f80
    x87m_wait \arch
    x87m_rprep \arch, 80, \arch\()_fld_f80
    ldr x3, [x9]
    ldrh w4, [x9, 8]
    x87_begin
    bl x87_push
    x87m_ret \arch
    x87m_rtail \arch, 80, \arch\()_fld_f80
x87m_gadget \arch, fbld
    x87m_wait \arch
    x87m_rprep \arch, 80, \arch\()_fbld
    ldr x0, [x9]
    ldrh w9, [x9, 8]
    x87_begin
    bl x87_ld_bcd
    mov x3, x5
    mov w4, w6
    bl x87_push
    x87m_ret \arch
    x87m_rtail \arch, 80, \arch\()_fbld
.endm

# Stores: convert ST(0) (the indefinite if it is empty), then -- unless an
# unmasked IE, OE or UE, which store nothing and pop nothing -- store, and
# pop for the P forms.
.macro x87m_store arch, name, bits, pop, conv, st
x87m_gadget \arch, \name
    x87m_wait \arch
    x87m_wprep \arch, \bits, \arch\()_\name
    x87_begin
    mov w0, 0
    bl x87_get1
    \conv
    bic w10, w7, w15
    mov w11, (X87_IE | X87_OE | X87_UE)
    tst w10, w11
    b.ne 8f
    \st
    x87m_wdone \arch, \bits, \arch\()_\name
    .if \pop
        bl x87_pop1
    .else
        ldrh w8, [_cpu, CPU_fsw]
        bl x87_put_fsw
    .endif
    x87m_ret \arch
8:  ldrh w8, [_cpu, CPU_fsw]
    bl x87_put_fsw
    x87m_ret \arch
    x87m_wtail \arch, \bits, \arch\()_\name
.endm
.macro x87c_f32
    mov w16, 40
    mov w17, 0xfe
    bl x87_to_ieee
.endm
.macro x87c_f64
    mov w16, 11
    mov w17, 0x7fe
    bl x87_to_ieee
.endm
.macro x87c_none
.endm
.macro x87c_i16
    mov w0, 16
    bl x87_to_int
.endm
.macro x87c_i32
    mov w0, 32
    bl x87_to_int
.endm
.macro x87c_i64
    mov w0, 64
    bl x87_to_int
.endm
.macro x87c_tt16
    orr w15, w15, 0xc00
    mov w0, 16
    bl x87_to_int
.endm
.macro x87c_tt32
    orr w15, w15, 0xc00
    mov w0, 32
    bl x87_to_int
.endm
.macro x87c_tt64
    orr w15, w15, 0xc00
    mov w0, 64
    bl x87_to_int
.endm
.macro x87c_bcd
    bl x87_to_bcd
.endm
.macro x87s_w
    str w0, [x19]
.endm
.macro x87s_x
    str x0, [x19]
.endm
.macro x87s_h
    strh w0, [x19]
.endm
.macro x87s_t
    str x3, [x19]
    strh w4, [x19, 8]
.endm
.macro x87s_bcd
    str x0, [x19]
    strh w9, [x19, 8]
.endm
.macro x87m_stores arch
    x87m_store \arch, fst_f32, 32, 0, x87c_f32, x87s_w
    x87m_store \arch, fstp_f32, 32, 1, x87c_f32, x87s_w
    x87m_store \arch, fst_f64, 64, 0, x87c_f64, x87s_x
    x87m_store \arch, fstp_f64, 64, 1, x87c_f64, x87s_x
    x87m_store \arch, fstp_f80, 80, 1, x87c_none, x87s_t
    x87m_store \arch, fist_i16, 16, 0, x87c_i16, x87s_h
    x87m_store \arch, fistp_i16, 16, 1, x87c_i16, x87s_h
    x87m_store \arch, fist_i32, 32, 0, x87c_i32, x87s_w
    x87m_store \arch, fistp_i32, 32, 1, x87c_i32, x87s_w
    x87m_store \arch, fistp_i64, 64, 1, x87c_i64, x87s_x
    x87m_store \arch, fisttp_i16, 16, 1, x87c_tt16, x87s_h
    x87m_store \arch, fisttp_i32, 32, 1, x87c_tt32, x87s_w
    x87m_store \arch, fisttp_i64, 64, 1, x87c_tt64, x87s_x
    x87m_store \arch, fbstp, 80, 1, x87c_bcd, x87s_bcd
.endm

# The control and status words. FLDCW keeps the bits that exist (bit 6 reads
# as 1) and recomputes ES.
.macro x87m_control arch
x87m_gadget \arch, fldcw
    x87m_wait \arch
    x87m_rprep \arch, 16, \arch\()_fldcw
    ldrh w15, [x9]
    mov w8, 0x1f3f
    and w15, w15, w8
    orr w15, w15, 0x40
    strh w15, [_cpu, CPU_fcw]
    ldrh w8, [_cpu, CPU_fsw]
    x87_es_update
    strh w8, [_cpu, CPU_fsw]
    x87m_ret \arch
    x87m_rtail \arch, 16, \arch\()_fldcw
x87m_gadget \arch, fnstcw
    x87m_wprep \arch, 16, \arch\()_fnstcw
    ldrh w0, [_cpu, CPU_fcw]
    strh w0, [x19]
    x87m_wdone \arch, 16, \arch\()_fnstcw
    x87m_ret \arch
    x87m_wtail \arch, 16, \arch\()_fnstcw
x87m_gadget \arch, fnstsw
    x87m_wprep \arch, 16, \arch\()_fnstsw
    ldrh w0, [_cpu, CPU_fsw]
    strh w0, [x19]
    x87m_wdone \arch, 16, \arch\()_fnstsw
    x87m_ret \arch
    x87m_wtail \arch, 16, \arch\()_fnstsw
.endm

# w0 = the full tag word: per physical register 11 empty, 01 zero, 10 special
# (NaN, infinity, denormal, unsupported), 00 valid. Clobbers x8-x14.
x87_fulltag:
    ldrb w8, [_cpu, CPU_x87_valid]
    mov w0, 0
    mov w9, 7
1:  lsl w0, w0, 2
    lsr w10, w8, w9
    mov w11, 3
    tbz w10, 0, 2f
    add x12, _cpu, CPU_fp
    add x12, x12, x9, lsl 4
    ldr x13, [x12]
    ldrh w14, [x12, 8]
    x87_class w11, x13, w14, w10
    cmp w11, 1
    mov w11, 0
    b.eq 2f
    mov w11, 1
    b.lo 2f
    mov w11, 2
2:  orr w0, w0, w11
    subs w9, w9, 1
    b.ge 1b
    ret

# A full tag word in w0 -> x87_valid (11 is empty, anything else a value).
x87_tag_load:
    mov w8, 0
    mov w9, 0
1:  lsr w10, w0, w9
    and w10, w10, 3
    cmp w10, 3
    cset w10, ne
    lsr w11, w9, 1
    lsl w10, w10, w11
    orr w8, w8, w10
    add w9, w9, 2
    cmp w9, 16
    b.lo 1b
    strb w8, [_cpu, CPU_x87_valid]
    ret

# The environment, from x9: control, status (ES recomputed), tag. \w: 4 for
# the 32-bit format (fields a dword apart), 2 for the 16-bit one. Then the
# eight registers, ST(0) first, from \regs on (0: none).
.macro x87_env_load w, regs
    ldrh w15, [x9]
    mov w8, 0x1f3f
    and w15, w15, w8
    orr w15, w15, 0x40
    strh w15, [_cpu, CPU_fcw]
    ldrh w8, [x9, \w]
    x87_es_update
    strh w8, [_cpu, CPU_fsw]
    ldrh w0, [x9, 2*\w]
    mov x16, x9
    bl x87_tag_load
    .if \regs
        mov x9, x16
        ldrh w8, [_cpu, CPU_fsw]
        ubfx w8, w8, 11, 3
        mov w10, 0
1:      add w11, w10, w8
        and w11, w11, 7
        add x12, _cpu, CPU_fp
        add x12, x12, x11, lsl 4
        mov w13, 10
        madd x13, x10, x13, x9
        ldr x14, [x13, \regs]
        str x14, [x12]
        add x13, x13, \regs
        ldrh w14, [x13, 8]
        strh w14, [x12, 8]
        add w10, w10, 1
        cmp w10, 8
        b.lo 1b
    .endif
.endm
# ... and stored to x19. The 32-bit format's high halves of control, status,
# tag and FDS read as FFFF; the instruction and data pointers and the opcode
# are not kept (they are 0).
.macro x87_env_store w, regs
    bl x87_fulltag
    ldrh w8, [_cpu, CPU_fcw]
    ldrh w9, [_cpu, CPU_fsw]
    .if \w == 4
        mov w10, 0xffff0000
        orr w8, w8, w10
        orr w9, w9, w10
        orr w0, w0, w10
        str w8, [x19]
        str w9, [x19, 4]
        str w0, [x19, 8]
        str xzr, [x19, 12]
        str wzr, [x19, 20]
        str w10, [x19, 24]
    .else
        strh w8, [x19]
        strh w9, [x19, 2]
        strh w0, [x19, 4]
        str xzr, [x19, 6]
    .endif
    .if \regs
        ldrh w8, [_cpu, CPU_fsw]
        ubfx w8, w8, 11, 3
        mov w10, 0
1:      add w11, w10, w8
        and w11, w11, 7
        add x12, _cpu, CPU_fp
        add x12, x12, x11, lsl 4
        mov w13, 10
        madd x13, x10, x13, x19
        ldr x14, [x12]
        str x14, [x13, \regs]
        ldrh w14, [x12, 8]
        add x13, x13, \regs
        strh w14, [x13, 8]
        add w10, w10, 1
        cmp w10, 8
        b.lo 1b
    .endif
.endm

.macro x87m_env arch
x87m_gadget \arch, fldenv32
    x87m_wait \arch
    x87m_rprep \arch, 224, \arch\()_fldenv32
    x87_env_load 4, 0
    x87m_ret \arch
    x87m_rtail \arch, 224, \arch\()_fldenv32
x87m_gadget \arch, fldenv16
    x87m_wait \arch
    x87m_rprep \arch, 112, \arch\()_fldenv16
    x87_env_load 2, 0
    x87m_ret \arch
    x87m_rtail \arch, 112, \arch\()_fldenv16
x87m_gadget \arch, frstor32
    x87m_wait \arch
    x87m_rprep \arch, 864, \arch\()_frstor32
    x87_env_load 4, 28
    x87m_ret \arch
    x87m_rtail \arch, 864, \arch\()_frstor32
x87m_gadget \arch, frstor16
    x87m_wait \arch
    x87m_rprep \arch, 752, \arch\()_frstor16
    x87_env_load 2, 14
    x87m_ret \arch
    x87m_rtail \arch, 752, \arch\()_frstor16
# FNSTENV then masks every exception (and so ES clears); FNSAVE then
# initializes, as FNINIT.
x87m_gadget \arch, fnstenv32
    x87m_wprep \arch, 224, \arch\()_fnstenv32
    x87_env_store 4, 0
    x87m_wdone \arch, 224, \arch\()_fnstenv32
    b \arch\()_x87_after_fnstenv
    x87m_wtail \arch, 224, \arch\()_fnstenv32
x87m_gadget \arch, fnstenv16
    x87m_wprep \arch, 112, \arch\()_fnstenv16
    x87_env_store 2, 0
    x87m_wdone \arch, 112, \arch\()_fnstenv16
    b \arch\()_x87_after_fnstenv
    x87m_wtail \arch, 112, \arch\()_fnstenv16
x87m_gadget \arch, fnsave32
    x87m_wprep \arch, 864, \arch\()_fnsave32
    x87_env_store 4, 28
    x87m_wdone \arch, 864, \arch\()_fnsave32
    b \arch\()_x87_after_fnsave
    x87m_wtail \arch, 864, \arch\()_fnsave32
x87m_gadget \arch, fnsave16
    x87m_wprep \arch, 752, \arch\()_fnsave16
    x87_env_store 2, 14
    x87m_wdone \arch, 752, \arch\()_fnsave16
    b \arch\()_x87_after_fnsave
    x87m_wtail \arch, 752, \arch\()_fnsave16
.endm

# The tails of FNSTENV and FNSAVE, after the store: one per guest, for its
# stream length.
.macro x87_after arch
\arch\()_x87_after_fnstenv:
    ldrh w15, [_cpu, CPU_fcw]
    orr w15, w15, 0x3f
    strh w15, [_cpu, CPU_fcw]
    ldrh w8, [_cpu, CPU_fsw]
    x87_es_update
    strh w8, [_cpu, CPU_fsw]
    x87m_ret \arch
\arch\()_x87_after_fnsave:
    mov w8, 0x37f
    strh w8, [_cpu, CPU_fcw]
    strh wzr, [_cpu, CPU_fsw]
    strb wzr, [_cpu, CPU_x87_valid]
    x87m_ret \arch
.endm

# FXSAVE: the 512-byte area -- control, status, the abridged tag, no
# opcode or pointers (AMD keeps them only with an exception pending), MXCSR
# with the host's flags folded in as STMXCSR does, MXCSR_MASK, the registers
# in stack order padded with zeros, and the XMM registers the mode has. The
# rest of the area is not written, as on hardware.
.macro x87m_fxsave arch, nxmm
x87m_gadget \arch, fxsave
    .ifc \arch,i386
        tst _addr, 15                   // the area must be 16-byte aligned: #GP
        b.ne x87_i386_gp
    .endif
    .ifc \arch,amd64
        // A straddling area is staged, and amd64's write staging starts
        // blank: read the area first (which stages it, contents and all), so
        // the bytes FXSAVE leaves alone go back as they were.
        x87m_rprep \arch, 4096, \arch\()_fxsave_r
    .endif
    x87m_wprep \arch, 4096, \arch\()_fxsave
    ldrh w8, [_cpu, CPU_fcw]
    strh w8, [x19]
    ldrh w8, [_cpu, CPU_fsw]
    strh w8, [x19, 2]
    ldrb w8, [_cpu, CPU_x87_valid]
    strh w8, [x19, 4]
    strh wzr, [x19, 6]
    stp xzr, xzr, [x19, 8]
    mrs x13, fpsr
    ldr w12, [_cpu, CPU_mxcsr]
    and w14, w13, 1                     /* IOC -> IE */
    orr w12, w12, w14
    ubfx w14, w13, 1, 1                 /* DZC -> ZE */
    orr w12, w12, w14, lsl 2
    ubfx w14, w13, 2, 1                 /* OFC -> OE */
    orr w12, w12, w14, lsl 3
    ubfx w14, w13, 3, 1                 /* UFC -> UE */
    orr w12, w12, w14, lsl 4
    .ifc \arch,i386
        ubfx w10, w12, 15, 1            /* a flush to zero (FTZ) is inexact */
        and w10, w10, w14
        orr w12, w12, w10, lsl 5
    .endif
    ubfx w14, w13, 4, 1                 /* IXC -> PE */
    orr w12, w12, w14, lsl 5
    ubfx w14, w13, 7, 1                 /* IDC -> DE */
    orr w12, w12, w14, lsl 1
    str w12, [_cpu, CPU_mxcsr]
    msr fpsr, xzr
    str w12, [x19, 24]
    mov w8, 0xffff
    str w8, [x19, 28]
    ldrh w8, [_cpu, CPU_fsw]
    ubfx w8, w8, 11, 3
    mov w10, 0
1:  add w11, w10, w8
    and w11, w11, 7
    add x12, _cpu, CPU_fp
    add x12, x12, x11, lsl 4
    add x13, x19, x10, lsl 4
    ldr x14, [x12]
    ldrh w9, [x12, 8]
    stp x14, x9, [x13, 32]
    add w10, w10, 1
    cmp w10, 8
    b.lo 1b
    add x12, _cpu, CPU_xmm
    mov w10, 0
2:  ldr q0, [x12, x10, lsl 4]
    add x13, x19, x10, lsl 4
    str q0, [x13, 160]
    add w10, w10, 1
    cmp w10, \nxmm
    b.lo 2b
    mov w7, 0
    mov w15, 0
    x87m_wdone \arch, 4096, \arch\()_fxsave
    x87m_ret \arch
    x87m_wtail \arch, 4096, \arch\()_fxsave
    .ifc \arch,amd64
        x87m_rtail \arch, 4096, \arch\()_fxsave_r
    .endif
.endm

# FXRSTOR: MXCSR's reserved bits are #GP before anything changes; then all
# of it back, ES recomputed, MXCSR's rounding (and for i386, FTZ, which its
# SSE runs on the host) installed in FPCR and its flags out of FPSR.
.macro x87m_fxrstor arch, nxmm
x87m_gadget \arch, fxrstor
    .ifc \arch,i386
        tst _addr, 15                   // the area must be 16-byte aligned: #GP
        b.ne x87_i386_gp
    .endif
    x87m_rprep \arch, 4096, \arch\()_fxrstor
    ldr w12, [x9, 24]
    lsr w13, w12, 16
    cbnz w13, 9f
    ldrh w15, [x9]
    mov w8, 0x1f3f
    and w15, w15, w8
    orr w15, w15, 0x40
    strh w15, [_cpu, CPU_fcw]
    ldrh w8, [x9, 2]
    x87_es_update
    strh w8, [_cpu, CPU_fsw]
    ldrb w10, [x9, 4]
    strb w10, [_cpu, CPU_x87_valid]
    ubfx w8, w8, 11, 3
    mov w10, 0
1:  add w11, w10, w8
    and w11, w11, 7
    add x13, _cpu, CPU_fp
    add x13, x13, x11, lsl 4
    add x14, x9, x10, lsl 4
    ldr x0, [x14, 32]
    str x0, [x13]
    ldrh w0, [x14, 40]
    strh w0, [x13, 8]
    add w10, w10, 1
    cmp w10, 8
    b.lo 1b
    add x13, _cpu, CPU_xmm
    mov w10, 0
2:  add x14, x9, x10, lsl 4
    ldr q0, [x14, 160]
    str q0, [x13, x10, lsl 4]
    add w10, w10, 1
    cmp w10, \nxmm
    b.lo 2b
    str w12, [_cpu, CPU_mxcsr]
    msr fpsr, xzr
    ubfx w13, w12, 13, 1
    ubfx w14, w12, 14, 1
    orr w13, w14, w13, lsl 1
    mrs x14, fpcr
    bic x14, x14, (7 << 22)             /* RMode and FZ */
    orr x14, x14, x13, lsl 22
    .ifc \arch,i386
        ubfx w13, w12, 15, 1
        orr x14, x14, x13, lsl 24
    .endif
    msr fpcr, x14
    x87m_ret \arch
9:  str xzr, [_cpu, CPU_segfault_addr]
    strb wzr, [_cpu, CPU_segfault_was_write]
    strb wzr, [_cpu, CPU_segfault_reported]
    .ifc \arch,i386
        ldr x8, [_ip]
        mov eip, w8
    .endif
    mov x0, INT_GPF
    b jit_exit
    x87m_rtail \arch, 4096, \arch\()_fxrstor
.endm

.irp arch, i386, amd64
    x87m_arith_all \arch, f32, 32, x87_ld_f32
    x87m_arith_all \arch, f64, 64, x87_ld_f64
    x87m_arith_all \arch, i16, 16, x87_ld_int
    x87m_arith_all \arch, i32, 32, x87_ld_int
    x87m_loads \arch
    x87m_stores \arch
    x87m_control \arch
    x87m_env \arch
    x87_after \arch
.endr
x87m_fxsave i386, 8
x87m_fxsave amd64, 16
x87m_fxrstor i386, 8
x87m_fxrstor amd64, 16

# #GP(0) at the i386 instruction ([orig_ip] in the stream).
x87_i386_gp:
    str xzr, [_cpu, CPU_segfault_addr]
    strb wzr, [_cpu, CPU_segfault_was_write]
    strb wzr, [_cpu, CPU_segfault_reported]
    ldr x8, [_ip]
    mov eip, w8
    mov x0, INT_GPF
    b jit_exit

# EMMS: every x87 register empty.
.gadget x87_emms
    x87_wait
    strb wzr, [_cpu, CPU_x87_valid]
    gret_nomem 1
