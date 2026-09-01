// A task blocked in recv(2) or send(2) must die promptly on a fatal signal.
// Under AOK it could survive kill -9 forever, the same defect accept_kill.c
// covers for accept(2) and the rest of the bug class that fix deliberately
// left open.
//
// The root cause is not in the signal path: a SIGUSR1 poke aimed at a host
// thread that is ALREADY blocked inside a host wait is simply never delivered.
// Measured with 8 guest tasks parked in accept(2) and all SIGKILLed, every
// pthread_kill returned 0 to a distinct, correct, started thread, each parked
// with SIGUSR1 unblocked in its host mask and its sigunwind point armed (both
// confirmed by in-process instrumentation immediately before the wait -- lldb
// masks signals during expression evaluation and reports a false blocked mask
// there). Only the first one or two ever ran the handler; the rest slept on
// with SIGKILL pending, permanently deaf, and a second SIGKILL did not rescue
// them. kernel/signal.c's poll_notify_poke comment documents the loss;
// fs/poll.c defends with a non-lossy notify pipe and fs/real.c with a 100ms
// timeout.
//
// fs/sock.c had no such defence. sys_recvfrom_common, sys_sendmsg, sock_read
// and sock_write all blocked directly in the host recvmsg/sendmsg/read/write,
// where a pipe fd cannot be added to the wait at all, and socket_wait_ready()
// slept in an infinite host poll() with neither a notify pipe nor a timeout.
// The fix keeps the host description nonblocking (as sys_accept4_common
// already does for listeners) so every wait happens in socket_wait_ready's
// poll(), which now publishes the same notify pipe.
//
// PARALLELISM IS THE TRIGGER, not the socket configuration. One task at a time
// always wakes, on every socket type and option combination tried, so a test
// that kills one child at a time passes vacuously on a fully broken build.
// Eight parked at once wedges most of them every round.
//
// Making the host call nonblocking moves a pile of semantics from the host
// kernel into fs/sock.c, so the second half of this file guards those: a
// blocking stream send must still transmit everything (Darwin returns partial
// counts once the fd is nonblocking, which would silently truncate every
// write(sock, buf, n)), MSG_WAITALL must still fill the buffer (Darwin returns
// EAGAIN rather than a short read on a nonblocking socket, so passing the flag
// through would spin forever), SO_RCVTIMEO/SO_SNDTIMEO are no longer enforced
// by the host at all, and a partial sendmsg must not repeat its control
// message and hand the peer a second copy of an SCM_RIGHTS parcel.
//
// A/B: unfixed fails the parallel rounds (7 of 8 parked tasks survive SIGKILL
// on both AF_UNIX and TCP); fixed passes. The semantic half also passes on
// real Linux, where it is checking that AOK still behaves like the kernel.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "test_common.h"

#define PARALLEL_KIDS 8
#define PARALLEL_ROUNDS 3

// Larger than any plausible socket buffer, so one send() of this cannot be
// satisfied without the peer reading.
#define BIG_SEND (4 * 1024 * 1024)

static char big_buf[BIG_SEND];

// ---------------------------------------------------------------- helpers

// Returns 1 if the child was reaped within `secs`, storing its wait status in
// *st_out when given. Polling rather than blocking so a wedged child fails the
// run instead of hanging it.
static int reap_status_within(pid_t pid, unsigned secs, int *st_out) {
    for (unsigned i = 0; i < secs * 100; i++) {
        int st = 0;
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) {
            if (st_out != NULL)
                *st_out = st;
            return 1;
        }
        if (r < 0 && errno == ECHILD)
            return 1;
        usleep(10 * 1000);
    }
    return 0;
}

static int reap_within(pid_t pid, unsigned secs) {
    return reap_status_within(pid, secs, NULL);
}

// A connected AF_INET stream pair over loopback, so the TCP rounds exercise a
// different host transport from the AF_UNIX ones.
static int tcp_pair(int sv[2]) {
    int l = socket(AF_INET, SOCK_STREAM, 0);
    if (l < 0)
        return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t al = sizeof a;
    if (bind(l, (struct sockaddr *) &a, sizeof a) < 0 || listen(l, 4) < 0 ||
            getsockname(l, (struct sockaddr *) &a, &al) < 0) {
        close(l);
        return -1;
    }
    int c = socket(AF_INET, SOCK_STREAM, 0);
    if (c < 0) { close(l); return -1; }
    if (connect(c, (struct sockaddr *) &a, sizeof a) < 0) { close(l); close(c); return -1; }
    int s = accept(l, NULL, NULL);
    close(l);
    if (s < 0) { close(c); return -1; }
    sv[0] = c;
    sv[1] = s;
    return 0;
}

