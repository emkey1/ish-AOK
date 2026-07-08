#include <string.h>
#include <sys/stat.h>
#ifdef __APPLE__
#include <mach/mach.h>
#endif
#include "emu/memory.h"
#include "kernel/calls.h"
#include "fs/proc.h"
#include "fs/fd.h"
#include "fs/tty.h"
#include "kernel/fs.h"
#include "kernel/vdso.h"
#include "platform/platform.h"
#include "util/sync.h"

extern pthread_mutex_t extra_lock;
extern bool doEnableExtraLocking;
extern dword_t extra_lock_pid;
extern const char extra_lock_comm;

static void proc_pid_getname(struct proc_entry *entry, char *buf) {
    snprintf(buf, 256, "%d", entry->pid);
}

static char proc_task_state_char_from_snapshot(pid_t_ pid, bool zombie, bool io_block, bool stopped) {
    return (zombie ? 'Z' :
            stopped ? 'T' :
            io_block && pid != current->pid ? 'S' :
            'R');
}

static struct task *proc_get_task(struct proc_entry *entry) {
    return pid_get_task_ref(entry->pid);
}
static void proc_put_task(struct task *task) {
    if (task != NULL)
        task_ref_cnt_mod(task, -1);
}

static struct mm *proc_task_mm_retain(struct task *task) {
    struct mm *mm = NULL;
    lock(&task->general_lock, 0);
    if (task->mm != NULL) {
        mm = task->mm;
        mm_retain(mm);
    }
    unlock(&task->general_lock);
    return mm;
}

static struct fdtable *proc_task_files_retain(struct task *task) {
    struct fdtable *files = NULL;
    lock(&task->general_lock, 0);
    if (task->files != NULL)
        files = fdtable_retain(task->files);
    unlock(&task->general_lock);
    return files;
}

static struct fs_info *proc_task_fs_retain(struct task *task) {
    struct fs_info *fs = NULL;
    lock(&task->general_lock, 0);
    if (task->fs != NULL)
        fs = fs_info_retain(task->fs);
    unlock(&task->general_lock);
    return fs;
}

static int proc_pid_copy_user_range(struct task *task, struct mem *mem, addr_t start,
                                    size_t size, struct proc_data *buf) {
    if (mem == NULL || size == 0)
        return 0;

    char *data = malloc(size);
    if (data == NULL)
        return _ENOMEM;

    if (user_read_task_mem(task, mem, start, data, size) == 0)
        proc_buf_append(buf, data, size);
    free(data);
    return 0;
}

static int proc_pid_copy_cmdline_range(struct task *task, struct mem *mem, addr_t start,
                                       size_t size, addr_t env_start, addr_t env_end,
                                       struct proc_data *buf) {
    if (mem == NULL || size == 0)
        return 0;

    char *data = malloc(size);
    if (data == NULL)
        return _ENOMEM;
    if (user_read_task_mem(task, mem, start, data, size) == 0) {
        char *first_nul = memchr(data, '\0', size);
        if (first_nul != NULL && first_nul > data) {
            size_t title_len = first_nul - data;
            bool has_tail = false;
            for (char *p = first_nul + 1; p < data + size; p++) {
                if (*p != '\0') {
                    has_tail = true;
                    break;
                }
            }
            if (has_tail && memchr(data, ':', title_len) != NULL) {
                proc_buf_append(buf, data, title_len);
                proc_buf_append(buf, "\0", 1);
                free(data);
                return 0;
            }
        }

        size_t visible = size;
        if (data[size - 1] != '\0' && env_end > env_start) {
            visible = strnlen(data, size);
            if (visible == size) {
                size_t env_size = env_end - env_start;
                char *expanded = realloc(data, size + env_size);
                if (expanded == NULL) {
                    free(data);
                    return _ENOMEM;
                }
                data = expanded;
                if (user_read_task_mem(task, mem, env_start, data + size, env_size) == 0)
                    visible = strnlen(data, size + env_size);
                else
                    visible = size;
            }
        }
        proc_buf_append(buf, data, visible);
    }
    free(data);
    return 0;
}

// Get user and system CPU time for a task's thread, in jiffies (USER_HZ = 100).
// Uses the Mach thread_info API so it works from any thread, not just the target.
static void proc_task_cpu_time(struct task *task, unsigned long *out_utime, unsigned long *out_stime) {
#ifdef __APPLE__
    mach_port_t mach_thread = pthread_mach_thread_np(task->thread);
    if (mach_thread != MACH_PORT_NULL) {
        thread_basic_info_data_t info;
        mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
        kern_return_t kr = thread_info(mach_thread, THREAD_BASIC_INFO,
                                       (thread_info_t)&info, &count);
        if (kr == KERN_SUCCESS) {
            *out_utime = (unsigned long)info.user_time.seconds * 100
                         + info.user_time.microseconds / 10000;
            *out_stime = (unsigned long)info.system_time.seconds * 100
                         + info.system_time.microseconds / 10000;
            return;
        }
    }
#endif
    *out_utime = 0;
    *out_stime = 0;
}

