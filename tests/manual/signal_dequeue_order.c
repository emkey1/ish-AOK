/*
 * signal_dequeue_order.c -- which pending signal is taken first, and where a
 * signal sent to a process waits.
 *
 * Linux takes a thread's pending signals in a fixed order:
 *
 *   1. get_signal first takes a signal an instruction raised -- a synchronous
 *      one (SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGFPE, SIGSYS) on the thread's
 *      own queue with a positive si_code -- oldest first
 *      (dequeue_synchronous_signal). sigtimedwait and signalfd do not.
 *   2. The thread's own queue before the process's shared one
 *      (dequeue_signal).
 *   3. Within a queue, a synchronous signal before any other, then the
 *      lowest-numbered (next_signal).
 *
 * Frames stack, so the first signal taken runs its handler LAST, and it is
 * the one whose handler decides whether the syscall it cut short restarts.
 * AOK took the lowest-numbered signal across both queues.
 *
 * And a signal sent to a process -- kill(pid), kill(0), sigqueue,
 * pidfd_send_signal -- waits on the process's queue, where any thread can
 * see it and take it: sigpending and signalfd in every thread, a sigwait
 * started after it was sent, a thread that unblocks it later. AOK queued it on
 * one thread chosen as it was sent. And SIGCONT and the stop signals cancel
 * each other on the process's queue and every thread's own
 * (prepare_signal), which AOK did for the one thread it was sent to.
 *
 *   own before shared      interval timer SIGALRM (the process's) and
 *                          raise(SIGTERM) (the thread's), unblocked together
 *   synchronous first      raised SIGINT and SIGSEGV, unblocked together
 *   fault first            a SIGSEGV queued with a positive si_code, a raised
 *                          SIGILL and SIGINT
 *   sigwaitinfo / signalfd the same queues taken by name
 *   restart decider        a read stopped with SIGSTOP; while it is stopped,
 *                          a child exits (SIGCHLD, the process's) and SIGURG
 *                          is sent to the thread; SIGCONT. The thread's
 *                          SIGURG decides: its SA_RESTART is what counts.
 *   kill vs raise          kill(getpid()), kill(0), sigqueue and
 *                          pidfd_send_signal against raise()
 *   process queue          every thread blocks the signal, kill(getpid()):
 *                          a worker's later sigtimedwait, sigpending,
 *                          signalfd, and unblocking all find it
 *   stop and continue      SIGCONT removes pending stop signals from every
 *                          thread, a stop signal removes SIGCONT -- sent to
 *                          the process, or to one thread
 *   /proc                  ShdPnd shows the process's pending signals
 *
 * Checked against Linux 6.12 (Devuan 6, x86_64 and -m32, unprivileged).
 *
 * Exits 0 and prints "signal_dequeue_order: PASS" on success.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

// Older headers predate both; the numbers are the same on every architecture.
#ifndef SYS_pidfd_send_signal
#define SYS_pidfd_send_signal 424
#endif
#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif
#ifndef SEGV_MAPERR
#define SEGV_MAPERR 1
#endif

#define MAX_EVENTS 8

/* The handlers that ran, in the order they ran, and the thread each ran in. */
static volatile int nevents;
static char events[MAX_EVENTS + 1];
static pid_t event_tid[MAX_EVENTS];

static pid_t current_tid(void) {
    return (pid_t) syscall(SYS_gettid);
}

static char letter_of(int sig) {
    switch (sig) {
        case SIGALRM: return 'A';
        case SIGTERM: return 'T';
        case SIGINT: return 'I';
        case SIGSEGV: return 'S';
        case SIGILL: return 'L';
        case SIGCHLD: return 'C';
        case SIGURG: return 'U';
        case SIGUSR1: return '1';
        case SIGUSR2: return '2';
        default: return '?';
    }
}

static void on_signal(int sig, siginfo_t *info, void *uc) {
    (void) info;
    (void) uc;
    int i = __atomic_fetch_add(&nevents, 1, __ATOMIC_SEQ_CST);
    if (i >= MAX_EVENTS)
        return;
    event_tid[i] = current_tid();
    events[i] = letter_of(sig);
}

