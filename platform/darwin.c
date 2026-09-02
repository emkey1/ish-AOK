#include <mach/mach.h>
#include <mach/mach_time.h>
#include <TargetConditionals.h>
#include <stdatomic.h>
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
        // max_mem, the 64-bit "actual size of physical memory". NOT memory_size
        // beside it, which is a 32-bit natural_t the SDK header itself annotates
        // "size of memory in bytes, capped at 2 GB" (it measures larger than
        // that here, which only sharpens the point: it is not a figure to
        // compute with). It was the source of usage.available, and so of the
        // guest's MemAvailable: measured on this 24 GiB Mac it reads
        // 3419914240, and 3419914240 / 1024 = 3339760, which is exactly the
        // "MemAvailable: 3339760 kB" the guest printed -- a constant, about an
        // eighth of the machine, and not an availability figure in any case.
        usage.total = basic.max_mem;
    } else {
        // Fall back to sysctl for the physical memory size. hw.memsize is the
        // same number by another route (25769803776 here, agreeing with
        // max_mem to the byte).
        uint64_t memsize = 0;
        size_t len = sizeof(memsize);
        if (sysctlbyname("hw.memsize", &memsize, &len, NULL, 0) == 0)
            usage.total = memsize;
    }

    vm_statistics64_data_t vm = {};
    count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info_t) &vm, &count) == KERN_SUCCESS) {
        // free_count is NOT the free list. It is the free list PLUS the
        // speculative (read-ahead) pages, which are then reported a second time
        // in speculative_count, so the two overlap exactly. Measured here four
        // samples running: free_count 7654, speculative_count 3365, and
        // sysctl vm.page_free_count 4289 -- free_count minus speculative_count
        // to the page, which is also the figure vm_stat(1) labels "Pages free"
        // (4305 at that instant). Subtracting it is what makes MemFree and
        // Cached disjoint the way Linux reports them: before this the guest read
        // MemFree 171360 kB beside Cached 63536 kB with those 63536 kB in both,
        // so free(1) charged the speculative pool against "used" twice.
        uint64_t free_pages = vm.free_count > vm.speculative_count ?
                              (uint64_t) vm.free_count - vm.speculative_count : 0;
        usage.free = free_pages * vm_page_size;
        usage.cached = (uint64_t) vm.speculative_count * vm_page_size;
        usage.active = (uint64_t) vm.active_count * vm_page_size;
        usage.inactive = (uint64_t) vm.inactive_count * vm_page_size;
        usage.wirecount = (uint64_t) vm.wire_count * vm_page_size;
        usage.swapins = (uint64_t) vm.swapins * vm_page_size;
        usage.swapouts = (uint64_t) vm.swapouts * vm_page_size;

        // MemAvailable means "how much more could be had without paging", so it
        // has to be a live figure and a subset of total; setting it to the size
        // of memory says the machine is entirely idle, which is never true and
        // was the second half of the bug above. XNU publishes no single counter
        // for it, so add up the page classes the pager can hand out without an
        // application's involvement: the free list, the speculative pages it
        // reads ahead into, the inactive queue and the purgeable-volatile
        // pages. Each term appears ONCE -- free_pages above is already net of
        // speculative, and summing vm.free_count with speculative_count instead
        // counts that pool twice: at the sample above that is 4.73 GiB claimed
        // against 4.68 GiB real, 53840 kB of memory that does not exist.
        // Measured, in GiB: 0.07 free + 0.05 speculative + 4.55 inactive +
        // 0.01 purgeable = 4.68, beside 4.62 GiB active and 8.33 GiB held by
        // the compressor.
        //
        // This is deliberately MORE optimistic than Linux's own MemAvailable,
        // which is free minus the watermark plus the file LRU plus reclaimable
        // slab and pointedly EXCLUDES inactive anonymous memory, because Linux
        // would have to swap to get it back. XNU would not: the compressor
        // takes inactive anon without asking anyone. That distinction matters
        // here because inactive is mostly anon on this machine -- measured
        // alongside the numbers above, internal_page_count 481071 (7.34 GiB)
        // against external_page_count 123705 (1.89 GiB).
        usage.available = (free_pages + (uint64_t) vm.speculative_count +
                           vm.inactive_count + vm.purgeable_count) * vm_page_size;
        if (usage.available > usage.total)
            usage.available = usage.total;
    } else {
        // The VM stats are unavailable, so we know the machine's size and
        // nothing about its use. Report it all free and all available rather
        // than none: a guest allocator reading MemAvailable 0 gives up, and
        // this path exists precisely because these host_* calls have been seen
        // failing on iOS 26/27, where a guest that cannot allocate is a worse
        // outcome than an optimistic number. Free and available move together
        // because MemAvailable can never exceed MemFree plus what is
        // reclaimable, and here nothing is reported reclaimable at all -- an
        // idle, freshly booted machine is a state Linux produces, while
        // "0 free, 24 GiB available, nothing cached" (what this branch used to
        // print) is not. Everything else stays 0.
        usage.free = usage.total;
        usage.available = usage.total;
    }
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