static int make_pair(int unix_domain, int sv[2]) {
    if (unix_domain)
        return socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    return tcp_pair(sv);
}

// Read everything currently queued, so a wedged sender can be rescued.
static void drain(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    (void) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    for (;;) {
        ssize_t r = read(fd, big_buf, sizeof big_buf);
        if (r <= 0)
            break;
    }
    (void) fcntl(fd, F_SETFL, fl);
}

// ------------------------------------------------------- the wedge itself

enum park_kind { PARK_RECV, PARK_SEND };

// Child: report readiness, then block. The report goes out immediately before
// the blocking call so a signal can land anywhere in the vulnerable window.
static void child_park(int sock, int ready_fd, enum park_kind kind) {
    char byte = 'r';
    if (write(ready_fd, &byte, 1) != 1)
        _exit(91);
    close(ready_fd);
    if (kind == PARK_RECV) {
        char buf[64];
        recv(sock, buf, sizeof buf, 0);
    } else {
        // Fills the socket buffer and then parks in the wait with the request
        // only partly sent -- the send-side equivalent of an empty recv.
        send(sock, big_buf, BIG_SEND, 0);
    }
    _exit(0); // only reached if the parent rescued this child
}

// N tasks parked at once, all signalled together. This is the only shape that
// reproduces the lost poke; see the header.
static void test_parallel_kill(int sig, int unix_domain, enum park_kind kind,
                               const char *label) {
    for (int r = 0; r < PARALLEL_ROUNDS; r++) {
        pid_t kids[PARALLEL_KIDS];
        int peers[PARALLEL_KIDS];   // parent's end, kept for the rescue
        int ready[PARALLEL_KIDS][2];
        int started = 0;

        for (int i = 0; i < PARALLEL_KIDS; i++) {
            int sv[2];
            if (make_pair(unix_domain, sv) < 0)
                break;
            if (pipe(ready[i]) < 0) { close(sv[0]); close(sv[1]); break; }
            pid_t p = fork();
            if (p < 0) {
                close(sv[0]); close(sv[1]);
                close(ready[i][0]); close(ready[i][1]);
                break;
            }
            if (p == 0) {
                close(sv[0]);
                close(ready[i][0]);
                for (int j = 0; j < started; j++) {
                    close(peers[j]);
                    close(ready[j][0]);
                }
                child_park(sv[1], ready[i][1], kind);
                _exit(0);
            }
            close(sv[1]);
            close(ready[i][1]);
            peers[i] = sv[0];
            kids[i] = p;
            started++;
        }
        if (started == 0) {
            failf("parallel-setup", (uint64_t) errno, 0, 0, 0, 0, 0);
            return;
        }

        // Every child parked before any of them is signalled.
        for (int i = 0; i < started; i++) {
            char byte;
            (void) !read(ready[i][0], &byte, 1);
            close(ready[i][0]);
        }
        usleep(200 * 1000);
        for (int i = 0; i < started; i++)
            kill(kids[i], sig);

        int survivors = 0;
        for (int i = 0; i < started; i++) {
            if (reap_within(kids[i], test_watchdog_secs(5))) {
                close(peers[i]);
                continue;
            }
            survivors++;
            // Wake it the one way that still works on a broken build, so a
            // failing A/B run strands no unkillable processes.
            if (kind == PARK_RECV)
                (void) !write(peers[i], "x", 1);
            else
                drain(peers[i]);
            reap_within(kids[i], test_watchdog_secs(5));
            close(peers[i]);
        }
        if (survivors != 0) {
            printf("FAIL %s round %d: %d of %d parked tasks survived %s\n",
                   label, r, survivors, started,
                   sig == SIGKILL ? "SIGKILL" : "the fatal signal");
            failures_total++;
            return;
        }
        test_logf("  %s round %d: all %d died\n", label, r, started);
    }
}

