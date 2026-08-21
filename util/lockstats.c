#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util/lockstats.h"

bool lockstats_on = false;
bool fakefs_lockstats_on = false;
int lockstats_fd = 2;

#define MAX_SITES 192
// Waits shorter than this are indistinguishable from the uncontended acquire
// itself (two clock reads plus an uncontended pthread_mutex_lock).
#define CONTENDED_NS 1000
#define STALL_NS 100000

struct lock_site {
    const char *group;
    const char *name;
    _Atomic uint64_t calls;
    _Atomic uint64_t contended;
    _Atomic uint64_t wait_ns;
    _Atomic uint64_t wait_max_ns;
    _Atomic uint64_t hold_ns;
    _Atomic uint64_t hold_max_ns;
};

static struct lock_site sites[MAX_SITES];
static _Atomic unsigned site_count;
static pthread_mutex_t site_lock = PTHREAD_MUTEX_INITIALIZER;

static _Atomic uint64_t total_contended;
static _Atomic uint64_t total_stalls;
static _Atomic uint64_t threads_seen;
static _Atomic uint64_t dropped_deep;
static _Atomic uint64_t dropped_unmatched;
// Log2 buckets of wait time in nanoseconds: bucket i covers [2^i, 2^(i+1)).
static _Atomic uint64_t wait_hist[32];
// The activity window: first acquire to last release. Process lifetime would
// fold in startup and teardown and understate the workload's duty cycle.
static _Atomic uint64_t window_first;
static _Atomic uint64_t window_last;

_Thread_local struct lockstats_frame lockstats_stack[LOCKSTATS_DEPTH];
_Thread_local unsigned lockstats_depth;
static _Thread_local bool counted_thread;

void lockstats_init(void) {
    lockstats_on = getenv("ISH_LOCKSTATS") != NULL;
    fakefs_lockstats_on = getenv("ISH_FAKEFS_LOCKSTATS") != NULL;
}

struct lock_site *lockstats_site(const char *group, const char *name) {
    unsigned n = atomic_load_explicit(&site_count, memory_order_acquire);
    for (unsigned i = 0; i < n; i++)
        if (strcmp(sites[i].name, name) == 0 && strcmp(sites[i].group, group) == 0)
            return &sites[i];

    pthread_mutex_lock(&site_lock);
    n = atomic_load_explicit(&site_count, memory_order_relaxed);
    for (unsigned i = 0; i < n; i++) {
        if (strcmp(sites[i].name, name) == 0 && strcmp(sites[i].group, group) == 0) {
            pthread_mutex_unlock(&site_lock);
            return &sites[i];
        }
    }
    struct lock_site *site = &sites[n < MAX_SITES ? n : MAX_SITES - 1];
    if (n < MAX_SITES) {
        // Own the strings: `group` can point into a lock struct that outlives
        // neither the dump nor, for a dynamically allocated lock, the process.
        site->group = strdup(group);
        site->name = strdup(name);
        atomic_store_explicit(&site_count, n + 1, memory_order_release);
    }
    pthread_mutex_unlock(&site_lock);
    return site;
}

static void note_max(_Atomic uint64_t *slot, uint64_t value) {
    uint64_t old = atomic_load_explicit(slot, memory_order_relaxed);
    while (value > old &&
           !atomic_compare_exchange_weak_explicit(slot, &old, value,
                   memory_order_relaxed, memory_order_relaxed))
        ;
}

void lockstats_note_deep(void) {
    atomic_fetch_add_explicit(&dropped_deep, 1, memory_order_relaxed);
    // The first thread to overflow dumps what it is holding. A full stack is
    // almost always one acquire path that never pops, and the resident frames
    // name it outright -- far quicker than reasoning about which of the lock
    // helpers has an unhooked release.
    static atomic_flag reported = ATOMIC_FLAG_INIT;
    if (atomic_flag_test_and_set(&reported))
        return;
    dprintf(lockstats_fd, "lockstats: frame stack full, resident frames:\n");
    for (unsigned i = 0; i < lockstats_depth; i++) {
        struct lock_site *site = lockstats_stack[i].site;
        dprintf(lockstats_fd, "lockstats:   [%2u] %s / %s\n", i,
                site != NULL ? site->group : "?", site != NULL ? site->name : "?");
    }
}

