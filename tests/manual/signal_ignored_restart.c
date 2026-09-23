/*
 * signal_ignored_restart.c -- a signal whose delivery does nothing (SIGCHLD
 * left at SIG_DFL) never fails another thread's restartable syscall with EINTR.
 *
 * Two defects combined in kernel/signal.c.
 *
 * AOK dropped an ignored process-directed signal only when NO thread of the
 * process blocked it, and queued it otherwise. Linux (prepare_signal ->
 * sig_ignored) asks only the task the signal is sent to -- for a child's exit,
 * the thread that forked it. musl makes "some thread blocks it" routine: fork()
 * blocks every signal in the forking thread while it runs, pthread_exit()
 * blocks every signal while a thread exits, and glibc blocks them around
 * pthread_create's clone. So a child that died while any sibling was forking
 * or exiting had its SIGCHLD queued.
 *
 * And a queued ignored signal ended every sibling's wait, while the restart
 * decision counted only a job-control stop as "no handler runs", so the call
 * failed with EINTR. On Linux the woken thread returns -ERESTARTNOHAND,
 * -ERESTARTSYS or -ERESTART_RESTARTBLOCK, get_signal() dequeues the signal,
 * finds nothing to do, and the call is restarted. A signal is still genuinely
 * queued when the thread it is sent to blocks it, so fixing the first defect
 * alone left its siblings failing.
 *
 * Measured before the fix, with no checkpoint, on alpine-amd64-test and
 * devuan-amd64-test: one thread blocking SIGCHLD, another in clock_nanosleep
 * for 2 s, a child exiting 0.3 s in. clock_nanosleep failed with EINTR at
 * 0.3 s; Linux 6.12 completes it at 2.000 s.
 *
 * A restart must also keep a timed wait's deadline. A timed FUTEX_WAIT --
 * musl's pthread_cond_timedwait and sem_timedwait -- restarted with its whole
 * relative timeout, which EINTR had hidden: once the ignored signal restarted
 * it, a 2 s wait interrupted at 1 s took 3 s. Linux keeps the deadline
 * (futex_wait_restart).
 *
 * Each scenario runs in a process of its own. A child is forked 300ms in and
 * exits at 1000ms; at 2000ms the calls that wait for something get it -- a
 * pipe byte, a futex wake, another child's exit, a SIGUSR1 -- and the timed
 * ones reach their timeout. The elapsed time is asserted on every call, so a
 * timed call restarted with its whole timeout again ends near 3000ms and fails.
 * So is the CPU time the waiting thread used: a call that restarts without
 * the signal ever being taken ends its wait at once, every time, and spins
 * until its deadline -- on time, and invisible to the clock.
 *   - sibling: a thread that is NOT the child's parent blocks SIGCHLD. The
 *     signal is dropped, so nothing is interrupted at all: clock_nanosleep,
 *     nanosleep, ppoll, pselect6, a pipe read, a futex wait with no timeout,
 *     with a relative one and with an absolute one, wait4 for another child,
 *     sigsuspend, epoll_wait, rt_sigtimedwait and semtimedop each end at
 *     2000ms, on their timeout or on what they wait for.
 *   - parent: the forking thread blocks SIGCHLD itself, as it does inside
 *     fork() or pthread_exit(). The signal is queued and a sibling takes it;
 *     every restartable call above still ends at 2000ms. Afterwards, with no
 *     sibling left to take one, a second child's SIGCHLD is still pending:
 *     the signal really is queued in this shape, not dropped.
 *   - alone: the parent scenario once per restartable call, as the only thread
 *     that can take the signal -- which Linux, waking just one thread, cannot
 *     otherwise be relied on to interrupt. That the signal was taken proves
 *     the call was interrupted, and it still ends at 2000ms. These run side
 *     by side, each in its own process.
 *   - never restarted: the forking thread blocks SIGCHLD and the only thread
 *     that can take it waits in epoll_wait, rt_sigtimedwait or semtimedop.
 *     That call fails with EINTR at 1000ms, on Linux as here: signal(7) never
 *     restarts them. This is also the proof that the queued signal wakes a
 *     sibling, which the parent scenario cannot show by itself.
 *
 * On Linux one thread is woken for the queued signal; AOK wakes every thread
 * that can take it, so in the parent scenario all ten waiters are hit at once
 * and race each other to discard the signal. A wait parked in the host --
 * sleep, poll, select, pipe read -- then found nothing pending when it came to
 * decide, and failed with EINTR: in 48 of 80 rounds of twelve such waiters on
 * musl, 66 of 80 on glibc. That is why the scenario has so many of them.
 *
 * The timed calls and rt_sigtimedwait are issued as raw syscalls: musl's
 * sigtimedwait() retries EINTR in userspace, and a raw call is what shows what
 * the kernel did. Do not "simplify" them back to the libc wrappers.
 *
 * Checked against Linux 6.12 (Devuan 6, x86_64 and -m32, unprivileged).
 *
 * Exits 0 and prints "signal_ignored_restart: PASS" on success.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

/* The child whose SIGCHLD is under test is forked here... */
#define FORK_AT_MS 300
/* ...and exits here, well after its parent's fork() has returned: musl's fork
 * blocks every signal in the parent, and a child dying inside that window has
 * its SIGCHLD queued on Linux too, which would make the sibling scenario's
 * epoll_wait fail there as well. Halfway to ARRIVAL_MS, so that a timed call
 * restarted with its whole timeout ends a full second late. */
