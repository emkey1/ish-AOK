// What poll and epoll report, and what they will accept being asked.
//
//   EPOLLRDHUP did not exist. There was no bit for it anywhere in the poll
//   model, so nothing could produce one -- and a socket half-close was
//   reported as EPOLLHUP instead, on a socket that was still perfectly
//   writable. A program treats EPOLLHUP as "connection over" and drops it, so
//   the ordinary shutdown-write-then-read-the-reply exchange broke. Whether
//   end-of-input is a HANGUP depends on what the fd is: for a pipe the writer
//   closing IS the hangup, and that has to keep working.
//
//   epoll_ctl(ADD) on a regular file or a directory returned 0. Linux gives
//   EPERM, because such a file has no poll operation -- so the event could
//   never arrive, and an fd that never fires looks exactly like an idle one. A
//   program waiting on a file it should have been told to just read blocked
//   forever with no indication why. Linux decides per-file and its procfs
//   entries are not uniform (measured: /proc/self/mountinfo, /proc/cpuinfo and
//   /proc/uptime are accepted, /proc/self/stat is not, and every directory
//   including /proc is EPERM). AOK uses the coarser rule -- any directory, and
//   a regular file on an ordinary filesystem -- so it over-permits a few
//   procfs entries rather than breaking the pollable ones that matter.
//
//   select() and poll() sized their working arrays on the STACK from a
//   guest-controlled nfds, with no bound. select(-1, ...) made BITS_SIZE
//   compute an enormous size and the memset walked off the stack: the whole
//   emulator died with SIGBUS, not just the calling task. Linux bounds both,
//   and differently -- a negative nfds is EINVAL, an oversized one is clamped
//   to the fd table for select and refused with EINVAL past RLIMIT_NOFILE for
//   poll.
//
// Measured against x86_64 glibc on Linux 6.12.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <time.h>

#include "test_common.h"

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-52s got=0x%-7lx want=0x%lx\n", label, got, want);
}

static int ep_events_wait(int fd, int want, int timeout_ms) {
    int ep = epoll_create1(0);
    if (ep < 0)
        return -1;
    struct epoll_event ev = { .events = want };
    if (epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev) < 0) {
        close(ep);
        return -1;
    }
    struct epoll_event out;
    int n = epoll_wait(ep, &out, 1, timeout_ms);
    close(ep);
    return n > 0 ? (int) out.events : 0;
}

static int ep_events(int fd, int want) {
    return ep_events_wait(fd, want, 300);
}

static int poll_events_wait(int fd, int want, int timeout_ms) {
    struct pollfd p = { fd, want, 0 };
    return poll(&p, 1, timeout_ms) > 0 ? p.revents : 0;
}

static int poll_events(int fd, int want) {
    return poll_events_wait(fd, want, 300);
}

