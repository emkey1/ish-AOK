#!/usr/bin/env python3
# Generates tests/manual/x86/x87_kat.c, a known-answer test of the x87 whose
# answers come from real x86 hardware:
#
#   tools/gen-x87-test.py probe PROBE.c        # the probe program
#   (compile PROBE.c on an x86 machine, -m32 or not, run it: ANSWERS)
#   tools/gen-x87-test.py test ANSWERS OUT.c
#
# Every D8-DF form but the transcendentals and the environment loads and
# stores (x87_env.c has those) runs on the same pseudo-random operands --
# special values, denormals, pseudo-denormals, unnormals, pseudo-NaNs and
# integers mixed in -- under the four rounding modes times the four precision
# control settings, from a stack holding two values, eight (so a push
# overflows), or one with ST(1) or ST(0) empty (so an operand underflows).
# The probe prints, per case, a 16-bit hash of FXSAVE's control word, status
# word, abridged tag and the eight registers, plus the memory operand, the
# arithmetic flags and AX; the test embeds those. The encodings are bytes, so
# the -m32 and 64-bit builds run the same instructions; the answers are the
# same for both, and are from an AMD Ryzen (camd).
#
# Run the test with -v N to print every case of form N in full (do it on the
# hardware too and diff), -l to list the forms.
import sys

# (name, bytes, kind) -- kind: '' for no memory operand, or the memory
# operand's type: f m32, d m64, w m16 int, i m32 int, q m64 int, t m80, b BCD,
# c a control word. ST(0) = A, ST(1) = B; the memory operand is at (%esi).
F = []
def add(name, code, kind=''):
    F.append((name, code, kind))

def reg(op, r, i):
    return [op, 0xc0 | (r << 3) | i]
def mem(op, r):
    return [op, (r << 3) | 6]

ARITH = ['fadd', 'fmul', 'fcom', 'fcomp', 'fsub', 'fsubr', 'fdiv', 'fdivr']
for r, n in enumerate(ARITH):
    for i in (1, 0, 2):
        add(f'{n} st,st{i}', reg(0xd8, r, i))
for i in (1, 0, 7):
    add(f'fld st{i}', reg(0xd9, 0, i))
for i in (1, 0, 2):
    add(f'fxch st{i}', reg(0xd9, 1, i))
add('fnop', [0xd9, 0xd0])
add('fstp1 st1', reg(0xd9, 3, 1))
for b, n in ((0xe0, 'fchs'), (0xe1, 'fabs'), (0xe4, 'ftst'), (0xe5, 'fxam'),
             (0xe8, 'fld1'), (0xe9, 'fldl2t'), (0xea, 'fldl2e'), (0xeb, 'fldpi'),
             (0xec, 'fldlg2'), (0xed, 'fldln2'), (0xee, 'fldz'),
             (0xf4, 'fxtract'), (0xf5, 'fprem1'), (0xf6, 'fdecstp'), (0xf7, 'fincstp'),
             (0xf8, 'fprem'), (0xfa, 'fsqrt'), (0xfc, 'frndint'), (0xfd, 'fscale')):
    add(n, [0xd9, b])
for r, n in enumerate(['fcmovb', 'fcmove', 'fcmovbe', 'fcmovu']):
    add(f'{n} st1', reg(0xda, r, 1))
add('fucompp', [0xda, 0xe9])
for r, n in enumerate(['fcmovnb', 'fcmovne', 'fcmovnbe', 'fcmovnu']):
    add(f'{n} st1', reg(0xdb, r, 1))
for b, n in ((0xe0, 'feni'), (0xe1, 'fdisi'), (0xe2, 'fnclex'), (0xe3, 'fninit'), (0xe4, 'fsetpm')):
    add(n, [0xdb, b])
for i in (1, 0, 2):
    add(f'fucomi st{i}', reg(0xdb, 5, i))
    add(f'fcomi st{i}', reg(0xdb, 6, i))
