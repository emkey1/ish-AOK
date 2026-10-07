# ===========================================================================
# VEX (AVX, AVX2) for both x86 guests (included at the end of math.S, whose
# SSE body macros these reuse; jit/gen.c gen_vex).
#
# A VEX op is three-operand and covers 128 or 256 bits: dst = s1 OP s2, s1
# the VEX.vvvv register and s2 the ModRM r/m (a register, or memory staged
# by a vex_ld* gadget first into VEX_STAGE, 32 bytes of the JIT frame's
# crosspage buffer past anything a crosspage access uses), each 128-bit half
# on its own (the SSE body runs twice: xmm[], then ymm_hi[]). VEX.128 zeroes
# bits 255:128 of the destination, and every VEX register write zeroes bits
# 511:256 (zmm_hi[dst]).
# Bodies take q0 = s1's half, q1 = s2's, w12 = imm8, and leave the result in
# q0 (the integer bodies) or q2 (the float ones).
#
# Word: [s1 (0-3) | s2 (4-7) | dst (8-11) | s2 staged (12) | L (13) |
#        imm8 (16-23)], gret 1.
# ===========================================================================

#define VEX_STAGE (LOCAL_value + 256)

.macro vex_addr out, idx, base, shift
    mov  \out, \base
    add  \out, _cpu, \out
    add  \out, \out, \idx, lsl \shift
.endm

# x10 = &s1, x14 = &s2, x13 = &dst for half \hi (0: xmm[], 1: ymm_hi[]).
# Clobbers x9, x11, x15.
.macro vex_ptrs hi
    ubfx x9, x8, 0, 4
    ubfx x11, x8, 4, 4
    ubfx x15, x8, 8, 4
.if \hi
    vex_addr x10, x9, CPU_ymm_hi, 4
    vex_addr x13, x15, CPU_ymm_hi, 4
.else
    vex_addr x10, x9, CPU_xmm, 4
    vex_addr x13, x15, CPU_xmm, 4
.endif
    tbz  x8, 12, .Lvp_reg\@
    mov  x14, (VEX_STAGE + 16 * \hi)
    add  x14, _cpu, x14
    b    .Lvp_done\@
.Lvp_reg\@:
.if \hi
    vex_addr x14, x11, CPU_ymm_hi, 4
.else
    vex_addr x14, x11, CPU_xmm, 4
.endif
.Lvp_done\@:
.endm

# The end of every VEX register write: zmm_hi[dst] zeroed. Clobbers x9, x15.
.macro vex_zero_zmm
    ubfx x15, x8, 8, 4
    vex_addr x9, x15, CPU_zmm_hi, 5
    stp  xzr, xzr, [x9]
    stp  xzr, xzr, [x9, 16]
.endm

# dst = s1 OP s2, half by half. vex_l3i: the high half's imm8 is the word's
# shifted right by ihs (VBLENDPS's bits 7:4, VMPSADBW's 5:3, ...).
.macro vex_l3 name, res, body, args:vararg
    vex_l3g \name, 0, 0, \res, \body, \args
.endm
.macro vex_l3i name, ihs, res, body, args:vararg
    vex_l3g \name, \ihs, 0, \res, \body, \args
.endm
# s2lo: the high half reads s2's LOW half too (the broadcasts)
.macro vex_l3g name, ihs, s2lo, res, body, args:vararg
.gadget vex_\name
    ldr  x8, [_ip]
    vex_ptrs 0
    ldr  q0, [x10]
    ldr  q1, [x14]
    ubfx w12, w8, 16, 8
    \body \args
    str  \res, [x13]
.if \s2lo
    mov  x12, x14
    vex_ptrs 1
    mov  x14, x12
.else
    vex_ptrs 1
.endif
    tbz  x8, 13, .Ll3_128\@
    ldr  q0, [x10]
    ldr  q1, [x14]
    ubfx w12, w8, (16 + \ihs), 8
    \body \args
    str  \res, [x13]
    b    .Ll3_end\@
.Ll3_128\@:
    stp  xzr, xzr, [x13]
.Ll3_end\@:
    vex_zero_zmm
    gret 1
.endm

# Shifts by a count (VPSRLW .. VPSLLQ ymm, ymm, xmm/m128): dst = s1 shifted
# by s2's low quadword, in both halves.
.macro vex_shx name, kind, L, arr
.gadget vex_\name
    ldr  x8, [_ip]
    vex_ptrs 0
    ldr  x12, [x14]                       /* the count: s2's low quadword, read */
    ldr  q0, [x10]                        /* before dst (which may be s2) is written */
    mov  x10, x12
    vsh_body \kind, \L, \arr
    str  q0, [x13]
    vex_ptrs 1
    tbz  x8, 13, .Lsx_128\@
    ldr  q0, [x10]
    mov  x10, x12
    vsh_body \kind, \L, \arr
    str  q0, [x13]
    b    .Lsx_end\@
.Lsx_128\@:
    stp  xzr, xzr, [x13]
.Lsx_end\@:
    vex_zero_zmm
    gret 1
.endm

# Shifts by imm8 (the 71/72/73 groups): dst = s2 shifted by imm8 (here dst is
# VEX.vvvv and s2 the r/m register).
.macro vex_shi name, kind, L, arr
.gadget vex_\name
    ldr  x8, [_ip]
    vex_ptrs 0
    ldr  q0, [x14]
    ubfx x10, x8, 16, 8
    vsh_body \kind, \L, \arr
    str  q0, [x13]
    vex_ptrs 1
    tbz  x8, 13, .Lsi_128\@
    ldr  q0, [x14]
    ubfx x10, x8, 16, 8
    vsh_body \kind, \L, \arr
    str  q0, [x13]
    b    .Lsi_end\@
.Lsi_128\@:
    stp  xzr, xzr, [x13]
.Lsi_end\@:
    vex_zero_zmm
    gret 1
.endm

# VPSRLDQ / VPSLLDQ by imm8, each 128-bit lane on its own.
.macro vex_dq_body right
    ubfx x10, x8, 16, 8
    cmp  x10, 15
    b.ls .Ldq_in\@
    movi v0.2d, 0
    b    .Ldq_out\@
.Ldq_in\@:
    adr  x11, vex_iota16
    ldr  q2, [x11]
    dup  v3.16b, w10
.if \right
    add  v2.16b, v2.16b, v3.16b
.else
    sub  v2.16b, v2.16b, v3.16b
.endif
    tbl  v0.16b, {v0.16b}, v2.16b
.Ldq_out\@:
.endm
.macro vex_dq name, right
.gadget vex_\name
    ldr  x8, [_ip]
    vex_ptrs 0
    ldr  q0, [x14]
    vex_dq_body \right
    str  q0, [x13]
    vex_ptrs 1
    tbz  x8, 13, .Ldq_128\@
    ldr  q0, [x14]
    vex_dq_body \right
    str  q0, [x13]
    b    .Ldq_end\@
.Ldq_128\@:
    stp  xzr, xzr, [x13]
.Ldq_end\@:
    vex_zero_zmm
    gret 1
.endm
.balign 16
vex_iota16:
    .byte 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15

# ---- the integer ops (math.S's amd64_vi_* bodies) ----

vex_l3 vi_paddb, q0, vi_body, bin, add, 16b, 1
vex_l3 vi_paddw, q0, vi_body, bin, add, 8h, 1
vex_l3 vi_paddd, q0, vi_body, bin, add, 4s, 1
vex_l3 vi_paddq, q0, vi_body, dq, add, 2d, 1
vex_l3 vi_psubb, q0, vi_body, bin, sub, 16b, 1
vex_l3 vi_psubw, q0, vi_body, bin, sub, 8h, 1
vex_l3 vi_psubd, q0, vi_body, bin, sub, 4s, 1
vex_l3 vi_psubq, q0, vi_body, dq, sub, 2d, 1
vex_l3 vi_paddsb, q0, vi_body, bin, sqadd, 16b, 1
vex_l3 vi_paddsw, q0, vi_body, bin, sqadd, 8h, 1
vex_l3 vi_paddusb, q0, vi_body, bin, uqadd, 16b, 1
vex_l3 vi_paddusw, q0, vi_body, bin, uqadd, 8h, 1
vex_l3 vi_psubsb, q0, vi_body, bin, sqsub, 16b, 1
vex_l3 vi_psubsw, q0, vi_body, bin, sqsub, 8h, 1
vex_l3 vi_psubusb, q0, vi_body, bin, uqsub, 16b, 1
vex_l3 vi_psubusw, q0, vi_body, bin, uqsub, 8h, 1
vex_l3 vi_pcmpeqb, q0, vi_body, bin, cmeq, 16b, 1
vex_l3 vi_pcmpeqw, q0, vi_body, bin, cmeq, 8h, 1
vex_l3 vi_pcmpeqd, q0, vi_body, bin, cmeq, 4s, 1
vex_l3 vi_pcmpgtb, q0, vi_body, bin, cmgt, 16b, 1
vex_l3 vi_pcmpgtw, q0, vi_body, bin, cmgt, 8h, 1
vex_l3 vi_pcmpgtd, q0, vi_body, bin, cmgt, 4s, 1
vex_l3 vi_pand, q0, vi_body, bin, and, 16b, 1
vex_l3 vi_pandn, q0, vi_body, binr, bic, 16b, 1
vex_l3 vi_por, q0, vi_body, bin, orr, 16b, 1
vex_l3 vi_pxor, q0, vi_body, bin, eor, 16b, 1
vex_l3 vi_pminub, q0, vi_body, bin, umin, 16b, 1
vex_l3 vi_pmaxub, q0, vi_body, bin, umax, 16b, 1
vex_l3 vi_pminsw, q0, vi_body, bin, smin, 8h, 1
vex_l3 vi_pmaxsw, q0, vi_body, bin, smax, 8h, 1
vex_l3 vi_pavgb, q0, vi_body, bin, urhadd, 16b, 1
vex_l3 vi_pavgw, q0, vi_body, bin, urhadd, 8h, 1
vex_l3 vi_pmullw, q0, vi_body, bin, mul, 8h, 1
vex_l3 vi_pmulhw, q0, vi_body, mulh, smull, -, 1
vex_l3 vi_pmulhuw, q0, vi_body, mulh, umull, -, 1
vex_l3 vi_pmuludq, q0, vi_body, muludq, -, -, 1
vex_l3 vi_pmaddwd, q0, vi_body, madd, -, -, 1
vex_l3 vi_psadbw, q0, vi_body, sad, -, -, 1
vex_l3 vi_punpcklbw, q0, vi_body, bin, zip1, 16b, 1
vex_l3 vi_punpcklwd, q0, vi_body, bin, zip1, 8h, 1
vex_l3 vi_punpckldq, q0, vi_body, bin, zip1, 4s, 1
vex_l3 vi_punpckhbw, q0, vi_body, bin, zip2, 16b, 1
vex_l3 vi_punpckhwd, q0, vi_body, bin, zip2, 8h, 1
vex_l3 vi_punpckhdq, q0, vi_body, bin, zip2, 4s, 1
vex_l3 vi_punpcklqdq, q0, vi_body, bin, zip1, 2d, 1
vex_l3 vi_punpckhqdq, q0, vi_body, bin, zip2, 2d, 1
vex_l3 vi_packsswb, q0, vi_body, packbw, sqxtn, -, 1
vex_l3 vi_packuswb, q0, vi_body, packbw, sqxtun, -, 1
vex_l3 vi_packssdw, q0, vi_body, packwd, sqxtn, -, 1
vex_l3 vi_phaddw, q0, vi_body, bin, addp, 8h, 1
vex_l3 vi_phaddd, q0, vi_body, bin, addp, 4s, 1
vex_l3 vi_pcmpeqq, q0, vi_body, bin, cmeq, 2d, 1
vex_l3 vi_pcmpgtq, q0, vi_body, bin, cmgt, 2d, 1
vex_l3 vi_packusdw, q0, vi_body, packwd, sqxtun, -, 1
vex_l3 vi_pminsb, q0, vi_body, bin, smin, 16b, 1
vex_l3 vi_pminsd, q0, vi_body, bin, smin, 4s, 1
vex_l3 vi_pminuw, q0, vi_body, bin, umin, 8h, 1
vex_l3 vi_pminud, q0, vi_body, bin, umin, 4s, 1
vex_l3 vi_pmaxsb, q0, vi_body, bin, smax, 16b, 1
vex_l3 vi_pmaxsd, q0, vi_body, bin, smax, 4s, 1
vex_l3 vi_pmaxuw, q0, vi_body, bin, umax, 8h, 1
vex_l3 vi_pmaxud, q0, vi_body, bin, umax, 4s, 1
vex_l3 vi_pmulld, q0, vi_body, bin, mul, 4s, 1
vex_l3 vi_pshufb, q0, vi38_body, pshufb, -, 16b, 1
vex_l3 vi_phaddsw, q0, vi38_body, hpair, sqadd, 8h, 1
vex_l3 vi_phsubw, q0, vi38_body, hpair, sub, 8h, 1
vex_l3 vi_phsubd, q0, vi38_body, hpair, sub, 4s, 1
vex_l3 vi_phsubsw, q0, vi38_body, hpair, sqsub, 8h, 1
vex_l3 vi_pmaddubsw, q0, vi38_body, maddubs, -, -, 1
vex_l3 vi_pmulhrsw, q0, vi38_body, mulhrs, -, -, 1
vex_l3 vi_psignb, q0, vi38_body, sign, -, 16b, 1
vex_l3 vi_psignw, q0, vi38_body, sign, -, 8h, 1
vex_l3 vi_psignd, q0, vi38_body, sign, -, 4s, 1
vex_l3 vi_pabsb, q0, vi38_body, abs, -, 16b, 1
vex_l3 vi_pabsw, q0, vi38_body, abs, -, 8h, 1
vex_l3 vi_pabsd, q0, vi38_body, abs, -, 4s, 1
vex_l3 vi_pmuldq, q0, vi38_body, muldq, -, -, 1
vex_l3 vi_phminposuw, q0, vi38_body, minpos, -, -, 1

