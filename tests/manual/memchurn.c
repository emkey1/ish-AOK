// memchurn — models musl mallocng's mmap/munmap churn in a multithreaded
// process, the workload that makes AOK's mem-quiesce barrier "horrific".
//
//   ./memchurn <allocators> <workers> <iters> [forkers]
//
// allocators: threads doing mmap/touch/munmap in a loop (each munmap fires the
//             stop-the-world quiesce barrier, poking every sibling).
// workers:    threads spinning on CPU work (running JIT, holding the mem read
//             lock) — these are what the barrier must evict on every munmap.
// iters:      mmap/munmap cycles per allocator thread.
// forkers:    optional processes doing fork/exit in a loop, to contend pids_lock
//             (exercises the "poke no-ops under pids_lock contention" amplifier).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>

static int g_iters;
static volatile int g_stop;
static _Atomic long g_worker_iters; // total useful work done by sibling threads

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void *allocator(void *arg) {
    long n = 0;
    const char *pg = getenv("MEMCHURN_PAGES");
    size_t len = (pg ? atoi(pg) : 16) * 4096; // alloc size; default 64 KiB
    for (int i = 0; i < g_iters; i++) {
        void *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) { perror("mmap"); exit(2); }
        static int leak = -1, notouch = -1;
        if (leak == -1) leak = getenv("MEMCHURN_LEAK") ? 1 : 0;
        if (notouch == -1) notouch = getenv("MEMCHURN_NOTOUCH") ? 1 : 0;
        if (!notouch) memset(p, (char) i, 4096); // fault one page in
        if (!leak && munmap(p, len) != 0) { perror("munmap"); exit(2); } // <- barrier
        n++;
    }
    return (void *) n;
}

static void *worker(void *arg) {
    // Spin doing real CPU work so this thread is executing guest code (holding
    // the mem read lock) and must be poked/evicted by each allocator barrier.
    volatile unsigned long acc = 0;
    while (!__atomic_load_n(&g_stop, __ATOMIC_RELAXED)) {
        for (int i = 0; i < 100000; i++) acc += i * 2654435761u;
        __atomic_fetch_add(&g_worker_iters, 1, __ATOMIC_RELAXED); // one work unit
    }
    return (void *) acc;
}

// Sleeper: blocks forever in a read() (io_block=1, holds no mem read lock).
// Models the many idle sibling threads (poll/futex waiters) in a real process.
// Poking these on every barrier is pure waste — the fix should skip them.
static void *sleeper(void *arg) {
    int *pfd = (int *) arg;
    char c;
    read(pfd[0], &c, 1); // never written → blocks until process exit
    return NULL;
}

static void run_forkers(int forkers) {
    for (int f = 0; f < forkers; f++) {
        if (fork() == 0) {
            while (!__atomic_load_n(&g_stop, __ATOMIC_RELAXED)) {
                pid_t p = fork();
                if (p == 0) _exit(0);
                if (p > 0) waitpid(p, NULL, 0);
            }
            _exit(0);
        }
    }
}

int main(int argc, char **argv) {
    int nalloc   = argc > 1 ? atoi(argv[1]) : 1;
    int nworker  = argc > 2 ? atoi(argv[2]) : 3;
    g_iters      = argc > 3 ? atoi(argv[3]) : 4000;
    int forkers  = argc > 4 ? atoi(argv[4]) : 0;
    int nsleeper = argc > 5 ? atoi(argv[5]) : 0;

    printf("memchurn: %d allocators, %d workers, %d iters, %d forkers, %d sleepers\n",
           nalloc, nworker, g_iters, forkers, nsleeper);

    if (forkers) run_forkers(forkers);

    int spfd[2];
    if (pipe(spfd) != 0) { perror("pipe"); return 2; }
    pthread_t wt[64], at[64], st[64];
    for (int i = 0; i < nsleeper; i++) pthread_create(&st[i], NULL, sleeper, spfd);
    for (int i = 0; i < nworker; i++) pthread_create(&wt[i], NULL, worker, NULL);

    double t0 = now();
    for (int i = 0; i < nalloc; i++) pthread_create(&at[i], NULL, allocator, NULL);
    long total = 0;
    for (int i = 0; i < nalloc; i++) {
        void *r; pthread_join(at[i], &r); total += (long) r;
    }
    double t1 = now();

    __atomic_store_n(&g_stop, 1, __ATOMIC_RELAXED);
    for (int i = 0; i < nworker; i++) pthread_join(wt[i], NULL);

    double secs = t1 - t0;
    long witers = __atomic_load_n(&g_worker_iters, __ATOMIC_RELAXED);
    printf("munmaps=%ld  time=%.3fs  rate=%.0f munmap/s  (%.1f us/munmap)\n",
           total, secs, total / secs, secs * 1e6 / total);
    printf("worker_work=%ld units  (%.0f units/s = sibling throughput while allocator churns)\n",
           witers, witers / secs);
    return 0;
}
