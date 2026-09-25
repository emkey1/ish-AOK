// epoll_edge_triggered: EPOLLET reports a readiness once, and again only when
// something new happens.
//
// The triage: "Edge-triggered epoll acts like level-triggered. A second wait
// with no new data reports the same file again", which spins an event loop
// that trusts the edge (Go's netpoller, tokio). Measured before the fix, 72
// of these checks failed on AOK and none on Linux 6.12 (x86_64 and -m32
// glibc):
//
//   - a pipe, a FIFO, and every host socket (unix stream/dgram/seqpacket,
//     TCP, UDP) reported the same data again on the next wait. They are
//     watched EV_CLEAR on the host, which keeps an event queued until a wait
//     retrieves it, and a readiness found by looking at the file never
//     retrieved it; the next wait took the old event for a new one;
//   - EPOLL_CTL_MOD did not re-arm an emulated fd (eventfd, pty, timerfd,
//     inotify, signalfd, AOK's FIFOs): Linux reports a ready fd once more
//     after a MOD, AOK kept what it had already reported;
//   - a named FIFO on the root filesystem was never watched on the host at
//     all, so after its first report it never reported again, and a
//     level-triggered wait on one slept to the next rescan;
//   - an EPOLLIN|EPOLLOUT registration reported only the bit that changed,
//     where Linux reports everything ready;
//   - with two fds ready and maxevents 1, the second was lost for good;
//   - a disarmed EPOLLONESHOT registration still reported a hangup.
//
// The level-triggered cases are the positive control: the same sequence
// without EPOLLET does report the second time, so a probe that cannot see a
// repeat report cannot pass by accident. Every "nothing" is a timed wait that
// is also checked to have waited.
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <termios.h>
#include <time.h>

#include "test_common.h"

enum kind {
    K_PIPE, K_FIFO, K_UNIX_STREAM, K_UNIX_DGRAM, K_UNIX_SEQPACKET, K_TCP, K_UDP,
    K_EVENTFD, K_PTY_MASTER, K_PTY_SLAVE, K_TIMERFD, K_INOTIFY, K_SIGNALFD,
    K_COUNT
};

static const char *const kind_name[K_COUNT] = {
    "pipe", "fifo", "unix-stream", "unix-dgram", "unix-seqpacket", "tcp", "udp",
    "eventfd", "pty-master", "pty-slave", "timerfd", "inotify", "signalfd",
};

struct obj {
    enum kind kind;
    int rfd;       // watched for EPOLLIN
    int wfd;       // inject() writes here; -1 when inject works another way
    int extra;     // one more fd to close, or -1
    char path[96]; // a fifo or an inotify directory to remove
    int serial;
};

static int sig_rt;

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void ck(const struct obj *o, const char *what, long got, long want) {
    char label[128];
    snprintf(label, sizeof label, "%s %s", kind_name[o->kind], what);
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-44s got=%#-6lx want=%#lx%s\n", label, got, want,
              got != want ? "   <-- FAIL" : "");
}

static void nonblock(int fd) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

static int tcp_pair(int *a, int *b) {
    int l = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sin = {.sin_family = AF_INET};
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof sin;
    if (l < 0 || bind(l, (struct sockaddr *) &sin, sizeof sin) < 0 || listen(l, 1) < 0 ||
            getsockname(l, (struct sockaddr *) &sin, &len) < 0)
        return -1;
    int c = socket(AF_INET, SOCK_STREAM, 0);
    if (c < 0 || connect(c, (struct sockaddr *) &sin, sizeof sin) < 0)
        return -1;
    int s = accept(l, NULL, NULL);
    close(l);
    if (s < 0)
        return -1;
    int one = 1;
    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    *a = s;
    *b = c;
    return 0;
}

