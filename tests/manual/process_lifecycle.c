// fork and exec bookkeeping: exit and signal statuses, WNOHANG, the signal mask
// a child inherits, and which handlers, ignores, masks, argv and environment
// survive an exec.
#define _GNU_SOURCE

#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_common.h"

#ifndef CLONE_NEWUTS
#define CLONE_NEWUTS 0x04000000
#endif
#ifndef CLONE_PARENT
#define CLONE_PARENT 0x00008000
#endif

static volatile sig_atomic_t parent_handler_count;

static void parent_signal_handler(int sig) {
    if (sig == SIGUSR1)
        parent_handler_count++;
}

static void install_parent_handler(void) {
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = parent_signal_handler;
    sigemptyset(&act.sa_mask);
    if (sigaction(SIGUSR1, &act, NULL) != 0) {
        perror("sigaction");
        exit(1);
    }
}

static void reset_parent_sigusr1(void) {
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = SIG_DFL;
    sigemptyset(&act.sa_mask);
    if (sigaction(SIGUSR1, &act, NULL) != 0) {
        perror("sigaction");
        exit(1);
    }
}

static void set_sigusr1_ignored(void) {
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = SIG_IGN;
    sigemptyset(&act.sa_mask);
    if (sigaction(SIGUSR1, &act, NULL) != 0) {
        perror("sigaction");
        exit(1);
    }
}

static void set_sigusr1_blocked(int blocked) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    if (sigprocmask(blocked ? SIG_BLOCK : SIG_UNBLOCK, &set, NULL) != 0) {
        perror("sigprocmask");
        exit(1);
    }
}

static int helper_check_exec_handler_reset(void) {
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    if (sigaction(SIGUSR1, NULL, &act) != 0)
        return 101;
    return act.sa_handler == SIG_DFL ? 0 : 102;
}

static int helper_check_exec_ignore_persists(void) {
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    if (sigaction(SIGUSR1, NULL, &act) != 0)
        return 103;
    return act.sa_handler == SIG_IGN ? 0 : 104;
}

static int helper_check_exec_mask_persists(void) {
    sigset_t set;
    if (sigprocmask(SIG_SETMASK, NULL, &set) != 0)
        return 105;
    return sigismember(&set, SIGUSR1) ? 0 : 106;
}

static int helper_check_exec_env_argv(int argc, char **argv) {
    const char *env = getenv("ISH_AOK_LIFECYCLE");
    if (argc < 3)
        return 107;
    if (env == NULL || strcmp(env, "ok") != 0)
        return 108;
    if (strcmp(argv[2], "argv-ok") != 0)
        return 109;
    return 0;
}

static void test_fork_exit_status(void) {
    pid_t pid = fork();
    int status = 0;
    int rc;
    int failed;

    if (pid < 0) {
        perror("fork");
        exit(1);
    }
    if (pid == 0)
        _exit(42);

    rc = waitpid(pid, &status, 0);
    failed = rc != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 42;
    test_log_if(failed, "fork exit status: waitpid=%d exited=%d status=%d\n",
                rc, WIFEXITED(status), WEXITSTATUS(status));
    if (failed)
        failf("fork exit status", (uint64_t) rc, (uint64_t) WIFEXITED(status),
              (uint64_t) WEXITSTATUS(status), (uint64_t) pid, 1, 42);
}

static void test_fork_signal_status(void) {
    pid_t pid = fork();
    int status = 0;
    int rc;
    int failed;

    if (pid < 0) {
        perror("fork");
        exit(1);
    }
    if (pid == 0) {
        kill(getpid(), SIGTERM);
        _exit(99);
    }

    rc = waitpid(pid, &status, 0);
    failed = rc != pid || !WIFSIGNALED(status) || WTERMSIG(status) != SIGTERM;
    test_log_if(failed, "fork signal status: waitpid=%d signaled=%d sig=%d\n",
                rc, WIFSIGNALED(status), WTERMSIG(status));
    if (failed)
        failf("fork signal status", (uint64_t) rc, (uint64_t) WIFSIGNALED(status),
              (uint64_t) WTERMSIG(status), (uint64_t) pid, 1, SIGTERM);
}

static void test_waitpid_wnohang(void) {
    int pipefd[2];
    pid_t pid;
    int status = 0;
    int rc0;
    int rc1;
    char ch = 'x';
    int failed;

    if (pipe(pipefd) != 0) {
        perror("pipe");
        exit(1);
    }

    pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(1);
    }
    if (pid == 0) {
        close(pipefd[1]);
        if (read(pipefd[0], &ch, 1) != 1)
            _exit(111);
        _exit(7);
    }

    close(pipefd[0]);
    rc0 = waitpid(pid, &status, WNOHANG);
    if (write(pipefd[1], &ch, 1) != 1) {
        perror("write");
        exit(1);
    }
    close(pipefd[1]);
    rc1 = waitpid(pid, &status, 0);

    failed = rc0 != 0 || rc1 != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 7;
    test_log_if(failed, "waitpid wnohang: rc0=%d rc1=%d exited=%d status=%d\n",
                rc0, rc1, WIFEXITED(status), WEXITSTATUS(status));
    if (failed)
        failf("waitpid wnohang", (uint64_t) rc0, (uint64_t) rc1,
              (uint64_t) WEXITSTATUS(status), 0, (uint64_t) pid, 7);
}

