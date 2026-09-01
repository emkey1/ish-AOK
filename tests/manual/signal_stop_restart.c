// A job-control stop is not an interruption.
//
// SIGSTOP/SIGCONT going past a blocking syscall must not surface as EINTR:
// Linux parks the task inside the wait and resumes it, so read() and poll()
// and nanosleep() all carry on. That holds even for the interfaces SA_RESTART
// cannot rescue -- poll and nanosleep are ERESTARTNOHAND, which restarts
// whenever no handler runs, and a bare stop runs none.
//
// The one case that does surface is a SIGCONT with a handler installed: the
// handler runs, and a running handler cancels an ERESTARTNOHAND restart. read()
// still resumes there, because its handler asked for SA_RESTART.
//
// Restarting also has to resume the deadline the call already had rather than
// start its relative timeout over -- otherwise every ^Z/fg silently lengthens
// a timed wait. Measured against x86_64 glibc on Linux 6.12: a poll(3000ms)
// stopped at 2000ms for 300ms still returns at 3.00s, and so does a
// nanosleep(3s); the stopped time counts inside the deadline, it is not added
// to it.
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/select.h>
#include <signal.h>
#include <time.h>
#include <sys/wait.h>

#include "test_common.h"

static volatile sig_atomic_t cont_seen;
static void oncont(int s) { (void) s; cont_seen++; }

static long scale;
static void nap(long ms) {
    ms *= scale;
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}
static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec + ts.tv_nsec / 1e9;
}

static void install_cont_handler(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = oncont;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGCONT, &sa, NULL);
}

// Stop and continue `c`, then satisfy whatever it is waiting on.
static void stop_then_cont(pid_t c, int *fd, long block_ms) {
    nap(block_ms);
    kill(c, SIGSTOP);
    nap(300);
    kill(c, SIGCONT);
    nap(300);
    if (fd != NULL && write(fd[1], "z", 1) < 0) {}
}

// The whole ERESTARTNOHAND family, not just poll. All five leaked the raw
// -512 restart code to amd64 guests as errno 512 when a job-control stop
// interrupted them, and only poll was covered here -- so the other four could
// have regressed silently. See kernel/calls.c's amd64 native handler.
enum what { W_READ, W_POLL, W_PPOLL, W_SELECT, W_PSELECT, W_EPOLL };

// The blocking call under test, returning what the guest saw.
static int wait_once(enum what what, int fd, long scale) {
    switch (what) {
    case W_READ: { char b; return (int) read(fd, &b, 1); }
    case W_POLL: { struct pollfd p = { fd, POLLIN, 0 };
                   return poll(&p, 1, (int) (20000 * scale)); }
    case W_PPOLL: { struct pollfd p = { fd, POLLIN, 0 };
                    struct timespec t = { 20 * scale, 0 };
                    return ppoll(&p, 1, &t, NULL); }
    case W_SELECT: { fd_set r; FD_ZERO(&r); FD_SET(fd, &r);
                     struct timeval tv = { 20 * scale, 0 };
                     return select(fd + 1, &r, NULL, NULL, &tv); }
    case W_PSELECT: { fd_set r; FD_ZERO(&r); FD_SET(fd, &r);
                      struct timespec t = { 20 * scale, 0 };
                      return pselect(fd + 1, &r, NULL, NULL, &t, NULL); }
    default: { int e = epoll_create1(0);
               if (e < 0) return -1;
               struct epoll_event ev = { .events = EPOLLIN, .data.fd = fd };
               epoll_ctl(e, EPOLL_CTL_ADD, fd, &ev);
               struct epoll_event out;
               int r = epoll_wait(e, &out, 1, (int) (20000 * scale));
               close(e);
               return r; }
    }
}

