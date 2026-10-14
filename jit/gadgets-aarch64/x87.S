#include "gadgets.h"
#include "amd64-mem.h"
#include "emu/interrupt.h"

# The x87, for the i386 and amd64 guests alike: the 80-bit arithmetic, the
# register stack and its tags, the status and control words, and every D8-DF
# instruction -- here, in integer arithmetic, rather than in C (emu/fpu.c and
# emu/float80.c are the interpreter's).
#
# The arithmetic, the stack, the register forms and the transcendentals are
# not this file: they are x87-arith.S, x87-stack.S, x87-reg.S and x87-trans.S,
# and what a memory form does once it has its operand is x87-mem.S's macros,
# whose contract (what a host gives them) is in x87-arith.S. They were
# shared with AOK VM's QEMU engine until 2026-10-14; it took copies, and
# each tree owns its own since. What is here is this engine's side of that
# contract, and the forms that reach guest memory.
#
# State: cpu->fp[8] (16 bytes each: the 64-bit significand, then sign and
# exponent), TOP in fsw bits 11-13, the abridged tag in cpu->x87_valid (bit i
# set when PHYSICAL register i holds a value), fcw. ST(i) is fp[(TOP + i) & 7].

.macro x87_ld_fsw r
    ldrh \r, [_cpu, CPU_fsw]
.endm
.macro x87_st_fsw r
    strh \r, [_cpu, CPU_fsw]
.endm
.macro x87_ld_valid r
    ldrb \r, [_cpu, CPU_x87_valid]
.endm
.macro x87_st_valid r
    strb \r, [_cpu, CPU_x87_valid]
.endm
# Where a memory form writes its result (x87-mem.S): x87m_wprep's pointer.
x87_wp .req x19
# FCOMI's flags, from w9 (ZF, PF and CF as EFLAGS has them): the lazy flags
# are made real, and OF, SF and AF cleared.
.macro x87_comi_flags

    ldr w10, [_cpu, CPU_eflags]
    mov w11, (ZF_FLAG | PF_FLAG | SF_FLAG | AF_FLAG | 1)
    orr w11, w11, (1 << 11)
    bic w10, w10, w11
    orr w10, w10, w9
    str w10, [_cpu, CPU_eflags]
    and w9, w9, 1
    strb w9, [_cpu, CPU_cf]
    strb wzr, [_cpu, CPU_of]
    strb wzr, [_cpu, CPU_flags_res]
.endm

#include "x87-arith.S"
#include "x87-stack.S"

# ---------------------------------------------------------------------------
# #MF. Every x87 instruction but FNINIT, FNCLEX, FNSTSW, FNSTCW, FNSTENV,
# FNSAVE, FXSAVE and FXRSTOR waits: with ES set -- an unmasked exception an
# earlier instruction left pending -- it faults before it runs, INT_MF at
# that instruction (kernel/calls.c makes it Linux's SIGFPE).
#
# A register form's word is [imm | flags << 8 | ip << 16]: bit 8, the amd64
# register cache is live (store it); bit 9, the guest is amd64; ip, the
# instruction. A memory form has its instruction in the stream already
# (i386's [orig_ip]) or in cpu_state (amd64's rip, flushed).
# ---------------------------------------------------------------------------
.macro x87_wait
    ldrh w8, [_cpu, CPU_fsw]
    tbz w8, 7, .Lw\@
    b x87_mf_rr                     // (out of a tbnz's reach from most gadgets)
.Lw\@:
.endm
.macro x87m_wait arch
    ldrh w8, [_cpu, CPU_fsw]
    tbz w8, 7, .Lw\@
    .ifc \arch,i386
        b x87_mf_m_i386
    .else
        b x87_mf_m_amd64
    .endif
.Lw\@:
.endm
x87_mf_rr:
    ldr x8, [_ip]
    tbz x8, 8, 1f
    add x9, _cpu, CPU_amd64_regs
    str x20, [x9, 0]
    str x22, [x9, 8]
    str x23, [x9, 16]
    str x21, [x9, 24]
    str x27, [x9, 32]
    str x26, [x9, 40]
    str x24, [x9, 48]
    str x25, [x9, 56]
1:  lsr x9, x8, 16
    tbz x8, 9, 2f
    str x9, [_cpu, CPU_amd64_rip]
2:  mov eip, w9
    mov x0, INT_MF
    b jit_exit
x87_mf_m_i386:
    ldr x8, [_ip]
    mov eip, w8
    mov x0, INT_MF
    b jit_exit
x87_mf_m_amd64:
    mov x0, INT_MF
    b jit_exit

#include "x87-reg.S"

# ---------------------------------------------------------------------------
# FCMOVcc ST(0), ST(i) [i]. The i386 guest's flags may still be lazy (in
# CPU_res, per flags_res); the amd64 guest's ZF and PF are in eflags.
# ---------------------------------------------------------------------------
.macro x87_flag_i386 dst, which
    .ifc \which,c
        ldrb \dst, [_cpu, CPU_cf]
        cmp \dst, 0
        cset \dst, ne
    .endif
    .ifc \which,z
        ldrb w9, [_cpu, CPU_flags_res]
        ldr w10, [_cpu, CPU_eflags]
        ubfx \dst, w10, 6, 1
        tbz w9, 1, .Lfz\@
        ldr w10, [_cpu, CPU_res]
        cmp w10, 0
        cset \dst, eq
.Lfz\@:
    .endif
    .ifc \which,p
        ldrb w9, [_cpu, CPU_flags_res]
        ldr w10, [_cpu, CPU_eflags]
        ubfx \dst, w10, 2, 1
        tbz w9, 0, .Lfp\@
        ldr w10, [_cpu, CPU_res]
        eor w10, w10, w10, lsr 4
        eor w10, w10, w10, lsr 2
        eor w10, w10, w10, lsr 1
        mvn w10, w10
        and \dst, w10, 1
.Lfp\@:
    .endif
