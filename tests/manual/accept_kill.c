// A task blocked in accept(2) on a listening socket with no incoming
// connection must die promptly on a fatal signal. Under iSH it could survive
// kill -9 forever: `nc -l -p 9999` sat in state S with SIGKILL pending
// (/proc/PID/status SigPnd bit 9 set, SigBlk/SigIgn/SigCgt all zero) and
// nothing could reap it short of killing the whole emulator.
//
// sys_accept4_common's wait was the one blocking wait in the tree that called
// the host poll() bare. Every sibling wait -- socket_wait_ready() and the
// send/recv paths via socket_blocking_syscall_begin(), fs/poll.c's poll_wait,
// fs/real.c's realfs_wait_readable/writable -- first blocks SIGUSR1, arms the
// sigunwind_start() point, re-checks for an already-pending guest signal, and
// only then force-unblocks SIGUSR1 and sleeps. The accept wait did none of
// that, which is wrong twice over:
//
//   1. Nothing guaranteed SIGUSR1 was unblocked in the host thread mask
//      (guest sigprocmask moves only the guest mask -- the reason poll.c:746
//      and socket_blocking_syscall_begin force-unblock it), and with no unwind
//      point armed the wake depended entirely on the host poll() returning
//      EINTR. Nothing else in the tree relies on that.
//   2. The pending-signal check and the sleep were not atomic. iSH wakes a
//      blocked task by setting the pending bit and poking it with SIGUSR1
//      (kernel/signal.c signal_wake_task). A poke landing between the
//      accept() EAGAIN and the poll() ran a handler that did nothing, and the
//      task then slept with the fatal signal already pending. Further kills
//      could not rescue it: they re-sent the same poke into the same deaf
//      sleep (measured on device -- 200 further signals, zero CPU time
//      consumed, SigPnd just accumulating). Only an actual incoming
//      connection could still wake it, via fd readiness.
//
// Same bug class as the raw host write in fs/real.c's realfs_wait_writable,
// whose comment describes the identical "blocks in the raw host syscall with
// none of the sigunwind_start()/SIGUSR1-unblocking machinery, so it cannot be
// interrupted by anything -- not even SIGKILL" failure.
//
// The race is timing-dependent, so this sweeps the vulnerable window: the
// child reports readiness immediately before calling accept(), and the parent
// varies its delay before signalling across trials. A single survivor is a
// failure. Survivors are rescued with a real connection so an unfixed A/B run
// does not strand unkillable listeners.
//
// A/B: the unfixed build fails 3/3 on the CLI and fails on the device
// (aarch64 Devuan, iSH-AOK 546), in every case losing both SIGKILL and
// SIGTERM at the zero-delay trial, which is the window itself; the fixed
// build passes 120/120 trials on the CLI. Two verifications are still
// outstanding: the fixed build has not run on device (546 is unfixed there,
// so re-run a literal `nc -l -p 9999` plus kill -9, and a long-park kill,
// once a fixed build deploys), and the real-Linux oracle run has not happened
// (mint was unreachable). Both are expected green -- on real Linux a task in
// accept() has always died immediately on SIGKILL.
#define _GNU_SOURCE
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include "test_common.h"

#define KILL_TRIALS 60

// Bind an ephemeral loopback listener so trials never collide on a fixed port
// and a wedged child can always be rescued by connecting to its own port.
static int listen_ephemeral(uint16_t *port_out) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0)
        return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = 0;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, (struct sockaddr *) &a, sizeof a) < 0) { close(s); return -1; }
    if (listen(s, 5) < 0) { close(s); return -1; }
    socklen_t sl = sizeof a;
    if (getsockname(s, (struct sockaddr *) &a, &sl) < 0) { close(s); return -1; }
    *port_out = ntohs(a.sin_port);
    return s;
}

// Child: park in accept() with no peer, after telling the parent the port.
static void child_park_in_accept(int wfd) {
    uint16_t port = 0;
    int s = listen_ephemeral(&port);
    if (s < 0)
        _exit(90);
    if (write(wfd, &port, sizeof port) != (ssize_t) sizeof port)
        _exit(91);
    close(wfd);
    accept(s, NULL, NULL);
    // Only reached if something connected; the parent treats that as a rescue.
    _exit(0);
}

// Poll for the child's exit up to `secs`. Returns 1 if reaped.
static int reap_within(pid_t pid, unsigned secs) {
    for (unsigned i = 0; i < secs * 100; i++) {
        int st;
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid)
            return 1;
        if (r < 0 && errno == ECHILD)
            return 1;
        usleep(10 * 1000);
    }
    return 0;
}

// Wake a wedged listener the only way that still works, so a failing run does
// not leave unkillable processes behind.
static void rescue_connect(uint16_t port) {
    int c = socket(AF_INET, SOCK_STREAM, 0);
    if (c < 0)
        return;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    (void) !connect(c, (struct sockaddr *) &a, sizeof a);
    close(c);
}

