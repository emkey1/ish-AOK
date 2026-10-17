// ptrace_stop_restart: a blocking call carries on across a ptrace stop that
// runs no handler.
//
// A tracee blocked in read() on a pipe, stopped with PTRACE_INTERRUPT and
// resumed with PTRACE_CONT(pid, 0), got EINTR from the read. Linux restarts it,
// because no handler ran: 10 runs out of 10 on Linux 6.12, against EINTR 10 out
// of 10 on AOK, root and unprivileged, in arm64 Alpine and Devuan roots. So a
// tracer that interrupted and resumed a process made its blocking syscall fail
// -- and strace -p interrupts its target to detach from it. ptrace_eventmsg's
// seize case retried its read on EINTR because of this.
//
// AOK's PTRACE_INTERRUPT queued the tracee a real SIGTRAP, because that is what
// woke it out of the wait (Linux sets JOBCTL_TRAP_STOP, a flag). The syscall
// then chose between EINTR and a restart by the pending signal's disposition,
// and SIGTRAP's is terminate, so it chose EINTR, for a signal that is never
// delivered at all. The interrupt is now a flag too (ptrace.trap_stop, see
// ptrace_seize_trap_stop), and the restart predicates answer for it directly.
//
// The same choice was wrong for every signal a tracer sees first. A tracer can
// resume a signal-delivery-stop without the signal; then nothing is delivered
// and Linux restarts the call. That is gdb's ^C and `continue` on a program
// reading its terminal -- gdb does not pass SIGINT on by default -- and AOK had
// already chosen EINTR from the handler it expected to run. Every traced row
// below that completes failed with EINTR on AOK. Now such a call is set to
// restart, and a handler that actually runs cancels the restart if it lacks
// SA_RESTART: Linux's handle_signal rule, applied where Linux applies it.
//
// That rule also settles a job-control stop followed by a SIGCONT handler with
// no SA_RESTART. Linux fails the call with EINTR, since a handler ran; AOK
// restarted read, recv, waitpid and futex, because the stop had decided. The
// untraced rows at the end pin both halves.
//
// epoll_wait and sigtimedwait never restart, and fail with EINTR across any
// stop on Linux. They are here so that the fix cannot restart too much.
//
// A vfork parent's wait had the same misreading in a worse place; see
// vfork_row.
//
// Every expectation was measured on Linux 6.12 (x86_64, -m64 and -m32) before
// it was checked against AOK.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <time.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>

#define TEST_WANT_RAW_VFORK   // musl powerpc64: the system call, not a fork
#include "test_common.h"

#ifndef __WALL
#define __WALL 0x40000000
#endif
#ifndef PTRACE_SEIZE
#define PTRACE_SEIZE 0x4206
#endif
#ifndef PTRACE_INTERRUPT
#define PTRACE_INTERRUPT 0x4207
#endif
#ifndef PTRACE_EVENT_STOP
#define PTRACE_EVENT_STOP 128
#endif
#define FUTEX_WAIT_OP 0
#define FUTEX_WAKE_OP 1

// The status word waitpid reports for a stop.
#define STOP_STATUS(sig, event) (((event) << 16) | ((sig) << 8) | 0x7f)

// Every delay below is multiplied by ISH_TEST_WATCHDOG_SCALE, which a loaded run
// (the release procedure's concurrent multi-arch sweep) raises: a child slow to
// reach its call would otherwise take an injected signal before it, and fail a
// row that expects EINTR. sleep_ms is how long the timed calls sleep, several
// times what the stop, the resume and the settles take together.
static long scale = 1;
static long sleep_ms = 700;

enum kind {
    K_READ_PIPE, K_READ_EVENTFD, K_RECV, K_ACCEPT, K_OPEN_FIFO, K_WRITE_FULL_PIPE,
    K_WAITPID, K_WAITID, K_FUTEX, K_SETLKW, K_POLL, K_NANOSLEEP,
    K_CLOCK_NANOSLEEP, K_EPOLL_WAIT, K_SIGTIMEDWAIT, K_COUNT,
};

static const char *const kind_names[K_COUNT] = {
    "read pipe", "read eventfd", "recv", "accept", "open fifo", "write full pipe",
    "waitpid", "waitid", "futex wait", "F_SETLKW", "poll", "nanosleep",
    "clock_nanosleep", "epoll_wait", "sigtimedwait",
};

