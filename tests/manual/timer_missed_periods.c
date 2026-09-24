/*
 * timer_missed_periods.c -- a periodic timer that falls behind counts every
 * period it missed, and stays on its grid.
 *
 * Linux keeps a periodic timer's expiries at start + k * interval, whenever it
 * gets round to them (hrtimer_forward, bump_cpu_timer), and counts all of
 * them: a timerfd read returns every expiry since the last read, and a POSIX
 * timer whose signal is still queued adds them to its si_overrun. Nothing is
 * replayed in a burst; one wakeup carries the lot.
 *
 * AOK's timer thread (util/timer.c) instead dropped them. When it found the
 * next expiry already past it re-based the timer on "now": one expiry counted
 * however many periods had gone by, and the grid moved to wherever the thread
 * happened to wake. A guest's own SIGSTOP does not do that here -- the timer
 * runs on a host thread the stop does not touch -- but these do:
 *
 *   thread CPU clock   a CLOCK_THREAD_CPUTIME_ID timer reads its thread's CPU
 *                      time, which AOK samples in 10ms steps: a 1ms timer saw
 *                      ten periods go by at once, and counted one.
 *   process CPU clock  a CLOCK_PROCESS_CPUTIME_ID timer, with four threads
 *                      spinning: the clock runs four times faster than the
 *                      thread sleeping on it, and three periods in four went.
 *   the host stopped   iOS suspending the app, or the Mac's kill -STOP: every
 *                      timer thread stops with it. Run as `timer_missed_periods
 *                      host-stop` by timer_missed_periods.sh, which stops the
 *                      emulator for 300ms while this sleeps.
 *   armed in the past  timerfd_settime/timer_settime TIMER_ABSTIME with a
 *                      start 50ms ago: every period since is already due.
 *
 * and this, which Linux does and which runs the same checks where a guest can
 * make them:
 *
 *   process stopped    a child SIGSTOPs this process for 200ms, with a 1ms
 *                      timerfd and a 1ms POSIX timer (its signal blocked)
 *                      armed on a common grid.
 *
 * Each count is bracketed by clock readings taken before and after it, and
 * the grid by timerfd_gettime/timer_gettime read between two clock readings.
 *
 * Before the fix, on all six test roots: the thread CPU clock counted 29-30
 * expiries for ~297ms of CPU, the process clock 222-318 for ~1180ms, the
 * arming in the past 1 of ~51; the process stopped passed, the timer threads
 * running on through it; and with
 * the host stopped for 300ms a 900ms wait counted 574 of 882, the next expiry
 * 0.12-0.49ms off the grid. Linux 6.12 (camd, -m64 and -m32) passes.
 *
 * Exits 0 and prints "timer_missed_periods: PASS" on success.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <time.h>

#include "test_common.h"

#define NS 1000000000LL
#define PERIOD_NS 1000000LL    /* 1ms */
/* How far ahead of now the stop scenarios' grid starts, and how far behind
 * the past arming's does. */
#define LEAD_NS 20000000LL
#define PAST_NS 50000000LL
/* The guest stop, and how long the child waits before it. */
#define STOP_MS 200
#define STOP_AFTER_MS 50
/* How long the host-stop mode sleeps, inside which the driver stops us (for
 * 300ms), and the least stop that counts as one. */
#define HOST_STOP_WAIT_MS 900
#define HOST_STOP_MIN_MS 150
/* How many of the most recent periods a count may be short: the timer's host
 * thread wakes a little after each expiry, and the last may not be in yet. */
#define LATE_PERIODS 3
/* How far off the grid the next expiry may be, beyond the width of the
 * bracket that reads it. */
#define GRID_SLACK_NS 100000LL
/* The CPU clocks: how long to spin, and how many spinners on the process one. */
#define SPIN_MS 300
#define SPINNERS 4

#define SIG_MONO SIGUSR1
#define SIG_PCPU SIGUSR2
#define SIG_TCPU SIGURG

static int64_t ns_of(struct timespec t) {
    return (int64_t) t.tv_sec * NS + t.tv_nsec;
}

static struct timespec ts_of(int64_t ns) {
    return (struct timespec) {.tv_sec = ns / NS, .tv_nsec = ns % NS};
}

static int64_t clock_ns(clockid_t c) {
    struct timespec t;
    clock_gettime(c, &t);
    return ns_of(t);
}

static void sleep_ms(long ms) {
    struct timespec t = ts_of(ms * 1000000LL);
    while (nanosleep(&t, &t) < 0 && errno == EINTR)
        ;
}

static timer_t make_timer(clockid_t clock, int sig) {
    struct sigevent sev = {0};
    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = sig;
    timer_t t;
    if (timer_create(clock, &sev, &t) != 0) {
        printf("FAIL timer_create(%d): %s\n", (int) clock, strerror(errno));
        exit(1);
    }
    return t;
}

