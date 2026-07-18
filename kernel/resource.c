#if __linux__
// pull in RUSAGE_THREAD
#define _GNU_SOURCE
#include <sys/resource.h>
#elif __APPLE__
// pull in thread_info and friends
#include <mach/mach.h>
#else
#error
#endif

#include <limits.h>
#include <string.h>
#include <pthread.h>
#include "kernel/calls.h"
#include "platform/platform.h"
#include "util/sync.h"

static bool resource_valid(int resource) {
    return resource >= 0 && resource < RLIMIT_NLIMITS_;
}

static int rlimit_get(struct task *task, int resource, struct rlimit_ *limit) {
    if (!resource_valid(resource))
        return _EINVAL;
    struct tgroup *group = task->group;
    lock(&group->lock, 0);
    *limit = group->limits[resource];
    unlock(&group->lock);
    return 0;
}

static int rlimit_set(struct task *task, int resource, struct rlimit_ limit) {
    if (!resource_valid(resource))
        return _EINVAL;
    struct tgroup *group = task->group;
    lock(&group->lock, 0);
    group->limits[resource] = limit;
    unlock(&group->lock);
    return 0;
}

rlim_t_ rlimit(int resource) {
    struct rlimit_ limit;
    if (rlimit_get(current, resource, &limit) != 0)
        die("invalid resource %d", resource);
    return limit.cur;
}

static int do_getrlimit32(int resource, struct rlimit32_ *rlimit32) {
    STRACE("getlimit(%d)", resource);
    struct rlimit_ rlimit;
    int err = rlimit_get(current, resource, &rlimit);
    if (err < 0)
        return err;
    STRACE(" {cur=%#llx, max=%#llx}", (unsigned long long) rlimit.cur, (unsigned long long) rlimit.max);

    rlimit32->max = rlimit.max;
    rlimit32->cur = rlimit.cur;
    return 0;
}

dword_t sys_getrlimit32(dword_t resource, addr_t rlim_addr) {
    struct rlimit32_ rlimit;
    int err = do_getrlimit32(resource, &rlimit);
    if (err < 0)
        return err;
    if (user_put(rlim_addr, rlimit))
        return _EFAULT;
    return 0;
}

dword_t sys_getrlimit64_guest(dword_t resource, guest_addr_t rlim_addr) {
    struct rlimit_ rlimit;
    int err = rlimit_get(current, resource, &rlimit);
    if (err < 0)
        return err;
    if (user_put(rlim_addr, rlimit))
        return _EFAULT;
    return 0;
}

dword_t sys_getrlimit64(dword_t resource, addr_t rlim_addr) {
    return sys_getrlimit64_guest(resource, rlim_addr);
}

dword_t sys_old_getrlimit32(dword_t resource, addr_t rlim_addr) {
    struct rlimit32_ rlimit;
    int err = do_getrlimit32(resource, &rlimit);
    if (err < 0)
        return err;

    // This version of the call is for programs that aren't aware of rlim_t
    // being 64 bit. RLIM_INFINITY looks like -1 when truncated to 32 bits.
    if (rlimit.cur > INT_MAX)
        rlimit.cur = INT_MAX;
    if (rlimit.max > INT_MAX)
        rlimit.max = INT_MAX;

    if (user_put(rlim_addr, rlimit))
        return _EFAULT;
    return 0;
}

static int check_setrlimit(int resource, struct rlimit_ new_limit) {
    if (superuser())
        return 0;
    struct rlimit_ old_limit;
    int err = rlimit_get(current, resource, &old_limit);
    if (err < 0)
        return err;
    if (new_limit.max > old_limit.max)
        return _EPERM;
    return 0;
}

dword_t sys_setrlimit32(dword_t resource, addr_t rlim_addr) {
    struct rlimit_ rlimit;
    if (user_get(rlim_addr, rlimit))
        return _EFAULT;
    STRACE("setrlimit(%d, {cur=%#llx, max=%#llx})", resource, (unsigned long long) rlimit.cur, (unsigned long long) rlimit.max);
    int err = check_setrlimit(resource, rlimit);
    if (err < 0)
        return err;
    return rlimit_set(current, resource, rlimit);
}