enum how {
    // Traced. The tracer stops the blocked call and resumes it with no signal.
    H_INTERRUPT,            // PTRACE_INTERRUPT
    H_SIGNAL_HANDLED,       // SIGUSR1, whose handler has no SA_RESTART
    H_SIGNAL_DEFAULT,       // SIGINT, default action
    // Traced. The same stops, then PTRACE_DETACH with no signal.
    H_INTERRUPT_DETACH,
    H_SIGNAL_DETACH,
    // Traced. The signal is passed on, so its handler runs.
    H_INJECT_HANDLED,       // SIGUSR1: no SA_RESTART
    H_INJECT_RESTARTING,    // SIGTERM: SA_RESTART
    // Untraced. SIGSTOP, then SIGCONT.
    H_JOBSTOP,
    H_JOBSTOP_CONT_HANDLER, // SIGCONT has a handler, without SA_RESTART
    H_COUNT,
};

static const char *const how_names[H_COUNT] = {
    "PTRACE_INTERRUPT, resumed",
    "SIGUSR1 stop, resumed without it",
    "SIGINT stop, resumed without it",
    "PTRACE_INTERRUPT, detached",
    "SIGUSR1 stop, detached without it",
    "SIGUSR1 stop, passed on",
    "SA_RESTART SIGTERM stop, passed on",
    "SIGSTOP then SIGCONT",
    "SIGSTOP then a handled SIGCONT",
};

enum verdict { COMPLETES, FAILS_EINTR, NEITHER };
static const char *const verdict_names[] = { "completes", "EINTR", "neither" };

static const struct row {
    enum kind kind;
    enum how how;
    enum verdict want;
} rows[] = {
    // The report, and every other kind of wait, for both stops a tracer makes
    // without delivering anything.
    { K_READ_PIPE, H_INTERRUPT, COMPLETES },
    { K_READ_EVENTFD, H_INTERRUPT, COMPLETES },
    { K_RECV, H_INTERRUPT, COMPLETES },
    { K_ACCEPT, H_INTERRUPT, COMPLETES },
    { K_OPEN_FIFO, H_INTERRUPT, COMPLETES },
    { K_WRITE_FULL_PIPE, H_INTERRUPT, COMPLETES },
    { K_WAITPID, H_INTERRUPT, COMPLETES },
    { K_WAITID, H_INTERRUPT, COMPLETES },
    { K_FUTEX, H_INTERRUPT, COMPLETES },
    { K_SETLKW, H_INTERRUPT, COMPLETES },
    { K_POLL, H_INTERRUPT, COMPLETES },
    { K_NANOSLEEP, H_INTERRUPT, COMPLETES },
    { K_CLOCK_NANOSLEEP, H_INTERRUPT, COMPLETES },
    { K_EPOLL_WAIT, H_INTERRUPT, FAILS_EINTR },
    { K_SIGTIMEDWAIT, H_INTERRUPT, FAILS_EINTR },

    { K_READ_PIPE, H_SIGNAL_HANDLED, COMPLETES },
    { K_READ_EVENTFD, H_SIGNAL_HANDLED, COMPLETES },
    { K_RECV, H_SIGNAL_HANDLED, COMPLETES },
    { K_ACCEPT, H_SIGNAL_HANDLED, COMPLETES },
    { K_OPEN_FIFO, H_SIGNAL_HANDLED, COMPLETES },
    { K_WRITE_FULL_PIPE, H_SIGNAL_HANDLED, COMPLETES },
    { K_WAITPID, H_SIGNAL_HANDLED, COMPLETES },
    { K_WAITID, H_SIGNAL_HANDLED, COMPLETES },
    { K_FUTEX, H_SIGNAL_HANDLED, COMPLETES },
    { K_SETLKW, H_SIGNAL_HANDLED, COMPLETES },
    { K_POLL, H_SIGNAL_HANDLED, COMPLETES },
    { K_NANOSLEEP, H_SIGNAL_HANDLED, COMPLETES },
    { K_CLOCK_NANOSLEEP, H_SIGNAL_HANDLED, COMPLETES },
    { K_EPOLL_WAIT, H_SIGNAL_HANDLED, FAILS_EINTR },
    { K_SIGTIMEDWAIT, H_SIGNAL_HANDLED, FAILS_EINTR },

    // A default action the tracer never lets happen.
    { K_READ_PIPE, H_SIGNAL_DEFAULT, COMPLETES },
    { K_WAITPID, H_SIGNAL_DEFAULT, COMPLETES },
    { K_POLL, H_SIGNAL_DEFAULT, COMPLETES },

    // strace -p's exit: stop the target, detach at the stop.
    { K_READ_PIPE, H_INTERRUPT_DETACH, COMPLETES },
    { K_FUTEX, H_INTERRUPT_DETACH, COMPLETES },
    { K_NANOSLEEP, H_INTERRUPT_DETACH, COMPLETES },
    { K_POLL, H_SIGNAL_DETACH, COMPLETES },

    // A handler that runs decides. Without SA_RESTART it ends every call; with
    // it, only the calls a handler never restarts fail.
    { K_READ_PIPE, H_INJECT_HANDLED, FAILS_EINTR },
    { K_FUTEX, H_INJECT_HANDLED, FAILS_EINTR },
    { K_POLL, H_INJECT_HANDLED, FAILS_EINTR },
    { K_READ_PIPE, H_INJECT_RESTARTING, COMPLETES },
    { K_WAITPID, H_INJECT_RESTARTING, COMPLETES },
    { K_FUTEX, H_INJECT_RESTARTING, COMPLETES },
    { K_POLL, H_INJECT_RESTARTING, FAILS_EINTR },
    { K_NANOSLEEP, H_INJECT_RESTARTING, FAILS_EINTR },

    // No tracer. A stop alone ends nothing; the handler of the SIGCONT that
    // ends the stop does, whatever the stop would have decided.
    { K_READ_PIPE, H_JOBSTOP, COMPLETES },
    { K_FUTEX, H_JOBSTOP, COMPLETES },
    { K_READ_PIPE, H_JOBSTOP_CONT_HANDLER, FAILS_EINTR },
    { K_RECV, H_JOBSTOP_CONT_HANDLER, FAILS_EINTR },
    { K_WAITPID, H_JOBSTOP_CONT_HANDLER, FAILS_EINTR },
    { K_FUTEX, H_JOBSTOP_CONT_HANDLER, FAILS_EINTR },
    { K_POLL, H_JOBSTOP_CONT_HANDLER, FAILS_EINTR },
};

