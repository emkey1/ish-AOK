// SIMD/FP single-register loads and stores in their no-writeback forms --
// [xn, #imm] and the unscaled [xn, #simm9] -- which the arm64 JIT runs
// through offset-fed fast gadgets ("lspec", jit/guest-arm64/simd.S
// vload_single_fast). Checks for S, D and Q:
//   - the value moved, at scaled offsets, negative unscaled offsets, with SP
//     as the base, and into/out of high registers (v29, v31);
//   - a narrower load clears the rest of the V register (architectural);
//   - a store writes exactly its width (neighbours untouched);
//   - a faulting load reports the load's own PC (restart contract).
// And LD1/ST1 of a single lane ([xn], no writeback): only that lane moves.
// Every expectation is plain C, so the file also runs natively as its own
// oracle.
#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#if defined(__linux__)
#include <ucontext.h>

static sigjmp_buf jb;
static volatile uintptr_t fault_pc;
static void on_segv(int sig, siginfo_t *si, void *ctx) {
    (void) sig; (void) si;
    fault_pc = (uintptr_t) ((ucontext_t *) ctx)->uc_mcontext.pc;
    siglongjmp(jb, 1);
}
extern char vl_fault_ld[], vl_fault_st[], vl_fault_lane[];
#endif

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static unsigned char buf[512] __attribute__((aligned(16)));
static void fill(void) { for (int i = 0; i < 512; i++) buf[i] = (unsigned char) (i * 7 + 3); }

typedef struct { uint64_t lo, hi; } v128;

