#include "emu/interrupt.h"
#include "gadgets.h"

.global NAME(jit_enter)
.type_compat NAME(jit_enter),function
NAME(jit_enter):
    push %rbp
    push %rbx
    push %r12
    push %r13
    push %r14
    push %r15
    # make stack 16 byte aligned
    # this is preserved elsewhere by only doing even numbers of pushes
    sub $0x8, %rsp
    leaq JIT_BLOCK_code(%rdi), %_ip
    movq %rsi, %_cpu
    movq %rsp, LOCAL_bp(%_cpu)
    leaq TLB_entries(%rdx), %_tlb
    load_regs
    gret

.global jit_ret_chain
jit_ret_chain:
    btq $63, %_ip
    jc jit_ret
    mov CPU_poked_ptr(%_cpu), %r10
    cmpb $0, (%r10)
    jnz poke
    # Per-entry chain budget (jit_frame.chain_budget, reset to 8192 by the
    # frontend before every jit_enter). This is what makes BACKWARD edges safe
    # to link: a chained loop returns to C every few thousand dispatches, so
    # the cycle counter still fires and jetsam/quiesce write waiters cannot
    # starve. Kept in lockstep with the aarch64 host's jit_ret_chain -- the
    # frontend decision to link backward jumps is host-independent C, so an
    # Intel host without this bound would get exactly that starvation bug.
    decq LOCAL_chain_budget(%_cpu)
    jle poke
    leaq -JIT_BLOCK_code(%_ip), %r10
    mov %r10, LOCAL_last_block(%_cpu)
    gret

# .global so the ret gadget's return-cache path in control.S can branch here
# on chain-budget expiry, the way the aarch64 host's poke already is. Without
# it the Intel host fails to LINK, which an assemble-only check cannot catch.
.global poke
poke:
    mov -JIT_BLOCK_code+JIT_BLOCK_addr(%_ip), %_eip
    # fallthrough

.global jit_ret
jit_ret:
    movl $-1, %_tmp
    # fallthrough

.global jit_exit
jit_exit:
    save_regs
    movl %_eip, CPU_eip(%_cpu)
    movq LOCAL_bp(%_cpu), %rsp
    add $0x8, %rsp # keep stack 16 byte aligned
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbx
    pop %rbp
    mov %_tmp, %eax
    ret

.gadget interrupt
    movl (%_ip), %_tmp
    movl 16(%_ip), %r14d
    # Full quadword: see segfault_read/write in memory.S. The 32-bit load above
    # already zero-extends r14, so this stores the operand with a clean high
    # half instead of inheriting a 64-bit guest's leftovers.
    movq %r14, CPU_segfault_addr(%_cpu)
    movl 8(%_ip), %_eip
    movb $0, CPU_segfault_was_write(%_cpu)
    jmp jit_exit

.gadget exit
    movl (%_ip), %_eip
    jmp jit_ret
