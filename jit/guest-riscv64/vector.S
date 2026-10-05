// The V extension: one gadget for every vector instruction, which runs it in
// C (jit/riscv64_vector.c). Code stream: [gadget][insn][pc]; a trap (illegal
// instruction, or a fault with vstart left at the element) exits at pc.

#include "gadgets.h"

.text

.gadget vop
    ldr x8, [_ip]              // the encoding
    save_c
    mov x0, x1                 // cpu   (x1 = _cpu; read before x1 is written)
    sub x1, x2, #TLB_entries   // tlb   (consumes x2 before x2 is written)
    mov w2, w8
    bl NAME(riscv64_vector_exec)
    mov x27, x0
    restore_c
    tbnz w27, #31, 1f          // INT_NONE (-1): done
    ldr x9, [_ip, #8]
    str x9, [_cpu, #CPU_riscv64_pc]
    mov w0, w27
    b jit_exit
1:  gret 2

// Run the instruction at stream word \insn_off in C, as vop does, then step
// past \pop words; a trap exits at the pc in word \pc_off.
.macro vcall_c insn_off, pc_off, pop
    ldr x8, [_ip, #\insn_off]
    save_c
    mov x0, x1
    sub x1, x2, #TLB_entries
    mov w2, w8
    bl NAME(riscv64_vector_exec)
    mov x27, x0
    restore_c
    tbnz w27, #31, 7f
    ldr x9, [_ip, #\pc_off]
    str x9, [_cpu, #CPU_riscv64_pc]
    mov w0, w27
    b jit_exit
7:  gret \pop
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
9:  vcall_c 24, 32, 5
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
9:  vcall_c 24, 32, 5

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

// vfirst.m rd, vs2 (unmasked), vl up to 64: the index of the first set mask
// bit below vl, or -1. [gadget][rd_off][vs2_off][insn][pc]
.gadget vfirst_m
    ldr x10, [_cpu, #CPU_riscv64_vtype]
    tbnz x10, #63, 9f
    ldr x11, [_cpu, #CPU_riscv64_vstart]
    cbnz x11, 9f
    ldr x12, [_cpu, #CPU_riscv64_vl]
    cmp x12, #64
    b.hi 9f
    ldp x8, x9, [_ip]
    ldr x13, [_cpu, x9]
    mov x14, #-1
    cmp x12, #64
    b.eq 1f
    mov x15, #1
    lsl x15, x15, x12
    sub x15, x15, #1
    and x13, x13, x15
1:  cbz x13, 2f
    rbit x13, x13
    clz x14, x13
2:  str x14, [_cpu, x8]
    gret 4
9:  vcall_c 16, 24, 4

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
// \pc_off); a nonzero vstart goes to C, at 5f.
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
.macro vlanemask sew
    cmp x5, #64
    csel x9, x3, x4, lo
    lsr x9, x9, x5
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
    add x5, x5, #(16 / \sew)
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
5:  vcall_c 32, 40, 6
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
5:  vcall_c 32, 40, 6
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
5:  vcall_c 32, 40, 6
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
5:  vcall_c 32, 40, 6
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
5:  vcall_c 32, 40, 6
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
9:  vcall_c 24, 32, 5
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
.macro vmsfirst name, kind
.gadget \name
    vm_check
    ldr x12, [_cpu, #CPU_riscv64_vl]
    cbz x12, 8f
    ldp x13, x14, [_ip]
    add x14, _cpu, x14
    ldp x3, x4, [x14]
    vlmask x9, x10, x12, x11
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
9:  vcall_c 16, 24, 4
.endm
vmsfirst vmsbf_m, 1
vmsfirst vmsof_m, 2
vmsfirst vmsif_m, 3

// vcpop.m rd, vs2: [gadget][rd_off][vs2_off][insn][pc]
.gadget vcpop_m
    vm_check
    ldr x12, [_cpu, #CPU_riscv64_vl]
    ldp x13, x14, [_ip]
    add x14, _cpu, x14
    ldp x3, x4, [x14]
    vlmask x9, x10, x12, x11
    and x3, x3, x9
    and x4, x4, x10
    fmov d0, x3
    mov v0.d[1], x4
    cnt v0.16b, v0.16b
    addv b0, v0.16b
    umov w3, v0.b[0]
    str x3, [_cpu, x13]
    gret 4
9:  vcall_c 16, 24, 4

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
5:  vcall_c 24, 32, 5
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
5:  vcall_c 24, 32, 5
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
5:  vcall_c 16, 24, 4
.endm
vscalar 1
vscalar 2
vscalar 4
vscalar 8
vrow vxsbody
vrow vsxbody
vrow vidbody

// vzext.vf<n> / vsext.vf<n>, element by element, lowest first (the one
// overlap the spec allows, the source at the top of vd, survives that;
// gen.c sends any overlap to C anyway): [gadget][vtype][vd_off][vs2_off][insn][pc]
.macro vext name, dsz, ssz, sign
.gadget \name
    vt_check
    ldr x12, [_cpu, #CPU_riscv64_vl]
    cbz x12, 8f
    ldp x13, x14, [_ip, #8]
    add x13, _cpu, x13
    add x14, _cpu, x14
1:  vld_el \ssz, \sign, 3, x14
    vst_el \dsz, 3, x13
    subs x12, x12, #1
    b.ne 1b
8:  gret 5
9:  vcall_c 24, 32, 5
.endm
vext vzext_vf2_e2, 2, 1, 0
vext vzext_vf2_e4, 4, 2, 0
vext vzext_vf2_e8, 8, 4, 0
vext vzext_vf4_e4, 4, 1, 0
vext vzext_vf4_e8, 8, 2, 0
vext vzext_vf8_e8, 8, 1, 0
vext vsext_vf2_e2, 2, 1, 1
vext vsext_vf2_e4, 4, 2, 1
vext vsext_vf2_e8, 8, 4, 1
vext vsext_vf4_e4, 4, 1, 1
vext vsext_vf4_e8, 8, 2, 1
vext vsext_vf8_e8, 8, 1, 1

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
5:  vcall_c 32, 40, 6
.endm
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

// vsetvli x0, x0, vtype: vl kept, legal only when VLMAX is unchanged, which
// gen.c checked against the vtype it expects in force:
// [gadget][new vtype][expected vtype][insn][pc]
.gadget vsetvl_keep
    ldp x8, x9, [_ip]
    ldr x10, [_cpu, #CPU_riscv64_vtype]
    cmp x10, x9
    b.ne 9f
    str x8, [_cpu, #CPU_riscv64_vtype]
    str xzr, [_cpu, #CPU_riscv64_vstart]
    gret 4
9:  vcall_c 16, 24, 4

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
9:  vcall_c 24, 32, 5

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
9:  vcall_c 32, 40, 6
.endm
.irp isz, 1, 2, 4, 8
.irp dsz, 1, 2, 4, 8
    vlx \isz, \dsz
.endr
.endr

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
