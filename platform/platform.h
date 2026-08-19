#ifndef PLATFORM_H
#define PLATFORM_H
#include "misc.h"

// for some reason a tick is always 10ms
struct cpu_usage {
    uint64_t user_ticks;
    uint64_t system_ticks;
    uint64_t idle_ticks;
    uint64_t nice_ticks;
};
struct cpu_usage get_total_cpu_usage(void);

struct mem_usage {
    uint64_t total;
    uint64_t free;
    uint64_t available;
    uint64_t cached;
    uint64_t active;
    uint64_t inactive;
    uint64_t swapins;
    uint64_t swapouts;
    uint64_t wirecount;
};
struct mem_usage get_mem_usage(void);

struct uptime_info {
    // Since the GUEST booted (pid 1 created; see kernel/init.c), in 100 Hz
    // ticks -- the unit /proc/uptime and the per-CPU idle accounting divide
    // by. sysinfo(2) wants seconds and must divide; it did not, and reported
    // a 12-second-old guest as "up 20 min".
    uint64_t uptime_ticks;
    uint64_t load_1m, load_5m, load_15m;
};
struct uptime_info get_uptime(void);

int get_cpu_count(void);
// CPU count to advertise to guest scheduler-sizing queries (sched_getaffinity /
// nproc), which may be smaller than get_cpu_count() to reserve host cores for
// the UI. /proc/cpuinfo and /proc/stat still use the true get_cpu_count().
int get_cpu_count_for_affinity(void);

// True when the host process is close enough to its memory ceiling that guest
// memory growth should be refused (guest mmap/brk/mremap return ENOMEM) so the
// app's own allocations — UIKit, fakefs SQLite, malloc inside libobjc — keep
// working instead of crashing on NULL. On iOS this consults the jetsam budget
// via os_proc_available_memory(); everywhere else (macOS/Linux CLI, no hard
// per-process budget) it returns false. Threshold defaults to 192 MiB;
// override with ISH_GUEST_MEM_HEADROOM_MB (0 disables the guard).
bool host_mem_headroom_low(void);

#endif
