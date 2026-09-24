// What a socket reports when the host says "hung up" and Linux does not.
//
//   An AF_UNIX SOCK_DGRAM socketpair whose peer has closed is, on Linux, an
//   idle socket. Nothing is raised on it, it stays pointed at the dead peer,
//   and only its next send finds out. Darwin disconnects it at once and
//   leaves a pending ECONNRESET on it, which Linux never raises there. AOK
//   passed that through three ways, with nothing queued: epoll(EPOLLIN) said
//   readable, poll(events=0) said POLLERR, and recv said ECONNRESET. Linux
//   says nothing, 0 and EAGAIN. Whichever of them looks first takes the
//   error, so each one is checked first on a pair of its own.
//
//   Its sends were wrong too. Linux fails the first send ECONNREFUSED, which
//   is what disconnects it, then ENOTCONN; a sendto() to some other, live
//   address still goes through. AOK said ECONNRESET or ENOTCONN, depending on
//   who had looked first, write() said EDESTADDRREQ, and the sendto() failed.
//
//   After our OWN shutdown(SHUT_WR) on a stream socket, poll said
//   OUT|HUP|RDHUP (0x2014) where Linux says OUT (0x4). Darwin calls our own
//   half-close a hangup, and the zero-length send AOK used to tell a
//   half-close from a full one fails EPIPE for it too, because it asks about
//   OUR write direction, which is exactly the one we shut. A program that
//   shuts down writing and then polls for the reply was told the connection
//   was over before the reply came.
//
// Each "nothing" is checked with a positive control on the same kind of
// socket: the same probe, fed a state that DOES report, has to see it.
// Blocked waits are checked too, since a poll already waiting is answered by
// a different path (fs/poll.c rpe_events) from a poll made afterwards
// (sock_poll), and each wait's CPU cost is measured, since a host event that
// keeps firing for a state the guest is not told about is a spin.
//
// Measured against x86_64 glibc on Linux 6.12.
#define _GNU_SOURCE
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>

#include "test_common.h"

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-62s got=0x%-6lx want=0x%lx\n", label, got, want);
}

static double wall_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static double cpu_now(void) {
    struct rusage r;
    getrusage(RUSAGE_SELF, &r);
    return r.ru_utime.tv_sec + r.ru_utime.tv_usec / 1e6 +
           r.ru_stime.tv_sec + r.ru_stime.tv_usec / 1e6;
}

// revents, or 0 on a timeout. -1000 - errno on an error, which no revents is.
static int poll_wait(int fd, int want, int timeout_ms) {
    struct pollfd p = { fd, (short) want, 0 };
    int r = poll(&p, 1, timeout_ms);
    if (r < 0)
        return -1000 - errno;
    return r > 0 ? p.revents : 0;
}

static int ep_wait(int fd, int want, int timeout_ms) {
    int ep = epoll_create1(0);
    if (ep < 0)
        return -1000 - errno;
    struct epoll_event ev = { .events = (unsigned) want };
    if (epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev) < 0) {
        int e = errno;
        close(ep);
        return -1000 - e;
    }
    struct epoll_event out;
    int n = epoll_wait(ep, &out, 1, timeout_ms);
    int e = errno;
    close(ep);
    if (n < 0)
        return -1000 - e;
    return n > 0 ? (int) out.events : 0;
}

// recv's result as one number: the byte count, or -errno.
static long recv_now(int fd, int flags) {
    char b[8];
    errno = 0;
    ssize_t r = recv(fd, b, sizeof b, flags);
    return r >= 0 ? (long) r : -(long) errno;
}

static long so_error(int fd) {
    int e = -1;
    socklen_t l = sizeof e;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &l) < 0)
        return -1000 - errno;
    return e;
}

// ---- a thread that acts on the socket once the caller is blocked ----------
enum act { ACT_CLOSE, ACT_SEND_CLOSE, ACT_SHUT_WR };
struct later {
    int fd;
    enum act act;
};

