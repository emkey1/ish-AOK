#!/usr/bin/env python3
# Generates tests/manual/x86/amd64_avx512_float.c, or with --i386
# i386_avx512_float.c (usage: this script [--i386] [--answers FILE] OUT.c).
#
# AVX-512 (EVEX) floating-point arithmetic: VADD/VSUB/VMUL/VDIV/VMIN/VMAX/
# VSQRT PS/PD, and FMA (VF[N]MADD/VF[N]MSUB 132/213/231 PS/PD/SS/SD,
# VFMADDSUB/VFMSUBADD PS/PD, with {er}, the destination an operand too) at VL 128/256/512 (register, memory and {1toN} sources) and
# SS/SD (register and m32/m64), VCMP PS/PD/SS/SD into k1 under every
# predicate, unmasked, merge- and zero-masked (compares: with and without a
# {k2} write mask), plus (amd64) one zmm16-31 form per op for EVEX.R'/V'/X.
# Then the static roundings ({rn,rd,ru,rz}-sae: packed at 512 bits, where
# L'L is the rounding control, and scalar) and {sae} for min, max and the
# compares, register-register only; and encodings whose #UD-or-not is part
# of the answer.
#
# Every form runs under seven MXCSRs (the four roundings, FTZ+DAZ, FTZ
# alone, DAZ alone), loaded just before the instruction with the flags
# clear and stored just after it in the same asm block. Elements mix the
# special values (signed zeros and infinities, QNaNs and SNaNs with
# payloads, denormals, min normal, max finite, 1.0, values at rounding and
# overflow/underflow boundaries) with random ones whose exponents are spread
# enough to overflow, underflow and round; equal, negated and unordered
# pairs come up often. One case per mode puts an SNaN, a denormal, a zero
# divisor or an overflowing/invalid operand in each masked-off lane, and
# 1.0 in the enabled ones: a masked-off lane must raise no flag. The
# destination's 64 bytes, k1, MXCSR (control and flags) and the signal are
# hashed per form and checked against Intel SDE's (`sde64 -ptr-raise -spr
# -- ./test hashes` on camd, passed back with --answers); a zero-masked
# form's masked-off elements are checked to be 0 by the harness itself and
# hashed as 0 (SDE writes -0.0 there for VSUBPS/PD under round-down at 256
# and 512 bits; the SDM sets them to 0). Forms are keyed
# and seeded by name, so the i386 test (zmm0-7, no 16-31 forms) takes the
# amd64 answers. "dump" prints every case.
import re
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
NAME = 'i386_avx512_float' if I386 else 'amd64_avx512_float'
VL = [(128, 'xmm'), (256, 'ymm'), (512, 'zmm')]
MK = [('', ''), ('%{%%k1%}', 'm'), ('%{%%k1%}%{z%}', 'z')]
CMK = [('', ''), ('%{%%k2%}', 'm')]           # a compare writes k1; k2 masks it
ARITH = ['add', 'sub', 'mul', 'div', 'min', 'max', 'sqrt']
ER = ['add', 'sub', 'mul', 'div', 'sqrt']      # take {er}; min and max take {sae}
RC = ['rn', 'rd', 'ru', 'rz']
CMP_FEW = [0x00, 0x01, 0x03, 0x04, 0x0d, 0x11, 0x1c]
CMP_SAE = [0x00, 0x01, 0x11, 0x1f]
DBL = {'ps': 0, 'pd': 1, 'ss': 0, 'sd': 1}
F = []   # (name, asm, flags: 1 double, 2 a compare, 4 the zmm16-31 registers, 8 zero-masked (the
         #  elements it masks at bits 4-8), 1024 the destination is an operand too (FMA),
         #  2048 VL 128 with 1.0 in every element past it, 4096 PE and DE checked clear here and
         #  hashed clear, 8192 VSCALEF: see below, 16384 s2 random bits (a VFIXUPIMM table, an
         #  integer source), 32768 s2 seeded with floats at the integer ranges' edges, 65536 the asm
         #  uses eax/rax)
def add(name, asm, t, cmp=False, hi=False, dst=False, table=False, ibound=False, zchk=True, gpr=False):
    fl = DBL[t] | (2 if cmp else 0) | (4 if hi else 0) | (1024 if dst else 0) | (16384 if table else 0) | \
        (32768 if ibound else 0) | (65536 if gpr else 0)
    m = re.match(r'vrndscale[ps]d \$(0x[0-9a-f]+)', name)
    if m and int(m.group(1), 16) & 8:
        # SDE raises PE (and DE for a denormal) for VRNDSCALEPD/SD with imm8
        # bit 3, which suppresses PE (VRNDSCALEPS/SS and ROUNDSD honour it;
        # with bit 3 clear it raises no DE): the harness checks both clear
        fl |= 4096
    if name.startswith('vscalef'):
        # SDE's VSCALEF raises no OE, UE or PE under DAZ (it does without),
        # and no DE for a denormal s1 under FTZ or when the result overflows
        # or underflows. The SDM's pseudo-code is a multiply (DAZ on both
        # sources, DE for s1) and AOK sets flags as a multiply does; with no
        # AVX-512 hardware here to settle it, those flags are hashed clear
        # in those cases
        fl |= 8192
    if dst and ' 128 ' in name:
        # SDE evaluates an EVEX.128 FMA's elements 128-255 too, and raises
        # their flags (a denormal in element 5 of a VFMADD231PS xmm{k}
        # raises DE whatever k; VADDPS does not, nor VL 256 past 256): 1.0
        # there, which no hardware reads, keeps them out of MXCSR
        fl |= 2048
    if '%{z%}' in asm and not cmp and zchk:
        # SDE writes -0.0 into zero-masked VSUBPS/PD elements under round-down at
        # 256 and 512 bits (it appears to compute 0 - 0 there; its 128-bit form
        # writes +0); the SDM sets them to 0. So the harness checks those
        # elements are 0 itself and hashes them as 0.
        L = 128 if ' 128 ' in name else 256 if ' 256 ' in name else 512
        n = 1 if t in ('ss', 'sd') else L // (64 if DBL[t] else 32)
        fl |= 8 | n << 4
    F.append((name, asm, fl))
# packed arithmetic (sqrt's packed form is unary: its vvvv must be 1111b)
for op in ARITH:
    for t in ('ps', 'pd'):
        bc = 4 if t == 'ps' else 8
        for L, x in VL:
            s1 = '' if op == 'sqrt' else f', %%{x}2'
            for mk, mn in MK:
                add(f'v{op}{t} {L} reg{mn}', f'v{op}{t} %%{x}3{s1}, %%{x}1{mk}', t)
                add(f'v{op}{t} {L} mem{mn}', f'v{op}{t} (%1){s1}, %%{x}1{mk}', t)
                add(f'v{op}{t} {L} bcst{mn}', f'v{op}{t} (%1)%{{1to{L // (8 * bc)}%}}{s1}, %%{x}1{mk}', t)
