/*
 * sock_conn_error -- a dead connection reports itself ONCE, then stops.
 *
 * The bug this was written for: chronyd sat at 106% of one core on a device
 * that had been asleep. 12 seconds of strace showed 23748 pselect6 and 47496
 * recvmmsg, every recvmmsg failing with ECONNRESET on the same two fds, which
 * pselect kept reporting readable. Poll says ready, recv says error, nothing
 * changes, forever.
 *
 * The cause is a translation in fs/sock.c: iOS kills connected sockets when the
 * device sleeps, reads then return ENOTCONN, and AOK maps that to ECONNRESET.
 * The socket is dead for good, so the map fires on every call and the error is
 * re-delivered for ever while the fd goes on polling readable.
 *
 * A socket killed by device sleep cannot be created on demand, so this tests
 * the same CONTRACT through the two dead-connection shapes that can be:
 *
 *   1. a connected UDP socket that takes an ICMP port-unreachable, and
 *   2. a TCP connection the peer resets with SO_LINGER 0.
 *
 * In both, the rule a poll loop depends on is the same and is what AOK broke:
 * the error is delivered ONCE, and after that the socket does not go on
 * claiming it has something to read. Measured on real Linux (6.12) and on the
 * macOS host underneath AOK, which agree:
 *
 *     round 1: poll=1  recv=-1 Connection refused
 *     round 2: poll=0  recv=-1 Resource temporarily unavailable
 *     round 3: poll=0  recv=-1 Resource temporarily unavailable
 *
 * so a guest that answers differently is AOK's doing, not the host's.
 *
 * Passes on real Linux.
 */
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "test_common.h"

#define ROUNDS 4

static void fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("FAIL ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    failures_total++;
}

static const char *say(ssize_t r, int err) {
    static char b[80];
    if (r >= 0)
        snprintf(b, sizeof b, "%zd bytes", r);
    else
        snprintf(b, sizeof b, "-1 %s", strerror(err));
    return b;
}

// A loopback port with nothing on it: bind one, learn its number, close it.
static int dead_loopback_port(struct sockaddr_in *addr) {
    int probe = socket(AF_INET, SOCK_DGRAM, 0);
    if (probe < 0)
        return -1;
    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr->sin_port = 0;
    if (bind(probe, (struct sockaddr *) addr, sizeof(*addr)) < 0) {
        close(probe);
        return -1;
    }
    socklen_t len = sizeof(*addr);
    if (getsockname(probe, (struct sockaddr *) addr, &len) < 0) {
        close(probe);
        return -1;
    }
    close(probe);
    return 0;
}

// The shared contract. `label` names the shape; `expect_eof` says whether a
// drained stream socket should report end-of-file rather than EAGAIN.
//
// Waits for the error rather than assuming it has landed. On loopback both an
// ICMP unreachable and a TCP reset come back promptly, but "promptly" under an
// emulator running a hundred other tests on a loaded host is not "immediately",
// and a version of this that sampled a fixed 2s window reported "never
// reported the dead connection" for one that simply had not arrived yet.
static int check_settles(int s, const char *label, int expect_eof, int required) {
    unsigned budget_ms = test_watchdog_secs(required ? 20 : 2) * 1000;
    int errors_seen = 0, readable_after_error = 0, rounds_after_error = 0;

    for (unsigned waited = 0; waited < budget_ms; waited += 10) {
        struct pollfd p = {.fd = s, .events = POLLIN};
        int pr = poll(&p, 1, 10);
        char buf[64];
        ssize_t r = recv(s, buf, sizeof(buf), MSG_DONTWAIT);
        int err = errno;

        if (r < 0 && err != EAGAIN && err != EWOULDBLOCK) {
            errors_seen++;
            test_logf("  %s error #%d: poll=%d revents=0x%x recv=%s\n",
                      label, errors_seen, pr, pr > 0 ? p.revents : 0, say(r, err));
            continue;
        }
        if (errors_seen == 0)
            continue;               // not delivered yet; keep waiting

        rounds_after_error++;
        test_logf("  %s after %d: poll=%d revents=0x%x recv=%s\n",
                  label, rounds_after_error, pr, pr > 0 ? p.revents : 0, say(r, err));
        if (expect_eof && r == 0) {
            // A reset stream reads EOF for ever, and polls readable to say so.
        } else if (pr > 0 && r < 0) {
            // Past the error, and the socket is STILL claiming readability it
            // cannot honour. This is the 100%-CPU spin.
            readable_after_error++;
        }
        if (rounds_after_error >= ROUNDS)
            break;
    }

    if (errors_seen == 0) {
        if (required)
            fail("%s: never reported the dead connection, in %u ms", label, budget_ms);
        return 0;
    }
    if (errors_seen > 1)
        fail("%s: re-delivered the error %d times; a dead connection reports"
             " itself once", label, errors_seen);
    if (readable_after_error > 0)
        fail("%s: polled readable %d times after the error while recv had"
             " nothing to give -- this is the 100%%-CPU spin",
             label, readable_after_error);
    return 1;
}

