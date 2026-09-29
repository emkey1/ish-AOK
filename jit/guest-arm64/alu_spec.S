// AArch64 guest integer gadgets fed precomputed cpu_state offsets, the
// scalar side of simd_spec.S: gen.c resolves each register (SP included) to
// its byte offset in cpu_state, so the gadget is the operation and nothing
// else. Switched by the "ospec" bit of /proc/ish/arm64_jit_fuse.
//
// Code stream: [gadget][d_off | n_off << 16 | imm << 32]. A 32-bit form
// stores the whole 64-bit slot: the W-register op zeroes the upper half.

#include "gadgets.h"

.text

.macro ospec_alu_imm name, op, r
.gadget \name
    ldr x8, [_ip]
    ubfx x10, x8, #16, #16
    lsr x11, x8, #32
    ldr x12, [_cpu, x10]
    \op \r\()12, \r\()12, \r\()11
    and x9, x8, #0xffff
    str x12, [_cpu, x9]
    gret 1
.endm
ospec_alu_imm ospec_addi64, add, x
ospec_alu_imm ospec_addi32, add, w
ospec_alu_imm ospec_subi64, sub, x
ospec_alu_imm ospec_subi32, sub, w

.macro ospec_alu_imm_flags name, op, r
.gadget \name
    ldr x8, [_ip]
    ubfx x10, x8, #16, #16
    lsr x11, x8, #32
    ldr x12, [_cpu, x10]
    \op \r\()12, \r\()12, \r\()11
    store_flags
    and x9, x8, #0xffff
    str x12, [_cpu, x9]
    gret 1
.endm
ospec_alu_imm_flags ospec_addsi64, adds, x
ospec_alu_imm_flags ospec_addsi32, adds, w
ospec_alu_imm_flags ospec_subsi64, subs, x
ospec_alu_imm_flags ospec_subsi32, subs, w

// CMP/CMN (immediate): flags only. [gadget][n_off << 16 | imm << 32]
.macro ospec_cmp_imm name, op, r
.gadget \name
    ldr x8, [_ip]
    ubfx x10, x8, #16, #16
    lsr x11, x8, #32
    ldr x12, [_cpu, x10]
    \op \r\()zr, \r\()12, \r\()11
    store_flags
    gret 1
.endm
ospec_cmp_imm ospec_cmpi64, subs, x
ospec_cmp_imm ospec_cmpi32, subs, w
ospec_cmp_imm ospec_cmni64, adds, x
ospec_cmp_imm ospec_cmni32, adds, w

// Register forms: [gadget][d_off | n_off << 16 | m_off << 32 | lsl << 48]
// (the shift is used only by the _lsl gadgets).
.macro ospec_alu_reg name, op, r, flags=0
.gadget \name
    ldr x8, [_ip]
    ubfx x10, x8, #16, #16
    ubfx x11, x8, #32, #16
    ldr x12, [_cpu, x10]
    ldr x13, [_cpu, x11]
    \op \r\()12, \r\()12, \r\()13
.if \flags
    store_flags
.endif
    and x9, x8, #0xffff
    str x12, [_cpu, x9]
    gret 1
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
.macro ospec_alu_reg_lsl name, op, r
.gadget \name
    ldr x8, [_ip]
    ubfx x10, x8, #16, #16
    ubfx x11, x8, #32, #16
    lsr x14, x8, #48
    ldr x12, [_cpu, x10]
    ldr x13, [_cpu, x11]
    lslv \r\()13, \r\()13, \r\()14
    \op \r\()12, \r\()12, \r\()13
    and x9, x8, #0xffff
    str x12, [_cpu, x9]
    gret 1
.endm
ospec_alu_reg_lsl ospec_addr_lsl64, add, x
ospec_alu_reg_lsl ospec_addr_lsl32, add, w
ospec_alu_reg_lsl ospec_subr_lsl64, sub, x
ospec_alu_reg_lsl ospec_subr_lsl32, sub, w

// CMP/CMN (register): [gadget][n_off << 16 | m_off << 32]
.macro ospec_cmp_reg name, op, r
.gadget \name
    ldr x8, [_ip]
    ubfx x10, x8, #16, #16
    ubfx x11, x8, #32, #16
    ldr x12, [_cpu, x10]
    ldr x13, [_cpu, x11]
    \op \r\()zr, \r\()12, \r\()13
    store_flags
    gret 1
.endm
ospec_cmp_reg ospec_cmpr64, subs, x
ospec_cmp_reg ospec_cmpr32, subs, w
ospec_cmp_reg ospec_cmnr64, adds, x
ospec_cmp_reg ospec_cmnr32, adds, w

// Logical (immediate): the decoded bitmask is a second word.
// [gadget][d_off | n_off << 16][imm]
.macro ospec_logical_imm name, op, r, flags=0
.gadget \name
    ldp x8, x11, [_ip]
    ubfx x10, x8, #16, #16
    ldr x12, [_cpu, x10]
    \op \r\()12, \r\()12, \r\()11
.if \flags
    store_flags
.endif
    and x9, x8, #0xffff
    str x12, [_cpu, x9]
    gret 2
.endm
ospec_logical_imm ospec_andi64, and, x
ospec_logical_imm ospec_andi32, and, w
ospec_logical_imm ospec_orri64, orr, x
ospec_logical_imm ospec_orri32, orr, w
ospec_logical_imm ospec_eori64, eor, x
ospec_logical_imm ospec_eori32, eor, w
ospec_logical_imm ospec_andsi64, ands, x, 1
ospec_logical_imm ospec_andsi32, ands, w, 1
