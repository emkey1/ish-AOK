// (st_dev, st_ino) must uniquely identify a file ACROSS mounts, not just
// within one.
//
// History: fakefs keeps its metadata in SQLite and hands out its own inode
// numbers from that database, but it built the rest of the statbuf by calling
// through to realfs -- so st_ino came from SQLite while st_dev came from the
// HOST volume the backing store happened to sit on. Two consequences, both
// bad:
//
//   * every fakefs mount on one host volume reported the SAME st_dev, and
//   * each mount numbers its own inodes independently, starting from 1.
//
// So the pair collided constantly between roots. The report that opened this:
// three different files, one in each of three /AOK/roots mounts, ALL reported
// dev=265 ino=308. The sharpest case needs no coincidence at all -- every
// fakefs database numbers its root directory inode 1, so the root directories
// of any two fakefs mounts were reported as literally the same file.
//
// That is silent data loss, because the entire userland treats an equal
// (dev, ino) pair as "same file" and skips the work: cp refuses with
// "'x' and 'y' are the same file" and copies nothing, and mv, rsync, install,
// make and python's os.path.samefile all take the same shortcut. Measured on
// an unfixed build, between two ordinary Alpine roots: six colliding pairs in
// /etc alone, and `cp` across them failing while exiting nonzero into scripts
// that rarely check.
//
// Fixed by leaving stat.dev at 0 in fakefs and letting generic_statat /
// generic_fstat stamp mount->fake_dev, the per-mount anonymous device (Linux
// 0:xx) that the mount_stdev work already allocates for every filesystem
// without a real backing device. The inode and the device then name the same
// namespace again.
//
// Needs a SECOND mount to compare against; how it is found, in order:
//   1. $ISH_TEST_SECOND_MOUNT -- an existing directory on another mount.
//   2. /fakemnt2 -- the CLI repro rig (run ish with ISH_FAKE_MNT2=<root dir>).
//   3. /AOK/roots/<name>/data mounted read-write at a temp point, as root.
// Skips cleanly when none is available. Cannot run on real Linux as written
// (there is no second fakefs there), but the invariant it asserts is a plain
// POSIX one, so the generic form via $ISH_TEST_SECOND_MOUNT does run there
// against any second filesystem.
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "test_common.h"

static void check(int cond, const char *what) {
    if (cond) {
        test_logf("ok: %s\n", what);
    } else {
        printf("FAIL: %s (errno=%d %s)\n", what, errno, strerror(errno));
        failures_total++;
    }
}

#define SCRATCH_FILES 48

// Where the second mount came from, so a skip or a failure names itself.
static char second_mount[PATH_MAX];
static char mounted_here[PATH_MAX]; // non-empty if we must umount on the way out

static int same_file(const struct stat *a, const struct stat *b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino;
}

// Deciding "is this a separate mount?" must NOT use st_dev: st_dev is the
// value under test, and on an unfixed build every fakefs mount shares one, so
// an st_dev-based probe concludes there is no second mount and the whole test
// skips itself exactly when it should fail. Ask /proc/self/mountinfo instead.
// Line shape: id parent maj:min root POINT opts... - fstype SOURCE superopts.
// Returns 1 if `point` is a mount point; when `source_out` is non-NULL it also
// copies that mount's source.
static int mountinfo_lookup(const char *point, char *source_out, size_t source_len) {
    FILE *f = fopen("/proc/self/mountinfo", "r");
    if (f == NULL)
        return 0;
    char line[4096];
    int found = 0;
    while (!found && fgets(line, sizeof(line), f) != NULL) {
        char *save = NULL;
        char *tok = strtok_r(line, " \n", &save);
        char *field[32];
        unsigned n = 0;
        while (tok != NULL && n < 32) {
            field[n++] = tok;
            tok = strtok_r(NULL, " \n", &save);
        }
        if (n < 6 || strcmp(field[4], point) != 0)
            continue;
        found = 1;
        if (source_out == NULL)
            break;
        source_out[0] = '\0';
        for (unsigned i = 5; i + 2 < n; i++)
            if (strcmp(field[i], "-") == 0) {
                snprintf(source_out, source_len, "%s", field[i + 2]);
                break;
            }
    }
    fclose(f);
    return found;
}

