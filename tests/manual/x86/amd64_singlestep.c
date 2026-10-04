// amd64 PTRACE_SINGLESTEP: one guest instruction per stop, on the JIT.
//
// Until 2026-09-07 this path did not exist. cpu_single_step_amd64_frontend was
// three lines routing cpu->tf straight to emu/amd64_interp.c, and it was the
// last place ordinary amd64 execution reached the interpreter. It is now a
// real one-instruction JIT block, matching what i386 and arm64 already did --
// and nothing in the tree tested amd64 single-step at all, so a regression
// would have been silent (tools/ptraceomatic.c is i386, and
// tests/manual/arm64/ptrace_singlestep.c is arm64).
//
// Everything asserted here is address-INDEPENDENT -- a step COUNT and register
// values, never a RIP -- so the same source can be diffed against real x86_64
// hardware even though the guest loads at a different address. It was: the
// output below is byte-identical on camd, under ISH_HOST_AMD64_JIT=1, and
// under =0.
//
// The step count is the real assertion. A single-step path that executed two
// instructions per stop, or that re-executed one, still ends with the right
// register values; only counting the stops between two markers catches it.
// The markers are register writes rather than addresses, precisely so the
// count can be compared across engines and machines.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include "../test_common.h"

static const char *suite = "amd64_singlestep";

#if defined(__x86_64__)

#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/user.h>

// Twelve instructions between the two markers retiring, chosen to cover the
// shapes a one-instruction block has to terminate correctly: immediate moves,
// a three-operand imul, a logical, two one-operand group-3 forms, a shift, a
// register move, and a call, ret and jmp. The child runs victim() once before
// it is traced, so the frontend has dispatched (and published to its return
// cache) the block at the ret's target: a ret that entered that block from
// the cache would run on past one instruction (math.S amd64_sret). Today the
// single-step path cannot -- it runs on a fresh frame with an empty cache, and
// its translation does not use amd64_sret -- and this keeps it that way.
static void __attribute__((noinline)) victim(void) {
    __asm__ volatile(
        "movq $0x1111, %%rbx\n\t"
        "movq $1, %%rax\n\t"
        "addq $2, %%rax\n\t"
        "imulq $3, %%rax, %%rax\n\t"
        "xorq $7, %%rax\n\t"
        "notq %%rax\n\t"
        "negq %%rax\n\t"
        "shlq $2, %%rax\n\t"
        "movq %%rax, %%rcx\n\t"
        "call 1f\n\t"
        "jmp 2f\n\t"
        "1: ret\n\t"
        "2: movq $0x2222, %%rbx\n\t"
        ::: "rax", "rbx", "rcx");
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));

    pid_t child = fork();
    if (child < 0) {
        printf("%s: SKIP (fork failed, errno=%d)\n", suite, errno);
        return 0;
    }
    if (child == 0) {
        victim(); // warm the return cache (see victim)
        if (ptrace(PTRACE_TRACEME, 0, 0, 0) != 0)
            _exit(90);
        raise(SIGSTOP);
        victim();
        _exit(0);
    }

    int st;
    if (waitpid(child, &st, 0) < 0) {
        printf("%s: SKIP (waitpid failed, errno=%d)\n", suite, errno);
        return 0;
    }
    if (!WIFSTOPPED(st)) {
        printf("%s: SKIP (child did not stop for tracing)\n", suite);
        return 0;
    }

    long steps_between = -1;
    int seen_in = 0, stepped = 0, ptrace_ok = 1;
    unsigned long rax_out = 0, rcx_out = 0;

    for (long i = 0; i < 2000000; i++) {
        if (ptrace(PTRACE_SINGLESTEP, child, 0, 0) != 0) {
            ptrace_ok = 0;
            break;
        }
        stepped = 1;
        if (waitpid(child, &st, 0) < 0 || !WIFSTOPPED(st))
            break;
        struct user_regs_struct r;
        if (ptrace(PTRACE_GETREGS, child, 0, &r) != 0)
            continue;
        if (!seen_in && r.rbx == 0x1111) {
            seen_in = 1;
            steps_between = 0;
            continue;
        }
        if (seen_in) {
            steps_between++;
            if (r.rbx == 0x2222) {
                rax_out = (unsigned long) r.rax;
                rcx_out = (unsigned long) r.rcx;
                break;
            }
        }
    }
    ptrace(PTRACE_KILL, child, 0, 0);
    waitpid(child, &st, 0);

    if (!ptrace_ok && !stepped) {
        printf("%s: SKIP (PTRACE_SINGLESTEP unavailable, errno=%d)\n", suite, errno);
        return 0;
    }

    // Exactly twelve stops from the first marker retiring to the second.
    if (steps_between != 12)
        failf("one instruction per single-step stop",
              (uint64_t) steps_between, 0, 0, 12, 0, 0);
    // ((((1+2)*3) ^ 7) -> not -> neg) << 2, and the copy into rcx must agree.
    if (rax_out != 60)
        failf("stepped arithmetic result", rax_out, 0, 0, 60, 0, 0);
    if (rcx_out != rax_out)
        failf("register copy observed after the step", rcx_out, 0, 0, rax_out, 0, 0);

    return finish_suite(suite);
}

#else  /* !__x86_64__ */

int main(int argc, char **argv) {
    test_init(argc, argv);
    printf("%s: SKIP (x86_64 guest only)\n", suite);
    return 0;
}

#endif