# ---- the shifts (math.S's vsh_body) ----
vex_shx vsh_psrlw_x, srl, 16, 8h
vex_shi vsh_psrlw_i, srl, 16, 8h
vex_shx vsh_psrld_x, srl, 32, 4s
vex_shi vsh_psrld_i, srl, 32, 4s
vex_shx vsh_psrlq_x, srl, 64, 2d
vex_shi vsh_psrlq_i, srl, 64, 2d
vex_shx vsh_psraw_x, sra, 16, 8h
vex_shi vsh_psraw_i, sra, 16, 8h
vex_shx vsh_psrad_x, sra, 32, 4s
vex_shi vsh_psrad_i, sra, 32, 4s
vex_shx vsh_psllw_x, sll, 16, 8h
vex_shi vsh_psllw_i, sll, 16, 8h
vex_shx vsh_pslld_x, sll, 32, 4s
vex_shi vsh_pslld_i, sll, 32, 4s
vex_shx vsh_psllq_x, sll, 64, 2d
vex_shi vsh_psllq_i, sll, 64, 2d
vex_dq vsh_psrldq_i, 1
vex_dq vsh_pslldq_i, 0

# ---- staging a memory operand into VEX_STAGE ----
# i386: the address in _addr. [orig_ip], gret 1.
# amd64: [meta][disp][next_ip], the address from meta/disp (AMD64_JIT_MEM_
# ALIGN16 / ALIGN32 checked there), gret 3.
# A fault leaves before anything architectural has changed.
.macro vex_stage_store bytes, src
.if \bytes == 32
    ldp  q0, q1, [\src]
.elseif \bytes == 16
    ldr  q0, [\src]
.elseif \bytes == 8
    ldr  d0, [\src]
.elseif \bytes == 4
    ldr  s0, [\src]
.elseif \bytes == 2
    ldr  h0, [\src]
.else
    ldr  b0, [\src]
.endif
    mov  x15, VEX_STAGE
    add  x15, _cpu, x15
.if \bytes == 32
    stp  q0, q1, [x15]
.else
    str  q0, [x15]
.endif
.endm
.macro vex_ld_i386 bytes
.gadget vex_ld_i386_\bytes
    read_prep (\bytes * 8), vex_ld_i386_\bytes
    vex_stage_store \bytes, _xaddr
    gret 1
    read_bullshit (\bytes * 8), vex_ld_i386_\bytes
.endm
.macro vex_ld_amd64 bytes
.gadget vex_ld_amd64_\bytes
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    amd64_vmem_addr
    amd64_vread_prep \bytes, vex_ld_amd64_\bytes
    vex_stage_store \bytes, x9
    gret 3
    amd64_vread_bullshit \bytes, vex_ld_amd64_\bytes
.endm
.irp b, 1,2,4,8,16,32
vex_ld_i386 \b
vex_ld_amd64 \b
.endr

# A 32-byte aligned operand (VMOVAPS ymm and its kind): i386 [orig_ip], gret 1
# (memory.S's vec_align16 for 16 bytes).
.gadget vec_align32
    tst _addr, 31
    b.ne 1f
    gret 1
1:  str xzr, [_cpu, CPU_segfault_addr]
    strb wzr, [_cpu, CPU_segfault_was_write]
    strb wzr, [_cpu, CPU_segfault_reported]
    ldr x8, [_ip]
    mov eip, w8
    mov x0, INT_GPF
    b jit_exit

# ---- floating point (math.S's xf bodies: x86-exact NaNs, DAZ, FTZ, DE) ----
# Scalar forms compute lane 0 and keep s1's other lanes (the bodies merge
# into q0); VEX.L is ignored for them (gen.c clears it), so bits 255:128
# are zeroed.
.irp op, add, sub, mul
vex_l3 xf_\op\()ps, q2, xf_body_arith, f\op, s, 4s, 1, 0
vex_l3 xf_\op\()pd, q2, xf_body_arith, f\op, d, 2d, 1, 0
vex_l3 xf_\op\()ss, q2, xf_body_arith, f\op, s, 4s, 0, 0
vex_l3 xf_\op\()sd, q2, xf_body_arith, f\op, d, 2d, 0, 0
.endr
vex_l3 xf_divps, q2, xf_body_arith, fdiv, s, 4s, 1, 1
vex_l3 xf_divpd, q2, xf_body_arith, fdiv, d, 2d, 1, 1
vex_l3 xf_divss, q2, xf_body_arith, fdiv, s, 4s, 0, 1
vex_l3 xf_divsd, q2, xf_body_arith, fdiv, d, 2d, 0, 1
vex_l3 xf_minps, q2, xf_body_minmax, 0, s, 4s, 1
vex_l3 xf_minpd, q2, xf_body_minmax, 0, d, 2d, 1
vex_l3 xf_minss, q2, xf_body_minmax, 0, s, 4s, 0
vex_l3 xf_minsd, q2, xf_body_minmax, 0, d, 2d, 0
vex_l3 xf_maxps, q2, xf_body_minmax, 1, s, 4s, 1
vex_l3 xf_maxpd, q2, xf_body_minmax, 1, d, 2d, 1
vex_l3 xf_maxss, q2, xf_body_minmax, 1, s, 4s, 0
vex_l3 xf_maxsd, q2, xf_body_minmax, 1, d, 2d, 0
vex_l3 xf_sqrtps, q2, xf_body_sqrt, s, 4s, 1
vex_l3 xf_sqrtpd, q2, xf_body_sqrt, d, 2d, 1
vex_l3 xf_sqrtss, q2, xf_body_sqrt, s, 4s, 0
vex_l3 xf_sqrtsd, q2, xf_body_sqrt, d, 2d, 0
vex_l3 xf_addsubps, q2, xf_body_addsub, s, 4s
vex_l3 xf_addsubpd, q2, xf_body_addsub, d, 2d
vex_l3 xf_haddps, q2, xf_body_hadd, 0, s, 4s
vex_l3 xf_haddpd, q2, xf_body_hadd, 0, d, 2d
vex_l3 xf_hsubps, q2, xf_body_hadd, 1, s, 4s
vex_l3 xf_hsubpd, q2, xf_body_hadd, 1, d, 2d

# VCMPPS/PD/SS/SD imm8 0-31. The relation is imm & 7 (EQ LT LE FALSE NE GE GT
# TRUE, on the ordered lanes); an unordered lane is true for predicates 3-6,
# 8-10 and 15 (0x8778); the signalling ones (IE for any NaN, not only an
# SNaN) are 1 2 5 6 9 10 13 14 (0x6666), bit 4 flipping both. The relation
# runs on operands whose NaN lanes are zeroed, so it raises nothing itself;
# the ordered mask's quiet compare raises IE for an SNaN, and a signalling
# predicate's extra raw compare for any NaN.
.macro xc op, d, n, m, t, a, p
.if \p
    \op  v\d\().\a, v\n\().\a, v\m\().\a
.else
    \op  \t\d, \t\n, \t\m
.endif
.endm
.macro xf_body_cmp32 t, a, p
    xf_consts \t
    xf_lanes \t, \p
    xf_pre2 \a, 0
    and  w12, w12, 31
    xc fcmeq, 2, 4, 4, \t, \a, \p
    xc fcmeq, 3, 5, 5, \t, \a, \p
    and  v2.16b, v2.16b, v3.16b          /* the ordered lanes */
    mov  w9, 0x6666
    and  w10, w12, 15
    lsr  w9, w9, w10
    eor  w9, w9, w12, lsr 4
    tbz  w9, 0, .Lc32_q\@
    xc fcmge, 3, 4, 5, \t, \a, \p        /* signalling: IE for any NaN */
.Lc32_q\@:
    and  v4.16b, v4.16b, v2.16b
    and  v5.16b, v5.16b, v2.16b
    and  w9, w12, 7
    cmp  w9, 3
    b.eq .Lc32_false\@
    cmp  w9, 7
    b.eq .Lc32_true\@
    cmp  w9, 1
    b.eq .Lc32_lt\@
    cmp  w9, 2
    b.eq .Lc32_le\@
    cmp  w9, 5
    b.eq .Lc32_ge\@
    cmp  w9, 6
    b.eq .Lc32_gt\@
    xc fcmeq, 3, 4, 5, \t, \a, \p        /* EQ, NE */
    cmp  w9, 4
    b.ne .Lc32_rel\@
    mvn  v3.16b, v3.16b
    b    .Lc32_rel\@
.Lc32_lt\@:
    xc fcmgt, 3, 5, 4, \t, \a, \p
    b    .Lc32_rel\@
.Lc32_le\@:
    xc fcmge, 3, 5, 4, \t, \a, \p
    b    .Lc32_rel\@
.Lc32_ge\@:
    xc fcmge, 3, 4, 5, \t, \a, \p
    b    .Lc32_rel\@
.Lc32_gt\@:
    xc fcmgt, 3, 4, 5, \t, \a, \p
    b    .Lc32_rel\@
.Lc32_false\@:
    movi v3.2d, 0
    b    .Lc32_rel\@
.Lc32_true\@:
    movi v3.2d, 0xffffffffffffffff
.Lc32_rel\@:
    and  w9, w12, 15
    mov  w10, 0x8778
    lsr  w10, w10, w9
    tbz  w10, 0, .Lc32_ord\@
    orn  v3.16b, v3.16b, v2.16b          /* unordered: true */
    b    .Lc32_out\@
.Lc32_ord\@:
    and  v3.16b, v3.16b, v2.16b
.Lc32_out\@:
    mov  v2.16b, v3.16b
    xf_merge \t, \p
.endm
vex_l3 xf_cmpps, q2, xf_body_cmp32, s, 4s, 1
vex_l3 xf_cmppd, q2, xf_body_cmp32, d, 2d, 1
vex_l3 xf_cmpss, q2, xf_body_cmp32, s, 4s, 0
vex_l3 xf_cmpsd, q2, xf_body_cmp32, d, 2d, 0

# ---- shuffles by tbl: gen.c turns imm8 into each 128-bit lane's byte index
# (x86_shuffle_index). Two-source (VSHUFPS/PD, VPERMILPD): tbl {s1, s2};
# one-source (VPSHUFD/HW/LW, VPERMILPS): tbl {s2}. [word][idx lo lane: 2
# quads][idx hi lane: 2 quads], gret 5.
.macro vex_tbl name, two
.gadget vex_\name
    ldr  x8, [_ip]
    vex_ptrs 0
    ldr  q0, [x10]
    ldr  q1, [x14]
    ldr  q2, [_ip, 8]
.if \two
    tbl  v0.16b, {v0.16b, v1.16b}, v2.16b
.else
    tbl  v0.16b, {v1.16b}, v2.16b
