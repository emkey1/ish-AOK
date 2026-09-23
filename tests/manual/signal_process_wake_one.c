/*
 * signal_process_wake_one.c -- a signal sent to a PROCESS interrupts the one
 * thread that takes it, and no other.
 *
 * Linux queues a process-directed signal once, for the whole process, and
 * complete_signal() tells ONE thread about it: the thread it was sent to if
 * that thread can take it, otherwise another that can. Only that thread is
 * woken; every other thread's syscalls carry on. A child's SIGCHLD is sent to
 * the thread that forked it, an interval timer's SIGALRM to the leader.
 *
 * AOK woke every thread that did not block the signal, and each of them
 * counted it as its own, so they raced for it. Measured before the fix with a
 * SIGCHLD handler without SA_RESTART, the forking thread in waitpid and twelve
 * siblings parked in clock_nanosleep, nanosleep, ppoll, pselect6 and pipe
 * reads: the handler ran in a sibling, never in the forking thread, and
 * siblings' calls failed with EINTR, in 20 rounds of 20 on alpine-amd64-test
 * (musl) and 30 of 40 on devuan-amd64-test (glibc). Linux 6.12: 0 of 40.
 *
 * Each scenario runs in a process of its own. A child is forked at 300ms and
 * exits at 1000ms; at 2000ms everything the calls wait for arrives. Every
 * call's elapsed time AND the CPU time its thread used are asserted: a call
 * restarted over and over without the signal being taken spins until its
 * deadline and returns on time, which only the CPU shows.
 *
 *   - target: the forking thread takes the child's SIGCHLD. Its handler runs
 *     there, once, and waitpid returns the child. No sibling -- thirteen, in
 *     every kind of wait -- is interrupted.
 *   - target blocks it: the forking thread blocks SIGCHLD, so a sibling takes
 *     it. Exactly one sibling is interrupted, with EINTR, and it is the one
 *     the handler ran in; the other twelve are not touched.
 *   - sigtimedwait: every thread blocks SIGCHLD and one waits for it in
 *     rt_sigtimedwait. That thread gets it; nothing else is touched.
 *   - unblocked later (sigprocmask, sigsuspend): every thread blocks SIGCHLD,
 *     so nobody can take it when it comes. It stays queued, and the first
 *     thread to unblock it takes it -- before that sigprocmask returns, or at
 *     once in that sigsuspend.
 *   - handed on: the thread the child's SIGCHLD is sent to keeps blocking and
 *     unblocking it, and stays blocked once it is queued. A thread told to
 *     take a signal that then blocks it must hand it to one that can -- here a
 *     sibling in sigsuspend -- or it is stranded: nobody else looks at it.
 *     Several rounds, since whether the signal lands in the window is chance.
 *   - lost the race: the forking thread waits in a pipe read, or sigsuspend,
 *     while a sibling keeps calling sigprocmask, which makes that sibling
 *     look at the queued SIGCHLD and often take it first. The call fails with
 *     EINTR only if the handler ran in the forking thread; otherwise it goes
 *     on to 2000ms, as on Linux, where a thread whose signal another thread
 *     took finds nothing to run and restarts the call. Several rounds.
 *   - itimer: SIGALRM from setitimer goes to the leader, which waits in a pipe
 *     read and gets EINTR; no sibling is touched.
 *   - fatal: SIGALRM left at SIG_DFL kills the whole process at 1000ms.
 *
 * The timed calls and rt_sigtimedwait are raw syscalls: musl's sigtimedwait()
 * retries EINTR in userspace, and a raw call is what shows what the kernel did.
 *
 * Checked against Linux 6.12 (Devuan 6, x86_64 and -m32, unprivileged).
 *
 * Exits 0 and prints "signal_process_wake_one: PASS" on success.
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
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

/* The child whose SIGCHLD is under test is forked here, and exits well after
 * its parent's fork() has returned: musl's fork blocks every signal in the
 * parent while it runs. */
#define FORK_AT_MS 300
#define CHILD_EXIT_AT_MS 1000
/* When the "unblocked later" scenarios let SIGCHLD through. */
#define UNBLOCK_AT_MS 1300
/* By now the signal has been taken, if anything was going to take it. */
#define SETTLED_MS 1500
/* Every call's timeout, and when everything a call waits for arrives. */
#define ARRIVAL_MS 2000
/* How early a call may end: a millisecond clock read twice. */
#define EARLY_MS 10
/* The most CPU a waiting thread may use. A restart costs microseconds; a spin
 * from the interruption to the deadline costs a second. */
#define CPU_LIMIT_MS 250
/* Rounds of the scenarios that depend on where the signal lands. */
#define RACE_ROUNDS 4

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
    CALL_SIGCHLD_WAIT,  /* rt_sigtimedwait for SIGCHLD itself */
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
    [CALL_SIGTIMEDWAIT] = "rt_sigtimedwait(SIGUSR2)",
    [CALL_SEMTIMEDOP] = "semtimedop",
    [CALL_SIGCHLD_WAIT] = "rt_sigtimedwait(SIGCHLD)",
};

/* A sibling in each kind of wait. Each call can wait at most once per
 * scenario: they share one pipe, one futex word, one child. */
