// ppc64 guest loads and stores. A little-endian guest's memory is the
// host's: no byte is reversed but by the byte-reversed instructions.
//
// Streams: [gadget][rt][ra][disp][pc] (D and DS forms) and [gadget][rt][ra]
// [rb][pc] (X forms), rt the loaded register or the stored one, ra the offset
// of ppc64_zero when the instruction's RA is 0 and reads as zero. The update
// forms write rA with the address after the access, so a fault leaves it as
// it was. pc is the instruction's address, last, as every memory gadget's is
// (core.S); a gadget moves _ip past it before its access.
//
// x19 holds a load's destination or a store's value across the prep (which
// may call C), x20 and x21 an update's address and rA.

#include "gadgets.h"

.text

.macro ea mode
    ldr x7, [_cpu, x9]
    .ifc \mode,x
    ldr x10, [_cpu, x10]
    .endif
    add x7, x7, x10
.endm

.macro nopost
.endm
.macro post_rev16
    rev16 w11, w11
.endm
.macro post_rev32
    rev w11, w11
.endm
.macro post_rev64
    rev x11, x11
.endm

.macro load name, bits, insn, reg, mode, upd, post=nopost
.gadget \name
    ldr x19, [_ip]
    ldp x9, x10, [_ip, #8]
    add _ip, _ip, #32
    ea \mode
    .if \upd
    mov x20, x7
    mov x21, x9
    .endif
    read_prep \bits, \name
    \insn \reg, [x7]
    \post
    str x11, [_cpu, x19]
    .if \upd
    str x20, [_cpu, x21]
    .endif
    gret
    read_bullshit \bits, \name
.endm

.macro pre_rev16
    rev16 w19, w19
.endm
.macro pre_rev32
    rev w19, w19
.endm
.macro pre_rev64
    rev x19, x19
.endm

.macro store name, bits, insn, reg, mode, upd, pre=nopost
.gadget \name
    ldr x11, [_ip]
    ldp x9, x10, [_ip, #8]
    add _ip, _ip, #32
    ldr x19, [_cpu, x11]
    \pre
    ea \mode
    .if \upd
    mov x20, x7
    mov x21, x9
    .endif
    write_prep \bits, \name
    \insn \reg, [x7]
    write_done \bits, \name
    .if \upd
    str x20, [_cpu, x21]
    .endif
    gret
    write_bullshit \bits, \name
.endm

// every size, sign and form: name, bits, host load
.macro loads base, bits, insn, reg
    load \base, \bits, \insn, \reg, d, 0
    load \base\()x, \bits, \insn, \reg, x, 0
    load \base\()u, \bits, \insn, \reg, d, 1
    load \base\()ux, \bits, \insn, \reg, x, 1
.endm
loads lbz, 8, ldrb, w11
loads lhz, 16, ldrh, w11
loads lha, 16, ldrsh, x11
loads lwz, 32, ldr, w11
loads lwa, 32, ldrsw, x11
loads ld, 64, ldr, x11
load lhbrx, 16, ldrh, w11, x, 0, post_rev16
load lwbrx, 32, ldr, w11, x, 0, post_rev32
load ldbrx, 64, ldr, x11, x, 0, post_rev64

.macro stores base, bits, insn, reg
    store \base, \bits, \insn, \reg, d, 0
    store \base\()x, \bits, \insn, \reg, x, 0
    store \base\()u, \bits, \insn, \reg, d, 1
    store \base\()ux, \bits, \insn, \reg, x, 1
.endm
stores stb, 8, strb, w19
stores sth, 16, strh, w19
stores stw, 32, str, w19
stores std, 64, str, x19
store sthbrx, 16, strh, w19, x, 0, pre_rev16
store stwbrx, 32, str, w19, x, 0, pre_rev32
store stdbrx, 64, str, x19, x, 0, pre_rev64

// ---- floating point -------------------------------------------------------------
// rt is the FPR's (or VSR's) high doubleword slot.
//
// lfs and stfs convert between a single and a double bit for bit, as
// POWER does (no rounding, a signalling NaN stays one): the host's fcvt for
// a finite value, where it is exact, and by hand for an infinity or a NaN.
.macro post_s2d
    ubfx w12, w11, #23, #8
    cmp w12, #0xff
    b.eq 1f
    fmov s0, w11
    fcvt d0, s0
    fmov x11, d0
    b 2f
1:  and x12, x11, #0x80000000
    lsl x12, x12, #32
    orr x12, x12, #0x7ff0000000000000
    ubfx x13, x11, #0, #23
    orr x11, x12, x13, lsl #29
2:
.endm
.macro pre_d2s
    ubfx x12, x19, #52, #11
    cmp w12, #0x7ff
    b.eq 1f
    fmov d0, x19
    fcvt s0, d0
    fmov w19, s0
    b 2f
1:  lsr x12, x19, #32
    and w12, w12, #0x80000000
    orr w12, w12, #0x7f800000
    ubfx x13, x19, #29, #23
    orr w19, w12, w13
2:
.endm
.macro post_sxtw
    sxtw x11, w11
.endm
.macro fps base, bits, insn, reg, post
    load \base, \bits, \insn, \reg, d, 0, \post
    load \base\()x, \bits, \insn, \reg, x, 0, \post
    load \base\()u, \bits, \insn, \reg, d, 1, \post
    load \base\()ux, \bits, \insn, \reg, x, 1, \post
.endm
fps lfd, 64, ldr, x11, nopost
fps lfs, 32, ldr, w11, post_s2d
load lfiwax, 32, ldr, w11, x, 0, post_sxtw
load lfiwzx, 32, ldr, w11, x, 0
.macro stfps base, bits, insn, reg, pre
    store \base, \bits, \insn, \reg, d, 0, \pre
    store \base\()x, \bits, \insn, \reg, x, 0, \pre
    store \base\()u, \bits, \insn, \reg, d, 1, \pre
    store \base\()ux, \bits, \insn, \reg, x, 1, \pre
.endm
stfps stfd, 64, str, x19, nopost
stfps stfs, 32, str, w19, pre_d2s
store stfiwx, 32, str, w19, x, 0

// ---- VSX and VMX ---------------------------------------------------------------
// A VSR is the host's 128-bit value, big-endian element 0 the most
// significant (emu/cpu.h): rt is the VSR's slot, all 16 bytes.
.macro vload name, align, post
.gadget \name
    ldr x19, [_ip]
    ldp x9, x10, [_ip, #8]
    add _ip, _ip, #32
    ea x
    .if \align
    and x7, x7, #~15
    .endif
    read_prep 128, \name
    ldr q0, [x7]
    \post
    str q0, [_cpu, x19]
    gret
    read_bullshit 128, \name
.endm
// The value is read after the prep: a TLB miss calls C, which keeps the
// general registers and not the vector ones (save_c), and a page fault's
// zero fill runs NEON stores (jit/guest-arm64's rule, "vector registers
// across C calls"). Read before it, the first store into a fresh page wrote
// zeros there.
.macro vstore name, align, pre
.gadget \name
    ldr x19, [_ip]
    ldp x9, x10, [_ip, #8]
    add _ip, _ip, #32
    ea x
    .if \align
    and x7, x7, #~15
    .endif
    write_prep 128, \name
    ldr q0, [_cpu, x19]
    \pre
    str q0, [x7]
    write_done 128, \name
    gret
    write_bullshit 128, \name
.endm
// lxvd2x: doubleword 0 from the lower address
.macro swapd
    ext v0.16b, v0.16b, v0.16b, #8
.endm
// lxvw4x: word 0 from the lowest
.macro swapw
    rev64 v0.4s, v0.4s
    ext v0.16b, v0.16b, v0.16b, #8
.endm
.macro vnone
.endm
vload lxvd2x, 0, swapd
vload lxvw4x, 0, swapw
vload lvx, 1, vnone
vstore stxvd2x, 0, swapd
vstore stxvw4x, 0, swapw
vstore stvx, 1, vnone
// lxvdsx: one doubleword, in both
.gadget lxvdsx
    ldr x19, [_ip]
    ldp x9, x10, [_ip, #8]
    add _ip, _ip, #32
    ea x
    read_prep 64, lxvdsx
    ld1r {v0.2d}, [x7]
    str q0, [_cpu, x19]
    gret
    read_bullshit 64, lxvdsx
// stvebx, stvehx, stvewx: the one element, where lvx would have put it
.macro vstore_elem name, bits, insn, reg, ld
.gadget \name
    ldr x11, [_ip]
    ldp x9, x10, [_ip, #8]
    add _ip, _ip, #32
    ea x
    .if \bits > 8
    and x7, x7, #~(\bits / 8 - 1)
    .endif
    and x12, x7, #15
    add x11, x11, x12
    \ld \reg, [_cpu, x11]
    write_prep \bits, \name
    \insn \reg, [x7]
    write_done \bits, \name
    gret
    write_bullshit \bits, \name
.endm
vstore_elem stvebx, 8, strb, w19, ldrb
vstore_elem stvehx, 16, strh, w19, ldrh
vstore_elem stvewx, 32, str, w19, ldr

// lvsl, lvsr: [vt][ra][rb]; no access. Big-endian byte i of lvsl's vector is
// sh + i (sh = EA & 15), so host byte j holds sh + 15 - j; lvsr's is
// 16 - sh + i, so host byte j holds 31 - sh - j.
.gadget lvsl
    ldp x19, x9, [_ip]
    ldr x10, [_ip, #16]
    ea x
    and w7, w7, #15
    adr x12, ppc64_bytes_down
    ldr q0, [x12]
    dup v1.16b, w7
    add v0.16b, v0.16b, v1.16b
    str q0, [_cpu, x19]
    gret 3
.gadget lvsr
    ldp x19, x9, [_ip]
    ldr x10, [_ip, #16]
    ea x
    and w7, w7, #15
    adr x12, ppc64_bytes_down
    ldr q0, [x12]
    mov w13, #16
    sub w7, w13, w7
    dup v1.16b, w7
    add v0.16b, v0.16b, v1.16b
    str q0, [_cpu, x19]
    gret 3
.balign 16
ppc64_bytes_down:
    .byte 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0

// ---- cache blocks, barriers ---------------------------------------------------
// dcbz: the 128-byte block zeroed. [ra][rb][pc]
.gadget dcbz
    ldp x9, x10, [_ip]
    add _ip, _ip, #24
    ea x
    and x7, x7, #~127
    write_prep 1024, dcbz
    .irp off, 0, 32, 64, 96
    stp xzr, xzr, [x7, #\off]
    stp xzr, xzr, [x7, #(\off + 16)]
    .endr
    write_done 1024, dcbz
    gret
    write_bullshit 1024, dcbz

// sync, lwsync, eieio: guest threads run on host threads, so a real barrier
.gadget sync
    dmb ish
    gret

// ---- larx / stcx. -----------------------------------------------------------
// [rt][ra][rb][pc]. The reservation is riscv64's design: the guest address,
// the value read and its size; stcx. succeeds if its address and size match
// and memory still holds the value, by one exclusive compare-and-store, and
// clears the reservation either way. A misaligned one is an alignment
// interrupt: SIGBUS, BUS_ADRALN.
.macro x_align bits, write
    .if \bits > 8
    tst x7, #(\bits / 8 - 1)
    b.eq 1f
    ldur x9, [_ip, #-8]
    str x9, [_cpu, #CPU_ppc64_pc]
    str x7, [_cpu, #CPU_segfault_addr]
    mov w9, #\write
    strb w9, [_cpu, #CPU_segfault_was_write]
    mov w0, #INT_ALIGN
    b jit_exit
1:
    .endif
.endm
.macro larx name, bits, insn, reg
.gadget \name
    ldr x19, [_ip]
    ldp x9, x10, [_ip, #8]
    add _ip, _ip, #32
    ea x
    x_align \bits, 0
    str x7, [_cpu, #CPU_ppc64_res_addr]
    mov w12, #(\bits / 8)
    str w12, [_cpu, #CPU_ppc64_res_size]
    read_prep \bits, \name
    \insn \reg, [x7]
    str x11, [_cpu, #CPU_ppc64_res_val]
    str x11, [_cpu, x19]
    gret
    read_bullshit \bits, \name
.endm
larx lbarx, 8, ldaxrb, w11
larx lharx, 16, ldaxrh, w11
larx lwarx, 32, ldaxr, w11
larx ldarx, 64, ldaxr, x11

.macro stcx name, bits, ldx, stx, exp, old, new
.gadget \name
    ldr x11, [_ip]
    ldp x9, x10, [_ip, #8]
    add _ip, _ip, #32
    ldr x20, [_cpu, x11]
    ea x
    x_align \bits, 1
    ldr x11, [_cpu, #CPU_ppc64_res_addr]
    cmp x7, x11
    b.ne 8f
    ldr w11, [_cpu, #CPU_ppc64_res_size]
    cmp w11, #(\bits / 8)
    b.ne 8f
    write_prep \bits, \name
    ldr x21, [_cpu, #CPU_ppc64_res_val]
1:  \ldx \old, [x7]
    cmp \old, \exp
    b.ne 7f
    \stx w13, \new, [x7]
    cbnz w13, 1b
    mov w11, #2                 // EQ: stored
    b 9f
7:  clrex
8:  mov w11, #0
9:  ldr w12, [_cpu, #CPU_ppc64_so]
    orr w11, w11, w12
    str w11, [_cpu, #CPU_ppc64_cr]
    mov x9, #-1
    str x9, [_cpu, #CPU_ppc64_res_addr]
    write_done \bits, \name
    gret
    write_bullshit \bits, \name
.endm
stcx stbcx, 8, ldaxrb, stlxrb, w21, w12, w20
stcx sthcx, 16, ldaxrh, stlxrh, w21, w12, w20
stcx stwcx, 32, ldaxr, stlxr, w21, w12, w20
stcx stdcx, 64, ldaxr, stlxr, x21, x12, x20
