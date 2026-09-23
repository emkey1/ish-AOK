/*
 * reparent_zombie_disposition.c -- a zombie handed to a new parent is
 * announced to it as its own exit would have been: a new parent whose SIGCHLD
 * is SIG_IGN or has SA_NOCLDWAIT has the zombie released at once, and one with
 * SIG_IGN is sent nothing at all.
 *
 * Linux's reparent_leader goes through do_notify_parent(p, p->exit_signal) for
 * every zombie the exit hands on, the same routine an ordinary exit uses. With
 * the new parent's SIGCHLD at SIG_IGN or with SA_NOCLDWAIT that routine
 * reports autoreap, and the zombie is released on the spot (EXIT_DEAD, then
 * release_task); with SIG_IGN it sends no signal -- not even one queued because
 * the new parent blocks SIGCHLD. SA_NOCLDWAIT alone still sends it.
 *
 * AOK's do_exit announced every reparented zombie with one SIGCHLD whatever
 * the new parent's disposition, and left the zombie for a wait that a parent
 * which disclaimed SIGCHLD will never make. exit_notify_process_locked had
 * asked both questions since f759cf0c; the reparent loop was the other caller
 * of Linux's do_notify_parent, and did not.
 *
 * Each scenario runs in a process of its own, S, that is a child subreaper
 * (PR_SET_CHILD_SUBREAPER) and blocks SIGCHLD. A middle process M forks one or
 * three grandchildren G, waits until each is a zombie, and exits without
 * reaping them, so they are reparented to S. Two shapes:
 *   - deep:   S -> A -> M -> G. A stays alive and reaps M, so the only thing
 *             S can hear about is the reparented G.
 *   - direct: S -> M -> G. M's own exit is announced to S as well, after G's
 *             (forget_original_parent runs before do_notify_parent for M), so
 *             the one SIGCHLD left pending names G.
 * And four dispositions of S's SIGCHLD: SIG_DFL (the control: SIGCHLD pending,
 * the zombie waitable), SIG_IGN (nothing pending, the zombie gone), a handler
 * with SA_NOCLDWAIT and SIG_DFL with SA_NOCLDWAIT (SIGCHLD pending, the zombie
 * gone). "Gone" is checked three ways: waitpid(G, WNOHANG) is ECHILD,
 * kill(G, 0) is ESRCH, and once every child has been dealt with waitpid(-1)
 * is ECHILD, so nothing was left behind.
 *
 * Checked against Linux 6.12 (Devuan 6, x86_64 and -m32, unprivileged).
 *
 * Exits 0 and prints "reparent_zombie_disposition: PASS" on success.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

#ifndef PR_SET_CHILD_SUBREAPER
#define PR_SET_CHILD_SUBREAPER 36
#endif

/* How long a notice that is due may take to arrive. */
#define ARRIVAL_MS 2000
/* How long to watch for one that is not due before calling it absent. */
#define SETTLE_MS 200
/* A child that is never released exits by itself after this, so that a case
 * that went wrong still ends. */
#define CHILD_LIMIT_MS 8000

#define MAX_ZOMBIES 3
/* Grandchild i exits with this. */
#define ZOMBIE_CODE(i) (7 + (i))

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

/* ---- the new parent's SIGCHLD -------------------------------------------- */

enum disposition {
    DEFAULT,            /* SIG_DFL */
    IGNORED,            /* SIG_IGN */
    NOCLDWAIT_HANDLER,  /* a handler with SA_NOCLDWAIT */
    NOCLDWAIT_DEFAULT,  /* SIG_DFL with SA_NOCLDWAIT */
};

static void on_chld(int sig) {
    (void) sig;
}

static void set_sigchld(enum disposition how) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    switch (how) {
        case DEFAULT:
            sa.sa_handler = SIG_DFL;
            break;
        case IGNORED:
            sa.sa_handler = SIG_IGN;
            break;
        case NOCLDWAIT_HANDLER:
            sa.sa_handler = on_chld;
            sa.sa_flags = SA_NOCLDWAIT;
            break;
        case NOCLDWAIT_DEFAULT:
            sa.sa_handler = SIG_DFL;
            sa.sa_flags = SA_NOCLDWAIT;
            break;
    }
    sigaction(SIGCHLD, &sa, NULL);
}

/* Whether the new parent is sent SIGCHLD, and whether it keeps the zombie. */
static bool is_signalled(enum disposition how) {
    return how != IGNORED;
}

static bool keeps_zombie(enum disposition how) {
    return how == DEFAULT;
}

