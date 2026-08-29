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

enum what { W_READ, W_POLL };

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
        int eintr;
        if (what == W_READ) {
            char b;
            ssize_t r = read(fd[0], &b, 1);
            eintr = r < 0 && errno == EINTR;
        } else {
            struct pollfd p = { fd[0], POLLIN, 0 };
            int r = poll(&p, 1, (int) (20000 * scale));
            eintr = r < 0 && errno == EINTR;
        }
        _exit(eintr ? 1 : 0);
    }
    close(sync[1]);
    char rdy;
    if (read(sync[0], &rdy, 1) != 1) {}
    stop_then_cont(c, fd, 400);
    int st = 0;
    while (waitpid(c, &st, 0) < 0 && errno == EINTR)
        continue;
    int eintr = WIFEXITED(st) && WEXITSTATUS(st) == 1;
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

    case_deadline("poll deadline",      W_POLL);
    case_deadline("nanosleep deadline", W_READ);

    return finish_suite("signal_stop_restart");
}
