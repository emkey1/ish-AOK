#!/usr/bin/env python3
# Generates tests/manual/x86/amd64_avx512_misc.c, or with --i386
# i386_avx512_misc.c (usage: this script [--i386] [--answers FILE] OUT.c).
#
# AVX-512 shifts (by imm8, by an xmm count, per element), rotates, the qword
# ops (VPMINSQ/UQ, VPMAXSQ/UQ, VPABSQ, VPMULLQ), VPTERNLOGD/Q over a spread of
# truth tables, VPBLENDM*, every broadcast (an element, 32x2/32x4/64x2/
# 32x8/64x4, from a GPR) and the non-temporal moves, unmasked, merge- and
# zero-masked where allowed, register, memory and {1toN} sources; counts are
# kept small often enough to land inside the width. Then encodings whose
# #UD-or-not is part of the answer. The destination's 64 bytes, the memory
# and the signal are hashed per form and checked against Intel SDE's
# (`sde64 -ptr-raise -spr -- ./test hashes` on camd, passed back with
# --answers); the i386 test takes the amd64 answers.
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
NAME = 'i386_avx512_misc' if I386 else 'amd64_avx512_misc'
VL = [(128, 'xmm'), (256, 'ymm'), (512, 'zmm')]
MK = [('', ''), ('%{%%k1%}', 'm'), ('%{%%k1%}%{z%}', 'z')]
F = []   # (name, asm, counts: 0 none / e element bytes of small per-element counts / 'x' the low qword)
def add(name, asm, cnt=0):
    F.append((name, asm, cnt))
# shifts and rotates by imm8: (op, element bytes for {1toN}, the counts)
SHI = [('vpsrlw', 0), ('vpsraw', 0), ('vpsllw', 0), ('vpsrld', 4), ('vpsrad', 4), ('vpslld', 4), ('vpsrlq', 8),
       ('vpsraq', 8), ('vpsllq', 8), ('vprold', 4), ('vprolq', 8), ('vprord', 4), ('vprorq', 8)]
for op, bc in SHI:
    for c in (0, 1, 7, 15, 16, 31, 32, 63, 64, 200):
        for mk, mn in MK:
            add(f'{op} ${c} 512 reg{mn}', f'{op} ${c}, %%zmm3, %%zmm1{mk}')
    for L, x in VL:
        add(f'{op} $5 {L} reg m', f'{op} $5, %%{x}3, %%{x}1%{{%%k1%}}')
        add(f'{op} $5 {L} mem m', f'{op} $5, (%1), %%{x}1%{{%%k1%}}')
        if bc:
            add(f'{op} $5 {L} bcst z', f'{op} $5, (%1)%{{1to{L // (8 * bc)}%}}, %%{x}1%{{%%k1%}}%{{z%}}')
for op in ('vpsrldq', 'vpslldq'):
    for c in (0, 1, 5, 15, 16, 200):
        for L, x in VL:
            add(f'{op} ${c} {L} reg', f'{op} ${c}, %%{x}3, %%{x}1')
    add(f'{op} $3 512 mem', f'{op} $3, (%1), %%zmm1')
# by an xmm count
for op in ('vpsrlw', 'vpsraw', 'vpsllw', 'vpsrld', 'vpsrad', 'vpslld', 'vpsrlq', 'vpsraq', 'vpsllq'):
    for L, x in VL:
        for mk, mn in MK:
            add(f'{op} xmm {L} reg{mn}', f'{op} %%xmm3, %%{x}2, %%{x}1{mk}', 'x')
        add(f'{op} m128 {L} mem m', f'{op} (%1), %%{x}2, %%{x}1%{{%%k1%}}', 'x')
# per element
SHV = [('vpsrlvd', 4, 4), ('vpsrlvq', 8, 8), ('vpsravd', 4, 4), ('vpsravq', 8, 8), ('vpsllvd', 4, 4), ('vpsllvq', 8, 8),
       ('vpsrlvw', 0, 2), ('vpsravw', 0, 2), ('vpsllvw', 0, 2), ('vprolvd', 4, 4), ('vprolvq', 8, 8),
       ('vprorvd', 4, 4), ('vprorvq', 8, 8)]
for op, bc, e in SHV:
    for L, x in VL:
        for mk, mn in MK:
            add(f'{op} {L} reg{mn}', f'{op} %%{x}3, %%{x}2, %%{x}1{mk}', e)
        add(f'{op} {L} mem m', f'{op} (%1), %%{x}2, %%{x}1%{{%%k1%}}', e)
        if bc:
            add(f'{op} {L} bcst m', f'{op} (%1)%{{1to{L // (8 * bc)}%}}, %%{x}2, %%{x}1%{{%%k1%}}', e)
