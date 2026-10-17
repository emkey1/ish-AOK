/*
 * netlink_blocking_recv.c -- a BLOCKING receive on an AF_NETLINK socket with
 * nothing queued must wait, the way Linux's skb_recv_datagram does, instead of
 * failing at once.
 *
 * AOK emulates netlink in-process (real_fd < 0). Replies to a request are
 * queued synchronously inside sendmsg, so request/response users never block
 * on an empty queue. A program waiting for multicast notifications does, and
 * every receive path -- recv/recvfrom, recvmsg and read -- answered an empty
 * queue with EAGAIN whatever O_NONBLOCK and MSG_DONTWAIT said. Measured with
 * the probe this test grew from (bind RTMGRP_LINK, alarm, recv): Linux blocks
 * until the signal and reports EINTR after 1.00s, AOK reported EAGAIN after
 * 0.00s. iproute2's rtnl_listen() retries on EAGAIN, so `ip monitor` spun.
 *
 * What Linux does, and what is checked here (elapsed time on every one, never
 * the errno alone -- an EINTR or EAGAIN that arrives in 0ms is the bug):
 *   - a blocking receive waits for a signal: EINTR once a handler without
 *     SA_RESTART has run, on every receive path, and on a socket subscribed
 *     to RTMGRP_LINK exactly like the probe
 *   - a handler WITH SA_RESTART restarts the receive, which then returns the
 *     message another thread's request queued (the wake is the queueing, not
 *     a periodic re-check)
 *   - MSG_DONTWAIT, SOCK_NONBLOCK and fcntl(O_NONBLOCK) still fail at once
 *     with EAGAIN, and clearing O_NONBLOCK makes the socket block again
 *   - SO_RCVTIMEO reads back as set and ends the wait with EAGAIN after the
 *     timeout, on every receive path; a signal inside a timed wait is EINTR
 *     even with SA_RESTART (signal(7): a timed socket wait is never restarted)
 *   - SO_RCVTIMEO follows sock_set_timeout: the 64-bit _NEW layout, EINVAL for
 *     a short optlen, EDOM for a bad microsecond field, a negative timeout
 *     that reads back as zero and never waits; SO_SNDTIMEO reads back too, and
 *     a short getsockopt buffer truncates. AOK dropped both options on netlink
 *     sockets and answered getsockopt with EBADF.
 *   - read() of zero bytes returns 0 at once and leaves a queued message
 *     alone (sock_read_iter answers it before it looks at the queue)
 *   - other threads of the process running guest code and mapping memory do
 *     not end the wait. AOK pokes such threads when the address space changes,
 *     and the poke came back from a receive as EINTR with no signal sent.
 *   - a request's reply wakes poll and epoll_wait in another thread at once.
 *     AOK queued it silently, and the poller saw it ~700ms later, at its
 *     periodic recheck.
 *   - every GET is answered: a dump of a table AOK does not keep
 *     (RTM_GETNEIGH, RTM_GETRULE) ends with NLMSG_DONE. AOK sent nothing, so
 *     once a receive waited, `ip neigh` and `ip rule` waited for ever.
 *
 * Checked against Linux 6.12 (Devuan 6, x86_64 and -m32, unprivileged): all
 * hold.
 *
 * Exits 0 and prints "netlink_blocking_recv: PASS" on success.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>

#include "test_common.h"

/* Self-contained netlink ABI, as netlink_route.c: Alpine has no linux-headers. */
#ifndef AF_NETLINK
#define AF_NETLINK 16
#endif
#define NETLINK_ROUTE_ 0
#define RTMGRP_LINK_ 1
#define NLMSG_NOOP_ 1
#define NLMSG_ERROR_ 2
#define NLMSG_DONE_ 3
#define NLM_F_REQUEST_ 0x001
#define NLM_F_ACK_ 0x004
#define NLM_F_DUMP_ 0x300
/* The kernel's option numbers, not libc's: SO_RCVTIMEO is either, depending on
 * the libc's time_t, and these tests mean one layout in particular. */
#if defined(__powerpc__)
#define SO_RCVTIMEO_OLD_ 18    /* powerpc has 16-21 in another order */
#define SO_SNDTIMEO_OLD_ 19
#else
#define SO_RCVTIMEO_OLD_ 20
#define SO_SNDTIMEO_OLD_ 21
#endif
#define SO_RCVTIMEO_NEW_ 66
#define RTM_NEWLINK_ 16
#define RTM_DELLINK_ 17
#define RTM_GETLINK_ 18
#define RTM_GETNEIGH_ 30
#define RTM_GETRULE_ 34

struct nl_hdr {
    uint32_t nlmsg_len;
    uint16_t nlmsg_type;
    uint16_t nlmsg_flags;
    uint32_t nlmsg_seq;
    uint32_t nlmsg_pid;
};
struct nl_sockaddr {
    uint16_t nl_family;
    uint16_t nl_pad;
    uint32_t nl_pid;
    uint32_t nl_groups;
};

#define NL_ALIGN(len) (((len) + 3U) & ~3U)

