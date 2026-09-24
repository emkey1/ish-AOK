// checkpoint_timers.c -- timers, pending signals and interrupted sleeps across
// a checkpoint. Driven by checkpoint_timers.sh.
//
// A checkpoint image carried none of a process's timers: POSIX timers
// (timer_create), interval timers (setitimer) and alarm() simply were not in
// it, so a restored process that had armed one never got its signal -- a
// program using SIGALRM as a timeout waited for ever. And a RELATIVE sleep the
// freeze had interrupted was re-executed with its original argument: `sleep 5`
// frozen 3 s in slept 5 s more after the restore, where Linux sleeps the 2 s
// that were left. The poll family's timeouts did the same.
//
// What Linux does across a hibernation is the reference: every timer and
// timeout keeps its deadline on its own clock. CLOCK_MONOTONIC does not count
// the time the machine was stopped, so a MONOTONIC deadline -- and a relative
// timer or sleep, which Linux keeps on MONOTONIC even when it names
// CLOCK_REALTIME -- comes due when MONOTONIC reaches it. CLOCK_BOOTTIME and an
// absolute CLOCK_REALTIME arming DO count the stop, and come due that much
// sooner (at once, if the stop was longer). A CPU-time timer has CPU time
// left, which only running consumes. The restore continues the guest's clocks
// (checkpoint_clock.sh checks that), so every check here is a deadline read
// on the clock it names: the arming instant plus the duration, never "time
// after the resume", which would need the restore's own duration.
//
// Pending signals are checked too, because they are how a timer delivers: a
// signal queued but not taken when the image was written -- one of them a
// timer's, with its overrun count -- must still be there, with its siginfo.
// And "never" must stay never: a timer, a timerfd and a sleep set for
// TIME_T_MAX, which overflowed the arithmetic once and came back due.
//
// The witness for "when did the machine stop and come back" is a spinner
// thread reading CLOCK_REALTIME every few milliseconds: its largest gap IS the
// stop, measured on a clock the restore does not continue. It decides only
// whether the stop landed where the checks need it (INCONCLUSIVE otherwise);
// it grades nothing.
//
//     probe after     -- arms everything, then waits for an external save
//                        (ISH_CHECKPOINT_AFTER) and a restore
//     probe suspend   -- the same, and asks for the suspend itself, from a child
//     probe race      -- asks for the suspend while two expiries are being
//                        delivered; needs ISH_TEST_TIMER_FIRE_DELAY_MS in the
//                        saving run (see race_main)
//
// Prints "OK <check>" / "FAIL <check>: why" lines, then TIMERS-PROBE-DONE.
// Exit 4 means the stop did not land inside every wait under test.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/select.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef CLOCK_BOOTTIME
#define CLOCK_BOOTTIME 7
#endif
#ifndef sigev_notify_thread_id
#define sigev_notify_thread_id _sigev_un._tid
#endif
#ifndef SIGEV_THREAD_ID
#define SIGEV_THREAD_ID 4
#endif
#ifndef TFD_TIMER_CANCEL_ON_SET
#define TFD_TIMER_CANCEL_ON_SET (1 << 1)
#endif

// When the external save comes (ISH_CHECKPOINT_AFTER=8 in the driver) or the
// probe asks for the suspend: long enough after the armings that each wait is
// well under way, short enough that every one still has seconds left.
#define SUSPEND_AT_MONO 7.0
// A wait on MONOTONIC: ~3.5 s left at a stop 6.5 s in. A broken restore that
// starts it again waits the whole 10 s after the resume.
#define D_MONO 10.0
// A wait on BOOTTIME or an absolute REALTIME arming, which counts the stop:
// longer, so it comes due after the resume, and sooner than MONOTONIC's.
#define D_COUNTS_STOP 12.0
#define PERIOD 0.5
#define OVERRUN_PERIOD 0.02
// A periodic timerfd on BOOTTIME, never read: the stop passes many of its
// expiries, and every one must be counted.
#define OVERDUE_PERIOD 0.1
// CPU time, consumed by the burner at up to one second a second.
#define D_CPU 8.0
// How late a deadline may be met. The failure this is looking for is seconds.
#define LATE 1.0
// How late a periodic timer's expiries after its first may be taken, each
// against its place on the timer's grid.
#define GRID_LATE 0.1
// CPU the main thread burns before anything is armed. It is the task whose
// record carries the process's timers, and the save is made on another host
// thread: a CPU clock read through `current` there gets the saver's time for
// this thread's, and ITIMER_PROF would come back this much late.
#define MAIN_CPU 2.5

static int failures;

__attribute__((format(printf, 3, 4)))
static void check(const char *what, int ok, const char *fmt, ...) {
    char detail[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof(detail), fmt, ap);
    va_end(ap);
    printf("%s %s: %s\n", ok ? "OK" : "FAIL", what, detail);
    if (!ok)
        failures++;
}

static double now(clockid_t c) {
    struct timespec t;
    clock_gettime(c, &t);
    return (double) t.tv_sec + (double) t.tv_nsec / 1e9;
}

static struct timespec to_ts(double s) {
    struct timespec t;
    if (s < 0)
        s = 0;
    t.tv_sec = (time_t) s;
    t.tv_nsec = (long) ((s - (double) t.tv_sec) * 1e9);
    if (t.tv_nsec >= 1000000000L) {
        t.tv_sec++;
        t.tv_nsec -= 1000000000L;
    }
    return t;
}

static double ts_d(struct timespec t) {
    return (double) t.tv_sec + (double) t.tv_nsec / 1e9;
}

static int read_file(const char *path, char *buf, size_t size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    size_t at = 0;
    ssize_t n;
    while (at + 1 < size && (n = read(fd, buf + at, size - 1 - at)) != 0) {
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        at += (size_t) n;
    }
    close(fd);
    buf[at] = '\0';
    return (int) at;
}

static int restored(void) {
    static char buf[8192];
    if (read_file("/proc/ish/checkpoint", buf, sizeof(buf)) <= 0)
        return 0;
    return strstr(buf, "restored        yes") != NULL;
}

static pid_t gettid_(void) {
    return (pid_t) syscall(SYS_gettid);
}

// ---- the witness ----------------------------------------------------------

static pthread_mutex_t spin_lock = PTHREAD_MUTEX_INITIALIZER;
static double spin_gap, spin_before, spin_after;
static double spin_boot_after;

