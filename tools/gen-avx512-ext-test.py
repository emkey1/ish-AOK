#!/usr/bin/env python3
# Generates tests/manual/x86/amd64_avx512_ext.c, or with --i386
# i386_avx512_ext.c (usage: this script [--i386] [--answers FILE] OUT.c).
#
# The AVX-512 integer extensions: VBMI's VPMULTISHIFTQB, VBMI2's VPSHLD/
# VPSHRD (imm8) and VPSHLDV/VPSHRDV, VNNI (VPDPBUSD(S), VPDPWSSD(S)), IFMA
# (VPMADD52LUQ/HUQ), BITALG (VPOPCNTB/W, VPSHUFBITQMB) and GFNI
# (GF2P8MULB, GF2P8AFFINEQB, GF2P8AFFINEINVQB), AES (VAES*, AESIMC,
# AESKEYGENASSIST) and PCLMULQDQ -- GFNI's, AES's and PCLMULQDQ's VEX and
# SSE forms too:
# VL 128/256/512, register,
# memory and {1toN} sources, unmasked, merge- and zero-masked; and
# encodings whose #UD-or-not is part of the answer. Besides random
# elements, cases fill the operands with saturating patterns (0x8000
# words, 0x7fffffff and 0x80000000 accumulators). The destination's 64
# bytes, k1, the memory and the signal are hashed per form and checked
# against Intel SDE's (`sde64 -ptr-raise -spr -- ./test hashes` on camd,
# passed back with --answers); the i386 test takes the amd64 answers.
import sys
I386 = '--i386' in sys.argv
args = [a for a in sys.argv[1:] if a != '--i386']
answers = {}
if '--answers' in args:
    i = args.index('--answers')
    for line in open(args[i + 1]):
        k, v = line.rsplit(' ', 1)
        answers[k] = v.strip()
    del args[i:i + 2]
NAME = 'i386_avx512_ext' if I386 else 'amd64_avx512_ext'
VLS = {128: 'xmm', 256: 'ymm', 512: 'zmm'}
MK = [('', ''), ('%{%%k1%}', 'm'), ('%{%%k1%}%{z%}', 'z')]
F = []
def add(name, asm):
    F.append((name, asm))
def three(op, bc, imm='', pre=''):
    """op [imm,] s2, s1, dst: register (each masking), memory, {1toN}; pre: an
    encoding pseudo-prefix ({evex}: GFNI has VEX forms too)"""
    for L, x in VLS.items():
        for mk, mn in MK:
            add(f'{op}{imm and " " + imm} {L} reg{mn}', f'{pre}{op} {imm and imm + ", "}%%{x}3, %%{x}2, %%{x}1{mk}')
        add(f'{op}{imm and " " + imm} {L} mem m', f'{pre}{op} {imm and imm + ", "}(%1), %%{x}2, %%{x}1%{{%%k1%}}')
        if bc:
            add(f'{op}{imm and " " + imm} {L} bcst z',
                f'{pre}{op} {imm and imm + ", "}(%1)%{{1to{L // (8 * bc)}%}}, %%{x}2, %%{x}1%{{%%k1%}}%{{z%}}')
three('vpmultishiftqb', 8)
for d in ('l', 'r'):
    for t, bits, bc in (('w', 16, 0), ('d', 32, 4), ('q', 64, 8)):
        for imm in sorted({0, 1, bits - 1, bits, 0x41, 0xff}):
            three(f'vpsh{d}d{t}', bc, f'${imm:#x}')
        three(f'vpsh{d}dv{t}', bc)
for op in ('vpdpbusd', 'vpdpbusds', 'vpdpwssd', 'vpdpwssds'):
    three(op, 4)
for op in ('vpmadd52luq', 'vpmadd52huq'):
    three(op, 8)
for op in ('vpopcntb', 'vpopcntw'):
    for L, x in VLS.items():
        for mk, mn in MK:
            add(f'{op} {L} reg{mn}', f'{op} %%{x}3, %%{x}1{mk}')
        add(f'{op} {L} mem m', f'{op} (%1), %%{x}1%{{%%k1%}}')
