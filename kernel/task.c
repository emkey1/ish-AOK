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

pthread_mutex_t multicore_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t extra_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t delay_lock = PTHREAD_MUTEX_INITIALIZER;
extern lock_t atomic_l_lock;
pthread_mutex_t wait_for_lock = PTHREAD_MUTEX_INITIALIZER;
time_t boot_time;  // Store the boot time.  -mke

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

dword_t get_count_of_blocked_tasks(void) {
    task_ref_cnt_mod(current, 1);
    dword_t res = 0;
    struct pid *pid_entry;
    complex_lockt(&pids_lock, 0);
    list_for_each_entry(&alive_pids_list, pid_entry, alive) {
        if (pid_entry->task->io_block) {
            res++;
        }
    }
    task_ref_cnt_mod(current, -1);
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
    pid->task = task;
    list_add(&alive_pids_list, &pid->alive);

    list_init(&task->children);
    list_init(&task->siblings);
    if (parent != NULL) {
        task->parent = parent;
        list_add(&parent->children, &task->siblings);
    }
    unlock(&pids_lock);

    task->pending = 0;
    list_init(&task->queue);
    task->clear_tid = 0;
    task->robust_list = 0;
    task->did_exec = false;
    lock_init(&task->general_lock, "task_creat_gen\0");

    task->sockrestart = (struct task_sockrestart) {};
    list_init(&task->sockrestart.listen);

    task->waiting_cond = NULL;
    task->waiting_lock = NULL;
    lock_init(&task->waiting_cond_lock, "task_creat_wait\0");
    cond_init(&task->pause);

    lock_init(&task->ptrace.lock, "task_creat_ptr\0");
    cond_init(&task->ptrace.cond);
    
    task->locks_held.count = 0; // counter used to keep track of pending locks associated with task.  Do not delete when locks are present.  -mke
    task->reference.count = 0; // counter used to delay task deletion if positive.  --mke
    task->reference.ready_to_be_freed = false;
    return task;
}

// We consolidate the check for whether the task is in a critical section,
// holds locks, or has pending signals into a single function.
bool should_wait(struct task *t) {
    return task_ref_cnt_get(t, 0) > 1 || locks_held_count(t) || !!(t->pending & ~t->blocked);
}