// What the child reports about its one blocking call.
struct report {
    long ret;
    int err;
    long started_ms;
    long elapsed_ms;
};

static void on_signal(int sig) { (void) sig; }

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void nap(long ms) {
    struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };
    while (nanosleep(&t, &t) != 0 && errno == EINTR)
        ;
}

static void set_nonblock(int fd, bool on) {
    int flags = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, on ? flags | O_NONBLOCK : flags & ~O_NONBLOCK);
}

// The state letter from /proc/<pid>/stat: 'S' while asleep in a wait.
static char task_state(pid_t pid) {
    char path[64], buf[512];
    snprintf(path, sizeof path, "/proc/%d/stat", (int) pid);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return '?';
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0)
        return '?';
    buf[n] = '\0';
    char *paren = strrchr(buf, ')');
    return paren != NULL && paren[1] == ' ' ? paren[2] : '?';
}

// Everything a row's call can wait on, set up before the fork.
struct fixture {
    int pipe_fds[2];
    int event_fd;
    int sock_fds[2];
    int gate[2];            // the waitpid rows' grandchild exits when this is written
    int listen_fd;
    int lock_fd;
    int peer_fd;            // whatever the wake opened or connected
    int *futex_word;
    char sock_path[sizeof ((struct sockaddr_un *) 0)->sun_path];
    char lock_path[108];
    char fifo_path[108];
};

static const char *tmp_dir(void) {
    const char *dir = getenv("TMPDIR");
    return dir != NULL && dir[0] != '\0' ? dir : "/tmp";
}

