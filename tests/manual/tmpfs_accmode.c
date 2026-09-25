// A tmpfs file reads and writes only as its descriptor was opened to: write,
// pwrite and writev through an O_RDONLY descriptor are EBADF, and so are read,
// pread and readv through an O_WRONLY one, with the file left as it was.
//
// tmpfs never asked. open(2) checks the permission for the access mode it is
// given, and a write through a read-only descriptor then went straight into
// the file -- so any tmpfs file a user could read was theirs to overwrite.
// realfs never had the problem: its host descriptor is opened with the same
// access mode, and the host refuses. Found with exec_fd_pathless, whose
// read-only reopen of an unlinked tmpfs file wrote.
//
// Run on a tmpfs mounted for the purpose (as root), else on /dev/shm or /tmp
// if either is one; /tmp and a memfd are the controls either way.
//
// Oracle: Linux 6.12 (camd), glibc x86_64 and -m32, as root in `unshare -m`
// and unprivileged.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>
#include "test_common.h"

#define TEST_NAME "tmpfs_accmode"

#ifndef TMPFS_MAGIC
#define TMPFS_MAGIC 0x01021994
#endif
#ifndef MNT_DETACH
#define MNT_DETACH 2
#endif
#ifndef SYS_memfd_create
#if defined(__x86_64__)
#define SYS_memfd_create 319
#elif defined(__i386__)
#define SYS_memfd_create 356
#elif defined(__aarch64__) || defined(__riscv)
#define SYS_memfd_create 279
#endif
#endif

#define DATA "0123456789"

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

// An I/O call that must fail EBADF.
static void expect_ebadf(const char *where, const char *what, ssize_t res) {
    int err = errno;
    check(res < 0 && err == EBADF, "%s: %s is EBADF (got %zd, %s)", where, what, res,
          res < 0 ? strerror(err) : "no error");
}

// `rd` and `wr`: one file holding DATA, opened O_RDONLY and O_WRONLY.
static void descriptors(const char *where, int rd, int wr) {
    char buf[16];
    struct iovec iov = {.iov_base = buf, .iov_len = 4};

    errno = 0;
    expect_ebadf(where, "write through O_RDONLY", write(rd, "XXXX", 4));
    errno = 0;
    expect_ebadf(where, "pwrite through O_RDONLY", pwrite(rd, "XXXX", 4, 0));
    memcpy(buf, "XXXX", 4);
    errno = 0;
    expect_ebadf(where, "writev through O_RDONLY", writev(rd, &iov, 1));
    errno = 0;
    check(ftruncate(rd, 0) < 0 && errno == EINVAL, "%s: ftruncate through O_RDONLY is EINVAL (%s)",
          where, strerror(errno));

    errno = 0;
    expect_ebadf(where, "read through O_WRONLY", read(wr, buf, 4));
    errno = 0;
    expect_ebadf(where, "pread through O_WRONLY", pread(wr, buf, 4, 0));
    errno = 0;
    expect_ebadf(where, "readv through O_WRONLY", readv(wr, &iov, 1));

    // The file is as it was, and each descriptor still does what it may.
    memset(buf, 0, sizeof(buf));
    check(pread(rd, buf, sizeof(DATA) - 1, 0) == (ssize_t) sizeof(DATA) - 1 &&
              memcmp(buf, DATA, sizeof(DATA) - 1) == 0,
          "%s: the file is unchanged (got %.10s)", where, buf);
    check(lseek(rd, 0, SEEK_CUR) == 0, "%s: the refused calls left the read offset at 0", where);
    check(pwrite(wr, "A", 1, 0) == 1, "%s: O_WRONLY writes (%s)", where, strerror(errno));
    check(pread(rd, buf, 1, 0) == 1 && buf[0] == 'A', "%s: O_RDONLY reads what it wrote", where);
}

static void file_in(const char *dir, const char *label) {
    char path[PATH_MAX], where[PATH_MAX + 32];
    snprintf(path, sizeof(path), "%s/tmpfs_accmode.%d", dir, (int) getpid());
    snprintf(where, sizeof(where), "%s (%s)", label, dir);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    check(fd >= 0, "%s: create %s (%s)", where, path, strerror(errno));
    if (fd < 0)
        return;
    check(write(fd, DATA, sizeof(DATA) - 1) == (ssize_t) sizeof(DATA) - 1, "%s: fill it", where);
    close(fd);
    int rd = open(path, O_RDONLY);
    int wr = open(path, O_WRONLY);
    check(rd >= 0 && wr >= 0, "%s: open it O_RDONLY and O_WRONLY (%s)", where, strerror(errno));
    if (rd >= 0 && wr >= 0)
        descriptors(where, rd, wr);
    if (rd >= 0)
        close(rd);
    if (wr >= 0)
        close(wr);
    unlink(path);
}

static bool is_tmpfs(const char *dir) {
    struct statfs sfs;
    return statfs(dir, &sfs) == 0 && (unsigned long) sfs.f_type == TMPFS_MAGIC;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));

    char dir[] = "/tmp/tmpfs_accmode.XXXXXX";
    check(mkdtemp(dir) != NULL, "mkdtemp (%s)", strerror(errno));
    bool mounted = geteuid() == 0 && mount("tmpfs", dir, "tmpfs", 0, "size=4m") == 0;
    const char *tmpfs = mounted ? dir : is_tmpfs("/dev/shm") ? "/dev/shm" : is_tmpfs("/tmp") ? "/tmp" : NULL;
    if (tmpfs != NULL)
        file_in(tmpfs, "tmpfs");
    else
        printf("%s: no tmpfs to test (not root, and neither /dev/shm nor /tmp is one)\n", TEST_NAME);
    if (mounted)
        umount2(dir, MNT_DETACH);
    rmdir(dir);

    if (!is_tmpfs("/tmp"))
        file_in("/tmp", "control");

    // A memfd: its descriptors through /proc/self/fd, since it has no name.
    int m = (int) syscall(SYS_memfd_create, "tmpfs_accmode", 0);
    if (m < 0 && errno == ENOSYS) {
        printf("%s: memfd_create unsupported, skipping the memfd control\n", TEST_NAME);
    } else {
        check(m >= 0 && write(m, DATA, sizeof(DATA) - 1) == (ssize_t) sizeof(DATA) - 1,
              "memfd: create and fill (%s)", strerror(errno));
        char proc[64];
        snprintf(proc, sizeof(proc), "/proc/self/fd/%d", m);
        int rd = open(proc, O_RDONLY), wr = open(proc, O_WRONLY);
        check(rd >= 0 && wr >= 0, "memfd: open %s O_RDONLY and O_WRONLY (%s)", proc,
              strerror(errno));
        if (rd >= 0 && wr >= 0)
            descriptors("memfd", rd, wr);
        if (rd >= 0)
            close(rd);
        if (wr >= 0)
            close(wr);
        if (m >= 0)
            close(m);
    }

    if (failures_total != 0) {
        printf("%s: %u failure(s)\n", TEST_NAME, failures_total);
        return 1;
    }
    printf("%s: PASS\n", TEST_NAME);
    return 0;
}
