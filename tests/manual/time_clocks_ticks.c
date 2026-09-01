// Six things the time syscalls got wrong, several of which silently produced
// a plausible-looking wrong answer rather than an error.
//
//   times() always returned 0. Callers use the DIFFERENCE between two calls to
//   measure elapsed time, so every such measurement came out as zero -- what
//   `time` in a shell without a builtin, and any benchmark using times(),
//   reports. It also filled tms_cutime/tms_cstime with the CALLER's own CPU
//   time instead of its reaped children's, so a build tool asking how long its
//   children took was handed its own figure. The children's accounting already
//   existed and was already right; it just was not being read.
//
//   ...and the 32-bit struct tms layout was written to arm64 and riscv64
//   guests, whose clock_t is 64-bit. tms_utime happened to survive whenever
//   tms_stime was 0, which is why this hid: only tms_cutime came back as
//   obvious garbage.
//
//   nanosleep() did not validate its argument, so a tv_nsec outside
//   [0,999999999] or a negative tv_sec returned success without sleeping --
//   telling a caller that had computed a bad duration that its sleep happened.
//   clock_nanosleep already had the check.
//
//   clock_nanosleep on a CPU-time clock slept WALL time, which is exactly
//   backwards for the thing that clock measures: an idle process returned
//   immediately from a request Linux never completes, and a busy one returned
//   far too early.
//
//   CLOCK_REALTIME_ALARM / CLOCK_BOOTTIME_ALARM did not exist, so
//   clock_gettime and clock_getres on them failed with EINVAL -- which Linux
//   never does for any user -- and timerfd_create on them said EINVAL where
//   Linux says EPERM without CAP_WAKE_ALARM. EINVAL reads as "this kernel has
//   no such clock" and a caller gives up on it entirely.
//
//   The dynamic per-process / per-thread CPU clock ids were rejected. Linux
//   encodes a pid into a NEGATIVE clockid, which is what clock_getcpuclockid()
//   and pthread_getcpuclockid() hand out; the C library validates one by
//   calling clock_getres on it, so rejecting them made both functions fail.
//
// Measured against x86_64 glibc on Linux 6.12.
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/times.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <time.h>

#include <sys/syscall.h>
#include <signal.h>
#include <string.h>
// i386's syscall table predates 64-bit time_t, so musl there defines only the
// *_time64 numbers for the clock calls -- SYS_clock_gettime and
// SYS_clock_getres do not exist -- while its struct timespec already has a
// 64-bit tv_sec. The plain 32-bit numbers (SYS_clock_gettime32) are therefore
// the WRONG ones to pair with it: the kernel would read a 32-bit timespec out
// of a 64-bit one. Alias to the *_time64 numbers, which is what musl itself
// issues on that ABI. Same reasoning as timer_conventions.c.
#ifndef SYS_clock_gettime
#define SYS_clock_gettime SYS_clock_gettime64
#endif
#ifndef SYS_clock_getres
#define SYS_clock_getres SYS_clock_getres_time64
#endif

#include "test_common.h"

#ifndef CLOCK_REALTIME_ALARM
#define CLOCK_REALTIME_ALARM 8
#define CLOCK_BOOTTIME_ALARM 9
#endif

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-52s got=%-11ld want=%ld\n", label, got, want);
}

