/*
 * proc_stat_monotonic -- /proc/stat per-CPU counters must never go backward.
 *
 * AOK synthesizes /proc/stat's cpuN lines by bucketing each guest task into a
 * virtual-CPU slot (pid % ncpu): live tasks are sampled on every read, and
 * do_exit "banks" an exiting task's final thread time into per-slot dead
 * totals (kernel/task.c). The banking was not serialized against the reader:
 * a task exiting mid-read could be counted twice in one snapshot -- sampled
 * live during the task-list walk AND included via the dead-slot totals it
 * banked just before the reader loaded them. That read reported an inflated
 * user/system value, and the next read dropped back down: a backward-moving
 * counter, which a real kernel never produces.
 *
 * top-style tools assume monotonicity. ktop computed `cur - prev` on
 * unsigned long long, so one backward tick became a ~2^64 delta, the meter
 * fraction became astronomical, the cell count saturated to INT_MAX, an
 * overflow-broken clamp (`used + cells[i]`) failed to catch it, and the bar
 * fill loop read its text buffer thousands of bytes past the end -- straight
 * off the top of the stack at 0xfffff000. Crashed regularly under multi-arch
 * chroot churn (many short-lived exits). Fixed by cpu_slots_lock in
 * kernel/task.c (plus defensive clamps in ktop itself).
 *
 * This test churns short-lived CPU-burning children (so every exit banks a
 * nonzero tick count) while hammering /proc/stat reads, and fails on any
 * decrease in a cpuN line's user or system field. idle is deliberately NOT
 * checked: AOK's synthetic per-slot idle (uptime - slot busy) can legitimately
 * dip when a slot holds more than one busy task.
 *
 * Also passes on real Linux (counters there are monotonic by construction).
 */
#define _GNU_SOURCE

#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

#define MAX_CPUS 64
#define CHURNERS 6
#define TEST_SECONDS 10
#define SPIN_MS 20

struct cpu_line {
    int present;
    unsigned long long user, nice, system, idle;
};

static int read_cpu_lines(struct cpu_line *cpus) {
    FILE *f = fopen("/proc/stat", "r");
    if (f == NULL)
        return -1;
    for (int i = 0; i < MAX_CPUS; i++)
        cpus[i].present = 0;
    int found = 0;
    char line[512];
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strncmp(line, "cpu", 3) != 0 || !isdigit((unsigned char) line[3]))
            continue;
        int idx;
        unsigned long long user, nice, system, idle;
        if (sscanf(line, "cpu%d %llu %llu %llu %llu",
                   &idx, &user, &nice, &system, &idle) == 5
                && idx >= 0 && idx < MAX_CPUS) {
            cpus[idx].present = 1;
            cpus[idx].user = user;
            cpus[idx].nice = nice;
            cpus[idx].system = system;
            cpus[idx].idle = idle;
            found++;
        }
    }
    fclose(f);
    return found;
}

// Burn roughly ms of CPU time (not wall time) so the exiting task has a
// nonzero tick count to bank.
static void spin_cpu_ms(long ms) {
    struct timespec start, now;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &start);
    volatile unsigned long sink = 0;
    for (;;) {
        for (int i = 0; i < 20000; i++)
            sink += (unsigned long) i * 2654435761u;
        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &now);
        long elapsed_ms = (now.tv_sec - start.tv_sec) * 1000
                        + (now.tv_nsec - start.tv_nsec) / 1000000;
        if (elapsed_ms >= ms)
            break;
    }
}

// Churner: loop forever forking a grandchild that burns ~SPIN_MS of CPU and
// exits. The parent test kills us when it's done measuring.
static void churner_loop(void) {
    for (;;) {
        pid_t pid = fork();
        if (pid == 0) {
            spin_cpu_ms(SPIN_MS);
            _exit(0);
        }
        if (pid > 0)
            waitpid(pid, NULL, 0);
        else
            _exit(1); // fork pressure: just bail, parent keeps going
    }
}

int main(int argc, char **argv) {
    test_init(argc, argv);

    static struct cpu_line prev[MAX_CPUS], cur[MAX_CPUS];
    if (read_cpu_lines(prev) <= 0) {
        // No cpuN lines at all (single-cpu kernels can omit them): nothing to
        // test, treat as pass rather than error out of the suite.
        printf("proc_stat_monotonic: PASS (no cpuN lines)\n");
        return 0;
    }

    pid_t churners[CHURNERS];
    for (int i = 0; i < CHURNERS; i++) {
        churners[i] = fork();
        if (churners[i] == 0)
            churner_loop(); // never returns
        if (churners[i] < 0) {
            perror("fork churner");
            return 2;
        }
    }

    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += TEST_SECONDS;

    unsigned long reads = 0, regressions = 0;
    for (;;) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > deadline.tv_sec ||
                (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec))
            break;

        if (read_cpu_lines(cur) <= 0) {
            failf("read /proc/stat", 0, 0, 0, 1, 0, 0);
            break;
        }
        reads++;
        for (int i = 0; i < MAX_CPUS; i++) {
            if (!prev[i].present || !cur[i].present)
                continue;
            if (cur[i].user < prev[i].user) {
                regressions++;
                test_log_if(1, "cpu%d user went backward: %llu -> %llu (read %lu)\n",
                            i, prev[i].user, cur[i].user, reads);
            }
            if (cur[i].system < prev[i].system) {
                regressions++;
                test_log_if(1, "cpu%d system went backward: %llu -> %llu (read %lu)\n",
                            i, prev[i].system, cur[i].system, reads);
            }
            if (cur[i].nice < prev[i].nice) {
                regressions++;
                test_log_if(1, "cpu%d nice went backward: %llu -> %llu (read %lu)\n",
                            i, prev[i].nice, cur[i].nice, reads);
            }
        }
        memcpy(prev, cur, sizeof(prev));
    }

    for (int i = 0; i < CHURNERS; i++)
        if (churners[i] > 0)
            kill(churners[i], SIGKILL);
    for (int i = 0; i < CHURNERS; i++)
        if (churners[i] > 0)
            waitpid(churners[i], NULL, 0);

    test_logf("%lu reads over %ds, %lu backward steps\n",
              reads, TEST_SECONDS, regressions);
    if (regressions != 0)
        failf("cpuN counters went backward", regressions, 0, 0, 0, 0, 0);
    return finish_suite("proc_stat_monotonic");
}