#define CHILD_EXIT_AT_MS 1000
/* By now whatever child1's SIGCHLD woke has dealt with it. */
#define SETTLED_MS 1500
/* Every call's timeout, and when everything a call waits for arrives. */
#define ARRIVAL_MS 2000
/* How early a call may end: a millisecond clock read twice. */
#define EARLY_MS 10
/* The most CPU a thread may use while it waits. A restart costs microseconds;
 * a spin from the interruption to the deadline costs a second. */
#define CPU_LIMIT_MS 250

/* musl on i386 names the 32-bit-time calls for what they take. */
#if !defined(SYS_clock_nanosleep) && defined(SYS_clock_nanosleep_time32)
#define SYS_clock_nanosleep SYS_clock_nanosleep_time32
#endif
#if !defined(SYS_nanosleep) && defined(SYS_nanosleep_time32)
#define SYS_nanosleep SYS_nanosleep_time32
#endif
#if !defined(SYS_ppoll) && defined(SYS_ppoll_time32)
#define SYS_ppoll SYS_ppoll_time32
#endif
#if !defined(SYS_pselect6) && defined(SYS_pselect6_time32)
#define SYS_pselect6 SYS_pselect6_time32
#endif
#if !defined(SYS_rt_sigtimedwait) && defined(SYS_rt_sigtimedwait_time32)
#define SYS_rt_sigtimedwait SYS_rt_sigtimedwait_time32
#endif
#if !defined(SYS_futex) && defined(SYS_futex_time32)
#define SYS_futex SYS_futex_time32
#endif

/* The layout these raw calls take on every ABI this test builds for: a long
 * pair, 64-bit on x86_64, arm64 and riscv64 and 32-bit on i386. */
struct kernel_timespec {
    long tv_sec;
    long tv_nsec;
};

enum call {
    CALL_CLOCK_NANOSLEEP,
    CALL_NANOSLEEP,
    CALL_PPOLL,
    CALL_PSELECT6,
    CALL_PIPE_READ,
    CALL_FUTEX_WAIT,
    CALL_FUTEX_RELATIVE,
    CALL_FUTEX_ABSOLUTE,
    CALL_WAIT4,
    CALL_SIGSUSPEND,
    CALL_EPOLL_WAIT,
    CALL_SIGTIMEDWAIT,
    CALL_SEMTIMEDOP,
};

