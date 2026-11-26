// AArch64 guest data-processing gadgets. Adapted from OpenMinis/ish-arm64's
// asbestos/guest-arm64/gadgets-aarch64/math.S (GPLv3, see
// /CREDITS-aarch64.md). Mechanical renames from their source: _pc -> _ip
// (this file's gadgets.h names the code-stream pointer _ip, matching
// AOK-Workstation's i386-guest convention, not their _pc); gadget symbols get an
// arm64_ prefix via the .gadget macro (avoids collision with i386/amd64
// gadget names in the same final binary). Otherwise these gadget bodies
// are unchanged — the packed-parameter encoding in each comment matches
// exactly what emu/arm64_interp.c's arm64_execute() already decodes (see
// that file for the field-layout citations), so gen_step_arm64 (jit/gen.c)
// emits parameters in the same packed form these gadgets expect.

#include "gadgets.h"
#include "frag.h"

.text

// Load a guest register into _tmp (x0). Code stream: [gadget addr][reg_index]
.gadget load_reg
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x8, x8, #0x1f
    add x9, _cpu, #CPU_x0
    cmp x8, #31
    b.eq 1f
    ldr _tmp, [x9, x8, lsl #3]
    gret
1:  mov _tmp, #0            // XZR
    gret

// Store _tmp (x0) to a guest register. Code stream: [gadget addr][reg_index]
.gadget store_reg
    ldr x8, [_ip]
    and x8, x8, #0x1f
    add x9, _cpu, #CPU_x0
    cmp x8, #31
    b.eq 1f                 // XZR - discard
    str _tmp, [x9, x8, lsl #3]
1:  add _ip, _ip, #8
    gret

/*
 * Move wide immediate (MOVZ, MOVK, MOVN)
 * Code stream: [gadget addr][rd | hw<<8 | imm16<<16 | sf<<32]
 */

.gadget movz
    ldr x8, [_ip]
    add _ip, _ip, #8

    and x9, x8, #0x1f       // rd
    ubfx x10, x8, #8, #2    // hw
    ubfx x11, x8, #16, #16  // imm16
    ubfx x12, x8, #32, #1   // sf

    lsl x10, x10, #4        // hw * 16
    lsl x11, x11, x10       // imm16 << (hw*16)

    cbz x12, 1f
    b 2f
1:  and x11, x11, #0xffffffff
2:
    add x13, _cpu, #CPU_x0
    cmp x9, #31
    b.eq 3f
    str x11, [x13, x9, lsl #3]
3:  gret

.gadget movk
    ldr x8, [_ip]
    add _ip, _ip, #8

    and x9, x8, #0x1f       // rd
    ubfx x10, x8, #8, #2    // hw
    ubfx x11, x8, #16, #16  // imm16
    ubfx x12, x8, #32, #1   // sf

    add x13, _cpu, #CPU_x0
    cmp x9, #31
    b.eq 3f
    ldr x14, [x13, x9, lsl #3]

    cbz x12, 1f
    b 2f
1:  and x14, x14, #0xffffffff
2:
    lsl x10, x10, #4        // hw * 16
    mov x15, #0xffff
    lsl x15, x15, x10       // mask at position
    lsl x11, x11, x10       // value at position

    bic x14, x14, x15
    orr x14, x14, x11

    cbz x12, 4f
    b 5f
4:  and x14, x14, #0xffffffff
5:
    str x14, [x13, x9, lsl #3]
3:  gret

// ---- Fast-path move-wide (the specialization pass) ----------------------
// MOVZ/MOVN write a fully compile-time-known value, so the generic
// gadgets' runtime hw-shift/sf-mask/rd-decode branch tree (above) is pure
// overhead. gen_step_arm64 also FOLDS any immediately-following MOVKs to
// the same rd/sf into the constant (same next-instruction-lookahead
// precedent as compare+branch fusion, gen_arm64_peek_bcond), so gcc's
// standard 2-4 instruction big-immediate/address build becomes ONE
// dispatch. rd=31 keeps the generic path (the store is discarded there).
// Profiled on the iSH_benchmark bmm integer loop: the generic movz+movk
// pair materializing the loop bound was ~11% of total runtime.
// Code stream: [gadget][rd_slot][value]
f_gadget mov_const, f_mov_const, _, _, _, _, _

// MOVK not preceded by a fusable MOVZ/MOVN (old value is live at runtime):
// insert the compile-time-shifted 16 bits under a compile-time keep-mask.
// The sf=0 form's upper-32 clear is baked into the mask by gen_step_arm64.
// Code stream: [gadget][rd_slot][shifted value][keep mask]
.gadget movk_fast
    ldp x8, x9, [_ip]
    ldr x10, [_ip, #16]
    add _ip, _ip, #24
    ldr x11, [_cpu, x8]
    and x11, x11, x10
    orr x11, x11, x9
    str x11, [_cpu, x8]
    gret

.gadget movn
    ldr x8, [_ip]
    add _ip, _ip, #8

    and x9, x8, #0x1f       // rd
    ubfx x10, x8, #8, #2    // hw
    ubfx x11, x8, #16, #16  // imm16
    ubfx x12, x8, #32, #1   // sf

    lsl x10, x10, #4
    lsl x11, x11, x10
    mvn x11, x11            // NOT

    cbz x12, 1f
    b 2f
1:  and x11, x11, #0xffffffff
2:
    add x13, _cpu, #CPU_x0
    cmp x9, #31
    b.eq 3f
    str x11, [x13, x9, lsl #3]
3:  gret

/*
 * ADR/ADRP - PC-relative address
 * Code stream: [gadget addr][rd(0:4) | target(8:55)]
 * Target is a 48-bit guest address packed in bits 8-55 (already computed
 * by gen_step_arm64 at compile time, since the base PC is compile-time
 * known -- see arm64_execute()'s ADR/ADRP handler in emu/arm64_interp.c
 * for the equivalent runtime computation this gadget's caller does ahead
 * of time instead).
 */
.gadget adr
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f          // rd
    ubfx x10, x8, #8, #48      // target (48-bit)
    add x11, _cpu, #CPU_x0
    cmp x9, #31
    b.eq 1f
    str x10, [x11, x9, lsl #3]
    gret
1:  gret

/*
 * ADD/SUB immediate. Adapted from OpenMinis' math.S. Uses the host's own
 * adds/subs instructions to compute NZCV directly (adds x16,x15,x11 sets
 * the real ARM64 flags register as a side effect) then store_flags packs
 * it into cpu->arm64_nzcv -- the whole reason same-architecture dispatch
 * is fast: the host CPU does the semantics natively, nothing to compute
 * in software the way emu/arm64_interp.c's arm64_set_flags_add/sub do.
 * Code stream: [gadget addr][rd | rn<<8 | imm12<<16 | sf<<28 | S<<29 | sh<<30]
 */
.gadget add_imm
    ldr x8, [_ip]
    add _ip, _ip, #8

    and x9, x8, #0x1f       // rd
    ubfx x10, x8, #8, #5    // rn
    ubfx x11, x8, #16, #12  // imm12
    ubfx x12, x8, #28, #1   // sf (0=32-bit, 1=64-bit)
    ubfx x13, x8, #29, #1   // S (set flags)
    ubfx x17, x8, #30, #1   // sh (0=no shift, 1=LSL #12)

    cbz x17, 0f
    lsl x11, x11, #12
0:
    add x14, _cpu, #CPU_x0
    cmp x10, #31
    b.eq 1f
    ldr x15, [x14, x10, lsl #3]
    b 2f
1:  ldr x15, [_cpu, #CPU_sp]
2:
    cbz x13, add_imm_no_flags

    cbz x12, add_imm_adds_32
    adds x16, x15, x11
    b add_imm_store_flags
add_imm_adds_32:
    adds w16, w15, w11
add_imm_store_flags:
    store_flags
    b add_imm_store_result

add_imm_no_flags:
    add x16, x15, x11
    cbz x12, 3f
    b add_imm_store_result
3:  and x16, x16, #0xffffffff

add_imm_store_result:
    cmp x9, #31
    b.eq 5f
    str x16, [x14, x9, lsl #3]
    gret
5:  cbz x13, 7f
    gret
7:  and x16, x16, #0xffffffffffff
    str x16, [_cpu, #CPU_sp]
    gret

.gadget sub_imm
    ldr x8, [_ip]
    add _ip, _ip, #8

    and x9, x8, #0x1f       // rd
    ubfx x10, x8, #8, #5    // rn
    ubfx x11, x8, #16, #12  // imm12
    ubfx x12, x8, #28, #1   // sf
    ubfx x13, x8, #29, #1   // S
    ubfx x17, x8, #30, #1   // sh

    cbz x17, 0f
    lsl x11, x11, #12
0:
    add x14, _cpu, #CPU_x0
    cmp x10, #31
    b.eq 1f
    ldr x15, [x14, x10, lsl #3]
    b 2f
1:  ldr x15, [_cpu, #CPU_sp]
2:
    cbz x13, sub_imm_no_flags

    cbz x12, sub_imm_subs_32
    subs x16, x15, x11
    b sub_imm_store_flags
sub_imm_subs_32:
    subs w16, w15, w11
sub_imm_store_flags:
    store_flags
    b sub_imm_store_result

sub_imm_no_flags:
    sub x16, x15, x11
    cbz x12, 3f
    b sub_imm_store_result
3:  and x16, x16, #0xffffffff

sub_imm_store_result:
    cmp x9, #31
    b.eq 5f
    str x16, [x14, x9, lsl #3]
    gret
5:  cbz x13, 7f
    gret
7:  and x16, x16, #0xffffffffffff
    str x16, [_cpu, #CPU_sp]
    gret

// ---- Fast-path ALU gadgets (the specialization pass) ---------------------
// The generic gadgets above decode sf/S/op/shift from packed params with
// a branch tree at RUNTIME — ~20-25 host instructions per guest ALU op,
// which the cross-arch benchmark showed erases the same-architecture
// advantage entirely (arm64 measured ~1x the x86 guests on tight compute
// loops instead of the expected multiples). These variants bake
// everything except the register INDICES at compile time: gen_step_arm64
// picks the exact (op, width, flags) body, pre-shifts the immediate, and
// only emits a fast form when rd/rn are plain registers (31/SP forms and
// shifted operands fall back to the generic gadgets). Body: ~8 host
// instructions + dispatch.
//
// The "immediate" is loaded from the code stream into a register and
// applied with the REGISTER-form add/sub — bit-identical results, and it
// lets one body serve any immediate value.
// Code stream: [gadget][rd | rn<<8][imm (pre-shifted)]
.macro fast_alu_imm name, op, r16, r15, r11
.gadget \name
    ldr x8, [_ip]
    ldr x11, [_ip, #8]
    add _ip, _ip, #16
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    add x14, _cpu, #CPU_x0
    ldr x15, [x14, x10, lsl #3]
    \op \r16, \r15, \r11
    str x16, [x14, x9, lsl #3]
    gret
.endm

.macro fast_alu_imm_flags name, op, r16, r15, r11
.gadget \name
    ldr x8, [_ip]
    ldr x11, [_ip, #8]
    add _ip, _ip, #16
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    add x14, _cpu, #CPU_x0
    ldr x15, [x14, x10, lsl #3]
    \op \r16, \r15, \r11
    store_flags               // clobbers x17 only; x14/x9/x16 stay live
    str x16, [x14, x9, lsl #3]
    gret
.endm

fast_alu_imm       addi_fast64,  add,  x16, x15, x11
fast_alu_imm       addi_fast32,  add,  w16, w15, w11
fast_alu_imm       subi_fast64,  sub,  x16, x15, x11
fast_alu_imm       subi_fast32,  sub,  w16, w15, w11
fast_alu_imm_flags addsi_fast64, adds, x16, x15, x11
fast_alu_imm_flags addsi_fast32, adds, w16, w15, w11
fast_alu_imm_flags subsi_fast64, subs, x16, x15, x11
fast_alu_imm_flags subsi_fast32, subs, w16, w15, w11

// Flag-setting with Rd=31 (CMP/CMN aliases — extremely hot): no store at
// all, just compute flags. Code stream: [gadget][rn][imm]
.macro fast_cmp_imm name, op, r15, r11
.gadget \name
    ldr x10, [_ip]
    ldr x11, [_ip, #8]
    add _ip, _ip, #16
    add x14, _cpu, #CPU_x0
    ldr x15, [x14, x10, lsl #3]
    \op xzr, \r15, \r11
    store_flags
    gret
.endm

fast_cmp_imm cmpi_fast64, subs, x15, x11
fast_cmp_imm cmni_fast64, adds, x15, x11
.gadget cmpi_fast32
    ldr x10, [_ip]
    ldr x11, [_ip, #8]
    add _ip, _ip, #16
    add x14, _cpu, #CPU_x0
    ldr x15, [x14, x10, lsl #3]
    subs wzr, w15, w11
    store_flags
    gret
.gadget cmni_fast32
    ldr x10, [_ip]
    ldr x11, [_ip, #8]
    add _ip, _ip, #16
    add x14, _cpu, #CPU_x0
    ldr x15, [x14, x10, lsl #3]
    adds wzr, w15, w11
    store_flags
    gret

// ---- Fast-path register ALU (shift amount 0, plain registers) -----------
// Covers plain add/sub/adds/subs Xd,Xn,Xm and the whole logical family
// including `mov xd, xm` (ORR with Rn=ZR — gen substitutes a zeroed
// slot-read by passing rn=rm... no: rn=31 falls back to generic; instead
// the MOV alias gets its own dedicated gadget below).
// Code stream: [gadget][rd | rn<<8 | rm<<16]
.macro fast_alu_reg name, op, r16, r15, r13
.gadget \name
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    ubfx x12, x8, #16, #5
    add x14, _cpu, #CPU_x0
    ldr x15, [x14, x10, lsl #3]
    ldr x13, [x14, x12, lsl #3]
    \op \r16, \r15, \r13
    str x16, [x14, x9, lsl #3]
    gret
.endm

.macro fast_alu_reg_flags name, op, r16, r15, r13
.gadget \name
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    ubfx x12, x8, #16, #5
    add x14, _cpu, #CPU_x0
    ldr x15, [x14, x10, lsl #3]
    ldr x13, [x14, x12, lsl #3]
    \op \r16, \r15, \r13
    store_flags
    str x16, [x14, x9, lsl #3]
    gret
.endm

fast_alu_reg       addr_fast64,  add,  x16, x15, x13
fast_alu_reg       addr_fast32,  add,  w16, w15, w13
fast_alu_reg       subr_fast64,  sub,  x16, x15, x13
fast_alu_reg       subr_fast32,  sub,  w16, w15, w13
fast_alu_reg_flags addsr_fast64, adds, x16, x15, x13
fast_alu_reg_flags addsr_fast32, adds, w16, w15, w13
fast_alu_reg_flags subsr_fast64, subs, x16, x15, x13
fast_alu_reg_flags subsr_fast32, subs, w16, w15, w13
fast_alu_reg       andr_fast64,  and,  x16, x15, x13
fast_alu_reg       andr_fast32,  and,  w16, w15, w13
fast_alu_reg       orrr_fast64,  orr,  x16, x15, x13
fast_alu_reg       orrr_fast32,  orr,  w16, w15, w13
fast_alu_reg       eorr_fast64,  eor,  x16, x15, x13
fast_alu_reg       eorr_fast32,  eor,  w16, w15, w13
fast_alu_reg_flags andsr_fast64, ands, x16, x15, x13
fast_alu_reg_flags andsr_fast32, ands, w16, w15, w13

// MOV (register) alias — ORR Xd, XZR, Xm, shift 0. The single hottest
// register-form instruction in compiled code.
// Code stream: [gadget][rd | rm<<8]
.gadget movr_fast64
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    add x14, _cpu, #CPU_x0
    ldr x15, [x14, x10, lsl #3]
    str x15, [x14, x9, lsl #3]
    gret
.gadget movr_fast32
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    add x14, _cpu, #CPU_x0
    ldr x15, [x14, x10, lsl #3]   // w-loads don't allow lsl #3 addressing
    mov w15, w15                  // 32-bit form: zero the upper half
    str x15, [x14, x9, lsl #3]
    gret

// CMP/CMN (register): SUBS/ADDS with Rd=31. Code stream: [gadget][rn | rm<<8]
.macro fast_cmp_reg name, op, r15, r13
.gadget \name
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x10, x8, #0x1f
    ubfx x12, x8, #8, #5
    add x14, _cpu, #CPU_x0
    ldr x15, [x14, x10, lsl #3]
    ldr x13, [x14, x12, lsl #3]
    \op xzr, \r15, \r13
    store_flags
    gret
.endm

fast_cmp_reg cmpr_fast64, subs, x15, x13
fast_cmp_reg cmnr_fast64, adds, x15, x13
.gadget cmpr_fast32
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x10, x8, #0x1f
    ubfx x12, x8, #8, #5
    add x14, _cpu, #CPU_x0
    ldr x15, [x14, x10, lsl #3]
    ldr x13, [x14, x12, lsl #3]
    subs wzr, w15, w13
    store_flags
    gret
.gadget cmnr_fast32
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x10, x8, #0x1f
    ubfx x12, x8, #8, #5
    add x14, _cpu, #CPU_x0
    ldr x15, [x14, x10, lsl #3]
    ldr x13, [x14, x12, lsl #3]
    adds wzr, w15, w13
    store_flags
    gret
