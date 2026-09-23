#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <inttypes.h>
#ifdef __APPLE__
#include <mach/mach.h>
#endif
#include "emu/memory.h"
#include "kernel/calls.h"
#include "fs/proc.h"
#include "fs/fd.h"
#include "fs/mmap_cache.h"
#include "fs/tty.h"
#include "kernel/fs.h"
#include "kernel/vdso.h"
#include "platform/platform.h"
#include "kernel/swap.h"
#include "util/sync.h"

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
    // Zombies included. An exited-but-unreaped process still has a /proc entry
    // on Linux -- that is where ps gets the "Z" it shows and where a monitor
    // notices something died and was never reaped. AOK answered ENOENT for the
    // whole directory, so a zombie was invisible to every tool. The handlers
    // below were already written for one (proc_task_state_char_from_snapshot
    // takes a `zombie` flag and returns 'Z'); they simply never received it.
    return pid_get_task_zombie_ref(entry->pid);
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

// Read [start, end) into `page` a page segment at a time, stopping at the first
// segment that cannot be read -- access_remote_vm()'s rule, so a range running
// into unmapped memory yields its readable prefix rather than nothing. With a
// NULL `buf` the bytes go no further than `page` (at most `limit` of them);
// otherwise every segment is appended to `buf`. Returns the bytes read.
static size_t proc_pid_read_prefix(struct task *task, struct mem *mem, guest_addr_t start,
                                   guest_addr_t end, char *page, size_t limit,
                                   struct proc_data *buf) {
    size_t done = 0;
    guest_addr_t at = start;
    while (at < end && (buf != NULL || done < limit)) {
        size_t chunk = PAGE_SIZE - (size_t) (at & (PAGE_SIZE - 1));
        if (chunk > end - at)
            chunk = (size_t) (end - at);
        if (buf == NULL && chunk > limit - done)
            chunk = limit - done;
        char *dest = buf != NULL ? page : page + done;
        if (user_read_task_mem(task, mem, at, dest, chunk) != 0)
            break;
        if (buf != NULL)
            proc_buf_append(buf, page, chunk);
        done += chunk;
        at += chunk;
    }
    return done;
}

// /proc/<pid>/cmdline, by fs/proc/base.c's get_mm_cmdline():
//
//   - Nothing before exec has laid out an environment (env_end == 0), and
//     nothing for an empty or inverted argument range.
//   - [arg_start, arg_end) exactly -- each argument and its one NUL.
//   - Unless the byte at arg_end - 1 is not a NUL: a setproctitle() wrote over
//     the terminator, and the answer is the first string at arg_start with its
//     NUL, one page at most, and never past env_end. That string may run on
//     into the environment, but only when the environment starts where the
//     arguments end; otherwise arg_end is the limit.
//
// All three are measured on Linux 6.12 by tests/manual/proc_cmdline_environ.c,
// which also covers what this replaced: a title containing ':' was cut at its
// first NUL, a rule Linux does not have, and the addresses were held in the
// 32-bit addr_t, so a PR_SET_MM range above 4 GiB (systemd's rename_process
// maps one) read as ENOMEM.
static int proc_pid_copy_cmdline(struct task *task, struct mem *mem, guest_addr_t arg_start,
                                 guest_addr_t arg_end, guest_addr_t env_start,
                                 guest_addr_t env_end, struct proc_data *buf) {
    if (mem == NULL || env_end == 0 || arg_start >= arg_end)
        return 0;
    if (env_start != arg_end || env_end < env_start)
        env_start = env_end = arg_end;

    char *page = malloc(PAGE_SIZE);
    if (page == NULL)
        return _ENOMEM;
    char last;
    if (user_read_task_mem(task, mem, arg_end - 1, &last, 1) == 0 && last != '\0') {
        size_t got = proc_pid_read_prefix(task, mem, arg_start, env_end, page, PAGE_SIZE, NULL);
        size_t len = strnlen(page, got);
        if (len < got)
            len++; // the NUL that ended it
        proc_buf_append(buf, page, len);
    } else {
        proc_pid_read_prefix(task, mem, arg_start, arg_end, page, 0, buf);
    }
    free(page);
    return 0;
}

// /proc/<pid>/environ: [env_start, env_end), by environ_read()'s rules.
static int proc_pid_copy_environ(struct task *task, struct mem *mem, guest_addr_t env_start,
                                 guest_addr_t env_end, struct proc_data *buf) {
    if (mem == NULL || env_end == 0 || env_start >= env_end)
        return 0;
    char *page = malloc(PAGE_SIZE);
    if (page == NULL)
        return _ENOMEM;
    proc_pid_read_prefix(task, mem, env_start, env_end, page, 0, buf);
    free(page);
    return 0;
}

// Count the number of mapped pages in a mem.
// Intentionally lock-free: the count is only used for /proc reporting, so a
// slightly stale snapshot is acceptable.
static size_t proc_mem_count_pages(struct mem *mem) {
    return mem_mapped_page_count(mem);
}

// Of those mapped pages, the ones that are actually in host memory: mapped
// minus what the pager has evicted. This is what VmRSS, statm's second field,
// stat's field 24 and smaps' Rss all report, and they take it from here so that
// they cannot disagree with each other -- Linux derives all four from one
// counter, and section 11 of docs/simulated_swap_plan.md records that bounding
// them independently was tried and rejected four times because it replaces an
// impossible value with an inconsistent pair.
//
// With swap DISABLED this returns the caller's mapped count without walking
// anything. That is not an optimisation for its own sake: no entry can be
// non-resident when nothing evicts, so the value is identical, and skipping the
// walk keeps the cost of /proc/<pid>/status identical too -- top and htop read
// it once per process per refresh, and quietly doubling that walk for a figure
// that cannot have changed would be a real regression for every guest that
// never turns swap on.
//
// It is still not a true residency measure, and the comment on
// mem_resident_page_count in emu/memory.c says exactly why: AOK builds
// page-table entries eagerly at mmap() time and no pt_entry field records
// whether a page was ever touched, so an untouched mapping is counted here in
// full. What it does know is that a SWAPPED page is definitively absent.
static size_t proc_mem_count_resident_pages(struct mem *mem, size_t mapped_pages) {
    if (mem == NULL || !swap_enabled())
        return mapped_pages;
    size_t resident = mem_resident_page_count(mem);
    // The two counts are two separate lock-free walks at two different
    // instants, so a mapping created between them makes resident look larger
    // than mapped. Every caller subtracts one from the other (VmSize - VmRSS is
    // VmSwap), and an unsigned subtraction that wraps would print a process
    // with sixteen exabytes swapped. Clamping can only under-report the swapped
    // figure by whatever was mapped in that window.
    return resident > mapped_pages ? mapped_pages : resident;
}

// A synthetic kernel thread has no task behind it (kernel/task.c explains
// why it exists at all). Only the files a process listing actually reads are
// answered: `ps` wants cmdline and stat, and comm/status are cheap and would
// look broken by their absence. Everything else falls through to the normal
// path and reports ESRCH, which is the truth -- there is no process there.
//
// The empty cmdline is the entire point: that is how a listing decides to
// render a name in brackets, and the bracket is what the container heuristics
// look for.
static int proc_kthread_stat(pid_t_ pid, const char *name, struct proc_data *buf) {
    // Fields in the order /proc/PID/stat has them, with zeros for everything
    // that would be a lie to invent. State R and ppid 0 match Linux's kthreadd.
    proc_printf(buf, "%d (%s) S 0 0 0 0 -1 %u 0 0 0 0 0 0 0 0 20 0 1 0 0 0 0 "
                     "%u 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n",
                pid, name, 0x00200040u /* PF_KTHREAD|PF_NOFREEZE */, (unsigned) -1);
    return 0;
}

static int proc_kthread_status(pid_t_ pid, const char *name, struct proc_data *buf) {
    proc_printf(buf, "Name:\t%s\n", name);
    proc_printf(buf, "State:\tS (sleeping)\n");
    proc_printf(buf, "Tgid:\t%d\n", pid);
    proc_printf(buf, "Pid:\t%d\n", pid);
    proc_printf(buf, "PPid:\t0\n");
    proc_printf(buf, "Uid:\t0\t0\t0\t0\n");
    proc_printf(buf, "Gid:\t0\t0\t0\t0\n");
    proc_printf(buf, "Threads:\t1\n");
    return 0;
}