static const char *const call_names[] = {
    [CALL_CLOCK_NANOSLEEP] = "clock_nanosleep",
    [CALL_NANOSLEEP] = "nanosleep",
    [CALL_PPOLL] = "ppoll",
    [CALL_PSELECT6] = "pselect6",
    [CALL_PIPE_READ] = "pipe read",
    [CALL_FUTEX_WAIT] = "futex wait",
    [CALL_FUTEX_RELATIVE] = "futex wait, relative timeout",
    [CALL_FUTEX_ABSOLUTE] = "futex wait, absolute timeout",
    [CALL_WAIT4] = "wait4",
    [CALL_SIGSUSPEND] = "sigsuspend",
    [CALL_EPOLL_WAIT] = "epoll_wait",
    [CALL_SIGTIMEDWAIT] = "rt_sigtimedwait",
    [CALL_SEMTIMEDOP] = "semtimedop",
};

/* Everything a restartable call can be; signal(7) restarts each of these after
 * a handler with SA_RESTART, or after a signal that runs no handler at all. */
static const enum call restartable[] = {
    CALL_CLOCK_NANOSLEEP, CALL_NANOSLEEP, CALL_PPOLL, CALL_PSELECT6,
    CALL_PIPE_READ, CALL_FUTEX_WAIT, CALL_FUTEX_RELATIVE, CALL_FUTEX_ABSOLUTE,
    CALL_WAIT4, CALL_SIGSUSPEND,
};

/* The calls signal(7) never restarts. */
static const enum call never_restarted[] = {
    CALL_EPOLL_WAIT, CALL_SIGTIMEDWAIT, CALL_SEMTIMEDOP,
};

#define MAX_WAITERS 16

struct waiter {
    enum call call;
    pthread_t thread;
    long rc;
    int err;
    long end;       /* ms after the start */
    long cpu;       /* ms of CPU the thread used in the call */
};

static struct {
    long t0;
    pthread_barrier_t gate;
    int pipe_fd[2];          /* CALL_PIPE_READ */
    int child2_pipe[2];      /* tells the wait4 waiter's child to exit */
    int blocker_pipe[2];     /* releases the sibling scenario's blocker */
    unsigned futex_word;     /* CALL_FUTEX_WAIT */
    unsigned timed_word;     /* the timed futex waits; nothing wakes them */
    pid_t child2;            /* CALL_WAIT4, forked by that waiter */
    int epfd;                /* CALL_EPOLL_WAIT */
    int semid;               /* CALL_SEMTIMEDOP */
    pthread_t suspender;     /* CALL_SIGSUSPEND's thread, for SIGUSR1 */
    bool have_suspender;
} st;

static volatile sig_atomic_t usr1_hits;

