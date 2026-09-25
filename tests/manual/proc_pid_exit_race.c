// Readers of /proc/<pid>/* must not deadlock against the process exiting.
//
// Every /proc/<pid> handler pins the task with a reference
// (fs/proc/pid.c proc_get_task -> pid_get_task_zombie_ref) and most then take
// the task's general_lock to reach its mm, fd table or fs. do_exit() takes
// that same lock and, still holding it, waits for the references to go
// (kernel/exit.c exit_wait_needed). A reader that blocks on the lock while
// holding its reference therefore waits for the exit, and the exit waits for
// the reader: both stop for good, and so does whoever waits for the child.
// c0ccaed3 found that shape in /proc/meminfo and /proc/net; fs/sock.c's
// sock_task_files_retain closed two more copies of it.
//
// The same race also caught a lock-order inversion that froze the whole guest:
// /proc/<pid>/status took pids_lock (for Seccomp_filters) while holding the
// process's group->lock, and do_exit_group takes them the other way round.
//
// This races it head on. One process forks short-lived children in a loop,
// publishing each child's pid; several readers hammer that pid's status,
// stat, statm, maps, smaps_rollup, cmdline, environ, auxv, comm, io, limits,
// fd/, fdinfo/* and the exe/cwd/root links. The loop must keep reaping: a
// stall means some child never finished exiting. Four of four runs stalled
// before the fix, on arm64.
//
// The witness that the race was reached is the readers' own results: a read
// that finds the pid gone (ENOENT/ESRCH) ran while it was exiting or just after.
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "test_common.h"

#define NREADERS 8
#define RUN_SECS 8

struct shared {
    volatile int target;            // pid being hammered, 0 between children
    volatile int stop;
    volatile unsigned long reaped;  // children the churn loop has waited for
    volatile unsigned long reads[NREADERS];
    volatile unsigned long gone[NREADERS];  // reads that found the pid gone
};

static struct shared *sh;

static double now_secs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void churn(void) {
    while (!sh->stop) {
        pid_t pid = fork();
        if (pid < 0) {
            usleep(1000);
            continue;
        }
        if (pid == 0) {
            // Something for the readers to find, and a little to tear down.
            int fds[2];
            if (pipe(fds) == 0) {
                void *p = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                if (p != MAP_FAILED)
                    memset(p, 1, 4096);
            }
            for (volatile int i = 0; i < 20000; i++)
                ;
            _exit(0);
        }
        sh->target = pid;
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
            ;
        sh->target = 0;
        sh->reaped++;
    }
    _exit(0);
}

static const char *const files[] = {
    "status", "stat", "statm", "maps", "smaps_rollup", "cmdline",
    "environ", "auxv", "comm", "io", "limits",
};
static const char *const links[] = { "exe", "cwd", "root" };

static void note(int me, int ok) {
    sh->reads[me]++;
    if (!ok && (errno == ENOENT || errno == ESRCH))
        sh->gone[me]++;
}

static void read_file(int me, int pid, const char *name) {
    char path[64], buf[4096];
    snprintf(path, sizeof(path), "/proc/%d/%s", pid, name);
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        note(me, 0);
        return;
    }
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        ;
    note(me, n == 0);
    close(fd);
}

static void read_dir(int me, int pid, const char *name, int open_each) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/%s", pid, name);
    DIR *d = opendir(path);
    if (d == NULL) {
        note(me, 0);
        return;
    }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.')
            continue;
        if (open_each) {
            char sub[320];
            snprintf(sub, sizeof(sub), "%s/%s", name, de->d_name);
            read_file(me, pid, sub);
        } else {
            char sub[320], target[256];
            snprintf(sub, sizeof(sub), "%s/%s", path, de->d_name);
            note(me, readlink(sub, target, sizeof(target)) >= 0);
        }
    }
    closedir(d);
    sh->reads[me]++;
}

