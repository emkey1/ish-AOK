/*
 * signal_restart_record.c -- whether a syscall cut short by a signal restarts
 * is decided by that signal, never by an earlier syscall's interruption; a
 * handled signal outside rt_sigtimedwait's set ends it with EINTR; and a
 * job-control stop ends only the waits Linux ends.
 *
 * When a signal interrupts a wait parked on a cond_t, AOK records whether the
 * interrupted syscall may be restarted (wake_waiting_task), and the next
 * restart decision reads that record before it looks at the signal. Only the
 * calls that make a decision used to clear it. sigsuspend, rt_sigtimedwait,
 * msgrcv, msgsnd and semop answer EINTR without making one, and a wait that
 * completes -- an rt_sigtimedwait that takes its signal, a wait4 that reaps the
 * child whose SIGCHLD interrupted it -- makes none either. Each left the record
 * behind. A pipe read blocks in the host rather than on a cond_t, so a signal
 * interrupting it records nothing, and its decision found the stale answer: a
 * handler WITHOUT SA_RESTART restarted the read, which returned data that came
 * 400ms later instead of failing with EINTR. The record is now cleared at every
 * syscall entry.
 *
 * sigsuspend, pause, msgrcv and msgsnd are ERESTARTNOHAND calls on Linux: a
 * handler running ends them with EINTR, but a stop does not -- once continued,
 * they go back to waiting. AOK failed them with EINTR as soon as the process
 * was continued, because they never asked whether the interruption was a stop.
 *
 * Restarting across a stop has a second piece of state: the dispatcher notes
 * that it rewound an ERESTARTNOHAND call, so a handler that runs before the call
 * re-executes cancels the restart. On amd64 nothing cleared that note when the
 * re-executed call then ended, so a handler ending it "cancelled" a restart
 * that was not there, stepped the program counter two bytes forward, and the
 * process died with SIGSEGV. nanosleep had it before sigsuspend could restart;
 * both notes are now cleared at every syscall entry.
 *
 * The mark a signal leaves when it wakes a wait had two leaks of its own. The
 * sender stored it only after the wake, so the task could be through its
 * handler and into its next syscall first, and the late mark cut that
 * syscall's wait short with no signal pending; the stale record used to turn
 * that EINTR into a restart. And it was stored on a vfork parent's wait, which
 * ignores signals and never consumes it, so the parent's next sigsuspend or
 * rt_sigtimedwait returned at once. The interruption is now recorded before the
 * wake, and only for a wait that consumes it.
 *
 * What is checked, with the elapsed time asserted on every call:
 *   - record: each of sigsuspend, rt_sigtimedwait ended by a signal outside its
 *     set, rt_sigtimedwait taking the signal it waits for, pause, msgrcv,
 *     msgsnd, semop, and wait4 reaping a child, is ended at 200ms by a signal
 *     whose handler has SA_RESTART. Then a pipe read is interrupted at 200ms by
 *     a handler without SA_RESTART, with a byte written at 1000ms: it fails
 *     with EINTR at ~200ms. Two controls on the read alone: it fails the same
 *     way, and with SA_RESTART it restarts and returns the byte at ~1000ms --
 *     so the read can restart, and the EINTR is the kernel's answer
 *   - vfork: an SA_RESTART signal at 200ms, while a vfork parent waits for a
 *     child that exits at 400ms; then the parent's sigsuspend is ended by a
 *     handled SIGUSR2 at ~1000ms, and its rt_sigtimedwait takes SIGUSR2 at
 *     ~1000ms, rather than either returning EINTR as vfork returns
 *   - sigtimedwait: waiting for SIGUSR2 with a 2s timeout, a SIGALRM handler
 *     at 200ms, with and without SA_RESTART, ends the call with EINTR at
 *     ~200ms, and so it does for sigwaitinfo with no timeout. The control: a
 *     300ms timeout with no signal ends with EAGAIN at ~300ms
 *   - storm: signalfd reads for SIGUSR2, sent every ~0.7ms, under SIGUSR1
 *     handled with SA_RESTART every 2ms from another process, for 4s: no read
 *     fails with EINTR, and at least 100 handlers ran. A signalfd read is one
 *     of the waits that still ends on the bare mark, so a late mark shows here
 *     (eventfd, pipe and socket waits now take it for a spurious wakeup)
 *   - stop: SIGSTOP at 200ms, SIGCONT at 400ms. sigsuspend, pause, msgrcv,
 *     msgsnd, nanosleep and clock_nanosleep end at 1000ms, when what they wait
 *     for arrives -- a handled signal, for the ones that wait for nothing else,
 *     and the process must survive it; rt_sigtimedwait and semop fail with
 *     EINTR at ~400ms, as signal(7) documents. Those two are also the proof
 *     that the stop really happened.
 *
 * rt_sigtimedwait is issued as a raw syscall, NOT through sigtimedwait() or
 * sigwaitinfo(): musl's wrappers retry on EINTR, on Linux as much as on AOK, so
 * a musl guest waits out the whole timeout and reports EAGAIN whatever the
 * kernel did. Do not "simplify" these calls back to the libc wrappers.
 *
 * Checked against Linux 6.12 (Devuan 6, x86_64 and -m32, unprivileged).
 *
 * Exits 0 and prints "signal_restart_record: PASS" on success.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/signalfd.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TEST_WANT_RAW_VFORK   // musl powerpc64: the system call, not a fork
#include "test_common.h"

/* The interrupting signal, or the stop, lands this far in. */
#define SIGNAL_AT_MS 200
/* A stopped process is continued here. */
#define CONT_AT_MS 400
/* What really ends a call: data, room, a message, a handled signal. Far enough
 * past the interruption that ending at one cannot pass for ending at the other. */
