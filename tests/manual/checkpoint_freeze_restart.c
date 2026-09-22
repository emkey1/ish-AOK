// checkpoint_freeze_restart.c -- a checkpoint freeze must be invisible to a
// task blocked in a syscall. Driven by checkpoint_freeze_restart.sh, which
// takes the checkpoint while every case below is blocked.
//
// The freezer wakes a blocked task, its wait comes back EINTR, and the
// dispatcher is meant to turn that into a restart: the PC is rewound and the
// call re-executes after the thaw. On x86_64 most syscalls bypassed that
// conversion, so a timed recv, a write to a full socket, readv and nanosleep
// all failed with EINTR at the moment of the checkpoint.
//
// Each case is a child blocked in ONE raw syscall (syscall(2), so the number
// under test is the one named here whatever the libc wrapper would pick). A
// case passes when:
//   - it returned what it would have without a freeze (errno and result), and
//   - it did not return early: never before its own timeout or before the
//     helper that ends it acts, and
//   - a sleep or a poll-family wait did not return LATE either: its timeout is
//     a deadline, which the freeze must carry across the restart rather than
//     start again (task->sleep_restart_deadline, poll_restart_deadline). It
//     returns by that deadline, or at the thaw if the freeze outlasted it, and
//   - the freeze really happened inside the call. `saves` in
//     /proc/ish/checkpoint went from N to N+1 across the call, and the call
//     spans the longest pause a spinning witness process saw -- the freeze
//     itself. A case that the freeze missed is INCONCLUSIVE, not a pass.
//
//     checkpoint_freeze_restart            expect a checkpoint from outside
//     checkpoint_freeze_restart save PATH  take it ourselves, through
//                                          /proc/ish/checkpoint, once every
//                                          case is blocked
//     checkpoint_freeze_restart none       control run: no checkpoint expected
//
// Exit status: 0 all pass, 1 any FAIL, 2 any INCONCLUSIVE (and no FAIL).
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/file.h>
#include <sys/ipc.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <time.h>
#include <unistd.h>

#define SOCK_TIMEOUT_MS 4000L   // SO_RCVTIMEO / SO_SNDTIMEO
#define WAIT_MS         6000L   // sleeps, poll-family timeouts, helper deadlines
#define EARLY_SLACK_MS  20L     // clock granularity, not a real early return
// How late a carried deadline may be met. A wait that started its timeout
// again is late by the whole time it had already waited when the freeze came,
// which the save legs make at least a second.
#define LATE_SLACK_MS   500L

// Not every root carries linux-headers; these are ABI constants.
#define AF_NETLINK_     16
#define NETLINK_ROUTE_  0
#define FUTEX_WAIT_     0
struct sockaddr_nl_ { unsigned short family, pad; uint32_t pid, groups; };
struct sembuf_ { unsigned short num; short op; short flags; };
struct msgbuf_ { long mtype; char mtext[1]; };

enum { WANT_ANY = -1000000 };   // want_rc: any non-negative result

struct result {
    int index;
    char name[24];
    long rc;
    int err;
    int want_err;
    long want_rc;
    long t0, t1;          // monotonic microseconds around the call
    long not_before;      // the call must not return before this (us)
    int bounded;          // ...nor long after it, or after the thaw (us)
    long saves_before, saves_after;
};

static long now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000L + ts.tv_nsec / 1000L;
}

// Sleep to an absolute monotonic deadline, whatever interrupts it. Helpers use
// this, so a helper's own interrupted sleep can never end a case early.
static void sleep_until_us(long deadline) {
    for (;;) {
        long left = deadline - now_us();
        if (left <= 0)
            return;
        struct timespec ts = {left / 1000000L, (left % 1000000L) * 1000L};
        nanosleep(&ts, NULL);
    }
}