/* The signal arrives this long into a wait that must be interrupted. */
#define INTERRUPT_MS 250
/* SO_RCVTIMEO under test. A whole number of jiffies at HZ 100, 250 and 1000,
 * so Linux reads it back unrounded. */
#define RCVTIMEO_MS 300

static volatile sig_atomic_t interrupt_hits;
static volatile sig_atomic_t restart_hits;

static void on_interrupt(int sig) {
    (void) sig;
    interrupt_hits++;
}

static void on_restart(int sig) {
    (void) sig;
    restart_hits++;
}

static timer_t interrupt_timer; /* SIGUSR1, handler without SA_RESTART */
static timer_t restart_timer;   /* SIGUSR2, handler with SA_RESTART */

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* How late a wait may end and still count, scaled like the watchdogs for a
 * heavily loaded run. Only the upper bounds use it: the lower bounds are what
 * this test is about, and a loaded machine cannot make a wait end EARLY. */
static long slack_ms(long base) {
    return base * (long) test_watchdog_secs(1);
}

static void timer_arm(timer_t t, long ms) {
    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    its.it_value.tv_sec = ms / 1000;
    its.it_value.tv_nsec = (ms % 1000) * 1000000L;
    timer_settime(t, 0, &its, NULL);
}

static void timer_disarm(timer_t t) {
    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    timer_settime(t, 0, &its, NULL);
}

static void check(int ok, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (!ok || test_verbose) {
        printf("%s ", ok ? "ok" : "FAIL");
        vprintf(fmt, ap);
        printf("\n");
    }
    va_end(ap);
    if (!ok)
        failures_total++;
}

enum method { M_RECV, M_RECVFROM, M_RECVMSG, M_READ };
static const char *const method_names[] = {"recv", "recvfrom", "recvmsg", "read"};

static ssize_t receive(int fd, enum method m, int flags, char *buf, size_t len) {
    struct nl_sockaddr from;
    socklen_t from_len = sizeof(from);
    struct iovec iov = {.iov_base = buf, .iov_len = len};
    struct msghdr msg;
    switch (m) {
    case M_RECV:
        return recv(fd, buf, len, flags);
    case M_RECVFROM:
        return recvfrom(fd, buf, len, flags, (struct sockaddr *) &from, &from_len);
    case M_RECVMSG:
        memset(&msg, 0, sizeof(msg));
        msg.msg_name = &from;
        msg.msg_namelen = sizeof(from);
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        return recvmsg(fd, &msg, flags);
    case M_READ:
        return read(fd, buf, len);
    }
    return -1;
}

