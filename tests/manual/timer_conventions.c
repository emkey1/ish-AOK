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
//   and no way to know how far behind it was. timer_getoverrun reports the
//   count of the signal last taken, latched as it is taken; AOK's followed the
//   signal still queued, and went to 0 as soon as the next one was.
//
//   An itimer's signal -- alarm, ITIMER_REAL, ITIMER_VIRTUAL, ITIMER_PROF --
//   is SI_KERNEL, with no sender. AOK sent SI_TIMER with timer id 0, and that
//   is exactly how it recognises POSIX timer 0's signal: timer 0's expiry was
//   counted as an overrun onto a waiting itimer SIGALRM, and taking an itimer
//   SIGALRM set timer 0's timer_getoverrun to 0.
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
#include <sys/signalfd.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

// i386's syscall table predates 64-bit time_t. musl there defines only the
// *_time64 numbers -- SYS_clock_nanosleep and SYS_clock_settime simply do not
// exist -- while its struct timespec already has a 64-bit tv_sec. The plain
// 32-bit numbers (SYS_clock_settime32) are therefore the WRONG ones to pair
// with it: the kernel would read a 32-bit timespec out of a 64-bit one.
//
// So alias to the *_time64 numbers, which is exactly what musl itself issues
// on that ABI. The calls below stay raw syscalls -- the point of this test is
// to reach the kernel's own validation without musl's wrapper sanitising the
// arguments first, and a wrapper could not be used for the bad-pointer cases
// anyway, since it would fault reading the timespec before any syscall.
//
// Until this existed the i386 leg of the release gate never ran at all: the
// runner stops on the first build failure, so a single unbuildable test took
// the whole architecture's suite with it.
#ifndef SYS_clock_nanosleep
#define SYS_clock_nanosleep SYS_clock_nanosleep_time64
#endif
#ifndef SYS_clock_settime
#define SYS_clock_settime SYS_clock_settime64
#endif

#ifndef SI_KERNEL
#define SI_KERNEL 0x80
#endif

static int on_ish;

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-58s got=%-10ld want=%ld\n", label, got, want);
}

static double clock_secs(clockid_t clock) {
    struct timespec ts;
    clock_gettime(clock, &ts);
    return (double) ts.tv_sec + ts.tv_nsec / 1e9;
}

static void burn_cpu(double secs) {
    double start = clock_secs(CLOCK_PROCESS_CPUTIME_ID);
    while (clock_secs(CLOCK_PROCESS_CPUTIME_ID) - start < secs)
        for (volatile int k = 0; k < 100000; k++) { }
}

// Take SIG, which the caller blocks, within `secs` of wall time; -1 if it
// never came. With `burn`, spends CPU while it waits: the CPU-time itimers
// count only while the process runs.
static int take_signal(int sig, int burn, siginfo_t *si, double secs) {
    sigset_t one;
    sigemptyset(&one);
    sigaddset(&one, sig);
    double start = clock_secs(CLOCK_MONOTONIC);
    do {
        struct timespec wait = { 0, burn ? 0 : 50000000 };
        memset(si, 0xa5, sizeof *si);
        if (sigtimedwait(&one, si, &wait) == sig)
            return sig;
        if (burn)
            burn_cpu(0.002);
    } while (clock_secs(CLOCK_MONOTONIC) - start < secs);
    return -1;
}

static int wait_pending(int sig, double secs) {
    double start = clock_secs(CLOCK_MONOTONIC);
    do {
        sigset_t pend;
        sigpending(&pend);
        if (sigismember(&pend, sig))
            return 1;
        struct timespec tick = { 0, 1000000 };
        nanosleep(&tick, NULL);
    } while (clock_secs(CLOCK_MONOTONIC) - start < secs);
    return 0;
}

static void ck_range(const char *label, long got, long lo, long hi) {
    if (got < lo || got > hi)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) lo, (uint64_t) hi, 0);
    test_logf("  %-58s got=%-10ld want=%ld..%ld\n", label, got, lo, hi);
}

