/*
 * notify_parent_cldstop.c -- a child's stop and continue, and a tracee's
 * ptrace stops, are announced with SIGCHLD, whatever signal the child was
 * cloned to announce its exit with.
 *
 * Linux's do_notify_parent_cldstop sends every one of them as SIGCHLD -- the
 * exit signal announces the exit and nothing else -- with si_code
 * CLD_STOPPED or CLD_CONTINUED to the parent, and CLD_TRAPPED or CLD_STOPPED
 * to a tracer, to the recipient's process; and it sends nothing when the
 * recipient's SIGCHLD is SIG_IGN or has SA_NOCLDSTOP.
 *
 * Measured 2026-09-23, Linux 6.12 against alpine-amd64-test, with a child made
 * by syscall(SYS_clone, SIGUSR1, 0, 0, 0, 0) that stops itself with SIGSTOP
 * and is then sent SIGCONT: Linux sends SIGCHLD with CLD_STOPPED, then with
 * CLD_CONTINUED, and only the exit uses SIGUSR1; with exit signal 0 the stop
 * and continue still send SIGCHLD and the exit sends nothing. AOK sent the
 * stop and the continue as the exit signal -- SIGUSR1, or nothing for 0 --
 * and so did a tracer's notice of every ptrace stop, with an empty siginfo
 * (SI_KERNEL, si_pid 0). The same run found, all in the same few lines:
 *   - SA_NOCLDSTOP was asked only when the signal was SIGCHLD, and a tracer's
 *     not at all, so a SIGUSR1 child's stops got through it;
 *   - SIG_IGN was not asked at all: SIGCHLD blocked, it was queued anyway,
 *     for a stop, a continue and an exit alike;
 *   - a stop taken by a thread other than the leader -- tgkill'd to it, or in
 *     a process whose leader had left -- was told to the thread that created
 *     that thread, in the stopped process itself, and the parent heard
 *     nothing;
 *   - each notice went to one thread's own queue, where a sibling waiting in
 *     sigtimedwait never saw it;
 *   - a tracer that is not the parent was not told of a continue.
 *
 * The scenarios, each in a process of its own that blocks SIGCHLD and SIGUSR1
 * and takes them with sigtimedwait:
 *   - a child that stops itself and is continued, with exit signal SIGUSR1, 0
 *     and SIGCHLD, and a parent whose SIGCHLD is at its default, has
 *     SA_NOCLDSTOP, or is SIG_IGN;
 *   - the stop taken by a worker thread, by tgkill and with the leader gone:
 *     the notice still names the leader and goes to its parent;
 *   - a sibling thread of the parent takes the notices;
 *   - the parent as tracer: a signal-delivery-stop (CLD_TRAPPED, the signal),
 *     with exit signal SIGUSR1 and 0; a group-stop (one CLD_STOPPED) and the
 *     continue once the tracer lets the tracee go; a syscall-stop and a
 *     PTRACE_EVENT_EXIT stop (CLD_TRAPPED, SIGTRAP); a seizing tracer's
 *     PTRACE_INTERRUPT stop (CLD_STOPPED, 0) and group-stop; and a tracer with
 *     SA_NOCLDSTOP or SIG_IGN, told nothing;
 *   - a tracer that is not the parent: a ptrace stop is the tracer's alone, a
 *     group-stop and a continue are told to both, and the exit reaches the
 *     tracer as SIGCHLD and then the parent as its exit signal.
 * Every scenario also checks nothing more arrives, so a notice sent twice --
 * a parent that is also the tracer was told of a group-stop twice -- fails,
 * when the second comes after the first was taken.
 *
 * Checked against Linux 6.12 (Devuan 6, x86_64 and -m32, unprivileged).
 *
 * Exits 0 and prints "notify_parent_cldstop: PASS" on success.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <linux/futex.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

#ifndef __WALL
#define __WALL 0x40000000
#endif
#ifndef PTRACE_SETOPTIONS
#define PTRACE_SETOPTIONS 0x4200
#endif
#ifndef PTRACE_SEIZE
#define PTRACE_SEIZE 0x4206
#endif
#ifndef PTRACE_INTERRUPT
#define PTRACE_INTERRUPT 0x4207
#endif
#ifndef PTRACE_O_TRACESYSGOOD
#define PTRACE_O_TRACESYSGOOD 0x01
#endif
#ifndef PTRACE_O_TRACEEXIT
#define PTRACE_O_TRACEEXIT 0x40
#endif
#ifndef PTRACE_EVENT_EXIT
#define PTRACE_EVENT_EXIT 6
#endif
#ifndef PTRACE_EVENT_STOP
#define PTRACE_EVENT_STOP 128
#endif
#if !defined(SYS_futex) && defined(SYS_futex_time32)
#define SYS_futex SYS_futex_time32
#endif

/* How long a notice that is due may take to arrive. */
#define ARRIVAL_MS 2000
/* How long to watch for one that is not due before calling it absent. */
#define SETTLE_MS 200
/* A child that is never released exits by itself after this, so that a case
 * that went wrong still ends. */
#define CHILD_LIMIT_MS 8000

/* wait status words */
#define STOPPED(sig) (((sig) << 8) | 0x7f)
#define EVENT_STOPPED(sig, event) (((event) << 16) | STOPPED(sig))
#define CONTINUED 0xffff
#define EXITED(code) ((code) << 8)

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

