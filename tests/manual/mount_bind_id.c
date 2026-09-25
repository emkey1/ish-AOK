// A bind mount is a mount of its own: statx, /proc/self/fdinfo and
// /proc/self/mountinfo must all name it by its own ID, and mountinfo must say
// which directory of its filesystem it shows.
//
// Regression for a triage report. After `mount --bind /tmp/bp-src /tmp/bp-dst`
// Linux shows
//
//     229 39 254:3 /bp-src /tmp/bp-dst ...
//
// and statx(/tmp/bp-dst) gives mnt_id 229 with STATX_ATTR_MOUNT_ROOT set. AOK
// showed `8 2 0:15 / /tmp/bp-dst ...`, and statx(/tmp/bp-dst) gave mnt_id 2 --
// the ORIGIN, the mount the bind aliases -- with MOUNT_ROOT clear. A bind
// resolves to its origin for storage (fs/generic.c
// find_mount_and_trim_path_flags), and the ID was read off the origin, as was
// fdinfo's mnt_id for anything opened through the bind. systemd's
// path_is_mount_point() and util-linux mountpoint(1) compare a path's
// STATX_MNT_ID with its parent's and read STATX_ATTR_MOUNT_ROOT, so a bind
// mount point read as "not a mount point". mountinfo's field 4 was always "/".
//
// As root, binds a scratch source directory onto a scratch target, then a
// second bind of the first, and checks each path through each door:
//   - statx(target) is the bind's own ID, not its parent's, with MOUNT_ROOT;
//   - statx(source) and statx of a file under the target are not mount roots;
//     the file is on the bind, the source on the mount it always was on;
//   - statx(fd, "", AT_EMPTY_PATH) on descriptors opened through each agrees;
//   - fdinfo mnt_id of a file opened through the bind is the bind;
//   - mountinfo's root field is the source's path within its filesystem, the
//     same for the bind of the bind.
// Unprivileged, the mount is EPERM and the test only checks nothing broke.
//
// Oracle: Linux 6.12 (camd), glibc x86_64 and -m32, as root.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "test_common.h"

#define TEST_NAME "mount_bind_id"

#ifndef STATX_MNT_ID
#define STATX_MNT_ID 0x1000U
#endif
#ifndef STATX_ATTR_MOUNT_ROOT
#define STATX_ATTR_MOUNT_ROOT 0x2000U
#endif
#ifndef AT_EMPTY_PATH
#define AT_EMPTY_PATH 0x1000
#endif
#ifndef MNT_DETACH
#define MNT_DETACH 2
#endif
#if !defined(SYS_statx) && defined(__NR_statx)
#define SYS_statx __NR_statx
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

struct entry {
    int id;
    int parent;
    char root[1024];
    char point[1024];
};

// mountinfo escapes space, tab, newline and backslash as \ooo.
static void unescape(char *s) {
    char *out = s;
    for (char *in = s; *in != '\0'; ) {
        if (in[0] == '\\' && in[1] >= '0' && in[1] <= '7' && in[2] >= '0' && in[2] <= '7' &&
                in[3] >= '0' && in[3] <= '7') {
            *out++ = (char) (((in[1] - '0') << 6) | ((in[2] - '0') << 3) | (in[3] - '0'));
            in += 4;
        } else {
            *out++ = *in++;
        }
    }
    *out = '\0';
}

// Fills `out` from the LAST mountinfo line whose mount ID is `id` (id > 0) or
// whose mount point is `point` -- the mount on top, as a reader takes it.
static int find_entry(int id, const char *point, struct entry *out) {
    FILE *f = fopen("/proc/self/mountinfo", "r");
    if (f == NULL)
        return 0;
    char line[4096];
    int found = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        struct entry e = {};
        if (sscanf(line, "%d %d %*s %1023s %1023s", &e.id, &e.parent, e.root, e.point) != 4)
            continue;
        unescape(e.root);
        unescape(e.point);
        if ((id > 0 && e.id == id) || (id <= 0 && strcmp(e.point, point) == 0)) {
            *out = e;
            found = 1;
        }
    }
    fclose(f);
    return found;
}

