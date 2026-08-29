// SA_RESTART across the whole restartable surface, both directions.
//
// signal_restart.c covers read() and waitpid(). This covers the rest of what
// signal(7) says SA_RESTART applies to -- the write/readv/writev family, a
// blocking FIFO open, the blocking socket calls, flock and F_SETLKW -- and,
// just as important, the interfaces signal(7) says are NEVER restarted:
// poll, epoll_wait, nanosleep, System V IPC, and any socket call with
// SO_RCVTIMEO armed. Restarting one of those hangs a guest that relies on the
// interruption to make progress, so the controls are not padding.
//
// Every expectation here was measured against x86_64 glibc on Linux 6.12
// rather than read off the man page -- which is how msgrcv ended up in the
// control group: it uses ERESTARTNOHAND, so a running handler cancels the
// restart and it really does return EINTR.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <sys/epoll.h>
#include <sys/file.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/wait.h>

#include "test_common.h"

// Case outcomes.
#define R_RESTARTED  0
#define R_EINTR      1
#define R_SETUP     (-1)
#define R_RACED     (-2)   // signal landed before we blocked: retry, don't judge

static volatile sig_atomic_t handler_ran;
static double t_signal;
static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec + ts.tv_nsec / 1e9;
}
static void onsig(int s) { (void) s; handler_ran++; t_signal = now_s(); }

static double t_enter;
static void arm(void) { handler_ran = 0; t_signal = 0; t_enter = now_s(); }
// Judge only if the handler actually ran while we were blocked. Under load the
// helper's signal can beat the parent into the syscall, and then a call that
// simply completed normally is indistinguishable from a restarted one.
static int judged(int eintr) {
    if (!handler_ran || t_signal < t_enter)
        return R_RACED;
    return eintr ? R_EINTR : R_RESTARTED;
}

