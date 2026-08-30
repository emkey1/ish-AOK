// Argument validation across the *at() family, copy_file_range and sendfile.
// Seven things AOK accepted that Linux rejects, and one that hung.
//
//   A non-directory dirfd gave EACCES instead of ENOTDIR, because path
//   resolution went straight to the search-permission check on the fd -- and a
//   0644 file has no execute bit. A plausible-looking errno for entirely the
//   wrong reason, which sends the caller to look at permissions rather than at
//   the descriptor it passed.
//
//   readlinkat took its bufsiz unsigned, so a negative size read as an
//   enormous one, got clamped to MAX_PATH and wrote the whole target into a
//   buffer the caller had just said was not there.
//
//   statfs used NOFOLLOW, so it reported the filesystem a symlink LIVES on
//   rather than the one it points at, and did not require the last component
//   to exist -- so `df /no/such/file` printed a filesystem and exited 0.
//
//   fstatat and fchownat accepted any flag word and silently ignored the bits
//   they did not know, telling a caller probing for a flag this kernel lacks
//   that it had been honoured.
//
//   copy_file_range mapped a directory and an O_APPEND output both to EINVAL
//   (or to success): the O_APPEND case wrote the bytes at the requested offset
//   instead of appending, silently putting them in the wrong place.
//
//   sendfile checked nothing about in_fd, so a pipe went to a blocking read
//   and sat there until somebody wrote enough bytes -- which, for a caller
//   expecting the immediate EINVAL Linux gives, is never.
//
// Measured against x86_64 glibc on Linux 6.12.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#include "test_common.h"

#ifndef PROCFS_MAGIC
#define PROCFS_MAGIC 0x9fa0
#endif

static char base[160];
static char scratch[340];
static const char *P(const char *sub) {
    snprintf(scratch, sizeof scratch, "%s/%s", base, sub);
    return scratch;
}

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-54s got=%-9ld want=%ld\n", label, got, want);
}

// Run something that might block forever, under its own alarm, and report its
// errno -- or -1 if it had to be killed. A hang is a result here, not a stall.
static int with_deadline(int (*fn)(void)) {
    fflush(NULL);
    pid_t c = fork();
    if (c == 0) {
        alarm(5);
        _exit(fn());
    }
    int st;
    if (waitpid(c, &st, 0) != c)
        return -2;
    if (WIFSIGNALED(st))
        return -1;
    return WEXITSTATUS(st);
}