#define ARRIVAL_MS 1000
/* rt_sigtimedwait's timeout when a signal is meant to end it first. */
#define TIMEOUT_MS 2000
/* The control's timeout, with no signal at all. */
#define CONTROL_TIMEOUT_MS 300
/* How early a call may end: a millisecond clock read twice. */
#define EARLY_MS 10
/* The storm: how long it lasts, and how many handlers must have run for it to
 * count as one. */
#define STORM_MS 4000
#define STORM_MIN_HANDLERS 100

/* musl on i386 names the 32-bit-time calls for what they take. */
#if !defined(SYS_clock_nanosleep) && defined(SYS_clock_nanosleep_time32)
#define SYS_clock_nanosleep SYS_clock_nanosleep_time32
#endif
#if !defined(SYS_rt_sigtimedwait) && defined(SYS_rt_sigtimedwait_time32)
#define SYS_rt_sigtimedwait SYS_rt_sigtimedwait_time32
#endif

/* The layout rt_sigtimedwait, nanosleep and clock_nanosleep take on every ABI
 * this test builds for: a long pair, which is 64-bit on x86_64, arm64 and
 * riscv64 and 32-bit on i386. The kernel's sigset is 8 bytes on all of them. */
struct kernel_timespec {
    long tv_sec;
    long tv_nsec;
};

static volatile sig_atomic_t alrm_hits;
static volatile sig_atomic_t usr1_hits;
static volatile sig_atomic_t usr2_hits;
static volatile sig_atomic_t chld_hits;

static void on_signal(int sig) {
    if (sig == SIGALRM)
        alrm_hits++;
    else if (sig == SIGUSR1)
        usr1_hits++;
    else if (sig == SIGUSR2)
        usr2_hits++;
    else if (sig == SIGCHLD)
        chld_hits++;
}

static void check(int ok, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (!ok || test_verbose) {
        printf("%s ", ok ? "ok" : "FAIL");
        vprintf(fmt, ap);
        printf("\n");
    }
    va_end(ap);
    if (!ok)
        failures_total++;
}

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Sleep until `deadline` on the monotonic clock, which every process shares, so
 * a helper's schedule is measured from its parent's start and not from whenever
 * the helper got to run. */
static void sleep_until(long deadline) {
    for (;;) {
        long left = deadline - now_ms();
        if (left <= 0)
            return;
        struct timespec ts = {.tv_sec = left / 1000, .tv_nsec = (left % 1000) * 1000000L};
        nanosleep(&ts, NULL);
    }
}

/* How late a call may end and still count, scaled like the watchdogs for a
 * heavily loaded run. Only upper bounds use it: a loaded machine cannot make a
 * call end early. */
static long slack_ms(long base) {
    return base * (long) test_watchdog_secs(1);
}

static bool near(long elapsed, long at) {
    return elapsed >= at - EARLY_MS && elapsed < at + slack_ms(500);
}

