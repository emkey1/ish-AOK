// The /AOK/tools benchmarks, compiled in and runnable natively.
//
// /AOK/tools/iSH_benchmark.tgz carries bmm (integer/float/memory loops) and
// bmt (thread creation), which setup-ish-benchmark.sh extracts and builds in
// the guest with gcc. Those are the EMULATED numbers: the point of the
// benchmark is what the emulator costs.
//
// The same workloads compiled in here run as HOST code on the guest task's
// thread, so `bmm` under /AOK/native and a guest-built `bmm` measure the same
// program with and without emulation -- which is the comparison the benchmark
// exists to make, available without a compiler in the guest and identical on
// every guest ABI.
//
// Two honesty notes, printed with the results rather than buried here:
//
//   The guest Makefile builds bmm/bmt at -O0 and bmm2/bmt2 at -O2. This code
//   is built with AOK's own flags, so it corresponds to the -O2 pair; against
//   the -O0 ones it is not a like-for-like comparison.
//
//   Native bmt creates HOST threads, not guest tasks. That is exactly the
//   quantity worth comparing -- guest clone() versus host pthread_create -- but
//   it is a different kind of object, and a host that refuses at some depth is
//   reported rather than hidden.
//
// Everything here is function-local. A native program is a function call that
// the process outlives (kernel/native.h), so a global accumulator or a cached
// fd would leak into the next run.
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "kernel/native.h"
#include "kernel/native_io.h"

// Same barrier bmm.c uses: keeps the accumulator opaque so an optimizing
// build cannot fold the loop into a closed form, while adding no instructions.
#define KEEP(x) __asm__ volatile("" : "+r"(x))

#define BENCH_DEFAULT_ITERATIONS 1000000000LL
#define BENCH_DEFAULT_THREADS    10000

static double bench_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec + (double) ts.tv_nsec / 1e9;
}

// A count from argv[1], else the environment, else the default. Benchmarks get
// run on phones; being able to ask for a smaller one matters.
static long long bench_count(int argc, char *const argv[], char *const envp[],
        const char *env_name, long long fallback) {
    const char *text = NULL;
    if (argc > 1 && argv[1] != NULL)
        text = argv[1];
    if (text == NULL && envp != NULL) {
        size_t len = strlen(env_name);
        for (size_t i = 0; envp[i] != NULL; i++)
            if (strncmp(envp[i], env_name, len) == 0 && envp[i][len] == '=') {
                text = envp[i] + len + 1;
                break;
            }
    }
    if (text == NULL || *text == '\0')
        return fallback;
    char *end = NULL;
    long long v = strtoll(text, &end, 10);
    if (end == text || v <= 0)
        return fallback;
    return v;
}

int native_bmm_main(int argc, char *const argv[], char *const envp[]) {
    long long iterations = bench_count(argc, argv, envp, "ISH_BENCH_ITERATIONS",
            BENCH_DEFAULT_ITERATIONS);

    native_printf(1, "bmm (native): %lld iterations, host code, AOK build flags\n",
            iterations);

    double t0 = bench_now();
    long long isum = 0;
    for (long long i = 0; i < iterations; ++i) {
        isum += i;
        KEEP(isum);
    }
    double t1 = bench_now();
    native_printf(1, "Integer sum: %lld\n", isum);
    native_printf(1, "  integer loop: %.3f s\n", t1 - t0);

    t0 = bench_now();
    double fsum = 0.0;
    for (double i = 0.0; i < (double) iterations; ++i) {
        fsum += i;
        KEEP(fsum);
    }
    t1 = bench_now();
    native_printf(1, "Floating point sum: %f\n", fsum);
    native_printf(1, "  float loop:   %.3f s\n", t1 - t0);

    return 0;
}

static void *bench_thread(void *arg) {
    (void) arg;
    return NULL;
}

int native_bmt_main(int argc, char *const argv[], char *const envp[]) {
    long long want = bench_count(argc, argv, envp, "ISH_BENCH_THREADS",
            BENCH_DEFAULT_THREADS);

    pthread_t *threads = calloc((size_t) want, sizeof(*threads));
    if (threads == NULL) {
        native_printf(2, "bmt: out of memory for %lld thread handles\n", want);
        return 1;
    }

    native_printf(1, "bmt (native): %lld threads, host pthreads\n", want);

    double t0 = bench_now();
    long long made = 0;
    int create_errno = 0;
    for (long long i = 0; i < want; i++) {
        int rc = pthread_create(&threads[i], NULL, bench_thread, NULL);
        if (rc != 0) {
            // A host that will not give us this many is a real answer, not a
            // failure to hide: report where it stopped and time what was made.
            create_errno = rc;
            break;
        }
        made++;
    }
    for (long long i = 0; i < made; i++)
        pthread_join(threads[i], NULL);
    double t1 = bench_now();

    native_printf(1, "Created and joined %lld threads in %.3f s\n", made, t1 - t0);
    if (made > 0)
        native_printf(1, "  %.1f us per thread\n", (t1 - t0) * 1e6 / (double) made);
    if (create_errno != 0)
        native_printf(2, "bmt: the host stopped at %lld threads (pthread_create: %s)\n",
                made, strerror(create_errno));

    free(threads);
    return create_errno == 0 ? 0 : 1;
}
