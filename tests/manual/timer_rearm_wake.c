/*
 * timer_rearm_wake.c -- a timer re-armed from a long deadline to a short one
 * must fire at the short one, however soon after the first arming the second
 * comes.
 *
 * Every armed timer has a host thread that sleeps until its next expiry
 * (util/timer.c). Re-arming one has to cut that sleep short. It used to do so
 * with pthread_kill(SIGUSR1), which is lossy: a poke that landed after the
 * thread had dropped the timer's lock but before it blocked ran its handler
 * and was gone, and the thread slept on to the deadline it had read -- here
 * 30s -- so SIGALRM armed for 5ms never came. Only the gap between the two
 * arming calls decides whether the second lands in that window, so this sweeps
 * the gap in 1us steps from 0 to 60us, with several rounds at each:
 *
 *   1. arm ITIMER_REAL for 30s and let its thread go to sleep on it (1ms);
 *   2. arm it for 30s again, which wakes the thread to re-read the deadline;
 *   3. wait the gap, then arm it for 5ms;
 *   4. SIGALRM must come within LATE_MS, and not before the 5ms.
 *
 * Measured on the Mac CLI (alpine arm64) before the fix: 7 of 488 rounds lost
 * their SIGALRM, at gaps from 8 to 49us. Linux has no such window; this passes
 * there trivially.
 *
 * The witness is sigtimedwait's own timeout, which AOK serves from the waiting
 * task's wait, not from any timer thread.
 *
 * Exits 0 and prints "timer_rearm_wake: PASS" on success.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <signal.h>
#include <sys/time.h>
#include <time.h>

#include "test_common.h"

#define LONG_ARM_MS 30000
#define SHORT_ARM_MS 5
/* How late the short arming's SIGALRM may be. A lost re-arm is 30s late. */
#define LATE_MS 300
#define MAX_GAP_US 60
#define ROUNDS_PER_GAP 8

static double now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e6 + t.tv_nsec / 1e3;
}

static void arm_ms(long ms) {
    struct itimerval it = {0};
    it.it_value.tv_sec = ms / 1000;
    it.it_value.tv_usec = (ms % 1000) * 1000;
    if (setitimer(ITIMER_REAL, &it, NULL) != 0) {
        printf("FAIL setitimer(%ldms): %s\n", ms, strerror(errno));
        failures_total++;
    }
}

static void sleep_us(long us) {
    struct timespec t = {.tv_sec = us / 1000000, .tv_nsec = (us % 1000000) * 1000};
    while (nanosleep(&t, &t) < 0 && errno == EINTR)
        ;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    setvbuf(stdout, NULL, _IONBF, 0);

    sigset_t alrm;
    sigemptyset(&alrm);
    sigaddset(&alrm, SIGALRM);
    sigprocmask(SIG_BLOCK, &alrm, NULL);

    int lost = 0, early = 0, rounds = 0;
    double worst_ms = 0;
    arm_ms(LONG_ARM_MS);
    sleep_us(1000);
    for (int gap = 0; gap <= MAX_GAP_US; gap++) {
        for (int i = 0; i < ROUNDS_PER_GAP; i++) {
            rounds++;
            arm_ms(LONG_ARM_MS);
            double start = now_us();
            while (now_us() - start < gap)
                ;
            double armed = now_us();
            arm_ms(SHORT_ARM_MS);
            struct timespec to = {.tv_sec = 0, .tv_nsec = LATE_MS * 1000000L};
            siginfo_t si;
            int got = sigtimedwait(&alrm, &si, &to);
            double took_ms = (now_us() - armed) / 1000;
            if (got != SIGALRM) {
                if (lost == 0)
                    printf("FAIL gap %dus: SIGALRM re-armed from %ds to %dms had not come after "
                           "%.0fms -- the timer slept on to its old deadline\n",
                           gap, LONG_ARM_MS / 1000, SHORT_ARM_MS, took_ms);
                lost++;
            } else if (took_ms < SHORT_ARM_MS - 0.5) {
                if (early == 0)
                    printf("FAIL gap %dus: SIGALRM armed for %dms came at %.2fms\n",
                           gap, SHORT_ARM_MS, took_ms);
                early++;
            } else if (took_ms > worst_ms) {
                worst_ms = took_ms;
            }
            /* Put the thread back to sleep on a long deadline for the next. */
            arm_ms(LONG_ARM_MS);
            sleep_us(1000);
        }
    }
    arm_ms(0);
    if (lost != 0) {
        printf("FAIL %d of %d re-arms lost their SIGALRM\n", lost, rounds);
        failures_total++;
    }
    if (early != 0) {
        printf("FAIL %d of %d SIGALRMs came early\n", early, rounds);
        failures_total++;
    }
    test_logf("%d rounds, gaps 0-%dus: %d lost, %d early, latest %.2fms\n",
              rounds, MAX_GAP_US, lost, early, worst_ms);
    return finish_suite("timer_rearm_wake");
}
