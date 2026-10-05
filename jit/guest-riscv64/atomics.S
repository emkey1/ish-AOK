// RISC-V A-extension gadgets: LR/SC and AMOs.
//
// v8.0 BASELINE ONLY (no LSE casal/ldaddal): the gadget-ISA invariant is
// v8.0 unless hostcaps-gated (A8/A9-era devices are supported hosts), so
// everything here is ldaxr/stlxr exclusive loops. Acquire+release ordering
// on every op — RISC-V aq/rl bits are decoded but always treated as
// aq+rl, which is stronger and always correct.
//
// Code stream (all gadgets): [gadget][rd_off][rs1_off][rs2_off][orig_ip]
// (LR ignores the rs2 slot; layout kept uniform). Fault-restart contract
// as memory.S: orig_ip is ALWAYS the last word.
//
// A misaligned LR, SC or AMO is an address-misaligned trap, which Linux
// does not emulate for atomics: SIGBUS/BUS_ADRALN at the address
// (amo_align, INT_ALIGN). AOK does not claim Zama16b. So every access here
// is naturally aligned and never straddles a page -- and the host's
// exclusives, which need alignment on an ARMv8.0 host, always get it.
//
// LR stashes the GUEST address (pre-prep x7) in riscv64_res_addr and the
// loaded value in riscv64_res_val; SC succeeds only if its guest address
// matches res_addr AND memory still equals res_val (check + CAS in one
// exclusive sequence, the arm64_stxp design) — the reservation survives
// INT_PF so a CoW fault mid LL/SC restarts correctly, and is cleared on
// every SC regardless of outcome.

#include "gadgets.h"

.text

// Shared operand fetch: x19=rd_off, x20=rs2 value, x7=guest address.
// x19/x20 are callee-saved (prep may bl into C).
.macro amo_head
    ldr x19, [_ip]
    ldr x9, [_ip, #8]
    ldr x10, [_ip, #16]
    add _ip, _ip, #32
    ldr x20, [_cpu, x10]
    ldr x7, [_cpu, x9]
.endm

// A misaligned address: SIGBUS (BUS_ADRALN) at this instruction. \bits is
// the access size, \write 1 for SC and the AMOs.
.macro amo_align bits, write
    tst x7, #(\bits / 8 - 1)
    b.eq 1f
    ldur x9, [_ip, #-8]
    str x9, [_cpu, #CPU_riscv64_pc]
    str x7, [_cpu, #CPU_segfault_addr]
    mov w9, #\write
    strb w9, [_cpu, #CPU_segfault_was_write]
    mov w0, #INT_ALIGN
    b jit_exit
1:
.endm

// ---- LR ------------------------------------------------------------------
.gadget lr_w
    amo_head
    amo_align 32, 0
    str x7, [_cpu, #CPU_riscv64_res_addr]
    read_prep 32, rvlrw
    ldar w11, [x7]
    sxtw x11, w11
    str x11, [_cpu, #CPU_riscv64_res_val]
    str x11, [_cpu, x19]
    gret
    read_bullshit 32, rvlrw

.gadget lr_d
    amo_head
    amo_align 64, 0
    str x7, [_cpu, #CPU_riscv64_res_addr]
    read_prep 64, rvlrd
    ldar x11, [x7]
    str x11, [_cpu, #CPU_riscv64_res_val]
    str x11, [_cpu, x19]
    gret
    read_bullshit 64, rvlrd

// ---- SC ------------------------------------------------------------------
// rd = 0 on success, 1 on failure. Reservation cleared either way.
.macro sc_gadget name, size, id, exp, old, new
.gadget \name
    amo_head
    amo_align \size, 1
    ldr x11, [_cpu, #CPU_riscv64_res_addr]
    cmp x7, x11
    b.ne 8f                   // no/other reservation: fail without prep
    write_prep \size, \id
    ldr x21, [_cpu, #CPU_riscv64_res_val]
1:  ldaxr \old, [x7]
    cmp \old, \exp
    b.ne 7f                   // memory changed since LR: fail (clrex)
    stlxr w13, \new, [x7]
    cbnz w13, 1b
    mov x11, #0               // success
    b 9f
7:  clrex
8:  mov x11, #1               // failure
9:  str x11, [_cpu, x19]
    mov x9, #-1               // clear the reservation
    str x9, [_cpu, #CPU_riscv64_res_addr]
    write_done \size, \id
    gret
    write_bullshit \size, \id
.endm

sc_gadget sc_w, 32, rvscw, w21, w12, w20
sc_gadget sc_d, 64, rvscd, x21, x12, x20

// ---- AMOs ----------------------------------------------------------------
// rd = old memory value (sign-extended for W); memory = op(old, rs2).
// Each op is a named sub-macro (result, old, rs2) so multi-instruction
// bodies (min/max) fit the same skeleton.
.macro amo_op_swap o, a, b
    mov \o, \b
.endm
.macro amo_op_add o, a, b
    add \o, \a, \b
.endm
.macro amo_op_xor o, a, b
    eor \o, \a, \b
.endm
.macro amo_op_and o, a, b
    and \o, \a, \b
.endm
.macro amo_op_or o, a, b
    orr \o, \a, \b
.endm
.macro amo_op_min o, a, b
    cmp \a, \b
    csel \o, \a, \b, lt
.endm
.macro amo_op_max o, a, b
    cmp \a, \b
    csel \o, \a, \b, gt
.endm
.macro amo_op_minu o, a, b
    cmp \a, \b
    csel \o, \a, \b, lo
.endm
.macro amo_op_maxu o, a, b
    cmp \a, \b
    csel \o, \a, \b, hi
.endm

.macro amo_w_gadget op
.gadget amo\op\()_w
    amo_head
    amo_align 32, 1
    write_prep 32, rva\op\()w
1:  ldaxr w11, [x7]
    amo_op_\op w12, w11, w20
    stlxr w13, w12, [x7]
    cbnz w13, 1b
    sxtw x11, w11
    str x11, [_cpu, x19]
    write_done 32, rva\op\()w
    gret
    write_bullshit 32, rva\op\()w
.endm

.macro amo_d_gadget op
.gadget amo\op\()_d
    amo_head
    amo_align 64, 1
    write_prep 64, rva\op\()d
1:  ldaxr x11, [x7]
    amo_op_\op x12, x11, x20
    stlxr w13, x12, [x7]
    cbnz w13, 1b
    str x11, [_cpu, x19]
    write_done 64, rva\op\()d
    gret
    write_bullshit 64, rva\op\()d
.endm

.irp op, swap,add,xor,and,or,min,max,minu,maxu
    amo_w_gadget \op
    amo_d_gadget \op
.endr
