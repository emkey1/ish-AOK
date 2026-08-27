// mount_bind_rbind.c — self-checking regression lock for non-directory bind
// mounts and recursive binds (MS_REC), the two bind behaviors mount_flags.c
// predates. Asserts:
//
//   - MS_BIND of a file over a file works: reads and writes flow through both
//     names, and umount restores the covered file
//   - the mixed shapes are rejected with ENOTDIR (file over dir, dir over file)
//   - MS_BIND|MS_REC replicates a submount of the source under the target
//   - a recursive bind whose target sits inside the source subtree does not
//     clone itself (no target/target mount — Linux's clone-then-attach order,
//     which iSH-AOK's snapshot exclusion reproduces)
//
// Portable: all checks also pass on a real Linux kernel run as root; an
// unprivileged run skips at the first EPERM like mount_flags.c does.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include "test_common.h"

#ifndef MS_BIND
#define MS_BIND (1 << 12)
#endif
#ifndef MS_REC
#define MS_REC (1 << 14)
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

// Is `point` a mount point right now, per /proc/self/mountinfo field 5?
// Paths used here contain no spaces, so no \040 unescaping is needed.
static int is_mountpoint(const char *point) {
    FILE *f = fopen("/proc/self/mountinfo", "r");
    if (f == NULL)
        return 0;
    char line[1024];
    int found = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        char mp[512];
        // fields: id parent major:minor root mountpoint ...
        if (sscanf(line, "%*s %*s %*s %*s %511s", mp) == 1 && strcmp(mp, point) == 0) {
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

int main(int argc, char **argv) {
    test_init(argc, argv);

    char base[64];
    snprintf(base, sizeof base, "/tmp/bindrbind_XXXXXX");
    if (!mkdtemp(base) || chdir(base) != 0) { perror("setup"); return 2; }

    write_file("filea", "alpha");
    write_file("fileb", "beta");
    mkdir("src", 0755);
    mkdir("src/sub", 0755);
    mkdir("dst", 0755);
    write_file("src/file", "hello");

    // The first bind doubles as a privilege probe, like mount_flags.c: an
    // unprivileged real-Linux run can't mount, so skip cleanly.
    if (mount("filea", "fileb", NULL, MS_BIND, NULL) != 0) {
        if (errno == EPERM || errno == EACCES) {
            test_logf("skip: mount not permitted (%s)\n", strerror(errno));
            return finish_suite("mount_bind_rbind");
        }
        is_ok("filebind", -1);
        return finish_suite("mount_bind_rbind");
    }
    test_logf("ok   filebind\n");

    // The bind covers fileb with filea's content, shared both ways.
    check("filebind.reads_source", file_has("fileb", "alpha"));
    write_file("fileb", "written-via-bind");
    check("filebind.write_reaches_source", file_has("filea", "written-via-bind"));
    is_ok("filebind.umount", umount("fileb"));
    check("filebind.umount_restores", file_has("fileb", "beta"));

    // Mixed shapes: Linux rejects both with ENOTDIR.
    eq_errno("mixed.file_over_dir", mount("filea", "dst", NULL, MS_BIND, NULL), ENOTDIR);
    eq_errno("mixed.dir_over_file", mount("src", "filea", NULL, MS_BIND, NULL), ENOTDIR);

    // A recursive bind replicates the source's submounts. The submount is a
    // tmpfs so the check cannot pass by accident through the underlying dir.
    if (is_ok("rbind.submount_setup", mount("none", "src/sub", "tmpfs", 0, NULL))) {
        write_file("src/sub/marker", "inner");
        is_ok("rbind", mount("src", "dst", NULL, MS_BIND | MS_REC, NULL));
        check("rbind.reads_source", file_has("dst/file", "hello"));
        check("rbind.submount_replicated", file_has("dst/sub/marker", "inner"));
        umount("dst/sub");
        umount("dst");
        umount("src/sub");
    }

    // A recursive bind of a tree onto a point inside that same tree must not
    // clone itself: nest/inner appears once, with no nest/inner/inner.
    mkdir("nest", 0755);
    mkdir("nest/inner", 0755);
    char self_point[128], self_copy[128];
    snprintf(self_point, sizeof self_point, "%s/nest/inner", base);
    snprintf(self_copy, sizeof self_copy, "%s/nest/inner/inner", base);
    if (is_ok("rbind.self", mount("nest", "nest/inner", NULL, MS_BIND | MS_REC, NULL))) {
        check("rbind.self_mounted", is_mountpoint(self_point));
        check("rbind.self_not_cloned", !is_mountpoint(self_copy));
        umount("nest/inner");
    }

    return finish_suite("mount_bind_rbind");
}