static void events_reset(void) {
    nevents = 0;
    memset(events, 0, sizeof events);
}

static const char *events_str(void) {
    int n = nevents < MAX_EVENTS ? nevents : MAX_EVENTS;
    events[n] = '\0';
    return events;
}

static void install(int sig, int flags) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_signal;
    sa.sa_flags = SA_SIGINFO | flags;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
}

static sigset_t set_of(int a, int b, int c) {
    sigset_t set;
    sigemptyset(&set);
    if (a != 0)
        sigaddset(&set, a);
    if (b != 0)
        sigaddset(&set, b);
    if (c != 0)
        sigaddset(&set, c);
    return set;
}

static void check(bool ok, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (!ok) {
        printf("FAIL ");
        vprintf(fmt, ap);
        printf("\n");
        failures_total++;
    } else if (test_verbose) {
        vprintf(fmt, ap);
        printf("\n");
    }
    va_end(ap);
}

static void expect_events(const char *label, const char *want) {
    const char *got = events_str();
    check(strcmp(got, want) == 0, "%s: handlers ran \"%s\", expected \"%s\"",
          label, got, want);
}

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000 + (uint64_t) ts.tv_nsec / 1000000;
}

static void sleep_ms(unsigned ms) {
    struct timespec ts = { ms / 1000, (long) (ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
        ;
}

static int thread_kill(pid_t tgid, pid_t tid, int sig) {
    return (int) syscall(SYS_tgkill, tgid, tid, sig);
}

/* Queue SIG on this thread with a positive si_code, as the kernel does for a
 * signal an instruction raised. Only a thread's own queue takes that from
 * userspace. */
static int queue_fault_to_self(int sig) {
    siginfo_t si;
    memset(&si, 0, sizeof si);
    si.si_signo = sig;
    si.si_code = SEGV_MAPERR;
    return (int) syscall(SYS_rt_tgsigqueueinfo, getpid(), current_tid(), sig, &si);
}

/* Block SIGALRM and let an interval timer send it, which queues it on the
 * process. Returns once it is pending. */
static bool alarm_pending(void) {
    sigset_t alrm = set_of(SIGALRM, 0, 0);
    sigprocmask(SIG_BLOCK, &alrm, NULL);
    struct itimerval it;
    memset(&it, 0, sizeof it);
    it.it_value.tv_usec = 10000;
    if (setitimer(ITIMER_REAL, &it, NULL) != 0)
        return false;
    uint64_t deadline = now_ms() + 5000;
    while (now_ms() < deadline) {
        sigset_t pending;
        sigpending(&pending);
        if (sigismember(&pending, SIGALRM))
            return true;
        sleep_ms(5);
    }
    return false;
}

/* The state letter of /proc/PID/stat, or 0. */
static char proc_state(pid_t pid) {
    char path[64], buf[512];
    snprintf(path, sizeof path, "/proc/%d/stat", (int) pid);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[n] = '\0';
    char *paren = strrchr(buf, ')');
    return paren != NULL && paren[1] == ' ' ? paren[2] : 0;
}

static bool wait_state(pid_t pid, char want, unsigned timeout_ms) {
    uint64_t deadline = now_ms() + timeout_ms;
    while (now_ms() < deadline) {
        if (proc_state(pid) == want)
            return true;
        sleep_ms(5);
    }
    return false;
}

/* The hex mask on the "NAME:" line of /proc/self/status, or ~0. */
static uint64_t status_mask(const char *name) {
    char buf[4096];
    int fd = open("/proc/self/status", O_RDONLY);
    if (fd < 0)
        return ~0ull;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0)
        return ~0ull;
    buf[n] = '\0';
    size_t len = strlen(name);
    for (char *line = buf; line != NULL && *line != '\0'; ) {
        if (strncmp(line, name, len) == 0 && line[len] == ':')
            return strtoull(line + len + 1, NULL, 16);
        line = strchr(line, '\n');
        if (line != NULL)
            line++;
    }
    return ~0ull;
}

static uint64_t bit(int sig) {
    return 1ull << (sig - 1);
}

/* --------------------------------------------------- order within a thread */

/* An interval timer's SIGALRM waits on the process's queue, raise(SIGTERM) on
 * the thread's. SIGTERM is taken first and runs last. */
static void scenario_own_before_shared(void) {
    install(SIGALRM, 0);
    install(SIGTERM, 0);
    sigset_t both = set_of(SIGALRM, SIGTERM, 0);
    sigprocmask(SIG_BLOCK, &both, NULL);
    check(alarm_pending(), "own before shared: the timer's SIGALRM is pending");
    raise(SIGTERM);
    events_reset();
    sigprocmask(SIG_UNBLOCK, &both, NULL);
    expect_events("own before shared", "AT");
}

/* A synchronous signal is taken before a lower-numbered one. */
static void scenario_synchronous_first(void) {
    install(SIGINT, 0);
    install(SIGSEGV, 0);
    sigset_t both = set_of(SIGINT, SIGSEGV, 0);
    sigprocmask(SIG_BLOCK, &both, NULL);
    raise(SIGINT);
    raise(SIGSEGV);
    events_reset();
    sigprocmask(SIG_UNBLOCK, &both, NULL);
    expect_events("synchronous first", "IS");
}

/* SIGILL is raised before the SIGSEGV is queued, but the SIGSEGV carries a
 * positive si_code, as a fault's does: delivery takes it first, then SIGILL
 * (synchronous), then SIGINT. So SIGINT's handler runs first and SIGSEGV's
 * last. */
static void scenario_fault_first(void) {
    install(SIGINT, 0);
    install(SIGILL, 0);
    install(SIGSEGV, 0);
    sigset_t all = set_of(SIGINT, SIGILL, SIGSEGV);
    sigprocmask(SIG_BLOCK, &all, NULL);
    raise(SIGILL);
    check(queue_fault_to_self(SIGSEGV) == 0, "fault first: queue SIGSEGV (errno %d)", errno);
    raise(SIGINT);
    events_reset();
    sigprocmask(SIG_UNBLOCK, &all, NULL);
    expect_events("fault first", "ILS");
}

static void expect_taken(const char *label, int got, int want) {
    check(got == want, "%s: took %d, expected %d", label, got, want);
}

/* Taken by name, the same queues: synchronous first, and no fault step --
 * sigtimedwait is dequeue_signal alone. */
static void scenario_sigwaitinfo(void) {
    sigset_t two = set_of(SIGINT, SIGSEGV, 0);
    sigprocmask(SIG_BLOCK, &two, NULL);
    raise(SIGINT);
    raise(SIGSEGV);
    expect_taken("sigwaitinfo synchronous first, 1st", sigwaitinfo(&two, NULL), SIGSEGV);
    expect_taken("sigwaitinfo synchronous first, 2nd", sigwaitinfo(&two, NULL), SIGINT);

    sigset_t three = set_of(SIGINT, SIGILL, SIGSEGV);
    sigprocmask(SIG_BLOCK, &three, NULL);
    raise(SIGILL);
    check(queue_fault_to_self(SIGSEGV) == 0, "sigwaitinfo: queue SIGSEGV (errno %d)", errno);
    raise(SIGINT);
    expect_taken("sigwaitinfo no fault step, 1st", sigwaitinfo(&three, NULL), SIGILL);
    expect_taken("sigwaitinfo no fault step, 2nd", sigwaitinfo(&three, NULL), SIGSEGV);
    expect_taken("sigwaitinfo no fault step, 3rd", sigwaitinfo(&three, NULL), SIGINT);

    sigset_t both = set_of(SIGALRM, SIGTERM, 0);
    sigprocmask(SIG_BLOCK, &both, NULL);
    check(alarm_pending(), "sigtimedwait: the timer's SIGALRM is pending");
    raise(SIGTERM);
    struct timespec ts = { 2, 0 };
    expect_taken("sigtimedwait own before shared, 1st", sigtimedwait(&both, NULL, &ts), SIGTERM);
    expect_taken("sigtimedwait own before shared, 2nd", sigtimedwait(&both, NULL, &ts), SIGALRM);
}

static void scenario_signalfd(void) {
    sigset_t both = set_of(SIGALRM, SIGTERM, 0);
    sigprocmask(SIG_BLOCK, &both, NULL);
    int fd = signalfd(-1, &both, SFD_NONBLOCK | SFD_CLOEXEC);
    check(fd >= 0, "signalfd: created (errno %d)", errno);
    check(alarm_pending(), "signalfd: the timer's SIGALRM is pending");
    raise(SIGTERM);
    struct signalfd_siginfo info[4];
    memset(info, 0, sizeof info);
    ssize_t n = read(fd, info, sizeof info);
    check(n == 2 * (ssize_t) sizeof info[0], "signalfd: read %zd bytes, expected %zu",
          n, 2 * sizeof info[0]);
    expect_taken("signalfd own before shared, 1st", (int) info[0].ssi_signo, SIGTERM);
    expect_taken("signalfd own before shared, 2nd", (int) info[1].ssi_signo, SIGALRM);
    close(fd);
}

/* A read is stopped with SIGSTOP. While it is stopped a child exits, which
 * queues SIGCHLD on the process, and SIGURG is sent to the thread. After
 * SIGCONT the thread's SIGURG is taken first, so its handler is the one that
 * decides whether the read restarts, and runs after SIGCHLD's. */
static void scenario_restart_decider(bool own_restarts) {
    const char *label = own_restarts ? "restart decider, SIGURG restarts"
                                     : "restart decider, SIGCHLD restarts";
    install(SIGURG, own_restarts ? SA_RESTART : 0);
    install(SIGCHLD, own_restarts ? 0 : SA_RESTART);
    int data[2], release[2];
    if (pipe(data) != 0 || pipe(release) != 0) {
        check(false, "%s: pipes (errno %d)", label, errno);
        return;
    }
    pid_t self = getpid();
    pid_t child = fork();
    if (child == 0) {
        for (;;)
            pause();
    }
    pid_t helper = fork();
    if (helper == 0) {
        // Seen twice, a moment apart: sleeping in the read, not on its way.
        bool ok = wait_state(self, 'S', 10000);
        sleep_ms(50);
        ok = ok && wait_state(self, 'S', 10000);
        ok = ok && kill(self, SIGSTOP) == 0 && wait_state(self, 'T', 10000);
        ok = ok && kill(child, SIGKILL) == 0 && wait_state(child, 'Z', 10000);
        sleep_ms(100);
        ok = ok && thread_kill(self, self, SIGURG) == 0;
        kill(self, SIGCONT);
        // Late enough that a read which is to fail with EINTR has: on AOK
        // the read the stop rewound runs again, and would take data already
        // there.
        sleep_ms(1000 * test_watchdog_secs(1));
        if (write(data[1], ok ? "x" : "!", 1) != 1)
            _exit(2);
        char b;
        if (read(release[0], &b, 1) != 1)
            _exit(3);
        _exit(ok ? 0 : 1);
    }
    close(data[1]);
    close(release[0]);
    events_reset();
    char got = 0;
    ssize_t n = read(data[0], &got, 1);
    int err = errno;
    const char *ran = events_str();
    char order[MAX_EVENTS + 1];
    strcpy(order, ran);
    if (write(release[1], "r", 1) != 1)
        check(false, "%s: release the helper", label);
    int status = 0;
    check(waitpid(helper, &status, 0) == helper && WIFEXITED(status) &&
          WEXITSTATUS(status) == 0, "%s: the helper got through (status %#x)",
          label, (unsigned) status);
    waitpid(child, NULL, 0);

    check(strcmp(order, "CU") == 0, "%s: handlers ran \"%s\", expected \"CU\"", label, order);
    if (own_restarts)
        check(n == 1 && got == 'x', "%s: the read restarted and returned %zd '%c' (errno %d)",
              label, n, n == 1 ? got : '-', n < 0 ? err : 0);
    else
        check(n == -1 && err == EINTR, "%s: the read failed with EINTR (returned %zd, errno %d)",
              label, n, n < 0 ? err : 0);
}

/* ------------------------------------------------- a process's own queue */

static int pidfd_kill(pid_t pid, int sig) {
    int fd = (int) syscall(SYS_pidfd_open, pid, 0);
    if (fd < 0)
        return -1;
    int res = (int) syscall(SYS_pidfd_send_signal, fd, sig, NULL, 0);
    close(fd);
    return res;
}

/* SIGUSR1 sent to the process waits on its queue, SIGUSR2 raised on the
 * thread's: SIGUSR2 is taken first, and SIGUSR1's handler runs first. */
static void scenario_process_vs_raise(int how) {
    static const char *const names[] = { "kill(getpid())", "kill(0)", "sigqueue",
                                         "pidfd_send_signal" };
    char label[80];
    snprintf(label, sizeof label, "%s vs raise", names[how]);
    install(SIGUSR1, 0);
    install(SIGUSR2, 0);
    sigset_t both = set_of(SIGUSR1, SIGUSR2, 0);
    sigprocmask(SIG_BLOCK, &both, NULL);
    int res;
    switch (how) {
        case 0: res = kill(getpid(), SIGUSR1); break;
        case 1: res = setpgid(0, 0) == 0 ? kill(0, SIGUSR1) : -1; break;
        case 2: res = sigqueue(getpid(), SIGUSR1, (union sigval) { .sival_int = 1 }); break;
        default: res = pidfd_kill(getpid(), SIGUSR1); break;
    }
    check(res == 0, "%s: sent (errno %d)", label, errno);
    raise(SIGUSR2);
    events_reset();
    sigprocmask(SIG_UNBLOCK, &both, NULL);
    expect_events(label, "12");
}

/* A worker thread and a way to make it act, in turn. */
struct worker {
    pthread_t thread;
    int go[2];
    int done[2];
    void (*step)(struct worker *, int);
    pid_t tid;
    int result;
    sigset_t pending;
    uint64_t waited_ms;
};

static void *worker_main(void *arg) {
    struct worker *w = arg;
    w->tid = current_tid();
    if (write(w->done[1], "r", 1) != 1)
        return NULL;
    for (;;) {
        char c;
        if (read(w->go[0], &c, 1) != 1 || c == 'q')
            return NULL;
        w->step(w, c);
        if (write(w->done[1], "d", 1) != 1)
            return NULL;
    }
}

/* Returns once the worker is running, parked on its pipe, with the mask the
 * caller had. */
static bool worker_start(struct worker *w, void (*step)(struct worker *, int)) {
    memset(w, 0, sizeof *w);
    w->step = step;
    if (pipe(w->go) != 0 || pipe(w->done) != 0)
        return false;
    if (pthread_create(&w->thread, NULL, worker_main, w) != 0)
        return false;
    char c;
    return read(w->done[0], &c, 1) == 1 && w->tid != 0;
}

static void worker_do(struct worker *w, char c) {
    char d;
    if (write(w->go[1], &c, 1) != 1 || read(w->done[0], &d, 1) != 1)
        check(false, "worker step '%c' did not run", c);
}

static void worker_stop(struct worker *w) {
    if (write(w->go[1], "q", 1) == 1)
        pthread_join(w->thread, NULL);
}

static void step_sigtimedwait_usr1(struct worker *w, int c) {
    (void) c;
    sigset_t usr1 = set_of(SIGUSR1, 0, 0);
    struct timespec ts = { 2, 0 };
    uint64_t start = now_ms();
    w->result = sigtimedwait(&usr1, NULL, &ts);
    w->waited_ms = now_ms() - start;
}

/* Every thread blocks SIGUSR1, and the worker is not waiting for it when it
 * is sent. Its sigtimedwait afterwards finds it on the process's queue. */
static void scenario_sigwait_after(void) {
    sigset_t usr1 = set_of(SIGUSR1, 0, 0);
    sigprocmask(SIG_BLOCK, &usr1, NULL);
    struct worker w;
    check(worker_start(&w, step_sigtimedwait_usr1), "sigwait after: worker started");
    check(kill(getpid(), SIGUSR1) == 0, "sigwait after: kill (errno %d)", errno);
    worker_do(&w, 'w');
    check(w.result == SIGUSR1,
          "sigwait after: the worker's sigtimedwait took %d after %llu ms, expected %d",
          w.result, (unsigned long long) w.waited_ms, SIGUSR1);
    worker_stop(&w);
}

static void step_sigpending(struct worker *w, int c) {
    (void) c;
    sigpending(&w->pending);
}

/* Every thread blocks SIGUSR2 and SIGUSR1. Sent to the process, each is
 * pending in every thread; sent to the worker, only there. */
static void scenario_sigpending(void) {
    sigset_t both = set_of(SIGUSR1, SIGUSR2, 0);
    sigprocmask(SIG_BLOCK, &both, NULL);
    struct worker w;
    check(worker_start(&w, step_sigpending), "sigpending: worker started");
    check(kill(getpid(), SIGUSR2) == 0, "sigpending: kill (errno %d)", errno);
    check(thread_kill(getpid(), w.tid, SIGUSR1) == 0, "sigpending: tgkill (errno %d)", errno);
    worker_do(&w, 'p');
    sigset_t mine;
    sigpending(&mine);
    check(sigismember(&w.pending, SIGUSR2) == 1,
          "sigpending: the process's SIGUSR2 is pending in the worker");
    check(sigismember(&mine, SIGUSR2) == 1,
          "sigpending: the process's SIGUSR2 is pending in the main thread");
    check(sigismember(&w.pending, SIGUSR1) == 1,
          "sigpending: the worker's own SIGUSR1 is pending in the worker");
    check(sigismember(&mine, SIGUSR1) == 0,
          "sigpending: the worker's own SIGUSR1 is not pending in the main thread");
    worker_stop(&w);
}

static int worker_sfd = -1;

static void step_signalfd(struct worker *w, int c) {
    (void) c;
    struct pollfd pfd = { .fd = worker_sfd, .events = POLLIN };
    uint64_t start = now_ms();
    int ready = poll(&pfd, 1, 2000);
    w->waited_ms = now_ms() - start;
    w->result = 0;
    if (ready == 1) {
        struct signalfd_siginfo info;
        if (read(worker_sfd, &info, sizeof info) == (ssize_t) sizeof info)
            w->result = (int) info.ssi_signo;
    }
}

/* A signalfd read in a thread that is not the one the process's signal was
 * sent to. */
static void scenario_signalfd_worker(void) {
    sigset_t usr1 = set_of(SIGUSR1, 0, 0);
    sigprocmask(SIG_BLOCK, &usr1, NULL);
    worker_sfd = signalfd(-1, &usr1, SFD_NONBLOCK | SFD_CLOEXEC);
    check(worker_sfd >= 0, "signalfd in a worker: created (errno %d)", errno);
    struct worker w;
    check(worker_start(&w, step_signalfd), "signalfd in a worker: worker started");
    check(kill(getpid(), SIGUSR1) == 0, "signalfd in a worker: kill (errno %d)", errno);
    worker_do(&w, 's');
    check(w.result == SIGUSR1,
          "signalfd in a worker: read %d after %llu ms, expected %d",
          w.result, (unsigned long long) w.waited_ms, SIGUSR1);
    worker_stop(&w);
}

static void step_unblock_usr1(struct worker *w, int c) {
    (void) c;
    sigset_t usr1 = set_of(SIGUSR1, 0, 0);
    pthread_sigmask(SIG_UNBLOCK, &usr1, NULL);
    // Delivered as the call returns, if it is there to take.
    w->result = nevents;
}

/* Every thread blocks SIGUSR1 when it is sent; the worker then unblocks it and
 * takes it. */
static void scenario_unblock_takes(void) {
    install(SIGUSR1, 0);
    sigset_t usr1 = set_of(SIGUSR1, 0, 0);
    sigprocmask(SIG_BLOCK, &usr1, NULL);
    struct worker w;
    check(worker_start(&w, step_unblock_usr1), "unblock takes: worker started");
    events_reset();
    check(kill(getpid(), SIGUSR1) == 0, "unblock takes: kill (errno %d)", errno);
    worker_do(&w, 'u');
    check(w.result == 1 && nevents == 1 && event_tid[0] == w.tid,
          "unblock takes: the worker's handler ran as it unblocked (%d ran, in %d, worker %d)",
          w.result, nevents > 0 ? (int) event_tid[0] : 0, (int) w.tid);
    worker_stop(&w);
}

/* SIGCONT removes a pending stop signal from the process's queue and from
 * every thread's; a stop signal removes SIGCONT the same way. All of them
 * blocked, so nothing stops. */
static void scenario_stop_continue(void) {
    sigset_t stops = set_of(SIGTSTP, SIGTTIN, SIGCONT);
    sigaddset(&stops, SIGTTOU);
    sigprocmask(SIG_BLOCK, &stops, NULL);
    struct worker w;
    check(worker_start(&w, step_sigpending), "stop and continue: worker started");

    check(kill(getpid(), SIGTSTP) == 0, "stop and continue: kill SIGTSTP (errno %d)", errno);
    check(thread_kill(getpid(), w.tid, SIGTTIN) == 0, "stop and continue: tgkill SIGTTIN (errno %d)",
          errno);
    worker_do(&w, 'p');
    check(sigismember(&w.pending, SIGTSTP) == 1 && sigismember(&w.pending, SIGTTIN) == 1,
          "stop and continue: SIGTSTP and SIGTTIN pending in the worker before SIGCONT");
    // Unblocked, so that it is not left pending itself.
    sigset_t cont = set_of(SIGCONT, 0, 0);
    sigprocmask(SIG_UNBLOCK, &cont, NULL);
    check(kill(getpid(), SIGCONT) == 0, "stop and continue: kill SIGCONT (errno %d)", errno);
    worker_do(&w, 'p');
    sigset_t mine;
    sigpending(&mine);
    check(sigismember(&w.pending, SIGTSTP) == 0 && sigismember(&w.pending, SIGTTIN) == 0,
          "stop and continue: SIGCONT removed SIGTSTP and SIGTTIN from the worker (%d %d)",
          sigismember(&w.pending, SIGTSTP), sigismember(&w.pending, SIGTTIN));
    check(sigismember(&mine, SIGTSTP) == 0,
          "stop and continue: SIGCONT removed SIGTSTP from the main thread");

    sigprocmask(SIG_BLOCK, &cont, NULL);
    check(kill(getpid(), SIGCONT) == 0, "stop and continue: kill blocked SIGCONT (errno %d)",
          errno);
    check(thread_kill(getpid(), w.tid, SIGCONT) == 0, "stop and continue: tgkill SIGCONT (errno %d)",
          errno);
    worker_do(&w, 'p');
    check(sigismember(&w.pending, SIGCONT) == 1,
          "stop and continue: SIGCONT pending in the worker before SIGTSTP");
    check(kill(getpid(), SIGTSTP) == 0, "stop and continue: kill SIGTSTP again (errno %d)",
          errno);
    worker_do(&w, 'p');
    sigpending(&mine);
    check(sigismember(&w.pending, SIGCONT) == 0,
          "stop and continue: SIGTSTP removed SIGCONT from the worker");
    check(sigismember(&mine, SIGCONT) == 0,
          "stop and continue: SIGTSTP removed SIGCONT from the main thread");
    check(sigismember(&w.pending, SIGTSTP) == 1 && sigismember(&mine, SIGTSTP) == 1,
          "stop and continue: SIGTSTP pending in both threads");

    // Sent to one thread, SIGCONT still cancels a stop signal waiting on
    // another thread's own queue.
    sigprocmask(SIG_UNBLOCK, &cont, NULL);
    check(kill(getpid(), SIGCONT) == 0, "stop and continue: kill SIGCONT again (errno %d)",
          errno);
    check(thread_kill(getpid(), w.tid, SIGTTOU) == 0,
          "stop and continue: tgkill SIGTTOU (errno %d)", errno);
    worker_do(&w, 'p');
    check(sigismember(&w.pending, SIGTTOU) == 1,
          "stop and continue: SIGTTOU pending in the worker before SIGCONT");
    check(thread_kill(getpid(), getpid(), SIGCONT) == 0,
          "stop and continue: tgkill SIGCONT to the main thread (errno %d)", errno);
    worker_do(&w, 'p');
    check(sigismember(&w.pending, SIGTTOU) == 0,
          "stop and continue: SIGCONT to the main thread removed the worker's SIGTTOU");
    worker_stop(&w);
}

/* SigPnd is the thread's own queue, ShdPnd the process's. */
static void scenario_proc_status(void) {
    sigset_t both = set_of(SIGUSR1, SIGUSR2, 0);
    sigprocmask(SIG_BLOCK, &both, NULL);
    check(kill(getpid(), SIGUSR1) == 0, "/proc: kill (errno %d)", errno);
    raise(SIGUSR2);
    uint64_t sigpnd = status_mask("SigPnd");
    uint64_t shdpnd = status_mask("ShdPnd");
    uint64_t which = bit(SIGUSR1) | bit(SIGUSR2);
    check((sigpnd & which) == bit(SIGUSR2),
          "/proc: SigPnd holds the raised SIGUSR2 (%#llx)", (unsigned long long) sigpnd);
    check((shdpnd & which) == bit(SIGUSR1),
          "/proc: ShdPnd holds the process's SIGUSR1 (%#llx)", (unsigned long long) shdpnd);
}

/* ------------------------------------------------------------------- driver */

static void run(const char *name, void (*fn)(void)) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        /* This scenario's own failures, not the ones counted before it. */
        failures_total = 0;
        // Several scenarios use the interval timer, so the watchdog is the
        // parent's.
        alarm(0);
        fn();
        fflush(stdout);
        _exit(failures_total == 0 ? 0 : 1);
    }
    int status = 0;
    uint64_t deadline = now_ms() + 1000ull * test_watchdog_secs(60);
    pid_t got = 0;
    while (pid > 0 && (got = waitpid(pid, &status, WNOHANG)) == 0 && now_ms() < deadline)
        sleep_ms(10);
    if (pid > 0 && got == 0) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        printf("FAIL %s: timed out\n", name);
        failures_total++;
        return;
    }
    if (pid < 0 || got != pid) {
        printf("FAIL %s: could not run\n", name);
        failures_total++;
        return;
    }
    if (WIFSIGNALED(status)) {
        printf("FAIL %s: killed by signal %d\n", name, WTERMSIG(status));
        failures_total++;
    } else if (WEXITSTATUS(status) != 0) {
        failures_total++;
    }
}

