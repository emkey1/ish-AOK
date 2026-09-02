// ptrace_exit_kill: a traced task with PTRACE_O_TRACEEXIT that receives SIGKILL
// while stopped in its PTRACE_EVENT_EXIT report must die cleanly. The AOK bug:
// do_exit reported EVENT_EXIT, ptrace_stop_common saw the pending SIGKILL and
// called do_exit_group again, which re-reported EVENT_EXIT -> infinite recursion
// -> stack overflow -> whole-emulator crash. With the fix the child dies once.
//
// If the bug is present this CRASHES the ish emulator (not just this process);
// with the fix the child is reaped and the suite prints PASS.

#define _GNU_SOURCE

#include <errno.h>
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
#ifndef PTRACE_O_TRACEEXIT
#define PTRACE_O_TRACEEXIT 0x00000040
#endif
#ifndef PTRACE_EVENT_EXIT
#define PTRACE_EVENT_EXIT 6
#endif
#ifndef __WALL
#define __WALL 0x40000000
#endif

static pid_t g_child;
static void on_alarm(int s) {
    (void)s;
    if (g_child > 0) kill(g_child, SIGKILL);
    static const char m[] = "ptrace_exit_kill: FAIL timeout (recursion/hang in exit-event stop)\n";
    write(2, m, sizeof(m) - 1);
    _exit(1);
}

static void test_exit_kill(void) {
    int p[2];
    if (pipe(p) < 0) { perror("pipe"); failf("ptrace_exit_kill pipe", (uint64_t)errno, 0, 0, 0, 0, 0); return; }

    g_child = fork();
    if (g_child < 0) { perror("fork"); failf("ptrace_exit_kill fork", (uint64_t)errno, 0, 0, 0, 0, 0); return; }
    if (g_child == 0) {
        close(p[1]);
        char c;
        read(p[0], &c, 1);          // wait until parent seized us + set options
        _exit(0);                   // triggers the PTRACE_EVENT_EXIT stop
    }

    pid_t child = g_child;
    close(p[0]);
    if (ptrace(PTRACE_SEIZE, child, 0, (void *)PTRACE_O_TRACEEXIT) != 0) {
        perror("PTRACE_SEIZE");
        kill(child, SIGKILL);
        failf("ptrace_exit_kill seize", (uint64_t)errno, 0, 0, 0, 0, 0);
        return;
    }
    write(p[1], "go", 1);
    close(p[1]);

    int saw_exit_event = 0;
    for (int i = 0; i < 100; i++) {
        int status;
        pid_t w = waitpid(child, &status, __WALL);
        if (w < 0) { if (errno == EINTR) continue; perror("waitpid"); failf("ptrace_exit_kill waitpid", (uint64_t)errno, 0, 0, 0, 0, 0); return; }
        if (WIFSTOPPED(status)) {
            int event = (status >> 16) & 0xff;
            if (event == PTRACE_EVENT_EXIT) {
                saw_exit_event = 1;
                // Inject SIGKILL while the child is parked in its event-exit
                // stop -- this is what drove the recursion.
                kill(child, SIGKILL);
            }
            ptrace(PTRACE_CONT, child, 0, 0);  // may ESRCH if already dying
            continue;
        }
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            test_logf("child terminated (saw_exit_event=%d, %s)\n",
                      saw_exit_event, WIFSIGNALED(status) ? "killed" : "exited");
            return;
        }
    }
    kill(child, SIGKILL);
    failf("ptrace_exit_kill loop", 0, 0, 0, 0, 0, 0);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    signal(SIGALRM, on_alarm);
    alarm(15);
    test_exit_kill();
    return finish_suite("ptrace_exit_kill");
}
