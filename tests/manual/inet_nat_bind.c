// Duplicate binds on a NAT'd loopback alias.
//
// AOK re-binds an AF_INET bind the host cannot satisfy -- a 127.x.y.z alias
// macOS does not have, or a privileged port -- to 127.0.0.1:<ephemeral>, and
// remembers the guest-visible endpoint so getsockname and connect see what the
// guest asked for. The catch is that the host can no longer detect a
// collision: two guests asking for the SAME alias and port each land on a
// DIFFERENT ephemeral host port, so both binds succeed and both believe they
// own the endpoint, while only one can ever be reached. Linux says
// EADDRINUSE, so AOK has to check the NAT table itself.
//
// Every expectation was measured against x86_64 glibc on Linux 6.12.
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "test_common.h"

static void check(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-46s got=%ld want=%ld\n", label, got, want);
}

static int bind_at(const char *addr, int port, int reuseport, int *err) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { *err = errno; return -1; }
    if (reuseport) {
        int one = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
    }
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof sin);
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);
    sin.sin_addr.s_addr = inet_addr(addr);
    errno = 0;
    if (bind(s, (struct sockaddr *) &sin, sizeof sin) < 0) { *err = errno; close(s); return -1; }
    if (listen(s, 8) < 0) { *err = errno; close(s); return -1; }
    *err = 0;
    return s;
}

// getsockname must report the endpoint the guest asked for, not the host's.
static void check_name(const char *label, int s, const char *want_addr, int want_port) {
    struct sockaddr_in sin;
    socklen_t len = sizeof sin;
    memset(&sin, 0, sizeof sin);
    if (getsockname(s, (struct sockaddr *) &sin, &len) < 0) {
        failf(label, (uint64_t) errno, 0, 0, 0, 0, 0);
        return;
    }
    char buf[32];
    inet_ntop(AF_INET, &sin.sin_addr, buf, sizeof buf);
    if (strcmp(buf, want_addr) != 0 || ntohs(sin.sin_port) != want_port)
        failf(label, (uint64_t) ntohl(sin.sin_addr.s_addr), (uint64_t) ntohs(sin.sin_port), 0,
              0, (uint64_t) want_port, 0);
    test_logf("  %-46s %s:%d\n", label, buf, ntohs(sin.sin_port));
}

// Ports are picked high and per-pid-ish to avoid colliding with a real
// listener on the host or a concurrent suite run.
#define P1 19311
#define P2 19412
#define P3 19413

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));
    int e;

    int a = bind_at("127.0.0.1", P1, 0, &e);
    if (a < 0) {
        printf("inet_nat_bind: SKIP (cannot bind 127.0.0.1:%d: %s)\n", P1, strerror(e));
        return 0;
    }
    check_name("127.0.0.1 reports what was asked", a, "127.0.0.1", P1);
    int b = bind_at("127.0.0.1", P1, 0, &e);
    check("duplicate on 127.0.0.1 refused", b < 0, 1);
    check("  with EADDRINUSE", e, EADDRINUSE);
    if (b >= 0) close(b);

    // The NAT'd alias: a distinct endpoint, and duplicates of it must be
    // refused just the same. This is the case the host cannot see.
    int c = bind_at("127.0.0.2", P1, 0, &e);
    if (c < 0) {
        printf("  127.0.0.2: SKIP (%s)\n", strerror(e));
    } else {
        check_name("127.0.0.2 reports its own address", c, "127.0.0.2", P1);
        int d = bind_at("127.0.0.2", P1, 0, &e);
        check("duplicate on the alias refused", d < 0, 1);
        check("  with EADDRINUSE", e, EADDRINUSE);
        if (d >= 0) close(d);

        // A different alias at the same port is a different endpoint.
        int f = bind_at("127.0.0.3", P1, 0, &e);
        check("a different alias at the same port is fine", f >= 0, 1);
        if (f >= 0) {
            check_name("  and reports its own address", f, "127.0.0.3", P1);
            close(f);
        }
        close(c);
    }

    // A wildcard bind collides with a specific address already holding it.
    int g = bind_at("0.0.0.0", P1, 0, &e);
    check("0.0.0.0 over a held specific address refused", g < 0, 1);
    check("  with EADDRINUSE", e, EADDRINUSE);
    if (g >= 0) close(g);
    close(a);

    // The endpoint is released when the socket closes.
    int h = bind_at("127.0.0.4", P2, 0, &e);
    if (h < 0) {
        printf("  release-on-close: SKIP (%s)\n", strerror(e));
    } else {
        close(h);
        int i = bind_at("127.0.0.4", P2, 0, &e);
        check("alias rebindable after close", i >= 0, 1);
        if (i >= 0) close(i);
    }

    // SO_REUSEPORT still lets two sockets share, as on Linux.
    int j = bind_at("127.0.0.5", P3, 1, &e);
    if (j < 0) {
        printf("  SO_REUSEPORT: SKIP (%s)\n", strerror(e));
    } else {
        int k = bind_at("127.0.0.5", P3, 1, &e);
        check("second SO_REUSEPORT bind shares the alias", k >= 0, 1);
        int l = bind_at("127.0.0.5", P3, 0, &e);
        check("a non-REUSEPORT bind is still refused", l < 0, 1);
        check("  with EADDRINUSE", e, EADDRINUSE);
        if (k >= 0) close(k);
        if (l >= 0) close(l);
        close(j);
    }

    return finish_suite("inet_nat_bind");
}
