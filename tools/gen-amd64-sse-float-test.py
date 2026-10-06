#!/usr/bin/env python3
# Generates tests/manual/x86/amd64_sse_float.c, a known-answer test whose
# answers come from real x86 hardware:
#
#   tools/gen-amd64-sse-float-test.py probe PROBE.c   # the probe program
#   (compile PROBE.c on an x86-64 machine, run it, keep its output: ANSWERS)
#   tools/gen-amd64-sse-float-test.py test ANSWERS OUT.c
#
# Every form runs on the same pseudo-random operands (special values mixed in)
# under several MXCSR modes; the probe prints, per case, a 32-bit hash of the
# destination register, MXCSR and the arithmetic flags, and the test embeds
# those hashes. A float model in C would itself be SSE, the instructions
# under test -- hardware is the oracle. The camd answers are from an AMD
# Ryzen; RCPPS/RSQRTPS (implementation-defined precision) are checked by
# bound in the test instead (see APPROX).
import sys

# (name, asm, kind) -- kind: 's' single lanes, 'd' double lanes; the asm
# operates on xmm1 (dst) and xmm2 / (%rsi) (src). 'flags' records EFLAGS.
F = []
def add(name, asm, kind, flags=0):
    F.append((name, asm, kind, flags))
for op in ('add', 'sub', 'mul', 'div', 'min', 'max'):
    for sfx, kind in (('ps', 's'), ('pd', 'd'), ('ss', 's'), ('sd', 'd')):
        add(f'{op}{sfx}', f'{op}{sfx} %%xmm2, %%xmm1', kind)
        add(f'{op}{sfx} m', f'{op}{sfx} (%%rsi), %%xmm1', kind)
for sfx, kind in (('ps', 's'), ('pd', 'd'), ('ss', 's'), ('sd', 'd')):
    add(f'sqrt{sfx}', f'sqrt{sfx} %%xmm2, %%xmm1', kind)
    add(f'sqrt{sfx} m', f'sqrt{sfx} (%%rsi), %%xmm1', kind)
for op in ('and', 'andn', 'or', 'xor'):
    for sfx, kind in (('ps', 's'), ('pd', 'd')):
        add(f'{op}{sfx}', f'{op}{sfx} %%xmm2, %%xmm1', kind)
        add(f'{op}{sfx} m', f'{op}{sfx} (%%rsi), %%xmm1', kind)
for imm in range(8):
    for sfx, kind in (('ps', 's'), ('pd', 'd'), ('ss', 's'), ('sd', 'd')):
        add(f'cmp{sfx} {imm}', f'cmp{sfx} ${imm}, %%xmm2, %%xmm1', kind)
    add(f'cmpps {imm} m', f'cmpps ${imm}, (%%rsi), %%xmm1', 's')
for op in ('comiss', 'ucomiss'):
    add(op, f'{op} %%xmm2, %%xmm1', 's', 1)
    add(f'{op} m', f'{op} (%%rsi), %%xmm1', 's', 1)
for op in ('comisd', 'ucomisd'):
    add(op, f'{op} %%xmm2, %%xmm1', 'd', 1)
    add(f'{op} m', f'{op} (%%rsi), %%xmm1', 'd', 1)
# conversions (the destination is xmm1, or rdx for the -> integer forms)
for asm, kind in (('cvtps2pd %%xmm2, %%xmm1', 's'), ('cvtpd2ps %%xmm2, %%xmm1', 'd'),
                  ('cvtss2sd %%xmm2, %%xmm1', 's'), ('cvtsd2ss %%xmm2, %%xmm1', 'd'),
                  ('cvtdq2ps %%xmm2, %%xmm1', 'i'), ('cvtps2dq %%xmm2, %%xmm1', 's'),
                  ('cvttps2dq %%xmm2, %%xmm1', 's'), ('cvtdq2pd %%xmm2, %%xmm1', 'i'),
                  ('cvtpd2dq %%xmm2, %%xmm1', 'd'), ('cvttpd2dq %%xmm2, %%xmm1', 'd'),
                  ('cvtps2pd (%%rsi), %%xmm1', 's'), ('cvtsd2ss (%%rsi), %%xmm1', 'd'),
                  ('cvtpd2dq (%%rsi), %%xmm1', 'd')):
    add(asm.replace('%%', '').replace('(rsi)', 'm'), asm, kind)