static void nap(long ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

typedef void (*action_fn)(void *);
static long sig_ms, act_ms;

static pid_t spawn_helper(action_fn act, void *arg) {
    pid_t parent = getpid();
    fflush(NULL);
    pid_t c = fork();
    if (c != 0) return c;
    nap(sig_ms);
    kill(parent, SIGUSR1);
    if (act != NULL) { nap(act_ms); act(arg); }
    _exit(0);
}
static void reap(pid_t c) {
    if (c <= 0) return;
    int st;
    for (int i = 0; i < 150; i++) {          // never let a wedged helper hang us
        if (waitpid(c, &st, WNOHANG) == c) return;
        nap(100);
    }
    kill(c, SIGKILL);
    waitpid(c, &st, 0);
}

// ---- the cases --------------------------------------------------------
static void act_write_pipe(void *a) { int *fd = a; if (write(fd[1], "z", 1) < 0) {} }
static void act_drain_pipe(void *a) {
    int *fd = a; char buf[4096];
    fcntl(fd[0], F_SETFL, fcntl(fd[0], F_GETFL) | O_NONBLOCK);
    for (int i = 0; i < 256; i++) if (read(fd[0], buf, sizeof buf) <= 0) break;
}
static int fill_pipe(int *fd) {
    int fl = fcntl(fd[1], F_GETFL);
    fcntl(fd[1], F_SETFL, fl | O_NONBLOCK);
    char buf[4096]; long total = 0;
    while (total < (1 << 22)) { ssize_t w = write(fd[1], buf, sizeof buf); if (w <= 0) break; total += w; }
    fcntl(fd[1], F_SETFL, fl);
    return total > 0;
}

static int c_read(void) {
    int fd[2]; if (pipe(fd) < 0) return R_SETUP;
    pid_t c = spawn_helper(act_write_pipe, fd);
    char b; errno = 0; arm();
    ssize_t r = read(fd[0], &b, 1);
    int v = judged(r < 0 && errno == EINTR);
    reap(c); close(fd[0]); close(fd[1]); return v;
}
static int c_write(void) {
    int fd[2]; if (pipe(fd) < 0) return R_SETUP;
    if (!fill_pipe(fd)) { close(fd[0]); close(fd[1]); return R_SETUP; }
    pid_t c = spawn_helper(act_drain_pipe, fd);
    char buf[4096]; errno = 0; arm();
    ssize_t w = write(fd[1], buf, sizeof buf);
    int v = judged(w < 0 && errno == EINTR);
    reap(c); close(fd[0]); close(fd[1]); return v;
}
static int c_readv(void) {
    int fd[2]; if (pipe(fd) < 0) return R_SETUP;
    pid_t c = spawn_helper(act_write_pipe, fd);
    char b; struct iovec iov = { &b, 1 }; errno = 0; arm();
    ssize_t r = readv(fd[0], &iov, 1);
    int v = judged(r < 0 && errno == EINTR);
    reap(c); close(fd[0]); close(fd[1]); return v;
}
static int c_writev(void) {
    int fd[2]; if (pipe(fd) < 0) return R_SETUP;
    if (!fill_pipe(fd)) { close(fd[0]); close(fd[1]); return R_SETUP; }
    pid_t c = spawn_helper(act_drain_pipe, fd);
    char buf[4096]; struct iovec iov = { buf, sizeof buf }; errno = 0; arm();
    ssize_t w = writev(fd[1], &iov, 1);
    int v = judged(w < 0 && errno == EINTR);
    reap(c); close(fd[0]); close(fd[1]); return v;
}

static char fifo_path[128];
static void act_open_fifo(void *a) {
    (void) a;
    // O_NONBLOCK so the helper survives the parent failing to restart --
    // otherwise the bug under test wedges the test.
    int f = open(fifo_path, O_RDONLY | O_NONBLOCK);
    if (f >= 0) { nap(300); close(f); }
}
static int c_fifo_open(void) {
    snprintf(fifo_path, sizeof fifo_path, "/tmp/sarestart_fifo.%d", (int) getpid());
    unlink(fifo_path);
    if (mkfifo(fifo_path, 0600) < 0) return R_SETUP;
    pid_t c = spawn_helper(act_open_fifo, NULL);
    errno = 0; arm();
    int f = open(fifo_path, O_WRONLY);
    int v = judged(f < 0 && errno == EINTR);
    if (f >= 0) close(f);
    reap(c); unlink(fifo_path); return v;
}

static void act_send_sock(void *a) { int *sv = a; if (send(sv[1], "z", 1, 0) < 0) {} }
static void act_drain_sock(void *a) {
    int *sv = a; char buf[4096];
    fcntl(sv[1], F_SETFL, fcntl(sv[1], F_GETFL) | O_NONBLOCK);
    for (int i = 0; i < 1024; i++) if (recv(sv[1], buf, sizeof buf, 0) <= 0) break;
}
static int fill_sock(int *sv) {
    int fl = fcntl(sv[0], F_GETFL);
    fcntl(sv[0], F_SETFL, fl | O_NONBLOCK);
    char buf[4096]; long total = 0;
    while (total < (1 << 23)) { ssize_t w = send(sv[0], buf, sizeof buf, 0); if (w <= 0) break; total += w; }
    fcntl(sv[0], F_SETFL, fl);
    return total > 0;
}
static int c_recv(void) {
    int sv[2]; if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) return R_SETUP;
    pid_t c = spawn_helper(act_send_sock, sv);
    char b; errno = 0; arm();
    ssize_t r = recv(sv[0], &b, 1, 0);
    int v = judged(r < 0 && errno == EINTR);
    reap(c); close(sv[0]); close(sv[1]); return v;
}
static int c_recvmsg(void) {
    int sv[2]; if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) return R_SETUP;
    pid_t c = spawn_helper(act_send_sock, sv);
    char b; struct iovec iov = { &b, 1 }; struct msghdr m;
    memset(&m, 0, sizeof m); m.msg_iov = &iov; m.msg_iovlen = 1;
    errno = 0; arm();
    ssize_t r = recvmsg(sv[0], &m, 0);
    int v = judged(r < 0 && errno == EINTR);
    reap(c); close(sv[0]); close(sv[1]); return v;
}
static int c_send(void) {
    int sv[2]; if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) return R_SETUP;
    if (!fill_sock(sv)) { close(sv[0]); close(sv[1]); return R_SETUP; }
    pid_t c = spawn_helper(act_drain_sock, sv);
    char buf[4096]; errno = 0; arm();
    ssize_t w = send(sv[0], buf, sizeof buf, 0);
    int v = judged(w < 0 && errno == EINTR);
    reap(c); close(sv[0]); close(sv[1]); return v;
}
static int c_recv_timeo(void) {
    int sv[2]; if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) return R_SETUP;
    struct timeval tv = { 20, 0 };      // long enough that only the signal ends it
    if (setsockopt(sv[0], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) < 0) {
        close(sv[0]); close(sv[1]); return R_SETUP;
    }
    pid_t c = spawn_helper(act_send_sock, sv);
    char b; errno = 0; arm();
    ssize_t r = recv(sv[0], &b, 1, 0);
    int v = judged(r < 0 && errno == EINTR);
    reap(c); close(sv[0]); close(sv[1]); return v;
}