static int sendfile_from_pipe(void) {
    int pf[2];
    if (pipe(pf) < 0)
        return 99;
    if (write(pf[1], "hello", 5) != 5)
        return 99;
    int out = open(P("sfout"), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (out < 0)
        return 99;
    errno = 0;
    ssize_t r = syscall(SYS_sendfile, out, pf[0], (void *) NULL, (size_t) 5);
    int e = errno;
    close(out);
    close(pf[0]);
    close(pf[1]);
    return r < 0 ? e : 0;
}

static int sendfile_from_devzero(void) {
    int in = open("/dev/zero", O_RDONLY);
    if (in < 0)
        return 99;
    int out = open(P("sfout2"), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (out < 0)
        return 99;
    errno = 0;
    ssize_t r = syscall(SYS_sendfile, out, in, (void *) NULL, (size_t) 16);
    int e = errno;
    close(out);
    close(in);
    return r == 16 ? 0 : (r < 0 ? e : 98);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));

    // The scratch directory must not collide with this binary's own path.
    snprintf(base, sizeof base, "/tmp/fsat-%d", (int) getpid());
    { char cmd[400]; snprintf(cmd, sizeof cmd, "rm -rf '%s'", base); if (system(cmd) < 0) {} }
    ck("scratch directory", mkdir(base, 0755), 0);

    int f = open(P("plain"), O_RDWR | O_CREAT, 0644);
    ck("a plain file", f >= 0, 1);
    if (f >= 0 && write(f, "0123456789", 10) != 10)
        failures_total++;
    ck("a subdirectory", mkdir(P("sub"), 0755), 0);
    ck("a symlink to /proc", symlink("/proc", P("toproc")), 0);

    // ---- a non-directory dirfd is ENOTDIR, decided before any lookup ------
    if (f >= 0) {
        struct stat st;
        errno = 0; ck("openat(file_fd, \"x\")", openat(f, "x", O_RDONLY) < 0 ? errno : 0, ENOTDIR);
        errno = 0; ck("fstatat(file_fd, \".\")", fstatat(f, ".", &st, 0) < 0 ? errno : 0, ENOTDIR);
        errno = 0; ck("faccessat(file_fd, \"y\")", faccessat(f, "y", F_OK, 0) < 0 ? errno : 0, ENOTDIR);
        errno = 0; ck("openat(file_fd, \"a/b\")", openat(f, "a/b", O_RDONLY) < 0 ? errno : 0, ENOTDIR);
    }

    // ---- readlinkat bufsiz --------------------------------------------------
    {
        char buf[64];
        memset(buf, 'Q', sizeof buf);
        // Through the raw syscall: musl's readlink() substitutes bufsize 1 for
        // 0 in userspace, so the wrapper never asks the kernel the question.
        errno = 0;
        ck("readlinkat bufsiz 0 is EINVAL",
           syscall(SYS_readlinkat, AT_FDCWD, P("toproc"), buf, (size_t) 0) < 0 ? errno : 0, EINVAL);
        ck("  and nothing was written", (unsigned char) buf[0], 'Q');
        errno = 0;
        ck("readlinkat a negative bufsiz is EINVAL",
           syscall(SYS_readlinkat, AT_FDCWD, P("toproc"), buf, (size_t) -1L) < 0 ? errno : 0, EINVAL);
        ck("  and nothing was written there either", (unsigned char) buf[0], 'Q');
        errno = 0;
        ck("a short buffer truncates without an error", readlink(P("toproc"), buf, 3), 3);
    }

    // ---- statfs follows the final symlink and needs the path to exist ------
    {
        struct statfs sb;
        errno = 0;
        ck("statfs of a nonexistent final component is ENOENT",
           statfs(P("no_such_thing"), &sb) < 0 ? errno : 0, ENOENT);
        errno = 0;
        int r = statfs(P("toproc"), &sb);
        ck("statfs follows a symlink", r < 0 ? errno : 0, 0);
        ck("  reporting the TARGET's filesystem",
           r == 0 ? (long) sb.f_type : -1, (long) PROCFS_MAGIC);
        // ...and the root of the tree still works, which the first attempt at
        // this broke: a normalized "/" is the empty string internally.
        errno = 0;
        ck("statfs(\"/\") still works", statfs("/", &sb) < 0 ? errno : 0, 0);
    }

    // ---- AT_ flag words are validated --------------------------------------
    {
        struct stat st;
        errno = 0;
        ck("fstatat with an unknown flag bit is EINVAL",
           fstatat(AT_FDCWD, P("plain"), &st, 0x8000) < 0 ? errno : 0, EINVAL);
        errno = 0;
        ck("fchownat with an unknown flag bit is EINVAL",
           fchownat(AT_FDCWD, P("plain"), -1, -1, 0x8000) < 0 ? errno : 0, EINVAL);
        // The flags that DO exist still work.
        errno = 0;
        ck("  AT_SYMLINK_NOFOLLOW still works",
           fstatat(AT_FDCWD, P("toproc"), &st, AT_SYMLINK_NOFOLLOW) < 0 ? errno : 0, 0);
        ck("  and saw the link itself", S_ISLNK(st.st_mode), 1);
    }

    // ---- copy_file_range --------------------------------------------------
    {
        int in = open(P("plain"), O_RDONLY);
        int app = open(P("appout"), O_WRONLY | O_CREAT | O_APPEND, 0644);
        errno = 0;
        ck("copy_file_range to an O_APPEND fd is EBADF",
           syscall(SYS_copy_file_range, in, NULL, app, NULL, (size_t) 4, 0u) < 0 ? errno : 0, EBADF);
        close(app);
        int dir = open(P("sub"), O_RDONLY | O_DIRECTORY);
        errno = 0;
        ck("  a directory operand is EISDIR",
           syscall(SYS_copy_file_range, dir, NULL, in, NULL, (size_t) 4, 0u) < 0 ? errno : 0, EISDIR);
        close(dir);
        int out = open(P("cfrout"), O_RDWR | O_CREAT | O_TRUNC, 0644);
        long long neg = -1;
        errno = 0;
        ck("  a negative offset is EOVERFLOW",
           syscall(SYS_copy_file_range, in, &neg, out, NULL, (size_t) 4, 0u) < 0 ? errno : 0, EOVERFLOW);
        // ...and an ordinary copy still works.
        long long zero = 0;
        errno = 0;
        long n = syscall(SYS_copy_file_range, in, &zero, out, NULL, (size_t) 10, 0u);
        ck("  an ordinary copy still works", n, 10);
        close(out);
        close(in);
    }

    // ---- sendfile's input must be seekable ---------------------------------
    {
        ck("sendfile from a pipe is EINVAL, and does not hang",
           with_deadline(sendfile_from_pipe), EINVAL);
        ck("  a character device is still accepted",
           with_deadline(sendfile_from_devzero), 0);
    }

    if (f >= 0)
        close(f);
    { char cmd[400]; snprintf(cmd, sizeof cmd, "rm -rf '%s'", base); if (system(cmd) < 0) {} }
    return finish_suite("fs_at_validation");
}
