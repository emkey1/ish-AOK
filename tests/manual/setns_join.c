// setns_join.c -- setns(2) joins the namespace a /proc/<pid>/ns/* fd names, or
// those of a pidfd's process, with Linux's errors.
//
// atop opens /proc/1/ns/uts and calls setns(fd, CLONE_NEWUTS) to read the
// host's name; AOK answered with an ENOSYS stub, and logged it on every start.
// UTS and IPC namespaces are real in AOK, so joining one must switch the
// caller to it -- including a namespace no process is in any more, which the
// fd alone keeps alive. Every other kind has only its initial namespace, and
// joining that is the no-op Linux makes of it, except that a mount namespace
// resets the root and cwd to its root, and the user namespace one is already
// in may not be entered again.
//
// Every privileged case runs in a child, so a namespace or root it changes
// cannot leak into the rest of the test. Run as root for the privileged half;
// unprivileged, it checks the errors only.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include "test_common.h"

#ifndef CLONE_NEWUTS
#define CLONE_NEWUTS 0x04000000
#endif
#ifndef CLONE_NEWIPC
#define CLONE_NEWIPC 0x08000000
#endif
#ifndef CLONE_NEWUSER
#define CLONE_NEWUSER 0x10000000
#endif
#ifndef CLONE_NEWNS
#define CLONE_NEWNS 0x00020000
#endif
#ifndef CLONE_NEWPID
#define CLONE_NEWPID 0x20000000
#endif
#ifndef CLONE_NEWNET
#define CLONE_NEWNET 0x40000000
#endif
#ifndef CLONE_NEWTIME
#define CLONE_NEWTIME 0x00000080
#endif
#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif
#ifndef SYS_setns
#error "no SYS_setns"
#endif

static int check(int cond, const char *fmt, ...) {
    if (cond)
        return 1;
    va_list ap;
    va_start(ap, fmt);
    printf("FAIL ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    return 0;
}

// Raw, so a libc without a setns() wrapper still builds it.
static int do_setns(int fd, int nstype) {
    return (int) syscall(SYS_setns, fd, nstype);
}

static int setns_errno(int fd, int nstype) {
    errno = 0;
    int r = do_setns(fd, nstype);
    return r == 0 ? 0 : errno;
}

static int hostname_is(const char *want) {
    char buf[256];
    if (gethostname(buf, sizeof(buf)) < 0)
        return 0;
    buf[sizeof(buf) - 1] = '\0';
    return strcmp(buf, want) == 0;
}

static int same_ns(const char *a, const char *b) {
    char la[64] = {0}, lb[64] = {0};
    if (readlink(a, la, sizeof(la) - 1) < 0 || readlink(b, lb, sizeof(lb) - 1) < 0)
        return 0;
    return strcmp(la, lb) == 0;
}

// The child's exit status is its failure count; -1 if it did not exit.
static int run_child(int (*fn)(void)) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        int failures = fn();
        fflush(stdout); // _exit would discard what the case printed
        _exit(failures);
    }
    int status;
    if (waitpid(pid, &status, 0) != pid)
        return -1;
    if (WIFSIGNALED(status))
        printf("FAIL case killed by signal %d\n", WTERMSIG(status));
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// ---- errors any caller gets ----

static int errors_any_caller(void) {
    int bad = 0;
    int uts = open("/proc/self/ns/uts", O_RDONLY | O_CLOEXEC);
    int user = open("/proc/self/ns/user", O_RDONLY | O_CLOEXEC);
    int p[2];
    if (!check(uts >= 0 && user >= 0 && pipe(p) == 0, "setup: open ns fds: %s", strerror(errno)))
        return 1;

    bad += !check(setns_errno(-1, 0) == EBADF, "setns(-1) errno %d, want EBADF", setns_errno(-1, 0));
    int e = setns_errno(p[0], 0);
    bad += !check(e == EINVAL, "setns(pipe, 0) errno %d, want EINVAL", e);
    e = setns_errno(p[0], CLONE_NEWUTS);
    bad += !check(e == EINVAL, "setns(pipe, NEWUTS) errno %d, want EINVAL", e);
    e = setns_errno(uts, CLONE_NEWIPC);
    bad += !check(e == EINVAL, "setns(uts fd, NEWIPC) errno %d, want EINVAL (type mismatch)", e);
    // The user namespace one is already in: EINVAL before any capability
    // check, so root and everyone else get the same answer.
    e = setns_errno(user, 0);
    bad += !check(e == EINVAL, "setns(own user ns) errno %d, want EINVAL", e);

    int pidfd = (int) syscall(SYS_pidfd_open, getpid(), 0);
    if (pidfd >= 0) {
        e = setns_errno(pidfd, 0);
        bad += !check(e == EINVAL, "setns(pidfd, 0) errno %d, want EINVAL", e);
        e = setns_errno(pidfd, CLONE_NEWUTS | 0x100 /* CLONE_VM */);
        bad += !check(e == EINVAL, "setns(pidfd, NEWUTS|CLONE_VM) errno %d, want EINVAL", e);
        close(pidfd);
    }
    close(p[0]);
    close(p[1]);
    close(uts);
    close(user);
    return bad;
}