static int fixture_open(struct fixture *f, enum kind kind) {
    memset(f, 0, sizeof *f);
    f->pipe_fds[0] = f->pipe_fds[1] = f->sock_fds[0] = f->sock_fds[1] = -1;
    f->gate[0] = f->gate[1] = -1;
    f->event_fd = f->listen_fd = f->lock_fd = f->peer_fd = -1;
    f->futex_word = MAP_FAILED;
    if (pipe(f->pipe_fds) != 0 || pipe(f->gate) != 0)
        return -1;
    switch (kind) {
    case K_READ_EVENTFD:
        f->event_fd = eventfd(0, 0);
        return f->event_fd < 0 ? -1 : 0;
    case K_RECV:
        return socketpair(AF_UNIX, SOCK_STREAM, 0, f->sock_fds);
    case K_FUTEX:
        f->futex_word = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                             MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if (f->futex_word == MAP_FAILED)
            return -1;
        *f->futex_word = 0;
        return 0;
    case K_ACCEPT: {
        snprintf(f->sock_path, sizeof f->sock_path, "%s/ptrace_stop_restart.%d.sock",
                 tmp_dir(), (int) getpid());
        unlink(f->sock_path);
        f->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un addr = { .sun_family = AF_UNIX };
        memcpy(addr.sun_path, f->sock_path, sizeof addr.sun_path);
        if (f->listen_fd < 0 || bind(f->listen_fd, (struct sockaddr *) &addr, sizeof addr) != 0 ||
                listen(f->listen_fd, 1) != 0)
            return -1;
        return 0;
    }
    case K_SETLKW: {
        snprintf(f->lock_path, sizeof f->lock_path, "%s/ptrace_stop_restart.%d.lock",
                 tmp_dir(), (int) getpid());
        f->lock_fd = open(f->lock_path, O_RDWR | O_CREAT | O_TRUNC, 0600);
        struct flock lock = { .l_type = F_WRLCK, .l_whence = SEEK_SET };
        if (f->lock_fd < 0 || fcntl(f->lock_fd, F_SETLK, &lock) != 0)
            return -1;
        return 0;
    }
    case K_OPEN_FIFO:
        snprintf(f->fifo_path, sizeof f->fifo_path, "%s/ptrace_stop_restart.%d.fifo",
                 tmp_dir(), (int) getpid());
        unlink(f->fifo_path);
        return mkfifo(f->fifo_path, 0600);
    default:
        return 0;
    }
}

static void fixture_close(struct fixture *f) {
    int fds[] = { f->pipe_fds[0], f->pipe_fds[1], f->event_fd, f->sock_fds[0], f->sock_fds[1],
                  f->gate[0], f->gate[1], f->listen_fd, f->lock_fd, f->peer_fd };
    for (size_t i = 0; i < sizeof fds / sizeof fds[0]; i++)
        if (fds[i] >= 0)
            close(fds[i]);
    if (f->futex_word != MAP_FAILED)
        munmap(f->futex_word, 4096);
    if (f->sock_path[0] != '\0')
        unlink(f->sock_path);
    if (f->lock_path[0] != '\0')
        unlink(f->lock_path);
    if (f->fifo_path[0] != '\0')
        unlink(f->fifo_path);
}

static void install(int sig, void (*handler)(int), int flags) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = handler;
    sa.sa_flags = flags;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
}

