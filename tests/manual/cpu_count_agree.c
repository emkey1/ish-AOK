// Every way of asking how many CPUs there are gives the same answer.
//
// Regression for a triage report: nproc said 6 on a device where /proc/cpuinfo
// listed 9 processors. On iOS AOK kept a third of the cores back from
// sched_getaffinity, so that the programs sizing a thread pool from it (Go's
// GOMAXPROCS, `make -j$(nproc)`, OpenMP, Rust's available_parallelism) leave
// the app's UI room to run -- but /proc/cpuinfo, /proc/stat and
// /sys/devices/system/cpu reported every core, so a program counting CPUs any
// other way (glibc's sysconf reads /sys/devices/system/cpu/online, Node's
// os.cpus() reads /proc/cpuinfo) sized itself to all of them, and top showed
// CPUs that sched_getaffinity said this process could not run on.
//
// On an unrestricted Linux process they all agree (camd, 8 CPUs): the
// affinity mask, /proc/self/status Cpus_allowed_list, sysconf
// _SC_NPROCESSORS_ONLN and _CONF (musl answers both from the affinity mask,
// glibc from /sys), /sys/devices/system/cpu/online and the cpuN directories
// beside it, the processors in /proc/cpuinfo and the cpuN lines of /proc/stat.
// possible and present may name more CPUs than are online on real hardware
// (hotplug slots) but never fewer.
//
// The iOS reservation does not happen on the Mac, where every source already
// agreed; ISH_GUEST_CPU_RESERVE=1 applies it there so this can see it fail.
//
// Oracle: Linux 6.12 (camd), glibc x86_64 and -m32.
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

    struct { const char *name; long n; } seen[] = {
        {"/proc/self/status Cpus_allowed_list", status_allowed()},
        {"sysconf(_SC_NPROCESSORS_ONLN)", sysconf(_SC_NPROCESSORS_ONLN)},
        {"sysconf(_SC_NPROCESSORS_CONF)", sysconf(_SC_NPROCESSORS_CONF)},
        {"/sys/devices/system/cpu/online", file_list_count("/sys/devices/system/cpu/online")},
        {"/sys/devices/system/cpu/cpuN directories", cpu_dirs()},
        {"/proc/cpuinfo processors", count_lines("/proc/cpuinfo", "processor", 0)},
        {"/proc/stat cpuN lines", count_lines("/proc/stat", "cpu", 1)},
    };
    test_logf("sched_getaffinity: %ld CPUs\n", affinity);
    for (size_t i = 0; i < sizeof(seen) / sizeof(seen[0]); i++) {
        test_logf("%s: %ld\n", seen[i].name, seen[i].n);
        if (seen[i].n != affinity) {
            printf("FAIL: %s says %ld CPUs, sched_getaffinity %ld\n", seen[i].name, seen[i].n,
                   affinity);
            failures_total++;
        }
    }
    const char *wider[] = {"/sys/devices/system/cpu/possible", "/sys/devices/system/cpu/present"};
    for (size_t i = 0; i < 2; i++) {
        long n = file_list_count(wider[i]);
        test_logf("%s: %ld\n", wider[i], n);
        if (n < affinity) {
            printf("FAIL: %s names %ld CPUs, fewer than the %ld online\n", wider[i], n, affinity);
            failures_total++;
        }
    }
    return finish_suite(TEST_NAME);
}
