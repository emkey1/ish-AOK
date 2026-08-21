//
//  rw_locks.h
//  iSH-AOK
//
//  Created by Michael Miller on 11/29/23.
//
#ifndef RW_LOCK_H
#define RW_LOCK_H

#include "util/lockstats.h"

#include <strings.h>
#include "misc.h"
#include "debug.h"
#include "kernel/errno.h"
#include "kernel/log.h"
#include "pthread.h"
#include <pthread.h>
#include <stdatomic.h>

extern void modify_locks_held_count(struct task *task, int value);
extern void task_ref_cnt_mod(struct task *task, int value);

#define loop_lock_read(lock) loop_lock_generic(lock, 0)
#define loop_lock_write(lock) loop_lock_generic(lock, 1)

typedef struct {
    // Hand-rolled reader/writer state guarded by `m`, replacing the raw
    // pthread_rwlock the API used to wrap. macOS's psynch pthread_rwlock has
    // a lost-wakeup: when a thread does unlock-then-wrlock (read_to_write_lock)
    // or a blocking wrlock races concurrent rdlock, it can wedge with the lock
    // logically unowned (val==0) yet a writer and several readers all asleep
    // forever (reproduced under cargo's fork+signal storm). A plain
    // mutex+condvar cannot: every state change broadcasts under `m` and every
    // waiter re-tests its predicate on wake.
    pthread_mutex_t m;
    pthread_cond_t c;
    // val: >0 = that many active readers, 0 = free, -1 = a writer holds it.
    // Mutated only under `m`; kept atomic so the lock-free debug/lldb reads of
    // it elsewhere stay well-defined.
    atomic_int val;
    int writers_waiting;   // writer preference: new readers yield to these
    // Legacy raw rwlock, retained ONLY for jit.c's jetsam_lock, which reaches
    // into `.l` directly on its per-block hot path (rdlock + trywrlock, never
    // a blocking wrlock, so it never hits the lost-wakeup). No lock instance
    // mixes `.l` with the mutex+condvar API above.
    pthread_rwlock_t l;
    int favor_read;
    const char *file;
    int line;
    int pid;
    char comm[16];
    char lname[16];
    void *last_read_lock_pc;
    void *last_read_unlock_pc;
    const char *last_read_lock_file;
    int last_read_lock_line;
    const char *last_read_unlock_file;
    int last_read_unlock_line;
    struct {
        pthread_mutex_t lock;
        int count; // If positive, don't delete yet, wait_to_delete
        bool ready_to_be_freed; // Should be false initially
    } reference;
} wrlock_t;

void wrlock_init(wrlock_t *lock);
static inline int trylockw(wrlock_t *lock);

extern void _lock_destroy(wrlock_t *lock);

// Read and write acquisitions of the same rwlock are separate groups: the read
// side is shared (its duty cycle can legitimately exceed 100% -- that is the
// point of a shared lock) while the write side is the barrier. Built on the
// cold interning path only.
static inline struct lock_site *rw_site(wrlock_t *lock, const char *fn, int is_write) {
    char group[24];
    snprintf(group, sizeof(group), "%s-%s",
             lock->lname[0] ? lock->lname : "rwlock", is_write ? "wr" : "rd");
    return lockstats_site(group, fn);
}


static inline void _read_unlock(wrlock_t *lock, const char *file, int line) {
    pthread_mutex_lock(&lock->m);
    int old_val = atomic_load_explicit(&lock->val, memory_order_relaxed);
    if (old_val <= 0) {
        // Unbalanced read_unlock: we evidently do not hold a read lock. Do
        // not touch val (a -1 writer or a 0 free state must be left intact),
        // just report — same defensive stance as the original.
        printk("ERROR: read_unlock(%x) error(val: %d lock=%s holder=%s(%d) at %s:%d)\n",
               lock,
               old_val,
               lock->lname[0] ? lock->lname : "-",
               lock->comm[0] ? lock->comm : "-",
               lock->pid,
               lock->file != NULL ? lock->file : "-",
               lock->line);
        printk("ERROR: read_unlock(%x) pcs last_read_lock=%p last_read_unlock=%p current=%s:%d last_lock=%s:%d last_unlock=%s:%d\n",
               lock, lock->last_read_lock_pc, lock->last_read_unlock_pc,
               file != NULL ? file : "-", line,
               lock->last_read_lock_file != NULL ? lock->last_read_lock_file : "-",
               lock->last_read_lock_line,
               lock->last_read_unlock_file != NULL ? lock->last_read_unlock_file : "-",
               lock->last_read_unlock_line);
        pthread_mutex_unlock(&lock->m);
        return;
    }
    atomic_store_explicit(&lock->val, old_val - 1, memory_order_relaxed);
#if LOCK_DEBUG
    lock->last_read_unlock_pc = __builtin_return_address(0);
    lock->last_read_unlock_file = file;
    lock->last_read_unlock_line = line;
#else
    (void) file;
    (void) line;
#endif
    if (old_val - 1 == 0)
        pthread_cond_broadcast(&lock->c);  // last reader out: wake any writer
    pthread_mutex_unlock(&lock->m);
}

