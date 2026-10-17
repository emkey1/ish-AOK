#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "kernel/guestprof.h"
#include "kernel/task.h"
#include "kernel/mm.h"
#include "fs/proc.h"
#include "util/lockstats.h"   // lockstats_now(): the same cheap host clock

bool guestprof_on = false;
static unsigned guestprof_interval_us = 1000;
static int guestprof_fd = 2;

// ---------------------------------------------------------------- slots

// One per task that has executed anything. Padded to a cache line: the sampler
// reads every slot on every tick, and without the padding that read would
// bounce the line a running task is storing its PC into.
#define GP_SLOTS 512
struct gp_slot {
    _Atomic uint32_t live;
    _Atomic uint32_t pid;
    _Atomic uint32_t state;
    _Atomic uint32_t abi;
    _Atomic uint64_t scall;
    _Atomic uint64_t pc;
    _Atomic uint64_t mm_id;
    // Set by the sampler when it sees an mm_id it has no map for; cleared by
    // the task once it has published one. See guestprof_maps_checkpoint.
    _Atomic uint64_t want_maps;
// Let the compiler do the arithmetic. A hand-computed pad got the field count
// wrong and made the struct 112 bytes -- straddling two lines, which is worse
// than not padding at all.
} __attribute__((aligned(64)));
_Static_assert(sizeof(struct gp_slot) == 64, "gp_slot must be exactly one cache line");
static struct gp_slot gp_slots[GP_SLOTS];
static _Atomic unsigned gp_slots_exhausted;

// The slot index lives on the task (kernel/task.h), so any thread can release
// it. Claimed on first publish, which always runs on the task's own thread.
static struct gp_slot *gp_slot_for(struct task *task) {
    if (task == NULL)
        return NULL;
    if (task->prof_slot >= 0)
        return &gp_slots[task->prof_slot];
    if (task->prof_slot < -1)
        return NULL;    // already tried and the table was full
    for (unsigned i = 0; i < GP_SLOTS; i++) {
        uint32_t free_ = 0;
        if (atomic_compare_exchange_strong_explicit(&gp_slots[i].live, &free_, 1,
                    memory_order_acq_rel, memory_order_relaxed)) {
            struct gp_slot *s = &gp_slots[i];
            atomic_store_explicit(&s->pid, (uint32_t) task->pid, memory_order_relaxed);
            atomic_store_explicit(&s->abi, (uint32_t) task->abi, memory_order_relaxed);
            atomic_store_explicit(&s->state, GUESTPROF_IDLE, memory_order_relaxed);
            atomic_store_explicit(&s->pc, 0, memory_order_relaxed);
            atomic_store_explicit(&s->mm_id, 0, memory_order_relaxed);
            atomic_store_explicit(&s->want_maps, 0, memory_order_relaxed);
            task->prof_slot = (int) i;
            return s;
        }
    }
    atomic_fetch_add_explicit(&gp_slots_exhausted, 1, memory_order_relaxed);
    task->prof_slot = -2;   // stop rescanning the whole table on every publish
    return NULL;
}

static struct gp_slot *gp_slot(void) {
    return gp_slot_for(current);
}

void guestprof_slot_release(struct task *task) {
    if (task == NULL || task->prof_slot < 0) {
        if (task != NULL)
            task->prof_slot = -1;
        return;
    }
    struct gp_slot *s = &gp_slots[task->prof_slot];
    task->prof_slot = -1;
    atomic_store_explicit(&s->state, GUESTPROF_IDLE, memory_order_relaxed);
    atomic_store_explicit(&s->live, 0, memory_order_release);
}

void guestprof_state_for(struct task *task, unsigned state) {
    struct gp_slot *s = gp_slot_for(task);
    if (s == NULL)
        return;
    if (state == GUESTPROF_GUEST && task->mm != NULL)
        atomic_store_explicit(&s->mm_id, task->mm->id, memory_order_relaxed);
    atomic_store_explicit(&s->state, state, memory_order_relaxed);
}

void guestprof_state(unsigned state) {
    guestprof_state_for(current, state);
}

