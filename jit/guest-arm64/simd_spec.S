// AArch64 guest SIMD gadgets specialised by element size, whose operands
// arrive as byte offsets into cpu_state computed at translation time.
//
// The generic gadgets (simd.S's vext_to_gpr, vins_gpr, ins_elem, dup_elem,
// dup_elem_scalar, vmov_from_gpr) decode register numbers, the element size
// and the lane from a packed word on every execution, then branch on the
// size: a umov was ~25 host instructions, most of it that decoding, and
// these element moves are about a fifth of what software GL (llvmpipe)
// runs. Here gen.c resolves "V5.H[3]" to its byte offset in cpu_state once,
// and the gadget is the move itself.
//
// Code stream: [gadget][dst_off][src_off], byte offsets from _cpu, each its
// own word (fetched with one ldp: cheaper than unpacking a packed word, see
// alu_spec.S). A GPR operand is its arm64_regs slot (never register 31: gen.c keeps
// the XZR forms on the generic gadgets). Chosen in gen.c by
// gen_arm64_vspec, switched by the "vspec" bit of /proc/ish/arm64_jit_fuse.
//
// Nothing here calls C, so v0 is free scratch (see simd.S's register
// discipline note).

#include "gadgets.h"

.text

// UMOV Wd/Xd, Vn.T[i] (and its MOV and FMOV-to-general aliases): zero-
// extending load of the element, stored as the whole 64-bit GPR.
.macro vspec_to_gpr name, ld
.gadget \name
    ldp x8, x10, [_ip]
    \ld w11, [_cpu, x10]
    str x11, [_cpu, x8]
    gret 2
.endm
vspec_to_gpr vspec_umov8, ldrb
vspec_to_gpr vspec_umov16, ldrh
vspec_to_gpr vspec_umov32, ldr

.gadget vspec_umov64
    ldp x8, x10, [_ip]
    ldr x11, [_cpu, x10]
    str x11, [_cpu, x8]
    gret 2

// INS Vd.T[i], Wn/Xn: the element from the GPR slot's low bytes, the other
// lanes kept. Also INS Vd.T[i], Vn.T[j] (element to element): the source is
// then a V slot byte offset. Same body either way.
.macro vspec_ins name, ld, st, reg
.gadget \name
    ldp x8, x10, [_ip]
    \ld \reg\()11, [_cpu, x10]
    \st \reg\()11, [_cpu, x8]
    gret 2
.endm
vspec_ins vspec_ins8, ldrb, strb, w
vspec_ins vspec_ins16, ldrh, strh, w
vspec_ins vspec_ins32, ldr, str, w
vspec_ins vspec_ins64, ldr, str, x

// Scalar writes to a V register that zero the rest of it: FMOV Sd, Wn /
// FMOV Dd, Xn (source a GPR slot) and DUP/MOV Bd/Hd/Sd/Dd, Vn.T[i] (source
// an element). The scalar load zeroes the rest of v0, stored whole.
.macro vspec_scalar name, reg
.gadget \name
    ldp x8, x10, [_ip]
    ldr \reg\()0, [_cpu, x10]
    str q0, [_cpu, x8]
    gret 2
.endm
vspec_scalar vspec_scalar8, b
vspec_scalar vspec_scalar16, h
vspec_scalar vspec_scalar32, s
vspec_scalar vspec_scalar64, d

// DUP Vd.T, Vn.T[i]: replicate one element. A 64-bit arrangement's ld1r
// clears v0's upper half, as the architecture clears Vd's.
.macro vspec_dup name, arr
.gadget \name
    ldp x8, x10, [_ip]
    add x10, _cpu, x10
    ld1r {v0.\arr}, [x10]
    str q0, [_cpu, x8]
    gret 2
.endm
vspec_dup vspec_dup8b, 8b
vspec_dup vspec_dup16b, 16b
vspec_dup vspec_dup4h, 4h
vspec_dup vspec_dup8h, 8h
vspec_dup vspec_dup2s, 2s
vspec_dup vspec_dup4s, 4s
vspec_dup vspec_dup2d, 2d