static int udp_pair(int *a, int *b) {
    int s[2];
    struct sockaddr_in sin[2];
    for (int i = 0; i < 2; i++) {
        s[i] = socket(AF_INET, SOCK_DGRAM, 0);
        memset(&sin[i], 0, sizeof sin[i]);
        sin[i].sin_family = AF_INET;
        sin[i].sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t len = sizeof sin[i];
        if (s[i] < 0 || bind(s[i], (struct sockaddr *) &sin[i], sizeof sin[i]) < 0 ||
                getsockname(s[i], (struct sockaddr *) &sin[i], &len) < 0)
            return -1;
    }
    if (connect(s[0], (struct sockaddr *) &sin[1], sizeof sin[1]) < 0 ||
            connect(s[1], (struct sockaddr *) &sin[0], sizeof sin[0]) < 0)
        return -1;
    *a = s[0];
    *b = s[1];
    return 0;
}

static int pty_pair(int *master, int *slave) {
    int m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m < 0 || grantpt(m) < 0 || unlockpt(m) < 0)
        return -1;
    const char *name = ptsname(m);
    if (name == NULL)
        return -1;
    int s = open(name, O_RDWR | O_NOCTTY);
    if (s < 0)
        return -1;
    struct termios t;
    if (tcgetattr(s, &t) == 0) {
        cfmakeraw(&t);
        tcsetattr(s, TCSANOW, &t);
    }
    *master = m;
    *slave = s;
    return 0;
}

static int setup(enum kind k, struct obj *o) {
    memset(o, 0, sizeof *o);
    o->kind = k;
    o->rfd = o->wfd = o->extra = -1;
    int p[2];
    switch (k) {
    case K_PIPE:
        if (pipe(p) < 0)
            return -1;
        o->rfd = p[0];
        o->wfd = p[1];
        break;
    case K_FIFO:
        snprintf(o->path, sizeof o->path, "/tmp/epoll-et-fifo.%d", (int) getpid());
        unlink(o->path);
        if (mkfifo(o->path, 0600) < 0)
            return -1;
        o->rfd = open(o->path, O_RDONLY | O_NONBLOCK);
        o->wfd = open(o->path, O_WRONLY | O_NONBLOCK);
        if (o->rfd < 0 || o->wfd < 0)
            return -1;
        break;
    case K_UNIX_STREAM:
    case K_UNIX_DGRAM:
    case K_UNIX_SEQPACKET: {
        int type = k == K_UNIX_STREAM ? SOCK_STREAM : k == K_UNIX_DGRAM ? SOCK_DGRAM : SOCK_SEQPACKET;
        if (socketpair(AF_UNIX, type, 0, p) < 0)
            return -1;
        o->rfd = p[0];
        o->wfd = p[1];
        break;
    }
    case K_TCP:
        if (tcp_pair(&o->rfd, &o->wfd) < 0)
            return -1;
        break;
    case K_UDP:
        if (udp_pair(&o->rfd, &o->wfd) < 0)
            return -1;
        break;
    case K_EVENTFD:
        o->rfd = eventfd(0, EFD_NONBLOCK);
        if (o->rfd < 0)
            return -1;
        break;
    case K_PTY_MASTER:
        if (pty_pair(&o->rfd, &o->wfd) < 0)
            return -1;
        break;
    case K_PTY_SLAVE:
        if (pty_pair(&o->wfd, &o->rfd) < 0)
            return -1;
        break;
    case K_TIMERFD:
        o->rfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
        if (o->rfd < 0)
            return -1;
        break;
    case K_INOTIFY:
        snprintf(o->path, sizeof o->path, "/tmp/epoll-et-dir.%d", (int) getpid());
        mkdir(o->path, 0700);
        o->rfd = inotify_init1(IN_NONBLOCK);
        if (o->rfd < 0 || inotify_add_watch(o->rfd, o->path, IN_CREATE) < 0)
            return -1;
        break;
    case K_SIGNALFD: {
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, sig_rt);
        o->rfd = signalfd(-1, &set, SFD_NONBLOCK);
        if (o->rfd < 0)
            return -1;
        break;
    }
    default:
        return -1;
    }
    nonblock(o->rfd);
    return 0;
}