for L, x in VLS.items():
    add(f'vpshufbitqmb {L} reg', f'vpshufbitqmb %%{x}3, %%{x}2, %%k1')
    add(f'vpshufbitqmb {L} reg m', f'vpshufbitqmb %%{x}3, %%{x}2, %%k1%{{%%k1%}}')
    add(f'vpshufbitqmb {L} mem m', f'vpshufbitqmb (%1), %%{x}2, %%k1%{{%%k1%}}')
three('vgf2p8mulb', 0, pre='%{evex%} ')
for op in ('vgf2p8affineqb', 'vgf2p8affineinvqb'):
    for imm in (0, 0x63, 0xff):
        three(op, 8, f'${imm:#x}', pre='%{evex%} ')
# GFNI's VEX forms (VL 128/256; the destination zeroed above it) and its
# SSE forms (the destination is s1; above 128 bits untouched, as the SDM
# has it: SDE zeroes them for GFNI alone, though not for PADDB, PSHUFB or
# AESENC, so the harness checks those bits itself and hashes them as 0)
for op, imms in (('gf2p8mulb', ['']), ('gf2p8affineqb', ['$0x0', '$0x63']), ('gf2p8affineinvqb', ['$0x0', '$0x63'])):
    for imm in imms:
        ii = imm and imm + ', '
        n = f'{imm and " " + imm}'
        for L in (128, 256):
            x = VLS[L]
            add(f'{{vex}} v{op}{n} {L} reg', f'%{{vex%}} v{op} {ii}%%{x}3, %%{x}2, %%{x}1')
            add(f'{{vex}} v{op}{n} {L} mem', f'%{{vex%}} v{op} {ii}(%1), %%{x}2, %%{x}1')
        add(f'sse {op}{n} reg', f'{op} {ii}%%xmm3, %%xmm1')
        add(f'sse {op}{n} mem', f'{op} {ii}(%1), %%xmm1')
# AES and PCLMULQDQ: SSE (16-byte aligned memory, above 128 bits untouched),
# VEX.128, VEX.256 (VAES, VPCLMULQDQ), EVEX at every VL (unmasked: masking
# is #UD); AESIMC and AESKEYGENASSIST have only the SSE and VEX.128 forms
AES = ('aesenc', 'aesenclast', 'aesdec', 'aesdeclast')
PCL = ('$0x0', '$0x1', '$0x10', '$0x11', '$0xee', '$0x13')
for op in AES:
    add(f'sse {op} reg', f'{op} %%xmm3, %%xmm1')
    add(f'sse {op} mem', f'{op} (%1), %%xmm1')
    for L in (128, 256):
        x = VLS[L]
        add(f'{{vex}} v{op} {L} reg', f'%{{vex%}} v{op} %%{x}3, %%{x}2, %%{x}1')
        add(f'{{vex}} v{op} {L} mem', f'%{{vex%}} v{op} (%1), %%{x}2, %%{x}1')
    for L, x in VLS.items():
        add(f'{{evex}} v{op} {L} reg', f'%{{evex%}} v{op} %%{x}3, %%{x}2, %%{x}1')
        add(f'{{evex}} v{op} {L} mem', f'%{{evex%}} v{op} (%1), %%{x}2, %%{x}1')
for imm in PCL:
    add(f'sse pclmulqdq {imm} reg', f'pclmulqdq {imm}, %%xmm3, %%xmm1')
    add(f'sse pclmulqdq {imm} mem', f'pclmulqdq {imm}, (%1), %%xmm1')
    for L in (128, 256):
        x = VLS[L]
        add(f'{{vex}} vpclmulqdq {imm} {L} reg', f'%{{vex%}} vpclmulqdq {imm}, %%{x}3, %%{x}2, %%{x}1')
        add(f'{{vex}} vpclmulqdq {imm} {L} mem', f'%{{vex%}} vpclmulqdq {imm}, (%1), %%{x}2, %%{x}1')
    for L, x in VLS.items():
        add(f'{{evex}} vpclmulqdq {imm} {L} reg', f'%{{evex%}} vpclmulqdq {imm}, %%{x}3, %%{x}2, %%{x}1')
        add(f'{{evex}} vpclmulqdq {imm} {L} mem', f'%{{evex%}} vpclmulqdq {imm}, (%1), %%{x}2, %%{x}1')