// ----------------------------------------------------- semantics guards

// A blocking send on a stream must transmit the whole request. Once the host
// fd is nonblocking the host returns a partial count, so AOK has to finish the
// job itself; getting this wrong silently truncates guest writes.
static void test_blocking_send_is_complete(int use_write, const char *label) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        failf(label, (uint64_t) errno, 0, 0, 0, 0, 0);
        return;
    }
    pid_t pid = fork();
    if (pid < 0) {
        failf(label, (uint64_t) errno, 0, 0, 0, 0, 0);
        close(sv[0]); close(sv[1]);
        return;
    }
    if (pid == 0) {
        // Deliberately slow, so the sender really does have to wait repeatedly.
        close(sv[0]);
        size_t got = 0;
        while (got < BIG_SEND) {
            ssize_t r = read(sv[1], big_buf, 64 * 1024);
            if (r <= 0)
                break;
            got += (size_t) r;
            usleep(1000);
        }
        _exit(got == BIG_SEND ? 0 : 1);
    }
    close(sv[1]);
    ssize_t sent = use_write ? write(sv[0], big_buf, BIG_SEND)
                             : send(sv[0], big_buf, BIG_SEND, 0);
    if (sent != BIG_SEND)
        failf(label, (uint64_t) sent, (uint64_t) errno, 0, BIG_SEND, 0, 0);
    close(sv[0]);
    int st = 0;
    if (!reap_status_within(pid, test_watchdog_secs(30), &st))
        kill(pid, SIGKILL), failf("blocking-send-reader-hang", 0, 0, 0, 0, 0, 0);
    else if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
        failf("blocking-send-short-read", (uint64_t) (WIFEXITED(st) ? WEXITSTATUS(st) : -1),
              0, 0, 0, 0, 0);
    test_logf("%s: sent %d bytes in one call\n", label, BIG_SEND);
}

// Same contract over an iovec, which takes the harder resume path: the
// continuation runs off a scratch copy of the iovec array, because advancing
// the caller's would leave interior pointers in buffers it later frees.
static void test_blocking_writev_is_complete(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        failf("writev-setup", (uint64_t) errno, 0, 0, 0, 0, 0);
        return;
    }
    enum { CHUNKS = 4, CHUNK = BIG_SEND / CHUNKS };
    pid_t pid = fork();
    if (pid < 0) {
        failf("writev-fork", (uint64_t) errno, 0, 0, 0, 0, 0);
        close(sv[0]); close(sv[1]);
        return;
    }
    if (pid == 0) {
        close(sv[0]);
        size_t got = 0;
        int bad = 0;
        while (got < BIG_SEND) {
            char buf[32 * 1024];
            ssize_t r = read(sv[1], buf, sizeof buf);
            if (r <= 0)
                break;
            // Every byte must arrive exactly once and in order: the payload is
            // a position-dependent pattern, so a mis-advanced iovec shows up
            // as duplicated or skipped bytes rather than just a short count.
            for (ssize_t i = 0; i < r; i++) {
                if (buf[i] != (char) ((got + (size_t) i) & 0x7f))
                    bad = 1;
            }
            got += (size_t) r;
            usleep(1000);
        }
        _exit(got == BIG_SEND && !bad ? 0 : 1);
    }
    close(sv[1]);
    for (size_t i = 0; i < BIG_SEND; i++)
        big_buf[i] = (char) (i & 0x7f);
    struct iovec iov[CHUNKS];
    for (int i = 0; i < CHUNKS; i++) {
        iov[i].iov_base = big_buf + (size_t) i * CHUNK;
        iov[i].iov_len = CHUNK;
    }
    ssize_t sent = writev(sv[0], iov, CHUNKS);
    if (sent != BIG_SEND)
        failf("writev-complete", (uint64_t) sent, (uint64_t) errno, 0, BIG_SEND, 0, 0);
    close(sv[0]);
    int st = 0;
    if (!reap_status_within(pid, test_watchdog_secs(30), &st))
        kill(pid, SIGKILL), failf("writev-reader-hang", 0, 0, 0, 0, 0, 0);
    else if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
        failf("writev-payload", (uint64_t) (WIFEXITED(st) ? WEXITSTATUS(st) : -1),
              0, 0, 0, 0, 0);
    memset(big_buf, 0, BIG_SEND);
    test_logf("writev: %d bytes across %d iovecs, in order\n", BIG_SEND, CHUNKS);
}

