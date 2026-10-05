// SHA512H/H2/SU0/SU1 and CRC32{B,H,W,X}/CRC32C{B,H,W,X} on random operands,
// including Vd aliasing Vn or Vm and WZR/XZR operands, against a model of
// the Arm ARM written here. Run it twice under AOK: plainly (the native
// gadgets on a host with FEAT_SHA512/FEAT_CRC32) and with
// ISH_ARM64_FORCE_SOFT_CRYPTO=1 (the gadgets an older host gets: the A10X
// has no FEAT_SHA512, so the suite there runs the SHA512 soft gadget).
// It builds natively on an arm64 host too, where the hardware checks the
// model.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// The suite builds with plain cc: enable the extensions in the asm itself.
#define EXT ".arch_extension sha3\n.arch_extension crc\n"

typedef struct { uint64_t lo, hi; } v128;
static unsigned long checks, bad;
static uint64_t rs = 0x9e3779b97f4a7c15ull;
static uint64_t rnd(void) {
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    // a share of edge values: all ones, zero, one bit
    switch (rs % 23) {
    case 0: return 0;
    case 1: return ~0ull;
    case 2: return 1ull << (rs >> 58);
    }
    return rs;
}
static void check(const char *what, int i, v128 got, v128 want) {
    checks++;
    if (got.lo != want.lo || got.hi != want.hi) {
        if (bad++ < 20)
            printf("FAIL %s #%d: got %016llx:%016llx want %016llx:%016llx\n", what, i,
                   (unsigned long long) got.hi, (unsigned long long) got.lo,
                   (unsigned long long) want.hi, (unsigned long long) want.lo);
    }
}

// The Arm ARM's SHA512 pseudocode (lane 0 = lo, lane 1 = hi).
static uint64_t ror(uint64_t x, int n) { return x >> n | x << (64 - n); }
static uint64_t S0(uint64_t x) { return ror(x, 28) ^ ror(x, 34) ^ ror(x, 39); }
static uint64_t S1(uint64_t x) { return ror(x, 14) ^ ror(x, 18) ^ ror(x, 41); }
static uint64_t s0(uint64_t x) { return ror(x, 1) ^ ror(x, 8) ^ (x >> 7); }
static uint64_t s1(uint64_t x) { return ror(x, 19) ^ ror(x, 61) ^ (x >> 6); }
static uint64_t ch(uint64_t x, uint64_t y, uint64_t z) { return (x & y) ^ (~x & z); }
static uint64_t maj(uint64_t x, uint64_t y, uint64_t z) { return (x & y) ^ (x & z) ^ (y & z); }
static v128 m_h(v128 d, v128 n, v128 m) {
    uint64_t msigma1 = S1(m.hi);
    uint64_t vtmp_hi = d.hi + msigma1 + ch(m.hi, n.lo, n.hi);
    uint64_t tsigma1 = S1(vtmp_hi + m.lo);
    uint64_t vtmp_lo = d.lo + tsigma1 + ch(vtmp_hi + m.lo, m.hi, n.lo);
    return (v128) {vtmp_lo, vtmp_hi};
}
static v128 m_h2(v128 d, v128 n, v128 m) {
    uint64_t hi = d.hi + S0(m.lo) + maj(m.lo, m.hi, n.lo);
    uint64_t lo = d.lo + S0(hi) + maj(hi, m.lo, m.hi);
    return (v128) {lo, hi};
}
static v128 m_su0(v128 d, v128 n) {
    return (v128) {d.lo + s0(d.hi), d.hi + s0(n.lo)};
}
static v128 m_su1(v128 d, v128 n, v128 m) {
    return (v128) {d.lo + s1(n.lo) + m.lo, d.hi + s1(n.hi) + m.hi};
}
// CRC32: the reflected polynomial, bit by bit (the Arm ARM's BitReverse
// form, written the reflected way).
static uint32_t m_crc(uint32_t acc, uint64_t val, int bytes, uint32_t poly) {
    for (int i = 0; i < bytes * 8; i++) {
        uint32_t bit = (acc ^ (uint32_t) (val >> i)) & 1;
        acc = (acc >> 1) ^ (bit ? poly : 0);
    }
    return acc;
}

