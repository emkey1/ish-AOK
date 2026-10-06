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

# ===========================================================================
# The transcendentals: F2XM1, FYL2X, FYL2XP1, FPTAN, FPATAN, FSIN, FCOS,
# FSINCOS.
#
# An AMD Ryzen's results are not correctly rounded -- 2-3% of them are a unit
# away from it, from its own internal approximation, and no model of extra
# internal bits reproduces them -- so these compute the correctly rounded
# value, which is camd's in ~98% of cases and a unit from it otherwise
# (tests/manual/x86/x87_trans.c allows that unit, and C1 with it). What the
# hardware does exactly, these do exactly: the argument reduction of
# FSIN/FCOS/FPTAN/FSINCOS uses pi truncated to 66 bits (as Intel documents
# too), reducing |x| to the nearest multiple of pi66/2; |x| >= 2^63 sets C2
# and changes nothing; every special operand, the stack faults, PE (set for
# every computed result, exact or not), precision control (ignored).
#
# Values in flight are 128-bit "bigfloats" in a stack frame: [lo][hi][e][s],
# the value (-1)^s * (hi:lo) * 2^(e - 16383 - 127) with bit 127 of hi:lo set,
# or zero -- x87_round's own scale. Multiply, add and divide truncate. Each
# function is a leading term plus a power series in a small argument X (sin r
# = r + r P(r^2), cos r = 1 + P(r^2), atan u = u + u Q(u^2), atanh s = s + s
# S(s^2), expm1 t = t + t E(t)), and the series runs in 128-bit binary
# fractions against a table of coefficients until a term is zero. Where the
# leading term is exact (r, 1, u), the tail is kept apart and joined to it
# with a sticky bit, so a result just off a representable value still rounds
# the right way in every mode. Checked against a 130-digit reference over
# camd's cases: every result is the correctly rounded one, save FPTAN of
# arguments below 2^-32, where the reference itself is too short (and the
# hardware is a unit off, low, about half the time).
#
# The routines take pointers: x12 the destination (it may be an operand),
# x13 and x14 the operands. They clobber
# x0, x3-x6, x8-x11, x16, x17 and the argument registers, and keep x7 (the
# exceptions), x15 (the control word), x19 and the guests' x20-x28.
# ===========================================================================

#define BF_SIZE 32

# x10:x9 (hi:lo) with exponent x11 and sign x17, normalized, to [x12].
bf_put:
    orr x0, x9, x10
    cbz x0, 2f
    clz x0, x10
    cbnz x10, 1f
    clz x0, x9
    add x0, x0, 64
1:  cbz x0, 3f
    cmp x0, 64
    b.lo 4f
    sub x16, x0, 64
    lsl x10, x9, x16
    mov x9, 0
    b 5f
4:  neg x16, x0
    lsr x16, x9, x16
    lsl x10, x10, x0
    orr x10, x10, x16
    lsl x9, x9, x0
5:  sub x11, x11, x0
3:  stp x9, x10, [x12]
    stp x11, x17, [x12, 16]
    ret
2:  stp xzr, xzr, [x12]
    stp xzr, x17, [x12, 16]
    ret

bf_copy:                            // [x12] = [x13]
    ldp x9, x10, [x13]
    ldp x11, x17, [x13, 16]
    stp x9, x10, [x12]
    stp x11, x17, [x12, 16]
    ret
bf_neg:                             // [x12] = -[x13]
    ldp x9, x10, [x13]
    ldp x11, x17, [x13, 16]
    eor x17, x17, 1
    stp x9, x10, [x12]
    stp x11, x17, [x12, 16]
    ret
bf_int:                             // [x12] = x0, a signed integer
    lsr x17, x0, 63
    cmp x0, 0
    cneg x9, x0, lt
    mov x10, 0
    mov x11, 16383 + 127
    b bf_put
bf_from_f80:                        // [x12] = the extended value x3 / w4
    mov x9, 0
    mov x10, x3
    and w11, w4, 0x7fff
    cmp w11, 0
    csinc w11, w11, wzr, ne
    ubfx w17, w4, 15, 1
    b bf_put

# [x12] = [x13] * [x14]: the high half of the 256-bit product, exactly.
bf_mul:
    ldp x3, x4, [x13]
    ldp x5, x6, [x14]
    ldr x16, [x13, 24]
    ldr x17, [x14, 24]
    eor x17, x16, x17
    cbz x4, 9f
    cbz x6, 9f
    ldr x16, [x13, 16]
    ldr x0, [x14, 16]
    add x16, x16, x0
    mov x0, 16382
    sub x16, x16, x0
    umulh x8, x3, x5                // lo*lo, high word
    mul x9, x4, x5                  // hi*lo
    umulh x10, x4, x5
    mul x11, x3, x6                 // lo*hi
    umulh x0, x3, x6
    adds x8, x8, x9                 // word 1
    cset x9, cs
    adds x8, x8, x11
    cinc x9, x9, cs
    mul x11, x4, x6                 // hi*hi
    umulh x4, x4, x6
    adds x11, x11, x10              // word 2
    cset x10, cs
    adds x11, x11, x0
    cinc x10, x10, cs
    adds x11, x11, x9
    cinc x10, x10, cs
    add x4, x4, x10                 // word 3
    tbnz x4, 63, 1f
    extr x4, x4, x11, 63
    extr x11, x11, x8, 63
    sub x16, x16, 1
1:  stp x11, x4, [x12]
    stp x16, x17, [x12, 16]
    ret
9:  stp xzr, xzr, [x12]
    stp xzr, x17, [x12, 16]
    ret

# [x12] = [x13] + [x14] (bf_sub: - [x14]), the smaller aligned by a
# truncating shift.
bf_sub:
    mov x0, 1
    b 0f
bf_add:
    mov x0, 0
0:  ldp x3, x4, [x13]
    ldp x5, x6, [x14]
    ldp x8, x16, [x13, 16]
    ldp x10, x17, [x14, 16]
    eor x17, x17, x0
    cbz x6, 7f                      // b is zero: a
    cbz x4, 8f                      // a is zero: b
    // |a| >= |b|
    cmp x8, x10
    b.gt 2f
    b.lt 1f
    cmp x4, x6
    b.hi 2f
    b.lo 1f
    cmp x3, x5
    b.hs 2f
1:  mov x0, x3
    mov x3, x5
    mov x5, x0
    mov x0, x4
    mov x4, x6
    mov x6, x0
    mov x0, x8
    mov x8, x10
    mov x10, x0
    mov x0, x16
    mov x16, x17
    mov x17, x0
2:  sub x10, x8, x10                // the shift
    cmp x10, 128
    b.hs 7f                         // b vanishes
    cmp x10, 64
    b.lo 3f
    sub x10, x10, 64
    lsr x5, x6, x10
    mov x6, 0
    b 4f
3:  cbz x10, 4f
    neg x0, x10
    lsr x5, x5, x10
    lsl x0, x6, x0
    orr x5, x5, x0
    lsr x6, x6, x10
4:  eor x0, x16, x17
    cbnz x0, 5f
    adds x9, x3, x5
    adcs x10, x4, x6
    b.cc 6f
    extr x9, x10, x9, 1
    lsr x10, x10, 1
    orr x10, x10, 0x8000000000000000
    add x8, x8, 1
    b 6f
5:  subs x9, x3, x5
    sbc x10, x4, x6
6:  mov x11, x8
    mov x17, x16
    b bf_put
7:  // the result is a: x3 x4 x8 x16
    mov x9, x3
    mov x10, x4
    mov x11, x8
    mov x17, x16
    b bf_put
8:  mov x9, x5
    mov x10, x6
    mov x11, x10
    ldr x11, [x14, 16]
    b bf_put

# [x12] = [x13] / [x14]: the quotient truncated to 128 bits, floor(A 2^127 /
# B) for significands A and B. Its top bit is A >= B; then 32 bits at a time
# (31 the last time), each chunk c = floor(R 2^32 / B) estimated as R's top
# word over B's top 32 bits + 1 -- never over, at most 3 under -- and the
# remainder kept exact, so the corrections bring c to the floor.
bf_div:
    ldp x3, x4, [x13]
    ldp x5, x6, [x14]
    ldr x11, [x13, 16]
    ldr x0, [x14, 16]
    sub x11, x11, x0
    mov x0, 16383
    add x11, x11, x0
    ldr x17, [x13, 24]
    ldr x0, [x14, 24]
    eor x17, x17, x0
    mov x9, 0
    mov x10, 0
    cbz x4, bf_put                  // 0 / b
    stp x11, x17, [sp, -32]!
    str x12, [sp, 16]
    cmp x3, x5
    sbcs xzr, x4, x6
    cset x9, hs
    b.lo 1f
    subs x3, x3, x5
    sbc x4, x4, x6
