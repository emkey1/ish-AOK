// LOCK XADD on 32 bits: the exchanged register, memory and flags, random
// vectors, and a multi-threaded sum.
#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "atomic_common.h"

enum { THREADS = 4, LOOPS = 1000 };
enum { VECTOR_LOOPS = 512 };

static volatile uint32_t shared_xadd;

struct worker_result {
    uint64_t sum_old_values;
    uint32_t retries;
};

static inline uint32_t lock_xadd_u32(volatile uint32_t *ptr, uint32_t value) {
    asm volatile("lock xaddl %0, %1"
                 : "+r"(value), "+m"(*ptr)
                 :
                 : "cc", "memory");
    return value;
}

static void test_lock_xaddl_single(void) {
    volatile uint32_t mem = 0x12345678u;
    uint32_t reg = 0xfbca7654u;
    unsigned long eflags;
    uint32_t expected_mem = mem + reg;
    uint32_t expected_reg = mem;
    uint32_t expected_flags = add_flags32(mem, reg, expected_mem);

    asm volatile(
        "lock xaddl %0, %1\n\t"
        "pushf\n\t"
        "pop %2\n\t"
        : "+r"(reg), "+m"(mem), "=r"(eflags)
        :
        : "cc", "memory");

    test_logf("lock xaddl single: reg=%08" PRIx32 " mem=%08" PRIx32 " flags=%03" PRIx32 "\n",
              reg, mem, (uint32_t) (eflags & CC_MASK));

    if (reg != expected_reg || mem != expected_mem || (eflags & CC_MASK) != expected_flags)
        failf("lock xaddl single", reg, mem, (uint32_t) (eflags & CC_MASK),
              expected_reg, expected_mem, expected_flags);
}

static void test_lock_xaddl_vectors(void) {
    uint32_t seed = 0x13579bdfu;
    unsigned failures = 0;

    for (unsigned i = 0; i < VECTOR_LOOPS; i++) {
        volatile uint32_t mem = next_u32(&seed);
        uint32_t initial = mem;
        uint32_t reg = next_u32(&seed);
        uint32_t initial_reg = reg;
        unsigned long eflags;
        uint32_t expected_mem = initial + initial_reg;
        uint32_t expected_flags = add_flags32(initial, initial_reg, expected_mem);

        asm volatile(
            "lock xaddl %0, %1\n\t"
            "pushf\n\t"
            "pop %2\n\t"
            : "+r"(reg), "+m"(mem), "=r"(eflags)
            :
            : "cc", "memory");

        if (reg != initial || mem != expected_mem || (eflags & CC_MASK) != expected_flags)
            failures++;
    }

    test_log_if(failures != 0, "lock xaddl vectors: loops=%u failures=%u\n", VECTOR_LOOPS, failures);
    failures_total += failures;
}

static void *xadd_worker(void *arg) {
    struct worker_result *result = arg;
    uint64_t sum = 0;
    for (unsigned i = 0; i < LOOPS; i++)
        sum += lock_xadd_u32(&shared_xadd, 1);
    result->sum_old_values = sum;
    result->retries = 0;
    return NULL;
}

static void run_xadd_stress(void) {
    pthread_t threads[THREADS];
    struct worker_result results[THREADS];
    uint64_t total_sum = 0;
    uint32_t expected = THREADS * LOOPS;

    shared_xadd = 0;
    for (unsigned i = 0; i < THREADS; i++)
        pthread_create(&threads[i], NULL, xadd_worker, &results[i]);
    for (unsigned i = 0; i < THREADS; i++) {
        pthread_join(threads[i], NULL);
        total_sum += results[i].sum_old_values;
    }

    uint64_t expected_sum = (uint64_t) expected * (expected - 1) / 2;
    int failed = shared_xadd != expected || total_sum != expected_sum;
    test_log_if(failed, "lock xaddl stress: final=%08" PRIx32 " expected=%08" PRIx32,
                shared_xadd, expected);
    {
        test_log_if(failed, " sum=%016" PRIx64 " expected_sum=%016" PRIx64 " retries=0\n",
                    total_sum, expected_sum);
        if (shared_xadd != expected)
            failf("lock xaddl stress", shared_xadd, total_sum, 0, expected, expected_sum, 0);
        if (total_sum != expected_sum)
            failf("lock xaddl stress sum", total_sum, shared_xadd, 0, expected_sum, expected, 0);
    }
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    test_lock_xaddl_single();
    test_lock_xaddl_vectors();
    if (getenv("ISH_AOK_STRESS") != NULL)
        run_xadd_stress();
    return finish_suite("atomic_xadd32");
}
