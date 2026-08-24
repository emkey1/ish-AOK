// st_dev semantics for virtual filesystem mounts (tmpfs, proc, binds).
//
// History: iSH's tmpfs/proc/devpts/sysfs inodes never set stat.dev, so every
// virtual mount reported st_dev=0. busybox df matches a path to a mount by
// comparing st_dev of the path against st_dev of each mountpoint in
// /proc/mounts; with everything at dev 0 the LAST 0-dev mount won, so
// `df /tmp` printed the devpts line (0 blocks) even though statfs("/tmp")
// itself was correct. Same-filesystem checks (find -xdev, du -x) also could
// not tell virtual mounts apart. Fixed by allocating a unique anonymous
// device (Linux 0:xx semantics) per mount at mount time and stamping it into
// stat results whenever the filesystem leaves stat.dev at 0; bind mounts
// share their origin's device, matching Linux's shared-superblock rule.
// Ground-truthed against a real kernel: each tmpfs/proc/devpts mount gets a
// distinct anon dev with major 0, and a bind of a tmpfs reports the origin's.
//
// The same device has to be reported by the two places that name it, so this
// also checks /proc/self/mountinfo's field 3 (major:minor) against st_dev of
// each mount point. iSH printed a hardcoded "0:0" there, which agreed with
// nothing once every mount got its own anonymous device -- `stat /` said 0:15
// while the mountinfo line for / still said 0:0.
//
// Needs root to mount private tmpfs instances; mount-dependent checks are
// skipped when mounting fails. Also passes on real Linux.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include "test_common.h"

#ifndef SYS_memfd_create
#if defined(__i386__)
#define SYS_memfd_create 356
#elif defined(__x86_64__)
#define SYS_memfd_create 319
#else
#define SYS_memfd_create 279
#endif
#endif

static int my_memfd_create(const char *name, unsigned flags) {
    return syscall(SYS_memfd_create, name, flags);
}

static void check(int cond, const char *what) {
    if (cond) {
        test_logf("ok: %s\n", what);
    } else {
        printf("FAIL: %s (errno=%d %s)\n", what, errno, strerror(errno));
        failures_total++;
    }
}

// A /proc/self/mountinfo line, of which two fields matter here:
//   id parent MAJ:MIN root POINT opts... - fstype source superopts
//    0    1     2      3     4     5           n  n+1     n+2
// Field 3 (1-based, field[2] here) is the device files on that mount report
// via stat(2)'s st_dev, and field 5 (field[4]) is where the mount is attached.
struct mountinfo_line {
    char point[PATH_MAX];
    char fstype[64];
    unsigned maj, min;
};

// Splits one line in place. Returns 0 for a line that isn't a mountinfo entry.
static int mountinfo_parse_line(char *line, struct mountinfo_line *out) {
    char *save = NULL;
    char *field[40];
    unsigned n = 0;
    for (char *tok = strtok_r(line, " \n", &save); tok != NULL && n < 40;
            tok = strtok_r(NULL, " \n", &save))
        field[n++] = tok;
    if (n < 7)
        return 0;
    if (sscanf(field[2], "%u:%u", &out->maj, &out->min) != 2)
        return 0;
    snprintf(out->point, sizeof(out->point), "%s", field[4]);
    // The optional-fields run between the options and the "-" separator is
    // variable length, so the fstype is found relative to the separator.
    out->fstype[0] = '\0';
    for (unsigned i = 5; i + 1 < n; i++)
        if (strcmp(field[i], "-") == 0) {
            snprintf(out->fstype, sizeof(out->fstype), "%s", field[i + 1]);
            break;
        }
    return 1;
}

// The whole table in one snapshot: the checks below need to know whether a
// mount point appears on more than one line, which a line-at-a-time read
// can't answer. Caller frees; *count_out is 0 on failure.
static struct mountinfo_line *mountinfo_read_all(size_t *count_out) {
    *count_out = 0;
    FILE *f = fopen("/proc/self/mountinfo", "r");
    if (f == NULL)
        return NULL;
    size_t cap = 64, n = 0;
    struct mountinfo_line *lines = malloc(cap * sizeof(*lines));
    char buf[8192];
    while (lines != NULL && fgets(buf, sizeof(buf), f) != NULL) {
        if (n == cap) {
            struct mountinfo_line *bigger = realloc(lines, cap * 2 * sizeof(*lines));
            if (bigger == NULL)
                break;
            lines = bigger;
            cap *= 2;
        }
        if (mountinfo_parse_line(buf, &lines[n]))
            n++;
    }
    fclose(f);
    *count_out = n;
    return lines;
}

