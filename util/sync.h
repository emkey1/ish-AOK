#ifndef UTIL_SYNC_H
#define UTIL_SYNC_H
#define JUSTLOG 1

#include <errno.h>
#include <limits.h>
#include "util/ro_locks.h"
#include "util/rw_locks.h"
#include "debug.h"
#include "kernel/errno.h"
#include <string.h>
#include <setjmp.h>
#include "misc.h"

// locks, implemented using pthread

#define LOCK_DEBUG 0

extern inline void modify_locks_held_count(struct task *task, int value);


// The following is in task.c
extern struct pid *pid_get(dword_t id);

extern bool doEnableExtraLocking;

extern struct timespec lock_pause;

extern lock_t atomic_l_lock; // Used to make all lock operations atomic, even read->write and right->read -mke

#if LOCK_DEBUG
#define LOCK_INITIALIZER {PTHREAD_MUTEX_INITIALIZER, 0, { .initialized = true }}
#else
#define LOCK_INITIALIZER {PTHREAD_MUTEX_INITIALIZER, 0}
#endif

// conditions, implemented using pthread conditions but hacked so you can also
// be woken by a signal

typedef struct {
    pthread_cond_t cond;
    struct {
        pthread_mutex_t lock;
        int count; // If positive, don't delete yet, wait_to_delete
        bool ready_to_be_freed; // Should be false initially
    } reference;
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

void cond_init(cond_t *cond);
void cond_destroy(cond_t *cond);
//static bool is_signal_pending(lock_t *lock); // Not used externally to sync.c, doesn't eneed to be exposed
int wait_for(cond_t *cond, lock_t *lock, struct timespec *timeout);
int wait_for_ignore_signals(cond_t *cond, lock_t *lock, struct timespec *timeout);
void notify(cond_t *cond);
void notify_once(cond_t *cond);
void sigusr1_handler(int sig);
bool current_is_valid(void);

#endif