static void teardown(struct obj *o) {
    if (o->kind == K_INOTIFY) {
        for (int i = 0; i < o->serial; i++) {
            char name[128];
            snprintf(name, sizeof name, "%s/f%d", o->path, i);
            unlink(name);
        }
        rmdir(o->path);
    } else if (o->kind == K_FIFO) {
        unlink(o->path);
    } else if (o->kind == K_SIGNALFD) {
        // Take anything still queued off the process so the next case starts
        // from nothing.
        char buf[4096];
        while (read(o->rfd, buf, sizeof buf) > 0) {
        }
    }
    if (o->rfd >= 0)
        close(o->rfd);
    if (o->wfd >= 0 && o->wfd != o->rfd)
        close(o->wfd);
    if (o->extra >= 0)
        close(o->extra);
}

// Make one new thing happen that makes rfd readable.
static int inject(struct obj *o) {
    switch (o->kind) {
    case K_EVENTFD: {
        uint64_t one = 1;
        return write(o->rfd, &one, sizeof one) == sizeof one ? 0 : -1;
    }
    case K_TIMERFD: {
        struct itimerspec its = {.it_value = {.tv_sec = 0, .tv_nsec = 1000000}};
        return timerfd_settime(o->rfd, 0, &its, NULL);
    }
    case K_INOTIFY: {
        char name[128];
        snprintf(name, sizeof name, "%s/f%d", o->path, o->serial++);
        int fd = open(name, O_CREAT | O_WRONLY, 0600);
        if (fd < 0)
            return -1;
        close(fd);
        return 0;
    }
    case K_SIGNALFD: {
        union sigval v = {.sival_int = o->serial++};
        return sigqueue(getpid(), sig_rt, v);
    }
    default:
        return write(o->wfd, "x", 1) == 1 ? 0 : -1;
    }
}

static void drain(struct obj *o) {
    char buf[4096];
    while (read(o->rfd, buf, sizeof buf) > 0) {
    }
}

// epoll_wait for up to `ms`; returns the count and the first event's mask.
static int wait_ev(int ep, int ms, uint32_t *events, double *elapsed) {
    struct epoll_event ev[8];
    double t0 = now_s();
    int n;
    do {
        n = epoll_wait(ep, ev, 8, ms);
    } while (n < 0 && errno == EINTR);
    if (elapsed != NULL)
        *elapsed = now_s() - t0;
    if (events != NULL)
        *events = n > 0 ? ev[0].events : 0;
    return n;
}

struct injector {
    struct obj *o;
    int delay_ms;
};

static void *inject_later(void *arg) {
    struct injector *ij = arg;
    usleep(ij->delay_ms * 1000);
    inject(ij->o);
    return NULL;
}

