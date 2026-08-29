// POSIX timers (timer_create) do not survive execve.
//
// The tgroup outlives an exec, and AOK kept the timer armed on it, so a timer
// set before the exec fired into a program that never created it -- carrying
// the old image's signal number. With SIGALRM's default action that killed the
// new program outright, several seconds after an unrelated exec.
//
// The fork half already worked and is kept here as a guard, together with the
// rule that a process-directed signal may be taken by any thread that has it
// unblocked -- not necessarily the leader.
//
// Measured against x86_64 glibc on Linux 6.12.
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#include "test_common.h"

static char selfpath[256];
static pid_t gettid_(void) { return (pid_t) syscall(SYS_gettid); }
static void nap(long ms) {
    struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&t, NULL);
}

static int arm_timer(timer_t *tid, long secs) {
    struct sigevent sev;
    memset(&sev, 0, sizeof sev);
    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = SIGALRM;
    if (timer_create(CLOCK_MONOTONIC, &sev, tid) < 0)
        return -1;
    struct itimerspec its;
    memset(&its, 0, sizeof its);
    its.it_value.tv_sec = secs;
    return timer_settime(*tid, 0, &its, NULL);
}

// The timer is armed for 1s and then we exec something that runs for 2.5s.
// Linux destroys the timer at execve, so the new image must exit cleanly.
static void case_exec(void) {
    fflush(NULL);
    pid_t c = fork();
    if (c == 0) {
        timer_t tid;
        if (arm_timer(&tid, 1) < 0)
            _exit(70);
        execl(selfpath, selfpath, "sleeper", (char *) NULL);
        _exit(127);
    }
    if (c < 0) { printf("  case_exec: SKIP (fork failed)\n"); return; }
    int st = 0;
    while (waitpid(c, &st, 0) < 0 && errno == EINTR)
        continue;
    int sig = WIFSIGNALED(st) ? WTERMSIG(st) : 0;
    int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    if (code == 70) { printf("  case_exec: SKIP (timer_create unavailable)\n"); return; }
    if (sig != 0 || code != 0)
        failf("timer destroyed by execve", (uint64_t) sig, (uint64_t) code, 0, 0, 0, 0);
    test_logf("  %-40s sig=%d code=%d\n", "timer destroyed by execve", sig, code);
}

static volatile sig_atomic_t alarm_count;
static void onalrm(int s) { (void) s; alarm_count++; }

static void case_fork(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = onalrm;
    sigaction(SIGALRM, &sa, NULL);      // inherited, so the child can observe it

    timer_t tid;
    if (arm_timer(&tid, 1) < 0) { printf("  case_fork: SKIP (timer_create)\n"); return; }

    fflush(NULL);
    pid_t c = fork();
    if (c == 0) {
        alarm_count = 0;
        nap(2000);                      // outlive the parent's timer
        _exit(alarm_count != 0);        // nonzero == the timer came with us
    }
    int st = 0;
    while (waitpid(c, &st, 0) < 0 && errno == EINTR)
        continue;
    int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    if (code != 0)
        failf("timer not inherited by fork", (uint64_t) code, (uint64_t) st, 0, 0, 0, 0);
    test_logf("  %-40s child code=%d, parent fired %d\n",
              "timer not inherited by fork", code, (int) alarm_count);
    timer_delete(tid);
    signal(SIGALRM, SIG_IGN);
}

// A process-directed signal goes to the thread group, so any thread with it
// unblocked may take it -- including one that is not the leader.
static volatile sig_atomic_t handler_tid;
static volatile int worker_ready;
static void onusr1(int s) { (void) s; handler_tid = (sig_atomic_t) gettid_(); }
static void *worker(void *arg) {
    (void) arg;
    // Threads inherit the creator's mask, so unblock explicitly.
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_UNBLOCK, &set, NULL);
    worker_ready = 1;
    for (int i = 0; i < 200 && handler_tid == 0; i++)
        nap(25);
    return NULL;
}

static void case_process_directed(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = onusr1;
    sigaction(SIGUSR1, &sa, NULL);

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &set, NULL);     // the leader cannot take it

    handler_tid = 0;
    worker_ready = 0;
    pthread_t t;
    if (pthread_create(&t, NULL, worker, NULL) != 0) {
        printf("  case_process_directed: SKIP (pthread_create)\n");
        pthread_sigmask(SIG_UNBLOCK, &set, NULL);
        return;
    }
    for (int i = 0; i < 200 && !worker_ready; i++)
        nap(10);
    nap(100);
    kill(getpid(), SIGUSR1);
    for (int i = 0; i < 200 && handler_tid == 0; i++)
        nap(25);
    pthread_join(t, NULL);

    int leader = (int) gettid_();
    if (handler_tid == 0 || handler_tid == leader)
        failf("process-directed signal taken by an unblocked thread",
              (uint64_t) handler_tid, (uint64_t) leader, 0, 0, 0, 0);
    test_logf("  %-40s ran on tid=%d, leader=%d\n",
              "process-directed to an unblocked thread", (int) handler_tid, leader);
    pthread_sigmask(SIG_UNBLOCK, &set, NULL);
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "sleeper") == 0) {
        nap(2500);
        _exit(0);
    }
    test_init(argc, argv);
    alarm(test_watchdog_secs(120));
    if (readlink("/proc/self/exe", selfpath, sizeof selfpath - 1) <= 0)
        snprintf(selfpath, sizeof selfpath, "%s", argv[0]);

    // timer_create(2): a NULL sigevent means SIGEV_SIGNAL/SIGALRM with the
    // timer id as sival_int. AOK read the struct unconditionally, so the
    // documented default faulted and returned EFAULT.
    {
        timer_t tid;
        errno = 0;
        int r = timer_create(CLOCK_MONOTONIC, NULL, &tid);
        if (r != 0)
            failf("timer_create(NULL sigevent)", (uint64_t) errno, 0, 0, 0, 0, 0);
        else
            timer_delete(tid);
        test_logf("  %-40s rc=%d errno=%d\n", "timer_create(NULL sigevent)", r,
                  r < 0 ? errno : 0);
    }

    case_exec();
    case_fork();
    case_process_directed();
    return finish_suite("posix_timer_exec");
}