static long read_saves(void) {
    static char buf[1 << 16];
    int fd = open("/proc/ish/checkpoint", O_RDONLY);
    if (fd < 0)
        return -1;
    size_t len = 0;
    ssize_t n;
    while (len < sizeof(buf) - 1 &&
           (n = read(fd, buf + len, sizeof(buf) - 1 - len)) != 0) {
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        len += (size_t) n;
    }
    close(fd);
    buf[len] = '\0';
    char *p = strstr(buf, "\nsaves");
    return p == NULL ? -1 : strtol(p + 6, NULL, 10);
}

static int ready_fd = -1, result_fd = -1;
static int case_index;
static long deadline_us;   // shared by every helper-driven case

static void write_all(int fd, const void *p, size_t n) {
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            _exit(3);
        p = (const char *) p + w;
        n -= (size_t) w;
    }
}

// Everything a case sets up happens before this; the call follows at once.
static void begin(struct result *r, const char *name, int want_err, long want_rc) {
    memset(r, 0, sizeof(*r));
    r->index = case_index;
    snprintf(r->name, sizeof(r->name), "%s", name);
    r->want_err = want_err;
    r->want_rc = want_rc;
    r->saves_before = read_saves();
    char token = 1;
    write_all(ready_fd, &token, 1);
    r->t0 = now_us();
}

static void finish(struct result *r, long rc, int err, long not_before) {
    r->t1 = now_us();
    r->rc = rc;
    r->err = rc < 0 ? err : 0;
    r->not_before = not_before;
    r->saves_after = read_saves();
    write_all(result_fd, r, sizeof(*r));
}

static void on_usr1(int sig) { (void) sig; }

// ---- fixtures ---------------------------------------------------------------

static void set_timeout(int fd, int opt, long ms) {
    struct timeval tv = {ms / 1000, (ms % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, opt, &tv, sizeof(tv));
}

static int timed_udp(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET};
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(fd, (struct sockaddr *) &a, sizeof(a));
    set_timeout(fd, SO_RCVTIMEO, SOCK_TIMEOUT_MS);
    return fd;
}

static int timed_netlink(void) {
    int fd = socket(AF_NETLINK_, SOCK_RAW, NETLINK_ROUTE_);
    struct sockaddr_nl_ a = {.family = AF_NETLINK_};
    bind(fd, (struct sockaddr *) &a, sizeof(a));
    set_timeout(fd, SO_RCVTIMEO, SOCK_TIMEOUT_MS);
    return fd;
}

// A unix stream socket whose send buffer is full, with a send timeout.
static int full_unix_socket(void) {
    static int sv[2];   // the peer stays open for the child's lifetime
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    int fl = fcntl(sv[0], F_GETFL);
    fcntl(sv[0], F_SETFL, fl | O_NONBLOCK);
    static char chunk[4096];
    while (write(sv[0], chunk, sizeof(chunk)) > 0)
        ;
    while (write(sv[0], chunk, 1) == 1)
        ;
    fcntl(sv[0], F_SETFL, fl);
    set_timeout(sv[0], SO_SNDTIMEO, SOCK_TIMEOUT_MS);
    return sv[0];
}

static int timed_listener(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET};
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(fd, (struct sockaddr *) &a, sizeof(a));
    listen(fd, 1);
    set_timeout(fd, SO_RCVTIMEO, SOCK_TIMEOUT_MS);
    return fd;
}

// A child that runs `act` at the shared deadline and exits.
static pid_t helper_at_deadline(void (*act)(void *), void *arg) {
    pid_t pid = fork();
    if (pid == 0) {
        sleep_until_us(deadline_us);
        act(arg);
        _exit(0);
    }
    return pid;
}

static void act_nothing(void *arg) { (void) arg; }
static void act_write_byte(void *arg) { write_all(*(int *) arg, "x", 1); }
// The case's pid, taken before the fork -- not getppid(). A case that returned
// early has exited by the deadline, its helper now belongs to init, and
// getppid() would signal the probe itself.
static void act_signal_case(void *arg) { kill(*(pid_t *) arg, SIGUSR1); }
static void act_sem_post(void *arg) {
    struct sembuf_ op = {0, 1, 0};
    syscall(SYS_semop, *(int *) arg, &op, 1);
}
static void act_msg_send(void *arg) {
    struct msgbuf_ m = {1, {'x'}};
    syscall(SYS_msgsnd, *(int *) arg, &m, 1, 0);
}