static void *spinner(void *arg) {
    (void) arg;
    double last = now(CLOCK_REALTIME);
    for (;;) {
        // select, not nanosleep: a relative sleep is one of the things under
        // test, and a witness must not share what it witnesses.
        struct timeval tv = {0, 5000};
        select(0, NULL, NULL, NULL, &tv);
        double r = now(CLOCK_REALTIME);
        double b = now(CLOCK_BOOTTIME);
        pthread_mutex_lock(&spin_lock);
        if (r - last > spin_gap) {
            spin_gap = r - last;
            spin_before = last;
            spin_after = r;
            spin_boot_after = b;
        }
        pthread_mutex_unlock(&spin_lock);
        last = r;
    }
    return NULL;
}

// ---- the sleeps: one thread each, blocked in one raw syscall -------------

enum sleep_kind {
    SL_NANOSLEEP, SL_CNS_MONO, SL_CNS_REAL, SL_CNS_BOOT,
    SL_POLL, SL_PPOLL, SL_SELECT, SL_PSELECT, SL_EPOLL_WAIT, SL_EPOLL_PWAIT,
    SL_COUNT
};
static const char *sleep_name[SL_COUNT] = {
    "nanosleep", "clock_nanosleep-monotonic", "clock_nanosleep-realtime",
    "clock_nanosleep-boottime", "poll", "ppoll", "select", "pselect6",
    "epoll_wait", "epoll_pwait",
};
struct sleeper {
    enum sleep_kind kind;
    int available;
    double dur;
    volatile int started, done;
    double m0, b0, r0;          // just before the call
    double m1, b1, r1;          // just after it
    long rc;
    int err;
};
static struct sleeper sleepers[SL_COUNT];

static void *sleeper_thread(void *arg) {
    struct sleeper *s = arg;
    struct timespec ts = to_ts(s->dur);
    long ms = (long) (s->dur * 1000);
    int ep = -1;
    if (s->kind == SL_EPOLL_WAIT || s->kind == SL_EPOLL_PWAIT)
        ep = epoll_create1(0);
    s->m0 = now(CLOCK_MONOTONIC);
    s->b0 = now(CLOCK_BOOTTIME);
    s->r0 = now(CLOCK_REALTIME);
    s->started = 1;
    long rc = -1;
    struct epoll_event ev;
    switch (s->kind) {
    case SL_NANOSLEEP:
        rc = syscall(SYS_nanosleep, &ts, NULL); break;
    case SL_CNS_MONO:
        rc = syscall(SYS_clock_nanosleep, CLOCK_MONOTONIC, 0, &ts, NULL); break;
    case SL_CNS_REAL:
        rc = syscall(SYS_clock_nanosleep, CLOCK_REALTIME, 0, &ts, NULL); break;
    case SL_CNS_BOOT:
        rc = syscall(SYS_clock_nanosleep, CLOCK_BOOTTIME, 0, &ts, NULL); break;
#ifdef SYS_poll
    case SL_POLL:
        rc = syscall(SYS_poll, NULL, 0, (int) ms); break;
#endif
    case SL_PPOLL:
        rc = syscall(SYS_ppoll, NULL, 0, &ts, NULL, 8); break;
#ifdef SYS_select
    case SL_SELECT: {
        struct timeval tv = {(time_t) s->dur, 0};
        rc = syscall(SYS_select, 0, NULL, NULL, NULL, &tv); break; }
#endif
    case SL_PSELECT:
        rc = syscall(SYS_pselect6, 0, NULL, NULL, NULL, &ts, NULL); break;
#ifdef SYS_epoll_wait
    case SL_EPOLL_WAIT:
        rc = syscall(SYS_epoll_wait, ep, &ev, 1, (int) ms); break;
#endif
    case SL_EPOLL_PWAIT:
        rc = syscall(SYS_epoll_pwait, ep, &ev, 1, (int) ms, NULL, 8); break;
    default:
        break;
    }
    s->err = rc < 0 ? errno : 0;
    s->m1 = now(CLOCK_MONOTONIC);
    s->b1 = now(CLOCK_BOOTTIME);
    s->r1 = now(CLOCK_REALTIME);
    s->rc = rc;
    s->done = 1;
    // Parked, not returned. musl's pthread_exit blocks every signal while the
    // thread goes, and these all come due within milliseconds of the alarm()
    // child's death: a sleeper exiting used to make the kernel queue that
    // child's SIGCHLD, which would otherwise be ignored, and the queued signal
    // ended a sibling's wait with EINTR. That was a signal bug of its own, and
    // is fixed: only the thread a signal is sent to decides whether an ignored
    // one is queued, and one that is queued runs no handler, so the call
    // restarts, as on Linux (tests/manual/signal_ignored_restart.c). The
    // sleepers still park, so this test depends on neither.
    for (;;)
        pause();
    return NULL;
}

static int sleep_available(enum sleep_kind k) {
    switch (k) {
#ifndef SYS_poll
    case SL_POLL: return 0;
#endif
#ifndef SYS_select
    case SL_SELECT: return 0;
#endif
#ifndef SYS_epoll_wait
    case SL_EPOLL_WAIT: return 0;
#endif
    default: return 1;
    }
}

// A sleep asked for TIME_T_MAX, the way `sleep infinity` asks, which must still
// be asleep after the restore rather than wake with an overflowed deadline.
static volatile int forever_started, forever_done;
static void *forever_sleeper(void *arg) {
    (void) arg;
    struct timespec ts = {INT64_MAX, 999999999};
    forever_started = 1;
    syscall(SYS_nanosleep, &ts, NULL);
    forever_done = 1;
    for (;;)
        pause();
    return NULL;
}

// ---- the CPU burner --------------------------------------------------------
//
// Burns CPU for the CPU-time timers, and keeps its own account of what it has
// burned. A restored thread is a new host thread, so the CPU clocks start near
// zero again after a restore; the burner notices the drop and adds what it had
// before it to what it burns after, which is the CPU time the timers count.

static volatile int burn_go = 1;
static pthread_mutex_t burn_lock = PTHREAD_MUTEX_INITIALIZER;
static double burn_thread_before, burn_thread_last;    // own thread CPU
static double burn_proc_before, burn_proc_last;        // process CPU
static double burn_thread_base, burn_proc_base;        // at the armings
static timer_t tcpu_timer;
static volatile int tcpu_armed;
static pid_t main_tid;

#define SIG_TCPU (SIGRTMIN + 7)

