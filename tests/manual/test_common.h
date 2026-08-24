#ifndef ISH_TESTS_COMMON_H
#define ISH_TESTS_COMMON_H

#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned failures_total;
static int test_verbose;

// True when this test is running inside a chroot set up by
// /AOK/tools/mount-root.sh, which is how the release procedure's
// per-architecture run reaches a root that is not the booted one.
//
// That script bind-mounts /proc, /sys, /dev, /run, /AOK/tools and /AOK/tests
// from the BOOTED root into the target root. So /proc describes the booted
// root's mount table, not the filesystem you are standing in, and any test
// that cross-checks procfs against the live filesystem is comparing two
// different roots. It cannot pass however correct the kernel is: mount_stdev
// sees `/` as 0:15 in mountinfo and 0:29 from stat(), and reports five
// failures that mean nothing.
//
// Detected by what the bind list leaves out rather than by what it includes.
// /AOK is the booted root's aokfs, so /AOK/VERSION exists only there, while
// /AOK/tests is bound in -- it is how this test got here at all. Deliberately
// NOT detected by comparing procfs against stat(), because that is exactly
// what these tests assert, and a skip condition that overlaps the assertion
// would hide the bug it exists to catch.
static inline int test_in_foreign_proc_chroot(void) {
    return access("/AOK/tests", F_OK) == 0 && access("/AOK/VERSION", F_OK) != 0;
}

// Print the standard SKIP line for the above and return from main with 0.
#define TEST_SKIP_IF_FOREIGN_PROC(name)                                        \
    do {                                                                       \
        if (test_in_foreign_proc_chroot()) {                                   \
            printf("%s: SKIP (chroot with a bind-mounted /proc -- mountinfo "  \
                   "describes the booted root, not this one)\n", (name));      \
            return 0;                                                          \
        }                                                                      \
    } while (0)

static void test_init(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            test_verbose = 1;
            continue;
        }
        fprintf(stderr, "unknown option: %s\n", argv[i]);
        exit(2);
    }
}

// Watchdog duration for a test's alarm() safety net. Returns `base` seconds,
// scaled by ISH_TEST_WATCHDOG_SCALE (an integer env multiplier, default 1) so
// a known-heavy run -- e.g. the release procedure's 4-way concurrent
// multi-arch regression, which compiles AND runs all suites on one shared
// host and can stretch a normally-millisecond handshake by orders of
// magnitude -- can widen every watchdog without editing sources. The alarm is
// only a backstop against a genuine hang, so err generous: a correct run
// finishes far under it regardless of the scale.
static unsigned test_watchdog_secs(unsigned base) {
    const char *s = getenv("ISH_TEST_WATCHDOG_SCALE");
    long scale = s != NULL ? strtol(s, NULL, 10) : 1;
    if (scale < 1)
        scale = 1;
    return (unsigned) (base * (unsigned long) scale);
}

static void test_logf(const char *fmt, ...) {
    if (!test_verbose)
        return;
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

static void test_log_if(int cond, const char *fmt, ...) {
    if (!cond && !test_verbose)
        return;
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

static void failf(const char *label, uint64_t got0, uint64_t got1, uint64_t got2,
                  uint64_t exp0, uint64_t exp1, uint64_t exp2) {
    printf("FAIL %s got=%016" PRIx64 " %016" PRIx64 " %016" PRIx64
           " expected=%016" PRIx64 " %016" PRIx64 " %016" PRIx64 "\n",
           label, got0, got1, got2, exp0, exp1, exp2);
    failures_total++;
}

static int finish_suite(const char *suite_name) {
    if (failures_total != 0) {
        printf("%s: FAIL failures=%u\n", suite_name, failures_total);
        return 1;
    }
    printf("%s: PASS\n", suite_name);
    return 0;
}

#endif
