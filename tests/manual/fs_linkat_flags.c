// linkat()'s flags argument, which AOK did not have.
//
// sys_linkat took four parameters -- two dirfds and two names -- so the fifth
// one the syscall actually carries never reached the kernel at all. Both flags
// linkat defines were silently dropped, and a flag it does NOT define was
// silently accepted:
//
//   AT_SYMLINK_FOLLOW (0x400) says "link what the source POINTS AT". link() is
//   the one member of the *at() family that stops at a final symlink, and that
//   is its default; the flag is how a caller asks for the other thing. Without
//   it every one of these linked the symlink instead. Measured on Linux 6.12
//   (x86_64 glibc, 64-bit and -m32 identical) against AOK at 562a4eb5, with
//   l2f -> a regular file, l2d -> a directory and dang -> a missing name:
//
//     linkat(l2f, new, AT_SYMLINK_FOLLOW)    Linux: new is the FILE  AOK: a symlink
//     linkat(l2d, new, AT_SYMLINK_FOLLOW)    Linux: EPERM            AOK: succeeded
//     linkat(dang, new, AT_SYMLINK_FOLLOW)   Linux: ENOENT           AOK: succeeded
//
//   The first is not a corner: linkat("/proc/self/fd/N", ..., AT_SYMLINK_FOLLOW)
//   is how a caller gives a name to an inode it holds open (see
//   open_tmpfile.c), and dropping the flag hard-linked the magic symlink.
//
//   AT_EMPTY_PATH (0x1000) does the same job without procfs: an empty source
//   name means the descriptor itself. Linux allows it only to a caller holding
//   CAP_DAC_READ_SEARCH or still running on the very cred object the open ran
//   on, so that handing somebody a descriptor does not also hand them the
//   right to give its inode a name they can reach afterwards.
//
//   AT_SYMLINK_NOFOLLOW (0x100) is not one of linkat's flags -- do_linkat()
//   rejects it -- and AOK took it without complaint, telling a caller probing
//   for a kernel that follows by default that it had been answered.
//
// do_linkat() decides the flags before it looks at either name, so an unknown
// bit is EINVAL ahead of ENOENT, EEXIST, EBADF and even EFAULT; all of that is
// pinned below. So is the trailing-slash rule that must not move:
// fs_nofollow_trailing_slash.c owns it, and the flag does not change it,
// because lookup_last() sets LOOKUP_FOLLOW from the slash either way.
//
// Measured on Linux 6.12 (camd, x86_64 glibc, 64-bit and -m32 identical). The
// privileged arms were measured under `unshare -Ur`, which is where
// CAP_DAC_READ_SEARCH comes from without a password.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#include "test_common.h"

#ifndef AT_SYMLINK_FOLLOW
#define AT_SYMLINK_FOLLOW 0x400
#endif
#ifndef AT_EMPTY_PATH
#define AT_EMPTY_PATH 0x1000
#endif
#ifndef O_PATH
#define O_PATH 010000000
#endif

// The marker our own execve hands us; see the exec arm at the bottom. Checked
// before test_init(), which rejects anything it does not recognise.
#define EXEC_STAGE "--linkat-empty-path-after-exec"

static char base[160];
static char scratch[400];
static char scratch2[400];
static const char *P(const char *sub) {
    snprintf(scratch, sizeof scratch, "%s/%s", base, sub);
    return scratch;
}
// A second buffer, because a call takes a source AND a destination and C does
// not say which argument is evaluated first.
static const char *Q(const char *sub) {
    snprintf(scratch2, sizeof scratch2, "%s/%s", base, sub);
    return scratch2;
}

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-62s got=%-9ld want=%ld\n", label, got, want);
}

// Every linkat goes through the raw syscall: the flags are the thing under
// test, and no libc is allowed to reinterpret them on the way past.
static long lat(int ofd, const char *op, int nfd, const char *np, int flags) {
    errno = 0;
    return syscall(SYS_linkat, ofd, op, nfd, np, flags);
}
// ...reported as 0 for success, or the errno.
static long late(int ofd, const char *op, int nfd, const char *np, int flags) {
    long r = lat(ofd, op, nfd, np, flags);
    return r < 0 ? errno : 0;
}

