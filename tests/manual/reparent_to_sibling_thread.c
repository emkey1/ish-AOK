// reparent_to_sibling_thread.c -- a child handed to another thread of the
// same process has not changed parents, so nobody is told that it did.
//
// When a thread exits, its children go to a thread of the same process that is
// staying (Linux's find_alive_thread, AOK's find_new_parent), and only when
// there is none to a subreaper or to init. AOK's do_exit treated the two alike,
// which is right only for the second:
//
//   - A child that was already a zombie was announced to its new parent with a
//     SIGCHLD. That is how init learns it has inherited a zombie, and it has to
//     stay (reparent_zombie_notify.c). But to a sibling thread nothing has
//     happened: its process is the child's parent before and after, and it was
//     told about the child's exit when the child exited. A second SIGCHLD runs
//     a handler for nothing and interrupts whatever the process was doing
//     without SA_RESTART.
//   - A child's exit_signal was reset to SIGCHLD, which Linux's reparent_leader
//     does so that nobody can make init receive some other signal. A process
//     that cloned a child with another exit_signal from a worker thread got
//     SIGCHLD for it instead, once that worker had exited.
//
// Linux's forget_original_parent calls reparent_leader, which does both, only
// when the new parent is in another thread group: "If this is a threaded
// reparent there is no need to notify anyone anything has happened."
//
// Measured before the fix, on alpine-amd64-test and devuan-amd64-test: main
// blocks SIGCHLD, thread X forks a child that exits at once, main takes the
// child's SIGCHLD, X exits without reaping it -- and SIGCHLD is pending again.
// The same when the leader is the thread that exits. Linux 6.12: nothing.
//
// The scenarios, each in a process of its own:
//   - a worker thread exits leaving a zombie child: nothing is announced, and
//     the process can still reap the child;
//   - the leader exits leaving a zombie child, and the thread that stays is
//     told nothing;
//   - a worker exits leaving a running child, forked or cloned with SIGUSR1:
//     nothing is announced then, and the child's own exit later is announced
//     once, with the signal it was created with;
//   - a worker exits leaving a zombie cloned with SIGUSR1: nothing either.
// And the controls, where the new parent IS another process -- this one, as a
// subreaper, inheriting a grandchild whose parent has exited:
//   - a zombie is announced with SIGCHLD, as init needs it to be; this is also
//     what shows the checks above can see a reparent SIGCHLD at all;
//   - a running child cloned with SIGUSR1 is announced with SIGCHLD when it
//     exits, its exit_signal reset.
//
// The threads exit with the raw exit syscall, having first pointed their tid
// word somewhere of this test's own: pthread_exit would do libc bookkeeping
// (musl's pthread_join never returns for a thread that skipped it), and the
// word is cleared, and a futex waiter woken, by the kernel as the thread goes.
// AOK clears it before it reparents, as Linux does, so every check allows
// SETTLE_MS on top for what the exit is still going to send.
//
// Checked against Linux 6.12 (Devuan 6, x86_64 and -m32, unprivileged).
//
// Exits 0 and prints "reparent_to_sibling_thread: PASS" on success.
#define _GNU_SOURCE

#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

// musl on i386 names the 32-bit-time calls for what they take.
#if !defined(SYS_futex) && defined(SYS_futex_time32)
#define SYS_futex SYS_futex_time32
#endif

#ifndef __WALL
#define __WALL 0x40000000
#endif

// What an exit that has cleared its tid word may still send: a reparent
// SIGCHLD went out well inside this before the fix.
#define SETTLE_MS 250
// How long a signal that is due may take to arrive.
#define ARRIVAL_MS 2000

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

// Scaled like the watchdogs, for a heavily loaded run. A longer wait only
// makes the "nothing arrives" checks stricter.
static long scaled_ms(long base) {
    return base * (long) test_watchdog_secs(1);
}

static void sleep_ms(long ms) {
    long deadline = now_ms() + ms;
    for (;;) {
        long left = deadline - now_ms();
        if (left <= 0)
            return;
        struct timespec ts = {.tv_sec = left / 1000, .tv_nsec = (left % 1000) * 1000000L};
        nanosleep(&ts, NULL);
    }
}

static void block_signals(void) {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigaddset(&mask, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &mask, NULL);
}

// Which of the two signals under test are pending, for the messages.
static const char *pending_names(void) {
    sigset_t set;
    sigemptyset(&set);
    sigpending(&set);
    bool chld = sigismember(&set, SIGCHLD), usr1 = sigismember(&set, SIGUSR1);
    return chld && usr1 ? "SIGCHLD SIGUSR1" : chld ? "SIGCHLD" : usr1 ? "SIGUSR1" : "none";
}

static bool nothing_pending(void) {
    return strcmp(pending_names(), "none") == 0;
}

