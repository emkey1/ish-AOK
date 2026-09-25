// An inotify watch is on a file, not on the name it was placed through, so a
// change made through one name of a directory reaches a watch placed through
// any other. A bind mount's point and its source are two names for one
// directory.
//
// Regression for a report. After `mount --bind /tmp/src /tmp/dst`, a watch on
// /tmp/src for IN_CREATE|IN_MODIFY|IN_CLOSE_WRITE saw nothing of /tmp/dst/x
// being created and written, and a watch on /tmp/dst nothing of /tmp/src/x.
// Linux reports all three either way. AOK keyed watches by the path string
// they were added with, and each event carried the one name it was made
// through, so it reached only the watches on that name.
//
// As root, watching through one name and changing through the other, both
// ways:
//   - IN_CREATE, IN_MODIFY and IN_CLOSE_WRITE in a watched directory, with
//     the same name used both ways as the positive control;
//   - IN_OPEN, IN_ACCESS, IN_MODIFY, IN_ATTRIB and both closes on a watched
//     file;
//   - IN_DELETE, and IN_DELETE_SELF + IN_IGNORED on the file's own watch;
//   - IN_MOVED_FROM/IN_MOVED_TO sharing a cookie, IN_MOVE_SELF, and the
//     watches on the renamed file and below a renamed directory following
//     them to their new names -- including a file watch that did not ask
//     for IN_MOVE_SELF, which AOK left on the old name;
//   - one instance watching both names: inotify_add_watch hands back the
//     same wd, a second add replaces the mask and IN_MASK_ADD adds to it, each
//     event arrives once, and rm_watch removes it for both names;
//   - two instances, one per name, each told once;
//   - a bind of the bind;
//   - a watch placed through the bind outlives the bind's umount, and does
//     not see the directory the bind covered;
//   - a watch on a directory that a bind is then mounted over does not see
//     through the bind;
//   - chmod of the bind's point reaches a watch on the source's parent, under
//     the source's name -- while chmod or opendir of a tmpfs's mount point
//     reaches no watch on the directory above it, a filesystem's root having
//     no parent to tell (AOK told it both);
//   - a watch on / hears of a file created in it, which AOK's parent lookup
//     ("/" against a watch kept as "") never matched.
// Unprivileged, the mount is EPERM and only the plain watch is checked.
//
// Oracle: Linux 6.12 (camd), glibc x86_64 and -m32, as root and not.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>
#include "test_common.h"

#define TEST_NAME "inotify_bind_alias"

#ifndef MNT_DETACH
#define MNT_DETACH 2
#endif
#ifndef MS_PRIVATE
#define MS_PRIVATE (1 << 18)
#endif

#define CW (IN_CREATE | IN_MODIFY | IN_CLOSE_WRITE)

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

static char base[] = "/tmp/inba.XXXXXX";
static char src[64], dst[64], dst2[64], pre[64];

static void join(char *out, size_t n, const char *dir, const char *name) {
    snprintf(out, n, "%s/%s", dir, name);
}

struct ev {
    int wd;
    uint32_t mask;
    uint32_t cookie;
    char name[64];
};
static struct ev evs[128];
static int nev;

// Everything queued on `in` right now; the instance is non-blocking.
static void drain(int in) {
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    nev = 0;
    for (;;) {
        ssize_t n = read(in, buf, sizeof(buf));
        if (n <= 0)
            break;
        for (ssize_t off = 0; off < n; ) {
            struct inotify_event *ie = (struct inotify_event *) (buf + off);
            if (nev < (int) (sizeof(evs) / sizeof(evs[0]))) {
                struct ev *e = &evs[nev++];
                e->wd = ie->wd;
                e->mask = ie->mask;
                e->cookie = ie->cookie;
                snprintf(e->name, sizeof(e->name), "%s", ie->len > 0 ? ie->name : "");
            }
            off += sizeof(*ie) + ie->len;
        }
    }
    if (test_verbose)
        for (int i = 0; i < nev; i++)
            printf("  event: wd %d mask %#x cookie %u name '%s'\n", evs[i].wd,
                   (unsigned) evs[i].mask, (unsigned) evs[i].cookie, evs[i].name);
}

// How many drained events on `wd` carry `bit` and are named `name`, "" being
// an event on the watched object itself.
static int count(int wd, uint32_t bit, const char *name) {
    int c = 0;
    for (int i = 0; i < nev; i++)
        if (evs[i].wd == wd && (evs[i].mask & bit) && strcmp(evs[i].name, name) == 0)
            c++;
    return c;
}

