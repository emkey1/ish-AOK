// The CPU counts are shaped like Linux's: the CPUs online, and the ones this
// task may run on.
//
// Every view of the ONLINE CPUs agrees: /sys/devices/system/cpu/online and the
// cpuN directories beside it, the processors in /proc/cpuinfo and the cpuN
// lines of /proc/stat. possible and present may name more (hotplug slots) but
// never fewer. The affinity mask and /proc/self/status's Cpus_allowed_list
// agree with each other and name only online CPUs, and may name fewer: a
// cpuset or `taskset` does that on Linux, and AOK does it on iOS, keeping a
// third of the cores back so that what sizes a thread pool from the mask (Go's
// GOMAXPROCS, `make -j$(nproc)`, Rust's available_parallelism) leaves the
// app's UI room to run. sysconf follows its libc: glibc answers
// _SC_NPROCESSORS_ONLN from /sys (online) and _CONF from possible, musl
// answers both from the mask.
//
// This test first asserted that every view gives ONE number, and 096531e8 (in
// 556's cycle) made them do so by cutting all of them to the reduced count, so
// a 9-core M4 iPad listed 6 processors in /proc/cpuinfo. Linux itself fails
// that rule under `taskset`. The shape is checked here; the hardware count,
// which a guest cannot know, was checked on the device (cpuinfo 9, nproc 6).
//
// ISH_GUEST_CPU_RESERVE=1 applies the iOS reservation on the Mac, so the CLI
// has a mask smaller than what is online, as a device does.
//
// Oracle: Linux 6.12 (camd), glibc x86_64 and -m32, as it is and under
// `taskset -c 0-2`.
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "test_common.h"

#define TEST_NAME "cpu_count_agree"

// A kernel CPU list ("0-3,5,7-8"): how many CPUs it names, or -1.
static long list_count(const char *s) {
    long count = 0;
    while (*s != '\0' && *s != '\n') {
        char *end;
        long a = strtol(s, &end, 10);
        if (end == s || a < 0)
            return -1;
        long b = a;
        s = end;
        if (*s == '-') {
            b = strtol(s + 1, &end, 10);
            if (end == s + 1 || b < a)
                return -1;
            s = end;
        }
        count += b - a + 1;
        if (*s == ',')
            s++;
        else if (*s != '\0' && *s != '\n')
            return -1;
    }
    return count;
}

static long file_list_count(const char *path) {
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return -1;
    char line[256] = "";
    long n = fgets(line, sizeof(line), f) != NULL ? list_count(line) : -1;
    fclose(f);
    return n;
}

// Lines of `path` that start with `prefix` followed by what `rest` accepts.
static long count_lines(const char *path, const char *prefix, int want_digit) {
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return -1;
    char line[4096];
    long n = 0;
    size_t plen = strlen(prefix);
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strncmp(line, prefix, plen) != 0)
            continue;
        if (want_digit ? isdigit((unsigned char) line[plen])
                       : (line[plen] == ' ' || line[plen] == '\t' || line[plen] == ':'))
            n++;
    }
    fclose(f);
    return n;
}

static long status_allowed(void) {
    FILE *f = fopen("/proc/self/status", "r");
    if (f == NULL)
        return -1;
    char line[512];
    long n = -1;
    while (fgets(line, sizeof(line), f) != NULL)
        if (strncmp(line, "Cpus_allowed_list:", 18) == 0)
            n = list_count(line + 18 + strspn(line + 18, " \t"));
    fclose(f);
    return n;
}

static long cpu_dirs(void) {
    DIR *d = opendir("/sys/devices/system/cpu");
    if (d == NULL)
        return -1;
    long n = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL)
        if (strncmp(de->d_name, "cpu", 3) == 0 && isdigit((unsigned char) de->d_name[3]) &&
                de->d_name[3 + strspn(de->d_name + 3, "0123456789")] == '\0')
            n++;
    closedir(d);
    return n;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(30));

    cpu_set_t set;
    CPU_ZERO(&set);
    long affinity = sched_getaffinity(0, sizeof(set), &set) == 0 ? CPU_COUNT(&set) : -1;
    if (affinity <= 0) {
        printf("FAIL: sched_getaffinity (%s)\n", strerror(errno));
        return 1;
    }

    long allowed = status_allowed();
    long online = file_list_count("/sys/devices/system/cpu/online");
    test_logf("sched_getaffinity: %ld CPUs, Cpus_allowed_list %ld, online %ld\n",
              affinity, allowed, online);
    if (allowed != affinity) {
        printf("FAIL: Cpus_allowed_list names %ld CPUs, sched_getaffinity %ld\n", allowed, affinity);
        failures_total++;
    }
    if (online < affinity) {
        printf("FAIL: %ld CPUs online, fewer than the %ld in the affinity mask\n", online, affinity);
        failures_total++;
    }
    // Every CPU in the mask is online (online is "0-N" here, as on a machine
    // without holes, so that is: below N).
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
        if (CPU_ISSET(cpu, &set) && cpu >= online) {
            printf("FAIL: the affinity mask names CPU %d, which is not online (%ld online)\n",
                   cpu, online);
            failures_total++;
        }
    }

    struct { const char *name; long n; } seen[] = {
        {"/sys/devices/system/cpu/cpuN directories", cpu_dirs()},
        {"/proc/cpuinfo processors", count_lines("/proc/cpuinfo", "processor", 0)},
        {"/proc/stat cpuN lines", count_lines("/proc/stat", "cpu", 1)},
    };
    for (size_t i = 0; i < sizeof(seen) / sizeof(seen[0]); i++) {
        test_logf("%s: %ld\n", seen[i].name, seen[i].n);
        if (seen[i].n != online) {
            printf("FAIL: %s says %ld CPUs, /sys/devices/system/cpu/online %ld\n",
                   seen[i].name, seen[i].n, online);
            failures_total++;
        }
    }
    const char *wider[] = {"/sys/devices/system/cpu/possible", "/sys/devices/system/cpu/present"};
    for (size_t i = 0; i < 2; i++) {
        long n = file_list_count(wider[i]);
        test_logf("%s: %ld\n", wider[i], n);
        if (n < online) {
            printf("FAIL: %s names %ld CPUs, fewer than the %ld online\n", wider[i], n, online);
            failures_total++;
        }
    }

    long onln = sysconf(_SC_NPROCESSORS_ONLN), conf = sysconf(_SC_NPROCESSORS_CONF);
    test_logf("sysconf ONLN %ld, CONF %ld\n", onln, conf);
#ifdef __GLIBC__
    if (onln != online || conf < online) {
        printf("FAIL: glibc sysconf ONLN %ld CONF %ld, want ONLN = %ld online and CONF >= it\n",
               onln, conf, online);
        failures_total++;
    }
#else
    if (onln != affinity || conf != affinity) {
        printf("FAIL: musl sysconf ONLN %ld CONF %ld, want both the mask's %ld\n",
               onln, conf, affinity);
        failures_total++;
    }
#endif
    return finish_suite(TEST_NAME);
}