// Count the number of mapped pages in a mem.
// Intentionally lock-free: the count is only used for /proc reporting, so a
// slightly stale snapshot is acceptable.
static size_t proc_mem_count_pages(struct mem *mem) {
    return mem_mapped_page_count(mem);
}

static int proc_pid_stat_show(struct proc_entry *entry, struct proc_data *buf) {
    struct task *task = proc_get_task(entry);
    if ((task == NULL) || (task->exiting == true)) {
        proc_put_task(task);
        return _ESRCH;
    }

    // Gather CPU time and memory before the main locks (independent of general_lock)
    unsigned long utime_jiffies, stime_jiffies;
    proc_task_cpu_time(task, &utime_jiffies, &stime_jiffies);

    struct mm *mm = proc_task_mm_retain(task);
    size_t page_count = proc_mem_count_pages(mm ? &mm->mem : NULL);
    pid_t_ pid = 0;
    char comm[sizeof(task->comm) + 1];
    char proc_state = 'R';
    pid_t_ parent_pid = 0;
    pid_t_ pgid = 0;
    pid_t_ sid = 0;
    dword_t tty_dev = 0;
    int tty_fg_group = 0;
    long thread_count = 0;
    addr_t stack_start = 0;
    addr_t start_brk = 0;
    addr_t argv_start = 0;
    addr_t argv_end = 0;
    addr_t env_start = 0;
    addr_t env_end = 0;
    sigset_t_ pending = 0;
    sigset_t_ blocked = 0;
    int exit_signal = 0;
    bool zombie = false;
    bool io_block = false;

    lock(&task->general_lock, 0);
    if (mm != NULL) {
        stack_start = (addr_t)mm->stack_start;
        start_brk = (addr_t)mm->start_brk;
        argv_start = (addr_t)mm->argv_start;
        argv_end = (addr_t)mm->argv_end;
        env_start = (addr_t)mm->env_start;
        env_end = (addr_t)mm->env_end;
    }
    pid = task->pid;
    strncpy(comm, task->comm, sizeof(task->comm));
    comm[sizeof(task->comm)] = '\0';
    unlock(&task->general_lock);

    complex_lockt(&pids_lock, 1);
    lock(&task->group->lock, 0);
    bool stopped = task->group->stopped;
    pgid = task->group->pgid;
    sid = task->group->sid;
    struct tty *tty = task->group->tty;
    if (tty != NULL) {
        lock(&tty->lock, 0);
        tty_dev = dev_make(tty->driver->major, tty->num);
        tty_fg_group = tty->fg_group;
        unlock(&tty->lock);
    } else {
        tty_dev = 0;
        tty_fg_group = 0;
    }
    unlock(&task->group->lock);

    // program reads this using read-like syscall, so we are in blocking area,
    // which means its io_block is set to true. When a proc reads an
    // information about itself, but it shouldn't be marked as blocked.
    parent_pid = task->parent ? task->parent->pid : 0;
    thread_count = list_size(&task->group->threads);
    pending = task->pending;
    blocked = task->blocked;
    exit_signal = task->exit_signal;
    zombie = task->zombie;
    io_block = task->io_block;
    unlock(&pids_lock);
    if (mm != NULL)
        mm_release(mm);

    proc_state = proc_task_state_char_from_snapshot(pid, zombie, io_block, stopped);

    proc_printf(buf, "%d ", pid);
    proc_printf(buf, "(%.16s) ", comm);
    proc_printf(buf, "%c ", proc_state);
    proc_printf(buf, "%d ", parent_pid);
    proc_printf(buf, "%d ", pgid);
    proc_printf(buf, "%d ", sid);
    proc_printf(buf, "%d ", tty_dev);
    proc_printf(buf, "%d ", tty_fg_group);
    proc_printf(buf, "%u ", 0); // flags

    // page faults (no data available)
    proc_printf(buf, "%lu ", 0l); // minor faults
    proc_printf(buf, "%lu ", 0l); // children minor faults
    proc_printf(buf, "%lu ", 0l); // major faults
    proc_printf(buf, "%lu ", 0l); // children major faults

    // values that would be returned from getrusage
    proc_printf(buf, "%lu ", utime_jiffies); // user time
    proc_printf(buf, "%lu ", stime_jiffies); // system time
    proc_printf(buf, "%ld ", 0l); // children user time
    proc_printf(buf, "%ld ", 0l); // children system time

    proc_printf(buf, "%ld ", 20l); // priority (not adjustable)
    proc_printf(buf, "%ld ", 0l); // nice (also not adjustable)
    proc_printf(buf, "%ld ", thread_count);
    proc_printf(buf, "%ld ", 0l); // itimer value (deprecated, always 0)
    proc_printf(buf, "%lld ", 0ll); // jiffies on process start

    proc_printf(buf, "%lu ", (unsigned long)(page_count * PAGE_SIZE)); // vsize in bytes
    proc_printf(buf, "%ld ", (long)page_count); // rss in pages
    proc_printf(buf, "%lu ", (unsigned long)-1); // rss limit (RLIM_INFINITY)

    // bunch of shit that can only be accessed by a debugger
    proc_printf(buf, "%lu ", 0l); // startcode
    proc_printf(buf, "%lu ", 0l); // endcode
    proc_printf(buf, "%lu ", stack_start);
    proc_printf(buf, "%lu ", 0l); // kstkesp
    proc_printf(buf, "%lu ", 0l); // kstkeip

    proc_printf(buf, "%lu ", (unsigned long) pending & 0xffffffff);
    proc_printf(buf, "%lu ", (unsigned long) blocked & 0xffffffff);
    /* uint32_t ignored = 0; // Try enabling this at some point.
    uint32_t caught = 0;
    for (int i = 0; i < 32; i++) {
        if (task->sighand->action[i].handler == SIG_IGN_)
            ignored |= 1l << i;
        else if (task->sighand->action[i].handler != SIG_DFL_)
            caught |= 1l << i;
    }
    proc_printf(buf, "%lu ", (unsigned long) ignored);
    proc_printf(buf, "%lu ", (unsigned long) caught); */
    proc_printf(buf, "%lu ", 0l); // ignored
    proc_printf(buf, "%lu ", 0l); // caught

    proc_printf(buf, "%lu ", 0l); // wchan (wtf)
    proc_printf(buf, "%lu ", 0l); // nswap
    proc_printf(buf, "%lu ", 0l); // cnswap
    proc_printf(buf, "%d ", exit_signal);
    proc_printf(buf, "%d ", 0); // processor

    // htop and similar procfs consumers expect the modern trailing fields too.
    // We don't track most of these yet, but the record still needs to be
    // complete and correctly tokenized.
    proc_printf(buf, "%u ", 0u); // rt_priority
    proc_printf(buf, "%u ", 0u); // policy
    proc_printf(buf, "%llu ", 0ull); // delayacct_blkio_ticks
    proc_printf(buf, "%lu ", 0ul); // guest_time
    proc_printf(buf, "%ld ", 0l); // cguest_time
    proc_printf(buf, "%lu ", 0ul); // start_data
    proc_printf(buf, "%lu ", 0ul); // end_data
    proc_printf(buf, "%lu ", start_brk); // start_brk
    proc_printf(buf, "%lu ", argv_start); // arg_start
    proc_printf(buf, "%lu ", argv_end); // arg_end
    proc_printf(buf, "%lu ", env_start); // env_start
    proc_printf(buf, "%lu ", env_end); // env_end
    proc_printf(buf, "%d", 0); // exit_code
    proc_printf(buf, "\n");

    proc_put_task(task);
    return 0;
}

