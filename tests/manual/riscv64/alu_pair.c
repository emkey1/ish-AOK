// riscv64 JIT ALU-pair fusions (gen_riscv64_try_after_slli/_after_add, the
// "alu" bit of /proc/ish/riscv64_jit_fuse): each fused pair must leave every
// register exactly as the two instructions do.
//   slli rd, rs, a ; srli/srai rd, rd, b    (zext.w, sext, bit fields)
//   slli t, rs, s  ; add rd, t, rb / rb, t  (scaled index), incl. rb == t, rd == t
//   add t, ra, rb  ; l{b,h,w,d}[u] rd, imm(t), incl. rd == t, and a fault in
//                    the load reporting the load's pc with t already written
// Expected values are plain C. Run once with `echo alu=0 >
// /proc/ish/riscv64_jit_fuse` for the unfused gadgets.
#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static const uint64_t vals[] = {0, 1, 0x7f, 0x80, 0xffffffff, 0x80000000, 0x123456789abcdef0ull,
    0xfedcba9876543210ull, ~0ull, 0x8000000000000000ull};
#define NV (sizeof(vals) / sizeof(vals[0]))

#define SHIFT_PAIR(a, b) do { \
    for (unsigned i = 0; i < NV; i++) { \
        uint64_t x = vals[i], r1, r2; \
        __asm__ volatile("slli %0, %1, " #a "\n srli %0, %0, " #b : "=&r"(r1) : "r"(x)); \
        __asm__ volatile("slli %0, %1, " #a "\n srai %0, %0, " #b : "=&r"(r2) : "r"(x)); \
        CHECK(r1 == ((x << (a)) >> (b)), "slli %d srli %d of %#llx: %#llx", a, b, \
              (unsigned long long) x, (unsigned long long) r1); \
        CHECK(r2 == (uint64_t) ((int64_t) (x << (a)) >> (b)), "slli %d srai %d of %#llx", a, b, \
              (unsigned long long) x); \
    } } while (0)

static sigjmp_buf jb;
static volatile uintptr_t fault_pc, fault_t0;
static void on_segv(int sig, siginfo_t *si, void *ctx) {
    (void) sig; (void) si;
    fault_pc = ((ucontext_t *) ctx)->uc_mcontext.__gregs[REG_PC];
    fault_t0 = ((ucontext_t *) ctx)->uc_mcontext.__gregs[5];   // t0 = x5
    siglongjmp(jb, 1);
}
extern char rv_fault_ld[];

int main(void) {
    SHIFT_PAIR(32, 32); SHIFT_PAIR(48, 48); SHIFT_PAIR(56, 56); SHIFT_PAIR(0, 0);
    SHIFT_PAIR(63, 63); SHIFT_PAIR(3, 7); SHIFT_PAIR(20, 1); SHIFT_PAIR(0, 63);

    // slli + add, both operand orders, and the shifted register reused.
    for (unsigned i = 0; i < NV; i++) for (unsigned j = 0; j < NV; j++) {
        uint64_t x = vals[i], y = vals[j], t, r;
        __asm__ volatile("slli %0, %2, 3\n add %1, %0, %3" : "=&r"(t), "=&r"(r) : "r"(x), "r"(y));
        CHECK(t == x << 3 && r == y + (x << 3), "slli 3; add rd, t, rb");
        __asm__ volatile("slli %0, %2, 2\n add %1, %3, %0" : "=&r"(t), "=&r"(r) : "r"(x), "r"(y));
        CHECK(t == x << 2 && r == y + (x << 2), "slli 2; add rd, rb, t");
        __asm__ volatile("slli %0, %1, 1\n add %0, %0, %0" : "=&r"(t) : "r"(x));
        CHECK(t == (x << 1) * 2, "slli 1; add t, t, t");
        __asm__ volatile("mv %0, %2\n slli %0, %0, 4\n add %0, %0, %1" : "=&r"(t) : "r"(y), "r"(x));
        CHECK(t == (x << 4) + y, "slli t, t, 4; add t, t, rb");
    }

    // add + load, every width and sign.
    static unsigned char buf[64] __attribute__((aligned(16)));
    for (int i = 0; i < 64; i++) buf[i] = (unsigned char) (0x80 + i * 13);
    uintptr_t base = (uintptr_t) buf;
    long idx = 8;
#define ADD_LOAD(insn, type, off) do { \
        uint64_t t, r; \
        __asm__ volatile("li %0, 0x5a5a\n add %0, %2, %3\n " insn " %1, " #off "(%0)" : "=&r"(t), "=&r"(r) : "r"(base), "r"(idx)); \
        type want; memcpy(&want, buf + 8 + (off), sizeof(want)); \
        CHECK(t == base + 8 && r == (uint64_t) (int64_t) want, insn " off " #off ": %#llx", (unsigned long long) r); \
    } while (0)
    ADD_LOAD("lb", int8_t, 3); ADD_LOAD("lbu", uint8_t, 3); ADD_LOAD("lh", int16_t, -6);
    ADD_LOAD("lhu", uint16_t, 4); ADD_LOAD("lw", int32_t, 8); ADD_LOAD("lwu", uint32_t, -4);
    ADD_LOAD("ld", int64_t, 16);
    {   // the load's destination is the sum register
        uint64_t t;
        __asm__ volatile("add %0, %1, %2\n ld %0, 0(%0)" : "=&r"(t) : "r"(base), "r"(idx));
        uint64_t want; memcpy(&want, buf + 8, 8);
        CHECK(t == want, "add t; ld t, 0(t)");
    }

    // A fault in the load: pc is the load's, and t already holds the sum.
    {
        char *page = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        mprotect(page + 4096, 4096, PROT_NONE);
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = on_segv;
        sa.sa_flags = SA_SIGINFO;
        sigaction(SIGSEGV, &sa, NULL);
        static volatile uint64_t t_after;
        if (!sigsetjmp(jb, 1)) {
            __asm__ volatile("li t0, 0\n add t0, %0, %1\n .globl rv_fault_ld\n rv_fault_ld: ld t1, 8(t0)\n"
                             "sd t0, 0(%2)"
                             :: "r"(page + 4000), "r"(96L), "r"(&t_after) : "t0", "t1", "memory");
        }
        CHECK(fault_pc == (uintptr_t) rv_fault_ld, "fault pc %#lx want %#lx", (unsigned long) fault_pc,
              (unsigned long) (uintptr_t) rv_fault_ld);
        CHECK(fault_t0 == (uintptr_t) (page + 4000 + 96), "t0 at the fault %#lx (the add must be done)",
              (unsigned long) fault_t0);
    }

    printf("alu_pair: %s\n", fails ? "FAIL" : "PASS");
    return fails != 0;
}
