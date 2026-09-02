// smc_stale_block.c — stale JIT translation surviving code invalidation.
//
// Regression test for jit_invalidate_range not bumping cleanup_seq (and the
// per-thread dispatch cache not checking is_jetsam): invalidating a compiled
// block (guest IC IVAU via __builtin___clear_cache, or an SMC write miss)
// disconnected it from the lookup structures but left every thread's
// per-thread block cache, frame->last_block, and assembly ret_cache pointing
// at the STALE translation, which kept executing until an eventual OOM
// flush. Real-world: V8's concurrent Sparkplug thread garbage-collects
// baseline code and reuses the address for a different function; the main
// thread kept running the OLD function's translation against the NEW
// function's frames -> `npm install -g @anthropic-ai/claude-code` on an
// Alpine arm64 guest died ~50% of runs (LoadIC probing a feedback slot of
// the wrong IC kind -> SIGSEGV on a Smi deref, V8 JitPageReference CHECK
// failures, or SIGILL).
//
// Two cases, both rewriting code at a FIXED address and expecting the new
// code to execute:
//   1. same-thread:  write fn v1, call it (JIT compiles + caches), rewrite
//      to v2, __clear_cache, call again -> must return v2's value.
//   2. cross-thread: a writer thread rewrites the function and publishes
//      with C11 release/acquire; the executor calls and must observe the
//      new return value (the V8 concurrent-compile shape).
//
// A/B: unfixed AOK returns the OLD value (fails on the first same-thread
// round); fixed passes. Also passes on real Linux.

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int verbose;
#define V(...) do { if (verbose) printf(__VA_ARGS__); } while (0)

static uint32_t *code;

// mov w0, #n ; ret
static void write_fn(uint32_t *at, unsigned n) {
    at[0] = 0x52800000u | ((n & 0xffffu) << 5);
    at[1] = 0xd65f03c0u;
    __builtin___clear_cache((char *) at, (char *) (at + 2));
}

static unsigned call_fn(uint32_t *at) {
    return ((unsigned (*)(void)) at)();
}

#define ROUNDS 200

static int test_same_thread(void) {
    for (unsigned i = 0; i < ROUNDS; i++) {
        unsigned want = 1 + (i % 1000);
        write_fn(code, want);
        unsigned got = call_fn(code);
        if (got != want) {
            printf("smc_stale_block: FAIL same-thread round %u: got %u want %u (stale translation)\n",
                   i, got, want);
            return 1;
        }
    }
    V("same-thread: %d rounds ok\n", ROUNDS);
    return 0;
}

static _Atomic unsigned publish;   // value the writer just installed
static _Atomic unsigned consumed;  // executor round-trip ack
static _Atomic int writer_stop;

static void *writer(void *arg) {
    (void) arg;
    for (unsigned i = 0; i < ROUNDS && !writer_stop; i++) {
        unsigned want = 10000 + i;
        write_fn(code, want);
        atomic_store_explicit(&publish, want, memory_order_release);
        while (atomic_load_explicit(&consumed, memory_order_acquire) != want &&
               !writer_stop)
            ;
    }
    return NULL;
}

static int test_cross_thread(void) {
    atomic_store(&publish, 0);
    atomic_store(&consumed, 0);
    writer_stop = 0;
    pthread_t w;
    pthread_create(&w, NULL, writer, NULL);
    int bad = 0;
    for (unsigned i = 0; i < ROUNDS; i++) {
        unsigned want = 10000 + i;
        while (atomic_load_explicit(&publish, memory_order_acquire) != want)
            ;
        unsigned got = call_fn(code);
        if (got != want) {
            printf("smc_stale_block: FAIL cross-thread round %u: got %u want %u (stale translation)\n",
                   i, got, want);
            bad = 1;
            break;
        }
        atomic_store_explicit(&consumed, want, memory_order_release);
    }
    writer_stop = 1;
    atomic_store(&consumed, atomic_load(&publish)); // unstick writer
    pthread_join(w, NULL);
    if (!bad)
        V("cross-thread: %d rounds ok\n", ROUNDS);
    return bad;
}

int main(int argc, char *argv[]) {
    verbose = argc > 1 && strcmp(argv[1], "-v") == 0;

    code = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) {
        // some kernels refuse W+X; fall back to toggling mprotect
        printf("smc_stale_block: SKIP (mmap rwx refused)\n");
        return 0;
    }

    int rc = 0;
    rc |= test_same_thread();
    rc |= test_cross_thread();

    if (rc == 0)
        printf("smc_stale_block: PASS (%d + %d rounds)\n", ROUNDS, ROUNDS);
    return rc;
}
