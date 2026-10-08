#!/usr/bin/env python3
# Generates tests/manual/x86/amd64_avx512_perm.c, or with --i386
# i386_avx512_perm.c (usage: this script [--i386] [--answers FILE] OUT.c).
#
# AVX-512 lane-crossing and shuffle ops: VPERMB/W/D/Q, VPERMPS/PD (variable
# and the qword imm8 forms), VPERMI2* and VPERMT2* (B/W/D/Q/PS/PD), VALIGND/Q,
# VSHUFI/F32x4/64x2, VINSERTI/F32x4/64x2/32x8/64x4, VEXTRACTI/F (to a
# register and to memory, masked), VPSHUFD/HW/LW, VSHUFPS/PD, VPERMILPS/PD
# (imm8 and variable), VPALIGNR, VUNPCKL/HPS/PD: VL 128/256/512 where the op
# has it, register, memory and {1toN} sources, unmasked, merge- and
# zero-masked, imm8s across their fields and indices past the table. Then
# encodings whose #UD-or-not is part of the answer. The destination's 64
# bytes, the memory and the signal are hashed per form and checked against
# Intel SDE's (`sde64 -ptr-raise -spr -- ./test hashes` on camd, passed
# back with --answers); the i386 test takes the amd64 answers.
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
NAME = 'i386_avx512_perm' if I386 else 'amd64_avx512_perm'
VLS = {128: 'xmm', 256: 'ymm', 512: 'zmm'}
MK = [('', ''), ('%{%%k1%}', 'm'), ('%{%%k1%}%{z%}', 'z')]
F = []
def add(name, asm):
    F.append((name, asm))
def three(op, ls, bc, imm=None, masks=MK):
    """op with dst, s1 (vvvv), s2 (r/m): register, memory and {1toN}"""
    im = '' if imm is None else f'${imm}, '
    nm = op if imm is None else f'{op} ${imm}'
    for L in ls:
        x = VLS[L]
        for mk, mn in masks:
            add(f'{nm} {L} reg{mn}', f'{op} {im}%%{x}3, %%{x}2, %%{x}1{mk}')
        add(f'{nm} {L} mem m', f'{op} {im}(%1), %%{x}2, %%{x}1%{{%%k1%}}')
        if bc:
            add(f'{nm} {L} bcst z', f'{op} {im}(%1)%{{1to{L // (8 * bc)}%}}, %%{x}2, %%{x}1%{{%%k1%}}%{{z%}}')
def two(op, ls, bc, imm=None):
    """op with dst and a source r/m (vvvv 1111b)"""
    im = '' if imm is None else f'${imm}, '
    nm = op if imm is None else f'{op} ${imm}'
    for L in ls:
        x = VLS[L]
        for mk, mn in MK:
            add(f'{nm} {L} reg{mn}', f'{op} {im}%%{x}3, %%{x}1{mk}')
        add(f'{nm} {L} mem m', f'{op} {im}(%1), %%{x}1%{{%%k1%}}')
        if bc:
            add(f'{nm} {L} bcst z', f'{op} {im}(%1)%{{1to{L // (8 * bc)}%}}, %%{x}1%{{%%k1%}}%{{z%}}')
ALL = (128, 256, 512)
for op, bc in (('vpermd', 4), ('vpermq', 8), ('vpermps', 4), ('vpermpd', 8)):
    three(op, (256, 512), bc)
for op in ('vpermb', 'vpermw'):
    three(op, ALL, 0)
for op in ('vpermq', 'vpermpd'):
    for imm in (0x1b, 0xe4, 0x00, 0xff, 0x4e, 0x93):
        two(op, (256, 512), 8, imm)
for kind in ('i2', 't2'):
    for t, bc in (('b', 0), ('w', 0), ('d', 4), ('q', 8), ('ps', 4), ('pd', 8)):
        three(f'vperm{kind}{t}', ALL, bc)
for op, bc in (('valignd', 4), ('valignq', 8)):
    for imm in (0, 1, 3, 5, 7, 9, 15, 17, 255):
        three(op, ALL, bc, imm)
