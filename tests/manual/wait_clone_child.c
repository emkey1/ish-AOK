/*
 * wait_clone_child.c -- which children a wait is for: a child that announces
 * its exit with anything but SIGCHLD is a "clone child", and only __WCLONE or
 * __WALL waits for one.
 *
 * Linux's eligible_child: unless the wait passes __WALL or the child is traced
 * by the waiter, `(p->exit_signal != SIGCHLD) ^ !!(options & __WCLONE)` skips
 * the child. So a plain wait never sees a clone child, a __WCLONE wait sees
 * nothing else, and a wait that finds only children it is not for fails with
 * ECHILD -- WNOHANG or not, since they are not counted.
 *
 * Measured 2026-09-23, Linux 6.12 against alpine-amd64-test, with a child made
 * by syscall(SYS_clone, SIGUSR1, 0, 0, 0, 0) that exits 9: on Linux a plain
 * waitpid of it is ECHILD, and __WCLONE reaps it. AOK's do_wait asked nobody's
 * exit signal, so its plain waitpid reaped it, and it refused __WCLONE -- and
 * __WNOTHREAD -- with EINVAL, both of which Linux's wait4 and waitid accept.
 *
 * The rule makes the exit signal matter, and AOK had it wrong in three more
 * places, each measured the same day on Linux 6.12:
 *   - every exec makes the process a SIGCHLD child ("we have changed
 *     execution domain" in de_thread): a clone child that runs a program is an
 *     ordinary child from then on. AOK kept the clone's, and for a thread's
 *     exec copied the old leader's;
 *   - a CLONE_PARENT child announces itself the way its creator's process
 *     does, whatever the flags ask for;
 *   - and in a 64-bit siginfo, the exit signal carries the child's status:
 *     a SIGUSR1 handler reads si_status 9, where AOK gave 0. (The -m32 view on
 *     x86_64 reads 0 there, and a native i386 kernel 9, so 32-bit accepts
 *     either.)
 *
 * The scenarios, each in a process of its own:
 *   - clone child: its SIGUSR1 carries its pid and status; plain waitpid by
 *     pid, by -1 and by process group, and waitid by pid and P_ALL, are ECHILD;
 *     __WCLONE reaps it, and waitid reports it as SIGCHLD;
 *   - a running clone child alone: a blocking plain wait is ECHILD at once,
 *     where AOK blocked until the child exited and reaped it;
 *   - forked child: __WCLONE does not see it, a plain wait reaps it;
 *   - both at once: a blocking plain wait passes over a zombie clone child for
 *     the forked child that is still running; __WALL reaps both kinds;
 *   - no exit signal at all: a clone child, sending nothing;
 *   - exec by the leader, and by a thread: SIGCHLD, and a plain wait reaps;
 *   - CLONE_PARENT from a SIGCHLD child asking for SIGUSR1, and from a
 *     SIGUSR1 child asking for SIGCHLD: the creator's kind wins;
 *   - __WNOTHREAD: another thread's child is not this thread's to wait for;
 *   - a tracer: its tracee is always eligible, a clone child to a plain wait
 *     and a forked child to a __WCLONE one (Linux assumes __WALL for it);
 *   - the handler: a SIGUSR1 handler's siginfo;
 *   - option masks: wait4 and waitid take __WNOTHREAD, __WCLONE and __WALL,
 *     and still refuse a bit that is not a wait option.
 *
 * The clone children are made with the raw syscall and no stack, which is
 * fork() without the libc bookkeeping. They call nothing but _exit, read,
 * poll, execv, ptrace and raw syscalls before they exit or exec -- except the
 * thread-exec one, which needs a thread and gets it from pthread_create in a
 * copy of a single-threaded process.
 *
 * Checked against Linux 6.12 (Devuan 6, x86_64 and -m32, unprivileged).
 *
 * Exits 0 and prints "wait_clone_child: PASS" on success.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

#ifndef __WNOTHREAD
#define __WNOTHREAD 0x20000000
#endif
#ifndef __WALL
#define __WALL 0x40000000
#endif
#ifndef __WCLONE
#define __WCLONE 0x80000000
#endif
#ifndef CLONE_PARENT
#define CLONE_PARENT 0x00008000
#endif

/* How long a signal that is due may take to arrive. */
#define ARRIVAL_MS 2000
/* A child told to wait for its release exits by itself after this, so that a
 * wait that should have failed at once but blocked instead ends, and fails. */
