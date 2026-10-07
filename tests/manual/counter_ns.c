// counter_ns.c -- the guest's cycle counter: x86 RDTSC (i386: EDX:EAX; amd64:
// RAX/RDX, their upper halves zero), arm64 MRS CNTVCT_EL0 with CNTFRQ_EL0, or
// riscv64 rdtime (cycle and instret read the same counter under AOK).
// AOK makes it the host's system counter in nanoseconds (gadgets-generic.h
// host_counter_ns), so: read in order (LFENCE; RDTSC, ISB; MRS, as Linux's
// ordered reads are -- without the fence real hardware fails the next check
// too) it never goes backwards, on one thread or across four that hand the
// latest value round; and it runs at CLOCK_MONOTONIC's rate, within 2% over
// 200 ms -- arm64 at CNTFRQ_EL0's (1 GHz under AOK). A real x86's TSC rate and
// a real RISC-V's timebase are unknown here, so there the rate check runs only
// under AOK (/proc/ish).
#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

static uint64_t counter(void) {
#if defined(__x86_64__)
    uint64_t a = ~0ull, d = ~0ull;
    __asm__ volatile("mov $-1, %%rax\n mov $-1, %%rdx\n lfence\n rdtsc" : "=a"(a), "=d"(d) :: "memory");
    if ((a >> 32) || (d >> 32))
        return 0;                      // the upper halves must be zero
    return a | d << 32;
#elif defined(__i386__)
    uint32_t a, d;
    __asm__ volatile("lfence\n rdtsc" : "=a"(a), "=d"(d) :: "memory");
    return a | (uint64_t) d << 32;
#elif defined(__aarch64__)
    uint64_t v;
    __asm__ volatile("isb\n mrs %0, cntvct_el0" : "=r"(v) :: "memory");
    return v;
#elif defined(__riscv)
    uint64_t v, c, i;
    __asm__ volatile("fence\n rdtime %0\n rdcycle %1\n rdinstret %2" : "=r"(v), "=r"(c), "=r"(i) :: "memory");
    if (access("/proc/ish", F_OK) == 0 && (c < v || i < c))
        return 0;                      // under AOK all three are the one counter, read in order
    return v;
#else
    return 0;
#endif
}
static uint64_t freq(void) {
#if defined(__aarch64__)
    uint64_t f;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return f;
#else
    return 1000000000ull;
#endif
}
static uint64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000000ull + (uint64_t) ts.tv_nsec;
}

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static volatile uint64_t baton;
static volatile int stop;
static void *passer(void *arg) {
    int *bad = arg;
    while (!stop) {
        uint64_t seen = baton, now = counter();
        if (now < seen)
            (*bad)++;
        baton = now;
    }
    return NULL;
}

int main(void) {
#if !defined(__x86_64__) && !defined(__i386__) && !defined(__aarch64__) && !defined(__riscv)
    printf("counter_ns: SKIP (x86, arm64 and riscv64 only)\n");
    return 0;
#endif
    // one thread: never backwards, and never stuck for a million reads
    uint64_t prev = counter(), first = prev;
    int back = 0;
    for (int i = 0; i < 1000000; i++) {
        uint64_t v = counter();
        if (v < prev)
            back++;
        prev = v;
    }
    CHECK(back == 0, "went backwards %d times on one thread", back);
    CHECK(prev > first, "did not advance over a million reads");
    CHECK(first != 0, "zero (or, amd64, an upper half set)");

    // four threads passing the latest value round
    pthread_t t[4];
    int bad[4] = {0};
    for (int i = 0; i < 4; i++)
        pthread_create(&t[i], NULL, passer, &bad[i]);
    usleep(200000);
    stop = 1;
    for (int i = 0; i < 4; i++) {
        pthread_join(t[i], NULL);
        CHECK(bad[i] == 0, "thread %d saw it go backwards %d times", i, bad[i]);
    }

    int aok = access("/proc/ish", F_OK) == 0;
#if defined(__aarch64__)
    int rate_known = 1;
    if (aok)
        CHECK(freq() == 1000000000ull, "CNTFRQ %llu, want 1 GHz", (unsigned long long) freq());
#else
    int rate_known = aok;
#endif
    if (rate_known) {
        uint64_t c0 = counter(), m0 = mono_ns();
        usleep(200000);
        uint64_t c1 = counter(), m1 = mono_ns();
        double rate = (double) (c1 - c0) * 1e9 / (double) freq() / (double) (m1 - m0);
        CHECK(rate > 0.98 && rate < 1.02, "ran at %.4f x CLOCK_MONOTONIC's rate", rate);
    }
    printf("counter_ns: %s\n", fails ? "FAIL" : "PASS");
    return fails != 0;
}
