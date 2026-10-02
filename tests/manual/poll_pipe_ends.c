// What poll says about a pipe or FIFO once the other end has gone.
//
//   Linux's pipe_poll (6.12), for every kind measured here -- an anonymous
//   pipe, a FIFO on the root filesystem, a FIFO on tmpfs:
//     - a write end whose readers have all gone: POLLERR, whatever was asked,
//       with POLLOUT while there is room;
//     - a read end whose writers have all gone: POLLHUP, and POLLIN only
//       while there are bytes left to read;
//     - but a FIFO reader is hung up only by a writer that came after its own
//       open: one opened O_NONBLOCK with no writer yet is idle, not HUP.
//
//   AOK had all of it wrong somewhere:
//     - an anonymous pipe's writer heard nothing: realfs_poll's scrub of the
//       spurious POLLHUP Darwin raises on a FIFO read end opened before any
//       writer took the writer's real one with it, and rpe_events mapped a
//       blocked wait's EV_EOF to plain POLL_WRITE;
//     - an empty read end at end of file said POLLIN|POLLHUP (Darwin's);
//     - a named FIFO on the root filesystem (fakefs, a host FIFO) said
//       nothing at all in either direction, fresh or blocked, because Darwin
//       reports neither end's departure on a FIFO -- fs/host_fifo.c now keeps
//       the reader/writer accounting itself;
//     - tmpfs's FIFOs (fs/fifo.c) said POLLIN at end of file, and hung up a
//       reader no writer had ever reached;
//     - epoll watched the host for a hangup only when asked, so it heard of
//       one from the periodic rescan, a second late;
//     - select reported such an fd in the read set too, when asked only about
//       writing, and counted it twice.
//
//   GNU tail polls stdout for POLLERR between passes so that
//   `tail -f file | head -2` exits once head does; it followed forever.
//
// Each "reports" has its control (the same probe with the other end still
// open), and blocked waits are timed, so a timeout cannot pass for a wake.
//
// Measured against x86_64 glibc on Linux 6.12.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mount.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

static char where[64];   // the kind under test, for labels
static char fifo_path[256];

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double) t.tv_sec + (double) t.tv_nsec / 1e9;
}

static void expect(const char *what, int fd, short events, short want) {
    struct pollfd p = {fd, events, 0};
    int r = poll(&p, 1, 0);
    char label[160];
    snprintf(label, sizeof(label), "%s: %s (events %#x)", where, what, (unsigned short) events);
    if (r != (want != 0) || p.revents != want)
        failf(label, (uint64_t) r, (uint64_t) (uint16_t) p.revents, 0, want != 0, (uint64_t) (uint16_t) want, 0);
    test_logf("%s: revents=%#x\n", label, p.revents);
}

// p[0] the read end, p[1] the write end. A FIFO is opened the way a reader
// and then a writer would, and the read end left blocking.
static void make(bool named, int p[2]) {
    if (!named) {
        if (pipe(p) < 0) { perror("pipe"); exit(1); }
        return;
    }
    unlink(fifo_path);
    if (mkfifo(fifo_path, 0600) < 0) { perror("mkfifo"); exit(1); }
    p[0] = open(fifo_path, O_RDONLY | O_NONBLOCK);
    p[1] = open(fifo_path, O_WRONLY | O_NONBLOCK);
    if (p[0] < 0 || p[1] < 0) { perror("open fifo"); exit(1); }
    fcntl(p[0], F_SETFL, 0);
}

static void blocked_waits(bool named) {
    for (int use_epoll = 0; use_epoll < 2; use_epoll++) {
        for (int mask = 0; mask < (use_epoll ? 2 : 1); mask++) {
            int p[2];
            make(named, p);
            pid_t c = fork();
            if (c == 0) {
                close(p[1]);
                usleep(300000);
                _exit(0);
            }
            close(p[0]);
            double t0 = now();
            int r;
            unsigned got, want;
            if (!use_epoll) {
                struct pollfd d = {p[1], 0, 0};
                r = poll(&d, 1, 3000);
                got = (unsigned short) d.revents;
                want = POLLERR;
            } else {
                int ep = epoll_create1(EPOLL_CLOEXEC);
                struct epoll_event ee = {.events = mask ? EPOLLIN : 0}, out = {0};
                epoll_ctl(ep, EPOLL_CTL_ADD, p[1], &ee);
                r = epoll_wait(ep, &out, 1, 3000);
                got = r > 0 ? out.events : 0;
                want = EPOLLERR;
                close(ep);
            }
            double dt = now() - t0;
            char label[160];
            snprintf(label, sizeof(label), "%s: blocked %s%s on the writer wakes when the reader goes", where,
                     use_epoll ? "epoll_wait" : "poll", use_epoll ? (mask ? "(EPOLLIN)" : "(0)") : "(0)");
            if (r != 1 || got != want || dt < 0.2 || dt > 0.9)
                failf(label, (uint64_t) r, got, (uint64_t) (dt * 1000), 1, want, 300);
            test_logf("%s: r=%d got=%#x %.3fs\n", label, r, got, dt);
            waitpid(c, NULL, 0);
            close(p[1]);
        }
    }
}

