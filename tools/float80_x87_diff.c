// float80_x87_diff.c -- emu/float80.c against a real x87: result bits,
// exception flags (IE ZE OE UE PE) and C1, for add, sub, mul, div, sqrt,
// fst m64/m32, fistp m64, frndint and fld m64/m32, in all four rounding modes
// and three precisions, over edge operands and 60,000 random ones.
//
// It needs an x86_64 Linux host (the x87 is the oracle). On camd:
//
//   scp emu/float80.c emu/float80.h misc.h debug.h tools/float80_x87_diff.c camd:/tmp/f80/
//   ssh camd 'cd /tmp/f80 && gcc -O1 -I. -o d float80.c float80_x87_diff.c -lm && ./d'
//
// (with float80.c and float80.h under emu/ on the far side if misc.h's
// includes want it). It prints each mismatch, a count per operation, and a
// total, and exits 1 on any mismatch. NaN results are compared only for being
// NaN: float80 answers some invalid operations with the positive quiet NaN
// where the x87 gives the negative indefinite, a known separate difference.
// The x87 DE flag is not compared: float80 raises it only for loads and sqrt.
// 0 mismatches in 487,030 checks as of the commit that added this.
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "float80.h"
#include <signal.h>
#include <unistd.h>
static volatile int cur_op; static long double cur_a, cur_b; static volatile unsigned short cur_cw;
static void on_abrt(int s) { (void) s; union { long double ld; float80 f; } a = {cur_a}, b = {cur_b}; fprintf(stderr, "ABORT op=%d cw=%04x a=%04x:%016llx b=%04x:%016llx\n", cur_op, cur_cw, a.f.signExp, (unsigned long long) a.f.signif, b.f.signExp, (unsigned long long) b.f.signif); _exit(3); }

enum { ADD, SUB, MUL, DIV, SQRT, STD, STF, STI, RND, LDD, LDF, NOPS };
static const char *names[] = {"add", "sub", "mul", "div", "sqrt", "fst64", "fst32", "fist64", "frndint", "fld64", "fld32"};
static int fails, checks, flagfails, valfails;
static int cat[NOPS][3]; static int shown[NOPS][3];

typedef union { long double ld; float80 f; unsigned char b[16]; } u80;

static unsigned short hw(int op, long double a, long double b, unsigned short cw, u80 *out, uint64_t *iout) {
    unsigned short sw;
    memset(out, 0, sizeof(*out));
    *iout = 0;
    double d = (double) a; float fl = (float) a;
    memcpy(&d, &a, 8); // for LDD/LDF the caller passes bits in a
    __asm__ volatile("fldcw %0; fnclex" :: "m"(cw));
    switch (op) {
    case ADD: __asm__ volatile("fldt %3; fldt %2; fadd %%st(1), %%st; fnstsw %1; fstpt %0; fstp %%st(0)" : "=m"(out->ld), "=m"(sw) : "m"(a), "m"(b)); goto done; break;
    case SUB: __asm__ volatile("fldt %3; fldt %2; fsub %%st(1), %%st; fnstsw %1; fstpt %0; fstp %%st(0)" : "=m"(out->ld), "=m"(sw) : "m"(a), "m"(b)); goto done; break;
    case MUL: __asm__ volatile("fldt %3; fldt %2; fmul %%st(1), %%st; fnstsw %1; fstpt %0; fstp %%st(0)" : "=m"(out->ld), "=m"(sw) : "m"(a), "m"(b)); goto done; break;
    case DIV: __asm__ volatile("fldt %3; fldt %2; fdiv %%st(1), %%st; fnstsw %1; fstpt %0; fstp %%st(0)" : "=m"(out->ld), "=m"(sw) : "m"(a), "m"(b)); goto done; break;
    case SQRT: __asm__ volatile("fldt %2; fsqrt; fnstsw %1; fstpt %0" : "=m"(out->ld), "=m"(sw) : "m"(a)); goto done;
    case STD: { double r; __asm__ volatile("fldt %1; fstpl %0" : "=m"(r) : "m"(a)); memcpy(out->b, &r, 8); break; }
    case STF: { float r; __asm__ volatile("fldt %1; fstps %0" : "=m"(r) : "m"(a)); memcpy(out->b, &r, 4); break; }
    case STI: { int64_t r; __asm__ volatile("fldt %1; fistpll %0" : "=m"(r) : "m"(a)); *iout = r; break; }
    case RND: __asm__ volatile("fldt %2; frndint; fnstsw %1; fstpt %0" : "=m"(out->ld), "=m"(sw) : "m"(a)); goto done;
    case LDD: { double x; memcpy(&x, &a, 8); __asm__ volatile("fldl %1; fstpt %0" : "=m"(out->ld) : "m"(x)); break; }
    case LDF: { float x; memcpy(&x, &a, 4); __asm__ volatile("flds %1; fstpt %0" : "=m"(out->ld) : "m"(x)); break; }
    }
    __asm__ volatile("fnstsw %0" : "=m"(sw));
done:
    (void) d; (void) fl;
    return sw;
}