# the qword ops
for op, un in (('vpminsq', 0), ('vpminuq', 0), ('vpmaxsq', 0), ('vpmaxuq', 0), ('vpabsq', 1), ('vpmullq', 0)):
    s1 = '' if un else ', %%{x}2'
    for L, x in VL:
        for mk, mn in MK:
            add(f'{op} {L} reg{mn}', f'{op} %%{x}3{s1.format(x=x)}, %%{x}1{mk}')
        add(f'{op} {L} mem m', f'{op} (%1){s1.format(x=x)}, %%{x}1%{{%%k1%}}')
        add(f'{op} {L} bcst z', f'{op} (%1)%{{1to{L // 64}%}}{s1.format(x=x)}, %%{x}1%{{%%k1%}}%{{z%}}')
# VPTERNLOG
for op, n in (('vpternlogd', 16), ('vpternlogq', 8)):
    for imm in (0x00, 0xff, 0x96, 0xe8, 0xca, 0x5a, 0x01, 0x80, 0x3c, 0xb4):
        for mk, mn in MK:
            add(f'{op} ${imm:#x} 512 reg{mn}', f'{op} ${imm:#x}, %%zmm3, %%zmm2, %%zmm1{mk}')
    for L, x in VL:
        add(f'{op} $0xca {L} reg m', f'{op} $0xca, %%{x}3, %%{x}2, %%{x}1%{{%%k1%}}')
        add(f'{op} $0x96 {L} mem m', f'{op} $0x96, (%1), %%{x}2, %%{x}1%{{%%k1%}}')
        add(f'{op} $0xe8 {L} bcst z', f'{op} $0xe8, (%1)%{{1to{L * n // 512}%}}, %%{x}2, %%{x}1%{{%%k1%}}%{{z%}}')
# VPBLENDM
for op, bc in (('vpblendmb', 0), ('vpblendmw', 0), ('vpblendmd', 4), ('vpblendmq', 8), ('vblendmps', 4), ('vblendmpd', 8)):
    for L, x in VL:
        for mk, mn in MK:
            add(f'{op} {L} reg{mn}', f'{op} %%{x}3, %%{x}2, %%{x}1{mk}')
        add(f'{op} {L} mem m', f'{op} (%1), %%{x}2, %%{x}1%{{%%k1%}}')
        if bc:
            add(f'{op} {L} bcst m', f'{op} (%1)%{{1to{L // (8 * bc)}%}}, %%{x}2, %%{x}1%{{%%k1%}}')
# broadcasts: (op, VLs, sources: x = an xmm, m = memory, g = a GPR)
R32 = '%%ecx'
BC = [('vpbroadcastb', (128, 256, 512), 'xmg'), ('vpbroadcastw', (128, 256, 512), 'xmg'),
      ('vpbroadcastd', (128, 256, 512), 'xmg'), ('vpbroadcastq', (128, 256, 512), 'xm' + ('' if I386 else 'g')),
      ('vbroadcastss', (128, 256, 512), 'xm'), ('vbroadcastsd', (256, 512), 'xm'),
      ('vbroadcasti32x2', (128, 256, 512), 'xm'), ('vbroadcastf32x2', (256, 512), 'xm'),
      ('vbroadcasti32x4', (256, 512), 'm'), ('vbroadcasti64x2', (256, 512), 'm'), ('vbroadcastf32x4', (256, 512), 'm'),
      ('vbroadcastf64x2', (256, 512), 'm'), ('vbroadcasti32x8', (512,), 'm'), ('vbroadcasti64x4', (512,), 'm'),
      ('vbroadcastf32x8', (512,), 'm'), ('vbroadcastf64x4', (512,), 'm')]
for op, ls, srcs in BC:
    for L in ls:
        x = {128: 'xmm', 256: 'ymm', 512: 'zmm'}[L]
        for mk, mn in MK:
            if 'x' in srcs:
                add(f'{op} {L} xmm{mn}', f'{op} %%xmm3, %%{x}1{mk}')
            if 'm' in srcs:
                add(f'{op} {L} mem{mn}', f'{op} (%1), %%{x}1{mk}')
            if 'g' in srcs:
                g = '%%rcx' if op == 'vpbroadcastq' else R32
                add(f'{op} {L} gpr{mn}', f'{op} {g}, %%{x}1{mk}')
# the non-temporal moves (the memory operand is 64-byte aligned)
for op in ('vmovntdq', 'vmovntps', 'vmovntpd'):
    for L, x in VL:
        add(f'{op} {L} st', f'{op} %%{x}3, (%1)')
for L, x in VL:
    add(f'vmovntdqa {L} ld', f'vmovntdqa (%1), %%{x}1')
