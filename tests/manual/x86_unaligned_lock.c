// LOCK-prefixed instructions on operands that are not naturally aligned. x86
// makes them atomic at any alignment, against every other access to those
// bytes -- including ordinary stores -- and the i386 ABI makes the 64-bit case
// ordinary: it aligns uint64_t struct fields to 4 bytes, and Python 3.14 does
// a `lock cmpxchg8b` on one during startup.
//
// The host's atomics need natural alignment, so the emulator has to build
// these. It used to take one global lock around a read and a write. That
// serialises locked instructions against each other, but not against a plain
// store from another thread, which can land between the read and the write
// and be lost: up to 60% of them here, on both x86 engines.
//
// Each case runs two threads on one misaligned counter. One does locked
// increments. The other repeatedly writes a fresh marker with a plain store
// and then reads the counter back: since the first thread only ever adds, the
// value can never drop below a marker once that marker is stored. A read that
// finds it lower means the store was lost.
//
// x86 only: an arm64 or riscv64 exclusive on a misaligned address faults on
// real hardware too.
#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include "test_common.h"

#if defined(__i386__) || defined(__x86_64__)

static volatile int stop;

struct job {
    char *where;
    int bits;
    long increments;
    long stores;
    long lost;
    unsigned long long expected;
};

static void plain_store(struct job *j, uint64_t v) {
    if (j->bits == 64) {
#ifdef __x86_64__
        __asm__ volatile("movq %1, %0" : "=m"(*(uint64_t *) j->where) : "r"(v) : "memory");
#else
        __asm__ volatile("movq %1, %%xmm0\n\tmovq %%xmm0, %0"
                         : "=m"(*(uint64_t *) j->where) : "m"(v) : "xmm0", "memory");
#endif
    } else if (j->bits == 32) {
        __asm__ volatile("movl %1, %0" : "=m"(*(uint32_t *) j->where) : "r"((uint32_t) v) : "memory");
    } else {
        __asm__ volatile("movw %1, %0" : "=m"(*(uint16_t *) j->where) : "r"((uint16_t) v) : "memory");
    }
}

static uint64_t plain_load(struct job *j) {
    uint64_t v;
    if (j->bits == 64) {
#ifdef __x86_64__
        __asm__ volatile("movq %1, %0" : "=r"(v) : "m"(*(uint64_t *) j->where) : "memory");
#else
        __asm__ volatile("movq %1, %%xmm0\n\tmovq %%xmm0, %0"
                         : "=m"(v) : "m"(*(uint64_t *) j->where) : "xmm0", "memory");
#endif
        return v;
    }
    if (j->bits == 32)
        return *(volatile uint32_t *) j->where;
    return *(volatile uint16_t *) j->where;
}

static void locked_increment(struct job *j) {
    if (j->bits == 64) {
        // cmpxchg8b on i386, lock xadd on amd64
        __atomic_fetch_add((uint64_t *) j->where, 1, __ATOMIC_SEQ_CST);
    } else if (j->bits == 32) {
        __asm__ volatile("lock incl %0" : "+m"(*(uint32_t *) j->where) : : "memory", "cc");
    } else {
        __asm__ volatile("lock incw %0" : "+m"(*(uint16_t *) j->where) : : "memory", "cc");
    }
}

static void *incrementer(void *p) {
    struct job *j = p;
    while (!stop) {
        locked_increment(j);
        j->increments++;
    }
    return NULL;
}

// Markers climb in big steps, far above anything the incrementer adds between
// two of them, and stay inside the operand's width.
static void *storer(void *p) {
    struct job *j = p;
    uint64_t step = j->bits == 16 ? 0x100 : j->bits == 32 ? 0x100000 : 0x10000000000ull;
    uint64_t limit = j->bits == 16 ? 0xf000 : j->bits == 32 ? 0xf0000000u : 0xf000000000000000ull;
    uint64_t marker = step;
    while (!stop) {
        plain_store(j, marker);
        j->stores++;
        for (int i = 0; i < 64; i++) {
            if (plain_load(j) < marker) {
                j->lost++;
                break;
            }
        }
        marker += step;
        if (marker >= limit)
            marker = step;
        if (marker == step) {
            // wrapped: restart from a clean slate
            plain_store(j, 0);
        }
    }
    return NULL;
}

// Locked increments alone must all land: the count is exact.
static void *counter(void *p) {
    struct job *j = p;
    for (long i = 0; i < 20000; i++)
        locked_increment(j);
    return NULL;
}

static void run_case(const char *name, char *where, int bits) {
    // exactness: four locked incrementers, nothing else
    memset(where, 0, 8);
    struct job cj = { .where = where, .bits = bits };
    pthread_t t[4];
    for (int i = 0; i < 4; i++)
        pthread_create(&t[i], NULL, counter, &cj);
    for (int i = 0; i < 4; i++)
        pthread_join(t[i], NULL);
    uint64_t want = 80000 & (bits == 16 ? 0xffff : ~0ull);
    uint64_t got = plain_load(&cj);
    test_logf("%s: locked count %llu (want %llu)\n", name, (unsigned long long) got,
              (unsigned long long) want);
    if (got != want) {
        printf("FAIL %s: %d-bit locked increments lost updates: %llu, want %llu\n", name, bits,
               (unsigned long long) got, (unsigned long long) want);
        failures_total++;
    }

    // interlock: locked increments against plain stores
    memset(where, 0, 8);
    struct job j = { .where = where, .bits = bits };
    stop = 0;
    pthread_t a, b;
    pthread_create(&a, NULL, incrementer, &j);
    pthread_create(&b, NULL, storer, &j);
    struct timespec ts = { 1, 500000000 };
    nanosleep(&ts, NULL);
    stop = 1;
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    test_logf("%s: %ld increments, %ld plain stores, %ld lost\n", name, j.increments, j.stores, j.lost);
    if (j.increments == 0 || j.stores == 0) {
        printf("FAIL %s: a thread never ran (%ld increments, %ld stores)\n", name, j.increments, j.stores);
        failures_total++;
    } else if (j.lost != 0) {
        printf("FAIL %s: %ld of %ld plain stores lost to a %d-bit locked increment\n", name,
               j.lost, j.stores, bits);
        failures_total++;
    }
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    // A misaligned lock can kill the process outright; keep what came before.
    setvbuf(stdout, NULL, _IONBF, 0);
    char *page = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) {
        printf("FAIL mmap\n");
        return 1;
    }
    char *base = page + 1024;   // 16-byte aligned
    // 64-bit: inside one 16-byte block, across a 16-byte boundary, across a page
    run_case("qword_at_4", base + 4, 64);
    run_case("qword_at_12", base + 12, 64);
    run_case("qword_at_1", base + 1, 64);
    run_case("qword_across_page", page + 4092, 64);
    // 32- and 16-bit: misaligned inside a block, and across a 16-byte boundary.
    // Not yet on i386: its JIT's 16- and 32-bit locked gadgets still run an
    // exclusive load on the misaligned address, which the host faults (SIGBUS)
    // unless it happens to tolerate it -- see docs/TODO.md. The amd64 engine
    // takes these through the same exact path as the 64-bit ones.
#ifndef __i386__
    run_case("dword_at_2", base + 2, 32);
    run_case("dword_at_14", base + 14, 32);
    run_case("word_at_1", base + 1, 16);
    run_case("word_at_15", base + 15, 16);
#endif
    return finish_suite("x86_unaligned_lock");
}

#else

int main(int argc, char **argv) {
    test_init(argc, argv);
    printf("SKIP x86_unaligned_lock: x86 guests only\n");
    return finish_suite("x86_unaligned_lock");
}

#endif