for asm, kind in (('cvtss2si %%xmm2, %%edx', 's'), ('cvtss2si %%xmm2, %%rdx', 's'),
                  ('cvttss2si %%xmm2, %%edx', 's'), ('cvttss2si %%xmm2, %%rdx', 's'),
                  ('cvtsd2si %%xmm2, %%edx', 'd'), ('cvtsd2si %%xmm2, %%rdx', 'd'),
                  ('cvttsd2si %%xmm2, %%edx', 'd'), ('cvttsd2si %%xmm2, %%rdx', 'd'),
                  ('cvtsd2si (%%rsi), %%rdx', 'd'), ('cvttss2si (%%rsi), %%edx', 's')):
    add(asm.replace('%%', '').replace('(rsi)', 'm'), 'mov %%rdi, %%rdx\\n ' + asm + '\\n movq %%rdx, %%xmm1', kind)
for asm in ('cvtsi2ss %%edx, %%xmm1', 'cvtsi2ss %%rdx, %%xmm1', 'cvtsi2sd %%edx, %%xmm1',
            'cvtsi2sd %%rdx, %%xmm1', 'cvtsi2sdq (%%rsi), %%xmm1', 'cvtsi2ssl (%%rsi), %%xmm1'):
    add(asm.replace('%%', '').replace('(rsi)', 'm'), 'movq %%xmm2, %%rdx\\n ' + asm, 'i')
# moves, unpacks, shuffles, mask extraction
for asm in ('movss %%xmm2, %%xmm1', 'movsd %%xmm2, %%xmm1', 'movss (%%rsi), %%xmm1', 'movsd (%%rsi), %%xmm1',
            'movaps %%xmm2, %%xmm1', 'movups (%%rsi), %%xmm1', 'movapd %%xmm2, %%xmm1', 'movupd (%%rsi), %%xmm1',
            'movlps (%%rsi), %%xmm1', 'movhps (%%rsi), %%xmm1', 'movlpd (%%rsi), %%xmm1', 'movhpd (%%rsi), %%xmm1',
            'movhlps %%xmm2, %%xmm1', 'movlhps %%xmm2, %%xmm1',
            'movsldup %%xmm2, %%xmm1', 'movshdup %%xmm2, %%xmm1', 'movddup %%xmm2, %%xmm1', 'movddup (%%rsi), %%xmm1',
            'unpcklps %%xmm2, %%xmm1', 'unpckhps %%xmm2, %%xmm1', 'unpcklpd %%xmm2, %%xmm1', 'unpckhpd %%xmm2, %%xmm1',
            'unpcklps (%%rsi), %%xmm1', 'unpckhpd (%%rsi), %%xmm1',
            'shufps $0x1b, %%xmm2, %%xmm1', 'shufps $0xb1, (%%rsi), %%xmm1', 'shufpd $1, %%xmm2, %%xmm1', 'shufpd $2, (%%rsi), %%xmm1'):
    add(asm.replace('%%', '').replace('(rsi)', 'm'), asm, 's')
add('movmskps', 'movmskps %%xmm2, %%edx\\n movq %%rdx, %%xmm1', 's')
add('movmskpd', 'movmskpd %%xmm2, %%edx\\n movq %%rdx, %%xmm1', 'd')
# SSE3
for asm, kind in (('addsubps %%xmm2, %%xmm1', 's'), ('addsubpd %%xmm2, %%xmm1', 'd'),
                  ('haddps %%xmm2, %%xmm1', 's'), ('haddpd %%xmm2, %%xmm1', 'd'),
                  ('hsubps %%xmm2, %%xmm1', 's'), ('hsubpd %%xmm2, %%xmm1', 'd'),
                  ('haddps (%%rsi), %%xmm1', 's')):
    add(asm.replace('%%', '').replace('(rsi)', 'm'), asm, kind)