// The device field of the (topmost, i.e. last) line mounted at `point`.
static int mountinfo_dev_for_point(const char *point, unsigned *maj, unsigned *min) {
    size_t n = 0;
    struct mountinfo_line *lines = mountinfo_read_all(&n);
    int found = 0;
    for (size_t i = 0; i < n; i++)
        if (strcmp(lines[i].point, point) == 0) {
            *maj = lines[i].maj;
            *min = lines[i].min;
            found = 1;
        }
    free(lines);
    return found;
}

// Two private tmpfs mounts: nonzero anon devs (major 0), distinct from each
// other, from /, and from /proc; files inherit the mount's dev; fstat agrees
// with stat; a bind mount shares the origin's dev.
static void test_tmpfs_devs(void) {
    char dir_a[128], dir_b[128], dir_bind[128];
    snprintf(dir_a, sizeof(dir_a), "/tmp/stdev-a.%d", (int) getpid());
    snprintf(dir_b, sizeof(dir_b), "/tmp/stdev-b.%d", (int) getpid());
    snprintf(dir_bind, sizeof(dir_bind), "/tmp/stdev-bind.%d", (int) getpid());

    if (mkdir(dir_a, 0700) != 0 || mount("tmpfs", dir_a, "tmpfs", 0, NULL) != 0) {
        test_logf("skip: cannot mount private tmpfs (errno=%d)\n", errno);
        rmdir(dir_a);
        return;
    }
    struct stat root_st, proc_st, a_st, b_st;
    check(stat("/", &root_st) == 0, "stat /");
    check(stat("/proc", &proc_st) == 0, "stat /proc");
    check(stat(dir_a, &a_st) == 0, "stat tmpfs A");
    check(a_st.st_dev != 0, "tmpfs A has nonzero st_dev");
    check(major(a_st.st_dev) == 0, "tmpfs A st_dev is anonymous (major 0)");
    check(a_st.st_dev != root_st.st_dev, "tmpfs A st_dev != root's");

    check(proc_st.st_dev != 0, "/proc has nonzero st_dev");
    check(proc_st.st_dev != a_st.st_dev, "/proc st_dev != tmpfs A's");

    if (mkdir(dir_b, 0700) == 0 && mount("tmpfs", dir_b, "tmpfs", 0, NULL) == 0) {
        check(stat(dir_b, &b_st) == 0, "stat tmpfs B");
        check(b_st.st_dev != 0, "tmpfs B has nonzero st_dev");
        check(b_st.st_dev != a_st.st_dev, "two tmpfs mounts get distinct st_dev");
        umount(dir_b);
    } else {
        test_logf("skip: second tmpfs mount failed (errno=%d)\n", errno);
    }
    rmdir(dir_b);

    // a file lives on its mount's device, and fstat matches stat
    char file_a[160];
    snprintf(file_a, sizeof(file_a), "%s/f", dir_a);
    int fd = open(file_a, O_CREAT | O_RDWR, 0600);
    check(fd >= 0, "create file on tmpfs A");
    if (fd >= 0) {
        struct stat f_st, ffd_st;
        check(stat(file_a, &f_st) == 0 && f_st.st_dev == a_st.st_dev,
              "file inherits the mount's st_dev");
        check(fstat(fd, &ffd_st) == 0 && ffd_st.st_dev == a_st.st_dev,
              "fstat st_dev matches the mount");
        close(fd);
    }

    // ...and mountinfo's device field names that same device, for a mount
    // made right here rather than whatever the sweep below happens to find.
    unsigned maj, min;
    if (mountinfo_dev_for_point(dir_a, &maj, &min)) {
        check(maj == major(a_st.st_dev) && min == minor(a_st.st_dev),
              "tmpfs A mountinfo device field == its st_dev");
    } else {
        test_logf("skip: no mountinfo line for tmpfs A\n");
    }

    // bind mount shares the origin's device (same superblock on Linux)
    if (mkdir(dir_bind, 0700) == 0 && mount(dir_a, dir_bind, NULL, MS_BIND, NULL) == 0) {
        struct stat bind_st, bind_f_st;
        char file_bind[192];
        snprintf(file_bind, sizeof(file_bind), "%s/f", dir_bind);
        check(stat(dir_bind, &bind_st) == 0 && bind_st.st_dev == a_st.st_dev,
              "bind mount shares the origin's st_dev");
        check(stat(file_bind, &bind_f_st) == 0 && bind_f_st.st_dev == a_st.st_dev,
              "file via bind path has the origin's st_dev");
        if (mountinfo_dev_for_point(dir_bind, &maj, &min)) {
            check(maj == major(a_st.st_dev) && min == minor(a_st.st_dev),
                  "bind mountinfo device field == the origin's st_dev");
        } else {
            test_logf("skip: no mountinfo line for the bind\n");
        }
        umount(dir_bind);
    } else {
        test_logf("skip: bind mount failed (errno=%d)\n", errno);
    }
    rmdir(dir_bind);

    unlink(file_a);
    umount(dir_a);
    rmdir(dir_a);
}

