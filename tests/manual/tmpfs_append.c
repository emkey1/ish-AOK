// O_APPEND on tmpfs mounts (/tmp, /run, /dev/shm) -- and the same asserts on a
// non-tmpfs filesystem and on a memfd as controls.
//
// tmpfs_write took its write offset straight from fd->offset and never looked
// at O_APPEND_, so every append onto a tmpfs file wrote at wherever the fd
// happened to point. A freshly opened O_APPEND fd points at 0, so a shell's
// `>>` onto a file that already had content OVERWROTE the head of it with the
// bytes that were supposed to go after it -- silently, with no error and no
// size change beyond the original.
//
// Found via the crypto accelerator installer, which builds its compile probe as
// `grep ... > probe.c` then `printf 'int ish_accel_probe;\n' >> probe.c`. On an
// Arch ARM64 guest that produced:
//
//     int ish_accel_probe;                      <- 21 bytes, landed at offset 0
//     e.h>                                      <- surviving tail of line 1
//     #include <openssl/core_dispatch.h>
//
// because `int ish_accel_probe;\n` is exactly as long as `#include <openssl/cor`.
// The compiler then produced a page of errors about a file the installer had
// written itself and deleted on exit, and the installer reported the OpenSSL
// headers as the problem. The bytes are what identified the write path: grep
// only ever emits whole lines matching `^#include <openssl/`, so `e.h>` could
// not have come from grep, and no other filesystem could have produced it --
// realfs maps O_APPEND_ onto the host's O_APPEND (so fakefs, which opens
// through realfs, is fine) and kernel/memfd.c looks up its size the same way
// this fix now does.
//
// Why it hid for so long: appending to an empty or brand-new file lands at
// offset 0 legitimately, and successive writes on one fd advance fd->offset
// normally, so a loop that opens once and appends repeatedly is correct. Only
// reopening a file that ALREADY has content corrupts it -- which is exactly
// what `>>` does, and what every log-appending script does.
//
// Linux semantics asserted here:
//   - a write on an O_APPEND fd goes to end-of-file, whatever fd->offset says
//   - the offset afterwards is the new end of file
//   - an explicit lseek before the write does not move it (O_APPEND wins)
//   - two fds appending to one file interleave without clobbering
//   - O_APPEND acquired later via fcntl(F_SETFL) behaves identically
//   - writev appends as one unit
//   - pwrite on an O_APPEND fd APPENDS and ignores the offset it was handed.
//     POSIX says the opposite; Linux does this and documents it under BUGS in
//     man 2 pwrite, and kernel/memfd.c already matched Linux, so tmpfs does too.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>
#include "test_common.h"

#define TMPFS_MAGIC_ 0x01021994

static char tmpdir[256];
static char otherdir[256];
static int private_mount;

static void check(int cond, const char *what) {
    if (cond) {
        test_logf("ok: %s\n", what);
    } else {
        printf("FAIL: %s (errno=%d %s)\n", what, errno, strerror(errno));
        failures_total++;
    }
}

static void check_content(const char *path, const char *expect, const char *what) {
    char buf[512];
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        printf("FAIL: %s (open: errno=%d %s)\n", what, errno, strerror(errno));
        failures_total++;
        return;
    }
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n < 0) {
        printf("FAIL: %s (read: errno=%d %s)\n", what, errno, strerror(errno));
        failures_total++;
        return;
    }
    buf[n] = '\0';
    if (strcmp(buf, expect) == 0) {
        test_logf("ok: %s\n", what);
    } else {
        printf("FAIL: %s got=\"%s\" expected=\"%s\"\n", what, buf, expect);
        failures_total++;
    }
}

// Find or make a tmpfs directory. Prefer a private mount (needs root, which the
// suite has on-device); fall back to /dev/shm or /tmp if either already is one,
// which is the usual case on real Linux.
static int pick_tmpfs_dir(void) {
    struct statfs sfs;
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/tmpfs-append-test.%d", (int) getpid());
    if (mkdir(tmpdir, 0700) == 0 &&
            mount("tmpfs", tmpdir, "tmpfs", 0, NULL) == 0) {
        private_mount = 1;
        return 0;
    }
    rmdir(tmpdir);
    if (statfs("/dev/shm", &sfs) == 0 && sfs.f_type == TMPFS_MAGIC_) {
        snprintf(tmpdir, sizeof(tmpdir), "/dev/shm");
        return 0;
    }
    if (statfs("/tmp", &sfs) == 0 && sfs.f_type == TMPFS_MAGIC_) {
        snprintf(tmpdir, sizeof(tmpdir), "/tmp");
        return 0;
    }
    return -1;
}

// A writable directory that is NOT tmpfs, so the same asserts run against the
// filesystem that was already correct (fakefs/realfs under AOK, disk on Linux).
static int pick_other_dir(void) {
    static const char *candidates[] = { "/tmp", "/var/tmp", "/root", "." };
    struct statfs sfs;
    for (unsigned i = 0; i < sizeof(candidates) / sizeof(*candidates); i++) {
        if (statfs(candidates[i], &sfs) != 0 || sfs.f_type == TMPFS_MAGIC_)
            continue;
        snprintf(otherdir, sizeof(otherdir), "%s/tmpfs-append-other.%d",
                 candidates[i], (int) getpid());
        if (mkdir(otherdir, 0700) == 0)
            return 0;
    }
    return -1;
}

