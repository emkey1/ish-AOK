// Ownership and sticky-bit enforcement on metadata changes and deletions.
//
// Two regressions, both letting an unprivileged process do things Linux
// forbids:
//
//  * Neither generic_setattrat nor generic_fsetattr checked permissions at all,
//    so any process could chmod, chown or truncate a file it did not own.
//    Reported upstream as ish-app/ish#2197. Fixed with setattr_check(),
//    modelled on Linux's setattr_prepare(): chmod and chown are
//    owner-or-privileged regardless of the file's mode bits, while a size
//    change is an ordinary write.
//
//  * The sticky bit (S_ISVTX) was not enforced, so an unprivileged process
//    could unlink or rename another user's files out of a world-writable /tmp
//    -- the whole reason a sticky /tmp is safe. Fixed with sticky_check(),
//    modelled on Linux's check_sticky(). rename is subject to it twice: it
//    removes the source entry from its parent and replaces the destination if
//    one exists.
//
// The permitted operations are checked as carefully as the forbidden ones: a
// too-aggressive check would break ordinary use by the file's own owner, and
// AOK-iSH runs a non-root default user.
//
// Arch-neutral. Needs root (it drops privileges in a child). Takes an optional
// base directory argument so it can be pointed at a different filesystem;
// defaults to /tmp.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "test_common.h"

#define NOBODY 65534

static void want_err(const char *what, int r, int e1, int e2) {
    if (r < 0 && (errno == e1 || errno == e2)) {
        test_logf("  ok   %s -> %s\n", what, strerror(errno));
        return;
    }
    if (r == 0)
        printf("FAIL: %s unexpectedly succeeded (want %s)\n", what, strerror(e1));
    else
        printf("FAIL: %s -> %s (want %s)\n", what, strerror(errno), strerror(e1));
    failures_total++;
}

static void want_ok(const char *what, int r) {
    if (r == 0) {
        test_logf("  ok   %s allowed\n", what);
        return;
    }
    printf("FAIL: %s -> %s (want success)\n", what, strerror(errno));
    failures_total++;
}

static int mkfile(const char *path, uid_t owner, mode_t mode) {
    unlink(path);
    int fd = open(path, O_CREAT | O_WRONLY, mode);
    if (fd < 0)
        return -1;
    if (write(fd, "hello", 5) != 5) {
        close(fd);
        return -1;
    }
    close(fd);
    return chown(path, owner, owner);
}

