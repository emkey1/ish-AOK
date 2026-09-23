// A signal sent to a process group reaches each process in it through a
// thread that can take it.
//
// Linux queues a group signal once for each process, on the process
// (kill_pgrp -> group_send_sig_info -> the shared pending queue), and
// complete_signal gives it to a thread that does not block it. So it reaches
// a process whose main thread has left with pthread_exit while a worker runs
// on -- the leader is a zombie, the worker takes it -- and a process whose
// main thread blocks it while a worker does not.
//
// AOK delivers a process's signal into one thread's own queue, so the kernel
// chooses the thread as it sends. kill(pid) chose well. The group senders did
// not: send_group_signal -- the terminal's ^C, ^\ and ^Z, its hangup, the
// orphaned-group SIGHUP and SIGCONT -- skipped a process whose leader had
// exited, and gave every other process's signal to its leader whatever the
// leader blocked. kill(-pgid) found a live thread for an exited leader but
// still handed a blocking leader the signal, and a window resize's SIGWINCH
// and pidfd_send_signal did neither. The signal went to no thread at all, or
// waited on one that would never take it.
//
// Three shapes of process:
//   leader-exited  the main thread has left with pthread_exit; a worker runs on
//   leader-blocks  the main thread blocks every signal here; the worker does not
//   sigwait        every thread blocks them; the worker takes them with
//                  sigwaitinfo
// each put in a session of its own on a pty for the terminal's senders and
// the rest, and then in an orphaned group beside a stopped member. Every
// signal must be taken by the worker.
//
// Measured against x86_64 and i386 glibc on Linux 6.12.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <pty.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <linux/futex.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#include "test_common.h"

// musl on i386 names the 32-bit-time calls for what they take. The wait below
// passes no timeout, so which one it is does not matter.
#if !defined(SYS_futex) && defined(SYS_futex_time32)
#define SYS_futex SYS_futex_time32
#endif
// Older headers predate both; the numbers are the same on every architecture.
#ifndef SYS_pidfd_send_signal
#define SYS_pidfd_send_signal 424
#endif
#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif

enum shape { LEADER_EXITED, LEADER_BLOCKS, SIGWAIT_WORKER };
static const char *const shape_names[] = { "leader-exited", "leader-blocks", "sigwait" };

// One per signal a victim's thread takes, written by the thread that took it.
struct report {
    int sig;
    int tid;
};

static const int victim_sigs[] = {
    SIGHUP, SIGINT, SIGQUIT, SIGUSR1, SIGUSR2, SIGCONT, SIGTSTP, SIGWINCH,
};
#define NVICTIM_SIGS (sizeof victim_sigs / sizeof victim_sigs[0])

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-62s got=%-6ld want=%ld\n", label, got, want);
}

static void nap(long ms) {
    struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };
    while (nanosleep(&t, &t) < 0 && errno == EINTR)
        continue;
}

// The signals a victim takes. In the sigwait shape SIGCONT is left at its
// default and unblocked, which with nothing stopped discards it on both
// kernels. Both hangup senders send it right behind SIGHUP, and the worker
// takes one signal per sigwaitinfo call, so whether the worker is still in
// the call when SIGCONT is sent is a race. If it has left it, every thread
// blocks SIGCONT and none is asking for it: Linux keeps it on the process for
// the next call, and AOK leaves it with the leader, where no later call of the
// worker's sees it. That gap is recorded in docs/TODO.md, measured with a
// worker that pauses between calls; here it would only make a flake.
static sigset_t taken_set(enum shape shape) {
    sigset_t set;
    sigemptyset(&set);
    for (size_t i = 0; i < NVICTIM_SIGS; i++)
        sigaddset(&set, victim_sigs[i]);
    if (shape == SIGWAIT_WORKER)
        sigdelset(&set, SIGCONT);
    return set;
}

// ---- the victim process ----------------------------------------------------