void guestprof_state_syscall(unsigned long nr) {
    struct gp_slot *s = gp_slot();
    if (s == NULL)
        return;
    atomic_store_explicit(&s->scall, (uint64_t) nr, memory_order_relaxed);
    atomic_store_explicit(&s->abi, current != NULL ? (uint32_t) current->abi : 0,
            memory_order_relaxed);
    atomic_store_explicit(&s->state, GUESTPROF_KERNEL, memory_order_relaxed);
}

// The hot one: a single relaxed store, once per dispatch through
// cpu_run_to_interrupt's C loop.
void guestprof_pc(uint64_t pc, uint64_t mm_id) {
    struct gp_slot *s = gp_slot();
    if (s == NULL)
        return;
    atomic_store_explicit(&s->pc, pc, memory_order_relaxed);
    if (mm_id != 0)
        atomic_store_explicit(&s->mm_id, mm_id, memory_order_relaxed);
}

// ------------------------------------------------------------ map table

// Every execve makes a new address space, so this is a count of PROCESSES in
// the profiling window, not of live ones. A 40-second `apt install` went well
// past 64 -- and with a full table dropping new snapshots, every process after
// the 64th had no map at all and its samples landed in [no-file-mapping],
// which came to 42% of on-CPU time. Sized generously now, and full means
// EVICT the least recently used rather than refuse to learn.
#define GP_MAX_MAPS     512
#define GP_MAX_REGIONS  96
#define GP_MAX_DSOS     512

struct gp_region {
    uint64_t start, end, file_offset;
    unsigned dso;
};
struct gp_map {
    uint64_t mm_id;
    unsigned n;
    unsigned refreshes;     // capped; see gp_resolve
    uint64_t used;          // tick of last successful resolve, for eviction
    struct gp_region r[GP_MAX_REGIONS];
};
// A PC can miss an existing snapshot legitimately (a dlopen since it was
// taken), or for ever (JIT-generated guest code, an anonymous exec mapping).
// Re-ask a bounded number of times so the first case heals and the second
// does not turn into a snapshot on every tick.
#define GP_MAX_REFRESH 8

// Written by guest threads (one snapshot per address space), read by the
// sampler. Rare on both sides, so one plain mutex rather than anything clever.
static pthread_mutex_t gp_maps_lock = PTHREAD_MUTEX_INITIALIZER;
static struct gp_map gp_maps[GP_MAX_MAPS];
static unsigned gp_maps_n;
static unsigned gp_maps_evicted;
// Tick counter, for the map LRU. Written by the sampler, read under
// gp_maps_lock by a guest thread taking a snapshot; a stale read only picks a
// slightly different eviction victim.
static uint64_t gp_tick_now;
static char *gp_dso_name[GP_MAX_DSOS];
static unsigned gp_dso_n;

// 0-3 are reserved for the buckets that are not a DSO at all.
#define GP_DSO_KERNEL   0u
#define GP_DSO_BLOCKED  1u
#define GP_DSO_NATIVE   2u
#define GP_DSO_NOMAP    3u
#define GP_DSO_FIRST    4u

static unsigned gp_dso_intern(const char *name) {
    for (unsigned i = GP_DSO_FIRST; i < gp_dso_n; i++)
        if (strcmp(gp_dso_name[i], name) == 0)
            return i;
    if (gp_dso_n >= GP_MAX_DSOS)
        return GP_DSO_NOMAP;
    unsigned i = gp_dso_n;
    gp_dso_name[i] = strdup(name);
    if (gp_dso_name[i] == NULL)
        return GP_DSO_NOMAP;
    gp_dso_n = i + 1;
    return i;
}

static void gp_dso_init(void) {
    gp_dso_name[GP_DSO_KERNEL]  = strdup("[kernel]");
    gp_dso_name[GP_DSO_BLOCKED] = strdup("[blocked]");
    gp_dso_name[GP_DSO_NATIVE]  = strdup("[native]");
    gp_dso_name[GP_DSO_NOMAP]   = strdup("[no-file-mapping]");
    gp_dso_n = GP_DSO_FIRST;
}

