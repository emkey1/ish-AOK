#include "emu/interrupt.h"
#include "gadgets.h"

.global NAME(jit_enter)
.type_compat jit_enter,function
NAME(jit_enter):
    stp x18, x19, [sp, -0x70]!
    stp x20, x21, [sp, 0x10]
    stp x22, x23, [sp, 0x20]
    stp x24, x25, [sp, 0x30]
    stp x26, x27, [sp, 0x40]
    stp x28, x29, [sp, 0x50]
    str lr, [sp, 0x60]
    add _ip, x0, JIT_BLOCK_code
    # cpu is already x1
    add _tlb, x2, TLB_entries
    load_regs
    gret

.global jit_ret_chain
jit_ret_chain:
    cmp _ip, 0
    b.lt jit_ret
    ldr x8, [_cpu, CPU_poked_ptr]
    ldrb w8, [x8]
    cmp w8, 0
    b.ne poke
    # Per-entry chain budget (jit_frame.chain_budget, reset to 8192 by the
    # frontend before every jit_enter), decremented per chained dispatch and
    # exiting to C on expiry. This is what makes BACKWARD edges safe to link:
    # a chained loop returns to C every few thousand dispatches, so the cycle
    # counter still fires and jetsam_lock/quiesce write waiters cannot starve.
    # Same rule the amd64/arm64/riscv64 chain paths already use.
    ldr x8, [_cpu, LOCAL_chain_budget]
    subs x8, x8, 1
    str x8, [_cpu, LOCAL_chain_budget]
    b.le poke
    sub x8, _ip, JIT_BLOCK_code
    str x8, [_cpu, LOCAL_last_block]
    gret

.global poke
poke:
    ldr eip, [_ip, -JIT_BLOCK_code+JIT_BLOCK_addr]
    # fallthrough

.global jit_ret
jit_ret:
    # load -1
    mvn _tmp, wzr
    # fallthrough

.global jit_exit
jit_exit:
    save_regs
    ldr lr, [sp, 0x60]
    ldp x28, x29, [sp, 0x50]
    ldp x26, x27, [sp, 0x40]
    ldp x24, x25, [sp, 0x30]
    ldp x22, x23, [sp, 0x20]
    ldp x20, x21, [sp, 0x10]
    ldp x18, x19, [sp], 0x70
    # _tmp is already x0
    ret

.gadget interrupt
    ldr _tmp, [_ip]
    ldr w8, [_ip, 16]
    # Full doubleword: see segfault_read/write in memory.S. The 32-bit load
    # above already zero-extends into x8, so this stores the operand with a
    # clean high half instead of inheriting a 64-bit guest's leftovers.
    str x8, [_cpu, CPU_segfault_addr]
    ldr eip, [_ip, 8]
    strb wzr, [_cpu, CPU_segfault_was_write]
    b jit_exit

.gadget exit
    ldr eip, [_ip]
    b jit_ret
