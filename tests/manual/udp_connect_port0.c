// udp_connect_port0: a UDP socket may connect() to port 0, as Linux allows.
//
// glibc's getaddrinfo orders its answers by connecting a UDP socket to each
// one, port 0 when no service was asked for, and asking getsockname what
// source address the route chose (RFC 6724 rule 1, "avoid unusable
// destinations"). Darwin refuses port 0 with EADDRNOTAVAIL, AOK passed that
// through, every address looked unusable, and rule 6 then ranked IPv6 first:
// on a network with no IPv6 route, `getent ahosts google.com` led with an
// IPv6 address and `ping google.com` got "No route to host" for every packet
// while `ping 8.8.8.8` worked (seen 2026-11-22 on an A10X iPad, PSCAL image).
//
// What Linux does, measured with this test's own checks in a Debian arm64
// container: connect() succeeds and picks a source address; getpeername says
// ENOTCONN (there is no peer port to name); send() succeeds (the datagram
// goes to port 0, where nothing listens -- on loopback the port-unreachable
// that comes back can fail the next call with ECONNREFUSED); and connecting the same
// socket to a real port afterwards makes it an ordinary connected socket.
// Loopback only, so the result does not depend on the network.

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include "test_common.h"

static void fail(const char *what, int err) {
    printf("FAIL %s: %s\n", what, err ? strerror(err) : "wrong value");
    failures_total++;
}

static void check_family(int family) {
    const char *name = family == AF_INET ? "inet" : "inet6";
    struct sockaddr_storage to;
    socklen_t to_len;
    memset(&to, 0, sizeof(to));
    if (family == AF_INET) {
        struct sockaddr_in *sin = (struct sockaddr_in *) &to;
        sin->sin_family = AF_INET;
        sin->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        to_len = sizeof(*sin);
    } else {
        struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *) &to;
        sin6->sin6_family = AF_INET6;
        sin6->sin6_addr = in6addr_loopback;
        to_len = sizeof(*sin6);
    }

    int fd = socket(family, SOCK_DGRAM, 0);
    if (fd < 0) {
        if (family == AF_INET6 && (errno == EAFNOSUPPORT || errno == EPROTONOSUPPORT)) {
            printf("udp_connect_port0: %s: no IPv6 sockets here, skipped\n", name);
            return;
        }
        fail("socket", errno);
        return;
    }

    char what[64];
    if (connect(fd, (struct sockaddr *) &to, to_len) != 0) {
        snprintf(what, sizeof(what), "%s connect to port 0", name);
        fail(what, errno);
        close(fd);
        return;
    }

    // The probe glibc relies on: a source address was chosen.
    struct sockaddr_storage me;
    socklen_t me_len = sizeof(me);
    memset(&me, 0, sizeof(me));
    snprintf(what, sizeof(what), "%s getsockname after port-0 connect", name);
    if (getsockname(fd, (struct sockaddr *) &me, &me_len) != 0) {
        fail(what, errno);
    } else if (family == AF_INET &&
               ((struct sockaddr_in *) &me)->sin_addr.s_addr != htonl(INADDR_LOOPBACK)) {
        fail(what, 0);
    } else if (family == AF_INET6 &&
               memcmp(&((struct sockaddr_in6 *) &me)->sin6_addr, &in6addr_loopback,
                      sizeof(in6addr_loopback)) != 0) {
        fail(what, 0);
    }

    struct sockaddr_storage peer;
    socklen_t peer_len = sizeof(peer);
    errno = 0;
    int rc = getpeername(fd, (struct sockaddr *) &peer, &peer_len);
    if (rc != -1 || errno != ENOTCONN) {
        printf("FAIL %s getpeername after port-0 connect: rc=%d errno=%d, want -1 ENOTCONN\n",
               name, rc, errno);
        failures_total++;
    }

    snprintf(what, sizeof(what), "%s send after port-0 connect", name);
    if (send(fd, "x", 1, 0) != 1)
        fail(what, errno);
    // On loopback Linux answers that datagram with port-unreachable, so the
    // NEXT operation may report ECONNREFUSED; off loopback nothing answers.
    // Either is Linux.
    snprintf(what, sizeof(what), "%s write after port-0 connect", name);
    errno = 0;
    if (write(fd, "x", 1) != 1 && errno != ECONNREFUSED)
        fail(what, errno);

    // A real destination afterwards: an ordinary connected socket again, its
    // datagrams delivered and its peer named.
    int rx = socket(family, SOCK_DGRAM, 0);
    struct sockaddr_storage rx_addr = to;
    socklen_t rx_len = to_len;
    struct timeval tv = { 2, 0 };
    setsockopt(rx, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (rx < 0 || bind(rx, (struct sockaddr *) &rx_addr, rx_len) != 0 ||
            getsockname(rx, (struct sockaddr *) &rx_addr, &rx_len) != 0) {
        snprintf(what, sizeof(what), "%s receiver", name);
        fail(what, errno);
    } else {
        snprintf(what, sizeof(what), "%s reconnect to a real port", name);
        if (connect(fd, (struct sockaddr *) &rx_addr, rx_len) != 0) {
            fail(what, errno);
        } else {
            peer_len = sizeof(peer);
            unsigned short want = family == AF_INET
                ? ((struct sockaddr_in *) &rx_addr)->sin_port
                : ((struct sockaddr_in6 *) &rx_addr)->sin6_port;
            unsigned short got = 0;
            if (getpeername(fd, (struct sockaddr *) &peer, &peer_len) == 0)
                got = family == AF_INET ? ((struct sockaddr_in *) &peer)->sin_port
                                        : ((struct sockaddr_in6 *) &peer)->sin6_port;
            if (got != want) {
                printf("FAIL %s getpeername after reconnect: port %u, want %u\n",
                       name, ntohs(got), ntohs(want));
                failures_total++;
            }
            char buf[8] = {0};
            snprintf(what, sizeof(what), "%s send after reconnect", name);
            if (send(fd, "ok", 2, 0) != 2)
                fail(what, errno);
            snprintf(what, sizeof(what), "%s datagram after reconnect", name);
            ssize_t n = recv(rx, buf, sizeof(buf), 0);
            if (n != 2 || memcmp(buf, "ok", 2) != 0)
                fail(what, n < 0 ? errno : 0);
        }
    }
    if (rx >= 0)
        close(rx);
    close(fd);
}

int main(void) {
    check_family(AF_INET);
    check_family(AF_INET6);
    return finish_suite("udp_connect_port0");
}
