// vfork_fatal_signal.c — the vfork(2) parent wait: what wakes it, what does
// not, and the lifetime of the handoff struct it waits on.
//
// sys_clone_common parks the parent on a `struct vfork_info` until the child
// execs or _exits. Three separate defects lived in that wait:
//
// 1. It was uninterruptible ("FIXME this should stop waiting if a fatal signal
//    is received"). A parent SIGKILLed while waiting stayed parked until the
//    child finished, which for a child that never finishes is forever.
// 2. The first attempt at (1) broke the wait on ANY pending signal. That is
//    much worse than the bug: until the child execs or _exits it is running on
//    the PARENT's stack, so a parent that resumes guest code on a signal it
//    could have returned from executes over frames the child is still using.
//    Only a signal that will terminate the parent may end the wait.
// 3. `struct vfork_info` was stack-local to sys_clone_common. Once (1) is
//    fixed, the parent can return while the child is still running and still
//    due to touch that struct at its next exec or exit -- a use-after-free of
//    the parent's dead frame. It is now heap-allocated and refcounted, one
//    reference per side, and vfork_notify() claims the pointer with an atomic
//    exchange so it releases the child's reference exactly once even though a
//    child that execs and then exits calls it TWICE (exec.c, then exit.c).
//
// The behavior asserted for a fatal parent signal is Linux's: the wait ends,
// and the CHILD IS LEFT ALONE. It runs on to its exec or _exit with the shared
// mm kept alive by its own reference. (An earlier draft SIGKILLed the child;
// besides diverging from Linux it races -- the child may have already
// completed its execve and be on its way to setting done, in which case it no
// longer shares the parent's stack and is a legitimately running new program.)
//
// A leaked vfork_info is not observable from the guest, so the leak fixes are
// exercised rather than measured: every path here runs repeatedly, and the
// double-free / UAF that the same code would produce if the refcount were
// wrong does crash.
//
// A/B: the unfixed build fails 6 checks (all three fatal trials time out
// waiting for the victim to become reapable), the fixed build passes on i386
// and aarch64 guests. Everything asserted here is Linux behavior, but the
// real-Linux oracle run is still outstanding -- the host was unreachable.
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include "test_common.h"

// The vfork child naps this long before finishing. The fatal-signal trial
// requires the parent to be reaped in a fraction of it: an unkillable wait
// cannot be reaped until the nap ends, a killable one dies in milliseconds.
#define CHILD_NAP_MS 3000
#define REAP_DEADLINE_MS 1000

static int is_true(const char *label, int cond) {
    if (cond) { test_logf("ok   %s\n", label); return 1; }
    printf("FAIL %s\n", label);
    failures_total++;
    return 0;
}

static void nap_ms(long ms) {
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000 };
    nanosleep(&ts, NULL);
}

// Read one byte with a deadline. Returns 0 on timeout or EOF, so a peer that
// died instead of reporting fails the assertion rather than hanging the suite.
static int read_byte(int fd, char *out, int ms) {
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int r = poll(&pfd, 1, ms);
    if (r <= 0)
        return 0;
    return read(fd, out, 1) == 1;
}

// waitpid with a deadline. Returns milliseconds waited, or -1 if the deadline
// passed without the child being reapable.
static int reap_within(pid_t pid, int *status, int ms) {
    for (int waited = 0; waited <= ms; waited += 10) {
        if (waitpid(pid, status, WNOHANG) == pid)
            return waited;
        nap_ms(10);
    }
    return -1;
}

// A fatal signal must end the wait. The victim vforks a child that naps well
// past the point where we SIGKILL the victim, so the only way the victim is
// reapable promptly is if the wait gave up on the signal.
//
// Also asserts the two things that make that safe: the child outlives the
// parent's death (it reports again after its nap, from a shared mm whose last
// user is now itself), and the victim really did die from our signal.
static void trial_fatal_wakes_wait(int trial) {
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        is_true("fatal.pipe", 0);
        return;
    }

    pid_t victim = fork();
    if (victim == 0) {
        close(pipefd[0]);
        pid_t child = vfork();
        if (child == 0) {
            // Announce that the parent is now parked in the vfork wait, nap
            // past the kill, then announce that we outlived it.
            char c = 'A';
            (void) !write(pipefd[1], &c, 1);
            nap_ms(CHILD_NAP_MS);
            c = 'B';
            (void) !write(pipefd[1], &c, 1);
            _exit(0);
        }
        // Only reached if the wait was NOT killable: we sit here for the whole
        // nap and the SIGKILL lands after it, far too late for the deadline.
        _exit(0);
    }
    close(pipefd[1]);

    char c = 0;
    if (!is_true("fatal.child_entered_wait", read_byte(pipefd[0], &c, 5000) && c == 'A')) {
        kill(victim, SIGKILL);
        waitpid(victim, NULL, 0);
        close(pipefd[0]);
        return;
    }

    kill(victim, SIGKILL);
    int status = 0;
    int waited = reap_within(victim, &status, REAP_DEADLINE_MS);
    test_log_if(waited < 0, "trial %d: victim reaped after %dms (deadline %dms, child nap %dms)\n",
                trial, waited, REAP_DEADLINE_MS, CHILD_NAP_MS);
    is_true("fatal.wait_ended_promptly", waited >= 0);
    is_true("fatal.victim_killed_by_signal",
            WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);

    // Linux leaves the vfork child running when the parent takes a fatal
    // signal. If iSH tore the shared mm down with the parent, or killed the
    // child outright, this second report never arrives.
    c = 0;
    is_true("fatal.child_outlived_parent",
            read_byte(pipefd[0], &c, CHILD_NAP_MS + 5000) && c == 'B');
    close(pipefd[0]);
}

