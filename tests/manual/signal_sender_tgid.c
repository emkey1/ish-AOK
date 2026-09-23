/*
 * signal_sender_tgid.c -- a signal names the PROCESS that sent it, whichever
 * of its threads did the sending; and rt_sigqueueinfo sends what its caller
 * wrote.
 *
 * Linux fills si_pid with the sender's thread-group id for kill, tkill and
 * tgkill alike (prepare_kill_siginfo: task_tgid_vnr(current)), and for
 * pidfd_send_signal, which sends what kill does. AOK filled it with the
 * sending THREAD's id -- sys_gettid's value, which is the process's only for
 * the main thread. sigqueue() fills in si_pid itself, with getpid(), and
 * rt_sigqueueinfo overwrote it with the thread's id again, along with si_uid
 * and si_code (always SI_QUEUE).
 *
 * rt_sigqueueinfo and rt_tgsigqueueinfo copy the caller's siginfo as it is but
 * for si_signo; a negative si_code is a user's own and its pid, uid and value
 * are taken on trust. A code of zero or more, or SI_TKILL, would impersonate
 * the kernel or kill(), and is refused with EPERM unless the target is the
 * calling THREAD itself: Linux compares the target with task_pid_vnr(current),
 * so a worker naming its own process is refused and a main thread naming
 * itself is not. AOK let any of them through to anyone.
 *
 * Measured 2026-09-23 on Linux 6.12 (x86_64 and -m32) and on AOK's
 * alpine-amd64-test and devuan-amd64-test before the fix. A child's worker
 * thread signalled the test's process with each call: Linux's si_pid was the
 * child's pid, AOK's the worker's tid (12 for process 11). An rt_sigqueueinfo
 * naming pid 4242 and uid 4343 arrived on Linux as written, on AOK with the
 * worker's tid, uid 0 and SI_QUEUE; SI_USER, SI_TKILL and SI_KERNEL to another
 * process were EPERM on Linux and delivered by AOK. Also refused by Linux and
 * delivered by AOK: SI_USER from a worker to its own process, and
 * rt_tgsigqueueinfo SI_USER from a worker to the main thread.
 *
 * The same two calls' other answers, measured the same way: signal 0 is a probe
 * that returns 0 (AOK: EINVAL); pid 0 or below is ESRCH, not a process group
 * (AOK: EINVAL); an exited, unreaped child is there to be signalled and the
 * call returns 0, as kill() does (AOK: ESRCH); si_errno is the caller's (AOK:
 * 0). rt_tgsigqueueinfo with a tgid or tid of 0 is EINVAL on both.
 *
 * Signals are taken with a raw rt_sigtimedwait: glibc's sigtimedwait() rewrites
 * SI_TKILL as SI_USER.
 *
 * A stop taken by a thread other than the main one, and the SIGCHLD its parent
 * is sent, is tests/manual/notify_parent_cldstop.c's.
 *
 * Checked against Linux 6.12 (Devuan 6, x86_64 and -m32, unprivileged).
 *
 * Exits 0 and prints "signal_sender_tgid: PASS" on success.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

/* The same number on every architecture since Linux 5.1. */
#ifndef SYS_pidfd_send_signal
#define SYS_pidfd_send_signal 424
#endif
#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif
/* musl on i386 names the 32-bit-time call for what it takes. */
#if !defined(SYS_rt_sigtimedwait) && defined(SYS_rt_sigtimedwait_time32)
#define SYS_rt_sigtimedwait SYS_rt_sigtimedwait_time32
#endif

/* The layout the raw call takes on every ABI this test builds for: a long
 * pair, 64-bit on x86_64, arm64 and riscv64 and 32-bit on i386. */
struct kernel_timespec {
    long tv_sec;
    long tv_nsec;
};

/* Made-up sender details for rt_sigqueueinfo, which Linux passes through. */
#define FORGED_PID 4242
#define FORGED_UID 4343
#define FORGED_VALUE 4444

