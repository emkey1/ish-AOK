// Syscalls that were implemented but not reachable, because the per-ABI table
// entry was missing. The implementation existing is not the same as the guest
// being able to call it, and a libc that reaches the same functionality by
// another route hides the gap completely -- which is why these go through the
// raw syscall rather than the libc wrapper.
//
//  * truncate: i386 [92] was absent while truncate64 [193] was present, so a
//    statically-linked glibc calling it got the "missing syscall" ENOSYS. That
//    one was easy to spot. The bigger bug underneath was that
//    sys_truncate64_guest passed a NULL dirfd to generic_setattrat, which since
//    the AT_PWD work means "bad/closed dirfd" and returns EBADF -- so every
//    truncate(2) on EVERY ABI was failing, not just the unwired i386 entry.
//
//  * adjtimex: implemented since the clock_adjtime work but only wired into the
//    asm-generic table (arm64/riscv64 [171]); i386 [124] and amd64 [159] were
//    both missing. Reported upstream as ish-app/ish#1322. A modern glibc
//    implements adjtimex(3) on top of clock_adjtime64, which we already had, so
//    the libc wrapper worked fine and only the raw syscall failed.
//
//  * pread64/pwrite64: wired on every ABI, but the i386 entries pointed at
//    sys_pread/sys_pwrite, whose 4th parameter is a 64-bit off_t_ -- and the
//    legacy dispatcher only ever passes six 32-bit dwords, so the high half of
//    the offset was dropped. An i386 pwrite at 5GiB landed at 1GiB and silently
//    clobbered what was there, returning success and the right byte count.
//    Nothing about a wired-but-mismarshalled syscall shows up as an error,
//    which is why the >4GiB offsets below matter more than any errno check.
//
//  * preadv/pwritev/preadv2/pwritev2 and semctl: implemented and ABI-aware, but
//    the i386 table had no 333/334/378/379 entry at all, amd64 had no 295/296,
//    and i386 [394] pointed at semtimedop -- 394 is semctl on i386 (392 is
//    semtimedop), so every modern-musl semctl(2) was decoded as a semop vector.
//
// Arch-neutral: SYS_* resolve to the right per-ABI numbers.
//
// _FILE_OFFSET_BITS must be set before any header: on a 32-bit ABI glibc still
// defaults to a 32-bit off_t, which silently truncates every constant below --
// (off_t) 5 << 30 becomes 0x40000000, so the "5GiB" offsets collapse onto the
// 1GiB one and the entire >4GiB section degenerates into a no-op that passes.
// musl is always 64-bit here, so an i386-musl run looks fine and hides it;
// caught by building this file -m32 against glibc on the x86_64 oracle, where
// gcc also warns "result of '5 << 30' requires 34 bits". The static assert
// below is the part that cannot be ignored.
#define _FILE_OFFSET_BITS 64
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/timex.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <unistd.h>
#include "test_common.h"

// A 32-bit ABI splits every 64-bit syscall offset into a (low, high) dword
// pair. On i386 -- the only 32-bit ABI iSH runs -- the pair goes in two
// consecutive argument registers with no padding, because x86-32 has no
// even-register-pair alignment rule (compare arm/mips, which insert a dummy
// arg). 64-bit ABIs pass the offset whole in one register.
#if __SIZEOF_POINTER__ == 8
#define OFF_ARGS(off) ((long) (off))
#else
#define OFF_ARGS(off) \
    ((long) (uint32_t) (uint64_t) (off)), ((long) (uint32_t) ((uint64_t) (off) >> 32))
#endif