// MSG_WAITALL must fill the buffer even when the data dribbles in. Passing the
// flag to a nonblocking host socket returns EAGAIN instead of a short read, so
// AOK strips it and accumulates; a mistake here hangs or short-reads.
static void test_waitall_fills(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        failf("waitall-setup", (uint64_t) errno, 0, 0, 0, 0, 0);
        return;
    }
    enum { WANT = 256 };
    pid_t pid = fork();
    if (pid < 0) {
        failf("waitall-fork", (uint64_t) errno, 0, 0, 0, 0, 0);
        close(sv[0]); close(sv[1]);
        return;
    }
    if (pid == 0) {
        close(sv[0]);
        char chunk[16];
        for (int i = 0; i < WANT / 16; i++) {
            memset(chunk, 'a' + (i % 26), sizeof chunk);
            if (write(sv[1], chunk, sizeof chunk) != (ssize_t) sizeof chunk)
                _exit(1);
            usleep(5 * 1000);
        }
        // Stay open: MSG_WAITALL may also stop at EOF, which would let a
        // broken accumulate loop pass by accident.
        usleep(500 * 1000);
        _exit(0);
    }
    close(sv[1]);
    char buf[WANT];
    memset(buf, 0, sizeof buf);
    ssize_t got = recv(sv[0], buf, WANT, MSG_WAITALL);
    if (got != WANT)
        failf("waitall-short", (uint64_t) got, (uint64_t) errno, 0, WANT, 0, 0);
    else if (buf[0] != 'a' || buf[WANT - 1] != (char) ('a' + ((WANT / 16 - 1) % 26)))
        failf("waitall-content", (uint64_t) buf[0], (uint64_t) buf[WANT - 1], 0, 'a', 0, 0);
    close(sv[0]);
    kill(pid, SIGKILL);
    reap_within(pid, test_watchdog_secs(5));
    test_logf("waitall: filled %d bytes from a dribbling writer\n", WANT);
}

// A peek that asks for more than has arrived must not consume anything.
static void test_waitall_peek_leaves_data(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        failf("peek-setup", (uint64_t) errno, 0, 0, 0, 0, 0);
        return;
    }
    const char *payload = "0123456789abcdef";
    if (write(sv[1], payload, 16) != 16) {
        failf("peek-write", (uint64_t) errno, 0, 0, 0, 0, 0);
        close(sv[0]); close(sv[1]);
        return;
    }
    char buf[16];
    memset(buf, 0, sizeof buf);
    ssize_t got = recv(sv[0], buf, 16, MSG_PEEK | MSG_WAITALL);
    if (got != 16 || memcmp(buf, payload, 16) != 0) {
        failf("peek-waitall", (uint64_t) got, (uint64_t) errno, 0, 16, 0, 0);
    } else {
        memset(buf, 0, sizeof buf);
        ssize_t again = recv(sv[0], buf, 16, 0);
        if (again != 16 || memcmp(buf, payload, 16) != 0)
            failf("peek-consumed", (uint64_t) again, 0, 0, 16, 0, 0);
    }
    close(sv[0]);
    close(sv[1]);
    test_logf("peek|waitall: data left in the socket\n");
}

static long ms_since(const struct timespec *start) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long) ((now.tv_sec - start->tv_sec) * 1000 +
                   (now.tv_nsec - start->tv_nsec) / 1000000);
}

// SO_RCVTIMEO/SO_SNDTIMEO were enforced by the host kernel only because the
// host call was the blocking one. With the wait emulated they are AOK's to
// honor, and a missed deadline means an unbounded block -- so both checks run
// in a child under a bounded reap. Without that, the failure they are built to
// catch takes out the whole suite with the watchdog instead of naming itself:
// on a build whose socket_wait_ready() polls with no timeout, the host's own
// SO_RCVTIMEO expiry lands in an infinite poll and recv() never returns.
#define TIMEO_MS 300

// Exit codes carry the reason, since the check itself runs in the child.
enum { TIMEO_OK = 0, TIMEO_SETUP = 4, TIMEO_RESULT = 5, TIMEO_TIMING = 6 };