// Try /AOK/roots/<name>/data -- the on-device shape, where each installed root
// is a fakefs whose backing dir the app exposes read-write. Skips the root we
// are booted from: its data dir is already mounted at /, and mounting it a
// second time would compare a filesystem against itself.
static int try_mount_aok_root(void) {
    DIR *d = opendir("/AOK/roots");
    if (d == NULL)
        return -1;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.')
            continue;
        char src[PATH_MAX], point[PATH_MAX];
        snprintf(src, sizeof(src), "/AOK/roots/%s/data", ent->d_name);
        struct stat src_st;
        if (stat(src, &src_st) != 0 || !S_ISDIR(src_st.st_mode))
            continue;

        snprintf(point, sizeof(point), "/tmp/crossdev-mnt.%d", (int) getpid());
        if (mkdir(point, 0700) != 0 && errno != EEXIST)
            continue;
        if (mount(src, point, "fake", 0, NULL) != 0) {
            rmdir(point);
            continue;
        }
        // A root that turns out to BE the booted one is useless here: same
        // superblock, so sharing a device with / is correct, not a bug.
        // Compared by mount source, not st_dev, for the reason above.
        char root_src[PATH_MAX], cand_src[PATH_MAX];
        if (mountinfo_lookup("/", root_src, sizeof(root_src)) &&
                mountinfo_lookup(point, cand_src, sizeof(cand_src)) &&
                strcmp(root_src, cand_src) == 0) {
            umount(point);
            rmdir(point);
            continue;
        }
        snprintf(second_mount, sizeof(second_mount), "%s", point);
        snprintf(mounted_here, sizeof(mounted_here), "%s", point);
        test_logf("mounted %s at %s\n", src, point);
        closedir(d);
        return 0;
    }
    closedir(d);
    return -1;
}

static int find_second_mount(void) {
    const char *env = getenv("ISH_TEST_SECOND_MOUNT");
    if (env != NULL && env[0] != '\0') {
        struct stat st;
        if (stat(env, &st) != 0 || !S_ISDIR(st.st_mode)) {
            printf("FAIL: ISH_TEST_SECOND_MOUNT=%s is not a directory\n", env);
            failures_total++;
            return -1;
        }
        snprintf(second_mount, sizeof(second_mount), "%s", env);
        return 0;
    }

    struct stat st;
    if (stat("/fakemnt2", &st) == 0 && S_ISDIR(st.st_mode) &&
            mountinfo_lookup("/fakemnt2", NULL, 0)) {
        snprintf(second_mount, sizeof(second_mount), "/fakemnt2");
        return 0;
    }

    return try_mount_aok_root();
}

static void cleanup_mount(void) {
    if (mounted_here[0] == '\0')
        return;
    umount(mounted_here);
    rmdir(mounted_here);
}

// The content-independent case: two mount ROOTS. Every fakefs database
// numbers its root directory inode 1, so before the fix these two stats were
// bit-identical in both fields and the whole userland called them one file.
static void test_mount_roots_distinct(void) {
    struct stat a, b;
    check(stat("/", &a) == 0, "stat /");
    check(stat(second_mount, &b) == 0, "stat second mount root");
    test_logf("/ dev=%llu ino=%llu  %s dev=%llu ino=%llu\n",
            (unsigned long long) a.st_dev, (unsigned long long) a.st_ino,
            second_mount,
            (unsigned long long) b.st_dev, (unsigned long long) b.st_ino);
    check(a.st_dev != b.st_dev, "two mounts report different st_dev");
    check(!same_file(&a, &b), "two mount roots are not reported as the same file");
}

