// Differential test for float80.c's round-to-nearest fast paths
// (f80_add_fast, f80_mul_fast, f80_div_fast): every case they accept must give exactly the
// general code's result, precision flag (PE) and rounded-up flag (C1), at
// each precision-control width. Random operands plus the edges: halfway
// cases, carries out of the top bit, cancellation, exponent gaps around 63,
// results next to overflow and underflow.
//
//     cc -O2 -I. -o /tmp/f80fast emu/float80-fast-test.c emu/float80.c && /tmp/f80fast
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "emu/float80.h"

extern __thread int f80_inexact, f80_rounded_up, f80_exceptions;
typedef unsigned __int128 u128;
uint64_t f80_u128_isqrt(u128 n, u128 *rem);

// The digit-by-digit integer root f80_u128_isqrt replaced, as the reference.
static uint64_t isqrt_ref(u128 n, u128 *rem) {
    u128 res = 0, bit = (u128) 1 << 126;
    while (bit > n)
        bit >>= 2;
    while (bit != 0) {
        if (n >= res + bit) { n -= res + bit; res = (res >> 1) + bit; }
        else res >>= 1;
        bit >>= 2;
    }
    *rem = n;
    return (uint64_t) res;
}

static int isqrt_check(u128 n) {
    u128 r1, r2;
    uint64_t a = f80_u128_isqrt(n, &r1), b = isqrt_ref(n, &r2);
    if (a != b || r1 != r2) {
        printf("FAIL isqrt n=%016llx%016llx: %llx vs ref %llx\n", (unsigned long long) (n >> 64),
                (unsigned long long) n, (unsigned long long) a, (unsigned long long) b);
        return 1;
    }
    return 0;
}

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint64_t next(void) {
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return rng;
}

static float80 pick(void) {
    float80 f;
    uint64_t r = next();
    f.sign = r & 1;
    switch ((r >> 1) % 8) {
    case 0: f.signif = 1ull << 63; break;                              // power of two
    case 1: f.signif = ~0ull; break;                                   // all ones
    case 2: f.signif = (1ull << 63) | (next() & 0x7ff); break;         // low bits only
    case 3: f.signif = (1ull << 63) | (next() << 11 >> 1); break;      // double-like
    default: f.signif = next() | (1ull << 63); break;
    }
    switch ((r >> 4) % 6) {
    case 0: f.exp = 0x3fff + (int) (next() % 8) - 4; break;
    case 1: f.exp = 1 + next() % 80; break;                             // near underflow
    case 2: f.exp = 0x7ffe - next() % 80; break;                        // near overflow
    default: f.exp = 1 + next() % 0x7ffe; break;
    }
    return f;
}

static float80 near(float80 a) {
    // an operand whose exponent is within ~70 of a's, so the add does real work
    float80 b = pick();
    int e = (int) a.exp + (int) (next() % 141) - 70;
    if (e < 1) e = 1;
    if (e > 0x7ffe) e = 0x7ffe;
    b.exp = e;
    return b;
}

static int same(float80 x, float80 y) {
    return x.signif == y.signif && x.exp == y.exp && x.sign == y.sign;
}

int main(int argc, char **argv) {
    long n = argc > 1 ? atol(argv[1]) : 3000000;
    static const int precs[] = { 64, 53, 24 };
    long bad = 0, taken[3] = { 0, 0, 0 }, tried[3] = { 0, 0, 0 };
    f80_rounding_mode = round_to_nearest;
    // f80_u128_isqrt (FSQRT's integer root) against the digit-by-digit one:
    // squares and their neighbours near both ends, then random operands.
    {
        long isq = 0;
        static const uint64_t ks[] = { 1, 2, 3, 0xffffffffull, 1ull << 63, (1ull << 63) + 1,
            0xb504f333f9de6484ull, ~0ull, ~0ull - 1, 0xfffffffffffffffeull >> 1 };
        for (unsigned i = 0; i < sizeof(ks) / sizeof(ks[0]); i++) {
            u128 sq = (u128) ks[i] * ks[i];
            isq += isqrt_check(sq) + isqrt_check(sq - 1) + isqrt_check(sq + 1);
        }
        isq += isqrt_check(0) + isqrt_check(~(u128) 0) + isqrt_check((u128) 1 << 126) +
               isqrt_check(((u128) 1 << 126) - 1);
        for (long i = 0; i < n; i++) {
            u128 v = ((u128) next() << 64) | next();
            isq += isqrt_check(v | ((u128) 1 << 126));      // FSQRT's range
            isq += isqrt_check(v >> (next() % 128));          // anything
            uint64_t k = next();
            u128 sq = (u128) k * k;
            isq += isqrt_check(sq) + isqrt_check(sq - 1);
        }
        printf("isqrt: %ld mismatches\n", isq);
        bad += isq;
    }
    for (int pi = 0; pi < 3; pi++) {
        int p = precs[pi];
        f80_precision = p;
        for (long i = 0; i < n; i++) {
            float80 a = pick(), b = (i & 1) ? near(a) : pick();
            for (int op = 0; op < 3; op++) {
                float80 fast, gen;
                bool ix, up;
                tried[op]++;
                bool ok = op == 0 ? f80_add_fast(a, b, p, &fast, &ix, &up)
                        : op == 1 ? f80_mul_fast(a, b, p, &fast, &ix, &up)
                                  : f80_div_fast(a, b, p, &fast, &ix, &up);
                if (!ok)
                    continue;
                taken[op]++;
                f80_inexact = 0; f80_rounded_up = 0; f80_exceptions = 0;
                gen = op == 0 ? f80_add(a, b) : op == 1 ? f80_mul(a, b) : f80_div(a, b);
                if (!same(fast, gen) || ix != (f80_inexact != 0) || up != (f80_rounded_up != 0) ||
                        f80_exceptions != 0) {
                    if (bad++ < 10)
                        printf("MISMATCH %s p=%d a=%d:%04x:%016llx b=%d:%04x:%016llx fast=%d:%04x:%016llx ix%d up%d gen=%d:%04x:%016llx ix%d up%d exc%x\n",
                               op == 0 ? "add" : op == 1 ? "mul" : "div", p, a.sign, a.exp, (unsigned long long) a.signif,
                               b.sign, b.exp, (unsigned long long) b.signif,
                               fast.sign, fast.exp, (unsigned long long) fast.signif, ix, up,
                               gen.sign, gen.exp, (unsigned long long) gen.signif,
                               f80_inexact != 0, f80_rounded_up != 0, f80_exceptions);
                }
            }
        }
    }
    printf("add: %ld/%ld fast, mul: %ld/%ld fast, div: %ld/%ld fast, %ld mismatches\n",
           taken[0], tried[0], taken[1], tried[1], taken[2], tried[2], bad);
    return bad != 0;
}
