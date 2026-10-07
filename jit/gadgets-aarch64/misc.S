#include "gadgets.h"
#include "emu/interrupt.h"
#include "math.h"

# RDTSC: EDX:EAX = the host counter in ns (gadgets-generic.h host_counter_ns).
.gadget rdtsc
    host_counter_ns x8, x9, x10, x11
    mov  eax, w8
    lsr  x8, x8, 32
    mov  edx, w8
    gret

.gadget cpuid
    ldr  x10, [_ip]                  /* cpuid_tables[0] */
    mov  w8, eax
    mov  w9, ecx
    bl   cpuid_lookup
    mov  eax, w11
    mov  ebx, w12
    mov  ecx, w13
    mov  edx, w14
    gret 1

# CPUID's answer for leaf w8, subleaf w9, from the table at x10 (struct
# cpuid_table, emu/cpuid.h): do_cpuid's choice of slot, in asm. Out: w11 eax,
# w12 ebx, w13 ecx, w14 edx. Clobbers x15. Shared with the amd64 gadget.
.global cpuid_lookup
cpuid_lookup:
    mov  w15, 0                      /* CPUID_SLOT_LEAF0 */
    cbz  w8, 9f
    mov  w15, 1
    cmp  w8, 1
    b.eq 9f
    cmp  w8, 7
    b.eq 7f
    cmp  w8, 0xd
    b.eq 8f
    mov  w15, 9                      /* CPUID_SLOT_EXT0 */
    mov  w11, 0x80000000
    cmp  w8, w11
    b.eq 9f
    mov  w15, 10
    add  w11, w11, 1
    cmp  w8, w11
    b.eq 9f
    mov  w15, 13                     /* any other extended leaf */
    tbnz w8, 31, 9f
    ldr  w11, [x10]                  /* leaf 0's eax: the basic maximum */
    mov  w15, 11
    cmp  w8, w11
    b.hi 9f
    mov  w15, 12                     /* below it, unimplemented: zeros */
    b    9f
7:  mov  w15, 12
    cbnz w9, 9f
    mov  w15, 2
    b    9f
8:  mov  w15, 12                     /* leaf 0xD: subleaves 0 1 2 5 6 7 */
    cmp  w9, 2
    b.hi 81f
    add  w15, w9, 3
    b    9f
81: cmp  w9, 7
    b.hi 9f
    cmp  w9, 5
    b.lo 9f
    add  w15, w9, 1
9:  add  x15, x10, x15, lsl 4
    ldp  w11, w12, [x15]
    ldp  w13, w14, [x15, 8]
    ret

.gadget xgetbv
    # [xcr0][orig_ip]: ECX selects the register, and XCR0 is the only one;
    # anything else is #GP at the instruction, as on hardware.
    cbnz ecx, 1f
    ldr  x8, [_ip]
    mov  eax, w8
    lsr  x8, x8, 32
    mov  edx, w8
    gret 2
1:  str  xzr, [_cpu, CPU_segfault_addr]
    strb wzr, [_cpu, CPU_segfault_was_write]
    strb wzr, [_cpu, CPU_segfault_reported]
    ldr  eip, [_ip, 8]
    mov  _tmp, INT_GPF
    b    jit_exit


