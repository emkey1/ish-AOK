#include "gadgets.h"

.gadget load32_addr
    movl %_addr, %_tmp
    gret

.gadget load16_gs
    movw CPU_gs(%_cpu), %r10w
    gret

.gadget store16_gs
    movw %r10w, CPU_gs(%_cpu)
    gret

.gadget amd64_set_rip
    movq (%_ip), %r10
    movq %r10, CPU_amd64_rip(%_cpu)
    movl %r10d, CPU_eip(%_cpu)
    gret 1

.gadget amd64_mov64_reg_reg
    movq (%_ip), %r10
    movq %r10, %r11
    andq $0xf, %r10
    shrq $4, %r11
    andq $0xf, %r11
    movq CPU_amd64_regs(%_cpu,%r10,8), %r14
    movq %r14, CPU_amd64_regs(%_cpu,%r11,8)
    gret 1

.gadget amd64_mov32_reg_reg
    movq (%_ip), %r10
    movq %r10, %r11
    andq $0xf, %r10
    shrq $4, %r11
    andq $0xf, %r11
    movl CPU_amd64_regs(%_cpu,%r10,8), %r14d
    movq %r14, CPU_amd64_regs(%_cpu,%r11,8)
    gret 1

.gadget amd64_mov16_reg_reg
    movq (%_ip), %r10
    movq %r10, %r11
    andq $0xf, %r10
    shrq $4, %r11
    andq $0xf, %r11
    movw CPU_amd64_regs(%_cpu,%r10,8), %r14w
    movw %r14w, CPU_amd64_regs(%_cpu,%r11,8)
    gret 1

.gadget amd64_mov8_reg_reg
    movq (%_ip), %r10
    movq %r10, %r11
    andq $0xf, %r10
    shrq $4, %r11
    andq $0xf, %r11
    movb CPU_amd64_regs(%_cpu,%r10,8), %r14b
    movb %r14b, CPU_amd64_regs(%_cpu,%r11,8)
    gret 1

.gadget amd64_mov_imm_reg
    movq (%_ip), %r10
    movq 8(%_ip), %r11
    movq %r10, %r14
    andq $0xf, %r10
    shrq $4, %r14
    andq $0x7f, %r14
    cmpq $64, %r14
    je 1f
    cmpq $32, %r14
    je 2f
    movw %r11w, CPU_amd64_regs(%_cpu,%r10,8)
    gret 2
1:
    movq %r11, CPU_amd64_regs(%_cpu,%r10,8)
    gret 2
2:
    movl %r11d, %r11d
    movq %r11, CPU_amd64_regs(%_cpu,%r10,8)
    gret 2

.gadget amd64_mov_imm8_reg
    movq (%_ip), %r10
    movq 8(%_ip), %r11
    andq $0xf, %r10
    movb %r11b, CPU_amd64_regs(%_cpu,%r10,8)
    gret 2

.gadget amd64_xchg_rax_reg
    movq (%_ip), %r10
    movq %r10, %r11
    andq $0xf, %r10
    shrq $4, %r11
    andq $0x7f, %r11
    movq CPU_amd64_regs(%_cpu), %r14
    movq CPU_amd64_regs(%_cpu,%r10,8), %r15
    cmpq $64, %r11
    je 1f
    cmpq $32, %r11
    je 2f
    movw %r15w, CPU_amd64_regs(%_cpu)
    movw %r14w, CPU_amd64_regs(%_cpu,%r10,8)
    gret 1
1:
    movq %r15, CPU_amd64_regs(%_cpu)
    movq %r14, CPU_amd64_regs(%_cpu,%r10,8)
    gret 1
2:
    movl %r15d, %r15d
    movl %r14d, %r14d
    movq %r15, CPU_amd64_regs(%_cpu)
    movq %r14, CPU_amd64_regs(%_cpu,%r10,8)
    gret 1