static int proc_pid_statm_show(struct proc_entry *entry, struct proc_data *buf) {
    struct task *task = proc_get_task(entry);
    if ((task == NULL) || (task->exiting == true)) {
        proc_put_task(task);
        return _ESRCH;
    }
    struct mm *mm = proc_task_mm_retain(task);
    size_t page_count = proc_mem_count_pages(mm ? &mm->mem : NULL);
    if (mm != NULL)
        mm_release(mm);
    proc_put_task(task);
    proc_printf(buf, "%lu ", (unsigned long)page_count); // size (total pages)
    proc_printf(buf, "%lu ", (unsigned long)page_count); // resident (same, no swap)
    proc_printf(buf, "%lu ", 0l); // shared
    proc_printf(buf, "%lu ", 0l); // text
    proc_printf(buf, "%lu ", 0l); // lib (unused since Linux 2.6)
    proc_printf(buf, "%lu ", 0l); // data
    proc_printf(buf, "%lu\n", 0l); // dt (unused since Linux 2.6)
    return 0;
}

static int proc_pid_auxv_show(struct proc_entry *entry, struct proc_data *buf) {
    struct task *task = proc_get_task(entry);
    if ((task == NULL) || (task->exiting == true)) {
        proc_put_task(task);
        return _ESRCH;
    }
    // task is ref-pinned by proc_get_task() above and dropped by proc_put_task() below
    int err = 0;
    struct mm *mm = NULL;
    addr_t start = 0;
    size_t size = 0;
    lock(&task->general_lock, 0);
    if (task->mm == NULL)
        goto out_free_task;

    start = task->mm->auxv_start;
    size = task->mm->auxv_end - start;
    mm = task->mm;
    mm_retain(mm);
    unlock(&task->general_lock);

    err = proc_pid_copy_user_range(task, &mm->mem, start, size, buf);
    mm_release(mm);
    proc_put_task(task);
    return err;

out_free_task:
    unlock(&task->general_lock);
    proc_put_task(task);
    return err;
}