/* Scaled like the watchdogs, for a heavily loaded run. */
static long scaled_ms(long base) {
    return base * (long) test_watchdog_secs(1);
}

static void nap_ms(long ms) {
    struct timespec t = {ms / 1000, (ms % 1000) * 1000000L};
    while (nanosleep(&t, &t) != 0 && errno == EINTR)
        continue;
}

/* A child whose exit is announced with `exit_signal`, made with the raw
 * syscall: fork() without the libc bookkeeping. Such a child calls nothing
 * but raw syscalls, poll, read, write, ptrace, sigprocmask and _exit. */
static pid_t clone_child(int exit_signal) {
    return (pid_t) syscall(SYS_clone, (long) exit_signal, 0L, 0L, 0L, 0L);
}

/* Not raise(): it signals the thread libc believes it is, and a raw clone
 * child's thread descriptor is a copy of its parent's. */
static void stop_self(int sig) {
    syscall(SYS_kill, syscall(SYS_getpid), sig);
}

static long pt(int request, pid_t pid, long addr, long data) {
    return ptrace(request, pid, (void *) addr, (void *) data);
}

static const char *signame(int sig) {
    switch (sig) {
        case -1: return "nothing";
        case 0: return "no signal";
        case SIGCHLD: return "SIGCHLD";
        case SIGUSR1: return "SIGUSR1";
    }
    return "another signal";
}

static const char *codename(int code) {
    switch (code) {
        case CLD_EXITED: return "CLD_EXITED";
        case CLD_KILLED: return "CLD_KILLED";
        case CLD_DUMPED: return "CLD_DUMPED";
        case CLD_TRAPPED: return "CLD_TRAPPED";
        case CLD_STOPPED: return "CLD_STOPPED";
        case CLD_CONTINUED: return "CLD_CONTINUED";
        case SI_USER: return "SI_USER";
        case SI_KERNEL: return "SI_KERNEL";
    }
    return "?";
}

/* ---- notices: the SIGCHLD or SIGUSR1 a child's event sent ----------------- */

struct notice {
    int sig;        /* -1: nothing arrived */
    int code;
    int pid;
    int status;
};

static void block_signals(void) {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigaddset(&mask, SIGUSR1);
    sigprocmask(SIG_BLOCK, &mask, NULL);
}

/* Take SIGCHLD or SIGUSR1, waiting up to `ms`. */
static struct notice take_notice(long ms) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGCHLD);
    sigaddset(&set, SIGUSR1);
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    siginfo_t si;
    memset(&si, 0, sizeof(si));
    int sig;
    do {
        sig = sigtimedwait(&set, &si, &ts);
    } while (sig < 0 && errno == EINTR);
    struct notice n = {.sig = -1};
    if (sig > 0) {
        n.sig = sig;
        n.code = si.si_code;
        n.pid = (int) si.si_pid;
        n.status = si.si_status;
    }
    return n;
}

static const char *describe(struct notice n, char *buf, size_t len) {
    if (n.sig < 0)
        snprintf(buf, len, "nothing");
    else
        snprintf(buf, len, "%s (%d), si_code %s (%d), si_pid %d, si_status %d",
                 signame(n.sig), n.sig, codename(n.code), n.code, n.pid, n.status);
    return buf;
}

/* The SIGCHLD a stop, continue or trap sends: si_code `code`, naming `pid`,
 * with si_status `status`. */
static void check_notice(const char *label, const char *what, struct notice n, pid_t pid,
        int code, int status) {
    char buf[160];
    check(n.sig == SIGCHLD && n.code == code && n.pid == pid && n.status == status,
          "%s: %s sent %s; want SIGCHLD, %s, si_pid %d, si_status %d", label, what,
          describe(n, buf, sizeof(buf)), codename(code), (int) pid, status);
}

static void expect_notice(const char *label, const char *what, pid_t pid, int code, int status) {
    check_notice(label, what, take_notice(scaled_ms(ARRIVAL_MS)), pid, code, status);
}

/* Nothing arrives: watch for SETTLE_MS, then look at what is pending. */
static void expect_nothing(const char *label, const char *what) {
    nap_ms(scaled_ms(SETTLE_MS));
    struct notice n = take_notice(0);
    char buf[160];
    check(n.sig < 0, "%s: %s sent %s; want nothing", label, what, describe(n, buf, sizeof(buf)));
}

struct waited {
    pid_t rc;
    int err;
    int status;
};

static struct waited wait_for(pid_t pid, int options) {
    struct waited w = {.status = -1};
    do {
        errno = 0;
        w.rc = waitpid(pid, &w.status, options);
    } while (w.rc < 0 && errno == EINTR);
    w.err = w.rc < 0 ? errno : 0;
    return w;
}

static bool expect_wait(const char *label, const char *what, pid_t pid, int options,
        int want_status) {
    struct waited w = wait_for(pid, options);
    bool ok = w.rc == pid && w.status == want_status;
    check(ok, "%s: %s = %d (%s), status %#x; want %d, status %#x", label, what, (int) w.rc,
          w.rc < 0 ? strerror(w.err) : "-", w.status, (int) pid, want_status);
    return ok;
}