static int report_fd = -1, ready_fd = -1;
// The main thread's tid word in the leader-exited shape: the kernel clears it,
// and wakes a waiter on it, once the thread has exited.
static volatile int leader_word;

static void report(int sig) {
    struct report r = { sig, (int) syscall(SYS_gettid) };
    ssize_t w = write(report_fd, &r, sizeof r);
    (void) w;
}

static void on_signal(int sig) {
    report(sig);
}

static void *worker_main(void *arg) {
    enum shape shape = (enum shape) (intptr_t) arg;
    sigset_t set = taken_set(shape);
    if (shape == LEADER_BLOCKS)
        pthread_sigmask(SIG_UNBLOCK, &set, NULL);
    if (shape == LEADER_EXITED) {
        // A shared wait: the kernel's wake at a thread's exit is a shared one.
        int v;
        while ((v = __atomic_load_n(&leader_word, __ATOMIC_ACQUIRE)) != 0)
            syscall(SYS_futex, &leader_word, FUTEX_WAIT, v, NULL, NULL, 0);
    }
    int tid = (int) syscall(SYS_gettid);
    ssize_t w = write(ready_fd, &tid, sizeof tid);
    (void) w;
    if (shape == SIGWAIT_WORKER) {
        for (;;) {
            int sig = sigwaitinfo(&set, NULL);
            if (sig > 0)
                report(sig);
        }
    }
    for (;;)
        pause();
    return NULL;
}

// The victim's main thread. Reports go to report_fd; the worker's tid goes to
// ready_fd once everything is in place.
static _Noreturn void victim_main(enum shape shape) {
    sigset_t set = taken_set(shape), all;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigemptyset(&all);
    for (size_t i = 0; i < NVICTIM_SIGS; i++) {
        sigaddset(&all, victim_sigs[i]);
        if (sigismember(&set, victim_sigs[i]))
            sigaction(victim_sigs[i], &sa, NULL);
        else
            signal(victim_sigs[i], SIG_DFL);
    }
    // Whatever the runner left blocked, start from nothing.
    pthread_sigmask(SIG_UNBLOCK, &all, NULL);
    // The worker inherits this; leader-blocks unblocks it again there.
    if (shape != LEADER_EXITED)
        pthread_sigmask(SIG_BLOCK, &set, NULL);
    leader_word = (int) syscall(SYS_gettid);
    pthread_t t;
    if (pthread_create(&t, NULL, worker_main, (void *) (intptr_t) shape) != 0)
        _exit(81);
    if (shape == LEADER_EXITED) {
        // Point the kernel at a word of ours: the one pthread_exit leaves it
        // is libc's, and musl's (its thread-list lock) is not one a worker
        // can wait on. That lock is left held for good, so nothing the worker
        // does afterwards may take it -- no pthread_create, no fork.
        syscall(SYS_set_tid_address, &leader_word);
        pthread_exit(NULL);
    }
    for (;;)
        pause();
}

// ---- the harness -----------------------------------------------------------

// Read exactly `len` bytes within `ms` milliseconds; false on timeout or EOF.
static bool read_within(int fd, void *buf, size_t len, int ms) {
    size_t got = 0;
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    while (got < len) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long left = ms - ((now.tv_sec - start.tv_sec) * 1000L +
                          (now.tv_nsec - start.tv_nsec) / 1000000L);
        if (left <= 0)
            return false;
        struct pollfd p = { .fd = fd, .events = POLLIN };
        int r = poll(&p, 1, (int) left);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return false;
        ssize_t n = read(fd, (char *) buf + got, len - got);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        got += (size_t) n;
    }
    return true;
}

// Thread TID's state letter from /proc, or '?'. The comm field is
// parenthesised and may contain spaces, so it is read from the LAST ')'.
static char thread_state(pid_t pid, pid_t tid) {
    char path[64], buf[512];
    snprintf(path, sizeof path, "/proc/%d/task/%d/stat", (int) pid, (int) tid);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return '?';
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0)
        return '?';
    buf[n] = '\0';
    char *after = strrchr(buf, ')');
    return after != NULL && after[1] == ' ' ? after[2] : '?';
}

