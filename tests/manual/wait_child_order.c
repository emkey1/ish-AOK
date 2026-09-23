/*
 * wait_child_order.c -- wait(-1) finds a process's children oldest first, and
 * children handed to a new parent join the end of its list, in their order.
 *
 * Linux keeps each task's children in creation order: copy_process links a
 * new child with list_add_tail, CLONE_PARENT included, and de_thread puts an
 * exec'ing thread where its old leader was (list_replace_init). An exit hands
 * its children on with list_splice_tail_init, after the new parent's own and
 * in the order they were in, telling the new parent of each zombie in that
 * order (reparent_leader). do_wait walks the list from the front, so of
 * several zombies wait(-1) reaps the oldest, and the SIGCHLD a new parent that
 * blocks it finds -- the first of the reparent's, the rest coalescing into it
 * -- names the oldest orphan.
 *
 * AOK linked every child at the head instead: fork, CLONE_PARENT, exec's
 * de-thread and the reparent loop. Measured 2026-09-23 against Linux 6.12,
 * alpine-amd64-test: three children exited and waited for, Linux reaps 1 2 3
 * and AOK 3 2 1; and of three zombies reparented together, the pending
 * SIGCHLD named the youngest.
 *
 * The scenarios, each in a process of its own that blocks SIGCHLD:
 *   - four children exit; reaped with waitpid(-1), waitpid(0) and
 *     waitid(P_ALL), fork order each time, and a WNOWAIT peek first names
 *     the oldest;
 *   - a child clones a sibling with CLONE_PARENT: the sibling comes after it
 *     and before a child forked later;
 *   - the middle one of three children execs from a thread: it keeps its
 *     place;
 *   - a subreaper with a zombie child, a middle process and a second zombie
 *     child forked after the middle one's children; the middle one exits and
 *     its three zombies come after all three, oldest first, and the SIGCHLD
 *     left pending names the oldest. Twice: with the middle process the
 *     subreaper's child, and a generation further down;
 *   - a worker thread forks two children and exits after the main thread has
 *     forked one of its own: the main thread reaps its own first and then the
 *     worker's, which is not pid order.
 * Where /proc/<pid>/task/<tid>/children exists, it lists the same order.
 *
 * Checked against Linux 6.12 (Devuan 6, x86_64 and -m32, unprivileged).
 *
 * Exits 0 and prints "wait_child_order: PASS" on success.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

#ifndef PR_SET_CHILD_SUBREAPER
#define PR_SET_CHILD_SUBREAPER 36
#endif
#ifndef CLONE_PARENT
#define CLONE_PARENT 0x00008000
#endif

/* How long a notice that is due may take to arrive. */
#define ARRIVAL_MS 2000
/* How long to let an exit finish what it is still sending. */
#define SETTLE_MS 200
/* A child that is never released exits by itself after this, so that a case
 * that went wrong still ends. */
#define CHILD_LIMIT_MS 8000

#define MAX_KIDS 8

/* This program, for the child that execs from a thread. */
static char self_exe[PATH_MAX];

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

/* ---- pipes ----------------------------------------------------------------- */

