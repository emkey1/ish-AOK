#!/usr/bin/env python3
# Generates tests/manual/x86/x87_trans.c, a known-answer test of the x87
# transcendentals (F2XM1 FYL2X FYL2XP1 FPTAN FPATAN FSIN FCOS FSINCOS) whose
# answers come from real x86 hardware:
#
#   tools/gen-x87-trans-test.py probe PROBE.c     # the probe program
#   (compile PROBE.c on an x86 machine, -m32 or not, run it: ANSWERS)
#   tools/gen-x87-trans-test.py test ANSWERS OUT.c
#
# The program makes its own cases -- the same ones on the hardware and under
# AOK, from loops and an integer generator -- so only the answers are
# embedded: per case the status word, the abridged tag and ST(0), ST(1) from
# FXSAVE. Cases: every special operand (zeros, denormals, a pseudo-denormal,
# +-1, infinities, NaNs, unsupported encodings, the edges of each domain)
# against every other for the two-operand ones, in every rounding mode; a
# full stack (a push overflows) and an empty ST(0) or ST(1); condition codes
# set before the operation; exceptions unmasked; precision control set to
# single (the transcendentals ignore it); and random operands in each
# operation's interesting ranges -- near 1 for the logarithms, near multiples
# of pi/2 for the trigonometry, results that underflow and overflow.
#
# The answers are an AMD Ryzen's (camd). It rounds the transcendentals
# correctly in ~97% of cases and is otherwise 1 ulp off, so a value may be
# 1 ulp from the hardware's, and C1 (which then follows the hardware's
# internal approximation) is not compared for an inexact result. Everything
# else must match exactly: the other status bits, TOP, the tags, zeros'
# signs, NaNs.
#
# Run the test with -v to print every case in the probe's format (diff it
# against the answers, or score it against a high-precision reference).
import sys

HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct f80 { uint64_t m; uint16_t se; } __attribute__((packed));
struct io { struct f80 x, y; uint16_t cw, sw; uint32_t cfg; uint8_t fx[512] __attribute__((aligned(16))); };

// cfg: 1 six more values below (the stack full: a push overflows), 2 ST(1)
// empty, 4 ST(0) empty, 8 FCOM ST(0) first (condition codes set).
#define OP(name, insn) \
static void name(struct io *t) { \
    __asm__ volatile("fninit\n fldcw %[cw]\n testl $1, %[cfg]\n jz 1f\n fld1\n fld1\n fld1\n fld1\n fld1\n fld1\n" \
        "1: fldt %[y]\n fldt %[x]\n testl $2, %[cfg]\n jz 2f\n ffree %%st(1)\n 2: testl $4, %[cfg]\n jz 3f\n ffree %%st(0)\n" \
        "3: testl $8, %[cfg]\n jz 4f\n fcom %%st(0)\n 4: " insn "\n fnstsw %[sw]\n fxsave %[fx]\n fninit" \
        : [sw] "=m"(t->sw), [fx] "=m"(t->fx) : [cw] "m"(t->cw), [x] "m"(t->x), [y] "m"(t->y), [cfg] "m"(t->cfg) : "memory"); \
}
OP(f2xm1, "f2xm1") OP(fyl2x, "fyl2x") OP(fyl2xp1, "fyl2xp1") OP(fptan, "fptan")
OP(fpatan, "fpatan") OP(fsin, "fsin") OP(fcos, "fcos") OP(fsincos, "fsincos")
static const char *const names[] = {"f2xm1", "fyl2x", "fyl2xp1", "fptan", "fpatan", "fsin", "fcos", "fsincos"};
static void (*const fns[])(struct io *) = {f2xm1, fyl2x, fyl2xp1, fptan, fpatan, fsin, fcos, fsincos};
static const int two[] = {0, 1, 1, 0, 1, 0, 0, 0};

