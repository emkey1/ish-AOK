// Sleeps and timers: what they write, what they count, what they refuse.
//
//   nanosleep's rmtp is an OUTPUT for an interrupted sleep only. A completed
//   sleep must not touch it -- there is nothing remaining to report, and the
//   buffer is the caller's. AOK wrote it on success, which clobbered a buffer
//   the caller had left data in and, worse, FAULTED: a sleep that ran to
//   completion returned EFAULT when rmtp was not a valid pointer, a pointer
//   Linux never even looks at.
//
//   A POSIX timer never has more than one signal outstanding. When it expires
//   again while its last signal is still queued, the missed expiration is
//   counted on si_overrun -- the whole reason that field exists. AOK queued
//   one signal per expiration and hardcoded si_overrun to 0, so a periodic
//   timer whose signal was blocked for a second produced two hundred signals
//   and no way to know how far behind it was.
//
//   setitimer's interval survives a disarm for ITIMER_VIRTUAL and ITIMER_PROF
//   and does NOT for ITIMER_REAL. AOK had the two exactly the wrong way round.
//
//   clock_settime checked its permission before looking at its arguments, so
//   a caller that got the struct wrong was told it lacked permission and went
//   looking for the wrong problem. On the arm64 path the timespec pointer was
//   not even passed to the implementation.
//
//   gettimeofday's timezone is the KERNEL's, and Linux's is always zero.
//   Darwin answers with the host's, which leaked the Mac's DST flag into a
//   field no Linux ever sets. Probed through syscall() because musl's
//   gettimeofday ignores tz entirely and never issues the call, and glibc's
//   zeroes it in userspace -- neither tells you what the kernel did.
//
// Measured against x86_64 glibc on Linux 6.12.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

static int on_ish;

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-58s got=%-10ld want=%ld\n", label, got, want);
}

