// fcntl(F_GETFL) reports the status flags Linux reports.
//
// realfs answered F_GETFL by asking the host descriptor, which is wrong twice:
//   - the host's F_GETFL keeps neither O_DIRECTORY, O_NOFOLLOW nor O_LARGEFILE,
//     all of which Linux reports (and a 64-bit task gets O_LARGEFILE on every
//     open, force_o_largefile);
//   - realfs forces a pipe's HOST descriptor non-blocking the first time the
//     guest blocks on it, so F_GETFL said O_NONBLOCK about a pipe the guest
//     never made non-blocking, and the get-modify-restore idiom then left the
//     pipe non-blocking for real: every later read was EAGAIN.
// Opens are raw openat calls, so libc cannot add O_LARGEFILE on its own (musl
// always does); the bit's value is per architecture (arm64 relocates it).
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include "test_common.h"

#if defined(__aarch64__)
#define KERNEL_O_LARGEFILE 0x20000
#else
#define KERNEL_O_LARGEFILE 0x8000
#endif

static int fails;
#define CHECK(cond, ...) do { if (cond) { test_logf("ok: " __VA_ARGS__); test_logf("\n"); } \
    else { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static int raw_open(const char *path, int flags) {
    return (int) syscall(SYS_openat, AT_FDCWD, path, flags, 0644);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    int is64 = sizeof(long) == 8;
    char dir[] = "/tmp/getflXXXXXX", file[64], proc[64];
    if (mkdtemp(dir) == NULL) { perror("mkdtemp"); return 2; }
    snprintf(file, sizeof file, "%s/f", dir);

    int f = raw_open(file, O_RDWR | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC);
    int fl = fcntl(f, F_GETFL);
    CHECK(!!(fl & KERNEL_O_LARGEFILE) == is64, "a %d-bit open %s O_LARGEFILE (%#x)", is64 ? 64 : 32,
          is64 ? "gets" : "without it does not get", fl);
    CHECK((fl & O_ACCMODE) == O_RDWR && (fl & O_APPEND), "access mode and O_APPEND reported (%#x)", fl);
    CHECK(!(fl & (O_CREAT | O_TRUNC | O_CLOEXEC)), "no O_CREAT, O_TRUNC or O_CLOEXEC (%#x)", fl);
    close(f);
    if (!is64) {
        f = raw_open(file, O_RDONLY | KERNEL_O_LARGEFILE);
        CHECK(fcntl(f, F_GETFL) & KERNEL_O_LARGEFILE, "a 32-bit open that asks keeps O_LARGEFILE");
        close(f);
    }

    int d = raw_open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    fl = fcntl(d, F_GETFL);
    CHECK((fl & O_DIRECTORY) && (fl & O_NOFOLLOW), "O_DIRECTORY and O_NOFOLLOW reported (%#x)", fl);
    snprintf(proc, sizeof proc, "/proc/self/fd/%d", d);
    int r = raw_open(proc, O_RDONLY);
    fl = r >= 0 ? fcntl(r, F_GETFL) : -1;
    CHECK(r >= 0 && !(fl & (O_DIRECTORY | O_NOFOLLOW)) && !!(fl & KERNEL_O_LARGEFILE) == is64,
          "a directory reopened through /proc reports only what that open asked (%#x)", fl);
    close(r);
    close(d);

    // The pipe idiom: block once, then get-set-restore, then the pipe must block.
    int p[2];
    if (pipe(p) != 0) { perror("pipe"); return 2; }
    char c = 'x';
    if (write(p[1], &c, 1) != 1 || read(p[0], &c, 1) != 1) { perror("pipe io"); return 2; }
    fl = fcntl(p[0], F_GETFL);
    CHECK(!(fl & O_NONBLOCK), "a blocking pipe still reports blocking after a read (%#x)", fl);
    fcntl(p[0], F_SETFL, fl | O_NONBLOCK);
    CHECK(read(p[0], &c, 1) == -1 && errno == EAGAIN, "set non-blocking: an empty read is EAGAIN");
    fcntl(p[0], F_SETFL, fl);
    pid_t pid = fork();
    if (pid == 0) {
        usleep(200 * 1000);
        if (write(p[1], "y", 1) != 1) _exit(1);
        _exit(0);
    }
    ssize_t n = read(p[0], &c, 1);
    CHECK(n == 1 && c == 'y', "after the restore the read blocks until data comes (n=%zd errno=%d)",
          n, n < 0 ? errno : 0);
    waitpid(pid, NULL, 0);

    unlink(file);
    rmdir(dir);
    printf("fcntl_getfl_flags: %s\n", fails ? "FAIL" : "PASS");
    return fails != 0;
}
