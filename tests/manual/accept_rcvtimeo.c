// Linux accept(2) honors the listening socket's SO_RCVTIMEO: a blocking
// accept4() on a listener with a receive timeout returns EAGAIN when the
// timeout elapses with no pending connection. Darwin's accept() ignores
// SO_RCVTIMEO entirely (it applies to recv-family calls only there), and
// AOK's sys_accept4_common used to call the blocking host accept() directly,
// so under AOK the timeout never fired.
//
// systemd-userdbd's userwork workers (systemd >= 250, src/userdb/userwork.c)
// are built on exactly this contract: a BLOCKING listener with SO_RCVTIMEO
// armed by the manager, where the periodic EAGAIN tick drives the entire
// worker lifecycle -- idle exit after listen_idle_usec, retirement after
// ITERATIONS_MAX/RUNTIME_MAX, and the manager's respawn logic downstream of
// those exits. Under AOK all three pre-spawned workers blocked forever
// inside the host accept() (observed via lldb on device during the Arch
// aarch64 systemd-260 boot wedge at "Starting D-Bus System Message Bus..."),
// so no worker ever exited, no maintenance ran, and userdb varlink queries
// stalled with them.
//
// Fixed by never blocking in the host accept(): the host listener is kept
// permanently nonblocking and the wait is emulated with poll() -- guest
// O_NONBLOCK -> immediate EAGAIN, SO_RCVTIMEO -> EAGAIN at the deadline,
// plain blocking -> interruptible poll. Two secondary conformance fixes ride
// along and are covered here: BSD accepted sockets inherit the listener's
// O_NONBLOCK (Linux hands out fresh flags -- the inherited flag is stripped),
// and sock_getflags now reports the guest's own O_NONBLOCK from fd->flags so
// the forced host-side flag never leaks into guest F_GETFL.
//
// Also passes on real Linux (mint oracle, verified).
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "test_common.h"

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int mklistener(const char *path) {
    unlink(path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
    if (bind(fd, (struct sockaddr *) &sa, sizeof(sa)) < 0) return -1;
    if (listen(fd, 5) < 0) return -1;
    return fd;
}

static int connect_to(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
    if (connect(fd, (struct sockaddr *) &sa, sizeof(sa)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void check(int cond, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (!cond) {
        printf("FAIL ");
        vprintf(fmt, ap);
        printf("\n");
        failures_total++;
    } else if (test_verbose) {
        printf("ok ");
        vprintf(fmt, ap);
        printf("\n");
    }
    va_end(ap);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));

    // 1. userwork pattern: blocking listener + SO_RCVTIMEO=1s -> EAGAIN ~1s
    int lfd = mklistener("/tmp/acc_rt1.sock");
    check(lfd >= 0, "listener created (%s)", strerror(errno));
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    check(setsockopt(lfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0,
          "SO_RCVTIMEO set (%s)", strerror(errno));
    double t0 = now_sec();
    errno = 0;
    int c = accept4(lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    double dt = now_sec() - t0;
    check(c < 0 && errno == EAGAIN, "rcvtimeo accept4 -> EAGAIN (ret=%d errno=%d)", c, errno);
    check(dt >= 0.5 && dt <= 10.0, "rcvtimeo accept waited ~1s (%.2fs)", dt);

    // 2. guest F_GETFL on the listener must NOT show O_NONBLOCK (the guest
    // never set it; only AOK's internal host-side flag is nonblocking)
    int fl = fcntl(lfd, F_GETFL);
    check(fl >= 0 && !(fl & O_NONBLOCK), "listener F_GETFL has no O_NONBLOCK (%#x)", fl);

    // 3. pending connection: immediate accept; accepted fd gets fresh flags
    // (no BSD O_NONBLOCK inheritance), CLOEXEC per accept4 flag
    int cfd = connect_to("/tmp/acc_rt1.sock");
    check(cfd >= 0, "connect (%s)", strerror(errno));
    t0 = now_sec();
    int conn = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
    dt = now_sec() - t0;
    check(conn >= 0 && dt < 0.5, "pending connection accepted fast (ret=%d %.2fs)", conn, dt);
    fl = fcntl(conn, F_GETFL);
    check(fl >= 0 && !(fl & O_NONBLOCK), "accepted fd has no inherited O_NONBLOCK (%#x)", fl);
    check(fcntl(conn, F_GETFD) & FD_CLOEXEC, "accepted fd has CLOEXEC");
    // blocking read on the accepted fd must wait for data, not EAGAIN
    pid_t writer = fork();
    if (writer == 0) {
        usleep(300000);
        (void) !write(cfd, "x", 1);
        _exit(0);
    }
    char b;
    t0 = now_sec();
    ssize_t n = read(conn, &b, 1);
    dt = now_sec() - t0;
    check(n == 1 && dt >= 0.15, "blocking read on accepted fd waited (n=%zd %.2fs)", n, dt);
    waitpid(writer, NULL, 0);
    close(conn); close(cfd);

    // 4. thundering herd: 3 children in blocking rcvtimeo accept4, one
    // connection -> exactly one winner, two EAGAIN losers at the timeout
    int lfd2 = mklistener("/tmp/acc_rt2.sock");
    struct timeval tv2 = { .tv_sec = 2, .tv_usec = 0 };
    check(setsockopt(lfd2, SOL_SOCKET, SO_RCVTIMEO, &tv2, sizeof(tv2)) == 0,
          "herd SO_RCVTIMEO set");
    pid_t kids[3];
    for (int i = 0; i < 3; i++) {
        kids[i] = fork();
        if (kids[i] == 0) {
            int k = accept4(lfd2, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (k >= 0) _exit(10);
            _exit(errno == EAGAIN ? 11 : 12);
        }
    }
    usleep(500000);
    int cfd2 = connect_to("/tmp/acc_rt2.sock");
    check(cfd2 >= 0, "herd connect (%s)", strerror(errno));
    int winners = 0, losers = 0, others = 0;
    for (int i = 0; i < 3; i++) {
        int st;
        waitpid(kids[i], &st, 0);
        int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        if (code == 10) winners++;
        else if (code == 11) losers++;
        else others++;
    }
    check(winners == 1 && losers == 2 && others == 0,
          "herd: 1 winner, 2 EAGAIN losers (w=%d l=%d o=%d)", winners, losers, others);
    close(cfd2);

    // 5. plain blocking accept (no rcvtimeo): waits, wakes on connection
    int lfd3 = mklistener("/tmp/acc_rt3.sock");
    pid_t connector = fork();
    if (connector == 0) {
        usleep(400000);
        int k = connect_to("/tmp/acc_rt3.sock");
        usleep(200000);
        if (k >= 0) close(k);
        _exit(0);
    }
    t0 = now_sec();
    int conn3 = accept4(lfd3, NULL, NULL, 0);
    dt = now_sec() - t0;
    check(conn3 >= 0 && dt >= 0.2, "plain blocking accept woke on connection (ret=%d %.2fs)", conn3, dt);
    waitpid(connector, NULL, 0);

    // 6. guest-nonblocking listener: immediate EAGAIN both via creation flag
    // semantics (F_SETFL here) and reported in F_GETFL
    int lfd4 = mklistener("/tmp/acc_rt4.sock");
    fl = fcntl(lfd4, F_GETFL);
    check(fcntl(lfd4, F_SETFL, fl | O_NONBLOCK) == 0, "F_SETFL O_NONBLOCK");
    t0 = now_sec();
    errno = 0;
    c = accept4(lfd4, NULL, NULL, 0);
    dt = now_sec() - t0;
    check(c < 0 && errno == EAGAIN && dt < 0.2,
          "nonblocking accept -> immediate EAGAIN (ret=%d errno=%d %.2fs)", c, errno, dt);
    fl = fcntl(lfd4, F_GETFL);
    check(fl >= 0 && (fl & O_NONBLOCK), "nonblocking listener F_GETFL shows O_NONBLOCK");

    // 7. non-listening socket: EINVAL immediately, never blocks
    int nl = socket(AF_UNIX, SOCK_STREAM, 0);
    t0 = now_sec();
    errno = 0;
    c = accept4(nl, NULL, NULL, 0);
    dt = now_sec() - t0;
    check(c < 0 && errno == EINVAL && dt < 0.5,
          "accept on non-listening -> EINVAL fast (errno=%d %.2fs)", errno, dt);

    unlink("/tmp/acc_rt1.sock"); unlink("/tmp/acc_rt2.sock");
    unlink("/tmp/acc_rt3.sock"); unlink("/tmp/acc_rt4.sock");
    return finish_suite("accept_rcvtimeo");
}
