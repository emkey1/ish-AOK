// signal_forced_trap: synchronous fault signals that arrive blocked (or
// ignored) must force-default and KILL, exactly like Linux's
// force_sig_info_to_task -- "if we unblock the signal, we always reset it to
// SIG_DFL, since we do not want to have a signal handler that was blocked be
// invoked when user space had explicitly blocked it."
//
// The AOK bug (found via npm/node on arm64): deliver_signal only
// force-defaulted the ignored and blocked-with-SIG_DFL cases. A synchronous
// SIGSEGV arriving while blocked WITH A CUSTOM HANDLER installed was queued
// and skipped, so the faulting instruction re-executed forever: the guest
// process spun at 100% CPU, unkillable from inside the guest. node/V8 crash
// paths block signals around abort sequences (musl blocks everything around
// abort/thread teardown), so any V8 fatal error under npm could wedge the
// whole emulated system instead of dying.
//
// Cases:
//   1. SIGSEGV blocked + custom handler + fault -> killed by SIGSEGV
//      (handler must NOT run)
//   2. SIGSEGV blocked + SIG_DFL + fault        -> killed by SIGSEGV
//   3. SIGSEGV ignored + fault                  -> killed by SIGSEGV
//   4. control: SIGSEGV unblocked + handler     -> handler runs, exits 42
//   5. __builtin_trap() with SIGTRAP/SIGILL blocked -> killed by the trap
//      signal (SIGTRAP on arm64's brk, SIGILL on x86's ud2)
//
// Every case runs in a fork()ed child with an alarm watchdog; a WEDGE (the
// bug) shows up as the watchdog SIGKILL instead of the expected signal.
// Passes on real Linux (these are exact kernel semantics).

#define _GNU_SOURCE

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_common.h"

static void fault_unmapped(void) {
    // High and reliably unmapped on every guest layout (i386's 32-bit
    // address space wraps this constant, so derive per pointer size).
    volatile int *p;
    if (sizeof(void *) == 8)
        p = (volatile int *) (uintptr_t) 0x123400000000ULL;
    else
        p = (volatile int *) (uintptr_t) 0xeeee0000U;
    *p = 42;
}

static void control_handler(int sig) {
    (void) sig;
    _exit(42);
}

static void must_not_run_handler(int sig) {
    (void) sig;
    // The whole point: a blocked synchronous trap must never invoke the
    // handler userspace blocked.
    _exit(43);
}

// what=0: blocked+handler, 1: blocked+SIG_DFL, 2: ignored,
// 3: control (unblocked+handler), 4: blocked __builtin_trap
static pid_t spawn_case(int what) {
    pid_t pid = fork();
    if (pid != 0)
        return pid;

    alarm(6); // SIGALRM default kills: converts a wedge into a visible failure

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigset_t block;
    sigemptyset(&block);

    switch (what) {
        case 0:
            sa.sa_handler = must_not_run_handler;
            sigaction(SIGSEGV, &sa, NULL);
            sigaddset(&block, SIGSEGV);
            sigprocmask(SIG_BLOCK, &block, NULL);
            fault_unmapped();
            break;
        case 1:
            sigaddset(&block, SIGSEGV);
            sigprocmask(SIG_BLOCK, &block, NULL);
            fault_unmapped();
            break;
        case 2:
            sa.sa_handler = SIG_IGN;
            sigaction(SIGSEGV, &sa, NULL);
            fault_unmapped();
            break;
        case 3:
            sa.sa_handler = control_handler;
            sigaction(SIGSEGV, &sa, NULL);
            fault_unmapped();
            break;
        case 4:
            sigaddset(&block, SIGTRAP);
            sigaddset(&block, SIGILL);
            sigprocmask(SIG_BLOCK, &block, NULL);
            __builtin_trap();
            break;
    }
    _exit(44); // survived the fault: also wrong
}

static void check_case(const char *name, int what, int want_sig, int want_exit) {
    pid_t pid = spawn_case(what);
    if (pid < 0) {
        failf("signal_forced_trap fork", (uint64_t) errno, 0, 0, 0, 0, 0);
        return;
    }
    int status;
    if (waitpid(pid, &status, 0) != pid) {
        failf("signal_forced_trap waitpid", (uint64_t) errno, 0, 0, 0, 0, 0);
        return;
    }
    if (want_sig != 0) {
        if (!WIFSIGNALED(status) ||
                (WTERMSIG(status) != want_sig &&
                 // __builtin_trap: brk/SIGTRAP on arm64, ud2/SIGILL on x86,
                 // unimp/SIGILL on riscv64 -- accept the arch's trap signal
                 !(what == 4 && (WTERMSIG(status) == SIGTRAP || WTERMSIG(status) == SIGILL)))) {
            failf(name, (uint64_t) status, 0, 0, (uint64_t) want_sig, 0, 0);
            return;
        }
        // SIGALRM means the watchdog fired: the old wedge, not a kill
        if (WTERMSIG(status) == SIGALRM) {
            failf(name, (uint64_t) status, 0, 0, (uint64_t) want_sig, 0, 0);
            return;
        }
    } else {
        if (!WIFEXITED(status) || WEXITSTATUS(status) != want_exit) {
            failf(name, (uint64_t) status, 0, 0, (uint64_t) want_exit, 0, 0);
            return;
        }
    }
    test_logf("%s: ok (status %#x)\n", name, status);
}

int main(int argc, char **argv) {
    test_init(argc, argv);

    check_case("blocked+handler fault", 0, SIGSEGV, 0);
    check_case("blocked+default fault", 1, SIGSEGV, 0);
    check_case("ignored fault", 2, SIGSEGV, 0);
    check_case("unblocked handler control", 3, 0, 42);
    check_case("blocked builtin_trap", 4, SIGTRAP, 0);

    return finish_suite("signal_forced_trap");
}