// The thread directory, /proc/<pid>/task/<tid>/, defined with the task
// listing further down.
static struct proc_dir_entry proc_pid_task;

// Clock ticks (USER_HZ = 100), wide enough that no CPU total wraps.
static unsigned long long proc_ticks_from_timeval(struct timeval_ tv) {
    return (unsigned long long) tv.sec * 100 + tv.usec / 10000;
}

// A zombie is still shown: it is what ps reports as state Z, and the only way
// a monitor learns something exited and was never reaped. Only a task that is
// mid-exit and not yet a zombie is refused, because its fields are still
// changing under us. The files that need a live address space (exe, maps, mem,
// fd) keep the stricter guard -- Linux has nothing to show for those either.
static int proc_pid_stat_show(struct proc_entry *entry, struct proc_data *buf) {
    const char *kname;
    if (pid_is_kthread((dword_t) entry->pid, &kname))
        return proc_kthread_stat(entry->pid, kname, buf);
    struct task *task = proc_get_task(entry);
    if ((task == NULL) || (task->exiting == true && !task->zombie)) {
        proc_put_task(task);
        return _ESRCH;
    }

    // utime and stime, fields 14 and 15, are the WHOLE PROCESS: every live
    // thread plus every thread that has already exited. So they are in
    // /proc/<tid>/stat for any thread of it, as Linux's tgid entries are
    // (do_task_stat with whole=1). Only /proc/<pid>/task/<tid>/stat is one
    // thread. Every view used to report the one thread its id named, so a
    // process whose work ran on a worker thread showed none of it: Thunar's
    // GLib worker spun a whole core while ps, top, htop and btop all put
    // Thunar at 0%.
    //
    // Before the main locks: both helpers take pids_lock or group->lock.
    bool thread_view = entry->parent == &proc_pid_task;
    struct rusage_ cpu = thread_view ? rusage_get_thread_cpu(task)
                                     : rusage_get_group_cpu_of(task->group);
    unsigned long long utime_ticks = proc_ticks_from_timeval(cpu.utime);
    unsigned long long stime_ticks = proc_ticks_from_timeval(cpu.stime);
    // Fields 16 and 17, in both views: the process's waited-for children,
    // what getrusage(RUSAGE_CHILDREN) reports. They were a hardcoded 0.
    unsigned long long cutime_ticks = 0, cstime_ticks = 0;

    struct mm *mm = proc_task_mm_retain(task);
    size_t page_count = proc_mem_count_pages(mm ? &mm->mem : NULL);
    size_t resident_pages = proc_mem_count_resident_pages(mm ? &mm->mem : NULL, page_count);
    pid_t_ pid = 0;
    char comm[sizeof(task->comm) + 1];
    char proc_state = 'R';
    pid_t_ parent_pid = 0;
    pid_t_ pgid = 0;
    pid_t_ sid = 0;
    dword_t tty_dev = 0;
    int tty_fg_group = 0;
    long thread_count = 0;
    guest_addr_t stack_start = 0;
    guest_addr_t start_brk = 0;
    guest_addr_t argv_start = 0;
    guest_addr_t argv_end = 0;
    guest_addr_t env_start = 0;
    guest_addr_t env_end = 0;
    sigset_t_ pending = 0;
    sigset_t_ blocked = 0;
    int exit_signal = 0;
    bool zombie = false;
    bool io_block = false;

    lock(&task->general_lock, 0);
    if (mm != NULL) {
        // All six whole: a 64-bit guest's addresses do not fit addr_t.
        stack_start = mm->stack_start;
        start_brk = mm->start_brk;
        argv_start = mm->argv_start;
        argv_end = mm->argv_end;
        env_start = mm->env_start;
        env_end = mm->env_end;
    }
    uint64_t start_time_ticks = task->start_time_ticks;
    pid = task->pid;
    strncpy(comm, task->comm, sizeof(task->comm));
    comm[sizeof(task->comm)] = '\0';
    unlock(&task->general_lock);

    complex_lockt(&pids_lock, 1);
    lock(&task->group->lock, 0);
    bool stopped = task->group->stopped;
    pgid = task->group->pgid;
    sid = task->group->sid;
    cutime_ticks = proc_ticks_from_timeval(task->group->children_rusage.utime);
    cstime_ticks = proc_ticks_from_timeval(task->group->children_rusage.stime);
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
    // Parent PROCESS, not the forking thread: Linux's task_ppid_nr() is
    // task_tgid_nr(real_parent). See sys_getppid() for why ->pid is wrong here,
    // and task_process_parent for why a thread's own ->parent is.
    struct task *parent = task_process_parent(task);
    parent_pid = parent != NULL ? parent->tgid : 0;
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
    proc_printf(buf, "%llu ", utime_ticks); // user time
    proc_printf(buf, "%llu ", stime_ticks); // system time
    proc_printf(buf, "%llu ", cutime_ticks); // children user time
    proc_printf(buf, "%llu ", cstime_ticks); // children system time

    proc_printf(buf, "%ld ", 20l); // priority (not adjustable)
    proc_printf(buf, "%ld ", 0l); // nice (also not adjustable)
    proc_printf(buf, "%ld ", thread_count);
    proc_printf(buf, "%ld ", 0l); // itimer value (deprecated, always 0)
    proc_printf(buf, "%llu ", (unsigned long long) start_time_ticks); // starttime

    proc_printf(buf, "%lu ", (unsigned long)(page_count * PAGE_SIZE)); // vsize in bytes
    // Field 24, rss: pages in memory, so mapped MINUS what the pager has taken
    // away -- the same figure VmRSS, statm's second field and smaps' Rss report.
    // vsize above stays the whole mapped address space, which is what swapping
    // does not change.
    proc_printf(buf, "%ld ", (long)resident_pages); // rss in pages
    proc_printf(buf, "%lu ", (unsigned long)-1); // rss limit (RLIM_INFINITY)

    // bunch of shit that can only be accessed by a debugger
    proc_printf(buf, "%lu ", 0l); // startcode
    proc_printf(buf, "%lu ", 0l); // endcode
    proc_printf(buf, "%llu ", (unsigned long long) stack_start);
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
    proc_printf(buf, "%llu ", (unsigned long long) start_brk); // start_brk
    proc_printf(buf, "%llu ", (unsigned long long) argv_start); // arg_start
    proc_printf(buf, "%llu ", (unsigned long long) argv_end); // arg_end
    proc_printf(buf, "%llu ", (unsigned long long) env_start); // env_start
    proc_printf(buf, "%llu ", (unsigned long long) env_end); // env_end
    proc_printf(buf, "%d", 0); // exit_code
    proc_printf(buf, "\n");

    proc_put_task(task);
    return 0;
}

static int proc_pid_oom_score_adj_show(struct proc_entry *entry, struct proc_data *buf) {
    struct task *task = proc_get_task(entry);
    if (task == NULL)
        return _ESRCH;
    proc_printf(buf, "%d\n", task->oom_score_adj);
    proc_put_task(task);
    return 0;
}

