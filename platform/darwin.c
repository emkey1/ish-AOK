#include <mach/mach.h>
#include <dispatch/dispatch.h>
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

    // Everything above describes the MACHINE. The guest must not be told the
    // machine's figures: section 3.12 of docs/simulated_swap_plan.md says
    // MemTotal is the app's budget, and the rule behind it is that the host's
    // own counters must never leak into the guest.
    //
    // The cost of getting this wrong is not cosmetic. On a 3 GB iPhone SE the
    // guest read MemTotal 2957 MB -- the DEVICE's RAM -- while AOK's actual
    // ceiling was 2098 MB, and MemFree of ~26 MB, which is iOS holding most of
    // RAM as it always does and says nothing about what the guest may have.
    // So `free`, `top` and ktop inside the guest all reported the phone as
    // ~100% full at idle: ktop's own arithmetic was correct and its inputs were
    // somebody else's memory. A guest allocator cannot act on that.
    //
    // Only when a ceiling is actually latched. On macOS with no
    // ISH_GUEST_MEM_BUDGET_MB there is no per-process ceiling to report, and
    // the machine's figures are the honest answer -- which also leaves the CLI
    // and the whole regression gate byte-for-byte unchanged.
    struct mem_budget budget = get_mem_budget();
    if (budget.known && budget.total != 0) {
        usage.total = budget.total;
        // Both from the same figure, and deliberately: what the guest may still
        // have is the headroom under the ceiling, less the floor the growth
        // guard will not let it cross. MemAvailable can never exceed MemFree
        // plus what is reclaimable, and with the ceiling as the total there is
        // no host page cache in the guest's world to reclaim, so the two are
        // equal rather than invented separately.
        uint64_t floor = host_mem_headroom_floor();
        uint64_t usable = 0;
        if (budget.available_known && budget.available > floor)
            usable = budget.available - floor;
        else if (!budget.available_known)
            usable = budget.total;   // unmeasured reads as idle, not as dead
        usage.free = usable;
        usage.available = usable;
        if (usage.free > usage.total) usage.free = usage.total;
        if (usage.available > usage.total) usage.available = usage.total;
        // Cached/active/inactive described the machine's page classes and have
        // no meaning against a per-process ceiling; leaving them would have the
        // guest's `free` print a Cached column larger than MemTotal.
        usage.cached = 0;
        usage.active = 0;
        usage.inactive = 0;
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
        // 0 wearing a "known" flag -- and host_mem_headroom_low() now ACTS on
        // it, which it did not before section 3.10's flip. At or over the limit
        // the remainder is 0, which is under any floor, so the guard fires.
        available = reading.footprint < limit ? limit - reading.footprint : 0;
        available_known = true;
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

// The floor is a FRACTION OF THE CEILING, not a constant, with the configured
// value as its lower bound.
//
// MEASURED on a 3 GB iPhone SE, 2026-09-05, and the arithmetic is the whole
// argument. Its ceiling is 2097 MB (read off two MEMORY PRESSURE breadcrumbs:
// footprint 1076 + headroom 1021, and 119 + 1978). A flat 192 MB floor means
// the guard refuses growth once the footprint passes 1905 MB. Jetsam killed the
// app at a footprint of 1860 MB. The guard was 45 MB from firing and could
// never have saved it -- the margin was not thin, it was NEGATIVE.
//
// How much headroom a process needs before the system gives up on it scales
// with how large that process is allowed to become, so the floor has to scale
// with the ceiling too. A sixth puts the SE's floor at 350 MB, refusing growth
// around 1747 MB and leaving ~113 MB of margin against the observed kill.
//
// Capped at 512 MB so a device with a large ceiling does not have a gigabyte
// reserved out from under the guest, and never below the configured value, so
// ISH_GUEST_MEM_HEADROOM_MB still means what it says and setting it to 0 still
// disables the guard.
//
// Fitted to ONE device's kill. It is a better rule than a constant that is
// provably too small there, but the fraction itself deserves a second data
// point before it is treated as settled.
#define MEM_HEADROOM_CEILING_FRACTION 6
#define MEM_HEADROOM_FRACTION_CAP (512ull * 1024 * 1024)

uint64_t host_mem_headroom_floor(void) {
    uint64_t configured = (uint64_t) mem_headroom_threshold_mb() * 1024 * 1024;
    if (configured == 0)
        return 0;   // guard disabled; scaling nothing is still nothing
    uint64_t ceiling = atomic_load_explicit(&mem_budget_total, memory_order_relaxed);
    if (ceiling == 0)
        return configured;  // no ceiling latched yet, nothing to scale against
    uint64_t scaled = ceiling / MEM_HEADROOM_CEILING_FRACTION;
    if (scaled > MEM_HEADROOM_FRACTION_CAP)
        scaled = MEM_HEADROOM_FRACTION_CAP;
    return scaled > configured ? scaled : configured;
}

// ===========================================================================
// System-wide memory pressure.
//
// host_mem_headroom_low() answers "am I near MY limit". That is not the
// question that gets an app killed on a small device. MEASURED on a 3 GB
// iPhone SE, 2026-09-05: AOK was jetsam-killed with reason "proc-thrashing"
// while its own arithmetic said ~382 MB of headroom remained and its swap area
// was 200 MB empty. The JetsamEvent report named it largestProcess at 1.86 GB,
// the DEVICE had 40 MB free, and dozens of Apple daemons were being killed in
// the same cascade. Every signal AOK watched was per-process, so it watched its
// own limit stay comfortable right up to the moment the machine died under it.
//
// DISPATCH_SOURCE_TYPE_MEMORYPRESSURE is the system's own answer, and section
// 3.10 of docs/simulated_swap_plan.md has listed it as unused since the plan
// was written -- along with the observation that jetsam kills currently have no
// preceding breadcrumb in the log. Both are fixed here.
static _Atomic unsigned mem_pressure_level;

// ISH_GUEST_MEM_PRESSURE=warn|critical forces the level, because the real
// source cannot be provoked without root (`memory_pressure -l critical`) and a
// guard nobody has watched fire is worth nothing. Parsed once; unset means the
// real source decides.
static int mem_pressure_override(void) {
    static int override = -2;
    if (override == -2) {
        const char *e = getenv("ISH_GUEST_MEM_PRESSURE");
        if (e == NULL) override = -1;
        else if (strcmp(e, "critical") == 0) override = HOST_MEM_PRESSURE_CRITICAL;
        else if (strcmp(e, "warn") == 0) override = HOST_MEM_PRESSURE_WARN;
        else if (strcmp(e, "normal") == 0) override = HOST_MEM_PRESSURE_NORMAL;
        else override = -1;
    }
    return override;
}

unsigned host_mem_pressure_level(void) {
    int forced = mem_pressure_override();
    if (forced >= 0)
        return (unsigned) forced;
    return atomic_load_explicit(&mem_pressure_level, memory_order_relaxed);
}

// What the THROTTLE asks, as opposed to what the growth guard asks. Warn is
// enough here: the right response to the system getting tight is to start
// paying for memory sooner, not to start refusing it.
bool host_mem_should_reclaim(void) {
    return host_mem_pressure_level() >= HOST_MEM_PRESSURE_WARN ||
           host_mem_headroom_low();
}

void host_mem_pressure_start(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        dispatch_source_t src = dispatch_source_create(
                DISPATCH_SOURCE_TYPE_MEMORYPRESSURE, 0,
                DISPATCH_MEMORYPRESSURE_NORMAL | DISPATCH_MEMORYPRESSURE_WARN |
                DISPATCH_MEMORYPRESSURE_CRITICAL,
                dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0));
        if (src == NULL)
            return;
        dispatch_source_set_event_handler(src, ^{
            unsigned long flags = dispatch_source_get_data(src);
            unsigned level = HOST_MEM_PRESSURE_NORMAL;
            if (flags & DISPATCH_MEMORYPRESSURE_CRITICAL)
                level = HOST_MEM_PRESSURE_CRITICAL;
            else if (flags & DISPATCH_MEMORYPRESSURE_WARN)
                level = HOST_MEM_PRESSURE_WARN;
            unsigned was = atomic_exchange_explicit(&mem_pressure_level, level,
                    memory_order_relaxed);
            if (was == level)
                return;
            // The breadcrumb the plan asks for. A jetsam kill leaves nothing in
            // our own log to say it was coming; this is the only warning the
            // system gives, so record it with the numbers that explain it.
            struct host_mem_reading raw = {};
            struct mem_budget b = mem_budget_read(true, &raw);
            printk("MEMORY PRESSURE %s (was %s): footprint %llu MB, "
                   "own headroom %llu MB%s\n",
                   level == HOST_MEM_PRESSURE_CRITICAL ? "CRITICAL" :
                       level == HOST_MEM_PRESSURE_WARN ? "WARN" : "normal",
                   was == HOST_MEM_PRESSURE_CRITICAL ? "CRITICAL" :
                       was == HOST_MEM_PRESSURE_WARN ? "WARN" : "normal",
                   (unsigned long long) (raw.footprint >> 20),
                   (unsigned long long) (b.available >> 20),
                   b.available_known ? "" : " (unknown)");
        });
        dispatch_resume(src);
        // Deliberately never cancelled or released: it lives for the life of
        // the process, and letting it go would stop the only warning we get.
    });
}

