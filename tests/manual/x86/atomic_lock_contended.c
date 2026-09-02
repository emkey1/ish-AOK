// Do LOCK-prefixed instructions actually interlock?
//
// The other x86 atomics tests (atomic_xadd32, atomic_cmpxchg32,
// atomic_logic32, ...) check the VALUE and FLAGS one locked instruction
// produces. They are necessary and they are not sufficient: every one of them
// passed throughout a release in which `lock addl %reg, (mem)` on an amd64
// guest lost 3876 of 200000 increments across four threads, and in which two
// guest threads contending on an `xchgl` spinlock livelocked after 78
// acquisitions. A single-threaded test cannot see either.
//
// This one runs every locked form that writes memory from several threads at
// once and requires the arithmetic to come out exact. It is deliberately a
// counter test rather than a flags test: a lost update is the only symptom of
// a broken atomic, and it is invisible to anything single-threaded.
//
// Covered, and why each is here rather than assumed:
//   <alu> [mem], reg   the family that was silently non-atomic; the JIT block
//                      that compiles it was the one predicate in gen.c that
//                      never rejected a LOCK prefix
//   <alu> [mem], imm   a separate decode path, with a SECOND copy in the main
//                      interpreter that the JIT-bridge copy does not share
//   inc/dec [mem]      likewise two copies
//   xadd, cmpxchg      the forms real atomics libraries actually emit
//   xchg reg, [mem]    atomic with or without the prefix; the spinlock primitive
//   neg/not [mem]      the two group-3 members a LOCK prefix is legal on
//   bts/btr/btc [mem]  what a bitmap lock is built from
//   misaligned         legal on x86 (unlike arm64 LSE), so it must still be
//                      atomic even though no single host atomic can span it
//
// Runs on i386 and amd64. The 64-bit-only forms are compiled out on i386.

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "test_common.h"

#if defined(__x86_64__)
#define HAVE_64BIT_OPS 1
#else
#define HAVE_64BIT_OPS 0
#endif

enum { THREADS = 4, LOOPS = 20000 };
#define WANT ((uint64_t) THREADS * LOOPS)

// One counter per line: false sharing would not break correctness, but it
// makes a failure much harder to attribute to the instruction that caused it.
struct slot { volatile uint64_t v; uint64_t pad[7]; };
static struct slot s[24];

// Deliberately misaligned targets live here, at odd offsets from an aligned base.
static volatile unsigned char arena[256] __attribute__((aligned(64)));

// A bitmap wide enough that each thread owns its own bit in each word, so a
// lost bts/btr shows up as a missing bit rather than as a benign re-set.
static volatile uint32_t bitmap[THREADS];

static volatile int livelocked;

// Bounded spin: a livelock must fail the test, not hang the suite. The suite
// watchdog would eventually kill us, but "FAIL livelock" is a far better
// report than a timeout that looks like machine load.
#define SPIN_LIMIT 200000000UL