// Parse one /proc/<pid>/maps line, as proc_maps_dump prints it:
//   "%08llx-%08llx %c%c%c%c %08lx 00:00 %-10d %s\n"
// Only executable, file-backed regions are kept -- those are the ones a guest
// PC can land in and be worth a name.
static bool gp_parse_maps_line(const char *line, size_t len,
        uint64_t *start, uint64_t *end, uint64_t *off, const char **path, size_t *path_len) {
    char buf[512];
    if (len >= sizeof(buf))
        len = sizeof(buf) - 1;
    memcpy(buf, line, len);
    buf[len] = '\0';

    char *p = buf;
    char *dash = strchr(p, '-');
    if (dash == NULL)
        return false;
    *dash = '\0';
    *start = strtoull(p, NULL, 16);
    p = dash + 1;
    char *sp = strchr(p, ' ');
    if (sp == NULL)
        return false;
    *sp = '\0';
    *end = strtoull(p, NULL, 16);
    p = sp + 1;
    if (strlen(p) < 4 || p[2] != 'x')
        return false;           // not executable: cannot hold a sampled PC
    p += 4;
    while (*p == ' ') p++;
    *off = strtoull(p, &p, 16);
    while (*p == ' ') p++;      // dev
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;      // inode
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;
    if (*p == '\0')
        return false;           // anonymous: nothing to name it with
    // "[vdso]" is kept: iSH maps a real vdso (kernel/exec.c) and a guest's
    // clock_gettime really does run there, so it is code worth attributing.
    // The executable test above already excluded [stack] and friends.
    // Offsets into `buf` are useless to the caller; hand back a pointer into
    // the ORIGINAL line, at the same column.
    *path = line + (p - buf);
    size_t n = strlen(p);
    while (n > 0 && (p[n - 1] == '\n' || p[n - 1] == ' '))
        n--;
    *path_len = n;
    return n > 0;
}

void guestprof_maps_checkpoint(void) {
    if (!guestprof_on)
        return;
    struct gp_slot *s = gp_slot();
    if (s == NULL || current == NULL || current->mm == NULL)
        return;
    uint64_t want = atomic_load_explicit(&s->want_maps, memory_order_relaxed);
    if (want == 0 || want != current->mm->id)
        return;
    atomic_store_explicit(&s->want_maps, 0, memory_order_relaxed);

    extern void proc_maps_dump(struct task *task, struct proc_data *buf);
    struct proc_data buf = {};
    proc_maps_dump(current, &buf);
    if (buf.data == NULL)
        return;

    struct gp_map *m = malloc(sizeof(*m));
    if (m == NULL) {
        free(buf.data);
        return;
    }
    m->mm_id = current->mm->id;
    m->n = 0;
    m->refreshes = 0;

    pthread_mutex_lock(&gp_maps_lock);
    const char *p = buf.data;
    const char *limit = buf.data + buf.size;
    while (p < limit && m->n < GP_MAX_REGIONS) {
        const char *nl = memchr(p, '\n', (size_t) (limit - p));
        size_t len = nl != NULL ? (size_t) (nl - p) : (size_t) (limit - p);
        uint64_t start, end, off;
        const char *path;
        size_t path_len;
        if (gp_parse_maps_line(p, len, &start, &end, &off, &path, &path_len)) {
            char name[256];
            size_t n = path_len < sizeof(name) - 1 ? path_len : sizeof(name) - 1;
            memcpy(name, path, n);
            name[n] = '\0';
            struct gp_region *r = &m->r[m->n++];
            r->start = start;
            r->end = end;
            r->file_offset = off;
            r->dso = gp_dso_intern(name);
        }
        if (nl == NULL)
            break;
        p = nl + 1;
    }

    // Replace an existing snapshot for this address space (a dlopen added
    // regions), else append, else evict the least recently resolved -- which
    // is a process that has finished, long before it is one that is running.
    unsigned slot = GP_MAX_MAPS;
    for (unsigned i = 0; i < gp_maps_n; i++)
        if (gp_maps[i].mm_id == m->mm_id)
            slot = i;
    if (slot == GP_MAX_MAPS && gp_maps_n < GP_MAX_MAPS)
        slot = gp_maps_n++;
    if (slot == GP_MAX_MAPS) {
        slot = 0;
        for (unsigned i = 1; i < gp_maps_n; i++)
            if (gp_maps[i].used < gp_maps[slot].used)
                slot = i;
        gp_maps_evicted++;
    }
    m->refreshes = gp_maps[slot].mm_id == m->mm_id ? gp_maps[slot].refreshes : 0;
    m->used = gp_tick_now;
    gp_maps[slot] = *m;
    pthread_mutex_unlock(&gp_maps_lock);

    free(m);
    free(buf.data);
}

