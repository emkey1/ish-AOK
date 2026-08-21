// AT_EMPTY_PATH support for the *at metadata syscalls (fchownat/fchmodat).
//
// Regression for sys_fchownat: it resolved an empty pathname relative to the
// dir fd and returned ENOENT, with no AT_EMPTY_PATH branch (fchmodat already
// had one). glibc/systemd use fchownat(fd, "", uid, gid, AT_EMPTY_PATH) as
// fchown-by-fd. systemd-sysusers' copy_rights() does exactly that when copying
// /etc/gshadow's permissions onto its temp file, so creating the sshd/polkitd
// users failed with "Failed to copy permissions ...: No such file or directory"
// and openssh-server/polkitd would not configure.
//
// Arch-neutral; runs as root in the guest (chown to arbitrary ids).
#define _GNU_SOURCE
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <sys/syscall.h>
#include "test_common.h"

#ifndef AT_EMPTY_PATH
#define AT_EMPTY_PATH 0x1000
#endif

// Linux's fchmodat(2) syscall takes no flags argument, so AT_EMPTY_PATH for
// chmod is only reachable through fchmodat2(2) (kernel 6.6+). glibc >= 2.39
// routes fchmodat(..., AT_EMPTY_PATH) there for you; musl 1.1.24 -- what the
// Alpine 3.11 i386 e2e root ships -- instead rejects any flag other than
// AT_SYMLINK_NOFOLLOW with EINVAL in userspace, never issuing a syscall at
// all. That made this test report a kernel regression that was not there:
// verified against Linux 6.12/glibc 2.41 and against iSH-AOK, raw fchmodat2
// applies the mode on both, and raw fchmodat with an empty path is ENOENT on
// both. So fall back to the raw syscall when the libc declines to make one --
// the kernel behaviour is what this test locks, not the libc's routing.
#ifndef SYS_fchmodat2
# ifdef __NR_fchmodat2
#  define SYS_fchmodat2 __NR_fchmodat2
# else
#  define SYS_fchmodat2 452 // "common" number on i386 and amd64
# endif
#endif

static int fchmodat_empty_path(int fd, mode_t mode) {
    errno = 0;
    if (fchmodat(fd, "", mode, AT_EMPTY_PATH) == 0)
        return 0;
    if (errno != EINVAL)
        return -1;
    // Old libc, not an old kernel: ask the kernel directly.
    errno = 0;
    return (int) syscall(SYS_fchmodat2, fd, "", mode, AT_EMPTY_PATH);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    if (geteuid() != 0) {
        // Changing a file's owner needs privilege, so unprivileged this can only
        // ever report EPERM. Skip rather than fail: the iSH-AOK suite runs as uid 0,
        // and failing here reads like a real AT_EMPTY_PATH regression when it is
        // just the account the suite was launched under.
        printf("at_empty_path: SKIP (not privileged: euid=%d)\n", (int) geteuid());
        return 0;
    }
    const char *path = "/tmp/at_empty_path.dat";
    unlink(path);

    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        printf("FAIL: open: %s\n", strerror(errno));
        failures_total++;
        return finish_suite("at_empty_path");
    }

    struct stat st;

    // fchownat(fd, "", ..., AT_EMPTY_PATH) -- the systemd copy_rights idiom.
    errno = 0;
    if (fchownat(fd, "", 12345, 54321, AT_EMPTY_PATH) < 0) {
        printf("FAIL: fchownat(fd, \"\", AT_EMPTY_PATH) = -1 (%s)\n", strerror(errno));
        failures_total++;
    } else if (fstat(fd, &st) == 0 && (st.st_uid != 12345 || st.st_gid != 54321)) {
        printf("FAIL: owner not applied: uid=%d gid=%d (want 12345/54321)\n",
               (int) st.st_uid, (int) st.st_gid);
        failures_total++;
    } else {
        test_logf("fchownat AT_EMPTY_PATH -> uid=%d gid=%d ok\n", (int) st.st_uid, (int) st.st_gid);
    }

    // Companion: fchmodat(fd, "", ..., AT_EMPTY_PATH).
    if (fchmodat_empty_path(fd, 0641) < 0) {
        printf("FAIL: fchmodat(fd, \"\", AT_EMPTY_PATH) = -1 (%s)\n", strerror(errno));
        failures_total++;
    } else if (fstat(fd, &st) == 0 && (st.st_mode & 07777) != 0641) {
        printf("FAIL: mode not applied: %o (want 641)\n", st.st_mode & 07777);
        failures_total++;
    }

    // An empty path WITHOUT AT_EMPTY_PATH must still be rejected with ENOENT.
    errno = 0;
    if (fchownat(fd, "", 0, 0, 0) == 0) {
        printf("FAIL: empty path without AT_EMPTY_PATH unexpectedly succeeded\n");
        failures_total++;
    } else if (errno != ENOENT) {
        printf("FAIL: empty path without AT_EMPTY_PATH: %s (want ENOENT)\n", strerror(errno));
        failures_total++;
    }

    close(fd);
    unlink(path);
    return finish_suite("at_empty_path");
}
