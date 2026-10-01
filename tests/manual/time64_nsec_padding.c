// A 32-bit guest's 64-bit timespec has a padding word the kernel must ignore.
//
// On i386 the *_time64 system calls take struct timespec as {s64 tv_sec;
// s32 tv_nsec; 4 bytes of padding}. musl and glibc never fill the padding, so
// it holds whatever the stack held, and Linux reads only the low 32 bits of
// tv_nsec for a 32-bit caller (get_timespec64 under in_compat_syscall()). AOK
// read all 64 bits: a deadline whose padding was not zero had tv_nsec >= 1e9
// and came back EINVAL. futex_timeout_duration's FUTEX_WAIT_BITSET cases failed
// on every i386 device leg since 555 that way -- on the Mac CLI the stack word
// happened to be zero.
//
// Every call here gets padding 0xdeadbeef and must behave as if it were zero:
// the timed waits wait (and are not EINVAL), the timer fires, and utimensat
// stores the nanoseconds exactly. 64-bit guests have no padding: SKIP.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <linux/futex.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include "test_common.h"

#if defined(__i386__)

#define NR_clock_nanosleep_time64 407
#define NR_timerfd_settime64      411
#define NR_utimensat_time64       412
#define NR_pselect6_time64        413
#define NR_ppoll_time64           414
#define NR_rt_sigtimedwait_time64 421
#define NR_futex_time64           422
#define NR_epoll_pwait2           441

struct pts { int64_t sec; uint32_t nsec; uint32_t pad; };

static int fails;
static struct pts padded(int64_t sec, uint32_t nsec) {
    return (struct pts) {sec, nsec, 0xdeadbeef};
}
static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}
// A timed call that should wait about `ms`: not EINVAL, and at least 3/4 of it.
static void timed(const char *what, long rc, int err, double elapsed, double ms, long want_rc, int want_errno) {
    bool ok = rc == want_rc && (want_rc != -1 || err == want_errno) && elapsed >= ms * 0.75;
    if (ok) {
        test_logf("ok: %s (%.0f ms)\n", what, elapsed);
    } else {
        printf("FAIL: %s: rc=%ld errno=%d (%s) after %.0f ms; wanted rc=%ld%s after ~%.0f ms\n", what, rc, err,
               strerror(err), elapsed, want_rc, want_errno ? " ETIMEDOUT/EAGAIN" : "", ms);
        fails++;
    }
}

static int word;

int main(int argc, char **argv) {
    test_init(argc, argv);
    double t0;
    long rc;
    struct pts ts;

    // FUTEX_WAIT_BITSET, absolute monotonic deadline 200 ms out.
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int64_t ns = now.tv_nsec + 200000000;
    ts = padded(now.tv_sec + ns / 1000000000, (uint32_t) (ns % 1000000000));
    t0 = now_ms();
    rc = syscall(NR_futex_time64, &word, FUTEX_WAIT_BITSET | FUTEX_PRIVATE_FLAG, 0, &ts, NULL, FUTEX_BITSET_MATCH_ANY);
    timed("futex_time64 FUTEX_WAIT_BITSET deadline", rc, errno, now_ms() - t0, 200, -1, ETIMEDOUT);

    ts = padded(0, 200000000);
    t0 = now_ms();
    rc = syscall(NR_futex_time64, &word, FUTEX_WAIT | FUTEX_PRIVATE_FLAG, 0, &ts, NULL, 0);
    timed("futex_time64 FUTEX_WAIT relative", rc, errno, now_ms() - t0, 200, -1, ETIMEDOUT);

    ts = padded(0, 150000000);
    t0 = now_ms();
    rc = syscall(NR_clock_nanosleep_time64, CLOCK_MONOTONIC, 0, &ts, NULL);
    timed("clock_nanosleep_time64", rc, errno, now_ms() - t0, 150, 0, 0);

    ts = padded(0, 150000000);
    t0 = now_ms();
    rc = syscall(NR_ppoll_time64, NULL, 0, &ts, NULL, 8);
    timed("ppoll_time64", rc, errno, now_ms() - t0, 150, 0, 0);

    ts = padded(0, 150000000);
    t0 = now_ms();
    rc = syscall(NR_pselect6_time64, 0, NULL, NULL, NULL, &ts, NULL);
    timed("pselect6_time64", rc, errno, now_ms() - t0, 150, 0, 0);

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR2);
    sigprocmask(SIG_BLOCK, &set, NULL);
    ts = padded(0, 150000000);
    t0 = now_ms();
    rc = syscall(NR_rt_sigtimedwait_time64, &set, NULL, &ts, 8);
    timed("rt_sigtimedwait_time64", rc, errno, now_ms() - t0, 150, -1, EAGAIN);

    int ep = epoll_create1(0);
    struct epoll_event ev;
    ts = padded(0, 150000000);
    t0 = now_ms();
    rc = syscall(NR_epoll_pwait2, ep, &ev, 1, &ts, NULL, 8);
    timed("epoll_pwait2", rc, errno, now_ms() - t0, 150, 0, 0);
    close(ep);

    // timerfd_settime64: it_value 100 ms, both halves padded.
    int tfd = syscall(SYS_timerfd_create, CLOCK_MONOTONIC, 0);
    struct { struct pts interval, value; } its = {padded(0, 0), padded(0, 100000000)};
    t0 = now_ms();
    rc = syscall(NR_timerfd_settime64, tfd, 0, &its, NULL);
    uint64_t expirations = 0;
    if (rc == 0 && read(tfd, &expirations, sizeof expirations) != sizeof expirations)
        expirations = 0;
    timed("timerfd_settime64 fires", rc == 0 && expirations == 1 ? 0 : -1, rc == 0 ? 0 : errno,
          now_ms() - t0, 100, 0, 0);
    close(tfd);

    // utimensat_time64 stores the nanoseconds exactly.
    char path[] = "/tmp/t64padXXXXXX";
    int fd = mkstemp(path);
    struct pts times[2] = {padded(1000000000, 123456789), padded(1000000001, 987654321)};
    rc = syscall(NR_utimensat_time64, AT_FDCWD, path, times, 0);
    struct stat st;
    fstat(fd, &st);
    if (rc == 0 && st.st_atim.tv_nsec == 123456789 && st.st_mtim.tv_nsec == 987654321) {
        test_logf("ok: utimensat_time64\n");
    } else {
        printf("FAIL: utimensat_time64: rc=%ld errno=%d atime nsec %ld mtime nsec %ld\n", rc, errno,
               (long) st.st_atim.tv_nsec, (long) st.st_mtim.tv_nsec);
        fails++;
    }
    close(fd);
    unlink(path);

    printf("time64_nsec_padding: %s\n", fails ? "FAIL" : "PASS");
    return fails != 0;
}

#else
int main(void) {
    printf("time64_nsec_padding: SKIP (32-bit x86 only)\n");
    return 0;
}
#endif
