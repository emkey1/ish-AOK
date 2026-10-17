// ppc64 guest branch gadgets. A branch ends its block: a direct target is a
// tagged word the frontend chains (core.S, ppc64_branch_dispatch), an
// indirect one (LR, CTR) is stored as the pc and found by the frontend
// (ppc64_jump_x8). Taken and fall-through words are the stream's last two.

#include "gadgets.h"

.text

.macro take off
    ldr _ip, [_ip, #\off]
    b ppc64_branch_dispatch
.endm

// bc on one CR bit: [crf][mask][taken][fall]
.gadget bc_t
    ldp x9, x10, [_ip]
    ldr w9, [_cpu, x9]
    tst w9, w10
    b.eq 1f
    take 16
1:  take 24
.gadget bc_f
    ldp x9, x10, [_ip]
    ldr w9, [_cpu, x9]
    tst w9, w10
    b.ne 1f
    take 16
1:  take 24

// bdnz, bdz: [taken][fall]
.gadget bdnz
    ldr x9, [_cpu, #CPU_ppc64_ctr]
    subs x9, x9, #1
    str x9, [_cpu, #CPU_ppc64_ctr]
    b.eq 1f
    take 0
1:  take 8
.gadget bdz
    ldr x9, [_cpu, #CPU_ppc64_ctr]
    subs x9, x9, #1
    str x9, [_cpu, #CPU_ppc64_ctr]
    b.ne 1f
    take 0
1:  take 8

// Every other BO, whole: w15 = 1 to branch. x9 = BO, x10 = the field's
// offset, x11 = the bit's mask. CTR is decremented unless BO[2] (4) says not,
// and must then be nonzero, or zero with BO[1] (2); the CR bit counts unless
// BO[0] (16) says not, and must equal BO[3] (8).
.macro bo_cond
    mov w15, #1
    tbnz x9, #2, 1f
    ldr x12, [_cpu, #CPU_ppc64_ctr]
    sub x12, x12, #1
    str x12, [_cpu, #CPU_ppc64_ctr]
    cmp x12, #0
    cset w13, eq                // ctr == 0
    ubfx x14, x9, #1, #1
    eor w13, w13, w14
    eor w13, w13, #1
    and w15, w15, w13           // (ctr == 0) == BO[1]
1:
    tbnz x9, #4, 2f
    ldr w12, [_cpu, x10]
    tst w12, w11
    cset w13, ne
    ubfx x14, x9, #3, #1
    eor w13, w13, w14
    eor w13, w13, #1
    and w15, w15, w13           // bit == BO[3]
2:
.endm

// bc, any BO: [bo][crf][mask][taken][fall]
.gadget bc
    ldp x9, x10, [_ip]
    ldr x11, [_ip, #16]
    bo_cond
    cbz w15, 3f
    take 24
3:  take 32

// bclr, bcctr with a condition: [bo][crf][mask][fall]
.gadget bclr_c
    ldp x9, x10, [_ip]
    ldr x11, [_ip, #16]
    bo_cond
    cbz w15, 3f
    ldr x8, [_cpu, #CPU_ppc64_lr]
    b ppc64_jump_x8
3:  take 24
.gadget bcctr_c
    ldp x9, x10, [_ip]
    ldr x11, [_ip, #16]
    bo_cond
    cbz w15, 3f
    ldr x8, [_cpu, #CPU_ppc64_ctr]
    b ppc64_jump_x8
3:  take 24
// and with LK: [bo][crf][mask][link][fall]; LR is the link either way, and
// the target is the LR from before
.gadget bclrl_c
    ldp x9, x10, [_ip]
    ldr x11, [_ip, #16]
    ldr x8, [_cpu, #CPU_ppc64_lr]
    ldr x12, [_ip, #24]
    str x12, [_cpu, #CPU_ppc64_lr]
    mov x16, x8
    bo_cond
    mov x8, x16
    cbz w15, 3f
    b ppc64_jump_x8
3:  take 32
.gadget bcctrl_c
    ldp x9, x10, [_ip]
    ldr x11, [_ip, #16]
    ldr x12, [_ip, #24]
    str x12, [_cpu, #CPU_ppc64_lr]
    bo_cond
    cbz w15, 3f
    ldr x8, [_cpu, #CPU_ppc64_ctr]
    b ppc64_jump_x8
3:  take 32

// blr, bctr: no stream
.gadget blr
    ldr x8, [_cpu, #CPU_ppc64_lr]
    b ppc64_jump_x8
.gadget bctr
    ldr x8, [_cpu, #CPU_ppc64_ctr]
    b ppc64_jump_x8
// blrl, bctrl: [link]
.gadget blrl
    ldr x8, [_cpu, #CPU_ppc64_lr]
    ldr x9, [_ip]
    str x9, [_cpu, #CPU_ppc64_lr]
    b ppc64_jump_x8
.gadget bctrl
    ldr x9, [_ip]
    str x9, [_cpu, #CPU_ppc64_lr]
    ldr x8, [_cpu, #CPU_ppc64_ctr]
    b ppc64_jump_x8
