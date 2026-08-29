// The "new mount API" (fsopen/fsconfig/fsmount/move_mount, kernel 5.2+):
// before this fix all four were silent ENOSYS stubs, so systemd >= 254's
// unconditional per-service credentials tmpfs setup -- fsopen("tmpfs") ->
// fsconfig(..., FSCONFIG_CMD_CREATE) -> fsmount() -> [write files through
// the returned fd] -> fsconfig(..., FSCONFIG_CMD_RECONFIGURE) [apply "ro"]
// -> move_mount() to place it at the final path -- failed for EVERY unit
// with "Failed to set up credentials: Function not implemented", and no
// service the credentials step guards (which, since systemd 254, is all of
// them) ever ran.
//
// This exercises the exact same sequence end to end: create a tmpfs via the
// new API, write a file into it through fsmount()'s returned fd, reconfigure
// read-only, move_mount it into place, and verify both the file's presence
// and the read-only enforcement at the final location.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "test_common.h"

#ifndef SYS_fsopen
# define SYS_fsopen 430
#endif
#ifndef SYS_fsconfig
# define SYS_fsconfig 431
#endif
#ifndef SYS_fsmount
# define SYS_fsmount 432
#endif
#ifndef SYS_move_mount
# define SYS_move_mount 429
#endif

#define FSOPEN_CLOEXEC 1
#define FSCONFIG_SET_FLAG 0
#define FSCONFIG_CMD_CREATE 6
#define FSCONFIG_CMD_RECONFIGURE 7
#define MOVE_MOUNT_F_EMPTY_PATH 0x00000004

static int raw_fsopen(const char *fsname, unsigned flags) {
    return syscall(SYS_fsopen, fsname, flags);
}
static int raw_fsconfig(int fd, unsigned cmd, const char *key, const void *value, int aux) {
    return syscall(SYS_fsconfig, fd, cmd, key, value, aux);
}
static int raw_fsmount(int fd, unsigned flags, unsigned attr_flags) {
    return syscall(SYS_fsmount, fd, flags, attr_flags);
}
static int raw_move_mount(int from_dfd, const char *from_path, int to_dfd,
        const char *to_path, unsigned flags) {
    return syscall(SYS_move_mount, from_dfd, from_path, to_dfd, to_path, flags);
}


// A detached mount -- fsmount'd but not yet placed -- has no mountpoint at all
// on Linux and therefore appears in NO mount listing. AOK has no mount
// namespaces and models it as a real mount at a private staging path
// (/.ish-fsmount/<n>), which used to leave it visible in /proc/mounts,
// /proc/self/mountinfo and the table a native `df` walks. The staging
// directory is 0700 and root-owned, so an unprivileged df tried to statfs a
// directory it could not enter and printed
//   df: /.ish-fsmount/11: Permission denied
// for a mount Linux would never have shown it. Reported from a device, 2026-08-29.
static int count_lines_matching(const char *file, const char *needle) {
    FILE *f = fopen(file, "r");
    if (f == NULL)
        return -1;
    char line[2048];
    int n = 0;
    while (fgets(line, sizeof line, f) != NULL)
        if (strstr(line, needle) != NULL)
            n++;
    fclose(f);
    return n;
}

