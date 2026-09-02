#ifdef __linux__

#include <sys/sysinfo.h>
#include <time.h>          // time(), time_t -- for the guest's boot_time
#include <errno.h>         // EINTR -- the /proc/self/statm read below
#include <fcntl.h>         // open() -- ditto, and see why it is not fopen()
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>        // read(), close(), sysconf() -- ditto
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

// read_proc_line() above die()s when the key is missing. That is tolerable for
// the fields this build has always required, but not for one that a kernel may
// simply not export, so this reports failure instead of taking the emulator
// down. kB in, bytes out, since every caller here wants bytes.
//
// It reopens the file per key, as read_proc_line() always has, so a
// /proc/meminfo read costs six opens rather than four. That is a dev and CI
// build reading a procfs file, not the device path, and one open of
// /proc/meminfo is cheaper than the page-table walk the guest-side reader does
// on top of it.
static bool read_proc_kb(const char *file, const char *name, uint64_t *out) {
    FILE *f = fopen(file, "r");
    if (f == NULL)
        return false;
    char buf[1234];
    bool found = false;
    size_t name_len = strlen(name);
    while (fgets(buf, sizeof(buf), f) != NULL) {
        if (strncmp(name, buf, name_len) != 0)
            continue;
        uint64_t kb = 0;
        if (sscanf(buf + name_len, " %"SCNu64" kB", &kb) == 1) {
            *out = kb * 1024;
            found = true;
        }
        break;
    }
    fclose(f);
    return found;
}

struct cpu_usage get_total_cpu_usage(void) {
    struct cpu_usage usage = {};
    char buf[1234];
    read_proc_line("/proc/stat", "cpu", buf);
    sscanf(buf, "cpu %"SCNu64" %"SCNu64" %"SCNu64" %"SCNu64"\n", &usage.user_ticks, &usage.system_ticks, &usage.idle_ticks, &usage.nice_ticks);
    return usage;
}

struct mem_usage get_mem_usage(void) {
    // Zero-initialised, because this used to fill four of the nine fields and
    // leave the rest holding whatever was on the stack. available, cached,
    // swapins, swapouts and wirecount were all printed to the guest from
    // uninitialised memory: /proc/meminfo's MemAvailable and Cached, and
    // /proc/vmstat's pswpin/pswpout.
    struct mem_usage usage = {};

    // Every field of struct mem_usage is in BYTES -- fs/proc/root.c's show_kb()
    // divides by 1024 on its way out, and kernel/resource.c's
    // maxrss_plausible_pages() divides by PAGE_SIZE. /proc/meminfo is in kB,
    // and this used to store the kB figure straight into the struct, so the
    // Linux build reported a host with 16 GiB of RAM as MemTotal 16384 kB, i.e.
    // 16 MiB, to a guest -- and gave ru_maxrss a plausibility ceiling 1024
    // times too low, so on an 8 GiB host no peak above 7.7 MiB could be
    // recorded at all. read_proc_kb() scales on the way in.
    //
    // Each read is checked and a missing or malformed key leaves its field at
    // 0, rather than at the PREVIOUS field's value, which is what a single
    // shared sscanf() scratch variable does: a MemFree: line that failed to
    // parse would have told the guest MemFree == MemTotal, and an Inactive:
    // line Inactive == Active. A zero is read downstream as "not reported",
    // which is a state Linux produces; a neighbour's value wearing this field's
    // name is a state nothing has ever been tested against.
    read_proc_kb("/proc/meminfo", "MemFree:", &usage.free);
    read_proc_kb("/proc/meminfo", "Active:", &usage.active);
    read_proc_kb("/proc/meminfo", "Inactive:", &usage.inactive);
    read_proc_kb("/proc/meminfo", "Cached:", &usage.cached);

    // MemTotal is the one field here with no honest zero -- a machine with no
    // RAM is not a machine -- so it gets a second source rather than a die().
    // sysinfo(2) is the same figure by another route, from the same kernel.
    if (!read_proc_kb("/proc/meminfo", "MemTotal:", &usage.total)) {
        struct sysinfo info;
        if (sysinfo(&info) == 0)
            usage.total = (uint64_t) info.totalram * (uint64_t) info.mem_unit;
    }

