#include <errno.h>
#include <limits.h>
#include "kernel/task.h"
#include "util/sync.h"
#include "debug.h"
#include "kernel/errno.h"
#include <string.h>

int noprintk = 0; // Used to suprress calls to printk.  -mke
extern bool doEnableExtraLocking;
extern pthread_mutex_t wait_for_lock; // Synchroniztion lock

void cond_init(cond_t *cond) {
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
#if __linux__
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
#endif
    pthread_cond_init(&cond->cond, &attr);
}
void cond_destroy(cond_t *cond) {
    pthread_cond_destroy(&cond->cond);
}

static bool is_signal_pending(lock_t *lock) {
    if (!current)
        return false;
    if (lock != &current->sighand->lock)
        lock(&current->sighand->lock, 0);
    bool pending = !!(current->pending & ~current->blocked);
    if (lock != &current->sighand->lock)
        unlock(&current->sighand->lock);
    return pending;
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

int task_ref_cnt_val(struct task *task) {
    pthread_mutex_lock(&task->reference.lock);
    int cnt = task->reference.count;
    pthread_mutex_unlock(&task->reference.lock);
    return cnt;
}

int task_ref_cnt_val_wrapper(void) {
    return(task_ref_cnt_val(current));
}

void mem_ref_cnt_mod(struct mem *mem, int value) { // value Should only be -1 or 1.  -mke
    // Keep track of how many threads are referencing this task
    if(!doEnableExtraLocking) {// If they want to fly by the seat of their pants...  -mke
        return;
    }
    
    if(mem == NULL) {
            return;
    }
    
    pthread_mutex_lock(&mem->reference.lock);
    
    if(((mem->reference.count + value) < 0)) { // Prevent our unsigned value attempting to go negative.  -mke
        printk("ERROR: Attempt to decrement mem reference count to be negative, ignoring(%d:%d)\n", mem->reference.count, value);
        pthread_mutex_unlock(&mem->reference.lock);
        
        return;
    }
    
    
    mem->reference.count = mem->reference.count + value;
        
    pthread_mutex_unlock(&mem->reference.lock);
}

int mem_ref_cnt_val(struct mem *mem) {
    pthread_mutex_lock(&mem->reference.lock);
    int cnt = mem->reference.count;
    pthread_mutex_unlock(&mem->reference.lock);
    return cnt;
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

void modify_locks_held_count_wrapper(int value) { // sync.h can't know about the definition of struct due to recursive include files.  -mke
    if(current != NULL)
        modify_locks_held_count(current, value);
    return;
}

int wait_for(cond_t *cond, lock_t *lock, struct timespec *timeout) {
    if (is_signal_pending(lock))
        return _EINTR;
    int err = wait_for_ignore_signals(cond, lock, timeout);
    if (err < 0)
        return _ETIMEDOUT;
    if (is_signal_pending(lock))
        return _EINTR;
    return 0;
}

int wait_for_ignore_signals(cond_t *cond, lock_t *lock, struct timespec *timeout) {
    if (current) {
        lock(&current->waiting_cond_lock, 0);
        current->waiting_cond = cond;
        current->waiting_lock = lock;
        unlock(&current->waiting_cond_lock);
    }
    int rc = 0;
    char saveme[16];
    strncpy(saveme, lock->lname, 16); // Save for later
#if LOCK_DEBUG
    struct lock_debug lock_tmp = lock->debug;
    lock->debug = (struct lock_debug) { .initialized = lock->debug.initialized };
#endif
    if (!timeout) { // We timeout anyway after fifteen seconds.  It appears the process wakes up briefly before returning here if there is nothing else pending.  This is kluge.  -mke
        struct timespec trigger_time;
        trigger_time.tv_sec = 15;
        trigger_time.tv_nsec = 0;
        lock->wait4 = true;
        
        if(current->uid == 501) {  // This is here for testing of the process lockup issue.  -mke
            rc = pthread_cond_timedwait_relative_np(&cond->cond, &lock->m, &trigger_time);
            //if((rc == ETIMEDOUT) && current->parent != NULL) {
            if(rc == ETIMEDOUT) {
                if(current->children.next != NULL) {
                    notify(cond);  // This is a terrible hack that seems to avoid processes getting stuck.
                }
            }
            
            rc = 0;
            
        } else {
            pthread_cond_wait(&cond->cond, &lock->m);
        }
    } else {
#if __linux__
        struct timespec abs_timeout;
        clock_gettime(CLOCK_MONOTONIC, &abs_timeout);
        abs_timeout.tv_sec += timeout->tv_sec;
        abs_timeout.tv_nsec += timeout->tv_nsec;
        if (abs_timeout.tv_nsec > 1000000000) {
            abs_timeout.tv_sec++;
            abs_timeout.tv_nsec -= 1000000000;
        }
        rc = pthread_cond_timedwait(&cond->cond, &lock->m, &abs_timeout);
#elif __APPLE__
        rc = pthread_cond_timedwait_relative_np(&cond->cond, &lock->m, timeout);
#else
#error Unimplemented pthread_cond_wait relative timeout.
#endif
    }
#if LOCK_DEBUG
    lock->debug = lock_tmp;
#endif

    if(current) {
        lock(&current->waiting_cond_lock, 0);
        current->waiting_cond = NULL;
        current->waiting_lock = NULL;
        unlock(&current->waiting_cond_lock);
    }
    lock->wait4 = false;
    if(rc == ETIMEDOUT)
        return _ETIMEDOUT;
    return 0;
}

void notify(cond_t *cond) {
    pthread_cond_broadcast(&cond->cond);
}
void notify_once(cond_t *cond) {
    pthread_cond_signal(&cond->cond);
}

__thread sigjmp_buf unwind_buf;
__thread bool should_unwind = false;

void sigusr1_handler(void) {
    if (should_unwind) {
        should_unwind = false;
        siglongjmp(unwind_buf, 1);
    }
}

// Because sometimes we can't #include "kernel/task.h" -mke
unsigned task_ref_cnt_mod(struct task *task) {
    unsigned tmp = 0;
    pthread_mutex_lock(&task->reference.lock); // This would make more
    tmp = task->reference.count;
    if(tmp > 1000)  // Work around brain damage.  Remove when said brain damage is fixed
        tmp = 0;
    pthread_mutex_unlock(&task->reference.lock);

    return tmp;
}

void task_ref_cnt_mod_wrapper(int, const char*, int) { // sync.h can't know about the definition of struct due to recursive include files.  -mke
    return(task_ref_count(current, ));
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

unsigned locks_held_count_wrapper(void) { // sync.h can't know about the definition of struct due to recursive include files.  -mke
    if(current != NULL)
        return(locks_held_count(current));
    return 0;
}


// This is how you would mitigate the unlock/wait race if the wait
// is async signal safe. wait_for *should* be safe from this race
// because of synchronization involving the waiting_cond_lock.
#if 0
    sigset_t sigusr1;
    sigemptyset(&sigusr1);
    sigaddset(&sigusr1, SIGUSR1);

    if (current) {
        if (sigsetjmp(unwind_buf, 1)) {
            return _EINTR;
        }
        should_unwind = true;
        sigprocmask(SIG_BLOCK, &sigusr1, NULL);
        if (lock != &current->sighand->lock)
            lock(&current->sighand->lock, 0);
        bool pending = !!(current->pending & ~current->blocked);
        if (lock != &current->sighand->lock)
            unlock(&current->sighand->lock);
        sigprocmask(SIG_UNBLOCK, &sigusr1, NULL);
        if (pending) {
            should_unwind = false;
            return _EINTR;
        }
    }
#endif