// The tracee: make the one blocking call and report what it returned.
static void child_main(enum kind kind, enum how how, struct fixture *f, int ready_fd, int report_fd) {
    install(SIGUSR1, on_signal, 0);
    install(SIGTERM, on_signal, SA_RESTART);
    if (how == H_JOBSTOP_CONT_HANDLER)
        install(SIGCONT, on_signal, 0);

    pid_t grandchild = -1;
    int epoll_fd = -1;
    sigset_t wait_set;
    sigemptyset(&wait_set);
    char byte;
    if (kind == K_WAITPID || kind == K_WAITID) {
        grandchild = fork();
        if (grandchild == 0) {
            if (read(f->gate[0], &byte, 1) < 0) {}
            _exit(0);
        }
    }
    if (kind == K_EPOLL_WAIT) {
        epoll_fd = epoll_create1(0);
        struct epoll_event ev = { .events = EPOLLIN, .data.fd = f->pipe_fds[0] };
        epoll_ctl(epoll_fd, EPOLL_CTL_ADD, f->pipe_fds[0], &ev);
    }
    if (kind == K_WRITE_FULL_PIPE) {
        char chunk[4096];
        memset(chunk, 'f', sizeof chunk);
        set_nonblock(f->pipe_fds[1], true);
        while (write(f->pipe_fds[1], chunk, sizeof chunk) > 0)
            ;
        while (write(f->pipe_fds[1], chunk, 1) > 0)
            ;
        set_nonblock(f->pipe_fds[1], false);
    }
    if (kind == K_SIGTIMEDWAIT) {
        sigaddset(&wait_set, SIGUSR2);
        sigprocmask(SIG_BLOCK, &wait_set, NULL);
    }

    if (write(ready_fd, "r", 1) != 1)
        _exit(90);
    struct report report = { .started_ms = now_ms() };
    long ret = -1;
    errno = 0;
    switch (kind) {
    case K_READ_PIPE: ret = read(f->pipe_fds[0], &byte, 1); break;
    case K_READ_EVENTFD: { uint64_t count; ret = read(f->event_fd, &count, sizeof count); break; }
    case K_RECV: ret = recv(f->sock_fds[1], &byte, 1, 0); break;
    case K_ACCEPT: ret = accept(f->listen_fd, NULL, NULL); break;
    case K_OPEN_FIFO: ret = open(f->fifo_path, O_RDONLY); break;
    case K_WRITE_FULL_PIPE: ret = write(f->pipe_fds[1], "w", 1); break;
    case K_WAITPID: { int st; ret = waitpid(grandchild, &st, 0); break; }
    case K_WAITID: { siginfo_t info; ret = waitid(P_PID, grandchild, &info, WEXITED); break; }
    case K_FUTEX: ret = syscall(SYS_futex, f->futex_word, FUTEX_WAIT_OP, 0, NULL, NULL, 0); break;
    case K_SETLKW: {
        struct flock lock = { .l_type = F_WRLCK, .l_whence = SEEK_SET };
        ret = fcntl(f->lock_fd, F_SETLKW, &lock);
        break;
    }
    case K_POLL: { struct pollfd p = { f->pipe_fds[0], POLLIN, 0 }; ret = poll(&p, 1, -1); break; }
    case K_NANOSLEEP: {
        struct timespec t = { sleep_ms / 1000, (sleep_ms % 1000) * 1000000L };
        ret = nanosleep(&t, NULL);
        break;
    }
    case K_CLOCK_NANOSLEEP: {
        // The libc wrapper rather than the raw syscall: it returns the error
        // number itself, and it passes the right timespec on a 32-bit libc
        // with a 64-bit time_t.
        struct timespec t = { sleep_ms / 1000, (sleep_ms % 1000) * 1000000L };
        int err = clock_nanosleep(CLOCK_MONOTONIC, 0, &t, NULL);
        ret = err == 0 ? 0 : -1;
        errno = err;
        break;
    }
    case K_EPOLL_WAIT: { struct epoll_event ev; ret = epoll_wait(epoll_fd, &ev, 1, -1); break; }
    case K_SIGTIMEDWAIT:
        // The raw syscall: musl's sigtimedwait() retries EINTR itself, which
        // would hide the kernel's answer. 8 is the kernel's sigset size.
        ret = syscall(SYS_rt_sigtimedwait, &wait_set, NULL, NULL, 8);
        break;
    default: break;
    }
    report.ret = ret;
    report.err = errno;
    report.elapsed_ms = now_ms() - report.started_ms;
    if (write(report_fd, &report, sizeof report) != (ssize_t) sizeof report)
        _exit(91);
    _exit(0);
}

// Give the call what it is waiting for.
static void wake(enum kind kind, struct fixture *f, pid_t child) {
    switch (kind) {
    case K_READ_PIPE: case K_POLL: case K_EPOLL_WAIT:
        if (write(f->pipe_fds[1], "x", 1) != 1) {}
        break;
    case K_READ_EVENTFD: {
        uint64_t one = 1;
        if (write(f->event_fd, &one, sizeof one) != (ssize_t) sizeof one) {}
        break;
    }
    case K_RECV:
        if (send(f->sock_fds[0], "x", 1, 0) != 1) {}
        break;
    case K_ACCEPT: {
        f->peer_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un addr = { .sun_family = AF_UNIX };
        memcpy(addr.sun_path, f->sock_path, sizeof addr.sun_path);
        if (connect(f->peer_fd, (struct sockaddr *) &addr, sizeof addr) != 0) {}
        break;
    }
    case K_OPEN_FIFO:
        // Nonblocking: if the reader's open has already failed there is no one
        // to pair with, and a blocking open would hang the tracer instead.
        f->peer_fd = open(f->fifo_path, O_WRONLY | O_NONBLOCK);
        break;
    case K_WRITE_FULL_PIPE: {
        char buf[65536];
        set_nonblock(f->pipe_fds[0], true);
        while (read(f->pipe_fds[0], buf, sizeof buf) > 0)
            ;
        break;
    }
    case K_WAITPID: case K_WAITID:
        if (write(f->gate[1], "x", 1) != 1) {}
        break;
    case K_FUTEX:
        *f->futex_word = 1;
        syscall(SYS_futex, f->futex_word, FUTEX_WAKE_OP, 1, NULL, NULL, 0);
        break;
    case K_SETLKW: {
        struct flock unlock = { .l_type = F_UNLCK, .l_whence = SEEK_SET };
        fcntl(f->lock_fd, F_SETLK, &unlock);
        break;
    }
    case K_SIGTIMEDWAIT:
        kill(child, SIGUSR2);
        break;
    default:
        break;
    }
}