int main(int argc, char **argv) {
    const char *base = "/tmp";
    for (int i = 1; i < argc; i++)
        if (argv[i][0] != '-')
            base = argv[i];
    // Hand the remaining args (e.g. -v) to the common init.
    int fake_argc = argc > 1 && argv[1][0] == '-' ? 2 : 1;
    test_init(fake_argc, argv);
    alarm(test_watchdog_secs(60));

    if (geteuid() != 0) {
        printf("file_perms: SKIP (not privileged: euid=%d)\n", (int) geteuid());
        return 0;
    }

    char rootf[512], rootd[512], stickyd[512], plaind[512];
    char sticky_rootf[512], sticky_rootf2[512], sticky_mine[512], sticky_rootdir[512];
    char plain_rootf[512], moved[512];
    snprintf(rootf, sizeof rootf, "%s/fp_rootfile", base);
    snprintf(rootd, sizeof rootd, "%s/fp_rootdir", base);
    snprintf(stickyd, sizeof stickyd, "%s/fp_sticky", base);
    snprintf(plaind, sizeof plaind, "%s/fp_plain", base);
    snprintf(sticky_rootf, sizeof sticky_rootf, "%s/rootfile", stickyd);
    snprintf(sticky_rootf2, sizeof sticky_rootf2, "%s/rootfile2", stickyd);
    snprintf(sticky_mine, sizeof sticky_mine, "%s/myfile", stickyd);
    snprintf(sticky_rootdir, sizeof sticky_rootdir, "%s/rootdir", stickyd);
    snprintf(plain_rootf, sizeof plain_rootf, "%s/rootfile", plaind);
    snprintf(moved, sizeof moved, "%s/moved", stickyd);

    // Root-owned 0644 file and 0755 directory, both outside any sticky dir.
    if (mkfile(rootf, 0, 0644) < 0) {
        printf("file_perms: FAIL setup: %s\n", strerror(errno));
        return 1;
    }
    rmdir(rootd);
    if (mkdir(rootd, 0755) < 0 || chown(rootd, 0, 0) < 0) {
        printf("file_perms: FAIL setup mkdir: %s\n", strerror(errno));
        return 1;
    }

    // A sticky world-writable directory (like /tmp) and a plain one.
    unlink(sticky_rootf); unlink(sticky_rootf2); unlink(sticky_mine);
    rmdir(sticky_rootdir); rmdir(stickyd);
    unlink(plain_rootf); rmdir(plaind);
    if (mkdir(stickyd, 0777) < 0 || chmod(stickyd, 01777) < 0 || chown(stickyd, 0, 0) < 0 ||
        mkdir(plaind, 0777) < 0 || chmod(plaind, 0777) < 0 || chown(plaind, 0, 0) < 0) {
        printf("file_perms: FAIL setup dirs: %s\n", strerror(errno));
        return 1;
    }
    if (mkfile(sticky_rootf, 0, 0666) < 0 || mkfile(sticky_rootf2, 0, 0666) < 0 ||
        mkfile(sticky_mine, NOBODY, 0666) < 0 || mkfile(plain_rootf, 0, 0666) < 0) {
        printf("file_perms: FAIL setup entries: %s\n", strerror(errno));
        return 1;
    }
    if (mkdir(sticky_rootdir, 0777) < 0 || chown(sticky_rootdir, 0, 0) < 0) {
        printf("file_perms: FAIL setup sticky dir: %s\n", strerror(errno));
        return 1;
    }

    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) {
        printf("file_perms: FAIL fork: %s\n", strerror(errno));
        return 1;
    }
    if (pid == 0) {
        if (setgid(NOBODY) < 0 || setuid(NOBODY) < 0) {
            printf("FAIL: could not drop privileges: %s\n", strerror(errno));
            fflush(NULL);
            _exit(2);
        }

        // --- metadata changes on a file we do not own ----------------------
        want_err("chmod of a root-owned file", chmod(rootf, 0777), EPERM, EPERM);
        want_err("chown of a root-owned file", chown(rootf, NOBODY, NOBODY), EPERM, EPERM);
        want_err("chmod of a root-owned dir", chmod(rootd, 0777), EPERM, EPERM);
        want_err("truncate of a root-owned 0644 file", truncate(rootf, 0), EACCES, EACCES);
        want_err("open(O_WRONLY) of a root-owned 0644 file",
                 open(rootf, O_WRONLY) < 0 ? -1 : 0, EACCES, EACCES);

        // --- sticky directory ---------------------------------------------
        want_err("unlink of root's file in a sticky dir",
                 unlink(sticky_rootf), EPERM, EPERM);
        want_err("rmdir of root's dir in a sticky dir",
                 rmdir(sticky_rootdir), EPERM, EPERM);
        want_err("rename of root's file in a sticky dir",
                 rename(sticky_rootf, moved), EPERM, EPERM);
        want_ok("unlink of our OWN file in a sticky dir", unlink(sticky_mine));
        want_err("unlink of root's file in a sticky dir (again)",
                 unlink(sticky_rootf2), EPERM, EPERM);
        // Without the sticky bit, directory write permission is all it takes.
        want_ok("unlink of root's file in a NON-sticky 0777 dir", unlink(plain_rootf));

        printf("  (child) %u failure(s)\n", failures_total);
        fflush(NULL);
        _exit(failures_total != 0 ? 1 : 0);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        failures_total++;

    // --- the same operations must still work for their owner and for root ---
    want_ok("root chmod", chmod(rootf, 0600));
    want_ok("root chown", chown(rootf, NOBODY, NOBODY));
    want_ok("root truncate", truncate(rootf, 2));
    want_ok("root unlink in a sticky dir", unlink(sticky_rootf));
    want_ok("root rmdir in a sticky dir", rmdir(sticky_rootdir));

    fflush(NULL);
    pid = fork();
    if (pid == 0) {
        char mine[512];
        snprintf(mine, sizeof mine, "%s/owned", plaind);
        if (setgid(NOBODY) < 0 || setuid(NOBODY) < 0)
            _exit(2);
        unlink(mine);
        int fd = open(mine, O_CREAT | O_RDWR, 0644);
        if (fd < 0) {
            printf("FAIL: owner could not create its own file: %s\n", strerror(errno));
            fflush(NULL);
            _exit(1);
        }
        if (write(fd, "hello", 5) != 5) { close(fd); _exit(1); }
        close(fd);
        want_ok("owner chmod of its own file", chmod(mine, 0600));
        want_ok("owner truncate of its own file", truncate(mine, 2));
        want_ok("owner chgrp of its own file", chown(mine, (uid_t) -1, NOBODY));
        want_ok("owner unlink of its own file", unlink(mine));
        printf("  (child) %u failure(s)\n", failures_total);
        fflush(NULL);
        _exit(failures_total != 0 ? 1 : 0);
    }
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        failures_total++;

    unlink(rootf); rmdir(rootd);
    unlink(sticky_rootf2);
    rmdir(stickyd); rmdir(plaind);

    if (failures_total != 0) {
        printf("file_perms: FAIL failures=%u\n", failures_total);
        return 1;
    }
    printf("file_perms: PASS\n");
    return 0;
}
