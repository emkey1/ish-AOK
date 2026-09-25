// fp_env.c -- the floating-point environment: rounding modes and exception
// flags, and their survival across a thread, fork and a signal handler.
//
// Every engine ran its FP arithmetic on the host FPU in its default state, so
// fesetround() was stored and ignored -- 1.0/3.0 came out the same in all four
// modes -- and the sticky flags never rose: 1.0/0.0 left FE_DIVBYZERO clear.
// Linux (and every IEEE machine) answers the tables below; they are the same
// on x86_64, i386, aarch64 and riscv64, because IEEE 754 fixes the correctly
// rounded result of each basic operation in each mode.
//
// Operands are volatile so nothing is folded at compile time, and the test is
// built without -ffast-math or -frounding-math games: the arithmetic below is
// what the hardware (or AOK) does at run time.
//
// Covered:
//   1. every rounding mode: fesetround/fegetround, division (double, float,
//      negative), a sum and a difference that round differently, sqrt,
//      double->float conversion, rint/nearbyint/lrint, and C casts, which
//      truncate whatever the mode
//   2. long double, whose format differs by arch (x87 extended, IEEE quad):
//      only the ordering of the four roundings is checked
//   3. flags: FE_DIVBYZERO, FE_INVALID (0/0, inf-inf, sqrt(-1), an out of
//      range conversion), FE_OVERFLOW, FE_UNDERFLOW, FE_INEXACT, and an exact
//      operation raising nothing; float and long double as well
//   4. the flag API: feraiseexcept, feclearexcept of one flag, fegetexceptflag
//      and fesetexceptflag, fegetenv/fesetenv, FE_DFL_ENV, feholdexcept and
//      feupdateenv
//   5. persistence: a new thread and a forked child start with the creator's
//      mode and flags (C11 7.6p4, and fork copies the whole thread); a signal
//      handler's own changes are undone by sigreturn. What the handler SEES on
//      entry is per-arch Linux behaviour: x86 starts it in the default
//      environment (fpu__clear_user_states), arm64 and riscv64 hand it the
//      interrupted one.
#define _GNU_SOURCE
#include <fenv.h>
#include <float.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <sys/wait.h>
#include "test_common.h"


#define FLAGS (FE_DIVBYZERO | FE_INVALID | FE_OVERFLOW | FE_UNDERFLOW | FE_INEXACT)

static const int modes[4] = {FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO};
static const char *const mode_names[4] = {"nearest", "upward", "downward", "towardzero"};

static uint64_t dbits(double d) { uint64_t u; memcpy(&u, &d, sizeof(u)); return u; }
static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, sizeof(u)); return u; }

static const char *flag_names(int f, char *buf) {
    buf[0] = '\0';
    if (f & FE_INVALID) strcat(buf, "INVALID ");
    if (f & FE_DIVBYZERO) strcat(buf, "DIVBYZERO ");
    if (f & FE_OVERFLOW) strcat(buf, "OVERFLOW ");
    if (f & FE_UNDERFLOW) strcat(buf, "UNDERFLOW ");
    if (f & FE_INEXACT) strcat(buf, "INEXACT ");
    if (buf[0] == '\0') strcat(buf, "none");
    return buf;
}

static void expect_u64(const char *what, const char *mode, uint64_t got, uint64_t want) {
    if (got == want) {
        test_logf("  %-10s %-26s %016" PRIx64 "\n", mode, what, got);
        return;
    }
    printf("FAIL %s %s: got %016" PRIx64 " expected %016" PRIx64 "\n", mode, what, got, want);
    failures_total++;
}

static void expect_flags(const char *what, int got, int want) {
    char g[64], w[64];
    got &= FLAGS;
    if (got == want) {
        test_logf("  %-34s %s\n", what, flag_names(got, g));
        return;
    }
    printf("FAIL flags %s: got %s expected %s\n", what, flag_names(got, g), flag_names(want, w));
    failures_total++;
}

static volatile double one = 1.0, two = 2.0, three = 3.0, zero = 0.0, neg_one = -1.0;
static volatile double tiny = 0x1p-60, big = DBL_MAX, smallest = DBL_MIN, huge_int = 1e300;
static volatile double two_and_half = 2.5, neg_two_and_half = -2.5, three_and_half = 3.5;
static volatile double two_seven = 2.7, neg_two_seven = -2.7;
static volatile float fone = 1.0f, fthree = 3.0f, fzero = 0.0f, fbig = FLT_MAX, ftwo = 2.0f;
static volatile long double lone = 1.0L, lthree = 3.0L, lzero = 0.0L;

// Correctly rounded results, per mode (nearest, upward, downward, towardzero).
static const uint64_t third[4] = {
    0x3fd5555555555555, 0x3fd5555555555556, 0x3fd5555555555555, 0x3fd5555555555555};
