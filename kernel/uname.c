#include <sys/utsname.h>
#include <string.h>
#include "kernel/ipc_ns.h"
#include "kernel/calls.h"
#include "kernel/uts.h"
#include "kernel/hostinfo.h"
#include "task.h"
#include "platform/platform.h"
#include "kernel/swap.h"

static_assert(UTS_NAME_LENGTH == UNAME_LENGTH, "UTS name length must match struct uname");

#if __linux__
#include <sys/sysinfo.h>
#endif

const char *uname_version = "iSH-AOK";

// The kernel release AOK reports: uname -r, /proc/sys/kernel/osrelease and
// /proc/version.
//
// It said 5.20.66, a release that never existed -- 5.19 was followed by 6.0 --
// and one past features AOK did not have, so software that decides by version
// was promised rseq, io_uring and the rest. A real release now, and the one
// whose feature set AOK's matches: everything 5.10 always has is here,
//
//   pidfd_open, pidfd_send_signal, pidfd_getfd, clone3 (5.1-5.6)
//   openat2, faccessat2, close_range (5.6-5.9), process_madvise (5.10)
//   rseq (4.18), POSIX message queues, extended attributes
//
// and CAP_CHECKPOINT_RESTORE (5.9) makes cap_last_cap 40 (kernel/task.h).
// What 5.10 can be built without answers as such a build does: io_uring is
// ENOSYS (CONFIG_IO_URING=n), and so are the namespaces AOK does not model. A
// few later calls exist as well -- epoll_pwait2 (5.11), fchmodat2 (6.6) -- which
// no program is harmed to find, since callers probe for them. The next release
// up would promise mount_setattr (5.12) and futex_waitv (5.16), which are not.
//
// 5.10.0 rather than a later 5.10.y, as Debian's 5.10 kernels spell it; libuv
// turns io_uring on from 5.10.186, and should not go probing for it here.
#define ISH_KERNEL_RELEASE "5.10.0-ish_aok"

struct uts_namespace init_uts_ns = {
    .lock = LOCK_INITIALIZER,
    .refcount = 1,
    // Linux's PROC_UTS_INIT_INO.
    .inode = 4026531838ul,
};

struct uts_namespace *uts_ns_retain(struct uts_namespace *ns) {
    lock(&ns->lock, 0);
    ns->refcount++;
    unlock(&ns->lock);
    return ns;
}

void uts_ns_release(struct uts_namespace *ns) {
    if (ns == NULL)
        return;
    lock(&ns->lock, 0);
    bool dead = --ns->refcount == 0;
    unlock(&ns->lock);
    // init_uts_ns is static and holds a reference that is never dropped, so it
    // can never reach zero here -- provided every task that points at it took a
    // reference. construct_task() used to inherit the pointer without one; see
    // the comment there.
    if (dead)
        free(ns);
}

struct uts_namespace *uts_ns_copy(struct uts_namespace *ns) {
    struct uts_namespace *new_ns = malloc(sizeof(struct uts_namespace));
    if (new_ns == NULL)
        return NULL;
    *new_ns = (struct uts_namespace) {};
    lock_init(&new_ns->lock, "uts_ns\0");
    new_ns->refcount = 1;
    new_ns->inode = ns_alloc_inum();
    lock(&ns->lock, 0);
    memcpy(new_ns->hostname, ns->hostname, sizeof(new_ns->hostname));
    memcpy(new_ns->domainname, ns->domainname, sizeof(new_ns->domainname));
    unlock(&ns->lock);
    return new_ns;
}

struct uts_namespace *uts_ns_current(void) {
    if (current != NULL && current->uts_ns != NULL)
        return current->uts_ns;
    return &init_uts_ns;
}

void uts_set_boot_hostname(const char *hostname) {
    lock(&init_uts_ns.lock, 0);
    if (hostname == NULL)
        init_uts_ns.hostname[0] = '\0';
    else
        snprintf(init_uts_ns.hostname, sizeof(init_uts_ns.hostname), "%s", hostname);
    unlock(&init_uts_ns.lock);
}

