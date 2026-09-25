// `..` at a process's root is the root: no walk leaves a chroot upward.
//
// Linux's follow_dotdot() stops at nd->root, the caller's root, however the
// walk got there -- from the cwd, from a dirfd, from an absolute path, or in
// the middle of a symlink's target. AOK resolved `..` lexically in
// __path_normalize (fs/path.c) and never compared against the root it was
// handed, so after `chroot("/jail"); chdir("/")` a plain `chdir("..")` put the
// process in the REAL parent directory, and `/..`, a symlink to `../..`, or
// openat(root_fd, "..") all read files outside the jail.
//
// A cwd left OUTSIDE the root by a chroot without a chdir is another matter:
// Linux walks `..` from there freely, since the walk never stands on the root,
// and so must AOK. Only the moment the walk reaches the root is clamped.
//
// Requires privilege to chroot(): run as root, or on a Linux oracle under
// `unshare -r`. Each case runs in its own child: a chroot is not undone.
//
// Oracle: Linux 6.12 (camd), glibc x86_64 and -m32, as root.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "test_common.h"

#define TEST_NAME "chroot_dotdot"

static char base[] = "/tmp/crdd.XXXXXX";
static char jail[64];

static void check(int cond, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (cond) {
        if (test_verbose) {
            printf("ok: ");
            vprintf(fmt, ap);
            printf("\n");
        }
    } else {
        printf("FAIL: ");
        vprintf(fmt, ap);
        printf("\n");
        failures_total++;
    }
    va_end(ap);
}

// In the jail, jail-marker is at the root; base-marker is one level up,
// outside it, and must never be reachable through `..`.
static void check_in_root(const char *how) {
    char got[PATH_MAX];
    if (getcwd(got, sizeof(got)) == NULL)
        snprintf(got, sizeof(got), "<getcwd: %s>", strerror(errno));
    check(strcmp(got, "/") == 0, "%s: getcwd is / (got %s)", how, got);
    check(access("jail-marker", F_OK) == 0, "%s: the cwd is the jail's root (%s)", how,
          strerror(errno));
}

static void check_same(const char *a, const char *b) {
    struct stat sa, sb;
    int ra = stat(a, &sa), rb = stat(b, &sb);
    check(ra == 0 && rb == 0 && sa.st_ino == sb.st_ino && sa.st_dev == sb.st_dev,
          "%s is %s (%s)", a, b, ra != 0 || rb != 0 ? strerror(errno) : "different inodes");
}

static int enter(void) {
    if (chroot(jail) != 0) {
        check(0, "chroot %s (%s)", jail, strerror(errno));
        return -1;
    }
    return 0;
}

static void case_cwd(void) {
    if (enter() < 0 || chdir("/") != 0)
        return;
    check(chdir("..") == 0, "chdir .. (%s)", strerror(errno));
    check_in_root("chdir .. at the root");
    check(chdir("../../../..") == 0, "chdir ../../../.. (%s)", strerror(errno));
    check_in_root("chdir ../../../.. at the root");
    check(chdir("sub/../..") == 0, "chdir sub/../.. (%s)", strerror(errno));
    check_in_root("chdir sub/../.. at the root");
    check(access("../base-marker", F_OK) != 0, "../base-marker, outside the jail, is not there");
    check(access("../jail-marker", F_OK) == 0, "../jail-marker is the root's own (%s)",
          strerror(errno));
}

static void case_absolute(void) {
    if (enter() < 0 || chdir("/sub") != 0)
        return;
    check_same("/..", "/");
    check_same("/../../..", "/");
    check_same("/sub/../../..", "/");
    check(access("/../base-marker", F_OK) != 0, "/../base-marker is not there");
    check(access("/../jail-marker", F_OK) == 0, "/../jail-marker is (%s)", strerror(errno));
    check(access("../../jail-marker", F_OK) == 0, "../../jail-marker from /sub is (%s)",
          strerror(errno));
}

static void case_symlinks(void) {
    if (enter() < 0 || chdir("/") != 0)
        return;
    // up -> ../..  and  abs -> /..  , made before the chroot.
    check(access("up/jail-marker", F_OK) == 0, "a relative symlink to ../.. stays in (%s)",
          strerror(errno));
    check(access("abs/jail-marker", F_OK) == 0, "an absolute symlink to /.. stays in (%s)",
          strerror(errno));
    check(access("sub/deep/jail-marker", F_OK) == 0,
          "a symlink below the root to ../../.. stays in (%s)", strerror(errno));
    check(access("up/base-marker", F_OK) != 0, "up/base-marker is not there");
}