bool host_mem_headroom_low(void) {
    if (mem_headroom_threshold_mb() == 0)
        return false; // guard disabled

    // The system's own answer outranks ours: when the DEVICE is short of memory
    // it does not matter how much of our own limit is unused, which is exactly
    // the state the SE died in -- 382 MB of per-process headroom, 40 MB free on
    // the machine.
    //
    // CRITICAL only, NOT warn. This function REFUSES GUEST GROWTH, and iOS
    // raises warn routinely on a small device; treating warn as a refusal makes
    // the guest unusable rather than merely slow. Measured: forcing warn here
    // refused every mmap the shell needed and produced no output at all.
    // Warn instead drives the throttle (host_mem_should_reclaim), which slows
    // the guest and pages memory out without denying it anything.
    if (host_mem_pressure_level() >= HOST_MEM_PRESSURE_CRITICAL)
        return true;

    // And the DEVICE's own free memory, against the same floor. This guard was
    // written around a per-process ceiling, which is the right instrument when
    // the ceiling is much smaller than the machine -- and useless when it is not.
    //
    // MEASURED on a 3 GB iPhone SE, 2026-09-05: at the moment the guest could no
    // longer allocate, the DEVICE had 310 MB free while this function computed
    // ~1.6 GB of our own headroom and cheerfully allowed more. iOS's per-process
    // limit on that phone is roughly the size of RAM, so obeying only that limit
    // means AOK is permitted to consume the entire machine, and the earlier
    // JetsamEvent is what that looks like: largestProcess at 1.86 GB, reason
    // proc-thrashing, 40 MB free device-wide, Apple's own daemons dying around
    // it. The host refused the allocation before we did, which is the wrong way
    // round -- by then the damage is done.
    //
    // get_mem_usage() already reports the machine's figures (it is what the
    // guest's MemAvailable is built from), so this costs no new trap beyond the
    // ones it already makes.
    struct mem_usage machine = get_mem_usage();
    if (machine.available != 0 && machine.available < host_mem_headroom_floor())
        return true;

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

    // THERE IS NO LONGER A CARVE-OUT HERE, and that is the point of section 3.10
    // of docs/simulated_swap_plan.md. When the OS refused to name the bytes
    // remaining, this guard used to read that as "no limit information" and let
    // the guest grow -- at the one moment the process is closest to being
    // jetsammed.
    //
    // The two meanings of 0 are distinguishable, and sample_mem_budget() already
    // distinguishes them: os_proc_available_memory() returns 0 both when there
    // is no per-process limit to speak of (the simulator, macOS, before the
    // ceiling is known) and when the process is AT OR OVER that limit
    // (kern_memorystatus.c clamps it). A LATCHED CEILING proves the process is
    // an app with a real limit, so once budget.known is true -- which is the
    // only way execution reaches this line -- a refused remainder can only mean
    // the second. Reading it as "no information" was reading the most dangerous
    // state in the system as the safest.
    //
    // budget.available is the honest remainder computed from a footprint we
    // read, not from the refused reading, so the comparison below stands on its
    // own and no carve-out is needed: at or over the limit it is at or below
    // zero, which is under any floor, and the guard fires.
    //
    // Safe to make now in a way it was not before, because a guest that gets
    // ENOMEM from here is no longer the only thing standing between the app and
    // jetsam: kernel/mmap.c's mem_fault_backpressure throttles, and ultimately
    // OOM-kills, a guest committing memory through the fault path, which is
    // where the app was actually dying.

    return budget.available < host_mem_headroom_floor();
}