// Files, not just mount points: the dev a file reports must be its mount's,
// and fstat must agree with stat.
static void test_files_inherit_mount_dev(const char *dir_a, const char *dir_b) {
    char pa[PATH_MAX], pb[PATH_MAX];
    snprintf(pa, sizeof(pa), "%s/probe", dir_a);
    snprintf(pb, sizeof(pb), "%s/probe", dir_b);

    struct stat mnt_a, mnt_b, fa, fb;
    check(stat("/", &mnt_a) == 0, "stat / for file-dev comparison");
    check(stat(second_mount, &mnt_b) == 0, "stat second mount for file-dev comparison");
    check(stat(pa, &fa) == 0, "stat probe file on /");
    check(stat(pb, &fb) == 0, "stat probe file on second mount");
    check(fa.st_dev == mnt_a.st_dev, "file on / carries /'s st_dev");
    check(fb.st_dev == mnt_b.st_dev, "file on second mount carries its st_dev");
    check(!same_file(&fa, &fb), "probe files on different mounts are not the same file");

    int fd = open(pb, O_RDONLY);
    check(fd >= 0, "open probe on second mount");
    if (fd >= 0) {
        struct stat st;
        check(fstat(fd, &st) == 0, "fstat probe on second mount");
        check(st.st_dev == fb.st_dev, "fstat st_dev agrees with stat");
        check(st.st_ino == fb.st_ino, "fstat st_ino agrees with stat");
        close(fd);
    }
}

// The literal invariant: over a batch of files on each mount, no (dev, ino)
// pair may appear on both. Sequentially-created files land in each mount's own
// low inode range, which is exactly where two independent databases overlap.
static void test_no_colliding_pairs(const char *dir_a, const char *dir_b) {
    struct stat a[SCRATCH_FILES], b[SCRATCH_FILES];
    unsigned na = 0, nb = 0;

    for (unsigned i = 0; i < SCRATCH_FILES; i++) {
        char p[PATH_MAX];
        snprintf(p, sizeof(p), "%s/f%u", dir_a, i);
        if (stat(p, &a[na]) == 0)
            na++;
        snprintf(p, sizeof(p), "%s/f%u", dir_b, i);
        if (stat(p, &b[nb]) == 0)
            nb++;
    }
    check(na == SCRATCH_FILES, "all scratch files on / stat'd");
    check(nb == SCRATCH_FILES, "all scratch files on second mount stat'd");

    unsigned collisions = 0;
    for (unsigned i = 0; i < na; i++)
        for (unsigned j = 0; j < nb; j++)
            if (same_file(&a[i], &b[j])) {
                collisions++;
                test_log_if(1, "collide: %s/f%u and %s/f%u both dev=%llu ino=%llu\n",
                        dir_a, i, dir_b, j,
                        (unsigned long long) a[i].st_dev,
                        (unsigned long long) a[i].st_ino);
            }
    check(collisions == 0, "no (dev, ino) pair is shared across the two mounts");
}

// Ordinary files within ONE mount must still be distinguishable -- a fix that
// handed every file the same inode would pass the cross-mount checks above.
static void test_within_mount_still_unique(const char *dir_a) {
    struct stat x, y;
    char px[PATH_MAX], py[PATH_MAX];
    snprintf(px, sizeof(px), "%s/f0", dir_a);
    snprintf(py, sizeof(py), "%s/f1", dir_a);
    check(stat(px, &x) == 0 && stat(py, &y) == 0, "stat two files on one mount");
    check(!same_file(&x, &y), "two files on one mount are not the same file");

    // ...and a hard link IS the same file, which is what the pair is for.
    char link_path[PATH_MAX];
    snprintf(link_path, sizeof(link_path), "%s/f0.link", dir_a);
    unlink(link_path);
    if (link(px, link_path) == 0) {
        struct stat l;
        check(stat(link_path, &l) == 0, "stat hard link");
        check(same_file(&x, &l), "a hard link IS reported as the same file");
        unlink(link_path);
    } else {
        test_logf("skip: hard link unsupported here (errno=%d)\n", errno);
    }
}

static int run(const char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0 && !test_verbose) {
            dup2(devnull, 1);
            dup2(devnull, 2);
        }
        execv(argv[0], (char *const *) argv);
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int file_contains(const char *path, const char *want) {
    char buf[256];
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n < 0)
        return 0;
    buf[n] = '\0';
    return strstr(buf, want) != NULL;
}