static void install(int sig, int flags) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sa.sa_flags = flags;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
}

static void alarm_in_ms(long ms) {
    struct itimerval it;
    memset(&it, 0, sizeof(it));
    it.it_value.tv_sec = ms / 1000;
    it.it_value.tv_usec = (ms % 1000) * 1000;
    setitimer(ITIMER_REAL, &it, NULL);
}

static void block(int sig) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, sig);
    sigprocmask(SIG_BLOCK, &set, NULL);
}

static const char *errname(long r, int err) {
    return r < 0 ? strerror(err) : "-";
}

static void reap(pid_t pid) {
    if (pid <= 0)
        return;
    kill(pid, SIGKILL);
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
        continue;
}

/* ---- the calls ------------------------------------------------------------ */

static long raw_sigtimedwait(int sig, long timeout_ms) {
    uint64_t set = 1ull << (sig - 1);
    struct kernel_timespec ts = {timeout_ms / 1000, (timeout_ms % 1000) * 1000000L};
    return syscall(SYS_rt_sigtimedwait, &set, NULL, timeout_ms < 0 ? NULL : &ts, sizeof(set));
}

static long raw_pause(void) {
#ifdef SYS_pause
    return syscall(SYS_pause);
#else
    return pause(); /* arm64 and riscv64 have no pause: libc uses ppoll */
#endif
}

static long raw_nanosleep(void) {
    struct kernel_timespec ts = {5, 0};
    return syscall(SYS_nanosleep, &ts, NULL);
}

static long raw_clock_nanosleep(void) {
    struct kernel_timespec ts = {5, 0};
    return syscall(SYS_clock_nanosleep, CLOCK_MONOTONIC, 0, &ts, NULL);
}

static long do_sigsuspend(void) {
    sigset_t none;
    sigemptyset(&none);
    return sigsuspend(&none);
}

struct test_msg {
    long mtype;
    char mtext[1024];
};

static int queue_id = -1;
static int sem_id = -1;

static bool make_queue(void) {
    queue_id = msgget(IPC_PRIVATE, 0600);
    if (queue_id < 0)
        check(0, "msgget (%s)", strerror(errno));
    return queue_id >= 0;
}

static bool fill_queue(void) {
    struct test_msg m = {.mtype = 1};
    int n = 0;
    while (n < 1000 && msgsnd(queue_id, &m, sizeof(m.mtext), IPC_NOWAIT) == 0)
        n++;
    bool full = n > 0 && n < 1000 && errno == EAGAIN;
    if (!full)
        check(0, "fill a message queue: %d messages, then %s (want EAGAIN)", n, strerror(errno));
    return full;
}

static bool make_sem(void) {
    sem_id = semget(IPC_PRIVATE, 1, 0600);
    if (sem_id < 0)
        check(0, "semget (%s)", strerror(errno));
    return sem_id >= 0;
}

static void remove_ipc(void) {
    if (queue_id >= 0)
        msgctl(queue_id, IPC_RMID, NULL);
    if (sem_id >= 0)
        semctl(sem_id, 0, IPC_RMID);
    queue_id = sem_id = -1;
}

static long do_msgrcv(void) {
    struct test_msg m;
    return msgrcv(queue_id, &m, sizeof(m.mtext), 0, 0);
}

static long do_msgsnd(void) {
    struct test_msg m = {.mtype = 1};
    return msgsnd(queue_id, &m, sizeof(m.mtext), 0);
}

static long do_semop(void) {
    struct sembuf down = {.sem_num = 0, .sem_op = -1, .sem_flg = 0};
    return semop(sem_id, &down, 1);
}

/* ---- record: an answered interruption does not decide the next one ------- */

/* A pipe read, a handler without SA_RESTART at SIGNAL_AT_MS, a byte at
 * ARRIVAL_MS. `restart` installs the handler with SA_RESTART instead, which is
 * the control that shows the read can restart at all. */
