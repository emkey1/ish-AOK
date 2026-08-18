#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include "kernel/calls.h"
#include "kernel/native.h"
#include "kernel/task.h"
#include "emu/memory.h"
#include "emu/tlb.h"
#include "jit/jit.h"
#include "platform/platform.h"
#include "util/sync.h"
#include "util/timer.h"
#if defined(__APPLE__)
#include <libkern/OSAtomic.h>
#include <os/proc.h>
#include <mach/mach.h>
#endif
#include <dlfcn.h>

#define GRACE_PERIOD 2 // How long we want to deallocate tasks that have exited

pthread_mutex_t multicore_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t extra_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t delay_lock = PTHREAD_MUTEX_INITIALIZER;
extern lock_t atomic_l_lock;
pthread_mutex_t wait_for_lock = PTHREAD_MUTEX_INITIALIZER;
time_t boot_time;  // Store the boot time.

struct list tasks_pending_deletion_queue;
pthread_mutex_t tasks_pending_deletion_lock = PTHREAD_MUTEX_INITIALIZER;

int iOSMajorRelease;

bool doEnableMulticore; // Enable multicore if toggled, should default to false
bool isGlibC = false; // Try to guess if we're running a non-musl distro.
bool doEnableExtraLocking; // Enable extra locking if toggled, should default to true

__thread struct task *current;

static dword_t last_allocated_pid = 0;
static struct pid pids[MAX_PID + 1] = {};
lock_t pids_lock;
lock_t block_lock;
struct list alive_pids_list;

void init_pending_queues(void) {
// Initialize the pending deletion queues.  Tasks, memory and file descriptors (eventually)
    list_init(&tasks_pending_deletion_queue);
    
}

static bool pid_empty(struct pid *pid) {
    return pid->task == NULL && list_empty(&pid->session) && list_empty(&pid->pgroup);
}

struct pid *pid_get(dword_t id) {
    if (id >= sizeof(pids)/sizeof(pids[0]))
        return NULL;
    struct pid *pid = &pids[id];
    if (pid_empty(pid))
        return NULL;
    return pid;
}

struct task *pid_get_task_zombie(dword_t id) {
    struct pid *pid = pid_get(id);
    if (pid == NULL)
        return NULL;
    struct task *task = pid->task;
    return task;
}

struct task *pid_get_task(dword_t id) {
    struct task *task = pid_get_task_zombie(id);
    if (task != NULL && task->zombie)
        return NULL;
    return task;
}

struct task *pid_get_task_ref(dword_t id) {
    complex_lockt(&pids_lock, 0);
    struct task *task = pid_get_task(id);
    if (task != NULL)
        task_ref_cnt_mod(task, 1);
    unlock(&pids_lock);
    return task;
}

void task_snapshot_release(struct task_snapshot *snapshot) {
    for (unsigned i = 0; i < snapshot->count; i++)
        task_ref_cnt_mod(snapshot->tasks[i], -1);
    free(snapshot->tasks);
    snapshot->tasks = NULL;
    snapshot->count = 0;
}

int task_snapshot_collect(struct task_snapshot *snapshot, bool leaders_only) {
    unsigned cap = 0;
    complex_lockt(&pids_lock, 0);
    struct pid *pid_entry;
    list_for_each_entry(&alive_pids_list, pid_entry, alive) {
        struct task *task = pid_entry->task;
        if (task == NULL || task->zombie || task->exiting)
            continue;
        if (leaders_only && !task_is_leader(task))
            continue;
        if (snapshot->count == cap) {
            unsigned new_cap = cap ? cap * 2 : 64;
            struct task **new_tasks = realloc(snapshot->tasks, sizeof(*new_tasks) * new_cap);
            if (new_tasks == NULL) {
                unlock(&pids_lock);
                task_snapshot_release(snapshot);
                return _ENOMEM;
            }
            snapshot->tasks = new_tasks;
            cap = new_cap;
        }
        task_ref_cnt_mod(task, 1);
        snapshot->tasks[snapshot->count++] = task;
    }
    unlock(&pids_lock);
    return 0;
}

struct pid *pid_get_last_allocated(void) {
    if (!last_allocated_pid) {
        return NULL;
    }
    return pid_get(last_allocated_pid);
}