dword_t sys_setrlimit64(dword_t resource, addr_t rlim_addr) {
    return sys_setrlimit64_guest(resource, rlim_addr);
}

dword_t sys_setrlimit64_guest(dword_t resource, guest_addr_t rlim_addr) {
    struct rlimit_ rlimit;
    if (user_get(rlim_addr, rlimit))
        return _EFAULT;
    int err = check_setrlimit(resource, rlimit);
    if (err < 0)
        return err;
    return rlimit_set(current, resource, rlimit);
}

dword_t sys_prlimit64(pid_t_ pid, dword_t resource, addr_t new_limit_addr, addr_t old_limit_addr) {
    return sys_prlimit64_guest(pid, resource, new_limit_addr, old_limit_addr);
}

dword_t sys_prlimit64_guest(pid_t_ pid, dword_t resource, guest_addr_t new_limit_addr, guest_addr_t old_limit_addr) {
    STRACE("prlimit64(%d, %d)", pid, resource);
    if (pid != 0)
        return _EINVAL;

    if (old_limit_addr != 0) {
        struct rlimit_ rlimit;
        int err = rlimit_get(current, resource, &rlimit);
        if (err < 0)
            return err;
        STRACE(" old={cur=%#llx, max=%#llx}", (unsigned long long) rlimit.cur, (unsigned long long) rlimit.max);
        if (user_put(old_limit_addr, rlimit))
            return _EFAULT;
    }

    if (new_limit_addr != 0) {
        struct rlimit_ rlimit;
        if (user_get(new_limit_addr, rlimit))
            return _EFAULT;
        STRACE(" new={cur=%#llx, max=%#llx}", (unsigned long long) rlimit.cur, (unsigned long long) rlimit.max);
        int err = check_setrlimit(resource, rlimit);
        if (err < 0)
            return err;
        return rlimit_set(current, resource, rlimit);
    }
    return 0;
}

struct rusage_ rusage_get_current(void) {
    struct rusage_ rusage;
    memset(&rusage, 0, sizeof(rusage));

#if __linux__
    struct rusage usage;
    if (getrusage(RUSAGE_THREAD, &usage) != 0) {
        // Handle error appropriately, e.g., log an error or set default values
        perror("getrusage failed");
        return rusage;
    }
    rusage.utime.sec = usage.ru_utime.tv_sec;
    rusage.utime.usec = usage.ru_utime.tv_usec;
    rusage.stime.sec = usage.ru_stime.tv_sec;
    rusage.stime.usec = usage.ru_stime.tv_usec;
#elif __APPLE__
    thread_basic_info_data_t info;
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    if (thread_info(mach_thread_self(), THREAD_BASIC_INFO, (thread_info_t) &info, &count) != KERN_SUCCESS) {
        // Handle error appropriately
        printk("ERROR: thread_info failed (rusage_get_current()\n");
        return rusage;
    }
    rusage.utime.sec = info.user_time.seconds;
    rusage.utime.usec = info.user_time.microseconds;
    rusage.stime.sec = info.system_time.seconds;
    rusage.stime.usec = info.system_time.microseconds;
#endif
    return rusage;
}


// Usage for a live thread other than the caller. rusage_get_current() can
// only report the *calling* host thread's own usage (getrusage(RUSAGE_THREAD)
// and mach_thread_self() are both self-only) -- summing across a whole thread
// group needs a way to query a different thread's host pthread from here.
static struct rusage_ rusage_get_task(struct task *task) {
    if (task == current)
        return rusage_get_current();