// Take SIGCHLD or SIGUSR1, waiting up to ms. Returns the signal, or -1.
static int take_signal(siginfo_t *si, long ms) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGCHLD);
    sigaddset(&set, SIGUSR1);
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    memset(si, 0, sizeof(*si));
    return sigtimedwait(&set, si, &ts);
}

static const char *signame(int sig) {
    return sig == SIGCHLD ? "SIGCHLD" : sig == SIGUSR1 ? "SIGUSR1" : sig < 0 ? "nothing" : "?";
}

// A child created with some other exit_signal is a "clone child", which only
// __WALL (or __WCLONE) waits for.
static pid_t new_child(int exit_signal) {
    if (exit_signal == SIGCHLD)
        return fork();
    return (pid_t) syscall(SYS_clone, (long) exit_signal, 0L, 0L, 0L, 0L);
}

static int wait_flags(int exit_signal) {
    return exit_signal == SIGCHLD ? 0 : __WALL;
}

static void read_byte(int fd) {
    char c;
    while (read(fd, &c, 1) < 0 && errno == EINTR)
        continue;
}

static void write_byte(int fd) {
    if (write(fd, "x", 1) != 1)
        check(0, "pipe write: %s", strerror(errno));
}

// Exit this thread alone, with its tid word cleared and woken by the kernel.
static void thread_exit(volatile int *tid_word) {
    syscall(SYS_set_tid_address, tid_word);
    syscall(SYS_exit, 0);
}

// Shared, not private: the kernel's wake at thread exit is a shared one.
static void wait_thread_gone(volatile int *tid_word) {
    int v;
    while ((v = __atomic_load_n(tid_word, __ATOMIC_ACQUIRE)) != 0)
        syscall(SYS_futex, tid_word, FUTEX_WAIT, v, NULL, NULL, 0);
}

// The child is ours, and exited with `code`.
static void check_reaped(const char *label, pid_t child, int exit_signal, int code) {
    int status = 0;
    pid_t r = waitpid(child, &status, WNOHANG | wait_flags(exit_signal));
    check(r == child && WIFEXITED(status) && WEXITSTATUS(status) == code,
          "%s: waitpid(child) = %d (%s), status %#x, want the child, exited %d",
          label, (int) r, r < 0 ? strerror(errno) : "-", status, code);
}

// ---- a worker thread exits -------------------------------------------------

struct worker_case {
    const char *label;
    int exit_signal;        // the child's
    bool child_waits;       // running when the worker exits, or a zombie
    int code;               // the child's exit code
};

static struct {
    const struct worker_case *wc;
    int to_main[2];         // worker -> main: the child's pid
    int to_worker[2];       // main -> worker: exit now
    int release[2];         // main -> child: exit now
    volatile int tid_word;
} ws;

static void *worker_main(void *arg) {
    (void) arg;
    const struct worker_case *wc = ws.wc;
    pid_t child = new_child(wc->exit_signal);
    if (child == 0) {
        if (wc->child_waits)
            read_byte(ws.release[0]);
        _exit(wc->code);
    }
    if (write(ws.to_main[1], &child, sizeof(child)) != (ssize_t) sizeof(child))
        check(0, "%s: pipe write: %s", wc->label, strerror(errno));
    read_byte(ws.to_worker[0]);
    thread_exit(&ws.tid_word);
    return NULL;
}

static unsigned run_worker_case(const struct worker_case *wc) {
    failures_total = 0;     // forked from the harness, which has its own
    alarm(test_watchdog_secs(30));
    block_signals();        // before the worker exists, so it has them blocked too
    memset(&ws, 0, sizeof(ws));
    ws.wc = wc;
    ws.tid_word = 1;
    if (pipe(ws.to_main) != 0 || pipe(ws.to_worker) != 0 || pipe(ws.release) != 0) {
        check(0, "%s: pipe: %s", wc->label, strerror(errno));
        return failures_total;
    }
    pthread_t worker;
    if (pthread_create(&worker, NULL, worker_main, NULL) != 0) {
        check(0, "%s: pthread_create failed", wc->label);
        return failures_total;
    }
    pid_t child = -1;
    if (read(ws.to_main[0], &child, sizeof(child)) != (ssize_t) sizeof(child) || child <= 0) {
        check(0, "%s: the worker could not create a child", wc->label);
        return failures_total;
    }

    siginfo_t si;
    int sig;
    if (!wc->child_waits) {
        // The child's own exit, announced to this process: taking it is what
        // makes a SIGCHLD pending later mean something new.
        sig = take_signal(&si, scaled_ms(ARRIVAL_MS));
        check(sig == wc->exit_signal && si.si_pid == child,
              "%s: the child's exit is announced: got %s from %d, want %s from %d",
              wc->label, signame(sig), (int) si.si_pid, signame(wc->exit_signal), (int) child);
    }
    check(nothing_pending(), "%s: nothing else is pending before the worker exits (pending: %s)",
          wc->label, pending_names());

    write_byte(ws.to_worker[1]);
    wait_thread_gone(&ws.tid_word);
    sleep_ms(scaled_ms(SETTLE_MS));
    check(nothing_pending(), "%s: nothing is announced when the worker exits (pending: %s)",
          wc->label, pending_names());

    if (wc->child_waits) {
        // Its exit is still this process's to hear about -- once, and with the
        // signal it was created with.
        write_byte(ws.release[1]);
        sig = take_signal(&si, scaled_ms(ARRIVAL_MS));
        check(sig == wc->exit_signal && si.si_pid == child,
              "%s: the child's exit is announced: got %s from %d, want %s from %d",
              wc->label, signame(sig), (int) si.si_pid, signame(wc->exit_signal), (int) child);
        sleep_ms(scaled_ms(SETTLE_MS));
        check(nothing_pending(), "%s: and only once (pending: %s)", wc->label, pending_names());
    }
    check_reaped(wc->label, child, wc->exit_signal, wc->code);
    alarm(0);
    return failures_total;
}

