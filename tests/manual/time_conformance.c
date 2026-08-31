// time_conformance.c — self-checking regression lock for the time/clock/timer
// conformance fixes found by differential testing against real Linux (mint).
// Each check asserts the documented Linux behavior that iSH now matches:
//
//   - getitimer(ITIMER_REAL) works: unset -> {0,0}; after setitimer it reports
//     the remaining value; a bogus `which` -> EINVAL (was unwired -> SIGSYS)
//   - setitimer(ITIMER_VIRTUAL/PROF) fires SIGVTALRM/SIGPROF under CPU load
//     (previously EINVAL-only); a one-shot reads back disarmed after firing
//   - POSIX timer_settime/gettime marshal the right itimerspec width on amd64
//     (arm 100ms one-shot -> gettime remaining in (0,100ms]; interval kept;
//     SIGEV_SIGNAL actually delivers, si_code SI_TIMER)
//   - timerfd: same width fix (gettime remaining sane; a one-shot read returns
//     1 expiration); create rejects a non-timerfd clock and unknown flags
//   - nanosleep interrupted by a signal writes the remaining time to `rem`
//   - CLOCK_TAI reads epoch-based time (not boot-relative MONOTONIC)
//   - clock_settime(CLOCK_MONOTONIC) -> EINVAL (a non-settable clock), not EPERM
//
// The same source is a Tier-0 functional gate (run under iSH, no oracle) and a
// portable check that also passes on a real Linux kernel. NOTE: it deliberately
// does NOT exercise the privileged setters that actually change the wall clock
// (settimeofday / clock_settime(CLOCK_REALTIME)); those are covered by the
// differential corpus against mint's unprivileged kernel.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include "test_common.h"

#ifndef CLOCK_TAI
#define CLOCK_TAI 11
#endif
#ifndef SI_TIMER
#define SI_TIMER -2
#endif
#ifndef TFD_NONBLOCK
#define TFD_NONBLOCK O_NONBLOCK
#endif
#define EPOCH_FLOOR 1500000000LL

static int eq_errno(const char *label, long r, int want) {
    int e = (r < 0) ? errno : 0;
    if (r < 0 && e == want) { test_logf("ok   %s (errno %d)\n", label, e); return 1; }
    printf("FAIL %s: ret=%ld errno=%d, wanted -1/errno=%d\n", label, r, e, want);
    failures_total++;
    return 0;
}
static int is_ok(const char *label, long r) {
    if (r >= 0) { test_logf("ok   %s\n", label); return 1; }
    printf("FAIL %s: ret=%ld errno=%d, wanted success\n", label, r, errno);
    failures_total++;
    return 0;
}
static int is_true(const char *label, int cond) {
    if (cond) { test_logf("ok   %s\n", label); return 1; }
    printf("FAIL %s\n", label);
    failures_total++;
    return 0;
}

static int64_t ts_ns(struct timespec t) { return (int64_t) t.tv_sec * 1000000000LL + t.tv_nsec; }
static int64_t itv_ns(struct itimerspec s) { return (int64_t) s.it_value.tv_sec * 1000000000LL + s.it_value.tv_nsec; }
static int64_t iti_ns(struct itimerspec s) { return (int64_t) s.it_interval.tv_sec * 1000000000LL + s.it_interval.tv_nsec; }
static struct itimerspec mk(long val_ms, long ival_ms) {
    struct itimerspec s; memset(&s, 0, sizeof s);
    s.it_value.tv_sec = val_ms / 1000; s.it_value.tv_nsec = (val_ms % 1000) * 1000000L;
    s.it_interval.tv_sec = ival_ms / 1000; s.it_interval.tv_nsec = (ival_ms % 1000) * 1000000L;
    return s;
}

static void check_getitimer(void) {
    struct itimerval z; memset(&z, 0, sizeof z);
    setitimer(ITIMER_REAL, &z, NULL);                 /* disarm */
    struct itimerval got;
    memset(&got, 0, sizeof got);
    is_ok("getitimer unset", getitimer(ITIMER_REAL, &got));
    is_true("getitimer unset is zero",
            got.it_value.tv_sec == 0 && got.it_value.tv_usec == 0);

    struct itimerval want; memset(&want, 0, sizeof want);
    want.it_value.tv_sec = 10;
    is_ok("setitimer arm 10s", setitimer(ITIMER_REAL, &want, NULL));
    memset(&got, 0, sizeof got);
    is_ok("getitimer armed", getitimer(ITIMER_REAL, &got));
    int64_t us = (int64_t) got.it_value.tv_sec * 1000000 + got.it_value.tv_usec;
    is_true("getitimer armed in range", us > 0 && us <= 10000000LL);

    memset(&got, 0, sizeof got);
    errno = 0;
    eq_errno("getitimer bad which", getitimer(99, &got), EINVAL);
    setitimer(ITIMER_REAL, &z, NULL);                 /* disarm */
}