/* Expiries of a grid starting at `start` by the time `at`: start itself is
 * the first. */
static int64_t expiries_by(int64_t start, int64_t at) {
    return at < start ? 0 : (at - start) / PERIOD_NS + 1;
}

/* `got` expiries, counted between clock readings `before` and `after`. */
static void check_count(const char *what, int64_t got, int64_t start, int64_t before,
                        int64_t after, int64_t late_periods) {
    int64_t lo = expiries_by(start, before) - late_periods;
    int64_t hi = expiries_by(start, after);
    if (got < lo || got > hi) {
        printf("FAIL %s: %lld expiries counted, want %lld..%lld (a %lldms timer, "
               "%.1fms after its first expiry)\n",
               what, (long long) got, (long long) lo, (long long) hi,
               (long long) (PERIOD_NS / 1000000), (after - start) / 1e6);
        failures_total++;
    } else {
        test_logf("%s: %lld expiries, want %lld..%lld\n", what, (long long) got,
                  (long long) lo, (long long) hi);
    }
}

/* The next expiry is `left` after some moment in [before, after]; it must be
 * on the grid that starts at `start`. */
static void check_grid(const char *what, struct timespec left, int64_t start,
                       int64_t before, int64_t after) {
    int64_t lo = before + ns_of(left) - GRID_SLACK_NS;
    int64_t hi = after + ns_of(left) + GRID_SLACK_NS;
    /* The first grid point at or after lo. */
    int64_t k = lo <= start ? 0 : (lo - start + PERIOD_NS - 1) / PERIOD_NS;
    int64_t g = start + k * PERIOD_NS;
    int64_t off = (before + ns_of(left) - start) % PERIOD_NS;
    if (off > PERIOD_NS / 2)
        off -= PERIOD_NS;
    if (g > hi) {
        printf("FAIL %s: the next expiry is %.3fms off the grid it was armed on "
               "(the reading spans %.3fms)\n", what, off / 1e6, (after - before) / 1e6);
        failures_total++;
    } else {
        test_logf("%s: on the grid (%.3fms off, the reading spanning %.3fms)\n", what,
                  off / 1e6, (after - before) / 1e6);
    }
}

/* ---- a stop: by a child (Linux's case), or by the host -------------------- */

/* The child: stop the parent, check it did stop, let it go. */
static int stopper(pid_t parent) {
    sleep_ms(STOP_AFTER_MS);
    if (kill(parent, SIGSTOP) != 0)
        return 2;
    int stopped = 0;
    char path[64], buf[256];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int) parent);
    for (int i = 0; i < 100 && !stopped; i++) {
        int fd = open(path, O_RDONLY);
        ssize_t n = fd >= 0 ? read(fd, buf, sizeof(buf) - 1) : -1;
        if (fd >= 0)
            close(fd);
        if (n > 0) {
            buf[n] = 0;
            char *p = strrchr(buf, ')');
            if (p != NULL && p[1] == ' ' && p[2] == 'T')
                stopped = 1;
        }
        if (!stopped)
            sleep_ms(1);
    }
    sleep_ms(STOP_MS);
    kill(parent, SIGCONT);
    return stopped ? 0 : 3;
}

/* The host-stop mode's witness that the stop happened: the longest gap
 * between readings of a thread that reads the clock every millisecond. */
static atomic_int watch_go;
static int64_t watch_gap;

static void *gap_watcher(void *arg) {
    (void) arg;
    int64_t last = clock_ns(CLOCK_MONOTONIC);
    while (atomic_load(&watch_go)) {
        sleep_ms(1);
        int64_t t = clock_ns(CLOCK_MONOTONIC);
        if (t - last > watch_gap)
            watch_gap = t - last;
        last = t;
    }
    return NULL;
}

enum scenario { PROCESS_STOP, HOST_STOP, PAST };

