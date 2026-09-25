// x87 semantics that emulation got wrong in ways ordinary arithmetic tests
// miss: control-word precision, the status-word C2 bit, the memory form of
// fnstsw, and -- the nastiest -- FCMOVcc reading stale lazy flags.
//
// Regressions covered:
//
//  * fsincos (D9 FB) was simply absent from the decode table and raised SIGILL,
//    while every one of its neighbours D9 F8..FF was present.
//
//  * fsin/fcos never touched C2. Hardware sets C2 when the operand is too large
//    to range-reduce (|arg| >= 2^63), leaves the stack untouched, and expects
//    the caller to reduce the argument itself; it clears C2 otherwise. We did
//    neither, so fsin(1e30) returned sin() of a huge double -- 0.00933 -- where
//    hardware leaves 1e30 in place.
//
//  * fnstsw to memory (DD /7) was missing from the decode table and raised
//    SIGILL, though fnstsw ax (DF E0) and fnstcw m16 (D9 /7) were both there.
//
//  * Precision control (the control word's PC field) was parsed into cpu->pc
//    and then never read by anything, so every operation ran at the full 64-bit
//    significand no matter what the guest asked for. glibc's i386 sin()/cos()
//    switch to 53-bit and depend on each step rounding there, so their results
//    came out wrong in the low digits and tan() was wrong everywhere.
//
//  * fninit (DB E3) was missing from both decoders while its neighbour fnclex
//    (DB E2) was present, so it raised SIGILL. It resets the control word to
//    0x037f and clears the status word -- which also resets TOP, since TOP is a
//    field of fsw -- and because the control word changes, the live rounding
//    and precision state has to follow it the way fldcw does.
//
//  * FCMOVcc tested cpu->zf/cpu->pf directly instead of the ZF/PF macros. Those
//    flags are lazy: while the producing instruction's result is still in
//    cpu->res the raw fields hold whatever was last materialized. glibc applies
//    sin()'s sign with "and $0x2,%ecx; fchs; fcmove %st(1),%st" -- an fcmove on
//    a ZF set one instruction earlier, hence still deferred -- so we took the
//    wrong arm and returned a negated result: exactly the right magnitude,
//    wrong sign. That is why it looked like an argument-reduction bug.
//
//  * fptan (D9 F2), fdecstp (D9 F6) and fyl2xp1 (D9 F9) were missing from
//    both engines' decode tables and raised SIGILL. 32-bit HotSpot's Math.tan
//    is fptan, so a Java program on the i386 guest died there.
//
// x86 only (i386 and x86_64 guests). Needs no privileges.
#define _GNU_SOURCE
#include <math.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "../test_common.h"

// pushf/pop sized for the guest's word width.
#if defined(__x86_64__)
#define PUSHF_POP "pushfq\n\tpop %0\n\t"
#else
#define PUSHF_POP "pushfl\n\tpop %0\n\t"
#endif

#define ZF_BIT 6
#define PF_BIT 2
#define CF_BIT 0

static sigjmp_buf ill_jmp;
static volatile sig_atomic_t ill_signo;
static void on_ill(int sig) { ill_signo = sig; siglongjmp(ill_jmp, 1); }

static void check(const char *what, long got, long want) {
    if (got == want) {
        test_logf("  ok   %s = %ld\n", what, got);
        return;
    }
    printf("FAIL: %s = %ld (want %ld)\n", what, got, want);
    failures_total++;
}

static void check_d(const char *what, double got, double want) {
    if (fabs(got - want) < 1e-12) {
        test_logf("  ok   %s = %.12f\n", what, got);
        return;
    }
    printf("FAIL: %s = %.12f (want %.12f)\n", what, got, want);
    failures_total++;
}

// ---------------------------------------------------------------- fsincos

