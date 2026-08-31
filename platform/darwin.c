#include <mach/mach.h>
#include <mach/mach_time.h>
#include <TargetConditionals.h>
#include <stdlib.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/mman.h>
#include "kernel/errno.h"
#include "platform/platform.h"
#include "debug.h"

typedef double CFTimeInterval;

struct cpu_usage get_total_cpu_usage(void) {
    // HOST_CPU_LOAD_INFO reports the ENTIRE PHYSICAL DEVICE's system-wide CPU
    // load (every app on the device, not just us) -- guest tools reading
    // /proc/stat (top, mpstat, load-based scripts) expect iSH's OWN emulated
    // workload usage, and on a device running other apps this made the guest
    // see near-100% idle even while iSH itself was pegging a core. Use this
    // process's own cumulative user/system time instead; Mach has no
    // per-process "idle" concept, so derive it from wall-clock uptime across
    // the configured cpu count instead.
    struct cpu_usage usage = {0};
    struct task_absolutetime_info info;
    mach_msg_type_number_t count = TASK_ABSOLUTETIME_INFO_COUNT;
    kern_return_t kr = task_info(mach_task_self(), TASK_ABSOLUTETIME_INFO,
                                  (task_info_t) &info, &count);
    if (kr == KERN_SUCCESS) {
        mach_timebase_info_data_t timebase;
        mach_timebase_info(&timebase);
        double ns_per_mach_tick = (double) timebase.numer / (double) timebase.denom;
        usage.user_ticks = (uint64_t) (info.total_user * ns_per_mach_tick / 10000000.0);
        usage.system_ticks = (uint64_t) (info.total_system * ns_per_mach_tick / 10000000.0);
    }

    struct uptime_info uptime = get_uptime();
    uint64_t elapsed_ticks = (uint64_t) get_cpu_count() * uptime.uptime_ticks;
    uint64_t busy_ticks = usage.user_ticks + usage.system_ticks;
    usage.idle_ticks = elapsed_ticks > busy_ticks ? elapsed_ticks - busy_ticks : 0;
    return usage;
}

struct mem_usage get_mem_usage(void) {
    // These host_* calls can fail on iOS (sandbox / OS-version changes; observed
    // returning non-KERN_SUCCESS on iOS 26/27). They must NEVER abort the
    // emulator: /proc/meminfo is read by routine guest tools (busybox top/free,
    // init scripts), so an assert here takes down the whole app on a normal
    // guest read. Degrade gracefully and return best-effort values instead.
    struct mem_usage usage = {};

    host_basic_info_data_t basic = {};
    mach_msg_type_number_t count = HOST_BASIC_INFO_COUNT;
    if (host_info(mach_host_self(), HOST_BASIC_INFO, (host_info_t) &basic, &count) == KERN_SUCCESS) {
        usage.total = basic.max_mem;
        usage.available = basic.memory_size;
    } else {
        // Fall back to sysctl for the physical memory size.
        uint64_t memsize = 0;
        size_t len = sizeof(memsize);
        if (sysctlbyname("hw.memsize", &memsize, &len, NULL, 0) == 0) {
            usage.total = memsize;
            usage.available = memsize;
        }
    }

    vm_statistics64_data_t vm = {};
    count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info_t) &vm, &count) == KERN_SUCCESS) {
        usage.free = (uint64_t) vm.free_count * vm_page_size;
        usage.cached = (uint64_t) vm.speculative_count * vm_page_size;
        usage.active = (uint64_t) vm.active_count * vm_page_size;
        usage.inactive = (uint64_t) vm.inactive_count * vm_page_size;
        usage.wirecount = (uint64_t) vm.wire_count * vm_page_size;
        usage.swapins = (uint64_t) vm.swapins * vm_page_size;
        usage.swapouts = (uint64_t) vm.swapouts * vm_page_size;
    }
    // If the VM stats are unavailable, leave them zero rather than crashing.
    return usage;
}

CFTimeInterval getSystemUptime(void) {
    enum { NANOSECONDS_IN_SEC = 1000 * 1000 * 1000 };
    static double multiply = 0;
    if (multiply == 0)
    {
        mach_timebase_info_data_t s_timebase_info;
        kern_return_t result = mach_timebase_info(&s_timebase_info);
        assert(result == 0);
        // multiply to get value in the nano seconds
        multiply = (double)s_timebase_info.numer / (double)s_timebase_info.denom;
        // multiply to get value in the seconds
        multiply /= NANOSECONDS_IN_SEC;
    }
    return mach_continuous_time() * multiply;
}