static char proc_state(pid_t pid) {
    return thread_state(pid, pid);
}

// A sigwait worker takes a signal every thread blocks only while it is inside
// sigwaitinfo; before that the signal has no thread to go to, and on AOK it
// stays with the leader (see taken_set). Asleep means inside the call: its
// report write never blocks.
static void wait_asleep(pid_t pid, pid_t tid) {
    for (unsigned i = 0; i < test_watchdog_secs(5) * 100 && thread_state(pid, tid) != 'S'; i++)
        nap(10);
}

struct victim {
    enum shape shape;
    pid_t pid;          // the process, and its process group
    pid_t worker;       // the thread that must take every signal
    int reports;        // read end of the victim's report pipe
};

static unsigned wait_ms(void) {
    return test_watchdog_secs(3) * 1000;
}

// One report, of SIG, taken by the worker.
static void expect(struct victim *v, const char *what, int sig) {
    char label[160];
    struct report r = { 0, 0 };
    bool got = read_within(v->reports, &r, sizeof r, (int) wait_ms());
    snprintf(label, sizeof label, "%s: %s reaches the process", shape_names[v->shape], what);
    ck(label, got ? r.sig : 0, sig);
    if (got && r.sig == sig) {
        snprintf(label, sizeof label, "%s:   taken by the worker", shape_names[v->shape]);
        ck(label, r.tid, v->worker);
    }
}

// SIGHUP, and SIGCONT unless the shape discards it, in either order.
static void expect_hup_cont(struct victim *v, const char *what) {
    bool want_cont = v->shape != SIGWAIT_WORKER;
    bool hup = false, cont = false, other = false, stranger = false;
    for (int i = 0; i < (want_cont ? 2 : 1); i++) {
        struct report r;
        if (!read_within(v->reports, &r, sizeof r, (int) wait_ms()))
            break;
        if (r.sig == SIGHUP)
            hup = true;
        else if (r.sig == SIGCONT)
            cont = true;
        else
            other = true;
        if (r.tid != v->worker)
            stranger = true;
    }
    char label[160];
    snprintf(label, sizeof label, "%s: %s: SIGHUP reaches the process", shape_names[v->shape], what);
    ck(label, hup, 1);
    if (want_cont) {
        snprintf(label, sizeof label, "%s: %s: SIGCONT reaches the process", shape_names[v->shape], what);
        ck(label, cont, 1);
    }
    snprintf(label, sizeof label, "%s:   and nothing else arrives", shape_names[v->shape]);
    ck(label, other, 0);
    snprintf(label, sizeof label, "%s:   all taken by the worker", shape_names[v->shape]);
    ck(label, stranger, 0);
}

// Nothing more is on its way.
static void expect_quiet(struct victim *v) {
    struct report r;
    bool got = read_within(v->reports, &r, sizeof r, 300);
    char label[160];
    snprintf(label, sizeof label, "%s: no signal arrives twice", shape_names[v->shape]);
    ck(label, got ? r.sig : 0, 0);
}

static void settle(struct victim *v) {
    if (v->shape == SIGWAIT_WORKER)
        wait_asleep(v->pid, v->worker);
}

static void type_char(struct victim *v, int master, char c) {
    settle(v);
    ssize_t w = write(master, &c, 1);
    (void) w;
}

