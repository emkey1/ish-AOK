//
//  rw_locks.h
//  iSH-AOK
//
//  Created by Michael Miller on 11/29/23.
//
#ifndef RW_LOCK_H
#define RW_LOCK_H

#include <strings.h>
#include "misc.h"
#include "debug.h"
#include "kernel/errno.h"
#include "kernel/log.h"
#include "pthread.h"
#include <pthread.h>
#include <stdatomic.h>

extern inline void modify_locks_held_count(struct task *task, int value);
extern inline void task_ref_cnt_mod(struct task *task, int value);

#define loop_lock_read(lock) loop_lock_generic(lock, 0)
#define loop_lock_write(lock) loop_lock_generic(lock, 1)

typedef struct {
    pthread_rwlock_t l;
    atomic_int val;
    int favor_read;
    const char *file;
    int line;
    int pid;
    char comm[16];
    char lname[16];
    struct {
        pthread_mutex_t lock;
        int count; // If positive, don't delete yet, wait_to_delete
        bool ready_to_be_freed; // Should be false initially
    } reference;
} wrlock_t;

void wrlock_init(wrlock_t *lock);
static inline void read_unlock_and_destroy(wrlock_t *lock);
static inline int trylockw(wrlock_t *lock);

extern void _lock_destroy(wrlock_t *lock);

static inline void _read_unlock(wrlock_t *lock) {
    if(lock->val <= 0) {
        //printk("ERROR: read_unlock(%x) error(PID: %d Process: %s count %d) (%s:%d)\n",lock, current_pid(current), current_comm(current), lock->val);
        printk("ERROR: read_unlock(%x) error(val: %d)\n",lock, lock->val);
        lock->val = 0;
        lock->pid = -1;
        lock->comm[0] = 0;
        //modify_locks_held_count(current, -1);
        //STRACE("read_unlock(%x, %s(%d), %s, %d\n", lock, lock->comm, lock->pid, file, line);
        return;
    }
    assert(lock->val > 0);
    if (pthread_rwlock_unlock(&lock->l) != 0)
//        printk("URGENT: read_unlock(%x) error(PID: %d Process: %s)\n", lock, current_pid(current), current_comm(current));
        printk("URGENT: read_unlock(%x) failed\n", lock);
    lock->val--;
    //modify_locks_held_count(current, -1);
    //STRACE("read_unlock(%x, %s(%d), %s, %d\n", lock, lock->comm, lock->pid, file, line);
}

static inline void read_unlock(wrlock_t *lock) {
  /*  if(lock->pid != current_pid(current) && (lock->pid != -1)) {
        atomic_l_lockf("r_unlock\0", 0);
        _read_unlock(lock);
    } else { */ // We can unlock our own lock without additional locking.  -mke
        _read_unlock(lock);
        return;
    //}
    //if(lock->pid != current_pid(current) && (lock->pid != -1))
    //    atomic_l_unlockf();
}

static inline void _write_unlock(wrlock_t *lock) {
    if(pthread_rwlock_unlock(&lock->l) != 0)
      //  printk("URGENT: write_unlock(%x:%d) error(PID: %d Process: %s) \n", lock, lock->val, current_pid(current), current_comm(current));
        printk("URGENT: write_unlock(%x:%d) error on unlock\n", lock, lock->val);
    if(lock->val != -1) {
        //printk("ERROR: write_unlock(%x) on lock with val of %d (PID: %d Process: %s )\n", lock, lock->val, current_pid(current), current_comm(current));
        // printk("ERROR: write_unlock(%x) on lock with val of %d\n", lock, lock->val);  // Comment out for now.  Much noise, little impact (So far as I can tell)
    }
    //assert(lock->val == -1);
    lock->val = lock->line = lock->pid = 0;
    lock->pid = -1;
    lock->comm[0] = 0;
    //STRACE("write_unlock(%x, %s(%d), %s, %d\n", lock, lock->comm, lock->pid, file, line);
    lock->file = NULL;
    //modify_locks_held_count(current, -1);
}

static inline void write_unlock(wrlock_t *lock) { // Wrap it.  External calls lock, internal calls using _write_unlock() don't -mke
    atomic_l_lockf("w_unlock\0", 0);
    _write_unlock(lock);
    atomic_l_unlockf();
    return;
}

