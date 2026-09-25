// ipc_namespace.c -- IPC namespaces (unshare and clone with CLONE_NEWIPC), and
// the rest of unshare(2)'s flag rules.
//
// unshare(CLONE_NEWIPC) answered ENOSYS, and unshare of the thread group,
// signal handlers or address space answered ENOSYS too, where Linux succeeds
// when there is nothing to split and refuses with EINVAL when there is. Every
// expectation below was measured on Linux 6.12: the flag rules as uid 1000,
// the namespace half as root of a user namespace (unshare -Ur).
//
// The namespace half needs CAP_SYS_ADMIN, so it runs when the test is root
// (as AOK's CLI is); the refusals are checked from a child that gives root up.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <mqueue.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include "test_common.h"

#ifndef CLONE_NEWIPC
#define CLONE_NEWIPC 0x08000000
#endif
#define CLONE_PIDFD_ 0x00001000

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-60s got=%-6ld want=%ld\n", label, got, want);
}

// Runs fn in a forked child and returns its exit status (or 200 + signal).
static int in_child(int (*fn)(long), long arg) {
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0)
        _exit(fn(arg));
    int st = 0;
    if (waitpid(pid, &st, 0) != pid)
        return 199;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 200 + WTERMSIG(st);
}

// Child bodies return 0 for success or the errno, so in_child's answer reads
// like the call's.
static int do_unshare(long flags) {
    return unshare((int) flags) == 0 ? 0 : errno;
}

static void *parked(void *arg) {
    (void) arg;
    for (;;)
        pause();
    return NULL;
}
static int unshare_with_a_thread(long flags) {
    pthread_t t;
    if (pthread_create(&t, NULL, parked, NULL) != 0)
        return 98;
    return unshare((int) flags) == 0 ? 0 : errno;
}

static char vm_stack[64 * 1024] __attribute__((aligned(16)));
static int vm_sibling_body(void *arg) {
    (void) arg;
    for (;;)
        pause();
    return 0;
}
// With a CLONE_VM child that is not a thread: the address space is shared
// with another process, the signal handlers are not.
static int unshare_with_a_vm_sibling(long flags) {
    pid_t sib = clone(vm_sibling_body, vm_stack + sizeof vm_stack, CLONE_VM | SIGCHLD, NULL);
    if (sib < 0)
        return 98;
    int r = unshare((int) flags) == 0 ? 0 : errno;
    kill(sib, SIGKILL);
    waitpid(sib, NULL, 0);
    return r;
}

static int clone_with(long flags) {
    long r = syscall(SYS_clone, flags | SIGCHLD, 0L, 0L, 0L, 0L);
    if (r == 0)
        _exit(0);
    if (r < 0)
        return errno;
    waitpid((pid_t) r, NULL, 0);
    return 0;
}

// SEM_UNDO +1, then unshare(flags): the semaphore's value after.
static int undo_then_unshare(long flags) {
    int id = semget(IPC_PRIVATE, 1, 0600);
    if (id < 0)
        return 97;
    struct sembuf op = {0, 1, SEM_UNDO};
    if (semop(id, &op, 1) != 0)
        return 96;
    int r = unshare((int) flags);
    int val = semctl(id, 0, GETVAL);
    semctl(id, 0, IPC_RMID);
    return r != 0 ? 95 : val;
}

static void check_flag_rules(void) {
    test_logf("unshare flag rules\n");
    ck("unshare(0)", in_child(do_unshare, 0), 0);
    ck("an undefined flag is EINVAL", in_child(do_unshare, 0x1), EINVAL);
    // Not namespaces, and not unshare's: malformed, not missing.
    ck("CLONE_IO is EINVAL", in_child(do_unshare, CLONE_IO), EINVAL);
    ck("CLONE_PIDFD is EINVAL", in_child(do_unshare, CLONE_PIDFD_), EINVAL);
    ck("CLONE_PTRACE is EINVAL", in_child(do_unshare, CLONE_PTRACE), EINVAL);

    // Nothing to split off a single-threaded process: success.
    ck("CLONE_VM, single-threaded", in_child(do_unshare, CLONE_VM), 0);
    ck("CLONE_SIGHAND, single-threaded", in_child(do_unshare, CLONE_SIGHAND), 0);
    ck("CLONE_THREAD, single-threaded", in_child(do_unshare, CLONE_THREAD), 0);
    // Something to split: EINVAL, and before the privilege check.
    ck("CLONE_THREAD with a second thread", in_child(unshare_with_a_thread, CLONE_THREAD), EINVAL);
    ck("CLONE_SIGHAND with a second thread", in_child(unshare_with_a_thread, CLONE_SIGHAND), EINVAL);
    ck("CLONE_VM with a second thread", in_child(unshare_with_a_thread, CLONE_VM), EINVAL);
    ck("CLONE_NEWIPC|CLONE_THREAD with a second thread",
       in_child(unshare_with_a_thread, CLONE_NEWIPC | CLONE_THREAD), EINVAL);
    ck("CLONE_FILES with a second thread", in_child(unshare_with_a_thread, CLONE_FILES), 0);
    ck("CLONE_VM with a CLONE_VM sibling process", in_child(unshare_with_a_vm_sibling, CLONE_VM), EINVAL);
    ck("CLONE_SIGHAND with a CLONE_VM sibling process",
       in_child(unshare_with_a_vm_sibling, CLONE_SIGHAND), 0);

    // Leaving the SEM_UNDO list is what exit does to it: applied now.
    ck("CLONE_SYSVSEM applies SEM_UNDO", in_child(undo_then_unshare, CLONE_SYSVSEM), 0);
    ck("unshare(0) does not", in_child(undo_then_unshare, 0), 1);
}