static void *act_later(void *arg) {
    struct later *l = arg;
    struct timespec ts = { 0, 200 * 1000 * 1000 };
    nanosleep(&ts, NULL);
    if (l->act == ACT_SEND_CLOSE && send(l->fd, "x", 1, 0) != 1)
        failures_total++;
    if (l->act == ACT_SHUT_WR)
        shutdown(l->fd, SHUT_WR);
    else
        close(l->fd);
    return NULL;
}

// Waits on `fd` for `want` while a thread does `act` to `target` 200 ms in.
// Checks the value, and then EITHER that the wait was woken promptly (a
// wanted value) OR that it ran its whole timeout without spinning (0).
static void blocked(const char *what, int use_epoll, int fd, int want,
                    int target, enum act act, int expect) {
    struct later l = { target, act };
    pthread_t th;
    char label[128];
    if (pthread_create(&th, NULL, act_later, &l) != 0) {
        ck("pthread_create", -1, 0);
        return;
    }
    int timeout = expect ? 5000 : 1000;
    double c0 = cpu_now(), w0 = wall_now();
    int got = use_epoll ? ep_wait(fd, want, timeout) : poll_wait(fd, want, timeout);
    double wall = wall_now() - w0, cpu = cpu_now() - c0;
    pthread_join(th, NULL);
    snprintf(label, sizeof label, "%s, %s waiting", what, use_epoll ? "epoll" : "poll");
    ck(label, got, expect);
    long ms = (long) (wall * 1000);
    if (expect) {
        // ...woken BY the event: a rescan would find the same value later.
        snprintf(label, sizeof label, "  woke within 800 ms (took %ld)", ms);
        ck(label, ms < 800, 1);
    } else {
        // ...and not by it: a wait that returned early with nothing is a
        // wait the host woke for a state the guest is not told about.
        snprintf(label, sizeof label, "  ran its whole 1000 ms (took %ld)", ms);
        ck(label, ms >= 900, 1);
        // A cost is only a measurement over a wait that lasted.
        if (ms >= 900) {
            int pct = (int) (cpu * 100 / wall);
            snprintf(label, sizeof label, "  without spinning (%d%% CPU)", pct);
            ck(label, pct < 25, 1);
        }
    }
}

// ---- 1. a dgram pair whose peer has gone ----------------------------------

enum look { L_RECV, L_POLL0, L_EPOLLIN, L_POLLIN, L_SOERROR, L_POLLOUT, L_COUNT };
static const char *const look_names[] = {
    "recv(MSG_DONTWAIT) is EAGAIN",
    "poll(events=0) is 0",
    "epoll(EPOLLIN) reports nothing",
    "poll(POLLIN) is 0",
    "SO_ERROR is 0",
    "poll(POLLOUT) is OUT",
};

static void look(int fd, enum look which, const char *when) {
    char label[128];
    snprintf(label, sizeof label, "%s: %s", when, look_names[which]);
    switch (which) {
        case L_RECV: ck(label, recv_now(fd, MSG_DONTWAIT), -EAGAIN); break;
        case L_POLL0: ck(label, poll_wait(fd, 0, 150), 0); break;
        case L_EPOLLIN: ck(label, ep_wait(fd, EPOLLIN, 150), 0); break;
        case L_POLLIN: ck(label, poll_wait(fd, POLLIN, 150), 0); break;
        case L_SOERROR: ck(label, so_error(fd), 0); break;
        case L_POLLOUT: ck(label, poll_wait(fd, POLLOUT, 150), POLLOUT); break;
        default: break;
    }
}