static pid_t raw_gettid(void) { return (pid_t) syscall(SYS_gettid); }
static int raw_tkill(pid_t tid, int sig) {
    return (int) syscall(SYS_tkill, tid, sig);
}
/* glibc declares tgkill(); this is the raw call under another name. */
static int raw_tgkill(pid_t tgid, pid_t tid, int sig) {
    return (int) syscall(SYS_tgkill, tgid, tid, sig);
}
static int raw_sigqueueinfo(pid_t pid, int sig, siginfo_t *si) {
    return (int) syscall(SYS_rt_sigqueueinfo, pid, sig, si);
}
static int raw_tgsigqueueinfo(pid_t tgid, pid_t tid, int sig, siginfo_t *si) {
    return (int) syscall(SYS_rt_tgsigqueueinfo, tgid, tid, sig, si);
}

static void nap(long ms) {
    struct timespec t = {ms / 1000, (ms % 1000) * 1000000L};
    while (nanosleep(&t, &t) < 0 && errno == EINTR)
        ;
}

static void expect(const char *what, long got, long want) {
    if (got == want) {
        test_logf("ok   %-58s %ld\n", what, got);
        return;
    }
    printf("FAIL %s: got %ld, want %ld\n", what, got, want);
    failures_total++;
}

static void note(const char *what, long got) {
    test_logf("     %-58s %ld\n", what, got);
}

/* How long to wait for a signal that should come. */
static long arrival_ms(void) { return 1000L * (long) test_watchdog_secs(3); }

/* Takes `sig`, which the caller blocks, waiting up to `ms` for it; si_signo is
 * 0 if it never came. A raw call, because glibc's sigtimedwait() reports
 * SI_TKILL as SI_USER. */
static siginfo_t take(int sig, long ms) {
    uint64_t set = 1ull << (sig - 1);
    struct kernel_timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    siginfo_t si;
    long r;
    do {
        memset(&si, 0, sizeof si);
        r = syscall(SYS_rt_sigtimedwait, &set, &si, &ts, sizeof(set));
    } while (r < 0 && errno == EINTR);
    if (r != sig)
        memset(&si, 0, sizeof si);
    return si;
}

static bool pending(int sig) {
    sigset_t set;
    sigemptyset(&set);
    return sigpending(&set) == 0 && sigismember(&set, sig);
}

static void block(int sig) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, sig);
    sigprocmask(SIG_BLOCK, &set, NULL);
}

/* ---- a child's worker thread signals its parent ------------------------ */

struct sent {
    pid_t pid;      /* the child's process id */
    pid_t tid;      /* its worker's thread id */
    int rc;
    int err;
};

typedef int (*send_fn)(void);

static send_fn worker_fn;
static struct sent worker_sent;

static void *worker_main(void *arg) {
    (void) arg;
    worker_sent.pid = getpid();
    worker_sent.tid = raw_gettid();
    errno = 0;
    worker_sent.rc = worker_fn();
    worker_sent.err = errno;
    return NULL;
}

/* Forks a child whose worker thread -- not its main thread -- runs fn, and
 * reports what fn returned, with the child's pid and the worker's tid. The
 * child has been reaped by the time this returns. */
static struct sent from_worker(send_fn fn) {
    struct sent s = {-1, -1, -1, 0};
    int p[2];
    if (pipe(p) < 0)
        return s;
    fflush(NULL);
    pid_t c = fork();
    if (c == 0) {
        close(p[0]);
        worker_fn = fn;
        pthread_t t;
        if (pthread_create(&t, NULL, worker_main, NULL) != 0)
            _exit(1);
        pthread_join(t, NULL);
        (void) !write(p[1], &worker_sent, sizeof worker_sent);
        _exit(0);
    }
    close(p[1]);
    if (c > 0) {
        if (read(p[0], &s, sizeof s) != (ssize_t) sizeof s)
            s = (struct sent) {-1, -1, -1, 0};
        waitpid(c, NULL, 0);
    }
    close(p[0]);
    return s;
}

/* The test's own process, which every child's worker signals. Its pid, not
 * the worker's getppid(): that is a separate question, and a worker's answer
 * was once its own process (tests/manual/getppid_thread.c). */
static pid_t receiver;

static int sig_kill, sig_tgkill, sig_tkill, sig_sigqueue, sig_pidfd;
static int sig_forged, sig_perm_user, sig_perm_tkill, sig_perm_kernel;

