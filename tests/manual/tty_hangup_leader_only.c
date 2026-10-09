// tty_hangup_leader_only.c -- a pty hangup signals the session leader, not
// its foreground group; the group hears when the leader exits.
//
// Linux's __tty_hangup -> tty_signal_session_leader sends SIGHUP and SIGCONT
// to the processes that are session leaders and to nobody else, and notes the
// foreground group (tty_old_pgrp) for the leader's exit, when disassociate_ctty
// sends that group SIGHUP and SIGCONT. So a child in the leader's own group,
// with no job control -- `su - user --session-command script` is the case it
// was found with -- survives a hangup while the leader lives, as long as the
// leader blocks SIGHUP. AOK signalled the whole foreground group at the hangup:
// the child died at once. Measured on camd (Linux 6.12).
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void nap(long ms) {
    struct timespec t = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&t, NULL);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(60);
    struct sigaction was;
    if (sigaction(SIGHUP, NULL, &was) == 0 && was.sa_handler == SIG_IGN) {
        printf("tty_hangup_leader_only: SKIP (SIGHUP is ignored here -- started under nohup?)\n");
        return 0;
    }
    int master, slave;
    if (openpty(&master, &slave, NULL, NULL, NULL) < 0) {
        printf("tty_hangup_leader_only: SKIP (openpty: %s)\n", strerror(errno));
        return 0;
    }
    // The member is the leader's child; when the leader exits it comes here,
    // so that how it ended can be read.
    prctl(PR_SET_CHILD_SUBREAPER, 1);
    int report[2];
    pipe(report);

    pid_t leader = fork();
    if (leader == 0) {
        close(master);
        close(report[0]);
        setsid();
        if (ioctl(slave, TIOCSCTTY, 0) < 0)
            _exit(80);
        tcsetpgrp(slave, getpgrp());
        sigset_t hup;
        sigemptyset(&hup);
        sigaddset(&hup, SIGHUP);
        sigprocmask(SIG_BLOCK, &hup, NULL);
        int ready[2];
        pipe(ready);
        pid_t member = fork();
        if (member == 0) {
            // in the leader's process group, which is the foreground group
            sigprocmask(SIG_UNBLOCK, &hup, NULL);
            write(ready[1], "r", 1);
            for (;;)
                pause();
        }
        char c;
        read(ready[0], &c, 1);
        int32_t m = member;
        write(report[1], &m, sizeof m);
        // the hangup happens now (the parent closes the master); then what
        // reached whom
        nap(1500);
        sigset_t pending;
        sigpending(&pending);
        int st;
        char verdict[2] = {
            sigismember(&pending, SIGHUP) ? 'P' : '-',              // the leader was signalled
            waitpid(member, &st, WNOHANG) == 0 ? 'A' : '-',         // the member lives on
        };
        write(report[1], verdict, 2);
        _exit(0);       // and now the member hears: the leader's exit
    }
    close(slave);
    close(report[1]);
    int32_t member;
    if (read(report[0], &member, sizeof member) != sizeof member) {
        int st;
        waitpid(leader, &st, 0);
        printf("tty_hangup_leader_only: SKIP (the leader could not take the terminal: status %#x)\n", st);
        return 0;
    }
    nap(200);
    close(master);                      // the hangup

    char verdict[2] = {0, 0};
    read(report[0], verdict, 2);
    CHECK(verdict[0] == 'P', "the session leader has no SIGHUP pending after the hangup");
    CHECK(verdict[1] == 'A', "the hangup killed a member of the foreground group that is not a leader");
    int st = 0;
    waitpid(leader, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "the leader ended with status %#x", st);
    pid_t w = 0;
    for (int i = 0; i < 50 && w == 0; i++) {
        w = waitpid(member, &st, WNOHANG);
        if (w == 0)
            nap(100);
    }
    CHECK(w == member && WIFSIGNALED(st) && WTERMSIG(st) == SIGHUP,
          "the member after the leader's exit: %s, status %#x (want killed by SIGHUP)",
          w == member ? "ended" : w < 0 ? "gone already (the leader reaped it)" : "still running", st);
    if (w == 0) {
        kill(member, SIGKILL);
        waitpid(member, NULL, 0);
    }
    printf("tty_hangup_leader_only: %s (%d checks, %d failures)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures != 0;
}