static void dgram_peer_gone(void) {
    // Every observer, each one FIRST on a pair of its own: on Darwin the
    // first to look takes the pending error, and the rest see a quiet socket
    // whatever AOK does with it.
    for (int first = 0; first < L_COUNT; first++) {
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) != 0) {
            ck("dgram socketpair", -1, 0);
            return;
        }
        close(sv[1]);
        char when[96];
        snprintf(when, sizeof when, "dgram peer gone, %s first", look_names[first]);
        for (int i = 0; i < L_COUNT; i++)
            look(sv[0], (first + i) % L_COUNT, i == 0 ? when : "  then");
        close(sv[0]);
    }

    // Positive control: the same probes on the same kind of socket see a
    // datagram the peer left behind. Then the survivor drains it, and has
    // to go quiet -- Darwin raises the error only after the data is read.
    {
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0) {
            if (send(sv[1], "x", 1, 0) != 1)
                failures_total++;
            close(sv[1]);
            ck("dgram peer sent then went: poll(POLLIN) is IN",
               poll_wait(sv[0], POLLIN, 150), POLLIN);
            ck("  epoll(EPOLLIN) is IN", ep_wait(sv[0], EPOLLIN, 150), EPOLLIN);
            ck("  recv gets the datagram", recv_now(sv[0], MSG_DONTWAIT), 1);
            for (int i = 0; i < L_COUNT; i++)
                look(sv[0], i, "  drained");
            close(sv[0]);
        }
    }

    // Positive control for poll(events=0): it CAN report on a dgram socket.
    // A socket shut down both ways is a hangup, and says so unasked.
    {
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0) {
            shutdown(sv[0], SHUT_RDWR);
            ck("dgram shut down both ways: poll(events=0) is HUP",
               poll_wait(sv[0], 0, 150), POLLHUP);
            close(sv[0]);
            close(sv[1]);
        }
    }

    // Sends: ECONNREFUSED once, then ENOTCONN, whoever looked before. The
    // positive control is the same socket, which was connected: a socket
    // that never was gets ENOTCONN from the start.
    for (int first = -1; first < L_COUNT; first++) {
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) != 0)
            continue;
        close(sv[1]);
        char label[128];
        if (first >= 0)
            look(sv[0], first, "dgram peer gone, before the sends");
        snprintf(label, sizeof label, "dgram peer gone, %s: send is ECONNREFUSED",
                 first < 0 ? "nothing looked first" : "then");
        errno = 0;
        ck(label, send(sv[0], "x", 1, MSG_DONTWAIT) < 0 ? -errno : 0, -ECONNREFUSED);
        errno = 0;
        ck("  then ENOTCONN", send(sv[0], "x", 1, MSG_DONTWAIT) < 0 ? -errno : 0, -ENOTCONN);
        errno = 0;
        ck("  and write agrees", write(sv[0], "x", 1) < 0 ? -errno : 0, -ENOTCONN);
        close(sv[0]);
    }
    {
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0) {
            close(sv[1]);
            errno = 0;
            ck("dgram peer gone: write first is ECONNREFUSED too",
               write(sv[0], "x", 1) < 0 ? -errno : 0, -ECONNREFUSED);
            close(sv[0]);
        }
        int s = socket(AF_UNIX, SOCK_DGRAM, 0);
        errno = 0;
        ck("a dgram socket never connected: send is ENOTCONN",
           send(s, "x", 1, MSG_DONTWAIT) < 0 ? -errno : 0, -ENOTCONN);
        errno = 0;
        ck("  and so is write", write(s, "x", 1) < 0 ? -errno : 0, -ENOTCONN);
        close(s);
    }
    // ...and a datagram to some other, live address is still delivered, and
    // leaves the dead association for the next plain send to report.
    {
        struct sockaddr_un a = { .sun_family = AF_UNIX };
        snprintf(a.sun_path, sizeof a.sun_path, "/tmp/shutwr-dgram-%d", (int) getpid());
        unlink(a.sun_path);
        int rx = socket(AF_UNIX, SOCK_DGRAM, 0);
        int sv[2];
        if (rx >= 0 && bind(rx, (struct sockaddr *) &a, sizeof a) == 0 &&
                socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0) {
            close(sv[1]);
            ck("dgram peer gone: sendto a live address delivers",
               sendto(sv[0], "x", 1, 0, (struct sockaddr *) &a, sizeof a), 1);
            ck("  and it arrives", recv_now(rx, MSG_DONTWAIT), 1);
            errno = 0;
            ck("  then a plain send is ECONNREFUSED",
               send(sv[0], "x", 1, MSG_DONTWAIT) < 0 ? -errno : 0, -ECONNREFUSED);
            close(sv[0]);
        } else {
            ck("bound receiver", -1, 0);
        }
        if (rx >= 0)
            close(rx);
        unlink(a.sun_path);
    }
    // The same for a socket connect()ed to a named one that then closes.
    {
        struct sockaddr_un a = { .sun_family = AF_UNIX };
        snprintf(a.sun_path, sizeof a.sun_path, "/tmp/shutwr-dgram-c-%d", (int) getpid());
        unlink(a.sun_path);
        int rx = socket(AF_UNIX, SOCK_DGRAM, 0);
        int c = socket(AF_UNIX, SOCK_DGRAM, 0);
        if (rx >= 0 && c >= 0 && bind(rx, (struct sockaddr *) &a, sizeof a) == 0 &&
                connect(c, (struct sockaddr *) &a, sizeof a) == 0) {
            ck("connected dgram, server alive: send delivers", send(c, "x", 1, 0), 1);
            ck("  and it arrives", recv_now(rx, MSG_DONTWAIT), 1);
            close(rx);
            rx = -1;
            ck("connected dgram, server gone: poll(events=0) is 0", poll_wait(c, 0, 150), 0);
            ck("  epoll(EPOLLIN) reports nothing", ep_wait(c, EPOLLIN, 150), 0);
            ck("  recv(MSG_DONTWAIT) is EAGAIN", recv_now(c, MSG_DONTWAIT), -EAGAIN);
            ck("  SO_ERROR is 0", so_error(c), 0);
            errno = 0;
            ck("  send is ECONNREFUSED", send(c, "x", 1, MSG_DONTWAIT) < 0 ? -errno : 0,
               -ECONNREFUSED);
            errno = 0;
            ck("  then ENOTCONN", send(c, "x", 1, MSG_DONTWAIT) < 0 ? -errno : 0, -ENOTCONN);
        } else {
            ck("connected dgram pair", -1, 0);
        }
        if (rx >= 0)
            close(rx);
        if (c >= 0)
            close(c);
        unlink(a.sun_path);
    }

    // A blocking recv waits, as on any idle socket, until SO_RCVTIMEO. It
    // used to fail at once with ECONNRESET.
    {
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0) {
            struct timeval tv = { 0, 400 * 1000 };
            setsockopt(sv[0], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            if (send(sv[1], "x", 1, 0) != 1)
                failures_total++;
            close(sv[1]);
            double w0 = wall_now();
            ck("dgram peer gone: a blocking recv still gets what was sent",
               recv_now(sv[0], 0), 1);
            ck("  at once", wall_now() - w0 < 0.3, 1);
            w0 = wall_now();
            double c0 = cpu_now();
            ck("  and then waits out SO_RCVTIMEO: EAGAIN", recv_now(sv[0], 0), -EAGAIN);
            double wall = wall_now() - w0, cpu = cpu_now() - c0;
            char label[96];
            snprintf(label, sizeof label, "  after the whole 400 ms (took %ld)",
                     (long) (wall * 1000));
            ck(label, wall >= 0.35, 1);
            // A cost is only a measurement over a wait that lasted.
            if (wall >= 0.35) {
                snprintf(label, sizeof label, "  without spinning (%d%% CPU)",
                         (int) (cpu * 100 / wall));
                ck(label, cpu * 100 / wall < 25, 1);
            }
            close(sv[0]);
        }
    }

    // The same close, made while the survivor is already waiting. Nothing
    // wakes it, and it must not spin while the host keeps saying otherwise.
    // Positive control: a peer that sends before it goes does wake it.
    for (int use_epoll = 0; use_epoll < 2; use_epoll++) {
        for (int sends = 0; sends < 2; sends++) {
            int sv[2];
            if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) != 0)
                continue;
            blocked(sends ? "dgram peer sends then goes" : "dgram peer goes",
                    use_epoll, sv[0], use_epoll ? EPOLLIN : POLLIN, sv[1],
                    sends ? ACT_SEND_CLOSE : ACT_CLOSE, sends ? POLLIN : 0);
            close(sv[0]);
        }
    }
}