static int nl_open(int type_flags, uint32_t groups) {
    int fd = socket(AF_NETLINK, SOCK_RAW | type_flags, NETLINK_ROUTE_);
    if (fd < 0)
        return -1;
    struct nl_sockaddr sa = {.nl_family = AF_NETLINK, .nl_groups = groups};
    if (bind(fd, (struct sockaddr *) &sa, sizeof(sa)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* One request, exactly one reply: the ACK (NLMSG_ERROR, error 0). */
static ssize_t send_noop_ack(int fd, uint32_t seq) {
    struct nl_hdr req = {
        .nlmsg_len = sizeof(req),
        .nlmsg_type = NLMSG_NOOP_,
        .nlmsg_flags = NLM_F_REQUEST_ | NLM_F_ACK_,
        .nlmsg_seq = seq,
    };
    struct nl_sockaddr kernel = {.nl_family = AF_NETLINK};
    return sendto(fd, &req, sizeof(req), 0, (struct sockaddr *) &kernel, sizeof(kernel));
}

/* A dump request with an AF_UNSPEC rtgenmsg, as busybox's ip sends. */
static ssize_t send_dump(int fd, uint16_t type, uint32_t seq) {
    char req[NL_ALIGN(sizeof(struct nl_hdr)) + 4];
    memset(req, 0, sizeof(req));
    struct nl_hdr *nlh = (struct nl_hdr *) req;
    nlh->nlmsg_len = sizeof(req);
    nlh->nlmsg_type = type;
    nlh->nlmsg_flags = NLM_F_REQUEST_ | NLM_F_DUMP_;
    nlh->nlmsg_seq = seq;
    struct nl_sockaddr kernel = {.nl_family = AF_NETLINK};
    return sendto(fd, req, sizeof(req), 0, (struct sockaddr *) &kernel, sizeof(kernel));
}

static ssize_t send_link_dump(int fd, uint32_t seq) {
    return send_dump(fd, RTM_GETLINK_, seq);
}

/* Does this receive buffer end the dump? */
static int batch_has_done(const char *buf, ssize_t len) {
    ssize_t off = 0;
    while (off + (ssize_t) sizeof(struct nl_hdr) <= len) {
        const struct nl_hdr *h = (const struct nl_hdr *) (buf + off);
        if (h->nlmsg_len < sizeof(*h))
            return 0;
        if (h->nlmsg_type == NLMSG_DONE_ || h->nlmsg_type == NLMSG_ERROR_)
            return 1;
        off += NL_ALIGN(h->nlmsg_len);
    }
    return 0;
}

/* Drain a dump whose first batch has already been read. Every receive here is
 * a blocking one with the reply known to be on its way, guarded so a lost
 * part fails instead of hanging. */
static void drain_dump(int fd, char *buf, size_t cap, int done, const char *what) {
    int batches = 0;
    while (!done && batches++ < 1000) {
        timer_arm(interrupt_timer, 3000);
        ssize_t r = recv(fd, buf, cap, 0);
        int err = errno;
        timer_disarm(interrupt_timer);
        if (r <= 0) {
            check(0, "%s: drain recv=%zd errno=%s", what, r, strerror(err));
            return;
        }
        done = batch_has_done(buf, r);
    }
    check(done, "%s: dump ended with NLMSG_DONE", what);
}

/* A receive with nothing queued and nothing on its way: only the signal can
 * end it. */
static void expect_interrupted(int fd, enum method m, const char *label) {
    char buf[4096];
    interrupt_hits = 0;
    long start = now_ms();
    timer_arm(interrupt_timer, INTERRUPT_MS);
    errno = 0;
    ssize_t r = receive(fd, m, 0, buf, sizeof(buf));
    int err = errno;
    long elapsed = now_ms() - start;
    timer_disarm(interrupt_timer);
    check(r == -1 && err == EINTR && interrupt_hits == 1 &&
              elapsed >= INTERRUPT_MS - 50 && elapsed <= INTERRUPT_MS + slack_ms(1500),
          "%s %s: blocking, signal at %dms: ret=%zd errno=%s handler=%d after %ldms "
          "(want -1 EINTR handler=1 after ~%dms)",
          label, method_names[m], INTERRUPT_MS, r, strerror(err), (int) interrupt_hits,
          elapsed, INTERRUPT_MS);
}

/* A receive that must fail at once. Guarded by a signal so a receive that
 * blocks by mistake fails the check rather than hanging the test. */
static void expect_eagain_now(int fd, enum method m, int flags, const char *label) {
    char buf[4096];
    long start = now_ms();
    timer_arm(interrupt_timer, 2000);
    errno = 0;
    ssize_t r = receive(fd, m, flags, buf, sizeof(buf));
    int err = errno;
    long elapsed = now_ms() - start;
    timer_disarm(interrupt_timer);
    check(r == -1 && err == EAGAIN && elapsed < 500,
          "%s %s: ret=%zd errno=%s after %ldms (want -1 EAGAIN at once)",
          label, method_names[m], r, strerror(err), elapsed);
}

/* A receive on a socket with SO_RCVTIMEO: the timeout ends it. The guard at
 * 3s turns a timeout that never fires into a failure. */
static void expect_timed_out(int fd, enum method m, const char *label) {
    char buf[4096];
    interrupt_hits = 0;
    long start = now_ms();
    timer_arm(interrupt_timer, 3000);
    errno = 0;
    ssize_t r = receive(fd, m, 0, buf, sizeof(buf));
    int err = errno;
    long elapsed = now_ms() - start;
    timer_disarm(interrupt_timer);
    check(r == -1 && err == EAGAIN && interrupt_hits == 0 &&
              elapsed >= RCVTIMEO_MS - 50 && elapsed <= RCVTIMEO_MS + slack_ms(1500),
          "%s %s: SO_RCVTIMEO %dms: ret=%zd errno=%s after %ldms (want -1 EAGAIN after ~%dms)",
          label, method_names[m], RCVTIMEO_MS, r, strerror(err), elapsed, RCVTIMEO_MS);
}

static void test_blocking_waits_for_signal(void) {
    int fd = nl_open(0, 0);
    check(fd >= 0, "socket+bind (%s)", strerror(errno));
    if (fd < 0)
        return;
    for (int m = M_RECV; m <= M_READ; m++)
        expect_interrupted(fd, (enum method) m, "unsubscribed");
    close(fd);

    /* The shape of the original probe and of `ip monitor`: subscribed to link
     * events. A genuine link change could land during the wait -- this runs on
     * real machines -- so an RTM_NEWLINK/DELLINK is read past, not counted. */
    fd = nl_open(0, RTMGRP_LINK_);
    check(fd >= 0, "RTMGRP_LINK socket+bind (%s)", strerror(errno));
    if (fd < 0)
        return;
    char buf[8192];
    interrupt_hits = 0;
    long start = now_ms();
    timer_arm(interrupt_timer, INTERRUPT_MS);
    ssize_t r;
    int err, events = 0;
    for (;;) {
        errno = 0;
        r = recv(fd, buf, sizeof(buf), 0);
        err = errno;
        if (r > 0 && interrupt_hits == 0 && events < 100) {
            const struct nl_hdr *h = (const struct nl_hdr *) buf;
            if (h->nlmsg_type == RTM_NEWLINK_ || h->nlmsg_type == RTM_DELLINK_) {
                events++;
                continue;
            }
        }
        break;
    }
    long elapsed = now_ms() - start;
    timer_disarm(interrupt_timer);
    if (events != 0)
        test_logf("note: %d genuine link event(s) arrived during the wait\n", events);
    check(r == -1 && err == EINTR && interrupt_hits == 1 &&
              elapsed >= INTERRUPT_MS - 50 && elapsed <= INTERRUPT_MS + slack_ms(1500),
          "RTMGRP_LINK recv: blocking, signal at %dms: ret=%zd errno=%s handler=%d "
          "after %ldms (want -1 EINTR handler=1 after ~%dms)",
          INTERRUPT_MS, r, strerror(err), (int) interrupt_hits, elapsed, INTERRUPT_MS);
    close(fd);
}

struct sender_args {
    int fd;
    int delay_ms;
    ssize_t sent;
    int err;
};

static void *sender_main(void *arg) {
    struct sender_args *a = arg;
    struct timespec ts = {.tv_sec = a->delay_ms / 1000, .tv_nsec = (a->delay_ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
    errno = 0;
    a->sent = send_link_dump(a->fd, 0x4e42);
    a->err = errno;
    return NULL;
}

/* SA_RESTART restarts the wait, and the reply a second thread's request
 * queues is what ends it. */
static void test_restart_then_message_from_thread(void) {
    int fd = nl_open(0, 0);
    check(fd >= 0, "socket+bind (%s)", strerror(errno));
    if (fd < 0)
        return;

    /* Both timer signals go to this thread: the sender starts with them
     * blocked. */
    sigset_t timer_sigs, old;
    sigemptyset(&timer_sigs);
    sigaddset(&timer_sigs, SIGUSR1);
    sigaddset(&timer_sigs, SIGUSR2);
    pthread_sigmask(SIG_BLOCK, &timer_sigs, &old);
    struct sender_args args = {.fd = fd, .delay_ms = 400};
    pthread_t sender;
    int create_err = pthread_create(&sender, NULL, sender_main, &args);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    check(create_err == 0, "pthread_create (%s)", strerror(create_err));
    if (create_err != 0) {
        close(fd);
        return;
    }

    char buf[32768];
    restart_hits = 0;
    interrupt_hits = 0;
    long start = now_ms();
    timer_arm(restart_timer, 150);
    timer_arm(interrupt_timer, 3000); /* guard */
    errno = 0;
    ssize_t r = recv(fd, buf, sizeof(buf), 0);
    int err = errno;
    long elapsed = now_ms() - start;
    timer_disarm(restart_timer);
    timer_disarm(interrupt_timer);
    pthread_join(sender, NULL);

    check(args.sent > 0, "sender thread's RTM_GETLINK dump request (ret=%zd errno=%s)",
          args.sent, strerror(args.err));
    check(r > 0 && restart_hits == 1 && interrupt_hits == 0 &&
              elapsed >= 350 && elapsed <= slack_ms(1000),
          "recv: SA_RESTART signal at 150ms, request from another thread at 400ms: "
          "ret=%zd errno=%s restart_handler=%d guard=%d after %ldms "
          "(want data, restart_handler=1, after ~400ms)",
          r, r < 0 ? strerror(err) : "-", (int) restart_hits, (int) interrupt_hits, elapsed);
    if (r > 0) {
        const struct nl_hdr *h = (const struct nl_hdr *) buf;
        check(h->nlmsg_seq == 0x4e42, "reply answers the request (seq=%#x)", h->nlmsg_seq);
        drain_dump(fd, buf, sizeof(buf), batch_has_done(buf, r), "restart");
    }
    close(fd);
}

/* Helper threads start with the timer signals blocked, so every timer signal
 * lands on the thread under test. */
static int start_helper(pthread_t *thread, void *(*fn)(void *), void *arg) {
    sigset_t timer_sigs, old;
    sigemptyset(&timer_sigs);
    sigaddset(&timer_sigs, SIGUSR1);
    sigaddset(&timer_sigs, SIGUSR2);
    pthread_sigmask(SIG_BLOCK, &timer_sigs, &old);
    int err = pthread_create(thread, NULL, fn, arg);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    return err;
}

static atomic_int helpers_stop;

static void *spin_main(void *arg) {
    (void) arg;
    volatile unsigned long spins = 0;
    while (!atomic_load(&helpers_stop))
        spins++;
    return NULL;
}

static void *map_main(void *arg) {
    long *rounds = arg;
    while (!atomic_load(&helpers_stop)) {
        char *p = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p != MAP_FAILED) {
            p[0] = 1;
            munmap(p, 65536);
        }
        (*rounds)++;
    }
    return NULL;
}

/* Other threads running and changing the address space do not end the wait:
 * nothing signalled it. AOK pokes a process's other threads when one of them
 * maps or unmaps memory, and the poke came back from the receive as EINTR. */
static void test_sibling_threads_do_not_interrupt(void) {
    int fd = nl_open(0, 0);
    check(fd >= 0, "socket+bind (%s)", strerror(errno));
    if (fd < 0)
        return;
    atomic_store(&helpers_stop, 0);
    long rounds = 0;
    pthread_t spinner, mapper;
    int e1 = start_helper(&spinner, spin_main, NULL);
    int e2 = e1 == 0 ? start_helper(&mapper, map_main, &rounds) : -1;
    check(e1 == 0 && e2 == 0, "helper threads (%s)", strerror(e1 ? e1 : e2));
    if (e1 == 0 && e2 == 0) {
        const enum method methods[] = {M_RECV, M_RECVMSG};
        for (int i = 0; i < 2; i++) {
            char buf[4096];
            interrupt_hits = 0;
            long start = now_ms();
            timer_arm(interrupt_timer, 800);
            errno = 0;
            ssize_t r = receive(fd, methods[i], 0, buf, sizeof(buf));
            int err = errno;
            long elapsed = now_ms() - start;
            timer_disarm(interrupt_timer);
            check(r == -1 && err == EINTR && interrupt_hits == 1 && elapsed >= 750 &&
                      elapsed <= 800 + slack_ms(1500),
                  "%s with sibling threads spinning and mapping memory, signal at 800ms: "
                  "ret=%zd errno=%s handler=%d after %ldms (want -1 EINTR handler=1 after ~800ms)",
                  method_names[methods[i]], r, strerror(err), (int) interrupt_hits, elapsed);
        }
    }
    atomic_store(&helpers_stop, 1);
    if (e1 == 0)
        pthread_join(spinner, NULL);
    if (e2 == 0)
        pthread_join(mapper, NULL);
    test_logf("note: %ld mmap/munmap rounds during the waits\n", rounds);
    close(fd);
}

struct poller_args {
    int fd;
    int use_epoll;
    atomic_long entered_at; /* just before the poll call; 0 until then */
    long woke_at;
};

static void *poller_main(void *arg) {
    struct poller_args *a = arg;
    int n = -1;
    if (a->use_epoll) {
        int ep = epoll_create1(EPOLL_CLOEXEC);
        struct epoll_event ev = {.events = EPOLLIN};
        if (ep >= 0 && epoll_ctl(ep, EPOLL_CTL_ADD, a->fd, &ev) == 0) {
            struct epoll_event out;
            atomic_store(&a->entered_at, now_ms());
            n = epoll_wait(ep, &out, 1, 5000);
        } else {
            atomic_store(&a->entered_at, -1); /* never waited: stop the caller waiting */
        }
        if (ep >= 0)
            close(ep);
    } else {
        struct pollfd p = {.fd = a->fd, .events = POLLIN};
        atomic_store(&a->entered_at, now_ms());
        n = poll(&p, 1, 5000);
    }
    a->woke_at = n == 1 ? now_ms() : -1;
    return NULL;
}

/* One try: a thread polls the socket, and once it is really waiting a request
 * is sent. Returns how long after the request the poller woke, or -1 when it
 * never did, with how long the thread took to start waiting in *startup_ms. */
static long poller_wake_trial(int use_epoll, long *startup_ms) {
    *startup_ms = -1;
    int fd = nl_open(0, 0);
    if (fd < 0)
        return -1;
    struct poller_args args = {.fd = fd, .use_epoll = use_epoll, .woke_at = -1};
    long started_at = now_ms();
    pthread_t poller;
    if (start_helper(&poller, poller_main, &args) != 0) {
        close(fd);
        return -1;
    }
    /* Measured from a request sent once the poller is waiting: a thread that
     * is slow to start is not a late wake. */
    while (atomic_load(&args.entered_at) == 0 && now_ms() - started_at < 10000) {
        struct timespec tick = {.tv_sec = 0, .tv_nsec = 5000000L};
        nanosleep(&tick, NULL);
    }
    long entered_at = atomic_load(&args.entered_at);
    if (entered_at > 0)
        *startup_ms = entered_at - started_at;
    struct timespec settle = {.tv_sec = 0, .tv_nsec = 300000000L};
    nanosleep(&settle, NULL);
    long sent_at = now_ms();
    ssize_t sent = send_noop_ack(fd, 0x9011);
    pthread_join(poller, NULL);
    char buf[256];
    (void) recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
    close(fd);
    if (sent <= 0 || entered_at <= 0 || args.woke_at < 0)
        return -1;
    return args.woke_at - sent_at;
}

/* A request's reply is readiness for a thread polling the same socket, at
 * once. AOK queued it without saying so, and the poller found it at its next
 * periodic recheck, ~700ms later on every try.
 *
 * The bound is not scaled for a loaded run, because scaling it past ~700ms
 * would pass that bug. A try that is slow for another reason -- the host
 * starving the thread, measured once at 1.3s under a load average of 65 --
 * is tried again instead: the bug is slow every time, and a stall rarely
 * three times running. */
static void test_reply_wakes_poller(void) {
    for (int use_epoll = 0; use_epoll <= 1; use_epoll++) {
        const char *what = use_epoll ? "epoll_wait" : "poll";
        char tries[128] = "";
        size_t used = 0;
        int ok = 0;
        for (int attempt = 0; attempt < 3 && !ok; attempt++) {
            long startup_ms;
            long latency = poller_wake_trial(use_epoll, &startup_ms);
            ok = latency >= 0 && latency <= 500;
            int n = snprintf(tries + used, sizeof(tries) - used, "%s%ldms (started %ldms)",
                             attempt ? ", " : "", latency, startup_ms);
            if (n > 0 && (size_t) n < sizeof(tries) - used)
                used += (size_t) n;
        }
        check(ok, "%s in another thread, request queues a reply: woke %s after it "
              "(want within 500ms on one of 3 tries)", what, tries);
    }
}

static void test_nonblocking_fails_at_once(void) {
    int fd = nl_open(0, 0);
    check(fd >= 0, "socket+bind (%s)", strerror(errno));
    if (fd < 0)
        return;
    expect_eagain_now(fd, M_RECV, MSG_DONTWAIT, "MSG_DONTWAIT");
    expect_eagain_now(fd, M_RECVFROM, MSG_DONTWAIT, "MSG_DONTWAIT");
    expect_eagain_now(fd, M_RECVMSG, MSG_DONTWAIT, "MSG_DONTWAIT");

    /* O_NONBLOCK set after creation, then cleared again: read at call time. */
    int fl = fcntl(fd, F_GETFL);
    check(fl >= 0 && fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0, "fcntl O_NONBLOCK (%s)",
          strerror(errno));
    for (int m = M_RECV; m <= M_READ; m++)
        expect_eagain_now(fd, (enum method) m, 0, "fcntl O_NONBLOCK");
    check(fcntl(fd, F_SETFL, fl & ~O_NONBLOCK) == 0, "fcntl clear O_NONBLOCK (%s)",
          strerror(errno));
    expect_interrupted(fd, M_RECV, "O_NONBLOCK cleared");
    close(fd);

    fd = nl_open(SOCK_NONBLOCK, 0);
    check(fd >= 0, "SOCK_NONBLOCK socket+bind (%s)", strerror(errno));
    if (fd < 0)
        return;
    for (int m = M_RECV; m <= M_READ; m++)
        expect_eagain_now(fd, (enum method) m, 0, "SOCK_NONBLOCK");
    close(fd);
}

static void test_rcvtimeo(void) {
    int fd = nl_open(0, 0);
    check(fd >= 0, "socket+bind (%s)", strerror(errno));
    if (fd < 0)
        return;
    struct timeval tv = {.tv_sec = 0, .tv_usec = RCVTIMEO_MS * 1000};
    check(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0,
          "setsockopt SO_RCVTIMEO (%s)", strerror(errno));
    struct timeval got = {.tv_sec = -1, .tv_usec = -1};
    socklen_t got_len = sizeof(got);
    int gr = getsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &got, &got_len);
    check(gr == 0 && got_len == sizeof(got) && got.tv_sec == 0 &&
              got.tv_usec >= RCVTIMEO_MS * 1000 && got.tv_usec < RCVTIMEO_MS * 1000 + 10000,
          "getsockopt SO_RCVTIMEO: ret=%d (%s) len=%u value=%lds %ldus (want 0s %dus)",
          gr, gr == 0 ? "-" : strerror(errno), (unsigned) got_len, (long) got.tv_sec,
          (long) got.tv_usec, RCVTIMEO_MS * 1000);

    for (int m = M_RECV; m <= M_READ; m++)
        expect_timed_out(fd, (enum method) m, "timed");

    /* A signal inside the timed wait ends it with EINTR -- even with
     * SA_RESTART, because Linux never restarts a timed socket wait. */
    for (int m = M_RECV; m <= M_READ; m++) {
        char buf[4096];
        restart_hits = 0;
        long start = now_ms();
        timer_arm(restart_timer, 100);
        errno = 0;
        ssize_t r = receive(fd, (enum method) m, 0, buf, sizeof(buf));
        int err = errno;
        long elapsed = now_ms() - start;
        timer_disarm(restart_timer);
        check(r == -1 && err == EINTR && restart_hits == 1 && elapsed >= 50 &&
                  elapsed < RCVTIMEO_MS,
              "timed %s, SA_RESTART signal at 100ms: ret=%zd errno=%s handler=%d after %ldms "
              "(want -1 EINTR handler=1 after ~100ms, before the %dms timeout)",
              method_names[m], r, strerror(err), (int) restart_hits, elapsed, RCVTIMEO_MS);
    }

    /* A zero timeout switches the timeout off again. */
    struct timeval zero = {0, 0};
    check(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &zero, sizeof(zero)) == 0,
          "setsockopt SO_RCVTIMEO 0 (%s)", strerror(errno));
    expect_interrupted(fd, M_RECV, "timeout cleared");
    close(fd);
}

/* Linux's sock_set_timeout rules, in the kernel's own layouts. */
static void test_timeo_option_rules(void) {
    int fd = nl_open(0, 0);
    check(fd >= 0, "socket+bind (%s)", strerror(errno));
    if (fd < 0)
        return;

    /* _NEW is a 64-bit timeval on every ABI; _OLD is a pair of longs. */
    int64_t new_tv[2] = {0, 200000};
    errno = 0;
    int sr = setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO_NEW_, new_tv, sizeof(new_tv));
    check(sr == 0, "SO_RCVTIMEO_NEW {0,200000}: ret=%d (%s)", sr, sr ? strerror(errno) : "-");
    long old_tv[2] = {7, 7};
    socklen_t len = sizeof(old_tv);
    int gr = getsockopt(fd, SOL_SOCKET, SO_RCVTIMEO_OLD_, old_tv, &len);
    check(gr == 0 && len == sizeof(old_tv) && old_tv[0] == 0 && old_tv[1] >= 200000 &&
              old_tv[1] < 210000,
          "SO_RCVTIMEO_OLD reads it back: ret=%d len=%u value=%ld,%ld (want 0,200000)",
          gr, (unsigned) len, old_tv[0], old_tv[1]);
    char buf[256];
    long start = now_ms();
    timer_arm(interrupt_timer, 3000);
    errno = 0;
    ssize_t r = recv(fd, buf, sizeof(buf), 0);
    int err = errno;
    long elapsed = now_ms() - start;
    timer_disarm(interrupt_timer);
    check(r == -1 && err == EAGAIN && elapsed >= 150 && elapsed <= 200 + slack_ms(1500),
          "recv, SO_RCVTIMEO_NEW 200ms: ret=%zd errno=%s after %ldms (want -1 EAGAIN after ~200ms)",
          r, strerror(err), elapsed);

    errno = 0;
    sr = setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO_NEW_, new_tv, 8);
    check(sr == -1 && errno == EINVAL, "SO_RCVTIMEO_NEW with optlen 8: ret=%d errno=%s (want EINVAL)",
          sr, strerror(errno));
    long bad_usec[2] = {0, 1000000};
    errno = 0;
    sr = setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO_OLD_, bad_usec, sizeof(bad_usec));
    check(sr == -1 && errno == EDOM, "SO_RCVTIMEO usec=1000000: ret=%d errno=%s (want EDOM)",
          sr, strerror(errno));
    bad_usec[0] = -5;
    bad_usec[1] = -1;
    errno = 0;
    sr = setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO_OLD_, bad_usec, sizeof(bad_usec));
    check(sr == -1 && errno == EDOM, "SO_RCVTIMEO {-5,-1}: ret=%d errno=%s (want EDOM, usec first)",
          sr, strerror(errno));

    /* A negative second count is a zero-jiffy timeout: it reads back as zero
     * and the socket stops waiting at all. */
    long negative[2] = {-1, 0};
    errno = 0;
    sr = setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO_OLD_, negative, sizeof(negative));
    check(sr == 0, "SO_RCVTIMEO {-1,0}: ret=%d (%s)", sr, sr ? strerror(errno) : "-");
    old_tv[0] = old_tv[1] = 7;
    len = sizeof(old_tv);
    gr = getsockopt(fd, SOL_SOCKET, SO_RCVTIMEO_OLD_, old_tv, &len);
    check(gr == 0 && old_tv[0] == 0 && old_tv[1] == 0,
          "SO_RCVTIMEO {-1,0} reads back: ret=%d value=%ld,%ld (want 0,0)", gr, old_tv[0], old_tv[1]);
    expect_eagain_now(fd, M_RECV, 0, "SO_RCVTIMEO {-1,0}");

    /* SO_SNDTIMEO is kept and reads back, though a netlink send never waits. */
    long snd[2] = {2, 500000};
    errno = 0;
    sr = setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO_OLD_, snd, sizeof(snd));
    long snd_got[2] = {7, 7};
    len = sizeof(snd_got);
    gr = getsockopt(fd, SOL_SOCKET, SO_SNDTIMEO_OLD_, snd_got, &len);
    check(sr == 0 && gr == 0 && snd_got[0] == 2 && snd_got[1] == 500000,
          "SO_SNDTIMEO {2,500000}: set=%d get=%d value=%ld,%ld", sr, gr, snd_got[0], snd_got[1]);
    /* A buffer too short for the value is truncated, not refused. */
    long trunc[2] = {7, 7};
    len = sizeof(long) / 2;
    gr = getsockopt(fd, SOL_SOCKET, SO_SNDTIMEO_OLD_, trunc, &len);
    check(gr == 0 && len == sizeof(long) / 2 && trunc[1] == 7,
          "SO_SNDTIMEO into a half-long buffer: ret=%d len=%u (want 0, len %u, rest untouched)",
          gr, (unsigned) len, (unsigned) (sizeof(long) / 2));
    close(fd);
}