// MOV Vd.16B, Vn.16B (ORR with Rn == Rm) and its 8B form, which clears the
// upper half.
.gadget vspec_vmov128
    ldp x8, x10, [_ip]
    ldr q0, [_cpu, x10]
    str q0, [_cpu, x8]
    gret 2

.gadget vspec_vmov64
    ldp x8, x10, [_ip]
    ldr d0, [_cpu, x10]
    str q0, [_cpu, x8]
    gret 2

// UXTL/SXTL (USHLL/SSHLL #0), and their "2" forms, by arrangement:
// [gadget][d_off][n_off]. The generic vshll_gadget (simd.S) always follows the
// widen with a dup + ushl for the shift, which for #0 -- nearly every use --
// is a dependent pair doing nothing; a non-zero shift keeps that gadget.
.macro vspec_xtl name, xtl, srcarr, dstarr
.gadget \name
    ldp x8, x10, [_ip]
    ldr q30, [_cpu, x10]
    \xtl v30\dstarr, v30\srcarr
    str q30, [_cpu, x8]
    gret 2
.endm
vspec_xtl vspec_sxtl_8b,  sxtl,  .8b,  .8h
vspec_xtl vspec_sxtl2_16b, sxtl2, .16b, .8h
vspec_xtl vspec_sxtl_4h,  sxtl,  .4h,  .4s
vspec_xtl vspec_sxtl2_8h,  sxtl2, .8h,  .4s
vspec_xtl vspec_sxtl_2s,  sxtl,  .2s,  .2d
vspec_xtl vspec_sxtl2_4s,  sxtl2, .4s,  .2d
vspec_xtl vspec_uxtl_8b,  uxtl,  .8b,  .8h
vspec_xtl vspec_uxtl2_16b, uxtl2, .16b, .8h
vspec_xtl vspec_uxtl_4h,  uxtl,  .4h,  .4s
vspec_xtl vspec_uxtl2_8h,  uxtl2, .8h,  .4s
vspec_xtl vspec_uxtl_2s,  uxtl,  .2s,  .2d
vspec_xtl vspec_uxtl2_4s,  uxtl2, .4s,  .2d

// SSHR/USHR/SHL by arrangement: [gadget][d_off][n_off][amount]. The generic
// vshift_gadget's body (dup the signed amount, register-form shift: negative
// shifts right) fed offsets instead of a packed word. The accumulating and
// rounding forms keep their generic gadgets.
.macro vspec_shift name, shiftop, arr, dupw
.gadget \name
    ldp x8, x10, [_ip]
    ldr x9, [_ip, #16]
    ldr q30, [_cpu, x10]
    dup v29\arr, \dupw
    \shiftop v30\arr, v30\arr, v29\arr
    str q30, [_cpu, x8]
    gret 3
.endm
.irp arr, 8b,16b,4h,8h,2s,4s
    vspec_shift vspec_sshl_\arr, sshl, .\arr, w9
    vspec_shift vspec_ushl_\arr, ushl, .\arr, w9
.endr
vspec_shift vspec_sshl_2d, sshl, .2d, x9
vspec_shift vspec_ushl_2d, ushl, .2d, x9

// EXT by a table lookup: [gadget][d_off][n_off][m_off][index_vector]. The
// byte immediate cannot be a runtime operand of the host EXT, so gen.c hands
// over a pointer to a 16-byte TBL index for it (arm64_ext_index): bytes
// imm..imm+15 of {Vn, Vm}, and for the 8-byte form bytes of the two low
// halves with the upper eight lanes out of range (zero).
.gadget vspec_ext
    ldp x8, x9, [_ip]
    ldp x10, x11, [_ip, #16]
    ldr q1, [_cpu, x9]
    ldr q2, [_cpu, x10]
    ldr q3, [x11]
    tbl v0.16b, {v1.16b, v2.16b}, v3.16b
    str q0, [_cpu, x8]
    gret 4
