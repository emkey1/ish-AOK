#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void fail(const char *why) {
    fprintf(stderr, "%s\n", why);
    exit(1);
}

static void expect_int_sockopt(int fd, int level, int optname, int value, const char *label) {
    if (setsockopt(fd, level, optname, &value, sizeof(value)) < 0)
        fail(label);
    int actual = 0;
    socklen_t actual_len = sizeof(actual);
    if (getsockopt(fd, level, optname, &actual, &actual_len) < 0)
        fail(label);
    if (actual_len != sizeof(actual))
        fail("unexpected sockopt length");
    if (actual != value)
        fail("sockopt value mismatch");
}

int main(void) {
    int udp4 = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp4 < 0)
        fail("socket AF_INET failed");

    expect_int_sockopt(udp4, IPPROTO_IP, IP_MTU_DISCOVER, 2, "IP_MTU_DISCOVER");
    expect_int_sockopt(udp4, IPPROTO_IP, IP_RECVERR, 1, "IP_RECVERR");

    int udp6 = socket(AF_INET6, SOCK_DGRAM, 0);
    if (udp6 >= 0) {
        expect_int_sockopt(udp6, IPPROTO_IPV6, IPV6_MTU_DISCOVER, 1, "IPV6_MTU_DISCOVER");
        expect_int_sockopt(udp6, IPPROTO_IPV6, IPV6_MTU, 1280, "IPV6_MTU");
        expect_int_sockopt(udp6, IPPROTO_IPV6, IPV6_RECVERR, 1, "IPV6_RECVERR");
        close(udp6);
    }

    int tcp = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp < 0)
        fail("socket TCP failed");

    static const char congestion[] = "reno";
    if (setsockopt(tcp, IPPROTO_TCP, TCP_CONGESTION, congestion, sizeof(congestion)) < 0)
        fail("TCP_CONGESTION set failed");

    // Linux reports TCP_CA_NAME_MAX (16) bytes, NUL-padded -- not strlen().
    // Verified on x86_64 glibc, Linux 6.12: a 32-byte buffer comes back with
    // optlen 16, and so does a 16-byte one. This used to assert strlen(), which
    // is what AOK did and what no Linux does.
    char actual[32];
    memset(actual, 0xaa, sizeof(actual));
    socklen_t actual_len = sizeof(actual);
    if (getsockopt(tcp, IPPROTO_TCP, TCP_CONGESTION, actual, &actual_len) < 0)
        fail("TCP_CONGESTION get failed");
    if (actual_len != 16)
        fail("TCP_CONGESTION length mismatch");
    if (strncmp(actual, congestion, sizeof(actual)) != 0)
        fail("TCP_CONGESTION value mismatch");

    expect_int_sockopt(tcp, IPPROTO_TCP, TCP_DEFER_ACCEPT, 7, "TCP_DEFER_ACCEPT");

    puts("sockopt state ok");

    close(tcp);
    close(udp4);
    return 0;
}
