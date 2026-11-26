//
//  rw_locks.c
//  AOK-Workstation
//
//  Created by Michael Miller on 11/29/23.
//

#include "kernel/task.h"
#include "util/sync.h"

// The following are in log.c.  There should probably be in a log.h that gets included instead.
bool current_is_valid(void);

// this is a read-write lock that prefers writers, i.e. if there are any
// writers waiting a read lock will block.
// on darwin pthread_rwlock_t is already like this, on linux you can configure
// it to prefer writers. not worrying about anything else right now.


void wrlock_init(wrlock_t *lock) {
    memset(lock, 0, sizeof(*lock));
    pthread_rwlockattr_t *pattr = NULL;
#if defined(__GLIBC__)
    pthread_rwlockattr_t attr;
    pattr = &attr;
    pthread_rwlockattr_init(pattr);
    pthread_rwlockattr_setkind_np(pattr, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
#endif
#ifdef JUSTLOG
    if (pthread_rwlock_init(&lock->l, pattr))
        printk("URGENT: wrlock_init() error(PID: %d Process: %s)\n",current_pid(current), current_comm(current));
#else
    if (pthread_rwlock_init(&lock->l, pattr)) __builtin_trap();
#endif
    // Hand-rolled reader/writer state (rw_locks.h) that the API now uses in
    // place of `.l`: the macOS psynch pthread_rwlock lost-wakeups when a
    // read->write upgrade races concurrent readers. `.l` is retained solely
    // for jit.c's jetsam_lock, which reaches into it directly.
    pthread_mutex_init(&lock->m, NULL);
    pthread_cond_init(&lock->c, NULL);
    lock->writers_waiting = 0;
    atomic_store_explicit(&lock->val, 0, memory_order_relaxed);
    lock->line = 0;
    lock->pid = -1;
    lock->file = NULL;
    lock->comm[0] = 0;
    lock->lname[0] = 0;
}

void _lock_destroy(wrlock_t *lock) {
    pthread_mutex_destroy(&lock->m);
    pthread_cond_destroy(&lock->c);
#ifdef JUSTLOG
    if (pthread_rwlock_destroy(&lock->l) != 0) {
        printk("URGENT: lock_destroy(%x) on active lock. (PID: %d Process: %s Critical Region Count: %d)\n",&lock->l, current_pid(current), current_comm(current),task_ref_cnt_get(current, 0));
    }
#else
    if (pthread_rwlock_destroy(&lock->l) != 0) __builtin_trap();
#endif
}

void lock_destroy(wrlock_t *lock) {
    while((task_ref_cnt_get(current, 0) > 1) && (current_pid(current) != 1)) { // Wait for now, task is in one or more critical sections
        nanosleep(&lock_pause, NULL);
    }
    
    _lock_destroy(lock);
}

//#define trylockw(lock) trylockw(lock, __FILE__, __LINE__)
