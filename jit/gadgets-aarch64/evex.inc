# ===========================================================================
# EVEX (AVX-512) for both x86 guests (included at the end of math.S, after
# vex.inc, whose body macros these reuse; jit/gen.c gen_amd64_evex).
#
# Registers 0-31, 512 bits each, in four 128-bit quarters: quarter 0 is
# xmm[r] (r < 16) or xmm_ext[r - 16], quarter 1 ymm_hi[r], quarters 2 and 3
# zmm_hi[r]. A frame runs its body once per quarter of the vector length
# (VL 128/256/512: 1, 2 or 4), the body taking q0 = s1's quarter and q1 =
# s2's (a register, or EVEX_STAGE where a loader put the memory operand),
# w12 = imm8, and leaving its result in q0 or q2. The result is then masked
# by the opmask (merge, or zero with EVEX.z) at the element size, and the
# destination's quarters past VL are zeroed: every EVEX write is VLMAX-wide.
# Bodies keep x8 and x13 (the frame's word and quarter), as vex.inc's do.
#
# Word: [dst (0-4) | s1 (5-9) | s2 (10-14) | s2 staged (15) | VL: 0 128,
#        1 256, 2 512 (16-17) | opmask (18-20) | z (21) | log2 of the
#        masking element's bytes (22-23) | imm8 (24-31) | result to
#        EVEX_STAGE, unmasked, for a store (32) | s2 always its quarter 0
#        (a shift count, a broadcast) (33) | s2's quarter q & 1 (a 256-bit
#        broadcast) (34) | merge from s1, not the destination (VPBLENDM) (35)
#        | the destination's old quarter in q2 too (VPTERNLOG) (36) | a float
#        op: masked-off lanes of q0-q2 are 1.0 for the body, so they raise
#        nothing (37) | scalar: only element 0 is masked, the rest are the
#        body's (s1's) (38) | {er}: the rounding below for the op (48) | its
#        x86 RC (49-50) | {sae}: the op's flags (MXCSR, FPSR) discarded
#        (51)], gret 1.
# ===========================================================================

#define EVEX_STAGE VEX_STAGE                 /* (64 bytes there) */

# \out = &register \reg's quarter \q (w registers). Clobbers x15.
.macro evx_qaddr out, reg, q
    cmp  \q, 1
    b.hs .Leq1\@
    cmp  \reg, 16
    b.hs .Leqx\@
    mov  x15, CPU_xmm
    add  x15, x15, \reg, uxtw 4
    b    .Leqd\@
.Leqx\@:
    mov  x15, (CPU_xmm_ext - 256)
    add  x15, x15, \reg, uxtw 4
    b    .Leqd\@
.Leq1\@:
    b.hi .Leq2\@
    mov  x15, CPU_ymm_hi
    add  x15, x15, \reg, uxtw 4
    b    .Leqd\@
.Leq2\@:
    mov  x15, (CPU_zmm_hi - 32)
    add  x15, x15, \q, uxtw 4
    add  x15, x15, \reg, uxtw 4      /* (x32: the extend shifts at most 4) */
    add  x15, x15, \reg, uxtw 4
.Leqd\@:
    add  \out, _cpu, x15
.endm

# \vd: the lanes of quarter \q (w) the opmask enables, at the word's element
# size (all of them with no opmask). Clobbers x9, x11, x15-x17, v30, v31.
.macro evx_kmask vd, q
    ubfx w9, w8, 18, 3
    cbnz w9, .Lekm_k\@
    movi \vd\().16b, 0xff
    b    .Lekm_done\@
.Lekm_k\@:
    mov  x15, CPU_avx512_k
    add  x15, _cpu, x15
    ldr  x11, [x15, x9, lsl 3]
    ubfx w9, w8, 22, 2
    mov  w15, 16
    lsr  w15, w15, w9                    /* the quarter's elements */
    mul  w16, \q, w15
    lsr  x11, x11, x16
    adr  x17, evex_kbits
    add  x17, x17, x9, lsl 4
    ldr  q30, [x17]
    cbz  w9, .Lekm_b\@
    cmp  w9, 2
    b.lo .Lekm_h\@
    b.eq .Lekm_s\@
    dup  v31.2d, x11
    cmtst \vd\().2d, v31.2d, v30.2d
    b    .Lekm_done\@
.Lekm_s\@:
    dup  v31.4s, w11
    cmtst \vd\().4s, v31.4s, v30.4s
    b    .Lekm_done\@
.Lekm_h\@:
    dup  v31.8h, w11
    cmtst \vd\().8h, v31.8h, v30.8h
    b    .Lekm_done\@
.Lekm_b\@:
    dup  v31.8b, w11
    lsr  w11, w11, 8
    dup  v30.8b, w11
    ins  v31.d[1], v30.d[0]
    adr  x17, evex_kbits
    ldr  q30, [x17]
    cmtst \vd\().16b, v31.16b, v30.16b
.Lekm_done\@:
    tbz  x8, 38, .Lekm_end\@                /* scalar: element 0 only, the rest on */
    ubfx w9, w8, 22, 2
    movi v31.2d, 0xffffffffffffffff
    mov  x11, 0xffffffff00000000
    cmp  w9, 3
    csel x11, xzr, x11, eq               /* (a qword element: all of the low half) */
    mov  v31.d[0], x11
    orr  \vd\().16b, \vd\().16b, v31.16b
.Lekm_end\@:
.endm

# The body's result (v\res) to quarter w13 of the destination, masked; or,
# with word bit 32, to EVEX_STAGE unmasked. Clobbers x9-x11, x14-x17,
# v28-v31.
.macro evx_put res
    mov  v29.16b, v\res\().16b
    tbz  x8, 32, .Lepu_reg\@
    mov  x14, EVEX_STAGE
    add  x14, x14, x13, lsl 4
    str  q29, [_cpu, x14]
    b    .Lepu_done\@
.Lepu_reg\@:
    and  w10, w8, 31
    evx_qaddr x14, w10, w13
    ubfx w9, w8, 18, 3
    cbz  w9, .Lepu_st\@
    evx_kmask v28, w13
    tbnz x8, 21, .Lepu_z\@
    and  w10, w8, 31
    tbz  x8, 35, .Lepu_old\@
    ubfx w10, w8, 5, 5                   /* VPBLENDM: the old value is s1's */
.Lepu_old\@:
    evx_qaddr x14, w10, w13
    ldr  q30, [x14]
    and  w10, w8, 31
    evx_qaddr x14, w10, w13
    bif  v29.16b, v30.16b, v28.16b       /* merging: the old value where masked off */
    b    .Lepu_st\@
.Lepu_z\@:
    and  v29.16b, v29.16b, v28.16b
.Lepu_st\@:
    str  q29, [x14]
.Lepu_done\@:
.endm

# After the last quarter: the destination's quarters past VL zeroed (not for
# a store's EVEX_STAGE result). Clobbers x9, x10, x14, x15.
.macro evx_finish
    tbnz x8, 32, .Lefi_done\@
    ubfx w9, w8, 16, 2
    mov  w10, 1
    lsl  w10, w10, w9
.Lefi_loop\@:
    cmp  w10, 4
    b.hs .Lefi_done\@
    and  w9, w8, 31
    evx_qaddr x14, w9, w10
    stp  xzr, xzr, [x14]
    add  w10, w10, 1
    b    .Lefi_loop\@
.Lefi_done\@:
.endm

# {er} and {sae} around a float op: the host's rounding set from the word's
# RC, and the flags (FPSR, MXCSR) put back after, at EVEX_STAGE + 80..
.macro evx_fp_enter
    tbz  x8, 51, .Lefe_rc\@
    mrs  x9, fpsr
    mov  x10, (EVEX_STAGE + 80)
    str  x9, [_cpu, x10]
    ldr  w9, [_cpu, CPU_mxcsr]
    mov  x10, (EVEX_STAGE + 88)
    str  w9, [_cpu, x10]
.Lefe_rc\@:
    tbz  x8, 48, .Lefe_done\@
    mrs  x9, fpcr
    mov  x10, (EVEX_STAGE + 96)
    str  x9, [_cpu, x10]
    ubfx x11, x8, 49, 2                  /* x86 RC (N D U Z) to RMode (N P M Z) */
    lsl  w10, w11, 1
    orr  w11, w10, w11, lsr 1
    and  w11, w11, 3
    bfi  x9, x11, 22, 2
    msr  fpcr, x9
.Lefe_done\@:
.endm
.macro evx_fp_exit
    tbz  x8, 48, .Lefx_sae\@
    mov  x10, (EVEX_STAGE + 96)
    ldr  x9, [_cpu, x10]
    msr  fpcr, x9
.Lefx_sae\@:
    tbz  x8, 51, .Lefx_done\@
    mov  x10, (EVEX_STAGE + 80)
    ldr  x9, [_cpu, x10]
    msr  fpsr, x9
    mov  x10, (EVEX_STAGE + 88)
    ldr  w9, [_cpu, x10]
    str  w9, [_cpu, CPU_mxcsr]