#define CHILD_LIMIT_MS 3000

static char self_exe[4096];

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

/* A child whose exit is announced with `exit_signal`: SIGCHLD makes an
 * ordinary child, anything else -- 0 included -- a clone child. */
static pid_t clone_child(long flags) {
    return (pid_t) syscall(SYS_clone, flags, 0L, 0L, 0L, 0L);
}

static void block_signals(void) {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigaddset(&mask, SIGUSR1);
    sigprocmask(SIG_BLOCK, &mask, NULL);
}

static const char *signame(int sig) {
    return sig == SIGCHLD ? "SIGCHLD" : sig == SIGUSR1 ? "SIGUSR1" : sig < 0 ? "nothing" : "?";
}

/* Take SIGCHLD or SIGUSR1, waiting up to ARRIVAL_MS. Returns the signal, or
 * -1. */
static int take_signal(siginfo_t *si) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGCHLD);
    sigaddset(&set, SIGUSR1);
    long ms = scaled_ms(ARRIVAL_MS);
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    memset(si, 0, sizeof(*si));
    int sig;
    do {
        sig = sigtimedwait(&set, si, &ts);
    } while (sig < 0 && errno == EINTR);
    return sig;
}

/* The signal a child's exit sent, and who it names. The status is asked of a
 * 64-bit siginfo only: see the comment at the top. */
static void check_exit_signal(const char *label, int want_sig, pid_t child, int code) {
    siginfo_t si;
    int sig = take_signal(&si);
    check(sig == want_sig && si.si_code == CLD_EXITED && si.si_pid == child,
          "%s: the exit sent %s, code %d, si_pid %d; want %s, CLD_EXITED, %d",
          label, signame(sig), si.si_code, (int) si.si_pid, signame(want_sig), (int) child);
    if (sig != want_sig)
        return;
    if (sizeof(long) == 8 || want_sig == SIGCHLD)
        check(si.si_status == code, "%s: si_status %d, want %d", label, si.si_status, code);
    else
        check(si.si_status == code || si.si_status == 0,
              "%s: si_status %d, want %d or 0", label, si.si_status, code);
}