// Every sender that addresses a process through its terminal, its group or
// its pid, then the hangup.
static void tty_case(enum shape shape) {
    int master, slave;
    if (openpty(&master, &slave, NULL, NULL, NULL) < 0) {
        printf("  %s: tty senders SKIP (openpty: %s)\n", shape_names[shape], strerror(errno));
        return;
    }
    struct termios tio;
    if (tcgetattr(slave, &tio) == 0) {
        tio.c_lflag |= ISIG;
        tio.c_lflag &= ~ECHO;
        tio.c_cc[VINTR] = 003;
        tio.c_cc[VQUIT] = 034;
        tio.c_cc[VSUSP] = 032;
        tcsetattr(slave, TCSANOW, &tio);
    }
    int rep[2], rdy[2];
    if (pipe(rep) < 0) {
        printf("  %s: tty senders SKIP (pipe: %s)\n", shape_names[shape], strerror(errno));
        close(master);
        close(slave);
        return;
    }
    if (pipe(rdy) < 0) {
        printf("  %s: tty senders SKIP (pipe: %s)\n", shape_names[shape], strerror(errno));
        close(master);
        close(slave);
        close(rep[0]);
        close(rep[1]);
        return;
    }
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        close(master);
        close(rep[0]);
        close(rdy[0]);
        report_fd = rep[1];
        ready_fd = rdy[1];
        alarm(test_watchdog_secs(60));
        // Its own session, group and controlling terminal.
        setsid();
        if (ioctl(slave, TIOCSCTTY, 0) < 0)
            _exit(80);
        (void) tcsetpgrp(slave, getpgrp());
        victim_main(shape);
    }
    close(slave);
    close(rep[1]);
    close(rdy[1]);
    if (pid < 0) {
        printf("  %s: tty senders SKIP (fork: %s)\n", shape_names[shape], strerror(errno));
        close(master);
        close(rep[0]);
        close(rdy[0]);
        return;
    }
    // Opened while the leader still lives, so the pidfd names the process.
    int pidfd = (int) syscall(SYS_pidfd_open, pid, 0);
    struct victim v = { .shape = shape, .pid = pid, .reports = rep[0] };
    bool ready = read_within(rdy[0], &v.worker, sizeof v.worker, (int) (wait_ms() * 2));
    close(rdy[0]);
    char label[160];
    if (!ready) {
        int st = 0;
        kill(pid, SIGKILL);
        waitpid(pid, &st, 0);
        if (WIFEXITED(st) && WEXITSTATUS(st) == 80) {
            printf("  %s: tty senders SKIP (TIOCSCTTY unavailable)\n", shape_names[shape]);
        } else {
            snprintf(label, sizeof label, "%s: the victim is ready", shape_names[shape]);
            ck(label, ready, 1);
        }
        close(master);
        close(rep[0]);
        if (pidfd >= 0)
            close(pidfd);
        return;
    }
    snprintf(label, sizeof label, "%s: the victim is ready", shape_names[shape]);
    ck(label, ready, 1);
    if (shape == LEADER_EXITED) {
        // What the whole case is about: only the worker is left.
        snprintf(label, sizeof label, "%s: the worker is not the leader", shape_names[shape]);
        ck(label, v.worker != pid, 1);
    }

    type_char(&v, master, 003);
    expect(&v, "^C", SIGINT);
    type_char(&v, master, 034);
    expect(&v, "^\\", SIGQUIT);
    type_char(&v, master, 032);
    expect(&v, "^Z", SIGTSTP);

    settle(&v);
    struct winsize ws = { .ws_row = 31, .ws_col = 97 };
    ioctl(master, TIOCSWINSZ, &ws);
    expect(&v, "a window resize's SIGWINCH", SIGWINCH);

    settle(&v);
    kill(-pid, SIGUSR1);
    expect(&v, "kill(-pgid)", SIGUSR1);
    settle(&v);
    kill(pid, SIGUSR2);
    expect(&v, "kill(pid)", SIGUSR2);
    if (pidfd >= 0) {
        settle(&v);
        long r = syscall(SYS_pidfd_send_signal, pidfd, SIGUSR1, NULL, 0);
        snprintf(label, sizeof label, "%s: pidfd_send_signal returns 0", shape_names[shape]);
        ck(label, r, 0);
        expect(&v, "pidfd_send_signal", SIGUSR1);
        close(pidfd);
    } else {
        test_logf("  %s: pidfd_open: %s, not tested\n", shape_names[shape], strerror(errno));
    }

    settle(&v);
    close(master);                      // the hangup
    expect_hup_cont(&v, "the terminal's hangup");
    expect_quiet(&v);

    kill(pid, SIGKILL);
    int st;
    waitpid(pid, &st, 0);
    close(rep[0]);
}

