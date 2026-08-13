// ret_retcache.c -- the arm64 JIT's br/blr/ret target cache. The aarch64 twin
// of tests/manual/riscv64/jalr_retcache.c; read that one's header for the full
// argument, and keep the two in step -- they are one test in two dialects.
//
// br, blr and ret are this engine's only remaining exits to C (every
// compile-time-target edge already chains), so ret_cache_dispatch
// (jit/guest-arm64/control.S) looks the computed target up in
// jit_frame.ret_cache and dispatches straight into that block on a hit. Two
// things must hold, and this pins both:
//
//   1. A hit must dispatch to the block for THAT address. The table is
//      direct-mapped and 4096 entries, so collisions are certain and every
//      entry is validated against block->addr; a missing validation lands a
//      call in some other function.
//   2. A hit must NOT dispatch to a block that has been invalidated. The
//      frontend's cleanup_seq purge is what clears this cache, and it only runs
//      when the thread returns to C -- which is what a hit avoids. Unlike a
//      chained edge there is no jump_ip for jit_block_disconnect to unpatch, so
//      the gadget checks block->is_jetsam itself.
//
// Case 2 is only reachable when nothing exits to C between the invalidation and
// the next br/blr/ret, so the rewrite is done from INSIDE the callee: it stores
// the new instruction into the caller's post-call slot, clears the cache, and
// returns. Its `ret` is then the first such branch after the invalidation and
// its target is the entry that just went stale. On this guest that is
// especially clean -- __builtin___clear_cache inlines dc/ic/isb and the guest's
// IC IVAU is itself a gadget (jit/jit.c arm64_ic_ivau) that invalidates just the
// touched PAGE, so the helper's own block, which lives in the main text
// segment, stays valid and keeps running to its return.
//
// A/B with the is_jetsam check removed from ret_cache_dispatch: case 2 returns
// the PREVIOUS round's constant on round 1. Passes with the retcache knob both
// on and off (/proc/ish/arm64_jit_fuse).
//
// arm64 guest only: the code under test is hand-assembled A64.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int verbose;
#define V(...) do { if (verbose) printf(__VA_ARGS__); } while (0)

// ---- A64 encodings --------------------------------------------------------
#define INSN_SUB_SP_16   0xd10043ffu // sub sp, sp, #16
#define INSN_STR_X30_SP8 0xf90007feu // str x30, [sp, #8]
#define INSN_LDR_X30_SP8 0xf94007feu // ldr x30, [sp, #8]
#define INSN_ADD_SP_16   0x910043ffu // add sp, sp, #16
#define INSN_RET         0xd65f03c0u // ret (x30)
#define INSN_BLR_X0      0xd63f0000u // blr x0

// movz w0, #k -- the return value in one instruction, so a single word rewrite
// changes what the function produces. Unlike riscv64's addi form the immediate
// is an unsigned 16-bit field with no sign extension, but the constants are
// held to the same 1..2047 range as the riscv64 twin so the two tests stay
// diffable.
#define K_MAX 2047u
static uint32_t insn_mov_w0(unsigned k) {
    return 0x52800000u | ((k & 0xffffu) << 5);
}

#define STUB_WORDS 2      // movz w0, #k ; ret
#define NSTUBS     64
// The caller under test:
//   0: sub sp, sp, #16
//   1: str x30, [sp, #8]
//   2: blr x0             <- calls the helper passed in x0
//   3: movz w0, #K        <- RETURN SITE: its own block, and what gets rewritten
//   4: ldr x30, [sp, #8]
//   5: add sp, sp, #16
//   6: ret
#define CALLER_WORDS 7
#define CALLER_K_IDX 3

static volatile uint32_t *page;
static volatile uint32_t *caller;
static volatile uint32_t *stubs;

typedef unsigned (*caller_fn)(void *);
typedef unsigned (*stub_fn)(void);

static caller_fn caller_entry(void) { return (caller_fn) (uintptr_t) caller; }
static stub_fn stub_entry(unsigned i) {
    return (stub_fn) (uintptr_t) (stubs + i * STUB_WORDS);
}

// Bulk sync, for the cases where only correctness matters.
static void icache_sync(volatile uint32_t *at, size_t words) {
    __builtin___clear_cache((char *) at, (char *) (at + words));
}

// Single-line sync with NO CALL, for the one place the window matters.
//
// ⚠ This is the whole reason case 2 discriminates. __builtin___clear_cache is
// not inline on this target -- gcc emits `bl __clear_cache` into libgcc -- and
// that callee's OWN `ret` is then the first branch executed after the
// invalidation. If it misses the cache it exits to C, the frontend's
// cleanup_seq purge empties ret_cache, and by the time the helper returns there
// is no stale entry left to catch: the test passes even with the is_jetsam
// check deleted, which is a test that proves nothing. Measured exactly that way
// before this was inlined. The riscv64 twin does not hit it because its
// fence.i is one instruction with no call around it.
//
// Same sequence __clear_cache runs for one line (the rewritten word is 4 bytes,
// so one line each is enough), and every op is supported by the guest: DC CVAU
// is an accepted no-op and IC IVAU is a real gadget that reaches
// jit_invalidate_page (jit/gen.c, jit/jit.c arm64_ic_ivau).
static inline void icache_sync_one_nocall(volatile uint32_t *at) {
    __asm__ __volatile__(
        "dc cvau, %0\n\t"
        "dsb ish\n\t"
        "ic ivau, %0\n\t"
        "dsb ish\n\t"
        "isb\n\t"
        :: "r" (at) : "memory");
}

