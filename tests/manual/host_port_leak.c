/*
 * host_port_leak.c -- a guest thread that exits leaves no Mach port behind.
 *
 * Every guest task is a host thread of one host process, and iOS kills an app
 * that holds too many Mach port names: EXC_RESOURCE, "Exceeded system-wide
 * per-process Port Limit (114882 ports)" on an M4 iPad. kernel/resource.c's
 * cpu_usage_self() asked thread_info() about mach_thread_self() and never gave
 * the right back, and do_exit asks it for every thread that exits, so each
 * guest thread ever made left one dead port name in the app. The regression
 * suite's churn reached the limit about four roots in and the app vanished
 * mid-run.
 *
 * /proc/ish/host_ports counts the process's port names and how many are dead.
 * This makes THREADS threads and PROCS processes, all of which exit, and
 * checks the counts barely move. Before the fix the dead names rose by one per
 * thread and process: a delta of THREADS + PROCS.
 *
 * SKIPs where there is no such file or no Mach ports (a Linux host, or a
 * kernel from before the file).
 *
 * Exits 0 and prints "host_port_leak: PASS" on success.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_common.h"

#define THREADS 300
#define PROCS 100
#define BATCH 20
/* What other activity in the app may add while this runs. The leak adds one
 * dead name per thread and per process, so THREADS + PROCS. */
#define DEAD_SLACK 30
#define NAMES_SLACK 100

static int read_ports(long *names, long *dead) {
    FILE *f = fopen("/proc/ish/host_ports", "r");
    if (f == NULL)
        return -1;
    char key[32];
    long value;
    *names = *dead = -1;
    while (fscanf(f, "%31s %ld", key, &value) == 2) {
        if (strcmp(key, "names") == 0)
            *names = value;
        else if (strcmp(key, "dead_names") == 0)
            *dead = value;
    }
    fclose(f);
    return *names >= 0 && *dead >= 0 ? 0 : -1;
}

static void *thread_main(void *arg) {
    /* Some CPU time to account, and a getrusage of its own, so the exit has
     * something to roll up. */
    volatile unsigned long x = 0;
    for (int i = 0; i < 10000; i++)
        x += (unsigned long) i;
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return arg;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    setvbuf(stdout, NULL, _IONBF, 0);

    long names0, dead0;
    if (read_ports(&names0, &dead0) != 0) {
        printf("host_port_leak: SKIP (no /proc/ish/host_ports counts on this host)\n");
        return 0;
    }

    int made = 0;
    for (int done = 0; done < THREADS; done += BATCH) {
        pthread_t t[BATCH];
        int n = 0;
        for (int i = 0; i < BATCH; i++)
            if (pthread_create(&t[n], NULL, thread_main, NULL) == 0)
                n++;
        for (int i = 0; i < n; i++)
            pthread_join(t[i], NULL);
        made += n;
    }
    int forked = 0;
    for (int i = 0; i < PROCS; i++) {
        pid_t pid = fork();
        if (pid == 0)
            _exit(0);
        if (pid > 0 && waitpid(pid, NULL, 0) == pid)
            forked++;
    }
    /* The witness that the workload happened: a run that made nothing would
     * pass on a leaking kernel. */
    if (made < THREADS / 2 || forked < PROCS / 2) {
        printf("FAIL only %d of %d threads and %d of %d processes were made, so the "
               "counts say nothing\n", made, THREADS, forked, PROCS);
        failures_total++;
    }

    long names1, dead1;
    if (read_ports(&names1, &dead1) != 0) {
        printf("FAIL /proc/ish/host_ports stopped being readable\n");
        return 1;
    }
    test_logf("%d threads, %d processes: names %ld -> %ld, dead names %ld -> %ld\n",
              made, forked, names0, names1, dead0, dead1);
    if (dead1 - dead0 > DEAD_SLACK) {
        printf("FAIL %d threads and %d processes that exited left %ld dead port names "
               "(%ld -> %ld), want at most %d\n",
               made, forked, dead1 - dead0, dead0, dead1, DEAD_SLACK);
        failures_total++;
    }
    if (names1 - names0 > NAMES_SLACK) {
        printf("FAIL the process's port names rose by %ld (%ld -> %ld) across %d threads "
               "and %d processes, want at most %d\n",
               names1 - names0, names0, names1, made, forked, NAMES_SLACK);
        failures_total++;
    }
    return finish_suite("host_port_leak");
}