// ISH_GUEST_MEM_BUDGET_MB: treat this process as though its ceiling were that
// many MiB. macOS names no per-process ceiling -- measured, not assumed: the
// task_vm_info.limit_bytes_remaining this file reads below is 0 on this Mac --
// so without the knob host_mem_headroom_low() returned false unconditionally
// off iOS and the entire low-memory path (the mmap/brk/mremap ENOMEM guard, and
// everything phase 1 of docs/simulated_swap_plan.md hangs off the same signal)
// could be exercised only on a device. With it, ISH_GUEST_MEM_BUDGET_MB=768 on
// this Mac reaches the same code the jetsam budget reaches on an iPad.
//
// On iOS it can only TIGHTEN the real ceiling, never replace it:
// sample_mem_budget() publishes the smaller of the two. A knob that replaced a
// 2 GiB jetsam limit with =8192 would disable the shipping guard outright while
// looking like it had configured it.
//
// A budget at or below the ISH_GUEST_MEM_HEADROOM_MB floor leaves the guard on
// from the first guest mmap, which is the pair of knobs doing what they say;
// lower the floor to test a budget that small. Bytes, or 0 for "not set".
//
// Parsed with strtoll and complained about loudly, because the likeliest defect
// in a knob is a typo and this one used to swallow every kind: atol("512m") is
// 512, atol("abc") is 0, 0 means "not set", and so a mis-set knob left the
// low-memory test it was configuring passing for the wrong reason. Cached once;
// the parse is idempotent, so an init race costs a repeat and nothing else
// (same reasoning as mem_headroom_threshold_mb below).
static uint64_t mem_budget_knob_bytes(void) {
    static long long budget_mb = -1;
    if (budget_mb < 0) {
        // Largest MiB count whose byte value still fits in a uint64_t, so the
        // multiply below cannot wrap -- and a wrap to exactly 0 would read back
        // as "not set", which is the one failure this knob must never have.
        const long long max_mb = (long long) (UINT64_MAX >> 20);
        budget_mb = 0;
        const char *env = getenv("ISH_GUEST_MEM_BUDGET_MB");
        if (env != NULL && env[0] != '\0') {
            char *end = NULL;
            long long mb = strtoll(env, &end, 10);
            if (end == env || *end != '\0' || mb < 0)
                printk("ISH_GUEST_MEM_BUDGET_MB=%s is not a plain count of MiB; ignored\n", env);
            else if (mb > max_mb)
                printk("ISH_GUEST_MEM_BUDGET_MB=%s is too large; ignored\n", env);
            else
                budget_mb = mb;
        }
    }
    return (uint64_t) budget_mb * 1024 * 1024;
}