// Child blocks; exits 1 if it saw EINTR, 0 if the call completed.
static void case_eintr(const char *label, enum what what, int handler, int want_eintr) {
    int fd[2], sync[2];
    if (pipe(fd) < 0 || pipe(sync) < 0) {
        printf("  %-22s SKIP (pipe unavailable)\n", label);
        return;
    }
    fflush(NULL);
    pid_t c = fork();
    if (c == 0) {
        close(fd[1]);
        if (handler)
            install_cont_handler();
        if (write(sync[1], "r", 1) < 0) {}
        errno = 0;
        int r = wait_once(what, fd[0], scale);
        // 1 = EINTR, 0 = completed, 2 = an errno that is neither -- which is
        // how the leaked internal restart code shows up. Distinguished so a
        // regression reads as "leaked -512" rather than merely "not EINTR".
        if (r >= 0)
            _exit(0);
        _exit(errno == EINTR ? 1 : 2);
    }
    close(sync[1]);
    char rdy;
    if (read(sync[0], &rdy, 1) != 1) {}
    stop_then_cont(c, fd, 400);
    int st = 0;
    while (waitpid(c, &st, 0) < 0 && errno == EINTR)
        continue;
    int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    if (code == 2) {
        printf("FAIL: %s: blocking call returned an errno that is neither EINTR "
               "nor success -- an internal restart code reached the guest\n", label);
        failf(label, 2, (uint64_t) st, 0, (uint64_t) want_eintr, 0, 0);
        test_logf("  %-22s LEAKED\n", label);
        close(fd[0]); close(fd[1]); close(sync[0]);
        return;
    }
    int eintr = code == 1;
    if (eintr != want_eintr)
        failf(label, (uint64_t) eintr, (uint64_t) st, 0, (uint64_t) want_eintr, 0, 0);
    test_logf("  %-22s %-9s expect %s\n", label, eintr ? "EINTR" : "completed",
              want_eintr ? "EINTR" : "completed");
    close(fd[0]); close(fd[1]); close(sync[0]);
}

// A restart must resume the deadline, not restart the relative timeout.
// Stopped late in the wait, so the two outcomes are far apart: resuming the
// deadline finishes at ~2s, starting over finishes at ~4s.
static void case_deadline(const char *label, enum what what) {
    int fd[2];
    if (pipe(fd) < 0) {
        printf("  %-22s SKIP (pipe unavailable)\n", label);
        return;
    }
    double budget = 2.0 * (double) scale;
    fflush(NULL);
    pid_t c = fork();
    if (c == 0) {
        close(fd[1]);
        double t0 = now_s();
        if (what == W_POLL) {
            struct pollfd p = { fd[0], POLLIN, 0 };   // never readable
            poll(&p, 1, (int) (2000 * scale));
        } else {
            struct timespec ts = { 2 * scale, 0 };
            nanosleep(&ts, NULL);
        }
        // centiseconds, so the parent can read it out of the exit status
        double took = now_s() - t0;
        int cs = (int) (took * 100);
        _exit(cs > 250 ? 250 : cs);
    }
    nap(1800);
    kill(c, SIGSTOP);
    nap(200);
    kill(c, SIGCONT);
    int st = 0;
    while (waitpid(c, &st, 0) < 0 && errno == EINTR)
        continue;
    close(fd[0]); close(fd[1]);
    if (!WIFEXITED(st)) {
        printf("  %-22s SKIP (child signalled)\n", label);
        return;
    }
    // The status caps at 250cs, which already exceeds the ceiling below, so a
    // restarted-from-scratch wait is still distinguishable from a resumed one.
    double took = WEXITSTATUS(st) / 100.0;
    double ceiling = budget * 1.6;
    if (took >= ceiling)
        failf(label, (uint64_t) (took * 100), 0, 0, (uint64_t) (ceiling * 100), 0, 0);
    test_logf("  %-22s took %.2fs (budget %.1fs, ceiling %.2fs)\n", label, took, budget, ceiling);
}

