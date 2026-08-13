// mount_flags.c — self-checking regression lock for the mount(2) flag work.
// Asserts the behavior iSH now implements:
//
//   - MS_BIND creates a bind that shares the source's backing (reads and writes
//     are visible through both the source and the bind target)
//   - MS_BIND is transitive: binding a bind collapses to the real origin
//   - statfs/fstatfs on a bind report the origin's filesystem, not EBADF
//   - "ignored" option flags (MS_REC/MS_SILENT/...) don't break a valid mount
//   - propagation-only changes (MS_PRIVATE/SHARED/SLAVE/UNBINDABLE) are accepted
//     no-ops (iSH has no mount namespaces, so the change has no observable effect)
//   - MS_MOVE relocates an existing mount
//   - a genuinely-unknown flag is still rejected with EINVAL
//
// Portable: the positive checks also pass on a real Linux kernel run as root. The
// unknown-flag rejection is gated on /proc/ish, since real Linux ignores unknown
// high mount-flag bits rather than failing.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include "test_common.h"

// Some libc headers predate a few of these; define to the canonical Linux values.
#ifndef MS_BIND
#define MS_BIND (1 << 12)
#endif
#ifndef MS_MOVE
#define MS_MOVE (1 << 13)
#endif
#ifndef MS_REC
#define MS_REC (1 << 14)
#endif
#ifndef MS_SILENT
#define MS_SILENT (1 << 15)
#endif
#ifndef MS_PRIVATE
#define MS_PRIVATE (1 << 18)
#endif

static int is_ok(const char *label, long r) {
    if (r == 0) { test_logf("ok   %s\n", label); return 1; }
    printf("FAIL %s: ret=%ld errno=%d (%s), wanted 0\n", label, r, errno, strerror(errno));
    failures_total++;
    return 0;
}
static int eq_errno(const char *label, long r, int want) {
    int e = (r < 0) ? errno : 0;
    if (r < 0 && e == want) { test_logf("ok   %s (errno %d)\n", label, e); return 1; }
    printf("FAIL %s: ret=%ld errno=%d, wanted -1/errno=%d\n", label, r, e, want);
    failures_total++;
    return 0;
}
static int check(const char *label, int cond) {
    if (cond) { test_logf("ok   %s\n", label); return 1; }
    printf("FAIL %s\n", label);
    failures_total++;
    return 0;
}

static int statfs_ok(const char *label, const char *path, struct statfs *out) {
    if (statfs(path, out) == 0) { test_logf("ok   %s\n", label); return 1; }
    printf("FAIL %s: errno=%d (%s), wanted 0\n", label, errno, strerror(errno));
    failures_total++;
    return 0;
}

static int write_file(const char *path, const char *data) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    ssize_t n = write(fd, data, strlen(data));
    close(fd);
    return n == (ssize_t) strlen(data) ? 0 : -1;
}
static int file_has(const char *path, const char *want) {
    char buf[256];
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n < 0)
        return 0;
    buf[n] = '\0';
    return strcmp(buf, want) == 0;
}

int main(int argc, char **argv) {
    test_init(argc, argv);

    char base[64];
    snprintf(base, sizeof base, "/tmp/mountflags_XXXXXX");
    if (!mkdtemp(base) || chdir(base) != 0) { perror("setup"); return 2; }

    mkdir("src", 0755);
    mkdir("dst", 0755);
    mkdir("dst2", 0755);
    mkdir("dst3", 0755);
    mkdir("dst4", 0755);
    mkdir("moved", 0755);
    mkdir("src2", 0755);
    write_file("src/file", "hello");

    // The first bind doubles as a privilege probe: an unprivileged real-Linux run
    // can't mount, so skip cleanly rather than reporting a spurious failure.
    if (mount("src", "dst", NULL, MS_BIND, NULL) != 0) {
        if (errno == EPERM || errno == EACCES) {
            test_logf("skip: mount not permitted (%s)\n", strerror(errno));
            return finish_suite("mount_flags");
        }
        is_ok("bind", -1);
        return finish_suite("mount_flags");
    }
    test_logf("ok   bind\n");

    // A bind exposes the source's contents and shares its backing both ways.
    check("bind.reads_source", file_has("dst/file", "hello"));
    write_file("dst/newfile", "world");
    check("bind.shared_backing", file_has("src/newfile", "world"));

    // statfs must report the mount that actually backs the bind. A bind has no
    // backing of its own -- it borrows the origin's filesystem but has no
    // root_fd or data -- so asking that filesystem about the bind itself failed
    // EBADF for every fakefs- or realfs-backed origin, i.e. `df` on a bind of
    // any ordinary path. (A bind of a tmpfs worked and hid it: tmpfs's statfs
    // needs no backing.) The mkdtemp base below is the failing kind whenever
    // nothing has mounted a tmpfs over /tmp.
    struct statfs bind_sfs, src_sfs;
    int bind_sfs_ok = statfs_ok("bind.statfs", "dst", &bind_sfs);
    int src_sfs_ok = statfs_ok("bind.statfs_source", "src", &src_sfs);
    if (bind_sfs_ok && src_sfs_ok) {
        // f_bfree/f_bavail can legitimately move between the two calls; the
        // filesystem type and total size cannot.
        check("bind.statfs_type", bind_sfs.f_type == src_sfs.f_type);
        check("bind.statfs_blocks", bind_sfs.f_blocks == src_sfs.f_blocks);
    }

    // Same through an fd opened under the bind.
    int bind_fd = open("dst/file", O_RDONLY);
    if (check("bind.open", bind_fd >= 0)) {
        struct statfs fd_sfs;
        if (fstatfs(bind_fd, &fd_sfs) == 0) {
            test_logf("ok   bind.fstatfs\n");
            if (src_sfs_ok)
                check("bind.fstatfs_type", fd_sfs.f_type == src_sfs.f_type);
        } else {
            printf("FAIL bind.fstatfs: errno=%d (%s), wanted 0\n", errno, strerror(errno));
            failures_total++;
        }
        close(bind_fd);
    }

    // Binding a bind resolves transitively to the real origin.
    is_ok("bind.transitive", mount("dst", "dst2", NULL, MS_BIND, NULL));
    check("bind.transitive_reads", file_has("dst2/file", "hello"));
    struct statfs xitive_sfs;
    if (statfs_ok("bind.transitive_statfs", "dst2", &xitive_sfs) && src_sfs_ok)
        check("bind.transitive_statfs_type", xitive_sfs.f_type == src_sfs.f_type);

    // Ignored option flags must not break an otherwise-valid bind.
    is_ok("bind.ignored_flags", mount("src2", "dst3", NULL, MS_BIND | MS_REC | MS_SILENT, NULL));

    // A propagation-only change on a mount point is an accepted no-op.
    is_ok("propagation.private", mount(NULL, "dst", NULL, MS_PRIVATE, NULL));

    // MS_MOVE relocates an existing mount.
    is_ok("move", mount("dst", "moved", NULL, MS_MOVE, NULL));
    check("move.reads", file_has("moved/file", "hello"));

    // A genuinely-unknown flag is still rejected (iSH only; real Linux ignores it).
    if (access("/proc/ish", F_OK) == 0)
        eq_errno("unknown_flag.rejected", mount("src", "dst4", NULL, (unsigned long) 1 << 29, NULL), EINVAL);

    // Best-effort cleanup so repeat runs don't accumulate global mounts.
    umount("dst2");
    umount("dst3");
    umount("moved");

    return finish_suite("mount_flags");
}