1:  lsr x16, x6, 32
    add x16, x16, 1
    mov x17, 4
2:  cmp x17, 1
    b.eq 3f
    udiv x0, x4, x16
    lsr x8, x4, 32                  // R <<= 32, three words: x8:x4:x3
    extr x4, x4, x3, 32
    lsl x3, x3, 32
    extr x10, x10, x9, 32           // and Q
    lsl x9, x9, 32
    b 4f
3:  lsr x0, x4, 1
    udiv x0, x0, x16
    lsr x8, x4, 33
    extr x4, x4, x3, 33
    lsl x3, x3, 31
    extr x10, x10, x9, 33
    lsl x9, x9, 31
4:  mul x13, x0, x5                 // R -= c B
    umulh x14, x0, x5
    mul x11, x0, x6
    umulh x12, x0, x6
    adds x11, x11, x14
    cinc x12, x12, cs
    subs x3, x3, x13
    sbcs x4, x4, x11
    sbc x8, x8, x12
5:  cbnz x8, 6f                     // R >= B: once more
    cmp x3, x5
    sbcs xzr, x4, x6
    b.lo 7f
6:  subs x3, x3, x5
    sbcs x4, x4, x6
    sbc x8, x8, xzr
    add x0, x0, 1
    b 5b
7:  orr x9, x9, x0
    subs x17, x17, 1
    b.ne 2b
    ldr x12, [sp, 16]
    ldp x11, x17, [sp], 32
    b bf_put

# lead [x13] + tail [x14], joined with a sticky bit, for x87_round: x8:x9
# (hi:lo, unnormalized is fine), x10 the exponent, w11 the sign.
bf_join:
    ldp x3, x4, [x13]
    ldp x5, x6, [x14]
    ldp x8, x16, [x13, 16]
    ldp x10, x17, [x14, 16]
    cbz x6, 7f
    cbz x4, 8f
    cmp x8, x10
    b.gt 2f
    b.lt 1f
    cmp x4, x6
    b.hi 2f
    b.lo 1f
    cmp x3, x5
    b.hs 2f
1:  mov x0, x3
    mov x3, x5
    mov x5, x0
    mov x0, x4
    mov x4, x6
    mov x6, x0
    mov x0, x8
    mov x8, x10
    mov x10, x0
    mov x0, x16
    mov x16, x17
    mov x17, x0
2:  sub x10, x8, x10
    mov x0, 0                       // sticky
    cmp x10, 128
    b.lo 21f
    orr x0, x5, x6
    cmp x0, 0
    cset x0, ne
    mov x5, 0
    mov x6, 0
    b 4f
21: cmp x10, 64
    b.lo 3f
    sub x10, x10, 64
    mov x13, -1
    lsl x13, x13, x10
    bic x13, x6, x13
    orr x13, x13, x5
    cmp x13, 0
    cset x0, ne
    lsr x5, x6, x10
    mov x6, 0
    b 4f
3:  cbz x10, 4f
    neg x13, x10
    lsl x14, x5, x13
    cmp x14, 0
    cset x0, ne
    lsr x5, x5, x10
    lsl x13, x6, x13
    orr x5, x5, x13
    lsr x6, x6, x10
4:  eor x13, x16, x17
    cbnz x13, 5f
    adds x9, x3, x5
    adcs x10, x4, x6
    b.cc 6f
    and x13, x9, 1
    orr x0, x0, x13
    extr x9, x10, x9, 1
    lsr x10, x10, 1
    orr x10, x10, 0x8000000000000000
    add x8, x8, 1
    b 6f
5:  // a - b - sticky: the truncated part made b's magnitude a little more
    subs x9, x3, x5
    sbc x10, x4, x6
    subs x9, x9, x0
    sbc x10, x10, xzr
6:  orr x9, x9, x0
    mov x13, x8
    mov x8, x10
    mov x10, x13
    mov w11, w16
    ret
7:  mov x9, x3
    mov x10, x8
    mov x8, x4
    mov w11, w16
    ret
8:  mov x9, x5
    mov x8, x6
    mov w11, w17
    ret

// The x87_round call for a bigfloat result: [x13] -> A (x3/w4), w7's PE set
// (the hardware sets it for every computed result), C1 from the rounding.
// Clobbers what x87_round does.
.macro bf_round_slot off
    add x13, sp, \off
    ldp x9, x8, [x13]
    ldp x10, x11, [x13, 16]
    bl x87_round_bf
.endm
x87_round_bf:                       // x8:x9, x10, w11 -> A
    str lr, [sp, -16]!
    orr x0, x8, x9
    cbnz x0, 1f
    mov x3, 0
    lsl w4, w11, 15
    orr w7, w7, X87_PE
    bic w7, w7, X87_C1
    ldr lr, [sp], 16
    ret
1:  mov w16, 0
    mov w17, 0x7ffe
    bl x87_round
    // a transcendental's result is never exact to the hardware: PE always,
    // and so UE for a tiny one even where this value was exact
    orr w7, w7, X87_PE
    cbnz x10, 2f
    cbz x8, 2f
    orr w7, w7, X87_UE
2:  x87_pack
    ldr lr, [sp], 16
    ret


# x14:x13 = the high half of x6:x5 * x4:x3 (128-bit binary fractions: the
# product, truncated). Clobbers x8, x11.
.macro FX_MUL
    umulh x8, x5, x3
    mul x11, x6, x3
    umulh x13, x6, x3
    adds x8, x8, x11
    cset x11, cs
    mul x14, x5, x4
    adds x8, x8, x14
    cinc x11, x11, cs
    umulh x14, x5, x4
    adds x13, x13, x14
    cset x14, cs
    adds x13, x13, x11
    cinc x14, x14, cs
    mul x11, x6, x4
    adds x13, x13, x11
    cinc x14, x14, cs
    umulh x11, x6, x4
    add x14, x14, x11
.endm

# x10:x9 = |[x13]| as a 128-bit binary fraction (the value is below 1).
bf_fx:
    ldp x9, x10, [x13]
    ldr x11, [x13, 16]
    mov x0, 16382
    subs x11, x0, x11
    b.lt 8f                         // (never: 1 or more)
    cbz x10, 8f
    cmp x11, 128
    b.hs 8f
    cmp x11, 64
    b.lo 1f
    sub x11, x11, 64
    lsr x9, x10, x11
    mov x10, 0
    ret
1:  cbz x11, 2f
    neg x0, x11
    lsr x9, x9, x11
    lsl x0, x10, x0
    orr x9, x9, x0
    lsr x10, x10, x11
2:  ret
8:  mov x9, 0
    mov x10, 0
    ret

# x10:x9 = T = c1 + c2 X + c3 X^2 + ..., or alternating (c1 - c2 X + c3 X^2
# - ...) with bit 0 of x0 set; X = x10:x9 on entry, a binary fraction; the c
# from [x16], every (x0 >> 8) bytes, up to x17. A series is X T: T is of the
# order of c1, so it keeps its precision however small X is. It stops when a
# term is zero. Clobbers x0, x3-x6, x8, x11, x13, x14, x16, x17.
fx_series:
    stp x9, x10, [sp, -16]!
    ldp x9, x10, [x16]
    add x16, x16, x0, lsr 8
    and x8, x0, 1
    orr x0, x0, x8, lsl 1           // bit 1: the next term is subtracted
    ldp x5, x6, [sp]                // the power of X
1:  cmp x16, x17
    b.hs 9f
    ldp x3, x4, [x16]
    add x16, x16, x0, lsr 8
    FX_MUL
    orr x8, x13, x14
    cbz x8, 9f
    tbnz x0, 1, 2f
    adds x9, x9, x13
    adc x10, x10, x14
    b 3f
2:  subs x9, x9, x13
    sbc x10, x10, x14
3:  and x8, x0, 1
    eor x0, x0, x8, lsl 1
    ldp x3, x4, [sp]
    FX_MUL
    mov x5, x13
    mov x6, x14
    b 1b
9:  add sp, sp, 16
    ret

