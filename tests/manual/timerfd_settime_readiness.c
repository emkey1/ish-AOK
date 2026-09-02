// timerfd_settime must reset the expiration counter -- and therefore poll/
// epoll readiness -- on EVERY call, including a disarm (it_value = {0,0}).
// Linux (timerfd_settime(2)): "the counter of expirations is reset to zero"
// whenever the timer is set.
//
// AOK only ever cleared expirations in read(), so an expiration that was
// never read() survived a disarm and left the fd permanently POLL_READ.
// Real-world casualty: libwayland's event loop multiplexes all wl_event_loop
// timers onto ONE shared timerfd (timer heap). When the last armed timer is
// removed after an expiration that nobody read (labwc's pipemenu 4s timeout
// firing is exactly that), it disarms with settime(fd, 0, {0,0}) expecting
// readiness to clear. Under AOK the fd stayed hot, so labwc's epoll loop
// dispatched a phantom timer forever: ~3000 epoll_pwait+timerfd_settime
// cycles/second, one guest CPU pinned, on-device (Arch aarch64, labwc
// 0.20.1). Found while debugging the Display applet's empty Applications
// pipemenu on the old-iPad rig.
//
// Covers: readable-after-expiry baseline; disarm clears readiness (poll,
// epoll level-triggered, and O_NONBLOCK read -> EAGAIN); re-arm also clears
// stale readiness and the new arm still fires; read path still consumes
// normally. Also passes on real Linux.
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

static void check(int cond, const char *what) {
    test_log_if(!cond, "%s: %s\n", cond ? "ok" : "FAIL", what);
    if (!cond)
        failures_total++;
}

static void sleep_ms(int ms) {
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static int poll_readable(int fd, int timeout_ms) {
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int n = poll(&pfd, 1, timeout_ms);
    return n == 1 && (pfd.revents & POLLIN);
}

static void arm_ms(int fd, int ms) {
    struct itimerspec its = {
        .it_value = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L },
    };
    if (timerfd_settime(fd, 0, &its, NULL) != 0) {
        perror("timerfd_settime arm");
        exit(2);
    }
}

static void disarm(int fd) {
    struct itimerspec its = {};
    if (timerfd_settime(fd, 0, &its, NULL) != 0) {
        perror("timerfd_settime disarm");
        exit(2);
    }
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(30));

    int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (fd < 0) {
        perror("timerfd_create");
        return 2;
    }
    int ep = epoll_create1(0);
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = fd };
    if (ep < 0 || epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev) != 0) {
        perror("epoll setup");
        return 2;
    }

    // Baseline: an expiration makes the fd readable.
    arm_ms(fd, 20);
    check(poll_readable(fd, 2000), "fd readable after expiration");

    // Disarm WITHOUT reading: readiness must clear (the libwayland shape).
    disarm(fd);
    check(!poll_readable(fd, 0), "poll: not readable after disarm");
    int n = epoll_wait(ep, &ev, 1, 0);
    check(n == 0, "epoll: no event after disarm");
    uint64_t count;
    ssize_t r = read(fd, &count, sizeof(count));
    check(r == -1 && errno == EAGAIN, "read: EAGAIN after disarm");

    // The disarmed timer must not fire later either.
    sleep_ms(100);
    check(!poll_readable(fd, 0), "still not readable 100ms after disarm");

    // Re-arm over an unread expiration: the stale count is discarded, the fd
    // is unreadable until the NEW arm expires, then fires normally.
    arm_ms(fd, 20);
    check(poll_readable(fd, 2000), "readable after first re-arm expiry");
    arm_ms(fd, 300);  // re-arm without reading the pending expiration
    check(!poll_readable(fd, 0), "re-arm clears stale readiness");
    check(poll_readable(fd, 2000), "new arm still fires");
    r = read(fd, &count, sizeof(count));
    check(r == (ssize_t) sizeof(count) && count >= 1, "read consumes new expiration");
    check(!poll_readable(fd, 0), "not readable after read");

    // Plain read path sanity: arm, wait, read.
    arm_ms(fd, 20);
    check(poll_readable(fd, 2000), "readable for final read check");
    r = read(fd, &count, sizeof(count));
    check(r == (ssize_t) sizeof(count) && count >= 1, "final read returns count");

    close(ep);
    close(fd);
    return finish_suite("timerfd_settime_readiness");
}