for r, n in enumerate(['fadd', 'fmul', 'fcom2', 'fcomp3', 'fsubr', 'fsub', 'fdivr', 'fdiv']):
    for i in (1, 0, 2):
        add(f'{n} st{i},st', reg(0xdc, r, i))
for i in (1, 0):
    add(f'ffree st{i}', reg(0xdd, 0, i))
add('fxch4 st1', reg(0xdd, 1, 1))
for i in (1, 0, 2):
    add(f'fst st{i}', reg(0xdd, 2, i))
    add(f'fstp st{i}', reg(0xdd, 3, i))
    add(f'fucom st{i}', reg(0xdd, 4, i))
    add(f'fucomp st{i}', reg(0xdd, 5, i))
for r, n in enumerate(['faddp', 'fmulp', 'fcomp5', None, 'fsubrp', 'fsubp', 'fdivrp', 'fdivp']):
    if n:
        for i in (1, 2):
            add(f'{n} st{i},st', reg(0xde, r, i))
add('fcompp', [0xde, 0xd9])
add('ffreep st1', reg(0xdf, 0, 1))
add('fxch7 st1', reg(0xdf, 1, 1))
add('fstp8 st1', reg(0xdf, 2, 1))
add('fstp9 st1', reg(0xdf, 3, 1))
add('fnstsw ax', [0xdf, 0xe0])
for i in (1, 0, 2):
    add(f'fucomip st{i}', reg(0xdf, 5, i))
    add(f'fcomip st{i}', reg(0xdf, 6, i))
for op, kind, sz in ((0xd8, 'f', 'm32'), (0xdc, 'd', 'm64'), (0xda, 'i', 'm32int'), (0xde, 'w', 'm16int')):
    for r, n in enumerate(ARITH):
        add(f'f{"i" if kind in "iw" else ""}{n[1:]} {sz}', mem(op, r), kind)
for name, code, kind in (('fld m32', mem(0xd9, 0), 'f'), ('fst m32', mem(0xd9, 2), 'f'), ('fstp m32', mem(0xd9, 3), 'f'),
                         ('fldcw', mem(0xd9, 5), 'c'), ('fnstcw', mem(0xd9, 7), 'c'),
                         ('fild m32', mem(0xdb, 0), 'i'), ('fisttp m32', mem(0xdb, 1), 'i'), ('fist m32', mem(0xdb, 2), 'i'),
                         ('fistp m32', mem(0xdb, 3), 'i'), ('fld m80', mem(0xdb, 5), 't'), ('fstp m80', mem(0xdb, 7), 't'),
                         ('fld m64', mem(0xdd, 0), 'd'), ('fisttp m64', mem(0xdd, 1), 'q'), ('fst m64', mem(0xdd, 2), 'd'),
                         ('fstp m64', mem(0xdd, 3), 'd'), ('fnstsw m16', mem(0xdd, 7), 'w'),
                         ('fild m16', mem(0xdf, 0), 'w'), ('fisttp m16', mem(0xdf, 1), 'w'), ('fist m16', mem(0xdf, 2), 'w'),
                         ('fistp m16', mem(0xdf, 3), 'w'), ('fbld', mem(0xdf, 4), 'b'), ('fild m64', mem(0xdf, 5), 'q'),
                         ('fbstp', mem(0xdf, 6), 'b'), ('fistp m64', mem(0xdf, 7), 'q')):
    add(name, code, kind)

NCASE = 40
# control words: all exceptions masked, rounding x precision control
MODES = [0x7f | (pc << 8) | (rc << 10) for rc in range(4) for pc in (3, 2, 0, 1)]
# x87_kat_unmasked: exceptions unmasked -- all of them (three roundings and
# precisions), then each alone. Nothing after the operation is a waiting
# instruction, so the #MF the operation leaves pending never fires and what
# an unmasked exception does to the destination is what gets recorded.
UNMASKED = len(sys.argv) > 1 and sys.argv[1].endswith('-unmasked')
if UNMASKED:
    NCASE = 16
    MODES = [0x0340, 0x0b40, 0x0040, 0x037e, 0x037d, 0x037b, 0x0377, 0x036f, 0x035f]

