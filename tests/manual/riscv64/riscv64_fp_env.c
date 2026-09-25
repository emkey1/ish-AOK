// riscv64_fp_env.c -- frm, fflags and the per-instruction rounding mode.
//
// The riscv64 engine ran every FP instruction in the host's round-to-nearest
// and never set fflags; a static rm of RDN, RUP or RMM on fcvt to integer was
// converted as RNE (glibc's floor() is fcvt.l.d rdn, so floor(2.7) was 3); a
// NaN converted to 0 where RISC-V gives the largest integer; flt and fle raised
// nothing for a quiet NaN; and a static rm on arithmetic was ignored.
//
// No RISC-V hardware is at hand, so the expected values are the ISA's: the
// unprivileged spec's F/D chapters (rounding, fcvt's NaN and range table,
// which comparisons signal, the canonical NaN).
#include <stdint.h>
#include <string.h>
#include "../test_common.h"

#define NV 0x10
#define DZ 0x08
#define OF 0x04
#define UF 0x02
#define NX 0x01

static void check(const char *what, uint64_t got, uint64_t want) {
    if (got == want) {
        test_logf("  %-36s %#llx\n", what, (unsigned long long) got);
        return;
    }
    printf("FAIL %s: got %#llx expected %#llx\n", what, (unsigned long long) got, (unsigned long long) want);
    failures_total++;
}

