// ITIMER_PROF and ITIMER_VIRTUAL fire once per interval of the process's CPU
// time -- at the rate asked for, not a fraction of it.
//
// Regression for a triage report: the profiling timer fired at a quarter to
// half the requested rate, so a sampling profiler (gprof, gperftools, Go's
// pprof, Python's statistical profilers) saw two to four times fewer samples
// than the CPU spent and scaled every figure it printed by that much. AOK has
// no CPU-time timer to wait on; a sampler ticking on wall time compares the
// process's CPU clock against the deadline. It ticked every 20 ms, fired at
// most once per tick, and re-armed from the moment it looked rather than from
// the deadline, so a 10 ms interval could never fire more often than every
// 20 ms of wall time -- half the rate with one busy thread, a quarter with two.
//
// Measured over a fixed stretch of CPU time with every thread spinning: the
// signals delivered per interval of process CPU time consumed. Linux (camd,
// 6.12, HZ 250) delivers 0.97-1.00 of them with one thread and two, at 10 ms
// and 4 ms intervals. The bar here is 0.8, under emulation and with the suite
// running alongside -- a timer at half rate is nowhere near it.
//
// Oracle: Linux 6.12 (camd), glibc x86_64 and -m32.
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "test_common.h"

#define TEST_NAME "itimer_prof_rate"

static atomic_long fired;
static atomic_int stop;

static void on_signal(int sig) {
    (void) sig;
    atomic_fetch_add(&fired, 1);
}

// The process's CPU time: user only for ITIMER_VIRTUAL, which counts nothing
// else, and user plus system for ITIMER_PROF.
static double cpu_secs(int which) {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    double user = ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6;
    double sys = ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
    return which == ITIMER_VIRTUAL ? user : user + sys;
}

static void *spin(void *arg) {
    (void) arg;
    volatile unsigned long n = 0;
    while (!atomic_load(&stop))
        n++;
    return NULL;
}

// Signals per interval of CPU time, for `threads` spinning threads (the
// caller among them) and a timer of `which` every `interval_us`.
static double measure(int which, int sig, long interval_us, int threads, double cpu_budget) {
    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sa.sa_flags = SA_RESTART;
    sigaction(sig, &sa, NULL);
    atomic_store(&fired, 0);
    atomic_store(&stop, 0);

    pthread_t extra[8];
    for (int i = 0; i < threads - 1; i++)
        pthread_create(&extra[i], NULL, spin, NULL);

    struct itimerval it = {
        .it_interval = {.tv_sec = 0, .tv_usec = interval_us},
        .it_value = {.tv_sec = 0, .tv_usec = interval_us},
    };
    double start = cpu_secs(which);
    if (setitimer(which, &it, NULL) != 0) {
        printf("FAIL: setitimer (%s)\n", strerror(errno));
        failures_total++;
        return 0;
    }
    // Spin in user code, asking the clock -- a syscall, so system time --
    // only now and then.
    volatile unsigned long n = 0;
    do {
        for (unsigned long i = 0; i < 2000000; i++)
            n++;
    } while (cpu_secs(which) - start < cpu_budget);
    struct itimerval off = {0};
    setitimer(which, &off, NULL);
    double used = cpu_secs(which) - start;
    atomic_store(&stop, 1);
    for (int i = 0; i < threads - 1; i++)
        pthread_join(extra[i], NULL);

    double expected = used / (interval_us / 1e6);
    return atomic_load(&fired) / expected;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(120));

    struct {
        const char *name;
        int which, sig;
        long interval_us;
        int threads;
    } cases[] = {
        {"ITIMER_PROF 10ms, 1 thread", ITIMER_PROF, SIGPROF, 10000, 1},
        {"ITIMER_PROF 10ms, 2 threads", ITIMER_PROF, SIGPROF, 10000, 2},
        {"ITIMER_PROF 4ms, 1 thread", ITIMER_PROF, SIGPROF, 4000, 1},
        {"ITIMER_VIRTUAL 10ms, 1 thread", ITIMER_VIRTUAL, SIGVTALRM, 10000, 1},
        {"ITIMER_VIRTUAL 10ms, 2 threads", ITIMER_VIRTUAL, SIGVTALRM, 10000, 2},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        double ratio = measure(cases[i].which, cases[i].sig, cases[i].interval_us,
                               cases[i].threads, 1.0);
        test_logf("%-32s %.2f of the signals its CPU time is owed\n", cases[i].name, ratio);
        if (ratio < 0.8 || ratio > 1.1) {
            printf("FAIL: %s delivered %.2f of the signals its CPU time is owed\n",
                   cases[i].name, ratio);
            failures_total++;
        }
    }
    return finish_suite(TEST_NAME);
}
