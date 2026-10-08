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
#        (51) | ... | a legacy SSE encoding: the quarters past VL kept (63)],
#        gret 1.
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
    tbnz x8, 63, .Lefi_done\@               /* a legacy SSE encoding (GFNI) */
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
    tbz  x8, 58, 4f                      /* an explicit length (a narrowing store): 1 << bits 59-61 */
    ubfx x9, x8, 59, 3
    mov  w14, 1
    lsl  w14, w14, w9
4:  lsr  w9, w14, w15                    /* the elements */
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
3:  /* Compress/expand (word bit 62) touch the first popcount(k) elements, but
       fault as SDE has them: by the enabled elements' own positions. Those
       reach at least as far as the packed ones, so: the first page if any
       element is enabled (the packed start there), the second by position. */
    tbz  x8, 62, 6f
    cmp  x16, 0
    cset w9, ne
6:
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
    tbz  x8, 58, .Levc_len\@
    ubfx x15, x8, 59, 3
    mov  w16, 1
    lsl  w16, w16, w15
.Levc_len\@:
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

# Compress mode (word bit 62): \k becomes its first popcount(k) bits (of the
# operand's elements). Clobbers x15-x17, v30.
.macro evm_compress_mask k, esz
    tbz  x8, 62, .Lecm_done\@
    ubfx w15, w8, 16, 2
    mov  w16, 16
    lsl  w16, w16, w15
    lsr  w16, w16, \esz                  /* the elements */
    cmp  w16, 64
    b.hs 1f
    mov  x17, 1
    lsl  x17, x17, x16
    sub  x17, x17, 1
    and  \k, \k, x17
1:  fmov d30, \k
    cnt  v30.8b, v30.8b
    addv b30, v30.8b
    fmov w15, s30
    mov  \k, -1
    cmp  w15, 64
    b.hs .Lecm_done\@
    mov  \k, 1
    lsl  \k, \k, x15
    sub  \k, \k, 1
.Lecm_done\@:
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
    ubfx x9, x8, \ks, 3                  /* the opmask and element size, for a store's copy */
    mov  x11, CPU_avx512_k
    add  x11, _cpu, x11
    ldr  x12, [x11, x9, lsl 3]
    ubfx x13, x8, (\ks + 4), 2
    evm_compress_mask x12, w13
    mov  w17, w22                        /* the split (after: evm_compress_mask takes x15-x17) */
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
    ubfx x16, x8, \ks, 3
    mov  x11, CPU_avx512_k
    add  x11, _cpu, x11
    ldr  x0, [x11, x16, lsl 3]
    ubfx x19, x8, (\ks + 4), 2
    evm_compress_mask x0, w19
    mov  x15, (EVEX_STAGE + 64)          /* (after: evm_compress_mask takes x15-x17) */
    add  x15, _cpu, x15
    ldr  w17, [x15, 4]
    ldr  w9, [x15, 8]
    ldr  w10, [x15, 12]
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

# ---- VPLZCNTD/Q, VPOPCNTD/Q (s2's elements, as the unary bodies read) ----
.macro evb_lzcnt q
.if \q
    /* clz of a qword: of the high half, plus 32 + clz(low) when that is 0 */
    clz  v4.4s, v1.4s
    uzp1 v5.4s, v4.4s, v4.4s             /* (low halves' counts) */
    uzp2 v6.4s, v4.4s, v4.4s             /* (high halves' counts) */
    movi v7.4s, 32
    cmeq v2.4s, v6.4s, v7.4s
    add  v5.4s, v5.4s, v7.4s
    bit  v6.16b, v5.16b, v2.16b
    uxtl v0.2d, v6.2s
.else
    clz  v0.4s, v1.4s
.endif
.endm
.macro evb_popcnt q
    cnt  v0.16b, v1.16b
    uaddlp v0.8h, v0.16b
    uaddlp v0.4s, v0.8h
.if \q
    uaddlp v0.2d, v0.4s
.endif
.endm
evex_l3 plzcntd, 0, evb_lzcnt, 0
evex_l3 plzcntq, 0, evb_lzcnt, 1
evex_l3 popcntd, 0, evb_popcnt, 0
evex_l3 popcntq, 0, evb_popcnt, 1

# ---- VPMOVB2M/W2M/D2M/Q2M: k[reg] = each element's sign (s2's) ----
.macro evb_tok arr
    cmlt v0.\arr, v1.\arr, 0
.endm
evex_tomask pmovb2m, 0, evb_tok, 16b
evex_tomask pmovw2m, 1, evb_tok, 8h
evex_tomask pmovd2m, 2, evb_tok, 4s
evex_tomask pmovq2m, 3, evb_tok, 2d

# ---- VPMOVM2B/W/D/Q: each element all ones where k (the r/m's, at word
# bits 54-56) has its bit. (The frame's own opmask field is 0.) ----
.macro evb_m2v esz
    ubfx x9, x8, 54, 3
    mov  x15, CPU_avx512_k
    add  x15, _cpu, x15
    ldr  x11, [x15, x9, lsl 3]
    mov  w15, (16 >> \esz)
    mul  w16, w13, w15
    lsr  x11, x11, x16
    adr  x17, evex_kbits
.if \esz == 0
    ldr  q30, [x17]
    dup  v31.8b, w11
    lsr  w11, w11, 8
    dup  v0.8b, w11
    ins  v31.d[1], v0.d[0]
    cmtst v0.16b, v31.16b, v30.16b
.elseif \esz == 1
    ldr  q30, [x17, 16]
    dup  v31.8h, w11
    cmtst v0.8h, v31.8h, v30.8h
.elseif \esz == 2
    ldr  q30, [x17, 32]
    dup  v31.4s, w11
    cmtst v0.4s, v31.4s, v30.4s
