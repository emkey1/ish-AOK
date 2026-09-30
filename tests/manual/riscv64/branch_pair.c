// riscv64 JIT conditional branches: the constant-compare fusions (gen_riscv64_try_li_branch /
// _andi_branch, the "br" bit of /proc/ish/riscv64_jit_fuse): each fused pair
// must branch exactly as the two instructions do and leave t written, and
// every branch must go the same way forward and backward (the "btfn" gadget
// layouts).
//   li t, K ; b{eq,ne,lt,ge,ltu,geu} rs, t / t, rs
//   andi t, rs, K ; beqz/bnez t, and beq/bne with x0 first
// Expected values are plain C. Run once with `echo br=0 >
// /proc/ish/riscv64_jit_fuse` for the unfused gadgets.
#include <stdint.h>
#include <stdio.h>

static int fails, cases;
#define CHECK(cond, ...) do { cases++; if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static const uint64_t vals[] = {0, 1, 2, 5, 6, 7, 0x7f, 0x80, 0x7ff, 0x800, (uint64_t) -1, (uint64_t) -2,
    (uint64_t) -5, (uint64_t) -2048, (uint64_t) -2049, 0x8000000000000000ull, 0x7fffffffffffffffull,
    0x123456789abcdef0ull};
#define NV (sizeof(vals) / sizeof(vals[0]))

// A branch shape, forward (to 1f) or backward (to 1b, a loop's layout): the
// JIT picks a different gadget layout for each direction ("btfn").
#define FWD(setup, br) setup "\n " br " 1f\n li %1, 0\n j 2f\n1: li %1, 1\n2:"
#define BWD(setup, br) "j 3f\n1: li %1, 1\n j 2f\n3: " setup "\n " br " 1b\n li %1, 0\n2:"

// t is preset to a junk value so a missing write shows.
#define LI_BR(br, k, first, second, want_expr) do { \
    for (unsigned i = 0; i < NV * 2; i++) { \
        uint64_t x = vals[i % NV], t, taken; \
        if (i < NV) \
            __asm__ volatile(FWD("li %0, 0x5a5a\n li %0, " #k, br " " first ", " second ",") \
                             : "=&r"(t), "=&r"(taken) : "r"(x)); \
        else \
            __asm__ volatile(BWD("li %0, 0x5a5a\n li %0, " #k, br " " first ", " second ",") \
                             : "=&r"(t), "=&r"(taken) : "r"(x)); \
        int64_t sx = (int64_t) x, sk = (int64_t) (k); uint64_t ux = x, uk = (uint64_t) (int64_t) (k); \
        (void) sx; (void) sk; (void) ux; (void) uk; \
        CHECK(taken == (uint64_t) (want_expr) && t == (uint64_t) (int64_t) (k), \
              "%s x=%#llx k=%lld: taken %llu t %#llx", br " " first "," second, (unsigned long long) x, \
              (long long) (k), (unsigned long long) taken, (unsigned long long) t); \
    } } while (0)

#define LI_ALL(k) do { \
    LI_BR("beq", k, "%2", "%0", ux == uk); LI_BR("beq", k, "%0", "%2", ux == uk); \
    LI_BR("bne", k, "%2", "%0", ux != uk); LI_BR("bne", k, "%0", "%2", ux != uk); \
    LI_BR("blt", k, "%2", "%0", sx < sk);  LI_BR("blt", k, "%0", "%2", sk < sx); \
    LI_BR("bge", k, "%2", "%0", sx >= sk); LI_BR("bge", k, "%0", "%2", sk >= sx); \
    LI_BR("bltu", k, "%2", "%0", ux < uk); LI_BR("bltu", k, "%0", "%2", uk < ux); \
    LI_BR("bgeu", k, "%2", "%0", ux >= uk); LI_BR("bgeu", k, "%0", "%2", uk >= ux); \
    } while (0)