.endm
.macro x87_flag_amd64 dst, which
    .ifc \which,c
        ldrb \dst, [_cpu, CPU_cf]
        cmp \dst, 0
        cset \dst, ne
    .endif
    .ifc \which,z
        ldr \dst, [_cpu, CPU_eflags]
        ubfx \dst, \dst, 6, 1
    .endif
    .ifc \which,p
        ldr \dst, [_cpu, CPU_eflags]
        ubfx \dst, \dst, 2, 1
    .endif
.endm

x87_fcmov_all x87_fcmov, x87_flag_i386
x87_fcmov_all amd64_x87_fcmov, x87_flag_amd64

# FNSTSW AX on amd64: the register cache is flushed, so straight to RAX.
.gadget amd64_x87_fnstsw_ax
    ldrh w8, [_cpu, CPU_fsw]
    add x9, _cpu, CPU_amd64_regs
    strh w8, [x9]
    gret_nomem

# ---------------------------------------------------------------------------
# Memory forms, one macro for both guests. i386: g_addr left the guest
# address in _addr and the stream is [orig_ip]. amd64: the stream is [meta]
# [disp][next_ip], the register cache and rip flushed. A read leaves the host
# pointer in x9; a write's is kept in x19 while the value is worked out, and
# the store and its cross-page flush come before any x87 state changes, so a
# fault leaves the instruction to run again from the start.
# ---------------------------------------------------------------------------
.macro x87m_gadget arch, name
    .ifc \arch,i386
        .gadget x87m_\name
    .else
        .gadget amd64_x87m_\name
    .endif
.endm
.macro x87m_ret arch
    .ifc \arch,i386
        gret 1
    .else
        gret 3
    .endif
.endm
.macro x87m_rprep arch, bits, id
    .ifc \arch,i386
        read_prep \bits, \id
        mov x9, _xaddr
    .else
        ldr x8, [_ip]
        ldr x9, [_ip, 8]
        amd64_vmem_addr
        amd64_vread_prep (\bits/8), \id
    .endif
.endm
.macro x87m_rtail arch, bits, id
    .ifc \arch,i386
        read_bullshit \bits, \id
    .else
        amd64_vread_bullshit (\bits/8), \id
    .endif
.endm
.macro x87m_wprep arch, bits, id
    .ifc \arch,i386
        write_prep \bits, \id
        mov x19, _xaddr
    .else
        ldr x8, [_ip]
        ldr x9, [_ip, 8]
        amd64_vmem_addr
        amd64_vwrite_prep (\bits/8), \id
        mov x19, x9
    .endif
.endm
# The store is done: flush a cross-page one. w7 and w15 ride in w12/w13,
# which the flush's C call preserves.
.macro x87m_wdone arch, bits, id
    mov w12, w7
    mov w13, w15
    .ifc \arch,i386
        mov _xaddr, x19
        write_done \bits, \id
    .else
        mov x9, x19
        amd64_vwrite_done (\bits/8), \id
    .endif
    mov w7, w12
    mov w15, w13
.endm
.macro x87m_wtail arch, bits, id
    .ifc \arch,i386
        write_bullshit \bits, \id
    .else
        amd64_vwrite_bullshit (\bits/8), \id
    .endif
.endm

x87_pop1_here

# ES and B in fsw w8 recomputed from its flags and the masks in w15: what
# FLDCW, FLDENV, FRSTOR, FXRSTOR and FNSTENV leave.
.macro x87_es_update
    mov w10, 0x8080
    bic w8, w8, w10
    and w11, w8, 0x3f
    bic w11, w11, w15
    cbz w11, .Les\@
    orr w8, w8, w10
.Les\@:
.endm

# Arithmetic and compares with a memory operand: ST(0) op m.
#include "x87-mem.S"

# The control and status words. FLDCW keeps the bits that exist (bit 6 reads
# as 1) and recomputes ES.
.macro x87m_control arch
x87m_gadget \arch, fldcw
    x87m_wait \arch
    x87m_rprep \arch, 16, \arch\()_fldcw
    ldrh w15, [x9]
    mov w8, 0x1f3f
    and w15, w15, w8
    orr w15, w15, 0x40
    strh w15, [_cpu, CPU_fcw]
    ldrh w8, [_cpu, CPU_fsw]
    x87_es_update
    strh w8, [_cpu, CPU_fsw]
    x87m_ret \arch
    x87m_rtail \arch, 16, \arch\()_fldcw
x87m_gadget \arch, fnstcw
    x87m_wprep \arch, 16, \arch\()_fnstcw
    ldrh w0, [_cpu, CPU_fcw]
    strh w0, [x19]
    x87m_wdone \arch, 16, \arch\()_fnstcw
    x87m_ret \arch
    x87m_wtail \arch, 16, \arch\()_fnstcw
x87m_gadget \arch, fnstsw
    x87m_wprep \arch, 16, \arch\()_fnstsw
    ldrh w0, [_cpu, CPU_fsw]
    strh w0, [x19]
    x87m_wdone \arch, 16, \arch\()_fnstsw
    x87m_ret \arch
    x87m_wtail \arch, 16, \arch\()_fnstsw
.endm

# w0 = the full tag word: per physical register 11 empty, 01 zero, 10 special
# (NaN, infinity, denormal, unsupported), 00 valid. Clobbers x8-x14.
x87_fulltag:
    ldrb w8, [_cpu, CPU_x87_valid]
    mov w0, 0
    mov w9, 7
1:  lsl w0, w0, 2
    lsr w10, w8, w9
    mov w11, 3
    tbz w10, 0, 2f
    add x12, _cpu, CPU_fp
    add x12, x12, x9, lsl 4
    ldr x13, [x12]
    ldrh w14, [x12, 8]
    x87_class w11, x13, w14, w10
    cmp w11, 1
    mov w11, 0
    b.eq 2f
    mov w11, 1
    b.lo 2f
    mov w11, 2
