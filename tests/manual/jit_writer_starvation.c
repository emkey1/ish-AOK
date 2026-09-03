// A compute-bound thread must not stall its siblings for seconds.
//
// The JIT frees invalidated blocks under `jetsam_lock`, which every thread
// holds for read while inside jit_enter. A thread that wants to free raises
// `write_wanted` so running engines stand aside at their next block boundary,
// then polls pthread_rwlock_trywrlock for up to five seconds.
//
// Every thread runs that cleanup after every return from the JIT, so two
// siblings of one process want the write lock constantly -- one mmap is enough
// to put blocks on the jetsam list that both of them then notice. While
// `write_wanted` was a FLAG rather than a count, the first of the two to finish
// stored zero while the second was still waiting; with nothing left asking
// readers to stand aside, a sibling burning CPU in guest code re-took the read
// lock between every one of the waiter's five-millisecond polls, and the waiter
// ran its whole five-second timeout out having freed nothing.
//
// pthread_create is the cheapest way to ask for that work, since it maps a
// stack. Measured on the unfixed build against a glibc root: single creates of
// 6057 ms and, under this test's pressure, 6861 ms -- against about 1 ms once
// the count is a count. The failure is timing, not a wrong answer, so the
// threshold sits far from both: three seconds is half the defect and three
// thousand times the healthy case.
//
// This is the mechanism behind exec_de_thread's native case, which failed about
// two runs in three on a glibc root because its ten-second budget for the
// exec'd program to appear straddled the timeout. That test detects this bug
// only by coincidence of budget; this one measures it.
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

#define SPINNERS      1
#define CREATORS      2
#define PER_CREATOR  10

static volatile int stop_all;
static _Atomic long worst_ms;

static long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

// Busy in guest code with no syscalls to be interrupted at, which is what makes
// it hold the JIT read lock across long stretches.
static void *spinning_thread(void *arg) {
    (void) arg;
    volatile double a = 1.0;
    for (long i = 0; i < 2000000000L && !stop_all; i++)
        a = a * 1.0000001 + 0.5;
    return NULL;
}

static void *tiny_thread(void *arg) { (void) arg; return NULL; }

static void note(long dt) {
    long cur = worst_ms;
    while (dt > cur && !__atomic_compare_exchange_n(&worst_ms, &cur, dt, 0,
                __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
        continue;
}

static void *creator_thread(void *arg) {
    (void) arg;
    for (int i = 0; i < PER_CREATOR && !stop_all; i++) {
        pthread_t t;
        long t0 = now_ms();
        if (pthread_create(&t, NULL, tiny_thread, NULL) != 0)
            continue;
        note(now_ms() - t0);
        pthread_join(t, NULL);
    }
    return NULL;
}

// The same creates with nothing spinning, so a merely slow machine is
// distinguishable from a starved one: this number stays small either way.
static long baseline_worst(void) {
    long worst = 0;
    for (int i = 0; i < PER_CREATOR; i++) {
        pthread_t t;
        long t0 = now_ms();
        if (pthread_create(&t, NULL, tiny_thread, NULL) != 0)
            continue;
        long dt = now_ms() - t0;
        if (dt > worst)
            worst = dt;
        pthread_join(t, NULL);
    }
    return worst;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(300));

    long quiet = baseline_worst();
    test_logf("  %-46s worst=%ldms\n", "creates with nothing spinning", quiet);

    pthread_t spin[SPINNERS], make[CREATORS];
    for (int i = 0; i < SPINNERS; i++)
        pthread_create(&spin[i], NULL, spinning_thread, NULL);
    // Let the spinners get into translated code before asking for the lock.
    struct timespec settle = { 0, 100000000L };
    nanosleep(&settle, NULL);

    long t0 = now_ms();
    for (int i = 0; i < CREATORS; i++)
        pthread_create(&make[i], NULL, creator_thread, NULL);
    for (int i = 0; i < CREATORS; i++)
        pthread_join(make[i], NULL);
    long wall = now_ms() - t0;

    stop_all = 1;
    for (int i = 0; i < SPINNERS; i++)
        pthread_join(spin[i], NULL);

    long worst = worst_ms;
    // Scaled like the watchdog, so the release procedure's concurrent multi-arch
    // run can widen it without editing this file.
    long limit = (long) test_watchdog_secs(3) * 1000;
    test_logf("  %-46s worst=%ldms wall=%ldms limit=%ldms\n",
              "creates beside a compute-bound sibling", worst, wall, limit);
    if (worst >= limit)
        failf("pthread_create starved by a spinning sibling",
              (uint64_t) worst, 0, 0, (uint64_t) limit, 0, 0);

    return finish_suite("jit_writer_starvation");
}
