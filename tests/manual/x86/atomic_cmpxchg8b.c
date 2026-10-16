// LOCK CMPXCHG8B: the result and flags for each case, random vectors, and a
// multi-threaded 64-bit counter.
#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include "atomic_common.h"

enum { THREADS = 4, LOOPS = 100000 };
enum { VECTOR_LOOPS = 2000 };

static volatile uint64_t shared_cmpxchg8b;

struct worker_result {
    uint32_t retries;
};

static inline void raw_lock_cmpxchg8b_u64(volatile uint64_t *ptr, uint32_t desired_lo,
                                          uint32_t desired_hi, uint32_t *eax_io,
                                          uint32_t *edx_io, unsigned long *eflags_out) {
    uint32_t eax = *eax_io;
    uint32_t edx = *edx_io;
    unsigned long eflags;
    asm volatile(
        ".byte 0xf0, 0x0f, 0xc7, 0x0f\n\t" /* lock cmpxchg8b (%rdi) */
        "pushf\n\t"
        "pop %q[flags]\n\t"
        : "+a"(eax), "+d"(edx), [flags] "=r"(eflags), "+m"(*ptr)
        : "D"(ptr), "b"(desired_lo), "c"(desired_hi)
        : "cc", "memory");
    *eax_io = eax;
    *edx_io = edx;
    *eflags_out = eflags;
}

static inline int lock_cmpxchg_u64(volatile uint64_t *ptr, uint64_t expected,
                                   uint64_t desired, uint64_t *actual_out) {
    uint32_t eax = (uint32_t) expected;
    uint32_t edx = (uint32_t) (expected >> 32);
    unsigned long eflags;

    raw_lock_cmpxchg8b_u64(ptr, (uint32_t) desired, (uint32_t) (desired >> 32),
                           &eax, &edx, &eflags);
    *actual_out = ((uint64_t) edx << 32) | eax;
    return (eflags & CC_Z) != 0;
}

static void test_lock_cmpxchg8b_single(uint64_t expected, uint64_t initial, uint64_t desired) {
    volatile uint64_t mem = initial;
    uint32_t eax = (uint32_t) expected;
    uint32_t edx = (uint32_t) (expected >> 32);
    unsigned long eflags;
    uint64_t expected_mem = expected == initial ? desired : initial;
    uint64_t expected_acc = expected == initial ? expected : initial;
    uint32_t expected_zf = expected == initial ? CC_Z : 0;

    raw_lock_cmpxchg8b_u64(&mem, (uint32_t) desired, (uint32_t) (desired >> 32),
                           &eax, &edx, &eflags);

    test_logf("lock cmpxchg8b exp=%016" PRIx64 ": eax=%08" PRIx32
              " edx=%08" PRIx32 " mem=%016" PRIx64 " flags=%03" PRIx32 "\n",
              expected, eax, edx, mem, (uint32_t) (eflags & CC_MASK));

    if ((((uint64_t) edx << 32) | eax) != expected_acc || mem != expected_mem ||
        ((eflags & CC_Z) != expected_zf)) {
        failf("lock cmpxchg8b single",
              ((uint64_t) edx << 32) | eax, mem, (uint32_t) (eflags & CC_Z),
              expected_acc, expected_mem, expected_zf);
    }
}

static void test_lock_cmpxchg8b_vectors(void) {
    uint32_t seed = 0xdeadbeefu;
    unsigned failures = 0;

    for (unsigned i = 0; i < VECTOR_LOOPS; i++) {
        volatile uint64_t mem = ((uint64_t) next_u32(&seed) << 32) | next_u32(&seed);
        uint64_t initial_mem = mem;
        uint64_t expected = ((uint64_t) next_u32(&seed) << 32) | next_u32(&seed);
        uint64_t desired = ((uint64_t) next_u32(&seed) << 32) | next_u32(&seed);
        uint32_t eax = (uint32_t) expected;
        uint32_t edx = (uint32_t) (expected >> 32);
        unsigned long eflags;
        uint64_t expected_mem = expected == initial_mem ? desired : initial_mem;
        uint64_t expected_acc = expected == initial_mem ? expected : initial_mem;
        uint32_t expected_zf = expected == initial_mem ? CC_Z : 0;

        raw_lock_cmpxchg8b_u64(&mem, (uint32_t) desired, (uint32_t) (desired >> 32),
                               &eax, &edx, &eflags);

        if ((((uint64_t) edx << 32) | eax) != expected_acc || mem != expected_mem ||
            ((eflags & CC_Z) != expected_zf))
            failures++;
    }

    test_log_if(failures != 0, "lock cmpxchg8b vectors: loops=%u failures=%u\n", VECTOR_LOOPS, failures);
    failures_total += failures;
}

static void *cmpxchg8b_worker(void *arg) {
    struct worker_result *result = arg;
    uint32_t retries = 0;
    for (unsigned i = 0; i < LOOPS; i++) {
        uint64_t actual = 0;
        for (;;) {
            uint64_t expected = actual;
            if (lock_cmpxchg_u64(&shared_cmpxchg8b, expected, expected + 1, &actual))
                break;
            retries++;
        }
    }
    result->retries = retries;
    return NULL;
}

static void run_cmpxchg8b_stress(void) {
    pthread_t threads[THREADS];
    struct worker_result results[THREADS];
    uint32_t total_retries = 0;
    uint64_t expected = (uint64_t) THREADS * LOOPS;

    shared_cmpxchg8b = 0;
    for (unsigned i = 0; i < THREADS; i++)
        pthread_create(&threads[i], NULL, cmpxchg8b_worker, &results[i]);
    for (unsigned i = 0; i < THREADS; i++) {
        pthread_join(threads[i], NULL);
        total_retries += results[i].retries;
    }

    int failed = shared_cmpxchg8b != expected;
    test_log_if(failed, "lock cmpxchg8b stress: final=%016" PRIx64 " expected=%016" PRIx64 " retries=%" PRIu32 "\n",
                shared_cmpxchg8b, expected, total_retries);

    if (shared_cmpxchg8b != expected)
        failf("lock cmpxchg8b stress", shared_cmpxchg8b, total_retries, 0, expected, total_retries, 0);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    test_lock_cmpxchg8b_single(UINT64_C(0x000fbca765423456),
                               UINT64_C(0x000fbca765423456),
                               UINT64_C(0x0006532432432434));
    test_lock_cmpxchg8b_single(UINT64_C(0x000123456789abcd),
                               UINT64_C(0x000fbca765423456),
                               UINT64_C(0x0006532432432434));
    test_lock_cmpxchg8b_vectors();
    if (getenv("ISH_AOK_STRESS") != NULL)
        run_cmpxchg8b_stress();
    return finish_suite("atomic_cmpxchg8b");
}