// A write(2) interrupted by a job-control stop must restart transparently or
// give EINTR -- never an internal restart code. On amd64 it gave errno 85:
// realfs_write returns EINTR as soon as a signal is pending (pipe fullness is
// not involved), sys_write_common turns that into _ERESTART meaning "restart
// transparently", and the amd64 native dispatch wrote that straight to RAX.
// A user hit it by pressing ^Z on anything writing to a pipe.
//
// Deliberately not routed through case_eintr: this is about the WRITING side,
// and the child must keep writing across several stop/continue rounds for the
// window to be hit at all.
static void case_write_across_stop(const char *label) {
    int fd[2];
    if (pipe(fd) < 0) {
        printf("  %-22s SKIP (pipe unavailable)\n", label);
        return;
    }
    fflush(NULL);
    pid_t c = fork();
    if (c == 0) {
        close(fd[0]);
        char buf[64];
        memset(buf, 'x', sizeof buf);
        for (int i = 0; i < 400; i++) {
            errno = 0;
            if (write(fd[1], buf, sizeof buf) < 0) {
                if (errno == EINTR)
                    continue;
                // 250 caps the exit status; anything above 200 is the bug.
                _exit(errno > 200 ? 200 : errno);
            }
            nap(2);
        }
        _exit(0);
    }
    close(fd[1]);
    for (int i = 0; i < 6; i++) {
        nap(60); kill(c, SIGSTOP);
        nap(60); kill(c, SIGCONT);
        char b[512];
        if (read(fd[0], b, sizeof b) <= 0)
            break;
    }
    int st = 0;
    for (;;) {
        if (waitpid(c, &st, WNOHANG) == c)
            break;
        char b[512];
        if (read(fd[0], b, sizeof b) <= 0)
            break;
    }
    while (waitpid(c, &st, 0) < 0 && errno == EINTR)
        continue;
    int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    if (code != 0) {
        printf("FAIL: %s: write returned errno %d across a stop -- %s\n", label, code,
               code == 200 ? "an internal restart code reached the guest"
                           : "neither success nor EINTR");
        failf(label, (uint64_t) code, 0, 0, 0, 0, 0);
    }
    test_logf("  %-22s exit=%d\n", label, code);
    close(fd[0]);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(300));
    scale = (long) test_watchdog_secs(1);

    case_eintr("read, no handler",   W_READ, 0, 0);
    case_eintr("read, SIGCONT hdlr", W_READ, 1, 0);
    case_eintr("poll, no handler",   W_POLL, 0, 0);
    // The handler runs, and a running handler cancels an ERESTARTNOHAND
    // restart -- so this one, alone, is a genuine EINTR.
    case_eintr("poll, SIGCONT hdlr", W_POLL, 1, 1);
    // The rest of the ERESTARTNOHAND family, same rule. Each of these handed
    // an amd64 guest errno 512 until the amd64 native dispatch started
    // honouring the restart protocol.
    case_eintr("ppoll, SIGCONT hdlr",   W_PPOLL,   1, 1);
    case_eintr("ppoll, no handler",     W_PPOLL,   0, 0);
    case_eintr("select, SIGCONT hdlr",  W_SELECT,  1, 1);
    case_eintr("select, no handler",    W_SELECT,  0, 0);
    case_eintr("pselect, SIGCONT hdlr", W_PSELECT, 1, 1);
    case_eintr("pselect, no handler",   W_PSELECT, 0, 0);
    // epoll is EINTR either way, unlike every other member of this family.
    // Linux's ep_poll() breaks out with -EINTR the moment a signal is pending
    // and never reaches the restart machinery, so a job-control stop that
    // leaves poll() completing gives epoll_wait() EINTR whether a handler ran
    // or not. Measured on Devuan 6 / Linux 6.12; AOK used to share poll's
    // restart path and complete, which is what this pair now pins down.
    case_eintr("epoll, SIGCONT hdlr",   W_EPOLL,   1, 1);
    case_eintr("epoll, no handler",     W_EPOLL,   0, 1);

    case_write_across_stop("write across a stop");

    case_deadline("poll deadline",      W_POLL);
    case_deadline("nanosleep deadline", W_READ);

    return finish_suite("signal_stop_restart");
}