static int send_kill(void) { return kill(receiver, sig_kill); }
static int send_tgkill(void) { return raw_tgkill(receiver, receiver, sig_tgkill); }
static int send_tkill(void) { return raw_tkill(receiver, sig_tkill); }
static int send_sigqueue(void) {
    union sigval v = {.sival_int = FORGED_VALUE};
    return sigqueue(receiver, sig_sigqueue, v);
}
static int send_pidfd(void) {
    int fd = (int) syscall(SYS_pidfd_open, receiver, 0);
    if (fd < 0)
        return -1;
    int r = (int) syscall(SYS_pidfd_send_signal, fd, sig_pidfd, NULL, 0);
    int e = errno;
    close(fd);
    errno = e;
    return r;
}

static siginfo_t made_up(int sig, int code) {
    siginfo_t si;
    memset(&si, 0, sizeof si);
    si.si_signo = sig;
    si.si_code = code;
    si.si_pid = FORGED_PID;
    si.si_uid = FORGED_UID;
    si.si_value.sival_int = FORGED_VALUE;
    return si;
}

static int send_forged(void) {
    siginfo_t si = made_up(sig_forged, SI_QUEUE);
    return raw_sigqueueinfo(receiver, sig_forged, &si);
}
static int send_perm_user(void) {
    siginfo_t si = made_up(sig_perm_user, SI_USER);
    return raw_sigqueueinfo(receiver, sig_perm_user, &si);
}
static int send_perm_tkill(void) {
    siginfo_t si = made_up(sig_perm_tkill, SI_TKILL);
    return raw_sigqueueinfo(receiver, sig_perm_tkill, &si);
}
static int send_perm_kernel(void) {
    siginfo_t si = made_up(sig_perm_kernel, SI_KERNEL);
    return raw_sigqueueinfo(receiver, sig_perm_kernel, &si);
}

/* A sender named by its process id, and the rest of what it said. */
static void check_from_worker(const char *name, send_fn fn, int sig, int code) {
    struct sent s = from_worker(fn);
    char what[128];
    snprintf(what, sizeof what, "%s from a worker: rc", name);
    expect(what, s.rc, 0);
    if (s.rc != 0) {
        note("  errno", s.err);
        return;
    }
    siginfo_t si = take(sig, arrival_ms());
    snprintf(what, sizeof what, "%s from a worker: arrived", name);
    expect(what, si.si_signo, sig);
    if (si.si_signo != sig)
        return;
    note("  the worker's tid", s.tid);
    snprintf(what, sizeof what, "%s from a worker: si_pid is the process", name);
    expect(what, si.si_pid, s.pid);
    snprintf(what, sizeof what, "%s from a worker: si_code", name);
    expect(what, si.si_code, code);
    snprintf(what, sizeof what, "%s from a worker: si_uid", name);
    expect(what, (long) si.si_uid, (long) getuid());
    if (code == SI_QUEUE) {
        snprintf(what, sizeof what, "%s from a worker: si_value", name);
        expect(what, si.si_value.sival_int, FORGED_VALUE);
    }
}

static void case_senders(void) {
    check_from_worker("kill", send_kill, sig_kill, SI_USER);
    check_from_worker("tgkill", send_tgkill, sig_tgkill, SI_TKILL);
    check_from_worker("tkill", send_tkill, sig_tkill, SI_TKILL);
    check_from_worker("sigqueue", send_sigqueue, sig_sigqueue, SI_QUEUE);

    struct sent s = from_worker(send_pidfd);
    if (s.rc < 0 && s.err == ENOSYS) {
        test_logf("skip pidfd_send_signal: ENOSYS\n");
    } else {
        expect("pidfd_send_signal from a worker: rc", s.rc, 0);
        siginfo_t si = take(sig_pidfd, arrival_ms());
        expect("pidfd_send_signal from a worker: arrived", si.si_signo, sig_pidfd);
        if (si.si_signo == sig_pidfd) {
            expect("pidfd_send_signal from a worker: si_pid is the process",
                   si.si_pid, s.pid);
            expect("pidfd_send_signal from a worker: si_code", si.si_code, SI_USER);
        }
    }
}