.Lefx_done\@:
.endm
# A float op's masked-off lanes: 1.0 in q0, q1 (and q2 with bit 36) for the
# body. Clobbers x9, x11, x15-x17, v28-v31.
.macro evx_fp_lanes
    tbz  x8, 37, .Lefl_done\@
    ubfx w9, w8, 18, 3
    cbz  w9, .Lefl_done\@
    evx_kmask v28, w13
    ubfx w9, w8, 22, 2
    cmp  w9, 3
    b.eq .Lefl_d\@
    fmov v29.4s, 1.0
    b    .Lefl_set\@
.Lefl_d\@:
    fmov v29.2d, 1.0
.Lefl_set\@:
    bif  v0.16b, v29.16b, v28.16b
    bif  v1.16b, v29.16b, v28.16b
    tbz  x8, 36, .Lefl_done\@
    bif  v2.16b, v29.16b, v28.16b
.Lefl_done\@:
.endm

# dst = s1 OP s2, quarter by quarter (lane-local ops).
.macro evex_l3 name, res, body, args:vararg
.gadget evex_\name
    ldr  x8, [_ip]
    evx_fp_enter
    mov  w13, 0
.Lel3_loop\@:
    ubfx w9, w8, 5, 5
    evx_qaddr x10, w9, w13
    ldr  q0, [x10]
    mov  w11, w13                        /* s2's quarter */
    tbz  x8, 33, 1f
    mov  w11, 0
1:  tbz  x8, 34, 2f
    and  w11, w13, 1
2:  tbz  x8, 15, .Lel3_reg\@
    mov  x14, EVEX_STAGE
    add  x14, x14, x11, lsl 4
    add  x14, _cpu, x14
    b    .Lel3_s2\@
.Lel3_reg\@:
    ubfx w9, w8, 10, 5
    evx_qaddr x14, w9, w11
.Lel3_s2\@:
    ldr  q1, [x14]
    tbz  x8, 36, 3f
    and  w9, w8, 31
    evx_qaddr x14, w9, w13
    ldr  q2, [x14]
3:  evx_fp_lanes
    ubfx w12, w8, 24, 8
    \body \args
    evx_put \res
    add  w13, w13, 1
    ubfx w9, w8, 16, 2
    mov  w10, 1
    lsl  w10, w10, w9
    cmp  w13, w10
    b.lo .Lel3_loop\@
    evx_fp_exit
    evx_finish
    gret 1
.endm

.balign 16
evex_kbits:                              /* each lane's bit of the mask, by element size */
    .byte 1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128
    .hword 1, 2, 4, 8, 16, 32, 64, 128
    .word 1, 2, 4, 8
    .quad 1, 2
evex_elem_iota:                          /* each lane's element number, by element size */
    .byte 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
    .hword 0, 1, 2, 3, 4, 5, 6, 7
    .word 0, 1, 2, 3
    .quad 0, 1

# A 64-byte aligned operand (VMOVDQA32 zmm and its kind): i386 [orig_ip],
# gret 1 (vex.inc's vec_align16/32 for the others).
.gadget vec_align64
    tst _addr, 63
    b.ne 1f
    gret 1
1:  str xzr, [_cpu, CPU_segfault_addr]
    strb wzr, [_cpu, CPU_segfault_was_write]
    strb wzr, [_cpu, CPU_segfault_reported]
    ldr x8, [_ip]
    mov eip, w8
    mov x0, INT_GPF
    b jit_exit

# ---- staging: 64-byte loads, and the broadcasts (one element, replicated) ----
# i386: [orig_ip], gret 1; amd64: [meta][disp][next_ip], gret 3 (vex.inc's
# vex_ld_* for 16 and 32 bytes).
.macro evx_stage64 src
    ldp  q0, q1, [\src]
    ldp  q2, q3, [\src, 32]
    mov  x15, EVEX_STAGE
    add  x15, _cpu, x15
    stp  q0, q1, [x15]
    stp  q2, q3, [x15, 32]
.endm
.macro evx_stage_bcast bytes, src
.if \bytes == 4
    ld1r {v0.4s}, [\src]
.else
    ld1r {v0.2d}, [\src]
.endif
    mov  x15, EVEX_STAGE
    add  x15, _cpu, x15
    stp  q0, q0, [x15]
    stp  q0, q0, [x15, 32]
.endm
.gadget evex_ld_i386_64
    read_prep 512, evex_ld_i386_64
    evx_stage64 _xaddr
    gret 1
    read_bullshit 512, evex_ld_i386_64
.gadget evex_ld_amd64_64
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    amd64_vmem_addr
    amd64_vread_prep 64, evex_ld_amd64_64
    evx_stage64 x9
    gret 3
    amd64_vread_bullshit 64, evex_ld_amd64_64
.macro evex_ldb bytes
.gadget evex_ldb_i386_\bytes
    read_prep (\bytes * 8), evex_ldb_i386_\bytes
    evx_stage_bcast \bytes, _xaddr
    gret 1
    read_bullshit (\bytes * 8), evex_ldb_i386_\bytes
.gadget evex_ldb_amd64_\bytes
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    amd64_vmem_addr
    amd64_vread_prep \bytes, evex_ldb_amd64_\bytes
    evx_stage_bcast \bytes, x9
    gret 3
    amd64_vread_bullshit \bytes, evex_ldb_amd64_\bytes
.endm
evex_ldb 4
evex_ldb 8

# A store from EVEX_STAGE: 16, 32 or 64 bytes, unmasked. i386: [orig_ip],
# gret 1; amd64: [meta][disp][next_ip], gret 3.
.macro evex_st bytes
.gadget evex_st_i386_\bytes
    write_prep (\bytes * 8), evex_st_i386_\bytes
    mov  x15, EVEX_STAGE
    add  x15, _cpu, x15
    evx_copy \bytes, x15, _xaddr
    write_done (\bytes * 8), evex_st_i386_\bytes
    gret 1
    write_bullshit (\bytes * 8), evex_st_i386_\bytes
.gadget evex_st_amd64_\bytes
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    amd64_vmem_addr
    amd64_vwrite_prep \bytes, evex_st_amd64_\bytes
    mov  x15, EVEX_STAGE
    add  x15, _cpu, x15
    evx_copy \bytes, x15, x9
    amd64_vwrite_done \bytes, evex_st_amd64_\bytes
    gret 3
    amd64_vwrite_bullshit \bytes, evex_st_amd64_\bytes
.endm
.macro evx_copy bytes, from, to
.if \bytes == 64
    ldp  q0, q1, [\from]
    ldp  q2, q3, [\from, 32]
    stp  q0, q1, [\to]
    stp  q2, q3, [\to, 32]
.elseif \bytes == 32
    ldp  q0, q1, [\from]
    stp  q0, q1, [\to]
.else
    ldr  q0, [\from]
    str  q0, [\to]
.endif
.endm
evex_st 16
evex_st 32
evex_st 64

# ---- moves: dst = s2 (the loads and register forms; a store is the same
# frame into EVEX_STAGE, then evex_st or evex_stm) ----
.macro evb_mov
    mov  v0.16b, v1.16b
.endm
evex_l3 mov, 0, evb_mov

