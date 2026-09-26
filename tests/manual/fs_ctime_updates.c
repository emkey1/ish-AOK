// Every change to an inode moves its ctime.
//
// Regression for a triage report: chmod and chown did not update ctime. On
// fakefs the mode and owner live in the metadata database, not on the host
// file whose timestamps stat reports, so a chmod changed st_mode and left
// st_ctime where it was. ctime is the one timestamp a program cannot set, and
// the one backup tools (tar --newer-ctime, rsync, restic, find -cnewer) and
// build caches trust to say "this inode changed", precisely because nothing
// can forge it -- a permission change it misses is a permission change a
// backup never records.
//
// Each operation below gets its own fresh file, all made before one second
// boundary and all changed after it, so a filesystem that keeps whole seconds
// still shows the move. Linux (inode_set_ctime_current in notify_change,
// vfs_link, vfs_unlink, vfs_rename) moves ctime for:
//   chmod, fchmod, chown and fchown (here to the owner it already has, which
//   every filesystem allows and still counts as a change), utimes
//   and futimens (to times of the caller's choosing -- ctime is still "now"),
//   truncate and ftruncate, write, link and unlink (the inode, seen through
//   its other name), and rename (the inode renamed).
// A chmod does not move mtime; that is checked too, so a fix that stamps both
// is caught.
//
// Run on the root filesystem (fakefs here), a tmpfs mounted for the purpose
// (as root), and any realfs mount the guest has (/realmnt under
// ISH_REAL_MNT).
//
// Oracle: Linux 6.12 (camd), ext4 and tmpfs, glibc x86_64 and -m32, as root.
#define _GNU_SOURCE
#include <errno.h>
#include <stdbool.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "test_common.h"

#define TEST_NAME "fs_ctime_updates"

static void check(int cond, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (cond) {
        if (test_verbose) {
            printf("ok: ");
            vprintf(fmt, ap);
            printf("\n");
        }
    } else {
        printf("FAIL: ");
        vprintf(fmt, ap);
        printf("\n");
        failures_total++;
    }
    va_end(ap);
}

static int ts_after(struct timespec a, struct timespec b) {
    return a.tv_sec > b.tv_sec || (a.tv_sec == b.tv_sec && a.tv_nsec > b.tv_nsec);
}

static int ts_equal(struct timespec a, struct timespec b) {
    return a.tv_sec == b.tv_sec && a.tv_nsec == b.tv_nsec;
}

enum op {
    OP_CHMOD, OP_FCHMOD, OP_CHOWN, OP_FCHOWN, OP_UTIMES, OP_FUTIMENS, OP_TRUNCATE,
    OP_FTRUNCATE, OP_WRITE, OP_LINK, OP_UNLINK, OP_RENAME, OP_COUNT
};
static const char *const op_name[OP_COUNT] = {
    "chmod", "fchmod", "chown", "fchown", "utimes", "futimens", "truncate",
    "ftruncate", "write", "link", "unlink", "rename",
};

