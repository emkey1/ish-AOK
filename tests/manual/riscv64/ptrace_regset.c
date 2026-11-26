// ptrace_regset (riscv64-only): PTRACE_GETREGSET/SETREGSET(NT_PRSTATUS) must
// report and accept the real riscv64 register file. AOK-Workstation's riscv64 port
// (merged for 538) never wired ptrace's regset marshalling for
// GUEST_ABI_RISCV64 -- every request fell through to the i386 legacy layout,
// so a real `strace -f` on a riscv64 guest decoded every syscall as syscall 0
// (io_setup on the shared asm-generic table, i.e. all-zero register reads)
// and then crashed its own tracer process (issue #464).
//
// Synchronization follows ptrace_group_stop.c's proven pattern (SEIZE, then
// the child raises SIGSTOP -- "give the parent a beat"; the tracer delivers
// the SIGSTOP signal-stop to force a real group-stop, then resumes the
// PTRACE_EVENT_STOP report directly into syscall tracing).
//
// The test then drives the exact mechanism strace relies on:
//   1. GETREGSET at a syscall-entry stop must report the CORRECT syscall
//      number (via a7) for two different, known syscalls -- directly
//      refuting "always decodes as 0".
//   2. SETREGSET must actually take effect: rewriting a7 from SYS_write to
//      SYS_getpid at the entry stop must make the write() call *become* a
//      getpid() call, observed by comparing the return value at the exit
//      stop against the tracer's own knowledge of the child's pid -- not
//      just "the call didn't error", but "the kernel dispatched the
//      syscall we asked for".
//
// If the underlying bug is present the tracee's own reads/writes are
// garbage or the process crashes; with the fix this drives clean, correct
// syscall interception exactly like a real strace/seccomp-notify consumer.

#define _GNU_SOURCE

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
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
#ifndef PTRACE_SYSCALL
#define PTRACE_SYSCALL 24
#endif
#ifndef PTRACE_O_TRACESYSGOOD
#define PTRACE_O_TRACESYSGOOD 1
#endif
#ifndef PTRACE_GETREGSET
#define PTRACE_GETREGSET 0x4204
#endif
#ifndef PTRACE_SETREGSET
#define PTRACE_SETREGSET 0x4205
#endif
#ifndef NT_PRSTATUS
#define NT_PRSTATUS 1
#endif
#ifndef PTRACE_EVENT_STOP
#define PTRACE_EVENT_STOP 128
#endif

// Mirrors kernel/ptrace.h's struct user_regs_struct_riscv64_: pc followed by
// x1(ra)..x31(t6) in register-number order, matching the real kernel's
// struct user_regs_struct (asm/ptrace.h) exactly.
struct riscv64_regs {
    unsigned long pc;
    unsigned long ra, sp, gp, tp, t0, t1, t2, s0, s1;
    unsigned long a0, a1, a2, a3, a4, a5, a6, a7;
    unsigned long s2, s3, s4, s5, s6, s7, s8, s9, s10, s11;
    unsigned long t3, t4, t5, t6;
};

_Static_assert(sizeof(struct riscv64_regs) == 256, "riscv64_regs must match struct user_regs_struct exactly");

static pid_t g_child;

static void on_alarm(int sig) {
    (void) sig;
    if (g_child > 0)
        kill(g_child, SIGKILL);
    static const char msg[] = "ptrace_regset: FAIL timeout\n";
    write(2, msg, sizeof(msg) - 1);
    _exit(1);
}

static int getregset(pid_t pid, struct riscv64_regs *regs, size_t *got_len) {
    struct iovec iov = { .iov_base = regs, .iov_len = sizeof(*regs) };
    if (ptrace(PTRACE_GETREGSET, pid, (void *) NT_PRSTATUS, &iov) != 0)
        return -1;
    if (got_len)
        *got_len = iov.iov_len;
    return 0;
}