add('sse aesimc reg', 'aesimc %%xmm3, %%xmm1')
add('sse aesimc mem', 'aesimc (%1), %%xmm1')
add('{vex} vaesimc reg', 'vaesimc %%xmm3, %%xmm1')
add('{vex} vaesimc mem', 'vaesimc (%1), %%xmm1')
for imm in ('$0x0', '$0x1', '$0x1b', '$0x36', '$0xff'):
    add(f'sse aeskeygenassist {imm} reg', f'aeskeygenassist {imm}, %%xmm3, %%xmm1')
    add(f'sse aeskeygenassist {imm} mem', f'aeskeygenassist {imm}, (%1), %%xmm1')
    add(f'{{vex}} vaeskeygenassist {imm} reg', f'vaeskeygenassist {imm}, %%xmm3, %%xmm1')
    add(f'{{vex}} vaeskeygenassist {imm} mem', f'vaeskeygenassist {imm}, (%1), %%xmm1')
if not I386:
    add('vaesenc 512 hi', 'vmovdqa64 %%zmm2, %%zmm25\\n vmovdqa64 %%zmm3, %%zmm30\\n vaesenc %%zmm30, %%zmm25, %%zmm17\\n '
        'vmovdqa64 %%zmm17, %%zmm1')
    add('vpclmulqdq 512 hi', 'vmovdqa64 %%zmm2, %%zmm25\\n vmovdqa64 %%zmm3, %%zmm30\\n vpclmulqdq $0x10, %%zmm30, %%zmm25, %%zmm17\\n '
        'vmovdqa64 %%zmm17, %%zmm1')
# the EVEX gaps the VEX cutover's sweep found: AVX512DQ's float logic,
# VMOVSLDUP/SHDUP/DDUP, VPBROADCASTMB2Q/MW2D
for op in ('vandps', 'vandnps', 'vorps', 'vxorps'):
    three(op, 4)
for op in ('vandpd', 'vandnpd', 'vorpd', 'vxorpd'):
    three(op, 8)
for op in ('vmovsldup', 'vmovshdup', 'vmovddup'):
    for L, x in VLS.items():
        for mk, mn in MK:
            add(f'{op} {L} reg{mn}', f'%{{evex%}} {op} %%{x}3, %%{x}1{mk}')
            add(f'{op} {L} mem{mn}', f'%{{evex%}} {op} (%1), %%{x}1{mk}')
for L, x in VLS.items():
    add(f'vpbroadcastmb2q {L}', f'vpbroadcastmb2q %%k1, %%{x}1')
    add(f'vpbroadcastmw2d {L}', f'vpbroadcastmw2d %%k1, %%{x}1')
# the EVEX.128 element moves ({evex}: gas would pick VEX; and a form on
# the registers 16-31): VMOVSS/SD, the half moves, VMOVD/VMOVQ, VPINSR*,
# VPEXTR*, VEXTRACTPS, VINSERTPS; a general register is eax/rax
E = '%{evex%} '
HI = 'vmovdqa64 %%zmm1, %%zmm17\\n vmovdqa64 %%zmm2, %%zmm25\\n vmovdqa64 %%zmm3, %%zmm30\\n '   # (seeded)
R6 = 'e' if I386 else 'r'
for op in ('vmovss', 'vmovsd'):
    for mk, mn in MK:
        add(f'{op} reg{mn}', f'{E}{op} %%xmm3, %%xmm2, %%xmm1{mk}')
        add(f'{op} mem{mn}', f'{E}{op} (%1), %%xmm1{mk}')
        add(f'{op} rev reg{mn}', f'.byte 0x62, 0xf1, {"0x6e" if op == "vmovss" else "0xef"}, {"0x08" if not mk else "0x89" if "z" in mk else "0x09"}, 0x11, 0xd9')
    add(f'{op} st', f'{E}{op} %%xmm3, (%1)')
    add(f'{op} st m', f'{op} %%xmm3, (%1)%{{%%k1%}}')
    if not I386:
        add(f'{op} reg hi', f'{HI}{op} %%xmm30, %%xmm25, %%xmm17%{{%%k1%}}\\n vmovdqu64 %%zmm17, %%zmm1')
for op in ('vmovlps', 'vmovhps', 'vmovlpd', 'vmovhpd'):
    add(f'{op} ld', f'{E}{op} (%1), %%xmm2, %%xmm1')
    add(f'{op} st', f'{E}{op} %%xmm3, (%1)')