/* Until `pid` is a zombie, without reaping it. */
static void await_zombie(const char *label, pid_t pid) {
    long deadline = now_ms() + scaled_ms(CHILD_LIMIT_MS);
    siginfo_t si;
    int r;
    do {
        memset(&si, 0, sizeof(si));
        r = waitid(P_PID, (id_t) pid, &si, WEXITED | WNOHANG | WNOWAIT | __WALL);
        if (r == 0 && si.si_pid == pid)
            return;
        nap_ms(10);
    } while (now_ms() < deadline);
    check(0, "%s: the child never exited (waitid = %d, %s)", label, r,
          r < 0 ? strerror(errno) : "-");
}

/* A child's exit is announced with its exit signal -- the one thing that
 * signal is for -- carrying CLD_EXITED; with none, with nothing. The -m32
 * view of a SIGUSR1 carrying a child's status on x86_64 reads si_status 0
 * (tests/manual/wait_clone_child.c), so a 32-bit build accepts 0 there. */
static void expect_exit_notice(const char *label, int exit_signal, pid_t pid, int code) {
    if (exit_signal == 0) {
        await_zombie(label, pid);
        expect_nothing(label, "the exit, with no exit signal,");
        return;
    }
    struct notice n = take_notice(scaled_ms(ARRIVAL_MS));
    bool status_ok = n.status == code ||
        (exit_signal != SIGCHLD && sizeof(long) == 4 && n.status == 0);
    char buf[160];
    check(n.sig == exit_signal && n.code == CLD_EXITED && n.pid == pid && status_ok,
          "%s: the exit sent %s; want %s, CLD_EXITED, si_pid %d, si_status %d", label,
          describe(n, buf, sizeof(buf)), signame(exit_signal), (int) pid, code);
}

static void expect_reaped(const char *label, pid_t pid, int code) {
    expect_wait(label, "waitpid(child, __WALL)", pid, __WALL, EXITED(code));
}

/* ---- pipes ----------------------------------------------------------------- */

static bool open_pipe(const char *label, int fds[2]) {
    if (pipe(fds) == 0)
        return true;
    check(0, "%s: pipe: %s", label, strerror(errno));
    return false;
}

static void write_byte(const char *label, int fd) {
    if (write(fd, "x", 1) != 1)
        check(0, "%s: pipe write: %s", label, strerror(errno));
}

/* For a child: wait for a byte on `fd`, or give up after CHILD_LIMIT_MS. */
static void await_byte(int fd) {
    struct pollfd p = {.fd = fd, .events = POLLIN};
    long deadline = now_ms() + scaled_ms(CHILD_LIMIT_MS);
    for (;;) {
        long left = deadline - now_ms();
        if (left <= 0)
            return;
        int r = poll(&p, 1, (int) left);
        if (r > 0) {
            char c;
            (void) !read(fd, &c, 1);
            return;
        }
        if (r < 0 && errno != EINTR)
            return;
    }
}

static bool read_full(int fd, void *buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        ssize_t r = read(fd, (char *) buf + got, len - got);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return false;
        got += (size_t) r;
    }
    return true;
}

static void on_chld(int sig) {
    (void) sig;
}

/* ---- a child that stops itself and is continued ------------------------------ */

enum recipient {
    NOTIFIED,       /* SIGCHLD at its default */
    NOCLDSTOP,      /* a handler with SA_NOCLDSTOP */
    IGNORED,        /* SIG_IGN */
};

static void set_sigchld(enum recipient how) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    if (how == NOCLDSTOP) {
        sa.sa_handler = on_chld;
        sa.sa_flags = SA_NOCLDSTOP;
    } else {
        sa.sa_handler = how == IGNORED ? SIG_IGN : SIG_DFL;
    }
    sigaction(SIGCHLD, &sa, NULL);
}

/* The child stops itself with SIGSTOP, is sent SIGCONT and then exits `code`
 * once released. */
static void stop_and_continue(const char *label, int exit_signal, enum recipient how,
        int code) {
    set_sigchld(how);
    int release[2];
    if (!open_pipe(label, release))
        return;
    pid_t child = clone_child(exit_signal);
    if (child == 0) {
        close(release[1]);
        stop_self(SIGSTOP);
        await_byte(release[0]);
        _exit(code);
    }
    close(release[0]);
    if (child < 0) {
        check(0, "%s: clone: %s", label, strerror(errno));
        return;
    }

    bool told = how == NOTIFIED;
    if (told)
        expect_notice(label, "the stop", child, CLD_STOPPED, SIGSTOP);
    expect_wait(label, "waitpid(child, WUNTRACED|__WALL)", child, WUNTRACED | __WALL,
                STOPPED(SIGSTOP));
    expect_nothing(label, told ? "anything more for the stop" : "the stop");

    kill(child, SIGCONT);
    if (told)
        expect_notice(label, "the continue", child, CLD_CONTINUED, SIGCONT);
    expect_wait(label, "waitpid(child, WCONTINUED|__WALL)", child, WCONTINUED | __WALL,
                CONTINUED);
    expect_nothing(label, told ? "anything more for the continue" : "the continue");

    write_byte(label, release[1]);
    if (how == IGNORED && exit_signal == SIGCHLD) {
        /* Nobody is told, and nobody waits: the child is reaped as it exits. */
        long deadline = now_ms() + scaled_ms(CHILD_LIMIT_MS);
        struct waited w;
        do {
            w = wait_for(child, WNOHANG | __WALL);
            if (w.rc != 0)
                break;
            nap_ms(10);
        } while (now_ms() < deadline);
        check(w.rc == -1 && w.err == ECHILD,
              "%s: waitpid(child, WNOHANG|__WALL) after its exit = %d (%s), status %#x; "
              "want ECHILD", label, (int) w.rc, w.rc < 0 ? strerror(w.err) : "-", w.status);
        expect_nothing(label, "the exit");
        return;
    }
    expect_exit_notice(label, exit_signal, child, code);
    expect_reaped(label, child, code);
}

