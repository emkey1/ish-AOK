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
//   It is the PROCESS's exit that counts, not a thread's. Linux applies the
//   rule once the whole thread group is gone; AOK applied it at every thread's
//   exit, and since the orphan test leaves the exiting process out, a worker
//   thread's exit found its own group orphaned and sent it SIGHUP and SIGCONT
//   -- killing the process, still running, along with its stopped child. A
//   leader that exits before its other threads is the same case: the rule
//   waits for the last of them.
//
//   An exit can orphan a CHILD's group as well as its own. A shell puts each
//   job in a group of its own, so when the shell exits with a job stopped,
//   the group left without a way back is the job's. Linux asks about each
//   child it hands to another process (reparent_leader) as well as about the
//   exiting process's own group (exit_notify); AOK asked only the second, and
//   the job stayed in state T for ever. A group with another way back -- a
//   second shell, still there, with a job in it, or a subreaper in the
//   session that has adopted a member -- is not orphaned by the first one's
//   exit, which is why both questions are asked once the children have moved.
//   init, which adopts every other orphan, is never a way back: in the CLI it
//   shares the tests' session, and counted, it would keep every group it
//   adopted into from ever being orphaned.
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
#include <linux/futex.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_common.h"

// musl on i386 names the 32-bit-time calls for what they take.
#if !defined(SYS_futex) && defined(SYS_futex_time32)
#define SYS_futex SYS_futex_time32
#endif

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

// A child in the caller's group that stops itself; returns once it has.
static pid_t stopped_leaf(void) {
    pid_t leaf = fork();
    if (leaf == 0) {
        raise(SIGSTOP);
        _exit(42);
    }
    int st;
    while (waitpid(leaf, &st, WUNTRACED) < 0 && errno == EINTR)
        continue;
    return leaf;
}

// The leaf's state once it has had up to 4s to stop being stopped.
static char state_after_hangup(pid_t leaf) {
    for (int i = 0; i < 40 && proc_state(leaf) == 'T'; i++)
        usleep(100000);
    return proc_state(leaf);
}

// A child that stops itself in process group `pgid` -- 0 for a group of its
// own, as a shell puts each job -- and returns once it has. SIGHUP is left at
// its default and a resumed job waits for good, so a job that was hung up is
// dead and one that was merely continued is still there to be seen.
static pid_t stopped_job(pid_t pgid) {
    pid_t job = fork();
    if (job == 0) {
        setpgid(0, pgid);
        raise(SIGSTOP);
        for (;;)
            pause();
    }
    setpgid(job, pgid != 0 ? pgid : job);   // a shell's second setpgid: no race
    int st;
    while (waitpid(job, &st, WUNTRACED) < 0 && errno == EINTR)
        continue;
    return job;
}

// Dead, whether or not init has reaped it yet: what SIGHUP does to a job.
static int is_dead(char state) {
    return state == 'Z' || state == 'X' || state == '?';
}

// The job's state once it has had up to 4s to die. A hung-up job is seen
// running first: the SIGCONT that follows the SIGHUP is what lets it take it.
static char state_after_death(pid_t job) {
    for (int i = 0; i < 40 && !is_dead(proc_state(job)); i++)
        usleep(100000);
    return proc_state(job);
}

// This thread exits alone, with the raw syscall, having pointed its tid word
// at `word`: the kernel clears it and wakes a waiter as the thread goes.
// pthread_exit is avoided for musl's sake, whose join would never return for
// the leader.
static void thread_exit(volatile int *word) {
    syscall(SYS_set_tid_address, word);
    syscall(SYS_exit, 0);
}

// A shared wait: the kernel's wake at thread exit is a shared one. The exit
// clears the word before it is done, so the caller gives it time to finish.
static void wait_thread_gone(volatile int *word) {
    int v;
    while ((v = __atomic_load_n(word, __ATOMIC_ACQUIRE)) != 0)
        syscall(SYS_futex, word, FUTEX_WAIT, v, NULL, NULL, 0);
    for (unsigned i = 0; i < test_watchdog_secs(1); i++)
        usleep(300000);
}