static void test_fsincos(void) {
    static const double xs[] = {0.0, 0.5, 1.0, -1.0, 2.0};
    // volatile: a local modified between sigsetjmp and siglongjmp has an
    // indeterminate value after the jump unless it is volatile, and a reset
    // loop counter here turns one SIGILL into an infinite loop.
    for (volatile unsigned i = 0; i < sizeof xs / sizeof xs[0]; i++) {
        double x = xs[i], s = 0, c = 0;
        if (sigsetjmp(ill_jmp, 1) != 0) {
            printf("FAIL: fsincos(%g) raised signal %d\n", x, (int) ill_signo);
            failures_total++;
            continue;
        }
        // fsincos replaces ST(0) with sin and pushes cos, so ST(0) is cos and
        // ST(1) is sin on exit.
        __asm__ volatile("fldl %2\n\tfsincos\n\tfstpl %1\n\tfstpl %0"
                         : "=m"(s), "=m"(c) : "m"(x) : "memory");
        char label[64];
        snprintf(label, sizeof label, "fsincos(%g) sin", x);
        check_d(label, s, sin(x));
        snprintf(label, sizeof label, "fsincos(%g) cos", x);
        check_d(label, c, cos(x));
    }
}

// --------------------------------------------------------------- fsin C2

// Returns the status word after fsin, and stores the resulting ST(0).
static unsigned short fsin_status(double x, double *out) {
    unsigned short sw = 0;
    __asm__ volatile("fldl %2\n\tfsin\n\tfnstsw %1\n\tfstpl %0"
                     : "=m"(*out), "=m"(sw) : "m"(x) : "memory");
    return sw;
}

static void test_fsin_c2(void) {
    double out = 0;
    unsigned short sw;

    // In range: C2 clear, and the operand is actually replaced.
    sw = fsin_status(1.0, &out);
    check("fsin(1.0) C2", (sw >> 10) & 1, 0);
    check_d("fsin(1.0) result", out, sin(1.0));

    // Out of range (|arg| >= 2^63): C2 set and ST(0) left untouched for the
    // caller to reduce itself.
    sw = fsin_status(1e30, &out);
    check("fsin(1e30) C2", (sw >> 10) & 1, 1);
    check_d("fsin(1e30) operand preserved", out, 1e30);

    // And C2 must be cleared again by the next in-range call.
    sw = fsin_status(1.0, &out);
    check("fsin(1.0) C2 cleared again", (sw >> 10) & 1, 0);
}

// ------------------------------------------------------------ fnstsw m16

static void test_fnstsw_mem(void) {
    static const double xs[] = {1.0, -1.0, 0.0, 1e30};
    for (volatile unsigned i = 0; i < sizeof xs / sizeof xs[0]; i++) {
        double x = xs[i];
        unsigned short mem = 0, ax = 0;
        if (sigsetjmp(ill_jmp, 1) != 0) {
            printf("FAIL: fnstsw m16 raised signal %d\n", (int) ill_signo);
            failures_total++;
            return;
        }
        __asm__ volatile("fldl %2\n\tfsin\n\t"
                         "fnstsw %0\n\t"          // memory form (DD /7)
                         "fnstsw %%ax\n\tmovw %%ax,%1\n\t"
                         "fstp %%st(0)"
                         : "=m"(mem), "=m"(ax) : "m"(x) : "eax", "memory");
        char label[64];
        snprintf(label, sizeof label, "fnstsw m16 == fnstsw ax after fsin(%g)", x);
        check(label, mem == ax, 1);
    }
}

// ------------------------------------------------------ precision control

// 1.0 + 2^-60 is distinguishable from 1.0 with a 64-bit significand but rounds
// away to exactly 1.0 with a 53-bit one, so the sum's equality with 1.0 tells
// us directly whether PC was honoured. Returns ZF from comparing 1.0 with the
// sum: 1 means the addition rounded away, i.e. we computed at 53-bit.
static int sum_rounded_to_double(unsigned short cw) {
    double tiny = ldexp(1.0, -60);
    unsigned long flags = 0;
    __asm__ volatile("fldcw %1\n\t"
                     "fld1\n\t"
                     "fldl %2\n\t"
                     "faddp %%st,%%st(1)\n\t"
                     "fld1\n\t"
                     "fcomip %%st(1),%%st\n\t"
                     "fstp %%st(0)\n\t"
                     PUSHF_POP
                     : "=r"(flags) : "m"(cw), "m"(tiny) : "cc", "memory");
    return (int) ((flags >> ZF_BIT) & 1);
}