static void stop_usr1(void) { stop_and_continue("exit signal SIGUSR1", SIGUSR1, NOTIFIED, 9); }
static void stop_none(void) { stop_and_continue("no exit signal", 0, NOTIFIED, 10); }
static void stop_chld(void) { stop_and_continue("exit signal SIGCHLD", SIGCHLD, NOTIFIED, 11); }
static void nocldstop_usr1(void) { stop_and_continue("SA_NOCLDSTOP, SIGUSR1", SIGUSR1, NOCLDSTOP, 12); }
static void nocldstop_none(void) { stop_and_continue("SA_NOCLDSTOP, no exit signal", 0, NOCLDSTOP, 13); }
static void nocldstop_chld(void) { stop_and_continue("SA_NOCLDSTOP, SIGCHLD", SIGCHLD, NOCLDSTOP, 14); }
static void ignored_usr1(void) { stop_and_continue("SIG_IGN, SIGUSR1", SIGUSR1, IGNORED, 15); }
static void ignored_none(void) { stop_and_continue("SIG_IGN, no exit signal", 0, IGNORED, 16); }
static void ignored_chld(void) { stop_and_continue("SIG_IGN, SIGCHLD", SIGCHLD, IGNORED, 17); }

/* ---- a thread other than the leader takes the stop -------------------------- */

static struct {
    int to_parent;
    int release;
    int code;
    volatile int leader_word;
    bool leader_leaves;
} td;

static void *stop_worker(void *arg) {
    (void) arg;
    if (td.leader_leaves) {
        /* The kernel clears the word and wakes it -- a SHARED wake -- when the
         * leader is gone (set_tid_address). */
        while (__atomic_load_n(&td.leader_word, __ATOMIC_ACQUIRE) != 0)
            syscall(SYS_futex, &td.leader_word, FUTEX_WAIT, 1, NULL, NULL, 0);
    }
    pid_t tid = (pid_t) syscall(SYS_gettid);
    (void) !write(td.to_parent, &tid, sizeof(tid));
    await_byte(td.release);
    _exit(td.code);
}

/* The child has two threads. Either the leader has left (raw SYS_exit), so the
 * process's SIGSTOP can only go to the worker, or the worker is sent it by
 * tgkill. The notices still go to the child's parent and name the child --
 * the leader -- as a leader's own stop does. */
static void thread_stop(const char *label, int exit_signal, bool leader_leaves, int code) {
    int to_parent[2], release[2];
    if (!open_pipe(label, to_parent) || !open_pipe(label, release))
        return;
    pid_t child = clone_child(exit_signal);
    if (child == 0) {
        td.to_parent = to_parent[1];
        td.release = release[0];
        td.code = code;
        td.leader_leaves = leader_leaves;
        td.leader_word = 1;
        pthread_t worker;
        if (pthread_create(&worker, NULL, stop_worker, NULL) != 0)
            _exit(121);
        if (leader_leaves) {
            /* With the worker's code: a SIGCHLD that announces a process whose
             * leader left first carries the leader's own code, and wait the
             * last thread's (Linux 6.12). Not what this test is about. */
            syscall(SYS_set_tid_address, &td.leader_word);
            syscall(SYS_exit, code);
        }
        for (;;)
            pause();
    }
    close(to_parent[1]);
    close(release[0]);
    if (child < 0) {
        check(0, "%s: clone: %s", label, strerror(errno));
        return;
    }
    pid_t worker = -1;
    if (!read_full(to_parent[0], &worker, sizeof(worker)) || worker <= 0) {
        check(0, "%s: the child's worker never started", label);
        kill(child, SIGKILL);
        wait_for(child, __WALL);
        return;
    }
    check(worker != child, "%s: the worker is a thread of its own (tid %d)", label, (int) worker);

    if (leader_leaves)
        kill(child, SIGSTOP);
    else
        syscall(SYS_tgkill, child, worker, SIGSTOP);
    expect_notice(label, "the stop", child, CLD_STOPPED, SIGSTOP);
    expect_wait(label, "waitpid(child, WUNTRACED|__WALL)", child, WUNTRACED | __WALL,
                STOPPED(SIGSTOP));
    expect_nothing(label, "anything more for the stop");

    kill(child, SIGCONT);
    expect_notice(label, "the continue", child, CLD_CONTINUED, SIGCONT);
    expect_wait(label, "waitpid(child, WCONTINUED|__WALL)", child, WCONTINUED | __WALL,
                CONTINUED);
    expect_nothing(label, "anything more for the continue");

    write_byte(label, release[1]);
    expect_exit_notice(label, exit_signal, child, code);
    expect_reaped(label, child, code);
}

static void worker_stop_tgkill(void) {
    thread_stop("a worker is sent the stop", SIGUSR1, false, 21);
}

