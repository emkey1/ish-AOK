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
    and x9, x7, #0xfffffffffffff000
    ubfx x10, x7, #12, #10
    eor x10, x10, x7, lsr #22
    and x10, x10, #(1 << TLB_BITS) - 1
    mov w11, #TLB_ENTRY_SIZE
    madd x10, x10, x11, _tlb
    ldr x11, [x10, #\field]
    cmp x9, x11
    b.ne 9f
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

// vmseq.vi / vmsne.vi at SEW 8, LMUL 1, unmasked, from vstart 0 -- the
// compare of the vectorized string loops (strlen's vmseq.vi v1, v1, 0):
// sixteen byte compares packed into sixteen mask bits. Mask-producing ops
// treat the tail as agnostic, so all sixteen are written.
// [gadget][vd_off][vs2_off][imm][insn][pc]
.macro vmscmp_vi8 name, invert
.gadget \name
    ldr x10, [_cpu, #CPU_riscv64_vtype]
    and x11, x10, #(0x3f | (1 << 63))   // vill, vsew, vlmul all zero: e8, m1
    cbnz x11, 9f
    ldr x11, [_cpu, #CPU_riscv64_vstart]
    cbnz x11, 9f
    ldp x8, x9, [_ip]
    ldr x12, [_ip, #16]
    ldr q1, [_cpu, x9]
    dup v2.16b, w12
    cmeq v0.16b, v1.16b, v2.16b
    .if \invert
    mvn v0.16b, v0.16b
    .endif
    adr x13, vmask_weights
    ldr q3, [x13]
    and v0.16b, v0.16b, v3.16b
    addv b4, v0.8b
    ext v5.16b, v0.16b, v0.16b, #8
    addv b5, v5.8b
    umov w13, v4.b[0]
    umov w14, v5.b[0]
    orr w13, w13, w14, lsl #8
    strh w13, [_cpu, x8]
    gret 5
9:  vcall_c 24, 32, 5
.endm
vmscmp_vi8 vmseq_vi8, 0
vmscmp_vi8 vmsne_vi8, 1
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
