// epoll_dup_add: Linux keys epoll membership by the (fd number, open file
// description) pair, so dup'd fds -- stdout and stderr are typically dups of
// one tty description -- may each carry their own registration on the same
// epoll instance, each with its own event mask and data. AOK bug: membership
// was keyed by the underlying open file description (struct fd) alone, so the
// second EPOLL_CTL_ADD returned EEXIST. Bun's event loop registers stdout and
// stderr separately, so any Bun binary with both wired to the same tty/pipe
// died at startup ("EEXIST: file already exists, epoll_ctl" from OMP on
// Devuan6-arm64). Regression for fs/poll.c (fd,guest_fd) keying + host-watch
// union/fan-out; each registration must deliver its own data.
//
// Covered: dup'd ADD succeeds; same-fd re-ADD still EEXIST; both
// registrations report with their own data; DEL of one leaves the sibling;
// MOD is per-registration (ENOENT for the deleted one); a registration
// outlives close() of the guest fd it was made under while the description
// stays open (the classic epoll close gotcha); same again for a host-backed
// fd (socketpair) which shares one host watch under the covers.

#define _GNU_SOURCE

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/socket.h>

#include "test_common.h"

#define DATA_ORIG  UINT64_C(0x1111000011110000)
#define DATA_DUP   UINT64_C(0x2222000022220000)

// One epoll_wait pass; returns a bitmask: 1 if DATA_ORIG seen, 2 if DATA_DUP
// seen, and counts events into *count. Fails the test on unknown data.
static int wait_mask(const char *label, int ep, int timeout_ms, int *count) {
    struct epoll_event out[8];
    int n = epoll_wait(ep, out, 8, timeout_ms);
    if (count != NULL)
        *count = n;
    int mask = 0;
    for (int i = 0; i < n; i++) {
        if (out[i].data.u64 == DATA_ORIG)
            mask |= 1;
        else if (out[i].data.u64 == DATA_DUP)
            mask |= 2;
        else
            failf(label, out[i].data.u64, 0, 0, DATA_ORIG, DATA_DUP, 0);
    }
    test_logf("%s: n=%d mask=%d\n", label, n, mask);
    return mask;
}

static void add_fd(const char *label, int ep, int fd, uint64_t data, uint32_t events) {
    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    ev.events = events;
    ev.data.u64 = data;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev) != 0)
        failf(label, (uint64_t) errno, 0, 0, 0, 0, 0);
}

// The scenario from the bug report, on an emulated-in-AOK fd (pipe).
static void test_pipe_dup(void) {
    int p[2];
    if (pipe(p) != 0) { failf("pipe", (uint64_t) errno, 0, 0, 0, 0, 0); return; }
    int rd = p[0], rd2 = dup(p[0]);
    if (rd2 < 0) { failf("dup", (uint64_t) errno, 0, 0, 0, 0, 0); return; }

    int ep = epoll_create1(0);
    add_fd("pipe ADD rd", ep, rd, DATA_ORIG, EPOLLIN);

    // Same guest fd again: still EEXIST.
    struct epoll_event ev = { .events = EPOLLIN, .data.u64 = DATA_ORIG };
    int r = epoll_ctl(ep, EPOLL_CTL_ADD, rd, &ev);
    if (r != -1 || errno != EEXIST)
        failf("pipe re-ADD rd EEXIST", (uint64_t)(int64_t) r, (uint64_t) errno, 0,
              (uint64_t)(int64_t) -1, EEXIST, 0);

    // The regression: a dup'd fd is a distinct registration, not EEXIST.
    add_fd("pipe ADD dup", ep, rd2, DATA_DUP, EPOLLIN);

    if (write(p[1], "x", 1) != 1)
        failf("pipe write", (uint64_t) errno, 0, 0, 0, 0, 0);

    // Both registrations must report, each with its own data.
    int n = 0;
    int mask = wait_mask("pipe wait both", ep, 3000, &n);
    if (mask != 3 || n != 2)
        failf("pipe both events", (uint64_t) mask, (uint64_t)(int64_t) n, 0, 3, 2, 0);

    // DEL one registration; the sibling keeps reporting (level-triggered,
    // byte still buffered).
    if (epoll_ctl(ep, EPOLL_CTL_DEL, rd, NULL) != 0)
        failf("pipe DEL rd", (uint64_t) errno, 0, 0, 0, 0, 0);
    mask = wait_mask("pipe wait after DEL", ep, 3000, &n);
    if (mask != 2 || n != 1)
        failf("pipe sibling survives DEL", (uint64_t) mask, (uint64_t)(int64_t) n, 0, 2, 1, 0);

    // MOD is per-registration: the deleted one is ENOENT, the sibling works.
    ev.events = EPOLLIN; ev.data.u64 = DATA_ORIG;
    r = epoll_ctl(ep, EPOLL_CTL_MOD, rd, &ev);
    if (r != -1 || errno != ENOENT)
        failf("pipe MOD deleted ENOENT", (uint64_t)(int64_t) r, (uint64_t) errno, 0,
              (uint64_t)(int64_t) -1, ENOENT, 0);
    ev.events = EPOLLIN; ev.data.u64 = DATA_DUP;
    if (epoll_ctl(ep, EPOLL_CTL_MOD, rd2, &ev) != 0)
        failf("pipe MOD dup", (uint64_t) errno, 0, 0, 0, 0, 0);

    // Classic epoll gotcha: closing the guest fd a registration was made
    // under does NOT remove the registration while the description stays
    // open (rd still references it).
    close(rd2);
    mask = wait_mask("pipe wait after close(dup)", ep, 3000, &n);
    if (mask != 2 || n != 1)
        failf("pipe reg outlives close", (uint64_t) mask, (uint64_t)(int64_t) n, 0, 2, 1, 0);

    close(ep);
    close(rd);
    close(p[1]);
}

// Same shape on a host-backed fd (socketpair): both registrations share one
// host-side watch, so this exercises the union-of-interest programming and
// event fan-out.
static void test_socket_dup(void) {
    int s[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, s) != 0) {
        failf("socketpair", (uint64_t) errno, 0, 0, 0, 0, 0);
        return;
    }
    int sd = dup(s[0]);
    if (sd < 0) { failf("sock dup", (uint64_t) errno, 0, 0, 0, 0, 0); return; }

    int ep = epoll_create1(0);
    add_fd("sock ADD orig", ep, s[0], DATA_ORIG, EPOLLIN);
    add_fd("sock ADD dup", ep, sd, DATA_DUP, EPOLLIN);

    if (write(s[1], "y", 1) != 1)
        failf("sock write", (uint64_t) errno, 0, 0, 0, 0, 0);

    int n = 0;
    int mask = wait_mask("sock wait both", ep, 3000, &n);
    if (mask != 3 || n != 2)
        failf("sock both events", (uint64_t) mask, (uint64_t)(int64_t) n, 0, 3, 2, 0);

    if (epoll_ctl(ep, EPOLL_CTL_DEL, sd, NULL) != 0)
        failf("sock DEL dup", (uint64_t) errno, 0, 0, 0, 0, 0);
    mask = wait_mask("sock wait after DEL", ep, 3000, &n);
    if (mask != 1 || n != 1)
        failf("sock sibling survives DEL", (uint64_t) mask, (uint64_t)(int64_t) n, 0, 1, 1, 0);

    close(ep);
    close(sd);
    close(s[0]);
    close(s[1]);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(30));
    test_pipe_dup();
    test_socket_dup();
    return finish_suite("epoll_dup_add");
}