static bool nothing_pending(void) {
    sigset_t set;
    sigemptyset(&set);
    sigpending(&set);
    return !sigismember(&set, SIGCHLD) && !sigismember(&set, SIGUSR1);
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

static void expect_echild(const char *label, const char *what, struct waited w) {
    check(w.rc == -1 && w.err == ECHILD, "%s: %s = %d (%s), status %#x, want ECHILD",
          label, what, (int) w.rc, w.rc < 0 ? strerror(w.err) : "-", w.status);
}

static void expect_reaped(const char *label, const char *what, struct waited w,
        pid_t child, int code) {
    check(w.rc == child && WIFEXITED(w.status) && WEXITSTATUS(w.status) == code,
          "%s: %s = %d (%s), status %#x, want %d, exited %d",
          label, what, (int) w.rc, w.rc < 0 ? strerror(w.err) : "-", w.status,
          (int) child, code);
}

static int waitid_for(idtype_t type, id_t id, int options, siginfo_t *si) {
    int r;
    do {
        memset(si, 0, sizeof(*si));
        errno = 0;
        r = waitid(type, id, si, options);
    } while (r < 0 && errno == EINTR);
    return r;
}

static void expect_waitid_echild(const char *label, const char *what, idtype_t type,
        id_t id, int options) {
    siginfo_t si;
    int r = waitid_for(type, id, options, &si);
    int err = errno;
    check(r == -1 && err == ECHILD, "%s: %s = %d (%s), si_pid %d, want ECHILD",
          label, what, r, r < 0 ? strerror(err) : "-", (int) si.si_pid);
}

static void read_byte(int fd) {
    char c;
    while (read(fd, &c, 1) < 0 && errno == EINTR)
        continue;
}

/* For a child: wait for the release byte, or give up after CHILD_LIMIT_MS. */
static void await_release(int fd) {
    struct pollfd p = {.fd = fd, .events = POLLIN};
    long deadline = now_ms() + scaled_ms(CHILD_LIMIT_MS);
    for (;;) {
        long left = deadline - now_ms();
        if (left <= 0)
            return;
        int r = poll(&p, 1, (int) left);
        if (r > 0 || (r < 0 && errno != EINTR))
            return;
    }
}

static void write_byte(const char *label, int fd) {
    if (write(fd, "x", 1) != 1)
        check(0, "%s: pipe write: %s", label, strerror(errno));
}

static bool open_pipe(const char *label, int fds[2]) {
    if (pipe(fds) == 0)
        return true;
    check(0, "%s: pipe: %s", label, strerror(errno));
    return false;
}

static void exec_self(const char *mode, int code) {
    char arg[16];
    snprintf(arg, sizeof(arg), "%d", code);
    char *argv[] = {self_exe, (char *) mode, arg, NULL};
    execv(self_exe, argv);
    _exit(120);
}

/* ---- the scenarios --------------------------------------------------------- */

static void clone_child_alone(void) {
    const char *label = "clone child";
    pid_t child = clone_child(SIGUSR1);
    if (child == 0)
        _exit(9);
    check_exit_signal(label, SIGUSR1, child, 9);

    expect_echild(label, "waitpid(child, WNOHANG)", wait_for(child, WNOHANG));
    expect_echild(label, "waitpid(-1, WNOHANG)", wait_for(-1, WNOHANG));
    expect_echild(label, "waitpid(0, WNOHANG)", wait_for(0, WNOHANG));
    expect_waitid_echild(label, "waitid(P_PID, child, WEXITED|WNOHANG)", P_PID,
                         (id_t) child, WEXITED | WNOHANG);
    expect_waitid_echild(label, "waitid(P_ALL, WEXITED|WNOHANG)", P_ALL, 0,
                         WEXITED | WNOHANG);
    expect_waitid_echild(label, "waitid(P_PGID, own group, WEXITED|WNOHANG)", P_PGID,
                         (id_t) getpgrp(), WEXITED | WNOHANG);

    /* waitid names the kind of child it reports as SIGCHLD whatever it sent. */
    siginfo_t si;
    int r = waitid_for(P_PID, (id_t) child, WEXITED | WNOHANG | WNOWAIT | __WCLONE, &si);
    check(r == 0 && si.si_pid == child && si.si_signo == SIGCHLD &&
          si.si_code == CLD_EXITED && si.si_status == 9,
          "%s: waitid(P_PID, child, WNOWAIT|__WCLONE) = %d, si_pid %d, signo %d, "
          "code %d, status %d; want 0, %d, SIGCHLD, CLD_EXITED, 9",
          label, r, (int) si.si_pid, si.si_signo, si.si_code, si.si_status, (int) child);
    expect_reaped(label, "waitpid(child, WNOHANG|__WCLONE)",
                  wait_for(child, WNOHANG | __WCLONE), child, 9);
    expect_echild(label, "waitpid(-1, WNOHANG|__WALL) afterwards",
                  wait_for(-1, WNOHANG | __WALL));
}

static void running_clone_child(void) {
    const char *label = "running clone child";
    int release[2];
    if (!open_pipe(label, release))
        return;
    pid_t child = clone_child(SIGUSR1);
    if (child == 0) {
        close(release[1]);
        await_release(release[0]);
        _exit(9);
    }
    close(release[0]);

    long t0 = now_ms();
    struct waited w = wait_for(-1, 0);
    long took = now_ms() - t0;
    check(w.rc == -1 && w.err == ECHILD,
          "%s: blocking waitpid(-1, 0) = %d (%s) after %ldms, want ECHILD at once",
          label, (int) w.rc, w.rc < 0 ? strerror(w.err) : "-", took);
    if (w.rc == child) {
        /* The old answer: it waited for the child to exit and reaped it. */
        return;
    }
    t0 = now_ms();
    w = wait_for(child, 0);
    took = now_ms() - t0;
    check(w.rc == -1 && w.err == ECHILD,
          "%s: blocking waitpid(child, 0) = %d (%s) after %ldms, want ECHILD at once",
          label, (int) w.rc, w.rc < 0 ? strerror(w.err) : "-", took);
    if (w.rc == child)
        return;

    write_byte(label, release[1]);
    expect_reaped(label, "blocking waitpid(child, __WALL)", wait_for(child, __WALL), child, 9);
}

static void forked_child(void) {
    const char *label = "forked child";
    pid_t child = fork();
    if (child == 0)
        _exit(5);
    check_exit_signal(label, SIGCHLD, child, 5);
    expect_echild(label, "waitpid(child, WNOHANG|__WCLONE)", wait_for(child, WNOHANG | __WCLONE));
    expect_echild(label, "waitpid(-1, WNOHANG|__WCLONE)", wait_for(-1, WNOHANG | __WCLONE));
    expect_waitid_echild(label, "waitid(P_PID, child, WEXITED|WNOHANG|__WCLONE)", P_PID,
                         (id_t) child, WEXITED | WNOHANG | __WCLONE);
    expect_reaped(label, "waitpid(child, WNOHANG)", wait_for(child, WNOHANG), child, 5);
}

static void both_kinds(void) {
    const char *label = "both kinds";
    int release[2];
    if (!open_pipe(label, release))
        return;
    pid_t clone1 = clone_child(SIGUSR1);
    if (clone1 == 0)
        _exit(9);
    check_exit_signal(label, SIGUSR1, clone1, 9);
    pid_t forked = fork();
    if (forked == 0) {
        close(release[1]);
        await_release(release[0]);
        _exit(5);
    }
    close(release[0]);

    /* The zombie is not this wait's to report, and the forked child is still
     * running: nothing yet. */
    struct waited w = wait_for(-1, WNOHANG);
    check(w.rc == 0, "%s: waitpid(-1, WNOHANG) = %d (%s), want 0: a running child, "
          "and a zombie it is not for", label, (int) w.rc, w.rc < 0 ? strerror(w.err) : "-");
    write_byte(label, release[1]);
    expect_reaped(label, "blocking waitpid(-1, 0)", wait_for(-1, 0), forked, 5);
    check_exit_signal(label, SIGCHLD, forked, 5);
    expect_echild(label, "waitpid(-1, WNOHANG) with the clone zombie left",
                  wait_for(-1, WNOHANG));

    /* __WALL: both kinds. */
    pid_t clone2 = clone_child(SIGUSR1);
    if (clone2 == 0)
        _exit(3);
    check_exit_signal(label, SIGUSR1, clone2, 3);
    pid_t forked2 = fork();
    if (forked2 == 0)
        _exit(4);
    check_exit_signal(label, SIGCHLD, forked2, 4);
    bool seen_clone1 = false, seen_clone2 = false, seen_forked2 = false;
    for (int i = 0; i < 3; i++) {
        w = wait_for(-1, WNOHANG | __WALL);
        int code = w.rc > 0 && WIFEXITED(w.status) ? WEXITSTATUS(w.status) : -1;
        if (w.rc == clone1 && code == 9)
            seen_clone1 = true;
        else if (w.rc == clone2 && code == 3)
            seen_clone2 = true;
        else if (w.rc == forked2 && code == 4)
            seen_forked2 = true;
        else
            check(0, "%s: waitpid(-1, WNOHANG|__WALL) #%d = %d (%s), status %#x", label,
                  i + 1, (int) w.rc, w.rc < 0 ? strerror(w.err) : "-", w.status);
    }
    check(seen_clone1 && seen_clone2 && seen_forked2,
          "%s: __WALL reaped clone1 %d, clone2 %d, forked %d; want all three", label,
          seen_clone1, seen_clone2, seen_forked2);
    expect_echild(label, "waitpid(-1, WNOHANG|__WALL) afterwards",
                  wait_for(-1, WNOHANG | __WALL));
}

static void no_exit_signal(void) {
    const char *label = "no exit signal";
    pid_t child = clone_child(0);
    if (child == 0)
        _exit(11);
    /* Nothing is sent; ask until it is a zombie, without reaping it. */
    long deadline = now_ms() + scaled_ms(ARRIVAL_MS);
    siginfo_t si;
    int r;
    do {
        r = waitid_for(P_PID, (id_t) child, WEXITED | WNOHANG | WNOWAIT | __WALL, &si);
        if (r == 0 && si.si_pid == child)
            break;
        usleep(10000);
    } while (now_ms() < deadline);
    check(r == 0 && si.si_pid == child, "%s: the child never exited (waitid = %d, %s)",
          label, r, r < 0 ? strerror(errno) : "-");
    usleep(50000);
    check(nothing_pending(), "%s: an exit with no exit signal sent a signal", label);
    expect_echild(label, "waitpid(child, WNOHANG)", wait_for(child, WNOHANG));
    expect_echild(label, "waitpid(-1, WNOHANG)", wait_for(-1, WNOHANG));
    expect_reaped(label, "waitpid(child, WNOHANG|__WCLONE)",
                  wait_for(child, WNOHANG | __WCLONE), child, 11);
}

static void exec_by_leader(void) {
    const char *label = "exec by the leader";
    pid_t child = clone_child(SIGUSR1);
    if (child == 0)
        exec_self("--exit", 7);
    check_exit_signal(label, SIGCHLD, child, 7);
    expect_echild(label, "waitpid(child, WNOHANG|__WCLONE)",
                  wait_for(child, WNOHANG | __WCLONE));
    expect_reaped(label, "waitpid(child, WNOHANG)", wait_for(child, WNOHANG), child, 7);
}

static void *thread_exec_main(void *arg) {
    exec_self("--exit", (int) (long) arg);
    return NULL;
}

static void exec_by_thread(void) {
    const char *label = "exec by a thread";
    pid_t child = clone_child(SIGUSR1);
    if (child == 0) {
        /* Still a clone child: the thread's exec is its first. */
        pthread_t t;
        if (pthread_create(&t, NULL, thread_exec_main, (void *) 7L) != 0)
            _exit(121);
        for (;;)
            pause();
    }
    check_exit_signal(label, SIGCHLD, child, 7);
    expect_echild(label, "waitpid(child, WNOHANG|__WCLONE)",
                  wait_for(child, WNOHANG | __WCLONE));
    expect_reaped(label, "waitpid(child, WNOHANG)", wait_for(child, WNOHANG), child, 7);
}

/* The creator, a child of this process of kind `creator_sig`, makes a
 * CLONE_PARENT sibling asking for `asked_sig`, sends its pid here, and exits
 * 0. The sibling exits `code` once released. */
static void clone_parent_case(const char *label, int creator_sig, int asked_sig, int code) {
    int to_parent[2], release[2];
    if (!open_pipe(label, to_parent) || !open_pipe(label, release))
        return;
    pid_t creator = clone_child(creator_sig);
    if (creator == 0) {
        pid_t sibling = clone_child(CLONE_PARENT | asked_sig);
        if (sibling == 0) {
            close(release[1]);
            await_release(release[0]);
            _exit(code);
        }
        (void) !write(to_parent[1], &sibling, sizeof(sibling));
        _exit(0);
    }
    close(to_parent[1]);
    close(release[0]);
    pid_t sibling = -1;
    if (read(to_parent[0], &sibling, sizeof(sibling)) != (ssize_t) sizeof(sibling) ||
            sibling <= 0) {
        check(0, "%s: the creator could not make its sibling", label);
        return;
    }

    check_exit_signal(label, creator_sig, creator, 0);
    expect_reaped(label, "waitpid(creator, WNOHANG|__WALL)",
                  wait_for(creator, WNOHANG | __WALL), creator, 0);

    write_byte(label, release[1]);
    /* The sibling is this process's child, and its kind is its creator's. */
    check_exit_signal(label, creator_sig, sibling, code);
    int wrong = creator_sig == SIGCHLD ? __WCLONE : 0;
    int right = creator_sig == SIGCHLD ? 0 : __WCLONE;
    expect_echild(label, wrong ? "waitpid(sibling, WNOHANG|__WCLONE)" : "waitpid(sibling, WNOHANG)",
                  wait_for(sibling, WNOHANG | wrong));
    expect_reaped(label, right ? "waitpid(sibling, WNOHANG|__WCLONE)" : "waitpid(sibling, WNOHANG)",
                  wait_for(sibling, WNOHANG | right), sibling, code);
}

static void clone_parent_from_forked(void) {
    clone_parent_case("CLONE_PARENT from a forked child", SIGCHLD, SIGUSR1, 3);
}

static void clone_parent_from_clone(void) {
    clone_parent_case("CLONE_PARENT from a clone child", SIGUSR1, SIGCHLD, 4);
}

static struct {
    int to_main[2];
    int to_worker[2];
    struct waited reaped;
} nt;

static void *nothread_worker(void *arg) {
    (void) arg;
    pid_t child = fork();
    if (child == 0)
        _exit(6);
    (void) !write(nt.to_main[1], &child, sizeof(child));
    read_byte(nt.to_worker[0]);
    nt.reaped = wait_for(child, WNOHANG | __WNOTHREAD);
    return NULL;
}

static void wnothread(void) {
    const char *label = "__WNOTHREAD";
    if (!open_pipe(label, nt.to_main) || !open_pipe(label, nt.to_worker))
        return;
    pthread_t worker;
    if (pthread_create(&worker, NULL, nothread_worker, NULL) != 0) {
        check(0, "%s: pthread_create failed", label);
        return;
    }
    pid_t child = -1;
    if (read(nt.to_main[0], &child, sizeof(child)) != (ssize_t) sizeof(child) || child <= 0) {
        check(0, "%s: the worker could not fork", label);
        return;
    }
    check_exit_signal(label, SIGCHLD, child, 6);

    /* Visible to this thread without the flag: the control. */
    siginfo_t si;
    int r = waitid_for(P_PID, (id_t) child, WEXITED | WNOHANG | WNOWAIT, &si);
    check(r == 0 && si.si_pid == child,
          "%s: control: waitid(P_PID, worker's child, WNOWAIT) = %d, si_pid %d, want it",
          label, r, (int) si.si_pid);
    expect_echild(label, "waitpid(worker's child, WNOHANG|__WNOTHREAD)",
                  wait_for(child, WNOHANG | __WNOTHREAD));
    expect_echild(label, "waitpid(-1, WNOHANG|__WNOTHREAD)",
                  wait_for(-1, WNOHANG | __WNOTHREAD));
    expect_waitid_echild(label, "waitid(P_ALL, WEXITED|WNOHANG|__WNOTHREAD)", P_ALL, 0,
                         WEXITED | WNOHANG | __WNOTHREAD);

    write_byte(label, nt.to_worker[1]);
    pthread_join(worker, NULL);
    expect_reaped(label, "the worker's own waitpid(child, WNOHANG|__WNOTHREAD)",
                  nt.reaped, child, 6);
}

/* The child puts itself under its parent's trace, says so, stops, and then
 * exits `code`. Until it says so it is not traced, and a wait it is not
 * eligible for fails at once. */
static void traced_child_body(int traced_fd, int code) {
    if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) != 0)
        _exit(122);
    (void) !write(traced_fd, "x", 1);
    syscall(SYS_kill, syscall(SYS_getpid), SIGSTOP);
    _exit(code);
}