inline void task_ref_cnt_mod(struct task *task, int value) { // value should only be -1 or 1.
    // Keep track of how many threads are referencing this task. This used to
    // be skipped when doEnableExtraLocking was off, but the flag is a live
    // preference toggle: flipping it mid-run left counts taken under one
    // setting and released under the other permanently imbalanced. The count
    // gates task teardown, so it is now maintained unconditionally; as a
    // lock-free atomic it is cheap enough to always be on.
    if(task == NULL) {
        if(current != NULL) {
            task = current;
        } else {
            return;
        }
    }

    if (value != 1 && value != -1) {
        printk("ERROR: invalid task refcount delta %d for %s:%d\n",
               value, task->comm, task->pid);
        return;
    }

    int old_count = atomic_load_explicit(&task->reference.count, memory_order_relaxed);
    do {
        if(((old_count + value) < 0) && (task->pid > 9)) { // Prevent the count from going negative.
            void *caller = __builtin_return_address(0);
            Dl_info caller_info = {};
            const char *caller_name = "?";
            ptrdiff_t caller_offset = 0;
            if (caller != NULL && dladdr(caller, &caller_info) != 0 && caller_info.dli_sname != NULL) {
                caller_name = caller_info.dli_sname;
                caller_offset = (char *) caller - (char *) caller_info.dli_saddr;
            }
            printk("ERROR: Attempt to decrement task reference count to be negative, ignoring(%s:%d) (%d - %d) caller=%s+%td addr=%p\n",
                   task->comm, task->pid, old_count, value, caller_name, caller_offset, caller);
            return;
        }
    } while (!atomic_compare_exchange_weak_explicit(&task->reference.count, &old_count, old_count + value,
                                                    memory_order_acq_rel, memory_order_relaxed));
}

dword_t get_count_of_blocked_tasks(void) {
    // task_ref_cnt_mod(current, 1);  // Not needed?
    dword_t res = 0;
    struct pid *pid_entry;
    complex_lockt(&pids_lock, 0);
    list_for_each_entry(&alive_pids_list, pid_entry, alive) {
        if (pid_entry->task->io_block) {
            res++;
        }
    }
    // task_ref_cnt_mod(current, -1);
    unlock(&pids_lock);
    return res;
}

dword_t get_count_of_alive_tasks(void) {
    complex_lockt(&pids_lock, 0);
    dword_t res = 0;
    struct list *item;
    list_for_each(&alive_pids_list, item) {
        res++;
    }
    unlock(&pids_lock);
    return res;
}

// Linux-style load average computed over the guest's OWN runnable tasks, so
// /proc/loadavg reflects the guest rather than the host load that the platform
// getloadavg returns. The EMA is advanced lazily on read, one step per elapsed
// 5-second interval (the classic calc_load cadence).
#define GUEST_LOAD_FSHIFT 11
#define GUEST_LOAD_FIXED_1 (1u << GUEST_LOAD_FSHIFT)
void get_guest_loadavg(uint64_t out[3]) {
    static const unsigned exp[3] = {1884, 2014, 2037}; // 1/exp(5s/{1,5,15}min) in FIXED_1
    static lock_t load_lock = LOCK_INITIALIZER;
    static uint64_t load[3];
    static time_t last_sec;

    // Runnable tasks = alive minus io-blocked, excluding this reader itself.
    long active = (long) get_count_of_alive_tasks() - (long) get_count_of_blocked_tasks() - 1;
    if (active < 0)
        active = 0;
    struct timespec now = timespec_now(CLOCK_MONOTONIC);

    lock(&load_lock, 0);
    if (last_sec == 0)
        last_sec = now.tv_sec;
    long steps = (now.tv_sec - last_sec) / 5;
    if (steps > 0) {
        long do_steps = steps > 64 ? 64 : steps;
        for (long s = 0; s < do_steps; s++)
            for (int i = 0; i < 3; i++)
                load[i] = (load[i] * exp[i] +
                           (uint64_t) active * GUEST_LOAD_FIXED_1 * (GUEST_LOAD_FIXED_1 - exp[i]))
                          >> GUEST_LOAD_FSHIFT;
        last_sec = steps > 64 ? now.tv_sec : last_sec + steps * 5;
    }
    for (int i = 0; i < 3; i++)
        out[i] = load[i] << (16 - GUEST_LOAD_FSHIFT);
    unlock(&load_lock);
}

// ---- per-emulated-CPU time accounting (for /proc/stat's cpuN lines) --------
//
// iSH has no real CPU affinity: every guest task is a host pthread that the
// host kernel schedules wherever it likes, so "which emulated CPU did the
// work" has no ground truth. Define it here instead: each task is bucketed
// into virtual-CPU slot pid % ncpu and its REAL thread CPU time is charged to
// that slot -- live tasks are sampled on each /proc/stat read, and do_exit
// banks a task's final time so short-lived processes (compilers!) stay
// visible in the counters. This replaces the old even split of the process
// total, which made every cpuN line identical and diluted a busy task's usage
// by 1/ncpu. The slot sums won't exactly match the aggregate "cpu" line
// (which uses process-wide accounting including non-task host threads), and a
// slot holding several busy tasks can saturate at 100%; both are acceptable
// for what these lines are for (top/htop-style meters).