static void test_fork_sigmask_inheritance(void) {
    pid_t pid;
    int status = 0;
    int rc;
    sigset_t set;
    int blocked;
    int failed;

    set_sigusr1_blocked(1);
    pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(1);
    }
    if (pid == 0) {
        if (sigprocmask(SIG_SETMASK, NULL, &set) != 0)
            _exit(112);
        blocked = sigismember(&set, SIGUSR1);
        _exit(blocked ? 0 : 1);
    }

    rc = waitpid(pid, &status, 0);
    set_sigusr1_blocked(0);
    failed = rc != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0;
    test_log_if(failed, "fork sigmask inheritance: waitpid=%d exited=%d status=%d\n",
                rc, WIFEXITED(status), WEXITSTATUS(status));
    if (failed)
        failf("fork sigmask inheritance", (uint64_t) rc, (uint64_t) WIFEXITED(status),
              (uint64_t) WEXITSTATUS(status), (uint64_t) pid, 1, 0);
}

static void test_exec_handler_reset(char **argv) {
    pid_t pid;
    int status = 0;
    int rc;
    int failed;

    install_parent_handler();
    pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(1);
    }
    if (pid == 0) {
        char *const child_argv[] = {argv[0], "--child-check-exec-handler-reset", NULL};
        execv(argv[0], child_argv);
        _exit(113);
    }

    rc = waitpid(pid, &status, 0);
    reset_parent_sigusr1();
    failed = rc != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0;
    test_log_if(failed, "exec handler reset: waitpid=%d exited=%d status=%d\n",
                rc, WIFEXITED(status), WEXITSTATUS(status));
    if (failed)
        failf("exec handler reset", (uint64_t) rc, (uint64_t) WIFEXITED(status),
              (uint64_t) WEXITSTATUS(status), (uint64_t) pid, 1, 0);
}

static void test_exec_ignore_persists(char **argv) {
    pid_t pid;
    int status = 0;
    int rc;
    int failed;

    set_sigusr1_ignored();
    pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(1);
    }
    if (pid == 0) {
        char *const child_argv[] = {argv[0], "--child-check-exec-ignore-persists", NULL};
        execv(argv[0], child_argv);
        _exit(114);
    }

    rc = waitpid(pid, &status, 0);
    reset_parent_sigusr1();
    failed = rc != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0;
    test_log_if(failed, "exec ignore persists: waitpid=%d exited=%d status=%d\n",
                rc, WIFEXITED(status), WEXITSTATUS(status));
    if (failed)
        failf("exec ignore persists", (uint64_t) rc, (uint64_t) WIFEXITED(status),
              (uint64_t) WEXITSTATUS(status), (uint64_t) pid, 1, 0);
}

static void test_exec_sigmask_persists(char **argv) {
    pid_t pid;
    int status = 0;
    int rc;
    int failed;

    set_sigusr1_blocked(1);
    pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(1);
    }
    if (pid == 0) {
        char *const child_argv[] = {argv[0], "--child-check-exec-mask-persists", NULL};
        execv(argv[0], child_argv);
        _exit(115);
    }

    rc = waitpid(pid, &status, 0);
    set_sigusr1_blocked(0);
    failed = rc != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0;
    test_log_if(failed, "exec sigmask persists: waitpid=%d exited=%d status=%d\n",
                rc, WIFEXITED(status), WEXITSTATUS(status));
    if (failed)
        failf("exec sigmask persists", (uint64_t) rc, (uint64_t) WIFEXITED(status),
              (uint64_t) WEXITSTATUS(status), (uint64_t) pid, 1, 0);
}

static void test_exec_env_argv(char **argv) {
    pid_t pid;
    int status = 0;
    int rc;
    int failed;

    pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(1);
    }
    if (pid == 0) {
        char *const child_argv[] = {argv[0], "--child-check-exec-env-argv", "argv-ok", NULL};
        char *const child_envp[] = {"ISH_AOK_LIFECYCLE=ok", NULL};
        execve(argv[0], child_argv, child_envp);
        _exit(116);
    }

    rc = waitpid(pid, &status, 0);
    failed = rc != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0;
    test_log_if(failed, "exec env argv: waitpid=%d exited=%d status=%d\n",
                rc, WIFEXITED(status), WEXITSTATUS(status));
    if (failed)
        failf("exec env argv", (uint64_t) rc, (uint64_t) WIFEXITED(status),
              (uint64_t) WEXITSTATUS(status), (uint64_t) pid, 1, 0);
}