// ---- the leader exits ------------------------------------------------------

static struct {
    int to_survivor[2];     // leader -> survivor: the child's pid, then gone
    volatile int leader_word;
} ls;

static void *survivor_main(void *arg) {
    (void) arg;
    const char *label = "leader exits, zombie child";
    pid_t child = -1;
    if (read(ls.to_survivor[0], &child, sizeof(child)) != (ssize_t) sizeof(child) || child <= 0)
        check(0, "%s: no child from the leader", label);
    wait_thread_gone(&ls.leader_word);
    sleep_ms(scaled_ms(SETTLE_MS));
    check(nothing_pending(), "%s: the thread that stays is told nothing (pending: %s)",
          label, pending_names());
    if (child > 0)
        check_reaped(label, child, SIGCHLD, 7);
    fflush(stdout);
    // The process's exit status: the leader has already gone.
    _exit(failures_total > 100 ? 100 : (int) failures_total);
    return NULL;
}

static unsigned run_leader_case(void) {
    const char *label = "leader exits, zombie child";
    failures_total = 0;
    alarm(test_watchdog_secs(30));
    block_signals();
    memset(&ls, 0, sizeof(ls));
    ls.leader_word = 1;
    if (pipe(ls.to_survivor) != 0) {
        check(0, "%s: pipe: %s", label, strerror(errno));
        return failures_total;
    }
    pthread_t survivor;
    if (pthread_create(&survivor, NULL, survivor_main, NULL) != 0) {
        check(0, "%s: pthread_create failed", label);
        return failures_total;
    }
    pid_t child = fork();
    if (child == 0)
        _exit(7);
    siginfo_t si;
    int sig = take_signal(&si, scaled_ms(ARRIVAL_MS));
    check(sig == SIGCHLD && si.si_pid == child,
          "%s: the child's exit is announced: got %s from %d, want SIGCHLD from %d",
          label, signame(sig), (int) si.si_pid, (int) child);
    check(nothing_pending(), "%s: nothing else is pending before the leader exits (pending: %s)",
          label, pending_names());
    if (write(ls.to_survivor[1], &child, sizeof(child)) != (ssize_t) sizeof(child))
        check(0, "%s: pipe write: %s", label, strerror(errno));
    fflush(stdout);
    thread_exit(&ls.leader_word);
    return failures_total;      // not reached
}

// ---- the controls: another process inherits the child ----------------------

struct inherit_case {
    const char *label;
    int exit_signal;
    bool child_waits;
    int code;
};