static void traced_case(const char *label, int exit_signal, int extra, int code) {
    int traced[2];
    if (!open_pipe(label, traced))
        return;
    pid_t child = clone_child(exit_signal);
    if (child == 0)
        traced_child_body(traced[1], code);
    close(traced[1]);
    read_byte(traced[0]);

    /* By pid: the P_PID branch's tracer question. */
    struct waited w = wait_for(child, extra);
    check(w.rc == child && WIFSTOPPED(w.status) && WSTOPSIG(w.status) == SIGSTOP,
          "%s: waitpid(child, %#x) = %d (%s), status %#x, want its SIGSTOP stop",
          label, extra, (int) w.rc, w.rc < 0 ? strerror(w.err) : "-", w.status);
    if (w.rc != child || !WIFSTOPPED(w.status)) {
        kill(child, SIGKILL);
        wait_for(child, __WALL);
        return;
    }
    if (ptrace(PTRACE_CONT, child, NULL, NULL) != 0)
        check(0, "%s: PTRACE_CONT: %s", label, strerror(errno));
    /* By -1: the ptracees loop. */
    expect_reaped(label, extra ? "waitpid(-1, __WCLONE)" : "waitpid(-1, 0)",
                  wait_for(-1, extra), child, code);
}

static void traced_clone_child(void) {
    traced_case("tracer, clone child", SIGUSR1, 0, 9);
}

