// PR_SET_CHILD_SUBREAPER is per-process and does not survive fork.
//
// prctl(2): "The setting of the 'child subreaper' attribute is not inherited
// by children created by fork(2) and clone(2)." Linux's copy_signal() builds
// the child's signal_struct out of zeroed memory and propagates only the
// has_child_subreaper *hint*; is_child_subreaper is set by prctl and by
// nothing else.
//
// AOK's fork copied the whole struct tgroup, so the flag came along with it and
// EVERY descendant of a subreaper was a subreaper too. find_new_parent() then
// stopped its walk at the nearest ancestor instead of at the process that had
// actually asked to be the reaper -- so a service manager or container
// supervisor that set the flag to collect the orphans of its whole subtree got
// the orphans of its immediate children and nothing deeper. Everything below
// that was reaped one level down, by a process that never asked for it.
//
// Found while writing reparent_zombie_notify.c, whose middle process had to
// clear the flag by hand to keep the test saying what it meant.
//
// Two phases over the same shape, differing only in whether M asks:
//
//   T (this process)  -- runs no part of the test itself
//   `-- R             -- sets PR_SET_CHILD_SUBREAPER, and drives both phases
//       `-- M         -- phase 1: does NOT set it.  phase 2: sets it.
//           `-- C     -- exits immediately, orphaning O
//               `-- O -- reports the parent it lands on
//
// Phase 1 is the bug: O must land on R, walking past M. Phase 2 is the other
// half of the same fix -- a process that asks for the flag itself must still
// get it, so "never inherited" cannot be implemented by never honouring it.
// The exit status follows the same split: R can wait for O in phase 1 and must
// get ECHILD for it in phase 2.
//
// R is a forked child rather than this process because a test binary is pid 1
// under `ish -f <root> /bin/sh -c ...`, and init is where an orphan goes when
// there is no subreaper at all. Landing on pid 1 would then satisfy "landed on
// the subreaper" without the subreaper walk doing anything, so R is deliberately
// some other pid and every check below means what it says. R reports what it
// saw down a pipe; the assertions all run back here.
//
// Measured against x86_64 glibc on Linux 6.12.
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_common.h"

// Old libcs on some of the test roots predate these; the numbers are ABI.
#ifndef PR_SET_CHILD_SUBREAPER
#define PR_SET_CHILD_SUBREAPER 36
#endif
#ifndef PR_GET_CHILD_SUBREAPER
#define PR_GET_CHILD_SUBREAPER 37
#endif

#define ORPHAN_STATUS 41

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-58s got=%-10ld want=%ld\n", label, got, want);
}

// What one phase produced. m_flag is M's PR_GET_CHILD_SUBREAPER, read in M and
// carried into O across two forks; o_ppid is the parent O landed on once C's
// exit orphaned it; the wait_* fields are R's attempt to reap O.
struct phase {
    int built;
    int m_flag;
    int m_pid;
    int c_pid;
    int o_pid;
    int o_ppid;
    int timed_out;          // O gave up waiting for C to die
    int wait_rc;            // waitpid's return, or -1
    int wait_errno;
    int wait_exit;          // WEXITSTATUS, or -1 if it did not exit
};

struct results {
    int root_pid;
    int root_flag_set;      // PR_GET in R right after PR_SET 1
    int root_flag_cleared;   // ...and again after PR_SET 0
    struct phase p1, p2;
};

static int xfer(int fd, void *buf, size_t len, int writing) {
    char *p = buf;
    while (len > 0) {
        ssize_t n = writing ? write(fd, p, len) : read(fd, p, len);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        p += n;
        len -= (size_t) n;
    }
    return 0;
}

