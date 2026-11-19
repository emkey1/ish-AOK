// ptrace_singlestep (arm64-only): PTRACE_SINGLESTEP must execute exactly one
// guest instruction and stop, not run the tracee to completion.
//
// AOK-iSH's arm64 JIT frontend (jit/jit.c's GUEST_ABI_ARM64 dispatch branch)
// never checked cpu->tf (the trap/single-step flag PTRACE_SINGLESTEP's
// kernel/ptrace.c handler sets) at all -- it unconditionally called the
// ordinary run-to-interrupt path, so single-step behaved exactly like
// PTRACE_CONT: the tracee ran to completion (or to its next real interrupt)
// instead of stopping after one instruction. This broke any debugger's
// stepi/step/next, and the "restore original instruction, single-step past
// it, reinsert breakpoint" dance every debugger does even to report the very
// first breakpoint hit cleanly -- found chasing a report that gdb debugging
// was "totally unusable" on arm64 (a separate, already-fixed bug), where gdb
// reported a spurious SIGSEGV at a garbage PC instead of ever reporting a
// breakpoint hit.
//
// Regression shape: the child spins a tight, deterministic loop incrementing
// a counter in shared (MAP_SHARED) memory many times, then exits. The parent
// single-steps it a bounded, small number of times and checks two
// independent, hard-to-fake invariants:
//   1. The tracee must still be alive and stopped after each single step --
//      the original bug made it exit (or run far past) after the very first
//      PTRACE_SINGLESTEP.
//   2. The PC must advance a small, plausible amount per step (never by more
//      than a few instructions' worth, since arm64 instructions are fixed
//      4 bytes each) -- and the shared counter must stay far below the
//      loop's full iteration count, directly refuting "one singlestep call
//      ran the whole loop."
//
// Deliberately arm64-only: this whole class of bug (no interpreter fallback,
// single-step implemented as an ephemeral one-instruction JIT block) is
// specific to the arm64 JIT frontend; amd64/i386/riscv64 have (or should be
// verified to have) working interpreter-based single-step already.

#define _GNU_SOURCE

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_common.h"

#ifndef PTRACE_SEIZE
#define PTRACE_SEIZE 0x4206
#endif
#ifndef PTRACE_CONT
#define PTRACE_CONT 7
#endif
#ifndef PTRACE_SINGLESTEP
#define PTRACE_SINGLESTEP 9
#endif
#ifndef PTRACE_GETREGSET
#define PTRACE_GETREGSET 0x4204
#endif
#ifndef NT_PRSTATUS
#define NT_PRSTATUS 1
#endif
#ifndef PTRACE_EVENT_STOP
#define PTRACE_EVENT_STOP 128
#endif

// Mirrors kernel/ptrace.h's struct user_pt_regs_arm64_: x0..x30, sp, pc, pstate.
struct arm64_regs {
    unsigned long regs[31];
    unsigned long sp;
    unsigned long pc;
    unsigned long pstate;
};

#define LOOP_ITERATIONS 1000000
#define SINGLESTEP_COUNT 40
// Generous upper bound on how far the shared counter could plausibly move
// across SINGLESTEP_COUNT single steps -- real single-stepping through the
// loop's few-instruction body advances the counter by at most a handful per
// step (most steps don't even complete one full increment); the original bug
// ran the ENTIRE loop (LOOP_ITERATIONS) after the very first step.
#define MAX_PLAUSIBLE_COUNTER (SINGLESTEP_COUNT * 4)

static pid_t g_child;

static void on_alarm(int sig) {
    (void) sig;
    if (g_child > 0)
        kill(g_child, SIGKILL);
    static const char msg[] = "ptrace_singlestep: FAIL timeout\n";
    write(2, msg, sizeof(msg) - 1);
    _exit(1);
}