.endif
    str  q0, [x13]
    vex_ptrs 1
    tbz  x8, 13, .Ltb_128\@
    ldr  q0, [x10]
    ldr  q1, [x14]
    ldr  q2, [_ip, 24]
.if \two
    tbl  v0.16b, {v0.16b, v1.16b}, v2.16b
.else
    tbl  v0.16b, {v1.16b}, v2.16b
.endif
    str  q0, [x13]
    b    .Ltb_end\@
.Ltb_128\@:
    stp  xzr, xzr, [x13]
.Ltb_end\@:
    vex_zero_zmm
    gret 5
.endm
vex_tbl tbl1, 0
vex_tbl tbl2, 1

# ---- the 0F3A immediate ops (math.S's v3a bodies), DPPS/DPPD, ROUND ----
vex_l3i v3a_palignr, 0, q0, v3a_body, palignr, 1
vex_l3i v3a_pblendw, 0, q0, v3a_body, blendw, 1
vex_l3i v3a_blendps, 4, q0, v3a_body, blendps, 1
vex_l3i v3a_blendpd, 2, q0, v3a_body, blendpd, 1
vex_l3i v3a_mpsadbw, 3, q0, v3a_body, mpsadbw, 1
vex_l3 xf_dpps, q2, xf_body_dpps
vex_l3 xf_dppd, q2, xf_body_dppd
vex_l3 xf_roundps, q2, xf_body_round, s, 4s, 1
vex_l3 xf_roundpd, q2, xf_body_round, d, 2d, 1
vex_l3 xf_roundss, q2, xf_body_round, s, 4s, 0
vex_l3 xf_roundsd, q2, xf_body_round, d, 2d, 0

# ---- the lane-local conversions, reciprocals, MOVSLDUP/MOVSHDUP/MOVDDUP ----
vex_l3 xf_cvtdq2ps, q2, xfb_cvtdq2ps
vex_l3 xf_cvtps2dq, q2, xfb_cvtps2dq
vex_l3 xf_cvttps2dq, q2, xfb_cvttps2dq
vex_l3 xf_rcpps, q2, xf_recip_body, 0, 1
vex_l3 xf_rcpss, q2, xf_recip_body, 0, 0
vex_l3 xf_rsqrtps, q2, xf_recip_body, 1, 1
vex_l3 xf_rsqrtss, q2, xf_recip_body, 1, 0
vex_l3 xf_movsldup, q2, xf_dup_body, sl
vex_l3 xf_movshdup, q2, xf_dup_body, sh
vex_l3 xf_movddup, q2, xf_dup_body, dd

# ---- data movement ----
# Register moves and merges (the loads stage first): ALL a copy; SS/SD the
# low dword/qword over s1's (VMOVSS/SD xmm, xmm, xmm); HL VMOVHLPS (s2's
# high qword low); LH VMOVLHPS / VMOVHPS-load (s2's low qword high); Q the
# low qword, zero-extended (VMOVQ); LQ VMOVLPS-load (s2's low over s1's low).
.macro vmv_body kind
.ifc \kind,all
    mov  v0.16b, v1.16b
.endif
.ifc \kind,ss
    mov  v0.s[0], v1.s[0]
.endif
.ifc \kind,sd
    mov  v0.d[0], v1.d[0]
.endif
.ifc \kind,hl
    mov  v0.d[0], v1.d[1]
.endif
.ifc \kind,lh
    mov  v0.d[1], v1.d[0]
.endif
.ifc \kind,q
    fmov d0, d1
.endif
.endm
.irp k, all,ss,sd,hl,lh,q
vex_l3 mv_\k, q0, vmv_body, \k
.endr

# Broadcasts: s2's low element (or 128 bits) to every lane of both halves.
.macro vbc_body kind
.ifc \kind,b
    dup  v0.16b, v1.b[0]
.endif
.ifc \kind,w
    dup  v0.8h, v1.h[0]
.endif
.ifc \kind,d
    dup  v0.4s, v1.s[0]
.endif
.ifc \kind,q
    dup  v0.2d, v1.d[0]
.endif
.ifc \kind,x
    mov  v0.16b, v1.16b
.endif
.endm
.irp k, b,w,d,q,x
vex_l3g bc_\k, 0, 1, q0, vbc_body, \k
.endr

# Stores: N bytes from cpu_state offsets to memory. i386: [orig_ip][src lo |
# src hi << 16] (src hi: a 32-byte store's upper 16 bytes), gret 2; amd64:
# [meta][disp][next_ip][src lo | src hi << 16], gret 4.
.macro vex_st_copy bytes, dst
    ldr  x15, [_ip, \dst]
    and  x12, x15, 0xffff
.if \bytes == 32
    ubfx x11, x15, 16, 16
    ldr  q0, [_cpu, x12]
    ldr  q1, [_cpu, x11]
.elseif \bytes == 16
    ldr  q0, [_cpu, x12]
.elseif \bytes == 8
    ldr  d0, [_cpu, x12]
.elseif \bytes == 4
    ldr  s0, [_cpu, x12]
.elseif \bytes == 2
    ldr  h0, [_cpu, x12]
.else
    ldr  b0, [_cpu, x12]
.endif
.endm
.macro vex_st_put bytes, at
.if \bytes == 32
    stp  q0, q1, [\at]
.elseif \bytes == 16
    str  q0, [\at]
.elseif \bytes == 8
    str  d0, [\at]
.elseif \bytes == 4
    str  s0, [\at]
.elseif \bytes == 2
    str  h0, [\at]
.else
    str  b0, [\at]
.endif
.endm
.macro vex_st_i386 bytes
.gadget vex_st_i386_\bytes
    write_prep (\bytes * 8), vex_st_i386_\bytes
    vex_st_copy \bytes, 8
    vex_st_put \bytes, _xaddr
    write_done (\bytes * 8), vex_st_i386_\bytes
    gret 2
    write_bullshit (\bytes * 8), vex_st_i386_\bytes
.endm
.macro vex_st_amd64 bytes
.gadget vex_st_amd64_\bytes
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    amd64_vmem_addr
    amd64_vwrite_prep \bytes, vex_st_amd64_\bytes
    vex_st_copy \bytes, 24
    vex_st_put \bytes, x9
    amd64_vwrite_done \bytes, vex_st_amd64_\bytes
    gret 4
    amd64_vwrite_bullshit \bytes, vex_st_amd64_\bytes
.endm
.irp b, 1,2,4,8,16,32
vex_st_i386 \b
vex_st_amd64 \b
.endr

# A general register into VEX_STAGE, zero-extended to 16 bytes (VMOVD/VMOVQ
# xmm, r and the inserts): i386 one gadget per register, no operand; amd64
# [reg | 64-bit << 4] from amd64_regs (gen.c flushes the cache), gret 1.
.macro vex_gst_pair name, reg
.gadget vex_gst_\name
    mov  w9, \reg
    mov  x15, VEX_STAGE
    add  x15, _cpu, x15
    stp  x9, xzr, [x15]
    gret
.endm
.each_reg vex_gst_pair
.gadget vex_gst_amd64
    ldr  x8, [_ip]
    and  x9, x8, 15
    add  x10, _cpu, CPU_amd64_regs
    ldr  x9, [x10, x9, lsl 3]
    tbnz x8, 4, 1f
    mov  w9, w9
1:  mov  x15, VEX_STAGE
    add  x15, _cpu, x15
    stp  x9, xzr, [x15]
    gret 1
# The low dword or qword of a vector register to amd64_regs[reg]: [src off |
# reg << 16 | 64-bit << 20], gret 1. (i386 uses misc.S's vec_movd_from_*.)
.gadget vex_to_gpr_amd64
    ldr  x8, [_ip]
    and  x9, x8, 0xffff
    ldr  x10, [_cpu, x9]
    tbnz x8, 20, 1f
    mov  w10, w10
1:  ubfx x11, x8, 16, 4
    add  x12, _cpu, CPU_amd64_regs
    str  x10, [x12, x11, lsl 3]
    gret 1

# VZEROUPPER / VZEROALL: bits 511:128 (and for VZEROALL 127:0) of the first
# n registers zeroed. [n], gret 1.
.macro vex_zero name, all
.gadget vex_\name
    ldr  x8, [_ip]
    mov  x9, CPU_ymm_hi
    add  x9, _cpu, x9
    mov  x10, CPU_zmm_hi
    add  x10, _cpu, x10
    add  x11, _cpu, CPU_xmm
1:  stp  xzr, xzr, [x9], 16
    stp  xzr, xzr, [x10], 16
    stp  xzr, xzr, [x10], 16
.if \all
    stp  xzr, xzr, [x11], 16
.endif
    subs x8, x8, 1
    b.ne 1b
    gret 1
.endm
vex_zero zeroupper, 0
vex_zero zeroall, 1

# VINSERTF128/I128: dst = s1 with the 128-bit lane imm8 & 1 replaced by s2
# (an xmm, or staged m128). VEXTRACTF128/I128 to a register: dst = s2's lane
# imm8 & 1, the rest zeroed. VPERM2F128/I128: each dst lane imm8[1:0] /
# [5:4] of {s1 lo, s1 hi, s2 lo, s2 hi}, or zero for imm8 bit 3 / 7. All four
# sources are read before anything is written. Usual word, gret 1.
.macro vex_srcs4                        /* q0 s1 lo, q1 s1 hi, q2 s2 lo, q3 s2 hi */
    vex_ptrs 0
    ldr  q0, [x10]
    ldr  q2, [x14]
    vex_ptrs 1
    ldr  q1, [x10]
    ldr  q3, [x14]
.endm
.gadget vex_ins128
    ldr  x8, [_ip]
    vex_ptrs 0
    ldr  q0, [x10]
    ldr  q2, [x14]                       /* s2: its low 128 bits */
    vex_ptrs 1
    ldr  q1, [x10]
    tbnz x8, 16, 1f
    mov  v0.16b, v2.16b
    b    2f
1:  mov  v1.16b, v2.16b
2:  vex_ptrs 0
    str  q0, [x13]
    vex_ptrs 1
    str  q1, [x13]
    vex_zero_zmm
    gret 1
.gadget vex_ext128
    ldr  x8, [_ip]
    vex_ptrs 0
    ldr  q2, [x14]
    vex_ptrs 1
    tbz  x8, 16, 1f
    ldr  q2, [x14]
1:  stp  xzr, xzr, [x13]
    vex_ptrs 0
    str  q2, [x13]
    vex_zero_zmm
    gret 1
.macro vp2_lane sh, out
    ubfx x9, x8, (16 + \sh), 2
    tbz  x8, (19 + \sh), 1f
    movi v\out\().2d, 0
    b    5f
1:  cbnz x9, 2f
    mov  v\out\().16b, v0.16b
    b    5f
2:  cmp  x9, 1
    b.ne 3f
    mov  v\out\().16b, v1.16b
    b    5f
3:  cmp  x9, 2
    b.ne 4f
    mov  v\out\().16b, v2.16b
    b    5f
4:  mov  v\out\().16b, v3.16b
5:
.endm
.gadget vex_perm2x128
    ldr  x8, [_ip]
    vex_srcs4
    vp2_lane 0, 4
    vp2_lane 4, 5
    vex_ptrs 0
    str  q4, [x13]
    vex_ptrs 1
    str  q5, [x13]
    vex_zero_zmm
    gret 1

# VPERMQ/VPERMPD imm8 (a 32-byte tbl of s2, gen.c's index: [word][idx lo
# half: 2 quads][idx hi half: 2 quads], gret 5) and VPERMD/VPERMPS (each
# dword of dst = s2's dword s1.d[i] & 7).
.gadget vex_permq
    ldr  x8, [_ip]
    vex_srcs4
    mov  v0.16b, v2.16b
    mov  v1.16b, v3.16b
    ldr  q4, [_ip, 8]
    ldr  q5, [_ip, 24]
    tbl  v6.16b, {v0.16b, v1.16b}, v4.16b
    tbl  v7.16b, {v0.16b, v1.16b}, v5.16b
    vex_ptrs 0
    str  q6, [x13]
    vex_ptrs 1
    str  q7, [x13]
    vex_zero_zmm
    gret 5
.macro vpd_idx in, out                  /* bytes 4 * (d & 7) + {0,1,2,3} of each dword */
    movi v6.4s, 7
    and  v\out\().16b, v\in\().16b, v6.16b
    shl  v\out\().4s, v\out\().4s, 2
    adr  x9, vex_spread4
    ldr  q6, [x9]
    tbl  v\out\().16b, {v\out\().16b}, v6.16b
    adr  x9, vex_iota4
    ldr  q6, [x9]
    add  v\out\().16b, v\out\().16b, v6.16b
