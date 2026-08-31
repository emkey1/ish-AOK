// What the per-mount flags MEAN, and lazy unmount.
//
//   ro, nosuid, nodev and noexec belong to the MOUNT, not to the filesystem
//   underneath it. Two binds of the same directory can differ -- that is the
//   entire purpose of `mount -o remount,bind,ro`, and it is how a container
//   runtime hands a process a read-only view of a directory it still writes
//   through elsewhere.
//
//   AOK collapsed a bind to its origin before answering any question about it,
//   so every one of these was decided by the origin's flags instead of the
//   bind's: a read-only bind was writable, a noexec bind ran programs, and a
//   nosuid one granted setuid. `mount -o remount,bind,ro` did not even get
//   that far -- it was routed to the code that CREATES a bind, from a source
//   the caller never passed, and failed with ENOENT while the mount stayed
//   writable.
//
//   nosuid and nodev were worse than unimplemented: MS_SUPPORTED accepted them
//   and /proc/mounts printed them, so a caller that mounted untrusted media
//   nosuid, read the mount table back, and saw "nosuid" had been told
//   something untrue about a decision it had no way to re-check.
//
//   umount2(MNT_DETACH) was refused with EBUSY whenever the mount was in use,
//   which is precisely when it is called: it exists to take a busy mount out
//   of the namespace now and tear it down when the last user leaves. Shutdown
//   paths that unmount in dependency order rely on it not failing.
//
// Measured against x86_64 glibc on Linux 6.12.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_common.h"

#ifndef MS_BIND
#define MS_BIND (1 << 12)
#endif
#ifndef MNT_DETACH
#define MNT_DETACH 2
#endif

#define AS_UID 1000
#define AS_GID 1000

static char base[80], src[160], bind_pt[160];

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-58s got=%-8ld want=%ld\n", label, got, want);
}

// The errno from a call expected to fail, or 0 if it did not.
static int err_of(long r) { return r < 0 ? errno : 0; }

static void p(char *out, size_t n, const char *dir, const char *rel) {
    snprintf(out, n, "%s/%s", dir, rel);
}

static long try_create(const char *dir, const char *name) {
    char f[224];
    p(f, sizeof f, dir, name);
    errno = 0;
    int fd = open(f, O_WRONLY | O_CREAT, 0644);
    if (fd >= 0) {
        close(fd);
        unlink(f);
        return 0;
    }
    return -1;
}