static void on_usr1(int sig) {
    (void) sig;
    usr1_hits++;
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

static long clock_ms(clockid_t clock) {
    struct timespec ts;
    clock_gettime(clock, &ts);
    return (long) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static long now_ms(void) {
    return clock_ms(CLOCK_MONOTONIC);
}

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
 * heavily loaded run. Only upper bounds use it: load cannot end a call early. */
static long slack_ms(long base) {
    return base * (long) test_watchdog_secs(1);
}

static bool near(long elapsed, long at) {
    return elapsed >= at - EARLY_MS && elapsed < at + slack_ms(500);
}

static const char *errname(long rc, int err) {
    return rc < 0 ? strerror(err) : "-";
}

static struct kernel_timespec arrival_timespec(void) {
    return (struct kernel_timespec) {ARRIVAL_MS / 1000, (ARRIVAL_MS % 1000) * 1000000L};
}

/* ---- the calls ------------------------------------------------------------ */

static long issue(enum call call) {
    switch (call) {
    case CALL_CLOCK_NANOSLEEP: {
        struct kernel_timespec ts = arrival_timespec();
        return syscall(SYS_clock_nanosleep, CLOCK_MONOTONIC, 0, &ts, NULL);
    }
    case CALL_NANOSLEEP: {
        struct kernel_timespec ts = arrival_timespec();
        return syscall(SYS_nanosleep, &ts, NULL);
    }
    case CALL_PPOLL: {
        /* Linux writes the time left back here when it restarts the call. */
        struct kernel_timespec ts = arrival_timespec();
        return syscall(SYS_ppoll, NULL, 0, &ts, NULL, 8);
    }
    case CALL_PSELECT6: {
        struct kernel_timespec ts = arrival_timespec();
        return syscall(SYS_pselect6, 0, NULL, NULL, NULL, &ts, NULL);
    }
    case CALL_PIPE_READ: {
        char c;
        return read(st.pipe_fd[0], &c, 1);
    }
    case CALL_FUTEX_WAIT:
        /* The way a lock or condition variable waits: until the word changes.
         * EAGAIN is the word having changed already, which ends the loop. */
        while (__atomic_load_n(&st.futex_word, __ATOMIC_ACQUIRE) == 0) {
            long rc = syscall(SYS_futex, &st.futex_word, FUTEX_WAIT_PRIVATE, 0, NULL, NULL, 0);
            if (rc < 0 && errno != EAGAIN)
                return rc;
        }
        return 0;
    case CALL_FUTEX_RELATIVE: {
        /* How musl's pthread_cond_timedwait and sem_timedwait wait. */
        struct kernel_timespec ts = arrival_timespec();
        return syscall(SYS_futex, &st.timed_word, FUTEX_WAIT_PRIVATE, 0, &ts, NULL, 0);
    }
    case CALL_FUTEX_ABSOLUTE: {
        /* How glibc's do: a deadline on CLOCK_MONOTONIC. */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long nsec = now.tv_nsec + (ARRIVAL_MS % 1000) * 1000000L;
        struct kernel_timespec at = {now.tv_sec + ARRIVAL_MS / 1000 + nsec / 1000000000L,
                                     nsec % 1000000000L};
        return syscall(SYS_futex, &st.timed_word, FUTEX_WAIT_BITSET_PRIVATE, 0, &at, NULL,
                       FUTEX_BITSET_MATCH_ANY);
    }
    case CALL_WAIT4:
        return waitpid(st.child2, NULL, 0);
    case CALL_SIGSUSPEND: {
        sigset_t none;
        sigemptyset(&none);
        return sigsuspend(&none);
    }
    case CALL_EPOLL_WAIT: {
        struct epoll_event ev;
        return epoll_wait(st.epfd, &ev, 1, ARRIVAL_MS);
    }
    case CALL_SIGTIMEDWAIT: {
        uint64_t set = 1ull << (SIGUSR2 - 1);
        struct kernel_timespec ts = arrival_timespec();
        return syscall(SYS_rt_sigtimedwait, &set, NULL, &ts, sizeof(set));
    }
    case CALL_SEMTIMEDOP: {
        struct sembuf op = {.sem_num = 0, .sem_op = -1, .sem_flg = 0};
        struct timespec ts = {ARRIVAL_MS / 1000, (ARRIVAL_MS % 1000) * 1000000L};
        return semtimedop(st.semid, &op, 1, &ts);
    }
    }
    return -1;
}

static void *waiter_main(void *arg) {
    struct waiter *w = arg;
    /* Set outright: a thread inherits its creator's mask, and the parent
     * scenario's creator blocks SIGCHLD. No waiter ever blocks it. Only the
     * signals a call waits for are blocked, as a program using them would. */
    sigset_t mask;
    sigemptyset(&mask);
    if (w->call == CALL_SIGSUSPEND)
        sigaddset(&mask, SIGUSR1);
    if (w->call == CALL_SIGTIMEDWAIT)
        sigaddset(&mask, SIGUSR2);
    pthread_sigmask(SIG_SETMASK, &mask, NULL);

    if (w->call == CALL_WAIT4) {
        /* This thread's own child, so its exit is this thread's SIGCHLD --
         * and this thread never blocks it. */
        st.child2 = fork();
        if (st.child2 == 0) {
            char c;
            while (read(st.child2_pipe[0], &c, 1) < 0 && errno == EINTR)
                continue;
            _exit(0);
        }
    }

    /* Every thread is created before any call starts, so no thread's stack
     * mapping lands while a call is entering its wait. */
    pthread_barrier_wait(&st.gate);
    long cpu0 = clock_ms(CLOCK_THREAD_CPUTIME_ID);
    errno = 0;
    w->rc = issue(w->call);
    w->err = errno;
    w->end = now_ms() - st.t0;
    w->cpu = clock_ms(CLOCK_THREAD_CPUTIME_ID) - cpu0;
    return NULL;
}

static void *blocker_main(void *arg) {
    (void) arg;
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    pthread_sigmask(SIG_BLOCK, &mask, NULL);
    pthread_barrier_wait(&st.gate);
    char c;
    while (read(st.blocker_pipe[0], &c, 1) < 0 && errno == EINTR)
        continue;
    return NULL;
}

static void block_sigchld(void) {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    pthread_sigmask(SIG_BLOCK, &mask, NULL);
}

static bool sigchld_pending(void) {
    sigset_t set;
    sigemptyset(&set);
    sigpending(&set);
    return sigismember(&set, SIGCHLD);
}

/* What a call that nothing interrupted returns. */
static bool completed(const struct waiter *w) {
    switch (w->call) {
    case CALL_PIPE_READ:
        return w->rc == 1;
    case CALL_WAIT4:
        return w->rc == st.child2;
    case CALL_SIGSUSPEND:
        return w->rc == -1 && w->err == EINTR && usr1_hits == 1;
    case CALL_SIGTIMEDWAIT:
    case CALL_SEMTIMEDOP:
        return w->rc == -1 && w->err == EAGAIN;
    case CALL_FUTEX_RELATIVE:
    case CALL_FUTEX_ABSOLUTE:
        return w->rc == -1 && w->err == ETIMEDOUT;
    default:
        return w->rc == 0;
    }
}

enum shape {
    SIBLING_BLOCKS,     /* a thread that is not the parent blocks SIGCHLD */
    PARENT_BLOCKS,      /* the forking thread blocks SIGCHLD */
};

/* One scenario, in the calling process. Returns its failure count. */
static unsigned run_scenario(const char *label, enum shape shape,
        const enum call *calls, size_t ncalls, bool expect_eintr) {
    failures_total = 0;     /* forked from the harness, which has its own */
    alarm(test_watchdog_secs(30));
    memset(&st, 0, sizeof(st));
    st.epfd = -1;
    st.semid = -1;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_usr1;          /* no SA_RESTART: sigsuspend ends */
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);
    signal(SIGCHLD, SIG_DFL);         /* the default: ignored */
    usr1_hits = 0;

    if (pipe(st.pipe_fd) != 0 || pipe(st.child2_pipe) != 0 || pipe(st.blocker_pipe) != 0) {
        check(0, "%s: pipe: %s", label, strerror(errno));
        return failures_total;
    }
    for (size_t i = 0; i < ncalls; i++) {
        if (calls[i] == CALL_EPOLL_WAIT && (st.epfd = epoll_create1(0)) < 0)
            check(0, "%s: epoll_create1: %s", label, strerror(errno));
        if (calls[i] == CALL_SEMTIMEDOP &&
                (st.semid = semget(IPC_PRIVATE, 1, IPC_CREAT | 0600)) < 0)
            check(0, "%s: semget: %s", label, strerror(errno));
    }
    if (failures_total != 0)
        return failures_total;

    struct waiter waiters[MAX_WAITERS];
    size_t nthreads = ncalls + (shape == SIBLING_BLOCKS ? 1 : 0);
    pthread_barrier_init(&st.gate, NULL, (unsigned) nthreads + 1);
    for (size_t i = 0; i < ncalls; i++) {
        waiters[i] = (struct waiter) {.call = calls[i]};
        pthread_create(&waiters[i].thread, NULL, waiter_main, &waiters[i]);
        if (calls[i] == CALL_SIGSUSPEND) {
            st.suspender = waiters[i].thread;
            st.have_suspender = true;
        }
    }
    pthread_t blocker;
    if (shape == SIBLING_BLOCKS)
        pthread_create(&blocker, NULL, blocker_main, NULL);
    else
        block_sigchld();    /* after the waiters, which set their own masks */

    st.t0 = now_ms();
    pthread_barrier_wait(&st.gate);

    sleep_until(st.t0 + FORK_AT_MS);
    pid_t child1 = fork();
    if (child1 == 0) {
        sleep_until(st.t0 + CHILD_EXIT_AT_MS);
        _exit(0);
    }
    while (waitpid(child1, NULL, 0) < 0 && errno == EINTR)
        continue;
    if (shape == PARENT_BLOCKS && !expect_eintr) {
        /* A sibling took child1's SIGCHLD, rather than leaving it queued.
         * Asked before child2 exits, so that its SIGCHLD cannot be the one
         * seen: AOK sends a child's SIGCHLD once pids_lock is dropped, which
         * can be after its parent has reaped it and begun exiting. */
        sleep_until(st.t0 + SETTLED_MS);
        check(!sigchld_pending(), "%s: child1's SIGCHLD was taken by a sibling", label);
    }

    sleep_until(st.t0 + ARRIVAL_MS);
    if (write(st.pipe_fd[1], "x", 1) != 1)
        check(0, "%s: pipe write: %s", label, strerror(errno));
    __atomic_store_n(&st.futex_word, 1, __ATOMIC_RELEASE);
    syscall(SYS_futex, &st.futex_word, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
    if (write(st.child2_pipe[1], "x", 1) != 1)
        check(0, "%s: child pipe write: %s", label, strerror(errno));
    if (st.have_suspender)
        pthread_kill(st.suspender, SIGUSR1);

    long expect_at = expect_eintr ? CHILD_EXIT_AT_MS : ARRIVAL_MS;
    for (size_t i = 0; i < ncalls; i++) {
        struct waiter *w = &waiters[i];
        pthread_join(w->thread, NULL);
        const char *name = call_names[w->call];
        if (expect_eintr) {
            check(w->rc == -1 && w->err == EINTR && near(w->end, expect_at),
                  "%s: %s = %ld (%s) at %ldms, want EINTR at ~%ldms",
                  label, name, w->rc, errname(w->rc, w->err), w->end, expect_at);
        } else {
            check(completed(w) && near(w->end, expect_at),
                  "%s: %s = %ld (%s) at %ldms, want it to run to ~%ldms",
                  label, name, w->rc, errname(w->rc, w->err), w->end, expect_at);
        }
        check(w->cpu < CPU_LIMIT_MS, "%s: %s used %ldms of CPU, want it asleep",
              label, name, w->cpu);
    }
    if (shape == SIBLING_BLOCKS) {
        if (write(st.blocker_pipe[1], "x", 1) != 1)
            check(0, "%s: blocker pipe write: %s", label, strerror(errno));
        pthread_join(blocker, NULL);
    }

    if (shape == PARENT_BLOCKS && !expect_eintr) {
        /* The control: with no sibling left to take it, a SIGCHLD sent to a
         * thread that blocks it stays queued -- this shape queues the signal,
         * it does not drop it. Anything already pending is taken first, so
         * the check sees child3's alone. */
        uint64_t set = 1ull << (SIGCHLD - 1);
        struct kernel_timespec zero = {0, 0};
        while (syscall(SYS_rt_sigtimedwait, &set, NULL, &zero, sizeof(set)) == SIGCHLD)
            continue;
        check(!sigchld_pending(), "%s: control: no SIGCHLD pending before child3", label);
        pid_t child3 = fork();
        if (child3 == 0)
            _exit(0);
        while (waitpid(child3, NULL, 0) < 0 && errno == EINTR)
            continue;
        long deadline = now_ms() + slack_ms(1000);
        while (!sigchld_pending() && now_ms() < deadline)
            usleep(10000);
        check(sigchld_pending(), "%s: control: SIGCHLD queued while its target blocks it", label);
    }

    if (st.child2 > 0) {
        kill(st.child2, SIGKILL);
        waitpid(st.child2, NULL, WNOHANG);
    }
    if (st.semid >= 0)
        semctl(st.semid, 0, IPC_RMID);
    alarm(0);
    return failures_total;
}

/* Start a scenario in a process of its own, so every one starts with fresh
 * signal state and a hang or crash in one cannot take the others with it. */
static pid_t scenario_start(const char *label, enum shape shape,
        const enum call *calls, size_t ncalls, bool expect_eintr) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        check(0, "%s: fork: %s", label, strerror(errno));
        return -1;
    }
    if (pid == 0) {
        unsigned failures = run_scenario(label, shape, calls, ncalls, expect_eintr);
        fflush(stdout);
        _exit(failures > 100 ? 100 : (int) failures);
    }
    return pid;
}

