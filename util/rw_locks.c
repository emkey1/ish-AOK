//
//  rw_locks.c
//  iSH-AOK
//
//  Created by Michael Miller on 11/29/23.
//

#include "kernel/task.h"
#include "util/sync.h"

// The following are in log.c.  There should probably be in a log.h that gets included instead.
extern int current_pid(void);
extern int current_uid(void);
extern char* current_comm(void);
bool current_is_valid(void);

// this is a read-write lock that prefers writers, i.e. if there are any
// writers waiting a read lock will block.
// on darwin pthread_rwlock_t is already like this, on linux you can configure
// it to prefer writers. not worrying about anything else right now.

void loop_lock_generic(wrlock_t *lock, int is_write) {
            task_ref_cnt_mod_wrapper(1);
    modify_locks_held_count_wrapper(1);

    unsigned count = 0;
    int random_wait = is_write ? WAIT_SLEEP + rand() % 100 : WAIT_SLEEP + rand() % WAIT_SLEEP/4;
    struct timespec lock_pause = {0, random_wait};
    long count_max = (WAIT_MAX_UPPER - random_wait);
    count_max = (is_write && count_max < 25000) ? 25000 : count_max;

    while((is_write ? pthread_rwlock_trywrlock(&lock->l) : pthread_rwlock_tryrdlock(&lock->l))) {
        count++;
        if(count > count_max) {
            handle_lock_error(lock, is_write ? "loop_lock_write" : "loop_lock_read");
            count = 0;
        }
        atomic_l_unlockf();
        nanosleep(&lock_pause, NULL);
        atomic_l_lockf(is_write ? "llw\0" : "ll_read\0", 0);
    }

            task_ref_cnt_mod_wrapper(-1);
}



void _read_lock(wrlock_t *lock) {
    loop_lock_read(lock);
    task_ref_cnt_mod_wrapper(1);
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
                task_ref_cnt_mod_wrapper(-1);
        //STRACE("read_lock(%d, %s(%d), %s, %d\n", lock, lock->comm, lock->pid, file, line);
        return;
    }
    
    lock->pid = current_pid();
    if(lock->pid > 9)
        strncpy((char *)lock->comm, current_comm(), 16);
            task_ref_cnt_mod_wrapper(-1);
    //STRACE("read_lock(%d, %s(%d), %s, %d\n", lock, lock->comm, lock->pid, file, line);
}

void read_lock(wrlock_t *lock) { // Wrapper so that external calls lock, internal calls using _read_unlock() don't -mke
    atomic_l_lockf("r_lock\0", 0);
    _read_lock(lock);
    atomic_l_unlockf();
}

void _read_unlock(wrlock_t *lock) {
    if(lock->val <= 0) {
        printk("ERROR: read_unlock(%x) error(PID: %d Process: %s count %d) (%s:%d)\n",lock, current_pid(), current_comm(), lock->val);
        lock->val = 0;
        lock->pid = -1;
        lock->comm[0] = 0;
        modify_locks_held_count_wrapper(-1);
        //STRACE("read_unlock(%x, %s(%d), %s, %d\n", lock, lock->comm, lock->pid, file, line);
        return;
    }
    assert(lock->val > 0);
    if (pthread_rwlock_unlock(&lock->l) != 0)
        printk("URGENT: read_unlock(%x) error(PID: %d Process: %s)\n", lock, current_pid(), current_comm());
    lock->val--;
    modify_locks_held_count_wrapper(-1);
    //STRACE("read_unlock(%x, %s(%d), %s, %d\n", lock, lock->comm, lock->pid, file, line);
}

void read_unlock(wrlock_t *lock) {
    if(lock->pid != current_pid() && (lock->pid != -1)) {
        atomic_l_lockf("r_unlock\0", 0);
        _read_unlock(lock);
    } else { // We can unlock our own lock without additional locking.  -mke
        _read_unlock(lock);
        return;
    }
    if(lock->pid != current_pid() && (lock->pid != -1))
        atomic_l_unlockf();
}

void _write_unlock(wrlock_t *lock) {
    if(pthread_rwlock_unlock(&lock->l) != 0)
        printk("URGENT: write_unlock(%x:%d) error(PID: %d Process: %s) \n", lock, lock->val, current_pid(), current_comm());
    if(lock->val != -1) {
        printk("ERROR: write_unlock(%x) on lock with val of %d (PID: %d Process: %s )\n", lock, lock->val, current_pid(), current_comm());
    }
    //assert(lock->val == -1);
    lock->val = lock->line = lock->pid = 0;
    lock->pid = -1;
    lock->comm[0] = 0;
    //STRACE("write_unlock(%x, %s(%d), %s, %d\n", lock, lock->comm, lock->pid, file, line);
    lock->file = NULL;
    modify_locks_held_count_wrapper(-1);
}

void write_unlock(wrlock_t *lock) { // Wrap it.  External calls lock, internal calls using _write_unlock() don't -mke
    atomic_l_lockf("w_unlock\0", 0);
    _write_unlock(lock);
    atomic_l_unlockf();
    return;
}