// What did `name` turn into? S_IFMT of an lstat, or -errno.
static long kind_of(const char *name) {
    struct stat st;
    if (lstat(name, &st) < 0)
        return -errno;
    return (long) (st.st_mode & S_IFMT);
}
static long nlink_of(const char *name) {
    struct stat st;
    if (lstat(name, &st) < 0)
        return -errno;
    return (long) st.st_nlink;
}
static int same_inode(const char *a, const char *b) {
    struct stat sa, sb;
    if (lstat(a, &sa) < 0 || lstat(b, &sb) < 0)
        return 0;
    return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

// Run `fn` in a forked child and report the errno it saw (0 for success), or
// -1 if it died. The AT_EMPTY_PATH credential rule is about WHO is asking, so
// the asking has to happen somewhere else.
static long in_child(long (*fn)(void *), void *arg) {
    fflush(NULL);
    pid_t c = fork();
    if (c == 0)
        _exit((int) fn(arg));
    int st;
    if (waitpid(c, &st, 0) != c)
        return -2;
    if (WIFSIGNALED(st))
        return -1;
    return WEXITSTATUS(st);
}

struct fd_and_name { int fd; const char *name; };

static long child_links_inherited_fd(void *arg) {
    struct fd_and_name *a = arg;
    return late(a->fd, "", AT_FDCWD, a->name, AT_EMPTY_PATH);
}

// Open a descriptor and then exec THIS binary in place: same pid, same thread
// group, every uid and gid unchanged, and Linux still refuses -- execve
// replaces the cred object, and the rule compares the object.
static char exec_self[400];
static long child_execs_then_links(void *arg) {
    struct fd_and_name *a = arg;
    int fd = open(P("file"), O_RDONLY);
    if (fd < 0)
        return 99;
    char fdbuf[32];
    snprintf(fdbuf, sizeof fdbuf, "%d", fd);
    fflush(NULL);
    execl(exec_self, exec_self, EXEC_STAGE, fdbuf, a->name, (char *) NULL);
    return 98;
}

int main(int argc, char **argv) {
    // The far side of the execve above, before test_init sees the argument.
    if (argc >= 4 && strcmp(argv[1], EXEC_STAGE) == 0)
        return (int) late(atoi(argv[2]), "", AT_FDCWD, argv[3], AT_EMPTY_PATH);

    test_init(argc, argv);
    alarm(test_watchdog_secs(60));

    // Where to find ourselves again for the exec arm. The runner invokes tests
    // by absolute path; procfs is the fallback.
    if (argv[0] != NULL && strchr(argv[0], '/') != NULL)
        snprintf(exec_self, sizeof exec_self, "%s", argv[0]);
    else
        snprintf(exec_self, sizeof exec_self, "/proc/self/exe");

    int privileged = geteuid() == 0;

    snprintf(base, sizeof base, "/tmp/linkatflags-%d", (int) getpid());
    { char cmd[400]; snprintf(cmd, sizeof cmd, "rm -rf '%s'", base); if (system(cmd) < 0) {} }
    ck("scratch directory", mkdir(base, 0755), 0);

    int f = open(P("file"), O_RDWR | O_CREAT, 0644);
    ck("a plain file", f >= 0, 1);
    if (f >= 0)
        close(f);
    ck("a directory", mkdir(P("dir"), 0755), 0);
    ck("a symlink to the file", symlink("file", P("l2f")), 0);
    ck("a symlink to the directory", symlink("dir", P("l2d")), 0);
    ck("a dangling symlink", symlink("nothing-here", P("dang")), 0);

    // ---- AT_SYMLINK_FOLLOW: link what the source points at -----------------
    ck("FOLLOW on a symlink to a file succeeds",
       late(AT_FDCWD, P("l2f"), AT_FDCWD, Q("f-follow"), AT_SYMLINK_FOLLOW), 0);
    ck("  ...and the new name is a REGULAR file, not a symlink",
       kind_of(P("f-follow")), S_IFREG);
    ck("  ...and it is the same inode as the target",
       same_inode(P("f-follow"), Q("file")), 1);
    ck("  ...so the target now has two links", nlink_of(P("file")), 2);
    ck("  ...and the symlink itself still has one", nlink_of(P("l2f")), 1);

    ck("FOLLOW on a symlink to a DIRECTORY is EPERM",
       late(AT_FDCWD, P("l2d"), AT_FDCWD, Q("d-follow"), AT_SYMLINK_FOLLOW), EPERM);
    ck("  ...and made nothing", kind_of(P("d-follow")), -ENOENT);
    ck("FOLLOW on a DANGLING symlink is ENOENT",
       late(AT_FDCWD, P("dang"), AT_FDCWD, Q("x-follow"), AT_SYMLINK_FOLLOW), ENOENT);
    ck("  ...and made nothing", kind_of(P("x-follow")), -ENOENT);
    ck("FOLLOW on a real directory is EPERM",
       late(AT_FDCWD, P("dir"), AT_FDCWD, Q("dd-follow"), AT_SYMLINK_FOLLOW), EPERM);
    ck("FOLLOW on a plain file links it",
       late(AT_FDCWD, P("file"), AT_FDCWD, Q("f-plain"), AT_SYMLINK_FOLLOW), 0);
    ck("  ...as a regular file", kind_of(P("f-plain")), S_IFREG);

    {
        int d = open(base, O_RDONLY | O_DIRECTORY);
        ck("a dirfd on the scratch directory", d >= 0, 1);
        ck("FOLLOW through a dirfd and a relative name",
           late(d, "l2f", d, "rel-follow", AT_SYMLINK_FOLLOW), 0);
        ck("  ...links the file", kind_of(P("rel-follow")), S_IFREG);
        ck("FOLLOW through a dirfd, symlink to a directory, is EPERM",
           late(d, "l2d", d, "rel-dir", AT_SYMLINK_FOLLOW), EPERM);
        if (d >= 0)
            close(d);
    }

    // ---- no flag: link() links the LINK, and must keep doing so ------------
    ck("without the flag a symlink to a file is linked as a SYMLINK",
       late(AT_FDCWD, P("l2f"), AT_FDCWD, Q("f-nolink"), 0), 0);
    ck("  ...a symlink", kind_of(P("f-nolink")), S_IFLNK);
    ck("without the flag a symlink to a DIRECTORY links fine",
       late(AT_FDCWD, P("l2d"), AT_FDCWD, Q("d-nolink"), 0), 0);
    ck("  ...as a symlink", kind_of(P("d-nolink")), S_IFLNK);
    ck("without the flag a DANGLING symlink links fine",
       late(AT_FDCWD, P("dang"), AT_FDCWD, Q("x-nolink"), 0), 0);
    ck("  ...as a symlink", kind_of(P("x-nolink")), S_IFLNK);

    // ---- the trailing-slash rule the flag must not disturb -----------------
    // lookup_last() turns the slash into LOOKUP_FOLLOW|LOOKUP_DIRECTORY with
    // or without AT_SYMLINK_FOLLOW, so all six of these are the same either
    // way. fs_nofollow_trailing_slash.c owns the rule; this is the regression
    // guard for touching the source's resolution.
    ck("no flag,  \"l2d/\" is EPERM",
       late(AT_FDCWD, P("l2d/"), AT_FDCWD, Q("s1"), 0), EPERM);
    ck("no flag,  \"l2f/\" is ENOTDIR",
       late(AT_FDCWD, P("l2f/"), AT_FDCWD, Q("s2"), 0), ENOTDIR);
    ck("FOLLOW,   \"l2d/\" is EPERM",
       late(AT_FDCWD, P("l2d/"), AT_FDCWD, Q("s3"), AT_SYMLINK_FOLLOW), EPERM);
    ck("FOLLOW,   \"l2f/\" is ENOTDIR",
       late(AT_FDCWD, P("l2f/"), AT_FDCWD, Q("s4"), AT_SYMLINK_FOLLOW), ENOTDIR);
    ck("FOLLOW,   \"file/\" is ENOTDIR",
       late(AT_FDCWD, P("file/"), AT_FDCWD, Q("s5"), AT_SYMLINK_FOLLOW), ENOTDIR);
    ck("no flag,  \"dang/\" is ENOENT",
       late(AT_FDCWD, P("dang/"), AT_FDCWD, Q("s6"), 0), ENOENT);

    // ---- the flags are decided before either name --------------------------
    ck("AT_SYMLINK_NOFOLLOW is not a linkat flag: EINVAL",
       late(AT_FDCWD, P("file"), AT_FDCWD, Q("e1"), AT_SYMLINK_NOFOLLOW), EINVAL);
    ck("AT_REMOVEDIR is EINVAL",
       late(AT_FDCWD, P("file"), AT_FDCWD, Q("e2"), 0x200), EINVAL);
    ck("AT_NO_AUTOMOUNT is EINVAL",
       late(AT_FDCWD, P("file"), AT_FDCWD, Q("e3"), 0x800), EINVAL);
    ck("an unassigned bit is EINVAL",
       late(AT_FDCWD, P("file"), AT_FDCWD, Q("e4"), 0x2000), EINVAL);
    ck("every bit at once is EINVAL",
       late(AT_FDCWD, P("file"), AT_FDCWD, Q("e5"), -1), EINVAL);
    ck("a bad bit beside a good one is EINVAL",
       late(AT_FDCWD, P("file"), AT_FDCWD, Q("e6"), AT_SYMLINK_NOFOLLOW | AT_SYMLINK_FOLLOW),
       EINVAL);
    ck("  ...and none of those made anything", kind_of(P("e1")), -ENOENT);
    ck("a bad flag beats a missing source",
       late(AT_FDCWD, P("no-such-thing"), AT_FDCWD, Q("e7"), AT_SYMLINK_NOFOLLOW), EINVAL);
    ck("a bad flag beats a destination that exists",
       late(AT_FDCWD, P("file"), AT_FDCWD, Q("l2f"), AT_SYMLINK_NOFOLLOW), EINVAL);
    ck("a bad flag beats a dirfd that is not a descriptor",
       late(-7, "relative", AT_FDCWD, Q("e8"), AT_SYMLINK_NOFOLLOW), EINVAL);
    ck("a bad flag beats an empty source name",
       late(AT_FDCWD, "", AT_FDCWD, Q("e9"), AT_SYMLINK_NOFOLLOW), EINVAL);
    ck("a bad flag beats an empty destination name",
       late(AT_FDCWD, P("file"), AT_FDCWD, "", AT_SYMLINK_NOFOLLOW), EINVAL);
    ck("a bad flag beats an unreadable source pointer",
       late(AT_FDCWD, (char *) 1, AT_FDCWD, Q("e10"), AT_SYMLINK_NOFOLLOW), EINVAL);
    ck("with good flags an unreadable destination pointer is EFAULT",
       late(AT_FDCWD, P("file"), AT_FDCWD, (char *) 1, AT_SYMLINK_FOLLOW), EFAULT);

    // ---- an empty name is a name, until AT_EMPTY_PATH says otherwise -------
    ck("an empty source without the flag is ENOENT",
       late(AT_FDCWD, "", AT_FDCWD, Q("n1"), 0), ENOENT);
    ck("an empty source with only FOLLOW is ENOENT",
       late(AT_FDCWD, "", AT_FDCWD, Q("n2"), AT_SYMLINK_FOLLOW), ENOENT);
    ck("an empty destination is ENOENT",
       late(AT_FDCWD, P("file"), AT_FDCWD, "", 0), ENOENT);

    // ---- AT_EMPTY_PATH: the descriptor is the source -----------------------
    {
        int fd = open(P("file"), O_RDONLY);
        ck("a descriptor on the file", fd >= 0, 1);
        ck("AT_EMPTY_PATH links what the descriptor names",
           late(fd, "", AT_FDCWD, Q("ep-1"), AT_EMPTY_PATH), 0);
        ck("  ...the same inode", same_inode(P("ep-1"), Q("file")), 1);
        ck("  ...as a regular file", kind_of(P("ep-1")), S_IFREG);
        ck("AT_EMPTY_PATH|AT_SYMLINK_FOLLOW is the same thing",
           late(fd, "", AT_FDCWD, Q("ep-2"), AT_EMPTY_PATH | AT_SYMLINK_FOLLOW), 0);
        ck("  ...the same inode", same_inode(P("ep-2"), Q("file")), 1);
        ck("AT_EMPTY_PATH with a NON-empty name leaves the name in charge",
           late(fd, P("l2f"), AT_FDCWD, Q("ep-3"), AT_EMPTY_PATH), 0);
        ck("  ...so the symlink was linked, not followed", kind_of(P("ep-3")), S_IFLNK);
        ck("AT_EMPTY_PATH with a destination dirfd that is not one is EBADF",
           late(fd, "", -7, "relative", AT_EMPTY_PATH), EBADF);
        if (fd >= 0)
            close(fd);
    }
    {
        int fd = open(P("file"), O_PATH);
        ck("an O_PATH descriptor on the file", fd >= 0, 1);
        ck("AT_EMPTY_PATH works on an O_PATH descriptor",
           late(fd, "", AT_FDCWD, Q("ep-4"), AT_EMPTY_PATH), 0);
        ck("  ...the same inode", same_inode(P("ep-4"), Q("file")), 1);
        if (fd >= 0)
            close(fd);
    }
    {
        // The descriptor IS the object, so a descriptor that stopped at a
        // symlink links the symlink -- AT_SYMLINK_FOLLOW was spent at open().
        int fd = open(P("l2f"), O_PATH | O_NOFOLLOW);
        ck("an O_PATH|O_NOFOLLOW descriptor on a symlink", fd >= 0, 1);
        ck("AT_EMPTY_PATH on it links the SYMLINK",
           late(fd, "", AT_FDCWD, Q("ep-5"), AT_EMPTY_PATH), 0);
        ck("  ...a symlink", kind_of(P("ep-5")), S_IFLNK);
        if (fd >= 0)
            close(fd);
    }
    {
        int fd = open(P("dir"), O_RDONLY | O_DIRECTORY);
        ck("AT_EMPTY_PATH on a directory descriptor is EPERM",
           late(fd, "", AT_FDCWD, Q("ep-6"), AT_EMPTY_PATH), EPERM);
        if (fd >= 0)
            close(fd);
    }
    ck("AT_EMPTY_PATH with AT_FDCWD means the current directory, so EPERM",
       late(AT_FDCWD, "", AT_FDCWD, Q("ep-7"), AT_EMPTY_PATH), EPERM);
    {
        // A file unlinked since the descriptor was opened, whose name a new
        // file has taken. Linux gives no name to an inode with no links left
        // (ENOENT); AOK linked by the name the file had, so it linked the new
        // file.
        int fd = open(P("reused"), O_RDWR | O_CREAT, 0644);
        ck("a descriptor on a file about to be unlinked", fd >= 0, 1);
        ck("  ...unlinked", unlink(P("reused")), 0);
        int nf = open(P("reused"), O_RDWR | O_CREAT | O_EXCL, 0644);
        ck("  ...and a new file at its name", nf >= 0, 1);
        ck("AT_EMPTY_PATH on an unlinked file whose name is reused is ENOENT",
           late(fd, "", AT_FDCWD, Q("ep-reused"), AT_EMPTY_PATH), ENOENT);
        ck("  ...and the new file got no second name", kind_of(P("ep-reused")), -ENOENT);
        if (nf >= 0)
            close(nf);
        if (fd >= 0)
            close(fd);
        unlink(P("reused"));
    }

    // A descriptor with no filesystem object behind it: its inode is on
    // pipefs/sockfs, never on the filesystem the new name would go on.
    {
        int pf[2];
        ck("a pipe", pipe(pf), 0);
        ck("AT_EMPTY_PATH on a pipe is EXDEV",
           late(pf[0], "", AT_FDCWD, Q("ep-8"), AT_EMPTY_PATH), EXDEV);
        close(pf[0]);
        close(pf[1]);
    }
    {
        int s = socket(AF_UNIX, SOCK_STREAM, 0);
        ck("a socket", s >= 0, 1);
        ck("AT_EMPTY_PATH on a socket is EXDEV",
           late(s, "", AT_FDCWD, Q("ep-9"), AT_EMPTY_PATH), EXDEV);
        if (s >= 0)
            close(s);
    }
    // An inode whose last name is gone cannot get another one.
    {
        ck("a file to unlink underneath a descriptor",
           close(open(P("doomed"), O_RDWR | O_CREAT, 0644)), 0);
        int fd = open(P("doomed"), O_RDONLY);
        ck("a descriptor on it", fd >= 0, 1);
        ck("unlink its only name", unlink(P("doomed")), 0);
        ck("AT_EMPTY_PATH on a descriptor with no names left is ENOENT",
           late(fd, "", AT_FDCWD, Q("ep-10"), AT_EMPTY_PATH), ENOENT);
        if (fd >= 0)
            close(fd);
    }

    // ---- who may use AT_EMPTY_PATH -----------------------------------------
    // CAP_DAC_READ_SEARCH, or the credentials the open ran under -- and a fork
    // and an execve each replace those even though every uid stays the same.
    // Measured both ways: plain, and under `unshare -Ur` for the capability.
    test_logf("  %-62s %s\n", "AT_EMPTY_PATH privilege arm",
              privileged ? "privileged (expecting success)" : "unprivileged (expecting ENOENT)");
    {
        int fd = open(P("file"), O_RDONLY);
        ck("a descriptor to hand to a child", fd >= 0, 1);
        char child_name[400];
        snprintf(child_name, sizeof child_name, "%s/ep-child", base);
        struct fd_and_name a = { fd, child_name };
        ck("AT_EMPTY_PATH on a descriptor inherited by a forked child",
           in_child(child_links_inherited_fd, &a), privileged ? 0 : ENOENT);
        ck("  ...so the name exists only when it was allowed",
           kind_of(P("ep-child")) == S_IFREG, privileged ? 1 : 0);
        if (fd >= 0)
            close(fd);
    }
    {
        char exec_name[400];
        snprintf(exec_name, sizeof exec_name, "%s/ep-exec", base);
        struct fd_and_name a = { -1, exec_name };
        ck("AT_EMPTY_PATH on a descriptor opened before this process exec'd",
           in_child(child_execs_then_links, &a), privileged ? 0 : ENOENT);
        ck("  ...so the name exists only when it was allowed",
           kind_of(P("ep-exec")) == S_IFREG, privileged ? 1 : 0);
    }

    // ---- the destination is never followed, flag or no flag ----------------
    ck("FOLLOW, a destination that is an existing symlink is EEXIST",
       late(AT_FDCWD, P("file"), AT_FDCWD, Q("l2f"), AT_SYMLINK_FOLLOW), EEXIST);
    ck("FOLLOW, a destination that is a DANGLING symlink is EEXIST",
       late(AT_FDCWD, P("file"), AT_FDCWD, Q("dang"), AT_SYMLINK_FOLLOW), EEXIST);
    ck("  ...and the dangling symlink is still a symlink", kind_of(P("dang")), S_IFLNK);

    { char cmd[400]; snprintf(cmd, sizeof cmd, "rm -rf '%s'", base); if (system(cmd) < 0) {} }
    return finish_suite("fs_linkat_flags");
}