// pread64/pwrite64 take a single loff_t on a 64-bit ABI (OFF_ARGS above), but
// preadv/pwritev/preadv2/pwritev2 are SYSCALL_DEFINE5/6 on EVERY arch: the
// (pos_l, pos_h) pair is part of the ABI even on a 64-bit kernel, where
// pos_from_hilo() shifts pos_h clean out of the 64-bit loff_t and pos_l alone
// carries the offset. Passing just pos_l is not merely untidy -- for preadv2
// it leaves the *flags* argument holding whatever the caller happened to
// leave in that register, and Linux rejects an unrecognised RWF_* bit. That
// is not theoretical: on a real x86_64 kernel (Devuan 6, Linux 6.12) the very
// same 5-argument call returns 14 from one program and -1 from another,
// purely on register garbage. iSH ignores the flags word, so this only ever
// misbehaves against the oracle.
#if __SIZEOF_POINTER__ == 8
#define OFF_PAIR(off) ((long) (off)), 0L
#else
#define OFF_PAIR(off) OFF_ARGS(off)
#endif

static void check(const char *what, long got, long want) {
    if (got == want) {
        test_logf("  ok   %s = %ld\n", what, got);
        return;
    }
    printf("FAIL: %s = %ld (want %ld)\n", what, got, want);
    failures_total++;
}

static void check_str(const char *what, const char *got, const char *want) {
    if (strcmp(got, want) == 0) {
        test_logf("  ok   %s = '%s'\n", what, got);
        return;
    }
    printf("FAIL: %s = '%s' (want '%s')\n", what, got, want);
    failures_total++;
}

// --- positioned I/O with a >4GiB offset ----------------------------------
//
// Two markers written to a sparse file, one below 4GiB and one above it. If
// the high half of the offset is lost anywhere along the path, the second
// write lands on top of the first and reading back at 1GiB returns the 5GiB
// payload -- with both calls reporting success and the correct byte count, so
// the offset has to be checked by its effect on the data, not by an errno.
#define MARK_LEN 14
static const char MARK_LO[MARK_LEN + 1] = "AT-ONE-GIB----";
static const char MARK_HI[MARK_LEN + 1] = "AT-FIVE-GIB---";
_Static_assert(sizeof(off_t) == 8, "off_t must be 64-bit or the >4GiB offsets below truncate");
static const off_t OFF_LO = (off_t) 1 << 30;
static const off_t OFF_HI = (off_t) 5 << 30;

static void check_positioned_io(void) {
    const char *path = "/tmp/syscall_wiring_bigoff";
    unlink(path);
    int fd = open(path, O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        printf("FAIL: open for positioned I/O -> %s\n", strerror(errno));
        failures_total++;
        return;
    }

    // A filesystem that can't hold a sparse 5GiB file isn't what's under test.
    errno = 0;
    if (pwrite(fd, MARK_HI, MARK_LEN, OFF_HI) != MARK_LEN) {
        test_logf("  skip positioned I/O above 4GiB: pwrite -> %s\n", strerror(errno));
        close(fd);
        unlink(path);
        return;
    }

    char got[MARK_LEN + 1];
#define READ_BACK(off) do { memset(got, 0, sizeof got); \
    if (pread(fd, got, MARK_LEN, (off)) != MARK_LEN) { \
        printf("FAIL: pread readback -> %s\n", strerror(errno)); \
        failures_total++; \
        got[0] = '\0'; \
    } } while (0)

    // --- pread64/pwrite64 through the libc wrapper ------------------------
    check("pwrite(3) at 1GiB", pwrite(fd, MARK_LO, MARK_LEN, OFF_LO), MARK_LEN);
    READ_BACK(OFF_HI);
    check_str("pread(3) at 5GiB", got, MARK_HI);
    READ_BACK(OFF_LO);
    check_str("pread(3) at 1GiB is not the 5GiB payload", got, MARK_LO);

    // ...and through the raw syscall, which is where the (lo, hi) offset pair
    // is actually formed on a 32-bit ABI.
    errno = 0;
    long r = syscall(SYS_pwrite64, fd, MARK_LO, (size_t) MARK_LEN, OFF_ARGS(OFF_LO));
    check("raw SYS_pwrite64 at 1GiB", r, MARK_LEN);
    memset(got, 0, sizeof got);
    errno = 0;
    r = syscall(SYS_pread64, fd, got, (size_t) MARK_LEN, OFF_ARGS(OFF_HI));
    check("raw SYS_pread64 at 5GiB", r, MARK_LEN);
    check_str("raw SYS_pread64 at 5GiB payload", got, MARK_HI);
    memset(got, 0, sizeof got);
    errno = 0;
    r = syscall(SYS_pread64, fd, got, (size_t) MARK_LEN, OFF_ARGS(OFF_LO));
    check("raw SYS_pread64 at 1GiB", r, MARK_LEN);
    check_str("raw SYS_pread64 at 1GiB payload", got, MARK_LO);

    // A positioned read never moves the file position.
    check("pread leaves the file offset alone", (long) lseek(fd, 0, SEEK_CUR), 0);

    // --- preadv/pwritev ---------------------------------------------------
    // Split across two iovecs so a mis-sized iovec (the 32-bit i386 layout
    // read as the 64-bit one, or vice versa) shows up as a short or faulting
    // transfer rather than silently working.