// ------------------------------------------------------------- samples

#define GP_SITES 16384
struct gp_site {
    uint64_t key_dso;
    uint64_t key_off;
    uint64_t count;
    bool used;
};
// Touched only by the sampler thread, and by the dump after it has stopped.
static struct gp_site gp_sites[GP_SITES];
static uint64_t gp_sites_dropped;

static void gp_count(unsigned dso, uint64_t off) {
    uint64_t h = (uint64_t) dso * 0x9E3779B97F4A7C15ULL;
    h ^= off * 0xC2B2AE3D27D4EB4FULL;
    h ^= h >> 29;
    size_t i = (size_t) (h & (GP_SITES - 1));
    for (unsigned probe = 0; probe < 64; probe++) {
        struct gp_site *site = &gp_sites[i];
        if (!site->used) {
            site->used = true;
            site->key_dso = dso;
            site->key_off = off;
            site->count = 1;
            return;
        }
        if (site->key_dso == dso && site->key_off == off) {
            site->count++;
            return;
        }
        i = (i + 1) & (GP_SITES - 1);
    }
    gp_sites_dropped++;
}

static _Atomic bool gp_stop;
static pthread_t gp_thread;
static bool gp_thread_started;

static uint64_t gp_ticks;
static uint64_t gp_samples;
static unsigned gp_tasks_seen;
// Slots that have ever been non-idle. Sampler-thread only, like the counters.
static bool gp_seen[GP_SLOTS];
static uint64_t gp_samples_guest, gp_samples_kernel, gp_samples_blocked, gp_samples_native;
static uint64_t gp_overhead_ns;
static uint64_t gp_first_ns, gp_last_ns;

// Resolve a guest PC to (dso, file offset) using the snapshot for its address
// space. Pure arithmetic against a table the sampler already holds -- no guest
// structure is touched here, which is the whole point of the design.
#define GP_R_OK       0   // resolved
#define GP_R_NO_MAP   1   // no snapshot for this address space yet
#define GP_R_NO_MATCH 2   // have a snapshot, PC is not in any of its regions

static int gp_resolve(uint64_t mm_id, uint64_t pc, unsigned *dso, uint64_t *off,
        bool *may_refresh) {
    int rc = GP_R_NO_MAP;
    *may_refresh = true;
    pthread_mutex_lock(&gp_maps_lock);
    for (unsigned i = 0; i < gp_maps_n; i++) {
        if (gp_maps[i].mm_id != mm_id)
            continue;
        rc = GP_R_NO_MATCH;
        for (unsigned j = 0; j < gp_maps[i].n; j++) {
            struct gp_region *r = &gp_maps[i].r[j];
            if (pc >= r->start && pc < r->end) {
                *dso = r->dso;
                *off = r->file_offset + (pc - r->start);
                rc = GP_R_OK;
                gp_maps[i].used = gp_tick_now;
                break;
            }
        }
        if (rc == GP_R_NO_MATCH) {
            if (gp_maps[i].refreshes >= GP_MAX_REFRESH)
                *may_refresh = false;
            else
                gp_maps[i].refreshes++;
        }
        break;
    }
    pthread_mutex_unlock(&gp_maps_lock);
    return rc;
}