// One coherent reading of what the host says about THIS PROCESS's memory. Both
// figures come out of a single task_info(TASK_VM_INFO) trap, and that is the
// point: the ceiling is derived below as remaining + footprint, and taking
// those from two separate calls makes the sum drift by whatever the app
// allocated in between -- a scheduling quantum's worth if the thread is
// preempted between them, not a microsecond's. That drift lands in the guest's
// MemTotal, which is not allowed to wobble.
//
// Apple's os/proc.h says os_proc_available_memory() "is equivalent to the
// task_vm_info.limit_bytes_remaining field", and that the same task_info call
// also returns the phys_footprint that figure is calculated from -- so the two
// fields of one reply are the same quantity's two halves, each derived from the
// other. os_proc_available_memory() stays as the fallback for a kernel whose
// reply predates that field.
//
// Measured on this Mac: the reply is revision 7 (count 93, against
// TASK_VM_INFO_REV4_COUNT 86), phys_footprint is filled, limit_bytes_remaining
// is 0 -- macOS having no per-process limit, which is why the knob exists.
struct host_mem_reading {
    // phys_footprint: dirty anonymous plus compressed plus IOKit, the ledger
    // jetsam actually kills on, and NOT the file-backed pages resident_size
    // counts. The budget is measured against it and not against RSS.
    bool footprint_known;
    uint64_t footprint;
    // The bytes the host says are left under its own ceiling. Unknown rather
    // than 0 when the host declines to say.
    bool remaining_known;
    uint64_t remaining;
};

static struct host_mem_reading read_host_mem(void) {
    struct host_mem_reading reading = {};
    bool reply_has_limit_field = false;

    task_vm_info_data_t info = {};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t) &info, &count) == KERN_SUCCESS) {
        // phys_footprint arrived in revision 1 of this struct and
        // limit_bytes_remaining in revision 4. A kernel older than the SDK we
        // compiled against fills fewer fields and says so in count, and the
        // rest of the struct is then whatever we zeroed it to.
        if (count >= TASK_VM_INFO_REV1_COUNT) {
            reading.footprint = info.phys_footprint;
            reading.footprint_known = true;
        }
        if (count >= TASK_VM_INFO_REV4_COUNT) {
            reply_has_limit_field = true;
            // 0 is not a byte count here. The field clamps at 0 both when the
            // process is over its limit and when it has none at all (macOS, and
            // any process that is not an app), so it is left UNKNOWN and
            // sample_mem_budget() decides what the absence means with the
            // latched ceiling in hand.
            if (info.limit_bytes_remaining != 0) {
                reading.remaining = info.limit_bytes_remaining;
                reading.remaining_known = true;
            }
        }
    }

#if TARGET_OS_IPHONE
    if (!reply_has_limit_field) {
        // Only when the reply carried no such field: Apple documents the two as
        // the same number, so asking again after a reply that did carry it
        // would be a second trap for an answer we have.
        size_t proc_avail = os_proc_available_memory();
        if (proc_avail != 0) {
            reading.remaining = proc_avail;
            reading.remaining_known = true;
        }
    }
#else
    (void) reply_has_limit_field;
#endif
    return reading;
}

// The published sample. Values are stored first and the flag that vouches for
// them last, with release/acquire against the loads in get_mem_budget(), so a
// thread arriving while another is inside sample_mem_budget() reads the
// previous sample whole instead of half of the new one. The remaining loads are
// relaxed: the worst they can do is pair figures from two samples 10 ms apart,
// and no caller can act on that difference.
static _Atomic uint64_t mem_budget_sampled_at;   // mach_absolute_time units, 0 = never
static _Atomic uint64_t mem_budget_limit;        // the host's own ceiling, latched; 0 = never determined
static _Atomic uint64_t mem_budget_total;        // what we publish: the tighter of that and the knob
static _Atomic uint64_t mem_budget_available;
static _Atomic bool mem_budget_available_known;
static _Atomic bool mem_budget_remaining_refused; // see the carve-out in host_mem_headroom_low()

// Note what is deliberately NOT cached here: the host's own "bytes left under
// my ceiling" answer. host_mem_headroom_low() needs that raw figure, because
// the budget can be UNKNOWN in a state where the reading is not -- latching a
// ceiling needs BOTH halves of one task_info reply, so a
// task_info(TASK_VM_INFO) that fails outright latches nothing ever and would
// switch off the only thing standing between a runaway guest and a jetsam kill,
// on exactly the device whose kernel refused the trap. That is not
// hypothetical: get_mem_usage above records the sibling host_* calls returning
// non-KERN_SUCCESS on iOS 26/27.
//
// But the guard reads it from the reading IT took, handed back through
// mem_budget_read()'s raw_out, never from a published copy. A sibling thread
// sampling in between would overwrite a shared copy, and a sibling whose
// task_info failed would overwrite it with "unknown" -- making this guard go
// quiet on its own evidence of pressure. See the fallback in
// host_mem_headroom_low().

