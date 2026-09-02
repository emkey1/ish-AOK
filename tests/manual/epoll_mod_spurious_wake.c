// epoll_mod_spurious_wake: EPOLL_CTL_MOD on an emulated fd (eventfd) that is
// NOT currently ready must NOT wake a thread blocked in epoll_wait() on that
// set. AOK bug (introduced by the fix for epoll_mod_wake, commit 3e5903a0):
// poll_add_fd/poll_mod_fd poked the poll's notify pipe on EVERY ADD/MOD of an
// emulated fd, regardless of whether the newly-armed interest was actually
// satisfied. Real Linux (ep_insert/ep_modify) only wakes waiters when the
// file is already ready at ADD/MOD time.
//
// Symptom on-device (iSH-AOK 538): syslog-ng burned continuous, mostly-system
// CPU with near-zero log throughput. ivykis's cross-thread eventfd handling
// re-arms via EPOLL_CTL_MOD frequently as steady-state bookkeeping, not only
// when there's real data; each such MOD was forcing syslog-ng's blocked
// epoll_wait to wake, rescan (find nothing), and resleep -- a wake/rescan/
// resleep loop burning syscalls with zero throughput. Local repro isolated
// the exact mechanism: with the pre-fix poll.c, ~1900 idle MOD calls over
// 2.5s produced ~1900 sleep/wake cycles (traced via ISH_TRACE_POLL_WAIT);
// with the fix, the same spam produces exactly one sleep, ended only by the
// real write. Regression for fs/poll.c poll_add_fd/poll_mod_fd
// (poll_fd_currently_ready gate).
//
// This test also re-confirms the ORIGINAL epoll_mod_wake fix isn't broken:
// after spamming MOD with no data, a real write + MOD must still wake the
// waiter promptly.

#define _GNU_SOURCE

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/resource.h>

#include "test_common.h"

#ifndef EPOLLONESHOT
#define EPOLLONESHOT (1 << 30)
#endif

static int ep, efd;
static volatile int woke;
static struct timespec t_write, t_wake;
static double waiter_cpu_ms;

static double rusage_thread_ms(void) {
    struct rusage ru;
    if (getrusage(RUSAGE_THREAD, &ru) != 0)
        return -1.0;
    return (ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000.0 +
           (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1000.0;
}

static void *waiter(void *a) {
    (void)a;
    double cpu_before = rusage_thread_ms();
    struct epoll_event out[4];
    woke = epoll_wait(ep, out, 4, 4000);
    clock_gettime(CLOCK_MONOTONIC, &t_wake);
    double cpu_after = rusage_thread_ms();
    if (cpu_before >= 0.0 && cpu_after >= 0.0)
        waiter_cpu_ms = cpu_after - cpu_before;
    return NULL;
}

static void test_no_spurious_wake(void) {
    efd = eventfd(0, EFD_NONBLOCK);   // starts at 0, not ready

    ep = epoll_create1(0);
    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    ev.data.fd = efd;
    ev.events = EPOLLIN | EPOLLONESHOT;
    epoll_ctl(ep, EPOLL_CTL_ADD, efd, &ev);

    pthread_t t;
    pthread_create(&t, NULL, waiter, NULL);
    usleep(200000);   // waiter now blocked in epoll_wait, efd not ready

    // Spam MOD (re-arm, no write) for ~1.5s, mimicking ivykis's idle-time
    // oneshot re-arm bookkeeping. None of these should wake the waiter: the
    // eventfd is never written, so it never becomes ready.
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    int mods = 0;
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed_ms = (now.tv_sec - start.tv_sec) * 1000.0 +
                             (now.tv_nsec - start.tv_nsec) / 1e6;
        if (elapsed_ms > 1500.0)
            break;
        epoll_ctl(ep, EPOLL_CTL_MOD, efd, &ev);
        mods++;
    }
    test_logf("issued %d idle MOD calls with no data written\n", mods);
    if (woke != 0)
        failf("epoll_mod_spurious_wake premature", (uint64_t)(int64_t)woke, 0, 0, 0, 0, 0);

    // Now make it genuinely ready and confirm the ORIGINAL fix (epoll_mod_wake)
    // still works: a real write + MOD must wake the waiter promptly.
    uint64_t one = 1;
    if (write(efd, &one, 8) != 8) { failf("epoll_mod_spurious_wake write", 0, 0, 0, 8, 0, 0); return; }
    clock_gettime(CLOCK_MONOTONIC, &t_write);
    epoll_ctl(ep, EPOLL_CTL_MOD, efd, &ev);

    pthread_join(t, NULL);

    double ms = (t_wake.tv_sec - t_write.tv_sec) * 1000.0 +
                (t_wake.tv_nsec - t_write.tv_nsec) / 1e6;
    test_logf("woke n=%d, %.0f ms after real write+MOD\n", woke, ms);
    if (woke <= 0)
        failf("epoll_mod_spurious_wake ready", (uint64_t)(int64_t)woke, 0, 0, 1, 0, 0);
    if (ms >= 1000.0)
        failf("epoll_mod_spurious_wake latency_ms", (uint64_t)ms, 0, 0, 0, 0, 0);

    // The real bug symptom: a blocked waiter's thread burns host CPU on every
    // idle MOD (wake, rescan, resleep) even though nothing ever became ready
    // until the final real write. A correct implementation only wakes once
    // (for the real write), so the waiter thread's CPU time across the whole
    // ~1.7s block should be a few ms at most; the pre-fix bug burns CPU
    // roughly proportional to the ~1000 idle MODs above.
    test_logf("waiter thread CPU time while blocked: %.1f ms\n", waiter_cpu_ms);
    if (waiter_cpu_ms > 200.0)
        failf("epoll_mod_spurious_wake waiter_cpu_ms", (uint64_t)waiter_cpu_ms, 0, 0, 200, 0, 0);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    test_no_spurious_wake();
    return finish_suite("epoll_mod_spurious_wake");
}
