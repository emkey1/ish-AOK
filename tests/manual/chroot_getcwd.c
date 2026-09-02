// chroot() rebases getcwd() to the process root (Linux semantics).
//
// History: sys_getcwd returned generic_getpath(pwd) -- the full mount-absolute
// path -- without stripping the process's chroot root, so after chroot("/jail")
// + chdir("/") getcwd() returned "/jail" instead of "/". AOK already resolves
// paths relative to the chroot root (fs/path.c passes it to __path_normalize),
// so only the getcwd query was wrong. This matters because chroot into an
// i386/amd64/aarch64 rootfs is how AOK runs another guest arch, and tools that
// consult getcwd after chrooting (build systems, package managers) misbehaved.
//
// Requires privilege to chroot(): run as root (AOK runs guests as uid 0) or,
// on a real Linux oracle, under `unshare -r` (a user namespace grants
// CAP_SYS_CHROOT). Each case runs in its own child so the (irreversible)
// chroot doesn't bleed across cases.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "test_common.h"

#define JAIL "/cgj-test"

static int fails;

static void expect_cwd(const char *what, const char *want) {
    char buf[256];
    if (getcwd(buf, sizeof(buf)) == NULL) {
        printf("FAIL: %s: getcwd errno=%d (%s), wanted \"%s\"\n",
               what, errno, strerror(errno), want);
        fails++;
        return;
    }
    if (strcmp(buf, want) == 0) {
        test_logf("ok: %s: getcwd=\"%s\"\n", what, buf);
    } else {
        printf("FAIL: %s: getcwd=\"%s\", wanted \"%s\"\n", what, buf, want);
        fails++;
    }
}

static void expect_cwd_enoent(const char *what) {
    char buf[256];
    if (getcwd(buf, sizeof(buf)) != NULL) {
        printf("FAIL: %s: getcwd=\"%s\", wanted ENOENT (unreachable)\n", what, buf);
        fails++;
    } else if (errno != ENOENT) {
        printf("FAIL: %s: getcwd errno=%d (%s), wanted ENOENT\n",
               what, errno, strerror(errno));
        fails++;
    } else {
        test_logf("ok: %s: getcwd -> ENOENT (unreachable)\n", what);
    }
}

// Run one case body in a child; returns its failure count.
static int run_case(void (*body)(void)) {
    pid_t pid = fork();
    if (pid == 0) {
        fails = 0;
        body();
        exit(fails > 255 ? 255 : fails);
    }
    int st;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

static void case_root_and_sub(void) {
    if (chroot(JAIL) < 0) { printf("FAIL: chroot(%s): %s\n", JAIL, strerror(errno)); fails++; return; }
    if (chdir("/") < 0) { printf("FAIL: chdir(/): %s\n", strerror(errno)); fails++; return; }
    expect_cwd("chroot + chdir(/)", "/");
    if (chdir("/sub") < 0) { printf("FAIL: chdir(/sub): %s\n", strerror(errno)); fails++; return; }
    expect_cwd("chroot + chdir(/sub)", "/sub");
    if (chdir("/sub/deep") < 0) { printf("FAIL: chdir(/sub/deep): %s\n", strerror(errno)); fails++; return; }
    expect_cwd("chroot + chdir(/sub/deep)", "/sub/deep");
}

static void case_unreachable(void) {
    // cwd stays at the real root (outside the jail); chroot without a following
    // chdir leaves cwd unreachable from the new root -> getcwd ENOENT.
    if (chdir("/") < 0) { printf("FAIL: chdir(/): %s\n", strerror(errno)); fails++; return; }
    if (chroot(JAIL) < 0) { printf("FAIL: chroot(%s): %s\n", JAIL, strerror(errno)); fails++; return; }
    expect_cwd_enoent("chroot without chdir (cwd outside root)");
}

int main(int argc, char **argv) {
    test_init(argc, argv);

    // Build the jail tree from outside any chroot.
    mkdir(JAIL, 0755);
    mkdir(JAIL "/sub", 0755);
    mkdir(JAIL "/sub/deep", 0755);

    if (geteuid() != 0) {
        // Can't chroot without privilege; skip rather than fail so the suite
        // stays green on an unprivileged host (the AOK suite runs as uid 0).
        printf("chroot_getcwd: SKIP (not privileged: euid=%d)\n", (int) geteuid());
        return 0;
    }

    int f = 0;
    f += run_case(case_root_and_sub);
    f += run_case(case_unreachable);

    if (f != 0) {
        printf("chroot_getcwd: FAIL failures=%d\n", f);
        return 1;
    }
    printf("chroot_getcwd: PASS\n");
    return 0;
}