static void gp_tick(void) {
    for (unsigned i = 0; i < GP_SLOTS; i++) {
        struct gp_slot *s = &gp_slots[i];
        if (atomic_load_explicit(&s->live, memory_order_acquire) == 0)
            continue;
        unsigned state = atomic_load_explicit(&s->state, memory_order_relaxed);
        if (state == GUESTPROF_IDLE)
            continue;
        if (!gp_seen[i]) {
            gp_seen[i] = true;
            gp_tasks_seen++;
        }
        gp_samples++;
        switch (state) {
            case GUESTPROF_BLOCKED:
                gp_samples_blocked++;
                gp_count(GP_DSO_BLOCKED, 0);
                break;
            case GUESTPROF_NATIVE:
                gp_samples_native++;
                gp_count(GP_DSO_NATIVE, 0);
                break;
            case GUESTPROF_KERNEL: {
                gp_samples_kernel++;
                uint64_t nr = atomic_load_explicit(&s->scall, memory_order_relaxed);
                uint32_t abi = atomic_load_explicit(&s->abi, memory_order_relaxed);
                gp_count(GP_DSO_KERNEL, ((uint64_t) abi << 32) | (nr & 0xffffffffULL));
                break;
            }
            case GUESTPROF_GUEST: {
                gp_samples_guest++;
                uint64_t pc = atomic_load_explicit(&s->pc, memory_order_relaxed);
                uint64_t mm_id = atomic_load_explicit(&s->mm_id, memory_order_relaxed);
                unsigned dso;
                uint64_t off;
                bool may_refresh;
                int rc = gp_resolve(mm_id, pc, &dso, &off, &may_refresh);
                if (rc == GP_R_OK) {
                    gp_count(dso, off);
                } else {
                    // Keep the address. A big anonymous bucket is a question,
                    // and it can only be answered if the PCs in it survive.
                    gp_count(GP_DSO_NOMAP, pc);
                    if (mm_id != 0 && may_refresh)
                        atomic_store_explicit(&s->want_maps, mm_id, memory_order_relaxed);
                }
                break;
            }
            default:
                break;
        }
    }
}

static void *gp_sampler(void *arg) {
    (void) arg;
    // Darwin names the CALLING thread and takes only the name; every other
    // platform takes the thread too. Unguarded this builds on the Mac and
    // breaks the Linux CI job -- same trap as kernel/swap.c's kswapd0.
#if __APPLE__
    pthread_setname_np("ish-guestprof");
#else
    pthread_setname_np(pthread_self(), "ish-guestprof");
#endif
    uint64_t period = (uint64_t) guestprof_interval_us * 1000;
    uint64_t next = lockstats_now() + period;
    gp_first_ns = lockstats_now();
    while (!atomic_load_explicit(&gp_stop, memory_order_relaxed)) {
        uint64_t now = lockstats_now();
        if (now < next) {
            struct timespec ts = {
                .tv_sec = (time_t) ((next - now) / 1000000000ULL),
                .tv_nsec = (long) ((next - now) % 1000000000ULL),
            };
            nanosleep(&ts, NULL);
        }
        // Absolute deadline, so a slow tick does not make every later one late.
        next += period;
        uint64_t t0 = lockstats_now();
        if (next < t0)
            next = t0 + period;
        gp_ticks++;
        gp_tick_now = gp_ticks;
        gp_tick();
        uint64_t t1 = lockstats_now();
        gp_overhead_ns += t1 - t0;
        gp_last_ns = t1;
    }
    return NULL;
}

void guestprof_init(void) {
    const char *env = getenv("ISH_GUEST_PROFILE");
    if (env == NULL)
        return;
    if (env[0] != '\0' && env[0] != '1') {
        unsigned long v = strtoul(env, NULL, 10);
        if (v >= 50 && v <= 1000000)
            guestprof_interval_us = (unsigned) v;
    }
    const char *out = getenv("ISH_GUEST_PROFILE_OUT");
    if (out != NULL) {
        FILE *f = fopen(out, "w");
        if (f != NULL)
            guestprof_fd = fileno(f);
    } else {
        // The report is written from cli_halt, by which time guest teardown has
        // closed stderr -- the same reason lockstats dups its own copy (main.c).
        int fd = dup(STDERR_FILENO);
        if (fd >= 0)
            guestprof_fd = fd;
    }
    gp_dso_init();
    guestprof_on = true;
    if (pthread_create(&gp_thread, NULL, gp_sampler, NULL) == 0)
        gp_thread_started = true;
    else
        guestprof_on = false;
}

// ---------------------------------------------------------------- report

static int gp_site_cmp(const void *a, const void *b) {
    const struct gp_site *x = a, *y = b;
    if (x->count > y->count) return -1;
    if (x->count < y->count) return 1;
    return 0;
}

static const char *gp_abi_name(uint32_t abi) {
    switch (abi) {
        case GUEST_ABI_I386:    return "i386";
        case GUEST_ABI_AMD64:   return "amd64";
        case GUEST_ABI_ARM64:   return "arm64";
        case GUEST_ABI_RISCV64: return "riscv64";
        case GUEST_ABI_PPC64:   return "ppc64";
        default:                return "?";
    }
}