static const enum call every_wait[] = {
    CALL_CLOCK_NANOSLEEP, CALL_NANOSLEEP, CALL_PPOLL, CALL_PSELECT6,
    CALL_PIPE_READ, CALL_FUTEX_WAIT, CALL_FUTEX_RELATIVE, CALL_FUTEX_ABSOLUTE,
    CALL_WAIT4, CALL_SIGSUSPEND, CALL_EPOLL_WAIT, CALL_SIGTIMEDWAIT,
    CALL_SEMTIMEDOP,
};
#define N_EVERY_WAIT (sizeof(every_wait) / sizeof(every_wait[0]))

/* A few, for the scenarios whose subject is another thread. */
static const enum call some_waits[] = {
    CALL_NANOSLEEP, CALL_PPOLL, CALL_PIPE_READ, CALL_FUTEX_WAIT,
};
#define N_SOME_WAITS (sizeof(some_waits) / sizeof(some_waits[0]))

#define MAX_WAITERS 16

struct waiter {
    enum call call;
    bool blocks_sigchld;
    pthread_t thread;
    pid_t tid;
    long rc;
    int err;
    long end;       /* ms after the start */
    long cpu;       /* ms of CPU the thread used in the call */
};

static struct {
    long t0;
    pthread_barrier_t gate;
    int pipe_fd[2];          /* CALL_PIPE_READ */
    int main_pipe[2];        /* the main thread's own read, when it has one */
    int child2_pipe[2];      /* tells the wait4 waiter's child to exit */
    unsigned futex_word;     /* CALL_FUTEX_WAIT */
    unsigned timed_word;     /* the timed futex waits; nothing wakes them */
    pid_t child2;            /* CALL_WAIT4, forked by that waiter */
    int epfd;                /* CALL_EPOLL_WAIT */
    int semid;               /* CALL_SEMTIMEDOP */
    pthread_t suspender;     /* CALL_SIGSUSPEND's thread, for SIGUSR1 */
    bool have_suspender;
    pthread_t extra_suspender;   /* the "handed on" scenario's sibling */
    bool have_extra_suspender;
    pid_t main_tid;
    pthread_t main_thread;   /* when it waits in sigsuspend, for SIGUSR1 */
    bool main_suspends;
    volatile int stop_spinning;
} st;

/* Where and when each handler ran. */
#define MAX_RUNS 16
static struct handler_run {
    int sig;
    pid_t tid;
    long at;
} runs[MAX_RUNS];
static int nruns;
static volatile sig_atomic_t usr1_hits;

static long clock_ms(clockid_t clock) {
    struct timespec ts;
    clock_gettime(clock, &ts);
    return (long) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static long now_ms(void) {
    return clock_ms(CLOCK_MONOTONIC);
}

static pid_t gettid_(void) {
    return (pid_t) syscall(SYS_gettid);
}

static void on_signal(int sig) {
    int saved = errno;
    int i = __atomic_fetch_add(&nruns, 1, __ATOMIC_RELAXED);
    if (i < MAX_RUNS) {
        runs[i].sig = sig;
        runs[i].tid = gettid_();
        runs[i].at = now_ms() - st.t0;
    }
    errno = saved;
}

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

static void sleep_until(long deadline) {
    for (;;) {
        long left = deadline - now_ms();
        if (left <= 0)
            return;
        struct timespec ts = {.tv_sec = left / 1000, .tv_nsec = (left % 1000) * 1000000L};
        nanosleep(&ts, NULL);
    }
}

/* How late something may happen and still count, scaled like the watchdogs
 * for a heavily loaded run. Only upper bounds use it: load cannot make
 * anything early. */
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

static void install(int sig, void (*handler)(int)) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;          /* no SA_RESTART: interruptions show */
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
}

static void mask_sigchld(int how) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGCHLD);
    pthread_sigmask(how, &set, NULL);
}

static bool sigchld_pending(void) {
    sigset_t set;
    sigemptyset(&set);
    sigpending(&set);
    return sigismember(&set, SIGCHLD);
}

/* How many handler runs for `sig` began before `until`; the first's thread
 * and time. */
static int runs_before(int sig, long until, pid_t *tid, long *at) {
    int n = 0;
    int total = __atomic_load_n(&nruns, __ATOMIC_ACQUIRE);
    if (total > MAX_RUNS)
        total = MAX_RUNS;
    for (int i = 0; i < total; i++) {
        if (runs[i].sig != sig || runs[i].at >= until)
            continue;
        if (n++ == 0) {
            if (tid != NULL)
                *tid = runs[i].tid;
            if (at != NULL)
                *at = runs[i].at;
        }
    }
    return n;
}

/* ---- the calls ------------------------------------------------------------ */

static long issue(const struct waiter *w) {
    switch (w->call) {
    case CALL_CLOCK_NANOSLEEP: {
        struct kernel_timespec ts = arrival_timespec();
        return syscall(SYS_clock_nanosleep, CLOCK_MONOTONIC, 0, &ts, NULL);
    }
    case CALL_NANOSLEEP: {
        struct kernel_timespec ts = arrival_timespec();
        return syscall(SYS_nanosleep, &ts, NULL);
    }
    case CALL_PPOLL: {
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
        struct kernel_timespec ts = arrival_timespec();
        return syscall(SYS_futex, &st.timed_word, FUTEX_WAIT_PRIVATE, 0, &ts, NULL, 0);
    }
    case CALL_FUTEX_ABSOLUTE: {
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
        sigset_t mask;
        sigemptyset(&mask);
        if (w->blocks_sigchld)
            sigaddset(&mask, SIGCHLD);
        return sigsuspend(&mask);
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
    case CALL_SIGCHLD_WAIT: {
        uint64_t set = 1ull << (SIGCHLD - 1);
        struct kernel_timespec ts = arrival_timespec();
        return syscall(SYS_rt_sigtimedwait, &set, NULL, &ts, sizeof(set));
    }
    }
    return -1;
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
    case CALL_SIGCHLD_WAIT:
        return w->rc == -1 && w->err == EAGAIN;
    case CALL_FUTEX_RELATIVE:
    case CALL_FUTEX_ABSOLUTE:
        return w->rc == -1 && w->err == ETIMEDOUT;
    default:
        return w->rc == 0;
    }
}

