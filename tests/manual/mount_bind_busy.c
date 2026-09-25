// A bind mount is busy while anything is using it, and a lazily unmounted one
// keeps working for whoever is still inside it.
//
// Regression for a triage report. After
//     mount --bind $B/src $B/dst; exec 3<$B/dst/f; cd $B/dst; umount $B/dst
// Linux says "target is busy" (EBUSY) and AOK unmounted it. A bind resolves
// to its origin for storage (fs/generic.c find_mount_and_trim_path_seen), and
// a descriptor opened through one held a reference on the ORIGIN only, so the
// bind's own count stayed 0 and mount_remove never saw it in use.
//
// After `umount -l $B/dst` instead, Linux names what is still open there from
// the detached bind's own root -- readlink /proc/self/fd/3 is "/f",
// /proc/self/cwd is "/", getcwd() is ENOENT -- while lookups from inside it
// keep reaching its contents. AOK fell back to the source's path.
//
// As root: each thing that holds a mount on Linux -- a file, a directory, the
// bind's root, an O_PATH symlink, an O_PATH FIFO, an O_PATH directory, a dup,
// a descriptor opened relative to a dirfd, a /proc/self/fd reopen, a shared
// mapping, the cwd, another process's cwd, a chroot -- makes the bind EBUSY on
// its own, and the bind unmounts once it is let go. A descriptor on the source,
// or on a bind of the bind, does not hold it. Then the lazy half: those paths
// from the detached root, lookups from inside it that reach the bind's
// contents and nothing outside it, and a new bind on the same point that its
// predecessor's users do not hold.
//
// The origin is a tmpfs, and unmounting it last is the witness that nothing,
// a detached bind included, was leaked: a bind holds its origin here, so a
// bind that outlived its last user would keep the tmpfs busy.
//
// Unprivileged, the mount is EPERM and the test only checks nothing broke.
//
// Oracle: Linux 6.12 (camd), glibc x86_64 and -m32, as root under unshare -m.
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "test_common.h"

#define TEST_NAME "mount_bind_busy"

#ifndef O_PATH
#define O_PATH 010000000
#endif
#ifndef MNT_DETACH
#define MNT_DETACH 2
#endif
#ifndef MS_PRIVATE
#define MS_PRIVATE (1 << 18)
#endif

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

static char base[] = "/tmp/mbb.XXXXXX";
static char org[64], src[80], dst[64], dst2[64], src_f[96], dst_f[80], dst_sub[80];
static char dst_lnk[80], dst_fifo[80];

static void join(char *out, size_t n, const char *dir, const char *name) {
    snprintf(out, n, "%s/%s", dir, name);
}

// Private, so nothing mounted here propagates back to a Linux host whose / is
// shared (systemd's default); a no-op in AOK.
static int bind(const char *from, const char *to) {
    if (mount(from, to, NULL, MS_BIND, NULL) != 0) {
        check(0, "mount --bind %s %s (%s)", from, to, strerror(errno));
        return -1;
    }
    mount(NULL, to, NULL, MS_PRIVATE, NULL);
    return 0;
}

static void expect_busy(const char *point, const char *how) {
    int r = umount2(point, 0);
    int err = errno;
    check(r != 0 && err == EBUSY, "umount %s while %s is EBUSY (got %s)", point, how,
          r == 0 ? "success" : strerror(err));
    // It went anyway: put it back, so the release below has a bind to unmount.
    if (r == 0 && strcmp(point, dst) == 0)
        bind(src, dst);
}

static void expect_free(const char *point, const char *how) {
    int r = umount2(point, 0);
    check(r == 0, "umount %s %s (%s)", point, how, r == 0 ? "ok" : strerror(errno));
}

static int read_hi(int fd) {
    char buf[3] = "";
    return pread(fd, buf, 2, 0) == 2 && memcmp(buf, "hi", 2) == 0;
}

// `fd` was opened through the bind at dst, and is all that holds it.
static void busy_while_fd(int fd, const char *how) {
    check(fd >= 0, "%s: open (%s)", how, strerror(errno));
    expect_busy(dst, how);
    if (fd >= 0)
        close(fd);
    expect_free(dst, "once that is closed");
}