static volatile sig_atomic_t vtalrm_count, prof_count;
static void vtalrm_handler(int sig) { (void) sig; vtalrm_count++; }
static void prof_handler(int sig) { (void) sig; prof_count++; }

// ITIMER_VIRTUAL/PROF (issue #423 Tier 3): CPU-time-based itimers, driven by
// periodic sampling (kernel/time.c) since this codebase's timer subsystem has
// no native CPU-time clock. Delivery timing is approximate (sampled every
// ~20ms), so the burn budget below is generous.
static void check_itimer_vprof(void) {
    struct itimerval z; memset(&z, 0, sizeof z);
    struct sigaction act; memset(&act, 0, sizeof act);

    act.sa_handler = vtalrm_handler;
    sigaction(SIGVTALRM, &act, NULL);
    vtalrm_count = 0;
    struct itimerval val = {.it_value = {.tv_usec = 100000}};
    is_ok("setitimer ITIMER_VIRTUAL arm", setitimer(ITIMER_VIRTUAL, &val, NULL));

    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile long x = 0;
    do {
        for (int i = 0; i < 100000; i++) x += i;
        clock_gettime(CLOCK_MONOTONIC, &now);
    } while (vtalrm_count == 0 && (now.tv_sec - start.tv_sec) < 3);
    is_true("SIGVTALRM fires within 3s of CPU burn", vtalrm_count > 0);

    struct itimerval got; memset(&got, 0, sizeof got);
    getitimer(ITIMER_VIRTUAL, &got);
    is_true("ITIMER_VIRTUAL one-shot reads disarmed after firing",
            got.it_value.tv_sec == 0 && got.it_value.tv_usec == 0);
    setitimer(ITIMER_VIRTUAL, &z, NULL);

    act.sa_handler = prof_handler;
    sigaction(SIGPROF, &act, NULL);
    prof_count = 0;
    is_ok("setitimer ITIMER_PROF arm", setitimer(ITIMER_PROF, &val, NULL));
    clock_gettime(CLOCK_MONOTONIC, &start);
    x = 0;
    do {
        for (int i = 0; i < 100000; i++) x += i;
        clock_gettime(CLOCK_MONOTONIC, &now);
    } while (prof_count == 0 && (now.tv_sec - start.tv_sec) < 3);
    is_true("SIGPROF fires within 3s of CPU burn", prof_count > 0);
    setitimer(ITIMER_PROF, &z, NULL);
}

static volatile sig_atomic_t got_timer;
static volatile int got_code;
static void on_timer(int s, siginfo_t *si, void *u) { (void) s; (void) u; got_timer++; got_code = si ? si->si_code : 0; }

static void check_posix_timer(void) {
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_timer; sa.sa_flags = SA_SIGINFO; sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);

    struct sigevent sev; memset(&sev, 0, sizeof sev);
    sev.sigev_notify = SIGEV_SIGNAL; sev.sigev_signo = SIGUSR1;
    timer_t t;
    if (!is_ok("timer_create", timer_create(CLOCK_MONOTONIC, &sev, &t)))
        return;

    struct itimerspec want = mk(100, 0), got;
    is_ok("timer_settime one-shot", timer_settime(t, 0, &want, NULL));
    memset(&got, 0, sizeof got);
    is_ok("timer_gettime", timer_gettime(t, &got));
    is_true("timer remaining in range", itv_ns(got) > 0 && itv_ns(got) <= 100000000LL);
    is_true("timer interval zero", iti_ns(got) == 0);

    struct itimerspec iv = mk(500, 250);
    timer_settime(t, 0, &iv, NULL);
    memset(&got, 0, sizeof got);
    timer_gettime(t, &got);
    is_true("timer interval preserved", iti_ns(got) == 250000000LL);

    /* delivery */
    got_timer = 0; got_code = 0;
    struct itimerspec fire = mk(40, 0);
    timer_settime(t, 0, &fire, NULL);
    for (int i = 0; i < 200 && got_timer == 0; i++) { struct timespec d = {0, 10000000}; nanosleep(&d, NULL); }
    is_true("timer delivered SIGUSR1", got_timer >= 1);
    is_true("timer si_code SI_TIMER", got_code == SI_TIMER);
    is_ok("timer_delete", timer_delete(t));
}