.else
    ldr  q30, [x17, 48]
    dup  v31.2d, x11
    cmtst v0.2d, v31.2d, v30.2d
.endif
.endm
evex_l3 pmovm2b, 0, evb_m2v, 0
evex_l3 pmovm2w, 0, evb_m2v, 1
evex_l3 pmovm2d, 0, evb_m2v, 2
evex_l3 pmovm2q, 0, evb_m2v, 3

# ---- VPCONFLICTD/Q: element i = the bits j < i whose element equals i's
# (s2's elements, whole vector: v20-v23), by a scalar pass through memory
# at EVEX_STAGE + 128 (the source) and + 192 (the result) ----
.macro evfb_conflict q
    mov  x14, (EVEX_STAGE + 128)
    add  x14, _cpu, x14
    stp  q20, q21, [x14]
    stp  q22, q23, [x14, 32]
    evf_nelem (2 + \q)
    mov  w10, 0                          /* i */
.Lecf_i\@:
    cmp  w10, w9
    b.hs .Lecf_done\@
.if \q
    ldr  x11, [x14, x10, lsl 3]
.else
    ldr  w11, [x14, x10, lsl 2]
.endif
    mov  x15, 0                          /* the bits */
    mov  w16, 0                          /* j */
.Lecf_j\@:
    cmp  w16, w10
    b.hs .Lecf_put\@
.if \q
    ldr  x17, [x14, x16, lsl 3]
    cmp  x17, x11
.else
    ldr  w17, [x14, x16, lsl 2]
    cmp  w17, w11
.endif
    b.ne 1f
    mov  x17, 1
    lsl  x17, x17, x16
    orr  x15, x15, x17
1:  add  w16, w16, 1
    b    .Lecf_j\@
.Lecf_put\@:
    add  x17, x14, 64
.if \q
    str  x15, [x17, x10, lsl 3]
.else
    str  w15, [x17, x10, lsl 2]
.endif
    add  w10, w10, 1
    b    .Lecf_i\@
.Lecf_done\@:
    ldp  q0, q1, [x14, 64]
    ldp  q2, q3, [x14, 96]
.endm
evex_full pconflictd, evfb_conflict, 0
evex_full pconflictq, evfb_conflict, 1

# ---- VPMOVSX/ZX: each result quarter widened from its chunk of s2 (16 >>
# (de - se) bytes at q times that), pulled out of the whole source by tbl ----
.macro evf_xtl sign, from                /* v4 widened one step from element size \from */
.if \from == 0
  .if \sign
    sxtl v4.8h, v4.8b
  .else
    uxtl v4.8h, v4.8b
  .endif
.elseif \from == 1
  .if \sign
    sxtl v4.4s, v4.4h
  .else
    uxtl v4.4s, v4.4h
  .endif
.else
  .if \sign
    sxtl v4.2d, v4.2s
  .else
    uxtl v4.2d, v4.2s
  .endif
.endif
.endm
.macro evfb_ext1 vd, q, se, de, sign
    adr  x9, vex_iota16
    ldr  q4, [x9]
    mov  w9, ((16 >> (\de - \se)) * \q)
    dup  v5.16b, w9
    add  v4.16b, v4.16b, v5.16b
    tbl  v4.16b, {v20.16b, v21.16b, v22.16b, v23.16b}, v4.16b
    evf_xtl \sign, \se
.if \de - \se > 1
    evf_xtl \sign, (\se + 1)
.endif
.if \de - \se > 2
    evf_xtl \sign, (\se + 2)
.endif
    mov  \vd\().16b, v4.16b
.endm
.macro evfb_extend se, de, sign
    evfb_ext1 v0, 0, \se, \de, \sign
    evfb_ext1 v1, 1, \se, \de, \sign
    evfb_ext1 v2, 2, \se, \de, \sign
    evfb_ext1 v3, 3, \se, \de, \sign
.endm
.irp s, 0,1
evex_full pmov\s\()xbw, evfb_extend, 0, 1, \s
evex_full pmov\s\()xbd, evfb_extend, 0, 2, \s
evex_full pmov\s\()xbq, evfb_extend, 0, 3, \s
evex_full pmov\s\()xwd, evfb_extend, 1, 2, \s
evex_full pmov\s\()xwq, evfb_extend, 1, 3, \s
evex_full pmov\s\()xdq, evfb_extend, 2, 3, \s
.endr

# Masking done by the body (the narrowing moves): the frame stores unmasked;
# the word's opmask is at bits 54-56 (0: none), zeroing at 57. On the
# scratch at EVEX_STAGE + 192 (the result, n elements of size \esz), with
# the old destination at + 128: a masked-off element becomes 0 or the old
# one; past n it stays zero (a register destination is zeroed past the
# result). Then v0-v3 from it. Clobbers x9, x11, x12, x14-x17.
.macro evf_eltcp esz, from, to           /* element x16 of [\from] to [\to] */
.if \esz == 0
    ldrb w17, [\from, x16]
    strb w17, [\to, x16]
.elseif \esz == 1
    ldrh w17, [\from, x16, lsl 1]
    strh w17, [\to, x16, lsl 1]
.elseif \esz == 2
    ldr  w17, [\from, x16, lsl 2]
    str  w17, [\to, x16, lsl 2]
.else
    ldr  x17, [\from, x16, lsl 3]
    str  x17, [\to, x16, lsl 3]
.endif
.endm
.macro evf_eltzero esz, to               /* element x16 of [\to] = 0 */
.if \esz == 0
    strb wzr, [\to, x16]
.elseif \esz == 1
    strh wzr, [\to, x16, lsl 1]
.elseif \esz == 2
    str  wzr, [\to, x16, lsl 2]