.endm
.gadget vex_permd
    ldr  x8, [_ip]
    vex_srcs4                            /* q0/q1 the indices (s1), q2/q3 the table (s2) */
    vpd_idx 0, 4
    vpd_idx 1, 5
    tbl  v4.16b, {v2.16b, v3.16b}, v4.16b
    tbl  v5.16b, {v2.16b, v3.16b}, v5.16b
    vex_ptrs 0
    str  q4, [x13]
    vex_ptrs 1
    str  q5, [x13]
    vex_zero_zmm
    gret 1
.balign 16
vex_spread4:
    .byte 0, 0, 0, 0, 4, 4, 4, 4, 8, 8, 8, 8, 12, 12, 12, 12
vex_iota4:
    .byte 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3
vex_spread8:
    .byte 0, 0, 0, 0, 0, 0, 0, 0, 8, 8, 8, 8, 8, 8, 8, 8
vex_iota8:
    .byte 0, 1, 2, 3, 4, 5, 6, 7, 0, 1, 2, 3, 4, 5, 6, 7

# VPERMILPS/PD by a control register (lane-local): q0 the table (s1), q1 the
# control (s2): dword i = s1.d[c.d[i] & 3]; qword i = s1.q[(c.q[i] >> 1) & 1].
.macro vpermil_body kind
.ifc \kind,ps
    movi v6.4s, 3
    and  v2.16b, v1.16b, v6.16b
    shl  v2.4s, v2.4s, 2
    adr  x9, vex_spread4
    ldr  q6, [x9]
    tbl  v2.16b, {v2.16b}, v6.16b
    adr  x9, vex_iota4
    ldr  q6, [x9]
    add  v2.16b, v2.16b, v6.16b
.else
    ushr v2.2d, v1.2d, 1
    mov  x9, 1
    dup  v6.2d, x9
    and  v2.16b, v2.16b, v6.16b
    shl  v2.2d, v2.2d, 3
    adr  x9, vex_spread8
    ldr  q6, [x9]
    tbl  v2.16b, {v2.16b}, v6.16b
    adr  x9, vex_iota8
    ldr  q6, [x9]
    add  v2.16b, v2.16b, v6.16b
.endif
    tbl  v0.16b, {v0.16b}, v2.16b
.endm
vex_l3 permilps_v, q0, vpermil_body, ps
vex_l3 permilpd_v, q0, vpermil_body, pd

# VPSLLVD/Q, VPSRLVD/Q, VPSRAVD: each lane of s1 shifted by the same lane of
# s2, a count past the width giving 0 (or the sign for VPSRAVD).
.macro vshv_body kind
.ifc \kind,srld
    movi v3.4s, 32
    umin v2.4s, v1.4s, v3.4s
    neg  v2.4s, v2.4s
    ushl v0.4s, v0.4s, v2.4s
.endif
.ifc \kind,slld
    movi v3.4s, 32
    umin v2.4s, v1.4s, v3.4s
    ushl v0.4s, v0.4s, v2.4s
.endif
.ifc \kind,srad
    movi v3.4s, 31
    umin v2.4s, v1.4s, v3.4s
    neg  v2.4s, v2.4s
    sshl v0.4s, v0.4s, v2.4s
.endif
.ifc \kind,srlq
    mov  x9, 63
    dup  v3.2d, x9
    cmhi v3.2d, v1.2d, v3.2d             /* count > 63 */
    neg  v2.2d, v1.2d
    ushl v0.2d, v0.2d, v2.2d
    bic  v0.16b, v0.16b, v3.16b
.endif
.ifc \kind,sllq
    mov  x9, 63
    dup  v3.2d, x9
    cmhi v3.2d, v1.2d, v3.2d
    ushl v0.2d, v0.2d, v1.2d
    bic  v0.16b, v0.16b, v3.16b
.endif
.endm
.irp k, srld,slld,srad,srlq,sllq
vex_l3 shv_\k, q0, vshv_body, \k
.endr

# ---- the width-changing ops ----
# Widening (VCVTPS2PD, VCVTDQ2PD, VPMOVSX/ZX): the source is s2's low 128
# bits (or the staged memory); the low half of dst is the body on it, the
# high half the body on it shifted down by \step bytes (what the low half
# consumed). The body reads v1 and leaves \res; v24 keeps the source.
.macro vex_widen name, step, res, body, args:vararg
.gadget vex_\name
    ldr  x8, [_ip]
    vex_ptrs 0
    ldr  q1, [x14]
    mov  v24.16b, v1.16b
    \body \args
    str  \res, [x13]
    vex_ptrs 1
    tbz  x8, 13, .Lwd_128\@
    ext  v1.16b, v24.16b, v24.16b, \step
    \body \args
    str  \res, [x13]
    b    .Lwd_end\@
.Lwd_128\@:
    stp  xzr, xzr, [x13]
.Lwd_end\@:
    vex_zero_zmm
    gret 1
.endm
# Narrowing (VCVTPD2PS, VCVT(T)PD2DQ): each source half gives 64 bits; the
# low half's go in dst's low qword, the high half's (VEX.256) above them, and
# bits 255:128 are zeroed.
.macro vex_narrow name, body
.gadget vex_\name
    ldr  x8, [_ip]
    vex_ptrs 0
    ldr  q1, [x14]
    \body
    mov  v24.d[0], v2.d[0]
    mov  v24.d[1], xzr
    tbz  x8, 13, .Lnw_128\@
    vex_ptrs 1
    ldr  q1, [x14]
    \body
    mov  v24.d[1], v2.d[0]
.Lnw_128\@:
    vex_ptrs 0
    str  q24, [x13]
    vex_ptrs 1
    stp  xzr, xzr, [x13]
    vex_zero_zmm
    gret 1
.endm
vex_widen xf_cvtps2pd, 8, q2, xfb_cvtps2pd

# ---- F16C ----
# VCVTPH2PS: exact; DAZ and FTZ do not apply, no DE; an SNaN is IE and comes
# out quieted with its payload -- fcvtl's own.
.macro xfb_cvtph2ps
    fcvtl v2.4s, v1.4h
.endm
vex_widen xf_cvtph2ps, 8, q2, xfb_cvtph2ps

# VCVTPS2PH: the four singles of v1 to halves in v2's low 64 bits. DAZ and DE
# on the inputs; the rounding imm8[1:0], or MXCSR.RC with imm8[2]; tininess
# after rounding (fixed up as xf_tiny does, with an input scaled by 2^8); FTZ
# does not apply. NaNs and overflow are fcvtn's own. w12: imm8.
.macro xfb_cvtps2ph
    xf_consts s
    movi v7.16b, 0xff
    xf_pre1n 4s
    mrs  x15, fpcr
    tbnz w12, 2, 1f
    and  w9, w12, 3                      /* x86 RC (N D U Z) to RMode (N P M Z) */
    lsl  w11, w9, 1
    orr  w9, w11, w9, lsr 1
    and  w9, w9, 3
    mov  x11, x15
    bfi  x11, x9, 22, 2
    msr  fpcr, x11
1:  mrs  x16, fpsr
    fcvtn v2.4h, v5.4s
    /* the halves that came out exactly +-2^-14 */
    mov  v21.16b, v2.16b
    bic  v21.8h, 0x80, lsl 8
    movi v31.8h, 0x04, lsl 8
    cmeq v21.8h, v21.8h, v31.8h
    umaxv b31, v21.16b
    fmov w9, s31
    cbz  w9, .Lph_done\@
    mrs  x17, fpsr
    mov  w9, ((8 + 127) << 23)
    dup  v30.4s, w9
    fmul v29.4s, v5.4s, v30.4s           /* exact: the result normal, rounded as unbounded */
    fcvtn v28.4h, v29.4s
    bic  v28.8h, 0x80, lsl 8
    movi v30.8h, 0x24, lsl 8             /* 2^(-14 + 8) */
    cmhi v22.8h, v30.8h, v28.8h
    and  v22.16b, v22.16b, v21.16b
    sxtl v27.4s, v21.4h                  /* the other lanes' own UFC */
    bic  v27.16b, v5.16b, v27.16b
    msr  fpsr, xzr
    fcvtn v28.4h, v27.4s
    mrs  x9, fpsr
    and  x9, x9, 8
    msr  fpcr, x15
    xf_tiny_tail h, 8h, 0
    b    .Lph_end\@
.Lph_done\@:
    msr  fpcr, x15
.Lph_end\@:
.endm
# [s1 = the source | dst (8-11) | L (13) | to VEX_STAGE (14) | imm8], gret 1.
# The memory form converts into VEX_STAGE for vex_st_* (a vex_wprobe_* first,
# so a fault leaves before MXCSR changes).
.gadget vex_cvtps2ph
    ldr  x8, [_ip]
    vex_ptrs 0
    ubfx w12, w8, 16, 8
    ldr  q1, [x10]
    xfb_cvtps2ph
    mov  v24.d[0], v2.d[0]
    mov  v24.d[1], xzr
    tbz  x8, 13, .Lcph_one
    vex_ptrs 1
    ubfx w12, w8, 16, 8
    ldr  q1, [x10]
    xfb_cvtps2ph
    mov  v24.d[1], v2.d[0]
.Lcph_one:
    tbz  x8, 14, .Lcph_reg
    mov  x15, VEX_STAGE
    add  x15, _cpu, x15
    str  q24, [x15]
    gret 1
.Lcph_reg:
    vex_ptrs 0
    str  q24, [x13]
    vex_ptrs 1
    stp  xzr, xzr, [x13]
    vex_zero_zmm
    gret 1

# ---- gathers (VPGATHERDD/DQ/QD/QQ, VGATHERDPS/DPD/QPS/QPD) ----
# Word: [dst (0-3) | mask (4-7) | index (8-11) | scale (12-13) | qword data
# (14) | qword index (15) | L (16)]. Restartable as x86's: each step finds
# the lowest element whose mask MSB is set (zeroing the clear ones it
# passes), loads it, writes dst's element and zeroes the mask's, so a fault
# leaves every element below it done, its mask element zero, and the rest as
# they were; nothing is kept across the TLB slow path. Done, the
# mask is zeroed whole and dst past its elements (and both zmm_hi).
.macro vg_elem out, reg, boff           /* \out = &register \reg's byte \boff. Clobbers x15 */
    mov  x15, CPU_xmm
    cmp  \boff, 16
    b.lo .Lvge\@
    mov  x15, (CPU_ymm_hi - 16)
.Lvge\@:
    add  x15, x15, \boff, uxtw
    add  x15, x15, \reg, uxtw 4
    add  \out, _cpu, x15
.endm
# w12: the lowest element whose mask MSB is set, or w13 (the count) if none;
# w16: log2 of the element's bytes. Clobbers x11, x14, x15, x17.
.macro vg_find
    ubfx w16, w8, 14, 1
    add  w16, w16, 2
    ubfx w11, w8, 16, 1
    mov  w13, 2
    lsl  w13, w13, w11
    tst  w8, 0xc000
    b.ne .Lvgf_n\@
    lsl  w13, w13, 1                     /* dword index, dword data: 4 or 8 */
.Lvgf_n\@:
    ubfx w14, w8, 4, 4
    mov  w12, 0
.Lvgf_loop\@:
    cmp  w12, w13
    b.hs .Lvgf_done\@
    add  w11, w12, 1
    lsl  w11, w11, w16
    sub  w11, w11, 1                     /* the element's top byte */
    vg_elem x17, w14, w11
    ldrb w17, [x17]
    tbnz w17, 7, .Lvgf_done\@
    lsl  w11, w12, w16                   /* not gathered: done, so its mask element zeroed */
    vg_elem x17, w14, w11
    cmp  w16, 3
    b.eq .Lvgf_q\@
    str  wzr, [x17]
    b    .Lvgf_next\@
.Lvgf_q\@:
    str  xzr, [x17]
.Lvgf_next\@:
    add  w12, w12, 1
    b    .Lvgf_loop\@
.Lvgf_done\@:
.endm
.macro vg_index                          /* x10 = element w12's index, sign-extended, scaled */
    ubfx w14, w8, 8, 4
    tbnz w8, 15, .Lvgi_q\@
    lsl  w11, w12, 2
    vg_elem x17, w14, w11
    ldrsw x10, [x17]
    b    .Lvgi_done\@