for op in ('vmovhlps', 'vmovlhps'):
    add(f'{op} reg', f'{E}{op} %%xmm3, %%xmm2, %%xmm1')
    if not I386:
        add(f'{op} reg hi', f'{HI}{op} %%xmm30, %%xmm25, %%xmm17\\n vmovdqu64 %%zmm17, %%zmm1')
add('vmovd in reg', f'mov (%1), %%eax\\n {E}vmovd %%eax, %%xmm1')
add('vmovd in mem', f'{E}vmovd (%1), %%xmm1')
add('vmovd out reg', f'{E}vmovd %%xmm3, %%eax\\n vmovd %%eax, %%xmm1')
add('vmovd out mem', f'{E}vmovd %%xmm3, (%1)')
add('vmovq xmm reg', f'{E}vmovq %%xmm3, %%xmm1')
add('vmovq xmm mem', f'{E}vmovq (%1), %%xmm1')
add('vmovq st', f'{E}vmovq %%xmm3, (%1)')
add('vmovq d6 reg', '.byte 0x62, 0xf1, 0xfd, 0x08, 0xd6, 0xd9')       # vmovq xmm1, xmm3 the D6 way
if not I386:
    add('vmovq in reg', f'mov (%1), %%rax\\n {E}vmovq %%rax, %%xmm1')
    add('vmovq out reg', f'{E}vmovq %%xmm3, %%rax\\n vmovq %%rax, %%xmm1')
    add('vmovq in hi', 'mov (%1), %%rax\\n vmovq %%rax, %%xmm17\\n vmovdqu64 %%zmm17, %%zmm1')
for imm in ('$0x0', '$0x5', '$0xff'):
    add(f'vpinsrb {imm} reg', f'mov (%1), %%eax\\n {E}vpinsrb {imm}, %%eax, %%xmm2, %%xmm1')
    add(f'vpinsrb {imm} mem', f'{E}vpinsrb {imm}, (%1), %%xmm2, %%xmm1')
    add(f'vpinsrw {imm} reg', f'mov (%1), %%eax\\n {E}vpinsrw {imm}, %%eax, %%xmm2, %%xmm1')
    add(f'vpinsrw {imm} mem', f'{E}vpinsrw {imm}, (%1), %%xmm2, %%xmm1')
    add(f'vpinsrd {imm} reg', f'mov (%1), %%eax\\n {E}vpinsrd {imm}, %%eax, %%xmm2, %%xmm1')
    add(f'vpinsrd {imm} mem', f'{E}vpinsrd {imm}, (%1), %%xmm2, %%xmm1')
    add(f'vpextrb {imm} reg', f'{E}vpextrb {imm}, %%xmm3, %%eax\\n vmovd %%eax, %%xmm1')
    add(f'vpextrb {imm} mem', f'{E}vpextrb {imm}, %%xmm3, (%1)')
    add(f'vpextrw {imm} reg', f'{E}vpextrw {imm}, %%xmm3, %%eax\\n vmovd %%eax, %%xmm1')
    add(f'vpextrw {imm} mem', f'{E}vpextrw {imm}, %%xmm3, (%1)')
    add(f'vpextrw c5 {imm} reg', f'.byte 0x62, 0xf1, 0x7d, 0x08, 0xc5, 0xc3, {int(imm[1:], 16)}\\n vmovd %%eax, %%xmm1')
    add(f'vpextrd {imm} reg', f'{E}vpextrd {imm}, %%xmm3, %%eax\\n vmovd %%eax, %%xmm1')
    add(f'vpextrd {imm} mem', f'{E}vpextrd {imm}, %%xmm3, (%1)')
    add(f'vextractps {imm} reg', f'{E}vextractps {imm}, %%xmm3, %%eax\\n vmovd %%eax, %%xmm1')
    add(f'vextractps {imm} mem', f'{E}vextractps {imm}, %%xmm3, (%1)')
    if not I386:
        add(f'vpinsrq {imm} reg', f'mov (%1), %%rax\\n {E}vpinsrq {imm}, %%rax, %%xmm2, %%xmm1')
        add(f'vpinsrq {imm} mem', f'{E}vpinsrq {imm}, (%1), %%xmm2, %%xmm1')
        add(f'vpextrq {imm} reg', f'{E}vpextrq {imm}, %%xmm3, %%rax\\n vmovq %%rax, %%xmm1')
        add(f'vpextrq {imm} mem', f'{E}vpextrq {imm}, %%xmm3, (%1)')
        add(f'vpextrd {imm} hi', f'{HI}vpextrd {imm}, %%xmm30, %%eax\\n vmovd %%eax, %%xmm1')