.else
    str  xzr, [\to, x16, lsl 3]
.endif
.endm
.macro evf_bodymask esz, n               /* \n: a w register, the result's elements */
    tbnz x8, 32, .Lebm_load\@            /* a store: the store masks */
    ubfx x9, x8, 54, 3
    cbz  w9, .Lebm_load\@
    mov  x15, CPU_avx512_k
    add  x15, _cpu, x15
    ldr  x12, [x15, x9, lsl 3]           /* (not x10: the caller's count may be there) */
    mov  x14, (EVEX_STAGE + 128)
    add  x14, _cpu, x14
    stp  q24, q25, [x14]
    stp  q26, q27, [x14, 32]
    add  x15, x14, 64
    mov  x16, 0
.Lebm_loop\@:
    cmp  w16, \n
    b.hs .Lebm_load\@
    lsr  x11, x12, x16
    tbnz x11, 0, .Lebm_next\@
    tbnz x8, 57, .Lebm_z\@
    evf_eltcp \esz, x14, x15
    b    .Lebm_next\@
.Lebm_z\@:
    evf_eltzero \esz, x15
.Lebm_next\@:
    add  x16, x16, 1
    b    .Lebm_loop\@
.Lebm_load\@:
    mov  x14, (EVEX_STAGE + 192)
    add  x14, _cpu, x14
    ldp  q0, q1, [x14]
    ldp  q2, q3, [x14, 32]
.endm

# ---- VPMOV/VPMOVS/VPMOVUS (the narrowing moves): the source (word field
# s1, its VL at bits 52-53; the frame's VL is the result's) a quarter at a
# time, each narrowed to 16 >> (se - de) bytes appended at EVEX_STAGE + 192 ----
.macro evf_xtn kind, from                /* v4 narrowed one step from element size \from */
.if \from == 3
    \kind v4.2s, v4.2d
.elseif \from == 2
    \kind v4.4h, v4.4s
.else
    \kind v4.8b, v4.8h
.endif
.endm
.macro evfb_narrow kind, se, de
    mov  x14, (EVEX_STAGE + 192)
    add  x14, _cpu, x14
    stp  xzr, xzr, [x14]
    stp  xzr, xzr, [x14, 16]
    stp  xzr, xzr, [x14, 32]
    stp  xzr, xzr, [x14, 48]
    ubfx x9, x8, 52, 2
    mov  w10, 1
    lsl  w10, w10, w9                    /* the source's quarters */
    mov  w12, 0                          /* (the imm8 is unused here) */
.Lenr_q\@:
    cmp  w12, w10
    b.hs .Lenr_done\@
    ubfx w11, w8, 5, 5
    evx_qaddr x15, w11, w12
    ldr  q4, [x15]
    evf_xtn \kind, \se
.if \se - \de > 1
    evf_xtn \kind, (\se - 1)
.endif
.if \se - \de > 2
    evf_xtn \kind, (\se - 2)
.endif
    mov  x14, (EVEX_STAGE + 192)
    add  x14, _cpu, x14
    mov  w11, (16 >> (\se - \de))
    mul  w11, w11, w12
    add  x14, x14, x11
.if \se - \de == 1
    str  d4, [x14]
.elseif \se - \de == 2
    str  s4, [x14]
.else
    str  h4, [x14]
.endif
    add  w12, w12, 1
    b    .Lenr_q\@
.Lenr_done\@:
    lsl  w10, w10, (4 - \se)             /* the result's elements: the source's */
    evf_bodymask \de, w10
.endm
.irp k, xtn,sqxtn,uqxtn
evex_full pmov_\k\()_wb, evfb_narrow, \k, 1, 0
evex_full pmov_\k\()_db, evfb_narrow, \k, 2, 0
evex_full pmov_\k\()_qb, evfb_narrow, \k, 3, 0
evex_full pmov_\k\()_dw, evfb_narrow, \k, 2, 1
evex_full pmov_\k\()_qw, evfb_narrow, \k, 3, 1
evex_full pmov_\k\()_qd, evfb_narrow, \k, 3, 2
.endr

# ---- compress and expand: a scalar pass (the source at EVEX_STAGE + 128,
# the result at + 192). VPCOMPRESS*: the enabled elements of s1 (the reg)
# packed low; VPEXPAND*: s2's low elements spread to the enabled ones. The
# opmask is the word's bits 54-56 (0: all); an element not written is the
# old one, or zero (EVEX.z, word bit 57), or anything for a store. ----
.macro evf_kload                         /* x10 = the opmask (all ones for none) */
    ubfx x9, x8, 54, 3
    mov  x10, -1
    cbz  w9, 1f
    mov  x15, CPU_avx512_k
    add  x15, _cpu, x15
    ldr  x10, [x15, x9, lsl 3]
1:
.endm
.macro evf_prefill                       /* the result = 0 (zeroing, a store) or the old destination */
    mov  x14, (EVEX_STAGE + 192)
    add  x14, _cpu, x14
    tbnz x8, 57, 1f
    tbnz x8, 32, 1f
    stp  q24, q25, [x14]
    stp  q26, q27, [x14, 32]
    b    2f
1:  stp  xzr, xzr, [x14]
    stp  xzr, xzr, [x14, 16]
    stp  xzr, xzr, [x14, 32]
    stp  xzr, xzr, [x14, 48]
2:
.endm
.macro evf_cx_elt esz, from, fi, to, ti  /* [\to] element \ti = [\from] element \fi */
.if \esz == 0
    ldrb w17, [\from, \fi]
    strb w17, [\to, \ti]
.elseif \esz == 1
    ldrh w17, [\from, \fi, lsl 1]
    strh w17, [\to, \ti, lsl 1]
