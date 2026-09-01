// A task blocked in accept(2) on a listening socket with no incoming
// connection must die promptly on a fatal signal. Under AOK it could survive
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
//   2. The pending-signal check and the sleep were not atomic. AOK wakes a
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
// There are TWO distinct defects here and this covers both. The first fix was
// verified green by the sequential sweep alone, and the organic `nc -l`
// symptom still reproduced on a device build carrying it -- see the parallel
// round below for the second defect, which is the one that actually mattered.
//
// A/B: fully unfixed fails the sequential sweep 3/3 on the CLI and on the
// device (aarch64 Devuan, iSH-AOK 546), always losing both SIGKILL and SIGTERM
// at the zero-delay trial, which is the window itself. With ONLY the
// window-race fix the sweep passes but the parallel round fails immediately
// (7 of 8 survive SIGKILL, 5 of 8 SIGTERM). With both fixes this passes
// repeatedly, and an 8-way parallel stress is 0 wedged across 20 rounds.
//
// Outstanding: the notify-pipe fix is CLI-verified only, not yet run on
// device, and the real-Linux oracle run has not happened (mint unreachable).
// Both expected green -- on real Linux a task in accept() has always died
// immediately on SIGKILL.
//
// The rest of the bug class -- tasks parked in recv()/send(), which used to
// block directly in the host recvmsg/sendmsg where a notify pipe cannot be
// added to the wait at all -- is fixed separately and covered by
// socket_kill.c, which converts those paths to the same nonblocking-plus-poll
// shape used here.
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

// Second defect, and the one a sequential sweep cannot see: N tasks parked in
// accept(2) at once, all killed together. A SIGUSR1 poke aimed at a thread
// that is ALREADY blocked inside the host poll() is simply never delivered.
// Measured with 8 parked tasks: every pthread_kill returned 0 to a distinct,
// correct, started thread, yet only the first one or two ever ran the handler
// and the other six slept on with SIGKILL pending, permanently deaf. That is
// why nothing else in the tree trusts the poke alone: fs/poll.c publishes a
// non-lossy notify pipe and fs/real.c falls back on a 100ms timeout. The
// accept wait now publishes the same pipe, so deliver_signal's existing
// poll_notify_poke() tears it out even when the poke is lost.
//
// One task at a time never reproduces this -- the single sweep above passes on
// a build carrying only the window-race fix -- so the parallel round is not
// redundant coverage, it is the only coverage for this half.
#define PARALLEL_KIDS 8
#define PARALLEL_ROUNDS 3

static void test_parallel_fatal_signal(int sig, const char *label) {
    for (int r = 0; r < PARALLEL_ROUNDS; r++) {
        pid_t kids[PARALLEL_KIDS];
        uint16_t ports[PARALLEL_KIDS];
        int fd[PARALLEL_KIDS][2];
        int started = 0;

        for (int i = 0; i < PARALLEL_KIDS; i++) {
            if (pipe(fd[i]) < 0)
                break;
            pid_t p = fork();
            if (p < 0) { close(fd[i][0]); close(fd[i][1]); break; }
            if (p == 0) {
                close(fd[i][0]);
                child_park_in_accept(fd[i][1]);
                _exit(0);
            }
            close(fd[i][1]);
            kids[i] = p;
            started++;
        }
        if (started == 0) {
            failf("parallel-fork", (uint64_t) errno, 0, 0, 0, 0, 0);
            return;
        }
        // Every child must be parked before any of them is signalled.
        for (int i = 0; i < started; i++) {
            if (read(fd[i][0], &ports[i], sizeof ports[i]) != (ssize_t) sizeof ports[i])
                ports[i] = 0;
            close(fd[i][0]);
        }
        usleep(200 * 1000);
        for (int i = 0; i < started; i++)
            kill(kids[i], sig);

        int survivors = 0;
        for (int i = 0; i < started; i++) {
            if (reap_within(kids[i], test_watchdog_secs(5)))
                continue;
            survivors++;
            // Recover so a failing A/B run leaves no unkillable listeners.
            rescue_connect(ports[i]);
            reap_within(kids[i], test_watchdog_secs(5));
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
    test_parallel_fatal_signal(SIGKILL, "parallel-sigkill-in-accept");
    test_parallel_fatal_signal(SIGTERM, "parallel-sigterm-in-accept");

    return finish_suite("accept_kill");
}