.Lvgi_q\@:
    lsl  w11, w12, 3
    vg_elem x17, w14, w11
    ldr  x10, [x17]
.Lvgi_done\@:
    ubfx w11, w8, 12, 2
    lsl  x10, x10, x11
.endm
.macro vg_store                          /* dst's element w12 = x10; the mask's zeroed */
    lsl  w11, w12, w16
    and  w14, w8, 15
    vg_elem x17, w14, w11
    cmp  w16, 3
    b.eq .Lvgs_q\@
    str  w10, [x17]
    ubfx w14, w8, 4, 4
    vg_elem x17, w14, w11
    str  wzr, [x17]
    b    .Lvgs_done\@
.Lvgs_q\@:
    str  x10, [x17]
    ubfx w14, w8, 4, 4
    vg_elem x17, w14, w11
    str  xzr, [x17]
.Lvgs_done\@:
.endm
.macro vg_finish                         /* (after vg_find found none: w13 count, w16 log2 size) */
    lsl  w11, w13, w16                   /* dst's bytes: 8, 16 or 32 */
    and  w14, w8, 15
    vex_addr x9, x14, CPU_xmm, 4
    vex_addr x10, x14, CPU_ymm_hi, 4
    cmp  w11, 16
    b.hi .Lvgfin_mask\@
    stp  xzr, xzr, [x10]
    b.eq .Lvgfin_mask\@
    str  xzr, [x9, 8]
.Lvgfin_mask\@:
    vex_addr x9, x14, CPU_zmm_hi, 5
    stp  xzr, xzr, [x9]
    stp  xzr, xzr, [x9, 16]
    ubfx w14, w8, 4, 4
    vex_addr x9, x14, CPU_xmm, 4
    stp  xzr, xzr, [x9]
    vex_addr x9, x14, CPU_ymm_hi, 4
    stp  xzr, xzr, [x9]
    vex_addr x9, x14, CPU_zmm_hi, 5
    stp  xzr, xzr, [x9]
    stp  xzr, xzr, [x9, 16]
.endm
# i386: _addr = base + disp (+ segment), gen.c's; [orig_ip][word], gret 2.
.gadget vex_gather_i386
    mov  x15, VEX_STAGE
    add  x15, _cpu, x15
    str  _addr, [x15]
.Lvgi386_loop:
    ldr  x8, [_ip, 8]
    vg_find
    cmp  w12, w13
    b.hs .Lvgi386_fin
    vg_index
    mov  x15, VEX_STAGE
    add  x15, _cpu, x15
    ldr  w9, [x15]
    add  _addr, w9, w10                  /* (32-bit: the address wraps) */
    tbnz w8, 14, .Lvgi386_q
    read_prep 32, vex_gather_i386_4
    ldr  w10, [_xaddr]
    b    .Lvgi386_got
.Lvgi386_q:
    read_prep 64, vex_gather_i386_8
    ldr  x10, [_xaddr]
.Lvgi386_got:
    ldr  x8, [_ip, 8]
    vg_find
    vg_store
    b    .Lvgi386_loop
.Lvgi386_fin:
    vg_finish
    gret 2
    read_bullshit 32, vex_gather_i386_4
    read_bullshit 64, vex_gather_i386_8
# amd64: [meta (no index: base + disp + segment)][disp][next_ip][word], gret 4.
.gadget vex_gather_amd64
.Lvga_loop:
    ldr  x8, [_ip, 24]
    vg_find
    cmp  w12, w13
    b.hs .Lvga_fin
    vg_index
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    amd64_vmem_addr
    add  x9, x9, x10
    ldr  x8, [_ip, 24]
    tbnz w8, 14, .Lvga_q
    amd64_vread_prep 4, vex_gather_amd64_4
    ldr  w10, [x9]
    b    .Lvga_got
.Lvga_q:
    amd64_vread_prep 8, vex_gather_amd64_8
    ldr  x10, [x9]
.Lvga_got:
    ldr  x8, [_ip, 24]
    vg_find
    vg_store
    b    .Lvga_loop
.Lvga_fin:
    vg_finish
    gret 4
    amd64_vread_bullshit 4, vex_gather_amd64_4
    amd64_vread_bullshit 8, vex_gather_amd64_8

# A write probe: the store's fault, and nothing written. i386 [orig_ip],
# gret 1; amd64 [meta][disp][next_ip], gret 3.
.macro vex_wprobe_i386 bytes
.gadget vex_wprobe_i386_\bytes
    write_prep (\bytes * 8), vex_wprobe_i386_\bytes
back_write_done_vex_wprobe_i386_\bytes :   /* (write_done's label; no write-back) */
    gret 1
    write_bullshit (\bytes * 8), vex_wprobe_i386_\bytes
.endm
.macro vex_wprobe_amd64 bytes
.gadget vex_wprobe_amd64_\bytes
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    amd64_vmem_addr
    amd64_vwrite_prep \bytes, vex_wprobe_amd64_\bytes
    gret 3
    amd64_vwrite_bullshit \bytes, vex_wprobe_amd64_\bytes
.endm
.irp b, 8,16
vex_wprobe_i386 \b
vex_wprobe_amd64 \b
.endr
vex_widen xf_cvtdq2pd, 8, q2, xfb_cvtdq2pd
vex_narrow xf_cvtpd2ps, xfb_cvtpd2ps
vex_narrow xf_cvtpd2dq, xfb_cvtpd2dq
vex_narrow xf_cvttpd2dq, xfb_cvttpd2dq
vex_l3 xf_cvtss2sd, q2, xfb_cvtss2sd
vex_l3 xf_cvtsd2ss, q2, xfb_cvtsd2ss
vex_widen vi_pmovsxbw, 8, q0, vmovx_chain, s, 8h, 8b
vex_widen vi_pmovsxbd, 4, q0, vmovx_chain, s, 8h, 8b, 4s, 4h
vex_widen vi_pmovsxbq, 2, q0, vmovx_chain, s, 8h, 8b, 4s, 4h, 2d, 2s
vex_widen vi_pmovsxwd, 8, q0, vmovx_chain, s, 4s, 4h
vex_widen vi_pmovsxwq, 4, q0, vmovx_chain, s, 4s, 4h, 2d, 2s
vex_widen vi_pmovsxdq, 8, q0, vmovx_chain, s, 2d, 2s
vex_widen vi_pmovzxbw, 8, q0, vmovx_chain, z, 8h, 8b
vex_widen vi_pmovzxbd, 4, q0, vmovx_chain, z, 8h, 8b, 4s, 4h
vex_widen vi_pmovzxbq, 2, q0, vmovx_chain, z, 8h, 8b, 4s, 4h, 2d, 2s
vex_widen vi_pmovzxwd, 8, q0, vmovx_chain, z, 4s, 4h
vex_widen vi_pmovzxwq, 4, q0, vmovx_chain, z, 4s, 4h, 2d, 2s
vex_widen vi_pmovzxdq, 8, q0, vmovx_chain, z, 2d, 2s

# ---- to and from general registers, and the flags ----
# Results for a general register go to amd64_regs[g] (amd64: the register
# itself, the cache flushed; i386: a slot misc.S's vec_gpr_out_* then
# copies). Word as the frames', g in bits 8-11 instead of dst.
.macro vex_gptr out, slot               /* \out = &amd64_regs[the word's bits 8-11] */
    ubfx \slot, x8, 8, 4
    add  \out, _cpu, CPU_amd64_regs
    add  \out, \out, \slot, lsl 3
.endm
# VCVT(T)SS2SI / SD2SI: s2's low lane to g (32 or 64 bits).
.macro vex_cvt2gpr name, t, iw, trunc, a
.gadget vex_\name
    ldr  x8, [_ip]
    vex_ptrs 0
    ldr  q1, [x14]
    xf_consts \t
    xf_daz \a
    xf_f2i_scalar \t, \iw, \trunc
.if \iw == 32
    mov  w12, w12
.endif
    vex_gptr x13, x15
    str  x12, [x13]
    gret 1
.endm
.irp tr, 0, 1
  .if \tr
vex_cvt2gpr xf_cvttss2si32, s, 32, 1, 4s
vex_cvt2gpr xf_cvttss2si64, s, 64, 1, 4s
vex_cvt2gpr xf_cvttsd2si32, d, 32, 1, 2d
vex_cvt2gpr xf_cvttsd2si64, d, 64, 1, 2d
  .else
vex_cvt2gpr xf_cvtss2si32, s, 32, 0, 4s
vex_cvt2gpr xf_cvtss2si64, s, 64, 0, 4s
vex_cvt2gpr xf_cvtsd2si32, d, 32, 0, 2d
vex_cvt2gpr xf_cvtsd2si64, d, 64, 0, 2d
  .endif
.endr
# VMOVMSKPS/PD, VPMOVMSKB: the sign bits of s2's elements (both halves for
# VEX.256) to g, zero-extended.
.macro vmsk_half kind, out
.ifc \kind,ps
    ushr v2.4s, v1.4s, 31
    usra v2.2d, v2.2d, 31
    umov w9, v2.b[0]
    umov w11, v2.b[8]
    orr  \out, w9, w11, lsl 2
.endif
.ifc \kind,pd
    ushr v2.2d, v1.2d, 63
    umov w9, v2.b[0]
    umov w11, v2.b[8]
    orr  \out, w9, w11, lsl 1
.endif
.ifc \kind,b
    ushr v2.16b, v1.16b, 7
    usra v2.8h, v2.8h, 7
    usra v2.4s, v2.4s, 14
    usra v2.2d, v2.2d, 28
    umov w9, v2.b[0]
    umov w11, v2.b[8]
    orr  \out, w9, w11, lsl 8
.endif
.endm
.macro vex_movmsk name, kind, hshift
.gadget vex_\name
    ldr  x8, [_ip]
    vex_ptrs 0
    ldr  q1, [x14]
    vmsk_half \kind, w12
    tbz  x8, 13, 1f
    vex_ptrs 1
    ldr  q1, [x14]
    vmsk_half \kind, w13
    orr  w12, w12, w13, lsl \hshift
1:  vex_gptr x13, x15
    str  x12, [x13]
    gret 1
.endm
vex_movmsk movmskps, ps, 4
vex_movmsk movmskpd, pd, 2
vex_movmsk pmovmskb, b, 16

# VCOMISS/SD, VUCOMISS/SD: s1 (here the ModRM reg) against s2's low lane,
# into EFLAGS; nothing written but the flags (and MXCSR's).
.macro vex_comis name, t, a, cmp
.gadget vex_\name
    ldr  x8, [_ip]
    vex_ptrs 0
    ldr  q0, [x10]
    ldr  q1, [x14]
    xf_consts \t
    xf_lanes \t, 0
    xf_pre2 \a, 0
    \cmp \t\()4, \t\()5
    amd64_v_comis_flags
    gret 1
.endm
vex_comis xf_comiss, s, 4s, fcmpe
vex_comis xf_ucomiss, s, 4s, fcmp
vex_comis xf_comisd, d, 2d, fcmpe
vex_comis xf_ucomisd, d, 2d, fcmp
# VPTEST (all bits), VTESTPS/PD (the sign bits): ZF = (s1 AND s2) == 0,
# CF = (NOT s1 AND s2) == 0 over the whole width; AF OF PF SF cleared.
.macro vtest_half mask
    ldr  q0, [x10]
    ldr  q1, [x14]
.ifnb \mask
    and  v1.16b, v1.16b, \mask
.endif
    and  v2.16b, v0.16b, v1.16b
    bic  v3.16b, v1.16b, v0.16b
    orr  v20.16b, v20.16b, v2.16b
    orr  v21.16b, v21.16b, v3.16b
.endm
.macro vex_test name, kind
.gadget vex_\name
    ldr  x8, [_ip]
    movi v20.2d, 0
    movi v21.2d, 0
.ifc \kind,ps
    movi v22.4s, 0x80, lsl 24
.endif
.ifc \kind,pd
    mov  x9, 0x8000000000000000
    dup  v22.2d, x9
.endif
    vex_ptrs 0
.ifc \kind,all
    vtest_half
.else
    vtest_half v22.16b
.endif
    tbz  x8, 13, 1f
    vex_ptrs 1
.ifc \kind,all
    vtest_half
.else
    vtest_half v22.16b
