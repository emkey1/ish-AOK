#ifndef FS_FAKE_LOCKSTATS_H
#define FS_FAKE_LOCKSTATS_H

// The fakefs metadata mutex, measured through the shared engine in
// util/lockstats.h. Kept as its own header and its own env var because this
// lock is a sqlite3_mutex rather than one of the emulator's own lock types,
// so the generic lock()/read_lock() hooks cannot see it.
//
// Enabled by ISH_FAKEFS_LOCKSTATS. When off the cost is one branch on a plain
// bool per acquire.

#include "util/lockstats.h"

// The frame is keyed by the fakefs_db, not by the mutex: a pooled read holds a
// region with no mutex at all, and still needs a frame to close.
#define FAKEFS_LOCK(fs) do {                                                  \
    if (!fakefs_lockstats_on) {                                               \
        sqlite3_mutex_enter((fs)->lock);                                      \
        break;                                                                \
    }                                                                         \
    static struct lock_site *_fls_site;                                       \
    if (_fls_site == NULL)                                                    \
        _fls_site = lockstats_site("fakefs", __func__);                       \
    uint64_t _fls_t0 = lockstats_now();                                       \
    sqlite3_mutex_enter((fs)->lock);                                          \
    lockstats_held(_fls_site, (fs), _fls_t0);                                 \
} while (0)

#define FAKEFS_UNLOCK(fs) do {                                                \
    if (!fakefs_lockstats_on) {                                               \
        sqlite3_mutex_leave((fs)->lock);                                      \
        break;                                                                \
    }                                                                         \
    uint64_t _fls_t2 = lockstats_now();                                       \
    sqlite3_mutex_leave((fs)->lock);                                          \
    lockstats_account((fs), _fls_t2);                                         \
} while (0)

#endif