static int proc_pid_cmdline_show(struct proc_entry *entry, struct proc_data *buf) {
    struct task *task = proc_get_task(entry);
    if ((task == NULL) || (task->exiting == true)) {
        proc_put_task(task);
        return _ESRCH;
    }
    
    int err = 0;
    struct mm *mm = NULL;
    addr_t start = 0;
    addr_t env_start = 0;
    addr_t env_end = 0;
    size_t size = 0;
    lock(&task->general_lock, 0);
    
    if (task->mm == NULL)
        goto out_free_task;

    start = task->mm->argv_start;
    size = task->mm->argv_end - start;
    env_start = task->mm->env_start;
    env_end = task->mm->env_end;
    mm = task->mm;
    mm_retain(mm);
    unlock(&task->general_lock);

    err = proc_pid_copy_cmdline_range(task, &mm->mem, start, size, env_start, env_end, buf);
    mm_release(mm);
    proc_put_task(task);
    return err;
    
out_free_task:
    unlock(&task->general_lock);
    proc_put_task(task);
    return err;
}

static int proc_pid_comm_show(struct proc_entry *entry, struct proc_data *buf) {
    struct task *task = proc_get_task(entry);
    if ((task == NULL) || (task->exiting == true)) {
        proc_put_task(task);
        return _ESRCH;
    }

    char name[sizeof(task->comm) + 1];
    lock(&task->general_lock, 0);
    strncpy(name, task->comm, sizeof(task->comm));
    name[sizeof(task->comm)] = '\0';
    unlock(&task->general_lock);

    proc_printf(buf, "%s\n", name);
    proc_put_task(task);
    return 0;
}

static int proc_pid_environ_show(struct proc_entry *entry, struct proc_data *buf) {
    struct task *task = proc_get_task(entry);
    if ((task == NULL) || (task->exiting == true)) {
        proc_put_task(task);
        return _ESRCH;
    }

    int err = 0;
    struct mm *mm = NULL;
    addr_t start = 0;
    size_t size = 0;
    lock(&task->general_lock, 0);
    if (task->mm == NULL)
        goto out_free_task;

    start = task->mm->env_start;
    size = task->mm->env_end - start;
    mm = task->mm;
    mm_retain(mm);
    unlock(&task->general_lock);

    err = proc_pid_copy_user_range(task, &mm->mem, start, size, buf);
    mm_release(mm);
    proc_put_task(task);
    return err;

out_free_task:
    unlock(&task->general_lock);
    proc_put_task(task);
    return err;
}