// Arm a POSIX timer once for 20ms and wait until it has expired, and its
// signal, sent after that, has had time to arrive.
static void expire_once(timer_t t) {
    struct itimerspec its;
    memset(&its, 0, sizeof its);
    its.it_value.tv_nsec = 20000000;
    timer_settime(t, 0, &its, NULL);
    double start = clock_secs(CLOCK_MONOTONIC);
    do {
        struct timespec tick = { 0, 5000000 };
        nanosleep(&tick, NULL);
        timer_gettime(t, &its);
    } while ((its.it_value.tv_sec || its.it_value.tv_nsec) &&
             clock_secs(CLOCK_MONOTONIC) - start < 5.0);
    struct timespec settle = { 0, 50000000 };
    nanosleep(&settle, NULL);
}

// The same for ITIMER_REAL.
static void itimer_expire_once(void) {
    struct itimerval iv;
    memset(&iv, 0, sizeof iv);
    iv.it_value.tv_usec = 20000;
    setitimer(ITIMER_REAL, &iv, NULL);
    double start = clock_secs(CLOCK_MONOTONIC);
    do {
        struct timespec tick = { 0, 5000000 };
        nanosleep(&tick, NULL);
        getitimer(ITIMER_REAL, &iv);
    } while ((iv.it_value.tv_sec || iv.it_value.tv_usec) &&
             clock_secs(CLOCK_MONOTONIC) - start < 5.0);
    struct timespec settle = { 0, 50000000 };
    nanosleep(&settle, NULL);
}

// Take every pending SIGALRM, which the caller blocks: how many there were,
// and the first two.
static int take_alarms(siginfo_t got[2]) {
    sigset_t alrm;
    sigemptyset(&alrm);
    sigaddset(&alrm, SIGALRM);
    struct timespec zero = { 0, 0 };
    siginfo_t si;
    int n = 0;
    memset(got, 0, 2 * sizeof *got);
    while (sigtimedwait(&alrm, &si, &zero) == SIGALRM && n < 16) {
        if (n < 2)
            got[n] = si;
        n++;
    }
    return n;
}

// A SIGALRM handler's record of what it was given, in order.
static volatile int alarm_codes[4];
static volatile int alarm_runs;

static void record_alarm(int sig, siginfo_t *si, void *uc) {
    (void) sig;
    (void) uc;
    if (alarm_runs < 4)
        alarm_codes[alarm_runs] = si->si_code;
    alarm_runs++;
}

