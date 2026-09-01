// Resource limits, scheduling policy, nice, and the half of getrusage that is
// not CPU time.
//
//   No rlimit except RLIMIT_NOFILE was enforced. RLIMIT_FSIZE and RLIMIT_NPROC
//   were stored, reported through getrlimit and /proc, and never consulted, so
//   `ulimit -f` and `ulimit -u` were decorative -- which is worse than not
//   supporting them, because the whole point is that something is relying on
//   them to hold.
//
//   prlimit64 returned EINVAL for any nonzero pid INCLUDING the caller's own,
//   which is the form glibc's prlimit(2) wrapper and `prlimit --pid N` both
//   use.
//
//   sched_setscheduler refused SCHED_BATCH and SCHED_IDLE outright, so `chrt
//   -b`, `chrt -i`, background indexers demoting themselves and anything
//   setting SCHED_RESET_ON_FORK got a hard failure from a call Linux always
//   accepts from an unprivileged process. sched_getscheduler always claimed
//   SCHED_OTHER whatever had been set, and sched_get_priority_max reported
//   EINVAL for SCHED_FIFO -- a constant table on Linux, so a runtime asking
//   the range concluded the policy did not exist.
//
//   getpriority returned a flat 0 on the raw syscall. The raw convention is
//   20-nice, so libc decoded that as niceness 20 -- a value outside Linux's
//   -20..19 range entirely -- and setpriority accepted everything and changed
//   nothing, so `renice` reported success and the next read disagreed.
//
//   getrusage filled utime and stime and left every other field 0, which is a
//   value Linux cannot produce for a process that has run: `time -v` printed a
//   peak RSS of 0 KB and no faults, and wait4 supervisors measuring a child
//   saw nothing at all.
//
// Measured against x86_64 glibc on Linux 6.12.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_common.h"

#define UNPRIV_UID 1000
#define UNPRIV_GID 1000
#define SCHED_RESET_ON_FORK_ 0x40000000

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-56s got=%-8ld want=%ld\n", label, got, want);
}

// For values that are real measurements rather than constants: assert the
// SHAPE. The defect was a structural zero, and pinning an exact byte count
// would just make the test a thermometer for the allocator.
static void ck_range(const char *label, long got, long lo, long hi) {
    if (got < lo || got > hi)
        failf(label, (uint64_t) got, (uint64_t) lo, (uint64_t) hi, 0, 0, 0);
    test_logf("  %-56s got=%-8ld want %ld..%ld\n", label, got, lo, hi);
}

#define IN_CHILD(...) do {                                                     \
        fflush(NULL);                                                          \
        pid_t c_ = fork();                                                     \
        if (c_ == 0) {                                                         \
            failures_total = 0;                                                \
            __VA_ARGS__;                                                       \
            fflush(NULL);                                                      \
            _exit(failures_total > 250 ? 250 : (int) failures_total);          \
        }                                                                      \
        int st_;                                                               \
        if (waitpid(c_, &st_, 0) != c_) { failures_total++; break; }           \
        if (WIFSIGNALED(st_)) {                                                \
            printf("FAIL child died on signal %d\n", WTERMSIG(st_));           \
            failures_total++;                                                  \
        } else                                                                 \
            failures_total += (unsigned) WEXITSTATUS(st_);                     \
    } while (0)

static long raw(long nr, long a, long b, long c) {
    errno = 0;
    long r = syscall(nr, a, b, c);
    return r < 0 ? -errno : r;
}