void task_destroy(struct task *task, int caller) {
    if(trylock(&task->general_lock) == (_EBUSY)) { // Get it if a lock does not exist
        task->exiting = true;
        lock(&task->general_lock, 0);
    }
    
    //printk("TD(%s:%d): Called by %d\n", task->comm, task->pid, caller);
    
    // We use a single loop to wait for the task to be ready to destroy.
    // This loop replaces all the similar while-loops in the original code.
    int count = -4000; // Counter to limit the number of times we check.
    while (should_wait(task) && count < 0) {
        nanosleep(&lock_pause, NULL); // Sleep for a defined amount of time.
        count++;
    }
    
    // Now we lock the pids_lock if it's not already locked by this task.
    // The trylock prevents deadlocks by avoiding locking if this thread already has the lock.
    bool locked_pids_lock = false;
    if (!trylock(&pids_lock)) {
        locked_pids_lock = true;
    }

    // Remove the task from the sibling and alive lists.
    list_remove(&task->siblings);
    struct pid *pid = pid_get(task->pid);
    pid->task = NULL;
    list_remove(&pid->alive);

    // Unlock pids_lock if we were the one who locked it.
    if (locked_pids_lock) {
        unlock(&pids_lock);
    }

retry:
    // Free the task's resources.
    if (!task_ref_cnt_get(task, 0)) {
        free(task);
    } else {
        goto retry;
    }
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
    struct cpu_state *cpu = &current->cpu;
    struct tlb tlb = {};
    tlb_refresh(&tlb, &current->mem->mmu);
    
    while (true) {
        read_lock(&current->mem->lock);
        
        if(!doEnableMulticore) {
            pthread_mutex_lock(&multicore_lock);
        }
        
        int interrupt = cpu_run_to_interrupt(cpu, &tlb);
        
        read_unlock(&current->mem->lock);
        
        if(!doEnableMulticore)
            pthread_mutex_unlock(&multicore_lock);
 
        //struct timespec while_pause = {0 /*secs*/, WAIT_SLEEP /*nanosecs*/};
        if(current->parent != NULL) {
            current->parent->group->group_count_in_int++; // Keep track of how many children the parent has
            handle_interrupt(interrupt);
            current->parent->group->group_count_in_int--;
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

void task_ref_cnt_mod(struct task *task, int value) { // value Should only be -1 or 1.  -mke
    // Keep track of how many threads are referencing this task
    if(!doEnableExtraLocking) {// If they want to fly by the seat of their pants...  -mke
        return;
    }
    
    if(task == NULL) {
        if(current != NULL) {
            task = current;
        } else {
            return;
        }
    }
    
    bool ilocked = false;
    
    if (trylocknl(&task->general_lock, task->comm, task->pid) != _EBUSY) {
        ilocked = true; // Make sure this is locked, and unlock it later if we had to lock it.
    }
    
    pthread_mutex_lock(&task->reference.lock);
    
    if(((task->reference.count + value) < 0) && (task->pid > 9)) { // Prevent our unsigned value attempting to go negative.  -mke
        printk("ERROR: Attempt to decrement task reference count to be negative, ignoring(%s:%d) (%d - %d)\n", task->comm, task->pid, task->reference.count, value);
        if(ilocked == true)
            unlock(&task->general_lock);
        
        pthread_mutex_unlock(&task->reference.lock);
        
        return;
    }
    
    
    task->reference.count = task->reference.count + value;
        
    pthread_mutex_unlock(&task->reference.lock);
    
    if(ilocked == true)
        unlock(&task->general_lock);
}

void task_ref_cnt_mod_wrapper(int value) {
    // sync.h can't know about the definition of task struct due to recursive include files.  -mke
    if((current != NULL) && (doEnableExtraLocking))
        task_ref_cnt_mod(current, value);
    
    return;
}

void modify_locks_held_count(struct task *task, int value) { // value Should only be -1 or 1.  -mke
    if((task == NULL) && (current != NULL)) {
        task = current;
    } else {
        return;
    }
    
    pthread_mutex_lock(&task->locks_held.lock);
    if((task->locks_held.count + value < 0) && task->pid > 9) {
     //  if((task->pid > 2) && (!strcmp(task->comm, "init")))  // Why ask why?  -mke
            printk("ERROR: Attempt to decrement locks_held count below zero, ignoring\n");
        return;
    }
    task->locks_held.count = task->locks_held.count + value;
    pthread_mutex_unlock(&task->locks_held.lock);
}

//
unsigned task_ref_cnt_get(struct task *task, unsigned lock_if_zero) {
    unsigned tmp = 0;
    pthread_mutex_lock(&task->reference.lock); // This would make more
    tmp = task->reference.count;
    if(tmp > 1000)  // Work around brain damage.  Remove when said brain damage is fixed
        tmp = 0;
    pthread_mutex_unlock(&task->reference.lock);

    return tmp;
}

bool current_is_valid(void) {
    if(current != NULL)
        return true;
    
    return false;
}

unsigned locks_held_count(struct task *task) {
   // return 0; // Short circuit for now
    if(task->pid < 10)  // Here be monsters.  -mke
        return 0;
    if(task->locks_held.count > 0) {
        return(task->locks_held.count -1);
    }
    unsigned tmp = 0;
    pthread_mutex_lock(&task->locks_held.lock);
    tmp = task->locks_held.count;
    pthread_mutex_unlock(&task->locks_held.lock);

    return tmp;
}

void modify_locks_held_count_wrapper(int value) { // sync.h can't know about the definition of struct due to recursive include files.  -mke
    if(current != NULL)
        modify_locks_held_count(current, value);
    return;
}