2:  orr w0, w0, w11
    subs w9, w9, 1
    b.ge 1b
    ret

# A full tag word in w0 -> x87_valid (11 is empty, anything else a value).
x87_tag_load:
    mov w8, 0
    mov w9, 0
1:  lsr w10, w0, w9
    and w10, w10, 3
    cmp w10, 3
    cset w10, ne
    lsr w11, w9, 1
    lsl w10, w10, w11
    orr w8, w8, w10
    add w9, w9, 2
    cmp w9, 16
    b.lo 1b
    strb w8, [_cpu, CPU_x87_valid]
    ret

# The environment, from x9: control, status (ES recomputed), tag. \w: 4 for
# the 32-bit format (fields a dword apart), 2 for the 16-bit one. Then the
# eight registers, ST(0) first, from \regs on (0: none).
.macro x87_env_load w, regs
    ldrh w15, [x9]
    mov w8, 0x1f3f
    and w15, w15, w8
    orr w15, w15, 0x40
    strh w15, [_cpu, CPU_fcw]
    ldrh w8, [x9, \w]
    x87_es_update
    strh w8, [_cpu, CPU_fsw]
    ldrh w0, [x9, 2*\w]
    mov x16, x9
    bl x87_tag_load
    .if \regs
        mov x9, x16
        ldrh w8, [_cpu, CPU_fsw]
        ubfx w8, w8, 11, 3
        mov w10, 0
1:      add w11, w10, w8
        and w11, w11, 7
        add x12, _cpu, CPU_fp
        add x12, x12, x11, lsl 4
        mov w13, 10
        madd x13, x10, x13, x9
        ldr x14, [x13, \regs]
        str x14, [x12]
        add x13, x13, \regs
        ldrh w14, [x13, 8]
        strh w14, [x12, 8]
        add w10, w10, 1
        cmp w10, 8
        b.lo 1b
    .endif
.endm
# ... and stored to x19. The 32-bit format's high halves of control, status,
# tag and FDS read as FFFF; the instruction and data pointers and the opcode
# are not kept (they are 0).
.macro x87_env_store w, regs
    bl x87_fulltag
    ldrh w8, [_cpu, CPU_fcw]
    ldrh w9, [_cpu, CPU_fsw]
    .if \w == 4
        mov w10, 0xffff0000
        orr w8, w8, w10
        orr w9, w9, w10
        orr w0, w0, w10
        str w8, [x19]
        str w9, [x19, 4]
        str w0, [x19, 8]
        str xzr, [x19, 12]
        str wzr, [x19, 20]
        str w10, [x19, 24]
    .else
        strh w8, [x19]
        strh w9, [x19, 2]
        strh w0, [x19, 4]
        str xzr, [x19, 6]
    .endif
    .if \regs
        ldrh w8, [_cpu, CPU_fsw]
        ubfx w8, w8, 11, 3
        mov w10, 0
1:      add w11, w10, w8
        and w11, w11, 7
        add x12, _cpu, CPU_fp
        add x12, x12, x11, lsl 4
        mov w13, 10
        madd x13, x10, x13, x19
        ldr x14, [x12]
        str x14, [x13, \regs]
        ldrh w14, [x12, 8]
        add x13, x13, \regs
        strh w14, [x13, 8]
        add w10, w10, 1
        cmp w10, 8
        b.lo 1b
    .endif
.endm

.macro x87m_env arch
x87m_gadget \arch, fldenv32
    x87m_wait \arch
    x87m_rprep \arch, 224, \arch\()_fldenv32
    x87_env_load 4, 0
    x87m_ret \arch
    x87m_rtail \arch, 224, \arch\()_fldenv32
x87m_gadget \arch, fldenv16
    x87m_wait \arch
    x87m_rprep \arch, 112, \arch\()_fldenv16
    x87_env_load 2, 0
    x87m_ret \arch
    x87m_rtail \arch, 112, \arch\()_fldenv16
x87m_gadget \arch, frstor32
    x87m_wait \arch
    x87m_rprep \arch, 864, \arch\()_frstor32
    x87_env_load 4, 28
    x87m_ret \arch
    x87m_rtail \arch, 864, \arch\()_frstor32
x87m_gadget \arch, frstor16
    x87m_wait \arch
    x87m_rprep \arch, 752, \arch\()_frstor16
    x87_env_load 2, 14
    x87m_ret \arch
    x87m_rtail \arch, 752, \arch\()_frstor16
# FNSTENV then masks every exception (and so ES clears); FNSAVE then
# initializes, as FNINIT.
x87m_gadget \arch, fnstenv32
    x87m_wprep \arch, 224, \arch\()_fnstenv32
    x87_env_store 4, 0
    x87m_wdone \arch, 224, \arch\()_fnstenv32
    b \arch\()_x87_after_fnstenv
    x87m_wtail \arch, 224, \arch\()_fnstenv32
x87m_gadget \arch, fnstenv16
    x87m_wprep \arch, 112, \arch\()_fnstenv16
    x87_env_store 2, 0
    x87m_wdone \arch, 112, \arch\()_fnstenv16
    b \arch\()_x87_after_fnstenv
    x87m_wtail \arch, 112, \arch\()_fnstenv16
x87m_gadget \arch, fnsave32
    x87m_wprep \arch, 864, \arch\()_fnsave32
    x87_env_store 4, 28
    x87m_wdone \arch, 864, \arch\()_fnsave32
    b \arch\()_x87_after_fnsave
    x87m_wtail \arch, 864, \arch\()_fnsave32
x87m_gadget \arch, fnsave16
    x87m_wprep \arch, 752, \arch\()_fnsave16
    x87_env_store 2, 14
    x87m_wdone \arch, 752, \arch\()_fnsave16
    b \arch\()_x87_after_fnsave
    x87m_wtail \arch, 752, \arch\()_fnsave16
.endm