.gadget amd64_xchg_reg_reg
    movq (%_ip), %r10
    movq %r10, %r11
    movq %r10, %r14
    andq $0xf, %r10
    shrq $4, %r11
    andq $0xf, %r11
    shrq $8, %r14
    andq $0x7f, %r14
    cmpq $64, %r14
    je 1f
    cmpq $32, %r14
    je 2f
    cmpq $16, %r14
    je 3f
    movb CPU_amd64_regs(%_cpu,%r10,8), %r14b
    movb CPU_amd64_regs(%_cpu,%r11,8), %r15b
    movb %r15b, CPU_amd64_regs(%_cpu,%r10,8)
    movb %r14b, CPU_amd64_regs(%_cpu,%r11,8)
    gret 1
1:
    movq CPU_amd64_regs(%_cpu,%r10,8), %r14
    movq CPU_amd64_regs(%_cpu,%r11,8), %r15
    movq %r15, CPU_amd64_regs(%_cpu,%r10,8)
    movq %r14, CPU_amd64_regs(%_cpu,%r11,8)
    gret 1
2:
    movl CPU_amd64_regs(%_cpu,%r10,8), %r14d
    movl CPU_amd64_regs(%_cpu,%r11,8), %r15d
    movq %r15, CPU_amd64_regs(%_cpu,%r10,8)
    movq %r14, CPU_amd64_regs(%_cpu,%r11,8)
    gret 1
3:
    movw CPU_amd64_regs(%_cpu,%r10,8), %r14w
    movw CPU_amd64_regs(%_cpu,%r11,8), %r15w
    movw %r15w, CPU_amd64_regs(%_cpu,%r10,8)
    movw %r14w, CPU_amd64_regs(%_cpu,%r11,8)
    gret 1

.gadget amd64_sign_extend
    movq (%_ip), %r10
    movq %r10, %r11
    andq $0xff, %r10
    shrq $8, %r11
    andq $0x7f, %r11
    movq CPU_amd64_regs(%_cpu), %r14
    cmpq $0x99, %r10
    je 3f
    cmpq $64, %r11
    je 1f
    cmpq $32, %r11
    je 2f
    movsbl %r14b, %r15d
    movw %r15w, CPU_amd64_regs(%_cpu)
    gret 1
1:
    movslq %r14d, %r14
    movq %r14, CPU_amd64_regs(%_cpu)
    gret 1
2:
    movswl %r14w, %r14d
    movl %r14d, %r14d
    movq %r14, CPU_amd64_regs(%_cpu)
    gret 1
3:
    cmpq $64, %r11
    je 4f
    cmpq $32, %r11
    je 5f
    movswl %r14w, %r14d
    sarl $15, %r14d
    movw %r14w, CPU_amd64_regs+16(%_cpu)
    gret 1
4:
    sarq $63, %r14
    movq %r14, CPU_amd64_regs+16(%_cpu)
    gret 1
5:
    sarl $31, %r14d
    movl %r14d, %r14d
    movq %r14, CPU_amd64_regs+16(%_cpu)
    gret 1

.gadget amd64_movx_reg_reg
    movq (%_ip), %r10
    movq %r10, %r11
    movq %r10, %r12
    movq %r10, %r13
    andq $0xff, %r10
    shrq $8, %r11
    andq $0xf, %r11
    shrq $12, %r12
    andq $0xf, %r12
    shrq $16, %r13
    andq $0x7f, %r13
    cmpq $0xb7, %r10
    je 2f
    cmpq $0xbf, %r10
    je 3f
    movzbl CPU_amd64_regs(%_cpu,%r11,8), %r14d
    cmpq $0xbe, %r10
    jne 4f
    movsbq %r14b, %r14
    jmp 4f
2:
    movzwl CPU_amd64_regs(%_cpu,%r11,8), %r14d
    jmp 4f
3:
    movswq CPU_amd64_regs(%_cpu,%r11,8), %r14
4:
    cmpq $64, %r13
    je 5f
    movl %r14d, %r14d
5:
    movq %r14, CPU_amd64_regs(%_cpu,%r12,8)
    gret 1

.gadget amd64_movsxd_reg_reg
    movq (%_ip), %r10
    movq %r10, %r11
    movq %r10, %r12
    andq $0xf, %r10
    shrq $4, %r11
    andq $0xf, %r11
    shrq $8, %r12
    andq $0x7f, %r12
    cmpq $64, %r12
    je 1f
    movswl CPU_amd64_regs(%_cpu,%r10,8), %r14d
    movl %r14d, %r14d
    movq %r14, CPU_amd64_regs(%_cpu,%r11,8)
    gret 1
