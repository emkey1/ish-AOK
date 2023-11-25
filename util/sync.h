#ifndef UTIL_SYNC_H
#define UTIL_SYNC_H
#define JUSTLOG 1

#include <stdatomic.h>
#include <pthread.h>
#include <stdbool.h>
#include <setjmp.h>
#include<errno.h>
#include "misc.h"
#include "debug.h"
#include <strings.h>


// locks, implemented using pthread

#define LOCK_DEBUG 0

extern int current_pid(void);
extern int current_uid(void);
extern char* current_comm(void);
extern unsigned critical_region_count_wrapper(void);
extern void modify_critical_region_counter_wrapper(int, const char*, int);
extern unsigned locks_held_count_wrapper(void);
extern void modify_locks_held_count_wrapper(int);
extern struct pid *pid_get(dword_t id);
extern bool current_is_valid(void);

extern bool doEnableExtraLocking;

extern struct timespec lock_pause;


typedef struct {
    pthread_mutex_t m;
    pthread_t owner;
    //const char *comm;
    int pid;
    int uid;
    char comm[16];
    char lname[16];  // The name of the lock.  -mke
    bool wait4; // Is this lock in use by wait4
#if LOCK_DEBUG
    struct lock_debug {
        const char *file; // doubles as locked
        int line;
        int pid;
        bool initialized;
    } debug;
#endif
} lock_t;

extern lock_t atomic_l_lock; // Used to make all lock operations atomic, even read->write and right->read -mke

static inline void lock_init(lock_t *lock, char lname[16]) {
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

#if LOCK_DEBUG
#define LOCK_INITIALIZER {PTHREAD_MUTEX_INITIALIZER, 0, { .initialized = true }}
#else
#define LOCK_INITIALIZER {PTHREAD_MUTEX_INITIALIZER, 0}
#endif

static inline void atomic_l_lockf(char lname[16], const char *file, int line) {  // Make all locks atomic by wrapping them.  -mke
    if(!doEnableExtraLocking)
        return;
    int res = 0;
    modify_critical_region_counter_wrapper(1, file, line);
    if(atomic_l_lock.pid > 0) {
        if(current_pid() != atomic_l_lock.pid) { // Potential deadlock situation.  Also weird.  --mke
            res = pthread_mutex_lock(&atomic_l_lock.m);
            atomic_l_lock.pid = current_pid();
        } else {
            printk("WARNING: Odd attempt by process (%s:%d) to attain same locking lock twice.  Ignoring\n", current_comm(), current_pid());
            res = 0;
        }
    }
    if(!res) {
        strncpy((char *)&atomic_l_lock.comm, current_comm(), 16);
        strncpy((char *)&atomic_l_lock.lname, lname, 16);
        modify_locks_held_count_wrapper(1);
    } else {
        printk("Error on locking lock (%s) Called from %s:%d\n", lname, file, line);
    }
    
    //STRACE("atomic_l_lockf(%d)\n", count); // This is too verbose most of the time
    
    modify_critical_region_counter_wrapper(-1, file, line);
}

static inline void atomic_l_unlockf(void) {
    if(!doEnableExtraLocking)
        return;
    int res = 0;
    modify_critical_region_counter_wrapper(1, __FILE__, __LINE__);
    strncpy((char *)&atomic_l_lock.lname,"\0", 1);
    res = pthread_mutex_unlock(&atomic_l_lock.m);
    if(res) {
        printk("ERROR: unlocking locking lock\n");
    } else {
        atomic_l_lock.pid = -1; // Reset
    }
    
    modify_locks_held_count_wrapper(-1);
    modify_critical_region_counter_wrapper(-1, __FILE__, __LINE__);
    //STRACE("atomic_l_unlockf()\n");
  //  modify_critical_region_counter_wrapper(-1, __FILE__, __LINE__);
}

static inline void complex_lockt(lock_t *lock, int log_lock, __attribute__((unused)) const char *file, __attribute__((unused)) int line) {
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
                printk("ERROR: Possible deadlock, aborted lock attempt(PID: %d Process: %s) (Previously Owned:%s:%d) (Called By:%s:%d)\n",
                       current_pid(), current_comm(), lock->comm, lock->pid, file, line);
                pthread_mutex_unlock(&lock->m);
                modify_locks_held_count_wrapper(-1);
            }
            return;
        }
    }

    modify_locks_held_count_wrapper(1);

    if (count > count_max * 0.90) {
        if (!log_lock)
            printk("Warning: large lock attempt count (%d), aborted lock attempt(PID: %d Process: %s) (Previously Owned:%s:%d) (Called By:%s:%d)\n",
                   count, current_pid(), current_comm(), lock->comm, lock->pid, file, line);
    }

    lock->owner = pthread_self();
    //lock->pid = current_pid();
    //lock->uid = current_uid();
    //strncpy(lock->comm, current_comm(), sizeof(lock->comm) - 1);
    //lock->comm[sizeof(lock->comm) - 1] = '\0';  // Null-terminate just in case
}