// ===========================================================================
// The day-1 host page release probe (docs/simulated_swap_plan.md section 7).
//
// One question, and everything in the pager design is downstream of it: when
// AOK hands a 16 KiB host page back to the kernel, does the number iOS decides
// to kill this app on actually go down by 16 KiB? It has been measured
// byte-exact on this Mac -- section 4.1, -16,384 B per aligned host page,
// repeatedly -- and it has never once been observed on a device. That is open
// risk 1 in section 8, and section 7 makes settling it the first day's work
// because a "no" kills the whole design family before any pager code is
// written.
//
// Three findings from the verification round shape this probe, and each of them
// is a way a naive version reports a green light that is not there:
//
// 1. IT DECIDES ON phys_footprint, NEVER ON os_proc_available_memory(). That
//    call is API_UNAVAILABLE(macos), and its task_info twin
//    limit_bytes_remaining reads 0 on this Mac -- measured, and the reason
//    ISH_GUEST_MEM_BUDGET_MB exists at all -- while on iOS it clamps to 0 both
//    when the process is over its limit and when it has none. So a probe keyed
//    on it reads "nothing happened" on the reference host and "no memory left"
//    on a device that is merely busy. task_info(TASK_VM_INFO).phys_footprint is
//    the very ledger jetsam reads (section 2.4: os_proc_available_memory() is
//    memlimit - get_task_phys_footprint(task), clamped), so a delta there IS
//    the answer. avail is printed beside it for comparison and decides nothing.
//
// 2. A 0 RETURN FROM madvise() IS NOT EVIDENCE OF A RELEASE. XNU returns
//    KERN_SUCCESS with kill_pages = -1 and moves no ledger at all whenever the
//    range's VM object is copy-on-write shared or shadowed (section 2.4,
//    vm_map.c:17190-17253). fork(), a dirtied MAP_PRIVATE file mapping, and
//    mach_vm_read / mach_vm_copy above 32 KiB on arm64 all put an object into
//    that state, and it persists after the sharer is gone until a write fault
//    collapses the chain. AN ATTACHED DEBUGGER OR AN INSTRUMENTS MEMORY GRAPH
//    IS A mach_vm_read. So every case here verifies that the ledger moved and
//    prints DID NOT MOVE in as many words when it did not, the report says to
//    run with nothing attached, and case (d) is a deliberate positive control
//    for this exact branch: a dirtied private FILE mapping, which is EXPECTED to
//    return 0 and release nothing. If (a) reads like (d), the region was shared
//    or shadowed -- not iOS ignoring the primitive.
//
// 3. THE PROT_NONE COMPANION IS PART OF THE DESIGN NOW (section 2.1), because a
//    REUSABLE'd frame stays mapped, readable and byte-identical, so a pointer
//    holder the design missed writes into a released frame with no fault and no
//    diagnostic and the swap-in silently reverts the write. Its cost is vm_map
//    entries: 4096 scattered frames took one mapping from 2 entries to 8193 on
//    macOS, which took it in its stride, and the iOS ceiling is unknown (open
//    risk 3, the one thing that could make the companion unusable). So case (c)
//    scatters the release over alternate frames and counts entries before, while
//    N frames are out, and after restoring them.
//
// SAFETY, because this runs inside the app process on someone's iPad and a
// probe that gets the app jetsammed mid-run, or that leaks its allocation,
// leaves the machine worse than it found it:
//   - it never runs on a read. Only a write to /proc/ish/mem_release_probe
//     starts it (fs/proc/ish.c); reading reports the last result.
//   - the size is a parameter with a small default and a hard cap, not the
//     2 GiB section 7 sketched.
//   - it refuses to start when this process is already near its ceiling, using
//     the same fresh reading host_mem_headroom_low() decides on, and says so.
//   - every case unmaps what it mapped on every exit path, failures included,
//     and case (c) restores each frame's protection before writing to it.
// ===========================================================================

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/proc.h>
#include <unistd.h>

// One reading of everything the probe measures, out of ONE task_info trap --
// the same reason read_host_mem() above takes both of its fields from one
// reply. Two traps drift by whatever the rest of the app allocated in between,
// and here that drift lands directly inside the delta being measured.
struct release_probe_sample {
    bool known;                 // the reply carried phys_footprint
    uint64_t footprint;         // the ledger jetsam kills on: the decision
    uint64_t limit_remaining;   // task_vm_info.limit_bytes_remaining, raw
    int64_t regions;            // vm_map entries: task_vm_info.region_count
    bool proc_avail_known;      // os_proc_available_memory() exists here
    uint64_t proc_avail;
};

static struct release_probe_sample release_probe_take_sample(void) {
    struct release_probe_sample sample = {};
    task_vm_info_data_t info = {};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t) &info, &count) == KERN_SUCCESS) {
        // region_count sits ahead of phys_footprint in the struct and predates
        // revision 1, so any successful reply carries it. phys_footprint is the
        // field that needs a revision test -- the same one read_host_mem() makes.
        sample.regions = info.region_count;
        if (count >= TASK_VM_INFO_REV1_COUNT) {
            sample.footprint = info.phys_footprint;
            sample.known = true;
        }
        if (count >= TASK_VM_INFO_REV4_COUNT)
            sample.limit_remaining = info.limit_bytes_remaining;
    }
#if TARGET_OS_IPHONE
    // Printed for comparison only, never acted on: 0 here means "not an app" OR
    // "over the limit", and the call does not exist on macOS at all.
    sample.proc_avail = os_proc_available_memory();
    sample.proc_avail_known = true;
#endif
    return sample;
}