static bool open_pipe(const char *label, int fds[2]) {
    if (pipe(fds) == 0)
        return true;
    check(0, "%s: pipe: %s", label, strerror(errno));
    return false;
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

static void write_full(int fd, const void *buf, size_t len) {
    size_t put = 0;
    while (put < len) {
        ssize_t r = write(fd, (const char *) buf + put, len - put);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return;
        put += (size_t) r;
    }
}

/* For a child: wait for a byte on `fd`; EOF, or CHILD_LIMIT_MS, lets it go too. */
static void await_byte(int fd) {
    alarm((unsigned) (scaled_ms(CHILD_LIMIT_MS) / 1000));
    char c;
    while (read(fd, &c, 1) < 0 && errno == EINTR)
        continue;
    alarm(0);
}

/* ---- the children ------------------------------------------------------------ */

struct kid {
    const char *name;
    pid_t pid;
    int code;               /* its exit status */
};

static const char *kid_name(const struct kid *kids, int n, pid_t pid) {
    for (int i = 0; i < n; i++) {
        if (kids[i].pid == pid)
            return kids[i].name;
    }
    return "?";
}

/* "E1(101) M(102) ..." */
static void describe(char *buf, size_t len, const struct kid *kids, int n,
                     const pid_t *pids, int count) {
    size_t used = 0;
    buf[0] = '\0';
    for (int i = 0; i < count && used < len; i++) {
        int w = snprintf(buf + used, len - used, "%s%s(%d)", i ? " " : "",
                         kid_name(kids, n, pids[i]), (int) pids[i]);
        if (w < 0)
            break;
        used += (size_t) w;
    }
}

static void describe_kids(char *buf, size_t len, const struct kid *kids, int n) {
    pid_t pids[MAX_KIDS];
    for (int i = 0; i < n; i++)
        pids[i] = kids[i].pid;
    describe(buf, len, kids, n, pids, n);
}

/* Until `pid` is a zombie, without reaping it. */
static bool await_zombie(const char *label, const char *name, pid_t pid) {
    siginfo_t si;
    int r;
    do {
        memset(&si, 0, sizeof(si));
        r = waitid(P_PID, (id_t) pid, &si, WEXITED | WNOWAIT);
    } while (r < 0 && errno == EINTR);
    check(r == 0 && si.si_pid == pid, "%s: waitid(%s %d, WNOWAIT) = %d (%s), si_pid %d; "
          "want it, a zombie", label, name, (int) pid, r, r < 0 ? strerror(errno) : "-",
          (int) si.si_pid);
    return r == 0 && si.si_pid == pid;
}

static bool await_zombies(const char *label, const struct kid *kids, int n) {
    for (int i = 0; i < n; i++) {
        if (!await_zombie(label, kids[i].name, kids[i].pid))
            return false;
    }
    return true;
}

/* Take every SIGCHLD pending now, and whatever an exit still sends after it. */
static void drain_sigchld(void) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGCHLD);
    struct timespec zero = {0, 0};
    for (int round = 0; round < 2; round++) {
        while (sigtimedwait(&set, NULL, &zero) == SIGCHLD)
            continue;
        if (round == 0)
            nap_ms(scaled_ms(SETTLE_MS));
    }
}

/* /proc/<pid>/task/<tid>/children, where the kernel has it: the same list
 * wait walks. Linux without CONFIG_PROC_CHILDREN, and AOK, have no such file,
 * and then there is nothing to check. */
static void check_proc_children(const char *label, const struct kid *want, int n) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/task/%d/children", (int) getpid(),
             (int) syscall(SYS_gettid));
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        test_logf("-- %s: no %s (%s); not checked\n", label, path, strerror(errno));
        return;
    }
    pid_t got[MAX_KIDS + 4];
    int count = 0;
    int v;
    while (count < (int) (sizeof(got) / sizeof(got[0])) && fscanf(f, "%d", &v) == 1)
        got[count++] = (pid_t) v;
    fclose(f);
    bool same = count == n;
    for (int i = 0; same && i < n; i++)
        same = got[i] == want[i].pid;
    char g[256], w[256];
    describe(g, sizeof(g), want, n, got, count);
    describe_kids(w, sizeof(w), want, n);
    check(same, "%s: %s lists %s; want %s", label, path, g, w);
}

enum wait_call {
    REAP_ANY,       /* waitpid(-1) */
    REAP_PGRP,      /* waitpid(0) */
    REAP_WAITID,     /* waitid(P_ALL) */
};

static pid_t wait_one(enum wait_call how, int *status) {
    for (;;) {
        pid_t r;
        if (how == REAP_WAITID) {
            siginfo_t si;
            memset(&si, 0, sizeof(si));
            r = waitid(P_ALL, 0, &si, WEXITED) == 0 ? si.si_pid : -1;
            *status = si.si_code == CLD_EXITED ? (si.si_status & 0xff) << 8 : -1;
        } else {
            r = waitpid(how == REAP_PGRP ? 0 : -1, status, 0);
        }
        if (r < 0 && errno == EINTR)
            continue;
        return r;
    }
}

static const char *wait_call_name(enum wait_call how) {
    return how == REAP_PGRP ? "waitpid(0)" : how == REAP_WAITID ? "waitid(P_ALL)" : "waitpid(-1)";
}

/* Reap `n` children with `how`: they must come in the order of `want`, each
 * with its own status. */
