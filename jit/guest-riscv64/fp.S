// RISC-V F/D scalar FP gadgets (OP_FP plane) + the generic C-helper call.
//
// Register operands are cpu_state byte offsets like everything else in
// this port, so integer<->FP moves that are pure bit copies (fmv.d.x,
// fmv.x.d) don't even come here: gen emits the integer addi gadget with
// an f-slot offset. Only genuinely-FP semantics live in this file.
//
// Rounding: while guest code runs the host FPCR's rounding mode is frm (and
// DN is set, which gives RISC-V's canonical NaN), and the flags the host
// raises are gathered into fflags -- emu/fpenv.c. A dynamic rm (7) needs
// nothing more. A static rm on float->int picks the aarch64 convert that
// rounds that way; on arithmetic, gen brackets the op with rm_set and
// rm_restore, a pair of FPCR writes (~45 ns on an M5), which compilers never
// make it pay: they emit dynamic rm for arithmetic.
//
// S-format values are NaN-boxed: inputs read the low 32 bits (hardware
// treats improperly-boxed inputs as canonical NaN; we skip the check),
// outputs write the box (high 32 all-ones).
//
// Code stream: [gadget][rd_off][rs1_off][rs2_off], gret 3 (no memory
// access, no fault restart word needed). Unary ops ignore the rs2 slot.

#include "gadgets.h"

.text