# scalar arithmetic (sqrt too has an s1: the upper elements)
for op in ARITH:
    for t in ('ss', 'sd'):
        for mk, mn in MK:
            add(f'v{op}{t} reg{mn}', f'v{op}{t} %%xmm3, %%xmm2, %%xmm1{mk}', t)
            add(f'v{op}{t} mem{mn}', f'v{op}{t} (%1), %%xmm2, %%xmm1{mk}', t)
# zmm16-31: s1 zmm25, s2 zmm30, destination zmm17
if not I386:
    for op in ARITH:
        for t in ('ps', 'pd'):
            s1 = '' if op == 'sqrt' else ', %%zmm25'
            add(f'v{op}{t} 512 reg m hi', f'v{op}{t} %%zmm30{s1}, %%zmm17%{{%%k1%}}', t, hi=True)
        for t in ('ss', 'sd'):
            add(f'v{op}{t} reg m hi', f'v{op}{t} %%xmm30, %%xmm25, %%xmm17%{{%%k1%}}', t, hi=True)
# static rounding, register-register: packed at 512 bits and scalar
for op in ER:
    for rc in RC:
        for t in ('ps', 'pd'):
            s1 = '' if op == 'sqrt' else ', %%zmm2'
            for mk, mn in MK:
                add(f'v{op}{t} {{{rc}-sae}} 512 reg{mn}', f'v{op}{t} %{{{rc}-sae%}}, %%zmm3{s1}, %%zmm1{mk}', t)
        for t in ('ss', 'sd'):
            for mk, mn in MK:
                add(f'v{op}{t} {{{rc}-sae}} reg{mn}', f'v{op}{t} %{{{rc}-sae%}}, %%xmm3, %%xmm2, %%xmm1{mk}', t)
for op in ('min', 'max'):
    for t in ('ps', 'pd'):
        for mk, mn in MK:
            add(f'v{op}{t} {{sae}} 512 reg{mn}', f'v{op}{t} %{{sae%}}, %%zmm3, %%zmm2, %%zmm1{mk}', t)
    for t in ('ss', 'sd'):
        for mk, mn in MK:
            add(f'v{op}{t} {{sae}} reg{mn}', f'v{op}{t} %{{sae%}}, %%xmm3, %%xmm2, %%xmm1{mk}', t)
# compares into k1: every predicate register at 512 bits (scalar: xmm);
# a spread of them at the other widths and from memory and {1toN}
for t in ('ps', 'pd', 'ss', 'sd'):
    bc = 4 if t in ('ps', 'ss') else 8
    packed = t[0] == 'p'
    for imm in range(32):
        for mk, mn in CMK:
            x = 'zmm' if packed else 'xmm'
            nm = f'vcmp{t} ${imm:#x} {"512 " if packed else ""}reg{mn}'
            add(nm, f'vcmp{t} ${imm:#x}, %%{x}3, %%{x}2, %%k1{mk}', t, cmp=True)
    for imm in CMP_FEW:
        for mk, mn in CMK:
            if packed:
                for L, x in VL:
                    if L != 512:
                        add(f'vcmp{t} ${imm:#x} {L} reg{mn}', f'vcmp{t} ${imm:#x}, %%{x}3, %%{x}2, %%k1{mk}', t, cmp=True)
                    add(f'vcmp{t} ${imm:#x} {L} mem{mn}', f'vcmp{t} ${imm:#x}, (%1), %%{x}2, %%k1{mk}', t, cmp=True)
                    add(f'vcmp{t} ${imm:#x} {L} bcst{mn}',
                        f'vcmp{t} ${imm:#x}, (%1)%{{1to{L // (8 * bc)}%}}, %%{x}2, %%k1{mk}', t, cmp=True)
            else:
                add(f'vcmp{t} ${imm:#x} mem{mn}', f'vcmp{t} ${imm:#x}, (%1), %%xmm2, %%k1{mk}', t, cmp=True)
    for imm in CMP_SAE:
        for mk, mn in CMK:
            x = 'zmm' if packed else 'xmm'
            add(f'vcmp{t} ${imm:#x} {{sae}} {"512 " if packed else ""}reg{mn}',
                f'vcmp{t} ${imm:#x}, %{{sae%}}, %%{x}3, %%{x}2, %%k1{mk}', t, cmp=True)
    if not I386:
        x = 'zmm' if packed else 'xmm'
        add(f'vcmp{t} $0x1 reg m hi', f'vcmp{t} $0x1, %%{x}30, %%{x}25, %%k1%{{%%k2%}}', t, cmp=True, hi=True)
# FMA: 132/213/231, the destination an operand too
FMA_P = ['fmadd', 'fmsub', 'fnmadd', 'fnmsub', 'fmaddsub', 'fmsubadd']
FMA_S = ['fmadd', 'fmsub', 'fnmadd', 'fnmsub']
for form in ('132', '213', '231'):
    for op in FMA_P:
        for t in ('ps', 'pd'):
            bc = 4 if t == 'ps' else 8
            n = f'v{op}{form}{t}'
            for L, x in VL:
                for mk, mn in MK:
                    add(f'{n} {L} reg{mn}', f'{n} %%{x}3, %%{x}2, %%{x}1{mk}', t, dst=True)
                    add(f'{n} {L} mem{mn}', f'{n} (%1), %%{x}2, %%{x}1{mk}', t, dst=True)
                    add(f'{n} {L} bcst{mn}', f'{n} (%1)%{{1to{L // (8 * bc)}%}}, %%{x}2, %%{x}1{mk}', t, dst=True)
            for rc in RC:
                for mk, mn in MK:
                    add(f'{n} {{{rc}-sae}} 512 reg{mn}', f'{n} %{{{rc}-sae%}}, %%zmm3, %%zmm2, %%zmm1{mk}', t, dst=True)
            if not I386:
                add(f'{n} 512 reg m hi', f'{n} %%zmm30, %%zmm25, %%zmm17%{{%%k1%}}', t, hi=True, dst=True)
    for op in FMA_S:
        for t in ('ss', 'sd'):
            n = f'v{op}{form}{t}'
            for mk, mn in MK:
                add(f'{n} reg{mn}', f'{n} %%xmm3, %%xmm2, %%xmm1{mk}', t, dst=True)
                add(f'{n} mem{mn}', f'{n} (%1), %%xmm2, %%xmm1{mk}', t, dst=True)
                for rc in RC:
                    add(f'{n} {{{rc}-sae}} reg{mn}', f'{n} %{{{rc}-sae%}}, %%xmm3, %%xmm2, %%xmm1{mk}', t, dst=True)
            if not I386:
                add(f'{n} reg m hi', f'{n} %%xmm30, %%xmm25, %%xmm17%{{%%k1%}}', t, hi=True, dst=True)
