/*
 * sock_conformance.c — self-checking regression lock for the socket/IPC
 * Linux-conformance fixes found by the differential harness
 * (tests/remote/corpus_sock, vs mint's real Linux). Each check encodes the
 * Linux-correct behavior directly (no oracle); exits 0 and prints
 * "sock_conformance: PASS" on success.
 *
 * Regression-locks, in particular:
 *   - eventfd EFD_SEMAPHORE accepted + read returns 1 / decrements by 1
 *     (was EINVAL on create, and counter-drain semantics)
 *   - poll(): a negative fd is ignored (was wrongly POLLNVAL); a closed fd is
 *     POLLNVAL AND counts toward the return value (was revents-only, returned 0)
 *   - epoll_ctl: adding the epoll to itself -> EINVAL; an unknown op -> EINVAL
 *     (unknown op used to be treated as MOD)
 *   - getsockopt(SO_ACCEPTCONN) reflects listen() state (Darwin returned
 *     ENOPROTOOPT through the passthrough)
 *   - send() on an unconnected AF_UNIX datagram socket -> ENOTCONN (was
 *     EDESTADDRREQ from Darwin)
 *   - recv(MSG_TRUNC) on a datagram returns the REAL datagram length
 *   - poll on a pipe read end with unread data + closed writer stays readable
 *     and reports POLLHUP (the host-poll layer used to scrub readiness)
 * plus baseline stream/datagram round trips, epoll level-vs-edge, peer creds.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "test_common.h"

#ifndef EFD_SEMAPHORE
#define EFD_SEMAPHORE 1
#endif

static void check(const char *label, long got, long exp) {
    int bad = got != exp;
    test_log_if(bad, "%s: got=%ld exp=%ld\n", label, got, exp);
    if (bad)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) exp, 0, 0);
}

static volatile sig_atomic_t alarm_fired;
static void on_alarm(int s) { (void) s; alarm_fired = 1; }
static void arm_alarm(int sec) {
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_alarm; sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);
    alarm_fired = 0; alarm(sec);
}

/* ---- eventfd: counter and semaphore modes ---- */
static void test_eventfd(void) {
    int fd = eventfd(0, EFD_NONBLOCK);
    check("efd.create", fd >= 0, 1);
    uint64_t v = 0;
    write(fd, &(uint64_t){1}, 8);
    write(fd, &(uint64_t){2}, 8);
    write(fd, &(uint64_t){3}, 8);
    ssize_t r = read(fd, &v, 8);
    check("efd.counter.drain_sum", r == 8 && v == 6, 1);
    close(fd);

    errno = 0;
    int sfd = eventfd(0, EFD_SEMAPHORE | EFD_NONBLOCK);
    check("efd.semaphore.create_ok", sfd >= 0, 1);
    if (sfd >= 0) {
        write(sfd, &(uint64_t){3}, 8);
        uint64_t a = 0, b = 0, c = 0;
        read(sfd, &a, 8); read(sfd, &b, 8); read(sfd, &c, 8);
        check("efd.semaphore.three_ones", a == 1 && b == 1 && c == 1, 1);
        errno = 0;
        uint64_t d = 0;
        ssize_t r4 = read(sfd, &d, 8);
        check("efd.semaphore.empty_eagain", r4 < 0 && errno == EAGAIN, 1);
        close(sfd);
    }
}

/* ---- poll: negative fd ignored, closed fd POLLNVAL counted ---- */
static void test_poll_nval(void) {
    /* a negative fd is skipped: rc 0, revents cleared */
    struct pollfd neg = { .fd = -1, .events = POLLIN, .revents = 0xff };
    int rn = poll(&neg, 1, 0);
    check("poll.negfd.rc", rn, 0);
    check("poll.negfd.revents", neg.revents, 0);

    /* a closed fd is POLLNVAL and counts toward the return value */
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    close(sv[0]);
    struct pollfd dead = { .fd = sv[0], .events = POLLIN, .revents = 0 };
    int rd = poll(&dead, 1, 0);
    check("poll.closed.rc", rd, 1);
    check("poll.closed.nval", (dead.revents & POLLNVAL) != 0, 1);
    close(sv[1]);
}