void wrlock_init(wrlock_t *lock) {
    pthread_rwlockattr_t *pattr = NULL;
#if defined(__GLIBC__)
    pthread_rwlockattr_t attr;
    pattr = &attr;
    pthread_rwlockattr_init(pattr);
    pthread_rwlockattr_setkind_np(pattr, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
#endif
#ifdef JUSTLOG
    if (pthread_rwlock_init(&lock->l, pattr))
        printk("URGENT: wrlock_init() error(PID: %d Process: %s)\n",current_pid(), current_comm());
#else
    if (pthread_rwlock_init(&lock->l, pattr)) __builtin_trap();
#endif
    lock->val = lock->line = lock->pid = 0;
    lock->file = NULL;
}

void _lock_destroy(wrlock_t *lock) {
    while((task_ref_cnt_get(current) > 1) && (current_pid() != 1)) { // Wait for now, task is in one or more critical sections
        nanosleep(&lock_pause, NULL);
    }
#ifdef JUSTLOG
    if (pthread_rwlock_destroy(&lock->l) != 0) {
        printk("URGENT: lock_destroy(%x) on active lock. (PID: %d Process: %s Critical Region Count: %d)\n",&lock->l, current_pid(), current_comm(),task_ref_cnt_get(current));
    }
#else
    if (pthread_rwlock_destroy(&lock->l) != 0) __builtin_trap();
#endif
}

void lock_destroy(wrlock_t *lock) {
    while((task_ref_cnt_get(current) > 1) && (current_pid() != 1)) { // Wait for now, task is in one or more critical sections
        nanosleep(&lock_pause, NULL);
    }
    
    atomic_l_lockf("l_destroy\0", 0);
    _lock_destroy(lock);
    atomic_l_unlockf();
}

void _write_lock(wrlock_t *lock) { // Write lock
    loop_lock_write(lock);

    // assert(lock->val == 0);
    lock->val = -1;
  //  lock->file = file;
  //  lock->line = line;
    lock->pid = current_pid();
    if(lock->pid > 9)
        strncpy((char *)lock->comm, current_comm(), 16);
    //STRACE("write_lock(%x, %s(%d), %s, %d\n", lock, lock->comm, lock->pid, file, line);
}

void write_lock(wrlock_t *lock) {
    atomic_l_lockf("_w_lock", 0);
    _write_lock(lock);
    atomic_l_unlockf();
}

void handle_lock_error(wrlock_t *lock, const char *func) {
    printk("ERROR: %s(%x) tries exceeded %d, dealing with likely deadlock. (Lock held by PID: %d Process: %s) \n",
           func, lock, WAIT_MAX_UPPER, lock->pid, lock->comm);

    if(pid_get((dword_t)lock->pid) == NULL) {
        printk("ERROR: %s(%x) locking PID(%d) is gone for task %s\n", func, lock, lock->pid, lock->comm);
        pthread_rwlock_unlock(&lock->l);
    } else {
        printk("ERROR: %s(%x) locking PID(%d), %s is apparently wedged\n", func, lock, lock->pid, lock->comm);
        pthread_rwlock_unlock(&lock->l);
    }
    
    if(lock->val > 1) {
        lock->val--;
    } else if(lock->val == 1) {
        _read_unlock(lock);
    } else if(lock->val < 0) {
        _write_unlock(lock);
    }
}

void read_to_write_lock(wrlock_t *lock) {  // Try to atomically swap a RO lock to a Write lock.  -mke
            task_ref_cnt_mod_wrapper(1);
    atomic_l_lockf("rtw_lock\0", 0);
    _read_unlock(lock);
    _write_lock(lock);
    atomic_l_unlockf();
    task_ref_cnt_mod_wrapper(-1);
}

void write_to_read_lock(wrlock_t *lock) { // Try to atomically swap a Write lock to a RO lock.  -mke
            task_ref_cnt_mod_wrapper(1);
    atomic_l_lockf("wtr_lock\0", 0);
    _write_unlock(lock);
    _read_lock(lock);
    atomic_l_unlockf();
    task_ref_cnt_mod_wrapper(-1);
}

void write_unlock_and_destroy(wrlock_t *lock) {
            task_ref_cnt_mod_wrapper(1);
    atomic_l_lockf("wuad_lock\0", 0);
    _write_unlock(lock);
    _lock_destroy(lock);
    atomic_l_unlockf();
    task_ref_cnt_mod_wrapper(-1);
}

void read_unlock_and_destroy(wrlock_t *lock) {
    atomic_l_lockf("ruad_lock", 0);
    if(trylockw(lock)) // It should be locked, but just in case.  Likely masking underlying issue.  -mke
        _read_unlock(lock);
    _lock_destroy(lock);
    atomic_l_unlockf();
}

int trylockw(wrlock_t *lock) {
    atomic_l_lockf("trylockw\0", 0);
    int status = pthread_rwlock_trywrlock(&lock->l);
    atomic_l_unlockf();
#if LOCK_DEBUG
    if (!status) {
        lock->debug.file = file;
        lock->debug.line = line;
        extern int current_pid(void);
        lock->debug.pid = current_pid();
    }
#endif
    if(status == 0) {
        modify_locks_held_count_wrapper(1);
        //STRACE("trylockw(%x, %s(%d), %s, %d\n", lock, lock->comm, lock->pid, file, line);
        lock->pid = current_pid();
        strncpy(lock->comm, current_comm(), 16);
    }
    return status;
}

//#define trylockw(lock) trylockw(lock, __FILE__, __LINE__)