UD = {'vmovntdq mask': '62 f1 7d 49 e7 08', 'vmovntdq reg': '62 f1 7d 48 e7 cb', 'vbroadcastsd L0': '62 f2 fd 08 19 cb',
      'vbroadcasti32x4 reg': '62 f2 7d 48 5a cb', 'vbroadcasti32x8 L1': '62 f2 7d 28 5b 08',
      'vpsrldq mask': '62 f1 75 49 73 db 03', 'vpbroadcastd gpr mem': '62 f2 7d 48 7c 08',
      'vpsrld /3': '62 f1 75 48 72 db 03', 'vpternlogd ok': '62 f3 6d 48 25 cb 96',
      'vpsravw W0': '62 f2 6d 48 11 cb', 'vpabsq W0 (vpabsd)': '62 f2 7d 48 1f cb',
      'vpbroadcastd vvvv': '62 f2 75 48 58 cb', 'vpsrld bcast reg': '62 f1 75 58 72 d3 03'}
if not I386:
    UD['vpsrld R'] = '62 71 75 48 72 d3 03'
for k, b in UD.items():
    F.append((f'ud {k}', 'ud:' + ', '.join('0x' + x for x in b.split()), 0))
R = 'e' if I386 else 'r'
o = []; w = o.append
w(f'''// Generated by tools/gen-avx512-misc-test.py{" --i386" if I386 else ""} -- do not edit.
//
// AVX-512 shifts, rotates, the qword ops, VPTERNLOG, VPBLENDM, the
// broadcasts and the non-temporal moves, against Intel SDE (see the
// generator). (No target pragma: the harness itself must not use AVX-512.)
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static uint64_t rs;
static uint64_t rnd(void) {{ rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }}
// d: the destination's old value, a s1, b s2, m the memory operand, g a GPR
struct st {{ uint8_t d[64], a[64], b[64], out[64], m[64]; uint64_t k, g; }} __attribute__((aligned(64)));
static sigjmp_buf jb;
static volatile int sig;
static void handler(int s) {{ sig = s; siglongjmp(jb, 1); }}
#define LOAD "vmovdqu64 (%0), %%zmm1\\n vmovdqu64 64(%0), %%zmm2\\n vmovdqu64 128(%0), %%zmm3\\n kmovq 320(%0), %%k1\\n" \\
    "mov 328(%0), %%{R}cx\\n"
''')
for i, (name, asm, cnt) in enumerate(F):
    if asm.startswith('ud:'):
        w(f'''__attribute__((noinline)) static void f{i}(struct st *t) {{
    __asm__ volatile(LOAD "mov %1, %%{R}ax\\n .byte {asm[3:]}\\n vmovdqu64 %%zmm1, 192(%0)" :: "r"(t), "r"(t->m) : "memory", "{R}ax", "{R}cx", "xmm1", "xmm2", "xmm3");
}}''')
    else:
        w(f'''__attribute__((noinline)) static void f{i}(struct st *t) {{
    __asm__ volatile(LOAD "{asm}\\n vmovdqu64 %%zmm1, 192(%0)" :: "r"(t), "r"(t->m) : "memory", "{R}cx", "xmm1", "xmm2", "xmm3");
}}''')
w('static const struct { const char *name; void (*fn)(struct st *); int cnt; } forms[] = {')
for i, (name, asm, cnt) in enumerate(F):
    c = -1 if cnt == 'x' else cnt
    w(f'    {{"{name}", f{i}, {c}}},')
w('};')
w(f'#define NF {len(F)}')
if answers:
    w('static const uint64_t want[NF] = {')
    for name, asm, cnt in F:
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
// counts small enough to land inside the width, most of the time
static void small_counts(uint8_t *p, int e) {
    for (int i = 0; i < 64; i += e) {
        uint64_t r = rnd(), v = r % (8 * e + 8);
        if ((r >> 20) % 8 == 0) v = r >> 30;     /* now and then a huge one */
        memcpy(p + i, &v, e);
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
        int ud = !strncmp(forms[fi].name, "ud ", 3);
        for (int k = 0; k < (ud ? 1 : 16); k++) {
            for (int i = 0; i < 64; i += 8) {
                uint64_t r;
                r = rnd(); memcpy(t.d + i, &r, 8);
                r = rnd(); memcpy(t.a + i, &r, 8);
                r = rnd(); memcpy(t.b + i, &r, 8);
                r = rnd(); memcpy(t.m + i, &r, 8);
            }
            if (forms[fi].cnt > 0) {
                small_counts(t.b, forms[fi].cnt);
                small_counts(t.m, forms[fi].cnt);
            } else if (forms[fi].cnt < 0) {
                uint64_t c1 = rnd() % 70, c2 = rnd() % 70;
                if (k == 5) c1 = c2 = 1ull << 40;
                memcpy(t.b, &c1, 8);
                memcpy(t.m, &c2, 8);
            }
            t.k = rnd();
            t.g = rnd();
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