#define CPU_SLOTS_MAX 64
static _Atomic uint64_t cpu_slot_dead_user[CPU_SLOTS_MAX];
static _Atomic uint64_t cpu_slot_dead_system[CPU_SLOTS_MAX];

// Serializes banking against the /proc/stat reader. Without this, a task
// exiting mid-read could be counted twice in one snapshot: sampled live
// during the reader's task-list walk, then ALSO included via the dead-slot
// totals it banked before the reader loaded them. That read reports an
// inflated cpuN value and the next read drops back down -- a backward-moving
// /proc/stat counter, which real kernels never produce and which top-style
// tools turn into a huge unsigned delta (ktop crashed on exactly this).
// Lock order: pids_lock -> cpu_slots_lock (the reader); do_exit's banking
// takes only cpu_slots_lock.
static lock_t cpu_slots_lock = LOCK_INITIALIZER;

static int task_cpu_slot(struct task *task, int ncpu) {
    if (ncpu > CPU_SLOTS_MAX)
        ncpu = CPU_SLOTS_MAX;
    if (ncpu < 1)
        ncpu = 1;
    return (int) (task->pid % (dword_t) ncpu);
}

void task_thread_cpu_time(struct task *task, unsigned long *out_utime, unsigned long *out_stime) {
    *out_utime = 0;
    *out_stime = 0;
#ifdef __APPLE__
    mach_port_t mach_thread = pthread_mach_thread_np(task->thread);
    if (mach_thread != MACH_PORT_NULL) {
        thread_basic_info_data_t info;
        mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
        if (thread_info(mach_thread, THREAD_BASIC_INFO,
                        (thread_info_t) &info, &count) == KERN_SUCCESS) {
            *out_utime = (unsigned long) info.user_time.seconds * 100
                         + (unsigned long) info.user_time.microseconds / 10000;
            *out_stime = (unsigned long) info.system_time.seconds * 100
                         + (unsigned long) info.system_time.microseconds / 10000;
        }
    }
#else
    clockid_t clkid;
    if (pthread_getcpuclockid(task->thread, &clkid) == 0) {
        struct timespec ts;
        if (clock_gettime(clkid, &ts) == 0)
            *out_utime = (unsigned long) ts.tv_sec * 100
                         + (unsigned long) (ts.tv_nsec / 10000000);
    }
#endif
}

void task_bank_cpu_time(struct task *task) {
    lock(&cpu_slots_lock, 0);
    if (!task->cpu_time_banked) {
        unsigned long utime, stime;
        task_thread_cpu_time(task, &utime, &stime);
        int slot = task_cpu_slot(task, get_cpu_count());
        atomic_fetch_add(&cpu_slot_dead_user[slot], utime);
        atomic_fetch_add(&cpu_slot_dead_system[slot], stime);
        task->cpu_time_banked = true;
    }
    unlock(&cpu_slots_lock);
}

int get_emulated_per_cpu_usage(struct cpu_usage **cpus_usage) {
    int ncpu = get_cpu_count();
    if (ncpu < 1)
        ncpu = 1;
    struct cpu_usage *cpus = calloc((size_t) ncpu, sizeof(*cpus));
    if (cpus == NULL)
        return _ENOMEM;

    complex_lockt(&pids_lock, 0);
    // Hold cpu_slots_lock across BOTH the live walk and the dead-slot loads:
    // an exiting task then either banked before this snapshot (skipped live,
    // counted dead) or banks after it (counted live now, dead next time) --
    // never both in one read, so the reported counters stay monotonic.
    lock(&cpu_slots_lock, 0);
    struct pid *pid_entry;
    list_for_each_entry(&alive_pids_list, pid_entry, alive) {
        struct task *task = pid_entry->task;
        // A banked task's time is already in the dead-slot totals; its host
        // thread may be gone, so don't sample it (and don't double-count).
        // A task whose own host thread hasn't started yet still carries its
        // PARENT's pthread from the task_create_ struct copy -- sampling it
        // would charge the parent's whole CPU time to this task's slot, then
        // retract it on the next read (backward counters).
        if (task == NULL || task->cpu_time_banked || !task->host_thread_started)
            continue;
        unsigned long utime, stime;
        task_thread_cpu_time(task, &utime, &stime);
        int slot = task_cpu_slot(task, ncpu);
        cpus[slot].user_ticks += utime;
        cpus[slot].system_ticks += stime;
    }

    // Each virtual CPU has uptime ticks of capacity; whatever its tasks
    // didn't use was idle.
    uint64_t uptime_ticks = get_uptime().uptime_ticks;
    for (int i = 0; i < ncpu; i++) {
        if (i < CPU_SLOTS_MAX) {
            cpus[i].user_ticks += atomic_load(&cpu_slot_dead_user[i]);
            cpus[i].system_ticks += atomic_load(&cpu_slot_dead_system[i]);
        }
        uint64_t busy = cpus[i].user_ticks + cpus[i].system_ticks;
        cpus[i].idle_ticks = uptime_ticks > busy ? uptime_ticks - busy : 0;
        cpus[i].nice_ticks = 0;
    }
    unlock(&cpu_slots_lock);
    unlock(&pids_lock);
    *cpus_usage = cpus;
    return 0;
}

