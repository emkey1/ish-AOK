#include "gadgets.h"
#include "math.h"
#include "emu/interrupt.h"

.gadget load32_addr
    mov _tmp, _addr
    gret

.gadget load16_gs
    ldrh _tmp, [_cpu, #CPU_gs]
    gret

.gadget store16_gs
    strh _tmp, [_cpu, #CPU_gs]
    gret

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
    cmp \size, 64
    b.eq .Lamd64_cached_flags_64_\@
    cmp \size, 32
    b.eq .Lamd64_cached_flags_32_\@
    cmp \size, 16
    b.eq .Lamd64_cached_flags_16_\@
    tst \result, 0x80
    b.eq .Lamd64_cached_flags_done_\@
    orr w16, w16, SF_FLAG
    b .Lamd64_cached_flags_done_\@
.Lamd64_cached_flags_16_\@: tst \result, 0x8000
    b.eq .Lamd64_cached_flags_done_\@
    orr w16, w16, SF_FLAG
    b .Lamd64_cached_flags_done_\@
.Lamd64_cached_flags_32_\@: tbnz \result, 31, .Lamd64_cached_flags_set_sf_\@
    b .Lamd64_cached_flags_done_\@
.Lamd64_cached_flags_64_\@: tbnz \result, 63, .Lamd64_cached_flags_set_sf_\@
    b .Lamd64_cached_flags_done_\@
.Lamd64_cached_flags_set_sf_\@: orr w16, w16, SF_FLAG
.Lamd64_cached_flags_done_\@: str w16, [_cpu, CPU_eflags]
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

.gadget amd64_cached_logic_reg_imm
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

.macro amd64_cached_set_addsub_flags sign_bit
    strb w14, [_cpu, CPU_cf]
    strb w16, [_cpu, CPU_of]
    ldr w17, [_cpu, CPU_eflags]
    mov x12, (1 | PF_FLAG | AF_FLAG | ZF_FLAG | SF_FLAG | (1 << 11))
    bic w17, w17, w12
    cbz w14, .Lamd64_addsub_no_cf_\@
    orr w17, w17, 1
.Lamd64_addsub_no_cf_\@: cbz w16, .Lamd64_addsub_no_of_\@
    orr w17, w17, (1 << 11)
.Lamd64_addsub_no_of_\@: eor x12, x13, x10
    eor x12, x12, x15
    tbz x12, 4, .Lamd64_addsub_no_af_\@
    orr w17, w17, AF_FLAG
.Lamd64_addsub_no_af_\@: cmp x15, 0
    b.ne .Lamd64_addsub_no_zf_\@
    orr w17, w17, ZF_FLAG
.Lamd64_addsub_no_zf_\@: tbnz x15, \sign_bit, .Lamd64_addsub_set_sf_\@
    b .Lamd64_addsub_after_sf_\@
.Lamd64_addsub_set_sf_\@: orr w17, w17, SF_FLAG
.Lamd64_addsub_after_sf_\@: and w12, w15, 0xff
    eor w12, w12, w12, lsr 4
    eor w12, w12, w12, lsr 2
    eor w12, w12, w12, lsr 1
    mvn w12, w12
    and w12, w12, 1
    cbz w12, .Lamd64_addsub_no_pf_\@
    orr w17, w17, PF_FLAG
.Lamd64_addsub_no_pf_\@: ldrb w12, [_cpu, CPU_flags_res]
    bic w12, w12, (ZF_RES | SF_RES | PF_RES | AF_OPS)
    strb w12, [_cpu, CPU_flags_res]
    str w17, [_cpu, CPU_eflags]
.endm

.gadget amd64_cached_arith_reg_imm
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

.gadget amd64_cached_arith_reg_reg
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

.macro amd64_cached_set_shift_flags sign_bit
    strb w14, [_cpu, CPU_cf]
    strb w16, [_cpu, CPU_of]
    ldr w17, [_cpu, CPU_eflags]
    mov x12, (1 | PF_FLAG | AF_FLAG | ZF_FLAG | SF_FLAG | (1 << 11))
    bic w17, w17, w12
    cbz w14, .Lamd64_shift_no_cf_\@
    orr w17, w17, 1
.Lamd64_shift_no_cf_\@: cbz w16, .Lamd64_shift_no_of_\@
    orr w17, w17, (1 << 11)
.Lamd64_shift_no_of_\@: cmp x15, 0
    b.ne .Lamd64_shift_no_zf_\@
    orr w17, w17, ZF_FLAG
.Lamd64_shift_no_zf_\@: tbnz x15, \sign_bit, .Lamd64_shift_set_sf_\@
    b .Lamd64_shift_after_sf_\@
.Lamd64_shift_set_sf_\@: orr w17, w17, SF_FLAG
.Lamd64_shift_after_sf_\@: and w12, w15, 0xff
    eor w12, w12, w12, lsr 4
    eor w12, w12, w12, lsr 2
    eor w12, w12, w12, lsr 1
    mvn w12, w12
    and w12, w12, 1
    cbz w12, .Lamd64_shift_no_pf_\@
    orr w17, w17, PF_FLAG
.Lamd64_shift_no_pf_\@: ldrb w12, [_cpu, CPU_flags_res]
    bic w12, w12, (ZF_RES | SF_RES | PF_RES | AF_OPS)
    strb w12, [_cpu, CPU_flags_res]
    str w17, [_cpu, CPU_eflags]
.endm

.gadget amd64_cached_shift_reg_count
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
    cbz x10, 9f
    amd64_cached_read x8, x13
    mov w13, w13
    cmp x9, 4
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

.gadget amd64_cached_logic_reg_reg
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
.gadget amd64_arith_reg_reg
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

# Flush-style ADD/SUB/CMP reg,imm (0x81/0x83 /0,/5,/7), 32/64 -- high-reg sibling of
# cached_arith_reg_imm (low-8 only); arith reg-imm had no flush fallback (logic does),
# so r8-r15 bridged via amd64_jit_reg_imm_op. Operand: group bits 8-11 (0 add/5 sub/
# 7 cmp), rm bits 12-15, size bits 16-22; imm = operand 1. CMP (/7) sets flags, no write.
.gadget amd64_arith_reg_imm
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
        setf_a \arg, \dst, \s
    .endifin
    .ifin(\op, and,orr,eor)
        clearf_a
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

    .ifin(\op, add,sub,adc,sbc,and,orr,eor)
        setf_zsp \s, \dst
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
        gret
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
            gret 1
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
            gret
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
        gret 1
.endm
.each_reg xfa_add
.purgem xfa_add

.macro xfa_or name, reg
    .gadget fused_or32_imm_\name
        ldr w8, [_ip]
        do_op orr, 32, w8, \reg
        gret 1
.endm
.each_reg xfa_or
.purgem xfa_or

.macro xfa_adc name, reg
    .gadget fused_adc32_imm_\name
        ldr w8, [_ip]
        do_op adc, 32, w8, \reg
        gret 1
.endm
.each_reg xfa_adc
.purgem xfa_adc

.macro xfa_sbb name, reg
    .gadget fused_sbb32_imm_\name
        ldr w8, [_ip]
        do_op sbc, 32, w8, \reg
        gret 1
.endm
.each_reg xfa_sbb
.purgem xfa_sbb

.macro xfa_and name, reg
    .gadget fused_and32_imm_\name
        ldr w8, [_ip]
        do_op and, 32, w8, \reg
        gret 1
.endm
.each_reg xfa_and
.purgem xfa_and

.macro xfa_sub name, reg
    .gadget fused_sub32_imm_\name
        ldr w8, [_ip]
        do_op sub, 32, w8, \reg
        gret 1
.endm
.each_reg xfa_sub
.purgem xfa_sub

.macro xfa_xor name, reg
    .gadget fused_xor32_imm_\name
        ldr w8, [_ip]
        do_op eor, 32, w8, \reg
        gret 1
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

# atomics. oof

.macro do_op_size_atomic opname, op, size, s
    .gadget atomic_\opname\size\()_mem
        # There's so much stuff going on inside most of these operations that
        # the implementation is a compare-and-swap loop, instead of just ldaxr/stlxr
        write_prep \size, atomic_\opname\size\()_mem
        .if \size != 8
            tst _addr, (\size/8)-1
            b.eq 9f
            save_c
            mov x0, _cpu
            mov w1, _addr
            .if \size == 16
                mov w2, 3
            .else
                mov w2, 4
            .endif
            bl NAME(helper_trace_unaligned_atomic)
            restore_c
        9:
        .endif
        dmb ish
        ldr\s w12, [_xaddr]
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
            ldrb w10, [_cpu, CPU_cf]
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

.irp op, add,sub,adc,sbb,and,or,xor,inc,dec,xadd
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
    setf_a w10, _tmp, \s
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
    setf_zsp \s
.endm
.macro do_dec size, s
    mov w10, 1
    setf_a w10, _tmp, \s
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
    setf_zsp \s
.endm

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
            gret
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

# psrlq/psllq xmm, imm8 (66 0F 73 /2,/6): shift each 64-bit lane by an imm baked
# into bits 8-15 of the operand word; rm xmm id in bits 0-3 is both source and
# dest. A shift count > 63 yields all-zero lanes (the variable lsr/lsl would
# otherwise wrap mod 64), selected branchlessly: lsr/lsl leave NZCV from the cmp
# intact, so csel zeroes the lanes when count > 63.
.macro amd64_v_shift_q_imm name, shift
    .gadget amd64_v_\name\()_imm
        ldr x8, [_ip]
        and x9, x8, 0xf
        ubfx x10, x8, 8, 8
        add x11, _cpu, CPU_xmm
        add x9, x11, x9, lsl 4
        ldp x12, x13, [x9]
        cmp x10, 63
        \shift x12, x12, x10
        \shift x13, x13, x10
        csel x12, xzr, x12, hi
        csel x13, xzr, x13, hi
        stp x12, x13, [x9]
        gret 1
.endm

# 66 0F 73 /2 ib psrlq
amd64_v_shift_q_imm psrlq, lsr
# 66 0F 73 /6 ib psllq
amd64_v_shift_q_imm psllq, lsl

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

# cvtsi2sd xmm, r/m (F2 0F 2A), reg-reg: xmm[dst].f64[0] = (double) signed GPR[src].
# Operand: GPR src id bits 0-3, xmm dst id bits 4-7, REX.W in bit 8 (1=64-bit
# source, 0=32-bit). gen.c flushes the reg cache first so cpu->amd64_regs is
# current, then this reads the source straight from memory. scvtf matches the
# interp's C (double) cast bit-for-bit. Only the low 64 bits of dst are written.
.gadget amd64_v_cvtsi2sd_reg
    ldr x8, [_ip]
    and x9, x8, 0xf
    ubfx x10, x8, 4, 4
    add x12, _cpu, CPU_amd64_regs
    add x12, x12, x9, lsl 3
    tbz x8, 8, 1f
    ldr x12, [x12]
    scvtf d0, x12
    b 2f
1:  ldr w12, [x12]
    scvtf d0, w12
2:  add x13, _cpu, CPU_xmm
    add x13, x13, x10, lsl 4
    str d0, [x13]
    gret 1

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
.gadget amd64_v_movq_xx_reg
    ldr x8, [_ip]
    and x9, x8, 0xf
    ubfx x10, x8, 4, 4
    add x11, _cpu, CPU_xmm
    add x9, x11, x9, lsl 4
    add x10, x11, x10, lsl 4
    ldr x12, [x9]
    stp x12, xzr, [x10]
    gret 1

# movd/movq r/m, xmm (66 0F 7E), mod==3: store the low 32 (movd) or 64 (movq,
# REX.W bit 8) bits of xmm[src] into a GPR. The 32-bit form zero-extends into
# the full 64-bit register (all 32-bit GPR writes do). Writes a GPR, so gen.c
# flushes the reg cache first (flush also invalidates -- same contract as
# gadget_amd64_arith_reg_reg). Operand: GPR dst id bits 0-3, xmm src id bits
# 4-7, REX.W bit 8.
.gadget amd64_v_movd_store_reg
    ldr x8, [_ip]
    and x9, x8, 0xf
    ubfx x10, x8, 4, 4
    add x12, _cpu, CPU_amd64_regs
    add x12, x12, x9, lsl 3
    add x13, _cpu, CPU_xmm
    add x13, x13, x10, lsl 4
    tbz x8, 8, 1f
    ldr x14, [x13]
    str x14, [x12]
    gret 1
1:  ldr w14, [x13]                  /* zero-extends into x14 */
    str x14, [x12]
    gret 1

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
.gadget amd64_v_movd_reg
    ldr x8, [_ip]
    and x9, x8, 0xf
    ubfx x10, x8, 4, 4
    add x12, _cpu, CPU_amd64_regs
    add x12, x12, x9, lsl 3
    add x13, _cpu, CPU_xmm
    add x13, x13, x10, lsl 4
    tbz x8, 8, 1f
    ldr x14, [x12]
    str x14, [x13]
    str xzr, [x13, 8]
    gret 1
1:  ldr w14, [x12]                  /* 32-bit: zero-extends into x14 */
    str x14, [x13]
    str xzr, [x13, 8]
    gret 1

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
    ubfx x11, x9, 12, 10
    eor  x11, x11, x9, lsr 22
    and  x11, x11, 0x3ff
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
.endm

# movaps/movups xmm, m128 (0F 28 / 0F 10): cpu->xmm[reg] = [addr]
.gadget amd64_v_load128_mem
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    ubfx x10, x8, 8, 4
    amd64_vmem_addr
    amd64_vread_prep 16, load128
    ldp  x12, x13, [x9]
    add  x14, _cpu, CPU_xmm
    add  x14, x14, x10, lsl 4
    stp  x12, x13, [x14]
    gret 3
    amd64_vread_bullshit 16, load128

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
    ubfx x11, x9, 12, 10
    eor  x11, x11, x9, lsr 22
    and  x11, x11, 0x3ff
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

# Memory-source variant of the NEON binops above: dst = dst OP [addr], the
# 128-bit source read through the vread TLB fast path. Same operand layout as
# load128_mem ([meta][disp][next_ip], dst xmm id in meta bits 8-11); the reg
# cache and rip are flushed by gen.c so base/index regs are in memory and a
# fault re-executes the instruction. x10 (dst id) survives the prep macro:
# the fast path clobbers only x8/x11/x12/x13 and the miss helpers bracket
# their C calls with save_c/restore_c.
.macro amd64_v_neon_binop_mem name, op, arr
    .gadget amd64_v_\name\()_mem
        ldr  x8, [_ip]
        ldr  x9, [_ip, 8]
        ubfx x10, x8, 8, 4
        amd64_vmem_addr
        amd64_vread_prep 16, \name\()m
        ldr  q1, [x9]
        add  x14, _cpu, CPU_xmm
        add  x14, x14, x10, lsl 4
        ldr  q0, [x14]
        \op  v0.\arr, v0.\arr, v1.\arr
        str  q0, [x14]
        gret 3
        amd64_vread_bullshit 16, \name\()m
.endm

amd64_v_neon_binop_mem paddw, add, 8h
amd64_v_neon_binop_mem psubusw, uqsub, 8h
amd64_v_neon_binop_mem por, orr, 16b
amd64_v_neon_binop_mem punpcklbw, zip1, 16b
amd64_v_neon_binop_mem punpcklwd, zip1, 8h
amd64_v_neon_binop_mem punpckldq, zip1, 4s
amd64_v_neon_binop_mem punpcklqdq, zip1, 2d
amd64_v_neon_binop_mem pcmpeqb, cmeq, 16b

# movq xmm, m64 (F3 0F 7E mem form, or 66 0F 6E with REX.W): load 8 bytes into
# the low lane of xmm[reg], zero the high lane.
.gadget amd64_v_load64_mem
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    ubfx x10, x8, 8, 4
    amd64_vmem_addr
    amd64_vread_prep 8, load64
    ldr  x12, [x9]
    add  x14, _cpu, CPU_xmm
    add  x14, x14, x10, lsl 4
    stp  x12, xzr, [x14]
    gret 3
    amd64_vread_bullshit 8, load64

# movd xmm, m32 (66 0F 6E without REX.W): load 4 bytes, zero the other 12.
.gadget amd64_v_load32_mem
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    ubfx x10, x8, 8, 4
    amd64_vmem_addr
    amd64_vread_prep 4, load32
    ldr  w12, [x9]                  /* zero-extends into x12 */
    add  x14, _cpu, CPU_xmm
    add  x14, x14, x10, lsl 4
    stp  x12, xzr, [x14]
    gret 3
    amd64_vread_bullshit 4, load32

# movq m64, xmm (66 0F D6 mem form, or 66 0F 7E with REX.W): store the low 8
# bytes of xmm[reg].
.gadget amd64_v_store64_mem
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    ubfx x10, x8, 8, 4
    amd64_vmem_addr
    amd64_vwrite_prep 8, store64
    add  x14, _cpu, CPU_xmm
    add  x14, x14, x10, lsl 4
    ldr  x12, [x14]
    str  x12, [x9]
    amd64_vwrite_done 8, store64
    gret 3
    amd64_vwrite_bullshit 8, store64

# movd m32, xmm (66 0F 7E without REX.W): store the low 4 bytes of xmm[reg].
.gadget amd64_v_store32_mem
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    ubfx x10, x8, 8, 4
    amd64_vmem_addr
    amd64_vwrite_prep 4, store32
    add  x14, _cpu, CPU_xmm
    add  x14, x14, x10, lsl 4
    ldr  w12, [x14]
    str  w12, [x9]
    amd64_vwrite_done 4, store32
    gret 3
    amd64_vwrite_bullshit 4, store32

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
    add  x14, _cpu, CPU_amd64_regs
    ldr  x8, [x14, 4*8]
    add  x8, x8, 8
    str  x8, [x14, 4*8]              /* RSP += 8 */
    str  x10, [_cpu, CPU_amd64_rip]  /* rip = target */
    str  w10, [_cpu, CPU_eip]        /* mirror low 32 bits, as amd64_jmp does */
    b    jit_ret
    amd64_vread_bullshit 8, ret

# amd64 indirect jmp, register form (0xff /4, mod==3): rip = regs[rm], exit. No
# memory access. Operand 0 = rm register number (0-15). Always 64-bit in long mode.
.gadget amd64_jmp_indir_reg
    ldr  x8, [_ip]
    add  x14, _cpu, CPU_amd64_regs
    ldr  x10, [x14, x8, lsl 3]       /* target = regs[rm] */
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
    ubfx x11, x8, 40, 2              /* signed (bit0) | dst64 (bit1) */
    orr  x10, x10, x11, lsl 8        /* pack regid | flags<<8 (survives prep) */
    amd64_vmem_addr                  /* x9 = effective address */
    amd64_vread_prep \vb, \name      /* x9 = host addr of the source */
.if \vb == 1
    ldrb w11, [x9]                   /* zero-extended byte */
.else
    ldrh w11, [x9]                   /* zero-extended half */
.endif
    ubfx x12, x10, 8, 2              /* flags */
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
    str  x11, [x13, x10, lsl 3]      /* write dst (full 64-bit slot) */
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
.if \vbytes == 4
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
.macro amd64_v_packed_arith name, fpop, arr
    .gadget amd64_v_\name\()_reg
        ldr x8, [_ip]
        and x9, x8, 0xf
        ubfx x10, x8, 4, 4
        add x11, _cpu, CPU_xmm
        add x9, x11, x9, lsl 4
        add x10, x11, x10, lsl 4
        ldr q0, [x10]
        ldr q1, [x9]
        \fpop v0.\arr, v0.\arr, v1.\arr
        str q0, [x10]
        gret 1
.endm

amd64_v_packed_arith addpd, fadd, 2d
amd64_v_packed_arith subpd, fsub, 2d
amd64_v_packed_arith mulpd, fmul, 2d
amd64_v_packed_arith divpd, fdiv, 2d
amd64_v_packed_arith minpd, fmin, 2d
amd64_v_packed_arith maxpd, fmax, 2d
amd64_v_packed_arith addps, fadd, 4s
amd64_v_packed_arith subps, fsub, 4s
amd64_v_packed_arith mulps, fmul, 4s
amd64_v_packed_arith divps, fdiv, 4s
amd64_v_packed_arith minps, fmin, 4s
amd64_v_packed_arith maxps, fmax, 4s

# Packed 128-bit arithmetic, mem source: dst = dst OP m128.
.macro amd64_v_packed_arith_mem name, fpop, arr
    .gadget amd64_v_\name\()_mem
        ldr  x8, [_ip]
        ldr  x9, [_ip, 8]
        ubfx x10, x8, 8, 4
        amd64_vmem_addr
        amd64_vread_prep 16, \name\()_mem
        ldp  x12, x13, [x9]
        fmov d1, x12
        mov  v1.d[1], x13
        add  x14, _cpu, CPU_xmm
        add  x14, x14, x10, lsl 4
        ldr  q0, [x14]
        \fpop v0.\arr, v0.\arr, v1.\arr
        str  q0, [x14]
        gret 3
        amd64_vread_bullshit 16, \name\()_mem
.endm

amd64_v_packed_arith_mem addpd, fadd, 2d
amd64_v_packed_arith_mem subpd, fsub, 2d
amd64_v_packed_arith_mem mulpd, fmul, 2d
amd64_v_packed_arith_mem divpd, fdiv, 2d
amd64_v_packed_arith_mem minpd, fmin, 2d
amd64_v_packed_arith_mem maxpd, fmax, 2d
amd64_v_packed_arith_mem addps, fadd, 4s
amd64_v_packed_arith_mem subps, fsub, 4s
amd64_v_packed_arith_mem mulps, fmul, 4s
amd64_v_packed_arith_mem divps, fdiv, 4s
amd64_v_packed_arith_mem minps, fmin, 4s
amd64_v_packed_arith_mem maxps, fmax, 4s

# Scalar double, mem source: dst.f64[0] OP= m64, upper lane preserved.
.macro amd64_v_scalar_double_mem name, fpop
    .gadget amd64_v_\name\()_mem
        ldr  x8, [_ip]
        ldr  x9, [_ip, 8]
        ubfx x10, x8, 8, 4
        amd64_vmem_addr
        amd64_vread_prep 8, \name\()_mem
        ldr  x12, [x9]
        fmov d1, x12
        add  x14, _cpu, CPU_xmm
        add  x14, x14, x10, lsl 4
        ldr  d0, [x14]
        \fpop d0, d0, d1
        str  d0, [x14]
        gret 3
        amd64_vread_bullshit 8, \name\()_mem
.endm

amd64_v_scalar_double_mem addsd, fadd
amd64_v_scalar_double_mem subsd, fsub
amd64_v_scalar_double_mem mulsd, fmul
amd64_v_scalar_double_mem divsd, fdiv
amd64_v_scalar_double_mem minsd, fmin
amd64_v_scalar_double_mem maxsd, fmax

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
amd64_v_scalar_single minss, fmin
amd64_v_scalar_single maxss, fmax

# Scalar-double reg-reg stragglers for the existing macro.
amd64_v_scalar_double subsd, fsub
amd64_v_scalar_double divsd, fdiv
amd64_v_scalar_double minsd, fmin
amd64_v_scalar_double maxsd, fmax

# movsd xmm, m64 (F2 0F 10, mod!=3): low lane = m64, upper 64 CLEARED
# (the load form zeroes; only the reg-reg form merges).
.gadget amd64_v_movsd_load
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    ubfx x10, x8, 8, 4
    amd64_vmem_addr
    amd64_vread_prep 8, movsd_load
    ldr  x12, [x9]
    add  x14, _cpu, CPU_xmm
    add  x14, x14, x10, lsl 4
    stp  x12, xzr, [x14]
    gret 3
    amd64_vread_bullshit 8, movsd_load

# movss xmm, m32 (F3 0F 10, mod!=3): low lane = m32, upper 96 CLEARED.
.gadget amd64_v_movss_load
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    ubfx x10, x8, 8, 4
    amd64_vmem_addr
    amd64_vread_prep 4, movss_load
    ldr  w12, [x9]
    add  x14, _cpu, CPU_xmm
    add  x14, x14, x10, lsl 4
    stp  x12, xzr, [x14]
    gret 3
    amd64_vread_bullshit 4, movss_load

# movsd m64, xmm (F2 0F 11, mod!=3) / movss m32, xmm (F3 0F 11).
.gadget amd64_v_movsd_store
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    ubfx x10, x8, 8, 4
    // address FIRST: amd64_vmem_addr clobbers x14/x15 (index-register
    // shift path), which silently corrupted the staged store value for
    // indexed addressing (arrays) — caught by the fpforms O0/O2 cross-check.
    amd64_vmem_addr
    add  x14, _cpu, CPU_xmm
    add  x14, x14, x10, lsl 4
    ldr  x15, [x14]
    amd64_vwrite_prep 8, movsd_store
    str  x15, [x9]
    amd64_vwrite_done 8, movsd_store
    gret 3
    amd64_vwrite_bullshit 8, movsd_store

.gadget amd64_v_movss_store
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    ubfx x10, x8, 8, 4
    // address FIRST: amd64_vmem_addr clobbers x14/x15 (index-register
    // shift path), which silently corrupted the staged store value for
    // indexed addressing (arrays) — caught by the fpforms O0/O2 cross-check.
    amd64_vmem_addr
    add  x14, _cpu, CPU_xmm
    add  x14, x14, x10, lsl 4
    ldr  w15, [x14]
    amd64_vwrite_prep 4, movss_store
    str  w15, [x9]
    amd64_vwrite_done 4, movss_store
    gret 3
    amd64_vwrite_bullshit 4, movss_store

# movsd/movss xmm, xmm (mod==3): copy the low lane, PRESERVE dst's rest.
.gadget amd64_v_movsd_reg
    ldr x8, [_ip]
    and x9, x8, 0xf
    ubfx x10, x8, 4, 4
    add x11, _cpu, CPU_xmm
    add x9, x11, x9, lsl 4
    add x10, x11, x10, lsl 4
    ldr x12, [x9]
    str x12, [x10]
    gret 1

.gadget amd64_v_movss_reg
    ldr x8, [_ip]
    and x9, x8, 0xf
    ubfx x10, x8, 4, 4
    add x11, _cpu, CPU_xmm
    add x9, x11, x9, lsl 4
    add x10, x11, x10, lsl 4
    ldr w12, [x9]
    str w12, [x10]
    gret 1

# unpcklpd/unpckhpd (66 0F 14/15) and unpcklps/unpckhps (0F 14/15):
# interleave dst/src lanes — exactly NEON zip1/zip2.
.macro amd64_v_unpck name, zipop, arr
    .gadget amd64_v_\name\()_reg
        ldr x8, [_ip]
        and x9, x8, 0xf
        ubfx x10, x8, 4, 4
        add x11, _cpu, CPU_xmm
        add x9, x11, x9, lsl 4
        add x10, x11, x10, lsl 4
        ldr q0, [x10]
        ldr q1, [x9]
        \zipop v0.\arr, v0.\arr, v1.\arr
        str q0, [x10]
        gret 1
    .gadget amd64_v_\name\()_mem
        ldr  x8, [_ip]
        ldr  x9, [_ip, 8]
        ubfx x10, x8, 8, 4
        amd64_vmem_addr
        amd64_vread_prep 16, \name\()_mem
        ldp  x12, x13, [x9]
        fmov d1, x12
        mov  v1.d[1], x13
        add  x14, _cpu, CPU_xmm
        add  x14, x14, x10, lsl 4
        ldr  q0, [x14]
        \zipop v0.\arr, v0.\arr, v1.\arr
        str  q0, [x14]
        gret 3
        amd64_vread_bullshit 16, \name\()_mem
.endm

amd64_v_unpck unpcklpd, zip1, 2d
amd64_v_unpck unpckhpd, zip2, 2d
amd64_v_unpck unpcklps, zip1, 4s
amd64_v_unpck unpckhps, zip2, 4s

# movhpd/movhps xmm, m64 (0F 16, mod!=3): HIGH lane = m64, low preserved.
.gadget amd64_v_movhp_load
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    ubfx x10, x8, 8, 4
    amd64_vmem_addr
    amd64_vread_prep 8, movhp_load
    ldr  x12, [x9]
    add  x14, _cpu, CPU_xmm
    add  x14, x14, x10, lsl 4
    str  x12, [x14, 8]
    gret 3
    amd64_vread_bullshit 8, movhp_load

# movhpd/movhps m64, xmm (0F 17): store the HIGH lane.
.gadget amd64_v_movhp_store
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    ubfx x10, x8, 8, 4
    // address FIRST: amd64_vmem_addr clobbers x14/x15 (index-register
    // shift path), which silently corrupted the staged store value for
    // indexed addressing (arrays) — caught by the fpforms O0/O2 cross-check.
    amd64_vmem_addr
    add  x14, _cpu, CPU_xmm
    add  x14, x14, x10, lsl 4
    ldr  x15, [x14, 8]
    amd64_vwrite_prep 8, movhp_store
    str  x15, [x9]
    amd64_vwrite_done 8, movhp_store
    gret 3
    amd64_vwrite_bullshit 8, movhp_store

# movlpd/movlps xmm, m64 (0F 12, mod!=3): LOW lane = m64, high preserved
# (unlike movsd-load, which zeroes). 0F 13 stores the low lane.
.gadget amd64_v_movlp_load
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    ubfx x10, x8, 8, 4
    amd64_vmem_addr
    amd64_vread_prep 8, movlp_load
    ldr  x12, [x9]
    add  x14, _cpu, CPU_xmm
    add  x14, x14, x10, lsl 4
    str  x12, [x14]
    gret 3
    amd64_vread_bullshit 8, movlp_load

.gadget amd64_v_movlp_store
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    ubfx x10, x8, 8, 4
    // address FIRST: amd64_vmem_addr clobbers x14/x15 (index-register
    // shift path), which silently corrupted the staged store value for
    // indexed addressing (arrays) — caught by the fpforms O0/O2 cross-check.
    amd64_vmem_addr
    add  x14, _cpu, CPU_xmm
    add  x14, x14, x10, lsl 4
    ldr  x15, [x14]
    amd64_vwrite_prep 8, movlp_store
    str  x15, [x9]
    amd64_vwrite_done 8, movlp_store
    gret 3
    amd64_vwrite_bullshit 8, movlp_store

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
    b jit_ret_chain
1:
    ldr _ip, [_ip, \off]
    b jit_ret_chain
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
# gadget tables; missing entries (parity, and-family jb/jae/jo, addr/gs)
# resolve to 0 and gen.c declines the fusion.
# (COND_LIST lives in control.S, a different translation unit -- an
# unexpanded name here would silently produce a 12-entry all-zero array
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
# chosen word into _ip and hands off to amd64_branch_dispatch exactly like
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
    b amd64_branch_dispatch
8101:
    ldr _ip, [_ip, \toff]
    b amd64_branch_dispatch
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
