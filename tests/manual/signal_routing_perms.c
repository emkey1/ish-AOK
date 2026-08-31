// Where a queued signal goes, who is allowed to send one, and what signalfd
// says about it.
//
//   sigqueue()/rt_sigqueueinfo was delivered thread-directed, straight into
//   the resolved task's private queue. Linux routes it through
//   kill_proc_info, i.e. process-directed, so any thread of the target that
//   can take the signal is a legitimate destination -- and a sibling parked in
//   sigwait()/sigtimedwait() is the whole reason a program uses sigqueue at
//   all. That thread sat out its full timeout while the signal waited
//   undeliverable beside it.
//
//   The kill permission check had the credential rule and not the exception:
//   Linux allows SIGCONT to any process in the SAME SESSION whatever its
//   credentials. That exception is what job control is built on -- a shell
//   that started a privileged job keeps the stopped process in its session but
//   not under its uid, so without it `fg` could not resume anything that had
//   gone through sudo, and kill_group inherited the same refusal for the whole
//   process group.
//
//   signalfd copied every member of the siginfo UNION for every signal, so
//   fields outside the signal's own layout carried whatever that layout
//   happened to have stored there -- a sigqueue'd SIGUSR1 arrived with
//   ssi_status, ssi_addr and ssi_tid holding pieces of its own sigval. ssi_fd
//   was the constant -1, a value Linux produces for nothing.
//
// Measured against x86_64 glibc on Linux 6.12.
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_common.h"

#define UNPRIV_UID 1000
#define UNPRIV_GID 1000

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-56s got=%-8ld want=%ld\n", label, got, want);
}

#define IN_CHILD(...) do {                                                     \
        fflush(NULL);                                                          \
        pid_t c_ = fork();                                                     \
        if (c_ == 0) {                                                         \
            failures_total = 0;                                                \
            __VA_ARGS__;                                                       \
            fflush(NULL);                                                      \
            _exit(failures_total > 250 ? 250 : (int) failures_total);          \
        }                                                                      \
        int st_;                                                               \
        if (waitpid(c_, &st_, 0) != c_) { failures_total++; break; }           \
        if (WIFSIGNALED(st_)) {                                                \
            printf("FAIL child died on signal %d\n", WTERMSIG(st_));           \
            failures_total++;                                                  \
        } else                                                                 \
            failures_total += (unsigned) WEXITSTATUS(st_);                     \
    } while (0)

