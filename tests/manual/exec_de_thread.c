// execve from a non-leader thread (Linux's de_thread).
//
// POSIX says the new image starts single-threaded, and Linux delivers that by
// killing every other thread and handing the exec'ing thread the leader's pid.
// AOK did neither: three tasks survived where Linux has one, each still
// running the OLD program (exec swaps only the calling task's mm, so the
// siblings keep the previous address space alive by reference), and the
// process was left with getpid() != gettid() -- a state Linux only ever shows
// for a thread that is NOT the leader, and the standard way a program asks
// "am I the main thread".
//
// The parent-visible half matters just as much: the process keeps its pid, so
// wait() must still find it, kill() must still reach it, and the exit status
// must still come back. AOK's threads are children of their creator rather
// than of the leader's parent, so without an explicit fixup the exec'ing
// thread inherited itself as its own parent and the real parent's wait()
// returned ECHILD.
//
// Every expectation was measured against x86_64 glibc on Linux 6.12.
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#include "test_common.h"

static char selfpath[256];
static pid_t gettid_(void) { return (pid_t) syscall(SYS_gettid); }
static void nap(long ms) {
    struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&t, NULL);
}
static void check(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-46s got=%ld want=%ld\n", label, got, want);
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

// Threads the exec has to get rid of: one parked in a read that never
// completes, one busy in guest code with no syscalls to be interrupted at,
// and one sleeping. Each is a different way to be hard to stop.
static void *blocked_thread(void *arg) {
    int *fd = arg;
    char b;
    if (read(fd[0], &b, 1) < 0) {}
    return NULL;
}
static void *spinning_thread(void *arg) {
    (void) arg;
    volatile double a = 1.0;
    for (long i = 0; i < 2000000000L; i++)
        a = a * 1.0000001 + 0.5;
    return NULL;
}
static void *sleeping_thread(void *arg) {
    (void) arg;
    for (int i = 0; i < 400; i++)
        nap(50);
    return NULL;
}
static void *exec_thread(void *arg) {
    execl(selfpath, selfpath, (char *) arg, (char *) NULL);
    _exit(127);
    return NULL;
}

// Fork a child whose NON-leader thread execs `mode`, with three siblings
// running alongside it.
static pid_t spawn(const char *mode) {
    fflush(NULL);
    pid_t c = fork();
    if (c != 0)
        return c;
    int fd[2];
    if (pipe(fd) < 0)
        _exit(90);
    pthread_t a, b, d, e;
    pthread_create(&a, NULL, blocked_thread, fd);
    pthread_create(&b, NULL, spinning_thread, NULL);
    pthread_create(&d, NULL, sleeping_thread, NULL);
    nap(20);
    pthread_create(&e, NULL, exec_thread, (void *) mode);
    for (;;)
        nap(20);
}

// iSH-AOK's native dispatch (kernel/native.h) runs a program as host code in
// place of the image the ELF loader would have mapped, and it used to return
// from __do_execve before reaching any of this -- so a native exec killed no
// siblings and swapped no address space. That was survivable only while the
// surviving threads went on sharing the exec'ing thread's mm; once a native
// exec gets an address space of its own, a thread group straddling two of them
// is not a state it can be in. Skipped where /AOK/native does not exist, which
// is everywhere but here.
#define NATIVE_SMALLCLUE "/AOK/native/smallclue"

static void *exec_native_thread(void *arg) {
    (void) arg;
    execl(NATIVE_SMALLCLUE, "cat", (char *) NULL);
    _exit(127);
    return NULL;
}

// Same shape as spawn(), except the exec'ing thread runs a native program and
// the child's stdin is a pipe nobody writes to, so it stays there to be looked
// at. *keep_open is the write end, which the caller closes after reaping.
static pid_t spawn_native(int *keep_open) {
    int stdin_pipe[2];
    if (pipe(stdin_pipe) < 0)
        return -1;
    fflush(NULL);
    pid_t c = fork();
    if (c < 0) {
        close(stdin_pipe[0]); close(stdin_pipe[1]);
        return -1;
    }
    if (c != 0) {
        close(stdin_pipe[0]);
        *keep_open = stdin_pipe[1];
        return c;
    }
    close(stdin_pipe[1]);
    dup2(stdin_pipe[0], STDIN_FILENO);
    if (stdin_pipe[0] != STDIN_FILENO)
        close(stdin_pipe[0]);
    int fd[2];
    if (pipe(fd) < 0)
        _exit(90);
    pthread_t a, b, d, e;
    pthread_create(&a, NULL, blocked_thread, fd);
    pthread_create(&b, NULL, spinning_thread, NULL);
    pthread_create(&d, NULL, sleeping_thread, NULL);
    nap(20);
    pthread_create(&e, NULL, exec_native_thread, NULL);
    for (;;)
        nap(20);
}

// Threads of another process, from its /proc entry. -1 if it cannot be read.
static int count_tasks_of(pid_t pid) {
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/task", (int) pid);
    DIR *d = opendir(path);
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

static int comm_is(pid_t pid, const char *want) {
    char path[64], comm[64];
    snprintf(path, sizeof path, "/proc/%d/comm", (int) pid);
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return 0;
    if (fgets(comm, sizeof comm, f) == NULL)
        comm[0] = '\0';
    fclose(f);
    size_t n = strlen(comm);
    while (n > 0 && (comm[n - 1] == '\n' || comm[n - 1] == ' '))
        comm[--n] = '\0';
    return strcmp(comm, want) == 0;
}

static pid_t reap(pid_t c, int *st) {
    pid_t w;
    while ((w = waitpid(c, st, 0)) < 0 && errno == EINTR)
        continue;
    return w;
}

int main(int argc, char **argv) {
    // Re-exec'd modes. "report" encodes what the new image sees into its exit
    // status, since it has no other channel back to the test.
    if (argc > 1 && strcmp(argv[1], "report") == 0) {
        nap(200);                       // let any survivor show itself
        int single = count_tasks() == 1;
        int leaderish = getpid() == gettid_();
        _exit((single ? 0 : 1) | (leaderish ? 0 : 2));
    }
    if (argc > 1 && strcmp(argv[1], "exit42") == 0) { nap(200); _exit(42); }
    if (argc > 1 && strcmp(argv[1], "sleep") == 0)  { for (;;) nap(100); }
    if (argc > 1 && strcmp(argv[1], "forker") == 0) {
        pid_t g = fork();
        if (g == 0) _exit(7);
        int st = 0;
        pid_t w = reap(g, &st);
        _exit((w == g && WIFEXITED(st) && WEXITSTATUS(st) == 7) ? 0 : 1);
    }

    test_init(argc, argv);
    alarm(test_watchdog_secs(300));
    if (readlink("/proc/self/exe", selfpath, sizeof selfpath - 1) <= 0)
        snprintf(selfpath, sizeof selfpath, "%s", argv[0]);

    // 1. The new image is single-threaded and is the leader.
    {
        pid_t c = spawn("report");
        int st = 0;
        pid_t w = reap(c, &st);
        int code = (w == c && WIFEXITED(st)) ? WEXITSTATUS(st) : -1;
        check("new image sees exactly one task", (code & 1) == 0, 1);
        check("new image has getpid() == gettid()", (code & 2) == 0, 1);
    }

    // 2. The process keeps its pid, so the parent still reaps it there.
    {
        pid_t c = spawn("exit42");
        int st = 0;
        pid_t w = reap(c, &st);
        check("wait() returns the pid we forked", w == c, 1);
        check("exit status survives the swap", WIFEXITED(st) ? WEXITSTATUS(st) : -1, 42);
    }

    // 3. ...and is still reachable by that pid.
    {
        pid_t c = spawn("sleep");
        nap(1500);
        errno = 0;
        check("kill() on the original pid", kill(c, SIGTERM), 0);
        int st = 0;
        pid_t w = reap(c, &st);
        check("wait() still finds it", w == c, 1);
        check("it died of the signal", WIFSIGNALED(st) ? WTERMSIG(st) : -1, SIGTERM);
    }

    // 4. The re-identified process can still fork and reap.
    {
        pid_t c = spawn("forker");
        int st = 0;
        reap(c, &st);
        check("the new image can fork and reap", WIFEXITED(st) ? WEXITSTATUS(st) : -1, 0);
    }

    // 5. Repeated, so a leaked or wrongly reused pid shows up.
    {
        int wrong = 0;
        for (int i = 0; i < 8; i++) {
            pid_t c = spawn("exit42");
            int st = 0;
            pid_t w = reap(c, &st);
            if (w != c || !WIFEXITED(st) || WEXITSTATUS(st) != 42)
                wrong++;
        }
        check("8 more non-leader execs, all consistent", wrong, 0);
    }

    // 6. The same, for a program iSH-AOK runs as host code rather than loading.
    {
        struct stat st_native;
        if (stat(NATIVE_SMALLCLUE, &st_native) != 0) {
            test_logf("  no %s here, native half skipped\n", NATIVE_SMALLCLUE);
        } else {
            int keep_open = -1;
            pid_t c = spawn_native(&keep_open);
            if (c < 0) {
                test_logf("  could not fork for the native exec, skipped\n");
            } else {
                // comm under the process's own pid is the check, not just a
                // wait for readiness: it only becomes the applet's name if the
                // exec'ing thread took over the leader's identity. Without
                // de_thread the leader is still the old image and this never
                // changes, which is the failure, not a reason to skip.
                int landed = 0;
                for (int i = 0; i < 500 && !landed; i++) {
                    landed = comm_is(c, "cat");
                    if (!landed)
                        nap(20);
                }
                check("native exec takes over the leader's identity", landed, 1);
                if (landed) {
                    nap(200);   // give any survivor time to show itself
                    check("native exec leaves exactly one task", count_tasks_of(c), 1);
                }
                kill(c, SIGKILL);
                int st = 0;
                pid_t w = reap(c, &st);
                check("wait() finds the native exec'd pid", w == c, 1);
                close(keep_open);
            }
        }
    }

    return finish_suite("exec_de_thread");
}