COMMON = r'''#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint64_t rs;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
struct f80 { uint64_t m; uint16_t se; };
// an 80-bit operand: special classes, then integers, then reals
static struct f80 val(void) {
    uint64_t r = rnd(), m = rnd();
    uint16_t s = (uint16_t) ((r >> 63) << 15);
    switch (r % 24) {
    case 0: return (struct f80) {0, s};                                           // zero
    case 1: return (struct f80) {m >> (1 + (r >> 8) % 63), s};                    // denormal
    case 2: return (struct f80) {m | 1ull << 63, s};                              // pseudo-denormal
    case 3: return (struct f80) {1ull << 63, (uint16_t) (s | 0x7fff)};            // infinity
    case 4: return (struct f80) {m | 3ull << 62, (uint16_t) (s | 0x7fff)};        // QNaN
    case 5: return (struct f80) {((m >> 2) | 1) | 1ull << 63, (uint16_t) (s | 0x7fff)};  // SNaN
    case 6: return (struct f80) {3ull << 62, 0xffff};                             // the indefinite
    case 7: return (r >> 9) & 1 ? (struct f80) {m >> 1, (uint16_t) (s | 0x7fff)}    // pseudo-NaN/-infinity
                                : (struct f80) {m >> (1 + (r >> 10) % 8), (uint16_t) (s | (0x3fff + (r >> 16) % 64 - 32))};  // unnormal
    case 8: return (struct f80) {m | 1ull << 63, (uint16_t) (s | (1 + (r >> 8) % 70))};     // near the bottom
    case 9: return (struct f80) {m | 1ull << 63, (uint16_t) (s | (0x7ffe - (r >> 8) % 70))};  // near the top
    case 10: return (struct f80) {((r >> 9) & 1 ? ~0ull : 1ull << 63), (uint16_t) (s | ((r >> 10) & 1 ? 0x7ffe : 1))};
    case 11: case 12: case 13: {                                                  // integers, and halves
        int k = (int) ((r >> 8) % 66);
        uint64_t v = (m >> (63 - (k < 63 ? k : 63))) | 1ull << 63;
        if ((r >> 20) % 3 == 0) v &= ~0ull << (k < 63 ? 62 - k : 0);              // a half
        return (struct f80) {v, (uint16_t) (s | (0x3fff + k))};
    }
    case 14: return (struct f80) {1ull << 63, (uint16_t) (s | (0x3fff + (r >> 8) % 8))};    // 1, 2, 4 ...
    default: return (struct f80) {m | 1ull << 63, (uint16_t) (s | (0x3fff + (r >> 8) % 80 - 40))};
    }
}
static const uint64_t bcd_digits(uint64_t r) {
    uint64_t v = 0;
    for (int i = 0; i < 16; i++) v |= ((r >> (4 * i)) % 10) << (4 * i);
    return v;
}
// the memory operand, by kind
static void memval(uint8_t *mp, char kind) {
    uint64_t r = rnd(), m = rnd();
    for (int i = 0; i < 16; i++) mp[i] = (uint8_t) rnd();
    switch (kind) {
    case 'f': {
        static const uint32_t sp[] = {0, 0x80000000, 0x3f800000, 0x7f800000, 0xff800000, 0x7fc00000, 0xffc00000,
            0x7fa00000, 0xff812345, 0x00000001, 0x807fffff, 0x00800000, 0x7f7fffff, 0x4f000000, 0x5f000000, 0x3effffff};
        uint32_t x = r & 1 ? sp[(r >> 8) % 16] : r % 3 ? (uint32_t) ((m & 0x807fffff) | ((127 - 30 + (r >> 40) % 60) << 23)) : (uint32_t) m;
        memcpy(mp, &x, 4);
        break;
    }
    case 'd': {
        static const uint64_t sp[] = {0, 0x8000000000000000ull, 0x3ff0000000000000ull, 0x7ff0000000000000ull,
            0xfff0000000000000ull, 0x7ff8000000000000ull, 0xfff8000000000000ull, 0x7ff4000000000000ull,
            0xfff0000000000123ull, 1, 0x800fffffffffffffull, 0x0010000000000000ull, 0x7fefffffffffffffull,
            0x43e0000000000000ull, 0xc3e0000000000000ull, 0x3fdfffffffffffffull};
        uint64_t x = r & 1 ? sp[(r >> 8) % 16] : r % 3 ? (m & 0x800fffffffffffffull) | ((uint64_t) (1023 - 40 + (r >> 40) % 80) << 52) : m;
        memcpy(mp, &x, 8);
        break;
    }
    case 'w': { uint16_t x = r & 1 ? (uint16_t) ((r >> 8) % 2001 - 1000) : r & 2 ? (uint16_t) (0x8000 >> (r >> 30) % 3) : (uint16_t) m; memcpy(mp, &x, 2); break; }
    case 'i': { uint32_t x = r & 1 ? (uint32_t) ((r >> 8) % 2001 - 1000) : r & 2 ? 0x80000000u >> (r >> 30) % 3 : (uint32_t) m; memcpy(mp, &x, 4); break; }
    case 'q': { uint64_t x = r & 1 ? (uint64_t) ((r >> 8) % 2001 - 1000) : r & 2 ? 0x8000000000000000ull >> (r >> 30) % 3 : m; memcpy(mp, &x, 8); break; }
    case 't': { struct f80 v = val(); memcpy(mp, &v.m, 8); memcpy(mp + 8, &v.se, 2); break; }
    case 'b': {                                  // packed BCD: 18 digits and a sign byte, a few invalid
        uint64_t lo = bcd_digits(m);
        if (r % 7 == 0) lo |= 0xaull << (4 * ((r >> 8) % 16));
        memcpy(mp, &lo, 8);
        mp[8] = (uint8_t) ((r >> 16) % 10 | ((r >> 20) % 10) << 4);
        mp[9] = (r >> 24) & 1 ? 0x80 : (r >> 25) % 5 == 0 ? (uint8_t) (r >> 32) : 0;
        break;
    }
    case 'c': { uint16_t x = (uint16_t) ((m & 0x1f3f) | 0x40 | (r & 0xe080)); memcpy(mp, &x, 2); break; }
    }
}
struct io {
    uint8_t fx[512] __attribute__((aligned(16)));
    uint8_t st[8][16];         // B, A, then six fillers
    uint8_t mem[16];
    uint16_t cw;
    unsigned long efl_in, efl_out, ax, cfg;
};
static uint32_t hash(const uint8_t *p, int n, uint32_t h) { for (int i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u; return h; }
static uint16_t fold(uint32_t h) { return (uint16_t) (h ^ (h >> 16)); }
static uint32_t digest(const struct io *t) {
    uint32_t h = hash(t->fx, 5, 2166136261u);            // fcw, fsw, abridged tag
    for (int i = 0; i < 8; i++) h = hash(t->fx + 32 + 16 * i, 10, h);
    h = hash(t->mem, 16, h);
    uint32_t f = (uint32_t) (t->efl_out & 0x8d5), a = (uint32_t) (t->ax & 0xffff);
    h = hash((const uint8_t *) &f, 4, h);
    return hash((const uint8_t *) &a, 4, h);
}
static void show_in(const struct io *t) {
    printf(" in");
    for (int i = 0; i < 2; i++) { uint64_t m; uint16_t se; memcpy(&m, t->st[1 - i], 8); memcpy(&se, t->st[1 - i] + 8, 2); printf(" %04x:%016llx", se, (unsigned long long) m); }
    printf(" mem");
    for (int i = 15; i >= 0; i--) printf("%02x", t->mem[i]);
    printf(" fl %03lx ->", t->efl_in & 0x8d5);
}
static void show(const struct io *t) {
    uint16_t fcw, fsw; memcpy(&fcw, t->fx, 2); memcpy(&fsw, t->fx + 2, 2);
    printf(" cw %04x sw %04x tag %02x fl %03lx ax %04lx st", fcw, fsw, t->fx[4], t->efl_out & 0x8d5, t->ax & 0xffff);
    for (int i = 0; i < 8; i++) { uint64_t m; uint16_t se; memcpy(&m, t->fx + 32 + 16 * i, 8); memcpy(&se, t->fx + 40 + 16 * i, 2); printf(" %04x:%016llx", se, (unsigned long long) m); }
    printf(" mem");
    for (int i = 15; i >= 0; i--) printf("%02x", t->mem[i]);
    printf("\n");
}
'''

