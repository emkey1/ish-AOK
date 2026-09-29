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