static void expect(int wd, uint32_t bit, const char *bit_name, const char *name, int want,
                   const char *how) {
    int got = count(wd, bit, name);
    check(got == want, "%s: %s%s%s x%d (got %d)", how, bit_name, *name ? " " : "", name, want,
          got);
}
#define EXPECT(wd, bit, name, want, how) expect(wd, bit, #bit, name, want, how)

static int instance(void) {
    int in = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    check(in >= 0, "inotify_init1 (%s)", strerror(errno));
    return in;
}

static int watch(int in, const char *path, uint32_t mask) {
    int wd = inotify_add_watch(in, path, mask);
    check(wd >= 0, "inotify_add_watch %s (%s)", path, strerror(errno));
    return wd;
}

// Create `dir`/`name` and write a byte to it: IN_CREATE, IN_MODIFY and
// IN_CLOSE_WRITE in `dir`.
static void create_write(const char *dir, const char *name) {
    char p[128];
    join(p, sizeof(p), dir, name);
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    check(fd >= 0 && write(fd, "x", 1) == 1, "create and write %s (%s)", p, strerror(errno));
    if (fd >= 0)
        close(fd);
}

// The report: watched through one name, changed through another.
static void one_way(const char *watched, const char *through, const char *name,
                    const char *how) {
    int in = instance();
    int wd = watch(in, watched, CW);
    create_write(through, name);
    drain(in);
    EXPECT(wd, IN_CREATE, name, 1, how);
    EXPECT(wd, IN_MODIFY, name, 1, how);
    EXPECT(wd, IN_CLOSE_WRITE, name, 1, how);
    check(nev == 3, "%s: three events in all (got %d)", how, nev);
    close(in);
}

// One instance watching both names.
static void both_names(void) {
    const char *how;
    int in = instance();
    int w1 = watch(in, src, CW);
    int w2 = watch(in, dst, CW);
    check(w1 >= 0 && w1 == w2,
          "one instance: the source and the bind are one watch (wd %d, then %d)", w1, w2);
    create_write(dst, "b1");
    create_write(src, "b2");
    drain(in);
    how = "one instance watching both names, a file made through the bind";
    EXPECT(w1, IN_CREATE, "b1", 1, how);
    EXPECT(w1, IN_MODIFY, "b1", 1, how);
    EXPECT(w1, IN_CLOSE_WRITE, "b1", 1, how);
    how = "one instance watching both names, a file made through the source";
    EXPECT(w1, IN_CREATE, "b2", 1, how);
    EXPECT(w1, IN_MODIFY, "b2", 1, how);
    EXPECT(w1, IN_CLOSE_WRITE, "b2", 1, how);
    check(nev == 6, "one instance watching both names: six events in all (got %d)", nev);

    // A second add of the same directory replaces the mask, whichever name
    // it comes through...
    check(inotify_add_watch(in, dst, IN_MODIFY) == w1,
          "re-adding through the bind returns the same wd (%s)", strerror(errno));
    create_write(src, "b3");
    drain(in);
    how = "after re-adding through the bind for IN_MODIFY alone";
    EXPECT(w1, IN_CREATE, "b3", 0, how);
    EXPECT(w1, IN_MODIFY, "b3", 1, how);
    EXPECT(w1, IN_CLOSE_WRITE, "b3", 0, how);

    // ...and IN_MASK_ADD through the other name adds to it.
    check(inotify_add_watch(in, src, IN_CREATE | IN_MASK_ADD) == w1,
          "IN_MASK_ADD through the source returns the same wd (%s)", strerror(errno));
    create_write(dst, "b4");
    drain(in);
    how = "after IN_MASK_ADD of IN_CREATE through the source";
    EXPECT(w1, IN_CREATE, "b4", 1, how);
    EXPECT(w1, IN_MODIFY, "b4", 1, how);
    EXPECT(w1, IN_CLOSE_WRITE, "b4", 0, how);

    // Removing the one wd removes it for both names.
    check(inotify_rm_watch(in, w1) == 0, "inotify_rm_watch (%s)", strerror(errno));
    create_write(dst, "b5");
    create_write(src, "b6");
    drain(in);
    check(nev == 1 && evs[0].wd == w1 && evs[0].mask == IN_IGNORED,
          "after rm_watch: its IN_IGNORED, then nothing through either name (got %d events)",
          nev);
    close(in);
}

// One instance per name.
static void two_instances(void) {
    int a = instance(), b = instance();
    int wa = watch(a, src, CW), wb = watch(b, dst, CW);
    create_write(dst, "c1");
    drain(a);
    EXPECT(wa, IN_CLOSE_WRITE, "c1", 1, "two instances, the one watching the source");
    check(nev == 3, "two instances, the one watching the source: three events (got %d)", nev);
    drain(b);
    EXPECT(wb, IN_CLOSE_WRITE, "c1", 1, "two instances, the one watching the bind");
    check(nev == 3, "two instances, the one watching the bind: three events (got %d)", nev);
    close(a);
    close(b);
}