void guestprof_dump(void) {
    if (!guestprof_on)
        return;
    atomic_store_explicit(&gp_stop, true, memory_order_relaxed);
    if (gp_thread_started)
        pthread_join(gp_thread, NULL);
    guestprof_on = false;

    if (gp_samples == 0) {
        dprintf(guestprof_fd, "guestprof: %llu ticks, no samples "
                "(nothing was executing)\n", (unsigned long long) gp_ticks);
        return;
    }

    // Per-DSO totals.
    uint64_t per_dso[GP_MAX_DSOS];
    memset(per_dso, 0, sizeof(per_dso));
    unsigned used = 0;
    for (unsigned i = 0; i < GP_SITES; i++) {
        if (!gp_sites[i].used)
            continue;
        used++;
        if (gp_sites[i].key_dso < GP_MAX_DSOS)
            per_dso[gp_sites[i].key_dso] += gp_sites[i].count;
    }

    double total = (double) gp_samples;
    double oncpu = (double) (gp_samples_guest + gp_samples_kernel + gp_samples_native);
    uint64_t window = gp_last_ns > gp_first_ns ? gp_last_ns - gp_first_ns : 0;

    dprintf(guestprof_fd,
        "\n=== guestprof: guest PC sampling, %u us interval ===\n"
        "window %.3f s   ticks %llu   samples %llu   distinct sites %u%s\n"
        "tasks sampled %u   mean live tasks/tick %.2f   mean cores busy %.2f\n"
        "sampler overhead %.3f ms total (%.4f%% of window, %.1f us/tick)\n",
        guestprof_interval_us,
        window / 1e9,
        (unsigned long long) gp_ticks,
        (unsigned long long) gp_samples,
        used, gp_sites_dropped ? " (TABLE FULL, counts are low)" : "",
        gp_tasks_seen, gp_ticks ? (double) gp_samples / (double) gp_ticks : 0.0,
        gp_ticks ? oncpu / (double) gp_ticks : 0.0,
        gp_overhead_ns / 1e6,
        window ? 100.0 * (double) gp_overhead_ns / (double) window : 0.0,
        gp_ticks ? (double) gp_overhead_ns / (double) gp_ticks / 1000.0 : 0.0);

    dprintf(guestprof_fd,
        "\nstate split (share of all task-samples; BLOCKED is off-CPU wait)\n"
        "  guest code   %8llu  %6.2f%%\n"
        "  kernel       %8llu  %6.2f%%\n"
        "  native prog  %8llu  %6.2f%%\n"
        "  blocked      %8llu  %6.2f%%\n"
        "  --> outside guest code: %.2f%% of all samples, %.2f%% of on-CPU samples\n",
        (unsigned long long) gp_samples_guest,  100.0 * gp_samples_guest / total,
        (unsigned long long) gp_samples_kernel, 100.0 * gp_samples_kernel / total,
        (unsigned long long) gp_samples_native, 100.0 * gp_samples_native / total,
        (unsigned long long) gp_samples_blocked, 100.0 * gp_samples_blocked / total,
        100.0 * (total - gp_samples_guest) / total,
        oncpu > 0 ? 100.0 * (oncpu - gp_samples_guest) / oncpu : 0.0);

    // DSOs, hottest first.
    struct { unsigned dso; uint64_t count; } order[GP_MAX_DSOS];
    unsigned n = 0;
    for (unsigned i = 0; i < gp_dso_n; i++)
        if (per_dso[i] > 0)
            order[n].dso = i, order[n].count = per_dso[i], n++;
    for (unsigned i = 0; i + 1 < n; i++)
        for (unsigned j = i + 1; j < n; j++)
            if (order[j].count > order[i].count) {
                unsigned d = order[i].dso; uint64_t c = order[i].count;
                order[i].dso = order[j].dso; order[i].count = order[j].count;
                order[j].dso = d; order[j].count = c;
            }

    // Two denominators, because they answer different questions. "all" is
    // share of wall time and includes a shell parked in wait4 -- which in a
    // one-command run is half the samples and has nothing to do with the
    // workload. "on-CPU" excludes blocked time, and is the one that bounds
    // what any accelerator could win back.
    // %%wall is samples/TICKS, not samples/total-samples. With N tasks live
    // there are N task-samples per tick, so dividing by the sample total
    // understates a library's wall share by a factor of N -- on `apt install`
    // that was 7.2%% against a true 28.8%%. It can exceed 100%% when two tasks
    // are in the same object at once, which is real parallelism, not an error.
    dprintf(guestprof_fd, "\nby object   (%%wall = share of WALL TIME: samples/ticks, so it can\n             exceed 100%% when tasks run in parallel. %%on-CPU excludes\n             blocked time and is what bounds an accelerator's win.)\n    %%wall  %%on-CPU     samples  object\n");
    for (unsigned i = 0; i < n; i++) {
        bool is_blocked = order[i].dso == GP_DSO_BLOCKED;
        char oncpu_col[16];
        if (is_blocked || oncpu <= 0)
            snprintf(oncpu_col, sizeof(oncpu_col), "%8s", "-");
        else
            snprintf(oncpu_col, sizeof(oncpu_col), "%7.2f%%",
                    100.0 * (double) order[i].count / oncpu);
        dprintf(guestprof_fd, "  %7.2f%%  %s  %8llu  %s\n",
                gp_ticks ? 100.0 * (double) order[i].count / (double) gp_ticks : 0.0,
                oncpu_col,
                (unsigned long long) order[i].count,
                gp_dso_name[order[i].dso]);
    }

    // Hot offsets inside each real DSO, for host-side symbolization, plus the
    // kernel's own split by syscall number.
    struct gp_site *sorted = malloc(sizeof(*sorted) * used);
    if (sorted != NULL) {
        unsigned k = 0;
        for (unsigned i = 0; i < GP_SITES && k < used; i++)
            if (gp_sites[i].used)
                sorted[k++] = gp_sites[i];
        qsort(sorted, k, sizeof(*sorted), gp_site_cmp);

        dprintf(guestprof_fd, "\nhot offsets (feed to tools/guestprof-symbolize.py)\n");
        for (unsigned d = 0; d < n; d++) {
            unsigned dso = order[d].dso;
            if (dso == GP_DSO_BLOCKED || dso == GP_DSO_NATIVE)
                continue;
            unsigned shown = 0;
            for (unsigned i = 0; i < k && shown < 64; i++) {
                if (sorted[i].key_dso != dso)
                    continue;
                if (shown == 0)
                    dprintf(guestprof_fd, "  %s\n", gp_dso_name[dso]);
                if (dso == GP_DSO_NOMAP)
                    dprintf(guestprof_fd, "    %6.2f%%  %8llu  guest pc %#llx (no file-backed exec mapping)\n",
                            100.0 * (double) sorted[i].count / total,
                            (unsigned long long) sorted[i].count,
                            (unsigned long long) sorted[i].key_off);
                else if (dso == GP_DSO_KERNEL)
                    dprintf(guestprof_fd, "    %6.2f%%  %8llu  syscall %s/%llu\n",
                            100.0 * (double) sorted[i].count / total,
                            (unsigned long long) sorted[i].count,
                            gp_abi_name((uint32_t) (sorted[i].key_off >> 32)),
                            (unsigned long long) (sorted[i].key_off & 0xffffffffULL));
                else
                    dprintf(guestprof_fd, "    %6.2f%%  %8llu  SYM %s +0x%llx\n",
                            100.0 * (double) sorted[i].count / total,
                            (unsigned long long) sorted[i].count,
                            gp_dso_name[dso],
                            (unsigned long long) sorted[i].key_off);
                shown++;
            }
        }
        free(sorted);
    }

    if (gp_maps_evicted != 0)
        dprintf(guestprof_fd, "\nnote: %u address-space snapshots evicted "
                "(GP_MAX_MAPS=%d); a process whose map was evicted while still "
                "running re-takes it\n", gp_maps_evicted, GP_MAX_MAPS);
    unsigned exhausted = atomic_load_explicit(&gp_slots_exhausted, memory_order_relaxed);
    if (exhausted != 0)
        dprintf(guestprof_fd, "\nWARNING: %u threads found no free slot "
                "(GP_SLOTS=%d); their time is missing entirely\n", exhausted, GP_SLOTS);
    dprintf(guestprof_fd, "=== end guestprof ===\n");
}