// The orphaned-group rule. A middle process makes a group, forks a member
// that stops itself and the victim, and exits: nobody outside the group is
// left in the session who could resume the stopped member, so the group is
// sent SIGHUP and SIGCONT -- and that includes the victim.
static void orphan_case(enum shape shape) {
    int rep[2], info[2];
    if (pipe(rep) < 0 || pipe(info) < 0) {
        printf("  %s: orphaned group SKIP (pipe: %s)\n", shape_names[shape], strerror(errno));
        return;
    }
    fflush(NULL);
    pid_t middle = fork();
    if (middle == 0) {
        close(rep[0]);
        close(info[0]);
        alarm(test_watchdog_secs(60));
        setpgid(0, 0);
        pid_t leaf = fork();
        if (leaf == 0) {
            raise(SIGSTOP);
            _exit(42);
        }
        int st;
        while (waitpid(leaf, &st, WUNTRACED) < 0 && errno == EINTR)
            continue;
        int rdy[2];
        if (pipe(rdy) < 0)
            _exit(1);
        pid_t victim = fork();
        if (victim == 0) {
            close(rdy[0]);
            close(info[1]);
            report_fd = rep[1];
            ready_fd = rdy[1];
            alarm(test_watchdog_secs(60));
            victim_main(shape);
        }
        close(rdy[1]);
        close(rep[1]);
        pid_t worker = 0;
        if (read(rdy[0], &worker, sizeof worker) != (ssize_t) sizeof worker)
            _exit(1);
        if (shape == SIGWAIT_WORKER)
            wait_asleep(victim, worker);
        pid_t msg[3] = { leaf, victim, worker };
        ssize_t w = write(info[1], msg, sizeof msg);
        (void) w;
        _exit(0);                       // this exit orphans the group
    }
    close(rep[1]);
    close(info[1]);
    pid_t msg[3] = { 0, 0, 0 };
    bool got = read_within(info[0], msg, sizeof msg, (int) (wait_ms() * 3));
    close(info[0]);
    int st;
    waitpid(middle, &st, 0);
    char label[160];
    snprintf(label, sizeof label, "%s: orphaned group: the middle process set it up", shape_names[shape]);
    ck(label, got, 1);
    if (!got) {
        close(rep[0]);
        return;
    }
    struct victim v = { .shape = shape, .pid = msg[1], .worker = msg[2], .reports = rep[0] };
    expect_hup_cont(&v, "orphaned group");
    expect_quiet(&v);

    // The positive control: the stopped member was hung up too, so the rule
    // did fire. Without it a missing SIGHUP could mean no SIGHUP was sent.
    pid_t leaf = msg[0];
    for (int i = 0; i < 40 && proc_state(leaf) == 'T'; i++)
        nap(100);
    snprintf(label, sizeof label, "%s:   the stopped member was hung up as well", shape_names[shape]);
    ck(label, proc_state(leaf) == 'T', 0);

    kill(v.pid, SIGKILL);
    kill(leaf, SIGCONT);
    kill(leaf, SIGKILL);
    close(rep[0]);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(240));

    for (int s = LEADER_EXITED; s <= SIGWAIT_WORKER; s++) {
        tty_case((enum shape) s);
        orphan_case((enum shape) s);
    }
    // The orphaned victims and members were handed to init; if that is us,
    // collect them.
    while (waitpid(-1, NULL, WNOHANG) > 0)
        continue;
    return finish_suite("group_signal_target");
}
