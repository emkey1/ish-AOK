// A lazily unmounted mount is still what a cwd or a dirfd inside it reaches.
//
// `umount -l` takes a mount out of the tree at once, but whoever is already
// in it -- a cwd, a directory descriptor, an open file -- stays in it: the
// mount lives on, detached, until the last of them lets go. After
//
//     mount -t tmpfs t $B; echo in-tmpfs > $B/a; cd $B; umount -l $B
//
// Linux 6.12 lists `a` for `ls`, creates `echo made > b` on the tmpfs (the
// directory $B underneath stays empty), and reads /proc/self/cwd as "/": the
// path from the detached mount's own root. AOK listed nothing, created `b`
// on the root filesystem inside $B, and read the cwd as $B. Its path model
// is string based -- a relative lookup starts from the descriptor's path,
// the mount's point plus the path inside it -- and once the mount was out of
// the table that string named the directory underneath.
//
// What Linux does, all measured, and all checked here:
//   - relative names from a cwd or a dirfd in the detached mount reach it,
//     including through a relative symlink, and writes land on it;
//   - an absolute symlink in it resolves from the process's root, as ever;
//   - `..` at its root is its root: there is no parent to climb to;
//   - readlink of /proc/self/{cwd,fd/N} and /proc/self/maps name paths from
//     its root, and a /proc/self/fd/N reopen reaches the file;
//   - getcwd() is ENOENT (the kernel says "(unreachable)/", which glibc and
//     musl both refuse), unless the process's root is in the same detached
//     mount -- a chroot into it -- where getcwd is the ordinary answer;
//   - it is in no mount listing, and umount of its old point is EINVAL;
//   - every mount below it is detached too, and DISCONNECTED: a name from
//     its root reaches the directory a submount covered, and a descriptor
//     in the submount finds `..` at the submount's root;
//   - no absolute path reaches it. AOK parks a detached mount at a private
//     staging point, /.ish-fsmount/<n>; the check that no such name resolves
//     is trivially true on Linux and is what keeps that point private here.
//
// As root. Unprivileged, the mount is EPERM and the test checks nothing broke.
//
// Oracle: Linux 6.12 (camd), glibc x86_64 and -m32, as root (in and out of
// `unshare -m`) and not.
#define _GNU_SOURCE
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
#include <sys/statfs.h>
#include <sys/wait.h>
#include <unistd.h>
#include "test_common.h"

#define TEST_NAME "mount_lazy_detached"

#ifndef O_PATH
#define O_PATH 010000000
#endif
#ifndef MNT_DETACH
#define MNT_DETACH 2
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

static char base[] = "/tmp/mlzd.XXXXXX";

static void join(char *out, size_t n, const char *dir, const char *name) {
    snprintf(out, n, "%s/%s", dir, name);
}

static void put(const char *path, const char *text) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    check(fd >= 0, "create %s (%s)", path, strerror(errno));
    if (fd < 0)
        return;
    check(write(fd, text, strlen(text)) == (ssize_t) strlen(text), "write %s (%s)", path,
          strerror(errno));
    close(fd);
}

// The contents of `path` opened relative to `dirfd`, or "<errno text>".
static void slurp_at(int dirfd, const char *path, char *out, size_t n) {
    int fd = openat(dirfd, path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        snprintf(out, n, "<%s>", strerror(errno));
        return;
    }
    ssize_t got = read(fd, out, n - 1);
    out[got > 0 ? got : 0] = '\0';
    close(fd);
}

