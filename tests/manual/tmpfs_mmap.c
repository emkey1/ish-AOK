// tmpfs mmap: file-backed mappings on tmpfs mounts (/tmp, /run, /dev/shm).
//
// History: tmpfs_fdops had no .mmap op, so mmap of any file on a tmpfs mount
// returned ENODEV (found via the stress-ng memory+vm sweep). That broke every
// consumer of POSIX shared memory (shm_open + mmap on /dev/shm) and anything
// mmapping a file in /tmp or /run. Root cause: tmpfs file contents lived in a
// malloc'd buffer that realloc moves on every resize, so it could never be
// handed to the guest page tables. Fixed by lazily migrating a file's backing
// to an unlinked host temp file on first mmap, making tmpfs mappings host
// mmaps of the same host file (same machinery as realfs), which buys
// MAP_SHARED write-back and mmap<->read/write coherence from the host kernel.
//
// Verifies (all behaviors ground-truthed against real Linux): MAP_SHARED
// coherence in both directions (map write -> fd read, fd write -> map read);
// mmap-writes past EOF don't extend the file; MAP_PRIVATE isolation; EACCES
// for MAP_SHARED+PROT_WRITE on O_RDONLY and any mapping of O_WRONLY (while
// MAP_PRIVATE RW on O_RDONLY succeeds); ENODEV for directories and FIFOs;
// EINVAL for an unaligned offset; nonzero-offset mapping content; ftruncate
// grow/shrink visibility through a live mapping; fd I/O still correct after
// the backing migration; MAP_SHARED propagation across fork; and the full
// shm_open + ftruncate + mmap POSIX shm flow. Arch-neutral.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/wait.h>
#include <unistd.h>
#include "test_common.h"

#define TMPFS_MAGIC_ 0x01021994

static char tmpdir[128];

// Find or make a tmpfs directory to test in. Prefer mounting a private tmpfs
// (needs root, which the suite has on-device); fall back to /dev/shm or /tmp
// if either is already tmpfs (the usual case on real Linux).
static int pick_tmpfs_dir(void) {
    struct statfs sfs;
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/tmpfs-mmap-test.%d", (int) getpid());
    if (mkdir(tmpdir, 0700) == 0 &&
            mount("tmpfs", tmpdir, "tmpfs", 0, NULL) == 0)
        return 0;
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

static void check(int cond, const char *what) {
    if (cond) {
        test_logf("ok: %s\n", what);
    } else {
        printf("FAIL: %s (errno=%d %s)\n", what, errno, strerror(errno));
        failures_total++;
    }
}

static int make_file(char *path, size_t pathsize, const char *name, const char *data) {
    snprintf(path, pathsize, "%s/%s.%d", tmpdir, name, (int) getpid());
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0 && data != NULL)
        check(write(fd, data, strlen(data) + 1) == (ssize_t) strlen(data) + 1, "setup write");
    return fd;
}

// MAP_SHARED coherence: map write visible via fd read, fd write visible via map.
static void test_shared_coherence(void) {
    char path[256];
    int fd = make_file(path, sizeof(path), "shared", "hello tmpfs");
    check(fd >= 0, "open shared test file");
    if (fd < 0) return;

    char *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    check(p != MAP_FAILED, "mmap MAP_SHARED RW on tmpfs file");
    if (p == MAP_FAILED) { close(fd); unlink(path); return; }

    check(strcmp(p, "hello tmpfs") == 0, "initial content readable through mapping");

    memcpy(p, "WRITE", 5); // write via mapping...
    char buf[32] = {0};
    check(pread(fd, buf, sizeof(buf), 0) > 0 && strcmp(buf, "WRITE tmpfs") == 0,
          "map write visible through fd read");

    check(pwrite(fd, "x", 1, 6) == 1 && strcmp(p, "WRITE xmpfs") == 0,
          "fd write visible through mapping");

    // mmap-write past EOF within the mapped page must not extend the file
    p[100] = 'Z';
    struct stat st;
    check(fstat(fd, &st) == 0 && st.st_size == 12,
          "mmap write past EOF does not extend the file");

    munmap(p, 4096);
    close(fd);
    unlink(path);
}

// MAP_PRIVATE: writes stay private to the mapping.
static void test_private_isolation(void) {
    char path[256];
    int fd = make_file(path, sizeof(path), "private", "hello tmpfs");
    if (fd < 0) { check(0, "open private test file"); return; }

    char *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    check(p != MAP_FAILED, "mmap MAP_PRIVATE RW on tmpfs file");
    if (p != MAP_FAILED) {
        p[0] = 'Q';
        char c = 0;
        check(pread(fd, &c, 1, 0) == 1 && c == 'h',
              "MAP_PRIVATE write not visible through fd");
        munmap(p, 4096);
    }
    close(fd);
    unlink(path);
}