static bool sigchld_pending(void) {
    sigset_t set;
    sigemptyset(&set);
    sigpending(&set);
    return sigismember(&set, SIGCHLD) == 1;
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

/* ---- the middle process ---------------------------------------------------- */

/* M: fork `n` grandchildren that exit at once, wait until every one is a
 * zombie, report their pids on `report`, and exit without reaping them. Its
 * SIGCHLD goes back to SIG_DFL first: inherited from a new parent with SIG_IGN
 * or SA_NOCLDWAIT, it would have them released the moment they exit, and
 * there would be no zombie to hand on. */
static void middle(int n, int report) {
    set_sigchld(DEFAULT);
    pid_t g[MAX_ZOMBIES];
    for (int i = 0; i < n; i++) {
        g[i] = fork();
        if (g[i] < 0)
            _exit(90);
        if (g[i] == 0)
            _exit(ZOMBIE_CODE(i));
    }
    for (int i = 0; i < n; i++) {
        siginfo_t si;
        int r;
        do {
            memset(&si, 0, sizeof(si));
            r = waitid(P_PID, (id_t) g[i], &si, WEXITED | WNOWAIT);
        } while (r < 0 && errno == EINTR);
        if (r != 0)
            _exit(91);
    }
    write_full(report, g, sizeof(g[0]) * (size_t) n);
    _exit(0);
}

/* ---- the new parent --------------------------------------------------------- */

enum shape {
    DEEP,       /* S -> A -> M -> G */
    DIRECT,     /* S -> M -> G */
};

struct scenario_args {
    const char *name;
    enum shape shape;
    int zombies;
    enum disposition how;
};

/* In the direct shape: until M is a zombie, or gone. */
static bool await_middle_done(const char *label, pid_t m) {
    long deadline = now_ms() + scaled_ms(CHILD_LIMIT_MS);
    for (;;) {
        siginfo_t si;
        memset(&si, 0, sizeof(si));
        int r = waitid(P_PID, (id_t) m, &si, WEXITED | WNOHANG | WNOWAIT);
        if (r == 0 && si.si_pid == m)
            return true;
        if (r < 0 && errno == ECHILD)
            return true;
        if (r < 0 && errno != EINTR) {
            check(0, "%s: waitid(M): %s", label, strerror(errno));
            return false;
        }
        if (now_ms() > deadline) {
            check(0, "%s: the middle process never exited", label);
            return false;
        }
        nap_ms(5);
    }
}

static const char *expected_notice(enum disposition how) {
    return is_signalled(how) ? "SIGCHLD pending" : "nothing pending";
}

static void run(const struct scenario_args *sc) {
    const char *label = sc->name;
    if (prctl(PR_SET_CHILD_SUBREAPER, 1L, 0L, 0L, 0L) != 0) {
        check(0, "%s: PR_SET_CHILD_SUBREAPER: %s", label, strerror(errno));
        return;
    }
    set_sigchld(sc->how);

    int report[2], done[2], release[2];
    if (!open_pipe(label, report) || !open_pipe(label, done) || !open_pipe(label, release))
        return;

    pid_t a = -1, m = -1;
    if (sc->shape == DEEP) {
        a = fork();
        if (a == 0) {
            /* A: not a subreaper, and with SIGCHLD at its default, so it waits
             * for M like any parent; G goes past it to S. */
            set_sigchld(DEFAULT);
            close(report[0]);
            close(done[0]);
            close(release[1]);
            pid_t mm = fork();
            if (mm == 0)
                middle(sc->zombies, report[1]);
            if (mm < 0)
                _exit(92);
            int st;
            while (waitpid(mm, &st, 0) < 0 && errno == EINTR)
                continue;
            write_full(done[1], "x", 1);
            await_byte(release[0]);
            _exit(0);
        }
        if (a < 0) {
            check(0, "%s: fork A: %s", label, strerror(errno));
            return;
        }
    } else {
        m = fork();
        if (m == 0) {
            close(report[0]);
            middle(sc->zombies, report[1]);
        }
        if (m < 0) {
            check(0, "%s: fork M: %s", label, strerror(errno));
            return;
        }
    }
    close(report[1]);
    close(done[1]);
    close(release[0]);

    pid_t g[MAX_ZOMBIES];
    if (!read_full(report[0], g, sizeof(g[0]) * (size_t) sc->zombies)) {
        check(0, "%s: the middle process reported no grandchildren", label);
        if (a > 0)
            kill(a, SIGKILL);
        return;
    }

    /* The reparent is done once M has exited: A has reaped it, or S sees it
     * a zombie or gone. */
    if (sc->shape == DEEP) {
        char c;
        if (!read_full(done[0], &c, 1))
            check(0, "%s: A never reaped the middle process", label);
    } else {
        await_middle_done(label, m);
    }

    /* What the new parent was sent. A notice that is due gets ARRIVAL_MS; one
     * that is not gets SETTLE_MS to turn up anyway. */
    nap_ms(scaled_ms(SETTLE_MS));
    if (is_signalled(sc->how)) {
        long deadline = now_ms() + scaled_ms(ARRIVAL_MS);
        while (!sigchld_pending() && now_ms() < deadline)
            nap_ms(5);
    }
    bool pending = sigchld_pending();
    check(pending == is_signalled(sc->how), "%s: sigpending after the reparent: SIGCHLD %s; "
          "want %s", label, pending ? "pending" : "not pending", expected_notice(sc->how));

    if (pending) {
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, SIGCHLD);
        struct timespec zero = {0, 0};
        siginfo_t si;
        memset(&si, 0, sizeof(si));
        int sig = sigtimedwait(&set, &si, &zero);
        /* The first zombie handed on is the one named, and the rest coalesced
         * into it -- the other two of three, and in the direct shape M's own
         * exit, told after them. It is the oldest: an exit hands its children
         * on oldest first (wait_child_order.c). */
        check(sig == SIGCHLD && si.si_code == CLD_EXITED && si.si_pid == g[0] &&
              si.si_status == ZOMBIE_CODE(0),
              "%s: the SIGCHLD is %d, si_code %d, si_pid %d, si_status %d; want SIGCHLD, "
              "CLD_EXITED, the oldest reparented grandchild %d, exit %d", label, sig,
              si.si_code, (int) si.si_pid, si.si_status, (int) g[0], ZOMBIE_CODE(0));

        nap_ms(scaled_ms(SETTLE_MS));
        check(!sigchld_pending(), "%s: another SIGCHLD after the first was taken", label);
    }

    for (int i = 0; i < sc->zombies; i++) {
        errno = 0;
        int k = kill(g[i], 0);
        int kerr = k < 0 ? errno : 0;
        if (keeps_zombie(sc->how))
            check(k == 0, "%s: kill(grandchild %d, 0) = %d (%s); want 0, a zombie", label,
                  (int) g[i], k, k < 0 ? strerror(kerr) : "-");
        else
            check(k < 0 && kerr == ESRCH, "%s: kill(grandchild %d, 0) = %d (%s); want ESRCH, "
                  "released", label, (int) g[i], k, k < 0 ? strerror(kerr) : "-");

        int st = -1;
        errno = 0;
        pid_t w = waitpid(g[i], &st, WNOHANG);
        int werr = w < 0 ? errno : 0;
        if (keeps_zombie(sc->how))
            check(w == g[i] && WIFEXITED(st) && WEXITSTATUS(st) == ZOMBIE_CODE(i),
                  "%s: waitpid(grandchild %d, WNOHANG) = %d (%s), status %#x; want it, exit %d",
                  label, (int) g[i], (int) w, w < 0 ? strerror(werr) : "-", st,
                  ZOMBIE_CODE(i));
        else
            check(w < 0 && werr == ECHILD,
                  "%s: waitpid(grandchild %d, WNOHANG) = %d (%s), status %#x; want ECHILD",
                  label, (int) g[i], (int) w, w < 0 ? strerror(werr) : "-", st);
    }

    /* The rest of the family: M in the direct shape, A in the deep one. Only
     * cleanup -- their own exits are sigchld_disposition.c's to check. */
    if (sc->shape == DIRECT) {
        int st;
        while (waitpid(m, &st, 0) < 0 && errno == EINTR)
            continue;
    } else {
        write_full(release[1], "x", 1);
        int st;
        while (waitpid(a, &st, 0) < 0 && errno == EINTR)
            continue;
    }
    errno = 0;
    pid_t left = waitpid(-1, NULL, WNOHANG);
    int lerr = left < 0 ? errno : 0;
    check(left < 0 && lerr == ECHILD, "%s: waitpid(-1, WNOHANG) at the end = %d (%s); want "
          "ECHILD, no child left", label, (int) left, left < 0 ? strerror(lerr) : "-");
}

