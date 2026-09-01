// posix_timer_fork.c — POSIX timers (timer_create) must NOT be inherited across
// fork(). AOK's tgroup_copy shallow-copied the parent's posix_timers[] array
// (including the internal struct timer * pointers), so a forked child that
// called timer_delete on the inherited id freed a timer the parent still owned
// -- a double free / use-after-free of the running timer thread that crashed
// the emulator (found via stress-ng --cpu-sched, whose children delete the
// inherited timer at startup). This asserts the Linux behavior: the child
// inherits no timers (timer_delete on the parent's id -> EINVAL), the parent's
// timer keeps working, and nothing crashes.
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include "test_common.h"

static int is_ok(const char *label, long r) {
    if (r >= 0) { test_logf("ok   %s\n", label); return 1; }
    printf("FAIL %s: ret=%ld errno=%d, wanted success\n", label, r, errno);
    failures_total++;
    return 0;
}
static int is_true(const char *label, int cond) {
    if (cond) { test_logf("ok   %s\n", label); return 1; }
    printf("FAIL %s\n", label);
    failures_total++;
    return 0;
}

int main(int argc, char **argv) {
    test_init(argc, argv);

    // SIGEV_SIGNAL/SIGRTMIN timer, blocked so it can't kill us if it fires.
    sigset_t block; sigemptyset(&block); sigaddset(&block, SIGRTMIN);
    sigprocmask(SIG_BLOCK, &block, NULL);

    timer_t t;
    struct sigevent sev; memset(&sev, 0, sizeof sev);
    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = SIGRTMIN;
    if (!is_ok("timer_create.parent", timer_create(CLOCK_REALTIME, &sev, &t)))
        return finish_suite("posix_timer_fork");

    // Arm it recurring so the internal timer thread is live across the fork --
    // that is the window the double-free raced against.
    struct itimerspec its; memset(&its, 0, sizeof its);
    its.it_value.tv_nsec = 5 * 1000000;      // 5ms
    its.it_interval.tv_nsec = 5 * 1000000;
    is_ok("timer_settime.parent", timer_settime(t, 0, &its, NULL));

    pid_t pid = fork();
    if (pid == 0) {
        // Child inherited NO timers: deleting the parent's id must FAIL, not
        // succeed and free the parent's timer. (Before the fix this returned
        // success, double-freed, and crashed the whole emulator.) Test the
        // non-zero return rather than a specific errno -- musl's timer_delete
        // wrapper doesn't normalize errno uniformly across AOK's return path.
        int deleted_inherited = (timer_delete(t) == 0);
        // The child can create and delete its OWN timer on the copied group.
        timer_t ct;
        int cr = timer_create(CLOCK_REALTIME, &sev, &ct);
        if (cr == 0) timer_delete(ct);
        _exit(!deleted_inherited && cr == 0 ? 0 : 1);
    }
    is_true("fork", pid > 0);

    int status = 0;
    waitpid(pid, &status, 0);
    // The important regression: the child neither crashed (double-free) nor
    // freed the parent's timer.
    is_true("child.no_crash", !WIFSIGNALED(status));
    is_true("child.timer_not_inherited", WIFEXITED(status) && WEXITSTATUS(status) == 0);

    // Parent still owns its timer: re-arm, disarm, and delete all succeed.
    is_ok("timer_settime.parent.again", timer_settime(t, 0, &its, NULL));
    struct itimerspec zero; memset(&zero, 0, sizeof zero);
    is_ok("timer_disarm.parent", timer_settime(t, 0, &zero, NULL));
    is_ok("timer_delete.parent", timer_delete(t));
    // Deleting it a second time must fail (id no longer valid) -- don't crash.
    is_true("timer_delete.parent.twice_fails", timer_delete(t) != 0);

    return finish_suite("posix_timer_fork");
}
