// mqueue_ops.c -- POSIX message queues: mq_open, mq_unlink, mq_timedsend,
// mq_timedreceive, mq_notify and mq_getsetattr, and the mqueue filesystem.
//
// AOK answered ENOSYS for all six. Every expectation below was measured on
// Linux 6.12 as uid 1000 (and the filesystem half inside unshare -Urm, where a
// user may mount one); it runs unchanged as root, except where noted.
//
// The calls are made through syscall(2), not libc: glibc and musl both strip
// the leading '/' from a name and translate errors, and what is being tested is
// the kernel's answer. The mq_* libc wrappers are exercised once at the end, as
// the program a user runs would.
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "test_common.h"

// The timed calls are handed this libc's struct timespec. On a 32-bit ABI the
// plain numbers take 32-bit seconds, so where time_t is 64 bits there (musl on
// i386) the _time64 numbers are the ones that match it. Some libcs spell the
// plain ones _time32.
#if !defined(SYS_mq_timedsend) && defined(SYS_mq_timedsend_time32)
#define SYS_mq_timedsend SYS_mq_timedsend_time32
#define SYS_mq_timedreceive SYS_mq_timedreceive_time32
#endif
#if defined(SYS_mq_timedsend_time64)
#define TIMED_NR(plain, wide) (sizeof(time_t) > sizeof(long) ? (wide) : (plain))
#else
#define TIMED_NR(plain, wide) (plain)
#define SYS_mq_timedsend_time64 0
#define SYS_mq_timedreceive_time64 0
#endif

// The kernel's struct mq_attr: longs, so its size follows the ABI.
struct kattr {
    long flags, maxmsg, msgsize, curmsgs;
    long reserved[4];
};

static char name[64];
static char other[64];

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-56s got=%-6ld want=%ld\n", label, got, want);
}

static long rv(long r) {
    return r < 0 ? -errno : r;
}