static void *burner(void *arg) {
    (void) arg;
    // CLOCK_THREAD_CPUTIME_ID, of THIS thread, signalled to the main thread.
    struct sigevent sev = {0};
    sev.sigev_notify = SIGEV_THREAD_ID;
    sev.sigev_signo = SIG_TCPU;
    sev.sigev_notify_thread_id = main_tid;
    sev.sigev_value.sival_int = 7007;
    double t0 = now(CLOCK_THREAD_CPUTIME_ID), p0 = now(CLOCK_PROCESS_CPUTIME_ID);
    pthread_mutex_lock(&burn_lock);
    burn_thread_base = burn_thread_last = t0;
    burn_proc_base = burn_proc_last = p0;
    pthread_mutex_unlock(&burn_lock);
    if (timer_create(CLOCK_THREAD_CPUTIME_ID, &sev, &tcpu_timer) == 0) {
        struct itimerspec its = {.it_value = to_ts(D_CPU)};
        if (timer_settime(tcpu_timer, 0, &its, NULL) == 0)
            tcpu_armed = 1;
    }
    volatile unsigned long x = 0;
    while (burn_go) {
        for (int i = 0; i < 200000; i++)
            x += (unsigned long) i * 2654435761u;
        double t = now(CLOCK_THREAD_CPUTIME_ID), p = now(CLOCK_PROCESS_CPUTIME_ID);
        pthread_mutex_lock(&burn_lock);
        // A clock that went BACKWARD is a restore: bank what was burned
        // before it.
        if (t + 0.5 < burn_thread_last) {
            burn_thread_before += burn_thread_last - burn_thread_base;
            burn_thread_base = 0;
        }
        if (p + 0.5 < burn_proc_last) {
            burn_proc_before += burn_proc_last - burn_proc_base;
            burn_proc_base = 0;
        }
        burn_thread_last = t;
        burn_proc_last = p;
        pthread_mutex_unlock(&burn_lock);
    }
    return NULL;
}

// What the burner has burned since the armings, across a restore.
static double burned_thread(void) {
    pthread_mutex_lock(&burn_lock);
    double v = burn_thread_before + burn_thread_last - burn_thread_base;
    pthread_mutex_unlock(&burn_lock);
    return v;
}
static double burned_proc(void) {
    pthread_mutex_lock(&burn_lock);
    double v = burn_proc_before + burn_proc_last - burn_proc_base;
    pthread_mutex_unlock(&burn_lock);
    return v;
}

// ---- the timers ------------------------------------------------------------

#define SIG_MONO     (SIGRTMIN + 1)   // MONOTONIC, relative, to the main thread
#define SIG_REAL_REL (SIGRTMIN + 2)   // REALTIME, relative: Linux keeps it on MONOTONIC
#define SIG_REAL_ABS (SIGRTMIN + 3)   // REALTIME, TIMER_ABSTIME: a wall-clock instant
#define SIG_BOOT     (SIGRTMIN + 4)   // BOOTTIME, relative: counts the stop
#define SIG_PERIODIC (SIGRTMIN + 5)   // MONOTONIC, periodic
#define SIG_OVERRUN  (SIGRTMIN + 6)   // MONOTONIC, fast and never taken until the end
#define SIG_NEVER    (SIGRTMIN + 8)   // armed at TIME_T_MAX: must never come
#define SIG_RACE     (SIGRTMIN + 9)   // the race leg's one-shot

// Every arrival of every collected signal, with when it came on each clock and
// what it carried. Graded afterwards against the witness, which is the only
// thing that knows where the stop fell.
#define MAX_ARRIVALS 16
struct arrival {
    double m, b, r;
    double cpu;                  // SIG_TCPU/SIGPROF: CPU burned when it came
    int code, timerid, value, overrun;
};
struct arrivals {
    int count;
    struct arrival a[MAX_ARRIVALS];
};
#define NSIG_ 65
static struct arrivals arrivals[NSIG_];

// The first arrival at or after the realtime instant `since`, or NULL.
static const struct arrival *first_after(int sig, double since) {
    const struct arrivals *l = &arrivals[sig];
    for (int i = 0; i < l->count && i < MAX_ARRIVALS; i++)
        if (l->a[i].r >= since)
            return &l->a[i];
    return NULL;
}
static int count_after(int sig, double since) {
    const struct arrivals *l = &arrivals[sig];
    int n = 0;
    for (int i = 0; i < l->count && i < MAX_ARRIVALS; i++)
        n += l->a[i].r >= since;
    return n;
}
static int any_before(int sig, double until) {
    const struct arrivals *l = &arrivals[sig];
    return l->count > 0 && l->a[0].r < until;
}


static timer_t mk_timer(clockid_t c, int notify, int sig, int value) {
    struct sigevent sev = {0};
    sev.sigev_notify = notify;
    sev.sigev_signo = sig;
    sev.sigev_value.sival_int = value;
    if (notify == SIGEV_THREAD_ID)
        sev.sigev_notify_thread_id = main_tid;
    timer_t t = 0;
    if (timer_create(c, &sev, &t) != 0) {
        printf("SETUP timer_create(%d) failed: %s\n", (int) c, strerror(errno));
        exit(2);
    }
    return t;
}

static void arm(timer_t t, int flags, double value, double interval) {
    struct itimerspec its = {.it_value = to_ts(value), .it_interval = to_ts(interval)};
    if (timer_settime(t, flags, &its, NULL) != 0) {
        printf("SETUP timer_settime failed: %s\n", strerror(errno));
        exit(2);
    }
}

static int timer_id_of(timer_t t) {
    return (int) (intptr_t) t;
}