static void pipe_read(const char *after, bool restart) {
    int p[2];
    if (pipe(p) < 0) {
        check(0, "pipe (%s)", strerror(errno));
        return;
    }
    install(SIGUSR1, restart ? SA_RESTART : 0);
    usr1_hits = 0;
    pid_t self = getpid();
    long start = now_ms();
    fflush(stdout);
    pid_t helper = fork();
    if (helper < 0) {
        check(0, "fork (%s)", strerror(errno));
        return;
    }
    if (helper == 0) {
        sleep_until(start + SIGNAL_AT_MS);
        kill(self, SIGUSR1);
        sleep_until(start + ARRIVAL_MS);
        (void) !write(p[1], "d", 1);
        _exit(0);
    }
    char buf[8];
    errno = 0;
    ssize_t r = read(p[0], buf, sizeof(buf));
    int err = errno;
    long elapsed = now_ms() - start;
    reap(helper);
    close(p[0]);
    close(p[1]);
    if (restart) {
        check(r == 1 && buf[0] == 'd' && usr1_hits == 1 && near(elapsed, ARRIVAL_MS),
              "a pipe read, an SA_RESTART handler at %dms and a byte at %dms: ret=%zd "
              "errno=%s handler=%d after %ldms (want the byte, handler=1, after ~%dms)",
              SIGNAL_AT_MS, ARRIVAL_MS, r, errname(r, err), (int) usr1_hits, elapsed, ARRIVAL_MS);
        return;
    }
    check(r == -1 && err == EINTR && usr1_hits == 1 && near(elapsed, SIGNAL_AT_MS),
          "after %s, a pipe read, a handler without SA_RESTART at %dms and a byte at %dms: "
          "ret=%zd errno=%s handler=%d after %ldms (want -1 EINTR handler=1 after ~%dms, "
          "not a restart that returns the byte)",
          after, SIGNAL_AT_MS, ARRIVAL_MS, r, errname(r, err), (int) usr1_hits, elapsed,
          SIGNAL_AT_MS);
}

static void control_read_eintr(void) {
    pipe_read("nothing", false);
}

static void control_read_restart(void) {
    pipe_read("nothing", true);
}

/* A call ended by an SA_RESTART SIGALRM at SIGNAL_AT_MS, then the read. */
static void ended_by_restart_handler(const char *what, long (*call)(void)) {
    install(SIGALRM, SA_RESTART);
    alrm_hits = 0;
    long start = now_ms();
    alarm_in_ms(SIGNAL_AT_MS);
    errno = 0;
    long r = call();
    int err = errno;
    long elapsed = now_ms() - start;
    check(r == -1 && err == EINTR && alrm_hits == 1 && near(elapsed, SIGNAL_AT_MS),
          "%s, an SA_RESTART handler at %dms: ret=%ld errno=%s handler=%d after %ldms "
          "(want -1 EINTR handler=1 after ~%dms)",
          what, SIGNAL_AT_MS, r, errname(r, err), (int) alrm_hits, elapsed, SIGNAL_AT_MS);
    remove_ipc();
    pipe_read(what, false);
}

static long sigtimedwait_other(void) {
    block(SIGUSR2);
    return raw_sigtimedwait(SIGUSR2, TIMEOUT_MS);
}

static void record_sigsuspend(void) {
    ended_by_restart_handler("sigsuspend", do_sigsuspend);
}

static void record_sigtimedwait(void) {
    ended_by_restart_handler("rt_sigtimedwait for SIGUSR2", sigtimedwait_other);
}

static void record_pause(void) {
    ended_by_restart_handler("pause", raw_pause);
}

static void record_msgrcv(void) {
    if (make_queue())
        ended_by_restart_handler("msgrcv on an empty queue", do_msgrcv);
}

static void record_msgsnd(void) {
    if (make_queue() && fill_queue())
        ended_by_restart_handler("msgsnd to a full queue", do_msgsnd);
}

static void record_semop(void) {
    if (make_sem())
        ended_by_restart_handler("semop on a zero semaphore", do_semop);
}

/* rt_sigtimedwait takes the signal it waits for, whose handler has SA_RESTART:
 * the call completes, and the record its interruption left must go with it. */