static void stop_scenario(enum scenario how) {
    bool host = how == HOST_STOP;
    const char *label = how == PAST ? "armed in the past" :
                        host ? "host stopped" : "process stopped";
    char what[96];
    int tfd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (tfd < 0) {
        printf("FAIL timerfd_create: %s\n", strerror(errno));
        failures_total++;
        return;
    }
    timer_t t = make_timer(CLOCK_MONOTONIC, SIG_MONO);
    /* One grid for both, armed absolute. */
    int64_t start = clock_ns(CLOCK_MONOTONIC) + (how == PAST ? -PAST_NS : LEAD_NS);
    struct itimerspec its = {.it_value = ts_of(start), .it_interval = ts_of(PERIOD_NS)};
    if (timerfd_settime(tfd, TFD_TIMER_ABSTIME, &its, NULL) != 0 ||
            timer_settime(t, TIMER_ABSTIME, &its, NULL) != 0) {
        printf("FAIL %s: arming: %s\n", label, strerror(errno));
        failures_total++;
        return;
    }

    int64_t waited_from = clock_ns(CLOCK_MONOTONIC);
    if (how == PAST) {
        /* Nothing to wait for: it was all due as it was armed. */
    } else if (host) {
        pthread_t watcher;
        atomic_store(&watch_go, 1);
        pthread_create(&watcher, NULL, gap_watcher, NULL);
        printf("HOST-STOP-READY\n");
        fflush(stdout);
        sleep_ms(HOST_STOP_WAIT_MS);
        atomic_store(&watch_go, 0);
        pthread_join(watcher, NULL);
        if (watch_gap < HOST_STOP_MIN_MS * 1000000LL) {
            printf("FAIL %s: the longest gap in a 1ms clock reading was %.1fms -- the host "
                   "was not stopped, so nothing was tested\n", label, watch_gap / 1e6);
            failures_total++;
        } else {
            test_logf("%s: stopped for about %.1fms\n", label, watch_gap / 1e6);
        }
    } else {
        pid_t parent = getpid();
        pid_t child = fork();
        if (child == 0)
            _exit(stopper(parent));
        int st = 0;
        while (waitpid(child, &st, 0) < 0 && errno == EINTR)
            ;
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
            printf("FAIL %s: the stopper %s (status %#x), so nothing was stopped\n", label,
                   WIFEXITED(st) && WEXITSTATUS(st) == 3 ? "never saw this process stopped"
                                                         : "failed", st);
            failures_total++;
        }
    }
    int64_t waited = clock_ns(CLOCK_MONOTONIC) - waited_from;
    test_logf("%s: waited %.1fms\n", label, waited / 1e6);

    /* The timerfd: every expiry since the grid started, in one read. */
    uint64_t n = 0;
    int64_t before = clock_ns(CLOCK_MONOTONIC);
    ssize_t r = read(tfd, &n, sizeof(n));
    int64_t after = clock_ns(CLOCK_MONOTONIC);
    snprintf(what, sizeof(what), "%s: timerfd read", label);
    if (r != sizeof(n)) {
        printf("FAIL %s: %zd (%s)\n", what, r, strerror(errno));
        failures_total++;
    } else {
        check_count(what, (int64_t) n, start, before, after, LATE_PERIODS);
    }
    struct itimerspec cur;
    before = clock_ns(CLOCK_MONOTONIC);
    int g = timerfd_gettime(tfd, &cur);
    after = clock_ns(CLOCK_MONOTONIC);
    snprintf(what, sizeof(what), "%s: timerfd's next expiry", label);
    if (g == 0)
        check_grid(what, cur.it_value, start, before, after);

    /* The POSIX timer: one signal, and every other expiry an overrun on it. */
    sigset_t one;
    sigemptyset(&one);
    sigaddset(&one, SIG_MONO);
    siginfo_t si;
    struct timespec zero = {0, 0};
    before = clock_ns(CLOCK_MONOTONIC);
    int got = sigtimedwait(&one, &si, &zero);
    after = clock_ns(CLOCK_MONOTONIC);
    snprintf(what, sizeof(what), "%s: POSIX timer's signal", label);
    if (got != SIG_MONO) {
        printf("FAIL %s: sigtimedwait -> %d (%s)\n", what, got, strerror(errno));
        failures_total++;
    } else {
        check_count(what, 1 + (int64_t) si.si_overrun, start, before, after, LATE_PERIODS);
    }
    before = clock_ns(CLOCK_MONOTONIC);
    g = timer_gettime(t, &cur);
    after = clock_ns(CLOCK_MONOTONIC);
    snprintf(what, sizeof(what), "%s: POSIX timer's next expiry", label);
    if (g == 0)
        check_grid(what, cur.it_value, start, before, after);

    timer_delete(t);
    close(tfd);
    /* Whatever came in between. */
    while (sigtimedwait(&one, &si, &zero) > 0)
        ;
}

/* ---- CPU clocks that run ahead of the thread sleeping on them ------------ */

static atomic_int spin_go;

static void *spinner(void *arg) {
    (void) arg;
    while (atomic_load_explicit(&spin_go, memory_order_relaxed))
        ;
    return NULL;
}

/* One CPU-clock timer, 1ms periodic, its signal blocked; `spin` does the
 * work. Returns the signal's expiries (1 + si_overrun), bracketed by `clock`
 * readings. */