#ifdef SYS_preadv
    {
        char lo[8], hi[MARK_LEN - 8 + 1];
        struct iovec riov[2] = { { lo, sizeof lo }, { hi, sizeof hi - 1 } };
        memset(lo, 0, sizeof lo);
        memset(hi, 0, sizeof hi);
        errno = 0;
        r = syscall(SYS_preadv, fd, riov, 2, OFF_PAIR(OFF_HI));
        check("raw SYS_preadv at 5GiB", r, MARK_LEN);
        if (r == MARK_LEN) {
            memcpy(got, lo, sizeof lo);
            memcpy(got + sizeof lo, hi, sizeof hi - 1);
            got[MARK_LEN] = '\0';
            check_str("raw SYS_preadv at 5GiB payload", got, MARK_HI);
        }
    }
#else
    test_logf("  (SYS_preadv undefined for this ABI, skipping)\n");
#endif
#ifdef SYS_pwritev
    {
        static const char vmark[MARK_LEN + 1] = "AT-FIVE-VEC---";
        struct iovec wiov[2] = { { (void *) vmark, 8 },
                                 { (void *) (vmark + 8), MARK_LEN - 8 } };
        errno = 0;
        r = syscall(SYS_pwritev, fd, wiov, 2, OFF_PAIR(OFF_HI));
        check("raw SYS_pwritev at 5GiB", r, MARK_LEN);
        READ_BACK(OFF_HI);
        check_str("raw SYS_pwritev at 5GiB payload", got, vmark);
        // The 1GiB marker must still be there: a truncated offset would have
        // put this write on top of it.
        READ_BACK(OFF_LO);
        check_str("pwritev at 5GiB did not clobber 1GiB", got, MARK_LO);
        // Put the original marker back for the preadv2 checks below.
        check("restore 5GiB marker", pwrite(fd, MARK_HI, MARK_LEN, OFF_HI), MARK_LEN);
    }
#else
    test_logf("  (SYS_pwritev undefined for this ABI, skipping)\n");
#endif

    // --- preadv2/pwritev2 -------------------------------------------------
    // Same as preadv/pwritev plus a trailing flags word, and an offset of -1
    // meaning "use and advance the file position" instead of a positioned
    // access. Flags stay 0: RWF_* are all performance/durability hints iSH
    // accepts and ignores.