static const struct f80 V[] = {
    {0, 0}, {0, 0x8000},                                                // +-0
    {0x0000001234567890ull, 0}, {0x0000001234567890ull, 0x8000},       // +-denormal
    {0x8000000000000001ull, 0},                                         // a pseudo-denormal
    {0x8000000000000000ull, 0x3fff}, {0x8000000000000000ull, 0xbfff},   // +-1
    {0x8000000000000000ull, 0x3ffe}, {0x8000000000000000ull, 0xbffe},   // +-0.5
    {0x8000000000000000ull, 0x4000}, {0x8000000000000000ull, 0xc000},   // +-2
    {0x9999999999999999ull, 0x3ffd}, {0x9999999999999999ull, 0xbffd},   // +-0.3
    {0xcccccccccccccccdull, 0x3ffe}, {0xc000000000000000ull, 0x3fff},   // 0.8, 1.5
    {0x8000000000000000ull, 0x403e}, {0x8000000000000000ull, 0x403f},   // 2^63, 2^64
    {0x8000000000000000ull, 0xc03e}, {0xffffffffffffffffull, 0x403d},   // -2^63, just under 2^63
    {0x8000000000000000ull, 0x7ffe}, {0x8000000000000000ull, 0x0001},   // huge, the smallest normal
    {0x8000000000000000ull, 0x7fff}, {0x8000000000000000ull, 0xffff},   // +-infinity
    {0xc000000000000000ull, 0x7fff}, {0xa000000000000000ull, 0x7fff},   // a QNaN, an SNaN
    {0xc000000000000000ull, 0xffff},                                    // the indefinite
    {0x4000000000000000ull, 0x3fff}, {0x4000000000000000ull, 0x7fff},   // an unnormal, a pseudo-NaN
    {0xc90fdaa22168c235ull, 0x4000}, {0xc90fdaa22168c234ull, 0x3fff},   // pi, pi/2 (rounded)
    {0xffffffffffffffffull, 0xbffe},                                    // just above -1
};
#define NV ((int) (sizeof V / sizeof V[0]))
static const struct f80 ONE = {0x8000000000000000ull, 0x3fff};

