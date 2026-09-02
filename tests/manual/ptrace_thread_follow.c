// ptrace_thread_follow: a tracer using PTRACE_O_TRACECLONE + wait4(__WALL) must
// be able to follow a CLONE_THREAD worker of its tracee. This is the path
// strace -f uses to follow threads; the AOK do_wait bug skipped non-leader
// ptracees (and ignored __WALL), so a thread's ptrace-stop was never reported
// and the tracer hung in wait4 forever -- freezing any traced multithreaded
// program (e.g. syslog-ng under strace -f).
//
//   child : (after the parent seizes it) pthread_create a worker that sets a
//           flag, join, exit 42 iff the worker actually ran.
//   tracer: SEIZE child with TRACECLONE, loop wait4(-1, __WALL) continuing every
//           stop, and verify the child exits 42 without hanging.

#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ptrace.h>
#include <sys/wait.h>

#include "test_common.h"

#ifndef PTRACE_SEIZE
#define PTRACE_SEIZE 0x4206
#endif
#ifndef PTRACE_O_TRACECLONE
#define PTRACE_O_TRACECLONE 0x00000008
#endif
#ifndef PTRACE_EVENT_CLONE
#define PTRACE_EVENT_CLONE 3
#endif
#ifndef __WALL
#define __WALL 0x40000000
#endif

static pid_t g_child;
static void on_alarm(int s) {
    (void)s;
    if (g_child > 0) kill(g_child, SIGKILL);
    static const char m[] = "ptrace_thread_follow: FAIL timeout (tracer hung in wait4 - thread stop never reported)\n";
    write(2, m, sizeof(m) - 1);
    _exit(1);
}

static volatile int worker_ran;
static void *worker(void *a) { (void)a; worker_ran = 1; return NULL; }

static void test_thread_follow(void) {
    int p[2];
    if (pipe(p) < 0) { perror("pipe"); failf("ptrace_thread_follow pipe", (uint64_t)errno, 0, 0, 0, 0, 0); return; }

    g_child = fork();
    if (g_child < 0) { perror("fork"); failf("ptrace_thread_follow fork", (uint64_t)errno, 0, 0, 0, 0, 0); return; }
    if (g_child == 0) {
        close(p[1]);
        char c;
        read(p[0], &c, 1);                 // wait until the tracer has seized us
        pthread_t t;
        if (pthread_create(&t, NULL, worker, NULL) != 0) _exit(43);
        pthread_join(t, NULL);
        _exit(worker_ran ? 42 : 44);
    }

    pid_t child = g_child;
    close(p[0]);
    if (ptrace(PTRACE_SEIZE, child, 0, (void *)PTRACE_O_TRACECLONE) != 0) {
        perror("PTRACE_SEIZE");
        kill(child, SIGKILL);
        failf("ptrace_thread_follow seize", (uint64_t)errno, 0, 0, 0, 0, 0);
        return;
    }
    test_logf("seized %d with TRACECLONE\n", (int)child);
    write(p[1], "x", 1);                   // release the child
    close(p[1]);

    int saw_clone_event = 0;
    for (int i = 0; i < 200; i++) {
        int status;
        pid_t w = waitpid(-1, &status, __WALL);
        if (w < 0) {
            if (errno == EINTR) continue;
            perror("waitpid");
            failf("ptrace_thread_follow waitpid", (uint64_t)errno, 0, 0, 0, 0, 0);
            return;
        }
        if (WIFEXITED(status)) {
            if (w == child) {
                int code = WEXITSTATUS(status);
                test_logf("child %d exited %d (saw_clone_event=%d)\n", w, code, saw_clone_event);
                if (!saw_clone_event)
                    failf("ptrace_thread_follow clone_event", 0, 0, 0, 1, 0, 0);
                if (code != 42)
                    failf("ptrace_thread_follow exit", (uint64_t)code, 0, 0, 42, 0, 0);
                return;
            }
            test_logf("thread %d exited\n", w);
            continue;
        }
        if (WIFSIGNALED(status)) {
            failf("ptrace_thread_follow killed", (uint64_t)WTERMSIG(status), 0, 0, 0, 0, 0);
            return;
        }
        if (WIFSTOPPED(status)) {
            int sig = WSTOPSIG(status);
            int event = (status >> 16) & 0xff;
            if (event == PTRACE_EVENT_CLONE) saw_clone_event = 1;
            test_logf("stop pid=%d sig=%d event=%d\n", w, sig, event);
            // Continue every stop; deliver only genuine non-trap signals.
            int deliver = (sig == SIGTRAP || event != 0 || sig == SIGSTOP) ? 0 : sig;
            if (ptrace(PTRACE_CONT, w, 0, (void *)(long)deliver) != 0 && errno != ESRCH)
                test_logf("CONT pid=%d errno=%d\n", w, errno);
        }
    }
    kill(child, SIGKILL);
    failf("ptrace_thread_follow loop", 0, 0, 0, 0, 0, 0);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    signal(SIGALRM, on_alarm);
    alarm(15);
    test_thread_follow();
    return finish_suite("ptrace_thread_follow");
}
