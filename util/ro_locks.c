//
//  ro_locks.c
//  iSH-AOK
//
//  Created by Michael Miller on 11/29/23.
//

#include <strings.h>
#include "misc.h"
#include "debug.h"
#include "kernel/errno.h"
#include "kernel/task.h"
#include "util/sync.h"

// The following are in log.c.  There should probably be in a log.h that gets included instead.
extern int current_pid(void);
extern int current_uid(void);
extern char* current_comm(void);
bool current_is_valid(void);

// Lock to lock locks.  Used to assure transition between RO<->RW is automic for RW locks
lock_t atomic_l_lock;

// Function signatures and placeholders for implementation

void lock_init(lock_t *lock, char lname[16]) {
    int ret = pthread_mutex_init(&lock->m, NULL);
    if (ret != 0) {
        // Handle the error according to your application's needs
        printk("ERROR: Failed to initialize mutex: %s:(%s)\n", lname, strerror(ret));
        // Depending on how critical this failure is, you might choose to exit, return, or take other actions.
        return;
    }
    
    if(lname != NULL) {
        strncpy(lock->lname, lname, 16);
    } else {
        strncpy(lock->lname, "WTF", 16);
    }
    lock->wait4 = false;
#if LOCK_DEBUG
    lock->debug = (struct lock_debug) {
        .initialized = true,
    };
#endif
    lock->comm[0] = 0;
    lock->uid = -1;
}

void unlock(lock_t *lock) {
    //pid_t pid = current_pid();
    
    lock->owner = zero_init(pthread_t);
    pthread_mutex_unlock(&lock->m);
    lock->pid = -1; //
    lock->comm[0] = 0;
    modify_locks_held_count_wrapper(-1);
    
#if LOCK_DEBUG
    assert(lock->debug.initialized);
    assert(lock->debug.file && "Attempting to unlock an unlocked lock");
    lock->debug = (struct lock_debug) { .initialized = true };
#endif
    return;
}

void atomic_l_lockf(char lname[16], int skiplog) {
    if(!doEnableExtraLocking)
        return;
    int res = 0;
    if(atomic_l_lock.pid > 0) {
        if(current_pid() != atomic_l_lock.pid) { // Potential deadlock situation.  Also weird.  --mke
            res = pthread_mutex_lock(&atomic_l_lock.m);
            atomic_l_lock.pid = current_pid();
        } else if(!skiplog) {
            printk("WARNING: Odd attempt by process (%s:%d) to attain same locking lock twice.  Ignoring\n", current_comm(), current_pid());
            res = 0;
        }
    }
    if(!res) {
        strlcpy((char *)&atomic_l_lock.comm, current_comm(), 16);
        strlcpy((char *)&atomic_l_lock.lname, lname, 16);
        modify_locks_held_count_wrapper(1);
    } else if (!skiplog) {
        printk("Error on locking lock (%s) Called from %s:%d\n", lname);
    }
}

void mylock(lock_t *lock, int log_lock) {
    if(!strcmp(lock->lname, "task_creat_gen")) // kluge.  This means the lock is new, and SHOULD be unlocked
       unlock(lock);
    
    if(!log_lock) {
                task_ref_cnt_mod_wrapper(1);
        pthread_mutex_lock(&lock->m);
        modify_locks_held_count_wrapper(1);
        lock->owner = pthread_self();
        lock->pid = current_pid();
        lock->uid = current_uid();
        strlcpy(lock->comm, current_comm(), 16);
                task_ref_cnt_mod_wrapper(-1);
    } else {
        pthread_mutex_lock(&lock->m);
        lock->owner = pthread_self();
        lock->pid = current_pid();
        lock->uid = current_uid();
        strncpy(lock->comm, current_comm(), 16);
    }
    return;
}

void atomic_l_unlockf(void) {
    if(!doEnableExtraLocking)
        return;
    int res = 0;
    strncpy((char *)&atomic_l_lock.lname,"\0", 1);
    res = pthread_mutex_unlock(&atomic_l_lock.m);
    if(res) {
        printk("ERROR: unlocking locking lock\n");
    } else {
        atomic_l_lock.pid = -1; // Reset
    }
    
    modify_locks_held_count_wrapper(-1);
}

void complex_lockt(lock_t *lock, int log_lock) {
    if (lock->pid == current_pid())
        return;

    unsigned int count = 0;
    int random_wait = WAIT_SLEEP + rand() % WAIT_SLEEP;
    struct timespec lock_pause = {0, random_wait};
    long count_max = (WAIT_MAX_UPPER - random_wait);

    while (pthread_mutex_trylock(&lock->m)) {
        count++;
        if (nanosleep(&lock_pause, NULL) == -1) {
            // Handle error
        }
        if (count > count_max) {
            if (!log_lock) {
                printk("ERROR: Possible deadlock, aborted lock attempt(PID: %d Process: %s) (Previously Owned:%s:%d)\n",
                       current_pid(), current_comm(), lock->comm, lock->pid);
                pthread_mutex_unlock(&lock->m);
                modify_locks_held_count_wrapper(-1);
            }
            return;
        }
    }

    modify_locks_held_count_wrapper(1);

    if (count > count_max * 0.90) {
        if (!log_lock)
            printk("Warning: large lock attempt count (%d), aborted lock attempt(PID: %d Process: %s) (Previously Owned:%s:%d) \n",
                   count, current_pid(), current_comm(), lock->comm, lock->pid);
    }

    lock->owner = pthread_self();
    lock->pid = current_pid();
    lock->uid = current_uid();
    strncpy(lock->comm, current_comm(), sizeof(lock->comm) - 1);
    lock->comm[sizeof(lock->comm) - 1] = '\0';  // Null-terminate just in case
}

int trylock(lock_t *lock) {
    atomic_l_lockf("trylock\0", 0);
    int status = pthread_mutex_trylock(&lock->m);
    atomic_l_unlockf();
#if LOCK_DEBUG
    if (!status) {
        lock->debug.file = file;
        lock->debug.line = line;
        extern int current_pid(void);
        lock->debug.pid = current_pid();
    }
#endif
    if((!status) && (current_pid() > 10)) {// iSH-AOK crashes if low number processes are not excluded.  Might be able to go lower then 10?  -mke
        modify_locks_held_count_wrapper(1);
        
        //STRACE("trylock(%x, %s(%d), %s, %d\n", lock, lock->comm, lock->pid, file, line);
        lock->pid = current_pid();
        strncpy(lock->comm, current_comm(), 16);
    }
    return status;
}

int trylocknl(lock_t *lock, char *comm, int pid) {
    //Don't log, avoid recursion
    int status = pthread_mutex_trylock(&lock->m);
#if LOCK_DEBUG
    if (!status) {
        lock->debug.file = file;
        lock->debug.line = line;
        extern int current_pid(void);
        lock->debug.pid = current_pid();
    }
#endif
    if(!status) {// iSH-AOK crashes if low number processes are not excluded.  Might be able to go lower then 10?  -mke
        modify_locks_held_count_wrapper(1);
        
        //STRACE("trylock(%x, %s(%d), %s, %d\n", lock, lock->comm, lock->pid, file, line);
        lock->pid = pid;
        strncpy(lock->comm, comm, 16);
    }
    return status;
}

