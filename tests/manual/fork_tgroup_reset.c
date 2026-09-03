// A forked child's process-wide resource ledger starts at zero.
//
// Linux builds the child's signal_struct with kmem_cache_zalloc and then copies
// only the handful of fields copy_signal() names, so every accumulated total in
// it -- utime/stime, min_flt, ioac, maxrss -- begins at 0 for a new process.
// The per-TASK half of this is already covered: kernel/task.c's task_create_
// zeroes task->minflt, task->nvcsw and task->io, added after `time -v` was seen
// reporting a chain of nested shells' inherited fault counts.
//
// The per-PROCESS half was still inherited. AOK's fork copies the whole
// struct tgroup, and two of its fields are exactly that kind of accumulator:
//
//   group->rusage   the rolled-up usage of threads that have ALREADY exited,
//                   which rusage_get_group_of() tops up with the live ones to
//                   answer getrusage(RUSAGE_SELF) and CLOCK_PROCESS_CPUTIME_ID
//   group->io_dead  the same thing for I/O counters, summed into
//                   /proc/<pid>/io
//
// So a process that had created and joined a thread handed its child that
// thread's CPU time and I/O as the child's own opening balance -- and do_wait
// then reported the same inflated numbers back to the parent, both as the
// child's rusage and into the parent's RUSAGE_CHILDREN. Single-threaded
// parents are unaffected, which is why this hid: group->rusage only becomes
// non-zero once a thread of the group exits.
//
// The shape is therefore: burn CPU and do I/O in a worker THREAD, join it so
// its totals land in the group, and then fork and look at what the child
// starts with. Assertions are ratios against the parent's own figures rather
// than absolute numbers, because the inherited value IS the parent's value --
// so the two cases are separated by a factor, not by a threshold that has to
// be retuned for an emulated guest or a loaded host.
//
// group->stopped and group->group_exit_code were inherited by the same copy
// and are fixed alongside these, but they are only reachable by a clone racing
// another thread's SIGSTOP delivery, so there is nothing deterministic to
// assert here.
//
// Measured against x86_64 glibc on Linux 6.12.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_common.h"

// What the worker thread spends, and so what the group's totals must hold once
// it has exited. 64 KiB of writes is far above any incidental traffic, and
// 60ms of thread CPU is enough to stand out from a just-forked child's own.
#define WORKER_BYTES   (64 * 1024)
#define WORKER_CPU_US  60000
#define WORKER_WALL_S  20

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-58s got=%-10ld want=%ld\n", label, got, want);
}

// The process-wide ledger, as a program sees it.
struct snap {
    long long cpu_us;      // getrusage(RUSAGE_SELF) user+system
    long long wchar;       // /proc/self/io
    long long write_bytes;
    int have_io;
};

static long long rusage_us(int who) {
    struct rusage r;
    if (getrusage(who, &r) != 0)
        return -1;
    return (long long) r.ru_utime.tv_sec * 1000000 + r.ru_utime.tv_usec +
           (long long) r.ru_stime.tv_sec * 1000000 + r.ru_stime.tv_usec;
}

// Reads RUSAGE_SELF before touching /proc, so that in the just-forked child the
// CPU figure covers as little of the child's own work as possible.
static void snap_take(struct snap *s) {
    memset(s, 0, sizeof *s);
    s->cpu_us = rusage_us(RUSAGE_SELF);
    s->wchar = -1;
    s->write_bytes = -1;

    FILE *f = fopen("/proc/self/io", "r");
    if (f == NULL)
        return;
    char key[64];
    unsigned long long val;
    while (fscanf(f, "%63[^:]: %llu\n", key, &val) == 2) {
        if (strcmp(key, "wchar") == 0)
            s->wchar = (long long) val;
        else if (strcmp(key, "write_bytes") == 0)
            s->write_bytes = (long long) val;
    }
    fclose(f);
    s->have_io = (s->wchar >= 0);
}

static long long worker_cpu_us;   // what the worker actually managed to burn

