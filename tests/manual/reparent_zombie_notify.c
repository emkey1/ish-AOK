// A zombie handed to a new parent has to be announced to that new parent.
//
// When a process exits, its children are reparented -- to a subreaper if there
// is one, otherwise to init. A child that is still RUNNING will announce itself
// later, when it exits. A child that is ALREADY a zombie has nothing left to
// announce it: its exit was reported to a parent that is now gone. Linux
// handles that in reparent_leader(), which calls do_notify_parent() for exactly
// those children. AOK's kernel/exit.c moved them and told nobody.
//
// The failure needs a reaper that sleeps rather than polls, which is what real
// init does: asleep in select/pselect, reaping from a SIGCHLD handler. It never
// learned it had acquired a zombie, so the zombie stayed on the process table
// for the life of the system. Reported from a device carrying 16 of them, every
// one a `sudo` with ppid 1; a manual `kill -CHLD 1` reaped all 16 at once,
// which is what identified the missing notification.
//
// The shape below puts the orphaning DEEPER in the tree than the reaper's own
// children, which is what a shell that forks-by-relaunch produces and what the
// device hit. It matters for the test too: if the reaper were the exiting
// process's parent it would get a SIGCHLD for that exit anyway, and the missing
// one would be masked by it.
//
// R makes itself a child subreaper so that it is the one that inherits B. That
// is not a detail of the test rig: without it the orphan goes all the way to
// pid 1, which is where the device saw this, but a test that depended on being
// pid 1 could only run as init. find_new_parent() walks to the first subreaper
// and otherwise to init, and both arrive at the same code.
//
//   R (this process)   -- the reaper: asleep in select, SIGCHLD handler only
//   `-- A              -- alive across the whole window, so R hears nothing
//       `-- C          -- exits, orphaning B
//           `-- B      -- exits FIRST, so it is a zombie by the time C goes
//
// Checked separately: that the zombie really was reparented (waitpid finds it),
// and that R was TOLD. Only the second half was broken -- which is why the
// symptom was a process table that filled up rather than anything failing.
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_common.h"

static volatile sig_atomic_t chld_count = 0;
static void on_chld(int sig) { (void) sig; chld_count++; }

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-58s got=%-10ld want=%ld\n", label, got, want);
}

int main(int argc, char **argv) {
    (void) argc; (void) argv;

    // A reaper that only wakes on the signal. No SA_RESTART: the select below
    // has to come back EINTR, the way init's does.
    struct sigaction sa, old;
    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sa.sa_handler = on_chld;
    if (sigaction(SIGCHLD, &sa, &old) != 0) {
        test_logf("  cannot install a SIGCHLD handler, skipped\n");
        return finish_suite("reparent_zombie_notify");
    }

    if (prctl(PR_SET_CHILD_SUBREAPER, 1L, 0L, 0L, 0L) != 0) {
        test_logf("  no PR_SET_CHILD_SUBREAPER (%s), skipped\n", strerror(errno));
        sigaction(SIGCHLD, &old, NULL);
        return finish_suite("reparent_zombie_notify");
    }

    pid_t a = fork();
    if (a < 0) {
        test_logf("  fork failed (%s), skipped\n", strerror(errno));
        sigaction(SIGCHLD, &old, NULL);
        return finish_suite("reparent_zombie_notify");
    }
    if (a == 0) {
        // A must NOT be a subreaper itself, or it collects B and R -- the one
        // being tested -- never hears about it. Worth stating because it is
        // not obvious: AOK's fork copies the whole tgroup struct, so the flag
        // R just set is inherited here, where Linux would not have passed it
        // on. Clearing it makes the test say what it means either way.
        prctl(PR_SET_CHILD_SUBREAPER, 0L, 0L, 0L, 0L);
        pid_t c = fork();
        if (c < 0)
            _exit(3);
        if (c == 0) {
            pid_t b = fork();
            if (b < 0)
                _exit(4);
            if (b == 0)
                _exit(7);              // B: a zombie immediately
            usleep(300000);            // ...while C is still alive to hold it
            _exit(0);                  // C exits: B is orphaned onto the reaper
        }
        waitpid(c, NULL, 0);           // A reaps C, so R never hears about C
        usleep(1500000);               // A outlives the window below
        _exit(0);
    }

    // One second, spanning the moment B is orphaned (~300ms in). A is alive
    // throughout, so any SIGCHLD arriving here can only be about B.
    struct timeval tv = { 1, 0 };
    int r = select(0, NULL, NULL, NULL, &tv);
    int woken = (r < 0 && errno == EINTR);
    int signals = chld_count;

    int status = 0;
    pid_t reaped = waitpid(-1, &status, WNOHANG);
    int reaped_status = (reaped > 0 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;

    // The half that always worked: the zombie IS ours now.
    ck("an orphaned zombie is reparented onto the reaper", reaped > 0, 1);
    ck("and it still carries its exit status", reaped_status, 7);
    // The half that did not: the reaper has to be told, or it sleeps through it.
    ck("the reaper is woken when it inherits a zombie", woken, 1);
    ck("by a SIGCHLD it can act on", signals > 0, 1);
    test_logf("  %-58s %d\n", "(SIGCHLDs seen in the window)", signals);

    kill(a, SIGKILL);
    waitpid(a, NULL, 0);
    prctl(PR_SET_CHILD_SUBREAPER, 0L, 0L, 0L, 0L);
    sigaction(SIGCHLD, &old, NULL);
    return finish_suite("reparent_zombie_notify");
}