static int proc_pid_status_show(struct proc_entry *entry, struct proc_data *buf) {
    struct task *task = proc_get_task(entry);
    if ((task == NULL) || (task->exiting == true)) {
        proc_put_task(task);
        return _ESRCH;
    }

    pid_t_ ppid = 0;
    bool zombie = false;
    bool io_block = false;
    sigset_t_ pending = 0;
    sigset_t_ blocked = 0;
    unsigned long thread_count = 0;
    complex_lockt(&pids_lock, 0);
    if (task->parent != NULL)
        ppid = task->parent->pid;
    zombie = task->zombie;
    io_block = task->io_block;
    pending = task->pending;
    blocked = task->blocked;
    thread_count = list_size(&task->group->threads);
    unlock(&pids_lock);

    struct mm *mm = proc_task_mm_retain(task);
    size_t page_count = proc_mem_count_pages(mm ? &mm->mem : NULL);
    if (mm != NULL)
        mm_release(mm);
    struct fdtable *files = proc_task_files_retain(task);
    unsigned fd_size = 0;
    if (files != NULL) {
        lock(&files->lock, 0);
        fd_size = files->size;
        unlock(&files->lock);
        fdtable_release(files);
    }
    unsigned long vm_kb = (unsigned long)(page_count * (PAGE_SIZE / 1024));

    lock(&task->general_lock, 0);
    lock(&task->group->lock, 0);
    bool stopped = task->group->stopped;

    // Cpus_allowed reflects the affinity mask, so use the scheduler-visible
    // count (matches sched_getaffinity), not the full /proc/cpuinfo topology.
    unsigned cpu_count = get_cpu_count_for_affinity();
    unsigned allowed_mask = cpu_count >= 31 ? 0x7fffffffU : ((1U << cpu_count) - 1U);

    proc_printf(buf, "Name:\t%s\n", task->comm);
    proc_printf(buf, "State:\t%c\n", proc_task_state_char_from_snapshot(task->pid, zombie, io_block, stopped));
    proc_printf(buf, "Tgid:\t%d\n", task->tgid);
    proc_printf(buf, "Ngid:\t0\n");
    proc_printf(buf, "Pid:\t%d\n", task->pid);
    proc_printf(buf, "PPid:\t%d\n", ppid);
    proc_printf(buf, "TracerPid:\t0\n");
    proc_printf(buf, "Uid:\t%u\t%u\t%u\t%u\n", task->uid, task->euid, task->suid, task->fsuid);
    proc_printf(buf, "Gid:\t%u\t%u\t%u\t%u\n", task->gid, task->egid, task->sgid, task->fsgid);
    proc_printf(buf, "FDSize:\t%u\n", fd_size);
    proc_printf(buf, "Groups:\t");
    for (unsigned i = 0; i < task->ngroups; i++)
        proc_printf(buf, "%s%u", i == 0 ? "" : " ", task->groups[i]);
    proc_printf(buf, "\n");
    proc_printf(buf, "VmPeak:\t%lu kB\n", vm_kb);
    proc_printf(buf, "VmSize:\t%lu kB\n", vm_kb);
    proc_printf(buf, "VmLck:\t0 kB\n");
    proc_printf(buf, "VmPin:\t0 kB\n");
    proc_printf(buf, "VmHWM:\t%lu kB\n", vm_kb);
    proc_printf(buf, "VmRSS:\t%lu kB\n", vm_kb);
    proc_printf(buf, "Threads:\t%lu\n", thread_count);
    proc_printf(buf, "SigQ:\t0/0\n");
    proc_printf(buf, "SigPnd:\t%08x\n", pending);
    proc_printf(buf, "ShdPnd:\t00000000\n");
    proc_printf(buf, "SigBlk:\t%08x\n", blocked);
    proc_printf(buf, "SigIgn:\t00000000\n");
    proc_printf(buf, "SigCgt:\t00000000\n");
    proc_printf(buf, "CapInh:\t%08x%08x\n", task->cap_inheritable[1], task->cap_inheritable[0]);
    proc_printf(buf, "CapPrm:\t%08x%08x\n", task->cap_permitted[1], task->cap_permitted[0]);
    proc_printf(buf, "CapEff:\t%08x%08x\n", task->cap_effective[1], task->cap_effective[0]);
    proc_printf(buf, "CapBnd:\t%08x%08x\n", task->cap_permitted[1], task->cap_permitted[0]);
    proc_printf(buf, "CapAmb:\t0000000000000000\n");
    proc_printf(buf, "NoNewPrivs:\t0\n");
    proc_printf(buf, "Seccomp:\t0\n");
    proc_printf(buf, "Cpus_allowed:\t%x\n", allowed_mask);
    proc_printf(buf, "Cpus_allowed_list:\t0-%u\n", cpu_count > 0 ? cpu_count - 1 : 0);
    proc_printf(buf, "Mems_allowed:\t1\n");
    proc_printf(buf, "Mems_allowed_list:\t0\n");

    unlock(&task->group->lock);
    unlock(&task->general_lock);
    proc_put_task(task);
    return 0;
}

static int proc_pid_cgroup_show(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    // iSH does not track per-task cgroup membership, so report every task at the
    // root ("/") of each mounted hierarchy. The named v1 hierarchies (e.g.
    // name=elogind) must be listed here or elogind/systemd fail to start with
    // ENODATA ("Cannot determine cgroup we are running in").
    int next_id = 1;
    char seen[512] = "|";
    struct mount *mount;
    list_for_each_entry(&mounts, mount, mounts) {
        if (strcmp(mount->fs->name, "cgroup") != 0)
            continue;
        const char *name = mount->info != NULL ? strstr(mount->info, "name=") : NULL;
        if (name == NULL)
            continue;
        char controller[128];
        size_t n = 0;
        for (const char *p = name; *p != '\0' && *p != ',' && n + 1 < sizeof(controller); p++)
            controller[n++] = *p;
        controller[n] = '\0';
        // Dedupe hierarchies that happen to be mounted more than once.
        char key[130];
        memcpy(key, controller, n);
        key[n] = '|';
        key[n + 1] = '\0';
        if (strstr(seen, key) != NULL)
            continue;
        if (strlen(seen) + n + 1 < sizeof(seen))
            strcat(seen, key);
        proc_printf(buf, "%d:%s:/\n", next_id++, controller);
    }
    proc_printf(buf, "0::/\n");
    return 0;
}