for imm in ('$0x0', '$0x1d', '$0x6a', '$0xc5', '$0xff'):
    add(f'vinsertps {imm} reg', f'{E}vinsertps {imm}, %%xmm3, %%xmm2, %%xmm1')
    add(f'vinsertps {imm} mem', f'{E}vinsertps {imm}, (%1), %%xmm2, %%xmm1')
# VDBPSADBW; BF16: VCVTNEPS2BF16 (narrowing), VCVTNE2PS2BF16, VDPBF16PS
for imm in ('$0x0', '$0x1b', '$0xe4', '$0xff', '$0x93'):
    three('vdbpsadbw', 0, imm)
HALF = {128: 'xmm', 256: 'xmm', 512: 'ymm'}
SFX = {128: 'x', 256: 'y', 512: ''}
for L, x in VLS.items():
    h = HALF[L]
    for mk, mn in MK:
        add(f'vcvtneps2bf16 {L} reg{mn}', f'vcvtneps2bf16 %%{x}3, %%{h}1{mk}')
        add(f'vcvtneps2bf16 {L} mem{mn}', f'vcvtneps2bf16{SFX[L]} (%1), %%{h}1{mk}')
    add(f'vcvtneps2bf16 {L} bcst z', f'vcvtneps2bf16 (%1)%{{1to{L // 32}%}}, %%{h}1%{{%%k1%}}%{{z%}}')
three('vcvtne2ps2bf16', 4)
three('vdpbf16ps', 4)
UD = {'vpshldw W0': '62 f3 6d 48 70 cb 05', 'vpshldw ok': '62 f3 ed 48 70 cb 05',
      'vpshldvd W1 ok': '62 f2 ed 48 71 cb', 'vpdpbusd W1': '62 f2 ed 48 50 cb', 'vpdpbusd ok': '62 f2 6d 48 50 cb',
      'vpshufbitqmb z': '62 f2 6d c9 8f cb', 'vpshufbitqmb ok': '62 f2 6d 49 8f cb',
      'vpshufbitqmb W1': '62 f2 ed 48 8f cb', 'vgf2p8mulb W1': '62 f2 ed 48 cf cb',
      'vpopcntb vvvv': '62 f2 6d 48 54 cb', 'vpopcntb ok': '62 f2 7d 48 54 cb',
      'vpmadd52luq W0': '62 f2 6d 48 b4 cb', 'vgf2p8affineqb W0': '62 f3 6d 48 ce cb 00',
      'vpmultishiftqb W0': '62 f2 6d 48 83 cb', 'vpshufbitqmb bcst': '62 f2 6d 58 8f 08',
      'vex vgf2p8mulb W1': 'c4 e2 e9 cf cb', 'vex vgf2p8mulb ok': 'c4 e2 69 cf cb',
      'vex vgf2p8affineqb W0': 'c4 e3 69 ce cb 00', 'vex vgf2p8affineqb ok': 'c4 e3 e9 ce cb 00',
      'vaesenc ok': '62 f2 6d 48 dc cb', 'vaesenc masked': '62 f2 6d 49 dc cb', 'vaesenc z': '62 f2 6d c8 dc cb',
      'vaesenc W1': '62 f2 ed 48 dc cb', 'vaesenc b mem': '62 f2 6d 58 dc 08',
      'evex vaesimc': '62 f2 7d 08 db cb', 'vex vaesimc ok': 'c4 e2 79 db cb', 'vex vaesimc L1': 'c4 e2 7d db cb',
      'vex vaesimc vvvv': 'c4 e2 69 db cb', 'evex vaeskeygenassist': '62 f3 7d 08 df cb 01',
      'vex vaeskeygenassist L1': 'c4 e3 7d df cb 01', 'vpclmulqdq masked': '62 f3 6d 49 44 cb 00',
      'vex vpclmulqdq W1': 'c4 e3 e9 44 cb 00'}
