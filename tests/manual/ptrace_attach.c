// PTRACE_ATTACH -- what gdb uses to attach to an already-running process.
//
// It was missing from the ptrace dispatch entirely and fell through to the
// default arm's EPERM, so `gdb -p` reported "ptrace: Operation not permitted"
// and could not attach to a guest process at all. PTRACE_SEIZE was
// implemented; ATTACH differs in that it takes no options and, crucially,
// STOPS the tracee, so the tracer's first wait() reports a SIGSTOP
// signal-delivery-stop.
//
// Every expectation, including the errno values and the EIO for
// PTRACE_INTERRUPT on an ATTACH'd (rather than SEIZE'd) tracee, was measured
// against x86_64 glibc on Linux 6.12.
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/ptrace.h>
#include <sys/wait.h>

#include "test_common.h"

static void check(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-42s got=%ld want=%ld\n", label, got, want);
}
static void nap(long ms) {
    struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&t, NULL);
}

// Lives at the same address in the tracee, since fork copies the map.
static volatile long marker = 0x5a5a5a5a;

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(120));

    fflush(NULL);
    pid_t c = fork();
    if (c == 0) {
        for (int i = 0; i < 600; i++)
            nap(50);
        _exit(0);
    }
    if (c < 0) {
        printf("ptrace_attach: SKIP (fork failed)\n");
        return 0;
    }
    nap(300);

    errno = 0;
    long r = ptrace(PTRACE_ATTACH, c, 0, 0);
    if (r < 0) {
        failf("PTRACE_ATTACH", (uint64_t) errno, 0, 0, 0, 0, 0);
        kill(c, SIGKILL);
        waitpid(c, NULL, 0);
        return finish_suite("ptrace_attach");
    }
    check("PTRACE_ATTACH rc", r, 0);

    int st = 0;
    pid_t w;
    while ((w = waitpid(c, &st, 0)) < 0 && errno == EINTR)
        continue;
    check("wait returns the tracee", w == c, 1);
    check("tracee is stopped", WIFSTOPPED(st) != 0, 1);
    check("stopped with SIGSTOP", WIFSTOPPED(st) ? WSTOPSIG(st) : -1, SIGSTOP);

    errno = 0;
    check("PEEKDATA reads tracee memory",
          ptrace(PTRACE_PEEKDATA, c, (void *) &marker, 0), 0x5a5a5a5a);
    errno = 0;
    check("POKEDATA rc", ptrace(PTRACE_POKEDATA, c, (void *) &marker, 0x1234), 0);
    errno = 0;
    check("POKEDATA took effect",
          ptrace(PTRACE_PEEKDATA, c, (void *) &marker, 0), 0x1234);

    // INTERRUPT is a SEIZE-only request; an ATTACH'd tracee gets EIO.
    errno = 0;
    r = ptrace(PTRACE_INTERRUPT, c, 0, 0);
    check("INTERRUPT on an ATTACH'd tracee rc", r, -1);
    check("INTERRUPT on an ATTACH'd tracee errno", r < 0 ? errno : 0, EIO);

    errno = 0;
    check("PTRACE_CONT rc", ptrace(PTRACE_CONT, c, 0, 0), 0);
    nap(200);

    // Stop it again so the detach has a stopped tracee to release.
    kill(c, SIGSTOP);
    while (waitpid(c, &st, WUNTRACED) < 0 && errno == EINTR)
        continue;
    errno = 0;
    check("PTRACE_DETACH rc", ptrace(PTRACE_DETACH, c, 0, SIGCONT), 0);
    nap(300);
    errno = 0;
    check("tracee alive after detach", kill(c, 0), 0);

    kill(c, SIGKILL);
    while (waitpid(c, &st, 0) < 0 && errno == EINTR)
        continue;

    // Attaching to yourself is EPERM, to a missing pid is ESRCH.
    errno = 0;
    r = ptrace(PTRACE_ATTACH, getpid(), 0, 0);
    check("ATTACH to self rc", r, -1);
    check("ATTACH to self errno", r < 0 ? errno : 0, EPERM);

    errno = 0;
    r = ptrace(PTRACE_ATTACH, 0x7ffff, 0, 0);
    check("ATTACH to a missing pid rc", r, -1);
    check("ATTACH to a missing pid errno", r < 0 ? errno : 0, ESRCH);

    return finish_suite("ptrace_attach");
}