static void check_reap_order(const char *label, enum wait_call how,
                             const struct kid *want, int n) {
    pid_t got[MAX_KIDS];
    int status[MAX_KIDS];
    int count = 0;
    for (; count < n; count++) {
        got[count] = wait_one(how, &status[count]);
        if (got[count] < 0) {
            check(0, "%s: %s #%d: %s", label, wait_call_name(how), count + 1, strerror(errno));
            break;
        }
    }
    bool same = count == n;
    for (int i = 0; same && i < n; i++)
        same = got[i] == want[i].pid;
    char g[256], w[256];
    describe(g, sizeof(g), want, n, got, count);
    describe_kids(w, sizeof(w), want, n);
    check(same, "%s: %s reaps %s; want %s", label, wait_call_name(how), g, w);
    for (int i = 0; i < count; i++) {
        for (int j = 0; j < n; j++) {
            if (want[j].pid == got[i])
                check(WIFEXITED(status[i]) && WEXITSTATUS(status[i]) == want[j].code,
                      "%s: %s %d status %#x; want exit %d", label, want[j].name,
                      (int) got[i], status[i], want[j].code);
        }
    }
}

/* Nothing left: no child at all. */
static void check_no_children(const char *label) {
    errno = 0;
    pid_t left = waitpid(-1, NULL, WNOHANG);
    int lerr = left < 0 ? errno : 0;
    check(left < 0 && lerr == ECHILD, "%s: waitpid(-1, WNOHANG) at the end = %d (%s); want "
          "ECHILD, no child left", label, (int) left, left < 0 ? strerror(lerr) : "-");
}

/* The SIGCHLD pending now, taken: it names `want`. */
static void check_pending_names(const char *label, const struct kid *want) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGCHLD);
    long ms = scaled_ms(ARRIVAL_MS);
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    siginfo_t si;
    memset(&si, 0, sizeof(si));
    int sig;
    do
        sig = sigtimedwait(&set, &si, &ts);
    while (sig < 0 && errno == EINTR);
    check(sig == SIGCHLD && si.si_code == CLD_EXITED && si.si_pid == want->pid &&
          si.si_status == want->code,
          "%s: the pending SIGCHLD is %d, si_code %d, si_pid %d, si_status %d; want "
          "SIGCHLD, CLD_EXITED, the oldest orphan %s (%d), exit %d", label, sig, si.si_code,
          (int) si.si_pid, si.si_status, want->name, (int) want->pid, want->code);
}

/* A child that exits at once with `code`. */
static pid_t spawn_exit(const char *label, int code) {
    pid_t p = fork();
    if (p == 0)
        _exit(code);
    if (p < 0)
        check(0, "%s: fork: %s", label, strerror(errno));
    return p;
}

/* ---- fork order ---------------------------------------------------------------- */

static void fork_order(const char *label, enum wait_call how) {
    struct kid kids[] = {{"C1", 0, 11}, {"C2", 0, 12}, {"C3", 0, 13}, {"C4", 0, 14}};
    int n = (int) (sizeof(kids) / sizeof(kids[0]));
    for (int i = 0; i < n; i++) {
        if ((kids[i].pid = spawn_exit(label, kids[i].code)) < 0)
            return;
    }
    if (!await_zombies(label, kids, n))
        return;
    check_proc_children(label, kids, n);

    /* A look without reaping finds the one a wait would reap. */
    siginfo_t si;
    memset(&si, 0, sizeof(si));
    int r = waitid(P_ALL, 0, &si, WEXITED | WNOWAIT);
    check(r == 0 && si.si_pid == kids[0].pid, "%s: waitid(P_ALL, WNOWAIT) names %s (%d); "
          "want C1 (%d)", label, r == 0 ? kid_name(kids, n, si.si_pid) : strerror(errno),
          (int) si.si_pid, (int) kids[0].pid);

    check_reap_order(label, how, kids, n);
    check_no_children(label);
}

/* ---- CLONE_PARENT ------------------------------------------------------------- */