1:
    movslq CPU_amd64_regs(%_cpu,%r10,8), %r14
    movq %r14, CPU_amd64_regs(%_cpu,%r11,8)
    gret 1

.gadget amd64_lea_reg_mem
    movq (%_ip), %r10
    movq 8(%_ip), %r14
    movq %r10, %r15
    shrq $20, %r15
    andq $0xf, %r15
    btq $30, %r10
    jnc 1f
    addq CPU_amd64_regs(%_cpu,%r15,8), %r14
1:
    btq $31, %r10
    jnc 2f
    movq %r10, %r15
    shrq $24, %r15
    andq $0xf, %r15
    movq CPU_amd64_regs(%_cpu,%r15,8), %r15
    testq $(1 << 28), %r10
    jz 11f
    shlq $1, %r15
11:
    testq $(1 << 29), %r10
    jz 12f
    shlq $2, %r15
12:
    addq %r15, %r14
2:
    btq $32, %r10
    jnc 3f
    addq 16(%_ip), %r14
3:
    movq %r10, %r15
    shrq $8, %r15
    andq $0xf, %r15
    shrq $12, %r10
    andq $0xff, %r10
    cmpq $64, %r10
    je 4f
    cmpq $32, %r10
    je 5f
    movw %r14w, CPU_amd64_regs(%_cpu,%r15,8)
    gret 3
4:
    movq %r14, CPU_amd64_regs(%_cpu,%r15,8)
    gret 3
5:
    movl %r14d, %r14d
    movq %r14, CPU_amd64_regs(%_cpu,%r15,8)
    gret 3

.gadget amd64_bswap_reg
    movq (%_ip), %r10
    movq %r10, %r11
    andq $0xf, %r10
    shrq $4, %r11
    andq $0x7f, %r11
    movq CPU_amd64_regs(%_cpu,%r10,8), %r14
    cmpq $64, %r11
    je 1f
    bswapl %r14d
    movl %r14d, %r14d
    movq %r14, CPU_amd64_regs(%_cpu,%r10,8)
    gret 1
1:
    bswapq %r14
    movq %r14, CPU_amd64_regs(%_cpu,%r10,8)
    gret 1

.gadget amd64_test_reg_reg
    movq (%_ip), %r10
    movq %r10, %r11
    movq %r10, %r12
    andq $0xf, %r10
    shrq $4, %r11
    andq $0xf, %r11
    shrq $8, %r12
    andq $0x7f, %r12
    cmpq $64, %r12
    je 1f
    cmpq $32, %r12
    je 2f
    cmpq $16, %r12
    je 7f
    movzbl CPU_amd64_regs(%_cpu,%r10,8), %r14d
    andb CPU_amd64_regs(%_cpu,%r11,8), %r14b
    jmp 3f
7:
    movzwl CPU_amd64_regs(%_cpu,%r10,8), %r14d
    andw CPU_amd64_regs(%_cpu,%r11,8), %r14w
    jmp 3f
1:
    movq CPU_amd64_regs(%_cpu,%r10,8), %r14
    andq CPU_amd64_regs(%_cpu,%r11,8), %r14
    jmp 3f
2:
    movl CPU_amd64_regs(%_cpu,%r10,8), %r14d
    andl CPU_amd64_regs(%_cpu,%r11,8), %r14d
3:
    movl CPU_eflags(%_cpu), %r15d
    andl $~(1 | PF_FLAG | AF_FLAG | ZF_FLAG | SF_FLAG | (1 << 11)), %r15d
    movl $0, CPU_cf(%_cpu)
    movl $0, CPU_of(%_cpu)
    andb $~(ZF_RES | SF_RES | PF_RES | AF_OPS), CPU_flags_res(%_cpu)
    testq %r14, %r14
    setz %r13b
    shlb $6, %r13b
    orb %r13b, %r15b
    setp %r13b
    shlb $2, %r13b
    orb %r13b, %r15b
    cmpq $64, %r12
    je 4f
    cmpq $32, %r12
    je 5f
    cmpq $16, %r12
    je 7f
    testb %r14b, %r14b
    jns 6f
    orb $SF_FLAG, %r15b
    jmp 6f