static void test_precision_control(void) {
    unsigned short saved = 0;
    __asm__ volatile("fnstcw %0" : "=m"(saved) :: "memory");

    unsigned short pc_ext = (unsigned short) ((saved & ~0x0300) | 0x0300); // 64-bit
    unsigned short pc_dbl = (unsigned short) ((saved & ~0x0300) | 0x0200); // 53-bit

    check("PC=extended keeps 1.0+2^-60 distinct", sum_rounded_to_double(pc_ext), 0);
    check("PC=double rounds 1.0+2^-60 to 1.0", sum_rounded_to_double(pc_dbl), 1);

    __asm__ volatile("fldcw %0" :: "m"(saved) : "memory");
}

// -------------------------------------------------------------- fcomi set

static unsigned long fcomi_flags(double st0, double st1, int pop, int unordered) {
    unsigned long flags = 0;
    if (unordered && pop)
        __asm__ volatile("fldl %2\n\tfldl %1\n\tfucomip %%st(1),%%st\n\t" PUSHF_POP "fstp %%st(0)"
                         : "=r"(flags) : "m"(st0), "m"(st1) : "cc", "memory");
    else if (unordered)
        __asm__ volatile("fldl %2\n\tfldl %1\n\tfucomi %%st(1),%%st\n\t" PUSHF_POP
                         "fstp %%st(0)\n\tfstp %%st(0)"
                         : "=r"(flags) : "m"(st0), "m"(st1) : "cc", "memory");
    else if (pop)
        __asm__ volatile("fldl %2\n\tfldl %1\n\tfcomip %%st(1),%%st\n\t" PUSHF_POP "fstp %%st(0)"
                         : "=r"(flags) : "m"(st0), "m"(st1) : "cc", "memory");
    else
        __asm__ volatile("fldl %2\n\tfldl %1\n\tfcomi %%st(1),%%st\n\t" PUSHF_POP
                         "fstp %%st(0)\n\tfstp %%st(0)"
                         : "=r"(flags) : "m"(st0), "m"(st1) : "cc", "memory");
    return flags;
}

static void test_fcomi(void) {
    struct { double a, b; int cf, zf, pf; const char *name; } cases[] = {
        {1.0, 2.0, 1, 0, 0, "less"},
        {2.0, 1.0, 0, 0, 0, "greater"},
        {1.0, 1.0, 0, 1, 0, "equal"},
        {NAN, 1.0, 1, 1, 1, "unordered"},
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        for (int variant = 0; variant < 4; variant++) {
            int pop = variant & 1, unord = (variant >> 1) & 1;
            unsigned long f = fcomi_flags(cases[i].a, cases[i].b, pop, unord);
            char label[96];
            snprintf(label, sizeof label, "f%scomi%s %s CF",
                     unord ? "u" : "", pop ? "p" : "", cases[i].name);
            check(label, (f >> CF_BIT) & 1, cases[i].cf);
            snprintf(label, sizeof label, "f%scomi%s %s ZF",
                     unord ? "u" : "", pop ? "p" : "", cases[i].name);
            check(label, (f >> ZF_BIT) & 1, cases[i].zf);
            snprintf(label, sizeof label, "f%scomi%s %s PF",
                     unord ? "u" : "", pop ? "p" : "", cases[i].name);
            check(label, (f >> PF_BIT) & 1, cases[i].pf);
        }
    }
}

// ---------------------------------------------------------------- FCMOVcc

// Each fcmov is run twice: once with flags set by a cmp (materialized) and once
// by an and (deferred into cpu->res under the lazy flag model). ONLY the
// deferred form distinguishes a correct implementation from one reading the raw
// flag fields, so a version of this test using cmp alone would pass on the
// buggy build.
#define MK_FCMOV(fn, insn)                                                     \
    static double fn(int lazy) {                                               \
        double a = 1.0, b = 2.0, out = 0;                                      \
        if (lazy)                                                              \
            __asm__ volatile("fldl %2\n\tfldl %1\n\t"                          \
                             "movl $2,%%ecx\n\tandl $2,%%ecx\n\t"              \
                             insn "\n\tfstpl %0\n\tfstp %%st(0)"               \
                             : "=m"(out) : "m"(a), "m"(b) : "ecx", "cc", "memory"); \
        else                                                                   \
            __asm__ volatile("fldl %2\n\tfldl %1\n\t"                          \
                             "movl $2,%%ecx\n\tcmpl $2,%%ecx\n\t"              \
                             insn "\n\tfstpl %0\n\tfstp %%st(0)"               \
                             : "=m"(out) : "m"(a), "m"(b) : "ecx", "cc", "memory"); \
        return out;                                                            \
    }
