// native_stdio_lock.c — regression lock for the orphaned-stdio-lock wedge
// (second instance of the 1d8eaae0d class, found 2026-08-27 as "terminal
// hangs before prompt" on device).
//
// The shape: a native program killed by SIGPIPE mid-write (`yes | head -1`)
// used to take its fatal signal inside a host stdio callback, abandoning its
// stdout FILE lock -- Darwin never releases a dead owner's mutex -- and the
// next native program to READ via stdio hung forever in __srefill's
// _fwalk(lflush) walking onto the orphaned lock. Two fixes hold this test up:
// fatal delivery is deferred while inside a stdio callback
// (nlibc_stdio_defer_fatal), and native stdin is fully buffered so the refill
// never walks foreign streams at all.
//
// iSH-AOK-specific by nature (it exercises /AOK/native dispatch), so it skips
// anywhere the native multi-call binary is missing -- including real Linux.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include "test_common.h"

#define SMALLCLUE "/AOK/native/smallclue"

static int check(const char *label, int cond) {
    if (cond) { test_logf("ok   %s\n", label); return 1; }
    printf("FAIL %s\n", label);
    failures_total++;
    return 0;
}

// Run `seq` with stdout on a pipe, read one line, close the pipe: seq is an
// error-ignoring printf loop, so after the first write fails with EPIPE it
// issues ANOTHER stdout write, whose signal checkpoint is where the fatal
// SIGPIPE used to fire -- inside the stdio callback, stdout lock held. That
// second-write-after-EPIPE step is the culprit shape; `yes` does NOT
// reproduce it, because it checks its write result and exits cleanly after
// the first failure.
static int kill_native_writer_via_sigpipe(void) {
    int fds[2];
    if (pipe(fds) != 0)
        return -1;
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        dup2(fds[1], 1);
        close(fds[0]);
        close(fds[1]);
        char *argv[] = {"seq", "1", "1000000", NULL};
        execv(SMALLCLUE, argv);
        _exit(127);
    }
    close(fds[1]);
    char buf[64];
    ssize_t n = read(fds[0], buf, sizeof(buf));
    close(fds[0]); // writer now gets SIGPIPE/EPIPE
    int status = 0;
    waitpid(pid, &status, 0);
    return n > 0 ? 0 : -1;
}

// Run native `sed` over a pipe pair. Before the fix, the FIRST victim after a
// leaked lock never returned from its first stdio read; the suite-level
// watchdog below turns that into a failure instead of a hang.
static int native_sed_roundtrip(const char *tag) {
    int in[2], out[2];
    if (pipe(in) != 0 || pipe(out) != 0)
        return -1;
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        dup2(in[0], 0);
        dup2(out[1], 1);
        close(in[0]); close(in[1]); close(out[0]); close(out[1]);
        char *argv[] = {"sed", "s/ping/pong/", NULL};
        execv(SMALLCLUE, argv);
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    if (write(in[1], "ping\n", 5) != 5)
        return -1;
    close(in[1]);
    char buf[64] = "";
    ssize_t n = read(out[0], buf, sizeof(buf) - 1);
    close(out[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (n < 0)
        return -1;
    buf[n] = '\0';
    test_logf("ok   %s -> %.16s", tag, buf);
    return strncmp(buf, "pong", 4) == 0 ? 0 : -1;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    if (access(SMALLCLUE, X_OK) != 0) {
        printf("native_stdio_lock: SKIP (no %s)\n", SMALLCLUE);
        return 0;
    }
    // A regression here IS a hang; convert it to a visible failure.
    alarm(test_watchdog_secs(60));

    for (int round = 1; round <= 3; round++) {
        char tag[32];
        snprintf(tag, sizeof(tag), "round%d.sigpipe_kill", round);
        check(tag, kill_native_writer_via_sigpipe() == 0);
        snprintf(tag, sizeof(tag), "round%d.victim_read", round);
        check(tag, native_sed_roundtrip(tag) == 0);
    }
    return finish_suite("native_stdio_lock");
}