7:
    testw %r14w, %r14w
    jns 6f
    orb $SF_FLAG, %r15b
    jmp 6f
5:
    testl %r14d, %r14d
    jns 6f
    orb $SF_FLAG, %r15b
    jmp 6f
4:
    testq %r14, %r14
    jns 6f
    orb $SF_FLAG, %r15b
6:
    movl %r15d, CPU_eflags(%_cpu)
    gret 1

.gadget amd64_test_reg_imm
    movq (%_ip), %r10
    movq 8(%_ip), %r11
    movq %r10, %r12
    andq $0xf, %r10
    shrq $4, %r12
    andq $0x7f, %r12
    cmpq $64, %r12
    je 1f
    cmpq $32, %r12
    je 2f
    cmpq $16, %r12
    je 7f
    movzbl CPU_amd64_regs(%_cpu,%r10,8), %r14d
    andb %r11b, %r14b
    jmp 3f
7:
    movzwl CPU_amd64_regs(%_cpu,%r10,8), %r14d
    andw %r11w, %r14w
    jmp 3f
1:
    movq CPU_amd64_regs(%_cpu,%r10,8), %r14
    andq %r11, %r14
    jmp 3f
2:
    movl CPU_amd64_regs(%_cpu,%r10,8), %r14d
    andl %r11d, %r14d
3:
    movl CPU_eflags(%_cpu), %r15d
    andl $~(1 | PF_FLAG | AF_FLAG | ZF_FLAG | SF_FLAG | (1 << 11)), %r15d
    movl $0, CPU_cf(%_cpu)
    movl $0, CPU_of(%_cpu)
    andb $~(ZF_RES | SF_RES | PF_RES | AF_OPS), CPU_flags_res(%_cpu)
    testq %r14, %r14
    setz %r13b
    shlb $6, %r13b
    orb %r13b, %r15b
    setp %r13b
    shlb $2, %r13b
    orb %r13b, %r15b
    cmpq $64, %r12
    je 4f
    cmpq $32, %r12
    je 5f
    cmpq $16, %r12
    je 7f
    testb %r14b, %r14b
    jns 6f
    orb $SF_FLAG, %r15b
    jmp 6f
7:
    testw %r14w, %r14w
    jns 6f
    orb $SF_FLAG, %r15b
    jmp 6f
5:
    testl %r14d, %r14d
    jns 6f
    orb $SF_FLAG, %r15b
    jmp 6f
4:
    testq %r14, %r14
    jns 6f
    orb $SF_FLAG, %r15b
6:
    movl %r15d, CPU_eflags(%_cpu)
    gret 2

.gadget amd64_logic_reg_reg
    movq (%_ip), %r10
    movq %r10, %r11
    movq %r10, %r12
    movq %r10, %r13
    andq $0xff, %r10
    shrq $8, %r11
    andq $0xf, %r11
    shrq $12, %r12
    andq $0xf, %r12
    shrq $16, %r13
    andq $0x7f, %r13
    testq $2, %r10
    jnz 9f
    xchgq %r11, %r12
9:
    movq CPU_amd64_regs(%_cpu,%r11,8), %r14
    cmpq $0x20, %r10
    jae 8f
    orq CPU_amd64_regs(%_cpu,%r12,8), %r14
    jmp 3f
8:
    cmpq $0x30, %r10
    jae 9f
    andq CPU_amd64_regs(%_cpu,%r12,8), %r14
    jmp 3f
9:
    xorq CPU_amd64_regs(%_cpu,%r12,8), %r14
3:
    cmpq $64, %r13
    je 1f
    cmpq $32, %r13
    je 2f
    cmpq $16, %r13
    je 7f
    movb %r14b, CPU_amd64_regs(%_cpu,%r11,8)
    movzbl %r14b, %r14d
    jmp 4f