static pid_t wait_child(pid_t pid, int *status, int options) {
    alarm(test_watchdog_secs(10));
    pid_t got = waitpid(pid, status, options);
    int saved = errno;
    alarm(0);
    errno = saved;
    return got;
}

static void fail_row(const struct row *row, const char *fmt, ...) {
    va_list ap;
    printf("FAIL %s, %s: ", kind_names[row->kind], how_names[row->how]);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    failures_total++;
}

// Run one row. False means the stop provably landed before the call began, so
// the round tested nothing and has to be run again.
static bool run_row(const struct row *row) {
    enum kind kind = row->kind;
    enum how how = row->how;
    bool traced = how != H_JOBSTOP && how != H_JOBSTOP_CONT_HANDLER;
    bool valid = true;
    int status = 0;
    long stop_at = 0;
    struct fixture f;
    int ready[2] = { -1, -1 }, report_pipe[2] = { -1, -1 };
    if (fixture_open(&f, kind) != 0 || pipe(ready) != 0 || pipe(report_pipe) != 0) {
        fail_row(row, "setup: %s", strerror(errno));
        goto cleanup;
    }
    fflush(NULL);
    pid_t child = fork();
    if (child == 0) {
        close(ready[0]);
        close(report_pipe[0]);
        child_main(kind, how, &f, ready[1], report_pipe[1]);
    }
    close(ready[1]);
    close(report_pipe[1]);
    ready[1] = report_pipe[1] = -1;
    if (child < 0) {
        fail_row(row, "fork: %s", strerror(errno));
        goto cleanup;
    }
    if (traced && ptrace(PTRACE_SEIZE, child, 0, 0) != 0) {
        fail_row(row, "PTRACE_SEIZE: %s", strerror(errno));
        goto kill_child;
    }
    char byte;
    if (read(ready[0], &byte, 1) != 1) {
        fail_row(row, "the child never reached its call");
        goto kill_child;
    }

    // Asleep in the call, rather than on its way into it. A task in epoll_wait
    // never reads as asleep on AOK, which only costs that row the whole wait.
    long asleep_by = now_ms() + 500 * scale;
    while (task_state(child) != 'S' && now_ms() < asleep_by)
        nap(5);
    nap(100 * scale);

    stop_at = now_ms();
    if (!traced) {
        kill(child, SIGSTOP);
        if (wait_child(child, &status, WUNTRACED) != child || !WIFSTOPPED(status)) {
            fail_row(row, "no job-control stop: status %#x, %s", status, strerror(errno));
            goto kill_child;
        }
        nap(100 * scale);
        kill(child, SIGCONT);
    } else {
        int sig = how == H_SIGNAL_DEFAULT ? SIGINT :
                  how == H_INJECT_RESTARTING ? SIGTERM : SIGUSR1;
        int want_status = STOP_STATUS(sig, 0);
        if (how == H_INTERRUPT || how == H_INTERRUPT_DETACH) {
            want_status = STOP_STATUS(SIGTRAP, PTRACE_EVENT_STOP);
            if (ptrace(PTRACE_INTERRUPT, child, 0, 0) != 0) {
                fail_row(row, "PTRACE_INTERRUPT: %s", strerror(errno));
                goto kill_child;
            }
        } else {
            kill(child, sig);
        }
        if (wait_child(child, &status, __WALL) != child) {
            fail_row(row, "waiting for the stop: %s", strerror(errno));
            goto kill_child;
        }
        if (status != want_status) {
            fail_row(row, "stop status %#x, expected %#x", status, want_status);
            goto kill_child;
        }
        if (how == H_INTERRUPT_DETACH || how == H_SIGNAL_DETACH) {
            ptrace(PTRACE_DETACH, child, 0, 0);
            traced = false;
        } else {
            long pass = how == H_INJECT_HANDLED || how == H_INJECT_RESTARTING ? sig : 0;
            ptrace(PTRACE_CONT, child, 0, (void *) pass);
        }
    }

    // Long enough for a restarted call to be waiting again, so that one which
    // completes can only have done so because of the wake.
    nap(150 * scale);
    wake(kind, &f, child);

    for (;;) {
        if (wait_child(child, &status, __WALL) != child) {
            fail_row(row, "the call never finished (EINTR here is a hang)");
            goto kill_child;
        }
        if (traced && WIFSTOPPED(status)) {
            // A signal-delivery-stop for something else -- the grandchild's
            // SIGCHLD, say. Deliver it and carry on.
            long sig = (status >> 16) == 0 ? WSTOPSIG(status) : 0;
            ptrace(PTRACE_CONT, child, 0, (void *) sig);
            continue;
        }
        if (WIFEXITED(status) || WIFSIGNALED(status))
            break;
    }

    struct report rep;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
            read(report_pipe[0], &rep, sizeof rep) != (ssize_t) sizeof rep) {
        fail_row(row, "the child died before it reported: status %#x", status);
        goto cleanup;
    }
    if (rep.started_ms > stop_at) {
        test_logf("  again %s, %s: the stop landed before the call began\n",
                  kind_names[kind], how_names[how]);
        valid = false;
        goto cleanup;
    }

    enum verdict got;
    if (kind == K_NANOSLEEP || kind == K_CLOCK_NANOSLEEP)
        got = rep.ret == 0 && rep.elapsed_ms >= sleep_ms - 10 ? COMPLETES :
              rep.ret == -1 && rep.err == EINTR ? FAILS_EINTR : NEITHER;
    else if (kind == K_SIGTIMEDWAIT)
        got = rep.ret == SIGUSR2 ? COMPLETES :
              rep.ret == -1 && rep.err == EINTR ? FAILS_EINTR : NEITHER;
    else
        got = rep.ret >= 0 ? COMPLETES :
              rep.ret == -1 && rep.err == EINTR ? FAILS_EINTR : NEITHER;

    if (got != row->want)
        fail_row(row, "%s, expected %s (returned %ld, errno %d, after %ldms)",
                 verdict_names[got], verdict_names[row->want],
                 rep.ret, rep.err, rep.elapsed_ms);
    else
        test_logf("  ok   %-16s %-36s %s after %ldms\n", kind_names[kind],
                  how_names[how], verdict_names[got], rep.elapsed_ms);
    goto cleanup;