// The refusals, from a process without CAP_SYS_ADMIN. Root becomes nobody,
// which takes every capability with it; root of a user namespace that maps no
// other uid (unshare -Ur) cannot, and gives the capabilities up with capset(2)
// instead. (Not capset alone: AOK counts euid 0 as holding every capability.)
static int unprivileged_body(long unused) {
    (void) unused;
    if (geteuid() == 0 &&
            (setgroups(0, NULL) != 0 || setgid(65534) != 0 || setuid(65534) != 0)) {
        struct {
            uint32_t version;
            int pid;
        } hdr = {0x20080522, 0};
        uint32_t data[6] = {0};
        if (syscall(SYS_capset, &hdr, data) != 0)
            return 90;
    }
    int r;
    if ((r = do_unshare(CLONE_NEWIPC)) != EPERM)
        return 10 + r;
    if ((r = clone_with(CLONE_NEWIPC)) != EPERM)
        return 30 + r;
    // The privilege check comes first, and the pair's EINVAL after it.
    if ((r = clone_with(CLONE_NEWIPC | CLONE_SYSVSEM)) != EPERM)
        return 50 + r;
    if ((r = unshare_with_a_thread(CLONE_NEWIPC)) != EPERM)
        return 70 + r;
    return 0;
}

static unsigned long ipc_ino(void) {
    struct stat st;
    if (stat("/proc/self/ns/ipc", &st) != 0)
        return 0;
    return (unsigned long) st.st_ino;
}

static int count_lines(const char *path) {
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return -1;
    int n = 0;
    char line[512];
    while (fgets(line, sizeof line, f) != NULL)
        n++;
    fclose(f);
    return n;
}

static long read_long(const char *path) {
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return -1;
    long v = -1;
    if (fscanf(f, "%ld", &v) != 1)
        v = -1;
    fclose(f);
    return v;
}

static int write_str(const char *path, const char *s) {
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return errno;
    ssize_t n = write(fd, s, strlen(s));
    int e = n < 0 ? errno : 0;
    close(fd);
    return e;
}

static char qname[64];
#define SEM_KEY 0x1a2b3c4d
// Made by the parent in its own namespace, so it can remove them after.
static int old_shm = -1, old_sem = -1;

// A new namespace sees none of the old one's objects, and gets its own
// identity; what was already open stays usable.
static int new_namespace_view(long unused) {
    (void) unused;
    mqd_t q = mq_open(qname, O_RDWR);
    if (old_shm < 0 || old_sem < 0 || q == (mqd_t) -1)
        return 1;
    unsigned long before = ipc_ino();
    if (unshare(CLONE_NEWIPC) != 0)
        return 2;
    unsigned long after = ipc_ino();
    int bad = 0;
    if (after == 0 || after == before)
        bad |= 1 << 2;
    char link[64] = {0}, want[64];
    snprintf(want, sizeof want, "ipc:[%lu]", after);
    if (readlink("/proc/self/ns/ipc", link, sizeof link - 1) < 0 || strcmp(link, want) != 0)
        bad |= 1 << 3;
    struct shmid_ds ds;
    if (!(shmctl(old_shm, IPC_STAT, &ds) < 0 && errno == EINVAL))
        bad |= 1 << 4;
    if (!(semget(SEM_KEY, 1, 0600) < 0 && errno == ENOENT))
        bad |= 1 << 5;
    if (!(mq_open(qname, O_RDWR) == (mqd_t) -1 && errno == ENOENT))
        bad |= 1 << 6;
    // The descriptor opened in the old namespace still reaches its queue.
    char buf[8192];
    unsigned prio;
    if (mq_send(q, "x", 1, 0) != 0 || mq_receive(q, buf, sizeof buf, &prio) != 1)
        bad |= 1 << 7;
    // /proc/sysvipc lists this namespace's objects: none, only the header.
    if (count_lines("/proc/sysvipc/sem") != 1)
        bad |= 1 << 8;
    // The limits start at Linux's defaults, and are this namespace's own.
    if (read_long("/proc/sys/fs/mqueue/queues_max") != 256)
        bad |= 1 << 9;
    if (write_str("/proc/sys/fs/mqueue/msg_max", "5") != 0 ||
            read_long("/proc/sys/fs/mqueue/msg_max") != 5)
        bad |= 1 << 10;
    return bad;
}