# unary families on s2 (scalar: s1 the upper elements), {sae} at 512 and
# scalar: VGETEXP
def unary(op, imms=('',)):
    for imm in imms:
        ii = imm and imm + ', '
        ni = imm and ' ' + imm
        for t in ('ps', 'pd'):
            bc = 4 if t == 'ps' else 8
            n = f'v{op}{t}'
            for L, x in VL:
                for mk, mn in MK:
                    add(f'{n}{ni} {L} reg{mn}', f'{n} {ii}%%{x}3, %%{x}1{mk}', t)
                    add(f'{n}{ni} {L} mem{mn}', f'{n} {ii}(%1), %%{x}1{mk}', t)
                    add(f'{n}{ni} {L} bcst{mn}', f'{n} {ii}(%1)%{{1to{L // (8 * bc)}%}}, %%{x}1{mk}', t)
            for mk, mn in MK:
                add(f'{n}{ni} {{sae}} 512 reg{mn}', f'{n} {ii}%{{sae%}}, %%zmm3, %%zmm1{mk}', t)
            if not I386:
                add(f'{n}{ni} 512 reg m hi', f'{n} {ii}%%zmm30, %%zmm17%{{%%k1%}}', t, hi=True)
        for t in ('ss', 'sd'):
            n = f'v{op}{t}'
            for mk, mn in MK:
                add(f'{n}{ni} reg{mn}', f'{n} {ii}%%xmm3, %%xmm2, %%xmm1{mk}', t)
                add(f'{n}{ni} mem{mn}', f'{n} {ii}(%1), %%xmm2, %%xmm1{mk}', t)
                add(f'{n}{ni} {{sae}} reg{mn}', f'{n} {ii}%{{sae%}}, %%xmm3, %%xmm2, %%xmm1{mk}', t)
            if not I386:
                add(f'{n}{ni} reg m hi', f'{n} {ii}%%xmm30, %%xmm25, %%xmm17%{{%%k1%}}', t, hi=True)
unary('getexp')
unary('getmant', [f'${i:#x}' for i in range(16)])
RND = [0x00, 0x01, 0x02, 0x03, 0x04, 0x08, 0x0b, 0x0c, 0x10, 0x21, 0x32, 0x43, 0x5c, 0x88, 0xf0, 0xff]
unary('rndscale', [f'${i:#x}' for i in RND])
unary('reduce', [f'${i:#x}' for i in RND])
# VSCALEF: as the arithmetic ({er})
for t in ('ps', 'pd'):
    bc = 4 if t == 'ps' else 8
    for L, x in VL:
        for mk, mn in MK:
            add(f'vscalef{t} {L} reg{mn}', f'vscalef{t} %%{x}3, %%{x}2, %%{x}1{mk}', t)
            add(f'vscalef{t} {L} mem{mn}', f'vscalef{t} (%1), %%{x}2, %%{x}1{mk}', t)
            add(f'vscalef{t} {L} bcst{mn}', f'vscalef{t} (%1)%{{1to{L // (8 * bc)}%}}, %%{x}2, %%{x}1{mk}', t)
    for rc in RC:
        for mk, mn in MK:
            add(f'vscalef{t} {{{rc}-sae}} 512 reg{mn}', f'vscalef{t} %{{{rc}-sae%}}, %%zmm3, %%zmm2, %%zmm1{mk}', t)
    if not I386:
        add(f'vscalef{t} 512 reg m hi', f'vscalef{t} %%zmm30, %%zmm25, %%zmm17%{{%%k1%}}', t, hi=True)
for t in ('ss', 'sd'):
    for mk, mn in MK:
        add(f'vscalef{t} reg{mn}', f'vscalef{t} %%xmm3, %%xmm2, %%xmm1{mk}', t)
        add(f'vscalef{t} mem{mn}', f'vscalef{t} (%1), %%xmm2, %%xmm1{mk}', t)
        for rc in RC:
            add(f'vscalef{t} {{{rc}-sae}} reg{mn}', f'vscalef{t} %{{{rc}-sae%}}, %%xmm3, %%xmm2, %%xmm1{mk}', t)
    if not I386:
        add(f'vscalef{t} reg m hi', f'vscalef{t} %%xmm30, %%xmm25, %%xmm17%{{%%k1%}}', t, hi=True)
# VRANGE: two sources and an imm8 (all 16), {sae}
for i in range(16):
    imm = f'${i:#x}'
    for t in ('ps', 'pd'):
        bc = 4 if t == 'ps' else 8
        n = f'vrange{t}'
        for L, x in VL:
            for mk, mn in MK:
                add(f'{n} {imm} {L} reg{mn}', f'{n} {imm}, %%{x}3, %%{x}2, %%{x}1{mk}', t)
            add(f'{n} {imm} {L} mem m', f'{n} {imm}, (%1), %%{x}2, %%{x}1%{{%%k1%}}', t)
            add(f'{n} {imm} {L} bcst z', f'{n} {imm}, (%1)%{{1to{L // (8 * bc)}%}}, %%{x}2, %%{x}1%{{%%k1%}}%{{z%}}', t)
        add(f'{n} {imm} {{sae}} 512 reg', f'{n} {imm}, %{{sae%}}, %%zmm3, %%zmm2, %%zmm1', t)
        if not I386:
            add(f'{n} {imm} 512 reg m hi', f'{n} {imm}, %%zmm30, %%zmm25, %%zmm17%{{%%k1%}}', t, hi=True)
    for t in ('ss', 'sd'):
        n = f'vrange{t}'
        for mk, mn in MK:
            add(f'{n} {imm} reg{mn}', f'{n} {imm}, %%xmm3, %%xmm2, %%xmm1{mk}', t)
        add(f'{n} {imm} mem m', f'{n} {imm}, (%1), %%xmm2, %%xmm1%{{%%k1%}}', t)
        add(f'{n} {imm} {{sae}} reg', f'{n} {imm}, %{{sae%}}, %%xmm3, %%xmm2, %%xmm1', t)