.endif
1:  umaxv b2, v20.16b
    umaxv b3, v21.16b
    fmov w12, s2
    fmov w13, s3
    cmp  w12, 0
    cset w12, eq                         /* ZF */
    cmp  w13, 0
    cset w13, eq                         /* CF */
    ldr  w17, [_cpu, CPU_eflags]
    mov  w16, (1 | (1 << 2) | (1 << 4) | (1 << 6) | (1 << 7) | (1 << 11))
    bic  w17, w17, w16
    orr  w17, w17, w13
    orr  w17, w17, w12, lsl 6
    str  w17, [_cpu, CPU_eflags]
    strb w13, [_cpu, CPU_cf]
    strb wzr, [_cpu, CPU_of]
    ldrb w16, [_cpu, CPU_flags_res]
    bic  w16, w16, (ZF_RES | SF_RES | PF_RES | AF_OPS)
    strb w16, [_cpu, CPU_flags_res]
    gret 1
.endm
vex_test ptest, all
vex_test testps, ps
vex_test testpd, pd

# VCVTSI2SS/SD: the staged integer (s2's low 32 or 64 bits) converted into
# s1's low lane.
.macro vcvtsi_body t, iw
.if \iw == 32
    fmov w12, s1
    scvtf \t\()2, w12
.else
    fmov x12, d1
    scvtf \t\()2, x12
.endif
    mov  v0.\t[0], v2.\t[0]
.endm
vex_l3 xf_cvtsi2ss32, q0, vcvtsi_body, s, 32
vex_l3 xf_cvtsi2ss64, q0, vcvtsi_body, s, 64
vex_l3 xf_cvtsi2sd32, q0, vcvtsi_body, d, 32
vex_l3 xf_cvtsi2sd64, q0, vcvtsi_body, d, 64

# VPINSRB/W/D/Q: s2's low element (staged from a register or memory) into
# lane imm8 (masked to the lane count) of s1.
.macro vins_body arr, n
    and  w12, w12, (\n - 1)
.ifc \arr,2d
    dup  v3.2d, x12
.else
    dup  v3.\arr, w12
.endif
    adr  x9, vex_lanes_\arr
    ldr  q4, [x9]
    cmeq v3.\arr, v3.\arr, v4.\arr
.ifc \arr,16b
    dup  v5.16b, v1.b[0]
.endif
.ifc \arr,8h
    dup  v5.8h, v1.h[0]
.endif
.ifc \arr,4s
    dup  v5.4s, v1.s[0]
.endif
.ifc \arr,2d
    dup  v5.2d, v1.d[0]
.endif
    bit  v0.16b, v5.16b, v3.16b
.endm
.balign 16
vex_lanes_16b: .byte 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
vex_lanes_8h: .hword 0, 1, 2, 3, 4, 5, 6, 7
vex_lanes_4s: .word 0, 1, 2, 3
vex_lanes_2d: .quad 0, 1
vex_l3 pinsrb, q0, vins_body, 16b, 16
vex_l3 pinsrw, q0, vins_body, 8h, 8
vex_l3 pinsrd, q0, vins_body, 4s, 4
vex_l3 pinsrq, q0, vins_body, 2d, 2
# VINSERTPS: s2's dword imm8[7:6] (memory: gen.c clears those bits) into s1's
# lane imm8[5:4], then the lanes in imm8[3:0] zeroed.
.macro vinsps_body
    ubfx w9, w12, 6, 2
    dup  v3.4s, w9
    adr  x9, vex_lanes_4s
    ldr  q4, [x9]
    cmeq v3.4s, v3.4s, v4.4s
    and  v5.16b, v1.16b, v3.16b
    addv s5, v5.4s                       /* the chosen dword (one lane is nonzero) */
    dup  v5.4s, v5.s[0]
    ubfx w9, w12, 4, 2
    dup  v3.4s, w9
    cmeq v3.4s, v3.4s, v4.4s
    bit  v0.16b, v5.16b, v3.16b
    and  w9, w12, 15
    dup  v3.4s, w9
    movi v6.4s, 1
    ushl v6.4s, v6.4s, v4.4s             /* 1 2 4 8 */
    cmtst v3.4s, v3.4s, v6.4s
    bic  v0.16b, v0.16b, v3.16b
.endm
vex_l3 insertps, q0, vinsps_body

# VBLENDVPS/PD, VPBLENDVB: dst = s1 with the elements whose mask element (the
# register in imm8[7:4]) has its sign set taken from s2.
.macro vex_blendv name, arr
.gadget vex_\name
    ldr  x8, [_ip]
    ubfx x12, x8, 20, 4                  /* the mask register */
    vex_ptrs 0
    ldr  q0, [x10]
    ldr  q1, [x14]
    vex_addr x9, x12, CPU_xmm, 4
    ldr  q3, [x9]
    cmlt v3.\arr, v3.\arr, 0
    bit  v0.16b, v1.16b, v3.16b
    str  q0, [x13]
    vex_ptrs 1
    tbz  x8, 13, .Lbv_128\@
    ldr  q0, [x10]
    ldr  q1, [x14]
    ubfx x12, x8, 20, 4
    vex_addr x9, x12, CPU_ymm_hi, 4
    ldr  q3, [x9]
    cmlt v3.\arr, v3.\arr, 0
    bit  v0.16b, v1.16b, v3.16b
    str  q0, [x13]
    b    .Lbv_end\@
.Lbv_128\@:
    stp  xzr, xzr, [x13]
.Lbv_end\@:
    vex_zero_zmm
    gret 1
.endm
vex_blendv blendvps, 4s
vex_blendv blendvpd, 2d
vex_blendv pblendvb, 16b

# Bits 511:128 of register n zeroed (after a VEX.128 write by an SSE gadget:
# VPCMPESTRM/ISTRM's xmm0). [n], gret 1.
.gadget vex_zero_hi
    ldr  x8, [_ip]
    vex_addr x9, x8, CPU_ymm_hi, 4
    stp  xzr, xzr, [x9]
    vex_addr x9, x8, CPU_zmm_hi, 5
    stp  xzr, xzr, [x9]
    stp  xzr, xzr, [x9, 16]
    gret 1

# ---- VMASKMOVPS/PD, VPMASKMOVD/Q: masked loads and stores ----
# Element by element: an element whose mask element (the VEX.vvvv register)
# has its sign clear is neither read nor written, and can not fault; a load
# gives it 0. The mask and the store data are copied into the frame's buffer
# first (VEX_MASK, VEX_STAGE), so the destination may be either. Word after
# the address words: [n (0-3) | dst/data reg (8-11) | mask reg (12-15) |
# VEX.L (16)].
#define VEX_MASK (LOCAL_value + 320)
.macro vex_copy_reg off, reg_field, dst  /* 32 bytes of register (the word's \reg_field) to \dst (frame) */
    ubfx x9, \off, \reg_field, 4
    vex_addr x15, x9, CPU_xmm, 4
    ldr  q0, [x15]
    vex_addr x15, x9, CPU_ymm_hi, 4
    ldr  q1, [x15]
    mov  x15, \dst
    add  x15, _cpu, x15
    stp  q0, q1, [x15]
.endm
.macro vex_mask_bit idx, esz, out        /* \out = the mask element \idx's sign */
    mov  x15, VEX_MASK + \esz - 1
    add  x15, _cpu, x15
    add  x15, x15, \idx, lsl #(\esz / 4 + 1)
    ldrb \out, [x15]
.endm
.macro vex_store_dst word                 /* VEX_STAGE to the dst register, L-wide, the rest zeroed */
    mov  x15, VEX_STAGE
    add  x15, _cpu, x15
    ldp  q0, q1, [x15]
    ubfx x9, \word, 8, 4
    vex_addr x15, x9, CPU_xmm, 4
    str  q0, [x15]
    vex_addr x15, x9, CPU_ymm_hi, 4
    tbnz \word, 16, 1f
    movi v1.2d, 0
1:  str  q1, [x15]
    vex_addr x15, x9, CPU_zmm_hi, 5
    stp  xzr, xzr, [x15]
    stp  xzr, xzr, [x15, 16]
.endm

.macro vex_msk_i386 esz
.gadget vex_mskld_i386_\esz             /* [orig_ip][word], gret 2 */
    ldr  x8, [_ip, 8]
    vex_copy_reg x8, 12, VEX_MASK
    mov  x15, VEX_STAGE
    add  x15, _cpu, x15
    stp  xzr, xzr, [x15]
    stp  xzr, xzr, [x15, 16]
    mov  w13, _addr
    and  x11, x8, 15
    mov  x12, 0
1:  vex_mask_bit x12, \esz, w9
    tbz  w9, 7, 2f
    add  _addr, w13, w12, lsl #(\esz / 4 + 1)
    read_prep (\esz * 8), vex_mskld_i386_\esz
.if \esz == 4
    ldr  w9, [_xaddr]
    mov  x15, VEX_STAGE
    add  x15, _cpu, x15
    str  w9, [x15, x12, lsl 2]
.else
    ldr  x9, [_xaddr]
    mov  x15, VEX_STAGE
    add  x15, _cpu, x15
    str  x9, [x15, x12, lsl 3]
.endif
2:  add  x12, x12, 1
    cmp  x12, x11
    b.lo 1b
    ldr  x8, [_ip, 8]
    vex_store_dst x8
    gret 2
    read_bullshit (\esz * 8), vex_mskld_i386_\esz
.gadget vex_mskst_i386_\esz             /* [orig_ip][word], gret 2 */
    ldr  x8, [_ip, 8]
    vex_copy_reg x8, 12, VEX_MASK
    vex_copy_reg x8, 8, VEX_STAGE
    mov  w13, _addr
    and  x11, x8, 15
    mov  x12, 0
1:  vex_mask_bit x12, \esz, w9
    tbz  w9, 7, 2f
    add  _addr, w13, w12, lsl #(\esz / 4 + 1)
    write_prep (\esz * 8), vex_mskst_i386_\esz
    mov  x15, VEX_STAGE
    add  x15, _cpu, x15
.if \esz == 4
    ldr  w9, [x15, x12, lsl 2]
    str  w9, [_xaddr]
.else
    ldr  x9, [x15, x12, lsl 3]
    str  x9, [_xaddr]
.endif
    write_done (\esz * 8), vex_mskst_i386_\esz
2:  add  x12, x12, 1
    cmp  x12, x11
    b.lo 1b
    gret 2
    write_bullshit (\esz * 8), vex_mskst_i386_\esz
.endm
vex_msk_i386 4
vex_msk_i386 8

# amd64: [meta][disp][next_ip][word], gret 4; the register cache is flushed,
# so x20-x22 hold the loop (they survive the miss path's C call).
.macro vex_msk_amd64 esz
.gadget vex_mskld_amd64_\esz
    ldr  x8, [_ip, 24]
    vex_copy_reg x8, 12, VEX_MASK
    mov  x15, VEX_STAGE
    add  x15, _cpu, x15
    stp  xzr, xzr, [x15]
    stp  xzr, xzr, [x15, 16]
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    amd64_vmem_addr
    mov  x20, x9
    ldr  x8, [_ip, 24]
    and  x22, x8, 15
    mov  x21, 0
1:  vex_mask_bit x21, \esz, w9
    tbz  w9, 7, 2f
    add  x9, x20, x21, lsl #(\esz / 4 + 1)
    amd64_vread_prep \esz, vex_mskld_amd64_\esz
    mov  x15, VEX_STAGE
    add  x15, _cpu, x15
.if \esz == 4
    ldr  w8, [x9]
    str  w8, [x15, x21, lsl 2]
.else
    ldr  x8, [x9]
    str  x8, [x15, x21, lsl 3]
.endif
2:  add  x21, x21, 1
    cmp  x21, x22
    b.lo 1b
    ldr  x8, [_ip, 24]
    vex_store_dst x8
    gret 4
    amd64_vread_bullshit \esz, vex_mskld_amd64_\esz
.gadget vex_mskst_amd64_\esz
    ldr  x8, [_ip, 24]
    vex_copy_reg x8, 12, VEX_MASK
    vex_copy_reg x8, 8, VEX_STAGE
    ldr  x8, [_ip]
    ldr  x9, [_ip, 8]
    amd64_vmem_addr
    mov  x20, x9
    ldr  x8, [_ip, 24]
    and  x22, x8, 15
    mov  x21, 0
1:  vex_mask_bit x21, \esz, w9
    tbz  w9, 7, 2f
    add  x9, x20, x21, lsl #(\esz / 4 + 1)
    amd64_vwrite_prep \esz, vex_mskst_amd64_\esz
    mov  x15, VEX_STAGE
    add  x15, _cpu, x15
