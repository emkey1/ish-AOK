// execve of a binary that lives on tmpfs, plus positional I/O on tmpfs.
//
// tmpfs_fdops had no .pread/.pwrite. Most kernel paths guard with
// `if (fd->ops->pread)`, but kernel/exec.c's loader called fd->ops->pread
// directly for the residual file bytes of an EOF-straddling PT_LOAD -- a
// jump through NULL, host EXC_BAD_ACCESS, and the WHOLE app aborted. First
// hit for real once tmp.mount worked and the on-device regression suite
// built its test binaries under /tmp: executing the first one took down the
// emulator (device crash 2026-07-22, jit_crash_fn <- load_entry).
//
// This re-execs a copy of itself from a fresh tmpfs (the child just prints
// and exits, gated by an env marker), then exercises pread/pwrite guest
// syscalls on a tmpfs file: positional ops must work and must not move the
// fd offset.
//
// Also passes on real Linux (mint oracle, verified).
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

#include "test_common.h"

int main(int argc, char **argv) {
    if (getenv("TMPFS_EXEC_CHILD") != NULL) {
        printf("child-ran\n");
        return 42;
    }
    test_init(argc, argv);
    alarm(test_watchdog_secs(20));

    char dir[] = "/tmp/tmpfs_exec.XXXXXX";
    if (mkdtemp(dir) == NULL) {
        perror("mkdtemp");
        return 1;
    }
    // A mount needs privilege. The CLI harness runs as root, an ssh session
    // into the app does not, and an unprivileged run reported this as a flat
    // failure that read like a mount regression. Skip only for the privilege
    // errnos and only when actually unprivileged; anything else still fails.
    if (mount("tmpfs", dir, "tmpfs", 0, "") != 0) {
        if ((errno == EPERM || errno == EACCES) && geteuid() != 0) {
            printf("tmpfs_exec: SKIP (needs root to mount: %s)\n", strerror(errno));
            rmdir(dir);
            return 0;
        }
        printf("FAIL: mount(tmpfs) -> %s\n", strerror(errno));
        rmdir(dir);
        return 1;
    }

    // Copy our own binary onto the tmpfs.
    char target[sizeof(dir) + 16];
    snprintf(target, sizeof(target), "%s/self", dir);
    int src = open("/proc/self/exe", O_RDONLY);
    int dst = open(target, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (src < 0 || dst < 0) {
        printf("FAIL: open self/target (%s)\n", strerror(errno));
        return 1;
    }
    char buf[65536];
    ssize_t n;
    while ((n = read(src, buf, sizeof(buf))) > 0)
        if (write(dst, buf, (size_t) n) != n) {
            printf("FAIL: short write copying self\n");
            return 1;
        }
    close(src);
    close(dst);

    // Exec it. Before tmpfs grew .pread this killed the entire emulator.
    pid_t child = fork();
    if (child == 0) {
        char *env[] = {(char *) "TMPFS_EXEC_CHILD=1", NULL};
        char *args[] = {target, NULL};
        execve(target, args, env);
        _exit(99);
    }
    int status = 0;
    waitpid(child, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 42) {
        printf("FAIL: exec from tmpfs (status %#x, want exit 42)\n", status);
        failures_total++;
    } else if (test_verbose) {
        printf("ok exec from tmpfs\n");
    }

    // pread/pwrite on tmpfs: positional, and the fd offset must not move.
    char fpath[sizeof(dir) + 16];
    snprintf(fpath, sizeof(fpath), "%s/f", dir);
    int fd = open(fpath, O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        printf("FAIL: open tmpfs file (%s)\n", strerror(errno));
        return 1;
    }
    if (pwrite(fd, "hello world", 11, 5) != 11) {
        printf("FAIL: pwrite(tmpfs) -> %s\n", strerror(errno));
        failures_total++;
    }
    char rbuf[16] = {0};
    ssize_t got = pread(fd, rbuf, 5, 11);
    if (got != 5 || memcmp(rbuf, "world", 5) != 0) {
        printf("FAIL: pread(tmpfs) -> %zd \"%.5s\" (want 5 \"world\")\n", got, rbuf);
        failures_total++;
    }
    if (lseek(fd, 0, SEEK_CUR) != 0) {
        printf("FAIL: pread/pwrite moved the fd offset (now %lld)\n",
               (long long) lseek(fd, 0, SEEK_CUR));
        failures_total++;
    }
    // The first 5 bytes were never written: implicit zeros (file starts at
    // the pwrite offset, holes read back as 0).
    got = pread(fd, rbuf, 5, 0);
    char zeros[5] = {0};
    if (got != 5 || memcmp(rbuf, zeros, 5) != 0) {
        printf("FAIL: hole before pwrite offset not zero-filled\n");
        failures_total++;
    }
    // Past-EOF pread returns 0, not garbage.
    got = pread(fd, rbuf, 5, 4096);
    if (got != 0) {
        printf("FAIL: pread past EOF -> %zd (want 0)\n", got);
        failures_total++;
    }
    close(fd);

    umount2(dir, MNT_DETACH);
    rmdir(dir);
    return finish_suite("tmpfs_exec");
}