static void set_frm(unsigned rm) { __asm__ volatile("fsrm %0" : : "r"(rm)); }
static unsigned get_frm(void) { unsigned v; __asm__ volatile("frrm %0" : "=r"(v)); return v; }
static void clear_flags(void) { __asm__ volatile("fsflags zero"); }
static unsigned get_flags(void) { unsigned v; __asm__ volatile("frflags %0" : "=r"(v)); return v; }
static uint64_t dbits(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static double bdouble(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }

#define CVT(rm, x) ({ long r_; double x_ = (x); \
    __asm__ volatile("fcvt.l.d %0, %1, " #rm : "=r"(r_) : "f"(x_)); r_; })

static void test_cvt(void) {
    static const double in[3] = {2.5, -2.5, 2.7};
    static const long rne[3] = {2, -2, 3}, rtz[3] = {2, -2, 2}, rdn[3] = {2, -3, 2};
    static const long rup[3] = {3, -2, 3}, rmm[3] = {3, -3, 3};
    for (int i = 0; i < 3; i++) {
        char w[48];
        snprintf(w, sizeof(w), "fcvt.l.d rne %g", in[i]); check(w, CVT(rne, in[i]), rne[i]);
        snprintf(w, sizeof(w), "fcvt.l.d rtz %g", in[i]); check(w, CVT(rtz, in[i]), rtz[i]);
        snprintf(w, sizeof(w), "fcvt.l.d rdn %g", in[i]); check(w, CVT(rdn, in[i]), rdn[i]);
        snprintf(w, sizeof(w), "fcvt.l.d rup %g", in[i]); check(w, CVT(rup, in[i]), rup[i]);
        snprintf(w, sizeof(w), "fcvt.l.d rmm %g", in[i]); check(w, CVT(rmm, in[i]), rmm[i]);
    }
    // dyn follows frm
    static const long dyn[4] = {2, 2, 2, 3};   // RNE RTZ RDN RUP on 2.5
    for (unsigned rm = 0; rm < 4; rm++) {
        char w[48];
        set_frm(rm);
        snprintf(w, sizeof(w), "fcvt.l.d dyn frm=%u 2.5", rm);
        check(w, CVT(dyn, 2.5), dyn[rm]);
    }
    set_frm(0);
    clear_flags();
    (void) CVT(rne, 2.5);
    check("fcvt 2.5 flags", get_flags(), NX);

    // A NaN converts to the largest value, and is invalid.
    double nan = bdouble(0x7ff8000000000000ull);
    long l; unsigned long lu; int w; unsigned wu;
    clear_flags();
    __asm__ volatile("fcvt.l.d %0, %1, rtz" : "=r"(l) : "f"(nan));
    check("fcvt.l.d nan", (uint64_t) l, 0x7fffffffffffffffull);
    check("fcvt.l.d nan flags", get_flags(), NV);
    __asm__ volatile("fcvt.lu.d %0, %1, rtz" : "=r"(lu) : "f"(nan));
    check("fcvt.lu.d nan", lu, 0xffffffffffffffffull);
    __asm__ volatile("fcvt.w.d %0, %1, rtz" : "=r"(w) : "f"(nan));
    check("fcvt.w.d nan", (uint64_t) (int64_t) w, 0x7fffffff);
    long wu_raw;
    __asm__ volatile("fcvt.wu.d %0, %1, rtz" : "=r"(wu_raw) : "f"(nan));
    check("fcvt.wu.d nan (sign-extended)", (uint64_t) wu_raw, 0xffffffffffffffffull);
    (void) wu;
    clear_flags();
    __asm__ volatile("fcvt.w.d %0, %1, rtz" : "=r"(w) : "f"(1e300));
    check("fcvt.w.d 1e300", (uint64_t) (int64_t) w, 0x7fffffff);
    check("fcvt.w.d 1e300 flags", get_flags(), NV);
}

static void test_compare(void) {
    double qnan = bdouble(0x7ff8000000000000ull), snan = bdouble(0x7ff4000000000000ull), one = 1.0;
    long r;
    clear_flags();
    __asm__ volatile("feq.d %0, %1, %2" : "=r"(r) : "f"(qnan), "f"(one));
    check("feq qnan flags", get_flags(), 0);
    clear_flags();
    __asm__ volatile("feq.d %0, %1, %2" : "=r"(r) : "f"(snan), "f"(one));
    check("feq snan flags", get_flags(), NV);
    clear_flags();
    __asm__ volatile("flt.d %0, %1, %2" : "=r"(r) : "f"(qnan), "f"(one));
    check("flt qnan flags", get_flags(), NV);
    clear_flags();
    __asm__ volatile("fle.d %0, %1, %2" : "=r"(r) : "f"(qnan), "f"(one));
    check("fle qnan flags", get_flags(), NV);
    check("fle qnan result", r, 0);
}

static void test_arith(void) {
    double one = 1.0, tiny = 0x1p-60, zero = 0.0, r;
    __asm__ volatile("fadd.d %0, %1, %2, rup" : "=f"(r) : "f"(one), "f"(tiny));
    check("fadd.d rup 1+2^-60", dbits(r), 0x3ff0000000000001ull);
    __asm__ volatile("fadd.d %0, %1, %2, rdn" : "=f"(r) : "f"(one), "f"(tiny));
    check("fadd.d rdn 1+2^-60", dbits(r), 0x3ff0000000000000ull);
    check("frm untouched by static rm", get_frm(), 0);
    set_frm(3);
    __asm__ volatile("fadd.d %0, %1, %2" : "=f"(r) : "f"(one), "f"(tiny));
    check("fadd.d dyn frm=RUP 1+2^-60", dbits(r), 0x3ff0000000000001ull);
    __asm__ volatile("fadd.d %0, %1, %2, rne" : "=f"(r) : "f"(one), "f"(tiny));
    check("fadd.d rne under frm=RUP", dbits(r), 0x3ff0000000000000ull);
    __asm__ volatile("fadd.d %0, %1, %2" : "=f"(r) : "f"(one), "f"(tiny));
    check("fadd.d dyn after static rne", dbits(r), 0x3ff0000000000001ull);
    set_frm(0);

    clear_flags();
    __asm__ volatile("fdiv.d %0, %1, %2" : "=f"(r) : "f"(one), "f"(zero));
    check("fdiv 1/0 flags", get_flags(), DZ);
    clear_flags();
    __asm__ volatile("fdiv.d %0, %1, %2" : "=f"(r) : "f"(zero), "f"(zero));
    check("fdiv 0/0 flags", get_flags(), NV);
    check("fdiv 0/0 canonical NaN", dbits(r), 0x7ff8000000000000ull);
    double big = 1.7976931348623157e308, two = 2.0;
    clear_flags();
    __asm__ volatile("fmul.d %0, %1, %2" : "=f"(r) : "f"(big), "f"(two));
    check("fmul overflow flags", get_flags(), OF | NX);

    // fcsr: frm in bits 7:5, fflags in 4:0, read and written together.
    unsigned fcsr;
    __asm__ volatile("csrw fcsr, %0" : : "r"((2u << 5) | DZ));
    __asm__ volatile("csrr %0, fcsr" : "=r"(fcsr));
    check("fcsr round trip", fcsr, (2u << 5) | DZ);
    check("frm from fcsr", get_frm(), 2);
    check("fflags from fcsr", get_flags(), DZ);
    __asm__ volatile("csrw fcsr, zero");
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    test_logf("fcvt:\n");
    test_cvt();
    test_logf("compare:\n");
    test_compare();
    test_logf("arithmetic:\n");
    test_arith();
    return finish_suite("riscv64_fp_env");
}