/* ---- the harness ------------------------------------------------------------------ */

/* Run a scenario in a process of its own, so that each starts with no
 * children, its own SIGCHLD disposition and nothing pending, and a hang in one
 * cannot take the others with it. */
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
    {"deep, SIG_DFL", DEEP, 1, DEFAULT},
    {"deep, SIG_IGN", DEEP, 1, IGNORED},
    {"deep, SA_NOCLDWAIT handler", DEEP, 1, NOCLDWAIT_HANDLER},
    {"deep, SA_NOCLDWAIT with SIG_DFL", DEEP, 1, NOCLDWAIT_DEFAULT},
    {"direct, SIG_DFL", DIRECT, 1, DEFAULT},
    {"direct, SIG_IGN", DIRECT, 1, IGNORED},
    {"direct, SA_NOCLDWAIT handler", DIRECT, 1, NOCLDWAIT_HANDLER},
    {"direct, SA_NOCLDWAIT with SIG_DFL", DIRECT, 1, NOCLDWAIT_DEFAULT},
    {"three zombies, SIG_DFL", DEEP, 3, DEFAULT},
    {"three zombies, SIG_IGN", DEEP, 3, IGNORED},
    {"three zombies, SA_NOCLDWAIT handler", DEEP, 3, NOCLDWAIT_HANDLER},
    {"three zombies direct, SIG_IGN", DIRECT, 3, IGNORED},
};

int main(int argc, char **argv) {
    test_init(argc, argv);
    for (size_t i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++)
        scenario(&scenarios[i]);
    return finish_suite("reparent_zombie_disposition");
}