static void clone_parent(const char *label) {
    struct kid kids[] = {{"A", 0, 21}, {"X (A's CLONE_PARENT sibling)", 0, 22}, {"B", 0, 23}};
    int report[2];
    if (!open_pipe(label, report))
        return;
    kids[0].pid = fork();
    if (kids[0].pid == 0) {
        close(report[0]);
        pid_t x = (pid_t) syscall(SYS_clone, (long) (CLONE_PARENT | SIGCHLD), 0L, 0L, 0L, 0L);
        if (x == 0)
            _exit(22);
        write_full(report[1], &x, sizeof(x));
        _exit(21);
    }
    if (kids[0].pid < 0) {
        check(0, "%s: fork: %s", label, strerror(errno));
        return;
    }
    close(report[1]);
    if (!read_full(report[0], &kids[1].pid, sizeof(kids[1].pid)) || kids[1].pid <= 0) {
        check(0, "%s: A reported no CLONE_PARENT child (%d)", label, (int) kids[1].pid);
        return;
    }
    close(report[0]);
    if ((kids[2].pid = spawn_exit(label, 23)) < 0)
        return;
    if (!await_zombies(label, kids, 3))
        return;
    check_proc_children(label, kids, 3);
    check_reap_order(label, REAP_ANY, kids, 3);
    check_no_children(label);
}

/* ---- exec from a thread ------------------------------------------------------ */

static void *exec_thread(void *arg) {
    (void) arg;
    char *argv[] = {self_exe, "exec-exit", "32", NULL};
    execv(self_exe, argv);
    _exit(90);
}

static void thread_exec(const char *label) {
    struct kid kids[] = {{"C1", 0, 31}, {"C2 (execs from a thread)", 0, 32}, {"C3", 0, 33}};
    if ((kids[0].pid = spawn_exit(label, 31)) < 0)
        return;
    kids[1].pid = fork();
    if (kids[1].pid == 0) {
        pthread_t t;
        if (pthread_create(&t, NULL, exec_thread, NULL) != 0)
            _exit(91);
        /* The exec takes this thread away; it never returns. */
        for (;;)
            pause();
    }
    if (kids[1].pid < 0) {
        check(0, "%s: fork: %s", label, strerror(errno));
        return;
    }
    if ((kids[2].pid = spawn_exit(label, 33)) < 0)
        return;
    if (!await_zombies(label, kids, 3))
        return;
    check_proc_children(label, kids, 3);
    check_reap_order(label, REAP_ANY, kids, 3);
    check_no_children(label);
}

/* ---- reparent to a subreaper --------------------------------------------------- */

/* M: fork three children that exit at once, wait until each is a zombie,
 * report their pids, and exit without reaping them once told to. */
static void middle(int report, int go) {
    pid_t g[3];
    for (int i = 0; i < 3; i++) {
        g[i] = fork();
        if (g[i] < 0)
            _exit(92);
        if (g[i] == 0)
            _exit(41 + i);
    }
    for (int i = 0; i < 3; i++) {
        siginfo_t si;
        int r;
        do {
            memset(&si, 0, sizeof(si));
            r = waitid(P_PID, (id_t) g[i], &si, WEXITED | WNOWAIT);
        } while (r < 0 && errno == EINTR);
        if (r != 0)
            _exit(93);
    }
    write_full(report, g, sizeof(g));
    await_byte(go);
    _exit(40);
}

enum shape {
    DIRECT,     /* S -> M -> G */
    DEEP,       /* S -> A -> M -> G */
};