// A signal the parent could return from must NOT end the wait, because the
// child is still on the parent's stack. Observed through ordering: the child
// reports 'A' before its nap and 'C' after it, and the parent reports 'P' only
// once vfork() has returned. Correct order is A, C, P; a wait that broke on
// the handled signal yields A, P, C.
static void trial_nonfatal_does_not_wake_wait(void) {
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        is_true("nonfatal.pipe", 0);
        return;
    }

    pid_t victim = fork();
    if (victim == 0) {
        close(pipefd[0]);
        // A handled signal: signal_action() is CALL_HANDLER, not KILL.
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = SIG_IGN;
        sigaction(SIGUSR1, &sa, NULL);

        pid_t child = vfork();
        if (child == 0) {
            char c = 'A';
            (void) !write(pipefd[1], &c, 1);
            nap_ms(600);
            c = 'C';
            (void) !write(pipefd[1], &c, 1);
            _exit(0);
        }
        char c = 'P';
        (void) !write(pipefd[1], &c, 1);
        _exit(0);
    }
    close(pipefd[1]);

    char first = 0, second = 0, third = 0;
    int got_first = read_byte(pipefd[0], &first, 5000);
    // Signal repeatedly across the child's nap, so the delivery lands squarely
    // inside the parent's wait rather than depending on one lucky moment.
    for (int i = 0; i < 10; i++) {
        kill(victim, SIGUSR1);
        nap_ms(40);
    }
    int got_second = read_byte(pipefd[0], &second, 5000);
    int got_third = read_byte(pipefd[0], &third, 5000);

    is_true("nonfatal.child_entered_wait", got_first && first == 'A');
    test_log_if(!(got_second && second == 'C'), "nonfatal order: %c%c%c (want ACP)\n",
                first ? first : '?', second ? second : '?', third ? third : '?');
    is_true("nonfatal.parent_stayed_parked", got_second && second == 'C');
    is_true("nonfatal.parent_resumed_after", got_third && third == 'P');

    int status = 0;
    is_true("nonfatal.victim_exited_cleanly",
            reap_within(victim, &status, 5000) >= 0 &&
            WIFEXITED(status) && WEXITSTATUS(status) == 0);
    close(pipefd[0]);
}

// A vfork child that execs and then exits calls vfork_notify() twice, from
// exec.c and then from exit.c. The second call must not release the child's
// reference a second time. Repeat it: an unguarded double release frees the
// struct while the parent may still hold a reference, and the crash that
// follows is timing-dependent.
static void trial_exec_then_exit(const char *self) {
    for (int i = 0; i < 32; i++) {
        pid_t child = vfork();
        if (child == 0) {
            execl(self, self, "--exit-7", (char *) NULL);
            _exit(127);
        }
        if (!is_true("exec_then_exit.vfork", child > 0))
            return;
        int status = 0;
        if (!is_true("exec_then_exit.reaped", reap_within(child, &status, 10000) >= 0))
            return;
        if (!is_true("exec_then_exit.status",
                     WIFEXITED(status) && WEXITSTATUS(status) == 7))
            return;
    }
}

int main(int argc, char **argv) {
    // Re-exec target for trial_exec_then_exit. Checked before test_init, which
    // rejects unknown options.
    if (argc == 2 && strcmp(argv[1], "--exit-7") == 0)
        return 7;

    test_init(argc, argv);
    alarm(test_watchdog_secs(180));

    // Baseline: the ordinary vfork + _exit round trip still works.
    pid_t child = vfork();
    if (child == 0)
        _exit(42);
    is_true("basic.vfork", child > 0);
    int status = 0;
    is_true("basic.reaped", reap_within(child, &status, 10000) >= 0);
    is_true("basic.status", WIFEXITED(status) && WEXITSTATUS(status) == 42);

    trial_exec_then_exit(argv[0]);
    trial_nonfatal_does_not_wake_wait();
    for (int i = 0; i < 3; i++)
        trial_fatal_wakes_wait(i);

    return finish_suite("vfork_fatal_signal");
}