static uint64_t rs = 0x9e3779b97f4a7c15ull;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static struct f80 mk(int sign, int e, uint64_t m) {
    struct f80 v = {m | 1ull << 63, (uint16_t) ((sign << 15) | e)};
    return v;
}
// a random normal with unbiased exponent in [lo, hi), either sign
static struct f80 band(int lo, int hi) {
    uint64_t r = rnd();
    return mk((int) (r & 1), 0x3fff + lo + (int) ((r >> 8) % (uint64_t) (hi - lo)), rnd());
}
static struct f80 pos(struct f80 v) { v.se &= 0x7fff; return v; }
static struct f80 den(void) {
    struct f80 v = {rnd() >> (1 + rnd() % 63), (uint16_t) ((rnd() & 1) << 15)};
    if (v.m == 0)
        v.m = 1;
    return v;
}
// k * pi66/2 (pi66 = 0x3243f6a8885a308d3 * 2^-64, the x87's pi), truncated,
// then moved a few ulps: an argument whose reduction leaves almost nothing.
static struct f80 near_pi2(void) {
    uint64_t k = rnd() >> (4 + rnd() % 60);
    if (k == 0)
        k = 1;
    // X = k * P66, P66 = 3 * 2^64 + 0x243f6a8885a308d3: X = hi:lo, 128 bits
    uint64_t p = 0x243f6a8885a308d3ull, a0 = k & 0xffffffff, a1 = k >> 32;
    uint64_t b0 = p & 0xffffffff, b1 = p >> 32;
    uint64_t m00 = a0 * b0, m01 = a0 * b1, m10 = a1 * b0, m11 = a1 * b1;
    uint64_t mid = (m00 >> 32) + (m01 & 0xffffffff) + (m10 & 0xffffffff);
    uint64_t lo = (m00 & 0xffffffff) | mid << 32;
    uint64_t hi = m11 + (m01 >> 32) + (m10 >> 32) + (mid >> 32) + 3 * k;
    // the value is X * 2^-65: the top 64 bits of X, and its exponent
    int nb = 128;
    while (!(hi >> 63)) {
        hi = hi << 1 | lo >> 63;
        lo <<= 1;
        nb--;
    }
    uint64_t m = hi + (rnd() % 7) - 3;
    if (!(m >> 63))
        m = hi;
    return mk(0, 0x3fff + nb - 1 - 65, m);
}
'''

HARNESS += r'''
// The random operands for operation op, its n-th case.
static void random_case(int op, int n, struct f80 *x, struct f80 *y) {
    int k = n % 10;
    *y = (n & 1) ? band(-20, 20) : ONE;
    switch (op) {
    case 0:     // F2XM1
        if (k < 6) *x = band(-64, 0);
        else if (k == 6) *x = mk((int) (rnd() & 1), 1 + (int) (rnd() % 200), rnd());
        else if (k == 7) *x = den();
        else if (k == 8) *x = band(-3, 0);
        else *x = (rnd() & 1) ? mk((int) (rnd() & 1), 0x3ffe, ~(rnd() >> 50)) : mk((int) (rnd() & 1), 0x3fff, rnd() >> 50);
        break;
    case 1:     // FYL2X
        if (k < 4) *x = pos(band(-200, 200));
        else if (k == 4) *x = mk(0, 0x3fff, rnd() >> (8 + rnd() % 56));
        else if (k == 5) *x = mk(0, 0x3ffe, ~(rnd() >> (8 + rnd() % 56)));
        else if (k == 6) { *x = mk(0, 0x7ff0 + (int) (rnd() % 15), rnd()); *y = mk((int) (rnd() & 1), 0x7ff0 + (int) (rnd() % 15), rnd()); }
        else if (k == 7) { *x = mk(0, 0x4000, rnd() >> 20); *y = mk((int) (rnd() & 1), 1 + (int) (rnd() % 60), rnd()); }
        else if (k == 8) *x = pos(den());
        else *x = pos(band(-16000, 16000));
        break;
    case 2:     // FYL2XP1
        if (k < 4) *x = band(-64, -2);
        else if (k < 6) *x = band(-2, 0);
        else if (k == 6) *x = pos(band(0, 64));
        else if (k == 7) *x = mk(1, 0x3ffe, rnd() | 0xc000000000000000ull);
        else if (k == 8) *x = den();
        else *x = band(-16000, -64);
        break;
    case 4:     // FPATAN
        *x = band(-40, 40);
        *y = band(-40, 40);
        if (k == 5) *y = band(-16380, -16000);
        else if (k == 6) { *x = band(16000, 16380); *y = band(-200, 0); }
        else if (k == 7) { *y = *x; y->se ^= (uint16_t) ((rnd() & 1) << 15); x->se ^= (uint16_t) ((rnd() & 1) << 15); }
        else if (k == 8) *y = den();
        else if (k == 9) { *x = band(-5, 5); *y = band(-80, 0); }
        break;
    default:    // FPTAN FSIN FCOS FSINCOS
        if (k == 8) *x = near_pi2();
        else if (k == 9) *x = (n & 2) ? den() : band(-16000, -60);
        else if (k % 4 == 0) *x = band(-64, 0);
        else if (k % 4 == 1) *x = band(0, 4);
        else if (k % 4 == 2) *x = band(4, 32);
        else *x = band(32, 63);
        if (n % 20 == 18)
            x->se |= 0x8000;
        break;
    }
}

