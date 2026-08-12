// jalr_retcache.c -- the riscv64 JIT's jalr target/return cache.
//
// This engine leaves the JIT almost exclusively through jalr (an instrumented
// count put exits and jalr executions one apart), so gadget_riscv64_jalr_cached
// looks the computed target up in jit_frame.ret_cache and, on a hit, dispatches
// straight into that block instead of returning to the C frontend. The cache is
// filled by the frontend at dispatch time and holds block->code pointers; an
// entry is validated by reading block->addr back before it is used.
//
// Two things have to hold, and this pins both:
//
//   1. A hit must dispatch to the block for THAT address. A direct-mapped
//      4096-entry table keyed by a hash of the guest address will collide, so
//      every entry is validated; a missing or wrong validation shows up as a
//      call landing in some other function.
//
//   2. A hit must NOT dispatch to a block that has been invalidated. This is
//      the delicate half. Invalidated blocks stay allocated until a write-locked
//      cleanup, and the frontend's cleanup_seq purge -- which is what clears
//      this cache -- only runs when the thread next returns to C, which is the
//      very thing a cache hit avoids. A chained block-to-block edge is safe
//      there because jit_block_disconnect restores its jump_ip; a cache entry
//      has no such backlink, so the gadget checks block->is_jetsam itself.
//
// Case 2 is only reachable when the invalidation happens with no exit to C
// between it and the next jalr, so the test does the rewrite from INSIDE the
// callee: the callee stores the new instruction, executes fence.i, and returns.
// Its `ret` is then the first jalr after the invalidation, and its target is the
// caller's post-call block -- exactly the entry that just went stale. Rewriting
// from the outside (case 3) does not reach it: any libc call in between returns
// through a jalr, exits to C, and the purge runs first.
//
// A/B: with the is_jetsam check removed, case 2 returns the PREVIOUS round's
// constant on round 1. Passes with the retcache knob both on and off
// (/proc/ish/riscv64_jit_fuse); with it off the gadget without the lookup is
// emitted, and every case still has to pass.
//
// riscv64 guest only: the code under test is hand-assembled RV64I.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int verbose;
#define V(...) do { if (verbose) printf(__VA_ARGS__); } while (0)

// ---- RV64I encodings ------------------------------------------------------
#define INSN_ADDI_SP_M16 0xff010113u // addi sp, sp, -16
#define INSN_SD_RA_8SP   0x00113423u // sd   ra, 8(sp)
#define INSN_LD_RA_8SP   0x00813083u // ld   ra, 8(sp)
#define INSN_ADDI_SP_16  0x01010113u // addi sp, sp, 16
#define INSN_RET         0x00008067u // jalr x0, 0(ra)
#define INSN_JALR_RA_A0  0x000500e7u // jalr ra, 0(a0)

// addi a0, x0, k -- the return value, materialized in one instruction so a
// single word rewrite changes what the function produces. The immediate is
// 12-bit SIGNED and sign-extends into a0, so every constant here stays in
// 1..2047 (K_MAX); 2048 comes back as 0xfffff800 and would look like a stale
// dispatch rather than the encoding limit it is.
#define K_MAX 2047u
static uint32_t insn_li_a0(unsigned k) {
    return ((k & 0xfffu) << 20) | 0x513u;
}

#define STUB_WORDS 2      // li a0, k ; ret
#define NSTUBS     64
// The caller under test:
//   0: addi sp, sp, -16
//   1: sd   ra, 8(sp)
//   2: jalr ra, 0(a0)     <- calls the helper passed in a0
//   3: addi a0, x0, K     <- RETURN SITE: its own block, and what gets rewritten
//   4: ld   ra, 8(sp)
//   5: addi sp, sp, 16
//   6: jalr x0, 0(ra)
#define CALLER_WORDS 7
#define CALLER_K_IDX 3

static volatile uint32_t *page;     // one RWX page
static volatile uint32_t *caller;   // CALLER_WORDS at page[0]
static volatile uint32_t *stubs;    // NSTUBS * STUB_WORDS after it

typedef unsigned (*caller_fn)(void *);
typedef unsigned (*stub_fn)(void);

static caller_fn caller_entry(void) {
    return (caller_fn) (uintptr_t) caller;
}
static stub_fn stub_entry(unsigned i) {
    return (stub_fn) (uintptr_t) (stubs + i * STUB_WORDS);
}

static void icache_sync(void) {
    // The guest instruction that tells the JIT to drop its translations. Not
    // __builtin___clear_cache: on riscv64 Linux that may go out as the
    // __riscv_flush_icache syscall, and the point here is to exercise the
    // in-JIT fence_i gadget path.
    __asm__ __volatile__("fence.i" ::: "memory");
}

static unsigned stub_value(unsigned i, unsigned round) {
    return 1 + ((i * 7 + round * 13) % K_MAX);
}

static void write_stub(unsigned i, unsigned k) {
    volatile uint32_t *at = stubs + i * STUB_WORDS;
    at[0] = insn_li_a0(k);
    at[1] = INSN_RET;
}

// ---- case 1: an indirect call must reach the function it names -------------
//
// One jalr instruction, NSTUBS different targets 8 bytes apart, hammered until
// every entry has been through the cache many times. A dropped or wrong
// validation lands in a neighbouring stub and returns its constant.
#define C1_ROUNDS 40

