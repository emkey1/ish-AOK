// O_PATH symlink fds + waitid(P_PIDFD): the systemd >= 260 boot layer after
// statx_mnt_id_timerfd.
//
// Three regressions, all found booting Arch Linux ARM aarch64 systemd as
// PID 1 on AOK:
//
// 1. openat(link, O_PATH|O_NOFOLLOW) returned ELOOP; Linux opens the symlink
//    ITSELF. systemd's chase() opens every path component this way, so any
//    chase ending on a symlink failed "Too many levels of symbolic links" --
//    non-fatally for /etc/os-release, fatally for the /etc/localtime
//    timezone-change watch (the app's provisioning creates that symlink),
//    killing manager startup.
// 2. readlinkat(fd, "", ...) on such an fd (how chase reads each link)
//    returned an error instead of the target.
// 3. waitid(P_PIDFD, pidfd, ...) -- how systemd waits for every child it
//    forks (generators, executor) -- returned EINVAL: do_wait only knew
//    P_ALL/P_PID/P_PGID ("Failed to wait for ...10-arch: Invalid argument").
//
// Known deviations NOT asserted: read() on a non-symlink O_PATH fd succeeds
// on AOK (Linux: EBADF); openat2 resolve flags return EINVAL (systemd falls
// back to openat). Also passes on real Linux.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include "test_common.h"

#ifndef O_PATH
#define O_PATH 010000000
#endif
#ifndef P_PIDFD
#define P_PIDFD 3
#endif
#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif

static void check(int cond, const char *what) {
    if (cond) {
        test_logf("ok: %s\n", what);
    } else {
        printf("FAIL: %s (errno=%d %s)\n", what, errno, strerror(errno));
        failures_total++;
    }
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(30));

    // fixture: target file + symlink, in cwd-independent /tmp
    unlink("/tmp/opw_link"); unlink("/tmp/opw_target");
    int tfd = open("/tmp/opw_target", O_WRONLY|O_CREAT|O_TRUNC, 0644);
    check(tfd >= 0 && write(tfd, "x\n", 2) == 2, "create target file");
    close(tfd);
    check(symlink("opw_target", "/tmp/opw_link") == 0, "create symlink");

    // 1. O_PATH|O_NOFOLLOW opens the symlink itself
    int lfd = openat(AT_FDCWD, "/tmp/opw_link", O_PATH|O_NOFOLLOW|O_CLOEXEC);
    check(lfd >= 0, "openat(link, O_PATH|O_NOFOLLOW) succeeds");
    if (lfd >= 0) {
        struct stat st;
        check(fstat(lfd, &st) == 0 && S_ISLNK(st.st_mode),
              "fstat(opath fd) reports S_IFLNK");

        // 2. readlinkat(fd, "") returns the target
        char buf[64];
        ssize_t n = readlinkat(lfd, "", buf, sizeof(buf)-1);
        check(n == 10 && memcmp(buf, "opw_target", 10) == 0,
              "readlinkat(opath fd, \"\") returns target");

        // /proc/self/fd/N resolves to the link's path
        char p[64], resolved[128];
        snprintf(p, sizeof(p), "/proc/self/fd/%d", lfd);
        n = readlink(p, resolved, sizeof(resolved)-1);
        check(n > 0 && strstr("/tmp/opw_link", (resolved[n]=0, resolved)) != NULL,
              "/proc/self/fd of opath fd shows the link path");
        close(lfd);
    }

    // plain O_NOFOLLOW (no O_PATH) must still refuse with ELOOP
    errno = 0;
    int r2 = openat(AT_FDCWD, "/tmp/opw_link", O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    check(r2 < 0 && errno == ELOOP, "O_RDONLY|O_NOFOLLOW on link still ELOOP");
    if (r2 >= 0) close(r2);

    // O_PATH|O_NOFOLLOW|O_DIRECTORY on a link: ENOTDIR
    errno = 0;
    r2 = openat(AT_FDCWD, "/tmp/opw_link", O_PATH|O_NOFOLLOW|O_DIRECTORY|O_CLOEXEC);
    check(r2 < 0 && errno == ENOTDIR, "O_PATH|O_NOFOLLOW|O_DIRECTORY on link is ENOTDIR");
    if (r2 >= 0) close(r2);

    // 3. waitid(P_PIDFD): fork a child, wait on its pidfd
    pid_t child = fork();
    if (child == 0) {
        usleep(100 * 1000);
        _exit(42);
    }
    check(child > 0, "fork child");
    int pidfd = (int) syscall(SYS_pidfd_open, child, 0);
    check(pidfd >= 0, "pidfd_open(child)");
    siginfo_t si;
    memset(&si, 0, sizeof(si));
    int wr = waitid((idtype_t) P_PIDFD, (id_t) pidfd, &si, WEXITED);
    check(wr == 0, "waitid(P_PIDFD, pidfd, WEXITED) succeeds");
    check(si.si_pid == child && si.si_code == CLD_EXITED && si.si_status == 42,
          "waitid reports the child's pid/status");
    if (pidfd >= 0) close(pidfd);

    // bad fd and non-pidfd fd are both EBADF (Linux pidfd_pid())
    errno = 0;
    wr = waitid((idtype_t) P_PIDFD, (id_t) 9999, &si, WEXITED);
    check(wr < 0 && errno == EBADF, "waitid(P_PIDFD, bad fd) is EBADF");
    int plain = open("/tmp/opw_target", O_RDONLY);
    errno = 0;
    wr = waitid((idtype_t) P_PIDFD, (id_t) plain, &si, WEXITED);
    check(wr < 0 && errno == EBADF, "waitid(P_PIDFD, non-pidfd) is EBADF");
    close(plain);

    unlink("/tmp/opw_link"); unlink("/tmp/opw_target");
    return finish_suite("opath_symlink_pidfd_wait");
}