bool uts_boot_hostname_is_set(void) {
    lock(&init_uts_ns.lock, 0);
    bool set = init_uts_ns.hostname[0] != '\0';
    unlock(&init_uts_ns.lock);
    return set;
}

void get_current_hostname(char *hostname, size_t size) {
    struct uts_namespace *ns = uts_ns_current();
    lock(&ns->lock, 0);
    bool set = ns->hostname[0] != '\0';
    if (set)
        snprintf(hostname, size, "%s", ns->hostname);
    unlock(&ns->lock);
    if (set)
        return;

    struct utsname real_uname;
    if (uname(&real_uname) < 0) {
        printk("ERROR: uname failed\n");
        snprintf(hostname, size, "%s", "localhost");
        return;
    }
    snprintf(hostname, size, "%s", real_uname.nodename);
}

void do_uname(struct uname *uts) {
    struct utsname real_uname;
    if (uname(&real_uname) < 0) {
        printk("ERROR: uname failed\n");
    }
    char hostname[sizeof(uts->hostname)];
    get_current_hostname(hostname, sizeof(hostname));
    
    // uname -v is a build identifier: on Linux it is what tells you which
    // kernel build you are running. This used to format time(NULL), so it
    // reported the current clock as if it were a build date and moved every
    // time you asked -- useless for the one thing it is for, and actively
    // misleading when working out which build a device has installed.
    char *build_version = copyBuildVersion();

    const char *uname_version = "iSH-AOK"; // Version should be defined or externally managed

    // Fill the uname structure
    const char *machine = "i686";
    if (current != NULL)
        machine = task_abi_desc(current).uname_machine;
    strncpy(uts->arch, machine, sizeof(uts->arch));
    struct uts_namespace *ns = uts_ns_current();
    lock(&ns->lock, 0);
    if (ns->domainname[0] != '\0')
        snprintf(uts->domain, sizeof(uts->domain), "%s", ns->domainname);
    else
        strncpy(uts->domain, "(none)", sizeof(uts->domain));
    unlock(&ns->lock);
    strncpy(uts->release, ISH_KERNEL_RELEASE, sizeof(uts->release));
    strncpy(uts->system, "Linux", sizeof(uts->system));
    snprintf(uts->hostname, sizeof(uts->hostname), "%s", hostname);
    snprintf(uts->version, sizeof(uts->version), "%s %s%s%s", uname_version, build_version,
            ISH_BUILD_OPT_SUFFIX, ISH_BUILD_GRET_SUFFIX);
    free(build_version);
}

dword_t sys_uname(addr_t uts_addr) {
    return sys_uname_guest(uts_addr);
}

dword_t sys_uname_guest(guest_addr_t uts_addr) {
    struct uname uts;
    do_uname(&uts);
    if (user_put(uts_addr, uts))
        return _EFAULT;
    return 0;
}

dword_t sys_sethostname(addr_t hostname_addr, dword_t hostname_len) {
    return sys_sethostname_guest(hostname_addr, hostname_len);
}

dword_t sys_sethostname_guest(guest_addr_t hostname_addr, dword_t hostname_len) {
    struct uname uts;

    if (!superuser()) {
        return _EPERM;
    }

    if (hostname_len >= sizeof(uts.hostname)) {
        return _EINVAL;
    }
    
    char new_hostname[sizeof(uts.hostname)];
    if (user_read(hostname_addr, new_hostname, hostname_len))
        return _EFAULT;
    new_hostname[hostname_len] = '\0'; // Null-terminate the string

    struct uts_namespace *ns = uts_ns_current();
    lock(&ns->lock, 0);
    memcpy(ns->hostname, new_hostname, hostname_len + 1);
    unlock(&ns->lock);

    return 0;
}

dword_t sys_setdomainname(addr_t domainname_addr, dword_t domainname_len) {
    return sys_setdomainname_guest(domainname_addr, domainname_len);
}

