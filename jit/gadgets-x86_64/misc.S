#include "gadgets.h"

.gadget cpuid
    # regrettable
    push %rsi
    push %rdi
    push %r8
    push %r9
    push %r10
    push %r11
    subq $0x10, %rsp
    movl %eax, 0xc(%rsp)
    movl %ebx, 0x8(%rsp)
    movl %ecx, 0x4(%rsp)
    movl %edx, 0x0(%rsp)
    leaq 0xc(%rsp), %rdi
    leaq 0x8(%rsp), %rsi
    leaq 0x4(%rsp), %rdx
    leaq 0x0(%rsp), %rcx
    call NAME(helper_cpuid)
    movl 0xc(%rsp), %eax
    movl 0x8(%rsp), %ebx
    movl 0x4(%rsp), %ecx
    movl 0x0(%rsp), %edx
    addq $0x10, %rsp
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rdi
    pop %rsi
    gret

.macro cmpxchg_set_flags
    setf_oc
    # god help us
    setp %r10b
    seta %r13b
    setz %r14b
    sets %r15b
    shlb DOLLAR(2), %r10b
    shlb DOLLAR(4), %r13b
    shlb DOLLAR(6), %r14b
    shlb DOLLAR(7), %r15b
    orb %r10b, %r15b
    orb %r13b, %r15b
    orb %r14b, %r15b
    andl $~(PF_RES|ZF_RES|SF_RES|AF_OPS), CPU_flags_res(%_cpu)
    movb %r15b, CPU_eflags(%_cpu)
.endm


.gadget xgetbv
    # same frame as cpuid: ecx in, eax/edx out
    push %rsi
    push %rdi
    push %r8
    push %r9
    push %r10
    push %r11
    subq $0x10, %rsp
    movl %eax, 0xc(%rsp)
    movl %ebx, 0x8(%rsp)
    movl %ecx, 0x4(%rsp)
    movl %edx, 0x0(%rsp)
    leaq 0xc(%rsp), %rdi
    leaq 0x8(%rsp), %rsi
    leaq 0x4(%rsp), %rdx
    leaq 0x0(%rsp), %rcx
    call NAME(helper_xgetbv)
    movl 0xc(%rsp), %eax
    movl 0x8(%rsp), %ebx
    movl 0x4(%rsp), %ecx
    movl 0x0(%rsp), %edx
    addq $0x10, %rsp
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rdi
    pop %rsi
    gret


.macro do_cmpxchg size, s, ss
    .gadget cmpxchg\size\()_mem
        write_prep \size, cmpxchg\size\()_mem
        cmpxchg\ss %tmp\s, (%_addrq)
        pushf
        write_done \size, cmpxchg\size\()_mem
        popf
        cmpxchg_set_flags
        gret 1

    .gadget atomic_cmpxchg\size\()_mem
        write_prep \size, atomic_cmpxchg\size\()_mem
        lock cmpxchg\ss %tmp\s, (%_addrq)
        pushf
        write_done \size, atomic_cmpxchg\size\()_mem
        popf
        cmpxchg_set_flags
        gret 1
.endm

.irp size, SIZE_LIST
    ss \size, do_cmpxchg
.endr
.gadget_array cmpxchg
.gadget_array atomic_cmpxchg

.gadget atomic_cmpxchg8b
    write_prep 64, atomic_cmpxchg8b
    lock cmpxchg8b (%_addrq)
    setz %r15b
    write_done 64, atomic_cmpxchg8b
    andl $~ZF_RES, CPU_flags_res(%_cpu)
    andl $~ZF_FLAG, CPU_eflags(%_cpu)
    shlb $6, %r15b
    orb %r15b, CPU_eflags(%_cpu)
    gret 1

.gadget cmpxchg8b
    write_prep 64, cmpxchg8b
    cmpxchg8b (%_addrq)
    setz %r15b
    write_done 64, cmpxchg8b
    andl $~ZF_RES, CPU_flags_res(%_cpu)
    andl $~ZF_FLAG, CPU_eflags(%_cpu)
    shlb $6, %r15b
    orb %r15b, CPU_eflags(%_cpu)
    gret 1

