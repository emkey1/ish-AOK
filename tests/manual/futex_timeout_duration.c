// A futex timeout has to last as long as it was asked to last.
//
// futex_core.c already covers FUTEX_WAIT with a timeout, but it asserts the
// RETURN -- rc == -1, errno == ETIMEDOUT -- and never the elapsed time. Both
// are true of a wait that returns instantly, so a 64-bit guest went years
// answering ETIMEDOUT the moment it was called and the suite stayed green.
//
// What was wrong: every 64-bit ABI dispatched futex(2) through the parser for
// the 32-bit struct timespec. A 64-bit guest's timespec is two 64-bit fields,
// so tv_sec came from the low half of the real tv_sec and tv_nsec came from its
// HIGH half -- zero for any timestamp that fits in 32 bits. The nanoseconds
// were dropped whole:
//
//   relative {0s, 500000000ns}   -> read as {0s, 0ns}: no wait at all
//   relative {2s, 0ns}           -> read as {2s, 0ns}: correct, by luck
//   absolute deadline            -> truncated back to a whole second, which
//                                   is in the past nearly always
//
// glibc builds pthread_cond_timedwait, sem_timedwait and pthread_mutex_timedlock
// on FUTEX_WAIT_BITSET, so all three stopped waiting and their callers spun.
// MariaDB's statement_timer thread held ~80% of a core on an idle server,
// making ~3400 futex calls a second.
//
// Hence: assert DURATION, and use a sub-second one. A test that only ever asks
// for whole seconds passes on the broken kernel.
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <linux/futex.h>
#include <sys/syscall.h>

#include "test_common.h"

#define WANT_MS   500
// Generous both ways: the floor only has to be above "returned immediately",
// and the ceiling only has to catch a wait that never ends. An emulated guest
// on a loaded host oversleeps freely, so the ceiling is not a timing assertion.
#define FLOOR_MS  400
#define CEIL_MS   10000

static double elapsed_ms(struct timespec a, struct timespec b) {
    return (double) (b.tv_sec - a.tv_sec) * 1000.0 +
           (double) (b.tv_nsec - a.tv_nsec) / 1000000.0;
}

static void add_ms(struct timespec *ts, long ms) {
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_nsec -= 1000000000L;
        ts->tv_sec++;
    }
}

static void ck_slept(const char *what, double ms) {
    int ok = ms >= FLOOR_MS && ms <= CEIL_MS;
    if (!ok)
        failf(what, (uint64_t) ms, (uint64_t) FLOOR_MS, (uint64_t) CEIL_MS, 0, 0, 0);
    test_logf("  %-52s %s (%.0fms)\n", what, ok ? "ok" : "FAIL", ms);
}

static int futex_word;

// Which futex syscall can read the libc timespec this build has.
//
// A 32-bit guest whose libc carries a 64-bit time_t (musl 1.2+, glibc with
// _TIME_BITS=64) has a SIXTEEN-byte struct timespec, and the legacy SYS_futex
// cannot read it: that call takes the kernel's {long sec; long nsec}, eight
// bytes on a 32-bit ABI. Hand it the wider struct and the low half of tv_sec
// lands in sec while the HIGH half lands in nsec -- so a 500ms timeout arrives
// as {0,0} and returns at once, while a whole-second one survives intact and
// looks fine. That is a property of the CALLER, not of the kernel: real Linux
// truncates the identical call the identical way, and iSH-AOK's timespec_ is
// the same eight bytes Linux uses. The libc wrappers below never showed it
// because they already pick the right call.
//
// So pick the same way libc does, and keep testing what the kernel does with a
// timeout rather than what this test does with a struct.
static long futex_call(int *word, int op, int val, const struct timespec *t, unsigned bitset) {
#ifdef SYS_futex_time64
    if (sizeof t->tv_sec > sizeof(long))
        return syscall(SYS_futex_time64, word, op, val, t, NULL, bitset);
#endif
    return syscall(SYS_futex, word, op, val, t, NULL, bitset);
}

// Nobody ever wakes this word, so every wait below runs to its timeout.
static double futex_wait_rel(long ms) {
    struct timespec t0, t1, rel = {0, 0};
    add_ms(&rel, ms);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    futex_call(&futex_word, FUTEX_WAIT | FUTEX_PRIVATE_FLAG, 0, &rel, 0);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    return elapsed_ms(t0, t1);
}

static double futex_wait_abs(clockid_t clock, long ms) {
    struct timespec t0, t1, deadline;
    int op = FUTEX_WAIT_BITSET | FUTEX_PRIVATE_FLAG;
    if (clock == CLOCK_REALTIME)
        op |= FUTEX_CLOCK_REALTIME;
    clock_gettime(clock, &deadline);
    add_ms(&deadline, ms);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    futex_call(&futex_word, op, 0, &deadline, FUTEX_BITSET_MATCH_ANY);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    return elapsed_ms(t0, t1);
}