#define ANDI_BR(br, k, first, second, want_expr) do { \
    for (unsigned i = 0; i < NV * 2; i++) { \
        uint64_t x = vals[i % NV], t, taken; \
        if (i < NV) \
            __asm__ volatile(FWD("li %0, 0x5a5a\n andi %0, %2, " #k, br " " first ", " second ",") \
                             : "=&r"(t), "=&r"(taken) : "r"(x)); \
        else \
            __asm__ volatile(BWD("li %0, 0x5a5a\n andi %0, %2, " #k, br " " first ", " second ",") \
                             : "=&r"(t), "=&r"(taken) : "r"(x)); \
        uint64_t m = x & (uint64_t) (int64_t) (k); \
        CHECK(taken == (uint64_t) (want_expr) && t == m, "%s andi x=%#llx k=%lld", br " " first "," second, \
              (unsigned long long) x, (long long) (k)); \
    } } while (0)

#define ANDI_ALL(k) do { \
    ANDI_BR("beq", k, "%0", "zero", m == 0); ANDI_BR("beq", k, "zero", "%0", m == 0); \
    ANDI_BR("bne", k, "%0", "zero", m != 0); ANDI_BR("bne", k, "zero", "%0", m != 0); \
    } while (0)

// Plain two-register branches, both directions.
#define REG_BR(br, want_expr) do { \
    for (unsigned i = 0; i < NV; i++) for (unsigned j = 0; j < NV; j++) { \
        uint64_t x = vals[i], y = vals[j], taken, fw, bw; \
        __asm__ volatile(FWD("", br " %2, %3,") : "=&r"(taken), "=&r"(fw) : "r"(x), "r"(y)); \
        __asm__ volatile(BWD("", br " %2, %3,") : "=&r"(taken), "=&r"(bw) : "r"(x), "r"(y)); \
        (void) taken; \
        int64_t sx = (int64_t) x, sy = (int64_t) y; (void) sx; (void) sy; \
        CHECK(fw == (uint64_t) (want_expr) && bw == fw, "%s x=%#llx y=%#llx: fwd %llu bwd %llu", br, \
              (unsigned long long) x, (unsigned long long) y, (unsigned long long) fw, \
              (unsigned long long) bw); \
    } } while (0)

int main(void) {
    REG_BR("beq", x == y); REG_BR("bne", x != y); REG_BR("blt", sx < sy);
    REG_BR("bge", sx >= sy); REG_BR("bltu", x < y); REG_BR("bgeu", x >= y);

    LI_ALL(0); LI_ALL(1); LI_ALL(5); LI_ALL(-1); LI_ALL(-2); LI_ALL(31); LI_ALL(-32);
    LI_ALL(0x7f); LI_ALL(2047); LI_ALL(-2048);
    ANDI_ALL(1); ANDI_ALL(2); ANDI_ALL(7); ANDI_ALL(0x80); ANDI_ALL(-1); ANDI_ALL(-2048); ANDI_ALL(0);

    // andi t, t, K: the source is the destination.
    for (unsigned i = 0; i < NV; i++) {
        uint64_t t = vals[i], taken;
        __asm__ volatile("andi %0, %0, 6\n bnez %0, 1f\n li %1, 0\n j 2f\n1: li %1, 1\n2:"
                         : "+r"(t), "=&r"(taken));
        CHECK(t == (vals[i] & 6) && taken == ((vals[i] & 6) != 0), "andi t,t,6; bnez t x=%#llx",
              (unsigned long long) vals[i]);
    }
    // li t, K; beq t, t: both operands are t (not fused; still correct).
    {
        uint64_t t, taken;
        __asm__ volatile("li %0, 9\n beq %0, %0, 1f\n li %1, 0\n j 2f\n1: li %1, 1\n2:" : "=&r"(t), "=&r"(taken));
        CHECK(taken == 1 && t == 9, "li t; beq t, t");
    }
    // A loop driven by the fused compare: counts must match.
    {
        uint64_t n = 0, lim;
        __asm__ volatile("li %0, 0\n1: addi %0, %0, 1\n li %1, 100\n bltu %0, %1, 1b" : "+r"(n), "=&r"(lim));
        CHECK(n == 100 && lim == 100, "li/bltu loop ran %llu", (unsigned long long) n);
    }

    printf("branch_pair: %s (%d cases)\n", fails ? "FAIL" : "PASS", cases);
    return fails != 0;
}