# ---- the integer ops: vex.inc's bodies, quarter by quarter ----
evex_l3 vi_paddb, 0, vi_body, bin, add, 16b, 1
evex_l3 vi_paddw, 0, vi_body, bin, add, 8h, 1
evex_l3 vi_paddd, 0, vi_body, bin, add, 4s, 1
evex_l3 vi_paddq, 0, vi_body, dq, add, 2d, 1
evex_l3 vi_psubb, 0, vi_body, bin, sub, 16b, 1
evex_l3 vi_psubw, 0, vi_body, bin, sub, 8h, 1
evex_l3 vi_psubd, 0, vi_body, bin, sub, 4s, 1
evex_l3 vi_psubq, 0, vi_body, dq, sub, 2d, 1
evex_l3 vi_paddsb, 0, vi_body, bin, sqadd, 16b, 1
evex_l3 vi_paddsw, 0, vi_body, bin, sqadd, 8h, 1
evex_l3 vi_paddusb, 0, vi_body, bin, uqadd, 16b, 1
evex_l3 vi_paddusw, 0, vi_body, bin, uqadd, 8h, 1
evex_l3 vi_psubsb, 0, vi_body, bin, sqsub, 16b, 1
evex_l3 vi_psubsw, 0, vi_body, bin, sqsub, 8h, 1
evex_l3 vi_psubusb, 0, vi_body, bin, uqsub, 16b, 1
evex_l3 vi_psubusw, 0, vi_body, bin, uqsub, 8h, 1
evex_l3 vi_pand, 0, vi_body, bin, and, 16b, 1
evex_l3 vi_pandn, 0, vi_body, binr, bic, 16b, 1
evex_l3 vi_por, 0, vi_body, bin, orr, 16b, 1
evex_l3 vi_pxor, 0, vi_body, bin, eor, 16b, 1
evex_l3 vi_pminub, 0, vi_body, bin, umin, 16b, 1
evex_l3 vi_pmaxub, 0, vi_body, bin, umax, 16b, 1
evex_l3 vi_pminsw, 0, vi_body, bin, smin, 8h, 1
evex_l3 vi_pmaxsw, 0, vi_body, bin, smax, 8h, 1
evex_l3 vi_pavgb, 0, vi_body, bin, urhadd, 16b, 1
evex_l3 vi_pavgw, 0, vi_body, bin, urhadd, 8h, 1
evex_l3 vi_pmullw, 0, vi_body, bin, mul, 8h, 1
evex_l3 vi_pmulhw, 0, vi_body, mulh, smull, -, 1
evex_l3 vi_pmulhuw, 0, vi_body, mulh, umull, -, 1
evex_l3 vi_pmuludq, 0, vi_body, muludq, -, -, 1
evex_l3 vi_pmaddwd, 0, vi_body, madd, -, -, 1
evex_l3 vi_psadbw, 0, vi_body, sad, -, -, 1
evex_l3 vi_punpcklbw, 0, vi_body, bin, zip1, 16b, 1
evex_l3 vi_punpcklwd, 0, vi_body, bin, zip1, 8h, 1
evex_l3 vi_punpckldq, 0, vi_body, bin, zip1, 4s, 1
evex_l3 vi_punpckhbw, 0, vi_body, bin, zip2, 16b, 1
evex_l3 vi_punpckhwd, 0, vi_body, bin, zip2, 8h, 1
evex_l3 vi_punpckhdq, 0, vi_body, bin, zip2, 4s, 1
evex_l3 vi_punpcklqdq, 0, vi_body, bin, zip1, 2d, 1
evex_l3 vi_punpckhqdq, 0, vi_body, bin, zip2, 2d, 1
evex_l3 vi_packsswb, 0, vi_body, packbw, sqxtn, -, 1
evex_l3 vi_packuswb, 0, vi_body, packbw, sqxtun, -, 1
evex_l3 vi_packssdw, 0, vi_body, packwd, sqxtn, -, 1
evex_l3 vi_packusdw, 0, vi_body, packwd, sqxtun, -, 1
evex_l3 vi_pminsb, 0, vi_body, bin, smin, 16b, 1
evex_l3 vi_pminsd, 0, vi_body, bin, smin, 4s, 1
evex_l3 vi_pminuw, 0, vi_body, bin, umin, 8h, 1
evex_l3 vi_pminud, 0, vi_body, bin, umin, 4s, 1
evex_l3 vi_pmaxsb, 0, vi_body, bin, smax, 16b, 1
evex_l3 vi_pmaxsd, 0, vi_body, bin, smax, 4s, 1
evex_l3 vi_pmaxuw, 0, vi_body, bin, umax, 8h, 1
evex_l3 vi_pmaxud, 0, vi_body, bin, umax, 4s, 1
evex_l3 vi_pmulld, 0, vi_body, bin, mul, 4s, 1
evex_l3 vi_pshufb, 0, vi38_body, pshufb, -, 16b, 1
evex_l3 vi_pmaddubsw, 0, vi38_body, maddubs, -, -, 1
evex_l3 vi_pmulhrsw, 0, vi38_body, mulhrs, -, -, 1
evex_l3 vi_pabsb, 0, vi38_body, abs, -, 16b, 1
evex_l3 vi_pabsw, 0, vi38_body, abs, -, 8h, 1
evex_l3 vi_pabsd, 0, vi38_body, abs, -, 4s, 1
evex_l3 vi_pmuldq, 0, vi38_body, muldq, -, -, 1

# ---- masked memory operands: only the elements the opmask enables are
# touched, so a masked-off element on an unmapped page does not fault ----
# The operand (16 << VL bytes at A) lies in at most two pages: [0, split) in
# A's page and [split, bytes) in the next. A page is probed (and, for a
# store, written) only if an enabled element has a byte in it; a store
# probes both before writing either, so a fault leaves memory as it was.
# x8: the word, with the opmask at bits 18-20 (a load's: the frame's) or
# 40-42 (a store's), the element size at 22-23 or 44-45.
#
# evm_split: w17 = split, x16 = the enabled elements, w15 = log2 element
# size, w14 = the operand's bytes; w9 = whether [0, split) is needed, w10
# whether [split, bytes) is. \a: the address (x); \ks: the opmask field's
# bit. Clobbers x11.
.macro evm_split a, ks
    and  x17, \a, 0xfff                  /* (first: \a may be x9) */
    mov  w11, 0x1000
    sub  w17, w11, w17
    ubfx x9, x8, \ks, 3
    mov  x11, CPU_avx512_k
    add  x11, _cpu, x11
    ldr  x16, [x11, x9, lsl 3]
    ubfx x15, x8, (\ks + 4), 2
    ubfx w9, w8, 16, 2
    mov  w14, 16
    lsl  w14, w14, w9
    lsr  w9, w14, w15                    /* the elements */
    cmp  w9, 64
    b.hs 1f
    mov  x11, 1
    lsl  x11, x11, x9
    sub  x11, x11, 1
    and  x16, x16, x11
1:  cmp  w17, w14
    csel w17, w17, w14, lo
    /* [0, split): the elements below ceil(split / size) */
    mov  w11, 1
    lsl  w11, w11, w15
    sub  w11, w11, 1
    add  w11, w17, w11
    lsr  w11, w11, w15
    mov  x9, x16
    cmp  w11, 64
    b.hs 2f
    mov  x10, 1
    lsl  x10, x10, x11
    sub  x10, x10, 1
    and  x9, x16, x10
2:  cmp  x9, 0
    cset w9, ne
    /* [split, bytes): the elements from floor(split / size) */
    mov  w10, 0
    cmp  w17, w14
    b.hs 3f
    lsr  w11, w17, w15
    lsr  x11, x16, x11
    cmp  x11, 0
    cset w10, ne
3:
.endm

# The copy: EVEX_STAGE <-> the two pages' host pointers \h1 (for [0,
# split), split in w17) and \h2, byte by byte: a store's only where the
# element is enabled (\k the enabled elements, \esz the log2 size), a load's
# the needed parts whole and the rest zeroed. Clobbers x11, x14-x16.
.macro evm_copy store, h1, h2, need1, need2, k, esz
    mov  x14, EVEX_STAGE
    add  x14, _cpu, x14
    mov  w11, 0
.Levc_loop\@:
    ubfx w15, w8, 16, 2
    mov  w16, 16
    lsl  w16, w16, w15
    cmp  w11, w16
    b.hs .Levc_done\@
    cmp  w11, w17
    b.hs .Levc_hi\@
    cbz  \need1, .Levc_skip\@
    add  x15, \h1, x11
    b    .Levc_elem\@
.Levc_hi\@:
    cbz  \need2, .Levc_skip\@
    sub  w15, w11, w17
    add  x15, \h2, x15
.Levc_elem\@:
.if \store
    lsr  w16, w11, \esz                  /* this byte's element enabled? */
    lsr  x16, \k, x16
    tbz  x16, 0, .Levc_next\@
    ldrb w16, [x14, x11]
    strb w16, [x15]
.else
    ldrb w16, [x15]
    strb w16, [x14, x11]
.endif
    b    .Levc_next\@
.Levc_skip\@:
.if !\store
    strb wzr, [x14, x11]
.endif
.Levc_next\@:
    add  w11, w11, 1
    b    .Levc_loop\@
.Levc_done\@:
.endm

# amd64: [meta][disp][next_ip][word], gret 4. A, the split, the needs and
# the host pointers live in x20-x25 across the TLB's slow path (gen.c
# flushed the amd64 register cache, so they are free).
.macro evex_mem_amd64 name, store, ks
.gadget evex_\name\()_amd64
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    amd64_vmem_addr
    mov  x24, x9                         /* A */
    ldr  x8, [_ip, 24]
    evm_split x9, \ks
    mov  w22, w17
    mov  w20, w9
    mov  w21, w10
    cbz  w20, 1f
    mov  x9, x24
.if \store
    amd64_vwrite_prep 1, evex_\name\()_amd64_p1
.else
    amd64_vread_prep 1, evex_\name\()_amd64_p1
.endif
    mov  x23, x9
1:  cbz  w21, 2f
    add  x9, x24, x22
.if \store
    amd64_vwrite_prep 1, evex_\name\()_amd64_p2
.else
    amd64_vread_prep 1, evex_\name\()_amd64_p2
.endif
    mov  x25, x9
2:  ldr  x8, [_ip, 24]
    mov  w17, w22
    ubfx x9, x8, \ks, 3                  /* the opmask and element size, for a store's copy */
    mov  x11, CPU_avx512_k
    add  x11, _cpu, x11
    ldr  x12, [x11, x9, lsl 3]
    ubfx x13, x8, (\ks + 4), 2
    evm_copy \store, x23, x25, w20, w21, x12, w13
    gret 4
.if \store
    amd64_vwrite_bullshit 1, evex_\name\()_amd64_p1
    amd64_vwrite_bullshit 1, evex_\name\()_amd64_p2
.else
    amd64_vread_bullshit 1, evex_\name\()_amd64_p1
    amd64_vread_bullshit 1, evex_\name\()_amd64_p2
.endif
.endm
evex_mem_amd64 ldm, 0, 18
evex_mem_amd64 stm, 1, 40