static int proc_pid_sched_show(struct proc_entry *entry, struct proc_data *buf) {
    struct task *task = proc_get_task(entry);
    if ((task == NULL) || (task->exiting == true)) {
        proc_put_task(task);
        return _ESRCH;
    }

    unsigned long thread_count = 0;
    complex_lockt(&pids_lock, 0);
    thread_count = list_size(&task->group->threads);
    unlock(&pids_lock);

    proc_printf(buf, "%s (%d, #threads: %lu)\n", task->comm, task->pid, thread_count);
    proc_printf(buf, "---------------------------------------------------------\n");
    proc_printf(buf, "se.exec_start                                : 0.000000\n");
    proc_printf(buf, "se.vruntime                                  : 0.000000\n");
    proc_printf(buf, "se.sum_exec_runtime                          : 0.000000\n");
    proc_printf(buf, "nr_switches                                  : 0\n");
    proc_printf(buf, "nr_voluntary_switches                        : 0\n");
    proc_printf(buf, "nr_involuntary_switches                      : 0\n");
    proc_put_task(task);
    return 0;
}

void proc_maps_dump(struct task *task, struct proc_data *buf) {
    struct mm *mm = proc_task_mm_retain(task);
    struct mem *mem = mm ? &mm->mem : NULL;
    if (mem == NULL)
        return;

    mem_read_lock_quiesce_aware(mem);
    page_t page = 0;
    while (page < mem->page_limit) {
        // find a region
        while (page < mem->page_limit && mem_pt(mem, page) == NULL) {
            mem_next_page(mem, &page);
        }
        if (page >= mem->page_limit)
            break;
        page_t start = page;
        struct pt_entry *start_pt = mem_pt(mem, start);
        struct data *data = start_pt->data;

        // find the end of said region
        while (page < mem->page_limit) {
            struct pt_entry *pt = mem_pt(mem, page);
            if (pt == NULL)
                break;
            if ((pt->flags & P_RWX) != (start_pt->flags & P_RWX))
                break;
            // region continues if data is the same or both are anonymous
            if (!(pt->data == data || (pt->flags & P_ANONYMOUS && start_pt->flags & P_ANONYMOUS)))
                break;
            mem_next_page(mem, &page);
        }
        page_t end = page;

        // output info
        char path[MAX_PATH] = "";
        if (start_pt->flags & P_GROWSDOWN) {
            static const char s[] = "[stack]";
            memcpy(path, s, sizeof(s));
        } else if (data->name != NULL) {
            strcpy(path, data->name);
        } else if (data->fd != NULL) {
            generic_getpath(start_pt->data->fd, path);
        }
        proc_printf(buf, "%08llx-%08llx %c%c%c%c %08lx 00:00 %-10d %s\n",
                (unsigned long long) (start << PAGE_BITS), (unsigned long long) (end << PAGE_BITS),
                start_pt->flags & P_READ ? 'r' : '-',
                start_pt->flags & P_WRITE ? 'w' : '-',
                start_pt->flags & P_EXEC ? 'x' : '-',
                start_pt->flags & P_SHARED ? '-' : 'p',
                (unsigned long) data->file_offset, // offset
                0, // inode
                path);
    }
    mem_read_unlock_quiesce_aware(mem);
    mm_release(mm);
}

static int proc_pid_maps_show(struct proc_entry *entry, struct proc_data *buf) {
    struct task *task = proc_get_task(entry);
    if ((task == NULL) || (task->exiting == true)) {
        proc_put_task(task);
        return _ESRCH;
    }
    proc_maps_dump(task, buf);
    proc_put_task(task);
    return 0;
}

static ssize_t proc_pid_mem_pread(struct proc_entry *entry, struct proc_data *buf, off_t offset) {
    struct task *task = proc_get_task(entry);
    if (task == NULL)
        return _ESRCH;
    struct mm *mm = proc_task_mm_retain(task);
    if (mm == NULL) {
        proc_put_task(task);
        return _ESRCH;
    }
    int result = user_read_task_mem(task, &mm->mem, (addr_t)offset, buf->data, buf->size);
    mm_release(mm);
    proc_put_task(task);
    return result ? -1 : buf->size;
}

static ssize_t proc_pid_mem_pwrite(struct proc_entry *entry, struct proc_data *buf, off_t offset) {
    struct task *task = proc_get_task(entry);
    if (task == NULL)
        return _ESRCH;
    struct mm *mm = proc_task_mm_retain(task);
    if (mm == NULL) {
        proc_put_task(task);
        return _ESRCH;
    }
    int result = user_write_task_ptrace_mem(task, &mm->mem, (addr_t)offset, buf->data, buf->size);
    mm_release(mm);
    proc_put_task(task);
    return result ? -1 : buf->size;
}