struct task *task_create_(struct task *parent) {
    struct task *task = malloc(sizeof(struct task));
    if (task == NULL)
        return NULL;

    *task = (struct task) {};
    if (parent != NULL)
        *task = *parent; // uts_ns is only aliased here; copy_task retains or copies it
    else {
        task->uts_ns = uts_ns_retain(&init_uts_ns);
        // Treat init/root as starting with the full Linux capability set so
        // guest helpers such as setpriv can drop or reshuffle capabilities
        // without tripping over uninitialized state.
        task->abi = GUEST_ABI_I386;
        task->cap_effective[0] = task->cap_effective[1] = UINT32_MAX;
        task->cap_permitted[0] = task->cap_permitted[1] = UINT32_MAX;
        task->cap_inheritable[0] = task->cap_inheritable[1] = UINT32_MAX;
    }
    task->cpu_time_banked = false; // per-task, not inherited via the parent copy
    task->host_thread_started = false; // ditto; task_start sets it
    list_init(&task->group_links);
    list_init(&task->children);
    list_init(&task->siblings);
    list_init(&task->ptracees);
    list_init(&task->ptrace_siblings);
    list_init(&task->pidfds);
    task->pending = 0;
    task->waiting = 0;
    list_init(&task->queue);
    task->saved_mask = 0;
    task->has_saved_mask = false;
    task->clear_tid = 0;
    task->robust_list = 0;
    task->pdeath_signal = 0;
    task->did_exec = false;
    task->exit_code = 0;
    task->zombie = false;
    task->exiting = false;
    task->io_block = false;
    task->vfork = NULL;
    task->exit_signal = 0;

    // Both of these are OWNED heap pointers, and `*task = *parent` above is a
    // shallow copy, so leaving them aliased gives two tasks one allocation and
    // whichever dies first frees it under the other. task_free_final does
    // exactly that (native_exec_discard_pending, native_env_discard), and so
    // does native_env_init on the child's own exec.
    //
    // That is not theoretical. Native bash assigns `environ = export_env`, so
    // the task's environment vector IS bash's exported-variable array; every
    // command bash ran spawned a child task that inherited the pointer and
    // freed the array on its way out. The shell then read freed memory in
    // add_or_supercede_exported_var and the app died on a null entry -- while
    // running bash's own test suite, several commands after the one that
    // caused it.
    //
    // Null rather than duplicate: a task made here is on its way to an execve,
    // and the environment it ends up with is that call's envp. A native
    // program asking before then gets an empty vector from native_env_slot.
    task->native_env = NULL;
    task->native_exec = NULL;
    // The shim's signal bookkeeping describes the native program running in
    // the PARENT; a fresh task has none until it becomes one.
    task->native_prog_blocked = 0;
    task->native_held = 0;

    lock_init(&task->general_lock, "task_creat_gen\0");

    task->sockrestart = (struct task_sockrestart) {};
    list_init(&task->sockrestart.listen);

    task->waiting_cond = NULL;
    task->waiting_lock = NULL;
    task->waiting_interrupt_flag = NULL;
    task->wait_interrupted = false;
    task->restart_interrupted_syscall = false;
    task->futex_restart_futex = NULL;
    task->futex_restart_uaddr = 0;
    task->futex_restart_wake_seq = 0;
    task->poll_notify_fd = -1;
    lock_init(&task->waiting_cond_lock, "task_creat_wait\0");
    cond_init(&task->pause);

    task->ptrace = (typeof(task->ptrace)) {};
    lock_init(&task->ptrace.lock, "task_creat_ptr\0");
    cond_init(&task->ptrace.cond);

    task->locks_held.count = 0;
    pthread_mutex_init(&task->locks_held.lock, NULL);
    atomic_store_explicit(&task->reference.count, 0, memory_order_relaxed);
    task->reference.ready_to_be_freed = false;

    complex_lockt(&pids_lock, 0);
    do {
        last_allocated_pid++;
        if (last_allocated_pid > MAX_PID) last_allocated_pid = 1;
    } while (!pid_empty(&pids[last_allocated_pid]));
    struct pid *pid = &pids[last_allocated_pid];
    pid->id = last_allocated_pid;
    list_init(&pid->alive);
    list_init(&pid->session);
    list_init(&pid->pgroup);
    task->pid = pid->id;

