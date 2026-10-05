// The V extension (RVV 1.0 with Zvbb, Zvfhmin, Zvkt; VLEN 128), all in
// gadgets: gen.c (gen_riscv64_vector) picks one per instruction, typed for
// the SEW when it depends on one. vop is what is left: an encoding that is
// not a vector instruction, an illegal instruction. [gadget][insn][pc]

#include "gadgets.h"

.text

.gadget vop
    ldr x9, [_ip, #8]
    str x9, [_cpu, #CPU_riscv64_pc]
    mov w0, #INT_UNDEFINED
    b jit_exit

// an illegal instruction, at the pc in stream word \pc_off
.macro vtrap pc_off
    ldr x9, [_ip, #\pc_off]
    str x9, [_cpu, #CPU_riscv64_pc]
    mov w0, #INT_UNDEFINED
    b jit_exit
.endm


// vsetvli / vsetivli with a legal vtype, VLMAX worked out at translation:
// [gadget][vtype][vlmax][rd_off][avl: rs1_off or the immediate]. vl =
// min(AVL, VLMAX); (vsetvli x0, x0 keeps vl and stays in C.)
.macro vsetvl_body
    ldr x9, [_ip, #8]          // vlmax
    cmp x13, x9
    csel x13, x13, x9, lo
    ldr x8, [_ip]              // vtype
    str x8, [_cpu, #CPU_riscv64_vtype]
    str x13, [_cpu, #CPU_riscv64_vl]
    str xzr, [_cpu, #CPU_riscv64_vstart]
    ldr x10, [_ip, #16]        // rd_off (x0: the zero sink)
    str x13, [_cpu, x10]
    gret 4
.endm
.gadget vsetvl_reg
    ldr x11, [_ip, #24]
    ldr x13, [_cpu, x11]
    vsetvl_body
.gadget vsetvl_imm
    ldr x13, [_ip, #24]
    vsetvl_body

// Unmasked unit-stride vle<eew>.v / vle<eew>ff.v / vse<eew>.v within one
// page, from vstart 0: one copy between guest memory and the register file.
// [gadget][eew bytes][vd's offset in cpu_state][rs1_off][insn][pc]. Anything
// else -- vill, vstart, a page crossing, a TLB miss, more than the register
// file holds -- runs in C (where an illegal one traps).
.macro vmem_fast_head
    ldr x10, [_cpu, #CPU_riscv64_vtype]
    tbnz x10, #63, 9f
    ldr x11, [_cpu, #CPU_riscv64_vstart]
    cbnz x11, 9f
    ldr x12, [_cpu, #CPU_riscv64_vl]
    cbz x12, 8f
    ldr x13, [_ip]
    mul x12, x12, x13          // bytes
    cmp x12, #128
    b.hi 9f
    ldr x14, [_ip, #8]         // vd's offset in cpu_state
    add x15, x14, x12
    mov x16, #(CPU_riscv64_v + 32 * 16)
    cmp x15, x16
    b.hi 9f
    ldr x9, [_ip, #16]
    ldr x7, [_cpu, x9]         // address
    and x9, x7, #0xfff
    add x9, x9, x12
    cmp x9, #0x1000
    b.hi 9f
.endm
.macro vmem_tlb field
    vmem_tlb_miss \field, 9f
.endm
.macro vmem_tlb_miss field, miss
    and x9, x7, #0xfffffffffffff000
    ubfx x10, x7, #12, #10
    eor x10, x10, x7, lsr #22
    and x10, x10, #(1 << TLB_BITS) - 1
    mov w11, #TLB_ENTRY_SIZE
    madd x10, x10, x11, _tlb
    ldr x11, [x10, #\field]
    cmp x9, x11
    b.ne \miss
    ldr x11, [x10, #TLB_ENTRY_data_minus_addr]
    add x7, x11, x7
.endm
// copy x12 bytes from \src to \dst (both advanced), 16 at a time, then bytes
.macro vcopy dst, src
1:  cmp x12, #16
    b.lo 2f
    ldr q0, [\src], #16
    str q0, [\dst], #16
    sub x12, x12, #16
    b 1b
2:  cbz x12, 3f
    ldrb w9, [\src], #1
    strb w9, [\dst], #1
    sub x12, x12, #1
    b 2b
3:
.endm
.gadget vle_fast
    vmem_fast_head
    vmem_tlb TLB_ENTRY_page
    add x15, _cpu, x14
    vcopy x15, x7
8:  gret 5
9:  ldr w8, [_ip, #24]             // anything else: the general path
    mov x16, #32
    mov x17, #5
    b vmem_any
.gadget vse_fast
    vmem_fast_head
    ldr x9, [_tlb, #(-TLB_entries+TLB_mmu)]
    ldr x8, [_tlb, #(-TLB_entries+TLB_mem_changes)]
    ldr x11, [x9, #MMU_changes]
    cmp x8, x11
    b.ne 9f
    ldrb w9, [x9, #MMU_requires_write_revalidate]
    cbnz w9, 9f
    and x9, x7, #0xfffffffffffff000
    str x9, [_tlb, #(-TLB_entries+TLB_dirty_page)]
    vmem_tlb TLB_ENTRY_page_if_writable
    add x15, _cpu, x14
    vcopy x7, x15
8:  gret 5
9:  ldr w8, [_ip, #24]
    mov x16, #32
    mov x17, #5
    b vmem_any

// csrr rd, vl / vtype: [gadget][field's offset in cpu_state][rd_off]
.gadget vcsr_read
    ldr x8, [_ip]
    ldr x9, [_cpu, x8]
    ldr x10, [_ip, #8]
    str x9, [_cpu, x10]
    gret 2

.balign 16
vmask_weights:
    .byte 1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128

// vfirst.m rd, vs2[, v0.t]: the index of the first set (and active) bit
// below vl, or -1. [gadget][rd_off][vs2_off][insn][pc]
.macro vfirst_g name, masked
.gadget \name
    ldr x10, [_cpu, #CPU_riscv64_vtype]
    tbnz x10, #63, 9f
    ldr x11, [_cpu, #CPU_riscv64_vstart]
    cbnz x11, 9f
    ldr x12, [_cpu, #CPU_riscv64_vl]
    ldp x8, x9, [_ip]
    add x9, _cpu, x9
    ldp x3, x4, [x9]
    vlmask x5, x6, x12, x7
    and x3, x3, x5
    and x4, x4, x6
.if \masked
    ldr x5, [_cpu, #CPU_riscv64_v]
    ldr x6, [_cpu, #(CPU_riscv64_v + 8)]
    and x3, x3, x5
    and x4, x4, x6
.endif
    mov x10, #-1
    cbz x3, 1f
    rbit x3, x3
    clz x10, x3
    b 2f
1:  cbz x4, 2f
    rbit x4, x4
    clz x10, x4
    add x10, x10, #64
2:  str x10, [_cpu, x8]
    gret 4
9:  vtrap 24
.endm

.balign 16
vbyte_index:
    .byte 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15

// ---------------------------------------------------------------------------
// Typed gadgets: gen.c knows the vtype in force (set by a vsetvli earlier in
// the block, or guessed from the one in force when the block is compiled)
// and picks the gadget for its SEW. Each gadget's first stream word is that
// vtype; a different one at run time, or a nonzero vstart, runs the
// instruction in C instead. Tails are left undisturbed, as
// the C core leaves them. Register groups are whole registers, so a byte
// count of vl << log2(SEW) bytes from vd/vs2/vs1 stays inside each group.

// the expected vtype (stream word 0) is in force and vstart is 0
.macro vt_check
    ldr x10, [_ip]
    ldr x11, [_cpu, #CPU_riscv64_vtype]
    cmp x10, x11
    b.ne 9f
    ldr x11, [_cpu, #CPU_riscv64_vstart]
    cbnz x11, 9f
.endm

// A different vtype is in force (the block was compiled under another, or
// the guess was wrong): run the body for its SEW (\row: their offsets from
// it) once vill, vstart and the alignment of the register groups (stream
// words 1-3, as \vd/\vs2/\vs1 say) to its LMUL are checked. vill or a
// misaligned group is an illegal instruction (trapping at the pc in word
// \pc_off). So is a nonzero vstart (5f: the spec lets an arithmetic
// instruction trap on one; only a faulting load or store leaves one).
.macro vreg_aligned r, m=x13
    sub \r, \r, #CPU_riscv64_v
    lsr \r, \r, #4
    tst \r, \m
    b.ne 4f
.endm
// \vd/\vs2 = 2: a group of twice LMUL (a widening op's), which LMUL 8 cannot
// have; such ops have no SEW 64 either (their row sends it to vtrap_ill)
.macro vretype row, vd, vs2, vs1, pc_off
    ldr x10, [_cpu, #CPU_riscv64_vtype]
    tbnz x10, #63, 4f
    ldr x11, [_cpu, #CPU_riscv64_vstart]
    cbnz x11, 5f
    ubfx x11, x10, #3, #3      // vsew
    cmp x11, #3
    b.hi 5f
    and x12, x10, #7           // vlmul: a group of 1 << vlmul, or 1 if fractional
    mov x13, #1
    lsl x13, x13, x12
    cmp x12, #4
    csinc x13, x13, xzr, lo
    sub x13, x13, #1
.if \vd == 2 || \vs2 == 2
    cmp x12, #3
    b.eq 4f
    mov x16, #2                // twice LMUL: 2 << vlmul, or 1 if fractional
    lsl x16, x16, x12
    sub x16, x16, #1
    cmp x12, #4
    csel x16, x16, xzr, lo
.endif
.if \vd == 2
    ldr x9, [_ip, #8]
    vreg_aligned x9, x16
.elseif \vd
    ldr x9, [_ip, #8]
    vreg_aligned x9
.endif
.if \vs2 == 2
    ldr x9, [_ip, #16]
    vreg_aligned x9, x16
.elseif \vs2
    ldr x9, [_ip, #16]
    vreg_aligned x9
.endif
.if \vs1
    ldr x9, [_ip, #24]
    vreg_aligned x9
.endif
    adr x9, \row
    ldrsw x10, [x9, x11, lsl #2]
    add x9, x9, x10
    br x9
4:  ldr x9, [_ip, #\pc_off]      // vill, or a group not aligned to LMUL: reserved
    str x9, [_cpu, #CPU_riscv64_pc]
    mov w0, #INT_UNDEFINED
    b jit_exit
.endm
// the bodies \name\()_e1 ... _e8 for vretype, as offsets from \name
.macro vrow name
\name :
    .word \name\()_e1 - \name, \name\()_e2 - \name, \name\()_e4 - \name, \name\()_e8 - \name
.endm
// the same with no SEW 64 body: an illegal instruction there
.macro vrow3 name
\name :
    .word \name\()_e1 - \name, \name\()_e2 - \name, \name\()_e4 - \name, vtrap_ill_40 - \name
.endm
// an illegal instruction at the pc in a 6-word stream's last word
vtrap_ill_40:
    ldr x9, [_ip, #40]
    str x9, [_cpu, #CPU_riscv64_pc]
    mov w0, #INT_UNDEFINED
    b jit_exit

// \m0:\m1 = the low \vl bits set (vl 0..128)
.macro vlmask m0, m1, vl, t
    mov \t, #1
    lsl \m0, \t, \vl
    sub \m0, \m0, #1
    cmp \vl, #64
    csinv \m0, \m0, xzr, lo
    sub \t, \vl, #64
    mov \m1, #1
    lsl \m1, \m1, \t
    sub \m1, \m1, #1
    cmp \vl, #128
    csinv \m1, \m1, xzr, lo
    cmp \vl, #64
    csel \m1, \m1, xzr, hi
.endm

// [\p] = ([\p] & ~m) | (\a:\b & m) for the 128 bits at \p
.macro vmask_store p, a, b, m0, m1
    ldp x7, x8, [\p]
    bic x7, x7, \m0
    and \a, \a, \m0
    orr x7, x7, \a
    bic x8, x8, \m1
    and \b, \b, \m1
    orr x8, x8, \b
    stp x7, x8, [\p]
.endm

// element-wise ops on v0 (vs2's elements) and v1 (vs1's, or the scalar),
// result in v0; scratch v2, v3, v5, v7, v19 and x6-x9
.macro vo_add T, r, bits
    add v0.\T, v0.\T, v1.\T
.endm
.macro vo_sub T, r, bits
    sub v0.\T, v0.\T, v1.\T
.endm
.macro vo_rsub T, r, bits
    sub v0.\T, v1.\T, v0.\T
.endm
.macro vo_and T, r, bits
    and v0.16b, v0.16b, v1.16b
.endm
.macro vo_or T, r, bits
    orr v0.16b, v0.16b, v1.16b
.endm
.macro vo_xor T, r, bits
    eor v0.16b, v0.16b, v1.16b
.endm
.macro vo_andn T, r, bits
    bic v0.16b, v0.16b, v1.16b
.endm
.macro vo_mv T, r, bits
    mov v0.16b, v1.16b
.endm
.macro vo_minu T, r, bits
.ifc \T, 2d
    cmhi v2.2d, v0.2d, v1.2d
    bit v0.16b, v1.16b, v2.16b
.else
    umin v0.\T, v0.\T, v1.\T
.endif
.endm
.macro vo_min T, r, bits
.ifc \T, 2d
    cmgt v2.2d, v0.2d, v1.2d
    bit v0.16b, v1.16b, v2.16b
.else
    smin v0.\T, v0.\T, v1.\T
.endif
.endm
.macro vo_maxu T, r, bits
.ifc \T, 2d
    cmhi v2.2d, v1.2d, v0.2d
    bit v0.16b, v1.16b, v2.16b
.else
    umax v0.\T, v0.\T, v1.\T
.endif
.endm
.macro vo_max T, r, bits
.ifc \T, 2d
    cmgt v2.2d, v1.2d, v0.2d
    bit v0.16b, v1.16b, v2.16b
.else
    smax v0.\T, v0.\T, v1.\T
.endif
.endm
// shifts take the amount modulo SEW; ushl/sshl shift right for a negative one
.macro vshamt T, r, bits
    mov w9, #(\bits - 1)
    dup v7.\T, \r\()9
    and v2.16b, v1.16b, v7.16b
.endm
.macro vo_sll T, r, bits
    vshamt \T, \r, \bits
    ushl v0.\T, v0.\T, v2.\T
.endm
.macro vo_srl T, r, bits
    vshamt \T, \r, \bits
    neg v2.\T, v2.\T
    ushl v0.\T, v0.\T, v2.\T
.endm
.macro vo_sra T, r, bits
    vshamt \T, \r, \bits
    neg v2.\T, v2.\T
    sshl v0.\T, v0.\T, v2.\T
.endm
// Zvbb rotates: the two shifts, by r and by r - SEW (USHL by +-SEW is 0)
.macro vo_ror T, r, bits
    vshamt \T, \r, \bits
    neg v3.\T, v2.\T
    ushl v3.\T, v0.\T, v3.\T
    mov w9, #\bits
    dup v5.\T, \r\()9
    sub v5.\T, v5.\T, v2.\T
    ushl v0.\T, v0.\T, v5.\T
    orr v0.16b, v0.16b, v3.16b
.endm
.macro vo_rol T, r, bits
    vshamt \T, \r, \bits
    ushl v3.\T, v0.\T, v2.\T
    mov w9, #\bits
    dup v5.\T, \r\()9
    sub v5.\T, v2.\T, v5.\T
    ushl v0.\T, v0.\T, v5.\T
    orr v0.16b, v0.16b, v3.16b
.endm

// Multiplies. NEON has none at 64 bits: those go lane by lane through x6-x9.
// The multiply-adds read vd's chunk (x13) into v19.
.macro lanes2 i0
    mov x6, v0.d[0]
    mov x7, v1.d[0]
    \i0 x6, x6, x7
    mov x8, v0.d[1]
    mov x9, v1.d[1]
    \i0 x8, x8, x9
    mov v0.d[0], x6
    mov v0.d[1], x8
.endm
// v0 = \i0(\a, \b, \c) per 64-bit lane, the three named by v-register number
.macro lanes2_3 i0, a, b, c
.irp l, 0, 1
    mov x6, v\a\().d[\l]
    mov x7, v\b\().d[\l]
    mov x8, v\c\().d[\l]
    \i0 x6, x6, x7, x8
    mov v2.d[\l], x6
.endr
    mov v0.16b, v2.16b
.endm
.macro vo_mul T, r, bits
.ifc \T, 2d
    lanes2 mul
.else
    mul v0.\T, v0.\T, v1.\T
.endif
.endm
.macro vo_macc T, r, bits          // vd + vs1 * vs2
    ldr q19, [x13]
.ifc \T, 2d
    lanes2_3 madd, 0, 1, 19
.else
    mla v19.\T, v0.\T, v1.\T
    mov v0.16b, v19.16b
.endif
.endm
.macro vo_nmsac T, r, bits         // vd - vs1 * vs2
    ldr q19, [x13]
.ifc \T, 2d
    lanes2_3 msub, 0, 1, 19
.else
    mls v19.\T, v0.\T, v1.\T
    mov v0.16b, v19.16b
.endif
.endm
.macro vo_madd T, r, bits          // vs1 * vd + vs2
    ldr q19, [x13]
.ifc \T, 2d
    lanes2_3 madd, 1, 19, 0
.else
    mla v0.\T, v1.\T, v19.\T
.endif
.endm
.macro vo_nmsub T, r, bits         // vs2 - vs1 * vd
    ldr q19, [x13]
.ifc \T, 2d
    lanes2_3 msub, 1, 19, 0
.else
    mls v0.\T, v1.\T, v19.\T
.endif
.endm

// the second operand: vs1's group (vv: x15 its address), the scalar register
// (vx) or the immediate (vi), the scalar two dup'd into v1
.macro vsrc_setup kind, T, r
.ifc \kind, vv
    add x15, _cpu, x15
.endif
.ifc \kind, vx
    ldr x15, [_cpu, x15]
    dup v1.\T, \r\()15
.endif
.ifc \kind, vi
    dup v1.\T, \r\()15
.endif
.endm

// v4 = the lanes of this chunk whose v0 mask bit is set (x3:x4 the mask
// bits, x5 the chunk's first element, advanced; v6 the bit weights)
.macro vlanemask sew, lo=x3, hi=x4, pos=x5
    cmp \pos, #64
    csel x9, \lo, \hi, lo
    lsr x9, x9, \pos
    dup v4.8b, w9
    lsr w10, w9, #8
    dup v5.8b, w10
    ins v4.d[1], v5.d[0]
    cmtst v4.16b, v4.16b, v6.16b
.if \sew >= 2
    sxtl v4.8h, v4.8b
.endif
.if \sew >= 4
    sxtl v4.4s, v4.4h
.endif
.if \sew == 8
    sxtl v4.2d, v4.2s
.endif
    add \pos, \pos, #(16 / \sew)
.endm

// vd = vs2 op src: [gadget][vtype][vd_off][vs2_off][vs1_off|rs1_off|imm][insn][pc]
// The masked forms (vbinm_*) leave masked-off elements alone, except
// vmerge (op merge), which takes vs2's there and src's elsewhere.
.macro vbin op, kind, sew, T, lg, r, masked
.if \masked
.gadget vbinm_\op\()_\kind\()_e\sew
.else
.gadget vbin_\op\()_\kind\()_e\sew
.endif
    vt_check
vbody_\op\()_\kind\()_\masked\()_e\sew :
    ldr x12, [_cpu, #CPU_riscv64_vl]
    lsl x12, x12, #\lg
    cbz x12, 8f
    ldp x13, x14, [_ip, #8]
    ldr x15, [_ip, #24]
    add x13, _cpu, x13
    add x14, _cpu, x14
    vsrc_setup \kind, \T, \r
.if \masked
    ldr x3, [_cpu, #CPU_riscv64_v]
    ldr x4, [_cpu, #(CPU_riscv64_v + 8)]
    mov x5, #0
    adr x9, vmask_weights
    ldr q6, [x9]
.endif
1:  ldr q0, [x14]
.ifc \kind, vv
    ldr q1, [x15]
.endif
.if \masked
    vlanemask \sew
.endif
.ifc \op, merge
    bit v0.16b, v1.16b, v4.16b
.else
    vo_\op \T, \r, (\sew * 8)
.if \masked
    ldr q3, [x13]
    bif v0.16b, v3.16b, v4.16b
.endif
.endif
    cmp x12, #16
    b.lo 2f
    str q0, [x13], #16
    add x14, x14, #16
.ifc \kind, vv
    add x15, x15, #16
.endif
    subs x12, x12, #16
    b.ne 1b
    b 8f
2:  ldr q16, [x13]             // a last partial register: keep its tail
    adr x9, vbyte_index
    ldr q17, [x9]
    dup v18.16b, w12
    cmhi v18.16b, v18.16b, v17.16b
    bit v16.16b, v0.16b, v18.16b
    str q16, [x13]
8:  gret 6
.ifc \kind, vv
9:  vretype vbody_\op\()_\kind\()_\masked, 1, 1, 1, 40
.else
9:  vretype vbody_\op\()_\kind\()_\masked, 1, 1, 0, 40
.endif
5:  vtrap 40
.endm
.macro vbin_sews op, kind, masked
    vbin \op, \kind, 1, 16b, 0, w, \masked
    vbin \op, \kind, 2, 8h, 1, w, \masked
    vbin \op, \kind, 4, 4s, 2, w, \masked
    vbin \op, \kind, 8, 2d, 3, x, \masked
    vrow vbody_\op\()_\kind\()_\masked
.endm
.irp op, add, sub, rsub, and, or, xor, andn, mv, minu, min, maxu, max, sll, srl, sra, ror, rol, mul, macc, nmsac, madd, nmsub
.irp kind, vv, vx, vi
    vbin_sews \op, \kind, 0
    vbin_sews \op, \kind, 1
.endr
.endr
.irp kind, vv, vx, vi
    vbin_sews merge, \kind, 1
.endr

// Element by element in x registers, for what NEON has no lanes for: the
// high-half multiplies and division (RISC-V: x/0 is all ones, x%0 is x;
// AArch64's sdiv/udiv give 0, which msub turns into the right remainder).
// x10 = vs2[i] and x11 = vs1[i] or the scalar, extended as \asign/\bsign
// say; x7 = vd[i]; the result in x10. Masked-off elements and the tail are
// left alone. Stream as vbin's.
.macro vel_ld sz, sign, reg, base, i=x6
.if \sz == 1
.if \sign
    ldrsb x\reg, [\base, \i]
.else
    ldrb w\reg, [\base, \i]
.endif
.elseif \sz == 2
.if \sign
    ldrsh x\reg, [\base, \i, lsl #1]
.else
    ldrh w\reg, [\base, \i, lsl #1]
.endif
.elseif \sz == 4
.if \sign
    ldrsw x\reg, [\base, \i, lsl #2]
.else
    ldr w\reg, [\base, \i, lsl #2]
.endif
.else
    ldr x\reg, [\base, \i, lsl #3]
.endif
.endm
.macro vel_st sz, reg, base, i=x6
.if \sz == 1
    strb w\reg, [\base, \i]
.elseif \sz == 2
    strh w\reg, [\base, \i, lsl #1]
.elseif \sz == 4
    str w\reg, [\base, \i, lsl #2]
.else
    str x\reg, [\base, \i, lsl #3]
.endif
.endm
// extend x\reg's low \sz bytes as \sign says
.macro vel_ext sz, sign, reg
.if \sz == 1
.if \sign
    sxtb x\reg, w\reg
.else
    uxtb w\reg, w\reg
.endif
.elseif \sz == 2
.if \sign
    sxth x\reg, w\reg
.else
    uxth w\reg, w\reg
.endif
.elseif \sz == 4
.if \sign
    sxtw x\reg, w\reg
.else
    mov w\reg, w\reg
.endif
.endif
.endm
.macro ve_mulh sew
.if \sew == 8
    smulh x10, x10, x11
.else
    mul x10, x10, x11
    asr x10, x10, #(\sew * 8)
.endif
.endm
.macro ve_mulhu sew
.if \sew == 8
    umulh x10, x10, x11
.else
    mul x10, x10, x11
    lsr x10, x10, #(\sew * 8)
.endif
.endm
.macro ve_mulhsu sew                // signed vs2 times unsigned vs1
.if \sew == 8
    umulh x9, x10, x11
    cmp x10, #0
    csel x8, x11, xzr, lt
    sub x10, x9, x8
.else
    mul x10, x10, x11
    asr x10, x10, #(\sew * 8)
.endif
.endm
.macro ve_divu sew
    udiv x9, x10, x11
    cmp x11, #0
    csinv x10, x9, xzr, ne
.endm
.macro ve_div sew
    sdiv x9, x10, x11
    cmp x11, #0
    csinv x10, x9, xzr, ne
.endm
.macro ve_remu sew
    udiv x9, x10, x11
    msub x10, x9, x11, x10
.endm
.macro ve_rem sew
    sdiv x9, x10, x11
    msub x10, x9, x11, x10
.endm
.macro vel op, kind, sew, masked, asign, bsign
.gadget vel_\op\()_\kind\()_\masked\()_e\sew
    vt_check
velbody_\op\()_\kind\()_\masked\()_e\sew :
    ldr x12, [_cpu, #CPU_riscv64_vl]
    cbz x12, 8f
    ldp x13, x14, [_ip, #8]
    ldr x15, [_ip, #24]
    add x13, _cpu, x13
    add x14, _cpu, x14
.ifc \kind, vv
    add x15, _cpu, x15
.else
    ldr x11, [_cpu, x15]
    vel_ext \sew, \bsign, 11
.endif
.if \masked
    ldr x3, [_cpu, #CPU_riscv64_v]
    ldr x4, [_cpu, #(CPU_riscv64_v + 8)]
.endif
    mov x6, #0
1:
.if \masked
    cmp x6, #64
    csel x9, x3, x4, lo
    lsr x9, x9, x6
    tbz x9, #0, 2f
.endif
    vel_ld \sew, \asign, 10, x14
.ifc \kind, vv
    vel_ld \sew, \bsign, 11, x15
.endif
    ve_\op \sew
    vel_st \sew, 10, x13
2:  add x6, x6, #1
    cmp x6, x12
    b.lo 1b
8:  gret 6
.ifc \kind, vv
9:  vretype velbody_\op\()_\kind\()_\masked, 1, 1, 1, 40
.else
9:  vretype velbody_\op\()_\kind\()_\masked, 1, 1, 0, 40
.endif
5:  vtrap 40
.endm
.macro vel_sews op, kind, masked, asign, bsign
    vel \op, \kind, 1, \masked, \asign, \bsign
    vel \op, \kind, 2, \masked, \asign, \bsign
    vel \op, \kind, 4, \masked, \asign, \bsign
    vel \op, \kind, 8, \masked, \asign, \bsign
    vrow velbody_\op\()_\kind\()_\masked
.endm
.irp kind, vv, vx
.irp masked, 0, 1
    vel_sews divu, \kind, \masked, 0, 0
    vel_sews div, \kind, \masked, 1, 1
    vel_sews remu, \kind, \masked, 0, 0
    vel_sews rem, \kind, \masked, 1, 1
    vel_sews mulhu, \kind, \masked, 0, 0
    vel_sews mulhsu, \kind, \masked, 1, 0
    vel_sews mulh, \kind, \masked, 1, 1
.endr
.endr

// Widening and narrowing: element by element like vel, each operand its
// own width -- vs2 2*SEW if \aw, vd 2*SEW if \dw, vs1 or the scalar SEW --
// extended as \asign/\bsign say. SEW 8, 16, 32 (vrow3).
.macro vw_add sz
    add x10, x10, x11
.endm
.macro vw_sub sz
    sub x10, x10, x11
.endm
.macro vw_mul sz
    mul x10, x10, x11
.endm
.macro vw_macc sz
    madd x10, x10, x11, x7
.endm
.macro vw_srl sz                   // a 2*SEW vs2 shifted by up to 2*SEW-1
    and x11, x11, #(\sz * 16 - 1)
    lsr x10, x10, x11
.endm
.macro vw_sra sz
    and x11, x11, #(\sz * 16 - 1)
    asr x10, x10, x11
.endm
.macro vw_sll sz
    and x11, x11, #(\sz * 16 - 1)
    lsl x10, x10, x11
.endm
.macro vw name, opm, kind, sew, masked, aw, asign, bsign, dw
.gadget vw_\name\()_\kind\()_\masked\()_e\sew
    vt_check
vwbody_\name\()_\kind\()_\masked\()_e\sew :
    ldr x12, [_cpu, #CPU_riscv64_vl]
    cbz x12, 8f
    ldp x13, x14, [_ip, #8]
    ldr x15, [_ip, #24]
    add x13, _cpu, x13
    add x14, _cpu, x14
.ifc \kind, vv
    add x15, _cpu, x15
.endif
.ifc \kind, vx
    ldr x11, [_cpu, x15]
    vel_ext \sew, \bsign, 11
.endif
.ifc \kind, vi
    mov x11, x15
.endif
.if \masked
    ldr x3, [_cpu, #CPU_riscv64_v]
    ldr x4, [_cpu, #(CPU_riscv64_v + 8)]
.endif
    mov x6, #0
1:
.if \masked
    cmp x6, #64
    csel x9, x3, x4, lo
    lsr x9, x9, x6
    tbz x9, #0, 2f
.endif
    vel_ld (\sew << \aw), \asign, 10, x14
.ifc \kind, vv
    vel_ld \sew, \bsign, 11, x15
.endif
.ifc \opm, macc
    vel_ld (\sew << \dw), 0, 7, x13
.endif
    vw_\opm \sew
    vel_st (\sew << \dw), 10, x13
2:  add x6, x6, #1
    cmp x6, x12
    b.lo 1b
8:  gret 6
.ifc \kind, vv
9:  vretype vwbody_\name\()_\kind\()_\masked, (1 + \dw), (1 + \aw), 1, 40
.else
9:  vretype vwbody_\name\()_\kind\()_\masked, (1 + \dw), (1 + \aw), 0, 40
.endif
5:  vtrap 40
.endm
.macro vw_all name, opm, aw, asign, bsign, dw
.irp kind, vv, vx, vi
.irp masked, 0, 1
    vw \name, \opm, \kind, 1, \masked, \aw, \asign, \bsign, \dw
    vw \name, \opm, \kind, 2, \masked, \aw, \asign, \bsign, \dw
    vw \name, \opm, \kind, 4, \masked, \aw, \asign, \bsign, \dw
    vwrow \name, \kind, \masked
.endr
.endr
.endm
.macro vwrow name, kind, masked
    vrow3 vwbody_\name\()_\kind\()_\masked
.endm
// name, op, vs2 wide, vs2 signed, vs1/scalar signed, vd wide
vw_all waddu, add, 0, 0, 0, 1
vw_all wadd, add, 0, 1, 1, 1
vw_all wsubu, sub, 0, 0, 0, 1
vw_all wsub, sub, 0, 1, 1, 1
vw_all waddu_w, add, 1, 0, 0, 1
vw_all wadd_w, add, 1, 1, 1, 1
vw_all wsubu_w, sub, 1, 0, 0, 1
vw_all wsub_w, sub, 1, 1, 1, 1
vw_all wmulu, mul, 0, 0, 0, 1
vw_all wmulsu, mul, 0, 1, 0, 1
vw_all wmul, mul, 0, 1, 1, 1
vw_all wmaccu, macc, 0, 0, 0, 1
vw_all wmacc, macc, 0, 1, 1, 1
vw_all wmaccus, macc, 0, 1, 0, 1
vw_all wmaccsu, macc, 0, 0, 1, 1
vw_all nsrl, srl, 1, 0, 0, 0
vw_all nsra, sra, 1, 1, 0, 0
vw_all wsll, sll, 0, 0, 0, 1

// ---- element loops: permutations, carries, Zvbb unary ops ----
// Common shape: x12 = vl (0: nothing to do), x13 = vd, x14 = vs2, x15 the
// second operand -- vs1's address (vv), the scalar (vx, vf) or the
// immediate (vi) -- x6 the element, x3:x4 v0's mask bits if \masked.
.macro vloop_head kind, sew
    ldr x12, [_cpu, #CPU_riscv64_vl]
    cbz x12, 8f
    ldp x13, x14, [_ip, #8]
    ldr x15, [_ip, #24]
    add x13, _cpu, x13
    add x14, _cpu, x14
.ifc \kind, vv
    add x15, _cpu, x15
.endif
.ifc \kind, vx                     // all XLEN bits: offsets and indices are
    ldr x15, [_cpu, x15]           // not truncated to SEW
.endif
.ifc \kind, vf                     // an f register: NaN-boxed below 64 bits
    ldr x15, [_cpu, x15]
.if \sew == 4
    lsr x9, x15, #32
    cmn w9, #1
    mov w9, #0x7fc00000
    csel x15, x15, x9, eq
    mov w15, w15
.endif
.endif
    ldr x3, [_cpu, #CPU_riscv64_v]
    ldr x4, [_cpu, #(CPU_riscv64_v + 8)]
.endm
// skip to \skip unless element x6 is active (always active unmasked)
.macro vact masked, skip
.if \masked
    cmp x6, #64
    csel x9, x3, x4, lo
    lsr x9, x9, x6
    tbz x9, #0, \skip
.endif
.endm
// \out = VLMAX for the vtype in force, SEW \sew bytes
.macro vlmax_rt out, sew
    ldr x9, [_cpu, #CPU_riscv64_vtype]
    and x9, x9, #7
    mov x16, #(16 / \sew)
    lsl x17, x16, x9
    mov x10, #8
    sub x10, x10, x9
    lsr x16, x16, x10
    cmp x9, #4
    csel \out, x17, x16, lo
.endm
// the tail of every element-loop gadget: a 6-word stream
.macro vloop_tail row, vd, vs2, vs1
8:  gret 6
9:  vretype \row, \vd, \vs2, \vs1, 40
5:  vtrap 40
.endm

// vslideup.vx/vi: vd[i] = vs2[i - off] for off <= i < vl, highest first (so
// an overlapping vd, which is reserved, still reads its sources first)
.macro vslideup kind, sew, masked
.gadget vpslideup_\kind\()_\masked\()_e\sew
    vt_check
vpbody_slideup_\kind\()_\masked\()_e\sew :
    vloop_head \kind, \sew
    cmp x15, x12
    b.hs 8f
    sub x6, x12, #1
1:  vact \masked, 2f
    sub x7, x6, x15
    vel_ld \sew, 0, 10, x14, x7
    vel_st \sew, 10, x13
2:  cmp x6, x15
    sub x6, x6, #1
    b.hi 1b
    vloop_tail vpbody_slideup_\kind\()_\masked, 1, 1, 0
.endm
// vslidedown.vx/vi: vd[i] = vs2[i + off], 0 past VLMAX
.macro vslidedown kind, sew, masked
.gadget vpslidedown_\kind\()_\masked\()_e\sew
    vt_check
vpbody_slidedown_\kind\()_\masked\()_e\sew :
    vloop_head \kind, \sew
    vlmax_rt x5, \sew
    mov x6, #0
1:  vact \masked, 2f
    mov x10, #0
    cmp x15, x5
    b.hs 3f
    add x7, x6, x15
    cmp x7, x5
    b.hs 3f
    vel_ld \sew, 0, 10, x14, x7
3:  vel_st \sew, 10, x13
2:  add x6, x6, #1
    cmp x6, x12
    b.lo 1b
    vloop_tail vpbody_slidedown_\kind\()_\masked, 1, 1, 0
.endm
// vslide1up: vd[0] = the scalar, vd[i] = vs2[i - 1]; highest first
.macro vslide1up kind, sew, masked
.gadget vpslide1up_\kind\()_\masked\()_e\sew
    vt_check
vpbody_slide1up_\kind\()_\masked\()_e\sew :
    vloop_head \kind, \sew
    sub x6, x12, #1
1:  vact \masked, 2f
    mov x10, x15
    cbz x6, 3f
    sub x7, x6, #1
    vel_ld \sew, 0, 10, x14, x7
3:  vel_st \sew, 10, x13
2:  cbz x6, 8f
    sub x6, x6, #1
    b 1b
    vloop_tail vpbody_slide1up_\kind\()_\masked, 1, 1, 0
.endm
// vslide1down: vd[i] = vs2[i + 1], vd[vl - 1] = the scalar
.macro vslide1down kind, sew, masked
.gadget vpslide1down_\kind\()_\masked\()_e\sew
    vt_check
vpbody_slide1down_\kind\()_\masked\()_e\sew :
    vloop_head \kind, \sew
    sub x5, x12, #1
    mov x6, #0
1:  vact \masked, 2f
    mov x10, x15
    cmp x6, x5
    b.eq 3f
    add x7, x6, #1
    vel_ld \sew, 0, 10, x14, x7
3:  vel_st \sew, 10, x13
2:  add x6, x6, #1
    cmp x6, x12
    b.lo 1b
    vloop_tail vpbody_slide1down_\kind\()_\masked, 1, 1, 0
.endm
// vrgather.vv/vx/vi and vrgatherei16.vv (\isz 2): vd[i] = vs2[index], 0
// past VLMAX
.macro vrgather kind, sew, masked, isz=0
.if \isz
.gadget vpgatherei16_\kind\()_\masked\()_e\sew
.else
.gadget vpgather_\kind\()_\masked\()_e\sew
.endif
    vt_check
.if \isz
vpbody_gatherei16_\kind\()_\masked\()_e\sew :
.else
vpbody_gather_\kind\()_\masked\()_e\sew :
.endif
    vloop_head \kind, \sew
    vlmax_rt x5, \sew
    mov x6, #0
1:  vact \masked, 2f
.ifc \kind, vv
.if \isz
    vel_ld 2, 0, 7, x15
.else
    vel_ld \sew, 0, 7, x15
.endif
.else
    mov x7, x15
.endif
    mov x10, #0
    cmp x7, x5
    b.hs 3f
    vel_ld \sew, 0, 10, x14, x7
3:  vel_st \sew, 10, x13
2:  add x6, x6, #1
    cmp x6, x12
    b.lo 1b
.if \isz
    vloop_tail vpbody_gatherei16_\kind\()_\masked, 1, 1, 0
.else
.ifc \kind, vv
    vloop_tail vpbody_gather_\kind\()_\masked, 1, 1, 1
.else
    vloop_tail vpbody_gather_\kind\()_\masked, 1, 1, 0
.endif
.endif
.endm
// vcompress.vm vd, vs2, vs1 (unmasked only): vs2's elements whose vs1 bit
// is set, packed from vd[0]
.macro vcompress sew
.gadget vpcompress_e\sew
    vt_check
vpbody_compress_e\sew :
    vloop_head vv, \sew
    ldp x3, x4, [x15]
    mov x6, #0
    mov x8, #0
1:  cmp x6, #64
    csel x9, x3, x4, lo
    lsr x9, x9, x6
    tbz x9, #0, 2f
    vel_ld \sew, 0, 10, x14
    vel_st \sew, 10, x13, x8
    add x8, x8, #1
2:  add x6, x6, #1
    cmp x6, x12
    b.lo 1b
    vloop_tail vpbody_compress, 1, 1, 0
.endm
// viota.m vd, vs2: vd[i] = the set bits of vs2 below i among the active
// elements
.macro viota sew, masked
.gadget vpiota_\masked\()_e\sew
    vt_check
vpbody_iota_\masked\()_e\sew :
    vloop_head vv, \sew
    ldp x7, x8, [x14]
    mov x6, #0
    mov x10, #0
1:  vact \masked, 2f
    vel_st \sew, 10, x13
    cmp x6, #64
    csel x9, x7, x8, lo
    lsr x9, x9, x6
    and x9, x9, #1
    add x10, x10, x9
2:  add x6, x6, #1
    cmp x6, x12
    b.lo 1b
    vloop_tail vpbody_iota_\masked, 1, 0, 0
.endm

// vadc/vsbc (\op add/sub): vd[i] = vs2[i] +- src +- v0[i] (v0 a carry, not
// a mask); vmadc/vmsbc (\mask 1): vd's bit i = that carry or borrow out,
// with the carry in only if \cin
.macro vcarry op, kind, sew, mask, cin
.gadget vc\op\()_\kind\()_\mask\()\cin\()_e\sew
    vt_check
vcbody_\op\()_\kind\()_\mask\()\cin\()_e\sew :
    vloop_head \kind, \sew
    mov x6, #0
    mov x16, #0                     // the mask result, low and high
    mov x17, #0
1:  vel_ld \sew, 0, 10, x14
.ifc \kind, vv
    vel_ld \sew, 0, 11, x15
.else
    mov x11, x15
.if \sew < 8
    and x11, x11, #((1 << (\sew * 8)) - 1)
.endif
.endif
.if \cin
    cmp x6, #64
    csel x9, x3, x4, lo
    lsr x9, x9, x6
    and x9, x9, #1
.else
    mov x9, #0
.endif
.if \sew == 8
.ifc \op, add
    adds x10, x10, x11
    cset x7, cs
    adds x10, x10, x9
    cinc x7, x7, cs
.else
    subs x10, x10, x11
    cset x7, cc
    subs x10, x10, x9
    cinc x7, x7, cc
.endif
.else
.ifc \op, add
    add x10, x10, x11
    add x10, x10, x9
.else
    sub x10, x10, x11
    sub x10, x10, x9
.endif
    lsr x7, x10, #(\sew * 8)
.endif
.if \mask
    and x7, x7, #1
    lsl x7, x7, x6
    cmp x6, #64
    b.hs 3f
    orr x16, x16, x7
    b 2f
3:  orr x17, x17, x7
.else
    vel_st \sew, 10, x13
.endif
2:  add x6, x6, #1
    cmp x6, x12
    b.lo 1b
.if \mask
    vlmask x9, x10, x12, x11
    vmask_store x13, x16, x17, x9, x10
.endif
.if \mask
.ifc \kind, vv
    vloop_tail vcbody_\op\()_\kind\()_\mask\()\cin, 0, 1, 1
.else
    vloop_tail vcbody_\op\()_\kind\()_\mask\()\cin, 0, 1, 0
.endif
.else
.ifc \kind, vv
    vloop_tail vcbody_\op\()_\kind\()_\mask\()\cin, 1, 1, 1
.else
    vloop_tail vcbody_\op\()_\kind\()_\mask\()\cin, 1, 1, 0
.endif
.endif
.endm

// Zvbb unary: vbrev8 vrev8 vbrev vclz vctz vcpop.v, element by element
.macro vu_brev8 sew
    rbit x10, x10
    rev x10, x10
.endm
.macro vu_rev8 sew
    rev x10, x10
.if \sew < 8
    lsr x10, x10, #(64 - \sew * 8)
.endif
.endm
.macro vu_brev sew
    rbit x10, x10
.if \sew < 8
    lsr x10, x10, #(64 - \sew * 8)
.endif
.endm
.macro vu_clz sew
    clz x10, x10
.if \sew < 8
    sub x10, x10, #(64 - \sew * 8)
.endif
.endm
.macro vu_ctz sew
.if \sew < 8
    orr x10, x10, #(1 << (\sew * 8))
.endif
    rbit x10, x10
    clz x10, x10
.endm
.macro vu_cpop sew
    fmov d0, x10
    cnt v0.8b, v0.8b
    addv b0, v0.8b
    umov w10, v0.b[0]
.endm
.macro vunary op, sew, masked
.gadget vu\op\()_\masked\()_e\sew
    vt_check
vubody_\op\()_\masked\()_e\sew :
    vloop_head vv, \sew
    mov x6, #0
1:  vact \masked, 2f
    vel_ld \sew, 0, 10, x14
    vu_\op \sew
    vel_st \sew, 10, x13
2:  add x6, x6, #1
    cmp x6, x12
    b.lo 1b
    vloop_tail vubody_\op\()_\masked, 1, 1, 0
.endm

// instances and their rows
.macro vperm_sews m, a, b, c
    \m \a, 1, \b
    \m \a, 2, \b
    \m \a, 4, \b
    \m \a, 8, \b
.endm
.macro vprow name, kind, masked
    vrow vpbody_\name\()_\kind\()_\masked
.endm
.irp kind, vx, vi
.irp masked, 0, 1
    vperm_sews vslideup, \kind, \masked
    vprow slideup, \kind, \masked
    vperm_sews vslidedown, \kind, \masked
    vprow slidedown, \kind, \masked
.endr
.endr
// FP (vfslide1up/down) has no SEW 8 or 16 here (no Zvfh)
.macro vrowf name
\name :
    .word vtrap_ill_40 - \name, vtrap_ill_40 - \name, \name\()_e4 - \name, \name\()_e8 - \name
.endm
.macro vprowf name, kind, masked
    vrowf vpbody_\name\()_\kind\()_\masked
.endm
.irp masked, 0, 1
    vperm_sews vslide1up, vx, \masked
    vprow slide1up, vx, \masked
    vperm_sews vslide1down, vx, \masked
    vprow slide1down, vx, \masked
    vperm_sews vslide1up, vf, \masked
    vprowf slide1up, vf, \masked
    vperm_sews vslide1down, vf, \masked
    vprowf slide1down, vf, \masked
.endr
.irp kind, vv, vx, vi
.irp masked, 0, 1
    vperm_sews vrgather, \kind, \masked
    vprow gather, \kind, \masked
.endr
.endr
.irp masked, 0, 1
.irp sew, 1, 2, 4, 8
    vrgather vv, \sew, \masked, 2
.endr
    vprow gatherei16, vv, \masked
.endr
.irp sew, 1, 2, 4, 8
    vcompress \sew
.endr
    vrow vpbody_compress
.irp masked, 0, 1
.irp sew, 1, 2, 4, 8
    viota \sew, \masked
.endr
    vrow vpbody_iota_\masked
.endr
.macro vcrow op, kind, mask, cin
    vrow vcbody_\op\()_\kind\()_\mask\()\cin
.endm
.macro vcarry_sews op, kind, mask, cin
    vcarry \op, \kind, 1, \mask, \cin
    vcarry \op, \kind, 2, \mask, \cin
    vcarry \op, \kind, 4, \mask, \cin
    vcarry \op, \kind, 8, \mask, \cin
    vcrow \op, \kind, \mask, \cin
.endm
.irp op, add, sub
.irp kind, vv, vx, vi
    vcarry_sews \op, \kind, 0, 1     // vadc / vsbc
    vcarry_sews \op, \kind, 1, 0     // vmadc / vmsbc, no carry in
    vcarry_sews \op, \kind, 1, 1     // vmadc / vmsbc with v0's
.endr
.endr
.macro vurow op, masked
    vrow vubody_\op\()_\masked
.endm
.irp op, brev8, rev8, brev, clz, ctz, cpop
.irp masked, 0, 1
.irp sew, 1, 2, 4, 8
    vunary \op, \sew, \masked
.endr
    vurow \op, \masked
.endr
.endr

// ---- floating point: SEW 32 and 64 (no Zvfh: SEW 8 and 16 trap) ----
// The host FPCR carries frm and default-NaN while the guest runs and the
// host's flags become fflags (emu/fpenv.c), so NEON lanes round and raise
// as RISC-V does. Lanes that are masked off or past vl are given an input
// that raises nothing (v21: 0, or 1 for the divisions) before the op and
// get vd's old value back after it.

// v1 = the f register in stream word 3, NaN-boxed at SEW 32
.macro vf_scalar sew
    ldr x15, [_cpu, x15]
.if \sew == 4
    lsr x9, x15, #32
    cmn w9, #1
    mov w9, #0x7fc00000
    csel w15, w15, w9, eq
    dup v1.4s, w15
.else
    dup v1.2d, x15
.endif
.endm
// raise fflags bits in the host FPSR (IOC 1 = NV, DZC 2 = DZ, OFC 4 = OF,
// UFC 8 = UF, IXC 16 = NX)
.macro vraise bits
    mrs x9, fpsr
    orr x9, x9, #\bits
    msr fpsr, x9
.endm
// RISC-V min/max (minimumNumber): the non-NaN operand, even against a
// signalling NaN, where fminnm gives the default NaN (it still raises NV)
.macro vf_minmax insn, T
    \insn v24.\T, v0.\T, v23.\T
    fcmeq v25.\T, v0.\T, v0.\T
    fcmeq v26.\T, v23.\T, v23.\T
    bic v27.16b, v26.16b, v25.16b
    bit v24.16b, v23.16b, v27.16b
    bic v27.16b, v25.16b, v26.16b
    bit v24.16b, v0.16b, v27.16b
    mov v0.16b, v24.16b
.endm
// ops: v0 = vs2, v23 = vs1 or the scalar, v22 = vd; result in v0
.macro vfo_add T
    fadd v0.\T, v0.\T, v23.\T
.endm
.macro vfo_sub T
    fsub v0.\T, v0.\T, v23.\T
.endm
.macro vfo_rsub T
    fsub v0.\T, v23.\T, v0.\T
.endm
.macro vfo_mul T
    fmul v0.\T, v0.\T, v23.\T
.endm
.macro vfo_div T
    fdiv v0.\T, v0.\T, v23.\T
.endm
.macro vfo_rdiv T
    fdiv v0.\T, v23.\T, v0.\T
.endm
.macro vfo_min T
    vf_minmax fminnm, \T
.endm
.macro vfo_max T
    vf_minmax fmaxnm, \T
.endm
.macro vfo_sgnj T
    movi v2.2d, #0
    fneg v2.\T, v2.\T
    bit v0.16b, v23.16b, v2.16b
.endm
.macro vfo_sgnjn T
    movi v2.2d, #0
    fneg v2.\T, v2.\T
    mvn v3.16b, v23.16b
    bit v0.16b, v3.16b, v2.16b
.endm
.macro vfo_sgnjx T
    movi v2.2d, #0
    fneg v2.\T, v2.\T
    and v3.16b, v23.16b, v2.16b
    eor v0.16b, v0.16b, v3.16b
.endm
.macro vfo_mv T
    mov v0.16b, v23.16b
.endm
.macro vfo_sqrt T
    fsqrt v0.\T, v0.\T
.endm
.macro vfo_macc T                  // +(vs1 * vs2) + vd
    fmla v22.\T, v23.\T, v0.\T
    mov v0.16b, v22.16b
.endm
.macro vfo_nmacc T                 // -(vs1 * vs2) - vd
    fneg v22.\T, v22.\T
    fmls v22.\T, v23.\T, v0.\T
    mov v0.16b, v22.16b
.endm
.macro vfo_msac T                  // +(vs1 * vs2) - vd
    fneg v22.\T, v22.\T
    fmla v22.\T, v23.\T, v0.\T
    mov v0.16b, v22.16b
.endm
.macro vfo_nmsac T                 // -(vs1 * vs2) + vd
    fmls v22.\T, v23.\T, v0.\T
    mov v0.16b, v22.16b
.endm
.macro vfo_madd T                  // +(vs1 * vd) + vs2
    fmla v0.\T, v23.\T, v22.\T
.endm
.macro vfo_nmadd T                 // -(vs1 * vd) - vs2
    fneg v0.\T, v0.\T
    fmls v0.\T, v23.\T, v22.\T
.endm
.macro vfo_msub T                  // +(vs1 * vd) - vs2
    fneg v0.\T, v0.\T
    fmla v0.\T, v23.\T, v22.\T
.endm
.macro vfo_nmsub T                 // -(vs1 * vd) + vs2
    fmls v0.\T, v23.\T, v22.\T
.endm

// vd = op(vs2, vs1 or f): [gadget][vtype][vd][vs2][vs1 | f reg][insn][pc].
// merge (vfmerge.vfm) takes f where v0 is set and vs2 elsewhere.
.macro vfb op, kind, sew, T, lg, masked
.gadget vfb_\op\()_\kind\()_\masked\()_e\sew
    vt_check
vfbbody_\op\()_\kind\()_\masked\()_e\sew :
    ldr x12, [_cpu, #CPU_riscv64_vl]
    lsl x12, x12, #\lg
    cbz x12, 8f
    ldp x13, x14, [_ip, #8]
    ldr x15, [_ip, #24]
    add x13, _cpu, x13
    add x14, _cpu, x14
.ifc \kind, vv
    add x15, _cpu, x15
.else
    vf_scalar \sew
.endif
.ifc \op, div
    fmov v21.\T, #1.0
.else
.ifc \op, rdiv
    fmov v21.\T, #1.0
.else
    movi v21.2d, #0
.endif
.endif
    ldr x3, [_cpu, #CPU_riscv64_v]
    ldr x4, [_cpu, #(CPU_riscv64_v + 8)]
    mov x5, #0
    adr x9, vmask_weights
    ldr q6, [x9]
    adr x9, vbyte_index
    ldr q17, [x9]
1:  ldr q0, [x14]
.ifc \kind, vv
    ldr q23, [x15]
.else
    mov v23.16b, v1.16b
.endif
    ldr q19, [x13]
    movi v20.2d, #0xffffffffffffffff
    vlanemask \sew
.if \masked
.ifnc \op, merge                  // vfmerge's v0 picks the source, not the lanes
    mov v20.16b, v4.16b
.endif
.endif
    cmp x12, #16
    b.hs 3f
    dup v18.16b, w12
    cmhi v18.16b, v18.16b, v17.16b
    and v20.16b, v20.16b, v18.16b
3:
.ifc \op, merge
    bit v0.16b, v23.16b, v4.16b
.else
    bif v0.16b, v21.16b, v20.16b
    bif v23.16b, v21.16b, v20.16b
    mov v22.16b, v19.16b
    bif v22.16b, v21.16b, v20.16b
    vfo_\op \T
.endif
    bif v0.16b, v19.16b, v20.16b
    str q0, [x13], #16
    add x14, x14, #16
.ifc \kind, vv
    add x15, x15, #16
.endif
    subs x12, x12, #16
    b.gt 1b
8:  gret 6
.ifc \kind, vv
9:  vretype vfbbody_\op\()_\kind\()_\masked, 1, 1, 1, 40
.else
9:  vretype vfbbody_\op\()_\kind\()_\masked, 1, 1, 0, 40
.endif
5:  vtrap 40
.endm
.macro vfb_sews op, kind, masked
    vfb \op, \kind, 4, 4s, 2, \masked
    vfb \op, \kind, 8, 2d, 3, \masked
    vrowf vfbbody_\op\()_\kind\()_\masked
.endm
.irp op, add, sub, rsub, mul, div, rdiv, min, max, sgnj, sgnjn, sgnjx, mv, sqrt, macc, nmacc, msac, nmsac, madd, nmadd, msub, nmsub, merge
.irp kind, vv, vf
    vfb_sews \op, \kind, 0
    vfb_sews \op, \kind, 1
.endr
.endr

// compares: vd's low vl bits (active ones, if masked) = vs2 op vs1 or f
.macro vfc_eq T
    fcmeq v0.\T, v0.\T, v23.\T
.endm
.macro vfc_ne T
    fcmeq v0.\T, v0.\T, v23.\T
    mvn v0.16b, v0.16b
.endm
.macro vfc_lt T
    fcmgt v0.\T, v23.\T, v0.\T
.endm
.macro vfc_le T
    fcmge v0.\T, v23.\T, v0.\T
.endm
.macro vfc_gt T
    fcmgt v0.\T, v0.\T, v23.\T
.endm
.macro vfc_ge T
    fcmge v0.\T, v0.\T, v23.\T
.endm
.macro vfc op, kind, sew, T, lg, masked
.gadget vfc_\op\()_\kind\()_\masked\()_e\sew
    vt_check
vfcbody_\op\()_\kind\()_\masked\()_e\sew :
    ldr x12, [_cpu, #CPU_riscv64_vl]
    cbz x12, 8f
    ldp x13, x14, [_ip, #8]
    ldr x15, [_ip, #24]
    add x14, _cpu, x14
.ifc \kind, vv
    add x15, _cpu, x15
.else
    vf_scalar \sew
.endif
    lsl x6, x12, #\lg
    movi v21.2d, #0
    ldr x16, [_cpu, #CPU_riscv64_v]
    ldr x17, [_cpu, #(CPU_riscv64_v + 8)]
    mov x3, #0
    mov x4, #0
    mov x5, #0
    mov x7, #0
    adr x9, vmask_weights
    ldr q6, [x9]
    adr x9, vbyte_index
    ldr q17, [x9]
1:  ldr q0, [x14], #16
.ifc \kind, vv
    ldr q23, [x15], #16
.else
    mov v23.16b, v1.16b
.endif
    movi v20.2d, #0xffffffffffffffff
.if \masked
    vlanemask \sew, x16, x17, x7
    mov v20.16b, v4.16b
.endif
    cmp x6, #16
    b.hs 3f
    dup v18.16b, w6
    cmhi v18.16b, v18.16b, v17.16b
    and v20.16b, v20.16b, v18.16b
3:  bif v0.16b, v21.16b, v20.16b
    bif v23.16b, v21.16b, v20.16b
    vfc_\op \T
.if \sew == 8
    xtn v0.2s, v0.2d
.endif
    xtn v0.4h, v0.4s
    xtn v0.8b, v0.8h
    and v0.8b, v0.8b, v6.8b
    addv b2, v0.8b
    umov w9, v2.b[0]
    lsl x10, x9, x5
    cmp x5, #64
    b.hs 4f
    orr x3, x3, x10
    b 6f
4:  orr x4, x4, x10
6:  add x5, x5, #(16 / \sew)
    subs x6, x6, #16
    b.gt 1b
    vlmask x9, x10, x12, x11
.if \masked
    and x9, x9, x16
    and x10, x10, x17
.endif
    add x13, _cpu, x13
    vmask_store x13, x3, x4, x9, x10
8:  gret 6
.ifc \kind, vv
9:  vretype vfcbody_\op\()_\kind\()_\masked, 0, 1, 1, 40
.else
9:  vretype vfcbody_\op\()_\kind\()_\masked, 0, 1, 0, 40
.endif
5:  vtrap 40
.endm
.macro vfc_sews op, kind, masked
    vfc \op, \kind, 4, 4s, 2, \masked
    vfc \op, \kind, 8, 2d, 3, \masked
    vrowf vfcbody_\op\()_\kind\()_\masked
.endm
.irp op, eq, ne, lt, le, gt, ge
.irp kind, vv, vf
    vfc_sews \op, \kind, 0
    vfc_sews \op, \kind, 1
.endr
.endr

// vfmv.f.s fd, vs2: [gadget][vtype][fd][vs2][insn][pc] (NaN-boxed at SEW 32)
// vfmv.s.f vd, fs1: [gadget][vtype][vd][fs1][insn][pc]
.macro vfscalar sew
.gadget vfmv_f_s_e\sew
    vt_check
vfmvfsbody_e\sew :
    ldp x13, x14, [_ip, #8]
.if \sew == 4
    ldr w9, [_cpu, x14]
    orr x9, x9, #0xffffffff00000000
.else
    ldr x9, [_cpu, x14]
.endif
    str x9, [_cpu, x13]
    gret 5
9:  vretype vfmvfsbody, 0, 0, 0, 32
5:  vtrap 32
.gadget vfmv_s_f_e\sew
    vt_check
vfmvsfbody_e\sew :
    ldr x12, [_cpu, #CPU_riscv64_vl]
    cbz x12, 8f
    ldp x13, x15, [_ip, #8]
    vf_scalar \sew
.if \sew == 4
    str s1, [_cpu, x13]
.else
    str d1, [_cpu, x13]
.endif
8:  gret 5
9:  vretype vfmvsfbody, 0, 0, 0, 32
5:  vtrap 32
.endm
vfscalar 4
vfscalar 8
.macro vrowf5 name                  // vrowf for a 5-word stream
\name :
    .word vtrap_ill_32 - \name, vtrap_ill_32 - \name, \name\()_e4 - \name, \name\()_e8 - \name
.endm
vtrap_ill_32:
    ldr x9, [_ip, #32]
    str x9, [_cpu, #CPU_riscv64_pc]
    mov w0, #INT_UNDEFINED
    b jit_exit
vrowf5 vfmvfsbody
vrowf5 vfmvsfbody

// ---- FP element by element: conversions, reductions, widening, class,
// the 7-bit estimates. Scalar FP registers, so masked-off elements are
// never computed and raise nothing.
.macro vraise2 a, b
    mrs x9, fpsr
    orr x9, x9, #\a
    orr x9, x9, #\b
    msr fpsr, x9
.endm
// rows whose SEWs exist only where \x1..\x8 say (the rest trap)
.macro vrowsel name, x1, x2, x4, x8
\name :
.if \x1
    .word \name\()_e1 - \name
.else
    .word vtrap_ill_40 - \name
.endif
.if \x2
    .word \name\()_e2 - \name
.else
    .word vtrap_ill_40 - \name
.endif
.if \x4
    .word \name\()_e4 - \name
.else
    .word vtrap_ill_40 - \name
.endif
.if \x8
    .word \name\()_e8 - \name
.else
    .word vtrap_ill_40 - \name
.endif
.endm

// d16 (exact) -> an integer of \ibits in x10, as RISC-V converts: rounded
// by frm (or toward zero), saturating, NaN to the largest; NV alone when
// out of range, NX only for an inexact in-range result
.macro vf2i ibits, sign, rtz
    fcmp d16, d16
    b.vs 76f
.if \rtz
    frintz d17, d16
.else
    frinti d17, d16
.endif
.if \sign
    mov x9, #((1023 + \ibits - 1) << 52)
.else
    mov x9, #((1023 + \ibits) << 52)
.endif
    fmov d18, x9
    fcmp d17, d18
    b.ge 76f
.if \sign
    mov x9, #(((1023 + \ibits - 1) << 52) | (1 << 63))
    fmov d18, x9
    fcmp d17, d18
    b.lt 77f
    fcvtzs x10, d17
.else
    fcmp d17, #0.0
    b.lt 77f
    fcvtzu x10, d17
.endif
    fcmp d17, d16
    b.eq 79f
    vraise 16
    b 79f
76: vraise 1
.if \sign
    mov x10, #((1 << (\ibits - 1)) - 1)
.else
    mov x10, #-1
.endif
    b 79f
77: vraise 1
.if \sign
    mov x10, #(1 << (\ibits - 1))
.else
    mov x10, #0
.endif
79:
.endm
// conversions on x10: float of \ssz bytes -> integer, integer -> float of
// \dsz, float -> float
.macro vcv_f2i ssz, dsz, sign, rtz
.if \ssz == 4
    fmov s16, w10
    fcvt d16, s16
.else
    fmov d16, x10
.endif
    vf2i (\dsz * 8), \sign, \rtz
.endm
.macro vcv_i2f ssz, dsz, sign, unused=0
.if \dsz == 4
.if \sign
    scvtf s16, x10
.else
    ucvtf s16, x10
.endif
    fmov w10, s16
.else
.if \sign
    scvtf d16, x10
.else
    ucvtf d16, x10
.endif
    fmov x10, d16
.endif
.endm
.macro vcv_f2f ssz, dsz, rod, unused=0
.if \ssz == 2
    fmov s16, w10
    fcvt s16, h16
    fmov w10, s16
.elseif \ssz == 4 && \dsz == 8
    fmov s16, w10
    fcvt d16, s16
    fmov x10, d16
.elseif \ssz == 4
    fmov s16, w10
    fcvt h16, s16
    fmov w10, s16
.else
    fmov d16, x10
.if \rod
    fcvtxn s16, d16
.else
    fcvt s16, d16
.endif
    fmov w10, s16
.endif
.endm
// vfclass, vfrsqrt7, vfrec7 on x10, a float of \m fraction and \e exponent
// bits
.macro vf_class m, e
    ubfx x7, x10, #\m, #\e
    and x8, x10, #((1 << \m) - 1)
    lsr x11, x10, #(\m + \e)
    and x11, x11, #1
    cmp x7, #((1 << \e) - 1)
    b.ne 71f
    cbnz x8, 72f
    mov x10, #1                     // -inf 0, +inf 7
    mov x9, #(1 << 7)
    cmp x11, #0
    csel x10, x9, x10, eq
    b 79f
72: tst x8, #(1 << (\m - 1))         // sNaN 8, qNaN 9
    mov x10, #(1 << 8)
    mov x9, #(1 << 9)
    csel x10, x9, x10, ne
    b 79f
71: cbnz x7, 73f
    cbnz x8, 74f
    mov x10, #(1 << 4)              // -0 3, +0 4
    mov x9, #(1 << 3)
    cmp x11, #0
    csel x10, x9, x10, ne
    b 79f
74: mov x10, #(1 << 5)              // -subnormal 2, +subnormal 5
    mov x9, #(1 << 2)
    cmp x11, #0
    csel x10, x9, x10, ne
    b 79f
73: mov x10, #(1 << 6)              // -normal 1, +normal 6
    mov x9, #(1 << 1)
    cmp x11, #0
    csel x10, x9, x10, ne
79:
.endm
// a subnormal's fraction (x8) normalized: x7 = its exponent (0 - shift),
// x8 the fraction below its leading 1
.macro vf_normalize m
    clz x9, x8
    sub x9, x9, #(64 - \m)
    neg x7, x9
    add x9, x9, #1
    lsl x8, x8, x9
    and x8, x8, #((1 << \m) - 1)
.endm
.macro vf_rsqrt7 m, e
    ubfx x7, x10, #\m, #\e
    and x8, x10, #((1 << \m) - 1)
    lsr x11, x10, #(\m + \e)
    and x11, x11, #1
    cmp x7, #((1 << \e) - 1)
    b.ne 71f
    cbz x8, 72f
    tbnz x8, #(\m - 1), 70f         // NaN: NV if signalling
    vraise 1
70: mov x10, #((((1 << \e) - 1) << \m) | (1 << (\m - 1)))
    b 79f
72: cbnz x11, 73f                   // +inf: +0
    mov x10, #0
    b 79f
71: cbnz x7, 74f
    cbnz x8, 74f
    vraise 2                        // +-0: +-inf, DZ
    lsl x10, x11, #(\m + \e)
    orr x10, x10, #(((1 << \e) - 1) << \m)
    b 79f
74: cbz x11, 75f
73: vraise 1                        // negative: the canonical NaN, NV
    mov x10, #((((1 << \e) - 1) << \m) | (1 << (\m - 1)))
    b 79f
75: cbnz x7, 76f
    vf_normalize \m
76: and x9, x7, #1
    lsr x16, x8, #(\m - 6)
    orr x9, x16, x9, lsl #6
    adr x16, rsqrt7_table
    ldrb w9, [x16, x9]
    lsl x9, x9, #(\m - 7)
    mov x16, #(3 * ((1 << (\e - 1)) - 1) - 1)
    sub x16, x16, x7
    lsr x16, x16, #1
    orr x10, x9, x16, lsl #\m
79:
.endm
.macro vf_rec7 m, e
    ubfx x7, x10, #\m, #\e
    and x8, x10, #((1 << \m) - 1)
    lsr x11, x10, #(\m + \e)
    and x11, x11, #1
    lsl x5, x11, #(\m + \e)
    cmp x7, #((1 << \e) - 1)
    b.ne 71f
    cbz x8, 72f
    tbnz x8, #(\m - 1), 70f
    vraise 1
70: mov x10, #((((1 << \e) - 1) << \m) | (1 << (\m - 1)))
    b 79f
72: mov x10, x5                     // +-inf: +-0
    b 79f
71: cbnz x7, 74f
    cbnz x8, 73f
    vraise 2                        // +-0: +-inf, DZ
    orr x10, x5, #(((1 << \e) - 1) << \m)
    b 79f
73: vf_normalize \m
74: mov x16, #(2 * ((1 << (\e - 1)) - 1) - 1)
    sub x16, x16, x7                // the result's exponent, signed
    cmn x16, #1
    b.lt 77f
    cmp x16, #(2 * ((1 << (\e - 1)) - 1))
    b.gt 77f
    lsr x9, x8, #(\m - 7)
    adr x17, rec7_table
    ldrb w9, [x17, x9]
    lsl x9, x9, #(\m - 7)
    cmp x16, #0
    b.gt 75f
    orr x9, x9, #(1 << \m)           // a subnormal result
    mov x17, #1
    sub x17, x17, x16
    lsr x9, x9, x17
    mov x16, #0
75: orr x10, x5, x9
    orr x10, x10, x16, lsl #\m
    b 79f
77: vraise2 4, 16                   // a tiny input: overflow, rounded by frm
    mrs x9, fpcr
    ubfx x9, x9, #22, #2            // RMode: 0 RNE, 1 RUP, 2 RDN, 3 RTZ
    cbz x9, 78f
    cmp x11, #0
    mov x17, #1
    mov x16, #2
    csel x17, x17, x16, eq          // the mode that rounds this sign to inf
    cmp x9, x17
    b.eq 78f
    mov x9, #(((1 << \e) - 1) << \m)
    sub x9, x9, #1
    orr x10, x5, x9                 // the largest finite
    b 79f
78: orr x10, x5, #(((1 << \e) - 1) << \m)
79:
.endm
.macro vfu_class sew, u1=0, u2=0, u3=0
.if \sew == 4
    vf_class 23, 8
.else
    vf_class 52, 11
.endif
.endm
.macro vfu_rsqrt7 sew, u1=0, u2=0, u3=0
.if \sew == 4
    vf_rsqrt7 23, 8
.else
    vf_rsqrt7 52, 11
.endif
.endm
.macro vfu_rec7 sew, u1=0, u2=0, u3=0
.if \sew == 4
    vf_rec7 23, 8
.else
    vf_rec7 52, 11
.endif
.endm

// one element in, one out: [gadget][vtype][vd][vs2][-][insn][pc]; vs2's
// elements \ssz bytes (signed if \ssign), vd's \dsz; \vdg/\vs2g 2 for a
// group of twice LMUL
.macro vfe name, sew, masked, ssz, ssign, dsz, vdg, vs2g, opm, a1, a2=0, a3=0, a4=0
.gadget vfe_\name\()_\masked\()_e\sew
    vt_check
vfebody_\name\()_\masked\()_e\sew :
    vloop_head vv, \sew
    mov x6, #0
1:  vact \masked, 2f
    vel_ld \ssz, \ssign, 10, x14
    \opm \a1, \a2, \a3, \a4
    vel_st \dsz, 10, x13
2:  add x6, x6, #1
    cmp x6, x12
    b.lo 1b
    vloop_tail vfebody_\name\()_\masked, \vdg, \vs2g, 0
.endm
.macro vfe2 name, sew, ssz, ssign, dsz, vdg, vs2g, opm, a1, a2=0, a3=0, a4=0
    vfe \name, \sew, 0, \ssz, \ssign, \dsz, \vdg, \vs2g, \opm, \a1, \a2, \a3, \a4
    vfe \name, \sew, 1, \ssz, \ssign, \dsz, \vdg, \vs2g, \opm, \a1, \a2, \a3, \a4
.endm
.macro vferow name, x1, x2, x4, x8
    vrowsel vfebody_\name\()_0, \x1, \x2, \x4, \x8
    vrowsel vfebody_\name\()_1, \x1, \x2, \x4, \x8
.endm
// VFUNARY0, by vs1: 0 xu.f 1 x.f 2 f.xu 3 f.x 6 rtz.xu.f 7 rtz.x.f, the
// widening 8-15 and the narrowing 16-23
.irp sew, 4, 8
vfe2 cvt0, \sew, \sew, 0, \sew, 1, 1, vcv_f2i, \sew, \sew, 0, 0
vfe2 cvt1, \sew, \sew, 0, \sew, 1, 1, vcv_f2i, \sew, \sew, 1, 0
vfe2 cvt2, \sew, \sew, 0, \sew, 1, 1, vcv_i2f, \sew, \sew, 0
vfe2 cvt3, \sew, \sew, 1, \sew, 1, 1, vcv_i2f, \sew, \sew, 1
vfe2 cvt6, \sew, \sew, 0, \sew, 1, 1, vcv_f2i, \sew, \sew, 0, 1
vfe2 cvt7, \sew, \sew, 0, \sew, 1, 1, vcv_f2i, \sew, \sew, 1, 1
.endr
vfe2 cvt8, 4, 4, 0, 8, 2, 1, vcv_f2i, 4, 8, 0, 0
vfe2 cvt9, 4, 4, 0, 8, 2, 1, vcv_f2i, 4, 8, 1, 0
vfe2 cvt14, 4, 4, 0, 8, 2, 1, vcv_f2i, 4, 8, 0, 1
vfe2 cvt15, 4, 4, 0, 8, 2, 1, vcv_f2i, 4, 8, 1, 1
.irp sew, 2, 4
vfe2 cvt10, \sew, \sew, 0, (2 * \sew), 2, 1, vcv_i2f, \sew, (2 * \sew), 0
vfe2 cvt11, \sew, \sew, 1, (2 * \sew), 2, 1, vcv_i2f, \sew, (2 * \sew), 1
vfe2 cvt12, \sew, \sew, 0, (2 * \sew), 2, 1, vcv_f2f, \sew, (2 * \sew), 0
vfe2 cvt16, \sew, (2 * \sew), 0, \sew, 1, 2, vcv_f2i, (2 * \sew), \sew, 0, 0
vfe2 cvt17, \sew, (2 * \sew), 0, \sew, 1, 2, vcv_f2i, (2 * \sew), \sew, 1, 0
vfe2 cvt20, \sew, (2 * \sew), 0, \sew, 1, 2, vcv_f2f, (2 * \sew), \sew, 0
vfe2 cvt22, \sew, (2 * \sew), 0, \sew, 1, 2, vcv_f2i, (2 * \sew), \sew, 0, 1
vfe2 cvt23, \sew, (2 * \sew), 0, \sew, 1, 2, vcv_f2i, (2 * \sew), \sew, 1, 1
.endr
vfe2 cvt18, 4, 8, 0, 4, 1, 2, vcv_i2f, 8, 4, 0
vfe2 cvt19, 4, 8, 1, 4, 1, 2, vcv_i2f, 8, 4, 1
vfe2 cvt21, 4, 8, 0, 4, 1, 2, vcv_f2f, 8, 4, 1
.irp sew, 4, 8
vfe2 class, \sew, \sew, 0, \sew, 1, 1, vfu_class, \sew
vfe2 rsqrt7, \sew, \sew, 0, \sew, 1, 1, vfu_rsqrt7, \sew
vfe2 rec7, \sew, \sew, 0, \sew, 1, 1, vfu_rec7, \sew
.endr
.irp n, cvt0, cvt1, cvt2, cvt3, cvt6, cvt7, class, rsqrt7, rec7
    vferow \n, 0, 0, 1, 1
.endr
.irp n, cvt8, cvt9, cvt14, cvt15, cvt18, cvt19, cvt21
    vferow \n, 0, 0, 1, 0
.endr
.irp n, cvt10, cvt11, cvt12, cvt16, cvt17, cvt20, cvt22, cvt23
    vferow \n, 0, 1, 1, 0
.endr
rsqrt7_table:
    .byte 52, 51, 50, 48, 47, 46, 44, 43, 42, 41, 40, 39, 38, 36, 35, 34, 33, 32, 31, 30, 30, 29, 28, 27, 26, 25, 24, 23, 23, 22, 21, 20, 19, 19, 18, 17, 16, 16, 15, 14, 14, 13, 12, 12, 11, 10, 10, 9, 9, 8, 7, 7, 6, 6, 5, 4, 4, 3, 3, 2, 2, 1, 1, 0, 127, 125, 123, 121, 119, 118, 116, 114, 113, 111, 109, 108, 106, 105, 103, 102, 100, 99, 97, 96, 95, 93, 92, 91, 90, 88, 87, 86, 85, 84, 83, 82, 80, 79, 78, 77, 76, 75, 74, 73, 72, 71, 70, 70, 69, 68, 67, 66, 65, 64, 63, 63, 62, 61, 60, 59, 59, 58, 57, 56, 56, 55, 54, 53
rec7_table:
    .byte 127, 125, 123, 121, 119, 117, 116, 114, 112, 110, 109, 107, 105, 104, 102, 100, 99, 97, 96, 94, 93, 91, 90, 88, 87, 85, 84, 83, 81, 80, 79, 77, 76, 75, 74, 72, 71, 70, 69, 68, 66, 65, 64, 63, 62, 61, 60, 59, 58, 57, 56, 55, 54, 53, 52, 51, 50, 49, 48, 47, 46, 45, 44, 43, 42, 41, 40, 40, 39, 38, 37, 36, 35, 35, 34, 33, 32, 31, 31, 30, 29, 28, 28, 27, 26, 25, 25, 24, 23, 23, 22, 21, 21, 20, 19, 19, 18, 17, 17, 16, 15, 15, 14, 14, 13, 12, 12, 11, 11, 10, 9, 9, 8, 8, 7, 7, 6, 5, 5, 4, 4, 3, 3, 2, 2, 1, 1, 0

// FP reductions: vd[0] = vs1[0] op the active vs2[i] in order (the
// unordered sum is summed in order too); the widening ones (\wide) sum
// SEW 32 elements into a 64-bit vs1[0]. [gadget][vtype][vd][vs2][vs1][insn][pc]
.macro vfs_minmax insn, r
    \insn \r\()18, \r\()16, \r\()17
    fcmp \r\()16, \r\()16
    b.vc 61f
    fcmp \r\()17, \r\()17
    b.vs 62f
    fmov \r\()18, \r\()17
    b 62f
61: fcmp \r\()17, \r\()17
    b.vc 62f
    fmov \r\()18, \r\()16
62: fmov \r\()16, \r\()18
.endm
.macro vfr name, sew, masked, wide, opm
.gadget vfr_\name\()_\masked\()_e\sew
    vt_check
vfrbody_\name\()_\masked\()_e\sew :
    vloop_head vv, \sew
.if \sew == 8 || \wide
    ldr d16, [x15]
.else
    ldr s16, [x15]
.endif
    mov x6, #0
1:  vact \masked, 2f
.if \sew == 4
    ldr s17, [x14, x6, lsl #2]
.if \wide
    fcvt d17, s17
.endif
.else
    ldr d17, [x14, x6, lsl #3]
.endif
.if \sew == 8 || \wide
    \opm d
.else
    \opm s
.endif
2:  add x6, x6, #1
    cmp x6, x12
    b.lo 1b
.if \sew == 8 || \wide
    str d16, [x13]
.else
    str s16, [x13]
.endif
    vloop_tail vfrbody_\name\()_\masked, 0, 1, 0
.endm
.macro vfro_sum r
    fadd \r\()16, \r\()16, \r\()17
.endm
.macro vfro_min r
    vfs_minmax fminnm, \r
.endm
.macro vfro_max r
    vfs_minmax fmaxnm, \r
.endm
.irp masked, 0, 1
.irp sew, 4, 8
    vfr sum, \sew, \masked, 0, vfro_sum
    vfr min, \sew, \masked, 0, vfro_min
    vfr max, \sew, \masked, 0, vfro_max
.endr
    vfr wsum, 4, \masked, 1, vfro_sum
    vrowsel vfrbody_sum_\masked, 0, 0, 1, 1
    vrowsel vfrbody_min_\masked, 0, 0, 1, 1
    vrowsel vfrbody_max_\masked, 0, 0, 1, 1
    vrowsel vfrbody_wsum_\masked, 0, 0, 1, 0
.endr

// widening FP arithmetic at SEW 32: vd (64-bit) = vs2 (32, or 64 if \aw)
// op vs1 or f (32). [gadget][vtype][vd][vs2][vs1 | f][insn][pc]
.macro vfwo_add
    fadd d16, d16, d18
.endm
.macro vfwo_sub
    fsub d16, d16, d18
.endm
.macro vfwo_mul
    fmul d16, d16, d18
.endm
.macro vfwo_macc                   // vs1 * vs2 + vd
    fmadd d16, d18, d16, d19
.endm
.macro vfwo_nmacc                  // -(vs1 * vs2) - vd
    fnmadd d16, d18, d16, d19
.endm
.macro vfwo_msac                   // vs1 * vs2 - vd
    fnmsub d16, d18, d16, d19
.endm
.macro vfwo_nmsac                  // -(vs1 * vs2) + vd
    fmsub d16, d18, d16, d19
.endm
.macro vfw name, kind, masked, aw, opm, vd_in
.gadget vfw_\name\()_\kind\()_\masked\()_e4
    vt_check
vfwbody_\name\()_\kind\()_\masked\()_e4 :
    vloop_head \kind, 4
.ifc \kind, vf
    fmov s18, w15
    fcvt d18, s18
.endif
    mov x6, #0
1:  vact \masked, 2f
.if \aw
    ldr d16, [x14, x6, lsl #3]
.else
    ldr s16, [x14, x6, lsl #2]
    fcvt d16, s16
.endif
.ifc \kind, vv
    ldr s18, [x15, x6, lsl #2]
    fcvt d18, s18
.endif
.if \vd_in
    ldr d19, [x13, x6, lsl #3]
.endif
    \opm
    str d16, [x13, x6, lsl #3]
2:  add x6, x6, #1
    cmp x6, x12
    b.lo 1b
.ifc \kind, vv
    vloop_tail vfwbody_\name\()_\kind\()_\masked, 2, (1 + \aw), 1
.else
    vloop_tail vfwbody_\name\()_\kind\()_\masked, 2, (1 + \aw), 0
.endif
.endm
.macro vfwrow name, kind, masked
    vrowsel vfwbody_\name\()_\kind\()_\masked, 0, 0, 1, 0
.endm
.macro vfw_all name, aw, opm, vd_in
.irp kind, vv, vf
    vfw \name, \kind, 0, \aw, \opm, \vd_in
    vfw \name, \kind, 1, \aw, \opm, \vd_in
    vfwrow \name, \kind, 0
    vfwrow \name, \kind, 1
.endr
.endm
vfw_all add, 0, vfwo_add, 0
vfw_all sub, 0, vfwo_sub, 0
vfw_all addw, 1, vfwo_add, 0
vfw_all subw, 1, vfwo_sub, 0
vfw_all mul, 0, vfwo_mul, 0
vfw_all macc, 0, vfwo_macc, 1
vfw_all nmacc, 0, vfwo_nmacc, 1
vfw_all msac, 0, vfwo_msac, 1
vfw_all nmsac, 0, vfwo_nmsac, 1

// ---- vector memory: every load and store form, element by element ----
// vmem_any decodes the instruction at run time: unit-stride (plain,
// fault-only-first, whole-register, mask), strided and indexed, segments
// (nf fields), masked or not, from vstart. Each access goes through the TLB
// as the scalar gadgets' do (tlb_handle_miss on a miss, tlb_write_ptr_slow
// when the mmu wants writes revalidated); an element across a page is done
// a byte at a time. A fault stops at its element with vstart there --
// except a fault-only-first load past element 0, which ends with vl there.
// Entered by branch with w8 = the encoding, x16 = the byte offset in the
// stream of its pc, x17 = the stream words to pop.

// x7 = guest address -> host address, or 0 if it faults (vmx_read for a
// load, vmx_write for a store). Clobbers x9-x11, x27.
vmx_read:
    and x9, x7, #0xfffffffffffff000
    ubfx x10, x7, #12, #10
    eor x10, x10, x7, lsr #22
    and x10, x10, #(1 << TLB_BITS) - 1
    mov w11, #TLB_ENTRY_SIZE
    madd x10, x10, x11, _tlb
    ldr x11, [x10, #TLB_ENTRY_page]
    cmp x9, x11
    b.ne 1f
    ldr x11, [x10, #TLB_ENTRY_data_minus_addr]
    add x7, x11, x7
    ret
1:  save_c
    sub x0, _tlb, #TLB_entries     // x2 (_tlb) read before x2 is written
    mov x1, x7
    mov x2, #MEM_READ
    bl NAME(tlb_handle_miss)
    mov x27, x0
    restore_c
    mov x7, x27
    ret
vmx_write:
    ldr x10, [_tlb, #(-TLB_entries+TLB_mmu)]
    ldr x9, [_tlb, #(-TLB_entries+TLB_mem_changes)]
    ldr x11, [x10, #MMU_changes]
    cmp x9, x11
    b.ne 2f
    ldrb w10, [x10, #MMU_requires_write_revalidate]
    cbnz w10, 2f
    and x9, x7, #0xfffffffffffff000
    str x9, [_tlb, #(-TLB_entries+TLB_dirty_page)]
    ubfx x10, x7, #12, #10
    eor x10, x10, x7, lsr #22
    and x10, x10, #(1 << TLB_BITS) - 1
    mov w11, #TLB_ENTRY_SIZE
    madd x10, x10, x11, _tlb
    ldr x11, [x10, #TLB_ENTRY_page_if_writable]
    cmp x9, x11
    b.ne 1f
    ldr x11, [x10, #TLB_ENTRY_data_minus_addr]
    add x7, x11, x7
    ret
1:  save_c
    sub x0, _tlb, #TLB_entries
    mov x1, x7
    mov x2, #MEM_WRITE
    bl NAME(tlb_handle_miss)
    mov x27, x0
    restore_c
    mov x7, x27
    ret
2:  save_c
    sub x0, _tlb, #TLB_entries
    mov x1, x7
    bl NAME(tlb_write_ptr_slow)
    mov x27, x0
    restore_c
    mov x7, x27
    ret

// one element of x16 bytes between guest x24 and x26 (a load reads into
// x26, a store writes it); a fault goes to vmx_fault. Uses x7, x9-x11, x25.
vmx_load_el:
    str lr, [sp, #-16]!
    and x10, x24, #0xfff
    add x10, x10, x16
    cmp x10, #0x1000
    b.hi 3f
    mov x7, x24
    bl vmx_read
    cbz x7, 9f
    tbnz x16, #3, 8f
    tbnz x16, #2, 4f
    tbnz x16, #1, 2f
    ldrb w26, [x7]
    b 0f
2:  ldrh w26, [x7]
    b 0f
4:  ldr w26, [x7]
    b 0f
8:  ldr x26, [x7]
    b 0f
3:  mov x26, #0                    // across a page: a byte at a time
    mov x25, #0
5:  add x7, x24, x25
    bl vmx_read
    cbz x7, 9f
    ldrb w9, [x7]
    lsl x10, x25, #3
    lsl x9, x9, x10
    orr x26, x26, x9
    add x25, x25, #1
    cmp x25, x16
    b.lo 5b
0:  ldr lr, [sp], #16
    ret
9:  ldr lr, [sp], #16
    b vmx_fault
vmx_store_el:
    str lr, [sp, #-16]!
    and x10, x24, #0xfff
    add x10, x10, x16
    cmp x10, #0x1000
    b.hi 3f
    mov x7, x24
    bl vmx_write
    cbz x7, 9f
    tbnz x16, #3, 8f
    tbnz x16, #2, 4f
    tbnz x16, #1, 2f
    strb w26, [x7]
    b 0f
2:  strh w26, [x7]
    b 0f
4:  str w26, [x7]
    b 0f
8:  str x26, [x7]
    b 0f
3:  mov x25, #0
5:  add x7, x24, x25
    bl vmx_write
    cbz x7, 9f
    lsl x10, x25, #3
    lsr x9, x26, x10
    strb w9, [x7]
    add x25, x25, #1
    cmp x25, x16
    b.lo 5b
0:  ldr lr, [sp], #16
    ret
9:  ldr lr, [sp], #16
    b vmx_fault

// register-file element access: x23 = its address, x16 its size
.macro vmx_reg_ld
    tbnz x16, #3, 8f
    tbnz x16, #2, 4f
    tbnz x16, #1, 2f
    ldrb w26, [x23]
    b 0f
2:  ldrh w26, [x23]
    b 0f
4:  ldr w26, [x23]
    b 0f
8:  ldr x26, [x23]
0:
.endm
.macro vmx_reg_st
    tbnz x16, #3, 8f
    tbnz x16, #2, 4f
    tbnz x16, #1, 2f
    strb w26, [x23]
    b 0f
2:  strh w26, [x23]
    b 0f
4:  str w26, [x23]
    b 0f
8:  str x26, [x23]
0:
.endm

// x19-x26 are callee-saved state other gadgets keep; saved around the body
// with the entry's x8/x16/x17: [sp] x8 x16 x17 - x19 ... x26
vmem_any:
    stp x8, x16, [sp, #-96]!
    str x17, [sp, #16]
    stp x19, x20, [sp, #32]
    stp x21, x22, [sp, #48]
    stp x23, x24, [sp, #64]
    stp x25, x26, [sp, #80]
    // decode: x0 nf, x16 eew, x19 index eew, x20 mode (0 unit, 1 indexed,
    // 2 strided), x21 flags (1 store, 2 masked, 4 ff, 8 whole, 16 mask),
    // x12 the element count, x13 vd's address, x14 the base, x15 the stride
    // or the index group's address, x17 a field's spacing in bytes
    ubfx x9, x8, #12, #3           // width: 0 5 6 7 = 1 2 4 8 bytes
    cbz x9, 1f
    sub x9, x9, #4                 // 1 2 3
    mov x10, #1
    lsl x9, x10, x9                // 2 4 8
    b 2f
1:  mov x9, #1
2:  tbnz x8, #28, vmx_ill          // mew: EEW 128 and up
    ubfx x20, x8, #26, #2
    mov x10, #1
    cmp x20, #3                    // indexed-ordered runs as unordered
    csel x20, x10, x20, eq
    mov x21, #0
    and x10, x8, #0x7f
    cmp x10, #0x27
    cinc x21, x21, eq              // store
    tbnz x8, #25, 3f
    orr x21, x21, #2               // masked
3:  ubfx x0, x8, #29, #3
    add x0, x0, #1                 // nf
    ubfx x10, x8, #20, #5          // lumop / sumop
    mov x19, #0
    cmp x20, #1
    b.ne 4f
    mov x19, x9                    // indexed: the width is the index EEW;
    ldr x10, [_cpu, #CPU_riscv64_vtype]
    tbnz x10, #63, vmx_ill
    ubfx x10, x10, #3, #3          // the data EEW is SEW
    mov x9, #1
    lsl x9, x9, x10
    b 6f
4:  cbnz x20, 6f                   // unit-stride: lumop
    cmp x10, #0x10
    b.ne 5f
    tbnz x21, #0, vmx_ill          // fault-only-first: loads only
    orr x21, x21, #4
    b 6f
5:  cmp x10, #8
    b.ne 7f
    orr x21, x21, #8               // whole register
    tbnz x21, #1, vmx_ill
    b 6f
7:  cmp x10, #0xb
    b.ne 8f
    orr x21, x21, #16              // mask
    tbnz x21, #1, vmx_ill
    cmp x0, #1
    b.ne vmx_ill
    cmp x9, #1
    b.ne vmx_ill
    b 6f
8:  cbnz x10, vmx_ill
6:  mov x16, x9
    // the element count and the groups
    ubfx x10, x8, #7, #5           // vd
    tbnz x21, #3, 20f              // whole register: nf registers, no vtype
    ldr x11, [_cpu, #CPU_riscv64_vtype]
    tbnz x11, #63, vmx_ill
    ldr x12, [_cpu, #CPU_riscv64_vl]
    tbz x21, #4, 21f
    add x12, x12, #7               // mask: ceil(vl / 8) bytes
    lsr x12, x12, #3
    mov x17, #16
    b 24f
21: and x9, x11, #7                // lmul8 = 8 << vlmul, or 8 >> (8 - vlmul)
    mov x22, #8
    lsl x23, x22, x9
    mov x24, #8
    sub x24, x24, x9
    lsr x24, x22, x24
    cmp x9, #4
    csel x22, x23, x24, lo         // lmul8
    ubfx x11, x11, #3, #3
    mov x23, #1
    lsl x11, x23, x11              // SEW bytes
    cmp x20, #1
    b.ne 22f
    // indexed: data groups of LMUL; the index group EMUL = LMUL * ieew / SEW
    mul x23, x22, x19
    udiv x23, x23, x11             // index emul8
    cbz x23, vmx_ill
    cmp x23, #64
    b.hi vmx_ill
    lsr x24, x23, #3
    cmp x24, #1
    csinc x24, x24, xzr, hs        // index registers
    sub x24, x24, #1
    ubfx x9, x8, #20, #5
    tst x9, x24
    b.ne vmx_ill
    mov x23, x22                   // data emul8 = lmul8
    b 23f
22: mul x23, x22, x16              // data EMUL = LMUL * EEW / SEW
    udiv x23, x23, x11
    cbz x23, vmx_ill
    cmp x23, #64
    b.hi vmx_ill
23: lsr x24, x23, #3
    cmp x24, #1
    csinc x24, x24, xzr, hs        // registers per field
    mul x9, x24, x0
    cmp x9, #8
    b.hi vmx_ill
    add x9, x9, x10
    cmp x9, #32
    b.hi vmx_ill
    sub x9, x24, #1
    tst x10, x9
    b.ne vmx_ill
    lsl x17, x24, #4               // a field's spacing
24: b 25f
20: cmp x0, #1                     // whole register: nf of 1 2 4 8
    b.eq 26f
    cmp x0, #2
    b.eq 26f
    cmp x0, #4
    b.eq 26f
    cmp x0, #8
    b.ne vmx_ill
26: sub x9, x0, #1
    tst x10, x9
    b.ne vmx_ill
    lsl x12, x0, #4                // nf * 16 bytes / EEW elements
    udiv x12, x12, x16
    mov x0, #1                     // one field of nf registers
    mov x17, #16
25: tst x21, #2                    // a masked load into v0 is reserved
    b.eq 27f
    tst x21, #1
    b.ne 27f
    cbz x10, vmx_ill
27: add x13, _cpu, #CPU_riscv64_v
    add x13, x13, x10, lsl #4
    ubfx x9, x8, #15, #5
    add x9, x9, #(CPU_riscv64_regs / 8)
    ldr x14, [_cpu, x9, lsl #3]    // the base, x[rs1]
    ubfx x9, x8, #20, #5
    cmp x20, #2
    b.ne 28f
    add x9, x9, #(CPU_riscv64_regs / 8)
    ldr x15, [_cpu, x9, lsl #3]    // the stride, x[rs2]
    b 29f
28: add x15, _cpu, #CPU_riscv64_v
    add x15, x15, x9, lsl #4       // the index group
29: ldr x3, [_cpu, #CPU_riscv64_v]
    ldr x4, [_cpu, #(CPU_riscv64_v + 8)]
    ldr x6, [_cpu, #CPU_riscv64_vstart]
    // the elements
30: cmp x6, x12
    b.hs 39f
    tbz x21, #1, 31f
    cmp x6, #64
    csel x9, x3, x4, lo
    lsr x9, x9, x6
    tbz x9, #0, 38f
31: cmp x20, #1
    b.eq 32f
    b.hi 33f
    mul x22, x6, x0                // unit: base + (i * nf) * EEW
    mul x22, x22, x16
    add x22, x22, x14
    b 34f
32: mul x9, x6, x19                // indexed: base + index[i]
    add x9, x15, x9
    tbnz x19, #3, 35f
    tbnz x19, #2, 36f
    tbnz x19, #1, 37f
    ldrb w22, [x9]
    b 40f
37: ldrh w22, [x9]
    b 40f
36: ldr w22, [x9]
    b 40f
35: ldr x22, [x9]
40: add x22, x22, x14
    b 34f
33: mul x22, x6, x15               // strided: base + i * stride
    add x22, x22, x14
34: mov x5, #0
41: mul x9, x5, x16
    add x24, x22, x9               // field f's address
    mul x9, x5, x17
    add x23, x13, x9
    mul x9, x6, x16
    add x23, x23, x9               // its register-file element
    tbnz x21, #0, 42f
    bl vmx_load_el
    vmx_reg_st
    b 43f
42: vmx_reg_ld
    bl vmx_store_el
43: add x5, x5, #1
    cmp x5, x0
    b.lo 41b
38: add x6, x6, #1
    b 30b
39: str xzr, [_cpu, #CPU_riscv64_vstart]
    ldp x19, x20, [sp, #32]
    ldp x21, x22, [sp, #48]
    ldp x23, x24, [sp, #64]
    ldp x25, x26, [sp, #80]
    ldr x17, [sp, #16]
    add sp, sp, #96
    add _ip, _ip, x17, lsl #3
    gret
// a fault at element x6
vmx_fault:
    tbz x21, #2, 1f
    cbz x6, 1f
    str x6, [_cpu, #CPU_riscv64_vl] // fault-only-first past element 0: vl ends there
    mov x6, x12
    b 39b
1:  str x6, [_cpu, #CPU_riscv64_vstart]
    ldr x8, [_tlb, #(-TLB_entries+TLB_segfault_addr)]
    str x8, [_cpu, #CPU_segfault_addr]
    and w9, w21, #1
    strb w9, [_cpu, #CPU_segfault_was_write]
    mov w0, #INT_PF
    b vmx_exit
vmx_ill:
    mov w0, #INT_UNDEFINED
vmx_exit:
    ldr x16, [sp, #8]
    ldr x9, [_ip, x16]
    str x9, [_cpu, #CPU_riscv64_pc]
    ldp x19, x20, [sp, #32]
    ldp x21, x22, [sp, #48]
    ldp x23, x24, [sp, #64]
    ldp x25, x26, [sp, #80]
    add sp, sp, #96
    b jit_exit

// [gadget][insn][pc]: any vector load or store
.gadget vmem
    ldr w8, [_ip]
    mov x16, #8
    mov x17, #2
    b vmem_any

// ---- fixed point: element by element, rounding by vxrm, vxsat sticky ----
// x10 = vs2[i] and x11 = vs1[i] or the scalar or immediate, extended as
// each op needs; the result in x10. x17 = vxrm; x16 = 1 once something
// saturated (vxsat is set after the loop). Op labels are 60-69.

// x10 >>= x9 (\sign: arithmetic), rounded as vxrm (x17) says: rnu adds the
// bit below the result; rne adds it when a lower bit or the result's bit 0
// is set; rdn adds nothing; rod sets bit 0 if anything was dropped. Uses
// x7, x8, x11.
.macro vround sign
    cbz x9, 69f
    sub x7, x9, #1
    lsr x8, x10, x7
    and x8, x8, #1
    mov x11, #1
    lsl x11, x11, x7
    sub x11, x11, #1
    and x11, x10, x11
.if \sign
    asr x10, x10, x9
.else
    lsr x10, x10, x9
.endif
    cbz x17, 66f
    cmp x17, #2
    b.eq 69f
    b.hi 67f
    and x7, x10, #1
    cmp x11, #0
    csinc x7, x7, xzr, eq
    and x8, x8, x7
66: add x10, x10, x8
    b 69f
67: orr x11, x11, x8
    cmp x11, #0
    cset x7, ne
    orr x10, x10, x7
69:
.endm
// the same for a 65-bit value \hi:x10 shifted by 1
.macro vround65 hi
    and x8, x10, #1
    ubfx x7, x10, #1, #1
    lsr x10, x10, #1
    orr x10, x10, \hi, lsl #63
    cbz x17, 66f
    cmp x17, #2
    b.eq 69f
    b.hi 67f
    and x8, x8, x7
66: add x10, x10, x8
    b 69f
67: orr x10, x10, x8
69:
.endm
// clamp x10 to SEW \sew bytes, signed or not, noting a saturation
.macro vclamp sew, sign
.if \sign
    mov x9, #((1 << (\sew * 8 - 1)) - 1)
    cmp x10, x9
    b.le 62f
    mov x10, x9
    mov x16, #1
    b 63f
62: mvn x9, x9
    cmp x10, x9
    b.ge 63f
    mov x10, x9
    mov x16, #1
63:
.else
    mov x9, #((1 << (\sew * 8)) - 1)
    cmp x10, x9
    b.ls 63f
    mov x10, x9
    mov x16, #1
63:
.endif
.endm

.macro vx_saddu sew
.if \sew == 8
    adds x10, x10, x11
    b.cc 61f
    mov x10, #-1
    mov x16, #1
61:
.else
    add x10, x10, x11
    vclamp \sew, 0
.endif
.endm
.macro vx_sadd sew
.if \sew == 8
    adds x10, x10, x11
    b.vc 61f
    asr x9, x11, #63               // both had this sign: saturate that way
    eor x10, x9, #0x7fffffffffffffff
    mov x16, #1
61:
.else
    add x10, x10, x11
    vclamp \sew, 1
.endif
.endm
.macro vx_ssubu sew
    subs x10, x10, x11
    b.cs 61f
    mov x10, #0
    mov x16, #1
61:
.endm
.macro vx_ssub sew
.if \sew == 8
    subs x10, x10, x11
    b.vc 61f
    asr x9, x11, #63               // vs2 had the other sign: saturate its way
    eor x10, x9, #0x8000000000000000
    mov x16, #1
61:
.else
    sub x10, x10, x11
    vclamp \sew, 1
.endif
.endm
.macro vx_aaddu sew
.if \sew == 8
    adds x10, x10, x11
    cset x9, cs
    vround65 x9
.else
    add x10, x10, x11
    mov x9, #1
    vround 1
.endif
.endm
.macro vx_aadd sew
.if \sew == 8
    adds x10, x10, x11
    cset x9, mi
    cset x7, vs
    eor x9, x9, x7                 // the 65-bit sum's sign
    vround65 x9
.else
    add x10, x10, x11
    mov x9, #1
    vround 1
.endif
.endm
.macro vx_asubu sew
.if \sew == 8
    subs x10, x10, x11
    cset x9, cc                    // the borrow: bit 64 of the difference
    vround65 x9
.else
    sub x10, x10, x11
    mov x9, #1
    vround 1
.endif
.endm
.macro vx_asub sew
.if \sew == 8
    subs x10, x10, x11
    cset x9, mi
    cset x7, vs
    eor x9, x9, x7
    vround65 x9
.else
    sub x10, x10, x11
    mov x9, #1
    vround 1
.endif
.endm
// vsmul: (vs2 * vs1) >> (SEW - 1), rounded; MIN * MIN saturates to MAX
.macro vx_smul sew
.if \sew == 8
    mov x9, #0x8000000000000000
    cmp x10, x9
    ccmp x11, x9, #0, eq
    b.ne 60f
    mvn x10, x9
    mov x16, #1
    b 69f
60: mul x5, x10, x11
    smulh x9, x10, x11
    ubfx x8, x5, #62, #1           // the bit below the result
    and x11, x5, #0x3fffffffffffffff
    lsr x10, x5, #63
    orr x10, x10, x9, lsl #1
    cbz x17, 66f
    cmp x17, #2
    b.eq 69f
    b.hi 67f
    and x7, x10, #1
    cmp x11, #0
    csinc x7, x7, xzr, eq
    and x8, x8, x7
66: add x10, x10, x8
    b 69f
67: orr x11, x11, x8
    cmp x11, #0
    cset x7, ne
    orr x10, x10, x7
69:
.else
    mul x10, x10, x11
    mov x9, #(\sew * 8 - 1)
    vround 1
    vclamp \sew, 1
.endif
.endm
.macro vx_ssrl sew
    and x9, x11, #(\sew * 8 - 1)
    vround 0
.endm
.macro vx_ssra sew
    and x9, x11, #(\sew * 8 - 1)
    vround 1
.endm
.macro vx_nclipu sew               // vs2 2*SEW wide
    and x9, x11, #(\sew * 16 - 1)
    vround 0
    vclamp \sew, 0
.endm
.macro vx_nclip sew
    and x9, x11, #(\sew * 16 - 1)
    vround 1
    vclamp \sew, 1
.endm

// [gadget][vtype][vd][vs2][vs1 | rs1 | imm][insn][pc]; \aw: vs2 is 2*SEW
.macro vfx name, kind, sew, masked, aw, asign, bsign
.gadget vfx_\name\()_\kind\()_\masked\()_e\sew
    vt_check
vfxbody_\name\()_\kind\()_\masked\()_e\sew :
    ldr x12, [_cpu, #CPU_riscv64_vl]
    cbz x12, 8f
    ldp x13, x14, [_ip, #8]
    ldr x15, [_ip, #24]
    add x13, _cpu, x13
    add x14, _cpu, x14
.ifc \kind, vv
    add x15, _cpu, x15
.endif
.ifc \kind, vx
    ldr x15, [_cpu, x15]
    vel_ext \sew, \bsign, 15
.endif
    ldr w17, [_cpu, #CPU_riscv64_vxrm]
    mov x16, #0
    ldr x3, [_cpu, #CPU_riscv64_v]
    ldr x4, [_cpu, #(CPU_riscv64_v + 8)]
    mov x6, #0
1:  vact \masked, 2f
    vel_ld (\sew << \aw), \asign, 10, x14
.ifc \kind, vv
    vel_ld \sew, \bsign, 11, x15
.else
    mov x11, x15
.endif
    vx_\name \sew
    vel_st \sew, 10, x13
2:  add x6, x6, #1
    cmp x6, x12
    b.lo 1b
    cbz x16, 8f
    mov w9, #1
    str w9, [_cpu, #CPU_riscv64_vxsat]
8:  gret 6
.ifc \kind, vv
9:  vretype vfxbody_\name\()_\kind\()_\masked, 1, (1 + \aw), 1, 40
.else
9:  vretype vfxbody_\name\()_\kind\()_\masked, 1, (1 + \aw), 0, 40
.endif
5:  vtrap 40
.endm
.macro vfx_all name, aw, asign, bsign
.irp kind, vv, vx, vi
    vfx_k \name, \kind, \aw, \asign, \bsign
.endr
.endm
.macro vfx_k name, kind, aw, asign, bsign
.irp masked, 0, 1
    vfx \name, \kind, 1, \masked, \aw, \asign, \bsign
    vfx \name, \kind, 2, \masked, \aw, \asign, \bsign
    vfx \name, \kind, 4, \masked, \aw, \asign, \bsign
.if \aw == 0
    vfx \name, \kind, 8, \masked, \aw, \asign, \bsign
.endif
.endr
.if \aw
    vrowsel vfxbody_\name\()_\kind\()_0, 1, 1, 1, 0
    vrowsel vfxbody_\name\()_\kind\()_1, 1, 1, 1, 0
.else
    vrow vfxbody_\name\()_\kind\()_0
    vrow vfxbody_\name\()_\kind\()_1
.endif
.endm
vfx_all saddu, 0, 0, 0
vfx_all sadd, 0, 1, 1
vfx_all ssubu, 0, 0, 0
vfx_all ssub, 0, 1, 1
vfx_all aaddu, 0, 0, 0
vfx_all aadd, 0, 1, 1
vfx_all asubu, 0, 0, 0
vfx_all asub, 0, 1, 1
vfx_all smul, 0, 1, 1
vfx_all ssrl, 0, 0, 0
vfx_all ssra, 0, 1, 0
vfx_all nclipu, 1, 0, 0
vfx_all nclip, 1, 1, 0

// vwredsumu / vwredsum: vd[0] (2*SEW) = vs1[0] + the active vs2[i], each
// extended. [gadget][vtype][vd][vs2][vs1][insn][pc]
.macro vwred name, sew, masked, sign
.gadget vwr_\name\()_\masked\()_e\sew
    vt_check
vwrbody_\name\()_\masked\()_e\sew :
    ldr x12, [_cpu, #CPU_riscv64_vl]
    cbz x12, 8f
    ldp x13, x14, [_ip, #8]
    ldr x15, [_ip, #24]
    add x13, _cpu, x13
    add x14, _cpu, x14
    add x15, _cpu, x15
    ldr x3, [_cpu, #CPU_riscv64_v]
    ldr x4, [_cpu, #(CPU_riscv64_v + 8)]
    mov x6, #0
    vel_ld (2 * \sew), 0, 16, x15
1:  vact \masked, 2f
    vel_ld \sew, \sign, 9, x14
    add x16, x16, x9
2:  add x6, x6, #1
    cmp x6, x12
    b.lo 1b
    mov x6, #0
    vel_st (2 * \sew), 16, x13
8:  gret 6
9:  vretype vwrbody_\name\()_\masked, 0, 1, 0, 40
5:  vtrap 40
.endm
.irp masked, 0, 1
.irp sew, 1, 2, 4
    vwred sumu, \sew, \masked, 0
    vwred sum, \sew, \masked, 1
.endr
    vrowsel vwrbody_sumu_\masked, 1, 1, 1, 0
    vrowsel vwrbody_sum_\masked, 1, 1, 1, 0
.endr

// compares: v0 lanes all ones where vs2 op src holds
.macro vc_seq T
    cmeq v0.\T, v0.\T, v1.\T
.endm
.macro vc_sne T
    cmeq v0.\T, v0.\T, v1.\T
    mvn v0.16b, v0.16b
.endm
.macro vc_sltu T
    cmhi v0.\T, v1.\T, v0.\T
.endm
.macro vc_slt T
    cmgt v0.\T, v1.\T, v0.\T
.endm
.macro vc_sleu T
    cmhs v0.\T, v1.\T, v0.\T
.endm
.macro vc_sle T
    cmge v0.\T, v1.\T, v0.\T
.endm
.macro vc_sgtu T
    cmhi v0.\T, v0.\T, v1.\T
.endm
.macro vc_sgt T
    cmgt v0.\T, v0.\T, v1.\T
.endm

// vd's low vl bits = vs2 op src, one per element (the rest left alone):
// [gadget][vtype][vd_off][vs2_off][vs1_off|rs1_off|imm][insn][pc]. Every
// source chunk is read before vd is written, so vd may be one of them (or
// v0, for the masked forms, vcmpm_*).
.macro vcmp op, kind, sew, T, lg, r, masked
.if \masked
.gadget vcmpm_\op\()_\kind\()_e\sew
.else
.gadget vcmp_\op\()_\kind\()_e\sew
.endif
    vt_check
vcbody_\op\()_\kind\()_\masked\()_e\sew :
    ldr x12, [_cpu, #CPU_riscv64_vl]
    cbz x12, 8f
    ldp x13, x14, [_ip, #8]
    ldr x15, [_ip, #24]
    add x14, _cpu, x14
    vsrc_setup \kind, \T, \r
    lsl x6, x12, #\lg
    mov x3, #0
    mov x4, #0
    mov x5, #0
    adr x9, vmask_weights
    ldr q6, [x9]
1:  ldr q0, [x14], #16
.ifc \kind, vv
    ldr q1, [x15], #16
.endif
    vc_\op \T
.if \sew == 1
    and v0.16b, v0.16b, v6.16b
    addv b2, v0.8b
    ext v3.16b, v0.16b, v0.16b, #8
    addv b3, v3.8b
    umov w9, v2.b[0]
    umov w10, v3.b[0]
    orr w9, w9, w10, lsl #8
.else
.if \sew == 8
    xtn v0.2s, v0.2d
.endif
.if \sew >= 4
    xtn v0.4h, v0.4s
.endif
    xtn v0.8b, v0.8h
    and v0.8b, v0.8b, v6.8b
    addv b2, v0.8b
    umov w9, v2.b[0]
.endif
    lsl x10, x9, x5
    cmp x5, #64
    b.hs 3f
    orr x3, x3, x10
    b 4f
3:  orr x4, x4, x10
4:  add x5, x5, #(16 / \sew)
    subs x6, x6, #16
    b.gt 1b
    vlmask x9, x10, x12, x11
.if \masked                     // masked-off bits are left alone too
    ldr x11, [_cpu, #CPU_riscv64_v]
    and x9, x9, x11
    ldr x11, [_cpu, #(CPU_riscv64_v + 8)]
    and x10, x10, x11
.endif
    add x13, _cpu, x13
    vmask_store x13, x3, x4, x9, x10
8:  gret 6
.ifc \kind, vv
9:  vretype vcbody_\op\()_\kind\()_\masked, 0, 1, 1, 40
.else
9:  vretype vcbody_\op\()_\kind\()_\masked, 0, 1, 0, 40
.endif
5:  vtrap 40
.endm
.macro vcmp_sews op, kind, masked
    vcmp \op, \kind, 1, 16b, 0, w, \masked
    vcmp \op, \kind, 2, 8h, 1, w, \masked
    vcmp \op, \kind, 4, 4s, 2, w, \masked
    vcmp \op, \kind, 8, 2d, 3, x, \masked
    vrow vcbody_\op\()_\kind\()_\masked
.endm
.irp op, seq, sne, sltu, slt, sleu, sle, sgtu, sgt
.irp kind, vv, vx, vi
    vcmp_sews \op, \kind, 0
    vcmp_sews \op, \kind, 1
.endr
.endr

// vill clear and vstart 0, for the gadgets that do not depend on SEW
.macro vm_check
    ldr x10, [_cpu, #CPU_riscv64_vtype]
    tbnz x10, #63, 9f
    ldr x11, [_cpu, #CPU_riscv64_vstart]
    cbnz x11, 9f
.endm

// mask-register logical ops on the low vl bits:
// [gadget][vd_off][vs2_off][vs1_off][insn][pc]
.macro vmlogic name, i0, i1=nop
.gadget vm_\name
    vm_check
    ldr x12, [_cpu, #CPU_riscv64_vl]
    cbz x12, 8f
    ldp x13, x14, [_ip]
    ldr x15, [_ip, #16]
    add x14, _cpu, x14
    add x15, _cpu, x15
    ldp x3, x4, [x14]
    ldp x5, x6, [x15]
    \i0 x3, x3, x5
    \i0 x4, x4, x6
.ifnc \i1, nop
    \i1 x3, x3
    \i1 x4, x4
.endif
    vlmask x9, x10, x12, x11
    add x13, _cpu, x13
    vmask_store x13, x3, x4, x9, x10
8:  gret 5
9:  vtrap 32
.endm
vmlogic andn, bic
vmlogic and, and
vmlogic or, orr
vmlogic xor, eor
vmlogic orn, orn
vmlogic nand, and, mvn
vmlogic nor, orr, mvn
vmlogic xnor, eon

// vmsbf.m / vmsif.m / vmsof.m from the lowest set bit of vs2's low vl:
// [gadget][vd_off][vs2_off][insn][pc]
.macro vmsfirst name, kind, masked=0
.gadget \name
    vm_check
    ldr x12, [_cpu, #CPU_riscv64_vl]
    cbz x12, 8f
    ldp x13, x14, [_ip]
    add x14, _cpu, x14
    ldp x3, x4, [x14]
    vlmask x9, x10, x12, x11
.if \masked                       // only active elements, and only they change
    ldr x5, [_cpu, #CPU_riscv64_v]
    ldr x6, [_cpu, #(CPU_riscv64_v + 8)]
    and x9, x9, x5
    and x10, x10, x6
.endif
    and x3, x3, x9
    and x4, x4, x10
    negs x5, x3                // x5:x6 = the lowest set bit (0 if none)
    ngc x6, x4
    and x5, x5, x3
    and x6, x6, x4
.if \kind == 2                 // only it
    mov x3, x5
    mov x4, x6
.else                          // below it (all, if none), and it for vmsif
    subs x3, x5, #1
    sbc x4, x6, xzr
.if \kind == 3
    orr x3, x3, x5
    orr x4, x4, x6
.endif
.endif
    add x13, _cpu, x13
    vmask_store x13, x3, x4, x9, x10
8:  gret 4
9:  vtrap 24
.endm
vmsfirst vmsbf_m, 1
vmsfirst vmsof_m, 2
vmsfirst vmsif_m, 3
vmsfirst vmsbf_mm, 1, 1
vmsfirst vmsof_mm, 2, 1
vmsfirst vmsif_mm, 3, 1

// vcpop.m rd, vs2[, v0.t]: [gadget][rd_off][vs2_off][insn][pc]
.macro vcpop_g name, masked
.gadget \name
    vm_check
    ldr x12, [_cpu, #CPU_riscv64_vl]
    ldp x13, x14, [_ip]
    add x14, _cpu, x14
    ldp x3, x4, [x14]
    vlmask x9, x10, x12, x11
.if \masked
    ldr x5, [_cpu, #CPU_riscv64_v]
    ldr x6, [_cpu, #(CPU_riscv64_v + 8)]
    and x9, x9, x5
    and x10, x10, x6
.endif
    and x3, x3, x9
    and x4, x4, x10
    fmov d0, x3
    mov v0.d[1], x4
    cnt v0.16b, v0.16b
    addv b0, v0.16b
    umov w3, v0.b[0]
    str x3, [_cpu, x13]
    gret 4
9:  vtrap 24
.endm
vcpop_g vcpop_m, 0
vcpop_g vcpop_mm, 1

// load or store one element of \sz bytes at [\a], post-incremented
.macro vld_el sz, sign, reg, a
.if \sz == 1
.if \sign
    ldrsb x\reg, [\a], #1
.else
    ldrb w\reg, [\a], #1
.endif
.elseif \sz == 2
.if \sign
    ldrsh x\reg, [\a], #2
.else
    ldrh w\reg, [\a], #2
.endif
.elseif \sz == 4
.if \sign
    ldrsw x\reg, [\a], #4
.else
    ldr w\reg, [\a], #4
.endif
.else
    ldr x\reg, [\a], #8
.endif
.endm
.macro vst_el sz, reg, a
.if \sz == 1
    strb w\reg, [\a], #1
.elseif \sz == 2
    strh w\reg, [\a], #2
.elseif \sz == 4
    str w\reg, [\a], #4
.else
    str x\reg, [\a], #8
.endif
.endm

// vmv.x.s rd, vs2: [gadget][vtype][rd_off][vs2_off][insn][pc]
// vmv.s.x vd, rs1: [gadget][vtype][vd_off][rs1_off][insn][pc]
// vid.v vd: [gadget][vtype][vd_off][insn][pc]
.macro vscalar sew
.gadget vmv_x_s_e\sew
    vt_check
vxsbody_e\sew :
    ldp x13, x14, [_ip, #8]
    add x14, _cpu, x14
    vld_el \sew, 1, 3, x14
    str x3, [_cpu, x13]
    gret 5
9:  vretype vxsbody, 0, 0, 0, 32
5:  vtrap 32
.gadget vmv_s_x_e\sew
    vt_check
vsxbody_e\sew :
    ldr x12, [_cpu, #CPU_riscv64_vl]
    cbz x12, 8f
    ldp x13, x14, [_ip, #8]
    ldr x3, [_cpu, x14]
    add x13, _cpu, x13
    vst_el \sew, 3, x13
8:  gret 5
9:  vretype vsxbody, 0, 0, 0, 32
5:  vtrap 32
.gadget vid_v_e\sew
    vt_check
vidbody_e\sew :
    ldr x12, [_cpu, #CPU_riscv64_vl]
    cbz x12, 8f
    ldr x13, [_ip, #8]
    add x13, _cpu, x13
    mov x3, #0
1:  vst_el \sew, 3, x13
    add x3, x3, #1
    cmp x3, x12
    b.lo 1b
8:  gret 4
9:  vretype vidbody, 1, 0, 0, 24
5:  vtrap 24
.endm
vscalar 1
vscalar 2
vscalar 4
vscalar 8
// vid.v vd, v0.t
.macro vidm sew
.gadget vidm_v_e\sew
    vt_check
vidmbody_e\sew :
    vloop_head vv, \sew
    mov x6, #0
1:  vact 1, 2f
    vel_st \sew, 6, x13
2:  add x6, x6, #1
    cmp x6, x12
    b.lo 1b
8:  gret 4
9:  vretype vidmbody, 1, 0, 0, 24
5:  vtrap 24
.endm
vrow vxsbody
vrow vsxbody
vrow vidbody

// vzext.vf<f> / vsext.vf<f>: vd's SEW elements from vs2's SEW/f ones,
// lowest first (the one overlap the spec allows, the source at the top of
// vd, survives that). vs2's group, EMUL = LMUL/f, is not checked.
// [gadget][vtype][vd][vs2][-][insn][pc]
.macro vxt name, sew, masked, f, sign
.gadget vxt_\name\()_\masked\()_e\sew
    vt_check
vxtbody_\name\()_\masked\()_e\sew :
    vloop_head vv, \sew
    mov x6, #0
1:  vact \masked, 2f
    vel_ld (\sew / \f), \sign, 10, x14
    vel_st \sew, 10, x13
2:  add x6, x6, #1
    cmp x6, x12
    b.lo 1b
    vloop_tail vxtbody_\name\()_\masked, 1, 0, 0
.endm
.macro vxt_all name, f, sign, x1, x2, x4, x8
.irp masked, 0, 1
.if \x2
    vxt \name, 2, \masked, \f, \sign
.endif
.if \x4
    vxt \name, 4, \masked, \f, \sign
.endif
    vxt \name, 8, \masked, \f, \sign
.endr
    vrowsel vxtbody_\name\()_0, \x1, \x2, \x4, \x8
    vrowsel vxtbody_\name\()_1, \x1, \x2, \x4, \x8
.endm

// single-width integer reductions: vd[0] = vs1[0] op vs2[0..vl-1]
// [gadget][vtype][vd_off][vs2_off][vs1_off][insn][pc]
.macro vred op, sew, sign, i0, cond=al
.gadget vred_\op\()_e\sew
    vt_check
vrbody_\op\()_e\sew :
    ldr x12, [_cpu, #CPU_riscv64_vl]
    cbz x12, 8f
    ldp x13, x14, [_ip, #8]
    ldr x15, [_ip, #24]
    add x14, _cpu, x14
    add x15, _cpu, x15
    vld_el \sew, \sign, 3, x15
1:  vld_el \sew, \sign, 4, x14
.ifc \i0, csel
    cmp x3, x4
    csel x3, x3, x4, \cond
.else
    \i0 x3, x3, x4
.endif
    subs x12, x12, #1
    b.ne 1b
    add x13, _cpu, x13
    vst_el \sew, 3, x13
8:  gret 6
9:  vretype vrbody_\op, 0, 1, 0, 40
5:  vtrap 40
.endm
// masked: only the active elements count
.macro vredm op, sew, sign, i0, cond=al
.gadget vredm_\op\()_e\sew
    vt_check
vrmbody_\op\()_e\sew :
    ldr x12, [_cpu, #CPU_riscv64_vl]
    cbz x12, 8f
    ldp x13, x14, [_ip, #8]
    ldr x15, [_ip, #24]
    add x13, _cpu, x13
    add x14, _cpu, x14
    add x15, _cpu, x15
    ldr x3, [_cpu, #CPU_riscv64_v]
    ldr x4, [_cpu, #(CPU_riscv64_v + 8)]
    mov x6, #0
    vel_ld \sew, \sign, 5, x15
1:  vact 1, 2f
    vel_ld \sew, \sign, 11, x14
.ifc \i0, csel
    cmp x5, x11
    csel x5, x5, x11, \cond
.else
    \i0 x5, x5, x11
.endif
2:  add x6, x6, #1
    cmp x6, x12
    b.lo 1b
    mov x6, #0
    vel_st \sew, 5, x13
8:  gret 6
9:  vretype vrmbody_\op, 0, 1, 0, 40
5:  vtrap 40
.endm
.irp sew, 1, 2, 4, 8
    vredm sum, \sew, 0, add
    vredm and, \sew, 0, and
    vredm or, \sew, 0, orr
    vredm xor, \sew, 0, eor
    vredm minu, \sew, 0, csel, lo
    vredm min, \sew, 1, csel, lt
    vredm maxu, \sew, 0, csel, hi
    vredm max, \sew, 1, csel, gt
.endr
.irp op, sum, and, or, xor, minu, min, maxu, max
    vrow vrmbody_\op
.endr
// (one row of bodies per op, below)
.irp sew, 1, 2, 4, 8
    vred sum, \sew, 0, add
    vred and, \sew, 0, and
    vred or, \sew, 0, orr
    vred xor, \sew, 0, eor
    vred minu, \sew, 0, csel, lo
    vred min, \sew, 1, csel, lt
    vred maxu, \sew, 0, csel, hi
    vred max, \sew, 1, csel, gt
.endr
.irp op, sum, and, or, xor, minu, min, maxu, max
    vrow vrbody_\op
.endr

// \out = VLMAX of the legal vtype in \vt (x9-x11 scratch)
.macro vlmax_of out, vt
    ubfx x9, \vt, #3, #3
    and x10, \vt, #7
    mov x11, #16
    lsr x11, x11, x9               // VLEN / SEW
    lsl x9, x11, x10
    mov \out, #8
    sub \out, \out, x10
    lsr \out, x11, \out
    cmp x10, #4
    csel \out, x9, \out, lo
.endm
// vsetvli x0, x0, vtype: vl kept, legal only when VLMAX is unchanged: from
// the vtype gen.c expected that is known, else worked out; otherwise vill.
// [gadget][new vtype][expected vtype][insn][pc]
.gadget vsetvl_keep
    ldp x8, x12, [_ip]
    ldr x13, [_cpu, #CPU_riscv64_vtype]
    cmp x13, x12
    b.ne 9f
1:  str x8, [_cpu, #CPU_riscv64_vtype]
    str xzr, [_cpu, #CPU_riscv64_vstart]
    gret 4
9:  tbnz x13, #63, 7f
    vlmax_of x14, x13
    vlmax_of x15, x8
    cmp x14, x15
    b.eq 1b
7:  mov x9, #(1 << 63)
    str x9, [_cpu, #CPU_riscv64_vtype]
    str xzr, [_cpu, #CPU_riscv64_vl]
    str xzr, [_cpu, #CPU_riscv64_vstart]
    gret 4

// vsetvl rd, rs1, rs2 (the vtype from a register), and any vset* whose
// vtype is not legal: [gadget][rd_off][avl: rs1_off][vtype: rs2_off or the
// immediate][how: bit 0 the vtype is an immediate, bit 1 AVL is VLMAX
// (rs1 = x0, rd != x0), bit 2 vl is kept (both x0)]
.gadget vsetvl_rt
    ldp x13, x14, [_ip]
    ldp x15, x12, [_ip, #16]
    mov x8, x15
    tbnz x12, #0, 1f
    ldr x8, [_cpu, x15]            // the vtype
1:  lsr x9, x8, #8                 // reserved bits, vill included
    cbnz x9, 7f
    and x10, x8, #7
    cmp x10, #4
    b.eq 7f
    ubfx x9, x8, #3, #3
    cmp x9, #3
    b.hi 7f
    cmp x10, #4                    // a fraction: SEW <= ELEN * LMUL
    b.lo 2f
    mov x11, #8
    sub x11, x11, x10
    add x11, x11, x9
    cmp x11, #3
    b.hi 7f
2:  vlmax_of x15, x8
    tbz x12, #2, 3f
    ldr x10, [_cpu, #CPU_riscv64_vtype] // keep vl: only if VLMAX does not change
    tbnz x10, #63, 7f
    vlmax_of x16, x10
    cmp x16, x15
    b.ne 7f
    ldr x10, [_cpu, #CPU_riscv64_vl]
    b 4f
3:  mov x10, x15
    tbnz x12, #1, 4f
    ldr x10, [_cpu, x14]           // AVL
    cmp x10, x15
    csel x10, x10, x15, lo
4:  str x8, [_cpu, #CPU_riscv64_vtype]
    str x10, [_cpu, #CPU_riscv64_vl]
    str xzr, [_cpu, #CPU_riscv64_vstart]
    str x10, [_cpu, x13]
    gret 4
7:  mov x9, #(1 << 63)             // illegal: vill, vl 0
    str x9, [_cpu, #CPU_riscv64_vtype]
    str xzr, [_cpu, #CPU_riscv64_vl]
    str xzr, [_cpu, #CPU_riscv64_vstart]
    str xzr, [_cpu, x13]
    gret 4

// the vector CSRs other than reads of vl/vtype/vlenb: csrrw/s/c and their
// immediate forms on vstart, vxsat, vxrm, vcsr, and reads of vl, vtype
// and vlenb that write too (illegal: read-only). [gadget][csr][f3][rd_off]
// [rs1_off or the immediate][write][insn][pc]
.gadget vcsr_op
    ldp x8, x9, [_ip]              // csr, funct3
    cmp x8, #0x008
    b.ne 1f
    ldr x10, [_cpu, #CPU_riscv64_vstart]
    b 5f
1:  cmp x8, #0x009
    b.ne 2f
    ldr w10, [_cpu, #CPU_riscv64_vxsat]
    b 5f
2:  cmp x8, #0x00a
    b.ne 3f
    ldr w10, [_cpu, #CPU_riscv64_vxrm]
    b 5f
3:  cmp x8, #0x00f
    b.ne 4f
    ldr w10, [_cpu, #CPU_riscv64_vxrm]
    ldr w11, [_cpu, #CPU_riscv64_vxsat]
    orr x10, x11, x10, lsl #1
    b 5f
4:  ldr x10, [_cpu, #CPU_riscv64_vl]  // vl, vtype, vlenb: read-only
    cmp x8, #0xc21
    b.ne 41f
    ldr x10, [_cpu, #CPU_riscv64_vtype]
41: cmp x8, #0xc22
    mov x11, #16
    csel x10, x11, x10, eq
5:  ldp x13, x14, [_ip, #16]       // rd_off, rs1_off or imm
    ldr x15, [_ip, #32]            // write?
    cbz x15, 8f
    cmp x8, #0xc00
    b.hs 9f
    mov x11, x14
    tbnz x9, #2, 6f
    ldr x11, [_cpu, x14]
6:  and x12, x9, #3                // 1 rw, 2 rs, 3 rc
    cmp x12, #2
    orr x15, x10, x11
    csel x11, x15, x11, eq
    cmp x12, #3
    bic x15, x10, x11
    csel x11, x15, x11, eq
    cmp x8, #0x008
    b.ne 61f
    and x11, x11, #127             // enough bits for any element index
    str x11, [_cpu, #CPU_riscv64_vstart]
    b 8f
61: cmp x8, #0x009
    b.ne 62f
    and w11, w11, #1
    str w11, [_cpu, #CPU_riscv64_vxsat]
    b 8f
62: cmp x8, #0x00a
    b.ne 63f
    and w11, w11, #3
    str w11, [_cpu, #CPU_riscv64_vxrm]
    b 8f
63: and w12, w11, #1
    str w12, [_cpu, #CPU_riscv64_vxsat]
    ubfx w12, w11, #1, #2
    str w12, [_cpu, #CPU_riscv64_vxrm]
8:  str x10, [_cpu, x13]
    gret 7
9:  vtrap 48

// vmv<nr>r.v: [gadget][vd_off][vs2_off][bytes][insn][pc]
.gadget vmvnr
    vm_check
    ldp x13, x14, [_ip]
    ldr x12, [_ip, #16]
    add x13, _cpu, x13
    add x14, _cpu, x14
1:  ldr q0, [x14], #16
    str q0, [x13], #16
    subs x12, x12, #16
    b.ne 1b
    gret 5
9:  vtrap 32

// unmasked indexed loads (vluxei / vloxei: in order either way), each
// element within one page and through the TLB. A miss or a page crossing
// leaves the rest to C from that element (vstart), so vd may be the index
// group itself (same EEW: each index is read before its element lands).
// [gadget][vtype][vd_off][vs2_off][rs1_off][insn][pc]
.macro vlx isz, dsz
.gadget vlx_i\isz\()_e\dsz
    vt_check
    ldr x12, [_cpu, #CPU_riscv64_vl]
    cbz x12, 8f
    ldp x13, x14, [_ip, #8]
    ldr x15, [_ip, #24]
    add x13, _cpu, x13
    add x14, _cpu, x14
    ldr x15, [_cpu, x15]
    mov x6, #0
1:  vld_el \isz, 0, 3, x14
    add x7, x15, x3
    and x9, x7, #0xfff
    cmp x9, #(0x1000 - \dsz)
    b.hi 6f
    vmem_tlb_miss TLB_ENTRY_page, 6f
    vld_el \dsz, 0, 4, x7
    vst_el \dsz, 4, x13
    add x6, x6, #1
    cmp x6, x12
    b.lo 1b
8:  gret 6
6:  str x6, [_cpu, #CPU_riscv64_vstart]
9:  ldr w8, [_ip, #32]             // the general path, from vstart
    mov x16, #40
    mov x17, #6
    b vmem_any
.endm
.irp isz, 1, 2, 4, 8
.irp dsz, 1, 2, 4, 8
    vlx \isz, \dsz
.endr
.endr

// instances of macros defined above that use later ones
vidm 1
vidm 2
vidm 4
vidm 8
vrow vidmbody
vfirst_g vfirst_m, 0
vfirst_g vfirst_mm, 1
vxt_all zf2, 2, 0, 0, 1, 1, 1
vxt_all sf2, 2, 1, 0, 1, 1, 1
vxt_all zf4, 4, 0, 0, 0, 1, 1
vxt_all sf4, 4, 1, 0, 0, 1, 1
vxt_all zf8, 8, 0, 0, 0, 0, 1
vxt_all sf8, 8, 1, 0, 0, 0, 1

// tables for gen.c, [op][kind][sew]: kinds vv, vx, vi; sews 1, 2, 4, 8
.pushsection_rodata
.p2align 3
.global.name riscv64_vbin_gadgets
.irp op, add, sub, rsub, and, or, xor, andn, mv, minu, min, maxu, max, sll, srl, sra, ror, rol, mul, macc, nmsac, madd, nmsub
.irp kind, vv, vx, vi
    gadgets riscv64_vbin_\op\()_\kind, e1, e2, e4, e8
.endr
.endr
.global.name riscv64_vcmp_gadgets
.irp op, seq, sne, sltu, slt, sleu, sle, sgtu, sgt
.irp kind, vv, vx, vi
    gadgets riscv64_vcmp_\op\()_\kind, e1, e2, e4, e8
.endr
.endr
.global.name riscv64_vbinm_gadgets
.irp op, add, sub, rsub, and, or, xor, andn, mv, minu, min, maxu, max, sll, srl, sra, ror, rol, mul, macc, nmsac, madd, nmsub, merge
.irp kind, vv, vx, vi
    gadgets riscv64_vbinm_\op\()_\kind, e1, e2, e4, e8
.endr
.endr
.macro velrow op, kind, masked
    gadgets riscv64_vel_\op\()_\kind\()_\masked, e1, e2, e4, e8
.endm
.global.name riscv64_vel_gadgets
.irp op, divu, div, remu, rem, mulhu, mulhsu, mulh
.irp kind, vv, vx
    velrow \op, \kind, 0
    velrow \op, \kind, 1
.endr
.endr
.macro vwtab name, kind, masked
    gadgets riscv64_vw_\name\()_\kind\()_\masked, e1, e2, e4, e8
.endm
.global.name riscv64_vw_gadgets
.irp name, waddu, wadd, wsubu, wsub, waddu_w, wadd_w, wsubu_w, wsub_w, wmulu, wmulsu, wmul, wmaccu, wmacc, wmaccus, wmaccsu, nsrl, nsra, wsll
.irp kind, vv, vx, vi
    vwtab \name, \kind, 0
    vwtab \name, \kind, 1
.endr
.endr
// [kind][masked][sew] per op; kinds as each op has them
.macro vptab name, kind, masked
    gadgets riscv64_vp\name\()_\kind\()_\masked, e1, e2, e4, e8
.endm
.global.name riscv64_vslide_gadgets          // up, down x vx, vi x masked
.irp name, slideup, slidedown
.irp kind, vx, vi
    vptab \name, \kind, 0
    vptab \name, \kind, 1
.endr
.endr
.global.name riscv64_vslide1_gadgets         // up, down x vx, vf x masked
.irp name, slide1up, slide1down
.irp kind, vx, vf
    vptab \name, \kind, 0
    vptab \name, \kind, 1
.endr
.endr
.global.name riscv64_vgather_gadgets         // vv, vx, vi, ei16 x masked
.irp kind, vv, vx, vi
    vptab gather, \kind, 0
    vptab gather, \kind, 1
.endr
    vptab gatherei16, vv, 0
    vptab gatherei16, vv, 1
.global.name riscv64_vmisc_gadgets           // vcompress, viota, viota masked
    gadgets riscv64_vpcompress, e1, e2, e4, e8
    gadgets riscv64_vpiota_0, e1, e2, e4, e8
    gadgets riscv64_vpiota_1, e1, e2, e4, e8
.macro vctab op, kind, mc
    gadgets riscv64_vc\op\()_\kind\()_\mc, e1, e2, e4, e8
.endm
.global.name riscv64_vcarry_gadgets          // add, sub x vv, vx, vi x adc, madc, madc+v0
.irp op, add, sub
.irp kind, vv, vx, vi
    vctab \op, \kind, 01
    vctab \op, \kind, 10
    vctab \op, \kind, 11
.endr
.endr
.macro vutab op, masked
    gadgets riscv64_vu\op\()_\masked, e1, e2, e4, e8
.endm
.global.name riscv64_vunary_gadgets          // brev8 rev8 brev clz ctz cpop x masked
.irp op, brev8, rev8, brev, clz, ctz, cpop
    vutab \op, 0
    vutab \op, 1
.endr
.macro vfbtab op, kind, masked
    gadgets riscv64_vfb_\op\()_\kind\()_\masked, e1, e2, e4, e8
.endm
.global.name riscv64_vfb_gadgets               // [op][kind vv vf][masked][sew]
.irp op, add, sub, rsub, mul, div, rdiv, min, max, sgnj, sgnjn, sgnjx, mv, sqrt, macc, nmacc, msac, nmsac, madd, nmadd, msub, nmsub, merge
.irp kind, vv, vf
    vfbtab \op, \kind, 0
    vfbtab \op, \kind, 1
.endr
.endr
.macro vfctab op, kind, masked
    gadgets riscv64_vfc_\op\()_\kind\()_\masked, e1, e2, e4, e8
.endm
.global.name riscv64_vfc_gadgets               // [op eq ne lt le gt ge][kind][masked][sew]
.irp op, eq, ne, lt, le, gt, ge
.irp kind, vv, vf
    vfctab \op, \kind, 0
    vfctab \op, \kind, 1
.endr
.endr
.global.name riscv64_vfmv_gadgets              // vfmv.f.s, vfmv.s.f
    gadgets riscv64_vfmv_f_s, e1, e2, e4, e8
    gadgets riscv64_vfmv_s_f, e1, e2, e4, e8

.macro vfetab name, masked
    gadgets riscv64_vfe_\name\()_\masked, e1, e2, e4, e8
.endm
// [vs1 code 0-23][masked][sew]; holes are 0
.global.name riscv64_vfcvt_gadgets
.irp n, cvt0, cvt1, cvt2, cvt3, cvt4, cvt5, cvt6, cvt7, cvt8, cvt9, cvt10, cvt11, cvt12, cvt13, cvt14, cvt15, cvt16, cvt17, cvt18, cvt19, cvt20, cvt21, cvt22, cvt23
    vfetab \n, 0
    vfetab \n, 1
.endr
.global.name riscv64_vfun1_gadgets             // class, rsqrt7, rec7
.irp n, class, rsqrt7, rec7
    vfetab \n, 0
    vfetab \n, 1
.endr
.macro vfrtab name, masked
    gadgets riscv64_vfr_\name\()_\masked, e1, e2, e4, e8
.endm
.global.name riscv64_vfred_gadgets             // sum, min, max, wsum
.irp n, sum, min, max, wsum
    vfrtab \n, 0
    vfrtab \n, 1
.endr
.macro vfwtab name, kind, masked
    gadgets riscv64_vfw_\name\()_\kind\()_\masked, e1, e2, e4, e8
.endm
.global.name riscv64_vfw_gadgets               // [op][kind][masked][sew]
.irp n, add, sub, addw, subw, mul, macc, nmacc, msac, nmsac
.irp kind, vv, vf
    vfwtab \n, \kind, 0
    vfwtab \n, \kind, 1
.endr
.endr
.macro vxttab name, masked
    gadgets riscv64_vxt_\name\()_\masked, e1, e2, e4, e8
.endm
.global.name riscv64_vxt_gadgets               // zf2 sf2 zf4 sf4 zf8 sf8 x masked
.irp n, zf2, sf2, zf4, sf4, zf8, sf8
    vxttab \n, 0
    vxttab \n, 1
.endr
.macro vfxtab name, kind, masked
    gadgets riscv64_vfx_\name\()_\kind\()_\masked, e1, e2, e4, e8
.endm
.global.name riscv64_vfx_gadgets               // [op][kind vv vx vi][masked][sew]
.irp n, saddu, sadd, ssubu, ssub, aaddu, aadd, asubu, asub, smul, ssrl, ssra, nclipu, nclip
.irp kind, vv, vx, vi
    vfxtab \n, \kind, 0
    vfxtab \n, \kind, 1
.endr
.endr
.macro vwrtab name, masked
    gadgets riscv64_vwr_\name\()_\masked, e1, e2, e4, e8
.endm
.global.name riscv64_vwred_gadgets             // sumu, sum x masked
    vwrtab sumu, 0
    vwrtab sumu, 1
    vwrtab sum, 0
    vwrtab sum, 1
.global.name riscv64_vredm_gadgets             // masked integer reductions
.irp op, sum, and, or, xor, minu, min, maxu, max
    gadgets riscv64_vredm_\op, e1, e2, e4, e8
.endr
.global.name riscv64_vidm_gadgets
    gadgets riscv64_vidm_v, e1, e2, e4, e8
.global.name riscv64_vcmpm_gadgets
.irp op, seq, sne, sltu, slt, sleu, sle, sgtu, sgt
.irp kind, vv, vx, vi
    gadgets riscv64_vcmpm_\op\()_\kind, e1, e2, e4, e8
.endr
.endr
.global.name riscv64_vred_gadgets
.irp op, sum, and, or, xor, minu, min, maxu, max
    gadgets riscv64_vred_\op, e1, e2, e4, e8
.endr
.global.name riscv64_vm_gadgets
    gadgets riscv64_vm, andn, and, or, xor, orn, nand, nor, xnor
.global.name riscv64_vscalar_gadgets
    gadgets riscv64_vmv_x_s, e1, e2, e4, e8
    gadgets riscv64_vmv_s_x, e1, e2, e4, e8
    gadgets riscv64_vid_v, e1, e2, e4, e8
.global.name riscv64_vlx_gadgets
.irp isz, 1, 2, 4, 8
    gadgets riscv64_vlx_i\isz, e1, e2, e4, e8
.endr
.popsection