static void *waiter_main(void *arg) {
    struct waiter *w = arg;
    w->tid = gettid_();
    /* Set outright: a thread inherits its creator's mask. Only the signals a
     * call waits for are blocked, and SIGCHLD where the scenario says. */
    sigset_t mask;
    sigemptyset(&mask);
    if (w->blocks_sigchld || w->call == CALL_SIGCHLD_WAIT)
        sigaddset(&mask, SIGCHLD);
    if (w->call == CALL_SIGSUSPEND)
        sigaddset(&mask, SIGUSR1);
    if (w->call == CALL_SIGTIMEDWAIT)
        sigaddset(&mask, SIGUSR2);
    pthread_sigmask(SIG_SETMASK, &mask, NULL);

    if (w->call == CALL_WAIT4) {
        /* This thread's own child: its exit is this thread's SIGCHLD, and it
         * comes at ARRIVAL_MS. */
        st.child2 = fork();
        if (st.child2 == 0) {
            char c;
            while (read(st.child2_pipe[0], &c, 1) < 0 && errno == EINTR)
                continue;
            _exit(0);
        }
    }

    pthread_barrier_wait(&st.gate);
    long cpu0 = clock_ms(CLOCK_THREAD_CPUTIME_ID);
    errno = 0;
    w->rc = issue(w);
    w->err = errno;
    w->end = now_ms() - st.t0;
    w->cpu = clock_ms(CLOCK_THREAD_CPUTIME_ID) - cpu0;
    return NULL;
}

/* At ARRIVAL_MS, everything the calls wait for. Blocks every signal, so no
 * process signal is ever this thread's to take. */