def runner(i, code):
    b = ', '.join(f'0x{x:02x}' for x in code)
    return f'''__attribute__((noinline)) static void f{i}(struct io *t) {{
    __asm__ volatile("fninit\\n fldcw %[cw]\\n"
        " testl $1, %[cfg]\\n jz 1f\\n fldt 112(%[st])\\n fldt 96(%[st])\\n fldt 80(%[st])\\n fldt 64(%[st])\\n fldt 48(%[st])\\n fldt 32(%[st])\\n"
        "1: fldt (%[st])\\n fldt 16(%[st])\\n"
        " testl $2, %[cfg]\\n jz 2f\\n ffree %%st(1)\\n"
        "2: testl $4, %[cfg]\\n jz 3f\\n ffree %%st(0)\\n"
        "3: push %[fl]\\n popf\\n mov $0x5a5a5a5a, %%eax\\n"
        " .byte {b}\\n"
        " pushf\\n pop %[flo]\\n mov %%eax, %[ax]\\n fxsave %[fx]\\n fninit"
        : [fx] "=m"(t->fx), [flo] "=&r"(t->efl_out), [ax] "=m"(t->ax), "+m"(t->mem)
        : [cw] "m"(t->cw), [cfg] "m"(t->cfg), [st] "r"(t->st), [fl] "r"(t->efl_in), "S"(t->mem)
        : "memory", "cc", "eax");
}}'''

