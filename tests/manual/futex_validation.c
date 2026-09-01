// Three things futex(2) got wrong about its own arguments.
//
//   A futex word is a 32-bit value and every operation on it has to be
//   atomic, so it must be 4-byte aligned; Linux's get_futex_key rejects
//   anything else outright. Accepting an unaligned address meant the
//   atomicity the whole interface rests on quietly did not hold, and a caller
//   that had miscomputed its address got a lock that sometimes worked.
//
//   FUTEX_WAIT_BITSET with an already-expired absolute deadline returned
//   ETIMEDOUT without looking at anything else. Linux runs the wait setup
//   first, so a value that does not match is EAGAIN and an unreadable address
//   is EFAULT -- and only a caller whose word DID match gets ETIMEDOUT.
//   Answering ETIMEDOUT for all three told a caller its lock was contended
//   when the truth was that it had passed the wrong value or a bad pointer.
//
//   FUTEX_WAKE_OP reported EINVAL for an operation it does not implement,
//   where Linux says ENOSYS -- EINVAL means the caller passed a bad VALUE,
//   which sends it looking for a different bug. And FUTEX_OP_OPARG_SHIFT,
//   which means "the argument is a shift count", was refused for a count
//   above 31 where Linux masks it to the register width: a legal encoding
//   turned into an error.
//
// Measured against x86_64 glibc on Linux 6.12.
#define _GNU_SOURCE
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

// linux/futex.h is absent from some sysroots this compiles against; these are
// ABI constants.
#ifndef FUTEX_WAIT
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_CMP_REQUEUE 4
#define FUTEX_WAKE_OP 5
#define FUTEX_WAIT_BITSET 9
#define FUTEX_CLOCK_REALTIME 256
#define FUTEX_BITSET_MATCH_ANY 0xffffffff
#endif
#ifndef FUTEX_OP_SET
#define FUTEX_OP_SET 0
#endif

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-58s got=%-10ld want=%ld\n", label, got, want);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));

    // ---- alignment ---------------------------------------------------------
    {
        static char buf[64] __attribute__((aligned(8)));
        volatile int *bad = (volatile int *) (buf + 1);
        volatile int *ok = (volatile int *) buf;
        struct timespec brief = { 0, 1000000 };
        errno = 0;
        ck("FUTEX_WAKE on an unaligned address is EINVAL",
           syscall(SYS_futex, bad, FUTEX_WAKE, 1, NULL, NULL, 0) < 0 ? errno : 0, EINVAL);
        errno = 0;
        ck("FUTEX_WAIT on an unaligned address is EINVAL",
           syscall(SYS_futex, bad, FUTEX_WAIT, 0, &brief, NULL, 0) < 0 ? errno : 0, EINVAL);
        // An unaligned SECOND address is refused the same way.
        errno = 0;
        ck("an unaligned uaddr2 is EINVAL too",
           syscall(SYS_futex, ok, FUTEX_WAKE_OP, 1, (void *) 1, bad, 0) < 0 ? errno : 0, EINVAL);
        // ...and an aligned one still works, so it is an alignment check and
        // not a blanket refusal.
        errno = 0;
        ck("an aligned FUTEX_WAKE still works",
           (long) syscall(SYS_futex, ok, FUTEX_WAKE, 1, NULL, NULL, 0), 0);
    }

    // ---- an expired absolute deadline validates first ----------------------
    {
        static volatile int word = 5;
        struct timespec past = { 1, 0 };    // absolute, long gone
        errno = 0;
        long r = syscall(SYS_futex, &word, FUTEX_WAIT_BITSET | FUTEX_CLOCK_REALTIME,
                         99 /* != 5 */, &past, NULL, FUTEX_BITSET_MATCH_ANY);
        ck("expired deadline + wrong value is EAGAIN", r < 0 ? errno : 0, EAGAIN);
        errno = 0;
        r = syscall(SYS_futex, (void *) 8, FUTEX_WAIT_BITSET | FUTEX_CLOCK_REALTIME,
                    0, &past, NULL, FUTEX_BITSET_MATCH_ANY);
        ck("expired deadline + bad address is EFAULT", r < 0 ? errno : 0, EFAULT);
        // Only when the word DOES match is the answer about the deadline.
        errno = 0;
        r = syscall(SYS_futex, &word, FUTEX_WAIT_BITSET | FUTEX_CLOCK_REALTIME,
                    5, &past, NULL, FUTEX_BITSET_MATCH_ANY);
        ck("expired deadline + right value is ETIMEDOUT", r < 0 ? errno : 0, ETIMEDOUT);
    }

    // ---- FUTEX_WAKE_OP encoding -------------------------------------------
    {
        static volatile int a = 0, b = 0;
        // Comparison nibble 9 does not exist.
        errno = 0;
        long r = syscall(SYS_futex, &a, FUTEX_WAKE_OP, 1, (void *) 1, &b, 9 << 24);
        ck("an unknown comparison is ENOSYS", r < 0 ? errno : 0, ENOSYS);
        ck("  and specifically not EINVAL", r < 0 && errno == EINVAL ? 1 : 0, 0);

        // FUTEX_OP_OPARG_SHIFT (bit 31) with a shift count above 31: Linux
        // masks it, so 40 becomes 8 and the target gets 1 << 8.
        b = 0;
        errno = 0;
        r = syscall(SYS_futex, &a, FUTEX_WAKE_OP, 1, (void *) 1, &b,
                    (1 << 31) | (FUTEX_OP_SET << 28) | (40 << 12));
        ck("OPARG_SHIFT with a count of 40 succeeds", r, 0);
        ck("  having masked it to 8", (long) b, 256);

        // A shift count already in range is unchanged.
        b = 0;
        errno = 0;
        r = syscall(SYS_futex, &a, FUTEX_WAKE_OP, 1, (void *) 1, &b,
                    (1 << 31) | (FUTEX_OP_SET << 28) | (3 << 12));
        ck("OPARG_SHIFT with a count of 3 succeeds", r, 0);
        ck("  giving 1 << 3", (long) b, 8);

        // A plain, valid encoding still works.
        b = 0;
        errno = 0;
        r = syscall(SYS_futex, &a, FUTEX_WAKE_OP, 1, (void *) 1, &b,
                    (FUTEX_OP_SET << 28) | (42 << 12));
        ck("a plain FUTEX_OP_SET succeeds", r, 0);
        ck("  and stored the value", (long) b, 42);
    }

    return finish_suite("futex_validation");
}