static void *releaser_main(void *arg) {
    (void) arg;
    sigset_t all;
    sigfillset(&all);
    pthread_sigmask(SIG_SETMASK, &all, NULL);
    pthread_barrier_wait(&st.gate);
    sleep_until(st.t0 + ARRIVAL_MS);
    if (write(st.pipe_fd[1], "x", 1) != 1 || write(st.main_pipe[1], "x", 1) != 1 ||
            write(st.child2_pipe[1], "x", 1) != 1)
        check(0, "release: write: %s", strerror(errno));
    __atomic_store_n(&st.futex_word, 1, __ATOMIC_RELEASE);
    syscall(SYS_futex, &st.futex_word, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
    if (st.have_suspender)
        pthread_kill(st.suspender, SIGUSR1);
    if (st.have_extra_suspender)
        pthread_kill(st.extra_suspender, SIGUSR1);
    if (st.main_suspends)
        pthread_kill(st.main_thread, SIGUSR1);
    return NULL;
}

/* ---- one scenario's world ------------------------------------------------- */

struct world {
    struct waiter w[MAX_WAITERS];
    size_t n;
    pthread_t releaser;
    unsigned extra;          /* threads besides the waiters at the gate */
    pid_t child1;
};

static void world_add(struct world *wd, enum call call, bool blocks_sigchld) {
    wd->w[wd->n++] = (struct waiter) {.call = call, .blocks_sigchld = blocks_sigchld};
}

/* Everything but starting the threads. `extra` is how many threads other than
 * the waiters, the releaser and the caller will wait at the gate. */
static bool world_init(const char *label, struct world *wd, unsigned extra) {
    failures_total = 0;     /* forked from the harness, which has its own */
    alarm(test_watchdog_secs(30));
    memset(&st, 0, sizeof(st));
    st.epfd = -1;
    st.semid = -1;
    st.main_tid = gettid_();
    nruns = 0;
    usr1_hits = 0;
    install(SIGUSR1, on_usr1);
    install(SIGCHLD, on_signal);
    if (pipe(st.pipe_fd) != 0 || pipe(st.main_pipe) != 0 || pipe(st.child2_pipe) != 0) {
        check(0, "%s: pipe: %s", label, strerror(errno));
        return false;
    }
    for (size_t i = 0; i < wd->n; i++) {
        if (wd->w[i].call == CALL_EPOLL_WAIT && (st.epfd = epoll_create1(0)) < 0)
            check(0, "%s: epoll_create1: %s", label, strerror(errno));
        if (wd->w[i].call == CALL_SEMTIMEDOP &&
                (st.semid = semget(IPC_PRIVATE, 1, IPC_CREAT | 0600)) < 0)
            check(0, "%s: semget: %s", label, strerror(errno));
    }
    wd->extra = extra;
    return failures_total == 0;
}

static void world_start(struct world *wd) {
    pthread_barrier_init(&st.gate, NULL, (unsigned) wd->n + wd->extra + 2);
    for (size_t i = 0; i < wd->n; i++) {
        pthread_create(&wd->w[i].thread, NULL, waiter_main, &wd->w[i]);
        if (wd->w[i].call == CALL_SIGSUSPEND) {
            st.suspender = wd->w[i].thread;
            st.have_suspender = true;
        }
    }
    pthread_create(&wd->releaser, NULL, releaser_main, NULL);
}

/* The caller's side of the gate: every call starts together, at t0. */
static void world_go(void) {
    st.t0 = now_ms();
    pthread_barrier_wait(&st.gate);
}

static void fork_child1(struct world *wd) {
    sleep_until(st.t0 + FORK_AT_MS);
    wd->child1 = fork();
    if (wd->child1 == 0) {
        sleep_until(st.t0 + CHILD_EXIT_AT_MS);
        _exit(0);
    }
}

/* The releaser first: it pthread_kill()s threads that may have finished long
 * before, and a thread that has been joined is gone -- musl unmaps its stack,
 * and signalling it faults. */
static void world_join(struct world *wd) {
    pthread_join(wd->releaser, NULL);
    for (size_t i = 0; i < wd->n; i++)
        pthread_join(wd->w[i].thread, NULL);
}

static void world_end(struct world *wd) {
    if (st.child2 > 0) {
        kill(st.child2, SIGKILL);
        waitpid(st.child2, NULL, 0);
    }
    if (wd->child1 > 0)
        waitpid(wd->child1, NULL, WNOHANG);
    if (st.semid >= 0)
        semctl(st.semid, 0, IPC_RMID);
    alarm(0);
}

static struct waiter *waiter_of(struct world *wd, pid_t tid) {
    for (size_t i = 0; i < wd->n; i++)
        if (wd->w[i].tid == tid)
            return &wd->w[i];
    return NULL;
}

/* A waiter nothing interrupted: its call ran to ARRIVAL_MS and returned what
 * it waits for, asleep the whole time. */
static void check_untouched(const char *label, const struct waiter *w) {
    check(completed(w) && near(w->end, ARRIVAL_MS),
          "%s: %s = %ld (%s) at %ldms, want it to run to ~%dms",
          label, call_names[w->call], w->rc, errname(w->rc, w->err), w->end, ARRIVAL_MS);
    check(w->cpu < CPU_LIMIT_MS, "%s: %s used %ldms of CPU, want it asleep",
          label, call_names[w->call], w->cpu);
}

/* A waiter a handler without SA_RESTART interrupted at `at`. */
static void check_interrupted(const char *label, const struct waiter *w, long at) {
    check(w->rc == -1 && w->err == EINTR && near(w->end, at),
          "%s: %s = %ld (%s) at %ldms, want EINTR at ~%ldms",
          label, call_names[w->call], w->rc, errname(w->rc, w->err), w->end, at);
    check(w->cpu < CPU_LIMIT_MS, "%s: %s used %ldms of CPU, want it asleep",
          label, call_names[w->call], w->cpu);
}

/* The main thread's own wait for child1, which must return the child: on
 * Linux the child is a zombie before its SIGCHLD wakes anybody, and wait4
 * reaps it rather than fail. */
static void wait_child1(const char *label, struct world *wd, bool expect_first_try) {
    long rc = waitpid(wd->child1, NULL, 0);
    int err = errno;
    long end = now_ms() - st.t0;
    if (expect_first_try)
        check(rc == wd->child1 && near(end, CHILD_EXIT_AT_MS),
              "%s: main's waitpid = %ld (%s) at %ldms, want the child at ~%dms",
              label, rc, errname(rc, err), end, CHILD_EXIT_AT_MS);
    while (rc < 0 && err == EINTR) {
        rc = waitpid(wd->child1, NULL, 0);
        err = errno;
    }
    if (rc == wd->child1)
        wd->child1 = 0;
}

/* ---- the scenarios -------------------------------------------------------- */

/* The forking thread takes its child's SIGCHLD; no sibling is touched. */
static void scenario_target(const char *label) {
    struct world wd = {0};
    for (size_t i = 0; i < N_EVERY_WAIT; i++)
        world_add(&wd, every_wait[i], false);
    if (!world_init(label, &wd, 0))
        return;
    world_start(&wd);
    world_go();
    fork_child1(&wd);
    wait_child1(label, &wd, true);
    world_join(&wd);

    pid_t tid = 0;
    long at = 0;
    int n = runs_before(SIGCHLD, SETTLED_MS, &tid, &at);
    check(n == 1 && tid == st.main_tid && near(at, CHILD_EXIT_AT_MS),
          "%s: the handler ran %d time(s) before %dms, first in tid %d at %ldms; "
          "want once, in the forking thread (%d), at ~%dms",
          label, n, SETTLED_MS, (int) tid, at, (int) st.main_tid, CHILD_EXIT_AT_MS);
    for (size_t i = 0; i < wd.n; i++)
        check_untouched(label, &wd.w[i]);
    world_end(&wd);
}

/* The forking thread blocks SIGCHLD: exactly one sibling takes it. */
static void scenario_target_blocks(const char *label) {
    struct world wd = {0};
    for (size_t i = 0; i < N_EVERY_WAIT; i++)
        world_add(&wd, every_wait[i], false);
    if (!world_init(label, &wd, 0))
        return;
    mask_sigchld(SIG_BLOCK);
    world_start(&wd);
    world_go();
    fork_child1(&wd);
    wait_child1(label, &wd, true);
    world_join(&wd);

    pid_t tid = 0;
    long at = 0;
    int n = runs_before(SIGCHLD, SETTLED_MS, &tid, &at);
    struct waiter *taker = waiter_of(&wd, tid);
    check(n == 1 && taker != NULL && near(at, CHILD_EXIT_AT_MS),
          "%s: the handler ran %d time(s) before %dms, first in tid %d at %ldms; "
          "want once, in a sibling, at ~%dms",
          label, n, SETTLED_MS, (int) tid, at, CHILD_EXIT_AT_MS);
    for (size_t i = 0; i < wd.n; i++) {
        if (&wd.w[i] == taker)
            check_interrupted(label, &wd.w[i], at);
        else
            check_untouched(label, &wd.w[i]);
    }
    if (taker != NULL)
        test_logf("  %s: taken by the %s thread\n", label, call_names[taker->call]);
    world_end(&wd);
}

/* Every thread blocks SIGCHLD and one waits for it by name: that one gets it. */
static void scenario_sigtimedwait(const char *label) {
    struct world wd = {0};
    for (size_t i = 0; i < N_EVERY_WAIT; i++)
        world_add(&wd, every_wait[i], true);
    world_add(&wd, CALL_SIGCHLD_WAIT, true);
    if (!world_init(label, &wd, 0))
        return;
    mask_sigchld(SIG_BLOCK);
    world_start(&wd);
    world_go();
    fork_child1(&wd);
    wait_child1(label, &wd, true);
    world_join(&wd);

    check(runs_before(SIGCHLD, SETTLED_MS, NULL, NULL) == 0,
          "%s: the handler ran, want SIGCHLD taken by rt_sigtimedwait", label);
    for (size_t i = 0; i < wd.n; i++) {
        struct waiter *w = &wd.w[i];
        if (w->call != CALL_SIGCHLD_WAIT) {
            check_untouched(label, w);
            continue;
        }
        check(w->rc == SIGCHLD && near(w->end, CHILD_EXIT_AT_MS),
              "%s: %s = %ld (%s) at %ldms, want SIGCHLD at ~%dms",
              label, call_names[w->call], w->rc, errname(w->rc, w->err), w->end,
              CHILD_EXIT_AT_MS);
        check(w->cpu < CPU_LIMIT_MS, "%s: %s used %ldms of CPU, want it asleep",
              label, call_names[w->call], w->cpu);
    }
    world_end(&wd);
}

enum unblock_by { UNBLOCK_BY_SIGPROCMASK, UNBLOCK_BY_SIGSUSPEND };

static struct {
    enum unblock_by how;
    pid_t tid;
    long rc;
    int err;
    long returned;
    int runs_at_return;
} unblocker;

/* Blocks SIGCHLD like every other thread, then lets it through at
 * UNBLOCK_AT_MS -- after the child's SIGCHLD was queued with nobody able to
 * take it. */
static void *unblocker_main(void *arg) {
    (void) arg;
    unblocker.tid = gettid_();
    mask_sigchld(SIG_BLOCK);
    pthread_barrier_wait(&st.gate);
    sleep_until(st.t0 + UNBLOCK_AT_MS);
    errno = 0;
    if (unblocker.how == UNBLOCK_BY_SIGPROCMASK) {
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, SIGCHLD);
        unblocker.rc = pthread_sigmask(SIG_UNBLOCK, &set, NULL);
    } else {
        sigset_t none;
        sigemptyset(&none);
        unblocker.rc = sigsuspend(&none);
    }
    unblocker.err = errno;
    unblocker.returned = now_ms() - st.t0;
    unblocker.runs_at_return = __atomic_load_n(&nruns, __ATOMIC_ACQUIRE);
    return NULL;
}