// A move in the derived ceiling smaller than this is measurement skew and
// MemTotal must not follow it; a move larger than this is the host actually
// changing the limit. 4 MiB sits between the two by a wide margin in both
// directions: the skew of the fallback path is bounded by what this process can
// allocate while preempted between two traps (1 MiB at a sustained 1 GiB/s and
// a full millisecond of preemption, and the single-trap path above is orders of
// magnitude tighter than that), while a real limit change is the OS switching
// between two configured values that are whole megabytes apart.
#define MEM_BUDGET_LIMIT_STEP (4ull * 1024 * 1024)

// Fills *out with this sample and publishes it for the readers that are not due
// to sample. The caller gets its own result back directly, so the very first
// caller never has to read state nothing has written yet.
static void sample_mem_budget(struct mem_budget *out, struct host_mem_reading *raw_out) {
    struct host_mem_reading reading = read_host_mem();
    if (raw_out != NULL)
        *raw_out = reading;
    uint64_t knob = mem_budget_knob_bytes();

    // (1) The host's own ceiling, LATCHED, because this becomes the guest's
    // MemTotal and MemTotal is constant on every real Linux. It is derived as
    // remaining + footprint: the remaining figure is the limit minus
    // phys_footprint clamped at 0 (docs/simulated_swap_plan.md section 3.10,
    // reading xnu's kern_memorystatus.c; Apple's os/proc.h says the same in
    // prose), so adding the footprint back recovers the limit itself, and that
    // is the only way an app can learn its own jetsam limit.
    //
    // The latch is what makes the figure a constant. Sampling it afresh each
    // time would make MemTotal wobble by whatever the app allocated between the
    // two halves of the reading, and a /proc/meminfo whose MemTotal differs
    // between two consecutive reads is a machine no Linux produces without
    // memory hotplug -- while every consumer that caches it on first read (the
    // JVM sizing its heap, htop's scale, free(1)'s percentages) would be
    // describing a different machine from the next reader.
    //
    // It can still step, and deliberately so: Apple's os/proc.h warns that
    // "memory limits can change during the app life cycle", and the plan's
    // day-1 device probe (section 7, item (h)) still has "limit_bytes_remaining
    // read once while backgrounded" open, so we have not measured whether ours
    // does. A move past MEM_BUDGET_LIMIT_STEP is therefore taken as real and
    // republished -- the guest sees MemTotal step, the way a Linux guest sees
    // it step when a balloon inflates or a memory block is offlined -- and
    // anything smaller is ignored as skew.
    uint64_t limit = atomic_load_explicit(&mem_budget_limit, memory_order_relaxed);
    if (reading.remaining_known && reading.footprint_known) {
        uint64_t seen = reading.remaining + reading.footprint;
        uint64_t drift = seen > limit ? seen - limit : limit - seen;
        if (limit == 0 || drift > MEM_BUDGET_LIMIT_STEP) {
            limit = seen;
            atomic_store_explicit(&mem_budget_limit, limit, memory_order_relaxed);
        }
    }

    // (2) The ceiling we publish: the tighter of the host's and the knob's.
    uint64_t total = limit;
    if (knob != 0 && (total == 0 || knob < total))
        total = knob;

    // (3) The bytes left under each ceiling that answered, of which we publish
    // the smaller. A ceiling whose remainder nothing could measure contributes
    // nothing here rather than contributing a 0, because 0 is a byte count and
    // this is the absence of one -- the distinction struct mem_budget's
    // available_known exists to carry.
    bool available_known = false;
    bool remaining_refused = false;
    uint64_t available = 0;

    if (reading.remaining_known) {
        available = reading.remaining;
        available_known = true;
    } else if (limit != 0 && reading.footprint_known) {
        // The host declined to name the remainder. That 0 has two meanings and
        // the API does not say which -- Apple's os/proc.h: "0 is returned if the
        // calling process is not an app, or the calling process exceeds its
        // memory limit" -- but reaching this branch settles it. A ceiling is
        // only ever latched from a positive reading, and a process that is not
        // an app never gets one, so if we have a ceiling then this process IS
        // an app and the 0 means at or over the limit. With the ceiling and the
        // footprint both in hand the remainder is arithmetic, so the figure
        // published here is honest and self-consistent rather than an invented
        // 0 wearing a "known" flag. host_mem_headroom_low() still declines to
        // ACT on it; see the carve-out there, which is now the only place that
        // treats this reading specially.
        available = reading.footprint < limit ? limit - reading.footprint : 0;
        available_known = true;
        remaining_refused = true;
    }

    if (knob != 0 && reading.footprint_known) {
        uint64_t knob_left = reading.footprint < knob ? knob - reading.footprint : 0;
        if (!available_known || knob_left < available)
            available = knob_left;
        available_known = true;
    }

    if (!available_known)
        available = total;      // the header's contract: unmeasured reads as idle, not as dead
    else if (available > total)
        available = total;      // MemFree can never exceed MemTotal

    atomic_store_explicit(&mem_budget_total, total, memory_order_relaxed);
    atomic_store_explicit(&mem_budget_available, available, memory_order_relaxed);
    atomic_store_explicit(&mem_budget_remaining_refused, remaining_refused, memory_order_relaxed);
    atomic_store_explicit(&mem_budget_available_known, available_known, memory_order_release);

    out->known = total != 0;
    out->total = out->known ? total : 0;
    out->available_known = out->known && available_known;
    out->available = out->known ? available : 0;
}