static long k_open(const char *n, int oflag, mode_t mode, struct kattr *attr) {
    return rv(syscall(SYS_mq_open, n, oflag, mode, attr));
}
static long k_unlink(const char *n) {
    return rv(syscall(SYS_mq_unlink, n));
}
static long k_send(int fd, const void *msg, size_t len, unsigned prio, const struct timespec *ts) {
    return rv(syscall(TIMED_NR(SYS_mq_timedsend, SYS_mq_timedsend_time64), fd, msg, len, prio, ts));
}
static long k_recv(int fd, void *msg, size_t len, unsigned *prio, const struct timespec *ts) {
    return rv(syscall(TIMED_NR(SYS_mq_timedreceive, SYS_mq_timedreceive_time64), fd, msg, len, prio, ts));
}
static long k_getsetattr(int fd, const struct kattr *nattr, struct kattr *oattr) {
    return rv(syscall(SYS_mq_getsetattr, fd, nattr, oattr));
}
static long k_notify(int fd, const struct sigevent *sev) {
    return rv(syscall(SYS_mq_notify, fd, sev));
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

static void check_open_rules(void) {
    struct kattr a = {0};
    long fd = k_open(name, O_RDWR | O_CREAT | O_EXCL, 0600, NULL);
    ck("create with default attributes", fd >= 0, 1);
    ck("getattr", k_getsetattr((int) fd, NULL, &a), 0);
    ck("default maxmsg is msg_default", a.maxmsg, read_long("/proc/sys/fs/mqueue/msg_default"));
    ck("default msgsize is msgsize_default", a.msgsize, read_long("/proc/sys/fs/mqueue/msgsize_default"));
    ck("no messages yet", a.curmsgs, 0);
    ck("flags", a.flags, 0);
    ck("O_EXCL on an existing queue", k_open(name, O_RDWR | O_CREAT | O_EXCL, 0600, NULL), -EEXIST);
    long fd2 = k_open(name, O_RDONLY, 0, NULL);
    ck("open existing", fd2 >= 0, 1);
    struct stat st;
    ck("fstat", rv(fstat((int) fd2, &st)), 0);
    ck("a queue is a regular file", S_ISREG(st.st_mode), 1);
    ck("with the mode it was made with", st.st_mode & 07777, 0600);
    ck("owned by its creator", st.st_uid, (long) geteuid());
    close((int) fd2);
    close((int) fd);

    ck("open missing without O_CREAT", k_open(other, O_RDWR, 0, NULL), -ENOENT);
    ck("a name with a slash", k_open("a/b", O_RDWR | O_CREAT, 0600, NULL), -EACCES);
    ck("an empty name", k_open("", O_RDWR | O_CREAT, 0600, NULL), -ENOENT);
    char longname[300];
    memset(longname, 'q', sizeof longname);
    longname[256] = '\0';
    ck("a 256-byte name", k_open(longname, O_RDWR | O_CREAT, 0600, NULL), -ENAMETOOLONG);
    ck("unlink missing", k_unlink(other), -ENOENT);

    long msg_max = read_long("/proc/sys/fs/mqueue/msg_max");
    long msgsize_max = read_long("/proc/sys/fs/mqueue/msgsize_max");
    struct kattr bad = {.maxmsg = 0, .msgsize = 16};
    ck("maxmsg 0", k_open(other, O_RDWR | O_CREAT, 0600, &bad), -EINVAL);
    bad = (struct kattr) {.maxmsg = 4, .msgsize = 0};
    ck("msgsize 0", k_open(other, O_RDWR | O_CREAT, 0600, &bad), -EINVAL);
    bad = (struct kattr) {.maxmsg = -1, .msgsize = 16};
    ck("negative maxmsg", k_open(other, O_RDWR | O_CREAT, 0600, &bad), -EINVAL);
    if (geteuid() != 0) {
        // Past the sysctl ceilings is for CAP_SYS_RESOURCE only.
        bad = (struct kattr) {.maxmsg = msg_max + 1, .msgsize = 16};
        ck("maxmsg past msg_max", k_open(other, O_RDWR | O_CREAT, 0600, &bad), -EINVAL);
        bad = (struct kattr) {.maxmsg = 4, .msgsize = msgsize_max + 1};
        ck("msgsize past msgsize_max", k_open(other, O_RDWR | O_CREAT, 0600, &bad), -EINVAL);
    }
    // The attributes are only looked at when the queue is being created.
    bad = (struct kattr) {.maxmsg = 0, .msgsize = 0};
    fd = k_open(name, O_RDWR | O_CREAT, 0600, &bad);
    ck("bad attributes on an existing queue are ignored", fd >= 0, 1);
    close((int) fd);
    ck("unlink", k_unlink(name), 0);
    ck("unlink twice", k_unlink(name), -ENOENT);

    // Permission: the mode is checked on open, as a file's is.
    fd = k_open(name, O_WRONLY | O_CREAT, 0200, NULL);
    ck("create write-only", fd >= 0, 1);
    close((int) fd);
    if (geteuid() != 0)
        ck("open for reading without read permission", k_open(name, O_RDONLY, 0, NULL), -EACCES);
    fd = k_open(name, O_WRONLY, 0, NULL);
    ck("open for writing with write permission", fd >= 0, 1);
    char c = 'x';
    ck("read from a write-only descriptor", k_recv((int) fd, &c, 8192, NULL, NULL), -EBADF);
    close((int) fd);
    k_unlink(name);
}

static void check_messages(void) {
    struct kattr a = {.maxmsg = 3, .msgsize = 16};
    long fd = k_open(name, O_RDWR | O_CREAT | O_EXCL, 0600, &a);
    ck("create 3 x 16", fd >= 0, 1);
    int q = (int) fd;
    char buf[64];
    unsigned prio = 99;

    ck("send too big", k_send(q, "0123456789abcdefX", 17, 0, NULL), -EMSGSIZE);
    ck("send priority 32768", k_send(q, "x", 1, 32768, NULL), -EINVAL);
    ck("send low", k_send(q, "low", 3, 1, NULL), 0);
    ck("send high", k_send(q, "high", 4, 7, NULL), 0);
    ck("send low again", k_send(q, "low2", 4, 1, NULL), 0);
    ck("getattr counts them", (k_getsetattr(q, NULL, &a), a.curmsgs), 3);
    // Full, and the descriptor is blocking: a deadline in the past is
    // ETIMEDOUT, and a malformed one is EINVAL because the call would block.
    struct timespec past = {0, 0};
    ck("send to a full queue, deadline passed", k_send(q, "x", 1, 0, &past), -ETIMEDOUT);
    struct timespec badts = {0, 1000000000};
    ck("send to a full queue, bad deadline", k_send(q, "x", 1, 0, &badts), -EINVAL);

    ck("receive into too small a buffer", k_recv(q, buf, 15, &prio, NULL), -EMSGSIZE);
    memset(buf, 0, sizeof buf);
    ck("highest priority first", k_recv(q, buf, sizeof buf, &prio, NULL), 4);
    ck("...its text", strcmp(buf, "high"), 0);
    ck("...its priority", prio, 7);
    memset(buf, 0, sizeof buf);
    ck("then first in, first out", k_recv(q, buf, sizeof buf, &prio, NULL), 3);
    ck("...low", strcmp(buf, "low"), 0);
    ck("...low's priority", prio, 1);
    memset(buf, 0, sizeof buf);
    ck("last", k_recv(q, buf, sizeof buf, NULL, NULL), 4);
    ck("...low2", strcmp(buf, "low2"), 0);
    ck("receive from empty, deadline passed", k_recv(q, buf, sizeof buf, NULL, &past), -ETIMEDOUT);
    ck("receive from empty, bad deadline", k_recv(q, buf, sizeof buf, NULL, &badts), -EINVAL);
    ck("an empty message", k_send(q, "", 0, 3, NULL), 0);
    ck("receive the empty message", k_recv(q, buf, sizeof buf, &prio, NULL), 0);

    // A real timed wait: roughly the time asked for, then ETIMEDOUT.
    struct timespec now, deadline;
    clock_gettime(CLOCK_REALTIME, &now);
    deadline = now;
    deadline.tv_nsec += 150 * 1000 * 1000;
    if (deadline.tv_nsec >= 1000000000) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000;
    }
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    ck("timed receive times out", k_recv(q, buf, sizeof buf, NULL, &deadline), -ETIMEDOUT);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    ck("...after about the time asked for", ms >= 120 && ms < 2000, 1);

    // O_NONBLOCK through mq_setattr: only that flag can change.
    struct kattr set = {.flags = O_NONBLOCK, .maxmsg = 99, .msgsize = 99};
    struct kattr old;
    ck("setattr O_NONBLOCK", k_getsetattr(q, &set, &old), 0);
    ck("...reports the old flags", old.flags, 0);
    ck("...getattr", (k_getsetattr(q, NULL, &a), a.flags), O_NONBLOCK);
    ck("...and nothing else changed", a.maxmsg, 3);
    ck("receive from empty, nonblocking", k_recv(q, buf, sizeof buf, NULL, NULL), -EAGAIN);
    for (int i = 0; i < 3; i++)
        k_send(q, "f", 1, 0, NULL);
    ck("send to full, nonblocking", k_send(q, "x", 1, 0, NULL), -EAGAIN);
    set.flags = 0;
    ck("setattr back to blocking", k_getsetattr(q, &set, NULL), 0);

    // poll: readable with messages, writable with room.
    struct pollfd p = {.fd = q, .events = POLLIN | POLLOUT};
    ck("poll a full queue", rv(poll(&p, 1, 0)), 1);
    // Plain POLLIN and POLLOUT, without the RDNORM/WRNORM twins (measured).
    ck("...readable, not writable", p.revents, POLLIN);
    while (k_recv(q, buf, sizeof buf, NULL, &past) >= 0)
        ;
    p.revents = 0;
    ck("poll an empty queue", rv(poll(&p, 1, 0)), 1);
    ck("...writable, not readable", p.revents, POLLOUT);

    // A blocked receiver is woken by a send from another process.
    fflush(NULL);
    pid_t child = fork();
    if (child == 0) {
        usleep(100 * 1000);
        long r = k_send(q, "wake", 4, 2, NULL);
        _exit(r == 0 ? 0 : 1);
    }
    memset(buf, 0, sizeof buf);
    ck("blocked receive is woken by a send", k_recv(q, buf, sizeof buf, &prio, NULL), 4);
    ck("...with that message", strcmp(buf, "wake"), 0);
    int st;
    waitpid(child, &st, 0);

    // read() on the descriptor is the queue's status line, as the mqueue
    // filesystem shows it.
    k_send(q, "12345", 5, 0, NULL);
    char line[128] = {0};
    ck("read the status line", rv(read(q, line, sizeof line - 1)) > 0, 1);
    unsigned long qsize = 99;
    int notify = 99, signo = 99, npid = 99;
    ck("...parses", sscanf(line, "QSIZE:%lu NOTIFY:%d SIGNO:%d NOTIFY_PID:%d", &qsize, &notify, &signo, &npid), 4);
    ck("...QSIZE is the bytes queued", qsize, 5);
    ck("...no notification", npid, 0);
    ck("write to the descriptor", rv(write(q, "x", 1)), -EINVAL);

    // Unlinked while open: the queue lives on for its descriptors.
    ck("unlink while open", k_unlink(name), 0);
    ck("still usable", k_recv(q, buf, sizeof buf, NULL, NULL), 5);
    ck("the name is free again", k_open(name, O_RDWR, 0, NULL), -ENOENT);
    close(q);
    ck("mq_send on a plain file", k_send(0, "x", 1, 0, NULL), -EBADF);
}