// ---- 2. our own shutdown(SHUT_WR) is not a hangup -------------------------

// A connected pair of the given kind: a unix socketpair, or TCP on loopback.
static int make_pair(int kind, int sv[2]) {
    if (kind == 0)
        return socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    if (kind == 1)
        return socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv);
    int l = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr = { htonl(INADDR_LOOPBACK) } };
    socklen_t al = sizeof a;
    if (l < 0 || bind(l, (struct sockaddr *) &a, sizeof a) < 0 || listen(l, 1) < 0 ||
            getsockname(l, (struct sockaddr *) &a, &al) < 0) {
        if (l >= 0)
            close(l);
        return -1;
    }
    sv[0] = socket(AF_INET, SOCK_STREAM, 0);
    if (sv[0] < 0 || connect(sv[0], (struct sockaddr *) &a, sizeof a) < 0) {
        close(l);
        return -1;
    }
    sv[1] = accept(l, NULL, NULL);
    close(l);
    return sv[1] < 0 ? -1 : 0;
}

static void own_shut_wr(void) {
    static const char *const kinds[] = { "stream", "seqpacket", "tcp" };
    const int all = POLLIN | POLLOUT | POLLRDHUP;
    for (int k = 0; k < 3; k++) {
        int sv[2];
        char label[128];
        if (make_pair(k, sv) != 0) {
            snprintf(label, sizeof label, "%s pair", kinds[k]);
            ck(label, -1, 0);
            continue;
        }
        ck("shutdown(SHUT_WR)", shutdown(sv[0], SHUT_WR), 0);
        snprintf(label, sizeof label, "%s after our own SHUT_WR: poll(IN|OUT|RDHUP) is OUT", kinds[k]);
        ck(label, poll_wait(sv[0], all, 150), POLLOUT);
        ck("  epoll(IN|OUT|RDHUP) is OUT", ep_wait(sv[0], all, 150), EPOLLOUT);
        ck("  poll(IN|RDHUP) is 0", poll_wait(sv[0], POLLIN | POLLRDHUP, 150), 0);
        ck("  poll(events=0) is 0", poll_wait(sv[0], 0, 150), 0);
        ck("  epoll(EPOLLIN) reports nothing", ep_wait(sv[0], EPOLLIN, 150), 0);
        // Positive controls, on this very socket. The peer does see our
        // half-close, as RDHUP and not HUP...
        ck("  the peer: poll(IN|OUT|RDHUP) is IN|OUT|RDHUP",
           poll_wait(sv[1], POLLRDHUP, 1000) ? poll_wait(sv[1], all, 150) : 0,
           POLLIN | POLLOUT | POLLRDHUP);
        // ...and once the peer shuts its side too, both directions are down
        // and ours is a hangup after all.
        shutdown(sv[1], SHUT_WR);
        ck("  once the peer shuts its side too: IN|OUT|RDHUP|HUP",
           poll_wait(sv[0], POLLRDHUP, 1000) ? poll_wait(sv[0], all, 150) : 0,
           POLLIN | POLLOUT | POLLRDHUP | POLLHUP);
        ck("  and epoll agrees", ep_wait(sv[0], all, 150),
           EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLHUP);
        close(sv[0]);
        close(sv[1]);
    }

    // A wait already in progress when we shut our own side: nothing it asked
    // about has happened, so it runs its timeout, without spinning on the
    // host's EOF. Positive control: the peer closing does wake it, with the
    // full answer.
    for (int k = 0; k < 3; k++) {
        for (int use_epoll = 0; use_epoll < 2; use_epoll++) {
            int sv[2];
            char what[96];
            if (make_pair(k, sv) != 0)
                continue;
            snprintf(what, sizeof what, "%s, our own SHUT_WR while (IN|RDHUP)", kinds[k]);
            blocked(what, use_epoll, sv[0], POLLIN | POLLRDHUP, sv[0], ACT_SHUT_WR, 0);
            snprintf(what, sizeof what, "%s, then the peer closes: IN|RDHUP|HUP", kinds[k]);
            blocked(what, use_epoll, sv[0], POLLIN | POLLRDHUP, sv[1], ACT_CLOSE,
                    POLLIN | POLLRDHUP | POLLHUP);
            close(sv[0]);
        }
    }
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(120));
    dgram_peer_gone();
    own_shut_wr();
    return finish_suite("poll_shutwr_dgram");
}
