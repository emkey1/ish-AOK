// x86_fp_env.c -- the x87 control and status words and MXCSR, at the
// instruction level, where fp_env.c only sees what libc makes of them.
//
// The x87 is soft-float in AOK (emu/float80.c) and raised only PE before: no
// IE, ZE, OE or UE from any instruction, precision control rounded wrongly
// whenever the discarded bits sat above bit 63 of the 128-bit intermediate,
// FST m32 rounded twice, FYL2X of 0 (musl's i386 log) gave a NaN, and FNCLEX
// cleared EFLAGS.SF instead of the status word's stack fault; on amd64 an x87
// store to a page not yet writable (copy-on-write after fork) was a SIGSEGV
// rather than a page fault, and FUCOMPP was SIGILL. SSE ran in the
// host's round-to-nearest whatever MXCSR said, CVTSD2SI rounded to nearest
// regardless, and COMISD raised nothing for a quiet NaN.
//
// Every expected value here is what an x86_64 Linux machine gives, in 64- and
// 32-bit builds alike.
#include <float.h>
#include <stdint.h>
#include <string.h>
#include <sys/wait.h>
#include "../test_common.h"

#define IE 0x01
#define DE 0x02
#define ZE 0x04
#define OE 0x08
#define UE 0x10
#define PE 0x20
#define X87_FLAGS 0x3f

// All exceptions masked, the given precision and rounding control.
static unsigned short x87_cw(unsigned rc, unsigned pc) {
    return (unsigned short) (0x007f | (pc << 8) | (rc << 10));
}

typedef union { long double ld; struct { uint64_t m; uint16_t se; } p; unsigned char b[16]; } ld_bits;

static void check(const char *what, uint64_t got, uint64_t want) {
    if (got == want) {
        test_logf("  %-40s %#llx\n", what, (unsigned long long) got);
        return;
    }
    printf("FAIL %s: got %#llx expected %#llx\n", what, (unsigned long long) got, (unsigned long long) want);
    failures_total++;
}

// --- x87 ---------------------------------------------------------------------

static unsigned short x87_div(double a, double b, double *out) {
    unsigned short sw;
    __asm__ volatile("fnclex; fldl %2; fdivl %3; fnstsw %1; fstpl %0"
                     : "=m"(*out), "=m"(sw) : "m"(a), "m"(b));
    return sw & X87_FLAGS;
}

static unsigned short x87_sqrt(double a) {
    unsigned short sw;
    double r;
    __asm__ volatile("fnclex; fldl %2; fsqrt; fnstsw %1; fstpl %0" : "=m"(r), "=m"(sw) : "m"(a));
    return sw & X87_FLAGS;
}

// FCOM (signalling) or FUCOM (quiet) of a against b. The operands travel as
// bit patterns: an i386 caller passing a signalling NaN as a double would
// quiet it on the way, through the x87.
static unsigned short x87_compare(uint64_t a, uint64_t b, int quiet) {
    unsigned short sw;
    if (quiet)
        __asm__ volatile("fnclex; fldl %2; fldl %1; fucompp; fnstsw %0" : "=m"(sw) : "m"(a), "m"(b));
    else
        __asm__ volatile("fnclex; fldl %2; fldl %1; fcompp; fnstsw %0" : "=m"(sw) : "m"(a), "m"(b));
    return sw & X87_FLAGS;
}

static unsigned short x87_fistp(double a, int64_t *out) {
    unsigned short sw;
    __asm__ volatile("fnclex; fldl %2; fistpll %0; fnstsw %1" : "=m"(*out), "=m"(sw) : "m"(a));
    return sw & X87_FLAGS;
}

// 1/3 at a given control word, kept as an 80-bit value.
static unsigned short x87_third(unsigned short cw, ld_bits *out) {
    unsigned short sw, saved;
    double one = 1.0, three = 3.0;
    memset(out, 0, sizeof(*out));
    __asm__ volatile("fnstcw %3; fldcw %2; fnclex; fldl %4; fdivl %5; fnstsw %1; fstpt %0; fldcw %3"
                     : "=m"(out->ld), "=m"(sw) : "m"(cw), "m"(saved), "m"(one), "m"(three));
    return sw & X87_FLAGS;
}

// An 80-bit value stored as a double at a given control word.
static unsigned short x87_store(unsigned short cw, long double v, double *out) {
    unsigned short sw, saved;
    __asm__ volatile("fnstcw %3; fldcw %2; fnclex; fldt %4; fstpl %0; fnstsw %1; fldcw %3"
                     : "=m"(*out), "=m"(sw) : "m"(cw), "m"(saved), "m"(v));
    return sw & X87_FLAGS;
}

static uint64_t dbits(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }

static void test_x87(void) {
    double r, zero = 0.0, one = 1.0, minus_one = -1.0;
    const uint64_t q = 0x7ff8000000000000ull, s = 0x7ff4000000000000ull;

    check("x87 1/0 flags", x87_div(one, zero, &r), ZE);
    check("x87 1/0 value", dbits(r), 0x7ff0000000000000ull);
    check("x87 0/0 flags", x87_div(zero, zero, &r), IE);
    check("x87 1/3 flags", x87_div(one, 3.0, &r), PE);
    check("x87 sqrt(-1) flags", x87_sqrt(minus_one), IE);
    check("x87 sqrt(4) flags (exact)", x87_sqrt(4.0), 0);
    const uint64_t one_bits = 0x3ff0000000000000ull;
    check("x87 fcom qnan", x87_compare(q, one_bits, 0), IE);
    check("x87 fucom qnan", x87_compare(q, one_bits, 1), 0);
    check("x87 fucom snan", x87_compare(s, one_bits, 1), IE);
    check("x87 fcom 1 vs 1", x87_compare(one_bits, one_bits, 0), 0);

    int64_t i;
    check("x87 fistp 1e300 flags", x87_fistp(1e300, &i), IE);
    check("x87 fistp 1e300 value", (uint64_t) i, 0x8000000000000000ull);
    check("x87 fistp 2.5 flags", x87_fistp(2.5, &i), PE);
    check("x87 fistp 2.5 value (nearest)", (uint64_t) i, 2);

    // Rounding control and precision control on 1/3, kept as 80 bits.
    ld_bits t;
    x87_third(x87_cw(0, 3), &t);
    check("x87 1/3 nearest, 64-bit", t.p.m, 0xaaaaaaaaaaaaaaabull);
    x87_third(x87_cw(1, 3), &t);
    check("x87 1/3 down, 64-bit", t.p.m, 0xaaaaaaaaaaaaaaaaull);
    x87_third(x87_cw(0, 2), &t);
    check("x87 1/3 nearest, 53-bit", t.p.m, 0xaaaaaaaaaaaaa800ull);
    x87_third(x87_cw(2, 2), &t);
    check("x87 1/3 up, 53-bit", t.p.m, 0xaaaaaaaaaaaab000ull);
    x87_third(x87_cw(0, 0), &t);
    check("x87 1/3 nearest, 24-bit", t.p.m, 0xaaaaab0000000000ull);
    x87_third(x87_cw(3, 0), &t);
    check("x87 1/3 chop, 24-bit", t.p.m, 0xaaaaaa0000000000ull);

    // FST m64: overflow goes to infinity or to DBL_MAX by the rounding mode,
    // and a tiny inexact result underflows.
    long double big = (long double) DBL_MAX * 2.0L;
    check("x87 fst DBL_MAX*2 nearest flags", x87_store(x87_cw(0, 3), big, &r), OE | PE);
    check("x87 fst DBL_MAX*2 nearest value", dbits(r), 0x7ff0000000000000ull);
    check("x87 fst DBL_MAX*2 chop flags", x87_store(x87_cw(3, 3), big, &r), OE | PE);
    check("x87 fst DBL_MAX*2 chop value", dbits(r), 0x7fefffffffffffffull);
    long double tiny = (long double) DBL_MIN / 3.0L;
    check("x87 fst DBL_MIN/3 flags", x87_store(x87_cw(0, 3), tiny, &r), UE | PE);
    check("x87 fst 1-2^-60 down value", (x87_store(x87_cw(1, 3), 1.0L - 0x1p-60L, &r), dbits(r)),
            0x3fefffffffffffffull);

    // FYL2X of 0 is -inf and a division by zero (musl's i386 log(0.0)).
    unsigned short sw;
    ld_bits lr;
    memset(&lr, 0, sizeof(lr));
    __asm__ volatile("fnclex; fld1; fldz; fyl2x; fnstsw %1; fstpt %0" : "=m"(lr.ld), "=m"(sw));
    check("x87 fyl2x(0) flags", sw & X87_FLAGS, ZE);
    check("x87 fyl2x(0) value", ((uint64_t) lr.p.se << 48) | (lr.p.m >> 16), 0xffff800000000000ull);

    // An x87 store that is a forked child's first write to a page: a page
    // fault to resolve (copy-on-write), not a SIGSEGV.
    static long double shared_ld = 1.0L;
    pid_t pid = fork();
    if (pid == 0) {
        long double v = 2.5L;
        __asm__ volatile("fldt %1; fstpt %0" : "=m"(shared_ld) : "m"(v));
        _exit(shared_ld == 2.5L ? 0 : 1);
    }
    int status = -1;
    waitpid(pid, &status, 0);
    check("x87 store after fork: child status", (uint64_t) status, 0);

    // FNCLEX clears the x87 flags and must leave EFLAGS alone.
    int taken;
    __asm__ volatile("movl $-1, %%eax; testl %%eax, %%eax; fnclex; movl $0, %0; jns 1f; movl $1, %0; 1:"
                     : "=r"(taken) : : "eax", "cc");
    check("fnclex keeps EFLAGS.SF", taken, 1);
}