// fd types with no path also live on anonymous devices on Linux (pipefs,
// the internal shm tmpfs for memfds) — st_dev must not be 0.
static void test_anon_fd_devs(void) {
    int pipefd[2];
    if (pipe(pipefd) == 0) {
        struct stat st;
        check(fstat(pipefd[0], &st) == 0 && st.st_dev != 0,
              "pipe has nonzero st_dev");
        close(pipefd[0]);
        close(pipefd[1]);
    }

    int mfd = my_memfd_create("stdev-test", 0);
    if (mfd >= 0) {
        struct stat st;
        check(fstat(mfd, &st) == 0 && st.st_dev != 0,
              "memfd has nonzero st_dev");
        close(mfd);
    } else {
        test_logf("skip: memfd_create unavailable (errno=%d)\n", errno);
    }
}

// Every mount already in the table: mountinfo's device field must be the
// major:minor of st_dev of that mount's own mount point. The two are the same
// number on Linux by construction -- field 3 is defined as the device of the
// filesystem mounted there -- but iSH printed a literal "0:0" on every line,
// which agreed with nothing once each mount got its own anonymous device.
// findmnt and anything else parsing the field saw that wrong number.
static void test_mountinfo_dev_field(void) {
    size_t n = 0;
    struct mountinfo_line *lines = mountinfo_read_all(&n);
    if (n == 0) {
        test_logf("skip: cannot read /proc/self/mountinfo\n");
        free(lines);
        return;
    }

    unsigned checked = 0;
    int checked_root = 0;
    for (size_t i = 0; i < n; i++) {
        const char *point = lines[i].point;
        // A point carrying more than one mount resolves to the topmost one,
        // so stat() cannot say which line it belongs to.
        int overmounted = 0;
        for (size_t j = 0; j < n; j++)
            if (j != i && strcmp(lines[j].point, point) == 0)
                overmounted = 1;
        if (overmounted)
            continue;
        // A point with a space, tab or newline in it is written with octal
        // escapes (\040); un-escaping it is not what this test is about.
        if (strchr(point, '\\') != NULL)
            continue;
        // stat()ing an autofs point triggers the real mount underneath it,
        // whose device is not the one on this line.
        if (strcmp(lines[i].fstype, "autofs") == 0)
            continue;
        // Unreachable or unreadable (EACCES under another user's /run/user).
        struct stat st;
        if (stat(point, &st) != 0)
            continue;

        char what[PATH_MAX + 128];
        snprintf(what, sizeof(what), "%s: mountinfo device %u:%u == st_dev %u:%u",
                 point, lines[i].maj, lines[i].min,
                 (unsigned) major(st.st_dev), (unsigned) minor(st.st_dev));
        check(lines[i].maj == (unsigned) major(st.st_dev) &&
              lines[i].min == (unsigned) minor(st.st_dev), what);
        checked++;
        if (strcmp(point, "/") == 0)
            checked_root = 1;
    }

    // Guard against a vacuous pass: "/" is the reported case (its line said
    // 0:0 while stat / said 0:15) and is never escaped, never overmounted and
    // never unreadable, so a run that skipped it skipped everything that
    // matters. Under iSH the floor is /, /proc and /sys; a real kernel has
    // many more.
    check(checked_root, "the / line was among the lines checked");
    check(checked >= 3, "at least 3 mountinfo lines were checkable");
    test_logf("checked %u of %zu mountinfo lines\n", checked, n);
    free(lines);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    TEST_SKIP_IF_FOREIGN_PROC("mount_stdev");
    test_tmpfs_devs();
    test_anon_fd_devs();
    test_mountinfo_dev_field();
    return finish_suite("mount_stdev");
}