// Hold a lock of the given kind on `path` until the deadline. Returns once the
// lock is held, so the caller's blocking attempt really does block.
static pid_t lock_holder(const char *path, int use_flock) {
    int sync[2];
    pipe(sync);
    pid_t pid = fork();
    if (pid == 0) {
        int fd = open(path, O_RDWR | O_CREAT, 0600);
        if (use_flock) {
            flock(fd, LOCK_EX);
        } else {
            struct flock fl = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
            fcntl(fd, F_SETLK, &fl);
        }
        write_all(sync[1], "x", 1);
        sleep_until_us(deadline_us);
        _exit(0);
    }
    char c;
    while (read(sync[0], &c, 1) < 0 && errno == EINTR)
        ;
    return pid;
}

// ---- the cases ----------------------------------------------------------------

#define TIMED(name, want_err, want_rc, ms, call) do {                           \
        struct result r;                                                       \
        begin(&r, name, want_err, want_rc);                                    \
        long rc = (call);                                                      \
        int e = errno;                                                         \
        finish(&r, rc, e, r.t0 + (ms) * 1000L);                                \
    } while (0)

// A wait whose timeout is a deadline the freeze has to keep: see the header.
#define TIMED_BOUNDED(name, want_err, want_rc, ms, call) do {                   \
        struct result r;                                                       \
        begin(&r, name, want_err, want_rc);                                    \
        r.bounded = 1;                                                         \
        long rc = (call);                                                      \
        int e = errno;                                                         \
        finish(&r, rc, e, r.t0 + (ms) * 1000L);                                \
    } while (0)

#define BY_DEADLINE(name, want_err, want_rc, call) do {                        \
        struct result r;                                                       \
        begin(&r, name, want_err, want_rc);                                    \
        long rc = (call);                                                      \
        int e = errno;                                                         \
        finish(&r, rc, e, deadline_us);                                        \
    } while (0)

