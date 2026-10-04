// cgroup2's cgroup.procs, cgroup.threads and cgroup.events describe the live
// process table, and cgroup.events raises IN_MODIFY when it changes.
//
// AOK backs cgroup2 with tmpfs (fs/tmp.c), and these were stored files:
// cgroup.events said "populated 1" forever and cgroup.procs kept every pid
// ever written to it. systemd learns that a unit's processes are gone from
// cgroup.events (inotify IN_MODIFY, then a read of "populated"), so on an Arch
// guest every failed oneshot -- modprobe@configfs/drm/fuse, systemd-sysctl --
// sat in stop-sigterm for TimeoutStopSec, 90 s each, and the boot waited with
// it; the SIGTERM sweep read cgroup.procs and signalled pids that had exited.
//
// A child moves itself (writing "0", the writer) into probe/sub; then:
//   - sub/cgroup.procs and sub/cgroup.threads list it, probe/cgroup.procs
//     (exact cgroup only) does not; sub and probe both say "populated 1";
//   - when it exits, an IN_MODIFY arrives on probe/cgroup.events (an ancestor,
//     as systemd watches a slice), and while it is still an unreaped zombie
//     both say "populated 0" and sub/cgroup.procs is empty -- on Linux a
//     process leaves its cgroup in do_exit, before its parent hears.
//
// The moves are made by the child only, so the test's own membership never
// changes. Needs root to mount cgroup2; skips otherwise. Passes on Linux.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "test_common.h"

static char mnt[PATH_MAX];

static void check(int cond, const char *what) {
    if (cond) {
        test_logf("ok: %s\n", what);
    } else {
        printf("FAIL: %s\n", what);
        failures_total++;
    }
}

// Contents of mnt/rel, or "" if unreadable. Static buffer.
static const char *slurp(const char *rel) {
    static char buf[4096];
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", mnt, rel);
    buf[0] = '\0';
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return buf;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    buf[n > 0 ? n : 0] = '\0';
    close(fd);
    return buf;
}

static int has_line(const char *text, const char *line) {
    size_t len = strlen(line);
    for (const char *p = text; (p = strstr(p, line)) != NULL; p += len) {
        if ((p == text || p[-1] == '\n') && (p[len] == '\n' || p[len] == '\0'))
            return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));
    if (geteuid() != 0) {
        printf("cgroup2_live: SKIP (needs root)\n");
        return 0;
    }
    snprintf(mnt, sizeof(mnt), "/tmp/cgroup2_live.%d", (int) getpid());
    if (mkdir(mnt, 0755) != 0) {
        printf("cgroup2_live: SKIP (cannot create %s: %s)\n", mnt, strerror(errno));
        return 0;
    }
    if (mount("cgroup2", mnt, "cgroup2", 0, NULL) != 0) {
        printf("cgroup2_live: SKIP (cannot mount cgroup2: %s)\n", strerror(errno));
        rmdir(mnt);
        return 0;
    }
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/probe", mnt);
    check(mkdir(path, 0755) == 0, "mkdir probe");
    snprintf(path, sizeof(path), "%s/probe/sub", mnt);
    check(mkdir(path, 0755) == 0, "mkdir probe/sub");

    int ready[2], release[2];
    if (pipe(ready) != 0 || pipe(release) != 0) {
        printf("FAIL: pipe\n");
        return 1;
    }
    pid_t child = fork();
    if (child == 0) {
        close(ready[0]);
        close(release[1]);
        char procs[PATH_MAX];
        snprintf(procs, sizeof(procs), "%s/probe/sub/cgroup.procs", mnt);
        int fd = open(procs, O_WRONLY);
        int ok = fd >= 0 && write(fd, "0\n", 2) == 2;
        if (fd >= 0)
            close(fd);
        char c = ok ? 'y' : 'n';
        if (write(ready[1], &c, 1) != 1)
            _exit(2);
        read(release[0], &c, 1); // until the parent closes its end
        _exit(0);
    }
    close(ready[1]);
    close(release[0]);
    char c = 0;
    check(read(ready[0], &c, 1) == 1 && c == 'y', "child wrote 0 to probe/sub/cgroup.procs");

    char me[32];
    snprintf(me, sizeof(me), "%d", (int) child);
    check(has_line(slurp("probe/sub/cgroup.procs"), me), "sub/cgroup.procs lists the child");
    check(has_line(slurp("probe/sub/cgroup.threads"), me), "sub/cgroup.threads lists the child's thread");
    check(!has_line(slurp("probe/cgroup.procs"), me), "probe/cgroup.procs (the parent cgroup) does not");
    check(has_line(slurp("probe/sub/cgroup.events"), "populated 1"), "sub populated 1 while it runs");
    check(has_line(slurp("probe/cgroup.events"), "populated 1"), "probe populated 1 (a descendant has a process)");

    char cgline[PATH_MAX];
    snprintf(cgline, sizeof(cgline), "/proc/%d/cgroup", (int) child);
    FILE *f = fopen(cgline, "r");
    char line[256] = "";
    if (f != NULL) {
        if (fgets(line, sizeof(line), f) == NULL)
            line[0] = '\0';
        fclose(f);
    }
    check(strstr(line, "0::/probe/sub") != NULL, "/proc/<child>/cgroup says 0::/probe/sub");

    int ino = inotify_init1(IN_NONBLOCK);
    snprintf(path, sizeof(path), "%s/probe/cgroup.events", mnt);
    check(ino >= 0 && inotify_add_watch(ino, path, IN_MODIFY) >= 0, "watch probe/cgroup.events");

    close(release[1]); // the child exits
    struct pollfd pfd = {.fd = ino, .events = POLLIN};
    int got = poll(&pfd, 1, 5000);
    check(got == 1, "IN_MODIFY on probe/cgroup.events when the child exits");

    // Still unreaped: WNOWAIT leaves the zombie in place.
    siginfo_t si = {};
    check(waitid(P_PID, child, &si, WEXITED | WNOWAIT) == 0 && si.si_pid == child, "child is a zombie");
    check(has_line(slurp("probe/sub/cgroup.events"), "populated 0"), "sub populated 0 with the child a zombie");
    check(has_line(slurp("probe/cgroup.events"), "populated 0"), "probe populated 0 with the child a zombie");
    check(!has_line(slurp("probe/sub/cgroup.procs"), me), "sub/cgroup.procs no longer lists it");
    waitpid(child, NULL, 0);

    snprintf(path, sizeof(path), "%s/probe/sub", mnt);
    check(rmdir(path) == 0, "rmdir probe/sub");
    snprintf(path, sizeof(path), "%s/probe", mnt);
    check(rmdir(path) == 0, "rmdir probe");
    umount(mnt);
    rmdir(mnt);
    printf("cgroup2_live: %s\n", failures_total ? "FAIL" : "PASS");
    return failures_total != 0;
}
