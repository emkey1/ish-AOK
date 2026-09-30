// arm64 JIT CBZ/CBNZ/TBZ/TBNZ: the offset-fed gadgets ("ospec") and their
// forward/backward layouts ("btfn" in /proc/ish/arm64_jit_fuse). Every form
// must branch exactly as the architecture says, forward and backward: W forms
// test only the low 32 bits, TBZ/TBNZ test one bit at each of the 64
// positions. Expected values are plain C. Run once with `echo ospec=0` and
// once with `echo btfn=0` for the generic gadget and the forward layout.
#include <stdint.h>
#include <stdio.h>

static int fails, cases;
#define CHECK(cond, ...) do { cases++; if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static const uint64_t vals[] = {0, 1, 2, 0x80, 0xffffffff, 0x80000000, 0x100000000ull, 0xffffffff00000000ull,
    0x8000000000000000ull, ~0ull, 0x123456789abcdef0ull, 0x5555555555555555ull, 0xaaaaaaaaaaaaaaaaull};
#define NV (sizeof(vals) / sizeof(vals[0]))

// Forward (to 1f) and backward (to 1b, a loop's layout) shapes of one branch.
#define FWD(br) br " 1f\n mov %0, #0\n b 2f\n1: mov %0, #1\n2:"
#define BWD(br) "b 3f\n1: mov %0, #1\n b 2f\n3: " br " 1b\n mov %0, #0\n2:"

#define CB(br, reg, want_expr) do { \
    for (unsigned i = 0; i < NV; i++) { \
        uint64_t x = vals[i], fw, bw; \
        __asm__ volatile(FWD(br " %" reg "1,") : "=&r"(fw) : "r"(x)); \
        __asm__ volatile(BWD(br " %" reg "1,") : "=&r"(bw) : "r"(x)); \
        CHECK(fw == (uint64_t) (want_expr) && bw == fw, "%s %s x=%#llx: fwd %llu bwd %llu", br, reg, \
              (unsigned long long) x, (unsigned long long) fw, (unsigned long long) bw); \
    } } while (0)

#define TB(br, bit, want_nz) do { \
    for (unsigned i = 0; i < NV; i++) { \
        uint64_t x = vals[i], fw, bw; \
        __asm__ volatile(FWD(br " %x1, #" #bit ",") : "=&r"(fw) : "r"(x)); \
        __asm__ volatile(BWD(br " %x1, #" #bit ",") : "=&r"(bw) : "r"(x)); \
        uint64_t set = (x >> (bit)) & 1; \
        CHECK(fw == (want_nz ? set : !set) && bw == fw, "%s bit %d x=%#llx: fwd %llu bwd %llu", br, bit, \
              (unsigned long long) x, (unsigned long long) fw, (unsigned long long) bw); \
    } } while (0)
#define TB_BOTH(bit) do { TB("tbz", bit, 0); TB("tbnz", bit, 1); } while (0)
#define TB8(b) do { TB_BOTH(b); TB_BOTH(b + 1); TB_BOTH(b + 2); TB_BOTH(b + 3); \
    TB_BOTH(b + 4); TB_BOTH(b + 5); TB_BOTH(b + 6); TB_BOTH(b + 7); } while (0)

int main(void) {
    CB("cbz", "x", x == 0);
    CB("cbnz", "x", x != 0);
    CB("cbz", "w", (uint32_t) x == 0);
    CB("cbnz", "w", (uint32_t) x != 0);
    TB8(0); TB8(8); TB8(16); TB8(24); TB8(32); TB8(40); TB8(48); TB8(56);

    // A counted loop closed by a backward cbnz, and one by a backward tbz.
    {
        uint64_t n = 100, iters = 0;
        __asm__ volatile("1: add %1, %1, #1\n sub %0, %0, #1\n cbnz %0, 1b" : "+r"(n), "+r"(iters));
        CHECK(n == 0 && iters == 100, "cbnz loop ran %llu", (unsigned long long) iters);
    }
    {
        uint64_t n = 0, iters = 0;
        __asm__ volatile("1: add %1, %1, #1\n add %0, %0, #1\n tbz %0, #6, 1b" : "+r"(n), "+r"(iters));
        CHECK(n == 64 && iters == 64, "tbz loop ran %llu", (unsigned long long) iters);
    }

    printf("cbz_tbz: %s (%d cases)\n", fails ? "FAIL" : "PASS", cases);
    return fails != 0;
}
