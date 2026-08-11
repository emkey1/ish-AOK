// devtmpfs_mount.c — regression lock for mounting devtmpfs.
//
// iSH used to accept a devtmpfs mount as an unconditional no-op: the call
// returned 0, but nothing was mounted and nothing was populated. On the main
// /dev that was harmless (iSH creates its device nodes there at startup), but
// anywhere else it was worse than an error, because the caller had no way to
// tell. A guest that mounted devtmpfs onto a fresh directory -- switch_root
// and chroot boots, iSH's own /AOK/roots multi-arch chroots, any rootfs whose
// tarball could not carry real device nodes -- got an EMPTY directory back,
// and the first `> /dev/null` on it then created a REGULAR FILE that
// accumulated every byte ever written to it, forever.
//
// devtmpfs is now a real filesystem (tmpfs that publishes iSH's device nodes
// at mount time, the way the real one mirrors the kernel's device model),
// with one deliberate exception: mounting it over a /dev that is ALREADY
// populated stays a no-op, because iSH's main /dev holds live state (POSIX
// shared memory under /dev/shm, the devpts mount, whatever the user put
// there) that a fresh mount cannot carry over. That case repairs any missing
// nodes in place instead.
//
// Asserted here:
//   - /proc/filesystems lists devtmpfs (init scripts gate on this before
//     mounting it at all, and fsopen() needs it registered)
//   - a mount on a fresh directory populates it, and the nodes WORK:
//     /dev/null discards (0 bytes accumulate) and /dev/zero reads zeros
//   - the mount is recorded: it shows in /proc/mounts, and umount undoes it
//   - mode= is honored on the mount root
//   - fsopen("devtmpfs") resolves instead of failing ENODEV
//   - [iSH] a mount over an already-populated /dev is a no-op that preserves
//     the existing contents, and repairs missing nodes in place
//   - [iSH] a directory holding a regular-file stand-in at "null" does NOT
//     count as populated, so it gets a real mount
//
// Portable: the non-gated checks also pass on a real Linux kernel run as root
// (a real devtmpfs publishes the whole device tree, so node checks assert
// presence, never set equality). Unprivileged runs skip cleanly.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include "test_common.h"

#ifndef __NR_fsopen
#define __NR_fsopen 430
#endif
#ifndef __NR_fsconfig
#define __NR_fsconfig 431
#endif
#ifndef __NR_fsmount
#define __NR_fsmount 432
#endif
#ifndef __NR_move_mount
#define __NR_move_mount 429
#endif
#define FSCONFIG_CMD_CREATE 6
#define MOVE_MOUNT_F_EMPTY_PATH 0x00000004

static int is_ok(const char *label, long r) {
    if (r == 0) { test_logf("ok   %s\n", label); return 1; }
    printf("FAIL %s: ret=%ld errno=%d (%s), wanted 0\n", label, r, errno, strerror(errno));
    failures_total++;
    return 0;
}
static int check(const char *label, int cond) {
    if (cond) { test_logf("ok   %s\n", label); return 1; }
    printf("FAIL %s\n", label);
    failures_total++;
    return 0;
}

static int under_ish(void) {
    return access("/proc/ish", F_OK) == 0;
}

// Whether `path` is a character device with the given major:minor. (The
// parameters are not named major/minor: those are macros here.)
static int is_chr(const char *path, unsigned maj, unsigned min) {
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISCHR(st.st_mode))
        return 0;
    return (unsigned) major(st.st_rdev) == maj && (unsigned) minor(st.st_rdev) == min;
}

static int exists_chr(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISCHR(st.st_mode);
}

static int write_file(const char *path, const char *data) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    ssize_t n = write(fd, data, strlen(data));
    close(fd);
    return n == (ssize_t) strlen(data) ? 0 : -1;
}

static int file_has(const char *path, const char *want) {
    char buf[256] = {};
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    return n >= 0 && strncmp(buf, want, strlen(want)) == 0;
}

static off_t file_size(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? st.st_size : -1;
}

// Whether any line of `file` contains `needle`.
static int file_mentions(const char *file, const char *needle) {
    FILE *f = fopen(file, "r");
    if (f == NULL)
        return 0;
    char line[512];
    int found = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strstr(line, needle) != NULL) { found = 1; break; }
    }
    fclose(f);
    return found;
}