    // MemAvailable is what the guest's own MemAvailable comes from when no
    // budget is known, so take the host kernel's estimate rather than inventing
    // one; it has been there since Linux 3.14 but is not guaranteed, hence the
    // fallback to the free list, which is the pessimistic half of the same
    // figure.
    if (!read_proc_kb("/proc/meminfo", "MemAvailable:", &usage.available))
        usage.available = usage.free;
    if (usage.available > usage.total)
        usage.available = usage.total;

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

// ISH_GUEST_MEM_BUDGET_MB: treat this process as though it had that many MiB.
// A Linux host imposes no such ceiling, so without the knob there is nothing to
// report and host_mem_headroom_low() below stays false -- but with it, CI and a
// Linux dev box can exercise the guest-facing low-memory path (the mmap/brk/
// mremap ENOMEM guard) that otherwise only a device can reach. A budget at or
// below the ISH_GUEST_MEM_HEADROOM_MB floor leaves the guard on from the first
// guest mmap, which is the pair of knobs doing what they say; lower the floor
// to test a budget that small. Bytes, or 0 for "not set".
//
// Parsed and validated exactly as the Darwin build parses it -- see the long
// comment there. The short version: a typo used to be swallowed in both
// directions, atol("512m") is 512 and atol("abc") is 0, and 0 means "not set",
// so a mis-set knob left the low-memory test it was configuring passing for the
// wrong reason. Cached once; the parse is idempotent so an init race is
// harmless.
static uint64_t mem_budget_knob_bytes(void) {
    static long long budget_mb = -1;
    if (budget_mb < 0) {
        // Largest MiB count whose byte value still fits in a uint64_t, so the
        // multiply below cannot wrap -- and a wrap to exactly 0 would read back
        // as "not set", the one failure this knob must never have.
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

// The resident set from /proc/self/statm field 2 (pages), in bytes. Returns
// false when it cannot be read -- this runs on the guest mmap path, so it must
// not die() the way read_proc_line() above does, and "could not measure" must
// not arrive downstream as the number 0.
//
// open/read/close rather than stdio, deliberately. host_mem_headroom_low() is
// called from sys_brk_guest() with mem_write_lock_with_pokes already held (both
// are in kernel/mmap.c, the lock a few lines above the call), so once per
// sampling window this read runs with every sibling guest thread poked and
// waiting on the barrier. fopen() would take the host allocator's lock under
// that barrier for a file whose whole content is under 64 bytes.
//
// This is an APPROXIMATION of what the Darwin build measures. Darwin uses
// phys_footprint, the ledger jetsam kills on; RSS is not the same quantity. It
// counts file-backed pages this process merely has mapped from the host page
// cache, which a footprint largely excludes, and it stops counting anything the
// host has swapped or compressed, which a footprint still charges for. That is
// good enough for a knob whose purpose is to make the headroom guard reachable
// on a host with no per-process ceiling. It is not a jetsam ledger, and no
// policy decision that matters on a device should be tuned against it.
static bool linux_rss_bytes(uint64_t *out) {
    int fd = open("/proc/self/statm", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    char buf[128];
    ssize_t n;
    do {
        n = read(fd, buf, sizeof(buf) - 1);
    } while (n < 0 && errno == EINTR);
    close(fd);
    if (n <= 0)
        return false;
    buf[n] = '\0';

    // "size resident shared text lib data dt", in pages. Field 2 is the one we
    // want, and both conversions are checked: strtoull() returns 0 on a string
    // it cannot parse, which would read as an empty process.
    char *end = NULL;
    strtoull(buf, &end, 10);
    if (end == buf)
        return false;
    char *size_end = end;
    unsigned long long resident_pages = strtoull(size_end, &end, 10);
    if (end == size_end)
        return false;

    static long page_size;  // idempotent init, like the knobs
    if (page_size <= 0) {
        page_size = sysconf(_SC_PAGESIZE);
        if (page_size <= 0)
            page_size = 4096;
    }
    *out = (uint64_t) resident_pages * (uint64_t) page_size;
    return true;
}

// See the long comment on the Darwin implementation for why this is sampled
// rather than measured: host_mem_headroom_low() consults it on every guest
// mmap, brk growth and mremap growth. A sample here is three syscalls and a
// procfs text render rather than the one 0.96 us Mach trap measured there --
// not measured natively, since this build's homes are CI and a dev box, but
// the comparison only runs one way -- and at the brk site it is spent with
// every sibling guest thread held at the barrier. Same 10 ms window, and the
// same reasoning that a guest cannot move its footprint far enough inside one
// window to cross a 192 MiB margin unseen.
//
// The value is published before the flag that vouches for it, with
// release/acquire between them, so a reader either gets a whole sample or is
// told there is none.
static _Atomic uint64_t mem_budget_sampled_at_ms;  // 0 = never sampled
static _Atomic uint64_t mem_budget_available;
static _Atomic bool mem_budget_available_known;

// Never 0 on success, so 0 can mean "no sample yet" below.
static uint64_t monotonic_ms(void) {
    struct timespec ts;
#ifdef CLOCK_MONOTONIC_COARSE
    // A vDSO read of the tick the kernel already keeps, so no syscall to ask
    // the time before deciding not to sample. Granularity is one CONFIG_HZ
    // tick: 1 ms at the usual HZ=1000, and 10 ms at HZ=100, so the 10 ms window
    // can stretch to 20 ms on such a kernel. Still inside the tolerance argued
    // on the Darwin side, where the guest would have to move 20 MiB of
    // footprint unseen against a 192 MiB margin.
    clockid_t clock = CLOCK_MONOTONIC_COARSE;
#else
    clockid_t clock = CLOCK_MONOTONIC;
#endif
    if (clock_gettime(clock, &ts) != 0)
        return 0;
    uint64_t ms = (uint64_t) ts.tv_sec * 1000 + (uint64_t) ts.tv_nsec / 1000000;
    return ms != 0 ? ms : 1;
}

// Fills *out with this sample and publishes it for the readers that are not due
// to sample, so that the sampling thread never has to read back state it has
// only just written.
static void sample_mem_budget(uint64_t total, struct mem_budget *out) {
    uint64_t rss = 0;
    bool measured = linux_rss_bytes(&rss);
    uint64_t available = measured ? (rss < total ? total - rss : 0) : total;

    atomic_store_explicit(&mem_budget_available, available, memory_order_relaxed);
    atomic_store_explicit(&mem_budget_available_known, measured, memory_order_release);

    out->known = true;
    out->total = total;
    out->available_known = measured;
    out->available = available;
}

struct mem_budget get_mem_budget(void) {
    struct mem_budget budget = {};
    uint64_t total = mem_budget_knob_bytes();
    if (total == 0)
        return budget; // no per-process ceiling on this host, and none asked for

    // The "not sampled yet" case is checked explicitly rather than falling out
    // of the arithmetic. If it were not, then in the first 10 ms of the host's
    // uptime -- and permanently, if clock_gettime() ever failed -- this would
    // answer from a cache nothing has written. A broken clock instead resamples
    // on every call: right answer, slow.
    uint64_t now = monotonic_ms();
    uint64_t last = atomic_load_explicit(&mem_budget_sampled_at_ms, memory_order_relaxed);
    if (last == 0 || now - last >= 10) {
        // Stamp first so a burst of threads does not all read /proc.
        atomic_store_explicit(&mem_budget_sampled_at_ms, now, memory_order_relaxed);
        sample_mem_budget(total, &budget);
        return budget;
    }

    // Not due, so answer from the last sample. The flag is read FIRST, with
    // acquire against the release store that published it, which is what makes
    // the window between the stamp above and the store inside the sample safe:
    // a second thread arriving in that window is told the available figure is
    // unknown, and the header's contract turns that into "idle", not into "no
    // memory". Getting this wrong was guest-visible on exactly the
    // configuration the knob exists for -- with `known` unconditionally true on
    // this build, the unwritten 0 became an ENOMEM for every guest mmap, brk
    // and mremap growth, and a MemFree: 0 kB in any /proc/meminfo read that
    // raced the first sample.
    bool available_known = atomic_load_explicit(&mem_budget_available_known, memory_order_acquire);
    budget.known = true;
    budget.total = total;
    budget.available_known = available_known;
    budget.available = available_known ?
        atomic_load_explicit(&mem_budget_available, memory_order_relaxed) : total;
    return budget;
}

// Cached once; env parse is idempotent so an init race is harmless. Kept in
// step with the Darwin build, which owns the shipping default.
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
    // No hard per-process memory budget on a Linux host; the OOM killer and
    // overcommit policy own this, and the guard is an iOS-jetsam concern. The
    // one exception is a deliberately configured ISH_GUEST_MEM_BUDGET_MB, which
    // exists so that the guard, and the guest behaviour behind it, can be
    // tested here at all.
    if (mem_headroom_threshold_mb() == 0)
        return false; // guard disabled
    struct mem_budget budget = get_mem_budget();
    if (!budget.known)
        return false;
    if (!budget.available_known)
        return false; // a reading nobody could take is not evidence of pressure
    return budget.available < host_mem_headroom_floor();
}

#endif