// ---- sigqueue reaches a sibling parked in sigtimedwait --------------------
static sigset_t waitset;
static volatile int got_sig, got_code, got_val;
static void *waiter(void *arg) {
    siginfo_t info;
    struct timespec ts = { 5, 0 };
    int r = sigtimedwait(&waitset, &info, &ts);
    if (r > 0) {
        got_sig = info.si_signo;
        got_code = info.si_code;
        got_val = info.si_value.sival_int;
    } else {
        got_sig = -errno;
    }
    return arg;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(90));

    // ---- process-directed queueing ---------------------------------------
    {
        sigemptyset(&waitset);
        sigaddset(&waitset, SIGUSR2);
        ck("block SIGUSR2 process-wide", pthread_sigmask(SIG_BLOCK, &waitset, NULL), 0);
        pthread_t t;
        ck("start the waiting sibling", pthread_create(&t, NULL, waiter, NULL), 0);
        usleep(200000);
        union sigval v;
        v.sival_int = 42;
        ck("sigqueue to our own pid", sigqueue(getpid(), SIGUSR2, v), 0);
        pthread_join(t, NULL);
        // -EAGAIN here is the sibling having timed out: the signal went into
        // some other thread's private queue and was never deliverable to it.
        ck("  the sibling in sigtimedwait received it", got_sig, SIGUSR2);
        ck("  with si_code SI_QUEUE", got_code, SI_QUEUE);
        ck("  and the value that was sent", got_val, 42);
        pthread_sigmask(SIG_UNBLOCK, &waitset, NULL);
    }

    // ---- signalfd reports only the signal's own layout ---------------------
    {
        sigset_t m;
        sigemptyset(&m);
        sigaddset(&m, SIGUSR1);
        sigprocmask(SIG_BLOCK, &m, NULL);
        int fd = signalfd(-1, &m, 0);
        ck("signalfd", fd >= 0, 1);
        if (fd >= 0) {
            union sigval v;
            v.sival_int = 7;
            ck("sigqueue SIGUSR1", sigqueue(getpid(), SIGUSR1, v), 0);
            struct signalfd_siginfo si;
            // Poison first: the bug was fields left holding union aliases, and
            // a zeroed buffer would pass whether they were written or not.
            memset(&si, 0xAA, sizeof si);
            ck("  read one record", (long) read(fd, &si, sizeof si), (long) sizeof si);
            ck("  ssi_signo", si.ssi_signo, SIGUSR1);
            ck("  ssi_code is SI_QUEUE", si.ssi_code, SI_QUEUE);
            ck("  ssi_int carries the value", si.ssi_int, 7);
            ck("  ssi_pid is the sender", (long) si.ssi_pid, (long) getpid());
            // Everything below is outside SI_QUEUE's layout and must be
            // untouched -- in particular ssi_fd, which used to be a hardcoded
            // -1 for every signal.
            ck("  ssi_fd is not set for a non-SIGPOLL signal", (long) si.ssi_fd, 0);
            ck("  ssi_status is not set", (long) si.ssi_status, 0);
            ck("  ssi_addr is not set", (long) si.ssi_addr, 0);
            ck("  ssi_tid is not set", (long) si.ssi_tid, 0);
            ck("  ssi_utime is not set", (long) si.ssi_utime, 0);
            close(fd);
        }
        sigprocmask(SIG_UNBLOCK, &m, NULL);
    }

    // ...and a SIGCHLD arriving on a signalfd fills the CHILD arm, which is a
    // different one -- so this catches a "fix" that just zeroed everything.
    {
        sigset_t m;
        sigemptyset(&m);
        sigaddset(&m, SIGCHLD);
        sigprocmask(SIG_BLOCK, &m, NULL);
        int fd = signalfd(-1, &m, 0);
        if (fd >= 0) {
            fflush(NULL);
            pid_t c = fork();
            if (c == 0)
                _exit(9);
            struct signalfd_siginfo si;
            memset(&si, 0xAA, sizeof si);
            errno = 0;
            ssize_t n = read(fd, &si, sizeof si);
            if (n != (ssize_t) sizeof si)
                printf("  (SIGCHLD read returned %zd, errno %d: %s)\n",
                       n, errno, strerror(errno));
            ck("SIGCHLD record read", n == (ssize_t) sizeof si, 1);
            ck("  ssi_signo", si.ssi_signo, SIGCHLD);
            ck("  ssi_pid is the child", (long) si.ssi_pid, (long) c);
            ck("  ssi_status is the exit code", (long) si.ssi_status, 9);
            // ...while the RT arm's members stay clear.
            ck("  ssi_addr is not set for SIGCHLD", (long) si.ssi_addr, 0);
            ck("  ssi_fd is not set for SIGCHLD", (long) si.ssi_fd, 0);
            int st;
            waitpid(c, &st, 0);
            close(fd);
        }
        sigprocmask(SIG_UNBLOCK, &m, NULL);
        signal(SIGCHLD, SIG_DFL);
    }

    // ---- SIGCONT crosses the credential line inside a session -------------
    if (geteuid() == 0) {
        fflush(NULL);
        pid_t target = fork();
        if (target == 0) {
            raise(SIGSTOP);
            pause();
            _exit(0);
        }
        usleep(300000);
        IN_CHILD({
            ck("drop privilege", setgid(UNPRIV_GID) == 0 && setuid(UNPRIV_UID) == 0, 1);
            ck("same session as the target", getsid(0) == getsid(target), 1);
            errno = 0;
            int rc = kill(target, SIGCONT);
            ck("SIGCONT to a root-owned same-session target", rc, 0);
            // ...and the exception really is only SIGCONT.
            errno = 0;
            rc = kill(target, SIGTERM);
            ck("SIGTERM to the same target is refused", rc < 0 ? 1 : 0, 1);
            ck("  ...with EPERM", rc < 0 ? errno : 0, EPERM);
        });
        kill(target, SIGCONT);
        kill(target, SIGKILL);
        int st;
        waitpid(target, &st, 0);
    } else {
        test_logf("  %-56s (needs root to own the target)\n", "SIGCONT session exception");
    }

    return finish_suite("signal_routing_perms");
}