/* ---- epoll: self-add and unknown-op are EINVAL; level vs edge ---- */
static void test_epoll(void) {
    int ep = epoll_create1(0);
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = ep };
    errno = 0;
    int sr = epoll_ctl(ep, EPOLL_CTL_ADD, ep, &ev);   /* add self */
    check("epoll.self_add_einval", sr < 0 && errno == EINVAL, 1);

    int efd = eventfd(0, EFD_NONBLOCK);
    ev.data.fd = efd;
    errno = 0;
    int br = epoll_ctl(ep, 9999, efd, &ev);            /* bogus op */
    check("epoll.bad_op_einval", br < 0 && errno == EINVAL, 1);

    /* level-triggered: reported until drained */
    epoll_ctl(ep, EPOLL_CTL_ADD, efd, &ev);
    write(efd, &(uint64_t){1}, 8);
    struct epoll_event out[2];
    int l1 = epoll_wait(ep, out, 2, 0);
    int l2 = epoll_wait(ep, out, 2, 0);   /* not drained -> still ready */
    check("epoll.level_repeats", l1 == 1 && l2 == 1, 1);
    uint64_t drain; ssize_t dr = read(efd, &drain, 8); (void) dr;
    int l3 = epoll_wait(ep, out, 2, 0);
    check("epoll.level_drained", l3, 0);

    /* edge-triggered: one report per ready transition */
    struct epoll_event ee = { .events = EPOLLIN | EPOLLET, .data.fd = efd };
    epoll_ctl(ep, EPOLL_CTL_MOD, efd, &ee);
    write(efd, &(uint64_t){1}, 8);
    int e1 = epoll_wait(ep, out, 2, 0);
    int e2 = epoll_wait(ep, out, 2, 0);   /* no new edge */
    check("epoll.edge_once", e1 == 1 && e2 == 0, 1);
    close(efd); close(ep);
}

/* ---- SO_ACCEPTCONN reflects listen() ---- */
static void test_acceptconn(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET; sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(s, (struct sockaddr *) &sa, sizeof sa);
    int before = -1; socklen_t bl = sizeof before;
    getsockopt(s, SOL_SOCKET, SO_ACCEPTCONN, &before, &bl);
    check("acceptconn.before", before, 0);
    listen(s, 4);
    int after = -1; socklen_t al = sizeof after;
    getsockopt(s, SOL_SOCKET, SO_ACCEPTCONN, &after, &al);
    check("acceptconn.after", after, 1);
    close(s);
}

/* ---- AF_UNIX dgram send with no destination -> ENOTCONN ---- */
static void test_dgram_no_dest(void) {
    int s = socket(AF_UNIX, SOCK_DGRAM, 0);
    errno = 0;
    ssize_t n = send(s, "x", 1, 0);
    check("dgram.send_no_dest_enotconn", n < 0 && errno == ENOTCONN, 1);
    close(s);
}

/* ---- MSG_TRUNC on a datagram returns the real length ---- */
static void test_msg_trunc(void) {
    int sv[2];
    socketpair(AF_UNIX, SOCK_DGRAM, 0, sv);
    write(sv[0], "abcdefghij", 10);
    char buf[4];
    ssize_t n = recv(sv[1], buf, 4, MSG_TRUNC);
    check("msgtrunc.real_len", n, 10);
    /* the datagram was consumed despite truncation */
    errno = 0;
    ssize_t n2 = recv(sv[1], buf, 4, MSG_DONTWAIT);
    check("msgtrunc.consumed", n2 < 0 && errno == EAGAIN, 1);
    close(sv[0]); close(sv[1]);
}

/* ---- pipe poll: unread data + closed writer stays readable + HUP ---- */
static void test_pipe_poll_hup(void) {
    int p[2];
    if (pipe(p) < 0) { check("pipe.create", 0, 1); return; }
    write(p[1], "y", 1);
    close(p[1]);                  /* unread byte remains */
    struct pollfd pfd = { .fd = p[0], .events = POLLIN, .revents = 0 };
    int r = poll(&pfd, 1, 0);
    check("pipe.hup.ready", r, 1);
    check("pipe.hup.readable", (pfd.revents & POLLIN) != 0, 1);  /* data not hidden */
    check("pipe.hup.hup", (pfd.revents & POLLHUP) != 0, 1);
    close(p[0]);
}

/* ---- AF_UNIX stream socketpair: byte stream + self peer creds ---- */
static void test_unix_stream(void) {
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    write(sv[0], "ab", 2);
    write(sv[0], "cd", 2);
    char buf[8] = {0};
    ssize_t n = read(sv[1], buf, sizeof buf);
    check("ustream.coalesce", n == 4 && memcmp(buf, "abcd", 4) == 0, 1);
#ifdef SO_PEERCRED
    struct { pid_t pid; uid_t uid; gid_t gid; } uc;
    memset(&uc, 0, sizeof uc);
    socklen_t ul = sizeof uc;
    int gr = getsockopt(sv[0], SOL_SOCKET, SO_PEERCRED, &uc, &ul);
    check("ustream.peercred_self",
          gr == 0 && uc.pid == getpid() && uc.uid == geteuid(), 1);
#endif
    close(sv[0]); close(sv[1]);
}