#ifdef SYS_preadv2
    {
        char buf[MARK_LEN + 1];
        struct iovec iov[1] = { { buf, MARK_LEN } };
        memset(buf, 0, sizeof buf);
        errno = 0;
        // No ENOSYS escape hatch here: an unwired table entry IS the bug this
        // file is for, and iSH implements preadv2 on every ABI. A libc that
        // doesn't know the number is already excluded by the #ifdef.
        r = syscall(SYS_preadv2, fd, iov, 1, OFF_PAIR(OFF_HI), 0);
        check("raw SYS_preadv2 at 5GiB", r, MARK_LEN);
        buf[MARK_LEN] = '\0';
        check_str("raw SYS_preadv2 at 5GiB payload", buf, MARK_HI);

        // offset -1: reads at the current file position and advances it.
        check("seek to 1GiB", (long) (lseek(fd, OFF_LO, SEEK_SET) == OFF_LO), 1);
        memset(buf, 0, sizeof buf);
        errno = 0;
        r = syscall(SYS_preadv2, fd, iov, 1, OFF_PAIR((off_t) -1), 0);
        check("raw SYS_preadv2 with offset -1", r, MARK_LEN);
        buf[MARK_LEN] = '\0';
        check_str("raw SYS_preadv2 offset -1 payload", buf, MARK_LO);
        check("preadv2 offset -1 advanced the file position",
              (long) (lseek(fd, 0, SEEK_CUR) == OFF_LO + MARK_LEN), 1);
    }
#else
    test_logf("  (SYS_preadv2 undefined for this ABI, skipping)\n");
#endif
#ifdef SYS_pwritev2
    {
        static const char v2mark[MARK_LEN + 1] = "AT-FIVE-VEC2--";
        struct iovec iov[1] = { { (void *) v2mark, MARK_LEN } };
        errno = 0;
        r = syscall(SYS_pwritev2, fd, iov, 1, OFF_PAIR(OFF_HI), 0);
        check("raw SYS_pwritev2 at 5GiB", r, MARK_LEN);
        READ_BACK(OFF_HI);
        check_str("raw SYS_pwritev2 at 5GiB payload", got, v2mark);
        READ_BACK(OFF_LO);
        check_str("pwritev2 at 5GiB did not clobber 1GiB", got, MARK_LO);
    }
#else
    test_logf("  (SYS_pwritev2 undefined for this ABI, skipping)\n");
#endif
#undef READ_BACK

    close(fd);
    unlink(path);
}

// --- SysV semctl ----------------------------------------------------------
//
// SETVAL/GETVAL only: they carry no struct, so this fails on a wiring or
// numbering mistake and nothing else. On i386 this is syscall 394, which used
// to be semtimedop -- semctl(id, 0, SETVAL, 1) was then read as
// semop(id, (struct sembuf *) 0, 1) and came back EFAULT.
//
// cmd goes in WITHOUT IPC_64. x86_64's sys_semctl switches on the raw cmd, so
// SETVAL|IPC_64 does not match any case and real Linux returns EINVAL (checked
// on Linux 6.12/x86_64; glibc's own semctl(3) does not set it either). i386's
// 394 is sys_old_semctl, which does run ipc_parse_version and would accept
// either, but IPC_64 there only selects the semid_ds layout -- irrelevant to
// the layout-free commands used here. iSH masks IPC_64 off for every ABI, so
// it accepts both; the portable form is the one worth asserting on.
static void check_sysv_semctl(void) {
#if defined(SYS_semget) && defined(SYS_semctl)
    errno = 0;
    long id = syscall(SYS_semget, 0 /* IPC_PRIVATE */, 1, 0600 | 01000 /* IPC_CREAT */);
    if (id < 0) {
        printf("FAIL: raw SYS_semget -> %s\n", strerror(errno));
        failures_total++;
        return;
    }
    errno = 0;
    long r = syscall(SYS_semctl, id, 0, 16 /* SETVAL */, 7);
    check("raw SYS_semctl SETVAL", r, 0);
    errno = 0;
    r = syscall(SYS_semctl, id, 0, 12 /* GETVAL */, 0);
    check("raw SYS_semctl GETVAL reads back SETVAL", r, 7);
    errno = 0;
    r = syscall(SYS_semctl, id, 0, 0 /* IPC_RMID */, 0);
    check("raw SYS_semctl IPC_RMID", r, 0);
#else
    test_logf("  (SYS_semget/SYS_semctl undefined for this ABI, skipping)\n");
#endif
}

