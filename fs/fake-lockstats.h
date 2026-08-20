#ifndef FS_FAKE_LOCKSTATS_H
#define FS_FAKE_LOCKSTATS_H

// Contention instrumentation for the fakefs metadata mutex (fakefs_db::lock).
//
// That one sqlite3 mutex serializes every metadata operation on a mount:
// db_begin_read/db_begin_write hold it for the whole transaction, and fake.c
// deliberately keeps transactions open across the real host syscall (see the
// comment above fakefs_unlink). So the interesting quantities are not just how
// long callers WAIT, but what fraction of wall-clock the lock is HELD at all --
// that is the serialization ceiling for guest metadata work, no matter how many
// guest threads are running.
//
// Off unless ISH_FAKEFS_LOCKSTATS is set in the environment; when off the cost
// is one predictable branch on a plain bool per acquire.

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

struct fakefs_lock_site;

// Set once, before any guest thread starts. Read without synchronization on
// purpose: it never changes after fakefs_lockstats_init().
extern bool fakefs_lockstats_on;

// Per-thread carry between acquire and release. The lock is a non-recursive
// SQLITE_MUTEX_FAST, so a thread holds it at most once and one slot suffices.
extern _Thread_local struct fakefs_lock_site *fakefs_ls_site;
extern _Thread_local uint64_t fakefs_ls_wait;
extern _Thread_local uint64_t fakefs_ls_start;

void fakefs_lockstats_init(void);
// Interns a call site by function name.
struct fakefs_lock_site *fakefs_lockstats_site(const char *name);
// Folds one acquire/release pair into the counters. MUST be called after the
// mutex is released: every atomic in here would otherwise land inside the
// critical section and inflate the hold time this tool exists to measure.
void fakefs_lockstats_account(uint64_t t_release);
void fakefs_lockstats_dump(void);

// dup'd from stderr by main.c: the dump runs from cli_halt, after guest
// teardown has closed the (possibly shared) host stderr fd. Same reason as
// hle_stats_fd / jit_timing_stats_fd.
extern int fakefs_lockstats_fd;

static inline uint64_t fakefs_lockstats_now(void) {
#if defined(__APPLE__)
    // No syscall, ~15ns: this is on the path of every guest stat().
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000000ULL + (uint64_t) ts.tv_nsec;
#endif
}

// The only work done under the lock: three TLS stores.
static inline void fakefs_lockstats_held(struct fakefs_lock_site *site, uint64_t t0) {
    uint64_t t1 = fakefs_lockstats_now();
    fakefs_ls_site = site;
    fakefs_ls_wait = t1 - t0;
    fakefs_ls_start = t1;
}

// Wrap a bare (non-transactional) acquire of fs->lock, attributing it to the
// enclosing function. The static slot makes interning a one-time cost per site.
#define FAKEFS_LOCK(fs) do {                                                  \
    if (!fakefs_lockstats_on) {                                               \
        sqlite3_mutex_enter((fs)->lock);                                      \
        break;                                                                \
    }                                                                         \
    static struct fakefs_lock_site *_fls_site;                                \
    if (_fls_site == NULL)                                                    \
        _fls_site = fakefs_lockstats_site(__func__);                          \
    uint64_t _fls_t0 = fakefs_lockstats_now();                                \
    sqlite3_mutex_enter((fs)->lock);                                          \
    fakefs_lockstats_held(_fls_site, _fls_t0);                                \
} while (0)

#define FAKEFS_UNLOCK(fs) do {                                                \
    if (!fakefs_lockstats_on) {                                               \
        sqlite3_mutex_leave((fs)->lock);                                      \
        break;                                                                \
    }                                                                         \
    uint64_t _fls_t2 = fakefs_lockstats_now();                                \
    sqlite3_mutex_leave((fs)->lock);                                          \
    fakefs_lockstats_account(_fls_t2);                                        \
} while (0)

#endif