# Frames: n bigfloat slots, then lr, two spare words and x19 (the series
# loops count in it; the memory forms keep a write's address there).
.macro BF_ENTER n
    sub sp, sp, (\n) * BF_SIZE + 32
    str lr, [sp, (\n) * BF_SIZE]
    str x19, [sp, (\n) * BF_SIZE + 24]
.endm
.macro BF_LEAVE n
    ldr lr, [sp, (\n) * BF_SIZE]
    ldr x19, [sp, (\n) * BF_SIZE + 24]
    add sp, sp, (\n) * BF_SIZE + 32
    ret
.endm
.macro BF3 fn, d, a, b
    add x12, sp, (\d) * BF_SIZE
    add x13, sp, (\a) * BF_SIZE
    add x14, sp, (\b) * BF_SIZE
    bl \fn
.endm
.macro BF2 fn, d, a
    add x12, sp, (\d) * BF_SIZE
    add x13, sp, (\a) * BF_SIZE
    bl \fn
.endm
.macro BFC d, label
    add x12, sp, (\d) * BF_SIZE
    adr x13, \label
    bl bf_copy
.endm
.macro BFI d, val
    mov x0, \val
    add x12, sp, (\d) * BF_SIZE
    bl bf_int
.endm
// The exponent of slot d, + 1 (doubling), unless it is zero.
.macro BFX2 d
    ldr x0, [sp, (\d) * BF_SIZE + 8]
    cbz x0, .Lx2\@
    ldr x0, [sp, (\d) * BF_SIZE + 16]
    add x0, x0, 1
    str x0, [sp, (\d) * BF_SIZE + 16]
.Lx2\@:
.endm

// [d] = (-1)^sign [a] T(|[a]|): a series by fx_series over a table, as a
// bigfloat ([a] is X, nonnegative).
.macro BF_FXSERIES d, a, table, end, stride, alt, sign
    add x13, sp, (\a) * BF_SIZE
    bl bf_fx
    adr x16, \table
    adr x17, \end
    mov x0, (\alt) | ((\stride) << 8)
    bl fx_series
    mov x11, 16382
    mov x17, \sign
    add x12, sp, (\d) * BF_SIZE
    bl bf_put
    add x12, sp, (\d) * BF_SIZE
    add x13, sp, (\a) * BF_SIZE
    add x14, sp, (\d) * BF_SIZE
    bl bf_mul
.endm

# [x12] = log2(e) * 2 atanh([x13]) (that is, log2((1 + s) / (1 - s))):
# atanh s = s + s (s^2/3 + s^4/5 + ...).
k_atanh_log2:
    BF_ENTER 4                      // 0 s, 1 s^2 / atanh, 2 the series, 3 log2(e)
    str x12, [sp, 4 * BF_SIZE + 8]
    add x12, sp, 0
    bl bf_copy
    BF3 bf_mul, 1, 0, 0
    BF_FXSERIES 2, 1, fx_odd, fx_odd_end, 16, 0, 0
    BF3 bf_mul, 2, 0, 2
    BF3 bf_add, 1, 0, 2
    BFX2 1
    BFC 3, bfc_log2e
    ldr x12, [sp, 4 * BF_SIZE + 8]
    add x13, sp, 1 * BF_SIZE
    add x14, sp, 3 * BF_SIZE
    bl bf_mul
    BF_LEAVE 4

# [x12] = log2([x13]), [x13] finite, positive: E + log2(m), m in [sqrt(2)/2,
# sqrt(2)), by atanh((m - 1) / (m + 1)).
k_log2:
    BF_ENTER 5                      // 0 m, 1 one, 2 m - 1, 3 m + 1, 4 s
    str x12, [sp, 5 * BF_SIZE + 8]
    add x12, sp, 0
    bl bf_copy
    ldr x10, [sp, 16]
    mov x0, 16383
    sub x11, x10, x0                // E
    mov x10, x0
    ldp x8, x9, [sp]
    mov x0, 0x6484
    movk x0, 0xf9de, lsl 16
    movk x0, 0xf333, lsl 32
    movk x0, 0xb504, lsl 48         // sqrt(2) * 2^127, the high word
    cmp x9, x0
    b.lo 1f
    b.hi 0f
    mov x0, 0xc7c5
    movk x0, 0xc9b5, lsl 16
    movk x0, 0x6ef3, lsl 32
    movk x0, 0xf8dc, lsl 48         // ... and the low
    cmp x8, x0
    b.ls 1f
0:
    sub x10, x10, 1
    add x11, x11, 1
1:  str x10, [sp, 16]
    str x11, [sp, 5 * BF_SIZE + 16]
    BFI 1, 1
    BF3 bf_sub, 2, 0, 1
    ldr x0, [sp, 2 * BF_SIZE + 8]
    cbz x0, 3f                      // m is 1: log2 is E exactly
    BF3 bf_add, 3, 0, 1
    BF3 bf_div, 4, 2, 3
    add x12, sp, 0
    add x13, sp, 4 * BF_SIZE
    bl k_atanh_log2
    ldr x0, [sp, 5 * BF_SIZE + 16]
    cbz x0, 2f
    BFI 1, x0
    BF3 bf_add, 0, 1, 0
2:  ldr x12, [sp, 5 * BF_SIZE + 8]
    add x13, sp, 0
    bl bf_copy
    BF_LEAVE 5
3:  ldr x0, [sp, 5 * BF_SIZE + 16]
    ldr x12, [sp, 5 * BF_SIZE + 8]
    bl bf_int
    BF_LEAVE 5

# [x12] = atan([x13]) for [x13] in [0, 1]: atan(k/16) from the table, k the
# nearest, plus the series for (t - k/16) / (1 + t k/16).
k_atan:
    BF_ENTER 7                      // 0 t, 1 c, 2 num, 3 den/u, 4 sum, 5 u^2, 6 power/term
    str x12, [sp, 7 * BF_SIZE + 8]
    add x12, sp, 0
    bl bf_copy
    // k = round(16 t): t = hi * 2^(e - 16446), so 16 t = hi >> (16442 - e)
    ldr x9, [sp, 8]
    ldr x10, [sp, 16]
    mov x0, 0
    cbz x9, 2f
    mov x11, 16442
    sub x11, x11, x10               // the shift
    cmp x11, 64
    b.hi 2f
    b.lo 1f
    lsr x0, x9, 63                  // shift 64: only the rounding bit
    b 2f
1:  lsr x0, x9, x11
    sub x11, x11, 1
    lsr x10, x9, x11
    and x10, x10, 1
    add x0, x0, x10
2:  str x0, [sp, 7 * BF_SIZE + 16]
    cbz x0, 5f
    // u = (t - c) / (1 + t c), c = k / 16
    add x12, sp, 1 * BF_SIZE
    bl bf_int
    ldr x0, [sp, 1 * BF_SIZE + 16]
    sub x0, x0, 4
    str x0, [sp, 1 * BF_SIZE + 16]
    BF3 bf_sub, 2, 0, 1
    BF3 bf_mul, 3, 0, 1
    BFI 4, 1
    BF3 bf_add, 3, 4, 3
    BF3 bf_div, 3, 2, 3
    b 6f
5:  BF2 bf_copy, 3, 0               // k = 0: u = t
6:  // atan u = u + u Q
    BF2 bf_copy, 4, 3
    ldr x0, [sp, 3 * BF_SIZE + 8]
    cbz x0, 8f
    add x12, sp, 2 * BF_SIZE
    add x13, sp, 3 * BF_SIZE
    bl k_atan_tail
    BF3 bf_add, 4, 3, 2
8:  ldr x0, [sp, 7 * BF_SIZE + 16]
    cbz x0, 9f
    adr x14, bfc_atan16
    add x14, x14, x0, lsl 5
    add x12, sp, 4 * BF_SIZE
    add x13, sp, 4 * BF_SIZE
    bl bf_add
9:  ldr x12, [sp, 7 * BF_SIZE + 8]
    add x13, sp, 4 * BF_SIZE
    bl bf_copy
    BF_LEAVE 7

# atan u = u + u Q: [x12] = the tail u Q, Q = -u^2/3 + u^4/5 - ...
k_atan_tail:
    BF_ENTER 3                      // 0 u, 1 u^2, 2 Q
    str x12, [sp, 3 * BF_SIZE + 8]
    add x12, sp, 0
    bl bf_copy
    BF3 bf_mul, 1, 0, 0
    BF_FXSERIES 2, 1, fx_odd, fx_odd_end, 16, 1, 1
    ldr x12, [sp, 3 * BF_SIZE + 8]
    add x13, sp, 0
    add x14, sp, 2 * BF_SIZE
    bl bf_mul
    BF_LEAVE 3