static void run(bool named) {
    int p[2];

    // The write end. Control: a live reader, no error.
    make(named, p);
    expect("writer, reader open", p[1], POLLOUT, POLLOUT);
    expect("writer, reader open", p[1], 0, 0);
    close(p[0]);
    expect("writer, reader gone", p[1], 0, POLLERR);
    expect("writer, reader gone", p[1], POLLRDBAND, POLLERR);
    expect("writer, reader gone", p[1], POLLOUT, POLLOUT | POLLERR);
    {
        // select: in the write set, only there, counted once
        fd_set r, w;
        FD_ZERO(&r);
        FD_ZERO(&w);
        FD_SET(p[1], &w);
        struct timeval tv = {0, 0};
        int n = select(p[1] + 1, &r, &w, NULL, &tv);
        char label[160];
        snprintf(label, sizeof(label), "%s: select, writer, reader gone: write set only", where);
        if (n != 1 || !FD_ISSET(p[1], &w) || FD_ISSET(p[1], &r))
            failf(label, (uint64_t) n, FD_ISSET(p[1], &w), FD_ISSET(p[1], &r), 1, 1, 0);
    }
    close(p[1]);

    // The read end. Control: a live writer, no hangup.
    make(named, p);
    expect("reader, writer open, empty", p[0], POLLIN, 0);
    close(p[1]);
    expect("reader, writer gone, empty", p[0], POLLIN, POLLHUP);
    expect("reader, writer gone, empty", p[0], 0, POLLHUP);
    close(p[0]);
    make(named, p);
    if (write(p[1], "x", 1) != 1)
        perror("write");
    close(p[1]);
    expect("reader, writer gone, a byte left", p[0], POLLIN, POLLIN | POLLHUP);
    char ch;
    if (read(p[0], &ch, 1) != 1)
        perror("read");
    expect("reader, writer gone, drained", p[0], POLLIN, POLLHUP);
    close(p[0]);

    blocked_waits(named);

    if (named) {
        // A reader that opened with no writer is not hung up; one that opened
        // beside a live writer is, once it goes -- and so is the first.
        unlink(fifo_path);
        mkfifo(fifo_path, 0600);
        int r1 = open(fifo_path, O_RDONLY | O_NONBLOCK);
        expect("reader opened alone, no writer ever", r1, POLLIN, 0);
        int w1 = open(fifo_path, O_WRONLY | O_NONBLOCK);
        int r2 = open(fifo_path, O_RDONLY | O_NONBLOCK);
        close(w1);
        expect("reader opened beside a writer, gone", r2, POLLIN, POLLHUP);
        expect("first reader, after that writer", r1, POLLIN, POLLHUP);
        close(r1);
        close(r2);
        unlink(fifo_path);
    }
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    signal(SIGPIPE, SIG_IGN);
    alarm(test_watchdog_secs(60));

    snprintf(where, sizeof(where), "pipe");
    run(false);

    // A FIFO on the root filesystem: a host FIFO under fakefs.
    snprintf(where, sizeof(where), "fifo on /tmp");
    snprintf(fifo_path, sizeof(fifo_path), "/tmp/poll_pipe_ends.%d.fifo", (int) getpid());
    run(true);

    // A FIFO on tmpfs: fs/fifo.c. Mounting one takes root.
    char dir[] = "/tmp/poll_pipe_ends.XXXXXX";
    if (mkdtemp(dir) != NULL) {
        if (mount("tmpfs", dir, "tmpfs", 0, NULL) == 0) {
            snprintf(where, sizeof(where), "fifo on tmpfs");
            snprintf(fifo_path, sizeof(fifo_path), "%s/f", dir);
            run(true);
            umount2(dir, MNT_DETACH);
        } else {
            test_logf("tmpfs: not mounted (%s), skipped\n", strerror(errno));
        }
        rmdir(dir);
    }

    return finish_suite("poll_pipe_ends");
}