// 1. Connected UDP, ICMP port unreachable.
//
// Repeated, and deliberately not asserting that EVERY attempt delivers. AOK
// loses this error outright about a third of the time -- measured 14 of 20
// against 20 of 20 on the macOS host underneath it, and when it does arrive it
// is always on the very first poll, so it is presence-or-absence rather than
// slowness. That is a real bug, it is older than this test, and it is filed on
// its own in docs/TODO.md; asserting on a single attempt here would just make
// the suite flaky and teach people to ignore it.
//
// What IS asserted: at least one of ATTEMPTS delivers (a total regression to
// never fails this, and at a one-in-three loss rate ten misses in a row is not
// something to see), and every attempt that DOES deliver obeys the contract --
// reported once, and not still claiming to be readable afterwards.
#define UDP_ATTEMPTS 10

static int udp_unreachable_once(void) {
    struct sockaddr_in addr;
    if (dead_loopback_port(&addr) < 0) {
        fail("udp: could not reserve a dead loopback port: %s", strerror(errno));
        return -1;
    }
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        fail("udp: socket: %s", strerror(errno));
        return -1;
    }
    if (connect(s, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        fail("udp: connect to 127.0.0.1:%d: %s", ntohs(addr.sin_port), strerror(errno));
        close(s);
        return -1;
    }
    if (send(s, "x", 1, 0) < 0) {
        fail("udp: send: %s", strerror(errno));
        close(s);
        return -1;
    }
    int delivered = check_settles(s, "udp-unreachable", 0, 0);
    close(s);
    return delivered;
}

static void udp_unreachable(void) {
    int delivered = 0;
    for (int i = 0; i < UDP_ATTEMPTS; i++)
        if (udp_unreachable_once() > 0)
            delivered++;
    test_logf("  udp-unreachable delivered %d of %d attempts\n", delivered, UDP_ATTEMPTS);
    if (delivered == 0)
        fail("udp-unreachable: no attempt out of %d ever reported the dead"
             " connection; the host reports it on the first poll, every time",
             UDP_ATTEMPTS);
}

// 2. TCP reset by the peer -- the closest reachable shape to a socket iOS
//    killed underneath us.
static void tcp_reset(void) {
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        fail("tcp: socket(listener): %s", strerror(errno));
        return;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, (struct sockaddr *) &addr, sizeof(addr)) < 0 ||
            listen(listener, 1) < 0) {
        fail("tcp: bind/listen: %s", strerror(errno));
        close(listener);
        return;
    }
    socklen_t alen = sizeof(addr);
    if (getsockname(listener, (struct sockaddr *) &addr, &alen) < 0) {
        fail("tcp: getsockname: %s", strerror(errno));
        close(listener);
        return;
    }

    int c = socket(AF_INET, SOCK_STREAM, 0);
    if (c < 0 || connect(c, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        fail("tcp: connect: %s", strerror(errno));
        if (c >= 0) close(c);
        close(listener);
        return;
    }
    int server = accept(listener, NULL, NULL);
    if (server < 0) {
        fail("tcp: accept: %s", strerror(errno));
        close(c);
        close(listener);
        return;
    }
    // SO_LINGER with a zero timeout makes close() send RST instead of FIN.
    struct linger lg = {.l_onoff = 1, .l_linger = 0};
    if (setsockopt(server, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg)) < 0) {
        printf("sock_conn_error: SKIP (SO_LINGER unsupported: %s)\n", strerror(errno));
        close(server); close(c); close(listener);
        return;
    }
    // Something in flight makes the reset unambiguous on every stack.
    if (send(c, "hello", 5, 0) < 0) {
        fail("tcp: send before reset: %s", strerror(errno));
        close(server); close(c); close(listener);
        return;
    }
    close(server);
    close(listener);
    check_settles(c, "tcp-reset", 1, 1);
    close(c);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(120));

    udp_unreachable();
    tcp_reset();

    return finish_suite("sock_conn_error");
}