static void subreaper(const char *label, enum shape shape) {
    if (prctl(PR_SET_CHILD_SUBREAPER, 1L, 0L, 0L, 0L) != 0) {
        check(0, "%s: PR_SET_CHILD_SUBREAPER: %s", label, strerror(errno));
        return;
    }
    /* In the order they are on the list at the end: E1, then M (or A, which
     * stays running), then E2, forked after M's children, then those. */
    struct kid kids[] = {
        {"E1", 0, 51}, {shape == DIRECT ? "M" : "A", 0, 40}, {"E2", 0, 52},
        {"G1", 0, 41}, {"G2", 0, 42}, {"G3", 0, 43},
    };
    int report[2], go[2], done[2], release[2];
    if (!open_pipe(label, report) || !open_pipe(label, go) || !open_pipe(label, done) ||
            !open_pipe(label, release))
        return;

    if ((kids[0].pid = spawn_exit(label, 51)) < 0)
        return;
    kids[1].pid = fork();
    if (kids[1].pid == 0) {
        close(report[0]);
        close(go[1]);
        close(done[0]);
        close(release[1]);
        if (shape == DIRECT)
            middle(report[1], go[0]);
        /* A: not a subreaper, so M's children go past it to S. It reaps M
         * like any parent, and stays until released. */
        pid_t m = fork();
        if (m == 0)
            middle(report[1], go[0]);
        if (m < 0)
            _exit(94);
        int st;
        while (waitpid(m, &st, 0) < 0 && errno == EINTR)
            continue;
        write_full(done[1], "x", 1);
        await_byte(release[0]);
        _exit(40);
    }
    if (kids[1].pid < 0) {
        check(0, "%s: fork: %s", label, strerror(errno));
        return;
    }
    close(report[1]);
    close(go[0]);
    close(done[1]);
    close(release[0]);

    pid_t g[3];
    if (!read_full(report[0], g, sizeof(g))) {
        check(0, "%s: the middle process reported no children", label);
        kill(kids[1].pid, SIGKILL);
        return;
    }
    for (int i = 0; i < 3; i++)
        kids[3 + i].pid = g[i];
    /* Younger than M's children, and still ahead of them once they are ours. */
    if ((kids[2].pid = spawn_exit(label, 52)) < 0)
        return;
    if (!await_zombie(label, "E1", kids[0].pid) || !await_zombie(label, "E2", kids[2].pid))
        return;
    /* Nothing pending but what the reparent sends. */
    drain_sigchld();

    write_full(go[1], "x", 1);
    if (shape == DIRECT) {
        if (!await_zombie(label, "M", kids[1].pid))
            return;
    } else {
        char c;
        if (!read_full(done[0], &c, 1)) {
            check(0, "%s: A never reaped the middle process", label);
            return;
        }
    }
    nap_ms(scaled_ms(SETTLE_MS));

    /* One SIGCHLD for each orphan, oldest first, and M's own after them: the
     * rest coalesce into the first. */
    check_pending_names(label, &kids[3]);
    check_proc_children(label, kids, 6);

    if (shape == DIRECT) {
        check_reap_order(label, REAP_ANY, kids, 6);
    } else {
        /* A is still running: everything else, in order, and then A. */
        struct kid zombies[] = {kids[0], kids[2], kids[3], kids[4], kids[5]};
        check_reap_order(label, REAP_ANY, zombies, 5);
        write_full(release[1], "x", 1);
        int st;
        pid_t w;
        while ((w = waitpid(kids[1].pid, &st, 0)) < 0 && errno == EINTR)
            continue;
        check(w == kids[1].pid, "%s: waitpid(A) = %d", label, (int) w);
    }
    check_no_children(label);
}

/* ---- a thread's children, handed to the thread that stays ---------------------- */

struct worker_args {
    const char *label;
    pid_t kids[2];
    pid_t tid;
    int ready;          /* write end: the worker's children are zombies */
    int go;             /* read end: exit now */
};

static void *worker(void *arg) {
    struct worker_args *w = arg;
    w->tid = (pid_t) syscall(SYS_gettid);
    for (int i = 0; i < 2; i++) {
        w->kids[i] = fork();
        if (w->kids[i] == 0)
            _exit(62 + i);
    }
    for (int i = 0; i < 2; i++) {
        siginfo_t si;
        while (waitid(P_PID, (id_t) w->kids[i], &si, WEXITED | WNOWAIT) < 0 && errno == EINTR)
            continue;
    }
    write_full(w->ready, "x", 1);
    /* Not await_byte: its alarm is the process's, and would cancel the
     * scenario's watchdog. */
    char c;
    while (read(w->go, &c, 1) < 0 && errno == EINTR)
        continue;
    return NULL;
}

