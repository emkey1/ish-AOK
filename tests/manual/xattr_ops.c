// xattr_ops.c -- the extended-attribute calls: setxattr, getxattr, listxattr
// and removexattr, with their l* and f* forms.
//
// AOK answered ENOTSUP for every one of them on every file, so nothing that
// stores metadata beside a file could: setcap (security.capability), rsync -X,
// tar --xattrs, cp -a's attribute copy, systemd's user.* markers. Every
// expectation below was measured on Linux 6.12 (camd: ext4 /tmp, tmpfs
// /dev/shm), as uid 1000 and as root of a user namespace (unshare -Ur), and is
// branched on the privilege it depends on.
//
// Runs the whole set twice when it can: once in /tmp (fakefs in an AOK root)
// and once in a tmpfs this test mounts itself (root only), since those are the
// two filesystems AOK keeps attributes for.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stdint.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>
#include "test_common.h"

#ifndef XATTR_CREATE
#define XATTR_CREATE 1
#define XATTR_REPLACE 2
#endif
#ifndef O_PATH
#define O_PATH 010000000
#endif

// Any nonzero uid does; nothing needs it in /etc/passwd.
#define UNPRIV_UID 1000
#define UNPRIV_GID 1000

// security.capability's on-disk layouts (linux/capability.h).
#define VFS_CAP_REVISION_1 0x01000000u
#define VFS_CAP_REVISION_2 0x02000000u
#define VFS_CAP_REVISION_3 0x03000000u
#define VFS_CAP_FLAGS_EFFECTIVE 0x000001u
#define CAP_NET_BIND_SERVICE_BIT (1u << 10)

struct caps_v3 {
    uint32_t magic_etc;
    struct {
        uint32_t permitted;
        uint32_t inheritable;
    } data[2];
    uint32_t rootid;
};

static int is_root;
// Root of the initial user namespace, as opposed to root of one made with
// unshare -Ur: trusted.* and security.* answer to capabilities in the initial
// namespace only, so a namespace root is refused them as uid 1000 would be.
static int init_ns_root;

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-60s got=%-6ld want=%ld\n", label, got, want);
}

// A syscall's answer as one number: the result, or -errno.
static long rv(long r) {
    return r < 0 ? -errno : r;
}

static char base[200];
static char pathbuf[4][300];
static const char *P(int slot, const char *sub) {
    snprintf(pathbuf[slot], sizeof pathbuf[slot], "%s/%s", base, sub);
    return pathbuf[slot];
}

static int make_file(const char *path, mode_t mode) {
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, mode);
    if (fd < 0)
        return -errno;
    close(fd);
    return 0;
}

// Does a listxattr buffer of `len` bytes hold `name`?
static int list_has(const char *list, long len, const char *name) {
    for (long i = 0; i < len; ) {
        if (strcmp(list + i, name) == 0)
            return 1;
        i += (long) strlen(list + i) + 1;
    }
    return 0;
}

static int in_initial_userns(void) {
    FILE *f = fopen("/proc/self/uid_map", "r");
    if (f == NULL)
        return 1;  // no user namespaces at all: this is the initial one
    unsigned long inside = 1, outside = 1, count = 0;
    int n = fscanf(f, "%lu %lu %lu", &inside, &outside, &count);
    fclose(f);
    return n == 3 && inside == 0 && outside == 0 && count == 4294967295ul;
}

// Run a block as an unprivileged user, folding its failures back into ours.
#define AS_USER(...) do {                                                      \
        fflush(NULL);                                                          \
        pid_t c_ = fork();                                                     \
        if (c_ == 0) {                                                         \
            if (setgroups(0, NULL) != 0 || setgid(UNPRIV_GID) != 0 ||          \
                    setuid(UNPRIV_UID) != 0) {                                 \
                printf("FAIL could not drop to uid %d: %s\n",                  \
                       UNPRIV_UID, strerror(errno));                           \
                fflush(NULL);                                                  \
                _exit(1);                                                      \
            }                                                                  \
            failures_total = 0;                                                \
            __VA_ARGS__;                                                       \
            fflush(NULL);                                                      \
            _exit(failures_total > 250 ? 250 : (int) failures_total);          \
        }                                                                      \
        int st_;                                                               \
        if (waitpid(c_, &st_, 0) != c_) { failures_total++; break; }           \
        if (WIFSIGNALED(st_)) {                                                \
            printf("FAIL child died on signal %d\n", WTERMSIG(st_));           \
            failures_total++;                                                  \
        } else                                                                 \
            failures_total += (unsigned) WEXITSTATUS(st_);                     \
    } while (0)