.macro do_helper type, size=
    .gadget helper_\type\size
        .ifin(\type, read,write)
            \type\()_prep (\size), helper_\type\size
        .endifin
        save_regs
        save_c
        movq %_cpu, %rdi
        .ifc \type,1
            movq 8(%_ip), %rsi
        .endif
        .ifc \type,2
            movq 8(%_ip), %rsi
            movq 16(%_ip), %rdx
        .endif
        .ifin(\type, read,write)
            movq %_addrq, %rsi
            callq *8(%_ip)
        .endifin
        .ifin(\type, 0,1,2)
            callq *(%_ip)
        .endifin
        restore_c
        load_regs
        .ifc \type,write
            write_done (\size), helper_\type\size
        .endif
        .ifc \type,0
            gret 1
        .else; .ifc \type,2
            gret 3
        .else
            gret 2
        .endif; .endif
.endm
do_helper 0
do_helper 1
do_helper 2
.irp size, SIZE_LIST,64,80
    do_helper read, \size
    do_helper write, \size
.endr

.macro do_helper_tlb_retint type
    .gadget helper_tlb_\type\()_retint
        save_regs
        save_c
        movq %_cpu, %rdi
        leaq -TLB_entries(%_tlb), %rsi
        .ifc \type,1
            movq 8(%_ip), %rdx
        .endif
        .ifc \type,2
            movq 8(%_ip), %rdx
            movq 16(%_ip), %rcx
        .endif
        callq *(%_ip)
        movq %rax, %r14
        restore_c
        load_regs
        cmpl $-1, %r14d
        je 0f
        movl %r14d, %_tmp
        .ifc \type,0
            movl 16(%_ip), %_eip
        .endif
        .ifc \type,1
            movl 24(%_ip), %_eip
        .endif
        .ifc \type,2
            movl 32(%_ip), %_eip
        .endif
        jmp jit_exit
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
    movq %_cpu, %rdi
    leaq -TLB_entries(%_tlb), %rsi
    movq 8(%_ip), %rdx
    movq 16(%_ip), %rcx
    movq 24(%_ip), %r8
    callq *(%_ip)
    movq %rax, %r14
    restore_c
    load_regs
    cmpl $-1, %r14d
    je 0f
    movl %r14d, %_tmp
    movl 40(%_ip), %_eip
    jmp jit_exit
0:
    gret 4

.macro do_vec_helper rm, _imm, size=
    .gadget vec_helper_\rm\size\_imm
        .ifin(\rm, read,write)
            \rm\()_prep (\size), vec_helper_\rm\size\_imm
        .endifin
        save_regs
        save_c
        movq %_cpu, %rdi
        xorq %r14, %r14

        # the argument order should be a consistent src, dst
        .ifc \rm,reg
            # src
            movw 8(%_ip), %r14w
            leaq (%_cpu,%r14), %rsi
            # dst
            movw 10(%_ip), %r14w
            leaq (%_cpu,%r14), %rdx
        .endif
        .ifc \rm,read
            # src
            movq %_addrq, %rsi
            # dst
            movw 16(%_ip), %r14w
            leaq (%_cpu,%r14), %rdx
        .endif
        .ifc \rm,write
            # src
            movw 16(%_ip), %r14w
            leaq (%_cpu,%r14), %rsi
            # dst
            movq %_addrq, %rdx
        .endif
        .ifc \rm,imm
            # src
            movw 8(%_ip), %si
            # dst
            movw 10(%_ip), %r14w
            leaq (%_cpu,%r14), %rdx
        .endif

        .ifc _imm,_imm
            # imm for third argument
            .ifin(\rm, reg)
                movl 12(%_ip), %ecx
            .endifin
            .ifin(\rm, read,write)
                movl 20(%_ip), %ecx
            .endifin
        .endif

        .ifin(\rm, read,write)
            callq *8(%_ip)
        .endifin
        .ifin(\rm, reg,imm)
            callq *(%_ip)
        .endifin

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
.endm

.irp _imm, ,_imm
    .irp rm, reg,imm
        do_vec_helper \rm, \_imm
    .endr
    # 256 is for AVX (GH #525): a 256-bit memory operand. The crosspage
    # staging buffer (jit/frame.h value[4]) is exactly 32 bytes, so this is
    # the widest access these gadgets can stage.
    .irp size, SIZE_LIST,64,128,256
        do_vec_helper read, \_imm, \size
        do_vec_helper write, \_imm, \size
    .endr
.endr

.gadget fstsw_ax
    movw CPU_fsw(%_cpu), %ax
    gret
