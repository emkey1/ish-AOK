// AArch64 guest Logical (immediate) gadget: AND/ORR/EOR/ANDS. Independently
// written, NOT adapted from OpenMinis' equivalent gadgets. The bitmask
// immediate itself (immN:imms:immr -> a 64-bit pattern, ARM's
// "DecodeBitMasks") is decoded once at JIT-compile time by
// gen_step_arm64 calling arm64_decode_bitmask_imm (emu/arch/arm64/
// decode.h, shared with the interpreter) and packed directly into the
// code stream as a plain 64-bit value -- so this gadget never needs to
// reimplement DecodeBitMasks in assembly at all, unlike a real hardware
// decoder. ANDS's NZCV semantics (N,Z from the result; C,V always
// cleared) fall out for free from using the host's own native `ands`
// instruction on this same-architecture host -- no special-casing
// needed, the same reason same-architecture dispatch is fast throughout
// this port.
//
// Generic code stream: [gadget][rd | rn<<8 | opc<<16 | sf<<24][imm (64-bit)]
//
// ---- Fast-path logical immediate gadgets (the specialization pass) -----
// The generic gadget above decodes rd/rn/opc/sf from packed params with
// a branch tree at RUNTIME — ~15-20 host instructions of pure decode
// before the single logical operation. These variants bake opc and sf
// at compile time: gen_step_arm64 picks the exact (op, width) body and
// only emits a fast form when rn != 31 (SP/XZR). The 64-bit decoded
// immediate fits the same 16-byte stream as fast_alu_imm (math.S):
// [gadget][rd | rn<<8][imm (64-bit)] — opc and sf are in the gadget body.
// ANDS variants include store_flags and handle the TST alias (rd=31
// discards the result). Non-ANDS variants with rd=31 (AND/ORR/EOR to SP)
// fall back to the generic gadget, which handles the SP store.

#include "gadgets.h"

.text

.gadget logical_imm
    ldr x8, [_ip]
    ldr x11, [_ip, #8]      // imm, already decoded at compile time
    add _ip, _ip, #16

    and x9, x8, #0x1f       // rd
    ubfx x10, x8, #8, #5    // rn
    ubfx x12, x8, #16, #2   // opc: 0=AND,1=ORR,2=EOR,3=ANDS
    ubfx x13, x8, #24, #1   // sf

    add x14, _cpu, #CPU_x0
    cmp x10, #31
    b.eq 1f
    ldr x15, [x14, x10, lsl #3]
    b 2f
1:  mov x15, #0             // XZR
2:
    cmp x12, #0
    b.eq logical_imm_and
    cmp x12, #1
    b.eq logical_imm_orr
    cmp x12, #2
    b.eq logical_imm_eor

    // ANDS
    cbz x13, logical_imm_ands32
    ands x16, x15, x11
    b logical_imm_store_flags
logical_imm_ands32:
    ands w16, w15, w11
logical_imm_store_flags:
    store_flags
    b logical_imm_store_result

logical_imm_and:
    cbz x13, logical_imm_and32
    and x16, x15, x11
    b logical_imm_store_result
logical_imm_and32:
    and w16, w15, w11
    b logical_imm_store_result

logical_imm_orr:
    cbz x13, logical_imm_orr32
    orr x16, x15, x11
    b logical_imm_store_result
logical_imm_orr32:
    orr w16, w15, w11
    b logical_imm_store_result

logical_imm_eor:
    cbz x13, logical_imm_eor32
    eor x16, x15, x11
    b logical_imm_store_result
logical_imm_eor32:
    eor w16, w15, w11

logical_imm_store_result:
    // Rd=31 means SP for the non-flag-setting forms here (AND/ORR/EOR,
    // opc in x12 != 3) -- same SP-capable destination as ADD/SUB
    // immediate. Unlike add_imm/sub_imm (math.S) this does NOT mask to 48
    // bits before storing: the architected result is exactly `src OP imm`
    // with no truncation, and clipping it here would silently corrupt any
    // caller whose immediate legitimately sets bits above 47 (a real gap
    // caught by tests/manual/arm64/arm64_regress.c's and-sp check, which
    // uses such a value deliberately). Only ANDS (opc==3, x12==3) can't
    // target SP: its Rd=31 form is the TST alias and genuinely discards
    // the result. Found via a real on-device crash: LuaJIT's
    // `and sp, x1, #mask` stack-realignment (arm64 GC64 coroutine/call-
    // return path) was silently a no-op here, leaving SP stale so the
    // next SP-relative load read garbage -- manifested as a NaN-boxed
    // double or a GC string's raw bytes ending up dereferenced as a
    // pointer several instructions later.
    cmp x9, #31
    b.ne 3f
    cmp x12, #3
    b.eq 4f
    str x16, [_cpu, #CPU_sp]
4:  gret
3:  str x16, [x14, x9, lsl #3]
    gret

// Fast-path non-flag-setting logical immediate: AND/ORR/EOR.
// Code stream: [gadget][rd | rn<<8][imm (64-bit)] — 16 bytes.
// rn != 31 enforced by gen.c; rd = 31 means SP store (same as generic).
.macro fast_logical_imm name, op, r16, r15, r11
.gadget \name
    ldr x8, [_ip]
    ldr x11, [_ip, #8]         // pre-decoded bitmask immediate
    add _ip, _ip, #16
    and x9, x8, #0x1f          // rd
    ubfx x10, x8, #8, #5       // rn
    add x14, _cpu, #CPU_x0
    ldr x15, [x14, x10, lsl #3]
    \op \r16, \r15, \r11
    cmp x9, #31
    b.eq 99f
    str x16, [x14, x9, lsl #3]
    gret
99: str x16, [_cpu, #CPU_sp]   // AND/ORR/EOR with rd=31 targets SP
    gret
.endm

// Fast-path ANDS immediate: flag-setting, handles TST alias (rd=31).
// Code stream: [gadget][rd | rn<<8][imm (64-bit)] — 16 bytes.
.macro fast_ands_imm name, ands_op, r16, r15, r11
.gadget \name
    ldr x8, [_ip]
    ldr x11, [_ip, #8]
    add _ip, _ip, #16
    and x9, x8, #0x1f          // rd
    ubfx x10, x8, #8, #5       // rn
    add x14, _cpu, #CPU_x0
    ldr x15, [x14, x10, lsl #3]
    \ands_op \r16, \r15, \r11
    store_flags
    cmp x9, #31
    b.eq 99f                    // TST: discard result
    str x16, [x14, x9, lsl #3]
99: gret
.endm

fast_logical_imm andi_fast64,  and, x16, x15, x11
fast_logical_imm andi_fast32,  and, w16, w15, w11
fast_logical_imm orri_fast64,  orr, x16, x15, x11
fast_logical_imm orri_fast32,  orr, w16, w15, w11
fast_logical_imm eori_fast64,  eor, x16, x15, x11
fast_logical_imm eori_fast32,  eor, w16, w15, w11
fast_ands_imm    andsi_fast64, ands, x16, x15, x11
fast_ands_imm    andsi_fast32, ands, w16, w15, w11

# vim: ft=gas