#define NRANDOM 280
// Every case, in order, to one(op, cw, cfg, x, y).
static void cases(void (*one)(int, uint16_t, uint32_t, struct f80, struct f80)) {
    for (int op = 0; op < 8; op++)             // the special operands, every rounding mode
        for (int i = 0; i < NV; i++)
            for (int j = 0; j < (two[op] ? NV : 1); j++)
                for (int rc = 0; rc < 4; rc++) {
                    if (two[op] && rc && (i + j) % 4)
                        continue;
                    one(op, (uint16_t) (0x37f | rc << 10), 0, V[i], two[op] ? V[j] : ONE);
                }
    for (int op = 0; op < 8; op++)             // the stack: full, ST(1) empty, ST(0) empty
        for (int i = 0; i < 7; i++)
            for (int cfg = 1; cfg <= 4; cfg <<= 1)
                one(op, 0x37f, (uint32_t) cfg, V[i], ONE);
    for (int op = 0; op < 8; op++)             // condition codes already set
        for (int i = 0; i < NV; i++)
            one(op, 0x37f, 8, V[i], two[op] ? V[14] : ONE);
    for (int op = 0; op < 8; op++)             // every exception unmasked
        for (int i = 0; i < NV; i++)
            for (int j = 0; j < (two[op] ? NV : 1); j++) {
                if (two[op] && (i + j) % 3)
                    continue;
                one(op, (uint16_t) (0x340 | ((i + j) & 3) << 10), 0, V[i], two[op] ? V[j] : ONE);
            }
    for (int op = 0; op < 8; op++)             // random operands
        for (int n = 0; n < NRANDOM; n++) {
            struct f80 x, y;
            random_case(op, n, &x, &y);
            for (int rc = 0; rc < 4; rc++)
                one(op, (uint16_t) (0x37f | rc << 10), 0, x, y);
            if (n % 5 == 0)                     // unmasked
                one(op, (uint16_t) (0x340 | (n & 3) << 10), 0, x, y);
            if (n % 13 == 0)                    // single precision
                one(op, (uint16_t) (0x07f | (n & 3) << 10), 0, x, y);
            if (n % 11 == 0)
                one(op, 0x37f, 8, x, y);
        }
}

static void run(int op, uint16_t cw, uint32_t cfg, struct f80 x, struct f80 y, uint16_t *sw, uint8_t *tag,
        struct f80 *r0, struct f80 *r1) {
    struct io t;
    memset(&t, 0, sizeof t);
    t.x = x;
    t.y = y;
    t.cw = cw;
    t.cfg = cfg;
    fns[op](&t);
    *sw = t.sw;
    *tag = t.fx[4];
    memcpy(&r0->m, t.fx + 32, 8);
    memcpy(&r0->se, t.fx + 40, 2);
    memcpy(&r1->m, t.fx + 48, 8);
    memcpy(&r1->se, t.fx + 56, 2);
}

static void print_case(int op, uint16_t cw, uint32_t cfg, struct f80 x, struct f80 y, uint16_t sw, uint8_t tag,
        struct f80 r0, struct f80 r1) {
    printf("%s %04x %u %04x:%016llx %04x:%016llx -> %04x %02x %04x:%016llx %04x:%016llx\n", names[op], cw, cfg,
           x.se, (unsigned long long) x.m, y.se, (unsigned long long) y.m, sw, tag,
           r0.se, (unsigned long long) r0.m, r1.se, (unsigned long long) r1.m);
}
'''

PROBE = r'''
static void probe_one(int op, uint16_t cw, uint32_t cfg, struct f80 x, struct f80 y) {
    uint16_t sw;
    uint8_t tag;
    struct f80 r0, r1;
    run(op, cw, cfg, x, y, &sw, &tag, &r0, &r1);
    print_case(op, cw, cfg, x, y, sw, tag, r0, r1);
}
int main(void) {
    cases(probe_one);
    return 0;
}
'''

TEST = r'''
#include <stdlib.h>
struct answer { uint16_t sw; uint8_t tag; uint16_t se0, se1; uint64_t m0, m1; };
static const struct answer A[] = {
ANSWERS
};
#define NA ((long) (sizeof A / sizeof A[0]))

