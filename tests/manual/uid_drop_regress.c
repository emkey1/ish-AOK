// Dropping uid and gid as root sticks: setuid, seteuid, setgid and setegid
// cannot get the old ids back, with or without PR_SET_KEEPCAPS.
#define _GNU_SOURCE
#include <errno.h>
#include <sys/syscall.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define _LINUX_CAPABILITY_VERSION_3_ 0x20080522
#define PR_SET_KEEPCAPS_ 8

struct cap_user_header_ {
    unsigned int version;
    int pid;
};

struct cap_user_data_ {
    unsigned int effective;
    unsigned int permitted;
    unsigned int inheritable;
};

static int capget_local(struct cap_user_header_ *header, struct cap_user_data_ *data) {
    return syscall(SYS_capget, header, data);
}

static int capset_local(const struct cap_user_header_ *header, const struct cap_user_data_ *data) {
    return syscall(SYS_capset, header, data);
}

static int prctl_local(int option, unsigned long arg2) {
    return syscall(SYS_prctl, option, arg2, 0UL, 0UL, 0UL);
}

static void fail_errno(const char *what) {
    fprintf(stderr, "%s: %s\n", what, strerror(errno));
    exit(1);
}

static void fail_msg(const char *what) {
    fprintf(stderr, "%s\n", what);
    exit(1);
}

static void test_keepcaps(uid_t target_uid, gid_t target_gid) {
    struct cap_user_header_ header = {
        .version = _LINUX_CAPABILITY_VERSION_3_,
        .pid = 0,
    };
    struct cap_user_data_ caps[2];

    if (capget_local(&header, caps) < 0)
        fail_errno("capget");
    if (prctl_local(PR_SET_KEEPCAPS_, 1) < 0)
        fail_errno("prctl(PR_SET_KEEPCAPS)");
    if (setresgid(target_gid, target_gid, target_gid) < 0)
        fail_errno("setresgid keepcaps");
    if (setresuid(target_uid, target_uid, target_uid) < 0)
        fail_errno("setresuid keepcaps");
    if (capset_local(&header, caps) < 0)
        fail_errno("capset keepcaps");
    if (geteuid() != target_uid || getegid() != target_gid)
        fail_msg("keepcaps uid/gid transition did not stick");
}

int main(void) {
    uid_t old_uid = getuid();
    gid_t old_gid = getgid();
    uid_t target_uid = 65534;
    gid_t target_gid = 65534;

    if (old_uid != 0)
        fail_msg("must run as root");

    pid_t keepcaps_child = fork();
    if (keepcaps_child < 0)
        fail_errno("fork keepcaps");
    if (keepcaps_child == 0) {
        test_keepcaps(target_uid, target_gid);
        _exit(0);
    }
    int keepcaps_status;
    if (waitpid(keepcaps_child, &keepcaps_status, 0) < 0)
        fail_errno("waitpid keepcaps");
    if (!WIFEXITED(keepcaps_status) || WEXITSTATUS(keepcaps_status) != 0)
        fail_msg("keepcaps regression failed");

    if (setresgid(target_gid, target_gid, target_gid) < 0)
        fail_errno("setresgid");
    if (setresuid(target_uid, target_uid, target_uid) < 0)
        fail_errno("setresuid");

    errno = 0;
    if (setgid(old_gid) != -1)
        fail_msg("setgid unexpectedly restored old gid");
    errno = 0;
    if (setegid(old_gid) != -1)
        fail_msg("setegid unexpectedly restored old gid");
    errno = 0;
    if (setuid(old_uid) != -1)
        fail_msg("setuid unexpectedly restored old uid");
    errno = 0;
    if (seteuid(old_uid) != -1)
        fail_msg("seteuid unexpectedly restored old uid");

    if (getuid() != target_uid || geteuid() != target_uid)
        fail_msg("uid drop did not stick");
    if (getgid() != target_gid || getegid() != target_gid)
        fail_msg("gid drop did not stick");

    puts("uid_drop_regress ok");
    return 0;
}
