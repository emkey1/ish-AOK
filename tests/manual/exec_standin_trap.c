// A shell that execs a program which traps a signal: the pid is that program
// on Linux, so a trapped SIGTERM or SIGHUP leaves it running until its trap
// is done, and its status is whatever the trap exits with.
//
// On AOK a native shell's exec is a stand-in that spawns the program and
// waits for it (nlibc_exec_standin, kernel/native_libc.c). It forwarded the
// signal and then died of it itself, its handlers being SIG_DFL, so the pid
// was reported gone at once -- exit 129 or 143 -- while the trap ran on,
// unseen. `su -c 'exec sh start-wayland.sh'` is exactly this, and the Wayland
// session was reported ended while its cleanup still ran (bip, 2026-10-02).
// An untrapped signal must still end it as killed by that signal.
// Values from Linux 6.12 (camd, dash).

#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-60s got=%ld want=%ld\n", label, got, want);
}

// AOK's native dash where there is one -- the shell that has the stand-in,
// and the one start-wayland.sh runs under -- else the system's.
static const char *shell(void) {
    return access("/AOK/native/sh", X_OK) == 0 ? "/AOK/native/sh" : "/bin/sh";
}

static pid_t spawn(const char *script) {
    pid_t pid = fork();
    if (pid == 0) {
        execl(shell(), "sh", "-c", script, (char *) NULL);
        _exit(126);
    }
    return pid;
}

// Sends `sig` once the program is running, then reports whether the pid was
// still there `alive_ms` later, and its wait status.
static void run(const char *label, const char *script, int sig, long alive_ms, int *status,
        int *alive_after) {
    pid_t pid = spawn(script);
    usleep(800000);
    kill(pid, sig);
    usleep(alive_ms * 1000);
    *alive_after = waitpid(pid, status, WNOHANG) == 0;
    if (*alive_after)
        waitpid(pid, status, 0);
    test_logf("%s: status %#x\n", label, *status);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));
    test_logf("shell: %s\n", shell());
    int status, alive;

    run("trapped TERM",
        "exec sh -c 'trap \"sleep 2; exit 0\" TERM; sleep 30 & wait'",
        SIGTERM, 1000, &status, &alive);
    ck("an exec'd program trapping SIGTERM is still running 1 s on", alive, 1);
    ck("  and exits with its trap's status (0)", WIFEXITED(status) && WEXITSTATUS(status) == 0, 1);

    run("trapped HUP",
        "exec sh -c 'trap \"sleep 2; exit 7\" HUP; sleep 30 & wait'",
        SIGHUP, 1000, &status, &alive);
    ck("an exec'd program trapping SIGHUP is still running 1 s on", alive, 1);
    ck("  and exits 7, as its trap says", WIFEXITED(status) ? WEXITSTATUS(status) : -1, 7);

    run("untrapped TERM", "exec sleep 30", SIGTERM, 1000, &status, &alive);
    ck("an exec'd program not trapping SIGTERM is killed by it",
       WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM, 1);

    run("KILL", "exec sh -c 'trap \"sleep 2\" TERM; sleep 30 & wait'",
        SIGKILL, 500, &status, &alive);
    ck("SIGKILL ends it at once", alive, 0);
    ck("  killed by SIGKILL", WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL, 1);

    system("pkill -f 'sleep 30' 2>/dev/null");
    return finish_suite("exec_standin_trap");
}
