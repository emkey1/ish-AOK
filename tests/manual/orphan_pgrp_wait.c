// Two things about who can be waited for, and who resumes a stopped job.
//
//   POSIX: when a process's exit leaves a process group ORPHANED and that
//   group still holds a stopped member, the group is sent SIGHUP and then
//   SIGCONT. A group is orphaned when no member has a parent in a different
//   group of the SAME session -- nobody outside it is left who could resume
//   it. Without the rule its stopped members stay stopped for ever: the shell
//   that stopped them is gone, and a ^Z'd job whose shell then exited sat in
//   state T until the guest was rebooted.
//
//   A thread id is not a waitable child. Linux matches only thread-group
//   LEADERS for a non-tracer, so waiting on a child's thread tid fails
//   immediately with ECHILD. AOK resolved the tid to its leader, which made
//   the leader look like the match: WNOHANG returned 0 as though the child
//   were merely still running, telling the caller to keep polling a pid that
//   could never be reportable.
//
// Measured against x86_64 glibc on Linux 6.12.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_common.h"

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-58s got=%-10ld want=%ld\n", label, got, want);
}

// The process state character from /proc/<pid>/stat, or '?' if it is gone.
// The comm field is parenthesised and may contain spaces, so it is read from
// the LAST ')'.
static char proc_state(pid_t pid) {
    char path[64], buf[512];
    snprintf(path, sizeof path, "/proc/%d/stat", (int) pid);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return '?';
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0)
        return '?';
    buf[n] = '\0';
    char *after = strrchr(buf, ')');
    return after != NULL && after[1] == ' ' ? after[2] : '?';
}

static int tidpipe[2];
static void *report_tid(void *arg) {
    (void) arg;
    pid_t tid = (pid_t) syscall(SYS_gettid);
    ssize_t w = write(tidpipe[1], &tid, sizeof tid);
    (void) w;
    for (;;)
        pause();
    return NULL;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(120));

    // ---- waiting on a child's THREAD id -----------------------------------
    {
        ck("pipe", pipe(tidpipe), 0);
        fflush(NULL);
        pid_t c = fork();
        if (c == 0) {
            close(tidpipe[0]);
            alarm(30);
            pthread_t t;
            if (pthread_create(&t, NULL, report_tid, NULL) != 0)
                _exit(1);
            for (;;)
                pause();
        }
        close(tidpipe[1]);
        pid_t tid = 0;
        ssize_t got = read(tidpipe[0], &tid, sizeof tid);
        close(tidpipe[0]);
        ck("the child reported its thread's tid", got == (ssize_t) sizeof tid ? 1 : 0, 1);
        ck("  and it is not the child's pid", tid != c ? 1 : 0, 1);
        int st;
        errno = 0;
        long r = waitpid(tid, &st, WNOHANG);
        ck("waitpid on it with WNOHANG fails", r < 0 ? 1 : 0, 1);
        ck("  with ECHILD", r < 0 ? errno : 0, ECHILD);
        // ...and the blocking form fails the same way rather than hanging.
        errno = 0;
        r = waitpid(tid, &st, 0);
        ck("the blocking form fails too", r < 0 ? 1 : 0, 1);
        ck("  with ECHILD", r < 0 ? errno : 0, ECHILD);
        // The child ITSELF is still waitable, so this is a check on thread
        // ids and not a blanket refusal.
        errno = 0;
        r = waitpid(c, &st, WNOHANG);
        ck("waiting on the child's own pid still works", r, 0);
        kill(c, SIGKILL);
        waitpid(c, &st, 0);
    }

    // ---- an orphaned group with a stopped member --------------------------
    // A middle process makes its own group, forks a leaf that stops itself,
    // and then exits. That exit orphans the group -- nobody outside it in the
    // session is left -- so the leaf must be hupped and continued.
    {
        int pfd[2];
        ck("pipe", pipe(pfd), 0);
        fflush(NULL);
        pid_t middle = fork();
        if (middle == 0) {
            close(pfd[0]);
            alarm(30);
            setpgid(0, 0);
            pid_t leaf = fork();
            if (leaf == 0) {
                raise(SIGSTOP);
                _exit(42);
            }
            usleep(400000);
            ssize_t w = write(pfd[1], &leaf, sizeof leaf);
            (void) w;
            usleep(200000);
            _exit(0);
        }
        close(pfd[1]);
        pid_t leaf = 0;
        ssize_t got = read(pfd[0], &leaf, sizeof leaf);
        close(pfd[0]);
        ck("the middle process reported its leaf", got == (ssize_t) sizeof leaf ? 1 : 0, 1);
        int st;
        waitpid(middle, &st, 0);
        if (got == (ssize_t) sizeof leaf) {
            // Give the exit's notification time to land.
            for (int i = 0; i < 40 && proc_state(leaf) == 'T'; i++)
                usleep(100000);
            char state = proc_state(leaf);
            test_logf("  %-58s state='%c'\n", "leaf after its group was orphaned", state);
            // Gone, dead-but-unreaped, or running -- anything except still
            // stopped, which is what it was before the rule existed.
            ck("the stopped member is no longer stopped", state == 'T' ? 1 : 0, 0);
            kill(leaf, SIGCONT);
            kill(leaf, SIGKILL);
        }
    }

    // ---- the control: a group that is NOT orphaned keeps its stopped member
    // Same shape, except the middle process stays alive, so someone outside
    // the group and inside the session can still resume it.
    {
        int pfd[2];
        ck("pipe", pipe(pfd), 0);
        fflush(NULL);
        pid_t middle = fork();
        if (middle == 0) {
            close(pfd[0]);
            alarm(30);
            setpgid(0, 0);
            pid_t leaf = fork();
            if (leaf == 0) {
                raise(SIGSTOP);
                _exit(42);
            }
            usleep(400000);
            ssize_t w = write(pfd[1], &leaf, sizeof leaf);
            (void) w;
            for (;;)
                pause();            // stays alive: the group is not orphaned
        }
        close(pfd[1]);
        pid_t leaf = 0;
        ssize_t got = read(pfd[0], &leaf, sizeof leaf);
        close(pfd[0]);
        if (got == (ssize_t) sizeof leaf) {
            usleep(1200000);
            char state = proc_state(leaf);
            test_logf("  %-58s state='%c'\n", "leaf while its group still has a parent", state);
            // Still stopped: nothing orphaned it, so nothing hupped it. A fix
            // that hupped every stopped group would pass the case above and
            // fail here.
            ck("a non-orphaned group's member stays stopped", state, 'T');
            kill(leaf, SIGCONT);
            kill(leaf, SIGKILL);
        }
        kill(middle, SIGKILL);
        int st;
        waitpid(middle, &st, 0);
    }

    return finish_suite("orphan_pgrp_wait");
}
