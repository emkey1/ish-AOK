/*
 * proc_stat_monotonic -- /proc/stat's CPU counters must never go backward,
 * and must add up to the time that passed.
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
 * IDLE is checked too, on every cpuN line and on the aggregate "cpu" line.
 * It used to be exempt, because AOK derived it as "uptime minus busy" and
 * uptime was whole seconds of wall clock: busy time grows continuously, so
 * between two reads in the same second idle SHRANK. A waybar survey measured
 * idle going backward in 65 of 74 samples taken 50 ms apart (native Linux: 0
 * of 80), and waybar's cpu module takes its first reading from two samples
 * 100 ms apart, so that reading was garbage. The same subtraction also let
 * idle fall whenever a slot's busy time outran the clock -- several busy tasks
 * in one slot, or the process using more cores than it reports -- which is
 * why the second phase below runs more spinners than there are CPUs.
 *
 * And the fields have to ADD UP: over a phase, each line's total should grow
 * by about the elapsed time (times ncpu for the aggregate line). Holding idle
 * flat while busy runs ahead would keep every field monotonic and still
 * report a CPU that did two seconds of work per second.
 *
 * And the cpuN lines' busy time has to add up to the "cpu" line's, as on
 * Linux, where the "cpu" line is their sum. AOK's slots are only the CPUs in
 * the affinity mask, and a slot's time past its capacity was discarded: on a
 * 6-core iPhone with 4 in the mask, `stress-ng --cpu 6` read 86% on the "cpu"
 * line, 100/100/85/100 on cpu0-cpu3 and 0% on cpu4 and cpu5, as though the
 * performance cores were never used. The third phase runs one spinner more
 * than the mask names, which puts two in one slot and asks for a CPU outside
 * the mask; AOK now moves a full slot's overflow onto CPUs with room.
 *
 * Also passes on real Linux (counters there are monotonic by construction).
 */
#define _GNU_SOURCE

#include <ctype.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

#define MAX_CPUS 64
#define AGG MAX_CPUS            // slot for the aggregate "cpu" line
#define NFIELDS 8               // user nice system idle iowait irq softirq steal
#define CHECKED_FIELDS 4        // iowait may legitimately fall on Linux; see proc(5)
#define CHURNERS 6
#define CHURN_SECONDS 10
#define SPIN_SECONDS 6
#define SPIN_MS 20
#define MAX_REPORTS 10

static const char *field_names[NFIELDS] = {
    "user", "nice", "system", "idle", "iowait", "irq", "softirq", "steal",
};

struct cpu_line {
    int present;
    unsigned long long f[NFIELDS];
};

static int read_cpu_lines(struct cpu_line *cpus) {
    FILE *f = fopen("/proc/stat", "r");
    if (f == NULL)
        return -1;
    for (int i = 0; i <= AGG; i++)
        cpus[i].present = 0;
    int found = 0;
    char line[512];
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strncmp(line, "cpu", 3) != 0)
            continue;
        int idx;
        char *p = line + 3;
        if (*p == ' ') {
            idx = AGG;
        } else if (isdigit((unsigned char) *p)) {
            idx = (int) strtol(p, &p, 10);
            if (idx < 0 || idx >= MAX_CPUS)
                continue;
        } else {
            continue;
        }
        unsigned long long v[NFIELDS] = {0};
        int n = sscanf(p, " %llu %llu %llu %llu %llu %llu %llu %llu",
                       &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
        if (n < CHECKED_FIELDS)
            continue;
        cpus[idx].present = 1;
        memcpy(cpus[idx].f, v, sizeof(v));
        if (idx != AGG)
            found++;
    }
    fclose(f);
    return found;
}

static unsigned long long line_total(const struct cpu_line *l) {
    unsigned long long sum = 0;
    for (int j = 0; j < NFIELDS; j++)
        sum += l->f[j];
    return sum;
}

