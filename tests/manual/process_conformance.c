/*
 * process_conformance.c — self-checking regression lock for process/thread
 * lifecycle Linux-conformance fixes found by the differential harness
 * (tests/remote/corpus_proc, vs mint's real Linux). Each check encodes the
 * Linux-correct behavior directly (no oracle); exits 0 and prints
 * "process_conformance: PASS" on success.
 *
 * Regression-locks, in particular:
 *   - setpgid(pid, <0)            -> EINVAL  (was EPERM)
 *   - setpgid(exec'd child)       -> EACCES  (was silently OK)
 *   - waitpid(..., WEXITED)       -> EINVAL  (waitid-only flag; was OK)
 *   - wait4/waitid WCONTINUED reports a continued child (was never reported)
 * plus baseline exit-status encoding, setsid, fork fd-sharing, and WUNTRACED.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#include "test_common.h"

static void check(const char *label, long got, long exp) {
    int bad = got != exp;
    test_log_if(bad, "%s: got=%ld exp=%ld\n", label, got, exp);
    if (bad)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) exp, 0, 0);
}

static volatile sig_atomic_t alarm_fired;
static void on_alarm(int s) { (void) s; alarm_fired = 1; }

/* re-exec target for the setpgid-after-exec check: signal readiness, then sleep */
static int sleeper(int argc, char **argv) {
    if (argc >= 3) {
        int fd = atoi(argv[2]);
        char b = 'x';
        ssize_t w = write(fd, &b, 1);
        (void) w;
    }
    for (;;)
        pause();
    return 0;
}

/* exit(n) -> WIFEXITED, WEXITSTATUS = n & 0xff; signal death -> WIFSIGNALED */
static void test_exit_status(void) {
    pid_t pid = fork();
    if (pid == 0) _exit(42);
    int st = 0;
    waitpid(pid, &st, 0);
    check("exit.WIFEXITED", WIFEXITED(st) ? 1 : 0, 1);
    check("exit.WEXITSTATUS", WIFEXITED(st) ? WEXITSTATUS(st) : -1, 42);
    check("exit.rawword", st, 42 << 8);

    pid = fork();
    if (pid == 0) { signal(SIGTERM, SIG_DFL); raise(SIGTERM); _exit(0); }
    waitpid(pid, &st, 0);
    check("sig.WIFSIGNALED", WIFSIGNALED(st) ? 1 : 0, 1);
    check("sig.WTERMSIG", WIFSIGNALED(st) ? WTERMSIG(st) : -1, SIGTERM);

    /* truncation to the low 8 bits */
    pid = fork();
    if (pid == 0) _exit(0x1ff);
    waitpid(pid, &st, 0);
    check("exit.trunc", WIFEXITED(st) ? WEXITSTATUS(st) : -1, 0xff);
}