// 0, or the distance in ulps (capped at 2) between two finite values of the
// same sign; 2 for anything else unequal.
static int ulps(struct f80 a, struct f80 b) {
    if (a.m == b.m && a.se == b.se)
        return 0;
    int ea = a.se & 0x7fff, eb = b.se & 0x7fff;
    if (ea == 0x7fff || eb == 0x7fff || (a.se ^ b.se) & 0x8000)
        return 2;
    if ((ea && !(a.m >> 63)) || (eb && !(b.m >> 63)))
        return 2;
    if (ea == 0) ea = 1;
    if (eb == 0) eb = 1;
    if (ea == eb)
        return (a.m > b.m ? a.m - b.m : b.m - a.m) == 1 ? 1 : 2;
    if (ea < eb) {
        struct f80 t = a; a = b; b = t;
        int u = ea; ea = eb; eb = u;
    }
    // a one binade up: 1 ulp only as its smallest value against b's largest
    return ea == eb + 1 && a.m == 1ull << 63 && b.m == ~0ull ? 1 : 2;
}

static long idx, bad, inexact_off, verbose;
static void check_one(int op, uint16_t cw, uint32_t cfg, struct f80 x, struct f80 y) {
    uint16_t sw;
    uint8_t tag;
    struct f80 r0, r1;
    run(op, cw, cfg, x, y, &sw, &tag, &r0, &r1);
    if (verbose)
        print_case(op, cw, cfg, x, y, sw, tag, r0, r1);
    if (idx >= NA) {
        idx++;
        bad++;
        return;
    }
    const struct answer *a = &A[idx++];
    struct f80 h0 = {a->m0, a->se0}, h1 = {a->m1, a->se1};
    int top = (a->sw >> 11) & 7;
    int d0 = (a->tag >> top & 1) ? ulps(r0, h0) : 0;
    int d1 = (a->tag >> ((top + 1) & 7) & 1) ? ulps(r1, h1) : 0;
    uint16_t mask = (d0 || d1 || (a->sw & 0x20)) ? (uint16_t) ~0x200 : (uint16_t) 0xffff;
    if (d0 > 1 || d1 > 1 || ((sw ^ a->sw) & mask) || tag != a->tag) {
        if (bad++ < 40) {
            printf("MISMATCH %ld: ", idx - 1);
            print_case(op, cw, cfg, x, y, sw, tag, r0, r1);
            printf("   hardware: %04x %02x %04x:%016llx %04x:%016llx\n", a->sw, a->tag,
                   a->se0, (unsigned long long) a->m0, a->se1, (unsigned long long) a->m1);
        }
    } else if (d0 || d1) {
        inexact_off++;
    }
}
int main(int argc, char **argv) {
    verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
    cases(check_one);
    if (idx != NA) {
        printf("case count %ld, answers %ld\n", idx, NA);
        bad++;
    }
    printf("%s: %s (%ld cases, %ld 1 ulp from the hardware, %ld mismatches)\n", "x87_trans",
           bad ? "FAIL" : "PASS", idx, inexact_off, bad);
    return bad != 0;
}
'''

HEAD = ('// Generated by tools/gen-x87-trans-test.py -- do not edit. A known-answer\n'
        '// test of the x87 transcendentals, the answers an AMD Ryzen\'s; see the\n'
        '// generator for what it covers and how close a value must be.\n')

def main():
    if sys.argv[1] == 'probe':
        open(sys.argv[2], 'w').write(HEAD + HARNESS + PROBE)
        return
    rows = []
    for line in open(sys.argv[2]):
        p = line.split()
        if len(p) < 10 or p[5] != '->':
            continue
        sw, tag = int(p[6], 16), int(p[7], 16)
        r0 = p[8].split(':'); r1 = p[9].split(':')
        rows.append('{0x%04x,0x%02x,0x%s,0x%s,0x%sull,0x%sull},' % (sw, tag, r0[0], r1[0], r0[1], r1[1]))
    open(sys.argv[3], 'w').write(HEAD + HARNESS + TEST.replace('ANSWERS', '\n'.join(rows)))

main()
