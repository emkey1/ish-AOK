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
#include <unistd.h>
#include "misc.h"

// locks, implemented using pthread

#define LOCK_DEBUG 0

extern void modify_locks_held_count(struct task *task, int value);


// The following is in task.c
extern struct pid *pid_get(dword_t id);

extern struct timespec lock_pause;

extern lock_t atomic_l_lock; // Used to make lock state transitions atomic.

#if LOCK_DEBUG
#define LOCK_INITIALIZER { \
    .m = PTHREAD_MUTEX_INITIALIZER, \
    .cond = PTHREAD_COND_INITIALIZER, \
    .pid = -1, \
    .reference = { .lock = PTHREAD_MUTEX_INITIALIZER }, \
    .debug = { .initialized = true }, \
}
#else
#define LOCK_INITIALIZER { \
    .m = PTHREAD_MUTEX_INITIALIZER, \
    .cond = PTHREAD_COND_INITIALIZER, \
    .pid = -1, \
    .reference = { .lock = PTHREAD_MUTEX_INITIALIZER }, \
}

// Same, but carrying a name. Statically initialized locks otherwise have an
// empty lname, which lockstats groups together as plain "lock" -- worth
// spending on the ones whose contention anyone ever asks about.
#define LOCK_INITIALIZER_NAMED(lock_name) { \
    .m = PTHREAD_MUTEX_INITIALIZER, \
    .cond = PTHREAD_COND_INITIALIZER, \
    .pid = -1, \
    .reference = { .lock = PTHREAD_MUTEX_INITIALIZER }, \
    .lname = lock_name, \
}
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

#define COND_INITIALIZER ((cond_t) { \
    .cond = PTHREAD_COND_INITIALIZER, \
    .reference = { .lock = PTHREAD_MUTEX_INITIALIZER }, \
})

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
extern __thread bool should_mark_wait_interrupted;
// sigsetjmp MUST be called in the same stack frame as siglongjmp's target.
// A helper function won't work: the compiler refuses to inline functions
// containing sigsetjmp, so the jmp_buf would point at a dead frame.
// Use a macro to guarantee in-place expansion at every call site.
#define sigunwind_start() \
    ({ \
        int __sigunwind_result; \
        /* volatile: read back on the siglongjmp branch, so it must survive */ \
        volatile unsigned __sigunwind_frames = lockstats_depth; \
        if (sigsetjmp(unwind_buf, 1)) { \
            should_unwind = false; \
            /* A siglongjmp abandons every lockstats frame opened since the \
               setjmp -- nothing releases those locks through the hooks, so \
               without this the stack grows until it is full and every later \
               sample is dropped. It also made pids_lock report a 285% duty \
               cycle on an exclusive mutex: abandoned frames from many \
               threads overlapped in the totals. */ \
            lockstats_depth = __sigunwind_frames; \
            __sigunwind_result = 1; \
        } else { \
            should_unwind = true; \
            __sigunwind_result = 0; \
        } \
        __sigunwind_result; \
    })

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
// True only when ISH_TRACE_WAITS is set in the environment; gates the hot-path
// "INFO: wait" short-wait traces in kernel/time.c and kernel/poll.c. See sync.c.
bool wait_trace_enabled(void);
void sigusr1_handler(int sig);
// The backup wake poke, sent alongside SIGUSR1. See the definition in sync.c
// for why it exists and why it must not unwind.
void sigusr2_handler(int sig);
// Instantiate this thread's thread-local storage used by sigusr1_handler before
// SIGUSR1 is unblocked, so the handler never has to malloc() it from async
// signal context. See the definition in sync.c.
void signal_thread_locals_init(void);
// True if a signal is pending on `current` and not blocked for wake purposes --
// the same rule wait_for() uses, for blocking sites that are not parked in a
// cond_t. False when there is no current task.
bool task_wake_signal_pending(void);
// True if both wake signals (SIGUSR1, SIGUSR2) are unblocked in the calling
// host thread's mask.
bool signal_thread_wake_sigs_unblocked(void);
// If either wake signal is blocked in the calling host thread's mask, unblock
// it (which delivers it right here) and return true. Must be called from a
// normal call stack, never from a signal handler. See sync.c.
bool signal_thread_unwedge_wake_sigs(void);
bool current_is_valid(void);

#endif
