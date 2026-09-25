// /proc/self/mountinfo's mount IDs and parent IDs must form a tree.
//
// Regression for a triage report: "Plain findmnt spins at 100% CPU forever.
// The mount table has a loop: the root mount's parent sits underneath it.
// findmnt -l works." A mount's ID was its position in a list kept longest
// point first, so the root was numbered last, and its parent was reported as
// 1 -- the deepest mount, whose own parent was the root:
//
//     1 5 0:19 / /dev/pts rw - devpts devpts rw
//     5 1 0:15 / / rw - fake /dev/sda rw
//
// libmount finds the root of the tree by climbing parent IDs until one is
// missing from the table (mnt_table_get_root_fs), so it climbed forever. The
// positions also moved whenever any mount came or went, a mount stacked on
// another named the wrong parent and was listed before the one it covers,
// and /proc/self/fdinfo's mnt_id was a constant 1.
//
// On whatever table it finds:
//   - IDs are unique, and libmount's own root search ends, at "/";
//   - the root's parent is itself or an ID not in the table (Linux: the
//     rootfs outside the process's root, or itself for a namespace's root);
//   - every entry's parent chain reaches that root, without a cycle;
//   - a parent's mount point contains its child's;
//   - /proc/mounts lists the same mounts in the same order.
// With CAP_SYS_ADMIN, on mounts it makes -- tmpfs A on a scratch directory,
// tmpfs B on top of A, tmpfs C on B's subdirectory:
//   - no existing mount's ID changes as mounts come and go;
//   - A's parent is the mount it sits in, B's is A, C's is B;
//   - B is listed after A, as the later mount;
//   - statx STATX_MNT_ID and /proc/self/fdinfo's mnt_id name B and C.
//
// Oracle: Linux 6.12 (camd), glibc x86_64 and -m32, as root and as a user.
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

#define TEST_NAME "mountinfo_tree"

#ifndef STATX_MNT_ID
#define STATX_MNT_ID 0x1000U
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
    char point[1024];
};