.if \esz == 4
    ldr  w8, [x15, x21, lsl 2]
    str  w8, [x9]
.else
    ldr  x8, [x15, x21, lsl 3]
    str  x8, [x9]
.endif
    amd64_vwrite_done \esz, vex_mskst_amd64_\esz
2:  add  x21, x21, 1
    cmp  x21, x22
    b.lo 1b
    gret 4
    amd64_vwrite_bullshit \esz, vex_mskst_amd64_\esz
.endm
vex_msk_amd64 4
vex_msk_amd64 8

# ---- BMI1/BMI2 (VEX-encoded general-register ops) ----
# On amd64_regs slots (amd64: the registers themselves, the cache flushed;
# i386: slots gen.c fills from and empties into its registers). Word: [op
# (0-3) | dst (4-7) | a (8-11) | b (12-15) | 64-bit (16) | b staged (17) |
# dst2 (20-23) | imm8 (24-31)], gret 1. ops: 0 ANDN (~a & b), 1 BLSR, 2 BLSMSK,
# 3 BLSI (of b), 4 BZHI (b, index a), 5 PEXT (a, mask b), 6 PDEP, 7 MULX
# (a * b: high to dst, low to dst2, the high winning when they are one), 8
# BEXTR (b, control a), 9 SHLX, 10 SARX, 11 SHRX (b by a), 12 RORX (b by
# imm8). The flags (ops 0-4 and 8), as an AMD Ryzen sets them (camd): OF 0,
# PF ZF from the result, SF from it but 0 for BEXTR, AF 0 but 1 for BEXTR,
# CF: ANDN 0, BLSR/BLSMSK b == 0, BLSI b != 0, BZHI index >= width, BEXTR 0.
.gadget vex_bmi
    ldr  x8, [_ip]
    add  x14, _cpu, CPU_amd64_regs
    ubfx x9, x8, 8, 4
    ldr  x10, [x14, x9, lsl 3]            /* a */
    ubfx x9, x8, 12, 4
    ldr  x11, [x14, x9, lsl 3]            /* b */
    tbz  x8, 17, 1f
    mov  x9, VEX_STAGE
    ldr  x11, [_cpu, x9]
1:  mov  x13, 64                          /* the width */
    tbnz x8, 16, 2f
    mov  w10, w10
    mov  w11, w11
    mov  x13, 32
2:  and  x9, x8, 15
    mov  w15, 0                           /* CF */
    mov  w16, 0                           /* 1: write the flags */
    cmp  x9, 1
    b.lo .Lbmi_andn
    b.eq .Lbmi_blsr
    cmp  x9, 3
    b.lo .Lbmi_blsmsk
    b.eq .Lbmi_blsi
    cmp  x9, 5
    b.lo .Lbmi_bzhi
    b.eq .Lbmi_pext
    cmp  x9, 7
    b.lo .Lbmi_pdep
    b.eq .Lbmi_mulx
    cmp  x9, 9
    b.lo .Lbmi_bextr
    b.eq .Lbmi_shlx
    cmp  x9, 11
    b.lo .Lbmi_sarx
    b.eq .Lbmi_shrx
    b    .Lbmi_rorx
.Lbmi_andn:
    bic  x12, x11, x10
    mov  w16, 1
    b    .Lbmi_res
.Lbmi_blsr:
    sub  x12, x11, 1
    and  x12, x12, x11
    cmp  x11, 0
    cset w15, eq
    mov  w16, 1
    b    .Lbmi_res
.Lbmi_blsmsk:
    sub  x12, x11, 1
    eor  x12, x12, x11
    cmp  x11, 0
    cset w15, eq
    mov  w16, 1
    b    .Lbmi_res
.Lbmi_blsi:
    neg  x12, x11
    and  x12, x12, x11
    cmp  x11, 0
    cset w15, ne
    mov  w16, 1
    b    .Lbmi_res
.Lbmi_bzhi:
    and  x9, x10, 0xff                    /* the index */
    mov  x12, x11
    mov  w16, 1
    cmp  x9, x13
    b.hs 3f
    mov  x14, -1
    lsl  x14, x14, x9
    bic  x12, x11, x14
    b    .Lbmi_res
3:  mov  w15, 1
    b    .Lbmi_res
.Lbmi_pext:                               /* the bits of a under b's ones, packed low */
    mov  x12, 0
    mov  x9, 0                            /* the next result bit */
4:  cbz  x11, .Lbmi_res
    rbit x14, x11
    clz  x14, x14                         /* the lowest mask bit */
    lsr  x15, x10, x14
    and  x15, x15, 1
    lsl  x15, x15, x9
    orr  x12, x12, x15
    add  x9, x9, 1
    sub  x15, x11, 1
    and  x11, x11, x15
    b    4b
.Lbmi_pdep:                               /* a's low bits deposited at b's ones */
    mov  x12, 0
    mov  x9, 0
5:  cbz  x11, .Lbmi_res
    rbit x14, x11
    clz  x14, x14
    lsr  x15, x10, x9
    and  x15, x15, 1
    lsl  x15, x15, x14
    orr  x12, x12, x15
    add  x9, x9, 1
    sub  x15, x11, 1
    and  x11, x11, x15
    b    5b
.Lbmi_mulx:
    tbz  x8, 16, 6f
    mul  x9, x10, x11
    umulh x12, x10, x11
    b    7f
6:  mul  x12, x10, x11                    /* 32 x 32: the product's halves */
    mov  w9, w12
    lsr  x12, x12, 32
7:  add  x14, _cpu, CPU_amd64_regs
    ubfx x15, x8, 20, 4
    str  x9, [x14, x15, lsl 3]            /* low first: high wins */
    b    .Lbmi_res
.Lbmi_bextr:
    and  x9, x10, 0xff                    /* start */
    ubfx x14, x10, 8, 8                   /* length */
    mov  x12, 0
    mov  w16, 2
    cmp  x9, x13
    b.hs .Lbmi_res
    lsr  x12, x11, x9
    cmp  x14, 64
    b.hs .Lbmi_res
    mov  x15, -1
    lsl  x15, x15, x14
    bic  x12, x12, x15
    mov  w15, 0
    b    .Lbmi_res
.Lbmi_shlx:
    sub  x9, x13, 1
    and  x9, x10, x9
    lsl  x12, x11, x9
    b    .Lbmi_res
.Lbmi_sarx:
    sub  x9, x13, 1
    and  x9, x10, x9
    tbnz x8, 16, 8f
    sxtw x11, w11
8:  asr  x12, x11, x9
    b    .Lbmi_res
.Lbmi_shrx:
    sub  x9, x13, 1
    and  x9, x10, x9
    lsr  x12, x11, x9
    b    .Lbmi_res
.Lbmi_rorx:
    ubfx x9, x8, 24, 8
    tbz  x8, 16, 9f
    ror  x12, x11, x9
    b    .Lbmi_res
9:  ror  w12, w11, w9
.Lbmi_res:
    tbnz x8, 16, 10f
    mov  w12, w12                         /* a 32-bit result, zero-extended */
10: add  x14, _cpu, CPU_amd64_regs
    ubfx x9, x8, 4, 4
    str  x12, [x14, x9, lsl 3]
    cbz  w16, 11f
    /* the flags */
    eor  w9, w12, w12, lsr 4
    eor  w9, w9, w9, lsr 2
    eor  w9, w9, w9, lsr 1
    mvn  w9, w9
    and  w9, w9, 1
    lsl  w17, w9, 2                       /* PF */
    orr  w17, w17, w15                    /* CF */
    tbz  x8, 16, 12f
    cmp  x12, 0
    cset w9, eq
    orr  w17, w17, w9, lsl 6              /* ZF */
    lsr  x9, x12, 63
    b    13f
12: cmp  w12, 0
    cset w9, eq
    orr  w17, w17, w9, lsl 6
    lsr  w9, w12, 31
13: cmp  w16, 2
    b.ne 14f
    orr  w17, w17, (1 << 4)               /* BEXTR: AF 1, SF 0 */
    b    15f
14: orr  w17, w17, w9, lsl 7              /* SF */
15: ldr  w9, [_cpu, CPU_eflags]
    mov  w10, (1 | (1 << 2) | (1 << 4) | (1 << 6) | (1 << 7) | (1 << 11))
    bic  w9, w9, w10
    orr  w9, w9, w17
    str  w9, [_cpu, CPU_eflags]
    and  w10, w17, 1
    strb w10, [_cpu, CPU_cf]
    strb wzr, [_cpu, CPU_of]
    ldrb w10, [_cpu, CPU_flags_res]
    bic  w10, w10, (ZF_RES | SF_RES | PF_RES | AF_OPS)
    strb w10, [_cpu, CPU_flags_res]
11: gret 1

# ---- FMA3: VFMADD/VFMSUB/VFNMADD/VFNMSUB/VFMADDSUB/VFMSUBADD 132/213/231 ----
# The frame gives q0 = op1 (the destination's old value), q1 = op2 (vvvv),
# q2 = op3 (r/m). The form picks A * B + C: 132 op1 * op3 + op2, 213 op2 *
# op1 + op3, 231 op2 * op3 + op1. As an AMD Ryzen does it (camd): one
# rounding (the host's fused ops); a NaN operand gives the first NaN of A,
# B, C, quieted -- order, not signalling first; IE for an SNaN anywhere,
# and for inf * 0 or inf - inf with no NaN operand (whose result is x86's
# default NaN), but not for inf * 0 + a QNaN; DAZ, DE and FTZ as the SSE
# ops. The host's own NaN rules never apply: NaN lanes are zeroed before
# the fused op, so it raises nothing for them.
.macro fma_pick form                    /* v4 A, v5 B, v6 C */
.ifc \form,132
    mov  v4.16b, v0.16b
    mov  v5.16b, v2.16b
    mov  v6.16b, v1.16b
.endif
.ifc \form,213
    mov  v4.16b, v1.16b
    mov  v5.16b, v0.16b
    mov  v6.16b, v2.16b
.endif
.ifc \form,231
    mov  v4.16b, v1.16b
    mov  v5.16b, v2.16b
    mov  v6.16b, v0.16b
.endif
.endm
.macro fma_op d, n, m, acc, t, a, p      /* v\d = v\acc + v\n * v\m, fused */
.if \p
  .ifnc \d,\acc
    mov  v\d\().16b, v\acc\().16b
  .endif
    fmla v\d\().\a, v\n\().\a, v\m\().\a
.else
    fmadd \t\d, \t\n, \t\m, \t\acc
.endif
.endm
.macro fma_signs op, t, at                 /* v24 A -> A', v26 C -> C' */
.ifc \op,msub
    fneg v26.\at, v26.\at
.endif
.ifc \op,nmadd
    fneg v24.\at, v24.\at
.endif
.ifc \op,nmsub
    fneg v24.\at, v24.\at
    fneg v26.\at, v26.\at
.endif
.ifc \op,maddsub                         /* the even lanes subtract */
  .ifc \t,s
    mov  x9, 0x80000000
    dup  v3.2d, x9
  .else
    mov  x9, 0x8000000000000000
    fmov d3, x9
  .endif
    eor  v26.16b, v26.16b, v3.16b
.endif
.ifc \op,msubadd                         /* the odd lanes subtract */
    mov  x9, 0x8000000000000000
  .ifc \t,s
    dup  v3.2d, x9
  .else
    movi v3.2d, 0
    mov  v3.d[1], x9
  .endif
    eor  v26.16b, v26.16b, v3.16b