/* Every GET is answered, including ones AOK keeps no table for: it answers
 * with the empty table. A request answered with nothing leaves a blocking
 * receive waiting for ever, which is what `ip neigh` and `ip rule` did once
 * the receive waited. */
static void test_every_get_is_answered(void) {
    const uint16_t types[] = {RTM_GETNEIGH_, RTM_GETRULE_};
    const char *const names[] = {"RTM_GETNEIGH", "RTM_GETRULE"};
    for (int i = 0; i < 2; i++) {
        int fd = nl_open(0, 0);
        check(fd >= 0, "socket+bind (%s)", strerror(errno));
        if (fd < 0)
            return;
        check(send_dump(fd, types[i], 0x6e00 + (uint32_t) i) > 0, "%s dump request (%s)",
              names[i], strerror(errno));
        char buf[32768];
        long start = now_ms();
        timer_arm(interrupt_timer, 3000);
        errno = 0;
        ssize_t r = recv(fd, buf, sizeof(buf), 0);
        int err = errno;
        long elapsed = now_ms() - start;
        timer_disarm(interrupt_timer);
        check(r > 0 && elapsed < 500, "%s dump answered: ret=%zd errno=%s after %ldms "
              "(want a reply at once)", names[i], r, r < 0 ? strerror(err) : "-", elapsed);
        if (r > 0)
            drain_dump(fd, buf, sizeof(buf), batch_has_done(buf, r), names[i]);
        close(fd);
    }
}

