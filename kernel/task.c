#define _GNU_SOURCE
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "kernel/calls.h"
#include "kernel/task.h"
#include "emu/memory.h"
#include "emu/tlb.h"
#include "platform/platform.h"
#include "util/sync.h"
#include <libkern/OSAtomic.h>
#include <os/proc.h>
#include <dlfcn.h>

#define GRACE_PERIOD 2 // How long we want to deallocate tasks that have exited

pthread_mutex_t multicore_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t extra_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t delay_lock = PTHREAD_MUTEX_INITIALIZER;
extern lock_t atomic_l_lock;
pthread_mutex_t wait_for_lock = PTHREAD_MUTEX_INITIALIZER;
time_t boot_time;  // Store the boot time.  -mke

struct list tasks_pending_deletion_queue;
pthread_mutex_t tasks_pending_deletion_lock = PTHREAD_MUTEX_INITIALIZER;

int iOSMajorRelease;

bool doEnableMulticore; // Enable multicore if toggled, should default to false
bool isGlibC = false; // Try to guess if we're running a non musl distro.  -mke
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

struct pid *pid_get_last_allocated(void) {
    if (!last_allocated_pid) {
        return NULL;
    }
    return pid_get(last_allocated_pid);
}

inline void task_ref_cnt_mod(struct task *task, int value) { // value Should only be -1 or 1.  -mke
    // Keep track of how many threads are referencing this task
    if(!doEnableExtraLocking) { // If they want to fly by the seat of their pants...  -mke
        return;
    }
    
    if(task == NULL) {
        if(current != NULL) {
            task = current;
        } else {
            return;
        }
    }

    pthread_mutex_lock(&task->reference.lock);
    
    if(((task->reference.count + value) < 0) && (task->pid > 9)) { // Prevent our unsigned value attempting to go negative.  -mke
        void *caller = __builtin_return_address(0);
        Dl_info caller_info = {};
        const char *caller_name = "?";
        ptrdiff_t caller_offset = 0;
        if (caller != NULL && dladdr(caller, &caller_info) != 0 && caller_info.dli_sname != NULL) {
            caller_name = caller_info.dli_sname;
            caller_offset = (char *) caller - (char *) caller_info.dli_saddr;
        }
        printk("ERROR: Attempt to decrement task reference count to be negative, ignoring(%s:%d) (%d - %d) caller=%s+%td addr=%p\n",
               task->comm, task->pid, task->reference.count, value, caller_name, caller_offset, caller);
        pthread_mutex_unlock(&task->reference.lock);
        return;
    }
    
    task->reference.count = task->reference.count + value;
    pthread_mutex_unlock(&task->reference.lock);
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

struct task *task_create_(struct task *parent) {
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

    struct task *task = malloc(sizeof(struct task));
    if (task == NULL) {
        unlock(&pids_lock);
        return NULL;
    }
    *task = (struct task) {};
    if (parent != NULL)
        *task = *parent;
    task->pid = pid->id;
    list_init(&task->group_links);
    list_init(&task->children);
    list_init(&task->siblings);
    task->pending = 0;
    task->waiting = 0;
    list_init(&task->queue);
    task->saved_mask = 0;
    task->has_saved_mask = false;
    task->clear_tid = 0;
    task->robust_list = 0;
    task->did_exec = false;
    task->exit_code = 0;
    task->zombie = false;
    task->exiting = false;
    task->io_block = false;
    task->vfork = NULL;
    task->exit_signal = 0;

    lock_init(&task->general_lock, "task_creat_gen\0");

    task->sockrestart = (struct task_sockrestart) {};
    list_init(&task->sockrestart.listen);

    task->waiting_cond = NULL;
    task->waiting_lock = NULL;
    lock_init(&task->waiting_cond_lock, "task_creat_wait\0");
    cond_init(&task->pause);

    task->ptrace = (typeof(task->ptrace)) {};
    lock_init(&task->ptrace.lock, "task_creat_ptr\0");
    cond_init(&task->ptrace.cond);

    task->locks_held.count = 0;
    pthread_mutex_init(&task->locks_held.lock, NULL);
    task->reference.count = 0;
    task->reference.ready_to_be_freed = false;
    pthread_mutex_init(&task->reference.lock, NULL);

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
    return task_ref_cnt_get(t, 0) > 1 || locks_held_count(t) || !!(t->pending & ~t->blocked);
}

void task_destroy(struct task *task, int caller) {
    task->exiting = true;
    
    //printk("TD(%s:%d): Called by %d\n", task->comm, task->pid, caller);
    
    // We use a single loop to wait for the task to be ready to destroy.
    // This loop replaces all the similar while-loops in the original code.
    int count = -4000; // Counter to limit the number of times we check.
    while (should_wait(task) && count < 0) {
        nanosleep(&lock_pause, NULL); // Sleep for a defined amount of time.
        count++;
    }
    
    // Remove the task from the sibling and alive lists.
    list_remove(&task->siblings);
    struct pid *pid = pid_get(task->pid);
    pid->task = NULL;
    list_remove(&pid->alive);
    
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
        free(task);
    }
}