static void thread_children(const char *label) {
    /* Main's own child first, then the worker's two, though those are older:
     * the worker's are appended to main's list when it exits. */
    struct kid kids[] = {{"C1 (main thread's)", 0, 61}, {"W1 (worker's)", 0, 62},
                         {"W2 (worker's)", 0, 63}};
    int ready[2], go[2];
    if (!open_pipe(label, ready) || !open_pipe(label, go))
        return;
    struct worker_args w = {.label = label, .ready = ready[1], .go = go[0]};
    pthread_t t;
    if (pthread_create(&t, NULL, worker, &w) != 0) {
        check(0, "%s: pthread_create failed", label);
        return;
    }
    char c;
    if (!read_full(ready[0], &c, 1)) {
        check(0, "%s: the worker never reported", label);
        return;
    }
    kids[1].pid = w.kids[0];
    kids[2].pid = w.kids[1];
    if ((kids[0].pid = spawn_exit(label, 61)) < 0)
        return;
    if (!await_zombie(label, kids[0].name, kids[0].pid))
        return;
    write_full(go[1], "x", 1);
    pthread_join(t, NULL);

    /* The join returns once the worker's tid word is cleared, which is before
     * its exit hands its children on; its task entry goes after. */
    char path[64];
    snprintf(path, sizeof(path), "/proc/self/task/%d", (int) w.tid);
    long deadline = now_ms() + scaled_ms(CHILD_LIMIT_MS);
    struct stat sb;
    while (stat(path, &sb) == 0 && now_ms() < deadline)
        nap_ms(5);
    bool gone = stat(path, &sb) < 0;
    check(gone, "%s: %s %s after the worker exited; want it gone", label, path,
          gone ? "gone" : "still there");
    nap_ms(scaled_ms(SETTLE_MS));

    check_proc_children(label, kids, 3);
    check_reap_order(label, REAP_ANY, kids, 3);
    check_no_children(label);
}

/* ---- the harness ------------------------------------------------------------------ */

enum kind {
    FORK_ORDER,
    CLONE_PARENT_SIBLING,
    THREAD_EXEC,
    SUBREAPER,
    THREAD_CHILDREN,
};

struct scenario_args {
    const char *name;
    enum kind kind;
    int arg;
};

static void run(const struct scenario_args *sc) {
    switch (sc->kind) {
        case FORK_ORDER:
            fork_order(sc->name, (enum wait_call) sc->arg);
            break;
        case CLONE_PARENT_SIBLING:
            clone_parent(sc->name);
            break;
        case THREAD_EXEC:
            thread_exec(sc->name);
            break;
        case SUBREAPER:
            subreaper(sc->name, (enum shape) sc->arg);
            break;
        case THREAD_CHILDREN:
            thread_children(sc->name);
            break;
    }
}

/* Run a scenario in a process of its own, so that each starts with no
 * children and nothing pending, and a hang in one cannot take the others
 * with it. */
static void scenario(const struct scenario_args *sc) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        check(0, "%s: fork: %s", sc->name, strerror(errno));
        return;
    }
    if (pid == 0) {
        failures_total = 0;
        alarm(test_watchdog_secs(60));
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGCHLD);
        sigprocmask(SIG_BLOCK, &mask, NULL);
        run(sc);
        fflush(stdout);
        _exit(failures_total > 100 ? 100 : (int) failures_total);
    }
    int status;
    pid_t w;
    while ((w = waitpid(pid, &status, 0)) < 0 && errno == EINTR)
        continue;
    if (w != pid)
        check(0, "%s: waitpid: %s", sc->name, strerror(errno));
    else if (WIFSIGNALED(status))
        check(0, "%s: killed by signal %d", sc->name, WTERMSIG(status));
    else if (WEXITSTATUS(status) != 0)
        failures_total += (unsigned) WEXITSTATUS(status);
    else
        test_logf("ok %s\n", sc->name);
}

static const struct scenario_args scenarios[] = {
    {"fork order, waitpid(-1)", FORK_ORDER, REAP_ANY},
    {"fork order, waitpid(0)", FORK_ORDER, REAP_PGRP},
    {"fork order, waitid(P_ALL)", FORK_ORDER, REAP_WAITID},
    {"CLONE_PARENT sibling", CLONE_PARENT_SIBLING, 0},
    {"exec from a thread keeps its place", THREAD_EXEC, 0},
    {"reparent to a subreaper, direct", SUBREAPER, DIRECT},
    {"reparent to a subreaper, deep", SUBREAPER, DEEP},
    {"a worker thread's children go to the main thread", THREAD_CHILDREN, 0},
};

int main(int argc, char **argv) {
    /* The image the thread_exec scenario's thread runs. */
    if (argc == 3 && strcmp(argv[1], "exec-exit") == 0)
        _exit(atoi(argv[2]));
    test_init(argc, argv);

    ssize_t len = readlink("/proc/self/exe", self_exe, sizeof(self_exe) - 1);
    if (len > 0)
        self_exe[len] = '\0';
    else
        snprintf(self_exe, sizeof(self_exe), "%s", "/proc/self/exe");

    for (size_t i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++)
        scenario(&scenarios[i]);
    return finish_suite("wait_child_order");
}