    pid->task = task;
    list_add(&alive_pids_list, &pid->alive);
    if (parent != NULL) {
        task->parent = parent;
        list_add(&parent->children, &task->siblings);
    }
    unlock(&pids_lock);
    return task;
}

// We consolidate the check for whether the task is in a critical section,
// holds locks, or has pending signals into a single function.
bool should_wait(struct task *t) {
    // sighand is released (and nulled) before a task reaches this path during
    // teardown; a nulled sighand has nothing left to check.
    sigset_t_ group_pending = t->sighand != NULL ? t->sighand->pending : 0;
    return task_ref_cnt_get(t, 0) > 1 || locks_held_count(t) || !!((t->pending | group_pending) & ~t->blocked);
}

void task_unlink_locked(struct task *task) {
    task->exiting = true;
    list_remove(&task->siblings);
    list_remove_safe(&task->ptrace_siblings);
    struct pid *pid = pid_get(task->pid);
    pid->task = NULL;
    list_remove(&pid->alive);
}

static void task_free_final(struct task *task) {
    // A native program recorded by execve but never reached -- the task died
    // between the exec and its first execution (task_start failing, say).
    native_exec_discard_pending(task);
    native_env_discard(task);
    if (task != NULL && task_is_leader(task) && task->group != NULL) {
        cond_destroy(&task->group->child_exit);
        free(task->group->cgroup_path);
        free(task->group);
        task->group = NULL;
    }
    free(task);
}

void task_destroy_unlinked(struct task *task, int caller) {
    task->exiting = true;

    // We use a single loop to wait for the task to be ready to destroy.
    // This loop replaces all the similar while-loops in the original code.
    // Reap paths should not stall a waiting parent just to synchronously free
    // the task object. If references are still draining, defer cleanup.
    int count = caller == 2 ? 0 : -4000; // Counter to limit the number of times we check.
    while (count < 0 && should_wait(task)) {
        nanosleep(&lock_pause, NULL); // Sleep for a defined amount of time.
        count++;
    }

    if (task_ref_cnt_get(task, 1)) { // Check to see if another thread is accessing this process.  If yes, note that and defer freeing it
        struct task_pending_deletion *pd = malloc(sizeof(struct task_pending_deletion));
        if (pd) {
            task->reference.ready_to_be_freed = true;
            pd->task = task;
            pd->added_time = time(NULL);
            list_init(&pd->list);
            pthread_mutex_lock(&tasks_pending_deletion_lock);
            list_add(&tasks_pending_deletion_queue, &pd->list);
            pthread_mutex_unlock(&tasks_pending_deletion_lock);
        }
        // Lets cleanup any expired pending deletions here for now
        cleanup_pending_deletions();
        return;
    } else {
        task_free_final(task);
    }
}

void task_destroy(struct task *task, int caller) {
    task_unlink_locked(task);
    unlock(&pids_lock);
    task_destroy_unlinked(task, caller);
    complex_lockt(&pids_lock, 0);
}

// Cleanup function to delete tasks after the grace period
void cleanup_pending_deletions(void) {
    pthread_mutex_lock(&tasks_pending_deletion_lock);
    struct task_pending_deletion *pd, *tmp;
    list_for_each_entry_safe(&tasks_pending_deletion_queue, pd, tmp, list) {
        if (difftime(time(NULL), pd->added_time) >= GRACE_PERIOD &&
                atomic_load_explicit(&pd->task->reference.count, memory_order_acquire) == 0) { // Delete reaped tasks old and no longer referenced
            task_free_final(pd->task);
            list_remove(&pd->list);
            free(pd);
        }
    }
    pthread_mutex_unlock(&tasks_pending_deletion_lock);
}

void run_at_boot(void) {  // Stuff we run only once, at boot time.
    //atomic_thread_fence(__ATOMIC_SEQ_CST);
    struct uname uts;
    do_uname(&uts);
    unsigned short ncpu = get_cpu_count();
    lock_init(&pids_lock, "pids");
    lock_init(&block_lock, "block");
    lock_init(&atomic_l_lock, "run_at_boot");
    // No guest arch named here: this runs once at boot, and one session
    // can run i386, x86_64, and arm64 guests (per-task ABI).
    printk("iSH-AOK %s built %s %s booted on %d emulated CPU(s)\n",
            uts.release, __DATE__, __TIME__, ncpu);
    // Get boot time
    extern time_t boot_time;
         
    boot_time = time(NULL);
    //printk("Seconds since January 1, 1970 = %ld\n", boot_time);
}

extern _Atomic long quiesce_poke_calls;
extern _Atomic long quiesce_poke_noop;
extern _Atomic long quiesce_pokes_sent;
extern _Atomic long quiesce_pokes_skipped;
extern _Atomic long quiesce_reader_naps;

