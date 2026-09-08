// A blocking wait must end even when every wake poke to this task is lost.
//
// A task parked in a host blocking call is pulled out of it by
// pthread_kill(SIGUSR1/SIGUSR2) from kernel/signal.c's signal_wake_task. On
// Darwin that poke is intermittently swallowed in a way that leaves the signal
// blocked and pending in the target thread's own mask with no handler having
// run, and the state is PERMANENT -- every later poke to that thread is equally
// deaf (see signal_thread_unwedge_wake_sigs in util/sync.c).
//
// fs/poll.c used to hand real_poll_wait a NULL timeout whenever the guest named
// no deadline, so a task in that state never came back. On a device after a
// memory-pressure run -- which is fork/exec churn, which is what provokes the
// swallowed poke -- every shell wedged in exactly one place: the
// pselect6(0, NULL, NULL, NULL, NULL, mask) that zsh's `sigsuspend` becomes
// while it waits for a child's SIGCHLD. Its child was already a zombie. The app
// stayed healthy, nothing crashed, no signal was ever delivered, and every
// counter in the kernel read clean; commands simply never returned.
//
// The wait with NO FDS is the sharp case and the one tested first: the notify
// pipe fs/poll.c relies on carries fd readiness, so a wait with no fds has
// nothing that can write to it and the poke is genuinely the only way out.
//
// ISH_TEST_LOSE_WAKE_POKES makes the fault reproducible: with it set to this
// program's comm, signal_wake_task drops every poke to it, exactly as the
// Darwin fault does. Without the env var the test still runs and still has to
// pass -- it is then checking the ordinary wake path, which is worth doing too.
//
// Run it with:
//     ISH_TEST_LOSE_WAKE_POKES=wake_poke_lost ./build/ish -f <root> \
//         /AOK/tests/wake_poke_lost
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <time.h>

#include "test_common.h"

// The cap fs/poll.c applies to a wait the guest gave no deadline for
// (POLL_WAKE_RECHECK_NS). A recovery that takes about this long is the cap
// doing its job; the assertions below are deliberately loose about the exact
// figure and strict about the thing that matters, which is that the wait ends
// at all.
#define CAP_MS 1000
// Long enough that a wait ending at CAP_MS cannot be mistaken for one that
// ended on time, short enough not to pad the suite.
#define CHILD_MS 200
// A wait that has not returned by now is the hang this test exists for. Well
// clear of one cap so a slow machine cannot fail it.
#define GIVE_UP_MS 8000

static volatile sig_atomic_t chld_seen;
static void on_chld(int sig) { (void) sig; chld_seen = 1; }

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

// Fork a child that exits after CHILD_MS. The parent's SIGCHLD is what has to
// break the wait.
static pid_t spawn_ticker(void) {
    pid_t pid = fork();
    if (pid == 0) {
        struct timespec nap = { .tv_sec = 0, .tv_nsec = CHILD_MS * 1000000L };
        nanosleep(&nap, NULL);
        _exit(0);
    }
    return pid;
}

// An alarm is the only way to fail rather than hang if the fix is not there:
// without it a regression means the suite stops, which reads as a broken runner
// instead of a broken kernel.
static void on_alarm(int sig) { (void) sig; }

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "-v") == 0)
        test_verbose = 1;

    struct sigaction chld = { .sa_handler = on_chld };
    sigemptyset(&chld.sa_mask);
    sigaction(SIGCHLD, &chld, NULL);
    struct sigaction alrm = { .sa_handler = on_alarm };
    sigemptyset(&alrm.sa_mask);
    sigaction(SIGALRM, &alrm, NULL);

    // 1. No fds, no timeout: nothing but the poke can end this.
    {
        pid_t kid = spawn_ticker();
        double t0 = now_ms();
        alarm((GIVE_UP_MS + 999) / 1000);
        int r = select(0, NULL, NULL, NULL, NULL);
        int err = errno;
        double dt = now_ms() - t0;
        alarm(0);
        if (r != -1 || err != EINTR) {
            printf("FAIL: select(0, NULL...) returned %d errno=%d (want -1/EINTR)\n", r, err);
            failures_total++;
        } else if (dt >= GIVE_UP_MS) {
            printf("FAIL: select(0, NULL...) hung for %.0fms; a lost wake is permanent\n", dt);
            failures_total++;
        } else if (test_verbose) {
            printf("select(0, NULL...) woke after %.0fms (child at %dms, cap %dms)\n",
                   dt, CHILD_MS, CAP_MS);
        }
        waitpid(kid, NULL, 0);
    }

    // 2. The same wait with fds attached. The notify pipe can rescue this one,
    //    so it is the control: if 1 fails and 2 passes, the fault is confined to
    //    the no-fd shape.
    {
        int p[2];
        if (pipe(p) < 0) {
            printf("FAIL: pipe: %s\n", strerror(errno));
            return finish_suite("wake_poke_lost");
        }
        pid_t kid = spawn_ticker();
        struct pollfd pfd = { .fd = p[0], .events = POLLIN };
        double t0 = now_ms();
        alarm((GIVE_UP_MS + 999) / 1000);
        int r = poll(&pfd, 1, -1);
        int err = errno;
        double dt = now_ms() - t0;
        alarm(0);
        if (r != -1 || err != EINTR) {
            printf("FAIL: poll(fds, -1) returned %d errno=%d (want -1/EINTR from SIGCHLD)\n", r, err);
            failures_total++;
        } else if (dt >= GIVE_UP_MS) {
            printf("FAIL: poll(fds, -1) hung for %.0fms\n", dt);
            failures_total++;
        } else if (test_verbose) {
            printf("poll(fds, -1) woke after %.0fms\n", dt);
        }
        waitpid(kid, NULL, 0);
        close(p[0]);
        close(p[1]);
    }

    // 3. A deadline the guest DID name must still be honoured to its own
    //    length. The cap is shorter than this, so a cap that leaked out as a
    //    guest-visible timeout would end it early -- which is the regression a
    //    naive bound introduces, and it would break every `select` with a long
    //    timeout in the guest.
    {
        chld_seen = 0;
        struct timeval tv = { .tv_sec = 2, .tv_usec = 500000 };
        double t0 = now_ms();
        int r = select(0, NULL, NULL, NULL, &tv);
        double dt = now_ms() - t0;
        if (r != 0) {
            printf("FAIL: select with a 2500ms timeout returned %d errno=%d (want 0)\n", r, errno);
            failures_total++;
        } else if (dt < 2300) {
            printf("FAIL: select with a 2500ms timeout returned after %.0fms; "
                   "the internal cap is leaking out as a timeout\n", dt);
            failures_total++;
        } else if (test_verbose) {
            printf("select(2500ms) timed out after %.0fms\n", dt);
        }
    }

    return finish_suite("wake_poke_lost");
}