static void cpu_scenario(const char *label, clockid_t clock, int sig, int spinners) {
    timer_t t = make_timer(clock, sig);
    struct itimerspec its = {.it_value = ts_of(PERIOD_NS), .it_interval = ts_of(PERIOD_NS)};
    int64_t w0 = clock_ns(CLOCK_MONOTONIC);
    int64_t c0a = clock_ns(clock);
    timer_settime(t, 0, &its, NULL);
    int64_t c0b = clock_ns(clock);

    pthread_t th[SPINNERS];
    atomic_store(&spin_go, 1);
    for (int i = 0; i < spinners; i++)
        pthread_create(&th[i], NULL, spinner, NULL);
    if (spinners == 0) {
        while (clock_ns(CLOCK_MONOTONIC) - w0 < SPIN_MS * 1000000LL)
            ;
    } else {
        sleep_ms(SPIN_MS);
    }
    atomic_store(&spin_go, 0);
    for (int i = 0; i < spinners; i++)
        pthread_join(th[i], NULL);
    int64_t wall = clock_ns(CLOCK_MONOTONIC) - w0;

    sigset_t one;
    sigemptyset(&one);
    sigaddset(&one, sig);
    siginfo_t si;
    struct timespec zero = {0, 0};
    int64_t before = clock_ns(clock);
    int got = sigtimedwait(&one, &si, &zero);
    int64_t after = clock_ns(clock);
    timer_delete(t);
    while (sigtimedwait(&one, &si, &zero) > 0)
        ;

    int64_t cpu = after - c0a;
    test_logf("%s: %.1fms of CPU in %.1fms\n", label, cpu / 1e6, wall / 1e6);
    if (spinners > 1 && cpu < wall * 3 / 2) {
        /* Nothing ran in parallel, so the clock never got ahead of the timer:
         * this says nothing either way. */
        printf("%s: not checked -- %.0fms of CPU in %.0fms, no parallelism to be had\n",
               label, cpu / 1e6, wall / 1e6);
        return;
    }
    if (got != sig) {
        printf("FAIL %s: sigtimedwait -> %d (%s) after %.1fms of CPU\n", label, got,
               strerror(errno), cpu / 1e6);
        failures_total++;
        return;
    }
    /* The timer's first expiry is a period after it was armed. How far behind
     * the clock a count may be: AOK samples a thread's CPU time in 10ms steps,
     * and the process clock gains a period per spinner while the timer's
     * thread sleeps one out. Short of that, it is the failure. */
    int64_t late = LATE_PERIODS + (clock == CLOCK_THREAD_CPUTIME_ID ? 10 : spinners);
    int64_t first = c0b + PERIOD_NS;
    int64_t lo = expiries_by(first, before) - late - expiries_by(first, before) / 20;
    int64_t hi = expiries_by(c0a + PERIOD_NS, after);
    /* AOK's process timer runs on the emulator's whole CPU time, of which the
     * guest's is most: a little over. Its thread timer reads the thread's CPU
     * time in 10ms steps, the arming included, so it may start up to a step
     * before the clock this reads -- and run ten periods ahead of it -- and
     * from the host's thread_info, which keeps its own count. Counting a
     * period twice would be far over either. */
    if (clock == CLOCK_PROCESS_CPUTIME_ID)
        hi += hi / 5 + LATE_PERIODS;
    else
        hi += 10 + hi / 10;
    int64_t n = 1 + (int64_t) si.si_overrun;
    if (n < lo || n > hi) {
        printf("FAIL %s: %lld expiries counted, want %lld..%lld (a 1ms timer on a clock "
               "that ran %.1fms)\n", label, (long long) n, (long long) lo, (long long) hi,
               cpu / 1e6);
        failures_total++;
    } else {
        test_logf("%s: %lld expiries, want %lld..%lld\n", label, (long long) n,
                  (long long) lo, (long long) hi);
    }
}

int main(int argc, char **argv) {
    bool host_stop = argc > 1 && strcmp(argv[1], "host-stop") == 0;
    if (host_stop) {
        argc--;
        argv++;
    }
    test_init(argc, argv);
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(test_watchdog_secs(60));

    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIG_MONO);
    sigaddset(&sigs, SIG_PCPU);
    sigaddset(&sigs, SIG_TCPU);
    sigprocmask(SIG_BLOCK, &sigs, NULL);

    if (host_stop) {
        stop_scenario(HOST_STOP);
        return finish_suite("timer_missed_periods host-stop");
    }
    stop_scenario(PAST);
    stop_scenario(PROCESS_STOP);
    cpu_scenario("thread CPU clock", CLOCK_THREAD_CPUTIME_ID, SIG_TCPU, 0);
    cpu_scenario("process CPU clock", CLOCK_PROCESS_CPUTIME_ID, SIG_PCPU, SPINNERS);
    return finish_suite("timer_missed_periods");
}
