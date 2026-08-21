// kill() is PROCESS-directed: it must be deliverable to any thread in the
// group that is not blocking the signal -- including one parked in sigwait()
// for exactly that signal. AOK sent it to the addressed task's PRIVATE queue
// instead, so the standard daemon pattern below could not be signalled at all.
//
// The pattern: block the signals in every thread, then have one dedicated
// thread sigwait() for them. MariaDB, PostgreSQL and most sysv daemons do
// this. Found because mariadbd, having decided to abort, could not make its
// own signal thread exit -- it sat forever with SIGTERM pending on the main
// thread and nothing pending on the signal thread, which wedged the whole
// Devuan boot: the init script polled it forever, rc never exited, and
// sysvinit never reached the getty lines, so the console never got a shell.
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

#include "test_common.h"

static void ck(const char *what, int ok, const char *detail) {
    if (!ok)
        failf(what, (uint64_t) errno, 0, 0, 0, 0, 0);
    test_logf("  %-52s %s%s%s\n", what, ok ? "ok" : "FAIL",
              ok || detail == NULL ? "" : "   ", ok || detail == NULL ? "" : detail);
}

static sigset_t waitset;
static volatile int got_sig;
static volatile int waiter_ready;

static void *waiter(void *arg) {
    (void) arg;
    int sig = 0;
    waiter_ready = 1;
    if (sigwait(&waitset, &sig) == 0)
        got_sig = sig;
    return NULL;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    // The watchdog IS the assertion for the bug: before the fix the sigwait
    // thread never returns and this is the only thing that ends the run.
    alarm(test_watchdog_secs(30));

    sigemptyset(&waitset);
    sigaddset(&waitset, SIGTERM);
    sigaddset(&waitset, SIGHUP);
    // Blocked in the MAIN thread before the waiter is created, so the waiter
    // inherits the mask -- which is what makes every thread block it, the
    // condition under which the signal has nowhere to go but the shared queue.
    ck("block the wait set process-wide",
       pthread_sigmask(SIG_BLOCK, &waitset, NULL) == 0, strerror(errno));

    pthread_t th;
    ck("spawn the sigwait thread", pthread_create(&th, NULL, waiter, NULL) == 0,
       strerror(errno));
    while (!waiter_ready)
        usleep(1000);
    usleep(100 * 1000);   // let it reach sigwait, not merely be runnable

    // The whole point: process-directed, not pthread_kill at the waiter.
    ck("kill(getpid(), SIGTERM)", kill(getpid(), SIGTERM) == 0, strerror(errno));

    ck("the sigwait thread received it", pthread_join(th, NULL) == 0, strerror(errno));
    ck("...and it was SIGTERM", got_sig == SIGTERM, NULL);

    // kill(pid, 0) carries no signal: it is the "does this process exist"
    // probe, and every shell and package manager leans on it. It is in this
    // file because the first version of the fix above forgot it and indexed a
    // signal mask with 0, which is out of range -- apt died on the spot. A
    // signal-routing change must be tested against the call that routes no
    // signal at all.
    ck("kill(getpid(), 0) succeeds", kill(getpid(), 0) == 0, strerror(errno));
    ck("kill(nonexistent, 0) is ESRCH",
       kill(0x7ffffff, 0) == -1 && errno == ESRCH, strerror(errno));

    return finish_suite("sigwait_kill");
}