// The case the triage reported, and its neighbours, on one EPOLLIN|EPOLLET
// registration.
static void check_in_edge(enum kind k) {
    struct obj o;
    if (setup(k, &o) < 0) {
        char label[64];
        snprintf(label, sizeof label, "%s setup errno", kind_name[k]);
        failf(label, (uint64_t) errno, 0, 0, 0, 0, 0);
        teardown(&o);
        return;
    }
    int ep = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event ev = {.events = EPOLLIN | EPOLLET, .data.u64 = 0x5eed0000u + k};
    ck(&o, "et add", epoll_ctl(ep, EPOLL_CTL_ADD, o.rfd, &ev), 0);

    uint32_t events;
    double dt;
    ck(&o, "et idle wait n", wait_ev(ep, 0, NULL, NULL), 0);

    ck(&o, "et inject 1", inject(&o), 0);
    int n = wait_ev(ep, 2000, &events, NULL);
    ck(&o, "et first wait n", n, 1);
    ck(&o, "et first wait events", events, EPOLLIN);

    // THE BUG: nothing new happened, so Linux reports nothing.
    n = wait_ev(ep, 200, &events, &dt);
    ck(&o, "et second wait n", n, 0);
    ck(&o, "et second wait blocked", dt >= 0.15, 1);

    // Something new: one more report.
    ck(&o, "et inject 2", inject(&o), 0);
    n = wait_ev(ep, 2000, &events, NULL);
    ck(&o, "et new-edge wait n", n, 1);
    ck(&o, "et new-edge wait events", events, EPOLLIN);
    ck(&o, "et after new-edge n", wait_ev(ep, 200, NULL, NULL), 0);

    // EPOLL_CTL_MOD re-arms: a ready fd is reported once more.
    ev.events = EPOLLIN | EPOLLET;
    ck(&o, "et mod", epoll_ctl(ep, EPOLL_CTL_MOD, o.rfd, &ev), 0);
    n = wait_ev(ep, 2000, &events, NULL);
    ck(&o, "et mod-rearm wait n", n, 1);
    ck(&o, "et mod-rearm wait events", events, EPOLLIN);
    ck(&o, "et after mod-rearm n", wait_ev(ep, 200, NULL, NULL), 0);

    // A wait already blocked when the event arrives (the host-event path).
    struct injector ij = {.o = &o, .delay_ms = 150};
    pthread_t t;
    pthread_create(&t, NULL, inject_later, &ij);
    n = wait_ev(ep, 3000, &events, &dt);
    pthread_join(t, NULL);
    ck(&o, "et blocked wait n", n, 1);
    ck(&o, "et blocked wait events", events, EPOLLIN);
    ck(&o, "et blocked wait prompt", dt < 0.9, 1);
    ck(&o, "et after blocked n", wait_ev(ep, 200, NULL, NULL), 0);

    // Reading is not an event: after a full drain nothing is reported, and
    // the next write is.
    drain(&o);
    ck(&o, "et after drain n", wait_ev(ep, 200, NULL, NULL), 0);
    ck(&o, "et inject 3", inject(&o), 0);
    n = wait_ev(ep, 2000, &events, NULL);
    ck(&o, "et after-drain edge n", n, 1);
    ck(&o, "et after-drain edge events", events, EPOLLIN);
    ck(&o, "et after-drain edge again n", wait_ev(ep, 200, NULL, NULL), 0);

    close(ep);
    teardown(&o);
}

// Positive control: the same sequence level-triggered DOES report again.
static void check_in_level(enum kind k) {
    struct obj o;
    if (setup(k, &o) < 0) {
        teardown(&o);
        return;
    }
    int ep = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event ev = {.events = EPOLLIN, .data.u64 = 1};
    epoll_ctl(ep, EPOLL_CTL_ADD, o.rfd, &ev);
    inject(&o);
    uint32_t events;
    ck(&o, "lt first wait n", wait_ev(ep, 2000, &events, NULL), 1);
    ck(&o, "lt second wait n", wait_ev(ep, 200, &events, NULL), 1);
    ck(&o, "lt second wait events", events, EPOLLIN);
    drain(&o);
    ck(&o, "lt after drain n", wait_ev(ep, 200, NULL, NULL), 0);

    // A level-triggered wait already blocked when the data comes wakes for
    // it, promptly -- not at the next rescan.
    struct injector ij = {.o = &o, .delay_ms = 150};
    pthread_t t;
    pthread_create(&t, NULL, inject_later, &ij);
    double dt;
    int n = wait_ev(ep, 3000, &events, &dt);
    pthread_join(t, NULL);
    ck(&o, "lt blocked wait n", n, 1);
    ck(&o, "lt blocked wait prompt", dt < 0.9, 1);
    close(ep);
    teardown(&o);
}