// --- sched_setparam / sched_rr_get_interval / setdomainname ---------------
static void check_sched_and_domainname(void) {
#ifdef SYS_sched_setparam
    {
        int param = 0; // struct sched_param { int sched_priority; }
        errno = 0;
        long r = syscall(SYS_sched_setparam, 0, &param);
        check("raw SYS_sched_setparam priority 0", r, 0);
        // iSH is SCHED_OTHER-only, and Linux rejects a non-zero priority there.
        param = 1;
        errno = 0;
        r = syscall(SYS_sched_setparam, 0, &param);
        check("raw SYS_sched_setparam priority 1 is EINVAL", r < 0 && errno == EINVAL, 1);
    }
#else
    test_logf("  (SYS_sched_setparam undefined for this ABI, skipping)\n");
#endif

#ifdef SYS_sched_rr_get_interval
    {
        // The kernel writes its own __kernel_old_timespec here, whose fields
        // are register-width -- not libc's struct timespec, which on a 32-bit
        // ABI with a modern (time64) libc is 8 bytes wider than syscall 161
        // actually fills.
        struct { long sec; long nsec; } tp;
        memset(&tp, 0xa5, sizeof tp);
        errno = 0;
        long r = syscall(SYS_sched_rr_get_interval, 0, &tp);
        check("raw SYS_sched_rr_get_interval", r, 0);
        if (r == 0) {
            // Only the shape is portable, not the value. iSH reports a fixed
            // 100ms quantum; real Linux gives a SCHED_OTHER task whatever
            // get_rr_interval_fair() computes, which is 0 on an idle runqueue
            // (measured: {0, 0} on Linux 6.12/x86_64), and an unprivileged
            // caller cannot switch to SCHED_RR to see a real slice. Asserting
            // 100000000 here would be asserting an iSH-ism.
            //
            // The 0xa5 poison is the real check: it catches a handler that
            // writes nothing, and -- on a 64-bit ABI -- one that writes only a
            // 32-bit timespec into the 64-bit slot, since the upper half of
            // .nsec would survive as poison.
            check("sched_rr_get_interval quantum tv_sec", tp.sec, 0);
            check("sched_rr_get_interval filled in a sane tv_nsec",
                  tp.nsec >= 0 && tp.nsec < 1000000000L, 1);
        }
    }
#else
    test_logf("  (SYS_sched_rr_get_interval undefined for this ABI, skipping)\n");
#endif

#ifdef SYS_setdomainname
    {
        struct utsname before, after;
        char saved[sizeof before.domainname];
        if (uname(&before) < 0) {
            printf("FAIL: uname -> %s\n", strerror(errno));
            failures_total++;
            return;
        }
        snprintf(saved, sizeof saved, "%s", before.domainname);

        static const char probe[] = "ish-wiring-probe";
        errno = 0;
        long r = syscall(SYS_setdomainname, probe, (size_t) (sizeof probe - 1));
        if (r < 0 && errno == EPERM) {
            // Unprivileged is a legitimate outcome; ENOSYS/EFAULT is not.
            test_logf("  ok   raw SYS_setdomainname is EPERM (not root)\n");
            return;
        }
        check("raw SYS_setdomainname", r, 0);
        if (r == 0) {
            if (uname(&after) == 0)
                check_str("setdomainname is visible through uname", after.domainname, probe);
            errno = 0;
            r = syscall(SYS_setdomainname, saved, (size_t) strlen(saved));
            check("raw SYS_setdomainname restores the old value", r, 0);
        }
    }
#else
    test_logf("  (SYS_setdomainname undefined for this ABI, skipping)\n");
#endif
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));

    const char *path = "/tmp/syscall_wiring_test";
    unlink(path);
    int fd = open(path, O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        printf("syscall_wiring: FAIL setup: %s\n", strerror(errno));
        return 1;
    }
    if (write(fd, "0123456789ABCDEF", 16) != 16) {
        printf("syscall_wiring: FAIL setup write\n");
        close(fd);
        return 1;
    }
    close(fd);

    struct stat st;

    // --- truncate via the raw syscall -------------------------------------
    errno = 0;
    long r = syscall(SYS_truncate, path, (long) 8);
    if (r < 0) {
        printf("FAIL: raw SYS_truncate -> %s\n", strerror(errno));
        failures_total++;
    } else {
        stat(path, &st);
        check("raw SYS_truncate shrinks to 8", (long) st.st_size, 8);
    }

    // ...and via the libc wrapper, which may take a different route.
    errno = 0;
    if (truncate(path, 4) < 0) {
        printf("FAIL: truncate(3) -> %s\n", strerror(errno));
        failures_total++;
    } else {
        stat(path, &st);
        check("truncate(3) shrinks to 4", (long) st.st_size, 4);
    }

    // Growing, and the error case.
    errno = 0;
    if (truncate(path, 32) < 0) {
        printf("FAIL: truncate grow -> %s\n", strerror(errno));
        failures_total++;
    } else {
        stat(path, &st);
        check("truncate grows to 32", (long) st.st_size, 32);
    }
    errno = 0;
    r = truncate("/tmp/syscall_wiring_absent", 0);
    check("truncate of a missing file is ENOENT", r < 0 && errno == ENOENT, 1);

    unlink(path);

    // --- adjtimex via the raw syscall -------------------------------------
