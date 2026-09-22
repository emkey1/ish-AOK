// checkpoint_clock.c -- the guest's clocks across a checkpoint. Driven by
// checkpoint_clock.sh.
//
// A restore used to start the guest's CLOCK_MONOTONIC and uptime again near
// zero. Programs hold ABSOLUTE deadlines on those clocks in their own memory,
// which the image carries verbatim, so every one of them lay "uptime at the
// save" in the future: a Python daemon restored on the iPad sat in
// clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME) a minute past its deadline
// (Python's time.sleep is exactly that call), and `uptime` read "up 0 min".
//
// What a restore has to do is what Linux does across hibernation, and this
// checks all of it: CLOCK_MONOTONIC continues from its value at the save;
// CLOCK_BOOTTIME -- and so /proc/uptime -- also counts the time the machine
// was stopped; btime does not move; and a process's start time still means
// what it did. Then, because the guest's MONOTONIC and BOOTTIME now differ
// (on Darwin they ride the same host clock), every absolute arming is checked
// against the clock it names.
//
// The witness for "when did the machine come back" is a spinner thread
// reading CLOCK_REALTIME every few milliseconds: its largest gap IS the stop,
// measured on a clock the restore does not touch, so the clocks under test are
// never graded by themselves.
//
//     probe after     -- sleeps in 1 s absolute steps until an external save
//                        (ISH_CHECKPOINT_AFTER) and a restore have happened
//     probe suspend   -- asks for the suspend itself, from a child, and sleeps
//                        in the same 1 s absolute steps until it has happened
//
// Prints "OK <check>" / "FAIL <check>: why" lines, then CLOCK-PROBE-DONE.
// Exit 4 means the stop did not land inside the sleep under test, which says
// nothing either way.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef CLOCK_BOOTTIME
#define CLOCK_BOOTTIME 7
#endif

// When the save comes, and so roughly the uptime a broken restore makes every
// deadline wait for. Far above the 2 s a correct wake is allowed.
#define WARM_SECONDS 8.0

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
    t.tv_sec = (time_t) s;
    t.tv_nsec = (long) ((s - (double) t.tv_sec) * 1e9);
    if (t.tv_nsec >= 1000000000L) {
        t.tv_sec++;
        t.tv_nsec -= 1000000000L;
    }
    if (t.tv_nsec < 0)
        t.tv_nsec = 0;
    return t;
}

static int read_file(const char *path, char *buf, size_t size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    size_t at = 0;
    ssize_t n;
    while (at + 1 < size && (n = read(fd, buf + at, size - 1 - at)) > 0)
        at += (size_t) n;
    close(fd);
    buf[at] = '\0';
    return (int) at;
}

static double read_uptime(void) {
    char buf[128];
    if (read_file("/proc/uptime", buf, sizeof(buf)) <= 0)
        return -1;
    return atof(buf);
}

static long long read_btime(void) {
    static char buf[65536];
    if (read_file("/proc/stat", buf, sizeof(buf)) <= 0)
        return -1;
    char *p = strstr(buf, "\nbtime ");
    return p != NULL ? atoll(p + 7) : -1;
}

// /proc/<pid>/stat field 22, past a comm that may hold spaces.
static long long read_starttime(pid_t pid) {
    char path[64], buf[1024];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int) pid);
    if (read_file(path, buf, sizeof(buf)) <= 0)
        return -1;
    char *p = strrchr(buf, ')');
    if (p == NULL)
        return -1;
    p++;                                // the space before field 3
    for (int field = 3; field < 22; field++) {
        p = strchr(p + 1, ' ');         // the space before field + 1
        if (p == NULL)
            return -1;
    }
    return atoll(p + 1);
}

static pid_t child_pid;

static void kill_child(void) {
    if (child_pid > 0) {
        kill(child_pid, SIGKILL);
        waitpid(child_pid, NULL, 0);
        child_pid = 0;
    }
}

static int restored(void) {
    static char buf[8192];
    if (read_file("/proc/ish/checkpoint", buf, sizeof(buf)) <= 0)
        return 0;
    return strstr(buf, "restored        yes") != NULL;
}

