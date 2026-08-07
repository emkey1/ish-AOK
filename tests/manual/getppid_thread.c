// getppid_thread.c — the parent reported to a child must be the parent
// PROCESS, not the parent THREAD.
//
// Linux reports `real_parent->tgid` everywhere a parent pid is user-visible:
// getppid(2) is task_tgid_vnr(real_parent), /proc/<pid>/stat field 4 and
// /proc/<pid>/status PPid go through task_ppid_nr() == task_tgid_nr(
// real_parent), and taskstats ac_ppid is task_tgid_nr_ns(real_parent). iSH
// reported `parent->pid`, which is the parent's THREAD id (sys_gettid's value,
// not sys_getpid's). The two are identical whenever the forking task is its
// group's leader, which is why this hid: it only diverges when a NON-LEADER
// THREAD forks, and then every child sees a "parent" that is not a process id
// at all.
//
// Why it mattered (GH #523, yay on Arch ARM64 dying with "signal: terminated"
// on every operation). Go's syscall.forkAndExecInChild runs this in the child
// between fork and exec whenever SysProcAttr.Pdeathsig is set:
//
//     ppid = getpid()                       // captured in the PARENT (a TGID)
//     ...
//     prctl(PR_SET_PDEATHSIG, sig)
//     if getppid() != ppid {                // "my parent already died"
//         kill(getpid(), Pdeathsig)
//     }
//
// A Go program multiplexes goroutines across OS threads, so the fork runs on
// whichever thread the runtime picked. On a non-leader thread getppid()
// returned that thread's tid, the comparison failed, and the child SIGTERMed
// itself before ever reaching execve. The parent reaped CLD_KILLED/SIGTERM,
// which os/exec renders as the string "signal: terminated" -- with no error
// from the command itself, because the command never ran. Running the same
// commands by hand always worked, since a shell forks from its only thread.
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include "test_common.h"

static pid_t raw_getppid(void) { return (pid_t) syscall(SYS_getppid); }
static pid_t raw_getpid(void)  { return (pid_t) syscall(SYS_getpid); }
static pid_t raw_gettid(void)  { return (pid_t) syscall(SYS_gettid); }

static pid_t the_tgid;

static int is_true(const char *label, int cond) {
    if (cond) { test_logf("ok   %s\n", label); return 1; }
    printf("FAIL %s\n", label);
    failures_total++;
    return 0;
}

// /proc/<pid>/stat field 4. comm (field 2) is parenthesized and may contain
// spaces and parens, so parse after the LAST ')'.
static pid_t proc_stat_ppid(pid_t pid) {
    char path[64], buf[4096];
    snprintf(path, sizeof path, "/proc/%d/stat", (int) pid);
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return -1;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    char *close_paren = strrchr(buf, ')');
    if (close_paren == NULL)
        return -1;
    char state = 0;
    int ppid = -1;
    if (sscanf(close_paren + 1, " %c %d", &state, &ppid) != 2)
        return -1;
    return (pid_t) ppid;
}

static pid_t proc_status_ppid(pid_t pid) {
    char path[64], line[512];
    snprintf(path, sizeof path, "/proc/%d/status", (int) pid);
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return -1;
    pid_t ppid = -1;
    while (fgets(line, sizeof line, f) != NULL) {
        int v;
        if (sscanf(line, "PPid: %d", &v) == 1) { ppid = (pid_t) v; break; }
    }
    fclose(f);
    return ppid;
}

struct result {
    pid_t child_getppid;    // what the child saw
    pid_t forking_tid;      // tid of the task that forked it
    pid_t child_pid;
    pid_t stat_ppid;        // /proc/<child>/stat field 4
    pid_t status_ppid;      // /proc/<child>/status PPid
    int   pdeathsig_would_self_kill;
};