static inline void __lock(lock_t *lock, int log_lock, __attribute__((unused)) const char *file, __attribute__((unused)) int line) {
    if(!log_lock) {
        modify_critical_region_counter_wrapper(1,__FILE__, __LINE__);
        pthread_mutex_lock(&lock->m);
        modify_locks_held_count_wrapper(1);
        lock->owner = pthread_self();
        //lock->pid = current_pid();
        //lock->uid = current_uid();
        //strncpy(lock->comm, current_comm(), 16);
        modify_critical_region_counter_wrapper(-1, __FILE__, __LINE__);
    } else {
        pthread_mutex_lock(&lock->m);
        lock->owner = pthread_self();
        //lock->pid = current_pid();
        //lock->uid = current_uid();
        //strncpy(lock->comm, current_comm(), 16);
    }
    return;
}

#define lock(lock, log_lock) __lock(lock, log_lock, __FILE__, __LINE__)

static inline void unlock(lock_t *lock) {
    //pid_t pid = current_pid();
    
 /*   if ((pthread_mutex_trylock(&lock->m) == 0) && (pid > 0)) {  // Sometimes pid = -1 when it shouldn't be, ignore
        printk("WARNING: Process with PID %d trying to unlock an already unlocked lock\n", pid);
        //pthread_mutex_unlock(&lock->m);  // unlock it again
        //return;
    }
    if ((lock->pid != pid) && (pid > 0) && (lock->pid > 0)) { // Sometimes pid = -1 when it shouldn't be, ignore
        printk("WARNING: Process with PID %d trying to unlock a lock owned by PID %d\n", pid, lock->pid);
        //return; // Return early or handle the discrepancy in another manner if required
    }
  */
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

typedef struct {
    pthread_rwlock_t l;
    // 0: unlocked
    // -1: write-locked
    // >0: read-locked with this many readers
    atomic_int val;
    int favor_read;  // Increment this up each time a write lock is gained, down when a read lock is gained
    const char *file;
    int line;
    int pid;
    char comm[16];
    char lname[16];
} wrlock_t;

static inline void _read_unlock(wrlock_t *lock, const char*, int);
static inline void _write_unlock(wrlock_t *lock, const char*, int);
static inline void write_unlock_and_destroy(wrlock_t *lock);

static inline void handle_lock_error(wrlock_t *lock, const char *file, int line, const char *func) {
    printk("ERROR: %s(%x) tries exceeded %d, dealing with likely deadlock. (Lock held by PID: %d Process: %s) (%s:%d)\n",
           func, lock, WAIT_MAX_UPPER, lock->pid, lock->comm, file, line);

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
        _read_unlock(lock, __FILE__, __LINE__);
    } else if(lock->val < 0) {
        _write_unlock(lock, __FILE__, __LINE__);
    }
}

