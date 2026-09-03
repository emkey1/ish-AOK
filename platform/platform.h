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

// The HOST MACHINE's memory, in bytes. Every field here describes the computer
// iSH-AOK is running on, not the guest and not this process, so nothing in it
// can be handed to the guest unmodified: a guest-visible number has to be one a
// real Linux could produce for the machine the guest thinks it is on. Use
// get_mem_budget() for the guest-facing figures and fall back to these only
// when it says the host will not name a budget.
//
// AND FALL BACK AS A WHOLE, NEVER FIELD BY FIELD. A guest-visible file that
// takes MemTotal from the budget and its neighbours from here describes two
// machines at once: on this Mac the host's Active alone measures about 5 GiB,
// so beside a 512 MiB budget the guest reads MemFree + Active + Inactive an
// order of magnitude past MemTotal, which is not merely wrong but arithmetically
// impossible, and no consumer has ever been tested against it. Where the budget
// is known and a field here has no guest analogue, OMIT the key: htop, procps
// and busybox all read a missing /proc/meminfo key as 0, and a key Linux
// sometimes does not print is a state Linux produces, while a whole-machine
// figure inside a per-app kernel is not.
struct mem_usage {
    uint64_t total;
    uint64_t free;
    // An estimate of how much of `total` could still be handed out without
    // paging -- Linux's MemAvailable, approximated from the host's own page
    // classes. Always <= total. It used to be the size of memory 32-bit
    // truncated on Darwin, and uninitialised stack on Linux; see the comments
    // on both get_mem_usage() implementations.
    uint64_t available;
    uint64_t cached;
    uint64_t active;
    uint64_t inactive;
    // Dead weight, kept only so that a file mid-flight in another session does
    // not lose its compile: nothing outside platform/ reads these three any
    // more. They fed /proc/vmstat's pswpin/pswpout and a WireCount key in
    // /proc/meminfo, all three lifetime whole-machine XNU counters, which is
    // why fs/proc/root.c stopped printing them -- hundreds of gigabytes of
    // paging traffic beside SwapTotal: 0 kB is a machine that cannot exist. Do
    // not revive them; delete them once the swap work has settled.
    uint64_t swapins;
    uint64_t swapouts;
    uint64_t wirecount;
};
struct mem_usage get_mem_usage(void);

// What the host says about THIS PROCESS's memory ceiling -- the budget the app
// is killed at, not the size of the machine it runs on. The guest's MemTotal
// has to come from here. iSH-AOK is one app process with a jetsam budget, and
// reporting the device's RAM instead is both a number no Linux could produce
// for a machine of that size and a promise the host will not keep: OpenJDK 21
// in the Alpine root sized MaxHeapSize at 6442450944 -- a quarter of this Mac's
// 24 GiB MemTotal -- and Node reported totalmem 25769803776. See
// docs/simulated_swap_plan.md section 3.12.
struct mem_budget {
    // false means the host names no ceiling for this process, and total and
    // available are then both 0 and must NOT be printed as figures. That is the
    // honest answer on macOS and Linux with no ISH_GUEST_MEM_BUDGET_MB set (no
    // per-process ceiling exists to report -- measured: task_vm_info's
    // limit_bytes_remaining reads 0 on this Mac), and on iOS until the OS has
    // named a limit at least once. "Unknown" and "no bytes left" are different
    // states and a caller has to be able to tell them apart, so the flag is
    // separate from the values; when it is false, fall back to get_mem_usage()
    // IN FULL, which is what the guest saw before this existed.
    bool known;
    // Bytes this process may use in total, in the host's own accounting: the
    // jetsam limit on iOS, ISH_GUEST_MEM_BUDGET_MB where that is set, the
    // smaller of the two where both exist. Never 0 while `known` is true.
    //
    // CONSTANT while the process runs, which is the whole point: this is the
    // guest's MemTotal, and MemTotal changing between two reads is a state no
    // real Linux produces short of memory hotplug. ISH_GUEST_MEM_BUDGET_MB is
    // constant by construction; a host-named ceiling is latched from the first
    // reading that names it and republished only when the host actually moves
    // it, by more than the 4 MiB of slack MEM_BUDGET_LIMIT_STEP allows
    // for measurement skew (Apple's os/proc.h says memory limits can change
    // during an app's life cycle, so that is a real event and not one to paper
    // over) -- never by the drift of two samples taken at different instants.
    // platform/darwin.c's sample_mem_budget() holds the latch.
    uint64_t total;
    // Whether `available` below was measured. false means the host refused to
    // say and nothing here could derive it -- on Darwin, task_info() failing,
    // which has been seen on iOS 26/27; on Linux, /proc/self/statm unreadable
    // or not yet sampled. It is NOT the same as "no bytes left", and a caller
    // that treats it as one prints a dead machine to a guest with room.
    bool available_known;
    // How many of those bytes are still unspent, always <= total. 0 is a
    // legitimate value and means at or over the budget; available_known ==
    // false means "no information", and `available` is then set equal to
    // `total` so that a caller which ignores the flag reads an idle machine
    // rather than a dead one -- optimism being the safe direction here, since a
    // guest allocator that reads MemAvailable 0 gives up, and a refused reading
    // is not evidence of pressure.
    uint64_t available;
};
// Cheap: sampled on a short timer rather than measured per call, so that a
// guest reading /proc/meminfo in a loop does not pay a host trap a line. The
// answer can therefore be as old as that interval, which is fine for a figure
// printed beside host counters of the same vintage and is NOT fine for deciding
// whether the app is about to be killed. On Darwin, where that decision is a
// real one, host_mem_headroom_low() therefore does not read this sample and
// takes a fresh reading of its own; the measurement that forced that is on the
// Darwin implementation.
struct mem_budget get_mem_budget(void);