static void run_case(int which) {
    char buf[4096];
    struct iovec iov = {buf, sizeof(buf)};
    struct msghdr mh = {.msg_iov = &iov, .msg_iovlen = 1};
    struct timespec wait_ts = {WAIT_MS / 1000, 0};
    switch (which) {
    // -- socket receive, SO_RCVTIMEO: EAGAIN, and not before the timeout
    case 0: { int fd = timed_udp();
        TIMED("read-udp", EAGAIN, 0, SOCK_TIMEOUT_MS, syscall(SYS_read, fd, buf, sizeof(buf))); break; }
    case 1: { int fd = timed_udp();
        TIMED("readv-udp", EAGAIN, 0, SOCK_TIMEOUT_MS, syscall(SYS_readv, fd, &iov, 1)); break; }
    case 2: { int fd = timed_udp();
        TIMED("recvfrom-udp", EAGAIN, 0, SOCK_TIMEOUT_MS,
              syscall(SYS_recvfrom, fd, buf, sizeof(buf), 0, NULL, NULL)); break; }
    case 3: { int fd = timed_udp();
        TIMED("recvmsg-udp", EAGAIN, 0, SOCK_TIMEOUT_MS, syscall(SYS_recvmsg, fd, &mh, 0)); break; }
    case 4: { int fd = timed_netlink();
        TIMED("recvfrom-netlink", EAGAIN, 0, SOCK_TIMEOUT_MS,
              syscall(SYS_recvfrom, fd, buf, sizeof(buf), 0, NULL, NULL)); break; }
    case 5: { int fd = timed_listener();
        TIMED("accept-tcp", EAGAIN, 0, SOCK_TIMEOUT_MS, syscall(SYS_accept, fd, NULL, NULL)); break; }
    // -- socket send into a full buffer, SO_SNDTIMEO
    case 6: { int fd = full_unix_socket();
        TIMED("write-unix", EAGAIN, 0, SOCK_TIMEOUT_MS, syscall(SYS_write, fd, buf, sizeof(buf))); break; }
    case 7: { int fd = full_unix_socket();
        TIMED("writev-unix", EAGAIN, 0, SOCK_TIMEOUT_MS, syscall(SYS_writev, fd, &iov, 1)); break; }
    case 8: { int fd = full_unix_socket();
        TIMED("sendto-unix", EAGAIN, 0, SOCK_TIMEOUT_MS,
              syscall(SYS_sendto, fd, buf, sizeof(buf), 0, NULL, 0)); break; }
    case 9: { int fd = full_unix_socket();
        TIMED("sendmsg-unix", EAGAIN, 0, SOCK_TIMEOUT_MS, syscall(SYS_sendmsg, fd, &mh, 0)); break; }
    // -- sleeps and timed waits: they time out normally, and not early
    case 10:
        TIMED_BOUNDED("nanosleep", 0, 0, WAIT_MS, syscall(SYS_nanosleep, &wait_ts, NULL)); break;
    case 11:
        TIMED_BOUNDED("clock_nanosleep", 0, 0, WAIT_MS,
              syscall(SYS_clock_nanosleep, CLOCK_MONOTONIC, 0, &wait_ts, NULL)); break;
#ifdef SYS_select
    case 12: { struct timeval tv = {WAIT_MS / 1000, 0};
        TIMED_BOUNDED("select", 0, 0, WAIT_MS, syscall(SYS_select, 0, NULL, NULL, NULL, &tv)); break; }
#endif
    case 13:
        TIMED_BOUNDED("pselect6", 0, 0, WAIT_MS, syscall(SYS_pselect6, 0, NULL, NULL, NULL, &wait_ts, NULL)); break;
#ifdef SYS_poll
    case 14:
        TIMED_BOUNDED("poll", 0, 0, WAIT_MS, syscall(SYS_poll, NULL, 0, (int) WAIT_MS)); break;
#endif
    case 15:
        TIMED_BOUNDED("ppoll", 0, 0, WAIT_MS, syscall(SYS_ppoll, NULL, 0, &wait_ts, NULL, 8)); break;
#ifdef SYS_epoll_wait
    case 16: { int ep = epoll_create1(0); struct epoll_event ev;
        TIMED_BOUNDED("epoll_wait", 0, 0, WAIT_MS, syscall(SYS_epoll_wait, ep, &ev, 1, (int) WAIT_MS)); break; }
#endif
    case 17: { int ep = epoll_create1(0); struct epoll_event ev;
        TIMED_BOUNDED("epoll_pwait", 0, 0, WAIT_MS,
              syscall(SYS_epoll_pwait, ep, &ev, 1, (int) WAIT_MS, NULL, 8)); break; }
    case 18: { sigset_t set; sigemptyset(&set); sigaddset(&set, SIGUSR2);
        sigprocmask(SIG_BLOCK, &set, NULL);
        TIMED("rt_sigtimedwait", EAGAIN, 0, WAIT_MS,
              syscall(SYS_rt_sigtimedwait, &set, NULL, &wait_ts, 8)); break; }
    case 19: { int v = 0;
        TIMED("futex_wait", ETIMEDOUT, 0, WAIT_MS,
              syscall(SYS_futex, &v, FUTEX_WAIT_, 0, &wait_ts, NULL, 0)); break; }
    case 20: { int id = (int) syscall(SYS_semget, IPC_PRIVATE, 1, 0600); struct sembuf_ op = {0, -1, 0};
        TIMED("semtimedop", EAGAIN, 0, WAIT_MS, syscall(SYS_semtimedop, id, &op, 1, &wait_ts));
        syscall(SYS_semctl, id, 0, IPC_RMID, 0); break; }
    // -- ended by a helper at the shared deadline
    case 21: { pid_t h = helper_at_deadline(act_nothing, NULL); int st;
        BY_DEADLINE("wait4", 0, h, syscall(SYS_wait4, h, &st, 0, NULL)); break; }
    case 22: { pid_t h = helper_at_deadline(act_nothing, NULL); siginfo_t si;
        BY_DEADLINE("waitid", 0, 0, syscall(SYS_waitid, P_PID, h, &si, WEXITED, NULL)); break; }
    case 23: { struct sigaction sa = {.sa_handler = on_usr1}; sigaction(SIGUSR1, &sa, NULL);
        sigset_t set; sigemptyset(&set); sigaddset(&set, SIGUSR1); sigprocmask(SIG_BLOCK, &set, NULL);
        pid_t self = getpid(); helper_at_deadline(act_signal_case, &self);
        sigset_t none; sigemptyset(&none);
        BY_DEADLINE("rt_sigsuspend", EINTR, 0, syscall(SYS_rt_sigsuspend, &none, 8)); break; }
#ifdef SYS_pause
    case 24: { struct sigaction sa = {.sa_handler = on_usr1}; sigaction(SIGUSR1, &sa, NULL);
        pid_t self = getpid(); helper_at_deadline(act_signal_case, &self);
        BY_DEADLINE("pause", EINTR, 0, syscall(SYS_pause)); break; }
#endif
    case 25: { int p[2]; pipe(p); helper_at_deadline(act_write_byte, &p[1]);
        BY_DEADLINE("read-pipe", 0, 1, syscall(SYS_read, p[0], buf, sizeof(buf))); break; }
    case 26: { int p[2]; pipe(p); helper_at_deadline(act_write_byte, &p[1]);
        BY_DEADLINE("readv-pipe", 0, 1, syscall(SYS_readv, p[0], &iov, 1)); break; }
    case 27: { int id = (int) syscall(SYS_semget, IPC_PRIVATE, 1, 0600); struct sembuf_ op = {0, -1, 0};
        helper_at_deadline(act_sem_post, &id);
        BY_DEADLINE("semop", 0, 0, syscall(SYS_semop, id, &op, 1));
        syscall(SYS_semctl, id, 0, IPC_RMID, 0); break; }
    case 28: { int id = (int) syscall(SYS_msgget, IPC_PRIVATE, 0600); struct msgbuf_ m;
        helper_at_deadline(act_msg_send, &id);
        BY_DEADLINE("msgrcv", 0, 1, syscall(SYS_msgrcv, id, &m, 1, 0, 0));
        syscall(SYS_msgctl, id, IPC_RMID, NULL); break; }
    case 29: { char path[64]; snprintf(path, sizeof(path), "/tmp/freeze-lock.%d", (int) getpid());
        lock_holder(path, 0); int fd = open(path, O_RDWR);
        struct flock fl = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
        BY_DEADLINE("fcntl_setlkw", 0, 0, syscall(SYS_fcntl, fd, F_SETLKW, &fl));
        unlink(path); break; }
    case 30: { char path[64]; snprintf(path, sizeof(path), "/tmp/freeze-flock.%d", (int) getpid());
        lock_holder(path, 1); int fd = open(path, O_RDWR);
        BY_DEADLINE("flock", 0, 0, syscall(SYS_flock, fd, LOCK_EX));
        unlink(path); break; }
    }
    _exit(0);
}
#define NCASES 31