static inline void loop_lock_generic(wrlock_t *lock, const char *file, int line, int is_write) {
    modify_critical_region_counter_wrapper(1, __FILE__, __LINE__);
    modify_locks_held_count_wrapper(1);

    unsigned count = 0;
    int random_wait = is_write ? WAIT_SLEEP + rand() % 100 : WAIT_SLEEP + rand() % WAIT_SLEEP/4;
    struct timespec lock_pause = {0, random_wait};
    long count_max = (WAIT_MAX_UPPER - random_wait);
    count_max = (is_write && count_max < 25000) ? 25000 : count_max;

    while((is_write ? pthread_rwlock_trywrlock(&lock->l) : pthread_rwlock_tryrdlock(&lock->l))) {
        count++;
        if(count > count_max) {
            handle_lock_error(lock, file, line, is_write ? "loop_lock_write" : "loop_lock_read");
            count = 0;
        }
        atomic_l_unlockf();
        nanosleep(&lock_pause, NULL);
        atomic_l_lockf(is_write ? "llw\0" : "ll_read\0", __FILE__, __LINE__);
    }

    modify_critical_region_counter_wrapper(-1, __FILE__, __LINE__);
}

#define loop_lock_read(lock, file, line) loop_lock_generic(lock, file, line, 0)
#define loop_lock_write(lock, file, line) loop_lock_generic(lock, file, line, 1)

static inline void _read_unlock(wrlock_t *lock, __attribute__((unused)) const char *file, __attribute__((unused)) int line) {
    if(lock->val <= 0) {
        printk("ERROR: read_unlock(%x) error(PID: %d Process: %s count %d) (%s:%d)\n",lock, current_pid(), current_comm(), lock->val, file, line);
        lock->val = 0;
        lock->pid = -1;
        lock->comm[0] = 0;
        modify_locks_held_count_wrapper(-1);
        //STRACE("read_unlock(%x, %s(%d), %s, %d\n", lock, lock->comm, lock->pid, file, line);
        return;
    }
    assert(lock->val > 0);
    if (pthread_rwlock_unlock(&lock->l) != 0)
        printk("URGENT: read_unlock(%x) error(PID: %d Process: %s) (%s:%d)\n", lock, current_pid(), current_comm(), file, line);
    lock->val--;
    modify_locks_held_count_wrapper(-1);
    //STRACE("read_unlock(%x, %s(%d), %s, %d\n", lock, lock->comm, lock->pid, file, line);
}

static inline void read_unlock(wrlock_t *lock, __attribute__((unused)) const char *file, __attribute__((unused)) int line) {
    if(lock->pid != current_pid() && (lock->pid != -1)) {
        atomic_l_lockf("r_unlock\0", __FILE__, __LINE__);
        _read_unlock(lock, file, line);
    } else { // We can unlock our own lock without additional locking.  -mke
        _read_unlock(lock, file, line);
        return;
    }
    if(lock->pid != current_pid() && (lock->pid != -1))
        atomic_l_unlockf();
}

static inline void _write_unlock(wrlock_t *lock, __attribute__((unused)) const char *file, __attribute__((unused)) int line) {
    if(pthread_rwlock_unlock(&lock->l) != 0)
        printk("URGENT: write_unlock(%x:%d) error(PID: %d Process: %s) (%s:%d)\n", lock, lock->val, current_pid(), current_comm(), file, line);
    if(lock->val != -1) {
        printk("ERROR: write_unlock(%x) on lock with val of %d (PID: %d Process: %s (%s:%d))\n", lock, lock->val, current_pid(), current_comm(), file, line);
    }
    //assert(lock->val == -1);
    lock->val = lock->line = lock->pid = 0;
    lock->pid = -1;
    lock->comm[0] = 0;
    //STRACE("write_unlock(%x, %s(%d), %s, %d\n", lock, lock->comm, lock->pid, file, line);
    lock->file = NULL;
    modify_locks_held_count_wrapper(-1);
}

