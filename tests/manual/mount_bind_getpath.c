// A path read back from a descriptor opened through a bind mount names the
// bind, not the directory the bind shows.
//
// Regression for a triage report. After `mount --bind /tmp/bp-src /tmp/bp-dst;
// cd /tmp/bp-dst`, Linux answers /tmp/bp-dst to readlink /proc/self/cwd, to
// getcwd() and so to `pwd -P`, and /tmp/bp-dst/f for /proc/self/fd/N of a file
// opened there. AOK answered /tmp/bp-src and /tmp/bp-src/f. A bind resolves to
// its origin for storage (fs/generic.c find_mount_and_trim_path_seen), so the
// descriptor's mount is the origin, and generic_getpath built the path from the
// origin's point.
//
// The descriptor's path is also where every RELATIVE lookup from it starts
// (fs/path.c path_normalize), so the wrong answer was not only cosmetic:
//   - a read-only bind was writable through relative names from a cwd or a
//     dirfd inside it, because the lookup landed on the source's mount;
//   - a mount made on a directory inside the bind was invisible to relative
//     names, which reached the directory underneath it;
//   - a bind of a directory outside a chroot put a cwd inside it outside the
//     chroot, where getcwd() is ENOENT and `..` walked out of the jail.
//
// As root: binds a scratch source onto a scratch target, and checks getcwd,
// /proc/self/cwd and /proc/self/fd/N for a directory, a file, an O_PATH
// symlink, an O_PATH FIFO and a /proc/self/fd reopen through the bind, a bind
// of the bind, and each of the three relative-lookup consequences above; plus
// the two other readers of the path, inotify's IN_MODIFY/IN_CLOSE_WRITE and
// /proc/self/maps.
// Unprivileged, the mount is EPERM and the test only checks nothing broke.
//
// Oracle: Linux 6.12 (camd), glibc x86_64 and -m32, as root and not.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/wait.h>
#include <unistd.h>
#include "test_common.h"

#define TEST_NAME "mount_bind_getpath"

#ifndef O_PATH
#define O_PATH 010000000
#endif
#ifndef MNT_DETACH
#define MNT_DETACH 2
#endif
#ifndef MS_PRIVATE
#define MS_PRIVATE (1 << 18)
#endif
#define TMPFS_MAGIC_ 0x01021994

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

static char base[] = "/tmp/mbgp.XXXXXX";

static void join(char *out, size_t n, const char *dir, const char *name) {
    snprintf(out, n, "%s/%s", dir, name);
}

static void check_cwd(const char *want, const char *how) {
    char got[PATH_MAX];
    if (getcwd(got, sizeof(got)) == NULL)
        snprintf(got, sizeof(got), "<getcwd: %s>", strerror(errno));
    check(strcmp(got, want) == 0, "%s: getcwd is %s (got %s)", how, want, got);

    ssize_t n = readlink("/proc/self/cwd", got, sizeof(got) - 1);
    if (n < 0)
        snprintf(got, sizeof(got), "<readlink: %s>", strerror(errno));
    else
        got[n] = '\0';
    check(strcmp(got, want) == 0, "%s: /proc/self/cwd is %s (got %s)", how, want, got);
}

static void check_fd(int fd, const char *want, const char *how) {
    char link[64], got[PATH_MAX];
    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    ssize_t n = readlink(link, got, sizeof(got) - 1);
    if (n < 0)
        snprintf(got, sizeof(got), "<readlink: %s>", strerror(errno));
    else
        got[n] = '\0';
    check(strcmp(got, want) == 0, "%s: /proc/self/fd/N is %s (got %s)", how, want, got);
}

static void check_open(const char *path, int flags, const char *want, const char *how) {
    int fd = open(path, flags | O_CLOEXEC);
    check(fd >= 0, "%s: open %s (%s)", how, path, strerror(errno));
    if (fd < 0)
        return;
    check_fd(fd, want, how);
    close(fd);
}