// Access-mode checks, verified against real Linux.
static void test_access_checks(void) {
    char path[256];
    int fd = make_file(path, sizeof(path), "access", "data");
    if (fd < 0) { check(0, "open access test file"); return; }
    close(fd);

    int rofd = open(path, O_RDONLY);
    char *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, rofd, 0);
    check(p == MAP_FAILED && errno == EACCES,
          "MAP_SHARED PROT_WRITE on O_RDONLY fd -> EACCES");
    if (p != MAP_FAILED) munmap(p, 4096);

    p = mmap(NULL, 4096, PROT_READ, MAP_SHARED, rofd, 0);
    check(p != MAP_FAILED, "MAP_SHARED PROT_READ on O_RDONLY fd succeeds");
    if (p != MAP_FAILED) munmap(p, 4096);

    p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, rofd, 0);
    check(p != MAP_FAILED, "MAP_PRIVATE RW on O_RDONLY fd succeeds");
    if (p != MAP_FAILED) munmap(p, 4096);
    close(rofd);

    int wofd = open(path, O_WRONLY);
    p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, wofd, 0);
    check(p == MAP_FAILED && errno == EACCES,
          "MAP_SHARED RW on O_WRONLY fd -> EACCES");
    if (p != MAP_FAILED) munmap(p, 4096);
    p = mmap(NULL, 4096, PROT_WRITE, MAP_SHARED, wofd, 0);
    check(p == MAP_FAILED && errno == EACCES,
          "MAP_SHARED PROT_WRITE on O_WRONLY fd -> EACCES");
    if (p != MAP_FAILED) munmap(p, 4096);
    close(wofd);

    // unaligned offset
    fd = open(path, O_RDWR);
    p = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 123);
    check(p == MAP_FAILED && errno == EINVAL, "unaligned offset -> EINVAL");
    if (p != MAP_FAILED) munmap(p, 4096);
    close(fd);
    unlink(path);
}

// Directories and FIFOs on tmpfs: ENODEV, like real Linux.
static void test_enodev_types(void) {
    int dfd = open(tmpdir, O_RDONLY | O_DIRECTORY);
    check(dfd >= 0, "open tmpfs directory");
    if (dfd >= 0) {
        char *p = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, dfd, 0);
        check(p == MAP_FAILED && errno == ENODEV, "mmap tmpfs directory -> ENODEV");
        if (p != MAP_FAILED) munmap(p, 4096);
        close(dfd);
    }

    char path[256];
    snprintf(path, sizeof(path), "%s/fifo.%d", tmpdir, (int) getpid());
    if (mkfifo(path, 0600) == 0) {
        int ffd = open(path, O_RDWR); // O_RDWR on a FIFO doesn't block
        check(ffd >= 0, "open tmpfs fifo");
        if (ffd >= 0) {
            char *p = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, ffd, 0);
            check(p == MAP_FAILED && errno == ENODEV, "mmap tmpfs FIFO -> ENODEV");
            if (p != MAP_FAILED) munmap(p, 4096);
            close(ffd);
        }
        unlink(path);
    } else {
        check(0, "mkfifo on tmpfs");
    }
}

// Nonzero (page-aligned) offset maps the right part of the file.
static void test_offset_mapping(void) {
    char path[256];
    int fd = make_file(path, sizeof(path), "offset", NULL);
    if (fd < 0) { check(0, "open offset test file"); return; }
    check(ftruncate(fd, 3 * 4096) == 0, "ftruncate to 3 pages");
    check(pwrite(fd, "PAGE2", 5, 4096) == 5, "write marker at page 1");

    char *p = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 4096);
    check(p != MAP_FAILED && memcmp(p, "PAGE2", 5) == 0,
          "offset=4096 mapping shows page 1 content");
    if (p != MAP_FAILED) munmap(p, 4096);
    close(fd);
    unlink(path);
}

