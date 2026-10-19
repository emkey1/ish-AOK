// A call through NULL is SIGSEGV with si_code SEGV_MAPERR and si_addr 0, as
// it is on Linux on every architecture.
//
// The fetch fault reaches kernel/calls.c as INT_GPF with segfault_addr 0, and
// handle_i386_read_fault_gpf reads an address of 0 as "nothing reported" --
// unless the interrupt gadget set segfault_reported. ppc64's did; arm64's and
// riscv64's did not, so a call through NULL there came out as SI_KERNEL,
// which is what x86 reports for a #GP and what no Linux reports for this.
//
// Each case runs in a forked child whose SA_SIGINFO handler sends the
// siginfo back through a pipe. The control is a NULL data read, which takes
// the data-fault path and has always been reported correctly; a call to an
// unmapped page that is not NULL checks si_addr is the target and not 0.
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include "test_common.h"

static void check(int cond, const char *what) {
    if (cond) {
        test_logf("ok: %s\n", what);
    } else {
        printf("FAIL: %s (errno=%d %s)\n", what, errno, strerror(errno));
        failures_total++;
    }
}

struct report {
    int signo;
    int code;
    uintptr_t addr;
};

static int report_fd = -1;

static void on_fault(int sig, siginfo_t *info, void *ctx) {
    (void) ctx;
    struct report r = { sig, info->si_code, (uintptr_t) info->si_addr };
    ssize_t n = write(report_fd, &r, sizeof(r));
    _exit(n == (ssize_t) sizeof(r) ? 0 : 3);
}

enum fault_kind { READ_AT, CALL_AT };

// Runs one fault in a child. Returns 1 and fills *out when the child's
// handler reported a signal; 0 when it did not (it survived, or died some
// other way).
static int fault_report(enum fault_kind kind, uintptr_t addr, struct report *out) {
    int fds[2];
    if (pipe(fds) != 0) {
        check(0, "pipe");
        return 0;
    }
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        check(0, "fork");
        close(fds[0]);
        close(fds[1]);
        return 0;
    }
    if (pid == 0) {
        close(fds[0]);
        report_fd = fds[1];
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = on_fault;
        sa.sa_flags = SA_SIGINFO;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGSEGV, &sa, NULL);
        sigaction(SIGBUS, &sa, NULL);
        sigaction(SIGILL, &sa, NULL);
        if (kind == READ_AT) {
            volatile unsigned char *p = (volatile unsigned char *) addr;
            _exit(*p == 0 ? 40 : 41);
        }
        void (*volatile fn)(void) = (void (*)(void)) addr;
        fn();
        _exit(42);
    }
    close(fds[1]);
    struct report r;
    ssize_t n = read(fds[0], &r, sizeof(r));
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (n != (ssize_t) sizeof(r)) {
        test_logf("  no report: child %s %d\n",
                  WIFSIGNALED(status) ? "killed by signal" : "exited",
                  WIFSIGNALED(status) ? WTERMSIG(status) : WEXITSTATUS(status));
        return 0;
    }
    test_logf("  %s %#zx -> signal %d, si_code %d, si_addr %#zx\n",
              kind == READ_AT ? "read at" : "call to", (size_t) addr,
              r.signo, r.code, (size_t) r.addr);
    *out = r;
    return 1;
}

static void check_maperr(enum fault_kind kind, uintptr_t addr, const char *what) {
    struct report r;
    char label[160];
    int got = fault_report(kind, addr, &r);
    snprintf(label, sizeof(label), "%s: SIGSEGV", what);
    check(got && r.signo == SIGSEGV, label);
    if (!got)
        return;
    snprintf(label, sizeof(label), "%s: si_code SEGV_MAPERR (got %d)", what, r.code);
    check(r.code == SEGV_MAPERR, label);
    snprintf(label, sizeof(label), "%s: si_addr %#zx (got %#zx)", what,
             (size_t) addr, (size_t) r.addr);
    check(r.addr == addr, label);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));

    check_maperr(READ_AT, 0, "read of NULL (control)");
    check_maperr(CALL_AT, 0, "call through NULL");

    // A page that was mapped and is not any more: page-aligned, so every
    // architecture's alignment rule for a branch target holds.
    void *p = mmap((void *) 0x20000000, 4096, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED || munmap(p, 4096) != 0) {
        test_logf("skip: no page to unmap (%s)\n", strerror(errno));
    } else {
        check_maperr(CALL_AT, (uintptr_t) p, "call to an unmapped page");
    }

    return finish_suite("null_call_siginfo");
}