static unsigned short soft(int op, long double a, long double b, unsigned short cw, u80 *out, uint64_t *iout) {
    cur_op = op; cur_a = a; cur_b = b; cur_cw = cw;
    u80 ua = {0}, ub = {0};
    ua.ld = a; ub.ld = b;
    memset(out, 0, sizeof(*out));
    *iout = 0;
    f80_rounding_mode = (cw >> 10) & 3;
    unsigned pc = (cw >> 8) & 3;
    f80_precision = pc == 0 ? 24 : pc == 2 ? 53 : 64;
    f80_exceptions = 0; f80_inexact = 0; f80_rounded_up = 0;
    switch (op) {
    case ADD: out->f = f80_add(ua.f, ub.f); break;
    case SUB: out->f = f80_sub(ua.f, ub.f); break;
    case MUL: out->f = f80_mul(ua.f, ub.f); break;
    case DIV: out->f = f80_div(ua.f, ub.f); break;
    case SQRT: out->f = f80_sqrt(ua.f); break;
    case STD: { double r = f80_to_double(ua.f); memcpy(out->b, &r, 8); break; }
    case STF: { float r = f80_to_float(ua.f); memcpy(out->b, &r, 4); break; }
    case STI: *iout = f80_to_int(ua.f); break;
    case RND: out->f = f80_round(ua.f); break;
    case LDD: { double x; memcpy(&x, &a, 8); out->f = f80_from_double(x); break; }
    case LDF: { float x; memcpy(&x, &a, 4); out->f = f80_from_float(x); break; }
    }
    unsigned short sw = f80_exceptions & 0x1f;
    if (f80_inexact) sw |= 0x20;
    if (f80_rounded_up) sw |= 0x200;
    return sw;
}

static int is_nan80(const u80 *u) { return (u->f.exp == 0x7fff) && (u->f.signif << 1) != 0; }

static void one(int op, long double a, long double b, unsigned short cw) {
    u80 h, s; uint64_t hi, si;
    unsigned short hsw = hw(op, a, b, cw, &h, &hi) & 0x23d;   // IE ZE OE UE PE C1 (DE left out)
    unsigned short ssw = soft(op, a, b, cw, &s, &si) & 0x23d;
    checks++;
    int nbytes = op == STD ? 8 : op == STF ? 4 : op == STI ? 0 : 10;
    int val_bad = (op == STI) ? hi != si : memcmp(h.b, s.b, nbytes) != 0;
    // NaN results: only NaN-ness is compared (indefinite vs quiet NaN sign is
    // a known, separate difference).
    if (val_bad && nbytes == 10 && is_nan80(&h) && is_nan80(&s)) val_bad = 0;
    if (val_bad && (op == STD || op == STF)) {
        uint64_t hb = 0, sb = 0; memcpy(&hb, h.b, nbytes); memcpy(&sb, s.b, nbytes);
        int fb = op == STD ? 52 : 23, eb = op == STD ? 11 : 8;
        uint64_t em = ((1ull << eb) - 1) << fb, fm = (1ull << fb) - 1;
        if ((hb & em) == em && (hb & fm) && (sb & em) == em && (sb & fm)) val_bad = 0;
    }
    int c1_bad = ((hsw ^ ssw) & 0x200) && op != STI && op != LDD && op != LDF;
    int flag_bad = ((hsw ^ ssw) & 0x3d) != 0;
    if (val_bad || flag_bad || c1_bad) {
        fails++; valfails += val_bad; flagfails += flag_bad;
        int k = val_bad ? 0 : flag_bad ? 1 : 2;
        cat[op][k]++;
        if (shown[op][k]++ < 4) {
            u80 ua = {0}, ub = {0}; ua.ld = a; ub.ld = b;
            printf("%-7s cw=%04x a=%04x:%016llx b=%04x:%016llx  hw sw=%03x soft sw=%03x%s%s",
                   names[op], cw, ua.f.signExp, (unsigned long long) ua.f.signif, ub.f.signExp, (unsigned long long) ub.f.signif,
                   hsw, ssw, val_bad ? " VALUE" : "", c1_bad ? " C1" : "");
            if (val_bad) {
                if (op == STI) printf(" hw=%llx soft=%llx", (unsigned long long) hi, (unsigned long long) si);
                else { printf(" hw="); for (int i = nbytes - 1; i >= 0; i--) printf("%02x", h.b[i]); printf(" soft="); for (int i = nbytes - 1; i >= 0; i--) printf("%02x", s.b[i]); }
            }
            printf("\n");
        }
    }
}

