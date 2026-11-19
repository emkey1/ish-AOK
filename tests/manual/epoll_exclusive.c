// EPOLLEXCLUSIVE (bit 28, 1<<28) EINVAL validation.
//
// kernel/epoll.c's sys_epoll_ctl_guest stores whatever event bits the guest
// passes straight through to poll_add_fd/poll_mod_fd (AOK-iSH's internal
// POLL_* bit values were deliberately chosen to numerically match the guest
// EPOLLIN/EPOLLOUT/EPOLLONESHOT/EPOLLET bits, so there's no translation
// layer) -- EPOLLEXCLUSIVE just rode along unrecognized, so none of Linux's
// EPOLLEXCLUSIVE EINVAL rules (man epoll_ctl(2)) were enforced. This is a
// distinct conformance gap from the ELOOP nesting-cycle bug (epoll_eloop.c):
// found while fixing that bug, and part of why stress-ng's --epoll stressor
// still reported failures after the ELOOP fix landed, since its script
// exercises these exact EPOLLEXCLUSIVE cases.
//
// Only the three EINVAL *validation* rules are covered, not the full
// exclusive-wakeup load-balancing behavior (waking only one of several
// epoll instances watching the same fd) -- that's a separate, much larger
// feature and not what stress-ng's conformance check exercises:
//   1. EPOLLEXCLUSIVE on EPOLL_CTL_MOD is always EINVAL, regardless of
//      whether the target fd is even registered on that epoll instance.
//   2. Once an fd has been added with EPOLLEXCLUSIVE, any subsequent MOD on
//      that (epfd, fd) pair is EINVAL, even with new bits that drop
//      EPOLLEXCLUSIVE.
//   3. EPOLL_CTL_ADD with EPOLLEXCLUSIVE where the target fd is itself an
//      epoll instance (nested epoll) is EINVAL.
// Regression for kernel/epoll.c's sys_epoll_ctl_guest EPOLLEXCLUSIVE checks
// and fs/poll.c's poll_fd_is_exclusive.
#define _GNU_SOURCE
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include "test_common.h"

#ifndef EPOLLEXCLUSIVE
#define EPOLLEXCLUSIVE (1u << 28)
#endif

static void check_errno(const char *label, int ret, int want_ret, int want_errno) {
    int err = errno;
    if (ret != want_ret || (want_ret < 0 && err != want_errno)) {
        printf("FAIL %s: ret=%d errno=%d(%s) want ret=%d errno=%d\n",
               label, ret, err, strerror(err), want_ret, want_errno);
        failures_total++;
    } else {
        test_logf("%s: ret=%d errno=%d ok\n", label, ret, err);
    }
}

int main(int argc, char **argv) {
    test_init(argc, argv);

    int ep = epoll_create1(0);
    int ep_nested = epoll_create1(0);
    int s[2];
    if (ep < 0 || ep_nested < 0 || socketpair(AF_UNIX, SOCK_STREAM, 0, s) < 0) {
        printf("FAIL: setup: %s\n", strerror(errno));
        return finish_suite("epoll_exclusive");
    }
    int sockfd = s[0];

    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);

    // Rule 1: EPOLLEXCLUSIVE on MOD is EINVAL even when the fd was never
    // registered on this epoll instance at all (checked before fd lookup).
    ev.events = EPOLLIN | EPOLLEXCLUSIVE;
    ev.data.fd = sockfd;
    errno = 0;
    check_errno("MOD EXCLUSIVE on unregistered fd", epoll_ctl(ep, EPOLL_CTL_MOD, sockfd, &ev), -1, EINVAL);

    // Rule 3: ADD with EPOLLEXCLUSIVE where the target fd is itself an epoll
    // instance (nested epoll) is EINVAL.
    ev.events = EPOLLIN | EPOLLEXCLUSIVE;
    ev.data.fd = ep_nested;
    errno = 0;
    check_errno("ADD EXCLUSIVE nested epoll", epoll_ctl(ep, EPOLL_CTL_ADD, ep_nested, &ev), -1, EINVAL);
    // The rejected nested-epoll add must not have registered anything.
    ev.events = EPOLLIN;
    ev.data.fd = ep_nested;
    errno = 0;
    check_errno("DEL nested epoll after rejected ADD", epoll_ctl(ep, EPOLL_CTL_DEL, ep_nested, &ev), -1, ENOENT);

    // A plain (non-exclusive) ADD of sockfd succeeds and sets a baseline.
    ev.events = EPOLLIN;
    ev.data.fd = sockfd;
    errno = 0;
    check_errno("ADD sockfd (plain)", epoll_ctl(ep, EPOLL_CTL_ADD, sockfd, &ev), 0, 0);
    // A plain MOD on it is unaffected by these rules.
    ev.events = EPOLLIN | EPOLLOUT;
    ev.data.fd = sockfd;
    errno = 0;
    check_errno("MOD sockfd (plain)", epoll_ctl(ep, EPOLL_CTL_MOD, sockfd, &ev), 0, 0);
    check_errno("DEL sockfd (plain, reset)", epoll_ctl(ep, EPOLL_CTL_DEL, sockfd, NULL), 0, 0);

    // Rule 2: register sockfd with EPOLLEXCLUSIVE, then ANY subsequent MOD
    // on that pair is EINVAL -- even one that doesn't specify EPOLLEXCLUSIVE.
    ev.events = EPOLLIN | EPOLLEXCLUSIVE;
    ev.data.fd = sockfd;
    errno = 0;
    check_errno("ADD sockfd EXCLUSIVE", epoll_ctl(ep, EPOLL_CTL_ADD, sockfd, &ev), 0, 0);

    ev.events = EPOLLIN;
    ev.data.fd = sockfd;
    errno = 0;
    check_errno("MOD after EXCLUSIVE add (no EXCLUSIVE bit)", epoll_ctl(ep, EPOLL_CTL_MOD, sockfd, &ev), -1, EINVAL);

    ev.events = EPOLLIN | EPOLLEXCLUSIVE;
    ev.data.fd = sockfd;
    errno = 0;
    check_errno("MOD after EXCLUSIVE add (with EXCLUSIVE bit)", epoll_ctl(ep, EPOLL_CTL_MOD, sockfd, &ev), -1, EINVAL);

    // The exclusive registration itself is undisturbed by the rejected MODs:
    // DEL must still find it registered.
    check_errno("DEL sockfd (still registered)", epoll_ctl(ep, EPOLL_CTL_DEL, sockfd, NULL), 0, 0);

    // After DEL, a fresh non-exclusive ADD+MOD on the same fd must succeed
    // again -- the EINVAL from rule 2 is per-registration, not permanent.
    ev.events = EPOLLIN;
    ev.data.fd = sockfd;
    errno = 0;
    check_errno("ADD sockfd (fresh, plain)", epoll_ctl(ep, EPOLL_CTL_ADD, sockfd, &ev), 0, 0);
    ev.events = EPOLLIN | EPOLLOUT;
    ev.data.fd = sockfd;
    errno = 0;
    check_errno("MOD sockfd (fresh, plain)", epoll_ctl(ep, EPOLL_CTL_MOD, sockfd, &ev), 0, 0);

    close(ep);
    close(ep_nested);
    close(s[0]);
    close(s[1]);
    return finish_suite("epoll_exclusive");
}