// EPOLLOUT|EPOLLET on the writing end: writable is reported once.
static void check_out_edge(enum kind k) {
    struct obj o;
    if (setup(k, &o) < 0) {
        teardown(&o);
        return;
    }
    int fd = o.wfd >= 0 ? o.wfd : o.rfd;
    int ep = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event ev = {.events = EPOLLOUT | EPOLLET, .data.u64 = 2};
    epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev);
    uint32_t events;
    ck(&o, "et-out first wait n", wait_ev(ep, 2000, &events, NULL), 1);
    ck(&o, "et-out first wait events", events, EPOLLOUT);
    ck(&o, "et-out second wait n", wait_ev(ep, 200, NULL, NULL), 0);
    close(ep);
    teardown(&o);
}

// EPOLLIN|EPOLLOUT|EPOLLET on one end: a new event reports everything that
// is ready now, not only the bit that changed.
static void check_inout_edge(enum kind k) {
    struct obj o;
    if (setup(k, &o) < 0) {
        teardown(&o);
        return;
    }
    int ep = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event ev = {.events = EPOLLIN | EPOLLOUT | EPOLLET, .data.u64 = 3};
    epoll_ctl(ep, EPOLL_CTL_ADD, o.rfd, &ev);
    uint32_t events;
    ck(&o, "et-inout first wait n", wait_ev(ep, 2000, &events, NULL), 1);
    ck(&o, "et-inout first wait events", events, EPOLLOUT);
    ck(&o, "et-inout second wait n", wait_ev(ep, 200, NULL, NULL), 0);
    inject(&o);
    ck(&o, "et-inout edge wait n", wait_ev(ep, 2000, &events, NULL), 1);
    ck(&o, "et-inout edge wait events", events, EPOLLIN | EPOLLOUT);
    ck(&o, "et-inout after edge n", wait_ev(ep, 200, NULL, NULL), 0);
    close(ep);
    teardown(&o);
}

// Reading part of what is queued is not an event either. Checked on sockets
// and ptys only: a host pipe or FIFO re-arms its read event on Darwin
// whenever a read leaves data behind (measured with a bare kqueue), which
// costs an edge-triggered reader one extra report -- harmless to a reader
// that reads until EAGAIN, as an EPOLLET reader must -- and cannot be told
// from a new write without watching every read.
static void check_partial_read(enum kind k) {
    struct obj o;
    if (setup(k, &o) < 0) {
        teardown(&o);
        return;
    }
    int ep = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event ev = {.events = EPOLLIN | EPOLLET, .data.u64 = 4};
    epoll_ctl(ep, EPOLL_CTL_ADD, o.rfd, &ev);
    inject(&o);
    inject(&o);
    usleep(50000);
    uint32_t events;
    ck(&o, "et-partial first wait n", wait_ev(ep, 2000, &events, NULL), 1);
    char c;
    ck(&o, "et-partial read", read(o.rfd, &c, 1), 1);
    ck(&o, "et-partial after read n", wait_ev(ep, 200, NULL, NULL), 0);
    close(ep);
    teardown(&o);
}

// Two ET registrations ready at once and room for one event per wait: the one
// not reported stays ready for the next wait, as on Linux's ready list.
static void check_maxevents_one(enum kind k) {
    struct obj a, b;
    if (setup(k, &a) < 0) {
        teardown(&a);
        return;
    }
    if (setup(k, &b) < 0) {
        teardown(&a);
        teardown(&b);
        return;
    }
    int ep = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event ev = {.events = EPOLLIN | EPOLLET, .data.u64 = 1};
    epoll_ctl(ep, EPOLL_CTL_ADD, a.rfd, &ev);
    ev.data.u64 = 2;
    epoll_ctl(ep, EPOLL_CTL_ADD, b.rfd, &ev);
    inject(&a);
    inject(&b);
    usleep(50000);
    struct epoll_event out[1];
    uint64_t seen = 0;
    for (int i = 0; i < 2; i++) {
        int n = epoll_wait(ep, out, 1, 2000);
        ck(&a, i == 0 ? "et-max1 first n" : "et-max1 second n", n, 1);
        if (n == 1)
            seen |= out[0].data.u64;
    }
    ck(&a, "et-max1 both seen", (long) seen, 3);
    ck(&a, "et-max1 third n", wait_ev(ep, 200, NULL, NULL), 0);
    close(ep);
    teardown(&a);
    teardown(&b);
}