/* rt_sigqueueinfo sends what the caller wrote: a negative si_code is a
 * user's own, and its pid and uid are not checked. */
static void case_sigqueueinfo_passes_through(void) {
    struct sent s = from_worker(send_forged);
    expect("rt_sigqueueinfo SI_QUEUE to another process: rc", s.rc, 0);
    siginfo_t si = take(sig_forged, arrival_ms());
    expect("rt_sigqueueinfo SI_QUEUE: arrived", si.si_signo, sig_forged);
    if (si.si_signo != sig_forged)
        return;
    note("  the sender's pid", s.pid);
    note("  the sender's tid", s.tid);
    expect("rt_sigqueueinfo SI_QUEUE: si_code as sent", si.si_code, SI_QUEUE);
    expect("rt_sigqueueinfo SI_QUEUE: si_pid as sent", si.si_pid, FORGED_PID);
    expect("rt_sigqueueinfo SI_QUEUE: si_uid as sent", (long) si.si_uid, FORGED_UID);
    expect("rt_sigqueueinfo SI_QUEUE: si_value as sent", si.si_value.sival_int,
           FORGED_VALUE);
}

/* ...but only a thread signalling itself may claim kill()'s codes or the
 * kernel's. */
static void check_refused(const char *what, send_fn fn, int sig) {
    struct sent s = from_worker(fn);
    char buf[128];
    snprintf(buf, sizeof buf, "%s: rc", what);
    expect(buf, s.rc, -1);
    snprintf(buf, sizeof buf, "%s: errno", what);
    expect(buf, s.err, EPERM);
    nap(50);
    snprintf(buf, sizeof buf, "%s: nothing arrived", what);
    expect(buf, pending(sig), 0);
    if (pending(sig))
        (void) take(sig, 0);
}

static void case_sigqueueinfo_refuses(void) {
    check_refused("rt_sigqueueinfo SI_USER to another process", send_perm_user,
                  sig_perm_user);
    check_refused("rt_sigqueueinfo SI_TKILL to another process", send_perm_tkill,
                  sig_perm_tkill);
    check_refused("rt_sigqueueinfo SI_KERNEL to another process", send_perm_kernel,
                  sig_perm_kernel);
}

/* ---- inside one process -------------------------------------------------- */

static int sig_self_user, sig_own_process, sig_tg_self, sig_tg_sibling;
static pid_t main_tid;

struct in_worker {
    int rc_own_process, err_own_process;
    int rc_tg_self, err_tg_self;
    siginfo_t tg_self;
    int rc_tg_sibling_user, err_tg_sibling_user;
    int rc_tg_sibling_queue, err_tg_sibling_queue;
};
static struct in_worker in_worker;

static void *sibling_main(void *arg) {
    (void) arg;
    siginfo_t si;

    /* Its own process's pid is not its own id: kill()'s code is refused. */
    si = made_up(sig_own_process, SI_USER);
    errno = 0;
    in_worker.rc_own_process = raw_sigqueueinfo(getpid(), sig_own_process, &si);
    in_worker.err_own_process = errno;

    /* Itself, by thread id: anything goes. */
    si = made_up(sig_tg_self, SI_USER);
    errno = 0;
    in_worker.rc_tg_self = raw_tgsigqueueinfo(getpid(), raw_gettid(), sig_tg_self, &si);
    in_worker.err_tg_self = errno;
    if (in_worker.rc_tg_self == 0)
        in_worker.tg_self = take(sig_tg_self, arrival_ms());

    /* The main thread is not itself. */
    si = made_up(sig_tg_sibling, SI_USER);
    errno = 0;
    in_worker.rc_tg_sibling_user = raw_tgsigqueueinfo(getpid(), main_tid,
                                                      sig_tg_sibling, &si);
    in_worker.err_tg_sibling_user = errno;
    si = made_up(sig_tg_sibling, SI_QUEUE);
    errno = 0;
    in_worker.rc_tg_sibling_queue = raw_tgsigqueueinfo(getpid(), main_tid,
                                                       sig_tg_sibling, &si);
    in_worker.err_tg_sibling_queue = errno;
    return NULL;
}