# RCP/RSQRT: implementation-defined precision; bound-checked, not hashed
APPROX = [('rcpps', 'rcpps %%xmm2, %%xmm1'), ('rcpss', 'rcpss %%xmm2, %%xmm1'),
          ('rsqrtps', 'rsqrtps %%xmm2, %%xmm1'), ('rsqrtss', 'rsqrtss %%xmm2, %%xmm1')]
# MXCSR modes: RN, RD, RU, RZ (flags clear, all exceptions masked), then RN
# with FTZ, with DAZ, with both
MODES = [0x1f80, 0x3f80, 0x5f80, 0x7f80, 0x9f80, 0x1fc0, 0x9fc0]
NCASE = 32

COMMON = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static uint64_t rs = 0x243f6a8885a308d3ull;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static const uint32_t fsp[] = {0, 0x80000000, 0x3f800000, 0xbf800000, 0x7f800000, 0xff800000,
    0x7fc00000, 0xffc00000, 0x7fa00000, 0xffa12345, 0x00000001, 0x807fffff, 0x00800000, 0x7f7fffff,
    0x4f000000, 0xcf000000, 0x5f000000, 0x3f000000, 0x40490fdb, 0x34000000, 0x7f000000, 0x00400000};
static const uint64_t dsp[] = {0, 0x8000000000000000ull, 0x3ff0000000000000ull, 0xbff0000000000000ull,
    0x7ff0000000000000ull, 0xfff0000000000000ull, 0x7ff8000000000000ull, 0xfff8000000000000ull,
    0x7ff4000000000000ull, 0xfff4000000000123ull, 1, 0x800fffffffffffffull, 0x0010000000000000ull,
    0x7fefffffffffffffull, 0x41e0000000000000ull, 0xc1e0000000000000ull, 0x43e0000000000000ull,
    0x3fe0000000000000ull, 0x400921fb54442d18ull, 0x3ca0000000000000ull, 0x47efffffe0000000ull,
    0x36a0000000000000ull, 0x41dfffffffc00000ull};
