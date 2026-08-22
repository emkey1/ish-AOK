// An exec closes every descriptor marked close-on-exec. iSH-AOK's native
// dispatch (kernel/native.h) did not: it runs the program in place of the
// image the ELF loader would have mapped, and returned from __do_execve before
// reaching any of the process-state work below format_exec -- close-on-exec
// among it.
//
// That is not a cosmetic omission. The standard way to learn whether a child's
// exec worked is to hand it a close-on-exec pipe and read from it: EOF means
// the exec happened and closed the write end, and a few bytes of errno mean it
// did not. APT's pager setup does exactly that, and `apt search maria` wedged
// the whole app whenever the pager it discovered on PATH resolved to a native
// program: the write end survived the exec, so apt sat forever on a four-byte
// read while the pager sat on the stdin apt had not begun writing. Two
// backtraces, neither moving again.
//
// This reproduces the handshake. The control -- re-execing this binary -- must
// pass everywhere including on a real Linux oracle, which has no /AOK and
// therefore skips the native half.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_common.h"

#define NATIVE_SMALLCLUE "/AOK/native/smallclue"

enum {
    PROBE_EOF = 0,       // the exec closed it: correct
    PROBE_STILL_OPEN,    // the descriptor survived the exec: the bug
    PROBE_EXEC_FAILED,   // the child could not exec at all
    PROBE_SETUP_FAILED,
};

static void on_alarm(int sig) { (void) sig; }

// Forks a child whose stdin is a pipe nobody ever writes to, execs `path` with
// `arg0`, and reads the close-on-exec sync pipe for at most `secs` seconds.
static int probe(const char *path, const char *arg0, unsigned secs, int *child_errno) {
    *child_errno = 0;
    int sync_pipe[2], in_pipe[2];
    if (pipe(sync_pipe) != 0)
        return PROBE_SETUP_FAILED;
    if (pipe(in_pipe) != 0) {
        close(sync_pipe[0]); close(sync_pipe[1]);
        return PROBE_SETUP_FAILED;
    }
    if (fcntl(sync_pipe[1], F_SETFD, FD_CLOEXEC) != 0) {
        close(sync_pipe[0]); close(sync_pipe[1]);
        close(in_pipe[0]); close(in_pipe[1]);
        return PROBE_SETUP_FAILED;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(sync_pipe[0]); close(sync_pipe[1]);
        close(in_pipe[0]); close(in_pipe[1]);
        return PROBE_SETUP_FAILED;
    }
    if (pid == 0) {
        close(sync_pipe[0]);
        close(in_pipe[1]);
        dup2(in_pipe[0], STDIN_FILENO);
        if (in_pipe[0] != STDIN_FILENO)
            close(in_pipe[0]);
        char *const child_argv[] = { (char *) arg0, NULL };
        execv(path, child_argv);
        int err = errno;
        ssize_t ignored = write(sync_pipe[1], &err, sizeof err);
        (void) ignored;
        _exit(127);
    }
    close(sync_pipe[1]);
    // in_pipe[1] stays OPEN on purpose: the child's stdin must never reach EOF,
    // so a child that really did exec is still there to be observed.
    close(in_pipe[0]);

    struct sigaction sa, old;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_alarm;   // no SA_RESTART: the read has to fail EINTR
    sigaction(SIGALRM, &sa, &old);
    unsigned prev = alarm(secs);

    int reported = 0;
    ssize_t n = read(sync_pipe[0], &reported, sizeof reported);
    int read_errno = errno;

    alarm(prev);
    sigaction(SIGALRM, &old, NULL);
    close(sync_pipe[0]);
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    close(in_pipe[1]);

    if (n == 0)
        return PROBE_EOF;
    if (n > 0) {
        *child_errno = reported;
        return PROBE_EXEC_FAILED;
    }
    if (read_errno == EINTR)
        return PROBE_STILL_OPEN;
    *child_errno = read_errno;
    return PROBE_SETUP_FAILED;
}

static const char *probe_name(int r) {
    switch (r) {
        case PROBE_EOF:          return "EOF (exec closed it)";
        case PROBE_STILL_OPEN:   return "STILL OPEN after the exec";
        case PROBE_EXEC_FAILED:  return "exec failed";
        default:                 return "setup failed";
    }
}

static void check(const char *what, int result, int child_errno) {
    int ok = (result == PROBE_EOF);
    if (!ok)
        failf(what, (uint64_t) result, (uint64_t) child_errno, 0, PROBE_EOF, 0, 0);
    test_logf("  %-52s %s (%s)\n", what, ok ? "ok" : "FAIL", probe_name(result));
}

int main(int argc, char **argv) {
    // The child half of the control: hold still until killed. Checked before
    // test_init, which rejects options it does not know.
    if (argc >= 2 && strcmp(argv[1], "--block") == 0) {
        for (;;) {
            char c;
            if (read(STDIN_FILENO, &c, 1) <= 0)
                pause();
        }
    }

    test_init(argc, argv);
    alarm(test_watchdog_secs(120));

    // Control: an ordinary program image. Establishes that the handshake this
    // test is built on works at all, and it is the whole test on a Linux
    // oracle, which has no native dispatch to get wrong.
    int child_errno = 0;
    int r = probe(argv[0], argv[0], 5, &child_errno);
    if (r == PROBE_EXEC_FAILED)
        test_logf("  control: cannot re-exec %s (%s), skipped\n",
                  argv[0], strerror(child_errno));
    else
        check("close-on-exec survives an ordinary exec", r, child_errno);

    // Subject: the same handshake against a program iSH-AOK runs as host code.
    struct stat st;
    if (stat(NATIVE_SMALLCLUE, &st) != 0) {
        test_logf("  no %s here, native half skipped\n", NATIVE_SMALLCLUE);
    } else {
        // argv[0] picks the applet, as on any multicall binary. `cat` with no
        // file arguments reads stdin -- which is the pipe above, so it blocks,
        // which is what makes "did the descriptor close?" observable at all.
        r = probe(NATIVE_SMALLCLUE, "cat", 5, &child_errno);
        if (r == PROBE_EXEC_FAILED)
            test_logf("  native smallclue present but not runnable (%s), skipped\n",
                      strerror(child_errno));
        else
            check("close-on-exec survives a NATIVE exec", r, child_errno);
    }

    return finish_suite("native_exec_cloexec");
}