.macro do_cmpxchg size, s
    .gadget cmpxchg\size\()_mem
        write_prep \size, cmpxchg\size\()_mem
        ldr\s w8, [_xaddr]
        setf_a w8, eax, \s
        mov w9, eax
        do_add sub, w9, w8, \s
        setf_zsp \s, val=w9
        .ifnb \s
            cmp w9, 0
            and w9, eax, (-1 << \size)
            orr w8, w8, w9
        .endif
        csel eax, w8, eax, ne
        csel w8, _tmp, w8, eq
        cset w9, eq
        str\s w8, [_xaddr]
        write_done \size, cmpxchg\size\()_mem
        gret 1
        write_bullshit \size, cmpxchg\size\()_mem

    .gadget atomic_cmpxchg\size\()_mem
        .if \size != 8   // on the guest address, before write_prep
            tst _addr, (\size/8)-1
            b.eq 9f
            ua_slow UA_CMPXCHG, \size, 1
        9:
        .endif
        write_prep \size, atomic_cmpxchg\size\()_mem
        dmb ish
        mov w12, eax
        ldr\s w11, [_xaddr]
    1:
        mov w8, w11
        setf_a w8, eax, \s
        mov w9, eax
        do_add sub, w9, w8, \s
        setf_zsp \s, val=w9
        .ifnb \s
            cmp w9, 0
            and w9, eax, (-1 << \size)
            orr w8, w8, w9
        .endif
        csel eax, w8, eax, ne
        csel w8, _tmp, w8, eq
        cset w9, eq

        # all that setf stuff writes to memory which means instead of just using
        # ldaxr and stlxr we now have to do *another* compare-and-exchange
    2:
        ldaxr\s w10, [_xaddr]
        cmp w10, w11
        b.ne 3f
        stlxr\s w10, w8, [_xaddr]
        cbnz w10, 2b

        dmb ish
        write_done \size, atomic_cmpxchg\size\()_mem
        gret 1
        write_bullshit \size, atomic_cmpxchg\size\()_mem
    3:
        dmb ish
        mov w11, w10
        mov eax, w12
        b 1b
.endm

.irp size, SIZE_LIST
    ss \size, do_cmpxchg
.endr
.gadget_array cmpxchg
.gadget_array atomic_cmpxchg

.extern segfault_write

.gadget atomic_cmpxchg8b
    #Test for alignment.
    tst _addr, 0x7
    b.ne 2f

    #cmpxchg8b via aligned exclusive 8b load
    write_prep 64, atomic_cmpxchg8b
    dmb ish
    mov w9, eax
    bfi x9, xdx, 32, 32
    mov w10, ebx
    bfi x10, xcx, 32, 32

1:
    ldaxr x8, [_xaddr]
    cmp x9, x8
    b.ne 3f                   // CAS fails: actual != expected, don't corrupt x9
    stlxr w12, x10, [_xaddr]  // CAS matches: try to store new value
    cbnz w12, 1b              // exclusive store failed: retry (x9 unchanged = original expected)
    mov w11, #1               // success
    b 4f
3:
    clrex                     // clear exclusive reservation
    mov x9, x8                // x9 = actual value (for edx:eax return)
    mov w11, wzr              // failure
4:
    dmb ish
    write_done 64, atomic_cmpxchg8b
    ubfx xax, x9, 0, 32
    ubfx xdx, x9, 32, 32

    ldr w8, [_cpu, CPU_flags_res]
    ldr w9, [_cpu, CPU_eflags]
    and w8, w8, ~ZF_RES
    bfi w9, w11, 6, 1
    str w8, [_cpu, CPU_flags_res]
    str w9, [_cpu, CPU_eflags]
    gret 1
    write_bullshit 64, atomic_cmpxchg8b

2:  # Unaligned LOCK CMPXCHG8B: the ARM exclusive load above needs 8-byte
    # alignment, but x86 permits unaligned LOCK operands (and the i386 ABI
    # aligns int64 to only 4 bytes, so glibc/Python 64-bit atomics land here).
    # Spill the host-cached GP regs, then run a software-atomic compare-exchange
    # under the global atomic lock via a C helper (cross-page + #PF safe).
    # Returns 0 = ok (reload EAX/EDX, continue), 1 = fault (segfault_write).
    str eax, [_cpu, CPU_eax]
    str ebx, [_cpu, CPU_ebx]
    str ecx, [_cpu, CPU_ecx]
    str edx, [_cpu, CPU_edx]
    save_c
    mov x0, _cpu
    sub x1, _tlb, TLB_entries
    mov w2, _addr
    bl NAME(helper_atomic_cmpxchg8b)
    mov x19, x0
    restore_c
    ldr eax, [_cpu, CPU_eax]
    ldr edx, [_cpu, CPU_edx]
    cbnz x19, 5f
    gret 1
