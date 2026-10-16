// The 32-bit LOCK instructions together: CMPXCHG8B, XADD and CMPXCHG, each for
// its result and flags and under contention from several threads.
#include <pthread.h>
#include "atomic_common.h"

static void test_lock_cmpxchg8b_single(uint64_t expected, uint64_t initial, uint64_t desired) {
    volatile uint64_t mem = initial;
    uint32_t eax = (uint32_t) expected;
    uint32_t edx = (uint32_t) (expected >> 32);
    unsigned long eflags;
    uint64_t expected_mem = expected == initial ? desired : initial;
    uint64_t expected_acc = expected == initial ? expected : initial;
    uint32_t expected_zf = expected == initial ? CC_Z : 0;

    asm volatile(
        "lock cmpxchg8b %2\n\t"
        "pushf\n\t"
        "pop %3\n\t"
        : "+a"(eax), "+d"(edx), "+m"(mem), "=r"(eflags)
        : "b"((uint32_t) desired), "c"((uint32_t) (desired >> 32))
        : "cc", "memory");

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

static void test_lock_cmpxchgl_single(uint32_t eax_init) {
    volatile uint32_t mem = 0xfbca7654u;
    uint32_t eax = eax_init;
    uint32_t src = 0x12345678u;
    unsigned long eflags;
    uint32_t expected_result = eax_init - mem;
    uint32_t expected_flags = sub_flags32(eax_init, mem, expected_result);
    uint32_t expected_mem = eax_init == mem ? src : mem;
    uint32_t expected_eax = eax_init == mem ? eax_init : mem;

    asm volatile(
        "lock cmpxchgl %4, %1\n\t"
        "pushf\n\t"
        "pop %2\n\t"
        : "=a"(eax), "+m"(mem), "=r"(eflags)
        : "0"(eax), "r"(src)
        : "cc", "memory");

    test_logf("lock cmpxchgl eax=%08" PRIx32 ": eax=%08" PRIx32 " mem=%08" PRIx32 " flags=%03" PRIx32 "\n",
              eax_init, eax, mem, (uint32_t) (eflags & CC_MASK));

    if (eax != expected_eax || mem != expected_mem || (eflags & CC_MASK) != expected_flags)
        failf("lock cmpxchgl single", eax, mem, (uint32_t) (eflags & CC_MASK),
              expected_eax, expected_mem, expected_flags);
}

static inline uint32_t lock_xadd_u32(volatile uint32_t *ptr, uint32_t value) {
    asm volatile("lock xaddl %0, %1"
                 : "+r"(value), "+m"(*ptr)
                 :
                 : "cc", "memory");
    return value;
}

static inline int lock_cmpxchg_u32(volatile uint32_t *ptr, uint32_t expected,
                                   uint32_t desired, uint32_t *actual_out) {
    unsigned long eflags;
    asm volatile(
        "lock cmpxchgl %4, %1\n\t"
        "pushf\n\t"
        "pop %2\n\t"
        : "=a"(expected), "+m"(*ptr), "=r"(eflags)
        : "0"(expected), "r"(desired)
        : "cc", "memory");
    *actual_out = expected;
    return (eflags & CC_Z) != 0;
}

static inline int lock_cmpxchg_u64(volatile uint64_t *ptr, uint64_t expected,
                                   uint64_t desired, uint64_t *actual_out) {
    uint32_t eax = (uint32_t) expected;
    uint32_t edx = (uint32_t) (expected >> 32);
    unsigned long eflags;

    asm volatile(
        "lock cmpxchg8b %2\n\t"
        "pushf\n\t"
        "pop %3\n\t"
        : "=a"(eax), "=d"(edx), "=m"(*ptr), "=r"(eflags)
        : "0"(eax), "1"(edx), "m"(*ptr),
          "b"((uint32_t) desired), "c"((uint32_t) (desired >> 32))
        : "cc", "memory");
    *actual_out = ((uint64_t) edx << 32) | eax;
    return (eflags & CC_Z) != 0;
}

static void test_cmpxchg_jcc_single(uint32_t eax_init, uint32_t initial, uint32_t desired) {
    volatile uint32_t mem = initial;
    uint32_t eax = eax_init;
    uint32_t branch = 0;

    asm volatile(
        "lock cmpxchgl %3, %2\n\t"
        "jnz 1f\n\t"
        "movl $1, %0\n\t"
        "jmp 2f\n"
        "1:\n\t"
        "movl $0, %0\n"
        "2:\n\t"
        : "=r"(branch), "+a"(eax), "+m"(mem)
        : "r"(desired)
        : "cc", "memory");

    test_logf("lock cmpxchgl jz eax=%08" PRIx32 " mem0=%08" PRIx32
              " -> branch=%" PRIu32 " eax=%08" PRIx32 " mem=%08" PRIx32 "\n",
              eax_init, initial, branch, eax, mem);

    if (branch != (eax_init == initial) || eax != (eax_init == initial ? eax_init : initial) ||
        mem != (eax_init == initial ? desired : initial))
        failf("lock cmpxchgl jcc", branch, eax, mem,
              eax_init == initial, eax_init == initial ? eax_init : initial,
              eax_init == initial ? desired : initial);
}

enum { THREADS = 4, LOOPS = 100000 };
enum { VECTOR_LOOPS = 2000 };

static volatile uint32_t shared_xadd;
static volatile uint32_t shared_cmpxchg;
static volatile uint64_t shared_cmpxchg8b;

struct worker_result {
    uint64_t sum_old_values;
    uint32_t retries;
};

static void *xadd_worker(void *arg) {
    struct worker_result *result = arg;
    uint64_t sum = 0;
    for (unsigned i = 0; i < LOOPS; i++)
        sum += lock_xadd_u32(&shared_xadd, 1);
    result->sum_old_values = sum;
    result->retries = 0;
    return NULL;
}

static void *cmpxchg_worker(void *arg) {
    struct worker_result *result = arg;
    uint32_t retries = 0;
    for (unsigned i = 0; i < LOOPS; i++) {
        for (;;) {
            uint32_t old = shared_cmpxchg;
            uint32_t actual;
            if (lock_cmpxchg_u32(&shared_cmpxchg, old, old + 1, &actual))
                break;
            retries++;
        }
    }
    result->sum_old_values = 0;
    result->retries = retries;
    return NULL;
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
    result->sum_old_values = 0;
    result->retries = retries;
    return NULL;
}

static void run_threads(void *(*worker)(void *), volatile uint32_t *shared,
                        const char *name, int print_sum) {
    pthread_t threads[THREADS];
    struct worker_result results[THREADS];
    uint64_t total_sum = 0;
    uint32_t total_retries = 0;
    uint32_t expected = THREADS * LOOPS;

    *shared = 0;
    for (unsigned i = 0; i < THREADS; i++)
        pthread_create(&threads[i], NULL, worker, &results[i]);
    for (unsigned i = 0; i < THREADS; i++) {
        pthread_join(threads[i], NULL);
        total_sum += results[i].sum_old_values;
        total_retries += results[i].retries;
    }

    uint64_t expected_sum = print_sum ? (uint64_t) expected * (expected - 1) / 2 : total_sum;
    int failed = *shared != expected || (print_sum && total_sum != expected_sum);
    test_log_if(failed, "%s stress: final=%08" PRIx32 " expected=%08" PRIx32,
                name, *shared, expected);
    if (print_sum) {
        test_log_if(failed, " sum=%016" PRIx64 " expected_sum=%016" PRIx64,
                    total_sum, expected_sum);
    }
    test_log_if(failed, " retries=%" PRIu32 "\n", total_retries);

    if (*shared != expected)
        failf(name, *shared, total_retries, total_sum, expected,
              total_retries, expected_sum);
    if (print_sum) {
        if (total_sum != expected_sum)
            failf("lock xaddl stress sum", total_sum, *shared, total_retries,
                  expected_sum, expected, total_retries);
    }
}

static void run_threads64(void *(*worker)(void *), volatile uint64_t *shared, const char *name) {
    pthread_t threads[THREADS];
    struct worker_result results[THREADS];
    uint32_t total_retries = 0;
    uint64_t expected = (uint64_t) THREADS * LOOPS;

    *shared = 0;
    for (unsigned i = 0; i < THREADS; i++)
        pthread_create(&threads[i], NULL, worker, &results[i]);
    for (unsigned i = 0; i < THREADS; i++) {
        pthread_join(threads[i], NULL);
        total_retries += results[i].retries;
    }

    int failed = *shared != expected;
    test_log_if(failed, "%s stress: final=%016" PRIx64 " expected=%016" PRIx64 " retries=%" PRIu32 "\n",
                name, *shared, expected, total_retries);

    if (*shared != expected)
        failf(name, *shared, total_retries, 0, expected, total_retries, 0);
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

static void test_lock_cmpxchgl_vectors(void) {
    uint32_t seed = 0x2468ace1u;
    unsigned failures = 0;

    for (unsigned i = 0; i < VECTOR_LOOPS; i++) {
        volatile uint32_t mem = next_u32(&seed);
        uint32_t initial_mem = mem;
        uint32_t eax = next_u32(&seed);
        uint32_t initial_eax = eax;
        uint32_t src = next_u32(&seed);
        unsigned long eflags;
        uint32_t result = initial_eax - initial_mem;
        uint32_t expected_flags = sub_flags32(initial_eax, initial_mem, result);
        uint32_t expected_mem = initial_eax == initial_mem ? src : initial_mem;
        uint32_t expected_eax = initial_eax == initial_mem ? initial_eax : initial_mem;

        asm volatile(
            "lock cmpxchgl %4, %1\n\t"
            "pushf\n\t"
            "pop %2\n\t"
            : "=a"(eax), "+m"(mem), "=r"(eflags)
            : "0"(eax), "r"(src)
            : "cc", "memory");

        if (eax != expected_eax || mem != expected_mem || (eflags & CC_MASK) != expected_flags)
            failures++;
    }

    test_log_if(failures != 0, "lock cmpxchgl vectors: loops=%u failures=%u\n", VECTOR_LOOPS, failures);
    failures_total += failures;
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

        asm volatile(
            "lock cmpxchg8b %2\n\t"
            "pushf\n\t"
            "pop %3\n\t"
            : "+a"(eax), "+d"(edx), "+m"(mem), "=r"(eflags)
            : "b"((uint32_t) desired), "c"((uint32_t) (desired >> 32))
            : "cc", "memory");

        if ((((uint64_t) edx << 32) | eax) != expected_acc || mem != expected_mem ||
            ((eflags & CC_Z) != expected_zf))
            failures++;
    }

    test_log_if(failures != 0, "lock cmpxchg8b vectors: loops=%u failures=%u\n", VECTOR_LOOPS, failures);
    failures_total += failures;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    test_lock_xaddl_single();
    test_lock_cmpxchgl_single(0xfbca7654u);
    test_lock_cmpxchgl_single(0xfffefdfcu);
    test_lock_cmpxchg8b_single(UINT64_C(0x000fbca765423456),
                               UINT64_C(0x000fbca765423456),
                               UINT64_C(0x0006532432432434));
    test_lock_cmpxchg8b_single(UINT64_C(0x000123456789abcd),
                               UINT64_C(0x000fbca765423456),
                               UINT64_C(0x0006532432432434));
    test_cmpxchg_jcc_single(0xfbca7654u, 0xfbca7654u, 0x12345678u);
    test_cmpxchg_jcc_single(0xfffefdfcu, 0xfbca7654u, 0x12345678u);
    test_lock_xaddl_vectors();
    test_lock_cmpxchgl_vectors();
    test_lock_cmpxchg8b_vectors();
    run_threads(xadd_worker, &shared_xadd, "lock xaddl", 1);
    run_threads(cmpxchg_worker, &shared_cmpxchg, "lock cmpxchgl", 0);
    run_threads64(cmpxchg8b_worker, &shared_cmpxchg8b, "lock cmpxchg8b");
    return finish_suite("atomics32");
}