# The tails of FNSTENV and FNSAVE, after the store: one per guest, for its
# stream length.
.macro x87_after arch
\arch\()_x87_after_fnstenv:
    ldrh w15, [_cpu, CPU_fcw]
    orr w15, w15, 0x3f
    strh w15, [_cpu, CPU_fcw]
    ldrh w8, [_cpu, CPU_fsw]
    x87_es_update
    strh w8, [_cpu, CPU_fsw]
    x87m_ret \arch
\arch\()_x87_after_fnsave:
    mov w8, 0x37f
    strh w8, [_cpu, CPU_fcw]
    strh wzr, [_cpu, CPU_fsw]
    strb wzr, [_cpu, CPU_x87_valid]
    x87m_ret \arch
.endm

# FXSAVE: the 512-byte area -- control, status, the abridged tag, no
# opcode or pointers (AMD keeps them only with an exception pending), MXCSR
# with the host's flags folded in as STMXCSR does, MXCSR_MASK, the registers
# in stack order padded with zeros, and the XMM registers the mode has. The
# rest of the area is not written, as on hardware.
.macro x87m_fxsave arch, nxmm
x87m_gadget \arch, fxsave
    .ifc \arch,i386
        tst _addr, 15                   // the area must be 16-byte aligned: #GP
        b.ne x87_i386_gp
    .endif
    .ifc \arch,amd64
        // A straddling area is staged, and amd64's write staging starts
        // blank: read the area first (which stages it, contents and all), so
        // the bytes FXSAVE leaves alone go back as they were.
        x87m_rprep \arch, 4096, \arch\()_fxsave_r
    .endif
    x87m_wprep \arch, 4096, \arch\()_fxsave
    ldrh w8, [_cpu, CPU_fcw]
    strh w8, [x19]
    ldrh w8, [_cpu, CPU_fsw]
    strh w8, [x19, 2]
    ldrb w8, [_cpu, CPU_x87_valid]
    strh w8, [x19, 4]
    strh wzr, [x19, 6]
    stp xzr, xzr, [x19, 8]
    mrs x13, fpsr
    ldr w12, [_cpu, CPU_mxcsr]
    and w14, w13, 1                     /* IOC -> IE */
    orr w12, w12, w14
    ubfx w14, w13, 1, 1                 /* DZC -> ZE */
    orr w12, w12, w14, lsl 2
    ubfx w14, w13, 2, 1                 /* OFC -> OE */
    orr w12, w12, w14, lsl 3
    ubfx w14, w13, 3, 1                 /* UFC -> UE */
    orr w12, w12, w14, lsl 4
    .ifc \arch,i386
        ubfx w10, w12, 15, 1            /* a flush to zero (FTZ) is inexact */
        and w10, w10, w14
        orr w12, w12, w10, lsl 5
    .endif
    ubfx w14, w13, 4, 1                 /* IXC -> PE */
    orr w12, w12, w14, lsl 5
    ubfx w14, w13, 7, 1                 /* IDC -> DE */
    orr w12, w12, w14, lsl 1
    str w12, [_cpu, CPU_mxcsr]
    msr fpsr, xzr
    str w12, [x19, 24]
    mov w8, 0xffff
    str w8, [x19, 28]
    ldrh w8, [_cpu, CPU_fsw]
    ubfx w8, w8, 11, 3
    mov w10, 0
1:  add w11, w10, w8
    and w11, w11, 7
    add x12, _cpu, CPU_fp
    add x12, x12, x11, lsl 4
    add x13, x19, x10, lsl 4
    ldr x14, [x12]
    ldrh w9, [x12, 8]
    stp x14, x9, [x13, 32]
    add w10, w10, 1
    cmp w10, 8
    b.lo 1b
    add x12, _cpu, CPU_xmm
    mov w10, 0
2:  ldr q0, [x12, x10, lsl 4]
    add x13, x19, x10, lsl 4
    str q0, [x13, 160]
    add w10, w10, 1
    cmp w10, \nxmm
    b.lo 2b
    mov w7, 0
    mov w15, 0
    x87m_wdone \arch, 4096, \arch\()_fxsave
    x87m_ret \arch
    x87m_wtail \arch, 4096, \arch\()_fxsave
    .ifc \arch,amd64
        x87m_rtail \arch, 4096, \arch\()_fxsave_r
    .endif
.endm

# FXRSTOR: MXCSR's reserved bits are #GP before anything changes; then all
# of it back, ES recomputed, MXCSR's rounding (and for i386, FTZ, which its
# SSE runs on the host) installed in FPCR and its flags out of FPSR.
.macro x87m_fxrstor arch, nxmm
x87m_gadget \arch, fxrstor
    .ifc \arch,i386
        tst _addr, 15                   // the area must be 16-byte aligned: #GP
        b.ne x87_i386_gp
    .endif
    x87m_rprep \arch, 4096, \arch\()_fxrstor
    ldr w12, [x9, 24]
    lsr w13, w12, 16
    cbnz w13, 9f
    ldrh w15, [x9]
    mov w8, 0x1f3f
    and w15, w15, w8
    orr w15, w15, 0x40
    strh w15, [_cpu, CPU_fcw]
    ldrh w8, [x9, 2]
    x87_es_update
    strh w8, [_cpu, CPU_fsw]
    ldrb w10, [x9, 4]
    strb w10, [_cpu, CPU_x87_valid]
    ubfx w8, w8, 11, 3
    mov w10, 0
1:  add w11, w10, w8
    and w11, w11, 7
    add x13, _cpu, CPU_fp
    add x13, x13, x11, lsl 4
    add x14, x9, x10, lsl 4
    ldr x0, [x14, 32]
    str x0, [x13]
    ldrh w0, [x14, 40]
    strh w0, [x13, 8]
    add w10, w10, 1
    cmp w10, 8
    b.lo 1b
    add x13, _cpu, CPU_xmm
    mov w10, 0