MK_FCMOV(fcmov_e,   "fcmove %%st(1),%%st")
MK_FCMOV(fcmov_ne,  "fcmovne %%st(1),%%st")
MK_FCMOV(fcmov_b,   "fcmovb %%st(1),%%st")
MK_FCMOV(fcmov_nb,  "fcmovnb %%st(1),%%st")
MK_FCMOV(fcmov_be,  "fcmovbe %%st(1),%%st")
MK_FCMOV(fcmov_nbe, "fcmovnbe %%st(1),%%st")
MK_FCMOV(fcmov_u,   "fcmovu %%st(1),%%st")
MK_FCMOV(fcmov_nu,  "fcmovnu %%st(1),%%st")

static void test_fcmov(void) {
    // cmpl $2,%ecx with ecx=2: ZF=1, CF=0, PF=1 (parity of 0 is even).
    check_d("fcmove   flags from cmp",   fcmov_e(0),   2.0);
    check_d("fcmovne  flags from cmp",   fcmov_ne(0),  1.0);
    check_d("fcmovb   flags from cmp",   fcmov_b(0),   1.0);
    check_d("fcmovnb  flags from cmp",   fcmov_nb(0),  2.0);
    check_d("fcmovbe  flags from cmp",   fcmov_be(0),  2.0);
    check_d("fcmovnbe flags from cmp",   fcmov_nbe(0), 1.0);
    check_d("fcmovu   flags from cmp",   fcmov_u(0),   2.0);
    check_d("fcmovnu  flags from cmp",   fcmov_nu(0),  1.0);

    // andl $2,%ecx with ecx=2: result 2 -> ZF=0, CF=0, PF=0. Deferred.
    check_d("fcmove   flags from and (lazy)",   fcmov_e(1),   1.0);
    check_d("fcmovne  flags from and (lazy)",   fcmov_ne(1),  2.0);
    check_d("fcmovb   flags from and (lazy)",   fcmov_b(1),   1.0);
    check_d("fcmovnb  flags from and (lazy)",   fcmov_nb(1),  2.0);
    check_d("fcmovbe  flags from and (lazy)",   fcmov_be(1),  1.0);
    check_d("fcmovnbe flags from and (lazy)",   fcmov_nbe(1), 2.0);
    check_d("fcmovu   flags from and (lazy)",   fcmov_u(1),   1.0);
    check_d("fcmovnu  flags from and (lazy)",   fcmov_nu(1),  2.0);
}

// ------------------------------------------------------------- PE and C1

// PE (status word bit 5) is a sticky exception flag: any operation whose result
// had to be rounded sets it, and only fclex/fldenv clear it. C1 (bit 9) is not
// sticky -- it reports whether the most recent rounding went away from zero.
// Neither was implemented: every operation left both at whatever they were.
//
// Only operations that genuinely round in our implementation report these.
// fsin/fcos/fsincos/f2xm1 go through the host's double-precision libm and are
// then widened exactly, so no rounding happens on our side and they leave PE
// and C1 alone where hardware would set them. Reporting a rounding direction we
// never computed would be worse than reporting none, so that gap stands.
static unsigned short arith_status(double a, double b, int divide) {
    unsigned short sw = 0;
    if (divide)
        __asm__ volatile("fnclex\n\tfldl %1\n\tfldl %2\n\tfdivp %%st,%%st(1)\n\t"
                         "fnstsw %0\n\tfstp %%st(0)"
                         : "=m"(sw) : "m"(a), "m"(b) : "memory");
    else
        __asm__ volatile("fnclex\n\tfldl %1\n\tfldl %2\n\tfaddp %%st,%%st(1)\n\t"
                         "fnstsw %0\n\tfstp %%st(0)"
                         : "=m"(sw) : "m"(a), "m"(b) : "memory");
    return sw;
}