    struct rusage_ rusage;
    memset(&rusage, 0, sizeof(rusage));
#if __linux__
    // pthread_getcpuclockid's clock only reports combined user+system time --
    // unlike getrusage(RUSAGE_THREAD), there's no Linux API to read another
    // thread's split utime/stime from outside that thread. Attribute the
    // combined figure to utime; this undercounts stime for live sibling
    // threads specifically (the calling thread and already-exited siblings,
    // see the rusage_add call sites in kernel/exit.c, are still split
    // correctly), which is still far closer to correct than ignoring them.
    clockid_t clock_id;
    if (pthread_getcpuclockid(task->thread, &clock_id) != 0)
        return rusage;
    struct timespec ts;
    if (clock_gettime(clock_id, &ts) != 0)
        return rusage;
    rusage.utime.sec = ts.tv_sec;
    rusage.utime.usec = ts.tv_nsec / 1000;
#elif __APPLE__
    // pthread_mach_thread_np converts a pthread_t from any thread in this
    // process into its Mach thread port, so thread_info works cross-thread --
    // Mach ports are valid process-wide, not just for the calling thread.
    thread_basic_info_data_t info;
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    mach_port_t port = pthread_mach_thread_np(task->thread);
    if (thread_info(port, THREAD_BASIC_INFO, (thread_info_t) &info, &count) != KERN_SUCCESS)
        return rusage;
    rusage.utime.sec = info.user_time.seconds;
    rusage.utime.usec = info.user_time.microseconds;
    rusage.stime.sec = info.system_time.seconds;
    rusage.stime.usec = info.system_time.microseconds;
#endif
    return rusage;
}

static void timeval_add(struct timeval_ *dst, struct timeval_ *src) {
    dst->sec += src->sec;
    dst->usec += src->usec;
    if (dst->usec >= 1000000) {
        dst->usec -= 1000000;
        dst->sec++;
    }
}

void rusage_add(struct rusage_ *dst, struct rusage_ *src) {
    timeval_add(&dst->utime, &src->utime);
    timeval_add(&dst->stime, &src->stime);
}

// Process-wide usage: real Linux's getrusage(RUSAGE_SELF) and
// clock_gettime(CLOCK_PROCESS_CPUTIME_ID) both sum every thread in the
// process, not just the caller. group->rusage already accumulates each
// thread's final usage as it exits (see kernel/exit.c); add every
// currently-live thread's usage on top of that baseline. Lock order
// (pids_lock then group->lock) matches kernel/exit.c.
// Takes an explicit group rather than assuming current->group, so it can be
// called from a context with no meaningful `current` (e.g. the setitimer
// VIRTUAL/PROF sampler, which runs on its own bare timer thread).
struct rusage_ rusage_get_group_of(struct tgroup *group) {
    complex_lockt(&pids_lock, 0);
    lock(&group->lock, 0);
    struct rusage_ rusage = group->rusage;
    struct task *t;
    list_for_each_entry(&group->threads, t, group_links) {
        struct rusage_ live = rusage_get_task(t);
        rusage_add(&rusage, &live);
    }
    unlock(&group->lock);
    unlock(&pids_lock);
    return rusage;
}

struct rusage_ rusage_get_group(void) {
    return rusage_get_group_of(current->group);
}

int write_guest_rusage_abi(enum guest_abi abi, guest_addr_t addr, const struct rusage_ *rusage) {
    // Generic 64-bit rusage layout is shared by amd64 and arm64 (callers
    // pass current->abi, so arm64 was getting the i386 32-bit layout).
    if (guest_abi_is_64bit(abi)) {
        struct amd64_rusage_ guest = {
            .utime = {.sec = rusage->utime.sec, .usec = rusage->utime.usec},
            .stime = {.sec = rusage->stime.sec, .usec = rusage->stime.usec},
            .maxrss = rusage->maxrss,
            .ixrss = rusage->ixrss,
            .idrss = rusage->idrss,
            .isrss = rusage->isrss,
            .minflt = rusage->minflt,
            .majflt = rusage->majflt,
            .nswap = rusage->nswap,
            .inblock = rusage->inblock,
            .oublock = rusage->oublock,
            .msgsnd = rusage->msgsnd,
            .msgrcv = rusage->msgrcv,
            .nsignals = rusage->nsignals,
            .nvcsw = rusage->nvcsw,
            .nivcsw = rusage->nivcsw,
        };
        if (user_put(addr, guest))
            return _EFAULT;
    } else {
        if (user_put(addr, *rusage))
            return _EFAULT;
    }
    return 0;
}

