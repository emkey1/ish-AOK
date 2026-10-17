# The first ppc64 program: write(1, msg, 14) and exit(0), the error path
# included -- a failed write would exit 1.
.global _start
_start:
    li 0, 4            # write
    li 3, 1
    lis 4, msg@ha
    addi 4, 4, msg@l
    li 5, 14
    sc
    bso fail           # an error sets CR0[SO]
    li 0, 1            # exit
    li 3, 0
    sc
fail:
    li 0, 1
    li 3, 1
    sc
.section .rodata
msg: .ascii "hello ppc64le\n"