dword_t sys_setdomainname_guest(guest_addr_t domainname_addr, dword_t domainname_len) {
    if (!superuser())
        return _EPERM;
    if (domainname_len >= UNAME_LENGTH)
        return _EINVAL;
    char new_domainname[UNAME_LENGTH];
    if (user_read(domainname_addr, new_domainname, domainname_len))
        return _EFAULT;
    new_domainname[domainname_len] = '\0';

    struct uts_namespace *ns = uts_ns_current();
    lock(&ns->lock, 0);
    memcpy(ns->domainname, new_domainname, domainname_len + 1);
    unlock(&ns->lock);
    return 0;
}


// sysinfo(2)'s totalswap/freeswap, in both ABI layouts.
//
// These are AOK's own pager's figures or they are 0. The host's swap never
// reaches the guest in either branch, including the Linux one, which used to
// copy host_info.totalswap/freeswap straight through: that is the same leak
// fs/proc/root.c removed from /proc/meminfo and /proc/vmstat, where XNU's
// whole-machine paging counters were printed beside "SwapTotal: 0 kB" and
// described the Mac rather than the guest. The other fields of the Linux branch
// are still host-derived and still wrong in that way; that is pre-existing and
// wider than this change, and fixing it means giving platform/linux.c a
// guest-derived get_mem_usage the way Darwin's has.
//
// With swap off both fields are 0, which is exactly what the Apple branch
// returned before the pager existed, and swap_get_stats() reads the pager's own
// slot accounting, so it costs no page-table walk in either state.
#if __APPLE__
static void sysinfo_specific(struct sys_info *info) {
    struct mem_usage usage = get_mem_usage();
    struct swap_stats swap;
    swap_get_stats(&swap);
    uint64_t total = usage.total != 0 ? usage.total : usage.available;
    uint64_t free = usage.free;
    uint64_t shared = 0;
    uint64_t buffer = usage.cached;
    uint64_t mem_unit = 1;

    // The i386 layout's fields are 32 bits, so everything is expressed in
    // mem_unit-sized units and the scale has to be chosen from the LARGEST
    // figure in the struct. The swap figures belong in that choice: a swap area
    // bigger than 4 GiB, which the user is free to configure, would otherwise
    // truncate silently while every other field stayed exact -- and truncation
    // here reads as a smaller swap area rather than as an error.
    while ((total / mem_unit) > 0xffffffffu ||
           (free / mem_unit) > 0xffffffffu ||
           (shared / mem_unit) > 0xffffffffu ||
           (buffer / mem_unit) > 0xffffffffu ||
           (swap.total_bytes / mem_unit) > 0xffffffffu ||
           (swap.free_bytes / mem_unit) > 0xffffffffu) {
        mem_unit <<= 1;
    }

    info->totalram = (dword_t)(total / mem_unit);
    info->freeram = (dword_t)(free / mem_unit);
    info->sharedram = (dword_t)(shared / mem_unit);
    info->bufferram = (dword_t)(buffer / mem_unit);
    info->totalswap = (dword_t)(swap.total_bytes / mem_unit);
    info->freeswap = (dword_t)(swap.free_bytes / mem_unit);
    info->procs = 0;
    info->totalhigh = 0;
    info->freehigh = 0;
    info->mem_unit = (dword_t)mem_unit;
}
static void sysinfo_specific_amd64(struct amd64_sys_info *info) {
    struct mem_usage usage = get_mem_usage();
    struct swap_stats swap;
    swap_get_stats(&swap);
    // amd64 fields are 64-bit, so report raw byte counts with mem_unit == 1; no
    // need for the 32-bit mem_unit down-scaling loop the i386 path performs.
    info->totalram = usage.total != 0 ? usage.total : usage.available;
    info->freeram = usage.free;
    info->sharedram = 0;
    info->bufferram = usage.cached;
    info->totalswap = swap.total_bytes;
    info->freeswap = swap.free_bytes;
    info->procs = 0;
    info->totalhigh = 0;
    info->freehigh = 0;
    info->mem_unit = 1;
}
#elif __linux__
static void sysinfo_specific(struct sys_info *info) {
    struct sysinfo host_info;
    sysinfo(&host_info);
    struct swap_stats swap;
    swap_get_stats(&swap);
    info->totalram = host_info.totalram;
    info->freeram = host_info.freeram;
    info->sharedram = host_info.sharedram;
    // Scaled by the host's own mem_unit, since every other field here is: the
    // guest reads all of them through the one divisor this struct carries.
    uint64_t unit = host_info.mem_unit != 0 ? (uint64_t) host_info.mem_unit : 1;
    info->totalswap = (dword_t) (swap.total_bytes / unit);
    info->freeswap = (dword_t) (swap.free_bytes / unit);
    info->procs = host_info.procs;
    info->totalhigh = host_info.totalhigh;
    info->freehigh = host_info.freehigh;
    info->mem_unit = host_info.mem_unit;
}
static void sysinfo_specific_amd64(struct amd64_sys_info *info) {
    struct sysinfo host_info;
    sysinfo(&host_info);
    struct swap_stats swap;
    swap_get_stats(&swap);
    // host __kernel_ulong_t fields are 64-bit on a 64-bit Linux host; copy them
    // straight through without the truncation the 32-bit i386 path suffers.
    info->totalram = host_info.totalram;
    info->freeram = host_info.freeram;
    info->sharedram = host_info.sharedram;
    info->bufferram = host_info.bufferram;
    uint64_t unit = host_info.mem_unit != 0 ? (uint64_t) host_info.mem_unit : 1;
    info->totalswap = swap.total_bytes / unit;
    info->freeswap = swap.free_bytes / unit;
    info->procs = host_info.procs;
    info->totalhigh = host_info.totalhigh;
    info->freehigh = host_info.freehigh;
    info->mem_unit = host_info.mem_unit;
}
#endif