// The user-visible half: coreutils short-circuits on the pair, so a collision
// turns into a copy that silently does not happen.
static void test_cp_mv_across_mounts(const char *dir_a, const char *dir_b) {
    const char *cp = access("/bin/cp", X_OK) == 0 ? "/bin/cp" : NULL;
    const char *mv = access("/bin/mv", X_OK) == 0 ? "/bin/mv" : NULL;
    if (cp == NULL || mv == NULL) {
        test_logf("skip: /bin/cp or /bin/mv unavailable\n");
        return;
    }

    char src[PATH_MAX], dst[PATH_MAX];
    snprintf(src, sizeof(src), "%s/xfer-src", dir_a);
    snprintf(dst, sizeof(dst), "%s/xfer-dst", dir_b);

    int fd = open(src, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    check(fd >= 0, "create cp source");
    if (fd >= 0) {
        check(write(fd, "NEWDATA\n", 8) == 8, "write cp source");
        close(fd);
    }
    fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    check(fd >= 0, "create cp destination");
    if (fd >= 0) {
        check(write(fd, "STALEDATA\n", 10) == 10, "write cp destination");
        close(fd);
    }

    const char *cp_argv[] = {cp, src, dst, NULL};
    check(run(cp_argv) == 0, "cp across mounts succeeds");
    check(file_contains(dst, "NEWDATA"), "cp across mounts actually replaced the destination");

    // mv: destination must end up with the source's bytes, source must be gone.
    const char *mv_argv[] = {mv, src, dst, NULL};
    check(run(mv_argv) == 0, "mv across mounts succeeds");
    check(file_contains(dst, "NEWDATA"), "mv across mounts left the source's content at the destination");
    check(access(src, F_OK) != 0, "mv across mounts removed the source");

    unlink(dst);
}

static void make_scratch(const char *dir, char tag) {
    mkdir(dir, 0700);
    for (unsigned i = 0; i < SCRATCH_FILES; i++) {
        char p[PATH_MAX];
        snprintf(p, sizeof(p), "%s/f%u", dir, i);
        int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            char buf[32];
            int n = snprintf(buf, sizeof(buf), "%c%u\n", tag, i);
            ssize_t w = write(fd, buf, (size_t) n);
            (void) w;
            close(fd);
        }
    }
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s/probe", dir);
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0)
        close(fd);
}

static void remove_scratch(const char *dir) {
    for (unsigned i = 0; i < SCRATCH_FILES; i++) {
        char p[PATH_MAX];
        snprintf(p, sizeof(p), "%s/f%u", dir, i);
        unlink(p);
    }
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s/probe", dir);
    unlink(p);
    snprintf(p, sizeof(p), "%s/xfer-src", dir);
    unlink(p);
    snprintf(p, sizeof(p), "%s/xfer-dst", dir);
    unlink(p);
    rmdir(dir);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(120));

    if (find_second_mount() != 0) {
        if (failures_total != 0)
            return finish_suite("mount_cross_dev");
        printf("mount_cross_dev: SKIP (no second mount; set ISH_TEST_SECOND_MOUNT, "
               "run ish with ISH_FAKE_MNT2, or install a second /AOK/roots root)\n");
        return 0;
    }
    test_logf("second mount: %s\n", second_mount);

    // Both scratch dirs are built directly under their mount point, so each is
    // on the mount it is meant to test by construction. Deliberately NOT /tmp:
    // /tmp is part of the root fakefs on the CLI rig but a separate tmpfs on
    // device, and tmpfs already had a correct anonymous dev -- putting side A
    // there would quietly turn this into a tmpfs-vs-fakefs comparison that
    // passes on an unfixed build.
    char dir_a[PATH_MAX], dir_b[PATH_MAX];
    snprintf(dir_a, sizeof(dir_a), "/crossdev-a.%d", (int) getpid());
    snprintf(dir_b, sizeof(dir_b), "%s/crossdev-b.%d", second_mount, (int) getpid());

    if (mkdir(dir_a, 0700) != 0 && errno != EEXIST) {
        printf("mount_cross_dev: SKIP (cannot create %s: %s)\n", dir_a, strerror(errno));
        cleanup_mount();
        return 0;
    }
    make_scratch(dir_a, 'A');
    make_scratch(dir_b, 'B');

    test_mount_roots_distinct();
    test_files_inherit_mount_dev(dir_a, dir_b);
    test_no_colliding_pairs(dir_a, dir_b);
    test_within_mount_still_unique(dir_a);
    test_cp_mv_across_mounts(dir_a, dir_b);

    remove_scratch(dir_a);
    remove_scratch(dir_b);
    cleanup_mount();
    return finish_suite("mount_cross_dev");
}
