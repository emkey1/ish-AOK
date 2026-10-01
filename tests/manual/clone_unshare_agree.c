// clone() and unshare() answer the same for each namespace flag.
//
// For a namespace type AOK does not have (mount, PID, network, user, cgroup)
// unshare said ENOSYS while clone said EPERM -- "you may not" to a root caller
// who may. Whatever the answer, the two entry points must give the same one:
// both succeed (Linux as root), or both fail with one errno (AOK: ENOSYS).
// Each probe runs in its own child, so a namespace that does get created
// (on Linux) never touches the test itself.
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

int main(int argc, char **argv) {
    test_init(argc, argv);
    if (geteuid() != 0) {
        printf("clone_unshare_agree: SKIP (needs root)\n");
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
        if (c == u) {
            test_logf("ok: %s: clone and unshare both %s\n", ns[i].name, c ? strerror(c) : "succeed");
        } else {
            printf("FAIL: %s: clone %s, unshare %s\n", ns[i].name, c ? strerror(c) : "succeeds",
                   u ? strerror(u) : "succeeds");
            fails++;
        }
        if (c == EPERM)
            printf("FAIL: %s: clone says EPERM to root\n", ns[i].name), fails++;
    }
    printf("clone_unshare_agree: %s\n", fails ? "FAIL" : "PASS");
    return fails != 0;
}