for op, bc in (('vshufi32x4', 4), ('vshufi64x2', 8), ('vshuff32x4', 4), ('vshuff64x2', 8)):
    for imm in (0x00, 0x1b, 0xe4, 0x4e, 0xb1, 0xff, 0x01, 0x02):
        three(op, (256, 512), bc, imm)
for op in ('vinserti32x4', 'vinserti64x2', 'vinsertf32x4', 'vinsertf64x2'):
    for imm in (0, 1, 2, 3, 7):
        for L in (256, 512):
            x = VLS[L]
            for mk, mn in MK:
                add(f'{op} ${imm} {L} reg{mn}', f'{op} ${imm}, %%xmm3, %%{x}2, %%{x}1{mk}')
            add(f'{op} ${imm} {L} mem m', f'{op} ${imm}, (%1), %%{x}2, %%{x}1%{{%%k1%}}')
for op in ('vinserti32x8', 'vinserti64x4', 'vinsertf32x8', 'vinsertf64x4'):
    for imm in (0, 1, 2):
        for mk, mn in MK:
            add(f'{op} ${imm} 512 reg{mn}', f'{op} ${imm}, %%ymm3, %%zmm2, %%zmm1{mk}')
        add(f'{op} ${imm} 512 mem m', f'{op} ${imm}, (%1), %%zmm2, %%zmm1%{{%%k1%}}')
for op in ('vextracti32x4', 'vextracti64x2', 'vextractf32x4', 'vextractf64x2'):
    for imm in (0, 1, 2, 3, 6):
        for L in (256, 512):
            x = VLS[L]
            for mk, mn in MK:
                add(f'{op} ${imm} {L} reg{mn}', f'{op} ${imm}, %%{x}3, %%xmm1{mk}')
            for mk, mn in MK[:2]:
                add(f'{op} ${imm} {L} st{mn}', f'{op} ${imm}, %%{x}3, (%1){mk}')
for op in ('vextracti32x8', 'vextracti64x4', 'vextractf32x8', 'vextractf64x4'):
    for imm in (0, 1, 3):
        for mk, mn in MK:
            add(f'{op} ${imm} 512 reg{mn}', f'{op} ${imm}, %%zmm3, %%ymm1{mk}')
        for mk, mn in MK[:2]:
            add(f'{op} ${imm} 512 st{mn}', f'{op} ${imm}, %%zmm3, (%1){mk}')
for imm in (0x1b, 0x00, 0xe4, 0x4e, 0x39, 0xff):
    two('vpshufd', ALL, 4, imm)
    two('vpshufhw', ALL, 0, imm)
    two('vpshuflw', ALL, 0, imm)
    two('vpermilps', ALL, 4, imm)
    three('vshufps', ALL, 4, imm)
for imm in (0x00, 0xff, 0x55, 0xaa, 0x96, 0x3c):
    two('vpermilpd', ALL, 8, imm)
    three('vshufpd', ALL, 8, imm)
three('vpermilps', ALL, 4)
three('vpermilpd', ALL, 8)
for imm in (0, 1, 4, 8, 15, 16, 17, 31, 32, 200):
    three('vpalignr', ALL, 0, imm)
for op, bc in (('vunpcklps', 4), ('vunpckhps', 4), ('vunpcklpd', 8), ('vunpckhpd', 8)):
    three(op, ALL, bc)
UD = {'vpermd 128': '62 f2 6d 08 36 cb', 'vpermd ok': '62 f2 6d 48 36 cb', 'vpermq imm 128': '62 f3 fd 08 00 cb 1b',
      'vpermq imm vvvv': '62 f3 ed 48 00 cb 1b', 'vshufi32x4 128': '62 f3 6d 08 43 cb 01',
      'vinserti32x4 128': '62 f3 6d 08 38 cb 01', 'vinserti32x8 256': '62 f3 6d 28 3a cb 01',
      'vextracti32x4 128': '62 f3 7d 08 39 d9 01', 'vextracti32x4 vvvv': '62 f3 6d 48 39 d9 01',
      'vextracti32x4 st z': '62 f3 7d c9 39 18 01', 'vpermb ok': '62 f2 6d 48 8d cb',
      'vpermi2d W1 (q)': '62 f2 ed 48 76 cb', 'valignd bcst reg': '62 f3 6d 58 03 cb 01',
      'vpshufd vvvv': '62 f1 6d 48 70 cb 1b', 'vpalignr bcst': '62 f3 6d 58 0f 08 01'}