static void traced_forked_child(void) {
    traced_case("tracer, forked child, __WCLONE", SIGCHLD, __WCLONE, 5);
}

static volatile int handled_signo, handled_code, handled_pid, handled_status;

static void on_usr1(int sig, siginfo_t *si, void *ctx) {
    (void) sig;
    (void) ctx;
    handled_signo = si->si_signo;
    handled_code = si->si_code;
    handled_pid = si->si_pid;
    handled_status = si->si_status;
}

static void handler_siginfo(void) {
    const char *label = "handler";
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_usr1;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);

    pid_t child = clone_child(SIGUSR1);
    if (child == 0)
        _exit(9);
    sigset_t wait_mask;
    sigprocmask(SIG_SETMASK, NULL, &wait_mask);
    sigdelset(&wait_mask, SIGUSR1);
    sigsuspend(&wait_mask);
    check(handled_signo == SIGUSR1 && handled_code == CLD_EXITED && handled_pid == child,
          "%s: signo %d, code %d, si_pid %d; want SIGUSR1, CLD_EXITED, %d", label,
          handled_signo, handled_code, handled_pid, (int) child);
    if (sizeof(long) == 8)
        check(handled_status == 9, "%s: si_status %d, want 9", label, handled_status);
    else
        check(handled_status == 9 || handled_status == 0,
              "%s: si_status %d, want 9 or 0", label, handled_status);
    expect_reaped(label, "waitpid(child, WNOHANG|__WCLONE)",
                  wait_for(child, WNOHANG | __WCLONE), child, 9);
}