.endif
.endm
.macro fma_body form, op, t, a, p
    xf_consts \t
    xf_lanes \t, \p
    fma_pick \form
    /* the NaN result (v23), the NaN lanes (v20) */
    orr  v23.16b, v6.16b, v19.16b
    xf_isnan v22, v5, \a
    orr  v3.16b, v5.16b, v19.16b
    bit  v23.16b, v3.16b, v22.16b
    mov  v20.16b, v22.16b
    xf_isnan v22, v4, \a
    orr  v3.16b, v4.16b, v19.16b
    bit  v23.16b, v3.16b, v22.16b
    orr  v20.16b, v20.16b, v22.16b
    xf_isnan v22, v6, \a
    orr  v20.16b, v20.16b, v22.16b
    /* IE for an SNaN: quiet compares (the host raises IOC for an SNaN only) */
    xc fcmeq, 21, 4, 4, \t, \a, \p
    xc fcmeq, 21, 5, 5, \t, \a, \p
    xc fcmeq, 21, 6, 6, \t, \a, \p
    /* denormal operands: DAZ, or DE where the lane has no NaN */
    xf_denorm v21, v4, \a, v3
    xf_denorm v22, v5, \a, v3
    orr  v21.16b, v21.16b, v22.16b
    xf_denorm v22, v6, \a, v3
    orr  v21.16b, v21.16b, v22.16b
    and  v21.16b, v21.16b, v7.16b
    xf_any v21, .Lfma_den\@
    ldr  w9, [_cpu, CPU_mxcsr]
    tbz  w9, 6, .Lfma_de\@
    orr  v18.16b, v16.16b, v17.16b
    xf_denorm v22, v4, \a, v3
    and  v22.16b, v22.16b, v18.16b
    bic  v4.16b, v4.16b, v22.16b
    xf_denorm v22, v5, \a, v3
    and  v22.16b, v22.16b, v18.16b
    bic  v5.16b, v5.16b, v22.16b
    xf_denorm v22, v6, \a, v3
    and  v22.16b, v22.16b, v18.16b
    bic  v6.16b, v6.16b, v22.16b
    b    .Lfma_den\@
.Lfma_de\@:
    bic  v21.16b, v21.16b, v20.16b
    xf_any v21, .Lfma_den\@
    /* an invalid lane (inf * 0, inf - inf) reports IE, not DE */
    mov  v24.16b, v4.16b
    mov  v26.16b, v6.16b
    fma_signs \op, \t, \a
    eor  v3.16b, v24.16b, v5.16b
    eor  v3.16b, v3.16b, v26.16b         /* the product's sign vs C's */
.ifc \t,s
    sshr v3.4s, v3.4s, 31
.else
    sshr v3.2d, v3.2d, 63
.endif
    shl  v24.\a, v24.\a, 1
    ushr v24.\a, v24.\a, 1
    shl  v25.\a, v5.\a, 1
    ushr v25.\a, v25.\a, 1
    shl  v26.\a, v26.\a, 1
    ushr v26.\a, v26.\a, 1
    cmeq v22.\a, v24.\a, 0              /* 0 * inf */
    cmeq v27.\a, v25.\a, v16.\a
    and  v22.16b, v22.16b, v27.16b
    cmeq v24.\a, v24.\a, v16.\a         /* inf * 0 */
    orr  v27.16b, v27.16b, v24.16b       /* (an infinite product) */
    cmeq v25.\a, v25.\a, 0
    and  v25.16b, v25.16b, v24.16b
    orr  v22.16b, v22.16b, v25.16b
    cmeq v26.\a, v26.\a, v16.\a         /* inf - inf */
    and  v26.16b, v26.16b, v27.16b
    and  v26.16b, v26.16b, v3.16b
    orr  v22.16b, v22.16b, v26.16b
    bic  v21.16b, v21.16b, v22.16b
    xf_any v21, .Lfma_den\@
    ldr  w9, [_cpu, CPU_mxcsr]
    orr  w9, w9, 2
    str  w9, [_cpu, CPU_mxcsr]
.Lfma_den\@:
    bic  v4.16b, v4.16b, v20.16b
    bic  v5.16b, v5.16b, v20.16b
    bic  v6.16b, v6.16b, v20.16b
    /* the fused op as C' + A' * B: v24 A', v5 B, v26 C' */
    mov  v24.16b, v4.16b
    mov  v26.16b, v6.16b
    fma_signs \op, \t, \a
    mrs  x16, fpsr                       /* the flags before the op */
    fma_op 2, 24, 5, 26, \t, \a, \p
    /* Underflow and FTZ decide tininess after rounding on x86 (to the
       precision, the exponent unbounded), before it on the host. They differ
       only where the result came out exactly +-the smallest normal: there,
       redo the op with A' or B (the smaller) and C' scaled by 2^64 -- exact,
       and normal, so its rounding is the unbounded one -- to see whether x86
       has it tiny; and again with those lanes zeroed for the other lanes'
       own UFC. */
    xf_minnorm v21, v2, \t
    and  v21.16b, v21.16b, v7.16b
    bic  v21.16b, v21.16b, v20.16b
    xf_any v21, .Lfma_tiny_done\@
    mrs  x17, fpsr                       /* the op's flags */
    /* v22: the candidate lanes x86 has tiny */
    fabs v27.\a, v24.\a
    fabs v28.\a, v5.\a
    fcmge v29.\a, v28.\a, v27.\a         /* |A'| <= |B|: scale A' */
    xf_pow2 v30, \t, 64
    fmul v27.\a, v24.\a, v30.\a
    fmul v28.\a, v5.\a, v30.\a
    bsl  v29.16b, v27.16b, v24.16b       /* A' scaled, or A' */
    fabs v27.\a, v24.\a
    fabs v31.\a, v5.\a
    fcmge v31.\a, v31.\a, v27.\a
    mov  v27.16b, v5.16b
    bif  v27.16b, v28.16b, v31.16b       /* B, or B scaled */
    fmul v28.\a, v26.\a, v30.\a          /* C' scaled */
    fma_op 28, 29, 27, 28, \t, \a, \p
    fabs v28.\a, v28.\a
    xf_pow2 v30, \t, -62
.ifc \t,d
    xf_pow2 v30, \t, -958
.endif
    fcmgt v22.\a, v30.\a, v28.\a         /* |scaled result| below 2^(emin + 64) */
    and  v22.16b, v22.16b, v21.16b
    /* the other lanes' UFC */
    bic  v27.16b, v24.16b, v21.16b
    bic  v28.16b, v5.16b, v21.16b
    bic  v29.16b, v26.16b, v21.16b
    msr  fpsr, xzr
    fma_op 29, 27, 28, 29, \t, \a, \p
    mrs  x9, fpsr
    and  x9, x9, 8                       /* UFC of the other lanes */
    and  x15, x16, 8
    orr  x9, x9, x15                     /* or from before the op */
    bic  x17, x17, 8
    orr  x17, x17, x9
    umaxv b31, v22.16b
    fmov w15, s31
    cbz  w15, 1f
    orr  x17, x17, 8                     /* a candidate x86 has tiny */
    ldr  w9, [_cpu, CPU_mxcsr]
    tbz  w9, 15, 1f
    xf_lanebits_sign v3, \t             /* FTZ: flushed, the sign kept */
    and  v3.16b, v3.16b, v22.16b
    and  v3.16b, v3.16b, v2.16b
    bic  v2.16b, v2.16b, v22.16b
    orr  v2.16b, v2.16b, v3.16b
    orr  w9, w9, 0x30
    str  w9, [_cpu, CPU_mxcsr]
1:  msr  fpsr, x17
.Lfma_tiny_done\@:
    xf_ftz \a
    /* an invalid result with no NaN operand: x86's default NaN */
    xf_isnan v21, v2, \a
    bic  v21.16b, v21.16b, v20.16b
    xf_defnan \t
    bit  v2.16b, v18.16b, v21.16b
    bit  v2.16b, v23.16b, v20.16b
    xf_merge \t, \p
.endm
.macro vex_fma name, args:vararg
.gadget vex_\name
    ldr  x8, [_ip]
    vex_ptrs 0
    ldr  q0, [x13]
    ldr  q1, [x10]
    ldr  q2, [x14]
    fma_body \args
    str  q2, [x13]
    vex_ptrs 1
    tbz  x8, 13, .Lfm_128\@
    ldr  q0, [x13]
    ldr  q1, [x10]
    ldr  q2, [x14]
    fma_body \args
    str  q2, [x13]
    b    .Lfm_end\@
.Lfm_128\@:
    stp  xzr, xzr, [x13]
.Lfm_end\@:
    vex_zero_zmm
    gret 1
.endm
vex_fma fma_madd132ps, 132, madd, s, 4s, 1
vex_fma fma_madd132pd, 132, madd, d, 2d, 1
vex_fma fma_msub132ps, 132, msub, s, 4s, 1
vex_fma fma_msub132pd, 132, msub, d, 2d, 1
vex_fma fma_nmadd132ps, 132, nmadd, s, 4s, 1
vex_fma fma_nmadd132pd, 132, nmadd, d, 2d, 1
vex_fma fma_nmsub132ps, 132, nmsub, s, 4s, 1
vex_fma fma_nmsub132pd, 132, nmsub, d, 2d, 1
vex_fma fma_maddsub132ps, 132, maddsub, s, 4s, 1
vex_fma fma_maddsub132pd, 132, maddsub, d, 2d, 1
vex_fma fma_msubadd132ps, 132, msubadd, s, 4s, 1
vex_fma fma_msubadd132pd, 132, msubadd, d, 2d, 1
vex_fma fma_madd132ss, 132, madd, s, 4s, 0
vex_fma fma_madd132sd, 132, madd, d, 2d, 0
vex_fma fma_msub132ss, 132, msub, s, 4s, 0
vex_fma fma_msub132sd, 132, msub, d, 2d, 0
vex_fma fma_nmadd132ss, 132, nmadd, s, 4s, 0
vex_fma fma_nmadd132sd, 132, nmadd, d, 2d, 0
vex_fma fma_nmsub132ss, 132, nmsub, s, 4s, 0
vex_fma fma_nmsub132sd, 132, nmsub, d, 2d, 0
vex_fma fma_madd213ps, 213, madd, s, 4s, 1
vex_fma fma_madd213pd, 213, madd, d, 2d, 1
vex_fma fma_msub213ps, 213, msub, s, 4s, 1
vex_fma fma_msub213pd, 213, msub, d, 2d, 1
vex_fma fma_nmadd213ps, 213, nmadd, s, 4s, 1
vex_fma fma_nmadd213pd, 213, nmadd, d, 2d, 1
vex_fma fma_nmsub213ps, 213, nmsub, s, 4s, 1
vex_fma fma_nmsub213pd, 213, nmsub, d, 2d, 1
vex_fma fma_maddsub213ps, 213, maddsub, s, 4s, 1
vex_fma fma_maddsub213pd, 213, maddsub, d, 2d, 1
vex_fma fma_msubadd213ps, 213, msubadd, s, 4s, 1
vex_fma fma_msubadd213pd, 213, msubadd, d, 2d, 1
vex_fma fma_madd213ss, 213, madd, s, 4s, 0
vex_fma fma_madd213sd, 213, madd, d, 2d, 0
vex_fma fma_msub213ss, 213, msub, s, 4s, 0
vex_fma fma_msub213sd, 213, msub, d, 2d, 0
vex_fma fma_nmadd213ss, 213, nmadd, s, 4s, 0
vex_fma fma_nmadd213sd, 213, nmadd, d, 2d, 0
vex_fma fma_nmsub213ss, 213, nmsub, s, 4s, 0
vex_fma fma_nmsub213sd, 213, nmsub, d, 2d, 0
vex_fma fma_madd231ps, 231, madd, s, 4s, 1
vex_fma fma_madd231pd, 231, madd, d, 2d, 1
vex_fma fma_msub231ps, 231, msub, s, 4s, 1
vex_fma fma_msub231pd, 231, msub, d, 2d, 1
vex_fma fma_nmadd231ps, 231, nmadd, s, 4s, 1
vex_fma fma_nmadd231pd, 231, nmadd, d, 2d, 1
vex_fma fma_nmsub231ps, 231, nmsub, s, 4s, 1
vex_fma fma_nmsub231pd, 231, nmsub, d, 2d, 1
vex_fma fma_maddsub231ps, 231, maddsub, s, 4s, 1
vex_fma fma_maddsub231pd, 231, maddsub, d, 2d, 1
vex_fma fma_msubadd231ps, 231, msubadd, s, 4s, 1
vex_fma fma_msubadd231pd, 231, msubadd, d, 2d, 1
vex_fma fma_madd231ss, 231, madd, s, 4s, 0
vex_fma fma_madd231sd, 231, madd, d, 2d, 0
vex_fma fma_msub231ss, 231, msub, s, 4s, 0
vex_fma fma_msub231sd, 231, msub, d, 2d, 0
vex_fma fma_nmadd231ss, 231, nmadd, s, 4s, 0
vex_fma fma_nmadd231sd, 231, nmadd, d, 2d, 0
vex_fma fma_nmsub231ss, 231, nmsub, s, 4s, 0
vex_fma fma_nmsub231sd, 231, nmsub, d, 2d, 0