static void record_sigtimedwait_takes(void) {
    install(SIGUSR2, SA_RESTART);
    block(SIGUSR2);
    usr2_hits = 0;
    pid_t self = getpid();
    long start = now_ms();
    fflush(stdout);
    pid_t helper = fork();
    if (helper < 0) {
        check(0, "fork (%s)", strerror(errno));
        return;
    }
    if (helper == 0) {
        sleep_until(start + SIGNAL_AT_MS);
        kill(self, SIGUSR2);
        _exit(0);
    }
    errno = 0;
    long r = raw_sigtimedwait(SIGUSR2, TIMEOUT_MS);
    int err = errno;
    long elapsed = now_ms() - start;
    reap(helper);
    check(r == SIGUSR2 && usr2_hits == 0 && near(elapsed, SIGNAL_AT_MS),
          "rt_sigtimedwait for SIGUSR2 (handler with SA_RESTART), sent at %dms: ret=%ld "
          "errno=%s handler=%d after %ldms (want %d, handler=0, after ~%dms)",
          SIGNAL_AT_MS, r, errname(r, err), (int) usr2_hits, elapsed, SIGUSR2, SIGNAL_AT_MS);
    pipe_read("an rt_sigtimedwait that took its signal", false);
}

/* wait4 reaps a child that exits at SIGNAL_AT_MS, with SIGCHLD handled with
 * SA_RESTART: the child's SIGCHLD interrupts the wait, which then completes. */
static void record_wait4_reaps(void) {
    install(SIGCHLD, SA_RESTART);
    chld_hits = 0;
    long start = now_ms();
    fflush(stdout);
    pid_t child = fork();
    if (child < 0) {
        check(0, "fork (%s)", strerror(errno));
        return;
    }
    if (child == 0) {
        sleep_until(start + SIGNAL_AT_MS);
        _exit(7);
    }
    int status = 0;
    errno = 0;
    long r = wait4(child, &status, 0, NULL);
    int err = errno;
    long elapsed = now_ms() - start;
    check(r == child && WIFEXITED(status) && WEXITSTATUS(status) == 7 && chld_hits == 1 &&
              near(elapsed, SIGNAL_AT_MS),
          "wait4 for a child that exits at %dms, SIGCHLD handled with SA_RESTART: ret=%ld "
          "errno=%s status=%#x handler=%d after %ldms (want the child, exit 7, handler=1, "
          "after ~%dms)",
          SIGNAL_AT_MS, r, errname(r, err), status, (int) chld_hits, elapsed, SIGNAL_AT_MS);
    pipe_read("a wait4 that reaped a child", false);
}

/* A signal handled with SA_RESTART lands while a vfork parent waits for its
 * child. That wait ignores signals and consumes no interruption, so nothing
 * may be left behind by it: the parent's next wait waits. */
static const struct kernel_timespec vfork_child_nap = {0, 400000000L};

static void vfork_then(bool timedwait) {
    const char *what = timedwait ? "rt_sigtimedwait for SIGUSR2" : "sigsuspend";
    install(SIGUSR1, SA_RESTART);
    install(SIGUSR2, SA_RESTART);
    if (timedwait)
        block(SIGUSR2);
    usr1_hits = usr2_hits = 0;
    pid_t self = getpid();
    long start = now_ms();
    fflush(stdout);
    pid_t helper = fork();
    if (helper < 0) {
        check(0, "fork (%s)", strerror(errno));
        return;
    }
    if (helper == 0) {
        sleep_until(start + SIGNAL_AT_MS);
        kill(self, SIGUSR1);
        sleep_until(start + ARRIVAL_MS);
        kill(self, SIGUSR2);
        _exit(0);
    }
    /* Only a syscall and _exit in the child: it runs on the parent's stack. */
    pid_t child = vfork();
    if (child == 0) {
        syscall(SYS_nanosleep, &vfork_child_nap, NULL);
        _exit(0);
    }
    int vfork_err = errno;
    long vforked = now_ms() - start;
    long r;
    errno = 0;
    if (timedwait) {
        r = raw_sigtimedwait(SIGUSR2, 3000);
    } else {
        sigset_t none;
        sigemptyset(&none);
        r = sigsuspend(&none);
    }
    int err = errno;
    long elapsed = now_ms() - start;
    reap(helper);
    if (child > 0)
        while (waitpid(child, NULL, 0) < 0 && errno == EINTR)
            continue;
    if (child < 0) {
        check(0, "vfork (%s)", strerror(vfork_err));
        return;
    }
    bool ended = timedwait ? r == SIGUSR2 && usr2_hits == 0 :
        r == -1 && err == EINTR && usr2_hits == 1;
    check(near(vforked, 400) && usr1_hits == 1 && ended && near(elapsed, ARRIVAL_MS),
          "an SA_RESTART handler at %dms during vfork (child exits at 400ms, vfork returned "
          "after %ldms), then %s, SIGUSR2 at %dms: ret=%ld errno=%s SIGUSR2 handler=%d after "
          "%ldms (want %s after ~%dms, not a return as vfork does)",
          SIGNAL_AT_MS, vforked, what, ARRIVAL_MS, r, errname(r, err), (int) usr2_hits, elapsed,
          timedwait ? "SIGUSR2 taken, handler=0," : "-1 EINTR, handler=1,", ARRIVAL_MS);
}