// rt_sigqueueinfo(2) to this process: SIGALRM, claiming to be POSIX timer
// `id`'s signal. Through the system call, since sigqueue() sets its own
// si_code.
static long forge_timer_alarm(int id) {
    siginfo_t si;
    memset(&si, 0, sizeof si);
    si.si_signo = SIGALRM;
    si.si_code = SI_TIMER;
    si.si_timerid = id;
    si.si_overrun = 0;
    si.si_value.sival_int = id;
    return syscall(SYS_rt_sigqueueinfo, getpid(), SIGALRM, &si);
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
            // timer_getoverrun is the count of the signal last TAKEN, and none
            // has been: the one waiting has ~199 on it, and AOK used to say so.
            ck("timer_getoverrun is 0 until a signal is taken",
               (long) timer_getoverrun(t), 0);
            siginfo_t si;
            struct timespec zero = { 0, 0 };
            int got = sigtimedwait(&set, &si, &zero);
            struct timespec taken;
            clock_gettime(CLOCK_MONOTONIC, &taken);
            ck("a signal is waiting", got == SIGRTMIN ? 1 : 0, 1);
            long overrun = got > 0 ? si.si_overrun : -1;
            // ~199 in a second of 5ms periods, but scheduling jitter moves it
            // by a period or two either way, so a range rather than a number.
            ck_range("  si_overrun counts the missed periods", overrun, 150, 260);
            ck("  and timer_getoverrun agrees", (long) timer_getoverrun(t), overrun);
            // The point of counting them: only ONE signal was queued. Two
            // hundred queued signals is what this looked like before, and it
            // is why si_overrun exists.
            //
            // Not "nothing more is waiting", though. Linux works the overruns
            // out from the clock as the signal is taken and only then arms the
            // next expiry, so nothing can queue behind it for most of a
            // period. AOK counts each expiry as its timer thread delivers it,
            // and the nap above ends right on the 200th boundary: that
            // expiry can be delivered a few microseconds after the take, as a
            // fresh signal -- the same expiry, counted there instead of in the
            // overrun. On the M4 iPad it was, in 1 round of 12, while host
            // timers ran a fifth of a period late (util/timer.h,
            // host_nanosleep_precise). So: one more at most, and one for each
            // period the draining itself spans. Never a backlog.
            long behind = 0, latched = overrun;
            while (behind < 1000 && sigtimedwait(&set, &si, &zero) == SIGRTMIN) {
                behind++;
                latched = si.si_overrun;
            }
            struct timespec drained;
            clock_gettime(CLOCK_MONOTONIC, &drained);
            long spanned = ((drained.tv_sec - taken.tv_sec) * 1000000000L +
                            (drained.tv_nsec - taken.tv_nsec)) / 5000000;
            ck_range("  and only one signal was queued", behind, 0, 1 + spanned);
            // ...and the count it reports stays that signal's while the next
            // one waits: Linux latches it as a signal is taken. A handler that
            // runs past a period and then asks how many it missed wants its
            // own count. AOK followed the queued signal instead -- 0 the
            // moment the next expiry was queued, then counting up.
            sigset_t pend;
            int queued = 0;
            for (int spin = 0; spin < 2000 && !queued; spin++) {
                sigpending(&pend);
                queued = sigismember(&pend, SIGRTMIN);
                if (!queued) {
                    struct timespec tick = { 0, 500000 };
                    nanosleep(&tick, NULL);
                }
            }
            ck("  the next expiry is queued behind it", queued, 1);
            // A few more periods, so the signal waiting has overruns of its
            // own: the count AOK reported, and never 0 or the one taken.
            struct timespec periods = { 0, 25000000 };
            nanosleep(&periods, NULL);
            ck("  and timer_getoverrun still reports the one taken",
               (long) timer_getoverrun(t), latched);
            // Disarm before restoring the mask, or the next expiry arrives
            // unblocked and kills the test.
            memset(&its, 0, sizeof its);
            timer_settime(t, 0, &its, NULL);
            ck("  and setting the timer forgets it", (long) timer_getoverrun(t), 0);
            timer_delete(t);
        }
        // Drop anything that slipped through while disarming.
        struct timespec zero = { 0, 0 };
        siginfo_t drop;
        while (sigtimedwait(&set, &drop, &zero) > 0) { }
        sigprocmask(SIG_SETMASK, &old, NULL);
    }

    // ---- an itimer's signal is SI_KERNEL, not a POSIX timer's -----------
    // Linux sends them as SEND_SIG_PRIV: si_code SI_KERNEL, si_pid and si_uid
    // 0 (which also leaves si_timerid and si_overrun, the same bytes, 0).
    {
        sigset_t set, old;
        sigemptyset(&set);
        sigaddset(&set, SIGALRM);
        sigaddset(&set, SIGVTALRM);
        sigaddset(&set, SIGPROF);
        sigprocmask(SIG_BLOCK, &set, &old);
        // alarm() and ITIMER_REAL are one timer, and it is holding the
        // watchdog. Every wait below is bounded; it goes back at the end.
        unsigned watchdog = alarm(0);
        struct itimerval iv;
        memset(&iv, 0, sizeof iv);
        iv.it_value.tv_usec = 20000;            // one-shot, 20ms
        siginfo_t si;
        int got;

        static const struct { int which, sig; const char *name; } kinds[] = {
            { ITIMER_REAL, SIGALRM, "setitimer(ITIMER_REAL)" },
            { -1, SIGALRM, "alarm(1)" },
            { ITIMER_VIRTUAL, SIGVTALRM, "setitimer(ITIMER_VIRTUAL)" },
            { ITIMER_PROF, SIGPROF, "setitimer(ITIMER_PROF)" },
        };
        for (unsigned i = 0; i < sizeof kinds / sizeof kinds[0]; i++) {
            if (kinds[i].which < 0)
                alarm(1);
            else
                setitimer(kinds[i].which, &iv, NULL);
            got = take_signal(kinds[i].sig, kinds[i].which == ITIMER_VIRTUAL ||
                              kinds[i].which == ITIMER_PROF, &si, 5.0);
            char lab[80];
            snprintf(lab, sizeof lab, "%s's signal came", kinds[i].name);
            ck(lab, got, kinds[i].sig);
            ck("  with si_code SI_KERNEL", got > 0 ? si.si_code : 0, SI_KERNEL);
            ck("  si_pid 0", got > 0 ? (long) si.si_pid : -1, 0);
            ck("  and si_uid 0", got > 0 ? (long) si.si_uid : -1, 0);
        }

        // What that code is FOR: POSIX timer 0 on SIGALRM, as timer_create
        // with no sigevent makes it, running alongside an itimer. AOK hands
        // out the lowest free id, and every timer above is gone, so this is
        // timer 0 there -- the one an itimer's SI_TIMER claimed to be. Linux
        // numbers them per process and has moved on, which changes nothing.
        timer_t t;
        sigset_t alrm;
        sigemptyset(&alrm);
        sigaddset(&alrm, SIGALRM);
        struct timespec zero = { 0, 0 };
        if (timer_create(CLOCK_MONOTONIC, NULL, &t) != 0) {
            failf("timer_create(CLOCK_MONOTONIC, NULL)", (uint64_t) errno, 0, 0, 0, 0, 0);
        } else {
            if (on_ish)
                ck("timer_create with no sigevent is timer 0", (long) (intptr_t) t, 0);
            // An itimer's SIGALRM is waiting when timer 0 expires on SIGALRM.
            // That expiry is not an overrun of the itimer's signal: AOK
            // counted it onto it, and took the pair as timer 0's with
            // si_overrun 1.
            setitimer(ITIMER_REAL, &iv, NULL);
            ck("an itimer's SIGALRM is waiting", wait_pending(SIGALRM, 5.0), 1);
            struct itimerspec its;
            memset(&its, 0, sizeof its);
            its.it_value.tv_nsec = 20000000;
            timer_settime(t, 0, &its, NULL);
            double start = clock_secs(CLOCK_MONOTONIC);
            do {
                struct timespec tick = { 0, 5000000 };
                nanosleep(&tick, NULL);
                timer_gettime(t, &its);
            } while ((its.it_value.tv_sec || its.it_value.tv_nsec) &&
                     clock_secs(CLOCK_MONOTONIC) - start < 5.0);
            ck("  then timer 0 expires", its.it_value.tv_sec || its.it_value.tv_nsec, 0);
            struct timespec settle = { 0, 50000000 };
            nanosleep(&settle, NULL);           // its signal, sent after that
            got = sigtimedwait(&alrm, &si, &zero);
            ck("  the SIGALRM taken first", got, SIGALRM);
            ck("  is the itimer's, SI_KERNEL", got > 0 ? si.si_code : 0, SI_KERNEL);
            // Linux queues timer 0's own signal behind it: a POSIX timer's
            // signal is queued whatever else of its number is pending
            // (send_sigqueue has no legacy_queue check); only its OWN signal,
            // still queued, takes an overrun instead. AOK dropped it, as it
            // drops a second instance of any standard signal.
            got = sigtimedwait(&alrm, &si, &zero);
            ck("  the SIGALRM taken second", got, SIGALRM);
            ck("  is timer 0's, SI_TIMER", got > 0 ? si.si_code : 0, SI_TIMER);
            ck("  naming timer 0", got > 0 ? (long) si.si_timerid : -1, (long) (intptr_t) t);
            ck("  with no overrun", got > 0 ? (long) si.si_overrun : -1, 0);
            ck("  and no third", sigtimedwait(&alrm, &si, &zero), -1);
            ck("  and timer 0 counted no overrun", (long) timer_getoverrun(t), 0);
            timer_delete(t);
        }

        // The rest of that rule, SIGALRM each time, with nothing else pending.
        // Only a POSIX timer's own signal is queued whatever is pending: any
        // other standard signal is dropped behind one of its number
        // (legacy_queue) -- an itimer's, and one that rt_sigqueueinfo sends
        // CLAIMING to be a timer's (si_code SI_TIMER), which is also never
        // mistaken for the timer's own and given its overruns.
        timer_t t1, t2;
        siginfo_t two[2];
        int n;
        if (timer_create(CLOCK_MONOTONIC, NULL, &t1) != 0 ||
                timer_create(CLOCK_MONOTONIC, NULL, &t2) != 0) {
            failf("timer_create(CLOCK_MONOTONIC, NULL) x2", (uint64_t) errno, 0, 0, 0, 0, 0);
        } else {
            long id1 = (long) (intptr_t) t1, id2 = (long) (intptr_t) t2;
            take_alarms(two);

            // A timer's, then an itimer's: the itimer's is dropped.
            expire_once(t1);
            itimer_expire_once();
            n = take_alarms(two);
            ck("a timer's SIGALRM, then an itimer's: signals", n, 1);
            ck("  the timer's", two[0].si_code, SI_TIMER);

            // kill(), then a timer's: the timer's is queued behind it.
            kill(getpid(), SIGALRM);
            expire_once(t1);
            n = take_alarms(two);
            ck("kill(SIGALRM), then a timer's: signals", n, 2);
            ck("  kill()'s first", two[0].si_code, SI_USER);
            ck("  then the timer's", two[1].si_code, SI_TIMER);
            ck("  naming it", (long) two[1].si_timerid, id1);

            // Two timers on SIGALRM: each queues its own.
            expire_once(t1);
            expire_once(t2);
            n = take_alarms(two);
            ck("two timers' SIGALRMs: signals", n, 2);
            ck("  the first timer's", two[0].si_code == SI_TIMER &&
               (long) two[0].si_timerid == id1 && two[0].si_overrun == 0, 1);
            ck("  then the second's", two[1].si_code == SI_TIMER &&
               (long) two[1].si_timerid == id2 && two[1].si_overrun == 0, 1);

            // rt_sigqueueinfo claiming SI_TIMER, behind kill()'s: dropped.
            kill(getpid(), SIGALRM);
            ck("rt_sigqueueinfo(SIGALRM, SI_TIMER) behind kill()'s",
               forge_timer_alarm((int) id1), 0);
            n = take_alarms(two);
            ck("  signals", n, 1);
            ck("  kill()'s", two[0].si_code, SI_USER);

            // ...and ahead of the timer it names: not its signal, so the
            // timer's expiry is queued, not counted onto it.
            ck("rt_sigqueueinfo(SIGALRM, SI_TIMER) naming a timer",
               forge_timer_alarm((int) id1), 0);
            expire_once(t1);
            n = take_alarms(two);
            ck("  then the timer expires: signals", n, 2);
            ck("  the forged one, no overrun counted on it",
               two[0].si_code == SI_TIMER && two[0].si_overrun == 0, 1);
            ck("  then the timer's own", two[1].si_code == SI_TIMER &&
               (long) two[1].si_timerid == id1 && two[1].si_overrun == 0, 1);

            // Two of one standard signal, taken each way a signal is taken.
            // The pending bit stays while one is left.
            sigset_t pend;
            itimer_expire_once();
            expire_once(t1);
            got = sigtimedwait(&alrm, &si, &zero);
            ck("two queued: sigtimedwait takes the itimer's",
               got == SIGALRM && si.si_code == SI_KERNEL, 1);
            sigpending(&pend);
            ck("  and SIGALRM is still pending", sigismember(&pend, SIGALRM), 1);
            got = sigtimedwait(&alrm, &si, &zero);
            ck("  then the timer's", got == SIGALRM && si.si_code == SI_TIMER, 1);
            sigpending(&pend);
            ck("  and then it is not", sigismember(&pend, SIGALRM), 0);

            // A signalfd reads both, in one read.
            int sfd = signalfd(-1, &alrm, SFD_NONBLOCK);
            if (sfd < 0) {
                failf("signalfd(SIGALRM)", (uint64_t) errno, 0, 0, 0, 0, 0);
            } else {
                itimer_expire_once();
                expire_once(t1);
                struct signalfd_siginfo ssi[3];
                memset(ssi, 0, sizeof ssi);
                ssize_t r = read(sfd, ssi, sizeof ssi);
                ck("two queued: a signalfd read returns", r, 2 * (long) sizeof ssi[0]);
                ck("  the itimer's, then the timer's",
                   (int) ssi[0].ssi_code == SI_KERNEL && (int) ssi[1].ssi_code == SI_TIMER, 1);
                sigpending(&pend);
                ck("  and SIGALRM is no longer pending", sigismember(&pend, SIGALRM), 0);
                close(sfd);
            }

            // A handler runs for each, when SIGALRM is let through.
            struct sigaction sa, oldsa;
            memset(&sa, 0, sizeof sa);
            sa.sa_sigaction = record_alarm;
            sa.sa_flags = SA_SIGINFO;
            sigaction(SIGALRM, &sa, &oldsa);
            itimer_expire_once();
            expire_once(t1);
            alarm_runs = 0;
            sigprocmask(SIG_UNBLOCK, &alrm, NULL);
            sigprocmask(SIG_BLOCK, &alrm, NULL);
            ck("two queued: the handler runs", alarm_runs, 2);
            ck("  for the itimer's, then the timer's",
               alarm_codes[0] == SI_KERNEL && alarm_codes[1] == SI_TIMER, 1);
            sigpending(&pend);
            ck("  and SIGALRM is no longer pending", sigismember(&pend, SIGALRM), 0);
            sigaction(SIGALRM, &oldsa, NULL);

            timer_delete(t1);
            timer_delete(t2);
        }

        // Timer 0 latches its overruns as its signal is taken; taking an
        // itimer's SIGALRM afterwards must leave that alone. AOK took it for
        // timer 0's and latched its 0. On the CPU clock so that no expiry
        // follows while nothing runs: 10ms, then every 100ms, and 350ms spent
        // blocked is three overruns with 60ms to spare.
        if (timer_create(CLOCK_PROCESS_CPUTIME_ID, NULL, &t) != 0) {
            failf("timer_create(CLOCK_PROCESS_CPUTIME_ID, NULL)", (uint64_t) errno, 0, 0, 0, 0, 0);
        } else {
            if (on_ish)
                ck("timer_create with no sigevent is timer 0 again", (long) (intptr_t) t, 0);
            struct itimerspec its;
            memset(&its, 0, sizeof its);
            its.it_value.tv_nsec = 10000000;
            its.it_interval.tv_nsec = 100000000;
            ck("arm timer 0 on the process CPU clock", timer_settime(t, 0, &its, NULL), 0);
            burn_cpu(0.35);
            got = sigtimedwait(&alrm, &si, &zero);
            ck("its SIGALRM is waiting", got, SIGALRM);
            ck("  as SI_TIMER", got > 0 ? si.si_code : 0, SI_TIMER);
            long latched = timer_getoverrun(t);
            ck_range("  taking it latches its overruns", latched, 1, 10);
            setitimer(ITIMER_REAL, &iv, NULL);
            got = take_signal(SIGALRM, 0, &si, 5.0);
            ck("then an itimer's SIGALRM is taken", got == SIGALRM && si.si_code == SI_KERNEL, 1);
            ck("  and timer 0's timer_getoverrun still says so",
               (long) timer_getoverrun(t), latched);
            timer_delete(t);
        }

        memset(&iv, 0, sizeof iv);
        setitimer(ITIMER_REAL, &iv, NULL);
        setitimer(ITIMER_VIRTUAL, &iv, NULL);
        setitimer(ITIMER_PROF, &iv, NULL);
        while (sigtimedwait(&set, &si, &zero) > 0) { }
        sigprocmask(SIG_SETMASK, &old, NULL);
        if (watchdog != 0)
            alarm(watchdog);
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
        // A well-formed request is refused, not silently accepted: AOK cannot
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