// Everything but idle and iowait.
static unsigned long long line_busy(const struct cpu_line *l) {
    return line_total(l) - l->f[3] - l->f[4];
}

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec + (double) ts.tv_nsec / 1e9;
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

static void spinner_loop(void) {
    volatile unsigned long sink = 0;
    for (;;)
        sink++;
}

static void stop_children(pid_t *kids, int n) {
    for (int i = 0; i < n; i++)
        if (kids[i] > 0)
            kill(kids[i], SIGKILL);
    for (int i = 0; i < n; i++)
        if (kids[i] > 0)
            waitpid(kids[i], NULL, 0);
}

static const char *line_name(int idx, char *buf, size_t size) {
    if (idx == AGG)
        snprintf(buf, size, "cpu");
    else
        snprintf(buf, size, "cpu%d", idx);
    return buf;
}

// Read /proc/stat as fast as it will go for `seconds`, failing on any backward
// step in the checked fields, then check each line's total against the time
// that passed, and with check_sum the cpuN lines' busy time against the "cpu"
// line's.
static void watch_counters(const char *phase, int seconds, int ncpu, int check_sum) {
    static struct cpu_line first[AGG + 1], prev[AGG + 1], cur[AGG + 1];
    if (read_cpu_lines(first) <= 0) {
        failf("read /proc/stat", 0, 0, 0, 1, 0, 0);
        return;
    }
    double start = now_seconds();
    memcpy(prev, first, sizeof(prev));

    unsigned long reads = 0, regressions = 0;
    char name[16];
    while (now_seconds() - start < seconds) {
        if (read_cpu_lines(cur) <= 0) {
            failf("read /proc/stat", 0, 0, 0, 1, 0, 0);
            break;
        }
        reads++;
        for (int i = 0; i <= AGG; i++) {
            if (!prev[i].present || !cur[i].present)
                continue;
            for (int j = 0; j < CHECKED_FIELDS; j++) {
                if (cur[i].f[j] >= prev[i].f[j])
                    continue;
                regressions++;
                test_log_if(regressions <= MAX_REPORTS,
                            "  %s: %s %s went backward: %llu -> %llu (read %lu)\n",
                            phase, line_name(i, name, sizeof(name)), field_names[j],
                            prev[i].f[j], cur[i].f[j], reads);
            }
        }
        memcpy(prev, cur, sizeof(prev));
    }
    double elapsed = now_seconds() - start;

    test_logf("%s: %lu reads over %.1fs, %lu backward steps\n",
              phase, reads, elapsed, regressions);
    if (regressions != 0) {
        char label[96];
        snprintf(label, sizeof(label), "%s: CPU counters went backward", phase);
        failf(label, regressions, 0, 0, 0, 0, 0);
    }

    // The totals. USER_HZ is 100 on every Linux ABI; sysconf says so here.
    long hz = sysconf(_SC_CLK_TCK);
    if (hz <= 0)
        hz = 100;
    double expect_per_cpu = elapsed * (double) hz;
    for (int i = 0; i <= AGG; i++) {
        if (!first[i].present || !prev[i].present)
            continue;
        unsigned long long a = line_total(&first[i]), b = line_total(&prev[i]);
        double grew = b >= a ? (double) (b - a) : -(double) (a - b);
        double expect = i == AGG ? expect_per_cpu * ncpu : expect_per_cpu;
        double ratio = grew / expect;
        test_logf("  %s: %s total grew %.0f ticks, expected ~%.0f (x%.2f)\n",
                  phase, line_name(i, name, sizeof(name)), grew, expect, ratio);
        // Loose on purpose: this is not a precision check, it catches a line
        // whose idle stopped absorbing the clock (ratio far below 1) or whose
        // busy time outran it (far above).
        if (ratio < 0.5 || ratio > 1.5) {
            char label[96];
            snprintf(label, sizeof(label), "%s: %s total does not track elapsed time (x100)",
                     phase, name);
            failf(label, (uint64_t) (ratio * 100), 0, 0, 100, 0, 0);
        }
    }

    if (!check_sum || !first[AGG].present || !prev[AGG].present)
        return;
    double sum = 0;
    for (int i = 0; i < AGG; i++)
        if (first[i].present && prev[i].present)
            sum += (double) line_busy(&prev[i]) - (double) line_busy(&first[i]);
    double agg = (double) line_busy(&prev[AGG]) - (double) line_busy(&first[AGG]);
    test_logf("  %s: cpuN lines busy %.0f ticks, cpu line busy %.0f\n", phase, sum, agg);
    // The spinners keep at least one CPU busy, so the ratio means something.
    // AOK's "cpu" line is its whole process, host threads that are no guest
    // task included, so this is not exact there; on Linux it is.
    if (agg < expect_per_cpu / 2) {
        test_logf("  %s: under one CPU busy; the sum is not compared\n", phase);
        return;
    }
    double ratio = sum / agg;
    if (ratio < 0.9 || ratio > 1.1) {
        char label[96];
        snprintf(label, sizeof(label), "%s: cpuN lines' busy time is not the cpu line's (x100)",
                 phase);
        failf(label, (uint64_t) (ratio * 100), 0, 0, 100, 0, 0);
    }
}

