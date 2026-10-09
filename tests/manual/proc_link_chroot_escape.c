// proc_link_chroot_escape.c -- a /proc/self/fd/N link to a file outside the
// caller's chroot leads to that file. Linux follows these links by jumping to
// the file the descriptor holds (nd_jump_link), so a chrooted process reaches
// what it opened before the chroot: the file itself, and through a directory
// descriptor the files below it -- `/bin/sh /dev/fd/3/script` in a chroot
// depends on it. readlink shows the global path, unmarked. AOK walked the
// link's text instead, which outside the root is "(unreachable)...": ENOENT.
// The jail holds a decoy at the same global path, so reaching the decoy is
// told apart from reaching the file. Root (or `unshare -rm`): it bind-mounts
// /proc into the jail and chroots. Checked on camd (6.12, `unshare -rm`).
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void put(const char *path, const char *text) {
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0 || write(fd, text, strlen(text)) != (ssize_t) strlen(text)) {
        printf("FAIL cannot write %s: %s\n", path, strerror(errno));
        exit(1);
    }
    close(fd);
}

static void expect_text(const char *path, const char *want, const char *what) {
    char buf[64] = "";
    int fd = open(path, O_RDONLY);
    int e = errno;
    ssize_t n = fd >= 0 ? read(fd, buf, sizeof buf - 1) : -1;
    if (n > 0)
        buf[n] = '\0';
    if (fd >= 0)
        close(fd);
    CHECK(fd >= 0 && strcmp(buf, want) == 0, "%s: open(%s) %s, read \"%s\" (want \"%s\")", what, path,
          fd >= 0 ? "ok" : strerror(e), buf, want);
}

static int in_jail(const char *out, const char *jail);

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    char base[] = "/tmp/plce.XXXXXX";
    if (mkdtemp(base) == NULL) {
        printf("FAIL mkdtemp: %s\n", strerror(errno));
        return 1;
    }
    char out[256], jail[256], p[1024];
    snprintf(out, sizeof out, "%s/out", base);
    snprintf(jail, sizeof jail, "%s/jail", base);
    mkdir(out, 0755);
    mkdir(jail, 0755);
    snprintf(p, sizeof p, "%s/file.txt", out); put(p, "outside file");
    snprintf(p, sizeof p, "%s/inner.txt", out); put(p, "outside inner");
    // the decoy: the same global path, inside the jail
    snprintf(p, sizeof p, "%s%s", jail, out);
    char cmd[1100];
    snprintf(cmd, sizeof cmd, "mkdir -p %s", p);
    if (system(cmd) != 0) { printf("FAIL mkdir decoy\n"); return 1; }
    snprintf(p, sizeof p, "%s%s/file.txt", jail, out); put(p, "decoy file");
    snprintf(p, sizeof p, "%s%s/inner.txt", jail, out); put(p, "decoy inner");
    snprintf(p, sizeof p, "%s/proc", jail);
    mkdir(p, 0755);
    if (mount("/proc", p, NULL, MS_BIND | MS_REC, NULL) != 0) {
        printf("SKIP bind-mounting /proc needs root: %s\n", strerror(errno));
        return 0;
    }

    char procdir[1024];
    snprintf(procdir, sizeof procdir, "%s/proc", jail);
    pid_t child = fork();
    if (child == 0)
        _exit(in_jail(out, jail));
    int status = 0;
    waitpid(child, &status, 0);
    // clean up: the bind mount outlives the child, and the files
    umount2(procdir, MNT_DETACH);
    const char *files[] = {"out/file.txt", "out/inner.txt", NULL};
    for (int i = 0; files[i]; i++) {
        snprintf(p, sizeof p, "%s/%s", base, files[i]); unlink(p);
        snprintf(p, sizeof p, "%s%s/%s", jail, base, files[i]); unlink(p);
    }
    snprintf(p, sizeof p, "%s%s/out", jail, base); rmdir(p);
    snprintf(p, sizeof p, "%s%s", jail, base); rmdir(p);
    snprintf(p, sizeof p, "%s/tmp", jail); rmdir(p);
    rmdir(procdir); rmdir(jail); rmdir(out); rmdir(base);
    if (!WIFEXITED(status)) {
        printf("FAIL the jailed child: status %#x\n", status);
        return 1;
    }
    return WEXITSTATUS(status);
}

static int in_jail(const char *out, const char *jail) {
    char p[512];
    snprintf(p, sizeof p, "%s/file.txt", out);
    int ffd = open(p, O_RDONLY);
    int dfd = open(out, O_RDONLY | O_DIRECTORY);
    if (ffd < 0 || dfd < 0 || chroot(jail) != 0 || chdir("/") != 0) {
        printf("FAIL setup: %s\n", strerror(errno));
        return 1;
    }

    char link[64], text[512];
    snprintf(link, sizeof link, "/proc/self/fd/%d", ffd);
    expect_text(link, "outside file", "the file through its fd link");
    ssize_t n = readlink(link, text, sizeof text - 1);
    if (n >= 0)
        text[n] = '\0';
    snprintf(p, sizeof p, "%s/file.txt", out);
    CHECK(n >= 0 && strcmp(text, p) == 0, "readlink(%s) = \"%s\" (want the global path %s)", link,
          n >= 0 ? text : strerror(errno), p);
    snprintf(link, sizeof link, "/proc/self/fd/%d/inner.txt", dfd);
    expect_text(link, "outside inner", "a file below the directory's fd link");
    snprintf(link, sizeof link, "/proc/self/fd/%d/../out/inner.txt", dfd);
    expect_text(link, "outside inner", "`..` after the jump");
    // inside the jail, the decoy is what the global path names
    snprintf(p, sizeof p, "%s/file.txt", out);
    expect_text(p, "decoy file", "the global path inside the jail");

    printf("proc_link_chroot_escape: %s (%d checks, %d failures)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures != 0;
}