static void scenario_finish(const char *label, pid_t pid) {
    if (pid < 0)
        return;
    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        continue;
    if (WIFSIGNALED(status)) {
        check(0, "%s: killed by signal %d", label, WTERMSIG(status));
    } else if (WEXITSTATUS(status) != 0) {
        failures_total += (unsigned) WEXITSTATUS(status);
    } else {
        test_logf("ok %s\n", label);
    }
}

static void scenario(const char *label, enum shape shape,
        const enum call *calls, size_t ncalls, bool expect_eintr) {
    scenario_finish(label, scenario_start(label, shape, calls, ncalls, expect_eintr));
}

int main(int argc, char **argv) {
    test_init(argc, argv);

    enum call sibling_calls[sizeof(restartable) / sizeof(restartable[0]) +
                            sizeof(never_restarted) / sizeof(never_restarted[0])];
    size_t n = 0;
    for (size_t i = 0; i < sizeof(restartable) / sizeof(restartable[0]); i++)
        sibling_calls[n++] = restartable[i];
    for (size_t i = 0; i < sizeof(never_restarted) / sizeof(never_restarted[0]); i++)
        sibling_calls[n++] = never_restarted[i];

    scenario("sibling", SIBLING_BLOCKS, sibling_calls, n, false);
    scenario("parent", PARENT_BLOCKS, restartable,
             sizeof(restartable) / sizeof(restartable[0]), false);

    enum { NRESTARTABLE = sizeof(restartable) / sizeof(restartable[0]) };
    char alone_labels[NRESTARTABLE][64];
    pid_t alone[NRESTARTABLE];
    for (size_t i = 0; i < NRESTARTABLE; i++) {
        snprintf(alone_labels[i], sizeof(alone_labels[i]), "alone (%s)",
                 call_names[restartable[i]]);
        alone[i] = scenario_start(alone_labels[i], PARENT_BLOCKS, &restartable[i], 1, false);
    }
    for (size_t i = 0; i < NRESTARTABLE; i++)
        scenario_finish(alone_labels[i], alone[i]);
    for (size_t i = 0; i < sizeof(never_restarted) / sizeof(never_restarted[0]); i++) {
        char label[64];
        snprintf(label, sizeof(label), "never restarted (%s)", call_names[never_restarted[i]]);
        scenario(label, PARENT_BLOCKS, &never_restarted[i], 1, true);
    }

    return finish_suite("signal_ignored_restart");
}