static void test_vfork_exec_true(void) {
    pid_t pid;
    int status = 0;
    int rc;
    int failed;

    pid = vfork();
    if (pid < 0) {
        perror("vfork");
        exit(1);
    }
    if (pid == 0) {
        char *const child_argv[] = {"/bin/true", NULL};
        execv("/bin/true", child_argv);
        _exit(117);
    }

    rc = waitpid(pid, &status, 0);
    failed = rc != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0;
    test_log_if(failed, "vfork exec true: waitpid=%d exited=%d status=%d\n",
                rc, WIFEXITED(status), WEXITSTATUS(status));
    if (failed)
        failf("vfork exec true", (uint64_t) rc, (uint64_t) WIFEXITED(status),
              (uint64_t) WEXITSTATUS(status), (uint64_t) pid, 1, 0);
}

// Covers two clone-flag fixes (issue #423 Tier 1b):
//  1. CLONE_NEWUTS is implemented (issue #527, see uts_namespace.c), so it
//     must SUCCEED for a privileged caller and give the child its own UTS
//     namespace. Unprivileged, it must still be EPERM -- "you lack
//     CAP_SYS_ADMIN", the errno a caller that falls back on missing namespace
//     privilege expects -- and never the generic "flag not recognized" EINVAL
//     that this originally regressed against. The other CLONE_NEW* flags stay
//     unimplemented and keep returning EPERM to everyone.
static void test_clone_newuts(void) {
    bool privileged = geteuid() == 0;
    char *stack = malloc(65536);
    pid_t pid = clone((int (*)(void *)) getpid, stack + 65536, CLONE_NEWUTS | SIGCHLD, NULL);
    int err = errno;
    free(stack);
    if (pid != -1) {
        waitpid(pid, NULL, 0);
        if (!privileged)
            failf("clone CLONE_NEWUTS unprivileged -> EPERM (unexpectedly succeeded)",
                  (uint64_t) pid, 0, 0, (uint64_t) -1, 0, 0);
        return;
    }
    if (privileged) {
        test_logf("clone CLONE_NEWUTS as root: errno=%d (%s)\n", err, strerror(err));
        failf("clone CLONE_NEWUTS as root -> success", (uint64_t) err, 0, 0, 0, 0, 0);
        return;
    }
    test_log_if(err != EPERM, "clone CLONE_NEWUTS: errno=%d (%s)\n", err, strerror(err));
    if (err != EPERM)
        failf("clone CLONE_NEWUTS -> EPERM", (uint64_t) err, 0, 0, (uint64_t) EPERM, 0, 0);
}

static int child_report_ppid(void *arg) {
    pid_t expected_grandparent = *(pid_t *) arg;
    _exit(getppid() == expected_grandparent ? 0 : 1);
}

//  2. CLONE_PARENT: a task cloned with this flag becomes a *sibling* of the
//     caller (its parent is the caller's own parent), not a child of the
//     caller. B (a direct child of A) clones C with CLONE_PARENT; C's parent
//     must be A, so A -- not B -- reaps it.
static void test_clone_parent_reparents(void) {
    pid_t a_pid = getpid();
    pid_t b_pid = fork();
    if (b_pid == 0) {
        char *stack = malloc(65536);
        pid_t expected = a_pid;
        pid_t c_pid = clone(child_report_ppid, stack + 65536, CLONE_PARENT | SIGCHLD, &expected);
        if (c_pid == -1)
            _exit(2);
        usleep(300000); // give C a chance to run and exit before B does
        _exit(0);
    }
    int b_status;
    waitpid(b_pid, &b_status, 0);
    int c_status;
    pid_t reaped = waitpid(-1, &c_status, 0);
    bool ok = reaped > 0 && WIFEXITED(c_status) && WEXITSTATUS(c_status) == 0;
    test_log_if(!ok, "clone CLONE_PARENT: reaped=%d c_status=%d\n", reaped, c_status);
    if (!ok)
        failf("clone CLONE_PARENT reparents to caller's parent",
              (uint64_t) reaped, (uint64_t) c_status, 0, 1, 0, 0);
}

int main(int argc, char **argv) {
    if (argc >= 2) {
        if (strcmp(argv[1], "--child-check-exec-handler-reset") == 0)
            return helper_check_exec_handler_reset();
        if (strcmp(argv[1], "--child-check-exec-ignore-persists") == 0)
            return helper_check_exec_ignore_persists();
        if (strcmp(argv[1], "--child-check-exec-mask-persists") == 0)
            return helper_check_exec_mask_persists();
        if (strcmp(argv[1], "--child-check-exec-env-argv") == 0)
            return helper_check_exec_env_argv(argc, argv);
    }

    test_init(argc, argv);
    test_fork_exit_status();
    test_fork_signal_status();
    test_waitpid_wnohang();
    test_fork_sigmask_inheritance();
    test_exec_handler_reset(argv);
    test_exec_ignore_persists(argv);
    test_exec_sigmask_persists(argv);
    test_exec_env_argv(argv);
    test_vfork_exec_true();
    test_clone_newuts();
    test_clone_parent_reparents();
    return finish_suite("process_lifecycle");
}
