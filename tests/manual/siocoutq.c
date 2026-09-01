// Linux SIOCOUTQ (== TIOCOUTQ, 0x5411) on a socket reports the number of
// bytes still queued in the send buffer. AOK's sock_ioctl had no handler for
// it, so it fell through to realfs_ioctl and returned ENOTTY.
//
// dbus-broker is built on this ioctl: socket_dispatch_write()
// (src/dbus/socket.c) calls ioctl(fd, SIOCOUTQ) whenever a connection has
// fd-carrying messages in its pending queue, and treats ANY failure as fatal
// (error_origin -> main-loop exit 1). Under AOK that meant the system bus
// died with exit-code 1 the first time a connection's output backed up --
// which is exactly what systemd-logind's session-scope creation traffic does
// on every login (ssh or console): pam_systemd CreateSession -> logind
// StartTransientUnit(session-c1.scope) through the broker -> broker exit 1,
// "Failed to start session scope: Connection reset by peer", and
// logind/resolved left nameless with 25s bus timeouts on every later call.
//
// The emulation reads the host send-queue depth (Darwin: getsockopt
// SO_NWRITE; Linux host: ioctl TIOCOUTQ). Exact values are not contractual
// here -- for AF_UNIX Darwin accounts bytes to the receiver where Linux
// charges the sender, so this test only pins down what callers actually rely
// on: the call succeeds on every socket family, reports 0/low when idle, and
// drains back to low once the peer consumed the data (dbus-broker's
// v > 128 "still draining" check).
//
// Also passes on real Linux (mint oracle, verified).
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include "test_common.h"

#ifndef SIOCOUTQ
#define SIOCOUTQ 0x5411
#endif
#ifndef SIOCINQ
#define SIOCINQ FIONREAD
#endif

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
    alarm(test_watchdog_secs(30));

    // 1. unix stream pair: idle socket reports success and an empty queue
    int sp[2];
    check(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0, "socketpair (%s)", strerror(errno));
    int v = -1;
    int r = ioctl(sp[0], SIOCOUTQ, &v);
    check(r == 0, "SIOCOUTQ on idle unix stream succeeds (ret=%d errno=%d %s)", r, errno, strerror(errno));
    check(v == 0, "idle unix stream outq is 0 (got %d)", v);

    // 2. with unread data in flight the call must still succeed (this is the
    // dbus-broker fatal path); the value is >= 0 but platform-dependent.
    // Write nonblocking: the pair's buffer is much smaller on some platforms
    // than Linux's default, and a large blocking write would deadlock here.
    char buf[65536];
    memset(buf, 'x', sizeof(buf));
    int fl = fcntl(sp[0], F_GETFL);
    fcntl(sp[0], F_SETFL, fl | O_NONBLOCK);
    ssize_t wr = 0;
    for (;;) {
        ssize_t n = write(sp[0], buf, sizeof(buf) - wr);
        if (n <= 0) break;
        wr += n;
        if (wr >= (ssize_t) sizeof(buf)) break;
    }
    fcntl(sp[0], F_SETFL, fl);
    check(wr > 0, "wrote %zd bytes into pair (%s)", wr, strerror(errno));
    v = -1;
    r = ioctl(sp[0], SIOCOUTQ, &v);
    check(r == 0 && v >= 0, "SIOCOUTQ with unread data succeeds (ret=%d v=%d)", r, v);

    // 3. peer-side SIOCINQ sees the bytes
    v = -1;
    r = ioctl(sp[1], SIOCINQ, &v);
    check(r == 0 && v == (int) wr, "SIOCINQ on peer sees %zd bytes (ret=%d v=%d)", wr, r, v);

    // 4. after the peer drains, outq falls to dbus-broker's "empty" band
    ssize_t got = 0;
    while (got < wr) {
        ssize_t n = read(sp[1], buf, sizeof(buf));
        if (n <= 0) break;
        got += n;
    }
    check(got == wr, "peer drained %zd/%zd bytes", got, wr);
    int settled = 0;
    for (int i = 0; i < 40; i++) {  // up to ~2s: the kernel-side release is async
        v = -1;
        if (ioctl(sp[0], SIOCOUTQ, &v) == 0 && v <= 128) {
            settled = 1;
            break;
        }
        usleep(50000);
    }
    check(settled, "outq drained to <= 128 after peer read (last v=%d)", v);
    close(sp[0]);
    close(sp[1]);

    // 5. unix dgram: dbus-broker's log stream also probes SIOCOUTQ
    check(socketpair(AF_UNIX, SOCK_DGRAM, 0, sp) == 0, "dgram socketpair (%s)", strerror(errno));
    v = -1;
    r = ioctl(sp[0], SIOCOUTQ, &v);
    check(r == 0 && v >= 0, "SIOCOUTQ on unix dgram succeeds (ret=%d v=%d)", r, v);
    close(sp[0]);
    close(sp[1]);

    // 6. TCP socket (unconnected is fine -- the queue is just empty)
    int tcp = socket(AF_INET, SOCK_STREAM, 0);
    check(tcp >= 0, "tcp socket (%s)", strerror(errno));
    v = -1;
    r = ioctl(tcp, SIOCOUTQ, &v);
    check(r == 0 && v == 0, "SIOCOUTQ on fresh tcp socket -> 0 (ret=%d v=%d)", r, v);
    close(tcp);

    return finish_suite("siocoutq");
}
