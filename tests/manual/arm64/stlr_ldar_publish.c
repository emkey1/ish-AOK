// stlr_ldar_publish.c — LDAR/STLR cross-thread publication ordering.
//
// Regression test for the arm64-guest JIT lowering LDAR/STLR to PLAIN
// load/store gadgets with no host barrier (jit/gen.c, exclusive/ordered
// family, o2=1 branch). Guest threads run on concurrent host threads on a
// weakly-ordered host (Apple Silicon), so dropping the ordering lets a
// consumer thread observe a release-published pointer/flag BEFORE the data
// it guards. V8 publishes SharedFunctionInfo/BytecodeArray state to its
// concurrent Sparkplug/GC threads exactly this way; the real-world symptom
// was `npm install -g @anthropic-ai/claude-code` on an Alpine arm64 guest
// dying ~50% of runs (LoadIC probing a feedback slot that belongs to a
// different IC kind after baseline code was compiled from half-published
// state; also V8 JIT-page CHECK failures and raw SIGSEGV).
//
// This is a classic message-passing litmus test (MP): the producer writes
// a data word with a plain store then publishes a sequence number with
// STLR; the consumer polls the flag with LDAR and, on seeing a new value,
// checks the data word matches. Any mismatch = release/acquire violation.
// The producer's data line is deliberately made contention-heavy (spinner
// threads hammer reads of it) to widen the store-buffer window on the
// unfixed build. NOTE: emulation inserts many host instructions between
// the two guest stores, so the unfixed build's violation is probabilistic
// — a pass here is necessary but not sufficient; the authoritative A/B for
// the original bug is the npm-install repro. A violation, however, is
// always a real bug, and this also pins decode coverage for LDAR/STLR.
//
// Passes on real Linux and on fixed AOK. Exits non-zero on any violation.

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ITERS 400000
#define SPINNERS 2

static int verbose;

// separate cache lines
static __attribute__((aligned(128))) volatile uint64_t data;
static __attribute__((aligned(128))) volatile uint64_t flag;
static __attribute__((aligned(128))) volatile int stop_spinners;
static __attribute__((aligned(128))) volatile uint64_t ack;

static unsigned long violations;

static void *producer(void *arg) {
    (void) arg;
    for (uint64_t i = 1; i <= ITERS; i++) {
        data = i * 3;                    // plain store
        __asm__ volatile("stlr %0, [%1]" :: "r"(i), "r"(&flag) : "memory");
        while (ack != i)                 // wait for consumer round-trip
            ;
    }
    return NULL;
}

static void *consumer(void *arg) {
    (void) arg;
    for (uint64_t i = 1; i <= ITERS; i++) {
        uint64_t f;
        do {
            __asm__ volatile("ldar %0, [%1]" : "=r"(f) : "r"(&flag) : "memory");
        } while (f != i);
        uint64_t d = data;               // plain load after acquire
        if (d != i * 3) {
            violations++;
            if (verbose)
                printf("violation at iter %llu: flag=%llu data=%llu (want %llu)\n",
                       (unsigned long long) i, (unsigned long long) f,
                       (unsigned long long) d, (unsigned long long) (i * 3));
        }
        ack = i;                         // plain store; producer spins on it
    }
    return NULL;
}

// hammer reads of the data line to keep it contended, widening the window
// in which the producer's data store sits in the store buffer
static void *spinner(void *arg) {
    (void) arg;
    volatile uint64_t sink = 0;
    while (!stop_spinners)
        sink += data;
    return NULL;
}

int main(int argc, char *argv[]) {
    verbose = argc > 1 && strcmp(argv[1], "-v") == 0;

    pthread_t prod, cons, spin[SPINNERS];
    for (int s = 0; s < SPINNERS; s++)
        pthread_create(&spin[s], NULL, spinner, NULL);
    pthread_create(&cons, NULL, consumer, NULL);
    pthread_create(&prod, NULL, producer, NULL);
    pthread_join(prod, NULL);
    pthread_join(cons, NULL);
    stop_spinners = 1;
    for (int s = 0; s < SPINNERS; s++)
        pthread_join(spin[s], NULL);

    if (violations != 0) {
        printf("stlr_ldar_publish: FAIL %lu release/acquire violations in %d iters\n",
               violations, ITERS);
        return 1;
    }
    printf("stlr_ldar_publish: PASS (%d iterations, no violations)\n", ITERS);
    return 0;
}
