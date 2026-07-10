# Vendor/user extension hook smoke test (riscv64_guest_plan.md patch 5b;
# jit/riscv64_vendor_ext.c; opt/AOK/docs/riscv64-vendor-extensions.md).
#
# Custom mnemonics (ish.clz/ish.ctz/ish.pcnt/ish.bswap) aren't known to any
# standard assembler, so each is a raw .word encoding rd/rs1/funct3 by hand
# (see the doc's "Testing" section for the encoding formula). Exit 0 = all
# pass; nonzero N = the Nth check failed (see the `li t6, N` right before
# each check).
#
# The hook is off by default -- run both ways to exercise the opt-in gate
# itself, not just the arithmetic:
#   ish -r / riscv64_vendor_ext                          # expect SIGILL (132)
#   ISH_RISCV64_VENDOR_EXT=1 ish -r / riscv64_vendor_ext  # expect exit 0

.global _start
_start:
    # ish.clz a0, a1 (a1 = 0xf00000000 -> 28 leading zeros)
    li a1, 0xf
    slli a1, a1, 32
    .word 0x0005850b      # ish.clz a0, a1
    li a2, 28
    li t6, 1
    bne a0, a2, fail

    # ish.ctz a0, a1 (a1 = 0x100 -> 8 trailing zeros)
    li a1, 0x100
    .word 0x0005950b      # ish.ctz a0, a1
    li a2, 8
    li t6, 2
    bne a0, a2, fail

    # ish.pcnt a0, a1 (a1 = 0xff -> 8 bits set)
    li a1, 0xff
    .word 0x0005a50b      # ish.pcnt a0, a1
    li a2, 8
    li t6, 3
    bne a0, a2, fail

    # ish.bswap a0, a1 (a1 = 0x0102030405060708 -> 0x0807060504030201)
    li a1, 0x0102030405060708
    .word 0x0005b50b      # ish.bswap a0, a1
    li a2, 0x0807060504030201
    li t6, 4
    bne a0, a2, fail

    # rd=x0: ish.clz x0, a1 must be a no-op (x0 stays hardwired zero)
    li a0, 123
    li a1, 5
    .word 0x0005800b      # ish.clz x0, a1 (rd field = 0)
    li t6, 5
    li a2, 123
    bne a0, a2, fail

    li a0, 0
    j done
fail:
    mv a0, t6
done:
    li a7, 93
    ecall