# VFPCLASS into k1 (k2 masking it): each category alone, and a few together
for imm in [f'${1 << i:#x}' for i in range(8)] + ['$0x0', '$0x81', '$0x66', '$0xff', '$0x3c']:
    for t in ('ps', 'pd'):
        bc = 4 if t == 'ps' else 8
        sfx = {128: 'x', 256: 'y', 512: 'z'}   # (memory forms need the size: vfpclasspsx)
        for L, x in VL:
            for mk, mn in CMK:
                add(f'vfpclass{t} {imm} {L} reg{mn}', f'vfpclass{t} {imm}, %%{x}3, %%k1{mk}', t, cmp=True)
            add(f'vfpclass{t} {imm} {L} mem', f'vfpclass{t}{sfx[L]} {imm}, (%1), %%k1', t, cmp=True)
            add(f'vfpclass{t} {imm} {L} bcst m', f'vfpclass{t} {imm}, (%1)%{{1to{L // (8 * bc)}%}}, %%k1%{{%%k2%}}', t, cmp=True)
    for t in ('ss', 'sd'):
        for mk, mn in CMK:
            add(f'vfpclass{t} {imm} reg{mn}', f'vfpclass{t} {imm}, %%xmm3, %%k1{mk}', t, cmp=True)
        add(f'vfpclass{t} {imm} mem', f'vfpclass{t} {imm}, (%1), %%k1', t, cmp=True)
# VFIXUPIMM: s1 the float, s2 (and memory) a table of random bits, the
# destination an operand; imm8 a spread, {sae}
for imm in ('$0x0', '$0x1', '$0x2', '$0x4', '$0x8', '$0x10', '$0x20', '$0x40', '$0x80', '$0xff', '$0x5a'):
    for t in ('ps', 'pd'):
        bc = 4 if t == 'ps' else 8
        n = f'vfixupimm{t}'
        for L, x in VL:
            for mk, mn in MK:
                add(f'{n} {imm} {L} reg{mn}', f'{n} {imm}, %%{x}3, %%{x}2, %%{x}1{mk}', t, dst=True, table=True)
            add(f'{n} {imm} {L} mem m', f'{n} {imm}, (%1), %%{x}2, %%{x}1%{{%%k1%}}', t, dst=True, table=True)
            add(f'{n} {imm} {L} bcst z', f'{n} {imm}, (%1)%{{1to{L // (8 * bc)}%}}, %%{x}2, %%{x}1%{{%%k1%}}%{{z%}}', t,
                dst=True, table=True)
        add(f'{n} {imm} {{sae}} 512 reg', f'{n} {imm}, %{{sae%}}, %%zmm3, %%zmm2, %%zmm1', t, dst=True, table=True)
        if not I386:
            add(f'{n} {imm} 512 reg m hi', f'{n} {imm}, %%zmm30, %%zmm25, %%zmm17%{{%%k1%}}', t, hi=True, dst=True,
                table=True)
    for t in ('ss', 'sd'):
        n = f'vfixupimm{t}'
        for mk, mn in MK:
            add(f'{n} {imm} reg{mn}', f'{n} {imm}, %%xmm3, %%xmm2, %%xmm1{mk}', t, dst=True, table=True)
        add(f'{n} {imm} mem m', f'{n} {imm}, (%1), %%xmm2, %%xmm1%{{%%k1%}}', t, dst=True, table=True)
        add(f'{n} {imm} {{sae}} reg', f'{n} {imm}, %{{sae%}}, %%xmm3, %%xmm2, %%xmm1', t, dst=True, table=True)
# conversions of the same width: integers to floats ({er}) from random bits,
# floats to integers ({er}, or {sae} for the truncating) with the ranges' edges
def cvt_same(op, t, er, **kw):
    bc = 4 if t in ('ps', 'dq') else 8
    tt = 'pd' if bc == 8 else 'ps'
    for L, x in VL:
        for mk, mn in MK:
            add(f'{op} {L} reg{mn}', f'{op} %%{x}3, %%{x}1{mk}', tt, **kw)
            add(f'{op} {L} mem{mn}', f'{op} (%1), %%{x}1{mk}', tt, **kw)
            add(f'{op} {L} bcst{mn}', f'{op} (%1)%{{1to{L // (8 * bc)}%}}, %%{x}1{mk}', tt, **kw)
    for rc in (RC if er else ['sae']):
        r = f'{rc}-sae' if er else 'sae'
        for mk, mn in MK:
            add(f'{op} {{{r}}} 512 reg{mn}', f'{op} %{{{r}%}}, %%zmm3, %%zmm1{mk}', tt, **kw)
    if not I386:
        add(f'{op} 512 reg m hi', f'{op} %%zmm30, %%zmm17%{{%%k1%}}', tt, hi=True, **kw)
for op in ('vcvtdq2ps', 'vcvtudq2ps'):
    cvt_same(op, 'dq', True, table=True)
for op in ('vcvtqq2pd', 'vcvtuqq2pd'):
    cvt_same(op, 'qq', True, table=True)
for op, t in (('vcvtps2dq', 'ps'), ('vcvtps2udq', 'ps'), ('vcvtpd2qq', 'pd'), ('vcvtpd2uqq', 'pd')):
    cvt_same(op, t, True, ibound=True)
for op, t in (('vcvttps2dq', 'ps'), ('vcvttps2udq', 'ps'), ('vcvttpd2qq', 'pd'), ('vcvttpd2uqq', 'pd')):
    cvt_same(op, t, False, ibound=True)
# conversions that widen (the source VL / 2) or narrow (the destination VL /
# 2): t the source's type (for the fill); no zero-masked check (the sizes
# differ; the hash has those elements)
HALF = {128: 'xmm', 256: 'xmm', 512: 'ymm'}
SFX = {128: 'x', 256: 'y', 512: ''}
def cvt_wn(op, t, widen, rnd, **kw):
    bc = 4 if t in ('ps', 'dq') else 8
    tt = 'pd' if bc == 8 else 'ps'
    for L, x in VL:
        s, d = (HALF[L], x) if widen else (x, HALF[L])
        n = L // 64 if widen else L // (8 * bc)
        sfx = '' if widen else SFX[L]
        for mk, mn in MK:
            add(f'{op} {L} reg{mn}', f'{op} %%{s}3, %%{d}1{mk}', tt, zchk=False, **kw)
            add(f'{op} {L} mem{mn}', f'{op}{sfx} (%1), %%{d}1{mk}', tt, zchk=False, **kw)
            add(f'{op} {L} bcst{mn}', f'{op} (%1)%{{1to{n}%}}, %%{d}1{mk}', tt, zchk=False, **kw)
    if rnd:
        s, d = ('ymm', 'zmm') if widen else ('zmm', 'ymm')
        for r in (['sae'] if rnd == 'sae' else [f'{c}-sae' for c in RC]):
            for mk, mn in MK:
                add(f'{op} {{{r}}} 512 reg{mn}', f'{op} %{{{r}%}}, %%{s}3, %%{d}1{mk}', tt, zchk=False, **kw)
    if not I386:
        s, d = ('ymm', 'zmm') if widen else ('zmm', 'ymm')
        add(f'{op} 512 reg m hi', f'{op} %%{s}30, %%{d}17%{{%%k1%}}', tt, hi=True, zchk=False, **kw)