/* ---- AF_INET TCP loopback round trip ---- */
static void test_inet_tcp(void) {
    int lst = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1; setsockopt(lst, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET; sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(lst, (struct sockaddr *) &sa, sizeof sa) < 0 || listen(lst, 4) < 0) {
        check("inet.setup", 0, 1); close(lst); return;
    }
    struct sockaddr_in bound; socklen_t bl = sizeof bound;
    getsockname(lst, (struct sockaddr *) &bound, &bl);

    int cli = socket(AF_INET, SOCK_STREAM, 0);
    arm_alarm(3);
    int cr = connect(cli, (struct sockaddr *) &bound, sizeof bound);
    int srv = accept(lst, NULL, NULL);
    alarm(0);
    check("inet.connect_accept", cr == 0 && srv >= 0, 1);
    if (srv >= 0) {
        write(cli, "hello", 5);
        char buf[8] = {0};
        arm_alarm(3);
        ssize_t n = read(srv, buf, sizeof buf);
        alarm(0);
        check("inet.data", n == 5 && memcmp(buf, "hello", 5) == 0, 1);
        close(srv);
    }
    close(cli); close(lst);
}

// A wildcard *destination* means "this host" on Linux: sendto(0.0.0.0:port)
// must reach a local listener. Darwin fails such sends with EHOSTUNREACH, so
// AOK rewrites the destination (fs/sock.c sockaddr_read). stress-ng --udp's
// client targets 0.0.0.0 and died instantly, stranding its server.
static void test_sendto_wildcard_dest(void) {
    int srv = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET; sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(srv, (struct sockaddr *) &sa, sizeof sa) < 0) {
        check("udp0000.setup", 0, 1); close(srv); return;
    }
    struct sockaddr_in bound; socklen_t bl = sizeof bound;
    getsockname(srv, (struct sockaddr *) &bound, &bl);
    struct sockaddr_in dst; memset(&dst, 0, sizeof dst);
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = htonl(INADDR_ANY); // 0.0.0.0, NOT loopback
    dst.sin_port = bound.sin_port;
    int cli = socket(AF_INET, SOCK_DGRAM, 0);
    ssize_t sr = sendto(cli, "ping", 4, 0, (struct sockaddr *) &dst, sizeof dst);
    check("udp0000.sendto", sr == 4, 1);
    if (sr == 4) {
        char buf[8] = {0};
        arm_alarm(3);
        ssize_t n = recv(srv, buf, sizeof buf, 0);
        alarm(0);
        check("udp0000.delivered", n == 4 && memcmp(buf, "ping", 4) == 0, 1);
    }
    close(cli); close(srv);
}

// Linux's ip_setsockopt accepts EITHER a single byte or an int for
// IP_MULTICAST_TTL / IP_MULTICAST_LOOP (optlen >= 1 reads one byte,
// optlen >= sizeof(int) reads an int); optlen 0 is EINVAL. avahi-daemon
// passes a uint8_t TTL -- AOK rejecting the 1-byte form made its IPv4
// mDNS socket setup fail and dropped it to IPv6-only mode.
static void test_multicast_optlen(void) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) { check("mcastlen.socket", 1, 0); return; }
    unsigned char ttl = 200;
    check("mcastlen.ttl_1byte", setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, 1), 0);
    int ttl_int = 100;
    check("mcastlen.ttl_int", setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, &ttl_int, sizeof(ttl_int)), 0);
    errno = 0;
    int r = setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, 0);
    check("mcastlen.ttl_0byte_einval", r == -1 && errno == EINVAL, 1);
    unsigned char loop = 1;
    check("mcastlen.loop_1byte", setsockopt(s, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, 1), 0);
    close(s);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    signal(SIGPIPE, SIG_IGN);
    test_multicast_optlen();
    test_eventfd();
    test_poll_nval();
    test_epoll();
    test_acceptconn();
    test_dgram_no_dest();
    test_msg_trunc();
    test_pipe_poll_hup();
    test_unix_stream();
    test_inet_tcp();
    test_sendto_wildcard_dest();
    return finish_suite("sock_conformance");
}