static void vfork_then_sigsuspend(void) {
    vfork_then(false);
}

static void vfork_then_sigtimedwait(void) {
    vfork_then(true);
}

/* ---- storm: an interruption is recorded while the wait still waits ------- */

/* A signalfd read, blocked on a cond_t and restarted over and over by
 * SA_RESTART handlers. Linux never fails it with EINTR. AOK did, now and then,
 * whenever a sender marked the reader's wait after the reader had left it: the
 * late mark ended the NEXT wait with no signal pending. It takes a storm to
 * land in that window, so this counts rather than times one call, and bounds
 * the whole run instead. */
static void storm_signalfd_read(void) {
    install(SIGUSR1, SA_RESTART);
    usr1_hits = 0;
    block(SIGUSR2);
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR2);
    int sfd = signalfd(-1, &set, 0);
    if (sfd < 0) {
        check(0, "signalfd (%s)", strerror(errno));
        return;
    }
    pid_t self = getpid();
    fflush(stdout);
    pid_t signaller = fork();
    if (signaller == 0) {
        struct timespec gap = {.tv_sec = 0, .tv_nsec = 2000000L};
        for (;;) {
            kill(self, SIGUSR1);
            nanosleep(&gap, NULL);
        }
    }
    pid_t feeder = fork();
    if (feeder == 0) {
        struct timespec gap = {.tv_sec = 0, .tv_nsec = 700000L};
        for (;;) {
            kill(self, SIGUSR2);
            nanosleep(&gap, NULL);
        }
    }
    if (signaller < 0 || feeder < 0) {
        check(0, "fork (%s)", strerror(errno));
        reap(signaller);
        reap(feeder);
        close(sfd);
        return;
    }
    long reads = 0, eintr = 0, other = 0;
    int other_errno = 0;
    long start = now_ms();
    while (now_ms() - start < STORM_MS) {
        struct signalfd_siginfo si;
        errno = 0;
        ssize_t r = read(sfd, &si, sizeof(si));
        if (r == (ssize_t) sizeof(si)) {
            reads++;
        } else if (r < 0 && errno == EINTR) {
            eintr++;
        } else {
            other++;
            other_errno = errno;
        }
    }
    long elapsed = now_ms() - start;
    reap(signaller);
    reap(feeder);
    close(sfd);
    check(eintr == 0 && other == 0 && reads > 0 && usr1_hits >= STORM_MIN_HANDLERS &&
              elapsed >= STORM_MS && elapsed < STORM_MS + slack_ms(1000),
          "signalfd reads for %dms, SIGUSR2 every ~0.7ms, SIGUSR1 with SA_RESTART every 2ms: "
          "%ld reads, %ld EINTR, %ld other (%s), %d handlers, %ldms (want no EINTR and at least "
          "%d handlers)",
          STORM_MS, reads, eintr, other, other ? strerror(other_errno) : "-", (int) usr1_hits,
          elapsed, STORM_MIN_HANDLERS);
}

/* ---- sigtimedwait: a handled signal outside the set ends it --------------- */

static void sigtimedwait_interrupted(int flags, long timeout_ms, const char *what) {
    install(SIGALRM, flags);
    block(SIGUSR2);
    alrm_hits = 0;
    long start = now_ms();
    alarm_in_ms(SIGNAL_AT_MS);
    errno = 0;
    long r = raw_sigtimedwait(SIGUSR2, timeout_ms);
    int err = errno;
    long elapsed = now_ms() - start;
    check(r == -1 && err == EINTR && alrm_hits == 1 && near(elapsed, SIGNAL_AT_MS),
          "%s for SIGUSR2, a SIGALRM handler %s at %dms: ret=%ld errno=%s handler=%d after "
          "%ldms (want -1 EINTR handler=1 after ~%dms)",
          what, flags & SA_RESTART ? "with SA_RESTART" : "without SA_RESTART", SIGNAL_AT_MS, r,
          errname(r, err), (int) alrm_hits, elapsed, SIGNAL_AT_MS);
}