static inline void loop_lock_generic(wrlock_t *lock, int is_write) {
    int random_wait = is_write ? WAIT_SLEEP + rand() % 100 : WAIT_SLEEP + rand() % WAIT_SLEEP/4;
    struct timespec lock_pause = {0, random_wait};

    while((is_write ? pthread_rwlock_trywrlock(&lock->l) : pthread_rwlock_tryrdlock(&lock->l))) {
        atomic_l_unlockf();
        nanosleep(&lock_pause, NULL);
        atomic_l_lockf(is_write ? "llw\0" : "ll_read\0", 0);
    }

}

static inline void _read_lock(wrlock_t *lock) {
    loop_lock_read(lock);
    //pthread_rwlock_rdlock(&lock->l);
    // assert(lock->val >= 0);  //  If it isn't >= zero we have a problem since that means there is a write lock somehow.  -mke
    if(lock->val) {
        lock->val++;
    } else if (lock->val > -1){  // Deal with insanity.  -mke
        lock->val++;
    } else {
        printk("ERROR: _read_lock() val is %d\n", lock->val);
        lock->val++;
    }
    
    if(lock->val > 1000) { // We likely have a problem.
        printk("WARNING: _read_lock(%x) has 1000+ pending read locks.  (File: %s, Line: %d) Breaking likely deadlock/process corruption(PID: %d Process: %s.\n", lock, lock->file, lock->line,lock->pid, lock->comm);
        read_unlock_and_destroy(lock);
        //STRACE("read_lock(%d, %s(%d), %s, %d\n", lock, lock->comm, lock->pid, file, line);
        return;
    }
    
    /* lock->pid = current_pid(current);
    if(lock->pid > 9)
        strncpy((char *)lock->comm, current_comm(current), 16); */
    //STRACE("read_lock(%d, %s(%d), %s, %d\n", lock, lock->comm, lock->pid, file, line);
}

static inline void read_lock(wrlock_t *lock) { // Wrapper so that external calls lock, internal calls using _read_unlock() don't -mke
    atomic_l_lockf("r_lock\0", 0);
    _read_lock(lock);
    atomic_l_unlockf();
}


static inline void _write_lock(wrlock_t *lock) { // Write lock
    loop_lock_write(lock);

    // assert(lock->val == 0);
    lock->val = -1;
  //  lock->file = file;
  //  lock->line = line;
   /* lock->pid = current_pid(current);
    if(lock->pid > 9)
        strncpy((char *)lock->comm, current_comm(current), 16); */
    //STRACE("write_lock(%x, %s(%d), %s, %d\n", lock, lock->comm, lock->pid, file, line);
}

static inline void write_lock(wrlock_t *lock) {
    atomic_l_lockf("_w_lock", 0);
    _write_lock(lock);
    atomic_l_unlockf();
}


static inline void read_to_write_lock(wrlock_t *lock) {  // Try to atomically swap a RO lock to a Write lock.  -mke
    atomic_l_lockf("rtw_lock\0", 0);
    _read_unlock(lock);
    _write_lock(lock);
    atomic_l_unlockf();
}

static inline void write_to_read_lock(wrlock_t *lock) { // Try to atomically swap a Write lock to a RO lock.  -mke
    atomic_l_lockf("wtr_lock\0", 0);
    _write_unlock(lock);
    _read_lock(lock);
    atomic_l_unlockf();
}

static inline void write_unlock_and_destroy(wrlock_t *lock) {
    atomic_l_lockf("wuad_lock\0", 0);
    _write_unlock(lock);
    _lock_destroy(lock);
    atomic_l_unlockf();
}

static inline void read_unlock_and_destroy(wrlock_t *lock) {
    atomic_l_lockf("ruad_lock", 0);
    if(trylockw(lock)) // It should be locked, but just in case.  Likely masking underlying issue.  -mke
        _read_unlock(lock);
    
    _lock_destroy(lock);
    atomic_l_unlockf();
}

static inline int trylockw(wrlock_t *lock) {
    atomic_l_lockf("trylockw\0", 0);
    int status = pthread_rwlock_trywrlock(&lock->l);
    atomic_l_unlockf();
#if LOCK_DEBUG
    if (!status) {
        lock->debug.file = file;
        lock->debug.line = line;
        extern int current_pid(current);
        lock->debug.pid = current_pid(current);
    }
#endif
    if(status == 0) {
        //modify_locks_held_count(current, 1);
        //STRACE("trylockw(%x, %s(%d), %s, %d\n", lock, lock->comm, lock->pid, file, line);
        //lock->pid = current_pid(current);
        //strncpy(lock->comm, current_comm(current), 16);
    }
    return status;
}

#endif // RW_LOCK_H