static void process_kill(void) { scenario_process_vs_raise(0); }
static void process_kill0(void) { scenario_process_vs_raise(1); }
static void process_sigqueue(void) { scenario_process_vs_raise(2); }
static void process_pidfd(void) { scenario_process_vs_raise(3); }
static void decider_own(void) { scenario_restart_decider(true); }
static void decider_shared(void) { scenario_restart_decider(false); }

int main(int argc, char **argv) {
    test_init(argc, argv);
    setvbuf(stdout, NULL, _IOLBF, 0);

    run("own before shared", scenario_own_before_shared);
    run("synchronous first", scenario_synchronous_first);
    run("fault first", scenario_fault_first);
    run("sigwaitinfo", scenario_sigwaitinfo);
    run("signalfd", scenario_signalfd);
    run("restart decider, SIGURG restarts", decider_own);
    run("restart decider, SIGCHLD restarts", decider_shared);
    run("kill(getpid()) vs raise", process_kill);
    run("kill(0) vs raise", process_kill0);
    run("sigqueue vs raise", process_sigqueue);
    run("pidfd_send_signal vs raise", process_pidfd);
    run("sigwait after", scenario_sigwait_after);
    run("sigpending", scenario_sigpending);
    run("signalfd in a worker", scenario_signalfd_worker);
    run("unblock takes", scenario_unblock_takes);
    run("stop and continue", scenario_stop_continue);
    run("/proc status", scenario_proc_status);

    return finish_suite("signal_dequeue_order");
}
