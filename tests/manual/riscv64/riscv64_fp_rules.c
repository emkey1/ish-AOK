// riscv64_fp_rules.c -- two RISC-V FP rules AArch64 does not share, in the
// scalar gadgets (jit/guest-riscv64/fp.S): fmin/fmax return the number against
// a signalling NaN (raising NV), where fminnm gives the default NaN; and a
// float->int convert out of range raises NV alone, NX only for an inexact
// result in range -- with the dynamic rounding mode too, which was rounded by
// frintx and so also raised NX out of range. And fclass.{s,d} on every class,
// signs and NaN kinds, against fpclassify/signbit; an S value not NaN-boxed
// classifies as the canonical (quiet) NaN.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;
static void check(const char *what, uint64_t got, uint64_t want) {
    if (got != want) {
        printf("FAIL %s: %#llx, want %#llx\n", what, (unsigned long long) got, (unsigned long long) want);
        failures++;
    }
}
static uint64_t d2b(double d) { uint64_t b; memcpy(&b, &d, 8); return b; }
static double b2d(uint64_t b) { double d; memcpy(&d, &b, 8); return d; }
#define NV 16
#define NX 1
#define SNAN64 0x7ff4000000000000ull
#define QNAN64 0x7ff8000000000000ull
#define SNAN32 0x7fa00000u

#define MINMAX_D(op, a, b, res, fl) do { \
    double r_; uint64_t f_; \
    __asm__ volatile("fsflags zero\n" op " %0, %2, %3\n frflags %1" : "=f"(r_), "=r"(f_) : "f"(b2d(a)), "f"(b2d(b))); \
    res = d2b(r_); fl = f_; } while (0)

static void minmax(void) {
    uint64_t r, f;
    MINMAX_D("fmin.d", SNAN64, d2b(1.5), r, f);
    check("fmin.d(sNaN, 1.5)", r, d2b(1.5)); check("fmin.d(sNaN, 1.5) flags", f, NV);
    MINMAX_D("fmax.d", d2b(-2.0), SNAN64, r, f);
    check("fmax.d(-2, sNaN)", r, d2b(-2.0)); check("fmax.d(-2, sNaN) flags", f, NV);
    MINMAX_D("fmin.d", QNAN64, d2b(3.0), r, f);
    check("fmin.d(qNaN, 3)", r, d2b(3.0)); check("fmin.d(qNaN, 3) flags", f, 0);
    MINMAX_D("fmin.d", SNAN64, QNAN64, r, f);
    check("fmin.d(sNaN, qNaN)", r, QNAN64); check("fmin.d(sNaN, qNaN) flags", f, NV);
    MINMAX_D("fmin.d", d2b(0.0), d2b(-0.0), r, f);
    check("fmin.d(+0, -0)", r, d2b(-0.0));
    MINMAX_D("fmax.d", d2b(-0.0), d2b(0.0), r, f);
    check("fmax.d(-0, +0)", r, d2b(0.0));
    // single: NaN-boxed operands
    float fr; uint64_t ff;
    uint32_t sn = SNAN32;
    float fs; memcpy(&fs, &sn, 4);
    __asm__ volatile("fsflags zero\n fmin.s %0, %2, %3\n frflags %1" : "=f"(fr), "=r"(ff) : "f"(fs), "f"(2.5f));
    check("fmin.s(sNaN, 2.5)", (uint64_t) (fr == 2.5f), 1); check("fmin.s(sNaN, 2.5) flags", ff, NV);
    __asm__ volatile("fsflags zero\n fmax.s %0, %2, %3\n frflags %1" : "=f"(fr), "=r"(ff) : "f"(-7.0f), "f"(fs));
    check("fmax.s(-7, sNaN)", (uint64_t) (fr == -7.0f), 1); check("fmax.s(-7, sNaN) flags", ff, NV);
}

// fcvt with the dynamic rounding mode (frm RNE) and with rtz
#define CVT(insn, rm, x, res, fl) do { \
    int64_t r_; uint64_t f_; \
    __asm__ volatile("fsflags zero\n" insn " %0, %2, " rm "\n frflags %1" : "=r"(r_), "=r"(f_) : "f"(x)); \
    res = (uint64_t) r_; fl = f_; } while (0)