// SEM_UNDO in the old namespace is applied at the unshare, as the parent
// sees; objects made in the new namespace go with it.
static void check_undo_and_teardown(void) {
    int id = semget(IPC_PRIVATE, 1, 0600);
    int to_parent[2], to_child[2];
    if (id < 0 || pipe(to_parent) != 0 || pipe(to_child) != 0) {
        ck("undo: setup", -1, 0);
        return;
    }
    fflush(NULL);
    pid_t c = fork();
    if (c == 0) {
        char b = 0;
        struct sembuf op = {0, 1, SEM_UNDO};
        semop(id, &op, 1);
        write(to_parent[1], "a", 1);
        read(to_child[0], &b, 1);
        b = unshare(CLONE_NEWIPC) == 0 ? 'u' : 'f';
        write(to_parent[1], &b, 1);
        read(to_child[0], &b, 1);
        // Objects of every kind, left for the namespace's teardown.
        shmget(IPC_PRIVATE, 4096, IPC_CREAT | 0600);
        semget(IPC_PRIVATE, 2, IPC_CREAT | 0600);
        msgget(IPC_PRIVATE, IPC_CREAT | 0600);
        mq_open(qname, O_CREAT | O_RDWR, 0600, NULL);
        _exit(0);
    }
    char b = 0;
    read(to_parent[0], &b, 1);
    ck("undo: the child's +1 is visible", semctl(id, 0, GETVAL), 1);
    write(to_child[1], "x", 1);
    read(to_parent[0], &b, 1);
    ck("undo: the child's unshare(CLONE_NEWIPC) succeeded", b, 'u');
    ck("undo: ...and applied its SEM_UNDO here", semctl(id, 0, GETVAL), 0);
    write(to_child[1], "x", 1);
    int st = 0;
    waitpid(c, &st, 0);
    ck("teardown: the child exited cleanly", WIFEXITED(st) && WEXITSTATUS(st) == 0, 1);
    ck("teardown: its queue was never ours", mq_open(qname, O_RDWR) == (mqd_t) -1 && errno == ENOENT, 1);
    semctl(id, 0, IPC_RMID);
    close(to_parent[0]);
    close(to_parent[1]);
    close(to_child[0]);
    close(to_child[1]);
}

// A queue outlives its namespace for as long as it is open: a child in a new
// namespace hands its descriptor over and exits, the namespace goes, and the
// queue still works.
static void check_queue_outlives_namespace(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        ck("outlive: socketpair", -1, 0);
        return;
    }
    fflush(NULL);
    pid_t c = fork();
    if (c == 0) {
        if (unshare(CLONE_NEWIPC) != 0)
            _exit(1);
        struct mq_attr attr = {.mq_maxmsg = 2, .mq_msgsize = 16};
        mqd_t q = mq_open(qname, O_CREAT | O_RDWR, 0600, &attr);
        if (q == (mqd_t) -1 || mq_send(q, "kept", 4, 1) != 0)
            _exit(2);
        char dummy = 'q';
        struct iovec iov = {&dummy, 1};
        char cbuf[CMSG_SPACE(sizeof(int))];
        struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1,
                             .msg_control = cbuf, .msg_controllen = sizeof cbuf};
        struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
        cm->cmsg_level = SOL_SOCKET;
        cm->cmsg_type = SCM_RIGHTS;
        cm->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cm), &q, sizeof(int));
        _exit(sendmsg(sv[1], &msg, 0) == 1 ? 0 : 3);
    }
    int st = 0;
    waitpid(c, &st, 0);
    ck("outlive: the child made and sent its queue", WIFEXITED(st) ? WEXITSTATUS(st) : -1, 0);
    char dummy;
    struct iovec iov = {&dummy, 1};
    char cbuf[CMSG_SPACE(sizeof(int))];
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1,
                         .msg_control = cbuf, .msg_controllen = sizeof cbuf};
    int fd = -1;
    if (recvmsg(sv[0], &msg, MSG_DONTWAIT) == 1) {
        struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
        if (cm != NULL && cm->cmsg_type == SCM_RIGHTS)
            memcpy(&fd, CMSG_DATA(cm), sizeof(int));
    }
    ck("outlive: received the descriptor", fd >= 0, 1);
    if (fd >= 0) {
        char buf[16] = {0};
        unsigned prio = 0;
        ck("outlive: its message is still there", mq_receive((mqd_t) fd, buf, sizeof buf, &prio), 4);
        ck("outlive: ...intact", strcmp(buf, "kept"), 0);
        ck("outlive: and the queue still takes more", mq_send((mqd_t) fd, "more", 4, 0), 0);
        close(fd);
    }
    ck("outlive: its name was never ours", mq_open(qname, O_RDWR) == (mqd_t) -1 && errno == ENOENT, 1);
    close(sv[0]);
    close(sv[1]);
}