struct release_probe_report {
    char *buf;
    size_t size;
    size_t len;
    bool truncated;
};

static void release_probe_printf(struct release_probe_report *r, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

static void release_probe_printf(struct release_probe_report *r, const char *format, ...) {
    if (r->buf == NULL || r->size == 0 || r->len + 1 >= r->size) {
        r->truncated = true;
        return;
    }
    va_list ap;
    va_start(ap, format);
    int n = vsnprintf(r->buf + r->len, r->size - r->len, format, ap);
    va_end(ap);
    if (n < 0) {
        r->truncated = true;
        return;
    }
    if ((size_t) n >= r->size - r->len) {
        r->len = r->size - 1;
        r->truncated = true;
        return;
    }
    r->len += (size_t) n;
}

static uint64_t release_probe_now_ns(void) {
    static mach_timebase_info_data_t timebase;
    if (timebase.denom == 0)
        mach_timebase_info(&timebase);
    return mach_absolute_time() * timebase.numer / timebase.denom;
}

// The one attachment that can be detected from inside the process. It is worth
// detecting because a debugger's mach_vm_read leaves the read region's VM
// object copy-on-write shared, and REUSABLE on a shared object returns 0 and
// moves nothing -- so a probe run under Xcode reports the design dead when it
// is not. An Instruments memory graph does the same thing and is NOT visible
// here, which is why the report tells the operator to detach both by hand.
static bool release_probe_debugger_attached(void) {
    struct kinfo_proc info = {};
    size_t size = sizeof(info);
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid() };
    if (sysctl(mib, 4, &info, &size, NULL, 0) != 0 || size == 0)
        return false;
    return (info.kp_proc.p_flag & P_TRACED) != 0;
}

static void release_probe_print_change(struct release_probe_report *r,
                                       const struct release_probe_sample *before,
                                       const struct release_probe_sample *after) {
    if (before->known && after->known)
        release_probe_printf(r, "      phys_footprint   before %-13" PRIu64 " after %-13" PRIu64 " delta %+" PRId64 "\n",
                             before->footprint, after->footprint,
                             (int64_t) after->footprint - (int64_t) before->footprint);
    else
        release_probe_printf(r, "      phys_footprint   UNAVAILABLE -- task_info(TASK_VM_INFO) did not answer\n");
    release_probe_printf(r, "      limit_remaining  before %-13" PRIu64 " after %-13" PRIu64 " delta %+" PRId64 "\n",
                         before->limit_remaining, after->limit_remaining,
                         (int64_t) after->limit_remaining - (int64_t) before->limit_remaining);
    if (before->proc_avail_known && after->proc_avail_known)
        release_probe_printf(r, "      os_proc_avail    before %-13" PRIu64 " after %-13" PRIu64 " delta %+" PRId64 "\n",
                             before->proc_avail, after->proc_avail,
                             (int64_t) after->proc_avail - (int64_t) before->proc_avail);
    release_probe_printf(r, "      vm_map entries   before %-13" PRId64 " after %-13" PRId64 " delta %+" PRId64 "\n",
                         before->regions, after->regions, after->regions - before->regions);
}

// The whole point of the probe. The return code of the release call is NOT
// consulted here on purpose (see note 2 at the top of this section): the ledger
// is the verdict and the only verdict. The one-quarter slack is for the rest of
// the app allocating underneath us while the case runs, not for a partial
// release -- the two states being told apart are "all of it" and "none of it",
// and on the reference host the delta is exact to the byte.
static bool release_probe_verdict(struct release_probe_report *r,
                                  const struct release_probe_sample *before,
                                  const struct release_probe_sample *after,
                                  uint64_t expected) {
    if (!before->known || !after->known) {
        release_probe_printf(r, "      VERDICT: UNKNOWN -- the host would not report phys_footprint\n");
        return false;
    }
    int64_t released = (int64_t) before->footprint - (int64_t) after->footprint;
    bool moved = expected > 0 && released >= (int64_t) (expected - expected / 4);
    release_probe_printf(r, "      VERDICT: %s -- released %" PRId64 " of %" PRIu64 " bytes expected\n",
                         moved ? "MOVED" : "DID NOT MOVE", released, expected);
    return moved;
}

static void release_probe_dirty(void *base, size_t bytes, size_t page, unsigned char pattern) {
    // volatile because an optimiser is entitled to drop stores to memory
    // nothing reads back, and dropping these would leave the region clean --
    // the probe would then be measuring the release of memory it never charged.
    volatile unsigned char *p = base;
    for (size_t off = 0; off < bytes; off += page)
        p[off] = pattern;
}

// (a) The pager's primary release primitive (section 3.4 step 6) and its
// restore (section 3.5 step 2). This case alone decides most of the answer.
// The REUSE half is measured separately because macOS re-charges the full
// amount on REUSE ALONE, with no write (section 4.1, run7 A3), and a device
// that did not would change the swap-in path.
static void release_probe_case_reusable(struct release_probe_report *r,
                                        size_t bytes, size_t page, bool *moved) {
    release_probe_printf(r, "(a) madvise(MADV_FREE_REUSABLE) over %zu dirtied private anonymous frames\n",
                         bytes / page);
    void *base = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (base == MAP_FAILED) {
        release_probe_printf(r, "      SKIPPED: mmap of %zu bytes failed (%s)\n", bytes, strerror(errno));
        return;
    }
    release_probe_dirty(base, bytes, page, 0xa1);

    struct release_probe_sample before = release_probe_take_sample();
    uint64_t start_ns = release_probe_now_ns();
    int rc = madvise(base, bytes, MADV_FREE_REUSABLE);
    int rc_errno = errno;   // before the clock read, which is entitled to clobber it
    uint64_t elapsed_ns = release_probe_now_ns() - start_ns;
    struct release_probe_sample after = release_probe_take_sample();
    release_probe_printf(r, "      madvise returned %d%s%s, %" PRIu64 " us for %zu frames\n",
                         rc, rc == 0 ? "" : " errno ", rc == 0 ? "" : strerror(rc_errno),
                         elapsed_ns / 1000, bytes / page);
    release_probe_print_change(r, &before, &after);
    *moved = release_probe_verdict(r, &before, &after, bytes);

    // a2: REUSE with no write at all.
    struct release_probe_sample reuse_before = release_probe_take_sample();
    int reuse_rc = madvise(base, bytes, MADV_FREE_REUSE);
    int reuse_errno = errno;
    struct release_probe_sample reuse_after = release_probe_take_sample();
    release_probe_printf(r, "    a2 madvise(MADV_FREE_REUSE), no write: returned %d%s%s\n",
                         reuse_rc, reuse_rc == 0 ? "" : " errno ",
                         reuse_rc == 0 ? "" : strerror(reuse_errno));
    release_probe_print_change(r, &reuse_before, &reuse_after);

    // a3: and the write on top, which is what a real swap-in does after its
    // pread. On macOS a2 has already re-charged everything, so this is ~0.
    struct release_probe_sample write_before = release_probe_take_sample();
    release_probe_dirty(base, bytes, page, 0xa3);
    struct release_probe_sample write_after = release_probe_take_sample();
    release_probe_printf(r, "    a3 write to every frame after the REUSE\n");
    release_probe_print_change(r, &write_before, &write_after);

    munmap(base, bytes);
}