static void worker_stop_leader_gone(void) {
    thread_stop("the leader has left", SIGCHLD, true, 22);
}

/* ---- another thread of the parent takes them ---------------------------------- */

static void *take_in_thread(void *arg) {
    *(struct notice *) arg = take_notice(scaled_ms(ARRIVAL_MS));
    return NULL;
}

/* Every thread of this process blocks both signals, and one that is not the
 * child's parent waits for them in sigtimedwait: a notice is the PROCESS's,
 * as a child's exit is, and that thread can take it. */
static struct notice take_in_sibling(const char *label) {
    struct notice n = {.sig = -1};
    pthread_t t;
    if (pthread_create(&t, NULL, take_in_thread, &n) != 0) {
        check(0, "%s: pthread_create failed", label);
        return n;
    }
    pthread_join(t, NULL);
    return n;
}

static void sibling_takes_them(void) {
    const char *label = "a sibling thread takes the notices";
    int release[2];
    if (!open_pipe(label, release))
        return;
    pid_t child = clone_child(SIGUSR1);
    if (child == 0) {
        close(release[1]);
        stop_self(SIGSTOP);
        await_byte(release[0]);
        _exit(23);
    }
    close(release[0]);
    if (child < 0) {
        check(0, "%s: clone: %s", label, strerror(errno));
        return;
    }

    check_notice(label, "the stop", take_in_sibling(label), child, CLD_STOPPED, SIGSTOP);
    expect_wait(label, "waitpid(child, WUNTRACED|__WALL)", child, WUNTRACED | __WALL,
                STOPPED(SIGSTOP));
    kill(child, SIGCONT);
    check_notice(label, "the continue", take_in_sibling(label), child, CLD_CONTINUED, SIGCONT);
    expect_wait(label, "waitpid(child, WCONTINUED|__WALL)", child, WCONTINUED | __WALL,
                CONTINUED);
    write_byte(label, release[1]);
    struct notice n = take_in_sibling(label);
    char buf[160];
    check(n.sig == SIGUSR1 && n.code == CLD_EXITED && n.pid == child,
          "%s: the exit sent %s; want SIGUSR1, CLD_EXITED, si_pid %d", label,
          describe(n, buf, sizeof(buf)), (int) child);
    expect_reaped(label, child, 23);
    expect_nothing(label, "anything left over");
}

/* ---- the tracer is the parent --------------------------------------------------- */

/* The child blocks SIGCONT: a traced child would take it as a
 * signal-delivery-stop of its own, and its tracer hear of that too. Blocked,
 * it still continues the child, which is all these cases need of it. */
static void block_sigcont(void) {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCONT);
    sigprocmask(SIG_BLOCK, &mask, NULL);
}

/* Traced by its parent, the child stops itself with `sig` and exits `code`
 * once released. */
static pid_t traced_child(const char *label, int exit_signal, int sig, int release_fd,
        int code) {
    pid_t child = clone_child(exit_signal);
    if (child == 0) {
        block_sigcont();
        if (pt(PTRACE_TRACEME, 0, 0, 0) != 0)
            _exit(120);
        stop_self(sig);
        await_byte(release_fd);
        _exit(code);
    }
    if (child < 0)
        check(0, "%s: clone: %s", label, strerror(errno));
    return child;
}

/* A signal-delivery-stop: CLD_TRAPPED with the signal. */
static void traced_delivery(const char *label, int exit_signal, int code) {
    int release[2];
    if (!open_pipe(label, release))
        return;
    pid_t child = traced_child(label, exit_signal, SIGUSR2, release[0], code);
    close(release[0]);
    if (child < 0)
        return;
    expect_notice(label, "the SIGUSR2 signal-delivery-stop", child, CLD_TRAPPED, SIGUSR2);
    expect_wait(label, "waitpid(child, __WALL)", child, __WALL, STOPPED(SIGUSR2));
    expect_nothing(label, "anything more for the signal-delivery-stop");
    pt(PTRACE_CONT, child, 0, 0);
    write_byte(label, release[1]);
    /* Traced by its parent, a child still announces its exit with its own
     * exit signal. */
    expect_exit_notice(label, exit_signal, child, code);
    expect_reaped(label, child, code);
}

static void traced_delivery_usr1(void) {
    traced_delivery("tracer, exit signal SIGUSR1", SIGUSR1, 31);
}

static void traced_delivery_none(void) {
    traced_delivery("tracer, no exit signal", 0, 32);
}

/* The SIGSTOP is delivered, and the tracee group-stops: CLD_TRAPPED, then one
 * CLD_STOPPED -- a tracer that is also the parent is told once. A SIGCONT
 * sent while the tracer holds it in the group-stop is announced when the
 * tracer lets it go, and once. */
