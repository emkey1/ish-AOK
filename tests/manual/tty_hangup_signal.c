// A terminal going away signals the session leader and foreground group.
//
// This is how a shell learns its session is over -- an ssh disconnect, a
// terminal window closing, the last master of a pty being closed. Linux sends
// SIGHUP and then SIGCONT (the SIGCONT so a stopped job runs far enough to
// notice the SIGHUP). AOK's tty_hangup woke every reader and poller and
// signalled nobody, so a shell sat in its read loop on a terminal that no
// longer existed.
//
// Measured against x86_64 glibc on Linux 6.12: the child dies of SIGHUP.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

#include "test_common.h"

static void nap(long ms) {
    struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&t, NULL);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(120));

    int master, slave;
    if (openpty(&master, &slave, NULL, NULL, NULL) < 0) {
        printf("tty_hangup_signal: SKIP (openpty: %s)\n", strerror(errno));
        return 0;
    }
    int sync[2];
    if (pipe(sync) < 0) {
        printf("tty_hangup_signal: SKIP (pipe: %s)\n", strerror(errno));
        return 0;
    }

    fflush(NULL);
    pid_t c = fork();
    if (c == 0) {
        close(master);
        close(sync[0]);
        setsid();                                   // become session leader
        if (ioctl(slave, TIOCSCTTY, 0) < 0)
            _exit(80);                              // no controlling terminal
        (void) tcsetpgrp(slave, getpgrp());         // and foreground group
        if (write(sync[1], "r", 1) < 0) {}
        // SIGHUP's default action terminates; just wait for it to arrive.
        for (int i = 0; i < 200; i++)
            nap(50);
        _exit(0);                                   // reached only with no SIGHUP
    }
    if (c < 0) {
        printf("tty_hangup_signal: SKIP (fork failed)\n");
        return 0;
    }
    close(slave);
    close(sync[1]);
    char rdy;
    if (read(sync[0], &rdy, 1) != 1) {
        printf("tty_hangup_signal: SKIP (child never took the tty)\n");
        kill(c, SIGKILL);
        waitpid(c, NULL, 0);
        return 0;
    }
    nap(300);

    close(master);                                  // the hangup

    int st = 0;
    pid_t w = 0;
    for (int i = 0; i < 60; i++) {
        w = waitpid(c, &st, WNOHANG);
        if (w == c)
            break;
        nap(100);
    }
    if (w != c) {
        failf("hangup delivers SIGHUP", 0, 0, 0, (uint64_t) SIGHUP, 0, 0);
        printf("FAIL: still alive 6s after the hangup -- no SIGHUP was sent\n");
        kill(c, SIGKILL);
        waitpid(c, &st, 0);
    } else {
        int sig = WIFSIGNALED(st) ? WTERMSIG(st) : 0;
        int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        if (code == 80) {
            printf("tty_hangup_signal: SKIP (TIOCSCTTY unavailable)\n");
            return 0;
        }
        if (sig != SIGHUP)
            failf("hangup delivers SIGHUP", (uint64_t) sig, (uint64_t) code, 0,
                  (uint64_t) SIGHUP, 0, 0);
        test_logf("  child died of signal %d (want %d)\n", sig, SIGHUP);
    }
    close(sync[0]);
    return finish_suite("tty_hangup_signal");
}