// One reading of the budget: sampled on a 10 ms timer for the callers that only
// print it, taken fresh for the one caller that has to act on it.
//
// SAMPLED for /proc/meminfo and sysinfo(2). A figure up to 10 ms old there is
// no worse than the host counters printed beside it, and a guest that cats
// /proc/meminfo in a loop would otherwise pay a Mach trap a line. Measured on
// this Mac: task_info(TASK_VM_INFO) costs 0.45 us per call against 5.5 ns for
// mach_absolute_time(), so reading the clock instead of the ledger is about 80x
// cheaper (the previous round of this work recorded 0.96 us for the same call;
// either way the trap is the expensive half by two orders of magnitude).
//
// FRESH for host_mem_headroom_low(), and that is a correction: the first two
// rounds of this work had the jetsam guard reading the 10 ms sample, and the
// measurement says a 10 ms-old answer is not good enough for it. The guard's
// whole job is to refuse guest growth BEFORE the app is killed, its default
// margin is 192 MiB, and a guest can eat most of that margin inside one window.
// Measured here with an arm64 guest under build/ish: first-touching fresh
// anonymous pages runs at up to 15.7 GiB/s (768 MiB in 0.048 s; three runs gave
// 6.3, 15.7, 15.6 GiB/s, the first with a cold free-page pool), which is
// 65-160 MiB inside one 10 ms window -- and vmmap confirms the app's
// phys_footprint follows the guest byte for byte, 4.35 MiB before the guest
// touched 512 MiB and 524.7 MiB after. A sampled guard would therefore say yes
// with as little as 32 MiB of its 192 MiB margin actually left, and the shape
// it exists to stop -- malloc in a loop: brk grow, touch, brk grow -- is
// exactly the shape that consults it far faster than the window refreshes. The
// standalone harness for this file shows the same thing without any arithmetic:
// with the guard on the sampled path, a row that latched a ceiling and then had
// the host's remaining figure drop to 100 MiB still answered 2400 MiB and let
// the guest grow.
//
// Lazy faulting does NOT rescue that, though it is worth being precise about
// what it does buy. The growth the guard is deciding about really does cost
// nothing at the instant it is granted -- measured: the guest's mmap of 512 MiB
// finished below its own clock's resolution, and the app's phys_footprint was
// still 4.35 MiB three seconds later, before the guest touched a page of it --
// so the guard is not racing a deadline for THAT mapping. But the reading it
// decides on is a statement about memory the guest was handed earlier and is
// dirtying right now, and that is the part the window hides.
//
// The freshness is affordable, which is the other half of the trade. Measured
// on this Mac, from inside the guest: mmap(4 KiB, anonymous) costs 10.1 us and
// brk(2) growing by one page costs 2.4 us, against the 0.45 us task_info that a
// fresh read adds -- 4.5% and 19% on the two call sites, to stop the guard
// going blind to up to 83% of its own margin. Apple's os/proc.h says the same
// thing about the figure underneath: "Developers can query this value
// efficiently whenever it is needed... Caching the result is not advised."
//
// Sampling more often does not make MemTotal wobble more, which is the thing
// the latch exists to prevent. Both halves of the latched ceiling come out of
// ONE task_info reply, in which the kernel computed limit_bytes_remaining from
// the phys_footprint it returned beside it, so `seen` is the limit exactly
// however often it is read. Only the os_proc_available_memory() fallback pairs
// two separate traps, and that branch cannot be reached on a supported device:
// limit_bytes_remaining arrived in revision 4 of task_vm_info (iOS 13) and this
// app's deployment target is iOS 15.0.
//
// The brk site is a BARRIER HOLD and not just CPU, which is why the sample
// stays a single Mach trap with no allocation in it: kernel/mmap.c takes
// mem_write_lock_with_pokes(&mm->mem) in sys_brk_guest and calls
// host_mem_headroom_low() a few lines later, so every sibling guest thread
// waits out the trap. 0.45 us added to a call that already holds that lock
// across a host mmap is the same argument that forced linux.c's /proc read off
// stdio -- there the sample is three syscalls, so linux.c keeps its guard on
// the 10 ms window; on Linux there is no jetsam and the guard is a test
// facility reached only through ISH_GUEST_MEM_BUDGET_MB, so nothing is at risk
// there that is at risk here.
static struct mem_budget mem_budget_read(bool must_be_fresh, struct host_mem_reading *raw_out) {
    struct mem_budget budget = {};
#if !TARGET_OS_IPHONE
    if (mem_budget_knob_bytes() == 0)
        return budget; // macOS: no per-process ceiling exists to report
#endif