static void traced_group_stop(void) {
    const char *label = "tracer, group-stop";
    int release[2];
    if (!open_pipe(label, release))
        return;
    pid_t child = traced_child(label, SIGUSR1, SIGSTOP, release[0], 33);
    close(release[0]);
    if (child < 0)
        return;
    expect_notice(label, "the SIGSTOP signal-delivery-stop", child, CLD_TRAPPED, SIGSTOP);
    expect_wait(label, "waitpid(child, __WALL)", child, __WALL, STOPPED(SIGSTOP));
    expect_nothing(label, "anything more for the signal-delivery-stop");

    pt(PTRACE_CONT, child, 0, SIGSTOP);
    expect_notice(label, "the group-stop", child, CLD_STOPPED, SIGSTOP);
    expect_wait(label, "waitpid(child, __WALL) for the group-stop", child, __WALL,
                STOPPED(SIGSTOP));
    expect_nothing(label, "anything more for the group-stop");

    kill(child, SIGCONT);
    expect_nothing(label, "SIGCONT to a tracee held in its group-stop");
    pt(PTRACE_CONT, child, 0, 0);
    expect_notice(label, "the continue", child, CLD_CONTINUED, SIGCONT);
    expect_nothing(label, "anything more for the continue");

    write_byte(label, release[1]);
    expect_exit_notice(label, SIGUSR1, child, 33);
    expect_reaped(label, child, 33);
}

/* A syscall-stop and a PTRACE_EVENT_EXIT stop: CLD_TRAPPED with SIGTRAP --
 * the stop signal less the TRACESYSGOOD bit. */
static void traced_syscall_and_event(void) {
    const char *label = "tracer, syscall and event stops";
    int release[2];
    if (!open_pipe(label, release))
        return;
    pid_t child = traced_child(label, SIGUSR1, SIGUSR2, release[0], 34);
    close(release[0]);
    if (child < 0)
        return;
    expect_notice(label, "the SIGUSR2 signal-delivery-stop", child, CLD_TRAPPED, SIGUSR2);
    expect_wait(label, "waitpid(child, __WALL)", child, __WALL, STOPPED(SIGUSR2));
    if (pt(PTRACE_SETOPTIONS, child, 0, PTRACE_O_TRACESYSGOOD | PTRACE_O_TRACEEXIT) != 0)
        check(0, "%s: PTRACE_SETOPTIONS: %s", label, strerror(errno));

    pt(PTRACE_SYSCALL, child, 0, 0);
    expect_notice(label, "a syscall-stop", child, CLD_TRAPPED, SIGTRAP);
    expect_wait(label, "waitpid(child, __WALL) for the syscall-stop", child, __WALL,
                STOPPED(SIGTRAP | 0x80));
    expect_nothing(label, "anything more for the syscall-stop");

    pt(PTRACE_CONT, child, 0, 0);
    write_byte(label, release[1]);
    expect_notice(label, "the PTRACE_EVENT_EXIT stop", child, CLD_TRAPPED, SIGTRAP);
    expect_wait(label, "waitpid(child, __WALL) for the exit event", child, __WALL,
                EVENT_STOPPED(SIGTRAP, PTRACE_EVENT_EXIT));
    expect_nothing(label, "anything more for the exit event");

    pt(PTRACE_CONT, child, 0, 0);
    expect_exit_notice(label, SIGUSR1, child, 34);
    expect_reaped(label, child, 34);
}

/* A seized tracee: PTRACE_INTERRUPT's stop and a group-stop are both
 * PTRACE_EVENT_STOPs, and both CLD_STOPPED -- with the group's stop signal,
 * which is 0 while the group is not stopped. */
static void seized_stops(void) {
    const char *label = "seizing tracer";
    int go[2], release[2];
    if (!open_pipe(label, go) || !open_pipe(label, release))
        return;
    pid_t child = clone_child(SIGUSR1);
    if (child == 0) {
        close(go[1]);
        close(release[1]);
        block_sigcont();
        await_byte(go[0]);
        stop_self(SIGSTOP);
        await_byte(release[0]);
        _exit(35);
    }
    close(go[0]);
    close(release[0]);
    if (child < 0) {
        check(0, "%s: clone: %s", label, strerror(errno));
        return;
    }
    if (pt(PTRACE_SEIZE, child, 0, 0) != 0) {
        check(0, "%s: PTRACE_SEIZE: %s", label, strerror(errno));
        kill(child, SIGKILL);
        wait_for(child, __WALL);
        return;
    }

    pt(PTRACE_INTERRUPT, child, 0, 0);
    expect_notice(label, "the PTRACE_INTERRUPT stop", child, CLD_STOPPED, 0);
    expect_wait(label, "waitpid(child, __WALL) for the interrupt", child, __WALL,
                EVENT_STOPPED(SIGTRAP, PTRACE_EVENT_STOP));
    expect_nothing(label, "anything more for the interrupt");
    pt(PTRACE_CONT, child, 0, 0);

    write_byte(label, go[1]);
    expect_notice(label, "the SIGSTOP signal-delivery-stop", child, CLD_TRAPPED, SIGSTOP);
    expect_wait(label, "waitpid(child, __WALL)", child, __WALL, STOPPED(SIGSTOP));
    pt(PTRACE_CONT, child, 0, SIGSTOP);
    expect_notice(label, "the group-stop", child, CLD_STOPPED, SIGSTOP);
    expect_wait(label, "waitpid(child, __WALL) for the group-stop", child, __WALL,
                EVENT_STOPPED(SIGSTOP, PTRACE_EVENT_STOP));
    expect_nothing(label, "anything more for the group-stop");

    pt(PTRACE_CONT, child, 0, 0);
    write_byte(label, release[1]);
    expect_exit_notice(label, SIGUSR1, child, 35);
    expect_reaped(label, child, 35);
}

/* A tracer whose SIGCHLD is SA_NOCLDSTOP, or SIG_IGN, is told of no stop and
 * no continue -- only of the exit, which SIGUSR1 announces. */