static const char *timeo_reason(int code) {
    switch (code) {
        case TIMEO_SETUP:  return "setup/setsockopt failed";
        case TIMEO_RESULT: return "wrong return or errno";
        case TIMEO_TIMING: return "returned outside the deadline window";
        default:           return "unknown";
    }
}

static void timeo_report(const char *label, pid_t pid) {
    int st = 0;
    // Comfortably past the deadline, but far short of "hung".
    if (!reap_status_within(pid, test_watchdog_secs(20), &st)) {
        kill(pid, SIGKILL);
        reap_within(pid, test_watchdog_secs(5));
        printf("FAIL %s: blocked past its %dms timeout (never returned)\n",
               label, TIMEO_MS);
        failures_total++;
        return;
    }
    if (!WIFEXITED(st) || WEXITSTATUS(st) != TIMEO_OK) {
        int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        printf("FAIL %s: %s (code %d)\n", label, timeo_reason(code), code);
        failures_total++;
        return;
    }
    test_logf("%s: honored its %dms deadline\n", label, TIMEO_MS);
}

static void test_rcvtimeo(void) {
    pid_t pid = fork();
    if (pid < 0) {
        failf("rcvtimeo-fork", (uint64_t) errno, 0, 0, 0, 0, 0);
        return;
    }
    if (pid == 0) {
        int sv[2];
        struct timeval tv = {.tv_sec = 0, .tv_usec = TIMEO_MS * 1000};
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0 ||
                setsockopt(sv[0], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) < 0)
            _exit(TIMEO_SETUP);
        struct timespec start;
        clock_gettime(CLOCK_MONOTONIC, &start);
        char buf[16];
        errno = 0;
        ssize_t r = recv(sv[0], buf, sizeof buf, 0);
        long elapsed = ms_since(&start);
        if (r >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
            _exit(TIMEO_RESULT);
        if (elapsed < TIMEO_MS - 100 || elapsed > 5000)
            _exit(TIMEO_TIMING);
        _exit(TIMEO_OK);
    }
    timeo_report("rcvtimeo", pid);
}

static void test_sndtimeo(void) {
    pid_t pid = fork();
    if (pid < 0) {
        failf("sndtimeo-fork", (uint64_t) errno, 0, 0, 0, 0, 0);
        return;
    }
    if (pid == 0) {
        int sv[2];
        struct timeval tv = {.tv_sec = 0, .tv_usec = TIMEO_MS * 1000};
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0 ||
                setsockopt(sv[0], SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv) < 0)
            _exit(TIMEO_SETUP);
        struct timespec start;
        clock_gettime(CLOCK_MONOTONIC, &start);
        errno = 0;
        // Nobody reads sv[1], so this fills the buffer and then hits the
        // deadline. Linux reports the bytes that did get through, or EAGAIN if
        // none did; either is correct, an unbounded block is not.
        ssize_t r = send(sv[0], big_buf, BIG_SEND, 0);
        long elapsed = ms_since(&start);
        if (r == BIG_SEND)
            _exit(TIMEO_RESULT);
        if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
            _exit(TIMEO_RESULT);
        if (elapsed < TIMEO_MS - 100 || elapsed > 5000)
            _exit(TIMEO_TIMING);
        _exit(TIMEO_OK);
    }
    timeo_report("sndtimeo", pid);
}