static int proc_pid_oom_score_adj_update(struct proc_entry *entry, struct proc_data *data) {
    size_t start = 0;
    size_t end = data->size;
    while (start < end && (data->data[start] == ' ' || data->data[start] == '\t' ||
            data->data[start] == '\r' || data->data[start] == '\n'))
        start++;
    while (end > start && (data->data[end - 1] == ' ' || data->data[end - 1] == '\t' ||
            data->data[end - 1] == '\r' || data->data[end - 1] == '\n'))
        end--;
    if (start == end)
        return _EINVAL;

    char buf[16];
    size_t len = end - start;
    if (len >= sizeof(buf))
        return _EINVAL;
    memcpy(buf, data->data + start, len);
    buf[len] = '\0';

    char *endptr;
    long value = strtol(buf, &endptr, 10);
    if (*endptr != '\0' || value < -1000 || value > 1000)
        return _EINVAL;

    struct task *task = proc_get_task(entry);
    if (task == NULL)
        return _ESRCH;
    task->oom_score_adj = (int) value;
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
    size_t resident_pages = proc_mem_count_resident_pages(mm ? &mm->mem : NULL, page_count);
    if (mm != NULL)
        mm_release(mm);
    proc_put_task(task);
    proc_printf(buf, "%lu ", (unsigned long)page_count); // size (total pages)
    // Resident: no longer "the same, no swap" -- mapped minus what the pager
    // has evicted, from the same source as VmRSS and stat's field 24.
    proc_printf(buf, "%lu ", (unsigned long)resident_pages); // resident
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
    // Same credential rule as /proc/<pid>/mem: this is the target's address
    // space by another name. environ in particular carries whatever secrets a
    // process was started with.
    if (!current_may_access_task_mem(task)) {
        proc_put_task(task);
        return _EACCES;
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
    // Empty, exactly as a kernel thread's is.
    if (pid_is_kthread((dword_t) entry->pid, NULL))
        return 0;
    struct task *task = proc_get_task(entry);
    if (task == NULL)
        return _ESRCH;
    // Past the start of exit -- exiting, or a zombie waiting to be reaped --
    // there is no address space to describe, and Linux reads that as empty
    // (get_task_mm() is NULL once exit_mm has run), not as an error. This
    // answered ESRCH for a directory that was still there to be listed.
    if (task->exiting) {
        proc_put_task(task);
        return 0;
    }

    int err = 0;
    struct mm *mm = NULL;
    guest_addr_t arg_start = 0;
    guest_addr_t arg_end = 0;
    guest_addr_t env_start = 0;
    guest_addr_t env_end = 0;
    lock(&task->general_lock, 0);

    if (task->mm != NULL) {
        arg_start = task->mm->argv_start;
        arg_end = task->mm->argv_end;
        env_start = task->mm->env_start;
        env_end = task->mm->env_end;
    }

    // A natively-implemented program (kernel/native.h) has no guest argv region
    // to read: its address space carries no image and no stack, because there
    // was no image to load. Its arguments are kept on the task instead, already
    // in this file's format. Before the mm == NULL bail below, since a task can
    // be running one with no address space left at all.
    if (arg_start == arg_end && task->native_cmdline != NULL && task->native_cmdline_len != 0) {
        size_t len = task->native_cmdline_len;
        char *copy = malloc(len);
        if (copy != NULL)
            memcpy(copy, task->native_cmdline, len);
        unlock(&task->general_lock);
        if (copy == NULL) {
            proc_put_task(task);
            return _ENOMEM;
        }
        proc_buf_append(buf, copy, len);
        free(copy);
        proc_put_task(task);
        return 0;
    }

    if (task->mm == NULL)
        goto out_free_task;

    mm = task->mm;
    mm_retain(mm);
    unlock(&task->general_lock);

    err = proc_pid_copy_cmdline(task, &mm->mem, arg_start, arg_end, env_start, env_end, buf);
    mm_release(mm);
    proc_put_task(task);
    return err;

out_free_task:
    unlock(&task->general_lock);
    proc_put_task(task);
    return err;
}

static int proc_pid_comm_show(struct proc_entry *entry, struct proc_data *buf) {
    const char *kname;
    if (pid_is_kthread((dword_t) entry->pid, &kname)) {
        proc_printf(buf, "%s\n", kname);
        return 0;
    }
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
    // A kernel thread, and (below) a task past exit_mm, have no environment to
    // show: Linux opens the file and reads nothing. Only a reader the file's
    // mode lets in gets this far -- once the mm is gone the file is root's, so
    // anyone else was refused at open, which is also what Linux does.
    if (pid_is_kthread((dword_t) entry->pid, NULL))
        return 0;
    struct task *task = proc_get_task(entry);
    if (task == NULL)
        return _ESRCH;
    if (task->exiting) {
        proc_put_task(task);
        return 0;
    }
    // Same credential rule as /proc/<pid>/mem: this is the target's address
    // space by another name. environ in particular carries whatever secrets a
    // process was started with.
    if (!current_may_access_task_mem(task)) {
        proc_put_task(task);
        return _EACCES;
    }

    int err = 0;
    struct mm *mm = NULL;
    guest_addr_t env_start = 0;
    guest_addr_t env_end = 0;
    lock(&task->general_lock, 0);
    if (task->mm == NULL)
        goto out_free_task;

    env_start = task->mm->env_start;
    env_end = task->mm->env_end;
    mm = task->mm;
    mm_retain(mm);
    unlock(&task->general_lock);

    err = proc_pid_copy_environ(task, &mm->mem, env_start, env_end, buf);
    mm_release(mm);
    proc_put_task(task);
    return err;

out_free_task:
    unlock(&task->general_lock);
    proc_put_task(task);
    return err;
}

static int proc_pid_status_show(struct proc_entry *entry, struct proc_data *buf) {
    const char *kname;
    if (pid_is_kthread((dword_t) entry->pid, &kname))
        return proc_kthread_status(entry->pid, kname, buf);
    struct task *task = proc_get_task(entry);
    if ((task == NULL) || (task->exiting == true && !task->zombie)) {
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
    // PPid is the parent PROCESS (task_tgid_nr(real_parent)), see sys_getppid()
    // and task_process_parent.
    struct task *parent = task_process_parent(task);
    if (parent != NULL)
        ppid = parent->tgid;
    zombie = task->zombie;
    io_block = task->io_block;
    pending = task->pending;
    blocked = task->blocked;
    thread_count = list_size(&task->group->threads);
    unlock(&pids_lock);

    struct mm *mm = proc_task_mm_retain(task);
    size_t page_count = proc_mem_count_pages(mm ? &mm->mem : NULL);
    size_t resident_pages = proc_mem_count_resident_pages(mm ? &mm->mem : NULL, page_count);
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
    unsigned long rss_kb = (unsigned long)(resident_pages * (PAGE_SIZE / 1024));
    unsigned long swap_kb = vm_kb - rss_kb;

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
    // VmHWM is a peak, so it stays the whole mapped figure: it is by
    // construction >= VmRSS, and lowering it to the current resident count
    // would be reporting a high-water mark that goes DOWN. It is still not a
    // real peak -- there is no rss high-water mark on struct mm that this layer
    // can read -- which is unchanged by swap and pre-existing.
    proc_printf(buf, "VmHWM:\t%lu kB\n", vm_kb);
    proc_printf(buf, "VmRSS:\t%lu kB\n", rss_kb);
    // VmSwap appears only when swap is on, because with it off this file has to
    // be byte-for-byte what it was before the pager existed and there was no
    // VmSwap line here at all. Linux prints it unconditionally; the comment on
    // swap_enabled() in kernel/swap.h says why the disabled column wins that
    // argument. Its value is exactly VmSize - VmRSS, so the three lines close.
    if (swap_enabled())
        proc_printf(buf, "VmSwap:\t%lu kB\n", swap_kb);
    proc_printf(buf, "Threads:\t%lu\n", thread_count);
    // Linux prints Umask between Name and State; it was missing entirely, and
    // it is the only place a process's umask is observable from outside.
    // The umask lives on the (possibly shared) fs_info, not the task.
    mode_t_ umask = 0;
    if (task->fs != NULL) {
        lock(&task->fs->lock, 0);
        umask = task->fs->umask;
        unlock(&task->fs->lock);
    }
    proc_printf(buf, "Umask:\t%04o\n", umask & 07777);
    proc_printf(buf, "SigQ:\t0/0\n");
    // Linux's render_sigset_t() prints the whole 64-bit sigset, all 16 hex
    // digits of it. sigset_t_ is 64 bits here too, so %08x both under-read the
    // argument and told consumers the mask was half the width it is.
    // SigIgn and SigCgt were hardcoded zero before, which told every reader the
    // process ignored nothing and caught nothing -- the two fields a debugger
    // or a supervisor reads to find out which signals will actually reach it.
    // Built from the dispositions, the way Linux's collect_sigign_sigcatch
    // does. ShdPnd was zero too: what is pending on the process rather than on
    // one thread, which is where kill() leaves a signal every thread blocks.
    sigset_t_ ignored = 0, caught = 0, shared_pending = 0;
    if (task->sighand != NULL) {
        lock(&task->sighand->lock, 0);
        for (int sig = 1; sig < NUM_SIGS; sig++) {
            guest_addr_t handler = task->sighand->action[sig].handler;
            if (handler == SIG_IGN_)
                ignored |= (sigset_t_) 1 << (sig - 1);
            else if (handler != SIG_DFL_)
                caught |= (sigset_t_) 1 << (sig - 1);
        }
        shared_pending = task->sighand->pending;
        unlock(&task->sighand->lock);
    }
    proc_printf(buf, "SigPnd:\t%016llx\n", (unsigned long long)pending);
    proc_printf(buf, "ShdPnd:\t%016llx\n", (unsigned long long) shared_pending);
    proc_printf(buf, "SigBlk:\t%016llx\n", (unsigned long long)blocked);
    proc_printf(buf, "SigIgn:\t%016llx\n", (unsigned long long) ignored);
    proc_printf(buf, "SigCgt:\t%016llx\n", (unsigned long long) caught);
    proc_printf(buf, "CapInh:\t%08x%08x\n", task->cap_inheritable[1], task->cap_inheritable[0]);
    proc_printf(buf, "CapPrm:\t%08x%08x\n", task->cap_permitted[1], task->cap_permitted[0]);
    proc_printf(buf, "CapEff:\t%08x%08x\n", task->cap_effective[1], task->cap_effective[0]);
    proc_printf(buf, "CapBnd:\t%08x%08x\n", task->cap_permitted[1], task->cap_permitted[0]);
    proc_printf(buf, "CapAmb:\t0000000000000000\n");
    proc_printf(buf, "NoNewPrivs:\t%d\n", task->no_new_privs ? 1 : 0);
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

static int proc_pid_io_show(struct proc_entry *entry, struct proc_data *buf) {
    struct task *task = proc_get_task(entry);
    if (task == NULL)
        return _ESRCH;

    // For a thread-group leader report the whole process (live threads plus
    // the io_dead rollup of exited ones), matching Linux's /proc/<tgid>/io;
    // for any other task just that thread's counters.
    struct task_io_counters io = {};
    complex_lockt(&pids_lock, 1);
    if (task_is_leader(task)) {
        task_io_counters_add(&io, &task->group->io_dead);
        struct task *thread;
        list_for_each_entry(&task->group->threads, thread, group_links)
            task_io_counters_add(&io, &thread->io);
    } else {
        task_io_counters_add(&io, &task->io);
    }
    unlock(&pids_lock);
    proc_put_task(task);

    proc_printf(buf, "rchar: %llu\n", (unsigned long long) io.rchar);
    proc_printf(buf, "wchar: %llu\n", (unsigned long long) io.wchar);
    proc_printf(buf, "syscr: %llu\n", (unsigned long long) io.syscr);
    proc_printf(buf, "syscw: %llu\n", (unsigned long long) io.syscw);
    proc_printf(buf, "read_bytes: %llu\n", (unsigned long long) io.read_bytes);
    proc_printf(buf, "write_bytes: %llu\n", (unsigned long long) io.write_bytes);
    proc_printf(buf, "cancelled_write_bytes: %llu\n", (unsigned long long) io.cancelled_write_bytes);
    return 0;
}

static int proc_pid_cgroup_show(struct proc_entry *entry, struct proc_data *buf) {
    // v1 hierarchies are untracked, so every task reports at their root
    // ("/"). The named v1 hierarchies (e.g. name=elogind) must be listed
    // here or elogind/systemd fail to start with ENODATA ("Cannot determine
    // cgroup we are running in"). The v2 line reports the membership
    // recorded when the pid was written to a cgroup.procs file on the fake
    // cgroup2 hierarchy (fs/tmp.c) -- systemd --user derives its delegated
    // subtree from this line, and the previously hardcoded "0::/" made it
    // try to create its init.scope at the hierarchy root (EACCES for a
    // non-root manager, "Failed to allocate manager object").
    int next_id = 1;
    char seen[512] = "|";
    size_t seen_len = 1; // Length of seen
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
        if (seen_len + n + 1 < sizeof(seen)) {
            memcpy(seen + seen_len, key, n + 2);
            seen_len += n + 1;
        }
        proc_printf(buf, "%d:%s:/\n", next_id++, controller);
    }
    char v2_path[MAX_PATH] = "/";
    struct task *task = proc_get_task(entry);
    if (task != NULL) {
        if (task->group != NULL) {
            lock(&task->group->lock, 0);
            if (task->group->cgroup_path != NULL) {
                strncpy(v2_path, task->group->cgroup_path, sizeof(v2_path) - 1);
                v2_path[sizeof(v2_path) - 1] = '\0';
            }
            unlock(&task->group->lock);
        }
        proc_put_task(task);
    }
    proc_printf(buf, "0::%s\n", v2_path);
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

// Lazy reservations have no page-table entries, so a walk of the page table
// cannot see them -- but the guest has them mapped, and readers of maps and
// smaps (the JVM sizes itself from maps; sanitizers and debuggers parse both)
// must be told. Collects a copy sorted by address into `pending`
// (MEM_LAZY_MAX long) for the walk to merge in: both files are in address
// order and parsers rely on that. Caller holds the mem lock.
static unsigned collect_pending_reservations(struct mem *mem, struct mem_lazy_map *pending) {
    unsigned pending_n = 0;
    for (unsigned i = 0; i < mem->lazy_count; i++)
        if (mem->lazy[i].start < mem->lazy[i].end)
            pending[pending_n++] = mem->lazy[i];
    for (unsigned i = 1; i < pending_n; i++)
        for (unsigned j = i; j > 0 && pending[j - 1].start > pending[j].start; j--) {
            struct mem_lazy_map t = pending[j - 1];
            pending[j - 1] = pending[j]; pending[j] = t;
        }
    return pending_n;
}

// Emit any lazy reservation starting before `limit`, keeping proc_maps_dump's
// output in address order.
static void emit_pending_maps(struct proc_data *buf, struct mem_lazy_map *pending,
                              unsigned pending_n, unsigned *pending_i, page_t limit) {
    while (*pending_i < pending_n && pending[*pending_i].start < limit) {
        struct mem_lazy_map *l = &pending[(*pending_i)++];
        proc_printf(buf, "%08llx-%08llx %c%c%c%c 00000000 00:00 %-10d \n",
                (unsigned long long) (l->start << PAGE_BITS),
                (unsigned long long) (l->end << PAGE_BITS),
                l->flags & P_READ ? 'r' : '-',
                l->flags & P_WRITE ? 'w' : '-',
                l->flags & P_EXEC ? 'x' : '-',
                l->flags & P_SHARED ? '-' : 'p', 0);
    }
}

void proc_maps_dump(struct task *task, struct proc_data *buf) {
    struct mm *mm = proc_task_mm_retain(task);
    struct mem *mem = mm ? &mm->mem : NULL;
    if (mem == NULL)
        return;

    mem_read_lock_quiesce_aware(mem);

    // See collect_pending_reservations.
    struct mem_lazy_map pending[MEM_LAZY_MAX];
    unsigned pending_n = collect_pending_reservations(mem, pending);
    unsigned pending_i = 0;

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
            page_t prev = page;
            mem_next_page(mem, &page);
            // mem_next_page is a SPARSE walk: from the last page of a leaf it
            // jumps over every unallocated leaf that follows, landing on the
            // next mapped page or on page_limit. That is what the hole-skipping
            // loop above wants and the exact opposite of what this one does --
            // the jump clears the pt == NULL test above, the both-anonymous
            // test passes, and the skipped hole disappears into the middle of a
            // region that claims to cover it.
            //
            // A native shell with one mapped megabyte at the top of its address
            // space reported a single 33 MB anonymous region ending at the
            // address ceiling: 256 live pages and 8192 holes, printed as one
            // extent. VmSize, which counts page-table entries rather than
            // walking regions, said 1024 kB alongside it. Parsers of this file
            // size themselves from these extents.
            //
            // Rewind to the page after the last mapped one: that is where the
            // region really ends, and where the outer loop should resume.
            if (page != prev + 1) {
                page = prev + 1;
                break;
            }
        }
        page_t end = page;

        emit_pending_maps(buf, pending, pending_n, &pending_i, start);

        // output info
        char path[MAX_PATH] = "";
        if (start_pt->flags & P_GROWSDOWN) {
            static const char s[] = "[stack]";
            memcpy(path, s, sizeof(s));
        } else if (data->name != NULL) {
            strncpy(path, data->name, sizeof(path) - 1);
            path[sizeof(path) - 1] = '\0';
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
    emit_pending_maps(buf, pending, pending_n, &pending_i, mem->page_limit);
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

// smaps-style per-region accounting. iSH has no swap and no dirty-bit
// tracking, so every guest-mapped page is counted as resident (Rss == Size)
// and anonymous/writable pages are treated as dirty, matching the common
// case (freshly-touched heap/stack) even though it can't observe an actual
// clean anonymous page. Pss approximates true proportional accounting by
// dividing a region's Rss by an estimated sharer count -- there is no
// reverse index from a data block to the tasks mapping it, so this is an
// estimate, not an exact cross-process PSS.
//
// The sharer count combines two independent signals, neither of which is
// struct data's raw refcount (that counts one increment per *page* in the
// region -- see pt_map -- so for an unshared N-page region it equals N, not
// 1; dividing by it directly would make Pss collapse toward a single page
// for every ordinary private mapping):
//   - intra-lineage sharing (fork): refcount / region_pages. This is exact
//     for never-written regions (the only ones eligible for the
//     cross-process registry below) since they can't fragment via COW, and
//     is a reasonable estimate otherwise.
//   - cross-process sharing: mmap_cache_count() for mappings registered in
//     fs/mmap_cache.c (never-writable file-backed mappings, where the host
//     OS's own page cache already backs independent mmap() calls onto the
//     same file+offset with the same physical pages -- this registry only
//     tracks that pre-existing sharing for reporting purposes).
// The two are combined with max() rather than summed: summing would need
// each cache entry to track its members' own intra-lineage counts to avoid
// double-counting, which isn't worth the complexity for a best-effort
// estimate.
struct smaps_totals {
    uint64_t rss_kb, pss_kb;
    uint64_t shared_clean_kb, shared_dirty_kb;
    uint64_t private_clean_kb, private_dirty_kb;
    uint64_t anonymous_kb;
    uint64_t swap_kb;
};

static void proc_smaps_region(struct proc_data *buf, page_t start, page_t end,
                               struct pt_entry *start_pt, struct data *data,
                               const char *path, bool print_header,
                               uint64_t swapped_pages,
                               struct smaps_totals *totals) {
    uint64_t region_pages = (uint64_t)(end - start);
    uint64_t size_kb = region_pages * (PAGE_SIZE / 1024);
    // Rss = Size - Swap, which is section 3.12's rule for this row and the same
    // arithmetic /proc/<pid>/status uses for VmRSS. Everything derived from
    // rss_kb below -- Pss, the Shared/Private split, Referenced, Anonymous --
    // follows it, so a region with pages out does not report them twice.
    // swapped_pages is 0 whenever swap is off, so this is size_kb and every
    // figure in this block is exactly what it was.
    uint64_t swap_kb = swapped_pages * (PAGE_SIZE / 1024);
    if (swap_kb > size_kb)
        swap_kb = size_kb;
    uint64_t rss_kb = size_kb - swap_kb;
    unsigned refcount = data != NULL ?
        atomic_load_explicit(&data->refcount, memory_order_relaxed) : 1;
    if (refcount == 0)
        refcount = 1;
    uint64_t intra_lineage_sharers = region_pages != 0 ? refcount / region_pages : refcount;
    if (intra_lineage_sharers == 0)
        intra_lineage_sharers = 1;
    unsigned cross_process_sharers = mmap_cache_count(data != NULL ? data->cache_entry : NULL);
    uint64_t sharers = intra_lineage_sharers > cross_process_sharers ?
        intra_lineage_sharers : cross_process_sharers;
    bool shared = (start_pt->flags & P_SHARED) || sharers > 1;
    bool anon = (start_pt->flags & P_ANONYMOUS) != 0;
    uint64_t pss_kb = shared ? rss_kb / sharers : rss_kb;

    uint64_t shared_clean_kb = 0, shared_dirty_kb = 0;
    uint64_t private_clean_kb = 0, private_dirty_kb = 0;
    if (shared) {
        if (anon)
            shared_dirty_kb = rss_kb;
        else
            shared_clean_kb = rss_kb;
    } else if (anon) {
        private_dirty_kb = rss_kb;
    } else {
        private_clean_kb = rss_kb;
    }

    if (print_header) {
        proc_printf(buf, "%08llx-%08llx %c%c%c%c %08lx 00:00 %-10d %s\n",
                (unsigned long long) (start << PAGE_BITS), (unsigned long long) (end << PAGE_BITS),
                start_pt->flags & P_READ ? 'r' : '-',
                start_pt->flags & P_WRITE ? 'w' : '-',
                start_pt->flags & P_EXEC ? 'x' : '-',
                shared ? 's' : 'p',
                (unsigned long) data->file_offset, 0, path);
        proc_printf(buf, "Size:           %8"PRIu64" kB\n", size_kb);
        proc_printf(buf, "KernelPageSize: %8u kB\n", PAGE_SIZE / 1024);
        proc_printf(buf, "MMUPageSize:    %8u kB\n", PAGE_SIZE / 1024);
        proc_printf(buf, "Rss:            %8"PRIu64" kB\n", rss_kb);
        proc_printf(buf, "Pss:            %8"PRIu64" kB\n", pss_kb);
        proc_printf(buf, "Shared_Clean:   %8"PRIu64" kB\n", shared_clean_kb);
        proc_printf(buf, "Shared_Dirty:   %8"PRIu64" kB\n", shared_dirty_kb);
        proc_printf(buf, "Private_Clean:  %8"PRIu64" kB\n", private_clean_kb);
        proc_printf(buf, "Private_Dirty:  %8"PRIu64" kB\n", private_dirty_kb);
        proc_printf(buf, "Referenced:     %8"PRIu64" kB\n", rss_kb);
        proc_printf(buf, "Anonymous:      %8"PRIu64" kB\n", anon ? rss_kb : 0);
        proc_printf(buf, "AnonHugePages:  %8d kB\n", 0);
        proc_printf(buf, "Swap:           %8"PRIu64" kB\n", swap_kb);
        // Locked stays 0: mlock/mlockall are still range checks with no pin
        // behind them (kernel/mmap.c), so no page in this region is actually
        // pinned and 0 is the true count, not a placeholder. Section 3.12 pairs
        // real pins with this field; both belong to the same change and neither
        // is in this layer.
        proc_printf(buf, "Locked:         %8d kB\n", 0);
        proc_printf(buf, "VmFlags:%s%s%s%s\n",
                start_pt->flags & P_READ ? " rd" : "",
                start_pt->flags & P_WRITE ? " wr" : "",
                start_pt->flags & P_EXEC ? " ex" : "",
                shared ? " sh" : "");
    }

    if (totals != NULL) {
        totals->rss_kb += rss_kb;
        totals->pss_kb += pss_kb;
        totals->shared_clean_kb += shared_clean_kb;
        totals->shared_dirty_kb += shared_dirty_kb;
        totals->private_clean_kb += private_clean_kb;
        totals->private_dirty_kb += private_dirty_kb;
        totals->anonymous_kb += anon ? rss_kb : 0;
        totals->swap_kb += swap_kb;
    }
}

// A lazy reservation, as smaps shows it: mapped, never touched, so nothing of
// it is resident, shared or swapped. Its size, page sizes and flags are set; every count is 0.
// Without this smaps left out every reservation that maps lists: an untouched
// 128M mmap was in maps and missing here, and once splits stopped materialising
// the rest of a reservation, a JVM heap with one committed page was three
// regions in maps and one here.
static void proc_smaps_reservation(struct proc_data *buf, const struct mem_lazy_map *l,
                                   bool print_header) {
    if (!print_header)
        return;
    uint64_t size_kb = (uint64_t) (l->end - l->start) * (PAGE_SIZE / 1024);
    bool shared = (l->flags & P_SHARED) != 0;
    proc_printf(buf, "%08llx-%08llx %c%c%c%c %08lx 00:00 %-10d \n",
            (unsigned long long) (l->start << PAGE_BITS), (unsigned long long) (l->end << PAGE_BITS),
            l->flags & P_READ ? 'r' : '-',
            l->flags & P_WRITE ? 'w' : '-',
            l->flags & P_EXEC ? 'x' : '-',
            shared ? 's' : 'p',
            0ul, 0);
    proc_printf(buf, "Size:           %8"PRIu64" kB\n", size_kb);
    proc_printf(buf, "KernelPageSize: %8u kB\n", PAGE_SIZE / 1024);
    proc_printf(buf, "MMUPageSize:    %8u kB\n", PAGE_SIZE / 1024);
    static const char *const zero_rows[] = {
        "Rss:            ", "Pss:            ", "Shared_Clean:   ", "Shared_Dirty:   ",
        "Private_Clean:  ", "Private_Dirty:  ", "Referenced:     ", "Anonymous:      ",
        "AnonHugePages:  ", "Swap:           ", "Locked:         ",
    };
    for (size_t i = 0; i < sizeof(zero_rows) / sizeof(zero_rows[0]); i++)
        proc_printf(buf, "%s%8d kB\n", zero_rows[i], 0);
    proc_printf(buf, "VmFlags:%s%s%s%s\n",
            l->flags & P_READ ? " rd" : "",
            l->flags & P_WRITE ? " wr" : "",
            l->flags & P_EXEC ? " ex" : "",
            shared ? " sh" : "");
}

static void proc_smaps_walk(struct task *task, struct proc_data *buf, bool rollup) {
    struct mm *mm = proc_task_mm_retain(task);
    struct mem *mem = mm ? &mm->mem : NULL;
    if (mem == NULL)
        return;

    struct smaps_totals totals = {0};
    page_t rollup_start = 0;
    page_t rollup_end = 0;
    bool any_region = false;
    // Asked once for the whole file, not once per page: with swap off no entry
    // can be non-resident, so the per-page load below would always answer the
    // same thing and this walk stays exactly as expensive as it was.
    bool count_swapped = swap_enabled();

    mem_read_lock_quiesce_aware(mem);
    // Reservations are regions too, merged in by address as proc_maps_dump
    // does, so the two files list the same regions.
    struct mem_lazy_map pending[MEM_LAZY_MAX];
    unsigned pending_n = collect_pending_reservations(mem, pending);
    unsigned pending_i = 0;
    page_t page = 0;
    while (page < mem->page_limit) {
        while (page < mem->page_limit && mem_pt(mem, page) == NULL) {
            mem_next_page(mem, &page);
        }
        if (page >= mem->page_limit)
            break;
        page_t start = page;
        struct pt_entry *start_pt = mem_pt(mem, start);
        struct data *data = start_pt->data;
        uint64_t swapped_pages = 0;

        while (page < mem->page_limit) {
            struct pt_entry *pt = mem_pt(mem, page);
            if (pt == NULL)
                break;
            if ((pt->flags & P_RWX) != (start_pt->flags & P_RWX))
                break;
            if (!(pt->data == data || (pt->flags & P_ANONYMOUS && start_pt->flags & P_ANONYMOUS)))
                break;
            // Counted here, after the three tests that decide the page is part
            // of this region and before the step that can end it, so each page
            // of the region is counted exactly once -- including the page that
            // ends the region through the sparse-walk guard below, which is in
            // it, unlike the pages that fail a test above, which are not.
            //
            // Through mem_page_is_swapped, which asks the FRAME rather than
            // reading pt_entry::swap_state directly. That byte is a
            // conservative hint (see its comment in emu/memory.h): after a
            // fork, the address space that did not fault a frame back keeps
            // entries reading SWAPPED over memory that is resident. Reading it
            // here would make this file's Swap disagree with VmSwap in
            // /proc/<pid>/status, which comes from mem_resident_page_count and
            // does ask the frame -- two figures for one fact, differing, which
            // is exactly what deriving them from one source is meant to
            // prevent.
            if (count_swapped && mem_page_is_swapped(pt))
                swapped_pages++;
            page_t prev = page;
            mem_next_page(mem, &page);
            // Same sparse-walk trap as proc_maps_dump; see the comment there.
            // Here it also inflated every total this file reports, since a
            // region's Rss is computed from its extent.
            if (page != prev + 1) {
                page = prev + 1;
                break;
            }
        }
        page_t end = page;

        for (; pending_i < pending_n && pending[pending_i].start < start; pending_i++) {
            if (!any_region)
                rollup_start = pending[pending_i].start;
            any_region = true;
            proc_smaps_reservation(buf, &pending[pending_i], !rollup);
        }
        if (!any_region)
            rollup_start = start;
        rollup_end = end;
        any_region = true;

        char path[MAX_PATH] = "";
        if (start_pt->flags & P_GROWSDOWN) {
            static const char s[] = "[stack]";
            memcpy(path, s, sizeof(s));
        } else if (data->name != NULL) {
            strncpy(path, data->name, sizeof(path) - 1);
            path[sizeof(path) - 1] = '\0';
        } else if (data->fd != NULL) {
            generic_getpath(data->fd, path);
        }

        proc_smaps_region(buf, start, end, start_pt, data, path, !rollup, swapped_pages, &totals);
    }
    for (; pending_i < pending_n; pending_i++) {
        if (!any_region)
            rollup_start = pending[pending_i].start;
        any_region = true;
        rollup_end = pending[pending_i].end;
        proc_smaps_reservation(buf, &pending[pending_i], !rollup);
    }
    mem_read_unlock_quiesce_aware(mem);

    if (rollup && any_region) {
        proc_printf(buf, "%08llx-%08llx ---p 00000000 00:00 0                          [rollup]\n",
                (unsigned long long) (rollup_start << PAGE_BITS), (unsigned long long) (rollup_end << PAGE_BITS));
        proc_printf(buf, "Rss:            %8"PRIu64" kB\n", totals.rss_kb);
        proc_printf(buf, "Pss:            %8"PRIu64" kB\n", totals.pss_kb);
        proc_printf(buf, "Shared_Clean:   %8"PRIu64" kB\n", totals.shared_clean_kb);
        proc_printf(buf, "Shared_Dirty:   %8"PRIu64" kB\n", totals.shared_dirty_kb);
        proc_printf(buf, "Private_Clean:  %8"PRIu64" kB\n", totals.private_clean_kb);
        proc_printf(buf, "Private_Dirty:  %8"PRIu64" kB\n", totals.private_dirty_kb);
        proc_printf(buf, "Referenced:     %8"PRIu64" kB\n", totals.rss_kb);
        proc_printf(buf, "Anonymous:      %8"PRIu64" kB\n", totals.anonymous_kb);
        proc_printf(buf, "Swap:           %8"PRIu64" kB\n", totals.swap_kb);
        proc_printf(buf, "Locked:         %8d kB\n", 0);
    }

    mm_release(mm);
}

static int proc_pid_smaps_show(struct proc_entry *entry, struct proc_data *buf) {
    struct task *task = proc_get_task(entry);
    if ((task == NULL) || (task->exiting == true)) {
        proc_put_task(task);
        return _ESRCH;
    }
    proc_smaps_walk(task, buf, false);
    proc_put_task(task);
    return 0;
}

static int proc_pid_smaps_rollup_show(struct proc_entry *entry, struct proc_data *buf) {
    struct task *task = proc_get_task(entry);
    if ((task == NULL) || (task->exiting == true)) {
        proc_put_task(task);
        return _ESRCH;
    }
    proc_smaps_walk(task, buf, true);
    proc_put_task(task);
    return 0;
}

static ssize_t proc_pid_mem_pread(struct proc_entry *entry, struct proc_data *buf, off_t offset, int UNUSED(flags)) {
    struct task *task = proc_get_task(entry);
    if (task == NULL)
        return _ESRCH;
    // The mode alone is not the gate: an fd opened when the credentials
    // allowed it must not keep working after they change, and root-owned
    // 0600 still has to be refused to a different uid.
    if (!current_may_access_task_mem(task)) {
        proc_put_task(task);
        return _EACCES;
    }
    struct mm *mm = proc_task_mm_retain(task);
    if (mm == NULL) {
        proc_put_task(task);
        return _ESRCH;
    }
    int result = user_read_task_mem(task, &mm->mem, (guest_addr_t)offset, buf->data, buf->size);
    mm_release(mm);
    proc_put_task(task);
    // An address the target has not mapped is an I/O error, not a permission
    // one -- mem_rw's access_remote_vm failure returns -EIO. The bare -1 this
    // used to return is _EPERM, which reads as "you may not do this at all"
    // and is what gdb reported when it probed an unmapped address.
    // MEASURED on x86_64 Linux 6.12: pread of an unmapped address through
    // /proc/<pid>/task/<tid>/mem gives EIO, at address 0 too.
    return result ? _EIO : (ssize_t) buf->size;
}

static ssize_t proc_pid_mem_pwrite(struct proc_entry *entry, struct proc_data *buf, off_t offset) {
    struct task *task = proc_get_task(entry);
    if (task == NULL)
        return _ESRCH;
    // The mode alone is not the gate: an fd opened when the credentials
    // allowed it must not keep working after they change, and root-owned
    // 0600 still has to be refused to a different uid.
    if (!current_may_access_task_mem(task)) {
        proc_put_task(task);
        return _EACCES;
    }
    struct mm *mm = proc_task_mm_retain(task);
    if (mm == NULL) {
        proc_put_task(task);
        return _ESRCH;
    }
    int result = user_write_task_ptrace_mem(task, &mm->mem, (guest_addr_t)offset, buf->data, buf->size);
    mm_release(mm);
    proc_put_task(task);
    // See the read side: EIO, not the bare -1 that means EPERM.
    return result ? _EIO : (ssize_t) buf->size;
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
    if (err >= 0)
        err = fs_rebase_readlink_path(current->fs, buf);
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
    // Real Linux appends a "Pid:" line for pidfds (fs/proc/fd.c's
    // pidfd_show_fdinfo); glibc's pidfd_get_pid() reads exactly this as its
    // fallback when the PIDFD_GET_INFO ioctl isn't supported, and treats a
    // missing line as "not a pidfd" (-ENOTTY). See fd_pidfd_pid's comment.
    pid_t_ pidfd_pid = fd_pidfd_pid(fd);
    if (pidfd_pid != -1)
        proc_printf(buf, "Pid:\t%d\n", pidfd_pid);
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
    // Rebase against the CALLING process's chroot root, not the target
    // task's -- readlink of /proc/*/{exe,cwd,root,fd/N} is d_path() against
    // current's root on real Linux (see fs_rebase_readlink_path).
    if (err >= 0)
        err = fs_rebase_readlink_path(current->fs, buf);
    return err;
}

static void proc_pid_task_getname(struct proc_entry *entry, char *buf) {
    snprintf(buf, 256, "%d", entry->pid);
}

static struct proc_dir_entry proc_pid_task;

// A thread cannot itself have a thread group, so /proc/<pid>/task/<tid>/
// must NOT expose a "task" child the way /proc/<pid>/ does -- reusing
// proc_pid_children wholesale there would make each thread directory
// contain another "task" entry pointing at itself, i.e.
// task/<tid>/task/<tid>/task/... forever. htop's thread scanner walked
// straight into that, endlessly rediscovering "more threads" one level
// down and leaking an fd per open() until it hit its fd-table limit and
// wedged. Filter that one entry out.
extern struct proc_children proc_pid_children;

static bool proc_pid_task_dir_readdir(struct proc_entry *entry, unsigned long *index, struct proc_entry *next_entry) {
    while (*index < proc_pid_children.count) {
        struct proc_dir_entry *candidate = &proc_pid_children.entries[*index];
        (*index)++;
        if (strcmp(candidate->name, "task") == 0)
            continue;
        *next_entry = (struct proc_entry) {candidate, .pid = entry->pid};
        return true;
    }
    return false;
}

static bool proc_pid_task_readdir(struct proc_entry *entry, unsigned long *index, struct proc_entry *next_entry) {
    struct task *task = proc_get_task(entry);
    if ((task == NULL) || (task->exiting == true)) {
        proc_put_task(task);
        return false;
    }

    pid_t_ tid = 0;
    bool found = false;
    unsigned long i = 0;
    complex_lockt(&pids_lock, 0);
    struct task *t;
    list_for_each_entry(&task->group->threads, t, group_links) {
        if (i == *index) {
            tid = t->pid;
            found = true;
            break;
        }
        i++;
    }
    unlock(&pids_lock);
    proc_put_task(task);

    if (!found)
        return false;
    (*index)++;
    *next_entry = (struct proc_entry) {&proc_pid_task, .pid = tid};
    return true;
}

// iSH implements no Linux namespaces at all: clone()/unshare() reject every
// CLONE_NEW* flag outright (kernel/fork.c), so every task lives in exactly one
// global namespace of each kind. Real Linux reports that same situation (no
// namespaces in use) with symlinks to the initial namespaces' well-known inode
// numbers, so we can report the identical values for every task rather than
// inventing per-task ones.
// The CLONE_NEW* flag each kind corresponds to, which is what NS_GET_NSTYPE
// reports. Defined here rather than shared with kernel/fork.c's copies
// because that file's are its own argument-validation constants; these are
// wire values handed to the guest. CLONE_NEWTIME has no entry there at all,
// since unshare rejects it along with the rest.
#define CLONE_NEWTIME_ 0x00000080
#define CLONE_NEWNS_ 0x00020000
#define CLONE_NEWCGROUP_ 0x02000000
#define CLONE_NEWUTS_ 0x04000000
#define CLONE_NEWIPC_ 0x08000000
#define CLONE_NEWUSER_ 0x10000000
#define CLONE_NEWPID_ 0x20000000
#define CLONE_NEWNET_ 0x40000000

struct proc_ns_type {
    const char *name;
    unsigned long inode;
    unsigned nstype;
};

static const struct proc_ns_type proc_ns_types[] = {
    {"cgroup", 4026531835, CLONE_NEWCGROUP_},
    {"ipc", 4026531839, CLONE_NEWIPC_},
    {"mnt", 4026531840, CLONE_NEWNS_},
    {"net", 4026531956, CLONE_NEWNET_},
    {"pid", 4026531836, CLONE_NEWPID_},
    {"pid_for_children", 4026531836, CLONE_NEWPID_},
    {"time", 4026531834, CLONE_NEWTIME_},
    {"time_for_children", 4026531834, CLONE_NEWTIME_},
    {"user", 4026531837, CLONE_NEWUSER_},
    {"uts", 4026531838, CLONE_NEWUTS_},
};
#define PROC_NS_TYPES_LEN (sizeof(proc_ns_types) / sizeof(proc_ns_types[0]))

static struct proc_dir_entry proc_pid_ns_entry;

static bool proc_pid_ns_readdir(struct proc_entry *entry, unsigned long *index, struct proc_entry *next_entry) {
    struct task *task = proc_get_task(entry);
    if ((task == NULL) || (task->exiting == true)) {
        proc_put_task(task);
        return false;
    }
    proc_put_task(task);
    if (*index >= PROC_NS_TYPES_LEN)
        return false;
    *next_entry = (struct proc_entry) {&proc_pid_ns_entry, .pid = entry->pid, .fd = (sdword_t) *index};
    (*index)++;
    return true;
}

static void proc_pid_ns_getname(struct proc_entry *entry, char *buf) {
    snprintf(buf, 256, "%s", proc_ns_types[entry->fd].name);
}

static int proc_pid_ns_readlink(struct proc_entry *entry, char *buf) {
    struct task *task = proc_get_task(entry);
    if ((task == NULL) || (task->exiting == true)) {
        proc_put_task(task);
        return _ESRCH;
    }
    proc_put_task(task);
    snprintf(buf, MAX_PATH, "%s:[%lu]", proc_ns_types[entry->fd].name, proc_ns_types[entry->fd].inode);
    return 0;
}

// A namespace fd does no I/O of its own; it exists to be handed to setns().
// (Our setns is an ENOSYS stub, which savers like nix tolerate at restore
// time -- it's the failure to OPEN the fd that they treat as fatal.)
// Linux fails reads on an nsfs fd with EINVAL (no read op in nsfs), not
// EBADF, even though the fd was opened readable.
static ssize_t proc_ns_read(struct fd *UNUSED(fd), void *UNUSED(buf), size_t UNUSED(bufsize)) {
    return _EINVAL;
}
// The nsfs ioctls (linux/nsfs.h). A namespace fd cannot be read or written,
// but it can be ASKED things, and lsns is built entirely out of asking: it
// opens every /proc/<pid>/ns/* link and calls NS_GET_USERNS on each to group
// namespaces by owner. Answering ENOTTY made it give up with "Unsupported
// ioctl NS_GET_USERNS" and print nothing at all.
//
// Every answer below is what Devuan gives for the INITIAL namespaces, which
// is the situation AOK is permanently in -- one namespace of each kind, owned
// by root, with no parent anybody may look at. Measured, not assumed:
//
//   NS_GET_USERNS     an fd for the owning user namespace; EPERM when asked
//                     of a user namespace that is its own owner
//   NS_GET_PARENT     EINVAL for the flat kinds (mnt, net, uts, ipc, cgroup,
//                     time), EPERM for the hierarchical ones (pid, user)
//                     whose parent is outside what the caller may see
//   NS_GET_NSTYPE     the CLONE_NEW* flag for the kind
//   NS_GET_OWNER_UID  the owner's uid, and only for a user namespace --
//                     EINVAL for every other kind
//
// The distinction between EINVAL and EPERM on NS_GET_PARENT is not cosmetic:
// EINVAL means "this kind of namespace has no parent" and EPERM means "it has
// one and you may not see it", and lsns uses exactly that to decide whether a
// namespace is a hierarchy root.
#define NS_GET_USERNS_ 0xb701
#define NS_GET_PARENT_ 0xb702
#define NS_GET_NSTYPE_ 0xb703
#define NS_GET_OWNER_UID_ 0xb704

static ssize_t proc_ns_ioctl_size(int cmd) {
    // Only NS_GET_OWNER_UID takes a pointer; the rest carry their answer in
    // the return value.
    if (cmd == NS_GET_OWNER_UID_)
        return sizeof(uid_t_);
    return 0;
}

static struct fd *proc_ns_fd_for_index(unsigned index);

static int proc_ns_ioctl(struct fd *fd, int cmd, void *arg) {
    unsigned index = fd->nsfs.type_index;
    if (index >= PROC_NS_TYPES_LEN)
        return _EINVAL;
    unsigned nstype = proc_ns_types[index].nstype;
    switch (cmd) {
        case NS_GET_NSTYPE_:
            return (int) nstype;

        case NS_GET_OWNER_UID_:
            // A user namespace is owned by the uid that created it, and the
            // initial one is root's. Asking any other kind is EINVAL -- the
            // question only means something about a user namespace.
            if (nstype != CLONE_NEWUSER_)
                return _EINVAL;
            *(uid_t_ *) arg = 0;
            return 0;

        case NS_GET_PARENT_:
            // Only user and pid namespaces nest. For the rest the question is
            // malformed; for these two the parent of the initial namespace is
            // itself, which the caller is not permitted to be handed.
            if (nstype != CLONE_NEWUSER_ && nstype != CLONE_NEWPID_)
                return _EINVAL;
            return _EPERM;

        case NS_GET_USERNS_: {
            // Everything here is owned by the one user namespace. Asking that
            // namespace who owns IT walks off the top, which Linux refuses.
            if (nstype == CLONE_NEWUSER_)
                return _EPERM;
            unsigned user_index;
            for (user_index = 0; user_index < PROC_NS_TYPES_LEN; user_index++)
                if (proc_ns_types[user_index].nstype == CLONE_NEWUSER_)
                    break;
            if (user_index == PROC_NS_TYPES_LEN)
                return _EINVAL;
            struct fd *userns = proc_ns_fd_for_index(user_index);
            if (IS_ERR(userns))
                return PTR_ERR(userns);
            // A namespace fd handed out by an ioctl is close-on-exec on
            // Linux, and lsns leaks one per namespace per process without it.
            return f_install(userns, O_CLOEXEC_);
        }
    }
    return _ENOTTY;
}

static const struct fd_ops proc_ns_fdops = {
    .read = proc_ns_read,
    .ioctl_size = proc_ns_ioctl_size,
    .ioctl = proc_ns_ioctl,
    .anon_inode_class = "nsfs",
};

// One namespace fd, for the kind at `index`. The fd carries nothing but which
// kind it is: there is exactly one namespace of each, so the kind IS the
// identity, and its inode number is the one Linux gives the initial namespace.
static struct fd *proc_ns_fd_for_index(unsigned index) {
    struct fd *fd = adhoc_fd_create(&proc_ns_fdops);
    if (fd == NULL)
        return ERR_PTR(_ENOMEM);
    fd->stat.mode = S_IFREG | 0444;
    fd->stat.inode = proc_ns_types[index].inode;
    fd->nsfs.type_index = index;
    return fd;
}

// On Linux the /proc/pid/ns/* entries are nsfs magic links: following them
// with open(2) yields a namespace fd, not a lookup of the "mnt:[inode]"
// readlink text as a path. Programs save such an fd to setns() back later --
// nix aborts outright ("saving parent mount namespace") if the open fails.
// Returns NULL if name is not a namespace entry (caller falls through to
// normal path resolution and its ENOENT).
struct fd *proc_ns_open(int pid, const char *name) {
    size_t i;
    for (i = 0; i < PROC_NS_TYPES_LEN; i++)
        if (strcmp(proc_ns_types[i].name, name) == 0)
            break;
    if (i == PROC_NS_TYPES_LEN)
        return NULL;
    struct task *task = pid_get_task_ref(pid);
    if (task == NULL)
        return ERR_PTR(_ENOENT);
    task_ref_cnt_mod(task, -1);
    return proc_ns_fd_for_index((unsigned) i);
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
    if (err >= 0)
        err = fs_rebase_readlink_path(current->fs, buf);
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
    if (err >= 0)
        err = fs_rebase_readlink_path(current->fs, buf);
    return err;
}

struct proc_children proc_pid_children = PROC_CHILDREN({
    // 0400 like Linux, not the 0444 default.
    {"auxv", S_IFREG | 0400, .show = proc_pid_auxv_show},
    {"cgroup", .show = proc_pid_cgroup_show},
    {"cmdline", .show = proc_pid_cmdline_show},
    {"comm", .show = proc_pid_comm_show},
    {"cwd", S_IFLNK, .readlink = proc_pid_cwd_readlink},
    // 0400 like Linux, not the 0444 default.
    {"environ", S_IFREG | 0400, .show = proc_pid_environ_show},
    {"exe", S_IFLNK, .readlink = proc_pid_exe_readlink},
    {"fd", S_IFDIR, .readdir = proc_pid_fd_readdir},
    {"fdinfo", S_IFDIR, .readdir = proc_pid_fdinfo_readdir},
    {"io", .show = proc_pid_io_show},
    {"maps", .show = proc_pid_maps_show},
    // 0600 like Linux, not the 0444 default: the mode is the first gate, and
    // the credential check in the handlers is the second.
    {"mem", S_IFREG | 0600, .pread = proc_pid_mem_pread, .pwrite = proc_pid_mem_pwrite},
    {"mountinfo", .show = proc_show_mountinfo},
    {"mounts", .show = proc_show_mounts},
    {"ns", S_IFDIR, .readdir = proc_pid_ns_readdir},
    // 0644, as Linux has it: writing is how a process lowers its own OOM
    // score, and the update handler below has always worked -- only the
    // advertised mode said read-only, so anything that checked before writing
    // gave up without trying.
    {"oom_score_adj", S_IFREG | 0644, .show = proc_pid_oom_score_adj_show, .update = proc_pid_oom_score_adj_update},
    {"root", S_IFLNK, .readlink = proc_pid_root_readlink},
    {"sched", .show = proc_pid_sched_show},
    {"smaps", .show = proc_pid_smaps_show},
    {"smaps_rollup", .show = proc_pid_smaps_rollup_show},
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

static struct proc_dir_entry proc_pid_ns_entry = {NULL, S_IFLNK,
    .getname = proc_pid_ns_getname, .readlink = proc_pid_ns_readlink};

static struct proc_dir_entry proc_pid_task = {NULL, S_IFDIR,
    .readdir = proc_pid_task_dir_readdir, .getname = proc_pid_task_getname};

void proc_pid_init(void) {
    struct proc_dir_entry *fd_dir;
    struct proc_dir_entry *fdinfo_dir;
    struct proc_dir_entry *task_dir;
    struct proc_dir_entry *ns_dir;

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

    ns_dir = proc_children_find(&proc_pid_children, "ns");
    if (ns_dir != NULL)
        proc_pid_ns_entry.parent = ns_dir;
}