void task_poke_shared_mem(struct task *task, struct mem *mem) {
    if (task == NULL || mem == NULL)
        return;

    atomic_fetch_add_explicit(&quiesce_poke_calls, 1, memory_order_relaxed);
    if (trylock(&pids_lock) != 0) {
        atomic_fetch_add_explicit(&quiesce_poke_noop, 1, memory_order_relaxed);
        return;
    }
    struct pid *pid_entry;
    list_for_each_entry(&alive_pids_list, pid_entry, alive) {
        struct task *other = pid_entry->task;
        if (other == NULL || other == task)
            continue;
        if (other->mem != mem)
            continue;
        if (other->zombie || other->exiting)
            continue;
        // Only readers executing guest code hold the mem read lock and must be
        // evicted. A sibling parked in a blocking syscall (io_block) holds no
        // read lock, so poking it is pure waste -- the SIGUSR1 just bounces it
        // out of poll/futex for nothing (the real git/daemon storm: most
        // siblings sit in poll). Skip it. The race where it leaves io_block and
        // enters JIT right after this check is covered by mem_write_lock_with_
        // pokes re-poking every 64 attempts: by then io_block is clear and the
        // trylockw it now blocks forces another poke round that catches it.
        if (other->io_block) {
            atomic_fetch_add_explicit(&quiesce_pokes_skipped, 1, memory_order_relaxed);
            continue;
        }
        // Same reasoning for a sibling parked in task_wait_for_mem_quiesce
        // (quiesce_parked, task.h): it holds no read lock, and under a
        // barrier storm (one mprotect per pthread_create in the thread
        // benchmark) re-signalling every parked sibling each poke round is
        // exactly the SIGUSR1 flood that melted the host scheduler. Same
        // stale-read recovery contract as io_block above.
        if (atomic_load_explicit(&other->quiesce_parked, memory_order_relaxed)) {
            atomic_fetch_add_explicit(&quiesce_pokes_skipped, 1, memory_order_relaxed);
            continue;
        }
        // Already poked and hasn't consumed it: the sticky flag is still up,
        // so the sibling either hasn't reached a block boundary yet (the
        // flag, not the signal, is what evicts a JIT runner) or has already
        // exited guest code and is blocked on one of OUR locks. Re-signalling
        // it does nothing for the barrier — and a SIGUSR1 storm against a
        // thread parked in __psynch_rw_wrlock/rdlock is exactly the
        // repeated-EINTR pattern that wedged Darwin's psynch rwlock in the
        // mprotect-storm stress (writers asleep forever on a FREE lock).
        // cpu_take_poke clears the flag only when the task re-enters its run
        // loop, so this can't suppress a needed eviction.
        if (other->cpu.poked_ptr != NULL &&
                __atomic_load_n(other->cpu.poked_ptr, __ATOMIC_SEQ_CST)) {
            atomic_fetch_add_explicit(&quiesce_pokes_skipped, 1, memory_order_relaxed);
            continue;
        }
        pthread_kill(other->thread, SIGUSR1);
        atomic_fetch_add_explicit(&quiesce_pokes_sent, 1, memory_order_relaxed);
        if (other->cpu.poked_ptr == NULL)
            continue;
        cpu_poke(&other->cpu);
    }
    unlock(&pids_lock);
}

static void task_wait_for_mem_quiesce(struct task *task) {
    struct mem *mem = task != NULL ? task->mem : NULL;
    if (mem == NULL ||
            atomic_load_explicit(&mem->quiesce_requested, memory_order_acquire) == 0)
        return;
    // No mem read lock is held in here, so poking us can't help the barrier
    // writer — flag ourselves skippable (quiesce_parked, task.h). Cleared
    // before returning: the caller takes the read lock right after.
    atomic_store_explicit(&task->quiesce_parked, true, memory_order_relaxed);
    int spins = 0;
    while (atomic_load_explicit(&mem->quiesce_requested, memory_order_acquire) > 0)
        mem_quiesce_wait(mem, &spins);
    atomic_store_explicit(&task->quiesce_parked, false, memory_order_relaxed);
}

void task_run_current(void) {
    // A task whose image is a natively-implemented program never enters the
    // emulator at all: it is dispatched here instead, and does not return. The
    // execve entry points handle the ordinary case of an already-running task
    // exec'ing one; this covers a task whose FIRST image is native, which is
    // reached without any execve syscall returning -- the CLI's top-level
    // command and kernel/init.c's boot-command launcher both land here.
    native_exec_run_pending();

    struct task* save = current; // Because I kinda suspect that current gets messed up sometimes
    struct cpu_state *cpu = &save->cpu;
    struct tlb tlb = {};
    tlb_refresh(&tlb, &save->mem->mmu);
    
    while (true) {
        task_wait_for_mem_quiesce(save);
        read_lock(&save->mem->lock);

        int interrupt = cpu_run_to_interrupt(cpu, &tlb);

        read_unlock(&save->mem->lock);
        jit_cleanup_jetsam_after_interrupt(cpu);
 
        handle_interrupt(interrupt);
    }
}