5:
    b segfault_write


.gadget cmpxchg8b
    write_prep 64, cmpxchg8b
    mov w9, eax
    bfi x9, xdx, 32, 32
    mov w10, ebx
    bfi x10, xcx, 32, 32

    ldr x8, [_xaddr]
    cmp x9, x8
    csel x9, x8, x9, ne
    csel x8, x10, x8, eq
    cset w11, eq
    str x8, [_xaddr]
    write_done 64, cmpxchg8b
    ubfx xax, x9, 0, 32
    ubfx xdx, x9, 32, 32

    ldr w8, [_cpu, CPU_flags_res]
    ldr w9, [_cpu, CPU_eflags]
    and w8, w8, ~ZF_RES
    bfi w9, w11, 6, 1
    str w8, [_cpu, CPU_flags_res]
    str w9, [_cpu, CPU_eflags]
    gret 1
    write_bullshit 64, cmpxchg8b

.macro do_helper type, size=, pfx=helper, regs=1
    .gadget \pfx\()_\type\size
        .ifin(\type, read,write)
            \type\()_prep (\size), \pfx\()_\type\size
        .endifin
        .if \regs
            save_regs
        .endif
        save_c
        mov x0, _cpu
        .ifc \type,1
            ldr x1, [_ip, 8]
        .endif
        .ifc \type,2
            ldr x1, [_ip, 8]
            ldr x2, [_ip, 16]
        .endif
        .ifin(\type, read,write)
            mov x1, _xaddr
            ldr x8, [_ip, 8]
        .endifin
        .ifin(\type, 0,1,2)
            ldr x8, [_ip]
        .endifin
        blr x8
        restore_c
        .if \regs
            load_regs
        .endif
        .ifc \type,write
            write_done (\size), \pfx\()_\type\size
        .endif
        .ifc \type,0
            gret 1
        .else N .ifc \type,2
            gret 3
        .else
            gret 2
        .endif N .endif
        .ifc \type,read
            read_bullshit (\size), \pfx\()_\type\size
        .else N .ifc \type,write
            write_bullshit (\size), \pfx\()_\type\size
        .endif N .endif
.endm
do_helper 0
do_helper 1
do_helper 2
# The x87 helpers (emu/fpu.c) read and write only cpu->fp and the flags in
# struct cpu_state, never a guest general register, and the guest registers
# live in callee-saved x20-x28, so these skip the spill and reload. FNSTSW AX,
# the one x87 op that writes a GPR, has its own gadget (fstsw_ax).
do_helper 0,, fhelper, 0
do_helper 1,, fhelper, 0
do_helper 2,, fhelper, 0
# The FPU state-save/restore area widths (bits, so /8 gives bytes): FSTENV and
# FLDENV move 28 bytes, FNSAVE and FRSTOR 108, FXSAVE and FXRSTOR 512. These
# are fixed by the instruction, not by an operand-size prefix, and they are
# what read_prep/write_prep must check -- the four-byte check these used to get
# validated one dword and let the helper write the rest through it, past the
# end of the page and, on the crosspage path, past the end of the staging
# buffer. See jit/gen.c's h_read_bits/h_write_bits.
.irp size, SIZE_LIST,64,80,224,864,4096
    do_helper read, \size
    do_helper write, \size
    do_helper read, \size, fhelper, 0
    do_helper write, \size, fhelper, 0
.endr

.macro do_helper_tlb_retint type
    .gadget helper_tlb_\type\()_retint
        save_regs
        save_c
        mov x0, _cpu
        sub x1, _tlb, TLB_entries
        .ifc \type,1
            ldr x2, [_ip, 8]
        .endif
        .ifc \type,2
            ldr x2, [_ip, 8]
            ldr x3, [_ip, 16]
        .endif
        ldr x8, [_ip]
        blr x8
        mov x14, x0
        restore_c
        load_regs
        cmn w14, #1
        b.eq 0f
        mov x0, x14
        .ifc \type,0
            ldr eip, [_ip, 16]
        .endif
        .ifc \type,1
            ldr eip, [_ip, 24]
        .endif
        .ifc \type,2
            ldr eip, [_ip, 32]
        .endif
        b jit_exit
    0:
        .ifc \type,0
            gret 1
        .endif
        .ifc \type,1
            gret 2
        .endif
        .ifc \type,2
            gret 3
        .endif
