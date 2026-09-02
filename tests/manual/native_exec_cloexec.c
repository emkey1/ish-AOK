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
#include <stdio.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
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

// ---------------------------------------------------------------------------
// The address space a native exec leaves behind.
//
// A native program replaces the process image the way an ELF one does, so it
// must get a new address space too. It did not: __do_execve returned before
// anything below format_exec, so the program ran in whatever space it
// inherited. `/AOK/native/zsh` kept /bin/busybox and the musl loader mapped for
// its whole run -- an image it does not execute, holding memory that the exec
// should have released -- and /proc reported the PARENT's command line and the
// parent's binary as /proc/<pid>/exe.
//
// Checked from the parent, on a child that is definitely inside the native
// program (it blocks on a pipe nobody writes to).

// Start a child running the native `cat` applet on a pipe stdin, so it blocks.
// Returns its pid, or -1; *keep_open is a descriptor the caller must close
// after reaping, being the write end that keeps the child's stdin from EOF.
static pid_t start_blocked_native(int *keep_open) {
    int in_pipe[2];
    if (pipe(in_pipe) != 0)
        return -1;
    pid_t pid = fork();
    if (pid < 0) {
        close(in_pipe[0]); close(in_pipe[1]);
        return -1;
    }
    if (pid == 0) {
        close(in_pipe[1]);
        dup2(in_pipe[0], STDIN_FILENO);
        if (in_pipe[0] != STDIN_FILENO)
            close(in_pipe[0]);
        char *const child_argv[] = { (char *) "cat", NULL };
        execv(NATIVE_SMALLCLUE, child_argv);
        _exit(127);
    }
    close(in_pipe[0]);
    *keep_open = in_pipe[1];
    return pid;
}

// First line of /proc/<pid>/<what>, NULs turned into spaces so cmdline reads
// like a string. Empty on any failure.
static void proc_read(pid_t pid, const char *what, char *out, size_t outsz) {
    out[0] = '\0';
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/%s", (int) pid, what);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return;
    ssize_t n = read(fd, out, outsz - 1);
    close(fd);
    if (n <= 0) { out[0] = '\0'; return; }
    out[n] = '\0';
    for (ssize_t i = 0; i < n; i++)
        if (out[i] == '\0' || out[i] == '\n')
            out[i] = ' ';
    while (n > 0 && out[n - 1] == ' ')
        out[--n] = '\0';
}

// Wait until the child's comm says it is the applet, i.e. the exec has landed.
static int wait_for_comm(pid_t pid, const char *want, unsigned tries) {
    char comm[64];
    for (unsigned i = 0; i < tries; i++) {
        proc_read(pid, "comm", comm, sizeof comm);
        if (strcmp(comm, want) == 0)
            return 1;
        struct timespec t = { 0, 20 * 1000 * 1000 };   // 20ms
        nanosleep(&t, NULL);
    }
    return 0;
}

// How many regions of the child's address space name a file, and one example.
static int maps_file_backed(pid_t pid, char *example, size_t exsz) {
    example[0] = '\0';
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/maps", (int) pid);
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return -1;
    char line[512];
    int named = 0;
    while (fgets(line, sizeof line, f) != NULL) {
        const char *slash = strchr(line, '/');
        if (slash == NULL)
            continue;
        // "[stack]" and friends are not files; a real path starts with '/'.
        if (named++ == 0) {
            size_t n = 0;
            while (slash[n] != '\0' && slash[n] != '\n' && n + 1 < exsz) {
                example[n] = slash[n];
                n++;
            }
            example[n] = '\0';
        }
    }
    fclose(f);
    return named;
}