// ---- an unprivileged caller may not join ----

static int unprivileged_eperm(void) {
    int bad = 0;
    int uts = open("/proc/self/ns/uts", O_RDONLY | O_CLOEXEC);
    int ipc = open("/proc/self/ns/ipc", O_RDONLY | O_CLOEXEC);
    int mnt = open("/proc/self/ns/mnt", O_RDONLY | O_CLOEXEC);
    int user = open("/proc/self/ns/user", O_RDONLY | O_CLOEXEC);
    if (!check(uts >= 0 && ipc >= 0 && mnt >= 0 && user >= 0, "setup: open ns fds: %s", strerror(errno)))
        return 1;
    if (geteuid() == 0) {
        if (!check(setgid(65534) == 0 && setuid(65534) == 0, "setup: drop to 65534: %s", strerror(errno)))
            return 1;
    }
    int e = setns_errno(uts, CLONE_NEWUTS);
    bad += !check(e == EPERM, "unprivileged setns(uts) errno %d, want EPERM", e);
    e = setns_errno(ipc, 0);
    bad += !check(e == EPERM, "unprivileged setns(ipc) errno %d, want EPERM", e);
    e = setns_errno(mnt, 0);
    bad += !check(e == EPERM, "unprivileged setns(mnt) errno %d, want EPERM", e);
    e = setns_errno(user, 0);
    bad += !check(e == EINVAL, "unprivileged setns(own user ns) errno %d, want EINVAL", e);
    int pidfd = (int) syscall(SYS_pidfd_open, getpid(), 0);
    if (pidfd >= 0) {
        e = setns_errno(pidfd, CLONE_NEWUTS);
        bad += !check(e == EPERM, "unprivileged setns(pidfd, NEWUTS) errno %d, want EPERM", e);
        close(pidfd);
    }
    return bad;
}

// ---- root: UTS ----

// atop's call: pid 1's UTS namespace, by an fd from /proc/1/ns/uts.
static int uts_join_pid1(void) {
    int bad = 0;
    int fd = open("/proc/1/ns/uts", O_RDONLY | O_CLOEXEC);
    if (!check(fd >= 0, "open /proc/1/ns/uts: %s", strerror(errno)))
        return 1;
    if (!check(unshare(CLONE_NEWUTS) == 0, "unshare(NEWUTS): %s", strerror(errno)))
        return 1;
    bad += !check(sethostname("setns-inner", 11) == 0, "sethostname: %s", strerror(errno));
    bad += !check(!same_ns("/proc/self/ns/uts", "/proc/1/ns/uts"), "unshare left the caller in pid 1's uts ns");
    bad += !check(do_setns(fd, CLONE_NEWUTS) == 0, "setns(/proc/1/ns/uts, NEWUTS): %s", strerror(errno));
    bad += !check(same_ns("/proc/self/ns/uts", "/proc/1/ns/uts"), "after setns, not in pid 1's uts ns");
    bad += !check(!hostname_is("setns-inner"), "after setns, still sees the private hostname");
    return bad;
}

// A namespace nobody is in any more is still joinable through an fd on it.
static int uts_kept_by_fd(void) {
    int bad = 0;
    int ready[2], go[2];
    if (!check(pipe(ready) == 0 && pipe(go) == 0, "pipe: %s", strerror(errno)))
        return 1;
    pid_t pid = fork();
    if (pid == 0) {
        char c = 0;
        if (unshare(CLONE_NEWUTS) != 0 || sethostname("setns-kept", 10) != 0)
            c = 1;
        if (write(ready[1], &c, 1) != 1 || read(go[0], &c, 1) < 0)
            _exit(1);
        _exit(0);
    }
    char c = 1;
    if (!check(pid > 0 && read(ready[0], &c, 1) == 1 && c == 0, "child could not unshare/sethostname"))
        return 1;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/ns/uts", (int) pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    bad += !check(fd >= 0, "open %s: %s", path, strerror(errno));
    if (write(go[1], &c, 1) != 1)
        bad++;
    int status;
    waitpid(pid, &status, 0);
    if (fd < 0)
        return bad;
    // The only process in that namespace has been reaped.
    bad += !check(do_setns(fd, 0) == 0, "setns(fd of a reaped process's uts ns): %s", strerror(errno));
    bad += !check(hostname_is("setns-kept"), "joined namespace lost its hostname");
    close(fd);
    return bad;
}