for k, b in UD.items():
    F.append((f'ud {k}', 'ud:' + ', '.join('0x' + x for x in b.split())))
R = 'e' if I386 else 'r'
o = []; w = o.append
w(f'''// Generated by tools/gen-avx512-ext-test.py{" --i386" if I386 else ""} -- do not edit.
//
// AVX-512 integer extensions against Intel SDE (see
// the generator). (No target pragma: the harness itself must not use AVX-512.)
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
static uint64_t rs;
static uint64_t rnd(void) {{ rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }}
struct st {{ uint8_t d[64], a[64], b[64], out[64], m[64]; uint64_t k, kout; }} __attribute__((aligned(64)));
static sigjmp_buf jb;
static volatile int sig;
static void handler(int s) {{ sig = s; siglongjmp(jb, 1); }}
#define LOAD "vmovdqu64 (%0), %%zmm1\\n vmovdqu64 64(%0), %%zmm2\\n vmovdqu64 128(%0), %%zmm3\\n kmovq 320(%0), %%k1\\n"
#define SAVE "\\n vmovdqu64 %%zmm1, 192(%0)\\n kmovq %%k1, 328(%0)"
''')
for i, (name, asm) in enumerate(F):
    if asm.startswith('ud:'):
        w(f'''__attribute__((noinline)) static void f{i}(struct st *t) {{
    __asm__ volatile(LOAD "mov %1, %%{R}ax\\n .byte {asm[3:]}" SAVE :: "r"(t), "r"(t->m) : "memory", "{R}ax", "xmm1", "xmm2", "xmm3");
}}''')
    else:
        gx = f', "{R}ax"' if ('%%eax' in asm or '%%rax' in asm or '0xc3, ' in asm) else ''
        w(f'''__attribute__((noinline)) static void f{i}(struct st *t) {{
    __asm__ volatile(LOAD "{asm}" SAVE :: "r"(t), "r"(t->m) : "memory", "xmm1", "xmm2", "xmm3"{gx});
}}''')
w('static const struct { const char *name; void (*fn)(struct st *); } forms[] = {')
for i, (name, asm) in enumerate(F):
    w(f'    {{"{name}", f{i}}},')
w('};')
w(f'#define NF {len(F)}')
if answers:
    w('static const uint64_t want[NF] = {')
    for name, asm in F:
        w(f'    {answers[name]}ull,')
    w('};')
else:
    w('static const uint64_t want[NF];   // (run with "hashes" under SDE)')