static void check_native_address_space(void) {
    int keep_open = -1;
    pid_t pid = start_blocked_native(&keep_open);
    if (pid < 0) {
        test_logf("  could not start a blocked native program, skipped\n");
        return;
    }
    if (!wait_for_comm(pid, "cat", 250)) {
        test_logf("  native program never reached its applet, skipped\n");
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        close(keep_open);
        return;
    }

    char example[256];
    int named = maps_file_backed(pid, example, sizeof example);
    if (named < 0) {
        test_logf("  cannot read the child's maps, skipped\n");
    } else {
        if (named != 0)
            failf("a native program's maps name no file", (uint64_t) named, 0, 0, 0, 0, 0);
        test_logf("  %-52s %s (%d region(s)%s%s)\n",
                  "native exec leaves no inherited mapping",
                  named == 0 ? "ok" : "FAIL", named,
                  named ? ", e.g. " : "", named ? example : "");
    }

    char cmdline[256], exe[256];
    proc_read(pid, "cmdline", cmdline, sizeof cmdline);
    // exe is a symlink: read the link, not whatever it points at.
    {
        char path[64];
        snprintf(path, sizeof path, "/proc/%d/exe", (int) pid);
        ssize_t n = readlink(path, exe, sizeof exe - 1);
        exe[n > 0 ? (size_t) n : 0] = '\0';
    }
    // Neither is readable from the guest's own argv region -- there is none --
    // so both come from what the exec recorded.
    if (strcmp(cmdline, "cat") != 0)
        failf("a native program's cmdline is its own argv", 0, 0, 0, 0, 0, 0);
    test_logf("  %-52s %s (\"%s\")\n", "native exec publishes its command line",
              strcmp(cmdline, "cat") == 0 ? "ok" : "FAIL", cmdline);
    if (strcmp(exe, NATIVE_SMALLCLUE) != 0)
        failf("a native program's exe is the program", 0, 0, 0, 0, 0, 0);
    test_logf("  %-52s %s (\"%s\")\n", "native exec points /proc/<pid>/exe at itself",
              strcmp(exe, NATIVE_SMALLCLUE) == 0 ? "ok" : "FAIL", exe);

    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    close(keep_open);
}

// ---------------------------------------------------------------------------
// The scratch a native program marshals its syscalls through (the arena in
// kernel/native_syscall.c) is a megabyte of GUEST address space, mapped on
// first use. It was mapped per thread and never released, which is invisible
// while the address space dies with the program -- and is a straightforward
// leak of the caller's memory when it does not.
//
// vfork and posix_spawn are exactly when it does not: the child shares its
// parent's address space (CLONE_VM) right up to the exec, so the arena landed
// in the PARENT and stayed there for the parent's whole life. Measured before
// the fix: a process doing 40 vfork/posix_spawn launches of a native applet
// grew from 704 kB to 41,668 kB -- a megabyte a child, none of it ever given
// back.
static long self_vmsize_kb(void) {
    FILE *f = fopen("/proc/self/status", "r");
    if (f == NULL)
        return -1;
    char line[256];
    long kb = -1;
    while (fgets(line, sizeof line, f) != NULL)
        if (strncmp(line, "VmSize:", 7) == 0) {
            if (sscanf(line + 7, "%ld", &kb) != 1)
                kb = -1;
            break;
        }
    fclose(f);
    return kb;
}

static void check_native_exec_leaves_no_scratch(void) {
    long before = self_vmsize_kb();
    if (before < 0) {
        test_logf("  no VmSize in /proc/self/status, scratch check skipped\n");
        return;
    }

    // `sleep 0` returns at once and prints nothing, but it does reach the
    // kernel -- which is the point: an applet that issues no syscall never
    // allocates any scratch and would pass this test on a broken build.
    char *const child_argv[] = { (char *) "sleep", (char *) "0", NULL };
    const int rounds = 8;
    for (int i = 0; i < rounds; i++) {
        pid_t p = vfork();
        if (p == 0) {
            execv(NATIVE_SMALLCLUE, child_argv);
            _exit(127);
        }
        if (p < 0) {
            test_logf("  vfork failed, scratch check skipped\n");
            return;
        }
        waitpid(p, NULL, 0);
    }

    long after = self_vmsize_kb();
    long grew = after - before;
    // Slack for anything else the loop legitimately touched; the leak this
    // catches is 1024 kB per child, so 8 children move this by 8 MB.
    const long slack = 256;
    if (grew > slack)
        failf("vfork+exec of a native program leaks no address space",
              (uint64_t) grew, (uint64_t) before, (uint64_t) after,
              0, 0, 0);
    test_logf("  %-52s %s (%ld kB over %d children)\n",
              "native exec strands no scratch in its caller",
              grew <= slack ? "ok" : "FAIL", grew, rounds);
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

        check_native_address_space();
        check_native_exec_leaves_no_scratch();
    }

    return finish_suite("native_exec_cloexec");
}