static void scenario_unblocked_later(const char *label, enum unblock_by how) {
    struct world wd = {0};
    for (size_t i = 0; i < N_SOME_WAITS; i++)
        world_add(&wd, some_waits[i], true);
    if (!world_init(label, &wd, 1))
        return;
    memset(&unblocker, 0, sizeof(unblocker));
    unblocker.how = how;
    mask_sigchld(SIG_BLOCK);
    world_start(&wd);
    pthread_t unblocker_thread;
    pthread_create(&unblocker_thread, NULL, unblocker_main, NULL);
    world_go();
    fork_child1(&wd);
    wait_child1(label, &wd, true);
    sleep_until(st.t0 + UNBLOCK_AT_MS - 100);
    check(sigchld_pending() && runs_before(SIGCHLD, UNBLOCK_AT_MS, NULL, NULL) == 0,
          "%s: SIGCHLD queued and not taken while every thread blocks it", label);
    pthread_join(unblocker_thread, NULL);
    world_join(&wd);

    pid_t tid = 0;
    long at = 0;
    int n = runs_before(SIGCHLD, SETTLED_MS, &tid, &at);
    check(n == 1 && tid == unblocker.tid && at >= UNBLOCK_AT_MS - EARLY_MS &&
          unblocker.runs_at_return == 1,
          "%s: the handler ran %d time(s), first in tid %d at %ldms, %d time(s) when "
          "the unblock returned; want once, in the thread that unblocked it, before "
          "that returned", label, n, (int) tid, at, unblocker.runs_at_return);
    if (how == UNBLOCK_BY_SIGPROCMASK)
        check(unblocker.rc == 0 && near(unblocker.returned, UNBLOCK_AT_MS),
              "%s: pthread_sigmask = %ld at %ldms", label, unblocker.rc, unblocker.returned);
    else
        check(unblocker.rc == -1 && unblocker.err == EINTR &&
              near(unblocker.returned, UNBLOCK_AT_MS),
              "%s: sigsuspend = %ld (%s) at %ldms, want EINTR at ~%dms", label,
              unblocker.rc, errname(unblocker.rc, unblocker.err), unblocker.returned,
              UNBLOCK_AT_MS);
    for (size_t i = 0; i < wd.n; i++)
        check_untouched(label, &wd.w[i]);
    world_end(&wd);
}