# i386: _addr = A (gen.c); [orig_ip][word], gret 2. A, the split and the
# needs are kept at EVEX_STAGE + 64 (the C slow path keeps x8-x13 only), the
# host pointers in x12/x13.
.macro evex_mem_i386 name, store, ks
.gadget evex_\name\()_i386
    ldr  x8, [_ip, 8]
    mov  x15, (EVEX_STAGE + 64)
    add  x15, _cpu, x15
    str  _addr, [x15]
    evm_split _xaddr, \ks
    mov  x15, (EVEX_STAGE + 64)
    add  x15, _cpu, x15
    str  w17, [x15, 4]
    str  w9, [x15, 8]
    str  w10, [x15, 12]
    cbz  w9, 1f
.if \store
    write_prep 8, evex_\name\()_i386_p1
.else
    read_prep 8, evex_\name\()_i386_p1
.endif
    mov  x12, _xaddr
1:  mov  x15, (EVEX_STAGE + 64)
    add  x15, _cpu, x15
    ldr  w10, [x15, 12]
    cbz  w10, 2f
    ldr  w9, [x15]
    ldr  w10, [x15, 4]
    add  _addr, w9, w10
.if \store
    write_prep 8, evex_\name\()_i386_p2
.else
    read_prep 8, evex_\name\()_i386_p2
.endif
    mov  x13, _xaddr
2:  ldr  x8, [_ip, 8]
    mov  x15, (EVEX_STAGE + 64)
    add  x15, _cpu, x15
    ldr  w17, [x15, 4]
    ldr  w9, [x15, 8]
    ldr  w10, [x15, 12]
    ubfx x16, x8, \ks, 3
    mov  x11, CPU_avx512_k
    add  x11, _cpu, x11
    ldr  x0, [x11, x16, lsl 3]
    ubfx x19, x8, (\ks + 4), 2
    evm_copy \store, x12, x13, w9, w10, x0, w19
    gret 2
.if \store
back_write_done_evex_\name\()_i386_p1 :
    b    back_evex_\name\()_i386_p1      /* (write_done's label: the probes write nothing) */
back_write_done_evex_\name\()_i386_p2 :
    b    back_evex_\name\()_i386_p2
    write_bullshit 8, evex_\name\()_i386_p1
    write_bullshit 8, evex_\name\()_i386_p2
.else
    read_bullshit 8, evex_\name\()_i386_p1
    read_bullshit 8, evex_\name\()_i386_p2
.endif
.endm
evex_mem_i386 ldm, 0, 18
evex_mem_i386 stm, 1, 40

# A masked broadcast: nothing read when the opmask enables no element.
# i386 [orig_ip][word], gret 2; amd64 [meta][disp][next_ip][word], gret 4.
.macro evex_ldbm bytes
.gadget evex_ldbm_i386_\bytes
    ldr  x8, [_ip, 8]
    evm_split _xaddr, 18
    cbz  x16, 1f
    read_prep (\bytes * 8), evex_ldbm_i386_\bytes
    evx_stage_bcast \bytes, _xaddr
1:  gret 2
    read_bullshit (\bytes * 8), evex_ldbm_i386_\bytes
.gadget evex_ldbm_amd64_\bytes
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    amd64_vmem_addr
    ldr  x8, [_ip, 24]
    mov  x24, x9
    evm_split x9, 18
    cbz  x16, 1f
    mov  x9, x24
    amd64_vread_prep \bytes, evex_ldbm_amd64_\bytes
    evx_stage_bcast \bytes, x9
1:  gret 4
    amd64_vread_bullshit \bytes, evex_ldbm_amd64_\bytes
.endm
evex_ldbm 4
evex_ldbm 8

# ---- results to an opmask: VPCMP*, VPTESTM/NM ----
# The body leaves each element of the quarter all ones (true) or zero; the
# frame packs those to bits, the quarters' bits into one value (in
# EVEX_STAGE + 72 across the bodies), ANDs it with the write mask (the word's
# opmask field; zeroing is the only kind) and writes k[dst & 7], every bit
# past VL's elements zero.
.macro evx_lanebits esz                  /* w9 = the bits of v0's elements (size \esz). Clobbers v1, v30 */
    adr  x9, evex_kbits
.if \esz == 0
    ldr  q30, [x9]
    and  v0.16b, v0.16b, v30.16b
    ext  v1.16b, v0.16b, v0.16b, 8
    zip1 v0.16b, v0.16b, v1.16b          /* lo, hi byte pairs: a halfword each */
    addv h0, v0.8h
.elseif \esz == 1
    ldr  q30, [x9, 16]
    and  v0.16b, v0.16b, v30.16b
    addv h0, v0.8h
.elseif \esz == 2
    ldr  q30, [x9, 32]
    and  v0.16b, v0.16b, v30.16b
    addv s0, v0.4s
.else
    ldr  q30, [x9, 48]
    and  v0.16b, v0.16b, v30.16b
    addp d0, v0.2d
.endif
    fmov w9, s0
.endm
.macro evex_tomask name, esz, body, args:vararg
.gadget evex_\name
    ldr  x8, [_ip]
    evx_fp_enter
    mov  x9, (EVEX_STAGE + 72)
    str  xzr, [_cpu, x9]
    mov  w13, 0
.Letm_loop\@:
    ubfx w9, w8, 5, 5
    evx_qaddr x10, w9, w13
    ldr  q0, [x10]
    tbz  x8, 15, .Letm_reg\@
    mov  x14, EVEX_STAGE
    add  x14, x14, x13, lsl 4
    add  x14, _cpu, x14
    b    .Letm_s2\@
.Letm_reg\@:
    ubfx w9, w8, 10, 5
    evx_qaddr x14, w9, w13
.Letm_s2\@:
    ldr  q1, [x14]
    evx_fp_lanes
    ubfx w12, w8, 24, 8
    \body \args
    evx_lanebits \esz
    mov  w10, (16 >> \esz)
    mul  w10, w10, w13
    lsl  x9, x9, x10
    mov  x10, (EVEX_STAGE + 72)
    ldr  x11, [_cpu, x10]
    orr  x11, x11, x9
    str  x11, [_cpu, x10]
    add  w13, w13, 1
    ubfx w9, w8, 16, 2
    mov  w10, 1
    lsl  w10, w10, w9
    cmp  w13, w10
    b.lo .Letm_loop\@
    evx_fp_exit
    mov  x10, (EVEX_STAGE + 72)
    ldr  x11, [_cpu, x10]
    mov  x14, CPU_avx512_k
    add  x14, _cpu, x14
    ubfx w9, w8, 18, 3
    cbz  w9, 1f
    ldr  x10, [x14, x9, lsl 3]
    and  x11, x11, x10
1:  and  w9, w8, 7
    str  x11, [x14, x9, lsl 3]
    gret 1
.endm

# The predicates of VPCMP/VPCMPU (imm8[2:0]): EQ LT LE FALSE NE NLT NLE TRUE,
# signed (cmgt/cmge) or unsigned (cmhi/cmhs). q0 = a, q1 = b.
.macro evb_pcmp arr, gt, ge
    and  w9, w12, 7
    adr  x10, .Lepc_tab\@
    ldr  w11, [x10, x9, lsl 2]
    add  x10, x10, w11, sxtw
    br   x10
.Lepc_tab\@:
    .word .Lepc_eq\@ - .Lepc_tab\@, .Lepc_lt\@ - .Lepc_tab\@, .Lepc_le\@ - .Lepc_tab\@
    .word .Lepc_f\@ - .Lepc_tab\@, .Lepc_ne\@ - .Lepc_tab\@, .Lepc_nlt\@ - .Lepc_tab\@
    .word .Lepc_nle\@ - .Lepc_tab\@, .Lepc_t\@ - .Lepc_tab\@
.Lepc_eq\@:
    cmeq v0.\arr, v0.\arr, v1.\arr
    b    .Lepc_done\@
.Lepc_lt\@:
    \gt  v0.\arr, v1.\arr, v0.\arr
    b    .Lepc_done\@
.Lepc_le\@:
    \ge  v0.\arr, v1.\arr, v0.\arr
    b    .Lepc_done\@
.Lepc_f\@:
    movi v0.16b, 0
    b    .Lepc_done\@
.Lepc_ne\@:
    cmeq v0.\arr, v0.\arr, v1.\arr
    mvn  v0.16b, v0.16b
    b    .Lepc_done\@
.Lepc_nlt\@:
    \ge  v0.\arr, v0.\arr, v1.\arr
    b    .Lepc_done\@
.Lepc_nle\@:
    \gt  v0.\arr, v0.\arr, v1.\arr
    b    .Lepc_done\@
.Lepc_t\@:
    movi v0.16b, 0xff
.Lepc_done\@:
.endm
.macro evb_cmp2 op, arr                  /* a OP b */
    \op  v0.\arr, v0.\arr, v1.\arr
.endm
.macro evb_testm arr, n
    cmtst v0.\arr, v0.\arr, v1.\arr
.if \n
    mvn  v0.16b, v0.16b