7:
    movw %r14w, CPU_amd64_regs(%_cpu,%r11,8)
    movzwl %r14w, %r14d
    jmp 4f
1:
    movq %r14, CPU_amd64_regs(%_cpu,%r11,8)
    jmp 4f
2:
    movl %r14d, %r14d
    movq %r14, CPU_amd64_regs(%_cpu,%r11,8)
4:
    movq %r13, %r12
    movl CPU_eflags(%_cpu), %r15d
    andl $~(1 | PF_FLAG | AF_FLAG | ZF_FLAG | SF_FLAG | (1 << 11)), %r15d
    movl $0, CPU_cf(%_cpu)
    movl $0, CPU_of(%_cpu)
    andb $~(ZF_RES | SF_RES | PF_RES | AF_OPS), CPU_flags_res(%_cpu)
    testq %r14, %r14
    setz %r13b
    shlb $6, %r13b
    orb %r13b, %r15b
    setp %r13b
    shlb $2, %r13b
    orb %r13b, %r15b
    cmpq $64, %r12
    je 5f
    cmpq $32, %r12
    je 6f
    cmpq $16, %r12
    je 7f
    testb %r14b, %r14b
    jns 10f
    orb $SF_FLAG, %r15b
    jmp 10f
7:
    testw %r14w, %r14w
    jns 10f
    orb $SF_FLAG, %r15b
    jmp 10f
6:
    testl %r14d, %r14d
    jns 10f
    orb $SF_FLAG, %r15b
    jmp 10f
5:
    testq %r14, %r14
    jns 10f
    orb $SF_FLAG, %r15b
10:
    movl %r15d, CPU_eflags(%_cpu)
    gret 1

.gadget amd64_logic_reg_imm
    movq (%_ip), %r10
    movq 8(%_ip), %r11
    movq %r10, %r12
    movq %r10, %r13
    shrq $8, %r12
    andq $0xf, %r12
    shrq $12, %r13
    andq $0xf, %r13
    shrq $16, %r10
    andq $0x7f, %r10
    movq CPU_amd64_regs(%_cpu,%r13,8), %r14
    cmpq $4, %r12
    je 1f
    cmpq $6, %r12
    je 2f
    orq %r11, %r14
    jmp 3f
1:
    andq %r11, %r14
    jmp 3f
2:
    xorq %r11, %r14
3:
    cmpq $64, %r10
    je 4f
    cmpq $32, %r10
    je 5f
    cmpq $16, %r10
    je 6f
    movb %r14b, CPU_amd64_regs(%_cpu,%r13,8)
    movzbl %r14b, %r14d
    jmp 7f
6:
    movw %r14w, CPU_amd64_regs(%_cpu,%r13,8)
    movzwl %r14w, %r14d
    jmp 7f
4:
    movq %r14, CPU_amd64_regs(%_cpu,%r13,8)
    jmp 7f
5:
    movl %r14d, %r14d
    movq %r14, CPU_amd64_regs(%_cpu,%r13,8)
7:
    movq %r10, %r12
    movl CPU_eflags(%_cpu), %r15d
    andl $~(1 | PF_FLAG | AF_FLAG | ZF_FLAG | SF_FLAG | (1 << 11)), %r15d
    movl $0, CPU_cf(%_cpu)
    movl $0, CPU_of(%_cpu)
    andb $~(ZF_RES | SF_RES | PF_RES | AF_OPS), CPU_flags_res(%_cpu)
    testq %r14, %r14
    setz %r13b
    shlb $6, %r13b
    orb %r13b, %r15b
    setp %r13b
    shlb $2, %r13b
    orb %r13b, %r15b
    cmpq $64, %r12
    je 8f
    cmpq $32, %r12
    je 9f
    cmpq $16, %r12
    je 10f
    testb %r14b, %r14b
    jns 11f
    orb $SF_FLAG, %r15b
    jmp 11f
10:
    testw %r14w, %r14w
    jns 11f
    orb $SF_FLAG, %r15b
    jmp 11f
9:
    testl %r14d, %r14d
    jns 11f
    orb $SF_FLAG, %r15b
    jmp 11f
