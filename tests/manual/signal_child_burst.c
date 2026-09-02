// Burst of near-simultaneous child exits must never strand a shell's
// sigsuspend()/wait4() reaper loop -- the standard "wait" builtin / job
// control pattern used by every POSIX shell.
//
// SIGCHLD is a standard (non-realtime) signal: Linux never queues a second
// instance while one is already pending, and AOK's deliver_signal_unlocked_locked
// / deliver_signal_to_group_locked correctly mirror that for the *queue*. But
// they used to treat "already pending" as a reason to skip the *wake* too,
// via a single early return covering both. sigset_has(pending, SIGCHLD) here
// and the clearing of that bit in receive_signals() (when the parent actually
// drains its queue) are not atomic with each other: if a second child exits
// and calls send_signal_to_group(SIGCHLD) while the first occurrence's
// pending bit is still set -- because the parent hasn't gotten around to
// receive_signals() yet, even though it was already woken once and may have
// already gone back to sigsuspend() to wait for the *next* child -- that
// second, distinct occurrence must still wake the parent. Dropping the wake
// alongside the (correct) skipped requeue can leave the parent parked in
// sigsuspend() forever with a live, waitable zombie it will never be told
// about again: a permanent hang, not a missed optimization.
//
// This reproduces the exact failure mode reported from a device: labwc
// (opt/AOK/tools/start-wayland.sh) spawning several app processes in a tight
// burst wedged large parts of the guest -- a fresh SSH session afterward hung
// on nothing but `echo`. A live sample of the hung process during
// investigation showed its one remaining thread stuck in
// sys_rt_sigsuspend_guest -> wait_for -> pthread_cond_wait, forever: exactly
// the shell's own reaper loop, not filesystem contention (a from-scratch
// reproduction with zero filesystem I/O -- pure fork()/exit() churn --
// reproduced the same hang on the unfixed kernel).
//
// The race is timing-sensitive enough that a hand-rolled reaper loop within
// this test process (plain fork() + nanosleep(), no exec, no subshells) does
// not reliably hit it -- it needs the extra process layering and scheduling
// pressure of a real shell fork-bombing itself with subshells. So rather than
// reinvent that exact timing, this test drives a real /bin/sh doing the
// actual thing that wedged the device: spawn a burst of backgrounded
// subshells that all sleep the same short duration then exit, and `wait` for
// them. Detection is per-trial via a watchdog alarm around a blocking
// waitpid() for that shell subprocess, so the suite itself can't hang even
// when the bug reproduces; a trial that times out is confirmed by killing
// the whole process group and counted as stuck.

#define _GNU_SOURCE
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_common.h"

static sigjmp_buf watchdog_jmp;

static void sigalrm_handler(int sig) {
    (void) sig;
    siglongjmp(watchdog_jmp, 1);
}

#define SUBSHELLS 24
#define TRIALS 25
#define WATCHDOG_SECONDS 10

static const char *burst_script =
    "N=24\n"
    "i=0\n"
    "while [ $i -lt $N ]; do\n"
    "  ( sleep 1; exit 0 ) &\n"
    "  i=$((i+1))\n"
    "done\n"
    "wait\n";

static void run_trial(int trial, unsigned *stuck) {
    pid_t child = fork();
    if (child < 0) {
        perror("fork");
        exit(2);
    }
    if (child == 0) {
        if (setpgid(0, 0) != 0) {
            perror("setpgid");
            _exit(2);
        }
        execl("/bin/sh", "sh", "-c", burst_script, (char *) NULL);
        perror("execl");
        _exit(2);
    }

    // Make sure the child's own setpgid() has landed before we might need to
    // signal the whole group.
    if (setpgid(child, child) != 0 && errno != EACCES)
        test_logf("trial %d: setpgid(parent side) errno=%d\n", trial, errno);

    int stuck_this_trial = 0;
    int status = 0;
    if (sigsetjmp(watchdog_jmp, 1) == 0) {
        alarm(WATCHDOG_SECONDS);
        pid_t r = waitpid(child, &status, 0);
        alarm(0);
        if (r != child) {
            failf("waitpid", (uint64_t) errno, 0, 0, (uint64_t) child, 0, 0);
            stuck_this_trial = 1;
        }
    } else {
        stuck_this_trial = 1;
    }

    if (stuck_this_trial) {
        (*stuck)++;
        // Rescue: kill the whole process group (the shell plus any
        // subshells/sleep processes still alive) and reap it so the suite
        // doesn't itself wedge on the failure it's reporting.
        kill(-child, SIGKILL);
        kill(child, SIGKILL);
        waitpid(child, &status, 0);
        test_logf("trial %d: STUCK (rescued)\n", trial);
    } else {
        test_logf("trial %d: ok\n", trial);
    }
}

static void test_child_burst(void) {
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = sigalrm_handler;
    sigemptyset(&act.sa_mask);
    if (sigaction(SIGALRM, &act, NULL) != 0) {
        perror("sigaction(SIGALRM)");
        exit(2);
    }

    // A single confirmed stuck trial is already a real bug, but require a
    // small threshold before failing so one load-induced hiccup can't flake
    // the suite. The buggy build hits this race easily under a synchronized
    // burst (observed ~50% of trials); a correct build stays at zero.
    const unsigned STUCK_FAIL = 2;
    unsigned stuck = 0;
    int trials_run = 0;
    for (int t = 0; t < TRIALS && stuck < STUCK_FAIL; t++, trials_run++)
        run_trial(t, &stuck);

    test_logf("child burst trials=%d stuck=%u\n", trials_run, stuck);
    if (stuck >= STUCK_FAIL)
        failf("child_burst_left_shell_stuck_in_sigsuspend", stuck, (uint64_t) trials_run, 0, 0, 0, 0);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    test_child_burst();
    return finish_suite("signal_child_burst");
}