static const uint64_t neg_third[4] = {
    0xbfd5555555555555, 0xbfd5555555555555, 0xbfd5555555555556, 0xbfd5555555555555};
static const uint64_t one_plus_tiny[4] = {
    0x3ff0000000000000, 0x3ff0000000000001, 0x3ff0000000000000, 0x3ff0000000000000};
static const uint64_t one_minus_tiny[4] = {
    0x3ff0000000000000, 0x3ff0000000000000, 0x3fefffffffffffff, 0x3fefffffffffffff};
static const uint64_t sqrt_two[4] = {
    0x3ff6a09e667f3bcd, 0x3ff6a09e667f3bcd, 0x3ff6a09e667f3bcc, 0x3ff6a09e667f3bcc};
static const uint32_t fthird[4] = {0x3eaaaaab, 0x3eaaaaab, 0x3eaaaaaa, 0x3eaaaaaa};
static const uint32_t third_to_float[4] = {0x3eaaaaab, 0x3eaaaaab, 0x3eaaaaaa, 0x3eaaaaaa};
static const double rint_2_5[4] = {2.0, 3.0, 2.0, 2.0};
static const double rint_m2_5[4] = {-2.0, -2.0, -3.0, -2.0};
static const double rint_3_5[4] = {4.0, 4.0, 3.0, 3.0};

// Called through pointers: without -frounding-math GCC inlines rint() as
// "round |x|, put the sign back", which assumes round-to-nearest and turns
// upward into downward for a negative argument. The suite builds with plain
// -O2, so the library routine has to be reached explicitly.
static double (*volatile rint_fn)(double) = rint;
static double (*volatile nearbyint_fn)(double) = nearbyint;
static long (*volatile lrint_fn)(double) = lrint;
static float (*volatile rintf_fn)(float) = rintf;

static void check_mode_results(int m) {
    const char *mn = mode_names[m];
    expect_u64("1.0/3.0", mn, dbits(one / three), third[m]);
    expect_u64("-1.0/3.0", mn, dbits(neg_one / three), neg_third[m]);
    expect_u64("1.0+2^-60", mn, dbits(one + tiny), one_plus_tiny[m]);
    expect_u64("1.0-2^-60", mn, dbits(one - tiny), one_minus_tiny[m]);
    expect_u64("sqrt(2.0)", mn, dbits(sqrt(two)), sqrt_two[m]);
    expect_u64("1.0f/3.0f", mn, fbits(fone / fthree), fthird[m]);
    volatile double t = 0x1.5555555555555p-2;   // 1/3 to nearest
    expect_u64("(float) (1/3)", mn, fbits((float) t), third_to_float[m]);
    expect_u64("rint(2.5)", mn, dbits(rint_fn(two_and_half)), dbits(rint_2_5[m]));
    expect_u64("rint(-2.5)", mn, dbits(rint_fn(neg_two_and_half)), dbits(rint_m2_5[m]));
    expect_u64("nearbyint(3.5)", mn, dbits(nearbyint_fn(three_and_half)), dbits(rint_3_5[m]));
    expect_u64("lrint(2.5)", mn, (uint64_t) lrint_fn(two_and_half), (uint64_t) (long) rint_2_5[m]);
    expect_u64("rintf(2.5f)", mn, fbits(rintf_fn((float) two_and_half)), fbits((float) rint_2_5[m]));
    expect_u64("(int) 2.7", mn, (uint64_t) (int64_t) (int) two_seven, 2);
    expect_u64("(int) -2.7", mn, (uint64_t) (int64_t) (int) neg_two_seven, (uint64_t) -2);
}

static void test_rounding(void) {
    long double lres[4];
    for (int m = 0; m < 4; m++) {
        if (fesetround(modes[m]) != 0) {
            printf("FAIL fesetround(%s) refused\n", mode_names[m]);
            failures_total++;
            continue;
        }
        if (fegetround() != modes[m]) {
            printf("FAIL fegetround after fesetround(%s): got %d\n", mode_names[m], fegetround());
            failures_total++;
        }
        check_mode_results(m);
        lres[m] = lone / lthree;
    }
    fesetround(FE_TONEAREST);
    // up > down, toward zero == down for a positive result, and nearest is one
    // of the two -- whatever long double's format.
    if (!(lres[1] > lres[2]) || lres[3] != lres[2] || (lres[0] != lres[1] && lres[0] != lres[2])) {
        printf("FAIL long double 1/3 ordering: nearest=%La up=%La down=%La zero=%La\n",
               lres[0], lres[1], lres[2], lres[3]);
        failures_total++;
    } else {
        test_logf("  long double 1/3 ordered: up %La > down %La\n", lres[1], lres[2]);
    }
}