// The chroot half runs in a child: a chroot is not undone.
static void jail_child(const char *jail) {
    if (chroot(jail) != 0) {
        check(0, "chroot %s (%s)", jail, strerror(errno));
        return;
    }
    check(chdir("/mnt") == 0, "chroot: chdir /mnt (%s)", strerror(errno));
    char got[PATH_MAX];
    if (getcwd(got, sizeof(got)) == NULL)
        snprintf(got, sizeof(got), "<getcwd: %s>", strerror(errno));
    check(strcmp(got, "/mnt") == 0,
          "chroot: getcwd in a bind of a directory outside the jail is /mnt (got %s)", got);
    check(access("outside-marker", F_OK) == 0,
          "chroot: a relative name reaches the bind's contents (%s)", strerror(errno));
    check(chdir("..") == 0, "chroot: chdir .. (%s)", strerror(errno));
    if (getcwd(got, sizeof(got)) == NULL)
        snprintf(got, sizeof(got), "<getcwd: %s>", strerror(errno));
    check(strcmp(got, "/") == 0, "chroot: .. from the bind is the jail's root (got %s)", got);
    check(access("jail-marker", F_OK) == 0,
          "chroot: .. from the bind stays in the jail, where jail-marker is (%s)",
          strerror(errno));
    check(chdir("../../../..") == 0 && access("jail-marker", F_OK) == 0,
          "chroot: no number of .. leaves the jail (%s)", strerror(errno));
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));

    if (mkdtemp(base) == NULL) {
        printf("FAIL: mkdtemp (%s)\n", strerror(errno));
        return 1;
    }
    char src[64], dst[64], dst2[64], ro[64], jail[64], jmnt[80], outside[64], tmp[128];
    char src_sub[80], dst_sub[80], src_f[80], dst_f[80], dst_lnk[80], dst_fifo[80];
    join(src, sizeof(src), base, "src");
    join(dst, sizeof(dst), base, "dst");
    join(dst2, sizeof(dst2), base, "dst2");
    join(ro, sizeof(ro), base, "ro");
    join(jail, sizeof(jail), base, "jail");
    join(jmnt, sizeof(jmnt), jail, "mnt");
    join(outside, sizeof(outside), base, "outside");
    join(src_sub, sizeof(src_sub), src, "sub");
    join(dst_sub, sizeof(dst_sub), dst, "sub");
    join(src_f, sizeof(src_f), src, "f");
    join(dst_f, sizeof(dst_f), dst, "f");
    join(dst_lnk, sizeof(dst_lnk), dst, "lnk");
    join(dst_fifo, sizeof(dst_fifo), dst, "fifo");

    const char *dirs[] = {src, dst, dst2, ro, jail, jmnt, outside, src_sub};
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
        check(mkdir(dirs[i], 0755) == 0, "mkdir %s (%s)", dirs[i], strerror(errno));
    int ffd = open(src_f, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    check(ffd >= 0, "create %s (%s)", src_f, strerror(errno));
    if (ffd >= 0)
        close(ffd);
    join(tmp, sizeof(tmp), src, "lnk");
    check(symlink("f", tmp) == 0, "symlink %s (%s)", tmp, strerror(errno));
    join(tmp, sizeof(tmp), src, "fifo");
    check(mkfifo(tmp, 0644) == 0, "mkfifo %s (%s)", tmp, strerror(errno));
    join(tmp, sizeof(tmp), jail, "jail-marker");
    ffd = open(tmp, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (ffd >= 0)
        close(ffd);
    join(tmp, sizeof(tmp), outside, "outside-marker");
    ffd = open(tmp, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (ffd >= 0)
        close(ffd);

    if (mount(src, dst, NULL, MS_BIND, NULL) != 0) {
        int err = errno;
        check(err == EPERM, "mount --bind %s %s (%s)", src, dst, strerror(err));
        test_logf("mount: %s, not privileged: bind checks skipped\n", strerror(err));
        // Nothing through a bind, but the plain answers must still hold.
        check(chdir(src) == 0, "chdir %s (%s)", src, strerror(errno));
        check_cwd(src, "unprivileged, the source");
        check_open(src_f, O_RDONLY, src_f, "unprivileged, the source's file");
        check(chdir("/") == 0, "chdir / (%s)", strerror(errno));
        goto cleanup;
    }
    // Private, so the tmpfs mounted inside the bind below does not propagate
    // back onto the source on a Linux whose / is shared (systemd's default).
    check(mount(NULL, dst, NULL, MS_PRIVATE, NULL) == 0, "mount --make-private %s (%s)", dst,
          strerror(errno));

    // The report: cwd in the bind.
    check(chdir(dst) == 0, "chdir %s (%s)", dst, strerror(errno));
    check_cwd(dst, "cwd at the bind's root");
    check(chdir("sub") == 0, "chdir sub (%s)", strerror(errno));
    check_cwd(dst_sub, "cwd below the bind, reached relatively");
    check(chdir("..") == 0, "chdir .. (%s)", strerror(errno));
    check_cwd(dst, ".. from below the bind");
    check(chdir("..") == 0, "chdir .. (%s)", strerror(errno));
    check_cwd(base, ".. from the bind's root");

    // Descriptors opened through the bind, one of each kind generic_openat
    // makes: a real open, the O_PATH pseudo-descriptor for a symlink, and the
    // one for a FIFO. The source's own names are unchanged.
    check_open(dst, O_RDONLY | O_DIRECTORY, dst, "the bind's root");
    check_open(dst_f, O_RDONLY, dst_f, "a file through the bind");
    check_open(dst_lnk, O_PATH | O_NOFOLLOW, dst_lnk, "an O_PATH symlink through the bind");
    check_open(dst_fifo, O_PATH, dst_fifo, "an O_PATH FIFO through the bind");
    check_open(src_f, O_RDONLY, src_f, "the same file through the source");
    check_open(src, O_RDONLY | O_DIRECTORY, src, "the source directory");

    // A dirfd in the bind: relative opens from it stay on the bind.
    int dfd = open(dst_sub, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    check(dfd >= 0, "open %s (%s)", dst_sub, strerror(errno));
    if (dfd >= 0) {
        int fd = openat(dfd, "../f", O_RDONLY | O_CLOEXEC);
        check(fd >= 0, "openat(sub, ../f) (%s)", strerror(errno));
        if (fd >= 0) {
            check_fd(fd, dst_f, "openat(dirfd below the bind, \"../f\")");
            // Reopening the magic link opens the same thing again.
            char link[64];
            snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
            check_open(link, O_RDONLY, dst_f, "a /proc/self/fd reopen of a file through the bind");
            close(fd);
        }
        close(dfd);
    }

    // The same path is where a write through the bind is reported: IN_MODIFY
    // and IN_CLOSE_WRITE are named from the descriptor, while IN_OPEN came
    // from the name it was opened by, so a watch on the bind saw the open and
    // never the write.
    int in = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    int wd = in >= 0 ? inotify_add_watch(in, dst, IN_MODIFY | IN_CLOSE_WRITE) : -1;
    check(wd >= 0, "inotify watch on %s (%s)", dst, strerror(errno));
    if (wd >= 0) {
        int fd = open(dst_f, O_WRONLY | O_CLOEXEC);
        check(fd >= 0 && write(fd, "x", 1) == 1, "write %s (%s)", dst_f, strerror(errno));
        if (fd >= 0)
            close(fd);
        char evbuf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
        ssize_t n = read(in, evbuf, sizeof(evbuf));
        unsigned seen = 0;
        for (ssize_t off = 0; n > 0 && off < n; ) {
            struct inotify_event *ev = (struct inotify_event *) (evbuf + off);
            if (ev->wd == wd && ev->len > 0 && strcmp(ev->name, "f") == 0)
                seen |= ev->mask;
            off += sizeof(*ev) + ev->len;
        }
        check(seen & IN_MODIFY, "a watch on the bind sees IN_MODIFY for a write through it");
        check(seen & IN_CLOSE_WRITE,
              "a watch on the bind sees IN_CLOSE_WRITE for a write through it");
    }
    if (in >= 0)
        close(in);

    // And a mapping of a file opened through the bind is listed by its path.
    int mfd = open(dst_f, O_RDONLY | O_CLOEXEC);
    void *map = mfd >= 0 ? mmap(NULL, 1, PROT_READ, MAP_SHARED, mfd, 0) : MAP_FAILED;
    check(map != MAP_FAILED, "mmap %s (%s)", dst_f, strerror(errno));
    if (map != MAP_FAILED) {
        FILE *maps = fopen("/proc/self/maps", "r");
        char line[1024];
        int named = 0;
        while (maps != NULL && fgets(line, sizeof(line), maps) != NULL) {
            line[strcspn(line, "\n")] = '\0';
            size_t len = strlen(line), want = strlen(dst_f);
            if (len >= want && strcmp(line + len - want, dst_f) == 0)
                named = 1;
        }
        if (maps != NULL)
            fclose(maps);
        check(named, "/proc/self/maps names the mapping %s", dst_f);
        munmap(map, 1);
    }
    if (mfd >= 0)
        close(mfd);

    // A bind of the bind is a mount of its own again.
    if (mount(dst, dst2, NULL, MS_BIND, NULL) == 0) {
        check(chdir(dst2) == 0, "chdir %s (%s)", dst2, strerror(errno));
        char want[128];
        check_cwd(dst2, "cwd at a bind of a bind");
        join(want, sizeof(want), dst2, "f");
        check_open("f", O_RDONLY, want, "a file through a bind of a bind");
        check(chdir("/") == 0, "chdir / (%s)", strerror(errno));
        check(umount2(dst2, 0) == 0, "umount %s (%s)", dst2, strerror(errno));
    } else {
        check(0, "mount --bind %s %s (%s)", dst, dst2, strerror(errno));
    }

    // A read-only bind: a relative name from a cwd or a dirfd in it is on the
    // bind, so it is read-only too, whatever the source's mount says.
    if (mount(src, ro, NULL, MS_BIND, NULL) == 0) {
        check(mount(NULL, ro, NULL, MS_BIND | MS_REMOUNT | MS_RDONLY, NULL) == 0,
              "mount -o remount,bind,ro %s (%s)", ro, strerror(errno));
        check(chdir(ro) == 0, "chdir %s (%s)", ro, strerror(errno));
        int fd = open("new", O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
        int err = errno;
        check(fd < 0 && err == EROFS,
              "open(\"new\", O_CREAT) from a cwd in a read-only bind is EROFS (got %s)",
              fd >= 0 ? "success" : strerror(err));
        if (fd >= 0)
            close(fd);
        check(mkdir("newdir", 0755) != 0 && errno == EROFS,
              "mkdir(\"newdir\") from a cwd in a read-only bind is EROFS (%s)", strerror(errno));
        check(chdir("/") == 0, "chdir / (%s)", strerror(errno));
        int rfd = open(ro, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (rfd >= 0) {
            fd = openat(rfd, "new2", O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
            err = errno;
            check(fd < 0 && err == EROFS,
                  "openat(dirfd of a read-only bind, \"new2\", O_CREAT) is EROFS (got %s)",
                  fd >= 0 ? "success" : strerror(err));
            if (fd >= 0)
                close(fd);
            close(rfd);
        }
        join(tmp, sizeof(tmp), src, "new");
        check(access(tmp, F_OK) != 0, "nothing was created in the source through the read-only bind");
        unlink(tmp);
        join(tmp, sizeof(tmp), src, "new2");
        unlink(tmp);
        join(tmp, sizeof(tmp), src, "newdir");
        rmdir(tmp);
        check(umount2(ro, 0) == 0, "umount %s (%s)", ro, strerror(errno));
    } else {
        check(0, "mount --bind %s %s (%s)", src, ro, strerror(errno));
    }

    // A mount on a directory inside the bind is what a relative name from the
    // bind reaches, not the directory underneath it.
    if (mount("tmpfs", dst_sub, "tmpfs", 0, NULL) == 0) {
        check(chdir(dst) == 0, "chdir %s (%s)", dst, strerror(errno));
        struct statfs sfs;
        check(statfs("sub", &sfs) == 0 && (unsigned long) sfs.f_type == TMPFS_MAGIC_,
              "statfs(\"sub\") from the bind is the tmpfs mounted there (f_type %#lx)",
              (unsigned long) sfs.f_type);
        int fd = open("sub/t", O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
        check(fd >= 0, "create sub/t from a cwd in the bind (%s)", strerror(errno));
        if (fd >= 0)
            close(fd);
        join(tmp, sizeof(tmp), dst_sub, "t");
        check(access(tmp, F_OK) == 0, "%s is on the tmpfs", tmp);
        join(tmp, sizeof(tmp), src_sub, "t");
        check(access(tmp, F_OK) != 0, "%s, under the tmpfs, was not created", tmp);
        unlink(tmp);
        check(chdir("/") == 0, "chdir / (%s)", strerror(errno));
        check(umount2(dst_sub, 0) == 0, "umount %s (%s)", dst_sub, strerror(errno));
    } else {
        check(0, "mount -t tmpfs tmpfs %s (%s)", dst_sub, strerror(errno));
    }

    // A bind of a directory outside a chroot, inside it.
    if (mount(outside, jmnt, NULL, MS_BIND, NULL) == 0) {
        fflush(stdout);
        pid_t pid = fork();
        if (pid == 0) {
            failures_total = 0;
            jail_child(jail);
            fflush(stdout);
            _exit(failures_total == 0 ? 0 : 1);
        }
        int status = 0;
        check(pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
                  WEXITSTATUS(status) == 0,
              "the chroot child passed (status %#x)", status);
        check(umount2(jmnt, 0) == 0, "umount %s (%s)", jmnt, strerror(errno));
    } else {
        check(0, "mount --bind %s %s (%s)", outside, jmnt, strerror(errno));
    }

    check(umount2(dst, 0) == 0, "umount %s (%s)", dst, strerror(errno));

cleanup:
    umount2(jmnt, MNT_DETACH);
    umount2(dst_sub, MNT_DETACH);
    umount2(dst2, MNT_DETACH);
    umount2(ro, MNT_DETACH);
    umount2(dst, MNT_DETACH);
    join(tmp, sizeof(tmp), src, "lnk");
    unlink(tmp);
    join(tmp, sizeof(tmp), src, "fifo");
    unlink(tmp);
    unlink(src_f);
    join(tmp, sizeof(tmp), jail, "jail-marker");
    unlink(tmp);
    join(tmp, sizeof(tmp), outside, "outside-marker");
    unlink(tmp);
    const char *rm[] = {src_sub, src, dst, dst2, ro, jmnt, jail, outside, base};
    for (size_t i = 0; i < sizeof(rm) / sizeof(rm[0]); i++)
        rmdir(rm[i]);
    return finish_suite(TEST_NAME);
}
