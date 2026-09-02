// ptrace_group_stop: a SEIZE'd tracee that enters a job-control group-stop must
// report it to the tracer (as PTRACE_EVENT_STOP) and be resumable to completion.
// This is the regression for iSH-AOK's ptrace_group_stop() path -- before it,
// strace -f following a fork/clone child hung because a traced task's group-stop
// was invisible to the tracer's wait4 and PTRACE_CONT had nothing to resume.
//
// The flow also matches real Linux semantics, so this doubles as a conformance
// test (note: it requires a working ptrace, so it cannot run under qemu-user).
//
//   child : raise(SIGSTOP) -> then _exit(42)
//   tracer: SEIZE child; on the SIGSTOP signal-delivery-stop, deliver SIGSTOP to
//           force group-stop; expect a PTRACE_EVENT_STOP report; SIGCONT +
//           PTRACE_CONT to let it finish; verify exit code 42.
//
// The second case runs the OTHER order: the tracee is already group-stopped
// when the tracer seizes it. Linux reports the group-stop on attach
// (ptrace_attach() wakes a __TASK_STOPPED tracee so it can re-enter the trap);
// iSH-AOK used to leave that tracee parked in handle_interrupt's job-control
// wait, where nothing would wake it and nothing would notice it had become
// traced, so the tracer's wait4 hung forever.
//
// Both orders are reachable from the first case alone -- there is no
// synchronisation between the child's raise(SIGSTOP) and the parent's
// ptrace() -- but the parent essentially always won, so the bug sat unseen.
// It became a reliable failure the moment the arm64 DC ZVA gadget landed
// (2026-08-23) and shifted musl's memset timing. That is why the order is
// FORCED here with waitpid(WUNTRACED) rather than left to a race: a test that
// only fails when the scheduler cooperates is a test that reports "fixed"
// while the bug is still there.

#define _GNU_SOURCE

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_common.h"

#ifndef PTRACE_SEIZE
#define PTRACE_SEIZE 0x4206
#endif
#ifndef PTRACE_EVENT_STOP
#define PTRACE_EVENT_STOP 128
#endif

static pid_t g_child;

static void drive_tracee(pid_t child, int want_exit, const char *label);

static void on_alarm(int sig) {
    (void) sig;
    if (g_child > 0)
        kill(g_child, SIGKILL);
    static const char msg[] = "ptrace_group_stop: FAIL timeout (no group-stop report / never resumed)\n";
    write(2, msg, sizeof(msg) - 1);
    _exit(1);
}

static void test_group_stop_reported(void) {
    g_child = fork();
    if (g_child < 0) {
        perror("fork");
        failf("ptrace_group_stop fork", 0, 0, 0, 0, 0, 0);
        return;
    }
    if (g_child == 0) {
        // Give the parent a beat to PTRACE_SEIZE us before we stop.
        raise(SIGSTOP);
        _exit(42);
    }

    pid_t child = g_child;
    if (ptrace(PTRACE_SEIZE, child, 0, 0) != 0) {
        perror("PTRACE_SEIZE");
        kill(child, SIGKILL);
        failf("ptrace_group_stop seize", (uint64_t) errno, 0, 0, 0, 0, 0);
        return;
    }
    test_logf("seized %d\n", (int) child);

    drive_tracee(child, 42, "ptrace_group_stop");
}