// The amd64 struct sysinfo must come out to exactly 112 bytes from natural C
// alignment alone (procs/pad at 80/82, then a 4-byte gap so totalhigh lands at
// offset 88, freehigh at 96, mem_unit at 104, padded out to 112).
static_assert(sizeof(struct amd64_sys_info) == 112, "amd64 sysinfo layout mismatch");

dword_t sys_sysinfo_guest(guest_addr_t info_addr) {
    // sysinfo(2)'s uptime is in SECONDS. It was uptime_ticks / 100 -- and
    // before that the undivided ticks, which had busybox uptime read a
    // 12-second-old guest as "up 20 min". Linux rounds UP: do_sysinfo reports
    // tv_sec plus one for any fraction, so sysinfo never reads below
    // /proc/uptime. Measured on Linux 6.12: boottime 1647044.474,
    // sysinfo.uptime 1647045. From the nanosecond clock rather than
    // uptime_ticks, because rounding up needs the fraction the ticks drop.
    uint64_t uptime_s = (guest_uptime_ns() + 999999999ull) / 1000000000ull;
    uint64_t loads[3];
    get_guest_loadavg(loads);

    // amd64 glibc expects the 112-byte struct sysinfo with 64-bit fields, so on
    // that ABI fill and write the wider layout with raw (un-truncated) values.
    if (guest_abi_is_64bit(current->abi)) { // arm64 shares the 64-bit layout
        struct amd64_sys_info info = {0};
        info.uptime = (sqword_t) uptime_s;
        info.loads[0] = loads[0];
        info.loads[1] = loads[1];
        info.loads[2] = loads[2];
        sysinfo_specific_amd64(&info);
        if (user_put(info_addr, info))
            return _EFAULT;
        return 0;
    }

    struct sys_info info = {0};
    info.uptime = (dword_t) uptime_s;   // seconds, rounded up, as above
    info.loads[0] = (dword_t)loads[0];
    info.loads[1] = (dword_t)loads[1];
    info.loads[2] = (dword_t)loads[2];
    sysinfo_specific(&info);
    if (user_put(info_addr, info))
        return _EFAULT;
    return 0;
}

dword_t sys_sysinfo(addr_t info_addr) {
    return sys_sysinfo_guest(info_addr);
}