static volatile sig_atomic_t notified;
static volatile int notified_code, notified_value, notified_pid;
static void on_notify(int sig, siginfo_t *si, void *uc) {
    (void) sig;
    (void) uc;
    notified_code = si->si_code;
    notified_value = si->si_value.sival_int;
    notified_pid = si->si_pid;
    notified = 1;
}

static void check_notify(void) {
    long fd = k_open(name, O_RDWR | O_CREAT | O_EXCL, 0600, NULL);
    int q = (int) fd;
    struct sigaction sa = {.sa_sigaction = on_notify, .sa_flags = SA_SIGINFO};
    sigaction(SIGUSR2, &sa, NULL);
    struct sigevent sev = {.sigev_notify = SIGEV_SIGNAL, .sigev_signo = SIGUSR2};
    sev.sigev_value.sival_int = 4242;
    ck("notify: register", k_notify(q, &sev), 0);
    ck("notify: a second registration", k_notify(q, &sev), -EBUSY);
    char line[128] = {0};
    pread(q, line, sizeof line - 1, 0);
    int npid = 0, signo = 0;
    sscanf(line, "QSIZE:%*u NOTIFY:%*d SIGNO:%d NOTIFY_PID:%d", &signo, &npid);
    ck("notify: the status line names the signal", signo, SIGUSR2);
    ck("notify: ...and the process", npid, (long) getpid());

    // Another process may not register while this one is.
    fflush(NULL);
    pid_t child = fork();
    if (child == 0)
        _exit(k_notify(q, &sev) == -EBUSY ? 0 : 1);
    int st;
    waitpid(child, &st, 0);
    ck("notify: another process is refused", WIFEXITED(st) && WEXITSTATUS(st) == 0, 1);

    // A message arriving in the empty queue fires it, once.
    notified = 0;
    fflush(NULL);
    child = fork();
    if (child == 0)
        _exit(k_send(q, "n", 1, 0, NULL) == 0 ? 0 : 1);
    waitpid(child, &st, 0);
    for (int i = 0; i < 200 && !notified; i++)
        usleep(5000);
    ck("notify: fired", notified, 1);
    ck("notify: si_code SI_MESGQ", notified_code, SI_MESGQ);
    ck("notify: si_value", notified_value, 4242);
    ck("notify: si_pid is the sender", notified_pid, (long) child);
    ck("notify: one-shot -- registering again works", k_notify(q, &sev), 0);
    ck("notify: unregister", k_notify(q, NULL), 0);
    ck("notify: unregister again", k_notify(q, NULL), 0);
    // Not while the queue already holds a message.
    notified = 0;
    ck("notify: register on a non-empty queue", k_notify(q, &sev), 0);
    k_send(q, "m", 1, 0, NULL);
    usleep(50 * 1000);
    ck("notify: no signal for a queue that was not empty", notified, 0);
    k_notify(q, NULL);
    struct sigevent none = {.sigev_notify = SIGEV_NONE};
    ck("notify: SIGEV_NONE", k_notify(q, &none), 0);
    struct sigevent badsig = {.sigev_notify = SIGEV_SIGNAL, .sigev_signo = 999};
    k_notify(q, NULL);
    ck("notify: an invalid signal", k_notify(q, &badsig), -EINVAL);
    struct sigevent badkind = {.sigev_notify = 77};
    ck("notify: an invalid kind", k_notify(q, &badkind), -EINVAL);
    close(q);
    k_unlink(name);
    signal(SIGUSR2, SIG_DFL);
}