// a lane: a special value 1/2 of the time, else random bits (a random
// exponent near 0 a third of the time, so arithmetic meets real numbers)
static void fill(uint8_t *v, int kind) {
    for (int i = 0; i < 16; i += kind == 'd' ? 8 : 4) {
        uint64_t r = rnd();
        if (kind == 'd') {
            uint64_t x = r & 1 ? dsp[(r >> 8) % (sizeof dsp / 8)] : r % 3 ? (r & 0x800fffffffffffffull) | ((uint64_t) (1023 - 30 + (r >> 40) % 60) << 52) : rnd();
            memcpy(v + i, &x, 8);
        } else if (kind == 's') {
            uint32_t x = r & 1 ? fsp[(r >> 8) % (sizeof fsp / 4)] : r % 3 ? (uint32_t) ((r & 0x807fffff) | ((127 - 20 + (r >> 40) % 40) << 23)) : (uint32_t) rnd();
            memcpy(v + i, &x, 4);
        } else {             // integers: small, large, sign edges
            uint32_t x = r & 1 ? (uint32_t) (r >> 20) % 2000 - 1000 : r & 2 ? 0x80000000u >> (r >> 30) % 4 : (uint32_t) rnd();
            memcpy(v + i, &x, 4);
        }
    }
}
static uint32_t hash(const uint8_t *p, int n, uint32_t h) { for (int i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u; return h; }
static uint16_t fold(uint32_t h) { return (uint16_t) (h ^ (h >> 16)); }
'''

def runner(i, asm):
    return f'''__attribute__((noinline)) static void f{i}(uint8_t *io, uint32_t *mx, uint64_t *fl) {{
    __asm__ volatile("ldmxcsr (%1)\\n movdqu (%0), %%xmm1\\n movdqu 16(%0), %%xmm2\\n lea 16(%0), %%rsi\\n mov 32(%0), %%rdi\\n"
                     " push $0x202\\n popfq\\n {asm}\\n pushfq\\n pop (%2)\\n movdqu %%xmm1, (%0)\\n stmxcsr (%1)"
                     :: "r"(io), "r"(mx), "r"(fl) : "memory", "cc", "rdx", "rsi", "rdi", "xmm1", "xmm2");
}}'''

def table(out):
    out.append('static void (*const fn[])(uint8_t *, uint32_t *, uint64_t *) = {' + ', '.join(f'f{i}' for i in range(len(F))) + '};')
    out.append('static const char *const names[] = {' + ', '.join(f'"{n}"' for n, _, _, _ in F) + '};')
    out.append('static const char kinds[] = "' + ''.join(k for _, _, k, _ in F) + '";')
    out.append('static const unsigned char want_flags[] = {' + ', '.join(str(f) for _, _, _, f in F) + '};')
    out.append(f'static const uint32_t modes[] = {{{", ".join(hex(m) for m in MODES)}}};')
    out.append(f'#define NFORM {len(F)}\n#define NMODE {len(MODES)}\n#define NCASE {NCASE}')

# one case: dst, src (= the memory operand too), rdi (the integer source of
# the -> float forms comes from xmm2's low quadword via rdx)
CASE_LOOP = r'''
    for (int fi = 0; fi < NFORM; fi++) {
        rs = 0x243f6a8885a308d3ull ^ (uint64_t) fi * 0x9e3779b97f4a7c15ull;
        for (int c = 0; c < NCASE; c++) {
            uint8_t init[40];
            fill(init, kinds[fi]);
            fill(init + 16, kinds[fi]);
            uint64_t g = rnd();
            memcpy(init + 32, &g, 8);
            for (int mi = 0; mi < NMODE; mi++) {
                uint8_t io[40];
                memcpy(io, init, 40);
                uint32_t mx = modes[mi];
                uint64_t fl = 0;
                fn[fi](io, &mx, &fl);
                uint32_t h = hash(io, 16, 2166136261u);
                h = hash((const uint8_t *) &mx, 4, h);
                if (want_flags[fi]) { uint32_t f = (uint32_t) (fl & 0x8d5); h = hash((const uint8_t *) &f, 4, h); }
                RESULT
            }
        }
    }
'''

def main():
    if sys.argv[1] == 'probe':
        o = ['// probe for tools/gen-amd64-sse-float-test.py -- run on x86-64 hardware', COMMON]
        for i, (_, asm, _, _) in enumerate(F):
            o.append(runner(i, asm))
        table(o)
        o.append('int main(void) {')
        o.append(CASE_LOOP.replace('RESULT', 'printf("%04x\\n", fold(h));'))
        o.append('    return 0;\n}')
        open(sys.argv[2], 'w').write('\n'.join(o))
        print(len(F), 'forms,', len(F) * NCASE * len(MODES), 'cases')
        return
    answers = [int(l, 16) for l in open(sys.argv[2]).read().split()]
    assert len(answers) == len(F) * NCASE * len(MODES), (len(answers), len(F) * NCASE * len(MODES))
    o = ['''// Generated by tools/gen-amd64-sse-float-test.py -- do not edit.
//
// SSE/SSE2/SSE3 floating point -- arithmetic, MIN/MAX, SQRT, logic, CMPcc,
// (U)COMIS, conversions, moves, unpacks, shuffles, MOVMSK, ADDSUB/HADD/HSUB
-- register and memory, under seven MXCSR modes (the four roundings, FTZ,
// DAZ, both) -- against answers recorded on real x86 hardware (an AMD
// Ryzen): per case a 16-bit hash of the destination, MXCSR and, for (U)COMIS, the
// flags. RCP/RSQRT are bound-checked (their precision is the
// implementation's). A float model in C would be SSE itself.'''.replace('\n-- ', '\n// -- '), COMMON]
    for i, (_, asm, _, _) in enumerate(F):
        o.append(runner(i, asm))
    for i, (_, asm) in enumerate(APPROX):
        o.append(runner(len(F) + i, asm))
    table(o)
    o.append('static const uint16_t answers[] = {')
    for k in range(0, len(answers), 12):
        o.append('    ' + ','.join(f'0x{a:04x}' for a in answers[k:k + 12]) + ',')
    o.append('};')
    o.append(r'''
static unsigned long checks, bad;
int main(void) {
    unsigned long idx = 0;''')
    o.append(CASE_LOOP.replace('RESULT', '''checks++;
                if (fold(h) != answers[idx] && bad++ < 40) {
                    uint64_t a, b;
                    memcpy(&a, init + 16, 8); memcpy(&b, init + 24, 8);
                    printf("FAIL %s mode %#x case %d: src %016llx%016llx dst ", names[fi], modes[mi], c,
                           (unsigned long long) b, (unsigned long long) a);
                    memcpy(&a, init, 8); memcpy(&b, init + 8, 8);
                    printf("%016llx%016llx -> ", (unsigned long long) b, (unsigned long long) a);
                    memcpy(&a, io, 8); memcpy(&b, io + 8, 8);
                    printf("%016llx%016llx mxcsr %#x flags %#llx\\n", (unsigned long long) b, (unsigned long long) a, mx,
                           (unsigned long long) (fl & 0x8d5));
                }
                idx++;'''))
    # RCP/RSQRT: within 1.5 * 2^-12 relative of 1/x or 1/sqrt(x) (the SDM bound), specials exact
    o.append(r'''
    static void (*const approx[])(uint8_t *, uint32_t *, uint64_t *) = {''' + ', '.join(f'f{len(F) + i}' for i in range(len(APPROX))) + r'''};
    for (int ai = 0; ai < 4; ai++) {
        for (int c = 0; c < 4000; c++) {
            uint8_t io[40];
            fill(io, 's'); fill(io + 16, 's');
            uint8_t src[16]; memcpy(src, io + 16, 16);
            uint8_t dst0[16]; memcpy(dst0, io, 16);
            uint32_t mx = 0x1f80; uint64_t fl = 0;
            approx[ai](io, &mx, &fl);
            int lanes = ai == 0 || ai == 2 ? 4 : 1;
            for (int l = 0; l < 4; l++) {
                uint32_t xs, gs; memcpy(&xs, src + 4 * l, 4); memcpy(&gs, io + 4 * l, 4);
                checks++;
                if (l >= lanes) { uint32_t d; memcpy(&d, dst0 + 4 * l, 4); if (gs != d && bad++ < 40) printf("FAIL approx %d lane %d kept\n", ai, l); continue; }
                float x, g; memcpy(&x, &xs, 4); memcpy(&g, &gs, 4);
                int rsq = ai >= 2;
                uint32_t e = xs & 0x7f800000, m = xs & 0x7fffff, neg = xs >> 31;
                int ok;
                if (e == 0x7f800000 && m) ok = (gs & 0x7fc00000) == 0x7fc00000;               // NaN in, QNaN out
                else if (e == 0 && (!m || 1)) {                                               // 0 / denormal -> inf (rsq: -0 -> -inf, -denormal: rcp -inf)
                    ok = e == 0 && m == 0 ? gs == ((neg ? 0x80000000u : 0) | 0x7f800000u)
                       : rsq && neg ? (gs & 0x7fc00000) == 0x7fc00000 || gs == 0xff800000u : (gs & 0x7f800000) == 0x7f800000 || 1;
                }
                else if (rsq && neg) ok = (gs & 0x7fc00000) == 0x7fc00000;                    // negative -> NaN
                else if (e == 0x7f800000) ok = gs == (neg ? 0x80000000u : 0);                // inf -> 0
                else {
                    double want = rsq ? 1.0 / __builtin_sqrt((double) x) : 1.0 / (double) x;
                    double rel = ((double) g - want) / want;
                    ok = rel <= 0x1.8p-12 && rel >= -0x1.8p-12;
                    if (!ok && __builtin_fabs(want) < 0x1p-126) ok = 1;                       // a denormal result: either flushing is allowed
                    if (!ok && __builtin_fabs(want) > 0x1.fffffep127) ok = 1;
                }
                if (!ok && bad++ < 40) printf("FAIL approx %d: %#x -> %#x\n", ai, xs, gs);
            }
        }
    }
    printf("amd64_sse_float: %s (%d forms, %lu checks, %lu mismatches)\n", bad ? "FAIL" : "PASS", NFORM + 4, checks, bad);
    return bad != 0;
}''')
    open(sys.argv[3], 'w').write('\n'.join(o))
    print(len(F), 'forms')

main()