w(r'''
static int run(void (*fn)(struct st *), struct st *t) {
    sig = 0;
    if (!sigsetjmp(jb, 1))
        fn(t);
    return sig;
}
// A legacy SSE operand must be 16-byte aligned: #GP, SIGSEGV. SDE aborts on
// it instead ("unaligned memory reference"); "sde" as argv[1] skips this,
// and the SSE forms' upper-bits check, which SDE fails.
static int under_sde;
static uint8_t buf[64] __attribute__((aligned(64)));
static int misaligned(int which) {
    sig = 0;
    if (!sigsetjmp(jb, 1)) {
        if (which == 1)
            __asm__ volatile("gf2p8affineinvqb $0x63, 8(%0), %%xmm1" :: "r"(buf) : "memory", "xmm1");
        else if (which == 2)
            __asm__ volatile("aesenc 4(%0), %%xmm1" :: "r"(buf) : "memory", "xmm1");
        else if (which == 3)
            __asm__ volatile("pclmulqdq $0x11, 2(%0), %%xmm1" :: "r"(buf) : "memory", "xmm1");
        else
            __asm__ volatile("gf2p8mulb 1(%0), %%xmm1" :: "r"(buf) : "memory", "xmm1");
    }
    return sig;
}
static unsigned long fixed_checks(unsigned long *checks) {
    static const char *const what[4] = {"gf2p8mulb", "gf2p8affineinvqb", "aesenc", "pclmulqdq"};
    unsigned long bad = 0;
    if (under_sde)
        return 0;
    for (int i = 0; i < 4; i++) {
        int sg = misaligned(i);
        (*checks)++;
        if (sg != SIGSEGV && bad++ < 40)
            printf("FAIL sse %s misaligned: signal %d (want %d)\n", what[i], sg, SIGSEGV);
    }
    return bad;
}

int main(int argc, char **argv) {
    int print = argc > 1 && !strcmp(argv[1], "hashes"), dump = argc > 1 && !strcmp(argv[1], "dump");
    under_sde = argc > 1 && !strcmp(argv[1], "sde");
    unsigned long checks = 0, bad = 0;
    static struct st t;
    signal(SIGILL, handler);
    signal(SIGSEGV, handler);
    for (int fi = 0; fi < NF; fi++) {
        uint64_t h = 0xcbf29ce484222325ull;
        rs = 0x9e3779b97f4a7c15ull;
        for (const char *c = forms[fi].name; *c; c++) rs = (rs ^ (uint8_t) *c) * 0x100000001b3ull;
        int ud = !strncmp(forms[fi].name, "ud ", 3);
        for (int k = 0; k < (ud ? 1 : 16); k++) {
            for (int i = 0; i < 64; i += 8) {
                uint64_t r;
                r = rnd(); memcpy(t.d + i, &r, 8);
                r = rnd(); memcpy(t.a + i, &r, 8);
                r = rnd(); memcpy(t.b + i, &r, 8);
                r = rnd(); memcpy(t.m + i, &r, 8);
            }
            if (k & 1)                           /* small and equal values: small counts and products */
                for (int i = 0; i < 64; i++) { t.b[i] &= 0x83; t.m[i] &= 0x83; }
            if (k == 5)                          /* 0x8000 words; 0x7fffffff accumulators */
                for (int i = 0; i < 64; i++) {
                    t.a[i] = t.b[i] = t.m[i] = i & 1 ? 0x80 : 0;
                    t.d[i] = (i & 3) == 3 ? 0x7f : 0xff;
                }
            if (k == 7)                          /* 0x7f bytes and words; 0x7fffffff */
                for (int i = 0; i < 64; i++) {
                    t.a[i] = t.b[i] = t.m[i] = 0x7f;
                    t.d[i] = (i & 3) == 3 ? 0x7f : 0xff;
                }
            if (k == 9)                          /* 255 * -128; 0x80000000 */
                for (int i = 0; i < 64; i++) {
                    t.a[i] = 0xff;
                    t.b[i] = t.m[i] = 0x80;
                    t.d[i] = (i & 3) == 3 ? 0x80 : 0;
                }
            t.k = rnd();
            if (k == 2) t.k = 0;
            if (k == 3) t.k = ~0ull;
            t.kout = 0;
            memset(t.out, 0, 64);
            int sg = run(forms[fi].fn, &t);
            if (!strncmp(forms[fi].name, "sse ", 4)) {
                if (!under_sde && !print && !dump) {
                    checks++;
                    if (memcmp(t.out + 16, t.d + 16, 48) && bad++ < 40)
                        printf("FAIL %s %d: the bits above 128 changed\n", forms[fi].name, k);
                }
                memset(t.out + 16, 0, 48);
            }
            for (int i = 0; i < 64; i++) h = (h ^ t.out[i]) * 0x100000001b3ull;
            for (int i = 0; i < 64; i++) h = (h ^ t.m[i]) * 0x100000001b3ull;
            for (int b = 0; b < 8; b++) h = (h ^ ((t.kout >> (8 * b)) & 0xff)) * 0x100000001b3ull;
            h = (h ^ (unsigned) sg) * 0x100000001b3ull;
            if (dump) {
                printf("%s %d: sig %d k1 %016llx out ", forms[fi].name, k, sg, (unsigned long long) t.kout);
                for (int i = 63; i >= 0; i--) printf("%02x", t.out[i]);
                printf(" m ");
                for (int i = 63; i >= 0; i--) printf("%02x", t.m[i]);
                printf("\n");
            }
        }
        if (print) printf("%s %#llx\n", forms[fi].name, (unsigned long long) h);
        checks++;
        if (!print && !dump && h != want[fi] && bad++ < 40)
            printf("FAIL %s\n", forms[fi].name);
    }
    if (!print && !dump)
        bad += fixed_checks(&checks);
    if (!print && !dump)
        printf("NAME: %s (%d forms, %lu checks, %lu mismatches)\n", bad ? "FAIL" : "PASS", NF, checks, bad);
    return bad != 0;
}''')
open(args[0], 'w').write('\n'.join(o).replace('NAME: %s', NAME + ': %s'))
print(len(F), 'forms')