# F2XM1: A = 2^A - 1 = expm1(A ln 2), A finite, nonzero, |A| < 1:
# expm1 t = t + t^2 T, T = 1/2! + t/3! + t^2/4! + ... (alternating in |t| for
# t < 0).
v_f2xm1:
    BF_ENTER 4                      // 0 x, 1 t, 2 E, 3 ln 2
    add x12, sp, 0
    bl bf_from_f80
    BFC 3, bfc_ln2
    BF3 bf_mul, 1, 0, 3
    add x13, sp, 1 * BF_SIZE
    bl bf_fx
    adr x16, fx_fact2
    adr x17, fx_fact_end
    ldr x0, [sp, 1 * BF_SIZE + 24]
    orr x0, x0, 16 << 8
    bl fx_series
    mov x11, 16382
    mov x17, 0
    add x12, sp, 2 * BF_SIZE
    bl bf_put
    BF3 bf_mul, 2, 1, 2
    BF3 bf_mul, 2, 1, 2
    BF3 bf_add, 2, 1, 2
    bf_round_slot 2 * BF_SIZE
    BF_LEAVE 4

# FYL2X: A = B * log2(A), A finite and positive, B finite and nonzero.
v_fyl2x:
    BF_ENTER 3                      // 0 x, then the product; 1 log2; 2 y
    stp x5, x6, [sp, 3 * BF_SIZE + 8]
    add x12, sp, 0
    bl bf_from_f80
    add x12, sp, 1 * BF_SIZE
    add x13, sp, 0
    bl k_log2
    ldp x3, x4, [sp, 3 * BF_SIZE + 8]
    add x12, sp, 2 * BF_SIZE
    bl bf_from_f80
    BF3 bf_mul, 0, 2, 1
    bf_round_slot 0
    BF_LEAVE 3

# FYL2XP1: A = B * log2(1 + A), A finite and above -1, B finite and nonzero.
# |A| < 1/4: 2 atanh(A / (2 + A)), exact in A's own scale; else log2 of 1 + A.
v_fyl2xp1:
    BF_ENTER 3                      // 0 x, 1 log2, 2 scratch / y
    stp x5, x6, [sp, 3 * BF_SIZE + 8]
    and w0, w4, 0x7fff
    mov w9, 0x3ffd
    cmp w0, w9
    add x12, sp, 0
    b.hs 1f
    bl bf_from_f80
    BFI 2, 2
    BF3 bf_add, 2, 2, 0
    BF3 bf_div, 2, 0, 2
    add x12, sp, 1 * BF_SIZE
    add x13, sp, 2 * BF_SIZE
    bl k_atanh_log2
    b 2f
1:  bl bf_from_f80
    BFI 2, 1
    BF3 bf_add, 2, 2, 0
    add x12, sp, 1 * BF_SIZE
    add x13, sp, 2 * BF_SIZE
    bl k_log2
2:  ldp x3, x4, [sp, 3 * BF_SIZE + 8]
    add x12, sp, 2 * BF_SIZE
    bl bf_from_f80
    BF3 bf_mul, 0, 2, 1
    bf_round_slot 0
    BF_LEAVE 3

# FPATAN: A = atan2(B, A) (y = B, x = A), both finite and nonzero.
v_fpatan:
    BF_ENTER 6                      // 0 |y|, 1 |x|, 2 t, 3 atan, 4 tail / pi, 5 scratch
    ubfx w9, w4, 15, 1
    ubfx w10, w6, 15, 1
    stp x9, x10, [sp, 6 * BF_SIZE + 8]    // x's sign, y's sign
    add x12, sp, 1 * BF_SIZE
    bl bf_from_f80
    str xzr, [sp, 1 * BF_SIZE + 24]
    mov x3, x5
    mov w4, w6
    add x12, sp, 0
    bl bf_from_f80
    str xzr, [sp, 24]
    // |y| <= |x|?
    ldp x9, x10, [sp, 16]           // |y|: e (x9)
    ldr x11, [sp, 1 * BF_SIZE + 16] // |x|: e
    cmp x9, x11
    b.gt 5f
    b.lt 1f
    ldr x9, [sp, 8]
    ldr x10, [sp, 1 * BF_SIZE + 8]
    cmp x9, x10
    b.hi 5f
    b.lo 1f
    ldr x9, [sp, 0]
    ldr x10, [sp, 1 * BF_SIZE]
    cmp x9, x10
    b.hi 5f
1:  BF3 bf_div, 2, 0, 1             // t = |y| / |x|, in [0, 1]
    ldr x0, [sp, 6 * BF_SIZE + 8]
    cbnz x0, 2f                     // x < 0: through pi
    // atan(t) of a t below 1/32: t + t Q, joined with a sticky bit
    ldr x9, [sp, 2 * BF_SIZE + 16]
    mov x10, 16378                  // 2^-5
    cmp x9, x10
    b.ge 2f
    add x12, sp, 4 * BF_SIZE
    add x13, sp, 2 * BF_SIZE
    bl k_atan_tail
    ldr x0, [sp, 6 * BF_SIZE + 16]  // y's sign on both
    str x0, [sp, 2 * BF_SIZE + 24]
    ldr x9, [sp, 4 * BF_SIZE + 24]
    eor x9, x9, x0
    str x9, [sp, 4 * BF_SIZE + 24]
    add x13, sp, 2 * BF_SIZE
    add x14, sp, 4 * BF_SIZE
    bl bf_join
    bl x87_round_bf
    BF_LEAVE 6
2:  add x12, sp, 3 * BF_SIZE
    add x13, sp, 2 * BF_SIZE
    bl k_atan
    b 6f
5:  BF3 bf_div, 2, 1, 0             // t = |x| / |y|: pi/2 - atan t
    add x12, sp, 3 * BF_SIZE
    add x13, sp, 2 * BF_SIZE
    bl k_atan
    BFC 4, bfc_pi2
    BF3 bf_sub, 3, 4, 3
6:  ldr x0, [sp, 6 * BF_SIZE + 8]
    cbz x0, 7f
    BFC 4, bfc_pi                   // x < 0: pi - that
    BF3 bf_sub, 3, 4, 3
7:  ldr x0, [sp, 6 * BF_SIZE + 16]
    str x0, [sp, 3 * BF_SIZE + 24]
    bf_round_slot 3 * BF_SIZE
    BF_LEAVE 6
    .balign 16
bfc_ln2:
    .quad 0xc9e3b39803f2f6af, 0xb17217f7d1cf79ab, 16382, 0
bfc_log2e:
    .quad 0xbe87fed0691d3e88, 0xb8aa3b295c17f0bb, 16383, 0
bfc_pi:
    .quad 0xc4c6628b80dc1cd1, 0xc90fdaa22168c234, 16384, 0
bfc_pi2:
    .quad 0xc4c6628b80dc1cd1, 0xc90fdaa22168c234, 16383, 0
bfc_atan16:
    .quad 0x0000000000000000, 0x0000000000000000, 0, 0    // atan(0/16)
    .quad 0xcb2792dc0e2e0d51, 0xffaaddb967ef4e36, 16378, 0    // atan(1/16)
    .quad 0xc897989f3e888ef7, 0xfeadd4d5617b6e32, 16379, 0    // atan(2/16)
    .quad 0x7b0b4f881c9c7487, 0xbdcbda5e72d81134, 16380, 0    // atan(3/16)
    .quad 0x6dc79ef5f7a217e5, 0xfadbafc96406eb15, 16380, 0    // atan(4/16)
    .quad 0xc5abb498d27af328, 0x9b13b9b83f5e5e69, 16381, 0    // atan(5/16)
    .quad 0x8aa32122dcfe4483, 0xb7b0ca0f26f78473, 16381, 0    // atan(6/16)
    .quad 0x427c95e9001e7136, 0xd327761e611fe5b6, 16381, 0    // atan(7/16)
    .quad 0x6fe445ecbc3a8d03, 0xed63382b0dda7b45, 16381, 0    // atan(8/16)
    .quad 0x4b6a09cb61a515c0, 0x832bf4a6d9867e2a, 16382, 0    // atan(9/16)
    .quad 0x5c835e1665c43747, 0x8f005d5ef7f59f9b, 16382, 0    // atan(10/16)
    .quad 0x4226f8e2204ff3bc, 0x9a2f80e671bdda20, 16382, 0    // atan(11/16)
    .quad 0x19a87f2a457dac9e, 0xa4bc7d1934f70924, 16382, 0    // atan(12/16)
    .quad 0x14725e2f3e52070a, 0xaeac4c38b4d8c080, 16382, 0    // atan(13/16)
    .quad 0xcb2da55210a4443d, 0xb8053e2bc2319e73, 16382, 0    // atan(14/16)
    .quad 0x89dd62c46e92fa24, 0xc0ce85b8ac526640, 16382, 0    // atan(15/16)
    .quad 0xc4c6628b80dc1cd1, 0xc90fdaa22168c234, 16382, 0    // atan(16/16)
