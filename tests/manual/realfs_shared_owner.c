// The owner a guest user sees on a shared realfs mount (/AOK/persist,
// /AOK/roots), and what it may do there.
//
// Every file on such a mount belongs, at the host, to the app's own uid,
// whichever guest user made it, and the host can record no other owner.
// realfs reported that uid (501 on a device), so every guest user but root was
// "other" to every file: uid 1000 could not even use a directory it had just
// made (`d=$(mktemp -d /AOK/persist/x.XXXXXX); touch $d/f` was EACCES, found by
// the 556 device leg). New files were meant to come out world-writable, but the
// host's own umask took those bits off again, and the app widened everything to
// 0666/0777 on each launch instead -- undoing any chmod, so an ssh key kept
// there could never be 0600.
//
// Now a file the app owns is reported as owned by whoever asks, as macOS shows
// a volume whose ownership is ignored: each guest user has an owner's access to
// the shared area, which is what the area is for, and a chmod stays as made.
// A chown to that owner changes nothing and succeeds; to anyone else it is
// EPERM, because the host cannot record it.
//
// No Linux oracle: no Linux filesystem reports its caller as the owner (vfat
// reports the fixed uid= of its mount). Needs a shared realfs mount -- the
// app's /AOK/persist, or one named by REALFS_SHARED, such as /realmnt from
// `ISH_REAL_MNT=<dir> ISH_REAL_MNT_SHARED=1 ish ...` -- and SKIPs without
// one. As root it runs the checks as root and again as uid 65534.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "test_common.h"

static void check(int cond, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (!cond) {
        printf("FAIL ");
        vprintf(fmt, ap);
        printf("\n");
        failures_total++;
    } else if (test_verbose) {
        printf("ok ");
        vprintf(fmt, ap);
        printf("\n");
    }
    va_end(ap);
}

// A realfs mount point from /proc/mounts, in order of preference.
static int is_realfs_mount(const char *point) {
    FILE *f = fopen("/proc/mounts", "r");
    if (f == NULL)
        return 0;
    char src[PATH_MAX], dir[PATH_MAX], type[64];
    int found = 0;
    while (fscanf(f, "%4095s %4095s %63s %*[^\n]", src, dir, type) == 3) {
        if (strcmp(dir, point) == 0 && (strcmp(type, "real") == 0 || strcmp(type, "realfs") == 0))
            found = 1;
    }
    fclose(f);
    return found;
}

static void checks(const char *mnt, const char *who) {
    char dir[PATH_MAX / 2], file[PATH_MAX], other[PATH_MAX];
    struct stat st;

    // mktemp -d: a 0700 directory, then something in it.
    snprintf(dir, sizeof(dir), "%s/rso.XXXXXX", mnt);
    check(mkdtemp(dir) != NULL, "%s: mkdtemp in %s (%s)", who, mnt, strerror(errno));
    snprintf(file, sizeof(file), "%s/f", dir);
    int fd = open(file, O_CREAT | O_WRONLY | O_CLOEXEC, 0644);
    check(fd >= 0, "%s: create a file in the directory it just made (%s)", who, strerror(errno));
    if (fd >= 0) {
        check(write(fd, "x", 1) == 1, "%s: write it (%s)", who, strerror(errno));
        close(fd);
    }

    check(stat(dir, &st) == 0 && st.st_uid == geteuid() && st.st_gid == getegid(),
          "%s: the directory is its own, %d:%d (got %d:%d)", who, (int) geteuid(), (int) getegid(),
          (int) st.st_uid, (int) st.st_gid);
    check(stat(file, &st) == 0 && st.st_uid == geteuid() && st.st_gid == getegid(),
          "%s: the file is its own, %d:%d (got %d:%d)", who, (int) geteuid(), (int) getegid(),
          (int) st.st_uid, (int) st.st_gid);

    // A chmod is the owner's to make, and it stays as made.
    check(chmod(file, 0600) == 0, "%s: chmod 0600 (%s)", who, strerror(errno));
    check(stat(file, &st) == 0 && (st.st_mode & 07777) == 0600, "%s: the mode is 0600 (got %o)",
          who, (unsigned) (st.st_mode & 07777));

    // chown to the owner it has: nothing to change. To anyone else: EPERM.
    check(chown(file, geteuid(), getegid()) == 0, "%s: chown to itself (%s)", who, strerror(errno));
    uid_t them = geteuid() == 4242 ? 4243 : 4242;
    errno = 0;
    check(chown(file, them, (gid_t) -1) < 0 && errno == EPERM, "%s: chown to uid %d is EPERM (%s)",
          who, (int) them, strerror(errno));

    // Something made by someone else is as much this user's as its own.
    snprintf(other, sizeof(other), "%s/by-root", mnt);
    if (geteuid() != 0 && access(other, F_OK) == 0) {
        fd = open(other, O_WRONLY | O_APPEND | O_CLOEXEC);
        check(fd >= 0, "%s: open root's 0600 file for writing (%s)", who, strerror(errno));
        if (fd >= 0)
            close(fd);
    }

    unlink(file);
    check(rmdir(dir) == 0, "%s: remove the directory (%s)", who, strerror(errno));
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(30));
    // Named, not detected: nothing a guest can read says a mount is shared
    // but the very owner this test is about. /AOK/persist always is.
    const char *mnt = getenv("REALFS_SHARED");
    if (mnt == NULL || mnt[0] == '\0')
        mnt = "/AOK/persist";
    if (strlen(mnt) > 64 || !is_realfs_mount(mnt)) {
        printf("realfs_shared_owner: SKIP (no shared realfs mount: /AOK/persist, or REALFS_SHARED "
               "naming one, e.g. /realmnt with ISH_REAL_MNT_SHARED=1)\n");
        return 0;
    }
    test_logf("shared realfs mount: %s\n", mnt);

    checks(mnt, geteuid() == 0 ? "root" : "user");
    if (geteuid() == 0) {
        char other[PATH_MAX];
        snprintf(other, sizeof(other), "%s/by-root", mnt);
        int fd = open(other, O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0600);
        check(fd >= 0, "root: create %s 0600 (%s)", other, strerror(errno));
        if (fd >= 0)
            close(fd);
        fflush(stdout);
        pid_t pid = fork();
        if (pid == 0) {
            if (setgid(65534) != 0 || setuid(65534) != 0) {
                printf("FAIL cannot drop to uid 65534 (%s)\n", strerror(errno));
                _exit(1);
            }
            checks(mnt, "uid 65534");
            fflush(stdout);
            _exit(failures_total != 0);
        }
        int status = 0;
        check(pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
                  WEXITSTATUS(status) == 0,
              "uid 65534's checks pass (status %#x)", status);
        unlink(other);
    }
    return finish_suite("realfs_shared_owner");
}