static void sigtimedwait_restart_handler(void) {
    sigtimedwait_interrupted(SA_RESTART, TIMEOUT_MS, "rt_sigtimedwait (2s)");
}

static void sigtimedwait_plain_handler(void) {
    sigtimedwait_interrupted(0, TIMEOUT_MS, "rt_sigtimedwait (2s)");
}

static void sigwaitinfo_restart_handler(void) {
    sigtimedwait_interrupted(SA_RESTART, -1, "rt_sigtimedwait (no timeout)");
}

static void sigtimedwait_control(void) {
    block(SIGUSR2);
    long start = now_ms();
    errno = 0;
    long r = raw_sigtimedwait(SIGUSR2, CONTROL_TIMEOUT_MS);
    int err = errno;
    long elapsed = now_ms() - start;
    check(r == -1 && err == EAGAIN && near(elapsed, CONTROL_TIMEOUT_MS),
          "rt_sigtimedwait for SIGUSR2, %dms timeout, no signal: ret=%ld errno=%s after %ldms "
          "(want -1 EAGAIN after ~%dms)",
          CONTROL_TIMEOUT_MS, r, errname(r, err), elapsed, CONTROL_TIMEOUT_MS);
}

/* ---- stop: which waits a stop and a continue end -------------------------- */

enum arrival { ARRIVE_NOTHING, ARRIVE_SIGUSR1, ARRIVE_MESSAGE, ARRIVE_ROOM, ARRIVE_SEM_POST };

/* SIGSTOP at SIGNAL_AT_MS, SIGCONT at CONT_AT_MS, and at ARRIVAL_MS whatever
 * really ends the call. `resumes` says which way Linux goes: back to waiting,
 * ending at ARRIVAL_MS, or EINTR once continued. */
static void stopped(const char *what, long (*call)(void), enum arrival arrival, bool resumes,
        long want_ret) {
    install(SIGUSR1, 0);
    usr1_hits = 0;
    pid_t self = getpid();
    long start = now_ms();
    fflush(stdout);
    pid_t helper = fork();
    if (helper < 0) {
        check(0, "fork (%s)", strerror(errno));
        return;
    }
    if (helper == 0) {
        sleep_until(start + SIGNAL_AT_MS);
        kill(self, SIGSTOP);
        sleep_until(start + CONT_AT_MS);
        kill(self, SIGCONT);
        sleep_until(start + ARRIVAL_MS);
        struct test_msg m = {.mtype = 1};
        struct sembuf up = {.sem_num = 0, .sem_op = 1, .sem_flg = 0};
        switch (arrival) {
        case ARRIVE_NOTHING:
            break;
        case ARRIVE_SIGUSR1:
            kill(self, SIGUSR1);
            break;
        case ARRIVE_MESSAGE:
            (void) !msgsnd(queue_id, &m, 8, 0);
            break;
        case ARRIVE_ROOM:
            while (msgrcv(queue_id, &m, sizeof(m.mtext), 0, IPC_NOWAIT) >= 0)
                continue;
            break;
        case ARRIVE_SEM_POST:
            (void) !semop(sem_id, &up, 1);
            break;
        }
        _exit(0);
    }
    errno = 0;
    long r = call();
    int err = errno;
    long elapsed = now_ms() - start;
    reap(helper);
    remove_ipc();
    if (resumes) {
        bool ok = r == want_ret && (want_ret != -1 || err == EINTR) &&
            usr1_hits == (arrival == ARRIVE_SIGUSR1) && near(elapsed, ARRIVAL_MS);
        check(ok, "%s, stopped at %dms and continued at %dms, ended at %dms: ret=%ld "
              "errno=%s handler=%d after %ldms (want ret=%ld after ~%dms: a stop does not "
              "end it)",
              what, SIGNAL_AT_MS, CONT_AT_MS, ARRIVAL_MS, r, errname(r, err), (int) usr1_hits,
              elapsed, want_ret, ARRIVAL_MS);
        return;
    }
    check(r == -1 && err == EINTR && usr1_hits == 0 && near(elapsed, CONT_AT_MS),
          "%s, stopped at %dms and continued at %dms: ret=%ld errno=%s after %ldms "
          "(want -1 EINTR after ~%dms)",
          what, SIGNAL_AT_MS, CONT_AT_MS, r, errname(r, err), elapsed, CONT_AT_MS);
}