static void test_setsid(void) {
    pid_t pid = fork();
    if (pid == 0) {
        pid_t sid = setsid();
        _exit(sid == getpid() && getsid(0) == getpid() && getpgrp() == getpid() ? 0 : 1);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    check("setsid.basic", WIFEXITED(st) ? WEXITSTATUS(st) : -1, 0);
}

/* setpgid(0, <negative>) -> EINVAL (not EPERM) */
static void test_setpgid_negative(void) {
    pid_t pid = fork();
    if (pid == 0) {
        errno = 0;
        int rc = setpgid(0, -3);
        _exit(rc == -1 && errno == EINVAL ? 0 : (errno ? errno : 99));
    }
    int st = 0;
    waitpid(pid, &st, 0);
    check("setpgid.negative.EINVAL", WIFEXITED(st) ? WEXITSTATUS(st) : -1, 0);
}

/* setpgid of a child that has already execve()'d -> EACCES */
static void test_setpgid_after_exec(char **argv) {
    int p[2];
    if (pipe(p) != 0) { check("setpgid.afterexec.pipe", 0, 1); return; }
    pid_t pid = fork();
    if (pid == 0) {
        close(p[0]);
        char fdbuf[16];
        snprintf(fdbuf, sizeof fdbuf, "%d", p[1]);
        char *const cargv[] = { argv[0], (char *) "--sleeper", fdbuf, NULL };
        execv(argv[0], cargv);
        _exit(127);
    }
    close(p[1]);
    char b = 0;
    ssize_t r;
    do { r = read(p[0], &b, 1); } while (r < 0 && errno == EINTR);
    close(p[0]);
    errno = 0;
    int rc = setpgid(pid, pid);
    int got_errno = errno;
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    check("setpgid.afterexec.EACCES", (rc == -1 && got_errno == EACCES) ? 0 : (got_errno ? got_errno : 99), 0);
}

/* waitpid with the waitid-only WEXITED flag -> EINVAL */
static void test_waitpid_wexited(void) {
    pid_t pid = fork();
    if (pid == 0) _exit(0);
    int st = 0;
    errno = 0;
    int rc = waitpid(pid, &st, WEXITED);
    int got_errno = errno;
    check("waitpid.WEXITED.EINVAL", (rc == -1 && got_errno == EINVAL) ? 0 : (got_errno ? got_errno : 99), 0);
    waitpid(pid, &st, 0);   /* now reap for real */
}

/* WNOHANG returns 0 while the child runs, then the pid once it exits */
static void test_wnohang(void) {
    pid_t pid = fork();
    if (pid == 0) { usleep(120 * 1000); _exit(5); }
    int st = 0;
    int rc0 = waitpid(pid, &st, WNOHANG);
    check("wnohang.alive", rc0, 0);
    int rc1;
    do { rc1 = waitpid(pid, &st, 0); } while (rc1 < 0 && errno == EINTR);
    check("wnohang.exit", WIFEXITED(st) ? WEXITSTATUS(st) : -1, 5);
}

/* WUNTRACED reports a job-control stop; WCONTINUED reports the resume */
static void test_stop_continue(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_alarm;
    sigaction(SIGALRM, &sa, NULL);

    int sync[2];
    if (pipe(sync) != 0) { check("stopcont.pipe", 0, 1); return; }
    pid_t pid = fork();
    if (pid == 0) {
        close(sync[1]);
        raise(SIGSTOP);
        char b;
        ssize_t r; do { r = read(sync[0], &b, 1); } while (r < 0 && errno == EINTR);
        _exit(33);
    }
    close(sync[0]);
    int st = 0;

    alarm_fired = 0; alarm(3);
    int rc = waitpid(pid, &st, WUNTRACED);
    alarm(0);
    check("stop.WIFSTOPPED", (rc == pid && WIFSTOPPED(st)) ? 1 : 0, 1);
    check("stop.WSTOPSIG", (rc == pid && WIFSTOPPED(st)) ? WSTOPSIG(st) : -1, SIGSTOP);

    kill(pid, SIGCONT);
    alarm_fired = 0; alarm(3);
    rc = waitpid(pid, &st, WCONTINUED);
    alarm(0);
    check("cont.WIFCONTINUED", (rc == pid && WIFCONTINUED(st)) ? 1 : 0, 1);
    if (rc != pid)
        test_log_if(1, "WCONTINUED not reported (alarm_fired=%d)\n", alarm_fired);

    char go = 'g';
    ssize_t w = write(sync[1], &go, 1); (void) w;
    close(sync[1]);
    do { rc = waitpid(pid, &st, 0); } while (rc < 0 && errno == EINTR);
    check("stopcont.finalexit", WIFEXITED(st) ? WEXITSTATUS(st) : -1, 33);
}

/* fork shares the open file description: a child's read advances the parent's
 * file offset (single shared position) */
static void test_fork_shared_offset(void) {
    char path[] = "/tmp/procconfXXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { check("forkoff.mkstemp", 0, 1); return; }
    ssize_t w = write(fd, "AB", 2); (void) w;
    int rd = open(path, O_RDONLY);
    unlink(path);
    if (rd < 0) { close(fd); check("forkoff.open", 0, 1); return; }

    pid_t pid = fork();
    if (pid == 0) {
        char c; ssize_t n = read(rd, &c, 1);
        _exit(n == 1 && c == 'A' ? 0 : 1);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    check("forkoff.child_read_A", WIFEXITED(st) ? WEXITSTATUS(st) : -1, 0);
    char c = '?';
    ssize_t n = read(rd, &c, 1);
    check("forkoff.parent_sees_B", (n == 1 && c == 'B') ? 1 : 0, 1);
    close(fd); close(rd);
}

// kill() on an exited-but-unreaped (zombie) child returns 0 on Linux -- the
// process still exists until reaped; the signal is just discarded. AOK
// returned ESRCH, which stress-ng hits with kill(child, SIGKILL) right
// before wait4().
static void test_kill_zombie(void) {
    pid_t pid = fork();
    if (pid == 0) _exit(0);
    usleep(300000); // let the child become a zombie; do NOT reap yet
    errno = 0;
    check("kill_zombie.ret", kill(pid, SIGKILL), 0);
    int st;
    check("kill_zombie.reap", waitpid(pid, &st, 0), pid);
}

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "--sleeper") == 0)
        return sleeper(argc, argv);

    test_init(argc, argv);
    test_exit_status();
    test_setsid();
    test_setpgid_negative();
    test_setpgid_after_exec(argv);
    test_waitpid_wexited();
    test_wnohang();
    test_stop_continue();
    test_fork_shared_offset();
    test_kill_zombie();
    return finish_suite("process_conformance");
}
