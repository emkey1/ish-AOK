// VmSize counts the whole address space, VmRSS what is actually in memory,
// and VmPeak and VmHWM are high-water marks of each.
//
// Regression for a triage report: ps showed VSZ equal to RSS, and the peaks
// repeated the current figures. AOK's VmSize counted page-table entries only,
// so a lazily reserved range (every anonymous mapping of 64 MiB or more) was
// missing from it; VmRSS counted every mapped page whether or not it had ever
// been touched, PROT_NONE guard regions included, so it came out equal to
// VmSize; and VmPeak and VmHWM were printed as that same figure, so they fell
// with it the moment memory was freed. A leak hunter reading VmHWM, a
// supervisor reading ru_maxrss, and `ps -o vsz,rss` all saw one number three
// times.
//
// What Linux does (camd, 6.12), and what this checks, in pages and to within a
// small slack for the C library's own allocations:
//   - VmSize is the sum of /proc/self/maps, PROT_NONE and never-touched
//     ranges included ([vsyscall] is outside the mm and is left out), and
//     statm's size and stat's vsize are the same figure;
//   - mapping PROT_NONE, mapping without touching, and a large mapping all
//     grow VmSize by their size and VmRSS by nothing;
//   - writing N pages grows VmRSS by N; statm's resident and stat's rss agree;
//   - unmapping shrinks both, while VmPeak and VmHWM keep the highest values
//     seen, and never read below VmSize and VmRSS.
//
// Oracle: Linux 6.12 (camd), glibc x86_64 and -m32.
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include "test_common.h"

#define TEST_NAME "mem_vsz_rss_peaks"
#define MIB ((size_t) 1024 * 1024)
// Linux is not exact either, and these are what it needs, measured on camd:
// the C library's own allocations move the figures between reads (and between
// the four files one sample reads), statm and stat take RSS from approximate
// per-CPU counters that differ from VmRSS by ~120 kB, and transparent huge
// pages round a 16 MiB write in the middle of a mapping out to 2 MiB edges --
// 36 MiB resident for 32 MiB written, on -m32. Every bug this looks for is off
// by tens of MiB.
#define SLACK_KB 6144L
#define READ_SLACK_KB 1024L

struct usage {
    long size, rss, peak, hwm;        // kB, from /proc/self/status
    long statm_size, statm_rss;       // pages
    unsigned long long stat_vsize;    // bytes
    long stat_rss;                    // pages
    long maps_kb;                     // sum of /proc/self/maps
};

static long status_kb(const char *text, const char *key) {
    const char *p = strstr(text, key);
    return p == NULL ? -1 : strtol(p + strlen(key), NULL, 10);
}

static void read_file(const char *path, char *buf, size_t n) {
    FILE *f = fopen(path, "r");
    size_t got = f != NULL ? fread(buf, 1, n - 1, f) : 0;
    buf[got] = '\0';
    if (f != NULL)
        fclose(f);
}

static struct usage sample(void) {
    struct usage u = {0};
    static char buf[65536];
    read_file("/proc/self/status", buf, sizeof(buf));
    u.size = status_kb(buf, "VmSize:");
    u.rss = status_kb(buf, "VmRSS:");
    u.peak = status_kb(buf, "VmPeak:");
    u.hwm = status_kb(buf, "VmHWM:");
    read_file("/proc/self/statm", buf, sizeof(buf));
    sscanf(buf, "%ld %ld", &u.statm_size, &u.statm_rss);
    read_file("/proc/self/stat", buf, sizeof(buf));
    // Fields 23 and 24, counted from after the ")" that ends the name.
    char *p = strrchr(buf, ')');
    if (p != NULL) {
        int field = 2;
        for (char *tok = strtok(p + 1, " "); tok != NULL; tok = strtok(NULL, " ")) {
            field++;
            if (field == 23)
                u.stat_vsize = strtoull(tok, NULL, 10);
            else if (field == 24)
                u.stat_rss = strtol(tok, NULL, 10);
        }
    }
    FILE *f = fopen("/proc/self/maps", "r");
    char line[512];
    while (f != NULL && fgets(line, sizeof(line), f) != NULL) {
        unsigned long long a, b;
        if (sscanf(line, "%llx-%llx", &a, &b) == 2 && strstr(line, "[vsyscall]") == NULL)
            u.maps_kb += (long) ((b - a) / 1024);
    }
    if (f != NULL)
        fclose(f);
    return u;
}