// Another process's cwd -- or, with `jail`, only its root -- is in the bind.
static void busy_while_child(int jail, const char *how) {
    int go[2], done[2];
    if (pipe(go) != 0 || pipe(done) != 0) {
        check(0, "pipe (%s)", strerror(errno));
        return;
    }
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        close(go[0]);
        close(done[1]);
        int ok;
        if (jail) {
            // chroot, then leave through a descriptor opened before it, so
            // the root is the only thing in the bind.
            int out = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            ok = out >= 0 && chroot(dst) == 0 && fchdir(out) == 0;
            if (out >= 0)
                close(out);
        } else {
            ok = chdir(dst_sub) == 0;
        }
        char c = ok ? 'y' : 'n';
        if (write(go[1], &c, 1) != 1 || read(done[0], &c, 1) < 0)
            _exit(1);
        _exit(0);
    }
    close(go[1]);
    close(done[0]);
    char c = 0;
    check(pid > 0 && read(go[0], &c, 1) == 1 && c == 'y', "%s: the child is in place", how);
    expect_busy(dst, how);
    close(done[1]);
    close(go[0]);
    int status = 0;
    if (pid > 0)
        waitpid(pid, &status, 0);
    expect_free(dst, "once the child has exited");
}

static void check_link(const char *link, const char *want, const char *how) {
    char got[PATH_MAX];
    ssize_t n = readlink(link, got, sizeof(got) - 1);
    if (n < 0)
        snprintf(got, sizeof(got), "<readlink: %s>", strerror(errno));
    else
        got[n] = '\0';
    check(strcmp(got, want) == 0, "%s: %s is %s (got %s)", how, link, want, got);
}

static void check_fd_link(int fd, const char *want, const char *how) {
    char link[64];
    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    check_link(link, want, how);
}

static int fdinfo_mnt_id(int fd) {
    char path[64], line[256];
    snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", fd);
    FILE *f = fopen(path, "r");
    int id = -1;
    while (f != NULL && fgets(line, sizeof(line), f) != NULL)
        if (sscanf(line, "mnt_id: %d", &id) == 1)
            break;
    if (f != NULL)
        fclose(f);
    return id;
}

// Is `point` a mount point in /proc/self/mountinfo (field 5)?
static int mountinfo_lists(const char *point) {
    FILE *f = fopen("/proc/self/mountinfo", "r");
    char line[1024], mp[PATH_MAX];
    int found = 0;
    while (f != NULL && fgets(line, sizeof(line), f) != NULL)
        if (sscanf(line, "%*d %*d %*s %*s %4095s", mp) == 1 && strcmp(mp, point) == 0)
            found = 1;
    if (f != NULL)
        fclose(f);
    return found;
}

// Does /proc/self/maps name a mapping `path`?
static int maps_names(const char *path) {
    FILE *f = fopen("/proc/self/maps", "r");
    char line[1024];
    int found = 0;
    size_t want = strlen(path);
    while (f != NULL && fgets(line, sizeof(line), f) != NULL) {
        line[strcspn(line, "\n")] = '\0';
        size_t len = strlen(line);
        if (len > want && line[len - want - 1] == ' ' && strcmp(line + len - want, path) == 0)
            found = 1;
    }
    if (f != NULL)
        fclose(f);
    return found;
}

static int make_file(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    int ok = fd >= 0 && write(fd, "hi\n", 3) == 3;
    if (fd >= 0)
        close(fd);
    return ok;
}

// The report itself, on the root filesystem rather than a tmpfs.
static void the_report(void) {
    char rsrc[80], rdst[80], rf[96];
    join(rsrc, sizeof(rsrc), base, "rsrc");
    join(rdst, sizeof(rdst), base, "rdst");
    check(mkdir(rsrc, 0755) == 0 && mkdir(rdst, 0755) == 0, "mkdir %s %s (%s)", rsrc, rdst,
          strerror(errno));
    join(rf, sizeof(rf), rsrc, "f");
    check(make_file(rf), "create %s (%s)", rf, strerror(errno));
    if (bind(rsrc, rdst) == 0) {
        join(rf, sizeof(rf), rdst, "f");
        int fd = open(rf, O_RDONLY | O_CLOEXEC);
        check(fd >= 0, "open %s (%s)", rf, strerror(errno));
        check(chdir(rdst) == 0, "chdir %s (%s)", rdst, strerror(errno));
        int r = umount2(rdst, 0);
        int err = errno;
        check(r != 0 && err == EBUSY,
              "the report: umount of a bind with a file open and the cwd in it is EBUSY (got %s)",
              r == 0 ? "success" : strerror(err));
        check(fd >= 0 && read_hi(fd), "the report: the file still reads");
        check(chdir("/") == 0, "chdir / (%s)", strerror(errno));
        if (fd >= 0)
            close(fd);
        if (r != 0)
            expect_free(rdst, "once the file is closed and the cwd has left");
    }
    join(rf, sizeof(rf), rsrc, "f");
    unlink(rf);
    rmdir(rsrc);
    rmdir(rdst);
}

