// pidfd_open (and the io_uring stub family) must not SIGSYS on amd64.
//
// pidfd_open (#434) is now a real implementation (kernel/pidfd.c, issue #423
// Tier 1b -- see pidfd_clone.c for its functional behavior); this
// test is what's left of its original purpose: a regression lock on the
// amd64 arg-count classification. It was originally an ENOSYS stub in the
// syscall table but absent from amd64_syscall_legacy_arg_count, so it fell
// to the default 6-arg classification: the marshaller validated unused upper
// arg registers and, when one held a value with high bits set (not a
// sign-extended -1), raised SIGSYS ("needs full-width args") -- killing
// login (exit 159). io_uring's stubs have the same classification gap and
// are still ENOSYS.
//
// This test passes garbage with high bits in the unused arg registers to
// force the old failure; it must return cleanly (ENOSYS, or success now that
// pidfd_open is implemented), never be killed by SIGSYS. Arch-neutral.
//
// seccomp (#317 amd64 / #354 i386) is the same bug class with a twist: the arg
// that tripped the marshaller is a *real* one -- the 3rd arg is a sock_fprog*
// whose 64-bit guest address has high bits set on amd64. It was likewise absent
// from amd64_syscall_legacy_arg_count, so it fell to default-6 and SIGSYS-killed
// man-db's sandbox probe. We exercise it with a genuine on-stack pointer. It
// was an EOPNOTSUPP stub then; it is real now (tests/manual/seccomp_filter.c),
// so the zeroed stand-in program -- length 0 -- is refused EINVAL, as Linux
// refuses it, which still proves the pointer reached the kernel.
#define _GNU_SOURCE
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <sys/syscall.h>
#include "test_common.h"

#ifndef SYS_pidfd_open
# ifdef __NR_pidfd_open
#  define SYS_pidfd_open __NR_pidfd_open
# else
#  define SYS_pidfd_open 434 // "common" number on i386 and amd64
# endif
#endif

#ifndef SYS_seccomp
# ifdef __NR_seccomp
#  define SYS_seccomp __NR_seccomp
# elif defined(__x86_64__)
#  define SYS_seccomp 317
# else
#  define SYS_seccomp 354 // i386
# endif
#endif

#define SECCOMP_SET_MODE_FILTER 1

#ifndef SYS_membarrier
# ifdef __NR_membarrier
#  define SYS_membarrier __NR_membarrier
# elif defined(__x86_64__)
#  define SYS_membarrier 324
# else
#  define SYS_membarrier 375 // i386
# endif
#endif

#define MEMBARRIER_CMD_QUERY 0

#define GARBAGE 0x1ffffffffULL // high bits set, not a sign-extended -1

static void check(const char *name, long r, int e) {
    if (r >= 0)
        test_logf("%s -> %ld (implemented)\n", name, r);
    else if (e == ENOSYS || e == EOPNOTSUPP)
        test_logf("%s -> -1 %s (callers fall back) ok\n", name, strerror(e));
    else {
        printf("FAIL: %s -> -1 %s (want ENOSYS or success)\n", name, strerror(e));
        failures_total++;
    }
}

int main(int argc, char **argv) {
    test_init(argc, argv);

    // pidfd_open(getpid(), 0), but stuff garbage into the 4 unused arg registers
    // so the marshaller would reject them under the old default-6 classification.
    errno = 0;
    long r = syscall(SYS_pidfd_open, (long) getpid(), 0L, GARBAGE, GARBAGE, GARBAGE, GARBAGE);
    check("pidfd_open", r, errno);

    if (r >= 0)
        close((int) r);

    // seccomp(SECCOMP_SET_MODE_FILTER, 0, &prog): arg3 is a real pointer whose
    // amd64 guest address has high bits set -- the exact case that SIGSYS'd mandb.
    // A zeroed stand-in is a program of length 0, which is EINVAL.
    char prog[16] = {0}; // stand-in for struct sock_fprog
    errno = 0;
    r = syscall(SYS_seccomp, (long) SECCOMP_SET_MODE_FILTER, 0L,
                (long) (intptr_t) prog, 0L, 0L, 0L);
    if (r < 0 && errno == EINVAL)
        test_logf("seccomp -> -1 EINVAL (empty program refused) ok\n");
    else
        check("seccomp", r, errno);

    // membarrier: the same marshaller hazard from the other direction -- an
    // over-count. Base ABI is (cmd, flags); the optional 3rd arg cpuid is unset
    // garbage in liburcu's 2-arg syscall() calls (syslog-ng) and sys_membarrier
    // ignores it. It was classified 3-arg, so the marshaller validated the
    // garbage 3rd register and SIGSYS'd. Force the old failure with an explicit
    // high-bits 3rd arg; QUERY must dispatch and report the supported mask.
    errno = 0;
    r = syscall(SYS_membarrier, (long) MEMBARRIER_CMD_QUERY, 0L, (long) GARBAGE, 0L, 0L, 0L);
    check("membarrier", r, errno);

    return finish_suite("pidfd_open");
}