// ftruncate through a live mapping: grown region readable (zeros) after grow.
// Growth happens via the fd while the mapping is live; the mapping must keep
// tracking the same file.
static void test_truncate_interaction(void) {
    char path[256];
    int fd = make_file(path, sizeof(path), "trunc", NULL);
    if (fd < 0) { check(0, "open trunc test file"); return; }
    check(ftruncate(fd, 4096) == 0, "ftruncate to 1 page");

    // Map 2 pages up front; page 1 is beyond EOF for now.
    char *p = mmap(NULL, 2 * 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    check(p != MAP_FAILED, "mmap 2 pages of 1-page file");
    if (p == MAP_FAILED) { close(fd); unlink(path); return; }

    p[10] = 'A';
    check(ftruncate(fd, 2 * 4096) == 0, "grow file under live mapping");
    check(p[4096] == 0, "grown page reads back zero through mapping");
    p[4096 + 10] = 'B';
    char c = 0;
    check(pread(fd, &c, 1, 4096 + 10) == 1 && c == 'B',
          "write to grown page visible through fd");

    munmap(p, 2 * 4096);
    close(fd);
    unlink(path);
}

// fd I/O must stay correct after the first mmap migrates the file's backing
// store (append via write, read back full contents, resize down).
static void test_fd_io_after_migration(void) {
    char path[256];
    int fd = make_file(path, sizeof(path), "migrate", "0123456789");
    if (fd < 0) { check(0, "open migrate test file"); return; }

    char *p = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0);
    check(p != MAP_FAILED, "mmap to force backing migration");
    if (p != MAP_FAILED) munmap(p, 4096); // mapping gone; file remains migrated

    check(lseek(fd, 0, SEEK_END) == 11, "lseek END after migration");
    check(write(fd, "abc", 3) == 3, "append after migration");
    char buf[32] = {0};
    check(pread(fd, buf, sizeof(buf), 0) == 14 && memcmp(buf, "0123456789\0abc", 14) == 0,
          "read back full contents after migration");
    struct stat st;
    check(fstat(fd, &st) == 0 && st.st_size == 14, "size updated after append");
    check(ftruncate(fd, 5) == 0 && fstat(fd, &st) == 0 && st.st_size == 5,
          "shrink after migration");
    check(pread(fd, buf, sizeof(buf), 0) == 5, "read clamped to shrunk size");

    close(fd);
    unlink(path);
}

// MAP_SHARED across fork: child's write must be visible in the parent.
static void test_shared_fork(void) {
    char path[256];
    int fd = make_file(path, sizeof(path), "fork", "parent data");
    if (fd < 0) { check(0, "open fork test file"); return; }

    char *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    check(p != MAP_FAILED, "mmap MAP_SHARED before fork");
    if (p == MAP_FAILED) { close(fd); unlink(path); return; }

    pid_t pid = fork();
    if (pid == 0) {
        memcpy(p, "CHILD!", 6);
        _exit(0);
    }
    int status;
    check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "forked child exited cleanly");
    check(memcmp(p, "CHILD!", 6) == 0, "child's MAP_SHARED write visible in parent");

    munmap(p, 4096);
    close(fd);
    unlink(path);
}

// The real-world victim: POSIX shared memory (shm_open + ftruncate + mmap).
static void test_posix_shm(void) {
    struct statfs sfs;
    if (statfs("/dev/shm", &sfs) != 0 || sfs.f_type != TMPFS_MAGIC_) {
        test_logf("skip: /dev/shm is not tmpfs here\n");
        return;
    }
    shm_unlink("/tmpfs-mmap-test");
    int fd = shm_open("/tmpfs-mmap-test", O_RDWR | O_CREAT | O_EXCL, 0600);
    check(fd >= 0, "shm_open");
    if (fd < 0) return;
    check(ftruncate(fd, 8192) == 0, "ftruncate shm object");
    char *p = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    check(p != MAP_FAILED, "mmap shm object");
    if (p != MAP_FAILED) {
        strcpy(p, "shm works");
        char buf[16] = {0};
        check(pread(fd, buf, sizeof(buf), 0) > 0 && strcmp(buf, "shm works") == 0,
              "shm map write visible through fd");
        munmap(p, 8192);
    }
    close(fd);
    shm_unlink("/tmpfs-mmap-test");
}

int main(int argc, char **argv) {
    test_init(argc, argv);

    if (pick_tmpfs_dir() != 0) {
        // Mounting one needs privilege, and neither /dev/shm nor /tmp is
        // guaranteed to be tmpfs already. Unprivileged with no tmpfs to hand
        // is not a kernel failure -- it is nothing to test -- and reporting it
        // as one gave the gate's unprivileged leg a flat FAIL that read like a
        // tmpfs regression. tmpfs_statfs and tmpfs_nlink already skip here;
        // this is the same case, worded the same way. Still a hard failure for
        // a privileged run, where a tmpfs must have been mountable.
        if (geteuid() != 0) {
            printf("tmpfs_mmap: SKIP (no tmpfs and no privilege to mount one)\n");
            return 0;
        }
        printf("FAIL: no tmpfs available (mount failed and neither /dev/shm nor /tmp is tmpfs)\n");
        return 1;
    }
    test_logf("using tmpfs dir: %s\n", tmpdir);

    test_shared_coherence();
    test_private_isolation();
    test_access_checks();
    test_enodev_types();
    test_offset_mapping();
    test_truncate_interaction();
    test_fd_io_after_migration();
    test_shared_fork();
    test_posix_shm();

    // Clean up a private mount if we made one.
    if (strncmp(tmpdir, "/tmp/tmpfs-mmap-test.", 21) == 0) {
        umount(tmpdir);
        rmdir(tmpdir);
    }

    if (failures_total) {
        printf("tmpfs_mmap: %u FAILURES\n", failures_total);
        return 1;
    }
    printf("tmpfs_mmap: PASS\n");
    return 0;
}