.endm

do_helper_tlb_retint 0
do_helper_tlb_retint 1
do_helper_tlb_retint 2

.gadget helper_tlb_3_retint
    save_regs
    save_c
    mov x0, _cpu
    sub x1, _tlb, TLB_entries
    ldr x2, [_ip, 8]
    ldr x3, [_ip, 16]
    ldr x4, [_ip, 24]
    ldr x8, [_ip]
    blr x8
    mov x14, x0
    restore_c
    load_regs
    cmn w14, #1
    b.eq 0f
    mov x0, x14
    ldr eip, [_ip, 40]
    b jit_exit
0:
    gret 4

# [helper][arg][orig_ip], for an i386 C helper that works on the address the
# address gadgets left in _addr and may fault or #GP (i386_jit_sreg):
# helper(cpu, tlb, arg, addr) returns -1 to go on past the three words, or an
# interrupt to raise at orig_ip. _addr is already the fourth argument
# register; the mov clears its high half.
.gadget helper_addr_retint
    save_regs
    save_c
    mov x0, _cpu
    sub x1, _tlb, TLB_entries
    ldr x2, [_ip, 8]
    mov w3, _addr
    ldr x8, [_ip]
    blr x8
    mov x14, x0
    restore_c
    load_regs
    cmn w14, #1
    b.eq 0f
    mov x0, x14
    ldr eip, [_ip, 16]
    b jit_exit
0:
    gret 3

.macro do_vec_helper rm, _imm, size=
    .gadget vec_helper_\rm\size\_imm
        .ifin(\rm, read,write)
            \rm\()_prep (\size), vec_helper_\rm\size\_imm
        .endifin
        save_regs
        save_c
        mov x0, _cpu

        # the argument order should be a consistent src, dst
        .ifc \rm,reg
            # src
            ldrh w1, [_ip, 8]
            add x1, x0, x1
            # dst
            ldrh w2, [_ip, 10]
            add x2, x0, x2
        .endif
        .ifc \rm,read
            # src
            mov x1, _xaddr
            # dst
            ldrh w2, [_ip, 16]
            add x2, x0, x2
        .endif
        .ifc \rm,write
            # src
            ldrh w1, [_ip, 16]
            add x1, x0, x1
            # dst
            mov x2, _xaddr
        .endif
        .ifc \rm,imm
            # src
            ldrh w1, [_ip, 8]
            # dst
            ldrh w2, [_ip, 10]
            add x2, x0, x2
        .endif

        .ifc _imm,_imm
            # imm for third argument
            .ifin(\rm, reg)
                ldr w3, [_ip, 12]
            .endifin
            .ifin(\rm, read,write)
                ldr w3, [_ip, 20]
            .endifin
        .endif

        .ifin(\rm, read,write)
            ldr x8, [_ip, 8]
        .endifin
        .ifin(\rm, reg,imm)
            ldr x8, [_ip]
        .endifin
        // store-release: flush prior stores before the 64-bit write reaches memory
        .ifc \rm,write
            .ifc \size,64
                dmb ish
            .endif
        .endif
        blr x8
        // load-acquire: ensure the 64-bit load is visible before subsequent loads
        .ifc \rm,read
            .ifc \size,64
                dmb ish
            .endif
        .endif

        restore_c
        load_regs
        .ifc \rm,write
            write_done (\size), vec_helper_\rm\size\_imm
        .endif
        .ifin(\rm, reg,imm)
            gret 2
        .endifin
        .ifin(\rm, read,write)
            gret 3
        .endifin
        .ifc \rm,read
            read_bullshit (\size), vec_helper_\rm\size\_imm
        .else N .ifc \rm,write
            write_bullshit (\size), vec_helper_\rm\size\_imm
        .endif N .endif