static void stop_sigsuspend(void) {
    stopped("sigsuspend", do_sigsuspend, ARRIVE_SIGUSR1, true, -1);
}

static void stop_pause(void) {
    stopped("pause", raw_pause, ARRIVE_SIGUSR1, true, -1);
}

static void stop_nanosleep(void) {
    stopped("nanosleep (5s)", raw_nanosleep, ARRIVE_SIGUSR1, true, -1);
}

static void stop_clock_nanosleep(void) {
    stopped("clock_nanosleep (5s)", raw_clock_nanosleep, ARRIVE_SIGUSR1, true, -1);
}

static void stop_msgrcv(void) {
    if (make_queue())
        stopped("msgrcv on an empty queue", do_msgrcv, ARRIVE_MESSAGE, true, 8);
}

static void stop_msgsnd(void) {
    if (make_queue() && fill_queue())
        stopped("msgsnd to a full queue", do_msgsnd, ARRIVE_ROOM, true, 0);
}

static void stop_sigtimedwait(void) {
    stopped("rt_sigtimedwait for SIGUSR2 (2s)", sigtimedwait_other, ARRIVE_NOTHING, false, -1);
}

static void stop_semop(void) {
    if (make_sem())
        stopped("semop on a zero semaphore", do_semop, ARRIVE_SEM_POST, false, -1);
}

/* Each case in a child of its own: fresh dispositions, a fresh mask, and a
 * hang that costs one case rather than the run. */
static void run(void (*scenario)(void), const char *name) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        check(0, "%s: fork (%s)", name, strerror(errno));
        return;
    }
    if (pid == 0) {
        failures_total = 0;
        scenario();
        fflush(stdout);
        _exit(failures_total > 100 ? 100 : (int) failures_total);
    }
    long deadline = now_ms() + (long) test_watchdog_secs(20) * 1000;
    int status = 0;
    for (;;) {
        pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid)
            break;
        if (done < 0 && errno != EINTR) {
            check(0, "%s: waitpid (%s)", name, strerror(errno));
            return;
        }
        if (now_ms() > deadline) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            check(0, "%s: still running after %us", name, test_watchdog_secs(20));
            return;
        }
        struct timespec tick = {.tv_sec = 0, .tv_nsec = 10000000L};
        nanosleep(&tick, NULL);
    }
    if (WIFEXITED(status))
        failures_total += WEXITSTATUS(status);
    else
        check(0, "%s: killed by signal %d", name, WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

int main(int argc, char **argv) {
    test_init(argc, argv);

    run(control_read_eintr, "control_read_eintr");
    run(control_read_restart, "control_read_restart");
    run(record_sigsuspend, "record_sigsuspend");
    run(record_sigtimedwait, "record_sigtimedwait");
    run(record_sigtimedwait_takes, "record_sigtimedwait_takes");
    run(record_pause, "record_pause");
    run(record_msgrcv, "record_msgrcv");
    run(record_msgsnd, "record_msgsnd");
    run(record_semop, "record_semop");
    run(record_wait4_reaps, "record_wait4_reaps");
    run(vfork_then_sigsuspend, "vfork_then_sigsuspend");
    run(vfork_then_sigtimedwait, "vfork_then_sigtimedwait");

    run(storm_signalfd_read, "storm_signalfd_read");

    run(sigtimedwait_control, "sigtimedwait_control");
    run(sigtimedwait_restart_handler, "sigtimedwait_restart_handler");
    run(sigtimedwait_plain_handler, "sigtimedwait_plain_handler");
    run(sigwaitinfo_restart_handler, "sigwaitinfo_restart_handler");

    run(stop_sigsuspend, "stop_sigsuspend");
    run(stop_pause, "stop_pause");
    run(stop_nanosleep, "stop_nanosleep");
    run(stop_clock_nanosleep, "stop_clock_nanosleep");
    run(stop_msgrcv, "stop_msgrcv");
    run(stop_msgsnd, "stop_msgsnd");
    run(stop_sigtimedwait, "stop_sigtimedwait");
    run(stop_semop, "stop_semop");

    return finish_suite("signal_restart_record");
}
