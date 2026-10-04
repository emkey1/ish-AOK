// What clone() and unshare() answer, as root, for each namespace flag.
//
// On Linux as root both succeed. For a namespace type AOK does not have
// (mount, PID, network, user, cgroup) each entry point must fail with the
// errno its consumers read as "no sandbox here, carry on", and those differ:
//
//   clone:   EINVAL (Linux's answer for a namespace type it was built
//            without). systemd forks its generator sandbox with
//            raw_clone(SIGCHLD|CLONE_NEWNS) and falls back on
//            EPERM/EACCES/EINVAL only; 557 answered ENOSYS and PID 1 froze at
//            "Failed to start up manager" on Arch Linux ARM.
//   unshare: ENOSYS. systemd's unshare(CLONE_NEWNS) (namespace setup for a
//            service) falls back on EPERM/EACCES/EOPNOTSUPP/ENOSYS, and
//            treats EINVAL as a failure.
//
// EPERM would satisfy both, but says "you may not" to a root caller who may,
// so it is refused too. Each probe runs in its own child, so a namespace that
// does get created (on Linux) never touches the test itself.
//
// Root only: as anyone else both say EPERM, which proves nothing.
#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include "test_common.h"

// 0 for success, else the errno.
static int try_clone(int flag) {
    long pid = syscall(SYS_clone, flag | SIGCHLD, 0, 0, 0, 0);
    if (pid == 0)
        _exit(0);
    if (pid < 0)
        return errno;
    waitpid((pid_t) pid, NULL, 0);
    return 0;
}

static int try_unshare(int flag) {
    pid_t pid = fork();
    if (pid == 0)
        _exit(unshare(flag) == 0 ? 0 : (errno & 0xff));
    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static const char *describe(int e) {
    return e ? strerror(e) : "succeeds";
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    if (geteuid() != 0) {
        printf("namespace_errno: SKIP (needs root)\n");
        return 0;
    }
    static const struct { int flag; const char *name; } ns[] = {
        {CLONE_NEWNS, "CLONE_NEWNS"}, {CLONE_NEWPID, "CLONE_NEWPID"},
        {CLONE_NEWNET, "CLONE_NEWNET"}, {CLONE_NEWUSER, "CLONE_NEWUSER"},
        {CLONE_NEWCGROUP, "CLONE_NEWCGROUP"}, {CLONE_NEWUTS, "CLONE_NEWUTS"},
        {CLONE_NEWIPC, "CLONE_NEWIPC"},
    };
    int fails = 0;
    for (unsigned i = 0; i < sizeof ns / sizeof ns[0]; i++) {
        int c = try_clone(ns[i].flag), u = try_unshare(ns[i].flag);
        if (c != 0 && c != EINVAL) {
            printf("FAIL: %s: clone %s, want success or EINVAL\n", ns[i].name, describe(c));
            fails++;
        }
        if (u != 0 && u != ENOSYS) {
            printf("FAIL: %s: unshare %s, want success or ENOSYS\n", ns[i].name, describe(u));
            fails++;
        }
        // A namespace one entry point can make, the other must make too.
        if ((c == 0) != (u == 0)) {
            printf("FAIL: %s: clone %s but unshare %s\n", ns[i].name, describe(c), describe(u));
            fails++;
        }
        test_logf("%s: clone %s, unshare %s\n", ns[i].name, describe(c), describe(u));
    }
    printf("namespace_errno: %s\n", fails ? "FAIL" : "PASS");
    return fails != 0;
}
