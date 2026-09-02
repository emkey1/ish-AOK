// tmpfs statfs: block/inode counts for tmpfs mounts (/tmp, /run, /dev/shm).
//
// History: the tmpfs fs_ops had no .statfs op, so statfs on any tmpfs mount
// reported 0 total/free blocks (only f_type came through, from the fs_ops
// magic fallback). Anything that checks free space before writing considered
// the mount full: stress-ng --tmpfs skipped with "cannot find writeable free
// space on a tmpfs filesystem", and df showed a 0-size filesystem. Fixed by
// implementing tmpfs_statfs mirroring real Linux semantics (ground-truthed on
// a real kernel): f_bsize = f_frsize = 4096, f_blocks = the mount's size cap
// (Linux defaults to half of RAM; AOK reports half of host RAM), f_bfree =
// f_bavail = cap minus pages actually stored, f_files/f_ffree the same cap
// minus inodes in use (the root directory counts as one), f_namelen = 255.
// cgroup/cgroup2 mounts got a statfs reporting what Linux reports for them:
// zero blocks/inodes, just f_bsize 4096 and f_namelen 255.
//
// Known deviation (not asserted here): Linux charges only pages actually
// allocated, so a sparse ftruncate consumes no blocks; AOK's tmpfs backing is
// dense, so block usage tracks file size. write()n data is charged the same
// on both, which is what this test uses.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <unistd.h>
#include "test_common.h"

#define TMPFS_MAGIC_ 0x01021994
#define CGROUP2_MAGIC_ 0x63677270

static char tmpdir[128];
static int private_mount; // we mounted it ourselves: nothing else writes to it

static void check(int cond, const char *what) {
    if (cond) {
        test_logf("ok: %s\n", what);
    } else {
        printf("FAIL: %s (errno=%d %s)\n", what, errno, strerror(errno));
        failures_total++;
    }
}

// Find or make a tmpfs directory to test in. Prefer mounting a private tmpfs
// (needs root, which the suite has on-device); fall back to /dev/shm or /tmp
// if either is already tmpfs (the usual case on real Linux).
static int pick_tmpfs_dir(void) {
    struct statfs sfs;
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/tmpfs-statfs-test.%d", (int) getpid());
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

static void test_static_fields(void) {
    struct statfs sfs;
    check(statfs(tmpdir, &sfs) == 0, "statfs on tmpfs mount");
    if (statfs(tmpdir, &sfs) != 0)
        return;
    check(sfs.f_type == TMPFS_MAGIC_, "f_type is TMPFS_MAGIC");
    check(sfs.f_bsize >= 512 && (sfs.f_bsize & (sfs.f_bsize - 1)) == 0,
          "f_bsize is a sane power of two");
    check(sfs.f_frsize == 0 || (unsigned long) sfs.f_frsize == (unsigned long) sfs.f_bsize,
          "f_frsize matches f_bsize");
    check(sfs.f_blocks > 0, "f_blocks nonzero (was: 0, mount looked full)");
    check(sfs.f_bfree > 0 && sfs.f_bfree <= sfs.f_blocks, "f_bfree in (0, f_blocks]");
    check(sfs.f_bavail > 0 && sfs.f_bavail <= sfs.f_blocks, "f_bavail in (0, f_blocks]");
    check(sfs.f_namelen == 255, "f_namelen is 255");
    // nr_inodes=0 (unlimited) reports f_files=0 on Linux; only assert when capped
    if (sfs.f_files > 0)
        check(sfs.f_ffree < sfs.f_files, "some inodes in use (root dir counts)");
}

// write()ing data must consume blocks and an inode; deleting must give them back.
static void test_usage_accounting(void) {
    struct statfs before, with_file, after;
    if (statfs(tmpdir, &before) != 0) {
        check(0, "statfs before");
        return;
    }
    const unsigned pages = 64;
    size_t len = (size_t) before.f_bsize * pages;
    char path[256];
    snprintf(path, sizeof(path), "%s/statfs-test.%d", tmpdir, (int) getpid());
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    check(fd >= 0, "create test file");
    if (fd < 0)
        return;
    char page[4096];
    memset(page, 'x', sizeof(page));
    size_t written = 0;
    while (written < len) {
        size_t chunk = len - written < sizeof(page) ? len - written : sizeof(page);
        if (write(fd, page, chunk) != (ssize_t) chunk)
            break;
        written += chunk;
    }
    check(written == len, "write test data");

    check(statfs(tmpdir, &with_file) == 0, "statfs with file present");
    check(with_file.f_bfree + pages <= before.f_bfree,
          "f_bfree dropped by at least the pages written");
    if (before.f_files > 0)
        check(with_file.f_ffree < before.f_ffree, "f_ffree dropped for the new inode");

    // fstatfs on an fd inside the mount sees the same filesystem
    struct statfs fsfs;
    check(fstatfs(fd, &fsfs) == 0 && fsfs.f_type == TMPFS_MAGIC_,
          "fstatfs reports TMPFS_MAGIC");
    check(fsfs.f_blocks == with_file.f_blocks, "fstatfs f_blocks matches statfs");

    close(fd);
    check(unlink(path) == 0, "unlink test file");
    check(statfs(tmpdir, &after) == 0, "statfs after unlink");
    if (private_mount) {
        // nothing else writes to a private mount: exact restore
        check(after.f_bfree == before.f_bfree, "f_bfree restored after unlink");
        check(after.f_ffree == before.f_ffree, "f_ffree restored after unlink");
    } else {
        check(after.f_bfree >= with_file.f_bfree + pages,
              "f_bfree recovered the pages after unlink");
    }
}

// cgroup2 mounts report zero blocks/inodes on Linux, just bsize and namelen.
// Best-effort: needs root to mount; skipped when the mount fails.
static void test_cgroup2_statfs(void) {
    char dir[128];
    snprintf(dir, sizeof(dir), "/tmp/cgroup2-statfs-test.%d", (int) getpid());
    if (mkdir(dir, 0700) != 0)
        return;
    if (mount("none", dir, "cgroup2", 0, NULL) != 0) {
        test_logf("skip: cannot mount cgroup2 (errno=%d)\n", errno);
        rmdir(dir);
        return;
    }
    struct statfs sfs;
    check(statfs(dir, &sfs) == 0, "statfs on cgroup2 mount");
    check(sfs.f_type == CGROUP2_MAGIC_, "cgroup2 f_type");
    check(sfs.f_blocks == 0 && sfs.f_bfree == 0, "cgroup2 reports zero blocks");
    check(sfs.f_bsize == 4096, "cgroup2 f_bsize 4096");
    check(sfs.f_namelen == 255, "cgroup2 f_namelen 255");
    umount(dir);
    rmdir(dir);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    if (pick_tmpfs_dir() != 0) {
        printf("tmpfs_statfs: SKIP (no tmpfs filesystem available)\n");
        return 0;
    }
    test_logf("using tmpfs dir: %s (private=%d)\n", tmpdir, private_mount);

    test_static_fields();
    test_usage_accounting();
    test_cgroup2_statfs();

    if (private_mount) {
        umount(tmpdir);
        rmdir(tmpdir);
    }
    return finish_suite("tmpfs_statfs");
}