2:  add x14, x9, x10, lsl 4
    ldr q0, [x14, 160]
    str q0, [x13, x10, lsl 4]
    add w10, w10, 1
    cmp w10, \nxmm
    b.lo 2b
    str w12, [_cpu, CPU_mxcsr]
    msr fpsr, xzr
    ubfx w13, w12, 13, 1
    ubfx w14, w12, 14, 1
    orr w13, w14, w13, lsl 1
    mrs x14, fpcr
    bic x14, x14, (7 << 22)             /* RMode and FZ */
    orr x14, x14, x13, lsl 22
    .ifc \arch,i386
        ubfx w13, w12, 15, 1
        orr x14, x14, x13, lsl 24
    .endif
    msr fpcr, x14
    x87m_ret \arch
9:  str xzr, [_cpu, CPU_segfault_addr]
    strb wzr, [_cpu, CPU_segfault_was_write]
    strb wzr, [_cpu, CPU_segfault_reported]
    .ifc \arch,i386
        ldr x8, [_ip]
        mov eip, w8
    .endif
    mov x0, INT_GPF
    b jit_exit
    x87m_rtail \arch, 4096, \arch\()_fxrstor
.endm

.irp arch, i386, amd64
    x87m_arith_all \arch, f32, 32, x87_ld_f32
    x87m_arith_all \arch, f64, 64, x87_ld_f64
    x87m_arith_all \arch, i16, 16, x87_ld_int
    x87m_arith_all \arch, i32, 32, x87_ld_int
    x87m_loads \arch
    x87m_stores \arch
    x87m_control \arch
    x87m_env \arch
    x87_after \arch
.endr
x87m_fxsave i386, 8
x87m_fxsave amd64, 16
x87m_fxrstor i386, 8
x87m_fxrstor amd64, 16

# ---- XSAVE / XRSTOR (0F AE /4 /5, memory; REX.W's forms differ only in the
# x87 pointers, which are not kept): the standard format at emu/cpuid.h's
# offsets, by emu/xsave.h's rules -- RFBM = EDX:EAX & XCR0; XSAVE writes the
# requested components and their XSTATE_BV bits (XINUSE, from the values),
# and MXCSR for SSE or AVX; XRSTOR #GPs on XSTATE_BV outside XCR0, header
# bytes 8-23 not zero or (SSE or AVX asked for) MXCSR's reserved bits, then
# loads each requested component XSTATE_BV has and initializes the others.
# The area is 64-byte aligned (#GP otherwise) and under 4K, so at most two
# pages, translated first -- every fault before anything is written or
# restored -- and no 64-byte chunk straddles them. Outside 64-bit mode
# (i386) XMM8-15 and their upper halves are not accessed, nor ZMM16-31.
# i386: [orig_ip], gret 1; amd64: [meta][disp][next_ip], gret 3.
#
# After xs_pages: x10 the host address of the area's offset 0 as page one
# has it, x11 as page two has it, x12 the first offset on page two; w13 RFBM.
.macro xs_ptr dst, off                  /* \dst = the host address of offset \off (a register) */
    cmp  \off, x12
    csel \dst, x10, x11, lo
    add  \dst, \dst, \off
.endm
.macro xs_rfbm arch                     /* w13 = RFBM. Clobbers w15 */
    mov  w15, XCR0_SUPPORTED_
    .ifc \arch,i386
        and  w13, eax, w15
    .else
        ldr  w13, [_cpu, CPU_amd64_regs]
        and  w13, w13, w15
    .endif
.endm
# w14 = the end of the bytes XSAVE/XRSTOR touch for RFBM w13 (\rd: XRSTOR,
# which reads the legacy area and header whatever RFBM). Clobbers w15.
.macro xs_end arch, rd
    mov  w14, 576
    tst  w13, XCR0_YMM_
    mov  w15, (XSAVE_YMM_OFFSET_ + XSAVE_YMM_SIZE_)
    csel w14, w15, w14, ne
    tst  w13, XCR0_OPMASK_
    mov  w15, (XSAVE_OPMASK_OFFSET_ + XSAVE_OPMASK_SIZE_)
    csel w14, w15, w14, ne
    tst  w13, XCR0_ZMM_HI256_
    mov  w15, (XSAVE_ZMM_HI_OFFSET_ + XSAVE_ZMM_HI_SIZE_)
    csel w14, w15, w14, ne
    .ifc \arch,amd64
        tst  w13, XCR0_HI16_ZMM_
        mov  w15, (XSAVE_HI16_OFFSET_ + XSAVE_HI16_SIZE_)
        csel w14, w15, w14, ne
    .endif
.endm
# Translate the area's pages for \type (read or write): the one with offset
# \first (0 or 512) and, if the bytes up to xs_end's cross into the next,
# that one. The guest address is in _addr (i386) or [meta][disp] (amd64).
.macro xs_pages arch, type, id, rd
    .ifc \arch,i386
        mov  w11, _addr                 /* (x0, x11-x13 survive the preps and their slow paths) */
        tst  w11, 63
        b.ne xs_gp_i386
        xs_rfbm i386
        mov  w12, 0
        .if \rd == 0
            tst  w13, (XCR0_X87_ | XCR0_SSE_ | XCR0_YMM_)
            mov  w15, 512
            csel w12, w15, w12, eq
        .endif
        add  _addr, w11, w12
        \type\()_prep 512, xs1_\id
        .ifc \type,write
            write_done 512, xs1_\id
        .endif
        sub  x0, _xaddr, x12            /* page one's offset 0 */
        xs_rfbm i386
        xs_end i386, \rd
        and  w12, w11, 0xfff
        mov  w15, 0x1000
        sub  w13, w15, w12              /* the first offset on page two */
        cmp  w14, w13
        b.ls 1f
        sub  w12, w14, 64
        add  _addr, w11, w12
        \type\()_prep 512, xs2_\id
        .ifc \type,write
            write_done 512, xs2_\id
        .endif
        sub  x11, _xaddr, x12           /* page two's offset 0 */
        b    2f