// Cleanup function to delete tasks after the grace period
void cleanup_pending_deletions(void) {
    pthread_mutex_lock(&tasks_pending_deletion_lock);
    struct task_pending_deletion *pd, *tmp;
    list_for_each_entry_safe(&tasks_pending_deletion_queue, pd, tmp, list) {
        if ((difftime(time(NULL), pd->added_time) >= GRACE_PERIOD) && !! (!pd->task->reference.count)) { // Delete reaped tasks old and no longer referenced
            if (task_ref_cnt_get(pd->task, 0) == 0) {
                free(pd->task);
                list_remove(&pd->list);
                free(pd);
            }
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
    printk("iSH-AOK %s booted on %d emulated %s CPU(s)\n",uts.release, ncpu, uts.arch);
    // Get boot time
    extern time_t boot_time;
         
    boot_time = time(NULL);
    //printk("Seconds since January 1, 1970 = %ld\n", boot_time);
}

void task_run_current(void) {
    struct task* save = current; // Because I kinda suspect that current gets messed up sometimes
    struct cpu_state *cpu = &save->cpu;
    struct tlb tlb = {};
    tlb_refresh(&tlb, &save->mem->mmu);
    
    while (true) {
        read_lock(&save->mem->lock);

        int interrupt = cpu_run_to_interrupt(cpu, &tlb);

        read_unlock(&save->mem->lock);
 
        //struct timespec while_pause = {0 /*secs*/, WAIT_SLEEP /*nanosecs*/};
        if(save->parent != NULL) {
            save->parent->group->group_count_in_int++; // Keep track of how many children the parent has
            handle_interrupt(interrupt);
            save->parent->group->group_count_in_int--;
        } else {
            handle_interrupt(interrupt);
        }
    }
}

static void *task_thread(void *task) {
    
    current = task;
    
    update_thread_name();
    
    task_run_current();
    die("task_thread returned"); // above function call should never return
}

static pthread_attr_t task_thread_attr;
__attribute__((constructor)) static void create_attr(void) {
    pthread_attr_init(&task_thread_attr);
    pthread_attr_setdetachstate(&task_thread_attr, PTHREAD_CREATE_DETACHED);
}

void task_start(struct task *task) {
    if (pthread_create(&task->thread, &task_thread_attr, task_thread, task) < 0)
        die("could not create thread");
}

int_t sys_sched_yield(void) {
    STRACE("sched_yield()");
    sched_yield();
    return 0;
}

void update_thread_name(void) {
    char name[16]; // Maximum length for thread names in many systems, including Linux
    int result;

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

inline void modify_locks_held_count(struct task *task, int value) { // value Should only be -1 or 1.  -mke
    if ((task == NULL) && (current != NULL)) {
        task = current;
    } else if (task == NULL) {
        return;
    }
    
    pthread_mutex_lock(&task->locks_held.lock);
    if((task->locks_held.count + value < 0) && task->pid > 9) {
        printk("ERROR: Attempt to decrement locks_held count below zero, ignoring\n");
        pthread_mutex_unlock(&task->locks_held.lock);
        return;
    }
    task->locks_held.count = task->locks_held.count + value;
    pthread_mutex_unlock(&task->locks_held.lock);
}

bool current_is_valid(void) {
    if(current != NULL)
        return true;
    
    return false;
}