// ---- the witness ----------------------------------------------------------

static pthread_mutex_t spin_lock = PTHREAD_MUTEX_INITIALIZER;
static double spin_gap, spin_before, spin_after;

static void *spinner(void *arg) {
    (void) arg;
    double last = now(CLOCK_REALTIME);
    struct timespec nap = {0, 5000000};
    for (;;) {
        nanosleep(&nap, NULL);
        double r = now(CLOCK_REALTIME);
        pthread_mutex_lock(&spin_lock);
        if (r - last > spin_gap) {
            spin_gap = r - last;
            spin_before = last;
            spin_after = r;
        }
        pthread_mutex_unlock(&spin_lock);
        last = r;
    }
    return NULL;
}

// ---- a CLOCK_MONOTONIC condition variable, waiting across the stop -------
//
// glibc's pthread_cond_timedwait on a CLOCK_MONOTONIC condvar is futex
// FUTEX_WAIT_BITSET with an ABSOLUTE deadline, the second shape of the bug.
// musl computes a relative wait from the absolute deadline instead; the
// restart re-issues that, so it returns on time but -- by a restarted clock --
// long before its deadline, and a caller that checks goes back to waiting.
// The deadline check below catches that shape too.

static pthread_mutex_t cv_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv;
static pthread_mutex_t cvrec_lock = PTHREAD_MUTEX_INITIALIZER;
struct cvrec {
    double start_real, ret_real, deadline, ret_mono;
    int rc, valid;
};
// Every recent wait, not just the last: they follow once a second, and the
// one the stop landed inside must still be there when it is looked for.
#define CV_RING 64
static struct cvrec cv_ring[CV_RING];
static unsigned cv_count;

static void *cond_waiter(void *arg) {
    (void) arg;
    for (;;) {
        double start_real = now(CLOCK_REALTIME);
        double deadline = now(CLOCK_MONOTONIC) + 1.0;
        struct timespec dl = to_ts(deadline);
        pthread_mutex_lock(&cv_lock);
        int rc;
        do
            rc = pthread_cond_timedwait(&cv, &cv_lock, &dl);
        while (rc == 0);    // nobody signals it; 0 is a spurious wake
        pthread_mutex_unlock(&cv_lock);
        double ret_mono = now(CLOCK_MONOTONIC);
        double ret_real = now(CLOCK_REALTIME);
        pthread_mutex_lock(&cvrec_lock);
        cv_ring[cv_count++ % CV_RING] =
            (struct cvrec) {start_real, ret_real, deadline, ret_mono, rc, 1};
        pthread_mutex_unlock(&cvrec_lock);
    }
    return NULL;
}

// ---- readings --------------------------------------------------------------

// Every clock bracketed by realtime reads, so a check can say exactly which
// realtime instant each reading belongs to however slow the machine is.
struct sample {
    double r0, mono, boot, raw, r1, uptime, r2;
};

static struct sample take_sample(void) {
    struct sample s;
    s.r0 = now(CLOCK_REALTIME);
    s.mono = now(CLOCK_MONOTONIC);
    s.boot = now(CLOCK_BOOTTIME);
    s.raw = now(CLOCK_MONOTONIC_RAW);
    s.r1 = now(CLOCK_REALTIME);
    s.uptime = read_uptime();
    s.r2 = now(CLOCK_REALTIME);
    return s;
}

// A 0.3 s absolute arming, made after the restore: how long it really took.
// The floor catches a deadline read as already past (an instant return); the
// ceiling catches one rebased onto the wrong clock, which after the stop is
// seconds away from the right one.
static void check_arming(const char *what, double took) {
    check(what, took >= 0.29 && took <= 1.3, "0.3 s absolute deadline took %.3f s", took);
}

static double abs_nanosleep(clockid_t c, double delta) {
    double r0 = now(CLOCK_REALTIME);
    struct timespec dl = to_ts(now(c) + delta);
    int rc;
    while ((rc = clock_nanosleep(c, TIMER_ABSTIME, &dl, NULL)) == EINTR)
        ;
    if (rc != 0)
        return -1;
    return now(CLOCK_REALTIME) - r0;
}