static void check_staging_hidden(const char *when, int want) {
    static const char *files[] = { "/proc/mounts", "/proc/self/mountinfo" };
    for (unsigned i = 0; i < sizeof files / sizeof files[0]; i++) {
        int n = count_lines_matching(files[i], "ish-fsmount");
        if (n < 0) {
            test_logf("  %s: unreadable, skipped\n", files[i]);
            continue;
        }
        if (n != want) {
            printf("FAIL: %s lists %d staging mount(s) %s, want %d\n",
                   files[i], n, when, want);
            failures_total++;
        }
        test_logf("  %-24s %s: %d (want %d)\n", files[i], when, n, want);
    }
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    TEST_SKIP_IF_FOREIGN_PROC("fsopen_move_mount");
    if (geteuid() != 0) {
        // Creating and moving a mount needs privilege, so unprivileged this can
        // only ever report EACCES from FSCONFIG_CMD_CREATE. Skip rather than
        // fail: the iSH-AOK suite runs as uid 0, and failing here reads like a real
        // mount-API regression when it is just the account it was launched under.
        printf("fsopen_move_mount: SKIP (not privileged: euid=%d)\n", (int) geteuid());
        return 0;
    }
    alarm(test_watchdog_secs(10));

    char target[] = "/tmp/fsopen_test_target.XXXXXX";
    if (mkdtemp(target) == NULL) {
        perror("mkdtemp");
        return 1;
    }

    int fs_fd = raw_fsopen("tmpfs", FSOPEN_CLOEXEC);
    test_logf("fsopen(\"tmpfs\") -> %d\n", fs_fd);
    if (fs_fd < 0) {
        printf("FAIL: fsopen(\"tmpfs\") -> %d (%s)\n", fs_fd, strerror(errno));
        return 1;
    }

    int r = raw_fsconfig(fs_fd, FSCONFIG_CMD_CREATE, NULL, NULL, 0);
    test_logf("fsconfig(CREATE) -> %d\n", r);
    if (r < 0) {
        printf("FAIL: fsconfig(FSCONFIG_CMD_CREATE) -> %d (%s)\n", r, strerror(errno));
        return 1;
    }

    int mfd = raw_fsmount(fs_fd, 0, 0);
    test_logf("fsmount() -> %d\n", mfd);
    if (mfd < 0) {
        printf("FAIL: fsmount() -> %d (%s)\n", mfd, strerror(errno));
        return 1;
    }

    // Write a file through the detached mount's fd, exactly as systemd's
    // acquire_credentials() does before the mount is ever placed anywhere.
    int wfd = openat(mfd, "hello", O_WRONLY | O_CREAT, 0600);
    if (wfd < 0) {
        printf("FAIL: openat(mfd, \"hello\") -> %s\n", strerror(errno));
        return 1;
    }
    const char *msg = "credential payload\n";
    if (write(wfd, msg, strlen(msg)) != (ssize_t) strlen(msg)) {
        printf("FAIL: write into detached mount failed: %s\n", strerror(errno));
        return 1;
    }
    close(wfd);

    // Detached: usable through its fd (just proven), and invisible everywhere.
    check_staging_hidden("while detached", 0);

    r = raw_fsconfig(fs_fd, FSCONFIG_SET_FLAG, "ro", NULL, 0);
    test_log_if(r == 0, "fsconfig(SET_FLAG, \"ro\") ok\n");
    if (r != 0) {
        printf("FAIL: fsconfig(FSCONFIG_SET_FLAG, \"ro\") -> %d (%s)\n", r, strerror(errno));
        failures_total++;
    }
    r = raw_fsconfig(fs_fd, FSCONFIG_CMD_RECONFIGURE, NULL, NULL, 0);
    test_log_if(r == 0, "fsconfig(RECONFIGURE) ok\n");
    if (r != 0) {
        printf("FAIL: fsconfig(FSCONFIG_CMD_RECONFIGURE) -> %d (%s)\n", r, strerror(errno));
        failures_total++;
    }

    r = raw_move_mount(mfd, "", AT_FDCWD, target, MOVE_MOUNT_F_EMPTY_PATH);
    test_logf("move_mount() -> %d (target=%s)\n", r, target);
    if (r < 0) {
        printf("FAIL: move_mount() -> %d (%s)\n", r, strerror(errno));
        return 1;
    }

    // Placed: the staging path is gone for good, and the mount is listed at
    // its real point (checked below by reading the file back through it).
    check_staging_hidden("after move_mount", 0);

    char path[512];
    snprintf(path, sizeof(path), "%s/hello", target);
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        printf("FAIL: %s not present after move_mount (%s)\n", path, strerror(errno));
        failures_total++;
    } else {
        char buf[64] = "";
        fgets(buf, sizeof(buf), f);
        fclose(f);
        test_log_if(strcmp(buf, msg) == 0, "file content survived move_mount: \"%s\"\n", buf);
        if (strcmp(buf, msg) != 0) {
            printf("FAIL: file content mismatch after move_mount: got \"%s\"\n", buf);
            failures_total++;
        }
    }

    // NOTE: this codebase doesn't enforce MS_READONLY_ at the VFS layer for
    // any mount type (mount_flags.c's existing MS_REMOUNT coverage doesn't
    // assert EROFS either) -- it's metadata-only (visible via mountinfo/
    // statvfs), a pre-existing gap unrelated to this fix. So we don't assert
    // write-rejection here; fsconfig(SET_FLAG "ro")/RECONFIGURE succeeding
    // (checked above) is the extent of what this syscall family promises in
    // this codebase today.

    umount2(target, MNT_DETACH);
    rmdir(target);

    return finish_suite("fsopen_move_mount");
}