#define FLAG_CASE(what, expr, want) do {                                        \
        feclearexcept(FE_ALL_EXCEPT);                                           \
        volatile __typeof__(expr) sink_ = (expr);                               \
        (void) sink_;                                                           \
        expect_flags(what, fetestexcept(FE_ALL_EXCEPT), want);                  \
    } while (0)

static void test_flags(void) {
    fesetround(FE_TONEAREST);
    FLAG_CASE("1.0/0.0", one / zero, FE_DIVBYZERO);
    FLAG_CASE("-1.0/0.0", neg_one / zero, FE_DIVBYZERO);
    FLAG_CASE("0.0/0.0", zero / zero, FE_INVALID);
    FLAG_CASE("inf-inf", (one / zero) - (one / zero), FE_DIVBYZERO | FE_INVALID);
    FLAG_CASE("sqrt(-1.0)", sqrt(neg_one), FE_INVALID);
    FLAG_CASE("DBL_MAX*2", big * two, FE_OVERFLOW | FE_INEXACT);
    FLAG_CASE("DBL_MIN/3", smallest / three, FE_UNDERFLOW | FE_INEXACT);
    FLAG_CASE("1.0/3.0", one / three, FE_INEXACT);
    FLAG_CASE("1.0+2.0", one + two, 0);
    FLAG_CASE("(long long) 1e300", (long long) huge_int, FE_INVALID);
    FLAG_CASE("1.0f/0.0f", fone / fzero, FE_DIVBYZERO);
    FLAG_CASE("FLT_MAX*2", fbig * ftwo, FE_OVERFLOW | FE_INEXACT);
    FLAG_CASE("(float) DBL_MAX", (float) big, FE_OVERFLOW | FE_INEXACT);
    FLAG_CASE("1.0L/0.0L", lone / lzero, FE_DIVBYZERO);
    FLAG_CASE("1.0L/3.0L", lone / lthree, FE_INEXACT);

    // sticky: a second operation adds to what the first raised
    feclearexcept(FE_ALL_EXCEPT);
    volatile double s = one / zero;
    s = one / three;
    (void) s;
    expect_flags("sticky DIVBYZERO then INEXACT", fetestexcept(FE_ALL_EXCEPT), FE_DIVBYZERO | FE_INEXACT);
    feclearexcept(FE_DIVBYZERO);
    expect_flags("feclearexcept(DIVBYZERO) only", fetestexcept(FE_ALL_EXCEPT), FE_INEXACT);

    // Whether raising OVERFLOW or UNDERFLOW also raises INEXACT is left to the
    // implementation (C11 7.6.2.3), so only the flag asked for is checked.
    feclearexcept(FE_ALL_EXCEPT);
    feraiseexcept(FE_OVERFLOW);
    expect_flags("feraiseexcept(OVERFLOW)", fetestexcept(FE_OVERFLOW), FE_OVERFLOW);
    feclearexcept(FE_ALL_EXCEPT);
    feraiseexcept(FE_INVALID);
    expect_flags("feraiseexcept(INVALID)", fetestexcept(FE_ALL_EXCEPT), FE_INVALID);
}

static void test_flag_api(void) {
    fexcept_t saved;
    feclearexcept(FE_ALL_EXCEPT);
    feraiseexcept(FE_DIVBYZERO | FE_INVALID);
    fegetexceptflag(&saved, FE_ALL_EXCEPT);
    feclearexcept(FE_ALL_EXCEPT);
    expect_flags("cleared", fetestexcept(FE_ALL_EXCEPT), 0);
    fesetexceptflag(&saved, FE_ALL_EXCEPT);
    expect_flags("fesetexceptflag restores", fetestexcept(FE_ALL_EXCEPT), FE_DIVBYZERO | FE_INVALID);

    fenv_t env;
    fesetround(FE_UPWARD);
    feclearexcept(FE_ALL_EXCEPT);
    feraiseexcept(FE_INVALID);
    fegetenv(&env);
    fesetround(FE_DOWNWARD);
    feclearexcept(FE_ALL_EXCEPT);
    fesetenv(&env);
    expect_flags("fesetenv flags", fetestexcept(FE_ALL_EXCEPT), FE_INVALID);
    expect_u64("fesetenv mode", "", (uint64_t) fegetround(), FE_UPWARD);
    expect_u64("fesetenv 1/3", "", dbits(one / three), third[1]);

    fesetenv(FE_DFL_ENV);
    expect_u64("FE_DFL_ENV mode", "", (uint64_t) fegetround(), FE_TONEAREST);
    expect_flags("FE_DFL_ENV flags", fetestexcept(FE_ALL_EXCEPT), 0);

    fesetround(FE_TOWARDZERO);
    feclearexcept(FE_ALL_EXCEPT);
    feraiseexcept(FE_INVALID);
    feholdexcept(&env);
    expect_flags("feholdexcept clears", fetestexcept(FE_ALL_EXCEPT), 0);
    volatile double q = one / zero;
    (void) q;
    feupdateenv(&env);
    expect_u64("feupdateenv mode", "", (uint64_t) fegetround(), FE_TOWARDZERO);
    expect_flags("feupdateenv merges", fetestexcept(FE_ALL_EXCEPT), FE_INVALID | FE_DIVBYZERO);
    fesetenv(FE_DFL_ENV);
}