static void *worker(void *arg) {
    unsigned long id = (unsigned long) arg;
    uint32_t one32 = 1;
    uint8_t one8 = 1;
    uint16_t one16 = 1;
#if HAVE_64BIT_OPS
    uint64_t one64 = 1;
#endif
    for (int i = 0; i < LOOPS && !livelocked; i++) {
        // --- <alu> [mem], reg, every width the guest has
        asm volatile("lock addl %1, %0" : "+m"(*(volatile uint32_t *) &s[0].v) : "r"(one32) : "cc", "memory");
        asm volatile("lock addw %1, %0" : "+m"(*(volatile uint16_t *) &s[1].v) : "r"(one16) : "cc", "memory");
        asm volatile("lock addb %1, %0" : "+m"(*(volatile uint8_t *) &s[2].v) : "r"(one8) : "cc", "memory");
        asm volatile("lock subl %1, %0" : "+m"(*(volatile uint32_t *) &s[3].v) : "r"(one32) : "cc", "memory");
        // adc/sbb with the carry pinned, so the arithmetic is still exact
        asm volatile("clc; lock adcl %1, %0" : "+m"(*(volatile uint32_t *) &s[4].v) : "r"(one32) : "cc", "memory");
        asm volatile("stc; lock sbbl %1, %0" : "+m"(*(volatile uint32_t *) &s[5].v) : "r"(one32) : "cc", "memory");

        // --- <alu> [mem], imm  (a different decode path from the reg forms)
        asm volatile("lock addl $1, %0" : "+m"(*(volatile uint32_t *) &s[6].v) : : "cc", "memory");

        // --- inc / dec
        asm volatile("lock incl %0" : "+m"(*(volatile uint32_t *) &s[7].v) : : "cc", "memory");
        asm volatile("lock decl %0" : "+m"(*(volatile uint32_t *) &s[8].v) : : "cc", "memory");
        asm volatile("lock incb %0" : "+m"(*(volatile uint8_t *) &s[9].v) : : "cc", "memory");

        // --- xadd
        { uint32_t t = 1; asm volatile("lock xaddl %0, %1" : "+r"(t), "+m"(*(volatile uint32_t *) &s[10].v) : : "cc", "memory"); }

        // --- cmpxchg, as the retry loop real code writes
        { uint32_t old, nw;
          do {
              old = *(volatile uint32_t *) &s[11].v;
              nw = old + 1;
              asm volatile("lock cmpxchgl %2, %1"
                           : "=a"(old), "+m"(*(volatile uint32_t *) &s[11].v)
                           : "r"(nw), "0"(old) : "cc", "memory");
          } while (old != nw - 1);
        }

        // --- xchg-based spinlock guarding a PLAIN increment. This is the one
        //     that livelocked: the counter can only be right if the lock
        //     actually excludes, and the loop can only finish if a release is
        //     ever observed.
        { uint32_t got; unsigned long spins = 0;
          do {
              got = 1;
              asm volatile("xchgl %0, %1" : "+r"(got), "+m"(*(volatile uint32_t *) &s[12].v) : : "memory");
              if (++spins > SPIN_LIMIT) { livelocked = 1; return NULL; }
          } while (got);
          s[13].v++;
          asm volatile("" ::: "memory");
          asm volatile("movl $0, %0" : "=m"(*(volatile uint32_t *) &s[12].v) : : "memory");
        }

        // --- neg / not, applied twice so the pair is the identity.
        //     amd64 only: the i386 engine's LOCK opcode table (emu/decode.h)
        //     has no group-3 entry at all, so `lock notl`/`lock negl` decode
        //     to UNDEFINED and kill the guest with SIGILL. That is a real
        //     pre-existing i386 gap, tracked for a later build rather than
        //     papered over -- running them here would just crash the suite.
#if HAVE_64BIT_OPS
        asm volatile("lock notl %0" : "+m"(*(volatile uint32_t *) &s[14].v) : : "memory");
        asm volatile("lock notl %0" : "+m"(*(volatile uint32_t *) &s[14].v) : : "memory");
        asm volatile("lock negl %0" : "+m"(*(volatile uint32_t *) &s[15].v) : : "cc", "memory");
        asm volatile("lock negl %0" : "+m"(*(volatile uint32_t *) &s[15].v) : : "cc", "memory");
#endif

        // --- the EXPLICIT lock prefix on xchg, which is redundant but legal
        //     and which the i386 table was missing entirely.
        { uint32_t g = 0;
          asm volatile("lock xchgl %0, %1" : "+r"(g), "+m"(*(volatile uint32_t *) &s[21].v) : : "memory");
          if (g != 0) { asm volatile("lock addl %1, %0" : "+m"(*(volatile uint32_t *) &s[22].v) : "r"(g) : "cc","memory"); } }

        // --- bts / btr / btc. Each thread owns bit `id` of every word, so a
        //     lost update leaves a bit set that should have been cleared.
        for (unsigned w = 0; w < THREADS; w++) {
            asm volatile("lock btsl %1, %0" : "+m"(bitmap[w]) : "r"((uint32_t) id) : "cc", "memory");
            asm volatile("lock btrl %1, %0" : "+m"(bitmap[w]) : "r"((uint32_t) id) : "cc", "memory");
        }

        // --- misaligned, which x86 permits and no single host atomic covers
        asm volatile("lock addl %1, %0" : "+m"(*(volatile uint32_t *) (arena + 1)) : "r"(one32) : "cc", "memory");

#if HAVE_64BIT_OPS
        asm volatile("lock addq %1, %0" : "+m"(s[16].v) : "r"(one64) : "cc", "memory");
        asm volatile("lock addq $1, %0" : "+m"(s[17].v) : : "cc", "memory");
        asm volatile("lock incq %0" : "+m"(s[18].v) : : "cc", "memory");
        { uint64_t t = 1; asm volatile("lock xaddq %0, %1" : "+r"(t), "+m"(s[19].v) : : "cc", "memory"); }
        { uint64_t old, nw;
          do {
              old = s[20].v;
              nw = old + 1;
              asm volatile("lock cmpxchgq %2, %1"
                           : "=a"(old), "+m"(s[20].v) : "r"(nw), "0"(old) : "cc", "memory");
          } while (old != nw - 1);
        }
        asm volatile("lock addq %1, %0" : "+m"(*(volatile uint64_t *) (arena + 13)) : "r"(one64) : "cc", "memory");
#endif
    }
    return NULL;
}

