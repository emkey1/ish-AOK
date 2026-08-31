//
//  ro_locks.h
//  iSH-AOK
//
//  Created by Michael Miller on 11/29/23.
//
#ifndef RO_LOCKS_H
#define RO_LOCKS_H

#include <strings.h>
#include <string.h>
#include "misc.h"
#include "util/lockstats.h"
#include "debug.h"
#include "kernel/errno.h"
#include "kernel/log.h"
#include "pthread.h"
#include <time.h>  // For timespec and clock_gettime

extern __thread struct task *current;

extern void modify_locks_held_count(struct task *task, int value);
extern void task_ref_cnt_mod(struct task *task, int value);

typedef struct {
    pthread_mutex_t m;            // Mutex for the lock
    pthread_cond_t cond;          // Condition variable for timeout
    pthread_t owner;              // Thread ID of the owner
    int pid;                      // Process ID of the owner
    int uid;                      // User ID of the owner
    char comm[16];                // Command name associated with the owner
    char lname[16];               // Name of the lock (for debugging/logging)
    bool wait4;                   // Flag to indicate if the lock is in use
    struct {
        pthread_mutex_t lock;     // Additional lock for reference management
        int count;                // Reference count
        bool ready_to_be_freed;   // Flag to indicate if the object is ready to be freed
    } reference;
#if LOCK_DEBUG
    struct lock_debug {
        const char *file;         // File where the lock was acquired (for debugging)
        int line;                 // Line number where the lock was acquired (for debugging)
        int pid;                  // Process ID when the lock was acquired (for debugging)
        bool initialized;         // Flag to indicate if the lock is initialized (for debugging)
    } debug;
#endif
} lock_t;

extern lock_t atomic_l_lock; // Used to make lock state transitions atomic.

void lock_init(lock_t *lock, char lname[16]);

// Locks initialized with LOCK_INITIALIZER have an all-zero lname; those get
// bucketed together, which is fine because the call site still separates them.
static inline const char *lock_group_name(lock_t *lock) {
    return lock->lname[0] != '\0' ? lock->lname : "lock";
}


static inline void unlock(lock_t *lock) {
    //pid_t pid = current_pid();
    // Sampled before the release, accounted after it: see util/lockstats.h.
    uint64_t _ls_t = lockstats_on ? lockstats_now() : 0;

    lock->owner = zero_init(pthread_t);
    lock->pid = -1; //
    lock->comm[0] = 0;
    
  /*  lock->wait4 = false;
    pthread_cond_signal(&lock->cond);
    pthread_mutex_unlock(&lock->m);
  */
    modify_locks_held_count(current, -1);
    pthread_mutex_unlock(&lock->m);
    if (lockstats_on)
        lockstats_account_named(lock, _ls_t, lock_group_name(lock));
    
#if LOCK_DEBUG
    assert(lock->debug.initialized);
    assert(lock->debug.file && "Attempting to unlock an unlocked lock");
    lock->debug = (struct lock_debug) { .initialized = true };
#endif
    return;
}

// Locks initialized with LOCK_INITIALIZER have an all-zero lname; those get
// bucketed together, which is fine because the call site still separates them.
// The instrumented form is a macro so the call site's function name can be
// baked in; mylock_at is what it expands to. See util/lockstats.h.
static inline void mylock_at(lock_t *lock, int log_lock, struct lock_site *site) {
    uint64_t _ls_t0 = (site != NULL) ? lockstats_now() : 0;
    pthread_mutex_lock(&lock->m);
    if (site != NULL)
        lockstats_held(site, lock, _ls_t0);
    if(!log_lock) {
        modify_locks_held_count(current, 1);
    }
    lock->owner = pthread_self();
    //lock->pid = current_pid(current);
    //lock->uid = current_uid(current);
    /* if(!log_lock) {
        strlcpy(lock->comm, current_comm(current), 16);
    } else {
        strncpy(lock->comm, current_comm(current), 16);
    } */
    return;
}

static inline void mylock(lock_t *lock, int log_lock) {
    mylock_at(lock, log_lock, NULL);
}

// Not a macro, so this one is attributed to itself rather than to its caller.
// It still has to push a frame: unlock() is instrumented and would otherwise
// count every release from here as unmatched.
static inline void complex_lockt(lock_t *lock, int log_lock) {
    uint64_t _ls_t0 = lockstats_on ? lockstats_now() : 0;
    struct timespec start = {};
    struct timespec end = {};
    bool contended = pthread_mutex_trylock(&lock->m) != 0;
    if (contended) {
        clock_gettime(CLOCK_MONOTONIC, &start);
        pthread_mutex_lock(&lock->m);
        clock_gettime(CLOCK_MONOTONIC, &end);
        if (!log_lock) {
            long waited_ms = (end.tv_sec - start.tv_sec) * 1000L +
                (end.tv_nsec - start.tv_nsec) / 1000000L;
            if (waited_ms >= 1000)
                printk("INFO: contended lock %s waited %ldms\n", lock->lname, waited_ms);
        }
    }

    modify_locks_held_count(current, 1);

    lock->owner = pthread_self();
    lock->comm[sizeof(lock->comm) - 1] = '\0';  // Null-terminate just in case
    if (lockstats_on)
        lockstats_held(lockstats_site(lock_group_name(lock), "complex_lockt"), lock, _ls_t0);
}

static inline int trylock(lock_t *lock) {
    int status = pthread_mutex_trylock(&lock->m);
#if LOCK_DEBUG
    if (!status) {
        lock->debug.file = file;
        lock->debug.line = line;
        extern int current_pid(struct task *task);
        lock->debug.pid = current_pid(current);
    }
#endif
    if (!status) {
        modify_locks_held_count(current, 1);
        lock->owner = pthread_self();
        lock->comm[sizeof(lock->comm) - 1] = '\0';
        // A successful trylock is an acquire like any other; without a frame
        // its unlock() shows up as an unmatched release and its hold time is
        // simply lost.
        if (lockstats_on)
            lockstats_held(lockstats_site(lock_group_name(lock), "trylock"),
                           lock, lockstats_now());
    }
    return status;
}

#define lock(l, log_lock) do {                                               \
    static struct lock_site *_ls_site;                                        \
    if (lockstats_on && _ls_site == NULL)                                     \
        _ls_site = lockstats_site(lock_group_name(l), __func__);              \
    mylock_at((l), (log_lock), lockstats_on ? _ls_site : NULL);               \
} while (0)

#endif
