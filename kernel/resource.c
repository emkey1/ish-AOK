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
    // The stack bound is cached in the address space, because the page-fault
    // path cannot take group->lock (see struct mem's stack_top comment), so a
    // change to RLIMIT_STACK has to be pushed there. gnulib's "working
    // sigaltstack" probe does exactly this -- drops the limit to 1 MB and then
    // overflows on purpose -- so honouring it only at exec would miss the
    // case this exists for.
    //
    // Only for the calling task: reading another task's ->mm here would need
    // general_lock, and prlimit64 against a third party is rare enough that
    // picking the change up at its next exec is the better trade. Its stack
    // stays bounded by the guard gap meanwhile.
    if (resource == RLIMIT_STACK_ && task == current && current->mm != NULL)
        mem_set_stack_bounds(&current->mm->mem, 0,
                             limit.cur == RLIM_INFINITY_ ? 0 : (uint64_t) limit.cur);
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

// The task whose limit is being raised, not the caller: prlimit64 can target
// another process, and the "may not raise your own hard limit" rule is about
// the TARGET's ceiling.
static int check_setrlimit_task(struct task *task, int resource, struct rlimit_ new_limit) {
    if (superuser())
        return 0;
    struct rlimit_ old_limit;
    int err = rlimit_get(task, resource, &old_limit);
    if (err < 0)
        return err;
    if (new_limit.max > old_limit.max)
        return _EPERM;
    return 0;
}

static int check_setrlimit(int resource, struct rlimit_ new_limit) {
    return check_setrlimit_task(current, resource, new_limit);
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
    // Any nonzero pid was EINVAL -- including the caller's OWN pid, which is
    // the form glibc's prlimit(2) wrapper and the read-and-write-in-one-call
    // idiom both use, and what `prlimit --pid N` passes.
    struct task *task = current;
    bool release = false;
    if (pid != 0 && pid != current->pid) {
        // See sched_task_for: pid_get_task_ref locks internally.
        task = pid_get_task_ref(pid);
        if (task == NULL)
            return _ESRCH;
        release = true;
        // Another process's limits are readable and writable only by a
        // matching real uid, or CAP_SYS_RESOURCE.
        if (!current_capable(CAP_SYS_RESOURCE_) && task->uid != current->uid) {
            task_ref_cnt_mod(task, -1);
            return _EPERM;
        }
    }
#define PRLIMIT_RETURN(v) do { \
        int r_ = (v); \
        if (release) task_ref_cnt_mod(task, -1); \
        return r_; \
    } while (0)

    if (old_limit_addr != 0) {
        struct rlimit_ rlimit;
        int err = rlimit_get(task, resource, &rlimit);
        if (err < 0)
            PRLIMIT_RETURN(err);
        STRACE(" old={cur=%#llx, max=%#llx}", (unsigned long long) rlimit.cur, (unsigned long long) rlimit.max);
        if (user_put(old_limit_addr, rlimit))
            PRLIMIT_RETURN(_EFAULT);
    }

    if (new_limit_addr != 0) {
        struct rlimit_ rlimit;
        if (user_get(new_limit_addr, rlimit))
            PRLIMIT_RETURN(_EFAULT);
        STRACE(" new={cur=%#llx, max=%#llx}", (unsigned long long) rlimit.cur, (unsigned long long) rlimit.max);
        int err = check_setrlimit_task(task, resource, rlimit);
        if (err < 0)
            PRLIMIT_RETURN(err);
        PRLIMIT_RETURN(rlimit_set(task, resource, rlimit));
    }
    PRLIMIT_RETURN(0);
#undef PRLIMIT_RETURN
}

// The largest resident page count that could possibly be true: every page of
// host RAM. Cached -- get_mem_usage() is a host call and this is a constant for
// the life of the process, while getrusage is not especially rare.
//
// This is a plausibility bound, not a fix for what produces an implausible
// sample. That is a lock-free page-table walk that can read a leaf array
// another thread is freeing (see the comment in task_maxrss_kb, and
// docs/build_554_musts.md). What the bound does is stop one torn read becoming
// a permanent, latched, user-visible lie.
static size_t maxrss_plausible_pages(void) {
    static size_t cached;
    if (cached == 0) {
        uint64_t total = get_mem_usage().total;        // bytes of host RAM
        cached = total > 0 ? (size_t) (total / PAGE_SIZE) : (size_t) -1;
    }
    return cached;
}

