// AArch64 guest integer gadgets fed precomputed cpu_state offsets, the
// scalar side of simd_spec.S: gen.c resolves each register (SP included) to
// its byte offset in cpu_state, so the gadget is the operation and nothing
// else. Switched by the "ospec" bit of /proc/ish/arm64_jit_fuse.
//
// Each operand is its own stream word, fetched with ldp/ldr: packing them
// into one word and unpacking with ubfx put the shifts on the critical path
// and measured 29% slower per instruction on an A10X (26% on an M4) than
// the plain loads, which the core issues in parallel.
//
// Code streams: immediate forms [d_off][n_off][imm]; register forms
// [d_off][n_off][m_off] (+[shift] for the _lsl ones); CMP/CMN drop d_off.
// A 32-bit form stores the whole 64-bit slot: the W-register op zeroes the
// upper half.

#include "gadgets.h"

.text

.macro ospec_alu_imm name, op, r, flags=0
.gadget \name
    ldp x9, x10, [_ip]
    ldr x11, [_ip, #16]
    ldr x12, [_cpu, x10]
    \op \r\()12, \r\()12, \r\()11
.if \flags
    store_flags
.endif
    str x12, [_cpu, x9]
    gret 3
.endm
ospec_alu_imm ospec_addi64, add, x
ospec_alu_imm ospec_addi32, add, w
ospec_alu_imm ospec_subi64, sub, x
ospec_alu_imm ospec_subi32, sub, w
ospec_alu_imm ospec_addsi64, adds, x, 1
ospec_alu_imm ospec_addsi32, adds, w, 1
ospec_alu_imm ospec_subsi64, subs, x, 1
ospec_alu_imm ospec_subsi32, subs, w, 1
// Logical (immediate): the decoded bitmask is the third word.
ospec_alu_imm ospec_andi64, and, x
ospec_alu_imm ospec_andi32, and, w
ospec_alu_imm ospec_orri64, orr, x
ospec_alu_imm ospec_orri32, orr, w
ospec_alu_imm ospec_eori64, eor, x
ospec_alu_imm ospec_eori32, eor, w
ospec_alu_imm ospec_andsi64, ands, x, 1
ospec_alu_imm ospec_andsi32, ands, w, 1

// CMP/CMN (immediate): flags only. [n_off][imm]
.macro ospec_cmp_imm name, op, r
.gadget \name
    ldp x10, x11, [_ip]
    ldr x12, [_cpu, x10]
    \op \r\()zr, \r\()12, \r\()11
    store_flags
    gret 2
.endm
ospec_cmp_imm ospec_cmpi64, subs, x
ospec_cmp_imm ospec_cmpi32, subs, w
ospec_cmp_imm ospec_cmni64, adds, x
ospec_cmp_imm ospec_cmni32, adds, w

.macro ospec_alu_reg name, op, r, flags=0
.gadget \name
    ldp x9, x10, [_ip]
    ldr x11, [_ip, #16]
    ldr x12, [_cpu, x10]
    ldr x13, [_cpu, x11]
    \op \r\()12, \r\()12, \r\()13
.if \flags
    store_flags
.endif
    str x12, [_cpu, x9]
    gret 3
.endm
ospec_alu_reg ospec_addr64, add, x
ospec_alu_reg ospec_addr32, add, w
ospec_alu_reg ospec_subr64, sub, x
ospec_alu_reg ospec_subr32, sub, w
ospec_alu_reg ospec_andr64, and, x
ospec_alu_reg ospec_andr32, and, w
ospec_alu_reg ospec_orrr64, orr, x
ospec_alu_reg ospec_orrr32, orr, w
ospec_alu_reg ospec_eorr64, eor, x
ospec_alu_reg ospec_eorr32, eor, w
ospec_alu_reg ospec_addsr64, adds, x, 1
ospec_alu_reg ospec_addsr32, adds, w, 1
ospec_alu_reg ospec_subsr64, subs, x, 1
ospec_alu_reg ospec_subsr32, subs, w, 1
ospec_alu_reg ospec_andsr64, ands, x, 1
ospec_alu_reg ospec_andsr32, ands, w, 1

// ADD/SUB with Rm shifted left by a constant (address arithmetic).
// [d_off][n_off][m_off][shift]
.macro ospec_alu_reg_lsl name, op, r
.gadget \name
    ldp x9, x10, [_ip]
    ldp x11, x14, [_ip, #16]
    ldr x12, [_cpu, x10]
    ldr x13, [_cpu, x11]
    lslv \r\()13, \r\()13, \r\()14
    \op \r\()12, \r\()12, \r\()13
    str x12, [_cpu, x9]
    gret 4
.endm
ospec_alu_reg_lsl ospec_addr_lsl64, add, x
ospec_alu_reg_lsl ospec_addr_lsl32, add, w
ospec_alu_reg_lsl ospec_subr_lsl64, sub, x
ospec_alu_reg_lsl ospec_subr_lsl32, sub, w

// CMP/CMN (register): [n_off][m_off]
.macro ospec_cmp_reg name, op, r
.gadget \name
    ldp x10, x11, [_ip]
    ldr x12, [_cpu, x10]
    ldr x13, [_cpu, x11]
    \op \r\()zr, \r\()12, \r\()13
    store_flags
    gret 2
.endm
ospec_cmp_reg ospec_cmpr64, subs, x
ospec_cmp_reg ospec_cmpr32, subs, w
ospec_cmp_reg ospec_cmnr64, adds, x
ospec_cmp_reg ospec_cmnr32, adds, w