.elseif \esz == 2
    ldr  w17, [\from, \fi, lsl 2]
    str  w17, [\to, \ti, lsl 2]
.else
    ldr  x17, [\from, \fi, lsl 3]
    str  x17, [\to, \ti, lsl 3]
.endif
.endm
.macro evfb_cx esz, expand
    mov  x14, (EVEX_STAGE + 128)
    add  x14, _cpu, x14
.if \expand
    stp  q20, q21, [x14]
    stp  q22, q23, [x14, 32]
.else
    stp  q16, q17, [x14]
    stp  q18, q19, [x14, 32]
.endif
    evf_prefill
    evf_nelem \esz
    mov  w13, w9                         /* the elements (evf_kload takes x9, x10) */
    evf_kload
    mov  x14, (EVEX_STAGE + 128)
    add  x14, _cpu, x14
    add  x15, x14, 64
    mov  x16, 0                          /* i: the position the mask names */
    mov  x12, 0                          /* j: the packed side */
.Lecx_loop\@:
    cmp  w16, w13
    b.hs .Lecx_done\@
    lsr  x11, x10, x16
    tbz  x11, 0, .Lecx_next\@
.if \expand
    evf_cx_elt \esz, x14, x12, x15, x16
.else
    evf_cx_elt \esz, x14, x16, x15, x12
.endif
    add  x12, x12, 1
.Lecx_next\@:
    add  x16, x16, 1
    b    .Lecx_loop\@
.Lecx_done\@:
    ldp  q0, q1, [x15]
    ldp  q2, q3, [x15, 32]
.endm
.irp z, 0,1,2,3
evex_full compress\z, evfb_cx, \z, 0
evex_full expand\z, evfb_cx, \z, 1
.endr

# ---- VPMULTISHIFTQB (VBMI): each byte of s2's qword at the bit offset
# s1's byte names (mod 64), the qword rotated ----
.macro evb_mshq_q i
    mov  x9, v1.d[\i]
    mov  x10, v0.d[\i]
    mov  x11, 0
.irp j, 0,1,2,3,4,5,6,7
    ubfx x14, x10, (8 * \j), 6
    ror  x15, x9, x14
    and  x15, x15, 0xff
    orr  x11, x11, x15, lsl #(8 * \j)
.endr
    mov  v2.d[\i], x11
.endm
.macro evb_mshq
    evb_mshq_q 0
    evb_mshq_q 1
.endm
evex_l3 pmultishiftqb, 2, evb_mshq

# ---- VPSHLD/VPSHRD (VBMI2): the element pair concatenated and shifted, by
# imm8 (a = s1, b = s2) or, the V forms, by s2's element (a = the
# destination, b = s1): left: a << c | b >> (bits - c); right: a >> c |
# b << (bits - c), c mod bits. A NEON shift by bits or more is 0. ----
.macro evb_shd left, var, arr, bits
.ifc \arr,2d
    mov  x9, (\bits - 1)
    dup  v3.2d, x9
    mov  x9, \bits
    dup  v7.2d, x9
.else
    mov  w9, (\bits - 1)
    dup  v3.\arr, w9
    mov  w9, \bits
    dup  v7.\arr, w9
.endif
.if \var
    mov  v5.16b, v2.16b
    mov  v6.16b, v0.16b
    and  v4.16b, v1.16b, v3.16b
.else
    mov  v5.16b, v0.16b
    mov  v6.16b, v1.16b
  .ifc \arr,2d
    and  x9, x12, (\bits - 1)
    dup  v4.2d, x9
  .else
    and  w9, w12, (\bits - 1)
    dup  v4.\arr, w9
  .endif
.endif
.if \left
    ushl v5.\arr, v5.\arr, v4.\arr
    sub  v7.\arr, v4.\arr, v7.\arr       /* c - bits: right by bits - c */
    ushl v6.\arr, v6.\arr, v7.\arr
.else
    neg  v4.\arr, v4.\arr
    ushl v5.\arr, v5.\arr, v4.\arr
    add  v7.\arr, v4.\arr, v7.\arr       /* bits - c */
    ushl v6.\arr, v6.\arr, v7.\arr
.endif
    orr  v0.16b, v5.16b, v6.16b
.endm
evex_l3 pshldw, 0, evb_shd, 1, 0, 8h, 16
evex_l3 pshldd, 0, evb_shd, 1, 0, 4s, 32
evex_l3 pshldq, 0, evb_shd, 1, 0, 2d, 64
evex_l3 pshrdw, 0, evb_shd, 0, 0, 8h, 16
evex_l3 pshrdd, 0, evb_shd, 0, 0, 4s, 32
evex_l3 pshrdq, 0, evb_shd, 0, 0, 2d, 64
evex_l3 pshldvw, 0, evb_shd, 1, 1, 8h, 16
evex_l3 pshldvd, 0, evb_shd, 1, 1, 4s, 32
evex_l3 pshldvq, 0, evb_shd, 1, 1, 2d, 64
evex_l3 pshrdvw, 0, evb_shd, 0, 1, 8h, 16
evex_l3 pshrdvd, 0, evb_shd, 0, 1, 4s, 32
evex_l3 pshrdvq, 0, evb_shd, 0, 1, 2d, 64

# ---- VNNI: the destination's dwords plus the dot product of their s1/s2
# bytes (s1 unsigned, s2 signed: VPDPBUSD) or words (signed: VPDPWSSD);
# the S forms saturate the sum to a signed dword ----
.macro evb_dpbusd sat
    uxtl v4.8h, v0.8b
    uxtl2 v5.8h, v0.16b
    sxtl v6.8h, v1.8b
    sxtl2 v7.8h, v1.16b
    mul  v4.8h, v4.8h, v6.8h             /* (255 * -128 .. 255 * 127: a halfword) */
    mul  v5.8h, v5.8h, v7.8h
    saddlp v4.4s, v4.8h
    saddlp v5.4s, v5.8h
    addp v4.4s, v4.4s, v5.4s