#ifdef SYS_adjtimex
    {
        struct timex tx;
        memset(&tx, 0, sizeof tx);
        tx.modes = 0;                    // pure query
        errno = 0;
        r = syscall(SYS_adjtimex, &tx);
        if (r < 0) {
            printf("FAIL: raw SYS_adjtimex -> %s\n", strerror(errno));
            failures_total++;
        } else {
            test_logf("  ok   raw SYS_adjtimex -> %ld (tick=%ld)\n", r, tx.tick);
            // A query returns a clock state, not an error, and populates the
            // struct. Real Linux reports TIME_ERROR (5) when unsynchronised.
            check("adjtimex query returns a clock state", r >= 0, 1);
            // The struct contents are only meaningful when libc's struct timex
            // agrees with the layout raw syscall 124 actually uses. On a 32-bit
            // ABI whose libc has a 64-bit time_t (musl 1.2+, glibc built with
            // _TIME_BITS=64), libc's timex is the 64-bit __kernel_timex layout
            // while syscall 124 is the legacy old_timex32 one, so `time` is 4
            // bytes wider than the kernel wrote and every field after it --
            // including tick -- is read from the wrong offset. That is a libc
            // property, not an emulator bug: real Linux reads tick=0 here too
            // (verified on the mint oracle by running this exact static i386
            // musl binary on an x86_64 kernel, which FAILs the same assertion).
            if (sizeof(void *) == 8 || sizeof(tx.time.tv_sec) == 4) {
                check("adjtimex query fills in tick", tx.tick != 0, 1);
            } else {
                test_logf("  skip adjtimex field check: libc timex is time64 "
                          "but raw syscall %d is the legacy layout\n", SYS_adjtimex);
            }
        }

        // Any modes != 0 needs privilege we deliberately do not grant.
        memset(&tx, 0, sizeof tx);
        tx.modes = ADJ_FREQUENCY;
        errno = 0;
        r = syscall(SYS_adjtimex, &tx);
        check("adjtimex with modes set is EPERM", r < 0 && errno == EPERM, 1);
    }
#else
    test_logf("  (SYS_adjtimex undefined for this ABI, skipping)\n");
#endif

    check_positioned_io();
    check_sysv_semctl();
    check_sched_and_domainname();

    if (failures_total != 0) {
        printf("syscall_wiring: FAIL failures=%u\n", failures_total);
        return 1;
    }
    printf("syscall_wiring: PASS\n");
    return 0;
}