.endif
.endm
evex_tomask pcmpeqb, 0, evb_cmp2, cmeq, 16b
evex_tomask pcmpeqw, 1, evb_cmp2, cmeq, 8h
evex_tomask pcmpeqd, 2, evb_cmp2, cmeq, 4s
evex_tomask pcmpeqq, 3, evb_cmp2, cmeq, 2d
evex_tomask pcmpgtb, 0, evb_cmp2, cmgt, 16b
evex_tomask pcmpgtw, 1, evb_cmp2, cmgt, 8h
evex_tomask pcmpgtd, 2, evb_cmp2, cmgt, 4s
evex_tomask pcmpgtq, 3, evb_cmp2, cmgt, 2d
evex_tomask pcmpb, 0, evb_pcmp, 16b, cmgt, cmge
evex_tomask pcmpw, 1, evb_pcmp, 8h, cmgt, cmge
evex_tomask pcmpd, 2, evb_pcmp, 4s, cmgt, cmge
evex_tomask pcmpq, 3, evb_pcmp, 2d, cmgt, cmge
evex_tomask pcmpub, 0, evb_pcmp, 16b, cmhi, cmhs
evex_tomask pcmpuw, 1, evb_pcmp, 8h, cmhi, cmhs
evex_tomask pcmpud, 2, evb_pcmp, 4s, cmhi, cmhs
evex_tomask pcmpuq, 3, evb_pcmp, 2d, cmhi, cmhs
evex_tomask ptestmb, 0, evb_testm, 16b, 0
evex_tomask ptestmw, 1, evb_testm, 8h, 0
evex_tomask ptestmd, 2, evb_testm, 4s, 0
evex_tomask ptestmq, 3, evb_testm, 2d, 0
evex_tomask ptestnmb, 0, evb_testm, 16b, 1
evex_tomask ptestnmw, 1, evb_testm, 8h, 1
evex_tomask ptestnmd, 2, evb_testm, 4s, 1
evex_tomask ptestnmq, 3, evb_testm, 2d, 1

# ---- shifts: by imm8 (dst = vvvv, the source the r/m, as s2), by an xmm's
# low quadword (word bit 33: s2's quarter 0), by each lane's own count ----
.macro evb_shi kind, L, arr
    mov  v0.16b, v1.16b
    mov  x10, x12
    vsh_body \kind, \L, \arr
.endm
.macro evb_shx kind, L, arr
    fmov x10, d1
    vsh_body \kind, \L, \arr
.endm
.macro evb_shv kind, L, arr              /* a count past the width: 0, or the sign */
.if \L == 64
    mov  x9, (\L - 1)
    dup  v3.2d, x9
    cmhi v4.2d, v1.2d, v3.2d             /* count > 63 */
  .ifc \kind,sra
    bif  v3.16b, v1.16b, v4.16b          /* min(count, 63) */
    neg  v2.2d, v3.2d
    sshl v0.2d, v0.2d, v2.2d
  .else
    .ifc \kind,srl
    neg  v2.2d, v1.2d
    .else
    mov  v2.16b, v1.16b
    .endif
    ushl v0.2d, v0.2d, v2.2d
    bic  v0.16b, v0.16b, v4.16b
  .endif
.else
  .ifc \kind,sra
    movi v3.\arr, (\L - 1)
  .else
    movi v3.\arr, \L
  .endif
    umin v2.\arr, v1.\arr, v3.\arr
  .ifnc \kind,sll
    neg  v2.\arr, v2.\arr
  .endif
  .ifc \kind,sra
    sshl v0.\arr, v0.\arr, v2.\arr
  .else
    ushl v0.\arr, v0.\arr, v2.\arr
  .endif
.endif
.endm
# Rotates: by imm8 (as s2's source, like the shifts) or each lane's count,
# modulo the width: x << c | x >> (L - c), NEON's ushl giving 0 for a shift
# of the whole width.
.macro evb_rot left, imm, L, arr
.if \imm
    mov  v0.16b, v1.16b
    and  w9, w12, (\L - 1)
  .if \L == 64
    dup  v2.2d, x9
  .else
    dup  v2.\arr, w9
  .endif
.else
  .if \L == 64
    mov  x9, (\L - 1)
    dup  v3.2d, x9
  .else
    movi v3.\arr, (\L - 1)
  .endif
    and  v2.16b, v1.16b, v3.16b
.endif
.if !\left
    neg  v2.\arr, v2.\arr                /* right by c: left by -c and by L - c */
.endif
.if \L == 64
    mov  x9, \L
    dup  v3.2d, x9
.else
    movi v3.\arr, \L
.endif
    ushl v4.\arr, v0.\arr, v2.\arr
.if \left
    sub  v2.\arr, v2.\arr, v3.\arr       /* c - L: right by L - c */
.else
    add  v2.\arr, v2.\arr, v3.\arr       /* L - c */
.endif
    ushl v0.\arr, v0.\arr, v2.\arr
    orr  v0.16b, v0.16b, v4.16b
.endm
.irp k, srl,sra,sll
evex_l3 shi_\k\()w, 0, evb_shi, \k, 16, 8h
evex_l3 shi_\k\()d, 0, evb_shi, \k, 32, 4s
evex_l3 shi_\k\()q, 0, evb_shi, \k, 64, 2d
evex_l3 shx_\k\()w, 0, evb_shx, \k, 16, 8h
evex_l3 shx_\k\()d, 0, evb_shx, \k, 32, 4s
evex_l3 shx_\k\()q, 0, evb_shx, \k, 64, 2d
evex_l3 shv_\k\()w, 0, evb_shv, \k, 16, 8h
evex_l3 shv_\k\()d, 0, evb_shv, \k, 32, 4s
evex_l3 shv_\k\()q, 0, evb_shv, \k, 64, 2d
.endr
evex_l3 roli_d, 0, evb_rot, 1, 1, 32, 4s
evex_l3 roli_q, 0, evb_rot, 1, 1, 64, 2d
evex_l3 rori_d, 0, evb_rot, 0, 1, 32, 4s
evex_l3 rori_q, 0, evb_rot, 0, 1, 64, 2d
evex_l3 rolv_d, 0, evb_rot, 1, 0, 32, 4s
evex_l3 rolv_q, 0, evb_rot, 1, 0, 64, 2d
evex_l3 rorv_d, 0, evb_rot, 0, 0, 32, 4s
evex_l3 rorv_q, 0, evb_rot, 0, 0, 64, 2d
# VPSRLDQ/VPSLLDQ by imm8, each 128-bit lane on its own (vex.inc's body)
.macro evb_dq right                     /* (vex.inc's vex_dq_body, the count from w12) */
    mov  v0.16b, v1.16b
    cmp  w12, 15
    b.ls .Ledq_in\@
    movi v0.2d, 0
    b    .Ledq_out\@
.Ledq_in\@:
    adr  x11, vex_iota16
    ldr  q2, [x11]
    dup  v3.16b, w12
.if \right
    add  v2.16b, v2.16b, v3.16b
.else
    sub  v2.16b, v2.16b, v3.16b
.endif
    tbl  v0.16b, {v0.16b}, v2.16b
.Ledq_out\@:
.endm
evex_l3 psrldq, 0, evb_dq, 1
evex_l3 pslldq, 0, evb_dq, 0

# ---- the qword ops AVX-512 adds: VPMINSQ/UQ, VPMAXSQ/UQ, VPABSQ, VPMULLQ ----
.macro evb_minmaxq cmp, max
    \cmp v2.2d, v0.2d, v1.2d             /* a > b */
.if \max
    bif  v0.16b, v1.16b, v2.16b
.else
    bit  v0.16b, v1.16b, v2.16b
.endif
.endm
.macro evb_absq                         /* (unary: the source is s2, as vex.inc's abs) */
    abs  v0.2d, v1.2d
.endm
.macro evb_mullq
    fmov x9, d0
    fmov x10, d1
    mul  x9, x9, x10
    mov  x10, v0.d[1]
    mov  x11, v1.d[1]
    mul  x10, x10, x11
    fmov d0, x9
    mov  v0.d[1], x10
.endm
evex_l3 pminsq, 0, evb_minmaxq, cmgt, 0
evex_l3 pmaxsq, 0, evb_minmaxq, cmgt, 1
evex_l3 pminuq, 0, evb_minmaxq, cmhi, 0
evex_l3 pmaxuq, 0, evb_minmaxq, cmhi, 1
evex_l3 pabsq, 0, evb_absq
evex_l3 pmullq, 0, evb_mullq

# ---- VPTERNLOGD/Q: each bit from imm8's truth table, index (dst << 2) |
# (s1 << 1) | s2 (word bit 36 puts dst's old quarter in q2): Shannon on dst,
# then s1, then s2 ----
.macro evb_tlog_bit v, n                 /* \v: all ones if imm8 bit \n, else 0 */
    ubfx w9, w12, \n, 1
    neg  w9, w9
    dup  \v\().4s, w9
.endm
.macro evb_ternlog
    evb_tlog_bit v20, 0
    evb_tlog_bit v21, 1
    evb_tlog_bit v22, 2
    evb_tlog_bit v23, 3
    evb_tlog_bit v24, 4
    evb_tlog_bit v25, 5
    evb_tlog_bit v26, 6
    evb_tlog_bit v27, 7
    bif  v21.16b, v20.16b, v1.16b        /* s2 ? b1 : b0 */
    bif  v23.16b, v22.16b, v1.16b        /* s2 ? b3 : b2 */
    bif  v25.16b, v24.16b, v1.16b
    bif  v27.16b, v26.16b, v1.16b
    bif  v23.16b, v21.16b, v0.16b        /* s1 ? (b3|b2) : (b1|b0) */
    bif  v27.16b, v25.16b, v0.16b
    bif  v27.16b, v23.16b, v2.16b        /* dst ? (the upper four) : (the lower) */
    mov  v0.16b, v27.16b