// The exact installer shape, and the only case that fails on an unfixed build:
// write a file, close it, reopen with O_APPEND, write once.
static void test_reopen_append(const char *dir, const char *label) {
    char path[512], what[256];
    snprintf(path, sizeof(path), "%s/reopen.%d", dir, (int) getpid());

    static const char first[] = "#include <openssl/core.h>\n";
    static const char second[] = "int ish_accel_probe;\n";

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    snprintf(what, sizeof(what), "%s: create", label);
    check(fd >= 0 && write(fd, first, strlen(first)) == (ssize_t) strlen(first), what);
    if (fd >= 0)
        close(fd);

    fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    snprintf(what, sizeof(what), "%s: reopen O_APPEND", label);
    check(fd >= 0, what);
    if (fd >= 0) {
        ssize_t n = write(fd, second, strlen(second));
        snprintf(what, sizeof(what), "%s: append write returns full count", label);
        check(n == (ssize_t) strlen(second), what);
        // Linux leaves the offset at the new end of file.
        off_t pos = lseek(fd, 0, SEEK_CUR);
        snprintf(what, sizeof(what), "%s: offset at EOF after append", label);
        check(pos == (off_t) (strlen(first) + strlen(second)), what);
        close(fd);
    }

    char expect[512];
    snprintf(expect, sizeof(expect), "%s%s", first, second);
    snprintf(what, sizeof(what), "%s: appended data follows existing content", label);
    check_content(path, expect, what);

    struct stat st;
    snprintf(what, sizeof(what), "%s: size is the sum of both writes", label);
    check(stat(path, &st) == 0 &&
          st.st_size == (off_t) (strlen(first) + strlen(second)), what);
    unlink(path);
}

// O_APPEND overrides an explicit seek: Linux repositions inside the write.
static void test_seek_ignored(const char *dir, const char *label) {
    char path[512], what[256];
    snprintf(path, sizeof(path), "%s/seek.%d", dir, (int) getpid());

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        (void) !write(fd, "HEAD", 4);
        close(fd);
    }
    fd = open(path, O_WRONLY | O_APPEND);
    snprintf(what, sizeof(what), "%s: open for seek test", label);
    check(fd >= 0, what);
    if (fd >= 0) {
        check(lseek(fd, 0, SEEK_SET) == 0, "lseek to 0 on O_APPEND fd");
        (void) !write(fd, "TAIL", 4);
        close(fd);
    }
    snprintf(what, sizeof(what), "%s: lseek(0) does not defeat O_APPEND", label);
    check_content(path, "HEADTAIL", what);
    unlink(path);
}

// Two independent O_APPEND descriptions on one file. Each write must find the
// current end, not the position its own fd was left at.
static void test_two_writers(const char *dir, const char *label) {
    char path[512], what[256];
    snprintf(path, sizeof(path), "%s/two.%d", dir, (int) getpid());

    int a = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
    int b = open(path, O_WRONLY | O_APPEND);
    snprintf(what, sizeof(what), "%s: two O_APPEND fds open", label);
    check(a >= 0 && b >= 0, what);
    if (a >= 0 && b >= 0) {
        (void) !write(a, "aaa", 3);
        (void) !write(b, "bbb", 3);
        (void) !write(a, "ccc", 3);
        (void) !write(b, "ddd", 3);
    }
    if (a >= 0) close(a);
    if (b >= 0) close(b);
    snprintf(what, sizeof(what), "%s: interleaved appenders do not clobber", label);
    check_content(path, "aaabbbcccddd", what);
    unlink(path);
}

// O_APPEND acquired after the fact, via fcntl(F_SETFL).
static void test_setfl_append(const char *dir, const char *label) {
    char path[512], what[256];
    snprintf(path, sizeof(path), "%s/setfl.%d", dir, (int) getpid());

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    snprintf(what, sizeof(what), "%s: open for F_SETFL test", label);
    check(fd >= 0, what);
    if (fd < 0)
        return;
    (void) !write(fd, "HEAD", 4);
    check(lseek(fd, 0, SEEK_SET) == 0, "rewind before enabling O_APPEND");

    int flags = fcntl(fd, F_GETFL);
    snprintf(what, sizeof(what), "%s: F_SETFL O_APPEND accepted", label);
    check(flags >= 0 && fcntl(fd, F_SETFL, flags | O_APPEND) == 0, what);
    snprintf(what, sizeof(what), "%s: F_GETFL reports O_APPEND", label);
    check((fcntl(fd, F_GETFL) & O_APPEND) != 0, what);

    (void) !write(fd, "TAIL", 4);
    close(fd);
    snprintf(what, sizeof(what), "%s: write after F_SETFL O_APPEND appends", label);
    check_content(path, "HEADTAIL", what);
    unlink(path);
}