// The name and value rules, which are the same for every filesystem that
// keeps attributes and every caller who may write the file.
static void check_basic(const char *fs) {
    char label[160];
#define L(what) (snprintf(label, sizeof label, "%s: %s", fs, what), label)
    const char *f = P(0, "basic");
    ck(L("create file"), make_file(f, 0644), 0);
    char buf[512];

    ck(L("set user.a"), rv(setxattr(f, "user.a", "hello", 5, 0)), 0);
    memset(buf, 0, sizeof buf);
    ck(L("get user.a"), rv(getxattr(f, "user.a", buf, sizeof buf)), 5);
    ck(L("get user.a value"), memcmp(buf, "hello", 5), 0);
    ck(L("get user.a size query"), rv(getxattr(f, "user.a", NULL, 0)), 5);
    ck(L("get user.a short buffer"), rv(getxattr(f, "user.a", buf, 2)), -ERANGE);
    ck(L("get missing"), rv(getxattr(f, "user.missing", buf, sizeof buf)), -ENODATA);
    ck(L("size query of missing"), rv(getxattr(f, "user.missing", NULL, 0)), -ENODATA);

    ck(L("CREATE over existing"), rv(setxattr(f, "user.a", "x", 1, XATTR_CREATE)), -EEXIST);
    ck(L("REPLACE of missing"), rv(setxattr(f, "user.b", "x", 1, XATTR_REPLACE)), -ENODATA);
    ck(L("REPLACE of existing"), rv(setxattr(f, "user.a", "world!", 6, XATTR_REPLACE)), 0);
    memset(buf, 0, sizeof buf);
    ck(L("replaced value size"), rv(getxattr(f, "user.a", buf, sizeof buf)), 6);
    ck(L("replaced value"), memcmp(buf, "world!", 6), 0);
    ck(L("CREATE of missing, empty value"), rv(setxattr(f, "user.b", "", 0, XATTR_CREATE)), 0);
    ck(L("get empty value"), rv(getxattr(f, "user.b", buf, sizeof buf)), 0);
    ck(L("unknown flag bit"), rv(setxattr(f, "user.c", "x", 1, 4)), -EINVAL);
    // Both flags: the existence test of each applies.
    ck(L("CREATE|REPLACE of existing"), rv(setxattr(f, "user.a", "x", 1, XATTR_CREATE | XATTR_REPLACE)), -EEXIST);
    ck(L("CREATE|REPLACE of missing"), rv(setxattr(f, "user.c", "x", 1, XATTR_CREATE | XATTR_REPLACE)), -ENODATA);

    // Names. The prefix picks the namespace; a bare prefix is malformed, an
    // unknown one is a namespace this filesystem does not have.
    ck(L("name 'user.'"), rv(setxattr(f, "user.", "x", 1, 0)), -EINVAL);
    ck(L("get name 'user.'"), rv(getxattr(f, "user.", buf, sizeof buf)), -EINVAL);
    ck(L("name 'user'"), rv(setxattr(f, "user", "x", 1, 0)), -EOPNOTSUPP);
    ck(L("name 'bogus.x'"), rv(setxattr(f, "bogus.x", "x", 1, 0)), -EOPNOTSUPP);
    ck(L("get name 'bogus.x'"), rv(getxattr(f, "bogus.x", buf, sizeof buf)), -EOPNOTSUPP);
    ck(L("empty name"), rv(setxattr(f, "", "x", 1, 0)), -ERANGE);
    ck(L("get empty name"), rv(getxattr(f, "", buf, sizeof buf)), -ERANGE);
    ck(L("remove empty name"), rv(removexattr(f, "")), -ERANGE);
    char name[300];
    memset(name, 'n', sizeof name);
    memcpy(name, "user.", 5);
    name[255] = '\0';  // 255 bytes: XATTR_NAME_MAX
    ck(L("255-byte name"), rv(setxattr(f, name, "x", 1, 0)), 0);
    ck(L("get 255-byte name"), rv(getxattr(f, name, buf, sizeof buf)), 1);
    ck(L("remove 255-byte name"), rv(removexattr(f, name)), 0);
    name[255] = 'n';
    name[256] = '\0';
    ck(L("256-byte name"), rv(setxattr(f, name, "x", 1, 0)), -ERANGE);
    ck(L("get 256-byte name"), rv(getxattr(f, name, buf, sizeof buf)), -ERANGE);

    ck(L("name 'security.' get"), rv(getxattr(f, "security.", buf, sizeof buf)), -EINVAL);
    ck(L("name 'system.foo'"), rv(setxattr(f, "system.foo", "x", 1, 0)), -EOPNOTSUPP);
    ck(L("get name 'system.foo'"), rv(getxattr(f, "system.foo", buf, sizeof buf)), -EOPNOTSUPP);

    // Values: 64 KiB is the VFS ceiling, whatever the filesystem stores.
    static char big[65537];
    memset(big, 'v', sizeof big);
    ck(L("65537-byte value"), rv(setxattr(f, "user.big", big, sizeof big, 0)), -E2BIG);
    // A size past the ceiling on the way OUT is clamped, not refused: the
    // answer is the value's own size.
    // Through volatile pointers so the compiler cannot see what they are.
    char *volatile vbuf = buf;
    void *volatile bad = (void *) 8;
    ck(L("get with a huge size"), rv(getxattr(f, "user.a", vbuf, 1u << 30)), 6);
    ck(L("value from a bad pointer"), rv(setxattr(f, "user.bad", bad, 10, 0)), -EFAULT);
    ck(L("get into a bad pointer"), rv(getxattr(f, "user.a", bad, 64)), -EFAULT);
    ck(L("name from a bad pointer"), rv(setxattr(f, (const char *) bad, "x", 1, 0)), -EFAULT);

    // Lists: NUL-terminated names, size query first.
    long need = rv(listxattr(f, NULL, 0));
    ck(L("list size query"), need, (long) (sizeof "user.a" + sizeof "user.b"));
    char list[512];
    memset(list, 0, sizeof list);
    long got = rv(listxattr(f, list, sizeof list));
    ck(L("list"), got, need);
    ck(L("list has user.a"), list_has(list, got, "user.a"), 1);
    ck(L("list has user.b"), list_has(list, got, "user.b"), 1);
    ck(L("list short buffer"), rv(listxattr(f, list, 3)), -ERANGE);
    ck(L("llist of a file"), rv(llistxattr(f, list, sizeof list)), need);

    ck(L("remove user.b"), rv(removexattr(f, "user.b")), 0);
    ck(L("remove user.b again"), rv(removexattr(f, "user.b")), -ENODATA);
    ck(L("list after remove"), rv(listxattr(f, list, sizeof list)), (long) sizeof "user.a");
    ck(L("lremove of a file"), rv(lremovexattr(f, "user.a")), 0);
    ck(L("list now empty"), rv(listxattr(f, list, sizeof list)), 0);
    ck(L("lset of a file"), rv(lsetxattr(f, "user.l", "l", 1, 0)), 0);
    ck(L("lget of a file"), rv(lgetxattr(f, "user.l", buf, sizeof buf)), 1);

    ck(L("missing path"), rv(setxattr(P(1, "nonexistent"), "user.a", "x", 1, 0)), -ENOENT);
    ck(L("get on missing path"), rv(getxattr(P(1, "nonexistent"), "user.a", buf, sizeof buf)), -ENOENT);
    ck(L("list on missing path"), rv(listxattr(P(1, "nonexistent"), list, sizeof list)), -ENOENT);
#undef L
}