cvt_wn('vcvtps2pd', 'ps', True, 'sae')
cvt_wn('vcvtdq2pd', 'dq', True, None, table=True)
cvt_wn('vcvtudq2pd', 'dq', True, None, table=True)
for op in ('vcvtps2qq', 'vcvtps2uqq'):
    cvt_wn(op, 'ps', True, 'er', ibound=True)
for op in ('vcvttps2qq', 'vcvttps2uqq'):
    cvt_wn(op, 'ps', True, 'sae', ibound=True)
cvt_wn('vcvtpd2ps', 'pd', False, 'er')
cvt_wn('vcvtqq2ps', 'qq', False, 'er', table=True)
cvt_wn('vcvtuqq2ps', 'qq', False, 'er', table=True)
for op in ('vcvtpd2dq', 'vcvtpd2udq'):
    cvt_wn(op, 'pd', False, 'er', ibound=True)
for op in ('vcvttpd2dq', 'vcvttpd2udq'):
    cvt_wn(op, 'pd', False, 'sae', ibound=True)
# scalar conversions to a general register (the result moved to xmm1 for
# the hash) and from one (loaded from the memory buffer); r64 amd64 only
for t in ('ss', 'sd'):
    for tr in ('', 't'):
        for u in ('', 'u'):
            op = f'vcvt{tr}{t}2{u}si'
            for gw, g in ((32, 'eax'), (64, 'rax')):
                if gw == 64 and I386:
                    continue
                mv = f'\\n vmov{"q" if gw == 64 else "d"} %%{g}, %%xmm1'
                add(f'{op} r{gw} reg', f'{op} %%xmm3, %%{g}{mv}', t, gpr=True, ibound=True)
                add(f'{op} r{gw} mem', f'{op} (%1), %%{g}{mv}', t, gpr=True, ibound=True)
                add(f'{op} r{gw} evex reg', f'%{{evex%}} {op} %%xmm3, %%{g}{mv}', t, gpr=True, ibound=True)
                add(f'{op} r{gw} evex mem', f'%{{evex%}} {op} (%1), %%{g}{mv}', t, gpr=True, ibound=True)
                for r in (['sae'] if tr else [f'{c}-sae' for c in RC]):
                    add(f'{op} r{gw} {{{r}}}', f'{op} %{{{r}%}}, %%xmm3, %%{g}{mv}', t, gpr=True, ibound=True)
    for u in ('', 'u'):
        op = f'vcvt{u}si2{t}'
        for gw, g, sfx in ((32, 'eax', 'l'), (64, 'rax', 'q')):
            if gw == 64 and I386:
                continue
            ld = f'mov (%1), %%{g}\\n '
            add(f'{op} r{gw} reg', f'{ld}{op} %%{g}, %%xmm2, %%xmm1', t, gpr=True, table=True)
            add(f'{op} r{gw} mem', f'{op}{sfx} (%1), %%xmm2, %%xmm1', t, table=True)
            add(f'{op} r{gw} evex reg', f'{ld}%{{evex%}} {op} %%{g}, %%xmm2, %%xmm1', t, gpr=True, table=True)
            add(f'{op} r{gw} evex mem', f'%{{evex%}} {op}{sfx} (%1), %%xmm2, %%xmm1', t, table=True)
            if not (t == 'sd' and gw == 32):
                for c in RC:
                    add(f'{op} r{gw} {{{c}-sae}}', f'{ld}{op} %%{g}, %{{{c}-sae%}}, %%xmm2, %%xmm1', t, gpr=True,
                        table=True)                 # (gas: the rounding after the GPR)
for op, t, r in (('vcvtss2sd', 'ss', ['sae']), ('vcvtsd2ss', 'sd', [f'{c}-sae' for c in RC])):
    for mk, mn in MK:
        add(f'{op} reg{mn}', f'{op} %%xmm3, %%xmm2, %%xmm1{mk}', t, zchk=False)
        add(f'{op} mem{mn}', f'{op} (%1), %%xmm2, %%xmm1{mk}', t, zchk=False)
        for rr in r:
            add(f'{op} {{{rr}}} reg{mn}', f'{op} %{{{rr}%}}, %%xmm3, %%xmm2, %%xmm1{mk}', t, zchk=False)
    if not I386:
        add(f'{op} reg m hi', f'{op} %%xmm30, %%xmm25, %%xmm17%{{%%k1%}}', t, hi=True, zchk=False)
# VCVTPH2PS (halves of random bits) and VCVTPS2PH (imm8 roundings; to a
# register, or to memory, which is then read back into zmm1 for the hash)
for L, x in VL:
    h = HALF[L]
    for mk, mn in MK:
        add(f'vcvtph2ps {L} reg{mn}', f'vcvtph2ps %%{h}3, %%{x}1{mk}', 'ps', table=True, zchk=False)
        add(f'vcvtph2ps {L} mem{mn}', f'vcvtph2ps (%1), %%{x}1{mk}', 'ps', table=True, zchk=False)
for mk, mn in MK:
    add(f'vcvtph2ps {{sae}} 512 reg{mn}', f'vcvtph2ps %{{sae%}}, %%ymm3, %%zmm1{mk}', 'ps', table=True, zchk=False)
for imm in ('$0x0', '$0x1', '$0x2', '$0x3', '$0x4', '$0xf8'):
    for L, x in VL:
        h = HALF[L]
        for mk, mn in MK:
            add(f'vcvtps2ph {imm} {L} reg{mn}', f'vcvtps2ph {imm}, %%{x}3, %%{h}1{mk}', 'ps', zchk=False)
        for mk, mn in MK[:2]:
            add(f'vcvtps2ph {imm} {L} st{mn}', f'vcvtps2ph {imm}, %%{x}3, (%1){mk}\\n vmovdqu64 (%1), %%zmm1', 'ps',
                zchk=False)
    for mk, mn in MK:
        add(f'vcvtps2ph {imm} {{sae}} 512 reg{mn}', f'vcvtps2ph {imm}, %{{sae%}}, %%zmm3, %%ymm1{mk}', 'ps', zchk=False)
