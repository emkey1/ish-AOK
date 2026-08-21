// pidfd_open(2) on a task that has exited but not been reaped.
//
// A zombie is still a process. It holds its pid, wait() can still find it, and
// on Linux pidfd_open succeeds for one -- the fd is immediately readable,
// which is how a pidfd reports an exit at all. ESRCH is Linux's answer only
// for a pid that does not exist, meaning after reaping.
//
// AOK answered ESRCH for a zombie, because sys_pidfd_open went through
// pid_get_task_ref and pid_get_task filters zombies out by design. That made
// pidfd_open fail whenever the child won the race to exit -- which is exactly
// what pidfd_epoll_deadlock provokes on purpose, and why it failed about one
// run in three on x86_64 while looking like a flaky test.
//
// This one has no race in it. waitid(WNOWAIT) waits for the child to exit and
// leaves it unreaped, so by the time pidfd_open is called the task is
// certainly a zombie, and a build with the old behaviour fails every run.
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include "test_common.h"

#ifndef SYS_pidfd_open
# if defined(__aarch64__) || defined(__x86_64__) || defined(__i386__) || defined(__riscv)
#  define SYS_pidfd_open 434
# endif
#endif

// P_PIDFD is a kernel ABI constant, but musl only grew the idtype_t enumerator
// in 1.2.x -- the Alpine 3.11 i386 root the e2e suite builds against is still
// on 1.1.24, where this is a hard "'P_PIDFD' undeclared" error that stops
// setup-regressions.sh (set -e, builds everything before running anything) and
// so blocks the whole suite. Same fallback opath_symlink_pidfd_wait.c uses.
#ifndef P_PIDFD
#define P_PIDFD 3
#endif

#define CHILD_STATUS 42

static int pidfd_open_(pid_t pid) {
    return (int) syscall(SYS_pidfd_open, pid, 0);
}

static void fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("pidfd_zombie: FAIL ");
    vprintf(fmt, ap);
    va_end(ap);
    failures_total++;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(30));

    pid_t child = fork();
    if (child < 0) {
        fail("fork: %s\n", strerror(errno));
        return finish_suite("pidfd_zombie");
    }
    if (child == 0) {
        _exit(CHILD_STATUS);
    }

    // Wait for the exit WITHOUT reaping, so what follows is not a race: the
    // child is a zombie from here until the waitid below.
    siginfo_t info;
    memset(&info, 0, sizeof(info));
    if (waitid(P_PID, (id_t) child, &info, WEXITED | WNOWAIT) != 0) {
        fail("waitid(WNOWAIT): %s\n", strerror(errno));
        return finish_suite("pidfd_zombie");
    }
    test_logf("child %d is a zombie, exit status %d\n", child, info.si_status);

    int pidfd = pidfd_open_(child);
    if (pidfd < 0) {
        if (errno == ENOSYS || errno == EINVAL) {
            printf("pidfd_zombie: SKIP (pidfd_open unavailable: %s)\n", strerror(errno));
            waitid(P_PID, (id_t) child, &info, WEXITED);
            return finish_suite("pidfd_zombie");
        }
        // The regression: ESRCH for a pid that plainly exists.
        fail("pidfd_open on a zombie: %s (want success)\n", strerror(errno));
        waitid(P_PID, (id_t) child, &info, WEXITED);
        return finish_suite("pidfd_zombie");
    }
    test_logf("pidfd_open on a zombie -> fd %d ok\n", pidfd);

    // An exited task's pidfd is readable at once, with no waiting.
    struct pollfd pfd = { .fd = pidfd, .events = POLLIN };
    int ready = poll(&pfd, 1, 0);
    if (ready != 1 || !(pfd.revents & POLLIN)) {
        fail("poll of a zombie's pidfd -> %d revents=%#x (want 1, POLLIN)\n",
             ready, pfd.revents);
    } else {
        test_logf("poll -> POLLIN immediately ok\n");
    }

    // And it reaps through the fd, reporting what the child actually returned.
    memset(&info, 0, sizeof(info));
    if (waitid((idtype_t) P_PIDFD, (id_t) pidfd, &info, WEXITED) != 0) {
        // Not every build routes P_PIDFD; fall back so the reap still happens
        // and the ESRCH check below stays meaningful.
        test_logf("waitid(P_PIDFD): %s -- falling back to P_PID\n", strerror(errno));
        memset(&info, 0, sizeof(info));
        if (waitid(P_PID, (id_t) child, &info, WEXITED) != 0) {
            fail("waitid(P_PID): %s\n", strerror(errno));
        }
    }
    if (info.si_status != CHILD_STATUS) {
        fail("reaped status %d (want %d)\n", info.si_status, CHILD_STATUS);
    } else {
        test_logf("reaped through the pidfd, status %d ok\n", info.si_status);
    }
    close(pidfd);

    // Reaped now, so the pid really is gone and ESRCH is the right answer.
    int gone = pidfd_open_(child);
    if (gone >= 0) {
        fail("pidfd_open after reaping -> fd %d (want ESRCH)\n", gone);
        close(gone);
    } else if (errno != ESRCH) {
        fail("pidfd_open after reaping -> %s (want ESRCH)\n", strerror(errno));
    } else {
        test_logf("pidfd_open after reaping -> ESRCH ok\n");
    }

    return finish_suite("pidfd_zombie");
}