static int setregset(pid_t pid, struct riscv64_regs *regs) {
    struct iovec iov = { .iov_base = regs, .iov_len = sizeof(*regs) };
    return ptrace(PTRACE_SETREGSET, pid, (void *) NT_PRSTATUS, &iov);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    signal(SIGALRM, on_alarm);
    alarm(15);

    g_child = fork();
    if (g_child < 0) {
        failf("ptrace_regset fork", (uint64_t) errno, 0, 0, 0, 0, 0);
        return finish_suite("ptrace_regset");
    }

    if (g_child == 0) {
        // Give the parent a beat to PTRACE_SEIZE us before we stop (same
        // proven pattern as ptrace_group_stop.c).
        raise(SIGSTOP);
        // A known, easily-recognized pair of syscalls for the tracer to
        // observe across entry/exit stops.
        getpid();
        char c = 'x';
        write(2, &c, 0); // count=0: hijack target, but never actually writes
        _exit(0);
    }

    pid_t child = g_child;
    if (ptrace(PTRACE_SEIZE, child, 0, (void *) (long) PTRACE_O_TRACESYSGOOD) != 0) {
        failf("ptrace_regset seize", (uint64_t) errno, 0, 0, 0, 0, 0);
        kill(child, SIGKILL);
        waitpid(child, NULL, 0);
        return finish_suite("ptrace_regset");
    }
    test_logf("seized %d\n", (int) child);

    int seen_getpid = 0, seen_write = 0, substituted = 0, saw_group_stop = 0;
    long substituted_result = -1;
    // Real ptrace strictly alternates entry/exit on every syscall-stop,
    // unconditionally -- NOT based on whether the syscall number matches the
    // previous stop. (A syscall-number-based heuristic breaks exactly on the
    // substituted call below: rewriting a7 mid-flight makes the exit stop
    // report a *different* number than the entry stop it came from.) Starts
    // at 0 so the first flip below lands on 1 (entry) for the first stop.
    int at_entry = 0;
    // Resume request to issue each time through the loop: 0 until we're past
    // the group-stop handshake, then always a bare PTRACE_SYSCALL continue.
    int tracing_syscalls = 0;

    for (int iterations = 0; iterations < 200; iterations++) {
        int status;
        pid_t w = waitpid(child, &status, 0);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            failf("ptrace_regset waitpid", (uint64_t) errno, 0, 0, 0, 0, 0);
            break;
        }

        if (WIFEXITED(status) || WIFSIGNALED(status))
            break;
        if (!WIFSTOPPED(status)) {
            failf("ptrace_regset status", (uint64_t) status, 0, 0, 0, 0, 0);
            break;
        }

        int sig = WSTOPSIG(status);
        int event = (status >> 16) & 0xff;

        if (!tracing_syscalls) {
            // Group-stop handshake, exactly like ptrace_group_stop.c.
            if (event == PTRACE_EVENT_STOP) {
                saw_group_stop = 1;
                tracing_syscalls = 1;
                if (ptrace(PTRACE_SYSCALL, child, 0, 0) != 0) {
                    failf("ptrace_regset arm syscall trace", (uint64_t) errno, 0, 0, 0, 0, 0);
                    break;
                }
            } else if (sig == SIGSTOP) {
                if (ptrace(PTRACE_CONT, child, 0, SIGSTOP) != 0) {
                    failf("ptrace_regset deliver SIGSTOP", (uint64_t) errno, 0, 0, 0, 0, 0);
                    break;
                }
            } else {
                ptrace(PTRACE_CONT, child, 0, (sig == SIGTRAP) ? 0 : (long) sig);
            }
            continue;
        }

        if (sig != (SIGTRAP | 0x80)) {
            // Not a syscall stop (shouldn't happen once armed); pass it through.
            ptrace(PTRACE_SYSCALL, child, 0, (long) sig);
            continue;
        }

        struct riscv64_regs regs;
        size_t got_len;
        if (getregset(child, &regs, &got_len) != 0) {
            failf("ptrace_regset GETREGSET", (uint64_t) errno, 0, 0, 0, 0, 0);
            ptrace(PTRACE_SYSCALL, child, 0, 0);
            continue;
        }
        if (got_len != sizeof(regs)) {
            failf("ptrace_regset GETREGSET iov_len", got_len, 0, 0, sizeof(regs), 0, 0);
            ptrace(PTRACE_SYSCALL, child, 0, 0);
            continue;
        }

        long syscall_no = (long) regs.a7;
        at_entry = !at_entry;
        test_logf("syscall stop: no=%ld at_entry=%d a0=%#lx\n", syscall_no, at_entry, regs.a0);

        if (at_entry && syscall_no == SYS_getpid) {
            seen_getpid = 1;
        } else if (at_entry && syscall_no == SYS_write) {
            seen_write = 1;
            // The core regression check: rewrite the entry-stop's syscall
            // number so write() *becomes* getpid(). If SETREGSET doesn't
            // really take effect (the original bug's failure mode), the
            // child's write() proceeds unmodified and the result check
            // below fails.
            regs.a7 = SYS_getpid;
            if (setregset(child, &regs) != 0)
                failf("ptrace_regset SETREGSET", (uint64_t) errno, 0, 0, 0, 0, 0);
            else
                substituted = 1;
        } else if (!at_entry && substituted && substituted_result < 0) {
            // Exit stop of the substituted call: a0 now holds getpid()'s
            // return value (the child's own pid), not write()'s (0 bytes).
            substituted_result = (long) (int32_t) regs.a0;
        }

        if (ptrace(PTRACE_SYSCALL, child, 0, 0) != 0) {
            failf("ptrace_regset resume", (uint64_t) errno, 0, 0, 0, 0, 0);
            break;
        }
    }

    waitpid(child, NULL, 0);

    if (!saw_group_stop)
        failf("ptrace_regset never saw group-stop", 0, 0, 0, 0, 0, 0);
    if (!seen_getpid)
        failf("ptrace_regset never observed SYS_getpid", 0, 0, 0, 0, 0, 0);
    if (!seen_write)
        failf("ptrace_regset never observed SYS_write", 0, 0, 0, 0, 0, 0);
    if (!substituted)
        failf("ptrace_regset syscall substitution never attempted", 0, 0, 0, 0, 0, 0);
    if (substituted && substituted_result != child)
        failf("ptrace_regset substituted result", (uint64_t) substituted_result, 0, 0, (uint64_t) child, 0, 0);

    return finish_suite("ptrace_regset");
}