// (b) The fallback section 7 names if (a) fails: replace the range with fresh
// anonymous memory at the same address.
//
// Spelled mmap(MAP_FIXED) over the LIVE mapping rather than munmap-then-mmap,
// and that is a safety deviation from the plan's wording, not an oversight. A
// munmap here opens a window in which any other thread of this app -- and there
// are dozens, one per guest thread plus GCD -- can be handed that virtual
// address by its own mmap, and the MAP_FIXED that follows would then silently
// clobber a live mapping belonging to someone else. Section 2.2 measured
// mmap(NULL,...) reusing a just-munmap'd VA 2000 times out of 2000, so this is
// the likely case and not the unlucky one. MAP_FIXED over a live range replaces
// it inside the kernel with no window, and moves the same ledger: section 4.1
// records mmap(MAP_FIXED) at -16,384 B per frame in every state tested.
static void release_probe_case_remap(struct release_probe_report *r,
                                     size_t bytes, size_t page, bool *moved) {
    release_probe_printf(r, "(b) mmap(MAP_FIXED) replacing %zu dirtied private anonymous frames\n",
                         bytes / page);
    void *base = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (base == MAP_FAILED) {
        release_probe_printf(r, "      SKIPPED: mmap of %zu bytes failed (%s)\n", bytes, strerror(errno));
        return;
    }
    release_probe_dirty(base, bytes, page, 0xb1);

    struct release_probe_sample before = release_probe_take_sample();
    void *again = mmap(base, bytes, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    int rc_errno = errno;
    struct release_probe_sample after = release_probe_take_sample();
    if (again == MAP_FAILED) {
        release_probe_printf(r, "      mmap(MAP_FIXED) failed (%s); the old mapping is untouched\n",
                             strerror(rc_errno));
        munmap(base, bytes);
        return;
    }
    release_probe_printf(r, "      mmap(MAP_FIXED) returned the same address: %s\n",
                         again == base ? "yes" : "NO -- unexpected, both ranges are unmapped below");
    release_probe_print_change(r, &before, &after);
    *moved = release_probe_verdict(r, &before, &after, bytes);

    struct release_probe_sample write_before = release_probe_take_sample();
    release_probe_dirty(again, bytes, page, 0xb2);
    struct release_probe_sample write_after = release_probe_take_sample();
    release_probe_printf(r, "    b2 write to every frame of the replacement\n");
    release_probe_print_change(r, &write_before, &write_after);

    munmap(again, bytes);
    if (again != base)
        munmap(base, bytes);
}

// (c) REUSABLE followed by mprotect(PROT_NONE), which section 2.1 makes
// mandatory in production: without it a released frame stays readable and
// writable, and a missed pointer holder corrupts guest memory in complete
// silence. Two things are being measured, and the second is the reason this
// case scatters the release over alternate frames instead of covering the range
// in one call: whether PROT_NONE keeps the footprint drop, and what it costs in
// vm_map entries, which is open risk 3 -- macOS took 131,072 PROT_NONE holes
// without complaint and the iOS ceiling is unknown. Alternate frames is the
// worst case for the map, and the shape real eviction produces.
//
// The order is forced: REUSABLE on a range that is already PROT_NONE returns
// EPERM (section 2.1, c2plat5 section 2).
static void release_probe_case_protect(struct release_probe_report *r,
                                       size_t bytes, size_t page, bool *moved,
                                       int64_t *entries_base, int64_t *entries_out,
                                       int64_t *entries_restored) {
    size_t frames = bytes / page;
    size_t targets = (frames + 1) / 2;
    release_probe_printf(r, "(c) madvise(MADV_FREE_REUSABLE) + mprotect(PROT_NONE) on %zu of %zu frames, alternating\n",
                         targets, frames);
    void *base = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (base == MAP_FAILED) {
        release_probe_printf(r, "      SKIPPED: mmap of %zu bytes failed (%s)\n", bytes, strerror(errno));
        return;
    }
    release_probe_dirty(base, bytes, page, 0xc1);

    struct release_probe_sample before = release_probe_take_sample();
    size_t released = 0, protected_ok = 0, reusable_failed = 0, protect_failed = 0;
    int first_errno = 0;
    uint64_t start_ns = release_probe_now_ns();
    for (size_t f = 0; f < frames; f += 2) {
        char *frame = (char *) base + f * page;
        if (madvise(frame, page, MADV_FREE_REUSABLE) != 0) {
            if (first_errno == 0)
                first_errno = errno;
            reusable_failed++;
            continue;
        }
        // Counted as released HERE, before the mprotect, and that ordering is
        // the whole point. The madvise above has already handed the frame back
        // and its bytes are already out of phys_footprint; if the mprotect then
        // fails, the frame is still released and still in the delta. Counting
        // it only on the mprotect would understate `released`, and since that
        // is the denominator release_probe_verdict() scores the delta against,
        // a run where every mprotect failed would compare a full delta against
        // an expectation of zero and print MOVED -- hiding exactly the vm_map
        // entry exhaustion this case exists to find.
        released += page;
        if (mprotect(frame, page, PROT_NONE) != 0) {
            if (first_errno == 0)
                first_errno = errno;
            protect_failed++;
            continue;
        }
        protected_ok += page;
    }
    uint64_t elapsed_ns = release_probe_now_ns() - start_ns;
    struct release_probe_sample after = release_probe_take_sample();
    // strerror() is only reached when something actually failed: strerror(0) is
    // "Undefined error: 0", which read as a failure in the first dry run of a
    // case where nothing had failed at all.
    release_probe_printf(r, "      %zu frames released (%zu of them also PROT_NONE), %zu REUSABLE failures, %zu mprotect failures%s%s\n",
                         released / page, protected_ok / page, reusable_failed, protect_failed,
                         first_errno == 0 ? "" : "; first errno: ",
                         first_errno == 0 ? "" : strerror(first_errno));
    if (released > 0)
        release_probe_printf(r, "      %.2f us per frame to evict\n",
                             (double) elapsed_ns / 1000.0 / (double) (released / page));
    release_probe_print_change(r, &before, &after);
    *moved = release_probe_verdict(r, &before, &after, released);
    *entries_base = before.regions;
    *entries_out = after.regions;

    // Restore, one frame at a time, writing ONLY to a frame whose protection
    // came back. A write to a frame still PROT_NONE is a SIGBUS that would take
    // the whole app down, and mprotect can fail here -- running out of vm_map
    // entries is precisely the failure this case exists to find.
    size_t restore_failed = 0;
    uint64_t restore_start_ns = release_probe_now_ns();
    for (size_t f = 0; f < frames; f += 2) {
        char *frame = (char *) base + f * page;
        if (mprotect(frame, page, PROT_READ | PROT_WRITE) != 0) {
            restore_failed++;
            continue;
        }
        madvise(frame, page, MADV_FREE_REUSE);
        ((volatile unsigned char *) frame)[0] = 0xc2;
    }
    uint64_t restore_ns = release_probe_now_ns() - restore_start_ns;
    struct release_probe_sample restored = release_probe_take_sample();
    release_probe_printf(r, "    c2 restore: mprotect(RW) + MADV_FREE_REUSE + write, %zu failures, %.2f us per frame\n",
                         restore_failed,
                         targets > 0 ? (double) restore_ns / 1000.0 / (double) targets : 0.0);
    release_probe_print_change(r, &after, &restored);
    *entries_restored = restored.regions;

    munmap(base, bytes);
}

// (d) The control, and the reason a DID NOT MOVE above is readable at all. A
// dirtied MAP_PRIVATE file mapping sits on a shadowed VM object, so REUSABLE on
// it is EXPECTED to return 0 and release nothing (section 2.4: "a dirtied 16 MiB
// private file mapping gives ret 0 delta 0"). If (a) produced the same reading
// as this one, the anonymous region was on a shared or shadowed object too --
// which is what a debugger or an Instruments memory graph does to it -- rather
// than iOS ignoring the primitive.
//
// It measures two more things worth having. That these pages are charged to
// phys_footprint 1:1 in the first place, which is open risk 15: every relocated
// .data, .got and RELRO page of every ELF the guest runs is in this class
// (fs/real.c maps guest files MAP_PRIVATE|PROT_WRITE on Apple), and the pager
// can never release them with REUSABLE. And that mprotect CAN release them,
// which is the only route to that class if one is ever wanted.
//
// The file is sparse and unlinked immediately: ftruncate costs no disk write,
// the dirtying happens in the shadow object rather than on NAND, and nothing is
// left behind if this process dies in the middle.
static void release_probe_case_private_file(struct release_probe_report *r,
                                            size_t bytes, size_t page,
                                            bool *reusable_moved, bool *protect_moved) {
    release_probe_printf(r, "(d) control: %zu dirtied MAP_PRIVATE file-backed frames\n", bytes / page);
    const char *tmpdir = getenv("TMPDIR");
    if (tmpdir == NULL || tmpdir[0] == '\0')
        tmpdir = "/tmp";
    char path[1024];
    int n = snprintf(path, sizeof(path), "%s/ish-aok-release-probe.XXXXXX", tmpdir);
    if (n < 0 || (size_t) n >= sizeof(path)) {
        release_probe_printf(r, "      SKIPPED: TMPDIR path too long\n");
        return;
    }
    int fd = mkstemp(path);
    if (fd < 0) {
        release_probe_printf(r, "      SKIPPED: could not create a file under %s (%s)\n",
                             tmpdir, strerror(errno));
        return;
    }
    unlink(path);
    if (ftruncate(fd, (off_t) bytes) != 0) {
        release_probe_printf(r, "      SKIPPED: ftruncate to %zu bytes failed (%s)\n",
                             bytes, strerror(errno));
        close(fd);
        return;
    }
    struct release_probe_sample map_before = release_probe_take_sample();
    void *base = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    if (base == MAP_FAILED) {
        release_probe_printf(r, "      SKIPPED: private file mmap of %zu bytes failed (%s)\n",
                             bytes, strerror(errno));
        close(fd);
        return;
    }
    release_probe_dirty(base, bytes, page, 0xd1);
    struct release_probe_sample map_after = release_probe_take_sample();
    release_probe_printf(r, "    d1 map and dirty (this class is charged to the footprint 1:1)\n");
    release_probe_print_change(r, &map_before, &map_after);

    struct release_probe_sample before = release_probe_take_sample();
    int rc = madvise(base, bytes, MADV_FREE_REUSABLE);
    int rc_errno = errno;
    struct release_probe_sample after = release_probe_take_sample();
    release_probe_printf(r, "    d2 madvise(MADV_FREE_REUSABLE): returned %d%s%s\n",
                         rc, rc == 0 ? "" : " errno ", rc == 0 ? "" : strerror(rc_errno));
    release_probe_print_change(r, &before, &after);
    *reusable_moved = release_probe_verdict(r, &before, &after, bytes);
    release_probe_printf(r, "      (DID NOT MOVE is the EXPECTED reading here -- shadowed object)\n");

    struct release_probe_sample prot_before = release_probe_take_sample();
    int prot_rc = mprotect(base, bytes, PROT_NONE);
    int prot_errno = errno;
    struct release_probe_sample prot_after = release_probe_take_sample();
    release_probe_printf(r, "    d3 mprotect(PROT_NONE) on the same range: returned %d%s%s\n",
                         prot_rc, prot_rc == 0 ? "" : " errno ",
                         prot_rc == 0 ? "" : strerror(prot_errno));
    release_probe_print_change(r, &prot_before, &prot_after);
    *protect_moved = release_probe_verdict(r, &prot_before, &prot_after, bytes);

    munmap(base, bytes);
    close(fd);
}

int host_mem_release_probe(unsigned long mb, char *report, size_t report_size) {
    struct release_probe_report r = { .buf = report, .size = report_size };
    if (report != NULL && report_size > 0)
        report[0] = '\0';

    // The design's frame is 16 KiB because that is the host page on Apple
    // silicon, and the host releases, protects and re-accounts nothing smaller
    // (section 3.2). Key everything off the real host page rather than a
    // hard-coded 16384, so that a 4 KiB-page host measures its own page and the
    // report says which one it measured instead of quietly rounding.
    size_t page = (size_t) vm_page_size;
    if (page == 0)
        page = 16384;
    if (mb == 0)
        mb = HOST_RELEASE_PROBE_DEFAULT_MB;
    if (mb > HOST_RELEASE_PROBE_MAX_MB)
        mb = HOST_RELEASE_PROBE_MAX_MB;
    size_t bytes = (size_t) mb * 1024 * 1024;
    bytes -= bytes % page;
    if (bytes < page * 2)
        bytes = page * 2;
    // The file case is capped harder than the anonymous ones: it is a control,
    // it needs no size to say what it says, and it is the only case that puts a
    // file on the device's storage even for a moment.
    size_t file_bytes = bytes;
    if (file_bytes > 16 * 1024 * 1024)
        file_bytes = 16 * 1024 * 1024;

    release_probe_printf(&r, "iSH-AOK host page release probe -- docs/simulated_swap_plan.md section 7, day 1\n");
    release_probe_printf(&r, "The question: does releasing a host page reduce the footprint iOS kills on?\n");
    release_probe_printf(&r, "RUN THIS WITH NO DEBUGGER AND NO INSTRUMENTS ATTACHED. A live mach_vm_read copy\n");
    release_probe_printf(&r, "of a region leaves its VM object copy-on-write shared, and MADV_FREE_REUSABLE on\n");
    release_probe_printf(&r, "a shared object returns 0 and moves no ledger -- so an attached tool makes this\n");
    release_probe_printf(&r, "probe report the design dead when it is not.\n\n");

#if TARGET_OS_SIMULATOR
    const char *host = "iOS Simulator (NOT a device: it has the Mac's VM, and answers for the Mac)";
#elif TARGET_OS_IPHONE
    const char *host = "iOS / iPadOS device -- this is the reading that decides the design";
#else
    const char *host = "macOS -- the reference host, not the answer for iOS";
#endif
    release_probe_printf(&r, "host             %s\n", host);
    release_probe_printf(&r, "host page        %zu bytes (the design's frame; 16384 on Apple silicon)\n", page);
    release_probe_printf(&r, "region           %zu bytes = %zu frames (file case: %zu bytes)\n",
                         bytes, bytes / page, file_bytes);
    bool traced = release_probe_debugger_attached();
    release_probe_printf(&r, "debugger         %s\n",
                         traced ? "ATTACHED -- see the warning at the end; results are not trustworthy"
                                  : "not attached (a memory-graph snapshot is invisible here; detach it by hand)");

    // The guard. It reads exactly what host_mem_headroom_low() reads, freshly,
    // because a probe that pushes a device already near its ceiling over the
    // edge gets the app jetsammed mid-run -- and the operator would then have a
    // dead app and no result, which is the one outcome worse than a "no".
    // Say once, at the top, what the "for comparison only" column is and why it
    // may be missing, so that a reader of the device report and a reader of the
    // Mac report are not looking for the same line in vain.
    struct release_probe_sample probe_avail = release_probe_take_sample();
    if (probe_avail.proc_avail_known)
        release_probe_printf(&r, "os_proc_avail    printed below for comparison ONLY: it clamps to 0 both when this process is over its limit and when it has none\n");
    else
        release_probe_printf(&r, "os_proc_avail    API_UNAVAILABLE(macos), so not printed below; its task_info twin limit_bytes_remaining is, and reads 0 here for the same reason\n");

    struct release_probe_sample baseline = release_probe_take_sample();
    if (baseline.known)
        release_probe_printf(&r, "footprint        %" PRIu64 " bytes now, in %" PRId64 " vm_map entries\n",
                             baseline.footprint, baseline.regions);
    else
        release_probe_printf(&r, "footprint        UNAVAILABLE -- task_info(TASK_VM_INFO) did not answer, so no case below can reach a verdict\n");
    if (!baseline.known) {
        // Refuse rather than run. Every verdict this probe prints is a
        // phys_footprint delta, so with no footprint there is nothing to
        // measure -- and this is the one state where the two guards below are
        // also blind, because host_mem_headroom_low() and mem_budget_read()
        // read their figures from the same trap that just refused. Allocating
        // and dirtying up to the cap here would put real memory pressure on a
        // device for a result that is uninterpretable by construction.
        release_probe_printf(&r, "\n");
        release_probe_printf(&r, "REFUSED: nothing was allocated. Every case below scores itself on a\n");
        release_probe_printf(&r, "phys_footprint delta, and this host would not report one, so the run could\n");
        release_probe_printf(&r, "not answer the question it exists to ask. The headroom guard reads the same\n");
        release_probe_printf(&r, "trap, so it cannot bound the run either.\n");
        return _ENOTSUP;
    }
    struct mem_budget budget = mem_budget_read(true, NULL);
    if (budget.known)
        release_probe_printf(&r, "budget           total %" PRIu64 ", available %s%" PRIu64 ", headroom floor %" PRIu64 "\n",
                             budget.total, budget.available_known ? "" : "(unmeasured) ",
                             budget.available, host_mem_headroom_floor());
    else
        // Not "no information about memory": the footprint above was read from
        // the same trap. It means no per-process CEILING exists to be near, so
        // the size cap is the only thing bounding this run. On iOS the jetsam
        // limit supplies one; on macOS ISH_GUEST_MEM_BUDGET_MB does.
        release_probe_printf(&r, "budget           unknown -- this host names no per-process ceiling, so the %lu MiB cap is the only bound (set ISH_GUEST_MEM_BUDGET_MB to give the guard one)\n",
                             (unsigned long) HOST_RELEASE_PROBE_MAX_MB);
    release_probe_printf(&r, "\n");

    if (host_mem_headroom_low()) {
        release_probe_printf(&r, "REFUSED: this process is already inside its headroom floor. Running the probe\n");
        release_probe_printf(&r, "now would allocate %zu bytes on top of that, and being jetsammed mid-run costs\n", bytes);
        release_probe_printf(&r, "the result as well as the app. Free some guest memory and try again.\n");
        return _ENOMEM;
    }
    if (budget.known && budget.available_known) {
        // Peak extra footprint is one region: the cases run one at a time and
        // each unmaps before the next starts. Ask for that plus the floor the
        // guard defends plus a margin, so the probe cannot itself be what puts
        // the process inside the floor.
        uint64_t need = (uint64_t) bytes + host_mem_headroom_floor() + 64 * 1024 * 1024;
        if (budget.available < need) {
            release_probe_printf(&r, "REFUSED: %" PRIu64 " bytes available, and this probe wants %" PRIu64 " -- the %zu byte\n",
                                 budget.available, need, bytes);
            release_probe_printf(&r, "region, the %" PRIu64 " byte headroom floor it must not eat into, and 64 MiB of margin.\n",
                                 host_mem_headroom_floor());
            release_probe_printf(&r, "Run it with a smaller mb=, or free some guest memory first.\n");
            return _ENOMEM;
        }
    }

    bool moved_reusable = false, moved_remap = false, moved_protect = false;
    bool file_reusable_moved = false, file_protect_moved = false;
    // -1 so that a case (c) which never ran (its mmap failed) prints "not
    // measured" instead of three zeroes that read like a real census.
    int64_t entries_base = -1, entries_out = -1, entries_restored = -1;

    uint64_t started_ns = release_probe_now_ns();
    release_probe_case_reusable(&r, bytes, page, &moved_reusable);
    release_probe_printf(&r, "\n");
    release_probe_case_remap(&r, bytes, page, &moved_remap);
    release_probe_printf(&r, "\n");
    release_probe_case_protect(&r, bytes, page, &moved_protect,
                               &entries_base, &entries_out, &entries_restored);
    release_probe_printf(&r, "\n");
    release_probe_case_private_file(&r, file_bytes, page, &file_reusable_moved, &file_protect_moved);
    uint64_t total_ms = (release_probe_now_ns() - started_ns) / 1000000;

    // Open risk 3: the PROT_NONE companion the design now requires turns one
    // mapping into thousands of vm_map entries, and the iOS ceiling for that is
    // unknown. This line is the measurement that finds it.
    if (entries_base >= 0)
        release_probe_printf(&r, "\nvm_map entries   %" PRId64 " before, %" PRId64 " with %zu scattered frames released, %" PRId64 " after restoring them\n",
                             entries_base, entries_out, (bytes / page + 1) / 2, entries_restored);
    else
        release_probe_printf(&r, "\nvm_map entries   not measured -- case (c) never ran\n");
    release_probe_printf(&r, "whole probe      %" PRIu64 " ms\n\n", total_ms);

    release_probe_printf(&r, "How to read this. (d) is the control: a dirtied private FILE mapping is on a\n");
    release_probe_printf(&r, "shadowed VM object, so its REUSABLE is EXPECTED to return 0 and release nothing.\n");
    release_probe_printf(&r, "If (a) read the same way as (d), the anonymous region was shared or shadowed too\n");
    release_probe_printf(&r, "-- detach every debugger and memory-graph tool and run it again -- rather than\n");
    release_probe_printf(&r, "this host ignoring the primitive. On the macOS reference (section 4.1) (a), (b)\n");
    release_probe_printf(&r, "and (c) each release the full region to the byte, (c) costs about one vm_map\n");
    release_probe_printf(&r, "entry per released frame and gives them all back on restore, and (d) releases\n");
    release_probe_printf(&r, "nothing until mprotect.\n\n");

    if (traced) {
        release_probe_printf(&r, "WARNING: A DEBUGGER IS ATTACHED. Any DID NOT MOVE above is uninterpretable:\n");
        release_probe_printf(&r, "its mach_vm_read of this process is exactly the state that makes REUSABLE a\n");
        release_probe_printf(&r, "silent no-op, and the state outlives the read. Detach and run it again.\n\n");
    }
    if (r.truncated)
        release_probe_printf(&r, "(report truncated)\n\n");

    // The one line the whole probe exists to produce.
    if (moved_reusable && moved_protect)
        release_probe_printf(&r, "PAGER DESIGN: VIABLE on this host -- releasing a host page really does reduce the footprint this OS kills the app on, and the PROT_NONE guard rail keeps the drop.\n");
    else if (moved_reusable)
        release_probe_printf(&r, "PAGER DESIGN: VIABLE on this host, but the PROT_NONE companion did not hold the drop, so section 2.1's guard rail needs rethinking before the pager is built.\n");
    else if (moved_remap || moved_protect)
        release_probe_printf(&r, "PAGER DESIGN: VIABLE on this host only through the fallback -- MADV_FREE_REUSABLE released nothing, so eviction must use the mmap(MAP_FIXED) or mprotect path instead.\n");
    else if (!file_protect_moved)
        release_probe_printf(&r, "PAGER DESIGN: NO ANSWER -- not one primitive moved the footprint, not even mprotect on the control mapping, so this reading is measuring something other than the ledger and must not be believed either way.\n");
    else
        release_probe_printf(&r, "PAGER DESIGN: NOT VIABLE on this host -- no release primitive moved phys_footprint on ordinary anonymous memory, so there is no headroom for a pager to buy back and the design family in docs/simulated_swap_plan.md is dead here.\n");
    return 0;
}
