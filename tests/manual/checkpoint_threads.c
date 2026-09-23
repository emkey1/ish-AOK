// checkpoint_threads.c -- a multi-threaded process across a checkpoint.
// Driven by checkpoint_threads.sh.
//
// The image had no thread groups: every task was saved as a process, with its
// own copy of what had been one address space, and restored as one. rsyslogd
// came back as four processes on the iPad, and nothing said so -- each thread
// kept running, alone, on memory nobody else could see any more.
//
// So each check below is one a split process cannot pass: the threads must be
// one process (one pid, one /proc/self/task), see each other's writes, hand a
// futex across, share descriptors and a working directory, keep their own
// signal masks, and be joinable -- which needs the exiting thread's clear-tid
// wake to land in the memory the joiner is waiting on.
//
// Two more shapes, by argument:
//
//   departed  the leader calls pthread_exit and a thread runs on through the
//             save. AOK keeps such a leader in the pid table until the last
//             thread goes (Linux keeps it as a zombie); the image had no record
//             of it, so the restore had threads whose process it did not have.
//             The restored thread must still be that process, and its exit
//             status must still reach the parent.
//   zombie    a child that exited and was not yet reaped. The task collection
//             the save used skipped zombies, so every one was lost and the
//             parent's wait() failed with ECHILD after a restore.
//   order     four children that exited, not yet reaped. wait(-1) reaps them
//             oldest first, as Linux does (wait_child_order.c), and has to
//             after a restore: the restore links each child at the end of its
//             parent's list as it builds it, so the image must list siblings
//             in their parent's order, and the save wrote them in whatever
//             order its placement left them.
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void check(const char *what, int ok, const char *detail) {
    printf("%s %s: %s\n", ok ? "OK" : "FAIL", what, detail);
    fflush(stdout);
}

static pid_t tid(void) { return (pid_t) syscall(SYS_gettid); }

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int ticket, served;                  // under mu

static _Atomic int spins, stop;
static _Atomic pid_t spinner_pid, waiter_tid, spinner_tid;
static _Atomic int probe_fd = -1, fd_seen;
static _Atomic int want_cwd;
static char spinner_cwd[256];

// Blocked on a condition variable -- a futex -- across the save, with SIGUSR1
// blocked in this thread alone.
static void *waiter(void *arg) {
    (void) arg;
    waiter_tid = tid();
    sigset_t s;
    sigemptyset(&s);
    sigaddset(&s, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &s, NULL);
    pthread_mutex_lock(&mu);
    while (ticket == 0)
        pthread_cond_wait(&cv, &mu);
    served = ticket * 10;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
    return (void *) 42L;
}

// Running, and sleeping, across the save; answers questions asked of it.
static void *spinner(void *arg) {
    (void) arg;
    spinner_tid = tid();
    while (!stop) {
        spins++;
        spinner_pid = getpid();
        int pfd = probe_fd;
        if (pfd >= 0 && fcntl(pfd, F_GETFD) >= 0)
            fd_seen = 1;
        if (want_cwd) {
            if (getcwd(spinner_cwd, sizeof(spinner_cwd)) == NULL)
                snprintf(spinner_cwd, sizeof(spinner_cwd), "errno %d", errno);
            want_cwd = 0;
        }
        usleep(1000);
    }
    return (void *) 7L;
}

static int count_tasks(void) {
    DIR *d = opendir("/proc/self/task");
    if (d == NULL)
        return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
        if (e->d_name[0] != '.')
            n++;
    closedir(d);
    return n;
}

static unsigned long long sigblk_of(pid_t t) {
    char path[64], line[256];
    snprintf(path, sizeof(path), "/proc/self/task/%d/status", (int) t);
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return ~0ULL;
    unsigned long long v = ~0ULL;
    while (fgets(line, sizeof(line), f) != NULL)
        if (sscanf(line, "SigBlk: %llx", &v) == 1)
            break;
    fclose(f);
    return v;
}

// Wait up to `ms` for *flag to become nonzero.
static int wait_for(_Atomic int *flag, int ms) {
    for (int i = 0; i < ms && !*flag; i++)
        usleep(1000);
    return *flag;
}

static pid_t departed_pid;

static void *departed_worker(void *arg) {
    (void) arg;
    for (int i = 0; i < 7; i++)
        sleep(1);                                   // the checkpoint lands here
    char d[128];
    snprintf(d, sizeof(d), "getpid %d, the leader was %d", (int) getpid(),
             (int) departed_pid);
    check("DEPARTED-SAME-PROCESS", getpid() == departed_pid, d);
    printf("THREADS-DONE\n");
    fflush(stdout);
    exit(3);                                        // the process's status
}

static int departed(void) {
    departed_pid = getpid();
    pthread_t t;
    pthread_create(&t, NULL, departed_worker, NULL);
    pthread_exit(NULL);
}

