// systemd >= 260 boot prerequisites: statx STATX_MNT_ID and
// timerfd TFD_TIMER_CANCEL_ON_SET.
//
// Regression for two bugs that froze Arch Linux ARM's aarch64 systemd as
// PID 1 ("Failed to allocate manager object ... Freezing execution"):
//
// 1. statx never reported a mount-id bit in stx_mask. systemd 260's
//    xstatx_full() (src/basic/stat-util.c) statx()es with XSTATX_MNT_ID_BEST
//    and turns a success whose stx_mask lacks STATX_MNT_ID|STATX_MNT_ID_UNIQUE
//    into -EUNATCH ("Protocol driver not attached"), so every chase()-based
//    open failed: system.conf, os-release, /etc/machine-id, all unit dirs.
//    Now statx reports STATX_MNT_ID with the same 1-based id that
//    /proc/self/mountinfo shows (systemd cross-checks the two).
//
// 2. timerfd_settime rejected any flag besides TFD_TIMER_ABSTIME. sd-event's
//    time-change watcher arms TFD_TIMER_ABSTIME|TFD_TIMER_CANCEL_ON_SET at
//    it_value = TIME_T_MAX and treats EINVAL as fatal ("Failed to create time
//    change event source"), aborting manager allocation. Linux accepts the
//    flag on any timerfd (it only activates cancel-on-clock-set semantics for
//    CLOCK_REALTIME); we accept and ignore it. The TIME_T_MAX deadline also
//    exercised two host-side overflow hazards (timespec_add wrap in
//    timer_set, Darwin nanosleep ns-conversion overflow busy-spin), both now
//    guarded -- the sentinel arm must neither fire nor spin.
//
// Arch-neutral (i386 note: iSH's i386 statx deliberately returns ENOSYS --
// glibc then falls back to fstatat64, which the `working` branch relies on
// for dpkg on APFS-backed roots -- so the statx checks are skipped there and
// only the timerfd half is exercised).
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/syscall.h>
#include "test_common.h"

#ifndef STATX_BASIC_STATS
#define STATX_BASIC_STATS 0x7ffU
#endif
#ifndef STATX_MNT_ID
#define STATX_MNT_ID 0x1000U
#endif
#ifndef AT_EMPTY_PATH
#define AT_EMPTY_PATH 0x1000
#endif
#ifndef TFD_TIMER_CANCEL_ON_SET
#define TFD_TIMER_CANCEL_ON_SET (1 << 1)
#endif

// Local layout so this builds against headers without <linux/stat.h>
// (Alpine minimal images) and without glibc's struct statx.
struct test_statx_ts { int64_t tv_sec; uint32_t tv_nsec; int32_t pad; };
struct test_statx {
    uint32_t stx_mask; uint32_t stx_blksize; uint64_t stx_attributes;
    uint32_t stx_nlink; uint32_t stx_uid; uint32_t stx_gid;
    uint16_t stx_mode; uint16_t pad0;
    uint64_t stx_ino; uint64_t stx_size; uint64_t stx_blocks;
    uint64_t stx_attributes_mask;
    struct test_statx_ts stx_atime, stx_btime, stx_ctime, stx_mtime;
    uint32_t stx_rdev_major, stx_rdev_minor, stx_dev_major, stx_dev_minor;
    uint64_t stx_mnt_id;
    uint32_t stx_dio_mem_align, stx_dio_offset_align;
    uint64_t spare[12];
};

static void check(int cond, const char *what) {
    if (cond) {
        test_logf("ok: %s\n", what);
    } else {
        printf("FAIL: %s (errno=%d %s)\n", what, errno, strerror(errno));
        failures_total++;
    }
}

static int do_statx(int dirfd, const char *path, int flags, unsigned mask,
        struct test_statx *sx) {
    memset(sx, 0, sizeof(*sx));
    return syscall(SYS_statx, dirfd, path, flags, mask, sx);
}

// Any directory on the root filesystem answers what this test asks -- does
// statx report a mount id, and does the AT_EMPTY_PATH form agree. /etc was
// hardcoded, and the tier0 roots are minimal enough not to have one, so this
// reported six statx failures in a root that simply had no /etc. Only on
// x86_64: i386 makes statx ENOSYS on purpose and skips the whole half, which
// is why one arch looked fine and the other did not.
static const char *statx_target_dir(void) {
    static const char *candidates[] = {"/etc", "/tmp", "/bin", "/"};
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        struct stat st;
        if (stat(candidates[i], &st) == 0 && S_ISDIR(st.st_mode))
            return candidates[i];
    }
    return "/";
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(30));

    const char *dir = statx_target_dir();
    test_logf("statx target directory: %s\n", dir);

    struct test_statx sx;
    int statx_enosys = 0;
    int r = do_statx(AT_FDCWD, dir, 0, STATX_BASIC_STATS | STATX_MNT_ID, &sx);
    if (r < 0 && errno == ENOSYS) {
        // i386 guest: statx intentionally ENOSYS (dpkg/APFS fstatat64
        // fallback); skip the statx half entirely.
        statx_enosys = 1;
        test_logf("statx ENOSYS (expected on i386), skipping statx checks\n");
    } else {
        check(r == 0, "statx(dir) succeeds");
        check(sx.stx_mask & STATX_MNT_ID, "stx_mask reports STATX_MNT_ID");
        check(sx.stx_mnt_id != 0, "stx_mnt_id is nonzero");
    }

    if (!statx_enosys) {
        // fd + AT_EMPTY_PATH form (systemd stats fds constantly)
        int dfd = open(dir, O_RDONLY | O_DIRECTORY);
        check(dfd >= 0, "open(dir, O_DIRECTORY)");
        r = do_statx(dfd, "", AT_EMPTY_PATH, STATX_BASIC_STATS | STATX_MNT_ID, &sx);
        check(r == 0, "statx(fd, \"\", AT_EMPTY_PATH) succeeds");
        check((sx.stx_mask & STATX_MNT_ID) && sx.stx_mnt_id != 0,
              "AT_EMPTY_PATH form reports a nonzero mnt_id");
        close(dfd);
    }

    // sd-event timechange sentinel: ABSTIME|CANCEL_ON_SET at TIME_T_MAX must
    // arm successfully, and must neither be immediately readable nor fire
    // within a short window (a wrapped deadline reads as already-expired and
    // fires in a tight loop).
    int tfd = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK | TFD_CLOEXEC);
    check(tfd >= 0, "timerfd_create(CLOCK_REALTIME)");
    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    its.it_value.tv_sec = (time_t) INT64_MAX;
    r = timerfd_settime(tfd, TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET, &its, NULL);
    check(r == 0, "timerfd_settime(ABSTIME|CANCEL_ON_SET, TIME_T_MAX) accepted");
    struct pollfd pfd = { .fd = tfd, .events = POLLIN };
    r = poll(&pfd, 1, 300);
    check(r == 0, "sentinel timer does not fire within 300ms");
    close(tfd);

    // A normal short relative timerfd still fires.
    tfd = timerfd_create(CLOCK_MONOTONIC, 0);
    check(tfd >= 0, "timerfd_create(CLOCK_MONOTONIC)");
    memset(&its, 0, sizeof(its));
    its.it_value.tv_nsec = 50 * 1000 * 1000;
    r = timerfd_settime(tfd, 0, &its, NULL);
    uint64_t exp = 0;
    ssize_t n = read(tfd, &exp, sizeof(exp));
    check(r == 0 && n == (ssize_t) sizeof(exp) && exp == 1,
          "50ms relative timerfd fires");
    close(tfd);

    return finish_suite("statx_mnt_id_timerfd");
}
