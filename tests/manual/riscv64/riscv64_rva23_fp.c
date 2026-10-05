// riscv64_rva23_fp.c -- the RVA23 FP extensions AOK's riscv64 JIT
// implements, Zfa and Zfhmin, against references built for rv64gc (so the
// compiler cannot use the instructions under test): fli.s/d (all 32
// constants), fminm/fmaxm, fround/froundnx in every rounding mode (and
// their inexact flag), fltq/fleq (quiet: no NV for a quiet NaN),
// fcvtmod.w.d (value and flags); flh/fsh, fmv.x.h/fmv.h.x, fcvt between
// half and single/double (soft-float references), and the NaN boxing.
// One line per mismatch, PASS/FAIL at the end, exit 1 on failure.

#include <fenv.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define OPT ".option push\n.option arch, +zfa, +zfhmin, +d\n"
#define REF __attribute__((target("arch=rv64gc"), noinline))

static unsigned long checks, bad;
static void check(const char *what, double in, uint64_t got, uint64_t want) {
    checks++;
    if (got != want && bad++ < 40)
        printf("%s(%g) = %#llx, want %#llx\n", what, in, (unsigned long long) got,
               (unsigned long long) want);
}
static uint64_t dbits(double d) { uint64_t b; memcpy(&b, &d, 8); return b; }
static uint32_t fbits(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }
static double bitsd(uint64_t b) { double d; memcpy(&d, &b, 8); return d; }
static float bitsf(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }
static unsigned fflags(void) { unsigned f; __asm__ volatile("frflags %0" : "=r"(f)); return f; }
static void clear_fflags(void) { __asm__ volatile("fsflags zero"); }
#define NV 0x10
#define NX 0x01
#define CANON_D 0x7ff8000000000000ull
#define CANON_S 0x7fc00000u

static uint64_t rng = 0x2545f4914f6cdd1dull;
static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static double rnd_double(void) {
    static const double special[] = {0.0, -0.0, 0.5, -0.5, 1.5, 2.5, -2.5, 0.49999999999999994,
        4503599627370495.5, 4503599627370496.0, 1e300, -1e300, 2147483647.0, 2147483648.0,
        -2147483648.0, -2147483649.0, 4294967296.5, 1e-310, INFINITY, -INFINITY, NAN, 3.7, -3.7};
    uint64_t r = rnd();
    if (r % 4 == 0)
        return special[r % (sizeof(special) / sizeof(special[0]))];
    double d = bitsd(rnd());
    if (r % 4 == 1)
        d = (double) (int64_t) (rnd() % 2000000) / 8.0 - 125000.0;
    return d;
}