// umount -l of a bind that is in use.
static void lazy(void) {
    if (bind(src, dst) != 0)
        return;
    int fd = open(dst_f, O_RDONLY | O_CLOEXEC);
    int dfd = open(dst_sub, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    check(fd >= 0 && dfd >= 0, "lazy: open %s and %s (%s)", dst_f, dst_sub, strerror(errno));
    void *map = fd >= 0 ? mmap(NULL, 1, PROT_READ, MAP_SHARED, fd, 0) : MAP_FAILED;
    check(map != MAP_FAILED, "lazy: mmap %s (%s)", dst_f, strerror(errno));
    int id_before = fd >= 0 ? fdinfo_mnt_id(fd) : -1;
    check(chdir(dst) == 0, "lazy: chdir %s (%s)", dst, strerror(errno));
    check_fd_link(fd, dst_f, "lazy, before the umount");

    check(umount2(dst, MNT_DETACH) == 0, "umount -l %s with a file, a directory, a mapping and "
          "the cwd in it (%s)", dst, strerror(errno));
    check(!mountinfo_lists(dst), "lazy: %s is gone from mountinfo", dst);
    check(access(dst_f, F_OK) != 0 && errno == ENOENT,
          "lazy: %s is the empty directory underneath again (%s)", dst_f, strerror(errno));

    // Named from the detached bind's own root.
    check_fd_link(fd, "/f", "lazy: a file open in the detached bind");
    check_fd_link(dfd, "/sub", "lazy: a directory open in the detached bind");
    check_link("/proc/self/cwd", "/", "lazy: the cwd, at the detached bind's root");
    check(maps_names("/f"), "lazy: /proc/self/maps names the mapping /f");
    char got[PATH_MAX];
    errno = 0;
    char *cwd = getcwd(got, sizeof(got));
    check(cwd == NULL && errno == ENOENT, "lazy: getcwd in the detached bind is ENOENT (got %s)",
          cwd != NULL ? cwd : strerror(errno));
    check(fd >= 0 && fdinfo_mnt_id(fd) == id_before,
          "lazy: fdinfo's mnt_id still names the bind (%d, was %d)",
          fd >= 0 ? fdinfo_mnt_id(fd) : -1, id_before);

    // Everything open in it still works, and a lookup from inside it reaches
    // the bind's contents and nothing else: tmp is in / and not in the bind.
    check(fd >= 0 && read_hi(fd), "lazy: the open file still reads");
    int rfd = open("f", O_RDONLY | O_CLOEXEC);
    check(rfd >= 0 && read_hi(rfd), "lazy: open(\"f\") from the cwd reads the bind's file (%s)",
          strerror(errno));
    if (rfd >= 0)
        close(rfd);
    rfd = dfd >= 0 ? openat(dfd, "../f", O_RDONLY | O_CLOEXEC) : -1;
    check(rfd >= 0 && read_hi(rfd), "lazy: openat(directory, \"../f\") reads the bind's file (%s)",
          strerror(errno));
    if (rfd >= 0)
        close(rfd);
    check(access("sub", F_OK) == 0, "lazy: sub is reachable from the cwd (%s)", strerror(errno));
    check(access("tmp", F_OK) != 0 && errno == ENOENT,
          "lazy: tmp, which is in / and not in the bind, is not reachable from the cwd (%s)",
          strerror(errno));
    rfd = open("/proc/self/cwd/f", O_RDONLY | O_CLOEXEC);
    check(rfd >= 0 && read_hi(rfd), "lazy: /proc/self/cwd/f reads the bind's file (%s)",
          strerror(errno));
    if (rfd >= 0)
        close(rfd);
    check(access("/proc/self/cwd/tmp", F_OK) != 0 && errno == ENOENT,
          "lazy: /proc/self/cwd/tmp does not reach / (%s)", strerror(errno));
    char link[64];
    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    rfd = open(link, O_RDONLY | O_CLOEXEC);
    check(rfd >= 0 && read_hi(rfd), "lazy: a /proc/self/fd reopen reads the file (%s)",
          strerror(errno));
    if (rfd >= 0)
        close(rfd);
    struct stat a, b;
    check(fd >= 0 && fstat(fd, &a) == 0 && stat(link, &b) == 0 && a.st_ino == b.st_ino,
          "lazy: stat of /proc/self/fd/N is the open file (%s)", strerror(errno));

    // A new bind on the same point is a mount of its own, which the old one's
    // users do not hold.
    if (bind(src, dst) == 0) {
        int nfd = open(dst_f, O_RDONLY | O_CLOEXEC);
        check(nfd >= 0, "open %s through the new bind (%s)", dst_f, strerror(errno));
        if (nfd >= 0) {
            check_fd_link(nfd, dst_f, "a file through a new bind on the same point");
            close(nfd);
        }
        check_fd_link(fd, "/f", "lazy: the old bind's file, beside a new bind on its point");
        expect_free(dst, "with only the lazily unmounted bind before it still in use");
    }

    check(chdir("/") == 0, "chdir / (%s)", strerror(errno));
    if (map != MAP_FAILED)
        munmap(map, 1);
    if (dfd >= 0)
        close(dfd);
    if (fd >= 0)
        close(fd);

    // And one with nothing in it goes at once.
    if (bind(src, dst) == 0)
        check(umount2(dst, MNT_DETACH) == 0, "umount -l %s with nothing in it (%s)", dst,
              strerror(errno));
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));

    if (mkdtemp(base) == NULL) {
        printf("FAIL: mkdtemp (%s)\n", strerror(errno));
        return 1;
    }
    char tmp[128];
    join(org, sizeof(org), base, "org");
    join(src, sizeof(src), org, "src");
    join(dst, sizeof(dst), base, "dst");
    join(dst2, sizeof(dst2), base, "dst2");
    join(src_f, sizeof(src_f), src, "f");
    join(dst_f, sizeof(dst_f), dst, "f");
    join(dst_sub, sizeof(dst_sub), dst, "sub");
    join(dst_lnk, sizeof(dst_lnk), dst, "lnk");
    join(dst_fifo, sizeof(dst_fifo), dst, "fifo");
    check(mkdir(org, 0755) == 0 && mkdir(dst, 0755) == 0 && mkdir(dst2, 0755) == 0,
          "mkdir (%s)", strerror(errno));

    if (mount("tmpfs", org, "tmpfs", 0, NULL) != 0) {
        int err = errno;
        check(err == EPERM, "mount -t tmpfs tmpfs %s (%s)", org, strerror(err));
        test_logf("mount: %s, not privileged: bind checks skipped\n", strerror(err));
        check(umount2(dst, 0) != 0 && errno == EPERM, "unprivileged umount is EPERM (%s)",
              strerror(errno));
        goto cleanup;
    }
    mount(NULL, org, NULL, MS_PRIVATE, NULL);
    check(mkdir(src, 0755) == 0, "mkdir %s (%s)", src, strerror(errno));
    check(make_file(src_f), "create %s (%s)", src_f, strerror(errno));
    join(tmp, sizeof(tmp), src, "sub");
    check(mkdir(tmp, 0755) == 0, "mkdir %s (%s)", tmp, strerror(errno));
    join(tmp, sizeof(tmp), src, "lnk");
    check(symlink("f", tmp) == 0, "symlink %s (%s)", tmp, strerror(errno));
    join(tmp, sizeof(tmp), src, "fifo");
    check(mkfifo(tmp, 0644) == 0, "mkfifo %s (%s)", tmp, strerror(errno));

    the_report();

    // The positive control: a bind nothing is using unmounts.
    if (bind(src, dst) == 0)
        expect_free(dst, "with nothing in it");

    // Each thing that holds a mount on Linux, alone.
    if (bind(src, dst) == 0)
        busy_while_fd(open(dst_f, O_RDONLY | O_CLOEXEC), "a file is open in it");
    if (bind(src, dst) == 0)
        busy_while_fd(open(dst_sub, O_RDONLY | O_DIRECTORY | O_CLOEXEC),
                      "a directory is open in it");
    if (bind(src, dst) == 0)
        busy_while_fd(open(dst, O_RDONLY | O_DIRECTORY | O_CLOEXEC), "its root is open");
    if (bind(src, dst) == 0)
        busy_while_fd(open(dst_lnk, O_PATH | O_NOFOLLOW | O_CLOEXEC),
                      "an O_PATH symlink is open in it");
    if (bind(src, dst) == 0)
        busy_while_fd(open(dst_fifo, O_PATH | O_CLOEXEC), "an O_PATH FIFO is open in it");
    if (bind(src, dst) == 0)
        busy_while_fd(open(dst_sub, O_PATH | O_DIRECTORY | O_CLOEXEC),
                      "an O_PATH directory is open in it");
    if (bind(src, dst) == 0) {
        int fd = open(dst_f, O_RDONLY | O_CLOEXEC);
        int fd2 = fd >= 0 ? fcntl(fd, F_DUPFD_CLOEXEC, 0) : -1;
        if (fd >= 0)
            close(fd);
        busy_while_fd(fd2, "a dup of a file in it is open, the original closed");
    }
    if (bind(src, dst) == 0) {
        int dfd = open(dst_sub, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        int fd = dfd >= 0 ? openat(dfd, "../f", O_RDONLY | O_CLOEXEC) : -1;
        if (dfd >= 0)
            close(dfd);
        busy_while_fd(fd, "a file opened relative to a directory in it is open");
    }
    if (bind(src, dst) == 0) {
        int fd = open(dst_f, O_RDONLY | O_CLOEXEC);
        char link[64];
        snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
        int fd2 = fd >= 0 ? open(link, O_RDONLY | O_CLOEXEC) : -1;
        if (fd >= 0)
            close(fd);
        busy_while_fd(fd2, "a /proc/self/fd reopen of a file in it is open");
    }
    if (bind(src, dst) == 0) {
        int fd = open(dst_f, O_RDONLY | O_CLOEXEC);
        void *map = fd >= 0 ? mmap(NULL, 1, PROT_READ, MAP_SHARED, fd, 0) : MAP_FAILED;
        check(map != MAP_FAILED, "mmap %s (%s)", dst_f, strerror(errno));
        if (fd >= 0)
            close(fd);
        if (map != MAP_FAILED) {
            expect_busy(dst, "a file in it is mapped, its descriptor closed");
            munmap(map, 1);
        }
        expect_free(dst, "once the mapping is gone");
    }
    if (bind(src, dst) == 0) {
        check(chdir(dst_sub) == 0, "chdir %s (%s)", dst_sub, strerror(errno));
        expect_busy(dst, "the cwd is in it");
        check(chdir("/") == 0, "chdir / (%s)", strerror(errno));
        expect_free(dst, "once the cwd has left");
    }
    if (bind(src, dst) == 0)
        busy_while_child(0, "another process's cwd is in it");
    if (bind(src, dst) == 0)
        busy_while_child(1, "another process is chrooted into it");

    // What does not hold it: the same file through the source, and a file
    // through a bind of the bind, which is a mount of its own.
    if (bind(src, dst) == 0) {
        int fd = open(src_f, O_RDONLY | O_CLOEXEC);
        check(fd >= 0, "open %s (%s)", src_f, strerror(errno));
        expect_free(dst, "with the same file open through the source");
        if (fd >= 0)
            close(fd);
    }
    if (bind(src, dst) == 0) {
        if (bind(dst, dst2) == 0) {
            join(tmp, sizeof(tmp), dst2, "f");
            int fd = open(tmp, O_RDONLY | O_CLOEXEC);
            check(fd >= 0, "open %s (%s)", tmp, strerror(errno));
            expect_free(dst, "with a file open only through a bind of it");
            int r = umount2(dst2, 0);
            int err = errno;
            check(r != 0 && err == EBUSY,
                  "umount %s, a bind of a bind, with a file open in it is EBUSY (got %s)", dst2,
                  r == 0 ? "success" : strerror(err));
            check(fd >= 0 && read_hi(fd), "the file in the bind of the bind still reads");
            if (fd >= 0)
                close(fd);
            if (r != 0)
                expect_free(dst2, "once that is closed");
        } else {
            umount2(dst, 0);
        }
    }

    lazy();

    // Nothing is left holding the origin: every bind, the detached one
    // included, has gone with its last user.
    check(umount2(org, 0) == 0, "umount the origin tmpfs %s at the end (%s)", org,
          strerror(errno));

cleanup:
    umount2(dst2, MNT_DETACH);
    umount2(dst, MNT_DETACH);
    umount2(org, MNT_DETACH);
    rmdir(org);
    rmdir(dst);
    rmdir(dst2);
    rmdir(base);
    return finish_suite(TEST_NAME);
}