// A watch on a file, which is used through the bind.
static void file_watch(void) {
    const char *how = "a watch on the file through the source, used through the bind";
    char sf[128], df[128];
    join(sf, sizeof(sf), src, "f");
    join(df, sizeof(df), dst, "f");
    int fd = open(sf, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    check(fd >= 0, "create %s (%s)", sf, strerror(errno));
    if (fd >= 0)
        close(fd);

    int in = instance();
    int wd = watch(in, sf, IN_OPEN | IN_ACCESS | IN_MODIFY | IN_ATTRIB | IN_CLOSE_WRITE |
                           IN_CLOSE_NOWRITE);
    char c;
    fd = open(df, O_RDWR | O_CLOEXEC);
    check(fd >= 0 && write(fd, "y", 1) == 1 && lseek(fd, 0, SEEK_SET) == 0 &&
              read(fd, &c, 1) == 1 && fchmod(fd, 0600) == 0,
          "open, write, read and fchmod %s (%s)", df, strerror(errno));
    if (fd >= 0)
        close(fd);
    check(chmod(df, 0644) == 0, "chmod %s (%s)", df, strerror(errno));
    fd = open(df, O_RDONLY | O_CLOEXEC);
    check(fd >= 0, "open %s (%s)", df, strerror(errno));
    if (fd >= 0)
        close(fd);
    drain(in);
    EXPECT(wd, IN_OPEN, "", 2, how);
    EXPECT(wd, IN_MODIFY, "", 1, how);
    EXPECT(wd, IN_ACCESS, "", 1, how);
    EXPECT(wd, IN_ATTRIB, "", 2, how);
    EXPECT(wd, IN_CLOSE_WRITE, "", 1, how);
    EXPECT(wd, IN_CLOSE_NOWRITE, "", 1, how);
    close(in);
    unlink(sf);
}

static void unlink_through_bind(void) {
    const char *how = "unlinked through the bind";
    char sg[128], dg[128];
    join(sg, sizeof(sg), src, "g");
    join(dg, sizeof(dg), dst, "g");
    int fd = open(sg, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    check(fd >= 0, "create %s (%s)", sg, strerror(errno));
    if (fd >= 0)
        close(fd);
    int in = instance();
    int wdir = watch(in, src, IN_DELETE);
    int wf = watch(in, sg, IN_DELETE_SELF);
    check(unlink(dg) == 0, "unlink %s (%s)", dg, strerror(errno));
    drain(in);
    EXPECT(wdir, IN_DELETE, "g", 1, how);
    EXPECT(wf, IN_DELETE_SELF, "", 1, how);
    EXPECT(wf, IN_IGNORED, "", 1, how);
    close(in);
}

static void rename_through_bind(void) {
    const char *how = "renamed through the bind";
    char p[128], q[128];
    join(p, sizeof(p), src, "m1");
    int fd = open(p, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    check(fd >= 0, "create %s (%s)", p, strerror(errno));
    if (fd >= 0)
        close(fd);

    join(q, sizeof(q), src, "m3");
    fd = open(q, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    check(fd >= 0, "create %s (%s)", q, strerror(errno));
    if (fd >= 0)
        close(fd);

    int in = instance();
    int wdir = watch(in, src, IN_MOVED_FROM | IN_MOVED_TO);
    int wf = watch(in, p, IN_MOVE_SELF | IN_MODIFY);
    int wquiet = watch(in, q, IN_MODIFY);
    join(q, sizeof(q), src, "sub/deep");
    int wdeep = watch(in, q, IN_CREATE);

    join(p, sizeof(p), dst, "m1");
    join(q, sizeof(q), dst, "m2");
    check(rename(p, q) == 0, "rename %s %s (%s)", p, q, strerror(errno));
    drain(in);
    EXPECT(wdir, IN_MOVED_FROM, "m1", 1, how);
    EXPECT(wdir, IN_MOVED_TO, "m2", 1, how);
    uint32_t from = 0, to = 0;
    for (int i = 0; i < nev; i++) {
        if (evs[i].wd == wdir && (evs[i].mask & IN_MOVED_FROM))
            from = evs[i].cookie;
        if (evs[i].wd == wdir && (evs[i].mask & IN_MOVED_TO))
            to = evs[i].cookie;
    }
    check(from != 0 && from == to, "%s: IN_MOVED_FROM and IN_MOVED_TO share a cookie (%u, %u)",
          how, (unsigned) from, (unsigned) to);
    EXPECT(wf, IN_MOVE_SELF, "", 1, how);

    // The file's watch followed it to its new name.
    join(p, sizeof(p), src, "m2");
    fd = open(p, O_WRONLY | O_CLOEXEC);
    check(fd >= 0 && write(fd, "z", 1) == 1, "write %s (%s)", p, strerror(errno));
    if (fd >= 0)
        close(fd);
    drain(in);
    EXPECT(wf, IN_MODIFY, "", 1, "the renamed file's watch, written through the source");
    unlink(p);

    // Whether or not the watch asked to hear of the move.
    join(p, sizeof(p), dst, "m3");
    join(q, sizeof(q), dst, "m4");
    check(rename(p, q) == 0, "rename %s %s (%s)", p, q, strerror(errno));
    join(p, sizeof(p), src, "m4");
    fd = open(p, O_WRONLY | O_CLOEXEC);
    check(fd >= 0 && write(fd, "z", 1) == 1, "write %s (%s)", p, strerror(errno));
    if (fd >= 0)
        close(fd);
    drain(in);
    EXPECT(wquiet, IN_MODIFY, "", 1,
           "a renamed file's watch without IN_MOVE_SELF, written through the source");
    unlink(p);

    // So did a watch below a directory renamed through the bind.
    join(p, sizeof(p), dst, "sub");
    join(q, sizeof(q), dst, "sub2");
    check(rename(p, q) == 0, "rename %s %s (%s)", p, q, strerror(errno));
    join(p, sizeof(p), src, "sub2/deep");
    create_write(p, "d1");
    drain(in);
    EXPECT(wdeep, IN_CREATE, "d1", 1,
           "a watch below a directory renamed through the bind, used through the source");
    join(p, sizeof(p), src, "sub2/deep/d1");
    unlink(p);
    join(p, sizeof(p), src, "sub2");
    join(q, sizeof(q), src, "sub");
    check(rename(p, q) == 0, "rename %s back (%s)", p, strerror(errno));
    close(in);
}

// A watch on a directory placed before a bind covers it watches that
// directory, which the bind hides; the source's own watch is the control.
static void watch_under_bind(void) {
    const char *how = "a bind mounted over a watched directory";
    int in = instance();
    int wpre = watch(in, pre, CW);
    int wsrc = watch(in, src, CW);
    if (mount(src, pre, NULL, MS_BIND, NULL) != 0) {
        check(0, "mount --bind %s %s (%s)", src, pre, strerror(errno));
        close(in);
        return;
    }
    create_write(pre, "h1");
    drain(in);
    EXPECT(wsrc, IN_CREATE, "h1", 1, how);
    EXPECT(wpre, IN_CREATE, "h1", 0, how);
    check(umount2(pre, 0) == 0, "umount %s (%s)", pre, strerror(errno));
    close(in);
}

// chmod of the bind's point changes the source directory, which its parent
// reports under the source's name.
static void chmod_bind_point(void) {
    const char *how = "chmod of the bind's point, watched from the source's parent";
    int in = instance();
    int wd = watch(in, base, IN_ATTRIB);
    check(chmod(dst, 0751) == 0, "chmod %s (%s)", dst, strerror(errno));
    drain(in);
    EXPECT(wd, IN_ATTRIB, "src", 1, how);
    EXPECT(wd, IN_ATTRIB, "dst", 0, how);
    chmod(dst, 0755);
    close(in);
}

// A tmpfs's root is the root of its filesystem, which has no parent to tell;
// the plain directory beside it is the control.
static void mount_root_parent(void) {
    const char *how = "chmod and opendir of a tmpfs's mount point, watched from above";
    char t[128];
    join(t, sizeof(t), base, "t");
    check(mkdir(t, 0755) == 0, "mkdir %s (%s)", t, strerror(errno));
    if (mount("tmpfs", t, "tmpfs", 0, NULL) != 0) {
        check(0, "mount -t tmpfs tmpfs %s (%s)", t, strerror(errno));
        return;
    }
    int in = instance();
    int wd = watch(in, base, IN_ATTRIB | IN_OPEN);
    check(chmod(t, 0751) == 0, "chmod %s (%s)", t, strerror(errno));
    int fd = open(t, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    check(fd >= 0, "open %s (%s)", t, strerror(errno));
    if (fd >= 0)
        close(fd);
    check(chmod(pre, 0751) == 0, "chmod %s (%s)", pre, strerror(errno));
    drain(in);
    EXPECT(wd, IN_ATTRIB, "pre", 1, how);
    EXPECT(wd, IN_ATTRIB, "t", 0, how);
    EXPECT(wd, IN_OPEN, "t", 0, how);
    check(umount2(t, 0) == 0, "umount %s (%s)", t, strerror(errno));
    close(in);
}

// A watch placed through the bind is on the source directory, and stays
// there after the bind is gone.
static void watch_outlives_bind(void) {
    const char *how = "a watch placed through the bind, after its umount";
    int in = instance();
    int wd = watch(in, dst, CW);
    if (umount2(dst, 0) != 0) {
        check(0, "umount %s (%s)", dst, strerror(errno));
        close(in);
        return;
    }
    create_write(src, "u1");
    create_write(dst, "u2");
    drain(in);
    EXPECT(wd, IN_CREATE, "u1", 1, how);
    EXPECT(wd, IN_CLOSE_WRITE, "u1", 1, how);
    EXPECT(wd, IN_CREATE, "u2", 0, how);
    close(in);
}

// A watch on / is told of what is created in it.
static void root_watch(void) {
    const char *how = "a watch on /";
    char p[128];
    snprintf(p, sizeof(p), "/%s-root", strrchr(base, '/') + 1);
    int in = instance();
    int wd = watch(in, "/", IN_CREATE);
    int fd = open(p, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    check(fd >= 0, "create %s (%s)", p, strerror(errno));
    if (fd >= 0)
        close(fd);
    drain(in);
    EXPECT(wd, IN_CREATE, p + 1, 1, how);
    unlink(p);
    close(in);
}

static int rm_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw) {
    (void) st;
    (void) ftw;
    if (flag == FTW_DP)
        rmdir(path);
    else
        unlink(path);
    return 0;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));

    if (mkdtemp(base) == NULL) {
        printf("FAIL: mkdtemp (%s)\n", strerror(errno));
        return 1;
    }
    char tmp[128];
    join(src, sizeof(src), base, "src");
    join(dst, sizeof(dst), base, "dst");
    join(dst2, sizeof(dst2), base, "dst2");
    join(pre, sizeof(pre), base, "pre");
    const char *dirs[] = {src, dst, dst2, pre};
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
        check(mkdir(dirs[i], 0755) == 0, "mkdir %s (%s)", dirs[i], strerror(errno));
    join(tmp, sizeof(tmp), src, "sub");
    check(mkdir(tmp, 0755) == 0, "mkdir %s (%s)", tmp, strerror(errno));
    join(tmp, sizeof(tmp), src, "sub/deep");
    check(mkdir(tmp, 0755) == 0, "mkdir %s (%s)", tmp, strerror(errno));

    if (mount(src, dst, NULL, MS_BIND, NULL) != 0) {
        int err = errno;
        check(err == EPERM, "mount --bind %s %s (%s)", src, dst, strerror(err));
        test_logf("mount: %s, not privileged: bind checks skipped\n", strerror(err));
        one_way(src, src, "plain", "unprivileged, watched and written through one name");
        goto cleanup;
    }
    // Private, as mount_bind_getpath does, so nothing here propagates on a
    // Linux whose / is shared.
    check(mount(NULL, dst, NULL, MS_PRIVATE, NULL) == 0, "mount --make-private %s (%s)", dst,
          strerror(errno));

    one_way(src, src, "a0", "watched and written through the source");
    one_way(dst, dst, "a1", "watched and written through the bind");
    one_way(src, dst, "a2", "watched through the source, written through the bind");
    one_way(dst, src, "a3", "watched through the bind, written through the source");
    both_names();
    two_instances();
    file_watch();
    unlink_through_bind();
    rename_through_bind();

    if (mount(dst, dst2, NULL, MS_BIND, NULL) == 0) {
        one_way(src, dst2, "e1", "watched through the source, written through a bind of the bind");
        one_way(dst2, src, "e2", "watched through a bind of the bind, written through the source");
        check(umount2(dst2, 0) == 0, "umount %s (%s)", dst2, strerror(errno));
    } else {
        check(0, "mount --bind %s %s (%s)", dst, dst2, strerror(errno));
    }

    watch_under_bind();
    chmod_bind_point();
    mount_root_parent();
    watch_outlives_bind();
    root_watch();

cleanup:
    join(tmp, sizeof(tmp), base, "t");
    umount2(tmp, MNT_DETACH);
    umount2(pre, MNT_DETACH);
    umount2(dst2, MNT_DETACH);
    umount2(dst, MNT_DETACH);
    nftw(base, rm_entry, 16, FTW_DEPTH | FTW_PHYS | FTW_MOUNT);
    return finish_suite(TEST_NAME);
}
