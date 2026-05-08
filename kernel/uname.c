#include <sys/utsname.h>
#include <string.h>
#include "kernel/calls.h"
#include "task.h"
#include "platform/platform.h"

#if __linux__
#include <sys/sysinfo.h>
#endif

const char *uname_version = "iSH-AOK";
char *uname_hostname_override = NULL;

void get_current_hostname(char *hostname, size_t size) {
    if (uname_hostname_override != NULL && uname_hostname_override[0] != '\0') {
        snprintf(hostname, size, "%s", uname_hostname_override);
        return;
    }

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
    
    // Get current date and format it in a sane way.
    char build_date[100];
    time_t now = time(NULL);
    if (now == (time_t)-1) {
        printk("ERROR: time failed\n");
    }

    struct tm *t = localtime(&now);
    if (t == NULL) {
        printk("ERROR: localtime failed\n");
    }

    if (strftime(build_date, sizeof(build_date), "%Y-%m-%d %H:%M", t) == 0) {
        printk("ERROR: strftime failed\n");
    }

    const char *uname_version = "iSH-AOK"; // Version should be defined or externally managed

    // Fill the uname structure
    const char *machine = "i686";
    if (current != NULL)
        machine = task_abi_desc(current).uname_machine;
    strncpy(uts->arch, machine, sizeof(uts->arch));
    strncpy(uts->domain, "(none)", sizeof(uts->domain));
    strncpy(uts->release, "4.20.69-ish_aok", sizeof(uts->release));
    strncpy(uts->system, "Linux", sizeof(uts->system));
    snprintf(uts->hostname, sizeof(uts->hostname), "%s", hostname);
    snprintf(uts->version, sizeof(uts->version), "%s %s", uname_version, build_date);
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
    
    char *new_hostname = malloc(hostname_len + 1);
    if (new_hostname == NULL) {
        // Handle allocation failure
        return _ENOMEM;
    }
    
    int result = user_read(hostname_addr, new_hostname, hostname_len);
    if (result != 0) {
        free(new_hostname);
        // Handle read failure
        return result;
    }
    new_hostname[hostname_len] = '\0'; // Null-terminate the string

    free(uname_hostname_override);
    uname_hostname_override = new_hostname;

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
#endif

dword_t sys_sysinfo(addr_t info_addr) {
    struct sys_info info = {0};
    struct uptime_info uptime = get_uptime();
    info.uptime = (dword_t)uptime.uptime_ticks;
    info.loads[0] = (dword_t)uptime.load_1m;
    info.loads[1] = (dword_t)uptime.load_5m;
    info.loads[2] = (dword_t)uptime.load_15m;
    sysinfo_specific(&info);

    if (user_put(info_addr, info))
        return _EFAULT;
    return 0;
}

dword_t sys_sysinfo_guest(guest_addr_t info_addr) {
    return sys_sysinfo(info_addr);
}