// ---- fli ----
#define FLI_D(i) ({ double r_; __asm__ volatile(OPT "fli.d %0, " #i "\n.option pop" : "=f"(r_)); dbits(r_); })
#define FLI_S(i) ({ float r_; __asm__ volatile(OPT "fli.s %0, " #i "\n.option pop" : "=f"(r_)); fbits(r_); })
static void check_fli(void) {
    static const double v[32] = {-1.0, 0, 0x1p-16, 0x1p-15, 0x1p-8, 0x1p-7, 0.0625, 0.125,
        0.25, 0.3125, 0.375, 0.4375, 0.5, 0.625, 0.75, 0.875, 1.0, 1.25, 1.5, 1.75, 2.0, 2.5,
        3.0, 4.0, 8.0, 16.0, 128.0, 256.0, 32768.0, 65536.0, 0, 0};
    uint64_t d[32] = {
        FLI_D(-1.0), FLI_D(min), FLI_D(1.52587890625e-05), FLI_D(3.0517578125e-05),
        FLI_D(0.00390625), FLI_D(0.0078125), FLI_D(0.0625), FLI_D(0.125), FLI_D(0.25),
        FLI_D(0.3125), FLI_D(0.375), FLI_D(0.4375), FLI_D(0.5), FLI_D(0.625), FLI_D(0.75),
        FLI_D(0.875), FLI_D(1.0), FLI_D(1.25), FLI_D(1.5), FLI_D(1.75), FLI_D(2.0), FLI_D(2.5),
        FLI_D(3.0), FLI_D(4.0), FLI_D(8.0), FLI_D(16.0), FLI_D(128.0), FLI_D(256.0),
        FLI_D(32768.0), FLI_D(65536.0), FLI_D(inf), FLI_D(nan)};
    uint32_t s[32] = {
        FLI_S(-1.0), FLI_S(min), FLI_S(1.52587890625e-05), FLI_S(3.0517578125e-05),
        FLI_S(0.00390625), FLI_S(0.0078125), FLI_S(0.0625), FLI_S(0.125), FLI_S(0.25),
        FLI_S(0.3125), FLI_S(0.375), FLI_S(0.4375), FLI_S(0.5), FLI_S(0.625), FLI_S(0.75),
        FLI_S(0.875), FLI_S(1.0), FLI_S(1.25), FLI_S(1.5), FLI_S(1.75), FLI_S(2.0), FLI_S(2.5),
        FLI_S(3.0), FLI_S(4.0), FLI_S(8.0), FLI_S(16.0), FLI_S(128.0), FLI_S(256.0),
        FLI_S(32768.0), FLI_S(65536.0), FLI_S(inf), FLI_S(nan)};
    for (int i = 0; i < 32; i++) {
        uint64_t wd = i == 1 ? 0x0010000000000000ull : i == 30 ? 0x7ff0000000000000ull
                    : i == 31 ? CANON_D : dbits(v[i]);
        uint32_t ws = i == 1 ? 0x00800000u : i == 30 ? 0x7f800000u
                    : i == 31 ? CANON_S : fbits((float) v[i]);
        check("fli.d", i, d[i], wd);
        check("fli.s", i, s[i], ws);
    }
}

// ---- fminm / fmaxm, fltq / fleq ----
REF static uint64_t ref_minm(double a, double b, int max) {
    if (isnan(a) || isnan(b))
        return CANON_D;
    if (a == b && a == 0) // -0 < +0
        return dbits(max ? (signbit(a) ? b : a) : (signbit(a) ? a : b));
    return dbits(max ? (a > b ? a : b) : (a < b ? a : b));
}
static void check_minmax_cmp(void) {
    for (int i = 0; i < 4000; i++) {
        double a = rnd_double(), b = (i & 3) == 0 ? a : rnd_double();
        if (i % 7 == 0) b = -a;
        double r;
        __asm__ volatile(OPT "fminm.d %0, %1, %2\n.option pop" : "=f"(r) : "f"(a), "f"(b));
        check("fminm.d", a, dbits(r), ref_minm(a, b, 0));
        __asm__ volatile(OPT "fmaxm.d %0, %1, %2\n.option pop" : "=f"(r) : "f"(a), "f"(b));
        check("fmaxm.d", a, dbits(r), ref_minm(a, b, 1));
        float fa = (float) a, fb = (float) b, fr;
        __asm__ volatile(OPT "fminm.s %0, %1, %2\n.option pop" : "=f"(fr) : "f"(fa), "f"(fb));
        uint64_t w = ref_minm(fa, fb, 0);
        check("fminm.s", a, fbits(fr), w == CANON_D ? CANON_S : fbits((float) bitsd(w)));
        long lt, le;
        clear_fflags();
        __asm__ volatile(OPT "fltq.d %0, %2, %3\nfleq.d %1, %2, %3\n.option pop"
                         : "=r"(lt), "=r"(le) : "f"(a), "f"(b));
        unsigned fl = fflags();
        check("fltq.d", a, (uint64_t) lt, __builtin_isless(a, b));
        check("fleq.d", a, (uint64_t) le, __builtin_islessequal(a, b));
        // NV only for a signalling NaN (quiet bit clear), never for a quiet one
        int snan = (isnan(a) && !(dbits(a) & (1ull << 51))) || (isnan(b) && !(dbits(b) & (1ull << 51)));
        check("fltq/fleq NV only for a signalling NaN", a, fl & NV, snan ? NV : 0);
        __asm__ volatile(OPT "fltq.s %0, %2, %3\nfleq.s %1, %2, %3\n.option pop"
                         : "=r"(lt), "=r"(le) : "f"(fa), "f"(fb));
        check("fltq.s", a, (uint64_t) lt, __builtin_isless(fa, fb));
        check("fleq.s", a, (uint64_t) le, __builtin_islessequal(fa, fb));
    }
}