static char un_path[128];
static void act_connect(void *a) {
    (void) a;
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, un_path, sizeof sa.sun_path - 1);
    if (connect(s, (struct sockaddr *) &sa, sizeof sa) < 0) {}
    nap(200); close(s);
}
static int c_accept(void) {
    snprintf(un_path, sizeof un_path, "/tmp/sarestart_un.%d", (int) getpid());
    unlink(un_path);
    int ls = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, un_path, sizeof sa.sun_path - 1);
    if (ls < 0 || bind(ls, (struct sockaddr *) &sa, sizeof sa) < 0 || listen(ls, 4) < 0) {
        if (ls >= 0) close(ls);
        return R_SETUP;
    }
    pid_t c = spawn_helper(act_connect, NULL);
    errno = 0; arm();
    int as = accept(ls, NULL, NULL);
    int v = judged(as < 0 && errno == EINTR);
    if (as >= 0) close(as);
    reap(c); close(ls); unlink(un_path); return v;
}

static char lock_path[128];
// The helper must hold the lock BEFORE we try for it, so it syncs through a
// pipe rather than a sleep.
static int contended_lock(int use_flock) {
    snprintf(lock_path, sizeof lock_path, "/tmp/sarestart_lk.%d", (int) getpid());
    int lf = open(lock_path, O_RDWR | O_CREAT, 0600);
    if (lf < 0) return R_SETUP;
    int sync[2];
    if (pipe(sync) < 0) { close(lf); return R_SETUP; }
    fflush(NULL);
    pid_t parent = getpid();
    pid_t c = fork();
    if (c == 0) {
        int f2 = open(lock_path, O_RDWR);
        struct flock l;
        memset(&l, 0, sizeof l);
        l.l_type = F_WRLCK; l.l_whence = SEEK_SET;
        if (use_flock) flock(f2, LOCK_EX); else fcntl(f2, F_SETLK, &l);
        if (write(sync[1], "r", 1) < 0) {}
        nap(sig_ms); kill(parent, SIGUSR1);
        nap(act_ms);
        if (use_flock) flock(f2, LOCK_UN);
        else { l.l_type = F_UNLCK; fcntl(f2, F_SETLK, &l); }
        close(f2);
        _exit(0);
    }
    char rdy;
    if (read(sync[0], &rdy, 1) != 1) { reap(c); close(lf); close(sync[0]); close(sync[1]); return R_SETUP; }
    int r;
    errno = 0; arm();
    if (use_flock) {
        r = flock(lf, LOCK_EX);
    } else {
        struct flock l;
        memset(&l, 0, sizeof l);
        l.l_type = F_WRLCK; l.l_whence = SEEK_SET;
        r = fcntl(lf, F_SETLKW, &l);
    }
    int v = judged(r < 0 && errno == EINTR);
    reap(c); close(lf); close(sync[0]); close(sync[1]); unlink(lock_path);
    return v;
}
static int c_flock(void)  { return contended_lock(1); }
static int c_setlkw(void) { return contended_lock(0); }

static int c_waitpid(void) {
    fflush(NULL);
    pid_t parent = getpid();
    pid_t c = fork();
    if (c < 0) return R_SETUP;
    if (c == 0) { nap(sig_ms); kill(parent, SIGUSR1); nap(act_ms); _exit(7); }
    int st; errno = 0; arm();
    pid_t w = waitpid(c, &st, 0);
    int v = judged(w < 0 && errno == EINTR);
    if (w < 0) reap(c);
    return v;
}

