// utimensat's two tv_nsec sentinels, and the validation around them.
//
// UTIME_OMIT means "leave this timestamp alone" and UTIME_NOW means "set it to
// now, ignoring tv_sec". Neither is a nanosecond count, so a kernel that
// passes them through writes the sentinel itself as nanoseconds -- and
// UTIME_OMIT, whose whole job is to preserve a timestamp, instead lands it on
// 1970-01-01 00:00:01. `touch -m`, `cp -p`, tar and rsync all rely on it.
//
// Every expectation was measured against x86_64 glibc on Linux 6.12.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>

#include "test_common.h"

static char path[64];

static void seed(void) {
    struct timespec ts[2] = { { 1000000, 0 }, { 2000000, 0 } };
    if (utimensat(AT_FDCWD, path, ts, 0) < 0)
        failf("seed", (uint64_t) errno, 0, 0, 0, 0, 0);
}

// want < 0 means "must be recent"; otherwise an exact tv_sec.
static void check_times(const char *label, long want_a, long want_m) {
    struct stat st;
    if (stat(path, &st) < 0) {
        failf(label, (uint64_t) errno, 0, 0, 0, 0, 0);
        return;
    }
    time_t now = time(NULL);
    long a = (long) st.st_atim.tv_sec, m = (long) st.st_mtim.tv_sec;
    int ok_a = want_a < 0 ? (a > now - 120 && a <= now + 120) : a == want_a;
    int ok_m = want_m < 0 ? (m > now - 120 && m <= now + 120) : m == want_m;
    if (!ok_a || !ok_m)
        failf(label, (uint64_t) a, (uint64_t) m, 0,
              (uint64_t) want_a, (uint64_t) want_m, 0);
    test_logf("  %-34s atime=%ld mtime=%ld\n", label, a, m);
}

static void check_rc(const char *label, int r, int want_rc, int want_errno) {
    int e = r < 0 ? errno : 0;
    if (r != want_rc || e != want_errno)
        failf(label, (uint64_t) r, (uint64_t) e, 0,
              (uint64_t) want_rc, (uint64_t) want_errno, 0);
    test_logf("  %-34s rc=%d errno=%d\n", label, r, e);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));
    snprintf(path, sizeof path, "/tmp/utimensat_omit.%d", (int) getpid());
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        printf("utimensat_omit: SKIP (cannot create %s)\n", path);
        return 0;
    }
    close(fd);

    struct timespec ts[2];

    seed();
    check_times("seeded", 1000000, 2000000);

    ts[0] = (struct timespec) { 0, UTIME_OMIT };
    ts[1] = (struct timespec) { 3000000, 0 };
    utimensat(AT_FDCWD, path, ts, 0);
    check_times("atime OMIT, mtime set", 1000000, 3000000);

    seed();
    ts[0] = (struct timespec) { 4000000, 0 };
    ts[1] = (struct timespec) { 0, UTIME_OMIT };
    utimensat(AT_FDCWD, path, ts, 0);
    check_times("atime set, mtime OMIT", 4000000, 2000000);

    seed();
    ts[0] = ts[1] = (struct timespec) { 999, UTIME_NOW };   // tv_sec ignored
    utimensat(AT_FDCWD, path, ts, 0);
    check_times("both NOW (tv_sec junk)", -1, -1);

    seed();
    utimensat(AT_FDCWD, path, NULL, 0);
    check_times("times == NULL", -1, -1);

    // Both omitted changes nothing and succeeds.
    seed();
    ts[0] = ts[1] = (struct timespec) { 0, UTIME_OMIT };
    errno = 0;
    check_rc("both OMIT", utimensat(AT_FDCWD, path, ts, 0), 0, 0);
    check_times("both OMIT left it alone", 1000000, 2000000);

    // Validation. tv_nsec must be in range or one of the two sentinels.
    ts[0] = (struct timespec) { 0, 1000000000L };
    ts[1] = (struct timespec) { 0, 0 };
    errno = 0;
    check_rc("tv_nsec == 1e9", utimensat(AT_FDCWD, path, ts, 0), -1, EINVAL);

    ts[0] = (struct timespec) { 0, -1 };
    errno = 0;
    check_rc("tv_nsec == -1", utimensat(AT_FDCWD, path, ts, 0), -1, EINVAL);

    // A sentinel in one slot does not excuse a bad value in the other.
    ts[0] = (struct timespec) { 0, UTIME_OMIT };
    ts[1] = (struct timespec) { 0, 1000000001L };
    errno = 0;
    check_rc("OMIT + bad other slot", utimensat(AT_FDCWD, path, ts, 0), -1, EINVAL);

    ts[0] = ts[1] = (struct timespec) { 0, UTIME_NOW };
    errno = 0;
    check_rc("unknown flag 0x40", utimensat(AT_FDCWD, path, ts, 0x40), -1, EINVAL);

    // Nothing above may have destroyed the file's times as a side effect.
    check_times("times survived the bad calls", 1000000, 2000000);

    unlink(path);
    return finish_suite("utimensat_omit");
}