.endm
evex_l3 pternlog, 0, evb_ternlog

# ---- VPBLENDMB/W/D/Q, VBLENDMPS/PD: s2 where the mask is set, s1 (word bit
# 35) or zero where not ----
evex_l3 blendm, 0, evb_mov

# ---- broadcasts: element 0 of s2's quarter 0 (word bit 33) over every
# element; the 128-bit ones are evb_mov with bit 33, the 256-bit ones with
# bit 34 ----
.macro evb_bcast arr, el
    dup  v0.\arr, v1.\el[0]
.endm
evex_l3 bcast_b, 0, evb_bcast, 16b, b
evex_l3 bcast_w, 0, evb_bcast, 8h, h
evex_l3 bcast_d, 0, evb_bcast, 4s, s
evex_l3 bcast_q, 0, evb_bcast, 2d, d

# ---- floating point: math.S's xf bodies (x86's NaNs, DAZ/FTZ/DE, the
# tininess fix), quarter by quarter; scalar forms compute element 0 and keep
# s1's others (word bit 38 masks only element 0) ----
.irp op, add, sub, mul
evex_l3 xf_\op\()ps, 2, xf_body_arith, f\op, s, 4s, 1, 0
evex_l3 xf_\op\()pd, 2, xf_body_arith, f\op, d, 2d, 1, 0
evex_l3 xf_\op\()ss, 2, xf_body_arith, f\op, s, 4s, 0, 0
evex_l3 xf_\op\()sd, 2, xf_body_arith, f\op, d, 2d, 0, 0
.endr
evex_l3 xf_divps, 2, xf_body_arith, fdiv, s, 4s, 1, 1
evex_l3 xf_divpd, 2, xf_body_arith, fdiv, d, 2d, 1, 1
evex_l3 xf_divss, 2, xf_body_arith, fdiv, s, 4s, 0, 1
evex_l3 xf_divsd, 2, xf_body_arith, fdiv, d, 2d, 0, 1
evex_l3 xf_minps, 2, xf_body_minmax, 0, s, 4s, 1
evex_l3 xf_minpd, 2, xf_body_minmax, 0, d, 2d, 1
evex_l3 xf_minss, 2, xf_body_minmax, 0, s, 4s, 0
evex_l3 xf_minsd, 2, xf_body_minmax, 0, d, 2d, 0
evex_l3 xf_maxps, 2, xf_body_minmax, 1, s, 4s, 1
evex_l3 xf_maxpd, 2, xf_body_minmax, 1, d, 2d, 1
evex_l3 xf_maxss, 2, xf_body_minmax, 1, s, 4s, 0
evex_l3 xf_maxsd, 2, xf_body_minmax, 1, d, 2d, 0
evex_l3 xf_sqrtps, 2, xf_body_sqrt, s, 4s, 1
evex_l3 xf_sqrtpd, 2, xf_body_sqrt, d, 2d, 1
evex_l3 xf_sqrtss, 2, xf_body_sqrt, s, 4s, 0
evex_l3 xf_sqrtsd, 2, xf_body_sqrt, d, 2d, 0
# VCMPPS/PD/SS/SD into an opmask: the 32 predicates (vex.inc's body); a
# scalar compare is element 0 only
.macro evb_fcmp t, a, p
    /* SDE (every model) raises no DE for the constant predicates in EVEX
       form -- FALSE_OQ, TRUE_UQ, FALSE_OS, TRUE_US: imm8 & 0x0b == 0x0b --
       though it does in VEX form, as an AMD Ryzen does; IE for a NaN under
       the signalling two stays. So DE is put back as it was for those. */
    ldr  w9, [_cpu, CPU_mxcsr]
    mov  x10, (EVEX_STAGE + 104)
    str  w9, [_cpu, x10]
    xf_body_cmp32 \t, \a, \p
    mov  w9, 0x0b
    and  w9, w12, w9
    cmp  w9, 0x0b
    b.ne .Lefc_de\@
    mov  x10, (EVEX_STAGE + 104)
    ldr  w9, [_cpu, x10]
    ldr  w10, [_cpu, CPU_mxcsr]
    and  w9, w9, 2                       /* DE (bit 1) as before the op */
    bic  w10, w10, 2
    orr  w10, w10, w9
    str  w10, [_cpu, CPU_mxcsr]
.Lefc_de\@:
    mov  v0.16b, v2.16b
.if !\p
  .ifc \t,s
    movi v1.2d, 0
    mov  v1.s[0], v0.s[0]
  .else
    movi v1.2d, 0
    mov  v1.d[0], v0.d[0]
  .endif
    mov  v0.16b, v1.16b
.endif
.endm
evex_tomask xf_cmpps, 2, evb_fcmp, s, 4s, 1
evex_tomask xf_cmppd, 3, evb_fcmp, d, 2d, 1
evex_tomask xf_cmpss, 2, evb_fcmp, s, 4s, 0
evex_tomask xf_cmpsd, 3, evb_fcmp, d, 2d, 0

# ===========================================================================
# Lane-crossing ops: the whole vector at once. s1 in v16-v19, s2 (or
# EVEX_STAGE) in v20-v23, the destination's old value in v24-v27, a quarter
# past VL zero; the body leaves the result's quarters in v0-v3 (it may use
# v4-v7 and x9-x12, x14-x17, and reads w12 = imm8); each is then masked as
# evx_put does and the destination zeroed past VL. NEON's tbl takes four
# consecutive registers as one 64-byte table, so a whole zmm permutes in one.
# Word: as the frames' above.
# ===========================================================================
.macro evf_ld n, field, q                /* v\n = quarter \q of the register in the word's \field (or zero past VL) */
    ubfx w9, w8, 16, 2
    mov  w10, 1
    lsl  w10, w10, w9
    cmp  w10, \q
    b.ls .Lefl_zero\@
    mov  w13, \q
.ifc \field,stage
    tbz  x8, 15, .Lefl_reg\@
    mov  x14, (EVEX_STAGE + 16 * \q)
    ldr  q\n, [_cpu, x14]
    b    .Lefl_done\@
.Lefl_reg\@:
    ubfx w9, w8, 10, 5
.else
    ubfx w9, w8, \field, 5
.endif
    evx_qaddr x14, w9, w13
    ldr  q\n, [x14]
    b    .Lefl_done\@
.Lefl_zero\@:
    movi v\n\().16b, 0
.Lefl_done\@:
.endm
.macro evf_put1 vres, vold, q            /* quarter \q of the result (\vres), masked against \vold */
    ubfx w9, w8, 16, 2
    mov  w10, 1
    lsl  w10, w10, w9
    cmp  w10, \q
    b.ls .Lefp_done\@
    mov  w13, \q
    mov  v29.16b, \vres\().16b
    tbz  x8, 32, .Lefp_reg\@
    mov  x14, (EVEX_STAGE + 16 * \q)
    str  q29, [_cpu, x14]
    b    .Lefp_done\@
.Lefp_reg\@:
    ubfx w9, w8, 18, 3
    cbz  w9, .Lefp_st\@
    evx_kmask v28, w13
    tbnz x8, 21, .Lefp_z\@
    bif  v29.16b, \vold\().16b, v28.16b
    b    .Lefp_st\@
.Lefp_z\@:
    and  v29.16b, v29.16b, v28.16b
.Lefp_st\@:
    and  w9, w8, 31
    evx_qaddr x14, w9, w13
    str  q29, [x14]
.Lefp_done\@:
.endm
.macro evex_full name, body, args:vararg
.gadget evex_\name
    ldr  x8, [_ip]
    evf_ld 16, 5, 0
    evf_ld 17, 5, 1
    evf_ld 18, 5, 2
    evf_ld 19, 5, 3
    evf_ld 20, stage, 0
    evf_ld 21, stage, 1
    evf_ld 22, stage, 2
    evf_ld 23, stage, 3
    evf_ld 24, 0, 0
    evf_ld 25, 0, 1
    evf_ld 26, 0, 2
    evf_ld 27, 0, 3
    ubfx w12, w8, 24, 8
    \body \args
    evf_put1 v0, v24, 0
    evf_put1 v1, v25, 1
    evf_put1 v2, v26, 2
    evf_put1 v3, v27, 3
    evx_finish
    gret 1
.endm

# Element indices to byte indices for tbl: \vd = (\vs & \vm) << esz, each
# element's bytes counting up (\vm: the index mask, dup'd at the size).
.macro evf_bidx vd, vs, vm, esz
    and  \vd\().16b, \vs\().16b, \vm\().16b
.if \esz == 1
    shl  \vd\().8h, \vd\().8h, 1
    mov  w11, 0x0101
    dup  v6.8h, w11
    mul  \vd\().8h, \vd\().8h, v6.8h
    mov  w11, 0x0100
    dup  v6.8h, w11
    add  \vd\().8h, \vd\().8h, v6.8h