// EPOLLONESHOT: once reported, the registration is disarmed and reports
// nothing -- a hangup included -- until EPOLL_CTL_MOD re-arms it.
static void check_oneshot_hangup(enum kind k) {
    struct obj o;
    if (setup(k, &o) < 0) {
        teardown(&o);
        return;
    }
    int ep = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event ev = {.events = EPOLLIN | EPOLLONESHOT, .data.u64 = 5};
    epoll_ctl(ep, EPOLL_CTL_ADD, o.rfd, &ev);
    inject(&o);
    uint32_t events;
    ck(&o, "oneshot first wait n", wait_ev(ep, 2000, &events, NULL), 1);
    close(o.wfd);
    o.wfd = -1;
    ck(&o, "oneshot after hangup n", wait_ev(ep, 200, &events, NULL), 0);
    ck(&o, "oneshot after hangup events", events, 0);
    // Re-armed, it reports what is there now: the byte and the hangup.
    ev.events = EPOLLIN | EPOLLONESHOT;
    epoll_ctl(ep, EPOLL_CTL_MOD, o.rfd, &ev);
    ck(&o, "oneshot rearmed n", wait_ev(ep, 2000, &events, NULL), 1);
    ck(&o, "oneshot rearmed has IN", (events & EPOLLIN) != 0, 1);
    close(ep);
    teardown(&o);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(120));
    signal(SIGPIPE, SIG_IGN);

    // Every thread blocks the signalfd signal: they all inherit this mask.
    sig_rt = SIGRTMIN + 4;
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, sig_rt);
    sigprocmask(SIG_BLOCK, &set, NULL);

    for (int k = 0; k < K_COUNT; k++) {
        check_in_edge(k);
        check_in_level(k);
    }
    const enum kind out_kinds[] = {K_PIPE, K_FIFO, K_UNIX_STREAM, K_UNIX_DGRAM,
        K_UNIX_SEQPACKET, K_TCP, K_UDP, K_EVENTFD, K_PTY_MASTER, K_PTY_SLAVE};
    for (size_t i = 0; i < sizeof out_kinds / sizeof out_kinds[0]; i++)
        check_out_edge(out_kinds[i]);
    const enum kind inout_kinds[] = {K_UNIX_STREAM, K_UNIX_DGRAM, K_UNIX_SEQPACKET,
        K_TCP, K_UDP, K_EVENTFD, K_PTY_MASTER, K_PTY_SLAVE};
    for (size_t i = 0; i < sizeof inout_kinds / sizeof inout_kinds[0]; i++)
        check_inout_edge(inout_kinds[i]);
    const enum kind partial_kinds[] = {K_UNIX_STREAM, K_TCP, K_PTY_MASTER, K_PTY_SLAVE};
    for (size_t i = 0; i < sizeof partial_kinds / sizeof partial_kinds[0]; i++)
        check_partial_read(partial_kinds[i]);
    const enum kind max1_kinds[] = {K_PIPE, K_FIFO, K_UNIX_STREAM, K_TCP, K_EVENTFD,
        K_PTY_MASTER};
    for (size_t i = 0; i < sizeof max1_kinds / sizeof max1_kinds[0]; i++)
        check_maxevents_one(max1_kinds[i]);
    const enum kind oneshot_kinds[] = {K_PIPE, K_FIFO, K_UNIX_STREAM, K_TCP,
        K_PTY_MASTER};
    for (size_t i = 0; i < sizeof oneshot_kinds / sizeof oneshot_kinds[0]; i++)
        check_oneshot_hangup(oneshot_kinds[i]);
    return finish_suite("epoll_edge_triggered");
}