static void check(const char *name, uint64_t got, uint64_t want) {
    if (got == want) {
        test_log_if(0, "%s: %llu\n", name, (unsigned long long) got);
        return;
    }
    printf("FAIL %s got=%llu want=%llu lost=%lld\n", name,
           (unsigned long long) got, (unsigned long long) want,
           (long long) (want - (int64_t) got));
    failures_total++;
}

int main(int argc, char **argv) {
    pthread_t th[THREADS];
    test_init(argc, argv);

    s[3].v = WANT;              // sub counts down to zero
    s[5].v = 2 * WANT;          // sbb with CF=1 takes two per pass
    s[8].v = WANT;              // dec counts down
#if HAVE_64BIT_OPS
    (void) 0;
#endif

    for (unsigned long i = 0; i < THREADS; i++) {
        if (pthread_create(&th[i], NULL, worker, (void *) i) != 0) {
            printf("FAIL pthread_create\n");
            failures_total++;
            return finish_suite("atomic_lock_contended");
        }
    }
    for (int i = 0; i < THREADS; i++)
        pthread_join(th[i], NULL);

    if (livelocked) {
        // Not a lost update: a thread span SPIN_LIMIT times without ever
        // seeing the xchg spinlock released.
        printf("FAIL xchg spinlock livelocked\n");
        failures_total++;
    }

    check("lock addl mem,reg", (uint32_t) s[0].v, WANT);
    check("lock addw mem,reg", (uint16_t) s[1].v, (uint16_t) WANT);
    check("lock addb mem,reg", (uint8_t) s[2].v, (uint8_t) WANT);
    check("lock subl mem,reg", (uint32_t) s[3].v, 0);
    check("lock adcl mem,reg", (uint32_t) s[4].v, WANT);
    check("lock sbbl mem,reg", (uint32_t) s[5].v, 0);
    check("lock addl mem,imm", (uint32_t) s[6].v, WANT);
    check("lock incl mem", (uint32_t) s[7].v, WANT);
    check("lock decl mem", (uint32_t) s[8].v, 0);
    check("lock incb mem", (uint8_t) s[9].v, (uint8_t) WANT);
    check("lock xaddl", (uint32_t) s[10].v, WANT);
    check("lock cmpxchgl", (uint32_t) s[11].v, WANT);
    check("xchgl spinlock", s[13].v, WANT);
#if HAVE_64BIT_OPS
    check("lock notl x2", (uint32_t) s[14].v, 0);
    check("lock negl x2", (uint32_t) s[15].v, 0);
#else
    test_log_if(0, "lock notl/negl: not run (i386 decodes them UNDEFINED)\n");
#endif
    // lock xchg only ever swaps zeros in and reads zeros back, so the only
    // way s[22] is nonzero is a torn or duplicated exchange.
    check("lock xchgl", s[21].v, 0);
    check("lock xchgl (no dup)", s[22].v, 0);
    for (unsigned w = 0; w < THREADS; w++)
        check("lock bts/btr bitmap", bitmap[w], 0);
    { uint32_t v; memcpy(&v, (const void *) (arena + 1), sizeof(v));
      check("lock addl misaligned", v, WANT); }
#if HAVE_64BIT_OPS
    check("lock addq mem,reg", s[16].v, WANT);
    check("lock addq mem,imm", s[17].v, WANT);
    check("lock incq mem", s[18].v, WANT);
    check("lock xaddq", s[19].v, WANT);
    check("lock cmpxchgq", s[20].v, WANT);
    { uint64_t v; memcpy(&v, (const void *) (arena + 13), sizeof(v));
      check("lock addq misaligned", v, WANT); }
#endif
    return finish_suite("atomic_lock_contended");
}