dword_t sys_getrusage_guest(dword_t who, guest_addr_t rusage_addr) {
    struct rusage_ rusage;
    switch (who) {
        case RUSAGE_THREAD_:
            // Just the calling host thread's own usage.
            rusage = rusage_get_current();
            break;
        case RUSAGE_SELF_:
            rusage = rusage_get_group();
            break;
        case RUSAGE_CHILDREN_:
            lock(&current->group->lock, 0);
            rusage = current->group->children_rusage;
            unlock(&current->group->lock);
            break;
        default:
            return _EINVAL;
    }
    if (write_guest_rusage_abi(current->abi, rusage_addr, &rusage))
        return _EFAULT;
    return 0;
}

dword_t sys_getrusage(dword_t who, addr_t rusage_addr) {
    return sys_getrusage_guest(who, rusage_addr);
}

int_t sys_sched_getaffinity(pid_t_ pid, dword_t cpusetsize, addr_t cpuset_addr) {
    return sys_sched_getaffinity_guest(pid, cpusetsize, cpuset_addr);
}

int_t sys_sched_getaffinity_guest(pid_t_ pid, dword_t cpusetsize, guest_addr_t cpuset_addr) {
    STRACE("sched_getaffinity(%d, %d, %#x)", pid, cpusetsize, cpuset_addr);

    // Handle pid check separately for clarity
    if (pid != 0) {
        struct task *task = pid_get_task_ref(pid);
        if (task == NULL)
            return _ESRCH;
        task_ref_cnt_mod(task, -1);
    }

    // Report the scheduler-visible CPU count (may reserve host cores for the
    // UI), not the raw host core count or the /proc/cpuinfo topology. The Go
    // runtime sizes GOMAXPROCS from this, and nproc reports it.
    long cpus = get_cpu_count_for_affinity();

    // The real kernel returns a cpumask whose length is a multiple of the
    // guest's sizeof(long): 8 bytes on amd64, 4 on i386. Returning a
    // non-conforming size (the old cpus/8+1, e.g. a single byte) is mishandled
    // by musl's __get_nprocs -- which backs nproc and sysconf(_SC_NPROCESSORS_*)
    // on Alpine -- causing it to report a single CPU. glibc masks this by
    // falling back to /proc, so it only surfaced on the amd64/musl guest. Match
    // the kernel's sizing so the count is correct everywhere.
    size_t unit = guest_abi_is_64bit(current->abi) ? sizeof(uint64_t) : sizeof(uint32_t);
    size_t bytes_for_cpus = (size_t) (cpus + 7) / 8;
    size_t mask_size = ((bytes_for_cpus + unit - 1) / unit) * unit;
    if (mask_size == 0)
        mask_size = unit;
    if (cpusetsize < mask_size)
        return _EINVAL;

    char cpuset[mask_size];
    memset(cpuset, 0, mask_size);

    // Set bits for each CPU
    for (long i = 0; i < cpus; i++)
        bit_set((size_t) i, cpuset);

    // Write to user space, handle error separately
    if (user_write(cpuset_addr, cpuset, mask_size))
        return _EFAULT;

    // Return the number of bytes written
    return (int_t) mask_size;
}

int_t sys_sched_setaffinity(pid_t_ UNUSED(pid), dword_t UNUSED(cpusetsize), addr_t UNUSED(cpuset_addr)) {
    // meh
    return 0;
}

int_t sys_sched_setaffinity_guest(pid_t_ pid, dword_t cpusetsize, guest_addr_t cpuset_addr) {
    return sys_sched_setaffinity(pid, cpusetsize, (addr_t) cpuset_addr);
}