static long double mk(unsigned se, uint64_t sig) { u80 u = {0}; u.f.signExp = se; u.f.signif = sig; return u.ld; }

int main(void) {
    signal(SIGABRT, on_abrt);
    long double vals[] = {
        0.0L, -0.0L, 1.0L, -1.0L, 2.0L, 3.0L, 1.0L/3.0L, -1.0L/3.0L, 0.1L, 1e300L, -1e300L, 4.0L, 2.5L, -2.5L, 3.5L,
        DBL_MAX, -DBL_MAX, DBL_MIN, DBL_MIN / 3.0L, DBL_MIN / 4.0L, FLT_MAX, FLT_MIN, FLT_MIN / 3.0L, 5e-324L, 1e-320L,
        LDBL_MAX, LDBL_MIN, LDBL_MIN / 3.0L, 9223372036854775807.0L, 9223372036854775807.5L, -9223372036854775808.0L, -9223372036854775808.5L,
        1e19L, 0x1.fffffffffffffffep62L, 0.5L, 1.5L, 0x1.00000000000008p0L, 0x1.00000000000018p0L, 0x1.000001p0L, 0x1.0000018p0L,
        mk(0x7fff, 0x8000000000000000ull), mk(0xffff, 0x8000000000000000ull), mk(0x7fff, 0xc000000000000000ull),
        mk(0x7fff, 0xa000000000000000ull), mk(0x0000, 0x0000000000000001ull), mk(0x0000, 0x4000000000000000ull),
    };
    int nv = sizeof(vals) / sizeof(vals[0]);
    unsigned short cws[] = {0x037f, 0x077f, 0x0b7f, 0x0f7f, 0x027f, 0x067f, 0x0a7f, 0x0e7f, 0x007f, 0x0c7f};
    srand(12345);
    for (unsigned c = 0; c < sizeof(cws) / sizeof(cws[0]); c++) {
        for (int i = 0; i < nv; i++) {
            for (int j = 0; j < nv; j++)
                for (int op = ADD; op <= DIV; op++)
                    one(op, vals[i], vals[j], cws[c]);
            for (int op = SQRT; op <= RND; op++)
                one(op, vals[i], 0, cws[c]);
        }
        for (int k = 0; k < 20000; k++) {
            u80 a = {0}, b = {0};
            a.f.signif = ((uint64_t) rand() << 33) ^ ((uint64_t) rand() << 2) ^ rand() | 0x8000000000000000ull;
            b.f.signif = ((uint64_t) rand() << 33) ^ ((uint64_t) rand() << 2) ^ rand() | 0x8000000000000000ull;
            a.f.signExp = 0x3fff + (rand() % 400) - 200 + ((rand() & 1) << 15);
            b.f.signExp = 0x3fff + (rand() % 400) - 200 + ((rand() & 1) << 15);
            if (k % 7 == 0) a.f.signExp = (a.f.signExp & 0x8000) | (0x3c00 + rand() % 2);   // near double denormal range
            if (k % 11 == 0) a.f.signExp = (a.f.signExp & 0x8000) | (0x43fe + rand() % 2); // near double max
            one(rand() % 4, a.ld, b.ld, cws[c]);
            one(SQRT + rand() % 5, a.ld, 0, cws[c]);
        }
        // fld m64 / m32 of interesting bit patterns
        uint64_t dbits[] = {0x7ff0000000000001ull, 0x7ff8000000000000ull, 0x0000000000000001ull, 0x3ff0000000000000ull, 0x8000000000000000ull};
        for (unsigned k = 0; k < 5; k++) { long double t = 0; memcpy(&t, &dbits[k], 8); one(LDD, t, 0, cws[c]); }
        uint32_t fbits[] = {0x7f800001u, 0x7fc00000u, 0x00000001u, 0x3f800000u};
        for (unsigned k = 0; k < 4; k++) { long double t = 0; memcpy(&t, &fbits[k], 4); one(LDF, t, 0, cws[c]); }
    }
    for (int op = 0; op < NOPS; op++)
        if (cat[op][0] + cat[op][1] + cat[op][2])
            printf("  %-8s value=%d flags=%d c1=%d\n", names[op], cat[op][0], cat[op][1], cat[op][2]);
    printf("f80diff: %d checks, %d mismatches (%d value, %d flags)\n", checks, fails, valfails, flagfails);
    return fails != 0;
}