static int differ(long a, long b) {
    return a - b > READ_SLACK_KB || b - a > READ_SLACK_KB;
}

static void check_consistent(const char *when, struct usage u) {
    long page_kb = sysconf(_SC_PAGESIZE) / 1024;
    if (differ(u.size, u.maps_kb)) {
        printf("FAIL: %s: VmSize %ld kB is not the sum of /proc/self/maps, %ld kB\n", when,
               u.size, u.maps_kb);
        failures_total++;
    }
    if (differ(u.statm_size * page_kb, u.size) || differ((long) (u.stat_vsize / 1024), u.size)) {
        printf("FAIL: %s: statm size %ld pages and stat vsize %llu bytes are not VmSize %ld kB\n",
               when, u.statm_size, u.stat_vsize, u.size);
        failures_total++;
    }
    if (differ(u.statm_rss * page_kb, u.rss) || differ(u.stat_rss * page_kb, u.rss)) {
        printf("FAIL: %s: statm resident %ld and stat rss %ld pages are not VmRSS %ld kB\n", when,
               u.statm_rss, u.stat_rss, u.rss);
        failures_total++;
    }
    if (u.peak < u.size || u.hwm < u.rss) {
        printf("FAIL: %s: a peak is below its current value (VmPeak %ld < VmSize %ld, or "
               "VmHWM %ld < VmRSS %ld)\n", when, u.peak, u.size, u.hwm, u.rss);
        failures_total++;
    }
    test_logf("%-26s VmSize %8ld VmPeak %8ld VmRSS %7ld VmHWM %7ld kB\n", when, u.size, u.peak,
              u.rss, u.hwm);
}

// `got` is within SLACK_KB of `want`.
static void near(const char *what, long got, long want) {
    if (got < want - SLACK_KB || got > want + SLACK_KB) {
        printf("FAIL: %s: %ld kB, expected %ld kB\n", what, got, want);
        failures_total++;
    } else {
        test_logf("ok: %s: %ld kB (expected %ld)\n", what, got, want);
    }
}

static void *map(size_t len, int prot) {
    void *p = mmap(NULL, len, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        printf("FAIL: mmap %zu MiB (%s)\n", len / MIB, strerror(errno));
        exit(1);
    }
    return p;
}