static struct proc_dir_entry proc_pid_fd;
static struct proc_dir_entry proc_pid_fdinfo_entry;

static bool proc_pid_fd_readdir(struct proc_entry *entry, unsigned long *index, struct proc_entry *next_entry) {
    struct task *task = proc_get_task(entry);
    if ((task == NULL) || (task->exiting == true)) {
        proc_put_task(task);
        return false;
    }
    struct fdtable *files = proc_task_files_retain(task);
    if (files == NULL) {
        proc_put_task(task);
        return false;
    }
    lock(&files->lock, 0);
    while (*index < files->size && files->files[*index] == NULL)
        (*index)++;
    fd_t f = (*index)++;
    bool any_left = (unsigned) f < files->size;
    unlock(&files->lock);
    fdtable_release(files);
    proc_put_task(task);
    *next_entry = (struct proc_entry) {&proc_pid_fd, .pid = entry->pid, .fd = f};
    return any_left;
}

static void proc_pid_fd_getname(struct proc_entry *entry, char *buf) {
    snprintf(buf, 256, "%d", entry->fd);
}

static bool proc_pid_fdinfo_readdir(struct proc_entry *entry, unsigned long *index, struct proc_entry *next_entry) {
    struct task *task = proc_get_task(entry);
    if ((task == NULL) || (task->exiting == true)) {
        proc_put_task(task);
        return false;
    }
    struct fdtable *files = proc_task_files_retain(task);
    if (files == NULL) {
        proc_put_task(task);
        return false;
    }
    lock(&files->lock, 0);
    while (*index < files->size && files->files[*index] == NULL)
        (*index)++;
    fd_t f = (*index)++;
    bool any_left = (unsigned) f < files->size;
    unlock(&files->lock);
    fdtable_release(files);
    proc_put_task(task);
    *next_entry = (struct proc_entry) {&proc_pid_fdinfo_entry, .pid = entry->pid, .fd = f};
    return any_left;
}

static int proc_pid_fd_readlink(struct proc_entry *entry, char *buf) {
    struct task *task = proc_get_task(entry);
    if ((task == NULL) || (task->exiting == true)) {
        proc_put_task(task);
        return _ESRCH;
    }
    struct fdtable *files = proc_task_files_retain(task);
    if (files == NULL) {
        proc_put_task(task);
        return _ESRCH;
    }
    lock(&files->lock, 0);
    struct fd *fd = fdtable_get(files, entry->fd);
    if (fd != NULL)
        fd = fd_retain(fd);
    unlock(&files->lock);
    fdtable_release(files);
    int err = fd == NULL ? _ENOENT : generic_getpath(fd, buf);
    if (fd != NULL)
        fd_close(fd);
    proc_put_task(task);
    return err;
}

static int proc_pid_fdinfo_show(struct proc_entry *entry, struct proc_data *buf) {
    struct task *task = proc_get_task(entry);
    if ((task == NULL) || (task->exiting == true)) {
        proc_put_task(task);
        return _ESRCH;
    }
    struct fdtable *files = proc_task_files_retain(task);
    if (files == NULL) {
        proc_put_task(task);
        return _ESRCH;
    }
    lock(&files->lock, 0);
    struct fd *fd = fdtable_get(files, entry->fd);
    if (fd == NULL) {
        unlock(&files->lock);
        fdtable_release(files);
        proc_put_task(task);
        return _ENOENT;
    }
    fd = fd_retain(fd);
    unlock(&files->lock);
    fdtable_release(files);
    proc_printf(buf, "pos:\t%lu\n", fd->offset);
    proc_printf(buf, "flags:\t0%o\n", fd_getflags(fd));
    proc_printf(buf, "mnt_id:\t1\n");
    fd_close(fd);
    proc_put_task(task);
    return 0;
}

static int proc_pid_exe_readlink(struct proc_entry *entry, char *buf) {
    struct task *task = proc_get_task(entry);
    if (task == NULL || task->exiting == true) {
        proc_put_task(task);
        return _ESRCH;
    }
    lock(&task->general_lock, 0);
    struct fd *fd = NULL;
    if (task->mm != NULL && task->mm->exefile != NULL)
        fd = fd_retain(task->mm->exefile);
    unlock(&task->general_lock);
    if (fd == NULL) {
        proc_put_task(task);
        return _ESRCH;
    }
    int err = generic_getpath(fd, buf);
    fd_close(fd);
    proc_put_task(task);
    return err;
}