static void *task_thread(void *task) {
    current = task;

    // The wake signals are blocked on entry (task_start created us that way).
    // Instantiate the thread-local storage sigusr1_handler relies on -- on this
    // normal call stack, where malloc is safe -- before unblocking them. The assignment
    // above instantiates `current`; this covers should_unwind / unwind_buf /
    // should_mark_wait_interrupted as well.
    signal_thread_locals_init();

    sigset_t wake_sigs;
    sigemptyset(&wake_sigs);
    sigaddset(&wake_sigs, SIGUSR1);
    sigaddset(&wake_sigs, SIGUSR2); // the backup poke, see util/sync.c
    pthread_sigmask(SIG_UNBLOCK, &wake_sigs, NULL);

    update_thread_name();
    
    task_run_current();
    die("task_thread returned"); // above function call should never return
    return NULL;
}

// A guest task's host stack has to be big enough for a NATIVE program, which
// runs as an ordinary C function on this thread rather than inside the
// emulator. Emulated code keeps its own recursion on the guest's stack and
// barely touches this one, so the 512 KB Darwin gives a non-main thread was
// never noticed -- until zsh and bash started running natively.
//
// Measured with native zsh: shell-function recursion costs ~3.5 KB of this
// stack per level, so 512 KB ran out at about 150 nested calls. zsh's own
// guard, FUNCNEST, defaults to 500 and is what makes `r() { r }; r` print
// "maximum nested function level reached" on a real zsh; here the C stack was
// exhausted first, and a native program is a function call on a thread of the
// APP, so the resulting SIGBUS took the whole app down (host exit 138) rather
// than one shell. `builtin() { builtin print x }; builtin` did the same thing
// in one line.
//
// So the stack is sized to fit the guard rather than the guard shrunk to fit
// the stack: 4 MB was measured to hold between 1250 and 1300 levels, i.e. two
// and a half times FUNCNEST's default, which leaves zsh's own guard to stop
// first and say so in zsh's own words. Shrinking FUNCNEST instead would have
// been a divergence from what the same script does off-device, and it would
// have fixed only zsh -- bash and every future native program share this
// thread. The cost is address space, not
// memory -- the pages are committed on demand, so a thread that never recurses
// still touches a few KB -- which is what makes this affordable to give to
// every guest task rather than only to the ones running native programs, and
// we cannot know which those are at creation time anyway.
#define TASK_THREAD_STACK_SIZE (4 * 1024 * 1024)

static pthread_attr_t task_thread_attr;
__attribute__((constructor)) static void create_attr(void) {
    pthread_attr_init(&task_thread_attr);
    pthread_attr_setdetachstate(&task_thread_attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&task_thread_attr, TASK_THREAD_STACK_SIZE);
#if defined(__APPLE__)
    // Run emulated guest threads one QoS band below the UI thread
    // (USER_INTERACTIVE). A multi-threaded guest workload spawns one OS thread
    // per emulated CPU (e.g. `go run`/`go build` with GOMAXPROCS = ncpu); left
    // at the default priority those threads are CPU-bound at the same band as
    // the main thread and starve the terminal/UI, making the app unresponsive
    // for the duration of the burst. USER_INITIATED still runs guest work
    // promptly on the performance cores but lets the UI preempt it.
    pthread_attr_set_qos_class_np(&task_thread_attr, QOS_CLASS_USER_INITIATED, 0);
#endif
}