static void option_masks(void) {
    const char *label = "options";
    expect_echild(label, "waitpid(-1, WNOHANG|__WNOTHREAD|__WCLONE|__WALL) with no children",
                  wait_for(-1, WNOHANG | __WNOTHREAD | __WCLONE | __WALL));
    expect_waitid_echild(label, "waitid(P_ALL, WEXITED|WNOHANG|__WNOTHREAD|__WCLONE|__WALL)",
                         P_ALL, 0, WEXITED | WNOHANG | __WNOTHREAD | __WCLONE | __WALL);
    /* The control: a bit that is no wait option is still refused. */
    struct waited w = wait_for(-1, WNOHANG | 0x10000000);
    check(w.rc == -1 && w.err == EINVAL, "%s: waitpid(-1, WNOHANG|0x10000000) = %d (%s), "
          "want EINVAL", label, (int) w.rc, w.rc < 0 ? strerror(w.err) : "-");
    siginfo_t si;
    int r = waitid_for(P_ALL, 0, WEXITED | WNOHANG | 0x10000000, &si);
    int err = errno;
    check(r == -1 && err == EINVAL, "%s: waitid(P_ALL, WEXITED|WNOHANG|0x10000000) = %d (%s), "
          "want EINVAL", label, r, r < 0 ? strerror(err) : "-");
}