if not I386:
    add('vcvtph2ps 512 reg m hi', 'vcvtph2ps %%ymm30, %%zmm17%{%%k1%}', 'ps', hi=True, table=True, zchk=False)
    add('vcvtps2ph $0x0 512 reg m hi', 'vcvtps2ph $0x0, %%zmm30, %%ymm17%{%%k1%}', 'ps', hi=True, zchk=False)
# 62 P0 P1 P2 op modrm [imm]: vaddps zmm1, zmm2, zmm3 is 62 f1 6c 48 58 cb, and
# a memory operand is [eax]/[rax] = the memory buffer
UD = {
    'vaddps ok': '62 f1 6c 48 58 cb', 'vaddpd ok': '62 f1 ed 48 58 cb', 'vaddss ok': '62 f1 6e 08 58 cb',
    'vaddsd ok': '62 f1 ef 08 58 cb', 'vaddps z no mask': '62 f1 6c c8 58 cb', 'vaddss z no mask': '62 f1 6e 88 58 cb',
    'vaddps z mask': '62 f1 6c c9 58 cb',
    'vcmpps ok': '62 f1 6c 48 c2 cb 01', 'vcmpps z mask': '62 f1 6c ca c2 cb 01', 'vcmpps z no mask': '62 f1 6c c8 c2 cb 01',
    'vcmpss z mask': '62 f1 6e 8a c2 cb 01', 'vcmpss ok': '62 f1 6e 08 c2 cb 01',
    'vaddss mem bcst': '62 f1 6e 18 58 08', 'vaddsd mem bcst': '62 f1 ef 18 58 08',
    'vaddss mem bcst LL3': '62 f1 6e 78 58 08', 'vaddss mem ok': '62 f1 6e 08 58 08',
    'vcmpss mem bcst': '62 f1 6e 18 c2 08 01', 'vsqrtss mem bcst': '62 f1 6e 18 51 08',
    'vaddps mem bcst LL3': '62 f1 6c 78 58 08', 'vaddps mem bcst LL2': '62 f1 6c 58 58 08',
    'vaddps reg LL3': '62 f1 6c 68 58 cb', 'vaddps reg rz-sae': '62 f1 6c 78 58 cb', 'vaddps reg rn-sae': '62 f1 6c 18 58 cb',
    'vaddss reg LL3': '62 f1 6e 68 58 cb', 'vaddss reg rz-sae': '62 f1 6e 78 58 cb',
    'vminps reg sae LL0': '62 f1 6c 18 5d cb', 'vminps reg sae LL3': '62 f1 6c 78 5d cb',
    'vcmpps reg sae LL0': '62 f1 6c 18 c2 cb 01',
    'vaddps W1': '62 f1 ec 48 58 cb', 'vaddpd W0': '62 f1 6d 48 58 cb', 'vaddss W1': '62 f1 ee 08 58 cb',
    'vaddsd W0': '62 f1 6f 08 58 cb', 'vcmpps W1': '62 f1 ec 48 c2 cb 01', 'vcmppd W0': '62 f1 6d 48 c2 cb 01',
    'vsqrtps W1': '62 f1 fc 48 51 cb', 'vsqrtpd W0': '62 f1 7d 48 51 cb',
    'vsqrtps ok': '62 f1 7c 48 51 cb', 'vsqrtps vvvv': '62 f1 6c 48 51 cb', 'vsqrtpd ok': '62 f1 fd 48 51 cb',
    'vsqrtpd vvvv': '62 f1 ed 48 51 cb', 'vsqrtps mem vvvv': '62 f1 6c 48 51 08',
    # FMA: vfmadd231ps zmm1, zmm2, zmm3 is 62 f2 6d 48 b8 cb
    'vfmadd231ps ok': '62 f2 6d 48 b8 cb', 'vfmadd231ss LL3': '62 f2 6d 68 b9 cb',
    'vfmadd231ss rz-sae': '62 f2 6d 78 b9 cb', 'vfmadd231ps mem bcst LL3': '62 f2 6d 78 b8 08',
    'vfmadd231ps z no mask': '62 f2 6d c8 b8 cb', 'vfmaddsub231ps ok': '62 f2 6d 48 b6 cb',
    'vfmadd231ss mem bcst': '62 f2 6d 18 b9 08',
}
if not I386:
    UD.update({"vsqrtps V'": '62 f1 7c 40 51 cb', "vcmpps R'": '62 e1 6c 48 c2 cb 01',
               'vcmpps R': '62 71 6c 48 c2 cb 01', "vaddps V' mem": '62 f1 34 40 58 08'})   # (V' and vvvv 1001: s1 = zmm25, which LOAD sets)
for k, b in UD.items():
    F.append((f'ud {k}', 'ud:' + ', '.join('0x' + x for x in b.split()), 0))
R = 'e' if I386 else 'r'
o = []; w = o.append
w(f'''// Generated by tools/gen-avx512-float-test.py{" --i386" if I386 else ""} -- do not edit.
//
// AVX-512 floating-point arithmetic and compares, packed and scalar, with
// masking, broadcasts, static rounding and SAE, under seven MXCSRs, against
// Intel SDE (see the generator). (No target pragma: the harness itself must
// not use AVX-512.)
#include <setjmp.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static uint64_t rs;
static uint64_t rnd(void) {{ rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }}
// d: the destination's old value, a s1, b s2, m the memory operand, k the
// opmask (k1, and k2 for a compare), kout k1 after, mx the MXCSR the case
// runs under, mxo MXCSR after, mxd the default MXCSR put back
struct st {{ uint8_t d[64], a[64], b[64], out[64], m[64]; uint64_t k, kout; uint32_t mx, mxo, mxd; }} __attribute__((aligned(64)));
_Static_assert(offsetof(struct st, k) == 320 && offsetof(struct st, kout) == 328 && offsetof(struct st, mx) == 336 &&
               offsetof(struct st, mxo) == 340 && offsetof(struct st, mxd) == 344, "struct st layout");
static sigjmp_buf jb;
static volatile int sig;
static void handler(int s) {{ sig = s; siglongjmp(jb, 1); }}
#define LOAD "vmovdqu64 (%0), %%zmm1\\n vmovdqu64 64(%0), %%zmm2\\n vmovdqu64 128(%0), %%zmm3\\n" \\
''' + ('''    "vmovdqu64 (%0), %%zmm17\\n vmovdqu64 64(%0), %%zmm25\\n vmovdqu64 128(%0), %%zmm30\\n" \\
''' if not I386 else '') + '''    "kmovq 320(%0), %%k1\\n kmovq 320(%0), %%k2\\n ldmxcsr 336(%0)\\n"
#define SAVE(z) "\\n stmxcsr 340(%0)\\n ldmxcsr 344(%0)\\n vmovdqu64 %%" z ", 192(%0)\\n kmovq %%k1, 328(%0)"
''')
for i, (name, asm, fl) in enumerate(F):
    if asm.startswith('ud:'):
        w(f'''__attribute__((noinline)) static void f{i}(struct st *t) {{
    __asm__ volatile(LOAD "mov %1, %%{R}ax\\n .byte {asm[3:]}" SAVE("zmm1") :: "r"(t), "r"(t->m) : "memory", "{R}ax", "xmm1", "xmm2", "xmm3");
}}''')
    else:
        out = 'zmm17' if fl & 4 else 'zmm1'   # (zmm16-31: gcc does not use them here)
        gx = f', "{R}ax"' if fl & 65536 else ''
        w(f'''__attribute__((noinline)) static void f{i}(struct st *t) {{
    __asm__ volatile(LOAD "{asm}" SAVE("{out}") :: "r"(t), "r"(t->m) : "memory", "xmm1", "xmm2", "xmm3"{gx});
}}''')
w('static const struct { const char *name; void (*fn)(struct st *); int fl; } forms[] = {')
for i, (name, asm, fl) in enumerate(F):
    w(f'    {{"{name}", f{i}, {fl}}},')