int task_start(struct task *task) {
    // Create the thread with SIGUSR1 blocked so it cannot run sigusr1_handler
    // before task_thread has instantiated its thread-local storage (see
    // signal_thread_locals_init). Otherwise a sibling's TLB-shootdown poke
    // (task_poke_shared_mem -> pthread_kill(.., SIGUSR1)) could be delivered
    // while the new thread is mid-malloc instantiating that storage, making the
    // handler re-enter malloc and abort on the malloc lock. The new thread
    // inherits this mask and unblocks them itself once it is safe.
    sigset_t wake_sigs, oldmask;
    sigemptyset(&wake_sigs);
    sigaddset(&wake_sigs, SIGUSR1);
    sigaddset(&wake_sigs, SIGUSR2); // same reasoning, see util/sync.c
    pthread_sigmask(SIG_BLOCK, &wake_sigs, &oldmask);
    // Test knob: ISH_TEST_FAIL_TASK_START_AFTER=N makes every create after
    // the Nth fail as if the host were at its thread limit, so the unwind
    // path below can be regression-tested without a 16k-thread storm.
    static _Atomic int test_fail_after = -2; // -2 unparsed, -1 disabled
    if (atomic_load_explicit(&test_fail_after, memory_order_relaxed) == -2) {
        const char *env = getenv("ISH_TEST_FAIL_TASK_START_AFTER");
        atomic_store_explicit(&test_fail_after, env != NULL ? atoi(env) : -1,
                              memory_order_relaxed);
    }
    if (atomic_load_explicit(&test_fail_after, memory_order_relaxed) >= 0) {
        static _Atomic int started = 0;
        if (atomic_fetch_add_explicit(&started, 1, memory_order_relaxed) >=
                atomic_load_explicit(&test_fail_after, memory_order_relaxed)) {
            pthread_sigmask(SIG_SETMASK, &oldmask, NULL);
            return _EAGAIN;
        }
    }
    // pthread_create returns a POSITIVE errno on failure (EAGAIN at the host
    // thread limit). The old `< 0` check could never fire, so a failed create
    // was silently ignored: the fully-linked task had no host thread behind
    // it — a ghost that leaked its pid forever and wedged its thread group's
    // exit. Found via the bmt 10k-thread storm benchmark.
    int err = pthread_create(&task->thread, &task_thread_attr, task_thread, task);
    pthread_sigmask(SIG_SETMASK, &oldmask, NULL);
    if (err != 0)
        return _EAGAIN; // matches Linux clone() at the thread/rlimit ceiling
    // Only now does task->thread refer to this task's own host thread rather
    // than the parent's (copied in task_create_); let the per-CPU accounting
    // walker sample it. See host_thread_started in task.h.
    task->host_thread_started = true;
    return 0;
}

int_t sys_sched_yield(void) {
    STRACE("sched_yield()");
    sched_yield();
    return 0;
}

void update_thread_name(void) {
    char name[16]; // Maximum length for thread names in many systems, including Linux
    int result;

#ifdef __APPLE__
    // Never rename the main thread. The iOS app creates the visible
    // terminal's session by running do_execve (which lands here) with
    // `current` set ON the main thread, which stamped UIKit's main thread
    // with a guest name like "login-459" — making every crash report look
    // like a guest-thread crash. (CLI main-thread task loses its cosmetic
    // name too; top shows the process name regardless.)
    if (pthread_main_np())
        return;
#endif

    // Ensure that the name buffer is always null-terminated
    memset(name, 0, sizeof(name));

    // Create the thread name with PID
    //result = snprintf(name, sizeof(name) - 1, "%s-%d", current->comm, current->pid);
    result = snprintf(name, sizeof(name) - 1, "%.7s-%d", current->comm, current->pid);

    // Check if the output was truncated
    if (result >= (int)sizeof(name)) {
        // Handle truncation (e.g., by logging, adjusting the name format, etc.)
        // For this example, we just log a warning
        printk("WARNING: Thread name truncated in update_thread_name(%s).\n", name);
    }

#if __APPLE__
    pthread_setname_np(name);
#else
    pthread_setname_np(pthread_self(), name);
#endif
}

inline void modify_locks_held_count(struct task *task, int value) { // value should only be -1 or 1.
    if ((task == NULL) && (current != NULL)) {
        task = current;
    } else if (task == NULL) {
        return;
    }

    if (value != 1 && value != -1) {
        printk("ERROR: invalid locks_held delta %d for %s:%d\n",
               value, task->comm, task->pid);
        return;
    }

    // Only the task's own thread ever modifies its own locks_held count — every
    // caller passes current (lock()/unlock() in util/ro_locks.h, jit_crash_fn).
    // So this is single-writer and needs no CAS retry loop; one relaxed RMW is
    // correct. Concurrent readers (exit_wait_needed / task-teardown gating) use
    // relaxed atomic loads and only care whether the count is nonzero. This runs
    // on every lock()/unlock() — several times per guest syscall — so collapsing
    // the old load+CAS-loop to a single atomic add (an LSE `ldadd` on Apple
    // Silicon) measurably trims per-syscall lock overhead.
    int new_count = __atomic_add_fetch(&task->locks_held.count, value, __ATOMIC_RELAXED);

    if (new_count < 0) {
        // Unbalanced unlock (a bug): clamp back to zero rather than underflow.
        __atomic_store_n(&task->locks_held.count, 0, __ATOMIC_RELAXED);
        if (task->pid > 9)
            printk("ERROR: Attempt to decrement locks_held count below zero, ignoring\n");
    }
}

bool current_is_valid(void) {
    if(current != NULL)
        return true;
    
    return false;
}