static void test_pe_c1(void) {
    unsigned short sw;

    // 1/3 cannot be represented, so it rounds -- and on this operand rounds up.
    sw = arith_status(3.0, 1.0, 1);
    check("1/3 sets PE", (sw >> 5) & 1, 1);
    check("1/3 sets C1 (rounded up)", (sw >> 9) & 1, 1);

    // Exact results must leave both clear.
    sw = arith_status(2.0, 4.0, 1);
    check("4/2 leaves PE clear", (sw >> 5) & 1, 0);
    check("4/2 leaves C1 clear", (sw >> 9) & 1, 0);
    sw = arith_status(2.0, 1.0, 0);
    check("1+2 leaves PE clear", (sw >> 5) & 1, 0);
    check("1+2 leaves C1 clear", (sw >> 9) & 1, 0);

    // PE is sticky across a later exact operation; C1 is not.
    {
        double one = 1.0, three = 3.0, two = 2.0, four = 4.0;
        unsigned short after = 0;
        __asm__ volatile("fnclex\n\t"
                         "fldl %1\n\tfldl %2\n\tfdivp %%st,%%st(1)\n\tfstp %%st(0)\n\t"
                         "fldl %3\n\tfldl %4\n\tfdivp %%st,%%st(1)\n\tfstp %%st(0)\n\t"
                         "fnstsw %0"
                         : "=m"(after)
                         : "m"(three), "m"(one), "m"(two), "m"(four) : "memory");
        check("PE stays set across a later exact op", (after >> 5) & 1, 1);
        check("C1 is cleared by that later exact op", (after >> 9) & 1, 0);
    }
}

// ---------------------------------------------------------------- fninit

static void test_fninit(void) {
    unsigned short cw = 0, sw = 0;
    // Dirty the FPU first so a no-op would be visible: a different control word
    // and a non-empty stack.
    unsigned short odd = 0x0f7f;
    __asm__ volatile("fldcw %0\n\tfld1\n\tfld1\n\tfld1" :: "m"(odd) : "memory");
    __asm__ volatile("fninit");
    __asm__ volatile("fnstcw %0\n\tfnstsw %1" : "=m"(cw), "=m"(sw) :: "memory");
    check("fninit resets the control word to 0x037f", cw, 0x037f);
    check("fninit clears the status word, TOP included", sw, 0);
    // And the FPU is usable afterwards, i.e. TOP really was reset.
    double out = 0, in = 2.5;
    __asm__ volatile("fldl %1\n\tfstpl %0" : "=m"(out) : "m"(in) : "memory");
    check_d("FPU usable after fninit", out, 2.5);
}

// ----------------------------------------------- fptan, fyl2xp1, fdecstp

static void test_fptan(void) {
    static const double xs[] = {0.0, 0.5, 1.0, -1.0, 1.4};
    for (volatile unsigned i = 0; i < sizeof xs / sizeof xs[0]; i++) {
        double x = xs[i], t = 0, one = 0;
        unsigned short sw = 0;
        if (sigsetjmp(ill_jmp, 1) != 0) {
            printf("FAIL: fptan(%g) raised signal %d\n", x, (int) ill_signo);
            failures_total++;
            continue;
        }
        // fptan replaces ST(0) with its tangent and pushes 1.0.
        __asm__ volatile("fldl %3\n\tfptan\n\tfnstsw %2\n\tfstpl %1\n\tfstpl %0"
                         : "=m"(t), "=m"(one), "=m"(sw) : "m"(x) : "memory");
        char label[64];
        snprintf(label, sizeof label, "fptan(%g)", x);
        check_d(label, t, tan(x));
        snprintf(label, sizeof label, "fptan(%g) pushed 1.0", x);
        check_d(label, one, 1.0);
        snprintf(label, sizeof label, "fptan(%g) C2", x);
        check(label, (sw >> 10) & 1, 0);
    }
    // Out of range: C2 set, the operand left in place, nothing pushed.
    if (sigsetjmp(ill_jmp, 1) == 0) {
        double x = 1e30, out = 0;
        unsigned short sw = 0;
        __asm__ volatile("fldl %2\n\tfptan\n\tfnstsw %1\n\tfstpl %0"
                         : "=m"(out), "=m"(sw) : "m"(x) : "memory");
        check("fptan(1e30) C2", (sw >> 10) & 1, 1);
        check_d("fptan(1e30) operand preserved", out, 1e30);
    } else {
        printf("FAIL: fptan(1e30) raised signal %d\n", (int) ill_signo);
        failures_total++;
    }
}