// pi66 = P66 * 2^-64: P66 = 0x3243f6a8885a308d3 (66 bits)
x87_p66: .quad 0x243f6a8885a308d3, 0x3

// Flip the signs of slots a and b.
.macro BF_FLIP2 a, b
    ldr x0, [sp, (\a) * BF_SIZE + 24]
    eor x0, x0, 1
    str x0, [sp, (\a) * BF_SIZE + 24]
    ldr x0, [sp, (\b) * BF_SIZE + 24]
    eor x0, x0, 1
    str x0, [sp, (\b) * BF_SIZE + 24]
.endm

// lead (slot a) + tail (slot b), joined and rounded into A.
.macro BF_JOIN_ROUND a, b
    add x13, sp, (\a) * BF_SIZE
    add x14, sp, (\b) * BF_SIZE
    bl bf_join
    bl x87_round_bf
.endm

// lead (slot a) + tail (slot b), joined, to slot d as a bigfloat.
.macro BF_JOIN_PUT d, a, b
    add x13, sp, (\a) * BF_SIZE
    add x14, sp, (\b) * BF_SIZE
    bl bf_join
    mov w17, w11
    mov x11, x10
    mov x10, x8
    add x12, sp, (\d) * BF_SIZE
    bl bf_put
.endm

# FSIN (w0 = 0), FCOS (1), FPTAN (2), FSINCOS (3), A finite, nonzero, below
# 2^63: A = the sine, cosine or tangent; for FSINCOS A = the sine, B = the
# cosine (rounded first).
#
# The reduction is the x87's own: |x| = q pi66/2 + r, q the nearest integer
# and pi66 = P66 * 2^-64 -- pi to 66 bits, which is why a large x's sine is
# not the true one, on the hardware or here. In units of 2^-65, X = |x| is
# an integer below 2^128 and pi66/2 is P66; q and the remainder R come from
# a long division, R exact. Then sin r = r + r Ps and cos r = 1 + Pc, the
# leading terms kept apart from the tails.
v_trig:
    BF_ENTER 9                      // 0 r, 1 r^2, 2 term, 3 Ps, 4 Pc, 5 r Ps, 6 one, 7, 8
    str x0, [sp, 9 * BF_SIZE + 8]
    ubfx w0, w4, 15, 1
    str x0, [sp, 9 * BF_SIZE + 16]
    and w9, w4, 0x7fff
    cmp w9, 0
    csinc w9, w9, wzr, ne
    mov w10, 16381
    subs w9, w9, w10                // the shift that makes X an integer
    b.ge 1f
    // |x| below 2^-65: r = |x|, q = 0
    and w4, w4, 0x7fff
    add x12, sp, 0
    bl bf_from_f80
    mov x0, 0
    b 5f
1:  // X = m << w9 (0-64): x8 the high word, x10 the low
    mov x10, 0
    mov x8, x3
    cmp w9, 64
    b.eq 2f
    mov x10, x3
    mov x8, 0
    cbz w9, 2f
    lsl x10, x3, x9
    neg w11, w9
    lsr x8, x3, x11
2:  mov x5, 0x08d3
    movk x5, 0x85a3, lsl 16
    movk x5, 0x6a88, lsl 32
    movk x5, 0x243f, lsl 48         // P66 = 3:x5
    mov x6, 3
    // q = floor(X / P66): X's high word times floor(2^64 / pi) is at most 2
    // under it, never over; then R = X - q P66, and up to q + 2
    mov x0, 0x0a94
    movk x0, 0x2722, lsl 16
    movk x0, 0xc1b7, lsl 32
    movk x0, 0x517c, lsl 48
    umulh x11, x8, x0
    mul x12, x11, x5
    umulh x13, x11, x5
    add x13, x13, x11, lsl 1
    add x13, x13, x11               // q P66 = x13:x12
    subs x12, x10, x12
    sbc x9, x8, x13
    mov x8, x12                     // R = x9:x8
    b 31f
3:  subs x8, x8, x5
    sbc x9, x9, x6
    add x11, x11, 1
31: cmp x8, x5
    sbcs xzr, x9, x6
    b.hs 3b
    // the nearest: R above P66 / 2 = 1:921fb54442d18469 (P66 is odd: no
    // ties), that is, at least 1:921fb54442d1846a
    mov x17, 0
    mov x12, 0x846a
    movk x12, 0x42d1, lsl 16
    movk x12, 0xb544, lsl 32
    movk x12, 0x921f, lsl 48
    mov x13, 1
    cmp x8, x12
    sbcs xzr, x9, x13
    b.lo 41f
    add x11, x11, 1
    subs x8, x5, x8
    sbc x9, x6, x9
    mov x17, 1
41: str x11, [sp, 7 * BF_SIZE]
    mov x10, x9
    mov x9, x8
    mov x11, 16445
    add x12, sp, 0
    bl bf_put
    ldr x0, [sp, 7 * BF_SIZE]
5:  and x0, x0, 3
    ldr x9, [sp, 9 * BF_SIZE + 16]
    orr x0, x0, x9, lsl 2           // q mod 4, and x's sign at bit 2
    str x0, [sp, 9 * BF_SIZE + 16]
    // the series: FSIN and FCOS need only the one their quadrant uses
    BF3 bf_mul, 1, 0, 0
    BFI 6, 1
    ldp x9, x10, [sp, 9 * BF_SIZE + 8]
    eor x10, x10, x9
    cmp x9, 2
    b.hs 52f
    tbnz x10, 0, 53f
52: BF_FXSERIES 3, 1, fx_sinc, fx_fact_end, 32, 1, 1
    BF3 bf_mul, 5, 0, 3
    ldr x9, [sp, 9 * BF_SIZE + 8]
    cmp x9, 2
    b.lo 54f
53: BF_FXSERIES 4, 1, fx_cosc, fx_fact_end, 32, 1, 1
54:
    ldr x0, [sp, 9 * BF_SIZE + 8]
    cmp x0, 2
    b.ne 6f
    ldr x0, [sp, 9 * BF_SIZE + 16]
    tbnz x0, 0, 6f
    // FPTAN, q even: tan r = r + r Q, Q = (Ps - Pc) / (1 + Pc)
    BF3 bf_sub, 7, 3, 4
    BF3 bf_add, 8, 6, 4
    BF3 bf_div, 7, 7, 8
    BF3 bf_mul, 7, 0, 7
    ldr x0, [sp, 9 * BF_SIZE + 16]
    tbz x0, 2, 51f
    BF_FLIP2 0, 7
51: BF_JOIN_ROUND 0, 7
    b 9f
6:  // the quadrant: S = sin, cos, -sin, -cos; C = cos, -sin, -cos, sin
    // (r q), then S negated for a negative x. Flips go onto the pairs:
    // S's is (q >= 2) ^ sign, C's (q is 1 or 2).
    ldr x0, [sp, 9 * BF_SIZE + 16]
    ubfx x9, x0, 1, 1
    ubfx x10, x0, 2, 1
    eor x9, x9, x10                 // S flips
    ubfx x10, x0, 0, 1
    ubfx x11, x0, 1, 1
    eor x10, x10, x11               // C flips
    stp x9, x10, [sp, 7 * BF_SIZE]  // (slot 7 as two scratch words)
    tbnz x0, 0, 7f
    cbz x9, 61f
    BF_FLIP2 0, 5
61: ldr x10, [sp, 7 * BF_SIZE + 8]
    cbz x10, 8f
    BF_FLIP2 6, 4
    b 8f
7:  cbz x9, 71f
    BF_FLIP2 6, 4
71: ldr x10, [sp, 7 * BF_SIZE + 8]
    cbz x10, 8f
    BF_FLIP2 0, 5