// Bytes of the budget that host_mem_headroom_low() refuses to let the guest
// spend, i.e. the amount by which the guest's usable memory is smaller than
// get_mem_budget().total. This is the guest's equivalent of a Linux watermark,
// so a guest-visible MemFree/MemAvailable derived from the budget should have
// it subtracted. 0 when the guard is disabled (ISH_GUEST_MEM_HEADROOM_MB=0).
uint64_t host_mem_headroom_floor(void);

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
// app's own allocations -- UIKit, fakefs SQLite, malloc inside libobjc -- keep
// working instead of crashing on NULL. It reads the same host ledger
// get_mem_budget() does, so on iOS it fires against the jetsam budget as it
// always has, and on macOS or Linux it fires only when ISH_GUEST_MEM_BUDGET_MB
// gives it a budget to fire against -- without that knob those hosts impose no
// per-process ceiling and it returns false, which is why the whole low-memory
// path used to be untestable off a device. Threshold defaults to 192 MiB;
// override with ISH_GUEST_MEM_HEADROOM_MB (0 disables the guard).
//
// IT MUST NEVER BE WEAKER THAN THE PRE-BUDGET GUARD, which read the host's
// remaining-bytes figure directly. In particular a budget of "unknown" does not
// switch it off: the budget needs a ceiling, the ceiling needs a host call that
// answers, and a host call that merely FAILS would otherwise disable the jetsam
// guard for the life of the process. Where the host names no ceiling but does
// still name the bytes remaining, the guard falls back to that raw figure and
// the same floor -- exactly what it ran on before struct mem_budget existed.
//
// It never fires on a budget nobody measured: available_known == false is read
// as "no evidence of pressure", not as "no memory". On iOS it also keeps the
// decision it has always shipped when the OS refuses to name the remaining
// bytes -- deliberately, and narrower than it was; the carve-out and the case
// for removing it are at the bottom of platform/darwin.c.
bool host_mem_headroom_low(void);

// The day-1 host page release probe from docs/simulated_swap_plan.md section 7,
// reached from the guest as /proc/ish/mem_release_probe (fs/proc/ish.c).
//
// It answers one question, and every other piece of the simulated-swap design
// is downstream of it: does releasing a 16 KiB host page actually reduce the
// process footprint this OS decides to kill the app on? Measured byte-exact on
// macOS (section 4.1) and never once observed on a device, which is open risk 1
// in section 8. The implementation is in platform/darwin.c and the long comment
// there records why it decides on phys_footprint rather than
// os_proc_available_memory(), why a 0 return from madvise() is not evidence of
// anything, and why it carries a file-backed control case.
//
// DARWIN ONLY, and deliberately not stubbed elsewhere. The primitives it
// measures (MADV_FREE_REUSABLE/MADV_FREE_REUSE, the phys_footprint ledger,
// task_vm_info.region_count) are XNU's, and a Linux stub answering "not
// supported" from here would only move the same message one file further from
// the reader. fs/proc/ish.c prints that message itself, so nothing outside
// platform/ has to know which hosts have a probe.
#ifdef __APPLE__
// A conservative default, and a cap the guest cannot argue with. Section 7
// sketched a 2 GiB touch; this runs inside the shipping app on someone's iPad,
// where allocating gigabytes is a way to get jetsammed mid-measurement, and
// 16 MiB of dirtied frames measures the same ledger move as 2 GiB would. The
// answer is qualitative -- MOVED or DID NOT MOVE -- and the size only has to be
// large enough to stand out of the noise of an app that is still running a
// guest underneath the probe.
#define HOST_RELEASE_PROBE_DEFAULT_MB 16
#define HOST_RELEASE_PROBE_MAX_MB 256
// Runs the probe at `mb` MiB (0 means the default; anything above the cap is
// clamped to it) and writes a human-readable report into `report`, which is
// always left NUL-terminated and is filled even when the probe refuses to run,
// because the refusal is the thing the operator needs to read. Returns 0 if the
// probe ran, or a negative kernel/errno.h code if it declined to -- _ENOMEM
// when this process is already too close to its ceiling to allocate the region
// safely.
int host_mem_release_probe(unsigned long mb, char *report, size_t report_size);
#endif

#endif
