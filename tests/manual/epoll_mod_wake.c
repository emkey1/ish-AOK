// epoll_mod_wake: arming an already-ready emulated fd (eventfd) via
// EPOLL_CTL_MOD must wake a thread already blocked in epoll_wait on that set.
// AOK bug: emulated fds have no host-side wait queue and no periodic rescan, and
// poll_mod_fd never poked the notify pipe -- so the blocked epoll_wait slept
// until its timeout instead of waking on the MOD. This is the lost wakeup that
// wedged syslog-ng/ivykis (a worker re-arms an eventfd to notify the writer
// thread, which never woke). Regression for fs/poll.c poll_add_fd/poll_mod_fd.
//
// The waiter uses a 6s epoll_wait timeout and the MOD happens ~0.5s in; a
// correct kernel wakes within a few ms, a broken one waits the full timeout.

#define _GNU_SOURCE

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>

#include "test_common.h"

#ifndef EPOLLONESHOT
#define EPOLLONESHOT (1 << 30)
#endif

static int ep, efd;
static volatile int woke;
static struct timespec t_mod, t_wake;

static void *waiter(void *a) {
    (void)a;
    struct epoll_event out[4];
    woke = epoll_wait(ep, out, 4, 6000);
    clock_gettime(CLOCK_MONOTONIC, &t_wake);
    return NULL;
}

static void test_mod_wake(void) {
    efd = eventfd(0, EFD_NONBLOCK);
    uint64_t one = 1;
    if (write(efd, &one, 8) != 8) { failf("epoll_mod_wake write", 0, 0, 0, 8, 0, 0); return; }

    ep = epoll_create1(0);
    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    ev.data.fd = efd;
    ev.events = 0;                                   // registered, not yet watched
    epoll_ctl(ep, EPOLL_CTL_ADD, efd, &ev);

    pthread_t t;
    pthread_create(&t, NULL, waiter, NULL);
    usleep(500000);                                  // waiter now blocked in epoll_wait

    clock_gettime(CLOCK_MONOTONIC, &t_mod);
    ev.events = EPOLLIN | EPOLLONESHOT;              // arm the already-ready eventfd
    epoll_ctl(ep, EPOLL_CTL_MOD, efd, &ev);

    pthread_join(t, NULL);

    double ms = (t_wake.tv_sec - t_mod.tv_sec) * 1000.0 +
                (t_wake.tv_nsec - t_mod.tv_nsec) / 1e6;
    test_logf("woke n=%d, %.0f ms after MOD\n", woke, ms);
    if (woke <= 0)
        failf("epoll_mod_wake ready", (uint64_t)(int64_t)woke, 0, 0, 1, 0, 0);
    if (ms >= 1000.0)   // correct: a few ms; bug: ~5500ms (waited to timeout)
        failf("epoll_mod_wake latency_ms", (uint64_t)ms, 0, 0, 0, 0, 0);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    test_mod_wake();
    return finish_suite("epoll_mod_wake");
}