1:      mov  x11, x0
2:      mov  w12, w13
    .else
        ldr  x8, [_ip]
        ldr  x9, [_ip, 8]
        amd64_vmem_addr
        mov  x10, x9                    /* (x0, x3 and x10 survive the preps and their slow paths) */
        tst  x10, 63
        b.ne xs_gp_amd64
        xs_rfbm amd64
        mov  w0, 0
        .if \rd == 0
            tst  w13, (XCR0_X87_ | XCR0_SSE_ | XCR0_YMM_)
            mov  w15, 512
            csel w0, w15, w0, eq
        .endif
        add  x9, x10, x0
        amd64_v\type\()_prep 64, xs1_\id
        sub  x0, x9, x0                 /* page one's offset 0 */
        xs_rfbm amd64
        xs_end amd64, \rd
        and  x12, x10, 0xfff
        mov  x15, 0x1000
        sub  x3, x15, x12               /* the first offset on page two */
        cmp  x14, x3
        b.ls 1f
        sub  x14, x14, 64
        add  x9, x10, x14
        mov  x10, x14
        amd64_v\type\()_prep 64, xs2_\id
        sub  x11, x9, x10               /* page two's offset 0 */
        b    2f
1:      mov  x11, x0
2:      mov  x12, x3
    .endif
    mov  x10, x0
    xs_rfbm \arch
.endm
.macro xs_tails arch, type, id
    .ifc \arch,i386
        \type\()_bullshit 512, xs1_\id
        \type\()_bullshit 512, xs2_\id
    .else
        amd64_v\type\()_bullshit 64, xs1_\id
        amd64_v\type\()_bullshit 64, xs2_\id
    .endif
.endm

# w8 = MXCSR with the flags the host has raised since folded in (stored
# back, FPSR cleared), as STMXCSR and FXSAVE make it. Clobbers x9, x14, x15.
.macro x87_mxcsr_fold arch
    mrs  x15, fpsr
    ldr  w8, [_cpu, CPU_mxcsr]
    and  w14, w15, 1                    /* IOC -> IE */
    orr  w8, w8, w14
    ubfx w14, w15, 1, 1                 /* DZC -> ZE */
    orr  w8, w8, w14, lsl 2
    ubfx w14, w15, 2, 1                 /* OFC -> OE */
    orr  w8, w8, w14, lsl 3
    ubfx w14, w15, 3, 1                 /* UFC -> UE */
    orr  w8, w8, w14, lsl 4
    .ifc \arch,i386
        ubfx w9, w8, 15, 1              /* a flush to zero (FTZ) is inexact */
        and  w9, w9, w14
        orr  w8, w8, w9, lsl 5
    .endif
    ubfx w14, w15, 4, 1                 /* IXC -> PE */
    orr  w8, w8, w14, lsl 5
    ubfx w14, w15, 7, 1                 /* IDC -> DE */
    orr  w8, w8, w14, lsl 1
    str  w8, [_cpu, CPU_mxcsr]
    msr  fpsr, xzr
.endm
# MXCSR = w8 (checked), its rounding (and for i386, FTZ) into FPCR, FPSR
# cleared, as FXRSTOR and LDMXCSR leave it. Clobbers x9, x14.
.macro x87_mxcsr_set arch
    str  w8, [_cpu, CPU_mxcsr]
    msr  fpsr, xzr
    ubfx w9, w8, 13, 1
    ubfx w14, w8, 14, 1
    orr  w9, w14, w9, lsl 1
    mrs  x14, fpcr
    bic  x14, x14, (7 << 22)            /* RMode and FZ */
    orr  x14, x14, x9, lsl 22
    .ifc \arch,i386
        ubfx w9, w8, 15, 1
        orr  x14, x14, x9, lsl 24
    .endif
    msr  fpcr, x14
.endm

# w8 = 1 if the \n bytes of cpu_state at \off are not all zero. Clobbers
# x14, x15, v0, v1.
.macro xs_nz off, n
    mov  x15, \off
    add  x14, _cpu, x15
    mov  x15, \n
    movi v0.16b, 0
.Lxsnz\@:
    ldr  q1, [x14], 16
    orr  v0.16b, v0.16b, v1.16b
    subs x15, x15, 16
    b.ne .Lxsnz\@
    umaxv b0, v0.16b
    fmov w8, s0
    cmp  w8, 0
    cset w8, ne
.endm
# w16 = XINUSE (emu/xsave.h): which components are not in their initial
# configuration, over the \nxmm registers the mode has (i386's JIT stages
# operands in xmm15, which 32-bit code cannot name). Clobbers x8, x14, x15,
# w17, v0, v1.
.macro xs_xinuse nxmm
    ldrh w8, [_cpu, CPU_fcw]
    ldrb w15, [_cpu, CPU_x87_valid]
    mov  w14, 0x37f
    eor  w8, w8, w14
    ldrh w14, [_cpu, CPU_fsw]
    orr  w8, w8, w14
    orr  w8, w8, w15
    cmp  w8, 0
    cset w16, ne
    xs_nz CPU_xmm, (16 * \nxmm)
    orr  w16, w16, w8, lsl 1
    xs_nz CPU_ymm_hi, (16 * \nxmm)
    orr  w16, w16, w8, lsl 2
    xs_nz CPU_avx512_k, 64
    orr  w16, w16, w8, lsl 5
    xs_nz CPU_zmm_hi, (32 * \nxmm)
    orr  w16, w16, w8, lsl 6
    .if \nxmm == 8
        .exitm
    .endif
    xs_nz CPU_xmm_ext, 256
    mov  w17, w8
    xs_nz (CPU_ymm_hi + 256), 256
    orr  w17, w17, w8
    xs_nz (CPU_zmm_hi + 512), 512
    orr  w17, w17, w8
    orr  w16, w16, w17, lsl 7
.endm