kill_child:
    kill(child, SIGKILL);
    while (waitpid(child, &status, __WALL) == child && !WIFEXITED(status) && !WIFSIGNALED(status))
        ;
cleanup:
    for (int i = 0; i < 2; i++) {
        if (ready[i] >= 0)
            close(ready[i]);
        if (report_pipe[i] >= 0)
            close(report_pipe[i]);
    }
    fixture_close(&f);
    return valid;
}

// A vfork parent's wait is killable but not interruptible: until the child
// execs or exits it runs on the parent's stack. A stop is not a kill, so
// neither PTRACE_INTERRUPT nor a SIGINT the tracer resumes without may end the
// wait, and Linux reports no stop until the child has finished. AOK took both
// for fatal, since both dispositions are terminate: the parent stopped at once,
// came back out of vfork while the child was still running, and died of SIGSEGV
// on the stack they shared.
static volatile int vfork_child_done;

static const enum how vfork_hows[] = { H_INTERRUPT, H_SIGNAL_DEFAULT };

// Same contract as run_row: false means the stop landed before the vfork.
static bool vfork_row(enum how which) {
    const char *how = how_names[which];
    int sent = which == H_SIGNAL_DEFAULT ? SIGINT : 0;
    bool valid = true;
    int status = 0;
    int gate[2] = { -1, -1 }, ready[2] = { -1, -1 }, report_pipe[2] = { -1, -1 };
    if (pipe(gate) != 0 || pipe(ready) != 0 || pipe(report_pipe) != 0) {
        printf("FAIL vfork, %s: pipe: %s\n", how, strerror(errno));
        failures_total++;
        goto cleanup;
    }
    fflush(NULL);
    pid_t child = fork();
    if (child == 0) {
        if (write(ready[1], "r", 1) != 1)
            _exit(90);
        struct report rep = { .started_ms = now_ms() };
        vfork_child_done = 0;
        pid_t grandchild = vfork();
        if (grandchild == 0) {
            char byte;
            if (read(gate[0], &byte, 1) < 0) {}
            vfork_child_done = 1;
            _exit(0);
        }
        // Back out of vfork: the child must have finished by now.
        rep.ret = vfork_child_done ? grandchild : -1;
        if (write(report_pipe[1], &rep, sizeof rep) != (ssize_t) sizeof rep)
            _exit(91);
        int st;
        waitpid(grandchild, &st, 0);
        _exit(0);
    }
    close(ready[1]);
    close(report_pipe[1]);
    ready[1] = report_pipe[1] = -1;
    if (child < 0) {
        printf("FAIL vfork, %s: fork: %s\n", how, strerror(errno));
        failures_total++;
        goto cleanup;
    }
    if (ptrace(PTRACE_SEIZE, child, 0, 0) != 0) {
        printf("FAIL vfork, %s: PTRACE_SEIZE: %s\n", how, strerror(errno));
        failures_total++;
        goto kill_child;
    }
    char byte;
    if (read(ready[0], &byte, 1) != 1) {
        printf("FAIL vfork, %s: the child never reached vfork\n", how);
        failures_total++;
        goto kill_child;
    }
    // A vfork wait reads as 'D' on Linux, so there is no state to wait for;
    // the vfork child blocks on the gate long before this is up.
    nap(300 * scale);

    long stop_at = now_ms();
    if (sent != 0)
        kill(child, sent);
    else if (ptrace(PTRACE_INTERRUPT, child, 0, 0) != 0) {
        printf("FAIL vfork, %s: PTRACE_INTERRUPT: %s\n", how, strerror(errno));
        failures_total++;
        goto kill_child;
    }
    nap(300 * scale);
    // Nothing to report yet: the parent is still waiting for its child.
    if (waitpid(child, &status, WNOHANG | __WALL) == child) {
        printf("FAIL vfork, %s: the parent stopped before its vfork child finished "
               "(status %#x)\n", how, status);
        failures_total++;
        if (WIFSTOPPED(status))
            ptrace(PTRACE_CONT, child, 0, 0);
    }
    if (write(gate[1], "x", 1) != 1) {}

    for (;;) {
        if (wait_child(child, &status, __WALL) != child) {
            printf("FAIL vfork, %s: the parent never finished\n", how);
            failures_total++;
            goto kill_child;
        }
        if (WIFSTOPPED(status)) {
            // The interrupt's stop, or the signal the tracer holds back; pass
            // on anything else, such as the child's SIGCHLD.
            long sig = (status >> 16) == 0 && WSTOPSIG(status) != sent ? WSTOPSIG(status) : 0;
            ptrace(PTRACE_CONT, child, 0, (void *) sig);
            continue;
        }
        break;
    }

    struct report rep;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
            read(report_pipe[0], &rep, sizeof rep) != (ssize_t) sizeof rep) {
        printf("FAIL vfork, %s: the parent died: status %#x\n", how, status);
        failures_total++;
        goto cleanup;
    }
    if (rep.started_ms > stop_at) {
        test_logf("  again vfork, %s: the stop landed before the vfork\n", how);
        valid = false;
        goto cleanup;
    }
    if (rep.ret < 0) {
        printf("FAIL vfork, %s: vfork returned before the child finished\n", how);
        failures_total++;
    } else {
        test_logf("  ok   %-16s %-36s completes\n", "vfork", how);
    }
    goto cleanup;