// Spend roughly this much wall time doing arithmetic, so there is CPU time to
// account for. Deliberately not a sleep -- a sleep consumes none.
static void burn_cpu(double secs) {
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    volatile double x = 0;
    do {
        for (int i = 0; i < 200000; i++)
            x += i * 0.5;
        clock_gettime(CLOCK_MONOTONIC, &b);
    } while ((b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9 < secs);
}

static volatile int burner_running = 1;
static void *burner(void *arg) {
    (void) arg;
    volatile double x = 0;
    while (burner_running)
        for (int i = 0; i < 100000; i++)
            x += i * 0.5;
    return NULL;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(120));

    // ---- times() ----------------------------------------------------------
    {
        struct tms t;
        clock_t first = times(&t);
        burn_cpu(0.25);
        clock_t second = times(&t);
        test_logf("    first=%ld second=%ld delta=%ld\n",
                  (long) first, (long) second, (long) (second - first));
        ck("times() returns a nonzero tick count", first > 0, 1);
        ck("  that advances", second > first, 1);
    }
    {
        struct tms before, after;
        times(&before);
        fflush(NULL);
        pid_t c = fork();
        if (c == 0) {
            burn_cpu(0.5);
            _exit(0);
        }
        int st;
        waitpid(c, &st, 0);
        times(&after);
        long child_ticks = (long) (after.tms_cutime - before.tms_cutime);
        struct rusage ru;
        getrusage(RUSAGE_CHILDREN, &ru);
        double children_sec = ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6;
        test_logf("    tms_cutime delta=%ld  own tms_utime=%ld  RUSAGE_CHILDREN=%.3fs\n",
                  child_ticks, (long) after.tms_utime, children_sec);
        ck("a reaped child's CPU time lands in tms_cutime", child_ticks > 0, 1);
        ck("  and RUSAGE_CHILDREN agrees it ran", children_sec > 0.1, 1);
        // The layout check: garbage here is what a 32-bit struct written to a
        // 64-bit guest produces.
        ck("  the value is sane, not a truncation artefact",
           child_ticks > 0 && child_ticks < 100000, 1);
    }

    // ---- nanosleep validates ----------------------------------------------
    {
        struct timespec over = { 0, 1000000000 };
        errno = 0; ck("nanosleep tv_nsec == 1e9 is EINVAL", nanosleep(&over, NULL) < 0 ? errno : 0, EINVAL);
        struct timespec neg_ns = { 0, -1 };
        errno = 0; ck("nanosleep tv_nsec < 0 is EINVAL", nanosleep(&neg_ns, NULL) < 0 ? errno : 0, EINVAL);
        struct timespec neg_s = { -1, 0 };
        errno = 0; ck("nanosleep tv_sec < 0 is EINVAL", nanosleep(&neg_s, NULL) < 0 ? errno : 0, EINVAL);
        struct timespec ok = { 0, 1000000 };
        errno = 0; ck("  and a valid one still sleeps", nanosleep(&ok, NULL) < 0 ? errno : 0, 0);
    }

    // ---- clock_nanosleep on a CPU clock waits for CPU, not wall time -------
    {
        // Refused by the C library on both sides, so it never reaches the
        // kernel -- measured, not assumed.
        struct timespec d = { 0, 10000000 };
        ck("clock_nanosleep(THREAD_CPUTIME) is rejected by libc",
           clock_nanosleep(CLOCK_THREAD_CPUTIME_ID, 0, &d, NULL), EINVAL);

        // An IDLE process consumes no CPU, so a CPU-time sleep must not finish.
        fflush(NULL);
        pid_t c = fork();
        if (c == 0) {
            alarm(3);
            struct timespec cpu = { 0, 100000000 };
            clock_nanosleep(CLOCK_PROCESS_CPUTIME_ID, 0, &cpu, NULL);
            _exit(0);                    // returning at all means wall-clock
        }
        int st;
        waitpid(c, &st, 0);
        ck("an idle process never finishes a CPU-time sleep",
           WIFEXITED(st) && WEXITSTATUS(st) == 0, 0);

        // ...but a BUSY one must, or the fix has just made it hang.
        pthread_t t;
        burner_running = 1;
        if (pthread_create(&t, NULL, burner, NULL) == 0) {
            struct timespec a, b;
            clock_gettime(CLOCK_MONOTONIC, &a);
            struct timespec req = { 0, 200000000 };
            int r = clock_nanosleep(CLOCK_PROCESS_CPUTIME_ID, 0, &req, NULL);
            clock_gettime(CLOCK_MONOTONIC, &b);
            burner_running = 0;
            pthread_join(t, NULL);
            double ms = (b.tv_sec - a.tv_sec) * 1000.0 + (b.tv_nsec - a.tv_nsec) / 1e6;
            test_logf("    busy: rc=%d after %.0fms wall\n", r, ms);
            ck("  a busy process does finish one", r, 0);
            ck("  in a sane amount of time", ms > 50 && ms < 5000, 1);
        }
    }

    // ---- the alarm clocks exist -------------------------------------------
    {
        struct timespec ts;
        errno = 0;
        ck("clock_gettime(CLOCK_REALTIME_ALARM)",
           clock_gettime(CLOCK_REALTIME_ALARM, &ts) < 0 ? errno : 0, 0);
        errno = 0;
        ck("clock_getres(CLOCK_BOOTTIME_ALARM)",
           clock_getres(CLOCK_BOOTTIME_ALARM, &ts) < 0 ? errno : 0, 0);
        // timerfd on one needs CAP_WAKE_ALARM: it succeeds with the capability
        // and is EPERM without. Never EINVAL, which is what a caller reads as
        // "no such clock" -- so that is what is asserted, since the suite runs
        // as root here and as an ordinary user on the oracle.
        errno = 0;
        int fd = timerfd_create(CLOCK_REALTIME_ALARM, 0);
        int e = fd < 0 ? errno : 0;
        test_logf("    timerfd_create(REALTIME_ALARM) rc=%d errno=%d\n", fd, e);
        ck("timerfd_create on an alarm clock is never EINVAL", e == EINVAL, 0);
        ck("  it either works or says EPERM", fd >= 0 || e == EPERM, 1);
        if (fd >= 0)
            close(fd);
    }

    // ---- the dynamic CPU clock ids ----------------------------------------
    {
        clockid_t cid;
        ck("clock_getcpuclockid(self)", clock_getcpuclockid(getpid(), &cid), 0);
        struct timespec ts;
        errno = 0;
        int g = clock_gettime(cid, &ts);
        ck("  its handle reads", g < 0 ? errno : 0, 0);
        ck("  a nonzero CPU time", g == 0 && (ts.tv_sec > 0 || ts.tv_nsec > 0), 1);

        ck("pthread_getcpuclockid(self)", pthread_getcpuclockid(pthread_self(), &cid), 0);
        errno = 0;
        ck("  its handle reads too", clock_gettime(cid, &ts) < 0 ? errno : 0, 0);

        // A dead pid must fail rather than answer, which is how the library
        // decides an id is unusable.
        errno = 0;
        int r = clock_getcpuclockid(999999, &cid);
        ck("  and a pid that does not exist fails", r != 0, 1);
    }

    // ---- the DYNAMIC cpu-clock encoding, reached without the library ------
    //
    // These have to be raw syscalls. musl passes the plain
    // CLOCK_PROCESS_CPUTIME_ID constant straight through, so on an Alpine root
    // nothing above ever sends the kernel the dynamic form -- while glibc
    // turns the same constant into MAKE_PROCESS_CPUCLOCK(0, SCHED) first:
    // 0xfffffffa (-6) for the process, 0xfffffffe (-2) per-thread. The kernel
    // decoded those for clock_gettime but not for timer_create or a NULL
    // clock_getres, so every CPU-time timer a glibc program created came back
    // EINVAL and clock_getcpuclockid came back EFAULT -- on a Devuan guest,
    // invisibly to any suite run only on Alpine.
    {
        const int PROC_CLK = -6;    // pid 0, process-wide
        const int THRD_CLK = -2;    // pid 0, this thread
        struct timespec ts;

        errno = 0;
        ck("dynamic process cpu-clock reads",
           syscall(SYS_clock_gettime, PROC_CLK, &ts) < 0 ? errno : 0, 0);
        errno = 0;
        ck("dynamic thread cpu-clock reads",
           syscall(SYS_clock_gettime, THRD_CLK, &ts) < 0 ? errno : 0, 0);

        // clock_getres(2): "if res is NULL, the resolution is not returned" --
        // so a NULL is a pure "is this clock usable?" question, and it is
        // exactly how the C library validates an id it just computed.
        errno = 0;
        ck("clock_getres(dynamic, NULL) is legal",
           syscall(SYS_clock_getres, PROC_CLK, NULL) < 0 ? errno : 0, 0);
        errno = 0;
        ck("clock_getres(CLOCK_MONOTONIC, NULL) too",
           syscall(SYS_clock_getres, CLOCK_MONOTONIC, NULL) < 0 ? errno : 0, 0);

        // ...and a timer can be built on one, which is what glibc's
        // timer_create(CLOCK_PROCESS_CPUTIME_ID, ...) actually asks for.
        struct sigevent sev;
        memset(&sev, 0, sizeof sev);
        sev.sigev_notify = SIGEV_SIGNAL;
        sev.sigev_signo = SIGALRM;
        int tid = 0;
        errno = 0;
        if (syscall(SYS_timer_create, PROC_CLK, &sev, &tid) < 0) {
            ck("timer_create on a dynamic process cpu-clock", errno, 0);
        } else {
            ck("timer_create on a dynamic process cpu-clock", 0, 0);
            syscall(SYS_timer_delete, tid);
        }
        errno = 0;
        if (syscall(SYS_timer_create, THRD_CLK, &sev, &tid) < 0) {
            ck("timer_create on a dynamic thread cpu-clock", errno, 0);
        } else {
            ck("timer_create on a dynamic thread cpu-clock", 0, 0);
            syscall(SYS_timer_delete, tid);
        }

        // Another process's CPU clock is not something a timer may name.
        errno = 0;
        int other = (int) (~(pid_t) 999999 << 3) | 2;
        ck("  but not another process's",
           syscall(SYS_timer_create, other, &sev, &tid) < 0 ? 1 : 0, 1);
    }

    return finish_suite("time_clocks_ticks");
}
