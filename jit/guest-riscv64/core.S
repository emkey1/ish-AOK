// RISC-V guest core gadgets: constants, addi, control flow, ecall, exits.
// Operand convention: register operands arrive as byte offsets into
// cpu_state (see gadgets.h header). Immediates arrive sign-extended to 64
// bits by gen_step_riscv64.

#include "gadgets.h"

// Exit-to-kernel with an interrupt code and a 64-bit guest pc.
// Code stream: [gadget][code][pc][addr]
// Same layout/rationale as guest-arm64 control.S's interrupt gadget.
.gadget interrupt
    ldr _tmp, [_ip]
    ldr x8, [_ip, #8]
    str x8, [_cpu, #CPU_riscv64_pc]
    ldr x8, [_ip, #16]
    str x8, [_cpu, #CPU_segfault_addr]
    strb wzr, [_cpu, #CPU_segfault_was_write]
    b jit_exit

// HLE "giant gadget" -- riscv64 twin of guest-arm64 control.S's hle_call
// (see that gadget's comment for the full contract). Bridges to
// hle_call(cpu, tlb, fn, entry_ip); INT_NONE -> jit_ret with the pc the C
// side stored, any real interrupt code -> jit_exit. x27 carries the result
// across restore_c. Code stream: [fn | abi<<8][entry_ip].
.gadget hle_call
    ldr x8, [_ip]
    ldr x9, [_ip, #8]
    save_c
    mov x0, x1                 // cpu   (x1 = _cpu; read before x1 is written)
    sub x1, x2, #TLB_entries   // tlb   (consumes x2 before x2 is written)
    mov x2, x8                 // fn word
    mov x3, x9                 // entry ip
    bl NAME(hle_call)
    mov x27, x0
    restore_c
    tbnz w27, #31, 1f          // INT_NONE (-1): success, pc updated
    mov w0, w27
    b jit_exit
1:  b jit_ret

.extern hle_call

// ECALL. Code stream: [gadget][next_pc]. PC is left at the instruction
// AFTER the ecall; prepare_syscall_restart rewinds by 4 when needed.
.gadget ecall
    ldr x8, [_ip]
    str x8, [_cpu, #CPU_riscv64_pc]
    mov w0, #INT_RISCV64_ECALL
    b jit_exit

// FENCE: guest threads run on concurrent host threads, so a guest data
// fence must be a real host barrier (same reasoning as guest-arm64's
// LDAR/STLR and DMB handling). No code-stream params.
.gadget fence
    dmb ish
    gret

// FENCE.I: synchronize instruction fetch with prior stores. Write-driven
// block invalidation only fires on the TLB write-MISS path, so a code
// rewrite through a still-cached writable TLB entry is invisible and this
// instruction is the guest's only invalidation signal (same bug class as
// guest-arm64's IC IVAU; see jit/jit.c's arm64_ic_ivau comment and
// tests/manual/arm64/smc_stale_block.c). FENCE.I carries no VA, so it
// drops every translated block via riscv64_fence_i (jit/jit.c).
.gadget fence_i
    save_c
    sub x0, x2, #TLB_entries
    bl NAME(riscv64_fence_i)
    restore_c
    gret

.extern riscv64_fence_i

// rd = constant (LUI, AUIPC, li-style folds; also jal/jalr link writes).
// Code stream: [gadget][rd_off][value]
.gadget mov_const
    ldr x9, [_ip]
    ldr x10, [_ip, #8]
    str x10, [_cpu, x9]
    gret 2

// rd = rs1 + imm (ADDI; covers mv and li-with-base).
// Code stream: [gadget][rd_off][rs1_off][imm]
.gadget addi
    ldr x9, [_ip]           // rd_off
    ldr x10, [_ip, #8]      // rs1_off
    ldr x11, [_ip, #16]     // imm
    ldr x10, [_cpu, x10]
    add x10, x10, x11
    str x10, [_cpu, x9]
    gret 3

// Unconditional compile-time branch (JAL after its link write; also
// gen_exit's block-size-cap continuation). Code stream: [gadget][target],
// target tagged with bit 63 when unchained — same tagged-word chaining
// scheme as guest-arm64 control.S; riscv64_branch_dispatch below is that
// design with this guest's pc field.
.gadget b
    ldr _ip, [_ip]
    b riscv64_branch_dispatch

.global riscv64_branch_dispatch
riscv64_branch_dispatch:
    tbz _ip, #63, riscv64_chain
    and x8, _ip, #0x7fffffffffffffff
    str x8, [_cpu, #CPU_riscv64_pc]
    b jit_ret
riscv64_chain:
    ldr x8, [_cpu, #CPU_poked_ptr]
    ldrb w8, [x8]
    cbnz w8, riscv64_chain_poked
    // Chain budget bounds time-in-JIT (see guest-arm64 control.S and
    // jit_frame.chain_budget). The frontend patches BOTH forward and
    // backward edges (jit.c cpu_step_to_interrupt_riscv64), so chained
    // loops are real: this budget is what keeps them from starving
    // jetsam/quiesce waiters.
    ldr x8, [_cpu, #LOCAL_chain_budget]
    subs x8, x8, #1
    str x8, [_cpu, #LOCAL_chain_budget]
    b.le riscv64_chain_poked
    sub x8, _ip, #JIT_BLOCK_code
    str x8, [_cpu, #LOCAL_last_block]
    gret
riscv64_chain_poked:
    ldur x8, [_ip, #(JIT_BLOCK_addr - JIT_BLOCK_code)]
    str x8, [_cpu, #CPU_riscv64_pc]
    b jit_ret