// probe race -- the save lands while two expiries are being delivered.
//
// The driver holds every expiry between its timer deciding to fire and the
// signal being sent (ISH_TEST_TIMER_FIRE_DELAY_MS), a window of microseconds
// otherwise, and this asks for the suspend inside it. What the save reads of a
// timer there is a one-shot that has fired and a signal not yet queued, so an
// image that took the timer's word for it had neither: the expiry was lost, and
// a program waiting on it -- alarm() as a timeout -- waited for ever. Both
// signals have to arrive after the restore.
static int race_main(void) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIG_RACE);
    sigaddset(&set, SIGALRM);
    sigprocmask(SIG_BLOCK, &set, NULL);
    nanosleep(&(struct timespec) {1, 0}, NULL);
    timer_t t = mk_timer(CLOCK_MONOTONIC, SIGEV_SIGNAL, SIG_RACE, 1009);
    arm(t, 0, 0.3, 0);
    struct itimerval itv = {.it_value = {0, 300000}};
    setitimer(ITIMER_REAL, &itv, NULL);
    // Due at 0.3 s, delivered at 0.3 s plus the hold: ask at 0.5 s.
    nanosleep(&(struct timespec) {0, 500000000}, NULL);
    pid_t asker = fork();
    if (asker == 0) {
        int fd = open("/proc/ish/checkpoint", O_WRONLY);
        if (fd < 0 || write(fd, "suspend\n", 8) != 8)
            _exit(2);
        close(fd);
        for (;;)            // see the main leg's asker
            pause();
    }
    printf("RACE-START timer %d\n", timer_id_of(t));
    for (int i = 0; !restored(); i++) {
        if (i > 600) {
            printf("NO-RESTORE\n");
            _exit(3);
        }
        nanosleep(&(struct timespec) {0, 50000000}, NULL);
    }
    int got_race = 0, got_alrm = 0, race_value = 0, race_id = -1;
    for (int i = 0; i < 2; i++) {
        siginfo_t si;
        struct timespec wait = {3, 0};
        int sig = sigtimedwait(&set, &si, &wait);
        if (sig == SIG_RACE) {
            got_race = 1;
            race_value = si.si_value.sival_int;
            race_id = si.si_timerid;
        } else if (sig == SIGALRM) {
            got_alrm = 1;
        }
    }
    check("race-posix", got_race && race_value == 1009 && race_id == timer_id_of(t),
          "%s (value %d, timerid %d)", got_race ? "its signal came" : "its signal was lost",
          race_value, race_id);
    check("race-itimer", got_alrm, "ITIMER_REAL's SIGALRM %s", got_alrm ? "came" : "was lost");
    kill(asker, SIGKILL);
    waitpid(asker, NULL, 0);
    printf("TIMERS-PROBE-DONE failures=%d\n", failures);
    _exit(failures == 0 ? 0 : 1);
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "after";
    if (strcmp(mode, "race") == 0) {
        setvbuf(stdout, NULL, _IONBF, 0);
        return race_main();
    }
    int suspend = strcmp(mode, "suspend") == 0;
    // Unbuffered: a line still in stdio's buffer at the save is inside the
    // image, and the restored run would print it a second time.
    setvbuf(stdout, NULL, _IONBF, 0);
    main_tid = gettid_();

    // Every signal a timer or the checks deliver, blocked before any thread
    // exists so that only the main thread's sigtimedwait takes them.
    sigset_t all, collect;
    sigemptyset(&all);
    int sigs[] = {SIG_MONO, SIG_REAL_REL, SIG_REAL_ABS, SIG_BOOT, SIG_PERIODIC,
                  SIG_OVERRUN, SIG_TCPU, SIG_NEVER, SIGALRM, SIGPROF, SIGUSR2, SIGURG,
                  SIGPWR};
    for (unsigned i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++)
        sigaddset(&all, sigs[i]);
    sigprocmask(SIG_BLOCK, &all, NULL);
    // Collected as they come. SIG_OVERRUN, SIGUSR2 and SIGURG are left
    // pending, to be looked for after the restore.
    collect = all;
    sigdelset(&collect, SIG_OVERRUN);
    sigdelset(&collect, SIGPWR);
    sigdelset(&collect, SIGUSR2);
    sigdelset(&collect, SIGURG);

    // Settle past the dynamic loading, so the armings are made by a process
    // that is really running.
    nanosleep(&(struct timespec) {1, 0}, NULL);
    // See MAIN_CPU.
    for (volatile unsigned long x = 0; now(CLOCK_THREAD_CPUTIME_ID) < MAIN_CPU; )
        for (int i = 0; i < 100000; i++)
            x += (unsigned long) i;

    pthread_t th;
    pthread_create(&th, NULL, spinner, NULL);

    // ---- alarm(), the reported case: a child using SIGALRM as a timeout.
    // It has no handler, so SIGALRM kills it -- on time, or never.
    double alarm_m0 = now(CLOCK_MONOTONIC);
    pid_t alarm_child = fork();
    if (alarm_child == 0) {
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        alarm((unsigned) D_MONO);
        for (;;)
            pause();
    }

    // ---- POSIX timers, on each kind of clock.
    double m0 = now(CLOCK_MONOTONIC), b0 = now(CLOCK_BOOTTIME), r0 = now(CLOCK_REALTIME);
    timer_t t_mono = mk_timer(CLOCK_MONOTONIC, SIGEV_THREAD_ID, SIG_MONO, 1001);
    timer_t t_real_rel = mk_timer(CLOCK_REALTIME, SIGEV_SIGNAL, SIG_REAL_REL, 1002);
    timer_t t_real_abs = mk_timer(CLOCK_REALTIME, SIGEV_SIGNAL, SIG_REAL_ABS, 1003);
    timer_t t_boot = mk_timer(CLOCK_BOOTTIME, SIGEV_SIGNAL, SIG_BOOT, 1004);
    timer_t t_periodic = mk_timer(CLOCK_MONOTONIC, SIGEV_SIGNAL, SIG_PERIODIC, 1005);
    timer_t t_overrun = mk_timer(CLOCK_MONOTONIC, SIGEV_SIGNAL, SIG_OVERRUN, 1006);
    timer_t t_none = mk_timer(CLOCK_MONOTONIC, SIGEV_NONE, 0, 0);
    timer_t t_pcpu = mk_timer(CLOCK_PROCESS_CPUTIME_ID, SIGEV_NONE, 0, 0);
    // "Never", both ways it is asked for: a POSIX timer at TIME_T_MAX, and a
    // timerfd armed the way systemd arms its clock-change watch.
    timer_t t_never = mk_timer(CLOCK_MONOTONIC, SIGEV_SIGNAL, SIG_NEVER, 1008);
    int tfd_never = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK);
    int tfd_overdue = timerfd_create(CLOCK_BOOTTIME, TFD_NONBLOCK);
    arm(t_mono, 0, D_MONO, 0);
    arm(t_real_rel, 0, D_MONO, 0);
    arm(t_real_abs, TIMER_ABSTIME, r0 + D_COUNTS_STOP, 0);
    arm(t_boot, 0, D_COUNTS_STOP, 0);
    arm(t_periodic, 0, D_MONO, PERIOD);
    arm(t_overrun, 0, OVERRUN_PERIOD, OVERRUN_PERIOD);
    arm(t_none, 0, D_MONO + 20, 3.0);
    arm(t_pcpu, 0, 1000.0, 0);
    double od_b0 = now(CLOCK_BOOTTIME);
    {
        struct itimerspec its = {.it_value = to_ts(OVERDUE_PERIOD),
                                 .it_interval = to_ts(OVERDUE_PERIOD)};
        if (timerfd_settime(tfd_overdue, 0, &its, NULL) != 0) {
            printf("SETUP arming the BOOTTIME timerfd failed: %s\n", strerror(errno));
            exit(2);
        }
    }
    double od_b1 = now(CLOCK_BOOTTIME);
    {
        struct itimerspec forever = {.it_value = {INT64_MAX, 0}};
        if (timer_settime(t_never, 0, &forever, NULL) != 0 ||
                timerfd_settime(tfd_never, TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET,
                                &forever, NULL) != 0) {
            printf("SETUP arming at TIME_T_MAX failed: %s\n", strerror(errno));
            exit(2);
        }
    }

    // ---- setitimer: ITIMER_REAL periodic, ITIMER_PROF on the burner's CPU.
    struct itimerval itv = {.it_value = {(time_t) D_MONO, 0},
                            .it_interval = {0, (suseconds_t) (PERIOD * 1e6)}};
    setitimer(ITIMER_REAL, &itv, NULL);
    pthread_create(&th, NULL, burner, NULL);
    while (!tcpu_armed)
        nanosleep(&(struct timespec) {0, 1000000}, NULL);
    double prof_base = burned_proc();
    struct itimerval ptv = {.it_value = {(time_t) D_CPU, 0}};
    setitimer(ITIMER_PROF, &ptv, NULL);

    // ---- signals that are pending, not taken, when the image is written.
    union sigval v = {.sival_int = 0xC0FFEE};
    sigqueue(getpid(), SIGUSR2, v);
    kill(getpid(), SIGURG);
    // Two of one standard signal: kill()'s SIGPWR, and a periodic POSIX
    // timer's queued behind it -- a timer's own signal is queued whatever is
    // pending -- every later expiry counted on the timer's.
    kill(getpid(), SIGPWR);
    timer_t t_pwr = mk_timer(CLOCK_MONOTONIC, SIGEV_SIGNAL, SIGPWR, 1010);
    arm(t_pwr, 0, OVERRUN_PERIOD, OVERRUN_PERIOD);

    // ---- the sleeps.
    for (int k = 0; k < SL_COUNT; k++) {
        struct sleeper *s = &sleepers[k];
        s->kind = (enum sleep_kind) k;
        s->available = sleep_available(s->kind);
        s->dur = k == SL_CNS_BOOT ? D_COUNTS_STOP : D_MONO;
        if (s->available)
            pthread_create(&th, NULL, sleeper_thread, s);
    }
    pthread_create(&th, NULL, forever_sleeper, NULL);
    for (int k = 0; k < SL_COUNT; k++)
        while (sleepers[k].available && !sleepers[k].started)
            nanosleep(&(struct timespec) {0, 1000000}, NULL);
    while (!forever_started)
        nanosleep(&(struct timespec) {0, 1000000}, NULL);
    // Give the last sleeper time to get from its reading into its call.
    nanosleep(&(struct timespec) {0, 100000000}, NULL);
    double r_setup = now(CLOCK_REALTIME);

    printf("PROBE-START %s mono=%.3f alarm_child=%d timer_ids=%d,%d,%d,%d\n", mode, m0,
           (int) alarm_child, timer_id_of(t_mono), timer_id_of(t_real_rel),
           timer_id_of(t_boot), timer_id_of(t_periodic));

    pid_t asker = -1;
    int alarm_status = -1;
    double alarm_m1 = -1;
    int restored_seen = 0;
    double restored_m = 0;
    double last_probe = 0;
    // timer_getoverrun on the timer whose signal is never taken: the count on
    // that queued signal, sampled until the restore, then the timer stopped.
    // The queued signal has to come back holding at least what it held at the
    // stop; a fresh one queued after the restore would hold only a handful.
    struct { double r; int ov; } ov_ring[64];
    unsigned ov_n = 0;
    double r_ov_stopped = 0;
    for (;;) {
        // The collection loop: every timer's signal, with when it came.
        siginfo_t si;
        struct timespec tick = {0, 20000000};
        int sig = sigtimedwait(&collect, &si, &tick);
        double m = now(CLOCK_MONOTONIC), b = now(CLOCK_BOOTTIME), r = now(CLOCK_REALTIME);
        if (sig > 0 && sig < NSIG_) {
            struct arrivals *l = &arrivals[sig];
            if (l->count < MAX_ARRIVALS) {
                struct arrival *a = &l->a[l->count];
                *a = (struct arrival) {
                    .m = m, .b = b, .r = r,
                    .code = si.si_code, .timerid = si.si_timerid,
                    .value = si.si_value.sival_int, .overrun = si.si_overrun,
                };
                if (sig == SIG_TCPU)
                    a->cpu = burned_thread();
                else if (sig == SIGPROF)
                    a->cpu = burned_proc() - prof_base;
            }
            l->count++;
        }
        if (alarm_status < 0) {
            int st;
            if (waitpid(alarm_child, &st, WNOHANG) == alarm_child) {
                alarm_status = st;
                alarm_m1 = m;
            }
        }
        if (!restored_seen) {
            int ov = timer_getoverrun(t_overrun);
            if (ov >= 0)
                ov_ring[ov_n++ % 64] = (typeof(ov_ring[0])) {r, ov};
            if (suspend && asker < 0 && m >= SUSPEND_AT_MONO) {
                asker = fork();
                if (asker == 0) {
                    int fd = open("/proc/ish/checkpoint", O_WRONLY);
                    if (fd < 0 || write(fd, "suspend\n", 8) != 8)
                        _exit(2);
                    close(fd);
                    // It does not exit. It takes the checkpoint, so the image
                    // holds this process wherever it had got to -- musl's
                    // fork(), in the parent, with every signal blocked -- and an
                    // exit right after the resume queues a SIGCHLD for the
                    // parent that would otherwise have been ignored. That ended
                    // a sleeper's wait with EINTR, a signal bug of its own: no
                    // handler runs, so the call restarts, as on Linux, and now
                    // does here too (tests/manual/signal_ignored_restart.c).
                    // The asker still parks, so none of it is in what this
                    // measures.
                    for (;;)
                        pause();
                }
            }
            if (m - last_probe >= 0.2) {
                last_probe = m;
                if (restored()) {
                    restored_seen = 1;
                    restored_m = m;
                    struct itimerspec stop = {0};
                    timer_settime(t_overrun, 0, &stop, NULL);
                    timer_settime(t_pwr, 0, &stop, NULL);
                    r_ov_stopped = now(CLOCK_REALTIME);
                }
            }
            if (m > 40) {
                printf("NO-RESTORE by monotonic %.1f\n", m);
                _exit(3);
            }
            continue;
        }
        // Everything on a clock is due within D_COUNTS_STOP of the armings;
        // the CPU-time timers need the burner to finish its D_CPU, which a
        // busy host may starve, so they get longer.
        int cpu_waiting = (arrivals[SIG_TCPU].count == 0 || arrivals[SIGPROF].count == 0) &&
                          m < restored_m + 20;
        if (m >= m0 + D_COUNTS_STOP + LATE + 1.0 && !cpu_waiting)
            break;
    }
    burn_go = 0;

    pthread_mutex_lock(&spin_lock);
    double gap = spin_gap, stopped_at = spin_before, resumed_at = spin_after;
    double boot_resumed = spin_boot_after;
    pthread_mutex_unlock(&spin_lock);
    printf("stop: %.3f s (realtime %.3f..%.3f), boottime at the resume %.3f\n",
           gap, stopped_at, resumed_at, boot_resumed);

    // Every wait under test has to have been under way when the machine
    // stopped, or its check proves nothing: set up before the stop, and not
    // over before it.
    int timer_sigs[] = {SIG_MONO, SIG_REAL_REL, SIG_REAL_ABS, SIG_BOOT, SIG_PERIODIC,
                        SIG_TCPU, SIGALRM, SIGPROF};
    if (stopped_at < r_setup) {
        printf("INCONCLUSIVE: the stop (%.3f..%.3f) came before the setup ended (%.3f)\n",
               stopped_at, resumed_at, r_setup);
        _exit(4);
    }
    for (unsigned i = 0; i < sizeof(timer_sigs) / sizeof(timer_sigs[0]); i++) {
        if (any_before(timer_sigs[i], stopped_at)) {
            printf("INCONCLUSIVE: signal %d came before the stop\n", timer_sigs[i]);
            _exit(4);
        }
    }
    for (int k = 0; k < SL_COUNT; k++) {
        struct sleeper *s = &sleepers[k];
        if (s->available && s->done && s->r1 < stopped_at) {
            printf("INCONCLUSIVE: %s returned (%.3f) before the stop (%.3f)\n",
                   sleep_name[k], s->r1, stopped_at);
            _exit(4);
        }
    }

    // ---- 1. alarm() in a child, which has no handler: SIGALRM kills it at
    //         its deadline on MONOTONIC.
    double alarm_due = alarm_m0 + D_MONO;
    if (alarm_status < 0)
        check("alarm", 0, "the child with alarm(%d) is still alive at monotonic %.3f "
              "(it was due at %.3f)", (int) D_MONO, now(CLOCK_MONOTONIC), alarm_due);
    else
        check("alarm", WIFSIGNALED(alarm_status) && WTERMSIG(alarm_status) == SIGALRM &&
                       alarm_m1 >= alarm_due - 0.05 && alarm_m1 <= alarm_due + LATE,
              "child %s at monotonic %.3f, due at %.3f",
              WIFSIGNALED(alarm_status) ? strsignal(WTERMSIG(alarm_status)) : "exited",
              alarm_m1, alarm_due);

    // ---- 2. POSIX timers, each at its deadline on the clock that counts it.
    const struct arrival *a;
    a = first_after(SIG_MONO, stopped_at);
    check("posix-monotonic", a != NULL && a->m >= m0 + D_MONO - 0.01 &&
                             a->m <= m0 + D_MONO + LATE && a->code == SI_TIMER &&
                             a->value == 1001 && a->timerid == timer_id_of(t_mono),
          "%s monotonic %.3f (due %.3f), code %d value %d timerid %d (want %d)",
          a != NULL ? "came at" : "never came; last looked at", a != NULL ? a->m : now(CLOCK_MONOTONIC),
          m0 + D_MONO, a != NULL ? a->code : 0, a != NULL ? a->value : 0,
          a != NULL ? a->timerid : -1, timer_id_of(t_mono));
    a = first_after(SIG_REAL_REL, stopped_at);
    check("posix-realtime-relative", a != NULL && a->m >= m0 + D_MONO - 0.01 &&
                                     a->m <= m0 + D_MONO + LATE && a->value == 1002,
          "%s monotonic %.3f (due %.3f: a relative arming is on MONOTONIC, even on "
          "CLOCK_REALTIME)", a != NULL ? "came at" : "never came; last looked at",
          a != NULL ? a->m : now(CLOCK_MONOTONIC), m0 + D_MONO);
    a = first_after(SIG_REAL_ABS, stopped_at);
    double abs_due = r0 + D_COUNTS_STOP;
    double abs_hi = (abs_due > resumed_at ? abs_due : resumed_at) + LATE;
    check("posix-realtime-absolute", a != NULL && a->r >= abs_due - 0.01 && a->r <= abs_hi &&
                                     a->value == 1003,
          "%s realtime %.3f (due %.3f, or at the resume %.3f if that was later)",
          a != NULL ? "came at" : "never came; last looked at",
          a != NULL ? a->r : now(CLOCK_REALTIME), abs_due, resumed_at);
    a = first_after(SIG_BOOT, stopped_at);
    double boot_due = b0 + D_COUNTS_STOP;
    double boot_hi = (boot_due > boot_resumed ? boot_due : boot_resumed) + LATE;
    check("posix-boottime", a != NULL && a->b >= boot_due - 0.01 && a->b <= boot_hi &&
                            a->value == 1004,
          "%s boottime %.3f (due %.3f, or at the resume %.3f if that was later)",
          a != NULL ? "came at" : "never came; last looked at",
          a != NULL ? a->b : now(CLOCK_BOOTTIME), boot_due, boot_resumed);
    a = first_after(SIG_PERIODIC, stopped_at);
    int n_periodic = count_after(SIG_PERIODIC, stopped_at);
    int periodic_ok = a != NULL && n_periodic >= 3 && a->m >= m0 + D_MONO - 0.01 &&
                      a->m <= m0 + D_MONO + LATE;
    // After the first, each on the grid the timer was armed on -- due at
    // m0 + D_MONO + k * PERIOD -- and taken within GRID_LATE of it, a later
    // expiry each time. A first one the restore made late, its deadline
    // falling inside the restore, is followed by the next on the grid, sooner
    // than a period after it: Linux keeps a periodic timer's grid
    // (hrtimer_forward), and so does util/timer.c. Measuring the gaps between
    // arrivals instead called that short gap a failure.
    double grid0 = m0 + D_MONO, step_min = 1e9, step_max = 0, off_max = 0;
    long last_k = -1;
    for (int i = 0; i < arrivals[SIG_PERIODIC].count && i < MAX_ARRIVALS; i++) {
        double m = arrivals[SIG_PERIODIC].a[i].m;
        double since = m - grid0 + 0.01;
        long k = since < 0 ? -1 : (long) (since / PERIOD);
        double off = m - (grid0 + (double) k * PERIOD);
        if (i > 0) {
            double step = m - arrivals[SIG_PERIODIC].a[i - 1].m;
            if (step < step_min) step_min = step;
            if (step > step_max) step_max = step;
            if (off > off_max) off_max = off;
            if (k <= last_k || off > GRID_LATE)
                periodic_ok = 0;
        }
        last_k = k;
    }
    check("posix-periodic", periodic_ok,
          "%d signal(s), the first %s monotonic %.3f (due %.3f), then %.3f..%.3f s apart, "
          "each at most %.3f s after its place on the %.1f s grid",
          n_periodic, a != NULL ? "at" : "never; looked at",
          a != NULL ? a->m : now(CLOCK_MONOTONIC), m0 + D_MONO,
          n_periodic > 1 ? step_min : 0.0, n_periodic > 1 ? step_max : 0.0,
          off_max, PERIOD);

    // A timer nobody is told about still counts down, and keeps its interval.
    struct itimerspec cur = {0};
    int gt = timer_gettime(t_none, &cur);
    double none_left = ts_d(cur.it_value);
    double none_want = m0 + D_MONO + 20 - now(CLOCK_MONOTONIC);
    check("posix-sigev-none", gt == 0 && none_left >= none_want - 0.2 &&
                              none_left <= none_want + 0.2 && ts_d(cur.it_interval) == 3.0,
          "timer_gettime %s: %.3f s left (want %.3f), interval %.3f",
          gt == 0 ? "ok" : strerror(errno), none_left, none_want, ts_d(cur.it_interval));
    cur = (struct itimerspec) {0};
    gt = timer_gettime(t_pcpu, &cur);
    check("posix-process-cpu", gt == 0 && ts_d(cur.it_value) > 900,
          "timer_gettime %s: %.3f s of CPU left", gt == 0 ? "ok" : strerror(errno),
          ts_d(cur.it_value));

    // ---- 3. setitimer(ITIMER_REAL), periodic.
    a = first_after(SIGALRM, stopped_at);
    struct itimerval now_itv = {0};
    getitimer(ITIMER_REAL, &now_itv);
    check("itimer-real", a != NULL && count_after(SIGALRM, stopped_at) >= 2 &&
                         a->m >= m0 + D_MONO - 0.02 && a->m <= m0 + D_MONO + LATE &&
                         now_itv.it_interval.tv_usec == (suseconds_t) (PERIOD * 1e6),
          "%d SIGALRM(s), the first %s monotonic %.3f (due %.3f), interval now %ld us",
          count_after(SIGALRM, stopped_at), a != NULL ? "at" : "never; looked at",
          a != NULL ? a->m : now(CLOCK_MONOTONIC), m0 + D_MONO,
          (long) now_itv.it_interval.tv_usec);

    // ---- 4. CPU time: what had been burned when each came.
    a = first_after(SIG_TCPU, stopped_at);
    check("posix-thread-cpu", a != NULL && a->cpu >= D_CPU - 0.1 && a->cpu <= D_CPU + LATE &&
                              a->value == 7007,
          "%s when the burner had burned %.3f s of its own CPU (want %.1f)",
          a != NULL ? "came" : "never came; looked", a != NULL ? a->cpu : burned_thread(),
          D_CPU);
    a = first_after(SIGPROF, stopped_at);
    // The whole process's CPU, of which the burner is nearly all.
    check("itimer-prof", a != NULL && a->cpu >= D_CPU - 0.3 && a->cpu <= D_CPU + LATE,
          "%s when the process had burned %.3f s of CPU (want %.1f)",
          a != NULL ? "came" : "never came; looked",
          a != NULL ? a->cpu : burned_proc() - prof_base, D_CPU);

    // ---- 5. What was pending when the image was written, taken now.
    struct timespec zero = {0, 0};
    sigset_t one;
    siginfo_t si;
    sigemptyset(&one);
    sigaddset(&one, SIGUSR2);
    int got = sigtimedwait(&one, &si, &zero);
    check("pending-queued", got == SIGUSR2 && si.si_code == SI_QUEUE &&
                            si.si_value.sival_int == 0xC0FFEE && si.si_pid == getpid(),
          "sigtimedwait -> %d, code %d, value %#x, pid %d", got, got > 0 ? si.si_code : 0,
          got > 0 ? si.si_value.sival_int : 0, got > 0 ? (int) si.si_pid : 0);
    // Never taken: its first expiry is queued, and every one since is an
    // overrun counted on it -- before the save and after the restore, until the
    // timer was stopped once the restore was seen.
    int ov_before = -1;
    for (unsigned i = 0; i < ov_n && i < 64; i++) {
        unsigned k = (ov_n - 1 - i) % 64;
        if (ov_ring[k].r < stopped_at) {
            ov_before = ov_ring[k].ov;
            break;
        }
    }
    sigemptyset(&one);
    sigaddset(&one, SIG_OVERRUN);
    got = sigtimedwait(&one, &si, &zero);
    // At least what it held at the last reading before the stop -- more, for
    // the periods the save itself took (a timer's thread runs on while the
    // tasks are parked) and those before the timer was stopped. A signal that
    // had been lost and queued afresh after the restore would hold only the
    // handful of those last.
    check("pending-timer-overrun", got == SIG_OVERRUN && si.si_code == SI_TIMER &&
                                   si.si_timerid == timer_id_of(t_overrun) &&
                                   si.si_value.sival_int == 1006 && ov_before > 50 &&
                                   si.si_overrun >= ov_before,
          "sigtimedwait -> %d, code %d, timerid %d, overrun %d (it held %d at the stop; "
          "the timer was stopped %.3f s after the resume)",
          got, got > 0 ? si.si_code : 0, got > 0 ? si.si_timerid : -1,
          got > 0 ? si.si_overrun : 0, ov_before, r_ov_stopped - resumed_at);
    // Both SIGPWRs, kill()'s and the timer's, and no third. The timer ran on
    // after the restore, so its signal there had to be recognised as its own
    // -- counted on, not queued again -- which the image has to say.
    siginfo_t pw[3];
    int npw = 0;
    sigemptyset(&one);
    sigaddset(&one, SIGPWR);
    while (npw < 3 && sigtimedwait(&one, &pw[npw], &zero) == SIGPWR)
        npw++;
    check("pending-two-of-one", npw == 2 && pw[0].si_code == SI_USER &&
                                pw[0].si_pid == getpid() && pw[1].si_code == SI_TIMER &&
                                pw[1].si_timerid == timer_id_of(t_pwr) &&
                                pw[1].si_value.sival_int == 1010 && pw[1].si_overrun > 0,
          "%d SIGPWR(s): %s code %d, then %s code %d timerid %d overrun %d%s", npw,
          npw > 0 ? "first" : "no", npw > 0 ? pw[0].si_code : 0, npw > 1 ? "second" : "no",
          npw > 1 ? pw[1].si_code : 0, npw > 1 ? pw[1].si_timerid : -1,
          npw > 1 ? pw[1].si_overrun : 0, npw > 2 ? ", and a third" : "");
    // SIGURG, sent with kill(): let it through. Its default is to be ignored,
    // so it goes -- and nothing is left pending that no queue holds, which
    // would end every wait at once, for ever.
    sigset_t pend;
    sigpending(&pend);
    int urg_was = sigismember(&pend, SIGURG);
    sigemptyset(&one);
    sigaddset(&one, SIGURG);
    sigprocmask(SIG_UNBLOCK, &one, NULL);
    double s0 = now(CLOCK_MONOTONIC);
    struct timeval tv0 = {0, 200000};
    int sel = select(0, NULL, NULL, NULL, &tv0);
    int sel_err = sel < 0 ? errno : 0;
    double took = now(CLOCK_MONOTONIC) - s0;
    sigpending(&pend);
    check("pending-kill", urg_was && !sigismember(&pend, SIGURG) && sel == 0 && took >= 0.19,
          "SIGURG %s at the restore, %s after it was unblocked; a 0.2 s select "
          "returned %d (%s) after %.3f s", urg_was ? "pending" : "NOT pending",
          sigismember(&pend, SIGURG) ? "still pending" : "gone", sel,
          sel_err ? strerror(sel_err) : "ok", took);

    // ---- A periodic timerfd on BOOTTIME, never read: every period since it
    //      was armed, the stop's among them. BOOTTIME counts the stop, so the
    //      image carries a next expiry that the stop has already passed; the
    //      restored timer is armed that far overdue and counts every period
    //      since (util/timer.c), as Linux's does after a hibernation. It used
    //      to come back due once and count one, the stop's periods lost.
    uint64_t od = 0;
    double od_r0 = now(CLOCK_BOOTTIME);
    ssize_t odr = read(tfd_overdue, &od, sizeof(od));
    double od_r1 = now(CLOCK_BOOTTIME);
    // The first expiry is a period after the arming; a few of the latest may
    // not be in yet.
    long od_lo = (long) ((od_r0 - od_b1 - OVERDUE_PERIOD) / OVERDUE_PERIOD) + 1 - 3;
    long od_hi = (long) ((od_r1 - od_b0 - OVERDUE_PERIOD) / OVERDUE_PERIOD) + 1;
    check("overdue-periodic", odr == (ssize_t) sizeof(od) && (long) od >= od_lo &&
                              (long) od <= od_hi,
          "read -> %zd, %llu expirations, want %ld..%ld (a %.1f s timer on BOOTTIME, "
          "armed %.3f s before, which the %.3f s stop is part of)", odr,
          (unsigned long long) od, od_lo, od_hi, OVERDUE_PERIOD, od_r0 - od_b1, gap);

    // ---- 6. The sleeps: each wakes at its deadline on the clock that counts
    //         it, not its whole duration after the resume.
    for (int k = 0; k < SL_COUNT; k++) {
        struct sleeper *s = &sleepers[k];
        if (!s->available)
            continue;
        char what[64];
        snprintf(what, sizeof(what), "sleep-%s", sleep_name[k]);
        if (k == SL_CNS_BOOT) {
            double due = s->b0 + s->dur;
            double hi = (due > boot_resumed ? due : boot_resumed) + LATE;
            if (!s->done)
                check(what, 0, "still asleep at boottime %.3f; due at %.3f, or at the "
                      "resume %.3f", now(CLOCK_BOOTTIME), due, boot_resumed);
            else
                check(what, s->rc == 0 && s->b1 >= due - 0.01 && s->b1 <= hi,
                      "rc %ld errno %d, woke at boottime %.3f (due %.3f, or at the resume "
                      "%.3f)", s->rc, s->err, s->b1, due, boot_resumed);
        } else {
            double due = s->m0 + s->dur;
            if (!s->done)
                check(what, 0, "still asleep at monotonic %.3f; due at %.3f",
                      now(CLOCK_MONOTONIC), due);
            else
                check(what, s->rc == 0 && s->m1 >= due - 0.01 && s->m1 <= due + LATE,
                      "rc %ld errno %d, woke at monotonic %.3f (due %.3f; %.3f s after "
                      "the resume)", s->rc, s->err, s->m1, due, s->r1 - resumed_at);
        }
    }

    // ---- 7. "Never": a timer armed at TIME_T_MAX, a timerfd armed the way
    //         systemd watches for a clock change, and a sleep for TIME_T_MAX.
    //         Each is still waiting -- not woken by a deadline that overflowed
    //         on its way through the image.
    struct itimerspec nv = {0};
    gt = timer_gettime(t_never, &nv);
    check("never-posix", arrivals[SIG_NEVER].count == 0 && gt == 0 &&
                         (long long) nv.it_value.tv_sec > 1000000000000LL,
          "%d signal(s); timer_gettime %s, %lld s left", arrivals[SIG_NEVER].count,
          gt == 0 ? "ok" : strerror(errno), (long long) nv.it_value.tv_sec);
    uint64_t never_exp = 0;
    ssize_t tr = read(tfd_never, &never_exp, sizeof(never_exp));
    int tr_err = tr < 0 ? errno : 0;
    struct itimerspec tfd_left = {0};
    int tg = timerfd_gettime(tfd_never, &tfd_left);
    check("never-timerfd", tr < 0 && tr_err == EAGAIN && tg == 0 &&
                           (long long) tfd_left.it_value.tv_sec > 1000000000000LL,
          "read -> %zd (%s, %llu expirations); timerfd_gettime %s, %lld s left", tr,
          tr_err ? strerror(tr_err) : "an expiry", (unsigned long long) never_exp,
          tg == 0 ? "ok" : strerror(errno), (long long) tfd_left.it_value.tv_sec);
    check("never-sleep", !forever_done, "a nanosleep of TIME_T_MAX %s",
          forever_done ? "RETURNED" : "is still asleep");

    if (asker > 0) {
        kill(asker, SIGKILL);
        waitpid(asker, NULL, 0);
    }
    printf("TIMERS-PROBE-DONE failures=%d\n", failures);
    // As pid 1 on the CLI this ends the machine, sleepers and all.
    _exit(failures == 0 ? 0 : 1);
}