// One operation per turn, each on whichever child is current at that moment,
// and each reader starting at a different one: a reader that finished a whole
// round on one pid before looking again spent nearly all its reads on a child
// that was already gone.
static void reader(int me) {
    unsigned nfiles = sizeof(files) / sizeof(files[0]);
    unsigned nlinks = sizeof(links) / sizeof(links[0]);
    unsigned nops = nfiles + nlinks + 3;
    for (unsigned op = (unsigned) me; !sh->stop; op = (op + 1) % nops) {
        int pid = sh->target;
        if (pid == 0) {
            op--;
            continue;
        }
        if (op < nfiles) {
            read_file(me, pid, files[op]);
        } else if (op < nfiles + nlinks) {
            char path[64], target[256];
            snprintf(path, sizeof(path), "/proc/%d/%s", pid, links[op - nfiles]);
            note(me, readlink(path, target, sizeof(target)) >= 0);
        } else if (op == nfiles + nlinks) {
            read_dir(me, pid, "fd", 0);
        } else if (op == nfiles + nlinks + 1) {
            read_dir(me, pid, "fdinfo", 1);
        } else {
            char path[64];
            struct stat st;
            snprintf(path, sizeof(path), "/proc/%d", pid);
            note(me, stat(path, &st) == 0);
        }
    }
    _exit(0);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    setvbuf(stdout, NULL, _IONBF, 0);

    sh = mmap(NULL, sizeof(*sh), PROT_READ | PROT_WRITE,
              MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (sh == MAP_FAILED) {
        printf("FAIL: mmap shared: %s\n", strerror(errno));
        return 1;
    }
    memset((void *) sh, 0, sizeof(*sh));

    pid_t kids[NREADERS + 1];
    for (int i = 0; i <= NREADERS; i++) {
        kids[i] = fork();
        if (kids[i] < 0) {
            printf("FAIL: fork: %s\n", strerror(errno));
            return 1;
        }
        if (kids[i] == 0) {
            if (i == NREADERS)
                churn();
            reader(i);
        }
    }

    double start = now_secs();
    while (now_secs() - start < RUN_SECS)
        usleep(100000);
    sh->stop = 1;

    // Everyone stops on their own. One that does not is stuck in the kernel --
    // a child that never finished exiting holds up the churn loop's wait, and
    // a reader stuck with it is blocked on a host mutex no signal reaches --
    // so name it and leave rather than wait on anything.
    unsigned stall_limit = test_watchdog_secs(10);
    int left = NREADERS + 1;
    double deadline = now_secs() + stall_limit;
    while (left > 0 && now_secs() < deadline) {
        for (int i = 0; i <= NREADERS; i++) {
            if (kids[i] > 0 && waitpid(kids[i], NULL, WNOHANG) == kids[i]) {
                kids[i] = 0;
                left--;
            }
        }
        if (left > 0)
            usleep(50000);
    }
    if (left > 0) {
        for (int i = 0; i <= NREADERS; i++) {
            if (kids[i] > 0)
                printf("FAIL: %s pid %d did not stop within %us (target was %d)\n",
                       i == NREADERS ? "exit churn" : "reader", kids[i],
                       stall_limit, sh->target);
        }
        printf("proc_pid_exit_race: FAIL failures=%d\n", left);
        _exit(1);
    }

    unsigned long reads = 0, gone = 0;
    for (int i = 0; i < NREADERS; i++) {
        reads += sh->reads[i];
        gone += sh->gone[i];
    }
    test_logf("children reaped %lu, reads %lu, reads that found it gone %lu\n",
              sh->reaped, reads, gone);
    if (sh->reaped < 20) {
        printf("FAIL: only %lu children in %ds -- the race was barely run\n",
               sh->reaped, RUN_SECS);
        failures_total++;
    }
    if (gone == 0) {
        printf("FAIL: no read ever overlapped an exit (reads=%lu) -- the race "
               "was not reached\n", reads);
        failures_total++;
    }
    return finish_suite("proc_pid_exit_race");
}