// Closes *arg after 200 ms, long enough that the caller is already blocked.
static void *close_later(void *arg) {
    struct timespec ts = { 0, 200 * 1000 * 1000 };
    nanosleep(&ts, NULL);
    close(*(int *) arg);
    return NULL;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));

    // ---- a socket half-close is RDHUP, not HUP ----------------------------
    {
        int sv[2];
        ck("socketpair", socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
        if (write(sv[1], "x", 1) != 1)
            failures_total++;
        ck("  peer shuts down writing", shutdown(sv[1], SHUT_WR), 0);
        ck("  epoll reports IN|RDHUP", ep_events(sv[0], EPOLLIN | EPOLLRDHUP),
           EPOLLIN | EPOLLRDHUP);
        ck("  poll reports IN|RDHUP", poll_events(sv[0], POLLIN | POLLRDHUP),
           POLLIN | POLLRDHUP);
        // ...which is the point: it is still a working socket in one direction
        ck("  and it is still writable",
           (poll_events(sv[0], POLLOUT) & POLLOUT) != 0, 1);
        ck("  so a write still goes through", write(sv[0], "y", 1) == 1, 1);
        close(sv[0]);
        close(sv[1]);
    }

    // ---- a full close is RDHUP AND HUP ------------------------------------
    {
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) {
            if (write(sv[1], "x", 1) != 1)
                failures_total++;
            close(sv[1]);
            ck("a full close reports IN|RDHUP|HUP",
               ep_events(sv[0], EPOLLIN | EPOLLRDHUP) & (EPOLLIN | EPOLLRDHUP | EPOLLHUP),
               EPOLLIN | EPOLLRDHUP | EPOLLHUP);
            close(sv[0]);
        }
    }

    // ---- ...and poll's answer to a closed peer is exactly IN|HUP ----------
    // Exact, not masked: 555 measured 0x10 here, POLLHUP without POLLIN, and
    // a program that waits for POLLIN before reading never reads the
    // end-of-file it is being told about. With or without data still queued,
    // since an empty queue is the case that dropped POLLIN. SEQPACKET takes
    // the same path. Linux 6.12 answers 0x11 for all four, and epoll 0x11 for
    // an EPOLLIN-only registration.
    {
        static const int types[] = { SOCK_STREAM, SOCK_SEQPACKET };
        static const char *const names[] = { "stream", "seqpacket" };
        for (int t = 0; t < 2; t++) {
            for (int data = 0; data < 2; data++) {
                int sv[2];
                char label[96];
                if (socketpair(AF_UNIX, types[t], 0, sv) != 0) {
                    snprintf(label, sizeof label, "%s socketpair", names[t]);
                    ck(label, -1, 0);
                    continue;
                }
                if (data && write(sv[1], "x", 1) != 1)
                    failures_total++;
                close(sv[1]);
                snprintf(label, sizeof label, "%s peer closed%s: poll(POLLIN) is IN|HUP",
                         names[t], data ? ", data queued" : "");
                ck(label, poll_events(sv[0], POLLIN), POLLIN | POLLHUP);
                snprintf(label, sizeof label, "%s peer closed%s: epoll(EPOLLIN) is IN|HUP",
                         names[t], data ? ", data queued" : "");
                ck(label, ep_events(sv[0], EPOLLIN), EPOLLIN | EPOLLHUP);
                close(sv[0]);
            }
        }
    }

    // ---- ...including when the poll is already waiting ---------------------
    // The same close, but made while the poll is blocked, so the answer comes
    // from the wakeup rather than from a fresh look at the socket. Those are
    // two different paths, and this one still said 0x10 after the other had
    // been fixed. A thread closes the peer 200 ms in.
    {
        static const int types[] = { SOCK_STREAM, SOCK_SEQPACKET };
        static const char *const names[] = { "stream", "seqpacket" };
        for (int t = 0; t < 2; t++) {
            for (int use_epoll = 0; use_epoll < 2; use_epoll++) {
                int sv[2];
                char label[96];
                if (socketpair(AF_UNIX, types[t], 0, sv) != 0) {
                    snprintf(label, sizeof label, "%s socketpair", names[t]);
                    ck(label, -1, 0);
                    continue;
                }
                pthread_t th;
                if (pthread_create(&th, NULL, close_later, &sv[1]) != 0) {
                    ck("pthread_create", -1, 0);
                    close(sv[0]);
                    close(sv[1]);
                    continue;
                }
                snprintf(label, sizeof label, "%s peer closed while %s waits: IN|HUP",
                         names[t], use_epoll ? "epoll(EPOLLIN)" : "poll(POLLIN)");
                struct timespec t0, t1;
                clock_gettime(CLOCK_MONOTONIC, &t0);
                int got = use_epoll ? ep_events_wait(sv[0], EPOLLIN, 5000)
                                    : poll_events_wait(sv[0], POLLIN, 5000);
                clock_gettime(CLOCK_MONOTONIC, &t1);
                long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
                ck(label, got, POLLIN | POLLHUP);
                // ...and woken BY the close, ~200 ms in. The value alone is
                // not enough: an epoll waiter that the close failed to wake
                // still read 0x11, from the rescan when its wait was capped
                // at one second.
                snprintf(label, sizeof label, "  and woke within 800 ms (took %ld)", ms);
                ck(label, ms < 800, 1);
                pthread_join(th, NULL);
                close(sv[0]);
            }
        }
    }

    // ---- RDHUP is only delivered when asked for ---------------------------
    {
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) {
            if (write(sv[1], "x", 1) != 1)
                failures_total++;
            shutdown(sv[1], SHUT_WR);
            ck("a registration that did not ask gets neither RDHUP nor HUP",
               ep_events(sv[0], EPOLLIN) & (EPOLLRDHUP | EPOLLHUP), 0);
            close(sv[0]);
            close(sv[1]);
        }
    }

    // ---- but a pipe's EOF is still a hangup -------------------------------
    {
        int pf[2];
        if (pipe(pf) == 0) {
            if (write(pf[1], "x", 1) != 1)
                failures_total++;
            close(pf[1]);
            ck("a pipe read end still reports IN|HUP",
               ep_events(pf[0], EPOLLIN) & (EPOLLIN | EPOLLHUP), EPOLLIN | EPOLLHUP);
            ck("  and poll agrees",
               poll_events(pf[0], POLLIN) & (POLLIN | POLLHUP), POLLIN | POLLHUP);
            close(pf[0]);
        }
    }

    // ---- epoll_ctl only accepts things that can be polled ------------------
    {
        char path[128];
        snprintf(path, sizeof path, "/tmp/epollreg-%d", (int) getpid());
        unlink(path);
        int ep = epoll_create1(0);
        struct epoll_event ev = { .events = EPOLLIN };
        int f = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
        ck("epoll_ctl(ADD) on a regular file is EPERM",
           (errno = 0, epoll_ctl(ep, EPOLL_CTL_ADD, f, &ev) < 0 ? errno : 0), EPERM);
        close(f);
        unlink(path);
        int d = open("/tmp", O_RDONLY | O_DIRECTORY);
        ck("epoll_ctl(ADD) on a directory is EPERM",
           (errno = 0, epoll_ctl(ep, EPOLL_CTL_ADD, d, &ev) < 0 ? errno : 0), EPERM);
        close(d);
        // ...but a procfs FILE is pollable and must stay so: that is how
        // mount-change notification works, and both mountinfo_epollet and
        // epoll_nested in this suite depend on it. A procfs DIRECTORY is not.
        int mi = open("/proc/self/mountinfo", O_RDONLY);
        if (mi >= 0) {
            ck("epoll_ctl(ADD) on /proc/self/mountinfo is allowed",
               (errno = 0, epoll_ctl(ep, EPOLL_CTL_ADD, mi, &ev) < 0 ? errno : 0), 0);
            epoll_ctl(ep, EPOLL_CTL_DEL, mi, NULL);
            close(mi);
        }
        int pd = open("/proc", O_RDONLY | O_DIRECTORY);
        if (pd >= 0) {
            ck("  but /proc itself is still EPERM",
               (errno = 0, epoll_ctl(ep, EPOLL_CTL_ADD, pd, &ev) < 0 ? errno : 0), EPERM);
            close(pd);
        }

        // ...and the things that can be polled still can
        int pf[2];
        if (pipe(pf) == 0) {
            ck("epoll_ctl(ADD) on a pipe still works",
               (errno = 0, epoll_ctl(ep, EPOLL_CTL_ADD, pf[0], &ev) < 0 ? errno : 0), 0);
            if (write(pf[1], "x", 1) != 1)
                failures_total++;
            struct epoll_event out;
            ck("  and its event arrives", epoll_wait(ep, &out, 1, 300), 1);
            close(pf[0]);
            close(pf[1]);
        }
        close(ep);
    }

    // ---- nfds is bounded, and above all does not blow the stack -----------
    {
        struct rlimit rl;
        getrlimit(RLIMIT_NOFILE, &rl);
        struct timeval tv = { 0, 0 };
        struct timespec zero_ts = { 0, 0 };
        ck("select(nfds = -1) is EINVAL",
           (errno = 0, select(-1, NULL, NULL, NULL, &tv) < 0 ? errno : 0), EINVAL);
        fd_set r;
        FD_ZERO(&r);
        errno = 0;
        long rc = syscall(SYS_pselect6, 2000000, &r, NULL, NULL, &zero_ts, NULL);
        ck("select(nfds = 2000000) is clamped, not refused", rc >= 0 ? 0 : errno, 0);
        struct pollfd one = { .fd = -1, .events = POLLIN };
        ck("poll(nfds past RLIMIT_NOFILE) is EINVAL",
           (errno = 0, syscall(SYS_ppoll, &one, (unsigned long) rl.rlim_cur + 1000,
                               &zero_ts, NULL, 0) < 0 ? errno : 0), EINVAL);
        ck("poll(nfds = 2000000) is EINVAL",
           (errno = 0, syscall(SYS_ppoll, &one, 2000000UL, &zero_ts, NULL, 0) < 0 ? errno : 0),
           EINVAL);
        // ...and ordinary sizes still work, which is the thing a clamp can break
        int pf[2];
        if (pipe(pf) == 0) {
            if (write(pf[1], "x", 1) != 1)
                failures_total++;
            struct pollfd p = { .fd = pf[0], .events = POLLIN };
            ck("an ordinary poll still works", poll(&p, 1, 300), 1);
            fd_set rs;
            FD_ZERO(&rs);
            FD_SET(pf[0], &rs);
            struct timeval t2 = { 0, 200000 };
            ck("an ordinary select still works", select(pf[0] + 1, &rs, NULL, NULL, &t2), 1);
            close(pf[0]);
            close(pf[1]);
        }
    }

    return finish_suite("poll_rdhup_bounds");
}
