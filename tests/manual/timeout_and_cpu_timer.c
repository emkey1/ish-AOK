// Two calls that were given a deadline and ignored it.
//
//   semtimedop's timeout was dropped on the floor, so it was semop: a caller
//   that asked to wait 200ms for a semaphore waited as long as it took, or
//   forever. That is the one thing the call exists to avoid -- every user of
//   it is a program that has decided it would rather give up than hang, and it
//   was given the hang anyway with no way to tell.
//
//   A POSIX timer on CLOCK_THREAD_CPUTIME_ID never fired. That clock belongs
//   to one thread, and the timer runs on its own -- which is asleep, so its
//   thread clock never advanced and the deadline never arrived. The timer was
//   created and armed and reported success and then did nothing, which is
//   worse than refusing: a watchdog built on it simply never barks.
//
// Measured against x86_64 glibc on Linux 6.12.
//
// On a kernel where semtimedop ignores its timeout this test cannot report a
// failure, because the call it is testing never returns: it dies on the
// watchdog alarm instead (exit 142, "Alarm clock"), with no PASS line. That is
// the intended failure mode and the only one available -- there is no way to
// ask "does this give up" without risking the hang. It is why the timer
// section below uses SIGUSR1: a SIGALRM handler here would swallow the
// watchdog and turn the bounded death into a real hang.
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-56s got=%-8ld want=%ld\n", label, got, want);
}

static double elapsed_since(struct timespec a) {
    struct timespec b;
    clock_gettime(CLOCK_MONOTONIC, &b);
    return (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
}

// ---- CPU-clock timers -----------------------------------------------------
static volatile sig_atomic_t fired;
static void onfire(int s) { (void) s; fired = 1; }

// Burn CPU for up to `seconds` of wall time, stopping early once the timer
// fires. Wall-bounded rather than CPU-bounded so a kernel where the timer
// never fires ends the test instead of spinning until the watchdog.
static void burn(double seconds) {
    struct timespec a;
    clock_gettime(CLOCK_MONOTONIC, &a);
    volatile unsigned long x = 0;
    while (!fired && elapsed_since(a) < seconds)
        for (int i = 0; i < 100000; i++)
            x += i;
}

// SIGUSR1 rather than the conventional SIGALRM: test_init's watchdog is an
// alarm(), and installing a handler for SIGALRM here would swallow it. On a
// kernel where semtimedop blocks forever this test then hangs instead of
// failing, which is exactly what happened the first time it was run against
// the parent commit.
static int timer_fires_on(clockid_t clk) {
    fired = 0;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = onfire;
    if (sigaction(SIGUSR1, &sa, NULL) != 0)
        return -1;
    struct sigevent sev;
    memset(&sev, 0, sizeof sev);
    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = SIGUSR1;
    timer_t t;
    if (timer_create(clk, &sev, &t) != 0)
        return -errno;
    struct itimerspec its;
    memset(&its, 0, sizeof its);
    its.it_value.tv_nsec = 50000000;   // 50ms of the clock in question
    if (timer_settime(t, 0, &its, NULL) != 0) {
        int e = errno;
        timer_delete(t);
        return -e;
    }
    burn(5.0);
    timer_delete(t);
    return fired ? 1 : 0;
}

// i386 has no semtimedop syscall of its own -- SysV IPC goes through the ipc()
// multiplexer there -- so musl does not define that syscall number on the ABI
// and the raw form below does not even compile. That aborted the whole i386
// regression run at build time, since the runner stops on the first failure.
//
// The raw syscall is deliberate everywhere it works: musl's wrapper has hidden
// kernel behaviour from these tests before. Where it cannot work, libc's
// wrapper is not a workaround but the real path -- an i386 guest reaches
// semtimedop through ipc() no matter who makes the call, so the kernel side
// under test is the same one either way.
static long do_semtimedop(int id, struct sembuf *ops, size_t nops,
                          struct timespec *timeout) {
#ifdef SYS_semtimedop
    return syscall(SYS_semtimedop, id, ops, nops, timeout);
#else
    return semtimedop(id, ops, nops, timeout);
#endif
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(120));

    // ---- semtimedop gives up when told to ---------------------------------
    {
        int id = semget(IPC_PRIVATE, 1, 0600);
        ck("semget", id >= 0, 1);
        if (id >= 0) {
            // The semaphore starts at 0, so "subtract 1" can never proceed:
            // whatever happens here is the timeout's doing.
            struct sembuf op = { 0, -1, 0 };
            struct timespec ts = { 0, 200000000 };   // 200ms
            struct timespec start;
            clock_gettime(CLOCK_MONOTONIC, &start);
            errno = 0;
            long r = do_semtimedop(id, &op, 1, &ts);
            double took = elapsed_since(start);
            int e = errno;
            ck("semtimedop on a blocked op fails", r < 0 ? 1 : 0, 1);
            ck("  with EAGAIN", r < 0 ? e : 0, EAGAIN);
            // ...and it actually waited: returning EAGAIN immediately would
            // pass the checks above while being just as wrong.
            test_logf("  %-56s %.3fs\n", "  after waiting", took);
            ck("  having waited at least the timeout", took >= 0.15 ? 1 : 0, 1);
            ck("  and not much longer", took < 5.0 ? 1 : 0, 1);

            // A zero timeout is the non-blocking form and must not wait.
            struct timespec zero = { 0, 0 };
            clock_gettime(CLOCK_MONOTONIC, &start);
            errno = 0;
            r = do_semtimedop(id, &op, 1, &zero);
            e = errno;
            took = elapsed_since(start);
            ck("a zero timeout is EAGAIN too", r < 0 ? e : 0, EAGAIN);
            ck("  and returns immediately", took < 0.5 ? 1 : 0, 1);

            // A malformed timeout is refused rather than silently ignored.
            // Before the "up" below, while the op still cannot proceed: if the
            // semaphore were already available the op would succeed and this
            // would pass without the validation existing at all.
            struct timespec bad = { 0, 1000000000 };
            errno = 0;
            r = do_semtimedop(id, &op, 1, &bad);
            ck("an out-of-range tv_nsec is EINVAL", r < 0 ? errno : 0, EINVAL);

            // An operation that CAN proceed must still succeed with a timeout
            // set -- a fix that made every timed op fail would pass everything
            // above.
            struct sembuf up = { 0, 1, 0 };
            ck("an op that can proceed still succeeds",
               do_semtimedop(id, &up, 1, &ts), 0);

            semctl(id, 0, IPC_RMID);
        }
    }

    // ---- a timer on each CPU clock actually fires --------------------------
    {
        // The wall clock is the control: if this one fails the test is broken,
        // not the CPU clocks.
        ck("a CLOCK_MONOTONIC timer fires", timer_fires_on(CLOCK_MONOTONIC), 1);
        // The process clock always worked -- the timer thread is in the same
        // process, so reading it anywhere gives the same number.
        ck("a CLOCK_PROCESS_CPUTIME_ID timer fires", timer_fires_on(CLOCK_PROCESS_CPUTIME_ID), 1);
        // The one that did not: this clock belongs to one thread, and the
        // timer's own thread is asleep.
        ck("a CLOCK_THREAD_CPUTIME_ID timer fires", timer_fires_on(CLOCK_THREAD_CPUTIME_ID), 1);
    }

    return finish_suite("timeout_and_cpu_timer");
}