// ---- persistence ------------------------------------------------------------

struct seen { int mode; int flags; uint64_t third; };

static void look(struct seen *s) {
    s->mode = fegetround();
    s->flags = fetestexcept(FE_ALL_EXCEPT) & FLAGS;
    s->third = dbits(one / three);
}

static void *thread_look(void *arg) {
    look(arg);
    return NULL;
}

static volatile sig_atomic_t in_handler;
static struct seen handler_saw;
static void handler(int sig) {
    (void) sig;
    handler_saw.mode = fegetround();
    handler_saw.flags = fetestexcept(FE_ALL_EXCEPT) & FLAGS;
    // Change both; sigreturn has to put the interrupted ones back.
    fesetround(FE_DOWNWARD);
    feclearexcept(FE_ALL_EXCEPT);
    feraiseexcept(FE_INVALID);
    in_handler = 1;
}

static void test_persistence(void) {
    struct seen s;
    const int want_flags = FE_DIVBYZERO;

    fesetround(FE_UPWARD);
    feclearexcept(FE_ALL_EXCEPT);
    feraiseexcept(FE_DIVBYZERO);

    pthread_t t;
    memset(&s, 0, sizeof(s));
    if (pthread_create(&t, NULL, thread_look, &s) == 0) {
        pthread_join(t, NULL);
        expect_u64("thread mode", "", (uint64_t) s.mode, FE_UPWARD);
        expect_flags("thread flags", s.flags, want_flags);
        expect_u64("thread 1/3", "", s.third, third[1]);
    } else {
        printf("FAIL pthread_create\n");
        failures_total++;
    }

    int pipefd[2];
    if (pipe(pipefd) == 0) {
        pid_t pid = fork();
        if (pid == 0) {
            look(&s);
            if (write(pipefd[1], &s, sizeof(s)) != sizeof(s))
                _exit(2);
            _exit(0);
        }
        int status;
        memset(&s, 0, sizeof(s));
        ssize_t n = read(pipefd[0], &s, sizeof(s));
        waitpid(pid, &status, 0);
        close(pipefd[0]);
        close(pipefd[1]);
        if (n != sizeof(s)) {
            printf("FAIL fork child reported nothing\n");
            failures_total++;
        } else {
            expect_u64("fork mode", "", (uint64_t) s.mode, FE_UPWARD);
            expect_flags("fork flags", s.flags, want_flags);
            expect_u64("fork 1/3", "", s.third, third[1]);
        }
    }

    // The parent's own state must not have been disturbed by any of that.
    fesetround(FE_UPWARD);
    feclearexcept(FE_ALL_EXCEPT);
    feraiseexcept(FE_DIVBYZERO);
    signal(SIGUSR1, handler);
    in_handler = 0;
    raise(SIGUSR1);
    if (!in_handler) {
        printf("FAIL handler did not run\n");
        failures_total++;
    }
    look(&s);
    expect_u64("after sigreturn mode", "", (uint64_t) s.mode, FE_UPWARD);
    expect_flags("after sigreturn flags", s.flags, want_flags);
    expect_u64("after sigreturn 1/3", "", s.third, third[1]);
#if defined(__i386__) || defined(__x86_64__)
    // Linux x86 enters a handler with the FPU reset to its init state.
    expect_u64("handler entry mode", "", (uint64_t) handler_saw.mode, FE_TONEAREST);
    expect_flags("handler entry flags", handler_saw.flags, 0);
#else
    // arm64 and riscv64 keep FPCR/FPSR (fcsr) as they were.
    expect_u64("handler entry mode", "", (uint64_t) handler_saw.mode, FE_UPWARD);
    expect_flags("handler entry flags", handler_saw.flags, want_flags);
#endif
    fesetenv(FE_DFL_ENV);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));
    test_logf("rounding:\n");
    test_rounding();
    test_logf("flags:\n");
    test_flags();
    test_logf("flag API:\n");
    test_flag_api();
    test_logf("persistence:\n");
    test_persistence();
    return finish_suite("fp_env");
}