static unsigned stub_value(unsigned i, unsigned round) {
    return 1 + ((i * 7 + round * 13) % K_MAX);
}

static void write_stub(unsigned i, unsigned k) {
    volatile uint32_t *at = stubs + i * STUB_WORDS;
    at[0] = insn_mov_w0(k);
    at[1] = INSN_RET;
}

// ---- case 1: an indirect call must reach the function it names -------------
#define C1_ROUNDS 40

static int test_target_validation(void) {
    for (unsigned i = 0; i < NSTUBS; i++)
        write_stub(i, stub_value(i, 0));
    icache_sync(stubs, NSTUBS * STUB_WORDS);

    for (unsigned round = 0; round < C1_ROUNDS; round++) {
        for (unsigned i = 0; i < NSTUBS; i++) {
            unsigned want = stub_value(i, 0);
            unsigned got = stub_entry(i)();
            if (got != want) {
                printf("ret_retcache: FAIL indirect call round %u stub %u: "
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
#define C2_ROUNDS 60

static volatile unsigned next_k;

__attribute__((noinline))
static void rewrite_helper(void *self) {
    (void) self; // x0 held the helper's own address at the call
    caller[CALLER_K_IDX] = insn_mov_w0(next_k);
    icache_sync_one_nocall(caller + CALLER_K_IDX);
}

static int test_rewrite_from_callee(void) {
    caller[0] = INSN_SUB_SP_16;
    caller[1] = INSN_STR_X30_SP8;
    caller[2] = INSN_BLR_X0;
    caller[CALLER_K_IDX] = insn_mov_w0(1);
    caller[4] = INSN_LDR_X30_SP8;
    caller[5] = INSN_ADD_SP_16;
    caller[6] = INSN_RET;
    icache_sync(caller, CALLER_WORDS);

    for (unsigned round = 0; round < C2_ROUNDS; round++) {
        // Never the value already sitting in the caller, so a stale return-site
        // block cannot pass by accident.
        next_k = 1 + ((round * 37 + 5) % K_MAX);
        unsigned got = caller_entry()((void *) rewrite_helper);
        if (got != next_k) {
            printf("ret_retcache: FAIL callee-rewrite round %u: got %u want %u "
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
// The ordinary self-modifying-code shape, which arm64/smc_stale_block.c already
// covers for the per-thread block cache; kept here so the same source proves the
// cache does not reintroduce it. Weaker than case 2: the purge has usually run
// by the time the call happens.
#define C3_ROUNDS 60

static int test_rewrite_between_calls(void) {
    for (unsigned round = 1; round <= C3_ROUNDS; round++) {
        unsigned i = round % NSTUBS;
        unsigned want = stub_value(i, round);
        write_stub(i, want);
        icache_sync(stubs + i * STUB_WORDS, STUB_WORDS);
        unsigned got = stub_entry(i)();
        if (got != want) {
            printf("ret_retcache: FAIL rewrite round %u stub %u: got %u want %u "
                   "(stale translation)\n", round, i, got, want);
            return 1;
        }
    }
    V("rewrite between calls: %u rounds ok\n", C3_ROUNDS);
    return 0;
}

// ---- case 4: ordinary returns ----------------------------------------------
__attribute__((noinline)) static unsigned fib(unsigned n) {
    return n < 2 ? n : fib(n - 1) + fib(n - 2);
}

static int test_plain_returns(void) {
    unsigned got = fib(21);
    if (got != 10946) {
        printf("ret_retcache: FAIL fib(21) = %u, want 10946\n", got);
        return 1;
    }
    V("plain returns: fib(21) ok\n");
    return 0;
}

int main(int argc, char *argv[]) {
    verbose = argc > 1 && strcmp(argv[1], "-v") == 0;

#if !defined(__aarch64__)
    printf("ret_retcache: SKIP (arm64 guest only)\n");
    (void) page; (void) caller; (void) stubs;
    return 0;
#else
    size_t need = (CALLER_WORDS + NSTUBS * STUB_WORDS) * sizeof(uint32_t);
    void *p = mmap(NULL, (need + 4095) & ~(size_t) 4095,
                   PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        printf("ret_retcache: SKIP (mmap rwx refused)\n");
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
        printf("ret_retcache: PASS (%u indirect rounds, %u callee-rewrite, "
               "%u rewrite)\n", C1_ROUNDS, C2_ROUNDS, C3_ROUNDS);
    return rc;
#endif
}
