#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fs/fake-lockstats.h"

bool fakefs_lockstats_on = false;
int fakefs_lockstats_fd = 2;

#define MAX_SITES 64
// Waits shorter than this are indistinguishable from the uncontended
// acquire itself (two clock reads plus an uncontended pthread_mutex_lock).
#define CONTENDED_NS 1000
#define STALL_NS 100000

struct fakefs_lock_site {
    const char *name;
    _Atomic uint64_t calls;
    _Atomic uint64_t contended;
    _Atomic uint64_t wait_ns;
    _Atomic uint64_t wait_max_ns;
    _Atomic uint64_t hold_ns;
    _Atomic uint64_t hold_max_ns;
};

static struct fakefs_lock_site sites[MAX_SITES];
static _Atomic unsigned site_count;
static pthread_mutex_t site_lock = PTHREAD_MUTEX_INITIALIZER;

static _Atomic uint64_t total_contended;
static _Atomic uint64_t total_stalls;
static _Atomic uint64_t threads_seen;
// Log2 buckets of wait time in nanoseconds: bucket i covers [2^i, 2^(i+1)) ns.
static _Atomic uint64_t wait_hist[32];
// The activity window: first acquire to last release. Using the process
// lifetime instead would fold in startup and teardown and understate the duty
// cycle of the workload we actually care about.
static _Atomic uint64_t window_first;
static _Atomic uint64_t window_last;

_Thread_local struct fakefs_lock_site *fakefs_ls_site;
_Thread_local uint64_t fakefs_ls_wait;
_Thread_local uint64_t fakefs_ls_start;
static _Thread_local bool counted_thread;

void fakefs_lockstats_init(void) {
    fakefs_lockstats_on = getenv("ISH_FAKEFS_LOCKSTATS") != NULL;
}

struct fakefs_lock_site *fakefs_lockstats_site(const char *name) {
    // Interning by name rather than by the __func__ pointer keeps one row per
    // function even when the same site is reached from two objects.
    unsigned n = atomic_load_explicit(&site_count, memory_order_acquire);
    for (unsigned i = 0; i < n; i++)
        if (strcmp(sites[i].name, name) == 0)
            return &sites[i];