# The \n 16-byte units from cpu_state offset \coff (stride \cs) to the area
# at \aoff (stride \as) -- or back with \back, or zeroed with \zero.
# Clobbers x0, x8, x9, x15, x17, v0 (not x16: XRSTOR's components to load).
.macro xs_copy aoff, as, coff, cs, n, back=0, zero=0
    mov  x0, \aoff
    mov  x17, \coff
    mov  w9, \n
.Lxsc\@:
    xs_ptr x15, x0
    .if \zero
        add  x8, _cpu, x17
        stp  xzr, xzr, [x8]
    .elseif \back
        ldr  q0, [x15]
        str  q0, [_cpu, x17]
    .else
        ldr  q0, [_cpu, x17]
        str  q0, [x15]
    .endif
    add  x0, x0, \as
    add  x17, x17, \cs
    subs w9, w9, 1
    b.ne .Lxsc\@
.endm

.macro x87m_xsave arch, nxmm
x87m_gadget \arch, xsave
    xs_pages \arch, write, \arch\()_xsave, 0
    /* x87: bytes 0-23 and 32-159 */
    tbz  w13, 0, 3f
    mov  x16, 0
    xs_ptr x15, x16
    ldrh w8, [_cpu, CPU_fcw]
    strh w8, [x15]
    ldrh w8, [_cpu, CPU_fsw]
    strh w8, [x15, 2]
    ldrb w8, [_cpu, CPU_x87_valid]
    strh w8, [x15, 4]
    strh wzr, [x15, 6]
    stp  xzr, xzr, [x15, 8]
    ldrh w8, [_cpu, CPU_fsw]
    ubfx w8, w8, 11, 3
    mov  w16, 0
1:  add  w17, w16, w8
    and  w17, w17, 7
    add  x9, _cpu, CPU_fp
    add  x9, x9, x17, lsl 4
    ldr  x14, [x9]
    ldrh w9, [x9, 8]
    lsl  x17, x16, 4
    add  x17, x17, 32
    xs_ptr x15, x17
    stp  x14, x9, [x15]
    add  w16, w16, 1
    cmp  w16, 8
    b.lo 1b
3:  /* MXCSR and its mask, for SSE or AVX */
    tst  w13, (XCR0_SSE_ | XCR0_YMM_)
    b.eq 4f
    x87_mxcsr_fold \arch
    mov  x16, 0
    xs_ptr x15, x16
    str  w8, [x15, 24]
    mov  w8, 0xffff
    str  w8, [x15, 28]
4:  tbz  w13, 1, 5f
    xs_copy 160, 16, CPU_xmm, 16, \nxmm
5:  /* the header's XSTATE_BV: the requested bits are XINUSE's */
    xs_xinuse \nxmm
    mov  x17, XSAVE_LEGACY_SIZE_
    xs_ptr x15, x17
    ldr  x8, [x15]
    bic  x8, x8, x13
    and  w16, w16, w13
    orr  x8, x8, x16
    str  x8, [x15]
    tbz  w13, 2, 6f
    xs_copy XSAVE_YMM_OFFSET_, 16, CPU_ymm_hi, 16, \nxmm
6:  tbz  w13, 5, 7f
    xs_copy XSAVE_OPMASK_OFFSET_, 16, CPU_avx512_k, 16, 4
7:  tbz  w13, 6, 8f
    xs_copy XSAVE_ZMM_HI_OFFSET_, 16, CPU_zmm_hi, 16, (2 * \nxmm)
8:
    .ifc \arch,amd64
        tbz  w13, 7, 9f
        xs_copy XSAVE_HI16_OFFSET_, 64, CPU_xmm_ext, 16, 16
        xs_copy (XSAVE_HI16_OFFSET_ + 16), 64, (CPU_ymm_hi + 256), 16, 16
        xs_copy (XSAVE_HI16_OFFSET_ + 32), 64, (CPU_zmm_hi + 512), 32, 16
        xs_copy (XSAVE_HI16_OFFSET_ + 48), 64, (CPU_zmm_hi + 512 + 16), 32, 16
    .endif
9:  x87m_ret \arch
    xs_tails \arch, write, \arch\()_xsave
.endm

.macro x87m_xrstor arch, nxmm
x87m_gadget \arch, xrstor
    xs_pages \arch, read, \arch\()_xrstor, 1
    /* the checks, before anything changes */
    mov  x16, XSAVE_LEGACY_SIZE_
    xs_ptr x15, x16
    ldp  x16, x17, [x15]                /* XSTATE_BV, XCOMP_BV */
    ldr  x8, [x15, 16]
    orr  x8, x8, x17
    mov  x17, XCR0_SUPPORTED_
    bic  x17, x16, x17
    orr  x8, x8, x17
    cbnz x8, xs_gp_\arch
    and  w16, w16, w13                  /* the components to load; the rest of RFBM initializes */
    mov  x17, 0
    xs_ptr x15, x17
    ldr  w8, [x15, 24]
    tst  w13, (XCR0_SSE_ | XCR0_YMM_)
    b.eq 1f
    lsr  w9, w8, 16
    cbnz w9, xs_gp_\arch
    x87_mxcsr_set \arch
1:  /* x87 */
    tbz  w13, 0, 4f
    tbz  w16, 0, 3f
    mov  x17, 0
    xs_ptr x15, x17
    ldrb w9, [x15, 4]
    strb w9, [_cpu, CPU_x87_valid]
    ldrh w8, [x15]
    mov  w9, 0x1f3f
    and  w8, w8, w9
    orr  w8, w8, 0x40
    strh w8, [_cpu, CPU_fcw]
    and  w17, w8, 0x3f                  /* the masks */
    ldrh w8, [x15, 2]
    mov  w9, 0x8080                     /* ES and B: an unmasked exception pending */
    bic  w8, w8, w9
    and  w14, w8, 0x3f
    bic  w14, w14, w17
    cbz  w14, 11f
    orr  w8, w8, w9