static int test_target_validation(void) {
    for (unsigned i = 0; i < NSTUBS; i++)
        write_stub(i, stub_value(i, 0));
    icache_sync();

    for (unsigned round = 0; round < C1_ROUNDS; round++) {
        for (unsigned i = 0; i < NSTUBS; i++) {
            unsigned want = stub_value(i, 0);
            unsigned got = stub_entry(i)();
            if (got != want) {
                printf("jalr_retcache: FAIL indirect call round %u stub %u: "
                       "got %u want %u (dispatched to the wrong block)\n",
                       round, i, got, want);
                return 1;
            }
        }
    }
    V("indirect call: %u stubs x %u rounds ok\n", NSTUBS, C1_ROUNDS);
    return 0;
}

// ---- case 2: invalidation from inside the callee ---------------------------
//
// The discriminating case. rewrite_helper runs as the callee of `caller`; it
// rewrites caller's post-call constant and executes fence.i, so when it returns,
// the block it is returning INTO has just been invalidated and no exit to C has
// happened since. That return is a jalr, its target is in the cache, and the
// entry names the stale block.
#define C2_ROUNDS 60

static volatile unsigned next_k;

__attribute__((noinline))
static void rewrite_helper(void *self) {
    (void) self; // a0 held the helper's own address at the call
    caller[CALLER_K_IDX] = insn_li_a0(next_k);
    icache_sync();
}

static int test_rewrite_from_callee(void) {
    caller[0] = INSN_ADDI_SP_M16;
    caller[1] = INSN_SD_RA_8SP;
    caller[2] = INSN_JALR_RA_A0;
    caller[CALLER_K_IDX] = insn_li_a0(1);
    caller[4] = INSN_LD_RA_8SP;
    caller[5] = INSN_ADDI_SP_16;
    caller[6] = INSN_RET;
    icache_sync();

    for (unsigned round = 0; round < C2_ROUNDS; round++) {
        // Deliberately never the value already sitting in the caller, so a
        // stale return-site block cannot pass by accident.
        next_k = 1 + ((round * 37 + 5) % K_MAX);
        unsigned got = caller_entry()((void *) rewrite_helper);
        if (got != next_k) {
            printf("jalr_retcache: FAIL callee-rewrite round %u: got %u want %u "
                   "(returned into the invalidated block)\n",
                   round, got, next_k);
            return 1;
        }
    }
    V("callee rewrite: %u rounds ok\n", C2_ROUNDS);
    return 0;
}

// ---- case 3: invalidation between calls ------------------------------------
//
// The ordinary self-modifying-code shape (tests/manual/arm64/smc_stale_block.c's
// same-thread case, in this guest's encoding). Weaker than case 2 -- the purge
// has usually run by the time the call happens -- but it is the shape a real
// JIT-in-the-guest produces, and it costs nothing to keep.
#define C3_ROUNDS 60

static int test_rewrite_between_calls(void) {
    for (unsigned round = 1; round <= C3_ROUNDS; round++) {
        unsigned i = round % NSTUBS;
        unsigned want = stub_value(i, round);
        write_stub(i, want);
        icache_sync();
        unsigned got = stub_entry(i)();
        if (got != want) {
            printf("jalr_retcache: FAIL rewrite round %u stub %u: got %u want %u "
                   "(stale translation)\n", round, i, got, want);
            return 1;
        }
    }
    V("rewrite between calls: %u rounds ok\n", C3_ROUNDS);
    return 0;
}

// ---- case 4: ordinary returns ----------------------------------------------
//
// Compiled calls and returns, not generated ones: a nested call chain whose
// value is wrong if any `ret` resumes at the wrong place. Cheap insurance on
// the hottest path the new gadget touches.
__attribute__((noinline)) static unsigned fib(unsigned n) {
    return n < 2 ? n : fib(n - 1) + fib(n - 2);
}

static int test_plain_returns(void) {
    unsigned got = fib(21);
    if (got != 10946) {
        printf("jalr_retcache: FAIL fib(21) = %u, want 10946\n", got);
        return 1;
    }
    V("plain returns: fib(21) ok\n");
    return 0;
}

int main(int argc, char *argv[]) {
    verbose = argc > 1 && strcmp(argv[1], "-v") == 0;

#if !defined(__riscv) || __riscv_xlen != 64
    printf("jalr_retcache: SKIP (riscv64 guest only)\n");
    (void) page; (void) caller; (void) stubs;
    return 0;
#else
    size_t need = (CALLER_WORDS + NSTUBS * STUB_WORDS) * sizeof(uint32_t);
    void *p = mmap(NULL, (need + 4095) & ~(size_t) 4095,
                   PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        printf("jalr_retcache: SKIP (mmap rwx refused)\n");
        return 0;
    }
    page = p;
    caller = page;
    stubs = page + CALLER_WORDS;

    int rc = 0;
    rc |= test_plain_returns();
    rc |= test_target_validation();
    rc |= test_rewrite_from_callee();
    rc |= test_rewrite_between_calls();

    if (rc == 0)
        printf("jalr_retcache: PASS (%u indirect rounds, %u callee-rewrite, "
               "%u rewrite)\n", C1_ROUNDS, C2_ROUNDS, C3_ROUNDS);
    return rc;
#endif
}