// prlimit64 takes FOUR arguments (pid, resource, new_limit, old_limit), and
// raw() above passes three -- so the fourth slot was whatever happened to be
// in the register or on the stack, and the kernel dutifully faulted on it.
// EFAULT was the correct answer to the call actually being made. Real Linux
// returns EFAULT for it too, so this was never a kernel defect.
static long raw4(long nr, long a, long b, long c, long d) {
    errno = 0;
    long r = syscall(nr, a, b, c, d);
    return r < 0 ? -errno : r;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(120));

    // ---- RLIMIT_FSIZE truncates, it does not refuse ----------------------
    // The shape is the part that is easy to get wrong: a write that would
    // CROSS the limit is short, not failed. Only one starting at or past it
    // fails, with EFBIG and a SIGXFSZ.
    IN_CHILD({
        // Ignore SIGXFSZ BEFORE the limit exists, not after. RLIMIT_FSIZE is
        // per-process and covers every regular file the child writes --
        // including its own stdout, which the suite runner redirects to a log
        // file already far past 64 bytes. So the very next diagnostic line
        // started beyond the limit and the child was killed by SIGXFSZ before
        // it reached the signal() call meant to protect it. Real Linux kills
        // it in exactly the same place. With the disposition set first, that
        // write fails with EFBIG instead, the child survives, and its
        // assertions still run -- IN_CHILD carries the verdict out in the exit
        // status, not in the output.
        signal(SIGXFSZ, SIG_IGN);
        struct rlimit rl = { 64, 64 };
        ck("setrlimit(RLIMIT_FSIZE, 64)", setrlimit(RLIMIT_FSIZE, &rl), 0);
        char path[64];
        snprintf(path, sizeof path, "/tmp/rls-fsize-%d", (int) getpid());
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        ck("  open", fd >= 0, 1);
        if (fd >= 0) {
            char buf[4096];
            memset(buf, 'x', sizeof buf);
            errno = 0;
            ck("  a 4096-byte write is cut to the 64 that fit",
               (long) write(fd, buf, sizeof buf), 64);
            // Now at the limit: the next write has nowhere to go.
            errno = 0;
            ssize_t n = write(fd, buf, sizeof buf);
            ck("  the next write fails", n < 0 ? 1 : 0, 1);
            ck("  ...with EFBIG", n < 0 ? errno : 0, EFBIG);
            close(fd);
        }
        unlink(path);
    });

    // ...and a pipe has no size for a size limit to be about.
    IN_CHILD({
        // Same reason as above: this child never set the disposition at all.
        signal(SIGXFSZ, SIG_IGN);
        struct rlimit rl = { 64, 64 };
        setrlimit(RLIMIT_FSIZE, &rl);
        int pf[2];
        ck("pipe", pipe(pf), 0);
        char buf[512];
        memset(buf, 'x', sizeof buf);
        ck("a pipe write is not limited by RLIMIT_FSIZE",
           (long) write(pf[1], buf, sizeof buf), 512);
        close(pf[0]);
        close(pf[1]);
    });

    // ---- RLIMIT_NPROC ----------------------------------------------------
    if (geteuid() == 0) {
        IN_CHILD({
            ck("drop privilege", setgid(UNPRIV_GID) == 0 && setuid(UNPRIV_UID) == 0, 1);
            struct rlimit rl = { 1, 1 };
            ck("setrlimit(RLIMIT_NPROC, 1)", setrlimit(RLIMIT_NPROC, &rl), 0);
            errno = 0;
            pid_t g = fork();
            if (g == 0)
                _exit(0);
            int e = errno;
            ck("  fork past the limit fails", g < 0 ? 1 : 0, 1);
            ck("  ...with EAGAIN", g < 0 ? e : 0, EAGAIN);
            if (g > 0) {
                int s;
                waitpid(g, &s, 0);
            }
        });
    } else {
        test_logf("  %-56s (needs root to drop to a spare uid)\n", "RLIMIT_NPROC");
    }

    // ---- prlimit64 -------------------------------------------------------
    {
        struct rlimit out;
        ck("prlimit64(0, ...)",
           raw4(SYS_prlimit64, 0, RLIMIT_NOFILE, 0, 0), 0);
        errno = 0;
        ck("prlimit64 on the caller's OWN pid",
           (long) syscall(SYS_prlimit64, (long) getpid(), RLIMIT_NOFILE, NULL, &out) < 0 ? -errno : 0, 0);
        errno = 0;
        long r = syscall(SYS_prlimit64, 0x7ffffffL, RLIMIT_NOFILE, NULL, &out);
        ck("prlimit64 on a pid that does not exist", r < 0 ? errno : 0, ESRCH);
    }

    // ---- nice, in the raw syscall's own 20-nice convention ---------------
    IN_CHILD({
        ck("raw getpriority is 20 at nice 0", raw(SYS_getpriority, PRIO_PROCESS, 0, 0), 20);
        ck("setpriority(5)", raw(SYS_setpriority, PRIO_PROCESS, 0, 5), 0);
        ck("  reads back as 15", raw(SYS_getpriority, PRIO_PROCESS, 0, 0), 15);
        // Out of range is clamped, not refused.
        ck("setpriority(99)", raw(SYS_setpriority, PRIO_PROCESS, 0, 99), 0);
        ck("  clamped to 19, so reads back as 1", raw(SYS_getpriority, PRIO_PROCESS, 0, 0), 1);
        ck("getpriority(which=3) is EINVAL", raw(SYS_getpriority, 3, 0, 0), -EINVAL);
        ck("getpriority(missing pid) is ESRCH", raw(SYS_getpriority, PRIO_PROCESS, 999999, 0), -ESRCH);
        ck("setpriority(which=3) is EINVAL", raw(SYS_setpriority, 3, 0, 0), -EINVAL);
        ck("setpriority(missing pid) is ESRCH", raw(SYS_setpriority, PRIO_PROCESS, 999999, 5), -ESRCH);
        // PRIO_PGRP and PRIO_USER are valid whichs, not just PRIO_PROCESS.
        ck("getpriority(PRIO_PGRP) works", raw(SYS_getpriority, PRIO_PGRP, 0, 0) > 0, 1);
        ck("getpriority(PRIO_USER) works", raw(SYS_getpriority, PRIO_USER, 0, 0) > 0, 1);
    });

    // ---- scheduling policy -----------------------------------------------
    // musl stubs the sched_* wrappers with ENOSYS regardless of the kernel, so
    // going through libc here would measure musl and not this.
    IN_CHILD({
        struct sched_param sp = { 0 };
        ck("set SCHED_BATCH",
           (long) syscall(SYS_sched_setscheduler, 0, SCHED_BATCH, &sp), 0);
        ck("  and getscheduler says so",
           (long) syscall(SYS_sched_getscheduler, 0), SCHED_BATCH);
        ck("set SCHED_IDLE",
           (long) syscall(SYS_sched_setscheduler, 0, SCHED_IDLE, &sp), 0);
        ck("  and getscheduler says so",
           (long) syscall(SYS_sched_getscheduler, 0), SCHED_IDLE);
    });
    IN_CHILD({
        struct sched_param sp = { 0 };
        ck("SCHED_OTHER|RESET_ON_FORK is accepted",
           (long) syscall(SYS_sched_setscheduler, 0, SCHED_OTHER | SCHED_RESET_ON_FORK_, &sp), 0);
        ck("  and the flag comes back in getscheduler",
           (long) syscall(SYS_sched_getscheduler, 0), SCHED_OTHER | SCHED_RESET_ON_FORK_);
    });
    IN_CHILD({
        // A non-realtime policy takes priority 0 and nothing else.
        struct sched_param sp = { .sched_priority = 1 };
        errno = 0;
        long r = syscall(SYS_sched_setscheduler, 0, SCHED_BATCH, &sp);
        ck("SCHED_BATCH with a nonzero priority is EINVAL", r < 0 ? errno : 0, EINVAL);
        struct sched_param zero = { 0 };
        errno = 0;
        r = syscall(SYS_sched_setscheduler, 0, SCHED_FIFO, &zero);
        ck("SCHED_FIFO with priority 0 is EINVAL", r < 0 ? errno : 0, EINVAL);
        errno = 0;
        r = syscall(SYS_sched_setscheduler, 0, 99, &zero);
        ck("an unknown policy is EINVAL", r < 0 ? errno : 0, EINVAL);
    });
    // The priority range is a constant table: what the policy's range IS, not
    // what this caller may set.
    ck("priority_max(SCHED_OTHER)", raw(SYS_sched_get_priority_max, SCHED_OTHER, 0, 0), 0);
    ck("priority_max(SCHED_BATCH)", raw(SYS_sched_get_priority_max, SCHED_BATCH, 0, 0), 0);
    ck("priority_max(SCHED_IDLE)", raw(SYS_sched_get_priority_max, SCHED_IDLE, 0, 0), 0);
    ck("priority_max(SCHED_FIFO)", raw(SYS_sched_get_priority_max, SCHED_FIFO, 0, 0), 99);
    ck("priority_min(SCHED_FIFO)", raw(SYS_sched_get_priority_min, SCHED_FIFO, 0, 0), 1);
    ck("priority_max(99) is EINVAL", raw(SYS_sched_get_priority_max, 99, 0, 0), -EINVAL);

    // ---- getrusage reports more than CPU time ----------------------------
    {
        // Make the numbers mean something: touch 8 MB, move real bytes to a
        // file, and sleep once.
        size_t span = 8u * 1024 * 1024;
        volatile char *p = malloc(span);
        ck("malloc 8 MB", p != NULL, 1);
        if (p != NULL)
            for (size_t i = 0; i < span; i += 4096)
                p[i] = 1;
        char path[64];
        snprintf(path, sizeof path, "/tmp/rls-io-%d", (int) getpid());
        int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            char buf[65536];
            memset(buf, 'x', sizeof buf);
            for (int i = 0; i < 16; i++)
                if (write(fd, buf, sizeof buf) < 0)
                    break;
            close(fd);
            unlink(path);
        }
        usleep(50000);

        struct rusage r;
        memset(&r, 0, sizeof r);
        ck("getrusage(RUSAGE_SELF)", getrusage(RUSAGE_SELF, &r), 0);
        // 8 MB touched, so a peak under 1 MB means it is not being measured at
        // all; the upper bound is loose on purpose.
        ck_range("  ru_maxrss is a real peak, in KB", r.ru_maxrss, 1024, 4L * 1024 * 1024);
        ck("  ru_oublock counted the 1 MB written", r.ru_oublock >= 2048 ? 1 : 0, 1);
        ck("  ru_nvcsw counted the sleep", r.ru_nvcsw > 0 ? 1 : 0, 1);
        // minflt is deliberately NOT range-checked against a Linux-like count:
        // AOK maps most pages eagerly, so it genuinely faults far less. What
        // must not happen is a structural zero after a fork, where breaking
        // copy-on-write pages guarantees some.
        if (p != NULL)
            free((void *) p);
    }
    {
        // A child's peak has to survive its exit -- do_exit releases the
        // address space before it snapshots the final usage, so this read
        // came back 0 even once the live-process path worked.
        fflush(NULL);
        pid_t c = fork();
        if (c == 0) {
            size_t span = 8u * 1024 * 1024;
            volatile char *q = malloc(span);
            if (q != NULL)
                for (size_t i = 0; i < span; i += 4096)
                    q[i] = 2;
            usleep(30000);
            _exit(0);
        }
        int st;
        ck("child ran", waitpid(c, &st, 0) == c, 1);
        struct rusage r;
        memset(&r, 0, sizeof r);
        ck("getrusage(RUSAGE_CHILDREN)", getrusage(RUSAGE_CHILDREN, &r), 0);
        ck_range("  ru_maxrss survives the child's exit", r.ru_maxrss, 1024, 4L * 1024 * 1024);
        ck("  ru_minflt is not a structural zero", r.ru_minflt > 0 ? 1 : 0, 1);
    }

    return finish_suite("resource_limits_sched");
}
