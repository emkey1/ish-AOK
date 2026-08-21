// AB-BA deadlock between an epoll_wait scan over a pidfd and a task exiting.
//
// kernel/pidfd.c's pidfd_poll() takes pids_lock, and fs/poll.c's
// poll_scan_ready_locked() calls it while holding poll->lock. Meanwhile
// do_exit() holds pids_lock and called poll_wakeup() (via pidfd_notify_exit),
// which wants poll->lock. Opposite orders on the same pair:
//
//   epoll_wait scan:  poll->lock -> pids_lock
//   task exiting:     pids_lock  -> poll->lock
//
// That is exactly what fs/poll.h forbids -- "do not call poll_wakeup while
// holding a lock your poll operation acquires" -- and the same bug the
// signalfd path had with sighand->lock (see signalfd_epoll_deadlock.c).
//
// It does not wedge one process, it wedges the whole emulator: every thread
// that subsequently wants pids_lock (any fork, any exit, any /proc/<pid>
// lookup, the app's own UI thread via pid_get_task_ref) queues behind the
// holder forever. Found exactly that way -- the device seized during a
// regression run with ~30 threads stacked on pids_lock, and it was diagnosed
// from an lldb backtrace rather than from any test failing, because a test
// cannot report anything once the emulator stops scheduling.
//
// The window is small: the exiting task must call pidfd_notify_exit at the
// moment a sibling is inside poll_scan_ready_locked for a poll set containing
// a pidfd for it. So hammer it -- many rounds, several waiter threads per
// round so at least one is reliably mid-scan, and children that exit
// immediately. The watchdog is the verdict: on a build with the bug this test
// does not fail, it HANGS, and the alarm is what turns that into a failure.
//
// It has since caught the same cycle a SECOND time through a different edge,
// so do not read a hang here as "pidfd_notify_exit regressed". The exit path
// also raises SIGCHLD to the parent group, and that runs signalfd_wakeup_task
// (kernel/signal.c) with pids_lock held -- which trylocked the parent's fd
// table, and on failure released the table through fdtable_release(), which
// blocked on the very lock whose trylock had just failed. Three locks, one
// cycle: pids_lock -> files->lock -> poll->lock -> pids_lock. Fixed 2026-08-21
// by making fdtable_release() take files->lock only for the final teardown;
// see the comment on it in fs/fd.c. If this test hangs again, sample the
// process and read the cycle off the stacks rather than assuming which edge.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include "test_common.h"

#define ROUNDS 200
#define WAITERS 4

#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif

static int pidfd_open_(pid_t pid) {
    return (int) syscall(SYS_pidfd_open, pid, 0);
}

struct waiter_arg {
    int pidfd;
    int stop;
};

// Sit in epoll_wait over the pidfd. The point is not to observe the event but
// to be inside the scan -- holding poll->lock and reaching into pidfd_poll --
// while the child exits.
static void *waiter(void *p) {
    struct waiter_arg *arg = p;
    int ep = epoll_create1(0);
    if (ep < 0)
        return NULL;
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = arg->pidfd };
    if (epoll_ctl(ep, EPOLL_CTL_ADD, arg->pidfd, &ev) == 0) {
        struct epoll_event out[4];
        while (!__atomic_load_n(&arg->stop, __ATOMIC_RELAXED)) {
            // A short timeout keeps the thread cycling through the scan, so
            // the exit is far more likely to land inside one.
            if (epoll_wait(ep, out, 4, 2) > 0)
                break;
        }
    }
    close(ep);
    return NULL;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    // Generous: the failure mode is a hang, and on a loaded 4-way concurrent
    // sweep a correct run still finishes far inside this.
    alarm(test_watchdog_secs(120));

    int rounds_done = 0;
    for (int round = 0; round < ROUNDS; round++) {
        pid_t child = fork();
        if (child < 0) {
            printf("pidfd_epoll_deadlock: FAIL fork: %s\n", strerror(errno));
            return 1;
        }
        if (child == 0) {
            // Exit immediately: do_exit -> pidfd_notify_exit is the half of
            // the race we need, and we want it as soon as possible after the
            // waiters have armed.
            _exit(0);
        }

        int pidfd = pidfd_open_(child);
        if (pidfd < 0) {
            if (errno == ENOSYS || errno == EINVAL) {
                printf("pidfd_epoll_deadlock: SKIP (pidfd_open unavailable: %s)\n",
                       strerror(errno));
                waitpid(child, NULL, 0);
                return 0;
            }
            // The child may already be a zombie; that is fine and still worth
            // racing, but a hard error is not.
            printf("pidfd_epoll_deadlock: FAIL pidfd_open: %s\n", strerror(errno));
            waitpid(child, NULL, 0);
            return 1;
        }

        struct waiter_arg arg = { .pidfd = pidfd, .stop = 0 };
        pthread_t threads[WAITERS];
        int started = 0;
        for (int i = 0; i < WAITERS; i++) {
            if (pthread_create(&threads[i], NULL, waiter, &arg) == 0)
                started++;
        }

        // Reap while the waiters are scanning. This is the moment the two
        // lock orders can cross.
        waitpid(child, NULL, 0);

        __atomic_store_n(&arg.stop, 1, __ATOMIC_RELAXED);
        for (int i = 0; i < started; i++)
            pthread_join(threads[i], NULL);
        close(pidfd);
        rounds_done++;
    }

    // Reaching here at all is the result: a build with the deadlock stops
    // scheduling and the alarm fires instead.
    test_logf("  completed %d rounds with %d waiters each\n", rounds_done, WAITERS);
    if (rounds_done != ROUNDS) {
        printf("pidfd_epoll_deadlock: FAIL only %d/%d rounds\n", rounds_done, ROUNDS);
        failures_total++;
    }
    return finish_suite("pidfd_epoll_deadlock");
}