#define V3(insn) \
    __asm__ volatile(EXT "ldr q0, [%1]\n ldr q1, [%2]\n ldr q2, [%3]\n" insn "\n str q0, [%0]" \
                     :: "r"(&got), "r"(&d), "r"(&n), "r"(&m) : "v0", "v1", "v2", "memory")
#define CRC(insn, w, acc, val) \
    __asm__ volatile(EXT "mov x1, %1\n mov x2, %2\n mov x0, #0x5555\n" insn "\n mov %0, x0" \
                     : "=r"(w) : "r"((uint64_t) (acc)), "r"((uint64_t) (val)) : "x0", "x1", "x2")

int main(void) {
    for (int i = 0; i < 3000; i++) {
        v128 d = {rnd(), rnd()}, n = {rnd(), rnd()}, m = {rnd(), rnd()}, got;
        V3("sha512h q0, q1, v2.2d"); check("sha512h", i, got, m_h(d, n, m));
        V3("sha512h2 q0, q1, v2.2d"); check("sha512h2", i, got, m_h2(d, n, m));
        V3("sha512su0 v0.2d, v1.2d"); check("sha512su0", i, got, m_su0(d, n));
        V3("sha512su1 v0.2d, v1.2d, v2.2d"); check("sha512su1", i, got, m_su1(d, n, m));
        // Vd aliasing an operand: everything is read before Vd is written
        V3("sha512h q0, q0, v2.2d"); check("sha512h d=n", i, got, m_h(d, d, m));
        V3("sha512h q0, q1, v0.2d"); check("sha512h d=m", i, got, m_h(d, n, d));
        V3("sha512h2 q0, q0, v0.2d"); check("sha512h2 d=n=m", i, got, m_h2(d, d, d));
        V3("sha512h2 q0, q1, v0.2d"); check("sha512h2 d=m", i, got, m_h2(d, n, d));
        V3("sha512su0 v0.2d, v0.2d"); check("sha512su0 d=n", i, got, m_su0(d, d));
        V3("sha512su1 v0.2d, v0.2d, v2.2d"); check("sha512su1 d=n", i, got, m_su1(d, d, m));
        V3("sha512su1 v0.2d, v1.2d, v0.2d"); check("sha512su1 d=m", i, got, m_su1(d, n, d));

        uint32_t acc = (uint32_t) rnd();
        uint64_t val = rnd();
        uint64_t w;
        const uint32_t P = 0xedb88320u, C = 0x82f63b78u;
#define CK(name, insn, a, v, bytes, poly) \
        CRC(insn, w, acc, val); \
        check(name, i, (v128) {w, 0}, (v128) {m_crc(a, v, bytes, poly), 0})
        CK("crc32b", "crc32b w0, w1, w2", acc, val, 1, P);
        CK("crc32h", "crc32h w0, w1, w2", acc, val, 2, P);
        CK("crc32w", "crc32w w0, w1, w2", acc, val, 4, P);
        CK("crc32x", "crc32x w0, w1, x2", acc, val, 8, P);
        CK("crc32cb", "crc32cb w0, w1, w2", acc, val, 1, C);
        CK("crc32ch", "crc32ch w0, w1, w2", acc, val, 2, C);
        CK("crc32cw", "crc32cw w0, w1, w2", acc, val, 4, C);
        CK("crc32cx", "crc32cx w0, w1, x2", acc, val, 8, C);
        CK("crc32x wzr acc", "crc32x w0, wzr, x2", 0, val, 8, P);
        CK("crc32cw wzr val", "crc32cw w0, w1, wzr", acc, 0, 4, C);
        CK("crc32b d=n", "crc32b w1, w1, w2\n mov x0, x1", acc, val, 1, P);
        CK("crc32cx d=m", "crc32cx w2, w1, x2\n mov x0, x2", acc, val, 8, C);
    }
    printf("arm64_sha512_crc32: %s (%lu checks, %lu mismatches)\n", bad ? "FAIL" : "PASS", checks, bad);
    return bad != 0;
}