static double cond_timedwait(clockid_t clock, long ms) {
    pthread_condattr_t ca;
    pthread_cond_t cond;
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    struct timespec t0, t1, deadline;

    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, clock);
    pthread_cond_init(&cond, &ca);

    clock_gettime(clock, &deadline);
    add_ms(&deadline, ms);
    pthread_mutex_lock(&mutex);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    pthread_cond_timedwait(&cond, &mutex, &deadline);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    pthread_mutex_unlock(&mutex);

    pthread_cond_destroy(&cond);
    pthread_condattr_destroy(&ca);
    return elapsed_ms(t0, t1);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    // Backstop only: every wait here is bounded, so the alarm firing means one
    // of them lost its timeout entirely and blocked forever.
    alarm(test_watchdog_secs(120));

    ck_slept("FUTEX_WAIT, relative 500ms", futex_wait_rel(WANT_MS));

    // A whole-second timeout survived the truncation intact, which is what let
    // the bug hide -- keep one here so a future regression that breaks only the
    // whole-second case is caught too.
    {
        double ms = futex_wait_rel(1000);
        int ok = ms >= 800 && ms <= CEIL_MS;
        if (!ok)
            failf("FUTEX_WAIT, relative 1s", (uint64_t) ms, 800, CEIL_MS, 0, 0, 0);
        test_logf("  %-52s %s (%.0fms)\n", "FUTEX_WAIT, relative 1s",
                  ok ? "ok" : "FAIL", ms);
    }

    // Seconds AND nanoseconds together: reading the timespec one field short
    // shifts tv_nsec into tv_sec, so a mixed value is the one that catches a
    // half-fixed parser.
    ck_slept("FUTEX_WAIT, relative 1.5s -> at least 500ms", futex_wait_rel(1500));

    ck_slept("FUTEX_WAIT_BITSET, monotonic deadline +500ms",
             futex_wait_abs(CLOCK_MONOTONIC, WANT_MS));
    ck_slept("FUTEX_WAIT_BITSET, realtime deadline +500ms",
             futex_wait_abs(CLOCK_REALTIME, WANT_MS));

    // The three glibc primitives built on the above. These are what real
    // software calls, and what spun.
    {
        sem_t sem;
        struct timespec t0, t1, deadline;
        sem_init(&sem, 0, 0);
        clock_gettime(CLOCK_REALTIME, &deadline);
        add_ms(&deadline, WANT_MS);
        clock_gettime(CLOCK_MONOTONIC, &t0);
        sem_timedwait(&sem, &deadline);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        sem_destroy(&sem);
        ck_slept("sem_timedwait 500ms", elapsed_ms(t0, t1));
    }
    ck_slept("pthread_cond_timedwait, CLOCK_REALTIME 500ms",
             cond_timedwait(CLOCK_REALTIME, WANT_MS));
    ck_slept("pthread_cond_timedwait, CLOCK_MONOTONIC 500ms",
             cond_timedwait(CLOCK_MONOTONIC, WANT_MS));
    {
        pthread_mutex_t held = PTHREAD_MUTEX_INITIALIZER;
        struct timespec t0, t1, deadline;
        pthread_mutex_lock(&held);   // contended against ourselves
        clock_gettime(CLOCK_REALTIME, &deadline);
        add_ms(&deadline, WANT_MS);
        clock_gettime(CLOCK_MONOTONIC, &t0);
        pthread_mutex_timedlock(&held, &deadline);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        ck_slept("pthread_mutex_timedlock 500ms", elapsed_ms(t0, t1));
    }

    // The other half of the contract: a deadline already in the past must NOT
    // wait. Making every timeout long would "fix" the bug above and break this.
    {
        struct timespec t0, t1, past = {1, 0};   // absolute, long gone
        clock_gettime(CLOCK_MONOTONIC, &t0);
        int r = (int) syscall(SYS_futex, &futex_word,
                              FUTEX_WAIT_BITSET | FUTEX_PRIVATE_FLAG, 0, &past, NULL,
                              FUTEX_BITSET_MATCH_ANY);
        int err = r < 0 ? errno : 0;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double ms = elapsed_ms(t0, t1);
        int ok = err == ETIMEDOUT && ms < 200;
        if (!ok)
            failf("expired deadline returns ETIMEDOUT at once",
                  (uint64_t) err, (uint64_t) ms, 0, ETIMEDOUT, 0, 0);
        test_logf("  %-52s %s (%.0fms, %s)\n",
                  "expired deadline returns ETIMEDOUT at once", ok ? "ok" : "FAIL",
                  ms, strerror(err));
    }

    return finish_suite("futex_timeout_duration");
}