.elseif \esz == 2
    shl  \vd\().4s, \vd\().4s, 2
    mov  w11, 0x01010101
    dup  v6.4s, w11
    mul  \vd\().4s, \vd\().4s, v6.4s
    movz w11, 0x0100
    movk w11, 0x0302, lsl 16
    dup  v6.4s, w11
    add  \vd\().4s, \vd\().4s, v6.4s
.elseif \esz == 3
    shl  \vd\().2d, \vd\().2d, 3
    trn1 \vd\().4s, \vd\().4s, \vd\().4s     /* the low word into both halves */
    mov  w11, 0x01010101
    dup  v6.4s, w11
    mul  \vd\().4s, \vd\().4s, v6.4s
    movz x11, 0x0100
    movk x11, 0x0302, lsl 16
    movk x11, 0x0504, lsl 32
    movk x11, 0x0706, lsl 48
    dup  v6.2d, x11
    add  \vd\().16b, \vd\().16b, v6.16b
.endif
.endm
.macro evf_dupmask vm, esz, n            /* \vm = register \n's value in every element */
.if \esz == 0
    dup  \vm\().16b, w\n
.elseif \esz == 1
    dup  \vm\().8h, w\n
.elseif \esz == 2
    dup  \vm\().4s, w\n
.else
    dup  \vm\().2d, x\n
.endif
.endm
# w9 = the elements in the vector: (16 << VL) >> esz
.macro evf_nelem esz
    ubfx w9, w8, 16, 2
    mov  w10, (16 >> \esz)
    lsl  w9, w10, w9
.endm

# VPERMB/W/D/Q, VPERMPS/PD: dst[i] = s2[s1[i] mod n] (the table is s2)
.macro evfb_perm1q vd, vidx, esz         /* one quarter (v5: the index mask) */
    evf_bidx v4, \vidx, v5, \esz
    tbl  \vd\().16b, {v20.16b, v21.16b, v22.16b, v23.16b}, v4.16b
.endm
.macro evfb_perm1 esz
    evf_nelem \esz
    sub  w9, w9, 1
    evf_dupmask v5, \esz, 9
    evfb_perm1q v0, v16, \esz
    evfb_perm1q v1, v17, \esz
    evfb_perm1q v2, v18, \esz
    evfb_perm1q v3, v19, \esz
.endm
evex_full permb, evfb_perm1, 0
evex_full permw, evfb_perm1, 1
evex_full permd, evfb_perm1, 2
evex_full permq, evfb_perm1, 3
# VPERMQ/PD imm8: each 256-bit half's four qwords picked by imm8's 2-bit
# fields; qword j of the result is qword ((j & 4) | field(j & 3)) of s2.
.macro evfb_permqi_qw j                  /* v4.d[j & 1]: qword j's byte indices */
    ubfx w9, w12, (2 * (\j & 3)), 2
    add  w9, w9, (\j & 4)
    lsl  w9, w9, 3
    mov  w11, 0x01010101
    mul  w9, w9, w11
    orr  x9, x9, x9, lsl 32
    movz x11, 0x0100
    movk x11, 0x0302, lsl 16
    movk x11, 0x0504, lsl 32
    movk x11, 0x0706, lsl 48
    add  x9, x9, x11
    mov  v4.d[\j & 1], x9
.endm
.macro evfb_permqi
    evfb_permqi_qw 0
    evfb_permqi_qw 1
    tbl  v0.16b, {v20.16b, v21.16b, v22.16b, v23.16b}, v4.16b
    evfb_permqi_qw 2
    evfb_permqi_qw 3
    tbl  v1.16b, {v20.16b, v21.16b, v22.16b, v23.16b}, v4.16b
    evfb_permqi_qw 4
    evfb_permqi_qw 5
    tbl  v2.16b, {v20.16b, v21.16b, v22.16b, v23.16b}, v4.16b
    evfb_permqi_qw 6
    evfb_permqi_qw 7
    tbl  v3.16b, {v20.16b, v21.16b, v22.16b, v23.16b}, v4.16b
.endm
evex_full permqi, evfb_permqi

# VPERMI2*/VPERMT2*: two n-element tables, the index's bit log2(n) picking
# one. I2: index = the destination, tables s1 (low) and s2 (high); T2:
# index = s1, tables the destination (low) and s2 (high). v5: the index
# mask (n - 1), v6/v7 scratch (evf_bidx), the select bit n in x/w15.
.macro evfb_perm2_one vd, vidx, i2, esz  /* one quarter */
    evf_bidx v4, \vidx, v5, \esz
.if \i2
    tbl  \vd\().16b, {v16.16b, v17.16b, v18.16b, v19.16b}, v4.16b
.else
    tbl  \vd\().16b, {v24.16b, v25.16b, v26.16b, v27.16b}, v4.16b
.endif
    tbl  v6.16b, {v20.16b, v21.16b, v22.16b, v23.16b}, v4.16b
    evf_dupmask v7, \esz, 15
.if \esz == 0
    cmtst v7.16b, \vidx\().16b, v7.16b
.elseif \esz == 1
    cmtst v7.8h, \vidx\().8h, v7.8h
.elseif \esz == 2
    cmtst v7.4s, \vidx\().4s, v7.4s
.else
    cmtst v7.2d, \vidx\().2d, v7.2d
.endif
    bit  \vd\().16b, v6.16b, v7.16b
.endm
.macro evfb_perm2 i2, esz
    evf_nelem \esz
    mov  w15, w9
    sub  w9, w9, 1
    evf_dupmask v5, \esz, 9
.if \i2
    evfb_perm2_one v0, v24, 1, \esz
    evfb_perm2_one v1, v25, 1, \esz
    evfb_perm2_one v2, v26, 1, \esz
    evfb_perm2_one v3, v27, 1, \esz
.else
    evfb_perm2_one v0, v16, 0, \esz
    evfb_perm2_one v1, v17, 0, \esz
    evfb_perm2_one v2, v18, 0, \esz
    evfb_perm2_one v3, v19, 0, \esz
.endif
.endm
evex_full permi2b, evfb_perm2, 1, 0
evex_full permi2w, evfb_perm2, 1, 1
evex_full permi2d, evfb_perm2, 1, 2
evex_full permi2q, evfb_perm2, 1, 3
evex_full permt2b, evfb_perm2, 0, 0
evex_full permt2w, evfb_perm2, 0, 1
evex_full permt2d, evfb_perm2, 0, 2
evex_full permt2q, evfb_perm2, 0, 3

# VALIGND/Q: (s1:s2 as one 2n-element vector, s1 high) >> imm8 mod n
# elements: element i = i + count, from s2 below n and s1 above.
.macro evfb_align_one vd, q, esz         /* v5: n - 1, w15: count, w14: n */
    adr  x9, evex_elem_iota
    ldr  q4, [x9, (\esz * 16)]           /* each lane's element number in a quarter */
    mov  w9, ((16 >> \esz) * \q)
    add  w9, w9, w15
    evf_dupmask v6, \esz, 9
.if \esz == 2
    add  v4.4s, v4.4s, v6.4s             /* i + count */
.else
    add  v4.2d, v4.2d, v6.2d
.endif
    mov  v7.16b, v4.16b                  /* (kept: the select is bit log2(n)) */
    evf_bidx v4, v7, v5, \esz
    tbl  \vd\().16b, {v20.16b, v21.16b, v22.16b, v23.16b}, v4.16b
    tbl  v6.16b, {v16.16b, v17.16b, v18.16b, v19.16b}, v4.16b
    evf_dupmask v4, \esz, 14
.if \esz == 2
    cmtst v4.4s, v7.4s, v4.4s
.else
    cmtst v4.2d, v7.2d, v4.2d
.endif
    bit  \vd\().16b, v6.16b, v4.16b
.endm
.macro evfb_align esz
    evf_nelem \esz
    mov  w14, w9
    sub  w9, w9, 1
    and  w15, w12, w9                    /* the count: imm8 mod n */
    evf_dupmask v5, \esz, 9
    evfb_align_one v0, 0, \esz
    evfb_align_one v1, 1, \esz
    evfb_align_one v2, 2, \esz
    evfb_align_one v3, 3, \esz
.endm
evex_full alignd, evfb_align, 2
evex_full alignq, evfb_align, 3

# VSHUFI32x4/64x2, VSHUFF32x4/64x2: the result's 128-bit lanes from s1's
# (the lower half of them) and s2's (the upper), imm8 picking: 512 bits,
# 2-bit fields; 256, a bit each.
.macro evfb_lane_pick vd, tab, lane_w    /* \vd = 128-bit lane \lane_w of the table (v16 or v20 group) */
    adr  x9, vex_iota16
    ldr  q4, [x9]
    lsl  w9, \lane_w, 4
    dup  v6.16b, w9
    add  v4.16b, v4.16b, v6.16b
.ifc \tab,s1
    tbl  \vd\().16b, {v16.16b, v17.16b, v18.16b, v19.16b}, v4.16b
.else
    tbl  \vd\().16b, {v20.16b, v21.16b, v22.16b, v23.16b}, v4.16b