// Drive a seized tracee from wherever it is now through to exit, expecting
// exactly one PTRACE_EVENT_STOP group-stop report along the way. Shared by
// both attach orders.
static void drive_tracee(pid_t child, int want_exit, const char *label) {
    int saw_group_stop = 0;
    for (int iterations = 0; iterations < 100; iterations++) {
        int status;
        pid_t w = waitpid(child, &status, 0);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            perror("waitpid");
            failf(label, (uint64_t) errno, 0, 0, 0, 0, 0);
            return;
        }

        if (WIFEXITED(status)) {
            int code = WEXITSTATUS(status);
            test_logf("child exited %d (saw_group_stop=%d)\n", code, saw_group_stop);
            if (!saw_group_stop)
                failf(label, 0, 0, 0, 1, 0, 0);
            if (code != want_exit)
                failf(label, (uint64_t) code, 0, 0, (uint64_t) want_exit, 0, 0);
            return;
        }
        if (WIFSIGNALED(status)) {
            failf(label, (uint64_t) WTERMSIG(status), 0, 0, 0, 0, 0);
            return;
        }
        if (!WIFSTOPPED(status)) {
            failf(label, (uint64_t) status, 0, 0, 0, 0, 0);
            return;
        }

        int sig = WSTOPSIG(status);
        int event = (status >> 16) & 0xff;
        test_logf("stop: sig=%d event=%d status=%#x\n", sig, event, status);

        if (event == PTRACE_EVENT_STOP) {
            // The group-stop report. Lift job control (SIGCONT for real-Linux
            // listener semantics; harmless on AOK) and continue.
            saw_group_stop = 1;
            kill(child, SIGCONT);
            if (ptrace(PTRACE_CONT, child, 0, 0) != 0) {
                perror("PTRACE_CONT after event-stop");
                failf(label, (uint64_t) errno, 0, 0, 0, 0, 0);
                return;
            }
        } else if (sig == SIGSTOP) {
            // Signal-delivery-stop of SIGSTOP: deliver it so the child actually
            // group-stops (and thus reports PTRACE_EVENT_STOP next).
            if (ptrace(PTRACE_CONT, child, 0, SIGSTOP) != 0) {
                perror("PTRACE_CONT deliver SIGSTOP");
                failf(label, (uint64_t) errno, 0, 0, 0, 0, 0);
                return;
            }
        } else {
            // Any other stop (SIGCONT delivery etc.): pass over it.
            if (ptrace(PTRACE_CONT, child, 0, 0) != 0) {
                perror("PTRACE_CONT pass");
                failf(label, (uint64_t) errno, 0, 0, 0, 0, 0);
                return;
            }
        }
    }

    kill(child, SIGKILL);
    failf(label, 0, 0, 0, 0, 0, 0);
}

// The attach-after-stop order, forced rather than raced: wait for the child to
// actually be group-stopped, THEN seize it. Before the fix the tracee sat in
// handle_interrupt's job-control wait with nothing to wake it, so no
// PTRACE_EVENT_STOP ever reached us and this hit the watchdog.
static void test_seize_after_group_stop(void) {
    g_child = fork();
    if (g_child < 0) {
        perror("fork");
        failf("ptrace_seize_stopped fork", 0, 0, 0, 0, 0, 0);
        return;
    }
    if (g_child == 0) {
        raise(SIGSTOP);
        _exit(43);
    }
    pid_t child = g_child;

    // WUNTRACED reports the job-control stop of an untraced child, so this
    // returns only once the child really is stopped -- that is what makes the
    // ordering deterministic instead of a coin flip.
    int status = 0;
    pid_t w;
    do {
        w = waitpid(child, &status, WUNTRACED);
    } while (w < 0 && errno == EINTR);
    if (w != child || !WIFSTOPPED(status) || WSTOPSIG(status) != SIGSTOP) {
        kill(child, SIGKILL);
        (void) waitpid(child, NULL, 0);
        failf("ptrace_seize_stopped prestop", (uint64_t) w, (uint64_t) status,
              (uint64_t) errno, (uint64_t) child, 0, 0);
        return;
    }
    test_logf("child %d is group-stopped; seizing now\n", (int) child);

    if (ptrace(PTRACE_SEIZE, child, 0, 0) != 0) {
        perror("PTRACE_SEIZE (already stopped)");
        kill(child, SIGKILL);
        (void) waitpid(child, NULL, 0);
        failf("ptrace_seize_stopped seize", (uint64_t) errno, 0, 0, 0, 0, 0);
        return;
    }

    drive_tracee(child, 43, "ptrace_seize_stopped");
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    signal(SIGALRM, on_alarm);
    // Generous, env-scalable watchdog. The group-stop handshake completes in
    // tens of milliseconds even under heavy host oversubscription; the old
    // fixed alarm(15) still false-FAILED under the release procedure's 4-way
    // concurrent multi-arch run (4 suites compiling+running on one shared
    // host), issue #478. 120s is ~3000x the observed worst case yet still
    // trips on a genuine never-resumed hang; ISH_TEST_WATCHDOG_SCALE widens it
    // further for extreme runs.
    alarm(test_watchdog_secs(120));
    test_group_stop_reported();
    test_seize_after_group_stop();
    return finish_suite("ptrace_group_stop");
}