// What kind of object may carry user.* attributes: files and directories only.
static void check_object_types(const char *fs) {
    char label[160];
#define L(what) (snprintf(label, sizeof label, "%s: %s", fs, what), label)
    char buf[64], list[256];
    const char *d = P(0, "dir");
    ck(L("mkdir"), rv(mkdir(d, 0755)), 0);
    ck(L("user.* on a directory"), rv(setxattr(d, "user.d", "d", 1, 0)), 0);
    ck(L("get it back"), rv(getxattr(d, "user.d", buf, sizeof buf)), 1);

    const char *target = P(1, "target");
    const char *link = P(2, "link");
    ck(L("link target"), make_file(target, 0644), 0);
    ck(L("symlink"), rv(symlink(target, link)), 0);
    ck(L("lset user.* on a symlink"), rv(lsetxattr(link, "user.s", "s", 1, 0)), -EPERM);
    ck(L("lget user.* on a symlink"), rv(lgetxattr(link, "user.s", buf, sizeof buf)), -ENODATA);
    ck(L("lremove user.* on a symlink"), rv(lremovexattr(link, "user.s")), -EPERM);
    ck(L("llist of a symlink"), rv(llistxattr(link, list, sizeof list)), 0);
    ck(L("set through a symlink"), rv(setxattr(link, "user.t", "t", 1, 0)), 0);
    ck(L("...lands on the target"), rv(getxattr(target, "user.t", buf, sizeof buf)), 1);
    ck(L("list through a symlink"), rv(listxattr(link, list, sizeof list)), (long) sizeof "user.t");

    const char *fifo = P(3, "fifo");
    ck(L("mkfifo"), rv(mkfifo(fifo, 0644)), 0);
    ck(L("user.* on a fifo"), rv(setxattr(fifo, "user.f", "f", 1, 0)), -EPERM);
    ck(L("get user.* on a fifo"), rv(getxattr(fifo, "user.f", buf, sizeof buf)), -ENODATA);
    ck(L("list of a fifo"), rv(listxattr(fifo, list, sizeof list)), 0);
    unlink(fifo);

    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un sun = {.sun_family = AF_UNIX};
    snprintf(sun.sun_path, sizeof sun.sun_path, "%s", P(3, "sock"));
    ck(L("bind a socket node"), rv(bind(s, (struct sockaddr *) &sun, sizeof sun)), 0);
    ck(L("user.* on a socket node"), rv(setxattr(P(3, "sock"), "user.k", "k", 1, 0)), -EPERM);
    close(s);
    unlink(P(3, "sock"));
#undef L
}

