// The jetsam headroom guard must never refuse a SMALL growth.
//
// kernel/mmap.c refuses guest address-space growth when the app is close to its
// iOS memory limit, so a runaway guest gets clean ENOMEMs instead of the app
// being jetsammed. That guard used to ignore how much was being asked for, and
// the difference between those two answers is the difference between a
// constrained guest and a dead one.
//
// MEASURED on a device, 2026-09-07, after `stress --vm-bytes 5.5G`:
//
//     WARNING: 744(stress) mmap refused, low iOS memory headroom (len=0x140001000)
//     WARNING: 745(zsh) mmap refused, low iOS memory headroom (len=0x100000)
//
// The first is the guard working. The second is a shell denied a MEGABYTE nine
// seconds later, with stress already dead and 1228 MB of the app's own headroom
// free -- and it is fatal, because a guest that cannot get a megabyte cannot
// start a process, cannot exec, and cannot run the command that would fix it.
// Every command after it failed; the guest was bricked until the app restarted.
//
// Refusing a small request buys almost nothing: anonymous growth here is a lazy
// reservation that costs the app nothing until the pages are touched, and
// touching them is policed separately by mem_fault_backpressure, which throttles
// and ultimately OOM-kills on evidence of sustained growth. See
// mem_growth_refused in kernel/mmap.c.
//
// The guard is off unless the build is told what ceiling to defend, so this test
// SKIPS rather than passing vacuously. To run it for real:
//
//     ISH_GUEST_MEM_BUDGET_MB=512 ISH_GUEST_MEM_HEADROOM_MB=256 \
//         ./build/ish -f <root> /AOK/tests/mem_guard_small_growth
//
// with a hog holding a few hundred MB, or just let this test's own allocation
// below push the footprint over the floor, which on those numbers it does.
#define _GNU_SOURCE
#include <errno.h>
#include <sys/mman.h>

#include "test_common.h"

// Comfortably under any exemption the guard computes, and the size a shell
// actually asks for -- the device log above shows zsh refused at exactly this.
#define SMALL_GROWTH (1024 * 1024)
// Comfortably over it, and over any headroom a phone has. This one SHOULD be
// refused: a test that only proved small allocations succeed would also pass
// against a guard that had been removed entirely.
#define LARGE_GROWTH (3ull * 1024 * 1024 * 1024)
// Touched to push the app's footprint past the floor so the guard engages.
#define HOG_MB 400

static int guard_is_engaged(void) {
    FILE *f = fopen("/proc/ish/mem_guard", "r");
    if (f == NULL)
        return 0;
    char line[256];
    int engaged = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strstr(line, "growth refused now") != NULL && strstr(line, "YES") != NULL)
            engaged = 1;
    }
    fclose(f);
    return engaged;
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "-v") == 0)
        test_verbose = 1;

    // Take the footprint up so the guard has something to refuse. Freed before
    // the assertions would be wrong: the guard has to be ENGAGED while they run,
    // which is the whole point -- the first attempt at this test let the hog
    // exit first, and then it passed against the broken code too.
    size_t n = (size_t) HOG_MB * 1024 * 1024;
    char *hog = malloc(n);
    if (hog == NULL) {
        printf("mem_guard_small_growth: SKIP (could not allocate %d MB to engage the guard)\n",
               HOG_MB);
        return 0;
    }
    for (size_t i = 0; i < n; i += 4096)
        hog[i] = 1;

    if (!guard_is_engaged()) {
        printf("mem_guard_small_growth: SKIP (guard not engaged; needs "
               "ISH_GUEST_MEM_BUDGET_MB and ISH_GUEST_MEM_HEADROOM_MB -- see the header)\n");
        free(hog);
        return 0;
    }

    errno = 0;
    void *small = mmap(NULL, SMALL_GROWTH, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (small == MAP_FAILED) {
        printf("FAIL: a %d MiB mapping was refused while the guard was engaged (%s); "
               "a guest that cannot get this cannot start a process\n",
               SMALL_GROWTH / (1024 * 1024), strerror(errno));
        failures_total++;
    } else {
        // Touch it: a reservation that faults on first use would be the same
        // failure wearing a success.
        memset(small, 1, SMALL_GROWTH);
        if (test_verbose)
            printf("small growth allowed and writable while the guard was engaged\n");
        munmap(small, SMALL_GROWTH);
    }

    errno = 0;
    void *large = mmap(NULL, LARGE_GROWTH, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (large != MAP_FAILED) {
        printf("FAIL: a %llu GiB mapping was allowed while the guard was engaged; "
               "the guard is not refusing anything\n",
               (unsigned long long) (LARGE_GROWTH / (1024 * 1024 * 1024)));
        failures_total++;
        munmap(large, LARGE_GROWTH);
    } else if (test_verbose) {
        printf("large growth refused, as it must be (%s)\n", strerror(errno));
    }

    free(hog);
    return finish_suite("mem_guard_small_growth");
}