static inline void write_unlock(wrlock_t *lock, __attribute__((unused)) const char *file, __attribute__((unused)) int line) { // Wrap it.  External calls lock, internal calls using _write_unlock() don't -mke
    atomic_l_lockf("w_unlock\0", __FILE__, __LINE__);
    _write_unlock(lock, file, line);
    atomic_l_unlockf();
    return;
}

static inline void __write_lock(wrlock_t *lock, const char *file, int line) { // Write lock
    loop_lock_write(lock, file, line);

    // assert(lock->val == 0);
    lock->val = -1;
    lock->file = file;
    lock->line = line;
    lock->pid = current_pid();
    if(lock->pid > 9)
        strncpy((char *)lock->comm, current_comm(), 16);
    //STRACE("write_lock(%x, %s(%d), %s, %d\n", lock, lock->comm, lock->pid, file, line);
}

static inline void _write_lock(wrlock_t *lock, const char *file, int line) {
    atomic_l_lockf("_w_lock", __FILE__, __LINE__);
    __write_lock(lock, file, line);
    atomic_l_unlockf();
}

static inline int trylockw(wrlock_t *lock, __attribute__((unused)) const char *file, __attribute__((unused)) int line) {
    //modify_critical_region_counter_wrapper(1,__FILE__, __LINE__);
    atomic_l_lockf("trylockw\0", __FILE__, __LINE__);
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

#define trylockw(lock) trylockw(lock, __FILE__, __LINE__)

static inline int trylock(lock_t *lock, __attribute__((unused)) const char *file, __attribute__((unused)) int line) {
    //modify_critical_region_counter_wrapper(1,__FILE__, __LINE__);
    atomic_l_lockf("trylock\0", __FILE__, __LINE__);
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

#define trylock(lock) trylock(lock, __FILE__, __LINE__)

// conditions, implemented using pthread conditions but hacked so you can also
// be woken by a signal

typedef struct {
    pthread_cond_t cond;
} cond_t;
#define COND_INITIALIZER ((cond_t) {PTHREAD_COND_INITIALIZER})

// Must call before using the condition
void cond_init(cond_t *cond);
// Must call when finished with the condition (currently doesn't do much but might do something important eventually I guess)
void cond_destroy(cond_t *cond);
// Releases the lock, waits for the condition, and reacquires the lock.
// Returns _EINTR if waiting stopped because the thread received a signal,
// _ETIMEDOUT if waiting stopped because the timout expired, 0 otherwise.
// Will never return _ETIMEDOUT if timeout is NULL.
int must_check wait_for(cond_t *cond, lock_t *lock, struct timespec *timeout);
// Same as wait_for, except it will never return _EINTR
int wait_for_ignore_signals(cond_t *cond, lock_t *lock, struct timespec *timeout);
// Wake up all waiters.
void notify(cond_t *cond);
// Wake up one waiter.
void notify_once(cond_t *cond);

static inline void read_to_write_lock(wrlock_t *lock);
static inline void read_unlock_and_destroy(wrlock_t *lock);

// this is a read-write lock that prefers writers, i.e. if there are any
// writers waiting a read lock will block.
// on darwin pthread_rwlock_t is already like this, on linux you can configure
// it to prefer writers. not worrying about anything else right now.

static inline void wrlock_init(wrlock_t *lock) {
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

static inline void _lock_destroy(wrlock_t *lock) {
    while((critical_region_count_wrapper() > 1) && (current_pid() != 1)) { // Wait for now, task is in one or more critical sections
        nanosleep(&lock_pause, NULL);
    }
#ifdef JUSTLOG
    if (pthread_rwlock_destroy(&lock->l) != 0) {
        printk("URGENT: lock_destroy(%x) on active lock. (PID: %d Process: %s Critical Region Count: %d)\n",&lock->l, current_pid(), current_comm(),critical_region_count_wrapper());
    }
#else
    if (pthread_rwlock_destroy(&lock->l) != 0) __builtin_trap();
#endif
}

static inline void lock_destroy(wrlock_t *lock) {
    while((critical_region_count_wrapper() > 1) && (current_pid() != 1)) { // Wait for now, task is in one or more critical sections
        nanosleep(&lock_pause, NULL);
    }
    
    atomic_l_lockf("l_destroy\0", __FILE__, __LINE__);
    _lock_destroy(lock);
    atomic_l_unlockf();
}

static inline void _read_lock(wrlock_t *lock, __attribute__((unused)) const char *file, __attribute__((unused)) int line) {
    loop_lock_read(lock, file, line);
    modify_critical_region_counter_wrapper(1, __FILE__, __LINE__);
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
        modify_critical_region_counter_wrapper(-1, __FILE__, __LINE__);
        //STRACE("read_lock(%d, %s(%d), %s, %d\n", lock, lock->comm, lock->pid, file, line);
        return;
    }
    
    lock->pid = current_pid();
    if(lock->pid > 9)
        strncpy((char *)lock->comm, current_comm(), 16);
    modify_critical_region_counter_wrapper(-1, __FILE__, __LINE__);
    //STRACE("read_lock(%d, %s(%d), %s, %d\n", lock, lock->comm, lock->pid, file, line);
}

static inline void read_lock(wrlock_t *lock, __attribute__((unused)) const char *file, __attribute__((unused)) int line) { // Wrapper so that external calls lock, internal calls using _read_unlock() don't -mke
    atomic_l_lockf("r_lock\0", __FILE__, __LINE__);
    _read_lock(lock, file, line);
    atomic_l_unlockf();
}

#define write_lock(lock) _write_lock(lock, __FILE__, __LINE__)

static inline void read_to_write_lock(wrlock_t *lock) {  // Try to atomically swap a RO lock to a Write lock.  -mke
    modify_critical_region_counter_wrapper(1, __FILE__, __LINE__);
    atomic_l_lockf("rtw_lock\0", __FILE__, __LINE__);
    _read_unlock(lock, __FILE__, __LINE__);
    __write_lock(lock, __FILE__, __LINE__);
    atomic_l_unlockf();
    modify_critical_region_counter_wrapper(-1, __FILE__, __LINE__);
}

static inline void write_to_read_lock(wrlock_t *lock, __attribute__((unused)) const char *file, __attribute__((unused)) int line) { // Try to atomically swap a Write lock to a RO lock.  -mke
    modify_critical_region_counter_wrapper(1, __FILE__, __LINE__);
    atomic_l_lockf("wtr_lock\0", __FILE__, __LINE__);
    _write_unlock(lock, file, line);
    _read_lock(lock, file, line);
    atomic_l_unlockf();
    modify_critical_region_counter_wrapper(-1, __FILE__, __LINE__);
}

static inline void write_unlock_and_destroy(wrlock_t *lock) {
    modify_critical_region_counter_wrapper(1, __FILE__, __LINE__);
    atomic_l_lockf("wuad_lock\0", __FILE__, __LINE__);
    _write_unlock(lock, __FILE__, __LINE__);
    _lock_destroy(lock);
    atomic_l_unlockf();
    modify_critical_region_counter_wrapper(-1, __FILE__, __LINE__);
}

static inline void read_unlock_and_destroy(wrlock_t *lock) {
    atomic_l_lockf("ruad_lock", __FILE__, __LINE__);
    if(trylockw(lock)) // It should be locked, but just in case.  Likely masking underlying issue.  -mke
        _read_unlock(lock, __FILE__, __LINE__);
    _lock_destroy(lock);
    atomic_l_unlockf();
}

extern __thread sigjmp_buf unwind_buf;
extern __thread bool should_unwind;
static inline int sigunwind_start(void) {
    if (sigsetjmp(unwind_buf, 1)) {
        should_unwind = false;
        return 1;
    } else {
        should_unwind = true;
        return 0;
    }
}

static inline void sigunwind_end(void) {
    should_unwind = false;
}

#endif