static volatile sig_atomic_t alrm;
static void on_alrm(int s) { (void) s; alrm = 1; }

static void check_timerfd(void) {
    errno = 0;
    eq_errno("timerfd bad clock", timerfd_create(CLOCK_PROCESS_CPUTIME_ID, 0), EINVAL);
    errno = 0;
    eq_errno("timerfd bad flags", timerfd_create(CLOCK_MONOTONIC, 0x40000), EINVAL);

    int fd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (!is_ok("timerfd_create", fd))
        return;
    struct itimerspec s = mk(500, 0), got;
    is_ok("timerfd_settime", timerfd_settime(fd, 0, &s, NULL));
    memset(&got, 0, sizeof got);
    is_ok("timerfd_gettime", timerfd_gettime(fd, &got));
    is_true("timerfd remaining in range", itv_ns(got) > 0 && itv_ns(got) <= 500000000LL);

    /* re-arm short and read the single expiration (alarm-guarded) */
    struct itimerspec sh = mk(60, 0);
    timerfd_settime(fd, 0, &sh, NULL);
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_alrm; sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);
    alrm = 0; alarm(3);
    uint64_t ticks = 0;
    ssize_t n = read(fd, &ticks, sizeof ticks);
    alarm(0);
    is_true("timerfd read 1 expiration", n == (ssize_t) sizeof ticks && ticks == 1 && !alrm);
    close(fd);

    /* O_NONBLOCK unarmed read -> EAGAIN */
    fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (fd >= 0) {
        errno = 0;
        eq_errno("timerfd nonblock EAGAIN", read(fd, &ticks, sizeof ticks), EAGAIN);
        close(fd);
    }
}

static void on_usr2(int s) { (void) s; }

static void check_nanosleep_eintr(void) {
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_usr2; sigemptyset(&sa.sa_mask);       /* no SA_RESTART */
    sigaction(SIGUSR2, &sa, NULL);

    pid_t parent = getpid();
    pid_t pid = fork();
    if (pid == 0) {
        struct timespec d = {0, 60000000};                  /* 60ms */
        nanosleep(&d, NULL);
        kill(parent, SIGUSR2);
        _exit(0);
    }
    struct timespec req = {1, 0}, rem = {0, 0};
    errno = 0;
    int r = nanosleep(&req, &rem);
    int e = errno;
    is_true("nanosleep interrupted EINTR", r < 0 && e == EINTR);
    is_true("nanosleep rem in range", ts_ns(rem) > 0 && ts_ns(rem) <= 1000000000LL);
    int st; while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
}

static void check_clocks(void) {
    struct timespec rt, tai, mono, a, b;
    is_ok("clock_gettime REALTIME", clock_gettime(CLOCK_REALTIME, &rt));
    is_ok("clock_gettime MONOTONIC", clock_gettime(CLOCK_MONOTONIC, &mono));
    is_true("MONOTONIC < REALTIME", ts_ns(mono) < ts_ns(rt));

    is_ok("clock_gettime CLOCK_TAI", clock_gettime(CLOCK_TAI, &tai));
    is_true("CLOCK_TAI is epoch-based", tai.tv_sec > EPOCH_FLOOR);

    clock_gettime(CLOCK_MONOTONIC, &a);
    for (volatile int i = 0; i < 100000; i++) { }
    clock_gettime(CLOCK_MONOTONIC, &b);
    is_true("MONOTONIC nondecreasing", ts_ns(b) >= ts_ns(a));

    /* clock_settime on a non-settable clock -> EINVAL (uid-independent, and a
     * no-op even on a hypothetical success since MONOTONIC can't be set). */
    errno = 0;
    eq_errno("clock_settime MONOTONIC EINVAL", clock_settime(CLOCK_MONOTONIC, &mono), EINVAL);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    check_clocks();
    check_nanosleep_eintr();
    check_getitimer();
    check_itimer_vprof();
    check_posix_timer();
    check_timerfd();
    return finish_suite("time_conformance");
}