// The f* forms act on the open file, whatever became of its name.
static void check_fd_forms(const char *fs) {
    char label[160];
#define L(what) (snprintf(label, sizeof label, "%s: %s", fs, what), label)
    char buf[64], list[256];
    const char *f = P(0, "fdfile");
    ck(L("create"), make_file(f, 0644), 0);
    int fd = open(f, O_RDONLY);
    // A read-only descriptor is enough: permission is the inode's, not the
    // descriptor's.
    ck(L("fset on an O_RDONLY fd"), rv(fsetxattr(fd, "user.f", "fd", 2, 0)), 0);
    ck(L("fget"), rv(fgetxattr(fd, "user.f", buf, sizeof buf)), 2);
    ck(L("path get sees it"), rv(getxattr(f, "user.f", buf, sizeof buf)), 2);
    ck(L("flist"), rv(flistxattr(fd, list, sizeof list)), (long) sizeof "user.f");
    ck(L("fremove"), rv(fremovexattr(fd, "user.f")), 0);
    ck(L("fget after fremove"), rv(fgetxattr(fd, "user.f", buf, sizeof buf)), -ENODATA);
    ck(L("fset CREATE"), rv(fsetxattr(fd, "user.g", "g", 1, XATTR_CREATE)), 0);
    ck(L("fset CREATE again"), rv(fsetxattr(fd, "user.g", "g", 1, XATTR_CREATE)), -EEXIST);
    close(fd);
    ck(L("fget on a closed fd"), rv(fgetxattr(fd, "user.g", buf, sizeof buf)), -EBADF);
    ck(L("fset on fd -1"), rv(fsetxattr(-1, "user.g", "g", 1, 0)), -EBADF);

    // Linux 6.12: an O_PATH descriptor is not one these calls accept.
    int opath = open(f, O_PATH);
    ck(L("fget on O_PATH"), rv(fgetxattr(opath, "user.g", buf, sizeof buf)), -EBADF);
    ck(L("fset on O_PATH"), rv(fsetxattr(opath, "user.g", "g", 1, 0)), -EBADF);
    ck(L("flist on O_PATH"), rv(flistxattr(opath, list, sizeof list)), -EBADF);
    close(opath);

    // Unlinked but open: the attributes belong to the inode.
    const char *g = P(1, "unlinked");
    int ufd = open(g, O_RDWR | O_CREAT, 0644);
    ck(L("create to unlink"), ufd >= 0, 1);
    ck(L("set before unlink"), rv(fsetxattr(ufd, "user.u", "1", 1, 0)), 0);
    ck(L("unlink"), rv(unlink(g)), 0);
    ck(L("fget after unlink"), rv(fgetxattr(ufd, "user.u", buf, sizeof buf)), 1);
    ck(L("fset after unlink"), rv(fsetxattr(ufd, "user.v", "22", 2, 0)), 0);
    ck(L("fget new after unlink"), rv(fgetxattr(ufd, "user.v", buf, sizeof buf)), 2);
    ck(L("flist after unlink"), rv(flistxattr(ufd, list, sizeof list)), (long) (sizeof "user.u" + sizeof "user.v"));
    close(ufd);

    // A name made later is a new inode: nothing carried over from the one
    // unlinked above.
    ck(L("recreate the name"), make_file(g, 0644), 0);
    ck(L("fresh inode has none"), rv(listxattr(g, list, sizeof list)), 0);

    // Hard links share the inode, and so its attributes; renames keep them.
    const char *h = P(2, "hardlink");
    const char *r = P(3, "renamed");
    ck(L("set on original"), rv(setxattr(f, "user.h", "hh", 2, 0)), 0);
    ck(L("hard link"), rv(link(f, h)), 0);
    ck(L("seen through the link"), rv(getxattr(h, "user.h", buf, sizeof buf)), 2);
    ck(L("set through the link"), rv(setxattr(h, "user.i", "i", 1, 0)), 0);
    ck(L("seen through the original"), rv(getxattr(f, "user.i", buf, sizeof buf)), 1);
    ck(L("rename the link"), rv(rename(h, r)), 0);
    ck(L("kept across rename"), rv(getxattr(r, "user.h", buf, sizeof buf)), 2);
    ck(L("unlink the original"), rv(unlink(f)), 0);
    ck(L("kept by the surviving name"), rv(getxattr(r, "user.i", buf, sizeof buf)), 1);
    // A rename OVER a name with attributes: the replaced inode's go with it.
    ck(L("victim"), make_file(f, 0644), 0);
    ck(L("victim attribute"), rv(setxattr(f, "user.victim", "v", 1, 0)), 0);
    ck(L("rename over the victim"), rv(rename(r, f)), 0);
    ck(L("victim's attribute is gone"), rv(getxattr(f, "user.victim", buf, sizeof buf)), -ENODATA);
    ck(L("mover's attribute is there"), rv(getxattr(f, "user.h", buf, sizeof buf)), 2);
    // A directory's attributes follow it through a rename too, and its
    // children's with it.
    const char *d1 = P(0, "mvdir");
    ck(L("mkdir to move"), rv(mkdir(d1, 0755)), 0);
    ck(L("attribute on the dir"), rv(setxattr(d1, "user.dir", "d", 1, 0)), 0);
    snprintf(pathbuf[1], sizeof pathbuf[1], "%s/child", d1);
    ck(L("child file"), make_file(pathbuf[1], 0644), 0);
    ck(L("attribute on the child"), rv(setxattr(pathbuf[1], "user.child", "c", 1, 0)), 0);
    const char *d2 = P(2, "mvdir2");
    ck(L("rename the dir"), rv(rename(d1, d2)), 0);
    ck(L("dir attribute moved"), rv(getxattr(d2, "user.dir", buf, sizeof buf)), 1);
    snprintf(pathbuf[1], sizeof pathbuf[1], "%s/child", d2);
    ck(L("child attribute moved"), rv(getxattr(pathbuf[1], "user.child", buf, sizeof buf)), 1);
#undef L
}

// What a caller without privilege may do: user.* by the file's permission
// bits, trusted.* nothing, security.* nothing but read.
static struct timespec ctime_of(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0)
        return (struct timespec) {0, 0};
    return st.st_ctim;
}

static int moved(struct timespec before, struct timespec after) {
    return after.tv_sec > before.tv_sec ||
           (after.tv_sec == before.tv_sec && after.tv_nsec > before.tv_nsec);
}

