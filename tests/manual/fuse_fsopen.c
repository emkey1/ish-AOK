// fuse_fsopen.c -- a FUSE filesystem mounted with the new mount API: fsopen,
// fsconfig (fd, rootmode, user_id, group_id, CREATE), fsmount, move_mount.
// fsmount returns a descriptor on the new mount without asking the
// filesystem anything, so it returns before the daemon serves -- daemons
// mount first and serve after. AOK opened the mount's root there, a request
// nobody would answer: fsmount hung. Then the daemon (a minimal one: INIT,
// GETATTR, LOOKUP) serves and the mount works. Root, or a user namespace with
// a mount namespace (`unshare -rm` on camd, Linux 6.12).
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/fuse.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef FSCONFIG_SET_STRING
#define FSCONFIG_SET_STRING 1
#define FSCONFIG_CMD_CREATE 6
#endif
#ifndef MOVE_MOUNT_F_EMPTY_PATH
#define MOVE_MOUNT_F_EMPTY_PATH 0x00000004
#endif

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void reply(int devfd, uint64_t unique, int error, const void *body, size_t len) {
    char buf[4096];
    struct fuse_out_header *out = (struct fuse_out_header *) buf;
    out->len = (uint32_t) (sizeof(*out) + (error ? 0 : len));
    out->error = error;
    out->unique = unique;
    if (!error && len)
        memcpy(buf + sizeof(*out), body, len);
    if (write(devfd, buf, out->len) < 0) {}
}

static void root_attr(struct fuse_attr *a) {
    memset(a, 0, sizeof(*a));
    a->ino = 1;
    a->mode = S_IFDIR | 0755;
    a->nlink = 2;
    a->uid = getuid();
    a->gid = getgid();
}

static void daemon_loop(int devfd) {
    char buf[1 << 17];
    for (;;) {
        ssize_t n = read(devfd, buf, sizeof buf);
        if (n <= 0)
            return;
        struct fuse_in_header *in = (struct fuse_in_header *) buf;
        if (in->opcode == FUSE_INIT) {
            struct fuse_init_out init = {.major = FUSE_KERNEL_VERSION, .minor = 31, .max_write = 65536};
            reply(devfd, in->unique, 0, &init, sizeof init);
        } else if (in->opcode == FUSE_GETATTR) {
            struct fuse_attr_out out = {.attr_valid = 1};
            root_attr(&out.attr);
            reply(devfd, in->unique, 0, &out, sizeof out);
        } else if (in->opcode == FUSE_LOOKUP) {
            reply(devfd, in->unique, -ENOENT, NULL, 0);
        } else if (in->opcode == FUSE_FORGET || in->opcode == FUSE_BATCH_FORGET) {
            // no reply
        } else if (in->opcode == FUSE_DESTROY) {
            reply(devfd, in->unique, 0, NULL, 0);
            return;
        } else {
            reply(devfd, in->unique, -ENOSYS, NULL, 0);
        }
    }
}

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(60);                          // a hang here must not hang the suite
    int devfd = open("/dev/fuse", O_RDWR);
    if (devfd < 0) {
        printf("fuse_fsopen: SKIP (no /dev/fuse: %s)\n", strerror(errno));
        return 0;
    }
    char base[] = "/tmp/fusefsopen.XXXXXX";
    if (mkdtemp(base) == NULL) {
        printf("FAIL mkdtemp: %s\n", strerror(errno));
        return 1;
    }
    char mnt[64];
    snprintf(mnt, sizeof mnt, "%s/mnt", base);
    mkdir(mnt, 0755);

    int fsfd = (int) syscall(SYS_fsopen, "fuse", 0);
    if (fsfd < 0) {
        printf("fuse_fsopen: SKIP (fsopen: %s)\n", strerror(errno));
        rmdir(mnt); rmdir(base);
        return 0;
    }
    char v[32];
    snprintf(v, sizeof v, "%d", devfd);
    int ok = syscall(SYS_fsconfig, fsfd, FSCONFIG_SET_STRING, "fd", v, 0) == 0 &&
             syscall(SYS_fsconfig, fsfd, FSCONFIG_SET_STRING, "rootmode", "40000", 0) == 0;
    snprintf(v, sizeof v, "%u", (unsigned) getuid());
    ok = ok && syscall(SYS_fsconfig, fsfd, FSCONFIG_SET_STRING, "user_id", v, 0) == 0;
    snprintf(v, sizeof v, "%u", (unsigned) getgid());
    ok = ok && syscall(SYS_fsconfig, fsfd, FSCONFIG_SET_STRING, "group_id", v, 0) == 0;
    if (!ok || syscall(SYS_fsconfig, fsfd, FSCONFIG_CMD_CREATE, NULL, NULL, 0) != 0) {
        if (errno == EPERM) {
            printf("fuse_fsopen: SKIP (not permitted: %s)\n", strerror(errno));
            rmdir(mnt); rmdir(base);
            return 0;
        }
        printf("FAIL fsconfig: %s\n", strerror(errno));
        return 1;
    }
    double t0 = now();
    int mfd = (int) syscall(SYS_fsmount, fsfd, 0, 0);
    double took = now() - t0;
    CHECK(mfd >= 0, "fsmount: %s", strerror(errno));
    CHECK(took < 2.0, "fsmount took %.1f s, with no daemon serving", took);
    if (mfd < 0)
        return 1;
    CHECK(syscall(SYS_move_mount, mfd, "", AT_FDCWD, mnt, MOVE_MOUNT_F_EMPTY_PATH) == 0,
          "move_mount: %s", strerror(errno));
    // (the fsmount descriptor pins the mount, as any open file on it does: the
    // daemon must not inherit it, or the umount below is EBUSY)
    close(mfd);
    close(fsfd);

    pid_t daemon = fork();
    if (daemon == 0) {
        daemon_loop(devfd);
        _exit(0);
    }
    struct stat st;
    int r = stat(mnt, &st);
    CHECK(r == 0 && S_ISDIR(st.st_mode) && st.st_ino == 1, "stat of the mounted root: %s, mode %#o ino %llu",
          r == 0 ? "ok" : strerror(errno), (unsigned) st.st_mode, (unsigned long long) st.st_ino);
    char probe[96];
    snprintf(probe, sizeof probe, "%s/nothing", mnt);
    errno = 0;
    CHECK(access(probe, F_OK) != 0 && errno == ENOENT, "lookup in the mount: errno %d (want ENOENT)", errno);

    CHECK(umount2(mnt, 0) == 0, "umount: %s", strerror(errno));
    kill(daemon, SIGTERM);
    waitpid(daemon, NULL, 0);
    rmdir(mnt);
    rmdir(base);
    printf("fuse_fsopen: %s (%d checks, %d failures)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures != 0;
}