kill_child:
    kill(child, SIGKILL);
    while (waitpid(child, &status, __WALL) == child && !WIFEXITED(status) && !WIFSIGNALED(status))
        ;
cleanup:
    for (int i = 0; i < 2; i++) {
        if (gate[i] >= 0)
            close(gate[i]);
        if (ready[i] >= 0)
            close(ready[i]);
        if (report_pipe[i] >= 0)
            close(report_pipe[i]);
    }
    return valid;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    scale = (long) test_watchdog_secs(1);
    sleep_ms = 700 * scale;
    // No SA_RESTART: the alarm exists to break a wait that would otherwise
    // hang, and a restarted wait hangs just the same.
    install(SIGALRM, on_signal, 0);
    signal(SIGPIPE, SIG_IGN);

    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        int attempts = 0;
        while (!run_row(&rows[i])) {
            if (++attempts == 3) {
                fail_row(&rows[i], "never stopped the call while it was blocked");
                break;
            }
        }
    }
    for (size_t i = 0; i < sizeof vfork_hows / sizeof vfork_hows[0]; i++) {
        int attempts = 0;
        while (!vfork_row(vfork_hows[i])) {
            if (++attempts == 3) {
                printf("FAIL vfork, %s: never stopped the parent inside vfork\n",
                       how_names[vfork_hows[i]]);
                failures_total++;
                break;
            }
        }
    }
    return finish_suite("ptrace_stop_restart");
}