8:  // S is the sine pair (0, 5) for q even, the cosine pair (6, 4) for q odd
    ldr x0, [sp, 9 * BF_SIZE + 8]
    ldr x9, [sp, 9 * BF_SIZE + 16]
    cmp x0, 1
    b.eq 82f
    cmp x0, 2
    b.eq 83f
    cmp x0, 3
    b.eq 84f
    tbnz x9, 0, 81f                 // FSIN
    BF_JOIN_ROUND 0, 5
    b 9f
81: BF_JOIN_ROUND 6, 4
    b 9f
82: tbnz x9, 0, 821f                // FCOS
    BF_JOIN_ROUND 6, 4
    b 9f
821: BF_JOIN_ROUND 0, 5
    b 9f
83: // FPTAN, q odd: S / C, each joined
    BF_JOIN_PUT 7, 6, 4
    BF_JOIN_PUT 8, 0, 5
    BF3 bf_div, 7, 7, 8
    bf_round_slot 7 * BF_SIZE
    b 9f
84: tbnz x9, 0, 841f                // FSINCOS: the cosine, then the sine
    BF_JOIN_ROUND 6, 4
    stp x3, x4, [sp, 8 * BF_SIZE]
    BF_JOIN_ROUND 0, 5
    b 842f
841: BF_JOIN_ROUND 0, 5
    stp x3, x4, [sp, 8 * BF_SIZE]
    BF_JOIN_ROUND 6, 4
842: ldp x5, x6, [sp, 8 * BF_SIZE]
9:  BF_LEAVE 9
bfc_pi4:
    .quad 0xc4c6628b80dc1cd1, 0xc90fdaa22168c234, 16382, 0
bfc_pi34:
    .quad 0x9394c9e8a0a5159c, 0x96cbe3f9990e91a7, 16384, 0

# Series coefficients as 128-bit binary fractions (floor(2^128 c)): 1/n! for
# n = 2..40 -- the sine takes the odd ones from 1/3!, the cosine the even
# ones from 1/2!, expm1 all of them -- and 1/(2k+1) for the arctangents.
    .balign 16
fx_fact2:
fx_cosc:
    .quad 0x0000000000000000, 0x8000000000000000   // 1/2!
fx_sinc:
    .quad 0xaaaaaaaaaaaaaaaa, 0x2aaaaaaaaaaaaaaa   // 1/3!
    .quad 0xaaaaaaaaaaaaaaaa, 0x0aaaaaaaaaaaaaaa   // 1/4!
    .quad 0x2222222222222222, 0x0222222222222222   // 1/5!
    .quad 0x05b05b05b05b05b0, 0x005b05b05b05b05b   // 1/6!
    .quad 0x00d00d00d00d00d0, 0x000d00d00d00d00d   // 1/7!
    .quad 0xa01a01a01a01a01a, 0x0001a01a01a01a01   // 1/8!
    .quad 0x671f5583911ca002, 0x00002e3bc74aad8e   // 1/9!
    .quad 0xd71cbbc05b4fa999, 0x0000049f93edde27   // 1/10!
    .quad 0x138e3f9d1f92e0df, 0x0000006b99159fd5   // 1/11!
    .quad 0x6c4bdaa26d4c3d67, 0x00000008f76c77fc   // 1/12!
    .quad 0x43684be51c198e91, 0x00000000b092309d   // 1/13!
    .quad 0x603e4e905d6f8a2e, 0x000000000c9cba54   // 1/14!
    .quad 0x399dc0f88ec32b58, 0x0000000000d73f9f   // 1/15!
    .quad 0xf399dc0f88ec32b5, 0x00000000000d73f9   // 1/16!
    .quad 0x3b81856a53593028, 0x000000000000ca96   // 1/17!
    .quad 0x3c31dcbecbbdd802, 0x0000000000000b41   // 1/18!
    .quad 0xa4da340a0ab92650, 0x0000000000000097   // 1/19!
    .quad 0x950ae900808941ea, 0x0000000000000007   // 1/20!
    .quad 0x5c6e3bdb73d5c62f, 0x0000000000000000   // 1/21!
    .quad 0x04338e5b6dfe14a5, 0x0000000000000000   // 1/22!
    .quad 0x002ec368262c7033, 0x0000000000000000   // 1/23!
    .quad 0x0001f2cf01972f57, 0x0000000000000000   // 1/24!
    .quad 0x000013f3ccdd165f, 0x0000000000000000   // 1/25!
    .quad 0x000000c4742fe352, 0x0000000000000000   // 1/26!
    .quad 0x0000000746ac70b7, 0x0000000000000000   // 1/27!
    .quad 0x0000000042862898, 0x0000000000000000   // 1/28!
    .quad 0x00000000024b3f31, 0x0000000000000000   // 1/29!
    .quad 0x000000000013932c, 0x0000000000000000   // 1/30!
    .quad 0x000000000000a1a6, 0x0000000000000000   // 1/31!
    .quad 0x000000000000050d, 0x0000000000000000   // 1/32!
    .quad 0x0000000000000027, 0x0000000000000000   // 1/33!
    .quad 0x0000000000000001, 0x0000000000000000   // 1/34!
    .quad 0x0000000000000000, 0x0000000000000000   // 1/35!
    .quad 0x0000000000000000, 0x0000000000000000   // 1/36!
    .quad 0x0000000000000000, 0x0000000000000000   // 1/37!
    .quad 0x0000000000000000, 0x0000000000000000   // 1/38!
    .quad 0x0000000000000000, 0x0000000000000000   // 1/39!
    .quad 0x0000000000000000, 0x0000000000000000   // 1/40!
fx_fact_end:
fx_odd:
    .quad 0x5555555555555555, 0x5555555555555555   // 1/3
    .quad 0x3333333333333333, 0x3333333333333333   // 1/5
    .quad 0x4924924924924924, 0x2492492492492492   // 1/7
    .quad 0xc71c71c71c71c71c, 0x1c71c71c71c71c71   // 1/9
    .quad 0x745d1745d1745d17, 0x1745d1745d1745d1   // 1/11
    .quad 0x3b13b13b13b13b13, 0x13b13b13b13b13b1   // 1/13
    .quad 0x1111111111111111, 0x1111111111111111   // 1/15
    .quad 0x0f0f0f0f0f0f0f0f, 0x0f0f0f0f0f0f0f0f   // 1/17
    .quad 0xe50d79435e50d794, 0x0d79435e50d79435   // 1/19
    .quad 0xc30c30c30c30c30c, 0x0c30c30c30c30c30   // 1/21
    .quad 0x42c8590b21642c85, 0x0b21642c8590b216   // 1/23
    .quad 0xa3d70a3d70a3d70a, 0x0a3d70a3d70a3d70   // 1/25
    .quad 0xed097b425ed097b4, 0x097b425ed097b425   // 1/27
    .quad 0xd3dcb08d3dcb08d3, 0x08d3dcb08d3dcb08   // 1/29
    .quad 0x8421084210842108, 0x0842108421084210   // 1/31
    .quad 0x7c1f07c1f07c1f07, 0x07c1f07c1f07c1f0   // 1/33
    .quad 0x7507507507507507, 0x0750750750750750   // 1/35
    .quad 0x5306eb3e45306eb3, 0x06eb3e45306eb3e4   // 1/37
    .quad 0x6906906906906906, 0x0690690690690690   // 1/39
    .quad 0x63e7063e7063e706, 0x063e7063e7063e70   // 1/41
    .quad 0xf417d05f417d05f4, 0x05f417d05f417d05   // 1/43
    .quad 0x5b05b05b05b05b05, 0x05b05b05b05b05b0   // 1/45
    .quad 0x882b9310572620ae, 0x0572620ae4c415c9   // 1/47
    .quad 0x0a72f05397829cbc, 0x05397829cbc14e5e   // 1/49
    .quad 0x0505050505050505, 0x0505050505050505   // 1/51
    .quad 0x4873ecade304d487, 0x04d4873ecade304d   // 1/53
    .quad 0x4a7904a7904a7904, 0x04a7904a7904a790   // 1/55
    .quad 0xf7047dc11f7047dc, 0x047dc11f7047dc11   // 1/57
    .quad 0x15b1e5f75270d045, 0x0456c797dd49c341   // 1/59
    .quad 0x4325c53ef368eb04, 0x04325c53ef368eb0   // 1/61
    .quad 0x4104104104104104, 0x0410410410410410   // 1/63
    .quad 0x3f03f03f03f03f03, 0x03f03f03f03f03f0   // 1/65
    .quad 0x40f4898d5f85bb39, 0x03d226357e16ece5   // 1/67
    .quad 0xc0ed7303b5cc0ed7, 0x03b5cc0ed7303b5c   // 1/69
    .quad 0x240e6c2b4481cd85, 0x039b0ad12073615a   // 1/71
    .quad 0x070381c0e070381c, 0x0381c0e070381c0e   // 1/73
    .quad 0x369d0369d0369d03, 0x0369d0369d0369d0   // 1/75
    .quad 0x3531dec0d4c77b03, 0x03531dec0d4c77b0   // 1/77
    .quad 0xa5440cf6474a8819, 0x033d91d2a2067b23   // 1/79
    .quad 0xa4587e6b74f03291, 0x0329161f9add3c0c   // 1/81
    .quad 0x6f0940c565c87b5f, 0x03159721ed7e7534   // 1/83
    .quad 0x0303030303030303, 0x0303030303030303   // 1/85
    .quad 0xf149902f149902f1, 0x02f149902f149902   // 1/87
    .quad 0xc0b81702e05c0b81, 0x02e05c0b81702e05   // 1/89
    .quad 0x2d02d02d02d02d02, 0x02d02d02d02d02d0   // 1/91
    .quad 0x2c0b02c0b02c0b02, 0x02c0b02c0b02c0b0   // 1/93
    .quad 0x6102b1da46102b1d, 0x02b1da46102b1da4   // 1/95
    .quad 0xa0fd5c5f02a3a0fd, 0x02a3a0fd5c5f02a3   // 1/97
    .quad 0x295fad40a57eb502, 0x0295fad40a57eb50   // 1/99
    .quad 0xc83cd4e930288df0, 0x0288df0cac5b3f5d   // 1/101
    .quad 0x88b2f392a409f116, 0x027c45979c95204f   // 1/103
    .quad 0x2702702702702702, 0x0270270270270270   // 1/105
    .quad 0xdc1cb5d4ef40991f, 0x02647c69456217ec   // 1/107
    .quad 0x9b02593f69b02593, 0x02593f69b02593f6   // 1/109
    .quad 0x71024e6a171024e6, 0x024e6a171024e6a1   // 1/111
    .quad 0x43f6f0243f6f0243, 0x0243f6f0243f6f02   // 1/113
    .quad 0x0d5b450239e0d5b4, 0x0239e0d5b450239e   // 1/115
    .quad 0x2302302302302302, 0x0230230230230230   // 1/117
    .quad 0xb90226b90226b902, 0x0226b90226b90226   // 1/119
    .quad 0xc4c2a50658dc0876, 0x021d9ead7cd391fb   // 1/121
    .quad 0x214d0214d0214d02, 0x0214d0214d0214d0   // 1/123
    .quad 0xed916872b020c49b, 0x020c49ba5e353f7c   // 1/125
    .quad 0x0408102040810204, 0x0204081020408102   // 1/127
    .quad 0xfc07f01fc07f01fc, 0x01fc07f01fc07f01   // 1/129