// ---- fround / froundnx in each rounding mode ----
REF static double ref_round(double a, int mode) { // nearbyint under `mode`
    int old = fegetround();
    fesetround(mode);
    double r = nearbyint(a);
    fesetround(old);
    return r;
}
static void check_round(void) {
    static const int modes[] = {FE_TONEAREST, FE_TOWARDZERO, FE_DOWNWARD, FE_UPWARD};
    for (int m = 0; m < 4; m++) {
        for (int i = 0; i < 2000; i++) {
            double a = rnd_double(), r;
            double want = ref_round(a, modes[m]);
            uint64_t wb = isnan(a) ? CANON_D : dbits(want);
            fesetround(modes[m]);
            clear_fflags();
            __asm__ volatile(OPT "fround.d %0, %1\n.option pop" : "=f"(r) : "f"(a));
            unsigned f1 = fflags();
            clear_fflags();
            double rx;
            __asm__ volatile(OPT "froundnx.d %0, %1\n.option pop" : "=f"(rx) : "f"(a));
            unsigned f2 = fflags();
            fesetround(FE_TONEAREST);
            check("fround.d", a, dbits(r), wb);
            check("froundnx.d", a, dbits(rx), wb);
            check("fround.d no NX", a, f1 & NX, 0);
            check("froundnx.d NX", a, f2 & NX, !isnan(a) && !isinf(a) && want != a ? NX : 0);
            float fa = (float) a, fr;
            fesetround(modes[m]);
            __asm__ volatile(OPT "fround.s %0, %1\n.option pop" : "=f"(fr) : "f"(fa));
            fesetround(FE_TONEAREST);
            float fw = (float) ref_round(fa, modes[m]);
            check("fround.s", a, fbits(fr), isnan(fa) ? CANON_S : fbits(fw));
        }
    }
    // a static rounding mode in the instruction overrides frm
    double r, a = 2.5;
    __asm__ volatile(OPT "fround.d %0, %1, rup\n.option pop" : "=f"(r) : "f"(a));
    check("fround.d rup", a, dbits(r), dbits(3.0));
    __asm__ volatile(OPT "fround.d %0, %1, rdn\n.option pop" : "=f"(r) : "f"(-a));
    check("fround.d rdn", -a, dbits(r), dbits(-3.0));
}

// ---- fcvtmod.w.d ----
REF static int64_t ref_toint32(double a, unsigned *flags) {
    *flags = 0;
    if (isnan(a) || isinf(a)) {
        *flags = NV;
        return 0;
    }
    double t = trunc(a);
    if (t < -2147483648.0 || t > 2147483647.0)
        *flags = NV;
    else if (t != a)
        *flags = NX;
    double m = fmod(t, 4294967296.0);
    if (m < 0)
        m += 4294967296.0;
    uint32_t u = (uint32_t) m;
    return (int64_t) (int32_t) u;
}
static void check_fcvtmod(void) {
    static const double edge[] = {
        2147483647.0, 2147483648.0, -2147483648.0, -2147483649.0, 2147483647.5,
        -2147483648.5, -2147483647.5, 4294967296.0 + 5, -4294967296.0 - 5, 0x1p52, 0x1p52 + 1,
        0x1p63, -0x1p63, 0x1p84 + 0x1p33, 0x1p1023, -0.0, 0.0, 0x1p-1074, -0x1p-1074, 0.5,
        -0.5, 0.999999, 1.0, -1.0, 0x1p-64, 0x1.fffffffffffffp-1, 0x1p31 - 0x1p-21,
    };
    for (int i = 0; i < 20000 + (int) (sizeof(edge) / sizeof(edge[0])); i++) {
        double a = rnd_double();
        if (i % 3 == 0)
            a = ldexp((double) (int64_t) rnd(), (int) (rnd() % 80) - 64);
        if (i >= 20000)
            a = edge[i - 20000];
        long r;
        clear_fflags();
        __asm__ volatile(OPT "fcvtmod.w.d %0, %1, rtz\n.option pop" : "=r"(r) : "f"(a));
        unsigned f = fflags(), wf;
        int64_t want = ref_toint32(a, &wf);
        check("fcvtmod.w.d", a, (uint64_t) r, (uint64_t) want);
        check("fcvtmod.w.d flags", a, f, wf);
    }
}

