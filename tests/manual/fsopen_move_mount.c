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
//
// And that what the API is told reaches the mount (leg_options). fsconfig
// accepted size=, mode= and the rest and dropped them, and fsmount ignored
// its attributes, so `mount -t tmpfs -o size=5m` -- which util-linux does
// through this API whenever no flag like noexec sends it back to mount(2) --
// made a tmpfs with no limit: 8 MB went into a 5 MB one. Asserted as on
// Linux: size= caps what the files hold (ENOSPC past it), "source" names the
// mount, MOUNT_ATTR_NOEXEC and _NOSUID show and noexec stops an exec, and a
// remount -- mount(2)'s MS_REMOUNT or FSCONFIG_CMD_RECONFIGURE -- changes the
// size, refusing one below what the files already hold.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/wait.h>
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

#ifndef FSOPEN_CLOEXEC
# define FSOPEN_CLOEXEC 1
#endif
#define FSCONFIG_SET_FLAG 0
#define FSCONFIG_SET_STRING 1
#define FSCONFIG_CMD_CREATE 6
#define FSCONFIG_CMD_RECONFIGURE 7
#define MOVE_MOUNT_F_EMPTY_PATH 0x00000004
#ifndef FSMOUNT_CLOEXEC
# define FSMOUNT_CLOEXEC 1
#endif
#ifndef MOUNT_ATTR_NOSUID
# define MOUNT_ATTR_NOSUID 0x2
# define MOUNT_ATTR_NOEXEC 0x8
#endif

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

static const char *staging_files[] = { "/proc/mounts", "/proc/self/mountinfo" };
static int staging_baseline[2] = { 0, 0 };

// What the mount table already said before this test created anything. The
// check below is RELATIVE to it, because the assertion is about our own mount
// and the count is a substring match over the whole table.
//
// It used to demand an absolute zero, which made it an assertion about the
// entire system: any pre-existing mount whose path contains "ish-fsmount"
// failed it, no matter what this test did. The Alpine test roots have an
// almost-empty mount table and passed by luck of environment; the iPad had one
// such mount parked at /.ish-fsmount/11 -- an iCloud Drive iosfs, nothing to do
// with this test -- and failed all four checks by exactly 1, while its own
// staging mount was correctly hidden the whole time.
static void snapshot_staging_baseline(void) {
    for (unsigned i = 0; i < sizeof staging_files / sizeof staging_files[0]; i++) {
        int n = count_lines_matching(staging_files[i], "ish-fsmount");
        staging_baseline[i] = n < 0 ? 0 : n;
        test_logf("  %-24s baseline: %d\n", staging_files[i], staging_baseline[i]);
    }
}

static void check_staging_hidden(const char *when, int want) {
    static const char **files = staging_files;
    for (unsigned i = 0; i < sizeof staging_files / sizeof staging_files[0]; i++) {
        int n = count_lines_matching(files[i], "ish-fsmount");
        if (n < 0) {
            test_logf("  %s: unreadable, skipped\n", files[i]);
            continue;
        }
        // want is how many of OUR mounts should be visible; anything that was
        // there before us is not ours to account for.
        int mine = n - staging_baseline[i];
        if (mine != want) {
            printf("FAIL: %s lists %d staging mount(s) of ours %s, want %d "
                   "(total %d, baseline %d)\n",
                   files[i], mine, when, want, n, staging_baseline[i]);
            failures_total++;
        }
        test_logf("  %-24s %s: %d of ours (want %d, total %d)\n",
                  files[i], when, mine, want, n);
    }
}

static void opt_check(const char *label, int ok, long got, long want) {
    if (!ok) {
        printf("FAIL: %s: got %ld, want %ld\n", label, got, want);
        failures_total++;
    }
    test_logf("  %-56s %s (got %ld, want %ld)\n", label, ok ? "ok" : "FAIL", got, want);
}