static void case_dirfd(void) {
    if (enter() < 0 || chdir("/") != 0)
        return;
    int rfd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    check(rfd >= 0, "open / (%s)", strerror(errno));
    if (rfd < 0)
        return;
    int up = openat(rfd, "..", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    check(up >= 0, "openat(root, \"..\") (%s)", strerror(errno));
    struct stat sr, su;
    if (up >= 0) {
        check(fstat(rfd, &sr) == 0 && fstat(up, &su) == 0 && sr.st_ino == su.st_ino &&
                  sr.st_dev == su.st_dev,
              "openat(root, \"..\") is the root");
        close(up);
    }
    check(faccessat(rfd, "../base-marker", F_OK, 0) != 0,
          "faccessat(root, \"../base-marker\") is not there");
    check(faccessat(rfd, "../../jail-marker", F_OK, 0) == 0,
          "faccessat(root, \"../../jail-marker\") is (%s)", strerror(errno));
    close(rfd);
}

// Linux lets a cwd left outside the root by chroot-without-chdir walk `..`
// freely; only a walk that reaches the root stops there.
static void case_cwd_outside(void) {
    if (chdir(base) != 0 || enter() < 0)
        return;
    check(access("base-marker", F_OK) == 0,
          "a cwd left outside the root still names what is there (%s)", strerror(errno));
    check(access("jail/jail-marker", F_OK) == 0, "and reaches the jail relatively (%s)",
          strerror(errno));
    check(chdir("jail") == 0, "chdir jail (%s)", strerror(errno));
    check_in_root("chdir into the root from outside it");
    check(chdir("..") == 0, "chdir .. (%s)", strerror(errno));
    check_in_root("chdir .. once the walk is on the root");
}

static void run_case(const char *name, void (*body)(void)) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        failures_total = 0;
        body();
        fflush(stdout);
        _exit(failures_total == 0 ? 0 : 1);
    }
    int status = 0;
    check(pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
              WEXITSTATUS(status) == 0,
          "case %s (status %#x)", name, status);
}

static void touch(const char *dir, const char *name) {
    char path[128];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    int fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    check(fd >= 0, "create %s (%s)", path, strerror(errno));
    if (fd >= 0)
        close(fd);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));

    if (mkdtemp(base) == NULL) {
        printf("FAIL: mkdtemp (%s)\n", strerror(errno));
        return 1;
    }
    char sub[80], path[128];
    snprintf(jail, sizeof(jail), "%s/jail", base);
    snprintf(sub, sizeof(sub), "%s/sub", jail);
    check(mkdir(jail, 0755) == 0 && mkdir(sub, 0755) == 0, "mkdir %s (%s)", sub,
          strerror(errno));
    touch(base, "base-marker");
    touch(jail, "jail-marker");
    snprintf(path, sizeof(path), "%s/up", jail);
    check(symlink("../..", path) == 0, "symlink %s (%s)", path, strerror(errno));
    snprintf(path, sizeof(path), "%s/abs", jail);
    check(symlink("/..", path) == 0, "symlink %s (%s)", path, strerror(errno));
    snprintf(path, sizeof(path), "%s/deep", sub);
    check(symlink("../../..", path) == 0, "symlink %s (%s)", path, strerror(errno));

    if (chroot("/") != 0) {
        int err = errno;
        check(err == EPERM, "chroot / (%s)", strerror(err));
        test_logf("chroot: %s, not privileged: checks skipped\n", strerror(err));
    } else {
        run_case("cwd", case_cwd);
        run_case("absolute", case_absolute);
        run_case("symlinks", case_symlinks);
        run_case("dirfd", case_dirfd);
        run_case("cwd outside", case_cwd_outside);
    }

    const char *files[] = {"jail/up", "jail/abs", "jail/sub/deep", "jail/jail-marker",
                           "base-marker"};
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", base, files[i]);
        unlink(path);
    }
    rmdir(sub);
    rmdir(jail);
    rmdir(base);
    return finish_suite(TEST_NAME);
}
