// epoll_oneshot_rearm: after an EPOLLONESHOT event is delivered, the fd must
// stay REGISTERED in the epoll so EPOLL_CTL_MOD can re-arm it (Linux semantics).
// AOK bug: delivering a oneshot event removed+freed the registration, so the
// re-arm MOD returned ENOENT. ivykis's iv_fd_epoll uses an EPOLLONESHOT eventfd
// as its cross-thread wakeup and re-arms it with MOD -- the ENOENT iv_fatal'd
// and aborted syslog-ng, and the premature free fed a use-after-free in the
// multi-epoll close path. Deterministic: one delivery + re-arm per iteration.

#define _GNU_SOURCE

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>

#include "test_common.h"

#ifndef EPOLLONESHOT
#define EPOLLONESHOT (1 << 30)
#endif

static void test_oneshot_rearm(void) {
    int e = eventfd(0, EFD_NONBLOCK);
    uint64_t one = 1;
    if (write(e, &one, 8) != 8) { failf("epoll_oneshot_rearm write", (uint64_t)errno, 0, 0, 8, 0, 0); return; }

    int ep = epoll_create1(0);
    struct epoll_event ev, out[4];
    memset(&ev, 0, sizeof ev);
    ev.data.fd = e;
    ev.events = 0;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, e, &ev) != 0) { failf("epoll_oneshot_rearm add", (uint64_t)errno, 0, 0, 0, 0, 0); return; }

    for (int i = 0; i < 5; i++) {
        ev.events = EPOLLIN | EPOLLONESHOT;
        if (epoll_ctl(ep, EPOLL_CTL_MOD, e, &ev) != 0) {
            // ENOENT here means the oneshot delivery unregistered the fd.
            test_logf("iter %d: MOD failed: %s\n", i, strerror(errno));
            failf("epoll_oneshot_rearm rearm", (uint64_t)errno, (uint64_t)i, 0, 0, 0, 0);
            return;
        }
        int n = epoll_wait(ep, out, 4, 1000);   // delivers the oneshot -> disarm
        test_logf("iter %d: MOD ok, epoll_wait=%d\n", i, n);
        if (n <= 0)
            failf("epoll_oneshot_rearm ready", (uint64_t)(int64_t)n, (uint64_t)i, 0, 1, 0, 0);
    }
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    test_oneshot_rearm();
    return finish_suite("epoll_oneshot_rearm");
}