// ---- root: IPC ----

static int ipc_join_back(void) {
    int bad = 0;
    int orig = open("/proc/self/ns/ipc", O_RDONLY | O_CLOEXEC);
    if (!check(orig >= 0, "open /proc/self/ns/ipc: %s", strerror(errno)))
        return 1;
    key_t key = (key_t) (0x5e750000 | (getpid() & 0xffff));
    int q = msgget(key, IPC_CREAT | IPC_EXCL | 0600);
    if (q < 0 && errno == EEXIST) {
        // Left behind by a run that died: remove it and start clean.
        msgctl(msgget(key, 0), IPC_RMID, NULL);
        q = msgget(key, IPC_CREAT | IPC_EXCL | 0600);
    }
    if (!check(q >= 0, "msgget(IPC_CREAT): %s", strerror(errno)))
        return 1;
    if (!check(unshare(CLONE_NEWIPC) == 0, "unshare(NEWIPC): %s", strerror(errno))) {
        msgctl(q, IPC_RMID, NULL);
        return 1;
    }
    errno = 0;
    bad += !check(msgget(key, 0) < 0 && errno == ENOENT, "queue visible in a new IPC ns");
    bad += !check(do_setns(orig, CLONE_NEWIPC) == 0, "setns(orig ipc): %s", strerror(errno));
    int again = msgget(key, 0);
    bad += !check(again == q, "after setns back, msgget gave %d, want %d", again, q);
    msgctl(q, IPC_RMID, NULL);
    return bad;
}

// ---- root: pidfd ----

static int pidfd_joins_all_or_nothing(void) {
    int bad = 0;
    int ready[2], go[2];
    if (!check(pipe(ready) == 0 && pipe(go) == 0, "pipe: %s", strerror(errno)))
        return 1;
    pid_t pid = fork();
    if (pid == 0) {
        char c = 0;
        if (unshare(CLONE_NEWUTS) != 0 || sethostname("setns-pidfd", 11) != 0)
            c = 1;
        if (write(ready[1], &c, 1) != 1 || read(go[0], &c, 1) < 0)
            _exit(1);
        _exit(0);
    }
    char c = 1;
    if (!check(pid > 0 && read(ready[0], &c, 1) == 1 && c == 0, "child could not unshare/sethostname"))
        return 1;
    int pidfd = (int) syscall(SYS_pidfd_open, pid, 0);
    if (pidfd < 0) {
        printf("  (pidfd_open: %s -- pidfd cases skipped)\n", strerror(errno));
    } else {
        char before[256] = {0};
        gethostname(before, sizeof(before) - 1);
        // The user namespace is the caller's own: EINVAL, and the UTS half
        // listed with it must not have happened either.
        int e = setns_errno(pidfd, CLONE_NEWUTS | CLONE_NEWUSER);
        bad += !check(e == EINVAL, "setns(pidfd, NEWUTS|NEWUSER) errno %d, want EINVAL", e);
        bad += !check(hostname_is(before), "a failed pidfd setns changed the UTS namespace anyway");
        bad += !check(do_setns(pidfd, CLONE_NEWUTS | CLONE_NEWIPC) == 0,
                      "setns(pidfd, NEWUTS|NEWIPC): %s", strerror(errno));
        bad += !check(hostname_is("setns-pidfd"), "pidfd setns did not join the child's UTS namespace");
    }
    if (write(go[1], &c, 1) != 1)
        bad++;
    int status;
    waitpid(pid, &status, 0);
    if (pidfd >= 0) {
        // Reaped: nothing left to join.
        int e = setns_errno(pidfd, CLONE_NEWUTS);
        bad += !check(e == ESRCH, "setns(pidfd of a reaped process) errno %d, want ESRCH", e);
        close(pidfd);
    }
    return bad;
}

// ---- root: the kinds AOK has one of ----

static int single_kinds_are_noops(void) {
    int bad = 0;
    static const struct { const char *name; int type; } kinds[] = {
        {"pid", CLONE_NEWPID}, {"net", CLONE_NEWNET}, {"cgroup", 0x02000000},
    };
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
        char path[64];
        snprintf(path, sizeof(path), "/proc/self/ns/%s", kinds[i].name);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            continue; // a kernel built without it
        bad += !check(do_setns(fd, kinds[i].type) == 0, "setns(own %s ns): %s", kinds[i].name, strerror(errno));
        close(fd);
    }
    return bad;
}