// Cases 12, 14, 16 and 24 are syscalls x86_64 has and arm64 does not.
static int case_available(int which) {
    switch (which) {
#ifndef SYS_select
    case 12: return 0;
#endif
#ifndef SYS_poll
    case 14: return 0;
#endif
#ifndef SYS_epoll_wait
    case 16: return 0;
#endif
#ifndef SYS_pause
    case 24: return 0;
#endif
    default: return 1;
    }
}

// The witness. Spins through a syscall every iteration and remembers its
// longest pause; with the machine frozen it cannot run, so that pause is the
// freeze. A case whose call spans it was blocked while the machine stopped.
struct witness { long g0, g1, cases_done; };

static void spin(int stop_fd) {
    fcntl(stop_fd, F_SETFL, O_NONBLOCK);
    struct witness w = {0};
    long prev = now_us();
    char c;
    for (;;) {
        ssize_t n = read(stop_fd, &c, 1);
        long t = now_us();
        if (t - prev > w.g1 - w.g0) {
            w.g0 = prev;
            w.g1 = t;
        }
        prev = t;
        if (n == 0)
            break;
    }
    write_all(result_fd, &w, sizeof(w));
    _exit(0);
}

static const char *errname(int e) {
    switch (e) {
    case 0: return "0";
    case EINTR: return "EINTR";
    case EAGAIN: return "EAGAIN";
    case ETIMEDOUT: return "ETIMEDOUT";
    case ENOSYS: return "ENOSYS";
    case EINVAL: return "EINVAL";
    case EBADF: return "EBADF";
    case ENOTSOCK: return "ENOTSOCK";
    case EPERM: return "EPERM";
    }
    static char other[16];
    snprintf(other, sizeof(other), "errno%d", e);
    return other;
}