w('};')
w(f'#define NF {len(F)}')
if answers:
    w('static const uint64_t want[NF] = {')
    for name, asm, fl in F:
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
// the four roundings, FTZ+DAZ, FTZ alone, DAZ alone; flags clear
static const uint32_t modes[] = {0x1f80, 0x3f80, 0x5f80, 0x7f80, 0x9fc0, 0x9f80, 0x1fc0};
#define NM 7
#define NJ 6   /* cases per mode */
// special values: +-0, +-inf, QNaNs and SNaNs with payloads, denormals,
// +-min normal, +-max finite, +-1.0, 1+ulp, 2-ulp, 2^(emax), values whose
// squares overflow or underflow, 2^-(p+1) (a tie added to 1.0), 2^p, 2^p-1
static const uint32_t sv[] = {0, 0x80000000, 0x7f800000, 0xff800000, 0x7fc00000, 0xffc12345, 0x7f800001,
    0xff812345, 0x7fa00000, 0x00000001, 0x807fffff, 0x00400000, 0x00800000, 0x80800000, 0x7f7fffff, 0xff7fffff,
    0x3f800000, 0xbf800000, 0x3f800001, 0x3fffffff, 0x7f000000, 0x5f800000, 0x1f800000, 0x20000000, 0x00800001,
    0x33800000, 0x4b800000, 0x4b7fffff};
static const uint64_t dv[] = {0, 0x8000000000000000ull, 0x7ff0000000000000ull, 0xfff0000000000000ull,
    0x7ff8000000000000ull, 0xfff8000012345678ull, 0x7ff0000000000001ull, 0xfff0000012345678ull,
    0x7ff4000000000000ull, 1, 0x800fffffffffffffull, 0x0008000000000000ull, 0x0010000000000000ull,
    0x8010000000000000ull, 0x7fefffffffffffffull, 0xffefffffffffffffull, 0x3ff0000000000000ull,
    0xbff0000000000000ull, 0x3ff0000000000001ull, 0x3fffffffffffffffull, 0x7fe0000000000000ull,
    0x5ff0000000000000ull, 0x1ff0000000000000ull, 0x2000000000000000ull, 0x0010000000000001ull,
    0x3ca0000000000000ull, 0x4340000000000000ull, 0x433fffffffffffffull};
#define NSV (sizeof sv / sizeof sv[0])
#define NDV (sizeof dv / sizeof dv[0])
static uint64_t elem(int dbl) {
    uint64_t r = rnd(), s = rnd(), sel = r & 7, e;
    if (sel < 3)
        return dbl ? dv[(r >> 8) % NDV] : sv[(r >> 8) % NSV];
    int eb = dbl ? 11 : 8, mb = dbl ? 52 : 23;
    uint64_t emax = (1u << eb) - 1, bias = emax >> 1;
    if (sel < 6) e = (r >> 8) & emax;                                   /* anywhere (inf/NaN too) */
    else if (sel == 6) e = bias - 3 + (r >> 8) % 7;                     /* near 1, often exact */
    else e = (r >> 9) & 1 ? (r >> 10) % 24 : emax - 1 - (r >> 10) % 24;   /* near the edges */
    uint64_t man = s & ((1ull << mb) - 1);
    if (sel == 6) man &= ~((1ull << (mb / 2)) - 1);
    return (r >> 63) << (eb + mb) | e << mb | man;
}
static void put(uint8_t *p, int dbl, int i, uint64_t v) { memcpy(p + i * (dbl ? 8 : 4), &v, dbl ? 8 : 4); }
static uint64_t get(const uint8_t *p, int dbl, int i) { uint64_t v = 0; memcpy(&v, p + i * (dbl ? 8 : 4), dbl ? 8 : 4); return v; }
static void fill(struct st *t, int dbl) {
    int n = dbl ? 8 : 16;
    for (int i = 0; i < n; i++) {
        put(t->d, dbl, i, elem(dbl));
        put(t->a, dbl, i, elem(dbl));
        put(t->b, dbl, i, elem(dbl));
        put(t->m, dbl, i, elem(dbl));
        uint64_t r = rnd() & 7, sign = dbl ? 1ull << 63 : 1u << 31;
        if (r < 2) { put(t->b, dbl, i, get(t->a, dbl, i)); put(t->m, dbl, i, get(t->a, dbl, i)); }   /* equal */
        else if (r == 2) { put(t->b, dbl, i, get(t->a, dbl, i) ^ sign); put(t->m, dbl, i, get(t->a, dbl, i) ^ sign); }
    }
}
// enabled lanes 1.0 (no flag from any op); each masked-off lane an SNaN, a
// denormal, a zero divisor or max finite against -3 (overflow, inexact,
// sqrt invalid), rotated by mode so the scalar lane sees each
static void quiet_lanes(struct st *t, int dbl, int mode, int dst) {
    int n = dbl ? 8 : 16;
    uint64_t one = dbl ? 0x3ff0000000000000ull : 0x3f800000, snan = dbl ? 0x7ff0000000000123ull : 0x7f800123,
             den = dbl ? 0x000fedcba9876543ull : 0x00654321, maxf = dbl ? 0x7fefffffffffffffull : 0x7f7fffff,
             m3 = dbl ? 0xc008000000000000ull : 0xc0400000;
    for (int i = 0; i < n; i++) {
        uint64_t a = one, b = one;
        if (!(t->k >> i & 1)) {
            switch ((i + mode) & 3) {
            case 0: a = snan; break;
            case 1: a = den; break;
            case 2: b = 0; break;
            default: a = maxf; b = m3; break;
            }
        }
        put(t->a, dbl, i, a);
        put(t->b, dbl, i, b);
        put(t->m, dbl, i, b);
        if (dst)                                 /* (FMA: the destination is an operand) */
            put(t->d, dbl, i, a);
    }
}