8:
    testq %r14, %r14
    jns 11f
    orb $SF_FLAG, %r15b
11:
    movl %r15d, CPU_eflags(%_cpu)
    gret 2

.gadget amd64_xor_zero_reg
    movq (%_ip), %r10
    movq %r10, %r11
    andq $0xf, %r10
    shrq $4, %r11
    andq $0x7f, %r11
    cmpq $64, %r11
    je 1f
    cmpq $32, %r11
    je 2f
    movw $0, CPU_amd64_regs(%_cpu,%r10,8)
    jmp 3f
1:
    movq $0, CPU_amd64_regs(%_cpu,%r10,8)
    jmp 3f
2:
    movq $0, CPU_amd64_regs(%_cpu,%r10,8)
3:
    movl CPU_eflags(%_cpu), %r15d
    andl $~(1 | PF_FLAG | AF_FLAG | SF_FLAG | (1 << 11)), %r15d
    orl $(PF_FLAG | ZF_FLAG), %r15d
    movl $0, CPU_cf(%_cpu)
    movl $0, CPU_of(%_cpu)
    andb $~(ZF_RES | SF_RES | PF_RES | AF_OPS), CPU_flags_res(%_cpu)
    movl %r15d, CPU_eflags(%_cpu)
    gret 1

.macro mov32_reg_reg dst_name, dst, src_name, src
    .gadget mov32_reg_reg_\dst_name\()_\src_name
        movl %\src, %\dst
        gret
.endm

.macro mov32_reg_reg_row dst_name, dst
    .macro mov32_reg_reg_col src_name, src
        mov32_reg_reg \dst_name, \dst, \src_name, \src
    .endm
    .each_reg mov32_reg_reg_col
    .purgem mov32_reg_reg_col
.endm

.each_reg mov32_reg_reg_row

.pushsection_rodata
.global.name mov32_reg_reg_gadgets
.irp dst, REG_LIST
    .irp src, REG_LIST
        .quad NAME(gadget_mov32_reg_reg_\dst\()_\src)
    .endr
.endr
.popsection

# this would have been just a few nice compact nested loops, but gas said "nuh uh"

.macro _do_op op, arg, size, s, ss
    .ifc \op,load
        mov\ss \arg, %tmp\s
        .exitm
    .else; .ifc \op,store
        mov\ss %tmp\s, \arg
        .exitm
    .endif; .endif

    .ifin(\op, add,sub,adc,sbb)
        mov\ss \arg, %r14\s
        setf_a src=%r14\s, dst=%tmp\s, ss=\ss
    .endifin
    .ifin(\op, and,or,xor)
        clearf_a
        clearf_oc
    .endifin
    .ifin(\op, adc,sbb)
        btw $0, CPU_cf(%_cpu)
    .endifin

    \op\ss \arg, %tmp\s

    .ifin(\op, add,sub,adc,sbb,imul)
        setf_oc
    .endifin
    .ifin(\op, add,sub,adc,sbb,and,or,xor)
        setf_zsp %tmp\s, \ss
    .endifin
    .ifin(\op, bsf,bsr)
        # I sure hope this isn't too hot
        setzb %r14b
        andb $~(1<<6), CPU_eflags(%_cpu)
        shlb $6, %r14b
        orb %r14b, CPU_eflags(%_cpu)
        andl $~ZF_RES, CPU_flags_res(%_cpu)
    .endifin
    .ifc \op,popcnt
        # popcntw/popcntl above already left the host ZF set iff the count is
        # 0 (identical to the architectural rule); CF/OF/SF/AF/PF are always
        # cleared regardless of the result (Intel SDM POPCNT, not computed
        # from the count -- e.g. PF is not the parity of the count).
        setzb %r14b
        movb $0, CPU_cf(%_cpu)
        movb $0, CPU_of(%_cpu)
        andb $~(ZF_FLAG | SF_FLAG | PF_FLAG | AF_FLAG), CPU_eflags(%_cpu)
        shlb $6, %r14b
        orb %r14b, CPU_eflags(%_cpu)
        andl $~(ZF_RES | SF_RES | PF_RES | AF_OPS), CPU_flags_res(%_cpu)
    .endif
    .ifin(\op, tzcnt,lzcnt)
        # Unlike bsf/bsr, tzcnt/lzcnt define the result as the operand size
        # when the source is zero (the host tzcntl/lzcntl above already
        # computed that), and additionally set CF to that same zero-source
        # condition. OF/SF/AF/PF are left undefined, same as bsf/bsr.
        setcb %r14b
        movb %r14b, CPU_cf(%_cpu)
        setzb %r14b
        andb $~(1<<6), CPU_eflags(%_cpu)
        shlb $6, %r14b
        orb %r14b, CPU_eflags(%_cpu)
        andl $~ZF_RES, CPU_flags_res(%_cpu)
    .endifin