struct statx_min {
    uint32_t mask, blksize;
    uint64_t attributes;
    uint32_t nlink, uid, gid;
    uint16_t mode, pad0;
    uint64_t ino, size, blocks, attributes_mask;
    struct { int64_t sec; uint32_t nsec; int32_t pad; } atime, btime, ctime, mtime;
    uint32_t rdev_major, rdev_minor, dev_major, dev_minor;
    uint64_t mnt_id;
    uint64_t spare[14];
};

static int have_statx = 1;

// statx's mount ID for (dirfd, path), and whether it is a mount root. -1 with
// errno ENOSYS on i386 AOK, which makes statx ENOSYS on purpose (see
// statx_mnt_id_timerfd).
static long long statx_id(int dirfd, const char *path, int flags, int *root) {
#ifdef SYS_statx
    struct statx_min sx = {};
    if (syscall(SYS_statx, dirfd, path, flags, STATX_MNT_ID, &sx) < 0)
        return -1;
    if (!(sx.mask & STATX_MNT_ID)) {
        errno = ENOSYS;
        return -1;
    }
    *root = (sx.attributes_mask & STATX_ATTR_MOUNT_ROOT) ? !!(sx.attributes & STATX_ATTR_MOUNT_ROOT)
                                                         : -1;
    return (long long) sx.mnt_id;
#else
    (void) dirfd, (void) path, (void) flags, (void) root;
    errno = ENOSYS;
    return -1;
#endif
}

static int fdinfo_mnt_id(int fd) {
    char path[64], line[256];
    snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", fd);
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return -1;
    int id = -1;
    while (fgets(line, sizeof(line), f) != NULL)
        if (sscanf(line, "mnt_id: %d", &id) == 1)
            break;
    fclose(f);
    return id;
}

// Is `outer` a mount point containing `inner` (equal, or a directory above)?
static int point_contains(const char *outer, const char *inner) {
    size_t n = strlen(outer);
    if (strcmp(outer, "/") == 0)
        return inner[0] == '/';
    return strncmp(outer, inner, n) == 0 && (inner[n] == '\0' || inner[n] == '/');
}

// The mount `path` is on, read from mountinfo alone: the last-listed entry
// with the longest point containing it.
static int mount_of(const char *path, struct entry *out) {
    FILE *f = fopen("/proc/self/mountinfo", "r");
    if (f == NULL)
        return 0;
    char line[4096];
    int found = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        struct entry e = {};
        if (sscanf(line, "%d %d %*s %1023s %1023s", &e.id, &e.parent, e.root, e.point) != 4)
            continue;
        unescape(e.root);
        unescape(e.point);
        if (point_contains(e.point, path) &&
                (!found || strlen(e.point) >= strlen(out->point))) {
            *out = e;
            found = 1;
        }
    }
    fclose(f);
    return found;
}

// What mountinfo's root field must say for a bind of `src`: the root of the
// mount src is on, joined with src's path below that mount's point.
static void expected_root(const struct entry *home, const char *src, char *out, size_t n) {
    const char *rel = strcmp(home->point, "/") == 0 ? src : src + strlen(home->point);
    if (strcmp(home->root, "/") == 0)
        snprintf(out, n, "%s", rel[0] != '\0' ? rel : "/");
    else
        snprintf(out, n, "%s%s", home->root, rel);
}