int main(int argc, char **argv) {
    int print = argc > 1 && !strcmp(argv[1], "hashes"), dump = argc > 1 && !strcmp(argv[1], "dump");
    unsigned long checks = 0, bad = 0;
    static struct st t;
    signal(SIGILL, handler);
    signal(SIGSEGV, handler);
    for (int fi = 0; fi < NF; fi++) {
        uint64_t h = 0xcbf29ce484222325ull;
        rs = 0x9e3779b97f4a7c15ull;
        for (const char *c = forms[fi].name; *c; c++) rs = (rs ^ (uint8_t) *c) * 0x100000001b3ull;
        int ud = !strncmp(forms[fi].name, "ud ", 3), dbl = forms[fi].fl & 1;
        for (int c = 0; c < (ud ? 1 : NM * NJ); c++) {
            int mode = c % NM, j = c / NM;
            fill(&t, dbl);
            t.k = rnd();
            if (j == 1) quiet_lanes(&t, dbl, mode, forms[fi].fl & 1024);
            if (forms[fi].fl & 16384)                     /* a VFIXUPIMM table: random bits */
                for (int i = 0; i < 64; i += 8) {
                    uint64_t r = rnd();
                    memcpy(t.b + i, &r, 8);
                    r = rnd();
                    memcpy(t.m + i, &r, 8);
                }
            if (forms[fi].fl & 32768)                     /* the integer ranges' edges */
                for (int i = 0; i < (dbl ? 8 : 16); i++) {
                    static const uint32_t fe[] = {0x4f000000, 0xcf000000, 0x4effffff, 0xcf000001, 0x4f800000, 0x4f7fffff,
                                                  0x5f000000, 0xdf000000, 0x5effffff, 0x5f800000, 0x5f7fffff, 0x3f000000,
                                                  0xbf000000, 0xbf800000, 0xbf7fffff, 0x3fc00000, 0xbfc00000, 0x40200000};
                    static const uint64_t de[] = {0x41e0000000000000ull, 0xc1e0000000000000ull, 0x41dfffffffffffffull,
                                                  0x41dfffffffe00000ull, 0xc1e0000000100000ull, 0x41efffffffe00000ull,
                                                  0x41f0000000000000ull, 0x43e0000000000000ull, 0xc3e0000000000000ull,
                                                  0x43dfffffffffffffull, 0x43f0000000000000ull, 0x43efffffffffffffull,
                                                  0x3fe0000000000000ull, 0xbfe0000000000000ull, 0xbff0000000000000ull,
                                                  0xbfefffffffffffffull, 0x41dfffffffd00000ull, 0xc1e00000001fffffull};
                    uint64_t r = rnd();
                    if (r & 1)
                        continue;
                    uint64_t v = dbl ? de[(r >> 8) % 18] : fe[(r >> 8) % 18];
                    put(t.b, dbl, i, v);
                    put(t.m, dbl, i, v);
                }
            if (forms[fi].fl & 2048)
                for (int i = 16 >> (2 + dbl); i < (dbl ? 8 : 16); i++) {
                    uint64_t one = dbl ? 0x3ff0000000000000ull : 0x3f800000;
                    put(t.d, dbl, i, one);
                    put(t.a, dbl, i, one);
                    put(t.b, dbl, i, one);
                    put(t.m, dbl, i, one);
                }
            if (j == 2) t.k = mode & 1 ? ~0ull : 0;
            t.mx = ud ? 0x1f80 : modes[mode];
            t.mxd = 0x1f80;
            t.mxo = 0xdead;
            t.kout = 0;
            memset(t.out, 0, 64);
            int sg = run(forms[fi].fn, &t);
            if ((forms[fi].fl & 8) && sg == 0) {          /* zero-masked: those elements must be 0 */
                int n = (forms[fi].fl >> 4) & 31, es = dbl ? 8 : 4, nz = 0;
                for (int i = 0; i < n; i++) {
                    if (t.k >> i & 1)
                        continue;
                    for (int b = 0; b < es; b++) nz |= t.out[i * es + b];
                    memset(t.out + i * es, 0, es);
                }
                if (nz && !print && !dump && bad++ < 40)
                    printf("FAIL %s (case %d): a zero-masked element is not 0\n", forms[fi].name, c);
            }
            if ((forms[fi].fl & 4096) && sg == 0) {      /* imm8 bit 3: no PE; RNDSCALE: no DE */
                if ((t.mxo & 0x22) && !print && !dump && bad++ < 40)
                    printf("FAIL %s (case %d): PE or DE raised (MXCSR %#x)\n", forms[fi].name, c, t.mxo);
                t.mxo &= ~0x22u;
            }
            if ((forms[fi].fl & 8192) && sg == 0) {      /* VSCALEF: see the generator */
                if (t.mx & 0x40)
                    t.mxo &= ~0x38u;
                if ((t.mx & 0x8000) || (t.mxo & 0x18))
                    t.mxo &= ~0x02u;
            }
            for (int i = 0; i < 64; i++) h = (h ^ t.out[i]) * 0x100000001b3ull;
            for (int b = 0; b < 8; b++) h = (h ^ ((t.kout >> (8 * b)) & 0xff)) * 0x100000001b3ull;
            h = (h ^ (t.mxo & 0xffff)) * 0x100000001b3ull;
            h = (h ^ (unsigned) sg) * 0x100000001b3ull;
            if (dump) {
                printf("%s %d: mxcsr %#x sig %d mxcsr' %#x k1 %016llx out ", forms[fi].name, c, t.mx, sg, t.mxo,
                       (unsigned long long) t.kout);
                for (int i = 63; i >= 0; i--) printf("%02x", t.out[i]);
                printf("\n");
            }
        }
        if (print) printf("%s %#llx\n", forms[fi].name, (unsigned long long) h);
        checks++;
        if (!print && !dump && h != want[fi] && bad++ < 40)
            printf("FAIL %s\n", forms[fi].name);
    }
    if (!print && !dump)
        printf("NAME: %s (%d forms, %lu checks, %lu mismatches)\n", bad ? "FAIL" : "PASS", NF, checks, bad);
    return bad != 0;
}''')
open(args[0], 'w').write('\n'.join(o).replace('NAME: %s', NAME + ': %s'))
print(len(F), 'forms')