static void quiet_tracer(const char *label, enum recipient how, int code) {
    set_sigchld(how);
    int release[2];
    if (!open_pipe(label, release))
        return;
    pid_t child = traced_child(label, SIGUSR1, SIGSTOP, release[0], code);
    close(release[0]);
    if (child < 0)
        return;
    expect_wait(label, "waitpid(child, __WALL)", child, __WALL, STOPPED(SIGSTOP));
    expect_nothing(label, "the signal-delivery-stop");
    pt(PTRACE_CONT, child, 0, SIGSTOP);
    expect_wait(label, "waitpid(child, __WALL) for the group-stop", child, __WALL,
                STOPPED(SIGSTOP));
    expect_nothing(label, "the group-stop");
    kill(child, SIGCONT);
    pt(PTRACE_CONT, child, 0, 0);
    expect_nothing(label, "the continue");
    write_byte(label, release[1]);
    expect_exit_notice(label, SIGUSR1, child, code);
    expect_reaped(label, child, code);
}

static void tracer_nocldstop(void) {
    quiet_tracer("tracer with SA_NOCLDSTOP", NOCLDSTOP, 36);
}

static void tracer_ignores(void) {
    quiet_tracer("tracer ignoring SIGCHLD", IGNORED, 37);
}

/* ---- a tracer that is not the parent -------------------------------------------- */

/* This process attaches to its grandchild C, whose parent P reports what it
 * is sent. A ptrace stop is the tracer's alone; a group-stop and the continue
 * are announced to both; the exit reaches the tracer as SIGCHLD and then, once
 * the tracer has waited for it, the parent as SIGUSR1. */
static void stranger_tracer(void) {
    const char *label = "tracer not the parent";
    int to_tracer[2], report[2], release[2];
    if (!open_pipe(label, to_tracer) || !open_pipe(label, report) || !open_pipe(label, release))
        return;
    fflush(stdout);
    pid_t p = fork();
    if (p == 0) {
        close(to_tracer[0]);
        close(report[0]);
        pid_t c = clone_child(SIGUSR1);
        if (c == 0) {
            close(report[1]);
            close(release[1]);
            /* Blocked before the tracer can attach: it names this process
             * only once this is done. */
            block_sigcont();
            pid_t self = (pid_t) syscall(SYS_getpid);
            (void) !write(to_tracer[1], &self, sizeof(self));
            close(to_tracer[1]);
            await_byte(release[0]);
            _exit(38);
        }
        close(release[0]);
        close(release[1]);
        close(to_tracer[1]);
        /* Everything C's events send here, in order, until its exit. */
        for (;;) {
            struct notice n = take_notice(scaled_ms(CHILD_LIMIT_MS + ARRIVAL_MS));
            if (n.sig < 0)
                break;
            (void) !write(report[1], &n, sizeof(n));
            if (n.code == CLD_EXITED && n.pid == c)
                break;
        }
        /* And whether it could reap C afterwards, as the last record. */
        struct waited w = {.rc = 0};
        long deadline = now_ms() + scaled_ms(ARRIVAL_MS);
        do {
            w = wait_for(c, WNOHANG | __WALL);
            if (w.rc != 0)
                break;
            nap_ms(10);
        } while (now_ms() < deadline);
        struct notice reaped = {.sig = 0, .code = w.rc == c ? 0 : -1, .pid = (int) w.rc,
                                .status = w.status};
        (void) !write(report[1], &reaped, sizeof(reaped));
        _exit(0);
    }
    close(to_tracer[1]);
    close(report[1]);
    close(release[0]);
    if (p < 0) {
        check(0, "%s: fork: %s", label, strerror(errno));
        return;
    }
    pid_t c = -1;
    if (!read_full(to_tracer[0], &c, sizeof(c)) || c <= 0) {
        check(0, "%s: the parent never made its child", label);
        wait_for(p, 0);
        return;
    }

    if (pt(PTRACE_ATTACH, c, 0, 0) != 0) {
        check(0, "%s: PTRACE_ATTACH: %s", label, strerror(errno));
        kill(c, SIGKILL);
        wait_for(p, 0);
        return;
    }
    /* PTRACE_ATTACH's SIGSTOP, delivered: the tracer's. */
    expect_notice(label, "the SIGSTOP signal-delivery-stop", c, CLD_TRAPPED, SIGSTOP);
    expect_wait(label, "waitpid(child, __WALL)", c, __WALL, STOPPED(SIGSTOP));
    expect_nothing(label, "anything more for the signal-delivery-stop");

    /* Injected, it stops the group: both hear of that. */
    pt(PTRACE_CONT, c, 0, SIGSTOP);
    expect_notice(label, "the group-stop", c, CLD_STOPPED, SIGSTOP);
    expect_wait(label, "waitpid(child, __WALL) for the group-stop", c, __WALL,
                STOPPED(SIGSTOP));
    expect_nothing(label, "anything more for the group-stop");

    /* A SIGCONT is announced once the tracer lets the tracee go -- to the
     * parent, and to the tracer as well, since it is not the parent. */
    kill(c, SIGCONT);
    expect_nothing(label, "SIGCONT to a tracee held in its group-stop");
    pt(PTRACE_CONT, c, 0, 0);
    expect_notice(label, "the continue", c, CLD_CONTINUED, SIGCONT);
    expect_nothing(label, "anything more for the continue");

    write_byte(label, release[1]);
    struct notice n = take_notice(scaled_ms(ARRIVAL_MS));
    char buf[160];
    check(n.sig == SIGCHLD && n.code == CLD_EXITED && n.pid == c && n.status == 38,
          "%s: the exit sent the tracer %s; want SIGCHLD, CLD_EXITED, si_pid %d, si_status 38",
          label, describe(n, buf, sizeof(buf)), (int) c);
    expect_wait(label, "the tracer's waitpid(child, __WALL)", c, __WALL, EXITED(38));

    /* What the parent was sent, in order. */
    struct notice got[8];
    int ngot = 0;
    struct notice rec;
    while (ngot < 8 && read_full(report[0], &rec, sizeof(rec)))
        got[ngot++] = rec;
    struct waited pw = wait_for(p, 0);
    check(pw.rc == p && pw.status == 0, "%s: the parent's own exit status %#x", label,
          pw.status);

    int i = 0;
    const char *what[] = {"the group-stop", "the continue"};
    const int codes[] = {CLD_STOPPED, CLD_CONTINUED};
    const int statuses[] = {SIGSTOP, SIGCONT};
    for (int k = 0; k < 2; k++) {
        char event[80];
        snprintf(event, sizeof(event), "%s, to the parent,", what[k]);
        if (i < ngot && got[i].sig != 0)
            check_notice(label, event, got[i++], c, codes[k], statuses[k]);
        else
            check(0, "%s: %s sent nothing; want SIGCHLD, %s", label, event, codename(codes[k]));
    }
    bool exit_ok = i < ngot && got[i].sig == SIGUSR1 && got[i].code == CLD_EXITED &&
        got[i].pid == c && (got[i].status == 38 || (sizeof(long) == 4 && got[i].status == 0));
    check(exit_ok, "%s: the exit, to the parent, sent %s; want SIGUSR1, CLD_EXITED, si_pid %d",
          label, i < ngot ? describe(got[i], buf, sizeof(buf)) : "nothing", (int) c);
    if (i < ngot && got[i].sig != 0)
        i++;
    for (; i < ngot && got[i].sig != 0; i++)
        check(0, "%s: the parent was also sent %s", label, describe(got[i], buf, sizeof(buf)));
    bool reaped = i < ngot && got[i].sig == 0 && got[i].code == 0 &&
        got[i].status == EXITED(38);
    check(reaped, "%s: the parent's waitpid(child, WNOHANG|__WALL) = %d, status %#x; want %d, "
          "status %#x", label, i < ngot ? got[i].pid : -1, i < ngot ? got[i].status : -1,
          (int) c, EXITED(38));
    /* The parent's own exit. */
    take_notice(0);
}