struct uptime_info get_uptime(void) {
    struct timeval now;
    if (gettimeofday(&now, NULL) != 0) {
        printk("ERROR: in gettimeofday() call\n");
    }
    // The guest's boot, set where pid 1 is created (kernel/init.c). NOT the
    // host's kern.boottime, which this used to read into a local and never
    // use: had it been used it would have reported when the DEVICE last
    // booted, which is further from the truth than the value it ignored.
    extern time_t boot_time;

    struct {
        uint32_t ldavg[3];
        long scale;
    } vm_loadavg;
    size_t size = sizeof(vm_loadavg);
    if (sysctlbyname("vm.loadavg", &vm_loadavg, &size, NULL, 0) != 0) {
        printk("ERROR: in sysctlbyname(vm.loadavg) call\n");
    }

    // Adjust the scale of load averages
    for (int i = 0; i < 3; i++) {
        if (FSHIFT < 16)
            vm_loadavg.ldavg[i] <<= 16 - FSHIFT;
        else
            vm_loadavg.ldavg[i] >>= FSHIFT - 16;
    }

    struct uptime_info uptime = {
        .uptime_ticks = (now.tv_sec - boot_time) * 100, // Ensure this calculation is as intended
        .load_1m = vm_loadavg.ldavg[0],
        .load_5m = vm_loadavg.ldavg[1],
        .load_15m = vm_loadavg.ldavg[2],
    };
    return uptime;
}

int get_cpu_count(void) {
     int ncpu = 1;
     size_t size = sizeof(int);
     sysctlbyname("hw.ncpu", &ncpu, &size, NULL, 0);
     const char *override = getenv("ISH_GUEST_CPU_COUNT");
     if (override != NULL && override[0] != '\0') {
         long forced = strtol(override, NULL, 10);
         if (forced > 0)
             ncpu = (int) forced;
     }
 #if TARGET_OS_OSX && defined(__aarch64__)
     // Standalone CLI / macOS dev harness: default to 4 emulated CPUs so local
     // and fakefs repro runs reproduce the concurrency -- and the TLB/COW/futex/
     // heap races -- of a multi-core device, instead of the old 2-core cap that
     // hid that whole class of bug. 4 exposes real parallelism without
     // oversubscribing a big host (this branch is macOS-only; the iOS app is not
     // TARGET_OS_OSX and keeps the true hw.ncpu). Override with
     // ISH_GUEST_CPU_COUNT=N (e.g. =6 to match a device, =1 to force serial).
     else
         ncpu = 4;
 #endif
     if (ncpu < 1)
         ncpu = 1;
     return ncpu;
}

// The number of CPUs to advertise to guest scheduler-sizing queries
// (sched_getaffinity / nproc), as opposed to the true core count reported by
// /proc/cpuinfo and /proc/stat. Multi-threaded guest workloads spawn one OS
// thread per "available" CPU -- e.g. the Go runtime sets GOMAXPROCS from
// sched_getaffinity, and `make -j$(nproc)` from nproc -- and under emulation
// running hw.ncpu such threads saturates every core, both starving the app UI
// and drowning the guest in lock/futex/TLB-shootdown overhead (Go actually
// compiles *faster* with fewer threads). On iOS we reserve roughly a third of
// the cores (at least one) so those programs leave headroom; /proc/cpuinfo
// still reports the true count, so htop and friends show all CPUs.
int get_cpu_count_for_affinity(void) {
    int ncpu = get_cpu_count();
#if TARGET_OS_IPHONE
    if (getenv("ISH_GUEST_CPU_COUNT") == NULL && ncpu > 2) {
        int reserve = ncpu / 3;
        if (reserve < 1)
            reserve = 1;
        ncpu -= reserve;
    }
#endif
    if (ncpu < 1)
        ncpu = 1;
    return ncpu;
}

#if TARGET_OS_IPHONE
#include <os/proc.h>
#endif

bool host_mem_headroom_low(void) {
#if TARGET_OS_IPHONE
    // Cached once; env parse is idempotent so an init race is harmless.
    static long threshold_mb = -1;
    if (threshold_mb < 0) {
        const char *env = getenv("ISH_GUEST_MEM_HEADROOM_MB");
        long t = env != NULL ? atol(env) : 192;
        threshold_mb = t >= 0 ? t : 0;
    }
    if (threshold_mb == 0)
        return false; // guard disabled
    size_t avail = os_proc_available_memory();
    // 0 means "no limit information" (e.g. simulator/debugging contexts),
    // not "no memory left" — never starve the guest on a bad reading.
    if (avail == 0)
        return false;
    return avail < (size_t) threshold_mb * 1024 * 1024;
#else
    // macOS: no per-process jetsam budget; the CLI can use what the system has.
    return false;
#endif
}
