#!/usr/bin/env python3
# Generates tests/manual/x86/amd64_avx512_float.c, or with --i386
# i386_avx512_float.c (usage: this script [--i386] [--answers FILE] OUT.c).
#
# AVX-512 (EVEX) floating-point arithmetic: VADD/VSUB/VMUL/VDIV/VMIN/VMAX/
# VSQRT PS/PD at VL 128/256/512 (register, memory and {1toN} sources) and
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
# -- ./test hashes` on camd, passed back with --answers). Forms are keyed
# and seeded by name, so the i386 test (zmm0-7, no 16-31 forms) takes the
# amd64 answers. "dump" prints every case.
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
F = []   # (name, asm, flags: 1 double, 2 a compare, 4 the zmm16-31 registers)
def add(name, asm, t, cmp=False, hi=False):
    F.append((name, asm, DBL[t] | (2 if cmp else 0) | (4 if hi else 0)))
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
}
if not I386:
    UD.update({"vsqrtps V'": '62 f1 7c 40 51 cb', "vcmpps R'": '62 e1 6c 48 c2 cb 01',
               'vcmpps R': '62 71 6c 48 c2 cb 01', "vaddps V' mem": '62 f1 6c 40 58 08'})
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
        w(f'''__attribute__((noinline)) static void f{i}(struct st *t) {{
    __asm__ volatile(LOAD "{asm}" SAVE("{out}") :: "r"(t), "r"(t->m) : "memory", "xmm1", "xmm2", "xmm3");
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
static void quiet_lanes(struct st *t, int dbl, int mode) {
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
            if (j == 1) quiet_lanes(&t, dbl, mode);
            if (j == 2) t.k = mode & 1 ? ~0ull : 0;
            t.mx = ud ? 0x1f80 : modes[mode];
            t.mxd = 0x1f80;
            t.mxo = 0xdead;
            t.kout = 0;
            memset(t.out, 0, 64);
            int sg = run(forms[fi].fn, &t);
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