static void test_queued_reply_and_zero_read(void) {
    int fd = nl_open(0, 0);
    check(fd >= 0, "socket+bind (%s)", strerror(errno));
    if (fd < 0)
        return;

    /* read() of nothing answers before it looks at the queue. */
    char buf[32768];
    long start = now_ms();
    timer_arm(interrupt_timer, 2000);
    errno = 0;
    ssize_t r = read(fd, buf, 0);
    int err = errno;
    long elapsed = now_ms() - start;
    timer_disarm(interrupt_timer);
    check(r == 0 && elapsed < 500, "read 0 bytes, empty queue: ret=%zd errno=%s after %ldms "
          "(want 0 at once)", r, r < 0 ? strerror(err) : "-", elapsed);

    /* ...and so it leaves a queued message where it is. */
    check(send_noop_ack(fd, 0x7a70) > 0, "NLMSG_NOOP|NLM_F_ACK request (%s)", strerror(errno));
    errno = 0;
    r = read(fd, buf, 0);
    err = errno;
    check(r == 0, "read 0 bytes, ACK queued: ret=%zd errno=%s (want 0)", r,
          r < 0 ? strerror(err) : "-");
    errno = 0;
    r = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
    err = errno;
    const struct nl_hdr *ack = (const struct nl_hdr *) buf;
    check(r >= (ssize_t) sizeof(*ack) && ack->nlmsg_type == NLMSG_ERROR_ && ack->nlmsg_seq == 0x7a70,
          "ACK still queued after the zero-byte read: ret=%zd errno=%s type=%u seq=%#x",
          r, r < 0 ? strerror(err) : "-", r > 0 ? ack->nlmsg_type : 0,
          r > 0 ? ack->nlmsg_seq : 0);
    expect_eagain_now(fd, M_RECV, MSG_DONTWAIT, "ACK consumed");

    /* A queued reply is returned at once by a blocking receive. */
    check(send_link_dump(fd, 0x5157) > 0, "RTM_GETLINK dump request (%s)", strerror(errno));
    start = now_ms();
    timer_arm(interrupt_timer, 2000);
    errno = 0;
    r = recv(fd, buf, sizeof(buf), 0);
    err = errno;
    elapsed = now_ms() - start;
    timer_disarm(interrupt_timer);
    check(r > 0 && elapsed < 500, "blocking recv, reply queued: ret=%zd errno=%s after %ldms "
          "(want data at once)", r, r < 0 ? strerror(err) : "-", elapsed);
    if (r > 0)
        drain_dump(fd, buf, sizeof(buf), batch_has_done(buf, r), "queued");
    close(fd);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));

    struct sigaction interrupt_sa, restart_sa;
    memset(&interrupt_sa, 0, sizeof(interrupt_sa));
    interrupt_sa.sa_handler = on_interrupt;
    sigemptyset(&interrupt_sa.sa_mask);
    memset(&restart_sa, 0, sizeof(restart_sa));
    restart_sa.sa_handler = on_restart;
    restart_sa.sa_flags = SA_RESTART;
    sigemptyset(&restart_sa.sa_mask);
    if (sigaction(SIGUSR1, &interrupt_sa, NULL) != 0 ||
            sigaction(SIGUSR2, &restart_sa, NULL) != 0) {
        printf("netlink_blocking_recv: FAIL sigaction: %s\n", strerror(errno));
        return 1;
    }
    struct sigevent sev;
    memset(&sev, 0, sizeof(sev));
    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = SIGUSR1;
    int t1 = timer_create(CLOCK_MONOTONIC, &sev, &interrupt_timer);
    sev.sigev_signo = SIGUSR2;
    int t2 = t1 == 0 ? timer_create(CLOCK_MONOTONIC, &sev, &restart_timer) : -1;
    if (t1 != 0 || t2 != 0) {
        printf("netlink_blocking_recv: FAIL timer_create: %s\n", strerror(errno));
        return 1;
    }

    int probe = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE_);
    if (probe < 0) {
        printf("netlink_blocking_recv: SKIP (no NETLINK_ROUTE socket: %s)\n", strerror(errno));
        return 0;
    }
    close(probe);

    test_queued_reply_and_zero_read();
    test_every_get_is_answered();
    test_nonblocking_fails_at_once();
    test_blocking_waits_for_signal();
    test_restart_then_message_from_thread();
    test_sibling_threads_do_not_interrupt();
    test_reply_wakes_poller();
    test_rcvtimeo();
    test_timeo_option_rules();

    return finish_suite("netlink_blocking_recv");
}
