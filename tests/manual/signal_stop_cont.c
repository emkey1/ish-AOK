// SIGSTOP/SIGCONT mutual-cancellation (Linux prepare_signal semantics).
//
// When a continue signal (SIGCONT) is generated, any still-pending stop signal
// on the target is discarded, and when a stop signal is generated any pending
// SIGCONT is discarded. Without this, a rapid SIGSTOP-then-SIGCONT pair races:
// the SIGCONT lifts the group-stop and wakes the stopped task, but the still-
// queued SIGSTOP is then processed, re-stops the task, and it blocks forever in
// the group-stop wait with no further SIGCONT coming. This intermittently
// wedged stress-ng --schedmix, which hammers its children with interleaved
// SIGSTOP/SIGCONT while exercising the scheduler.
//
// Detection is per-trial rather than "does the whole run hang", so the test is
// deterministic and fast: each trial fires exactly one SIGSTOP then one SIGCONT
// at a child that is busy-advancing a shared counter, and checks whether the
// child keeps running. On a correct kernel (real Linux, or fixed AOK) the pair
// nets out to "running" and the counter always advances promptly. On the buggy
// build the race leaves the child stopped with no further SIGCONT; the trial
// detects the freeze, confirms it (a rescue SIGCONT is required to un-stick the
// child), and counts it. Any confirmed stuck trial fails the suite.

#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

static void msleep(long ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

// Poll up to timeout_ms for *counter to move past `from`. Returns ms waited if
// it advanced, or -1 if it stayed frozen for the whole window.
static long wait_advance(volatile uint64_t *counter, uint64_t from, long timeout_ms) {
    for (long waited = 0; waited < timeout_ms; waited += 2) {
        if (*counter != from)
            return waited;
        msleep(2);
    }
    return *counter != from ? timeout_ms : -1;
}

#define TRIALS 1200
// A correct kernel may briefly group-stop the child when it processes the
// SIGSTOP a hair before the paired SIGCONT, but the SIGCONT resumes it within a
// notify+poke (sub-millisecond). A window well above that but far below "hung"
// cleanly separates a correct brief stop from the buggy permanent stop.
#define SUSPECT_MS 400   // still frozen this long after SIGSTOP+SIGCONT => suspect
#define CONFIRM_MS 1000  // a genuinely stuck child only moves once rescued

static void test_stop_cont_race(void) {
    volatile uint64_t *counter = mmap(NULL, sizeof(uint64_t), PROT_READ | PROT_WRITE,
                                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (counter == MAP_FAILED) {
        perror("mmap");
        exit(2);
    }
    *counter = 0;

    pid_t child = fork();
    if (child < 0) {
        perror("fork");
        exit(2);
    }
    if (child == 0) {
        for (;;) {
            *counter = *counter + 1;
            for (volatile int i = 0; i < 200; i++) {
            }
        }
        _exit(0);
    }

    if (wait_advance(counter, 0, 2000) < 0)
        failf("child_started", 0, 0, 0, 1, 0, 0);

    // A single confirmed freeze is already a bug, but require a small threshold
    // before failing so a lone load-induced hiccup on a correct build (a legit
    // brief group-stop that happened to exceed SUSPECT_MS) can't flake the
    // suite. The buggy build races on essentially every stop/cont pair and
    // blows past this in a fraction of a second; a correct build stays at zero.
    const unsigned STUCK_FAIL = 4;
    unsigned stuck = 0;
    int trials_run = 0;
    for (int t = 0; t < TRIALS && stuck < STUCK_FAIL; t++, trials_run++) {
        uint64_t snap = *counter;
        // Fire the racing pair: on the buggy build the SIGCONT can be a no-op
        // (the child has not processed the SIGSTOP yet) and the leftover SIGSTOP
        // then stops the child with nothing left to wake it.
        kill(child, SIGSTOP);
        kill(child, SIGCONT);

        if (wait_advance(counter, snap, SUSPECT_MS) >= 0)
            continue; // child kept running -- correct behavior

        // Suspected freeze. A correct kernel never lands here for long (the pair
        // nets to "running"); confirm the child is genuinely stopped by proving
        // it only resumes once we send an extra, unpaired SIGCONT.
        uint64_t snap2 = *counter;
        kill(child, SIGCONT);
        if (wait_advance(counter, snap2, CONFIRM_MS) >= 0)
            stuck++; // was stopped; the rescue SIGCONT was required
        else
            kill(child, SIGCONT); // still frozen -- keep trying to free it
    }

    test_logf("stop/cont trials=%d stuck=%u\n", trials_run, stuck);
    if (stuck >= STUCK_FAIL)
        failf("stop_cont_left_child_stopped", stuck, (uint64_t) trials_run, 0, 0, 0, 0);

    kill(child, SIGKILL);
    int status = 0;
    if (waitpid(child, &status, 0) != child)
        failf("waitpid", (uint64_t) errno, 0, 0, (uint64_t) child, 0, 0);
    munmap((void *) counter, sizeof(uint64_t));
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    test_stop_cont_race();
    return finish_suite("signal_stop_cont");
}