.endm
.macro do_op op, size, arg
    ss \size, _do_op, \op, \arg
.endm

.macro do_reg_op op, size, reg
    .gadget \op\size\()_reg_\reg
        .if \size == 32
            do_op \op, \size, %e\reg\()x
        .elseif \size == 16
            do_op \op, \size, %\reg\()x
        .elseif \size == 8
            do_op \op, \size, %\reg\()l
        .endif
        gret
.endm

.macro do_hi_op op, size, reg
    xchg %\reg\()h, %\reg\()l
    do_op \op, \size, %\reg\()l
    xchg %\reg\()h, %\reg\()l
.endm

.macro do_op_size op, size
    .ifnc \op,store
        .gadget \op\size\()_imm
            do_op \op, \size, (%_ip)
            gret 1
    .endif

    .gadget \op\size\()_mem
        .ifc \op,store
            write_prep \size, \op\size\()_mem
        .else; .ifc \op,xchg
            write_prep \size, \op\size\()_mem
        .else
            read_prep \size, \op\size\()_mem
        .endif; .endif
        do_op \op, \size, (%_addrq)
        .ifc \op,store
            write_done \size, \op\size\()_mem
        .else; .ifc \op,xchg
            write_done \size, \op\size\()_mem
        .endif; .endif
        gret 1

    .irp reg, a,b,c,d
        do_reg_op \op, \size, \reg
    .endr

    .irp reg, si,di,sp,bp
        .gadget \op\size\()_reg_\reg
            .if \size == 32
                .ifnc \reg,sp
                    do_op \op, \size, %e\reg
                .else
                    do_op \op, \size, %_esp
                .endif
            .elseif \size == 16
                .ifnc \reg,sp
                    do_op \op, \size, %\reg
                .else
                    do_op \op, \size, %_sp
                .endif
            .elseif \size == 8
                .ifc \reg,sp; do_hi_op \op, \size, a; .else
                .ifc \reg,bp; do_hi_op \op, \size, c; .else
                .ifc \reg,si; do_hi_op \op, \size, d; .else
                .ifc \reg,di; do_hi_op \op, \size, b
                .endif; .endif; .endif; .endif
            .endif
            gret
    .endr
.endm

.irp op, load,store,xchg,add,sub,adc,sbb,and,or,xor
    .irp size, SIZE_LIST
        do_op_size \op, \size
    .endr
    .gadget_array \op
.endr
.irp op, imul,bsf,bsr,popcnt,tzcnt,lzcnt
    .irp size, 16,32
        do_op_size \op, \size
    .endr
    .gadget_array \op
.endr

# same as above, but only atomics
.macro _do_op_atomic op, arg, size, s, ss
    .ifin(\op, and,or,xor)
        clearf_a
        clearf_oc
    .endifin
    .ifin(\op, adc,sbb)
        btw $0, CPU_cf(%_cpu)
    .endifin

    mov\ss \arg, %r15\s
    xchg %r15, %rax # cmpxchg uses rax as implicit operand
    .ifc \op,xadd
        push %tmp
    .endif