static void proc_pid_task_getname(struct proc_entry *entry, char *buf) {
    snprintf(buf, 256, "%d", entry->pid);
}

static struct proc_dir_entry proc_pid_task;

static bool proc_pid_task_readdir(struct proc_entry *entry, unsigned long *index, struct proc_entry *next_entry) {
    // TODO: Expose all threads
    *next_entry = (struct proc_entry) {&proc_pid_task, .pid = entry->pid};
    return !(*index)++;
}

static int proc_pid_cwd_readlink(struct proc_entry *entry, char *buf) {
    struct task *task = proc_get_task(entry);
    if (task == NULL || task->exiting == true) {
        proc_put_task(task);
        return _ESRCH;
    }
    struct fs_info *fs = proc_task_fs_retain(task);
    if (fs == NULL) {
        proc_put_task(task);
        return _ESRCH;
    }
    complex_lockt(&fs->lock, 0);
    struct fd *pwd = fs->pwd ? fd_retain(fs->pwd) : NULL;
    unlock(&fs->lock);
    fs_info_release(fs);
    int err = pwd == NULL ? _ESRCH : generic_getpath(pwd, buf);
    if (pwd != NULL)
        fd_close(pwd);
    proc_put_task(task);
    return err;
}

static int proc_pid_root_readlink(struct proc_entry *entry, char *buf) {
    struct task *task = proc_get_task(entry);
    if (task == NULL || task->exiting == true) {
        proc_put_task(task);
        return _ESRCH;
    }
    struct fs_info *fs = proc_task_fs_retain(task);
    if (fs == NULL) {
        proc_put_task(task);
        return _ESRCH;
    }
    complex_lockt(&fs->lock, 0);
    struct fd *root = fs->root ? fd_retain(fs->root) : NULL;
    unlock(&fs->lock);
    fs_info_release(fs);
    int err = root == NULL ? _ESRCH : generic_getpath(root, buf);
    if (root != NULL)
        fd_close(root);
    proc_put_task(task);
    return err;
}

struct proc_children proc_pid_children = PROC_CHILDREN({
    {"auxv", .show = proc_pid_auxv_show},
    {"cgroup", .show = proc_pid_cgroup_show},
    {"cmdline", .show = proc_pid_cmdline_show},
    {"comm", .show = proc_pid_comm_show},
    {"cwd", S_IFLNK, .readlink = proc_pid_cwd_readlink},
    {"environ", .show = proc_pid_environ_show},
    {"exe", S_IFLNK, .readlink = proc_pid_exe_readlink},
    {"fd", S_IFDIR, .readdir = proc_pid_fd_readdir},
    {"fdinfo", S_IFDIR, .readdir = proc_pid_fdinfo_readdir},
    {"maps", .show = proc_pid_maps_show},
    {"mem", .pread = proc_pid_mem_pread, .pwrite = proc_pid_mem_pwrite},
    {"mountinfo", .show = proc_show_mountinfo},
    {"root", S_IFLNK, .readlink = proc_pid_root_readlink},
    {"sched", .show = proc_pid_sched_show},
    {"stat", .show = proc_pid_stat_show},
    {"statm", .show = proc_pid_statm_show},
    {"status", .show = proc_pid_status_show},
    {"task", S_IFDIR, .readdir = proc_pid_task_readdir},
});

struct proc_dir_entry proc_pid = {NULL, S_IFDIR,
    .children = &proc_pid_children, .getname = proc_pid_getname};

static struct proc_dir_entry proc_pid_fd = {NULL, S_IFLNK,
    .getname = proc_pid_fd_getname, .readlink = proc_pid_fd_readlink};

static struct proc_dir_entry proc_pid_fdinfo_entry = {NULL, S_IFREG,
    .getname = proc_pid_fd_getname, .show = proc_pid_fdinfo_show};

static struct proc_dir_entry proc_pid_task = {NULL, S_IFDIR,
    .children = &proc_pid_children, .getname = proc_pid_task_getname};

void proc_pid_init(void) {
    struct proc_dir_entry *fd_dir;
    struct proc_dir_entry *fdinfo_dir;
    struct proc_dir_entry *task_dir;

    proc_set_children_parent(&proc_pid_children, &proc_pid);

    fd_dir = proc_children_find(&proc_pid_children, "fd");
    if (fd_dir != NULL)
        proc_pid_fd.parent = fd_dir;

    fdinfo_dir = proc_children_find(&proc_pid_children, "fdinfo");
    if (fdinfo_dir != NULL)
        proc_pid_fdinfo_entry.parent = fdinfo_dir;

    task_dir = proc_children_find(&proc_pid_children, "task");
    if (task_dir != NULL)
        proc_pid_task.parent = task_dir;
}