static volatile int thread_notified;
static void thread_fn(union sigval v) {
    thread_notified = v.sival_int;
}

// What a program actually calls: libc's wrappers, including SIGEV_THREAD,
// which both glibc and musl implement with a netlink socket the kernel sends a
// cookie to.
static void check_libc(void) {
    struct mq_attr attr = {.mq_maxmsg = 4, .mq_msgsize = 32};
    char lname[80];
    snprintf(lname, sizeof lname, "/%s", name);
    mqd_t q = mq_open(lname, O_RDWR | O_CREAT | O_EXCL, 0600, &attr);
    ck("libc: mq_open", q != (mqd_t) -1, 1);
    struct sigevent sev = {.sigev_notify = SIGEV_THREAD, .sigev_notify_function = thread_fn};
    sev.sigev_value.sival_int = 777;
    ck("libc: mq_notify SIGEV_THREAD", rv(mq_notify(q, &sev)), 0);
    ck("libc: mq_send", rv(mq_send(q, "hello", 5, 3)), 0);
    for (int i = 0; i < 200 && thread_notified == 0; i++)
        usleep(5000);
    ck("libc: the notification thread ran", thread_notified, 777);
    char buf[32];
    unsigned prio;
    ck("libc: mq_receive", rv(mq_receive(q, buf, sizeof buf, &prio)), 5);
    ck("libc: priority", prio, 3);
    struct mq_attr got;
    ck("libc: mq_getattr", rv(mq_getattr(q, &got)), 0);
    ck("libc: maxmsg", got.mq_maxmsg, 4);
    ck("libc: mq_close", rv(mq_close(q)), 0);
    ck("libc: mq_unlink", rv(mq_unlink(lname)), 0);
}