    // 10 ms in mach_absolute_time units. Same idempotent-init race as above.
    static uint64_t sample_interval_ticks;
    if (sample_interval_ticks == 0) {
        mach_timebase_info_data_t timebase;
        mach_timebase_info(&timebase);
        sample_interval_ticks = 10ull * 1000 * 1000 * timebase.denom / timebase.numer;
    }
    // last == 0 is checked on its own rather than left to the subtraction, so
    // that a first call inside the first 10 ms of the host's uptime samples
    // instead of answering from a cache nothing has written yet.
    uint64_t now = mach_absolute_time();
    uint64_t last = atomic_load_explicit(&mem_budget_sampled_at, memory_order_relaxed);
    if (must_be_fresh || last == 0 || now - last >= sample_interval_ticks) {
        // Stamp before sampling so that on the TIMER path a burst of threads
        // arriving together does not all take the trap. The ones that skip read
        // the PREVIOUS sample, which is a whole and self-consistent one; they
        // cannot see this one half-written, because its values are published
        // before the flag that vouches for them. A must_be_fresh caller takes
        // the trap regardless -- that is the point of it -- and the stamp then
        // just buys the next /proc reader a very recent sample.
        atomic_store_explicit(&mem_budget_sampled_at, now, memory_order_relaxed);
        sample_mem_budget(&budget, raw_out);
        return budget;
    }

    bool available_known = atomic_load_explicit(&mem_budget_available_known, memory_order_acquire);
    budget.total = atomic_load_explicit(&mem_budget_total, memory_order_relaxed);
    if (budget.total == 0)
        return (struct mem_budget) {}; // no ceiling determined yet
    budget.known = true;
    budget.available_known = available_known;
    budget.available = available_known ?
        atomic_load_explicit(&mem_budget_available, memory_order_relaxed) : budget.total;
    return budget;
}

struct mem_budget get_mem_budget(void) {
    return mem_budget_read(false, NULL);
}