for k, b in UD.items():
    F.append((f'ud {k}', 'ud:' + ', '.join('0x' + x for x in b.split())))
R = 'e' if I386 else 'r'
o = []; w = o.append
w(f'''// Generated by tools/gen-avx512-perm-test.py{" --i386" if I386 else ""} -- do not edit.
//
// AVX-512 lane-crossing and shuffle ops against Intel SDE (see the
// generator). (No target pragma: the harness itself must not use AVX-512.)
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static uint64_t rs;
static uint64_t rnd(void) {{ rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }}
// d: the destination's old value (and an index for VPERMI2), a s1, b s2,
// m the memory operand, k the opmask
struct st {{ uint8_t d[64], a[64], b[64], out[64], m[64]; uint64_t k; }} __attribute__((aligned(64)));
static sigjmp_buf jb;
static volatile int sig;
static void handler(int s) {{ sig = s; siglongjmp(jb, 1); }}
#define LOAD "vmovdqu64 (%0), %%zmm1\\n vmovdqu64 64(%0), %%zmm2\\n vmovdqu64 128(%0), %%zmm3\\n kmovq 320(%0), %%k1\\n"
''')
for i, (name, asm) in enumerate(F):
    if asm.startswith('ud:'):
        w(f'''__attribute__((noinline)) static void f{i}(struct st *t) {{
    __asm__ volatile(LOAD "mov %1, %%{R}ax\\n .byte {asm[3:]}\\n vmovdqu64 %%zmm1, 192(%0)" :: "r"(t), "r"(t->m) : "memory", "{R}ax", "xmm1", "xmm2", "xmm3");
}}''')
    else:
        w(f'''__attribute__((noinline)) static void f{i}(struct st *t) {{
    __asm__ volatile(LOAD "{asm}\\n vmovdqu64 %%zmm1, 192(%0)" :: "r"(t), "r"(t->m) : "memory", "xmm1", "xmm2", "xmm3");
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
        int ud = !strncmp(forms[fi].name, "ud ", 3);
        for (int k = 0; k < (ud ? 1 : 16); k++) {
            for (int i = 0; i < 64; i += 8) {
                uint64_t r;
                r = rnd(); memcpy(t.d + i, &r, 8);
                r = rnd(); memcpy(t.a + i, &r, 8);
                r = rnd(); memcpy(t.b + i, &r, 8);
                r = rnd(); memcpy(t.m + i, &r, 8);
            }
            if (k & 1) {                         /* small indices too, so the tables' low ends get picked */
                for (int i = 0; i < 64; i++) { t.a[i] &= 0x8f; t.d[i] &= 0x8f; }
            }
            t.k = rnd();
            if (k == 2) t.k = 0;
            if (k == 3) t.k = ~0ull;
            memset(t.out, 0, 64);
            int sg = run(forms[fi].fn, &t);
            for (int i = 0; i < 64; i++) h = (h ^ t.out[i]) * 0x100000001b3ull;
            for (int i = 0; i < 64; i++) h = (h ^ t.m[i]) * 0x100000001b3ull;
            h = (h ^ (unsigned) sg) * 0x100000001b3ull;
            if (dump) {
                printf("%s %d: sig %d out ", forms[fi].name, k, sg);
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
        printf("NAME: %s (%d forms, %lu checks, %lu mismatches)\n", bad ? "FAIL" : "PASS", NF, checks, bad);
    return bad != 0;
}''')
open(args[0], 'w').write('\n'.join(o).replace('NAME: %s', NAME + ': %s'))
print(len(F), 'forms')
