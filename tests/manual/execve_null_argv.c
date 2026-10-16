// execve with argv == NULL runs the program, as Linux runs it, instead of
// failing with EFAULT.
//
// Linux's count() takes a NULL argv as an empty one; 5.10 then starts the
// program with argc 0, and 5.18 and later with argv {""} (argc 1, and a
// warning in the kernel log). AOK already runs an empty argv the 5.18 way,
// so a NULL one must come out the same: argc 1 and an empty argv[0]. The
// positive control is the same exec with argv = {NULL}, the empty array
// itself, which has always worked.
//
// The child is this program again, told apart by its empty argv[0]: it
// exits 0 when it sees argc 1 and argv[0] "", 4 for argc 0 (the 5.10 way),
// and 3 otherwise. A NULL envp goes with the NULL argv once more, since a
// NULL envp was already an empty environment.
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include "test_common.h"

static int run(const char *label, char *const *argv, char *const *envp) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(1);
    }
    if (pid == 0) {
        // The raw system call: libc wrappers may refuse a NULL argv first.
        syscall(SYS_execve, "/proc/self/exe", argv, envp);
        _exit(100 + (errno & 0x1f));
    }
    int status;
    if (waitpid(pid, &status, 0) != pid) {
        perror("waitpid");
        exit(1);
    }
    if (!WIFEXITED(status)) {
        printf("FAIL %s: child killed by signal %d\n", label, WTERMSIG(status));
        return 1;
    }
    int code = WEXITSTATUS(status);
    if (code == 0) {
        printf("ok %s\n", label);
        return 0;
    }
    if (code >= 100)
        printf("FAIL %s: execve failed, errno %d\n", label, code - 100);
    else
        printf("FAIL %s: child saw the wrong argv (exit %d)\n", label, code);
    return 1;
}

int main(int argc, char **argv) {
    // The parent always has a name; a child run from an empty argv has none.
    if (argc == 0)
        return 4;   // the 5.10 way: argc 0
    if (argv[0][0] == '\0')
        return argc == 1 && argv[1] == NULL ? 0 : 3;
    char *env[] = {"NULL_ARGV_CHILD=1", NULL};
    char *empty[] = {NULL};
    int failures = 0;
    failures += run("empty argv (control)", empty, env);
    failures += run("NULL argv", NULL, env);
    failures += run("NULL argv and NULL envp", NULL, NULL);
    if (failures) {
        printf("execve_null_argv: FAIL\n");
        return 1;
    }
    printf("execve_null_argv: PASS\n");
    return 0;
}