int main(void) {
    fill();
    v128 out;
    // Loads: value and zeroing of the rest of the register.
    __asm__ volatile("movi v29.2d, #0xffffffffffffffff\n ldr s29, [%1, #36]\n str q29, [%0]"
                     :: "r"(&out), "r"(buf) : "v29", "memory");
    { uint32_t w; memcpy(&w, buf + 36, 4);
      CHECK(out.lo == w && out.hi == 0, "ldr s [x,#36]: %llx %llx", (unsigned long long) out.lo, (unsigned long long) out.hi); }
    __asm__ volatile("movi v31.2d, #0xffffffffffffffff\n ldr d31, [%1, #64]\n str q31, [%0]"
                     :: "r"(&out), "r"(buf) : "v31", "memory");
    { uint64_t d; memcpy(&d, buf + 64, 8);
      CHECK(out.lo == d && out.hi == 0, "ldr d [x,#64]"); }
    __asm__ volatile("ldr q0, [%1, #128]\n str q0, [%0]" :: "r"(&out), "r"(buf) : "v0", "memory");
    CHECK(!memcmp(&out, buf + 128, 16), "ldr q [x,#128]");
    __asm__ volatile("add x9, %1, #300\n ldur q1, [x9, #-7]\n str q1, [%0]" :: "r"(&out), "r"(buf) : "x9", "v1", "memory");
    CHECK(!memcmp(&out, buf + 293, 16), "ldur q [x,#-7]");
    __asm__ volatile("movi v2.2d, #0xffffffffffffffff\n add x9, %1, #100\n ldur s2, [x9, #-3]\n str q2, [%0]"
                     :: "r"(&out), "r"(buf) : "x9", "v2", "memory");
    { uint32_t w; memcpy(&w, buf + 97, 4);
      CHECK(out.lo == w && out.hi == 0, "ldur s [x,#-3]"); }
    // SP as the base.
    {
        unsigned char got[16];
        __asm__ volatile("sub sp, sp, #64\n"
                         "ldr q3, [%1, #16]\n str q3, [sp, #32]\n"
                         "ldr q4, [sp, #32]\n str q4, [%0]\n"
                         "add sp, sp, #64" :: "r"(got), "r"(buf) : "v3", "v4", "memory");
        CHECK(!memcmp(got, buf + 16, 16), "q through [sp, #32]");
    }
    // Stores write exactly their width.
    {
        unsigned char dst[64];
        memset(dst, 0xaa, sizeof dst);
        __asm__ volatile("ldr q5, [%1]\n str s5, [%0, #8]\n str d5, [%0, #24]\n stur q5, [%0, #41]"
                         :: "r"(dst), "r"(buf) : "v5", "memory");
        unsigned char want[64];
        memset(want, 0xaa, sizeof want);
        memcpy(want + 8, buf, 4);
        memcpy(want + 24, buf, 8);
        memcpy(want + 41, buf, 16);
        CHECK(!memcmp(dst, want, 64), "str s/d, stur q widths");
    }
    // Every register number round-trips through the fast forms.
    for (int pass = 0; pass < 2; pass++) {
        unsigned char dst[32 * 16];
        memset(dst, 0, sizeof dst);
        __asm__ volatile(
#define R(n) "ldr q" #n ", [%1, #" #n "*8]\n str q" #n ", [%0, #" #n "*16]\n"
            R(0) R(1) R(2) R(3) R(4) R(5) R(6) R(7) R(8) R(9) R(10) R(11) R(12) R(13) R(14) R(15)
            R(16) R(17) R(18) R(19) R(20) R(21) R(22) R(23) R(24) R(25) R(26) R(27) R(28) R(29) R(30) R(31)
#undef R
            :: "r"(dst), "r"(buf) : "memory", "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8", "v9",
              "v10", "v11", "v12", "v13", "v14", "v15", "v16", "v17", "v18", "v19", "v20", "v21", "v22",
              "v23", "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31");
        for (int n = 0; n < 32; n++)
            CHECK(!memcmp(dst + n * 16, buf + n * 8, 16), "q%d round trip", n);
    }
    // Register offset: [xn, xm, lsl #s], [xn, wm, sxtw #s] (negative index),
    // [xn, wm, uxtw] (index with bit 31 set must not sign-extend).
    {
        v128 o;
        __asm__ volatile("mov x10, #5\n ldr q8, [%1, x10, lsl #4]\n str q8, [%0]"
                         :: "r"(&o), "r"(buf) : "x10", "v8", "memory");
        CHECK(!memcmp(&o, buf + 80, 16), "ldr q [x, x, lsl #4]");
        __asm__ volatile("movi v9.2d, #0xffffffffffffffff\n mov w10, #-3\n ldr s9, [%1, w10, sxtw #2]\n str q9, [%0]"
                         :: "r"(&o), "r"(buf + 256) : "x10", "v9", "memory");
        { uint32_t w; memcpy(&w, buf + 244, 4);
          CHECK(o.lo == w && o.hi == 0, "ldr s [x, w, sxtw #2] (-3)"); }
        __asm__ volatile("mov w10, #0x80000000\n sub x11, %1, x10\n ldr d10, [x11, w10, uxtw]\n str q10, [%0]"
                         :: "r"(&o), "r"(buf + 72) : "x10", "x11", "v10", "memory");
        { uint64_t d; memcpy(&d, buf + 72, 8);
          CHECK(o.lo == d && o.hi == 0, "ldr d [x, w, uxtw] (0x80000000)"); }
        unsigned char dst[64], want[64];
        memset(dst, 0x33, 64);
        __asm__ volatile("ldr q11, [%1]\n mov x10, #2\n str q11, [%0, x10, lsl #4]\n"
                         "mov w10, #-1\n add x12, %0, #16\n str s11, [x12, w10, sxtw #2]\n"
                         "mov w10, #7\n str d11, [%0, w10, uxtw]"
                         :: "r"(dst), "r"(buf + 160) : "x10", "x12", "v11", "memory");
        memset(want, 0x33, 64);
        memcpy(want + 32, buf + 160, 16);
        memcpy(want + 12, buf + 160, 4);
        memcpy(want + 7, buf + 160, 8);
        CHECK(!memcmp(dst, want, 64), "str q/s/d register offset");
    }
    // Pairs, offset form (and the non-temporal LDNP/STNP, same lowering).
    {
        v128 o[2];
        __asm__ volatile("ldp q12, q13, [%1, #32]\n stp q12, q13, [%0]"
                         :: "r"(o), "r"(buf) : "v12", "v13", "memory");
        CHECK(!memcmp(o, buf + 32, 32), "ldp q, q [x, #32]");
        __asm__ volatile("movi v14.2d, #0xffffffffffffffff\n movi v15.2d, #0xffffffffffffffff\n"
                         "ldp s14, s15, [%1, #-8]\n str q14, [%0]\n str q15, [%0, #16]"
                         :: "r"(o), "r"(buf + 100) : "v14", "v15", "memory");
        { uint32_t a, b; memcpy(&a, buf + 92, 4); memcpy(&b, buf + 96, 4);
          CHECK(o[0].lo == a && o[0].hi == 0 && o[1].lo == b && o[1].hi == 0, "ldp s, s [x, #-8]"); }
        unsigned char dst[64], want[64];
        memset(dst, 0x77, 64);
        __asm__ volatile("ldr q16, [%1]\n ldr q17, [%1, #16]\n stp d16, d17, [%0, #8]\n stnp q17, q16, [%0, #32]"
                         :: "r"(dst), "r"(buf + 64) : "v16", "v17", "memory");
        memset(want, 0x77, 64);
        memcpy(want + 8, buf + 64, 8); memcpy(want + 16, buf + 80, 8);
        memcpy(want + 32, buf + 80, 16); memcpy(want + 48, buf + 64, 16);
        CHECK(!memcmp(dst, want, 64), "stp d, d / stnp q, q");
        unsigned char got[32];
        __asm__ volatile("sub sp, sp, #64\n ldp q18, q19, [%1]\n stp q19, q18, [sp, #16]\n"
                         "ldnp q20, q21, [sp, #16]\n stp q20, q21, [%0]\n add sp, sp, #64"
                         :: "r"(got), "r"(buf + 144) : "v18", "v19", "v20", "v21", "memory");
        CHECK(!memcmp(got, buf + 160, 16) && !memcmp(got + 16, buf + 144, 16), "pair through [sp, #16]");
    }
    // LD1 {Vt.T}[lane], [xn]: the lane gets the bytes, the rest of Vt stays.
    {
        unsigned char base[16], got[16], want[16];
        for (int i = 0; i < 16; i++) base[i] = (unsigned char) (0xc0 + i);
#define LANE_LD(T, lane, bytes) do { \
        __asm__ volatile("ldr q30, [%1]\n ld1 {v30." T "}[" #lane "], [%2]\n str q30, [%0]" \
                         :: "r"(got), "r"(base), "r"(buf + 200) : "v30", "memory"); \
        memcpy(want, base, 16); memcpy(want + (lane) * (bytes), buf + 200, bytes); \
        CHECK(!memcmp(got, want, 16), "ld1 {v30." T "}[" #lane "]"); } while (0)
        LANE_LD("b", 0, 1); LANE_LD("b", 13, 1); LANE_LD("h", 3, 2); LANE_LD("h", 7, 2);
        LANE_LD("s", 1, 4); LANE_LD("s", 3, 4); LANE_LD("d", 0, 8); LANE_LD("d", 1, 8);
#undef LANE_LD
    }
    // ST1 {Vt.T}[lane], [xn]: exactly the lane's bytes land.
    {
        unsigned char dst[32], want[32];
#define LANE_ST(T, lane, bytes) do { \
        memset(dst, 0x55, 32); \
        __asm__ volatile("ldr q7, [%1]\n st1 {v7." T "}[" #lane "], [%0]" \
                         :: "r"(dst + 8), "r"(buf + 48) : "v7", "memory"); \
        memset(want, 0x55, 32); memcpy(want + 8, buf + 48 + (lane) * (bytes), bytes); \
        CHECK(!memcmp(dst, want, 32), "st1 {v7." T "}[" #lane "]"); } while (0)
        LANE_ST("b", 5, 1); LANE_ST("h", 6, 2); LANE_ST("s", 2, 4); LANE_ST("d", 1, 8);
#undef LANE_ST
    }
#if defined(__linux__)
    // A fault on the fast path reports the load's / store's own PC.
    {
        char *page = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        mprotect(page + 4096, 4096, PROT_NONE);
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = on_segv;
        sa.sa_flags = SA_SIGINFO;
        sigaction(SIGSEGV, &sa, NULL);
        fault_pc = 0;
        if (!sigsetjmp(jb, 1))
            __asm__ volatile("mov x9, %0\n .globl vl_fault_ld\n vl_fault_ld: ldr q6, [x9, #16]"
                             :: "r"(page + 4096) : "x9", "v6", "memory");
        CHECK(fault_pc == (uintptr_t) vl_fault_ld, "fault pc %#lx want %#lx", (unsigned long) fault_pc,
              (unsigned long) (uintptr_t) vl_fault_ld);
        fault_pc = 0;
        if (!sigsetjmp(jb, 1))
            __asm__ volatile("mov x9, %0\n .globl vl_fault_st\n vl_fault_st: stur d6, [x9, #-8]"
                             :: "r"(page + 4096 + 8) : "x9", "v6", "memory");
        CHECK(fault_pc == (uintptr_t) vl_fault_st, "store fault pc");
        fault_pc = 0;
        if (!sigsetjmp(jb, 1))
            __asm__ volatile("mov x9, %0\n .globl vl_fault_lane\n vl_fault_lane: ld1 {v6.s}[2], [x9]"
                             :: "r"(page + 4096) : "x9", "v6", "memory");
        CHECK(fault_pc == (uintptr_t) vl_fault_lane, "lane load fault pc");
    }
#endif
    printf("vldst_lspec: %s\n", fails ? "FAIL" : "PASS");
    return fails != 0;
}
