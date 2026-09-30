// A bound UDP socket through an iOS suspension (fs/sockrestart.c). iOS
// defuncts every TCP and UDP socket of a suspended app; a defunct UDP socket
// polls readable and hung up for ever and every receive fails, so a daemon
// waiting on one spins -- chronyd's command sockets (127.0.0.1:323, [::1]:323)
// took a whole core on an iPad. A resume now rebuilds a bound datagram socket
// with its options, as it already rebuilt listeners.
//
// Run on a Mac CLI build, where the suspension is simulated:
//   zig cc -target aarch64-linux-musl -O2 -o udp_suspend_rebuild udp_suspend_rebuild.c
//   ISH_SOCKRESTART_AFTER=2 ISH_SOCKRESTART_TEST_DESTROY=defunct \
//       ./build/ish -f <root> /path/udp_suspend_rebuild
// It waits 4 s for the suspend and resume to pass, then checks, for IPv4 and
// IPv6: nothing is readable before anything is sent (a dead socket is), a
// datagram sent afterwards arrives, and it still carries its PKTINFO control
// message (the option survived the rebuild). Prints "ok" and exits 0.
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int failures;

static void fail(const char *what, const char *why) {
    printf("FAIL %s: %s\n", what, why);
    failures++;
}

static int bound(int family, struct sockaddr_storage *addr, socklen_t *len) {
    int one = 1;
    int fd = socket(family, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;
    memset(addr, 0, sizeof(*addr));
    if (family == AF_INET) {
        struct sockaddr_in *a = (struct sockaddr_in *) addr;
        a->sin_family = AF_INET;
        a->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        *len = sizeof(*a);
        setsockopt(fd, IPPROTO_IP, IP_PKTINFO, &one, sizeof(one));
    } else {
        struct sockaddr_in6 *a = (struct sockaddr_in6 *) addr;
        a->sin6_family = AF_INET6;
        a->sin6_addr = in6addr_loopback;
        *len = sizeof(*a);
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one));
        setsockopt(fd, IPPROTO_IPV6, IPV6_RECVPKTINFO, &one, sizeof(one));
    }
    if (bind(fd, (struct sockaddr *) addr, *len) < 0 ||
            getsockname(fd, (struct sockaddr *) addr, len) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void check(int fd, int family, struct sockaddr_storage *addr, socklen_t len) {
    const char *name = family == AF_INET ? "IPv4" : "IPv6";
    char what[64];

    // Nothing was sent, so nothing may be readable. A defunct socket is:
    // POLLIN|POLLHUP at once, for ever.
    struct pollfd p = {.fd = fd, .events = POLLIN};
    int n = poll(&p, 1, 0);
    snprintf(what, sizeof(what), "%s idle poll", name);
    if (n != 0) {
        char why[64];
        snprintf(why, sizeof(why), "poll=%d revents=%#x before any send", n, p.revents);
        fail(what, why);
        return;
    }

    int tx = socket(family, SOCK_DGRAM, 0);
    if (sendto(tx, "ping", 4, 0, (struct sockaddr *) addr, len) != 4) {
        snprintf(what, sizeof(what), "%s send", name);
        fail(what, strerror(errno));
        close(tx);
        return;
    }
    close(tx);

    p.revents = 0;
    n = poll(&p, 1, 2000);
    snprintf(what, sizeof(what), "%s poll after send", name);
    if (n != 1 || !(p.revents & POLLIN)) {
        fail(what, "not readable within 2 s");
        return;
    }

    char buf[16];
    char control[256];
    struct iovec iov = {buf, sizeof(buf)};
    struct msghdr msg = {
        .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = control, .msg_controllen = sizeof(control),
    };
    ssize_t got = recvmsg(fd, &msg, MSG_DONTWAIT);
    snprintf(what, sizeof(what), "%s recvmsg", name);
    if (got != 4 || memcmp(buf, "ping", 4) != 0) {
        fail(what, got < 0 ? strerror(errno) : "wrong datagram");
        return;
    }
    int want_level = family == AF_INET ? IPPROTO_IP : IPPROTO_IPV6;
    int want_type = family == AF_INET ? IP_PKTINFO : IPV6_PKTINFO;
    int found = 0;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c != NULL; c = CMSG_NXTHDR(&msg, c))
        if (c->cmsg_level == want_level && c->cmsg_type == want_type)
            found = 1;
    snprintf(what, sizeof(what), "%s PKTINFO", name);
    if (!found)
        fail(what, "no PKTINFO control message: the option did not survive");
}

int main(void) {
    struct sockaddr_storage a4, a6;
    socklen_t l4, l6;
    int s4 = bound(AF_INET, &a4, &l4);
    int s6 = bound(AF_INET6, &a6, &l6);
    if (s4 < 0 || s6 < 0) {
        printf("FAIL setup: %s\n", strerror(errno));
        return 1;
    }
    // ISH_SOCKRESTART_AFTER=2: the suspend lands at 2 s and the resume half
    // a second later.
    sleep(4);
    check(s4, AF_INET, &a4, l4);
    check(s6, AF_INET6, &a6, l6);
    if (failures)
        return 1;
    printf("ok\n");
    return 0;
}