static double abs_timerfd(int tfd, clockid_t c, double delta) {
    double r0 = now(CLOCK_REALTIME);
    struct itimerspec its = {.it_value = to_ts(now(c) + delta)};
    if (timerfd_settime(tfd, TFD_TIMER_ABSTIME, &its, NULL) != 0)
        return -1;
    uint64_t n;
    if (read(tfd, &n, sizeof(n)) != (ssize_t) sizeof(n))
        return -1;
    return now(CLOCK_REALTIME) - r0;
}

static double abs_posix_timer(clockid_t c, double delta) {
    struct sigevent sev = {.sigev_notify = SIGEV_SIGNAL, .sigev_signo = SIGUSR1};
    timer_t t;
    if (timer_create(c, &sev, &t) != 0)
        return -1;
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    double r0 = now(CLOCK_REALTIME);
    struct itimerspec its = {.it_value = to_ts(now(c) + delta)};
    if (timer_settime(t, TIMER_ABSTIME, &its, NULL) != 0) {
        timer_delete(t);
        return -1;
    }
    struct timespec cap = {5, 0};   // relative: the thing under test is the arming
    int sig = sigtimedwait(&set, NULL, &cap);
    double took = now(CLOCK_REALTIME) - r0;
    timer_delete(t);
    return sig == SIGUSR1 ? took : -1;
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "after";
    int suspend = strcmp(mode, "suspend") == 0;
    // Unbuffered: a line still in stdio's buffer at the save is inside the
    // image, and the restored run would print it a second time.
    setvbuf(stdout, NULL, _IONBF, 0);

    // The POSIX timers' signal, blocked before any thread exists so that only
    // sigtimedwait ever takes it.
    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGUSR1);
    sigprocmask(SIG_BLOCK, &sigs, NULL);

    // A process with a start time well away from zero, whose /proc entry is
    // read before and after: a restore that stamped it with its own clock
    // reading would move it.
    struct timespec settle = {1, 500000000};
    nanosleep(&settle, NULL);
    pid_t child = fork();
    if (child == 0) {
        for (;;)
            pause();
    }
    // Gone however this ends. As pid 1 on the CLI its exit ends the machine
    // anyway; run from a shell on a device, an orphan would outlive the test.
    child_pid = child;
    atexit(kill_child);
    nanosleep(&(struct timespec) {0, 100000000}, NULL);
    long long child_start_pre = read_starttime(child);
    long long btime_pre = read_btime();

    // Two timerfds that travel in the image, disarmed, to be armed after the
    // restore: the image has to remember which guest clock each is on.
    int tfd_boot = timerfd_create(CLOCK_BOOTTIME, 0);
    int tfd_mono = timerfd_create(CLOCK_MONOTONIC, 0);

    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&cv, &ca);
    pthread_t th;
    pthread_create(&th, NULL, spinner, NULL);
    pthread_create(&th, NULL, cond_waiter, NULL);

    printf("PROBE-START %s mono=%.3f uptime=%.2f btime=%lld child-start=%lld\n",
           mode, now(CLOCK_MONOTONIC), read_uptime(), btime_pre, child_start_pre);

    struct sample pre, post;
    double deadline;
    pid_t asker = -1;
    if (suspend) {
        // Up to the uptime a broken restore would make the deadline wait for.
        struct timespec warm = to_ts(WARM_SECONDS);
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &warm, NULL) == EINTR)
            ;
        // Asked for once, and the sleeps below go on until it has happened:
        // the CLI stops the machine 0.3 s in, but the app does work of its own
        // before it freezes, and on an iPad that was past the end of a 1 s
        // sleep.
        asker = fork();
        if (asker == 0) {
            nanosleep(&(struct timespec) {0, 300000000}, NULL);
            int fd = open("/proc/ish/checkpoint", O_WRONLY);
            if (fd < 0 || write(fd, "suspend\n", 8) != 8)
                _exit(2);
            close(fd);
            _exit(0);
        }
    }
    for (int round = 0;; round++) {
        pre = take_sample();
        deadline = pre.mono + 1.0;
        struct timespec dl = to_ts(deadline);
        int rc;
        while ((rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &dl, NULL)) == EINTR)
            ;
        post = take_sample();
        if (rc != 0) {
            check("sleep", 0, "clock_nanosleep returned %d", rc);
            return 1;
        }
        // A 1 s sleep that took longer held the stop -- decided from the sleep
        // itself, because the witness may not have run yet if the deadline
        // was only milliseconds away when the machine came back.
        if (post.r0 - pre.r2 > 1.5 && restored())
            break;
        if (round > 60) {
            printf("NO-RESTORE after %d rounds\n", round);
            return 3;
        }
    }
    nanosleep(&(struct timespec) {0, 50000000}, NULL);   // the witness's turn
    pthread_mutex_lock(&spin_lock);
    double gap = spin_gap, stopped_at = spin_before, resumed_at = spin_after;
    pthread_mutex_unlock(&spin_lock);

    printf("pre : mono=%.6f boot=%.6f raw=%.6f uptime=%.2f\n",
           pre.mono, pre.boot, pre.raw, pre.uptime);
    printf("post: mono=%.6f boot=%.6f raw=%.6f uptime=%.2f\n",
           post.mono, post.boot, post.raw, post.uptime);
    printf("stop: %.3f s, the sleep woke %.3f s after the resume\n",
           gap, post.r0 - resumed_at);

    // The stop has to have happened INSIDE the sleep under test; otherwise
    // its deadline was taken after the restore and proves nothing. (Allowing
    // the witness its 5 ms steps either side; the stop is seconds long.)
    if (stopped_at < pre.r2 - 0.01 || resumed_at > post.r0 + 0.05) {
        printf("INCONCLUSIVE: the stop (%.3f..%.3f) was not inside the sleep (%.3f..%.3f)\n",
               stopped_at, resumed_at, pre.r2, post.r0);
        return 4;
    }

    // 1. THE bug. The deadline was at most a second ahead when the machine
    //    stopped, so it can have had at most that much left when it came back
    //    -- not the saved uptime. And it did sleep to its deadline.
    double woke_after = post.r0 - resumed_at;
    check("wake", woke_after <= 2.0 && post.mono >= deadline - 0.0001,
          "woke %.3f s after the resume (deadline %.3f, monotonic at wake %.3f, "
          "uptime at the save about %.1f)", woke_after, deadline, post.mono, pre.uptime);

    // 2. Uptime never goes backward across a restore, and like BOOTTIME it
    //    counts the stop: it advanced by exactly the wall-clock time between
    //    the two reads, within its hundredths.
    double up_lo = (post.r1 - pre.r2) - 0.02, up_hi = (post.r2 - pre.r1) + 0.02;
    double up_d = post.uptime - pre.uptime;
    check("uptime", post.uptime >= pre.uptime && up_d >= up_lo && up_d <= up_hi,
          "/proc/uptime %.2f -> %.2f (+%.2f, the wall clock says %.2f..%.2f)",
          pre.uptime, post.uptime, up_d, up_lo, up_hi);

    // 3. CLOCK_MONOTONIC went on from where it was and did NOT count the stop.
    //    It may count part of what the spinner saw as the stop, and must: the
    //    clocks are read once EVERY task has parked -- read earlier, a task
    //    that parked later would see MONOTONIC go backward -- and on an iPad
    //    parking everything took about a second, all of it counted, as Linux's
    //    freezer counts it before a hibernation. So the upper bound asks only
    //    that at least half of the stop was skipped; a clock that counted all
    //    of it, as BOOTTIME does, is seconds over.
    double mono_d = post.mono - pre.mono;
    double mono_lo = (post.r0 - pre.r1) - gap - 0.01;
    double mono_hi = (post.r1 - pre.r0) - gap / 2;
    check("monotonic", post.mono >= pre.mono && mono_d >= mono_lo && mono_d <= mono_hi,
          "%.3f -> %.3f (+%.3f; running time %.3f..%.3f)",
          pre.mono, post.mono, mono_d, mono_lo, mono_hi);
    double raw_d = post.raw - pre.raw;
    check("monotonic-raw", post.raw >= pre.raw && raw_d >= mono_lo - 0.01 && raw_d <= mono_hi,
          "%.3f -> %.3f (+%.3f)", pre.raw, post.raw, raw_d);

    // 4. CLOCK_BOOTTIME DID count the stop, exactly: it advanced by the
    //    wall-clock time between the two readings, bracketed.
    double boot_d = post.boot - pre.boot;
    double boot_lo = (post.r0 - pre.r1) - 0.05, boot_hi = (post.r1 - pre.r0) + 0.05;
    check("boottime", boot_d >= boot_lo && boot_d <= boot_hi,
          "%.3f -> %.3f (+%.3f, the wall clock says %.3f..%.3f)",
          pre.boot, post.boot, boot_d, boot_lo, boot_hi);
    check("order", post.mono <= post.boot + 0.0001,
          "monotonic %.3f <= boottime %.3f", post.mono, post.boot);

    // 5. btime is when the machine booted, and a restore is not a boot.
    long long btime_post = read_btime();
    check("btime", btime_post >= 0 && llabs(btime_post - btime_pre) <= 1,
          "%lld -> %lld", btime_pre, btime_post);

    // 6. A process's start time still means what it did.
    long long child_start_post = read_starttime(child);
    check("starttime", child_start_post == child_start_pre &&
                       (double) child_start_post / 100.0 <= post.uptime + 0.01,
          "pid %d started at %lld ticks, now %lld (uptime %.2f)",
          (int) child, child_start_pre, child_start_post, post.uptime);

    // 7. The condition variable that was waiting when the machine stopped.
    // Spanning: begun before the spinner's last reading ahead of the stop and
    // returned after the stop. The 50 ms covers a wait that ends in the few
    // ms before the spinner's first reading after it; the stop is seconds.
    struct cvrec rec = {0};
    for (int i = 0; i < 400 && !rec.valid; i++) {
        pthread_mutex_lock(&cvrec_lock);
        for (unsigned k = 0; k < CV_RING && k < cv_count; k++) {
            struct cvrec r = cv_ring[k];
            if (r.valid && r.start_real <= stopped_at && r.ret_real >= resumed_at - 0.05)
                rec = r;
        }
        pthread_mutex_unlock(&cvrec_lock);
        if (!rec.valid)
            nanosleep(&(struct timespec) {0, 25000000}, NULL);
    }
    if (!rec.valid)
        check("condvar", 0, "no CLOCK_MONOTONIC wait spanning the stop returned within 10 s");
    else
        check("condvar", rec.rc == ETIMEDOUT && rec.ret_real - resumed_at <= 2.0 &&
                         rec.ret_mono >= rec.deadline - 0.0001,
              "rc %d, returned %.3f s after the resume (deadline %.3f, monotonic %.3f)",
              rec.rc, rec.ret_real - resumed_at, rec.deadline, rec.ret_mono);

    // 8. Absolute armings made after the restore, on each clock. After a stop
    //    MONOTONIC and BOOTTIME read seconds apart, so one rebased onto the
    //    other's origin is seconds late.
    check_arming("nanosleep-monotonic", abs_nanosleep(CLOCK_MONOTONIC, 0.3));
    check_arming("nanosleep-boottime", abs_nanosleep(CLOCK_BOOTTIME, 0.3));
    check_arming("timerfd-boottime-restored", abs_timerfd(tfd_boot, CLOCK_BOOTTIME, 0.3));
    check_arming("timerfd-monotonic-restored", abs_timerfd(tfd_mono, CLOCK_MONOTONIC, 0.3));
    int tfd_new = timerfd_create(CLOCK_BOOTTIME, 0);
    check_arming("timerfd-boottime-new", abs_timerfd(tfd_new, CLOCK_BOOTTIME, 0.3));
    check_arming("posix-timer-boottime", abs_posix_timer(CLOCK_BOOTTIME, 0.3));
    check_arming("posix-timer-monotonic", abs_posix_timer(CLOCK_MONOTONIC, 0.3));

    kill_child();
    if (asker > 0)
        waitpid(asker, NULL, 0);
    printf("CLOCK-PROBE-DONE failures=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