.if \sat
    sqadd v2.4s, v2.4s, v4.4s
.else
    add  v2.4s, v2.4s, v4.4s
.endif
.endm
.macro evb_dpwssd sat
    smull v4.4s, v0.4h, v1.4h
    smull2 v5.4s, v0.8h, v1.8h
.if \sat
    saddlp v4.2d, v4.4s                  /* (the pair can reach 2^31) */
    saddlp v5.2d, v5.4s
    sxtl v6.2d, v2.2s
    sxtl2 v7.2d, v2.4s
    add  v6.2d, v6.2d, v4.2d
    add  v7.2d, v7.2d, v5.2d
    sqxtn v2.2s, v6.2d
    sqxtn2 v2.4s, v7.2d
.else
    addp v4.4s, v4.4s, v5.4s
    add  v2.4s, v2.4s, v4.4s
.endif
.endm
evex_l3 pdpbusd, 2, evb_dpbusd, 0
evex_l3 pdpbusds, 2, evb_dpbusd, 1
evex_l3 pdpwssd, 2, evb_dpwssd, 0
evex_l3 pdpwssds, 2, evb_dpwssd, 1

# ---- IFMA: the destination's qwords plus the low (L) or high (H) 52 bits
# of the 104-bit product of s1's and s2's low 52 bits ----
.macro evb_madd52_q i, hi
    mov  x9, v0.d[\i]
    mov  x10, v1.d[\i]
    ubfx x9, x9, 0, 52
    ubfx x10, x10, 0, 52
.if \hi
    umulh x11, x9, x10
    mul  x9, x9, x10
    extr x9, x11, x9, 52                 /* bits 52-115 (the top 12 are 0) */
.else
    mul  x9, x9, x10
    ubfx x9, x9, 0, 52
.endif
    mov  x10, v2.d[\i]
    add  x9, x9, x10
    mov  v2.d[\i], x9
.endm
.macro evb_madd52 hi
    evb_madd52_q 0, \hi
    evb_madd52_q 1, \hi
.endm
evex_l3 pmadd52luq, 2, evb_madd52, 0
evex_l3 pmadd52huq, 2, evb_madd52, 1

# ---- BITALG: VPOPCNTB/W (s2's elements), VPSHUFBITQMB (k: the bit of s1's
# qword that each byte of s2's names, mod 64) ----
.macro evb_popcntbw w
    cnt  v0.16b, v1.16b
.if \w
    uaddlp v0.8h, v0.16b
.endif
.endm
evex_l3 popcntb, 0, evb_popcntbw, 0
evex_l3 popcntw, 0, evb_popcntbw, 1
.macro evb_shufbitqmb
    movi v5.16b, 0x3f
    and  v4.16b, v1.16b, v5.16b
    ushr v6.16b, v4.16b, 3               /* the byte of the qword */
    movi v7.2d, 0
    mov  x9, 0x0808080808080808
    mov  v7.d[1], x9
    add  v6.16b, v6.16b, v7.16b
    tbl  v6.16b, {v0.16b}, v6.16b
    movi v5.16b, 7
    and  v4.16b, v4.16b, v5.16b
    neg  v4.16b, v4.16b
    ushl v6.16b, v6.16b, v4.16b          /* the bit to bit 0 */
    movi v5.16b, 1
    cmtst v0.16b, v6.16b, v5.16b
.endm
evex_tomask pshufbitqmb, 0, evb_shufbitqmb

# ---- GFNI: GF(2^8) mod x^8 + x^4 + x^3 + x + 1. GF2P8MULB: the product of
# s1's and s2's bytes. GF2P8AFFINE(INV)QB: each byte x of s1 (or its
# inverse, 0 for 0) through the matrix s2's qword is: result bit b =
# parity(matrix byte 7 - b & x) ^ imm8 bit b ----
.macro evb_gf2p8mulb
    pmull v4.8h, v0.8b, v1.8b
    pmull2 v5.8h, v0.16b, v1.16b
    shrn v6.8b, v4.8h, 8                 /* the product's bits 8-14 */
    shrn2 v6.16b, v5.8h, 8
    xtn  v0.8b, v4.8h
    xtn2 v0.16b, v5.8h
    movi v3.16b, 0x1b
    pmull v4.8h, v6.8b, v3.8b            /* x^8 = x^4 + x^3 + x + 1: bits 8-10 again */
    pmull2 v5.8h, v6.16b, v3.16b
    xtn  v7.8b, v4.8h
    xtn2 v7.16b, v5.8h
    eor  v0.16b, v0.16b, v7.16b
    shrn v6.8b, v4.8h, 8
    shrn2 v6.16b, v5.8h, 8
    pmul v6.16b, v6.16b, v3.16b          /* (at most 7 bits now) */
    eor  v0.16b, v0.16b, v6.16b
