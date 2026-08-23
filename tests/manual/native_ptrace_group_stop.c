// A NATIVE program that group-stops must report the stop to its tracer, the
// same way a translated one does.
//
// iSH-AOK runs some programs as host code on the guest task's own thread
// (kernel/native.h), so nothing dispatches instructions for them and nothing
// checks for signals the way the emulator's interrupt path does. They poll
// instead, at native_checkpoint() (kernel/native.c), and that function carried
// its own copy of the job-control group-stop wait -- a copy whose comment
// claimed to mirror handle_interrupt (kernel/calls.c) and did not. It had no
// ptrace handling whatsoever: no ptrace_group_stop() report on the way in, and
// no re-check of ptrace.traced while parked, which is what catches a tracer
// that ATTACHES to an already-stopped tracee. So `strace` (or anything else)
// tracing a native program that got ^Z'd waited forever for a stop report that
// could not arrive.
//
// The translated-code half of this was fixed in cba743c17 and is guarded by
// tests/manual/ptrace_group_stop.c. This is the native half, and both files
// share the same discipline: the attach order is FORCED with
// waitpid(WUNTRACED) rather than raced for, because a test that only fails
// when the scheduler cooperates reports "fixed" while the bug is still there.
//
// Four cases -- two attach orders against two kinds of program:
//
//   control : re-exec this binary in --spew mode, an ordinary guest ELF. It
//             stops through handle_interrupt, so it must pass everywhere,
//             including on a real Linux oracle which has no native dispatch.
//             If the control fails, the harness is wrong, not the kernel.
//   native  : /AOK/native/smallclue invoked as `yes`, which is host code.
//             Skipped where /AOK/native does not exist.
//
// The victim writes continuously into a pipe, which buys two things the
// handshake needs: the first read proves the program really is running (so the
// stop lands inside it, not somewhere in exec), and draining that pipe while
// the child is stopped makes a later successful read positive proof that it
// was RESUMED -- as opposed to killed, or still parked. "It died when I killed
// it" proves nothing: SIGKILL reaps a stopped task too.

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_common.h"

#ifndef PTRACE_SEIZE
#define PTRACE_SEIZE 0x4206
#endif
#ifndef PTRACE_EVENT_STOP
#define PTRACE_EVENT_STOP 128
#endif

#define NATIVE_SMALLCLUE "/AOK/native/smallclue"
#define CASE_NAME_MAX 64

static pid_t g_child;
static const char *g_self;
// Which case is in flight, for the watchdog to name. stdout is block-buffered
// when the suite captures it and the handler cannot safely flush it, so
// without this a hang reports only "timeout" and every test_logf line written
// so far is lost -- which is exactly the information needed to tell the
// control (translated code) from the subject (native code).
static const char *volatile g_case = "startup";

static void on_alarm(int sig) {
    (void) sig;
    if (g_child > 0)
        kill(g_child, SIGKILL);
    static const char msg[] =
        "native_ptrace_group_stop: FAIL timeout in case ";
    static const char tail[] = " (group-stop never reported, or never resumed)\n";
    // Bounded: g_case is written by the main flow and read here, so a stray
    // alarm mid-snprintf must not send this scanning off the end.
    const char *name = g_case;
    size_t n = 0;
    while (n < CASE_NAME_MAX && name[n] != '\0')
        n++;
    write(2, msg, sizeof(msg) - 1);
    write(2, name, n);
    write(2, tail, sizeof(tail) - 1);
    _exit(1);
}

// Start `path` with argv[0] == `arg0` (plus `arg1` if non-NULL), its stdout on
// a fresh pipe, and wait until it has actually produced output. Returns the
// read end, or -1.
static int start_spewer(const char *path, const char *arg0, const char *arg1,
                        pid_t *out_pid) {
    int p[2];
    if (pipe(p) != 0)
        return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(p[0]);
        close(p[1]);
        return -1;
    }
    if (pid == 0) {
        dup2(p[1], STDOUT_FILENO);
        close(p[0]);
        if (p[1] != STDOUT_FILENO)
            close(p[1]);
        // The native victim is a multicall binary picking its applet from
        // argv[0] and taking no arguments; the control needs --spew to know
        // which half of itself to be. Hence the optional second word.
        char *const av[] = { (char *) arg0, (char *) arg1, NULL };
        execv(path, av);
        _exit(127);
    }

    close(p[1]);
    char buf[256];
    ssize_t n;
    do {
        n = read(p[0], buf, sizeof buf);
    } while (n < 0 && errno == EINTR);
    if (n <= 0) {
        close(p[0]);
        kill(pid, SIGKILL);
        (void) waitpid(pid, NULL, 0);
        return -1;
    }

    *out_pid = pid;
    return p[0];
}