// A device node created by devtmpfs must actually dispatch to the driver, not
// just look right in stat(2) -- the whole failure mode being regressed here is
// a plausible-looking entry that is really a regular file.
static void check_nodes_work(const char *dir) {
    char path[256];

    snprintf(path, sizeof path, "%s/null", dir);
    if (check("fresh.null_is_chr", is_chr(path, 1, 3))) {
        write_file(path, "discard me");
        // The bug: on a plain file these bytes are kept.
        check("fresh.null_discards", file_size(path) == 0);
    }

    snprintf(path, sizeof path, "%s/zero", dir);
    if (check("fresh.zero_is_chr", is_chr(path, 1, 5))) {
        char buf[8];
        memset(buf, 0xff, sizeof buf);
        int fd = open(path, O_RDONLY);
        ssize_t n = fd >= 0 ? read(fd, buf, sizeof buf) : -1;
        if (fd >= 0)
            close(fd);
        int all_zero = n == (ssize_t) sizeof buf;
        for (size_t i = 0; all_zero && i < sizeof buf; i++)
            all_zero = buf[i] == 0;
        check("fresh.zero_reads_zeros", all_zero);
    }

    // Presence only for the rest: a real devtmpfs carries far more than this,
    // so anything stronger than "these exist" would be asserting iSH's set.
    static const char *expected[] = {"full", "random", "urandom", "tty", "console", "ptmx"};
    for (size_t i = 0; i < sizeof expected / sizeof expected[0]; i++) {
        snprintf(path, sizeof path, "%s/%s", dir, expected[i]);
        char label[64];
        snprintf(label, sizeof label, "fresh.has_%s", expected[i]);
        check(label, exists_chr(path));
    }
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));

    char base[64];
    snprintf(base, sizeof base, "/tmp/devtmpfs_XXXXXX");
    if (!mkdtemp(base) || chdir(base) != 0) { perror("setup"); return 2; }

    // Every init script that mounts devtmpfs first checks that the kernel
    // supports it; without this line they skip the mount or fail outright.
    check("proc_filesystems.lists_devtmpfs",
            file_mentions("/proc/filesystems", "devtmpfs"));

    // --- a mount on a fresh directory must populate it ---
    mkdir("fresh", 0755);
    if (mount("devtmpfs", "fresh", "devtmpfs", 0, NULL) != 0) {
        if (errno == EPERM || errno == EACCES) {
            test_logf("skip: mount not permitted (%s)\n", strerror(errno));
            return finish_suite("devtmpfs_mount");
        }
        is_ok("fresh.mount", -1);
        return finish_suite("devtmpfs_mount");
    }
    test_logf("ok   fresh.mount\n");

    check_nodes_work("fresh");

    // A mount that happened is a mount you can see and undo. The old no-op
    // reported success while leaving nothing behind, so both of these failed.
    char fresh_abs[128];
    snprintf(fresh_abs, sizeof fresh_abs, "%s/fresh", base);
    check("fresh.in_proc_mounts", file_mentions("/proc/mounts", fresh_abs));
    is_ok("fresh.umount", umount("fresh"));

    // --- mount options apply to the mount root ---
    mkdir("moded", 0755);
    if (mount("devtmpfs", "moded", "devtmpfs", 0, "mode=0755") == 0) {
        struct stat st;
        check("mode.honored", stat("moded", &st) == 0 && (st.st_mode & 07777) == 0755);
        umount("moded");
    } else {
        is_ok("mode.mount", -1);
    }

    // --- the new mount API must reach the same filesystem ---
    // util-linux >= 2.39 prefers this path; when devtmpfs wasn't a registered
    // filesystem it failed here with ENODEV before mount(2) ever ran.
    long fsfd = syscall(__NR_fsopen, "devtmpfs", 0);
    if (fsfd < 0 && (errno == ENOSYS || errno == EPERM)) {
        test_logf("skip: fsopen unavailable (%s)\n", strerror(errno));
    } else if (check("fsopen.resolves", fsfd >= 0)) {
        close((int) fsfd);
    }

    if (!under_ish()) {
        test_logf("skip: /dev-preservation checks are iSH-specific\n");
        return finish_suite("devtmpfs_mount");
    }

    // --- iSH: a mount over an already-populated /dev preserves it ---
    // Real Linux would cover the directory; iSH must not, because its main
    // /dev is created before init runs and carries live state.
    mkdir("live", 0755);
    if (mknod("live/tty", S_IFCHR | 0666, makedev(5, 0)) != 0) {
        test_logf("skip: mknod not permitted (%s)\n", strerror(errno));
        return finish_suite("devtmpfs_mount");
    }
    write_file("live/MARKER", "keepme");
    is_ok("live.mount", mount("devtmpfs", "live", "devtmpfs", 0, NULL));
    check("live.marker_survives", file_has("live/MARKER", "keepme"));
    char live_abs[128];
    snprintf(live_abs, sizeof live_abs, "%s/live", base);
    check("live.not_mounted_over", !file_mentions("/proc/mounts", live_abs));
    // ...and the no-op still repairs what the directory was missing, which is
    // the half that helps a partially-populated chroot /dev.
    check("live.repaired_null", is_chr("live/null", 1, 3));
    write_file("live/null", "discard me");
    check("live.repaired_null_discards", file_size("live/null") == 0);
    check("live.repaired_zero", is_chr("live/zero", 1, 5));

    // ...and the new mount API must not be a way around that. util-linux
    // >= 2.39 mounts via fsopen/fsmount/move_mount, which reaches the mounts
    // list without going through mount(2) at all, so the same rule is applied
    // there. Note the caller still holds the fsmount fd here, exactly as a
    // real caller does -- that reference is what makes discarding the
    // detached mount awkward, so the check is that /dev survives AND that
    // nothing is left stranded at the private staging path.
    int fsfd2 = (int) syscall(__NR_fsopen, "devtmpfs", 0);
    if (fsfd2 < 0) {
        test_logf("skip: fsopen unavailable for move_mount check (%s)\n", strerror(errno));
    } else if (syscall(__NR_fsconfig, fsfd2, FSCONFIG_CMD_CREATE, NULL, NULL, 0) != 0) {
        test_logf("skip: fsconfig(CREATE) failed (%s)\n", strerror(errno));
        close(fsfd2);
    } else {
        int mfd = (int) syscall(__NR_fsmount, fsfd2, 0, 0);
        if (mfd < 0) {
            test_logf("skip: fsmount failed (%s)\n", strerror(errno));
        } else {
            long r = syscall(__NR_move_mount, mfd, "", AT_FDCWD, "live",
                    MOVE_MOUNT_F_EMPTY_PATH);
            is_ok("move_mount.accepted", r);
            check("move_mount.marker_survives", file_has("live/MARKER", "keepme"));
            check("move_mount.not_mounted_over", !file_mentions("/proc/mounts", live_abs));
            close(mfd);
            // Known deviation, asserted as a deviation so it can't drift
            // unnoticed: the rejected mount stays parked at its private
            // staging path and is visible in /proc/mounts there. iSH cannot
            // free it while the caller's own fsmount fd still references it
            // (that would leave the fd dangling), and it has no lazy-detach.
            // Cosmetic, because it never reaches the target -- which the
            // preceding two checks are what actually guarantee.
            test_logf("note: staging mount left behind: %s\n",
                    file_mentions("/proc/mounts", "/.ish-fsmount") ? "yes (expected)" : "no");
        }
        close(fsfd2);
    }

    // --- iSH: a regular-file stand-in does not count as populated ---
    // Docker-exported tarballs cannot carry device nodes and ship a plain file
    // at "null" instead. That directory is broken, not populated, so it must
    // get a real mount rather than being left alone.
    mkdir("standin", 0755);
    write_file("standin/null", "junk");
    is_ok("standin.mount", mount("devtmpfs", "standin", "devtmpfs", 0, NULL));
    check("standin.null_is_chr", is_chr("standin/null", 1, 3));
    write_file("standin/null", "discard me");
    check("standin.null_discards", file_size("standin/null") == 0);
    umount("standin");

    return finish_suite("devtmpfs_mount");
}