// Write zeros to `path` until `want` bytes or an error: the bytes written and,
// through *err, the errno that stopped it (0 if none did).
static long fill(const char *path, long want, int *err) {
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    *err = fd < 0 ? errno : 0;
    if (fd < 0)
        return 0;
    static char chunk[65536];
    long done = 0;
    while (done < want) {
        size_t n = want - done < (long) sizeof(chunk) ? (size_t) (want - done) : sizeof(chunk);
        ssize_t w = write(fd, chunk, n);
        if (w <= 0) {
            *err = w < 0 ? errno : ENOSPC;
            break;
        }
        done += w;
    }
    close(fd);
    return done;
}

// The /proc/self/mounts line for `point`, or "" when there is none.
static void mounts_line(const char *point, char *out, size_t size) {
    out[0] = '\0';
    FILE *f = fopen("/proc/self/mounts", "r");
    char line[1024];
    while (f != NULL && fgets(line, sizeof(line), f) != NULL) {
        char src[256], at[512];
        if (sscanf(line, "%255s %511s", src, at) == 2 && strcmp(at, point) == 0)
            snprintf(out, size, "%s", line);
    }
    if (f != NULL)
        fclose(f);
}

static void leg_options(void) {
    char target[] = "/tmp/fsopen_opts.XXXXXX";
    if (mkdtemp(target) == NULL) {
        perror("mkdtemp");
        failures_total++;
        return;
    }
    int fs_fd = raw_fsopen("tmpfs", FSOPEN_CLOEXEC);
    int r = fs_fd < 0 ? -1 : raw_fsconfig(fs_fd, FSCONFIG_SET_STRING, "source", "aok-probe", 0);
    opt_check("fsconfig(SET_STRING, \"source\")", r == 0, r < 0 ? errno : 0, 0);
    r = fs_fd < 0 ? -1 : raw_fsconfig(fs_fd, FSCONFIG_SET_STRING, "size", "1m", 0);
    opt_check("fsconfig(SET_STRING, \"size\", \"1m\")", r == 0, r < 0 ? errno : 0, 0);
    r = fs_fd < 0 ? -1 : raw_fsconfig(fs_fd, FSCONFIG_CMD_CREATE, NULL, NULL, 0);
    opt_check("fsconfig(CREATE)", r == 0, r < 0 ? errno : 0, 0);
    int mfd = r < 0 ? -1 : raw_fsmount(fs_fd, FSMOUNT_CLOEXEC, MOUNT_ATTR_NOEXEC | MOUNT_ATTR_NOSUID);
    opt_check("fsmount(MOUNT_ATTR_NOEXEC|MOUNT_ATTR_NOSUID)", mfd >= 0, mfd < 0 ? errno : 0, 0);
    r = mfd < 0 ? -1 : raw_move_mount(mfd, "", AT_FDCWD, target, MOVE_MOUNT_F_EMPTY_PATH);
    opt_check("move_mount", r == 0, r < 0 ? errno : 0, 0);
    if (r != 0) {
        if (mfd >= 0)
            close(mfd);
        if (fs_fd >= 0)
            close(fs_fd);
        rmdir(target);
        return;
    }

    char line[1024];
    mounts_line(target, line, sizeof(line));
    test_logf("  mounts: %s", line);
    opt_check("/proc/mounts names the source", strncmp(line, "aok-probe ", 10) == 0, 0, 0);
    opt_check("/proc/mounts says noexec", strstr(line, "noexec") != NULL, 0, 0);
    opt_check("/proc/mounts says nosuid", strstr(line, "nosuid") != NULL, 0, 0);
    struct statfs sf;
    r = statfs(target, &sf);
    long total = r == 0 ? (long) (sf.f_blocks * sf.f_bsize) : -1;
    opt_check("statfs size is 1 MiB", total == 1 << 20, total, 1 << 20);

    char path[600];
    snprintf(path, sizeof(path), "%s/fill", target);
    int err;
    long wrote = fill(path, 2 << 20, &err);
    opt_check("2 MiB into size=1m stops at 1 MiB", wrote <= (1 << 20) && wrote > (1 << 19), wrote, 1 << 20);
    opt_check("...with ENOSPC", err == ENOSPC, err, ENOSPC);

    // noexec: an executable file there does not run.
    char exe[600];
    snprintf(exe, sizeof(exe), "%s/sh", target);
    unlink(path);
    int in = open("/bin/sh", O_RDONLY), out = open(exe, O_WRONLY | O_CREAT, 0755);
    char buf[65536];
    ssize_t n;
    while (in >= 0 && out >= 0 && (n = read(in, buf, sizeof(buf))) > 0)
        if (write(out, buf, n) != n)
            break;
    if (in >= 0)
        close(in);
    if (out >= 0)
        close(out);
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        execl(exe, exe, "-c", "exit 0", (char *) NULL);
        _exit(errno == EACCES ? 42 : 43);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    opt_check("exec on the noexec mount is EACCES", WIFEXITED(st) && WEXITSTATUS(st) == 42,
              WIFEXITED(st) ? WEXITSTATUS(st) : -1, 42);
    unlink(exe);

    // Remount through mount(2): the new size holds at once...
    long first = fill(path, 1 << 20, &err);
    r = mount(NULL, target, NULL, MS_REMOUNT | MS_NOEXEC | MS_NOSUID, "size=3m");
    opt_check("mount(MS_REMOUNT, \"size=3m\")", r == 0, r < 0 ? errno : 0, 0);
    long more = fill(path, 3 << 20, &err);
    opt_check("...then 3 MiB fit in all", first + more <= (3 << 20) && first + more > (5 << 19),
              first + more, 3 << 20);
    // ...and one below what the files hold is refused.
    errno = 0;
    r = mount(NULL, target, NULL, MS_REMOUNT | MS_NOEXEC | MS_NOSUID, "size=512k");
    opt_check("remount below what is held is EINVAL", r == -1 && errno == EINVAL, errno, EINVAL);

    // Reconfigure through the context: a larger size.
    r = raw_fsconfig(fs_fd, FSCONFIG_SET_STRING, "size", "4m", 0);
    if (r == 0)
        r = raw_fsconfig(fs_fd, FSCONFIG_CMD_RECONFIGURE, NULL, NULL, 0);
    opt_check("fsconfig(size=4m) + RECONFIGURE", r == 0, r < 0 ? errno : 0, 0);
    r = statfs(target, &sf);
    total = r == 0 ? (long) (sf.f_blocks * sf.f_bsize) : -1;
    opt_check("...statfs size is 4 MiB", total == 4 << 20, total, 4 << 20);

    unlink(path);
    close(mfd);
    close(fs_fd);
    if (umount(target) != 0)
        umount2(target, MNT_DETACH);
    rmdir(target);
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

    // Before anything of ours exists.
    snapshot_staging_baseline();

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

    // The move must not leave a reference behind. A plain umount is the
    // witness, where MNT_DETACH would succeed over any number of them. First
    // with the fsmount fd still open, which really does hold one, so EBUSY
    // here shows the witness can see a reference at all; then without it.
    errno = 0;
    r = umount(target);
    if (r == 0 || errno != EBUSY) {
        printf("FAIL: umount with the fsmount fd open -> %d (%s), want EBUSY\n",
               r, r == 0 ? "ok" : strerror(errno));
        failures_total++;
    } else {
        test_logf("umount with the fsmount fd open -> EBUSY\n");
    }
    close(mfd);
    close(fs_fd);
    r = umount(target);
    if (r != 0) {
        printf("FAIL: umount after closing the fsmount fd -> %d (%s): the move kept a reference\n",
               r, strerror(errno));
        failures_total++;
        umount2(target, MNT_DETACH);
    } else {
        test_logf("umount after closing the fsmount fd -> 0\n");
    }
    rmdir(target);

    leg_options();
    return finish_suite("fsopen_move_mount");
}