// Peak resident size in KB. Samples the address space's current mapped-page
// count and folds it into the high-water mark kept on the mm, so a later read
// never reports less than an earlier one saw -- which is what "max" means.
size_t task_maxrss_kb(struct task *task) {
    // The address space is PINNED for the walk, and reading task->mm without
    // doing so was a use-after-free that killed the emulator.
    //
    // do_exit does `mm_release(task->mm); task->mm = NULL;` with neither
    // pids_lock nor anything else held, and mm_release's refcount can reach
    // zero and enter mem_destroy -- which frees every page-table chunk and
    // NULLs pgdir_root and pgdir_root_bitmap -- while task->mm still points at
    // the dying mm. A sampler that read the pointer in that window walked a
    // half-destroyed page table.
    //
    // MEASURED: the guest regression suite killed the whole emulator on the
    // devuan-arm64 leg with SIGSEGV at address 0x50, stack
    // mem_next_chunk_root <- mem_page_count_walk <- mem_mapped_page_count <-
    // task_maxrss_kb <- rusage_fill_task_counters <- rusage_get_task <-
    // rusage_get_group_of <- cpu_time_now_of <- itimer_vprof_sampler_notify <-
    // timer_thread. An ITIMER_PROF sampler firing on another thread's task
    // while that task exits.
    //
    // trylock, and skip the fresh sample if it fails, for the reason
    // fs/proc/root.c's collect_mem_page_stats documents: do_exit spins in
    // exit_wait_backoff() while holding general_lock, so blocking here can
    // deadlock. And SKIP THE LOCK ENTIRELY if this thread already holds it,
    // which is do_exit's own call a few lines before it releases the mm --
    // that one is safe by construction and must not be turned into a
    // trylock failure, or the exit-time sample that latches the final peak
    // would be lost.
    // The lock is held ACROSS the walk rather than used to take a reference,
    // and that is deliberate. Retaining the mm and releasing it afterwards
    // would make this thread the last referrer whenever the task exits in
    // between -- and mm_release then runs mem_destroy right here, on the timer
    // thread, which has no `current` and must never tear an address space down
    // (a ->close waiting on a guest process would never be woken). Holding
    // general_lock instead keeps task->mm alive without ever owning a
    // reference to it, because do_exit holds the same lock across
    // `mm_release(task->mm); task->mm = NULL;`.
    bool held_by_us = current == task &&
        pthread_equal(task->general_lock.owner, pthread_self());
    if (!held_by_us && trylock(&task->general_lock) != 0)
        return task->maxrss_kb;     // mid-exit; the latched value is the answer
    struct mm *mm = task->mm;
    if (mm != NULL) {
        size_t pages = mem_mapped_page_count(&mm->mem);
        // That walk is deliberately lock-free (see proc_mem_count_pages): a
        // stale count is fine for /proc. It is NOT fine here, because this is
        // a high-water mark -- a latch. One torn read of a page table another
        // thread is mutating becomes the permanent answer for the life of the
        // address space, and then, because a forked child used to inherit it,
        // for every descendant too.
        //
        // Seen on device: ru_maxrss reporting 2,819,362,696 KB -- 2.7 TB --
        // from a process whose real peak was about 4 MB, reproducibly for
        // every process in one shell's subtree and never in a fresh one.
        //
        // Refuse to latch a sample that cannot be real. The bound has to be
        // PHYSICAL memory, not the address space: a 64-bit guest's page_limit
        // is astronomical, so bounding by it accepts everything and catches
        // nothing -- which is exactly what a first attempt at this did, and it
        // let 2.7 TB through again on the next device run. Resident memory, by
        // definition, cannot exceed the memory that exists.
        if (pages <= maxrss_plausible_pages() && pages > mm->rss_pages_hwm)
            mm->rss_pages_hwm = pages;
        size_t kb = mm->rss_pages_hwm * (PAGE_SIZE / 1024);
        if (kb > task->maxrss_kb)
            task->maxrss_kb = kb;
    }
    if (!held_by_us)
        unlock(&task->general_lock);
    // With the address space already gone (do_exit), or not lockable because
    // the task is mid-exit, the latched value is all there is -- and it is the
    // right answer.
    return task->maxrss_kb;
}