// How many CPUs this process may run on: what nproc says.
static int allowed_cpus(void) {
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) != 0)
        return -1;
    return CPU_COUNT(&set);
}

static int run_spinners(const char *phase, int spinners, int ncpu, int check_sum) {
    if (spinners > MAX_CPUS)
        spinners = MAX_CPUS;
    pid_t spin[MAX_CPUS];
    for (int i = 0; i < spinners; i++) {
        spin[i] = fork();
        if (spin[i] == 0)
            spinner_loop(); // never returns
        if (spin[i] < 0) {
            perror("fork spinner");
            stop_children(spin, i);
            return -1;
        }
    }
    watch_counters(phase, SPIN_SECONDS, ncpu, check_sum);
    stop_children(spin, spinners);
    return 0;
}

int main(int argc, char **argv) {
    test_init(argc, argv);

    static struct cpu_line probe[AGG + 1];
    int ncpu = read_cpu_lines(probe);
    if (ncpu <= 0) {
        // No cpuN lines at all (single-cpu kernels can omit them): nothing to
        // test, treat as pass rather than error out of the suite.
        printf("proc_stat_monotonic: PASS (no cpuN lines)\n");
        return 0;
    }

    // Phase 1: fork churn, which is what the banking race needs.
    pid_t churners[CHURNERS];
    for (int i = 0; i < CHURNERS; i++) {
        churners[i] = fork();
        if (churners[i] == 0)
            churner_loop(); // never returns
        if (churners[i] < 0) {
            perror("fork churner");
            stop_children(churners, i);
            return 2;
        }
    }
    watch_counters("churn", CHURN_SECONDS, ncpu, 0);
    stop_children(churners, CHURNERS);

    // Phase 2: more busy loops than CPUs. At least one slot must then hold two
    // of them, and the whole set can use more cores than the kernel reports --
    // busy time outrunning the clock, which is where a derived idle falls.
    if (run_spinners("saturate", ncpu + 2, ncpu, 1) != 0)
        return 2;

    // Phase 3: one busy loop more than the affinity mask names -- two share a
    // slot, and the time that slot cannot hold has to show on a CPU outside
    // the mask, when there is one, or on a slot with room.
    int allowed = allowed_cpus();
    if (allowed <= 0) {
        perror("sched_getaffinity");
        return 2;
    }
    test_logf("%d CPUs online, %d in the affinity mask\n", ncpu, allowed);
    if (run_spinners("spread", allowed + 1, ncpu, 1) != 0)
        return 2;

    return finish_suite("proc_stat_monotonic");
}