#include <poll.h>

// Read exactly n bytes, giving up at `limit` (monotonic us). A case whose
// timeout is never honoured must cost a FAIL, not a hung run.
static ssize_t read_full(int fd, void *p, size_t n, long limit) {
    size_t got = 0;
    while (got < n) {
        long left_ms = (limit - now_us()) / 1000;
        if (left_ms <= 0)
            return -1;
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int pr = poll(&pfd, 1, (int) left_ms);
        if (pr <= 0)
            continue;
        ssize_t r = read(fd, (char *) p + got, n - got);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return r;
        got += (size_t) r;
    }
    return (ssize_t) got;
}

int main(int argc, char **argv) {
    enum { OUTSIDE, SELF, NONE } mode = OUTSIDE;
    const char *save_path = NULL;
    if (argc >= 2 && strcmp(argv[1], "none") == 0)
        mode = NONE;
    else if (argc >= 3 && strcmp(argv[1], "save") == 0)
        mode = SELF, save_path = argv[2];
    setvbuf(stdout, NULL, _IOLBF, 0);

    int ready[2], results[2], witnessed[2], stop[2];
    if (pipe(ready) || pipe(results) || pipe(witnessed) || pipe(stop)) {
        perror("pipe");
        return 3;
    }
    long start = now_us();
    deadline_us = start + WAIT_MS * 1000L;
    // Long past every case's own end, but bounded: a call that never honours
    // its timeout is reported, and does not hang the run.
    long give_up = start + 40 * 1000000L;
    long saves0 = read_saves();

    if (fork() == 0) {
        close(stop[1]);
        result_fd = witnessed[1];
        spin(stop[0]);
    }
    close(stop[0]);

    int started = 0;
    for (int i = 0; i < NCASES; i++) {
        if (!case_available(i))
            continue;
        if (fork() == 0) {
            close(stop[1]);
            case_index = i;
            ready_fd = ready[1];
            result_fd = results[1];
            run_case(i);
        }
        started++;
    }

    // Every case reports just before it blocks.
    for (int got = 0; got < started; got++) {
        char token;
        if (read_full(ready[0], &token, 1, give_up) != 1) {
            printf("SUMMARY only %d of %d cases reached their call\n", got, started);
            kill(-1, SIGKILL);
            return 3;
        }
    }
    long all_blocked = now_us();
    printf("BLOCKED %d cases, %ld ms after start\n", started, (all_blocked - start) / 1000);

    if (mode == SELF) {
        // Give the last case time to get from its ready token into the call.
        sleep_until_us(all_blocked + 1000000L);
        char req[4200];
        int len = snprintf(req, sizeof(req), "save %s\n", save_path);
        int fd = open("/proc/ish/checkpoint", O_WRONLY);
        ssize_t w = fd < 0 ? -1 : write(fd, req, (size_t) len);
        printf("SAVE-REQUEST %s\n", w == len ? "written" : strerror(errno));
        if (fd >= 0)
            close(fd);
    }

    struct result rs[NCASES];
    int have[NCASES] = {0};
    int got = 0;
    for (; got < started; got++) {
        struct result r;
        if (read_full(results[0], &r, sizeof(r), give_up) != (ssize_t) sizeof(r))
            break;
        if (r.index >= 0 && r.index < NCASES) {
            rs[r.index] = r;
            have[r.index] = 1;
        }
    }
    close(stop[1]);
    struct witness w;
    if (read_full(witnessed[0], &w, sizeof(w), give_up + 5 * 1000000L) != (ssize_t) sizeof(w)) {
        printf("SUMMARY lost the witness\n");
        kill(-1, SIGKILL);
        return 3;
    }
    long saves1 = read_saves();
    if (got < started)
        kill(-1, SIGKILL);   // whatever is still blocked is a FAIL below
    while (wait(NULL) > 0 || errno == EINTR)
        ;

    printf("FREEZE saves %ld -> %ld, longest pause %ld ms at +%ld..+%ld ms\n",
           saves0, saves1, (w.g1 - w.g0) / 1000, (w.g0 - start) / 1000, (w.g1 - start) / 1000);
    int froze = saves0 >= 0 && saves1 == saves0 + 1;
    if (mode == NONE && saves1 != saves0) {
        printf("SUMMARY the control run saw a checkpoint (saves %ld -> %ld)\n", saves0, saves1);
        return 2;
    }

    int pass = 0, fail = 0, inconclusive = 0;
    for (int i = 0; i < NCASES; i++) {
        if (!case_available(i))
            continue;
        if (!have[i]) {
            printf("CASE #%-17d FAIL         no result after %ld s -- never returned\n",
                   i, (give_up - start) / 1000000L);
            fail++;
            continue;
        }
        struct result *r = &rs[i];
        const char *why = NULL;
        if (r->err != r->want_err)
            why = "wrong errno";
        else if (r->want_err == 0 && r->rc < 0)
            why = "failed";
        else if (r->want_err == 0 && r->want_rc != WANT_ANY && r->rc != r->want_rc)
            why = "wrong result";
        else if (r->t1 + EARLY_SLACK_MS * 1000L < r->not_before)
            why = "returned early";
        else if (r->bounded &&
                 r->t1 > (r->not_before > w.g1 ? r->not_before : w.g1) + LATE_SLACK_MS * 1000L)
            why = "returned late: the freeze started its timeout again";
        int in_call = r->saves_before == saves0 && r->saves_after == saves0 + 1 &&
                      r->t0 < w.g0 && r->t1 > w.g1;
        const char *verdict;
        if (why != NULL) {
            verdict = "FAIL";
            fail++;
        } else if (mode != NONE && !(froze && in_call)) {
            verdict = "INCONCLUSIVE";
            why = "the freeze did not land inside this call";
            inconclusive++;
        } else {
            verdict = "PASS";
            pass++;
        }
        printf("CASE %-18s %-12s rc=%ld errno=%s want=%s elapsed=%ld ms, due at %ld ms, saves %ld->%ld%s%s\n",
               r->name, verdict, r->rc, errname(r->err), errname(r->want_err),
               (r->t1 - r->t0) / 1000, (r->not_before - r->t0) / 1000,
               r->saves_before, r->saves_after, why ? " -- " : "", why ? why : "");
    }
    printf("SUMMARY pass=%d fail=%d inconclusive=%d\n", pass, fail, inconclusive);
    return fail ? 1 : inconclusive ? 2 : 0;
}
