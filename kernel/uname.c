#include <sys/utsname.h>
#include <string.h>
#include "kernel/calls.h"
#include "kernel/uts.h"
#include "kernel/hostinfo.h"
#include "task.h"
#include "platform/platform.h"

static_assert(UTS_NAME_LENGTH == UNAME_LENGTH, "UTS name length must match struct uname");

#if __linux__
#include <sys/sysinfo.h>
#endif

const char *uname_version = "iSH-AOK";

struct uts_namespace init_uts_ns = {
    .lock = LOCK_INITIALIZER,
    .refcount = 1,
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
    // init_uts_ns is static and holds a reference that is never dropped, so
    // it can never reach zero here.
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
    strncpy(uts->release, "5.20.66-ish_aok", sizeof(uts->release));
    strncpy(uts->system, "Linux", sizeof(uts->system));
    snprintf(uts->hostname, sizeof(uts->hostname), "%s", hostname);
    snprintf(uts->version, sizeof(uts->version), "%s %s%s", uname_version, build_version,
            ISH_BUILD_OPT_SUFFIX);
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


#if __APPLE__
static void sysinfo_specific(struct sys_info *info) {
    struct mem_usage usage = get_mem_usage();
    uint64_t total = usage.total != 0 ? usage.total : usage.available;
    uint64_t free = usage.free;
    uint64_t shared = 0;
    uint64_t buffer = usage.cached;
    uint64_t mem_unit = 1;

    while ((total / mem_unit) > 0xffffffffu ||
           (free / mem_unit) > 0xffffffffu ||
           (shared / mem_unit) > 0xffffffffu ||
           (buffer / mem_unit) > 0xffffffffu) {
        mem_unit <<= 1;
    }

    info->totalram = (dword_t)(total / mem_unit);
    info->freeram = (dword_t)(free / mem_unit);
    info->sharedram = (dword_t)(shared / mem_unit);
    info->bufferram = (dword_t)(buffer / mem_unit);
    info->totalswap = 0;
    info->freeswap = 0;
    info->procs = 0;
    info->totalhigh = 0;
    info->freehigh = 0;
    info->mem_unit = (dword_t)mem_unit;
}
static void sysinfo_specific_amd64(struct amd64_sys_info *info) {
    struct mem_usage usage = get_mem_usage();
    // amd64 fields are 64-bit, so report raw byte counts with mem_unit == 1; no
    // need for the 32-bit mem_unit down-scaling loop the i386 path performs.
    info->totalram = usage.total != 0 ? usage.total : usage.available;
    info->freeram = usage.free;
    info->sharedram = 0;
    info->bufferram = usage.cached;
    info->totalswap = 0;
    info->freeswap = 0;
    info->procs = 0;
    info->totalhigh = 0;
    info->freehigh = 0;
    info->mem_unit = 1;
}
#elif __linux__
static void sysinfo_specific(struct sys_info *info) {
    struct sysinfo host_info;
    sysinfo(&host_info);
    info->totalram = host_info.totalram;
    info->freeram = host_info.freeram;
    info->sharedram = host_info.sharedram;
    info->totalswap = host_info.totalswap;
    info->freeswap = host_info.freeswap;
    info->procs = host_info.procs;
    info->totalhigh = host_info.totalhigh;
    info->freehigh = host_info.freehigh;
    info->mem_unit = host_info.mem_unit;
}
static void sysinfo_specific_amd64(struct amd64_sys_info *info) {
    struct sysinfo host_info;
    sysinfo(&host_info);
    // host __kernel_ulong_t fields are 64-bit on a 64-bit Linux host; copy them
    // straight through without the truncation the 32-bit i386 path suffers.
    info->totalram = host_info.totalram;
    info->freeram = host_info.freeram;
    info->sharedram = host_info.sharedram;
    info->bufferram = host_info.bufferram;
    info->totalswap = host_info.totalswap;
    info->freeswap = host_info.freeswap;
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
    struct uptime_info uptime = get_uptime();
    uint64_t loads[3];
    get_guest_loadavg(loads);

    // amd64 glibc expects the 112-byte struct sysinfo with 64-bit fields, so on
    // that ABI fill and write the wider layout with raw (un-truncated) values.
    if (guest_abi_is_64bit(current->abi)) { // arm64 shares the 64-bit layout
        struct amd64_sys_info info = {0};
        info.uptime = (sqword_t)uptime.uptime_ticks;
        info.loads[0] = loads[0];
        info.loads[1] = loads[1];
        info.loads[2] = loads[2];
        sysinfo_specific_amd64(&info);
        if (user_put(info_addr, info))
            return _EFAULT;
        return 0;
    }

    struct sys_info info = {0};
    info.uptime = (dword_t)uptime.uptime_ticks;
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
