/*
 * diff_common.h — shared harness for AOK-iSH differential JIT tests.
 *
 * A differential test does NOT bake in expected answers. It executes an x86
 * instruction (via inline asm) on controlled operands, captures the result and
 * the architecturally-DEFINED EFLAGS bits, and prints one canonical line per
 * case. The conductor runs the same binary across every (arch x engine) cell
 * plus a native x86 oracle (Rosetta) and requires byte-identical lines for
 * matching keys. The oracle is the ground truth; the test stays "dumb".
 *
 * The same source compiles three ways from one file:
 *   zig cc -target x86-linux-musl     -static   (iSH i386 guest)
 *   zig cc -target x86_64-linux-musl  -static   (iSH amd64 guest)
 *   cc -arch x86_64  (macOS Mach-O, run under `arch -x86_64` = oracle)
 *
 * Width handling: 64-bit cases are compiled only for __x86_64__. Comparison is
 * key-based, so an i386 cell legitimately omits w64 lines.
 */
#ifndef ISH_DIFF_COMMON_H
#define ISH_DIFF_COMMON_H

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

/* EFLAGS status bits */
#define FL_CF 0x001UL
#define FL_PF 0x004UL
#define FL_AF 0x010UL
#define FL_ZF 0x040UL
#define FL_SF 0x080UL
#define FL_OF 0x800UL
#define FL_ALL (FL_CF | FL_PF | FL_AF | FL_ZF | FL_SF | FL_OF)

#if defined(__x86_64__)
#  define PUSHF "pushfq"
#  define POPF  "popfq"
#  define HAVE_W64 1
#else
#  define PUSHF "pushfl"
#  define POPF  "popfl"
#  define HAVE_W64 0
#endif

/* ---- engine self-selection (guest only; silently no-ops on macOS oracle) ---- */

static void dc_write_proc(const char *path, const char *val) {
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return; /* not a guest, or knob absent — fine */
    (void)!write(fd, val, strlen(val));
    close(fd);
}

static void dc_self_comm(char *out, size_t n) {
    out[0] = '\0';
    int fd = open("/proc/self/comm", O_RDONLY);
    if (fd < 0)
        return;
    ssize_t r = read(fd, out, n - 1);
    if (r < 0)
        r = 0;
    out[r] = '\0';
    for (char *p = out; *p; p++)
        if (*p == '\n') { *p = '\0'; break; }
}

/* engine spec: "amd64:jit" "amd64:interp" "i386:jit" "i386:no_cache"
 * "i386:single_step" "oracle"/"native" (no-op). Applied before any case runs,
 * so the op-under-test's JIT block is compiled under the chosen engine. */
static void dc_apply_engine(const char *spec) {
    if (!spec)
        return;
    if (strcmp(spec, "amd64:jit") == 0)
        dc_write_proc("/proc/ish/amd64_jit", "1");
    else if (strcmp(spec, "amd64:interp") == 0)
        dc_write_proc("/proc/ish/amd64_jit", "0");
    else if (strcmp(spec, "i386:no_cache") == 0) {
        char comm[32]; dc_self_comm(comm, sizeof comm);
        dc_write_proc("/proc/ish/i386_no_cache_comm", comm);
    } else if (strcmp(spec, "i386:single_step") == 0) {
        char comm[32]; dc_self_comm(comm, sizeof comm);
        dc_write_proc("/proc/ish/i386_single_step_comm", comm);
    }
    /* "i386:jit", "oracle", "native", unknown -> default engine, no knob */
}