// Read everything already buffered, without blocking. Only meaningful while
// the writer is stopped -- that is what makes the next blocking read a proof
// of resumption rather than a reading of stale bytes.
//
// The flags are set and cleared ABSOLUTELY rather than with the usual
// get-modify-set dance, because on iSH-AOK the get half of that dance lies
// about a pipe: fcntl(F_GETFL) reports the HOST descriptor's flags
// (fs/real.c realfs_getflags), and realfs_read permanently forces the host
// descriptor non-blocking the first time the guest does a blocking read on it.
// So `flags = F_GETFL; F_SETFL(flags)` writes back an O_NONBLOCK the guest
// never asked for, and every later read here returned EAGAIN. See docs/TODO.md.
// Nothing else in this test cares about the pipe's flags, so setting them
// outright is both correct and immune to that.
static long drain(int fd) {
    fcntl(fd, F_SETFL, O_NONBLOCK);
    long total = 0;
    for (;;) {
        char buf[4096];
        ssize_t n = read(fd, buf, sizeof buf);
        if (n <= 0)
            break;
        total += n;
    }
    fcntl(fd, F_SETFL, 0);
    return total;
}

// Drive a seized tracee until it reports its group-stop. Returns 1 on the
// PTRACE_EVENT_STOP report, 0 on anything else (including the tracee dying).
static int await_group_stop_report(pid_t child, const char *label) {
    for (int i = 0; i < 100; i++) {
        int status = 0;
        pid_t w = waitpid(child, &status, 0);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            failf(label, (uint64_t) errno, 0, 0, 0, 0, 0);
            return 0;
        }
        if (!WIFSTOPPED(status)) {
            // Exited or was killed: no report is coming.
            failf(label, (uint64_t) status, 0, 0, 0, 0, 0);
            return 0;
        }

        int sig = WSTOPSIG(status);
        int event = (status >> 16) & 0xff;
        test_logf("    stop: sig=%d event=%d status=%#x\n", sig, event, status);

        if (event == PTRACE_EVENT_STOP)
            return 1;
        // Signal-delivery-stop of SIGSTOP: deliver it, so the tracee actually
        // enters group-stop and reports it next time round.
        int deliver = (sig == SIGSTOP) ? SIGSTOP : 0;
        if (ptrace(PTRACE_CONT, child, 0, deliver) != 0) {
            failf(label, (uint64_t) errno, (uint64_t) sig, 0, 0, 0, 0);
            return 0;
        }
    }
    failf(label, 0, 0, 0, 1, 0, 0);
    return 0;
}

// Shared tail: the tracee is group-stopped and reported. Drain, resume, and
// require fresh output.
static void finish_case(pid_t child, int fd, const char *label) {
    long stale = drain(fd);
    test_logf("    drained %ld stale bytes while stopped\n", stale);

    kill(child, SIGCONT);
    if (ptrace(PTRACE_CONT, child, 0, 0) != 0) {
        failf(label, (uint64_t) errno, 0, 0, 0, 0, 0);
        return;
    }

    char buf[256];
    ssize_t n;
    do {
        n = read(fd, buf, sizeof buf);
    } while (n < 0 && errno == EINTR);
    test_logf("    post-resume read %zd\n", n);
    if (n <= 0)
        failf(label, (uint64_t) n, (uint64_t) errno, 0, 1, 0, 0);
}