// --- SSE -----------------------------------------------------------------------
// SSE2 explicitly: a plain -m32 build may default to x87-only code.
#define SSE2 __attribute__((target("sse2")))

SSE2 static unsigned get_mxcsr(void) { unsigned v; __asm__ volatile("stmxcsr %0" : "=m"(v)); return v; }
SSE2 static void set_mxcsr(unsigned v) { __asm__ volatile("ldmxcsr %0" : : "m"(v)); }

// Bit patterns again, loaded by movsd, which does not touch a NaN.
SSE2 static unsigned sse_comisd(uint64_t a, uint64_t b, int quiet) {
    set_mxcsr(0x1f80);
    if (quiet)
        __asm__ volatile("movsd %0, %%xmm0; movsd %1, %%xmm1; ucomisd %%xmm1, %%xmm0"
                         : : "m"(a), "m"(b) : "xmm0", "xmm1", "cc");
    else
        __asm__ volatile("movsd %0, %%xmm0; movsd %1, %%xmm1; comisd %%xmm1, %%xmm0"
                         : : "m"(a), "m"(b) : "xmm0", "xmm1", "cc");
    return get_mxcsr() & X87_FLAGS;
}

SSE2 static long sse_cvtsd2si(double a, unsigned rc, unsigned *flags) {
    long r;
    set_mxcsr(0x1f80 | (rc << 13));
    __asm__ volatile("cvtsd2si %1, %0" : "=r"(r) : "x"(a));
    *flags = get_mxcsr() & X87_FLAGS;
    set_mxcsr(0x1f80);
    return r;
}

SSE2 static double sse_div(double a, double b, unsigned mxcsr, unsigned *flags) {
    set_mxcsr(mxcsr);
    __asm__ volatile("divsd %1, %0" : "+x"(a) : "x"(b));
    *flags = get_mxcsr() & X87_FLAGS;
    set_mxcsr(0x1f80);
    return a;
}

SSE2 static void test_sse(void) {
    const uint64_t q = 0x7ff8000000000000ull, s = 0x7ff4000000000000ull;
    const uint64_t one = 0x3ff0000000000000ull, two = 0x4000000000000000ull;
    check("comisd qnan", sse_comisd(q, one, 0), IE);
    check("ucomisd qnan", sse_comisd(q, one, 1), 0);
    check("ucomisd snan", sse_comisd(s, one, 1), IE);
    check("comisd 1 vs 2", sse_comisd(one, two, 0), 0);

    unsigned f;
    static const long want[4] = {2, 2, 3, 2};   // nearest, down, up, zero
    for (unsigned rc = 0; rc < 4; rc++) {
        char what[48];
        snprintf(what, sizeof(what), "cvtsd2si 2.5 rc=%u", rc);
        check(what, (uint64_t) sse_cvtsd2si(2.5, rc, &f), (uint64_t) want[rc]);
        snprintf(what, sizeof(what), "cvtsd2si 2.5 rc=%u flags", rc);
        check(what, f, PE);
    }
    check("cvtsd2si -2.5 down", (uint64_t) sse_cvtsd2si(-2.5, 1, &f), (uint64_t) -3);
    check("cvtsd2si 1e300 flags", (sse_cvtsd2si(1e300, 0, &f), f), IE);

    double r = sse_div(1.0, 3.0, 0x1f80 | (2 << 13), &f);
    check("divsd 1/3 up", dbits(r), 0x3fd5555555555556ull);
    check("divsd 1/3 up flags", f, PE);
    r = sse_div(1.0, 0.0, 0x1f80, &f);
    check("divsd 1/0 flags", f, ZE);
    // FTZ: a tiny result is flushed to zero, raising UE and PE.
    r = sse_div(DBL_MIN, 3.0, 0x1f80 | 0x8000, &f);
    check("divsd DBL_MIN/3 ftz value", dbits(r), 0);
    check("divsd DBL_MIN/3 ftz flags", f, UE | PE);
    r = sse_div(DBL_MIN, 3.0, 0x1f80, &f);
    check("divsd DBL_MIN/3 flags", f, UE | PE);
    check("divsd DBL_MIN/3 value", dbits(r), 0x0005555555555555ull);

    // MXCSR round-trips, and its flags are sticky until written.
    set_mxcsr(0x1f80 | (3 << 13) | ZE);
    check("mxcsr round trip", get_mxcsr(), 0x1f80 | (3 << 13) | ZE);
    set_mxcsr(0x1f80);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    test_logf("x87:\n");
    test_x87();
    test_logf("SSE:\n");
    test_sse();
    return finish_suite("x86_fp_env");
}
