// Core signal delivery: kill to self with its siginfo, sigpending and
// sigtimedwait, signalfd, sigsuspend resuming, and tgkill reaching the thread
// it names.
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

static volatile sig_atomic_t handler_count;
static volatile sig_atomic_t last_sig;
static volatile sig_atomic_t last_code;
static volatile sig_atomic_t last_pid;
static volatile sig_atomic_t last_tid;

static pid_t gettid_linux(void) {
    return (pid_t) syscall(SYS_gettid);
}

static void send_thread_signal(pid_t tid, int sig) {
    if (syscall(SYS_tgkill, getpid(), tid, sig) != 0) {
        perror("tgkill");
        exit(1);
    }
}

static void record_handler(int sig, siginfo_t *info, void *ucontext) {
    (void) ucontext;
    handler_count++;
    last_sig = sig;
    last_code = info != NULL ? info->si_code : 0;
    last_pid = info != NULL ? info->si_pid : 0;
    last_tid = gettid_linux();
}

static void install_siginfo_handler(int sig) {
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_sigaction = record_handler;
    act.sa_flags = SA_SIGINFO;
    sigemptyset(&act.sa_mask);
    if (sigaction(sig, &act, NULL) != 0) {
        perror("sigaction");
        exit(1);
    }
}

static void reset_handler_state(void) {
    handler_count = 0;
    last_sig = 0;
    last_code = 0;
    last_pid = 0;
    last_tid = 0;
}

static int wait_for_handler_count(int want, int timeout_ms) {
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000};
    for (int i = 0; i < timeout_ms; i++) {
        if (handler_count >= want)
            return 0;
        nanosleep(&ts, NULL);
    }
    return -1;
}

static void test_kill_self_siginfo(void) {
    reset_handler_state();
    install_siginfo_handler(SIGUSR1);
    if (kill(getpid(), SIGUSR1) != 0) {
        perror("kill");
        exit(1);
    }

    test_logf("signal kill self: count=%d sig=%d code=%d pid=%d tid=%d\n",
              handler_count, last_sig, last_code, last_pid, last_tid);

    if (handler_count != 1 || last_sig != SIGUSR1 || last_pid != getpid())
        failf("signal kill self", handler_count, last_sig, last_pid, 1, SIGUSR1, getpid());
}

static void test_sigpending_sigtimedwait(void) {
    sigset_t set;
    sigset_t oldset;
    sigset_t pending;
    struct timespec timeout = {.tv_sec = 1, .tv_nsec = 0};
    siginfo_t info;
    memset(&info, 0, sizeof(info));

    sigemptyset(&set);
    sigaddset(&set, SIGUSR2);
    if (pthread_sigmask(SIG_BLOCK, &set, &oldset) != 0) {
        perror("pthread_sigmask");
        exit(1);
    }
    if (kill(getpid(), SIGUSR2) != 0) {
        perror("kill");
        exit(1);
    }
    if (sigpending(&pending) != 0) {
        perror("sigpending");
        exit(1);
    }
    test_logf("signal pending: sigusr2=%d\n", sigismember(&pending, SIGUSR2));
    if (sigismember(&pending, SIGUSR2) != 1)
        failf("signal pending", sigismember(&pending, SIGUSR2), 0, 0, 1, 0, 0);

    errno = 0;
    int sig = sigtimedwait(&set, &info, &timeout);
    test_logf("signal sigtimedwait: sig=%d code=%d pid=%d\n", sig, info.si_code, info.si_pid);
    if (sig != SIGUSR2 || info.si_pid != getpid())
        failf("signal sigtimedwait", sig, info.si_pid, 0, SIGUSR2, getpid(), 0);

    if (pthread_sigmask(SIG_SETMASK, &oldset, NULL) != 0) {
        perror("pthread_sigmask restore");
        exit(1);
    }
}

static void test_signalfd_self_signal(void) {
    sigset_t set;
    sigset_t oldset;
    struct signalfd_siginfo fdinfo;
    int fd;
    ssize_t nread;

    sigemptyset(&set);
    sigaddset(&set, SIGWINCH);
    if (pthread_sigmask(SIG_BLOCK, &set, &oldset) != 0) {
        perror("pthread_sigmask");
        exit(1);
    }

    fd = signalfd(-1, &set, 0);
    if (fd < 0) {
        perror("signalfd");
        exit(1);
    }
    if (kill(getpid(), SIGWINCH) != 0) {
        perror("kill");
        exit(1);
    }

    memset(&fdinfo, 0, sizeof(fdinfo));
    nread = read(fd, &fdinfo, sizeof(fdinfo));
    test_logf("signal signalfd: nread=%zd signo=%u pid=%u\n",
              nread, fdinfo.ssi_signo, fdinfo.ssi_pid);

    if (nread != (ssize_t) sizeof(fdinfo) || fdinfo.ssi_signo != SIGWINCH ||
        fdinfo.ssi_pid != (uint32_t) getpid()) {
        failf("signal signalfd", (uint64_t) nread, fdinfo.ssi_signo, fdinfo.ssi_pid,
              sizeof(fdinfo), SIGWINCH, getpid());
    }

    close(fd);
    if (pthread_sigmask(SIG_SETMASK, &oldset, NULL) != 0) {
        perror("pthread_sigmask restore");
        exit(1);
    }
}

