// arm64_fp_env.c -- FPCR and FPSR at the instruction level.
//
// The arm64 engine kept FPCR and FPSR as plain cpu_state words that no FP
// instruction looked at: MSR FPCR changed nothing about rounding, FZ or DN,
// FPSR never collected a flag (QC included, though a comment claimed it did),
// and FCMPE was lowered to the quiet FCMP. The host is aarch64 too, so these
// expectations are what an aarch64 core does, checked natively on an Apple M5.
// The one engine-specific choice: AOK delivers no FP traps, so FPCR's
// trap-enable bits read back as zero, as on the many cores without FP
// trapping, and glibc's feenableexcept reports failure rather than silently
// never trapping. (An M5 keeps them; that check is the only one it fails.)
#include <stdint.h>
#include <string.h>
#include "../test_common.h"

#define IOC (1u << 0)
#define DZC (1u << 1)
#define OFC (1u << 2)
#define UFC (1u << 3)
#define IXC (1u << 4)
#define QC  (1u << 27)
#define RMODE_RP (1u << 22)
#define RMODE_RM (2u << 22)
#define FZ  (1u << 24)
#define DN  (1u << 25)
#define IOE (1u << 8)

static void check(const char *what, uint64_t got, uint64_t want) {
    if (got == want) {
        test_logf("  %-36s %#llx\n", what, (unsigned long long) got);
        return;
    }
    printf("FAIL %s: got %#llx expected %#llx\n", what, (unsigned long long) got, (unsigned long long) want);
    failures_total++;
}

static uint64_t get_fpcr(void) { uint64_t v; __asm__ volatile("mrs %0, fpcr" : "=r"(v)); return v; }
static void set_fpcr(uint64_t v) { __asm__ volatile("msr fpcr, %0" : : "r"(v)); }
static uint64_t get_fpsr(void) { uint64_t v; __asm__ volatile("mrs %0, fpsr" : "=r"(v)); return v; }
static void set_fpsr(uint64_t v) { __asm__ volatile("msr fpsr, %0" : : "r"(v)); }
static uint64_t dbits(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static double bdouble(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }

static double add(double a, double b) { double r; __asm__ volatile("fadd %d0, %d1, %d2" : "=w"(r) : "w"(a), "w"(b)); return r; }
static double divide(double a, double b) { double r; __asm__ volatile("fdiv %d0, %d1, %d2" : "=w"(r) : "w"(a), "w"(b)); return r; }

int main(int argc, char **argv) {
    test_init(argc, argv);
    uint64_t saved = get_fpcr();

    set_fpcr(RMODE_RP);
    check("fpcr RP reads back", get_fpcr() & (3u << 22), RMODE_RP);
    check("fadd RP 1+2^-60", dbits(add(1.0, 0x1p-60)), 0x3ff0000000000001ull);
    set_fpcr(RMODE_RM);
    check("fadd RM 1-2^-60", dbits(add(1.0, -0x1p-60)), 0x3fefffffffffffffull);
    double r;
    __asm__ volatile("frinti %d0, %d1" : "=w"(r) : "w"(2.5));
    check("frinti RM 2.5", dbits(r), dbits(2.0));
    set_fpcr(0);
    check("fadd RN 1+2^-60", dbits(add(1.0, 0x1p-60)), 0x3ff0000000000000ull);

    set_fpcr(IOE);
    check("trap enable reads back as 0", get_fpcr() & IOE, 0);
    set_fpcr(0);

    // FZ flushes a tiny result, raising UFC; DN turns every NaN result into
    // the default NaN.
    set_fpsr(0);
    set_fpcr(FZ);
    check("fdiv DBL_MIN/3 under FZ", dbits(divide(0x1p-1022, 3.0)), 0);
    check("  UFC raised", get_fpsr() & UFC, UFC);
    set_fpcr(0);
    double payload = bdouble(0x7ff8000000000123ull);
    check("NaN payload propagates", dbits(add(payload, 1.0)), 0x7ff8000000000123ull);
    set_fpcr(DN);
    check("DN: default NaN", dbits(add(payload, 1.0)), 0x7ff8000000000000ull);
    set_fpcr(0);

    // FPSR: sticky flags, cleared by a write.
    set_fpsr(0);
    (void) divide(1.0, 0.0);
    check("fdiv 1/0 DZC", get_fpsr() & (IOC | DZC | OFC | UFC | IXC), DZC);
    (void) divide(1.0, 3.0);
    check("then 1/3 adds IXC", get_fpsr() & (IOC | DZC | OFC | UFC | IXC), DZC | IXC);
    set_fpsr(0);
    check("msr fpsr clears", get_fpsr() & (IOC | DZC | OFC | UFC | IXC | QC), 0);
    __asm__ volatile("frintx %d0, %d1" : "=w"(r) : "w"(2.5));
    check("frintx 2.5 IXC", get_fpsr() & IXC, IXC);
    set_fpsr(0);
    long l;
    __asm__ volatile("fcvtzs %0, %d1" : "=r"(l) : "w"(1e300));
    check("fcvtzs 1e300 IOC", get_fpsr() & IOC, IOC);

    // QC from a saturating op.
    set_fpsr(0);
    int32_t big = 0x7fffffff, sat;
    // \n\t, not ';': Darwin's assembler reads ';' as a comment, and the same
    // source is checked natively on an Apple host.
    __asm__ volatile("fmov s0, %w1\n\tfmov s1, %w1\n\tsqadd s0, s0, s1\n\tfmov %w0, s0"
                     : "=&r"(sat) : "r"(big) : "v0", "v1");
    check("sqadd saturates", (uint32_t) sat, 0x7fffffff);
    check("  QC raised", get_fpsr() & QC, QC);

    // FCMPE signals on a quiet NaN, FCMP does not.
    double qnan = bdouble(0x7ff8000000000000ull);
    set_fpsr(0);
    __asm__ volatile("fcmp %d0, %d1" : : "w"(qnan), "w"(1.0) : "cc");
    check("fcmp qnan", get_fpsr() & IOC, 0);
    __asm__ volatile("fcmpe %d0, %d1" : : "w"(qnan), "w"(1.0) : "cc");
    check("fcmpe qnan", get_fpsr() & IOC, IOC);
    set_fpsr(0);
    __asm__ volatile("fcmp %d0, #0.0" : : "w"(qnan) : "cc");
    check("fcmp qnan, #0", get_fpsr() & IOC, 0);
    __asm__ volatile("fcmpe %d0, #0.0" : : "w"(qnan) : "cc");
    check("fcmpe qnan, #0", get_fpsr() & IOC, IOC);

    set_fpsr(0);
    set_fpcr(saved);
    return finish_suite("arm64_fp_env");
}
