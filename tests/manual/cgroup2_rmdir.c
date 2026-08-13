// An empty cgroup must be removable with rmdir(2).
//
// cgroup2 gives every directory it creates a set of interface files
// (cgroup.procs, cgroup.controllers, cgroup.events, ...). They belong to the
// filesystem, not to the guest: on Linux they do not count toward the
// directory's emptiness, so rmdir on a cgroup with no processes and no child
// cgroups succeeds. iSH backs cgroup2 with tmpfs (fs/tmp.c) and created those
// interface files as ordinary tmpfs files, so the generic emptiness test saw
// seven children and every rmdir answered ENOTEMPTY -- for EVERY cgroup, since
// a cgroup is born holding them:
//
//     mkdir /sys/fs/cgroup/probe.scope     # fine
//     rmdir /sys/fs/cgroup/probe.scope     # rmdir: Directory not empty
//
// systemd deletes cgroups with rmdir and nothing else, so nothing it created
// was ever cleaned up. Measured on an Arch guest: cgroups accumulated for the
// life of the boot and could not be cleared by hand either, which is a leak
// that grows with every service start and every login session.
//
// The interesting part is what must NOT change. "Empty" has to keep meaning
// empty: a cgroup holding a child cgroup, or any file the guest itself put
// there, must still refuse. And the discount is specific to cgroup2 -- a plain
// tmpfs directory with a file in it must still be ENOTEMPTY, or this would have
// quietly turned rmdir into rm -r for every tmpfs in the system.
//
// Needs root to mount cgroup2; skips cleanly otherwise. Also passes on real
// Linux, where it is checking the kernel's own behaviour.
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>
#include "test_common.h"

static void check(int cond, const char *what) {
    if (cond) {
        test_logf("ok: %s\n", what);
    } else {
        printf("FAIL: %s (errno=%d %s)\n", what, errno, strerror(errno));
        failures_total++;
    }
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));

    char mnt[PATH_MAX], path[PATH_MAX], child[PATH_MAX];
    snprintf(mnt, sizeof(mnt), "/tmp/cgroup2_rmdir.%d", (int) getpid());
    if (mkdir(mnt, 0755) != 0) {
        printf("cgroup2_rmdir: SKIP (cannot create %s: %s)\n", mnt, strerror(errno));
        return 0;
    }
    if (mount("cgroup2", mnt, "cgroup2", 0, NULL) != 0) {
        printf("cgroup2_rmdir: SKIP (cannot mount cgroup2: %s; needs root)\n", strerror(errno));
        rmdir(mnt);
        return 0;
    }

    // A fresh cgroup arrives holding the interface files. That is the state the
    // emptiness test has to see through, so assert it rather than assume it --
    // if a future cgroup2 stops creating them, this test would otherwise pass
    // while checking nothing.
    snprintf(path, sizeof(path), "%s/probe.scope", mnt);
    check(mkdir(path, 0755) == 0, "create a cgroup");
    unsigned interface_files = 0;
    DIR *d = opendir(path);
    check(d != NULL, "open the new cgroup");
    if (d != NULL) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
                continue;
            interface_files++;
            test_logf("   interface file: %s\n", ent->d_name);
        }
        closedir(d);
    }
    check(interface_files > 0, "a fresh cgroup holds interface files");

    // The fix: those files do not make it non-empty.
    check(rmdir(path) == 0, "rmdir an empty cgroup succeeds");
    check(access(path, F_OK) != 0, "the cgroup is gone");

    // "Empty" still means empty: a child cgroup blocks removal.
    check(mkdir(path, 0755) == 0, "recreate the cgroup");
    snprintf(child, sizeof(child), "%s/inner", path);
    check(mkdir(child, 0755) == 0, "create a child cgroup");
    check(rmdir(path) != 0 && errno == ENOTEMPTY,
            "rmdir a cgroup holding a child cgroup returns ENOTEMPTY");
    check(rmdir(child) == 0, "the child removes");
    check(rmdir(path) == 0, "and then the parent does too");

    // A file the guest created must also block removal. Writable interface
    // files exist, so this uses a name of its own rather than reusing one.
    check(mkdir(path, 0755) == 0, "recreate the cgroup again");
    char guest_file[PATH_MAX];
    snprintf(guest_file, sizeof(guest_file), "%s/not-an-interface-file", path);
    int fd = open(guest_file, O_WRONLY | O_CREAT, 0644);
    if (fd >= 0) {
        close(fd);
        check(rmdir(path) != 0 && errno == ENOTEMPTY,
                "rmdir a cgroup holding a guest-created file returns ENOTEMPTY");
        check(unlink(guest_file) == 0, "remove the guest file");
    } else {
        // Real cgroup2 refuses to create arbitrary files; that is fine, the
        // child-cgroup case above already pins the "still means empty" half.
        test_logf("skip: cgroup2 here does not allow creating a plain file (errno=%d)\n", errno);
    }
    check(rmdir(path) == 0, "the cgroup removes once emptied");

    umount(mnt);
    rmdir(mnt);

    // The discount must be cgroup2-only: an ordinary tmpfs directory with a
    // file in it still refuses, or rmdir would have become rm -r everywhere.
    char plain[PATH_MAX], plain_file[PATH_MAX];
    snprintf(plain, sizeof(plain), "/tmp/cgroup2_rmdir_plain.%d", (int) getpid());
    check(mkdir(plain, 0755) == 0, "create a plain tmpfs directory");
    snprintf(plain_file, sizeof(plain_file), "%s/f", plain);
    fd = open(plain_file, O_WRONLY | O_CREAT, 0644);
    check(fd >= 0, "create a file in it");
    if (fd >= 0)
        close(fd);
    check(rmdir(plain) != 0 && errno == ENOTEMPTY,
            "rmdir a non-empty plain tmpfs directory still returns ENOTEMPTY");
    unlink(plain_file);
    check(rmdir(plain) == 0, "and removes once emptied");

    return finish_suite("cgroup2_rmdir");
}