// x9=rd_off; d16/d17 (or s16/s17) hold operands.
.macro fp_head
    ldr x9, [_ip]
    ldr x10, [_ip, #8]
    ldr x11, [_ip, #16]
    add x10, _cpu, x10
    add x11, _cpu, x11
.endm

// ---- op sub-macros: (result, a, b) ----
.macro fpop_add o, a, b
    fadd \o, \a, \b
.endm
.macro fpop_sub o, a, b
    fsub \o, \a, \b
.endm
.macro fpop_mul o, a, b
    fmul \o, \a, \b
.endm
.macro fpop_div o, a, b
    fdiv \o, \a, \b
.endm
// RISC-V fmin/fmax return the non-NaN operand = AArch64 fminnm/fmaxnm
.macro fpop_min o, a, b
    fminnm \o, \a, \b
.endm
.macro fpop_max o, a, b
    fmaxnm \o, \a, \b
.endm

.macro fp_d_gadget op
.gadget f\op\()_d
    fp_head
    ldr d16, [x10]
    ldr d17, [x11]
    fpop_\op d16, d16, d17
    str d16, [_cpu, x9]
    gret 3
.endm

.macro fp_s_gadget op
.gadget f\op\()_s
    fp_head
    ldr s16, [x10]
    ldr s17, [x11]
    fpop_\op s16, s16, s17
    fmov w10, s16
    orr x10, x10, #0xffffffff00000000   // NaN-box
    str x10, [_cpu, x9]
    gret 3
.endm

.irp op, add,sub,mul,div,min,max
    fp_d_gadget \op
    fp_s_gadget \op
.endr

.gadget fsqrt_d
    fp_head
    ldr d16, [x10]
    fsqrt d16, d16
    str d16, [_cpu, x9]
    gret 3
.gadget fsqrt_s
    fp_head
    ldr s16, [x10]
    fsqrt s16, s16
    fmov w10, s16
    orr x10, x10, #0xffffffff00000000
    str x10, [_cpu, x9]
    gret 3

// ---- sign injection: pure bit ops, done in integer registers ----
// fsgnj: rd = {sign(b), magnitude(a)}; n variant flips b's sign; x xors.
// Spelled out (three ops x two widths; the masking differs per op).
.gadget fsgnj_d
    fp_head
    ldr x12, [x10]
    ldr x13, [x11]
    lsr x13, x13, #63
    bfi x12, x13, #63, #1
    str x12, [_cpu, x9]
    gret 3
.gadget fsgnjn_d
    fp_head
    ldr x12, [x10]
    ldr x13, [x11]
    mvn x13, x13
    lsr x13, x13, #63
    bfi x12, x13, #63, #1
    str x12, [_cpu, x9]
    gret 3
.gadget fsgnjx_d
    fp_head
    ldr x12, [x10]
    ldr x13, [x11]
    and x13, x13, #0x8000000000000000
    eor x12, x12, x13
    str x12, [_cpu, x9]
    gret 3
.gadget fsgnj_s
    fp_head
    ldr w12, [x10]
    ldr w13, [x11]
    lsr w13, w13, #31
    bfi w12, w13, #31, #1
    orr x12, x12, #0xffffffff00000000
    str x12, [_cpu, x9]
    gret 3
.gadget fsgnjn_s
    fp_head
    ldr w12, [x10]
    ldr w13, [x11]
    mvn w13, w13
    lsr w13, w13, #31
    bfi w12, w13, #31, #1
    orr x12, x12, #0xffffffff00000000
    str x12, [_cpu, x9]
    gret 3
.gadget fsgnjx_s
    fp_head
    ldr w12, [x10]
    ldr w13, [x11]
    and w13, w13, #0x80000000
    eor w12, w12, w13
    orr x12, x12, #0xffffffff00000000
    str x12, [_cpu, x9]
    gret 3

// ---- compares: rd (integer) = 0/1. Quiet NaN handling comes out right:
// fcmp with a NaN sets no eq/lt/le-satisfying flags. feq is a quiet compare
// (NV only for a signalling NaN); flt and fle signal on any NaN, so fcmpe.
.macro fcmp_gadget name, width, cond, cmp
.gadget \name
    fp_head
    ldr \width\()16, [x10]
    ldr \width\()17, [x11]
    \cmp \width\()16, \width\()17
    cset x12, \cond
    str x12, [_cpu, x9]
    gret 3
.endm
fcmp_gadget feq_d, d, eq, fcmp
fcmp_gadget flt_d, d, mi, fcmpe
fcmp_gadget fle_d, d, ls, fcmpe
fcmp_gadget feq_s, s, eq, fcmp
fcmp_gadget flt_s, s, mi, fcmpe
fcmp_gadget fle_s, s, ls, fcmpe

// ---- conversions ----
// int -> fp (rounding irrelevant for exact cases; RNE otherwise, fine)
.macro cvt_i2f_gadget name, insn, src, dst, box
.gadget \name
    fp_head
    ldr \src, [x10]
    \insn \dst, \src
.if \box
    fmov w10, \dst
    orr x10, x10, #0xffffffff00000000
    str x10, [_cpu, x9]
.else
    str \dst, [_cpu, x9]
.endif
    gret 3
.endm
cvt_i2f_gadget fcvt_d_w,  scvtf, w12, d16, 0
cvt_i2f_gadget fcvt_d_wu, ucvtf, w12, d16, 0
cvt_i2f_gadget fcvt_d_l,  scvtf, x12, d16, 0
cvt_i2f_gadget fcvt_d_lu, ucvtf, x12, d16, 0
cvt_i2f_gadget fcvt_s_w,  scvtf, w12, s16, 1
cvt_i2f_gadget fcvt_s_wu, ucvtf, w12, s16, 1
cvt_i2f_gadget fcvt_s_l,  scvtf, x12, s16, 1
cvt_i2f_gadget fcvt_s_lu, ucvtf, x12, s16, 1

// fp -> int: gen picks the variant by rm -- n/z/m/p/a for RNE, RTZ, RDN,
// RUP and RMM (fcvta is ties-away, exactly RMM), and x for DYN: frintx
// rounds by the host FPCR, which is frm, raising NX as needed, and the
// fcvtz after it is exact. A NaN converts to the largest value of the
// destination on RISC-V (aarch64 gives 0), and the invalid flag either way.
// W results sign-extend into rd per RV64, which also makes .wu's all-ones.
.macro cvt_f2i_gadget name, round, insn, src, dst, sext, nanval
.gadget \name
    fp_head
    ldr \src, [x10]
.ifnb \round
    \round \src, \src
.endif
    \insn \dst, \src
    fcmp \src, \src
    b.vc 1f
    mov x12, #\nanval
1:
.if \sext
    sxtw x12, w12
.endif
    str x12, [_cpu, x9]
    gret 3
.endm
.macro cvt_f2i_family rmode, round, cvt
cvt_f2i_gadget fcvt\rmode\()_w_d,  \round, \cvt\()s, d16, w12, 1, 0x7fffffff
cvt_f2i_gadget fcvt\rmode\()_wu_d, \round, \cvt\()u, d16, w12, 1, 0xffffffff
cvt_f2i_gadget fcvt\rmode\()_l_d,  \round, \cvt\()s, d16, x12, 0, 0x7fffffffffffffff
cvt_f2i_gadget fcvt\rmode\()_lu_d, \round, \cvt\()u, d16, x12, 0, 0xffffffffffffffff
cvt_f2i_gadget fcvt\rmode\()_w_s,  \round, \cvt\()s, s16, w12, 1, 0x7fffffff
cvt_f2i_gadget fcvt\rmode\()_wu_s, \round, \cvt\()u, s16, w12, 1, 0xffffffff
cvt_f2i_gadget fcvt\rmode\()_l_s,  \round, \cvt\()s, s16, x12, 0, 0x7fffffffffffffff
cvt_f2i_gadget fcvt\rmode\()_lu_s, \round, \cvt\()u, s16, x12, 0, 0xffffffffffffffff
.endm
cvt_f2i_family n, , fcvtn
cvt_f2i_family z, , fcvtz
cvt_f2i_family m, , fcvtm
cvt_f2i_family p, , fcvtp
cvt_f2i_family a, , fcvta
cvt_f2i_family x, frintx, fcvtz

// A static rounding mode on an arithmetic instruction: [rm_set][rmode],
// the op, then [rm_restore], which puts frm's mode back (RNE 0, RTZ 3,
// RDN 2, RUP 1 in FPCR.RMode; RMM has none and runs as RNE).
.gadget rm_set
    ldr x8, [_ip]
    mrs x9, fpcr
    bfi x9, x8, #22, #2
    msr fpcr, x9
    gret 1
.gadget rm_restore
    ldr w8, [_cpu, #CPU_riscv64_fcsr]
    ubfx w8, w8, #5, #3
    lsl w8, w8, #2
    mov w9, #0x1230
    lsr w9, w9, w8
    and w9, w9, #3
    mrs x10, fpcr
    bfi x10, x9, #22, #2
    msr fpcr, x10
    gret

// fp <-> fp width
.gadget fcvt_d_s
    fp_head
    ldr s16, [x10]
    fcvt d16, s16
    str d16, [_cpu, x9]
    gret 3
.gadget fcvt_s_d
    fp_head
    ldr d16, [x10]
    fcvt s16, d16
    fmov w10, s16
    orr x10, x10, #0xffffffff00000000
    str x10, [_cpu, x9]
    gret 3

// fmv.x.w: low 32 bits of the f-reg, SIGN-extended (spec); fmv.w.x boxes.
.gadget fmv_x_w
    fp_head
    ldrsw x12, [x10]
    str x12, [_cpu, x9]
    gret 3
.gadget fmv_w_x
    fp_head
    ldr w12, [x10]
    orr x12, x12, #0xffffffff00000000
    str x12, [_cpu, x9]
    gret 3

// ---- generic C helper call: [gadget][fn][arg], helper(cpu, arg) --------
// _ip (x28) is callee-saved, so it survives the call; save_c/restore_c
// preserve the gadget register file (_cpu/_tlb/_tmp and friends).
// Reused by the CSR ops now and the vendor-extension hook (plan 5b) later.
.gadget call_helper
    save_c
    // The documented x0/x1/x2 aliasing rule (guest-arm64 memory.S): _cpu
    // IS x1, so read it into x0 BEFORE loading the argument over it.
    ldr x9, [_ip]
    mov x0, _cpu
    ldr x1, [_ip, #8]
    blr x9
    restore_c
    gret 2

// ---- fused multiply-add family (4 register operands) ----------------------
// riscv->aarch64 mnemonic mapping is a crosswise trap, verified per-op:
//   riscv fmadd  (a*b)+c  -> aarch64 fmadd
//   riscv fmsub  (a*b)-c  -> aarch64 fnmsub (n*m - a)
//   riscv fnmsub -(a*b)+c -> aarch64 fmsub  (a - n*m)
//   riscv fnmadd -(a*b)-c -> aarch64 fnmadd (-a - n*m)
// Code stream: [gadget][rd_off][rs1_off][rs2_off][rs3_off], gret 4.
.macro fma_gadget name, insn, w, box
.gadget \name
    ldr x9, [_ip]
    ldr x10, [_ip, #8]
    ldr x11, [_ip, #16]
    ldr x12, [_ip, #24]
    add x10, _cpu, x10
    add x11, _cpu, x11
    add x12, _cpu, x12
    ldr \w\()16, [x10]
    ldr \w\()17, [x11]
    ldr \w\()18, [x12]
    \insn \w\()16, \w\()16, \w\()17, \w\()18
.if \box
    fmov w10, s16
    orr x10, x10, #0xffffffff00000000
    str x10, [_cpu, x9]
.else
    str d16, [_cpu, x9]
.endif
    gret 4
.endm

fma_gadget fmadd_d,  fmadd,  d, 0
fma_gadget fmsub_d,  fnmsub, d, 0
fma_gadget fnmsub_d, fmsub,  d, 0
fma_gadget fnmadd_d, fnmadd, d, 0
fma_gadget fmadd_s,  fmadd,  s, 1
fma_gadget fmsub_s,  fnmsub, s, 1
fma_gadget fnmsub_s, fmsub,  s, 1
fma_gadget fnmadd_s, fnmadd, s, 1