// ---- Zfhmin ----
REF static uint16_t ref_s2h(float f) { _Float16 h = (_Float16) f; uint16_t b; memcpy(&b, &h, 2); return b; }
REF static float ref_h2s(uint16_t b) { _Float16 h; memcpy(&h, &b, 2); return (float) h; }
static void check_half(void) {
    for (int i = 0; i < 20000; i++) {
        uint16_t h = (uint16_t) rnd();
        if (i < 8)
            h = (uint16_t[]){0, 0x8000, 0x3c00, 0x7c00, 0xfc00, 0x0001, 0x7bff, 0x7e00}[i];
        uint64_t boxed = 0xffffffffffff0000ull | h;
        // fmv.h.x boxes; fmv.x.h sign-extends; fmv.x.d shows the box
        uint64_t raw, back;
        __asm__ volatile(OPT "fmv.h.x ft0, %2\nfmv.x.d %0, ft0\nfmv.x.h %1, ft0\n.option pop"
                         : "=&r"(raw), "=&r"(back) : "r"((uint64_t) h | 0x123450000ull) : "ft0");
        check("fmv.h.x box", h, raw, boxed);
        check("fmv.x.h", h, back, (uint64_t) (int64_t) (int16_t) h);
        float s;
        double d;
        __asm__ volatile(OPT "fmv.h.x ft0, %2\nfcvt.s.h %0, ft0\nfcvt.d.h %1, ft0\n.option pop"
                         : "=f"(s), "=f"(d) : "r"((uint64_t) h) : "ft0");
        float ws = ref_h2s(h);
        check("fcvt.s.h", h, fbits(s), isnan(ws) ? CANON_S : fbits(ws));
        check("fcvt.d.h", h, dbits(d), isnan(ws) ? CANON_D : dbits((double) ws));
        float f = bitsf((uint32_t) rnd());
        if (i % 2)
            f = (float) rnd_double();
        uint64_t hs;
        __asm__ volatile(OPT "fcvt.h.s ft0, %1\nfmv.x.d %0, ft0\n.option pop" : "=r"(hs) : "f"(f) : "ft0");
        uint16_t wh = isnan(f) ? 0x7e00 : ref_s2h(f);
        check("fcvt.h.s", f, hs, 0xffffffffffff0000ull | wh);
        // flh / fsh through memory, unaligned included
        uint8_t buf[8] = {0};
        memcpy(buf + 1, &h, 2);
        uint64_t loaded;
        __asm__ volatile(OPT "flh ft0, 1(%1)\nfmv.x.d %0, ft0\nfsh ft0, 4(%1)\n.option pop"
                         : "=&r"(loaded) : "r"(buf) : "ft0", "memory"); // & : not buf's register
        uint16_t stored;
        memcpy(&stored, buf + 4, 2);
        check("flh box", h, loaded, boxed);
        check("fsh", h, stored, h);
    }
    double d = 65520.0, r; // rounds to infinity in half
    uint64_t hb;
    __asm__ volatile(OPT "fcvt.h.d ft0, %1\nfmv.x.d %0, ft0\n.option pop" : "=r"(hb) : "f"(d) : "ft0");
    check("fcvt.h.d overflow", d, hb, 0xffffffffffff7c00ull);
    d = 1.0009765625; // exactly half-representable
    __asm__ volatile(OPT "fcvt.h.d ft0, %1\nfcvt.d.h %0, ft0\n.option pop" : "=f"(r) : "f"(d) : "ft0");
    check("fcvt.h.d/fcvt.d.h round trip", d, dbits(r), dbits(d));
}

int main(void) {
    check_fli();
    check_minmax_cmp();
    check_round();
    check_fcvtmod();
    check_half();
    printf("riscv64_rva23_fp: %s (%lu checks, %lu mismatches)\n", bad ? "FAIL" : "PASS", checks, bad);
    return bad != 0;
}
