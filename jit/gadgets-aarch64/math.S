#include "gadgets.h"
# AMD64_NF: set to 1 only while instantiating a gadget's no-flags twin (the
# "_nf" gadgets), which skip the flag deposit -- the three
# amd64_cached_set_*_flags macros expand to nothing under it. gen.c patches a
# flag-setting gadget to its twin when the flags it computes are overwritten
# before anything reads them (amd64_flags_note); see there for the rules.
.set AMD64_NF, 0
#include "math.h"
#include "emu/interrupt.h"

.gadget load32_addr
    mov _tmp, _addr
    gret_nomem

.gadget amd64_set_rip
    ldr x8, [_ip]
    str x8, [_cpu, CPU_amd64_rip]
    str w8, [_cpu, CPU_eip]
    gret 1

.gadget amd64_load_low8_reg_cache
    add x8, _cpu, CPU_amd64_regs
    ldr x20, [x8, 0]
    ldr x22, [x8, 8]
    ldr x23, [x8, 16]
    ldr x21, [x8, 24]
    ldr x27, [x8, 32]
    ldr x26, [x8, 40]
    ldr x24, [x8, 48]
    ldr x25, [x8, 56]
    gret

# ---- `mov r64/r32, r64/r32` specialised per register pair ----
#
# amd64_cached_mov_reg_reg and amd64_hybrid_mov_reg_reg pick both registers at
# run time through compare chains (amd64_cached_read/_write, up to eight
# cmp/b.ne each, plus a chain on the size): 7.6% of amd64 7-Zip together
# (2026-10-04). Here each (size, dst, src) is its own gadget with no operand
# word: low-8 to low-8 is one mov between cache registers; a pair with one of
# r8-r15 (which live only in CPU_amd64_regs) is one ldr or str. A 32-bit move
# zero-extends the destination, as x86 does. gen.c's table
# amd64_movr_gadgets[size64][dst][src] has 0 for r8-r15 to r8-r15, which keeps
# the generic gadget. Cache ids: 0 rax x20, 1 rcx x22, 2 rdx x23, 3 rbx x21,
# 4 rsp x27, 5 rbp x26, 6 rsi x24, 7 rdi x25.
.gadget amd64_movr32_0_0
    mov w20, w20
    gret
.gadget amd64_movr32_0_1
    mov w20, w22
    gret
.gadget amd64_movr32_0_2
    mov w20, w23
    gret
.gadget amd64_movr32_0_3
    mov w20, w21
    gret
.gadget amd64_movr32_0_4
    mov w20, w27
    gret
.gadget amd64_movr32_0_5
    mov w20, w26
    gret
.gadget amd64_movr32_0_6
    mov w20, w24
    gret
.gadget amd64_movr32_0_7
    mov w20, w25
    gret
.gadget amd64_movr32_0_8
    ldr w20, [_cpu, CPU_amd64_regs + 64]
    gret
.gadget amd64_movr32_0_9
    ldr w20, [_cpu, CPU_amd64_regs + 72]
    gret
.gadget amd64_movr32_0_10
    ldr w20, [_cpu, CPU_amd64_regs + 80]
    gret
.gadget amd64_movr32_0_11
    ldr w20, [_cpu, CPU_amd64_regs + 88]
    gret
.gadget amd64_movr32_0_12
    ldr w20, [_cpu, CPU_amd64_regs + 96]
    gret
.gadget amd64_movr32_0_13
    ldr w20, [_cpu, CPU_amd64_regs + 104]
    gret
.gadget amd64_movr32_0_14
    ldr w20, [_cpu, CPU_amd64_regs + 112]
    gret
.gadget amd64_movr32_0_15
    ldr w20, [_cpu, CPU_amd64_regs + 120]
    gret
.gadget amd64_movr32_1_0
    mov w22, w20
    gret
.gadget amd64_movr32_1_1
    mov w22, w22
    gret
.gadget amd64_movr32_1_2
    mov w22, w23
    gret
.gadget amd64_movr32_1_3
    mov w22, w21
    gret
.gadget amd64_movr32_1_4
    mov w22, w27
    gret
.gadget amd64_movr32_1_5
    mov w22, w26
    gret
.gadget amd64_movr32_1_6
    mov w22, w24
    gret
.gadget amd64_movr32_1_7
    mov w22, w25
    gret
.gadget amd64_movr32_1_8
    ldr w22, [_cpu, CPU_amd64_regs + 64]
    gret
.gadget amd64_movr32_1_9
    ldr w22, [_cpu, CPU_amd64_regs + 72]
    gret
.gadget amd64_movr32_1_10
    ldr w22, [_cpu, CPU_amd64_regs + 80]
    gret
.gadget amd64_movr32_1_11
    ldr w22, [_cpu, CPU_amd64_regs + 88]
    gret
.gadget amd64_movr32_1_12
    ldr w22, [_cpu, CPU_amd64_regs + 96]
    gret
.gadget amd64_movr32_1_13
    ldr w22, [_cpu, CPU_amd64_regs + 104]
    gret
.gadget amd64_movr32_1_14
    ldr w22, [_cpu, CPU_amd64_regs + 112]
    gret
.gadget amd64_movr32_1_15
    ldr w22, [_cpu, CPU_amd64_regs + 120]
    gret
.gadget amd64_movr32_2_0
    mov w23, w20
    gret
.gadget amd64_movr32_2_1
    mov w23, w22
    gret
.gadget amd64_movr32_2_2
    mov w23, w23
    gret
.gadget amd64_movr32_2_3
    mov w23, w21
    gret
.gadget amd64_movr32_2_4
    mov w23, w27
    gret
.gadget amd64_movr32_2_5
    mov w23, w26
    gret
.gadget amd64_movr32_2_6
    mov w23, w24
    gret
.gadget amd64_movr32_2_7
    mov w23, w25
    gret
.gadget amd64_movr32_2_8
    ldr w23, [_cpu, CPU_amd64_regs + 64]
    gret
.gadget amd64_movr32_2_9
    ldr w23, [_cpu, CPU_amd64_regs + 72]
    gret
.gadget amd64_movr32_2_10
    ldr w23, [_cpu, CPU_amd64_regs + 80]
    gret
.gadget amd64_movr32_2_11
    ldr w23, [_cpu, CPU_amd64_regs + 88]
    gret
.gadget amd64_movr32_2_12
    ldr w23, [_cpu, CPU_amd64_regs + 96]
    gret
.gadget amd64_movr32_2_13
    ldr w23, [_cpu, CPU_amd64_regs + 104]
    gret
.gadget amd64_movr32_2_14
    ldr w23, [_cpu, CPU_amd64_regs + 112]
    gret
.gadget amd64_movr32_2_15
    ldr w23, [_cpu, CPU_amd64_regs + 120]
    gret
.gadget amd64_movr32_3_0
    mov w21, w20
    gret
.gadget amd64_movr32_3_1
    mov w21, w22
    gret
.gadget amd64_movr32_3_2
    mov w21, w23
    gret
.gadget amd64_movr32_3_3
    mov w21, w21
    gret
.gadget amd64_movr32_3_4
    mov w21, w27
    gret
.gadget amd64_movr32_3_5
    mov w21, w26
    gret
.gadget amd64_movr32_3_6
    mov w21, w24
    gret
.gadget amd64_movr32_3_7
    mov w21, w25
    gret
.gadget amd64_movr32_3_8
    ldr w21, [_cpu, CPU_amd64_regs + 64]
    gret
.gadget amd64_movr32_3_9
    ldr w21, [_cpu, CPU_amd64_regs + 72]
    gret
.gadget amd64_movr32_3_10
    ldr w21, [_cpu, CPU_amd64_regs + 80]
    gret
.gadget amd64_movr32_3_11
    ldr w21, [_cpu, CPU_amd64_regs + 88]
    gret
.gadget amd64_movr32_3_12
    ldr w21, [_cpu, CPU_amd64_regs + 96]
    gret
.gadget amd64_movr32_3_13
    ldr w21, [_cpu, CPU_amd64_regs + 104]
    gret
.gadget amd64_movr32_3_14
    ldr w21, [_cpu, CPU_amd64_regs + 112]
    gret
.gadget amd64_movr32_3_15
    ldr w21, [_cpu, CPU_amd64_regs + 120]
    gret
.gadget amd64_movr32_4_0
    mov w27, w20
    gret
.gadget amd64_movr32_4_1
    mov w27, w22
    gret
.gadget amd64_movr32_4_2
    mov w27, w23
    gret
.gadget amd64_movr32_4_3
    mov w27, w21
    gret
.gadget amd64_movr32_4_4
    mov w27, w27
    gret
.gadget amd64_movr32_4_5
    mov w27, w26
    gret
.gadget amd64_movr32_4_6
    mov w27, w24
    gret
.gadget amd64_movr32_4_7
    mov w27, w25
    gret
.gadget amd64_movr32_4_8
    ldr w27, [_cpu, CPU_amd64_regs + 64]
    gret
.gadget amd64_movr32_4_9
    ldr w27, [_cpu, CPU_amd64_regs + 72]
    gret
.gadget amd64_movr32_4_10
    ldr w27, [_cpu, CPU_amd64_regs + 80]
    gret
.gadget amd64_movr32_4_11
    ldr w27, [_cpu, CPU_amd64_regs + 88]
    gret
.gadget amd64_movr32_4_12
    ldr w27, [_cpu, CPU_amd64_regs + 96]
    gret
.gadget amd64_movr32_4_13
    ldr w27, [_cpu, CPU_amd64_regs + 104]
    gret
.gadget amd64_movr32_4_14
    ldr w27, [_cpu, CPU_amd64_regs + 112]
    gret
.gadget amd64_movr32_4_15
    ldr w27, [_cpu, CPU_amd64_regs + 120]
    gret
.gadget amd64_movr32_5_0
    mov w26, w20
    gret
.gadget amd64_movr32_5_1
    mov w26, w22
    gret
.gadget amd64_movr32_5_2
    mov w26, w23
    gret
.gadget amd64_movr32_5_3
    mov w26, w21
    gret
.gadget amd64_movr32_5_4
    mov w26, w27
    gret
.gadget amd64_movr32_5_5
    mov w26, w26
    gret
.gadget amd64_movr32_5_6
    mov w26, w24
    gret
.gadget amd64_movr32_5_7
    mov w26, w25
    gret
.gadget amd64_movr32_5_8
    ldr w26, [_cpu, CPU_amd64_regs + 64]
    gret
.gadget amd64_movr32_5_9
    ldr w26, [_cpu, CPU_amd64_regs + 72]
    gret
.gadget amd64_movr32_5_10
    ldr w26, [_cpu, CPU_amd64_regs + 80]
    gret
.gadget amd64_movr32_5_11
    ldr w26, [_cpu, CPU_amd64_regs + 88]
    gret
.gadget amd64_movr32_5_12
    ldr w26, [_cpu, CPU_amd64_regs + 96]
    gret
.gadget amd64_movr32_5_13
    ldr w26, [_cpu, CPU_amd64_regs + 104]
    gret
.gadget amd64_movr32_5_14
    ldr w26, [_cpu, CPU_amd64_regs + 112]
    gret
.gadget amd64_movr32_5_15
    ldr w26, [_cpu, CPU_amd64_regs + 120]
    gret
.gadget amd64_movr32_6_0
    mov w24, w20
    gret
.gadget amd64_movr32_6_1
    mov w24, w22
    gret
.gadget amd64_movr32_6_2
    mov w24, w23
    gret
.gadget amd64_movr32_6_3
    mov w24, w21
    gret
.gadget amd64_movr32_6_4
    mov w24, w27
    gret
.gadget amd64_movr32_6_5
    mov w24, w26
    gret
.gadget amd64_movr32_6_6
    mov w24, w24
    gret
.gadget amd64_movr32_6_7
    mov w24, w25
    gret
.gadget amd64_movr32_6_8
    ldr w24, [_cpu, CPU_amd64_regs + 64]
    gret
.gadget amd64_movr32_6_9
    ldr w24, [_cpu, CPU_amd64_regs + 72]
    gret
.gadget amd64_movr32_6_10
    ldr w24, [_cpu, CPU_amd64_regs + 80]
    gret
.gadget amd64_movr32_6_11
    ldr w24, [_cpu, CPU_amd64_regs + 88]
    gret
.gadget amd64_movr32_6_12
    ldr w24, [_cpu, CPU_amd64_regs + 96]
    gret
.gadget amd64_movr32_6_13
    ldr w24, [_cpu, CPU_amd64_regs + 104]
    gret
.gadget amd64_movr32_6_14
    ldr w24, [_cpu, CPU_amd64_regs + 112]
    gret
.gadget amd64_movr32_6_15
    ldr w24, [_cpu, CPU_amd64_regs + 120]
    gret
.gadget amd64_movr32_7_0
    mov w25, w20
    gret
.gadget amd64_movr32_7_1
    mov w25, w22
    gret
.gadget amd64_movr32_7_2
    mov w25, w23
    gret
.gadget amd64_movr32_7_3
    mov w25, w21
    gret
.gadget amd64_movr32_7_4
    mov w25, w27
    gret
.gadget amd64_movr32_7_5
    mov w25, w26
    gret
.gadget amd64_movr32_7_6
    mov w25, w24
    gret
.gadget amd64_movr32_7_7
    mov w25, w25
    gret
.gadget amd64_movr32_7_8
    ldr w25, [_cpu, CPU_amd64_regs + 64]
    gret
.gadget amd64_movr32_7_9
    ldr w25, [_cpu, CPU_amd64_regs + 72]
    gret
.gadget amd64_movr32_7_10
    ldr w25, [_cpu, CPU_amd64_regs + 80]
    gret
.gadget amd64_movr32_7_11
    ldr w25, [_cpu, CPU_amd64_regs + 88]
    gret
.gadget amd64_movr32_7_12
    ldr w25, [_cpu, CPU_amd64_regs + 96]
    gret
.gadget amd64_movr32_7_13
    ldr w25, [_cpu, CPU_amd64_regs + 104]
    gret
.gadget amd64_movr32_7_14
    ldr w25, [_cpu, CPU_amd64_regs + 112]
    gret
.gadget amd64_movr32_7_15
    ldr w25, [_cpu, CPU_amd64_regs + 120]
    gret
.gadget amd64_movr32_8_0
    mov w8, w20
    str x8, [_cpu, CPU_amd64_regs + 64]
    gret
.gadget amd64_movr32_8_1
    mov w8, w22
    str x8, [_cpu, CPU_amd64_regs + 64]
    gret
.gadget amd64_movr32_8_2
    mov w8, w23
    str x8, [_cpu, CPU_amd64_regs + 64]
    gret
.gadget amd64_movr32_8_3
    mov w8, w21
    str x8, [_cpu, CPU_amd64_regs + 64]
    gret
.gadget amd64_movr32_8_4
    mov w8, w27
    str x8, [_cpu, CPU_amd64_regs + 64]
    gret
.gadget amd64_movr32_8_5
    mov w8, w26
    str x8, [_cpu, CPU_amd64_regs + 64]
    gret
.gadget amd64_movr32_8_6
    mov w8, w24
    str x8, [_cpu, CPU_amd64_regs + 64]
    gret
.gadget amd64_movr32_8_7
    mov w8, w25
    str x8, [_cpu, CPU_amd64_regs + 64]
    gret
.gadget amd64_movr32_9_0
    mov w8, w20
    str x8, [_cpu, CPU_amd64_regs + 72]
    gret
.gadget amd64_movr32_9_1
    mov w8, w22
    str x8, [_cpu, CPU_amd64_regs + 72]
    gret
.gadget amd64_movr32_9_2
    mov w8, w23
    str x8, [_cpu, CPU_amd64_regs + 72]
    gret
.gadget amd64_movr32_9_3
    mov w8, w21
    str x8, [_cpu, CPU_amd64_regs + 72]
    gret
.gadget amd64_movr32_9_4
    mov w8, w27
    str x8, [_cpu, CPU_amd64_regs + 72]
    gret
.gadget amd64_movr32_9_5
    mov w8, w26
    str x8, [_cpu, CPU_amd64_regs + 72]
    gret
.gadget amd64_movr32_9_6
    mov w8, w24
    str x8, [_cpu, CPU_amd64_regs + 72]
    gret
.gadget amd64_movr32_9_7
    mov w8, w25
    str x8, [_cpu, CPU_amd64_regs + 72]
    gret
.gadget amd64_movr32_10_0
    mov w8, w20
    str x8, [_cpu, CPU_amd64_regs + 80]
    gret
.gadget amd64_movr32_10_1
    mov w8, w22
    str x8, [_cpu, CPU_amd64_regs + 80]
    gret
.gadget amd64_movr32_10_2
    mov w8, w23
    str x8, [_cpu, CPU_amd64_regs + 80]
    gret
.gadget amd64_movr32_10_3
    mov w8, w21
    str x8, [_cpu, CPU_amd64_regs + 80]
    gret
.gadget amd64_movr32_10_4
    mov w8, w27
    str x8, [_cpu, CPU_amd64_regs + 80]
    gret
.gadget amd64_movr32_10_5
    mov w8, w26
    str x8, [_cpu, CPU_amd64_regs + 80]
    gret
.gadget amd64_movr32_10_6
    mov w8, w24
    str x8, [_cpu, CPU_amd64_regs + 80]
    gret
.gadget amd64_movr32_10_7
    mov w8, w25
    str x8, [_cpu, CPU_amd64_regs + 80]
    gret
.gadget amd64_movr32_11_0
    mov w8, w20
    str x8, [_cpu, CPU_amd64_regs + 88]
    gret
.gadget amd64_movr32_11_1
    mov w8, w22
    str x8, [_cpu, CPU_amd64_regs + 88]
    gret
.gadget amd64_movr32_11_2
    mov w8, w23
    str x8, [_cpu, CPU_amd64_regs + 88]
    gret
.gadget amd64_movr32_11_3
    mov w8, w21
    str x8, [_cpu, CPU_amd64_regs + 88]
    gret
.gadget amd64_movr32_11_4
    mov w8, w27
    str x8, [_cpu, CPU_amd64_regs + 88]
    gret
.gadget amd64_movr32_11_5
    mov w8, w26
    str x8, [_cpu, CPU_amd64_regs + 88]
    gret
.gadget amd64_movr32_11_6
    mov w8, w24
    str x8, [_cpu, CPU_amd64_regs + 88]
    gret
.gadget amd64_movr32_11_7
    mov w8, w25
    str x8, [_cpu, CPU_amd64_regs + 88]
    gret
.gadget amd64_movr32_12_0
    mov w8, w20
    str x8, [_cpu, CPU_amd64_regs + 96]
    gret
.gadget amd64_movr32_12_1
    mov w8, w22
    str x8, [_cpu, CPU_amd64_regs + 96]
    gret
.gadget amd64_movr32_12_2
    mov w8, w23
    str x8, [_cpu, CPU_amd64_regs + 96]
    gret
.gadget amd64_movr32_12_3
    mov w8, w21
    str x8, [_cpu, CPU_amd64_regs + 96]
    gret
.gadget amd64_movr32_12_4
    mov w8, w27
    str x8, [_cpu, CPU_amd64_regs + 96]
    gret
.gadget amd64_movr32_12_5
    mov w8, w26
    str x8, [_cpu, CPU_amd64_regs + 96]
    gret
.gadget amd64_movr32_12_6
    mov w8, w24
    str x8, [_cpu, CPU_amd64_regs + 96]
    gret
.gadget amd64_movr32_12_7
    mov w8, w25
    str x8, [_cpu, CPU_amd64_regs + 96]
    gret
.gadget amd64_movr32_13_0
    mov w8, w20
    str x8, [_cpu, CPU_amd64_regs + 104]
    gret
.gadget amd64_movr32_13_1
    mov w8, w22
    str x8, [_cpu, CPU_amd64_regs + 104]
    gret
.gadget amd64_movr32_13_2
    mov w8, w23
    str x8, [_cpu, CPU_amd64_regs + 104]
    gret
.gadget amd64_movr32_13_3
    mov w8, w21
    str x8, [_cpu, CPU_amd64_regs + 104]
    gret
.gadget amd64_movr32_13_4
    mov w8, w27
    str x8, [_cpu, CPU_amd64_regs + 104]
    gret
.gadget amd64_movr32_13_5
    mov w8, w26
    str x8, [_cpu, CPU_amd64_regs + 104]
    gret
.gadget amd64_movr32_13_6
    mov w8, w24
    str x8, [_cpu, CPU_amd64_regs + 104]
    gret
.gadget amd64_movr32_13_7
    mov w8, w25
    str x8, [_cpu, CPU_amd64_regs + 104]
    gret
.gadget amd64_movr32_14_0
    mov w8, w20
    str x8, [_cpu, CPU_amd64_regs + 112]
    gret
.gadget amd64_movr32_14_1
    mov w8, w22
    str x8, [_cpu, CPU_amd64_regs + 112]
    gret
.gadget amd64_movr32_14_2
    mov w8, w23
    str x8, [_cpu, CPU_amd64_regs + 112]
    gret
.gadget amd64_movr32_14_3
    mov w8, w21
    str x8, [_cpu, CPU_amd64_regs + 112]
    gret
.gadget amd64_movr32_14_4
    mov w8, w27
    str x8, [_cpu, CPU_amd64_regs + 112]
    gret
.gadget amd64_movr32_14_5
    mov w8, w26
    str x8, [_cpu, CPU_amd64_regs + 112]
    gret
.gadget amd64_movr32_14_6
    mov w8, w24
    str x8, [_cpu, CPU_amd64_regs + 112]
    gret
.gadget amd64_movr32_14_7
    mov w8, w25
    str x8, [_cpu, CPU_amd64_regs + 112]
    gret
.gadget amd64_movr32_15_0
    mov w8, w20
    str x8, [_cpu, CPU_amd64_regs + 120]
    gret
.gadget amd64_movr32_15_1
    mov w8, w22
    str x8, [_cpu, CPU_amd64_regs + 120]
    gret
.gadget amd64_movr32_15_2
    mov w8, w23
    str x8, [_cpu, CPU_amd64_regs + 120]
    gret
.gadget amd64_movr32_15_3
    mov w8, w21
    str x8, [_cpu, CPU_amd64_regs + 120]
    gret
.gadget amd64_movr32_15_4
    mov w8, w27
    str x8, [_cpu, CPU_amd64_regs + 120]
    gret
.gadget amd64_movr32_15_5
    mov w8, w26
    str x8, [_cpu, CPU_amd64_regs + 120]
    gret
.gadget amd64_movr32_15_6
    mov w8, w24
    str x8, [_cpu, CPU_amd64_regs + 120]
    gret
.gadget amd64_movr32_15_7
    mov w8, w25
    str x8, [_cpu, CPU_amd64_regs + 120]
    gret
.gadget amd64_movr64_0_0
    mov x20, x20
    gret
.gadget amd64_movr64_0_1
    mov x20, x22
    gret
.gadget amd64_movr64_0_2
    mov x20, x23
    gret
.gadget amd64_movr64_0_3
    mov x20, x21
    gret
.gadget amd64_movr64_0_4
    mov x20, x27
    gret
.gadget amd64_movr64_0_5
    mov x20, x26
    gret
.gadget amd64_movr64_0_6
    mov x20, x24
    gret
.gadget amd64_movr64_0_7
    mov x20, x25
    gret
.gadget amd64_movr64_0_8
    ldr x20, [_cpu, CPU_amd64_regs + 64]
    gret
.gadget amd64_movr64_0_9
    ldr x20, [_cpu, CPU_amd64_regs + 72]
    gret
.gadget amd64_movr64_0_10
    ldr x20, [_cpu, CPU_amd64_regs + 80]
    gret
.gadget amd64_movr64_0_11
    ldr x20, [_cpu, CPU_amd64_regs + 88]
    gret
.gadget amd64_movr64_0_12
    ldr x20, [_cpu, CPU_amd64_regs + 96]
    gret
.gadget amd64_movr64_0_13
    ldr x20, [_cpu, CPU_amd64_regs + 104]
    gret
.gadget amd64_movr64_0_14
    ldr x20, [_cpu, CPU_amd64_regs + 112]
    gret
.gadget amd64_movr64_0_15
    ldr x20, [_cpu, CPU_amd64_regs + 120]
    gret
.gadget amd64_movr64_1_0
    mov x22, x20
    gret
.gadget amd64_movr64_1_1
    mov x22, x22
    gret
.gadget amd64_movr64_1_2
    mov x22, x23
    gret
.gadget amd64_movr64_1_3
    mov x22, x21
    gret
.gadget amd64_movr64_1_4
    mov x22, x27
    gret
.gadget amd64_movr64_1_5
    mov x22, x26
    gret
.gadget amd64_movr64_1_6
    mov x22, x24
    gret
.gadget amd64_movr64_1_7
    mov x22, x25
    gret
.gadget amd64_movr64_1_8
    ldr x22, [_cpu, CPU_amd64_regs + 64]
    gret
.gadget amd64_movr64_1_9
    ldr x22, [_cpu, CPU_amd64_regs + 72]
    gret
.gadget amd64_movr64_1_10
    ldr x22, [_cpu, CPU_amd64_regs + 80]
    gret
.gadget amd64_movr64_1_11
    ldr x22, [_cpu, CPU_amd64_regs + 88]
    gret
.gadget amd64_movr64_1_12
    ldr x22, [_cpu, CPU_amd64_regs + 96]
    gret
.gadget amd64_movr64_1_13
    ldr x22, [_cpu, CPU_amd64_regs + 104]
    gret
.gadget amd64_movr64_1_14
    ldr x22, [_cpu, CPU_amd64_regs + 112]
    gret
.gadget amd64_movr64_1_15
    ldr x22, [_cpu, CPU_amd64_regs + 120]
    gret
.gadget amd64_movr64_2_0
    mov x23, x20
    gret
.gadget amd64_movr64_2_1
    mov x23, x22
    gret
.gadget amd64_movr64_2_2
    mov x23, x23
    gret
.gadget amd64_movr64_2_3
    mov x23, x21
    gret
.gadget amd64_movr64_2_4
    mov x23, x27
    gret
.gadget amd64_movr64_2_5
    mov x23, x26
    gret
.gadget amd64_movr64_2_6
    mov x23, x24
    gret
.gadget amd64_movr64_2_7
    mov x23, x25
    gret
.gadget amd64_movr64_2_8
    ldr x23, [_cpu, CPU_amd64_regs + 64]
    gret
.gadget amd64_movr64_2_9
    ldr x23, [_cpu, CPU_amd64_regs + 72]
    gret
.gadget amd64_movr64_2_10
    ldr x23, [_cpu, CPU_amd64_regs + 80]
    gret
.gadget amd64_movr64_2_11
    ldr x23, [_cpu, CPU_amd64_regs + 88]
    gret
.gadget amd64_movr64_2_12
    ldr x23, [_cpu, CPU_amd64_regs + 96]
    gret
.gadget amd64_movr64_2_13
    ldr x23, [_cpu, CPU_amd64_regs + 104]
    gret
.gadget amd64_movr64_2_14
    ldr x23, [_cpu, CPU_amd64_regs + 112]
    gret
.gadget amd64_movr64_2_15
    ldr x23, [_cpu, CPU_amd64_regs + 120]
    gret
.gadget amd64_movr64_3_0
    mov x21, x20
    gret
.gadget amd64_movr64_3_1
    mov x21, x22
    gret
.gadget amd64_movr64_3_2
    mov x21, x23
    gret
.gadget amd64_movr64_3_3
    mov x21, x21
    gret
.gadget amd64_movr64_3_4
    mov x21, x27
    gret
.gadget amd64_movr64_3_5
    mov x21, x26
    gret
.gadget amd64_movr64_3_6
    mov x21, x24
    gret
.gadget amd64_movr64_3_7
    mov x21, x25
    gret
.gadget amd64_movr64_3_8
    ldr x21, [_cpu, CPU_amd64_regs + 64]
    gret
.gadget amd64_movr64_3_9
    ldr x21, [_cpu, CPU_amd64_regs + 72]
    gret
.gadget amd64_movr64_3_10
    ldr x21, [_cpu, CPU_amd64_regs + 80]
    gret
.gadget amd64_movr64_3_11
    ldr x21, [_cpu, CPU_amd64_regs + 88]
    gret
.gadget amd64_movr64_3_12
    ldr x21, [_cpu, CPU_amd64_regs + 96]
    gret
.gadget amd64_movr64_3_13
    ldr x21, [_cpu, CPU_amd64_regs + 104]
    gret
.gadget amd64_movr64_3_14
    ldr x21, [_cpu, CPU_amd64_regs + 112]
    gret
.gadget amd64_movr64_3_15
    ldr x21, [_cpu, CPU_amd64_regs + 120]
    gret
.gadget amd64_movr64_4_0
    mov x27, x20
    gret
.gadget amd64_movr64_4_1
    mov x27, x22
    gret
.gadget amd64_movr64_4_2
    mov x27, x23
    gret
.gadget amd64_movr64_4_3
    mov x27, x21
    gret
.gadget amd64_movr64_4_4
    mov x27, x27
    gret
.gadget amd64_movr64_4_5
    mov x27, x26
    gret
.gadget amd64_movr64_4_6
    mov x27, x24
    gret
.gadget amd64_movr64_4_7
    mov x27, x25
    gret
.gadget amd64_movr64_4_8
    ldr x27, [_cpu, CPU_amd64_regs + 64]
    gret
.gadget amd64_movr64_4_9
    ldr x27, [_cpu, CPU_amd64_regs + 72]
    gret
.gadget amd64_movr64_4_10
    ldr x27, [_cpu, CPU_amd64_regs + 80]
    gret
.gadget amd64_movr64_4_11
    ldr x27, [_cpu, CPU_amd64_regs + 88]
    gret
.gadget amd64_movr64_4_12
    ldr x27, [_cpu, CPU_amd64_regs + 96]
    gret
.gadget amd64_movr64_4_13
    ldr x27, [_cpu, CPU_amd64_regs + 104]
    gret
.gadget amd64_movr64_4_14
    ldr x27, [_cpu, CPU_amd64_regs + 112]
    gret
.gadget amd64_movr64_4_15
    ldr x27, [_cpu, CPU_amd64_regs + 120]
    gret
.gadget amd64_movr64_5_0
    mov x26, x20
    gret
.gadget amd64_movr64_5_1
    mov x26, x22
    gret
.gadget amd64_movr64_5_2
    mov x26, x23
    gret
.gadget amd64_movr64_5_3
    mov x26, x21
    gret
.gadget amd64_movr64_5_4
    mov x26, x27
    gret
.gadget amd64_movr64_5_5
    mov x26, x26
    gret
.gadget amd64_movr64_5_6
    mov x26, x24
    gret
.gadget amd64_movr64_5_7
    mov x26, x25
    gret
.gadget amd64_movr64_5_8
    ldr x26, [_cpu, CPU_amd64_regs + 64]
    gret
.gadget amd64_movr64_5_9
    ldr x26, [_cpu, CPU_amd64_regs + 72]
    gret
.gadget amd64_movr64_5_10
    ldr x26, [_cpu, CPU_amd64_regs + 80]
    gret
.gadget amd64_movr64_5_11
    ldr x26, [_cpu, CPU_amd64_regs + 88]
    gret
.gadget amd64_movr64_5_12
    ldr x26, [_cpu, CPU_amd64_regs + 96]
    gret
.gadget amd64_movr64_5_13
    ldr x26, [_cpu, CPU_amd64_regs + 104]
    gret
.gadget amd64_movr64_5_14
    ldr x26, [_cpu, CPU_amd64_regs + 112]
    gret
.gadget amd64_movr64_5_15
    ldr x26, [_cpu, CPU_amd64_regs + 120]
    gret
.gadget amd64_movr64_6_0
    mov x24, x20
    gret
.gadget amd64_movr64_6_1
    mov x24, x22
    gret
.gadget amd64_movr64_6_2
    mov x24, x23
    gret
.gadget amd64_movr64_6_3
    mov x24, x21
    gret
.gadget amd64_movr64_6_4
    mov x24, x27
    gret
.gadget amd64_movr64_6_5
    mov x24, x26
    gret
.gadget amd64_movr64_6_6
    mov x24, x24
    gret
.gadget amd64_movr64_6_7
    mov x24, x25
    gret
.gadget amd64_movr64_6_8
    ldr x24, [_cpu, CPU_amd64_regs + 64]
    gret
.gadget amd64_movr64_6_9
    ldr x24, [_cpu, CPU_amd64_regs + 72]
    gret
.gadget amd64_movr64_6_10
    ldr x24, [_cpu, CPU_amd64_regs + 80]
    gret
.gadget amd64_movr64_6_11
    ldr x24, [_cpu, CPU_amd64_regs + 88]
    gret
.gadget amd64_movr64_6_12
    ldr x24, [_cpu, CPU_amd64_regs + 96]
    gret
.gadget amd64_movr64_6_13
    ldr x24, [_cpu, CPU_amd64_regs + 104]
    gret
.gadget amd64_movr64_6_14
    ldr x24, [_cpu, CPU_amd64_regs + 112]
    gret
.gadget amd64_movr64_6_15
    ldr x24, [_cpu, CPU_amd64_regs + 120]
    gret
.gadget amd64_movr64_7_0
    mov x25, x20
    gret
.gadget amd64_movr64_7_1
    mov x25, x22
    gret
.gadget amd64_movr64_7_2
    mov x25, x23
    gret
.gadget amd64_movr64_7_3
    mov x25, x21
    gret
.gadget amd64_movr64_7_4
    mov x25, x27
    gret
.gadget amd64_movr64_7_5
    mov x25, x26
    gret
.gadget amd64_movr64_7_6
    mov x25, x24
    gret
.gadget amd64_movr64_7_7
    mov x25, x25
    gret
.gadget amd64_movr64_7_8
    ldr x25, [_cpu, CPU_amd64_regs + 64]
    gret
.gadget amd64_movr64_7_9
    ldr x25, [_cpu, CPU_amd64_regs + 72]
    gret
.gadget amd64_movr64_7_10
    ldr x25, [_cpu, CPU_amd64_regs + 80]
    gret
.gadget amd64_movr64_7_11
    ldr x25, [_cpu, CPU_amd64_regs + 88]
    gret
.gadget amd64_movr64_7_12
    ldr x25, [_cpu, CPU_amd64_regs + 96]
    gret
.gadget amd64_movr64_7_13
    ldr x25, [_cpu, CPU_amd64_regs + 104]
    gret
.gadget amd64_movr64_7_14
    ldr x25, [_cpu, CPU_amd64_regs + 112]
    gret
.gadget amd64_movr64_7_15
    ldr x25, [_cpu, CPU_amd64_regs + 120]
    gret
.gadget amd64_movr64_8_0
    str x20, [_cpu, CPU_amd64_regs + 64]
    gret
.gadget amd64_movr64_8_1
    str x22, [_cpu, CPU_amd64_regs + 64]
    gret
.gadget amd64_movr64_8_2
    str x23, [_cpu, CPU_amd64_regs + 64]
    gret
.gadget amd64_movr64_8_3
    str x21, [_cpu, CPU_amd64_regs + 64]
    gret
.gadget amd64_movr64_8_4
    str x27, [_cpu, CPU_amd64_regs + 64]
    gret
.gadget amd64_movr64_8_5
    str x26, [_cpu, CPU_amd64_regs + 64]
    gret
.gadget amd64_movr64_8_6
    str x24, [_cpu, CPU_amd64_regs + 64]
    gret
.gadget amd64_movr64_8_7
    str x25, [_cpu, CPU_amd64_regs + 64]
    gret
.gadget amd64_movr64_9_0
    str x20, [_cpu, CPU_amd64_regs + 72]
    gret
.gadget amd64_movr64_9_1
    str x22, [_cpu, CPU_amd64_regs + 72]
    gret
.gadget amd64_movr64_9_2
    str x23, [_cpu, CPU_amd64_regs + 72]
    gret
.gadget amd64_movr64_9_3
    str x21, [_cpu, CPU_amd64_regs + 72]
    gret
.gadget amd64_movr64_9_4
    str x27, [_cpu, CPU_amd64_regs + 72]
    gret
.gadget amd64_movr64_9_5
    str x26, [_cpu, CPU_amd64_regs + 72]
    gret
.gadget amd64_movr64_9_6
    str x24, [_cpu, CPU_amd64_regs + 72]
    gret
.gadget amd64_movr64_9_7
    str x25, [_cpu, CPU_amd64_regs + 72]
    gret
.gadget amd64_movr64_10_0
    str x20, [_cpu, CPU_amd64_regs + 80]
    gret
.gadget amd64_movr64_10_1
    str x22, [_cpu, CPU_amd64_regs + 80]
    gret
.gadget amd64_movr64_10_2
    str x23, [_cpu, CPU_amd64_regs + 80]
    gret
.gadget amd64_movr64_10_3
    str x21, [_cpu, CPU_amd64_regs + 80]
    gret
.gadget amd64_movr64_10_4
    str x27, [_cpu, CPU_amd64_regs + 80]
    gret
.gadget amd64_movr64_10_5
    str x26, [_cpu, CPU_amd64_regs + 80]
    gret
.gadget amd64_movr64_10_6
    str x24, [_cpu, CPU_amd64_regs + 80]
    gret
.gadget amd64_movr64_10_7
    str x25, [_cpu, CPU_amd64_regs + 80]
    gret
.gadget amd64_movr64_11_0
    str x20, [_cpu, CPU_amd64_regs + 88]
    gret
.gadget amd64_movr64_11_1
    str x22, [_cpu, CPU_amd64_regs + 88]
    gret
.gadget amd64_movr64_11_2
    str x23, [_cpu, CPU_amd64_regs + 88]
    gret
.gadget amd64_movr64_11_3
    str x21, [_cpu, CPU_amd64_regs + 88]
    gret
.gadget amd64_movr64_11_4
    str x27, [_cpu, CPU_amd64_regs + 88]
    gret
.gadget amd64_movr64_11_5
    str x26, [_cpu, CPU_amd64_regs + 88]
    gret
.gadget amd64_movr64_11_6
    str x24, [_cpu, CPU_amd64_regs + 88]
    gret
.gadget amd64_movr64_11_7
    str x25, [_cpu, CPU_amd64_regs + 88]
    gret
.gadget amd64_movr64_12_0
    str x20, [_cpu, CPU_amd64_regs + 96]
    gret
.gadget amd64_movr64_12_1
    str x22, [_cpu, CPU_amd64_regs + 96]
    gret
.gadget amd64_movr64_12_2
    str x23, [_cpu, CPU_amd64_regs + 96]
    gret
.gadget amd64_movr64_12_3
    str x21, [_cpu, CPU_amd64_regs + 96]
    gret
.gadget amd64_movr64_12_4
    str x27, [_cpu, CPU_amd64_regs + 96]
    gret
.gadget amd64_movr64_12_5
    str x26, [_cpu, CPU_amd64_regs + 96]
    gret
.gadget amd64_movr64_12_6
    str x24, [_cpu, CPU_amd64_regs + 96]
    gret
.gadget amd64_movr64_12_7
    str x25, [_cpu, CPU_amd64_regs + 96]
    gret
.gadget amd64_movr64_13_0
    str x20, [_cpu, CPU_amd64_regs + 104]
    gret
.gadget amd64_movr64_13_1
    str x22, [_cpu, CPU_amd64_regs + 104]
    gret
.gadget amd64_movr64_13_2
    str x23, [_cpu, CPU_amd64_regs + 104]
    gret
.gadget amd64_movr64_13_3
    str x21, [_cpu, CPU_amd64_regs + 104]
    gret
.gadget amd64_movr64_13_4
    str x27, [_cpu, CPU_amd64_regs + 104]
    gret
.gadget amd64_movr64_13_5
    str x26, [_cpu, CPU_amd64_regs + 104]
    gret
.gadget amd64_movr64_13_6
    str x24, [_cpu, CPU_amd64_regs + 104]
    gret
.gadget amd64_movr64_13_7
    str x25, [_cpu, CPU_amd64_regs + 104]
    gret
.gadget amd64_movr64_14_0
    str x20, [_cpu, CPU_amd64_regs + 112]
    gret
.gadget amd64_movr64_14_1
    str x22, [_cpu, CPU_amd64_regs + 112]
    gret
.gadget amd64_movr64_14_2
    str x23, [_cpu, CPU_amd64_regs + 112]
    gret
.gadget amd64_movr64_14_3
    str x21, [_cpu, CPU_amd64_regs + 112]
    gret
.gadget amd64_movr64_14_4
    str x27, [_cpu, CPU_amd64_regs + 112]
    gret
.gadget amd64_movr64_14_5
    str x26, [_cpu, CPU_amd64_regs + 112]
    gret
.gadget amd64_movr64_14_6
    str x24, [_cpu, CPU_amd64_regs + 112]
    gret
.gadget amd64_movr64_14_7
    str x25, [_cpu, CPU_amd64_regs + 112]
    gret
.gadget amd64_movr64_15_0
    str x20, [_cpu, CPU_amd64_regs + 120]
    gret
.gadget amd64_movr64_15_1
    str x22, [_cpu, CPU_amd64_regs + 120]
    gret
.gadget amd64_movr64_15_2
    str x23, [_cpu, CPU_amd64_regs + 120]
    gret
.gadget amd64_movr64_15_3
    str x21, [_cpu, CPU_amd64_regs + 120]
    gret
.gadget amd64_movr64_15_4
    str x27, [_cpu, CPU_amd64_regs + 120]
    gret
.gadget amd64_movr64_15_5
    str x26, [_cpu, CPU_amd64_regs + 120]
    gret
.gadget amd64_movr64_15_6
    str x24, [_cpu, CPU_amd64_regs + 120]
    gret
.gadget amd64_movr64_15_7
    str x25, [_cpu, CPU_amd64_regs + 120]
    gret
.pushsection_rodata
.global.name amd64_movr_gadgets
    .quad NAME(gadget_amd64_movr32_0_0)
    .quad NAME(gadget_amd64_movr32_0_1)
    .quad NAME(gadget_amd64_movr32_0_2)
    .quad NAME(gadget_amd64_movr32_0_3)
    .quad NAME(gadget_amd64_movr32_0_4)
    .quad NAME(gadget_amd64_movr32_0_5)
    .quad NAME(gadget_amd64_movr32_0_6)
    .quad NAME(gadget_amd64_movr32_0_7)
    .quad NAME(gadget_amd64_movr32_0_8)
    .quad NAME(gadget_amd64_movr32_0_9)
    .quad NAME(gadget_amd64_movr32_0_10)
    .quad NAME(gadget_amd64_movr32_0_11)
    .quad NAME(gadget_amd64_movr32_0_12)
    .quad NAME(gadget_amd64_movr32_0_13)
    .quad NAME(gadget_amd64_movr32_0_14)
    .quad NAME(gadget_amd64_movr32_0_15)
    .quad NAME(gadget_amd64_movr32_1_0)
    .quad NAME(gadget_amd64_movr32_1_1)
    .quad NAME(gadget_amd64_movr32_1_2)
    .quad NAME(gadget_amd64_movr32_1_3)
    .quad NAME(gadget_amd64_movr32_1_4)
    .quad NAME(gadget_amd64_movr32_1_5)
    .quad NAME(gadget_amd64_movr32_1_6)
    .quad NAME(gadget_amd64_movr32_1_7)
    .quad NAME(gadget_amd64_movr32_1_8)
    .quad NAME(gadget_amd64_movr32_1_9)
    .quad NAME(gadget_amd64_movr32_1_10)
    .quad NAME(gadget_amd64_movr32_1_11)
    .quad NAME(gadget_amd64_movr32_1_12)
    .quad NAME(gadget_amd64_movr32_1_13)
    .quad NAME(gadget_amd64_movr32_1_14)
    .quad NAME(gadget_amd64_movr32_1_15)
    .quad NAME(gadget_amd64_movr32_2_0)
    .quad NAME(gadget_amd64_movr32_2_1)
    .quad NAME(gadget_amd64_movr32_2_2)
    .quad NAME(gadget_amd64_movr32_2_3)
    .quad NAME(gadget_amd64_movr32_2_4)
    .quad NAME(gadget_amd64_movr32_2_5)
    .quad NAME(gadget_amd64_movr32_2_6)
    .quad NAME(gadget_amd64_movr32_2_7)
    .quad NAME(gadget_amd64_movr32_2_8)
    .quad NAME(gadget_amd64_movr32_2_9)
    .quad NAME(gadget_amd64_movr32_2_10)
    .quad NAME(gadget_amd64_movr32_2_11)
    .quad NAME(gadget_amd64_movr32_2_12)
    .quad NAME(gadget_amd64_movr32_2_13)
    .quad NAME(gadget_amd64_movr32_2_14)
    .quad NAME(gadget_amd64_movr32_2_15)
    .quad NAME(gadget_amd64_movr32_3_0)
    .quad NAME(gadget_amd64_movr32_3_1)
    .quad NAME(gadget_amd64_movr32_3_2)
    .quad NAME(gadget_amd64_movr32_3_3)
    .quad NAME(gadget_amd64_movr32_3_4)
    .quad NAME(gadget_amd64_movr32_3_5)
    .quad NAME(gadget_amd64_movr32_3_6)
    .quad NAME(gadget_amd64_movr32_3_7)
    .quad NAME(gadget_amd64_movr32_3_8)
    .quad NAME(gadget_amd64_movr32_3_9)
    .quad NAME(gadget_amd64_movr32_3_10)
    .quad NAME(gadget_amd64_movr32_3_11)
    .quad NAME(gadget_amd64_movr32_3_12)
    .quad NAME(gadget_amd64_movr32_3_13)
    .quad NAME(gadget_amd64_movr32_3_14)
    .quad NAME(gadget_amd64_movr32_3_15)
    .quad NAME(gadget_amd64_movr32_4_0)
    .quad NAME(gadget_amd64_movr32_4_1)
    .quad NAME(gadget_amd64_movr32_4_2)
    .quad NAME(gadget_amd64_movr32_4_3)
    .quad NAME(gadget_amd64_movr32_4_4)
    .quad NAME(gadget_amd64_movr32_4_5)
    .quad NAME(gadget_amd64_movr32_4_6)
    .quad NAME(gadget_amd64_movr32_4_7)
    .quad NAME(gadget_amd64_movr32_4_8)
    .quad NAME(gadget_amd64_movr32_4_9)
    .quad NAME(gadget_amd64_movr32_4_10)
    .quad NAME(gadget_amd64_movr32_4_11)
    .quad NAME(gadget_amd64_movr32_4_12)
    .quad NAME(gadget_amd64_movr32_4_13)
    .quad NAME(gadget_amd64_movr32_4_14)
    .quad NAME(gadget_amd64_movr32_4_15)
    .quad NAME(gadget_amd64_movr32_5_0)
    .quad NAME(gadget_amd64_movr32_5_1)
    .quad NAME(gadget_amd64_movr32_5_2)
    .quad NAME(gadget_amd64_movr32_5_3)
    .quad NAME(gadget_amd64_movr32_5_4)
    .quad NAME(gadget_amd64_movr32_5_5)
    .quad NAME(gadget_amd64_movr32_5_6)
    .quad NAME(gadget_amd64_movr32_5_7)
    .quad NAME(gadget_amd64_movr32_5_8)
    .quad NAME(gadget_amd64_movr32_5_9)
    .quad NAME(gadget_amd64_movr32_5_10)
    .quad NAME(gadget_amd64_movr32_5_11)
    .quad NAME(gadget_amd64_movr32_5_12)
    .quad NAME(gadget_amd64_movr32_5_13)
    .quad NAME(gadget_amd64_movr32_5_14)
    .quad NAME(gadget_amd64_movr32_5_15)
    .quad NAME(gadget_amd64_movr32_6_0)
    .quad NAME(gadget_amd64_movr32_6_1)
    .quad NAME(gadget_amd64_movr32_6_2)
    .quad NAME(gadget_amd64_movr32_6_3)
    .quad NAME(gadget_amd64_movr32_6_4)
    .quad NAME(gadget_amd64_movr32_6_5)
    .quad NAME(gadget_amd64_movr32_6_6)
    .quad NAME(gadget_amd64_movr32_6_7)
    .quad NAME(gadget_amd64_movr32_6_8)
    .quad NAME(gadget_amd64_movr32_6_9)
    .quad NAME(gadget_amd64_movr32_6_10)
    .quad NAME(gadget_amd64_movr32_6_11)
    .quad NAME(gadget_amd64_movr32_6_12)
    .quad NAME(gadget_amd64_movr32_6_13)
    .quad NAME(gadget_amd64_movr32_6_14)
    .quad NAME(gadget_amd64_movr32_6_15)
    .quad NAME(gadget_amd64_movr32_7_0)
    .quad NAME(gadget_amd64_movr32_7_1)
    .quad NAME(gadget_amd64_movr32_7_2)
    .quad NAME(gadget_amd64_movr32_7_3)
    .quad NAME(gadget_amd64_movr32_7_4)
    .quad NAME(gadget_amd64_movr32_7_5)
    .quad NAME(gadget_amd64_movr32_7_6)
    .quad NAME(gadget_amd64_movr32_7_7)
    .quad NAME(gadget_amd64_movr32_7_8)
    .quad NAME(gadget_amd64_movr32_7_9)
    .quad NAME(gadget_amd64_movr32_7_10)
    .quad NAME(gadget_amd64_movr32_7_11)
    .quad NAME(gadget_amd64_movr32_7_12)
    .quad NAME(gadget_amd64_movr32_7_13)
    .quad NAME(gadget_amd64_movr32_7_14)
    .quad NAME(gadget_amd64_movr32_7_15)
    .quad NAME(gadget_amd64_movr32_8_0)
    .quad NAME(gadget_amd64_movr32_8_1)
    .quad NAME(gadget_amd64_movr32_8_2)
    .quad NAME(gadget_amd64_movr32_8_3)
    .quad NAME(gadget_amd64_movr32_8_4)
    .quad NAME(gadget_amd64_movr32_8_5)
    .quad NAME(gadget_amd64_movr32_8_6)
    .quad NAME(gadget_amd64_movr32_8_7)
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad NAME(gadget_amd64_movr32_9_0)
    .quad NAME(gadget_amd64_movr32_9_1)
    .quad NAME(gadget_amd64_movr32_9_2)
    .quad NAME(gadget_amd64_movr32_9_3)
    .quad NAME(gadget_amd64_movr32_9_4)
    .quad NAME(gadget_amd64_movr32_9_5)
    .quad NAME(gadget_amd64_movr32_9_6)
    .quad NAME(gadget_amd64_movr32_9_7)
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad NAME(gadget_amd64_movr32_10_0)
    .quad NAME(gadget_amd64_movr32_10_1)
    .quad NAME(gadget_amd64_movr32_10_2)
    .quad NAME(gadget_amd64_movr32_10_3)
    .quad NAME(gadget_amd64_movr32_10_4)
    .quad NAME(gadget_amd64_movr32_10_5)
    .quad NAME(gadget_amd64_movr32_10_6)
    .quad NAME(gadget_amd64_movr32_10_7)
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad NAME(gadget_amd64_movr32_11_0)
    .quad NAME(gadget_amd64_movr32_11_1)
    .quad NAME(gadget_amd64_movr32_11_2)
    .quad NAME(gadget_amd64_movr32_11_3)
    .quad NAME(gadget_amd64_movr32_11_4)
    .quad NAME(gadget_amd64_movr32_11_5)
    .quad NAME(gadget_amd64_movr32_11_6)
    .quad NAME(gadget_amd64_movr32_11_7)
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad NAME(gadget_amd64_movr32_12_0)
    .quad NAME(gadget_amd64_movr32_12_1)
    .quad NAME(gadget_amd64_movr32_12_2)
    .quad NAME(gadget_amd64_movr32_12_3)
    .quad NAME(gadget_amd64_movr32_12_4)
    .quad NAME(gadget_amd64_movr32_12_5)
    .quad NAME(gadget_amd64_movr32_12_6)
    .quad NAME(gadget_amd64_movr32_12_7)
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad NAME(gadget_amd64_movr32_13_0)
    .quad NAME(gadget_amd64_movr32_13_1)
    .quad NAME(gadget_amd64_movr32_13_2)
    .quad NAME(gadget_amd64_movr32_13_3)
    .quad NAME(gadget_amd64_movr32_13_4)
    .quad NAME(gadget_amd64_movr32_13_5)
    .quad NAME(gadget_amd64_movr32_13_6)
    .quad NAME(gadget_amd64_movr32_13_7)
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad NAME(gadget_amd64_movr32_14_0)
    .quad NAME(gadget_amd64_movr32_14_1)
    .quad NAME(gadget_amd64_movr32_14_2)
    .quad NAME(gadget_amd64_movr32_14_3)
    .quad NAME(gadget_amd64_movr32_14_4)
    .quad NAME(gadget_amd64_movr32_14_5)
    .quad NAME(gadget_amd64_movr32_14_6)
    .quad NAME(gadget_amd64_movr32_14_7)
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad NAME(gadget_amd64_movr32_15_0)
    .quad NAME(gadget_amd64_movr32_15_1)
    .quad NAME(gadget_amd64_movr32_15_2)
    .quad NAME(gadget_amd64_movr32_15_3)
    .quad NAME(gadget_amd64_movr32_15_4)
    .quad NAME(gadget_amd64_movr32_15_5)
    .quad NAME(gadget_amd64_movr32_15_6)
    .quad NAME(gadget_amd64_movr32_15_7)
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad NAME(gadget_amd64_movr64_0_0)
    .quad NAME(gadget_amd64_movr64_0_1)
    .quad NAME(gadget_amd64_movr64_0_2)
    .quad NAME(gadget_amd64_movr64_0_3)
    .quad NAME(gadget_amd64_movr64_0_4)
    .quad NAME(gadget_amd64_movr64_0_5)
    .quad NAME(gadget_amd64_movr64_0_6)
    .quad NAME(gadget_amd64_movr64_0_7)
    .quad NAME(gadget_amd64_movr64_0_8)
    .quad NAME(gadget_amd64_movr64_0_9)
    .quad NAME(gadget_amd64_movr64_0_10)
    .quad NAME(gadget_amd64_movr64_0_11)
    .quad NAME(gadget_amd64_movr64_0_12)
    .quad NAME(gadget_amd64_movr64_0_13)
    .quad NAME(gadget_amd64_movr64_0_14)
    .quad NAME(gadget_amd64_movr64_0_15)
    .quad NAME(gadget_amd64_movr64_1_0)
    .quad NAME(gadget_amd64_movr64_1_1)
    .quad NAME(gadget_amd64_movr64_1_2)
    .quad NAME(gadget_amd64_movr64_1_3)
    .quad NAME(gadget_amd64_movr64_1_4)
    .quad NAME(gadget_amd64_movr64_1_5)
    .quad NAME(gadget_amd64_movr64_1_6)
    .quad NAME(gadget_amd64_movr64_1_7)
    .quad NAME(gadget_amd64_movr64_1_8)
    .quad NAME(gadget_amd64_movr64_1_9)
    .quad NAME(gadget_amd64_movr64_1_10)
    .quad NAME(gadget_amd64_movr64_1_11)
    .quad NAME(gadget_amd64_movr64_1_12)
    .quad NAME(gadget_amd64_movr64_1_13)
    .quad NAME(gadget_amd64_movr64_1_14)
    .quad NAME(gadget_amd64_movr64_1_15)
    .quad NAME(gadget_amd64_movr64_2_0)
    .quad NAME(gadget_amd64_movr64_2_1)
    .quad NAME(gadget_amd64_movr64_2_2)
    .quad NAME(gadget_amd64_movr64_2_3)
    .quad NAME(gadget_amd64_movr64_2_4)
    .quad NAME(gadget_amd64_movr64_2_5)
    .quad NAME(gadget_amd64_movr64_2_6)
    .quad NAME(gadget_amd64_movr64_2_7)
    .quad NAME(gadget_amd64_movr64_2_8)
    .quad NAME(gadget_amd64_movr64_2_9)
    .quad NAME(gadget_amd64_movr64_2_10)
    .quad NAME(gadget_amd64_movr64_2_11)
    .quad NAME(gadget_amd64_movr64_2_12)
    .quad NAME(gadget_amd64_movr64_2_13)
    .quad NAME(gadget_amd64_movr64_2_14)
    .quad NAME(gadget_amd64_movr64_2_15)
    .quad NAME(gadget_amd64_movr64_3_0)
    .quad NAME(gadget_amd64_movr64_3_1)
    .quad NAME(gadget_amd64_movr64_3_2)
    .quad NAME(gadget_amd64_movr64_3_3)
    .quad NAME(gadget_amd64_movr64_3_4)
    .quad NAME(gadget_amd64_movr64_3_5)
    .quad NAME(gadget_amd64_movr64_3_6)
    .quad NAME(gadget_amd64_movr64_3_7)
    .quad NAME(gadget_amd64_movr64_3_8)
    .quad NAME(gadget_amd64_movr64_3_9)
    .quad NAME(gadget_amd64_movr64_3_10)
    .quad NAME(gadget_amd64_movr64_3_11)
    .quad NAME(gadget_amd64_movr64_3_12)
    .quad NAME(gadget_amd64_movr64_3_13)
    .quad NAME(gadget_amd64_movr64_3_14)
    .quad NAME(gadget_amd64_movr64_3_15)
    .quad NAME(gadget_amd64_movr64_4_0)
    .quad NAME(gadget_amd64_movr64_4_1)
    .quad NAME(gadget_amd64_movr64_4_2)
    .quad NAME(gadget_amd64_movr64_4_3)
    .quad NAME(gadget_amd64_movr64_4_4)
    .quad NAME(gadget_amd64_movr64_4_5)
    .quad NAME(gadget_amd64_movr64_4_6)
    .quad NAME(gadget_amd64_movr64_4_7)
    .quad NAME(gadget_amd64_movr64_4_8)
    .quad NAME(gadget_amd64_movr64_4_9)
    .quad NAME(gadget_amd64_movr64_4_10)
    .quad NAME(gadget_amd64_movr64_4_11)
    .quad NAME(gadget_amd64_movr64_4_12)
    .quad NAME(gadget_amd64_movr64_4_13)
    .quad NAME(gadget_amd64_movr64_4_14)
    .quad NAME(gadget_amd64_movr64_4_15)
    .quad NAME(gadget_amd64_movr64_5_0)
    .quad NAME(gadget_amd64_movr64_5_1)
    .quad NAME(gadget_amd64_movr64_5_2)
    .quad NAME(gadget_amd64_movr64_5_3)
    .quad NAME(gadget_amd64_movr64_5_4)
    .quad NAME(gadget_amd64_movr64_5_5)
    .quad NAME(gadget_amd64_movr64_5_6)
    .quad NAME(gadget_amd64_movr64_5_7)
    .quad NAME(gadget_amd64_movr64_5_8)
    .quad NAME(gadget_amd64_movr64_5_9)
    .quad NAME(gadget_amd64_movr64_5_10)
    .quad NAME(gadget_amd64_movr64_5_11)
    .quad NAME(gadget_amd64_movr64_5_12)
    .quad NAME(gadget_amd64_movr64_5_13)
    .quad NAME(gadget_amd64_movr64_5_14)
    .quad NAME(gadget_amd64_movr64_5_15)
    .quad NAME(gadget_amd64_movr64_6_0)
    .quad NAME(gadget_amd64_movr64_6_1)
    .quad NAME(gadget_amd64_movr64_6_2)
    .quad NAME(gadget_amd64_movr64_6_3)
    .quad NAME(gadget_amd64_movr64_6_4)
    .quad NAME(gadget_amd64_movr64_6_5)
    .quad NAME(gadget_amd64_movr64_6_6)
    .quad NAME(gadget_amd64_movr64_6_7)
    .quad NAME(gadget_amd64_movr64_6_8)
    .quad NAME(gadget_amd64_movr64_6_9)
    .quad NAME(gadget_amd64_movr64_6_10)
    .quad NAME(gadget_amd64_movr64_6_11)
    .quad NAME(gadget_amd64_movr64_6_12)
    .quad NAME(gadget_amd64_movr64_6_13)
    .quad NAME(gadget_amd64_movr64_6_14)
    .quad NAME(gadget_amd64_movr64_6_15)
    .quad NAME(gadget_amd64_movr64_7_0)
    .quad NAME(gadget_amd64_movr64_7_1)
    .quad NAME(gadget_amd64_movr64_7_2)
    .quad NAME(gadget_amd64_movr64_7_3)
    .quad NAME(gadget_amd64_movr64_7_4)
    .quad NAME(gadget_amd64_movr64_7_5)
    .quad NAME(gadget_amd64_movr64_7_6)
    .quad NAME(gadget_amd64_movr64_7_7)
    .quad NAME(gadget_amd64_movr64_7_8)
    .quad NAME(gadget_amd64_movr64_7_9)
    .quad NAME(gadget_amd64_movr64_7_10)
    .quad NAME(gadget_amd64_movr64_7_11)
    .quad NAME(gadget_amd64_movr64_7_12)
    .quad NAME(gadget_amd64_movr64_7_13)
    .quad NAME(gadget_amd64_movr64_7_14)
    .quad NAME(gadget_amd64_movr64_7_15)
    .quad NAME(gadget_amd64_movr64_8_0)
    .quad NAME(gadget_amd64_movr64_8_1)
    .quad NAME(gadget_amd64_movr64_8_2)
    .quad NAME(gadget_amd64_movr64_8_3)
    .quad NAME(gadget_amd64_movr64_8_4)
    .quad NAME(gadget_amd64_movr64_8_5)
    .quad NAME(gadget_amd64_movr64_8_6)
    .quad NAME(gadget_amd64_movr64_8_7)
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad NAME(gadget_amd64_movr64_9_0)
    .quad NAME(gadget_amd64_movr64_9_1)
    .quad NAME(gadget_amd64_movr64_9_2)
    .quad NAME(gadget_amd64_movr64_9_3)
    .quad NAME(gadget_amd64_movr64_9_4)
    .quad NAME(gadget_amd64_movr64_9_5)
    .quad NAME(gadget_amd64_movr64_9_6)
    .quad NAME(gadget_amd64_movr64_9_7)
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad NAME(gadget_amd64_movr64_10_0)
    .quad NAME(gadget_amd64_movr64_10_1)
    .quad NAME(gadget_amd64_movr64_10_2)
    .quad NAME(gadget_amd64_movr64_10_3)
    .quad NAME(gadget_amd64_movr64_10_4)
    .quad NAME(gadget_amd64_movr64_10_5)
    .quad NAME(gadget_amd64_movr64_10_6)
    .quad NAME(gadget_amd64_movr64_10_7)
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad NAME(gadget_amd64_movr64_11_0)
    .quad NAME(gadget_amd64_movr64_11_1)
    .quad NAME(gadget_amd64_movr64_11_2)
    .quad NAME(gadget_amd64_movr64_11_3)
    .quad NAME(gadget_amd64_movr64_11_4)
    .quad NAME(gadget_amd64_movr64_11_5)
    .quad NAME(gadget_amd64_movr64_11_6)
    .quad NAME(gadget_amd64_movr64_11_7)
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad NAME(gadget_amd64_movr64_12_0)
    .quad NAME(gadget_amd64_movr64_12_1)
    .quad NAME(gadget_amd64_movr64_12_2)
    .quad NAME(gadget_amd64_movr64_12_3)
    .quad NAME(gadget_amd64_movr64_12_4)
    .quad NAME(gadget_amd64_movr64_12_5)
    .quad NAME(gadget_amd64_movr64_12_6)
    .quad NAME(gadget_amd64_movr64_12_7)
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad NAME(gadget_amd64_movr64_13_0)
    .quad NAME(gadget_amd64_movr64_13_1)
    .quad NAME(gadget_amd64_movr64_13_2)
    .quad NAME(gadget_amd64_movr64_13_3)
    .quad NAME(gadget_amd64_movr64_13_4)
    .quad NAME(gadget_amd64_movr64_13_5)
    .quad NAME(gadget_amd64_movr64_13_6)
    .quad NAME(gadget_amd64_movr64_13_7)
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad NAME(gadget_amd64_movr64_14_0)
    .quad NAME(gadget_amd64_movr64_14_1)
    .quad NAME(gadget_amd64_movr64_14_2)
    .quad NAME(gadget_amd64_movr64_14_3)
    .quad NAME(gadget_amd64_movr64_14_4)
    .quad NAME(gadget_amd64_movr64_14_5)
    .quad NAME(gadget_amd64_movr64_14_6)
    .quad NAME(gadget_amd64_movr64_14_7)
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad NAME(gadget_amd64_movr64_15_0)
    .quad NAME(gadget_amd64_movr64_15_1)
    .quad NAME(gadget_amd64_movr64_15_2)
    .quad NAME(gadget_amd64_movr64_15_3)
    .quad NAME(gadget_amd64_movr64_15_4)
    .quad NAME(gadget_amd64_movr64_15_5)
    .quad NAME(gadget_amd64_movr64_15_6)
    .quad NAME(gadget_amd64_movr64_15_7)
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .quad 0
.popsection


.gadget amd64_store_low8_reg_cache
    add x8, _cpu, CPU_amd64_regs
    str x20, [x8, 0]
    str x22, [x8, 8]
    str x23, [x8, 16]
    str x21, [x8, 24]
    str x27, [x8, 32]
    str x26, [x8, 40]
    str x24, [x8, 48]
    str x25, [x8, 56]
    gret

# A register-form x87 instruction: [opcode<<8|modrm][next rip][start rip].
# amd64_jit_x87_reg touches only cpu->fp and the flags in struct cpu_state
# (FNSTSW AX, the exception, keeps the flushing bridge), and x20-x27 are
# callee-saved, so the register cache stays live across the call instead of
# being stored and reloaded around it. On an interrupt the cache is stored
# (the _cached form, when it holds the registers) and rip set to the
# instruction before leaving.
.macro amd64_x87_reg_gadget name, cached
.gadget \name
    save_c
    mov x0, _cpu
    sub x1, _tlb, TLB_entries
    ldr x2, [_ip]
    ldr x3, [_ip, 8]
    bl NAME(amd64_jit_x87_reg)
    mov x14, x0
    restore_c
    cmn w14, #1
    b.ne 1f
    gret 3
1:
    .if \cached
        add x8, _cpu, CPU_amd64_regs
        str x20, [x8, 0]
        str x22, [x8, 8]
        str x23, [x8, 16]
        str x21, [x8, 24]
        str x27, [x8, 32]
        str x26, [x8, 40]
        str x24, [x8, 48]
        str x25, [x8, 56]
    .endif
    ldr x8, [_ip, 16]
    str x8, [_cpu, CPU_amd64_rip]
    mov x0, x14
    ldr eip, [_ip, 16]
    b jit_exit
.endm
amd64_x87_reg_gadget amd64_x87_reg, 0
amd64_x87_reg_gadget amd64_x87_reg_cached, 1

# A memory-form x87 instruction: [meta][disp][next rip], addressing decoded at
# translation (gen_amd64_decode_mem_meta). amd64_jit_x87_mem reads the base
# and index registers from cpu->amd64_regs, so the generator stores a dirty
# cache first, but it writes no general register, so the cache stays valid
# across the call (the flushing bridge ended in load_regs, which reloads the
# i386 view and leaves the cache to be reloaded by the next instruction).
.gadget amd64_x87_mem
    save_c
    mov x0, _cpu
    sub x1, _tlb, TLB_entries
    ldr x2, [_ip]
    ldr x3, [_ip, 8]
    ldr x4, [_ip, 16]
    bl NAME(amd64_jit_x87_mem)
    mov x14, x0
    restore_c
    cmn w14, #1
    b.ne 1f
    gret 3
1:
    mov x0, x14
    ldr x8, [_cpu, CPU_amd64_rip]
    mov eip, w8
    b jit_exit

.macro amd64_cached_read id, dst
    cmp \id, 0
    b.ne .Lamd64_cached_read_1_\@
    mov \dst, x20
    b .Lamd64_cached_read_done_\@
.Lamd64_cached_read_1_\@: cmp \id, 1
    b.ne .Lamd64_cached_read_2_\@
    mov \dst, x22
    b .Lamd64_cached_read_done_\@
.Lamd64_cached_read_2_\@: cmp \id, 2
    b.ne .Lamd64_cached_read_3_\@
    mov \dst, x23
    b .Lamd64_cached_read_done_\@
.Lamd64_cached_read_3_\@: cmp \id, 3
    b.ne .Lamd64_cached_read_4_\@
    mov \dst, x21
    b .Lamd64_cached_read_done_\@
.Lamd64_cached_read_4_\@: cmp \id, 4
    b.ne .Lamd64_cached_read_5_\@
    mov \dst, x27
    b .Lamd64_cached_read_done_\@
.Lamd64_cached_read_5_\@: cmp \id, 5
    b.ne .Lamd64_cached_read_6_\@
    mov \dst, x26
    b .Lamd64_cached_read_done_\@
.Lamd64_cached_read_6_\@: cmp \id, 6
    b.ne .Lamd64_cached_read_7_\@
    mov \dst, x24
    b .Lamd64_cached_read_done_\@
.Lamd64_cached_read_7_\@: mov \dst, x25
.Lamd64_cached_read_done_\@:
.endm

.macro amd64_cached_write_one reg
    cmp x11, 64
    b.eq .Lamd64_cached_write_one_64_\@
    cmp x11, 32
    b.eq .Lamd64_cached_write_one_32_\@
    cmp x11, 16
    b.eq .Lamd64_cached_write_one_16_\@
    bfi \reg, x12, 0, 8
    b .Lamd64_cached_write_one_done_\@
.Lamd64_cached_write_one_64_\@: mov \reg, x12
    b .Lamd64_cached_write_one_done_\@
.Lamd64_cached_write_one_32_\@: ubfx \reg, x12, 0, 32
    b .Lamd64_cached_write_one_done_\@
.Lamd64_cached_write_one_16_\@: bfi \reg, x12, 0, 16
.Lamd64_cached_write_one_done_\@:
.endm

.macro amd64_cached_write id, src, size
    mov x11, \size
    mov x12, \src
    cmp \id, 0
    b.ne .Lamd64_cached_write_1_\@
    amd64_cached_write_one x20
    b .Lamd64_cached_write_done_\@
.Lamd64_cached_write_1_\@: cmp \id, 1
    b.ne .Lamd64_cached_write_2_\@
    amd64_cached_write_one x22
    b .Lamd64_cached_write_done_\@
.Lamd64_cached_write_2_\@: cmp \id, 2
    b.ne .Lamd64_cached_write_3_\@
    amd64_cached_write_one x23
    b .Lamd64_cached_write_done_\@
.Lamd64_cached_write_3_\@: cmp \id, 3
    b.ne .Lamd64_cached_write_4_\@
    amd64_cached_write_one x21
    b .Lamd64_cached_write_done_\@
.Lamd64_cached_write_4_\@: cmp \id, 4
    b.ne .Lamd64_cached_write_5_\@
    amd64_cached_write_one x27
    b .Lamd64_cached_write_done_\@
.Lamd64_cached_write_5_\@: cmp \id, 5
    b.ne .Lamd64_cached_write_6_\@
    amd64_cached_write_one x26
    b .Lamd64_cached_write_done_\@
.Lamd64_cached_write_6_\@: cmp \id, 6
    b.ne .Lamd64_cached_write_7_\@
    amd64_cached_write_one x24
    b .Lamd64_cached_write_done_\@
.Lamd64_cached_write_7_\@: amd64_cached_write_one x25
.Lamd64_cached_write_done_\@:
.endm

.macro amd64_read_low_or_mem id, dst
    cmp \id, 8
    b.hs .Lamd64_read_mem_\@
    amd64_cached_read \id, \dst
    b .Lamd64_read_done_\@
.Lamd64_read_mem_\@:
    add x12, _cpu, CPU_amd64_regs
    ldr \dst, [x12, \id, lsl 3]
.Lamd64_read_done_\@:
.endm

.macro amd64_write_low_or_mem id, src, size
    mov x15, \src
    cmp \id, 8
    b.hs .Lamd64_write_mem_\@
    amd64_cached_write \id, x15, \size
    b .Lamd64_write_done_\@
.Lamd64_write_mem_\@:
    add x12, _cpu, CPU_amd64_regs
    add x12, x12, \id, lsl 3
    cmp \size, 64
    b.eq .Lamd64_write_mem_64_\@
    cmp \size, 32
    b.eq .Lamd64_write_mem_32_\@
    cmp \size, 16
    b.eq .Lamd64_write_mem_16_\@
    strb w15, [x12]
    b .Lamd64_write_done_\@
.Lamd64_write_mem_16_\@: strh w15, [x12]
    b .Lamd64_write_done_\@
.Lamd64_write_mem_32_\@: ubfx x12, x15, 0, 32
    add x13, _cpu, CPU_amd64_regs
    str x12, [x13, \id, lsl 3]
    b .Lamd64_write_done_\@
.Lamd64_write_mem_64_\@: str x15, [x12]
.Lamd64_write_done_\@:
.endm

.macro amd64_cached_mask_result result, size
    cmp \size, 64
    b.eq .Lamd64_cached_mask_done_\@
    cmp \size, 32
    b.eq .Lamd64_cached_mask_32_\@
    cmp \size, 16
    b.eq .Lamd64_cached_mask_16_\@
    and \result, \result, 0xff
    b .Lamd64_cached_mask_done_\@
.Lamd64_cached_mask_16_\@: and \result, \result, 0xffff
    b .Lamd64_cached_mask_done_\@
.Lamd64_cached_mask_32_\@: mov w15, w15
.Lamd64_cached_mask_done_\@:
.endm

.macro amd64_cached_set_logic_flags result, size
.if AMD64_NF == 0
    ldr w16, [_cpu, CPU_eflags]
    mov w17, (1 | PF_FLAG | AF_FLAG | ZF_FLAG | SF_FLAG | (1 << 11))
    bic w16, w16, w17
    strb wzr, [_cpu, CPU_cf]
    strb wzr, [_cpu, CPU_of]
    ldrb w17, [_cpu, CPU_flags_res]
    bic w17, w17, (ZF_RES | SF_RES | PF_RES | AF_OPS)
    strb w17, [_cpu, CPU_flags_res]
    cmp \result, 0
    cset w17, eq
    orr w16, w16, w17, lsl 6
    and w17, w15, 0xff
    eor w17, w17, w17, lsr 4
    eor w17, w17, w17, lsr 2
    eor w17, w17, w17, lsr 1
    mvn w17, w17
    and w17, w17, 1
    orr w16, w16, w17, lsl 2
    /* SF = bit (size - 1), without branching on the size */
    sub x17, \size, 1
    lsr x17, \result, x17
    and w17, w17, 1
    orr w16, w16, w17, lsl 7
    str w16, [_cpu, CPU_eflags]
.endif
.endm

.gadget amd64_cached_mov_reg_reg
    ldr x8, [_ip]
    and x9, x8, 0xf
    ubfx x10, x8, 4, 4
    ubfx x11, x8, 8, 7
    amd64_cached_read x9, x12
    amd64_cached_write x10, x12, x11
    gret 1

.gadget amd64_hybrid_mov_reg_reg
    ldr x8, [_ip]
    and x9, x8, 0xf
    ubfx x10, x8, 4, 4
    ubfx x11, x8, 8, 7
    amd64_read_low_or_mem x9, x15
    amd64_write_low_or_mem x10, x15, x11
    gret 1

.gadget amd64_cached_mov_imm_reg
    ldr x8, [_ip]
    ldr x12, [_ip, 8]
    and x10, x8, 0xf
    ubfx x11, x8, 4, 7
    amd64_cached_write x10, x12, x11
    gret 2

.macro def_amd64_cached_logic_reg_imm sfx
.gadget amd64_cached_logic_reg_imm\sfx
    ldr x8, [_ip]
    ldr x13, [_ip, 8]
    ubfx x9, x8, 8, 4
    ubfx x10, x8, 12, 4
    ubfx x11, x8, 16, 7
    amd64_cached_read x10, x15
    cmp x9, 4
    b.eq 1f
    cmp x9, 6
    b.eq 2f
    orr x15, x15, x13
    b 3f
1:  and x15, x15, x13
    b 3f
2:  eor x15, x15, x13
3:  amd64_cached_mask_result x15, x11
    amd64_cached_write x10, x15, x11
    amd64_cached_set_logic_flags x15, x11
    gret 2

.endm
def_amd64_cached_logic_reg_imm
.set AMD64_NF, 1
def_amd64_cached_logic_reg_imm _nf
.set AMD64_NF, 0
.purgem def_amd64_cached_logic_reg_imm
.macro amd64_cached_set_addsub_flags sign_bit
.if AMD64_NF == 0
    /* Branch-free: every flag is data, and the old per-flag branches
       mispredicted on it (24% of amd64 7-Zip samples, 2026-10-04). CF/OF test
       nonzero like the cbz they replace (some OF producers leave more than
       bit 0). PF: bit 0 of the folded low byte is the parity of bits 0-7. */
    strb w14, [_cpu, CPU_cf]
    strb w16, [_cpu, CPU_of]
    ldr w17, [_cpu, CPU_eflags]
    mov x12, (1 | PF_FLAG | AF_FLAG | ZF_FLAG | SF_FLAG | (1 << 11))
    bic w17, w17, w12
    cmp w14, 0
    cset w12, ne
    orr w17, w17, w12
    cmp w16, 0
    cset w12, ne
    orr w17, w17, w12, lsl 11
    eor x12, x13, x10
    eor x12, x12, x15
    and w12, w12, AF_FLAG
    orr w17, w17, w12
    cmp x15, 0
    cset w12, eq
    orr w17, w17, w12, lsl 6
    ubfx x12, x15, \sign_bit, 1
    orr w17, w17, w12, lsl 7
    eor w12, w15, w15, lsr 4
    eor w12, w12, w12, lsr 2
    eor w12, w12, w12, lsr 1
    mvn w12, w12, lsl 2
    and w12, w12, PF_FLAG
    orr w17, w17, w12
    ldrb w12, [_cpu, CPU_flags_res]
    bic w12, w12, (ZF_RES | SF_RES | PF_RES | AF_OPS)
    strb w12, [_cpu, CPU_flags_res]
    str w17, [_cpu, CPU_eflags]
.endif
.endm

.macro def_amd64_cached_arith_reg_imm sfx
.gadget amd64_cached_arith_reg_imm\sfx
    ldr x8, [_ip]
    ldr x10, [_ip, 8]
    ubfx x9, x8, 8, 4
    ubfx x11, x8, 12, 4
    ubfx x12, x8, 16, 7
    mov x8, x11
    mov x0, x12
    amd64_cached_read x11, x13
    cmp x12, 64
    b.eq 1f
    mov w13, w13
    mov w10, w10
    cmp x9, 0
    b.eq 2f
    subs w15, w13, w10
    cset w14, cc
    cset w16, vs
    b 3f
2:  adds w15, w13, w10
    cset w14, cs
    cset w16, vs
3:  mov w15, w15
    amd64_cached_set_addsub_flags 31
    cmp x9, 7
    b.eq 4f
    amd64_cached_write x8, x15, x0
4:  gret 2
1:  cmp x9, 0
    b.eq 5f
    subs x15, x13, x10
    cset w14, cc
    cset w16, vs
    b 6f
5:  adds x15, x13, x10
    cset w14, cs
    cset w16, vs
6:  amd64_cached_set_addsub_flags 63
    cmp x9, 7
    b.eq 7f
    amd64_cached_write x8, x15, x0
7:  gret 2
.endm
def_amd64_cached_arith_reg_imm
.set AMD64_NF, 1
def_amd64_cached_arith_reg_imm _nf
.set AMD64_NF, 0
.purgem def_amd64_cached_arith_reg_imm

# imul body: x13 = op1, x10 = op2, x9 = dst reg id, x12 = size (32/64).
# result = low N bits of the signed product -> dst; CF=OF = (product was truncated),
# matching amd64_set_mul_flags. SF/ZF/AF/PF are left untouched (undefined per spec;
# the prior op's lazy/eager flag state stays valid). Clobbers x14-x17.
.macro amd64_imul_body
    cmp x12, 64
    b.eq 8001f
    sxtw x13, w13
    sxtw x10, w10
    mul x14, x13, x10
    sxtw x15, w14
    cmp x15, x14
    cset w16, ne
    mov w15, w14
    b 8002f
8001:
    mul x15, x13, x10
    smulh x14, x13, x10
    asr x17, x15, 63
    cmp x14, x17
    cset w16, ne
8002:
    amd64_cached_write x9, x15, x12
    strb w16, [_cpu, CPU_cf]
    strb w16, [_cpu, CPU_of]
    ldr w17, [_cpu, CPU_eflags]
    mov w14, (1 | (1 << 11))
    bic w17, w17, w14
    cbz w16, 8003f
    orr w17, w17, w14
8003:
    str w17, [_cpu, CPU_eflags]
.endm

# imul reg, rm, imm (69 id / 6B ib), reg form: dst <- rm * imm (signed)
.gadget amd64_imul_imm
    ldr x8, [_ip]
    ldr x10, [_ip, 8]
    ubfx x9, x8, 8, 4
    ubfx x11, x8, 12, 4
    ubfx x12, x8, 16, 7
    amd64_cached_read x11, x13
    amd64_imul_body
    gret 2

# imul reg, rm (0F AF), reg form: dst <- dst * rm (signed)
.gadget amd64_imul_reg
    ldr x8, [_ip]
    ubfx x9, x8, 8, 4
    ubfx x11, x8, 12, 4
    ubfx x12, x8, 16, 7
    amd64_cached_read x9, x13
    amd64_cached_read x11, x10
    amd64_imul_body
    gret 1

.macro def_amd64_cached_arith_reg_reg sfx
.gadget amd64_cached_arith_reg_reg\sfx
    ldr x8, [_ip]
    and x9, x8, 0xff
    ubfx x10, x8, 8, 4
    ubfx x11, x8, 12, 4
    ubfx x12, x8, 16, 7
    mov x0, x12
    tst x9, 2
    b.ne 1f
    mov x8, x11
    amd64_cached_read x11, x13
    amd64_cached_read x10, x10
    b 2f
1:  mov x8, x10
    amd64_cached_read x10, x13
    amd64_cached_read x11, x10
2:  cmp x0, 64
    b.eq 3f
    mov w13, w13
    mov w10, w10
    cmp x9, 0x28
    b.hs 4f
    adds w15, w13, w10
    cset w14, cs
    cset w16, vs
    b 5f
4:  subs w15, w13, w10
    cset w14, cc
    cset w16, vs
5:  mov w15, w15
    amd64_cached_set_addsub_flags 31
    cmp x9, 0x38
    b.hs 6f
    amd64_cached_write x8, x15, x0
6:  gret 1
3:  cmp x9, 0x28
    b.hs 7f
    adds x15, x13, x10
    cset w14, cs
    cset w16, vs
    b 8f
7:  subs x15, x13, x10
    cset w14, cc
    cset w16, vs
8:  amd64_cached_set_addsub_flags 63
    cmp x9, 0x38
    b.hs 9f
    amd64_cached_write x8, x15, x0
9:  gret 1

.endm
def_amd64_cached_arith_reg_reg
.set AMD64_NF, 1
def_amd64_cached_arith_reg_reg _nf
.set AMD64_NF, 0
.purgem def_amd64_cached_arith_reg_reg
.macro amd64_cached_set_shift_flags sign_bit
.if AMD64_NF == 0
    /* Branch-free like amd64_cached_set_addsub_flags; AF is left clear. */
    strb w14, [_cpu, CPU_cf]
    strb w16, [_cpu, CPU_of]
    ldr w17, [_cpu, CPU_eflags]
    mov x12, (1 | PF_FLAG | AF_FLAG | ZF_FLAG | SF_FLAG | (1 << 11))
    bic w17, w17, w12
    cmp w14, 0
    cset w12, ne
    orr w17, w17, w12
    cmp w16, 0
    cset w12, ne
    orr w17, w17, w12, lsl 11
    cmp x15, 0
    cset w12, eq
    orr w17, w17, w12, lsl 6
    ubfx x12, x15, \sign_bit, 1
    orr w17, w17, w12, lsl 7
    eor w12, w15, w15, lsr 4
    eor w12, w12, w12, lsr 2
    eor w12, w12, w12, lsr 1
    mvn w12, w12, lsl 2
    and w12, w12, PF_FLAG
    orr w17, w17, w12
    ldrb w12, [_cpu, CPU_flags_res]
    bic w12, w12, (ZF_RES | SF_RES | PF_RES | AF_OPS)
    strb w12, [_cpu, CPU_flags_res]
    str w17, [_cpu, CPU_eflags]
.endif
.endm

.macro def_amd64_cached_shift_reg_count sfx
.gadget amd64_cached_shift_reg_count\sfx
    ldr x8, [_ip]
    ldr x10, [_ip, 8]
    tbnz x10, 63, 16f
    b 17f
16: and x10, x22, 0xff
17:
    ubfx x9, x8, 8, 4
    ubfx x11, x8, 12, 4
    ubfx x12, x8, 16, 7
    mov x8, x11
    mov x0, x12
    cmp x12, 64
    b.eq 1f
    and x10, x10, 0x1f
    amd64_cached_read x8, x13
    mov w13, w13
    cbnz x10, 18f
    amd64_cached_write x8, x13, x0   /* a count of 0 still writes the 32-bit */
    b 9f                             /* register: zero-extended (hardware) */
18: cmp x9, 4
    b.eq 2f
    cmp x9, 5
    b.eq 3f
    asr w15, w13, w10
    sub w12, w10, 1
    lsr w14, w13, w12
    b 4f
2:  lsl w15, w13, w10
    mov w14, w13
    mov w12, 32
    sub w12, w12, w10
    lsr w14, w14, w12
    b 4f
3:  lsr w15, w13, w10
    sub w12, w10, 1
    lsr w14, w13, w12
4:  and w14, w14, 1
    mov w16, 0
    cmp x10, 1
    b.ne 5f
    cmp x9, 4
    b.eq 6f
    cmp x9, 5
    b.eq 7f
    b 5f
6:  lsr w16, w15, 31
    eor w16, w16, w14
    b 5f
7:  lsr w16, w13, 31
5:  mov w15, w15
    amd64_cached_write x8, x15, x0
    amd64_cached_set_shift_flags 31
    gret 2
1:  and x10, x10, 0x3f
    cbz x10, 9f
    amd64_cached_read x8, x13
    cmp x9, 4
    b.eq 10f
    cmp x9, 5
    b.eq 11f
    asr x15, x13, x10
    sub x12, x10, 1
    lsr x14, x13, x12
    b 12f
10: lsl x15, x13, x10
    mov x14, x13
    mov x12, 64
    sub x12, x12, x10
    lsr x14, x14, x12
    b 12f
11: lsr x15, x13, x10
    sub x12, x10, 1
    lsr x14, x13, x12
12: and w14, w14, 1
    mov w16, 0
    cmp x10, 1
    b.ne 13f
    cmp x9, 4
    b.eq 14f
    cmp x9, 5
    b.eq 15f
    b 13f
14: lsr x16, x15, 63
    eor w16, w16, w14
    b 13f
15: lsr x16, x13, 63
13: amd64_cached_write x8, x15, x0
    amd64_cached_set_shift_flags 63
9:  gret 2
.endm
def_amd64_cached_shift_reg_count
.set AMD64_NF, 1
def_amd64_cached_shift_reg_count _nf
.set AMD64_NF, 0
.purgem def_amd64_cached_shift_reg_count

# ROL/ROR reg, imm8 (0xc1 /0 rol, /1 ror), reg form, 32/64-bit. THE SHA-512 hot op
# (Sigma/sigma are ror-chains) -- had no native gadget, bridged per-op through the
# interp (amd64_rotate_value + amd64_set_rotate_flags + sync/collapse). Flush style:
# gen.c flushes the reg cache so regs[] memory is current, and this reads/writes the
# operand straight from CPU_amd64_regs (handles r8-r15, which SHA-512 lives in). Only
# CF and OF change -- ZF/SF/PF/AF are untouched (so only eflags bits 0 and 11 are
# rewritten, lazy flags_res left intact); a rotate by count%size==0 changes nothing;
# OF is defined only for a 1-bit rotate (else left as-is). cf=x14, of=x16 match the
# interp's amd64_set_rotate_flags exactly. Operand: rm bits 0-3, subop bit 4, imm8
# bits 8-15. ARM `ror` gives ROR directly; ROL = ROR by (size-eff).
.macro amd64_gen_rotate_imm name, vbytes
.gadget amd64_\name
    ldr  x8, [_ip]
    and  x9, x8, 0xf                 /* rm reg id */
    add  x11, _cpu, CPU_amd64_regs
    add  x11, x11, x9, lsl 3         /* &regs[rm] */
    ubfx x10, x8, 8, 8               /* imm count */
.if \vbytes == 4
    and  x10, x10, 31                /* eff = count % 32 */
    cbz  x10, 9f
    ldr  w13, [x11]
    tbnz x8, 4, 1f
    mov  w12, 32                     /* ROL32: ror by 32-eff */
    sub  w12, w12, w10
    ror  w15, w13, w12
    and  w14, w15, 1                 /* cf = result & 1 */
    lsr  w16, w15, 31
    eor  w16, w16, w14               /* of = cf ^ MSB */
    b    2f
1:  ror  w15, w13, w10               /* ROR32 */
    lsr  w14, w15, 31                /* cf = MSB */
    ubfx w16, w15, 30, 1             /* next-MSB (bit 30) */
    eor  w16, w16, w14               /* ROR OF (1-bit) = MSB ^ next-MSB */
2:  ubfx x12, x15, 0, 32
    str  x12, [x11]
.else
    and  x10, x10, 63                /* eff = count % 64 */
    cbz  x10, 9f
    ldr  x13, [x11]
    tbnz x8, 4, 1f
    mov  x12, 64                     /* ROL64 */
    sub  x12, x12, x10
    ror  x15, x13, x12
    and  x14, x15, 1
    lsr  x16, x15, 63
    eor  x16, x16, x14
    b    2f
1:  ror  x15, x13, x10               /* ROR64 */
    lsr  x14, x15, 63
    ubfx x16, x15, 62, 1             /* next-MSB (bit 62) */
    eor  x16, x16, x14               /* ROR OF (1-bit) = MSB ^ next-MSB */
2:  str  x15, [x11]
.endif
    strb w14, [_cpu, CPU_cf]
    ldr  w17, [_cpu, CPU_eflags]
    bic  w17, w17, 1
    orr  w17, w17, w14               /* eflags CF (bit 0) */
    cmp  x10, 1
    b.ne 8f                          /* OF defined only for 1-bit rotate */
    strb w16, [_cpu, CPU_of]
    bic  w17, w17, (1 << 11)
    orr  w17, w17, w16, lsl 11       /* eflags OF (bit 11) */
8:  str  w17, [_cpu, CPU_eflags]
9:  gret 1
.endm

amd64_gen_rotate_imm rotate_imm32, 4
amd64_gen_rotate_imm rotate_imm64, 8

# SHL (/4) / SHR (/5) / SAR (/7) reg, imm8 (0xc1), 32/64-bit, flush style so it covers
# r8-r15 (SHA-512's sigma functions do `shr r8,imm`; cached_shift_reg_count is low-8
# only). Mirrors cached_shift_reg_count's per-op result/CF/OF math exactly (CF = last
# bit shifted out; OF defined only for a 1-bit shift: SHL = MSB(res)^CF, SHR =
# MSB(src), SAR = 0) and reuses amd64_cached_set_shift_flags (cache-agnostic: writes
# CPU_*). Shift count%size==0 -> no change. Operand: rm bits 0-3, group bits 4-7,
# imm8 bits 8-15.
.macro amd64_gen_shift_imm name, vbytes
.gadget amd64_\name
    ldr  x8, [_ip]
    and  x11, x8, 0xf                /* rm id */
    ubfx x9, x8, 4, 4                /* group (4 shl / 5 shr / 7 sar) */
    ubfx x10, x8, 8, 8              /* imm count */
    add  x13, _cpu, CPU_amd64_regs
    add  x11, x13, x11, lsl 3       /* &regs[rm] */
.if \vbytes == 4
    and  x10, x10, 31
    cbz  x10, 9f
    ldr  w13, [x11]
    cmp  x9, 4
    b.eq 2f
    cmp  x9, 5
    b.eq 3f
    asr  w15, w13, w10              /* SAR */
    sub  w12, w10, 1
    lsr  w14, w13, w12
    b    4f
2:  lsl  w15, w13, w10             /* SHL */
    mov  w14, w13
    mov  w12, 32
    sub  w12, w12, w10
    lsr  w14, w14, w12
    b    4f
3:  lsr  w15, w13, w10             /* SHR */
    sub  w12, w10, 1
    lsr  w14, w13, w12
4:  and  w14, w14, 1
    mov  w16, 0
    cmp  x10, 1
    b.ne 5f
    cmp  x9, 4
    b.eq 6f
    cmp  x9, 5
    b.eq 7f
    b    5f
6:  lsr  w16, w15, 31
    eor  w16, w16, w14
    b    5f
7:  lsr  w16, w13, 31
5:  mov  w15, w15
    ubfx x12, x15, 0, 32
    str  x12, [x11]
    amd64_cached_set_shift_flags 31
.else
    and  x10, x10, 63
    cbz  x10, 9f
    ldr  x13, [x11]
    cmp  x9, 4
    b.eq 2f
    cmp  x9, 5
    b.eq 3f
    asr  x15, x13, x10             /* SAR */
    sub  x12, x10, 1
    lsr  x14, x13, x12
    b    4f
2:  lsl  x15, x13, x10            /* SHL */
    mov  x14, x13
    mov  x12, 64
    sub  x12, x12, x10
    lsr  x14, x14, x12
    b    4f
3:  lsr  x15, x13, x10            /* SHR */
    sub  x12, x10, 1
    lsr  x14, x13, x12
4:  and  w14, w14, 1
    mov  w16, 0
    cmp  x10, 1
    b.ne 5f
    cmp  x9, 4
    b.eq 6f
    cmp  x9, 5
    b.eq 7f
    b    5f
6:  lsr  x16, x15, 63
    eor  w16, w16, w14
    b    5f
7:  lsr  x16, x13, 63
5:  str  x15, [x11]
    amd64_cached_set_shift_flags 63
.endif
9:  gret 1
.endm

amd64_gen_shift_imm shift_imm32, 4
amd64_gen_shift_imm shift_imm64, 8

# ROL (/0) / ROR (/1) reg, CL (0xd3) or reg, 1 (0xd1), 32/64, flush style (covers all
# regs incl r8-r15 AND low-8, since rotate-by-cl/1 had NO native path -- group 0/1 always
# bridged). Identical result/CF/OF math to amd64_gen_rotate_imm; the ONLY difference is
# the count source: bit8 of the operand (use_cl) selects CL (= regs[rcx]=regs[1] low byte)
# vs the literal 1. Count is masked %size; effective==0 -> NO flag change (x86); OF defined
# only for effective==1; matches interp amd64_set_rotate_flags (which uses count%size for
# both the no-op and the ==1 checks). Operand: rm bits0-3, group bit4 (0=ROL/1=ROR), use_cl
# bit8.
.macro amd64_gen_rotate_count name, vbytes
.gadget amd64_\name
    ldr  x8, [_ip]
    and  x9, x8, 0xf                 /* rm reg id */
    add  x11, _cpu, CPU_amd64_regs
    add  x11, x11, x9, lsl 3         /* &regs[rm] */
    tbz  x8, 8, 12f                  /* use_cl? */
    ldr  x10, [_cpu, (CPU_amd64_regs + 8)]  /* CL = low byte of rcx=regs[1] */
    and  x10, x10, 0xff
    b    13f
12: mov  x10, 1                      /* reg,1 form */
13:
.if \vbytes == 4
    and  x10, x10, 31                /* effective = count % 32 */
    ldr  w13, [x11]
    cbnz x10, 3f
    str  x13, [x11]                  /* a count of 0 still writes the 32-bit */
    b    9f                          /* register: zero-extended (hardware) */
3:  tbnz x8, 4, 1f
    mov  w12, 32                     /* ROL32: ror by 32-eff */
    sub  w12, w12, w10
    ror  w15, w13, w12
    and  w14, w15, 1                 /* cf = result & 1 */
    lsr  w16, w15, 31
    eor  w16, w16, w14               /* of = cf ^ MSB */
    b    2f
1:  ror  w15, w13, w10               /* ROR32 */
    lsr  w14, w15, 31                /* cf = MSB */
    ubfx w16, w15, 30, 1             /* next-MSB (bit 30) */
    eor  w16, w16, w14               /* ROR OF (1-bit) = MSB ^ next-MSB */
2:  ubfx x12, x15, 0, 32
    str  x12, [x11]
.else
    and  x10, x10, 63                /* effective = count % 64 */
    cbz  x10, 9f
    ldr  x13, [x11]
    tbnz x8, 4, 1f
    mov  x12, 64                     /* ROL64 */
    sub  x12, x12, x10
    ror  x15, x13, x12
    and  x14, x15, 1
    lsr  x16, x15, 63
    eor  x16, x16, x14
    b    2f
1:  ror  x15, x13, x10               /* ROR64 */
    lsr  x14, x15, 63
    ubfx x16, x15, 62, 1             /* next-MSB (bit 62) */
    eor  x16, x16, x14               /* ROR OF (1-bit) = MSB ^ next-MSB */
2:  str  x15, [x11]
.endif
    strb w14, [_cpu, CPU_cf]
    ldr  w17, [_cpu, CPU_eflags]
    bic  w17, w17, 1
    orr  w17, w17, w14               /* eflags CF (bit 0) */
    cmp  x10, 1
    b.ne 8f                          /* OF defined only for 1-bit rotate */
    strb w16, [_cpu, CPU_of]
    bic  w17, w17, (1 << 11)
    orr  w17, w17, w16, lsl 11       /* eflags OF (bit 11) */
8:  str  w17, [_cpu, CPU_eflags]
9:  gret 1
.endm

amd64_gen_rotate_count rotate_count32, 4
amd64_gen_rotate_count rotate_count64, 8

# SHL (/4) / SHR (/5) / SAR (/7) reg, CL (0xd3) or reg, 1 (0xd1), 32/64, flush style for
# r8-r15 (low-8 already native via cached_shift_reg_count). Identical result/CF/OF math to
# amd64_gen_shift_imm; only the count source differs (bit8 use_cl -> CL=regs[1] low byte,
# otherwise literal 1). count%size==0 -> no change/no flags. Operand: rm bits0-3, group
# bits4-7, use_cl bit8.
.macro amd64_gen_shift_count name, vbytes
.gadget amd64_\name
    ldr  x8, [_ip]
    and  x11, x8, 0xf                /* rm id */
    ubfx x9, x8, 4, 4               /* group (4 shl / 5 shr / 7 sar) */
    add  x13, _cpu, CPU_amd64_regs
    add  x11, x13, x11, lsl 3       /* &regs[rm] */
    tbz  x8, 8, 12f                 /* use_cl? */
    ldr  x10, [_cpu, (CPU_amd64_regs + 8)]  /* CL */
    and  x10, x10, 0xff
    b    13f
12: mov  x10, 1
13:
.if \vbytes == 4
    and  x10, x10, 31
    cbz  x10, 9f
    ldr  w13, [x11]
    cmp  x9, 4
    b.eq 2f
    cmp  x9, 5
    b.eq 3f
    asr  w15, w13, w10              /* SAR */
    sub  w12, w10, 1
    lsr  w14, w13, w12
    b    4f
2:  lsl  w15, w13, w10             /* SHL */
    mov  w14, w13
    mov  w12, 32
    sub  w12, w12, w10
    lsr  w14, w14, w12
    b    4f
3:  lsr  w15, w13, w10             /* SHR */
    sub  w12, w10, 1
    lsr  w14, w13, w12
4:  and  w14, w14, 1
    mov  w16, 0
    cmp  x10, 1
    b.ne 5f
    cmp  x9, 4
    b.eq 6f
    cmp  x9, 5
    b.eq 7f
    b    5f
6:  lsr  w16, w15, 31
    eor  w16, w16, w14
    b    5f
7:  lsr  w16, w13, 31
5:  mov  w15, w15
    ubfx x12, x15, 0, 32
    str  x12, [x11]
    amd64_cached_set_shift_flags 31
.else
    and  x10, x10, 63
    cbz  x10, 9f
    ldr  x13, [x11]
    cmp  x9, 4
    b.eq 2f
    cmp  x9, 5
    b.eq 3f
    asr  x15, x13, x10             /* SAR */
    sub  x12, x10, 1
    lsr  x14, x13, x12
    b    4f
2:  lsl  x15, x13, x10            /* SHL */
    mov  x14, x13
    mov  x12, 64
    sub  x12, x12, x10
    lsr  x14, x14, x12
    b    4f
3:  lsr  x15, x13, x10            /* SHR */
    sub  x12, x10, 1
    lsr  x14, x13, x12
4:  and  w14, w14, 1
    mov  w16, 0
    cmp  x10, 1
    b.ne 5f
    cmp  x9, 4
    b.eq 6f
    cmp  x9, 5
    b.eq 7f
    b    5f
6:  lsr  x16, x15, 63
    eor  w16, w16, w14
    b    5f
7:  lsr  x16, x13, 63
5:  str  x15, [x11]
    amd64_cached_set_shift_flags 63
.endif
9:  gret 1
.endm

amd64_gen_shift_count shift_count32, 4
amd64_gen_shift_count shift_count64, 8

.macro def_amd64_cached_logic_reg_reg sfx
.gadget amd64_cached_logic_reg_reg\sfx
    ldr x8, [_ip]
    and x9, x8, 0xff
    ubfx x10, x8, 8, 4
    ubfx x11, x8, 12, 4
    ubfx x12, x8, 16, 7
    mov x0, x12
    tbnz x9, 1, 1f
    mov x14, x11
    mov x13, x10
    b 2f
1:  mov x14, x10
    mov x13, x11
2:  amd64_cached_read x14, x15
    amd64_cached_read x13, x16
    cmp x9, 0x20
    b.hs 3f
    orr x15, x15, x16
    b 5f
3:  cmp x9, 0x30
    b.hs 4f
    and x15, x15, x16
    b 5f
4:  eor x15, x15, x16
5:  amd64_cached_mask_result x15, x0
    amd64_cached_write x14, x15, x0
    amd64_cached_set_logic_flags x15, x0
    gret 1

.endm
def_amd64_cached_logic_reg_reg
.set AMD64_NF, 1
def_amd64_cached_logic_reg_reg _nf
.set AMD64_NF, 0
.purgem def_amd64_cached_logic_reg_reg
.gadget amd64_hybrid_logic_reg_reg
    ldr x8, [_ip]
    and x9, x8, 0xff
    ubfx x10, x8, 8, 4
    ubfx x11, x8, 12, 4
    ubfx x12, x8, 16, 7
    mov x0, x12
    tbnz x9, 1, 1f
    mov x14, x11
    mov x13, x10
    b 2f
1:  mov x14, x10
    mov x13, x11
2:  amd64_read_low_or_mem x14, x15
    amd64_read_low_or_mem x13, x16
    cmp x9, 0x20
    b.hs 3f
    orr x15, x15, x16
    b 5f
3:  cmp x9, 0x30
    b.hs 4f
    and x15, x15, x16
    b 5f
4:  eor x15, x15, x16
5:  amd64_cached_mask_result x15, x0
    amd64_write_low_or_mem x14, x15, x0
    amd64_cached_set_logic_flags x15, x0
    gret 1

.gadget amd64_cached_test_reg_reg
    ldr x8, [_ip]
    and x9, x8, 0xf
    ubfx x10, x8, 4, 4
    ubfx x11, x8, 8, 7
    amd64_cached_read x9, x13
    amd64_cached_read x10, x14
    and x15, x13, x14
    amd64_cached_mask_result x15, x11
    amd64_cached_set_logic_flags x15, x11
    gret 1

.gadget amd64_hybrid_test_reg_reg
    ldr x8, [_ip]
    and x9, x8, 0xf
    ubfx x10, x8, 4, 4
    ubfx x11, x8, 8, 7
    amd64_read_low_or_mem x9, x13
    amd64_read_low_or_mem x10, x14
    and x15, x13, x14
    amd64_cached_mask_result x15, x11
    amd64_cached_set_logic_flags x15, x11
    gret 1

.gadget amd64_cached_test_reg_imm
    ldr x8, [_ip]
    ldr x10, [_ip, 8]
    and x9, x8, 0xf
    ubfx x11, x8, 4, 7
    amd64_cached_read x9, x13
    and x15, x13, x10
    amd64_cached_mask_result x15, x11
    amd64_cached_set_logic_flags x15, x11
    gret 2

.gadget amd64_cached_xor_zero_reg
    ldr x8, [_ip]
    and x10, x8, 0xf
    ubfx x11, x8, 4, 7
    mov x12, 0
    amd64_cached_write x10, x12, x11
    ldr w12, [_cpu, CPU_eflags]
    mov w13, (1 | PF_FLAG | AF_FLAG | SF_FLAG | (1 << 11))
    bic w12, w12, w13
    orr w12, w12, PF_FLAG
    orr w12, w12, ZF_FLAG
    strb wzr, [_cpu, CPU_cf]
    strb wzr, [_cpu, CPU_of]
    ldrb w13, [_cpu, CPU_flags_res]
    bic w13, w13, (ZF_RES | SF_RES | PF_RES | AF_OPS)
    strb w13, [_cpu, CPU_flags_res]
    str w12, [_cpu, CPU_eflags]
    gret 1

.gadget amd64_mov64_reg_reg
    ldr x8, [_ip]
    and x9, x8, 0xf
    ubfx x10, x8, 4, 4
    add x11, _cpu, CPU_amd64_regs
    ldr x12, [x11, x9, lsl 3]
    str x12, [x11, x10, lsl 3]
    gret 1

.gadget amd64_mov32_reg_reg
    ldr x8, [_ip]
    and x9, x8, 0xf
    ubfx x10, x8, 4, 4
    add x11, _cpu, CPU_amd64_regs
    add x13, x11, x9, lsl 3
    ldr w12, [x13]
    str x12, [x11, x10, lsl 3]
    gret 1

.gadget amd64_mov16_reg_reg
    ldr x8, [_ip]
    and x9, x8, 0xf
    ubfx x10, x8, 4, 4
    add x11, _cpu, CPU_amd64_regs
    add x13, x11, x9, lsl 3
    ldrh w12, [x13]
    add x13, x11, x10, lsl 3
    strh w12, [x13]
    gret 1

.gadget amd64_mov8_reg_reg
    ldr x8, [_ip]
    and x9, x8, 0xf
    ubfx x10, x8, 4, 4
    add x11, _cpu, CPU_amd64_regs
    add x13, x11, x9, lsl 3
    ldrb w12, [x13]
    add x13, x11, x10, lsl 3
    strb w12, [x13]
    gret 1

.gadget amd64_mov_imm_reg
    ldr x8, [_ip]
    ldr x9, [_ip, 8]
    and x10, x8, 0xf
    ubfx x11, x8, 4, 7
    add x12, _cpu, CPU_amd64_regs
    add x12, x12, x10, lsl 3
    cmp x11, 64
    b.eq 1f
    cmp x11, 32
    b.eq 2f
    ldr x13, [x12]
    bfi x13, x9, 0, 16
    str x13, [x12]
    gret 2
1:
    str x9, [x12]
    gret 2
2:
    str x9, [x12]
    gret 2

.gadget amd64_mov_imm8_reg
    ldr x8, [_ip]
    ldr x9, [_ip, 8]
    and x10, x8, 0xf
    add x11, _cpu, CPU_amd64_regs
    add x11, x11, x10, lsl 3
    strb w9, [x11]
    gret 2

.gadget amd64_xchg_rax_reg
    ldr x8, [_ip]
    and x9, x8, 0xf
    ubfx x10, x8, 4, 7
    add x11, _cpu, CPU_amd64_regs
    ldr x12, [x11]
    ldr x13, [x11, x9, lsl 3]
    cmp x10, 64
    b.eq 1f
    cmp x10, 32
    b.eq 2f
    strh w13, [x11]
    add x14, x11, x9, lsl 3
    strh w12, [x14]
    gret 1
1:
    str x13, [x11]
    str x12, [x11, x9, lsl 3]
    gret 1
2:
    mov w12, w12
    mov w13, w13
    str x13, [x11]
    str x12, [x11, x9, lsl 3]
    gret 1

.gadget amd64_xchg_reg_reg
    ldr x8, [_ip]
    and x9, x8, 0xf
    ubfx x10, x8, 4, 4
    ubfx x11, x8, 8, 7
    add x12, _cpu, CPU_amd64_regs
    add x13, x12, x9, lsl 3
    add x14, x12, x10, lsl 3
    cmp x11, 64
    b.eq 1f
    cmp x11, 32
    b.eq 2f
    cmp x11, 16
    b.eq 3f
    ldrb w15, [x13]
    ldrb w16, [x14]
    strb w16, [x13]
    strb w15, [x14]
    gret 1
1:
    ldr x15, [x13]
    ldr x16, [x14]
    str x16, [x13]
    str x15, [x14]
    gret 1
2:
    ldr w15, [x13]
    ldr w16, [x14]
    str x16, [x13]
    str x15, [x14]
    gret 1
3:
    ldrh w15, [x13]
    ldrh w16, [x14]
    strh w16, [x13]
    strh w15, [x14]
    gret 1

.gadget amd64_sign_extend
    ldr x8, [_ip]
    and x9, x8, 0xff
    ubfx x10, x8, 8, 7
    add x11, _cpu, CPU_amd64_regs
    ldr x12, [x11]
    cmp x9, 0x99
    b.eq 3f
    cmp x10, 64
    b.eq 1f
    cmp x10, 32
    b.eq 2f
    sxtb w13, w12
    strh w13, [x11]
    gret 1
1:
    sxtw x12, w12
    str x12, [x11]
    gret 1
2:
    sxth w12, w12
    str x12, [x11]
    gret 1
3:
    cmp x10, 64
    b.eq 4f
    cmp x10, 32
    b.eq 5f
    sxth w13, w12
    asr w13, w13, 15
    strh w13, [x11, 16]
    gret 1
4:
    asr x12, x12, 63
    str x12, [x11, 16]
    gret 1
5:
    asr w12, w12, 31
    str x12, [x11, 16]
    gret 1

.gadget amd64_movx_reg_reg
    ldr x8, [_ip]
    and x9, x8, 0xff
    ubfx x10, x8, 8, 4
    ubfx x11, x8, 12, 4
    ubfx x12, x8, 16, 7
    add x13, _cpu, CPU_amd64_regs
    add x14, x13, x10, lsl 3
    cmp x9, 0xb7
    b.eq 2f
    cmp x9, 0xbf
    b.eq 3f
    ldrb w15, [x14]
    cmp x9, 0xbe
    b.ne 4f
    sxtb x15, w15
    b 4f
2:
    ldrh w15, [x14]
    b 4f
3:
    ldrsh x15, [x14]
4:
    cmp x12, 64
    b.eq 5f
    mov w15, w15
5:
    str x15, [x13, x11, lsl 3]
    gret 1

# MOVZX/MOVSX between any registers, every destination width, AH..BH
# sources. packed(0) = op2 (0-7) | src (8-11) | dst (12-15) | dst size (16-22) |
# src is AH..BH (24). gret 1.
.gadget amd64_movx_any
    ldr  x8, [_ip]
    and  x9, x8, 0xff
    ubfx x10, x8, 8, 4
    ubfx x11, x8, 12, 4
    ubfx x12, x8, 16, 7
    add  x13, _cpu, CPU_amd64_regs
    ldr  x15, [x13, x10, lsl 3]
    tbz  x8, 24, 1f
    lsr  x15, x15, 8
1:  cmp  x9, 0xb7
    b.eq 2f
    cmp  x9, 0xbf
    b.eq 3f
    cmp  x9, 0xbe
    b.eq 4f
    and  x15, x15, 0xff              /* B6 */
    b    6f
4:  sxtb x15, w15                    /* BE */
    b    6f
2:  and  x15, x15, 0xffff            /* B7 */
    b    6f
3:  sxth x15, w15                    /* BF */
6:  cmp  x12, 64
    b.eq 8f
    cmp  x12, 32
    b.eq 7f
    ldr  x14, [x13, x11, lsl 3]      /* 16: the low word only */
    bfi  x14, x15, 0, 16
    mov  x15, x14
    b    8f
7:  mov  w15, w15
8:  str  x15, [x13, x11, lsl 3]
    gret 1

.gadget amd64_movsxd_reg_reg
    ldr x8, [_ip]
    and x9, x8, 0xf
    ubfx x10, x8, 4, 4
    ubfx x11, x8, 8, 7
    add x12, _cpu, CPU_amd64_regs
    add x13, x12, x9, lsl 3
    cmp x11, 64
    b.eq 1f
    ldrsh w14, [x13]
    str x14, [x12, x10, lsl 3]
    gret 1
1:
    ldrsw x14, [x13]
    str x14, [x12, x10, lsl 3]
    gret 1

.gadget amd64_lea_reg_mem
    ldr x8, [_ip]
    ldr x9, [_ip, 8]
    ubfx x10, x8, 8, 4
    ubfx x11, x8, 12, 8
    ubfx x12, x8, 20, 4
    add x13, _cpu, CPU_amd64_regs
    tbz x8, 30, 1f
    ldr x14, [x13, x12, lsl 3]
    add x9, x9, x14
1:
    tbz x8, 31, 2f
    ubfx x12, x8, 24, 4
    ubfx x15, x8, 28, 2
    ldr x14, [x13, x12, lsl 3]
    lsl x14, x14, x15
    add x9, x9, x14
2:
    tbz x8, 32, 3f
    ldr x14, [_ip, 16]
    add x9, x9, x14
3:
    cmp x11, 64
    b.eq 4f
    cmp x11, 32
    b.eq 5f
    add x12, x13, x10, lsl 3
    strh w9, [x12]
    gret 3
4:
    str x9, [x13, x10, lsl 3]
    gret 3
5:
    add x12, x13, x10, lsl 3
    mov w9, w9
    str x9, [x12]
    gret 3

.gadget amd64_cached_lea_reg_mem
    ldr x8, [_ip]
    ldr x9, [_ip, 8]
    ubfx x10, x8, 8, 4
    ubfx x11, x8, 12, 8
    tbz x8, 30, 1f
    ubfx x0, x8, 20, 4
    amd64_read_low_or_mem x0, x14
    add x9, x9, x14
1:
    tbz x8, 31, 2f
    ubfx x0, x8, 24, 4
    ubfx x15, x8, 28, 2
    amd64_read_low_or_mem x0, x14
    lsl x14, x14, x15
    add x9, x9, x14
2:
    tbz x8, 32, 3f
    ldr x14, [_ip, 16]
    add x9, x9, x14
3:
    amd64_write_low_or_mem x10, x9, x11
    gret 3

.gadget amd64_bswap_reg
    ldr x8, [_ip]
    and x9, x8, 0xf
    ubfx x10, x8, 4, 7
    add x11, _cpu, CPU_amd64_regs
    ldr x12, [x11, x9, lsl 3]
    cmp x10, 64
    b.eq 1f
    rev w12, w12
    str x12, [x11, x9, lsl 3]
    gret 1
1:
    rev x12, x12
    str x12, [x11, x9, lsl 3]
    gret 1

.gadget amd64_test_reg_reg
    ldr x8, [_ip]
    and x9, x8, 0xf
    ubfx x10, x8, 4, 4
    ubfx x11, x8, 8, 7
    add x12, _cpu, CPU_amd64_regs
    ldr x13, [x12, x9, lsl 3]
    ldr x14, [x12, x10, lsl 3]
    and x15, x13, x14
    cmp x11, 64
    b.eq 1f
    cmp x11, 32
    b.eq 2f
    cmp x11, 16
    b.eq 4f
    and w15, w15, 0xff
    b 3f
4:
    uxth w15, w15
    b 3f
2:
    mov w15, w15
3:
1:
    ldr w16, [_cpu, CPU_eflags]
    mov w17, (1 | PF_FLAG | AF_FLAG | ZF_FLAG | SF_FLAG | (1 << 11))
    bic w16, w16, w17
    strb wzr, [_cpu, CPU_cf]
    strb wzr, [_cpu, CPU_of]
    ldrb w17, [_cpu, CPU_flags_res]
    bic w17, w17, (ZF_RES | SF_RES | PF_RES | AF_OPS)
    strb w17, [_cpu, CPU_flags_res]
    cmp x15, 0
    cset w17, eq
    orr w16, w16, w17, lsl 6
    and w17, w15, 0xff
    eor w17, w17, w17, lsr 4
    eor w17, w17, w17, lsr 2
    eor w17, w17, w17, lsr 1
    mvn w17, w17
    and w17, w17, 1
    orr w16, w16, w17, lsl 2
    cmp x11, 64
    b.eq 4f
    cmp x11, 32
    b.eq 5f
    cmp x11, 16
    b.eq 8f
    tst w15, 0x80
    b.eq 6f
    orr w16, w16, SF_FLAG
    b 6f
8:
    tst w15, 0x8000
    b.eq 6f
    orr w16, w16, SF_FLAG
    b 6f
5:
    tbnz w15, 31, 7f
    b 6f
4:
    tbnz x15, 63, 7f
    b 6f
7:
    orr w16, w16, SF_FLAG
6:
    str w16, [_cpu, CPU_eflags]
    gret 1

.gadget amd64_test_reg_imm
    ldr x8, [_ip]
    ldr x10, [_ip, 8]
    and x9, x8, 0xf
    ubfx x11, x8, 4, 7
    add x12, _cpu, CPU_amd64_regs
    ldr x13, [x12, x9, lsl 3]
    and x15, x13, x10
    cmp x11, 64
    b.eq 1f
    cmp x11, 32
    b.eq 2f
    cmp x11, 16
    b.eq 4f
    and w15, w15, 0xff
    b 3f
4:
    uxth w15, w15
    b 3f
2:
    mov w15, w15
3:
1:
    ldr w16, [_cpu, CPU_eflags]
    mov w17, (1 | PF_FLAG | AF_FLAG | ZF_FLAG | SF_FLAG | (1 << 11))
    bic w16, w16, w17
    strb wzr, [_cpu, CPU_cf]
    strb wzr, [_cpu, CPU_of]
    ldrb w17, [_cpu, CPU_flags_res]
    bic w17, w17, (ZF_RES | SF_RES | PF_RES | AF_OPS)
    strb w17, [_cpu, CPU_flags_res]
    cmp x15, 0
    cset w17, eq
    orr w16, w16, w17, lsl 6
    and w17, w15, 0xff
    eor w17, w17, w17, lsr 4
    eor w17, w17, w17, lsr 2
    eor w17, w17, w17, lsr 1
    mvn w17, w17
    and w17, w17, 1
    orr w16, w16, w17, lsl 2
    cmp x11, 64
    b.eq 4f
    cmp x11, 32
    b.eq 5f
    cmp x11, 16
    b.eq 8f
    tst w15, 0x80
    b.eq 6f
    orr w16, w16, SF_FLAG
    b 6f
8:
    tst w15, 0x8000
    b.eq 6f
    orr w16, w16, SF_FLAG
    b 6f
5:
    tbnz w15, 31, 7f
    b 6f
4:
    tbnz x15, 63, 7f
    b 6f
7:
    orr w16, w16, SF_FLAG
6:
    str w16, [_cpu, CPU_eflags]
    gret 2

.gadget amd64_logic_reg_reg
    ldr x8, [_ip]
    and x9, x8, 0xff
    ubfx x10, x8, 8, 4
    ubfx x11, x8, 12, 4
    ubfx x12, x8, 16, 7
    tbnz x9, 1, 1f
    mov x14, x11
    mov x15, x10
    b 2f
1:
    mov x14, x10
    mov x15, x11
2:
    add x13, _cpu, CPU_amd64_regs
    ldr x16, [x13, x14, lsl 3]
    ldr x17, [x13, x15, lsl 3]
    cmp x9, 0x20
    b.hs 3f
    orr x15, x16, x17
    b 5f
3:
    cmp x9, 0x30
    b.hs 4f
    and x15, x16, x17
    b 5f
4:
    eor x15, x16, x17
5:
    add x14, x13, x14, lsl 3
    cmp x12, 64
    b.eq 6f
    cmp x12, 32
    b.eq 7f
    cmp x12, 16
    b.eq 8f
    strb w15, [x14]
    and w15, w15, 0xff
    b 9f
8:
    strh w15, [x14]
    uxth w15, w15
    b 9f
7:
    mov w15, w15
    str x15, [x14]
    b 9f
6:
    str x15, [x14]
9:
    ldr w16, [_cpu, CPU_eflags]
    mov w17, (1 | PF_FLAG | AF_FLAG | ZF_FLAG | SF_FLAG | (1 << 11))
    bic w16, w16, w17
    strb wzr, [_cpu, CPU_cf]
    strb wzr, [_cpu, CPU_of]
    ldrb w17, [_cpu, CPU_flags_res]
    bic w17, w17, (ZF_RES | SF_RES | PF_RES | AF_OPS)
    strb w17, [_cpu, CPU_flags_res]
    cmp x15, 0
    cset w17, eq
    orr w16, w16, w17, lsl 6
    and w17, w15, 0xff
    eor w17, w17, w17, lsr 4
    eor w17, w17, w17, lsr 2
    eor w17, w17, w17, lsr 1
    mvn w17, w17
    and w17, w17, 1
    orr w16, w16, w17, lsl 2
    cmp x12, 64
    b.eq 10f
    cmp x12, 32
    b.eq 11f
    cmp x12, 16
    b.eq 12f
    tst w15, 0x80
    b.eq 13f
    orr w16, w16, SF_FLAG
    b 13f
12:
    tst w15, 0x8000
    b.eq 13f
    orr w16, w16, SF_FLAG
    b 13f
11:
    tbnz w15, 31, 14f
    b 13f
10:
    tbnz x15, 63, 14f
    b 13f
14:
    orr w16, w16, SF_FLAG
13:
    str w16, [_cpu, CPU_eflags]
    gret 1

# Flush-style ADD/SUB/CMP reg,reg (0x00-03/28-2b/38-3b), 32/64 -- the high-reg
# sibling of cached_arith_reg_reg (which is low-8 only). SHA-512's `add r8,r9` was the
# top remaining C bridge (amd64_jit_reg_reg_op); arith had a cached path but, unlike
# logic, no flush/hybrid fallback, so any r8-r15 operand bridged. gen.c flushes the
# cache, this reads/writes operands straight from CPU_amd64_regs. Same add/sub math +
# eager addsub flags as cached_arith_reg_reg; CMP (>=0x38) sets flags but no write.
# Operand: opcode bits 0-7, reg bits 8-11, rm bits 12-15, size bits 16-22.
.macro def_amd64_arith_reg_reg sfx
.gadget amd64_arith_reg_reg\sfx
    ldr  x8, [_ip]
    and  x9, x8, 0xff
    ubfx x10, x8, 8, 4
    ubfx x11, x8, 12, 4
    ubfx x0, x8, 16, 7
    add  x12, _cpu, CPU_amd64_regs
    tst  x9, 2
    b.ne 1f
    mov  x8, x11                     /* d=0: dst = rm */
    ldr  x13, [x12, x11, lsl 3]
    ldr  x10, [x12, x10, lsl 3]
    b    2f
1:  mov  x8, x10                     /* d=1: dst = reg */
    ldr  x13, [x12, x10, lsl 3]
    ldr  x10, [x12, x11, lsl 3]
2:  cmp  x0, 64
    b.eq 3f
    mov  w13, w13
    mov  w10, w10
    cmp  x9, 0x28
    b.hs 4f
    adds w15, w13, w10
    cset w14, cs
    cset w16, vs
    b    5f
4:  subs w15, w13, w10
    cset w14, cc
    cset w16, vs
5:  mov  w15, w15
    amd64_cached_set_addsub_flags 31
    cmp  x9, 0x38
    b.hs 8f
    add  x12, _cpu, CPU_amd64_regs
    ubfx x13, x15, 0, 32
    str  x13, [x12, x8, lsl 3]
8:  gret 1
3:  cmp  x9, 0x28
    b.hs 6f
    adds x15, x13, x10
    cset w14, cs
    cset w16, vs
    b    7f
6:  subs x15, x13, x10
    cset w14, cc
    cset w16, vs
7:  amd64_cached_set_addsub_flags 63
    cmp  x9, 0x38
    b.hs 9f
    add  x12, _cpu, CPU_amd64_regs
    str  x15, [x12, x8, lsl 3]
9:  gret 1
.endm
def_amd64_arith_reg_reg
.set AMD64_NF, 1
def_amd64_arith_reg_reg _nf
.set AMD64_NF, 0
.purgem def_amd64_arith_reg_reg

# Flush-style ADD/SUB/CMP reg,imm (0x81/0x83 /0,/5,/7), 32/64 -- high-reg sibling of
# cached_arith_reg_imm (low-8 only); arith reg-imm had no flush fallback (logic does),
# so r8-r15 bridged via amd64_jit_reg_imm_op. Operand: group bits 8-11 (0 add/5 sub/
# 7 cmp), rm bits 12-15, size bits 16-22; imm = operand 1. CMP (/7) sets flags, no write.
.macro def_amd64_arith_reg_imm sfx
.gadget amd64_arith_reg_imm\sfx
    ldr  x8, [_ip]
    ldr  x10, [_ip, 8]
    ubfx x9, x8, 8, 4
    ubfx x11, x8, 12, 4
    ubfx x0, x8, 16, 7
    add  x12, _cpu, CPU_amd64_regs
    ldr  x13, [x12, x11, lsl 3]
    mov  x8, x11
    cmp  x0, 64
    b.eq 1f
    mov  w13, w13
    mov  w10, w10
    cmp  x9, 0
    b.eq 2f
    subs w15, w13, w10
    cset w14, cc
    cset w16, vs
    b    3f
2:  adds w15, w13, w10
    cset w14, cs
    cset w16, vs
3:  mov  w15, w15
    amd64_cached_set_addsub_flags 31
    cmp  x9, 7
    b.eq 4f
    add  x12, _cpu, CPU_amd64_regs
    ubfx x13, x15, 0, 32
    str  x13, [x12, x8, lsl 3]
4:  gret 2
1:  cmp  x9, 0
    b.eq 5f
    subs x15, x13, x10
    cset w14, cc
    cset w16, vs
    b    6f
5:  adds x15, x13, x10
    cset w14, cs
    cset w16, vs
6:  amd64_cached_set_addsub_flags 63
    cmp  x9, 7
    b.eq 7f
    add  x12, _cpu, CPU_amd64_regs
    str  x15, [x12, x8, lsl 3]
7:  gret 2

.endm
def_amd64_arith_reg_imm
.set AMD64_NF, 1
def_amd64_arith_reg_imm _nf
.set AMD64_NF, 0
.purgem def_amd64_arith_reg_imm
.gadget amd64_logic_reg_imm
    ldr x8, [_ip]
    ldr x10, [_ip, 8]
    ubfx x9, x8, 8, 4
    ubfx x11, x8, 12, 4
    ubfx x12, x8, 16, 7
    add x13, _cpu, CPU_amd64_regs
    add x14, x13, x11, lsl 3
    ldr x15, [x14]
    cmp x9, 4
    b.eq 1f
    cmp x9, 6
    b.eq 2f
    orr x15, x15, x10
    b 3f
1:
    and x15, x15, x10
    b 3f
2:
    eor x15, x15, x10
3:
    cmp x12, 64
    b.eq 4f
    cmp x12, 32
    b.eq 5f
    cmp x12, 16
    b.eq 6f
    strb w15, [x14]
    and w15, w15, 0xff
    b 7f
6:
    strh w15, [x14]
    uxth w15, w15
    b 7f
5:
    mov w15, w15
    str x15, [x14]
    b 7f
4:
    str x15, [x14]
7:
    ldr w16, [_cpu, CPU_eflags]
    mov w17, (1 | PF_FLAG | AF_FLAG | ZF_FLAG | SF_FLAG | (1 << 11))
    bic w16, w16, w17
    strb wzr, [_cpu, CPU_cf]
    strb wzr, [_cpu, CPU_of]
    ldrb w17, [_cpu, CPU_flags_res]
    bic w17, w17, (ZF_RES | SF_RES | PF_RES | AF_OPS)
    strb w17, [_cpu, CPU_flags_res]
    cmp x15, 0
    cset w17, eq
    orr w16, w16, w17, lsl 6
    and w17, w15, 0xff
    eor w17, w17, w17, lsr 4
    eor w17, w17, w17, lsr 2
    eor w17, w17, w17, lsr 1
    mvn w17, w17
    and w17, w17, 1
    orr w16, w16, w17, lsl 2
    cmp x12, 64
    b.eq 8f
    cmp x12, 32
    b.eq 9f
    cmp x12, 16
    b.eq 10f
    tst w15, 0x80
    b.eq 11f
    orr w16, w16, SF_FLAG
    b 11f
10:
    tst w15, 0x8000
    b.eq 11f
    orr w16, w16, SF_FLAG
    b 11f
9:
    tbnz w15, 31, 12f
    b 11f
8:
    tbnz x15, 63, 12f
    b 11f
12:
    orr w16, w16, SF_FLAG
11:
    str w16, [_cpu, CPU_eflags]
    gret 2

.gadget amd64_xor_zero_reg
    ldr x8, [_ip]
    and x9, x8, 0xf
    ubfx x10, x8, 4, 7
    add x11, _cpu, CPU_amd64_regs
    add x11, x11, x9, lsl 3
    cmp x10, 64
    b.eq 1f
    cmp x10, 32
    b.eq 2f
    strh wzr, [x11]
    b 3f
1:
    str xzr, [x11]
    b 3f
2:
    str xzr, [x11]
3:
    ldr w12, [_cpu, CPU_eflags]
    mov w13, (1 | PF_FLAG | AF_FLAG | SF_FLAG | (1 << 11))
    bic w12, w12, w13
    orr w12, w12, PF_FLAG
    orr w12, w12, ZF_FLAG
    strb wzr, [_cpu, CPU_cf]
    strb wzr, [_cpu, CPU_of]
    ldrb w13, [_cpu, CPU_flags_res]
    bic w13, w13, (ZF_RES | SF_RES | PF_RES | AF_OPS)
    strb w13, [_cpu, CPU_flags_res]
    str w12, [_cpu, CPU_eflags]
    gret 1

.macro mov32_reg_reg dst_name, dst, src_name, src
    .gadget mov32_reg_reg_\dst_name\()_\src_name
        mov \dst, \src
        gret_nomem
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

# \dst is the register the operation accumulates into. It is _tmp for every
# ordinary gadget (the load/store gadgets move the guest register in and out of
# _tmp around it); a fused reg,imm gadget passes the guest register itself so no
# load or store gadget is needed. Only the add/sub/adc/sbc and and/orr/eor paths
# honour \dst -- load, store, xchg, imul, the shifts and the bit ops still use
# _tmp explicitly, and must not be given a guest destination.
.macro _do_op op, arg, dst, size, s
    .ifc \op,load
        movs _tmp, \arg, \s
        uxts _tmp, _tmp, \s
        .exitm
    .else N .ifc \op,store
        movs \arg, _tmp, \s
        .exitm
    .endif N .endif

    .ifin(\op, add,sub,adc,sbc)
        setf_ops \arg, \dst, \s
    .endifin
    .ifin(\op, and,orr,eor)
        clearf_oc
    .endifin
    .ifin(\op, adc,sbc)
        ldrb w10, [_cpu, CPU_cf]
        .ifc \op,adc
            cmp w10, 1
        .else
            mvn w10, w10
            cmn w10, 1
        .endif
    .endifin

    .ifin(\op, and,orr,eor)
        \op \dst, \dst, \arg
    .endifin

    .ifin(\op, add,sub,adc,sbc)
        do_add \op, \dst, \arg, \s
    .endifin

    .ifc \op,imul
        .ifnb \s
            sxt\s w10, \arg
            sxt\s _tmp, _tmp            /* dst is zero-extended on load; sign-extend it for the signed multiply */
            mul _tmp, _tmp, w10
            cmp _tmp, _tmp, sxt\s
        .else
            smull _xtmp, _tmp, \arg
            cmp _xtmp, _tmp, sxtw
        .endif
        cset w10, ne
        strb w10, [_cpu, CPU_cf]
        strb w10, [_cpu, CPU_of]
    .endif

    .ifin(\op, bsf,bsr)
        .ifnb \s
            uxt\s w10, \arg
        .else
            mov w10, \arg
        .endif
        .ifc \op,bsf
            .if \size != 32
                orr w10, w10, 1<<\size
            .endif
            rbit w10, w10
            clz w10, w10
            cmp w10, \size
        .else
            clz w10, w10
            .if \size != 32
                sub w10, w10, 32-\size
            .endif
            cmp w10, \size
            mov w9, \size-1
            sub w10, w9, w10
        .endif
        csel _tmp, w10, _tmp, ne
        cset w10, eq
        ldrb w9, [_cpu, CPU_eflags]
        bic w9, w9, ZF_FLAG
        orr w9, w9, w10, lsl 6
        strb w9, [_cpu, CPU_eflags]
        ldrb w9, [_cpu, CPU_flags_res]
        bic w9, w9, ZF_RES
        strb w9, [_cpu, CPU_flags_res]
    .endifin

    # POPCNT: no scalar GPR popcount on aarch64, so bounce through NEON --
    # this is the first NEON use in this macro; d0/v0 are caller-saved, dead
    # after gret, and no C call happens in between, so they're free scratch
    # (see the identical reasoning next to the amd64 scalar-double gadgets
    # above). fmov d0,x10 zero-extends the size-masked value into all 8
    # bytes of v0 (upper bytes already 0 from the uxt/mov above), cnt.8b
    # gives a per-byte popcount, addv sums the 8 lanes into b0 (max 32, fits
    # a byte), and the addv write zeroes the rest of v0 per the AArch64
    # upper-bits-zeroed rule so the fmov back out needs no masking.
    .ifc \op,popcnt
        .ifnb \s
            uxt\s w10, \arg
        .else
            mov w10, \arg
        .endif
        fmov d0, x10
        cnt v0.8b, v0.8b
        addv b0, v0.8b
        fmov w10, s0
        mov _tmp, w10
        strb wzr, [_cpu, CPU_cf]
        strb wzr, [_cpu, CPU_of]
        cmp w10, 0
        cset w9, eq
        ldrb w8, [_cpu, CPU_eflags]
        # bic's immediate form only accepts AArch64 logical-immediate bit
        # patterns, and this OR of four single-bit flags isn't one -- clear
        # each bit individually instead.
        bic w8, w8, PF_FLAG
        bic w8, w8, AF_FLAG
        bic w8, w8, ZF_FLAG
        bic w8, w8, SF_FLAG
        orr w8, w8, w9, lsl 6
        strb w8, [_cpu, CPU_eflags]
        ldrb w8, [_cpu, CPU_flags_res]
        bic w8, w8, PF_RES
        bic w8, w8, ZF_RES
        bic w8, w8, SF_RES
        bic w8, w8, AF_OPS
        strb w8, [_cpu, CPU_flags_res]
    .endif

    # TZCNT/LZCNT: unlike bsf/bsr, the result is architecturally defined as
    # the operand size when the source is zero, and CF mirrors that
    # zero-source condition (ZF mirrors the result instead). The rbit+clz
    # (tzcnt) / clz (lzcnt) sequences below already yield exactly that
    # zero-defined value by construction -- see the bsf/bsr block above,
    # which computes the identical intermediate before converting it to a
    # bit-index; tzcnt/lzcnt use it directly with no conversion or csel.
    .ifin(\op, tzcnt,lzcnt)
        .ifnb \s
            uxt\s w11, \arg
        .else
            mov w11, \arg
        .endif
        mov w10, w11
        .ifc \op,tzcnt
            .if \size != 32
                orr w10, w10, 1<<\size
            .endif
            rbit w10, w10
            clz w10, w10
        .else
            clz w10, w10
            .if \size != 32
                sub w10, w10, 32-\size
            .endif
        .endif
        mov _tmp, w10
        cmp w11, 0
        cset w9, eq
        strb w9, [_cpu, CPU_cf]
        cmp w10, 0
        cset w9, eq
        ldrb w8, [_cpu, CPU_eflags]
        bic w8, w8, ZF_FLAG
        orr w8, w8, w9, lsl 6
        strb w8, [_cpu, CPU_eflags]
        ldrb w8, [_cpu, CPU_flags_res]
        bic w8, w8, ZF_RES
        strb w8, [_cpu, CPU_flags_res]
    .endifin

    .ifc \op,xchg
        mov w9, _tmp
        mov _tmp, \arg
        movs \arg, w9, \s
    .endif

    .ifin(\op, add,sub,adc,sbc)
        setf_all_res \s, \dst
    .endifin
    .ifin(\op, and,orr,eor)
        setf_logic_res \s, \dst
    .endifin
.endm
# dst defaults to _tmp, so every existing caller is unchanged. It exists so a
# FUSED gadget can run the op directly on a guest register instead of the scratch,
# collapsing load(dst)+op(src)+store(dst) into one dispatch. `ss` expands to
# `\macro \args, \size, <suffix>`, so \dst must be the last of \args to land in
# _do_op's third parameter.
.macro do_op op, size, arg, dst=_tmp
    ss \size, _do_op, \op, \arg, \dst
.endm

.macro do_reg_op op, armop, size, reg
    .gadget \op\size\()_reg_\reg
        do_op \armop, \size, e\reg\()x
        gret_nomem
.endm

.macro do_hi_op op, size, reg
    ubfx w12, e\reg\()x, 8, 8
    do_op \op, \size, w12
    bfi e\reg\()x, w12, 8, 8
.endm

.macro do_op_size op, armop, size, s
    .ifnc \op,store
        .gadget \op\size\()_imm
            ldr\s w8, [_ip]
            do_op \armop, \size, w8
            gret_nomem 1
    .endif

    .ifnc \op,xchg
        .gadget \op\size\()_mem
            .ifc \op,store
                write_prep \size, \op\size\()_mem
            .else N .ifc \op,xchg
                write_prep \size, \op\size\()_mem
            .else
                read_prep \size, \op\size\()_mem
            .endif N .endif
            ldr\s w8, [_xaddr]
            do_op \armop, \size, w8
            .ifc \op,store
                str\s w8, [_xaddr]
                write_done \size, \op\size\()_mem
            .endif
            gret 1
            .ifc \op,store
                write_bullshit \size, \op\size\()_mem
            .else N .ifc \op,xchg
                write_bullshit \size, \op\size\()_mem
            .else
                read_bullshit \size, \op\size\()_mem
            .endif N .endif
    .else
        # xchg must be atomic
        .gadget \op\size\()_mem
            .if \size != 8   // on the guest address, before write_prep
                tst _addr, (\size/8)-1
                b.eq 9f
                ua_slow UA_XCHG, \size
            9:
            .endif
            write_prep \size, \op\size\()_mem
            dmb ish
        1:
            ldaxr\s w8, [_xaddr]
            stlxr\s w10, _tmp, [_xaddr]
            cbnz w10, 1b
            movs _tmp, w8
            dmb ish
            write_done \size, \op\size\()_mem
            gret 1
            write_bullshit \size, \op\size\()_mem
    .endif

    .irp reg, a,b,c,d
        do_reg_op \op, \armop, \size, \reg
    .endr

    .irp reg, si,di,sp,bp
        .gadget \op\size\()_reg_\reg
            .if \size == 8
                .ifc \reg,sp N do_hi_op \armop, \size, a N .else
                .ifc \reg,bp N do_hi_op \armop, \size, c N .else
                .ifc \reg,si N do_hi_op \armop, \size, d N .else
                .ifc \reg,di N do_hi_op \armop, \size, b
                .endif N .endif N .endif N .endif
            .else
                do_op \armop, \size, e\reg
            .endif
            gret_nomem
    .endr

.endm

.irp op, load,store,xchg,add,sub,adc,sbb,and,or,xor
    .irp size, SIZE_LIST
        # a couple operations have slightly different names on arm
        .ifc \op,xor
            ss \size, do_op_size, \op, eor
        .else N .ifc \op,sbb
            ss \size, do_op_size, \op, sbc
        .else N .ifc \op,or
            ss \size, do_op_size, \op, orr
        .else
            ss \size, do_op_size, \op, \op
        .endif N .endif N .endif
    .endr
    .gadget_array \op
.endr
.irp op, imul,bsf,bsr,popcnt,tzcnt,lzcnt
    .irp size, 16,32
        ss \size, do_op_size, \op, \op
    .endr
    .gadget_array \op
.endr

# ---- FUSED reg,imm ALU: load(dst) + op(imm) + store(dst) collapsed to ONE gadget ----
#
# Emitted by gen_alu_imm_fused() in jit/gen.c. The stream goes from
#     [load32_reg_<dst>][<op>32_imm][imm][store32_reg_<dst>]   4 words, 3 dispatches
# to
#     [fused_<op>32_imm_<dst>][imm]                            2 words, 1 dispatch
#
# Why this shape and not the reg,reg one: measured emission on a real gcc compile
# (207139 MOVs, 102510 ALU load-op-store trios) says reg,imm is ~60% of all ALU
# trios -- `add reg,imm` alone is 35.3% and `sub reg,imm` 18.1% -- while reg,reg
# is ~21%. reg,imm needs 7 ops x 8 destinations = 56 gadgets; reg,reg would need
# 7 x 64 = 448 for a third of the benefit, i.e. ~22x worse per byte of gadget text,
# which is I-cache this project cannot spend freely on old devices.
#
# The body is the ordinary imm-form gadget with the accumulator re-pointed from the
# _tmp scratch to the guest register itself, via do_op's dst parameter. All the rest
# of it -- the eager CF/OF, the lazy op1/op2/res/flags_res deposit, the per-family
# differences between the add group and the and/orr/eor group -- comes from the same
# _do_op expansion the unfused gadgets use, so the flag state is identical by
# construction rather than by inspection. Do NOT hand-write a flag epilogue here:
# the logic family also clears AF in CPU_eflags, which is easy to omit and invisible
# until af_ops happens to be 0.
#
# 32-bit ONLY, and that is load-bearing, not conservatism:
#   - at size 8 the gadget names reg_sp/bp/si/di mean the HIGH BYTES ah/ch/dh/bh
#     (see do_hi_op above and arg_reg_ah = arg_reg_sp in jit/gen.c), so an
#     8-register generator would silently operate on esp instead of ah;
#   - at 8/16 the load is a bfxil partial write and do_add's sized path writes a
#     sign-extended out-of-range intermediate into its destination to detect
#     overflow, so it cannot accumulate straight into a guest register.
# mov32_reg_reg is 32-bit only for the same reasons.
#
# CMP and TEST are deliberately absent: they use lo() (no store-back), and the
# cmp/test+jcc fusion identifies its target by pointer-matching the op word against
# sub_gadgets/and_gadgets. A fused cmp would not match, silently disabling a fusion
# worth far more than this one.
.macro xfa_add name, reg
    .gadget fused_add32_imm_\name
        ldr w8, [_ip]
        do_op add, 32, w8, \reg
        gret_nomem 1
.endm
.each_reg xfa_add
.purgem xfa_add

.macro xfa_or name, reg
    .gadget fused_or32_imm_\name
        ldr w8, [_ip]
        do_op orr, 32, w8, \reg
        gret_nomem 1
.endm
.each_reg xfa_or
.purgem xfa_or

.macro xfa_adc name, reg
    .gadget fused_adc32_imm_\name
        ldr w8, [_ip]
        do_op adc, 32, w8, \reg
        gret_nomem 1
.endm
.each_reg xfa_adc
.purgem xfa_adc

.macro xfa_sbb name, reg
    .gadget fused_sbb32_imm_\name
        ldr w8, [_ip]
        do_op sbc, 32, w8, \reg
        gret_nomem 1
.endm
.each_reg xfa_sbb
.purgem xfa_sbb

.macro xfa_and name, reg
    .gadget fused_and32_imm_\name
        ldr w8, [_ip]
        do_op and, 32, w8, \reg
        gret_nomem 1
.endm
.each_reg xfa_and
.purgem xfa_and

.macro xfa_sub name, reg
    .gadget fused_sub32_imm_\name
        ldr w8, [_ip]
        do_op sub, 32, w8, \reg
        gret_nomem 1
.endm
.each_reg xfa_sub
.purgem xfa_sub

.macro xfa_xor name, reg
    .gadget fused_xor32_imm_\name
        ldr w8, [_ip]
        do_op eor, 32, w8, \reg
        gret_nomem 1
.endm
.each_reg xfa_xor
.purgem xfa_xor

# One 8-entry array per op, indexed by (dst_reg - arg_reg_a). MUST be built from
# REG_LIST (a,c,d,b,sp,bp,si,di), which is enum arg's order -- NOT .each_reg's
# order (a,b,c,d,si,di,bp,sp), which is what instantiated the bodies above. Using
# the wrong order here yields a silently permuted table: the wrong register on
# every single operation, with no build error.
.gadget_list fused_add32_imm, REG_LIST
.gadget_list fused_or32_imm, REG_LIST
.gadget_list fused_adc32_imm, REG_LIST
.gadget_list fused_sbb32_imm, REG_LIST
.gadget_list fused_and32_imm, REG_LIST
.gadget_list fused_sub32_imm, REG_LIST
.gadget_list fused_xor32_imm, REG_LIST

# ---- FUSED reg,reg ALU: load(dst) + op(src) + store(dst) in ONE gadget ----
#
# Emitted by gen_alu_rr_fused() in jit/gen.c: [fused_<op>32_rr_<dst>_<src>],
# 1 word and 1 dispatch instead of 3 words and 3. The reg,imm note above priced
# this family out on a gcc profile (reg,reg ~21% of ALU trios); 7-Zip's LZMA
# loop is register ALU for a third of its instructions, and the staging
# load32_reg_*/store32_reg_* gadgets were ~30% of its i386 JIT time
# (2026-10-04). Same do_op body and the same 32-bit-only and no-CMP/TEST rules
# as the reg,imm family. src == dst (xor eax,eax) is fine: setf_ops records both
# operands before the op writes the destination.
.macro xfrr op, armop, dst_name, dst, src_name, src
    .gadget fused_\op\()32_rr_\dst_name\()_\src_name
        do_op \armop, 32, \src, \dst
        gret_nomem
.endm
.macro xfrr_ops dst_name, dst, src_name, src
    xfrr add, add, \dst_name, \dst, \src_name, \src
    xfrr or, orr, \dst_name, \dst, \src_name, \src
    xfrr adc, adc, \dst_name, \dst, \src_name, \src
    xfrr sbb, sbc, \dst_name, \dst, \src_name, \src
    xfrr and, and, \dst_name, \dst, \src_name, \src
    xfrr sub, sub, \dst_name, \dst, \src_name, \src
    xfrr xor, eor, \dst_name, \dst, \src_name, \src
.endm
.macro xfrr_row dst_name, dst
    .macro xfrr_col src_name, src
        xfrr_ops \dst_name, \dst, \src_name, \src
    .endm
    .each_reg xfrr_col
    .purgem xfrr_col
.endm
.each_reg xfrr_row
.purgem xfrr_row
.purgem xfrr_ops
.purgem xfrr

# [dst * 8 + src], both in REG_LIST order -- see the note above.
# (The name pasting lives in macros: clang consumes a \() at the outermost
# .irp, so a third nesting level would see "\dst_" instead of "\dst".)
.macro xfrr_quad op, dst, src
    .quad NAME(gadget_fused_\op\()32_rr_\dst\()_\src)
.endm
.macro xfrr_table op
    .global.name fused_\op\()32_rr_gadgets
.endm
.pushsection_rodata
.irp op, add,or,adc,sbb,and,sub,xor
    xfrr_table \op
    .irp dst, REG_LIST
        .irp src, REG_LIST
            xfrr_quad \op, \dst, \src
        .endr
    .endr
.endr
.popsection
.purgem xfrr_quad
.purgem xfrr_table

# ---- FUSED `mov <reg>, imm32`: [fused_movi32_<reg>][imm], 1 dispatch instead of 2 ----
# load32_imm + store32_reg_<reg> loaded the immediate into _tmp and moved it out.
# 1.6% of 7-Zip's i386 instructions (2026-10-04). No flags, no memory.
.macro xfmovi name, reg
    .gadget fused_movi32_\name
        ldr \reg, [_ip]
        gret_nomem 1
.endm
.each_reg xfmovi
.purgem xfmovi
.gadget_list fused_movi32, REG_LIST

# ---- FUSED `imul <dst>, <src>` (32-bit registers): 1 dispatch instead of 3 ----
# [fused_imul32_rr_<dst>_<src>], table [dst * 8 + src]. The imul body works on
# _tmp (see _do_op), so the destination is moved through it inside the gadget,
# exactly as load32_reg/store32_reg did around it. 1.85% of 7-Zip's i386
# instructions (2026-10-04).
.macro xfimul dst_name, dst, src_name, src
    .gadget fused_imul32_rr_\dst_name\()_\src_name
        mov _tmp, \dst
        do_op imul, 32, \src
        mov \dst, _tmp
        gret_nomem
.endm
.macro xfimul_row dst_name, dst
    .macro xfimul_col src_name, src
        xfimul \dst_name, \dst, \src_name, \src
    .endm
    .each_reg xfimul_col
    .purgem xfimul_col
.endm
.each_reg xfimul_row
.purgem xfimul_row
.purgem xfimul
.pushsection_rodata
.global.name fused_imul32_rr_gadgets
.irp dst, REG_LIST
    .irp src, REG_LIST
        .quad NAME(gadget_fused_imul32_rr_\dst\()_\src)
    .endr
.endr
.popsection

# ---- FUSED inc/dec reg (32-bit): [fused_<op>32_<reg>], 1 dispatch instead of 3 ----
# The do_inc/do_dec body on _tmp with the guest register moved in and out.
.macro xfincdec op, name, reg
    .gadget fused_\op\()32_\name
        mov _tmp, \reg
        do_\op 32,
        mov \reg, _tmp
        gret_nomem
.endm
.macro xfinc name, reg
    xfincdec inc, \name, \reg
.endm
.macro xfdec name, reg
    xfincdec dec, \name, \reg
.endm

# atomics. oof

.macro do_op_size_atomic opname, op, size, s
    .gadget atomic_\opname\size\()_mem
        # There's so much stuff going on inside most of these operations that
        # the implementation is a compare-and-swap loop, instead of just ldaxr/stlxr
        # Alignment first, on the GUEST address: write_prep replaces _addr
        # with the host pointer.
        .if \size != 8
            tst _addr, (\size/8)-1
            b.eq 9f
            .ifc \op,add N ua_slow UA_ADD, \size N .endif
            .ifc \op,sub N ua_slow UA_SUB, \size N .endif
            .ifc \op,adc N ua_slow UA_ADC, \size N .endif
            .ifc \op,sbc N ua_slow UA_SBB, \size N .endif
            .ifc \op,and N ua_slow UA_AND, \size N .endif
            .ifc \op,orr N ua_slow UA_OR, \size N .endif
            .ifc \op,eor N ua_slow UA_XOR, \size N .endif
            .ifc \op,inc N ua_slow UA_INC, \size N .endif
            .ifc \op,dec N ua_slow UA_DEC, \size N .endif
            .ifc \op,xadd N ua_slow UA_XADD, \size N .endif
            .ifc \op,not N ua_slow UA_NOT, \size N .endif
            .ifc \op,neg N ua_slow UA_NEG, \size N .endif
        9:
        .endif
        write_prep \size, atomic_\opname\size\()_mem
        dmb ish
        ldr\s w12, [_xaddr]
        .ifin(\op, adc,sbc)
            # CF is an INPUT to adc/sbb -- but do_add below OVERWRITES CPU_cf
            # with the carry the operation produces. This is a compare-exchange
            # loop, so a contended retry re-enters the body at 1b and would
            # read that result CF as its input, silently computing a different
            # instruction. Snapshot the real input once, out here.
            #
            # Only ever wrong under contention, which is why every
            # single-threaded atomics test passed over it: measured as ~841 of
            # 80000 `stc; lock sbbl` iterations taking the wrong carry with
            # four guest threads on one word.
            ldrb w14, [_cpu, CPU_cf]
        .endifin
    1:
        mov w8, w12

        # do the operation
        # dest = w8, src = _tmp
        .ifin(\op, add,sub,adc,sbc)
            setf_a src=_tmp, dst=w8, s=\s
        .endifin
        .ifin(\op, and,orr,eor)
            clearf_a
            clearf_oc
        .endifin
        .ifin(\op, adc,sbc)
            mov w10, w14
            .ifc \op,adc
                cmp w10, 1
            .else
                mvn w10, w10
                cmn w10, 1
            .endif
        .endifin

        .ifin(\op, and,orr,eor)
            \op w8, w8, _tmp
        .endifin
        .ifin(\op, add,sub,adc,sbc)
            do_add \op, w8, _tmp, \s
        .endifin
        .ifc \op,xadd
            # exchange, then add
            mov w9, w8
            mov w8, _tmp
            setf_a src=w9, dst=w8, s=\s
            do_add add, w8, w9, \s
        .endif

        .ifin(\op, add,sub,adc,sbc,and,orr,eor,xadd)
            setf_zsp \s, val=w8
        .endifin

        .ifin(\op, inc,dec)
            mov w10, 1
            setf_a src=w10, dst=w8, s=\s
            .ifb \s
                .ifc \op,inc
                    adds w8, w8, 1
                .else
                    subs w8, w8, 1
                .endif
                cset w9, vs
            .else
                sxt\s w8, w8
                .ifc \op,inc
                    adds w8, w8, 1
                .else
                    subs w8, w8, 1
                .endif
                cmp w8, w8, sxt\s
                cset w9, ne
            .endif
            strb w9, [_cpu, CPU_of]
            setf_zsp \s, val=w8
        .endifin

        # The two group-3 members LOCK is legal on. Both ignore _tmp: like
        # inc/dec they have one operand, the memory word itself.
        #
        # not changes NO flags, so it must not touch the lazy-flag state
        # either -- a flag an earlier instruction left pending has to survive
        # it. mvn on the whole register is fine for 8/16: the sized stlxr
        # below stores only the low bits.
        .ifc \op,not
            mvn w8, w8
        .endif
        # neg is 0 - operand with the whole sub flag rule, exactly what the
        # unlocked NEG gets from the sub gadget: setf_a with the operand as
        # op1 and 0 as op2, then do_add sub for CF/OF, then ZF/SF/PF from the
        # result. w11 holds the operand; nothing on this path uses it.
        .ifc \op,neg
            mov w11, w8
            mov w8, wzr
            setf_a src=w11, dst=w8, s=\s
            do_add sub, w8, w11, \s
            setf_zsp \s, val=w8
        .endif

    2:
        ldaxr\s w13, [_xaddr]
        cmp w12, w13
        b.ne 3f
        stlxr\s w13, w8, [_xaddr]
        cbnz w13, 2b
        .ifc \op,xadd
            mov _tmp, w9
        .endif
        dmb ish
        write_done \size, atomic_\opname\size\()_mem
        gret 1
        write_bullshit \size, atomic_\opname\size\()_mem
    3:
        dmb ish
        mov w12, w13
        b 1b
.endm

.irp op, add,sub,adc,sbb,and,or,xor,inc,dec,xadd,not,neg
    .irp size, SIZE_LIST
        .ifc \op,xor
            ss \size, do_op_size_atomic, \op, eor
        .else N .ifc \op,sbb
            ss \size, do_op_size_atomic, \op, sbc
        .else N .ifc \op,or
            ss \size, do_op_size_atomic, \op, orr
        .else
            ss \size, do_op_size_atomic, \op, \op
        .endif N .endif N .endif
    .endr
    .gadget_array atomic_\op
.endr

# unary operations (well, only one explicit operand)

.macro do_inc size, s
    mov w10, 1
    setf_ops w10, _tmp, \s
    .ifb \s
        adds _tmp, _tmp, 1
        cset w8, vs
    .else
        sxt\s _tmp, _tmp
        add _tmp, _tmp, 1
        cmp _tmp, _tmp, sxt\s
        cset w8, ne
    .endif
    strb w8, [_cpu, CPU_of]
    setf_all_res \s
.endm
.macro do_dec size, s
    mov w10, 1
    setf_ops w10, _tmp, \s
    .ifb \s
        subs _tmp, _tmp, 1
        cset w8, vs
    .else
        sxt\s _tmp, _tmp
        sub _tmp, _tmp, 1
        cmp _tmp, _tmp, sxt\s
        cset w8, ne
    .endif
    strb w8, [_cpu, CPU_of]
    setf_all_res \s
.endm

.each_reg xfinc
.each_reg xfdec
.purgem xfinc
.purgem xfdec
.purgem xfincdec
.gadget_list fused_inc32, REG_LIST
.gadget_list fused_dec32, REG_LIST

.macro do_sign_extend size, s
    .if \size != 32
        # movs\ss\()l %tmp\s, %tmpd
        sxt\s _tmp, _tmp
    .endif
.endm
.macro do_zero_extend size, s
    .if \size != 32
        uxt\s _tmp, _tmp
    .endif
.endm
.macro do_div fault, size, s
    .if \size == 8
        uxth w8, eax
        uxtb _tmp, _tmp
        cbz _tmp, \fault
        udiv w9, w8, _tmp
        cmp w9, w9, uxtb
        b.ne \fault
        msub w10, w9, _tmp, w8
        bfi eax, w9, 0, 8
        bfi eax, w10, 8, 8
    .elseif \size == 16
        bfi w8, eax, 0, 16
        bfi w8, edx, 16, 16
        uxth _tmp, _tmp
        cbz _tmp, \fault
        udiv w9, w8, _tmp
        cmp w9, w9, uxth
        b.ne \fault
        msub w10, w9, _tmp, w8
        bfi eax, w9, 0, 16
        bfi edx, w10, 0, 16
    .elseif \size == 32
        bfi x8, xax, 0, 32
        bfi x8, xdx, 32, 32
        uxtw _xtmp, _tmp
        cbz _xtmp, \fault
        udiv x9, x8, _xtmp
        cmp x9, w9, uxtw
        b.ne \fault
        msub x10, x9, _xtmp, x8
        mov eax, w9
        mov edx, w10
    .endif
.endm
.macro do_idiv fault, size, s
    # another lazy ass copy paste job
    .if \size == 8
        sxth w8, eax
        sxtb _tmp, _tmp
        cbz _tmp, \fault
        sdiv w9, w8, _tmp
        cmp w9, w9, sxtb
        b.ne \fault
        msub w10, w9, _tmp, w8
        bfi eax, w9, 0, 8
        bfi eax, w10, 8, 8
    .elseif \size == 16
        bfi w8, eax, 0, 16
        bfi w8, edx, 16, 16
        sxth _tmp, _tmp
        cbz _tmp, \fault
        sdiv w9, w8, _tmp
        cmp w9, w9, sxth
        b.ne \fault
        msub w10, w9, _tmp, w8
        bfi eax, w9, 0, 16
        bfi edx, w10, 0, 16
    .elseif \size == 32
        bfi x8, xax, 0, 32
        bfi x8, xdx, 32, 32
        sxtw _xtmp, _tmp
        cbz _xtmp, \fault
        sdiv x9, x8, _xtmp
        cmp x9, w9, sxtw
        b.ne \fault
        msub x10, x9, _xtmp, x8
        mov eax, w9
        mov edx, w10
    .endif
.endm
.macro do_mul size, s
    .ifb \s
        umull xax, eax, _tmp
        lsr xdx, xax, 32
        cmp xax, eax, uxtw
    .else
        uxt\s w8, eax
        uxt\s _tmp, _tmp
        mul w8, w8, _tmp
        cmp w8, w8, uxt\s
        .if \size == 8
            bfxil eax, w8, 0, \size*2
        .else
            bfxil eax, w8, 0, \size
            bfxil edx, w8, \size, \size
        .endif
    .endif
    cset w8, ne
    strb w8, [_cpu, CPU_cf]
    strb w8, [_cpu, CPU_of]
.endm
.macro do_imul1 size, s
    .ifb \s
        smull xax, eax, _tmp
        lsr xdx, xax, 32
        cmp xax, eax, sxtw
    .else
        sxt\s w8, eax
        sxt\s _tmp, _tmp
        mul w8, w8, _tmp
        cmp w8, w8, sxt\s
        .if \size == 8
            bfxil eax, w8, 0, \size*2
        .else
            bfxil eax, w8, 0, \size
            bfxil edx, w8, \size, \size
        .endif
    .endif
    cset w8, ne
    strb w8, [_cpu, CPU_cf]
    strb w8, [_cpu, CPU_of]
.endm
.macro do_not size, s
    .ifb \s
        mvn _tmp, _tmp
    .else
        movs w10, _tmp, \s
        mvn w10, w10
        movs _tmp, w10, \s
    .endif
.endm

.irp op, inc,dec,sign_extend,zero_extend,mul,imul1,not
    .irp size, SIZE_LIST
        .gadget \op\()_\size
            ss \size, do_\op
            gret_nomem
    .endr
    .gadget_list \op, SIZE_LIST
.endr

# div/idiv carry the faulting instruction IP as operand 0 so a #DE (zero divisor
# or quotient overflow) can set eip and raise INT_DIV. ARM udiv/sdiv don't trap;
# do_div/do_idiv branch to the per-gadget fault label on those conditions.
.irp op, div, idiv
    .irp size, SIZE_LIST
        .gadget \op\()_\size
            ss \size, do_\op, .Lde_\op\()_\size
            gret 1
        .Lde_\op\()_\size:
            ldr eip, [_ip]
            mov x0, INT_DIV
            b jit_exit
    .endr
    .gadget_list \op, SIZE_LIST
.endr

.gadget cvt_16
    tst eax, 0x8000
    cinv w8, wzr, ne
    bfxil edx, w8, 0, 16
    gret
.gadget cvt_32
    tst eax, 0x80000000
    cinv edx, wzr, ne
    gret
.gadget_list cvt, SIZE_LIST

.gadget cvte_16
    sxtb w8, eax
    bfxil eax, w8, 0, 16
    gret
.gadget cvte_32
    sxth eax, eax
    gret
.gadget_list cvte, SIZE_LIST

# ---------------------------------------------------------------------------
# amd64 native SSE register-register gadgets.
#
# These replace the per-op C bridge (amd64_jit_0f_vec_rm) for the hottest
# register-register vector ops. They touch only cpu->xmm[] (and the operand
# word), never the GPR reg-cache (x20-x27) or _ip/_cpu/_tlb, so gen.c does NOT
# flush the reg cache around them. No memory operand => no TLB/prep => x8-x15
# are all free scratch (same as the amd64_*_reg_reg GPR gadgets above).
#
# Operand word layout (one gadget slot): src xmm id in bits 0-3, dst xmm id in
# bits 4-7. cpu->xmm[i] lives at CPU_xmm + i*16.
# ---------------------------------------------------------------------------

# movdqa/movdqu xmm1, xmm2 (66/F3 0F 6F, mod==3): cpu->xmm[dst] = cpu->xmm[src]
.gadget amd64_v_mov128_reg
    ldr x8, [_ip]
    and x9, x8, 0xf
    ubfx x10, x8, 4, 4
    add x11, _cpu, CPU_xmm
    add x9, x11, x9, lsl 4
    add x10, x11, x10, lsl 4
    ldp x12, x13, [x9]
    stp x12, x13, [x10]
    gret 1

# Register-register binary op over the two 64-bit lanes of an xmm:
# cpu->xmm[dst] = cpu->xmm[dst] OP cpu->xmm[src]. 64-bit-lane integer ops
# (paddq/psubq) and the 128-bit bitwise ops (pxor/pand/por/pandn) all reduce to
# two independent 64-bit ops on the lane pair. \rev swaps the operands so pandn
# (result = ~dst & src) maps to bic Rd, src, dst.
.macro amd64_v_lane64_binop name, insn, rev=0
    .gadget amd64_v_\name\()_reg
        ldr x8, [_ip]
        and x9, x8, 0xf
        ubfx x10, x8, 4, 4
        add x11, _cpu, CPU_xmm
        add x9, x11, x9, lsl 4
        add x10, x11, x10, lsl 4
        ldp x12, x13, [x10]
        ldp x14, x15, [x9]
        .if \rev
            \insn x12, x14, x12
            \insn x13, x15, x13
        .else
            \insn x12, x12, x14
            \insn x13, x13, x15
        .endif
        stp x12, x13, [x10]
        gret 1
.endm

# 66 0F D4 paddq
amd64_v_lane64_binop paddq, add
# 66 0F FB psubq
amd64_v_lane64_binop psubq, sub
# 0F 57 xorps / 66 0F 57 xorpd: 128-bit bitwise XOR (two 64-bit lanes)
amd64_v_lane64_binop pxor, eor

# Scalar-double SSE2 (F2 0F 58 addsd / 59 mulsd), reg-reg: operate on the low
# 64-bit lane only, dst.f64[0] = dst.f64[0] OP src.f64[0]; dst's high 64 bits are
# preserved (the 8-byte str writes only the low lane). aarch64 C `double` math
# lowers to these same fadd/fmul, so this is bit-identical to the interpreter.
# (First NEON use in a gadget: d0/d1 are caller-saved, loaded fresh from cpu->xmm
# each time and dead after gret, and no C call is made, so they are free scratch.)
.macro amd64_v_scalar_double name, fpop
    .gadget amd64_v_\name\()_reg
        ldr x8, [_ip]
        and x9, x8, 0xf
        ubfx x10, x8, 4, 4
        add x11, _cpu, CPU_xmm
        add x9, x11, x9, lsl 4
        add x10, x11, x10, lsl 4
        ldr d0, [x10]
        ldr d1, [x9]
        \fpop d0, d0, d1
        str d0, [x10]
        gret 1
.endm

# F2 0F 58 addsd
amd64_v_scalar_double addsd, fadd
# F2 0F 59 mulsd
amd64_v_scalar_double mulsd, fmul

# Packed byte op over the full 16 bytes of an xmm: cpu->xmm[dst] = xmm[dst] OP
# xmm[src], via a single NEON .16b instruction. Used for the glibc strlen/memchr
# SSE2 byte-scan that dominates crypt/login (pcmpeqb + psubb). Touches only
# cpu->xmm[] (no reg cache flush). Operand word: src id bits 0-3, dst id bits 4-7.
.macro amd64_v_neon16_binop name, op
    .gadget amd64_v_\name\()_reg
        ldr x8, [_ip]
        and x9, x8, 0xf
        ubfx x10, x8, 4, 4
        add x11, _cpu, CPU_xmm
        add x9, x11, x9, lsl 4
        add x10, x11, x10, lsl 4
        ldr q0, [x10]
        ldr q1, [x9]
        \op v0.16b, v0.16b, v1.16b
        str q0, [x10]
        gret 1
.endm

# 66 0F F8 psubb: packed subtract of 16 bytes
amd64_v_neon16_binop psubb, sub
# 66 0F 74 pcmpeqb: per-byte compare-equal -> 0xFF/0x00
amd64_v_neon16_binop pcmpeqb, cmeq

# Same reg-reg shape as amd64_v_neon16_binop but with the NEON arrangement as a
# parameter, for lane widths other than bytes. dst = dst OP src; operand word:
# src id bits 0-3, dst id bits 4-7. These are the remaining hot SSE2 integer
# ops from the JIT bridge histogram (glibc's SSE2 str*/mem* and zlib's
# adler32/crc paths): interleaves, packed word arith, and 128-bit OR.
.macro amd64_v_neon_binop name, op, arr
    .gadget amd64_v_\name\()_reg
        ldr x8, [_ip]
        and x9, x8, 0xf
        ubfx x10, x8, 4, 4
        add x11, _cpu, CPU_xmm
        add x9, x11, x9, lsl 4
        add x10, x11, x10, lsl 4
        ldr q0, [x10]
        ldr q1, [x9]
        \op v0.\arr, v0.\arr, v1.\arr
        str q0, [x10]
        gret 1
.endm

# 66 0F FD paddw: packed 16-bit add
amd64_v_neon_binop paddw, add, 8h
# 66 0F D9 psubusw: packed 16-bit subtract with unsigned saturation
amd64_v_neon_binop psubusw, uqsub, 8h
# 66 0F EB por: 128-bit bitwise OR
amd64_v_neon_binop por, orr, 16b
# 66 0F 60/61/62/6C punpcklbw/wd/dq/qdq: interleave low elements, dst first.
# x86 PUNPCKL* takes dst[0], src[0], dst[1], src[1], ... from the LOW halves --
# exactly NEON zip1 with Vn=dst, Vm=src at the matching lane width.
amd64_v_neon_binop punpcklbw, zip1, 16b
amd64_v_neon_binop punpcklwd, zip1, 8h
amd64_v_neon_binop punpckldq, zip1, 4s
amd64_v_neon_binop punpcklqdq, zip1, 2d

# movq xmm, xmm (F3 0F 7E mod==3, or 66 0F D6 mod==3 with src/dst swapped by
# gen.c's operand packing): copy the low 64 bits, zero the high 64. Operand:
# src id bits 0-3, dst id bits 4-7.
# movd/movq r/m, xmm (66 0F 7E), mod==3: store the low 32 (movd) or 64 (movq,
# REX.W bit 8) bits of xmm[src] into a GPR. The 32-bit form zero-extends into
# the full 64-bit register (all 32-bit GPR writes do). Writes a GPR, so gen.c
# flushes the reg cache first (flush also invalidates -- same contract as
# gadget_amd64_arith_reg_reg). Operand: GPR dst id bits 0-3, xmm src id bits
# 4-7, REX.W bit 8.
# Generic 16-byte shuffle: dst = tbl(src, mask), mask baked into the code
# stream by gen.c at translation time (the shuffle imm8 is a compile-time
# constant). tbl yields 0 for any index byte >= 16, which encodes the
# zero-fill of pslldq/psrldq and makes one gadget serve pshufd, pshuflw,
# pshufhw (identity indices for the untouched half), and both byte shifts.
# Operands: [src | dst<<4][mask bytes 0-7][mask bytes 8-15].
.gadget amd64_v_tbl_reg
    ldr x8, [_ip]
    and x9, x8, 0xf
    ubfx x10, x8, 4, 4
    add x11, _cpu, CPU_xmm
    add x9, x11, x9, lsl 4
    add x10, x11, x10, lsl 4
    ldr q0, [x9]
    ldr q1, [_ip, 8]
    tbl v0.16b, {v0.16b}, v1.16b
    str q0, [x10]
    gret 3

# movd/movq xmm, r/m (66 0F 6E), reg-reg: load a 32-bit (movd) or 64-bit (movq,
# REX.W bit 8) GPR into the low lane of xmm[dst] and ZERO the rest of the 128 bits.
# Reads a GPR, so gen.c flushes the reg cache first. Operand: GPR src id bits 0-3,
# xmm dst id bits 4-7, REX.W bit 8.
# pmovmskb r32, xmm (66 0F D7), reg-reg: pack the most-significant bit of each of
# the 16 bytes of xmm[src] into a 16-bit mask in GPR[dst] (zero-extended). The
# core of glibc's SSE2 strlen/memchr (after pcmpeqb). Standard NEON coalesce:
# isolate each byte's MSB, then shift-right-accumulate to gather bytes 0-7 into
# byte 0 and bytes 8-15 into byte 8. Writes a GPR, so gen.c flushes the reg cache.
# Operand: xmm src id bits 0-3, GPR dst id bits 4-7.
.gadget amd64_v_pmovmskb_reg
    ldr x8, [_ip]
    and x9, x8, 0xf
    ubfx x10, x8, 4, 4
    add x11, _cpu, CPU_xmm
    add x9, x11, x9, lsl 4
    ldr q0, [x9]
    ushr v0.16b, v0.16b, 7
    usra v0.8h, v0.8h, 7
    usra v0.4s, v0.4s, 14
    usra v0.2d, v0.2d, 28
    umov w14, v0.b[0]
    umov w15, v0.b[8]
    orr w14, w14, w15, lsl 8
    add x13, _cpu, CPU_amd64_regs
    str x14, [x13, x10, lsl 3]
    gret 1

# ---------------------------------------------------------------------------
# 66 0F 3A 0F palignr xmm, xmm, imm8   (mod==3)
# 66 0F 3A 63 pcmpistri xmm, xmm, imm8 (mod==3)
#
# These two are the ENTIRE three-byte-opcode footprint of glibc's string
# functions -- 309 palignr and 202 pcmpistri in libc.so.6 -- and together they
# were 98.7% of every amd64 JIT block fallback measured on a gcc workload
# (16384 fallbacks, 6 distinct opcodes, 0f 3a = 16177 of them).
#
# A fallback is not one bridged instruction. jit.c frees the whole compiled
# block and runs it interpreted, so a single pcmpistri inside strlen de-JITs
# every instruction around it -- which is why glibc string code has been the
# slowest thing an amd64 guest does.
#
# Both call into C rather than open-coding the semantics: palignr is a 32-byte
# funnel shift and pcmpistri is the full SSE4.2 aggregation-with-imm-control,
# and emu/vec.c already has both, bit-exact against real Intel via
# tests/remote/corpus/sse4.c. The win here is keeping the BLOCK compiled, not
# shaving the instruction.
#
# Operand word: xmm src (rm) id in bits 0-3, xmm dst (reg) id in bits 4-7,
# imm8 in bits 8-15.
# ---------------------------------------------------------------------------
# Register form: xmm src (rm) id in bits 0-3, xmm dst (reg) id in bits 4-7,
# imm8 in bits 8-15.
.macro amd64_v_str3a_reg name, helper
    .gadget amd64_v_\name\()_xmm
        ldr  x8, [_ip]
        and  x9, x8, 0xf
        ubfx x10, x8, 4, 4
        ubfx x11, x8, 8, 8
        add  x12, _cpu, CPU_xmm
        save_c
        mov  x0, _cpu
        add  x1, x12, x9, lsl 4
        add  x2, x12, x10, lsl 4
        mov  w3, w11
        bl   NAME(\helper)
        restore_c
        gret 1
.endm

amd64_v_str3a_reg palignr,   vec_palignr128
amd64_v_str3a_reg pcmpistri, amd64_jit_pcmpistri
amd64_v_str3a_reg pcmpestri, amd64_jit_pcmpestri

# ---------------------------------------------------------------------------
# Native amd64 SSE memory load/store.
#
# Unlike the i386 SSE path, amd64 guest addresses are 47-bit, so the 32-bit
# _addr(w3)-based read_prep/handlers cannot be used. These gadgets compute the
# full 64-bit effective address (reusing the meta/disp encoding produced by
# gen_amd64_decode_mem_meta, same bit layout the LEA gadget uses) and resolve it
# through a 64-bit TLB fast path, falling to the C slow helpers (which already
# take a 64-bit guest_addr_t) on miss/cross-page, and to a controlled INT_PF
# exit on a real fault.
#
# Contract from gen.c: the reg cache is FLUSHED first (so cpu->amd64_regs is
# current for the address read AND correct for fault re-execution, since save_regs
# only mirrors the cache to the i386 slots), and the rip is FLUSHED first (so
# cpu->amd64_rip already points at this instruction for a fault). Operands:
#   [_ip,0]=meta  [_ip,8]=disp  [_ip,16]=next_ip (rip-relative addend).  gret 3.
# Register use: x9=address (-> host ptr), x10=xmm id (survives via save_c),
# x8/x11/x12/x13/x14/x15/x0 scratch; never touches x20-x27 (the flushed cache).
# ---------------------------------------------------------------------------

# 64-bit TLB read fast path: x9 = guest addr in -> host ptr out (or miss/xpage).
.macro amd64_vread_prep size, id
    and  x8, x9, 0xfff
    cmp  x8, (0x1000-\size)
    b.hi amd64_vxpage_\id
    and  x8, x9, #0xfffffffffffff000
    ubfx x11, x9, 12, TLB_BITS
    eor  x11, x11, x9, lsr #(12 + TLB_BITS)
    and  x11, x11, #(1 << TLB_BITS) - 1
    mov  w12, TLB_ENTRY_SIZE
    madd x11, x11, x12, _tlb
    ldr  x13, [x11, TLB_ENTRY_page]
    cmp  x8, x13
    b.ne amd64_vmiss_\id
    ldr  x13, [x11, TLB_ENTRY_data_minus_addr]
    add  x9, x13, x9
amd64_vback_\id:
.endm

.macro amd64_vread_bullshit size, id
amd64_vmiss_\id:
    bl amd64_vmem_read_miss
    b amd64_vback_\id
amd64_vxpage_\id:
    mov x19, \size
    bl amd64_vmem_crosspage_read
    b amd64_vback_\id
.endm

.global amd64_vmem_read_miss
amd64_vmem_read_miss:
    save_c
    sub x0, _tlb, TLB_entries
    mov x1, x9
    mov x2, xzr
    bl NAME(tlb_handle_miss)
    mov x19, x0
    restore_c
    cbz x19, amd64_vmem_segfault_read
    mov x9, x19
    ret

.global amd64_vmem_crosspage_read
amd64_vmem_crosspage_read:
    save_c
    sub x0, _tlb, TLB_entries
    mov x2, LOCAL_value
    add x2, _cpu, x2
    mov x1, x9
    mov x3, x19
    bl NAME(__tlb_read_cross_page)
    mov x19, x0
    restore_c
    cbz x19, amd64_vmem_segfault_read
    mov x9, LOCAL_value
    add x9, _cpu, x9
    ret

.global amd64_vmem_segfault_read
amd64_vmem_segfault_read:
    ldr x8, [_tlb, (-TLB_entries+TLB_segfault_addr)]
    str x8, [_cpu, CPU_segfault_addr]
    strb wzr, [_cpu, CPU_segfault_was_write]
    mov x0, INT_PF
    b jit_exit

# Compute the 64-bit effective address from meta(x8)/disp(x9) into x9. Reads base
# and index from cpu->amd64_regs in memory (the reg cache was flushed by gen.c).
.macro amd64_vmem_addr
    tbz  x8, 30, 7001f
    ubfx x0, x8, 20, 4
    add  x14, _cpu, CPU_amd64_regs
    ldr  x14, [x14, x0, lsl 3]
    add  x9, x9, x14
7001:
    tbz  x8, 31, 7002f
    ubfx x0, x8, 24, 4
    ubfx x15, x8, 28, 2
    add  x14, _cpu, CPU_amd64_regs
    ldr  x14, [x14, x0, lsl 3]
    lsl  x14, x14, x15
    add  x9, x9, x14
7002:
    tbz  x8, 32, 7003f
    ldr  x14, [_ip, 16]
    add  x9, x9, x14
7003:
    /* AMD64_JIT_MEM_FS (bit 33): an %fs-relative address, which in long mode is
       just a flat base added to the effective address. Without this every
       FS-prefixed memory instruction had to bridge -- and that is nearly all of
       them in threaded code, because the stack-protector canary at %fs:0x28 and
       every __thread variable go through it. Measured on a 4-thread pthread
       workload it was 22% of ALL interpreter bridges: mov 8b 12.3%, sub 2b
       5.3%, mov 89 3.0%, xor 33 1.3%. */
    tbz  x8, 33, 7004f
    ldr  x14, [_cpu, CPU_tls_ptr]
    add  x9, x9, x14
7004:
    /* AMD64_JIT_MEM_GS (bit 36): the same for a %gs-relative address, whose
       base ARCH_SET_GS sets. */
    tbz  x8, 36, 7005f
    ldr  x14, [_cpu, CPU_amd64_gs_base]
    add  x9, x9, x14
7005:
    /* AMD64_JIT_MEM_ALIGN16 (bit 37): a legacy-SSE m128 that must be 16-byte
       aligned; misaligned is #GP(0) at the instruction (rip was flushed to it
       before the gadget). */
    tbz  x8, 37, 7006f
    tst  x9, 15
    b.ne amd64_vmem_misaligned
7006:
.endm

amd64_vmem_misaligned:
    str  xzr, [_cpu, CPU_segfault_addr]
    strb wzr, [_cpu, CPU_segfault_was_write]
    strb wzr, [_cpu, CPU_segfault_reported]
    mov  x0, INT_GPF
    b    jit_exit

# movaps/movups xmm, m128 (0F 28 / 0F 10): cpu->xmm[reg] = [addr]
# 64-bit TLB write fast path: x9 = guest addr in -> writable host ptr out (or the
# cross-page staging buffer, flushed by amd64_vwrite_done). Mirrors the 32-bit
# write_prep: a TLB-staleness / host-page-mirroring check routes to the slow C
# resolver; the hit path checks page_if_writable and records the dirty page.
.macro amd64_vwrite_prep size, id
    and  x8, x9, 0xfff
    cmp  x8, (0x1000-\size)
    b.hi amd64_vwxpage_\id
    ldr  x11, [_tlb, (-TLB_entries+TLB_mmu)]
    ldr  x8,  [_tlb, (-TLB_entries+TLB_mem_changes)]
    ldr  x12, [x11, MMU_changes]
    cmp  x8, x12
    b.ne amd64_vwslow_\id
    ldrb w12, [x11, MMU_requires_write_revalidate]
    cbz  w12, amd64_vwfast_\id
amd64_vwslow_\id:
    bl   amd64_vmem_write_resolve
    b    amd64_vwback_\id
amd64_vwfast_\id:
    and  x8, x9, #0xfffffffffffff000
    str  x8, [_tlb, (-TLB_entries+TLB_dirty_page)]
    ubfx x11, x9, 12, TLB_BITS
    eor  x11, x11, x9, lsr #(12 + TLB_BITS)
    and  x11, x11, #(1 << TLB_BITS) - 1
    mov  w12, TLB_ENTRY_SIZE
    madd x11, x11, x12, _tlb
    ldr  x13, [x11, TLB_ENTRY_page_if_writable]
    cmp  x8, x13
    b.ne amd64_vwmiss_\id
    ldr  x13, [x11, TLB_ENTRY_data_minus_addr]
    add  x9, x13, x9
amd64_vwback_\id:
.endm

# After the store, if x9 was the cross-page staging buffer, flush it to memory.
.macro amd64_vwrite_done size, id
    mov x8, LOCAL_value
    add x8, _cpu, x8
    cmp  x8, x9
    b.ne amd64_vwdone_\id
    mov  x19, \size
    bl   amd64_vmem_crosspage_write
amd64_vwdone_\id:
.endm

.macro amd64_vwrite_bullshit size, id
amd64_vwmiss_\id:
    bl   amd64_vmem_write_miss
    b    amd64_vwback_\id
amd64_vwxpage_\id:
    str  x9, [_cpu, LOCAL_value_addr]
    mov x9, LOCAL_value
    add x9, _cpu, x9
    b    amd64_vwback_\id
.endm

.global amd64_vmem_write_resolve
amd64_vmem_write_resolve:
    save_c
    sub  x0, _tlb, TLB_entries
    mov  x1, x9
    bl   NAME(tlb_write_ptr_slow)
    mov  x19, x0
    restore_c
    cbz  x19, amd64_vmem_segfault_write
    mov  x9, x19
    ret

.global amd64_vmem_write_miss
amd64_vmem_write_miss:
    save_c
    sub  x0, _tlb, TLB_entries
    mov  x1, x9
    mov  x2, 1
    bl   NAME(tlb_handle_miss)
    mov  x19, x0
    restore_c
    cbz  x19, amd64_vmem_segfault_write
    mov  x9, x19
    ret

.global amd64_vmem_crosspage_write
amd64_vmem_crosspage_write:
    save_c
    sub  x0, _tlb, TLB_entries
    mov x2, LOCAL_value
    add x2, _cpu, x2
    ldr  x1, [_cpu, LOCAL_value_addr]
    mov  x3, x19
    bl   NAME(__tlb_write_cross_page)
    mov  x19, x0
    restore_c
    cbz  x19, amd64_vmem_segfault_write
    ret

.global amd64_vmem_segfault_write
amd64_vmem_segfault_write:
    ldr  x8, [_tlb, (-TLB_entries+TLB_segfault_addr)]
    str  x8, [_cpu, CPU_segfault_addr]
    mov  w8, 1
    strb w8, [_cpu, CPU_segfault_was_write]
    mov  x0, INT_PF
    b    jit_exit

# movaps/movups m128, xmm (0F 29 / 0F 11): [addr] = cpu->xmm[reg]
.gadget amd64_v_store128_mem
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    ubfx x10, x8, 8, 4
    amd64_vmem_addr
    amd64_vwrite_prep 16, store128
    add  x14, _cpu, CPU_xmm
    add  x14, x14, x10, lsl 4
    ldp  x12, x13, [x14]
    stp  x12, x13, [x9]
    amd64_vwrite_done 16, store128
    gret 3
    amd64_vwrite_bullshit 16, store128

# ===========================================================================
# The SSE2 and MMX packed-integer ops, every one, register and memory
# sources (math.S amd64_vi_*). Four gadgets per op:
#   _xr  XMM reg:  [packed = src (0-3) | dst (4-7)], gret 1
#   _xm  XMM mem:  [meta][disp][next_ip], dst in meta 8-11, gret 3
#   _mr  MMX reg:  [packed = src (0-2) | dst (4-6)], gret 1
#   _mm  MMX mem:  [meta][disp][next_ip], dst in meta 8-10, gret 3
# The body works on v0 (dst) and v1 (src) into v0, temps v2-v5; `kind`
# picks the shape (bin: op dst, dst, src at the arrangement; binr: op dst,
# src, dst; dq: 64-bit lanes, scalar at MMX width; the rest are named).
# MMX registers are cpu->mm[], apart from the x87 stack, as everywhere in
# AOK; nothing here touches the x87 tag word.
# ===========================================================================

.macro vi_body kind, op, arr, q
.ifc \kind,bin
    \op  v0.\arr, v0.\arr, v1.\arr
.endif
.ifc \kind,binr
    \op  v0.\arr, v1.\arr, v0.\arr
.endif
.ifc \kind,dq
  .if \q
    \op  v0.2d, v0.2d, v1.2d
  .else
    \op  d0, d0, d1
  .endif
.endif
.ifc \kind,mulh                      /* PMULHW (smull) / PMULHUW (umull) */
    \op  v2.4s, v0.4h, v1.4h
  .if \q
    \op\()2 v3.4s, v0.8h, v1.8h
  .endif
    shrn v0.4h, v2.4s, 16
  .if \q
    shrn2 v0.8h, v3.4s, 16
  .endif
.endif
.ifc \kind,madd                      /* PMADDWD */
    smull v2.4s, v0.4h, v1.4h
  .if \q
    smull2 v3.4s, v0.8h, v1.8h
    addp v0.4s, v2.4s, v3.4s
  .else
    addp v0.4s, v2.4s, v2.4s
  .endif
.endif
.ifc \kind,muludq                    /* PMULUDQ: the even dwords */
  .if \q
    xtn  v2.2s, v0.2d
    xtn  v3.2s, v1.2d
    umull v0.2d, v2.2s, v3.2s
  .else
    umull v0.2d, v0.2s, v1.2s
  .endif
.endif
.ifc \kind,sad                       /* PSADBW: per quadword */
  .if \q
    uabd v2.16b, v0.16b, v1.16b
    uaddlp v2.8h, v2.16b
    uaddlp v2.4s, v2.8h
    uaddlp v0.2d, v2.4s
  .else
    uabd v2.8b, v0.8b, v1.8b
    uaddlp v2.4h, v2.8b
    uaddlp v2.2s, v2.4h
    uaddlp v0.1d, v2.2s
  .endif
.endif
.ifc \kind,packbw                    /* PACKSSWB (sqxtn) / PACKUSWB (sqxtun) */
  .if \q
    \op  v0.8b, v0.8h
    \op\()2 v0.16b, v1.8h
  .else
    mov  v0.d[1], v1.d[0]
    \op  v0.8b, v0.8h
  .endif
.endif
.ifc \kind,packwd                    /* PACKSSDW */
  .if \q
    \op  v0.4h, v0.4s
    \op\()2 v0.8h, v1.4s
  .else
    mov  v0.d[1], v1.d[0]
    \op  v0.4h, v0.4s
  .endif
.endif
.endm

.macro amd64_vi name, kind, op, a16, a8
.gadget amd64_vi_\name\()_xr
    ldr  x8, [_ip]
    and  x9, x8, 0xf
    ubfx x10, x8, 4, 4
    add  x11, _cpu, CPU_xmm
    add  x9, x11, x9, lsl 4
    add  x10, x11, x10, lsl 4
    ldr  q0, [x10]
    ldr  q1, [x9]
    vi_body \kind, \op, \a16, 1
    str  q0, [x10]
    gret 1
.gadget amd64_vi_\name\()_xm
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    ubfx x10, x8, 8, 4
    amd64_vmem_addr
    amd64_vread_prep 16, vi_\name\()_xm
    ldr  q1, [x9]
    add  x14, _cpu, CPU_xmm
    add  x14, x14, x10, lsl 4
    ldr  q0, [x14]
    vi_body \kind, \op, \a16, 1
    str  q0, [x14]
    gret 3
    amd64_vread_bullshit 16, vi_\name\()_xm
.ifnc \a8,-
.gadget amd64_vi_\name\()_mr
    ldr  x8, [_ip]
    and  x9, x8, 7
    ubfx x10, x8, 4, 3
    add  x11, _cpu, CPU_mm
    add  x9, x11, x9, lsl 3
    add  x10, x11, x10, lsl 3
    ldr  d0, [x10]
    ldr  d1, [x9]
    vi_body \kind, \op, \a8, 0
    str  d0, [x10]
    gret 1
.gadget amd64_vi_\name\()_mm
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    ubfx x10, x8, 8, 3
    amd64_vmem_addr
    amd64_vread_prep 8, vi_\name\()_mm
    ldr  d1, [x9]
    add  x14, _cpu, CPU_mm
    add  x14, x14, x10, lsl 3
    ldr  d0, [x14]
    vi_body \kind, \op, \a8, 0
    str  d0, [x14]
    gret 3
    amd64_vread_bullshit 8, vi_\name\()_mm
.endif
.endm

amd64_vi paddb, bin, add, 16b, 8b
amd64_vi paddw, bin, add, 8h, 4h
amd64_vi paddd, bin, add, 4s, 2s
amd64_vi paddq, dq, add, 2d, 1d
amd64_vi psubb, bin, sub, 16b, 8b
amd64_vi psubw, bin, sub, 8h, 4h
amd64_vi psubd, bin, sub, 4s, 2s
amd64_vi psubq, dq, sub, 2d, 1d
amd64_vi paddsb, bin, sqadd, 16b, 8b
amd64_vi paddsw, bin, sqadd, 8h, 4h
amd64_vi paddusb, bin, uqadd, 16b, 8b
amd64_vi paddusw, bin, uqadd, 8h, 4h
amd64_vi psubsb, bin, sqsub, 16b, 8b
amd64_vi psubsw, bin, sqsub, 8h, 4h
amd64_vi psubusb, bin, uqsub, 16b, 8b
amd64_vi psubusw, bin, uqsub, 8h, 4h
amd64_vi pcmpeqb, bin, cmeq, 16b, 8b
amd64_vi pcmpeqw, bin, cmeq, 8h, 4h
amd64_vi pcmpeqd, bin, cmeq, 4s, 2s
amd64_vi pcmpgtb, bin, cmgt, 16b, 8b
amd64_vi pcmpgtw, bin, cmgt, 8h, 4h
amd64_vi pcmpgtd, bin, cmgt, 4s, 2s
amd64_vi pand, bin, and, 16b, 8b
amd64_vi pandn, binr, bic, 16b, 8b
amd64_vi por, bin, orr, 16b, 8b
amd64_vi pxor, bin, eor, 16b, 8b
amd64_vi pminub, bin, umin, 16b, 8b
amd64_vi pmaxub, bin, umax, 16b, 8b
amd64_vi pminsw, bin, smin, 8h, 4h
amd64_vi pmaxsw, bin, smax, 8h, 4h
amd64_vi pavgb, bin, urhadd, 16b, 8b
amd64_vi pavgw, bin, urhadd, 8h, 4h
amd64_vi pmullw, bin, mul, 8h, 4h
amd64_vi pmulhw, mulh, smull, -, x
amd64_vi pmulhuw, mulh, umull, -, x
amd64_vi pmuludq, muludq, -, -, x
amd64_vi pmaddwd, madd, -, -, x
amd64_vi psadbw, sad, -, -, x
amd64_vi punpcklbw, bin, zip1, 16b, 8b
amd64_vi punpcklwd, bin, zip1, 8h, 4h
amd64_vi punpckldq, bin, zip1, 4s, 2s
amd64_vi punpckhbw, bin, zip2, 16b, 8b
amd64_vi punpckhwd, bin, zip2, 8h, 4h
amd64_vi punpckhdq, bin, zip2, 4s, 2s
amd64_vi punpcklqdq, bin, zip1, 2d, -
amd64_vi punpckhqdq, bin, zip2, 2d, -
amd64_vi packsswb, packbw, sqxtn, -, x
amd64_vi packuswb, packbw, sqxtun, -, x
amd64_vi packssdw, packwd, sqxtn, -, x

# The packed shifts, XMM and MMX: PSRLW/D/Q, PSRAW/D, PSLLW/D/Q by an imm8
# (0F 71-73 /2 /4 /6) or by the low quadword of a register or memory
# operand (0F D1-D3, E1-E2, F1-F3), and PSRLDQ/PSLLDQ (66 0F 73 /3 /7). A
# count past the lane width clears the lane -- or, for PSRA, fills it with
# the sign -- whatever its upper bits (the whole 64-bit count counts).
# Gadgets per op: _xi [dst (0-3) | imm (8-15)], _xr [src (0-3) | dst (4-7)],
# _xm [meta][disp][next_ip] (an m128 count), and the MMX _mi/_mr/_mm.
# gret 1 / 1 / 3.

# v0 shifted by x10 (the count) per lane: kind srl/sra/sll, lane bits L,
# arrangement arr (8h 4s 2d / 4h 2s 1d). Clobbers x10, v2.
.macro vsh_body kind, L, arr
    cmp  x10, (\L - 1)
.ifc \kind,sra
    b.ls 1f
    mov  x10, (\L - 1)               /* past the width: all sign */
1:  neg  x10, x10
.else
    b.ls 1f
    movi v0.2d, 0                    /* past the width: all zero */
    b    2f
1:
  .ifc \kind,srl
    neg  x10, x10
  .endif
.endif
.ifc \arr,1d
    fmov d2, x10
  .ifc \kind,sra
    sshl d0, d0, d2
  .else
    ushl d0, d0, d2
  .endif
.else
  .if \L == 64
    dup  v2.\arr, x10
  .else
    dup  v2.\arr, w10
  .endif
  .ifc \kind,sra
    sshl v0.\arr, v0.\arr, v2.\arr
  .else
    ushl v0.\arr, v0.\arr, v2.\arr
  .endif
.endif
2:
.endm

.macro amd64_vsh name, kind, L, a16, a8
.gadget amd64_vsh_\name\()_xi
    ldr  x8, [_ip]
    and  x9, x8, 0xf
    ubfx x10, x8, 8, 8
    add  x11, _cpu, CPU_xmm
    add  x9, x11, x9, lsl 4
    ldr  q0, [x9]
    vsh_body \kind, \L, \a16
    str  q0, [x9]
    gret 1
.gadget amd64_vsh_\name\()_xr
    ldr  x8, [_ip]
    and  x9, x8, 0xf
    ubfx x11, x8, 4, 4
    add  x12, _cpu, CPU_xmm
    add  x13, x12, x9, lsl 4
    ldr  x10, [x13]                  /* the count: src's low quadword */
    add  x9, x12, x11, lsl 4
    ldr  q0, [x9]
    vsh_body \kind, \L, \a16
    str  q0, [x9]
    gret 1
.gadget amd64_vsh_\name\()_xm
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    ubfx x10, x8, 8, 4
    amd64_vmem_addr
    amd64_vread_prep 16, vsh_\name\()_xm
    add  x14, _cpu, CPU_xmm
    add  x14, x14, x10, lsl 4
    ldr  x10, [x9]                   /* the count: the low quadword of the m128 */
    ldr  q0, [x14]
    vsh_body \kind, \L, \a16
    str  q0, [x14]
    gret 3
    amd64_vread_bullshit 16, vsh_\name\()_xm
.gadget amd64_vsh_\name\()_mi
    ldr  x8, [_ip]
    and  x9, x8, 7
    ubfx x10, x8, 8, 8
    add  x11, _cpu, CPU_mm
    add  x9, x11, x9, lsl 3
    ldr  d0, [x9]
    vsh_body \kind, \L, \a8
    str  d0, [x9]
    gret 1
.gadget amd64_vsh_\name\()_mr
    ldr  x8, [_ip]
    and  x9, x8, 7
    ubfx x11, x8, 4, 3
    add  x12, _cpu, CPU_mm
    ldr  x10, [x12, x9, lsl 3]
    add  x9, x12, x11, lsl 3
    ldr  d0, [x9]
    vsh_body \kind, \L, \a8
    str  d0, [x9]
    gret 1
.gadget amd64_vsh_\name\()_mm
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    ubfx x10, x8, 8, 3
    amd64_vmem_addr
    amd64_vread_prep 8, vsh_\name\()_mm
    add  x14, _cpu, CPU_mm
    add  x14, x14, x10, lsl 3
    ldr  x10, [x9]
    ldr  d0, [x14]
    vsh_body \kind, \L, \a8
    str  d0, [x14]
    gret 3
    amd64_vread_bullshit 8, vsh_\name\()_mm
.endm
amd64_vsh psrlw, srl, 16, 8h, 4h
amd64_vsh psrld, srl, 32, 4s, 2s
amd64_vsh psrlq, srl, 64, 2d, 1d
amd64_vsh psraw, sra, 16, 8h, 4h
amd64_vsh psrad, sra, 32, 4s, 2s
amd64_vsh psllw, sll, 16, 8h, 4h
amd64_vsh pslld, sll, 32, 4s, 2s
amd64_vsh psllq, sll, 64, 2d, 1d

# PSRLDQ / PSLLDQ xmm, imm8: whole bytes, through a table lookup on the byte
# indices i + n (right) or i - n (left); a count past 15 clears it all.
# [dst (0-3) | n (8-15)], gret 1.
.macro amd64_vsh_dq name, right
.gadget amd64_vsh_\name\()_xi
    ldr  x8, [_ip]
    and  x9, x8, 0xf
    ubfx x10, x8, 8, 8
    add  x11, _cpu, CPU_xmm
    add  x9, x11, x9, lsl 4
    ldr  q0, [x9]
    cmp  x10, 15
    b.ls 1f
    movi v0.2d, 0
    b    2f
1:  adr  x11, 3f
    ldr  q2, [x11]                   /* 0, 1, ... 15 */
    dup  v3.16b, w10
.if \right
    add  v2.16b, v2.16b, v3.16b      /* past 15: tbl gives 0 */
.else
    sub  v2.16b, v2.16b, v3.16b      /* below 0 wraps past 15 */
.endif
    tbl  v0.16b, {v0.16b}, v2.16b
2:  str  q0, [x9]
    gret 1
.balign 16
3:  .byte 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
.endm
amd64_vsh_dq psrldq, 1
amd64_vsh_dq pslldq, 0

# ===========================================================================
# MMX moves and the MMX forms of the integer odds and ends (cpu->mm[]; MMX
# register numbers ignore REX). Register forms that touch only mm/xmm leave
# the GPR cache alone; GPR forms are flush style (gen.c writes the cache back
# or flushes it); memory forms are [meta][disp][next_ip] with the mm number in
# meta bits 8-10.
# ===========================================================================

# MOVQ mm, mm (0F 6F / 0F 7F, mod 3): [src (0-2) | dst (4-6)], gret 1
.gadget amd64_mmx_mov_rr
    ldr  x8, [_ip]
    and  x9, x8, 7
    ubfx x10, x8, 4, 3
    add  x11, _cpu, CPU_mm
    ldr  x12, [x11, x9, lsl 3]
    str  x12, [x11, x10, lsl 3]
    gret 1

# mm <- m64 / zero-extended m32 (MOVQ 0F 6F, MOVD/MOVQ 0F 6E). gret 3
.macro amd64_mmx_load name, vb
.gadget amd64_mmx_\name
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    ubfx x10, x8, 8, 3
    amd64_vmem_addr
    amd64_vread_prep \vb, mmx_\name
.if \vb == 4
    ldr  w12, [x9]
.else
    ldr  x12, [x9]
.endif
    add  x11, _cpu, CPU_mm
    str  x12, [x11, x10, lsl 3]
    gret 3
    amd64_vread_bullshit \vb, mmx_\name
.endm
amd64_mmx_load load32, 4
amd64_mmx_load load64, 8

# m64 / m32 <- mm (MOVQ 0F 7F, MOVNTQ 0F E7, MOVD/MOVQ 0F 7E). gret 3
.macro amd64_mmx_store name, vb
.gadget amd64_mmx_\name
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    ubfx x10, x8, 8, 3
    amd64_vmem_addr
    amd64_vwrite_prep \vb, mmx_\name
    add  x11, _cpu, CPU_mm
    ldr  x12, [x11, x10, lsl 3]
.if \vb == 4
    str  w12, [x9]
.else
    str  x12, [x9]
.endif
    amd64_vwrite_done \vb, mmx_\name
    gret 3
    amd64_vwrite_bullshit \vb, mmx_\name
.endm
amd64_mmx_store store32, 4
amd64_mmx_store store64, 8

# MOVD/MOVQ mm, r32/r64 (0F 6E mod 3): [gpr (0-3) | mm (4-6) | 64-bit (8)], gret 1
.gadget amd64_mmx_from_gpr
    ldr  x8, [_ip]
    and  x9, x8, 0xf
    ubfx x10, x8, 4, 3
    add  x11, _cpu, CPU_amd64_regs
    ldr  x12, [x11, x9, lsl 3]
    tbnz x8, 8, 1f
    mov  w12, w12
1:  add  x11, _cpu, CPU_mm
    str  x12, [x11, x10, lsl 3]
    gret 1

# MOVD/MOVQ r32/r64, mm (0F 7E mod 3): [mm (0-2) | gpr (4-7) | 64-bit (8)], gret 1
.gadget amd64_mmx_to_gpr
    ldr  x8, [_ip]
    and  x9, x8, 7
    ubfx x10, x8, 4, 4
    add  x11, _cpu, CPU_mm
    ldr  x12, [x11, x9, lsl 3]
    tbnz x8, 8, 1f
    mov  w12, w12                    /* a 32-bit GPR write zero-extends */
1:  add  x11, _cpu, CPU_amd64_regs
    str  x12, [x11, x10, lsl 3]
    gret 1

# MOVQ2DQ xmm, mm (F3 0F D6): [mm (0-2) | xmm (4-7)]; MOVDQ2Q mm, xmm (F2 0F D6):
# [xmm (0-3) | mm (4-6)]. gret 1
.gadget amd64_mmx_movq2dq
    ldr  x8, [_ip]
    and  x9, x8, 7
    ubfx x10, x8, 4, 4
    add  x11, _cpu, CPU_mm
    ldr  d0, [x11, x9, lsl 3]        /* the upper half is zeroed */
    add  x11, _cpu, CPU_xmm
    add  x11, x11, x10, lsl 4
    str  q0, [x11]
    gret 1
.gadget amd64_mmx_movdq2q
    ldr  x8, [_ip]
    and  x9, x8, 0xf
    ubfx x10, x8, 4, 3
    add  x11, _cpu, CPU_xmm
    add  x11, x11, x9, lsl 4
    ldr  x12, [x11]
    add  x11, _cpu, CPU_mm
    str  x12, [x11, x10, lsl 3]
    gret 1

# PMOVMSKB r32, mm (0F D7): the eight sign bits. [mm (0-2) | gpr (4-7)], gret 1
.gadget amd64_mmx_pmovmskb
    ldr  x8, [_ip]
    and  x9, x8, 7
    ubfx x10, x8, 4, 4
    add  x11, _cpu, CPU_mm
    ldr  d0, [x11, x9, lsl 3]
    ushr v0.8b, v0.8b, 7
    usra v0.4h, v0.4h, 7
    usra v0.2s, v0.2s, 14
    usra d0, d0, 28
    umov w14, v0.b[0]
    add  x11, _cpu, CPU_amd64_regs
    str  x14, [x11, x10, lsl 3]
    gret 1

# PEXTRW r32, mm, imm8 (0F C5): [mm (0-2) | gpr (4-7) | lane (8-9)], gret 1
.gadget amd64_mmx_pextrw
    ldr  x8, [_ip]
    and  x9, x8, 7
    ubfx x10, x8, 4, 4
    ubfx x12, x8, 8, 2
    add  x11, _cpu, CPU_mm
    add  x11, x11, x9, lsl 3
    ldrh w13, [x11, x12, lsl 1]
    add  x11, _cpu, CPU_amd64_regs
    str  x13, [x11, x10, lsl 3]
    gret 1

# PINSRW mm, r32/m16, imm8 (0F C4): reg [gpr (0-3) | mm (4-6) | lane (8-9)],
# gret 1; mem [meta][disp][next_ip][lane], gret 4
.gadget amd64_mmx_pinsrw_reg
    ldr  x8, [_ip]
    and  x9, x8, 0xf
    ubfx x10, x8, 4, 3
    ubfx x12, x8, 8, 2
    add  x11, _cpu, CPU_amd64_regs
    ldr  x13, [x11, x9, lsl 3]
    add  x11, _cpu, CPU_mm
    add  x11, x11, x10, lsl 3
    strh w13, [x11, x12, lsl 1]
    gret 1
.gadget amd64_mmx_pinsrw_mem
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    ubfx x10, x8, 8, 3
    amd64_vmem_addr
    amd64_vread_prep 2, mmx_pinsrw_mem
    ldrh w13, [x9]
    ldr  x12, [_ip, 24]
    and  x12, x12, 3
    add  x11, _cpu, CPU_mm
    add  x11, x11, x10, lsl 3
    strh w13, [x11, x12, lsl 1]
    gret 4
    amd64_vread_bullshit 2, mmx_pinsrw_mem

# PSHUFW mm, mm/m64, imm8 (0F 70): word i = source word (imm >> 2i) & 3.
# reg [src (0-2) | dst (4-6) | imm (8-15)], gret 1; mem [meta][disp][next_ip][imm], gret 4
.macro amd64_pshufw_body               /* x13 = source, x14 = imm, x15 = dst slot */
    mov  x12, 0
.irp i, 0, 1, 2, 3
    ubfx x5, x14, (2 * \i), 2
    lsl  x5, x5, 4
    lsr  x6, x13, x5
    and  x6, x6, 0xffff
    orr  x12, x12, x6, lsl #(16 * \i)
.endr
    str  x12, [x15]
.endm
.gadget amd64_mmx_pshufw_reg
    ldr  x8, [_ip]
    and  x9, x8, 7
    ubfx x10, x8, 4, 3
    ubfx x14, x8, 8, 8
    add  x11, _cpu, CPU_mm
    ldr  x13, [x11, x9, lsl 3]
    add  x15, x11, x10, lsl 3
    amd64_pshufw_body
    gret 1
.gadget amd64_mmx_pshufw_mem
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    ubfx x10, x8, 8, 3
    amd64_vmem_addr
    amd64_vread_prep 8, mmx_pshufw_mem
    ldr  x13, [x9]
    ldr  x14, [_ip, 24]
    add  x11, _cpu, CPU_mm
    add  x15, x11, x10, lsl 3
    amd64_pshufw_body
    gret 4
    amd64_vread_bullshit 8, mmx_pshufw_mem

# MASKMOVQ mm, mm (0F F7) / MASKMOVDQU xmm, xmm (66 0F F7): the bytes of the
# first operand whose mask byte (the second) has its top bit set, stored to
# [rdi]. The whole width is write-prepped (a fault re-runs it) and merged.
# [data (0-3) | mask (4-7)], gret 1 -- rip flushed, rdi in the register file.
.macro amd64_gen_maskmov name, vb
.gadget amd64_\name
    ldr  x8, [_ip]
    and  x24, x8, 0xf                /* x24/x25 (the flushed reg cache) survive the prep */
    ubfx x25, x8, 4, 4
    ldr  x9, [_cpu, CPU_amd64_regs + 7 * 8]   /* rdi */
    amd64_vwrite_prep \vb, \name
.if \vb == 8
    add  x11, _cpu, CPU_mm
    ldr  d0, [x11, x24, lsl 3]
    ldr  d1, [x11, x25, lsl 3]
    ldr  d2, [x9]
    cmlt v1.8b, v1.8b, 0
    bsl  v1.8b, v0.8b, v2.8b
    str  d1, [x9]
.else
    add  x11, _cpu, CPU_xmm
    add  x12, x11, x24, lsl 4
    ldr  q0, [x12]
    add  x12, x11, x25, lsl 4
    ldr  q1, [x12]
    ldr  q2, [x9]
    cmlt v1.16b, v1.16b, 0
    bsl  v1.16b, v0.16b, v2.16b
    str  q1, [x9]
.endif
    amd64_vwrite_done \vb, \name
    gret 1
    amd64_vwrite_bullshit \vb, \name
.endm
amd64_gen_maskmov maskmovq, 8
amd64_gen_maskmov maskmovdqu, 16

# ===========================================================================
# SSE/SSE2/SSE3 floating point for the amd64 guest, exact to x86
# (math.S amd64_xf_*): ADD SUB MUL DIV MIN MAX SQRT CMPcc (U)COMIS ADDSUB
# HADD HSUB, packed (ps/pd) and scalar (ss/sd), register and memory sources.
#
# The host does the arithmetic under MXCSR's rounding mode (emu/fpenv.c puts
# it on FPCR, with FZ off for amd64) and its IE ZE OE UE PE flags fold into
# MXCSR. What x86 does differently is done here, in the integer domain so it
# raises nothing itself:
#  - DAZ: denormal operands become signed zeros first; without DAZ a lane
#    with a denormal operand raises DE -- unless the lane has a NaN operand,
#    or (DIV) a zero divisor, or (SQRT) a negative operand: those exceptions
#    come first and DE is not reported.
#  - FTZ: a denormal result becomes a signed zero, raising UE and PE.
#  - NaNs: a NaN result is the first operand's NaN, quieted, else the
#    second's, else the default NaN -- which on x86 is negative (0xffc00000,
#    0xfff8...), where aarch64's is positive.
#  - MIN/MAX: a < b ? a : b (a > b for MAX) -- the second operand when
#    either is a NaN or both are zeros, as a signalling compare.
# Scalar forms compute lane 0 alone (so no other lane raises a flag) and
# keep the rest of the destination.
#
# v0 = the first (destination) operand, v1 = the second; v4/v5 = the same
# after DAZ; v2 = the result; v7 = the active lanes; v16 exponent mask, v17
# mantissa mask, v19 quiet bit; v3 v6 v18 scratch. x14 = &xmm[dst]; the
# bodies use x9 x11 x12 w15-w17 and leave x10 and x14.
# Gadgets: _r [src (0-3) | dst (4-7) | imm (8-15)], gret 1;
#          _m [meta][disp][next_ip][imm], dst in meta 8-11, gret 4.
# ===========================================================================

.macro xf_consts t
.ifc \t,s
    mov  w9, 0x7f800000
    dup  v16.4s, w9
    mov  w9, 0x7fffff
    dup  v17.4s, w9
    mov  w9, 0x400000
    dup  v19.4s, w9
.else
    mov  x9, 0x7ff0000000000000
    dup  v16.2d, x9
    mov  x9, 0xfffffffffffff
    dup  v17.2d, x9
    mov  x9, 0x8000000000000
    dup  v19.2d, x9
.endif
.endm

.macro xf_lanes t, p                    /* v7: the lanes the op writes */
.if \p
    movi v7.16b, 0xff
.else
  .ifc \t,s
    movi d7, 0x00000000ffffffff
  .else
    movi d7, 0xffffffffffffffff
  .endif
.endif
.endm

.macro xf_denorm m, x, a, tmp           /* exponent 0, mantissa not */
    cmtst \tmp\().\a, \x\().\a, v16.\a
    cmtst \m\().\a, \x\().\a, v17.\a
    bic  \m\().16b, \m\().16b, \tmp\().16b
.endm

.macro xf_isnan m, x, a                 /* |x| above infinity */
    shl  \m\().\a, \x\().\a, 1
    ushr \m\().\a, \m\().\a, 1
    cmhi \m\().\a, \m\().\a, v16.\a
.endm

.macro xf_any m, label                  /* branch if no byte of \m is set */
    umaxv b18, \m\().16b
    fmov w11, s18
    cbz  w11, \label
.endm

# Both operands: DAZ, or DE. excl 1 = DIV (a zero divisor comes first).
.macro xf_pre2 a, excl
    mov  v4.16b, v0.16b
    mov  v5.16b, v1.16b
    xf_denorm v3, v0, \a, v2
    xf_denorm v6, v1, \a, v2
    orr  v2.16b, v3.16b, v6.16b
    and  v2.16b, v2.16b, v7.16b
    xf_any v2, .Lxp2_done_\@
    ldr  w9, [_cpu, CPU_mxcsr]
    tbz  w9, 6, .Lxp2_de_\@
    orr  v18.16b, v16.16b, v17.16b      /* DAZ: signed zeros */
    and  v3.16b, v3.16b, v18.16b
    and  v6.16b, v6.16b, v18.16b
    bic  v4.16b, v4.16b, v3.16b
    bic  v5.16b, v5.16b, v6.16b
    b    .Lxp2_done_\@
.Lxp2_de_\@:
    xf_isnan v3, v0, \a
    xf_isnan v6, v1, \a
    orr  v3.16b, v3.16b, v6.16b
    bic  v2.16b, v2.16b, v3.16b
.if \excl == 1
    shl  v6.\a, v1.\a, 1
    cmeq v6.\a, v6.\a, 0
    bic  v2.16b, v2.16b, v6.16b
.endif
    xf_any v2, .Lxp2_done_\@
    orr  w9, w9, 2
    str  w9, [_cpu, CPU_mxcsr]
.Lxp2_done_\@:
.endm

# The second operand only (SQRT): a negative one is invalid first.
.macro xf_pre1 a
    mov  v5.16b, v1.16b
    xf_denorm v6, v1, \a, v2
    and  v2.16b, v6.16b, v7.16b
    xf_any v2, .Lxp1_done_\@
    ldr  w9, [_cpu, CPU_mxcsr]
    tbz  w9, 6, .Lxp1_de_\@
    orr  v18.16b, v16.16b, v17.16b
    and  v6.16b, v6.16b, v18.16b
    bic  v5.16b, v5.16b, v6.16b
    b    .Lxp1_done_\@
.Lxp1_de_\@:
    cmlt v3.\a, v1.\a, 0
    bic  v2.16b, v2.16b, v3.16b
    xf_any v2, .Lxp1_done_\@
    orr  w9, w9, 2
    str  w9, [_cpu, CPU_mxcsr]
.Lxp1_done_\@:
.endm

.macro xf_ftz a                         /* FTZ: denormal results -> signed zeros, UE PE */
    ldr  w9, [_cpu, CPU_mxcsr]
    tbz  w9, 15, .Lxz_done_\@
    xf_denorm v3, v2, \a, v6
    and  v3.16b, v3.16b, v7.16b
    xf_any v3, .Lxz_done_\@
    orr  v18.16b, v16.16b, v17.16b
    and  v3.16b, v3.16b, v18.16b
    bic  v2.16b, v2.16b, v3.16b
    orr  w9, w9, 0x30
    str  w9, [_cpu, CPU_mxcsr]
.Lxz_done_\@:
.endm

.macro xf_defnan t                      /* v18: x86's default NaN */
.ifc \t,s
    mov  w9, 0xffc00000
    dup  v18.4s, w9
.else
    mov  x9, 0xfff8000000000000
    dup  v18.2d, x9
.endif
.endm

# A NaN result: the first operand's NaN (quieted), else the second's, else
# the default NaN. n = 2 (v0, v1) or 1 (v1).
.macro xf_nanfix t, a, n
    xf_isnan v3, v2, \a
    and  v3.16b, v3.16b, v7.16b
    xf_any v3, .Lxn_done_\@
    xf_defnan \t
    xf_isnan v5, v1, \a
    orr  v6.16b, v1.16b, v19.16b
    bsl  v5.16b, v6.16b, v18.16b
.if \n == 2
    xf_isnan v4, v0, \a
    orr  v6.16b, v0.16b, v19.16b
    bsl  v4.16b, v6.16b, v5.16b
    bit  v2.16b, v4.16b, v3.16b
.else
    bit  v2.16b, v5.16b, v3.16b
.endif
.Lxn_done_\@:
.endm

.macro xf_merge t, p                    /* the result into v2, whole */
.if \p == 0
    mov  v0.\t[0], v2.\t[0]
    mov  v2.16b, v0.16b
.endif
.endm

# ---- bodies (v0, v1 loaded; result v2) ----
.macro xf_body_arith op, t, a, p, excl
    xf_consts \t
    xf_lanes \t, \p
    xf_pre2 \a, \excl
.if \p
    \op  v2.\a, v4.\a, v5.\a
.else
    \op  \t\()2, \t\()4, \t\()5
.endif
    xf_ftz \a
    xf_nanfix \t, \a, 2
    xf_merge \t, \p
.endm

.macro xf_body_minmax max, t, a, p
    xf_consts \t
    xf_lanes \t, \p
    xf_pre2 \a, 0
.if \p
  .if \max
    fcmgt v3.\a, v4.\a, v5.\a
  .else
    fcmgt v3.\a, v5.\a, v4.\a
  .endif
.else
  .if \max
    fcmgt \t\()3, \t\()4, \t\()5
  .else
    fcmgt \t\()3, \t\()5, \t\()4
  .endif
.endif
    bsl  v3.16b, v4.16b, v5.16b
    mov  v2.16b, v3.16b
    xf_merge \t, \p
.endm

.macro xf_body_sqrt t, a, p
    xf_consts \t
    xf_lanes \t, \p
    xf_pre1 \a
.if \p
    fsqrt v2.\a, v5.\a
.else
    fsqrt \t\()2, \t\()5
.endif
    xf_nanfix \t, \a, 1
    xf_merge \t, \p
.endm

# CMPcc: imm in w12 (0-7): EQ LT LE UNORD NEQ NLT NLE ORD. EQ/UNORD/NEQ/ORD
# are quiet (IE for an SNaN), LT/LE/NLT/NLE signalling (IE for any NaN).
.macro xf_body_cmp t, a, p
    xf_consts \t
    xf_lanes \t, \p
    xf_pre2 \a, 0
    and  w12, w12, 7
    cmp  w12, 4
    b.lo 1f
    sub  w12, w12, 4
    mov  w15, 1                          /* NEQ NLT NLE ORD: the complements */
    b    2f
1:  mov  w15, 0
2:  cbz  w12, 10f
    cmp  w12, 1
    b.eq 11f
    cmp  w12, 2
    b.eq 12f
.if \p                                   /* UNORD / ORD */
    fcmeq v2.\a, v4.\a, v4.\a
    fcmeq v3.\a, v5.\a, v5.\a
.else
    fcmeq \t\()2, \t\()4, \t\()4
    fcmeq \t\()3, \t\()5, \t\()5
.endif
    and  v2.16b, v2.16b, v3.16b
    eor  w15, w15, 1                     /* UNORD is the complement of ORD */
    b    13f
10:
.if \p
    fcmeq v2.\a, v4.\a, v5.\a
.else
    fcmeq \t\()2, \t\()4, \t\()5
.endif
    b    13f
11:
.if \p
    fcmgt v2.\a, v5.\a, v4.\a
.else
    fcmgt \t\()2, \t\()5, \t\()4
.endif
    b    13f
12:
.if \p
    fcmge v2.\a, v5.\a, v4.\a
.else
    fcmge \t\()2, \t\()5, \t\()4
.endif
13: cbz  w15, 14f
    mvn  v2.16b, v2.16b
14: xf_merge \t, \p
.endm

# ADDSUB: the even lanes subtract (as an add of the negated second operand:
# the same rounding and zero signs; the NaN fix sees the original).
.macro xf_body_addsub t, a
    xf_consts \t
    xf_lanes \t, 1
    xf_pre2 \a, 0
.ifc \t,s
    mov  x9, 0x80000000
    dup  v3.2d, x9
.else
    mov  x9, 0x8000000000000000
    fmov d3, x9
.endif
    eor  v5.16b, v5.16b, v3.16b
    fadd v2.\a, v4.\a, v5.\a
    xf_ftz \a
    xf_nanfix \t, \a, 2
.endm

# HADD / HSUB: the pairs of each operand, regrouped into first (the lower
# element) and second (the upper) operands: r = lo + hi, lo - hi. The lower
# is the first operand for the NaN choice too (hardware: an AMD Ryzen).
.macro xf_body_hadd sub, t, a
    uzp1 v2.\a, v0.\a, v1.\a             /* lo elements */
    uzp2 v3.\a, v0.\a, v1.\a             /* hi elements */
    mov  v0.16b, v2.16b
    mov  v1.16b, v3.16b
.if \sub
    xf_body_arith fsub, \t, \a, 1, 0
.else
    xf_body_arith fadd, \t, \a, 1, 0
.endif
.endm

# ---- the gadget frames ----
.macro xf_frame_r name
.gadget amd64_xf_\name\()_r
    ldr  x8, [_ip]
    and  x9, x8, 0xf
    ubfx x10, x8, 4, 4
    ubfx w12, w8, 8, 8
    add  x11, _cpu, CPU_xmm
    add  x9, x11, x9, lsl 4
    add  x14, x11, x10, lsl 4
    ldr  q0, [x14]
    ldr  q1, [x9]
.endm
.macro xf_frame_m name, vb
.gadget amd64_xf_\name\()_m
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    ubfx x10, x8, 8, 4
    amd64_vmem_addr
    amd64_vread_prep \vb, xf_\name\()_m
.if \vb == 16
    ldr  q1, [x9]
.elseif \vb == 8
    ldr  d1, [x9]
.else
    ldr  s1, [x9]
.endif
    add  x14, _cpu, CPU_xmm
    add  x14, x14, x10, lsl 4
    ldr  q0, [x14]
    ldr  w12, [_ip, 24]
.endm

.macro amd64_xf name, body, t, a, p, arg1=0, arg2=0
    xf_frame_r \name
.ifc \body,arith
    xf_body_arith \arg1, \t, \a, \p, \arg2
.endif
.ifc \body,minmax
    xf_body_minmax \arg1, \t, \a, \p
.endif
.ifc \body,sqrt
    xf_body_sqrt \t, \a, \p
.endif
.ifc \body,cmp
    xf_body_cmp \t, \a, \p
.endif
.ifc \body,addsub
    xf_body_addsub \t, \a
.endif
.ifc \body,hadd
    xf_body_hadd \arg1, \t, \a
.endif
    str  q2, [x14]
    gret 1
.if \p
    xf_frame_m \name, 16
.else
  .ifc \t,s
    xf_frame_m \name, 4
  .else
    xf_frame_m \name, 8
  .endif
.endif
.ifc \body,arith
    xf_body_arith \arg1, \t, \a, \p, \arg2
.endif
.ifc \body,minmax
    xf_body_minmax \arg1, \t, \a, \p
.endif
.ifc \body,sqrt
    xf_body_sqrt \t, \a, \p
.endif
.ifc \body,cmp
    xf_body_cmp \t, \a, \p
.endif
.ifc \body,addsub
    xf_body_addsub \t, \a
.endif
.ifc \body,hadd
    xf_body_hadd \arg1, \t, \a
.endif
    str  q2, [x14]
    gret 4
.if \p
    amd64_vread_bullshit 16, xf_\name\()_m
.else
  .ifc \t,s
    amd64_vread_bullshit 4, xf_\name\()_m
  .else
    amd64_vread_bullshit 8, xf_\name\()_m
  .endif
.endif
.endm

.irp op, add, sub, mul, div
  .ifc \op,div
    amd64_xf \op\()ps, arith, s, 4s, 1, fdiv, 1
    amd64_xf \op\()pd, arith, d, 2d, 1, fdiv, 1
    amd64_xf \op\()ss, arith, s, 4s, 0, fdiv, 1
    amd64_xf \op\()sd, arith, d, 2d, 0, fdiv, 1
  .else
    amd64_xf \op\()ps, arith, s, 4s, 1, f\op, 0
    amd64_xf \op\()pd, arith, d, 2d, 1, f\op, 0
    amd64_xf \op\()ss, arith, s, 4s, 0, f\op, 0
    amd64_xf \op\()sd, arith, d, 2d, 0, f\op, 0
  .endif
.endr
amd64_xf minps, minmax, s, 4s, 1, 0
amd64_xf minpd, minmax, d, 2d, 1, 0
amd64_xf minss, minmax, s, 4s, 0, 0
amd64_xf minsd, minmax, d, 2d, 0, 0
amd64_xf maxps, minmax, s, 4s, 1, 1
amd64_xf maxpd, minmax, d, 2d, 1, 1
amd64_xf maxss, minmax, s, 4s, 0, 1
amd64_xf maxsd, minmax, d, 2d, 0, 1
amd64_xf sqrtps, sqrt, s, 4s, 1
amd64_xf sqrtpd, sqrt, d, 2d, 1
amd64_xf sqrtss, sqrt, s, 4s, 0
amd64_xf sqrtsd, sqrt, d, 2d, 0
amd64_xf cmpps, cmp, s, 4s, 1
amd64_xf cmppd, cmp, d, 2d, 1
amd64_xf cmpss, cmp, s, 4s, 0
amd64_xf cmpsd, cmp, d, 2d, 0
amd64_xf addsubps, addsub, s, 4s, 1
amd64_xf addsubpd, addsub, d, 2d, 1
amd64_xf haddps, hadd, s, 4s, 1, 0
amd64_xf haddpd, hadd, d, 2d, 1, 0
amd64_xf hsubps, hadd, s, 4s, 1, 1
amd64_xf hsubpd, hadd, d, 2d, 1, 1

# ---- conversions, RCP/RSQRT, MOVMSK, the 64-bit-half moves (amd64_xf_*) ----
#
# float -> int: NaN, or a value out of range after rounding, is the integer
# indefinite (0x80000000 / 0x8000...) with IE -- aarch64 saturates and gives
# 0 for a NaN. The range check is made on the rounded value (FRINTI, no
# flags); the conversion then runs on the in-range lanes only, so an invalid
# lane does not also raise PE. DAZ applies; DE does not (x86 raises none).
# float -> float: DAZ, DE, and FTZ on the narrowing ones.

# DE for a denormal source operand (no exclusion), or DAZ.
.macro xf_pre1n a
    mov  v5.16b, v1.16b
    xf_denorm v6, v1, \a, v2
    and  v2.16b, v6.16b, v7.16b
    xf_any v2, .Lxq_done_\@
    ldr  w9, [_cpu, CPU_mxcsr]
    tbz  w9, 6, .Lxq_de_\@
    orr  v18.16b, v16.16b, v17.16b
    and  v6.16b, v6.16b, v18.16b
    bic  v5.16b, v5.16b, v6.16b
    b    .Lxq_done_\@
.Lxq_de_\@:
    orr  w9, w9, 2
    str  w9, [_cpu, CPU_mxcsr]
.Lxq_done_\@:
.endm

# DAZ only (float -> int)
.macro xf_daz a
    mov  v5.16b, v1.16b
    ldr  w9, [_cpu, CPU_mxcsr]
    tbz  w9, 6, .Lxd_done_\@
    xf_denorm v6, v1, \a, v2
    orr  v18.16b, v16.16b, v17.16b
    and  v6.16b, v6.16b, v18.16b
    bic  v5.16b, v5.16b, v6.16b
.Lxd_done_\@:
.endm

# Packed float -> int32 (s: 4 lanes; d: 2 lanes, narrowed into the low half).
# v5 the (DAZ'd) source; out v2. trunc: CVTT. Clobbers v3 v4 v6 v18.
.macro xf_f2i_packed t, a, trunc
.ifc \t,s
    mov  w9, 0x4f000000                  /* 2^31 */
    dup  v4.4s, w9
    mov  w9, 0xcf000000                  /* -2^31 */
    dup  v18.4s, w9
.else
    mov  x9, 0x41e0000000000000
    dup  v4.2d, x9
    mov  x9, 0xc1e0000000000000
    dup  v18.2d, x9
.endif
.if \trunc
    frintz v3.\a, v5.\a
.else
    frinti v3.\a, v5.\a
.endif
    fcmgt v6.\a, v4.\a, v3.\a            /* r < 2^31 (false for a NaN: IE) */
    fcmge v3.\a, v3.\a, v18.\a           /* r >= -2^31 */
    and  v6.16b, v6.16b, v3.16b          /* valid */
    xf_isnan v3, v5, \a
    orr  v3.16b, v3.16b, v6.16b
    mvn  v3.16b, v3.16b                  /* out of range and not a NaN: IE by hand */
    and  v3.16b, v3.16b, v7.16b
    xf_any v3, .Lxfi_ok_\@
    ldr  w9, [_cpu, CPU_mxcsr]
    orr  w9, w9, 1
    str  w9, [_cpu, CPU_mxcsr]
.Lxfi_ok_\@:
    and  v4.16b, v5.16b, v6.16b          /* in-range lanes only (0 elsewhere) */
.if \trunc == 0
    frintx v4.\a, v4.\a
.endif
    fcvtzs v4.\a, v4.\a
.ifc \t,s
    movi v3.4s, 0x80, lsl 24
.else
    mov  x9, 0x80000000                  /* the int32 indefinite, in the half xtn keeps */
    dup  v3.2d, x9
.endif
    bsl  v6.16b, v4.16b, v3.16b
.ifc \t,s
    mov  v2.16b, v6.16b
.else
    xtn  v2.2s, v6.2d                    /* the 64-bit results fit 32 bits; upper half 0 */
.endif
.endm

# Scalar float (lane 0 of v5) -> a GPR (w12/x12): iw 32 or 64.
.macro xf_f2i_scalar t, iw, trunc
.ifc \t,s
  .if \iw == 32
    mov  w9, 0x4f000000
  .else
    mov  w9, 0x5f000000
  .endif
    fmov s4, w9
    eor  w9, w9, 0x80000000
    fmov s18, w9
.else
  .if \iw == 32
    mov  x9, 0x41e0000000000000
  .else
    mov  x9, 0x43e0000000000000
  .endif
    fmov d4, x9
    eor  x9, x9, 0x8000000000000000
    fmov d18, x9
.endif
.if \trunc
    frintz \t\()3, \t\()5
.else
    frinti \t\()3, \t\()5
.endif
    fcmp \t\()3, \t\()3
    b.vs 8f                              /* NaN */
    fcmp \t\()3, \t\()4
    b.ge 7f
    fcmp \t\()3, \t\()18
    b.lt 7f
.if \trunc == 0
    frintx \t\()3, \t\()5
.else
    fmov \t\()3, \t\()5
.endif
.if \iw == 32
    fcvtzs w12, \t\()3
.else
    fcvtzs x12, \t\()3
.endif
    b    9f
7:  ldr  w9, [_cpu, CPU_mxcsr]           /* out of range: IE by hand */
    orr  w9, w9, 1
    str  w9, [_cpu, CPU_mxcsr]
8:  fcvtzs w12, \t\()5                   /* a NaN: the host raises IE (result unused) */
    mov  x12, 0x8000000000000000
.if \iw == 32
    mov  w12, 0x80000000
.endif
9:
.endm

# xmm -> xmm conversions: [src | dst | -] / [meta][disp][next][-], v0 dst, v1 src
.macro xf_cvt name, vb, body
    xf_frame_r \name
    \body
    str  q2, [x14]
    gret 1
    xf_frame_m \name, \vb
    \body
    str  q2, [x14]
    gret 4
    amd64_vread_bullshit \vb, xf_\name\()_m
.endm

.macro xfb_cvtps2pd
    xf_consts s
    movi d7, 0xffffffffffffffff          /* the two low singles */
    xf_pre1n 4s
    fcvtl v2.2d, v5.2s
.endm
.macro xfb_cvtpd2ps
    xf_consts d
    movi v7.16b, 0xff
    xf_pre1n 2d
    fcvtn v2.2s, v5.2d
    xf_consts s
    movi d7, 0xffffffffffffffff
    xf_ftz 4s
.endm
.macro xfb_cvtss2sd
    xf_consts s
    movi d7, 0x00000000ffffffff
    xf_pre1n 4s
    fcvt d2, s5
    mov  v0.d[0], v2.d[0]
    mov  v2.16b, v0.16b
.endm
.macro xfb_cvtsd2ss
    xf_consts d
    movi d7, 0xffffffffffffffff
    xf_pre1n 2d
    fcvt s2, d5
    xf_consts s
    movi d7, 0x00000000ffffffff
    xf_ftz 4s
    mov  v0.s[0], v2.s[0]
    mov  v2.16b, v0.16b
.endm
.macro xfb_cvtdq2ps
    scvtf v2.4s, v1.4s
.endm
.macro xfb_cvtdq2pd
    sxtl v2.2d, v1.2s
    scvtf v2.2d, v2.2d
.endm
.macro xfb_cvtps2dq
    xf_consts s
    movi v7.16b, 0xff
    xf_daz 4s
    xf_f2i_packed s, 4s, 0
.endm
.macro xfb_cvttps2dq
    xf_consts s
    movi v7.16b, 0xff
    xf_daz 4s
    xf_f2i_packed s, 4s, 1
.endm
.macro xfb_cvtpd2dq
    xf_consts d
    movi v7.16b, 0xff
    xf_daz 2d
    xf_f2i_packed d, 2d, 0
.endm
.macro xfb_cvttpd2dq
    xf_consts d
    movi v7.16b, 0xff
    xf_daz 2d
    xf_f2i_packed d, 2d, 1
.endm
xf_cvt cvtps2pd, 8, xfb_cvtps2pd
xf_cvt cvtpd2ps, 16, xfb_cvtpd2ps
xf_cvt cvtss2sd, 4, xfb_cvtss2sd
xf_cvt cvtsd2ss, 8, xfb_cvtsd2ss
xf_cvt cvtdq2ps, 16, xfb_cvtdq2ps
xf_cvt cvtdq2pd, 8, xfb_cvtdq2pd
xf_cvt cvtps2dq, 16, xfb_cvtps2dq
xf_cvt cvttps2dq, 16, xfb_cvttps2dq
xf_cvt cvtpd2dq, 16, xfb_cvtpd2dq
xf_cvt cvttpd2dq, 16, xfb_cvttpd2dq

# CVT(T)SS2SI / CVT(T)SD2SI: xmm or memory -> a GPR. reg [xmm (0-3) | gpr
# (4-7)], gret 1; mem [meta][disp][next_ip][-], the GPR in meta 8-11, gret 4.
.macro xf_cvt2gpr name, t, vb, iw, trunc, a
.gadget amd64_xf_\name\()_r
    ldr  x8, [_ip]
    and  x9, x8, 0xf
    ubfx x10, x8, 4, 4
    add  x11, _cpu, CPU_xmm
    add  x9, x11, x9, lsl 4
    ldr  q1, [x9]
    xf_consts \t
    xf_daz \a
    xf_f2i_scalar \t, \iw, \trunc
    add  x11, _cpu, CPU_amd64_regs
.if \iw == 32
    mov  w12, w12
.endif
    str  x12, [x11, x10, lsl 3]
    gret 1
.gadget amd64_xf_\name\()_m
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    ubfx x10, x8, 8, 4
    amd64_vmem_addr
    amd64_vread_prep \vb, xf_\name\()_m
.if \vb == 4
    ldr  s1, [x9]
.else
    ldr  d1, [x9]
.endif
    xf_consts \t
    xf_daz \a
    xf_f2i_scalar \t, \iw, \trunc
    add  x11, _cpu, CPU_amd64_regs
.if \iw == 32
    mov  w12, w12
.endif
    str  x12, [x11, x10, lsl 3]
    gret 4
    amd64_vread_bullshit \vb, xf_\name\()_m
.endm
xf_cvt2gpr cvtss2si32, s, 4, 32, 0, 4s
xf_cvt2gpr cvtss2si64, s, 4, 64, 0, 4s
xf_cvt2gpr cvttss2si32, s, 4, 32, 1, 4s
xf_cvt2gpr cvttss2si64, s, 4, 64, 1, 4s
xf_cvt2gpr cvtsd2si32, d, 8, 32, 0, 2d
xf_cvt2gpr cvtsd2si64, d, 8, 64, 0, 2d
xf_cvt2gpr cvttsd2si32, d, 8, 32, 1, 2d
xf_cvt2gpr cvttsd2si64, d, 8, 64, 1, 2d

# CVTSI2SS / CVTSI2SD: a GPR or memory (32/64) -> lane 0, the rest kept.
# reg [gpr (0-3) | xmm (4-7) | 64-bit (8)], gret 1; mem [meta][disp][next_ip][64-bit], gret 4.
.macro xf_cvtsi name, t
.gadget amd64_xf_\name\()_r
    ldr  x8, [_ip]
    and  x9, x8, 0xf
    ubfx x10, x8, 4, 4
    add  x11, _cpu, CPU_amd64_regs
    ldr  x12, [x11, x9, lsl 3]
    add  x14, _cpu, CPU_xmm
    add  x14, x14, x10, lsl 4
    ldr  q0, [x14]
    tbnz x8, 8, 1f
    scvtf \t\()2, w12
    b    2f
1:  scvtf \t\()2, x12
2:  mov  v0.\t[0], v2.\t[0]
    str  q0, [x14]
    gret 1
.gadget amd64_xf_\name\()_m4
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    ubfx x10, x8, 8, 4
    amd64_vmem_addr
    amd64_vread_prep 4, xf_\name\()_m4
    ldr  w12, [x9]
    scvtf \t\()2, w12
    add  x14, _cpu, CPU_xmm
    add  x14, x14, x10, lsl 4
    ldr  q0, [x14]
    mov  v0.\t[0], v2.\t[0]
    str  q0, [x14]
    gret 4
    amd64_vread_bullshit 4, xf_\name\()_m4
.gadget amd64_xf_\name\()_m8
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    ubfx x10, x8, 8, 4
    amd64_vmem_addr
    amd64_vread_prep 8, xf_\name\()_m8
    ldr  x12, [x9]
    scvtf \t\()2, x12
    add  x14, _cpu, CPU_xmm
    add  x14, x14, x10, lsl 4
    ldr  q0, [x14]
    mov  v0.\t[0], v2.\t[0]
    str  q0, [x14]
    gret 4
    amd64_vread_bullshit 8, xf_\name\()_m8
.endm
xf_cvtsi cvtsi2ss, s
xf_cvtsi cvtsi2sd, d

# The MMX conversions. CVTPI2PS: two int32 (mm or m64) -> the low two singles,
# the high half kept; CVTPI2PD: -> two doubles. CVT(T)PS2PI: the low two
# singles (xmm or m64) -> mm; CVT(T)PD2PI: two doubles (xmm or m128) -> mm.
# reg [src (0-3) | dst (4-7)], gret 1; mem [meta][disp][next_ip][-], dst in meta, gret 4.
.macro xf_mmxcvt name, kind, vb
.gadget amd64_xf_\name\()_r
    ldr  x8, [_ip]
    and  x9, x8, 0xf
    ubfx x10, x8, 4, 4
.ifc \kind,pi2
    add  x11, _cpu, CPU_mm
    ldr  d1, [x11, x9, lsl 3]
    add  x14, _cpu, CPU_xmm
    add  x14, x14, x10, lsl 4
.else
    add  x11, _cpu, CPU_xmm
    add  x9, x11, x9, lsl 4
    ldr  q1, [x9]
    add  x14, _cpu, CPU_mm
    add  x14, x14, x10, lsl 3
.endif
    xf_mmxcvt_body \name
    gret 1
.gadget amd64_xf_\name\()_m
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    ubfx x10, x8, 8, 4
    amd64_vmem_addr
    amd64_vread_prep \vb, xf_\name\()_m
.if \vb == 16
    ldr  q1, [x9]
.else
    ldr  d1, [x9]
.endif
.ifc \kind,pi2
    add  x14, _cpu, CPU_xmm
    add  x14, x14, x10, lsl 4
.else
    and  x10, x10, 7
    add  x14, _cpu, CPU_mm
    add  x14, x14, x10, lsl 3
.endif
    xf_mmxcvt_body \name
    gret 4
    amd64_vread_bullshit \vb, xf_\name\()_m
.endm
.macro xf_mmxcvt_body name
.ifc \name,cvtpi2ps
    ldr  q0, [x14]
    scvtf v2.2s, v1.2s
    mov  v0.d[0], v2.d[0]
    str  q0, [x14]
.endif
.ifc \name,cvtpi2pd
    sxtl v2.2d, v1.2s
    scvtf v2.2d, v2.2d
    str  q2, [x14]
.endif
.ifc \name,cvtps2pi
    xf_consts s
    movi d7, 0xffffffffffffffff
    xf_daz 4s
    xf_f2i_packed s, 4s, 0
    str  d2, [x14]
.endif
.ifc \name,cvttps2pi
    xf_consts s
    movi d7, 0xffffffffffffffff
    xf_daz 4s
    xf_f2i_packed s, 4s, 1
    str  d2, [x14]
.endif
.ifc \name,cvtpd2pi
    xf_consts d
    movi v7.16b, 0xff
    xf_daz 2d
    xf_f2i_packed d, 2d, 0
    str  d2, [x14]
.endif
.ifc \name,cvttpd2pi
    xf_consts d
    movi v7.16b, 0xff
    xf_daz 2d
    xf_f2i_packed d, 2d, 1
    str  d2, [x14]
.endif
.endm
xf_mmxcvt cvtpi2ps, pi2, 8
xf_mmxcvt cvtpi2pd, pi2, 8
xf_mmxcvt cvtps2pi, 2pi, 8
xf_mmxcvt cvttps2pi, 2pi, 8
xf_mmxcvt cvtpd2pi, 2pi, 16
xf_mmxcvt cvttpd2pi, 2pi, 16

# RCPPS/SS, RSQRTPS/SS: the host estimate (8 bits) and one Newton step
# (~16 bits; x86 guarantees 12). x86 raises no flag for these, so FPSR is
# put back afterwards. Zeros, infinities, NaNs keep the estimate's answer.
.macro xf_recip_body rsq, p
    mrs  x12, fpsr
.if \rsq
  .if \p
    frsqrte v2.4s, v1.4s
    fmul v3.4s, v1.4s, v2.4s
    frsqrts v3.4s, v3.4s, v2.4s
  .else
    frsqrte s2, s1
    fmul s3, s1, s2
    frsqrts s3, s3, s2
  .endif
.else
  .if \p
    frecpe v2.4s, v1.4s
    frecps v3.4s, v1.4s, v2.4s
  .else
    frecpe s2, s1
    frecps s3, s1, s2
  .endif
.endif
    fmul v3.4s, v2.4s, v3.4s
    msr  fpsr, x12
    mov  w9, 0x7f800000
    dup  v16.4s, w9
    cmtst v4.4s, v2.4s, v16.4s           /* exponent nonzero */
    and  v5.16b, v2.16b, v16.16b
    cmeq v5.4s, v5.4s, v16.4s            /* exponent all ones */
    bic  v4.16b, v4.16b, v5.16b          /* finite, nonzero, normal: refine */
    bit  v2.16b, v3.16b, v4.16b
.if \rsq                                 /* a negative (non-zero, non-NaN) operand: */
    cmlt v3.4s, v1.4s, 0                 /* x86's default NaN, which is negative */
    shl  v4.4s, v1.4s, 1
    cmeq v5.4s, v4.4s, 0
    bic  v3.16b, v3.16b, v5.16b          /* not -0 */
    mov  w9, 0xff000000
    dup  v5.4s, w9
    cmhi v4.4s, v4.4s, v5.4s             /* |x| << 1 above inf << 1: a NaN */
    bic  v3.16b, v3.16b, v4.16b
    mov  w9, 0xffc00000
    dup  v5.4s, w9
    bit  v2.16b, v5.16b, v3.16b
.endif
.if \p == 0
    mov  v0.s[0], v2.s[0]
    mov  v2.16b, v0.16b
.endif
.endm
.macro xf_recip name, rsq, p, vb
    xf_frame_r \name
    xf_recip_body \rsq, \p
    str  q2, [x14]
    gret 1
    xf_frame_m \name, \vb
    xf_recip_body \rsq, \p
    str  q2, [x14]
    gret 4
    amd64_vread_bullshit \vb, xf_\name\()_m
.endm
xf_recip rcpps, 0, 1, 16
xf_recip rcpss, 0, 0, 4
xf_recip rsqrtps, 1, 1, 16
xf_recip rsqrtss, 1, 0, 4

# MOVMSKPS / MOVMSKPD r32, xmm: the sign bits. [xmm (0-3) | gpr (4-7)], gret 1.
.gadget amd64_xf_movmskps
    ldr  x8, [_ip]
    and  x9, x8, 0xf
    ubfx x10, x8, 4, 4
    add  x11, _cpu, CPU_xmm
    add  x9, x11, x9, lsl 4
    ldr  q1, [x9]
    ushr v2.4s, v1.4s, 31
    usra v2.2d, v2.2d, 31
    umov w12, v2.b[0]
    umov w13, v2.b[8]
    orr  w12, w12, w13, lsl 2
    add  x11, _cpu, CPU_amd64_regs
    str  x12, [x11, x10, lsl 3]
    gret 1
.gadget amd64_xf_movmskpd
    ldr  x8, [_ip]
    and  x9, x8, 0xf
    ubfx x10, x8, 4, 4
    add  x11, _cpu, CPU_xmm
    add  x9, x11, x9, lsl 4
    ldr  q1, [x9]
    ushr v2.2d, v1.2d, 63
    umov x12, v2.d[0]
    umov x13, v2.d[1]
    orr  x12, x12, x13, lsl 1
    add  x11, _cpu, CPU_amd64_regs
    str  x12, [x11, x10, lsl 3]
    gret 1

# The 64-bit halves (0F 12/13/16/17 with memory, 66 too): load into the low
# or high quadword (the other kept), or store it. MOVSLDUP/MOVSHDUP/MOVDDUP
# (F3 0F 12, F3 0F 16, F2 0F 12): register or memory sources.
# load/dup: [meta][disp][next_ip][-], gret 4 (mem) / [src | dst], gret 1 (reg)
.macro xf_half_load name, hi
.gadget amd64_xf_\name\()_m
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    ubfx x10, x8, 8, 4
    amd64_vmem_addr
    amd64_vread_prep 8, xf_\name\()_m
    ldr  d1, [x9]
    add  x14, _cpu, CPU_xmm
    add  x14, x14, x10, lsl 4
    ldr  q0, [x14]
    mov  v0.d[\hi], v1.d[0]
    str  q0, [x14]
    gret 4
    amd64_vread_bullshit 8, xf_\name\()_m
.endm
xf_half_load movlps, 0
xf_half_load movhps, 1
.macro xf_half_store name, hi
.gadget amd64_xf_\name\()_m
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    ubfx x10, x8, 8, 4
    amd64_vmem_addr
    amd64_vwrite_prep 8, xf_\name\()_m
    add  x14, _cpu, CPU_xmm
    add  x14, x14, x10, lsl 4
    ldr  x12, [x14, (8 * \hi)]
    str  x12, [x9]
    amd64_vwrite_done 8, xf_\name\()_m
    gret 4
    amd64_vwrite_bullshit 8, xf_\name\()_m
.endm
xf_half_store movlps_st, 0
xf_half_store movhps_st, 1
.macro xf_dup_body kind
.ifc \kind,sl
    trn1 v2.4s, v1.4s, v1.4s
.endif
.ifc \kind,sh
    trn2 v2.4s, v1.4s, v1.4s
.endif
.ifc \kind,dd
    dup  v2.2d, v1.d[0]
.endif
.endm
.macro xf_dup name, kind, vb
    xf_frame_r \name
    xf_dup_body \kind
    str  q2, [x14]
    gret 1
    xf_frame_m \name, \vb
    xf_dup_body \kind
    str  q2, [x14]
    gret 4
    amd64_vread_bullshit \vb, xf_\name\()_m
.endm
xf_dup movsldup, sl, 16
xf_dup movshdup, sh, 16
xf_dup movddup, dd, 8

# ---- SSE data moves (amd64_xm_*): MOVUPS/APS/UPD/APD, MOVSS/SD, MOVDQA/DQU,
# MOVD/MOVQ, LDDQU, MOVNTPS/PD. reg [src (0-3) | dst (4-7) | 64-bit (8)],
# gret 1; mem [meta][disp][next_ip], the xmm in meta 8-11, gret 3. FS/GS ride
# in meta; the alignment rule is decode_mem_meta's.
.gadget amd64_xm_mov_rr
    ldr  x8, [_ip]
    and  x9, x8, 0xf
    ubfx x10, x8, 4, 4
    add  x11, _cpu, CPU_xmm
    add  x9, x11, x9, lsl 4
    add  x10, x11, x10, lsl 4
    ldr  q0, [x9]
    str  q0, [x10]
    gret 1
.macro amd64_xm_lane_rr name, lane     /* MOVSS / MOVSD reg: the low lane only */
.gadget amd64_xm_\name
    ldr  x8, [_ip]
    and  x9, x8, 0xf
    ubfx x10, x8, 4, 4
    add  x11, _cpu, CPU_xmm
    add  x9, x11, x9, lsl 4
    add  x10, x11, x10, lsl 4
    ldr  q0, [x10]
    ldr  q1, [x9]
    mov  v0.\lane[0], v1.\lane[0]
    str  q0, [x10]
    gret 1
.endm
amd64_xm_lane_rr movss_rr, s
amd64_xm_lane_rr movsd_rr, d
.gadget amd64_xm_movq_rr                  /* the low quadword, the high one zeroed */
    ldr  x8, [_ip]
    and  x9, x8, 0xf
    ubfx x10, x8, 4, 4
    add  x11, _cpu, CPU_xmm
    add  x9, x11, x9, lsl 4
    add  x10, x11, x10, lsl 4
    ldr  d0, [x9]
    str  q0, [x10]
    gret 1
.gadget amd64_xm_from_gpr                 /* MOVD/MOVQ xmm, r32/r64: zero-extended */
    ldr  x8, [_ip]
    and  x9, x8, 0xf
    ubfx x10, x8, 4, 4
    add  x11, _cpu, CPU_amd64_regs
    ldr  x12, [x11, x9, lsl 3]
    tbnz x8, 8, 1f
    mov  w12, w12
1:  fmov d0, x12
    add  x11, _cpu, CPU_xmm
    add  x10, x11, x10, lsl 4
    str  q0, [x10]
    gret 1
.gadget amd64_xm_to_gpr                   /* MOVD/MOVQ r32/r64, xmm */
    ldr  x8, [_ip]
    and  x9, x8, 0xf
    ubfx x10, x8, 4, 4
    add  x11, _cpu, CPU_xmm
    add  x9, x11, x9, lsl 4
    ldr  x12, [x9]
    tbnz x8, 8, 1f
    mov  w12, w12
1:  add  x11, _cpu, CPU_amd64_regs
    str  x12, [x11, x10, lsl 3]
    gret 1
.macro amd64_xm_load name, vb            /* xmm <- m (16: whole; 4/8: zero-extended) */
.gadget amd64_xm_\name
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    ubfx x10, x8, 8, 4
    amd64_vmem_addr
    amd64_vread_prep \vb, xm_\name
.if \vb == 16
    ldr  q0, [x9]
.elseif \vb == 8
    ldr  d0, [x9]
.else
    ldr  s0, [x9]
.endif
    add  x14, _cpu, CPU_xmm
    add  x14, x14, x10, lsl 4
    str  q0, [x14]
    gret 3
    amd64_vread_bullshit \vb, xm_\name
.endm
amd64_xm_load load128, 16
amd64_xm_load load64z, 8
amd64_xm_load load32z, 4
.macro amd64_xm_store name, vb           /* m <- xmm's low \vb bytes */
.gadget amd64_xm_\name
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    ubfx x10, x8, 8, 4
    amd64_vmem_addr
    amd64_vwrite_prep \vb, xm_\name
    add  x14, _cpu, CPU_xmm
    add  x14, x14, x10, lsl 4
.if \vb == 16
    ldr  q0, [x14]
    str  q0, [x9]
.elseif \vb == 8
    ldr  x12, [x14]
    str  x12, [x9]
.else
    ldr  w12, [x14]
    str  w12, [x9]
.endif
    amd64_vwrite_done \vb, xm_\name
    gret 3
    amd64_vwrite_bullshit \vb, xm_\name
.endm
amd64_xm_store store128, 16
amd64_xm_store store64, 8
amd64_xm_store store32, 4






# Memory form -- the one that matters. glibc compares AGAINST memory, so mod
# 0/1 is 99.75% of real use (20199 of 20250 decodes on a gcc workload); the
# register form was 51.
#
# Operand layout is the same [meta][disp][next_ip] the other vector memory
# gadgets use, so the miss/cross-page machinery is unchanged, with imm8 packed
# into meta bits 40-47 -- nothing above bit 35 is used by AMD64_JIT_MEM_*, and
# a fourth operand word would have meant teaching the fault path a new stride.
#
# meta has to be re-read from [_ip] after the prep: amd64_vread_prep clobbers
# x8 (and x11/x12/x13), while x10 survives it. The source is pulled into q1
# before save_c -- NEON registers are untouched by it -- and spilled to a
# 16-byte stack slot, because the vec.c helpers take a pointer.
.macro amd64_v_str3a_mem name, helper
    .gadget amd64_v_\name\()_mem
        ldr  x8, [_ip]
        ldr  x9, [_ip, 8]
        ubfx x10, x8, 8, 4
        amd64_vmem_addr
        amd64_vread_prep 16, \name\()m
        ldr  q1, [x9]
        ldr  x8, [_ip]
        ubfx x11, x8, 40, 8
        save_c
        sub  sp, sp, 16
        str  q1, [sp]
        # _cpu IS x1, so every use of it must happen before x1 becomes the
        # src argument. Computing the dst pointer after `mov x1, sp` reads the
        # stack pointer as the cpu base and lands the destination in nowhere --
        # which shows up not as a crash but as pcmpistri quietly returning a
        # constant, and glibc's strcmp silently agreeing with itself.
        mov  x0, _cpu
        add  x2, _cpu, CPU_xmm
        add  x2, x2, x10, lsl 4
        mov  w3, w11
        mov  x1, sp
        bl   NAME(\helper)
        add  sp, sp, 16
        restore_c
        gret 3
        amd64_vread_bullshit 16, \name\()m
.endm

# 66 0F 38 00 pshufb xmm, xmm/m128.
#
# x86: for each byte i, dst[i] = (src[i] & 0x80) ? 0 : dst[src[i] & 0x0f].
# NEON TBL returns 0 for any index >= 16, so masking the index vector with 0x8f
# reproduces BOTH halves of that rule exactly and for free: a set high bit
# becomes 0x80-0x8f, which is out of range and zeroes; a clear one becomes 0-15,
# which indexes the table. dst is both the table and the destination.
#
# This was the last opcode still de-JITting blocks on a gcc workload, and only
# in its register form -- 21 of 21 measured decodes were mod==3, 66-prefixed.
# The memory form is here too so the opcode is closed rather than nearly closed.
.gadget amd64_v_pshufb_xmm
    ldr  x8, [_ip]
    and  x9, x8, 0xf
    ubfx x10, x8, 4, 4
    add  x14, _cpu, CPU_xmm
    add  x11, x14, x9, lsl 4
    add  x12, x14, x10, lsl 4
    ldr  q1, [x11]
    ldr  q0, [x12]
    movi v2.16b, #0x8f
    and  v1.16b, v1.16b, v2.16b
    tbl  v0.16b, {v0.16b}, v1.16b
    str  q0, [x12]
    gret 1

.gadget amd64_v_pshufb_mem
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    ubfx x10, x8, 8, 4
    amd64_vmem_addr
    amd64_vread_prep 16, pshufbm
    ldr  q1, [x9]
    add  x14, _cpu, CPU_xmm
    add  x14, x14, x10, lsl 4
    ldr  q0, [x14]
    movi v2.16b, #0x8f
    and  v1.16b, v1.16b, v2.16b
    tbl  v0.16b, {v0.16b}, v1.16b
    str  q0, [x14]
    gret 3
    amd64_vread_bullshit 16, pshufbm

# 0F 90+cc SETcc m8 -- the memory form. Same condition encoding as the register
# form in control.S; this one lives here because it needs amd64_vmem_addr and
# amd64_vwrite_prep, and amd64_do_jump was moved to gadgets.h so both files can
# reach it.
#
# The condition is evaluated FIRST: amd64_do_jump clobbers w8/w9, and those are
# exactly the meta word and the resolved host pointer. x10 carries the 0/1
# across the address resolution, which only touches x0/x8/x11-x15 (and its miss
# helpers bracket their C calls with save_c/restore_c, which preserves x10).
.macro amd64_gen_setcc_mem name, cond, pol
    .gadget amd64_set\pol\()_\name\()_mem
        amd64_do_jump \cond, 1f
        .ifc \pol,n
            mov  x10, 1
        .else
            mov  x10, 0
        .endif
        b    2f
    1:
        .ifc \pol,n
            mov  x10, 0
        .else
            mov  x10, 1
        .endif
    2:  ldr  x8, [_ip]
        ldr  x9, [_ip, 8]
        amd64_vmem_addr
        amd64_vwrite_prep 1, set\pol\()\name
        strb w10, [x9]
        amd64_vwrite_done 1, set\pol\()\name
        gret 3
        amd64_vwrite_bullshit 1, set\pol\()\name
.endm

.irp cond, o,c,z,cz,s,p,sxo,sxoz
    amd64_gen_setcc_mem \cond, \cond,
    amd64_gen_setcc_mem \cond, \cond, n
.endr

amd64_v_str3a_mem palignr,   vec_palignr128
amd64_v_str3a_mem pcmpistri, amd64_jit_pcmpistri
amd64_v_str3a_mem pcmpestri, amd64_jit_pcmpestri

# movq xmm, m64 (F3 0F 7E mem form, or 66 0F 6E with REX.W): load 8 bytes into
# the low lane of xmm[reg], zero the high lane.
# movd xmm, m32 (66 0F 6E without REX.W): load 4 bytes, zero the other 12.
# movq m64, xmm (66 0F D6 mem form, or 66 0F 7E with REX.W): store the low 8
# bytes of xmm[reg].
# movd m32, xmm (66 0F 7E without REX.W): store the low 4 bytes of xmm[reg].
# Memory-source variant of amd64_v_tbl_reg (pshufd/pshuflw/pshufhw xmm, m128,
# imm8): dst = tbl([addr], mask). Operands: [meta][disp][next_ip][mask 0-7]
# [mask 8-15] -- next_ip (which includes the imm8 byte) keeps the RIP-relative
# path in amd64_vmem_addr correct, and the mask rides after it.
.gadget amd64_v_tbl_mem
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    ubfx x10, x8, 8, 4
    amd64_vmem_addr
    amd64_vread_prep 16, tblm
    ldr  q0, [x9]
    ldr  q1, [_ip, 24]
    tbl  v0.16b, {v0.16b}, v1.16b
    add  x14, _cpu, CPU_xmm
    add  x14, x14, x10, lsl 4
    str  q0, [x14]
    gret 5
    amd64_vread_bullshit 16, tblm

# amd64 push reg (0x50+r; REX.B selects r8-r15): always a 64-bit stack push in
# long mode -- the 0x66 operand-size form is excluded by gen.c. The reg cache and
# rip are flushed before this gadget, so guest registers live in CPU_amd64_regs
# (memory) and a page fault re-executes the instruction. RSP is regs[4].
# Operand 0: source register number (0-15). `push rsp` pushes the pre-decrement
# RSP because the value is read before RSP is updated.
.gadget amd64_push_reg
    ldr  x8, [_ip]
    add  x14, _cpu, CPU_amd64_regs
    ldr  x10, [x14, x8, lsl 3]        /* x10 = value (survives prep via save_c) */
    ldr  x9, [x14, 4*8]               /* x9 = RSP (regs[4]) */
    sub  x9, x9, 8                    /* x9 = new RSP = store address */
    amd64_vwrite_prep 8, push_reg
    str  x10, [x9]
    amd64_vwrite_done 8, push_reg
    add  x14, _cpu, CPU_amd64_regs
    ldr  x8, [x14, 4*8]               /* re-read original RSP (still unmodified) */
    sub  x8, x8, 8
    str  x8, [x14, 4*8]               /* commit RSP -= 8 only after the store */
    gret 1
    amd64_vwrite_bullshit 8, push_reg

# amd64 pop reg (0x58+r; REX.B selects r8-r15): always a 64-bit stack pop.
# Operand 0: destination register number (0-15). `pop rsp` works because regs[rsp]
# is written first and then overwritten by the popped value.
.gadget amd64_pop_reg
    ldr  x8, [_ip]
    mov  x10, x8                      /* x10 = dest reg (survives prep via save_c) */
    add  x14, _cpu, CPU_amd64_regs
    ldr  x9, [x14, 4*8]               /* x9 = RSP = read address */
    amd64_vread_prep 8, pop_reg
    ldr  x11, [x9]                    /* x11 = popped value */
    add  x14, _cpu, CPU_amd64_regs
    ldr  x8, [x14, 4*8]
    add  x8, x8, 8
    str  x8, [x14, 4*8]               /* regs[rsp] = rsp + 8 */
    str  x11, [x14, x10, lsl 3]       /* regs[dest] = value (pop rsp: value wins) */
    gret 1
    amd64_vread_bullshit 8, pop_reg

# 16-bit stack pushes and pops (0x66): PUSH r16 (50+r), PUSH imm16 (68 iw /
# 6A ib), POP r16 (58+r), LEAVE (C9). RSP moves by 2; a register keeps bits
# 16-63 (so `pop %sp` keeps RSP's upper 48 bits, after the +2). RSP is
# committed only after the access. push16_reg: [reg], push16_imm: [imm],
# pop16_reg: [reg]; gret 1. leave16: gret 0.
.gadget amd64_push16_reg
    ldr  x8, [_ip]
    add  x14, _cpu, CPU_amd64_regs
    ldr  x10, [x14, x8, lsl 3]
    ldr  x9, [x14, 4*8]
    sub  x9, x9, 2
    amd64_vwrite_prep 2, push16_reg
    strh w10, [x9]
    amd64_vwrite_done 2, push16_reg
    add  x14, _cpu, CPU_amd64_regs
    ldr  x8, [x14, 4*8]
    sub  x8, x8, 2
    str  x8, [x14, 4*8]
    gret 1
    amd64_vwrite_bullshit 2, push16_reg
.gadget amd64_push16_imm
    ldr  x10, [_ip]
    add  x14, _cpu, CPU_amd64_regs
    ldr  x9, [x14, 4*8]
    sub  x9, x9, 2
    amd64_vwrite_prep 2, push16_imm
    strh w10, [x9]
    amd64_vwrite_done 2, push16_imm
    add  x14, _cpu, CPU_amd64_regs
    ldr  x8, [x14, 4*8]
    sub  x8, x8, 2
    str  x8, [x14, 4*8]
    gret 1
    amd64_vwrite_bullshit 2, push16_imm
.gadget amd64_pop16_reg
    ldr  x8, [_ip]
    mov  x10, x8
    add  x14, _cpu, CPU_amd64_regs
    ldr  x9, [x14, 4*8]
    amd64_vread_prep 2, pop16_reg
    ldrh w11, [x9]
    add  x14, _cpu, CPU_amd64_regs
    ldr  x8, [x14, 4*8]
    add  x8, x8, 2
    str  x8, [x14, 4*8]
    ldr  x12, [x14, x10, lsl 3]
    bfi  x12, x11, 0, 16
    str  x12, [x14, x10, lsl 3]
    gret 1
    amd64_vread_bullshit 2, pop16_reg
.gadget amd64_leave16
    add  x14, _cpu, CPU_amd64_regs
    ldr  x9, [x14, 5*8]               /* RBP: the new RSP, and the read address */
    amd64_vread_prep 2, leave16
    ldrh w11, [x9]
    add  x14, _cpu, CPU_amd64_regs
    ldr  x8, [x14, 5*8]
    add  x12, x8, 2
    str  x12, [x14, 4*8]              /* RSP = RBP + 2 */
    bfi  x8, x11, 0, 16
    str  x8, [x14, 5*8]               /* BP = the popped word */
    gret 0
    amd64_vread_bullshit 2, leave16

# amd64 push imm (0x68 imm32 / 0x6a imm8): push a sign-extended 64-bit immediate.
# gen.c has already sign-extended the immediate to 64 bits and emits it as
# operand 0, so the gadget just stores it. Same flush/fault contract as push_reg.
.gadget amd64_push_imm
    ldr  x10, [_ip]                   /* x10 = sign-extended 64-bit immediate */
    add  x14, _cpu, CPU_amd64_regs
    ldr  x9, [x14, 4*8]               /* x9 = RSP */
    sub  x9, x9, 8                    /* x9 = new RSP = store address */
    amd64_vwrite_prep 8, push_imm
    str  x10, [x9]
    amd64_vwrite_done 8, push_imm
    add  x14, _cpu, CPU_amd64_regs
    ldr  x8, [x14, 4*8]
    sub  x8, x8, 8
    str  x8, [x14, 4*8]
    gret 1
    amd64_vwrite_bullshit 8, push_imm

# PUSHF (9C) / POPF (9D), 64-bit and, with 0x66, 16-bit. PUSHF pushes the
# flags as collapse_flags would picture them: CF and OF from their bytes (set
# when nonzero), bit 1 set, bits 3 and 5 clear, IF set, the rest as eflags has
# them; 16-bit pushes the low word. POPF loads CF PF AF ZF SF TF DF OF NT, and
# in 64-bit AC and ID too -- what Linux lets user mode change (IF, IOPL and the
# rest stay); CF and OF go to their bytes as well. A POPF that leaves TF set
# ends the block, so the frontend single-steps from the next instruction and
# the trap comes after it, as on hardware. RSP moves only once the access
# succeeded, so a fault re-executes cleanly. PUSHF: no operands, gret 0.
# POPF: [next_ip], gret 1.
.macro amd64_pushf_value
    ldr  w10, [_cpu, CPU_eflags]
    ldrb w11, [_cpu, CPU_cf]
    ldrb w12, [_cpu, CPU_of]
    bic  w10, w10, 1
    bic  w10, w10, (1 << 11)
    cmp  w11, 0
    cset w11, ne
    orr  w10, w10, w11
    cmp  w12, 0
    cset w12, ne
    orr  w10, w10, w12, lsl 11
    orr  w10, w10, 2
    bic  w10, w10, 8
    bic  w10, w10, 32
    orr  w10, w10, (1 << 9)
    str  w10, [_cpu, CPU_eflags]
.endm
.gadget amd64_pushf64
    amd64_pushf_value
    add  x14, _cpu, CPU_amd64_regs
    ldr  x9, [x14, 4*8]
    sub  x9, x9, 8
    amd64_vwrite_prep 8, pushf64
    str  x10, [x9]
    amd64_vwrite_done 8, pushf64
    add  x14, _cpu, CPU_amd64_regs
    ldr  x8, [x14, 4*8]
    sub  x8, x8, 8
    str  x8, [x14, 4*8]
    gret 0
    amd64_vwrite_bullshit 8, pushf64
.gadget amd64_pushf16
    amd64_pushf_value
    add  x14, _cpu, CPU_amd64_regs
    ldr  x9, [x14, 4*8]
    sub  x9, x9, 2
    amd64_vwrite_prep 2, pushf16
    strh w10, [x9]
    amd64_vwrite_done 2, pushf16
    add  x14, _cpu, CPU_amd64_regs
    ldr  x8, [x14, 4*8]
    sub  x8, x8, 2
    str  x8, [x14, 4*8]
    gret 0
    amd64_vwrite_bullshit 2, pushf16
.macro amd64_gen_popf name, vbytes, writable_hi
.gadget amd64_\name
    add  x14, _cpu, CPU_amd64_regs
    ldr  x9, [x14, 4*8]
    amd64_vread_prep \vbytes, \name
.if \vbytes == 2
    ldrh w10, [x9]
.else
    ldr  x10, [x9]
.endif
    ldr  w11, [_cpu, CPU_eflags]
    mov  w12, 0x4dd5                 /* CF PF AF ZF SF TF DF OF NT */
    movk w12, \writable_hi, lsl 16   /* 64-bit: AC (18), ID (21) */
    bic  w11, w11, w12
    and  w13, w10, w12
    orr  w11, w11, w13
    str  w11, [_cpu, CPU_eflags]
    and  w13, w11, 1
    strb w13, [_cpu, CPU_cf]
    ubfx w13, w11, 11, 1
    strb w13, [_cpu, CPU_of]
    strb wzr, [_cpu, CPU_flags_res]  /* the eager bits are authoritative now */
    add  x14, _cpu, CPU_amd64_regs
    ldr  x8, [x14, 4*8]
    add  x8, x8, \vbytes
    str  x8, [x14, 4*8]
    tbz  w11, 8, 1f                  /* TF: leave the JIT (a yield), so */
    ldr  x10, [_ip]                  /* the frontend single-steps on */
    str  x10, [_cpu, CPU_amd64_rip]
    str  w10, [_cpu, CPU_eip]
    mov  w0, INT_TIMER
    b    jit_exit
1:  gret 1
    amd64_vread_bullshit \vbytes, \name
.endm
amd64_gen_popf popf64, 8, 0x24
amd64_gen_popf popf16, 2, 0

# ===========================================================================
# amd64 string instructions: MOVS STOS LODS SCAS CMPS, 1/2/4/8 bytes, with or
# without REP/REPE (F3) / REPNE (F2), the 0x67 address size (ESI/EDI/ECX, a
# 32-bit update), and an FS/GS override on the DS:RSI operand (MOVS, LODS,
# CMPS; ES:RDI cannot be overridden). DF picks the direction. Per element
# through the read/write preps, with RCX/RSI/RDI written back after each one:
# a fault leaves them as the restart state with rip at the instruction. A REP
# checks for a poke every 256 elements and leaves at the instruction, so it
# stays interruptible. Forward REP MOVS/STOS with 64-bit addressing moves a
# page span at a time instead (a MOVS whose destination overlaps the source
# ahead of it -- the byte-smearing case -- stays per element).
# The register cache is flushed before, so x20-x26 hold the loop state:
# x20 count, x21 rsi, x22 rdi, x23 step (+-size), x24 packed, x25 rax,
# x26 the poke countdown.
# Code stream: [packed][start_ip][next_ip], packed = rep (0-1: 0 none, 1 F3,
# 2 F2) | addr32 (2) | fs (3) | gs (4). gret 3.

# x9 += the source segment's base
.macro amd64_str_seg
    tbz  x24, 3, .Lss_fs_\@
    ldr  x8, [_cpu, CPU_tls_ptr]
    add  x9, x9, x8
.Lss_fs_\@:
    tbz  x24, 4, .Lss_gs_\@
    ldr  x8, [_cpu, CPU_amd64_gs_base]
    add  x9, x9, x8
.Lss_gs_\@:
.endm

.macro amd64_str_load vb, n, src
.if \vb == 1
    ldrb w\n, [\src]
.elseif \vb == 2
    ldrh w\n, [\src]
.elseif \vb == 4
    ldr  w\n, [\src]
.else
    ldr  x\n, [\src]
.endif
.endm

# CMP lhs (x13), rhs (x10), both zero-extended from \bits -> flags; x15 =
# the result.
.macro amd64_str_cmp_flags bits
.if \bits == 64
    subs x15, x13, x10
    cset w14, cc
    cset w16, vs
.else
    lsl  x4, x13, (64 - \bits)
    lsl  x5, x10, (64 - \bits)
    subs x6, x4, x5
    cset w14, cc
    cset w16, vs
    lsr  x15, x6, (64 - \bits)
.endif
    amd64_cached_set_addsub_flags (\bits - 1)
.endm

# The registers back to CPU state: RSI, RDI, and RCX under a REP; 32-bit
# under 0x67.
.macro amd64_str_store_regs
    tbz  x24, 2, .Lsr_64_\@
    mov  w20, w20
    mov  w21, w21
    mov  w22, w22
.Lsr_64_\@:
    add  x14, _cpu, CPU_amd64_regs
    str  x21, [x14, 6*8]
    str  x22, [x14, 7*8]
    tst  x24, 3
    b.eq .Lsr_norep_\@
    str  x20, [x14, 1*8]
.Lsr_norep_\@:
.endm

.macro amd64_gen_string op, vb, bits, lg
.gadget amd64_str_\op\vb
    ldr  x24, [_ip]
    add  x14, _cpu, CPU_amd64_regs
    ldr  x20, [x14, 1*8]
    ldr  x21, [x14, 6*8]
    ldr  x22, [x14, 7*8]
    ldr  x25, [x14]
    tbz  x24, 2, 1f
    mov  w20, w20
    mov  w21, w21
    mov  w22, w22
1:  ldr  w8, [_cpu, CPU_eflags]
    mov  x23, \vb
    tbz  w8, 10, 2f
    neg  x23, x23
2:  mov  x26, 256
    tst  x24, 3
    b.eq 30f                         /* no REP: one element */

.ifc \op,movs
    /* forward REP MOVS, 64-bit addressing, no source override: page spans */
    tbnz x23, 63, 20f
    tst  x24, (4 | 8 | 16)
    b.ne 20f
40: cbz  x20, 90f
    and  x8, x22, 0xfff
    mov  x9, 0x1000
    sub  x9, x9, x8                  /* bytes to the destination's page end */
    and  x8, x21, 0xfff
    mov  x10, 0x1000
    sub  x10, x10, x8
    cmp  x9, x10
    csel x9, x9, x10, lo             /* the nearer page end */
    lsr  x9, x9, \lg
    cmp  x9, x20
    csel x9, x9, x20, lo             /* x9 = whole elements in this span */
    cbz  x9, 20f                     /* an element straddles a page */
    lsl  x26, x9, \lg   /* bytes */
    cmp  x22, x21                    /* destination ahead of the source and */
    b.ls 41f                         /* inside this span: element by element */
    sub  x8, x22, x21
    cmp  x8, x26
    b.lo 20f
41: mov  x9, x21
    amd64_vread_prep 1, str_\op\vb\()_bs
    mov  x10, x9                     /* host source (x10 survives a C call) */
    mov  x9, x22
    amd64_vwrite_prep 1, str_\op\vb\()_bd
    mov  x11, x26
    bl   amd64_str_copy              /* x9 <- x10, x11 bytes */
    lsr  x8, x26, \lg
    sub  x20, x20, x8
    add  x21, x21, x26
    add  x22, x22, x26
    amd64_str_store_regs
    cbz  x20, 90f
    ldr  x8, [_cpu, CPU_poked_ptr]
    ldrb w8, [x8]
    cbnz w8, 80f
    b    40b
.endif
.ifc \op,stos
    /* forward REP STOS, 64-bit addressing: page spans */
    tbnz x23, 63, 20f
    tst  x24, 4
    b.ne 20f
    /* the pattern, the accumulator's low \vb bytes over 8 */
.if \vb == 1
    and  x8, x25, 0xff
    mov  x9, 0x0101010101010101
    mul  x8, x8, x9
.elseif \vb == 2
    and  x8, x25, 0xffff
    mov  x9, 0x0001000100010001
    mul  x8, x8, x9
.elseif \vb == 4
    mov  w8, w25
    orr  x8, x8, x8, lsl 32
.else
    mov  x8, x25
.endif
    mov  x3, x8                      /* x3 survives a C call */
40: cbz  x20, 90f
    and  x8, x22, 0xfff
    mov  x9, 0x1000
    sub  x9, x9, x8
    lsr  x9, x9, \lg
    cmp  x9, x20
    csel x9, x9, x20, lo
    cbz  x9, 20f
    lsl  x26, x9, \lg
    mov  x9, x22
    amd64_vwrite_prep 1, str_\op\vb\()_bd
    mov  x11, x26
    bl   amd64_str_fill              /* x9, x11 bytes of the pattern x3 */
    lsr  x8, x26, \lg
    sub  x20, x20, x8
    add  x22, x22, x26
    amd64_str_store_regs
    cbz  x20, 90f
    ldr  x8, [_cpu, CPU_poked_ptr]
    ldrb w8, [x8]
    cbnz w8, 80f
    b    40b
.endif

    /* REP, element by element */
20: mov  x26, 256
21: cbz  x20, 90f
30: /* one element */
.ifc \op,movs
    mov  x9, x21
    amd64_str_seg
    amd64_vread_prep \vb, str_\op\vb\()_r
    amd64_str_load \vb, 10, x9
    mov  x9, x22
    amd64_vwrite_prep \vb, str_\op\vb\()_w
.if \vb == 1
    strb w10, [x9]
.elseif \vb == 2
    strh w10, [x9]
.elseif \vb == 4
    str  w10, [x9]
.else
    str  x10, [x9]
.endif
    amd64_vwrite_done \vb, str_\op\vb\()_w
    add  x21, x21, x23
    add  x22, x22, x23
.endif
.ifc \op,stos
    mov  x9, x22
    amd64_vwrite_prep \vb, str_\op\vb\()_w
.if \vb == 1
    strb w25, [x9]
.elseif \vb == 2
    strh w25, [x9]
.elseif \vb == 4
    str  w25, [x9]
.else
    str  x25, [x9]
.endif
    amd64_vwrite_done \vb, str_\op\vb\()_w
    add  x22, x22, x23
.endif
.ifc \op,lods
    mov  x9, x21
    amd64_str_seg
    amd64_vread_prep \vb, str_\op\vb\()_r
    amd64_str_load \vb, 10, x9
.if \vb == 1
    bfi  x25, x10, 0, 8
.elseif \vb == 2
    bfi  x25, x10, 0, 16
.elseif \vb == 4
    mov  w25, w10
.else
    mov  x25, x10
.endif
    add  x14, _cpu, CPU_amd64_regs
    str  x25, [x14]
    add  x21, x21, x23
.endif
.ifc \op,scas
    mov  x9, x22
    amd64_vread_prep \vb, str_\op\vb\()_r
    amd64_str_load \vb, 10, x9
.if \vb == 8
    mov  x13, x25
.elseif \vb == 4
    mov  w13, w25
.elseif \vb == 2
    and  x13, x25, 0xffff
.else
    and  x13, x25, 0xff
.endif
    amd64_str_cmp_flags \bits
    add  x22, x22, x23
.endif
.ifc \op,cmps
    mov  x9, x21
    amd64_str_seg
    amd64_vread_prep \vb, str_\op\vb\()_r
    amd64_str_load \vb, 3, x9        /* x3: the next prep leaves it alone */
    mov  x9, x22
    amd64_vread_prep \vb, str_\op\vb\()_r2
    amd64_str_load \vb, 10, x9
    mov  x13, x3
    amd64_str_cmp_flags \bits
    add  x21, x21, x23
    add  x22, x22, x23
.endif
    tst  x24, 3
    b.eq 31f
    sub  x20, x20, 1
31: amd64_str_store_regs
    tst  x24, 3
    b.eq 90f                         /* no REP: done */
.ifin(\op, scas,cmps)
    tbnz x24, 1, 32f
    cbnz x15, 90f                    /* REPE: stop when not equal */
    b    33f
32: cbz  x15, 90f                    /* REPNE: stop when equal */
33:
.endifin
    subs x26, x26, 1
    b.ne 21b
    mov  x26, 256
    cbz  x20, 90f
    ldr  x8, [_cpu, CPU_poked_ptr]
    ldrb w8, [x8]
    cbz  w8, 21b
80: /* poked with elements left: leave at the instruction, the registers
       being its restart state */
    ldr  x8, [_ip, 8]
    str  x8, [_cpu, CPU_amd64_rip]
    str  w8, [_cpu, CPU_eip]
    b    jit_ret
90: gret 3
.ifin(\op, movs,lods,scas,cmps)
    amd64_vread_bullshit \vb, str_\op\vb\()_r
.endifin
.ifc \op,cmps
    amd64_vread_bullshit \vb, str_\op\vb\()_r2
.endif
.ifin(\op, movs,stos)
    amd64_vwrite_bullshit \vb, str_\op\vb\()_w
    amd64_vwrite_bullshit 1, str_\op\vb\()_bd
.endifin
.ifc \op,movs
    amd64_vread_bullshit 1, str_\op\vb\()_bs
.endif
.endm

# x11 bytes, host x10 -> host x9, lowest first: right for a destination at or
# below the source (each chunk is loaded before the stores that could reach
# it). Clobbers x9-x12, q0-q3.
amd64_str_copy:
1:  cmp  x11, 64
    b.lo 2f
    ldp  q0, q1, [x10]
    ldp  q2, q3, [x10, 32]
    add  x10, x10, 64
    stp  q0, q1, [x9]
    stp  q2, q3, [x9, 32]
    add  x9, x9, 64
    sub  x11, x11, 64
    b    1b
2:  cmp  x11, 8
    b.lo 3f
    ldr  x12, [x10], 8
    str  x12, [x9], 8
    sub  x11, x11, 8
    b    2b
3:  cbz  x11, 4f
    ldrb w12, [x10], 1
    strb w12, [x9], 1
    sub  x11, x11, 1
    b    3b
4:  ret

# x11 bytes at host x9 with the 8-byte pattern x3 (phase from the start).
# Clobbers x9, x11, x12, q0.
amd64_str_fill:
    dup  v0.2d, x3
1:  cmp  x11, 64
    b.lo 2f
    stp  q0, q0, [x9]
    stp  q0, q0, [x9, 32]
    add  x9, x9, 64
    sub  x11, x11, 64
    b    1b
2:  mov  x12, x3
3:  cmp  x11, 8
    b.lo 4f
    str  x12, [x9], 8
    sub  x11, x11, 8
    b    3b
4:  cbz  x11, 5f
    strb w12, [x9], 1
    lsr  x12, x12, 8
    sub  x11, x11, 1
    b    4b
5:  ret

.irp op, movs,stos,lods,scas,cmps
amd64_gen_string \op, 1, 8, 0
amd64_gen_string \op, 2, 16, 1
amd64_gen_string \op, 4, 32, 2
amd64_gen_string \op, 8, 64, 3
.endr

# CPUID (0F A2): cpuid_lookup (misc.S) on the amd64 table; the four results
# are 32-bit writes, zero-extended. [table], gret 1. XGETBV (0F 01 D0):
# EDX:EAX = XCR0 for ECX = 0, else #GP at the instruction (gen.c flushed rip
# to it). [xcr0], gret 1.
.gadget amd64_cpuid
    ldr  x10, [_ip]
    add  x14, _cpu, CPU_amd64_regs
    ldr  w8, [x14]
    ldr  w9, [x14, 1*8]
    bl   cpuid_lookup
    add  x15, _cpu, CPU_amd64_regs
    str  x11, [x15]                  /* the w writes zeroed the upper halves */
    str  x13, [x15, 1*8]
    str  x14, [x15, 2*8]
    str  x12, [x15, 3*8]
    gret 1
.gadget amd64_xgetbv
    add  x14, _cpu, CPU_amd64_regs
    ldr  w9, [x14, 1*8]
    cbnz w9, 1f
    ldr  x8, [_ip]
    mov  w10, w8
    str  x10, [x14]
    lsr  x8, x8, 32
    str  x8, [x14, 2*8]
    gret 1
1:  str  xzr, [_cpu, CPU_segfault_addr]
    strb wzr, [_cpu, CPU_segfault_was_write]
    strb wzr, [_cpu, CPU_segfault_reported]
    mov  x0, INT_GPF
    b    jit_exit

# PUSH r/m (FF /6), memory operand, 64-bit or, with 0x66, 16-bit: the operand
# is read at its address (computed before RSP moves -- `push (%rsp)` pushes
# what RSP pointed at), then pushed; RSP is committed after the store, so a
# fault on either access re-executes cleanly. [meta][disp][next_ip], gret 3.
.macro amd64_gen_push_rm_mem name, vbytes
.gadget amd64_\name
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    amd64_vmem_addr
    amd64_vread_prep \vbytes, \name\()_r
.if \vbytes == 2
    ldrh w10, [x9]
.else
    ldr  x10, [x9]
.endif
    add  x14, _cpu, CPU_amd64_regs
    ldr  x9, [x14, 4*8]
    sub  x9, x9, \vbytes
    amd64_vwrite_prep \vbytes, \name\()_w
.if \vbytes == 2
    strh w10, [x9]
.else
    str  x10, [x9]
.endif
    amd64_vwrite_done \vbytes, \name\()_w
    add  x14, _cpu, CPU_amd64_regs
    ldr  x8, [x14, 4*8]
    sub  x8, x8, \vbytes
    str  x8, [x14, 4*8]
    gret 3
    amd64_vread_bullshit \vbytes, \name\()_r
    amd64_vwrite_bullshit \vbytes, \name\()_w
.endm
amd64_gen_push_rm_mem push_rm_mem16, 2
amd64_gen_push_rm_mem push_rm_mem64, 8


# amd64 leave (0xc9): mov rsp, rbp ; pop rbp. RBP is regs[5]. The pop reads from
# [RBP] (the new RSP), then RSP = RBP + 8 and RBP = the popped value. RSP/RBP are
# committed only after the read succeeds, so a page fault re-executes cleanly.
# 64-bit only; the 0x66 (16-bit) form stays bridged. No operands.
.gadget amd64_leave
    add  x14, _cpu, CPU_amd64_regs
    ldr  x9, [x14, 5*8]               /* x9 = RBP = read address */
    amd64_vread_prep 8, leave
    ldr  x11, [x9]                    /* x11 = popped value (new RBP) */
    add  x14, _cpu, CPU_amd64_regs
    ldr  x8, [x14, 5*8]               /* re-read old RBP */
    add  x8, x8, 8                    /* new RSP = old RBP + 8 */
    str  x8, [x14, 4*8]              /* regs[rsp] = old_rbp + 8 */
    str  x11, [x14, 5*8]             /* regs[rbp] = popped value */
    gret 0
    amd64_vread_bullshit 8, leave

# A branch target must be canonical (bits 63-47 all equal). A non-canonical
# one is #GP(0) on the branch itself -- rip at the jmp/call/ret, no stack
# change -- as Linux reports it (camd; tests/manual/x86/gpf_siginfo.c); it was
# reported at the target. Every gadget below checks before committing
# anything, with rip already flushed to the instruction (jmp_indir_reg stores
# it from its own operand).
.macro amd64_branch_canonical reg
    sbfx x12, \reg, 0, 48
    cmp  x12, \reg
    b.ne amd64_branch_noncanonical
.endm

amd64_branch_noncanonical:
    str  xzr, [_cpu, CPU_segfault_addr]
    strb wzr, [_cpu, CPU_segfault_was_write]
    strb wzr, [_cpu, CPU_segfault_reported]
    mov  x0, INT_GPF
    b    jit_exit

# amd64 ret (0xc3): pop the 64-bit return address into RIP and exit the block;
# the main loop then finds/runs the target block (indirect target, so no static
# link). Saves the gadget->C->gadget round-trip the bridge paid. A page fault on
# the stack read re-executes the ret (rip flushed). RSP is committed only after
# the read succeeds. No operands; ends the block via jit_ret (no gret).
.gadget amd64_ret
    add  x14, _cpu, CPU_amd64_regs
    ldr  x9, [x14, 4*8]               /* x9 = RSP = read address */
    amd64_vread_prep 8, ret
    ldr  x10, [x9]                    /* x10 = return address (popped) */
    amd64_branch_canonical x10
    add  x14, _cpu, CPU_amd64_regs
    ldr  x8, [x14, 4*8]
    add  x8, x8, 8
    str  x8, [x14, 4*8]              /* RSP += 8 */
    str  x10, [_cpu, CPU_amd64_rip]  /* rip = target */
    str  w10, [_cpu, CPU_eip]        /* mirror low 32 bits, as amd64_jmp does */
    b    jit_ret
    amd64_vread_bullshit 8, ret

# RET imm16 (C2 iw): RET, then RSP += imm16 on top of the 8 the pop takes. The
# target is checked the way RET checks it, before RSP moves. Operand: [imm].
.gadget amd64_ret_imm
    add  x14, _cpu, CPU_amd64_regs
    ldr  x9, [x14, 4*8]
    amd64_vread_prep 8, ret_imm
    ldr  x10, [x9]
    amd64_branch_canonical x10
    add  x14, _cpu, CPU_amd64_regs
    ldr  x8, [x14, 4*8]
    ldr  x11, [_ip]
    add  x8, x8, 8
    add  x8, x8, x11
    str  x8, [x14, 4*8]
    str  x10, [_cpu, CPU_amd64_rip]
    str  w10, [_cpu, CPU_eip]
    b    jit_ret
    amd64_vread_bullshit 8, ret_imm

# POP r/m (8F /0), 64-bit or, with 0x66, 16-bit. A memory destination's
# address is computed with RSP already advanced (the SDM's rule, which `pop
# off(%rsp)` depends on), but RSP is committed only once the store has worked:
# a faulting store re-executes from the original slot -- musl's sigsetjmp does
# `popq off(%rdi)` into a jmp_buf that may not be touched yet. A register
# destination takes the value after RSP moves, so `pop %rsp` loads it.
# Memory: [meta][disp][next_ip], gret 3. Register: [reg], gret 1.
.macro amd64_gen_pop_rm_mem name, vbytes
.gadget amd64_\name
    add  x14, _cpu, CPU_amd64_regs
    ldr  x9, [x14, 4*8]
    amd64_vread_prep \vbytes, \name\()_r
.if \vbytes == 2
    ldrh w10, [x9]
.else
    ldr  x10, [x9]
.endif
    add  x14, _cpu, CPU_amd64_regs
    ldr  x3, [x14, 4*8]              /* RSP as it was */
    add  x4, x3, \vbytes
    str  x4, [x14, 4*8]              /* advanced, for the address only */
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    amd64_vmem_addr
    add  x14, _cpu, CPU_amd64_regs
    str  x3, [x14, 4*8]              /* back, until the store has worked */
    amd64_vwrite_prep \vbytes, \name\()_w
.if \vbytes == 2
    strh w10, [x9]
.else
    str  x10, [x9]
.endif
    amd64_vwrite_done \vbytes, \name\()_w
    add  x14, _cpu, CPU_amd64_regs
    ldr  x3, [x14, 4*8]
    add  x3, x3, \vbytes
    str  x3, [x14, 4*8]
    gret 3
    amd64_vread_bullshit \vbytes, \name\()_r
    amd64_vwrite_bullshit \vbytes, \name\()_w
.endm
amd64_gen_pop_rm_mem pop_rm_mem16, 2
amd64_gen_pop_rm_mem pop_rm_mem64, 8
.macro amd64_gen_pop_rm_reg name, vbytes
.gadget amd64_\name
    add  x14, _cpu, CPU_amd64_regs
    ldr  x9, [x14, 4*8]
    amd64_vread_prep \vbytes, \name
.if \vbytes == 2
    ldrh w10, [x9]
.else
    ldr  x10, [x9]
.endif
    add  x14, _cpu, CPU_amd64_regs
    ldr  x8, [x14, 4*8]
    add  x8, x8, \vbytes
    str  x8, [x14, 4*8]
    ldr  x11, [_ip]
.if \vbytes == 2
    ldr  x12, [x14, x11, lsl 3]
    bfi  x12, x10, 0, 16
    str  x12, [x14, x11, lsl 3]
.else
    str  x10, [x14, x11, lsl 3]
.endif
    gret 1
    amd64_vread_bullshit \vbytes, \name
.endm
amd64_gen_pop_rm_reg pop_rm_reg16, 2
amd64_gen_pop_rm_reg pop_rm_reg64, 8

# amd64 indirect jmp, register form (0xff /4, mod==3): rip = regs[rm], exit. No
# memory access. Operand 0 = rm register number (0-15). Always 64-bit in long mode.
.gadget amd64_jmp_indir_reg
    ldr  x8, [_ip]
    add  x14, _cpu, CPU_amd64_regs
    ldr  x10, [x14, x8, lsl 3]       /* target = regs[rm] */
    sbfx x12, x10, 0, 48
    cmp  x12, x10
    b.eq 1f
    ldr  x11, [_ip, 8]               /* the jmp's own rip (operand 1) */
    str  x11, [_cpu, CPU_amd64_rip]
    str  w11, [_cpu, CPU_eip]
    b    amd64_branch_noncanonical
1:
    str  x10, [_cpu, CPU_amd64_rip]
    str  w10, [_cpu, CPU_eip]
    b    jit_ret

# amd64 indirect call, register form (0xff /2, mod==3): push the return address
# and set rip = regs[rm]. The target is read BEFORE the push (x86 order; matters
# for `call *%rsp`). Operand 0 = rm reg number, operand 1 = next_ip (return addr).
# rip flushed so a #PF on the push re-executes the call.
.gadget amd64_call_indir_reg
    add  x14, _cpu, CPU_amd64_regs
    ldr  x8, [_ip]
    ldr  x10, [x14, x8, lsl 3]       /* x10 = target = regs[rm] (survives prep) */
    amd64_branch_canonical x10
    ldr  x9, [x14, 4*8]              /* RSP */
    sub  x9, x9, 8                   /* new RSP = store address */
    amd64_vwrite_prep 8, call_ir
    ldr  x11, [_ip, 8]               /* return address (operand 1) */
    str  x11, [x9]                   /* push return address */
    amd64_vwrite_done 8, call_ir
    add  x14, _cpu, CPU_amd64_regs
    ldr  x8, [x14, 4*8]
    sub  x8, x8, 8
    str  x8, [x14, 4*8]             /* RSP -= 8 */
    str  x10, [_cpu, CPU_amd64_rip]  /* rip = target (read before the push) */
    str  w10, [_cpu, CPU_eip]
    b    jit_ret
    amd64_vwrite_bullshit 8, call_ir

# amd64 indirect jmp, memory form (0xff /4, mod!=3): the target is the 8-byte value
# at the effective address -- this is the PLT thunk `jmp *off(%rip)` and vtable/
# function-pointer dispatch. rip = *(u64 *)addr, then exit. meta(op0)/disp(op1)
# describe the addressing mode and next_ip(op2) is the rip-relative base consumed by
# amd64_vmem_addr. rip is flushed by gen.c so a #PF on the target load re-executes.
# The FS-prefix form is excluded in gen.c (amd64_vmem_addr does not add tls_ptr).
.gadget amd64_jmp_indir_mem
    ldr  x8, [_ip]                   /* meta */
    ldr  x9, [_ip, 8]                /* disp */
    amd64_vmem_addr                  /* x9 = effective address */
    amd64_vread_prep 8, jmp_im       /* x9 = host addr of the target */
    ldr  x10, [x9]                   /* target = *(u64 *)addr */
    amd64_branch_canonical x10
    str  x10, [_cpu, CPU_amd64_rip]
    str  w10, [_cpu, CPU_eip]
    b    jit_ret
    amd64_vread_bullshit 8, jmp_im

# amd64 indirect call, memory form (0xff /2, mod!=3): read the 8-byte target from the
# effective address FIRST (x86 order), then push the return address and set rip. Two
# faulting accesses -- the target load (id call_im) and the push (id call_im_w). x10
# holds the target across the push's vwrite_prep (preserved by save_c). RSP is
# committed only after the push store and rip is flushed by gen.c, so a #PF on either
# access cleanly re-executes the whole instruction. FS-prefix form excluded in gen.c.
.gadget amd64_call_indir_mem
    ldr  x8, [_ip]                   /* meta */
    ldr  x9, [_ip, 8]                /* disp */
    amd64_vmem_addr                  /* x9 = effective address */
    amd64_vread_prep 8, call_im      /* x9 = host addr of the target */
    ldr  x10, [x9]                   /* x10 = target (survives the push prep) */
    amd64_branch_canonical x10
    add  x14, _cpu, CPU_amd64_regs
    ldr  x9, [x14, 4*8]              /* RSP */
    sub  x9, x9, 8                   /* new RSP = push store address */
    amd64_vwrite_prep 8, call_im_w
    ldr  x11, [_ip, 16]              /* return address (operand 2 = next_ip) */
    str  x11, [x9]                   /* push return address */
    amd64_vwrite_done 8, call_im_w
    add  x14, _cpu, CPU_amd64_regs
    ldr  x8, [x14, 4*8]
    sub  x8, x8, 8
    str  x8, [x14, 4*8]             /* RSP -= 8 (commit after the store) */
    str  x10, [_cpu, CPU_amd64_rip]  /* rip = target (read before the push) */
    str  w10, [_cpu, CPU_eip]
    b    jit_ret
    amd64_vread_bullshit 8, call_im
    amd64_vwrite_bullshit 8, call_im_w

# amd64 MOV reg<-[mem] (0x8b), mod!=3: load \vbytes from the effective address into
# the modrm.reg register with amd64 size semantics (32-bit zero-extends the upper 32,
# 16-bit merges the low 16 and preserves the rest). The reg cache is flushed by gen.c,
# so base/index come from CPU_amd64_regs and the destination is written there too (a
# #PF re-executes via the flushed rip). meta(op0)/disp(op1) describe the addressing
# mode; next_ip(op2) is amd64_vmem_addr's rip base. A per-size gadget keeps the
# vread_prep size literal (correct cross-page staging). Byte (0x8a) keeps bridging --
# its modrm.reg encodes AH/CH/DH/BH without REX (amd64_reg_get_encoded8).
.macro amd64_gen_mov_load name, vbytes
.gadget amd64_\name
    ldr  x8, [_ip]                   /* meta */
    ldr  x9, [_ip, 8]                /* disp */
    ubfx x10, x8, 8, 4               /* dest reg id (survives prep via save_c) */
    amd64_vmem_addr                  /* x9 = effective address */
    amd64_vread_prep \vbytes, \name  /* x9 = host addr of the source */
    add  x13, _cpu, CPU_amd64_regs
    add  x13, x13, x10, lsl 3        /* &regs[dest] */
.if \vbytes == 2
    ldrh w10, [x9]
    strh w10, [x13]                  /* merge low 16, preserve upper */
.elseif \vbytes == 4
    ldr  w10, [x9]
    str  x10, [x13]                  /* 32-bit load zero-extends; full slot store */
.else
    ldr  x10, [x9]
    str  x10, [x13]
.endif
    gret 3
    amd64_vread_bullshit \vbytes, \name
.endm

# amd64 MOV [mem]<-reg (0x89), mod!=3: store the low \vbytes of the modrm.reg register
# to the effective address. Reg cache flushed by gen.c (source read from
# CPU_amd64_regs). The source value lives in x10 across vwrite_prep (only x10 survives
# a prep slow-path bl); rip flushed so a #PF on the store re-executes. Byte (0x88)
# keeps bridging (AH/CH/DH/BH encoding).
.macro amd64_gen_mov_store name, vbytes
.gadget amd64_\name
    ldr  x8, [_ip]                   /* meta */
    ldr  x9, [_ip, 8]                /* disp */
    ubfx x10, x8, 8, 4               /* source reg id */
    amd64_vmem_addr                  /* x9 = effective address */
    add  x11, _cpu, CPU_amd64_regs
    ldr  x10, [x11, x10, lsl 3]      /* x10 = source value (survives prep) */
    amd64_vwrite_prep \vbytes, \name /* x9 = host write addr */
.if \vbytes == 2
    strh w10, [x9]
.elseif \vbytes == 4
    str  w10, [x9]
.else
    str  x10, [x9]
.endif
    amd64_vwrite_done \vbytes, \name
    gret 3
    amd64_vwrite_bullshit \vbytes, \name
.endm

amd64_gen_mov_load  mov_load16,  2
amd64_gen_mov_load  mov_load32,  4
amd64_gen_mov_load  mov_load64,  8
amd64_gen_mov_store mov_store16, 2
amd64_gen_mov_store mov_store32, 4
amd64_gen_mov_store mov_store64, 8

# amd64 MOV reg8<-[mem] (0x8a), mod!=3: load one byte into the modrm.reg byte
# register. Byte registers alias: without REX, reg 4-7 are AH/CH/DH/BH = the HIGH
# byte (bits 8-15) of regs[reg-4]; otherwise the low byte of regs[reg]. Computing a
# BYTE POINTER up front (&regs[reg], or &regs[reg-4]+1 for the high-byte case) makes
# a single strb handle both. The byte ptr is held in x10 across the prep (only x10
# survives a slow-path bl). meta bit34 = REX_PRESENT. Cache+rip flushed by gen.c.
.gadget amd64_mov_load8
    ldr  x8, [_ip]                   /* meta */
    ldr  x9, [_ip, 8]                /* disp */
    ubfx x12, x8, 8, 4               /* reg id */
    add  x10, _cpu, CPU_amd64_regs
    add  x10, x10, x12, lsl 3        /* &regs[reg] (low-byte address) */
    tbnz x8, 34, 1f                  /* REX present -> low byte */
    cmp  x12, 4
    b.lo 1f                          /* reg 0-3 -> low byte */
    sub  x10, x10, 4*8               /* reg 4-7, no REX: &regs[reg-4]... */
    add  x10, x10, 1                 /* ...+1 = AH/CH/DH/BH high byte */
1:
    amd64_vmem_addr                  /* x9 = effective address */
    amd64_vread_prep 1, mov_load8    /* x9 = host addr of the source byte */
    ldrb w11, [x9]
    strb w11, [x10]                  /* write the byte (low or high), preserve rest */
    gret 3
    amd64_vread_bullshit 1, mov_load8

# amd64 MOV [mem]<-reg8 (0x88), mod!=3: store one byte from the modrm.reg byte
# register (AH/CH/DH/BH high-byte aliasing as above). The source byte is read through
# the same byte pointer into x10 (survives the vwrite_prep), then stored.
.gadget amd64_mov_store8
    ldr  x8, [_ip]                   /* meta */
    ldr  x9, [_ip, 8]                /* disp */
    ubfx x12, x8, 8, 4               /* reg id */
    add  x11, _cpu, CPU_amd64_regs
    add  x11, x11, x12, lsl 3        /* &regs[reg] (low-byte address) */
    tbnz x8, 34, 1f
    cmp  x12, 4
    b.lo 1f
    sub  x11, x11, 4*8
    add  x11, x11, 1                 /* AH/CH/DH/BH high byte */
1:
    ldrb w10, [x11]                  /* x10 = source byte (survives prep) */
    amd64_vmem_addr                  /* x9 = effective address */
    amd64_vwrite_prep 1, mov_store8  /* x9 = host write addr */
    strb w10, [x9]
    amd64_vwrite_done 1, mov_store8
    gret 3
    amd64_vwrite_bullshit 1, mov_store8

# amd64 load-op arith: reg <op>= [mem] (d=1 forms ADD 0x03 / SUB 0x2b / CMP 0x3b),
# mod!=3, sizes 32/64. The reg operand (modrm.reg) is the destination/lhs and the
# memory operand is the rhs. Mirrors gadget_amd64_cached_arith_reg_reg but reads the
# rhs from memory and runs in flush style (cache flushed by gen.c -> regs in memory,
# #PF re-executes). Sets CF/OF/AF/ZF/SF/PF eagerly via amd64_cached_set_addsub_flags
# (lhs=x13, rhs=x10, result=x15, cf=x14, of=x16; the macro touches x12/x17 only, so
# opcode (x9) and dst id (x8) survive it). CMP (opcode>=0x38) sets flags but does not
# write the destination. Byte forms (0x02/2a/3a) and ADC/SBB bridge (parity with the
# reg-reg native path); 16-bit (0x66) bridges. opcode|regid packed in x10 survives
# the prep (only x10 does). Operands: meta(0)/disp(1)/next_ip(2).
.macro amd64_gen_loadop_arith name, vbytes, sbit
.gadget amd64_\name
    ldr  x8, [_ip]                   /* meta */
    ldr  x9, [_ip, 8]                /* disp */
    and  x10, x8, 0xfff              /* opcode|regid<<8 (survives prep) */
    amd64_vmem_addr                  /* x9 = effective address */
    amd64_vread_prep \vbytes, \name  /* x9 = host addr of the rhs */
.if \vbytes == 4
    ldr  w11, [x9]                   /* rhs = [mem] (32-bit) */
.else
    ldr  x11, [x9]                   /* rhs = [mem] (64-bit) */
.endif
    and  x9, x10, 0xff               /* x9 = opcode */
    ubfx x8, x10, 8, 4               /* x8 = dst reg id */
    add  x13, _cpu, CPU_amd64_regs
    ldr  x13, [x13, x8, lsl 3]       /* x13 = lhs = dst reg value */
    mov  x10, x11                    /* x10 = rhs */
.if \vbytes == 4
    mov  w13, w13                    /* truncate lhs to 32 */
    mov  w10, w10                    /* truncate rhs to 32 */
    cmp  x9, 0x28
    b.hs 1f
    adds w15, w13, w10
    cset w14, cs
    cset w16, vs
    b    2f
1:  subs w15, w13, w10
    cset w14, cc
    cset w16, vs
2:  mov  w15, w15                    /* zero-extend 32-bit result */
.else
    cmp  x9, 0x28
    b.hs 1f
    adds x15, x13, x10
    cset w14, cs
    cset w16, vs
    b    2f
1:  subs x15, x13, x10
    cset w14, cc
    cset w16, vs
2:
.endif
    amd64_cached_set_addsub_flags \sbit
    cmp  x9, 0x38                    /* CMP (>=0x38): flags only, no write */
    b.hs 3f
    add  x13, _cpu, CPU_amd64_regs
.if \vbytes == 4
    ubfx x12, x15, 0, 32             /* 32-bit write zero-extends the upper 32 */
    str  x12, [x13, x8, lsl 3]
.else
    str  x15, [x13, x8, lsl 3]
.endif
3:  gret 3
    amd64_vread_bullshit \vbytes, \name
.endm

amd64_gen_loadop_arith loadop_arith32, 4, 31
amd64_gen_loadop_arith loadop_arith64, 8, 63
.set AMD64_NF, 1
amd64_gen_loadop_arith loadop_arith32_nf, 4, 31
amd64_gen_loadop_arith loadop_arith64_nf, 8, 63
.set AMD64_NF, 0

# amd64 load-op logic: reg <op>= [mem] (d=1 forms OR 0x0b / AND 0x23 / XOR 0x33),
# mod!=3, sizes 32/64. reg is dst/lhs, [mem] is rhs. Like loadop_arith but with the
# logic flag rule (CF=OF=0, AF undefined, ZF/SF/PF from the result) via
# amd64_cached_set_logic_flags (it reads the result in x15 and the size in a reg).
# Byte forms (0x0a/22/32) + TEST + 16-bit bridge. Operands: meta(0)/disp(1)/next_ip(2).
.macro amd64_gen_loadop_logic name, vbytes, ssize
.gadget amd64_\name
    ldr  x8, [_ip]                   /* meta */
    ldr  x9, [_ip, 8]                /* disp */
    and  x10, x8, 0xfff              /* opcode|regid<<8 (survives prep) */
    amd64_vmem_addr                  /* x9 = effective address */
    amd64_vread_prep \vbytes, \name  /* x9 = host addr of the rhs */
.if \vbytes == 4
    ldr  w11, [x9]                   /* rhs = [mem] (32-bit) */
.else
    ldr  x11, [x9]                   /* rhs = [mem] (64-bit) */
.endif
    and  x9, x10, 0xff               /* x9 = opcode */
    ubfx x8, x10, 8, 4               /* x8 = dst reg id */
    add  x13, _cpu, CPU_amd64_regs
    ldr  x13, [x13, x8, lsl 3]       /* x13 = lhs = dst reg value */
    cmp  x9, 0x23
    b.eq 1f                          /* AND */
    cmp  x9, 0x33
    b.eq 2f                          /* XOR */
    orr  x15, x13, x11               /* OR (0x0b) */
    b    3f
1:  and  x15, x13, x11
    b    3f
2:  eor  x15, x13, x11
3:
.if \vbytes == 4
    mov  w15, w15                    /* zero-extend 32-bit result */
.endif
    mov  x11, \ssize                 /* size in a reg for the flag macro */
    amd64_cached_set_logic_flags x15, x11
    add  x13, _cpu, CPU_amd64_regs
.if \vbytes == 4
    ubfx x12, x15, 0, 32
    str  x12, [x13, x8, lsl 3]
.else
    str  x15, [x13, x8, lsl 3]
.endif
    gret 3
    amd64_vread_bullshit \vbytes, \name
.endm

amd64_gen_loadop_logic loadop_logic32, 4, 32
amd64_gen_loadop_logic loadop_logic64, 8, 64

# amd64 op-store/RMW arith: [mem] <op>= reg (d=0 forms ADD 0x01 / SUB 0x29 / CMP
# 0x39), mod!=3, sizes 32/64. [mem] is BOTH source (lhs) and destination; reg
# (modrm.reg) is the rhs. Read-modify-write: vread the old [mem], compute + set flags
# eagerly, then (unless CMP) vwrite the result back. Two preps on the same address,
# so the guest addr lives in x0 across both (x0 is saved by save_c and untouched by
# the prep fast paths) and the result moves to x10 before the write (x15 is NOT saved
# across a prep). Flags are set before the store, matching the bridge: a #PF on the
# store re-executes the whole insn (the re-read is idempotent). CMP (0x39) sets flags
# only. Byte (0x00/28/38), ADC/SBB, 16-bit, locked forms bridge. opcode|regid packed
# in x10 survives the read prep. Operands: meta(0)/disp(1)/next_ip(2).
.macro amd64_gen_opstore_arith name, vbytes, sbit
.gadget amd64_\name
    ldr  x8, [_ip]                   /* meta */
    ldr  x9, [_ip, 8]                /* disp */
    and  x10, x8, 0xfff              /* opcode|regid<<8 (survives read prep) */
    amd64_vmem_addr                  /* x9 = guest effective address */
    mov  x0, x9                      /* x0 = guest addr (survives both preps) */
    amd64_vread_prep \vbytes, \name\()_r   /* x9 = host read addr */
.if \vbytes == 4
    ldr  w13, [x9]                   /* lhs = [mem] (old, 32-bit) */
.else
    ldr  x13, [x9]                   /* lhs = [mem] (old, 64-bit) */
.endif
    and  x9, x10, 0xff               /* x9 = opcode */
    ubfx x12, x10, 8, 4              /* x12 = reg id (rhs source) */
    add  x11, _cpu, CPU_amd64_regs
    ldr  x10, [x11, x12, lsl 3]      /* x10 = rhs = reg value */
.if \vbytes == 4
    mov  w13, w13
    mov  w10, w10
    cmp  x9, 0x28
    b.hs 1f
    adds w15, w13, w10
    cset w14, cs
    cset w16, vs
    b    2f
1:  subs w15, w13, w10
    cset w14, cc
    cset w16, vs
2:  mov  w15, w15
.else
    cmp  x9, 0x28
    b.hs 1f
    adds x15, x13, x10
    cset w14, cs
    cset w16, vs
    b    2f
1:  subs x15, x13, x10
    cset w14, cc
    cset w16, vs
2:
.endif
    amd64_cached_set_addsub_flags \sbit
    cmp  x9, 0x38                    /* CMP (>=0x38): flags only, no store */
    b.hs 3f
    mov  x10, x15                    /* result -> x10 (survives the write prep) */
    mov  x9, x0                      /* guest addr -> x9 */
    amd64_vwrite_prep \vbytes, \name\()_w  /* x9 = host write addr */
.if \vbytes == 4
    str  w10, [x9]
.else
    str  x10, [x9]
.endif
    amd64_vwrite_done \vbytes, \name\()_w
3:  gret 3
    amd64_vread_bullshit \vbytes, \name\()_r
    amd64_vwrite_bullshit \vbytes, \name\()_w
.endm

amd64_gen_opstore_arith opstore_arith32, 4, 31
amd64_gen_opstore_arith opstore_arith64, 8, 63
.set AMD64_NF, 1
amd64_gen_opstore_arith opstore_arith32_nf, 4, 31
amd64_gen_opstore_arith opstore_arith64_nf, 8, 63
.set AMD64_NF, 0

# amd64 op-store/RMW logic + TEST: [mem] <op>= reg for OR (0x09), AND (0x21), XOR
# (0x31), and TEST (0x85 = [mem] AND reg, flags only, NO write), mod!=3, sizes 32/64.
# Same RMW frame as opstore_arith (guest addr in x0 across both preps; result -> x10
# before the store) with the logic flag rule via amd64_cached_set_logic_flags. OR/AND/
# XOR write the result back; TEST computes the AND for flags and skips the store.
# Byte forms (0x08/20/30/84), 16-bit, locked bridge. Operands: meta(0)/disp(1)/next(2).
.macro amd64_gen_opstore_logic name, vbytes, ssize
.gadget amd64_\name
    ldr  x8, [_ip]                   /* meta */
    ldr  x9, [_ip, 8]                /* disp */
    and  x10, x8, 0xfff              /* opcode|regid<<8 (survives read prep) */
    amd64_vmem_addr                  /* x9 = guest effective address */
    mov  x0, x9                      /* x0 = guest addr (survives both preps) */
    amd64_vread_prep \vbytes, \name\()_r   /* x9 = host read addr */
.if \vbytes == 4
    ldr  w13, [x9]                   /* lhs = [mem] (32-bit) */
.else
    ldr  x13, [x9]                   /* lhs = [mem] (64-bit) */
.endif
    and  x9, x10, 0xff               /* x9 = opcode */
    ubfx x12, x10, 8, 4              /* x12 = reg id (rhs) */
    add  x11, _cpu, CPU_amd64_regs
    ldr  x10, [x11, x12, lsl 3]      /* x10 = rhs = reg value */
    cmp  x9, 0x21
    b.eq 1f                          /* AND (0x21) */
    cmp  x9, 0x85
    b.eq 1f                          /* TEST (0x85) = AND */
    cmp  x9, 0x31
    b.eq 2f                          /* XOR (0x31) */
    orr  x15, x13, x10               /* OR (0x09) */
    b    3f
1:  and  x15, x13, x10
    b    3f
2:  eor  x15, x13, x10
3:
.if \vbytes == 4
    mov  w15, w15                    /* zero-extend 32-bit result */
.endif
    mov  x11, \ssize
    amd64_cached_set_logic_flags x15, x11
    cmp  x9, 0x85                    /* TEST: flags only, no store */
    b.eq 4f
    mov  x10, x15                    /* result -> x10 (survives the write prep) */
    mov  x9, x0                      /* guest addr -> x9 */
    amd64_vwrite_prep \vbytes, \name\()_w
.if \vbytes == 4
    str  w10, [x9]
.else
    str  x10, [x9]
.endif
    amd64_vwrite_done \vbytes, \name\()_w
4:  gret 3
    amd64_vread_bullshit \vbytes, \name\()_r
    amd64_vwrite_bullshit \vbytes, \name\()_w
.endm

amd64_gen_opstore_logic opstore_logic32, 4, 32
amd64_gen_opstore_logic opstore_logic64, 8, 64

# amd64 INC/DEC [mem] (0xff /0 = inc, /1 = dec), mod!=3, sizes 32/64. Read-modify-write
# on the SAME address (guest addr in x0 across both preps; result -> x10 before the
# store), exactly the opstore_arith frame. INC = lhs+1, DEC = lhs-1. Flags match the
# interp (amd64_jit_ff_group case 0/1): set the full add/sub flags for rhs=1, then
# RESTORE CF -- inc/dec preserve CF. CF is eager (CPU_cf byte + eflags bit0), so we
# snapshot it after the read (w9) and rewrite both after the flag macro. OF/AF/ZF/SF/PF
# come from adds/subs at native width (32/64 -> V is correct); the computed CF in x14 is
# discarded by the restore. is_dec = the reg-field low bit (meta bit 8); gen.c gates the
# group to 0/1 with !rex.r so the reg field is exactly the group. Byte (0xfe) and 16-bit
# (0x66) bridge -- their OF needs a
# sub-32 width-correct overflow that adds/subs don't give. Operands: meta(0)/disp(1)/
# next_ip(2).
.macro amd64_gen_incdec_mem name, vbytes, sbit
.gadget amd64_\name
    ldr  x8, [_ip]                   /* meta (opcode bits0-7, reg bits8-11) */
    ldr  x9, [_ip, 8]                /* disp */
    ubfx x10, x8, 8, 1               /* x10 = is_dec = reg low bit (survives read prep) */
    amd64_vmem_addr                  /* x9 = guest effective address */
    mov  x0, x9                      /* x0 = guest addr (survives both preps) */
    amd64_vread_prep \vbytes, \name\()_r   /* x9 = host read addr */
.if \vbytes == 4
    ldr  w13, [x9]                   /* lhs = [mem] (old, 32-bit) */
.else
    ldr  x13, [x9]                   /* lhs = [mem] (old, 64-bit) */
.endif
    ldrb w9, [_cpu, CPU_cf]          /* snapshot CF (eager) before the flag macro */
.if \vbytes == 4
    mov  w13, w13                    /* truncate lhs to 32 */
    cbnz w10, 1f
    adds w15, w13, 1                 /* INC: lhs + 1 */
    cset w14, cs
    cset w16, vs
    b    2f
1:  subs w15, w13, 1                 /* DEC: lhs - 1 */
    cset w14, cc
    cset w16, vs
2:  mov  w15, w15                    /* zero-extend 32-bit result */
.else
    cbnz x10, 1f
    adds x15, x13, 1                 /* INC: lhs + 1 */
    cset w14, cs
    cset w16, vs
    b    2f
1:  subs x15, x13, 1                 /* DEC: lhs - 1 */
    cset w14, cc
    cset w16, vs
2:
.endif
    mov  x10, 1                      /* rhs = 1 (AF: lhs ^ 1 ^ result, bit 4) */
    amd64_cached_set_addsub_flags \sbit
    strb w9, [_cpu, CPU_cf]          /* restore CF: inc/dec preserve it */
    ldr  w17, [_cpu, CPU_eflags]
    bic  w17, w17, 1
    orr  w17, w17, w9
    str  w17, [_cpu, CPU_eflags]
    mov  x10, x15                    /* result -> x10 (survives the write prep) */
    mov  x9, x0                      /* guest addr -> x9 */
    amd64_vwrite_prep \vbytes, \name\()_w  /* x9 = host write addr */
.if \vbytes == 4
    str  w10, [x9]
.else
    str  x10, [x9]
.endif
    amd64_vwrite_done \vbytes, \name\()_w
    gret 3
    amd64_vread_bullshit \vbytes, \name\()_r
    amd64_vwrite_bullshit \vbytes, \name\()_w
.endm

amd64_gen_incdec_mem incdec_mem32, 4, 31
amd64_gen_incdec_mem incdec_mem64, 8, 63

# amd64 INC (FF /0) / DEC (FF /1) on a REGISTER operand (mod==3) -- the form the
# memory gadgets above could not take, and the one that actually runs hot: in long
# mode the one-byte 0x40+r encodings are the REX prefixes, so EVERY `incq %rax` is
# FF /0 mod==3 (musl's __init_libc basename loop is the documented example).
# Previously this bridged to amd64_jit_ff_group, which cost three things per
# execution: a gadget_amd64_set_rip dispatch (the helper re-decodes at
# CPU_amd64_rip, so the bridge emitter must flush the deferred rip), the bridge
# dispatch, and a C helper that re-walked prefixes and re-decoded the modrm the
# translator had already decoded. This gadget removes all three.
#
# Register-only: it cannot fault, which is why the emitter may DEFER the rip
# instead of publishing it (the whole set_rip saving) -- there is no re-execution
# path that needs an accurate rip mid-instruction.
#
# The reg id is a runtime operand word rather than a table index, using the same
# hybrid accessors as amd64_hybrid_mov_reg_reg: ids 0-7 hit the x20-x27 register
# cache, 8-15 fall through to CPU_amd64_regs. One gadget per size covers all 16
# registers, so no 2-D table (16 regs x REX would be 256+ entries).
#
# Flags are the incdec_mem block verbatim, including the part that is the classic
# emulator bug: INC/DEC set OF/SF/ZF/AF/PF but leave CF ALONE. CF is eager here
# (the authoritative CPU_cf byte plus eflags bit 0, per the clc/stc gadgets), so
# it is snapshotted before the add/sub flag macro and both halves are restored
# after -- missing either half leaves a `stc; incq; jc` reading the wrong carry.
#   operand 0: packed -- rm id bits 0-3, is_dec bit 8
.macro amd64_gen_incdec_reg name, sbit, size
.gadget amd64_\name
    ldr  x8, [_ip]                   /* packed operand */
    and  x9, x8, 0xf                 /* x9 = reg id (0-15), must survive to the write */
    ubfx x10, x8, 8, 1               /* x10 = is_dec (x10 is reused as rhs below) */
    amd64_read_low_or_mem x9, x13    /* x13 = lhs (flag macro reads it) */
    ldrb w11, [_cpu, CPU_cf]         /* snapshot CF before the flag macro rewrites it */
.if \size == 32
    mov  w13, w13                    /* truncate lhs to 32 */
    cbnz w10, 1f
    adds w15, w13, 1                 /* INC: lhs + 1 */
    cset w14, cs
    cset w16, vs
    b    2f
1:  subs w15, w13, 1                 /* DEC: lhs - 1 */
    cset w14, cc
    cset w16, vs
2:  mov  w15, w15                    /* zero-extend the 32-bit result */
.else
    cbnz x10, 1f
    adds x15, x13, 1                 /* INC: lhs + 1 */
    cset w14, cs
    cset w16, vs
    b    2f
1:  subs x15, x13, 1                 /* DEC: lhs - 1 */
    cset w14, cc
    cset w16, vs
2:
.endif
    mov  x10, 1                      /* rhs = 1 (AF: lhs ^ 1 ^ result, bit 4) */
    amd64_cached_set_addsub_flags \sbit
.if AMD64_NF == 0
    strb w11, [_cpu, CPU_cf]         /* restore CF byte: inc/dec preserve it */
    ldr  w17, [_cpu, CPU_eflags]
    bic  w17, w17, 1
    orr  w17, w17, w11               /* and restore eflags bit 0 (the pushfq image) */
    str  w17, [_cpu, CPU_eflags]
.endif
    mov  x11, \size                  /* the hybrid write takes its size in a register */
    amd64_write_low_or_mem x9, x15, x11
    gret 1
.endm

amd64_gen_incdec_reg incdec_reg32, 31, 32
amd64_gen_incdec_reg incdec_reg64, 63, 64
.set AMD64_NF, 1
amd64_gen_incdec_reg incdec_reg32_nf, 31, 32
amd64_gen_incdec_reg incdec_reg64_nf, 63, 64
.set AMD64_NF, 0

# amd64 carry-flag ops: CLC (0xf8), STC (0xf9), CMC (0xf5). One byte, no operand, no
# memory, no register -- they ONLY touch CF (interp 0xf8/f9/f5: cpu->cf + cpu->cf_bit).
# CF is eager: the byte CPU_cf (authoritative, read by the CF macro / adc/sbb gadgets)
# plus eflags bit0 (cf_bit, the pushfq image). Each gadget rewrites both and leaves
# OF/ZF/SF/PF/AF untouched. Cached-style (no reg-cache flush) -- they never fault and
# never read/write GPRs, so the x20-x27 cache stays live across them. x16/x17 are
# inter-gadget scratch. No operands -> gret 0. Previously bridged: stc/clc/cmc forced an
# interp fallback for the rest of the block, hurting bignum/multi-precision asm
# (stc;...;adc chains).
.gadget amd64_clc
    strb wzr, [_cpu, CPU_cf]         /* CF byte = 0 */
    ldr  w17, [_cpu, CPU_eflags]
    bic  w17, w17, 1                 /* eflags bit0 = 0 */
    str  w17, [_cpu, CPU_eflags]
    gret 0

.gadget amd64_stc
    mov  w16, 1
    strb w16, [_cpu, CPU_cf]         /* CF byte = 1 */
    ldr  w17, [_cpu, CPU_eflags]
    orr  w17, w17, 1                 /* eflags bit0 = 1 */
    str  w17, [_cpu, CPU_eflags]
    gret 0

.gadget amd64_cmc
    ldrb w16, [_cpu, CPU_cf]
    eor  w16, w16, 1                 /* CF = !CF */
    strb w16, [_cpu, CPU_cf]
    ldr  w17, [_cpu, CPU_eflags]
    bic  w17, w17, 1
    orr  w17, w17, w16               /* eflags bit0 = new CF */
    str  w17, [_cpu, CPU_eflags]
    gret 0

# amd64 MOVSXD reg64 <- [mem]32 (movslq, 0x63 with REX.W), mod!=3: sign-extend a
# 32-bit memory operand into a 64-bit register. No flags. gen.c only routes the
# REX.W form here (the rare non-W 0x63 bridges). dst reg id in x10 survives the prep;
# ldrsw does the 32->64 sign extend. Operands: meta(0)/disp(1)/next_ip(2).
.gadget amd64_movsxd_mem
    ldr  x8, [_ip]                   /* meta */
    ldr  x9, [_ip, 8]                /* disp */
    ubfx x10, x8, 8, 4               /* dst reg id (survives prep) */
    amd64_vmem_addr                  /* x9 = effective address */
    amd64_vread_prep 4, movsxd_mem   /* x9 = host addr of the 32-bit source */
    ldrsw x11, [x9]                  /* sign-extend 32 -> 64 */
    add  x13, _cpu, CPU_amd64_regs
    str  x11, [x13, x10, lsl 3]      /* write the full 64-bit register */
    gret 3
    amd64_vread_bullshit 4, movsxd_mem

# amd64 MOVZX/MOVSX reg <- [mem]: 0F B6 (movzbl/q), B7 (movzwl/q), BE (movsbl/q),
# BF (movswl/q), mod!=3, dst 32/64. Zero- (B6/B7) or sign-extend (BE/BF) a 1- or
# 2-byte memory operand into the dst register. NO FLAGS. Flush style (covers r8-r15
# dst). gen.c packs into meta: dst reg id at bits 8-11 (as usual), is_signed at bit 40,
# dst64 (REX.W) at bit 41 -- free high bits (vmem_addr/decode_mem_meta only use 0-34).
# dst regid + the 2 flag bits are folded into x10 to survive the read prep (only x10/x0
# do). movzx is just ldrb/ldrh (already zero-extends to 64, correct for both dst sizes
# since a 32-bit x86 write zero-extends). movsx: sxt to W (movs_l) zeroes the upper 32
# = the x86 32-bit-write rule; sxt to X (movs_q, REX.W) sign-extends to 64.
# Operands: meta(0)/disp(1)/next_ip(2).
.macro amd64_gen_movx_mem name, vb
.gadget amd64_\name
    ldr  x8, [_ip]                   /* meta */
    ldr  x9, [_ip, 8]                /* disp */
    ubfx x10, x8, 8, 4               /* dst reg id */
    ubfx x11, x8, 40, 3              /* signed (bit0) | dst64 (bit1) | dst16 (bit2) */
    orr  x10, x10, x11, lsl 8        /* pack regid | flags<<8 (survives prep) */
    amd64_vmem_addr                  /* x9 = effective address */
    amd64_vread_prep \vb, \name      /* x9 = host addr of the source */
.if \vb == 1
    ldrb w11, [x9]                   /* zero-extended byte */
.else
    ldrh w11, [x9]                   /* zero-extended half */
.endif
    ubfx x12, x10, 8, 3              /* flags */
    and  x10, x10, 0xf               /* dst reg id */
    tbz  x12, 0, 9f                  /* unsigned (movzx): already zero-extended */
    tbnz x12, 1, 8f                  /* signed + dst64 */
.if \vb == 1
    sxtb w11, w11                    /* movsbl: sign-extend byte -> 32 (zeroes upper 32) */
.else
    sxth w11, w11                    /* movswl */
.endif
    b    9f
8:
.if \vb == 1
    sxtb x11, w11                    /* movsbq: sign-extend byte -> 64 */
.else
    sxth x11, w11                    /* movswq */
.endif
9:  add  x13, _cpu, CPU_amd64_regs
    tbz  x12, 2, 10f
    ldr  x14, [x13, x10, lsl 3]      /* 16-bit dst (0x66): the low word only */
    bfi  x14, x11, 0, 16
    mov  x11, x14
10: str  x11, [x13, x10, lsl 3]      /* write dst (full 64-bit slot) */
    gret 3
    amd64_vread_bullshit \vb, \name
.endm

amd64_gen_movx_mem movx_mem8, 1
amd64_gen_movx_mem movx_mem16, 2

# amd64 load-op adc/sbb: reg <op>= [mem] + carry (ADC 0x13 / SBB 0x1b), mod!=3, 32/64.
# reg=dst/lhs, [mem]=rhs, carry-in = CPU_cf. ARM adcs/sbcs do the carry arithmetic
# directly: set the ARM C flag from cf (cmp cf,1 => C=cf for adc; subs xzr,xzr,cf =>
# C=!cf for sbb, since ARM SBC uses C as "no borrow"), then adcs/sbcs sets the result
# + C (=x86 CF for adc, =!borrow for sbb) + V (=x86 OF). AF/ZF/SF/PF via
# amd64_cached_set_addsub_flags, which derives AF as bit 4 of (lhs ^ rhs ^ res) from
# the original rhs in x10 -- the result already carries cf in, so folding cf into rhs
# would corrupt AF (fixed alongside the interp adc/sbb helper).
.macro amd64_gen_loadop_addc name, vbytes, sbit
.gadget amd64_\name
    ldr  x8, [_ip]                   /* meta */
    ldr  x9, [_ip, 8]                /* disp */
    and  x10, x8, 0xfff              /* opcode|regid<<8 (survives prep) */
    amd64_vmem_addr                  /* x9 = effective address */
    amd64_vread_prep \vbytes, \name  /* x9 = host addr of the rhs */
.if \vbytes == 4
    ldr  w11, [x9]                   /* rhs = [mem] (32-bit) */
.else
    ldr  x11, [x9]                   /* rhs = [mem] (64-bit) */
.endif
    and  x9, x10, 0xff               /* x9 = opcode */
    ubfx x8, x10, 8, 4               /* x8 = dst reg id */
    add  x12, _cpu, CPU_amd64_regs
    ldr  x13, [x12, x8, lsl 3]       /* x13 = lhs = dst reg value */
    mov  x10, x11                    /* x10 = rhs */
    ldrb w12, [_cpu, CPU_cf]         /* x12 = carry-in (0/1) */
.if \vbytes == 4
    mov  w13, w13
    mov  w10, w10
.endif
    cmp  x9, 0x18
    b.hs 1f                          /* SBB (0x1b) */
    cmp  x12, 1                      /* ARM C = cf */
.if \vbytes == 4
    adcs w15, w13, w10
.else
    adcs x15, x13, x10
.endif
    cset w14, cs                     /* CF */
    cset w16, vs                     /* OF */
    b    2f
1:  subs xzr, xzr, x12               /* ARM C = !cf */
.if \vbytes == 4
    sbcs w15, w13, w10
.else
    sbcs x15, x13, x10
.endif
    cset w14, cc                     /* CF (borrow) */
    cset w16, vs                     /* OF */
2:
.if \vbytes == 4
    mov  w15, w15                    /* zero-extend 32-bit result for the flag macro */
.endif
    amd64_cached_set_addsub_flags \sbit  /* AF from original rhs (x10), not rhs+cf */
    add  x12, _cpu, CPU_amd64_regs
.if \vbytes == 4
    ubfx x13, x15, 0, 32
    str  x13, [x12, x8, lsl 3]
.else
    str  x15, [x12, x8, lsl 3]
.endif
    gret 3
    amd64_vread_bullshit \vbytes, \name
.endm

amd64_gen_loadop_addc loadop_addc32, 4, 31
amd64_gen_loadop_addc loadop_addc64, 8, 63

# amd64 op-store/RMW adc/sbb: [mem] <op>= reg + carry (ADC 0x11 / SBB 0x19), mod!=3,
# 32/64. Same carry handling as loadop_addc but [mem] is lhs+dest (RMW): guest addr in
# x0 across both preps, result -> x10 before the store. carry-in read after the read
# prep; flags (incl. the new CF) written before the store.
.macro amd64_gen_opstore_addc name, vbytes, sbit
.gadget amd64_\name
    ldr  x8, [_ip]                   /* meta */
    ldr  x9, [_ip, 8]                /* disp */
    and  x10, x8, 0xfff              /* opcode|regid<<8 (survives read prep) */
    amd64_vmem_addr                  /* x9 = guest effective address */
    mov  x0, x9                      /* x0 = guest addr (survives both preps) */
    amd64_vread_prep \vbytes, \name\()_r   /* x9 = host read addr */
.if \vbytes == 4
    ldr  w13, [x9]                   /* lhs = [mem] (32-bit) */
.else
    ldr  x13, [x9]                   /* lhs = [mem] (64-bit) */
.endif
    and  x9, x10, 0xff               /* x9 = opcode */
    ubfx x12, x10, 8, 4              /* x12 = reg id (rhs) */
    add  x11, _cpu, CPU_amd64_regs
    ldr  x10, [x11, x12, lsl 3]      /* x10 = rhs = reg value */
    ldrb w12, [_cpu, CPU_cf]         /* x12 = carry-in (0/1) */
.if \vbytes == 4
    mov  w13, w13
    mov  w10, w10
.endif
    cmp  x9, 0x18
    b.hs 1f                          /* SBB (0x19) */
    cmp  x12, 1                      /* ARM C = cf */
.if \vbytes == 4
    adcs w15, w13, w10
.else
    adcs x15, x13, x10
.endif
    cset w14, cs
    cset w16, vs
    b    2f
1:  subs xzr, xzr, x12               /* ARM C = !cf */
.if \vbytes == 4
    sbcs w15, w13, w10
.else
    sbcs x15, x13, x10
.endif
    cset w14, cc
    cset w16, vs
2:
.if \vbytes == 4
    mov  w15, w15                    /* zero-extend 32-bit result for the flag macro */
.endif
    amd64_cached_set_addsub_flags \sbit  /* AF from original rhs (x10), not rhs+cf */
    mov  x10, x15                    /* result -> x10 (survives the write prep) */
    mov  x9, x0                      /* guest addr -> x9 */
    amd64_vwrite_prep \vbytes, \name\()_w
.if \vbytes == 4
    str  w10, [x9]
.else
    str  x10, [x9]
.endif
    amd64_vwrite_done \vbytes, \name\()_w
    gret 3
    amd64_vread_bullshit \vbytes, \name\()_r
    amd64_vwrite_bullshit \vbytes, \name\()_w
.endm

amd64_gen_opstore_addc opstore_addc32, 4, 31
amd64_gen_opstore_addc opstore_addc64, 8, 63

# amd64 REGISTER adc/sbb -- reg-reg (0x11/13 adc, 0x19/1b sbb) + reg-imm (0x81/83 /2 adc,
# /3 sbb), mod==3, 32/64. The memory forms were already native (loadop/opstore_addc); the
# register forms bridged (ending the block) -- they appear in multi-precision arithmetic
# and the `sbb reg,reg` carry-materialize idiom. Same ARM carry trick as loadop_addc: set
# ARM C from CPU_cf (cmp cf,1 => C=cf for adc; subs xzr,xzr,cf => C=!cf for sbb), then
# adcs/sbcs gives result + CF + V(=OF). AF from the ORIGINAL rhs (x10), never rhs+cf.
# Flush-style (gen.c flushes the reg cache; lhs/rhs/dst are in CPU_amd64_regs).
# reg-reg packed: is_sbb(0) | dst_id(4-7) | src_id(8-11).
.macro amd64_gen_adcsbb_rr name, vbytes, sbit
.gadget amd64_\name
    ldr  x8, [_ip]
    add  x12, _cpu, CPU_amd64_regs
    ubfx x9, x8, 4, 4                /* dst id */
    ldr  x13, [x12, x9, lsl 3]       /* lhs = dst */
    ubfx x11, x8, 8, 4               /* src id */
    ldr  x10, [x12, x11, lsl 3]      /* rhs = src */
    ldrb w12, [_cpu, CPU_cf]         /* carry-in */
.if \vbytes == 4
    mov  w13, w13
    mov  w10, w10
.endif
    tbnz x8, 0, 1f                   /* SBB? */
    cmp  x12, 1                      /* ADC: ARM C = cf */
.if \vbytes == 4
    adcs w15, w13, w10
.else
    adcs x15, x13, x10
.endif
    cset w14, cs
    cset w16, vs
    b    2f
1:  subs xzr, xzr, x12               /* SBB: ARM C = !cf */
.if \vbytes == 4
    sbcs w15, w13, w10
.else
    sbcs x15, x13, x10
.endif
    cset w14, cc
    cset w16, vs
2:
.if \vbytes == 4
    mov  w15, w15
.endif
    amd64_cached_set_addsub_flags \sbit
    add  x12, _cpu, CPU_amd64_regs
    ubfx x9, x8, 4, 4
.if \vbytes == 4
    ubfx x13, x15, 0, 32
    str  x13, [x12, x9, lsl 3]
.else
    str  x15, [x12, x9, lsl 3]
.endif
    gret 1
.endm
amd64_gen_adcsbb_rr adcsbb_rr32, 4, 31
amd64_gen_adcsbb_rr adcsbb_rr64, 8, 63

# reg-imm adc/sbb. packed: is_sbb(0) | rm_id(4-7); imm (sign-extended by gen.c) operand 1.
.macro amd64_gen_adcsbb_ri name, vbytes, sbit
.gadget amd64_\name
    ldr  x8, [_ip]
    ldr  x10, [_ip, 8]               /* rhs = imm */
    add  x12, _cpu, CPU_amd64_regs
    ubfx x9, x8, 4, 4                /* rm id */
    ldr  x13, [x12, x9, lsl 3]       /* lhs = rm */
    ldrb w12, [_cpu, CPU_cf]         /* carry-in */
.if \vbytes == 4
    mov  w13, w13
    mov  w10, w10
.endif
    tbnz x8, 0, 1f
    cmp  x12, 1                      /* ADC: ARM C = cf */
.if \vbytes == 4
    adcs w15, w13, w10
.else
    adcs x15, x13, x10
.endif
    cset w14, cs
    cset w16, vs
    b    2f
1:  subs xzr, xzr, x12               /* SBB: ARM C = !cf */
.if \vbytes == 4
    sbcs w15, w13, w10
.else
    sbcs x15, x13, x10
.endif
    cset w14, cc
    cset w16, vs
2:
.if \vbytes == 4
    mov  w15, w15
.endif
    amd64_cached_set_addsub_flags \sbit
    add  x12, _cpu, CPU_amd64_regs
    ubfx x9, x8, 4, 4
.if \vbytes == 4
    ubfx x13, x15, 0, 32
    str  x13, [x12, x9, lsl 3]
.else
    str  x15, [x12, x9, lsl 3]
.endif
    gret 2
.endm
amd64_gen_adcsbb_ri adcsbb_ri32, 4, 31
amd64_gen_adcsbb_ri adcsbb_ri64, 8, 63

# amd64 imm-to-mem arith RMW: [mem] <op>= imm for ADD (group /0) and SUB (group /5),
# mod!=3, 32/64. The immediate (sign-extended to 64 by gen.c) is operand 3; the group
# (0=add, 5=sub) is operand 4. Same dual-prep RMW frame as opstore_arith (addr in x0,
# result in x10 across both preps) with eager addsub flags. cmp (group /7) is kept on
# the interpreter (glibc/musl startup), and adc/sbb-imm + byte/16-bit bridge.
# Operands: meta(0)/disp(8)/next_ip(16)/imm(24)/group(32) -> gret 5.
.macro amd64_gen_imm_arith name, vbytes, sbit
.gadget amd64_\name
    ldr  x8, [_ip]                   /* meta */
    ldr  x9, [_ip, 8]                /* disp */
    amd64_vmem_addr                  /* x9 = guest effective address */
    mov  x0, x9                      /* x0 = guest addr (survives both preps) */
    amd64_vread_prep \vbytes, \name\()_r   /* x9 = host read addr */
.if \vbytes == 4
    ldr  w13, [x9]                   /* lhs = [mem] (32-bit) */
.else
    ldr  x13, [x9]                   /* lhs = [mem] (64-bit) */
.endif
    ldr  x10, [_ip, 24]             /* rhs = immediate */
    ldr  x9, [_ip, 32]             /* group: 0=add, 5=sub */
.if \vbytes == 4
    mov  w13, w13
    mov  w10, w10
    cmp  x9, 0
    b.eq 1f
    subs w15, w13, w10
    cset w14, cc
    cset w16, vs
    b    2f
1:  adds w15, w13, w10
    cset w14, cs
    cset w16, vs
2:  mov  w15, w15
.else
    cmp  x9, 0
    b.eq 1f
    subs x15, x13, x10
    cset w14, cc
    cset w16, vs
    b    2f
1:  adds x15, x13, x10
    cset w14, cs
    cset w16, vs
2:
.endif
    amd64_cached_set_addsub_flags \sbit
    mov  x10, x15                    /* result -> x10 (survives the write prep) */
    mov  x9, x0                      /* guest addr -> x9 */
    amd64_vwrite_prep \vbytes, \name\()_w
.if \vbytes == 4
    str  w10, [x9]
.else
    str  x10, [x9]
.endif
    amd64_vwrite_done \vbytes, \name\()_w
    gret 5
    amd64_vread_bullshit \vbytes, \name\()_r
    amd64_vwrite_bullshit \vbytes, \name\()_w
.endm

# <alu> reg, r/m -- REGISTER destination, memory source: 0x03 add, 0x0b or,
# 0x23 and, 0x2b sub, 0x33 xor, 0x3b cmp.
#
# Simpler than the imm-to-mem family above because memory is READ-ONLY here:
# no write prep, no store, and nothing to undo if the write would have faulted.
# `sub 2b` alone was 6.7% of all remaining interpreter bridges after the FS
# work, because every one of these had to go through amd64_jit_mem_op.
#
# Register contract for the flag macros: x13 = lhs (the register), x10 = rhs
# (the memory value), x15 = result, w14 = CF, w16 = OF. Same as the imm family,
# with lhs and rhs swapped in origin -- there the memory operand is the
# destination, here it is the source.
#
# meta must be re-read from [_ip] after amd64_vread_prep, which clobbers x8.
# Operands: [meta][disp][next_ip][group]. gret 4.
.macro amd64_gen_regmem_arith name, vbytes, sbit
.gadget amd64_\name
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    amd64_vmem_addr
    amd64_vread_prep \vbytes, \name
.if \vbytes == 4
    ldr  w10, [x9]
.else
    ldr  x10, [x9]
.endif
    ldr  x8, [_ip]
    ubfx x12, x8, 8, 4
    add  x14, _cpu, CPU_amd64_regs
    ldr  x13, [x14, x12, lsl 3]
    ldr  x9, [_ip, 24]
.if \vbytes == 4
    mov  w13, w13
    mov  w10, w10
    cmp  x9, 0
    b.eq 1f
    subs w15, w13, w10
    cset w14, cc
    cset w16, vs
    b    2f
1:  adds w15, w13, w10
    cset w14, cs
    cset w16, vs
2:  mov  w15, w15
.else
    cmp  x9, 0
    b.eq 1f
    subs x15, x13, x10
    cset w14, cc
    cset w16, vs
    b    2f
1:  adds x15, x13, x10
    cset w14, cs
    cset w16, vs
2:
.endif
    amd64_cached_set_addsub_flags \sbit
    ldr  x8, [_ip]
    ubfx x12, x8, 8, 4
    add  x14, _cpu, CPU_amd64_regs
    str  x15, [x14, x12, lsl 3]
    gret 4
    amd64_vread_bullshit \vbytes, \name
.endm

# CMP reg, r/m (0x3b): identical to the subtract above but with no write back.
.macro amd64_gen_regmem_cmp name, vbytes, sbit
.gadget amd64_\name
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    amd64_vmem_addr
    amd64_vread_prep \vbytes, \name
.if \vbytes == 4
    ldr  w10, [x9]
.else
    ldr  x10, [x9]
.endif
    ldr  x8, [_ip]
    ubfx x12, x8, 8, 4
    add  x14, _cpu, CPU_amd64_regs
    ldr  x13, [x14, x12, lsl 3]
.if \vbytes == 4
    mov  w13, w13
    mov  w10, w10
    subs w15, w13, w10
    cset w14, cc
    cset w16, vs
    mov  w15, w15
.else
    subs x15, x13, x10
    cset w14, cc
    cset w16, vs
.endif
    amd64_cached_set_addsub_flags \sbit
    gret 4
    amd64_vread_bullshit \vbytes, \name
.endm

# <logic> reg, r/m: 0x0b or, 0x23 and, 0x33 xor. Clears CF and OF.
.macro amd64_gen_regmem_logic name, vbytes, ssize
.gadget amd64_\name
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    amd64_vmem_addr
    amd64_vread_prep \vbytes, \name
.if \vbytes == 4
    ldr  w10, [x9]
.else
    ldr  x10, [x9]
.endif
    ldr  x8, [_ip]
    ubfx x12, x8, 8, 4
    add  x14, _cpu, CPU_amd64_regs
    ldr  x13, [x14, x12, lsl 3]
    ldr  x9, [_ip, 24]
    cmp  x9, 4
    b.eq 1f
    cmp  x9, 6
    b.eq 2f
    orr  x15, x13, x10
    b    3f
1:  and  x15, x13, x10
    b    3f
2:  eor  x15, x13, x10
3:
.if \vbytes == 4
    mov  w15, w15
.endif
    mov  x11, \ssize
    amd64_cached_set_logic_flags x15, x11
    ldr  x8, [_ip]
    ubfx x12, x8, 8, 4
    add  x14, _cpu, CPU_amd64_regs
    str  x15, [x14, x12, lsl 3]
    gret 4
    amd64_vread_bullshit \vbytes, \name
.endm

amd64_gen_regmem_arith regmem_arith32, 4, 31
amd64_gen_regmem_arith regmem_arith64, 8, 63
amd64_gen_regmem_cmp   regmem_cmp32,   4, 31
amd64_gen_regmem_cmp   regmem_cmp64,   8, 63
amd64_gen_regmem_logic regmem_logic32, 4, 32
amd64_gen_regmem_logic regmem_logic64, 8, 64



# MOV [mem], imm -- store an immediate: 0xc7 (imm32, sign-extended to the
# operand size) and 0xc6 (imm8). A pure store, so no read prep, no flags -- the
# single most common thing after a load, struct-field init and zeroing. `movl
# $imm,(mem)` / `movq $imm,(mem)` were ~7% of the remaining bridges (opcode c7,
# memory forms). Operands: [meta][disp][next_ip][imm]. gret 4.
.macro amd64_gen_mov_imm_mem name, vbytes
.gadget amd64_\name
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    amd64_vmem_addr
    amd64_vwrite_prep \vbytes, \name
    ldr  x10, [_ip, 24]              /* imm (already sign-extended by gen.c) */
.if \vbytes == 1
    strb w10, [x9]
.elseif \vbytes == 2
    strh w10, [x9]
.elseif \vbytes == 4
    str  w10, [x9]
.else
    str  x10, [x9]
.endif
    amd64_vwrite_done \vbytes, \name
    gret 4
    amd64_vwrite_bullshit \vbytes, \name
.endm

amd64_gen_mov_imm_mem mov_imm_mem8,  1
amd64_gen_mov_imm_mem mov_imm_mem16, 2
amd64_gen_mov_imm_mem mov_imm_mem32, 4
amd64_gen_mov_imm_mem mov_imm_mem64, 8

# The source register of an r/m, reg form, \vb bytes wide, into x\n: the reg
# field (meta bits 8-11); for a byte, AH/CH/DH/BH when it is 4-7 and there is
# no REX (meta bit 34). Clobbers x12, x14.
.macro amd64_meta_reg_value vb, n
    ldr  x\n, [_ip]
    ubfx x12, x\n, 8, 4
    add  x14, _cpu, CPU_amd64_regs
.if \vb == 1
    tbnz x\n, 34, .Lmrv_low_\@
    cmp  x12, 4
    b.lo .Lmrv_low_\@
    sub  x12, x12, 4
    ldr  x\n, [x14, x12, lsl 3]
    ubfx x\n, x\n, 8, 8
    b    .Lmrv_done_\@
.Lmrv_low_\@:
    ldr  x\n, [x14, x12, lsl 3]
    and  x\n, x\n, 0xff
.elseif \vb == 2
    ldr  x\n, [x14, x12, lsl 3]
    and  x\n, x\n, 0xffff
.elseif \vb == 4
    add  x12, x14, x12, lsl 3        /* a w load cannot scale by 8 */
    ldr  w\n, [x12]
.else
    ldr  x\n, [x14, x12, lsl 3]
.endif
.Lmrv_done_\@:
.endm

# Write x\n (\vb bytes) to the register a meta word's reg field names, the
# same byte rules; 8 and 16 keep the rest, 32 zero-extends. Clobbers x8, x12,
# x14, x15 -- so \n must not be 8, 12, 14 or 15.
.macro amd64_meta_reg_write vb, n
    ldr  x8, [_ip]
    ubfx x12, x8, 8, 4
    add  x14, _cpu, CPU_amd64_regs
.if \vb == 1
    tbnz x8, 34, .Lmrw_low_\@
    cmp  x12, 4
    b.lo .Lmrw_low_\@
    sub  x12, x12, 4
    ldr  x15, [x14, x12, lsl 3]
    bfi  x15, x\n, 8, 8
    b    .Lmrw_st_\@
.Lmrw_low_\@:
    ldr  x15, [x14, x12, lsl 3]
    bfi  x15, x\n, 0, 8
.elseif \vb == 2
    ldr  x15, [x14, x12, lsl 3]
    bfi  x15, x\n, 0, 16
.elseif \vb == 4
    mov  w15, w\n
.else
    mov  x15, x\n
.endif
.Lmrw_st_\@:
    str  x15, [x14, x12, lsl 3]
.endm

# The flags of lhs (x13) <add|sub> rhs (x10), both zero-extended from \bits:
# CF and OF by putting the operands at the top of 64 bits; x15 = the result.
.macro amd64_sized_addsub_flags op, bits
.if \bits == 64
    \op\()s x15, x13, x10
.else
    lsl  x4, x13, (64 - \bits)
    lsl  x5, x10, (64 - \bits)
    \op\()s x6, x4, x5
.endif
.ifc \op,sub
    cset w14, cc
.else
    cset w14, cs
.endif
    cset w16, vs
.if \bits != 64
    lsr  x15, x6, (64 - \bits)
.endif
    amd64_cached_set_addsub_flags (\bits - 1)
.endm

# CMPXCHG [mem], reg (0F B0, 0F B1), 8/16/32/64 -- the atomic compare-exchange
# every mutex and lock-free structure is built on. Aligned (the case compilers
# produce, and every byte): a host exclusive compare-and-swap at the
# write-prepped host address (ARMv8.0: ldaxr/stlxr). Misaligned: x86_atomic_cas
# (emu/tlb.c), the shared misaligned-LOCK primitive -- a 16-byte block CAS, or
# this address space's split lock for an operand that straddles blocks. Either
# way the result is the old value; the flags are CMP rax, old; a failed
# compare loads it into the accumulator (AL/AX keep the rest of RAX, EAX
# zero-extends), a successful one leaves it alone. A page that is not
# writable faults even when the compare fails, as on x86. LOCK changes
# nothing a single thread can see; the swap is atomic anyway.
# Operands: [meta][disp][next_ip][size]. gret 4.
.macro amd64_gen_cmpxchg_mem name, vbytes, bits
.gadget amd64_\name
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    amd64_vmem_addr                  /* x9 = guest effective address */
.if \vbytes > 1
    tst  x9, (\vbytes - 1)
    b.ne 8f
.endif
    amd64_vwrite_prep \vbytes, \name
    amd64_meta_reg_value \vbytes, 3  /* desired: the source register */
    add  x14, _cpu, CPU_amd64_regs
    ldr  x10, [x14]                  /* expected: rax */
.if \vbytes == 1
    and  x10, x10, 0xff
1:  ldaxrb w13, [x9]
    cmp  w13, w10
    b.ne 2f
    stlxrb w11, w3, [x9]
.elseif \vbytes == 2
    and  x10, x10, 0xffff
1:  ldaxrh w13, [x9]
    cmp  w13, w10
    b.ne 2f
    stlxrh w11, w3, [x9]
.elseif \vbytes == 4
    mov  w10, w10
1:  ldaxr w13, [x9]
    cmp  w13, w10
    b.ne 2f
    stlxr w11, w3, [x9]
.else
1:  ldaxr x13, [x9]
    cmp  x13, x10
    b.ne 2f
    stlxr w11, x3, [x9]
.endif
    cbnz w11, 1b
    b    4f
2:  clrex
4:  mov  x7, x13                     /* the old value (zero-extended) */
    mov  x13, x10                    /* CMP rax, old: lhs rax, rhs old */
    mov  x10, x7
    amd64_sized_addsub_flags sub, \bits
    cbz  x15, 5f                     /* equal: swapped, the accumulator stays */
    add  x14, _cpu, CPU_amd64_regs
.if \vbytes == 1
    ldr  x15, [x14]
    bfi  x15, x7, 0, 8
    str  x15, [x14]
.elseif \vbytes == 2
    ldr  x15, [x14]
    bfi  x15, x7, 0, 16
    str  x15, [x14]
.else
    str  x7, [x14]                   /* rax = old (zero-extended for 32) */
.endif
5:  gret 4

.if \vbytes > 1
8:  /* misaligned: x86_atomic_cas(cpu, tlb, addr, size, expected, desired,
       &old, &swapped). _cpu is x1 and _tlb x2: read them before they are
       argument registers. */
    amd64_meta_reg_value \vbytes, 5
    add  x14, _cpu, CPU_amd64_regs
    ldr  x4, [x14]
.if \vbytes == 2
    and  x4, x4, 0xffff
.elseif \vbytes == 4
    mov  w4, w4
.endif
    sub  sp, sp, 16
    mov  x6, sp                      /* &old: this slot, above save_c's frame */
    add  x7, sp, 8                   /* &swapped */
    save_c
    mov  x0, _cpu
    sub  x1, _tlb, TLB_entries
    mov  x2, x9
    mov  x3, \vbytes
    bl   NAME(x86_atomic_cas)
    mov  x19, x0
    restore_c
    ldr  x13, [sp]
    add  sp, sp, 16
    cbnz w19, 9f
    add  x14, _cpu, CPU_amd64_regs
    ldr  x10, [x14]
.if \vbytes == 2
    and  x10, x10, 0xffff
.elseif \vbytes == 4
    mov  w10, w10
.endif
    b    4b
9:  mov  x0, x19                     /* INT_PF, segfault_addr already set */
    b    jit_exit
.endif
    amd64_vwrite_bullshit \vbytes, \name
.endm
amd64_gen_cmpxchg_mem cmpxchg_mem8, 1, 8
amd64_gen_cmpxchg_mem cmpxchg_mem16, 2, 16
amd64_gen_cmpxchg_mem cmpxchg_mem32, 4, 32
amd64_gen_cmpxchg_mem cmpxchg_mem64, 8, 64

# XADD [mem], reg (0F C0, 0F C1), 8/16/32/64, with or without LOCK (atomic
# either way): [mem] += reg, reg = the old value, the flags those of the ADD.
# Aligned: a host exclusive add loop; misaligned: x86_atomic_xadd (emu/tlb.c).
# Operands: [meta][disp][next_ip]. gret 3.
.macro amd64_gen_xadd_mem name, vbytes, bits
.gadget amd64_\name
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    amd64_vmem_addr
.if \vbytes > 1
    tst  x9, (\vbytes - 1)
    b.ne 8f
.endif
    amd64_vwrite_prep \vbytes, \name
    amd64_meta_reg_value \vbytes, 10 /* the addend */
.if \vbytes == 1
1:  ldaxrb w13, [x9]
    add  w11, w13, w10
    stlxrb w12, w11, [x9]
.elseif \vbytes == 2
1:  ldaxrh w13, [x9]
    add  w11, w13, w10
    stlxrh w12, w11, [x9]
.elseif \vbytes == 4
1:  ldaxr w13, [x9]
    add  w11, w13, w10
    stlxr w12, w11, [x9]
.else
1:  ldaxr x13, [x9]
    add  x11, x13, x10
    stlxr w12, x11, [x9]
.endif
    cbnz w12, 1b
4:  mov  x7, x13                     /* the old value */
    amd64_sized_addsub_flags add, \bits
    amd64_meta_reg_write \vbytes, 7
    gret 3

.if \vbytes > 1
8:  amd64_meta_reg_value \vbytes, 4
    sub  sp, sp, 16
    mov  x5, sp                      /* &old */
    save_c
    mov  x0, _cpu
    sub  x1, _tlb, TLB_entries
    mov  x2, x9
    mov  x3, \vbytes
    bl   NAME(x86_atomic_xadd)
    mov  x19, x0
    restore_c
    ldr  x13, [sp]
    add  sp, sp, 16
    cbnz w19, 9f
    amd64_meta_reg_value \vbytes, 10
    b    4b
9:  mov  x0, x19
    b    jit_exit
.endif
    amd64_vwrite_bullshit \vbytes, \name
.endm
amd64_gen_xadd_mem xadd_mem8, 1, 8
amd64_gen_xadd_mem xadd_mem16, 2, 16
amd64_gen_xadd_mem xadd_mem32, 4, 32
amd64_gen_xadd_mem xadd_mem64, 8, 64

# ===========================================================================
# amd64 ALU with a memory operand, every width and form the narrower native
# families leave out: ADD OR ADC SBB AND SUB XOR CMP, and TEST (op 8), on
# r/m, reg / reg, r/m / r/m, imm, 8/16/32/64 bits, with LOCK on the RMW
# directions (an exclusive loop, or x86_atomic_alu when misaligned). The
# flags are computed at the operand width: the operands go at the top of 64
# bits so the host's carry and overflow are the width's; ADC fills the bits
# below with ones so its carry-in ripples into the operand (SBB's borrow
# does on its own), and the carry-in is read once, before any LOCK retry
# rewrites CF.
# The register cache is flushed before: x24-x26 are free.
# Operands: [meta][disp][next_ip][packed][imm], packed = op (0-3) |
# reg destination (4) | immediate source (5) | lock (6). gret 5.
# ===========================================================================

# x13 lhs, x10 rhs (zero-extended from \bits), op in x24, carry-in in x26 ->
# x15 the result (zero-extended), flags set. Ops: 0-7 the ALU (ADD OR ADC SBB
# AND SUB XOR CMP), 8 TEST, 10 NEG (0 - lhs; x13/x10 become 0/lhs), 11 NOT
# (no flags). x24 bit 24 keeps CF (INC, DEC: ADD/SUB 1). Clobbers x4-x7, x11,
# x12, x14, x16, x17.
.macro amd64_alu_op bits
    and  x7, x24, 0xf
    cmp  x7, 10
    b.ne .Lao_notneg_\@
    mov  x10, x13                    /* NEG: SUB from 0 */
    mov  x13, 0
    mov  x7, 5
    b    .Lao_go_\@
.Lao_notneg_\@:
    cmp  x7, 11
    b.ne .Lao_go_\@
    mvn  x15, x13                    /* NOT: no flags */
.if \bits < 64
    and  x15, x15, ((1 << \bits) - 1)
.endif
    b    .Lao_end_\@
.Lao_go_\@:
    cmp  x7, 1
    b.eq .Lao_or_\@
    cmp  x7, 4
    b.eq .Lao_and_\@
    cmp  x7, 8
    b.eq .Lao_and_\@                 /* TEST */
    cmp  x7, 6
    b.eq .Lao_xor_\@
    lsl  x4, x13, (64 - \bits)
    lsl  x5, x10, (64 - \bits)
    mov  x6, 0                       /* the carry into the operation */
    cmp  x7, 2
    b.eq .Lao_adc_\@
    cmp  x7, 3
    b.eq .Lao_sbb_\@
    cmp  x7, 0
    b.ne .Lao_sub_\@
    adds x7, x4, x5                  /* ADD */
    cset w14, cs
    cset w16, vs
    b    .Lao_arith_\@
.Lao_adc_\@:
    mov  x6, x26
.if \bits < 64
    cbz  x6, .Lao_adc1_\@
    orr  x4, x4, ((1 << (64 - \bits)) - 1)
.Lao_adc1_\@:
.endif
    cmp  x6, 1                       /* C = carry-in */
    adcs x7, x4, x5
    cset w14, cs
    cset w16, vs
    b    .Lao_arith_\@
.Lao_sbb_\@:
    mov  x6, x26                     /* the borrow ripples up through the */
    negs xzr, x6                     /* zero low bits: C = !carry-in */
    sbcs x7, x4, x5
    cset w14, cc
    cset w16, vs
    b    .Lao_arith_\@
.Lao_sub_\@:                         /* SUB, CMP */
    subs x7, x4, x5
    cset w14, cc
    cset w16, vs
.Lao_arith_\@:
    lsr  x15, x7, (64 - \bits)
    amd64_cached_set_addsub_flags (\bits - 1)
    b    .Lao_done_\@
.Lao_or_\@:
    orr  x15, x13, x10
    b    .Lao_logic_\@
.Lao_and_\@:
    and  x15, x13, x10
    b    .Lao_logic_\@
.Lao_xor_\@:
    eor  x15, x13, x10
.Lao_logic_\@:
    mov  x11, \bits
    amd64_cached_set_logic_flags x15, x11
.Lao_done_\@:
    tbz  x24, 24, .Lao_end_\@
    strb w26, [_cpu, CPU_cf]         /* INC/DEC: CF as it was */
    ldr  w17, [_cpu, CPU_eflags]
    bfi  w17, w26, 0, 1
    str  w17, [_cpu, CPU_eflags]
.Lao_end_\@:
.endm

.macro amd64_alu_load vb, n
.if \vb == 1
    ldrb w\n, [x9]
.elseif \vb == 2
    ldrh w\n, [x9]
.elseif \vb == 4
    ldr  w\n, [x9]
.else
    ldr  x\n, [x9]
.endif
.endm
.macro amd64_alu_store vb, n
.if \vb == 1
    strb w\n, [x9]
.elseif \vb == 2
    strh w\n, [x9]
.elseif \vb == 4
    str  w\n, [x9]
.else
    str  x\n, [x9]
.endif
.endm

.macro amd64_gen_alu_mem name, vb, bits
.gadget amd64_\name
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    amd64_vmem_addr                  /* x9 = guest address */
    ldr  x24, [_ip, 24]              /* packed */
    ldrb w26, [_cpu, CPU_cf]         /* the carry-in, once */
    cmp  w26, 0
    cset x26, ne
    tbz  x24, 5, 1f
    ldr  x10, [_ip, 32]              /* rhs: the immediate */
.if \bits < 64
    and  x10, x10, ((1 << \bits) - 1)
.endif
    b    2f
1:
    amd64_meta_reg_value \vb, 10     /* rhs: the register */
2:  mov  x25, x10
    mov  x0, x9                      /* the guest address, across the preps */
    tbnz x24, 4, 50f                 /* register destination */
    and  x8, x24, 0xf
    cmp  x8, 7
    b.eq 40f                         /* CMP, TEST: read only */
    cmp  x8, 8
    b.eq 40f
    tbnz x24, 6, 60f                 /* LOCK */
    amd64_vread_prep \vb, \name\()_r
    amd64_alu_load \vb, 13
    mov  x10, x25
    amd64_alu_op \bits
    mov  x10, x15                    /* the result, across the write prep */
    mov  x9, x0
    amd64_vwrite_prep \vb, \name\()_w
    amd64_alu_store \vb, 10
    amd64_vwrite_done \vb, \name\()_w
    gret 5
40:
    amd64_vread_prep \vb, \name\()_c
    amd64_alu_load \vb, 13
    mov  x10, x25
    amd64_alu_op \bits
    gret 5
50: /* reg <op>= [mem]: the memory value is the rhs */
    amd64_vread_prep \vb, \name\()_d
    amd64_alu_load \vb, 10
    amd64_meta_reg_value \vb, 13
    amd64_alu_op \bits
    and  x8, x24, 0xf
    cmp  x8, 7
    b.hs 51f
    mov  x10, x15                    /* the macro uses x15 itself */
    amd64_meta_reg_write \vb, 10
51: gret 5
60: /* LOCK */
.if \vb > 1
    tst  x9, (\vb - 1)
    b.ne 70f
.endif
    amd64_vwrite_prep \vb, \name\()_l
61:
.if \vb == 1
    ldaxrb w13, [x9]
.elseif \vb == 2
    ldaxrh w13, [x9]
.elseif \vb == 4
    ldaxr w13, [x9]
.else
    ldaxr x13, [x9]
.endif
    mov  x10, x25
    amd64_alu_op \bits
.if \vb == 1
    stlxrb w12, w15, [x9]
.elseif \vb == 2
    stlxrh w12, w15, [x9]
.elseif \vb == 4
    stlxr w12, w15, [x9]
.else
    stlxr w12, x15, [x9]
.endif
    cbnz w12, 61b
    gret 5
.if \vb > 1
70: /* misaligned LOCK: x86_atomic_alu(cpu, tlb, addr, size, op, rhs, cin, &old) */
    sub  sp, sp, 16
    mov  x7, sp
    save_c
    mov  x0, _cpu
    sub  x1, _tlb, TLB_entries
    mov  x2, x9
    mov  x3, \vb
    and  x4, x24, 0xf
    mov  x5, x25
    mov  x6, x26
    bl   NAME(x86_atomic_alu)
    mov  x19, x0
    restore_c
    ldr  x13, [sp]
    add  sp, sp, 16
    cbnz w19, 79f
    mov  x10, x25
    amd64_alu_op \bits               /* the flags of what was stored */
    gret 5
79: mov  x0, x19
    b    jit_exit
.endif
    amd64_vread_bullshit \vb, \name\()_r
    amd64_vwrite_bullshit \vb, \name\()_w
    amd64_vread_bullshit \vb, \name\()_c
    amd64_vread_bullshit \vb, \name\()_d
    amd64_vwrite_bullshit \vb, \name\()_l
.endm
amd64_gen_alu_mem alu_mem8, 1, 8
amd64_gen_alu_mem alu_mem16, 2, 16
amd64_gen_alu_mem alu_mem32, 4, 32
amd64_gen_alu_mem alu_mem64, 8, 64

# The same ALU, TEST and MOV (op 9) between two registers, every width the
# native register families leave out (8-bit with AH..BH, 16-bit, ADC/SBB).
# Operand: [packed], packed = op (0-3) | dst (4-7) | dst is AH..BH (8) |
# src (12-15) | src is AH..BH (16). gret 1. The alu_ri* variants take an
# immediate source instead: [packed][imm], gret 2.
.macro amd64_rr_read vb, n, sh
    ubfx x12, x24, (\sh), 4
    ldr  x\n, [x14, x12, lsl 3]
.if \vb == 1
    tbz  x24, (\sh + 4), .Lrr_lo_\@
    ubfx x\n, x\n, 8, 8
    b    .Lrr_done_\@
.Lrr_lo_\@:
    and  x\n, x\n, 0xff
.Lrr_done_\@:
.elseif \vb == 2
    and  x\n, x\n, 0xffff
.elseif \vb == 4
    mov  w\n, w\n
.endif
.endm
.macro amd64_gen_alu_rr name, vb, bits, imm=0
.gadget amd64_\name
    ldr  x24, [_ip]
    ldrb w26, [_cpu, CPU_cf]
    cmp  w26, 0
    cset x26, ne
    add  x14, _cpu, CPU_amd64_regs
    amd64_rr_read \vb, 13, 4         /* dst: the lhs */
.if \imm
    ldr  x10, [_ip, 8]               /* src: the immediate */
.if \bits < 64
    and  x10, x10, ((1 << \bits) - 1)
.endif
.else
    amd64_rr_read \vb, 10, 12        /* src: the rhs */
.endif
    and  x8, x24, 0xf
    cmp  x8, 9
    b.eq 1f
    amd64_alu_op \bits
    and  x8, x24, 0xf
    cmp  x8, 7
    b.eq 9f                          /* CMP, TEST: flags only */
    cmp  x8, 8
    b.eq 9f
    b    2f
1:  mov  x15, x10                    /* MOV: no flags */
2:  add  x14, _cpu, CPU_amd64_regs
    ubfx x12, x24, 4, 4
    ldr  x11, [x14, x12, lsl 3]
.if \vb == 1
    tbz  x24, 8, 3f
    bfi  x11, x15, 8, 8
    b    4f
3:  bfi  x11, x15, 0, 8
4:
.elseif \vb == 2
    bfi  x11, x15, 0, 16
.elseif \vb == 4
    mov  w11, w15
.else
    mov  x11, x15
.endif
    str  x11, [x14, x12, lsl 3]
9:
.if \imm
    gret 2
.else
    gret 1
.endif
.endm
amd64_gen_alu_rr alu_ri8, 1, 8, 1
amd64_gen_alu_rr alu_ri16, 2, 16, 1
amd64_gen_alu_rr alu_ri32, 4, 32, 1
amd64_gen_alu_rr alu_ri64, 8, 64, 1
amd64_gen_alu_rr alu_rr8, 1, 8
amd64_gen_alu_rr alu_rr16, 2, 16
amd64_gen_alu_rr alu_rr32, 4, 32
amd64_gen_alu_rr alu_rr64, 8, 64

# Write x\n into the register named by packed field \sh (AH..BH flag at
# \sh + 4 for a byte): 8/16 keep the rest, 32 zero-extends. x14 = the
# register file. Clobbers x11 x12.
.macro amd64_rr_write vb, n, sh
    ubfx x12, x24, (\sh), 4
    ldr  x11, [x14, x12, lsl 3]
.if \vb == 1
    tbz  x24, (\sh + 4), .Lrw_lo_\@
    bfi  x11, x\n, 8, 8
    b    .Lrw_done_\@
.Lrw_lo_\@:
    bfi  x11, x\n, 0, 8
.Lrw_done_\@:
.elseif \vb == 2
    bfi  x11, x\n, 0, 16
.elseif \vb == 4
    mov  w11, w\n
.else
    mov  x11, x\n
.endif
    str  x11, [x14, x12, lsl 3]
.endm

# CMPXCHG r/m, r and XADD r/m, r with a register destination (0F B0/B1,
# C0/C1, mod 3). [packed] as amd64_alu_rr's: dst (4-7) | dst AH..BH (8) |
# src (12-15) | src AH..BH (16). gret 1.
# CMPXCHG: the flags of CMP acc, dst; equal, dst = src; not, the
# accumulator = dst (AL/AX keep the rest of RAX, EAX zero-extends). Only the
# register written changes: a successful 32-bit compare leaves RAX's top
# half, a failed one the destination's (hardware: an AMD Ryzen).
.macro amd64_gen_cmpxchg_rr name, vb, bits
.gadget amd64_\name
    ldr  x24, [_ip]
    add  x14, _cpu, CPU_amd64_regs
    ldr  x13, [x14]                  /* the accumulator */
.if \vb == 1
    and  x13, x13, 0xff
.elseif \vb == 2
    and  x13, x13, 0xffff
.elseif \vb == 4
    mov  w13, w13
.endif
    amd64_rr_read \vb, 10, 4         /* dst */
    mov  x25, x10
    mov  x26, x13
    amd64_sized_addsub_flags sub, \bits
    add  x14, _cpu, CPU_amd64_regs
    cmp  x26, x25
    b.ne 1f
    amd64_rr_read \vb, 10, 12        /* src */
    amd64_rr_write \vb, 10, 4
    gret 1
1:  ldr  x11, [x14]
.if \vb == 1
    bfi  x11, x25, 0, 8
.elseif \vb == 2
    bfi  x11, x25, 0, 16
.elseif \vb == 4
    mov  w11, w25
.else
    mov  x11, x25
.endif
    str  x11, [x14]
    gret 1
.endm
amd64_gen_cmpxchg_rr cmpxchg_rr8, 1, 8
amd64_gen_cmpxchg_rr cmpxchg_rr16, 2, 16
amd64_gen_cmpxchg_rr cmpxchg_rr32, 4, 32
amd64_gen_cmpxchg_rr cmpxchg_rr64, 8, 64

# XADD: sum = dst + src with ADD's flags; src = the old dst, then dst = sum
# (so `xadd r, r` leaves the sum).
.macro amd64_gen_xadd_rr name, vb, bits
.gadget amd64_\name
    ldr  x24, [_ip]
    add  x14, _cpu, CPU_amd64_regs
    amd64_rr_read \vb, 13, 4         /* dst */
    amd64_rr_read \vb, 10, 12        /* src */
    mov  x25, x13
    amd64_sized_addsub_flags add, \bits
    mov  x26, x15
    add  x14, _cpu, CPU_amd64_regs
    amd64_rr_write \vb, 25, 12
    amd64_rr_write \vb, 26, 4
    gret 1
.endm
amd64_gen_xadd_rr xadd_rr8, 1, 8
amd64_gen_xadd_rr xadd_rr16, 2, 16
amd64_gen_xadd_rr xadd_rr32, 4, 32
amd64_gen_xadd_rr xadd_rr64, 8, 64

# XCHG reg, [mem] (86, 87), 8/16/32/64 -- implicitly locked whether or not
# the prefix is written, so the spinlock-release idiom. Aligned: a host
# exclusive swap; misaligned: x86_atomic_xchg (emu/tlb.c). The old value goes
# to the register (8/16 keep the rest, AH..BH for a byte; 32 zero-extends).
# [meta][disp][next_ip], gret 3.
.macro amd64_gen_xchg_mem name, vb
.gadget amd64_\name
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    amd64_vmem_addr
.if \vb > 1
    tst  x9, (\vb - 1)
    b.ne 8f
.endif
    amd64_vwrite_prep \vb, \name
    amd64_meta_reg_value \vb, 10
    dmb  ish
.if \vb == 1
1:  ldaxrb w13, [x9]
    stlxrb w11, w10, [x9]
.elseif \vb == 2
1:  ldaxrh w13, [x9]
    stlxrh w11, w10, [x9]
.elseif \vb == 4
1:  ldaxr w13, [x9]
    stlxr w11, w10, [x9]
.else
1:  ldaxr x13, [x9]
    stlxr w11, x10, [x9]
.endif
    cbnz w11, 1b
    dmb  ish
    mov  x7, x13
    amd64_meta_reg_write \vb, 7
    gret 3
.if \vb > 1
8:  /* misaligned: x86_atomic_xchg(cpu, tlb, addr, size, value, &old) */
    amd64_meta_reg_value \vb, 4
    sub  sp, sp, 16
    mov  x5, sp
    save_c
    mov  x0, _cpu
    sub  x1, _tlb, TLB_entries
    mov  x2, x9
    mov  x3, \vb
    bl   NAME(x86_atomic_xchg)
    mov  x19, x0
    restore_c
    ldr  x7, [sp]
    add  sp, sp, 16
    cbnz w19, 9f
    amd64_meta_reg_write \vb, 7
    gret 3
9:  mov  x0, x19
    b    jit_exit
.endif
    amd64_vwrite_bullshit \vb, \name
.endm
amd64_gen_xchg_mem xchg_mem8, 1
amd64_gen_xchg_mem xchg_mem16, 2
amd64_gen_xchg_mem xchg_mem32, 4
amd64_gen_xchg_mem xchg_mem64, 8

# ===========================================================================
# Shifts and rotates, every width and count source the native shift family
# leaves out (memory operands, 8/16-bit, RCL/RCR): ROL ROR RCL RCR SHL SHR SAL
# (/6, as SHL -- what the hardware does) SAR, by 1 (D0/D1), CL (D2/D3) or
# imm8 (C0/C1). The count is masked to 5 bits (6 for 64); a masked count of 0
# changes nothing (a memory operand is still read). SHL/SHR/SAR set CF to
# the last bit out (SAR past the width: the sign), OF for a count of 1 (else
# 0), AF 0 and ZF/SF/PF from the result; ROL/ROR set CF, and OF for a count
# of 1, and nothing else; RCL/RCR rotate through CF, by count mod 9/17 for
# 8/16 bits.
# ===========================================================================

# x13 value (zero-extended from \bits), x6 the masked count (nonzero), x24
# bits 0-2 the subop -> x15 the result, flags. Clobbers x4, x5, x7, x10-x12,
# x14, x16, x17.
.macro amd64_shift_core bits
    and  x7, x24, 7
    cmp  x7, 2
    b.lo .Lsh_rot_\@
    cmp  x7, 4
    b.lo .Lsh_rc_\@
    cmp  x7, 5
    b.eq .Lsh_shr_\@
    cmp  x7, 7
    b.eq .Lsh_sar_\@
    /* SHL / SAL */
    lsl  x15, x13, x6
.if \bits < 64
    and  x15, x15, ((1 << \bits) - 1)
.endif
    mov  x14, 0                      /* CF: bit (bits - count), 0 past it */
    cmp  x6, \bits
    b.hi 1f
    mov  x4, \bits
    sub  x4, x4, x6
    lsr  x14, x13, x4
    and  x14, x14, 1
1:  mov  x16, 0                      /* OF (count 1): MSB(result) ^ CF */
    cmp  x6, 1
    b.ne .Lsh_flags_\@
    ubfx x16, x15, (\bits - 1), 1
    eor  x16, x16, x14
    b    .Lsh_flags_\@
.Lsh_shr_\@:
    lsr  x15, x13, x6
    mov  x14, 0
    cmp  x6, \bits
    b.hi 2f
    sub  x4, x6, 1
    lsr  x14, x13, x4
    and  x14, x14, 1
2:  mov  x16, 0
    cmp  x6, 1
    b.ne .Lsh_flags_\@
    ubfx x16, x13, (\bits - 1), 1    /* OF: the original MSB */
    b    .Lsh_flags_\@
.Lsh_sar_\@:
.if \bits < 64
    sbfx x4, x13, 0, \bits
.else
    mov  x4, x13
.endif
    asr  x15, x4, x6
.if \bits < 64
    and  x15, x15, ((1 << \bits) - 1)
.endif
    sub  x5, x6, 1
    asr  x14, x4, x5                 /* the last bit out; the sign past the width */
    and  x14, x14, 1
    mov  x16, 0
.Lsh_flags_\@:
    mov  x11, \bits
    mov  x5, x14                     /* the logic-flag macro uses w16/w17 */
    mov  x12, x16
    amd64_cached_set_logic_flags x15, x11   /* CF OF AF 0, ZF SF PF */
    strb w5, [_cpu, CPU_cf]
    strb w12, [_cpu, CPU_of]
    ldr  w16, [_cpu, CPU_eflags]
    orr  w16, w16, w5
    orr  w16, w16, w12, lsl 11
    str  w16, [_cpu, CPU_eflags]
    b    .Lsh_done_\@
.Lsh_rot_\@:                         /* ROL / ROR on the width */
    and  x4, x6, (\bits - 1)         /* count % bits */
    mov  x15, x13
    cbz  x4, 3f
    mov  x5, \bits
    sub  x5, x5, x4
    cbnz x7, 31f
    lsl  x10, x13, x4                /* ROL */
    lsr  x11, x13, x5
    b    32f
31: lsr  x10, x13, x4                /* ROR */
    lsl  x11, x13, x5
32: orr  x15, x10, x11
.if \bits < 64
    and  x15, x15, ((1 << \bits) - 1)
.endif
3:  cbnz x7, 33f
    and  x14, x15, 1                 /* ROL: CF = LSB; OF = MSB ^ CF */
    ubfx x16, x15, (\bits - 1), 1
    eor  x16, x16, x14
    b    34f
33: ubfx x14, x15, (\bits - 1), 1    /* ROR: CF = MSB; OF = MSB ^ MSB-1 */
    ubfx x16, x15, (\bits - 2), 1
    eor  x16, x16, x14
34: strb w14, [_cpu, CPU_cf]
    ldr  w17, [_cpu, CPU_eflags]
    bic  w17, w17, 1
    orr  w17, w17, w14
    cmp  x6, 1
    b.ne 35f
    strb w16, [_cpu, CPU_of]
    bic  w17, w17, (1 << 11)
    orr  w17, w17, w16, lsl 11
35: str  w17, [_cpu, CPU_eflags]
    b    .Lsh_done_\@
.Lsh_rc_\@:                          /* RCL / RCR, a bit at a time */
    mov  x4, x6
.if \bits < 32
    mov  x5, (\bits + 1)
    udiv x10, x4, x5
    msub x4, x10, x5, x4             /* count mod bits+1 */
.endif
    mov  x15, x13
    cbz  x4, .Lsh_done_\@            /* nothing changes */
    ldrb w14, [_cpu, CPU_cf]
    cmp  w14, 0
    cset x14, ne
    mov  x10, x4
4:  cmp  x7, 2
    b.ne 41f
    ubfx x11, x15, (\bits - 1), 1    /* RCL: the MSB goes out */
    lsl  x15, x15, 1
    orr  x15, x15, x14
    b    42f
41: and  x11, x15, 1                 /* RCR: the LSB goes out */
    lsr  x15, x15, 1
    orr  x15, x15, x14, lsl #(\bits - 1)
42:
.if \bits < 64
    and  x15, x15, ((1 << \bits) - 1)
.endif
    mov  x14, x11
    subs x10, x10, 1
    b.ne 4b
    strb w14, [_cpu, CPU_cf]
    ldr  w17, [_cpu, CPU_eflags]
    bic  w17, w17, 1
    orr  w17, w17, w14
    cmp  x4, 1
    b.ne 45f
    cmp  x7, 2
    b.ne 43f
    ubfx x16, x15, (\bits - 1), 1    /* RCL OF: MSB ^ CF */
    eor  x16, x16, x14
    b    44f
43: ubfx x16, x15, (\bits - 1), 1    /* RCR OF: MSB ^ MSB-1 */
    ubfx x11, x15, (\bits - 2), 1
    eor  x16, x16, x11
44: strb w16, [_cpu, CPU_of]
    bic  w17, w17, (1 << 11)
    orr  w17, w17, w16, lsl 11
45: str  w17, [_cpu, CPU_eflags]
.Lsh_done_\@:
.endm

# The count: packed bits 3-4 = 0 one, 1 CL, 2 the imm in bits 8-15; masked.
# -> x6. Clobbers x8.
.macro amd64_shift_count bits
    ubfx x8, x24, 3, 2
    mov  x6, 1
    cbz  x8, .Lsc_m_\@
    ubfx x6, x24, 8, 8
    cmp  x8, 2
    b.eq .Lsc_m_\@
    ldrb w6, [_cpu, CPU_amd64_regs + 8]   /* CL */
.Lsc_m_\@:
.if \bits == 64
    and  x6, x6, 63
.else
    and  x6, x6, 31
.endif
.endm

# Memory: [meta][disp][next_ip][packed], packed = subop (0-2) | count kind
# (3-4) | imm count (8-15). gret 4.
.macro amd64_gen_shift_mem name, vb, bits
.gadget amd64_\name
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    amd64_vmem_addr
    ldr  x24, [_ip, 24]
    mov  x0, x9
    amd64_vread_prep \vb, \name\()_r
    amd64_alu_load \vb, 13
    amd64_shift_count \bits
    cbz  x6, 9f                      /* a masked count of 0: nothing */
    amd64_shift_core \bits
    mov  x10, x15
    mov  x9, x0
    amd64_vwrite_prep \vb, \name\()_w
    amd64_alu_store \vb, 10
    amd64_vwrite_done \vb, \name\()_w
9:  gret 4
    amd64_vread_bullshit \vb, \name\()_r
    amd64_vwrite_bullshit \vb, \name\()_w
.endm
amd64_gen_shift_mem shift_mem8, 1, 8
amd64_gen_shift_mem shift_mem16, 2, 16
amd64_gen_shift_mem shift_mem32, 4, 32
amd64_gen_shift_mem shift_mem64, 8, 64

# Register: [packed], packed as above | reg (16-19) | AH..BH (20). gret 1.
.macro amd64_gen_shift_rr name, vb, bits
.gadget amd64_\name
    ldr  x24, [_ip]
    add  x14, _cpu, CPU_amd64_regs
    ubfx x12, x24, 16, 4
    ldr  x13, [x14, x12, lsl 3]
.if \vb == 1
    tbz  x24, 20, 1f
    lsr  x13, x13, 8
1:  and  x13, x13, 0xff
.elseif \vb == 2
    and  x13, x13, 0xffff
.elseif \vb == 4
    mov  w13, w13
.endif
    amd64_shift_count \bits
    mov  x15, x13                    /* a count of 0: the value, unchanged -- */
    cbz  x6, 8f                      /* but a 32-bit register is still written */
    amd64_shift_core \bits
8:  add  x14, _cpu, CPU_amd64_regs
    ubfx x12, x24, 16, 4
    ldr  x11, [x14, x12, lsl 3]
.if \vb == 1
    tbz  x24, 20, 2f
    bfi  x11, x15, 8, 8
    b    3f
2:  bfi  x11, x15, 0, 8
3:
.elseif \vb == 2
    bfi  x11, x15, 0, 16
.elseif \vb == 4
    mov  w11, w15
.else
    mov  x11, x15
.endif
    str  x11, [x14, x12, lsl 3]
9:  gret 1
.endm
amd64_gen_shift_rr shift_rr8, 1, 8
amd64_gen_shift_rr shift_rr16, 2, 16
amd64_gen_shift_rr shift_rr32, 4, 32
amd64_gen_shift_rr shift_rr64, 8, 64

# SHLD/SHRD (0F A4/A5/AC/AD): dst <<= count shifting in the top of src, or
# >>= shifting in the bottom of it. The count (imm8 or CL) is masked to 5
# bits, 6 at 64; a masked count of 0 changes nothing (a 32-bit register is
# still written, zero-extended, as on hardware). Past 16 at 16 bits the SDM
# leaves the result undefined. CF is the last bit shifted out of dst; OF
# (defined for a count of 1) is MSB(result) ^ MSB(dst); SF ZF PF from the
# result; AF undefined (0 here).
# In: x13 = dst, x10 = src (both masked to the width), x6 = count (nonzero),
# x24 bit 0 = SHLD. Out: x15 = the result. Clobbers x4 x5 x11 x12 x14 x16 x17.
.macro amd64_dsh_core bits
    tbz  x24, 0, .Ldsh_r_\@
.if \bits == 64
    mov  x4, 64
    sub  x4, x4, x6
    lsl  x15, x13, x6
    lsr  x5, x10, x4
    orr  x15, x15, x5
    lsr  x14, x13, x4                /* CF: dst bit (64 - count) */
.else
    lsl  x4, x13, (64 - \bits)       /* dst:src at the top of a 64-bit word */
    orr  x4, x4, x10, lsl #(64 - 2 * \bits)
    lsl  x15, x4, x6
    lsr  x15, x15, (64 - \bits)
    mov  x5, 64
    sub  x5, x5, x6
    lsr  x14, x4, x5                 /* CF: the last bit out of the top */
.endif
    b    .Ldsh_f_\@
.Ldsh_r_\@:
.if \bits == 64
    mov  x4, 64
    sub  x4, x4, x6
    lsr  x15, x13, x6
    lsl  x5, x10, x4
    orr  x15, x15, x5
    sub  x4, x6, 1
    lsr  x14, x13, x4                /* CF: dst bit (count - 1) */
.else
    orr  x4, x13, x10, lsl \bits     /* src:dst */
    lsr  x15, x4, x6
    and  x15, x15, ((1 << \bits) - 1)
    sub  x5, x6, 1
    lsr  x14, x4, x5
.endif
.Ldsh_f_\@:
    and  x14, x14, 1
    eor  x16, x15, x13
    ubfx x16, x16, (\bits - 1), 1    /* OF: the sign changed */
    mov  x11, \bits
    mov  x5, x14                     /* the logic-flag macro uses w16/w17 */
    mov  x12, x16
    amd64_cached_set_logic_flags x15, x11   /* CF OF AF 0, ZF SF PF */
    strb w5, [_cpu, CPU_cf]
    strb w12, [_cpu, CPU_of]
    ldr  w16, [_cpu, CPU_eflags]
    orr  w16, w16, w5
    orr  w16, w16, w12, lsl 11
    str  w16, [_cpu, CPU_eflags]
.endm

# The count: packed bit 1 = CL, else the imm8 in bits 8-15; masked. -> x6.
.macro amd64_dsh_count bits
    ubfx x6, x24, 8, 8
    tbz  x24, 1, .Ldc_\@
    ldrb w6, [_cpu, CPU_amd64_regs + 8]   /* CL */
.Ldc_\@:
.if \bits == 64
    and  x6, x6, 63
.else
    and  x6, x6, 31
.endif
.endm

.macro amd64_dsh_mask vb, n
.if \vb == 2
    and  x\n, x\n, 0xffff
.elseif \vb == 4
    mov  w\n, w\n
.endif
.endm

# Memory destination: [meta][disp][next_ip][packed], packed = SHLD (0) | CL (1) |
# src register (4-7) | imm8 (8-15). gret 4.
.macro amd64_gen_dsh_mem name, vb, bits
.gadget amd64_\name
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    amd64_vmem_addr
    ldr  x24, [_ip, 24]
    ubfx x12, x24, 4, 4
    add  x14, _cpu, CPU_amd64_regs
    ldr  x25, [x14, x12, lsl 3]      /* src: x25 survives the preps */
    mov  x0, x9
    amd64_vread_prep \vb, \name\()_r
    amd64_alu_load \vb, 13
    amd64_dsh_count \bits
    cbz  x6, 9f                      /* a masked count of 0: nothing */
    mov  x10, x25
    amd64_dsh_mask \vb, 10
    amd64_dsh_core \bits
    mov  x10, x15
    mov  x9, x0
    amd64_vwrite_prep \vb, \name\()_w
    amd64_alu_store \vb, 10
    amd64_vwrite_done \vb, \name\()_w
9:  gret 4
    amd64_vread_bullshit \vb, \name\()_r
    amd64_vwrite_bullshit \vb, \name\()_w
.endm
amd64_gen_dsh_mem dsh_mem16, 2, 16
amd64_gen_dsh_mem dsh_mem32, 4, 32
amd64_gen_dsh_mem dsh_mem64, 8, 64

# Register destination: [packed], packed as above | dst register (16-19). gret 1.
.macro amd64_gen_dsh_rr name, vb, bits
.gadget amd64_\name
    ldr  x24, [_ip]
    add  x14, _cpu, CPU_amd64_regs
    ubfx x12, x24, 16, 4
    ldr  x13, [x14, x12, lsl 3]
    ubfx x11, x24, 4, 4
    ldr  x10, [x14, x11, lsl 3]
    amd64_dsh_mask \vb, 13
    amd64_dsh_mask \vb, 10
    amd64_dsh_count \bits
    mov  x15, x13                    /* a count of 0: the value, unchanged -- */
    cbz  x6, 8f                      /* but a 32-bit register is still written */
    amd64_dsh_core \bits
8:  add  x14, _cpu, CPU_amd64_regs
    ubfx x12, x24, 16, 4
.if \vb == 2
    ldr  x11, [x14, x12, lsl 3]
    bfi  x11, x15, 0, 16
    str  x11, [x14, x12, lsl 3]
.else
    str  x15, [x14, x12, lsl 3]      /* 32-bit: x15 is zero-extended already */
.endif
    gret 1
.endm
amd64_gen_dsh_rr dsh_rr16, 2, 16
amd64_gen_dsh_rr dsh_rr32, 4, 32
amd64_gen_dsh_rr dsh_rr64, 8, 64

amd64_gen_imm_arith imm_arith32, 4, 31
amd64_gen_imm_arith imm_arith64, 8, 63

# amd64 imm-to-mem logic RMW: [mem] <op>= imm for OR (group /1), AND (/4), XOR (/6),
# mod!=3, 32/64. Like imm_arith but logic flags (CF=OF=0, ZF/SF/PF from result). The
# sign-extended immediate is operand 3; group is operand 4. TEST imm (0xf6/0xf7) is a
# different opcode and bridges. Operands: meta/disp/next_ip/imm/group -> gret 5.
.macro amd64_gen_imm_logic name, vbytes, ssize
.gadget amd64_\name
    ldr  x8, [_ip]                   /* meta */
    ldr  x9, [_ip, 8]                /* disp */
    amd64_vmem_addr                  /* x9 = guest effective address */
    mov  x0, x9                      /* x0 = guest addr (survives both preps) */
    amd64_vread_prep \vbytes, \name\()_r   /* x9 = host read addr */
.if \vbytes == 4
    ldr  w13, [x9]                   /* lhs = [mem] (32-bit) */
.else
    ldr  x13, [x9]                   /* lhs = [mem] (64-bit) */
.endif
    ldr  x10, [_ip, 24]             /* rhs = immediate (sign-extended) */
    ldr  x9, [_ip, 32]             /* group: 1=or, 4=and, 6=xor */
    cmp  x9, 4
    b.eq 1f
    cmp  x9, 6
    b.eq 2f
    orr  x15, x13, x10               /* OR (group /1) */
    b    3f
1:  and  x15, x13, x10
    b    3f
2:  eor  x15, x13, x10
3:
.if \vbytes == 4
    mov  w15, w15
.endif
    mov  x11, \ssize
    amd64_cached_set_logic_flags x15, x11
    mov  x10, x15                    /* result -> x10 (survives the write prep) */
    mov  x9, x0                      /* guest addr -> x9 */
    amd64_vwrite_prep \vbytes, \name\()_w
.if \vbytes == 4
    str  w10, [x9]
.else
    str  x10, [x9]
.endif
    amd64_vwrite_done \vbytes, \name\()_w
    gret 5
    amd64_vread_bullshit \vbytes, \name\()_r
    amd64_vwrite_bullshit \vbytes, \name\()_w
.endm

amd64_gen_imm_logic imm_logic32, 4, 32
amd64_gen_imm_logic imm_logic64, 8, 64

# amd64 CMP [mem], imm (group /7, 0x81/0x83), mod!=3, 32/64 -- the no-store sibling of
# imm_arith: read [mem]=lhs, subtract the immediate, set eager flags, write NOTHING.
# This is the SHA-512/crypt hot-loop bottleneck that was bridged (block-ending) for
# glibc/musl startup safety; the flags use the SAME amd64_cached_set_addsub_flags as
# native CMP 0x3b/0x39, so they are bit-exact by construction. Single vread (no dual
# prep, no write). Operands: meta(0)/disp(8)/next_ip(16)/imm(24) -> gret 4.
.macro amd64_gen_imm_cmp name, vbytes, sbit
.gadget amd64_\name
    ldr  x8, [_ip]                   /* meta */
    ldr  x9, [_ip, 8]                /* disp */
    amd64_vmem_addr                  /* x9 = effective address */
    amd64_vread_prep \vbytes, \name  /* x9 = host addr of [mem] */
.if \vbytes == 2
    /* 16-bit: compare in a 32-bit register on zero-extended halves; CF is the
       unsigned borrow out of bit 16 and OF the signed overflow at bit 15, both
       read from the 16-bit result rather than the host's 32-bit flags. */
    ldrh w13, [x9]                   /* lhs = [mem] (16-bit, zero-extended) */
    ldr  x10, [_ip, 24]             /* rhs = immediate (sign-extended) */
    and  w10, w10, 0xffff
    sub  w15, w13, w10
    lsr  w14, w15, 16               /* borrow: bit 16 set iff lhs < rhs */
    and  w14, w14, 1
    eor  w16, w13, w10               /* OF = (lhs^rhs) & (lhs^res), bit 15 */
    eor  w17, w13, w15
    and  w16, w16, w17
    lsr  w16, w16, 15
    and  w16, w16, 1
    and  w15, w15, 0xffff
.elseif \vbytes == 4
    ldr  w13, [x9]                   /* lhs = [mem] (32-bit) */
    ldr  x10, [_ip, 24]             /* rhs = immediate */
    mov  w13, w13
    mov  w10, w10
    subs w15, w13, w10
    cset w14, cc
    cset w16, vs
    mov  w15, w15
.else
    ldr  x13, [x9]                   /* lhs = [mem] (64-bit) */
    ldr  x10, [_ip, 24]             /* rhs = immediate */
    subs x15, x13, x10
    cset w14, cc
    cset w16, vs
.endif
    amd64_cached_set_addsub_flags \sbit
    gret 4
    amd64_vread_bullshit \vbytes, \name
.endm

amd64_gen_imm_cmp imm_cmp32, 4, 31
# 16-bit CMP [mem], imm (66 83 /7): 28 bridge sites, purely the operand-size form.
amd64_gen_imm_cmp imm_cmp16, 2, 15
amd64_gen_imm_cmp imm_cmp64, 8, 63

# amd64 CMP byte [mem], imm8 (0x80 /7), mod!=3 -- the byte sibling of imm_cmp, the
# other crypt hot-loop block-bridge. No store. 8-bit flags: operands are zero-extended
# to 0-255 so the 32-bit subs borrow == the 8-bit borrow (CF via cc); the 8-bit signed
# overflow (OF) is computed by hand ((lhs^rhs)&(lhs^res)&0x80); AF/ZF/SF/PF come from
# amd64_cached_set_addsub_flags with the 8-bit result and sign_bit=7. No encoded8
# concern -- the operand is memory. Operands: meta(0)/disp(8)/next_ip(16)/imm(24).
.gadget amd64_imm_cmp8
    ldr  x8, [_ip]                   /* meta */
    ldr  x9, [_ip, 8]                /* disp */
    amd64_vmem_addr                  /* x9 = effective address */
    amd64_vread_prep 1, imm_cmp8     /* x9 = host addr of the byte */
    ldrb w13, [x9]                   /* lhs = [mem] byte (0-255) */
    ldr  x10, [_ip, 24]             /* imm */
    and  w10, w10, 0xff              /* rhs = imm & 0xff */
    subs w15, w13, w10               /* 0-255 operands: 32-bit borrow == 8-bit borrow */
    cset w14, cc                     /* CF (borrow) */
    and  w15, w15, 0xff              /* 8-bit result */
    eor  w12, w13, w10               /* OF = (lhs^rhs) & (lhs^res) & 0x80 */
    eor  w17, w13, w15
    and  w12, w12, w17
    tst  w12, 0x80
    cset w16, ne                     /* OF */
    amd64_cached_set_addsub_flags 7
    gret 4
    amd64_vread_bullshit 1, imm_cmp8

# =====================================================================================
# amd64 BYTE (8-bit) integer ALU on REGISTER operands (mod==3) -- the cc1 hot path.
# These were the top remaining C bridges for cc1 (op byte r/m8,imm8 = amd64_jit_reg_imm_op
# /modrm_imm; byte op r/m8,r8 = amd64_jit_reg_reg_op). All are CACHE-AWARE (no flush):
# they read the FULL 64-bit register (cache-aware via amd64_read_low_or_mem), operate on
# one byte, and write the full modified value back (amd64_write_low_or_mem size 64). This
# uniformly handles low bytes (AL-DIL, R8B-R15B) AND the legacy high bytes (AH/CH/DH/BH):
# gen.c passes a corrected register id plus a is_high flag (bit shift 0 or 8). Flags are
# computed at 8-bit width: SUB/CMP CF via the 0-255-operand borrow (cc), ADD CF via bit 8,
# OF by hand at bit 7, and ZF/SF/PF/AF via amd64_cached_set_addsub_flags with sign_bit=7
# (the same byte rule proven by gadget_amd64_imm_cmp8). Register map (non-prep gadgets, so
# x8-x17 are free except where the called macros clobber): x8=packed operand word (stable),
# x9=full dst reg, x10=rhs byte, x13=lhs byte, x14=CF, x15=result byte, x16=OF; the macros
# clobber x11/x12/x17. ADC/SBB (groups 2/3) keep bridging (rare in compiler code).

# Compute one byte ALU op. Reads: x8 bits0-3 = group (0=ADD,1=OR,4=AND,5=SUB,6=XOR,7=CMP),
# x13 = lhs byte (0-255), x10 = rhs byte (0-255). Produces: x15 = 8-bit result and sets the
# eager flags (addsub flags for ADD/SUB/CMP, logic flags for OR/AND/XOR). Does NOT write the
# result back to the register. Clobbers x11/x12/x14/x15/x16/x17 (x8/x9/x10/x13 preserved).
.macro amd64_byte_compute
    and  x12, x8, 0xf                /* group */
    cmp  x12, 1
    b.eq .Lbc_or_\@
    cmp  x12, 4
    b.eq .Lbc_and_\@
    cmp  x12, 6
    b.eq .Lbc_xor_\@
    cmp  x12, 0
    b.eq .Lbc_add_\@
    /* SUB (5) / CMP (7): 0-255 operands so the 32-bit borrow == the 8-bit borrow */
    subs w15, w13, w10
    cset w14, cc                     /* CF (borrow) */
    and  w15, w15, 0xff              /* 8-bit result */
    eor  w12, w13, w10               /* OF(sub) = (lhs^rhs) & (lhs^res) & 0x80 */
    eor  w17, w13, w15
    and  w12, w12, w17
    tst  w12, 0x80
    cset w16, ne
    amd64_cached_set_addsub_flags 7
    b    .Lbc_done_\@
.Lbc_add_\@:
    add  w15, w13, w10               /* 0-510 */
    ubfx w14, w15, 8, 1              /* CF = carry out of bit 7 */
    and  w15, w15, 0xff
    eor  w12, w13, w10               /* OF(add) = ~(lhs^rhs) & (lhs^res) & 0x80 */
    eor  w17, w13, w15
    bic  w12, w17, w12               /* (lhs^res) & ~(lhs^rhs) */
    tst  w12, 0x80
    cset w16, ne
    amd64_cached_set_addsub_flags 7
    b    .Lbc_done_\@
.Lbc_or_\@:  orr  w15, w13, w10
    b    .Lbc_logic_\@
.Lbc_and_\@: and  w15, w13, w10
    b    .Lbc_logic_\@
.Lbc_xor_\@: eor  w15, w13, w10
.Lbc_logic_\@:
    and  w15, w15, 0xff
    mov  x11, 8
    amd64_cached_set_logic_flags x15, x11
.Lbc_done_\@:
.endm

# Insert the 8-bit result (x15) into the full register value (x9) at the operand's byte
# offset (is_high at x8 bit 8 -> shift 0 or 8), preserving all other bits, then write the
# full value back cache-aware. dst register id at x8 bits 4-7. Clobbers x11/x12/x13/x15/x17.
.macro amd64_byte_writeback
    ubfx x11, x8, 8, 1               /* is_high */
    lsl  x11, x11, 3                 /* byte shift: 0 or 8 */
    mov  x12, 0xff
    lsl  x12, x12, x11               /* mask for the target byte */
    bic  x9, x9, x12                 /* clear target byte */
    lsl  x17, x15, x11               /* result << shift */
    and  x17, x17, x12
    orr  x9, x9, x17                 /* x9 = full reg with the byte replaced */
    ubfx x13, x8, 4, 4               /* dst register id */
    mov  x10, 64                     /* write_low_or_mem needs size in a register */
    amd64_write_low_or_mem x13, x9, x10
.endm

# byte op r/m8, imm8 (0x80 /0,/1,/4,/5,/6,/7), mod==3. Operands: packed(0)/imm(8).
# packed: group(0-3) | dst_id(4-7) | is_high(8).  gret 2.
.gadget amd64_byte_alu_reg_imm
    ldr  x8, [_ip]
    ldr  x10, [_ip, 8]
    and  w10, w10, 0xff              /* rhs = imm8 */
    ubfx x11, x8, 4, 4               /* dst id */
    amd64_read_low_or_mem x11, x9    /* x9 = full dst register */
    ubfx x11, x8, 8, 1               /* is_high */
    lsl  x11, x11, 3
    lsr  x13, x9, x11
    and  x13, x13, 0xff              /* lhs byte */
    amd64_byte_compute
    and  x12, x8, 0xf
    cmp  x12, 7                      /* CMP: flags only, no write */
    b.eq 1f
    amd64_byte_writeback
1:  gret 2

# byte op r/m8, r8 / r8, r/m8 (0x00/02/08/0a/20/22/28/2a/30/32/38/3a) + TEST r/m8,r8 (0x84),
# mod==3. gen.c resolves the direction to dst (lhs) and src (rhs). Operands: packed(0).
# packed: op(0-3) | dst_id(4-7) | dst_high(8) | src_id(9-12) | src_high(13) | nowrite(16).
# op uses the group code (0/1/4/5/6/7); TEST is encoded as op=4 (AND) with nowrite set. gret 1.
.gadget amd64_byte_alu_reg_reg
    ldr  x8, [_ip]
    ubfx x11, x8, 9, 4               /* src id */
    amd64_read_low_or_mem x11, x10   /* x10 = full src */
    ubfx x11, x8, 13, 1              /* src_high */
    lsl  x11, x11, 3
    lsr  x10, x10, x11
    and  x10, x10, 0xff              /* rhs byte */
    ubfx x11, x8, 4, 4               /* dst id */
    amd64_read_low_or_mem x11, x9    /* x9 = full dst */
    ubfx x11, x8, 8, 1               /* dst_high */
    lsl  x11, x11, 3
    lsr  x13, x9, x11
    and  x13, x13, 0xff              /* lhs byte */
    amd64_byte_compute
    and  x12, x8, 0xf
    cmp  x12, 7                      /* CMP: no write */
    b.eq 1f
    tbnz x8, 16, 1f                  /* TEST: no write */
    amd64_byte_writeback
1:  gret 1

# TEST r/m8, imm8 (0xf6 /0,/1), mod==3 -- the high-byte/r8-r15 case (low-byte regs are
# already handled natively by gadget_amd64_cached_test_reg_imm). AND, logic flags, no write.
# Operands: packed(0)/imm(8). packed: rm_id(0-3 corrected) | is_high(4). gret 2.
.gadget amd64_byte_test_imm_reg
    ldr  x8, [_ip]
    ldr  x10, [_ip, 8]
    and  w10, w10, 0xff
    ubfx x11, x8, 0, 4
    amd64_read_low_or_mem x11, x9
    ubfx x11, x8, 4, 1
    lsl  x11, x11, 3
    lsr  x13, x9, x11
    and  x13, x13, 0xff
    and  w15, w13, w10
    and  w15, w15, 0xff
    mov  x11, 8
    amd64_cached_set_logic_flags x15, x11
    gret 2

# NOT (0xf6 /2) / NEG (0xf6 /3) r/m8, mod==3. packed: rm_id(0-3) | is_high(4) | is_neg(5).
# NOT: result = ~src, no flags. NEG: result = 0 - src, sub flags (lhs=0, rhs=src). gret 1.
.gadget amd64_byte_grp3
    ldr  x8, [_ip]
    ubfx x11, x8, 0, 4
    amd64_read_low_or_mem x11, x9    /* x9 = full reg */
    ubfx x11, x8, 4, 1
    lsl  x11, x11, 3
    lsr  x13, x9, x11
    and  x13, x13, 0xff              /* src byte */
    tbnz x8, 5, .Lbg_neg
    mvn  w15, w13                    /* NOT: ~src */
    and  w15, w15, 0xff
    b    .Lbg_write
.Lbg_neg:
    neg  w15, w13                    /* NEG: 0 - src */
    and  w15, w15, 0xff
    mov  w10, w13                    /* rhs = src (for AF = 0^src^res bit4) */
    subs wzr, wzr, w13               /* CF: 0<src borrows */
    cset w14, cc                     /* CF = (src != 0) */
    and  w12, w13, w15               /* OF(sub,lhs=0) = (src & res) & 0x80 */
    tst  w12, 0x80
    cset w16, ne
    mov  x13, 0                      /* lhs = 0 for the macro's AF */
    amd64_cached_set_addsub_flags 7
.Lbg_write:
    /* writeback at the byte offset, preserving the rest of the register */
    ubfx x11, x8, 4, 1
    lsl  x11, x11, 3
    mov  x12, 0xff
    lsl  x12, x12, x11
    bic  x9, x9, x12
    lsl  x17, x15, x11
    and  x17, x17, x12
    orr  x9, x9, x17
    ubfx x13, x8, 0, 4
    mov  x10, 64
    amd64_write_low_or_mem x13, x9, x10
    gret 1

# NOT (0xf7 /2) / NEG (0xf7 /3) reg, 32/64-bit, mod==3 -- the wider siblings of
# amd64_byte_grp3 (the byte 0xf6 form). NOT = ~src (no flags); NEG = 0 - src with sub
# flags matching the interp amd64_set_sub_flags(0, src, res): CF=(src!=0), OF=(src&res)
# at the sign bit, ZF/SF/PF/AF via cached_set_addsub_flags (lhs=0, rhs=src). Cache-aware
# (read/write_low_or_mem). 16-bit + memory operands keep bridging. packed: rm(0-3) |
# is_neg(4). The 32-bit result is upper-zeroed (x86 32-bit ops clear bits 32-63).
.macro amd64_gen_negnot name, vbytes
.gadget amd64_\name
    ldr  x8, [_ip]
    ubfx x11, x8, 0, 4               /* rm reg id */
    amd64_read_low_or_mem x11, x9    /* x9 = full reg */
.if \vbytes == 8
    mov  x13, x9                     /* src = full 64 */
    tbnz x8, 4, .Lnn_neg_\name
    mvn  x15, x13                    /* NOT: ~src */
    b    .Lnn_write_\name
.Lnn_neg_\name:
    neg  x15, x13                    /* NEG: 0 - src */
    mov  x10, x13                    /* rhs = src (for AF = 0^src^res bit4) */
    subs xzr, xzr, x13               /* CF = (src != 0) */
    cset w14, cc
    and  x12, x13, x15               /* OF(sub,lhs=0) = (src & res) & sign */
    tst  x12, (1 << 63)
    cset w16, ne
    mov  x13, 0                      /* lhs = 0 */
    amd64_cached_set_addsub_flags 63
.else
    mov  w13, w9                     /* src = low 32 */
    tbnz x8, 4, .Lnn_neg_\name
    mvn  w15, w13                    /* NOT (w-op zero-extends x15) */
    b    .Lnn_write_\name
.Lnn_neg_\name:
    neg  w15, w13                    /* NEG (w-op zero-extends x15) */
    mov  w10, w13                    /* rhs = src */
    subs wzr, wzr, w13               /* CF = (src != 0) */
    cset w14, cc
    and  w12, w13, w15               /* OF = (src & res) & sign */
    tst  w12, 0x80000000
    cset w16, ne
    mov  x13, 0                      /* lhs = 0 */
    amd64_cached_set_addsub_flags 31
.endif
.Lnn_write_\name:
    /* write the full result: 64-bit = the value; 32-bit = upper already zeroed */
    ubfx x13, x8, 0, 4
    mov  x10, 64
    amd64_write_low_or_mem x13, x15, x10
    gret 1
.endm
amd64_gen_negnot negnot32, 4
amd64_gen_negnot negnot64, 8

# =====================================================================================
# amd64 BYTE (8-bit) ALU on MEMORY operands (mod!=3) -- the other half of the cc1 byte
# hot path. TEST byte [mem],imm8 (0xf6 /0) was the #2 byte bridge (testb $imm,mem); the
# op byte [mem],imm8 (0x80 /0,/1,/4,/5,/6) RMW forms bridged via amd64_jit_modrm_imm.
# These reuse the proven TLB machinery (amd64_vmem_addr + vread_prep/vwrite_prep) and the
# 8-bit flag rule (amd64_cached_set_addsub_flags 7 / set_logic_flags), exactly like
# gadget_amd64_imm_cmp8 / gadget_amd64_opstore_arith. A 1-byte access never crosses a
# page. CMP (/7) stays on imm_cmp8; ADC/SBB (/2,/3) bridge.

# TEST byte [mem], imm8 (0xf6 /0,/1), mod!=3: AND for flags, no store, logic flag rule.
# Operands: meta(0)/disp(8)/next_ip(16)/imm(24). gret 4.
.gadget amd64_byte_test_imm_mem
    ldr  x8, [_ip]                   /* meta */
    ldr  x9, [_ip, 8]                /* disp */
    amd64_vmem_addr                  /* x9 = effective address */
    amd64_vread_prep 1, byte_test_imm_mem
    ldrb w13, [x9]                   /* lhs = [mem] byte */
    ldr  x10, [_ip, 24]             /* imm */
    and  w15, w13, w10               /* AND (rhs already < 256 via imm8) */
    and  w15, w15, 0xff
    mov  x11, 8
    amd64_cached_set_logic_flags x15, x11
    gret 4
    amd64_vread_bullshit 1, byte_test_imm_mem

# op byte [mem], imm8 (0x80 /0,/1,/4,/5,/6 = ADD/OR/AND/SUB/XOR), mod!=3. RMW: vread the
# old byte, run amd64_byte_compute (group from x8 bits0-3 -- reloaded from the group
# operand), vwrite the result. Dual-prep on one address: guest addr stays in x0 across
# both preps, result -> x10 before the write (only x0-x13 survive a prep slow-path bl).
# Operands: meta(0)/disp(8)/next_ip(16)/imm(24)/group(32). gret 5.
.gadget amd64_byte_imm_mem
    ldr  x8, [_ip]                   /* meta */
    ldr  x9, [_ip, 8]                /* disp */
    amd64_vmem_addr                  /* x9 = guest effective address */
    mov  x0, x9                      /* x0 = guest addr (survives both preps) */
    amd64_vread_prep 1, byte_imm_mem_r
    ldrb w13, [x9]                   /* lhs = [mem] byte */
    ldr  x10, [_ip, 24]             /* rhs = imm8 */
    and  w10, w10, 0xff
    ldr  x8, [_ip, 32]              /* x8 = group (for amd64_byte_compute) */
    amd64_byte_compute               /* x15 = result byte + eager flags */
    mov  x10, x15                    /* result -> x10 (survives the write prep) */
    mov  x9, x0                      /* guest addr -> x9 */
    amd64_vwrite_prep 1, byte_imm_mem_w
    strb w10, [x9]
    amd64_vwrite_done 1, byte_imm_mem_w
    gret 5
    amd64_vread_bullshit 1, byte_imm_mem_r
    amd64_vwrite_bullshit 1, byte_imm_mem_w

# =====================================================================================
# SAHF (0x9e) / LAHF (0x9f) -- AH<->flags. Neither was implemented in the amd64 engine
# (interp lacked case 0x9e/0x9f; the JIT had no gadget), so guest sahf/lahf SIGILL'd.
# Flush-style (gen.c flushes the reg cache first, like amd64_gen_rotate_count): operate
# on rax (regs[0]) and the eager flags (CPU_eflags sf/zf/af/pf bits + the CPU_cf byte)
# directly in CPU state. No memory access, no inline operands (gret 0).

# LAHF: AH = SF:ZF:0:AF:0:PF:1:CF (= the low byte of a collapsed EFLAGS). The amd64 ALU
# gadgets keep CPU_eflags' sf/zf/af/pf bits and the CPU_cf byte coherent (eager), so AH
# is (eflags & {sf,zf,af,pf}) | (CF<<0) | (1<<1). The flags themselves are unchanged.
.gadget amd64_lahf
    ldr  w8, [_cpu, CPU_eflags]
    mov  w12, (SF_FLAG | ZF_FLAG | AF_FLAG | PF_FLAG)   /* 0xd4 (not a valid AND imm) */
    and  w8, w8, w12
    ldrb w9, [_cpu, CPU_cf]
    and  w9, w9, 1
    orr  w8, w8, w9                  /* bit0 = CF */
    orr  w8, w8, 2                   /* bit1 = 1 (reserved, always reads as 1) */
    ldr  x10, [_cpu, CPU_amd64_regs] /* rax */
    bfi  x10, x8, 8, 8               /* AH = bits 8-15; rest of rax preserved */
    str  x10, [_cpu, CPU_amd64_regs]
    gret 0

# SAHF: load SF/ZF/AF/PF/CF from AH; OF (CPU_of + eflags bit 11) and the rest preserved.
# Clear flags_res so the eager bitfields stay authoritative (a prior bridged interp op
# may have left a lazy-flag valid bit set).
.gadget amd64_sahf
    ldr  x10, [_cpu, CPU_amd64_regs] /* rax */
    ubfx w8, w10, 8, 8               /* AH */
    and  w9, w8, 1
    strb w9, [_cpu, CPU_cf]          /* eager CF = AH bit0 */
    ldr  w11, [_cpu, CPU_eflags]
    mov  w12, (1 | PF_FLAG | AF_FLAG | ZF_FLAG | SF_FLAG)   /* 0xd5: bits 0,2,4,6,7 */
    bic  w11, w11, w12
    and  w8, w8, w12                 /* AH's bits {0,2,4,6,7} (CF also into eflags bit0) */
    orr  w11, w11, w8
    str  w11, [_cpu, CPU_eflags]
    strb wzr, [_cpu, CPU_flags_res]
    gret 0

# =====================================================================================
# amd64 16-bit (operand-size 0x66) integer ALU on REGISTER operands (mod==3). Previously
# every 16-bit ALU bridged: the native arith/logic reg gadgets gate on size 32/64 only,
# because their CF/OF come from the host's 32-bit ARM flags (wrong at width 16 -- CF is
# bit 16, OF is bit 15). This family computes the 16-bit flags by hand, exactly like the
# byte family but width 16 and with NO high-byte cases (16-bit regs are always the low 16
# of the register; the upper 48 bits are preserved on write). Memory operands still bridge
# (a 2-byte access can straddle a page, unlike the 1-byte forms). ADC/SBB (groups 2/3)
# still bridge. Register map mirrors the byte gadgets: x8=packed, x9=full dst, x10=rhs,
# x13=lhs, x15=result; the called flag macros clobber x11/x12/x14/x16/x17.

# Compute one 16-bit ALU op: x8 bits0-3=group (0=ADD,1=OR,4=AND,5=SUB,6=XOR,7=CMP),
# x13=lhs (0-65535), x10=rhs (0-65535) -> x15 = 16-bit result + eager flags. Does NOT
# write the result back. Clobbers x11/x12/x14/x15/x16/x17 (x8/x9/x10/x13 preserved).
.macro amd64_w16_compute
    and  x12, x8, 0xf
    cmp  x12, 1
    b.eq .Lwc_or_\@
    cmp  x12, 4
    b.eq .Lwc_and_\@
    cmp  x12, 6
    b.eq .Lwc_xor_\@
    cmp  x12, 0
    b.eq .Lwc_add_\@
    /* SUB (5) / CMP (7): 0-65535 operands so the 32-bit borrow == the 16-bit borrow */
    subs w15, w13, w10
    cset w14, cc                     /* CF (borrow) */
    and  w15, w15, 0xffff
    eor  w12, w13, w10               /* OF(sub) = (lhs^rhs) & (lhs^res) & 0x8000 */
    eor  w17, w13, w15
    and  w12, w12, w17
    tst  w12, 0x8000
    cset w16, ne
    amd64_cached_set_addsub_flags 15
    b    .Lwc_done_\@
.Lwc_add_\@:
    add  w15, w13, w10               /* 0-131070 */
    ubfx w14, w15, 16, 1             /* CF = carry out of bit 15 */
    and  w15, w15, 0xffff
    eor  w12, w13, w10               /* OF(add) = ~(lhs^rhs) & (lhs^res) & 0x8000 */
    eor  w17, w13, w15
    bic  w12, w17, w12
    tst  w12, 0x8000
    cset w16, ne
    amd64_cached_set_addsub_flags 15
    b    .Lwc_done_\@
.Lwc_or_\@:  orr  w15, w13, w10
    b    .Lwc_logic_\@
.Lwc_and_\@: and  w15, w13, w10
    b    .Lwc_logic_\@
.Lwc_xor_\@: eor  w15, w13, w10
.Lwc_logic_\@:
    and  w15, w15, 0xffff
    mov  x11, 16
    amd64_cached_set_logic_flags x15, x11
.Lwc_done_\@:
.endm

# op r/m16, imm16 (0x81 /0,1,4,5,6,7) or imm8-sign-extended (0x83), mod==3. gen.c puts the
# (already sign-extended for 0x83) immediate in operand 8. packed: group(0-3) | dst_id(4-7).
.gadget amd64_w16_alu_reg_imm
    ldr  x8, [_ip]
    ldr  x10, [_ip, 8]
    and  w10, w10, 0xffff            /* rhs = imm (low 16) */
    ubfx x11, x8, 4, 4               /* dst id */
    amd64_read_low_or_mem x11, x9    /* x9 = full dst register */
    and  x13, x9, 0xffff             /* lhs = low 16 */
    amd64_w16_compute
    and  x12, x8, 0xf
    cmp  x12, 7                      /* CMP: flags only, no write */
    b.eq 1f
    bfi  x9, x15, 0, 16              /* replace low 16, preserve upper 48 */
    ubfx x13, x8, 4, 4
    mov  x10, 64
    amd64_write_low_or_mem x13, x9, x10
1:  gret 2

# op r/m16,r16 + r16,r/m16 (0x01/03/09/0b/21/23/29/2b/31/33/39/3b) + TEST r/m16,r16 (0x85),
# mod==3. gen.c resolves direction to dst (lhs) and src (rhs). packed: op(0-3) | dst_id(4-7)
# | src_id(8-11) | nowrite(12). TEST encodes op=4 (AND) with nowrite set.
.gadget amd64_w16_alu_reg_reg
    ldr  x8, [_ip]
    ubfx x11, x8, 8, 4               /* src id */
    amd64_read_low_or_mem x11, x10
    and  x10, x10, 0xffff            /* rhs = src low 16 */
    ubfx x11, x8, 4, 4               /* dst id */
    amd64_read_low_or_mem x11, x9
    and  x13, x9, 0xffff             /* lhs = dst low 16 */
    amd64_w16_compute
    and  x12, x8, 0xf
    cmp  x12, 7                      /* CMP: no write */
    b.eq 1f
    tbnz x8, 12, 1f                  /* TEST: no write (unused -- gen.c routes only arith) */
    bfi  x9, x15, 0, 16
    ubfx x13, x8, 4, 4
    mov  x10, 64
    amd64_write_low_or_mem x13, x9, x10
1:  gret 1

# ---- SSE FP long-tail: the ops a compiled double/float hot loop actually
# emits, previously all routed through the amd64_jit_0f_vec_rm interpreter
# bridge (a matmul benchmark spent its whole life there — one bridge round
# trip per arithmetic instruction). Same conventions as the vector move
# gadgets above: mem forms use [gadget][meta][disp][next_ip] with the xmm
# register id in meta bits 8-11 and fault restart via next_ip; reg forms
# use [gadget][rm | reg<<4]. NEON d/s/q scratch regs are caller-saved,
# loaded fresh, and dead after gret.

# Packed 128-bit arithmetic, reg-reg: dst = dst OP src, lane-wise.
# x86 MIN/MAX (SSE minsd/maxsd/minss/maxss/minp*/maxp*) are not IEEE minNum
# and not arm64's fmin/fmax: MIN is `dst < src ? dst : src` and MAX
# `dst > src ? dst : src`, so when either operand is a NaN, or the two compare
# equal (+0 and -0), the result is the SOURCE, bit for bit -- a signalling NaN
# source comes back unquieted. fmin/fmax quiet NaNs and order -0 below +0, and
# 29 of 64 special-value pairs differed from the interpreter (2026-10-04).
# fcmp sets mi/gt only for an ordered strict comparison, which is exactly the
# rule. Scalar: \d = \a OP \b. fcmpe, the signalling compare: MIN/MAX raise
# Invalid for a NaN operand, quiet or signalling, and fcmp would raise it only
# for a signalling one. (fcmgt, below, signals the same way.)
.macro x86min d, a, b
    fcmpe \a, \b
    fcsel \d, \a, \b, mi
.endm
.macro x86max d, a, b
    fcmpe \a, \b
    fcsel \d, \a, \b, gt
.endm

# Packed: per lane, the same rule through a compare mask (fcmgt is false for
# an unordered lane, which then takes the source).
.macro amd64_x86_minmax_v kind, arr
    .ifc \kind,min
        fcmgt v2.\arr, v1.\arr, v0.\arr
    .else
        fcmgt v2.\arr, v0.\arr, v1.\arr
    .endif
    bsl v2.16b, v0.16b, v1.16b
    mov v0.16b, v2.16b
.endm
# Scalar single, reg-reg and mem source (dst.f32[0], upper 96 preserved).
.macro amd64_v_scalar_single name, fpop
    .gadget amd64_v_\name\()_reg
        ldr x8, [_ip]
        and x9, x8, 0xf
        ubfx x10, x8, 4, 4
        add x11, _cpu, CPU_xmm
        add x9, x11, x9, lsl 4
        add x10, x11, x10, lsl 4
        ldr s0, [x10]
        ldr s1, [x9]
        \fpop s0, s0, s1
        str s0, [x10]
        gret 1
    .gadget amd64_v_\name\()_mem
        ldr  x8, [_ip]
        ldr  x9, [_ip, 8]
        ubfx x10, x8, 8, 4
        amd64_vmem_addr
        amd64_vread_prep 4, \name\()_mem
        ldr  w12, [x9]
        fmov s1, w12
        add  x14, _cpu, CPU_xmm
        add  x14, x14, x10, lsl 4
        ldr  s0, [x14]
        \fpop s0, s0, s1
        str  s0, [x14]
        gret 3
        amd64_vread_bullshit 4, \name\()_mem
.endm

amd64_v_scalar_single addss, fadd
amd64_v_scalar_single subss, fsub
amd64_v_scalar_single mulss, fmul
amd64_v_scalar_single divss, fdiv

# Scalar-double reg-reg stragglers for the existing macro.
amd64_v_scalar_double subsd, fsub
amd64_v_scalar_double divsd, fdiv

# Scalar square root (F2 0F 51 sqrtsd, F3 0F 51 sqrtss): dst's low lane =
# sqrt(src's low lane), the rest of dst preserved. fsqrt is IEEE correctly
# rounded, as SSE's sqrt is, so results match the interpreter bit for bit
# (checked by tests/manual/x86/amd64_sqrt_scalar.c). The path it replaces
# was the interpreter: sqrtsd had no translation, so a hot loop with one in
# it left the JIT, failed a compile at it and was interpreted every
# iteration (2026-10-04: an n-body double loop ran 52x slower on the amd64
# guest than on arm64, all of it amd64_step_to_interrupt).
# The one place fsqrt and SSE differ is the invalid case, a negative non-zero
# input: SSE returns its "indefinite" QNaN, sign bit SET (fff8000000000000 /
# ffc00000), arm64 its default NaN with the sign clear. amd64_sqrt_fix
# substitutes the x86 value; fcmp against 0.0 sets N only for an ordered
# less-than, so -0 (sqrt is -0) and NaN inputs (quieted and propagated, the
# same on both) go to fsqrt untouched. \ind is the indefinite's top 16 bits.
# fsqrt runs in every case, the invalid one included: it is what raises the
# host's Invalid Operation flag, which the guest's MXCSR.IE is made from
# (sqrt(-1.0) has to report FE_INVALID -- fp_env checks it). Only the result's
# value is then replaced.
.macro amd64_sqrt_fix r, ldrr, ind, ind_shift
    fsqrt \r\()0, \r\()1
    fcmp \r\()1, #0.0
    b.pl 2f
    movz \ldrr\()12, \ind, lsl \ind_shift
    fmov \r\()0, \ldrr\()12
2:
.endm
# movhpd/movhps xmm, m64 (0F 16, mod!=3): HIGH lane = m64, low preserved.
# movhpd/movhps m64, xmm (0F 17): store the HIGH lane.
# movlpd/movlps xmm, m64 (0F 12, mod!=3): LOW lane = m64, high preserved
# (unlike movsd-load, which zeroes). 0F 13 stores the low lane.
# ---- Fused cmp/test + jcc ------------------------------------------------
#
# One gadget replacing the [sub32/and32 op][jmp_<cond>] pair that gen.c
# emits for a 32-bit cmp/test immediately followed by a jcc (gen.c's
# gen_try_fuse_jcc rewrites the op gadget in place; the branch then tests
# LIVE host flags instead of re-deriving the condition from the lazy
# CPU_res/CPU_cf/CPU_of state that do_jump reads).
#
# Contract (matches the unfused pair exactly):
#  - The full lazy-flag state is still deposited via the same do_op guts
#    (op1/op2+AF_OPS, eager CF/OF, res+ZF/SF/PF_RES): one x86 cmp can feed
#    MULTIPLE jcc (cmp; je; jl), and the second jcc lands in the
#    fall-through block reading CPU state. Anything after do_op here must
#    not clobber NZCV (cset/strb/str/ldr/orr don't; movs/uxts at size 32
#    are plain mov).
#  - Stream layout mirrors the unfused forms: reg form [gadget][to][else],
#    imm/mem forms [gadget][imm-or-orig_ip][to][else]; taken target first,
#    exactly like jmp_<cond>, so JN_REL's swapped-target negation and the
#    frontend's jump_ip chaining/patching work unchanged.
#  - sub family host-cond mapping is the AArch64 unsigned/signed compare
#    correspondence (x86 CF = borrow = host C clear).
#  - and (test) family: x86 test sets CF=OF=0, so the branch re-tests the
#    result with tst (Z/N live, C=V=0). Only conditions expressible under
#    C=V=0 are stamped; jb/jae/jo/jno/jp after test stay unfused (gen.c
#    finds a NULL array slot and declines).
#  - p (parity) has no host flag: never stamped, never fused.

.macro fused_jcc_tail bcond, off
    b\bcond 1f
    ldr _ip, [_ip, \off+8]
    chain_ip
1:
    ldr _ip, [_ip, \off]
    chain_ip
.endm

.macro fused_flagop_reg name, armop, cond, bcond, reg, hostreg, retest
    .gadget fused_\name\()32_\cond\()_reg_\reg
        do_op \armop, 32, \hostreg
        .ifnb \retest
            tst _tmp, _tmp
        .endif
        fused_jcc_tail \bcond, 0
.endm

.macro fused_flagop_family name, armop, cond, bcond, retest
    .gadget fused_\name\()32_\cond\()_imm
        ldr w8, [_ip]
        do_op \armop, 32, w8
        .ifnb \retest
            tst _tmp, _tmp
        .endif
        fused_jcc_tail \bcond, 8
    fused_flagop_reg \name, \armop, \cond, \bcond, a, eax, \retest
    fused_flagop_reg \name, \armop, \cond, \bcond, c, ecx, \retest
    fused_flagop_reg \name, \armop, \cond, \bcond, d, edx, \retest
    fused_flagop_reg \name, \armop, \cond, \bcond, b, ebx, \retest
    fused_flagop_reg \name, \armop, \cond, \bcond, sp, esp, \retest
    fused_flagop_reg \name, \armop, \cond, \bcond, bp, ebp, \retest
    fused_flagop_reg \name, \armop, \cond, \bcond, si, esi, \retest
    fused_flagop_reg \name, \armop, \cond, \bcond, di, edi, \retest
    .gadget fused_\name\()32_\cond\()_mem
        read_prep 32, fused_\name\()32_\cond\()_mem
        ldr w8, [_xaddr]
        do_op \armop, 32, w8
        .ifnb \retest
            tst _tmp, _tmp
        .endif
        fused_jcc_tail \bcond, 8
        read_bullshit 32, fused_\name\()32_\cond\()_mem
.endm

fused_flagop_family cmp, sub, o, vs
fused_flagop_family cmp, sub, c, cc
fused_flagop_family cmp, sub, z, eq
fused_flagop_family cmp, sub, cz, ls
fused_flagop_family cmp, sub, s, mi
fused_flagop_family cmp, sub, sxo, lt
fused_flagop_family cmp, sub, sxoz, le

fused_flagop_family test, and, z, eq, retest
fused_flagop_family test, and, cz, eq, retest
fused_flagop_family test, and, s, mi, retest
fused_flagop_family test, and, sxo, mi, retest
fused_flagop_family test, and, sxoz, le, retest

# cond-major [cond_count][arg_count] arrays, same arg layout as the op
# gadget tables; missing entries (parity, and-family jb/jae/jo, addr)
# resolve to 0 and gen.c declines the fusion.
# (COND_LIST lives in control.S, a different translation unit -- an
# unexpanded name here would silently produce an 11-entry all-zero array
# that gen.c indexes out of bounds, so define it locally, same order.)
#define COND_LIST o,c,z,cz,s,p,sxo,sxoz
_gadget_array_start fused_cmp32
    .irp cond, COND_LIST
        gadgets fused_cmp32_\cond, GADGET_LIST
    .endr
    .popsection
_gadget_array_start fused_test32
    .irp cond, COND_LIST
        gadgets fused_test32_\cond, GADGET_LIST
    .endr
    .popsection

# ---- 32-bit cmp/test reg,reg + jcc in ONE gadget ----------------------------
#
#     [load32_reg_<dst>][fused_cmp32_<cond>_reg_<src>][to][else]   2 dispatches
#     [fused_cmp32rr_<cond>_<dst>_<src>][to][else]                 1
#
# gen_try_fuse_jcc rewrites the pair when the cmp/test was register against
# register (it knows the load is that instruction's own: gen.c records where
# the CMP/TEST began). The body is the fused_flagop_reg one with the
# destination moved into _tmp first, as load32_reg did, so the lazy flags and
# the branch are identical. Stamped only for the conditions that occur after
# a register compare in practice: cmp with c/z/cz/s/sxo/sxoz (unsigned and
# signed loops), test with z (test r,r + je/jne). 7-Zip: cmp reg,reg is 2.6%
# of i386 instructions, 1.6% of them cmp+jae alone (2026-10-04). A missing
# combination is a 0 slot, and gen.c keeps the two-gadget form.
.macro fused_flagop_rr name, armop, cond, bcond, retest, dst_name, dst, src_name, src
    .gadget fused_\name\()32rr_\cond\()_\dst_name\()_\src_name
        mov _tmp, \dst
        do_op \armop, 32, \src
        .ifnb \retest
            tst _tmp, _tmp
        .endif
        fused_jcc_tail \bcond, 0
.endm
.macro fused_flagop_rr_family name, armop, cond, bcond, retest
    .macro ffrr_row dst_name, dst
        .macro ffrr_col src_name, src
            fused_flagop_rr \name, \armop, \cond, \bcond, \retest, \dst_name, \dst, \src_name, \src
        .endm
        .each_reg ffrr_col
        .purgem ffrr_col
    .endm
    .each_reg ffrr_row
    .purgem ffrr_row
.endm
fused_flagop_rr_family cmp, sub, c, cc
fused_flagop_rr_family cmp, sub, z, eq
fused_flagop_rr_family cmp, sub, cz, ls
fused_flagop_rr_family cmp, sub, s, mi
fused_flagop_rr_family cmp, sub, sxo, lt
fused_flagop_rr_family cmp, sub, sxoz, le
fused_flagop_rr_family test, and, z, eq, retest
.purgem fused_flagop_rr_family
.purgem fused_flagop_rr

# [cond][dst * 8 + src], COND_LIST order (o,c,z,cz,s,p,sxo,sxoz), REG_LIST order
# for both registers; an unstamped combination is 0.
.macro ffrr_quad name, cond, dst, src
    .ifndef NAME(gadget_fused_\name\()32rr_\cond\()_\dst\()_\src)
        .set NAME(gadget_fused_\name\()32rr_\cond\()_\dst\()_\src), 0
    .endif
    .quad NAME(gadget_fused_\name\()32rr_\cond\()_\dst\()_\src)
.endm
.macro ffrr_table name
    .global.name fused_\name\()32rr_gadgets
.endm
.pushsection_rodata
.irp name, cmp,test
    ffrr_table \name
    .irp cond, COND_LIST
        .irp dst, REG_LIST
            .irp src, REG_LIST
                ffrr_quad \name, \cond, \dst, \src
            .endr
        .endr
    .endr
.endr
.popsection
.purgem ffrr_quad
.purgem ffrr_table

# ---- 8-bit cmp/test + jcc -------------------------------------------------
#
# The same fusion for byte compares (gzip's match loop is 4% `cmpb reg,mem`
# alone). The 8-bit do_op leaves no usable host flags -- do_add works on
# sign-extended values to find OF -- so after it deposits the lazy state the
# gadget re-derives NZCV for the branch: for cmp, a native compare of the two
# bytes shifted into the top of a word (a = result + b; (a<<24) - (b<<24) sets
# N Z C V exactly as the x86 8-bit subtract does), for test, tst of the
# shifted result (C = V = 0, as x86 test leaves them). The source is intact
# after do_op (do_add writes only its destination).
#
# Register sources: al/cl/dl/bl only. At size 8, reg_sp..reg_di name the
# high bytes ah/ch/dh/bh (do_hi_op); those slots stay 0 and gen.c declines.
.macro fused_flagop8_retest kind, arg
    .ifc \kind,cmp
        add w9, _tmp, \arg
        lsl w9, w9, 24
        cmp w9, \arg, lsl 24
    .else
        lsl w9, _tmp, 24
        tst w9, w9
    .endif
.endm

.macro fused_flagop8_reg name, armop, cond, bcond, reg, hostreg
    .gadget fused_\name\()8_\cond\()_reg_\reg
        do_op \armop, 8, \hostreg
        fused_flagop8_retest \name, \hostreg
        fused_jcc_tail \bcond, 0
.endm

.macro fused_flagop8_family name, armop, cond, bcond
    .gadget fused_\name\()8_\cond\()_imm
        ldrb w8, [_ip]
        do_op \armop, 8, w8
        fused_flagop8_retest \name, w8
        fused_jcc_tail \bcond, 8
    fused_flagop8_reg \name, \armop, \cond, \bcond, a, eax
    fused_flagop8_reg \name, \armop, \cond, \bcond, c, ecx
    fused_flagop8_reg \name, \armop, \cond, \bcond, d, edx
    fused_flagop8_reg \name, \armop, \cond, \bcond, b, ebx
    .gadget fused_\name\()8_\cond\()_mem
        read_prep 8, fused_\name\()8_\cond\()_mem
        ldrb w8, [_xaddr]
        do_op \armop, 8, w8
        fused_flagop8_retest \name, w8
        fused_jcc_tail \bcond, 8
        read_bullshit 8, fused_\name\()8_\cond\()_mem
.endm

fused_flagop8_family cmp, sub, o, vs
fused_flagop8_family cmp, sub, c, cc
fused_flagop8_family cmp, sub, z, eq
fused_flagop8_family cmp, sub, cz, ls
fused_flagop8_family cmp, sub, s, mi
fused_flagop8_family cmp, sub, sxo, lt
fused_flagop8_family cmp, sub, sxoz, le

fused_flagop8_family test, and, z, eq
fused_flagop8_family test, and, cz, eq
fused_flagop8_family test, and, s, mi
fused_flagop8_family test, and, sxo, mi
fused_flagop8_family test, and, sxoz, le

_gadget_array_start fused_cmp8
    .irp cond, COND_LIST
        gadgets fused_cmp8_\cond, GADGET_LIST
    .endr
    .popsection
_gadget_array_start fused_test8
    .irp cond, COND_LIST
        gadgets fused_test8_\cond, GADGET_LIST
    .endr
    .popsection

# ---- amd64 fused cmp + jcc -----------------------------------------------
#
# Twins of amd64_cached_arith_reg_imm (op==7) / amd64_cached_arith_reg_reg
# (opcode 0x38-0x3b) that branch instead of falling through to the next
# dispatch: gen.c's amd64 jcc emitter rewrites the cmp gadget in place
# (jit/gen.c gen_amd64_try_fuse_jcc) when the jcc immediately follows.
# The full EAGER flag state is still deposited via the same
# amd64_cached_set_addsub_flags (later blocks re-read eflags), which
# clobbers NZCV -- so the tail tests the still-live cf (w14), of (w16)
# and result (x15, upper bits clean in the 32-bit path) instead of host
# flags. Stream: [gadget][packed]([imm])[taken][else]; the gadget loads the
# chosen word into _ip and chains (amd64_chain_ip) exactly like
# amd64_jcc_<cond>, so a fused tail chains (or exits) on the same terms as
# an unfused one. Parity has no cheap live-register form and is never fused
# (NULL table slot in gen.c).

.macro amd64_fused_cc_tail cond, sbit, toff
    .ifc \cond,o
        cbnz w16, 8101f
    .endif
    .ifc \cond,c
        cbnz w14, 8101f
    .endif
    .ifc \cond,z
        cbz x15, 8101f
    .endif
    .ifc \cond,cz
        cbnz w14, 8101f
        cbz x15, 8101f
    .endif
    .ifc \cond,s
        tbnz x15, \sbit, 8101f
    .endif
    .ifc \cond,sxo
        ubfx x12, x15, \sbit, 1
        cmp w12, w16
        b.ne 8101f
    .endif
    .ifc \cond,sxoz
        cbz x15, 8101f
        ubfx x12, x15, \sbit, 1
        cmp w12, w16
        b.ne 8101f
    .endif
    ldr _ip, [_ip, \toff+8]
    b 8102f
8101:
    ldr _ip, [_ip, \toff]
8102:
    amd64_chain_ip
.endm

.macro amd64_fused_cmp_ri cond
    .gadget amd64_fused_cmp_ri_\cond
        ldr x8, [_ip]
        ldr x10, [_ip, 8]
        ubfx x11, x8, 12, 4
        ubfx x12, x8, 16, 7
        amd64_cached_read x11, x13
        cmp x12, 64
        b.eq 8103f
        mov w13, w13
        mov w10, w10
        subs w15, w13, w10
        cset w14, cc
        cset w16, vs
        mov w15, w15
        amd64_cached_set_addsub_flags 31
        amd64_fused_cc_tail \cond, 31, 16
8103:
        subs x15, x13, x10
        cset w14, cc
        cset w16, vs
        amd64_cached_set_addsub_flags 63
        amd64_fused_cc_tail \cond, 63, 16
.endm

.macro amd64_fused_cmp_rr cond
    .gadget amd64_fused_cmp_rr_\cond
        ldr x8, [_ip]
        and x9, x8, 0xff
        ubfx x10, x8, 8, 4
        ubfx x11, x8, 12, 4
        ubfx x12, x8, 16, 7
        mov x0, x12
        tst x9, 2
        b.ne 8104f
        amd64_cached_read x11, x13
        amd64_cached_read x10, x10
        b 8105f
8104:
        amd64_cached_read x10, x13
        amd64_cached_read x11, x10
8105:
        cmp x0, 64
        b.eq 8106f
        mov w13, w13
        mov w10, w10
        subs w15, w13, w10
        cset w14, cc
        cset w16, vs
        mov w15, w15
        amd64_cached_set_addsub_flags 31
        amd64_fused_cc_tail \cond, 31, 8
8106:
        subs x15, x13, x10
        cset w14, cc
        cset w16, vs
        amd64_cached_set_addsub_flags 63
        amd64_fused_cc_tail \cond, 63, 8
.endm

.irp cond, o,c,z,cz,s,sxo,sxoz
    amd64_fused_cmp_ri \cond
    amd64_fused_cmp_rr \cond
.endr

# ===========================================================================
# Byte family, bit scan, and memory-source multiply -- the last interpreter
# bridges in compiler code (cc1: `shr al,imm`, `cmp [mem],r8`, `and r8,[mem]`,
# `bsr`, `imul reg,[mem]`).
#
# Byte-register operands live inside the 64-bit slots of cpu->amd64_regs: the
# low byte of regs[r] for AL/CL/DL/BL, SPL..DIL (REX present) and R8B..R15B, and
# BYTE 1 of regs[r-4] for AH/CH/DH/BH (no REX, rm/reg 4-7). gen.c resolves the
# slot and passes a "high" bit; the gadget forms the byte pointer with one add.
# Everything here is flush style: the register cache is flushed (and
# invalidated) before these run, so cpu->amd64_regs is authoritative and every
# write goes there. Nothing below touches x20-x27.
# ===========================================================================

# SHL/SHR/SAR r/m8 by 1 (0xd0), CL (0xd2) or imm8 (0xc0), mod==3.
# Operand: packed(0) = slot(0-3) | group(4-7: 4 shl / 5 shr / 7 sar) | count(8-15)
#   | is_high(16) | use_cl(17). The count is masked by 31 for every operand size,
# as the hardware does; a zero count leaves the flags alone. CF is the last bit
# shifted out (0 once the count exceeds the width), OF is defined for count 1
# only (SHL: bit7(res)^CF, SHR: sign of the source, SAR: 0), ZF/SF/PF come from
# the 8-bit result via amd64_cached_set_shift_flags 7. gret 1.
.gadget amd64_shift8
    ldr  x8, [_ip]
    and  x11, x8, 0xf                /* slot */
    ubfx x9, x8, 4, 4                /* group */
    ubfx x10, x8, 8, 8               /* count: imm8, or the literal 1 */
    add  x13, _cpu, CPU_amd64_regs
    tbz  x8, 17, 1f
    ldrb w10, [x13, 8]               /* CL = low byte of regs[rcx] */
1:  add  x11, x13, x11, lsl 3        /* &regs[slot] */
    tbz  x8, 16, 2f
    add  x11, x11, 1                 /* AH/CH/DH/BH: byte 1 of the slot */
2:  and  x10, x10, 31
    cbz  x10, 9f
    ldrb w13, [x11]                  /* operand byte, zero-extended */
    cmp  x9, 4
    b.eq 3f
    cmp  x9, 5
    b.eq 4f
    sxtb w13, w13                    /* SAR: shift the sign-extended value */
    asr  w15, w13, w10
    sub  w12, w10, 1
    lsr  w14, w13, w12               /* CF = bit (count-1) of the source */
    b    5f
3:  lsl  w15, w13, w10               /* SHL */
    mov  w12, 8
    sub  w12, w12, w10
    lsr  w14, w13, w12               /* CF = bit (8-count) of the source; 0 past 8 */
    b    5f
4:  lsr  w15, w13, w10               /* SHR */
    sub  w12, w10, 1
    lsr  w14, w13, w12               /* CF = bit (count-1) of the source */
5:  and  w14, w14, 1
    mov  w16, 0
    cmp  x10, 1
    b.ne 6f
    cmp  x9, 4
    b.eq 7f
    cmp  x9, 5
    b.eq 8f
    b    6f                          /* SAR: OF = 0 */
7:  lsr  w16, w15, 7                 /* SHL: OF = bit7(res) ^ CF */
    and  w16, w16, 1
    eor  w16, w16, w14
    b    6f
8:  lsr  w16, w13, 7                 /* SHR: OF = sign of the source */
6:  and  w15, w15, 0xff
    strb w15, [x11]
    amd64_cached_set_shift_flags 7
9:  gret 1

# <alu> r8, r/m8 (0x02/0a/22/2a/32/3a), mod!=3: register destination, memory
# source. Memory is read-only here, so no write prep and nothing to undo.
# Operands: [meta][disp][next_ip][packed], packed = group(0-3) | slot(4-7) |
# is_high(8). CMP (group 7) sets flags and skips the write. gret 4.
.gadget amd64_byte_alu_regmem
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    amd64_vmem_addr
    amd64_vread_prep 1, byte_alu_regmem
    ldrb w10, [x9]                   /* rhs = [mem] */
    ldr  x8, [_ip, 24]               /* packed (vread_prep clobbered x8) */
    ubfx x11, x8, 4, 4
    add  x14, _cpu, CPU_amd64_regs
    add  x9, x14, x11, lsl 3         /* x9 = &regs[slot]; byte_compute keeps x9 */
    tbz  x8, 8, 1f
    add  x9, x9, 1
1:  ldrb w13, [x9]                   /* lhs = destination byte */
    amd64_byte_compute
    and  x12, x8, 0xf
    cmp  x12, 7
    b.eq 2f
    strb w15, [x9]
2:  gret 4
    amd64_vread_bullshit 1, byte_alu_regmem

# CMP r/m8, r8 (0x38) and TEST r/m8, r8 (0x84), mod!=3: the memory operand is
# the lhs and nothing is written, so this is the read path -- a CMP against a
# read-only page must not fault as a write. packed = group(0-3: 7 cmp / 4 test)
# | slot(4-7) | is_high(8). gret 4.
.gadget amd64_byte_cmp_memreg
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    amd64_vmem_addr
    amd64_vread_prep 1, byte_cmp_memreg
    ldrb w13, [x9]                   /* lhs = [mem] */
    ldr  x8, [_ip, 24]
    ubfx x11, x8, 4, 4
    add  x14, _cpu, CPU_amd64_regs
    add  x9, x14, x11, lsl 3
    tbz  x8, 8, 1f
    add  x9, x9, 1
1:  ldrb w10, [x9]                   /* rhs = register byte */
    amd64_byte_compute               /* flags only; the result is dropped */
    gret 4
    amd64_vread_bullshit 1, byte_cmp_memreg

# <alu> r/m8, r8 (0x00/08/20/28/30), mod!=3: memory destination, read-modify-
# write in the dual-prep frame of amd64_gen_imm_arith (guest address in x0
# across both preps, result in x10 across the write prep). gen.c keeps LOCKed
# byte RMW on the helper, which does a host atomic. packed = group(0-3) |
# slot(4-7) | is_high(8). gret 4.
.gadget amd64_byte_alu_memreg
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    amd64_vmem_addr
    mov  x0, x9                      /* guest addr survives both preps */
    amd64_vread_prep 1, byte_alu_memreg_r
    ldrb w13, [x9]                   /* lhs = [mem] */
    ldr  x8, [_ip, 24]
    ubfx x11, x8, 4, 4
    add  x14, _cpu, CPU_amd64_regs
    add  x9, x14, x11, lsl 3
    tbz  x8, 8, 1f
    add  x9, x9, 1
1:  ldrb w10, [x9]                   /* rhs = register byte */
    amd64_byte_compute
    mov  x10, x15                    /* result -> x10 (survives the write prep) */
    mov  x9, x0
    amd64_vwrite_prep 1, byte_alu_memreg_w
    strb w10, [x9]
    amd64_vwrite_done 1, byte_alu_memreg_w
    gret 4
    amd64_vread_bullshit 1, byte_alu_memreg_r
    amd64_vwrite_bullshit 1, byte_alu_memreg_w

# BSF (0F BC) / BSR (0F BD) r, r/m, 16/32/64, and with a REP prefix TZCNT / LZCNT.
# Body inputs: x10 = source (zero-extended to 64), x12 = dst slot, x8 bit 16
# = reverse (BSR/LZCNT), bit 17 = 64-bit, bit 18 = count zeroes (TZCNT/LZCNT),
# bit 19 = 16-bit (the result goes into the low word only).
# BSF/BSR: a zero source sets ZF and leaves the destination alone, as the
# interpreter does; otherwise ZF is cleared and the destination gets the bit
# index. TZCNT/LZCNT: CF = (source == 0), the result is the operand size for a
# zero source or the count otherwise, ZF = (result == 0), destination always
# written. Nothing else is touched (architecturally undefined). ZF/CF go into
# eflags (and the CF byte); the lazy ZF bit is cleared.
.macro amd64_bitscan_body
    ldrb w11, [_cpu, CPU_flags_res]
    bic  w11, w11, ZF_RES
    strb w11, [_cpu, CPU_flags_res]
    ldr  w17, [_cpu, CPU_eflags]
    tbnz x8, 18, .Lbs_cz_\@
    cbnz x10, .Lbs_nonzero_\@
    orr  w17, w17, ZF_FLAG
    str  w17, [_cpu, CPU_eflags]
    b    .Lbs_done_\@
.Lbs_nonzero_\@:
    bic  w17, w17, ZF_FLAG
    str  w17, [_cpu, CPU_eflags]
    tbnz x8, 16, .Lbs_bsr_\@
    rbit x15, x10                    /* BSF: index of the lowest set bit */
    clz  x15, x15
    b    .Lbs_store_\@
.Lbs_bsr_\@:
    clz  x15, x10                    /* BSR: 63 - clz (31 - clz32 once zero-extended) */
    mov  x16, 63
    sub  x15, x16, x15
.Lbs_store_\@:
    add  x14, _cpu, CPU_amd64_regs
    tbz  x8, 19, .Lbs_store_wide_\@
    ldr  x16, [x14, x12, lsl 3]
    bfi  x16, x15, 0, 16
    mov  x15, x16
.Lbs_store_wide_\@:
    str  x15, [x14, x12, lsl 3]
    b    .Lbs_done_\@
.Lbs_cz_\@:
    mov  x16, 32
    tbz  x8, 19, .Lbs_cz_not16_\@
    mov  x16, 16
.Lbs_cz_not16_\@:
    tbz  x8, 17, .Lbs_cz_sized_\@
    mov  x16, 64
.Lbs_cz_sized_\@:
    cbz  x10, .Lbs_cz_zero_\@
    tbnz x8, 16, .Lbs_lz_\@
    rbit x15, x10                    /* TZCNT */
    clz  x15, x15
    b    .Lbs_cz_res_\@
.Lbs_lz_\@:
    clz  x15, x10                    /* LZCNT: leading zeros within the operand */
    sub  x15, x15, 64
    add  x15, x15, x16
    b    .Lbs_cz_res_\@
.Lbs_cz_zero_\@:
    mov  x15, x16                    /* zero source: the operand size */
.Lbs_cz_res_\@:
    cmp  x10, 0
    cset w14, eq                     /* CF = source == 0 */
    strb w14, [_cpu, CPU_cf]
    bic  w17, w17, 1                 /* 0x41 is not a logical immediate: two steps */
    bic  w17, w17, ZF_FLAG
    orr  w17, w17, w14
    cmp  x15, 0
    cset w14, eq                     /* ZF = result == 0 */
    orr  w17, w17, w14, lsl 6
    str  w17, [_cpu, CPU_eflags]
    b    .Lbs_store_\@
.Lbs_done_\@:
.endm

# Register source. packed(0) = src(0-3) | dst(4-7) | is_bsr(16) | is64(17) | cz(18) |
# is16(19). gret 1.
.gadget amd64_bitscan_reg
    ldr  x8, [_ip]
    and  x11, x8, 0xf
    ubfx x12, x8, 4, 4
    add  x14, _cpu, CPU_amd64_regs
    ldr  x10, [x14, x11, lsl 3]
    tbnz x8, 17, 5f
    mov  w10, w10                    /* 32-bit source */
    tbz  x8, 19, 5f
    and  x10, x10, 0xffff            /* 16-bit source */
5:  amd64_bitscan_body
    gret 1

# Memory source. Operands: [meta][disp][next_ip][packed], packed = dst(4-7) |
# is_bsr(16) | cz(18); the width is the gadget's (bit 17 set here). gret 4.
.macro amd64_gen_bitscan_mem name, vbytes
.gadget amd64_\name
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    amd64_vmem_addr
    amd64_vread_prep \vbytes, \name
.if \vbytes == 2
    ldrh w10, [x9]
.elseif \vbytes == 4
    ldr  w10, [x9]
.else
    ldr  x10, [x9]
.endif
    ldr  x8, [_ip, 24]
.if \vbytes == 8
    orr  x8, x8, (1 << 17)
.elseif \vbytes == 2
    orr  x8, x8, (1 << 19)
.endif
    ubfx x12, x8, 4, 4
    amd64_bitscan_body
    gret 4
    amd64_vread_bullshit \vbytes, \name
.endm
amd64_gen_bitscan_mem bitscan_mem16, 2
amd64_gen_bitscan_mem bitscan_mem32, 4
amd64_gen_bitscan_mem bitscan_mem64, 8

# POPCNT r, r/m (F3 0F B8), 16/32/64. Body inputs: x10 = the source, zero-
# extended from its size; x12 = the destination slot; x8 bits 8-9 = the size
# (1 = 16, 2 = 32, 3 = 64). CF, OF, SF, AF and PF clear, ZF = (source == 0);
# the lazy bits are cleared. A 16-bit destination keeps bits 16-63, a 32-bit
# one is zero-extended. No scalar popcount on aarch64: v0 is free scratch here
# (dead after gret, no C call in between), as in the i386 popcnt.
.macro amd64_popcnt_body
    fmov d0, x10
    cnt  v0.8b, v0.8b
    addv b0, v0.8b
    fmov w15, s0
    strb wzr, [_cpu, CPU_cf]
    strb wzr, [_cpu, CPU_of]
    cmp  x10, 0
    cset w9, eq
    ldrb w11, [_cpu, CPU_eflags]
    bic  w11, w11, PF_FLAG           /* not one logical immediate: bit by bit */
    bic  w11, w11, AF_FLAG
    bic  w11, w11, ZF_FLAG
    bic  w11, w11, SF_FLAG
    orr  w11, w11, w9, lsl 6
    strb w11, [_cpu, CPU_eflags]
    ldrb w11, [_cpu, CPU_flags_res]
    bic  w11, w11, (PF_RES | ZF_RES | SF_RES | AF_OPS)
    strb w11, [_cpu, CPU_flags_res]
    add  x14, _cpu, CPU_amd64_regs
    ubfx x13, x8, 8, 2
    cmp  x13, 1
    b.ne 1f
    ldr  x16, [x14, x12, lsl 3]
    bfi  x16, x15, 0, 16
    mov  x15, x16
1:  str  x15, [x14, x12, lsl 3]
.endm

# Register source. packed(0) = src(0-3) | dst(4-7) | size(8-9). gret 1.
.gadget amd64_popcnt_reg
    ldr  x8, [_ip]
    and  x11, x8, 0xf
    ubfx x12, x8, 4, 4
    add  x14, _cpu, CPU_amd64_regs
    ldr  x10, [x14, x11, lsl 3]
    ubfx x13, x8, 8, 2
    cmp  x13, 2
    b.hi 2f
    b.eq 1f
    and  x10, x10, 0xffff
    b    2f
1:  mov  w10, w10
2:  amd64_popcnt_body
    gret 1

# Memory source. Operands: [meta][disp][next_ip][packed = dst(4-7) | size(8-9)].
# gret 4.
.macro amd64_gen_popcnt_mem name, vbytes
.gadget amd64_\name
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    amd64_vmem_addr
    amd64_vread_prep \vbytes, \name
.if \vbytes == 2
    ldrh w10, [x9]
.elseif \vbytes == 4
    ldr  w10, [x9]
.else
    ldr  x10, [x9]
.endif
    ldr  x8, [_ip, 24]
    ubfx x12, x8, 4, 4
    amd64_popcnt_body
    gret 4
    amd64_vread_bullshit \vbytes, \name
.endm
amd64_gen_popcnt_mem popcnt_mem16, 2
amd64_gen_popcnt_mem popcnt_mem32, 4
amd64_gen_popcnt_mem popcnt_mem64, 8

# IMUL r, r/m (0F AF) and IMUL r, r/m, imm (69 id / 6B ib) with a memory
# source, 16/32/64. The multiply and the CF=OF=overflow rule are amd64_imul_body's;
# the difference is flush style -- the destination is written to
# cpu->amd64_regs, not the register cache, so any destination register works.
# Operands: [meta][disp][next_ip][packed = dst(8-11)] (+ [imm], sign-extended
# by gen.c, for the imm forms). gret 4 (reg form) / 5 (imm form).
.macro amd64_gen_imul_mem name, vbytes, use_imm
.gadget amd64_\name
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    amd64_vmem_addr
    amd64_vread_prep \vbytes, \name
.if \vbytes == 2
    ldrh w13, [x9]
.elseif \vbytes == 4
    ldr  w13, [x9]
.else
    ldr  x13, [x9]
.endif
    ldr  x8, [_ip, 24]
    ubfx x9, x8, 8, 4                /* dst slot */
    add  x14, _cpu, CPU_amd64_regs
.if \use_imm
    ldr  x10, [_ip, 32]              /* the immediate */
.else
    ldr  x10, [x14, x9, lsl 3]       /* dst is the other factor */
.endif
.if \vbytes == 2
    sxth x13, w13
    sxth x10, w10
    mul  x17, x13, x10
    sxth x15, w17
    cmp  x15, x17
    cset w16, ne                     /* overflow: the product does not fit 16 signed */
    ldr  x15, [x14, x9, lsl 3]
    bfi  x15, x17, 0, 16             /* only the low word changes */
.elseif \vbytes == 4
    sxtw x13, w13
    sxtw x10, w10
    mul  x14, x13, x10
    sxtw x15, w14
    cmp  x15, x14
    cset w16, ne                     /* overflow: the product does not fit 32 signed */
    mov  w15, w14                    /* zero-extend into the 64-bit slot */
.else
    mul  x15, x13, x10
    smulh x14, x13, x10
    asr  x17, x15, 63
    cmp  x14, x17
    cset w16, ne
.endif
    add  x14, _cpu, CPU_amd64_regs
    str  x15, [x14, x9, lsl 3]
    strb w16, [_cpu, CPU_cf]
    strb w16, [_cpu, CPU_of]
    ldr  w17, [_cpu, CPU_eflags]
    mov  w14, (1 | (1 << 11))
    bic  w17, w17, w14
    cbz  w16, 1f
    orr  w17, w17, w14
1:  str  w17, [_cpu, CPU_eflags]
.if \use_imm
    gret 5
.else
    gret 4
.endif
    amd64_vread_bullshit \vbytes, \name
.endm
amd64_gen_imul_mem imul_mem16, 2, 0
amd64_gen_imul_mem imul_mem32, 4, 0
amd64_gen_imul_mem imul_mem64, 8, 0
amd64_gen_imul_mem imul_imm_mem16, 2, 1
amd64_gen_imul_mem imul_imm_mem32, 4, 1
amd64_gen_imul_mem imul_imm_mem64, 8, 1

# ===========================================================================
# Round 2: the bit-test family, any-register IMUL, CMP [mem], reg.
# Flush style throughout (register cache flushed and invalidated before).
# ===========================================================================

# Set CF (and nothing else) from w14 (0/1): the CPU_cf byte the jump macro
# reads, and bit 0 of eflags. BT/BTS/BTR/BTC define only CF; the interpreter
# collapses the lazy flags first and leaves the rest as they were.
.macro amd64_set_cf_only
    strb w14, [_cpu, CPU_cf]
    ldr  w17, [_cpu, CPU_eflags]
    bic  w17, w17, 1
    orr  w17, w17, w14
    str  w17, [_cpu, CPU_eflags]
.endm

# Apply the bit op: x13 = value, x11 = bit (0..63), x8 bits 4-5 = op
# (0 bt, 1 bts, 2 btr, 3 btc). Out: w14 = CF, x15 = new value (== x13 for bt).
.macro amd64_bt_apply
    lsr  x14, x13, x11
    and  w14, w14, 1
    mov  x16, 1
    lsl  x16, x16, x11
    ubfx x12, x8, 4, 2
    mov  x15, x13
    cmp  x12, 1
    b.ne 1f
    orr  x15, x13, x16               /* bts */
    b    4f
1:  cmp  x12, 2
    b.ne 2f
    bic  x15, x13, x16               /* btr */
    b    4f
2:  cmp  x12, 3
    b.ne 4f
    eor  x15, x13, x16               /* btc */
4:
.endm

# BT/BTS/BTR/BTC r/m, r|imm8, register operand.
# packed(0) = rm(0-3) | op(4-5) | is64(6) | idx_is_imm(7) | idx_reg(8-11) | imm(16-23) |
# is16(24). The bit index is taken modulo the operand size, for both index
# forms; at 16 bits only a bit below 16 changes, so the whole register is
# stored as it is. gret 1.
.gadget amd64_bt_reg
    ldr  x8, [_ip]
    and  x9, x8, 0xf
    add  x10, _cpu, CPU_amd64_regs
    ldr  x13, [x10, x9, lsl 3]       /* value */
    tbnz x8, 7, 1f
    ubfx x11, x8, 8, 4
    ldr  x11, [x10, x11, lsl 3]      /* index register */
    b    2f
1:  ubfx x11, x8, 16, 8              /* imm8 index */
2:  tbnz x8, 6, 3f
    tbnz x8, 24, 7f
    and  x11, x11, 31
    mov  w13, w13
    b    4f
7:  and  x11, x11, 15
    b    4f
3:  and  x11, x11, 63
4:  amd64_bt_apply
    ubfx x12, x8, 4, 2
    cbz  x12, 5f                     /* bt: no write */
    tbnz x8, 6, 6f
    tbnz x8, 24, 6f
    mov  w15, w15                    /* 32-bit result zero-extends */
6:  str  x15, [x10, x9, lsl 3]
5:  amd64_set_cf_only
    gret 1

# Memory operand. Operands: [meta][disp][next_ip][packed], packed = op(4-5) |
# idx_is_imm(7) | idx_reg(8-11) | imm(16-23); the width is the gadget's.
# Address rule: the register index is sign-extended to the operand size and its
# element part (index >> log2 size) strides memory by the operand width; the
# imm8 form addresses only the operand itself (bit = imm mod size, no stride --
# hardware, checked on camd; the interpreter had this wrong too). The bit is
# index mod size.
# Computes the adjusted guest address in x9 and the bit in x11 before any prep.
.macro amd64_bt_mem_index vbytes
    ldr  x8, [_ip, 24]
    tbnz x8, 7, 1f
    ubfx x11, x8, 8, 4
    add  x10, _cpu, CPU_amd64_regs
    ldr  x11, [x10, x11, lsl 3]
.if \vbytes == 2
    sxth x11, w11                    /* signed 16-bit index */
    asr  x12, x11, 4
    add  x9, x9, x12, lsl 1
    and  x11, x11, 15
.elseif \vbytes == 4
    sxtw x11, w11                    /* signed 32-bit index */
    asr  x12, x11, 5
    add  x9, x9, x12, lsl 2
    and  x11, x11, 31
.else
    asr  x12, x11, 6
    add  x9, x9, x12, lsl 3
    and  x11, x11, 63
.endif
    b    2f
1:  ubfx x11, x8, 16, 8              /* imm8: bit = imm mod size, NO memory stride */
.if \vbytes == 2
    and  x11, x11, 15
.elseif \vbytes == 4
    and  x11, x11, 31
.else
    and  x11, x11, 63
.endif
2:
.endm

# BT [mem], idx: read only. gret 4.
.macro amd64_gen_bt_mem name, vbytes
.gadget amd64_\name
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    amd64_vmem_addr
    amd64_bt_mem_index \vbytes
    mov  x0, x11                     /* bit survives the prep (x0 is not touched by it) */
    amd64_vread_prep \vbytes, \name
.if \vbytes == 2
    ldrh w13, [x9]
.elseif \vbytes == 4
    ldr  w13, [x9]
.else
    ldr  x13, [x9]
.endif
    lsr  x14, x13, x0
    and  w14, w14, 1
    amd64_set_cf_only
    gret 4
    amd64_vread_bullshit \vbytes, \name
.endm
amd64_gen_bt_mem bt_mem16, 2
amd64_gen_bt_mem bt_mem32, 4
amd64_gen_bt_mem bt_mem64, 8

# BTS/BTR/BTC [mem], idx: dual-prep RMW. Only x0 is proven to survive a prep
# (the imm_arith frame), and the prep slow path uses x19 itself, so the bit
# rides in the top byte of x0 next to the (48-bit) guest address, and CF in
# the top byte of x10 next to the result across the write prep. gret 4.
# packed bit 0 = LOCK: an exclusive loop on the host word, or, misaligned,
# emu/tlb.c's x86_atomic_alu (OR / AND NOT / XOR with the bit).
.macro amd64_gen_btx_mem name, vbytes
.gadget amd64_\name
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    amd64_vmem_addr
    amd64_bt_mem_index \vbytes
    tbnz x8, 0, 60f
    orr  x0, x9, x11, lsl 56         /* x0 = adjusted guest addr | bit << 56 */
    amd64_vread_prep \vbytes, \name\()_r
.if \vbytes == 2
    ldrh w13, [x9]
.elseif \vbytes == 4
    ldr  w13, [x9]
.else
    ldr  x13, [x9]
.endif
    ldr  x8, [_ip, 24]
    lsr  x11, x0, 56                 /* bit */
    amd64_bt_apply
    amd64_set_cf_only                /* flags before the write, as imm_arith does */
.if \vbytes == 4
    mov  w10, w15                    /* result -> x10 (survives the write prep) */
.else
    mov  x10, x15
.endif
    and  x9, x0, 0xffffffffffffff    /* guest addr */
    amd64_vwrite_prep \vbytes, \name\()_w
.if \vbytes == 2
    strh w10, [x9]
.elseif \vbytes == 4
    str  w10, [x9]
.else
    str  x10, [x9]
.endif
    amd64_vwrite_done \vbytes, \name\()_w
    gret 4
60: /* LOCK */
    tst  x9, (\vbytes - 1)
    b.ne 70f
    orr  x0, x9, x11, lsl 56
    amd64_vwrite_prep \vbytes, \name\()_l
    ldr  x8, [_ip, 24]
    lsr  x11, x0, 56
61:
.if \vbytes == 2
    ldaxrh w13, [x9]
.elseif \vbytes == 4
    ldaxr w13, [x9]
.else
    ldaxr x13, [x9]
.endif
    amd64_bt_apply
.if \vbytes == 2
    stlxrh w12, w15, [x9]
.elseif \vbytes == 4
    stlxr w12, w15, [x9]
.else
    stlxr w12, x15, [x9]
.endif
    cbnz w12, 61b
    amd64_set_cf_only
    gret 4
70: /* misaligned LOCK: x86_atomic_alu(cpu, tlb, addr, size, op, mask, 0, &old) */
    mov  x16, 1
    lsl  x5, x16, x11
    ubfx x12, x8, 4, 2
    mov  x4, 1                       /* bts: OR */
    cmp  x12, 3
    mov  x16, 6
    csel x4, x16, x4, eq             /* btc: XOR */
    cmp  x12, 2
    mov  x16, 4
    csel x4, x16, x4, eq             /* btr: AND NOT */
    mvn  x16, x5
    csel x5, x16, x5, eq
    sub  sp, sp, 16
    mov  x7, sp
    save_c
    mov  x0, _cpu
    sub  x1, _tlb, TLB_entries
    mov  x2, x9
    mov  x3, \vbytes
    mov  x6, 0
    bl   NAME(x86_atomic_alu)
    mov  x19, x0
    restore_c
    ldr  x13, [sp]
    add  sp, sp, 16
    cbnz w19, 79f
    lsr  x14, x13, x11
    and  w14, w14, 1
    amd64_set_cf_only
    gret 4
79: mov  x0, x19
    b    jit_exit
    amd64_vread_bullshit \vbytes, \name\()_r
    amd64_vwrite_bullshit \vbytes, \name\()_w
    amd64_vwrite_bullshit \vbytes, \name\()_l
.endm
amd64_gen_btx_mem btx_mem16, 2
amd64_gen_btx_mem btx_mem32, 4
amd64_gen_btx_mem btx_mem64, 8

# IMUL for any register pair: the same multiply and CF=OF rule as
# amd64_imul_body, reading and writing cpu->amd64_regs (flush style) so
# r8-r15 work. In: x13 = src, x10 = other factor, x9 = dst slot, x12 = size.
# At 16 bits only the low word of the destination changes.
.macro amd64_imul_any_core
    cmp  x12, 16
    b.ne 8000f
    sxth x13, w13
    sxth x10, w10
    mul  x17, x13, x10
    sxth x15, w17
    cmp  x15, x17
    cset w16, ne
    add  x14, _cpu, CPU_amd64_regs
    ldr  x15, [x14, x9, lsl 3]
    bfi  x15, x17, 0, 16
    b    8002f
8000:
    cmp  x12, 64
    b.eq 8001f
    sxtw x13, w13
    sxtw x10, w10
    mul  x14, x13, x10
    sxtw x15, w14
    cmp  x15, x14
    cset w16, ne
    mov  w15, w14
    b    8002f
8001:
    mul  x15, x13, x10
    smulh x14, x13, x10
    asr  x17, x15, 63
    cmp  x14, x17
    cset w16, ne
8002:
    add  x14, _cpu, CPU_amd64_regs
    str  x15, [x14, x9, lsl 3]
    strb w16, [_cpu, CPU_cf]
    strb w16, [_cpu, CPU_of]
    ldr  w17, [_cpu, CPU_eflags]
    mov  w14, (1 | (1 << 11))
    bic  w17, w17, w14
    cbz  w16, 8003f
    orr  w17, w17, w14
8003:
    str  w17, [_cpu, CPU_eflags]
.endm

# imul reg, rm (0F AF), reg form, any registers. packed(0) = dst(8-11) | src(12-15) |
# size(16-22). gret 1.
.gadget amd64_imul_reg_any
    ldr  x8, [_ip]
    ubfx x9, x8, 8, 4
    ubfx x11, x8, 12, 4
    ubfx x12, x8, 16, 7
    add  x14, _cpu, CPU_amd64_regs
    ldr  x13, [x14, x11, lsl 3]
    ldr  x10, [x14, x9, lsl 3]
    amd64_imul_any_core
    gret 1

# imul reg, rm, imm (69/6B), reg form, any registers. packed(0) as above, imm(8). gret 2.
.gadget amd64_imul_imm_any
    ldr  x8, [_ip]
    ldr  x10, [_ip, 8]
    ubfx x9, x8, 8, 4
    ubfx x11, x8, 12, 4
    ubfx x12, x8, 16, 7
    add  x14, _cpu, CPU_amd64_regs
    ldr  x13, [x14, x11, lsl 3]
    amd64_imul_any_core
    gret 2

# CMP [mem], reg (0x39), 32/64: flags only, so the read path (a compare
# against a read-only page must not fault as a write). lhs = memory, rhs =
# register, the same eager addsub flags as regmem_cmp with the operands in
# the other order. Operands: [meta][disp][next_ip][reg]. gret 4.
.macro amd64_gen_cmp_memreg name, vbytes, sbit
.gadget amd64_\name
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    amd64_vmem_addr
    amd64_vread_prep \vbytes, \name
.if \vbytes == 4
    ldr  w13, [x9]
.else
    ldr  x13, [x9]
.endif
    ldr  x12, [_ip, 24]
    add  x14, _cpu, CPU_amd64_regs
    ldr  x10, [x14, x12, lsl 3]
.if \vbytes == 4
    mov  w13, w13
    mov  w10, w10
    subs w15, w13, w10
    cset w14, cc
    cset w16, vs
    mov  w15, w15
.else
    subs x15, x13, x10
    cset w14, cc
    cset w16, vs
.endif
    amd64_cached_set_addsub_flags \sbit
    gret 4
    amd64_vread_bullshit \vbytes, \name
.endm
amd64_gen_cmp_memreg cmp_memreg32, 4, 31
amd64_gen_cmp_memreg cmp_memreg64, 8, 63

# ===========================================================================
# Round 3a: one-operand MUL/IMUL/DIV/IDIV, TEST [mem], imm, CMOVcc r, [mem].
# ===========================================================================

# MUL/IMUL/DIV/IDIV r/m (F7 /4-/7), register operand, 32/64. Flush style.
# packed(0) = rm(0-3) | group(4-6) | is64(8). Fast paths cover the shapes
# compilers emit: 32-bit divides (edx:eax always fits 64 bits), a 64-bit DIV
# with rdx == 0, a 64-bit IDIV whose rdx is the sign of rax (the cqo idiom).
# Everything else -- a real 128-bit dividend, a zero divisor, the MIN/-1
# case -- is the slow path at label 9 (amd64_muldiv_body), which raises #DE
# (INT_DIV) through jit_exit like a page fault. MUL/IMUL set CF=OF=overflow
# and nothing else; DIV/IDIV leave the flags alone. gret 1.
.macro amd64_muldiv_flags
    strb w16, [_cpu, CPU_cf]
    strb w16, [_cpu, CPU_of]
    ldr  w17, [_cpu, CPU_eflags]
    mov  w14, (1 | (1 << 11))
    bic  w17, w17, w14
    cbz  w16, 8001f
    orr  w17, w17, w14
8001:
    str  w17, [_cpu, CPU_eflags]
.endm

# The MUL/IMUL/DIV/IDIV work: x8 packed (group 4-6 | is64 8), x11 = the
# register file, x12 = the source. Everything, #DE and 128-bit dividends
# included (they used to be a C twin's); a 64-bit DIV or
# IDIV with a dividend past 64 bits is amd64_udiv128 on the magnitudes now.
.macro amd64_muldiv_body n
    ldr  x13, [x11]                  /* rax */
    ldr  x14, [x11, 16]              /* rdx */
    cmp  x10, 4
    b.eq 4f
    cmp  x10, 5
    b.eq 5f
    tbnz x8, 8, 1f
    /* ---- 32-bit divides: dividend = edx:eax as one 64-bit value ---- */
    mov  w12, w12
    cbz  w12, 9f                     /* #DE: let C raise it */
    mov  w13, w13
    lsl  x15, x14, 32
    orr  x15, x15, x13               /* dividend */
    cmp  x10, 6
    b.ne 2f
    udiv x16, x15, x12               /* DIV: quotient must fit 32 bits */
    lsr  x17, x16, 32
    cbnz x17, 9f
    msub x17, x16, x12, x15          /* remainder */
    b    3f
2:  sxtw x12, w12                    /* IDIV: signed dividend / signed divisor */
    sdiv x16, x15, x12
    sxtw x17, w16
    cmp  x17, x16
    b.ne 9f                          /* quotient out of int32 range: #DE */
    msub x17, x16, x12, x15
3:  mov  w16, w16
    mov  w17, w17
    str  x16, [x11]
    str  x17, [x11, 16]
    gret \n
1:  /* ---- 64-bit divides ---- */
    cbz  x12, 9f
    cmp  x10, 6
    b.ne 6f
    cbnz x14, 9f                     /* DIV with rdx != 0: 128-bit dividend -> C */
    udiv x16, x13, x12
    msub x17, x16, x12, x13
    b    7f
6:  asr  x17, x13, 63
    cmp  x17, x14
    b.ne 9f                          /* IDIV with a real 128-bit dividend -> C */
    cmn  x12, 1
    b.ne 60f
    mov  x17, 1
    lsl  x17, x17, 63
    cmp  x13, x17
    b.eq 9f                          /* MIN / -1: #DE */
60: sdiv x16, x13, x12
    msub x17, x16, x12, x13
7:  str  x16, [x11]
    str  x17, [x11, 16]
    gret \n
4:  /* ---- MUL: rdx:rax = rax * src, CF=OF = high part != 0 ---- */
    tbnz x8, 8, 40f
    umull x15, w13, w12
    lsr  x16, x15, 32
    mov  w15, w15
    str  x15, [x11]
    str  x16, [x11, 16]
    cmp  x16, 0
    cset w16, ne
    amd64_muldiv_flags
    gret \n
40: mul  x15, x13, x12
    umulh x16, x13, x12
    str  x15, [x11]
    str  x16, [x11, 16]
    cmp  x16, 0
    cset w16, ne
    amd64_muldiv_flags
    gret \n
5:  /* ---- IMUL one operand: signed, CF=OF = high part is not the sign of the low ---- */
    tbnz x8, 8, 50f
    smull x15, w13, w12              /* signed 64-bit product of eax * src */
    sxtw x17, w15
    cmp  x17, x15
    cset w16, ne                     /* overflow: the low dword does not sign-extend to it */
    lsr  x17, x15, 32                /* edx = high dword, zero-extended */
    mov  w15, w15                    /* eax = low dword */
    str  x15, [x11]
    str  x17, [x11, 16]
    amd64_muldiv_flags
    gret \n
50: mul  x15, x13, x12
    smulh x17, x13, x12
    str  x15, [x11]
    str  x17, [x11, 16]
    asr  x15, x15, 63
    cmp  x15, x17
    cset w16, ne
    amd64_muldiv_flags
    gret \n
9:  /* ---- #DE, and 64-bit dividends that do not fit 64 bits ---- */
    tbz  x8, 8, 98f                  /* a 32-bit divide that reached here: #DE */
    cbz  x12, 98f
    cmp  x10, 6
    b.ne 95f
    cmp  x14, x12                    /* DIV: rdx >= divisor, the quotient */
    b.hs 98f                         /* would not fit 64 bits */
    bl   amd64_udiv128
    b    7b
95: /* IDIV: on the magnitudes, then the signs */
    lsr  x4, x14, 63                 /* the dividend's sign */
    lsr  x5, x12, 63                 /* the divisor's */
    cbz  x4, 96f
    negs x13, x13
    ngc  x14, x14
96: cbz  x5, 97f
    neg  x12, x12
97: cmp  x14, x12
    b.hs 98f
    stp  x4, x5, [sp, -16]!
    bl   amd64_udiv128
    ldp  x4, x5, [sp], 16
    eor  x6, x4, x5                  /* the quotient's sign */
    mov  x7, 1
    lsl  x7, x7, 63
    cbz  x6, 93f
    cmp  x16, x7                     /* negative: down to -2^63 */
    b.hi 98f
    neg  x16, x16
    b    94f
93: cmp  x16, x7                     /* positive: up to 2^63 - 1 */
    b.hs 98f
94: cbz  x4, 7b                      /* the remainder takes the dividend's sign */
    neg  x17, x17
    b    7b
98: mov  x0, INT_DIV                 /* #DE at the instruction (rip was flushed) */
    b    jit_exit
.endm

# x14:x13 / x12, unsigned, with x14 < x12 (so the quotient fits): x16 the
# quotient, x17 the remainder. Restoring division, a bit at a time; a carry
# out of x14 means the 65-bit partial remainder is past the divisor.
# Clobbers x5, x6, x13, x14.
amd64_udiv128:
    mov  x16, 0
    mov  x5, 64
1:  lsr  x6, x14, 63
    extr x14, x14, x13, 63
    lsl  x13, x13, 1
    lsl  x16, x16, 1
    cbnz x6, 2f
    cmp  x14, x12
    b.lo 3f
2:  sub  x14, x14, x12
    orr  x16, x16, 1
3:  subs x5, x5, 1
    b.ne 1b
    mov  x17, x14
    ret

.gadget amd64_grp3_muldiv
    ldr  x8, [_ip]
    and  x9, x8, 0xf
    ubfx x10, x8, 4, 3               /* group: 4 mul, 5 imul, 6 div, 7 idiv */
    add  x11, _cpu, CPU_amd64_regs
    ldr  x12, [x11, x9, lsl 3]       /* src */
    amd64_muldiv_body 1

# The memory forms (F7 /4-/7, mod != 3), 32/64. [meta][disp][next_ip][packed
# = group(4-6) | is64(8)], gret 4.
.macro amd64_gen_muldiv_mem name, vbytes
.gadget amd64_\name
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    amd64_vmem_addr
    amd64_vread_prep \vbytes, \name
.if \vbytes == 4
    ldr  w12, [x9]
.else
    ldr  x12, [x9]
.endif
    ldr  x8, [_ip, 24]
    ubfx x10, x8, 4, 3
    add  x11, _cpu, CPU_amd64_regs
    amd64_muldiv_body 4
    amd64_vread_bullshit \vbytes, \name
.endm
amd64_gen_muldiv_mem muldiv_mem32, 4
amd64_gen_muldiv_mem muldiv_mem64, 8

# MUL/IMUL/DIV/IDIV at 8 bits (F6 /4-/7: AX = AL * src, AL/AH = AX / src)
# and 16 (66 F7 /4-/7: DX:AX). The rest of RAX and RDX is kept; a byte form
# leaves RDX alone. x8 packed (group 4-6 | 16-bit 9), x11 the register file,
# x12 the source (masked to the width). #DE through jit_exit (rip flushed).
.macro amd64_muldiv_small_body n
    ldr  x13, [x11]                  /* rax */
    ldr  x14, [x11, 16]              /* rdx */
    tbnz x8, 9, 20f
    /* ---- 8 bits ---- */
    cmp  x10, 5
    b.hi 12f
    b.eq 11f
    and  x4, x13, 0xff               /* MUL: AX = AL * src */
    mul  x15, x4, x12
    lsr  x6, x15, 8
    cmp  x6, 0
    cset w16, ne
    b    13f
11: sxtb x4, w13                     /* IMUL */
    sxtb x5, w12
    mul  x15, x4, x5
    sxtb x6, w15
    cmp  x6, x15
    cset w16, ne
13: bfi  x13, x15, 0, 16
    str  x13, [x11]
    amd64_muldiv_flags
    gret \n
12: cbz  x12, 98f
    cmp  x10, 7
    b.eq 14f
    and  x15, x13, 0xffff            /* DIV: AX / src */
    udiv x16, x15, x12
    cmp  x16, 0xff
    b.hi 98f
    msub x17, x16, x12, x15
    b    15f
14: sxth x15, w13                    /* IDIV */
    sxtb x5, w12
    sdiv x16, x15, x5
    sxtb x6, w16
    cmp  x6, x16
    b.ne 98f                         /* the quotient does not fit int8 */
    msub x17, x16, x5, x15
15: bfi  x13, x16, 0, 8              /* AL = quotient, AH = remainder */
    bfi  x13, x17, 8, 8
    str  x13, [x11]
    gret \n
20: /* ---- 16 bits ---- */
    cmp  x10, 5
    b.hi 22f
    b.eq 21f
    and  x4, x13, 0xffff             /* MUL: DX:AX = AX * src */
    mul  x15, x4, x12
    lsr  x6, x15, 16
    cmp  x6, 0
    cset w16, ne
    b    23f
21: sxth x4, w13                     /* IMUL */
    sxth x5, w12
    mul  x15, x4, x5
    sxth x6, w15
    cmp  x6, x15
    cset w16, ne
23: bfi  x13, x15, 0, 16
    lsr  x6, x15, 16
    bfi  x14, x6, 0, 16
    str  x13, [x11]
    str  x14, [x11, 16]
    amd64_muldiv_flags
    gret \n
22: cbz  x12, 98f
    and  x15, x13, 0xffff
    bfi  x15, x14, 16, 16            /* DX:AX */
    cmp  x10, 7
    b.eq 24f
    udiv x16, x15, x12               /* DIV */
    lsr  x6, x16, 16
    cbnz x6, 98f                     /* the quotient does not fit 16 bits */
    msub x17, x16, x12, x15
    b    25f
24: sxtw x15, w15                    /* IDIV */
    sxth x5, w12
    sdiv x16, x15, x5
    sxth x6, w16
    cmp  x6, x16
    b.ne 98f                         /* the quotient does not fit int16 */
    msub x17, x16, x5, x15
25: bfi  x13, x16, 0, 16             /* AX = quotient, DX = remainder */
    bfi  x14, x17, 0, 16
    str  x13, [x11]
    str  x14, [x11, 16]
    gret \n
98: mov  x0, INT_DIV
    b    jit_exit
.endm

# Register source: packed(0) = rm (0-3) | group (4-6) | 16-bit (9) | AH..BH (10). gret 1.
.gadget amd64_grp3_muldiv_small
    ldr  x8, [_ip]
    and  x9, x8, 0xf
    ubfx x10, x8, 4, 3
    add  x11, _cpu, CPU_amd64_regs
    ldr  x12, [x11, x9, lsl 3]
    tbz  x8, 10, 1f
    lsr  x12, x12, 8
1:  tbnz x8, 9, 2f
    and  x12, x12, 0xff
    b    3f
2:  and  x12, x12, 0xffff
3:  amd64_muldiv_small_body 1

# Memory source: [meta][disp][next_ip][packed = group(4-6) | 16-bit (9)]. gret 4.
.macro amd64_gen_muldiv_mem_small name, vbytes
.gadget amd64_\name
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    amd64_vmem_addr
    amd64_vread_prep \vbytes, \name
.if \vbytes == 1
    ldrb w12, [x9]
.else
    ldrh w12, [x9]
.endif
    ldr  x8, [_ip, 24]
    ubfx x10, x8, 4, 3
    add  x11, _cpu, CPU_amd64_regs
    amd64_muldiv_small_body 4
    amd64_vread_bullshit \vbytes, \name
.endm
amd64_gen_muldiv_mem_small muldiv_mem8, 1
amd64_gen_muldiv_mem_small muldiv_mem16, 2

# TEST [mem], imm (F6 /0 byte, F7 /0 16/32/64): flags only, read path. The
# immediate is already sized/extended by gen.c. Operands:
# [meta][disp][next_ip][imm]. gret 4.
.macro amd64_gen_test_mem_imm name, vbytes
.gadget amd64_\name
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    amd64_vmem_addr
    amd64_vread_prep \vbytes, \name
.if \vbytes == 1
    ldrb w13, [x9]
.elseif \vbytes == 2
    ldrh w13, [x9]
.elseif \vbytes == 4
    ldr  w13, [x9]
.else
    ldr  x13, [x9]
.endif
    ldr  x10, [_ip, 24]
    and  x15, x13, x10
    mov  x11, (\vbytes * 8)
    amd64_cached_set_logic_flags x15, x11
    gret 4
    amd64_vread_bullshit \vbytes, \name
.endm
amd64_gen_test_mem_imm test_mem_imm8, 1
amd64_gen_test_mem_imm test_mem_imm16, 2
amd64_gen_test_mem_imm test_mem_imm32, 4
amd64_gen_test_mem_imm test_mem_imm64, 8

# CMOVcc r, [mem] (0F 40+cc, mod!=3), 16/32/64: the register form's condition
# evaluation with the source read from memory. The load happens whether or
# not the move is taken (as on hardware, so a bad address faults either way).
# Operands: [meta][disp][next_ip][dst]. gret 4.
.macro amd64_gen_cmov_mem name, cond, pol, vbytes
    .gadget amd64_cmov\pol\()_\name\()_mem\vbytes
        ldr  x8, [_ip]
        ldr  x9, [_ip, 8]
        amd64_vmem_addr
        amd64_vread_prep \vbytes, cmov\pol\()_\name\()_mem\vbytes
    .if \vbytes == 2
        ldrh w9, [x9]
    .elseif \vbytes == 4
        ldr  w9, [x9]
    .else
        ldr  x9, [x9]
    .endif
        mov  x0, x9                  /* source value (x0 is free after the prep) */
        amd64_do_jump \cond, 1f
        .ifc \pol,n
            mov  x16, 1
        .else
            mov  x16, 0
        .endif
        b    2f
    1:
        .ifc \pol,n
            mov  x16, 0
        .else
            mov  x16, 1
        .endif
    2:  ldr  x12, [_ip, 24]          /* dst slot */
        add  x14, _cpu, CPU_amd64_regs
        ldr  x10, [x14, x12, lsl 3]  /* current dst */
        cmp  x16, 0
        csel x15, x0, x10, ne        /* taken ? src : dst */
    .if \vbytes == 2
        bfi  x10, x15, 0, 16         /* 16-bit: the upper 48 kept */
        mov  x15, x10
    .elseif \vbytes == 4
        mov  w15, w15                /* 32-bit: zero-extend either way */
    .endif
        str  x15, [x14, x12, lsl 3]
        gret 4
        amd64_vread_bullshit \vbytes, cmov\pol\()_\name\()_mem\vbytes
.endm
.irp cond, o,c,z,cz,s,p,sxo,sxoz
    amd64_gen_cmov_mem \cond, \cond, , 2
    amd64_gen_cmov_mem \cond, \cond, n, 2
    amd64_gen_cmov_mem \cond, \cond, , 4
    amd64_gen_cmov_mem \cond, \cond, n, 4
    amd64_gen_cmov_mem \cond, \cond, , 8
    amd64_gen_cmov_mem \cond, \cond, n, 8
.endr

# ===========================================================================
# Round 3b: the SSE2 integer family with register AND memory sources, the
# shuffles (table-driven), pextrw/pinsrw, comis/ucomis, movhlps/movlhps.
#
# Conventions (as the vector gadgets above): reg forms take one operand word,
# src xmm id in bits 0-3 and dst in 4-7, touch only cpu->xmm[] and leave the
# GPR cache alone (gret 1). Mem forms take [meta][disp][next_ip] with the dst
# xmm id in meta bits 8-11 and read 16 bytes through amd64_vread_prep 16
# (gret 3); gen.c flushes the reg cache and rip before them. NEON v0-v3 are
# scratch and dead after gret.
# ===========================================================================

# (The SSE2 integer ops themselves are amd64_vi_*, above.)

# Shuffles, table driven: gen.c turns the imm8 into a 16-byte tbl index vector
# (two operand words) at compile time. tbl2 indexes a {dst, src} pair (0-31,
# for shufps/shufpd: low lanes from dst, high lanes from src); tbl1 indexes
# the source alone (pshufd/pshuflw/pshufhw). Reg: [packed][idx_lo][idx_hi],
# gret 3. Mem: [meta][disp][next_ip][idx_lo][idx_hi], gret 5.
.gadget amd64_v_tbl2_reg
    ldr  x8, [_ip]
    and  x9, x8, 0xf
    ubfx x10, x8, 4, 4
    add  x11, _cpu, CPU_xmm
    add  x9, x11, x9, lsl 4
    add  x10, x11, x10, lsl 4
    ldr  q0, [x10]
    ldr  q1, [x9]
    ldr  q2, [_ip, 8]
    tbl  v0.16b, {v0.16b, v1.16b}, v2.16b
    str  q0, [x10]
    gret 3
.gadget amd64_v_tbl2_mem
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    ubfx x10, x8, 8, 4
    amd64_vmem_addr
    amd64_vread_prep 16, tbl2_mem
    ldp  x12, x13, [x9]
    fmov d1, x12
    mov  v1.d[1], x13
    add  x14, _cpu, CPU_xmm
    add  x14, x14, x10, lsl 4
    ldr  q0, [x14]
    ldr  q2, [_ip, 24]
    tbl  v0.16b, {v0.16b, v1.16b}, v2.16b
    str  q0, [x14]
    gret 5
    amd64_vread_bullshit 16, tbl2_mem
.gadget amd64_v_tbl1_reg
    ldr  x8, [_ip]
    and  x9, x8, 0xf
    ubfx x10, x8, 4, 4
    add  x11, _cpu, CPU_xmm
    add  x9, x11, x9, lsl 4
    add  x10, x11, x10, lsl 4
    ldr  q1, [x9]
    ldr  q2, [_ip, 8]
    tbl  v0.16b, {v1.16b}, v2.16b
    str  q0, [x10]
    gret 3
.gadget amd64_v_tbl1_mem
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    ubfx x10, x8, 8, 4
    amd64_vmem_addr
    amd64_vread_prep 16, tbl1_mem
    ldp  x12, x13, [x9]
    fmov d1, x12
    mov  v1.d[1], x13
    add  x14, _cpu, CPU_xmm
    add  x14, x14, x10, lsl 4
    ldr  q2, [_ip, 24]
    tbl  v0.16b, {v1.16b}, v2.16b
    str  q0, [x14]
    gret 5
    amd64_vread_bullshit 16, tbl1_mem

# pextrw r32, xmm, imm8 (66 0F C5): a halfword out of the xmm image, zero-
# extended into the GPR. Flush style (writes a GPR).
# packed = src xmm(0-3) | dst gpr(4-7) | lane(8-10). gret 1.
.gadget amd64_v_pextrw
    ldr  x8, [_ip]
    and  x9, x8, 0xf
    ubfx x10, x8, 4, 4
    ubfx x11, x8, 8, 3
    add  x12, _cpu, CPU_xmm
    add  x12, x12, x9, lsl 4
    ldrh w13, [x12, x11, lsl 1]
    add  x14, _cpu, CPU_amd64_regs
    str  x13, [x14, x10, lsl 3]
    gret 1
# pinsrw xmm, r32, imm8 (66 0F C4, mod==3): the low halfword of the GPR into
# lane imm&7. packed = src gpr(0-3) | dst xmm(4-7) | lane(8-10). gret 1.
.gadget amd64_v_pinsrw_reg
    ldr  x8, [_ip]
    and  x9, x8, 0xf
    ubfx x10, x8, 4, 4
    ubfx x11, x8, 8, 3
    add  x14, _cpu, CPU_amd64_regs
    ldr  x13, [x14, x9, lsl 3]
    add  x12, _cpu, CPU_xmm
    add  x12, x12, x10, lsl 4
    strh w13, [x12, x11, lsl 1]
    gret 1
# pinsrw xmm, m16, imm8: [meta][disp][next_ip][lane]. gret 4.
.gadget amd64_v_pinsrw_mem
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    ubfx x10, x8, 8, 4
    amd64_vmem_addr
    amd64_vread_prep 2, pinsrw_mem
    ldrh w13, [x9]
    ldr  x11, [_ip, 24]
    add  x12, _cpu, CPU_xmm
    add  x12, x12, x10, lsl 4
    strh w13, [x12, x11, lsl 1]
    gret 4
    amd64_vread_bullshit 2, pinsrw_mem

# comiss/ucomiss (0F 2F/2E) and comisd/ucomisd (66 ...): ZF/PF/CF from an
# ordered compare -- unordered sets all three, less sets CF, equal sets ZF --
# and OF/SF/AF cleared. Written straight into eflags (and the CF/OF bytes),
# lazy bits cleared. \fcmp is "fcmp s0, s1" or "fcmp d0, d1".
.macro amd64_v_comis_flags
    /* NZCV after fcmp: unordered -> C=1,V=1; lt -> N=1; eq -> Z=1; gt -> C=1 */
    mov  w15, 0
    b.vc 1f
    mov  w15, (1 | (1 << 2) | (1 << 6))     /* unordered: CF|PF|ZF */
    b    3f
1:  b.pl 2f
    mov  w15, 1                              /* less: CF */
    b    3f
2:  b.ne 3f
    mov  w15, (1 << 6)                       /* equal: ZF */
3:  ldr  w17, [_cpu, CPU_eflags]
    mov  w16, (1 | (1 << 2) | (1 << 4) | (1 << 6) | (1 << 7) | (1 << 11))
    bic  w17, w17, w16
    orr  w17, w17, w15
    str  w17, [_cpu, CPU_eflags]
    and  w16, w15, 1
    strb w16, [_cpu, CPU_cf]
    strb wzr, [_cpu, CPU_of]
    ldrb w16, [_cpu, CPU_flags_res]
    bic  w16, w16, (ZF_RES | SF_RES | PF_RES | AF_OPS)
    strb w16, [_cpu, CPU_flags_res]
.endm

# (U)COMISS/SD: lane 0, DAZ/DE as the rest, then the compare into EFLAGS:
# fcmpe (COMIS: IE for any NaN) or fcmp (UCOMIS: for an SNaN).
.macro amd64_xf_comis name, t, a, cmp
    xf_frame_r \name
    xf_consts \t
    xf_lanes \t, 0
    xf_pre2 \a, 0
    \cmp \t\()4, \t\()5
    amd64_v_comis_flags
    gret 1
.ifc \t,s
    xf_frame_m \name, 4
.else
    xf_frame_m \name, 8
.endif
    xf_consts \t
    xf_lanes \t, 0
    xf_pre2 \a, 0
    \cmp \t\()4, \t\()5
    amd64_v_comis_flags
    gret 4
.ifc \t,s
    amd64_vread_bullshit 4, xf_\name\()_m
.else
    amd64_vread_bullshit 8, xf_\name\()_m
.endif
.endm
amd64_xf_comis comiss, s, 4s, fcmpe
amd64_xf_comis ucomiss, s, 4s, fcmp
amd64_xf_comis comisd, d, 2d, fcmpe
amd64_xf_comis ucomisd, d, 2d, fcmp

# movhlps (0F 12, mod==3): dst.low = src.high. movlhps (0F 16, mod==3):
# dst.high = src.low. packed = src(0-3) | dst(4-7). gret 1.
.gadget amd64_v_movhlps_reg
    ldr  x8, [_ip]
    and  x9, x8, 0xf
    ubfx x10, x8, 4, 4
    add  x11, _cpu, CPU_xmm
    add  x9, x11, x9, lsl 4
    add  x10, x11, x10, lsl 4
    ldr  x12, [x9, 8]
    str  x12, [x10]
    gret 1
.gadget amd64_v_movlhps_reg
    ldr  x8, [_ip]
    and  x9, x8, 0xf
    ubfx x10, x8, 4, 4
    add  x11, _cpu, CPU_xmm
    add  x9, x11, x9, lsl 4
    add  x10, x11, x10, lsl 4
    ldr  x12, [x9]
    str  x12, [x10, 8]
    gret 1

# ===========================================================================
# Round 4: the 16-bit family (imm-to-mem arith/logic, CMP mem<->r16, shifts),
# TZCNT/LZCNT as a mode of the bit-scan gadgets, fences need no gadget.
#
# 16-bit flag rule (as imm_cmp16): compute on zero-extended halves in a
# 32-bit register; CF is bit 16 of the raw result (borrow/carry out of bit
# 15), OF is ((lhs ^ rhs) & (lhs ^ res)) bit 15 for a subtract and
# (~(lhs ^ rhs) & (lhs ^ res)) bit 15 for an add; the stored result is the
# low 16 bits, and ZF/SF(15)/PF/AF come from amd64_cached_set_addsub_flags 15.
# ===========================================================================

# w13 = lhs (0..0xffff), w10 = rhs (0..0xffff), x9 = group (0 add, 5 sub).
# Out: w15 = 16-bit result, w14 = CF, w16 = OF; x13/x10 preserved for the
# flag macro's AF.
.macro amd64_w16_addsub
    cmp  x9, 0
    b.eq 1f
    sub  w15, w13, w10
    lsr  w14, w15, 16
    and  w14, w14, 1
    eor  w16, w13, w10
    eor  w17, w13, w15
    and  w16, w16, w17
    b    2f
1:  add  w15, w13, w10
    lsr  w14, w15, 16
    and  w14, w14, 1
    eor  w16, w13, w10
    eor  w17, w13, w15
    bic  w16, w17, w16
2:  lsr  w16, w16, 15
    and  w16, w16, 1
    and  w15, w15, 0xffff
.endm

# op word [mem], imm (66 81 /0,/5 imm16; 66 83 /0,/5 imm8 sign-extended),
# mod!=3, no LOCK. Dual-prep RMW. Operands: meta/disp/next_ip/imm/group, gret 5.
.gadget amd64_imm_arith16
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    amd64_vmem_addr
    mov  x0, x9
    amd64_vread_prep 2, imm_arith16_r
    ldrh w13, [x9]
    ldr  x10, [_ip, 24]
    and  w10, w10, 0xffff
    ldr  x9, [_ip, 32]
    amd64_w16_addsub
    amd64_cached_set_addsub_flags 15
    mov  x10, x15
    mov  x9, x0
    amd64_vwrite_prep 2, imm_arith16_w
    strh w10, [x9]
    amd64_vwrite_done 2, imm_arith16_w
    gret 5
    amd64_vread_bullshit 2, imm_arith16_r
    amd64_vwrite_bullshit 2, imm_arith16_w

# or/and/xor word [mem], imm (66 81/83 /1,/4,/6). gret 5.
.gadget amd64_imm_logic16
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    amd64_vmem_addr
    mov  x0, x9
    amd64_vread_prep 2, imm_logic16_r
    ldrh w13, [x9]
    ldr  x10, [_ip, 24]
    and  w10, w10, 0xffff
    ldr  x9, [_ip, 32]
    cmp  x9, 4
    b.eq 1f
    cmp  x9, 6
    b.eq 2f
    orr  w15, w13, w10
    b    3f
1:  and  w15, w13, w10
    b    3f
2:  eor  w15, w13, w10
3:  and  w15, w15, 0xffff
    mov  x11, 16
    amd64_cached_set_logic_flags x15, x11
    mov  x10, x15
    mov  x9, x0
    amd64_vwrite_prep 2, imm_logic16_w
    strh w10, [x9]
    amd64_vwrite_done 2, imm_logic16_w
    gret 5
    amd64_vread_bullshit 2, imm_logic16_r
    amd64_vwrite_bullshit 2, imm_logic16_w

# CMP word [mem], r16 (66 39): lhs = memory, rhs = register. [meta][disp][next_ip][reg], gret 4.
.gadget amd64_cmp_memreg16
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    amd64_vmem_addr
    amd64_vread_prep 2, cmp_memreg16
    ldrh w13, [x9]
    ldr  x12, [_ip, 24]
    add  x14, _cpu, CPU_amd64_regs
    ldr  x10, [x14, x12, lsl 3]
    and  w10, w10, 0xffff
    mov  x9, 5
    amd64_w16_addsub
    amd64_cached_set_addsub_flags 15
    gret 4
    amd64_vread_bullshit 2, cmp_memreg16

# CMP r16, word [mem] (66 3b): lhs = register, rhs = memory. [meta][disp][next_ip], reg in
# meta bits 8-11, gret 3.
.gadget amd64_regmem_cmp16
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    amd64_vmem_addr
    amd64_vread_prep 2, regmem_cmp16
    ldrh w10, [x9]
    ldr  x8, [_ip]
    ubfx x12, x8, 8, 4
    add  x14, _cpu, CPU_amd64_regs
    ldr  x13, [x14, x12, lsl 3]
    and  w13, w13, 0xffff
    mov  x9, 5
    amd64_w16_addsub
    amd64_cached_set_addsub_flags 15
    gret 3
    amd64_vread_bullshit 2, regmem_cmp16

# SHL/SHR/SAR r/m16 by 1 (66 d1), CL (66 d3) or imm8 (66 c1), mod==3. Same shape as
# shift8 with a 16-bit width: count masked by 31, CF the last bit out (0 past
# the width), OF for count 1 only, result written into the low 16 bits.
# packed = rm(0-3) | group(4-7) | count(8-15) | use_cl(17). gret 1.
.gadget amd64_shift16
    ldr  x8, [_ip]
    and  x11, x8, 0xf
    ubfx x9, x8, 4, 4
    ubfx x10, x8, 8, 8
    add  x13, _cpu, CPU_amd64_regs
    tbz  x8, 17, 1f
    ldrb w10, [x13, 8]               /* CL */
1:  add  x11, x13, x11, lsl 3        /* &regs[rm] */
    and  x10, x10, 31
    cbz  x10, 9f
    ldrh w13, [x11]                  /* operand, zero-extended */
    cmp  x9, 4
    b.eq 3f
    cmp  x9, 5
    b.eq 4f
    sxth w13, w13                    /* SAR on the sign-extended value */
    asr  w15, w13, w10
    sub  w12, w10, 1
    lsr  w14, w13, w12
    b    5f
3:  lsl  w15, w13, w10               /* SHL */
    mov  w12, 16
    sub  w12, w12, w10
    lsr  w14, w13, w12               /* CF = bit (16-count); 0 past 16 */
    b    5f
4:  lsr  w15, w13, w10               /* SHR */
    sub  w12, w10, 1
    lsr  w14, w13, w12
5:  and  w14, w14, 1
    mov  w16, 0
    cmp  x10, 1
    b.ne 6f
    cmp  x9, 4
    b.eq 7f
    cmp  x9, 5
    b.eq 8f
    b    6f
7:  lsr  w16, w15, 15                /* SHL: OF = bit15(res) ^ CF */
    and  w16, w16, 1
    eor  w16, w16, w14
    b    6f
8:  lsr  w16, w13, 15                /* SHR: OF = sign of the source */
6:  and  w15, w15, 0xffff
    strh w15, [x11]                  /* low 16 of the slot; upper 48 preserved */
    amd64_cached_set_shift_flags 15
9:  gret 1

# ===========================================================================
# Scalar int <-> float conversions: F2/F3 0F 2A cvtsi2sd/cvtsi2ss, 0F 2C
# cvttsd2si/cvttss2si, 0F 2D cvtsd2si/cvtss2si.
#
# x86 semantics, not aarch64's: a NaN or out-of-range source converts to the
# integer indefinite (0x80000000 / 0x8000000000000000) where fcvtzs would
# saturate. fcvtzs already yields 0x80000000 for anything below the range, so
# only the high side and NaN need the substitute: after `fcmp src, 2^N` the
# condition PL (N clear) is exactly "src >= 2^N or unordered". The rounding
# forms (0F 2D) go through frintx first -- the host FPCR's rounding mode,
# which is MXCSR.RC while guest code runs (emu/fpenv.c), raising inexact as
# CVTSD2SI does -- so the range check sees the ROUNDED value: 2147483647.6
# rounds to 2^31 and must be indefinite. cvtsi2sd/ss
# write only the low lane (a d/s-sized store), the other lanes survive.
#
# Reg forms: packed = src(0-3) | dst(4-7) | REX.W(8), gret 1. Mem forms:
# [meta][disp][next_ip], dst in meta bits 8-11, plus [w] as a 4th word for
# the int destinations (gret 4); the int sources have per-width gadgets
# (gret 3). All flush style: a GPR is read or written every time.
# ===========================================================================

# In: s0 (vbytes==4) or d0 = source, x12 bit 0 = REX.W. Out: x13 = result,
# zero-extended when 32-bit. Clobbers x14, v0, v1.
.macro amd64_v_f2i_convert vbytes, round
    .if \vbytes == 4
        .if \round
            frintx s0, s0
        .endif
        tbnz x12, 0, 1f
        fcvtzs w13, s0
        mov  w14, 0x4f000000                 /* 2^31 as float */
        fmov s1, w14
        fcmp s0, s1
        mov  w14, 0x80000000
        csel w13, w14, w13, pl
        b    2f
1:      fcvtzs x13, s0
        mov  w14, 0x5f000000                 /* 2^63 as float */
        fmov s1, w14
        fcmp s0, s1
        mov  x14, 0x8000000000000000
        csel x13, x14, x13, pl
2:
    .else
        .if \round
            frintx d0, d0
        .endif
        tbnz x12, 0, 1f
        fcvtzs w13, d0
        mov  x14, 0x41e0000000000000         /* 2^31 as double */
        fmov d1, x14
        fcmp d0, d1
        mov  w14, 0x80000000
        csel w13, w14, w13, pl
        b    2f
1:      fcvtzs x13, d0
        mov  x14, 0x43e0000000000000         /* 2^63 as double */
        fmov d1, x14
        fcmp d0, d1
        mov  x14, 0x8000000000000000
        csel x13, x14, x13, pl
2:
    .endif
.endm

# ---- add/sub/or/and/xor reg,reg and reg,imm specialised per register ----
#
# The cached arith/logic gadgets select registers, size and operation at run
# time (compare chains, amd64_cached_read/_write); here each (op, size, dst,
# src) is its own gadget, in a full form that deposits the flags exactly as
# the generic one does (same flag macros, same register conventions: x13 lhs,
# x10 rhs, x15 result, w14 CF, w16 OF) and a no-flags twin that gen.c's
# dead-flag pass patches in. CMP keeps the generic gadget: the cmp+jcc fusion
# rewrites its operand words. 32-bit results zero-extend.
#
# All sixteen registers: rax..rdi are the x20-x27 cache, r8-r15 are read and
# written in their CPU_amd64_regs slot, which is where they always live -- so
# an r8-r15 operand no longer needs the cache written back and reloaded
# around it (a third of 7-Zip's instructions touch r8-r15, 2026-10-04).
# Tables [op][size64][dst][src] (reg,reg) and [op][size64][dst] (reg,imm,
# imm in the stream word after the gadget), 16 registers each, full and _nf
# alike. Generated by the amd64_r16_* macros below.

# One ALU instruction: op is add/sub/or/and/xor/shl/shr/sar; setf 1 = the
# flag-setting form (add/sub only).
.macro amd64_r16_op op, setf, rd, rn, rm
    .ifc \op, add
        .if \setf
        adds \rd, \rn, \rm
        .else
        add \rd, \rn, \rm
        .endif
    .endif
    .ifc \op, sub
        .if \setf
        subs \rd, \rn, \rm
        .else
        sub \rd, \rn, \rm
        .endif
    .endif
    .ifc \op, or
        orr \rd, \rn, \rm
    .endif
    .ifc \op, and
        and \rd, \rn, \rm
    .endif
    .ifc \op, xor
        eor \rd, \rn, \rm
    .endif
    .ifc \op, shl
        lsl \rd, \rn, \rm
    .endif
    .ifc \op, shr
        lsr \rd, \rn, \rm
    .endif
    .ifc \op, sar
        asr \rd, \rn, \rm
    .endif
.endm
# Register id -> its value in \R\()\scratch: a move from the cache, or a load
# from the CPU_amd64_regs slot.
.macro amd64_r16_get R, scratch, id, host
    .if \id < 8
    mov \R\()\scratch, \R\()\host
    .else
    ldr \R\()\scratch, [_cpu, CPU_amd64_regs + 8 * \id]
    .endif
.endm
# x\()\scratch (32-bit results already zero-extended) -> register id.
.macro amd64_r16_put scratch, id, host
    .if \id < 8
    mov x\host, x\scratch
    .else
    str x\scratch, [_cpu, CPU_amd64_regs + 8 * \id]
    .endif
.endm
# \R\()\host op= \R\()\rm, in place for a cached destination, else through
# \R\()13 and the slot.
.macro amd64_r16_op_into op, R, id, host, rm
    .if \id < 8
    amd64_r16_op \op, 0, \R\host, \R\host, \rm
    .else
    ldr \R\()13, [_cpu, CPU_amd64_regs + 8 * \id]
    amd64_r16_op \op, 0, \R\()13, \R\()13, \rm
    str x13, [_cpu, CPU_amd64_regs + 8 * \id]
    .endif
.endm

# add/sub reg,reg
.macro amd64_arr16 op, s, R, bits, nf, d, hd, sr, hs
.if \nf
.gadget amd64_arr_\op\()\s\()_\d\()_\sr\()_nf
    .if \sr < 8
    amd64_r16_op_into \op, \R, \d, \hd, \R\hs
    .else
    ldr \R\()10, [_cpu, CPU_amd64_regs + 8 * \sr]
    amd64_r16_op_into \op, \R, \d, \hd, \R\()10
    .endif
    gret
.else
.gadget amd64_arr_\op\()\s\()_\d\()_\sr
    amd64_r16_get \R, 13, \d, \hd
    amd64_r16_get \R, 10, \sr, \hs
    amd64_r16_op \op, 1, \R\()15, \R\()13, \R\()10
    .ifc \op, add
    cset w14, cs
    .else
    cset w14, cc
    .endif
    cset w16, vs
    amd64_cached_set_addsub_flags \bits
    amd64_r16_put 15, \d, \hd
    gret
.endif
.endm
# or/and/xor reg,reg
.macro amd64_lrr16 op, s, R, bits, nf, d, hd, sr, hs
.if \nf
.gadget amd64_lrr_\op\()\s\()_\d\()_\sr\()_nf
    .if \sr < 8
    amd64_r16_op_into \op, \R, \d, \hd, \R\hs
    .else
    ldr \R\()10, [_cpu, CPU_amd64_regs + 8 * \sr]
    amd64_r16_op_into \op, \R, \d, \hd, \R\()10
    .endif
    gret
.else
.gadget amd64_lrr_\op\()\s\()_\d\()_\sr
    .if \d < 8
        .if \sr < 8
        amd64_r16_op \op, 0, \R\()15, \R\hd, \R\hs
        .else
        ldr \R\()10, [_cpu, CPU_amd64_regs + 8 * \sr]
        amd64_r16_op \op, 0, \R\()15, \R\hd, \R\()10
        .endif
    .else
        ldr \R\()13, [_cpu, CPU_amd64_regs + 8 * \d]
        .if \sr < 8
        amd64_r16_op \op, 0, \R\()15, \R\()13, \R\hs
        .else
        ldr \R\()10, [_cpu, CPU_amd64_regs + 8 * \sr]
        amd64_r16_op \op, 0, \R\()15, \R\()13, \R\()10
        .endif
    .endif
    amd64_r16_put 15, \d, \hd
    mov x11, \s
    amd64_cached_set_logic_flags x15, x11
    gret
.endif
.endm
# add/sub reg,imm
.macro amd64_ari16 op, s, R, bits, nf, d, hd
.if \nf
.gadget amd64_ari_\op\()\s\()_\d\()_nf
    ldr x8, [_ip]
    amd64_r16_op_into \op, \R, \d, \hd, \R\()8
    gret 1
.else
.gadget amd64_ari_\op\()\s\()_\d
    amd64_r16_get \R, 13, \d, \hd
    ldr x10, [_ip]
    .if \s == 32
    mov w10, w10
    .endif
    amd64_r16_op \op, 1, \R\()15, \R\()13, \R\()10
    .ifc \op, add
    cset w14, cs
    .else
    cset w14, cc
    .endif
    cset w16, vs
    amd64_cached_set_addsub_flags \bits
    amd64_r16_put 15, \d, \hd
    gret 1
.endif
.endm
# or/and/xor reg,imm
.macro amd64_lri16 op, s, R, bits, nf, d, hd
.if \nf
.gadget amd64_lri_\op\()\s\()_\d\()_nf
    ldr x8, [_ip]
    amd64_r16_op_into \op, \R, \d, \hd, \R\()8
    gret 1
.else
.gadget amd64_lri_\op\()\s\()_\d
    ldr x13, [_ip]
    .if \d < 8
    amd64_r16_op \op, 0, \R\()15, \R\hd, \R\()13
    .else
    ldr \R\()10, [_cpu, CPU_amd64_regs + 8 * \d]
    amd64_r16_op \op, 0, \R\()15, \R\()10, \R\()13
    .endif
    amd64_r16_put 15, \d, \hd
    mov x11, \s
    amd64_cached_set_logic_flags x15, x11
    gret 1
.endif
.endm

# rol/ror r32/r64, imm8 per register: amd64_roi_<op><size>_<reg>, full and
# _nf. The stream word is the right-rotate amount (gen.c turns a rol by n
# into a ror by size - n), with bit 8 set for a rotate by one, the only
# count that defines OF; gen.c leaves a zero count to the generic gadget.
# CF and OF as amd64_gen_rotate_imm writes them, the rest untouched. Tables
# amd64_roi{,_nf}_gadgets[(op * 2 + size64) * 16 + reg].
.macro amd64_roi16 op, s, R, bits, nf, d, hd
.if \nf
.gadget amd64_roi_\op\()\s\()_\d\()_nf
    ldr x10, [_ip]
    amd64_r16_get \R, 13, \d, \hd
    ror \R\()15, \R\()13, \R\()10
    amd64_r16_put 15, \d, \hd
    gret 1
.else
.gadget amd64_roi_\op\()\s\()_\d
    ldr x10, [_ip]
    amd64_r16_get \R, 13, \d, \hd
    ror \R\()15, \R\()13, \R\()10
    amd64_r16_put 15, \d, \hd
    .ifc \op, rol
    and w14, w15, 1
    lsr \R\()16, \R\()15, \bits
    eor w16, w16, w14
    .else
    lsr \R\()14, \R\()15, \bits
    lsr \R\()16, \R\()15, \bits - 1
    and w16, w16, 1
    eor w16, w16, w14
    .endif
    strb w14, [_cpu, CPU_cf]
    ldr w17, [_cpu, CPU_eflags]
    bfi w17, w14, 0, 1
    tbz x10, 8, 1f
    strb w16, [_cpu, CPU_of]
    bfi w17, w16, 11, 1
1:  str w17, [_cpu, CPU_eflags]
    gret 1
.endif
.endm

# The register loops: \m is one of the families above. Ids 0-7 carry their
# cache register number, 8-15 a placeholder (they live in memory).
.macro amd64_r16_srcs m, op, s, R, bits, nf, d, hd
    \m \op, \s, \R, \bits, \nf, \d, \hd, 0, 20
    \m \op, \s, \R, \bits, \nf, \d, \hd, 1, 22
    \m \op, \s, \R, \bits, \nf, \d, \hd, 2, 23
    \m \op, \s, \R, \bits, \nf, \d, \hd, 3, 21
    \m \op, \s, \R, \bits, \nf, \d, \hd, 4, 27
    \m \op, \s, \R, \bits, \nf, \d, \hd, 5, 26
    \m \op, \s, \R, \bits, \nf, \d, \hd, 6, 24
    \m \op, \s, \R, \bits, \nf, \d, \hd, 7, 25
    \m \op, \s, \R, \bits, \nf, \d, \hd, 8, 0
    \m \op, \s, \R, \bits, \nf, \d, \hd, 9, 0
    \m \op, \s, \R, \bits, \nf, \d, \hd, 10, 0
    \m \op, \s, \R, \bits, \nf, \d, \hd, 11, 0
    \m \op, \s, \R, \bits, \nf, \d, \hd, 12, 0
    \m \op, \s, \R, \bits, \nf, \d, \hd, 13, 0
    \m \op, \s, \R, \bits, \nf, \d, \hd, 14, 0
    \m \op, \s, \R, \bits, \nf, \d, \hd, 15, 0
.endm
.macro amd64_r16_rr m, op, s, R, bits, nf
    amd64_r16_srcs \m, \op, \s, \R, \bits, \nf, 0, 20
    amd64_r16_srcs \m, \op, \s, \R, \bits, \nf, 1, 22
    amd64_r16_srcs \m, \op, \s, \R, \bits, \nf, 2, 23
    amd64_r16_srcs \m, \op, \s, \R, \bits, \nf, 3, 21
    amd64_r16_srcs \m, \op, \s, \R, \bits, \nf, 4, 27
    amd64_r16_srcs \m, \op, \s, \R, \bits, \nf, 5, 26
    amd64_r16_srcs \m, \op, \s, \R, \bits, \nf, 6, 24
    amd64_r16_srcs \m, \op, \s, \R, \bits, \nf, 7, 25
    amd64_r16_srcs \m, \op, \s, \R, \bits, \nf, 8, 0
    amd64_r16_srcs \m, \op, \s, \R, \bits, \nf, 9, 0
    amd64_r16_srcs \m, \op, \s, \R, \bits, \nf, 10, 0
    amd64_r16_srcs \m, \op, \s, \R, \bits, \nf, 11, 0
    amd64_r16_srcs \m, \op, \s, \R, \bits, \nf, 12, 0
    amd64_r16_srcs \m, \op, \s, \R, \bits, \nf, 13, 0
    amd64_r16_srcs \m, \op, \s, \R, \bits, \nf, 14, 0
    amd64_r16_srcs \m, \op, \s, \R, \bits, \nf, 15, 0
.endm
.macro amd64_r16_ri m, op, s, R, bits, nf
    \m \op, \s, \R, \bits, \nf, 0, 20
    \m \op, \s, \R, \bits, \nf, 1, 22
    \m \op, \s, \R, \bits, \nf, 2, 23
    \m \op, \s, \R, \bits, \nf, 3, 21
    \m \op, \s, \R, \bits, \nf, 4, 27
    \m \op, \s, \R, \bits, \nf, 5, 26
    \m \op, \s, \R, \bits, \nf, 6, 24
    \m \op, \s, \R, \bits, \nf, 7, 25
    \m \op, \s, \R, \bits, \nf, 8, 0
    \m \op, \s, \R, \bits, \nf, 9, 0
    \m \op, \s, \R, \bits, \nf, 10, 0
    \m \op, \s, \R, \bits, \nf, 11, 0
    \m \op, \s, \R, \bits, \nf, 12, 0
    \m \op, \s, \R, \bits, \nf, 13, 0
    \m \op, \s, \R, \bits, \nf, 14, 0
    \m \op, \s, \R, \bits, \nf, 15, 0
.endm
# All sizes and both forms of one family and op.
.macro amd64_r16_family loop, m, op
    \loop \m, \op, 32, w, 31, 0
    \loop \m, \op, 32, w, 31, 1
    \loop \m, \op, 64, x, 63, 0
    \loop \m, \op, 64, x, 63, 1
.endm
amd64_r16_family amd64_r16_rr, amd64_arr16, add
amd64_r16_family amd64_r16_rr, amd64_arr16, sub
amd64_r16_family amd64_r16_rr, amd64_lrr16, or
amd64_r16_family amd64_r16_rr, amd64_lrr16, and
amd64_r16_family amd64_r16_rr, amd64_lrr16, xor
amd64_r16_family amd64_r16_ri, amd64_ari16, add
amd64_r16_family amd64_r16_ri, amd64_ari16, sub
amd64_r16_family amd64_r16_ri, amd64_lri16, or
amd64_r16_family amd64_r16_ri, amd64_lri16, and
amd64_r16_family amd64_r16_ri, amd64_lri16, xor
amd64_r16_family amd64_r16_ri, amd64_roi16, rol
amd64_r16_family amd64_r16_ri, amd64_roi16, ror

# Tables.
.macro amd64_r16_quad fam, op, s, d, sr, nf
    .if \nf
    .quad NAME(gadget_amd64_\fam\()_\op\()\s\()_\d\()_\sr\()_nf)
    .else
    .quad NAME(gadget_amd64_\fam\()_\op\()\s\()_\d\()_\sr)
    .endif
.endm
.macro amd64_r16_quad1 fam, op, s, d, nf
    .if \nf
    .quad NAME(gadget_amd64_\fam\()_\op\()\s\()_\d\()_nf)
    .else
    .quad NAME(gadget_amd64_\fam\()_\op\()\s\()_\d)
    .endif
.endm
.macro amd64_r16_rr_row fam, op, s, d, nf
    .irp sr, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
        amd64_r16_quad \fam, \op, \s, \d, \sr, \nf
    .endr
.endm
.macro amd64_r16_rr_ops fam, op, nf
    .irp s, 32,64
        .irp d, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
            amd64_r16_rr_row \fam, \op, \s, \d, \nf
        .endr
    .endr
.endm
.macro amd64_r16_ri_ops fam, op, nf
    .irp s, 32,64
        .irp d, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
            amd64_r16_quad1 \fam, \op, \s, \d, \nf
        .endr
    .endr
.endm
.pushsection_rodata
.global.name amd64_arr_gadgets
    amd64_r16_rr_ops arr, add, 0
    amd64_r16_rr_ops arr, sub, 0
.global.name amd64_arr_nf_gadgets
    amd64_r16_rr_ops arr, add, 1
    amd64_r16_rr_ops arr, sub, 1
.global.name amd64_ari_gadgets
    amd64_r16_ri_ops ari, add, 0
    amd64_r16_ri_ops ari, sub, 0
.global.name amd64_ari_nf_gadgets
    amd64_r16_ri_ops ari, add, 1
    amd64_r16_ri_ops ari, sub, 1
.global.name amd64_lrr_gadgets
    amd64_r16_rr_ops lrr, or, 0
    amd64_r16_rr_ops lrr, and, 0
    amd64_r16_rr_ops lrr, xor, 0
.global.name amd64_lrr_nf_gadgets
    amd64_r16_rr_ops lrr, or, 1
    amd64_r16_rr_ops lrr, and, 1
    amd64_r16_rr_ops lrr, xor, 1
.global.name amd64_lri_gadgets
    amd64_r16_ri_ops lri, or, 0
    amd64_r16_ri_ops lri, and, 0
    amd64_r16_ri_ops lri, xor, 0
.global.name amd64_lri_nf_gadgets
    amd64_r16_ri_ops lri, or, 1
    amd64_r16_ri_ops lri, and, 1
    amd64_r16_ri_ops lri, xor, 1
.global.name amd64_roi_gadgets
    amd64_r16_ri_ops roi, rol, 0
    amd64_r16_ri_ops roi, ror, 0
.global.name amd64_roi_nf_gadgets
    amd64_r16_ri_ops roi, rol, 1
    amd64_r16_ri_ops roi, ror, 1
.popsection

# ---- `mov r32/r64, [base + disp]` and the store, specialised per register ----
#
# The generic amd64_mov_load*/store* decode a meta word at run time, read the
# base from CPU_amd64_regs, and need the register cache written back and
# dropped before them (and reloaded after) -- three dispatches around every
# memory move in a block that also does register arithmetic. These read the
# base from the cache and load into (or store from) the cache, with the
# register cache left live: one dispatch, no meta decode. Shapes: any of the
# sixteen registers and either a base + disp32 or rip-relative (the address
# is then a translation-time constant). Each gadget publishes the
# instruction's rip itself (CPU_amd64_rip), so no set_rip dispatch either.
#
# The cache is authoritative while these run, so two things must save it:
#   - their slow paths (TLB miss, write revalidation, page-crossing access):
#     each can end in amd64_vmem_segfault_* -> jit_exit, which only mirrors
#     the cache to the i386 slots; amd64_spill_low8 stores rax..rdi first;
#   - a host fault on the access itself (the stale-TLB race): the gadgets sit
#     between amd64_specmem_start and amd64_specmem_end, and the fault
#     handlers (jit/jit.c jit_amd64_fault_spill, app/hook.c) copy x20-x27 from
#     the faulting thread's state into its CPU_amd64_regs before the unwind.
# Both store x20-x27 whatever the instruction's operands, so gen.c loads the
# cache before every one of these, even when its registers are all r8-r15.
# Stream: [gadget][disp or address][start rip].  gret 2.
.macro amd64_spill_low8
    add x8, _cpu, CPU_amd64_regs
    str x20, [x8, 0]
    str x22, [x8, 8]
    str x23, [x8, 16]
    str x21, [x8, 24]
    str x27, [x8, 32]
    str x26, [x8, 40]
    str x24, [x8, 48]
    str x25, [x8, 56]
.endm
.macro amd64_svread_bullshit size, id
amd64_vmiss_\id:
    amd64_spill_low8
    bl amd64_vmem_read_miss
    b amd64_vback_\id
amd64_vxpage_\id:
    amd64_spill_low8
    mov x19, \size
    bl amd64_vmem_crosspage_read
    b amd64_vback_\id
.endm
.macro amd64_svwrite_prep size, id
    and  x8, x9, 0xfff
    cmp  x8, (0x1000-\size)
    b.hi amd64_vwxpage_\id
    ldr  x11, [_tlb, (-TLB_entries+TLB_mmu)]
    ldr  x8,  [_tlb, (-TLB_entries+TLB_mem_changes)]
    ldr  x12, [x11, MMU_changes]
    cmp  x8, x12
    b.ne amd64_vwslow_\id
    ldrb w12, [x11, MMU_requires_write_revalidate]
    cbz  w12, amd64_vwfast_\id
amd64_vwslow_\id:
    amd64_spill_low8
    bl   amd64_vmem_write_resolve
    b    amd64_vwback_\id
amd64_vwfast_\id:
    and  x8, x9, #0xfffffffffffff000
    str  x8, [_tlb, (-TLB_entries+TLB_dirty_page)]
    ubfx x11, x9, 12, TLB_BITS
    eor  x11, x11, x9, lsr #(12 + TLB_BITS)
    and  x11, x11, #(1 << TLB_BITS) - 1
    mov  w12, TLB_ENTRY_SIZE
    madd x11, x11, x12, _tlb
    ldr  x13, [x11, TLB_ENTRY_page_if_writable]
    cmp  x8, x13
    b.ne amd64_vwmiss_\id
    ldr  x13, [x11, TLB_ENTRY_data_minus_addr]
    add  x9, x13, x9
amd64_vwback_\id:
.endm
.macro amd64_svwrite_done size, id
    mov x8, LOCAL_value
    add x8, _cpu, x8
    cmp  x8, x9
    b.ne amd64_vwdone_\id
    amd64_spill_low8
    mov  x19, \size
    bl   amd64_vmem_crosspage_write
amd64_vwdone_\id:
.endm
.macro amd64_svwrite_bullshit size, id
amd64_vwmiss_\id:
    amd64_spill_low8
    bl   amd64_vmem_write_miss
    b    amd64_vwback_\id
amd64_vwxpage_\id:
    str  x9, [_cpu, LOCAL_value_addr]
    mov x9, LOCAL_value
    add x9, _cpu, x9
    b    amd64_vwback_\id
.endm
.p2align 4
.global NAME(amd64_specmem_start)
NAME(amd64_specmem_start):
# The memory gadgets take any of the sixteen registers as the data register
# and as the base, like the arith families (r8-r15 in their CPU_amd64_regs
# slot; an r8-r15 store source is read after the write prep, which can call
# C), base 16 = rip-relative, where the operand word is the absolute
# address, and base 17 = an indexed address: x3 (from the amd64_ea gadget
# emitted just before) + disp. Names: amd64_sld/sst<size>_<reg>_<base>,
# amd64_sldr/sstr<size>_<reg> for rip, _x for indexed;
# amd64_slo_<op><size>_<dst>_<base>, base r for rip, x for indexed.
#
# [base + disp] (or the absolute address for base 16, or x3 + disp for base
# 17: x3 holds base + index * scale, left by an amd64_ea gadget just before)
# -> x9.
.macro amd64_m16_addr b, hb
    ldr x9, [_ip]
    .if \b < 8
    add x9, x9, x\hb
    .elseif \b < 16
    ldr x10, [_cpu, CPU_amd64_regs + 8 * \b]
    add x9, x9, x10
    .elseif \b == 17
    add x9, x9, x3
    .endif
.endm
# mov r32/r64, [base + disp] (op ld) and mov [base + disp], r32/r64 (op st).
# Operands: [disp or address][insn rip]; gret 2.
.macro amd64_smem16 op, s, R, bits, nf, r, hr, b, hb
.ifc \op, ld
    .if \b == 16
    .gadget amd64_sldr\s\()_\r
    .elseif \b == 17
    .gadget amd64_sld\s\()_\r\()_x
    .else
    .gadget amd64_sld\s\()_\r\()_\b
    .endif
    ldr x8, [_ip, 8]
    str x8, [_cpu, CPU_amd64_rip]
    amd64_m16_addr \b, \hb
    amd64_vread_prep (\s / 8), sld\s\()_\r\()_\b
    .if \r < 8
    ldr \R\hr, [x9]
    .else
    ldr \R\()10, [x9]
    str x10, [_cpu, CPU_amd64_regs + 8 * \r]
    .endif
    gret 2
    amd64_svread_bullshit (\s / 8), sld\s\()_\r\()_\b
.else
    .if \b == 16
    .gadget amd64_sstr\s\()_\r
    .elseif \b == 17
    .gadget amd64_sst\s\()_\r\()_x
    .else
    .gadget amd64_sst\s\()_\r\()_\b
    .endif
    ldr x8, [_ip, 8]
    str x8, [_cpu, CPU_amd64_rip]
    amd64_m16_addr \b, \hb
    amd64_svwrite_prep (\s / 8), sst\s\()_\r\()_\b
    .if \r < 8
    str \R\hr, [x9]
    .else
    ldr \R\()10, [_cpu, CPU_amd64_regs + 8 * \r]
    str \R\()10, [x9]
    .endif
    amd64_svwrite_done (\s / 8), sst\s\()_\r\()_\b
    gret 2
    amd64_svwrite_bullshit (\s / 8), sst\s\()_\r\()_\b
.endif
.endm

# add/sub/cmp/or/and/xor r32/r64, [base + disp] or [rip + disp], and cmp
# [mem], r32/r64 (op cmpm, the memory operand on the left): amd64_slo_*
# (ops in table order add sub cmp cmpm or and xor), full and _nf like
# the arith families. The _nf CMPs still perform the load: the access can
# fault even when its flags are dead. Operands: [disp or address][insn
# rip]; gret 2.
# The or/and/xor r, [mem] body of amd64_slo16: the loaded operand is in
# \R\()10, the result goes to the register with logic flags (full) or none.
.macro amd64_slo_logic op, s, R, nf, d, hd
    ldr \R\()10, [x9]
    .if \nf
    amd64_r16_op_into \op, \R, \d, \hd, \R\()10
    .else
    .if \d < 8
    amd64_r16_op \op, 0, \R\()15, \R\hd, \R\()10
    .else
    ldr \R\()13, [_cpu, CPU_amd64_regs + 8 * \d]
    amd64_r16_op \op, 0, \R\()15, \R\()13, \R\()10
    .endif
    amd64_r16_put 15, \d, \hd
    mov x11, \s
    amd64_cached_set_logic_flags x15, x11
    .endif
.endm
.macro amd64_slo16 op, s, R, bits, nf, d, hd, b, hb
.if \nf
    .if \b == 16
    .gadget amd64_slo_\op\()\s\()_\d\()_r_nf
    .elseif \b == 17
    .gadget amd64_slo_\op\()\s\()_\d\()_x_nf
    .else
    .gadget amd64_slo_\op\()\s\()_\d\()_\b\()_nf
    .endif
.else
    .if \b == 16
    .gadget amd64_slo_\op\()\s\()_\d\()_r
    .elseif \b == 17
    .gadget amd64_slo_\op\()\s\()_\d\()_x
    .else
    .gadget amd64_slo_\op\()\s\()_\d\()_\b
    .endif
.endif
    ldr x8, [_ip, 8]
    str x8, [_cpu, CPU_amd64_rip]
    amd64_m16_addr \b, \hb
    amd64_vread_prep (\s / 8), slo_\op\()\s\()_\d\()_\b\()_\nf
    .ifc \op, cmpm
    ldr \R\()13, [x9]
    .if \nf == 0
    amd64_r16_get \R, 10, \d, \hd
    subs \R\()15, \R\()13, \R\()10
    cset w14, cc
    cset w16, vs
    amd64_cached_set_addsub_flags \bits
    .endif
    .else
    .ifc \op, or
    amd64_slo_logic \op, \s, \R, \nf, \d, \hd
    .else
    .ifc \op, and
    amd64_slo_logic \op, \s, \R, \nf, \d, \hd
    .else
    .ifc \op, xor
    amd64_slo_logic \op, \s, \R, \nf, \d, \hd
    .else
    ldr \R\()10, [x9]
    .if \nf == 0
    amd64_r16_get \R, 13, \d, \hd
    .ifc \op, add
    adds \R\()15, \R\()13, \R\()10
    cset w14, cs
    .else
    subs \R\()15, \R\()13, \R\()10
    cset w14, cc
    .endif
    cset w16, vs
    amd64_cached_set_addsub_flags \bits
    .ifnc \op, cmp
    amd64_r16_put 15, \d, \hd
    .endif
    .else
    .ifnc \op, cmp
    amd64_r16_op_into \op, \R, \d, \hd, \R\()10
    .endif
    .endif
    .endif
    .endif
    .endif
    .endif
    gret 2
    amd64_svread_bullshit (\s / 8), slo_\op\()\s\()_\d\()_\b\()_\nf
.endm

# Sixteen registers x (sixteen bases + rip + indexed).
.macro amd64_r16_srcs17 m, op, s, R, bits, nf, d, hd
    amd64_r16_srcs \m, \op, \s, \R, \bits, \nf, \d, \hd
    \m \op, \s, \R, \bits, \nf, \d, \hd, 16, 0
    \m \op, \s, \R, \bits, \nf, \d, \hd, 17, 0
.endm
.macro amd64_r16_rr17 m, op, s, R, bits, nf
    amd64_r16_srcs17 \m, \op, \s, \R, \bits, \nf, 0, 20
    amd64_r16_srcs17 \m, \op, \s, \R, \bits, \nf, 1, 22
    amd64_r16_srcs17 \m, \op, \s, \R, \bits, \nf, 2, 23
    amd64_r16_srcs17 \m, \op, \s, \R, \bits, \nf, 3, 21
    amd64_r16_srcs17 \m, \op, \s, \R, \bits, \nf, 4, 27
    amd64_r16_srcs17 \m, \op, \s, \R, \bits, \nf, 5, 26
    amd64_r16_srcs17 \m, \op, \s, \R, \bits, \nf, 6, 24
    amd64_r16_srcs17 \m, \op, \s, \R, \bits, \nf, 7, 25
    amd64_r16_srcs17 \m, \op, \s, \R, \bits, \nf, 8, 0
    amd64_r16_srcs17 \m, \op, \s, \R, \bits, \nf, 9, 0
    amd64_r16_srcs17 \m, \op, \s, \R, \bits, \nf, 10, 0
    amd64_r16_srcs17 \m, \op, \s, \R, \bits, \nf, 11, 0
    amd64_r16_srcs17 \m, \op, \s, \R, \bits, \nf, 12, 0
    amd64_r16_srcs17 \m, \op, \s, \R, \bits, \nf, 13, 0
    amd64_r16_srcs17 \m, \op, \s, \R, \bits, \nf, 14, 0
    amd64_r16_srcs17 \m, \op, \s, \R, \bits, \nf, 15, 0
.endm
amd64_r16_rr17 amd64_smem16, ld, 32, w, 31, 0
amd64_r16_rr17 amd64_smem16, ld, 64, x, 63, 0
amd64_r16_rr17 amd64_smem16, st, 32, w, 31, 0
amd64_r16_rr17 amd64_smem16, st, 64, x, 63, 0
amd64_r16_family amd64_r16_rr17, amd64_slo16, add
amd64_r16_family amd64_r16_rr17, amd64_slo16, sub
amd64_r16_family amd64_r16_rr17, amd64_slo16, cmp
amd64_r16_family amd64_r16_rr17, amd64_slo16, cmpm
amd64_r16_family amd64_r16_rr17, amd64_slo16, or
amd64_r16_family amd64_r16_rr17, amd64_slo16, and
amd64_r16_family amd64_r16_rr17, amd64_slo16, xor

# 16-bit and byte moves on the register cache, any of the sixteen registers
# and the same 18 bases: amd64_smx_<kind>_<reg>_<base> (base r for rip, x
# for indexed). Kinds: st16 (mov m16, r16), zx16 (movzx r32/r64, m16),
# ld16 (mov r16, m16: the rest of the register kept), st8, zx8, ld8 -- the
# byte forms only for the low byte of a register (gen.c leaves ah..bh to
# the generic gadgets) -- and sx32 (movsxd r64, m32). Operands: [disp or address][insn rip]; gret 2.
.macro amd64_smx16 op, s, R, bits, nf, r, hr, b, hb
    .if \b == 16
    .gadget amd64_smx_\op\()_\r\()_r
    .elseif \b == 17
    .gadget amd64_smx_\op\()_\r\()_x
    .else
    .gadget amd64_smx_\op\()_\r\()_\b
    .endif
    ldr x8, [_ip, 8]
    str x8, [_cpu, CPU_amd64_rip]
    amd64_m16_addr \b, \hb
    .ifc \op, st16
    amd64_svwrite_prep 2, smx_\op\()_\r\()_\b
    .if \r < 8
    strh w\hr, [x9]
    .else
    ldr w10, [_cpu, CPU_amd64_regs + 8 * \r]
    strh w10, [x9]
    .endif
    amd64_svwrite_done 2, smx_\op\()_\r\()_\b
    gret 2
    amd64_svwrite_bullshit 2, smx_\op\()_\r\()_\b
    .exitm
    .endif
    .ifc \op, st8
    amd64_svwrite_prep 1, smx_\op\()_\r\()_\b
    .if \r < 8
    strb w\hr, [x9]
    .else
    ldr w10, [_cpu, CPU_amd64_regs + 8 * \r]
    strb w10, [x9]
    .endif
    amd64_svwrite_done 1, smx_\op\()_\r\()_\b
    gret 2
    amd64_svwrite_bullshit 1, smx_\op\()_\r\()_\b
    .exitm
    .endif
    .ifc \op, zx16
    amd64_vread_prep 2, smx_\op\()_\r\()_\b
    .if \r < 8
    ldrh w\hr, [x9]
    .else
    ldrh w10, [x9]
    str x10, [_cpu, CPU_amd64_regs + 8 * \r]
    .endif
    gret 2
    amd64_svread_bullshit 2, smx_\op\()_\r\()_\b
    .exitm
    .endif
    .ifc \op, zx8
    amd64_vread_prep 1, smx_\op\()_\r\()_\b
    .if \r < 8
    ldrb w\hr, [x9]
    .else
    ldrb w10, [x9]
    str x10, [_cpu, CPU_amd64_regs + 8 * \r]
    .endif
    gret 2
    amd64_svread_bullshit 1, smx_\op\()_\r\()_\b
    .exitm
    .endif
    .ifc \op, ld16
    amd64_vread_prep 2, smx_\op\()_\r\()_\b
    ldrh w10, [x9]
    .if \r < 8
    bfi x\hr, x10, 0, 16
    .else
    ldr x13, [_cpu, CPU_amd64_regs + 8 * \r]
    bfi x13, x10, 0, 16
    str x13, [_cpu, CPU_amd64_regs + 8 * \r]
    .endif
    gret 2
    amd64_svread_bullshit 2, smx_\op\()_\r\()_\b
    .exitm
    .endif
    .ifc \op, ld8
    amd64_vread_prep 1, smx_\op\()_\r\()_\b
    ldrb w10, [x9]
    .if \r < 8
    bfi x\hr, x10, 0, 8
    .else
    ldr x13, [_cpu, CPU_amd64_regs + 8 * \r]
    bfi x13, x10, 0, 8
    str x13, [_cpu, CPU_amd64_regs + 8 * \r]
    .endif
    gret 2
    amd64_svread_bullshit 1, smx_\op\()_\r\()_\b
    .exitm
    .endif
    .ifc \op, sx32
    amd64_vread_prep 4, smx_\op\()_\r\()_\b
    .if \r < 8
    ldrsw x\hr, [x9]
    .else
    ldrsw x10, [x9]
    str x10, [_cpu, CPU_amd64_regs + 8 * \r]
    .endif
    gret 2
    amd64_svread_bullshit 4, smx_\op\()_\r\()_\b
    .exitm
    .endif
    .error "amd64_smx16: unknown kind"
.endm
.irp kind, st16,zx16,ld16,st8,zx8,ld8,sx32
    amd64_r16_rr17 amd64_smx16, \kind, 0, w, 0, 0
.endr

# Byte compare and test with memory, flags only: amd64_scb_<op>_<reg>_<base>,
# op cmpmr (38: cmp m8, r8), cmprm (3A: cmp r8, m8), test (84). The flags
# are the 8-bit ones: the operands are compared shifted to the top of a
# 32-bit word, then the result is brought back down for ZF/SF/PF. Low byte
# of a register only, as for amd64_smx. Operands: [disp or address][insn
# rip]; gret 2.
.macro amd64_scb16 op, s, R, bits, nf, r, hr, b, hb
    .if \b == 16
    .gadget amd64_scb_\op\()_\r\()_r
    .elseif \b == 17
    .gadget amd64_scb_\op\()_\r\()_x
    .else
    .gadget amd64_scb_\op\()_\r\()_\b
    .endif
    ldr x8, [_ip, 8]
    str x8, [_cpu, CPU_amd64_rip]
    amd64_m16_addr \b, \hb
    amd64_vread_prep 1, scb_\op\()_\r\()_\b
    .ifc \op, cmprm
    ldrb w10, [x9]
    .if \r < 8
    and w13, w\hr, 0xff
    .else
    ldrb w13, [_cpu, CPU_amd64_regs + 8 * \r]
    .endif
    .else
    ldrb w13, [x9]
    .if \r < 8
    and w10, w\hr, 0xff
    .else
    ldrb w10, [_cpu, CPU_amd64_regs + 8 * \r]
    .endif
    .endif
    .ifc \op, test
    and w15, w13, w10
    mov x11, 8
    amd64_cached_set_logic_flags x15, x11
    .else
    lsl w11, w13, 24
    lsl w12, w10, 24
    subs w15, w11, w12
    cset w14, cc
    cset w16, vs
    lsr w15, w15, 24
    amd64_cached_set_addsub_flags 7
    .endif
    gret 2
    amd64_svread_bullshit 1, scb_\op\()_\r\()_\b
.endm
.irp op, cmpmr,cmprm,test
    amd64_r16_rr17 amd64_scb16, \op, 0, w, 0, 0
.endr

# push/pop r64 per register on the register cache (rsp is x27):
# amd64_spush_<reg>, amd64_spop_<reg>. A push commits rsp -= 8 only after
# its store, so a fault re-executes it unchanged; `push rsp` stores the old
# rsp. `pop rsp` loads rsp from the stack (the increment is lost, as on
# x86). Operand: [insn rip]; gret 1.
.macro amd64_spp16 op, s, R, bits, nf, r, hr
.ifc \op, push
.gadget amd64_spush_\r
    ldr x8, [_ip]
    str x8, [_cpu, CPU_amd64_rip]
    sub x9, x27, 8
    amd64_svwrite_prep 8, spush_\r
    .if \r < 8
    str x\hr, [x9]
    .else
    ldr x10, [_cpu, CPU_amd64_regs + 8 * \r]
    str x10, [x9]
    .endif
    amd64_svwrite_done 8, spush_\r
    sub x27, x27, 8
    gret 1
    amd64_svwrite_bullshit 8, spush_\r
.else
.gadget amd64_spop_\r
    ldr x8, [_ip]
    str x8, [_cpu, CPU_amd64_rip]
    mov x9, x27
    amd64_vread_prep 8, spop_\r
    .if \r == 4
    ldr x27, [x9]
    .else
    .if \r < 8
    ldr x\hr, [x9]
    .else
    ldr x10, [x9]
    str x10, [_cpu, CPU_amd64_regs + 8 * \r]
    .endif
    add x27, x27, 8
    .endif
    gret 1
    amd64_svread_bullshit 8, spop_\r
.endif
.endm
amd64_r16_ri amd64_spp16, push, 64, x, 63, 0
amd64_r16_ri amd64_spp16, pop, 64, x, 63, 0
# Enter the block for the guest address in x10 through jit_frame.ret_cache,
# which the frontend fills with the blocks it dispatches -- keyed, and
# validated against block->addr and is_jetsam, exactly as the arm64 guest's
# ret_cache_dispatch does -- by the same chain path a jmp takes (poke check,
# chain budget). On a miss, write the register cache back and leave to C with
# rip = x10. Ends the gadget; uses label 8.
.macro amd64_rc_dispatch
    eor x12, x10, x10, lsr #12
    ubfx x12, x12, #1, #12
    mov x13, LOCAL_ret_cache
    add x13, _cpu, x13
    ldr x13, [x13, x12, lsl #3]
    cbz x13, 8f
    ldur x14, [x13, #(JIT_BLOCK_addr - JIT_BLOCK_code)]
    cmp x14, x10
    b.ne 8f
    ldurb w14, [x13, #(JIT_BLOCK_is_jetsam - JIT_BLOCK_code)]
    cbnz w14, 8f
    mov _ip, x13
    amd64_chain_ip
8:  amd64_spill_low8
    str x10, [_cpu, CPU_amd64_rip]
    str w10, [_cpu, CPU_eip]
    b jit_ret
.endm

# ret on the register cache: pop the return address (rsp is x27) and, when
# the frontend has published a block for it in jit_frame.ret_cache, enter it
# directly -- keyed, and validated against block->addr and is_jetsam,
# exactly as the arm64 guest's ret_cache_dispatch does -- through the same
# chain path a jmp takes (poke check, chain budget). Otherwise write the
# cache back and leave to C with rip = the target. A non-canonical target
# is #GP with rsp unchanged. Operand: [insn rip]; ends the block.
.gadget amd64_sret
    ldr x8, [_ip]
    str x8, [_cpu, CPU_amd64_rip]
    mov x9, x27
    amd64_vread_prep 8, sret
    ldr x10, [x9]
    sbfx x12, x10, 0, 48
    cmp x12, x10
    b.ne 7f
    add x27, x27, 8
    amd64_rc_dispatch
7:  amd64_spill_low8
    b amd64_branch_noncanonical
    amd64_svread_bullshit 8, sret

# push imm (and a call's return address) on the register cache:
# [value][insn rip], gret 2, rsp committed after the store as above.
.gadget amd64_spushi
    ldr x8, [_ip, 8]
    str x8, [_cpu, CPU_amd64_rip]
    sub x9, x27, 8
    amd64_svwrite_prep 8, spushi
    ldr x10, [_ip]
    str x10, [x9]
    amd64_svwrite_done 8, spushi
    sub x27, x27, 8
    gret 2
    amd64_svwrite_bullshit 8, spushi

# inc/dec [mem] (FF /0, /1), 32/64-bit, read-modify-write on the register
# cache: amd64_sim_<op><size>_<base>, full and _nf, the same 18 bases. The
# guest address stays in x0 and the result goes to x10 across the two preps
# (save_c keeps both); the full form feeds the CF byte through the flag
# macro, as amd64_idr does, so CF is left as it was. A fault on the store
# re-executes the instruction from the unchanged memory. Operands: [disp or
# address][insn rip]; gret 2.
.macro amd64_sim16 op, s, R, bits, nf, d, hd, b, hb
.if \nf
    .if \b == 16
    .gadget amd64_sim_\op\()\s\()_r_nf
    .elseif \b == 17
    .gadget amd64_sim_\op\()\s\()_x_nf
    .else
    .gadget amd64_sim_\op\()\s\()_\b\()_nf
    .endif
.else
    .if \b == 16
    .gadget amd64_sim_\op\()\s\()_r
    .elseif \b == 17
    .gadget amd64_sim_\op\()\s\()_x
    .else
    .gadget amd64_sim_\op\()\s\()_\b
    .endif
.endif
    ldr x8, [_ip, 8]
    str x8, [_cpu, CPU_amd64_rip]
    amd64_m16_addr \b, \hb
    mov x0, x9
    amd64_vread_prep (\s / 8), sim_\op\()\s\()_\b\()_\nf\()r
    ldr \R\()13, [x9]
    .if \nf
    .ifc \op, inc
    add \R\()15, \R\()13, #1
    .else
    sub \R\()15, \R\()13, #1
    .endif
    .else
    .ifc \op, inc
    adds \R\()15, \R\()13, #1
    .else
    subs \R\()15, \R\()13, #1
    .endif
    cset w16, vs
    ldrb w14, [_cpu, CPU_cf]
    mov x10, 1
    amd64_cached_set_addsub_flags \bits
    .endif
    mov x10, x15
    mov x9, x0
    amd64_svwrite_prep (\s / 8), sim_\op\()\s\()_\b\()_\nf\()w
    str \R\()10, [x9]
    amd64_svwrite_done (\s / 8), sim_\op\()\s\()_\b\()_\nf\()w
    gret 2
    amd64_svread_bullshit (\s / 8), sim_\op\()\s\()_\b\()_\nf\()r
    amd64_svwrite_bullshit (\s / 8), sim_\op\()\s\()_\b\()_\nf\()w
.endm
.irp op, inc,dec
    amd64_r16_srcs17 amd64_sim16, \op, 32, w, 31, 0, 0, 0
    amd64_r16_srcs17 amd64_sim16, \op, 32, w, 31, 1, 0, 0
    amd64_r16_srcs17 amd64_sim16, \op, 64, x, 63, 0, 0, 0
    amd64_r16_srcs17 amd64_sim16, \op, 64, x, 63, 1, 0, 0
.endr

# Indirect jmp/call on the register cache, entering the target through the
# return cache like amd64_sret: amd64_sjmpr_<reg> / amd64_scallr_<reg> (target
# in a register, any of the sixteen) and amd64_sjmpm_<base> /
# amd64_scallm_<base> (target loaded from [base + disp], the 18 bases --
# PLT `jmp *x(%rip)`, vtables). x86 order: the target is read first, then a
# call pushes the return address (rsp committed after the store). A
# non-canonical target is #GP at the instruction, rsp unchanged. Operands:
# jmpr [insn rip]; callr [return address][insn rip]; jmpm [disp or
# address][insn rip]; callm [disp or address][insn rip][return address].
.macro amd64_sjr16 op, s, R, bits, nf, r, hr
.ifc \op, jmp
.gadget amd64_sjmpr_\r
    ldr x8, [_ip]
.else
.gadget amd64_scallr_\r
    ldr x8, [_ip, 8]
.endif
    str x8, [_cpu, CPU_amd64_rip]
    .if \r < 8
    mov x10, x\hr
    .else
    ldr x10, [_cpu, CPU_amd64_regs + 8 * \r]
    .endif
    sbfx x12, x10, 0, 48
    cmp x12, x10
    b.ne 7f
.ifc \op, call
    sub x9, x27, 8
    amd64_svwrite_prep 8, scallr_\r
    ldr x11, [_ip]
    str x11, [x9]
    amd64_svwrite_done 8, scallr_\r
    sub x27, x27, 8
.endif
    amd64_rc_dispatch
7:  amd64_spill_low8
    b amd64_branch_noncanonical
.ifc \op, call
    amd64_svwrite_bullshit 8, scallr_\r
.endif
.endm
amd64_r16_ri amd64_sjr16, jmp, 64, x, 63, 0
amd64_r16_ri amd64_sjr16, call, 64, x, 63, 0
.macro amd64_sjm16 op, s, R, bits, nf, d, hd, b, hb
.ifc \op, jmp
    .if \b == 16
    .gadget amd64_sjmpm_r
    .elseif \b == 17
    .gadget amd64_sjmpm_x
    .else
    .gadget amd64_sjmpm_\b
    .endif
.else
    .if \b == 16
    .gadget amd64_scallm_r
    .elseif \b == 17
    .gadget amd64_scallm_x
    .else
    .gadget amd64_scallm_\b
    .endif
.endif
    ldr x8, [_ip, 8]
    str x8, [_cpu, CPU_amd64_rip]
    amd64_m16_addr \b, \hb
    amd64_vread_prep 8, s\op\()m_\b\()_r
    ldr x10, [x9]
    sbfx x12, x10, 0, 48
    cmp x12, x10
    b.ne 7f
.ifc \op, call
    sub x9, x27, 8
    amd64_svwrite_prep 8, s\op\()m_\b\()_w
    ldr x11, [_ip, 16]
    str x11, [x9]
    amd64_svwrite_done 8, s\op\()m_\b\()_w
    sub x27, x27, 8
.endif
    amd64_rc_dispatch
7:  amd64_spill_low8
    b amd64_branch_noncanonical
    amd64_svread_bullshit 8, s\op\()m_\b\()_r
.ifc \op, call
    amd64_svwrite_bullshit 8, s\op\()m_\b\()_w
.endif
.endm
amd64_r16_srcs17 amd64_sjm16, jmp, 64, x, 63, 0, 0, 0
amd64_r16_srcs17 amd64_sjm16, call, 64, x, 63, 0, 0, 0

# cmp [mem], imm32 (81/83 /7), 32/64-bit, on the register cache:
# amd64_scmi<size>_<base>, the 18 bases, flags only. Operands: [disp or
# address][insn rip][imm]; gret 3.
.macro amd64_scmi16 op, s, R, bits, nf, d, hd, b, hb
    .if \b == 16
    .gadget amd64_scmi\s\()_r
    .elseif \b == 17
    .gadget amd64_scmi\s\()_x
    .else
    .gadget amd64_scmi\s\()_\b
    .endif
    ldr x8, [_ip, 8]
    str x8, [_cpu, CPU_amd64_rip]
    amd64_m16_addr \b, \hb
    amd64_vread_prep (\s / 8), scmi\s\()_\b
    ldr \R\()13, [x9]
    ldr x10, [_ip, 16]
    .if \s == 32
    mov w10, w10
    .endif
    subs \R\()15, \R\()13, \R\()10
    cset w14, cc
    cset w16, vs
    amd64_cached_set_addsub_flags \bits
    gret 3
    amd64_svread_bullshit (\s / 8), scmi\s\()_\b
.endm
amd64_r16_srcs17 amd64_scmi16, cmp, 32, w, 31, 0, 0, 0
amd64_r16_srcs17 amd64_scmi16, cmp, 64, x, 63, 0, 0, 0

# cmp m8, imm8 (80 /7) and test m8, imm8 (F6 /0), flags only, the 18 bases:
# amd64_sbi_<op>_<base>. The 8-bit flags as amd64_scb computes them.
# Operands: [disp or address][insn rip][imm]; gret 3. Table
# amd64_sbi_gadgets[op * 18 + base], op 0 cmp, 1 test.
.macro amd64_sbi16 op, s, R, bits, nf, d, hd, b, hb
    .if \b == 16
    .gadget amd64_sbi_\op\()_r
    .elseif \b == 17
    .gadget amd64_sbi_\op\()_x
    .else
    .gadget amd64_sbi_\op\()_\b
    .endif
    ldr x8, [_ip, 8]
    str x8, [_cpu, CPU_amd64_rip]
    amd64_m16_addr \b, \hb
    amd64_vread_prep 1, sbi_\op\()_\b
    ldrb w13, [x9]
    ldr x10, [_ip, 16]
    .ifc \op, test
    and w15, w13, w10
    and w15, w15, 0xff
    mov x11, 8
    amd64_cached_set_logic_flags x15, x11
    .else
    lsl w11, w13, 24
    lsl w12, w10, 24
    subs w15, w11, w12
    cset w14, cc
    cset w16, vs
    lsr w15, w15, 24
    amd64_cached_set_addsub_flags 7
    .endif
    gret 3
    amd64_svread_bullshit 1, sbi_\op\()_\b
.endm
amd64_r16_srcs17 amd64_sbi16, cmp, 8, w, 7, 0, 0, 0
amd64_r16_srcs17 amd64_sbi16, test, 8, w, 7, 0, 0, 0

# mov [mem], imm (C6/C7 /0), 8/16/32/64-bit, on the register cache:
# amd64_ssi<size>_<base>, the 18 bases. Operands: [disp or address][insn
# rip][imm]; gret 3.
.macro amd64_ssi16 op, s, R, bits, nf, d, hd, b, hb
    .if \b == 16
    .gadget amd64_ssi\s\()_r
    .elseif \b == 17
    .gadget amd64_ssi\s\()_x
    .else
    .gadget amd64_ssi\s\()_\b
    .endif
    ldr x8, [_ip, 8]
    str x8, [_cpu, CPU_amd64_rip]
    amd64_m16_addr \b, \hb
    amd64_svwrite_prep (\s / 8), ssi\s\()_\b
    ldr x10, [_ip, 16]
    .if \s == 8
    strb w10, [x9]
    .elseif \s == 16
    strh w10, [x9]
    .elseif \s == 32
    str w10, [x9]
    .else
    str x10, [x9]
    .endif
    amd64_svwrite_done (\s / 8), ssi\s\()_\b
    gret 3
    amd64_svwrite_bullshit (\s / 8), ssi\s\()_\b
.endm
.irp sz, 8,16,32,64
    amd64_r16_srcs17 amd64_ssi16, mov, \sz, x, 0, 0, 0, 0
.endr

# add/sub/or/and/xor [mem], imm (81/83 /0,/5,/1,/4,/6), 32/64-bit, read-modify-
# write on the register cache, as amd64_sim does (guest address in x0, result
# in x10 across the two preps): amd64_sri_<op><size>_<base>, full and _nf.
# Operands: [disp or address][insn rip][imm]; gret 3.
.macro amd64_sri16 op, s, R, bits, nf, d, hd, b, hb
.if \nf
    .if \b == 16
    .gadget amd64_sri_\op\()\s\()_r_nf
    .elseif \b == 17
    .gadget amd64_sri_\op\()\s\()_x_nf
    .else
    .gadget amd64_sri_\op\()\s\()_\b\()_nf
    .endif
.else
    .if \b == 16
    .gadget amd64_sri_\op\()\s\()_r
    .elseif \b == 17
    .gadget amd64_sri_\op\()\s\()_x
    .else
    .gadget amd64_sri_\op\()\s\()_\b
    .endif
.endif
    ldr x8, [_ip, 8]
    str x8, [_cpu, CPU_amd64_rip]
    amd64_m16_addr \b, \hb
    mov x0, x9
    amd64_vread_prep (\s / 8), sri_\op\()\s\()_\b\()_\nf\()r
    ldr \R\()13, [x9]
    ldr x10, [_ip, 16]
    .if \s == 32
    mov w10, w10
    .endif
    .if \nf
    amd64_r16_op \op, 0, \R\()15, \R\()13, \R\()10
    .else
    .ifc \op, add
    adds \R\()15, \R\()13, \R\()10
    cset w14, cs
    cset w16, vs
    amd64_cached_set_addsub_flags \bits
    .else
    .ifc \op, sub
    subs \R\()15, \R\()13, \R\()10
    cset w14, cc
    cset w16, vs
    amd64_cached_set_addsub_flags \bits
    .else
    amd64_r16_op \op, 0, \R\()15, \R\()13, \R\()10
    mov x11, \s
    amd64_cached_set_logic_flags x15, x11
    .endif
    .endif
    .endif
    mov x10, x15
    mov x9, x0
    amd64_svwrite_prep (\s / 8), sri_\op\()\s\()_\b\()_\nf\()w
    str \R\()10, [x9]
    amd64_svwrite_done (\s / 8), sri_\op\()\s\()_\b\()_\nf\()w
    gret 3
    amd64_svread_bullshit (\s / 8), sri_\op\()\s\()_\b\()_\nf\()r
    amd64_svwrite_bullshit (\s / 8), sri_\op\()\s\()_\b\()_\nf\()w
.endm
.irp op, add,sub,or,and,xor
    amd64_r16_srcs17 amd64_sri16, \op, 32, w, 31, 0, 0, 0
    amd64_r16_srcs17 amd64_sri16, \op, 32, w, 31, 1, 0, 0
    amd64_r16_srcs17 amd64_sri16, \op, 64, x, 63, 0, 0, 0
    amd64_r16_srcs17 amd64_sri16, \op, 64, x, 63, 1, 0, 0
.endr
.global NAME(amd64_specmem_end)
NAME(amd64_specmem_end):
# Tables: amd64_smem_gadgets[((st * 2 + size64) * 16 + reg) * 18 + base]
# (st 0 load, 1 store), amd64_slo{,_nf}_gadgets[((op * 2 + size64) * 16 +
# dst) * 18 + base], amd64_smx_gadgets[(kind * 16 + reg) * 18 + base] (kinds
# st16 zx16 ld16 st8 zx8 ld8 sx32) and amd64_scb_gadgets[(op * 16 + reg) * 18 +
# base] (cmpmr cmprm test); base 16 = rip-relative, 17 = indexed.
.macro amd64_slo_quad op, s, d, b, nf
    .if \b == 16
        .if \nf
        .quad NAME(gadget_amd64_slo_\op\()\s\()_\d\()_r_nf)
        .else
        .quad NAME(gadget_amd64_slo_\op\()\s\()_\d\()_r)
        .endif
    .elseif \b == 17
        .if \nf
        .quad NAME(gadget_amd64_slo_\op\()\s\()_\d\()_x_nf)
        .else
        .quad NAME(gadget_amd64_slo_\op\()\s\()_\d\()_x)
        .endif
    .else
        .if \nf
        .quad NAME(gadget_amd64_slo_\op\()\s\()_\d\()_\b\()_nf)
        .else
        .quad NAME(gadget_amd64_slo_\op\()\s\()_\d\()_\b)
        .endif
    .endif
.endm
.macro amd64_smem_row st, s, d
    .irp b, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17
        .ifc \st, ld
        amd64_smem_quad sld, \s, \d, \b
        .else
        amd64_smem_quad sst, \s, \d, \b
        .endif
    .endr
.endm
.macro amd64_smem_quad pre, s, d, b
    .if \b == 16
    .quad NAME(gadget_amd64_\pre\()r\()\s\()_\d)
    .elseif \b == 17
    .quad NAME(gadget_amd64_\pre\()\s\()_\d\()_x)
    .else
    .quad NAME(gadget_amd64_\pre\()\s\()_\d\()_\b)
    .endif
.endm
.macro amd64_slo_row op, s, d, nf
    .irp b, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17
        amd64_slo_quad \op, \s, \d, \b, \nf
    .endr
.endm
.macro amd64_smem_ops st
    .irp s, 32,64
        .irp d, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
            amd64_smem_row \st, \s, \d
        .endr
    .endr
.endm
.macro amd64_slo_ops op, nf
    .irp s, 32,64
        .irp d, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
            amd64_slo_row \op, \s, \d, \nf
        .endr
    .endr
.endm
.macro amd64_x16_quad fam, op, d, b
    .if \b == 16
    .quad NAME(gadget_amd64_\fam\()_\op\()_\d\()_r)
    .elseif \b == 17
    .quad NAME(gadget_amd64_\fam\()_\op\()_\d\()_x)
    .else
    .quad NAME(gadget_amd64_\fam\()_\op\()_\d\()_\b)
    .endif
.endm
.macro amd64_x16_row fam, op, d
    .irp b, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17
        amd64_x16_quad \fam, \op, \d, \b
    .endr
.endm
.macro amd64_x16_ops fam, op
    .irp d, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
        amd64_x16_row \fam, \op, \d
    .endr
.endm
.macro amd64_sim_quad op, s, b, nf
    .if \b == 16
        .if \nf
        .quad NAME(gadget_amd64_sim_\op\()\s\()_r_nf)
        .else
        .quad NAME(gadget_amd64_sim_\op\()\s\()_r)
        .endif
    .elseif \b == 17
        .if \nf
        .quad NAME(gadget_amd64_sim_\op\()\s\()_x_nf)
        .else
        .quad NAME(gadget_amd64_sim_\op\()\s\()_x)
        .endif
    .else
        .if \nf
        .quad NAME(gadget_amd64_sim_\op\()\s\()_\b\()_nf)
        .else
        .quad NAME(gadget_amd64_sim_\op\()\s\()_\b)
        .endif
    .endif
.endm
.macro amd64_sim_ops op, nf
    .irp s, 32,64
        .irp b, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17
            amd64_sim_quad \op, \s, \b, \nf
        .endr
    .endr
.endm
.macro amd64_spp_quad op, r
    .quad NAME(gadget_amd64_s\op\()_\r)
.endm
.macro amd64_sjr_quad op, r
    .quad NAME(gadget_amd64_s\op\()r_\r)
.endm
.macro amd64_sjm_quad op, b
    .if \b == 16
    .quad NAME(gadget_amd64_s\op\()m_r)
    .elseif \b == 17
    .quad NAME(gadget_amd64_s\op\()m_x)
    .else
    .quad NAME(gadget_amd64_s\op\()m_\b)
    .endif
.endm
.pushsection_rodata
.global.name amd64_sjmpr_gadgets
    .irp r, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
        amd64_sjr_quad jmp, \r
    .endr
.global.name amd64_scallr_gadgets
    .irp r, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
        amd64_sjr_quad call, \r
    .endr
.global.name amd64_sjmpm_gadgets
    .irp b, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17
        amd64_sjm_quad jmp, \b
    .endr
.global.name amd64_scallm_gadgets
    .irp b, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17
        amd64_sjm_quad call, \b
    .endr
.global.name amd64_spush_gadgets
    .irp r, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
        amd64_spp_quad push, \r
    .endr
.global.name amd64_spop_gadgets
    .irp r, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
        amd64_spp_quad pop, \r
    .endr
.global.name amd64_sim_gadgets
    amd64_sim_ops inc, 0
    amd64_sim_ops dec, 0
.global.name amd64_sim_nf_gadgets
    amd64_sim_ops inc, 1
    amd64_sim_ops dec, 1
.global.name amd64_smx_gadgets
    .irp kind, st16,zx16,ld16,st8,zx8,ld8,sx32
        amd64_x16_ops smx, \kind
    .endr
.global.name amd64_scb_gadgets
    .irp op, cmpmr,cmprm,test
        amd64_x16_ops scb, \op
    .endr
.global.name amd64_smem_gadgets
    amd64_smem_ops ld
    amd64_smem_ops st
.global.name amd64_slo_gadgets
    amd64_slo_ops add, 0
    amd64_slo_ops sub, 0
    amd64_slo_ops cmp, 0
    amd64_slo_ops cmpm, 0
    amd64_slo_ops or, 0
    amd64_slo_ops and, 0
    amd64_slo_ops xor, 0
.global.name amd64_slo_nf_gadgets
    amd64_slo_ops add, 1
    amd64_slo_ops sub, 1
    amd64_slo_ops cmp, 1
    amd64_slo_ops cmpm, 1
    amd64_slo_ops or, 1
    amd64_slo_ops and, 1
    amd64_slo_ops xor, 1
.popsection

# inc/dec r32/r64 per register, all sixteen: amd64_idr_<op><size>_<reg>,
# full and _nf. INC/DEC leave CF alone: the full form feeds the current CF
# byte through the flag macro as its "carry", which rewrites the byte and
# eflags bit 0 with the values they already had. No operands, gret 0.
# Tables amd64_idr{,_nf}_gadgets[(op * 2 + size64) * 16 + reg].
.macro amd64_idr16 op, s, R, bits, nf, d, hd
.if \nf
.gadget amd64_idr_\op\()\s\()_\d\()_nf
    .ifc \op, inc
    amd64_r16_op_into add, \R, \d, \hd, #1
    .else
    amd64_r16_op_into sub, \R, \d, \hd, #1
    .endif
    gret
.else
.gadget amd64_idr_\op\()\s\()_\d
    amd64_r16_get \R, 13, \d, \hd
    .ifc \op, inc
    adds \R\()15, \R\()13, #1
    .else
    subs \R\()15, \R\()13, #1
    .endif
    cset w16, vs
    ldrb w14, [_cpu, CPU_cf]
    mov x10, 1
    amd64_cached_set_addsub_flags \bits
    amd64_r16_put 15, \d, \hd
    gret
.endif
.endm
amd64_r16_family amd64_r16_ri, amd64_idr16, inc
amd64_r16_family amd64_r16_ri, amd64_idr16, dec
.pushsection_rodata
.global.name amd64_idr_gadgets
    amd64_r16_ri_ops idr, inc, 0
    amd64_r16_ri_ops idr, dec, 0
.global.name amd64_idr_nf_gadgets
    amd64_r16_ri_ops idr, inc, 1
    amd64_r16_ri_ops idr, dec, 1
.popsection

# shl/shr/sar r32/r64, imm8 (and the D1 by-one forms) per register, all
# sixteen like the arith families above: amd64_shi_<op><size>_<reg>, full
# and _nf, operand [count] with the count already masked and nonzero (gen.c
# leaves a zero count, which changes no flag, to the generic gadgets). CF/OF
# as amd64_cached_shift_reg_count computes them: OF only for a count of 1
# (else 0), and 0 for SAR. gret 1. Tables
# amd64_shi{,_nf}_gadgets[(op * 2 + size64) * 16 + reg].
.macro amd64_shi16 op, s, R, bits, nf, d, hd
.if \nf
.gadget amd64_shi_\op\()\s\()_\d\()_nf
    ldr x10, [_ip]
    amd64_r16_op_into \op, \R, \d, \hd, \R\()10
    gret 1
.else
.gadget amd64_shi_\op\()\s\()_\d
    ldr x10, [_ip]
    amd64_r16_get \R, 13, \d, \hd
    amd64_r16_op \op, 0, \R\()15, \R\()13, \R\()10
    .ifc \op, shl
    mov w12, \s
    sub w12, w12, w10
    lsr \R\()14, \R\()13, \R\()12
    and w14, w14, 1
    lsr \R\()16, \R\()15, \bits
    eor w16, w16, w14
    .else
    .ifc \op, shr
    lsr \R\()16, \R\()13, \bits
    .else
    mov w16, 0
    .endif
    sub w12, w10, 1
    lsr \R\()14, \R\()13, \R\()12
    and w14, w14, 1
    .endif
    cmp x10, 1
    csel w16, w16, wzr, eq
    amd64_r16_put 15, \d, \hd
    amd64_cached_set_shift_flags \bits
    gret 1
.endif
.endm
amd64_r16_family amd64_r16_ri, amd64_shi16, shl
amd64_r16_family amd64_r16_ri, amd64_shi16, shr
amd64_r16_family amd64_r16_ri, amd64_shi16, sar
.pushsection_rodata
.global.name amd64_shi_gadgets
    amd64_r16_ri_ops shi, shl, 0
    amd64_r16_ri_ops shi, shr, 0
    amd64_r16_ri_ops shi, sar, 0
.global.name amd64_shi_nf_gadgets
    amd64_r16_ri_ops shi, shl, 1
    amd64_r16_ri_ops shi, shr, 1
    amd64_r16_ri_ops shi, sar, 1
.popsection

# ---- indexed addresses, and lea, on the register cache ----
#
# amd64_ea_<base>_<index>_<scale>: x3 = base + (index << scale), base 16 =
# none, for the memory gadget emitted right after it (base 17 of the memory
# families adds the disp). No memory access, so outside the specmem range;
# nothing runs between the two gadgets, so x3 survives. Any of the sixteen
# registers for both (index 4, rsp, cannot be encoded; its slots exist only
# to keep the table rectangular). gret 0. Table amd64_ea_gadgets[(base * 16 +
# index) * 4 + scale].
.macro amd64_ea1 b, hb, i, hi, sc
.gadget amd64_ea_\b\()_\i\()_\sc
    .if \i < 8
    .if \b < 8
    add x3, x\hb, x\hi, lsl \sc
    .elseif \b < 16
    ldr x3, [_cpu, CPU_amd64_regs + 8 * \b]
    add x3, x3, x\hi, lsl \sc
    .else
    lsl x3, x\hi, \sc
    .endif
    .else
    ldr x8, [_cpu, CPU_amd64_regs + 8 * \i]
    .if \b < 8
    add x3, x\hb, x8, lsl \sc
    .elseif \b < 16
    ldr x3, [_cpu, CPU_amd64_regs + 8 * \b]
    add x3, x3, x8, lsl \sc
    .else
    lsl x3, x8, \sc
    .endif
    .endif
    gret
.endm
.macro amd64_ea_scales op, s, R, bits, nf, b, hb, i, hi
    amd64_ea1 \b, \hb, \i, \hi, 0
    amd64_ea1 \b, \hb, \i, \hi, 1
    amd64_ea1 \b, \hb, \i, \hi, 2
    amd64_ea1 \b, \hb, \i, \hi, 3
.endm
amd64_r16_rr amd64_ea_scales, 0, 0, x, 0, 0
amd64_r16_srcs amd64_ea_scales, 0, 0, x, 0, 0, 16, 0
# x3 = the %fs / %gs base, for an operand that is just %fs:disp or %gs:disp
# (the stack-protector canary at %fs:0x28, __thread variables): the memory
# gadget after it adds the disp as for an indexed operand (base 17).
.gadget amd64_ea_fs
    ldr x3, [_cpu, CPU_tls_ptr]
    gret
.gadget amd64_ea_gs
    ldr x3, [_cpu, CPU_amd64_gs_base]
    gret

# lea r32/r64 per register: amd64_lea<size>_<dst>_<base>: dst = base + disp;
# amd64_movi<size>_<dst>: dst = the operand (a rip-relative lea, worked out
# at translation); amd64_leax<size>_<dst>: dst = x3 + disp (an indexed lea,
# after its amd64_ea gadget). 32-bit forms zero-extend, as an x86 32-bit
# write does. gret 1. Table amd64_lea_gadgets[(size64 * 16 + dst) * 18 +
# base], base 16 = movi, 17 = leax.
.macro amd64_lea16 op, s, R, bits, nf, d, hd, b, hb
    .if \b == 16
    .gadget amd64_movi\s\()_\d
    .if \d < 8
    ldr \R\hd, [_ip]
    .else
    ldr \R\()8, [_ip]
    str x8, [_cpu, CPU_amd64_regs + 8 * \d]
    .endif
    gret 1
    .exitm
    .endif
    .if \b == 17
    .gadget amd64_leax\s\()_\d
    .else
    .gadget amd64_lea\s\()_\d\()_\b
    .endif
    ldr x8, [_ip]
    .if \b < 8
    .if \d < 8
    add \R\hd, \R\hb, \R\()8
    .else
    add \R\()13, \R\hb, \R\()8
    str x13, [_cpu, CPU_amd64_regs + 8 * \d]
    .endif
    .else
    .if \b < 16
    ldr \R\()10, [_cpu, CPU_amd64_regs + 8 * \b]
    .else
    mov x10, x3
    .endif
    .if \d < 8
    add \R\hd, \R\()10, \R\()8
    .else
    add \R\()13, \R\()10, \R\()8
    str x13, [_cpu, CPU_amd64_regs + 8 * \d]
    .endif
    .endif
    gret 1
.endm
amd64_r16_rr17 amd64_lea16, 0, 32, w, 0, 0
amd64_r16_rr17 amd64_lea16, 0, 64, x, 0, 0
.macro amd64_ea_quad b, i, sc
    .quad NAME(gadget_amd64_ea_\b\()_\i\()_\sc)
.endm
.macro amd64_ea_row b
    .irp i, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
        amd64_ea_quad \b, \i, 0
        amd64_ea_quad \b, \i, 1
        amd64_ea_quad \b, \i, 2
        amd64_ea_quad \b, \i, 3
    .endr
.endm
.macro amd64_lea_quad s, d, b
    .if \b == 16
    .quad NAME(gadget_amd64_movi\s\()_\d)
    .elseif \b == 17
    .quad NAME(gadget_amd64_leax\s\()_\d)
    .else
    .quad NAME(gadget_amd64_lea\s\()_\d\()_\b)
    .endif
.endm
.pushsection_rodata
.global.name amd64_ea_gadgets
    .irp b, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16
        amd64_ea_row \b
    .endr
.global.name amd64_lea_gadgets
    .irp s, 32,64
        .irp d, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
            .irp b, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17
                amd64_lea_quad \s, \d, \b
            .endr
        .endr
    .endr
.popsection

# cmp r32/r64, r32/r64 and cmp r32/r64, imm per register: amd64_crr<size>_<lhs>_<rhs>
# and amd64_cri<size>_<reg> (imm in the stream word). gen.c uses them for
# pairs that involve r8-r15; a pair of rax..rdi keeps the generic gadget,
# which the cmp+jcc fusion rewrites. Flags only, so no _nf twin. Tables
# amd64_crr_gadgets[(size64 * 16 + lhs) * 16 + rhs], amd64_cri_gadgets[size64
# * 16 + reg].
.macro amd64_crr16 op, s, R, bits, nf, d, hd, sr, hs
.gadget amd64_crr\s\()_\d\()_\sr
    amd64_r16_get \R, 13, \d, \hd
    amd64_r16_get \R, 10, \sr, \hs
    subs \R\()15, \R\()13, \R\()10
    cset w14, cc
    cset w16, vs
    amd64_cached_set_addsub_flags \bits
    gret
.endm
.macro amd64_cri16 op, s, R, bits, nf, d, hd
.gadget amd64_cri\s\()_\d
    amd64_r16_get \R, 13, \d, \hd
    ldr x10, [_ip]
    .if \s == 32
    mov w10, w10
    .endif
    subs \R\()15, \R\()13, \R\()10
    cset w14, cc
    cset w16, vs
    amd64_cached_set_addsub_flags \bits
    gret 1
.endm
amd64_r16_rr amd64_crr16, cmp, 32, w, 31, 0
amd64_r16_rr amd64_crr16, cmp, 64, x, 63, 0
amd64_r16_ri amd64_cri16, cmp, 32, w, 31, 0
amd64_r16_ri amd64_cri16, cmp, 64, x, 63, 0
.macro amd64_crr_quad s, d, sr
    .quad NAME(gadget_amd64_crr\s\()_\d\()_\sr)
.endm
.macro amd64_cri_quad s, d
    .quad NAME(gadget_amd64_cri\s\()_\d)
.endm
.pushsection_rodata
.global.name amd64_crr_gadgets
    .irp s, 32,64
        .irp d, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
            .irp sr, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
                amd64_crr_quad \s, \d, \sr
            .endr
        .endr
    .endr
.global.name amd64_cri_gadgets
    .irp s, 32,64
        .irp d, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
            amd64_cri_quad \s, \d
        .endr
    .endr
.popsection

# imul r32/r64, r32/r64 (0F AF) and imul r, r/m, imm (69/6B), register forms,
# per register, any of the sixteen: amd64_imr<size>_<dst>_<src> (dst *= src)
# and amd64_imi<size>_<dst>_<src> (dst = src * imm, imm in the stream word),
# full and _nf. Only CF and OF are written (both = the product did not fit),
# the rest of the flags left as they were, like amd64_imul_body. Tables
# amd64_im{r,i}{,_nf}_gadgets[(size64 * 16 + dst) * 16 + src].
.macro amd64_im16 op, s, R, bits, nf, d, hd, sr, hs
.ifc \op, rr
    .if \nf
    .gadget amd64_imr\s\()_\d\()_\sr\()_nf
    .else
    .gadget amd64_imr\s\()_\d\()_\sr
    .endif
    amd64_r16_get \R, 13, \d, \hd
    amd64_r16_get \R, 10, \sr, \hs
.else
    .if \nf
    .gadget amd64_imi\s\()_\d\()_\sr\()_nf
    .else
    .gadget amd64_imi\s\()_\d\()_\sr
    .endif
    amd64_r16_get \R, 13, \sr, \hs
    ldr x10, [_ip]
.endif
    .if \nf
    mul \R\()15, \R\()13, \R\()10
    .else
    .if \s == 32
    smull x14, w13, w10
    sxtw x15, w14
    cmp x15, x14
    cset w16, ne
    mov w15, w14
    .else
    mul x15, x13, x10
    smulh x14, x13, x10
    asr x17, x15, 63
    cmp x14, x17
    cset w16, ne
    .endif
    .endif
    amd64_r16_put 15, \d, \hd
    .if \nf == 0
    strb w16, [_cpu, CPU_cf]
    strb w16, [_cpu, CPU_of]
    ldr w17, [_cpu, CPU_eflags]
    mov w14, (1 | (1 << 11))
    bic w17, w17, w14
    orr w17, w17, w16
    orr w17, w17, w16, lsl 11
    str w17, [_cpu, CPU_eflags]
    .endif
.ifc \op, rr
    gret
.else
    gret 1
.endif
.endm
amd64_r16_family amd64_r16_rr, amd64_im16, rr
amd64_r16_family amd64_r16_rr, amd64_im16, ri
.macro amd64_im_quad op, s, d, sr, nf
    .if \nf
    .quad NAME(gadget_amd64_im\op\()\s\()_\d\()_\sr\()_nf)
    .else
    .quad NAME(gadget_amd64_im\op\()\s\()_\d\()_\sr)
    .endif
.endm
.macro amd64_im_table op, nf
    .irp s, 32,64
        .irp d, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
            .irp sr, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
                amd64_im_quad \op, \s, \d, \sr, \nf
            .endr
        .endr
    .endr
.endm
.pushsection_rodata
.global.name amd64_imr_gadgets
    amd64_im_table r, 0
.global.name amd64_imr_nf_gadgets
    amd64_im_table r, 1
.global.name amd64_imi_gadgets
    amd64_im_table i, 0
.global.name amd64_imi_nf_gadgets
    amd64_im_table i, 1
.popsection

# cmp + jcc fused per register pair, rax..rdi (the cache; r8-r15 compares
# take amd64_crr/cri, unfused): amd64_fcr_<cond><size>_<lhs>_<rhs> and
# amd64_fci_<cond><size>_<reg> (imm in the stream word). The flags deposit
# and the branch are amd64_fused_cmp_rr/ri's, without their run-time
# register selection. Stream: [gadget]([imm])[taken][else]. Tables
# amd64_fc{r,i}_gadgets[(cond * 2 + size64) * 64 + lhs * 8 + rhs] and
# [(cond * 2 + size64) * 8 + reg], conditions in COND_LIST order with the
# parity slots empty (parity is never fused).
.macro amd64_fcr8 cond, s, R, bits, d, hd, sr, hs
.gadget amd64_fcr_\cond\()\s\()_\d\()_\sr
    mov \R\()13, \R\hd
    mov \R\()10, \R\hs
    subs \R\()15, \R\()13, \R\()10
    cset w14, cc
    cset w16, vs
    amd64_cached_set_addsub_flags \bits
    amd64_fused_cc_tail \cond, \bits, 0
.endm
.macro amd64_fci8 cond, s, R, bits, d, hd
.gadget amd64_fci_\cond\()\s\()_\d
    mov \R\()13, \R\hd
    ldr x10, [_ip]
    .if \s == 32
    mov w10, w10
    .endif
    subs \R\()15, \R\()13, \R\()10
    cset w14, cc
    cset w16, vs
    amd64_cached_set_addsub_flags \bits
    amd64_fused_cc_tail \cond, \bits, 8
.endm
.macro amd64_fc8_srcs cond, s, R, bits, d, hd
    amd64_fcr8 \cond, \s, \R, \bits, \d, \hd, 0, 20
    amd64_fcr8 \cond, \s, \R, \bits, \d, \hd, 1, 22
    amd64_fcr8 \cond, \s, \R, \bits, \d, \hd, 2, 23
    amd64_fcr8 \cond, \s, \R, \bits, \d, \hd, 3, 21
    amd64_fcr8 \cond, \s, \R, \bits, \d, \hd, 4, 27
    amd64_fcr8 \cond, \s, \R, \bits, \d, \hd, 5, 26
    amd64_fcr8 \cond, \s, \R, \bits, \d, \hd, 6, 24
    amd64_fcr8 \cond, \s, \R, \bits, \d, \hd, 7, 25
    amd64_fci8 \cond, \s, \R, \bits, \d, \hd
.endm
.macro amd64_fc8_dsts cond, s, R, bits
    amd64_fc8_srcs \cond, \s, \R, \bits, 0, 20
    amd64_fc8_srcs \cond, \s, \R, \bits, 1, 22
    amd64_fc8_srcs \cond, \s, \R, \bits, 2, 23
    amd64_fc8_srcs \cond, \s, \R, \bits, 3, 21
    amd64_fc8_srcs \cond, \s, \R, \bits, 4, 27
    amd64_fc8_srcs \cond, \s, \R, \bits, 5, 26
    amd64_fc8_srcs \cond, \s, \R, \bits, 6, 24
    amd64_fc8_srcs \cond, \s, \R, \bits, 7, 25
.endm
.irp cond, o,c,z,cz,s,sxo,sxoz
    amd64_fc8_dsts \cond, 32, w, 31
    amd64_fc8_dsts \cond, 64, x, 63
.endr
.macro amd64_fcr_quad cond, s, d, sr
    .ifc \cond, p
    .quad 0
    .else
    .quad NAME(gadget_amd64_fcr_\cond\()\s\()_\d\()_\sr)
    .endif
.endm
.macro amd64_fci_quad cond, s, d
    .ifc \cond, p
    .quad 0
    .else
    .quad NAME(gadget_amd64_fci_\cond\()\s\()_\d)
    .endif
.endm
.macro amd64_fcr_rows cond, s
    .irp d, 0,1,2,3,4,5,6,7
        .irp sr, 0,1,2,3,4,5,6,7
            amd64_fcr_quad \cond, \s, \d, \sr
        .endr
    .endr
.endm
.macro amd64_fci_rows cond, s
    .irp d, 0,1,2,3,4,5,6,7
        amd64_fci_quad \cond, \s, \d
    .endr
.endm
.pushsection_rodata
.global.name amd64_fcr_gadgets
    .irp cond, o,c,z,cz,s,p,sxo,sxoz
        amd64_fcr_rows \cond, 32
        amd64_fcr_rows \cond, 64
    .endr
.global.name amd64_fci_gadgets
    .irp cond, o,c,z,cz,s,p,sxo,sxoz
        amd64_fci_rows \cond, 32
        amd64_fci_rows \cond, 64
    .endr
.popsection

# test + jcc, fused, for rax..rdi: amd64_ftr_<cond><size>_<a>_<b> (test a, b)
# and amd64_fti_<cond><size>_<reg> (test reg, imm; the imm in the first
# stream word). The flags of the test, then the branch on its result as the
# fused cmp gadgets do (CF = OF = 0). Tables amd64_ftr_gadgets[(cond * 2 +
# size64) * 64 + a * 8 + b] and amd64_fti_gadgets[(cond * 2 + size64) * 8 +
# reg], the same layout as amd64_fcr/fci, parity empty.
.macro amd64_ftr8 cond, s, R, bits, d, hd, sr, hs
.gadget amd64_ftr_\cond\()\s\()_\d\()_\sr
    and \R\()15, \R\hd, \R\hs
    mov x11, \s
    amd64_cached_set_logic_flags x15, x11
    mov w14, 0
    mov w16, 0
    amd64_fused_cc_tail \cond, \bits, 0
.endm
.macro amd64_fti8 cond, s, R, bits, d, hd
.gadget amd64_fti_\cond\()\s\()_\d
    ldr x10, [_ip]
    and \R\()15, \R\hd, \R\()10
    mov x11, \s
    amd64_cached_set_logic_flags x15, x11
    mov w14, 0
    mov w16, 0
    amd64_fused_cc_tail \cond, \bits, 8
.endm
.macro amd64_ft8_srcs cond, s, R, bits, d, hd
    amd64_ftr8 \cond, \s, \R, \bits, \d, \hd, 0, 20
    amd64_ftr8 \cond, \s, \R, \bits, \d, \hd, 1, 22
    amd64_ftr8 \cond, \s, \R, \bits, \d, \hd, 2, 23
    amd64_ftr8 \cond, \s, \R, \bits, \d, \hd, 3, 21
    amd64_ftr8 \cond, \s, \R, \bits, \d, \hd, 4, 27
    amd64_ftr8 \cond, \s, \R, \bits, \d, \hd, 5, 26
    amd64_ftr8 \cond, \s, \R, \bits, \d, \hd, 6, 24
    amd64_ftr8 \cond, \s, \R, \bits, \d, \hd, 7, 25
    amd64_fti8 \cond, \s, \R, \bits, \d, \hd
.endm
.macro amd64_ft8_dsts cond, s, R, bits
    amd64_ft8_srcs \cond, \s, \R, \bits, 0, 20
    amd64_ft8_srcs \cond, \s, \R, \bits, 1, 22
    amd64_ft8_srcs \cond, \s, \R, \bits, 2, 23
    amd64_ft8_srcs \cond, \s, \R, \bits, 3, 21
    amd64_ft8_srcs \cond, \s, \R, \bits, 4, 27
    amd64_ft8_srcs \cond, \s, \R, \bits, 5, 26
    amd64_ft8_srcs \cond, \s, \R, \bits, 6, 24
    amd64_ft8_srcs \cond, \s, \R, \bits, 7, 25
.endm
.irp cond, o,c,z,cz,s,sxo,sxoz
    amd64_ft8_dsts \cond, 32, w, 31
    amd64_ft8_dsts \cond, 64, x, 63
.endr
.macro amd64_ftr_quad cond, s, d, sr
    .ifc \cond, p
    .quad 0
    .else
    .quad NAME(gadget_amd64_ftr_\cond\()\s\()_\d\()_\sr)
    .endif
.endm
.macro amd64_fti_quad cond, s, d
    .ifc \cond, p
    .quad 0
    .else
    .quad NAME(gadget_amd64_fti_\cond\()\s\()_\d)
    .endif
.endm
.pushsection_rodata
.global.name amd64_ftr_gadgets
    .irp cond, o,c,z,cz,s,p,sxo,sxoz
        .irp s, 32,64
            .irp d, 0,1,2,3,4,5,6,7
                .irp sr, 0,1,2,3,4,5,6,7
                    amd64_ftr_quad \cond, \s, \d, \sr
                .endr
            .endr
        .endr
    .endr
.global.name amd64_fti_gadgets
    .irp cond, o,c,z,cz,s,p,sxo,sxoz
        .irp s, 32,64
            .irp d, 0,1,2,3,4,5,6,7
                amd64_fti_quad \cond, \s, \d
            .endr
        .endr
    .endr
.popsection

# neg/not r32/r64 per register, any of the sixteen: amd64_neg<size>_<reg>
# (full and _nf; the flags of 0 - x) and amd64_not<size>_<reg> (no flags).
# test r, r per register pair: amd64_tst<size>_<a>_<b>, logic flags, flags
# only. No operands, gret 0. Tables amd64_neg{,_nf}_gadgets[size64 * 16 +
# reg], amd64_not_gadgets[size64 * 16 + reg], amd64_tst_gadgets[(size64 *
# 16 + a) * 16 + b].
.macro amd64_nn16 op, s, R, bits, nf, d, hd
.ifc \op, not
.gadget amd64_not\s\()_\d
    .if \d < 8
    mvn \R\hd, \R\hd
    .else
    ldr \R\()13, [_cpu, CPU_amd64_regs + 8 * \d]
    mvn \R\()13, \R\()13
    str x13, [_cpu, CPU_amd64_regs + 8 * \d]
    .endif
    gret
.else
.if \nf
.gadget amd64_neg\s\()_\d\()_nf
    .if \d < 8
    neg \R\hd, \R\hd
    .else
    ldr \R\()13, [_cpu, CPU_amd64_regs + 8 * \d]
    neg \R\()13, \R\()13
    str x13, [_cpu, CPU_amd64_regs + 8 * \d]
    .endif
    gret
.else
.gadget amd64_neg\s\()_\d
    amd64_r16_get \R, 10, \d, \hd
    mov x13, 0
    subs \R\()15, \R\()zr, \R\()10
    cset w14, cc
    cset w16, vs
    amd64_cached_set_addsub_flags \bits
    amd64_r16_put 15, \d, \hd
    gret
.endif
.endif
.endm
amd64_r16_ri amd64_nn16, not, 32, w, 31, 0
amd64_r16_ri amd64_nn16, not, 64, x, 63, 0
amd64_r16_family amd64_r16_ri, amd64_nn16, neg
.macro amd64_tst16 op, s, R, bits, nf, d, hd, sr, hs
.gadget amd64_tst\s\()_\d\()_\sr
    .if \d < 8
    .if \sr < 8
    and \R\()15, \R\hd, \R\hs
    .else
    ldr \R\()10, [_cpu, CPU_amd64_regs + 8 * \sr]
    and \R\()15, \R\hd, \R\()10
    .endif
    .else
    ldr \R\()13, [_cpu, CPU_amd64_regs + 8 * \d]
    .if \sr < 8
    and \R\()15, \R\()13, \R\hs
    .else
    ldr \R\()10, [_cpu, CPU_amd64_regs + 8 * \sr]
    and \R\()15, \R\()13, \R\()10
    .endif
    .endif
    mov x11, \s
    amd64_cached_set_logic_flags x15, x11
    gret
.endm
amd64_r16_rr amd64_tst16, test, 32, w, 31, 0
amd64_r16_rr amd64_tst16, test, 64, x, 63, 0
.macro amd64_nn_quad op, s, d, nf
    .if \nf
    .quad NAME(gadget_amd64_\op\()\s\()_\d\()_nf)
    .else
    .quad NAME(gadget_amd64_\op\()\s\()_\d)
    .endif
.endm
.macro amd64_nn_table op, nf
    .irp s, 32,64
        .irp d, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
            amd64_nn_quad \op, \s, \d, \nf
        .endr
    .endr
.endm
.macro amd64_tst_quad s, d, sr
    .quad NAME(gadget_amd64_tst\s\()_\d\()_\sr)
.endm
.macro amd64_sbi_quad op, b
    .if \b == 16
    .quad NAME(gadget_amd64_sbi_\op\()_r)
    .elseif \b == 17
    .quad NAME(gadget_amd64_sbi_\op\()_x)
    .else
    .quad NAME(gadget_amd64_sbi_\op\()_\b)
    .endif
.endm
.macro amd64_scmi_quad s, b
    .if \b == 16
    .quad NAME(gadget_amd64_scmi\s\()_r)
    .elseif \b == 17
    .quad NAME(gadget_amd64_scmi\s\()_x)
    .else
    .quad NAME(gadget_amd64_scmi\s\()_\b)
    .endif
.endm
.pushsection_rodata
.global.name amd64_neg_gadgets
    amd64_nn_table neg, 0
.global.name amd64_neg_nf_gadgets
    amd64_nn_table neg, 1
.global.name amd64_not_gadgets
    amd64_nn_table not, 0
.global.name amd64_tst_gadgets
    .irp s, 32,64
        .irp d, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
            .irp sr, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
                amd64_tst_quad \s, \d, \sr
            .endr
        .endr
    .endr
.global.name amd64_sbi_gadgets
    .irp op, cmp,test
        .irp b, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17
            amd64_sbi_quad \op, \b
        .endr
    .endr
.global.name amd64_scmi_gadgets
    .irp s, 32,64
        .irp b, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17
            amd64_scmi_quad \s, \b
        .endr
    .endr
.popsection

# cmov r32/r64, r32/r64 on the register cache, in two gadgets:
# amd64_cc_<n><cond> leaves the condition (0/1) in x3, then
# amd64_csel<size>_<dst>_<src> moves src into dst if x3 is set -- any of the
# sixteen. The 32-bit form writes dst either way (zero-extending it), as
# x86 does. No operands. Tables amd64_cc_gadgets[cc] (the x86 condition
# code) and amd64_csel_gadgets[(size64 * 16 + dst) * 16 + src].
.macro amd64_cc1 cond, n
.gadget amd64_cc_\n\cond
    amd64_do_jump \cond, 1f
    .ifc \n, n
    mov x3, 1
    .else
    mov x3, 0
    .endif
    gret
1:
    .ifc \n, n
    mov x3, 0
    .else
    mov x3, 1
    .endif
    gret
.endm
.irp cond, o,c,z,cz,s,p,sxo,sxoz
    amd64_cc1 \cond,
    amd64_cc1 \cond, n
.endr
.macro amd64_csel16 op, s, R, bits, nf, d, hd, sr, hs
.gadget amd64_csel\s\()_\d\()_\sr
    cmp x3, 0
    .if \sr < 8
    .if \d < 8
    csel \R\hd, \R\hs, \R\hd, ne
    .else
    ldr \R\()13, [_cpu, CPU_amd64_regs + 8 * \d]
    csel \R\()13, \R\hs, \R\()13, ne
    str x13, [_cpu, CPU_amd64_regs + 8 * \d]
    .endif
    .else
    ldr \R\()10, [_cpu, CPU_amd64_regs + 8 * \sr]
    .if \d < 8
    csel \R\hd, \R\()10, \R\hd, ne
    .else
    ldr \R\()13, [_cpu, CPU_amd64_regs + 8 * \d]
    csel \R\()13, \R\()10, \R\()13, ne
    str x13, [_cpu, CPU_amd64_regs + 8 * \d]
    .endif
    .endif
    gret
.endm
amd64_r16_rr amd64_csel16, csel, 32, w, 31, 0
amd64_r16_rr amd64_csel16, csel, 64, x, 63, 0
.macro amd64_cc_quad cond, n
    .quad NAME(gadget_amd64_cc_\n\cond)
.endm
.macro amd64_csel_quad s, d, sr
    .quad NAME(gadget_amd64_csel\s\()_\d\()_\sr)
.endm
.pushsection_rodata
.global.name amd64_cc_gadgets
    .irp cond, o,c,z,cz,s,p,sxo,sxoz
    amd64_cc_quad \cond,
    amd64_cc_quad \cond, n
    .endr
.global.name amd64_csel_gadgets
    .irp s, 32,64
        .irp d, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
            .irp sr, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
                amd64_csel_quad \s, \d, \sr
            .endr
        .endr
    .endr
.popsection

# cbw/cwde/cdqe (98) and cwd/cdq/cqo (99) on the register cache (rax x20,
# rdx x23): amd64_sx_<kind>, table amd64_sx_gadgets[(opcode - 0x98) * 3 +
# size_idx] (16, 32, 64). No flags, no operands.
.gadget amd64_sx_cbw
    sxtb w8, w20
    bfi x20, x8, 0, 16
    gret
.gadget amd64_sx_cwde
    sxth w20, w20
    gret
.gadget amd64_sx_cdqe
    sxtw x20, w20
    gret
.gadget amd64_sx_cwd
    sxth w8, w20
    asr w8, w8, 15
    bfi x23, x8, 0, 16
    gret
.gadget amd64_sx_cdq
    asr w23, w20, 31
    gret
.gadget amd64_sx_cqo
    asr x23, x20, 63
    gret
.pushsection_rodata
.global.name amd64_sx_gadgets
    .quad NAME(gadget_amd64_sx_cbw), NAME(gadget_amd64_sx_cwde), NAME(gadget_amd64_sx_cdqe)
    .quad NAME(gadget_amd64_sx_cwd), NAME(gadget_amd64_sx_cdq), NAME(gadget_amd64_sx_cqo)
.popsection

# xchg rax, reg (90+r) and bswap reg (0F C8+r), 32/64-bit, per register on
# the cache: amd64_xchg<size>_<reg>, amd64_bsw<size>_<reg>. 32-bit forms
# zero-extend what they write (both registers, for xchg). No flags, no
# operands. Tables amd64_{xchg,bsw}_gadgets[size64 * 16 + reg].
.macro amd64_xb16 op, s, R, bits, nf, d, hd
.ifc \op, xchg
.gadget amd64_xchg\s\()_\d
    mov \R\()8, \R\()20
    .if \d < 8
    mov \R\()20, \R\hd
    mov \R\hd, \R\()8
    .else
    ldr \R\()10, [_cpu, CPU_amd64_regs + 8 * \d]
    mov \R\()20, \R\()10
    str x8, [_cpu, CPU_amd64_regs + 8 * \d]
    .endif
    gret
.else
.gadget amd64_bsw\s\()_\d
    .if \d < 8
    rev \R\hd, \R\hd
    .else
    ldr \R\()10, [_cpu, CPU_amd64_regs + 8 * \d]
    rev \R\()10, \R\()10
    str x10, [_cpu, CPU_amd64_regs + 8 * \d]
    .endif
    gret
.endif
.endm
amd64_r16_ri amd64_xb16, xchg, 32, w, 31, 0
amd64_r16_ri amd64_xb16, xchg, 64, x, 63, 0
amd64_r16_ri amd64_xb16, bsw, 32, w, 31, 0
amd64_r16_ri amd64_xb16, bsw, 64, x, 63, 0
.pushsection_rodata
.global.name amd64_xchg_gadgets
    amd64_nn_table xchg, 0
.global.name amd64_bsw_gadgets
    amd64_nn_table bsw, 0
.popsection

# setcc r8 per condition and register, on the register cache:
# amd64_scc_<n><cond>_<reg> for the low byte of any of the sixteen, and
# amd64_scc_<n><cond>_h<reg> for ah..bh (reg 0-3). n is empty or n (the
# negated condition). No operand. Table amd64_scc_gadgets[cc * 20 + slot],
# cc the x86 condition code (0-15), slot 0-15 low byte, 16-19 ah..bh.
.macro amd64_scc1 cond, n, d, hd, high
    .if \high
    .gadget amd64_scc_\n\cond\()_h\d
    .else
    .gadget amd64_scc_\n\cond\()_\d
    .endif
    amd64_do_jump \cond, 1f
    .ifc \n, n
    mov w15, 1
    .else
    mov w15, 0
    .endif
    b 2f
1:
    .ifc \n, n
    mov w15, 0
    .else
    mov w15, 1
    .endif
2:
    .if \d < 8
    bfi x\hd, x15, 8 * \high, 8
    .else
    ldr x13, [_cpu, CPU_amd64_regs + 8 * \d]
    bfi x13, x15, 0, 8
    str x13, [_cpu, CPU_amd64_regs + 8 * \d]
    .endif
    gret
.endm
.macro amd64_scc_regs cond, n
    amd64_scc1 \cond, \n, 0, 20, 0
    amd64_scc1 \cond, \n, 1, 22, 0
    amd64_scc1 \cond, \n, 2, 23, 0
    amd64_scc1 \cond, \n, 3, 21, 0
    amd64_scc1 \cond, \n, 4, 27, 0
    amd64_scc1 \cond, \n, 5, 26, 0
    amd64_scc1 \cond, \n, 6, 24, 0
    amd64_scc1 \cond, \n, 7, 25, 0
    .irp d, 8,9,10,11,12,13,14,15
    amd64_scc1 \cond, \n, \d, 0, 0
    .endr
    amd64_scc1 \cond, \n, 0, 20, 1
    amd64_scc1 \cond, \n, 1, 22, 1
    amd64_scc1 \cond, \n, 2, 23, 1
    amd64_scc1 \cond, \n, 3, 21, 1
.endm
.irp cond, o,c,z,cz,s,p,sxo,sxoz
    amd64_scc_regs \cond,
    amd64_scc_regs \cond, n
.endr
.macro amd64_scc_row cond, n
    .irp d, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
    .quad NAME(gadget_amd64_scc_\n\cond\()_\d)
    .endr
    .irp d, 0,1,2,3
    .quad NAME(gadget_amd64_scc_\n\cond\()_h\d)
    .endr
.endm
.pushsection_rodata
.global.name amd64_scc_gadgets
    .irp cond, o,c,z,cz,s,p,sxo,sxoz
    amd64_scc_row \cond,
    amd64_scc_row \cond, n
    .endr
.popsection

# movzx/movsx r32/r64, r8/r16 (0F B6/B7/BE/BF, register forms) per register:
# amd64_mx_<kind>_<dst>_<src>, any of the sixteen. Kinds: zb, zw (zero-extend
# a byte/word; the 32- and 64-bit results are the same), sb32, sb64, sw32,
# sw64 (sign-extend), and zbh, sbh32, sbh64 for ah..bh (bits 8-15 of rax..rbx,
# src 0-3), and sd (movsxd r64, r32: 63 with REX.W). No flags. Table
# amd64_mx_gadgets[(kind * 16 + dst) * 16 + src] in that kind order, the
# high-byte kinds' src 4-15 empty.
.macro amd64_mx_op kind, rd, wd, xs, ws
    .ifc \kind, zb
    uxtb \wd, \ws
    .endif
    .ifc \kind, zw
    uxth \wd, \ws
    .endif
    .ifc \kind, sb32
    sxtb \wd, \ws
    .endif
    .ifc \kind, sb64
    sxtb \rd, \ws
    .endif
    .ifc \kind, sw32
    sxth \wd, \ws
    .endif
    .ifc \kind, sw64
    sxth \rd, \ws
    .endif
    .ifc \kind, zbh
    ubfx \wd, \ws, 8, 8
    .endif
    .ifc \kind, sbh32
    sbfx \wd, \ws, 8, 8
    .endif
    .ifc \kind, sbh64
    sbfx \rd, \xs, 8, 8
    .endif
    .ifc \kind, sd
    sxtw \rd, \ws
    .endif
.endm
.macro amd64_mx16 op, s, R, bits, nf, d, hd, sr, hs
.gadget amd64_mx_\op\()_\d\()_\sr
    .if \sr < 8
    .if \d < 8
    amd64_mx_op \op, x\hd, w\hd, x\hs, w\hs
    .else
    amd64_mx_op \op, x13, w13, x\hs, w\hs
    str x13, [_cpu, CPU_amd64_regs + 8 * \d]
    .endif
    .else
    ldr x10, [_cpu, CPU_amd64_regs + 8 * \sr]
    .if \d < 8
    amd64_mx_op \op, x\hd, w\hd, x10, w10
    .else
    amd64_mx_op \op, x13, w13, x10, w10
    str x13, [_cpu, CPU_amd64_regs + 8 * \d]
    .endif
    .endif
    gret
.endm
.macro amd64_mxh_srcs op, s, R, bits, nf, d, hd
    amd64_mx16 \op, \s, \R, \bits, \nf, \d, \hd, 0, 20
    amd64_mx16 \op, \s, \R, \bits, \nf, \d, \hd, 1, 22
    amd64_mx16 \op, \s, \R, \bits, \nf, \d, \hd, 2, 23
    amd64_mx16 \op, \s, \R, \bits, \nf, \d, \hd, 3, 21
.endm
.irp kind, zb,zw,sb32,sb64,sw32,sw64
    amd64_r16_rr amd64_mx16, \kind, 0, x, 0, 0
.endr
.irp kind, zbh,sbh32,sbh64
    amd64_r16_ri amd64_mxh_srcs, \kind, 0, x, 0, 0
.endr
amd64_r16_rr amd64_mx16, sd, 0, x, 0, 0
.macro amd64_mx_quad kind, d, sr
    .quad NAME(gadget_amd64_mx_\kind\()_\d\()_\sr)
.endm
.macro amd64_mx_row kind, d
    .irp sr, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
        amd64_mx_quad \kind, \d, \sr
    .endr
.endm
.macro amd64_mxh_row kind, d
    .irp sr, 0,1,2,3
        amd64_mx_quad \kind, \d, \sr
    .endr
    .rept 12
    .quad 0
    .endr
.endm
.pushsection_rodata
.global.name amd64_mx_gadgets
    .irp kind, zb,zw,sb32,sb64,sw32,sw64
        .irp d, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
            amd64_mx_row \kind, \d
        .endr
    .endr
    .irp kind, zbh,sbh32,sbh64
        .irp d, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
            amd64_mxh_row \kind, \d
        .endr
    .endr
    .irp d, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
        amd64_mx_row sd, \d
    .endr
.popsection

# test r8/r32/r64, imm (F6 /0, F7 /0, A8, A9) per register:
# amd64_tsi<size>_<reg>, the imm in the stream word (zero-extended for 8 and
# 32, sign-extended for 64). Low byte only for 8 (ah..bh keep the byte
# gadget). Logic flags, no write. Table amd64_tsi_gadgets[size_idx * 16 +
# reg], size_idx 0 = 8, 1 = 32, 2 = 64.
.macro amd64_tsi16 op, s, R, bits, nf, d, hd
.gadget amd64_tsi\s\()_\d
    ldr x13, [_ip]
    .if \d < 8
    and \R\()15, \R\hd, \R\()13
    .else
    ldr \R\()10, [_cpu, CPU_amd64_regs + 8 * \d]
    and \R\()15, \R\()10, \R\()13
    .endif
    .if \s == 8
    and w15, w15, 0xff
    .endif
    mov x11, \s
    amd64_cached_set_logic_flags x15, x11
    gret 1
.endm
amd64_r16_ri amd64_tsi16, test, 8, w, 7, 0
amd64_r16_ri amd64_tsi16, test, 32, w, 31, 0
amd64_r16_ri amd64_tsi16, test, 64, x, 63, 0
.pushsection_rodata
.global.name amd64_tsi_gadgets
    .irp s, 8,32,64
        .irp d, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
            amd64_nn_quad tsi, \s, \d, 0
        .endr
    .endr
.popsection

.macro amd64_b18_quad pre, s, b, nf
    .if \b == 16
        .if \nf
        .quad NAME(gadget_amd64_\pre\()\s\()_r_nf)
        .else
        .quad NAME(gadget_amd64_\pre\()\s\()_r)
        .endif
    .elseif \b == 17
        .if \nf
        .quad NAME(gadget_amd64_\pre\()\s\()_x_nf)
        .else
        .quad NAME(gadget_amd64_\pre\()\s\()_x)
        .endif
    .else
        .if \nf
        .quad NAME(gadget_amd64_\pre\()\s\()_\b\()_nf)
        .else
        .quad NAME(gadget_amd64_\pre\()\s\()_\b)
        .endif
    .endif
.endm
.macro amd64_b18_row pre, s, nf
    .irp b, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17
        amd64_b18_quad \pre, \s, \b, \nf
    .endr
.endm
# Tables amd64_ssi_gadgets[size_index * 18 + base] (sizes 8 16 32 64) and
# amd64_sri{,_nf}_gadgets[(op * 2 + size64) * 18 + base] (ops add sub or and
# xor).
.pushsection_rodata
.global.name amd64_ssi_gadgets
    .irp sz, 8,16,32,64
        amd64_b18_row ssi, \sz, 0
    .endr
.global.name amd64_sri_gadgets
    .irp op, add,sub,or,and,xor
        amd64_b18_row sri_\op, 32, 0
        amd64_b18_row sri_\op, 64, 0
    .endr
.global.name amd64_sri_nf_gadgets
    .irp op, add,sub,or,and,xor
        amd64_b18_row sri_\op, 32, 1
        amd64_b18_row sri_\op, 64, 1
    .endr
.popsection