def table(out):
    out.append('static void (*const fn[])(struct io *) = {' + ', '.join(f'f{i}' for i in range(len(F))) + '};')
    out.append('static const char *const names[] = {' + ', '.join(f'"{n}"' for n, _, _ in F) + '};')
    out.append('static const char kinds[] = "' + ''.join(k or '-' for _, _, k in F) + '";')
    out.append(f'static const uint16_t modes[] = {{{", ".join(hex(m) for m in MODES)}}};')
    out.append(f'#define NFORM {len(F)}\n#define NMODE {len(MODES)}\n#define NCASE {NCASE}')

# One case: the stack's eight values, its configuration, the memory operand
# and the flags going in, run under every mode.
CASE_LOOP = r'''
    for (int fi = 0; fi < NFORM; fi++) {
        rs = 0x243f6a8885a308d3ull ^ (uint64_t) (fi + 1) * 0x9e3779b97f4a7c15ull;
        for (int c = 0; c < NCASE; c++) {
            static struct io init, t;
            memset(&init, 0, sizeof init);
            for (int i = 0; i < 8; i++) { struct f80 v = val(); memcpy(init.st[i], &v.m, 8); memcpy(init.st[i] + 8, &v.se, 2); }
            memval(init.mem, kinds[fi]);
            uint64_t r = rnd();
            init.cfg = c % 16 == 15 ? 1 : c % 16 == 14 ? 2 : c % 16 == 13 ? 4 : c % 16 == 12 ? 6 : 0;
            init.efl_in = 0x202 | (r & 0x8d5);
            for (int mi = 0; mi < NMODE; mi++) {
                t = init;
                t.cw = modes[mi];
                if (kinds[fi] == 'c') { uint16_t x; memcpy(&x, t.mem, 2); x = (uint16_t) ((x & ~0x3f) | (modes[mi] & 0x3f)); memcpy(t.mem, &x, 2); }   // the mode's masks
                fn[fi](&t);
                uint32_t h = digest(&t);
                RESULT
            }
        }
    }
'''

