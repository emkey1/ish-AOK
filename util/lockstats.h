#ifndef UTIL_LOCKSTATS_H
#define UTIL_LOCKSTATS_H

// Duty-cycle instrumentation for any lock in the emulator.
//
// The question this answers is not "how long do callers wait" but "what
// fraction of wall clock is this lock HELD at all" -- for an exclusive lock
// that ratio is the serialization ceiling, and it is what showed the fakefs
// metadata mutex was ~78% saturated by a single thread. Waits only appear once
// something else is already trying; duty cycle predicts the wall before you
// hit it.
//
// All accounting happens AFTER the lock is released. Doing it inside the
// critical section inflates the very number being measured -- by 2.6x when
// this was first written for fakefs -- so the hot path under the lock is three
// TLS stores and nothing else.
//
// KNOWN LIMITATION -- read before trusting the hold/duty columns.
//
// Hold time is measured by pairing an acquire with its release on the SAME
// thread, through the hooks in ro_locks.h / rw_locks.h. Any path that releases
// a lock without going through those hooks leaves the frame open, and the span
// gets charged to whatever release closes it next. The tell is a duty cycle
// above 100% on an exclusive mutex, which is physically impossible: pids_lock
// reports ~345% under `make -j4`, all of it on the two complex_lockt callers
// (pid_get_task_ref / pid_get_task_zombie_ref), with a 380 ms maximum hold
// that is not credible for a hash lookup and a refcount bump. Two invisible
// releases were found and fixed (condition waits, and
// write_unlock_and_destroy); at least one more remains unidentified on the
// pids path -- kernel/signal.c's direct pthread_mutex_lock/unlock of another
// task's waiting_lock is balanced and so is NOT it.
//
// The wait and contended columns do NOT have this problem. They are measured
// between the two clock reads that straddle the acquire itself and never
// depend on the pairing, so "is this lock contended" is answerable from them
// alone even where hold time is wrong.
//
// Enabled per subsystem so the cost lands only where it is wanted:
//   ISH_LOCKSTATS=1          lock_t (inodes_lock, pids_lock, ...) and wrlock_t
//   ISH_FAKEFS_LOCKSTATS=1   the fakefs metadata mutex
// Both feed one registry and one dump.

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

struct lock_site;

extern bool lockstats_on;         // ISH_LOCKSTATS: lock_t + wrlock_t hooks
extern bool fakefs_lockstats_on;  // ISH_FAKEFS_LOCKSTATS: the fakefs mutex
extern int lockstats_fd;

void lockstats_init(void);
struct lock_site *lockstats_site(const char *group, const char *name);
void lockstats_dump(void);

static inline uint64_t lockstats_now(void) {
#if defined(__APPLE__)
    // No syscall, ~15ns.
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000000ULL + (uint64_t) ts.tv_nsec;
#endif
}

// Held-lock stack. Locks nest (a thread can hold inodes_lock and then the
// fakefs write mutex), and they are not always released in LIFO order, so the
// release side matches on the lock object rather than assuming the top.
#define LOCKSTATS_DEPTH 16
struct lockstats_frame {
    struct lock_site *site;
    const void *obj;
    uint64_t wait;
    uint64_t start;
};
extern _Thread_local struct lockstats_frame lockstats_stack[LOCKSTATS_DEPTH];
extern _Thread_local unsigned lockstats_depth;

// Under the lock: three stores, no atomics, no clock beyond the one read.
void lockstats_note_deep(void);
static inline void lockstats_held(struct lock_site *site, const void *obj, uint64_t t0) {
    if (lockstats_depth >= LOCKSTATS_DEPTH) {
        lockstats_note_deep();  // deeper than we track; drop rather than corrupt
        return;
    }
    struct lockstats_frame *f = &lockstats_stack[lockstats_depth++];
    uint64_t t1 = lockstats_now();
    f->site = site;
    f->obj = obj;
    f->wait = t1 - t0;
    f->start = t1;
}

// After the lock is released. Folds the frame for `obj` into the counters.
void lockstats_account(const void *obj, uint64_t t_release);
// Same, but attributes an unmatched release to `group` so a leaking or
// unhooked acquire path can be found instead of guessed at.
void lockstats_account_named(const void *obj, uint64_t t_release, const char *group);

// For a lock that is about to be dropped by something other than unlock() --
// a condition wait -- and re-taken afterwards. See the comment on the
// definitions; parked time counts as neither hold nor wait.
struct lock_site *lockstats_suspend(const void *obj);
void lockstats_resume(struct lock_site *site, const void *obj);

#endif