static int zombie(void) {
    pid_t c = fork();
    if (c == 0)
        _exit(5);
    for (int i = 0; i < 6; i++)
        sleep(1);                                   // the checkpoint lands here
    alarm(10);
    char path[64], stat[256] = {0};
    snprintf(path, sizeof(path), "/proc/%d/stat", (int) c);
    FILE *f = fopen(path, "r");
    char state = '?';
    if (f != NULL) {
        if (fgets(stat, sizeof(stat), f) != NULL) {
            char *rp = strrchr(stat, ')');
            if (rp != NULL && rp[1] == ' ')
                state = rp[2];
        }
        fclose(f);
    }
    int st = 0;
    pid_t w = waitpid(c, &st, 0);
    int e = w < 0 ? errno : 0;
    char d[128];
    snprintf(d, sizeof(d), "state %c; waitpid %d (errno %d) status %#x", state,
             (int) w, e, st);
    check("ZOMBIE-REAPED", state == 'Z' && w == c && WIFEXITED(st) &&
          WEXITSTATUS(st) == 5, d);
    printf("THREADS-DONE\n");
    return 0;
}

static int zombie_order(void) {
    pid_t c[4];
    for (int i = 0; i < 4; i++) {
        c[i] = fork();
        if (c[i] == 0)
            _exit(11 + i);
    }
    for (int i = 0; i < 6; i++)
        sleep(1);                                   // the checkpoint lands here
    alarm(10);
    pid_t got[4] = {0};
    int ok = 1;
    for (int i = 0; i < 4; i++) {
        int st = 0;
        got[i] = waitpid(-1, &st, 0);
        ok = ok && got[i] == c[i] && WIFEXITED(st) && WEXITSTATUS(st) == 11 + i;
    }
    char d[160];
    snprintf(d, sizeof(d), "wait(-1) reaps %d %d %d %d; forked %d %d %d %d",
             (int) got[0], (int) got[1], (int) got[2], (int) got[3],
             (int) c[0], (int) c[1], (int) c[2], (int) c[3]);
    check("ZOMBIE-ORDER", ok, d);
    printf("THREADS-DONE\n");
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "departed") == 0)
        return departed();
    if (argc > 1 && strcmp(argv[1], "zombie") == 0)
        return zombie();
    if (argc > 1 && strcmp(argv[1], "order") == 0)
        return zombie_order();
    pthread_t a, b;
    pthread_create(&a, NULL, waiter, NULL);
    pthread_create(&b, NULL, spinner, NULL);
    for (int i = 0; i < 6; i++)
        sleep(1);                                   // the checkpoint lands here
    alarm(20);                                      // a hang is a failure

    char d[256];
    // 1. One process: one pid from every thread, and three tasks under it.
    usleep(20000);
    int ntask = count_tasks();
    snprintf(d, sizeof(d), "pid %d, spinner sees %d, %d tasks",
             (int) getpid(), (int) spinner_pid, ntask);
    check("ONE-PROCESS", spinner_pid == getpid() && ntask == 3, d);

    // 2. One address space: the spinner's count advances where main can see it.
    int s1 = spins;
    usleep(200000);
    int s2 = spins;
    snprintf(d, sizeof(d), "spins %d -> %d", s1, s2);
    check("SHARED-MEMORY", s2 > s1, d);

    // 3. Per-thread signal masks, where the threads are one process.
    unsigned long long wblk = sigblk_of(waiter_tid), mblk = sigblk_of(tid());
    unsigned long long usr1 = 1ULL << (SIGUSR1 - 1);
    snprintf(d, sizeof(d), "waiter SigBlk %#llx, main %#llx", wblk, mblk);
    check("SIGMASK", wblk != ~0ULL && (wblk & usr1) && mblk != ~0ULL && !(mblk & usr1), d);

    // 4. One descriptor table: a descriptor main opens now, at a number
    // nothing else uses, is open in the spinner too.
    int nul = open("/dev/null", O_RDONLY);
    int fd = dup2(nul, 77);
    close(nul);
    probe_fd = fd;
    int seen = wait_for(&fd_seen, 2000);
    snprintf(d, sizeof(d), "fd %d seen by the spinner: %d", fd, seen);
    check("SHARED-FDS", fd == 77 && seen, d);

    // 5. One working directory.
    chdir("/proc");
    want_cwd = 1;
    for (int i = 0; i < 2000 && want_cwd; i++)
        usleep(1000);
    snprintf(d, sizeof(d), "main chdir /proc, spinner getcwd [%s]", spinner_cwd);
    check("SHARED-CWD", strcmp(spinner_cwd, "/proc") == 0, d);

    // 6. A futex handed across: wake the waiter, and be woken by it.
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += 3;
    pthread_mutex_lock(&mu);
    ticket = 7;
    pthread_cond_broadcast(&cv);
    int rc = 0;
    while (served == 0 && rc == 0)
        rc = pthread_cond_timedwait(&cv, &mu, &until);
    int got = served;
    pthread_mutex_unlock(&mu);
    snprintf(d, sizeof(d), "served %d (rc %d)", got, rc);
    check("FUTEX-HANDOFF", got == 70, d);

    // 7. Joinable: the waiter's exit wakes its clear-tid futex in THIS memory.
    void *ra = NULL, *rb = NULL;
    int ja = pthread_join(a, &ra);
    stop = 1;
    int jb = pthread_join(b, &rb);
    snprintf(d, sizeof(d), "join %d -> %ld, join %d -> %ld", ja, (long) ra, jb, (long) rb);
    check("JOIN", ja == 0 && ra == (void *) 42L && jb == 0 && rb == (void *) 7L, d);
    printf("THREADS-DONE\n");
    return 0;
}