// execve the staged copy of this binary, as AS_UID, and report how it ended.
// It answers --child-euid with 42 when it holds root and 7 when it does not,
// so one fixture serves both the noexec and the nosuid question.
#define RAN_AS_ROOT 42
#define RAN_UNPRIVILEGED 7
static int run_helper(const char *path, int *exec_errno_out) {
    int pipefd[2];
    if (pipe(pipefd) != 0)
        return -1;
    fflush(NULL);
    pid_t c = fork();
    if (c == 0) {
        close(pipefd[0]);
        alarm(30);
        if (setgroups(0, NULL) != 0 || setgid(AS_GID) != 0 || setuid(AS_UID) != 0)
            _exit(90);
        char *const av[] = { (char *) "helper", (char *) "--child-euid", NULL };
        char *const ev[] = { NULL };
        errno = 0;
        execve(path, av, ev);
        int e = errno;
        ssize_t ignored = write(pipefd[1], &e, sizeof e);
        (void) ignored;
        _exit(91);
    }
    close(pipefd[1]);
    int e = 0;
    ssize_t got = read(pipefd[0], &e, sizeof e);
    close(pipefd[0]);
    int st;
    waitpid(c, &st, 0);
    if (exec_errno_out != NULL)
        *exec_errno_out = got == (ssize_t) sizeof e ? e : 0;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

// Where this binary lives, so it can be copied and made setuid. Staging a
// fixture this way needs no compiler in the guest and no assumption about
// which programs the root happens to ship.
static int self_path(char *out, size_t n, const char *argv0) {
    ssize_t r = readlink("/proc/self/exe", out, n - 1);
    if (r > 0) {
        out[r] = '\0';
        return 0;
    }
    if (argv0 != NULL && strchr(argv0, '/') != NULL) {
        snprintf(out, n, "%s", argv0);
        return 0;
    }
    return -1;
}

int main(int argc, char **argv) {
    // Answered before test_init, which rejects options it does not know.
    if (argc >= 2 && strcmp(argv[1], "--child-euid") == 0)
        return geteuid() == 0 ? RAN_AS_ROOT : RAN_UNPRIVILEGED;

    test_init(argc, argv);
    alarm(test_watchdog_secs(120));

    if (geteuid() != 0) {
        printf("mount_flag_perms: SKIP (needs root to mount)\n");
        return 0;
    }

    snprintf(base, sizeof base, "/tmp/mntperm-%d", (int) getpid());
    char cmd[256];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", base);
    if (system(cmd) < 0)
        failures_total++;
    ck("mkdir base", mkdir(base, 0755), 0);
    p(src, sizeof src, base, "src");
    p(bind_pt, sizeof bind_pt, base, "bind");
    ck("  mkdir the source", mkdir(src, 0755), 0);
    ck("  mkdir the bind point", mkdir(bind_pt, 0755), 0);

    // A setuid-root copy of this binary inside the source directory.
    char self[256], helper_src[224], helper_bind[224];
    p(helper_src, sizeof helper_src, src, "helper");
    p(helper_bind, sizeof helper_bind, bind_pt, "helper");
    int have_helper = 0;
    if (self_path(self, sizeof self, argv[0]) == 0) {
        snprintf(cmd, sizeof cmd, "cp '%s' '%s' 2>/dev/null", self, helper_src);
        if (system(cmd) == 0 && chown(helper_src, 0, 0) == 0 &&
                chmod(helper_src, 04755) == 0)
            have_helper = 1;
    }
    if (!have_helper)
        printf("mount_flag_perms: NOTE could not stage the setuid helper; "
               "skipping the nosuid and noexec sections\n");

    // The source tree has to be reachable by an unprivileged exec.
    snprintf(cmd, sizeof cmd, "chmod a+rx '%s' '%s'", base, src);
    if (system(cmd) < 0)
        failures_total++;

    // ---- can we mount at all? -------------------------------------------
    errno = 0;
    if (mount(src, bind_pt, NULL, MS_BIND, NULL) != 0) {
        int e = errno;
        if (e == EPERM || e == ENOSYS) {
            printf("mount_flag_perms: SKIP (mount(2) refused: %s)\n", strerror(e));
            snprintf(cmd, sizeof cmd, "rm -rf '%s'", base);
            if (system(cmd) < 0) { }
            return 0;
        }
        failf("bind mount", (uint64_t) -1, (uint64_t) e, 0, 0, 0, 0);
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", base);
        if (system(cmd) < 0) { }
        return finish_suite("mount_flag_perms");
    }

    // ---- MS_RDONLY on the bind, and only on the bind --------------------
    ck("remount the bind read-only",
       mount(NULL, bind_pt, NULL, MS_REMOUNT | MS_BIND | MS_RDONLY, NULL), 0);
    ck("  creating through it is EROFS", err_of(try_create(bind_pt, "new")), EROFS);
    {
        char f[224];
        p(f, sizeof f, bind_pt, "d");
        ck("  mkdir through it is EROFS", err_of(mkdir(f, 0755)), EROFS);
        p(f, sizeof f, bind_pt, "l");
        ck("  symlink through it is EROFS", err_of(symlink("x", f)), EROFS);
        ck("  chmod through it is EROFS", err_of(chmod(helper_bind, 0700)), EROFS);
        ck("  unlink through it is EROFS", err_of(unlink(helper_bind)), EROFS);
        // Reading is untouched: a read-only mount is readable, and a fix that
        // refused everything would pass every check above.
        errno = 0;
        int fd = open(bind_pt, O_RDONLY | O_DIRECTORY);
        ck("  opening it read-only still works", fd >= 0 ? 1 : 0, 1);
        if (fd >= 0)
            close(fd);
    }
    // The origin is a different mount and keeps its own flags.
    ck("the ORIGIN is still writable", try_create(src, "new"), 0);
    // ...and the restriction lifts again.
    ck("remount the bind read-write",
       mount(NULL, bind_pt, NULL, MS_REMOUNT | MS_BIND, NULL), 0);
    ck("  creating through it works again", try_create(bind_pt, "new"), 0);

    // ---- MS_NOEXEC and MS_NOSUID ----------------------------------------
    if (have_helper) {
        ck("remount the bind noexec",
           mount(NULL, bind_pt, NULL, MS_REMOUNT | MS_BIND | MS_NOEXEC, NULL), 0);
        int e = 0;
        run_helper(helper_bind, &e);
        ck("  exec through it is EACCES", e, EACCES);
        run_helper(helper_src, &e);
        ck("  exec through the ORIGIN still runs", e, 0);

        ck("remount the bind nosuid (and not noexec)",
           mount(NULL, bind_pt, NULL, MS_REMOUNT | MS_BIND | MS_NOSUID, NULL), 0);
        // It still runs -- Linux drops the set-id bits rather than refusing
        // the exec -- but it runs unprivileged.
        ck("  a setuid-root binary runs WITHOUT privilege",
           run_helper(helper_bind, NULL), RAN_UNPRIVILEGED);
        ck("  the same binary through the ORIGIN gets it",
           run_helper(helper_src, NULL), RAN_AS_ROOT);
        ck("remount the bind with no restrictions",
           mount(NULL, bind_pt, NULL, MS_REMOUNT | MS_BIND, NULL), 0);
    }

    // ---- MS_NODEV --------------------------------------------------------
    {
        char dev_src[224], dev_bind[224];
        p(dev_src, sizeof dev_src, src, "zero");
        p(dev_bind, sizeof dev_bind, bind_pt, "zero");
        // /dev/zero's numbers: a device that exists everywhere this runs.
        if (mknod(dev_src, S_IFCHR | 0666, makedev(1, 5)) == 0) {
            ck("remount the bind nodev",
               mount(NULL, bind_pt, NULL, MS_REMOUNT | MS_BIND | MS_NODEV, NULL), 0);
            errno = 0;
            int fd = open(dev_bind, O_RDONLY);
            ck("  opening a device through it is EACCES", fd < 0 ? errno : 0, EACCES);
            if (fd >= 0)
                close(fd);
            errno = 0;
            fd = open(dev_src, O_RDONLY);
            ck("  opening it through the ORIGIN works", fd >= 0 ? 1 : 0, 1);
            if (fd >= 0)
                close(fd);
            ck("remount the bind without nodev",
               mount(NULL, bind_pt, NULL, MS_REMOUNT | MS_BIND, NULL), 0);
        } else {
            printf("mount_flag_perms: NOTE mknod refused (%s); skipping nodev\n",
                   strerror(errno));
        }
    }
    ck("unmount the bind", umount2(bind_pt, MNT_DETACH), 0);

    // ---- a read-only mount that is not a bind ----------------------------
    {
        char ro[160];
        p(ro, sizeof ro, base, "ro");
        ck("mkdir a mount point", mkdir(ro, 0755), 0);
        ck("mount a tmpfs read-only", mount("none", ro, "tmpfs", MS_RDONLY, NULL), 0);
        ck("  creating on it is EROFS", err_of(try_create(ro, "x")), EROFS);
        ck("  remount it read-write", mount(NULL, ro, NULL, MS_REMOUNT, NULL), 0);
        ck("  creating on it works", try_create(ro, "x"), 0);
        ck("  unmount it", umount2(ro, MNT_DETACH), 0);
    }

    // ---- umount2(MNT_DETACH) on a busy mount -----------------------------
    {
        char busy[160];
        p(busy, sizeof busy, base, "busy");
        ck("mkdir a mount point", mkdir(busy, 0755), 0);
        ck("mount a tmpfs on it", mount("none", busy, "tmpfs", 0, NULL), 0);
        int held = open(busy, O_RDONLY | O_DIRECTORY);
        ck("  hold it open", held >= 0 ? 1 : 0, 1);
        ck("  a plain umount is EBUSY", err_of(umount(busy)), EBUSY);
        // The whole point: this one succeeds anyway.
        ck("  umount2(MNT_DETACH) succeeds", umount2(busy, MNT_DETACH), 0);
        // ...and it is detached NOW, not when the reference goes: nothing is
        // mounted there any more, so a second unmount has nothing to find.
        ck("  and it is gone immediately", err_of(umount2(busy, MNT_DETACH)), EINVAL);
        if (held >= 0)
            close(held);
    }

    // Defensive: nothing below should still be mounted, but a failure above
    // must not leave mounts behind for the rest of the suite.
    umount2(bind_pt, MNT_DETACH);
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", base);
    if (system(cmd) < 0)
        failures_total++;
    return finish_suite("mount_flag_perms");
}
