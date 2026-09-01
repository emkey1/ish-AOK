// kcmp(2): compare whether two processes share a kernel resource. systemd
// uses KCMP_FILE heavily (fd-store dedup, serialization across re-exec);
// AOK stubbed the syscall, so PID 1 logged an "arm64 stub syscall 272"
// ERROR line for every probe and systemd fell back to treating every fd as
// distinct. Linux's contract for the comparison types is an ordering:
// 0 == same object, 1/2 give a consistent (obfuscated) order, so equality
// is the only thing callers may rely on -- which is all this test pins.
//
// Also passes on real Linux (mint oracle, verified).
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_common.h"

#ifndef KCMP_FILE
#define KCMP_FILE 0
#define KCMP_VM 1
#define KCMP_FILES 2
#define KCMP_FS 3
#define KCMP_SIGHAND 4
#endif

static int kcmp(pid_t p1, pid_t p2, int type, unsigned long i1, unsigned long i2) {
    return (int) syscall(SYS_kcmp, p1, p2, type, i1, i2);
}

static void check(int cond, const char *what, int got) {
    if (!cond) {
        printf("FAIL: %s (got %d, errno %d %s)\n", what, got, errno, strerror(errno));
        failures_total++;
    } else if (test_verbose) {
        printf("ok %s\n", what);
    }
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(15));
    pid_t self = getpid();

    // A dup shares the open file description: KCMP_FILE == 0.
    int fd = open("/dev/null", O_RDONLY);
    int fd2 = dup(fd);
    int r = kcmp(self, self, KCMP_FILE, (unsigned long) fd, (unsigned long) fd2);
    check(r == 0, "dup'd fds compare equal", r);

    // A separate open of the same path is a different description.
    int fd3 = open("/dev/null", O_RDONLY);
    r = kcmp(self, self, KCMP_FILE, (unsigned long) fd, (unsigned long) fd3);
    check(r == 1 || r == 2, "separate opens compare unequal", r);
    // ...and the ordering is consistent when the args swap.
    int rr = kcmp(self, self, KCMP_FILE, (unsigned long) fd3, (unsigned long) fd);
    check(rr == 3 - r, "ordering consistent under swap", rr);

    // Self-comparisons of the shared tables are all "same".
    r = kcmp(self, self, KCMP_FILES, 0, 0);
    check(r == 0, "KCMP_FILES self == self", r);
    r = kcmp(self, self, KCMP_VM, 0, 0);
    check(r == 0, "KCMP_VM self == self", r);
    r = kcmp(self, self, KCMP_FS, 0, 0);
    check(r == 0, "KCMP_FS self == self", r);
    r = kcmp(self, self, KCMP_SIGHAND, 0, 0);
    check(r == 0, "KCMP_SIGHAND self == self", r);

    // A plain fork()ed child shares none of those tables with the parent,
    // but its fd 'fd' IS the same description (fd tables are copied, the
    // descriptions are shared).
    pid_t child = fork();
    if (child == 0) {
        pause();
        _exit(0);
    }
    r = kcmp(self, child, KCMP_FILES, 0, 0);
    check(r == 1 || r == 2, "fork child has its own fd table", r);
    r = kcmp(self, child, KCMP_FILE, (unsigned long) fd, (unsigned long) fd);
    check(r == 0, "same description via inherited fd", r);
    kill(child, SIGKILL);
    waitpid(child, NULL, 0);

    // Errors: bad fd, dead pid, bad type.
    errno = 0;
    r = kcmp(self, self, KCMP_FILE, 9999, 9999);
    check(r < 0 && errno == EBADF, "bad fd -> EBADF", r);
    errno = 0;
    r = kcmp(self, child, KCMP_FILES, 0, 0);  // child is reaped by now
    check(r < 0 && errno == ESRCH, "reaped pid -> ESRCH", r);
    errno = 0;
    r = kcmp(self, self, 99, 0, 0);
    check(r < 0 && errno == EINVAL, "bad type -> EINVAL", r);

    close(fd);
    close(fd2);
    close(fd3);
    return finish_suite("kcmp");
}