// Fork a child here (on whatever task calls this) and collect what the child
// sees, plus whether Go's Pdeathsig test would have made it kill itself.
static void fork_and_probe(struct result *out) {
    int fd[2];
    if (pipe(fd) < 0) { out->child_pid = -1; return; }
    out->forking_tid = raw_gettid();

    pid_t child = fork();
    if (child == 0) {
        close(fd[0]);
        // Exactly Go's sequence, minus the actual self-kill.
        syscall(SYS_prctl, PR_SET_PDEATHSIG, SIGTERM, 0, 0, 0);
        pid_t seen = raw_getppid();
        (void) !write(fd[1], &seen, sizeof seen);
        // Stay alive so the forking task can read our procfs entries. Scaled
        // by ISH_TEST_WATCHDOG_SCALE for the same reason the watchdogs are:
        // under the release procedure's 4-way concurrent multi-arch run, a
        // normally-instant read can stretch by orders of magnitude, and if it
        // landed after we exited the reads would see a reparented child.
        usleep(400 * 1000 * test_watchdog_secs(1));
        _exit(0);
    }
    close(fd[1]);
    out->child_pid = child;
    pid_t seen = 0;
    if (read(fd[0], &seen, sizeof seen) != (ssize_t) sizeof seen)
        seen = -1;
    close(fd[0]);
    out->child_getppid = seen;
    out->pdeathsig_would_self_kill = (seen != the_tgid);
    // Read procfs from HERE, while the forking task is still alive. Once a
    // worker thread exits, its children are reparented and the parent pid on
    // record becomes the leader's -- correct by accident, and it hides the bug.
    out->stat_ppid = proc_stat_ppid(child);
    out->status_ppid = proc_status_ppid(child);
}

static void *thread_main(void *arg) {
    fork_and_probe((struct result *) arg);
    return NULL;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(120));

    the_tgid = raw_getpid();
    is_true("main thread is the group leader", raw_gettid() == the_tgid);

    // Control: the leader forks. This always worked.
    struct result leader = {0};
    fork_and_probe(&leader);
    if (leader.child_pid > 0) {
        is_true("leader.child_getppid == tgid", leader.child_getppid == the_tgid);
        is_true("leader.stat_ppid == tgid", leader.stat_ppid == the_tgid);
        is_true("leader.status_ppid == tgid", leader.status_ppid == the_tgid);
        waitpid(leader.child_pid, NULL, 0);
    } else {
        is_true("leader.fork", 0);
    }

    // The regression: a NON-LEADER thread forks.
    struct result worker = {0};
    pthread_t th;
    if (pthread_create(&th, NULL, thread_main, &worker) != 0)
        return is_true("pthread_create", 0), finish_suite("getppid_thread");
    pthread_join(th, NULL);

    if (worker.child_pid <= 0) {
        is_true("worker.fork", 0);
        return finish_suite("getppid_thread");
    }

    test_log_if(worker.forking_tid == the_tgid,
                "note: worker tid == tgid (%d), the divergence cannot show here\n",
                (int) the_tgid);
    is_true("worker thread has its own tid", worker.forking_tid != the_tgid);

    test_log_if(worker.child_getppid != the_tgid,
                "child of worker thread saw ppid=%d; wanted tgid=%d (worker tid=%d)\n",
                (int) worker.child_getppid, (int) the_tgid, (int) worker.forking_tid);
    is_true("worker.child_getppid == tgid (not the forking tid)",
            worker.child_getppid == the_tgid);

    test_log_if(worker.stat_ppid != the_tgid, "/proc/%d/stat ppid=%d; wanted %d\n",
                (int) worker.child_pid, (int) worker.stat_ppid, (int) the_tgid);
    is_true("worker.stat_ppid == tgid", worker.stat_ppid == the_tgid);

    test_log_if(worker.status_ppid != the_tgid, "/proc/%d/status PPid=%d; wanted %d\n",
                (int) worker.child_pid, (int) worker.status_ppid, (int) the_tgid);
    is_true("worker.status_ppid == tgid", worker.status_ppid == the_tgid);

    // The consequence, stated as its own assertion: with this wrong, every Go
    // program that sets Pdeathsig loses children to a self-inflicted SIGTERM
    // before exec, reported only as "signal: terminated".
    is_true("go pdeathsig idiom would not self-kill",
            !worker.pdeathsig_would_self_kill);

    waitpid(worker.child_pid, NULL, 0);
    return finish_suite("getppid_thread");
}