// One trial: fork a child that parks in accept(), signal it after `delay_us`,
// and require it to die. Returns 1 on failure (survived).
static int trial(int sig, unsigned delay_us, const char *label) {
    int fd[2];
    if (pipe(fd) < 0) {
        failf("pipe", (uint64_t) errno, 0, 0, 0, 0, 0);
        return 1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        failf("fork", (uint64_t) errno, 0, 0, 0, 0, 0);
        close(fd[0]); close(fd[1]);
        return 1;
    }
    if (pid == 0) {
        close(fd[0]);
        child_park_in_accept(fd[1]);
        _exit(0);
    }
    close(fd[1]);

    uint16_t port = 0;
    if (read(fd[0], &port, sizeof port) != (ssize_t) sizeof port) {
        close(fd[0]);
        kill(pid, SIGKILL);
        reap_within(pid, 5);
        failf("child-setup", 0, 0, 0, 0, 0, 0);
        return 1;
    }
    close(fd[0]);

    if (delay_us > 0)
        usleep(delay_us);
    if (kill(pid, sig) < 0) {
        failf("kill", (uint64_t) errno, 0, 0, 0, 0, 0);
        return 1;
    }

    if (reap_within(pid, test_watchdog_secs(5))) {
        test_logf("  %s delay=%uus: died\n", label, delay_us);
        return 0;
    }

    // Survived a fatal signal. Confirm the documented "further kills do not
    // rescue it" property before recovering, since that is what makes the bug
    // unrecoverable in practice rather than merely slow.
    kill(pid, SIGKILL);
    int rescued_by_second_kill = reap_within(pid, 2);
    if (!rescued_by_second_kill) {
        rescue_connect(port);
        reap_within(pid, test_watchdog_secs(5));
    }
    printf("FAIL %s sig=%d delay=%uus: survived fatal signal%s\n",
           label, sig, delay_us,
           rescued_by_second_kill ? "" : " (and a second SIGKILL)");
    failures_total++;
    return 1;
}

// Sweep the window between the accept() EAGAIN and the poll() sleep. The child
// signals readiness immediately before accept(), so small delays land near the
// vulnerable window and larger ones confirm a fully-parked task is killable.
static void test_fatal_signal_sweep(int sig, const char *label) {
    for (int i = 0; i < KILL_TRIALS; i++) {
        unsigned delay_us = (unsigned) (i % 20) * 100;   // 0..1900us
        if (i % 10 == 9)
            delay_us = 200 * 1000;                       // fully parked
        if (trial(sig, delay_us, label))
            return;                                      // one survivor is enough
    }
    test_logf("%s: %d trials, all died\n", label, KILL_TRIALS);
}

// The fix must not break accept() itself: a real connection is still accepted.
static void test_accept_still_works(void) {
    int fd[2];
    if (pipe(fd) < 0) {
        failf("pipe2", (uint64_t) errno, 0, 0, 0, 0, 0);
        return;
    }
    pid_t pid = fork();
    if (pid < 0) {
        failf("fork2", (uint64_t) errno, 0, 0, 0, 0, 0);
        return;
    }
    if (pid == 0) {
        close(fd[0]);
        uint16_t port = 0;
        int s = listen_ephemeral(&port);
        if (s < 0) _exit(90);
        if (write(fd[1], &port, sizeof port) != (ssize_t) sizeof port) _exit(91);
        close(fd[1]);
        int c = accept(s, NULL, NULL);
        _exit(c >= 0 ? 0 : 92);
    }
    close(fd[1]);
    uint16_t port = 0;
    if (read(fd[0], &port, sizeof port) != (ssize_t) sizeof port) {
        close(fd[0]);
        kill(pid, SIGKILL);
        reap_within(pid, 5);
        failf("accept-works-setup", 0, 0, 0, 0, 0, 0);
        return;
    }
    close(fd[0]);
    usleep(100 * 1000);
    rescue_connect(port);

    int st = 0;
    if (!reap_within(pid, test_watchdog_secs(5))) {
        kill(pid, SIGKILL);
        reap_within(pid, 5);
        failf("accept-works-hang", 0, 0, 0, 0, 0, 0);
        return;
    }
    (void) st;
    test_logf("accept still accepts a real connection\n");
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    // Generous backstop: the whole suite is bounded so a genuine wedge fails
    // the run instead of hanging it.
    alarm(test_watchdog_secs(180));
    signal(SIGPIPE, SIG_IGN);

    test_accept_still_works();
    test_fatal_signal_sweep(SIGKILL, "sigkill-in-accept");
    test_fatal_signal_sweep(SIGTERM, "sigterm-in-accept");

    return finish_suite("accept_kill");
}