fx_odd_end:

# ---------------------------------------------------------------------------
# The transcendentals' special operands, as an AMD Ryzen takes them (camd's
# records: tests/manual/x86/x87_trans.c). The value routines above get only
# what is left: finite, nonzero, in the domain.
# ---------------------------------------------------------------------------

// x87_ret_a and the rest again, within a tbz's reach.
xt_ret_a:
    ldr lr, [sp], 16
    ret
xt_ret_invalid:
    orr w7, w7, X87_IE
    x87_indef x3, w4
    ldr lr, [sp], 16
    ret
xt_ret_nan:
    bl x87_nan2
    ldr lr, [sp], 16
    ret

// A = the constant [x13], sign w11, rounded: PE, and C1 if it rounded up.
x87_round_const:
    ldp x9, x8, [x13]
    ldr x10, [x13, 16]
    b x87_round_bf

// The NaN of a one-operand transcendental: A quieted, IE for an SNaN (class
// w12); B the same, for the two-result ones.
x87_tnan1:
    cmp w12, 5
    b.ne 1f
    orr w7, w7, X87_IE
1:  orr x3, x3, 0x4000000000000000
    mov x5, x3
    mov w6, w4
    b xt_ret_a

# F2XM1: A = 2^A - 1. Out of its domain (|A| > 1), A itself with PE.
x87_f2xm1_core:
    str lr, [sp, -16]!
    x87_class w12, x3, w4, w0
    cmp w12, 6
    b.eq xt_ret_invalid
    cmp w12, 4
    b.hs x87_tnan1
    cbz w12, xt_ret_a              // +-0: itself
    cmp w12, 3
    b.ne 1f
    tbz w4, 15, xt_ret_a           // +infinity: itself; -infinity: -1
    mov w4, 0xbfff
    b xt_ret_a
1:  cmp w12, 2
    b.ne 2f
    orr w7, w7, X87_DE
    tbz w15, 1, xt_ret_a           // DE unmasked: nothing happens
    b 4f
2:  and w9, w4, 0x7fff
    mov w10, 0x3fff
    cmp w9, w10
    b.lo 4f
    b.hi 3f
    mov x10, 0x8000000000000000
    cmp x3, x10
    b.ne 3f
    // +-1: -1 gives -0.5; +1 gives 1 rounding to nearest or up (with C1),
    // else the largest value below it
    orr w7, w7, X87_PE
    tbz w4, 15, 5f
    mov w4, 0xbffe
    b xt_ret_a
5:  tbnz w15, 10, 6f                // RC 01 (down) or 11 (toward zero)
    orr w7, w7, X87_C1
    b xt_ret_a
6:  mov x3, -1
    mov w4, 0x3ffe
    b xt_ret_a
3:  orr w7, w7, X87_PE
    b xt_ret_a
4:  bl v_f2xm1
    b xt_ret_a

# FSIN (w0 = 0), FCOS (1), FPTAN (2), FSINCOS (3) on A. FPTAN's B is the 1
# it pushes, FSINCOS's the cosine; A the value that replaces ST(0). An
# operand of 2^63 or more is out of range: C2 set, nothing else changes.
x87_trig_core:
    stp lr, x0, [sp, -16]!
    x87_class w12, x3, w4, w9
    cmp w12, 3
    b.eq 7f
    cmp w12, 6
    b.eq 7f
    cmp w12, 4
    b.hs x87_tnan1
    cbz w12, 5f
    and w9, w4, 0x7fff
    mov w10, 0x403e
    cmp w9, w10
    b.hs 6f
    cmp w12, 2
    b.ne 1f
    orr w7, w7, X87_DE
    tbz w15, 1, xt_ret_a
1:  bl v_trig
    ldr x0, [sp, 8]
    cmp w0, 2
    b.ne xt_ret_a
    mov x5, 0x8000000000000000      // FPTAN pushes 1
    mov w6, 0x3fff
    b xt_ret_a
5:  // zero: the sine and tangent are the zero, the cosine 1, exactly
    cbz w0, xt_ret_a
    mov x5, 0x8000000000000000
    mov w6, 0x3fff
    cmp w0, 1
    b.ne xt_ret_a
    mov x3, x5
    mov w4, w6
    b xt_ret_a
6:  orr w7, w7, X87_C2
    b xt_ret_a
7:  orr w7, w7, X87_IE              // infinity, unsupported: the indefinite
    x87_indef x3, w4
    mov x5, x3
    mov w6, w4
    b xt_ret_a

# Two operands, A = ST(0) = x and B = ST(1) = y: unsupported is invalid, then
# the NaN rule; w12, w13 the classes, w14 DE if either is a denormal (the
# cases below raise it or not as the hardware does).
.macro x87_tcheck2
    bl x87_cls2
    cmp w12, 6
    ccmp w13, 6, 4, ne
    b.eq xt_ret_invalid
    cmp w12, 4
    ccmp w13, 4, 2, lo
    b.hs xt_ret_nan
    cmp w12, 2
    ccmp w13, 2, 4, ne
    cset w14, eq
    lsl w14, w14, 1
.endm
// DE as w14 says, and if it is unmasked, nothing happens.
.macro x87_tde
    orr w7, w7, w14
    bic w9, w14, w15
    cbnz w9, xt_ret_a
.endm