int_t sys_getpriority(int_t which, pid_t_ who) {
    // Since changing process priority is not supported in iOS,
    // this function can return a default priority value.
    // The default nice value in Linux ranges from -20 (highest priority) to 19 (lowest priority).
    STRACE("getpriority(%d, %d)", which, who);
    return 0;
}
int_t sys_setpriority(int_t which, pid_t_ who, int_t prio) {
    STRACE("setpriority(%d, %d, %d)", which, who, prio);
    return 0;
}

// realtime scheduling stubs
int_t sys_sched_getparam(pid_t_ pid, addr_t param_addr) {
    return sys_sched_getparam_guest(pid, param_addr);
}

int_t sys_sched_getparam_guest(pid_t_ pid, guest_addr_t param_addr) {
    if (pid != 0) {
        struct task *task = pid_get_task_ref(pid);
        if (task == NULL)
            return _ESRCH;
        task_ref_cnt_mod(task, -1);
    }
    // iSH only supports SCHED_OTHER (priority 0)
    int_t sched_priority = 0;
    if (user_put(param_addr, sched_priority))
        return _EFAULT;
    return 0;
}
#define SCHED_OTHER_ 0

int_t sys_sched_setparam(pid_t_ pid, addr_t param_addr) {
    return sys_sched_setparam_guest(pid, param_addr);
}

int_t sys_sched_setparam_guest(pid_t_ pid, guest_addr_t param_addr) {
    int_t sched_priority;
    if (user_get(param_addr, sched_priority))
        return _EFAULT;
    // iSH only supports SCHED_OTHER (policy 0), and Linux returns EINVAL for
    // any non-zero priority on SCHED_OTHER tasks.
    if (sched_priority != 0)
        return _EINVAL;
    return 0;
}

int_t sys_sched_rr_get_interval(pid_t_ pid, addr_t tp_addr) {
    return sys_sched_rr_get_interval_guest(pid, tp_addr);
}

int_t sys_sched_rr_get_interval_guest(pid_t_ pid, guest_addr_t tp_addr) {
    if (pid != 0) {
        struct task *task = pid_get_task_ref(pid);
        if (task == NULL)
            return _ESRCH;
        task_ref_cnt_mod(task, -1);
    }
    // Default RR quantum is 100ms (same as Linux SCHED_RR default)
    if (guest_abi_is_64bit(current->abi)) {
        struct timespec64_ tp = { .sec = 0, .nsec = 100000000 };
        if (user_put(tp_addr, tp))
            return _EFAULT;
    } else {
        struct timespec_ tp = { .sec = 0, .nsec = 100000000 };
        if (user_put(tp_addr, tp))
            return _EFAULT;
    }
    return 0;
}

int_t sys_sched_getscheduler(pid_t_ UNUSED(pid)) {
    return SCHED_OTHER_;
}
int_t sys_sched_setscheduler(pid_t_ pid, int_t policy, addr_t param_addr) {
    return sys_sched_setscheduler_guest(pid, policy, param_addr);
}

int_t sys_sched_setscheduler_guest(pid_t_ UNUSED(pid), int_t policy, guest_addr_t param_addr) {
    if (policy != SCHED_OTHER_)
        return _EINVAL;
    int_t sched_priority;
    if (user_get(param_addr, sched_priority))
        return _EFAULT;
    if (sched_priority != 0)
        return _EINVAL;
    return 0;
}

int_t sys_sched_get_priority_max(int_t policy) {
    STRACE("sched_get_priority_max(%d)", policy);
    if (policy == 0)
        return 0;
    return _EINVAL;
}

int_t sys_sched_get_priority_min(int_t policy) {
    STRACE("sched_get_priority_min(%d)", policy);
    if (policy == 0)
        return 0;
    return _EINVAL;
}

int_t sys_ioprio_get(int_t UNUSED(which), int_t UNUSED(who), int_t UNUSED(ioprio)) {
    return 0;
}
int_t sys_ioprio_set(int_t UNUSED(which), int_t UNUSED(who), int_t UNUSED(ioprio)) {
    return 0;
}