static void scenario_unblocked_later_sigprocmask(const char *label) {
    scenario_unblocked_later(label, UNBLOCK_BY_SIGPROCMASK);
}

static void scenario_unblocked_later_sigsuspend(const char *label) {
    scenario_unblocked_later(label, UNBLOCK_BY_SIGSUSPEND);
}

/* The "handed on" scenario's sibling: SIGCHLD reaches it only inside
 * sigsuspend, where it waits the whole time. */
static struct {
    long rc;
    int err;
    long end;
    long cpu;
    pid_t tid;
} handover;

static void *handover_main(void *arg) {
    (void) arg;
    handover.tid = gettid_();
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigaddset(&mask, SIGUSR1);
    pthread_sigmask(SIG_SETMASK, &mask, NULL);
    pthread_barrier_wait(&st.gate);
    long cpu0 = clock_ms(CLOCK_THREAD_CPUTIME_ID);
    sigset_t none;
    sigemptyset(&none);
    errno = 0;
    handover.rc = sigsuspend(&none);
    handover.err = errno;
    handover.end = now_ms() - st.t0;
    handover.cpu = clock_ms(CLOCK_THREAD_CPUTIME_ID) - cpu0;
    return NULL;
}

static void scenario_handed_on(const char *label) {
    struct world wd = {0};
    for (size_t i = 0; i < N_SOME_WAITS; i++)
        world_add(&wd, some_waits[i], true);
    if (!world_init(label, &wd, 1))
        return;
    memset(&handover, 0, sizeof(handover));
    world_start(&wd);
    pthread_t sibling;
    pthread_create(&sibling, NULL, handover_main, NULL);
    st.extra_suspender = sibling;
    st.have_extra_suspender = true;
    world_go();
    fork_child1(&wd);
    /* Let SIGCHLD through and block it again, over and over, until it has
     * been taken or queued -- then stay blocked. Landing while this thread can
     * take it tells this thread, which may then block it before taking it. */
    long give_up = st.t0 + CHILD_EXIT_AT_MS + slack_ms(500);
    for (;;) {
        mask_sigchld(SIG_UNBLOCK);
        mask_sigchld(SIG_BLOCK);
        if (__atomic_load_n(&nruns, __ATOMIC_ACQUIRE) != 0 || sigchld_pending() ||
                now_ms() > give_up)
            break;
    }
    wait_child1(label, &wd, false);
    sleep_until(st.t0 + SETTLED_MS);
    bool stranded = sigchld_pending();
    world_join(&wd);
    pthread_join(sibling, NULL);

    pid_t tid = 0;
    long at = 0;
    int n = runs_before(SIGCHLD, SETTLED_MS, &tid, &at);
    check(!stranded && n == 1 && (tid == st.main_tid || tid == handover.tid),
          "%s: the handler ran %d time(s) before %dms, first in tid %d; SIGCHLD %s "
          "pending; want it taken once, by the forking thread or the sibling in "
          "sigsuspend", label, n, SETTLED_MS, (int) tid, stranded ? "still" : "not");
    if (tid == handover.tid) {
        check(handover.rc == -1 && handover.err == EINTR && near(handover.end, at),
              "%s: the sibling's sigsuspend = %ld (%s) at %ldms, want EINTR at ~%ldms",
              label, handover.rc, errname(handover.rc, handover.err), handover.end, at);
        test_logf("  %s: handed to the sibling (%ldms)\n", label, at);
    } else {
        check(handover.rc == -1 && handover.err == EINTR && usr1_hits == 1 &&
              near(handover.end, ARRIVAL_MS),
              "%s: the sibling's sigsuspend = %ld (%s) at %ldms, want it to wait for "
              "SIGUSR1 at ~%dms", label, handover.rc, errname(handover.rc, handover.err),
              handover.end, ARRIVAL_MS);
        test_logf("  %s: taken by the forking thread (%ldms)\n", label, at);
    }
    check(handover.cpu < CPU_LIMIT_MS, "%s: the sibling used %ldms of CPU, want it asleep",
          label, handover.cpu);
    for (size_t i = 0; i < wd.n; i++)
        check_untouched(label, &wd.w[i]);
    world_end(&wd);
}

/* The "lost the race" scenario's sibling: keeps calling sigprocmask, each of
 * which makes it look at the process's queued signals. */
static struct {
    pid_t tid;
    long calls;
    long failures;
} racer;