static volatile int worker_word = 1;
static void *worker_exits(void *arg) {
    (void) arg;
    thread_exit(&worker_word);
    return NULL;
}

static volatile int leader_word = 1;
static struct {
    int fd;
    pid_t leaf;
} survivor;
static void *survivor_main(void *arg) {
    (void) arg;
    wait_thread_gone(&leader_word);
    char state = proc_state(survivor.leaf);
    ssize_t w = write(survivor.fd, &state, 1);
    (void) w;
    _exit(0);                   // the last thread: now the process goes
    return NULL;
}

static volatile int forker_word = 1;
static pid_t forker_job;
// Forks a job, so that the job's parent is this thread, and exits alone.
static void *fork_job_then_exit(void *arg) {
    (void) arg;
    __atomic_store_n(&forker_job, stopped_job(0), __ATOMIC_RELEASE);
    thread_exit(&forker_word);
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

    // ---- a THREAD's exit orphans nothing ----------------------------------
    // The orphaned shape again, except that the middle process first loses a
    // worker thread. It is still there, so the leaf stays stopped, and the
    // middle process -- SIGHUP left at its default -- is not hung up. Then it
    // exits for real, and the rule applies as it does to any process.
    {
        int pfd[2];
        ck("pipe", pipe(pfd), 0);
        fflush(NULL);
        pid_t middle = fork();
        if (middle == 0) {
            close(pfd[0]);
            alarm(30);
            setpgid(0, 0);
            pid_t leaf = stopped_leaf();
            ssize_t w = write(pfd[1], &leaf, sizeof leaf);
            pthread_t t;
            if (pthread_create(&t, NULL, worker_exits, NULL) == 0)
                wait_thread_gone(&worker_word);
            char state = proc_state(leaf);
            w = write(pfd[1], &state, 1);
            (void) w;
            _exit(0);               // now the process goes: the group is orphaned
        }
        close(pfd[1]);
        pid_t leaf = 0;
        char state = '?';
        ssize_t got = read(pfd[0], &leaf, sizeof leaf);
        ssize_t got_state = read(pfd[0], &state, 1);
        close(pfd[0]);
        int st = 0;
        waitpid(middle, &st, 0);
        ck("the middle process reported its leaf", got == (ssize_t) sizeof leaf ? 1 : 0, 1);
        test_logf("  %-58s state='%c'\n", "leaf after a worker thread's exit",
                  got_state == 1 ? state : '?');
        ck("a worker thread's exit leaves the member stopped", got_state == 1 ? state : '?', 'T');
        ck("  and does not hang up its own process", WIFSIGNALED(st) ? WTERMSIG(st) : 0, 0);
        if (got == (ssize_t) sizeof leaf) {
            state = state_after_hangup(leaf);
            test_logf("  %-58s state='%c'\n", "leaf after the process's own exit", state);
            ck("the process's own exit then orphans the group", state == 'T' ? 1 : 0, 0);
            kill(leaf, SIGCONT);
            kill(leaf, SIGKILL);
        }
    }

    // ---- nor does the leader's, while another thread runs -----------------
    // The leader exits first, leaving a thread behind: the process is still
    // there. The rule applies when that last thread exits.
    {
        int pfd[2];
        ck("pipe", pipe(pfd), 0);
        fflush(NULL);
        pid_t middle = fork();
        if (middle == 0) {
            close(pfd[0]);
            alarm(30);
            setpgid(0, 0);
            survivor.fd = pfd[1];
            survivor.leaf = stopped_leaf();
            ssize_t w = write(pfd[1], &survivor.leaf, sizeof survivor.leaf);
            (void) w;
            pthread_t t;
            if (pthread_create(&t, NULL, survivor_main, NULL) != 0)
                _exit(1);
            thread_exit(&leader_word);
        }
        close(pfd[1]);
        pid_t leaf = 0;
        char state = '?';
        ssize_t got = read(pfd[0], &leaf, sizeof leaf);
        ssize_t got_state = read(pfd[0], &state, 1);
        close(pfd[0]);
        int st = 0;
        waitpid(middle, &st, 0);
        ck("the middle process reported its leaf", got == (ssize_t) sizeof leaf ? 1 : 0, 1);
        test_logf("  %-58s state='%c'\n", "leaf after the leader's exit",
                  got_state == 1 ? state : '?');
        ck("the leader's exit leaves the member stopped", got_state == 1 ? state : '?', 'T');
        ck("  and does not hang up its own process", WIFSIGNALED(st) ? WTERMSIG(st) : 0, 0);
        ck("  which exits as its last thread did", WIFEXITED(st) ? WEXITSTATUS(st) : -1, 0);
        if (got == (ssize_t) sizeof leaf) {
            state = state_after_hangup(leaf);
            test_logf("  %-58s state='%c'\n", "leaf after the last thread's exit", state);
            ck("the last thread's exit orphans the group", state == 'T' ? 1 : 0, 0);
            kill(leaf, SIGCONT);
            kill(leaf, SIGKILL);
        }
    }

    // ---- a stopped JOB, in a group of its own, whose shell exits ----------
    // The group the shell's exit orphans is the job's, not the shell's: the
    // shell was the job's only way back into the session.
    {
        int pfd[2];
        ck("pipe", pipe(pfd), 0);
        fflush(NULL);
        pid_t shell = fork();
        if (shell == 0) {
            close(pfd[0]);
            alarm(30);
            setpgid(0, 0);
            pid_t job = stopped_job(0);
            ssize_t w = write(pfd[1], &job, sizeof job);
            (void) w;
            _exit(0);
        }
        close(pfd[1]);
        pid_t job = 0;
        ssize_t got = read(pfd[0], &job, sizeof job);
        close(pfd[0]);
        int st;
        waitpid(shell, &st, 0);
        ck("the shell reported its job", got == (ssize_t) sizeof job ? 1 : 0, 1);
        if (got == (ssize_t) sizeof job) {
            char state = state_after_death(job);
            test_logf("  %-58s state='%c'\n", "job after its shell's exit", state);
            ck("a stopped job whose shell exits is hung up", is_dead(state), 1);
            kill(job, SIGCONT);
            kill(job, SIGKILL);
        }
    }

    // ---- the control: the same job, its shell still there -----------------
    // Nothing has orphaned the job's group, so nothing hangs the job up. A
    // fix that hung up every stopped job it could find would pass the case
    // above and fail here.
    {
        int pfd[2];
        ck("pipe", pipe(pfd), 0);
        fflush(NULL);
        pid_t shell = fork();
        if (shell == 0) {
            close(pfd[0]);
            alarm(30);
            setpgid(0, 0);
            pid_t job = stopped_job(0);
            ssize_t w = write(pfd[1], &job, sizeof job);
            (void) w;
            for (;;)
                pause();            // stays alive: the job's way back
        }
        close(pfd[1]);
        pid_t job = 0;
        ssize_t got = read(pfd[0], &job, sizeof job);
        close(pfd[0]);
        if (got == (ssize_t) sizeof job) {
            usleep(1200000);
            char state = proc_state(job);
            test_logf("  %-58s state='%c'\n", "job while its shell is still there", state);
            ck("a job whose shell is still there stays stopped", state, 'T');
            kill(job, SIGCONT);
            kill(job, SIGKILL);
        }
        if (shell > 0) {
            kill(shell, SIGKILL);
            int st;
            waitpid(shell, &st, 0);
        }
    }

    // ---- a group with two ways back is orphaned by the second exit --------
    // Two shells, one job group: the first shell's job made it and the second
    // shell's job joined it. The first shell's exit leaves the group a way
    // back through the second, so both jobs stay stopped; the second shell's
    // exit orphans the group, and both are hung up. By then init has adopted
    // the first job -- which is why init must not count as a way back.
    {
        int pfd[2];
        ck("pipe", pipe(pfd), 0);
        fflush(NULL);
        pid_t shell1 = fork();
        if (shell1 == 0) {
            close(pfd[0]);
            alarm(30);
            setpgid(0, 0);
            pid_t job = stopped_job(0);
            ssize_t w = write(pfd[1], &job, sizeof job);
            (void) w;
            for (;;)
                pause();
        }
        pid_t job1 = 0;
        ssize_t got1 = read(pfd[0], &job1, sizeof job1);
        pid_t shell2 = -1;
        if (got1 == (ssize_t) sizeof job1) {
            fflush(NULL);
            shell2 = fork();
            if (shell2 == 0) {
                close(pfd[0]);
                alarm(30);
                setpgid(0, 0);
                pid_t job = stopped_job(job1);  // into the first job's group
                ssize_t w = write(pfd[1], &job, sizeof job);
                (void) w;
                for (;;)
                    pause();
            }
        }
        close(pfd[1]);
        pid_t job2 = 0;
        ssize_t got2 = shell2 > 0 ? read(pfd[0], &job2, sizeof job2) : 0;
        close(pfd[0]);
        int both = got1 == (ssize_t) sizeof job1 && got2 == (ssize_t) sizeof job2;
        ck("both shells reported their jobs", both, 1);
        int st;
        if (both) {
            ck("  and the jobs share one group", getpgid(job2), job1);
            kill(shell1, SIGKILL);
            waitpid(shell1, &st, 0);
            usleep(1200000);
            char state1 = proc_state(job1), state2 = proc_state(job2);
            test_logf("  %-58s state='%c' '%c'\n", "jobs after the first shell's exit",
                      state1, state2);
            ck("the first shell's exit leaves its job stopped", state1, 'T');
            ck("  and the other shell's", state2, 'T');
            kill(shell2, SIGKILL);
            waitpid(shell2, &st, 0);
            state2 = state_after_death(job2);
            state1 = state_after_death(job1);
            test_logf("  %-58s state='%c' '%c'\n", "jobs after the second shell's exit",
                      state1, state2);
            ck("the second shell's exit hangs up its job", is_dead(state2), 1);
            ck("  and the job init adopted from the first", is_dead(state1), 1);
        }
        if (job1 > 0) {
            kill(job1, SIGCONT);
            kill(job1, SIGKILL);
        }
        if (job2 > 0) {
            kill(job2, SIGCONT);
            kill(job2, SIGKILL);
        }
        if (shell1 > 0) {
            kill(shell1, SIGKILL);
            waitpid(shell1, &st, 0);
        }
        if (shell2 > 0) {
            kill(shell2, SIGKILL);
            waitpid(shell2, &st, 0);
        }
    }

    // ---- a stopped child in ANOTHER session is not the shell's to orphan --
    // A child that has made a session of its own never had a way back through
    // the shell's, so the shell's exit changes nothing for its group, and it
    // stays stopped.
    {
        int pfd[2];
        ck("pipe", pipe(pfd), 0);
        fflush(NULL);
        pid_t shell = fork();
        if (shell == 0) {
            close(pfd[0]);
            alarm(30);
            setpgid(0, 0);
            pid_t loner = fork();
            if (loner == 0) {
                setsid();
                raise(SIGSTOP);
                for (;;)
                    pause();
            }
            int st;
            while (waitpid(loner, &st, WUNTRACED) < 0 && errno == EINTR)
                continue;
            ssize_t w = write(pfd[1], &loner, sizeof loner);
            (void) w;
            _exit(0);
        }
        close(pfd[1]);
        pid_t loner = 0;
        ssize_t got = read(pfd[0], &loner, sizeof loner);
        close(pfd[0]);
        int st;
        waitpid(shell, &st, 0);
        ck("the shell reported its other-session child", got == (ssize_t) sizeof loner ? 1 : 0, 1);
        if (got == (ssize_t) sizeof loner) {
            ck("  which leads a session of its own", getsid(loner), loner);
            usleep(1200000);
            char state = proc_state(loner);
            test_logf("  %-58s state='%c'\n", "other-session child after the shell's exit", state);
            ck("a stopped child in another session stays stopped", state, 'T');
            kill(loner, SIGCONT);
            kill(loner, SIGKILL);
        }
    }

    // ---- the THREAD that forked the job exits -----------------------------
    // The job's parent is the thread that forked it. That thread's exit hands
    // the job to another thread of the same process, which is no change of
    // parent that anyone can see -- Linux skips the rule for it outright --
    // so the job stays stopped. Then the process exits, and that orphans it.
    {
        int pfd[2];
        ck("pipe", pipe(pfd), 0);
        fflush(NULL);
        pid_t shell = fork();
        if (shell == 0) {
            close(pfd[0]);
            alarm(30);
            setpgid(0, 0);
            pthread_t t;
            if (pthread_create(&t, NULL, fork_job_then_exit, NULL) != 0)
                _exit(1);
            wait_thread_gone(&forker_word);
            pid_t job = __atomic_load_n(&forker_job, __ATOMIC_ACQUIRE);
            ssize_t w = write(pfd[1], &job, sizeof job);
            char state = proc_state(job);
            w = write(pfd[1], &state, 1);
            (void) w;
            _exit(0);               // now the process goes: the job is orphaned
        }
        close(pfd[1]);
        pid_t job = 0;
        char state = '?';
        ssize_t got = read(pfd[0], &job, sizeof job);
        ssize_t got_state = read(pfd[0], &state, 1);
        close(pfd[0]);
        int st = 0;
        waitpid(shell, &st, 0);
        int reported = got == (ssize_t) sizeof job && job > 0;
        ck("the shell reported its thread's job", reported, 1);
        test_logf("  %-58s state='%c'\n", "job after the thread that forked it exits",
                  got_state == 1 ? state : '?');
        ck("the forking thread's exit leaves the job stopped", got_state == 1 ? state : '?', 'T');
        ck("  and the shell exits as it chose to", WIFEXITED(st) ? WEXITSTATUS(st) : -1, 0);
        if (reported) {
            state = state_after_death(job);
            test_logf("  %-58s state='%c'\n", "job after the shell's own exit", state);
            ck("the shell's own exit then hangs the job up", is_dead(state), 1);
            kill(job, SIGCONT);
            kill(job, SIGKILL);
        }
    }

    // ---- init is no way back, for the exiting process's own group either --
    // A stopped member whose parent exited before the group's last link did
    // has been adopted by init. init is outside the group -- and in the CLI,
    // inside the session -- but it resumes nobody, so the last link's exit
    // orphans the group all the same.
    {
        int pfd[2];
        ck("pipe", pipe(pfd), 0);
        fflush(NULL);
        pid_t middle = fork();
        if (middle == 0) {
            close(pfd[0]);
            alarm(30);
            setpgid(0, 0);
            int ipfd[2];
            if (pipe(ipfd) != 0)
                _exit(1);
            pid_t between = fork();
            if (between == 0) {
                close(ipfd[0]);
                pid_t leaf = stopped_job(getpgrp());    // in this same group
                ssize_t w = write(ipfd[1], &leaf, sizeof leaf);
                (void) w;
                _exit(0);           // the leaf's parent goes; init adopts it
            }
            close(ipfd[1]);
            pid_t leaf = 0;
            ssize_t got = read(ipfd[0], &leaf, sizeof leaf);
            int st;
            waitpid(between, &st, 0);
            if (got != (ssize_t) sizeof leaf)
                _exit(1);
            char state = proc_state(leaf);
            ssize_t w = write(pfd[1], &leaf, sizeof leaf);
            w = write(pfd[1], &state, 1);
            (void) w;
            _exit(0);               // the group's last way back goes
        }
        close(pfd[1]);
        pid_t leaf = 0;
        char state = '?';
        ssize_t got = read(pfd[0], &leaf, sizeof leaf);
        ssize_t got_state = read(pfd[0], &state, 1);
        close(pfd[0]);
        int st;
        waitpid(middle, &st, 0);
        ck("the middle process reported the adopted leaf", got == (ssize_t) sizeof leaf ? 1 : 0, 1);
        test_logf("  %-58s state='%c'\n", "leaf after its own parent's exit",
                  got_state == 1 ? state : '?');
        ck("the leaf's parent's exit leaves it stopped", got_state == 1 ? state : '?', 'T');
        if (got == (ssize_t) sizeof leaf) {
            state = state_after_death(leaf);
            test_logf("  %-58s state='%c'\n", "adopted leaf after the group's last link exits", state);
            ck("the last link's exit hangs up the leaf init adopted", is_dead(state), 1);
            kill(leaf, SIGCONT);
            kill(leaf, SIGKILL);
        }
    }

    // ---- a SUBREAPER in the session is a way back -------------------------
    // Orphans go to the nearest subreaper rather than to init, and one in the
    // session and outside the group can resume it as the exited parent could:
    // the group is not orphaned. That holds for a job in a group of its own
    // and for a member of the exiting process's own group alike -- which is
    // why both questions are asked once the children have moved. Then the
    // subreaper exits, handing both to init, and that orphans both groups.
    {
        int pfd[2];
        ck("pipe", pipe(pfd), 0);
        fflush(NULL);
        pid_t keeper = fork();
        if (keeper == 0) {
            close(pfd[0]);
            alarm(30);
            setpgid(0, 0);
            prctl(PR_SET_CHILD_SUBREAPER, 1);
            pid_t adopted[2];
            char states[2];
            // [0]: a job in a group of its own; [1]: a member of the group of
            // the process that forked it.
            for (int own_group = 0; own_group < 2; own_group++) {
                int ipfd[2];
                if (pipe(ipfd) != 0)
                    _exit(1);
                pid_t middle = fork();
                if (middle == 0) {
                    close(ipfd[0]);
                    setpgid(0, 0);
                    pid_t job = stopped_job(own_group ? getpgrp() : 0);
                    ssize_t w = write(ipfd[1], &job, sizeof job);
                    (void) w;
                    _exit(0);       // the subreaper adopts what it leaves
                }
                close(ipfd[1]);
                pid_t job = 0;
                ssize_t got = read(ipfd[0], &job, sizeof job);
                close(ipfd[0]);
                int st;
                waitpid(middle, &st, 0);
                if (got != (ssize_t) sizeof job)
                    _exit(1);
                adopted[own_group] = job;
            }
            usleep(1200000);
            for (int i = 0; i < 2; i++)
                states[i] = proc_state(adopted[i]);
            ssize_t w = write(pfd[1], adopted, sizeof adopted);
            w = write(pfd[1], states, sizeof states);
            (void) w;
            _exit(0);               // and now init adopts them
        }
        close(pfd[1]);
        pid_t adopted[2] = { 0, 0 };
        char states[2] = { '?', '?' };
        ssize_t got = read(pfd[0], adopted, sizeof adopted);
        ssize_t got_states = read(pfd[0], states, sizeof states);
        close(pfd[0]);
        int st;
        waitpid(keeper, &st, 0);
        int reported = got == (ssize_t) sizeof adopted && got_states == (ssize_t) sizeof states;
        ck("the subreaper reported what it adopted", reported, 1);
        test_logf("  %-58s state='%c' '%c'\n", "adopted by the subreaper", states[0], states[1]);
        ck("a job whose shell exits under a subreaper stays stopped", states[0], 'T');
        ck("  as does a member of the exiting process's own group", states[1], 'T');
        if (reported) {
            char state0 = state_after_death(adopted[0]);
            char state1 = state_after_death(adopted[1]);
            test_logf("  %-58s state='%c' '%c'\n", "after the subreaper's own exit", state0, state1);
            ck("the subreaper's own exit hangs up the job", is_dead(state0), 1);
            ck("  and the other group's member", is_dead(state1), 1);
            for (int i = 0; i < 2; i++) {
                if (adopted[i] > 0) {
                    kill(adopted[i], SIGCONT);
                    kill(adopted[i], SIGKILL);
                }
            }
        }
    }

    return finish_suite("orphan_pgrp_wait");
}