11: strh w8, [_cpu, CPU_fsw]
    ubfx w8, w8, 11, 3
    mov  w14, 0
2:  lsl  x17, x14, 4
    add  x17, x17, 32
    xs_ptr x15, x17
    ldr  x17, [x15]
    ldrh w15, [x15, 8]
    add  w9, w14, w8
    and  w9, w9, 7
    add  x9, _cpu, x9, lsl 4
    add  x9, x9, CPU_fp
    str  x17, [x9]
    strh w15, [x9, 8]
    add  w14, w14, 1
    cmp  w14, 8
    b.lo 2b
    b    4f
3:  mov  w8, 0x37f                      /* initialized */
    strh w8, [_cpu, CPU_fcw]
    strh wzr, [_cpu, CPU_fsw]
    strb wzr, [_cpu, CPU_x87_valid]
    add  x9, _cpu, CPU_fp
    mov  w14, 8
5:  stp  xzr, xzr, [x9], 16
    subs w14, w14, 1
    b.ne 5b
4:  tbz  w13, 1, 6f
    tbz  w16, 1, 5f
    xs_copy 160, 16, CPU_xmm, 16, \nxmm, back=1
    b    6f
5:  xs_copy 160, 16, CPU_xmm, 16, \nxmm, zero=1
6:  tbz  w13, 2, 7f
    tbz  w16, 2, 5f
    xs_copy XSAVE_YMM_OFFSET_, 16, CPU_ymm_hi, 16, \nxmm, back=1
    b    7f
5:  xs_copy XSAVE_YMM_OFFSET_, 16, CPU_ymm_hi, 16, \nxmm, zero=1
7:  tbz  w13, 5, 8f
    tbz  w16, 5, 5f
    xs_copy XSAVE_OPMASK_OFFSET_, 16, CPU_avx512_k, 16, 4, back=1
    b    8f
5:  xs_copy XSAVE_OPMASK_OFFSET_, 16, CPU_avx512_k, 16, 4, zero=1
8:  tbz  w13, 6, 9f
    tbz  w16, 6, 5f
    xs_copy XSAVE_ZMM_HI_OFFSET_, 16, CPU_zmm_hi, 16, (2 * \nxmm), back=1
    b    9f
5:  xs_copy XSAVE_ZMM_HI_OFFSET_, 16, CPU_zmm_hi, 16, (2 * \nxmm), zero=1
9:
    .ifc \arch,amd64
        tbz  w13, 7, 10f
        tbz  w16, 7, 5f
        xs_copy XSAVE_HI16_OFFSET_, 64, CPU_xmm_ext, 16, 16, back=1
        xs_copy (XSAVE_HI16_OFFSET_ + 16), 64, (CPU_ymm_hi + 256), 16, 16, back=1
        xs_copy (XSAVE_HI16_OFFSET_ + 32), 64, (CPU_zmm_hi + 512), 32, 16, back=1
        xs_copy (XSAVE_HI16_OFFSET_ + 48), 64, (CPU_zmm_hi + 512 + 16), 32, 16, back=1
        b    10f
5:      xs_copy XSAVE_HI16_OFFSET_, 64, CPU_xmm_ext, 16, 16, zero=1
        xs_copy (XSAVE_HI16_OFFSET_ + 16), 64, (CPU_ymm_hi + 256), 16, 16, zero=1
        xs_copy (XSAVE_HI16_OFFSET_ + 32), 64, (CPU_zmm_hi + 512), 32, 16, zero=1
        xs_copy (XSAVE_HI16_OFFSET_ + 48), 64, (CPU_zmm_hi + 512 + 16), 32, 16, zero=1
10:
    .endif
    x87m_ret \arch
    xs_tails \arch, read, \arch\()_xrstor
.endm

x87m_xsave i386, 8
x87m_xsave amd64, 16
x87m_xrstor i386, 8
x87m_xrstor amd64, 16

xs_gp_i386:
    b    x87_i386_gp
xs_gp_amd64:
    str  xzr, [_cpu, CPU_segfault_addr]
    strb wzr, [_cpu, CPU_segfault_was_write]
    strb wzr, [_cpu, CPU_segfault_reported]
    mov  x0, INT_GPF
    b    jit_exit

# #GP(0) at the i386 instruction ([orig_ip] in the stream).
x87_i386_gp:
    str xzr, [_cpu, CPU_segfault_addr]
    strb wzr, [_cpu, CPU_segfault_was_write]
    strb wzr, [_cpu, CPU_segfault_reported]
    ldr x8, [_ip]
    mov eip, w8
    mov x0, INT_GPF
    b jit_exit

# EMMS: every x87 register empty.
.gadget x87_emms
    x87_wait
    strb wzr, [_cpu, CPU_x87_valid]
    gret_nomem 1

# What every MMX instruction but EMMS does to the x87 (the MMX registers are
# the x87's: emu/cpu.h CPU_MMX), in two gadgets around the instruction's own.
# Before: a pending unmasked x87 exception (#MF, nothing changed) -- mmx_wait,
# [x87 word], gret 1. After, so that an instruction whose memory operand
# faults changes none of it, as on the hardware: TOP 0, all eight registers
# valid, and the one it wrote (0-7; 8 for none) an all-ones sign and exponent
# -- mmx_touch, [register], gret 1.
.gadget mmx_wait
    x87_wait
    gret_nomem 1
.gadget mmx_touch
    ldrh w8, [_cpu, CPU_fsw]
    and  w8, w8, ~0x3800
    strh w8, [_cpu, CPU_fsw]
    mov  w8, 0xff
    strb w8, [_cpu, CPU_x87_valid]
    ldr  x9, [_ip]
    cmp  x9, 8
    b.hs 1f
    add  x10, _cpu, CPU_fp
    add  x10, x10, x9, lsl 4
    mov  w8, 0xffff
    strh w8, [x10, 8]
1:  gret_nomem 1

#include "x87-trans.S"