// The middle/orphan half, run in R. Leaves M ALIVE and returns its pid in
// out->m_pid: if M exited it would stop being a candidate reaper and phase 1
// would pass for the wrong reason.
static void run_phase(int middle_sets_flag, struct phase *out) {
    int pfd[2];
    if (pipe(pfd) != 0)
        return;

    pid_t m = fork();
    if (m < 0) {
        close(pfd[0]);
        close(pfd[1]);
        return;
    }

    if (m == 0) {
        close(pfd[0]);
        if (middle_sets_flag)
            prctl(PR_SET_CHILD_SUBREAPER, 1L, 0L, 0L, 0L);

        struct phase rep;
        memset(&rep, 0, sizeof rep);
        rep.built = 1;
        rep.m_pid = (int) getpid();
        rep.m_flag = -1;
        if (prctl(PR_GET_CHILD_SUBREAPER, &rep.m_flag, 0L, 0L, 0L) != 0)
            rep.m_flag = -2;

        pid_t c = fork();
        if (c < 0)
            _exit(3);
        if (c == 0) {
            // Recorded HERE, before O exists, so O can test its ppid against C
            // specifically. O's own first getppid() cannot serve as the
            // baseline: C exits the instant fork() returns to it, so on a fast
            // host the reparenting is already done before O looks, and waiting
            // for the value to CHANGE then waits for something that has
            // already happened. Real Linux lost that race every time; AOK won
            // it, which is exactly the kind of difference an oracle run is for.
            rep.c_pid = (int) getpid();
            pid_t o = fork();
            if (o < 0)
                _exit(4);
            if (o == 0) {
                // O. C's exit is what orphans it, so wait until C is no longer
                // the parent -- true immediately if it already is not.
                int i;
                for (i = 0; i < 4000; i++) {
                    if (getppid() != (pid_t) rep.c_pid)
                        break;
                    usleep(1000);
                }
                rep.timed_out = (i == 4000);
                rep.o_pid = (int) getpid();
                rep.o_ppid = (int) getppid();
                if (xfer(pfd[1], &rep, sizeof rep, 1) != 0)
                    _exit(5);
                _exit(ORPHAN_STATUS);
            }
            _exit(0);           // C: O is now an orphan
        }
        close(pfd[1]);          // leave only O holding the write end
        waitpid(c, NULL, 0);    // C is M's own child, and M's to reap
        for (;;)
            pause();            // stay a live reaper candidate until R kills us
    }

    close(pfd[1]);
    int rc = xfer(pfd[0], out, sizeof *out, 0);
    close(pfd[0]);
    if (rc != 0) {
        memset(out, 0, sizeof *out);
        kill(m, SIGKILL);
        waitpid(m, NULL, 0);
        return;
    }
    out->m_pid = (int) m;

    // Whether O is ours to reap is half the assertion: in phase 1 it is (R is
    // the subreaper it landed on), in phase 2 it is M's and this must be ECHILD.
    errno = 0;
    int status = 0;
    out->wait_rc = (int) waitpid((pid_t) out->o_pid, &status, 0);
    out->wait_errno = errno;
    out->wait_exit = (out->wait_rc > 0 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;

    kill(m, SIGKILL);
    waitpid(m, NULL, 0);
    // A zombie M was holding lands on US when it dies, since we are a
    // subreaper. Drain it so the next phase's waitpid sees only its own child;
    // retried because the handover happens inside M's exit and waitpid(m) can
    // return first. Purely hygiene -- nothing is asserted about what turns up.
    for (int tries = 0; tries < 40; tries++) {
        int drained = 0;
        while (waitpid(-1, NULL, WNOHANG) > 0)
            drained++;
        if (drained > 0)
            break;
        usleep(5000);
    }
}

// R: the process that actually asks to be a subreaper. Never pid 1.
static void run_root(int wfd) {
    struct results res;
    memset(&res, 0, sizeof res);
    res.root_pid = (int) getpid();
    res.root_flag_set = -1;
    res.root_flag_cleared = -1;

    alarm(test_watchdog_secs(90));

    if (prctl(PR_SET_CHILD_SUBREAPER, 1L, 0L, 0L, 0L) != 0)
        _exit(6);               // unsupported; the parent reports a skip
    if (prctl(PR_GET_CHILD_SUBREAPER, &res.root_flag_set, 0L, 0L, 0L) != 0)
        res.root_flag_set = -2;

    run_phase(0, &res.p1);
    run_phase(1, &res.p2);

    if (prctl(PR_SET_CHILD_SUBREAPER, 0L, 0L, 0L, 0L) == 0) {
        if (prctl(PR_GET_CHILD_SUBREAPER, &res.root_flag_cleared, 0L, 0L, 0L) != 0)
            res.root_flag_cleared = -2;
    }

    _exit(xfer(wfd, &res, sizeof res, 1) == 0 ? 0 : 7);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(120));

    int pfd[2];
    if (pipe(pfd) != 0) {
        printf("FAIL subreaper_not_inherited: pipe failed (%s)\n", strerror(errno));
        return finish_suite("subreaper_not_inherited");
    }

    pid_t root = fork();
    if (root < 0) {
        printf("FAIL subreaper_not_inherited: fork failed (%s)\n", strerror(errno));
        return finish_suite("subreaper_not_inherited");
    }
    if (root == 0) {
        close(pfd[0]);
        run_root(pfd[1]);
    }
    close(pfd[1]);

    struct results res;
    memset(&res, 0, sizeof res);
    int got_results = xfer(pfd[0], &res, sizeof res, 0) == 0;
    close(pfd[0]);

    int rstatus = 0;
    waitpid(root, &rstatus, 0);
    int rexit = WIFEXITED(rstatus) ? WEXITSTATUS(rstatus) : -1;

    if (!got_results) {
        if (rexit == 6) {
            test_logf("  no PR_SET_CHILD_SUBREAPER, skipped\n");
            return finish_suite("subreaper_not_inherited");
        }
        printf("FAIL subreaper_not_inherited: the subreaper root did not report "
               "(exit=%d, signal=%d)\n", rexit,
               WIFSIGNALED(rstatus) ? WTERMSIG(rstatus) : 0);
        return finish_suite("subreaper_not_inherited");
    }

    test_logf("  root=%d  phase1 m=%d c=%d o=%d  phase2 m=%d c=%d o=%d\n",
              res.root_pid, res.p1.m_pid, res.p1.c_pid, res.p1.o_pid,
              res.p2.m_pid, res.p2.c_pid, res.p2.o_pid);

    ck("PR_GET_CHILD_SUBREAPER reports what PR_SET stored", res.root_flag_set, 1);

    // Phase 1: M never asked. The flag must not have come along with the fork,
    // and the orphan must walk PAST M to the process that did ask.
    ck("the phase-1 process tree was built", res.p1.built, 1);
    ck("a fork does not inherit the subreaper flag", res.p1.m_flag, 0);
    ck("the orphan outlived the parent that orphaned it", res.p1.timed_out, 0);
    ck("an orphan does not stop at a non-subreaper ancestor",
       res.p1.o_ppid != res.p1.m_pid, 1);
    ck("it is reparented onto the subreaper that asked",
       res.p1.o_ppid, res.root_pid);
    ck("...which can then wait for it", res.p1.wait_rc == res.p1.o_pid, 1);
    ck("and collects its exit status", res.p1.wait_exit, ORPHAN_STATUS);

    // Phase 2: M asks for itself. "Not inherited" must not have become "never
    // honoured" -- the nearest subreaper is M now, and the orphan stops there.
    ck("the phase-2 process tree was built", res.p2.built, 1);
    ck("a process that sets the flag itself still gets it", res.p2.m_flag, 1);
    ck("the orphan outlived the parent that orphaned it (2)", res.p2.timed_out, 0);
    ck("and is reparented onto that nearer subreaper",
       res.p2.o_ppid, res.p2.m_pid);
    ck("so it is not the outer subreaper's to wait for",
       res.p2.wait_rc < 0 && res.p2.wait_errno == ECHILD, 1);

    ck("and the flag can be cleared again", res.root_flag_cleared, 0);

    return finish_suite("subreaper_not_inherited");
}