.endm

.irp _imm, ,_imm
    .irp rm, reg,imm
        do_vec_helper \rm, \_imm
    .endr
    # 256 is for AVX (GH #525): a 256-bit memory operand, which is the widest
    # vector operand the i386 front end emits. The crosspage staging buffer
    # (jit/frame.h value) is 512 bytes and so is not the constraint here --
    # it is sized for FXSAVE, which is wider than any vector form.
    .irp size, SIZE_LIST,64,128,256
        do_vec_helper read, \_imm, \size
        do_vec_helper write, \_imm, \size
    .endr
.endr

# The commonest SSE moves without the C call (gen_vec): Alpine's i386 gcc
# moves 64-bit values through xmm (movd r32 -> xmm, punpckldq, movq xmm ->
# m64; movsd for doubles), ~7% of Python's instructions, and each was a
# vec_helper call with every register saved and reloaded around it.
# The xmm register is an offset from _cpu in the operand word.
#   vec_movd_to_<reg>: xmm = zero-extended reg            [gadget][xmm]
#   vec_movd_from_<reg>: reg = xmm's low dword            [gadget][xmm]
#   vec_ld32z / vec_ld64z: xmm = [addr] zero-extended     [gadget][orig ip][xmm]
#   vec_st8 / 16 / 32 / 64: [addr] = the slot's low byte .. qword  [gadget][orig ip][offset]
#   vec_copy64z: dst = src's low qword, zero-extended     [gadget][src | dst << 16]
# The 64-bit memory forms keep the helpers' dmb ish (before a store, after a
# load): i386 C11 atomics do 64-bit loads and stores with movq.
.macro vec_movd_pair name, reg
.gadget vec_movd_to_\name
    ldrh w8, [_ip]
    add x8, _cpu, x8
    str \reg, [x8]
    str wzr, [x8, 4]
    str xzr, [x8, 8]
    gret_nomem 1
.gadget vec_movd_from_\name
    ldrh w8, [_ip]
    ldr \reg, [_cpu, x8]
    gret_nomem 1
.endm
.each_reg vec_movd_pair
# MOVD mm, r32: the register zero-extended to the 8 bytes of an MMX register.
.macro vec_movd_mm_pair name, reg
.gadget vec_movd_mm_to_\name
    ldrh w8, [_ip]
    add x8, _cpu, x8
    str \reg, [x8]
    str wzr, [x8, 4]
    gret_nomem 1
.endm
.each_reg vec_movd_mm_pair
# A general register for the amd64 engine's vector gadgets, which take theirs
# from cpu->amd64_regs (an i386 task has no use for it): into slot S,
# zero-extended, or out of it (gen.c gen_i386_vec_mapped). [S], gret 1.
.macro vec_gpr_pair name, reg
.gadget vec_gpr_in_\name
    ldr x9, [_ip]
    mov w8, \reg
    add x10, _cpu, CPU_amd64_regs
    str x8, [x10, x9, lsl 3]
    gret_nomem 1
.gadget vec_gpr_out_\name
    ldr x9, [_ip]
    add x10, _cpu, CPU_amd64_regs
    add x10, x10, x9, lsl 3
    ldr \reg, [x10]
    gret_nomem 1
.endm
.each_reg vec_gpr_pair
# PCMPESTRx/PCMPISTRx run math.S's amd64_pcmp*_r, which take the lengths from
# amd64_regs[0] and [2] (rax, rdx), put an index in [1] (rcx), and use x24-x27
# -- esi, edi, ebp and esp here -- as scratch: enter parks those in slots 8-11
# and copies eax and edx in; leave puts them back (and _ecx copies ECX out).
# No operands.
.gadget vec_pcmpstr_enter
    add x10, _cpu, CPU_amd64_regs
    mov w8, eax
    str x8, [x10]
    mov w8, edx
    str x8, [x10, 16]
    stp x24, x25, [x10, 64]
    stp x26, x27, [x10, 80]
    gret_nomem