.endif
.endm
.macro evfb_shuf4
    ubfx w9, w8, 16, 2
    cmp  w9, 2
    b.ne .Lefs_256\@
    ubfx w10, w12, 0, 2
    evfb_lane_pick v0, s1, w10
    ubfx w10, w12, 2, 2
    evfb_lane_pick v1, s1, w10
    ubfx w10, w12, 4, 2
    evfb_lane_pick v2, s2, w10
    ubfx w10, w12, 6, 2
    evfb_lane_pick v3, s2, w10
    b    .Lefs_done\@
.Lefs_256\@:
    ubfx w10, w12, 0, 1
    evfb_lane_pick v0, s1, w10
    ubfx w10, w12, 1, 1
    evfb_lane_pick v1, s2, w10
.Lefs_done\@:
.endm
evex_full shuf4, evfb_shuf4

# VINSERTI/F32x4/64x2 (word bit 39 clear) and 32x8/64x4 (set): s1 with the
# 128-bit lane (or 256-bit half) imm8 picks replaced by s2's low one(s).
.macro evfb_insert
    mov  v0.16b, v16.16b
    mov  v1.16b, v17.16b
    mov  v2.16b, v18.16b
    mov  v3.16b, v19.16b
    tbnz x8, 39, .Lefi_8\@
    ubfx w9, w8, 16, 2                   /* the lanes: 2 or 4 */
    mov  w10, 1
    lsl  w10, w10, w9
    sub  w10, w10, 1
    and  w10, w12, w10
    cmp  w10, 1
    b.lo .Lefi_l0\@
    b.eq .Lefi_l1\@
    cmp  w10, 3
    b.lo .Lefi_l2\@
    mov  v3.16b, v20.16b
    b    .Lefi_done\@
.Lefi_l2\@:
    mov  v2.16b, v20.16b
    b    .Lefi_done\@
.Lefi_l1\@:
    mov  v1.16b, v20.16b
    b    .Lefi_done\@
.Lefi_l0\@:
    mov  v0.16b, v20.16b
    b    .Lefi_done\@
.Lefi_8\@:
    tbnz w12, 0, .Lefi_hi\@
    mov  v0.16b, v20.16b
    mov  v1.16b, v21.16b
    b    .Lefi_done\@
.Lefi_hi\@:
    mov  v2.16b, v20.16b
    mov  v3.16b, v21.16b
.Lefi_done\@:
.endm
evex_full insert, evfb_insert
# VEXTRACTI/F32x4/64x2 (bit 39 clear) and 32x8/64x4 (set): the lane (half)
# imm8 picks of s1, by the source's VL in word bits 52-53 (the word's VL is
# the result's: 128 or 256 bits; bits 40-45 are a masked store's).
.macro evfb_extract                     /* (the frame loaded s1 only to the result's VL: read the source itself) */
    ubfx x9, x8, 52, 2                   /* the source's VL */
    ubfx w11, w8, 5, 5                   /* the source register */
    tbnz x8, 39, .Lefe_8\@
    mov  w10, 1
    lsl  w10, w10, w9
    sub  w10, w10, 1
    and  w10, w12, w10
    evx_qaddr x14, w11, w10
    ldr  q0, [x14]
    b    .Lefe_done\@
.Lefe_8\@:
    and  w10, w12, 1
    lsl  w10, w10, 1
    evx_qaddr x14, w11, w10
    ldr  q0, [x14]
    add  w10, w10, 1
    evx_qaddr x14, w11, w10
    ldr  q1, [x14]
.Lefe_done\@:
.endm
evex_full extract, evfb_extract

# ---- the in-lane shuffles with an imm8: tbl indices built from it (VEX's
# take theirs from the code stream; these from w12). q0 = s1, q1 = s2; the
# quarter in w13. ----
.macro evb_dsel lane, sh, base           /* v5.s[\lane] = the dword (imm8 >> \sh) & 3 picks, plus \base bytes */
    ubfx w9, w12, \sh, 2
    lsl  w9, w9, 2
    add  w9, w9, \base
    mov  w10, 0x01010101
    mul  w9, w9, w10
    movz w10, 0x0100
    movk w10, 0x0302, lsl 16
    add  w9, w9, w10
    mov  v5.s[\lane], w9
.endm
.macro evb_pshufd                        /* VPSHUFD, VPERMILPS imm8: s2's dwords */
    evb_dsel 0, 0, 0
    evb_dsel 1, 2, 0
    evb_dsel 2, 4, 0
    evb_dsel 3, 6, 0
    tbl  v0.16b, {v1.16b}, v5.16b
.endm
.macro evb_shufps                        /* VSHUFPS: two dwords of s1, then two of s2 */
    evb_dsel 0, 0, 0
    evb_dsel 1, 2, 0
    evb_dsel 2, 4, 16
    evb_dsel 3, 6, 16
    tbl  v0.16b, {v0.16b, v1.16b}, v5.16b
.endm
.macro evb_wsel lane, sh, base           /* v5.h[\lane]: the word (imm8 >> \sh) & 3 picks, plus \base bytes */
    ubfx w9, w12, \sh, 2
    lsl  w9, w9, 1
    add  w9, w9, \base
    mov  w10, 0x0101
    mul  w9, w9, w10
    add  w9, w9, 0x100
    mov  v5.h[\lane], w9
.endm
.macro evb_pshufhw hi                    /* VPSHUFHW/LW: four words of s2's half, the other half kept */
    adr  x9, vex_iota16
    ldr  q5, [x9]
.if \hi
    evb_wsel 4, 0, 8
    evb_wsel 5, 2, 8
    evb_wsel 6, 4, 8
    evb_wsel 7, 6, 8
.else
    evb_wsel 0, 0, 0
    evb_wsel 1, 2, 0
    evb_wsel 2, 4, 0
    evb_wsel 3, 6, 0
.endif
    tbl  v0.16b, {v1.16b}, v5.16b
.endm
.macro evb_shufpd                        /* VSHUFPD: s1's qword by imm8 bit 2q, s2's by bit 2q+1 */
    lsl  w9, w13, 1
    lsr  w10, w12, w9
    and  w11, w10, 1
    and  w10, w10, 2
    lsr  w10, w10, 1
    mov  v5.16b, v0.16b
    cbz  w11, 1f
    mov  v0.d[0], v5.d[1]
    b    2f
1:  mov  v0.d[0], v5.d[0]
2:  cbz  w10, 3f
    mov  v0.d[1], v1.d[1]
    b    4f
3:  mov  v0.d[1], v1.d[0]
4:
.endm
.macro evb_permilpd_i                    /* VPERMILPD imm8: s2's qwords by imm8 bits 2q and 2q+1 */
    lsl  w9, w13, 1
    lsr  w10, w12, w9
    and  w11, w10, 1
    and  w10, w10, 2
    cbz  w11, 1f
    mov  v0.d[0], v1.d[1]
    b    2f
1:  mov  v0.d[0], v1.d[0]
2:  cbz  w10, 3f
    mov  v0.d[1], v1.d[1]
    b    4f
3:  mov  v0.d[1], v1.d[0]
4:
.endm
.macro evb_permilps_v                    /* VPERMILPS var: s1's dwords by s2's (low two bits) */
    movi v5.4s, 3
    and  v5.16b, v1.16b, v5.16b
    shl  v5.4s, v5.4s, 2
    mov  w9, 0x01010101
    dup  v6.4s, w9
    mul  v5.4s, v5.4s, v6.4s
    movz w9, 0x0100
    movk w9, 0x0302, lsl 16
    dup  v6.4s, w9
    add  v5.4s, v5.4s, v6.4s
    tbl  v0.16b, {v0.16b}, v5.16b
.endm
.macro evb_permilpd_v                    /* VPERMILPD var: s1's qwords by bit 1 of s2's */
    ushr v5.2d, v1.2d, 1
    shl  v5.2d, v5.2d, 63
    cmlt v5.2d, v5.2d, 0                 /* all ones where bit 1 is set */
    dup  v6.2d, v0.d[0]
    dup  v7.2d, v0.d[1]
    bsl  v5.16b, v7.16b, v6.16b
    mov  v0.16b, v5.16b
.endm
.macro evb_palignr                       /* VPALIGNR: (s1:s2 in the lane) >> imm8 bytes; past 31 zero */
    adr  x9, vex_iota16
    ldr  q5, [x9]
    dup  v6.16b, w12
    uqadd v5.16b, v5.16b, v6.16b         /* (saturating: imm8 + i stays >= 32 -> zero) */
    mov  v3.16b, v1.16b                  /* the table: s2's bytes, then s1's */
    mov  v4.16b, v0.16b
    tbl  v0.16b, {v3.16b, v4.16b}, v5.16b
.endm
evex_l3 pshufd, 0, evb_pshufd
evex_l3 shufps, 0, evb_shufps
evex_l3 pshufhw, 0, evb_pshufhw, 1
evex_l3 pshuflw, 0, evb_pshufhw, 0
evex_l3 shufpd, 0, evb_shufpd
evex_l3 permilpd_i, 0, evb_permilpd_i
evex_l3 permilps_v, 0, evb_permilps_v
evex_l3 permilpd_v, 0, evb_permilpd_v
evex_l3 palignr, 0, evb_palignr