// Joining a mount namespace moves the root and cwd to its root: out of a
// chroot, and out of the directory the caller was in.
static int mnt_resets_root_and_cwd(void) {
    int bad = 0;
    int mnt = open("/proc/self/ns/mnt", O_RDONLY | O_CLOEXEC);
    if (!check(mnt >= 0, "open /proc/self/ns/mnt: %s", strerror(errno)))
        return 1;
    char dir[64];
    snprintf(dir, sizeof(dir), "/tmp/setns-join-%d", (int) getpid());
    mkdir(dir, 0755);
    if (!check(chdir(dir) == 0 && chroot(".") == 0, "chroot %s: %s", dir, strerror(errno)))
        return 1;
    bad += !check(access("/proc/self", F_OK) != 0, "chroot did not hide /proc (control)");
    bad += !check(do_setns(mnt, CLONE_NEWNS) == 0, "setns(own mnt ns): %s", strerror(errno));
    bad += !check(access("/proc/self", F_OK) == 0, "setns(mnt) left the root inside the chroot");
    char cwd[256] = {0};
    bad += !check(getcwd(cwd, sizeof(cwd)) != NULL && strcmp(cwd, "/") == 0,
                  "setns(mnt) left cwd at \"%s\", want \"/\"", cwd);
    rmdir(dir);
    return bad;
}

static int block_on(void *arg) {
    char c;
    return (int) read(*(int *) arg, &c, 1);
}

// Sharing the fs_struct (CLONE_FS) makes a mount namespace join EINVAL.
static int mnt_shared_fs_einval(void) {
    int bad = 0;
    int mnt = open("/proc/self/ns/mnt", O_RDONLY | O_CLOEXEC);
    int p[2];
    if (!check(mnt >= 0 && pipe(p) == 0, "setup: %s", strerror(errno)))
        return 1;
    static char stack[64 * 1024] __attribute__((aligned(16)));
    pid_t pid = clone(block_on, stack + sizeof(stack), CLONE_FS | SIGCHLD, &p[0]);
    if (!check(pid > 0, "clone(CLONE_FS): %s", strerror(errno)))
        return 1;
    int e = setns_errno(mnt, CLONE_NEWNS);
    bad += !check(e == EINVAL, "setns(mnt) with a shared fs_struct errno %d, want EINVAL", e);
    char c = 0;
    if (write(p[1], &c, 1) != 1)
        bad++;
    waitpid(pid, NULL, 0);
    return bad;
}

static void *thread_block(void *arg) {
    char c;
    if (read(*(int *) arg, &c, 1) < 0)
        return NULL;
    return NULL;
}

// A time namespace may be joined only by a single-threaded process: EUSERS.
static int time_multithreaded_eusers(void) {
    int fd = open("/proc/self/ns/time", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0; // a kernel without time namespaces
    int bad = 0;
    int p[2];
    pthread_t t;
    if (!check(pipe(p) == 0 && pthread_create(&t, NULL, thread_block, &p[0]) == 0, "setup: thread"))
        return 1;
    int e = setns_errno(fd, CLONE_NEWTIME);
    bad += !check(e == EUSERS, "multithreaded setns(time) errno %d, want EUSERS", e);
    char c = 0;
    if (write(p[1], &c, 1) != 1)
        bad++;
    pthread_join(t, NULL);
    // pthread_join returns when the thread clears its tid, which is before it
    // has left the thread group, so the kernel may still count it briefly.
    for (int i = 0; i < 200 && (e = setns_errno(fd, CLONE_NEWTIME)) == EUSERS; i++)
        usleep(10000);
    bad += !check(e == 0, "single-threaded setns(own time ns) errno %d, want 0", e);
    return bad;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    setvbuf(stdout, NULL, _IOLBF, 0); // a watchdog kill still shows the last case
    struct { const char *name; int (*fn)(void); int root; } cases[] = {
        {"errors_any_caller", errors_any_caller, 0},
        {"unprivileged_eperm", unprivileged_eperm, 0},
        {"uts_join_pid1", uts_join_pid1, 1},
        {"uts_kept_by_fd", uts_kept_by_fd, 1},
        {"ipc_join_back", ipc_join_back, 1},
        {"pidfd_joins_all_or_nothing", pidfd_joins_all_or_nothing, 1},
        {"single_kinds_are_noops", single_kinds_are_noops, 1},
        {"mnt_resets_root_and_cwd", mnt_resets_root_and_cwd, 1},
        {"mnt_shared_fs_einval", mnt_shared_fs_einval, 1},
        {"time_multithreaded_eusers", time_multithreaded_eusers, 1},
    };
    int root = geteuid() == 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (cases[i].root && !root) {
            test_logf("  %s: skipped, not root\n", cases[i].name);
            continue;
        }
        int r = run_child(cases[i].fn);
        test_log_if(r != 0, "  %s: %s\n", cases[i].name, r == 0 ? "ok" : "FAILED");
        if (r != 0)
            failures_total++;
    }
    if (!root)
        printf("setns_join: (not root: errors only)\n");
    return finish_suite("setns_join");
}