#define read_unlock(l) do {                                                  \
    uint64_t _ls_t = lockstats_on ? lockstats_now() : 0;                      \
    _read_unlock((l), __FILE__, __LINE__);                                    \
    if (lockstats_on)                                                         \
        lockstats_account_named((l), _ls_t,                                   \
                (l)->lname[0] ? (l)->lname : "rwlock");                       \
} while (0)

static inline void _write_unlock(wrlock_t *lock) {
    pthread_mutex_lock(&lock->m);
    if (atomic_load_explicit(&lock->val, memory_order_relaxed) != -1)
        printk("URGENT: write_unlock(%x) not write-held (val=%d)\n",
               lock, atomic_load_explicit(&lock->val, memory_order_relaxed));
    atomic_store_explicit(&lock->val, 0, memory_order_relaxed);
    lock->line = 0;
    lock->pid = -1;
    lock->comm[0] = 0;
    lock->file = NULL;
    // Wake everyone: a queued writer or any number of queued readers may now
    // proceed (writer preference is enforced in the waiters' predicates).
    pthread_cond_broadcast(&lock->c);
    pthread_mutex_unlock(&lock->m);
}

#define write_unlock(l) do {                                                 \
    uint64_t _ls_t = lockstats_on ? lockstats_now() : 0;                      \
    _write_unlock((l));                                                       \
    if (lockstats_on)                                                         \
        lockstats_account_named((l), _ls_t,                                   \
                (l)->lname[0] ? (l)->lname : "rwlock");                       \
} while (0)

// Blocking acquire under the hand-rolled state machine. Writer-preferring
// (matches the old Darwin PTHREAD_RWLOCK_PREFER_WRITER / writer-intent
// semantics the quiesce protocol was designed around): a reader waits while a
// writer holds OR any writer is queued; a writer waits until the lock is idle
// (val==0). Must be called with `lock->m` held; returns with it held and the
// count updated.
static inline void wrlock_acquire_locked(wrlock_t *lock, int is_write) {
    if (is_write) {
        lock->writers_waiting++;
        while (atomic_load_explicit(&lock->val, memory_order_relaxed) != 0)
            pthread_cond_wait(&lock->c, &lock->m);
        lock->writers_waiting--;
        atomic_store_explicit(&lock->val, -1, memory_order_relaxed);
    } else {
        while (atomic_load_explicit(&lock->val, memory_order_relaxed) < 0 ||
               lock->writers_waiting > 0)
            pthread_cond_wait(&lock->c, &lock->m);
        atomic_fetch_add_explicit(&lock->val, 1, memory_order_relaxed);
    }
}

static inline void _read_lock(wrlock_t *lock, const char *file, int line) {
    pthread_mutex_lock(&lock->m);
    wrlock_acquire_locked(lock, 0);
    int new_val = atomic_load_explicit(&lock->val, memory_order_relaxed);
    if (new_val > 1000 && (new_val == 1001 || (new_val % 256) == 0)) { // We likely have a problem.
        printk("WARNING: _read_lock(%x lock=%s) has %d active readers. holder=%s(%d) at %s:%d current=%s(%d)\n",
               lock,
               lock->lname[0] ? lock->lname : "-",
               new_val,
               lock->comm[0] ? lock->comm : "-",
               lock->pid,
               lock->file != NULL ? lock->file : "-",
               lock->line,
               "-",
               -1);
    }
#if LOCK_DEBUG
    lock->last_read_lock_pc = __builtin_return_address(0);
    lock->last_read_lock_file = file;
    lock->last_read_lock_line = line;
#else
    (void) file;
    (void) line;
#endif
    pthread_mutex_unlock(&lock->m);
}

#define read_lock(l) do {                                                    \
    static struct lock_site *_ls_site;                                        \
    uint64_t _ls_t0 = lockstats_on ? lockstats_now() : 0;                     \
    _read_lock((l), __FILE__, __LINE__);                                      \
    if (lockstats_on) {                                                       \
        if (_ls_site == NULL)                                                 \
            _ls_site = rw_site((l), __func__, 0);                             \
        lockstats_held(_ls_site, (l), _ls_t0);                                \
    }                                                                         \
} while (0)


