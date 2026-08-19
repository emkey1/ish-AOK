#ifdef __linux__

#include <sys/sysinfo.h>
#include <time.h>          // time(), time_t -- for the guest's boot_time
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kernel/errno.h"
#include "platform/platform.h"
#include "debug.h"

static void read_proc_line(const char *file, const char *name, char *buf) {
    FILE *f = fopen(file, "r");
    if (f == NULL) ERRNO_DIE(file);
    do {
        fgets(buf, 1234, f);
        if (feof(f))
            die("could not find proc line %s", name);
    } while (!(strncmp(name, buf, strlen(name)) == 0 && buf[strlen(name)] == ' '));
    fclose(f);
}

struct cpu_usage get_total_cpu_usage(void) {
    struct cpu_usage usage = {};
    char buf[1234];
    read_proc_line("/proc/stat", "cpu", buf);
    sscanf(buf, "cpu %"SCNu64" %"SCNu64" %"SCNu64" %"SCNu64"\n", &usage.user_ticks, &usage.system_ticks, &usage.idle_ticks, &usage.nice_ticks);
    return usage;
}

struct mem_usage get_mem_usage(void) {
    struct mem_usage usage;
    char buf[1234];

    read_proc_line("/proc/meminfo", "MemTotal:", buf);
    sscanf(buf, "MemTotal: %"PRIu64" kB\n", &usage.total);
    read_proc_line("/proc/meminfo", "MemFree:", buf);
    sscanf(buf, "MemFree: %"PRIu64" kB\n", &usage.free);
    read_proc_line("/proc/meminfo", "Active:", buf);
    sscanf(buf, "Active: %"PRIu64" kB\n", &usage.active);
    read_proc_line("/proc/meminfo", "Inactive:", buf);
    sscanf(buf, "Inactive: %"PRIu64" kB\n", &usage.inactive);

    return usage;
}

struct uptime_info get_uptime(void) {
    struct sysinfo info;
    sysinfo(&info);
    // info.uptime is the HOST's, which is a different machine from the guest
    // and usually a much older one. Take the guest's boot the same way the
    // Darwin build does -- set where pid 1 is created, kernel/init.c.
    extern time_t boot_time;
    struct uptime_info uptime = {
        .uptime_ticks = (uint64_t) (time(NULL) - boot_time) * 100,
        .load_1m = info.loads[0],
        .load_5m = info.loads[1],
        .load_15m = info.loads[2],
    };
    return uptime;
}

int get_cpu_count(void) {
    return get_nprocs();
}

int get_cpu_count_for_affinity(void) {
    // No UI to protect on the Linux/CLI build; report the real count.
    return get_cpu_count();
}

bool host_mem_headroom_low(void) {
    // No hard per-process memory budget on a Linux host; the OOM killer and
    // overcommit policy own this. The guard is an iOS-jetsam concern.
    return false;
}

#endif