static void case_one_process(void) {
    /* The main thread's id is its process's: it may say SI_USER to itself. */
    siginfo_t si = made_up(sig_self_user, SI_USER);
    errno = 0;
    int rc = raw_sigqueueinfo(getpid(), sig_self_user, &si);
    expect("rt_sigqueueinfo SI_USER to itself from the main thread: rc", rc, 0);
    if (rc != 0)
        note("  errno", errno);
    si = take(sig_self_user, arrival_ms());
    expect("  arrived", si.si_signo, sig_self_user);
    if (si.si_signo == sig_self_user) {
        expect("  si_code as sent", si.si_code, SI_USER);
        expect("  si_pid as sent", si.si_pid, FORGED_PID);
        expect("  si_uid as sent", (long) si.si_uid, FORGED_UID);
    }

    main_tid = raw_gettid();
    memset(&in_worker, 0, sizeof in_worker);
    pthread_t t;
    if (pthread_create(&t, NULL, sibling_main, NULL) != 0) {
        expect("pthread_create", -1, 0);
        return;
    }
    pthread_join(t, NULL);

    expect("rt_sigqueueinfo SI_USER to its own process from a worker: rc",
           in_worker.rc_own_process, -1);
    expect("  errno", in_worker.err_own_process, EPERM);
    nap(50);
    expect("  nothing arrived", pending(sig_own_process), 0);
    if (pending(sig_own_process))
        (void) take(sig_own_process, 0);

    expect("rt_tgsigqueueinfo SI_USER to itself from a worker: rc",
           in_worker.rc_tg_self, 0);
    if (in_worker.rc_tg_self != 0)
        note("  errno", in_worker.err_tg_self);
    expect("  arrived", in_worker.tg_self.si_signo, sig_tg_self);
    if (in_worker.tg_self.si_signo == sig_tg_self) {
        expect("  si_code as sent", in_worker.tg_self.si_code, SI_USER);
        expect("  si_pid as sent", in_worker.tg_self.si_pid, FORGED_PID);
    }

    expect("rt_tgsigqueueinfo SI_USER to the main thread from a worker: rc",
           in_worker.rc_tg_sibling_user, -1);
    expect("  errno", in_worker.err_tg_sibling_user, EPERM);
    expect("rt_tgsigqueueinfo SI_QUEUE to the main thread from a worker: rc",
           in_worker.rc_tg_sibling_queue, 0);
    if (in_worker.rc_tg_sibling_queue != 0)
        note("  errno", in_worker.err_tg_sibling_queue);
    si = take(sig_tg_sibling, arrival_ms());
    expect("  arrived", si.si_signo, sig_tg_sibling);
    if (si.si_signo == sig_tg_sibling) {
        expect("  si_code as sent", si.si_code, SI_QUEUE);
        expect("  si_pid as sent", si.si_pid, FORGED_PID);
    }
    /* Only the SI_QUEUE one: the refused one never queued. */
    nap(50);
    expect("  nothing else arrived", pending(sig_tg_sibling), 0);
}

/* ---- the rest of rt_sigqueueinfo's answers ---------------------------------- */

static int sig_edge, sig_zombie;

static void expect_call(const char *what, int rc, int err, int want_rc, int want_err) {
    char buf[128];
    snprintf(buf, sizeof buf, "%s: rc", what);
    expect(buf, rc, want_rc);
    if (want_rc < 0) {
        snprintf(buf, sizeof buf, "%s: errno", what);
        expect(buf, rc < 0 ? err : 0, want_err);
    }
}

#define CALL(what, expr, want_rc, want_err)                                    \
    do {                                                                       \
        errno = 0;                                                             \
        int rc_ = (expr);                                                      \
        expect_call((what), rc_, errno, (want_rc), (want_err));                \
    } while (0)