// Past the next whole second, so a ctime that moved differs even where a
// filesystem keeps coarse timestamps.
static void next_second(void) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    struct timespec d = {0, 1000000000L - now.tv_nsec + 20000000L};
    if (d.tv_nsec >= 1000000000L) {
        d.tv_sec = 1;
        d.tv_nsec -= 1000000000L;
    }
    nanosleep(&d, NULL);
}

// Setting or removing an attribute is a change to the inode and moves its
// ctime; reading one, listing them, or failing to remove one does not
// (measured on 6.12, ext4 and tmpfs alike). fakefs keeps attributes in its
// database, where the host file never saw them change.
static void check_ctime(const char *fs) {
    char label[160];
#define L(what) (snprintf(label, sizeof label, "%s: %s", fs, what), label)
    const char *a = P(0, "ctime-path"), *b = P(1, "ctime-fd"), *c = P(2, "ctime-read");
    ck(L("ctime: create files"), make_file(a, 0644) || make_file(b, 0644) || make_file(c, 0644), 0);
    int fd = open(b, O_RDONLY);
    char buf[64];
    struct timespec ta = ctime_of(a), tb = ctime_of(b), tc = ctime_of(c);
    next_second();
    ck(L("ctime: setxattr"), rv(setxattr(a, "user.t", "1", 1, 0)), 0);
    ck(L("ctime: fsetxattr"), rv(fsetxattr(fd, "user.t", "1", 1, 0)), 0);
    getxattr(c, "user.none", buf, sizeof buf);
    listxattr(c, buf, sizeof buf);
    ck(L("ctime: setxattr moves it"), moved(ta, ctime_of(a)), 1);
    ck(L("ctime: fsetxattr moves it"), moved(tb, ctime_of(b)), 1);
    ck(L("ctime: getxattr and listxattr do not"), moved(tc, ctime_of(c)), 0);
    ta = ctime_of(a);
    tb = ctime_of(b);
    tc = ctime_of(c);
    next_second();
    ck(L("ctime: removexattr"), rv(removexattr(a, "user.t")), 0);
    ck(L("ctime: fremovexattr"), rv(fremovexattr(fd, "user.t")), 0);
    ck(L("ctime: removexattr of a missing name"), rv(removexattr(c, "user.none")), -ENODATA);
    ck(L("ctime: removexattr moves it"), moved(ta, ctime_of(a)), 1);
    ck(L("ctime: fremovexattr moves it"), moved(tb, ctime_of(b)), 1);
    ck(L("ctime: a failed removexattr does not"), moved(tc, ctime_of(c)), 0);
    close(fd);
    unlink(a);
    unlink(b);
    unlink(c);
#undef L
}

static void check_unprivileged(const char *fs) {
    char label[160];
#define L(what) (snprintf(label, sizeof label, "%s: %s", fs, what), label)
    char buf[64], list[256];
    const char *f = P(0, "mine");
    ck(L("create own file"), make_file(f, 0644), 0);
    ck(L("set user.* on own file"), rv(setxattr(f, "user.o", "o", 1, 0)), 0);
    ck(L("chmod 0"), rv(chmod(f, 0)), 0);
    ck(L("get with no read bit"), rv(getxattr(f, "user.o", buf, sizeof buf)), -EACCES);
    ck(L("set with no write bit"), rv(setxattr(f, "user.o", "p", 1, 0)), -EACCES);
    ck(L("remove with no write bit"), rv(removexattr(f, "user.o")), -EACCES);
    // listxattr asks for no permission at all.
    ck(L("list with no permission"), rv(listxattr(f, list, sizeof list)), (long) sizeof "user.o");
    ck(L("chmod 0444"), rv(chmod(f, 0444)), 0);
    ck(L("get with read bit"), rv(getxattr(f, "user.o", buf, sizeof buf)), 1);
    ck(L("set, read-only file"), rv(setxattr(f, "user.o", "p", 1, 0)), -EACCES);
    ck(L("chmod 0644"), rv(chmod(f, 0644)), 0);

    ck(L("set trusted.*"), rv(setxattr(f, "trusted.t", "t", 1, 0)), -EPERM);
    ck(L("get trusted.*"), rv(getxattr(f, "trusted.t", buf, sizeof buf)), -ENODATA);
    ck(L("remove trusted.*"), rv(removexattr(f, "trusted.t")), -EPERM);
    ck(L("set security.*"), rv(setxattr(f, "security.s", "s", 1, 0)), -EPERM);
    ck(L("get security.*"), rv(getxattr(f, "security.s", buf, sizeof buf)), -ENODATA);
    ck(L("remove security.*"), rv(removexattr(f, "security.s")), -EPERM);
    // The privilege checks come before the name is looked at any further: a
    // bare "trusted." is refused as any trusted.* name would be.
    ck(L("set bare 'trusted.'"), rv(setxattr(f, "trusted.", "t", 1, 0)), -EPERM);
    ck(L("set bare 'security.'"), rv(setxattr(f, "security.", "s", 1, 0)), -EPERM);

    // security.capability is checked for shape before privilege.
    struct caps_v3 caps = {.magic_etc = VFS_CAP_REVISION_2 | VFS_CAP_FLAGS_EFFECTIVE};
    caps.data[0].permitted = CAP_NET_BIND_SERVICE_BIT;
    ck(L("malformed capability"), rv(setxattr(f, "security.capability", &caps, 5, 0)), -EINVAL);
    ck(L("well-formed capability"), rv(setxattr(f, "security.capability", &caps, 20, 0)), -EPERM);
    ck(L("get absent capability"), rv(getxattr(f, "security.capability", buf, sizeof buf)), -ENODATA);
#undef L
}