// Pops the frame for `obj` and folds it into the counters. Returns the site so
// a caller that is only pausing (a condition wait) can reopen it afterwards.
static struct lock_site *lockstats_close_named(const void *obj, uint64_t t_release, const char *group) {
    // Usually the top of the stack; a lock released out of order is found by
    // scanning down. An unmatched release means the lock was taken before
    // instrumentation was armed -- drop it rather than log a bogus hold.
    unsigned i = lockstats_depth;
    while (i > 0 && lockstats_stack[i - 1].obj != obj)
        i--;
    if (i == 0) {
        atomic_fetch_add_explicit(&dropped_unmatched, 1, memory_order_relaxed);
        if (group != NULL) {
            struct lock_site *u = lockstats_site("UNMATCHED", group);
            atomic_fetch_add_explicit(&u->calls, 1, memory_order_relaxed);
        }
        return NULL;
    }
    struct lockstats_frame f = lockstats_stack[i - 1];
    // Close the hole by sliding the frames above it down.
    for (unsigned j = i; j < lockstats_depth; j++)
        lockstats_stack[j - 1] = lockstats_stack[j];
    lockstats_depth--;

    struct lock_site *site = f.site;
    uint64_t wait = f.wait;
    uint64_t hold = t_release - f.start;

    if (!counted_thread) {
        counted_thread = true;
        atomic_fetch_add_explicit(&threads_seen, 1, memory_order_relaxed);
    }

    atomic_fetch_add_explicit(&site->calls, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&site->wait_ns, wait, memory_order_relaxed);
    note_max(&site->wait_max_ns, wait);
    atomic_fetch_add_explicit(&site->hold_ns, hold, memory_order_relaxed);
    note_max(&site->hold_max_ns, hold);
    if (wait >= CONTENDED_NS) {
        atomic_fetch_add_explicit(&site->contended, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&total_contended, 1, memory_order_relaxed);
    }
    if (wait >= STALL_NS)
        atomic_fetch_add_explicit(&total_stalls, 1, memory_order_relaxed);

    unsigned bucket = 0;
    while (bucket < 31 && (wait >> bucket) > 1)
        bucket++;
    atomic_fetch_add_explicit(&wait_hist[bucket], 1, memory_order_relaxed);

    uint64_t first = atomic_load_explicit(&window_first, memory_order_relaxed);
    if (first == 0)
        atomic_compare_exchange_strong_explicit(&window_first, &first, f.start,
                memory_order_relaxed, memory_order_relaxed);
    note_max(&window_last, t_release);
    return site;
}

static struct lock_site *lockstats_close(const void *obj, uint64_t t_release) {
    return lockstats_close_named(obj, t_release, NULL);
}

void lockstats_account(const void *obj, uint64_t t_release) {
    lockstats_close_named(obj, t_release, NULL);
}

void lockstats_account_named(const void *obj, uint64_t t_release, const char *group) {
    lockstats_close_named(obj, t_release, group);
}

// A condition wait drops the mutex where the lock/unlock hooks cannot see it.
// Without this pair, a thread parked in wait_for() bills the entire park as
// hold time -- which is how pids_lock first reported a 131% duty cycle on an
// exclusive mutex, a physically impossible number that gave the bug away.
// Time spent parked is excluded from hold, and from wait: the re-acquire
// contention happens inside pthread_cond_wait where it cannot be separated
// from the wakeup itself.
struct lock_site *lockstats_suspend(const void *obj) {
    return lockstats_close(obj, lockstats_now());
}

void lockstats_resume(struct lock_site *site, const void *obj) {
    if (site != NULL)
        lockstats_held(site, obj, lockstats_now());
}

static int by_hold_desc(const void *a, const void *b) {
    const struct lock_site *x = *(const struct lock_site **) a;
    const struct lock_site *y = *(const struct lock_site **) b;
    uint64_t xh = atomic_load_explicit(&x->hold_ns, memory_order_relaxed);
    uint64_t yh = atomic_load_explicit(&y->hold_ns, memory_order_relaxed);
    if (xh != yh)
        return yh > xh ? 1 : -1;
    return strcmp(x->group, y->group);
}

static double ms(uint64_t ns) { return (double) ns / 1000000.0; }