# FYL2X: A = y log2 x.
x87_fyl2x_core:
    str lr, [sp, -16]!
    x87_tcheck2
    cbz w12, 1f
    tbnz w4, 15, xt_ret_invalid    // x < 0
    cmp w12, 3
    b.eq 2f
    and w9, w4, 0x7fff
    mov w10, 0x3fff
    cmp w9, w10
    cset w10, lo                    // x < 1: the result's sign flips
    b.ne 3f
    mov x9, 0x8000000000000000
    cmp x3, x9
    b.ne 3f
    // x = 1: a zero with y's sign; y infinite is invalid
    cmp w13, 3
    b.eq xt_ret_invalid
    x87_tde
    mov x3, 0
    and w4, w6, 0x8000
    b xt_ret_a
3:  cbz w13, 4f
    cmp w13, 3
    b.eq 5f
    x87_tde
    bl v_fyl2x
    b xt_ret_a
4:  x87_tde                 // y zero: a zero
    mov x3, 0
    eor w4, w6, w10, lsl 15
    and w4, w4, 0x8000
    b xt_ret_a
5:  x87_tde                 // y infinite: an infinity
    mov x3, x5
    eor w4, w6, w10, lsl 15
    b xt_ret_a
1:  // x zero: -y infinity, ZE unless y is infinite; y zero is invalid
    cbz w13, xt_ret_invalid
    cmp w13, 3
    b.eq 11f
    orr w7, w7, X87_ZE
11: mov x3, 0x8000000000000000
    mvn w4, w6
    and w4, w4, 0x8000
    mov w9, 0x7fff
    orr w4, w4, w9
    b xt_ret_a
2:  // x +infinity: infinity with y's sign; y zero is invalid
    cbz w13, xt_ret_invalid
    x87_tde
    and w4, w6, 0x8000
    mov w9, 0x7fff
    orr w4, w4, w9
    b xt_ret_a

# FYL2XP1: A = y log2(1 + x). An x at or below -1 is A itself, with PE.
x87_fyl2xp1_core:
    str lr, [sp, -16]!
    x87_tcheck2
    cbz w12, 1f
    cmp w12, 3
    b.eq 2f
    tbz w4, 15, 3f
    and w9, w4, 0x7fff
    mov w10, 0x3fff
    cmp w9, w10
    b.lo 3f
    // x <= -1
    cbz w13, 4f
    cmp w13, 3
    b.eq 5f
    x87_tde
    orr w7, w7, X87_PE
    b xt_ret_a
4:  x87_tde                 // y zero: -y
    mov x3, 0
    mvn w4, w6
    and w4, w4, 0x8000
    b xt_ret_a
5:  mov x3, x5                      // y infinite: -y
    eor w4, w6, 0x8000
    b xt_ret_a
3:  eor w10, w4, w6
    and w10, w10, 0x8000            // the result's sign, for a zero or infinite y
    cbz w13, 31f
    cmp w13, 3
    b.eq 32f
    x87_tde
    bl v_fyl2xp1
    b xt_ret_a
31: x87_tde
    mov x3, 0
    mov w4, w10
    b xt_ret_a
32: x87_tde
    mov x3, x5
    mov w9, 0x7fff
    orr w4, w10, w9
    b xt_ret_a
1:  // x zero: a zero, the signs' product; y infinite is invalid
    cmp w13, 3
    b.eq xt_ret_invalid
    x87_tde
    mov x3, 0
    eor w4, w4, w6
    and w4, w4, 0x8000
    b xt_ret_a
2:  // x infinite: -infinity is invalid, so is y zero; else y's infinity
    tbnz w4, 15, xt_ret_invalid
    cbz w13, xt_ret_invalid
    x87_tde
    and w4, w6, 0x8000
    mov w9, 0x7fff
    orr w4, w4, w9
    b xt_ret_a

# FPATAN: A = atan2(y, x), the IEEE conventions for zeros and infinities.
x87_fpatan_core:
    str lr, [sp, -16]!
    x87_tcheck2
    ubfx w11, w6, 15, 1             // the result has y's sign
    cbnz w13, 1f
    // y zero: x negative, +-pi; else y
    x87_tde
    tbz w4, 15, 11f
    adr x13, bfc_pi
    bl x87_round_const
    b xt_ret_a
11: mov x3, x5
    mov w4, w6
    b xt_ret_a
1:  cbz w12, 2f
    cmp w13, 3
    b.ne 3f
    cmp w12, 3
    b.eq 4f
2:  // x zero, or y infinite and x finite: +-pi/2
    x87_tde
    adr x13, bfc_pi2
    bl x87_round_const
    b xt_ret_a
3:  cmp w12, 3
    b.ne 5f
    // x infinite, y finite: x negative, +-pi; else a zero
    x87_tde
    tbz w4, 15, 31f
    adr x13, bfc_pi
    bl x87_round_const
    b xt_ret_a
31: mov x3, 0
    and w4, w6, 0x8000
    b xt_ret_a
4:  // both infinite: +-pi/4, or +-3pi/4 for x negative
    adr x13, bfc_pi4
    tbz w4, 15, 41f
    adr x13, bfc_pi34
41: bl x87_round_const
    b xt_ret_a
5:  x87_tde
    bl v_fpatan
    b xt_ret_a

# ---------------------------------------------------------------------------
# The transcendental gadgets.
# ---------------------------------------------------------------------------

// C2 from w7 into the status word.
x87_put_c2:
    ldrh w8, [_cpu, CPU_fsw]
    ubfx w9, w7, 10, 1
    bfi w8, w9, 10, 1
    strh w8, [_cpu, CPU_fsw]
    ret

// ST(0) = A, then push B, C2 clear -- unless an unmasked IE, DE or ZE,
// which change nothing.
x87_put_push:
    ldrh w8, [_cpu, CPU_fsw]
    bic w8, w8, X87_C2
    bic w10, w7, w15
    tst w10, (X87_IE | X87_DE | X87_ZE)
    b.ne x87_put_fsw
    strh w8, [_cpu, CPU_fsw]
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
    mov x3, x5
    mov w4, w6
    b x87_push

.gadget x87_f2xm1
    x87_wait
    x87_begin
    mov w0, 0
    bl x87_get1
    cbnz w0, 1f
    bl x87_f2xm1_core
1:  mov w0, 0
    bl x87_put
    gret_nomem 1

// FSIN, FCOS: ST(0) replaced, C2 for an operand out of range.
.macro x87_trig1 name, op
.gadget x87_\name
    x87_wait
    x87_begin
    mov w0, 0
    bl x87_get1
    cbnz w0, 1f
    mov w0, \op
    bl x87_trig_core
1:  bl x87_put_c2
    mov w0, 0
    bl x87_put
    gret_nomem 1
.endm
x87_trig1 fsin, 0
x87_trig1 fcos, 1

// FPTAN, FSINCOS: ST(0) replaced and a value pushed. A full stack overflows
// (IE, SF, C1, the indefinite in ST(0) and pushed) -- unless the operand is
// out of range, which is only C2.
.macro x87_trig2 name, op
.gadget x87_\name
    x87_wait
    x87_begin
    mov w0, 0
    bl x87_get1
    cbnz w0, 2f
    x87_class w12, x3, w4, w0
    ldrh w8, [_cpu, CPU_fsw]
    ldrb w11, [_cpu, CPU_x87_valid]
    ubfx w9, w8, 11, 3
    sub w9, w9, 1
    and w9, w9, 7
    lsr w11, w11, w9
    tbz w11, 0, 1f
    cmp w12, 1
    b.ne 3f
    and w9, w4, 0x7fff
    mov w10, 0x403e
    cmp w9, w10
    b.lo 3f
1:  mov w0, \op
    bl x87_trig_core
    tbnz w7, 10, 5f
    b 4f
2:  mov x5, x3                      // underflow: the indefinite, twice
    mov w6, w4
    b 4f
3:  x87_indef x3, w4
    mov x5, x3
    mov w6, w4
    mov w7, (X87_IE | X87_SF | X87_C1)
4:  bl x87_put_push
    gret_nomem 1
5:  ldrh w8, [_cpu, CPU_fsw]
    orr w8, w8, X87_C2
    bl x87_put_fsw
    gret_nomem 1
.endm
x87_trig2 fptan, 2
x87_trig2 fsincos, 3

// FYL2X, FYL2XP1, FPATAN: ST(1) = the result, then pop.
.macro x87_t2 name
.gadget x87_\name
    x87_wait
    x87_begin
    mov w0, 0
    mov w10, 1
    bl x87_get2
    cbnz w0, 1f
    bl x87_\name\()_core
1:  mov w0, (1 << 3) | (1 << 6)
    bl x87_put
    gret_nomem 1
.endm
x87_t2 fyl2x
x87_t2 fyl2xp1
x87_t2 fpatan