// Everything getrusage reports that is not CPU time. Only utime/stime were
// ever filled, so every other field came back 0 -- and 0 is a value Linux
// cannot produce for a process that has run at all: `time -v` printed a peak
// RSS of 0 KB, Python's resource module reported no faults, and wait4
// supervisors measuring a child's memory saw nothing.
//
// ru_majflt stays 0 on purpose. A major fault means the page had to come from
// storage; AOK's guest memory is host memory throughout, so no fault here ever
// does, and inventing them would be worse than reporting none.
static void rusage_fill_task_counters(struct rusage_ *rusage, struct task *task) {
    if (task == NULL)
        return;
    rusage->minflt = (dword_t) task->minflt;
    rusage->nvcsw = (dword_t) task->nvcsw;
    // Linux counts these in 512-byte blocks, from the same file-backed traffic
    // /proc/<pid>/io reports as read_bytes/write_bytes.
    rusage->inblock = (dword_t) (atomic_load_explicit(&task->io.read_bytes, memory_order_relaxed) / 512);
    rusage->oublock = (dword_t) (atomic_load_explicit(&task->io.write_bytes, memory_order_relaxed) / 512);
    rusage->maxrss = (dword_t) task_maxrss_kb(task);
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
    rusage_fill_task_counters(&rusage, current);
    return rusage;
}


// Usage for a live thread other than the caller. rusage_get_current() can
// only report the *calling* host thread's own usage (getrusage(RUSAGE_THREAD)
// and mach_thread_self() are both self-only) -- summing across a whole thread
// group needs a way to query a different thread's host pthread from here.
struct rusage_ rusage_get_task(struct task *task) {
    if (task == current)
        return rusage_get_current();

