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

.macro cmpxchg_set_flags s, ss
    # Entered straight after the CMPXCHG, with its host flags live and the
    # ORIGINAL accumulator in r15. CMPXCHG sets the flags of `cmp acc, mem`
    # (acc - mem) on both paths, and afterwards the accumulator holds the
    # ORIGINAL memory operand on both paths (on success it already equalled it).
    #
    # CF and OF come off the host. ZF/SF/PF/AF are seeded LAZILY from the two
    # compared values, exactly as a SUB seeds them and as the aarch64 host's
    # cmpxchg does (`setf_a w8, eax`). AF cannot come off the host: x86 has no
    # SETcc that reads it. This macro used to shift `seta` (CF==0 && ZF==0) into
    # bit 4 instead, so AF came out SET for every unequal compare that did not
    # borrow: `lock cmpxchgl` reported flags 0x010 where hardware gives 0x000,
    # and tests/manual/x86 atomic_cmpxchg32 failed 976 of 2000 vectors on two
    # independent x86_64 hosts. The data results were always right.
    setf_oc
    mov %rax, %r14
    mov %r15, %tmp
    setf_a src=%r14\s, dst=%tmp\s, ss=\ss
    sub\ss %r14\s, %tmp\s
    setf_zsp %tmp\s, \ss
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

# The flags are recorded BEFORE write_done, which clobbers the host flags and
# r14. That also keeps the host stack untouched here: these gadgets used to
# bracket write_done with pushf/popf, an odd push that left crosspage_store's
# call into C 8 bytes off the 16-byte alignment the ABI requires. r15 carries the
# original accumulator across the CMPXCHG (tmp is its source operand, and write_prep
# is done with r15 by now).
.macro do_cmpxchg size, s, ss
    .gadget cmpxchg\size\()_mem
        write_prep \size, cmpxchg\size\()_mem
        mov %rax, %r15
        cmpxchg\ss %tmp\s, (%_addrq)
        cmpxchg_set_flags \s, \ss
        write_done \size, cmpxchg\size\()_mem
        gret 1

    .gadget atomic_cmpxchg\size\()_mem
        write_prep \size, atomic_cmpxchg\size\()_mem
        mov %rax, %r15
        lock cmpxchg\ss %tmp\s, (%_addrq)
        cmpxchg_set_flags \s, \ss
        write_done \size, atomic_cmpxchg\size\()_mem
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

# [helper][arg][orig_ip], for an i386 C helper that works on the address the
# address gadgets left in _addr and may fault or #GP (i386_jit_sreg):
# helper(cpu, tlb, arg, addr) returns -1 to go on past the three words, or an
# interrupt to raise at orig_ip.
.gadget helper_addr_retint
    save_regs
    save_c
    movq %_cpu, %rdi
    leaq -TLB_entries(%_tlb), %rsi
    movq 8(%_ip), %rdx
    movl %_addr, %ecx
    callq *(%_ip)
    movq %rax, %r14
    restore_c
    load_regs
    cmpl $-1, %r14d
    je 0f
    movl %r14d, %_tmp
    movl 16(%_ip), %_eip
    jmp jit_exit
0:
    gret 3

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
    movw CPU_fsw(%_cpu), %ax
    gret