// The guest's own O_NONBLOCK must still short-circuit, and must be what
// F_GETFL reports -- the host flag now deliberately diverges from it.
static void test_guest_nonblock_unaffected(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        failf("nonblock-setup", (uint64_t) errno, 0, 0, 0, 0, 0);
        return;
    }
    // Run one blocking call first, so the host fd is definitely forced
    // nonblocking before the guest-visible flags are inspected.
    if (write(sv[1], "x", 1) == 1) {
        char c;
        (void) !read(sv[0], &c, 1);
    }
    int fl = fcntl(sv[0], F_GETFL, 0);
    if (fl < 0 || (fl & O_NONBLOCK))
        failf("nonblock-leaked", (uint64_t) fl, 0, 0, 0, 0, 0);

    (void) fcntl(sv[0], F_SETFL, fl | O_NONBLOCK);
    char buf[16];
    errno = 0;
    ssize_t r = recv(sv[0], buf, sizeof buf, 0);
    if (r >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
        failf("nonblock-eagain", (uint64_t) r, (uint64_t) errno, 0, (uint64_t) -1, EAGAIN, 0);
    int fl2 = fcntl(sv[0], F_GETFL, 0);
    if (fl2 < 0 || !(fl2 & O_NONBLOCK))
        failf("nonblock-readback", (uint64_t) fl2, 0, 0, 0, 0, 0);

    // ...and clearing it must restore blocking behaviour rather than leaving
    // the guest wedged on a host flag it cannot see.
    (void) fcntl(sv[0], F_SETFL, fl2 & ~O_NONBLOCK);
    if (write(sv[1], "y", 1) != 1)
        failf("nonblock-write", (uint64_t) errno, 0, 0, 0, 0, 0);
    else if (recv(sv[0], buf, 1, 0) != 1)
        failf("nonblock-restore", (uint64_t) errno, 0, 0, 1, 0, 0);
    close(sv[0]);
    close(sv[1]);
    test_logf("guest O_NONBLOCK: honored and reported, host flag invisible\n");
}

// A partial sendmsg must not repeat its control message. The continuation
// carries the remaining bytes only; resending the cmsg would hand the peer a
// second copy of the SCM_RIGHTS parcel, which on a Wayland or X11 socket means
// leaked descriptors on every large message.
static void test_scm_rights_sent_once(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        failf("scm-setup", (uint64_t) errno, 0, 0, 0, 0, 0);
        return;
    }
    int payload_fd = dup(STDIN_FILENO);
    if (payload_fd < 0)
        payload_fd = open("/dev/null", O_RDONLY);
    if (payload_fd < 0) {
        failf("scm-fd", (uint64_t) errno, 0, 0, 0, 0, 0);
        close(sv[0]); close(sv[1]);
        return;
    }
    enum { SCM_BYTES = 1024 * 1024 };
    pid_t pid = fork();
    if (pid < 0) {
        failf("scm-fork", (uint64_t) errno, 0, 0, 0, 0, 0);
        close(sv[0]); close(sv[1]); close(payload_fd);
        return;
    }
    if (pid == 0) {
        // Reader: consume the whole message, counting every descriptor that
        // arrives with any part of it.
        close(sv[0]);
        close(payload_fd);
        size_t got = 0;
        int fds_seen = 0;
        while (got < SCM_BYTES) {
            char buf[64 * 1024];
            char control[CMSG_SPACE(sizeof(int) * 8)];
            struct iovec iov = {.iov_base = buf, .iov_len = sizeof buf};
            struct msghdr msg;
            memset(&msg, 0, sizeof msg);
            msg.msg_iov = &iov;
            msg.msg_iovlen = 1;
            msg.msg_control = control;
            msg.msg_controllen = sizeof control;
            ssize_t r = recvmsg(sv[1], &msg, 0);
            if (r <= 0)
                break;
            got += (size_t) r;
            for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c != NULL;
                 c = CMSG_NXTHDR(&msg, c)) {
                if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS)
                    continue;
                int n = (int) ((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
                for (int i = 0; i < n; i++) {
                    int fd;
                    memcpy(&fd, CMSG_DATA(c) + i * sizeof(int), sizeof fd);
                    if (fd >= 0)
                        close(fd);
                    fds_seen++;
                }
            }
            usleep(500);
        }
        if (got != SCM_BYTES)
            _exit(2);
        _exit(fds_seen == 1 ? 0 : 3 + (fds_seen > 9 ? 9 : fds_seen));
    }
    close(sv[1]);
    struct iovec iov = {.iov_base = big_buf, .iov_len = SCM_BYTES};
    char control[CMSG_SPACE(sizeof(int))];
    memset(control, 0, sizeof control);
    struct msghdr msg;
    memset(&msg, 0, sizeof msg);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof control;
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &payload_fd, sizeof payload_fd);
    ssize_t sent = sendmsg(sv[0], &msg, 0);
    if (sent != SCM_BYTES)
        failf("scm-sendmsg-short", (uint64_t) sent, (uint64_t) errno, 0, SCM_BYTES, 0, 0);
    close(sv[0]);
    close(payload_fd);
    int st = 0;
    if (!reap_status_within(pid, test_watchdog_secs(30), &st)) {
        kill(pid, SIGKILL);
        failf("scm-reader-hang", 0, 0, 0, 0, 0, 0);
    } else if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        // Exit 2 is a short message; 3+n means n descriptors arrived where
        // exactly one should have.
        failf("scm-rights-count", (uint64_t) (WIFEXITED(st) ? WEXITSTATUS(st) : -1),
              0, 0, 0, 0, 0);
    }
    test_logf("scm_rights: one parcel across a %d byte message\n", SCM_BYTES);
}