//   this process   -- a subreaper, so it is the one that inherits B
//   `-- A          -- alive throughout, and reaps C, so nothing is heard of C
//       `-- C      -- creates B, then exits
//           `-- B  -- a zombie by the time C exits, or waiting to be released
static unsigned run_inherit_case(const struct inherit_case *ic) {
    failures_total = 0;
    alarm(test_watchdog_secs(30));
    block_signals();
    if (prctl(PR_SET_CHILD_SUBREAPER, 1L, 0L, 0L, 0L) != 0) {
        check(0, "%s: PR_SET_CHILD_SUBREAPER: %s", ic->label, strerror(errno));
        return failures_total;
    }
    int to_me[2], c_gone[2], release[2];
    if (pipe(to_me) != 0 || pipe(c_gone) != 0 || pipe(release) != 0) {
        check(0, "%s: pipe: %s", ic->label, strerror(errno));
        return failures_total;
    }
    fflush(stdout);
    pid_t a = fork();
    if (a == 0) {
        pid_t c = fork();
        if (c == 0) {
            pid_t b = new_child(ic->exit_signal);
            if (b == 0) {
                if (ic->child_waits)
                    read_byte(release[0]);
                _exit(ic->code);
            }
            if (!ic->child_waits) {
                // Dead, and not reaped: WNOWAIT leaves the zombie.
                siginfo_t si;
                while (waitid(P_PID, (id_t) b, &si, WEXITED | WNOWAIT |
                              wait_flags(ic->exit_signal)) < 0 && errno == EINTR)
                    continue;
            }
            if (write(to_me[1], &b, sizeof(b)) != (ssize_t) sizeof(b))
                _exit(3);
            _exit(0);
        }
        while (waitpid(c, NULL, 0) < 0 && errno == EINTR)
            continue;
        if (write(c_gone[1], "x", 1) != 1)
            _exit(3);
        for (;;)
            pause();
    }
    pid_t b = -1;
    if (read(to_me[0], &b, sizeof(b)) != (ssize_t) sizeof(b) || b <= 0) {
        check(0, "%s: C did not report B", ic->label);
        kill(a, SIGKILL);
        waitpid(a, NULL, 0);
        return failures_total;
    }
    read_byte(c_gone[0]);

    siginfo_t si;
    int sig;
    if (!ic->child_waits) {
        long deadline = now_ms() + scaled_ms(ARRIVAL_MS);
        while (nothing_pending() && now_ms() < deadline)
            usleep(10000);
        sig = take_signal(&si, 0);
        check(sig == SIGCHLD && si.si_pid == b && si.si_status == ic->code,
              "%s: the inherited zombie is announced: got %s from %d status %d, "
              "want SIGCHLD from %d status %d", ic->label, signame(sig), (int) si.si_pid,
              si.si_status, (int) b, ic->code);
    } else {
        sleep_ms(scaled_ms(SETTLE_MS));
        check(nothing_pending(), "%s: nothing is announced while B runs (pending: %s)",
              ic->label, pending_names());
        write_byte(release[1]);
        sig = take_signal(&si, scaled_ms(ARRIVAL_MS));
        check(sig == SIGCHLD && si.si_pid == b,
              "%s: B's exit is announced with SIGCHLD: got %s from %d, want SIGCHLD from %d",
              ic->label, signame(sig), (int) si.si_pid, (int) b);
    }
    check_reaped(ic->label, b, ic->exit_signal, ic->code);

    kill(a, SIGKILL);
    while (waitpid(a, NULL, 0) < 0 && errno == EINTR)
        continue;
    prctl(PR_SET_CHILD_SUBREAPER, 0L, 0L, 0L, 0L);
    alarm(0);
    return failures_total;
}

// ---- the harness -----------------------------------------------------------

enum kind { WORKER, LEADER, INHERIT };

// Each scenario in a process of its own, so every one starts with fresh signal
// state and a hang or crash in one cannot take the others with it.
static void scenario(const char *label, enum kind kind, const void *arg) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        check(0, "%s: fork: %s", label, strerror(errno));
        return;
    }
    if (pid == 0) {
        unsigned failures = kind == WORKER ? run_worker_case(arg) :
                            kind == LEADER ? run_leader_case() : run_inherit_case(arg);
        fflush(stdout);
        _exit(failures > 100 ? 100 : (int) failures);
    }
    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        continue;
    if (WIFSIGNALED(status))
        check(0, "%s: killed by signal %d", label, WTERMSIG(status));
    else if (WEXITSTATUS(status) != 0)
        failures_total += (unsigned) WEXITSTATUS(status);
    else
        test_logf("ok %s\n", label);
}

int main(int argc, char **argv) {
    test_init(argc, argv);

    static const struct worker_case workers[] = {
        {"worker exits, zombie child", SIGCHLD, false, 7},
        {"worker exits, running child", SIGCHLD, true, 8},
        {"worker exits, running child cloned with SIGUSR1", SIGUSR1, true, 9},
        {"worker exits, zombie child cloned with SIGUSR1", SIGUSR1, false, 10},
    };
    for (size_t i = 0; i < sizeof(workers) / sizeof(workers[0]); i++)
        scenario(workers[i].label, WORKER, &workers[i]);

    scenario("leader exits, zombie child", LEADER, NULL);

    static const struct inherit_case inherits[] = {
        {"another process inherits a zombie", SIGCHLD, false, 7},
        {"another process inherits a child cloned with SIGUSR1", SIGUSR1, true, 9},
    };
    for (size_t i = 0; i < sizeof(inherits) / sizeof(inherits[0]); i++)
        scenario(inherits[i].label, INHERIT, &inherits[i]);

    return finish_suite("reparent_to_sibling_thread");
}