static void *delayed_process_signal(void *arg) {
    int sig = *(int *) arg;
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 20000000};
    nanosleep(&ts, NULL);
    kill(getpid(), sig);
    return NULL;
}

static void test_sigsuspend_resume(void) {
    sigset_t blockset;
    sigset_t oldset;
    sigset_t suspend_mask;
    pthread_t thread;
    int sig = SIGUSR1;

    reset_handler_state();
    install_siginfo_handler(SIGUSR1);

    sigemptyset(&blockset);
    sigaddset(&blockset, SIGUSR1);
    if (pthread_sigmask(SIG_BLOCK, &blockset, &oldset) != 0) {
        perror("pthread_sigmask");
        exit(1);
    }
    if (pthread_create(&thread, NULL, delayed_process_signal, &sig) != 0) {
        perror("pthread_create");
        exit(1);
    }

    suspend_mask = oldset;
    sigdelset(&suspend_mask, SIGUSR1);
    errno = 0;
    int rc = sigsuspend(&suspend_mask);
    int err = errno;
    pthread_join(thread, NULL);

    test_logf("signal sigsuspend: rc=%d errno=%d count=%d sig=%d\n",
              rc, err, handler_count, last_sig);

    if (rc != -1 || err != EINTR || handler_count != 1 || last_sig != SIGUSR1)
        failf("signal sigsuspend", rc, err, handler_count, -1, EINTR, 1);

    if (pthread_sigmask(SIG_SETMASK, &oldset, NULL) != 0) {
        perror("pthread_sigmask restore");
        exit(1);
    }
}

static volatile sig_atomic_t worker_ready;
static volatile sig_atomic_t worker_done;
static volatile sig_atomic_t worker_tid;

static void *thread_signal_target(void *unused) {
    sigset_t set;
    (void) unused;

    worker_tid = gettid_linux();
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_UNBLOCK, &set, NULL);
    worker_ready = 1;

    while (!worker_done) {
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000};
        nanosleep(&ts, NULL);
    }
    return NULL;
}

static void test_tgkill_targeted_delivery(void) {
    sigset_t set;
    sigset_t oldset;
    pthread_t thread;

    reset_handler_state();
    worker_ready = 0;
    worker_done = 0;
    worker_tid = 0;
    install_siginfo_handler(SIGUSR1);

    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    if (pthread_sigmask(SIG_BLOCK, &set, &oldset) != 0) {
        perror("pthread_sigmask");
        exit(1);
    }
    if (pthread_create(&thread, NULL, thread_signal_target, NULL) != 0) {
        perror("pthread_create");
        exit(1);
    }

    for (int i = 0; i < 1000 && !worker_ready; i++) {
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000};
        nanosleep(&ts, NULL);
    }
    if (!worker_ready) {
        failf("signal tgkill ready", 0, 0, 0, 1, 0, 0);
        worker_done = 1;
        pthread_join(thread, NULL);
        pthread_sigmask(SIG_SETMASK, &oldset, NULL);
        return;
    }

    send_thread_signal(worker_tid, SIGUSR1);
    if (wait_for_handler_count(1, 1000) != 0)
        failf("signal tgkill timeout", handler_count, 0, 0, 1, 0, 0);

    test_logf("signal tgkill: count=%d sig=%d handler_tid=%d target_tid=%d\n",
              handler_count, last_sig, last_tid, worker_tid);

    if (handler_count != 1 || last_sig != SIGUSR1 || last_tid != worker_tid)
        failf("signal tgkill", handler_count, last_sig, last_tid, 1, SIGUSR1, worker_tid);

    worker_done = 1;
    pthread_join(thread, NULL);
    if (pthread_sigmask(SIG_SETMASK, &oldset, NULL) != 0) {
        perror("pthread_sigmask restore");
        exit(1);
    }
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    test_kill_self_siginfo();
    test_sigpending_sigtimedwait();
    test_signalfd_self_signal();
    test_sigsuspend_resume();
    test_tgkill_targeted_delivery();
    return finish_suite("signal_core");
}