static void *racer_main(void *arg) {
    (void) arg;
    racer.tid = gettid_();
    sigset_t none;
    sigemptyset(&none);
    pthread_sigmask(SIG_SETMASK, &none, NULL);
    pthread_barrier_wait(&st.gate);
    sleep_until(st.t0 + FORK_AT_MS);
    while (!__atomic_load_n(&st.stop_spinning, __ATOMIC_ACQUIRE)) {
        if (pthread_sigmask(SIG_SETMASK, &none, NULL) != 0)
            racer.failures++;
        racer.calls++;
    }
    return NULL;
}

/* `suspend`: the forking thread waits in sigsuspend rather than a pipe read.
 * A read parked in the host looks again before it gives up, and finds the
 * signal gone; sigsuspend ends on the wake itself, and it is the restart
 * decision that has to see nothing is left to run. */
static void scenario_lost_race(const char *label, bool suspend) {
    struct world wd = {0};
    for (size_t i = 0; i < N_SOME_WAITS; i++)
        world_add(&wd, some_waits[i], true);
    if (!world_init(label, &wd, 1))
        return;
    memset(&racer, 0, sizeof(racer));
    sigset_t usr1;
    sigemptyset(&usr1);
    sigaddset(&usr1, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &usr1, NULL);   /* taken only in sigsuspend */
    st.main_thread = pthread_self();
    st.main_suspends = suspend;
    world_start(&wd);
    pthread_t racer_thread;
    pthread_create(&racer_thread, NULL, racer_main, NULL);
    world_go();
    fork_child1(&wd);
    long cpu0 = clock_ms(CLOCK_THREAD_CPUTIME_ID);
    long rc;
    errno = 0;
    if (suspend) {
        sigset_t none;
        sigemptyset(&none);
        rc = sigsuspend(&none);
    } else {
        char c;
        rc = read(st.main_pipe[0], &c, 1);
    }
    int err = errno;
    long end = now_ms() - st.t0;
    long cpu = clock_ms(CLOCK_THREAD_CPUTIME_ID) - cpu0;
    const char *call = suspend ? "sigsuspend" : "read";
    sleep_until(st.t0 + SETTLED_MS);
    __atomic_store_n(&st.stop_spinning, 1, __ATOMIC_RELEASE);
    pthread_join(racer_thread, NULL);
    wait_child1(label, &wd, false);
    world_join(&wd);

    pid_t tid = 0;
    long at = 0;
    int n = runs_before(SIGCHLD, SETTLED_MS, &tid, &at);
    check(n == 1 && (tid == st.main_tid || tid == racer.tid),
          "%s: the handler ran %d time(s) before %dms, first in tid %d; want once, in "
          "the forking thread or the sibling calling sigprocmask", label, n, SETTLED_MS,
          (int) tid);
    if (tid == st.main_tid) {
        check(rc == -1 && err == EINTR && near(end, at),
              "%s: main's %s = %ld (%s) at %ldms, want EINTR at ~%ldms, the handler "
              "having run in it", label, call, rc, errname(rc, err), end, at);
        test_logf("  %s: taken by the forking thread\n", label);
    } else {
        bool ok = suspend ? rc == -1 && err == EINTR && usr1_hits == 1 : rc == 1;
        check(ok && near(end, ARRIVAL_MS),
              "%s: main's %s = %ld (%s) at %ldms, want it to go on to ~%dms: the "
              "sibling ran the handler, so the call restarts", label, call, rc,
              errname(rc, err), end, ARRIVAL_MS);
        test_logf("  %s: taken by the sibling\n", label);
    }
    check(cpu < CPU_LIMIT_MS, "%s: main's %s used %ldms of CPU, want it asleep",
          label, call, cpu);
    check(racer.failures == 0 && racer.calls > 0,
          "%s: the sibling made %ld sigprocmask calls, %ld failed", label, racer.calls,
          racer.failures);
    for (size_t i = 0; i < wd.n; i++)
        check_untouched(label, &wd.w[i]);
    world_end(&wd);
}

static void scenario_lost_race_read(const char *label) {
    scenario_lost_race(label, false);
}

static void scenario_lost_race_sigsuspend(const char *label) {
    scenario_lost_race(label, true);
}

/* An interval timer's SIGALRM goes to the leader. */
static void scenario_itimer(const char *label) {
    struct world wd = {0};
    for (size_t i = 0; i < N_EVERY_WAIT; i++)
        world_add(&wd, every_wait[i], false);
    if (!world_init(label, &wd, 0))
        return;
    install(SIGALRM, on_signal);
    world_start(&wd);
    world_go();
    struct itimerval it = {.it_value = {CHILD_EXIT_AT_MS / 1000, (CHILD_EXIT_AT_MS % 1000) * 1000}};
    setitimer(ITIMER_REAL, &it, NULL);
    long cpu0 = clock_ms(CLOCK_THREAD_CPUTIME_ID);
    char c;
    errno = 0;
    long rc = read(st.main_pipe[0], &c, 1);
    int err = errno;
    long end = now_ms() - st.t0;
    long cpu = clock_ms(CLOCK_THREAD_CPUTIME_ID) - cpu0;
    alarm(test_watchdog_secs(30));  /* the itimer replaced the watchdog */
    world_join(&wd);

    pid_t tid = 0;
    long at = 0;
    int n = runs_before(SIGALRM, SETTLED_MS, &tid, &at);
    check(n == 1 && tid == st.main_tid && near(at, CHILD_EXIT_AT_MS),
          "%s: the handler ran %d time(s), first in tid %d at %ldms; want once, in the "
          "leader (%d), at ~%dms", label, n, (int) tid, at, (int) st.main_tid,
          CHILD_EXIT_AT_MS);
    check(rc == -1 && err == EINTR && near(end, CHILD_EXIT_AT_MS) && cpu < CPU_LIMIT_MS,
          "%s: the leader's read = %ld (%s) at %ldms, %ldms of CPU; want EINTR at ~%dms",
          label, rc, errname(rc, err), end, cpu, CHILD_EXIT_AT_MS);
    for (size_t i = 0; i < wd.n; i++)
        check_untouched(label, &wd.w[i]);
    world_end(&wd);
}