static void converts(void) {
    uint64_t r, f;
    CVT("fcvt.w.d", "dyn", 3000000000.5, r, f);
    check("fcvt.w.d dyn 3e9+0.5", r, 0x7fffffff); check("fcvt.w.d dyn 3e9+0.5 flags", f, NV);
    CVT("fcvt.w.d", "dyn", -3000000000.5, r, f);
    check("fcvt.w.d dyn -3e9-0.5", r, (uint64_t) (int64_t) INT32_MIN); check("... flags", f, NV);
    CVT("fcvt.wu.d", "dyn", -1.5, r, f);
    check("fcvt.wu.d dyn -1.5", r, 0); check("fcvt.wu.d dyn -1.5 flags", f, NV);
    CVT("fcvt.wu.d", "dyn", -0.3, r, f);
    check("fcvt.wu.d dyn -0.3", r, 0); check("fcvt.wu.d dyn -0.3 flags", f, NX);
    CVT("fcvt.w.d", "dyn", 2.5, r, f);
    check("fcvt.w.d dyn 2.5", r, 2); check("fcvt.w.d dyn 2.5 flags", f, NX);
    CVT("fcvt.w.d", "dyn", 7.0, r, f);
    check("fcvt.w.d dyn 7", r, 7); check("fcvt.w.d dyn 7 flags", f, 0);
    CVT("fcvt.l.d", "dyn", b2d(QNAN64), r, f);
    check("fcvt.l.d dyn NaN", r, 0x7fffffffffffffffull); check("fcvt.l.d dyn NaN flags", f, NV);
    CVT("fcvt.l.d", "dyn", 1e19 + 4096.0, r, f);
    check("fcvt.l.d dyn 1e19", r, 0x7fffffffffffffffull); check("fcvt.l.d dyn 1e19 flags", f, NV);
    CVT("fcvt.w.d", "rtz", 3000000000.5, r, f);
    check("fcvt.w.d rtz 3e9+0.5", r, 0x7fffffff); check("fcvt.w.d rtz 3e9+0.5 flags", f, NV);
    CVT("fcvt.w.d", "rtz", 2.75, r, f);
    check("fcvt.w.d rtz 2.75", r, 2); check("fcvt.w.d rtz 2.75 flags", f, NX);
    float big = 1e10f + 0.0f;
    int64_t r32; uint64_t f32;
    __asm__ volatile("fsflags zero\n fcvt.w.s %0, %2, dyn\n frflags %1" : "=r"(r32), "=r"(f32) : "f"(big));
    check("fcvt.w.s dyn 1e10", (uint64_t) r32, 0x7fffffff); check("fcvt.w.s dyn 1e10 flags", f32, NV);
    __asm__ volatile("fsflags zero\n fcvt.w.s %0, %2, dyn\n frflags %1" : "=r"(r32), "=r"(f32) : "f"(-2.5f));
    check("fcvt.w.s dyn -2.5", (uint64_t) r32, (uint64_t) -2); check("fcvt.w.s dyn -2.5 flags", f32, NX);
}

// fclass: the class from libm's view of the value, the NaN kind from the
// quiet bit -- an oracle that shares nothing with the gadget's bit tests.
#define want_class(v, quiet) want_class_(fpclassify(v), signbit(v) != 0, quiet)
static unsigned want_class_(int cls, int neg, int quiet) {
    switch (cls) {
    case FP_NAN: return quiet ? 9 : 8;
    case FP_INFINITE: return neg ? 0 : 7;
    case FP_ZERO: return neg ? 3 : 4;
    case FP_SUBNORMAL: return neg ? 2 : 5;
    default: return neg ? 1 : 6;
    }
}
static void classes(void) {
    static const uint64_t d[] = {
        0, 0x8000000000000000ull, 1, 0x800fffffffffffffull, 0x0010000000000000ull,
        0x3ff0000000000000ull, 0xbff8000000000000ull, 0x7fefffffffffffffull,
        0x7ff0000000000000ull, 0xfff0000000000000ull, QNAN64, SNAN64,
        0x7ff0000000000001ull, 0xfff8000000000001ull, 0x7fffffffffffffffull,
        0x000fffffffffffffull, 0xffefffffffffffffull,
    };
    char what[64];
    for (unsigned i = 0; i < sizeof(d) / sizeof(d[0]); i++) {
        uint64_t r;
        __asm__ volatile("fclass.d %0, %1" : "=r"(r) : "f"(b2d(d[i])));
        snprintf(what, sizeof(what), "fclass.d %#llx", (unsigned long long) d[i]);
        check(what, r, 1ull << want_class(b2d(d[i]), (d[i] >> 51) & 1));
    }
    static const uint32_t f[] = {
        0, 0x80000000u, 1, 0x807fffffu, 0x00800000u, 0x3f800000u, 0xbfc00000u,
        0x7f7fffffu, 0x7f800000u, 0xff800000u, 0x7fc00000u, SNAN32, 0x7f800001u,
        0xffc00001u, 0x7fffffffu, 0x007fffffu, 0xff7fffffu,
    };
    for (unsigned i = 0; i < sizeof(f) / sizeof(f[0]); i++) {
        uint64_t r;
        float v;
        memcpy(&v, &f[i], 4);
        __asm__ volatile("fclass.s %0, %1" : "=r"(r) : "f"(v));
        snprintf(what, sizeof(what), "fclass.s %#x", f[i]);
        check(what, r, 1ull << want_class(v, (f[i] >> 22) & 1));
        // the same bits not NaN-boxed (fmv.d.x leaves the upper half 0): the
        // canonical NaN, class 9, whatever they say
        uint64_t unboxed = f[i];
        __asm__ volatile("fmv.d.x ft0, %1\n fclass.s %0, ft0" : "=r"(r) : "r"(unboxed) : "ft0");
        snprintf(what, sizeof(what), "fclass.s unboxed %#x", f[i]);
        check(what, r, 1u << 9);
    }
}

int main(void) {
    minmax();
    converts();
    classes();
    printf("riscv64_fp_rules: %s\n", failures ? "FAIL" : "PASS");
    return failures != 0;
}
