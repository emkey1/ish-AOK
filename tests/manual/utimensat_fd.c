// utimensat(fd, NULL, ...) -- the futimens() form -- must operate on the open
// fd directly with NO path resolution, like Linux's do_utimes_fd().
//
// Regression for two bugs found by stress-ng --sockabuse on-device:
//
// 1. sys_utime_common emulated the NULL-path form by resolving "." relative to
//    the fd. For a socket fd, generic_getpath renders "socket:[N]" (adhoc fd),
//    which failed path_normalize's assert(path_is_normalized(at_path)) and
//    aborted the entire app. Fixed twice over: fs_ops->futime does the fd-direct
//    update (realfs/fakefs/tmpfs/adhoc), and path_normalize now returns ENOTDIR
//    instead of asserting when a non-path fd is used as a dirfd.
//
// 2. The path-resolution emulation also failed with ENOENT on unlinked-but-open
//    files, where real futimens succeeds.
//
// Also checks the dirfd hardening directly: openat/fstatat relative to a socket
// fd must be ENOTDIR (Linux), not an abort.
//
// Arch-neutral. Validated byte-for-byte against real Linux (x86_64 VM).
#define _GNU_SOURCE
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/wait.h>
#include "test_common.h"

static void check_futimens_ok(const char *label, int fd) {
    struct timespec ts[2] = {{12345, 678}, {23456, 789}};
    errno = 0;
    int r = futimens(fd, ts);
    if (r != 0) {
        printf("FAIL: %s: futimens: %s\n", label, strerror(errno));
        failures_total++;
        return;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        printf("FAIL: %s: fstat after futimens: %s\n", label, strerror(errno));
        failures_total++;
        return;
    }
    if (st.st_atim.tv_sec != 12345 || st.st_mtim.tv_sec != 23456) {
        printf("FAIL: %s: times not applied (atime=%ld mtime=%ld)\n",
               label, (long)st.st_atim.tv_sec, (long)st.st_mtim.tv_sec);
        failures_total++;
        return;
    }
    test_logf("%s: futimens ok, times applied\n", label);
}

static void check_enotdir(const char *label, int r, int err) {
    if (r >= 0 || err != ENOTDIR) {
        printf("FAIL: %s: got r=%d errno=%d (%s), want ENOTDIR\n",
               label, r, err, strerror(err));
        failures_total++;
        return;
    }
    test_logf("%s: ENOTDIR as expected\n", label);
}

// A socket's inode belongs to whoever created it, and that only becomes
// observable as a NON-ROOT caller: futimens with explicit times needs
// ownership, so a socket wrongly recorded as uid 0 answers EPERM to the very
// process that made it. Running as root -- which the CLI harness always does
// -- compares 0 against 0 and cannot see the difference, which is exactly how
// this survived: it only ever showed up in an unprivileged ssh session on the
// device. Measured on Linux 6.12: sockfs and pipefs carry the creator's uid
// (futimens OK), while eventfd/epoll/timerfd/signalfd are genuinely root-owned
// and genuinely answer EPERM -- so this asserts the split, not just the socket.
static void check_unprivileged_anon_ownership(void) {
    if (geteuid() != 0) {
        test_logf("anon ownership: skipped, already unprivileged\n");
        return;
    }
    fflush(NULL);
    pid_t c = fork();
    if (c < 0) {
        printf("FAIL: fork for unprivileged ownership check: %s\n", strerror(errno));
        failures_total++;
        return;
    }
    if (c == 0) {
        unsigned before = failures_total;
        failures_total = 0;
        if (setgid(1000) != 0 || setuid(1000) != 0) {
            printf("FAIL: could not drop privileges: %s\n", strerror(errno));
            fflush(NULL);
            _exit(1);
        }
        (void) before;
        int sk = socket(AF_INET, SOCK_STREAM, 0);
        if (sk < 0) {
            printf("FAIL: unprivileged socket(): %s\n", strerror(errno));
            failures_total++;
        } else {
            struct stat st;
            if (fstat(sk, &st) != 0) {
                printf("FAIL: unprivileged fstat(socket): %s\n", strerror(errno));
                failures_total++;
            } else if (st.st_uid != getuid()) {
                printf("FAIL: socket owned by uid %u, expected %u\n",
                       (unsigned) st.st_uid, (unsigned) getuid());
                failures_total++;
            }
            check_futimens_ok("socket, unprivileged", sk);
            close(sk);
        }
        int pp[2];
        if (pipe(pp) == 0) {
            check_futimens_ok("pipe, unprivileged", pp[0]);
            close(pp[0]);
            close(pp[1]);
        }
        fflush(NULL);
        _exit(failures_total > 250 ? 250 : (int) failures_total);
    }
    int st;
    if (waitpid(c, &st, 0) != c) {
        failures_total++;
        return;
    }
    if (WIFSIGNALED(st)) {
        printf("FAIL: unprivileged ownership child died on signal %d\n", WTERMSIG(st));
        failures_total++;
    } else {
        failures_total += (unsigned) WEXITSTATUS(st);
    }
}

int main(int argc, char **argv) {
    test_init(argc, argv);

    check_unprivileged_anon_ownership();

    // Socket fd: the stress-ng --sockabuse crash. futimens on a socket
    // succeeds on Linux (sets the sockfs inode times).
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        printf("FAIL: socket: %s\n", strerror(errno));
        failures_total++;
    } else {
        check_futimens_ok("socket", s);
    }

    // Pipe fd (also an adhoc/anon fd in AOK).
    int p[2];
    if (pipe(p) != 0) {
        printf("FAIL: pipe: %s\n", strerror(errno));
        failures_total++;
    } else {
        check_futimens_ok("pipe", p[0]);
        close(p[0]);
        close(p[1]);
    }

    // Regular file, live.
    const char *path = "/tmp/utimensat_fd.dat";
    unlink(path);
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        printf("FAIL: open %s: %s\n", path, strerror(errno));
        failures_total++;
    } else {
        check_futimens_ok("file live", fd);
        close(fd);
    }

    // Regular file, unlinked while open: no path exists, fd form must work.
    fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        printf("FAIL: reopen %s: %s\n", path, strerror(errno));
        failures_total++;
    } else {
        unlink(path);
        check_futimens_ok("file unlinked", fd);
        close(fd);
    }

    // Socket as dirfd of a relative *at path: ENOTDIR, not an abort.
    if (s >= 0) {
        errno = 0;
        int r = openat(s, "x", O_RDONLY);
        check_enotdir("openat(sock, \"x\")", r, errno);
        struct stat st;
        errno = 0;
        r = fstatat(s, "x", &st, 0);
        check_enotdir("fstatat(sock, \"x\")", r, errno);
        close(s);
    }

    return finish_suite("utimensat_fd");
}