// What only privilege may do, and what the file's owner cannot stop.
static void check_privileged(const char *fs) {
    char label[160];
#define L(what) (snprintf(label, sizeof label, "%s: %s", fs, what), label)
    char buf[64], list[256];
    const char *f = P(0, "root");
    ck(L("create"), make_file(f, 0644), 0);
    ck(L("chmod 0"), rv(chmod(f, 0)), 0);
    ck(L("set user.* past mode 0"), rv(setxattr(f, "user.r", "r", 1, 0)), 0);
    ck(L("get user.* past mode 0"), rv(getxattr(f, "user.r", buf, sizeof buf)), 1);
    ck(L("chmod 0644"), rv(chmod(f, 0644)), 0);

    if (init_ns_root) {
        ck(L("set trusted.*"), rv(setxattr(f, "trusted.t", "tt", 2, 0)), 0);
        ck(L("get trusted.*"), rv(getxattr(f, "trusted.t", buf, sizeof buf)), 2);
        ck(L("set security.*"), rv(setxattr(f, "security.s", "sss", 3, 0)), 0);
        ck(L("get security.*"), rv(getxattr(f, "security.s", buf, sizeof buf)), 3);
        // With the privilege, a bare prefix is only malformed.
        ck(L("set bare 'trusted.'"), rv(setxattr(f, "trusted.", "t", 1, 0)), -EINVAL);
        ck(L("set bare 'security.'"), rv(setxattr(f, "security.", "s", 1, 0)), -EINVAL);
        long n = rv(listxattr(f, list, sizeof list));
        ck(L("list has trusted.t"), list_has(list, n, "trusted.t"), 1);
        ck(L("list has security.s"), list_has(list, n, "security.s"), 1);
        // trusted.* is not restricted to files and directories.
        const char *link = P(1, "rootlink");
        ck(L("symlink"), rv(symlink(f, link)), 0);
        ck(L("trusted.* on a symlink"), rv(lsetxattr(link, "trusted.l", "l", 1, 0)), 0);
        ck(L("get it back"), rv(lgetxattr(link, "trusted.l", buf, sizeof buf)), 1);
        ck(L("user.* on a symlink, even so"), rv(lsetxattr(link, "user.l", "l", 1, 0)), -EPERM);

        // Hidden from anyone else's list: trusted.* is not even named to a
        // caller without CAP_SYS_ADMIN, and cannot be read by one.
        ck(L("user.* beside them"), rv(setxattr(f, "user.u", "u", 1, 0)), 0);
        AS_USER({
            long m = rv(listxattr(f, list, sizeof list));
            ck(L("unprivileged list"), m > 0, 1);
            ck(L("unprivileged list hides trusted.t"), list_has(list, m, "trusted.t"), 0);
            ck(L("unprivileged list shows security.s"), list_has(list, m, "security.s"), 1);
            ck(L("unprivileged list shows user.u"), list_has(list, m, "user.u"), 1);
            ck(L("unprivileged get trusted.t"), rv(getxattr(f, "trusted.t", buf, sizeof buf)), -ENODATA);
            ck(L("unprivileged get security.s"), rv(getxattr(f, "security.s", buf, sizeof buf)), 3);
            ck(L("unprivileged size query of the list"), rv(listxattr(f, NULL, 0)), m);
        });
    } else {
        // A user namespace's root holds nothing in the initial namespace,
        // which is where trusted.* is decided. security.* is decided in the
        // namespace that owns the filesystem, which is the initial one for
        // the disk -- and the user namespace itself for a tmpfs mounted in it.
        ck(L("namespace root: set trusted.*"), rv(setxattr(f, "trusted.t", "t", 1, 0)), -EPERM);
        if (strcmp(fs, "disk") == 0)
            ck(L("namespace root: set security.*"), rv(setxattr(f, "security.s", "s", 1, 0)), -EPERM);
    }

    // The sticky rule: in a sticky directory, only the owner of the directory
    // may set user.* on the directory itself.
    if (init_ns_root) {
        const char *sticky = P(1, "sticky");
        ck(L("sticky dir"), rv(mkdir(sticky, 0777)), 0);
        ck(L("chmod 1777"), rv(chmod(sticky, 01777)), 0);
        const char *plain = P(2, "plain");
        ck(L("plain dir"), rv(mkdir(plain, 0777)), 0);
        ck(L("chmod 0777"), rv(chmod(plain, 0777)), 0);
        AS_USER({
            ck(L("user.* on another's sticky dir"), rv(setxattr(sticky, "user.x", "x", 1, 0)), -EPERM);
            ck(L("user.* on another's writable dir"), rv(setxattr(plain, "user.x", "x", 1, 0)), 0);
        });
    }
#undef L
}