.gadget vec_pcmpstr_leave
    add x10, _cpu, CPU_amd64_regs
    ldp x24, x25, [x10, 64]
    ldp x26, x27, [x10, 80]
    gret_nomem
.gadget vec_pcmpstr_leave_ecx
    add x10, _cpu, CPU_amd64_regs
    ldp x24, x25, [x10, 64]
    ldp x26, x27, [x10, 80]
    ldr ecx, [x10, 8]
    gret_nomem
# Register-to-register moves of 16, 8 or 4 bytes, the rest of the destination
# kept (MOVAPS and its kind; MOVQ mm, MOVSD/MOVSS's register merge).
# [src | dst << 16], gret 1.
.gadget vec_copy128
    ldrh w8, [_ip]
    ldrh w9, [_ip, 2]
    ldr q0, [_cpu, x8]
    str q0, [_cpu, x9]
    gret_nomem 1
.gadget vec_copy64
    ldrh w8, [_ip]
    ldrh w9, [_ip, 2]
    ldr x10, [_cpu, x8]
    str x10, [_cpu, x9]
    gret_nomem 1
.gadget vec_copy32
    ldrh w8, [_ip]
    ldrh w9, [_ip, 2]
    ldr w10, [_cpu, x8]
    str w10, [_cpu, x9]
    gret_nomem 1
# [addr] = the 16 bytes at the cpu_state offset (MOVAPS/MOVNTDQ m128 and
# their kind). [orig_ip][offset], gret 2.
.gadget vec_st128
    write_prep 128, vec_st128
    ldrh w8, [_ip, 8]
    ldr q0, [_cpu, x8]
    str q0, [_xaddr]
    write_done 128, vec_st128
    gret 2
    write_bullshit 128, vec_st128
# MASKMOVQ / MASKMOVDQU: each byte of the data register whose byte in the
# mask register has its top bit set, to [edi + i]. [orig_ip][data | mask << 16],
# gret 2. (A byte store cannot cross a page: no staging.)
.macro vec_maskmov bytes
.gadget vec_maskmov\bytes
    ldrh w11, [_ip, 8]
    ldrh w12, [_ip, 10]
    add x11, _cpu, x11
    add x12, _cpu, x12
    mov w13, 0
1:  ldrb w14, [x12, x13]
    tbz w14, 7, 2f
    add _addr, edi, w13
    write_prep 8, vec_maskmov\bytes
    ldrb w14, [x11, x13]
    strb w14, [_xaddr]
    write_done 8, vec_maskmov\bytes
2:  add w13, w13, 1
    cmp w13, \bytes
    b.lo 1b
    gret 2
    write_bullshit 8, vec_maskmov\bytes
.endm
vec_maskmov 8
vec_maskmov 16

# MOVD mm, m32: zero-extended to 8 bytes. [orig_ip][offset], gret 2.
.gadget vec_ld32z8
    read_prep 32, vec_ld32z8
    ldr w9, [_xaddr]
    ldrh w8, [_ip, 8]
    add x8, _cpu, x8
    str x9, [x8]
    gret 2
    read_bullshit 32, vec_ld32z8

# The i386 guest's vector ops with a memory source run the amd64 engine's
# register-form gadgets (math.S) on a staged copy of the operand: these read
# it into the cpu_state slot at [_ip, 8] -- xmm15, which no i386 task has, or
# an MMX register parked in xmm15 by vec_mm_stash -- whole, whatever the op
# then reads of it. A fault leaves the slot unwritten. [orig_ip][offset],
# gret 2. (gen.c gen_vec, i386_vec_map.)
.macro vec_ldtmp bits
.gadget vec_ldtmp\bits
    read_prep \bits, vec_ldtmp\bits
    ldrh w8, [_ip, 8]
    add x8, _cpu, x8
