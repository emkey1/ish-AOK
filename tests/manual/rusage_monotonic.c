/*
 * rusage_monotonic -- a process's own CPU total must never go backward.
 *
 * getrusage(RUSAGE_SELF) and clock_gettime(CLOCK_PROCESS_CPUTIME_ID) are
 * process-wide: AOK answers both from rusage_get_group_of(), which adds
 * group->rusage (the rolled-up totals of threads that have already exited) to
 * a live sample of every thread still on group->threads. Both halves only ever
 * grow, so the sum must too -- and a guest that sees it shrink computes a
 * NEGATIVE delta. `time`, `top` and anything sampling a process's CPU over an
 * interval print nonsense from one such reading.
 *
 * A thread was counted in both halves at once at each end of its life:
 *
 *   exit   do_exit() rolls a thread's final usage into group->rusage, but the
 *          thread stays on group->threads until exit_tgroup() unlinks it ~140
 *          lines and a pids_lock acquisition later. In that window a reader
 *          added a live sample of the still-running host thread ON TOP of the
 *          total it had just been folded into, then watched the figure
 *          collapse once the unlink happened. Reproduced as a pthread_join
 *          followed by two RUSAGE_SELF reads where the second was lower by
 *          exactly the joined thread's own CPU time: 5 in 200 cycles.
 *
 *   clone  copy_task() links a new thread into group->threads well before
 *          task_start() gives it a host thread, and until then task->thread
 *          still holds the CREATING thread's pthread (task_create_ copies the
 *          whole struct). Sampling it added the creator's entire accumulated
 *          CPU a second time and retracted it once the real thread started:
 *          ~130 backward steps per 300 pthread_create/join cycles, each one
 *          the size of the creating thread's balance. /proc/stat's per-CPU
 *          walker already guarded this end (host_thread_started, and see
 *          proc_stat_monotonic); the rusage walk did not.
 *
 * So the shape is a sampler thread reading the process total as fast as it can
 * while another thread churns short-lived CPU-burning threads -- one sample
 * inside either window is a failure, and there is no tolerance to tune,
 * because a real kernel cannot produce a decrease here at all.
 *
 * Phase 2 then replays the exact sequence fork_tgroup_reset's last assertion
 * makes (join a worker, read, fork and wait, read again) with the sampler
 * still hammering the group lock, which is what widens the exit window enough
 * for that assertion to fail once in twenty runs on its own.
 *
 * Also passes on real Linux, where both readings are monotonic by
 * construction.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

// Enough thread churn that the windows above are hit many times over if they
// are open at all, and small enough to stay a few seconds under emulation.
#define CHURN_THREADS   150
#define CHURN_BURN_US   2000    // so an exiting thread's double count is visible
#define MAIN_BALANCE_US 150000  // so a clone-window double count is unmissable
#define FORK_CYCLES     25
#define FORK_BURN_US    5000
#define PHASE_BUDGET_S  40      // wall-clock backstop, per phase

static long long rusage_us(int who) {
    struct rusage r;
    if (getrusage(who, &r) != 0)
        return -1;
    return (long long) r.ru_utime.tv_sec * 1000000 + r.ru_utime.tv_usec +
           (long long) r.ru_stime.tv_sec * 1000000 + r.ru_stime.tv_usec;
}

static long long process_cputime_us(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) != 0)
        return -1;
    return (long long) ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static long long wall_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long) tv.tv_sec * 1000000 + tv.tv_usec;
}

static void burn_thread_cpu(long long want_us) {
    volatile unsigned long sink = 0;
    long long start = rusage_us(RUSAGE_THREAD);
    long long wall_deadline = wall_us() + 5 * 1000000;   // never spin forever
    if (start < 0)
        return;
    for (;;) {
        for (int i = 0; i < 20000; i++)
            sink += (unsigned long) i * 2654435761u;
        long long now = rusage_us(RUSAGE_THREAD);
        if (now < 0 || now - start >= want_us || wall_us() > wall_deadline)
            return;
    }
}

// What the sampler found, named by which reading it came from.
struct watch {
    const char *what;
    long long prev;
    long long drops;
    long long worst;
    long long samples;
    long long first_prev, first_now;    // the first violation, for the report
};

static void watch_take(struct watch *w, long long v) {
    if (v < 0)
        return;             // the call is unsupported; not this test's business
    w->samples++;
    if (w->prev >= 0 && v < w->prev) {
        if (w->drops == 0) {
            w->first_prev = w->prev;
            w->first_now = v;
        }
        w->drops++;
        if (w->prev - v > w->worst)
            w->worst = w->prev - v;
    }
    w->prev = v;
}

static struct watch self_watch = {"getrusage(RUSAGE_SELF)", -1, 0, 0, 0, 0, 0};
static struct watch clock_watch = {"CLOCK_PROCESS_CPUTIME_ID", -1, 0, 0, 0, 0, 0};
static atomic_int sampler_stop;

static void *sampler(void *arg) {
    (void) arg;
    while (!atomic_load(&sampler_stop)) {
        watch_take(&self_watch, rusage_us(RUSAGE_SELF));
        watch_take(&clock_watch, process_cputime_us());
    }
    return NULL;
}

// The burn is the argument, so an exiting thread's double count is a
// different, recognisable size in each phase.
static void *churn_worker(void *arg) {
    burn_thread_cpu(*(const long long *) arg);
    return NULL;
}

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-58s got=%-10ld want=%ld\n", label, got, want);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(300));

    // A distinctive balance on this thread first: it is what a clone-window
    // double count would add, so the failure is large rather than marginal.
    burn_thread_cpu(MAIN_BALANCE_US);
    test_logf("  main thread balance: %lldus\n", rusage_us(RUSAGE_THREAD));

    pthread_t sam;
    if (pthread_create(&sam, NULL, sampler, NULL) != 0) {
        printf("FAIL rusage_monotonic: pthread_create (sampler) failed (%s)\n",
               strerror(errno));
        return finish_suite("rusage_monotonic");
    }

    // Phase 1: thread churn under the sampler.
    static const long long churn_burn = CHURN_BURN_US;
    static const long long fork_burn = FORK_BURN_US;
    long long deadline = wall_us() + (long long) PHASE_BUDGET_S * 1000000;
    int churned = 0;
    for (int i = 0; i < CHURN_THREADS && wall_us() < deadline; i++) {
        pthread_t t;
        if (pthread_create(&t, NULL, churn_worker, (void *) &churn_burn) != 0)
            break;          // host thread ceiling; what ran is still a test
        pthread_join(t, NULL);
        churned++;
    }

    // Phase 2: fork_tgroup_reset's own sequence, with the sampler still
    // holding the group lock against the exiting worker.
    deadline = wall_us() + (long long) PHASE_BUDGET_S * 1000000;
    int cycles = 0, fork_drops = 0;
    long long worst_fork_drop = 0;
    for (int i = 0; i < FORK_CYCLES && wall_us() < deadline; i++) {
        pthread_t t;
        if (pthread_create(&t, NULL, churn_worker, (void *) &fork_burn) != 0)
            break;
        pthread_join(t, NULL);
        long long before = rusage_us(RUSAGE_SELF);
        pid_t kid = fork();
        if (kid < 0)
            break;
        if (kid == 0)
            _exit(0);
        int status = 0;
        waitpid(kid, &status, 0);
        long long after = rusage_us(RUSAGE_SELF);
        if (before >= 0 && after >= 0 && after < before) {
            fork_drops++;
            if (before - after > worst_fork_drop)
                worst_fork_drop = before - after;
        }
        cycles++;
    }

    atomic_store(&sampler_stop, 1);
    pthread_join(sam, NULL);

    test_logf("  churned=%d threads, fork cycles=%d\n", churned, cycles);
    test_logf("  %s: %lld samples, %lld backward, worst %lldus\n",
              self_watch.what, self_watch.samples, self_watch.drops, self_watch.worst);
    test_logf("  %s: %lld samples, %lld backward, worst %lldus\n",
              clock_watch.what, clock_watch.samples, clock_watch.drops, clock_watch.worst);

    // Without any churn at all there is nothing to have raced, so say so
    // rather than passing on an empty run.
    if (churned == 0 || self_watch.samples == 0) {
        test_logf("  no thread churn or no samples taken, skipped\n");
        return finish_suite("rusage_monotonic");
    }

    if (self_watch.drops != 0)
        test_logf("  first: %lld -> %lld\n", self_watch.first_prev, self_watch.first_now);
    ck("getrusage(RUSAGE_SELF) never goes backward",
       self_watch.drops == 0, 1);
    if (clock_watch.samples > 0) {
        if (clock_watch.drops != 0)
            test_logf("  first: %lld -> %lld\n", clock_watch.first_prev, clock_watch.first_now);
        ck("nor does CLOCK_PROCESS_CPUTIME_ID", clock_watch.drops == 0, 1);
    }
    if (cycles > 0) {
        if (fork_drops != 0)
            test_logf("  worst join-then-fork drop: %lldus\n", worst_fork_drop);
        ck("nor across a join, a fork and a wait", fork_drops == 0, 1);
    }

    return finish_suite("rusage_monotonic");
}