/* ---- the harness ------------------------------------------------------------------ */

/* Run a scenario in a process of its own, so that each starts with no
 * children, its own SIGCHLD disposition and nothing pending, and a hang in one
 * cannot take the others with it. */
static void scenario(const char *name, void (*run)(void)) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        check(0, "%s: fork: %s", name, strerror(errno));
        return;
    }
    if (pid == 0) {
        failures_total = 0;
        alarm(test_watchdog_secs(60));
        block_signals();
        run();
        fflush(stdout);
        _exit(failures_total > 100 ? 100 : (int) failures_total);
    }
    struct waited w = wait_for(pid, 0);
    if (w.rc != pid)
        check(0, "%s: waitpid: %s", name, strerror(w.err));
    else if (WIFSIGNALED(w.status))
        check(0, "%s: killed by signal %d", name, WTERMSIG(w.status));
    else if (WEXITSTATUS(w.status) != 0)
        failures_total += (unsigned) WEXITSTATUS(w.status);
    else
        test_logf("ok %s\n", name);
}

int main(int argc, char **argv) {
    test_init(argc, argv);

    scenario("exit signal SIGUSR1", stop_usr1);
    scenario("no exit signal", stop_none);
    scenario("exit signal SIGCHLD", stop_chld);
    scenario("SA_NOCLDSTOP, SIGUSR1", nocldstop_usr1);
    scenario("SA_NOCLDSTOP, no exit signal", nocldstop_none);
    scenario("SA_NOCLDSTOP, SIGCHLD", nocldstop_chld);
    scenario("SIG_IGN, SIGUSR1", ignored_usr1);
    scenario("SIG_IGN, no exit signal", ignored_none);
    scenario("SIG_IGN, SIGCHLD", ignored_chld);
    scenario("a worker is sent the stop", worker_stop_tgkill);
    scenario("the leader has left", worker_stop_leader_gone);
    scenario("a sibling thread takes the notices", sibling_takes_them);
    scenario("tracer, exit signal SIGUSR1", traced_delivery_usr1);
    scenario("tracer, no exit signal", traced_delivery_none);
    scenario("tracer, group-stop", traced_group_stop);
    scenario("tracer, syscall and event stops", traced_syscall_and_event);
    scenario("seizing tracer", seized_stops);
    scenario("tracer with SA_NOCLDSTOP", tracer_nocldstop);
    scenario("tracer ignoring SIGCHLD", tracer_ignores);
    scenario("tracer not the parent", stranger_tracer);

    return finish_suite("notify_parent_cldstop");
}