struct table {
    struct entry *e;
    size_t n;
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

// Fields 1, 2 and 5 of each line: mount ID, parent ID, mount point. For
// /proc/mounts (ids == 0), field 2 is the mount point.
static struct table read_table(const char *path, int ids) {
    struct table t = {};
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return t;
    size_t cap = 16;
    t.e = calloc(cap, sizeof(*t.e));
    char line[4096];
    while (t.e != NULL && fgets(line, sizeof(line), f) != NULL) {
        struct entry en = {};
        int ok = ids
            ? sscanf(line, "%d %d %*s %*s %1023s", &en.id, &en.parent, en.point) == 3
            : sscanf(line, "%*s %1023s", en.point) == 1;
        if (!ok)
            continue;
        unescape(en.point);
        if (t.n == cap) {
            cap *= 2;
            struct entry *bigger = realloc(t.e, cap * sizeof(*t.e));
            if (bigger == NULL)
                break;
            t.e = bigger;
        }
        t.e[t.n++] = en;
    }
    fclose(f);
    return t;
}

static long find_id(const struct table *t, int id) {
    for (size_t i = 0; i < t->n; i++)
        if (t->e[i].id == id)
            return (long) i;
    return -1;
}

// The LAST entry with this mount point: the one on top, as a reader takes it.
static long find_point(const struct table *t, const char *point) {
    long found = -1;
    for (size_t i = 0; i < t->n; i++)
        if (strcmp(t->e[i].point, point) == 0)
            found = (long) i;
    return found;
}

// Is `outer` a mount point containing `inner` (equal, or a directory above)?
static int point_contains(const char *outer, const char *inner) {
    size_t n = strlen(outer);
    if (strcmp(outer, "/") == 0)
        return inner[0] == '/';
    return strncmp(outer, inner, n) == 0 && (inner[n] == '\0' || inner[n] == '/');
}

// libmount's mnt_table_get_root_fs, step for step, but bounded: the entry
// with the smallest parent ID, then up through parents until the parent is
// missing or is the entry itself. -1 if the climb does not end.
static long libmount_root(const struct table *t) {
    long root = -1;
    for (size_t i = 0; i < t->n; i++)
        if (root < 0 || t->e[i].parent < t->e[root].parent)
            root = (long) i;
    for (size_t steps = 0; root >= 0 && steps <= t->n; steps++) {
        long up = find_id(t, t->e[root].parent);
        if (up < 0 || up == root)
            return root;
        root = up;
    }
    return -1;
}

static void check_tree(const struct table *t, const char *when) {
    check(t->n > 0, "%s: mountinfo has entries", when);
    if (t->n == 0)
        return;
    for (size_t i = 0; i < t->n; i++)
        for (size_t j = i + 1; j < t->n; j++)
            check(t->e[i].id != t->e[j].id, "%s: mount ID %d is unique (%s, %s)",
                  when, t->e[i].id, t->e[i].point, t->e[j].point);

    long root = libmount_root(t);
    check(root >= 0, "%s: libmount's root search (smallest parent ID, then up) ends", when);
    if (root < 0)
        return;
    const struct entry *r = &t->e[root];
    check(strcmp(r->point, "/") == 0, "%s: that root is \"/\" (got \"%s\", id %d)",
          when, r->point, r->id);
    check(r->parent == r->id || find_id(t, r->parent) < 0,
          "%s: the root's parent %d is itself or not in the table", when, r->parent);

    for (size_t i = 0; i < t->n; i++) {
        long at = (long) i;
        size_t steps = 0;
        for (;;) {
            long up = find_id(t, t->e[at].parent);
            if (up < 0 || up == at)
                break;
            if (++steps > t->n)
                break;
            at = up;
        }
        check(steps <= t->n && at == root,
              "%s: %s (id %d) reaches the root through its parents (stopped at id %d%s)",
              when, t->e[i].point, t->e[i].id, t->e[at].id, steps > t->n ? ", looping" : "");
        long p = find_id(t, t->e[i].parent);
        if ((long) i != root && p >= 0)
            check(point_contains(t->e[p].point, t->e[i].point),
                  "%s: parent %s (id %d) contains %s (id %d)", when, t->e[p].point,
                  t->e[p].id, t->e[i].point, t->e[i].id);
    }
}

// Every mount in `before` is still there under the same ID. One that is gone
// was unmounted by somebody else; an ID now naming a DIFFERENT mount is the
// bug -- IDs were list positions, and moved.
static void check_ids_kept(const struct table *before, const struct table *after,
        const char *when) {
    for (size_t i = 0; i < before->n; i++) {
        long j = find_id(after, before->e[i].id);
        if (j < 0) {
            test_logf("%s: %s (id %d) is gone\n", when, before->e[i].point, before->e[i].id);
            continue;
        }
        check(strcmp(after->e[j].point, before->e[i].point) == 0,
              "%s: id %d still names %s (now %s)", when, before->e[i].id, before->e[i].point,
              after->e[j].point);
    }
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

// statx's mount ID for a path: -1 with errno ENOSYS on i386 AOK, which makes
// statx ENOSYS on purpose (see statx_mnt_id_timerfd).
static long long statx_mnt_id(const char *path) {
#ifdef SYS_statx
    struct statx_min sx = {};
    if (syscall(SYS_statx, AT_FDCWD, path, 0, STATX_MNT_ID, &sx) < 0)
        return -1;
    if (!(sx.mask & STATX_MNT_ID)) {
        errno = ENOSYS;
        return -1;
    }
    return (long long) sx.mnt_id;
#else
    (void) path;
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

static void free_table(struct table *t) {
    free(t->e);
    *t = (struct table) {};
}

// The mounts this test makes, and what it can say about them.
static void check_own_mounts(void) {
    char dir[] = "/tmp/mitree.XXXXXX";
    if (mkdtemp(dir) == NULL) {
        check(0, "mkdtemp (%s)", strerror(errno));
        return;
    }
    char sub[sizeof(dir) + 8];
    snprintf(sub, sizeof(sub), "%s/sub", dir);

    struct table t0 = read_table("/proc/self/mountinfo", 1);
    if (mount("tmpfs", dir, "tmpfs", 0, "size=1m") != 0) {
        int err = errno;
        rmdir(dir);
        free_table(&t0);
        if (err == EPERM) {
            test_logf("mount: EPERM, not privileged: own-mount checks skipped\n");
            return;
        }
        check(0, "mount tmpfs A on %s (%s)", dir, strerror(err));
        return;
    }

    struct table t1 = read_table("/proc/self/mountinfo", 1);
    long a = find_point(&t1, dir);
    check(a >= 0, "tmpfs A on %s is listed", dir);
    // The mount it sits in: the topmost mount whose point contains dir,
    // before A existed.
    long home = -1;
    for (size_t i = 0; i < t0.n; i++)
        if (point_contains(t0.e[i].point, dir) &&
                (home < 0 || strlen(t0.e[i].point) >= strlen(t0.e[home].point)))
            home = (long) i;
    if (a >= 0 && home >= 0)
        check(t1.e[a].parent == t0.e[home].id, "A's parent %d is %s (id %d), the mount it is on",
              t1.e[a].parent, t0.e[home].point, t0.e[home].id);
    check_ids_kept(&t0, &t1, "after mounting A");
    int a_id = a >= 0 ? t1.e[a].id : -1;

    int b_ok = mount("tmpfs", dir, "tmpfs", 0, "size=1m") == 0;
    check(b_ok, "mount tmpfs B on top of A (%s)", strerror(errno));
    struct table t2 = read_table("/proc/self/mountinfo", 1);
    int b_id = -1;
    if (b_ok) {
        long b = find_point(&t2, dir);
        long a2 = find_id(&t2, a_id);
        check(b >= 0 && t2.e[b].id != a_id, "B is the last entry listed on %s", dir);
        check(a2 >= 0 && b > a2, "A (id %d) is listed before B, the later mount", a_id);
        if (b >= 0) {
            b_id = t2.e[b].id;
            check(t2.e[b].parent == a_id, "B's parent %d is A (id %d)", t2.e[b].parent, a_id);
        }
    }
    check_ids_kept(&t1, &t2, "after mounting B");

    int c_ok = mkdir(sub, 0755) == 0 && mount("tmpfs", sub, "tmpfs", 0, "size=1m") == 0;
    check(c_ok, "mount tmpfs C on %s (%s)", sub, strerror(errno));
    struct table t3 = read_table("/proc/self/mountinfo", 1);
    int c_id = -1;
    if (c_ok) {
        long c = find_point(&t3, sub);
        check(c >= 0, "C is listed on %s", sub);
        if (c >= 0) {
            c_id = t3.e[c].id;
            check(t3.e[c].parent == b_id, "C's parent %d is B (id %d)", t3.e[c].parent, b_id);
        }
    }
    check_ids_kept(&t2, &t3, "after mounting C");
    check_tree(&t3, "with A, B and C");

    // The same IDs through the two other doors to them.
    long long sx = statx_mnt_id(dir);
    if (sx < 0 && errno == ENOSYS) {
        test_logf("statx: ENOSYS, statx checks skipped\n");
    } else {
        check(sx == b_id, "statx(%s) mnt_id %lld is B (id %d), the mount on top", dir, sx, b_id);
        sx = statx_mnt_id(sub);
        check(sx == c_id, "statx(%s) mnt_id %lld is C (id %d)", sub, sx, c_id);
    }
    char file[sizeof(sub) + 8];
    snprintf(file, sizeof(file), "%s/f", sub);
    int fd = open(file, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    check(fd >= 0, "create %s (%s)", file, strerror(errno));
    if (fd >= 0) {
        int fid = fdinfo_mnt_id(fd);
        check(fid == c_id, "fdinfo mnt_id %d of a file on C is C (id %d)", fid, c_id);
        close(fd);
        unlink(file);
    }

    if (c_ok)
        check(umount(sub) == 0, "umount C (%s)", strerror(errno));
    if (b_ok)
        check(umount(dir) == 0, "umount B (%s)", strerror(errno));
    check(umount(dir) == 0, "umount A (%s)", strerror(errno));
    rmdir(dir);
    struct table t4 = read_table("/proc/self/mountinfo", 1);
    check(find_id(&t4, a_id) < 0 && (b_id < 0 || find_id(&t4, b_id) < 0) &&
          (c_id < 0 || find_id(&t4, c_id) < 0), "A, B and C are gone after umount");
    check_ids_kept(&t0, &t4, "after unmounting all three");
    check_tree(&t4, "after unmounting");
    free_table(&t0);
    free_table(&t1);
    free_table(&t2);
    free_table(&t3);
    free_table(&t4);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));

    struct table mi = read_table("/proc/self/mountinfo", 1);
    check_tree(&mi, "as found");

    // The two files walk the same table in the same order.
    struct table pm = read_table("/proc/mounts", 0);
    int same = pm.n == mi.n;
    for (size_t i = 0; same && i < mi.n; i++)
        same = strcmp(pm.e[i].point, mi.e[i].point) == 0;
    check(same, "/proc/mounts lists mountinfo's %zu mounts in the same order (has %zu)",
          mi.n, pm.n);
    free_table(&pm);
    free_table(&mi);

    // In a chroot whose /proc is bound in from the booted root, the table
    // describes mounts under paths this process cannot name.
    if (test_in_foreign_proc_chroot()) {
        test_logf("chroot with a foreign /proc: own-mount checks skipped\n");
        return finish_suite(TEST_NAME);
    }

    // The root, by all three names for its ID. fdinfo said 1 for every file,
    // and 1 was whichever mount the list happened to hold first.
    mi = read_table("/proc/self/mountinfo", 1);
    long root = find_point(&mi, "/");
    int root_id = root >= 0 ? mi.e[root].id : -1;
    int rootfd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    check(rootfd >= 0, "open / (%s)", strerror(errno));
    if (rootfd >= 0) {
        int fid = fdinfo_mnt_id(rootfd);
        check(fid == root_id, "fdinfo mnt_id %d of / is the root mount (id %d)", fid, root_id);
        close(rootfd);
    }
    long long sx = statx_mnt_id("/");
    if (!(sx < 0 && errno == ENOSYS))
        check(sx == root_id, "statx(/) mnt_id %lld is the root mount (id %d)", sx, root_id);
    // A pipe is on no mount anyone can see (Linux: pipefs's, internal).
    int pipefd[2];
    if (pipe(pipefd) == 0) {
        int fid = fdinfo_mnt_id(pipefd[0]);
        check(fid > 0 && find_id(&mi, fid) < 0, "fdinfo mnt_id %d of a pipe names no listed mount",
              fid);
        close(pipefd[0]);
        close(pipefd[1]);
    }
    free_table(&mi);

    check_own_mounts();

    return finish_suite(TEST_NAME);
}