.endm
evex_l3 gf2p8mulb, 0, evb_gf2p8mulb
.balign 16
evex_gf_inverse:                         /* x^-1 in GF(2^8), 0 for 0 */
    .byte 0x00, 0x01, 0x8d, 0xf6, 0xcb, 0x52, 0x7b, 0xd1, 0xe8, 0x4f, 0x29, 0xc0, 0xb0, 0xe1, 0xe5, 0xc7
    .byte 0x74, 0xb4, 0xaa, 0x4b, 0x99, 0x2b, 0x60, 0x5f, 0x58, 0x3f, 0xfd, 0xcc, 0xff, 0x40, 0xee, 0xb2
    .byte 0x3a, 0x6e, 0x5a, 0xf1, 0x55, 0x4d, 0xa8, 0xc9, 0xc1, 0x0a, 0x98, 0x15, 0x30, 0x44, 0xa2, 0xc2
    .byte 0x2c, 0x45, 0x92, 0x6c, 0xf3, 0x39, 0x66, 0x42, 0xf2, 0x35, 0x20, 0x6f, 0x77, 0xbb, 0x59, 0x19
    .byte 0x1d, 0xfe, 0x37, 0x67, 0x2d, 0x31, 0xf5, 0x69, 0xa7, 0x64, 0xab, 0x13, 0x54, 0x25, 0xe9, 0x09
    .byte 0xed, 0x5c, 0x05, 0xca, 0x4c, 0x24, 0x87, 0xbf, 0x18, 0x3e, 0x22, 0xf0, 0x51, 0xec, 0x61, 0x17
    .byte 0x16, 0x5e, 0xaf, 0xd3, 0x49, 0xa6, 0x36, 0x43, 0xf4, 0x47, 0x91, 0xdf, 0x33, 0x93, 0x21, 0x3b
    .byte 0x79, 0xb7, 0x97, 0x85, 0x10, 0xb5, 0xba, 0x3c, 0xb6, 0x70, 0xd0, 0x06, 0xa1, 0xfa, 0x81, 0x82
    .byte 0x83, 0x7e, 0x7f, 0x80, 0x96, 0x73, 0xbe, 0x56, 0x9b, 0x9e, 0x95, 0xd9, 0xf7, 0x02, 0xb9, 0xa4
    .byte 0xde, 0x6a, 0x32, 0x6d, 0xd8, 0x8a, 0x84, 0x72, 0x2a, 0x14, 0x9f, 0x88, 0xf9, 0xdc, 0x89, 0x9a
    .byte 0xfb, 0x7c, 0x2e, 0xc3, 0x8f, 0xb8, 0x65, 0x48, 0x26, 0xc8, 0x12, 0x4a, 0xce, 0xe7, 0xd2, 0x62
    .byte 0x0c, 0xe0, 0x1f, 0xef, 0x11, 0x75, 0x78, 0x71, 0xa5, 0x8e, 0x76, 0x3d, 0xbd, 0xbc, 0x86, 0x57
    .byte 0x0b, 0x28, 0x2f, 0xa3, 0xda, 0xd4, 0xe4, 0x0f, 0xa9, 0x27, 0x53, 0x04, 0x1b, 0xfc, 0xac, 0xe6
    .byte 0x7a, 0x07, 0xae, 0x63, 0xc5, 0xdb, 0xe2, 0xea, 0x94, 0x8b, 0xc4, 0xd5, 0x9d, 0xf8, 0x90, 0x6b
    .byte 0xb1, 0x0d, 0xd6, 0xeb, 0xc6, 0x0e, 0xcf, 0xad, 0x08, 0x4e, 0xd7, 0xe3, 0x5d, 0x50, 0x1e, 0xb3
    .byte 0x5b, 0x23, 0x38, 0x34, 0x68, 0x46, 0x03, 0x8c, 0xdd, 0x9c, 0x7d, 0xa0, 0xcd, 0x1a, 0x41, 0x1c
.macro evb_gf2p8affine inv
.if \inv
    adr  x9, evex_gf_inverse
    ld1  {v16.16b, v17.16b, v18.16b, v19.16b}, [x9], 64
    tbl  v4.16b, {v16.16b, v17.16b, v18.16b, v19.16b}, v0.16b
    movi v5.16b, 64
    sub  v6.16b, v0.16b, v5.16b
    ld1  {v16.16b, v17.16b, v18.16b, v19.16b}, [x9], 64
    tbx  v4.16b, {v16.16b, v17.16b, v18.16b, v19.16b}, v6.16b
    sub  v6.16b, v6.16b, v5.16b
    ld1  {v16.16b, v17.16b, v18.16b, v19.16b}, [x9], 64
    tbx  v4.16b, {v16.16b, v17.16b, v18.16b, v19.16b}, v6.16b
    sub  v6.16b, v6.16b, v5.16b
    ld1  {v16.16b, v17.16b, v18.16b, v19.16b}, [x9]
    tbx  v4.16b, {v16.16b, v17.16b, v18.16b, v19.16b}, v6.16b
    mov  v0.16b, v4.16b
.endif
    movi v7.2d, 0
    mov  x9, 0x0808080808080808
    mov  v7.d[1], x9                     /* each byte's qword, as a tbl base */
    dup  v2.16b, w12                     /* imm8 */
.irp b, 0,1,2,3,4,5,6,7
    movi v5.16b, (7 - \b)
    add  v5.16b, v5.16b, v7.16b
    tbl  v5.16b, {v1.16b}, v5.16b        /* matrix byte 7 - b */
    and  v5.16b, v5.16b, v0.16b
    cnt  v5.16b, v5.16b
    movi v6.16b, 1
    and  v5.16b, v5.16b, v6.16b
    shl  v5.16b, v5.16b, \b
    eor  v2.16b, v2.16b, v5.16b
.endr
.endm
evex_l3 gf2p8affineqb, 2, evb_gf2p8affine, 0
evex_l3 gf2p8affineinvqb, 2, evb_gf2p8affine, 1

