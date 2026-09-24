/*
 * timer_thread_cpu_early.c -- a CLOCK_THREAD_CPUTIME_ID timer must not fire
 * before its thread has used the CPU time it was armed for.
 *
 * AOK runs such a timer on a host thread that samples the owning thread's CPU
 * clock (posix_timer_thread_cpu_now, kernel/time.c). The sampler counted in
 * jiffies, 10ms steps, and timer_set takes "now" from it at the arming, so an
 * arming was anchored at the last step before it: a 5ms timer armed at 7.3ms
 * of CPU was due at 5ms by that clock, and fired once it read 10ms -- 2.7ms of
 * CPU after it was armed. Linux never fires one before the thread has used
 * the time asked for.
 *
 * A thread that only spins does not show it. The timer thread naps the time
 * left on the WALL clock between samples, and a spinning thread's CPU clock
 * keeps pace with the wall, so by the first sample the whole 5ms has gone and
 * the expiry comes late, by up to a step, rather than early (0 early rounds in
 * 20 on the old binary). A thread that also sleeps runs its CPU clock slower
 * than the wall, and the timer thread samples it with the 5ms not yet used. So
 * after each arming this spins in BURST_NS bursts with a NAP_NS sleep between
 * them, as any thread with work and waits does.
 *
 * Each round spins a random 1-9ms first, to land the arming anywhere in a
 * step, then reads the clock, arms the timer for 5ms, and reads the clock
 * again. It then polls: the pending set first, the clock after, so a signal
 * seen pending was queued before that clock reading. Seen with less than 5ms
 * gone since the reading BEFORE the arming, it fired early, since the arming
 * came after that reading. The signal stays blocked; sigpending is the
 * witness, not the timer under test. The signal must also come, within
 * LATE_NS of CPU after it was due.
 *
 * Before the fix, on the Mac CLI: 11-14 of 30 rounds fired early on each of
 * the six test roots, the earliest with 0.2-2.2ms of the 5ms used. After it,
 * every expiry came 5.016-5.045ms after the reading before the arming. Linux
 * 6.12 (camd, -m64 and -m32) passes, its expiries up to 14ms late.
 *
 * Exits 0 and prints "timer_thread_cpu_early: PASS" on success.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "test_common.h"

#define NS 1000000000LL
#define ROUNDS 30
/* The random spin before each arming: PRE_MIN_NS plus up to PRE_SPAN_NS. */
#define PRE_MIN_NS 1000000LL
#define PRE_SPAN_NS 8000000LL
#define ARM_NS 5000000LL
/* After the arming: spin this much CPU, then sleep this long, and again. */
#define BURST_NS 250000LL
#define NAP_NS 1000000LL
/* How much CPU past its due time the signal may take. Linux checks CPU timers
 * on the scheduler tick, which a thread that sleeps most of the time is often
 * not running for: it is late there by a few ticks. */
#define LATE_NS 100000000LL

#define SIG SIGUSR2

static long long clock_ns(clockid_t c) {
    struct timespec t;
    clock_gettime(c, &t);
    return (long long) t.tv_sec * NS + t.tv_nsec;
}

static long long cpu_ns(void) {
    return clock_ns(CLOCK_THREAD_CPUTIME_ID);
}

static int sig_pending(void) {
    sigset_t p;
    sigpending(&p);
    return sigismember(&p, SIG);
}

static void drain(void) {
    sigset_t one;
    sigemptyset(&one);
    sigaddset(&one, SIG);
    siginfo_t si;
    struct timespec zero = {0, 0};
    while (sigtimedwait(&one, &si, &zero) > 0)
        ;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(test_watchdog_secs(60));

    sigset_t block;
    sigemptyset(&block);
    sigaddset(&block, SIG);
    sigprocmask(SIG_BLOCK, &block, NULL);

    struct sigevent sev;
    memset(&sev, 0, sizeof(sev));
    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = SIG;
    timer_t timer;
    if (timer_create(CLOCK_THREAD_CPUTIME_ID, &sev, &timer) != 0) {
        printf("FAIL timer_create(CLOCK_THREAD_CPUTIME_ID): %s\n", strerror(errno));
        return 1;
    }

    unsigned seed = (unsigned) (getpid() ^ clock_ns(CLOCK_MONOTONIC));
    srand(seed);
    test_logf("seed %u\n", seed);

    int early = 0;
    long long earliest = ARM_NS, cpu_after = 0, wall_after = 0;
    for (int round = 0; round < ROUNDS; round++) {
        drain();
        long long pre = cpu_ns() + PRE_MIN_NS + rand() % PRE_SPAN_NS;
        while (cpu_ns() < pre)
            ;

        struct itimerspec its;
        memset(&its, 0, sizeof(its));
        its.it_value.tv_nsec = ARM_NS;
        long long w0 = clock_ns(CLOCK_MONOTONIC);
        long long t0 = cpu_ns();
        if (timer_settime(timer, 0, &its, NULL) != 0) {
            printf("FAIL timer_settime: %s\n", strerror(errno));
            return 1;
        }
        long long t1 = cpu_ns();

        long long seen = -1, burst_from = t1;
        for (;;) {
            int pending = sig_pending();
            long long c = cpu_ns();
            if (pending) {
                seen = c;
                break;
            }
            if (c - t1 > ARM_NS + LATE_NS)
                break;
            if (c - burst_from >= BURST_NS) {
                struct timespec nap = {0, NAP_NS};
                nanosleep(&nap, NULL);
                burst_from = cpu_ns();
            }
        }
        cpu_after += (seen >= 0 ? seen : cpu_ns()) - t0;
        wall_after += clock_ns(CLOCK_MONOTONIC) - w0;

        if (seen < 0) {
            printf("FAIL round %d: armed for %.1fms of CPU, and not fired after %.1fms\n",
                   round, ARM_NS / 1e6, (cpu_ns() - t1) / 1e6);
            failures_total++;
            memset(&its, 0, sizeof(its));
            timer_settime(timer, 0, &its, NULL);
            continue;
        }
        long long used = seen - t0;
        if (used < ARM_NS) {
            printf("FAIL round %d: armed for %.1fms of CPU, fired with at most %.3fms "
                   "used (armed at %.3fms of CPU)\n",
                   round, ARM_NS / 1e6, used / 1e6, t0 / 1e6);
            failures_total++;
            early++;
            if (used < earliest)
                earliest = used;
        } else {
            test_logf("round %2d: armed at %9.3fms of CPU, seen fired %.3fms after\n",
                      round, t0 / 1e6, used / 1e6);
        }
    }
    timer_delete(timer);
    drain();

    /* What makes an early expiry visible at all: see the top. */
    test_logf("after the armings: %.1fms of CPU in %.1fms\n", cpu_after / 1e6,
              wall_after / 1e6);
    if (early != 0)
        printf("%d of %d rounds fired early, the earliest with %.3fms of %.1fms used\n",
               early, ROUNDS, earliest / 1e6, ARM_NS / 1e6);
    return finish_suite("timer_thread_cpu_early");
}
