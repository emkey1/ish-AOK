#include "gadgets.h"
#include "math.h"

.gadget cpuid
    # regrettable
    save_c
    sub sp, sp, 0x10
    str eax, [sp, 0xc]
    str ebx, [sp, 0x8]
    str ecx, [sp, 0x4]
    str edx, [sp, 0x0]
    add x0, sp, 0xc
    add x1, sp, 0x8
    add x2, sp, 0x4
    mov x3, sp
    bl NAME(helper_cpuid)
    ldr eax, [sp, 0xc]
    ldr ebx, [sp, 0x8]
    ldr ecx, [sp, 0x4]
    ldr edx, [sp, 0x0]
    add sp, sp, 0x10
    restore_c
    gret

.gadget xgetbv
    # same frame as cpuid: ecx in, eax/edx out
    save_c
    sub sp, sp, 0x10
    str eax, [sp, 0xc]
    str ebx, [sp, 0x8]
    str ecx, [sp, 0x4]
    str edx, [sp, 0x0]
    add x0, sp, 0xc
    add x1, sp, 0x8
    add x2, sp, 0x4
    mov x3, sp
    bl NAME(helper_xgetbv)
    ldr eax, [sp, 0xc]
    ldr ebx, [sp, 0x8]
    ldr ecx, [sp, 0x4]
    ldr edx, [sp, 0x0]
    add sp, sp, 0x10
    restore_c
    gret

.extern helper_trace_unaligned_atomic

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
        write_prep \size, atomic_cmpxchg\size\()_mem
        .if \size != 8
            tst _addr, (\size/8)-1
            b.eq 9f
            save_c
            mov x0, _cpu
            mov w1, _addr
            .if \size == 16
                mov w2, 1
            .else
                mov w2, 2
            .endif
            bl NAME(helper_trace_unaligned_atomic)
            restore_c
        9:
        .endif
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

.macro do_helper type, size=
    .gadget helper_\type\size
        .ifin(\type, read,write)
            \type\()_prep (\size), helper_\type\size
        .endifin
        save_regs
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
        load_regs
        .ifc \type,write
            write_done (\size), helper_\type\size
        .endif
        .ifc \type,0
            gret 1
        .else N .ifc \type,2
            gret 3
        .else
            gret 2
        .endif N .endif
        .ifc \type,read
            read_bullshit (\size), helper_\type\size
        .else N .ifc \type,write
            write_bullshit (\size), helper_\type\size
        .endif N .endif
.endm
do_helper 0
do_helper 1
do_helper 2
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

.gadget fstsw_ax
    ldrh w10, [_cpu, CPU_fsw]
    movs eax, w10, h
    gret