// The mqueue filesystem: the same queues, as files. Mounted by the test when
// it may; /dev/mqueue is where systemd and OpenRC put one.
static void check_filesystem(void) {
    char mnt[] = "/tmp/mqueue_ops.mnt.XXXXXX";
    if (mkdtemp(mnt) == NULL)
        return;
    if (mount("mqueue", mnt, "mqueue", 0, NULL) != 0) {
        test_logf("filesystem leg skipped: %s\n", strerror(errno));
        rmdir(mnt);
        return;
    }
    struct kattr a = {.maxmsg = 2, .msgsize = 8};
    long fd = k_open(name, O_RDWR | O_CREAT | O_EXCL, 0640, &a);
    k_send((int) fd, "abc", 3, 0, NULL);
    char path[160];
    snprintf(path, sizeof path, "%s/%s", mnt, name);
    struct stat st;
    ck("fs: the queue is a file", rv(stat(path, &st)), 0);
    ck("fs: ...regular", S_ISREG(st.st_mode), 1);
    ck("fs: ...its mode", st.st_mode & 07777, 0640);
    int f = open(path, O_RDONLY);
    char line[128] = {0};
    ck("fs: read its status line", rv(read(f, line, sizeof line - 1)) > 0, 1);
    unsigned long qsize = 0;
    sscanf(line, "QSIZE:%lu", &qsize);
    ck("fs: QSIZE", qsize, 3);
    close(f);
    // Listing.
    DIR *d = opendir(mnt);
    int found = 0;
    struct dirent *de;
    while (d != NULL && (de = readdir(d)) != NULL)
        if (strcmp(de->d_name, name) == 0)
            found = 1;
    if (d != NULL)
        closedir(d);
    ck("fs: listed", found, 1);
    ck("fs: unlink removes the queue", rv(unlink(path)), 0);
    ck("fs: ...mq_open no longer finds it", k_open(name, O_RDWR, 0, NULL), -ENOENT);
    close((int) fd);
    // Creating a file makes a queue.
    snprintf(path, sizeof path, "%s/%s", mnt, other);
    f = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
    ck("fs: creating a file makes a queue", f >= 0, 1);
    if (f >= 0)
        close(f);
    long q2 = k_open(other, O_RDWR, 0, NULL);
    ck("fs: ...which mq_open opens", q2 >= 0, 1);
    struct kattr got;
    k_getsetattr((int) q2, NULL, &got);
    ck("fs: ...with the default maxmsg", got.maxmsg, read_long("/proc/sys/fs/mqueue/msg_default"));
    close((int) q2);
    k_unlink(other);
    snprintf(path, sizeof path, "%s/sub", mnt);
    ck("fs: no directories", rv(mkdir(path, 0755)), -EPERM);
    ck("fs: umount", rv(umount(mnt)), 0);
    rmdir(mnt);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(30));
    snprintf(name, sizeof name, "mqueue_ops.%d", (int) getpid());
    snprintf(other, sizeof other, "mqueue_ops.%d.other", (int) getpid());
    k_unlink(name);
    k_unlink(other);
    check_open_rules();
    check_messages();
    check_notify();
    check_libc();
    check_filesystem();
    k_unlink(name);
    k_unlink(other);
    return finish_suite("mqueue_ops");
}