    pthread_mutex_lock(&site_lock);
    n = atomic_load_explicit(&site_count, memory_order_relaxed);
    for (unsigned i = 0; i < n; i++) {
        if (strcmp(sites[i].name, name) == 0) {
            pthread_mutex_unlock(&site_lock);
            return &sites[i];
        }
    }
    struct fakefs_lock_site *site = &sites[n < MAX_SITES ? n : MAX_SITES - 1];
    if (n < MAX_SITES) {
        site->name = name;
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

void fakefs_lockstats_account(uint64_t t_release) {
    // Can be NULL if stats were switched on between an acquire and its
    // release; drop that one sample rather than logging a bogus hold.
    struct fakefs_lock_site *site = fakefs_ls_site;
    if (site == NULL)
        return;
    fakefs_ls_site = NULL;
    uint64_t wait = fakefs_ls_wait;
    uint64_t hold = t_release - fakefs_ls_start;

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
        atomic_compare_exchange_strong_explicit(&window_first, &first,
                fakefs_ls_start, memory_order_relaxed, memory_order_relaxed);
    note_max(&window_last, t_release);
}

static int by_wait_desc(const void *a, const void *b) {
    const struct fakefs_lock_site *x = *(const struct fakefs_lock_site **) a;
    const struct fakefs_lock_site *y = *(const struct fakefs_lock_site **) b;
    uint64_t xw = atomic_load_explicit(&x->hold_ns, memory_order_relaxed);
    uint64_t yw = atomic_load_explicit(&y->hold_ns, memory_order_relaxed);
    if (xw != yw)
        return yw > xw ? 1 : -1;
    return 0;
}

static double ms(uint64_t ns) { return (double) ns / 1000000.0; }

void fakefs_lockstats_dump(void) {
    if (!fakefs_lockstats_on)
        return;
    unsigned n = atomic_load_explicit(&site_count, memory_order_acquire);
    if (n == 0) {
        dprintf(fakefs_lockstats_fd, "fakefs-lockstats: no acquires\n");
        return;
    }

    struct fakefs_lock_site *sorted[MAX_SITES];
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
    qsort(sorted, n, sizeof(sorted[0]), by_wait_desc);

    uint64_t first = atomic_load_explicit(&window_first, memory_order_relaxed);
    uint64_t last = atomic_load_explicit(&window_last, memory_order_relaxed);
    uint64_t window = last > first ? last - first : 0;
    uint64_t contended = atomic_load_explicit(&total_contended, memory_order_relaxed);

    dprintf(fakefs_lockstats_fd,
            "fakefs-lockstats: %llu acquires by %llu threads over a %.1f ms window\n",
            (unsigned long long) calls,
            (unsigned long long) atomic_load_explicit(&threads_seen, memory_order_relaxed),
            ms(window));
    // The headline. The lock is exclusive, so held-time cannot exceed the
    // window: this ratio is how close the mount is to being serialization-
    // bound on one mutex.
    dprintf(fakefs_lockstats_fd,
            "fakefs-lockstats: held %.1f ms = %.1f%% duty cycle; waited %.1f ms total, "
            "%.3f ms max\n",
            ms(hold), window ? 100.0 * (double) hold / (double) window : 0.0,
            ms(wait), ms(wait_max));
    dprintf(fakefs_lockstats_fd,
            "fakefs-lockstats: contended %llu (%.1f%% of acquires), of which %llu waited >100us\n",
            (unsigned long long) contended,
            calls ? 100.0 * (double) contended / (double) calls : 0.0,
            (unsigned long long) atomic_load_explicit(&total_stalls, memory_order_relaxed));

    dprintf(fakefs_lockstats_fd,
            "fakefs-lockstats: %-28s %10s %10s %10s %12s %11s %11s\n",
            "site", "calls", "contended", "hold_avg_us", "hold_tot_ms", "hold_max_ms", "wait_tot_ms");
    for (unsigned i = 0; i < n; i++) {
        struct fakefs_lock_site *s = sorted[i];
        uint64_t c = atomic_load_explicit(&s->calls, memory_order_relaxed);
        uint64_t h = atomic_load_explicit(&s->hold_ns, memory_order_relaxed);
        if (c == 0)
            continue;
        dprintf(fakefs_lockstats_fd,
                "fakefs-lockstats: %-28s %10llu %10llu %10.1f %12.1f %11.3f %11.1f\n",
                s->name, (unsigned long long) c,
                (unsigned long long) atomic_load_explicit(&s->contended, memory_order_relaxed),
                (double) h / (double) c / 1000.0,
                ms(h),
                ms(atomic_load_explicit(&s->hold_max_ns, memory_order_relaxed)),
                ms(atomic_load_explicit(&s->wait_ns, memory_order_relaxed)));
    }

    char line[512];
    int off = snprintf(line, sizeof(line), "fakefs-lockstats: wait histogram:");
    for (unsigned b = 0; b < 32 && off > 0 && off < (int) sizeof(line); b++) {
        uint64_t v = atomic_load_explicit(&wait_hist[b], memory_order_relaxed);
        if (v == 0)
            continue;
        uint64_t lo = b == 0 ? 0 : (1ULL << b);
        const char *unit = "ns";
        double scaled = (double) lo;
        if (lo >= 1000000) { scaled = (double) lo / 1000000.0; unit = "ms"; }
        else if (lo >= 1000) { scaled = (double) lo / 1000.0; unit = "us"; }
        off += snprintf(line + off, sizeof(line) - off, " %g%s:%llu",
                scaled, unit, (unsigned long long) v);
    }
    dprintf(fakefs_lockstats_fd, "%s\n", line);
}