1:
    mov %rax, %r14
    .ifc \op,xadd
        mov (%rsp), %tmp
        xchg %tmp, %r14
    .endif
    .ifin(\op, add,sub,adc,sbb,xadd)
        setf_a src=%tmp\s, dst=%r14\s, ss=\ss
    .endifin
    .ifin(\op, inc,dec)
        setf_a src=$1, dst=%r14\s, ss=\ss
    .endifin

    .ifin(\op, add,sub,adc,sbb,and,or,xor)
        \op\ss %tmp\s, %r14\s
    .endifin
    .ifc \op,xadd
        add\ss %tmp\s, %r14\s
    .endif
    .ifin(\op, inc,dec)
        \op\ss %r14\s
    .endifin

    # CF and OF MUST be captured here, between the arithmetic above and the CAS
    # below. The `lock cmpxchg` overwrites the host flags (that is how its own
    # `jnz` retry works), so capturing after it -- which is what this macro used
    # to do -- recorded CMPXCHG's carry and overflow as if they were the guest
    # operation's. The data results were right, which is why it hid: only the
    # flags were wrong, so `lock xaddl` produced a correct sum with a wrong CF
    # (measured: got 0x004, expected 0x005) and any guest branching on the carry
    # after a locked op took the wrong path.
    #
    # Safe to place here for two reasons: SETcc reads the flags without modifying
    # them, so it cannot disturb the CAS's own condition; and a failed CAS jumps
    # back to re-run the arithmetic, so the iteration that finally succeeds is
    # the one whose flags are left behind.
    #
    # ZF/SF/PF need no such move: setf_zsp below recomputes them from the RESULT
    # VALUE in r14 rather than from live host flags.
    .ifin(\op, add,sub,adc,sbb,xadd)
        setf_oc
    .endifin
    .ifin(\op, inc,dec)
        seto CPU_of(%_cpu)
    .endifin

    lock cmpxchg\ss %r14\s, \arg
    jnz 1b
    mov %r15, %rax
    .ifc \op,xadd
        add $8, %rsp
    .endif

    # setf_zsp is apparently broken in such a way that it can only use _tmp
    xchg %r14, %r10
    setf_zsp %tmp\s, \ss
    xchg %r14, %r10
.endm
.macro do_op_atomic op, size, arg
    ss \size, _do_op_atomic, \op, \arg
.endm

.macro do_op_size_atomic op, size
    .gadget atomic_\op\size\()_mem
        write_prep \size, atomic_\op\size\()_mem
        do_op_atomic \op, \size, (%_addrq)
        write_done \size, atomic_\op\size\()_mem
        gret 1
.endm

.irp op, add,sub,adc,sbb,and,or,xor,inc,dec,xadd
    .irp size, SIZE_LIST
        do_op_size_atomic \op, \size
    .endr
    .gadget_array atomic_\op
.endr

# unary operations (well, only one explicit operand)

.irp op, inc,dec
    .macro do_\op size, s, ss
        setf_a src=$1, dst=%tmp\s, ss=\ss
        \op\()\ss %tmp\s
        seto CPU_of(%_cpu)
        setf_zsp %tmp\s, \ss
    .endm
.endr
.macro do_sign_extend size, s, ss
    .if \size != 32
        movs\ss\()l %tmp\s, %tmpd
    .endif
.endm
.macro do_zero_extend size, s, ss
    .if \size != 32
        movz\ss\()l %tmp\s, %tmpd
    .endif
.endm
.macro do_div size, s, ss
    div\ss %tmp\s
.endm
.macro do_idiv size, s, ss
    idiv\ss %tmp\s
.endm
.macro do_mul size, s, ss
    mul\ss %tmp\s
    setf_oc
.endm
.macro do_imul1 size, s, ss
    imul\ss %tmp\s
    setf_oc
.endm
.macro do_not size, s, ss
    not\ss %tmp\s
.endm

.irp op, inc,dec,sign_extend,zero_extend,div,idiv,mul,imul1,not
    .irp size, SIZE_LIST
        .gadget \op\()_\size
            ss \size, do_\op
            gret
    .endr
    .gadget_list \op, SIZE_LIST
.endr

.gadget cvt_16
    cwd
    gret
.gadget cvt_32
    cdq
    gret
.gadget_list cvt, SIZE_LIST

.gadget cvte_16
    cbw
    gret
.gadget cvte_32
    cwde
    gret
.gadget_list cvte, SIZE_LIST
