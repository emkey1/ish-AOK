//
//  ro_locks.h
//  iSH-AOK
//
//  Created by Michael Miller on 11/29/23.
//
#ifndef RO_LOCKS_H
#define RO_LOCKS_H

#include <strings.h>
#include "misc.h"
#include "debug.h"
#include "kernel/errno.h"
#include "kernel/log.h"
#include "pthread.h"

extern void modify_locks_held_count_wrapper(int value);
extern void task_ref_cnt_mod_wrapper(int value);

typedef struct {
    pthread_mutex_t m;
    pthread_t owner;
    int pid;
    int uid;
    char comm[16];
    char lname[16];
    bool wait4;
    struct {
        pthread_mutex_t lock;
        int count; // If positive, don't delete yet, wait_to_delete
        bool ready_to_be_freed; // Should be false initially
    } reference;
#if LOCK_DEBUG
    struct lock_debug {
        const char *file;
        int line;
        int pid;
        bool initialized;
    } debug;
#endif
} lock_t;

extern lock_t atomic_l_lock; // Used to make all lock operations atomic, even read->write and right->read -mke
extern bool doEnableExtraLocking;

void lock_init(lock_t *lock, char lname[16]);

static inline void unlock(lock_t *lock) {
    //pid_t pid = current_pid();
    
    lock->owner = zero_init(pthread_t);
    lock->pid = -1; //
    lock->comm[0] = 0;
    modify_locks_held_count_wrapper(-1);
    pthread_mutex_unlock(&lock->m);
    
#if LOCK_DEBUG
    assert(lock->debug.initialized);
    assert(lock->debug.file && "Attempting to unlock an unlocked lock");
    lock->debug = (struct lock_debug) { .initialized = true };
#endif
    return;
}

static inline void atomic_l_lockf(char lname[16], int skiplog) {
    if(!doEnableExtraLocking)
        return;
    
    int res = 0;
    /* if(atomic_l_lock.pid > 0) {
        if(current_pid(current) != atomic_l_lock.pid) { // Potential deadlock situation.  Also weird.  --mke */
            res = pthread_mutex_lock(&atomic_l_lock.m);
     /*       atomic_l_lock.pid = current_pid(current);
        } else if(!skiplog) {
            printk("WARNING: Odd attempt by process (%s:%d) to attain same locking lock twice.  Ignoring\n", current_comm(current), current_pid(current));
            res = 0;
        }
    } */
    if(!res) {
      //  strlcpy((char *)&atomic_l_lock.comm, current_comm(current), 16);
        strlcpy((char *)&atomic_l_lock.lname, lname, 16);
        modify_locks_held_count_wrapper(1);
    } else if (!skiplog) {
        printk("Error on locking lock (%s) Called from %s:%d\n", lname);
    }
}

static inline void mylock(lock_t *lock, int log_lock) {
    // struct task *foo = current; // Debugging
    if(!strcmp(lock->lname, "task_creat_gen")) // kluge.  This means the lock is new, and SHOULD be unlocked
       unlock(lock);
    task_ref_cnt_mod_wrapper(1);
    pthread_mutex_lock(&lock->m);
    if(!log_lock) {
        modify_locks_held_count_wrapper(1);
    }
    lock->owner = pthread_self();
    //lock->pid = current_pid(current);
    //lock->uid = current_uid(current);
    /* if(!log_lock) {
        strlcpy(lock->comm, current_comm(current), 16);
    } else {
        strncpy(lock->comm, current_comm(current), 16);
    } */
    task_ref_cnt_mod_wrapper(-1);
    return;
}

static inline void atomic_l_unlockf(void) {
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

static inline void complex_lockt(lock_t *lock, int log_lock) {
    //if (lock->pid == pid)
    //    return;

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
               // printk("ERROR: Possible deadlock, aborted lock attempt(PID: %d Process: %s) (Previously Owned:%s:%d)\n",
               //        current_pid(current), current_comm(current), lock->comm, lock->pid);
                pthread_mutex_unlock(&lock->m);
                modify_locks_held_count_wrapper(-1);
            }
            return;
        }
    }

    modify_locks_held_count_wrapper(1);

  /*  if (count > count_max * 0.90) {
        if (!log_lock)
            printk("Warning: large lock attempt count (%d), aborted lock attempt(PID: %d Process: %s) (Previously Owned:%s:%d) \n",
                   count, current_pid(current), current_comm(current), lock->comm, lock->pid);
    } */

    lock->owner = pthread_self();
    //lock->pid = current_pid(current);
    //lock->uid = current_uid(current);
    // strncpy(lock->comm, current_comm(current), sizeof(lock->comm) - 1);
    lock->comm[sizeof(lock->comm) - 1] = '\0';  // Null-terminate just in case
}

static inline int trylock(lock_t *lock) {
    atomic_l_lockf("trylock\0", 0);
    int status = pthread_mutex_trylock(&lock->m);
    atomic_l_unlockf();
#if LOCK_DEBUG
    if (!status) {
        lock->debug.file = file;
        lock->debug.line = line;
        extern int current_pid(struct task *task);
        lock->debug.pid = current_pid(current);
    }
#endif
   // if((!status) && (current_pid(current) > 10)) {// iSH-AOK crashes if low number processes are not excluded.  Might be able to go lower then 10?  -mke
        modify_locks_held_count_wrapper(1);
        
        //STRACE("trylock(%x, %s(%d), %s, %d\n", lock, lock->comm, lock->pid, file, line);
    //    lock->pid = current_pid(current);
        //strncpy(lock->comm, current_comm(current), 16);
   // }
    return status;
}

static inline int trylocknl(lock_t *lock, char *comm, int pid) {
    //Don't log, avoid recursion
    int status = pthread_mutex_trylock(&lock->m);
#if LOCK_DEBUG
    if (!status) {
        lock->debug.file = file;
        lock->debug.line = line;
        extern int current_pid(current);
        lock->debug.pid = current_pid(current);
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


#define lock(lock, log_lock) mylock(lock, log_lock)
//#define trylock(lock) trylock(lock, __FILE__, __LINE__)
//#define trylocknl(lock, comm, pid) trylocknl(lock, comm, pid, __FILE__, __LINE__)

#endif