static void check_path(const char *label, const char *path, long long want_id, int want_root) {
    int root = -1;
    long long id = statx_id(AT_FDCWD, path, 0, &root);
    if (id < 0 && errno == ENOSYS) {
        have_statx = 0;
        return;
    }
    check(id == want_id, "statx(%s) mnt_id %lld is %s's %lld", path, id, label, want_id);
    check(root == want_root, "statx(%s) STATX_ATTR_MOUNT_ROOT is %d (got %d)", path, want_root,
          root);

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    check(fd >= 0, "open %s (%s)", path, strerror(errno));
    if (fd < 0)
        return;
    root = -1;
    id = statx_id(fd, "", AT_EMPTY_PATH, &root);
    check(id == want_id, "statx(fd of %s, AT_EMPTY_PATH) mnt_id %lld is %s's %lld", path, id,
          label, want_id);
    check(root == want_root, "statx(fd of %s, AT_EMPTY_PATH) MOUNT_ROOT is %d (got %d)", path,
          want_root, root);
    int fid = fdinfo_mnt_id(fd);
    check(fid == want_id, "fdinfo mnt_id %d of %s is %s's %lld", fid, path, label, want_id);
    close(fd);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));
    TEST_SKIP_IF_FOREIGN_PROC(TEST_NAME);

    char base[] = "/tmp/mbid.XXXXXX";
    if (mkdtemp(base) == NULL) {
        printf("FAIL: mkdtemp (%s)\n", strerror(errno));
        return 1;
    }
    char src[64], dst[64], dst2[64], file[80], dfile[80];
    snprintf(src, sizeof(src), "%s/src", base);
    snprintf(dst, sizeof(dst), "%s/dst", base);
    snprintf(dst2, sizeof(dst2), "%s/dst2", base);
    snprintf(file, sizeof(file), "%s/f", src);
    snprintf(dfile, sizeof(dfile), "%s/f", dst);
    check(mkdir(src, 0755) == 0 && mkdir(dst, 0755) == 0 && mkdir(dst2, 0755) == 0,
          "mkdir src, dst, dst2 (%s)", strerror(errno));
    int ffd = open(file, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    check(ffd >= 0, "create %s (%s)", file, strerror(errno));
    if (ffd >= 0)
        close(ffd);

    struct entry home = {};
    check(mount_of(src, &home), "mountinfo has the mount %s is on", src);

    if (mount(src, dst, NULL, MS_BIND, NULL) != 0) {
        int err = errno;
        check(err == EPERM, "mount --bind %s %s (%s)", src, dst, strerror(err));
        test_logf("mount: %s, not privileged: bind checks skipped\n", strerror(err));
        goto out;
    }
    int dst2_ok = mount(dst, dst2, NULL, MS_BIND, NULL) == 0;
    check(dst2_ok, "mount --bind %s %s (%s)", dst, dst2, strerror(errno));

    struct entry bind = {}, bind2 = {};
    check(find_entry(0, dst, &bind), "mountinfo lists %s", dst);
    check(bind.id != home.id, "the bind (id %d) has an ID of its own, not %s's %d", bind.id,
          home.point, home.id);
    check(bind.parent == home.id, "the bind's parent %d is %s (id %d)", bind.parent, home.point,
          home.id);
    char want_root[2048];
    expected_root(&home, src, want_root, sizeof(want_root));
    check(strcmp(bind.root, want_root) == 0,
          "mountinfo root of %s is \"%s\", where it is in its filesystem (got \"%s\")", dst,
          want_root, bind.root);
    if (dst2_ok) {
        check(find_entry(0, dst2, &bind2), "mountinfo lists %s", dst2);
        check(bind2.id != bind.id && bind2.id != home.id, "the second bind (id %d) is its own",
              bind2.id);
        check(strcmp(bind2.root, want_root) == 0,
              "mountinfo root of %s, a bind of a bind, is \"%s\" too (got \"%s\")", dst2,
              want_root, bind2.root);
    }

    check_path("the bind", dst, bind.id, 1);
    check_path("the bind", dfile, bind.id, 0);
    check_path(home.point, src, home.id, strcmp(src, home.point) == 0);
    check_path(home.point, file, home.id, 0);
    if (dst2_ok)
        check_path("the second bind", dst2, bind2.id, 1);
    if (!have_statx)
        test_logf("statx: ENOSYS, statx checks skipped\n");

    if (dst2_ok)
        check(umount2(dst2, MNT_DETACH) == 0, "umount %s (%s)", dst2, strerror(errno));
    check(umount2(dst, MNT_DETACH) == 0, "umount %s (%s)", dst, strerror(errno));
    struct entry gone = {};
    check(!find_entry(bind.id, NULL, &gone), "the bind (id %d) is gone after umount", bind.id);

out:
    unlink(file);
    rmdir(src);
    rmdir(dst);
    rmdir(dst2);
    rmdir(base);
    return finish_suite(TEST_NAME);
}