static void *worker(void *arg) {
    (void) arg;

    // File-backed writes, so both wchar and write_bytes move.
    char buf[4096];
    memset(buf, 'x', sizeof buf);
    char path[64];
    snprintf(path, sizeof path, "/tmp/fork_tgroup_reset.%d", (int) getpid());
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
        for (size_t done = 0; done < WORKER_BYTES; done += sizeof buf) {
            if (write(fd, buf, sizeof buf) != (ssize_t) sizeof buf)
                break;
        }
        fsync(fd);
        close(fd);
        unlink(path);
    }

    // Then CPU, measured on this thread so it costs the same wherever it runs.
    // The wall-clock ceiling is a backstop: if the clock does not move we stop
    // anyway and main reports a skip rather than spinning out the watchdog.
    struct timeval start;
    gettimeofday(&start, NULL);
    volatile unsigned long sink = 0;
    for (;;) {
        for (int i = 0; i < 200000; i++)
            sink += (unsigned long) i * 2654435761u;
        long long us = rusage_us(RUSAGE_THREAD);
        if (us < 0 || us >= WORKER_CPU_US) {
            worker_cpu_us = us;
            break;
        }
        struct timeval now;
        gettimeofday(&now, NULL);
        if (now.tv_sec - start.tv_sec > WORKER_WALL_S) {
            worker_cpu_us = us;
            break;
        }
    }
    return NULL;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(180));

    pthread_t th;
    if (pthread_create(&th, NULL, worker, NULL) != 0) {
        printf("FAIL fork_tgroup_reset: pthread_create failed (%s)\n", strerror(errno));
        return finish_suite("fork_tgroup_reset");
    }
    pthread_join(th, NULL);

    struct snap parent;
    snap_take(&parent);
    test_logf("  parent: cpu=%lldus wchar=%lld write_bytes=%lld (worker cpu=%lldus)\n",
              parent.cpu_us, parent.wchar, parent.write_bytes, worker_cpu_us);

    // Preconditions. Each is a property of this suite's other tests
    // (getrusage_group, proc_pid_io) rather than of this one, so a failure here
    // is a skip: without the parent's totals actually holding the exited
    // worker's numbers there is nothing for the child to have inherited, and
    // the checks below would pass while testing nothing.
    if (parent.cpu_us < WORKER_CPU_US / 2) {
        test_logf("  the group total does not hold the exited worker's CPU "
                  "(%lldus), skipped\n", parent.cpu_us);
        return finish_suite("fork_tgroup_reset");
    }
    if (!parent.have_io || parent.wchar < WORKER_BYTES / 2) {
        test_logf("  no /proc/self/io, or it does not hold the exited worker's "
                  "writes (%lld), skipped\n", parent.wchar);
        return finish_suite("fork_tgroup_reset");
    }

    int pfd[2];
    if (pipe(pfd) != 0) {
        printf("FAIL fork_tgroup_reset: pipe failed (%s)\n", strerror(errno));
        return finish_suite("fork_tgroup_reset");
    }

    pid_t kid = fork();
    if (kid < 0) {
        printf("FAIL fork_tgroup_reset: fork failed (%s)\n", strerror(errno));
        return finish_suite("fork_tgroup_reset");
    }
    if (kid == 0) {
        struct snap child;
        snap_take(&child);
        // Measured before this write, which would otherwise be the child's own
        // first wchar.
        ssize_t n = write(pfd[1], &child, sizeof child);
        _exit(n == (ssize_t) sizeof child ? 0 : 1);
    }
    close(pfd[1]);

    struct snap child;
    memset(&child, 0, sizeof child);
    size_t got = 0;
    while (got < sizeof child) {
        ssize_t n = read(pfd[0], (char *) &child + got, sizeof child - got);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        got += (size_t) n;
    }
    close(pfd[0]);
    int kstatus = 0;
    waitpid(kid, &kstatus, 0);

    if (got != sizeof child) {
        printf("FAIL fork_tgroup_reset: the child did not report (exit=%d)\n",
               WIFEXITED(kstatus) ? WEXITSTATUS(kstatus) : -1);
        return finish_suite("fork_tgroup_reset");
    }
    test_logf("  child:  cpu=%lldus wchar=%lld write_bytes=%lld\n",
              child.cpu_us, child.wchar, child.write_bytes);

    // The inherited value is the parent's value, so a factor of 4 separates the
    // two outcomes by miles in either direction -- no absolute budget for the
    // child's own startup cost has to be guessed at.
    ck("a forked child does not inherit the group's CPU total",
       child.cpu_us >= 0 && child.cpu_us * 4 < parent.cpu_us, 1);
    ck("nor the exited threads' I/O in /proc/<pid>/io",
       child.wchar >= 0 && child.wchar * 4 < parent.wchar, 1);
    if (parent.write_bytes >= WORKER_BYTES / 2)
        ck("nor their file-backed write_bytes",
           child.write_bytes >= 0 && child.write_bytes * 4 < parent.write_bytes, 1);
    else
        test_logf("  %-58s parent=%lld\n",
                  "(write_bytes too small to compare)", parent.write_bytes);

    // And the parent's own totals were not disturbed by handing the child a
    // fresh set: it still holds what its worker spent.
    struct snap after;
    snap_take(&after);
    ck("and the parent keeps its own", after.cpu_us >= parent.cpu_us, 1);
    ck("...including its I/O", after.wchar >= parent.wchar, 1);

    return finish_suite("fork_tgroup_reset");
}
