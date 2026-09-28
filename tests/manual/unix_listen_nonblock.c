// Two ways the X display broke the Wayland desktop, both where a guest AF_UNIX
// socket is a host one and Darwin answers differently from Linux.
//
// 1. The listen backlog. Linux queues backlog + 1 connections on an AF_UNIX
//    listener, and a blocking connect() beyond that waits for room; it never
//    refuses a listener that exists. Darwin queued fewer and refused the rest
//    with ECONNREFUSED. wlroots listens on the X display with a backlog of 1,
//    so with one X client queued, the compositor's own connect was refused.
//
// 2. EINTR from a non-blocking receive. Linux never fails a non-blocking
//    recv/recvmsg with EINTR -- it transfers or says EAGAIN, and a pending
//    signal is delivered when the call returns. AOK checked for a pending
//    signal before the host call and returned EINTR, so under Xwayland's
//    timer signal its recvmsg on the window-manager socket failed now and
//    then; the X server took that for a dead client and closed the
//    connection, and labwc crashed. Extreme Tux Racer, the first X11 game
//    started, froze or killed the desktop either way.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-56s got=%-8ld want=%ld\n", label, got, want);
}

static socklen_t abstract_addr(struct sockaddr_un *a, const char *name) {
    size_t n = strlen(name);
    memset(a, 0, sizeof *a);
    a->sun_family = AF_UNIX;
    memcpy(a->sun_path + 1, name, n);
    return (socklen_t) (offsetof(struct sockaddr_un, sun_path) + 1 + n);
}

static void check_backlog(void) {
    struct sockaddr_un a;
    char name[64];
    snprintf(name, sizeof name, "/tmp/unix-listen-nonblock-%d", (int) getpid());
    socklen_t len = abstract_addr(&a, name);
    int l = socket(AF_UNIX, SOCK_STREAM, 0);
    if (l < 0 || bind(l, (struct sockaddr *) &a, len) < 0 || listen(l, 1) < 0) {
        printf("FAIL could not set up the listener: %s\n", strerror(errno));
        failures_total++;
        return;
    }
    // Two fit a backlog of 1 on Linux. Non-blocking, so a third that has to
    // wait says EAGAIN rather than hanging the test; never ECONNREFUSED.
    int c[3];
    int refused = 0;
    for (int i = 0; i < 3; i++) {
        c[i] = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (connect(c[i], (struct sockaddr *) &a, len) < 0 && errno == ECONNREFUSED)
            refused++;
    }
    ck("connects to a listen(1) socket refused", refused, 0);
    fcntl(l, F_SETFL, O_NONBLOCK);
    int accepted = 0;
    for (int s; (s = accept(l, NULL, NULL)) >= 0; close(s))
        accepted++;
    ck("the first two were queued and accepted", accepted >= 2, 1);
    struct pollfd p = { l, POLLIN, 0 };
    ck("the drained listener is not readable", poll(&p, 1, 0), 0);
    for (int i = 0; i < 3; i++)
        close(c[i]);
    close(l);
}

static int sv[2];
static volatile int stop_writer;
static pthread_t receiver;
static void on_signal(int sig) { (void) sig; }

// A signal every 500 us at the receiving thread: the old code failed several
// thousand of its calls in two seconds. SIGUSR1 rather than a timer, which
// would take SIGALRM from the watchdog.
static void *signaller(void *arg) {
    (void) arg;
    struct timespec gap = { 0, 500000 };
    while (!stop_writer) {
        pthread_kill(receiver, SIGUSR1);
        nanosleep(&gap, NULL);
    }
    return NULL;
}

static void *writer(void *arg) {
    (void) arg;
    char buf[5100];
    memset(buf, 'x', sizeof buf);
    while (!stop_writer) {
        if (send(sv[1], buf, sizeof buf, MSG_DONTWAIT) < 0 && errno == EAGAIN)
            usleep(100);
    }
    return NULL;
}

static void check_nonblock_eintr(void) {
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        printf("FAIL socketpair: %s\n", strerror(errno));
        failures_total++;
        return;
    }
    fcntl(sv[0], F_SETFL, O_NONBLOCK);
    struct sigaction sa = { .sa_handler = on_signal, .sa_flags = SA_RESTART };
    sigaction(SIGUSR1, &sa, NULL);
    receiver = pthread_self();
    pthread_t t, sig;
    pthread_create(&t, NULL, writer, NULL);
    pthread_create(&sig, NULL, signaller, NULL);
    long calls = 0, eintr_recv = 0, eintr_recvmsg = 0, got_data = 0;
    char buf[4096];
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    do {
        ssize_t n = recv(sv[0], buf, sizeof buf, 0);
        if (n < 0 && errno == EINTR)
            eintr_recv++;
        got_data += n > 0;
        struct iovec iov = { buf, sizeof buf };
        struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1 };
        n = recvmsg(sv[0], &m, MSG_DONTWAIT);
        if (n < 0 && errno == EINTR)
            eintr_recvmsg++;
        got_data += n > 0;
        calls += 2;
        clock_gettime(CLOCK_MONOTONIC, &now);
    } while (now.tv_sec - start.tv_sec < 2);
    stop_writer = 1;
    pthread_join(sig, NULL);
    pthread_join(t, NULL);
    test_logf("  %ld calls, %ld returned data\n", calls, got_data);
    ck("the receives ran and returned data", got_data > 0, 1);
    ck("EINTR from recv on an O_NONBLOCK socket", eintr_recv, 0);
    ck("EINTR from recvmsg with MSG_DONTWAIT", eintr_recvmsg, 0);
    close(sv[0]);
    close(sv[1]);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));
    check_backlog();
    check_nonblock_eintr();
    return finish_suite("unix_listen_nonblock");
}
