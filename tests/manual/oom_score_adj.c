// /proc/<pid>/oom_score_adj was entirely unimplemented (missing from
// proc_pid_children) -- every systemd-executor-spawned service opened
// /proc/self/oom_score_adj (ExecStart's mandatory OOMScoreAdjust write, done
// unconditionally for every unit regardless of whether the unit sets
// OOMScoreAdjust=) and got ENOENT, which systemd-executor treats as fatal:
// it calls exit(EXIT_OOM_ADJUST) (206) before ever exec'ing the target
// binary. Real trigger: EVERY service on Arch aarch64 boot (dbus-broker,
// systemd-userwork, udevd, ...) died with exit code 206 the instant
// clone3(CLONE_PIDFD) started actually working, blocking all of them behind
// "Failed to spawn executor"-class failures even after the clone3/
// PR_CAPBSET_DROP fixes landed.
//
// oom_score_adj is stored per-task (kernel/task.h's oom_score_adj field,
// inherited across fork via task_create_'s struct copy, matching Linux) --
// no real OOM killer is modeled, this file exists purely so the
// write/read-back contract systemd-executor depends on holds.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include "test_common.h"

static void check(int cond, const char *what) {
    if (cond) {
        test_logf("ok: %s\n", what);
    } else {
        printf("FAIL: %s (errno=%d %s)\n", what, errno, strerror(errno));
        failures_total++;
    }
}

static int read_oom_score_adj(const char *path, int *out) {
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    char buf[32] = {0};
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return -1;
    *out = atoi(buf);
    return 0;
}

static int write_oom_score_adj(const char *path, const char *value) {
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return -1;
    ssize_t n = write(fd, value, strlen(value));
    int saved_errno = errno;
    close(fd);
    if (n < 0) {
        errno = saved_errno;
        return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    const char *path = "/proc/self/oom_score_adj";

    // Default value (matches Linux: 0 unless a parent adjusted it). The file
    // existing and reading back at all is the regression this suite locks (it
    // was missing entirely -> ENOENT -> systemd-executor exit(206)), and that
    // half needs no privilege, so it runs before the guard below.
    int value = -12345;
    check(read_oom_score_adj(path, &value) == 0, "read /proc/self/oom_score_adj succeeds");
    check(value == 0, "default oom_score_adj is 0");
    if (failures_total != 0)
        return finish_suite("oom_score_adj");

    // Everything past here writes the file, which needs privilege: on Linux
    // *lowering* oom_score_adj needs CAP_SYS_RESOURCE (EACCES without it, and
    // an unprivileged task can never walk a raised value back down), and under
    // AOK the procfs entry is mode 0444 so every write needs root. Skip rather
    // than fail so an unprivileged run stays green -- the on-device suite runs
    // as uid 1000. Same convention as ambient_caps.c / chroot_getcwd.c.
    if (geteuid() != 0) {
        printf("oom_score_adj: SKIP (not privileged: euid=%d)\n", (int) geteuid());
        return 0;
    }

    // systemd-executor's actual write pattern: a small decimal string.
    check(write_oom_score_adj(path, "-900") == 0, "write oom_score_adj=-900 succeeds");
    value = -12345;
    check(read_oom_score_adj(path, &value) == 0 && value == -900, "read-back sees -900");

    check(write_oom_score_adj(path, "1000") == 0, "write oom_score_adj=1000 (max) succeeds");
    value = -12345;
    check(read_oom_score_adj(path, &value) == 0 && value == 1000, "read-back sees 1000 (max)");

    check(write_oom_score_adj(path, "-1000") == 0, "write oom_score_adj=-1000 (min) succeeds");
    value = -12345;
    check(read_oom_score_adj(path, &value) == 0 && value == -1000, "read-back sees -1000 (min)");

    // Out-of-range and malformed values are rejected, matching Linux.
    errno = 0;
    check(write_oom_score_adj(path, "1001") < 0 && errno == EINVAL,
          "write oom_score_adj=1001 (out of range) is EINVAL");
    errno = 0;
    check(write_oom_score_adj(path, "-1001") < 0 && errno == EINVAL,
          "write oom_score_adj=-1001 (out of range) is EINVAL");
    errno = 0;
    check(write_oom_score_adj(path, "not-a-number") < 0 && errno == EINVAL,
          "write oom_score_adj=garbage is EINVAL");

    // Reset to 0, then verify inheritance across fork (Linux semantics).
    check(write_oom_score_adj(path, "-500") == 0, "reset oom_score_adj=-500 before fork");
    pid_t child = fork();
    if (child == 0) {
        int child_value = -12345;
        int ok = read_oom_score_adj(path, &child_value) == 0 && child_value == -500;
        _exit(ok ? 0 : 1);
    }
    int status = 0;
    waitpid(child, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "child inherits parent's oom_score_adj across fork");

    return finish_suite("oom_score_adj");
}