static void case_sigqueueinfo_edges(void) {
    siginfo_t si = made_up(sig_edge, SI_QUEUE);
    pid_t me = getpid(), tid = raw_gettid();

    CALL("rt_sigqueueinfo signal 0 to itself", raw_sigqueueinfo(me, 0, &si), 0, 0);
    CALL("rt_tgsigqueueinfo signal 0 to itself",
         raw_tgsigqueueinfo(me, tid, 0, &si), 0, 0);
    CALL("rt_sigqueueinfo to pid 0", raw_sigqueueinfo(0, sig_edge, &si), -1, ESRCH);
    CALL("rt_sigqueueinfo to minus its own pid",
         raw_sigqueueinfo(-me, sig_edge, &si), -1, ESRCH);
    siginfo_t user = made_up(sig_edge, SI_USER);
    CALL("rt_sigqueueinfo SI_USER to pid 0", raw_sigqueueinfo(0, sig_edge, &user),
         -1, EPERM);
    CALL("rt_tgsigqueueinfo to tgid 0", raw_tgsigqueueinfo(0, tid, sig_edge, &si),
         -1, EINVAL);
    CALL("rt_tgsigqueueinfo to tid 0", raw_tgsigqueueinfo(me, 0, sig_edge, &si),
         -1, EINVAL);
    nap(50);
    expect("  nothing arrived", pending(sig_edge), 0);
    if (pending(sig_edge))
        (void) take(sig_edge, 0);

    /* si_signo is the signal; si_errno is whatever the caller said. */
    si = made_up(sig_zombie, SI_QUEUE);
    si.si_errno = 7;
    CALL("rt_sigqueueinfo with another si_signo and si_errno 7",
         raw_sigqueueinfo(me, sig_edge, &si), 0, 0);
    si = take(sig_edge, arrival_ms());
    expect("  arrived", si.si_signo, sig_edge);
    if (si.si_signo == sig_edge) {
        expect("  si_errno as sent", si.si_errno, 7);
        expect("  si_pid as sent", si.si_pid, FORGED_PID);
    }

    /* An exited child nobody has reaped is still there to be signalled. */
    fflush(NULL);
    pid_t c = fork();
    if (c == 0)
        _exit(0);
    siginfo_t wi;
    memset(&wi, 0, sizeof wi);
    int rc = waitid(P_PID, (id_t) c, &wi, WEXITED | WNOWAIT);
    expect("waitid(WNOWAIT) saw the child exit", rc == 0 ? wi.si_pid : -1, c);
    si = made_up(sig_zombie, SI_QUEUE);
    CALL("rt_sigqueueinfo to an unreaped child", raw_sigqueueinfo(c, sig_zombie, &si),
         0, 0);
    CALL("rt_sigqueueinfo signal 0 to an unreaped child",
         raw_sigqueueinfo(c, 0, &si), 0, 0);
    CALL("rt_tgsigqueueinfo to an unreaped child",
         raw_tgsigqueueinfo(c, c, sig_zombie, &si), 0, 0);
    waitpid(c, NULL, 0);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(120));
    receiver = getpid();

    sig_kill = SIGUSR1;
    sig_tgkill = SIGUSR2;
    sig_tkill = SIGRTMIN + 1;
    sig_sigqueue = SIGRTMIN + 2;
    sig_pidfd = SIGRTMIN + 3;
    sig_forged = SIGRTMIN + 4;
    sig_perm_user = SIGRTMIN + 5;
    sig_perm_tkill = SIGRTMIN + 6;
    sig_perm_kernel = SIGRTMIN + 7;
    sig_self_user = SIGRTMIN + 8;
    sig_own_process = SIGRTMIN + 9;
    sig_tg_self = SIGRTMIN + 10;
    sig_tg_sibling = SIGRTMIN + 11;
    sig_edge = SIGRTMIN + 12;
    sig_zombie = SIGRTMIN + 13;
    int all[] = {sig_kill, sig_tgkill, sig_tkill, sig_sigqueue, sig_pidfd,
                 sig_forged, sig_perm_user, sig_perm_tkill, sig_perm_kernel,
                 sig_self_user, sig_own_process, sig_tg_self, sig_tg_sibling,
                 sig_edge, sig_zombie};
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++)
        block(all[i]);

    case_senders();
    case_sigqueueinfo_passes_through();
    case_sigqueueinfo_refuses();
    case_one_process();
    case_sigqueueinfo_edges();

    return finish_suite("signal_sender_tgid");
}