// The wedge fix must not stop sockets working: an ordinary blocking recv still
// wakes on data, and a datagram round-trip still carries its payload.
static void test_ordinary_io_still_works(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        failf("ordinary-setup", (uint64_t) errno, 0, 0, 0, 0, 0);
        return;
    }
    pid_t pid = fork();
    if (pid < 0) {
        failf("ordinary-fork", (uint64_t) errno, 0, 0, 0, 0, 0);
        close(sv[0]); close(sv[1]);
        return;
    }
    if (pid == 0) {
        close(sv[0]);
        usleep(200 * 1000);      // the reader is blocked well before this
        (void) !write(sv[1], "hello", 5);
        _exit(0);
    }
    close(sv[1]);
    char buf[16];
    memset(buf, 0, sizeof buf);
    ssize_t r = recv(sv[0], buf, sizeof buf, 0);
    if (r != 5 || memcmp(buf, "hello", 5) != 0)
        failf("ordinary-recv", (uint64_t) r, (uint64_t) errno, 0, 5, 0, 0);
    close(sv[0]);
    reap_within(pid, test_watchdog_secs(5));

    int dv[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, dv) == 0) {
        if (send(dv[0], "dgram", 5, 0) != 5)
            failf("ordinary-dgram-send", (uint64_t) errno, 0, 0, 5, 0, 0);
        else {
            memset(buf, 0, sizeof buf);
            ssize_t dr = recv(dv[1], buf, sizeof buf, 0);
            if (dr != 5 || memcmp(buf, "dgram", 5) != 0)
                failf("ordinary-dgram-recv", (uint64_t) dr, 0, 0, 5, 0, 0);
        }
        close(dv[0]);
        close(dv[1]);
    }
    test_logf("ordinary I/O: stream wake and datagram round-trip intact\n");
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    // Unbuffered: the failure mode under test is a hang, and a run killed by
    // the watchdog must still show how far it got rather than dying with its
    // progress stuck in a stdio buffer.
    setvbuf(stdout, NULL, _IONBF, 0);
    // Backstop only: a correct run finishes far under this, and a genuine
    // wedge should fail the run rather than hang it.
    alarm(test_watchdog_secs(300));
    signal(SIGPIPE, SIG_IGN);

    // Ordinary I/O first: if sockets are broken outright, everything below
    // fails for uninteresting reasons.
    test_ordinary_io_still_works();

    // The headline coverage, before the semantics guards, so a build without
    // the fix reports the wedge rather than tripping over a lesser symptom on
    // the way there.
    test_parallel_kill(SIGKILL, 1, PARK_RECV, "parallel-sigkill-recv-unix");
    test_parallel_kill(SIGTERM, 1, PARK_RECV, "parallel-sigterm-recv-unix");
    test_parallel_kill(SIGKILL, 0, PARK_RECV, "parallel-sigkill-recv-tcp");
    test_parallel_kill(SIGKILL, 1, PARK_SEND, "parallel-sigkill-send-unix");
    test_parallel_kill(SIGKILL, 0, PARK_SEND, "parallel-sigkill-send-tcp");

    // Everything the host kernel used to do for us and now does not.
    test_blocking_send_is_complete(0, "blocking-send-complete");
    test_blocking_send_is_complete(1, "blocking-write-complete");
    test_blocking_writev_is_complete();
    test_waitall_fills();
    test_waitall_peek_leaves_data();
    test_rcvtimeo();
    test_sndtimeo();
    test_guest_nonblock_unaffected();
    test_scm_rights_sent_once();

    return finish_suite("socket_kill");
}