// ---- controls: signal(7)'s never-restarted list -----------------------
static int c_poll(void) {
    int fd[2]; if (pipe(fd) < 0) return R_SETUP;
    pid_t c = spawn_helper(act_write_pipe, fd);
    struct pollfd p = { fd[0], POLLIN, 0 };
    errno = 0; arm();
    int r = poll(&p, 1, 20000);
    int v = judged(r < 0 && errno == EINTR);
    reap(c); close(fd[0]); close(fd[1]); return v;
}
static int c_epoll(void) {
    int fd[2]; if (pipe(fd) < 0) return R_SETUP;
    int ep = epoll_create1(0);
    if (ep < 0) { close(fd[0]); close(fd[1]); return R_SETUP; }
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = fd[0] };
    epoll_ctl(ep, EPOLL_CTL_ADD, fd[0], &ev);
    pid_t c = spawn_helper(act_write_pipe, fd);
    struct epoll_event out;
    errno = 0; arm();
    int r = epoll_wait(ep, &out, 1, 20000);
    int v = judged(r < 0 && errno == EINTR);
    reap(c); close(ep); close(fd[0]); close(fd[1]); return v;
}
static int c_nanosleep(void) {
    pid_t c = spawn_helper(NULL, NULL);
    struct timespec ts = { 20, 0 }, rem;
    errno = 0; arm();
    int r = nanosleep(&ts, &rem);
    int v = judged(r < 0 && errno == EINTR);
    reap(c); return v;
}
struct mbuf { long mtype; char mtext[8]; };
static int c_msgrcv(void) {
    int q = msgget(IPC_PRIVATE, 0600 | IPC_CREAT);
    if (q < 0) return R_SETUP;
    fflush(NULL);
    pid_t parent = getpid();
    pid_t c = fork();
    if (c < 0) { msgctl(q, IPC_RMID, NULL); return R_SETUP; }
    if (c == 0) {
        nap(sig_ms); kill(parent, SIGUSR1); nap(act_ms);
        struct mbuf m = { 1, "hi" };
        msgsnd(q, &m, sizeof m.mtext, 0);
        _exit(0);
    }
    struct mbuf m; errno = 0; arm();
    ssize_t r = msgrcv(q, &m, sizeof m.mtext, 1, 0);
    int v = judged(r < 0 && errno == EINTR);
    reap(c); msgctl(q, IPC_RMID, NULL); return v;
}
static int c_semop(void) {
    int s = semget(IPC_PRIVATE, 1, 0600 | IPC_CREAT);
    if (s < 0) return R_SETUP;
    semctl(s, 0, SETVAL, 0);
    fflush(NULL);
    pid_t parent = getpid();
    pid_t c = fork();
    if (c < 0) { semctl(s, 0, IPC_RMID); return R_SETUP; }
    if (c == 0) {
        nap(sig_ms); kill(parent, SIGUSR1); nap(act_ms);
        struct sembuf up = { 0, 1, 0 };
        semop(s, &up, 1);
        _exit(0);
    }
    struct sembuf down = { 0, -1, 0 };
    errno = 0; arm();
    int r = semop(s, &down, 1);
    int v = judged(r < 0 && errno == EINTR);
    reap(c); semctl(s, 0, IPC_RMID); return v;
}

// ---- driver -----------------------------------------------------------
struct testcase { const char *name; int (*fn)(void); int want; };
static const struct testcase cases[] = {
    // want == R_RESTARTED: SA_RESTART applies (signal(7))
    { "read",             c_read,       R_RESTARTED },
    { "write",            c_write,      R_RESTARTED },
    { "readv",            c_readv,      R_RESTARTED },
    { "writev",           c_writev,     R_RESTARTED },
    { "open(FIFO)",       c_fifo_open,  R_RESTARTED },
    { "recv",             c_recv,       R_RESTARTED },
    { "recvmsg",          c_recvmsg,    R_RESTARTED },
    { "send",             c_send,       R_RESTARTED },
    { "accept",           c_accept,     R_RESTARTED },
    { "flock",            c_flock,      R_RESTARTED },
    { "fcntl(F_SETLKW)",  c_setlkw,     R_RESTARTED },
    { "waitpid",          c_waitpid,    R_RESTARTED },
    // want == R_EINTR: never restarted, whatever the handler asked for
    { "recv+SO_RCVTIMEO", c_recv_timeo, R_EINTR },
    { "poll",             c_poll,       R_EINTR },
    { "epoll_wait",       c_epoll,      R_EINTR },
    { "nanosleep",        c_nanosleep,  R_EINTR },
    { "msgrcv",           c_msgrcv,     R_EINTR },
    { "semop",            c_semop,      R_EINTR },
};

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(600));

    // Emulation is slow and the suite runs loaded; scale the handshake with
    // the same knob that widens the watchdog.
    long scale = (long) test_watchdog_secs(1);
    sig_ms = 400 * scale;
    act_ms = 500 * scale;

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = onsig;
    sa.sa_flags = SA_RESTART;              // the whole point
    sigaction(SIGUSR1, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        int v = R_RACED;
        for (int attempt = 0; attempt < 3 && v == R_RACED; attempt++)
            v = cases[i].fn();
        if (v == R_SETUP) {
            printf("  %-18s SKIP (setup unavailable)\n", cases[i].name);
            continue;
        }
        if (v == R_RACED) {
            printf("  %-18s SKIP (signal raced the syscall 3x)\n", cases[i].name);
            continue;
        }
        if (v != cases[i].want)
            failf(cases[i].name, (uint64_t) v, 0, 0, (uint64_t) cases[i].want, 0, 0);
        test_logf("  %-18s %-10s expect %s\n", cases[i].name,
                  v == R_EINTR ? "EINTR" : "restarted",
                  cases[i].want == R_EINTR ? "EINTR" : "restarted");
    }
    return finish_suite("signal_restart_coverage");
}