void lockstats_dump(void) {
    if (!lockstats_on && !fakefs_lockstats_on)
        return;
    unsigned n = atomic_load_explicit(&site_count, memory_order_acquire);
    if (n == 0) {
        dprintf(lockstats_fd, "lockstats: no acquires\n");
        return;
    }

    struct lock_site *sorted[MAX_SITES];
    uint64_t calls = 0, wait = 0, hold = 0, wait_max = 0;
    for (unsigned i = 0; i < n; i++) {
        sorted[i] = &sites[i];
        calls += atomic_load_explicit(&sites[i].calls, memory_order_relaxed);
        wait += atomic_load_explicit(&sites[i].wait_ns, memory_order_relaxed);
        hold += atomic_load_explicit(&sites[i].hold_ns, memory_order_relaxed);
        uint64_t m = atomic_load_explicit(&sites[i].wait_max_ns, memory_order_relaxed);
        if (m > wait_max)
            wait_max = m;
    }
    qsort(sorted, n, sizeof(sorted[0]), by_hold_desc);

    uint64_t first = atomic_load_explicit(&window_first, memory_order_relaxed);
    uint64_t last = atomic_load_explicit(&window_last, memory_order_relaxed);
    uint64_t window = last > first ? last - first : 0;

    dprintf(lockstats_fd,
            "lockstats: %llu acquires by %llu threads over a %.1f ms window\n",
            (unsigned long long) calls,
            (unsigned long long) atomic_load_explicit(&threads_seen, memory_order_relaxed),
            ms(window));

    // Per-group roll-up first: for an exclusive lock, held/window is how close
    // that lock is to being the serialization ceiling. A figure above 100%
    // means genuine overlap (a shared lock, or several distinct objects in the
    // same group), which is itself the answer to "does this thing serialize".
    dprintf(lockstats_fd, "lockstats: %-14s %10s %12s %9s %12s %11s\n",
            "group", "calls", "hold_tot_ms", "duty%", "wait_tot_ms", "contended");
    for (unsigned i = 0; i < n; i++) {
        if (sorted[i]->group == NULL)
            continue;
        // Fold every site sharing this group into one row, once.
        bool seen = false;
        for (unsigned j = 0; j < i; j++)
            if (strcmp(sorted[j]->group, sorted[i]->group) == 0) { seen = true; break; }
        if (seen)
            continue;
        uint64_t gc = 0, gh = 0, gw = 0, gcon = 0;
        for (unsigned j = 0; j < n; j++) {
            if (strcmp(sorted[j]->group, sorted[i]->group) != 0)
                continue;
            gc += atomic_load_explicit(&sorted[j]->calls, memory_order_relaxed);
            gh += atomic_load_explicit(&sorted[j]->hold_ns, memory_order_relaxed);
            gw += atomic_load_explicit(&sorted[j]->wait_ns, memory_order_relaxed);
            gcon += atomic_load_explicit(&sorted[j]->contended, memory_order_relaxed);
        }
        dprintf(lockstats_fd, "lockstats: %-14s %10llu %12.1f %8.1f%% %12.1f %11llu\n",
                sorted[i]->group, (unsigned long long) gc, ms(gh),
                window ? 100.0 * (double) gh / (double) window : 0.0,
                ms(gw), (unsigned long long) gcon);
    }

    dprintf(lockstats_fd, "lockstats: %-14s %-26s %10s %10s %11s %12s %11s\n",
            "group", "site", "calls", "contended", "hold_avg_us", "hold_tot_ms", "wait_tot_ms");
    for (unsigned i = 0; i < n; i++) {
        struct lock_site *s = sorted[i];
        uint64_t c = atomic_load_explicit(&s->calls, memory_order_relaxed);
        uint64_t h = atomic_load_explicit(&s->hold_ns, memory_order_relaxed);
        if (c == 0)
            continue;
        dprintf(lockstats_fd,
                "lockstats: %-14s %-26s %10llu %10llu %11.1f %12.1f %12.3f %11.1f\n",
                s->group, s->name, (unsigned long long) c,
                (unsigned long long) atomic_load_explicit(&s->contended, memory_order_relaxed),
                (double) h / (double) c / 1000.0, ms(h),
                ms(atomic_load_explicit(&s->hold_max_ns, memory_order_relaxed)),
                ms(atomic_load_explicit(&s->wait_ns, memory_order_relaxed)));
    }

    uint64_t deep = atomic_load_explicit(&dropped_deep, memory_order_relaxed);
    uint64_t unmatched = atomic_load_explicit(&dropped_unmatched, memory_order_relaxed);
    if (deep != 0 || unmatched != 0)
        dprintf(lockstats_fd,
                "lockstats: dropped samples: %llu too deep, %llu unmatched release\n",
                (unsigned long long) deep, (unsigned long long) unmatched);
    dprintf(lockstats_fd, "lockstats: %llu contended (%.1f%%), %llu waited >100us, max wait %.3f ms\n",
            (unsigned long long) atomic_load_explicit(&total_contended, memory_order_relaxed),
            calls ? 100.0 * (double) atomic_load_explicit(&total_contended, memory_order_relaxed) / (double) calls : 0.0,
            (unsigned long long) atomic_load_explicit(&total_stalls, memory_order_relaxed),
            ms(wait_max));
}