/* ---- deterministic PRNG (splitmix64): identical on every build ---- */
static uint64_t dc_rng_state;
static uint64_t dc_rng(void) {
    uint64_t z = (dc_rng_state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

/* ---- harness state ---- */
static struct {
    const char *engine;
    long only_case;   /* -1 = all */
    int list_only;
    int verbose;
    long counter;     /* emitted-case index */
    long emitted;
    uint64_t seed;    /* base seed; tests reseed per-width for cross-arch stability */
} dc;

static void dc_emit(const char *op, int width,
                    uint64_t a, uint64_t b, uint64_t res,
                    unsigned long flags, unsigned long defined) {
    long idx = dc.counter++;
    if (dc.list_only)
        return;
    if (dc.only_case >= 0 && idx != dc.only_case)
        return;
    int nib = width / 4;
    /* mask operands/result to width for stable, arch-independent text */
    uint64_t wmask = (width >= 64) ? ~0ULL : ((1ULL << width) - 1);
    printf("%-4s w%-2d a=%0*llx b=%0*llx res=%0*llx fl=%04lx\n",
           op, width,
           nib, (unsigned long long)(a & wmask),
           nib, (unsigned long long)(b & wmask),
           nib, (unsigned long long)(res & wmask),
           flags & defined);
    dc.emitted++;
}

/* Provided by each test: enumerate and dc_emit() every case. */
static void run_cases(void);

static void dc_usage(const char *argv0) {
    fprintf(stderr,
        "usage: %s [--engine SPEC] [--case N] [--seed S] [--list] [-v]\n"
        "  --engine  amd64:{jit,interp} | i386:{jit,no_cache,single_step} | oracle\n"
        "  --case N  emit only case N (for minimization); default: all\n"
        "  --seed S  PRNG seed for randomized operands (default 1)\n"
        "  --list    print case count only\n", argv0);
}

static int dc_main(int argc, char **argv) {
    dc.only_case = -1;
    dc_rng_state = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--engine") && i + 1 < argc)
            dc.engine = argv[++i];
        else if (!strcmp(argv[i], "--case") && i + 1 < argc)
            dc.only_case = strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc)
            dc_rng_state = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--list"))
            dc.list_only = 1;
        else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose"))
            dc.verbose = 1;
        else { dc_usage(argv[0]); return 2; }
    }
    dc_apply_engine(dc.engine);
    run_cases();
    if (dc.list_only)
        printf("cases=%ld\n", dc.counter);
    fflush(stdout);
    return 0;
}

int main(int argc, char **argv) { return dc_main(argc, argv); }

/* ---- ALU op macros: execute op, capture width-correct result + EFLAGS ----
 * Typed operands let the compiler pick size-implying register names, so the
 * mnemonic needs no suffix. PUSHF/pop captures flags immediately after. */

#define DC_ALU2(MNEM, CT, AIN, BIN, RESOUT, FLOUT) do {                  \
    CT r_ = (CT)(AIN); unsigned long f_;                                 \
    __asm__ volatile(MNEM " %[b], %[r]\n\t" PUSHF "\n\tpop %[f]"          \
        : [r] "+r"(r_), [f] "=r"(f_) : [b] "r"((CT)(BIN)) : "cc");        \
    (RESOUT) = (uint64_t)r_; (FLOUT) = f_;                               \
} while (0)

/* cmp/test: flags only, result is the (unchanged) destination */
#define DC_ALU2_CMP(MNEM, CT, AIN, BIN, FLOUT) do {                      \
    CT r_ = (CT)(AIN); unsigned long f_;                                 \
    __asm__ volatile(MNEM " %[b], %[r]\n\t" PUSHF "\n\tpop %[f]"          \
        : [r] "+r"(r_), [f] "=r"(f_) : [b] "r"((CT)(BIN)) : "cc");        \
    (FLOUT) = f_;                                                        \
} while (0)

/* one-operand ops: inc/dec/neg/not */
#define DC_ALU1(MNEM, CT, AIN, RESOUT, FLOUT) do {                       \
    CT r_ = (CT)(AIN); unsigned long f_;                                 \
    __asm__ volatile(MNEM " %[r]\n\t" PUSHF "\n\tpop %[f]"                \
        : [r] "+r"(r_), [f] "=r"(f_) : : "cc");                          \
    (RESOUT) = (uint64_t)r_; (FLOUT) = f_;                               \
} while (0)

/* adc/sbb with caller-chosen incoming CF (sets CF via add of 0xff..+1) */
#define DC_ALU2_CF(MNEM, CT, AIN, BIN, CFIN, RESOUT, FLOUT) do {         \
    CT r_ = (CT)(AIN); unsigned long f_; unsigned char cf_ = (CFIN);     \
    __asm__ volatile(                                                    \
        "add $0xff, %b[cf]\n\t"   /* cf_=1 -> CF=1 ; cf_=0 -> CF=0 */     \
        MNEM " %[b], %[r]\n\t" PUSHF "\n\tpop %[f]"                       \
        : [r] "+r"(r_), [f] "=r"(f_), [cf] "+q"(cf_)                      \
        : [b] "r"((CT)(BIN)) : "cc");                                    \
    (RESOUT) = (uint64_t)r_; (FLOUT) = f_;                               \
} while (0)

#endif /* ISH_DIFF_COMMON_H */