static void run_ops(const char *label, const char *dir) {
    char path[OP_COUNT][512], other[OP_COUNT][512];
    struct stat before[OP_COUNT];
    int ready[OP_COUNT] = {0};

    for (int i = 0; i < OP_COUNT; i++) {
        snprintf(path[i], sizeof(path[i]), "%s/ctime-%s", dir, op_name[i]);
        snprintf(other[i], sizeof(other[i]), "%s/ctime-%s-2", dir, op_name[i]);
        unlink(path[i]);
        unlink(other[i]);
        int fd = open(path[i], O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0) {
            check(0, "%s: create %s (%s)", label, path[i], strerror(errno));
            continue;
        }
        if (write(fd, "abc", 3) != 3)
            check(0, "%s: write %s (%s)", label, path[i], strerror(errno));
        close(fd);
        // unlink's inode is seen through a second name it keeps.
        if (i == OP_UNLINK && link(path[i], other[i]) != 0) {
            check(0, "%s: link for unlink (%s)", label, strerror(errno));
            continue;
        }
        if (stat(path[i], &before[i]) != 0) {
            check(0, "%s: stat %s (%s)", label, path[i], strerror(errno));
            continue;
        }
        ready[i] = 1;
    }

    // Past a whole-second boundary after the latest of those ctimes.
    time_t latest = 0;
    for (int i = 0; i < OP_COUNT; i++)
        if (ready[i] && before[i].st_ctim.tv_sec > latest)
            latest = before[i].st_ctim.tv_sec;
    struct timespec now;
    do {
        usleep(20000);
        clock_gettime(CLOCK_REALTIME, &now);
    } while (now.tv_sec <= latest);
    usleep(20000);

    for (int i = 0; i < OP_COUNT; i++) {
        if (!ready[i])
            continue;
        int rc = 0, fd = -1;
        const char *after_path = path[i];
        switch ((enum op) i) {
        case OP_CHMOD:
            rc = chmod(path[i], 0600);
            break;
        case OP_FCHMOD:
            fd = open(path[i], O_RDONLY | O_CLOEXEC);
            rc = fd < 0 ? -1 : fchmod(fd, 0600);
            break;
        case OP_CHOWN:
            rc = chown(path[i], before[i].st_uid, before[i].st_gid);
            break;
        case OP_FCHOWN:
            fd = open(path[i], O_RDONLY | O_CLOEXEC);
            rc = fd < 0 ? -1 : fchown(fd, before[i].st_uid, before[i].st_gid);
            break;
        case OP_UTIMES: {
            struct timeval tv[2] = {{1000000000, 0}, {1000000000, 0}};
            rc = utimes(path[i], tv);
            break;
        }
        case OP_FUTIMENS: {
            struct timespec ts[2] = {{1000000000, 0}, {1000000000, 0}};
            fd = open(path[i], O_WRONLY | O_CLOEXEC);
            rc = fd < 0 ? -1 : futimens(fd, ts);
            break;
        }
        case OP_TRUNCATE:
            rc = truncate(path[i], 1);
            break;
        case OP_FTRUNCATE:
            fd = open(path[i], O_WRONLY | O_CLOEXEC);
            rc = fd < 0 ? -1 : ftruncate(fd, 1);
            break;
        case OP_WRITE:
            fd = open(path[i], O_WRONLY | O_CLOEXEC);
            rc = fd < 0 ? -1 : (write(fd, "x", 1) == 1 ? 0 : -1);
            break;
        case OP_LINK:
            rc = link(path[i], other[i]);
            break;
        case OP_UNLINK:
            rc = unlink(other[i]);
            break;
        case OP_RENAME:
            rc = rename(path[i], other[i]);
            after_path = other[i];
            break;
        case OP_COUNT:
            break;
        }
        int err = errno;
        if (fd >= 0)
            close(fd);
        check(rc == 0, "%s: %s (%s)", label, op_name[i], rc == 0 ? "done" : strerror(err));
        if (rc != 0)
            continue;
        struct stat after;
        if (stat(after_path, &after) != 0) {
            check(0, "%s: stat after %s (%s)", label, op_name[i], strerror(errno));
            continue;
        }
        check(ts_after(after.st_ctim, before[i].st_ctim),
              "%s: %s moves ctime (%lld.%09ld -> %lld.%09ld)", label, op_name[i],
              (long long) before[i].st_ctim.tv_sec, before[i].st_ctim.tv_nsec,
              (long long) after.st_ctim.tv_sec, after.st_ctim.tv_nsec);
        if (i == OP_CHMOD || i == OP_CHOWN || i == OP_LINK || i == OP_RENAME)
            check(ts_equal(after.st_mtim, before[i].st_mtim),
                  "%s: %s leaves mtime alone (%lld.%09ld -> %lld.%09ld)", label, op_name[i],
                  (long long) before[i].st_mtim.tv_sec, before[i].st_mtim.tv_nsec,
                  (long long) after.st_mtim.tv_sec, after.st_mtim.tv_nsec);
        if (i == OP_UTIMES || i == OP_FUTIMENS)
            check(after.st_mtim.tv_sec == 1000000000,
                  "%s: %s set mtime to the time asked for (got %lld)", label, op_name[i],
                  (long long) after.st_mtim.tv_sec);
    }

    for (int i = 0; i < OP_COUNT; i++) {
        unlink(path[i]);
        unlink(other[i]);
    }
}

// A realfs mount the guest already has, if any: the first "real"/"realfs"
// entry in /proc/self/mounts that is writable.
static int find_realfs(char *out, size_t n) {
    FILE *f = fopen("/proc/self/mounts", "r");
    if (f == NULL)
        return 0;
    char src[512], point[512], type[64], opts[512];
    int found = 0;
    while (!found && fscanf(f, "%511s %511s %63s %511s %*d %*d", src, point, type, opts) == 4) {
        if ((strcmp(type, "real") == 0 || strcmp(type, "realfs") == 0) &&
                strncmp(opts, "rw", 2) == 0 && access(point, W_OK) == 0) {
            snprintf(out, n, "%s", point);
            found = 1;
        }
    }
    fclose(f);
    return found;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));

    char root_dir[] = "/tmp/ctime.XXXXXX";
    if (mkdtemp(root_dir) == NULL) {
        printf("FAIL: mkdtemp (%s)\n", strerror(errno));
        return 1;
    }
    run_ops("root fs", root_dir);

    char tmpfs_dir[sizeof(root_dir) + 8];
    snprintf(tmpfs_dir, sizeof(tmpfs_dir), "%s/tmpfs", root_dir);
    if (mkdir(tmpfs_dir, 0755) == 0) {
        if (mount("tmpfs", tmpfs_dir, "tmpfs", 0, "size=1m") == 0) {
            run_ops("tmpfs", tmpfs_dir);
            check(umount(tmpfs_dir) == 0, "umount tmpfs (%s)", strerror(errno));
        } else {
            check(errno == EPERM, "mount tmpfs (%s)", strerror(errno));
            test_logf("mount tmpfs: %s, tmpfs checks skipped\n", strerror(errno));
        }
        rmdir(tmpfs_dir);
    }

    char real[512];
    if (find_realfs(real, sizeof(real))) {
        char real_dir[600];
        snprintf(real_dir, sizeof(real_dir), "%s/ctime.XXXXXX", real);
        bool made = mkdtemp(real_dir) != NULL;
        if (made && access(real_dir, W_OK) != 0) {
            // A realfs mount that is not shared (a dev /realmnt) reports the
            // host's owner for every file, so a caller other than root is
            // "other" even in a directory it just made, and cannot create
            // anything in it. A shared one (/AOK/persist) reports the caller
            // (realfs_shared_owner). The suite re-runs this test as root
            // (needs_root_tests), which covers realfs either way.
            test_logf("realfs %s: not writable by uid %d, realfs checks skipped\n",
                      real_dir, (int) geteuid());
            rmdir(real_dir);
        } else if (made) {
            run_ops("realfs", real_dir);
            rmdir(real_dir);
        } else {
            check(0, "mkdtemp on realfs %s (%s)", real, strerror(errno));
        }
    } else {
        test_logf("no writable realfs mount, realfs checks skipped\n");
    }

    rmdir(root_dir);
    return finish_suite(TEST_NAME);
}
