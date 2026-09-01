// stack_guard_gap.c — regression for the stack growing into its neighbours
// (issue #521).
//
// Linux keeps a gap between the stack and whatever is mapped below it. Growth
// is refused when the next mapping down is closer than stack_guard_gap, so a
// runaway recursion faults in open space and the process gets the SIGSEGV it
// is entitled to.
//
// AOK bounded stack growth only by the distance to the mapping ABOVE, which a
// recursion never trips: each fault lands one page below the last, so the gap
// is always one page and growth was always allowed. The stack descended until
// it reached whatever was mapped beneath it -- and then stopped faulting
// altogether, because those pages were present and writable. The frames
// simply landed IN the neighbouring mapping.
//
// On a 64-bit guest nothing is close enough below to hit. On i386 the address
// space is 4 GiB and crowded, and the stack walked straight through musl's
// thread block ~134 MiB down. The SIGSEGV, when it finally came, was
// delivered correctly onto the alternate stack -- and the handler died on its
// first libc call, because the thread pointer had been overwritten. That is
// what broke gnulib's "checking for working sigaltstack" probe, and with it
// every configure run in a Buildroot build.
//
// Two properties, so a pass means the gap is real and not merely that the
// signal arrived:
//   - a canary mapping placed below the stack still holds its pattern after
//     the overflow, and
//   - libc works inside the handler, which on musl means TLS survived.
//
// Both cases run in a child: the overflow is fatal by design when the gap is
// missing, and the parent has to live to report it.
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include "test_common.h"

#define CANARY_BYTE   0x5a
#define CANARY_PAGES  16
// Far enough below the stack that the guard gap stops the descent quickly,
// close enough that the recursion reaches it in well under a second.
#define CANARY_BELOW  (8 * 1024 * 1024)

// Not SIGSTKSZ: glibc has made it a sysconf() call, so it cannot size an
// array. 256 KiB is comfortably above every SIGSTKSZ in play.
#define ALTSTACK_SIZE (256 * 1024)
static char altstack[ALTSTACK_SIZE];
static volatile unsigned char *canary;
static size_t canary_len;
static volatile int verdict;

// Exit codes carried back from the child.
#define V_HANDLER_RAN     0
#define V_CANARY_CLOBBERED 3
#define V_TLS_BROKEN      4
#define V_NOT_ON_ALTSTACK 5
#define V_NEVER_FAULTED   6

static void handler(int sig) {
    (void) sig;
    char local;

    if (&local < altstack || &local >= altstack + sizeof altstack)
        _exit(V_NOT_ON_ALTSTACK);

    // Does libc still work? On musl, errno lives in thread-local storage
    // reached through the thread pointer -- the very thing the descending
    // stack used to overwrite. A failing close() must leave EBADF behind.
    errno = 0;
    close(-1);
    if (errno != EBADF)
        _exit(V_TLS_BROKEN);

    if (canary != NULL) {
        for (size_t i = 0; i < canary_len; i++) {
            if (canary[i] != CANARY_BYTE)
                _exit(V_CANARY_CLOBBERED);
        }
    }
    _exit(V_HANDLER_RAN);
}

// volatile and the returned pointer keep the compiler from turning this into
// a loop or eliding the frames; this is gnulib's own shape.
static volatile int *recurse_1(volatile int n, volatile int *p) {
    if (n >= 0)
        *recurse_1(n + 1, p) += n;
    return p;
}

static int child_overflow(int want_canary) {
    char here;
    canary = NULL;
    canary_len = 0;

    if (want_canary) {
        canary_len = (size_t) CANARY_PAGES * (size_t) getpagesize();
        // A hint, not MAP_FIXED: nothing may be unmapped out from under the
        // process to make room. If the hint is not honoured the placement
        // check below drops the canary and the TLS half still runs.
        void *hint = (void *) (((uintptr_t) &here - CANARY_BELOW)
                               & ~(uintptr_t) (getpagesize() - 1));
        void *p = mmap(hint, canary_len, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED)
            return V_NEVER_FAULTED;
        // Only useful if it actually landed below the stack and near enough
        // that the recursion will arrive at it.
        if ((uintptr_t) p < (uintptr_t) &here &&
            (uintptr_t) &here - (uintptr_t) p <= 2 * CANARY_BELOW) {
            memset(p, CANARY_BYTE, canary_len);
            canary = p;
        } else {
            munmap(p, canary_len);
            canary_len = 0;
        }
    }

    stack_t ss = {.ss_sp = altstack, .ss_size = sizeof altstack, .ss_flags = 0};
    if (sigaltstack(&ss, NULL) < 0)
        return V_NEVER_FAULTED;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = handler;
    sa.sa_flags = SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);

    int sum = 0;
    (void) *recurse_1(0, &sum);
    return V_NEVER_FAULTED;              // the recursion returned: no fault
}

// Runs one case in a child. Returns the child's exit status, or -1 if it died
// on a signal -- which is itself the bug when the gap is missing.
static int run_case(int want_canary, int *killed_by) {
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0)
        return -2;
    if (pid == 0)
        _exit(child_overflow(want_canary));
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        continue;
    if (WIFSIGNALED(st)) {
        *killed_by = WTERMSIG(st);
        return -1;
    }
    *killed_by = 0;
    return WEXITSTATUS(st);
}

static void check(const char *label, int want_canary) {
    int killed_by = 0;
    int rc = run_case(want_canary, &killed_by);
    if (rc == V_HANDLER_RAN)
        return;
    if (rc == -1)
        printf("FAIL: %s: killed by signal %d (the overflow was never "
               "contained)\n", label, killed_by);
    else if (rc == V_CANARY_CLOBBERED)
        printf("FAIL: %s: the stack grew through the mapping below it\n", label);
    else if (rc == V_TLS_BROKEN)
        printf("FAIL: %s: libc unusable in the handler (TLS overwritten)\n", label);
    else if (rc == V_NOT_ON_ALTSTACK)
        printf("FAIL: %s: handler did not run on the alternate stack\n", label);
    else if (rc == V_NEVER_FAULTED)
        printf("FAIL: %s: the recursion never faulted\n", label);
    else
        printf("FAIL: %s: unexpected exit %d\n", label, rc);
    failf(label, (uint64_t) rc, (uint64_t) killed_by, 0, 0, 0, 0);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));
    // The gnulib probe's own shape, and the same thing with a canary in the
    // path the runaway stack used to take.
    check("overflow_caught_with_libc_intact", 0);
    check("overflow_stops_before_neighbour", 1);
    return finish_suite("stack_guard_gap");
}