// writev on an O_APPEND fd: the whole gather lands at end of file, in order.
static void test_writev_append(const char *dir, const char *label) {
    char path[512], what[256];
    snprintf(path, sizeof(path), "%s/wv.%d", dir, (int) getpid());

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        (void) !write(fd, "HEAD", 4);
        close(fd);
    }
    fd = open(path, O_WRONLY | O_APPEND);
    snprintf(what, sizeof(what), "%s: open for writev test", label);
    check(fd >= 0, what);
    if (fd >= 0) {
        struct iovec iov[2] = {
            { .iov_base = (void *) "one", .iov_len = 3 },
            { .iov_base = (void *) "two", .iov_len = 3 },
        };
        snprintf(what, sizeof(what), "%s: writev returns full count", label);
        check(writev(fd, iov, 2) == 6, what);
        close(fd);
    }
    snprintf(what, sizeof(what), "%s: writev appends in order", label);
    check_content(path, "HEADonetwo", what);
    unlink(path);
}

// Linux's documented pwrite deviation: with O_APPEND set, pwrite ignores the
// offset it was handed and appends (man 2 pwrite, BUGS).
static void test_pwrite_append(const char *dir, const char *label) {
    char path[512], what[256];
    snprintf(path, sizeof(path), "%s/pw.%d", dir, (int) getpid());

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        (void) !write(fd, "HEAD", 4);
        close(fd);
    }
    fd = open(path, O_WRONLY | O_APPEND);
    snprintf(what, sizeof(what), "%s: open for pwrite test", label);
    check(fd >= 0, what);
    if (fd >= 0) {
        // Park the position somewhere recognisable: Linux appends the data but
        // leaves the file offset exactly where it was, since pwrite never
        // touches it. Both halves verified against a real kernel.
        check(lseek(fd, 2, SEEK_SET) == 2, "seek before pwrite");
        snprintf(what, sizeof(what), "%s: pwrite returns full count", label);
        check(pwrite(fd, "TAIL", 4, 0) == 4, what);
        snprintf(what, sizeof(what), "%s: pwrite leaves the file offset alone", label);
        check(lseek(fd, 0, SEEK_CUR) == 2, what);
        close(fd);
    }
    snprintf(what, sizeof(what), "%s: pwrite at offset 0 appends when O_APPEND", label);
    check_content(path, "HEADTAIL", what);
    unlink(path);
}

static void run_suite(const char *dir, const char *label) {
    test_logf("--- %s (%s) ---\n", label, dir);
    test_reopen_append(dir, label);
    test_seek_ignored(dir, label);
    test_two_writers(dir, label);
    test_setfl_append(dir, label);
    test_writev_append(dir, label);
    test_pwrite_append(dir, label);
}

// The memfd arm: same host-file backing as a tmpfs file, and the place the
// correct O_APPEND handling already lived. memfd_create takes no O_APPEND, so
// the flag arrives via F_SETFL.
static void test_memfd_append(void) {
    int fd = (int) syscall(SYS_memfd_create, "append-test", 0u);
    if (fd < 0) {
        test_logf("memfd: SKIP (memfd_create: %s)\n", strerror(errno));
        return;
    }
    test_logf("--- memfd ---\n");
    check(write(fd, "HEAD", 4) == 4, "memfd: initial write");
    check(lseek(fd, 0, SEEK_SET) == 0, "memfd: rewind");
    int flags = fcntl(fd, F_GETFL);
    check(flags >= 0 && fcntl(fd, F_SETFL, flags | O_APPEND) == 0,
          "memfd: F_SETFL O_APPEND accepted");
    check(write(fd, "TAIL", 4) == 4, "memfd: append write");
    check(lseek(fd, 0, SEEK_CUR) == 8, "memfd: offset at EOF after append");

    char buf[64];
    ssize_t n = pread(fd, buf, sizeof(buf) - 1, 0);
    if (n < 0) {
        printf("FAIL: memfd: pread (errno=%d %s)\n", errno, strerror(errno));
        failures_total++;
    } else {
        buf[n] = '\0';
        check(strcmp(buf, "HEADTAIL") == 0, "memfd: append lands at EOF");
    }
    close(fd);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));

    int have_tmpfs = pick_tmpfs_dir() == 0;
    int have_other = pick_other_dir() == 0;

    if (have_tmpfs) {
        test_logf("tmpfs dir: %s (private=%d)\n", tmpdir, private_mount);
        run_suite(tmpdir, "tmpfs");
    } else {
        printf("tmpfs_append: no tmpfs available, tmpfs arm SKIPPED\n");
    }
    if (have_other) {
        test_logf("non-tmpfs dir: %s\n", otherdir);
        run_suite(otherdir, "non-tmpfs");
        rmdir(otherdir);
    } else {
        test_logf("non-tmpfs arm SKIPPED (no non-tmpfs writable dir)\n");
    }
    test_memfd_append();

    if (!have_tmpfs && !have_other) {
        printf("tmpfs_append: SKIP (no writable filesystem found)\n");
        return 0;
    }
    if (private_mount) {
        umount(tmpdir);
        rmdir(tmpdir);
    }
    return finish_suite("tmpfs_append");
}