// Cached once; env parse is idempotent so an init race is harmless.
static long mem_headroom_threshold_mb(void) {
    static long threshold_mb = -1;
    if (threshold_mb < 0) {
        const char *env = getenv("ISH_GUEST_MEM_HEADROOM_MB");
        long t = env != NULL ? atol(env) : 192;
        threshold_mb = t >= 0 ? t : 0;
    }
    return threshold_mb;
}

uint64_t host_mem_headroom_floor(void) {
    return (uint64_t) mem_headroom_threshold_mb() * 1024 * 1024;
}

bool host_mem_headroom_low(void) {
    if (mem_headroom_threshold_mb() == 0)
        return false; // guard disabled

    // Fresh, not the 10 ms sample -- this is the jetsam guard, and the case for
    // paying a Mach trap here is measured out above mem_budget_read().
    struct host_mem_reading raw = {};
    struct mem_budget budget = mem_budget_read(true, &raw);

    if (!budget.known) {
        // "Unknown" here is NOT "no pressure". Reading it as one was the
        // regression the budget work introduced, and this is where it is
        // repaired -- it is not a refinement. budget.known requires a LATCHED
        // ceiling, the latch requires both halves of one task_info reply, and
        // so a task_info(TASK_VM_INFO) that fails outright latches nothing,
        // ever -- total stays 0, known stays false, and the jetsam guard goes
        // quiet for the life of the process on a device where it used to fire.
        // struct mem_budget is an ADDITION for /proc's sake, not a replacement
        // for what this guard reads.
        //
        // So fall back to precisely what this function did before the budget
        // existed: the host's own remaining figure against the same floor.
        // mem_budget_read(true) has just taken it, and read_host_mem() sources
        // it from os_proc_available_memory() exactly when the task_info reply
        // did not answer, which is this state.
#if TARGET_OS_IPHONE
        // 0 stays "no limit information" rather than "no memory left" (the
        // simulator and debugging contexts return it), which is why
        // read_host_mem() left the flag false for it: never starve the guest on
        // a bad reading. Same decision, same source, same floor as before.
        // From OUR OWN reading, not from the published atomics. A sibling
        // thread sampling in the window between this call's store and its load
        // can overwrite them, and if that sibling's task_info failed it stores
        // remaining_known = false -- which would make this guard answer "no
        // pressure" on a reading of its own that said otherwise. A guard that
        // goes quiet because another thread had a bad sample is exactly the
        // silent weakening this fallback exists to prevent.
        if (!raw.remaining_known)
            return false;
        return raw.remaining < host_mem_headroom_floor();
#else
        // macOS with no knob set. No per-process ceiling exists to be near, and
        // os_proc_available_memory() is API_UNAVAILABLE(macos), so there is no
        // second source to fall back to -- the same false the #else arm of this
        // function returned before, for the same reason.
        return false;
#endif
    }

    if (!budget.available_known)
        return false; // a reading nobody could take is not evidence of pressure

#if TARGET_OS_IPHONE
    // The one carve-out, kept deliberately and now as narrow as it can be. When
    // the OS refuses to name the bytes remaining, this guard has always read
    // that as "no limit information" and let the guest grow. sample_mem_budget()
    // can now tell that 0's two meanings apart -- a latched ceiling proves the
    // process is an app, so the 0 means at or over the limit, and it publishes
    // the honest remainder for /proc to print. But flipping THIS line changes
    // what a device does at the moment it is closest to being jetsammed, from
    // "let the guest grow" to "refuse every growth", and that is a change to
    // make with a device to test it on rather than one to smuggle in beside a
    // refactor. docs/simulated_swap_plan.md section 3.10 specifies the flip.
    //
    // What is no longer done here is patching around the budget: /proc now
    // prints the arithmetic, and a guest that reads MemAvailable 0 while its
    // mmap still succeeds is looking at ordinary Linux overcommit, not at an
    // impossible pair of numbers. With the knob set the remainder came from a
    // footprint we read rather than from a refused reading, so it stands.
    if (mem_budget_knob_bytes() == 0 &&
        atomic_load_explicit(&mem_budget_remaining_refused, memory_order_relaxed))
        return false;
#endif

    return budget.available < host_mem_headroom_floor();
}