# ---- gathers and scatters: VPGATHER*/VGATHER*, VPSCATTER*/VSCATTER* ----
# Word: [the data register (0-4) | the index register (5-9) | scale (10-11) |
# qword data (12) | qword indices (13) | VL (16-17) | opmask (18-20) |
# scatter (21)]. n elements: the VL's, of the wider of data and index.
# Restartable as x86's: each step takes the lowest element the opmask still
# enables, moves it and clears its bit, so a fault leaves the elements below
# it done and their bits clear, the rest as they were. Nothing is kept
# across the TLB slow path (it keeps only x0-x3 and x8-x13): each step
# finds its element again after it. Done: the opmask zeroed whole, and a
# gather's destination past its elements.
.macro evg_next                          /* w12 = the lowest element the opmask enables, or 64. Clobbers x14-x17 */
    ubfx w14, w8, 18, 3
    mov  x16, CPU_avx512_k
    add  x16, _cpu, x16
    ldr  x17, [x16, x14, lsl 3]
    ubfx w14, w8, 16, 2
    mov  w15, 16
    lsl  w15, w15, w14                   /* the VL's bytes */
    ubfx w14, w8, 12, 2
    cmp  w14, 0
    cset w14, ne
    add  w14, w14, 2
    lsr  w15, w15, w14                   /* its elements (16 at most) */
    mov  x14, 1
    lsl  x14, x14, x15
    sub  x14, x14, 1
    and  x17, x17, x14
    rbit x17, x17
    clz  x12, x17
.endm
# \out = &element w12 of the register in the word's bits \field-(\field + 4),
# of 4 << word bit \szbit bytes. Clobbers x14-x17.
.macro evg_eaddr out, field, szbit
    ubfx w17, w8, \szbit, 1
    add  w17, w17, 2
    lsl  w17, w12, w17                   /* the element's byte */
    ubfx w14, w8, \field, 5
    lsr  w16, w17, 4
    evx_qaddr \out, w14, w16
    and  w17, w17, 15
    add  \out, \out, x17
.endm
.macro evg_index                         /* x13 = element w12's index, sign-extended, scaled. Clobbers x14-x17 */
    evg_eaddr x13, 5, 13
    tbnz w8, 13, .Legx_q\@
    ldrsw x13, [x13]
    b    .Legx_done\@
.Legx_q\@:
    ldr  x13, [x13]
.Legx_done\@:
    ubfx w14, w8, 10, 2
    lsl  x13, x13, x14
.endm
.macro evg_clear                         /* the opmask's bit w12 cleared. Clobbers x13, x14, x16, x17 */
    ubfx w14, w8, 18, 3
    mov  x16, CPU_avx512_k
    add  x16, _cpu, x16
    ldr  x17, [x16, x14, lsl 3]
    mov  x13, 1
    lsl  x13, x13, x12
    bic  x17, x17, x13
    str  x17, [x16, x14, lsl 3]
.endm
.macro evg_put                           /* the data register's element w12 = x10, its bit cleared */
    evg_eaddr x13, 0, 12
    tbnz w8, 12, .Legp_q\@
    str  w10, [x13]
    b    .Legp_done\@
.Legp_q\@:
    str  x10, [x13]
.Legp_done\@:
    evg_clear
.endm
.macro evg_get                           /* x10 = the data register's element w12 */
    evg_eaddr x13, 0, 12
    tbnz w8, 12, .Legg_q\@
    ldr  w10, [x13]
    b    .Legg_done\@
.Legg_q\@:
    ldr  x10, [x13]
.Legg_done\@:
.endm
.macro evg_finish
    ubfx w14, w8, 18, 3
    mov  x16, CPU_avx512_k
    add  x16, _cpu, x16
    str  xzr, [x16, x14, lsl 3]
    tbnz w8, 21, .Legf_done\@            /* (a scatter) */
    ubfx w14, w8, 16, 2
    mov  w13, 16
    lsl  w13, w13, w14                   /* the data's bytes: the VL's, */
    ubfx w14, w8, 12, 2
    cmp  w14, 2
    b.ne .Legf_loop\@
    lsr  w13, w13, 1                     /* or half for qword indices, dword data */
.Legf_loop\@:
    cmp  w13, 64
    b.hs .Legf_done\@
    lsr  w16, w13, 4
    and  w17, w8, 31
    evx_qaddr x14, w17, w16
    and  w17, w13, 15
    add  x14, x14, x17
    str  xzr, [x14]
    add  w13, w13, 8
    b    .Legf_loop\@
.Legf_done\@:
.endm

# amd64: [meta (no index: base + disp + segment)][disp][next_ip][word], gret 4.
.gadget evex_gather_amd64
.Lega_loop:
    ldr  x8, [_ip, 24]
    evg_next
    cmp  w12, 64
    b.hs .Lega_fin
    evg_index
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    amd64_vmem_addr
    add  x9, x9, x13
    ldr  x8, [_ip, 24]
    tbnz w8, 12, .Lega_q
    amd64_vread_prep 4, evex_gather_amd64_4
    ldr  w10, [x9]
    b    .Lega_got
.Lega_q:
    amd64_vread_prep 8, evex_gather_amd64_8
    ldr  x10, [x9]
.Lega_got:
    ldr  x8, [_ip, 24]
    evg_next
    evg_put
    b    .Lega_loop
.Lega_fin:
    evg_finish
    gret 4
    amd64_vread_bullshit 4, evex_gather_amd64_4
    amd64_vread_bullshit 8, evex_gather_amd64_8

.gadget evex_scatter_amd64
.Lesa_loop:
    ldr  x8, [_ip, 24]
    evg_next
    cmp  w12, 64
    b.hs .Lesa_fin
    evg_index
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    amd64_vmem_addr
    add  x9, x9, x13
    ldr  x8, [_ip, 24]
    tbnz w8, 12, .Lesa_q
    amd64_vwrite_prep 4, evex_scatter_amd64_4
    ldr  x8, [_ip, 24]
    evg_next
    evg_get
    str  w10, [x9]
    amd64_vwrite_done 4, evex_scatter_amd64_4
    b    .Lesa_done
