// native_zsh_blocked_sigchld.c -- native zsh's SIGCHLD handler never runs while zsh has SIGCHLD blocked
//
// A native program's handler runs at a syscall checkpoint (kernel/native_libc.c)
// and the shim took every pending signal it held a handler for, whatever the
// program's own mask said. zsh relies on that mask: zwaitjob blocks SIGCHLD and
// then walks the job's list of descriptors, closing each one. A child that
// exited during the walk had its SIGCHLD delivered at one of those closes; the
// handler found the job done and deleted it, freeing the list and its nodes,
// and the walk went on through freed memory. On a device that was a host bad
// access in pipecleanfilelist and the whole app aborted (build 557, Organizer,
// iPhone17,5: three crashes in an hour).
//
// The script makes the window certain rather than lucky. The pipeline sits in
// an `if`, as on the device. Its first stage writes until the pipe is closed,
// so it is still alive when zwaitjob starts and is killed by SIGPIPE during the
// walk, as soon as the shell closes its copy of the read end; the sixty stages
// after it put sixty more closes behind that one; and the last stage is a busy
// loop in the shell itself, so every other child has finished before the walk
// begins. Before the fix this ended the emulator itself (SIGSEGV) in the first
// few iterations on every root tried; after it, every iteration completes.
//
// Skips wherever the native shell is missing, real Linux included.
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_common.h"

#define ZSH "/AOK/native/zsh"
#define ITERATIONS 10
#define STAGES 60

static pid_t child;

static void on_alarm(int sig) {
    (void) sig;
    if (child > 0)
        kill(child, SIGKILL);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    if (access(ZSH, X_OK) != 0) {
        printf("native_zsh_blocked_sigchld: SKIP (no %s)\n", ZSH);
        return 0;
    }

    static char script[8192];
    int n = snprintf(script, sizeof script,
            "for i in {1..%d}; do if { while :; do print -r -- "
            "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx; done; }",
            ITERATIONS);
    for (int i = 0; i < STAGES; i++)
        n += snprintf(script + n, sizeof script - n, " | true");
    snprintf(script + n, sizeof script - n,
            " | { for ((j=0;j<60000;j++)); do :; done; }; then print -n .; fi; done; print done");

    char expect[64];
    memset(expect, '.', ITERATIONS);
    strcpy(expect + ITERATIONS, "done\n");

    int out[2];
    if (pipe(out) != 0) {
        printf("FAIL pipe: %s\n", strerror(errno));
        return 1;
    }
    child = fork();
    if (child == 0) {
        dup2(out[1], 1);
        dup2(out[1], 2);
        close(out[0]);
        execl(ZSH, "zsh", "-f", "-c", script, (char *) NULL);
        _exit(127);
    }
    close(out[1]);
    signal(SIGALRM, on_alarm);
    alarm(test_watchdog_secs(120));

    char got[4096];
    size_t len = 0;
    ssize_t r;
    while (len < sizeof got - 1) {
        r = read(out[0], got + len, sizeof got - 1 - len);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        len += (size_t) r;
    }
    got[len] = '\0';
    int st = 0;
    while (waitpid(child, &st, 0) < 0 && errno == EINTR)
        ;
    alarm(0);

    test_logf("zsh printed \"%s\", status %#x\n", got, st);
    if (strcmp(got, expect) != 0) {
        printf("FAIL zsh printed \"%s\", want \"%s\"\n", got, expect);
        failures_total++;
    }
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        printf("FAIL zsh ended with status %#x%s\n", st,
               WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL ? " (watchdog: it hung)" : "");
        failures_total++;
    }
    return finish_suite("native_zsh_blocked_sigchld");
}