static void test_fyl2xp1(void) {
    // ST(1) * log2(1 + ST(0)). Checked relative to the answer: at x = 1e-10
    // the absolute error of forming 1 + x first is invisible next to 1e-12,
    // and relative accuracy there is the instruction's reason to exist.
    static const double xs[] = {1e-10, 0.25, -0.25};
    static const double ys[] = {1.0, 3.0};
    for (volatile unsigned i = 0; i < sizeof xs / sizeof xs[0]; i++) {
        for (volatile unsigned j = 0; j < sizeof ys / sizeof ys[0]; j++) {
            double x = xs[i], y = ys[j], out = 0;
            if (sigsetjmp(ill_jmp, 1) != 0) {
                printf("FAIL: fyl2xp1(%g, %g) raised signal %d\n", y, x, (int) ill_signo);
                failures_total++;
                continue;
            }
            __asm__ volatile("fldl %1\n\tfldl %2\n\tfyl2xp1\n\tfstpl %0"
                             : "=m"(out) : "m"(y), "m"(x) : "memory");
            double want = y * log1p(x) / log(2.0);
            double rel = fabs(out - want) / fabs(want);
            char label[80];
            snprintf(label, sizeof label, "fyl2xp1(y=%g, x=%g) = %.17g, relative error <= 1e-12",
                     y, x, out);
            check(label, rel <= 1e-12, 1);
        }
    }
}

static void test_fdecstp(void) {
    // TOP is bits 13:11 of the status word. fdecstp moves it down one, fincstp
    // back up; the registers themselves do not move.
    unsigned short before = 0, after = 0, back = 0;
    if (sigsetjmp(ill_jmp, 1) != 0) {
        printf("FAIL: fdecstp raised signal %d\n", (int) ill_signo);
        failures_total++;
        return;
    }
    __asm__ volatile("fnstsw %0\n\tfdecstp\n\tfnstsw %1\n\tfincstp\n\tfnstsw %2"
                     : "=m"(before), "=m"(after), "=m"(back) : : "memory");
    unsigned top0 = (before >> 11) & 7, top1 = (after >> 11) & 7, top2 = (back >> 11) & 7;
    check("fdecstp moved TOP down one", top1, (top0 + 7) & 7);
    check("fincstp moved it back", top2, top0);
}

// -------------------------------------------------------------- fabs/fchs

static void test_abs_chs(void) {
    static const double xs[] = {0.5, -0.5, 0.0};
    for (unsigned i = 0; i < sizeof xs / sizeof xs[0]; i++) {
        double x = xs[i], o = 0;
        __asm__ volatile("fldl %1\n\tfabs\n\tfstpl %0" : "=m"(o) : "m"(x) : "memory");
        check_d("fabs", o, fabs(x));
        __asm__ volatile("fldl %1\n\tfchs\n\tfstpl %0" : "=m"(o) : "m"(x) : "memory");
        check_d("fchs", o, -x);
    }
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_ill;
    sa.sa_flags = SA_NODEFER;
    sigaction(SIGILL, &sa, NULL);

    test_fsincos();
    test_fsin_c2();
    test_fnstsw_mem();
    test_precision_control();
    test_fcomi();
    test_fcmov();
    test_pe_c1();
    test_fninit();
    test_abs_chs();
    test_fptan();
    test_fyl2xp1();
    test_fdecstp();

    if (failures_total != 0) {
        printf("x87_fpu: FAIL failures=%u\n", failures_total);
        return 1;
    }
    printf("x87_fpu: PASS\n");
    return 0;
}