// Order A: attach to a RUNNING program, then stop it.
static void case_stop_after_attach(const char *path, const char *arg0,
                                   const char *arg1, const char *label) {
    pid_t child;
    int fd = start_spewer(path, arg0, arg1, &child);
    if (fd < 0) {
        failf(label, (uint64_t) errno, 0, 0, 0, 0, 0);
        return;
    }
    g_child = child;

    if (ptrace(PTRACE_SEIZE, child, 0, 0) != 0) {
        failf(label, (uint64_t) errno, 0, 0, 0, 0, 0);
        goto out;
    }
    kill(child, SIGSTOP);
    if (await_group_stop_report(child, label))
        finish_case(child, fd, label);

out:
    kill(child, SIGCONT);
    kill(child, SIGKILL);
    (void) waitpid(child, NULL, 0);
    close(fd);
    g_child = 0;
}

// Order B: the program is ALREADY group-stopped when the tracer attaches.
// waitpid(WUNTRACED) reports the job-control stop of an untraced child, so
// this returns only once the child really is stopped -- that is what makes the
// ordering deterministic instead of a coin flip. This is the order that hung:
// PTRACE_SEIZE sets ptrace.traced from the tracer's thread, and a wait that
// tested that flag only on entry never noticed.
static void case_attach_after_stop(const char *path, const char *arg0,
                                   const char *arg1, const char *label) {
    pid_t child;
    int fd = start_spewer(path, arg0, arg1, &child);
    if (fd < 0) {
        failf(label, (uint64_t) errno, 0, 0, 0, 0, 0);
        return;
    }
    g_child = child;

    kill(child, SIGSTOP);
    int status = 0;
    pid_t w;
    do {
        w = waitpid(child, &status, WUNTRACED);
    } while (w < 0 && errno == EINTR);
    if (w != child || !WIFSTOPPED(status) || WSTOPSIG(status) != SIGSTOP) {
        failf(label, (uint64_t) w, (uint64_t) status, (uint64_t) errno,
              (uint64_t) child, 0, 0);
        goto out;
    }
    test_logf("    child %d is group-stopped; seizing now\n", (int) child);

    if (ptrace(PTRACE_SEIZE, child, 0, 0) != 0) {
        failf(label, (uint64_t) errno, 0, 0, 0, 0, 0);
        goto out;
    }
    if (await_group_stop_report(child, label))
        finish_case(child, fd, label);

out:
    kill(child, SIGCONT);
    kill(child, SIGKILL);
    (void) waitpid(child, NULL, 0);
    close(fd);
    g_child = 0;
}

static void run_both_orders(const char *path, const char *arg0, const char *arg1,
                            const char *kind) {
    static char label[CASE_NAME_MAX];   // static: the watchdog handler reads it
    test_logf("  %s: attach then stop\n", kind);
    snprintf(label, sizeof label, "%s_stop_after_attach", kind);
    g_case = label;
    case_stop_after_attach(path, arg0, arg1, label);

    test_logf("  %s: stop then attach\n", kind);
    snprintf(label, sizeof label, "%s_attach_after_stop", kind);
    g_case = label;
    case_attach_after_stop(path, arg0, arg1, label);
}

int main(int argc, char **argv) {
    // The victim half of the control: write forever. Checked before
    // test_init, which rejects options it does not know.
    if (argc >= 2 && strcmp(argv[1], "--spew") == 0) {
        for (;;)
            if (write(STDOUT_FILENO, "y\n", 2) < 0 && errno != EINTR)
                _exit(0);
    }

    test_init(argc, argv);
    g_self = argv[0];
    signal(SIGALRM, on_alarm);
    // Generous, env-scalable watchdog, on the same reasoning as
    // ptrace_group_stop.c: the handshake completes in tens of milliseconds,
    // and the failure mode being guarded against is an unbounded hang.
    alarm(test_watchdog_secs(120));

    // Control: an ordinary program image, which stops through
    // handle_interrupt. This is the whole test on a Linux oracle.
    run_both_orders(g_self, g_self, "--spew", "control");

    struct stat st;
    if (stat(NATIVE_SMALLCLUE, &st) != 0) {
        test_logf("  no %s here, native half skipped\n", NATIVE_SMALLCLUE);
    } else {
        // argv[0] picks the applet, as on any multicall binary. `yes` writes
        // forever, and every write is a native_checkpoint -- which is exactly
        // where the group-stop wait lives.
        run_both_orders(NATIVE_SMALLCLUE, "yes", NULL, "native");
    }

    return finish_suite("native_ptrace_group_stop");
}