.Lesa_q:
    amd64_vwrite_prep 8, evex_scatter_amd64_8
    ldr  x8, [_ip, 24]
    evg_next
    evg_get
    str  x10, [x9]
    amd64_vwrite_done 8, evex_scatter_amd64_8
.Lesa_done:
    ldr  x8, [_ip, 24]
    evg_next
    evg_clear
    b    .Lesa_loop
.Lesa_fin:
    evg_finish
    gret 4
    amd64_vwrite_bullshit 4, evex_scatter_amd64_4
    amd64_vwrite_bullshit 8, evex_scatter_amd64_8

# i386: _addr = base + disp (+ segment), gen.c's; [orig_ip][word], gret 2.
.gadget evex_gather_i386
    mov  x15, EVEX_STAGE
    add  x15, _cpu, x15
    str  _addr, [x15]
.Legi_loop:
    ldr  x8, [_ip, 8]
    evg_next
    cmp  w12, 64
    b.hs .Legi_fin
    evg_index
    mov  x15, EVEX_STAGE
    add  x15, _cpu, x15
    ldr  w9, [x15]
    add  _addr, w9, w13                  /* (32-bit: the address wraps) */
    tbnz w8, 12, .Legi_q
    read_prep 32, evex_gather_i386_4
    ldr  w10, [_xaddr]
    b    .Legi_got
.Legi_q:
    read_prep 64, evex_gather_i386_8
    ldr  x10, [_xaddr]
.Legi_got:
    ldr  x8, [_ip, 8]
    evg_next
    evg_put
    b    .Legi_loop
.Legi_fin:
    evg_finish
    gret 2
    read_bullshit 32, evex_gather_i386_4
    read_bullshit 64, evex_gather_i386_8

.gadget evex_scatter_i386
    mov  x15, EVEX_STAGE
    add  x15, _cpu, x15
    str  _addr, [x15]
.Lesi_loop:
    ldr  x8, [_ip, 8]
    evg_next
    cmp  w12, 64
    b.hs .Lesi_fin
    evg_index
    mov  x15, EVEX_STAGE
    add  x15, _cpu, x15
    ldr  w9, [x15]
    add  _addr, w9, w13
    tbnz w8, 12, .Lesi_q
    write_prep 32, evex_scatter_i386_4
    ldr  x8, [_ip, 8]
    evg_next
    evg_get
    str  w10, [_xaddr]
    write_done 32, evex_scatter_i386_4
    b    .Lesi_done
.Lesi_q:
    write_prep 64, evex_scatter_i386_8
    ldr  x8, [_ip, 8]
    evg_next
    evg_get
    str  x10, [_xaddr]
    write_done 64, evex_scatter_i386_8
.Lesi_done:
    ldr  x8, [_ip, 8]
    evg_next
    evg_clear
    b    .Lesi_loop
.Lesi_fin:
    evg_finish
    gret 2
    write_bullshit 32, evex_scatter_i386_4
    write_bullshit 64, evex_scatter_i386_8

# ---- AES (AESENC/ENCLAST/DEC/DECLAST, AESIMC, AESKEYGENASSIST) and
# PCLMULQDQ, each 128-bit lane: the legacy SSE, VEX and EVEX (VAES,
# VPCLMULQDQ) forms all come here (gen.c vex_to_evex). x86's round is ARM's
# with the round key added after the column mix, not before the S-box. ----
.arch_extension aes
.macro evb_aes dec, last                 /* s1 the state, s2 the round key */
    movi v4.2d, 0
.if \dec
    aesd v0.16b, v4.16b
  .if !\last
    aesimc v0.16b, v0.16b
  .endif
.else
    aese v0.16b, v4.16b
  .if !\last
    aesmc v0.16b, v0.16b
  .endif
.endif
    eor  v0.16b, v0.16b, v1.16b
.endm
evex_l3 aesenc, 0, evb_aes, 0, 0
evex_l3 aesenclast, 0, evb_aes, 0, 1
evex_l3 aesdec, 0, evb_aes, 1, 0
evex_l3 aesdeclast, 0, evb_aes, 1, 1
.macro evb_aesimc                        /* (unary: s2) */
    aesimc v0.16b, v1.16b
.endm
evex_l3 aesimc, 0, evb_aesimc
.macro evb_aeskga                        /* s2's dwords 1 and 3 through the S-box; each also rotated ^ imm8 */
    dup  v4.4s, v1.s[1]
    dup  v5.4s, v1.s[3]
    movi v6.2d, 0
    aese v4.16b, v6.16b                  /* (equal columns: ShiftRows moves nothing) */
    aese v5.16b, v6.16b
    mov  w9, v4.s[0]
    mov  w10, v5.s[0]
    ror  w11, w9, 8
    eor  w11, w11, w12
    ror  w14, w10, 8
    eor  w14, w14, w12
    mov  v0.s[0], w9
    mov  v0.s[1], w11
    mov  v0.s[2], w10
    mov  v0.s[3], w14
.endm
evex_l3 aeskeygenassist, 0, evb_aeskga
.macro evb_pclmul                        /* the qwords of s1 and s2 imm8 bits 0 and 4 pick, carryless */
    tst  w12, 1
    b.eq .Lepc_a0\@
    dup  v4.2d, v0.d[1]
    b    .Lepc_b\@
.Lepc_a0\@:
    mov  v4.16b, v0.16b
.Lepc_b\@:
    tst  w12, 0x10
    b.eq .Lepc_b0\@
    dup  v5.2d, v1.d[1]
    b    .Lepc_mul\@
.Lepc_b0\@:
    mov  v5.16b, v1.16b
.Lepc_mul\@:
    pmull v0.1q, v4.1d, v5.1d
.endm
evex_l3 pclmulqdq, 0, evb_pclmul