static void ck_range(const char *label, long got, long lo, long hi) {
    if (got < lo || got > hi)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) lo, (uint64_t) hi, 0);
    test_logf("  %-58s got=%-10ld want=%ld..%ld\n", label, got, lo, hi);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(120));
    on_ish = access("/proc/ish", F_OK) == 0;

    // ---- a completed sleep leaves rmtp alone -----------------------------
    // Raw syscalls: this is about what the kernel writes, and a libc wrapper
    // that substitutes its own buffer would hide it.
    {
        struct timespec req = { 0, 1000000 }, rem;
        rem.tv_sec = 12345;
        rem.tv_nsec = 54321;
        ck("nanosleep completes", (long) syscall(SYS_nanosleep, &req, &rem), 0);
        ck("  leaving rmtp.tv_sec alone", (long) rem.tv_sec, 12345);
        ck("  and rmtp.tv_nsec", (long) rem.tv_nsec, 54321);
        // ...and a bad rmtp is never dereferenced, so it cannot fail.
        errno = 0;
        long r = syscall(SYS_nanosleep, &req, (void *) 1);
        ck("nanosleep with a BAD rmtp still succeeds", r, 0);
        ck("  and specifically not EFAULT", r < 0 && errno == EFAULT ? 1 : 0, 0);
    }
    {
        // clock_nanosleep is a second implementation of the same rule.
        struct timespec req = { 0, 1000000 }, rem;
        rem.tv_sec = 999;
        rem.tv_nsec = 888;
        ck("clock_nanosleep completes",
           (long) syscall(SYS_clock_nanosleep, CLOCK_MONOTONIC, 0, &req, &rem), 0);
        ck("  leaving rmtp.tv_sec alone", (long) rem.tv_sec, 999);
        ck("  and rmtp.tv_nsec", (long) rem.tv_nsec, 888);
        errno = 0;
        long r = syscall(SYS_clock_nanosleep, CLOCK_MONOTONIC, 0, &req, (void *) 1);
        ck("clock_nanosleep with a BAD rmtp succeeds", r, 0);
    }

    // ---- how many POSIX timers, and how running out is reported ---------
    {
        struct sigevent sev;
        memset(&sev, 0, sizeof sev);
        sev.sigev_notify = SIGEV_NONE;
        static timer_t ts[64];
        int n = 0, err = 0;
        for (; n < 64; n++) {
            errno = 0;
            if (timer_create(CLOCK_MONOTONIC, &sev, &ts[n]) != 0) {
                err = errno;
                break;
            }
        }
        // Linux has no small fixed cap (its bound is RLIMIT_SIGPENDING);
        // AOK's array is fixed but far above anything a program uses. 64 is
        // well inside both, and was four times the old limit of 16.
        ck("64 POSIX timers can exist at once", n, 64);
        ck("  with no failure", err, 0);
        for (int i = 0; i < n; i++)
            timer_delete(ts[i]);
    }
    if (on_ish) {
        // Exhausting the supply reports EAGAIN -- what Linux gives when a
        // process hits its own timer limit, and what callers check for.
        // ENOMEM says the kernel is out of memory, which it is not, and sends
        // a caller down an allocation-failure path instead of a retry one.
        struct sigevent sev;
        memset(&sev, 0, sizeof sev);
        sev.sigev_notify = SIGEV_NONE;
        static timer_t ts[4096];
        int n = 0, err = 0;
        for (; n < 4096; n++) {
            errno = 0;
            if (timer_create(CLOCK_MONOTONIC, &sev, &ts[n]) != 0) {
                err = errno;
                break;
            }
        }
        ck("running out of timers is EAGAIN", err, EAGAIN);
        ck("  and not ENOMEM", err == ENOMEM ? 1 : 0, 0);
        for (int i = 0; i < n; i++)
            timer_delete(ts[i]);
    }

    // ---- timerfd_settime validates its timespecs ------------------------
    {
        int fd = timerfd_create(CLOCK_MONOTONIC, 0);
        ck("timerfd_create", fd >= 0 ? 1 : 0, 1);
        if (fd >= 0) {
            struct itimerspec bad;
            memset(&bad, 0, sizeof bad);
            bad.it_value.tv_nsec = 1000000000;
            errno = 0;
            ck("timerfd_settime it_value.tv_nsec=1e9 is EINVAL",
               timerfd_settime(fd, 0, &bad, NULL) < 0 ? errno : 0, EINVAL);
            memset(&bad, 0, sizeof bad);
            bad.it_value.tv_sec = 1;
            bad.it_interval.tv_nsec = 1000000000;
            errno = 0;
            ck("  and it_interval.tv_nsec=1e9 too",
               timerfd_settime(fd, 0, &bad, NULL) < 0 ? errno : 0, EINVAL);
            memset(&bad, 0, sizeof bad);
            bad.it_value.tv_nsec = -1;
            errno = 0;
            ck("  and a negative tv_nsec",
               timerfd_settime(fd, 0, &bad, NULL) < 0 ? errno : 0, EINVAL);
            // A valid one still works: a fix that refused everything would
            // pass all three above.
            struct itimerspec ok;
            memset(&ok, 0, sizeof ok);
            ok.it_value.tv_sec = 100;
            ck("  while a valid one is accepted", timerfd_settime(fd, 0, &ok, NULL), 0);
            close(fd);
        }
    }

    // ---- overruns -------------------------------------------------------
    {
        sigset_t set, old;
        sigemptyset(&set);
        sigaddset(&set, SIGRTMIN);
        sigprocmask(SIG_BLOCK, &set, &old);
        struct sigevent sev;
        memset(&sev, 0, sizeof sev);
        sev.sigev_notify = SIGEV_SIGNAL;
        sev.sigev_signo = SIGRTMIN;
        timer_t t;
        if (timer_create(CLOCK_MONOTONIC, &sev, &t) != 0) {
            failf("timer_create for overruns", (uint64_t) errno, 0, 0, 0, 0, 0);
        } else {
            struct itimerspec its;
            its.it_value.tv_sec = 0;
            its.it_value.tv_nsec = 5000000;       // 5ms, then every 5ms
            its.it_interval.tv_sec = 0;
            its.it_interval.tv_nsec = 5000000;
            ck("arm a 5ms periodic timer", timer_settime(t, 0, &its, NULL), 0);
            struct timespec nap = { 1, 0 };
            nanosleep(&nap, NULL);                // ~200 periods, signal blocked
            siginfo_t si;
            struct timespec zero = { 0, 0 };
            int got = sigtimedwait(&set, &si, &zero);
            ck("a signal is waiting", got == SIGRTMIN ? 1 : 0, 1);
            long overrun = got > 0 ? si.si_overrun : -1;
            // ~199 in a second of 5ms periods, but scheduling jitter moves it
            // by a period or two either way, so a range rather than a number.
            ck_range("  si_overrun counts the missed periods", overrun, 150, 260);
            ck("  and timer_getoverrun agrees", (long) timer_getoverrun(t), overrun);
            // The point of counting them: only ONE signal was queued. Two
            // hundred queued signals is what this looked like before, and it
            // is why si_overrun exists.
            errno = 0;
            int again = sigtimedwait(&set, &si, &zero);
            ck("  and only one signal was queued", again < 0 && errno == EAGAIN ? 1 : 0, 1);
            // Disarm before restoring the mask, or the next expiry arrives
            // unblocked and kills the test.
            memset(&its, 0, sizeof its);
            timer_settime(t, 0, &its, NULL);
            timer_delete(t);
        }
        // Drop anything that slipped through while disarming.
        struct timespec zero = { 0, 0 };
        siginfo_t drop;
        while (sigtimedwait(&set, &drop, &zero) > 0) { }
        sigprocmask(SIG_SETMASK, &old, NULL);
    }

    // ---- setitimer's interval across a disarm ---------------------------
    {
        struct itimerval iv, out;
        memset(&iv, 0, sizeof iv);
        iv.it_interval.tv_sec = 7;
        iv.it_value.tv_sec = 0;             // interval set, timer disarmed

        ck("setitimer(ITIMER_REAL) disarmed", setitimer(ITIMER_REAL, &iv, NULL), 0);
        memset(&out, 0, sizeof out);
        getitimer(ITIMER_REAL, &out);
        // ITIMER_REAL discards the interval with the disarm: do_setitimer
        // clears it_real_incr when it_value is zero.
        ck("  ITIMER_REAL forgets its interval", (long) out.it_interval.tv_sec, 0);
        ck("  and is not armed", (long) out.it_value.tv_sec, 0);

        for (int which = 0; which < 2; which++) {
            int w = which == 0 ? ITIMER_VIRTUAL : ITIMER_PROF;
            const char *n = which == 0 ? "ITIMER_VIRTUAL" : "ITIMER_PROF";
            char lab[80];
            snprintf(lab, sizeof lab, "setitimer(%s) disarmed", n);
            ck(lab, setitimer(w, &iv, NULL), 0);
            memset(&out, 0, sizeof out);
            getitimer(w, &out);
            // ...while the CPU itimers keep it: set_cpu_itimer stores it->incr
            // unconditionally and get_cpu_itimer returns it unconditionally.
            snprintf(lab, sizeof lab, "  %s KEEPS its interval", n);
            ck(lab, (long) out.it_interval.tv_sec, 7);
            snprintf(lab, sizeof lab, "  and %s is not armed", n);
            ck(lab, (long) out.it_value.tv_sec, 0);
            // Clear it so nothing is left set for the rest of the suite.
            memset(&iv, 0, sizeof iv);
            setitimer(w, &iv, NULL);
            iv.it_interval.tv_sec = 7;
        }
        memset(&iv, 0, sizeof iv);
        setitimer(ITIMER_REAL, &iv, NULL);
    }

    // ---- clock_settime looks before it refuses ---------------------------
    // Only the error paths. A well-formed request would MOVE THE CLOCK on a
    // Linux host running this as root, so it is never issued there.
    {
        errno = 0;
        long r = syscall(SYS_clock_settime, CLOCK_REALTIME, (void *) 1);
        ck("clock_settime with a bad pointer is EFAULT", r < 0 ? errno : 0, EFAULT);
        struct timespec bad;
        clock_gettime(CLOCK_REALTIME, &bad);
        bad.tv_nsec = 1000000000;
        errno = 0;
        r = syscall(SYS_clock_settime, CLOCK_REALTIME, &bad);
        ck("  and an out-of-range tv_nsec is EINVAL", r < 0 ? errno : 0, EINVAL);
        // An unknown clock is EINVAL regardless.
        errno = 0;
        r = syscall(SYS_clock_settime, 99, (void *) 1);
        ck("  an unknown clock is EINVAL", r < 0 ? errno : 0, EINVAL);
    }
    if (on_ish) {
        // A well-formed request is refused, not silently accepted: iSH cannot
        // move the host clock. Safe to issue here precisely because it does
        // nothing.
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        errno = 0;
        long r = syscall(SYS_clock_settime, CLOCK_REALTIME, &now);
        ck("a well-formed clock_settime is EPERM", r < 0 ? errno : 0, EPERM);
    }

    // ---- the timezone gettimeofday reports -------------------------------
#ifdef SYS_gettimeofday
    {
        struct timeval tv;
        struct timezone tz;
        memset(&tz, 0xAA, sizeof tz);
        memset(&tv, 0, sizeof tv);
        errno = 0;
        long r = syscall(SYS_gettimeofday, &tv, &tz);
        ck("gettimeofday succeeds", r, 0);
        ck("  and it really ran", tv.tv_sec > 0 ? 1 : 0, 1);
        ck("  tz_minuteswest is 0", (long) tz.tz_minuteswest, 0);
        // The one that leaked: Darwin reports the host's DST flag, and no
        // Linux ever sets this field.
        ck("  tz_dsttime is 0", (long) tz.tz_dsttime, 0);
    }
#endif

    return finish_suite("timer_conventions");
}