// A segment still attached when its namespace goes stays mapped, and its last
// detach frees it -- along with what was left of the namespace, which must not
// be touched after. The caller leaves its first namespace for a second while
// attached, which takes the first one's last reference.
static int segment_outlives_namespace(long unused) {
    (void) unused;
    if (unshare(CLONE_NEWIPC) != 0)
        return 1;
    int id = shmget(IPC_PRIVATE, 4096, IPC_CREAT | 0600);
    if (id < 0)
        return 2;
    char *p = shmat(id, NULL, 0);
    if (p == (void *) -1)
        return 3;
    strcpy(p, "still here");
    if (unshare(CLONE_NEWIPC) != 0)
        return 4;
    if (strcmp(p, "still here") != 0)
        return 5;
    p[0] = 'S';
    if (shmdt(p) != 0)
        return 6;
    // Nor does the new namespace know the segment.
    struct shmid_ds ds;
    if (!(shmctl(id, IPC_STAT, &ds) < 0 && errno == EINVAL))
        return 7;
    return 0;
}

static int clone_child_has_new_namespace(long parent_ino) {
    long r = syscall(SYS_clone, (long) (CLONE_NEWIPC | SIGCHLD), 0L, 0L, 0L, 0L);
    if (r == 0)
        _exit(ipc_ino() != (unsigned long) parent_ino && ipc_ino() != 0 ? 0 : 1);
    if (r < 0)
        return 100 + errno;
    int st = 0;
    waitpid((pid_t) r, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 200;
}

static void check_privileged(void) {
    test_logf("CLONE_NEWIPC\n");
    ck("unshare(CLONE_NEWIPC)", in_child(do_unshare, CLONE_NEWIPC), 0);
    ck("unshare(CLONE_NEWUTS|CLONE_NEWIPC)", in_child(do_unshare, CLONE_NEWUTS | CLONE_NEWIPC), 0);
    ck("unshare(CLONE_NEWIPC) with a second thread", in_child(unshare_with_a_thread, CLONE_NEWIPC), 0);
    ck("clone(CLONE_NEWIPC)", in_child(clone_with, CLONE_NEWIPC), 0);
    // CLONE_SYSVSEM shares an undo list the new namespace cannot reach.
    ck("clone(CLONE_NEWIPC|CLONE_SYSVSEM)", in_child(clone_with, CLONE_NEWIPC | CLONE_SYSVSEM), EINVAL);
    ck("clone(CLONE_NEWIPC) child is in a new namespace",
       in_child(clone_child_has_new_namespace, (long) ipc_ino()), 0);
    old_shm = shmget(IPC_PRIVATE, 4096, 0600);
    old_sem = semget(SEM_KEY, 1, IPC_CREAT | 0600);
    mqd_t q = mq_open(qname, O_CREAT | O_RDWR, 0600, NULL);
    // Bits of what differed, from the child: 0 is all as expected.
    ck("the new namespace's view", in_child(new_namespace_view, 0), 0);
    ck("...and ours is as it was", semget(SEM_KEY, 1, 0600), old_sem);
    shmctl(old_shm, IPC_RMID, NULL);
    semctl(old_sem, 0, IPC_RMID);
    if (q != (mqd_t) -1)
        mq_close(q);
    mq_unlink(qname);
    check_undo_and_teardown();
    check_queue_outlives_namespace();
    ck("a segment attached when its namespace goes stays usable, then detaches",
       in_child(segment_outlives_namespace, 0), 0);
    ck("this namespace's msg_max is untouched", read_long("/proc/sys/fs/mqueue/msg_max"), 10);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(30));
    snprintf(qname, sizeof qname, "/ipc_namespace.%d", (int) getpid());
    mq_unlink(qname);

    check_flag_rules();
    test_logf("without CAP_SYS_ADMIN\n");
    ck("CLONE_NEWIPC is refused (EPERM), by unshare and clone", in_child(unprivileged_body, 0), 0);
    if (geteuid() == 0)
        check_privileged();
    else
        test_logf("CLONE_NEWIPC success cases skipped: not root\n");
    mq_unlink(qname);
    return finish_suite("ipc_namespace");
}