// security.capability: its shape, and the conversions Linux makes on the way
// in and out, then what takes it away.
static void check_capability(const char *fs) {
    char label[160];
#define L(what) (snprintf(label, sizeof label, "%s: %s", fs, what), label)
    unsigned char buf[64];
    const char *f = P(0, "capfile");
    ck(L("create"), make_file(f, 0755), 0);

    struct caps_v3 caps = {.magic_etc = VFS_CAP_REVISION_1};
    caps.data[0].permitted = CAP_NET_BIND_SERVICE_BIT;
    ck(L("v1 is refused"), rv(setxattr(f, "security.capability", &caps, 12, 0)), -EINVAL);
    caps.magic_etc = VFS_CAP_REVISION_2 | VFS_CAP_FLAGS_EFFECTIVE;
    ck(L("v2 magic, v3 size"), rv(setxattr(f, "security.capability", &caps, 24, 0)), -EINVAL);
    ck(L("v2 magic, 12 bytes"), rv(setxattr(f, "security.capability", &caps, 12, 0)), -EINVAL);
    caps.magic_etc = 0x04000000u;
    ck(L("unknown revision"), rv(setxattr(f, "security.capability", &caps, 20, 0)), -EINVAL);
    caps.magic_etc = VFS_CAP_REVISION_2 | 0x100;
    ck(L("v2 with an unknown flag"), rv(setxattr(f, "security.capability", &caps, 20, 0)), -EINVAL);

    caps.magic_etc = VFS_CAP_REVISION_2 | VFS_CAP_FLAGS_EFFECTIVE;
    ck(L("v2"), rv(setxattr(f, "security.capability", &caps, 20, 0)), 0);
    memset(buf, 0, sizeof buf);
    ck(L("v2 reads back"), rv(getxattr(f, "security.capability", buf, sizeof buf)), 20);
    ck(L("v2 reads back unchanged"), memcmp(buf, &caps, 20), 0);
    ck(L("size query"), rv(getxattr(f, "security.capability", NULL, 0)), 20);
    char list[256];
    long n = rv(listxattr(f, list, sizeof list));
    ck(L("listed"), list_has(list, n, "security.capability"), 1);

    // A v3 whose root is the namespace's own root is stored as, and reads back
    // as, a v2.
    struct caps_v3 v3 = {.magic_etc = VFS_CAP_REVISION_3 | VFS_CAP_FLAGS_EFFECTIVE, .rootid = 0};
    v3.data[0].permitted = CAP_NET_BIND_SERVICE_BIT;
    v3.data[0].inheritable = 1u << 1;
    ck(L("v3, rootid 0"), rv(setxattr(f, "security.capability", &v3, 24, 0)), 0);
    memset(buf, 0, sizeof buf);
    ck(L("v3 rootid 0 reads back as v2"), rv(getxattr(f, "security.capability", buf, sizeof buf)), 20);
    struct caps_v3 back;
    memcpy(&back, buf, sizeof back);
    ck(L("...with a v2 magic"), back.magic_etc, VFS_CAP_REVISION_2 | VFS_CAP_FLAGS_EFFECTIVE);
    ck(L("...and the same permitted"), back.data[0].permitted, CAP_NET_BIND_SERVICE_BIT);
    ck(L("...and the same inheritable"), back.data[0].inheritable, 1u << 1);
    if (init_ns_root) {
        // Anyone else's root id is kept, and read back as the v3 it is.
        v3.rootid = 1000;
        v3.magic_etc = VFS_CAP_REVISION_3;
        ck(L("v3, rootid 1000"), rv(setxattr(f, "security.capability", &v3, 24, 0)), 0);
        memset(buf, 0, sizeof buf);
        ck(L("v3 rootid 1000 reads back as v3"), rv(getxattr(f, "security.capability", buf, sizeof buf)), 24);
        memcpy(&back, buf, sizeof back);
        ck(L("...with its magic"), back.magic_etc, VFS_CAP_REVISION_3);
        ck(L("...and its root id"), back.rootid, 1000);
        ck(L("v3 size query"), rv(getxattr(f, "security.capability", NULL, 0)), 24);
        ck(L("v3 short buffer"), rv(getxattr(f, "security.capability", buf, 20)), -ERANGE);
    }

    // Writing to the file takes its capabilities away -- even root's write --
    // and leaves every other attribute alone.
    ck(L("user.* beside it"), rv(setxattr(f, "user.keep", "k", 1, 0)), 0);
    ck(L("v2 again"), rv(setxattr(f, "security.capability", &caps, 20, 0)), 0);
    int fd = open(f, O_WRONLY);
    ck(L("write"), rv(write(fd, "x", 1)), 1);
    close(fd);
    ck(L("gone after write"), rv(getxattr(f, "security.capability", buf, sizeof buf)), -ENODATA);
    ck(L("user.* survives the write"), rv(getxattr(f, "user.keep", buf, sizeof buf)), 1);

    ck(L("v2 for truncate"), rv(setxattr(f, "security.capability", &caps, 20, 0)), 0);
    ck(L("truncate"), rv(truncate(f, 0)), 0);
    ck(L("gone after truncate"), rv(getxattr(f, "security.capability", buf, sizeof buf)), -ENODATA);

    ck(L("v2 for chown"), rv(setxattr(f, "security.capability", &caps, 20, 0)), 0);
    ck(L("chown to the same owner"), rv(chown(f, geteuid(), getegid())), 0);
    ck(L("gone after chown"), rv(getxattr(f, "security.capability", buf, sizeof buf)), -ENODATA);

    ck(L("v2 for chown -1 -1"), rv(setxattr(f, "security.capability", &caps, 20, 0)), 0);
    ck(L("chown(-1, -1)"), rv(chown(f, (uid_t) -1, (gid_t) -1)), 0);
    ck(L("gone after chown(-1, -1)"), rv(getxattr(f, "security.capability", buf, sizeof buf)), -ENODATA);

    ck(L("v2 for chmod"), rv(setxattr(f, "security.capability", &caps, 20, 0)), 0);
    ck(L("chmod"), rv(chmod(f, 0700)), 0);
    ck(L("kept across chmod"), rv(getxattr(f, "security.capability", buf, sizeof buf)), 20);
    ck(L("utimes"), rv(utimes(f, NULL)), 0);
    ck(L("kept across utimes"), rv(getxattr(f, "security.capability", buf, sizeof buf)), 20);
    ck(L("opening for write alone"), rv(fd = open(f, O_WRONLY)) >= 0, 1);
    close(fd);
    ck(L("kept by an open without a write"), rv(getxattr(f, "security.capability", buf, sizeof buf)), 20);
    ck(L("remove"), rv(removexattr(f, "security.capability")), 0);
    ck(L("gone after remove"), rv(getxattr(f, "security.capability", buf, sizeof buf)), -ENODATA);

    // Not only on regular files.
    const char *d = P(1, "capdir");
    ck(L("mkdir"), rv(mkdir(d, 0755)), 0);
    ck(L("capability on a directory"), rv(setxattr(d, "security.capability", &caps, 20, 0)), 0);
#undef L
}