static int getregs(pid_t pid, struct arm64_regs *regs) {
    struct iovec iov = { .iov_base = regs, .iov_len = sizeof(*regs) };
    return ptrace(PTRACE_GETREGSET, pid, (void *) NT_PRSTATUS, &iov);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    signal(SIGALRM, on_alarm);
    alarm(test_watchdog_secs(30));

    volatile long *counter = mmap(NULL, sizeof(long), PROT_READ | PROT_WRITE,
                                  MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (counter == MAP_FAILED) {
        failf("ptrace_singlestep mmap", (uint64_t) errno, 0, 0, 0, 0, 0);
        return finish_suite("ptrace_singlestep");
    }
    *counter = 0;

    g_child = fork();
    if (g_child < 0) {
        failf("ptrace_singlestep fork", (uint64_t) errno, 0, 0, 0, 0, 0);
        return finish_suite("ptrace_singlestep");
    }

    if (g_child == 0) {
        raise(SIGSTOP);
        for (long i = 0; i < LOOP_ITERATIONS; i++)
            (*counter)++;
        _exit(42);
    }

    pid_t child = g_child;
    if (ptrace(PTRACE_SEIZE, child, 0, 0) != 0) {
        failf("ptrace_singlestep seize", (uint64_t) errno, 0, 0, 0, 0, 0);
        kill(child, SIGKILL);
        waitpid(child, NULL, 0);
        return finish_suite("ptrace_singlestep");
    }
    test_logf("seized %d\n", (int) child);

    int saw_group_stop = 0;
    int tracing = 0;
    int steps_done = 0;
    unsigned long last_pc = 0;
    int pc_ever_changed = 0;
    int exited_too_early = 0;

    for (int iterations = 0; iterations < 200 && steps_done < SINGLESTEP_COUNT; iterations++) {
        int status;
        pid_t w = waitpid(child, &status, 0);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            failf("ptrace_singlestep waitpid", (uint64_t) errno, 0, 0, 0, 0, 0);
            break;
        }

        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            if (tracing && steps_done < SINGLESTEP_COUNT)
                exited_too_early = 1;
            break;
        }
        if (!WIFSTOPPED(status)) {
            failf("ptrace_singlestep status", (uint64_t) status, 0, 0, 0, 0, 0);
            break;
        }

        int sig = WSTOPSIG(status);
        int event = (status >> 16) & 0xff;

        if (!tracing) {
            // Group-stop handshake, same pattern as ptrace_group_stop.c /
            // riscv64/ptrace_regset.c.
            if (event == PTRACE_EVENT_STOP) {
                saw_group_stop = 1;
                tracing = 1;
                if (ptrace(PTRACE_SINGLESTEP, child, 0, 0) != 0) {
                    failf("ptrace_singlestep arm", (uint64_t) errno, 0, 0, 0, 0, 0);
                    break;
                }
            } else if (sig == SIGSTOP) {
                if (ptrace(PTRACE_CONT, child, 0, SIGSTOP) != 0) {
                    failf("ptrace_singlestep deliver SIGSTOP", (uint64_t) errno, 0, 0, 0, 0, 0);
                    break;
                }
            } else {
                ptrace(PTRACE_CONT, child, 0, (sig == SIGTRAP) ? 0 : (long) sig);
            }
            continue;
        }

        // Every stop from here on should be a single-step trap.
        struct arm64_regs regs;
        if (getregs(child, &regs) != 0) {
            failf("ptrace_singlestep GETREGSET", (uint64_t) errno, 0, 0, 0, 0, 0);
            break;
        }
        steps_done++;
        if (steps_done > 1 && regs.pc != last_pc)
            pc_ever_changed = 1;
        test_logf("step %d: pc=%#lx counter=%ld\n", steps_done, regs.pc, *counter);
        last_pc = regs.pc;

        if (*counter > MAX_PLAUSIBLE_COUNTER) {
            failf("ptrace_singlestep counter ran ahead", (uint64_t) *counter, 0, 0,
                  (uint64_t) MAX_PLAUSIBLE_COUNTER, 0, 0);
            break;
        }

        if (steps_done >= SINGLESTEP_COUNT)
            break;

        if (ptrace(PTRACE_SINGLESTEP, child, 0, 0) != 0) {
            failf("ptrace_singlestep resume", (uint64_t) errno, 0, 0, 0, 0, 0);
            break;
        }
    }

    kill(child, SIGKILL);
    waitpid(child, NULL, 0);

    if (!saw_group_stop)
        failf("ptrace_singlestep never saw group-stop", 0, 0, 0, 0, 0, 0);
    if (exited_too_early)
        failf("ptrace_singlestep tracee exited after too few steps", (uint64_t) steps_done,
              0, 0, (uint64_t) SINGLESTEP_COUNT, 0, 0);
    if (steps_done < SINGLESTEP_COUNT)
        failf("ptrace_singlestep too few steps completed", (uint64_t) steps_done, 0, 0,
              (uint64_t) SINGLESTEP_COUNT, 0, 0);
    if (!pc_ever_changed)
        failf("ptrace_singlestep PC never advanced", 0, 0, 0, 0, 0, 0);
    if (*counter > MAX_PLAUSIBLE_COUNTER)
        failf("ptrace_singlestep final counter too high", (uint64_t) *counter, 0, 0,
              (uint64_t) MAX_PLAUSIBLE_COUNTER, 0, 0);

    munmap((void *) counter, sizeof(long));
    return finish_suite("ptrace_singlestep");
}