    struct rusage_ rusage;
    memset(&rusage, 0, sizeof(rusage));
    // The non-CPU counters live on the task and are readable from here; only
    // the user/system split needs the host thread.
    rusage_fill_task_counters(&rusage, task);
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
    // Everything but the CPU times used to be dropped here, so even once the
    // per-task counters existed a RUSAGE_SELF (which sums the process's
    // threads) or a RUSAGE_CHILDREN total came back at zero again.
    dst->minflt += src->minflt;
    dst->majflt += src->majflt;
    dst->nvcsw += src->nvcsw;
    dst->nivcsw += src->nivcsw;
    dst->inblock += src->inblock;
    dst->oublock += src->oublock;
    dst->nsignals += src->nsignals;
    // maxrss is a peak, not a total: Linux reports the largest any one of them
    // reached, and summing would claim a footprint that never existed.
    if (src->maxrss > dst->maxrss)
        dst->maxrss = src->maxrss;
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
        // An exiting thread is on this list for a long stretch AFTER do_exit
        // has already rolled its final usage into group->rusage above --
        // adding a live sample on top counted the same CPU twice, and the
        // inflated figure collapsed back as soon as exit_tgroup() unlinked it.
        // getrusage(RUSAGE_SELF) and CLOCK_PROCESS_CPUTIME_ID therefore ran
        // BACKWARDS across a pthread_join, by exactly the joined thread's own
        // CPU time (measured: 2 violations in 200 join-then-fork cycles on the
        // devuan-arm64 guest, each drop within 1% of the worker's 60ms). A
        // guest computing a delta from two such reads gets a negative number.
        if (t->exit_rusage_counted)
            continue;
        struct rusage_ live;
        // The mirror image at the other end of a thread's life: a clone is
        // linked into this list by copy_task() well before task_start() gives
        // it a host thread, and until then task->thread still holds the
        // CREATING thread's pthread (task_create_ copies the whole struct).
        // Sampling it added the creator's entire accumulated CPU time a second
        // time, then retracted it once the real thread started -- 141 backward
        // steps in 300 pthread_create/join cycles, each the size of the
        // creating thread's own balance. Same guard get_emulated_per_cpu_usage
        // uses on the /proc/stat side; `current` is exempt because
        // rusage_get_task reads the calling thread directly and never touches
        // task->thread (which is how pid 1, whose host thread predates
        // task_start, still reports its own CPU).
        if (t == current || t->host_thread_started) {
            live = rusage_get_task(t);
        } else {
            // No CPU of its own yet, but the non-CPU counters are readable
            // and are what pid 1 contributes on the sampler-thread path.
            memset(&live, 0, sizeof(live));
            rusage_fill_task_counters(&live, t);
        }
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

// AOK does not act on nice at all -- there is no scheduler here to bias. What
// it must do is REMEMBER it. getpriority returned a flat 0 on the raw syscall,
// and the raw syscall's convention is 20-nice, so libc decoded that as
// niceness 20 -- a value Linux cannot produce (the range is -20..19). `nice`,
// `renice` and every runtime that reads its own niceness back saw a process
// that was somehow below the lowest possible priority, and setpriority
// accepted anything and changed nothing, so `renice 5 $$` reported success and
// the next read still said 20.
#define PRIO_PROCESS_ 0
#define PRIO_PGRP_    1
#define PRIO_USER_    2

static bool prio_target_matches(struct task *task, int_t which, pid_t_ who) {
    switch (which) {
        case PRIO_PROCESS_:
            return who == 0 ? task == current : task->pid == who;
        case PRIO_PGRP_:
            return task->group->pgid == (who == 0 ? current->group->pgid : (pid_t_) who);
        case PRIO_USER_:
            // The REAL uid, as Linux matches it.
            return task->uid == (who == 0 ? current->uid : (uid_t_) who);
    }
    return false;
}

static bool prio_which_valid(int_t which) {
    return which >= PRIO_PROCESS_ && which <= PRIO_USER_;
}

int_t sys_getpriority(int_t which, pid_t_ who) {
    STRACE("getpriority(%d, %d)", which, who);
    if (!prio_which_valid(which))
        return _EINVAL;
    int_t best = 0;
    bool found = false;
    lock(&pids_lock, 0);
    for (dword_t p = 1; p < MAX_PID; p++) {
        struct task *task = pid_get_task(p);
        if (task == NULL || !prio_target_matches(task, which, who))
            continue;
        if (!found || task->nice < best)
            best = task->nice;
        found = true;
    }
    unlock(&pids_lock);
    if (!found)
        return _ESRCH;
    // Linux reports the highest priority (lowest nice) of everything matched,
    // biased by 20 so the raw syscall never returns a negative for a call that
    // succeeded. libc subtracts it back out.
    return 20 - best;
}

int_t sys_setpriority(int_t which, pid_t_ who, int_t prio) {
    STRACE("setpriority(%d, %d, %d)", which, who, prio);
    if (!prio_which_valid(which))
        return _EINVAL;
    // Linux clamps rather than refusing: setpriority(..., 99) succeeds and
    // leaves the process at 19.
    if (prio > 19)
        prio = 19;
    if (prio < -20)
        prio = -20;

    int_t err = 0;
    bool found = false;
    bool privileged = current_capable(CAP_SYS_NICE_);
    lock(&pids_lock, 0);
    for (dword_t p = 1; p < MAX_PID; p++) {
        struct task *task = pid_get_task(p);
        if (task == NULL || !prio_target_matches(task, which, who))
            continue;
        found = true;
        // Somebody else's process is not yours to renice.
        if (!privileged && task->uid != current->uid && task->uid != current->euid) {
            err = _EPERM;
            continue;
        }
        // Raising priority -- lowering the nice value -- needs CAP_SYS_NICE.
        // Linux answers EACCES here, not EPERM.
        if (!privileged && prio < task->nice) {
            err = _EACCES;
            continue;
        }
        task->nice = prio;
    }
    unlock(&pids_lock);
    if (!found)
        return _ESRCH;
    return err;
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
#define SCHED_FIFO_  1
#define SCHED_RR_    2
#define SCHED_BATCH_ 3
#define SCHED_IDLE_  5
#define SCHED_RESET_ON_FORK_ 0x40000000

// Resolve a pid argument the way the sched_* calls take one: 0 means the
// caller. Returns a task with a reference taken (release with
// task_ref_cnt_mod(-1)), or NULL with *err set.
static struct task *sched_task_for(pid_t_ pid, int_t *err) {
    *err = 0;
    if (pid == 0 || pid == current->pid) {
        task_ref_cnt_mod(current, 1);
        return current;
    }
    // NOT under pids_lock: pid_get_task_ref takes it itself, and it is not
    // recursive. (pid_get_task, the non-ref form, is the one that requires the
    // caller to hold it -- the header comment covers both and means the
    // second.)
    struct task *task = pid_get_task_ref(pid);
    if (task == NULL)
        *err = _ESRCH;
    return task;
}

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

int_t sys_sched_getscheduler(pid_t_ pid) {
    STRACE("sched_getscheduler(%d)", pid);
    int_t err;
    struct task *task = sched_task_for(pid, &err);
    if (task == NULL)
        return err;
    // Including the SCHED_RESET_ON_FORK flag, which is what Linux returns.
    int_t policy = task->sched_policy;
    task_ref_cnt_mod(task, -1);
    return policy;
}
int_t sys_sched_setscheduler(pid_t_ pid, int_t policy, addr_t param_addr) {
    return sys_sched_setscheduler_guest(pid, policy, param_addr);
}

// AOK runs every task on a host thread and does not schedule them itself, so a
// policy is recorded rather than acted on. Recording it is still the point:
// SCHED_BATCH and SCHED_IDLE were refused outright with EINVAL, so `chrt -b`,
// `chrt -i`, background indexers demoting themselves and anything setting
// SCHED_RESET_ON_FORK (pipewire clients, the chromium sandbox) got a hard
// failure from a call Linux always accepts from an unprivileged process.
//
// The realtime policies stay refused, with EPERM rather than EINVAL. Nothing
// here can preempt, so accepting SCHED_FIFO would be a promise of latency AOK
// cannot keep -- and EPERM is what an unprivileged process gets on Linux
// anyway, so it is a state callers already handle.
int_t sys_sched_setscheduler_guest(pid_t_ pid, int_t policy, guest_addr_t param_addr) {
    STRACE("sched_setscheduler(%d, %#x)", pid, policy);
    int_t sched_priority;
    if (user_get(param_addr, sched_priority))
        return _EFAULT;

    int_t base = policy & ~SCHED_RESET_ON_FORK_;
    bool realtime;
    switch (base) {
        case SCHED_OTHER_:
        case SCHED_BATCH_:
        case SCHED_IDLE_:
            realtime = false;
            break;
        case SCHED_FIFO_:
        case SCHED_RR_:
            realtime = true;
            break;
        default:
            return _EINVAL;
    }

    // A non-realtime policy takes priority 0 and nothing else; a realtime one
    // takes 1..99. Measured on Linux 6.12: SCHED_BATCH with priority 1 is
    // EINVAL, SCHED_FIFO with priority 0 is EINVAL.
    if (!realtime && sched_priority != 0)
        return _EINVAL;
    if (realtime && (sched_priority < 1 || sched_priority > 99))
        return _EINVAL;
    if (realtime)
        return _EPERM;

    int_t err;
    struct task *task = sched_task_for(pid, &err);
    if (task == NULL)
        return err;
    if (task != current && !current_capable(CAP_SYS_NICE_) &&
            task->uid != current->uid && task->uid != current->euid) {
        task_ref_cnt_mod(task, -1);
        return _EPERM;
    }
    // Leaving SCHED_IDLE for anything else is a priority increase, so it needs
    // CAP_SYS_NICE -- measured: SCHED_IDLE then SCHED_BATCH is EPERM.
    if ((task->sched_policy & ~SCHED_RESET_ON_FORK_) == SCHED_IDLE_ &&
            base != SCHED_IDLE_ && !current_capable(CAP_SYS_NICE_)) {
        task_ref_cnt_mod(task, -1);
        return _EPERM;
    }
    task->sched_policy = policy;
    task_ref_cnt_mod(task, -1);
    return 0;
}

// A constant table on Linux -- it says what the policy's range IS, not what
// this caller may set. Reporting EINVAL for SCHED_FIFO made a runtime that
// asks the range before deciding whether to try conclude the policy does not
// exist, which is a different thing from not being allowed to use it.
int_t sys_sched_get_priority_max(int_t policy) {
    STRACE("sched_get_priority_max(%d)", policy);
    switch (policy) {
        case SCHED_OTHER_: case SCHED_BATCH_: case SCHED_IDLE_: return 0;
        case SCHED_FIFO_: case SCHED_RR_: return 99;
        default: return _EINVAL;
    }
}

int_t sys_sched_get_priority_min(int_t policy) {
    STRACE("sched_get_priority_min(%d)", policy);
    switch (policy) {
        case SCHED_OTHER_: case SCHED_BATCH_: case SCHED_IDLE_: return 0;
        case SCHED_FIFO_: case SCHED_RR_: return 1;
        default: return _EINVAL;
    }
}

int_t sys_ioprio_get(int_t UNUSED(which), int_t UNUSED(who), int_t UNUSED(ioprio)) {
    return 0;
}
int_t sys_ioprio_set(int_t UNUSED(which), int_t UNUSED(who), int_t UNUSED(ioprio)) {
    return 0;
}