static void touch(char *p, size_t len) {
    long page = sysconf(_SC_PAGESIZE);
    for (size_t off = 0; off < len; off += (size_t) page)
        p[off] = 1;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(120));

    struct usage base = sample();
    check_consistent("at start", base);

    // Address space that is not memory: a PROT_NONE reservation, an untouched
    // mapping, and one big enough to be reserved lazily.
    size_t none_len = 48 * MIB, small_len = 24 * MIB, big_len = 96 * MIB;
    char *none = map(none_len, PROT_NONE);
    char *small = map(small_len, PROT_READ | PROT_WRITE);
    char *big = map(big_len, PROT_READ | PROT_WRITE);
    struct usage mapped = sample();
    check_consistent("mapped, untouched", mapped);
    near("VmSize growth from mapping 168 MiB", mapped.size - base.size,
         (long) ((none_len + small_len + big_len) / 1024));
    near("VmRSS growth from mapping without touching", mapped.rss - base.rss, 0);

    // Memory: write every page of 16 MiB of the small mapping and 16 MiB of
    // the big one.
    touch(small, 16 * MIB);
    touch(big + 40 * MIB, 16 * MIB);
    struct usage touched = sample();
    check_consistent("32 MiB written", touched);
    near("VmRSS growth from writing 32 MiB", touched.rss - mapped.rss, 32 * 1024);
    near("VmSize growth from writing", touched.size - mapped.size, 0);

    munmap(none, none_len);
    munmap(small, small_len);
    munmap(big, big_len);
    struct usage freed = sample();
    check_consistent("all unmapped", freed);
    near("VmSize after unmapping it all", freed.size, base.size);
    near("VmRSS after unmapping it all", freed.rss, base.rss);
    if (freed.peak < touched.size) {
        printf("FAIL: VmPeak %ld kB fell below the %ld kB the address space reached\n",
               freed.peak, touched.size);
        failures_total++;
    }
    if (freed.hwm < touched.rss - READ_SLACK_KB) {
        printf("FAIL: VmHWM %ld kB fell below the %ld kB that was resident\n", freed.hwm,
               touched.rss);
        failures_total++;
    }

    // A peak reached and gone between two reads is still the peak: map more
    // than ever before, write more than ever before, and unmap it all again
    // without reading anything in between.
    size_t burst_vm = 256 * MIB, burst_rss = 48 * MIB;
    char *reserve = map(burst_vm, PROT_NONE);
    char *burst = map(burst_rss, PROT_READ | PROT_WRITE);
    touch(burst, burst_rss);
    munmap(burst, burst_rss);
    munmap(reserve, burst_vm);
    struct usage after = sample();
    check_consistent("after an unread burst", after);
    long want_hwm = freed.rss + (long) (burst_rss / 1024);
    if (after.hwm < want_hwm - SLACK_KB) {
        printf("FAIL: VmHWM %ld kB missed a %zu MiB burst it never saw (expected >= %ld kB)\n",
               after.hwm, burst_rss / MIB, want_hwm);
        failures_total++;
    }
    long want_peak = freed.size + (long) ((burst_vm + burst_rss) / 1024);
    if (after.peak < want_peak - SLACK_KB) {
        printf("FAIL: VmPeak %ld kB missed a %zu MiB burst it never saw (expected >= %ld kB)\n",
               after.peak, (burst_vm + burst_rss) / MIB, want_peak);
        failures_total++;
    }

    // A fork's child holds the parent's pages until it writes to them, and a
    // write replaces one page, not its neighbours: writing every fourth page
    // of 16 MiB the parent filled leaves the child's resident set where it
    // was. (AOK copies a whole host page's worth of guest pages at a time, and
    // the neighbours it copied along with the written page must not drop out
    // of the count.)
    size_t cow_len = 16 * MIB;
    char *cow = map(cow_len, PROT_READ | PROT_WRITE);
    touch(cow, cow_len);
    fflush(stdout);
    pid_t child = fork();
    if (child == 0) {
        struct usage before = sample();
        long page = sysconf(_SC_PAGESIZE);
        for (size_t off = 0; off < cow_len; off += 4 * (size_t) page)
            cow[off] = 2;
        struct usage after_cow = sample();
        int bad = 0;
        if (before.rss < (long) (cow_len / 1024) - SLACK_KB) {
            printf("FAIL: the child of a fork holds %ld kB, not the %zu MiB its parent wrote\n",
                   before.rss, cow_len / MIB);
            bad = 1;
        }
        if (after_cow.rss < before.rss - SLACK_KB) {
            printf("FAIL: writing every fourth page of %zu MiB after fork dropped the child's "
                   "VmRSS from %ld to %ld kB\n", cow_len / MIB, before.rss, after_cow.rss);
            bad = 1;
        }
        test_logf("child after fork: VmRSS %ld kB, after writing every 4th page %ld kB\n",
                  before.rss, after_cow.rss);
        fflush(stdout);
        _exit(bad);
    }
    int status = 0;
    waitpid(child, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        failures_total++;
    munmap(cow, cow_len);
    return finish_suite(TEST_NAME);
}