static void check_contents_at(int dirfd, const char *path, const char *want, const char *how) {
    char got[256];
    slurp_at(dirfd, path, got, sizeof(got));
    check(strcmp(got, want) == 0, "%s: %s reads \"%s\" (got \"%s\")", how, path, want, got);
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

static int same_file(const struct stat *a, const struct stat *b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino;
}

// Does any line of `file` contain `needle`?
static int file_mentions(const char *file, const char *needle) {
    FILE *f = fopen(file, "r");
    if (f == NULL)
        return 0;
    char line[4096];
    int found = 0;
    while (fgets(line, sizeof(line), f) != NULL)
        if (strstr(line, needle) != NULL)
            found = 1;
    fclose(f);
    return found;
}

// A chroot into a mount that is then lazily unmounted: the root is in the
// detached mount, so absolute names still reach it and getcwd is ordinary.
// In a child, since a chroot is not undone. `go` is read once the parent has
// unmounted; `done` carries nothing but the child's exit.
static void jail_child(const char *jail, int go) {
    if (chroot(jail) != 0 || chdir("/") != 0) {
        check(0, "chroot %s (%s)", jail, strerror(errno));
        return;
    }
    char c;
    if (read(go, &c, 1) != 1) {
        check(0, "chroot: wait for the unmount (%s)", strerror(errno));
        return;
    }
    char got[PATH_MAX];
    if (getcwd(got, sizeof(got)) == NULL)
        snprintf(got, sizeof(got), "<getcwd: %s>", strerror(errno));
    check(strcmp(got, "/") == 0, "chroot: getcwd at a detached root is / (got %s)", got);
    check_contents_at(AT_FDCWD, "/j", "in-jail", "chroot: an absolute name");
    check_contents_at(AT_FDCWD, "/../j", "in-jail", "chroot: /.. at the root");
    check(chdir("jd") == 0, "chroot: chdir jd (%s)", strerror(errno));
    if (getcwd(got, sizeof(got)) == NULL)
        snprintf(got, sizeof(got), "<getcwd: %s>", strerror(errno));
    check(strcmp(got, "/jd") == 0, "chroot: getcwd below a detached root is /jd (got %s)", got);
    check_contents_at(AT_FDCWD, "../../j", "in-jail", "chroot: a relative name climbing to the root");
    check_contents_at(AT_FDCWD, "jlnk", "in-jail", "chroot: an absolute symlink");
    int fd = open("/jnew", O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    check(fd >= 0, "chroot: create /jnew (%s)", strerror(errno));
    if (fd >= 0)
        close(fd);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));

    if (mkdtemp(base) == NULL) {
        printf("FAIL: mkdtemp (%s)\n", strerror(errno));
        return 1;
    }
    char m[64], m_sub[80], m_sub2[80], m2[64], m2_sub[80], jail[64], marker[64], tmp[160];
    join(m, sizeof(m), base, "m");
    join(m_sub, sizeof(m_sub), m, "sub");
    join(m_sub2, sizeof(m_sub2), m, "sub2");
    join(m2, sizeof(m2), base, "m2");
    join(m2_sub, sizeof(m2_sub), m2, "sub");
    join(jail, sizeof(jail), base, "jail");
    join(marker, sizeof(marker), base, "marker");
    const char *dirs[] = {m, m2, jail};
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
        check(mkdir(dirs[i], 0755) == 0, "mkdir %s (%s)", dirs[i], strerror(errno));
    put(marker, "on-root");

    if (mount("t", m, "tmpfs", 0, NULL) != 0) {
        int err = errno;
        check(err == EPERM, "mount -t tmpfs t %s (%s)", m, strerror(err));
        test_logf("mount: %s, not privileged: detach checks skipped\n", strerror(err));
        check(umount2(m, MNT_DETACH) != 0, "unprivileged umount -l %s fails", m);
        check(chdir(m) == 0 && access(".", F_OK) == 0, "unprivileged: chdir %s (%s)", m,
              strerror(errno));
        check(chdir("/") == 0, "chdir / (%s)", strerror(errno));
        goto cleanup;
    }

    // The detached mount's contents: a file, a directory with a file, a
    // relative and an absolute symlink, and two submounts -- one that will be
    // held by a descriptor and one that nothing holds -- over directories
    // that have something of their own underneath.
    join(tmp, sizeof(tmp), m, "a");
    put(tmp, "in-tmpfs");
    join(tmp, sizeof(tmp), m, "dir");
    check(mkdir(tmp, 0755) == 0, "mkdir %s (%s)", tmp, strerror(errno));
    join(tmp, sizeof(tmp), m, "dir/f");
    put(tmp, "in-dir");
    join(tmp, sizeof(tmp), m, "lnk");
    check(symlink("dir/f", tmp) == 0, "symlink %s (%s)", tmp, strerror(errno));
    join(tmp, sizeof(tmp), m, "abslnk");
    check(symlink(marker, tmp) == 0, "symlink %s (%s)", tmp, strerror(errno));
    check(mkdir(m_sub, 0755) == 0 && mkdir(m_sub2, 0755) == 0, "mkdir %s, %s (%s)", m_sub,
          m_sub2, strerror(errno));
    join(tmp, sizeof(tmp), m_sub, "under-sub");
    put(tmp, "under-sub");
    join(tmp, sizeof(tmp), m_sub2, "under-sub2");
    put(tmp, "under-sub2");
    check(mount("t", m_sub, "tmpfs", 0, NULL) == 0, "mount %s (%s)", m_sub, strerror(errno));
    check(mount("t", m_sub2, "tmpfs", 0, NULL) == 0, "mount %s (%s)", m_sub2, strerror(errno));
    join(tmp, sizeof(tmp), m_sub, "in-sub");
    put(tmp, "in-sub");
    join(tmp, sizeof(tmp), m_sub2, "in-sub2");
    put(tmp, "in-sub2");

    // In it: a cwd, a dirfd, a file, an O_PATH symlink, a mapping, and a
    // dirfd in the submount.
    check(chdir(m) == 0, "chdir %s (%s)", m, strerror(errno));
    struct stat root_before;
    check(stat(".", &root_before) == 0, "stat . (%s)", strerror(errno));
    int dfd = open("dir", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    int ffd = open("a", O_RDONLY | O_CLOEXEC);
    int lfd = open("lnk", O_PATH | O_NOFOLLOW | O_CLOEXEC);
    int sfd = open("sub", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    check(dfd >= 0 && ffd >= 0 && lfd >= 0 && sfd >= 0, "open dir, a, lnk, sub (%s)",
          strerror(errno));
    void *map = ffd >= 0 ? mmap(NULL, 1, PROT_READ, MAP_SHARED, ffd, 0) : MAP_FAILED;
    check(map != MAP_FAILED, "mmap a (%s)", strerror(errno));

    // The report.
    check(umount2(m, MNT_DETACH) == 0, "umount -l %s (%s)", m, strerror(errno));

    check_contents_at(AT_FDCWD, "a", "in-tmpfs", "a relative name from the detached cwd");
    struct statfs sfs;
    check(statfs(".", &sfs) == 0 && (unsigned long) sfs.f_type == TMPFS_MAGIC_,
          "statfs(\".\") is still the tmpfs (f_type %#lx)", (unsigned long) sfs.f_type);
    put("b", "made");
    check_contents_at(AT_FDCWD, "b", "made", "a file created from the detached cwd");
    check(mkdir("newdir", 0755) == 0, "mkdir newdir from the detached cwd (%s)", strerror(errno));
    join(tmp, sizeof(tmp), m, "a");
    check(access(tmp, F_OK) != 0, "%s, the directory underneath, has no a", tmp);
    join(tmp, sizeof(tmp), m, "b");
    check(access(tmp, F_OK) != 0, "%s: the write did not land underneath", tmp);
    join(tmp, sizeof(tmp), m, "newdir");
    check(access(tmp, F_OK) != 0, "%s: the mkdir did not land underneath", tmp);

    check_link("/proc/self/cwd", "/", "the detached cwd");
    char got[PATH_MAX];
    errno = 0;
    char *cwd = getcwd(got, sizeof(got));
    check(cwd == NULL && errno == ENOENT, "getcwd in a detached mount is ENOENT (got %s)",
          cwd != NULL ? cwd : strerror(errno));

    // `..` at the root of a detached mount is that root.
    struct stat dot, dotdot;
    check(stat(".", &dot) == 0 && stat("..", &dotdot) == 0 && same_file(&dot, &dotdot) &&
              same_file(&dot, &root_before),
          ".. at the detached root is the root (%s)", strerror(errno));
    check(chdir("../..") == 0, "chdir ../.. (%s)", strerror(errno));
    check_contents_at(AT_FDCWD, "a", "in-tmpfs", "a relative name after chdir ../..");
    check_link("/proc/self/cwd", "/", "the cwd after chdir ../..");

    // Symlinks: a relative one stays in the mount, an absolute one starts
    // from the process's root.
    check_contents_at(AT_FDCWD, "lnk", "in-dir", "a relative symlink in the detached mount");
    check_contents_at(AT_FDCWD, "abslnk", "on-root", "an absolute symlink in the detached mount");
    check_contents_at(AT_FDCWD, "dir/../../../a", "in-tmpfs", "a name climbing past the root");

    // Below the root.
    check(chdir("dir") == 0, "chdir dir (%s)", strerror(errno));
    check_link("/proc/self/cwd", "/dir", "a cwd below the detached root");
    check_contents_at(AT_FDCWD, "f", "in-dir", "a relative name from below the root");
    check_contents_at(AT_FDCWD, "../a", "in-tmpfs", "../a from below the root");
    check(chdir("..") == 0, "chdir .. (%s)", strerror(errno));

    // The descriptors opened before the unmount.
    if (dfd >= 0) {
        check_fd_link(dfd, "/dir", "a dirfd in the detached mount");
        check_contents_at(dfd, "f", "in-dir", "openat(dirfd)");
        check_contents_at(dfd, "../a", "in-tmpfs", "openat(dirfd, \"../a\")");
        struct stat up;
        check(fstatat(dfd, "../..", &up, 0) == 0 && same_file(&up, &root_before),
              "fstatat(dirfd, \"../..\") is the detached root (%s)", strerror(errno));
        int nfd = openat(dfd, "g", O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
        check(nfd >= 0, "openat(dirfd, \"g\", O_CREAT) (%s)", strerror(errno));
        if (nfd >= 0)
            close(nfd);
        join(tmp, sizeof(tmp), m, "dir");
        check(access(tmp, F_OK) != 0, "%s does not exist underneath", tmp);
    }
    if (ffd >= 0) {
        check_fd_link(ffd, "/a", "a file in the detached mount");
        char link[64];
        snprintf(link, sizeof(link), "/proc/self/fd/%d", ffd);
        check_contents_at(AT_FDCWD, link, "in-tmpfs", "a /proc/self/fd reopen");
    }
    if (lfd >= 0)
        check_fd_link(lfd, "/lnk", "an O_PATH symlink in the detached mount");
    if (map != MAP_FAILED) {
        FILE *maps = fopen("/proc/self/maps", "r");
        char line[1024];
        int named = 0;
        while (maps != NULL && fgets(line, sizeof(line), maps) != NULL) {
            line[strcspn(line, "\n")] = '\0';
            size_t len = strlen(line);
            if (len >= 3 && strcmp(line + len - 3, " /a") == 0)
                named = 1;
        }
        if (maps != NULL)
            fclose(maps);
        check(named, "/proc/self/maps names the mapping /a");
        munmap(map, 1);
    }

    // The submounts went with it, disconnected: from the root, "sub" and
    // "sub2" are the directories they covered; the one a descriptor holds is
    // its own detached root.
    check_contents_at(AT_FDCWD, "sub/under-sub", "under-sub", "sub from the detached root");
    check(access("sub/in-sub", F_OK) != 0, "sub from the detached root is not the submount");
    check_contents_at(AT_FDCWD, "sub2/under-sub2", "under-sub2", "sub2 from the detached root");
    check(access("sub2/in-sub2", F_OK) != 0, "sub2 from the detached root is not the submount");
    if (sfd >= 0) {
        check_fd_link(sfd, "/", "a dirfd at a submount's root");
        check_contents_at(sfd, "in-sub", "in-sub", "openat(submount dirfd)");
        struct stat subroot, up;
        check(fstat(sfd, &subroot) == 0 && fstatat(sfd, "..", &up, 0) == 0 &&
                  same_file(&subroot, &up),
              ".. from a detached submount's root is that root (%s)", strerror(errno));
        check(fchdir(sfd) == 0, "fchdir into the submount (%s)", strerror(errno));
        check_contents_at(AT_FDCWD, "../in-sub", "in-sub", "../in-sub from the submount");
        check(fchdir(dfd) == 0, "fchdir back (%s)", strerror(errno));
    }

    // Out of every listing, and not a mount point any more.
    check(!file_mentions("/proc/self/mountinfo", base), "mountinfo does not list %s", base);
    check(!file_mentions("/proc/mounts", base), "/proc/mounts does not list %s", base);
    check(!file_mentions("/proc/self/mountinfo", "ish-fsmount"),
          "mountinfo does not list a staging point");
    errno = 0;
    check(umount2(m, 0) != 0 && errno == EINVAL, "umount %s again is EINVAL (%s)", m,
          strerror(errno));
    errno = 0;
    check(umount2("..", 0) != 0 && errno == EINVAL,
          "umount of the detached mount, by a name from inside it, is EINVAL (%s)",
          strerror(errno));

    // No absolute name reaches it.
    int reached = 0;
    for (int i = 0; i < 256; i++) {
        snprintf(tmp, sizeof(tmp), "/.ish-fsmount/%d/a", i);
        if (access(tmp, F_OK) == 0)
            reached = 1;
    }
    check(!reached, "no /.ish-fsmount/<n> name reaches a detached mount");

    // A child inherits the cwd, and it is still in the detached mount.
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0)
        _exit(access("../a", F_OK) == 0 ? 0 : 1);
    int status = 0;
    check(pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
              WEXITSTATUS(status) == 0,
          "a forked child's inherited cwd is in the detached mount (status %#x)", status);

    check(chdir("/") == 0, "chdir / (%s)", strerror(errno));
    if (dfd >= 0)
        close(dfd);
    if (ffd >= 0)
        close(ffd);
    if (lfd >= 0)
        close(lfd);
    if (sfd >= 0)
        close(sfd);

    // A cwd in a SUBMOUNT when its parent is lazily unmounted: the submount
    // is disconnected, so it is the cwd's detached root.
    if (mount("t", m2, "tmpfs", 0, NULL) == 0) {
        check(mkdir(m2_sub, 0755) == 0, "mkdir %s (%s)", m2_sub, strerror(errno));
        check(mount("t", m2_sub, "tmpfs", 0, NULL) == 0, "mount %s (%s)", m2_sub,
              strerror(errno));
        join(tmp, sizeof(tmp), m2_sub, "d");
        check(mkdir(tmp, 0755) == 0, "mkdir %s (%s)", tmp, strerror(errno));
        join(tmp, sizeof(tmp), m2_sub, "in-sub");
        put(tmp, "m2-sub");
        join(tmp, sizeof(tmp), m2_sub, "d");
        check(chdir(tmp) == 0, "chdir %s (%s)", tmp, strerror(errno));
        check(umount2(m2, MNT_DETACH) == 0, "umount -l %s (%s)", m2, strerror(errno));
        check_link("/proc/self/cwd", "/d", "a cwd in a submount of a detached mount");
        check_contents_at(AT_FDCWD, "../in-sub", "m2-sub", "../in-sub from a detached submount");
        struct stat up1, up2;
        check(stat("..", &up1) == 0 && stat("../..", &up2) == 0 && same_file(&up1, &up2),
              ".. stops at the detached submount's root (%s)", strerror(errno));
        check(!file_mentions("/proc/self/mountinfo", base), "mountinfo does not list %s", base);
        check(chdir("/") == 0, "chdir / (%s)", strerror(errno));
    } else {
        check(0, "mount -t tmpfs t %s (%s)", m2, strerror(errno));
    }

    // A chroot into a mount that is then lazily unmounted.
    if (mount("t", jail, "tmpfs", 0, NULL) == 0) {
        join(tmp, sizeof(tmp), jail, "j");
        put(tmp, "in-jail");
        join(tmp, sizeof(tmp), jail, "jd");
        check(mkdir(tmp, 0755) == 0, "mkdir %s (%s)", tmp, strerror(errno));
        join(tmp, sizeof(tmp), jail, "jd/jlnk");
        check(symlink("/j", tmp) == 0, "symlink %s (%s)", tmp, strerror(errno));
        int go[2];
        check(pipe(go) == 0, "pipe (%s)", strerror(errno));
        fflush(stdout);
        pid = fork();
        if (pid == 0) {
            close(go[1]);
            failures_total = 0;
            jail_child(jail, go[0]);
            fflush(stdout);
            _exit(failures_total == 0 ? 0 : 1);
        }
        close(go[0]);
        // The child is in once its cwd is the jail's root; then unmount.
        char want[64], link[64];
        snprintf(link, sizeof(link), "/proc/%d/root", (int) pid);
        for (int i = 0; i < 500; i++) {
            ssize_t n = readlink(link, want, sizeof(want) - 1);
            if (n > 0 && (want[n] = '\0', strcmp(want, jail) == 0))
                break;
            usleep(10000);
        }
        check(umount2(jail, MNT_DETACH) == 0, "umount -l %s (%s)", jail, strerror(errno));
        check(write(go[1], "g", 1) == 1, "release the chroot child (%s)", strerror(errno));
        close(go[1]);
        status = 0;
        check(pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
                  WEXITSTATUS(status) == 0,
              "the chroot child passed (status %#x)", status);
        join(tmp, sizeof(tmp), jail, "jnew");
        check(access(tmp, F_OK) != 0, "%s: the chroot's write did not land underneath", tmp);
    } else {
        check(0, "mount -t tmpfs t %s (%s)", jail, strerror(errno));
    }

cleanup:
    chdir("/");
    umount2(m_sub, MNT_DETACH);
    umount2(m_sub2, MNT_DETACH);
    umount2(m, MNT_DETACH);
    umount2(m2_sub, MNT_DETACH);
    umount2(m2, MNT_DETACH);
    umount2(jail, MNT_DETACH);
    // Whatever landed underneath is removed too, so a failing run leaves the
    // same empty directories a passing one does.
    const char *under[] = {"a", "b", "newdir", "dir/g", "dir", "sub", "sub2", NULL};
    for (int i = 0; under[i] != NULL; i++) {
        join(tmp, sizeof(tmp), m, under[i]);
        if (unlink(tmp) != 0)
            rmdir(tmp);
    }
    join(tmp, sizeof(tmp), jail, "jnew");
    unlink(tmp);
    check(rmdir(m) == 0, "rmdir %s, no longer a mount point (%s)", m, strerror(errno));
    rmdir(m2_sub);
    rmdir(m2);
    rmdir(jail);
    unlink(marker);
    rmdir(base);
    return finish_suite(TEST_NAME);
}