static inline void _write_lock(wrlock_t *lock) { // Write lock
    pthread_mutex_lock(&lock->m);
    wrlock_acquire_locked(lock, 1);
    pthread_mutex_unlock(&lock->m);
}

#define write_lock(l) do {                                                   \
    static struct lock_site *_ls_site;                                        \
    uint64_t _ls_t0 = lockstats_on ? lockstats_now() : 0;                     \
    _write_lock((l));                                                         \
    if (lockstats_on) {                                                       \
        if (_ls_site == NULL)                                                 \
            _ls_site = rw_site((l), __func__, 1);                             \
        lockstats_held(_ls_site, (l), _ls_t0);                                \
    }                                                                         \
} while (0)


static inline void read_to_write_lock(wrlock_t *lock) {  // Atomically swap a read lock to a write lock.
    // The old drop-read-then-blocking-wrlock is exactly the macOS lost-wakeup
    // trigger. Under `m` this is a single critical section: drop our reader,
    // register write intent, wait for the remaining readers to drain, take the
    // write. No window where another thread can wedge the psynch state.
    pthread_mutex_lock(&lock->m);
    int old_val = atomic_load_explicit(&lock->val, memory_order_relaxed);
    if (old_val > 0)
        atomic_store_explicit(&lock->val, old_val - 1, memory_order_relaxed); // release our read
    lock->writers_waiting++;
    while (atomic_load_explicit(&lock->val, memory_order_relaxed) != 0)
        pthread_cond_wait(&lock->c, &lock->m);
    lock->writers_waiting--;
    atomic_store_explicit(&lock->val, -1, memory_order_relaxed);
    pthread_mutex_unlock(&lock->m);
}

static inline void write_to_read_lock(wrlock_t *lock) { // Atomically swap a write lock to a read lock.
    pthread_mutex_lock(&lock->m);
    atomic_store_explicit(&lock->val, 1, memory_order_relaxed);
    pthread_cond_broadcast(&lock->c);  // queued readers may proceed now
    pthread_mutex_unlock(&lock->m);
}

static inline void write_unlock_and_destroy(wrlock_t *lock) {
    // Accounts before the destroy: this is a release like any other, and
    // skipping it leaks the frame permanently (mem_destroy is the one caller,
    // so every address-space teardown used to leave one behind until the
    // thread's frame stack filled and every later sample was dropped).
    uint64_t _ls_t = lockstats_on ? lockstats_now() : 0;
    _write_unlock(lock);
    if (lockstats_on)
        lockstats_account_named(lock, _ls_t, lock->lname[0] ? lock->lname : "rwlock");
    _lock_destroy(lock);
}

// Non-blocking write acquire. Returns 0 on success, EBUSY otherwise —
// matching pthread_rwlock_trywrlock, which is what mem_write_lock_with_pokes
// loops on. Succeeds only when the lock is fully idle (no readers, no writer).
static inline int trylockw(wrlock_t *lock) {
    pthread_mutex_lock(&lock->m);
    if (atomic_load_explicit(&lock->val, memory_order_relaxed) != 0) {
        pthread_mutex_unlock(&lock->m);
        return _EBUSY;
    }
    atomic_store_explicit(&lock->val, -1, memory_order_relaxed);
    pthread_mutex_unlock(&lock->m);
    if (lockstats_on)
        lockstats_held(rw_site(lock, "trylockw", 1), lock, lockstats_now());
    return 0;
}

static inline int _trylockr(wrlock_t *lock, const char *file, int line) {
    pthread_mutex_lock(&lock->m);
    // Writer-preferring: fail if a writer holds OR is queued, same as the
    // blocking read path's predicate.
    if (atomic_load_explicit(&lock->val, memory_order_relaxed) < 0 ||
            lock->writers_waiting > 0) {
        pthread_mutex_unlock(&lock->m);
        return _EBUSY;
    }
    atomic_fetch_add_explicit(&lock->val, 1, memory_order_relaxed);
#if LOCK_DEBUG
    lock->last_read_lock_pc = __builtin_return_address(0);
    lock->last_read_lock_file = file;
    lock->last_read_lock_line = line;
#else
    (void) file;
    (void) line;
#endif
    pthread_mutex_unlock(&lock->m);
    if (lockstats_on)
        lockstats_held(rw_site(lock, "trylockr", 0), lock, lockstats_now());
    return 0;
}

#define trylockr(lock) _trylockr(lock, __FILE__, __LINE__)

#endif // RW_LOCK_H