/* ---- the harness ----------------------------------------------------------- */

/* Run a scenario in a process of its own, so that each starts with no children
 * and fresh signal state, and a hang in one cannot take the others with it. */
static void scenario(const char *name, void (*run)(void)) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        check(0, "%s: fork: %s", name, strerror(errno));
        return;
    }
    if (pid == 0) {
        failures_total = 0;
        alarm(test_watchdog_secs(30));
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
    /* The helper modes the exec scenarios run this program in. */
    if (argc == 3 && strcmp(argv[1], "--exit") == 0)
        _exit(atoi(argv[2]));

    test_init(argc, argv);
    ssize_t n = readlink("/proc/self/exe", self_exe, sizeof(self_exe) - 1);
    if (n <= 0) {
        check(0, "readlink(/proc/self/exe): %s", strerror(errno));
        return finish_suite("wait_clone_child");
    }
    self_exe[n] = '\0';

    scenario("clone child", clone_child_alone);
    scenario("running clone child", running_clone_child);
    scenario("forked child", forked_child);
    scenario("both kinds", both_kinds);
    scenario("no exit signal", no_exit_signal);
    scenario("exec by the leader", exec_by_leader);
    scenario("exec by a thread", exec_by_thread);
    scenario("CLONE_PARENT from a forked child", clone_parent_from_forked);
    scenario("CLONE_PARENT from a clone child", clone_parent_from_clone);
    scenario("__WNOTHREAD", wnothread);
    scenario("tracer, clone child", traced_clone_child);
    scenario("tracer, forked child", traced_forked_child);
    scenario("handler", handler_siginfo);
    scenario("options", option_masks);

    return finish_suite("wait_clone_child");
}