/* SIGALRM at SIG_DFL ends the process, every thread of it, when it comes.
 * Nothing to check in here: the harness looks at how and when it died. */
static void scenario_fatal(const char *label) {
    struct world wd = {0};
    for (size_t i = 0; i < N_EVERY_WAIT; i++)
        world_add(&wd, every_wait[i], false);
    if (!world_init(label, &wd, 0))
        return;
    signal(SIGALRM, SIG_DFL);
    world_start(&wd);
    world_go();
    struct itimerval it = {.it_value = {CHILD_EXIT_AT_MS / 1000, (CHILD_EXIT_AT_MS % 1000) * 1000}};
    setitimer(ITIMER_REAL, &it, NULL);
    char c;
    (void) read(st.main_pipe[0], &c, 1);
    world_join(&wd);
    check(0, "%s: still running at %ldms, want it killed by SIGALRM at ~%dms",
          label, now_ms() - st.t0, CHILD_EXIT_AT_MS);
    world_end(&wd);
}

/* ---- the harness ---------------------------------------------------------- */

struct run_slot {
    char label[64];
    pid_t pid;
    long started;
    int want_signal;    /* the scenario must die of this, or 0 */
};

/* Start a scenario in a process of its own, so every one starts with fresh
 * signal state and a hang or crash in one cannot take the others with it. */
static void scenario_start(struct run_slot *slot, const char *label,
        void (*fn)(const char *), int want_signal) {
    snprintf(slot->label, sizeof(slot->label), "%s", label);
    slot->want_signal = want_signal;
    fflush(stdout);
    slot->started = now_ms();
    slot->pid = fork();
    if (slot->pid < 0) {
        check(0, "%s: fork: %s", label, strerror(errno));
        return;
    }
    if (slot->pid == 0) {
        fn(slot->label);
        fflush(stdout);
        _exit(failures_total > 100 ? 100 : (int) failures_total);
    }
}

static void scenario_finish(struct run_slot *slot) {
    if (slot->pid <= 0)
        return;
    int status;
    while (waitpid(slot->pid, &status, 0) < 0 && errno == EINTR)
        continue;
    long elapsed = now_ms() - slot->started;
    if (slot->want_signal != 0) {
        /* Dead of it, and early: the process ended when the signal came, not
         * when its threads' waits would have. */
        check(WIFSIGNALED(status) && WTERMSIG(status) == slot->want_signal &&
              elapsed < ARRIVAL_MS - 200,
              "%s: status %#x after %ldms, want killed by signal %d at ~%dms",
              slot->label, status, elapsed, slot->want_signal, CHILD_EXIT_AT_MS);
        return;
    }
    if (WIFSIGNALED(status)) {
        check(0, "%s: killed by signal %d", slot->label, WTERMSIG(status));
    } else if (WEXITSTATUS(status) != 0) {
        failures_total += (unsigned) WEXITSTATUS(status);
    } else {
        test_logf("ok %s\n", slot->label);
    }
}

int main(int argc, char **argv) {
    test_init(argc, argv);

    struct run_slot slot;
    scenario_start(&slot, "target", scenario_target, 0);
    scenario_finish(&slot);
    scenario_start(&slot, "target blocks it", scenario_target_blocks, 0);
    scenario_finish(&slot);
    scenario_start(&slot, "sigtimedwait", scenario_sigtimedwait, 0);
    scenario_finish(&slot);
    scenario_start(&slot, "itimer", scenario_itimer, 0);
    scenario_finish(&slot);
    scenario_start(&slot, "fatal", scenario_fatal, SIGALRM);
    scenario_finish(&slot);

    /* The rest wait on other threads, not on each other: side by side. */
    struct run_slot slots[2 + 3 * RACE_ROUNDS];
    size_t n = 0;
    scenario_start(&slots[n++], "unblocked later (sigprocmask)",
                   scenario_unblocked_later_sigprocmask, 0);
    scenario_start(&slots[n++], "unblocked later (sigsuspend)",
                   scenario_unblocked_later_sigsuspend, 0);
    for (int r = 0; r < RACE_ROUNDS; r++) {
        char label[64];
        snprintf(label, sizeof(label), "handed on (round %d)", r + 1);
        scenario_start(&slots[n++], label, scenario_handed_on, 0);
        snprintf(label, sizeof(label), "lost the race, read (round %d)", r + 1);
        scenario_start(&slots[n++], label, scenario_lost_race_read, 0);
        snprintf(label, sizeof(label), "lost the race, sigsuspend (round %d)", r + 1);
        scenario_start(&slots[n++], label, scenario_lost_race_sigsuspend, 0);
    }
    for (size_t i = 0; i < n; i++)
        scenario_finish(&slots[i]);

    return finish_suite("signal_process_wake_one");
}