.if \bits == 128
    ldr q0, [_xaddr]
    str q0, [x8]
.elseif \bits == 64
    ldr x9, [_xaddr]
    dmb ish
    str x9, [x8]
.elseif \bits == 32
    ldr w9, [_xaddr]
    str w9, [x8]
.elseif \bits == 16
    ldrh w9, [_xaddr]
    strh w9, [x8]
.else
    ldrb w9, [_xaddr]
    strb w9, [x8]
.endif
    gret 2
    read_bullshit \bits, vec_ldtmp\bits
.endm
vec_ldtmp 8
vec_ldtmp 16
vec_ldtmp 32
vec_ldtmp 64
vec_ldtmp 128

# MMX has no spare register: an op with a memory source parks mm[S] (S not the
# destination) in xmm15's low half, loads the operand into mm[S], runs the
# register form, and puts mm[S] back. Nothing between can fault but the load,
# which leaves mm[S] as it was. [S], gret 1.
.gadget vec_mm_stash
    ldr  x8, [_ip]
    add  x9, _cpu, CPU_mm
    ldr  x10, [x9, x8, lsl 3]
    add  x11, _cpu, CPU_xmm
    str  x10, [x11, 15 * 16]
    gret 1
.gadget vec_mm_unstash
    ldr  x8, [_ip]
    add  x11, _cpu, CPU_xmm
    ldr  x10, [x11, 15 * 16]
    add  x9, _cpu, CPU_mm
    str  x10, [x9, x8, lsl 3]
    gret 1

.gadget vec_ld32z
    read_prep 32, vec_ld32z
    ldr w9, [_xaddr]
    ldrh w8, [_ip, 8]
    add x8, _cpu, x8
    str w9, [x8]
    str wzr, [x8, 4]
    str xzr, [x8, 8]
    gret 2
    read_bullshit 32, vec_ld32z
.gadget vec_ld64z
    read_prep 64, vec_ld64z
    ldr x9, [_xaddr]
    dmb ish
    ldrh w8, [_ip, 8]
    add x8, _cpu, x8
    str x9, [x8]
    str xzr, [x8, 8]
    gret 2
    read_bullshit 64, vec_ld64z
.gadget vec_st8
    write_prep 8, vec_st8
    ldrh w8, [_ip, 8]
    ldrb w9, [_cpu, x8]
    strb w9, [_xaddr]
    write_done 8, vec_st8
    gret 2
    write_bullshit 8, vec_st8
.gadget vec_st16
    write_prep 16, vec_st16
    ldrh w8, [_ip, 8]
    ldrh w9, [_cpu, x8]
    strh w9, [_xaddr]
    write_done 16, vec_st16
    gret 2
    write_bullshit 16, vec_st16
.gadget vec_st32
    write_prep 32, vec_st32
    ldrh w8, [_ip, 8]
    ldr w9, [_cpu, x8]
    str w9, [_xaddr]
    write_done 32, vec_st32
    gret 2
    write_bullshit 32, vec_st32
.gadget vec_st64
    write_prep 64, vec_st64
    ldrh w8, [_ip, 8]
    ldr x9, [_cpu, x8]
    dmb ish
    str x9, [_xaddr]
    write_done 64, vec_st64
    gret 2
    write_bullshit 64, vec_st64
.gadget vec_copy64z
    ldrh w8, [_ip]
    ldrh w9, [_ip, 2]
    ldr x10, [_cpu, x8]
    add x9, _cpu, x9
    str x10, [x9]
    str xzr, [x9, 8]
    gret_nomem 1

.gadget fstsw_ax
    ldrh w10, [_cpu, CPU_fsw]
    movs eax, w10, h
    gret

// ISH_JIT_PROFILE (jit/jitprof.c): count one entry of this block, emitted
// first in every i386/amd64 block while profiling. Code stream: [&counter].
.gadget block_count
    ldr x9, [_ip], #8
    ldr x10, [x9]
    add x10, x10, #1
    str x10, [x9]
    gret
