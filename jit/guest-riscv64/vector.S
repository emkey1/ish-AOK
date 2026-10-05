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