def main():
    if sys.argv[1] in ('probe', 'probe-unmasked'):
        o = ['// probe for tools/gen-x87-test.py -- run on x86 hardware', COMMON]
        for i, (_, code, _) in enumerate(F):
            o.append(runner(i, code))
        table(o)
        o.append('int main(int argc, char **argv) {\n    int verbose = argc > 2 && !strcmp(argv[1], "-v") ? atoi(argv[2]) : -1;')
        o.append(CASE_LOOP.replace('RESULT', '''if (verbose < 0) printf("%04x\\n", fold(h));
                else if (fi == verbose) { printf("%s mode %04x case %d cfg %lu", names[fi], modes[mi], c, init.cfg); show_in(&init); show(&t); }'''))
        o.append('    return 0;\n}')
        open(sys.argv[2], 'w').write('\n'.join(o))
        print(len(F), 'forms,', len(F) * NCASE * len(MODES), 'cases')
        return
    answers = [int(l, 16) for l in open(sys.argv[2]).read().split()]
    assert len(answers) == len(F) * NCASE * len(MODES), (len(answers), len(F) * NCASE * len(MODES))
    o = ['''// Generated by tools/gen-x87-test.py -- do not edit.
//
// The x87, every D8-DF form but the transcendentals and the environment
// loads and stores: arithmetic, compares, loads, stores, integer and BCD
// conversions, FCMOVcc, the stack and control operations, the reserved
// aliases -- under the four rounding modes times the four precision control
// settings, from stacks that overflow and underflow -- against answers
// recorded on real x86 hardware (an AMD Ryzen): per case a 16-bit hash of
// FXSAVE's control word, status word, tag and registers, the memory operand,
// the flags and AX. Builds -m32 and 64-bit alike. -v N prints form N in
// full (diff it against the hardware's), -l lists the forms.''', COMMON]
    for i, (_, code, _) in enumerate(F):
        o.append(runner(i, code))
    table(o)
    o.append('static const uint16_t answers[] = {')
    for k in range(0, len(answers), 12):
        o.append('    ' + ','.join(f'0x{a:04x}' for a in answers[k:k + 12]) + ',')
    o.append('};')
    o.append(r'''
static unsigned long checks, bad, bad_form[NFORM];
int main(int argc, char **argv) {
    int verbose = argc > 2 && !strcmp(argv[1], "-v") ? atoi(argv[2]) : -1;
    if (argc > 1 && !strcmp(argv[1], "-l")) { for (int i = 0; i < NFORM; i++) printf("%d %s\n", i, names[i]); return 0; }
    unsigned long idx = 0;''')
    o.append(CASE_LOOP.replace('RESULT', '''checks++;
                if (fi == verbose) { printf("%s mode %04x case %d cfg %lu", names[fi], modes[mi], c, init.cfg); show_in(&init); show(&t); }
                else if (verbose < 0 && fold(h) != answers[idx] && (bad_form[fi]++, bad++ < 40)) {
                    printf("FAIL %s mode %04x case %d cfg %lu", names[fi], modes[mi], c, init.cfg);
                    show_in(&init);
                    show(&t);
                }
                idx++;'''))
    o.append(r'''    if (verbose >= 0) return 0;
    for (int i = 0; i < NFORM; i++)
        if (bad_form[i])
            printf("  form %d %s: %lu of %d\n", i, names[i], bad_form[i], NCASE * NMODE);
    printf("%s: %s (%d forms, %lu checks, %lu mismatches)\n", TESTNAME, bad ? "FAIL" : "PASS", NFORM, checks, bad);
    return bad != 0;
}''')
    open(sys.argv[3], 'w').write('\n'.join(o).replace('TESTNAME', '"x87_kat_unmasked"' if UNMASKED else '"x87_kat"'))
    print(len(F), 'forms')

main()