// The pseudo-filesystems keep no attributes, and say so -- but only once the
// caller has passed the checks every filesystem makes first. Linux asks for
// permission, then applies the namespace's rules, and only then finds out
// whether the filesystem has attributes at all: so an unprivileged write to a
// 0444 /proc file is EACCES while root's is EOPNOTSUPP, and a pipe answers as
// any non-file does (user.* is for files and directories) without its
// filesystem being asked. A list of none is empty, not an error.
static void check_pseudo(void) {
    char buf[64], list[256];
    ck("proc: get", rv(getxattr("/proc/self/status", "user.x", buf, sizeof buf)), -EOPNOTSUPP);
    ck("proc: set", rv(setxattr("/proc/self/status", "user.x", "x", 1, 0)),
       is_root ? -EOPNOTSUPP : -EACCES);
    ck("proc: list", rv(listxattr("/proc/self/status", list, sizeof list)), 0);
    ck("proc: remove", rv(removexattr("/proc/self/status", "user.x")),
       is_root ? -EOPNOTSUPP : -EACCES);
    int p[2];
    if (pipe(p) == 0) {
        ck("pipe: fget", rv(fgetxattr(p[0], "user.x", buf, sizeof buf)), -ENODATA);
        ck("pipe: fset", rv(fsetxattr(p[0], "user.x", "x", 1, 0)), -EPERM);
        ck("pipe: flist", rv(flistxattr(p[0], list, sizeof list)), 0);
        close(p[0]);
        close(p[1]);
    }
}

static void run_all(const char *fs) {
    check_basic(fs);
    check_object_types(fs);
    check_fd_forms(fs);
    check_ctime(fs);
    if (is_root) {
        check_privileged(fs);
        check_capability(fs);
        // The unprivileged rules, as someone else: in a directory they may
        // write, with files of their own.
        if (init_ns_root) {
            char saved[sizeof base];
            snprintf(saved, sizeof saved, "%s", base);
            char sub[300];
            snprintf(sub, sizeof sub, "%s/user", base);
            ck("user dir", rv(mkdir(sub, 0777)), 0);
            ck("user dir chmod", rv(chmod(sub, 0777)), 0);
            snprintf(base, sizeof base, "%s", sub);
            AS_USER(check_unprivileged(fs));
            snprintf(base, sizeof base, "%s", saved);
        }
    } else {
        check_unprivileged(fs);
    }
}

static void rm_rf(const char *path) {
    char cmd[400];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", path);
    if (system(cmd) < 0)
        return;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));
    is_root = geteuid() == 0;
    init_ns_root = is_root && in_initial_userns();
    // The unprivileged leg drops to uid 1000; it must be allowed to reach the
    // test directory.
    umask(022);

    const char *top = getenv("XATTR_OPS_DIR");
    snprintf(base, sizeof base, "%s/xattr_ops.%d", top != NULL ? top : "/tmp", (int) getpid());
    rm_rf(base);
    if (mkdir(base, 0755) != 0 || chmod(base, 0755) != 0) {
        printf("FAIL mkdir %s: %s\n", base, strerror(errno));
        return finish_suite("xattr_ops");
    }
    char disk[sizeof base];
    snprintf(disk, sizeof disk, "%s", base);
    run_all("disk");

    // The same again on a tmpfs, when this caller may mount one.
    char tmp[sizeof base + 8];
    snprintf(tmp, sizeof tmp, "%s/tmpfs", disk);
    if (mkdir(tmp, 0755) == 0 && mount("xattr_ops", tmp, "tmpfs", 0, "mode=0755") == 0) {
        snprintf(base, sizeof base, "%s", tmp);
        run_all("tmpfs");
        if (umount(tmp) != 0)
            printf("FAIL umount %s: %s\n", tmp, strerror(errno));
    } else {
        test_logf("tmpfs leg skipped: %s\n", strerror(errno));
    }

    check_pseudo();
    rm_rf(disk);
    return finish_suite("xattr_ops");
}
