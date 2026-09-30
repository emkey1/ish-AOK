// FSQRT against real x87 hardware: every rounding mode x precision control
// (24/53/64 bits), a deterministic spread of operands (powers of two, exact
// squares and their neighbours, random significands, denormals, huge and tiny
// exponents), hashing each 80-bit result with the status word's PE and C1.
// The expected hash is what an AMD x86-64 machine (camd) produced for both
// the -m32 and the 64-bit build; run with -v to print per-case values when it
// differs. Written when FSQRT's integer root moved from a digit-by-digit loop
// to a double-precision estimate plus exact correction (emu/float80.c).
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define EXPECTED 0x7a21e22d54da4ee3ull  // camd, -m32 and 64-bit alike

static uint64_t rng = 0x243f6a8885a308d3ull;
static uint64_t next(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

struct f80 { uint64_t signif; uint16_t se; } __attribute__((packed));

static uint64_t hash = 0xcbf29ce484222325ull;
static void mix(const void *p, int n) {
    const unsigned char *b = p;
    for (int i = 0; i < n; i++) { hash ^= b[i]; hash *= 0x100000001b3ull; }
}

int main(int argc, char **argv) {
    int verbose = argc > 1;
    long cases = 0;
    for (int rc = 0; rc < 4; rc++) {
        for (int pc = 0; pc < 3; pc++) {
            static const int pcbits[] = { 0, 2, 3 };  // 24, 53, 64 bits
            uint16_t cw = 0x037f & ~0x0f00;
            cw |= pcbits[pc] << 8 | rc << 10;
            rng = 0x243f6a8885a308d3ull;
            for (int i = 0; i < 3000; i++) {
                struct f80 x;
                uint64_t r = next();
                switch (i % 6) {
                case 0: x.signif = 1ull << 63; break;
                case 1: { uint32_t k = (uint32_t) next() | 0x80000000u; uint64_t sq = (uint64_t) k * k;
                          x.signif = (uint64_t) sq + (int) (r % 3) - 1; if (!(x.signif >> 63)) x.signif |= 1ull << 63; break; }
                case 2: x.signif = ~0ull - (r & 0xff); break;
                default: x.signif = next() | (1ull << 63); break;
                }
                int e;
                switch ((r >> 8) % 5) {
                case 0: e = 0x3fff + (int) ((r >> 16) % 16) - 8; break;
                case 1: e = 1 + (int) ((r >> 16) % 64); break;
                case 2: e = 0x7ffe - (int) ((r >> 16) % 64); break;
                case 3: e = 0; x.signif >>= 1 + (r >> 20) % 62; break;  // denormal
                default: e = 1 + (int) ((r >> 16) % 0x7ffe); break;
                }
                x.se = (uint16_t) e;
                struct f80 out;
                uint16_t sw;
                __asm__ volatile(
                    "fninit\n fldcw %[cw]\n fldt %[x]\n fsqrt\n fstpt %[out]\n fnstsw %[sw]\n"
                    : [out] "=m" (out), [sw] "=m" (sw) : [x] "m" (x), [cw] "m" (cw) : "st");
                uint16_t flags = sw & (0x20 | 0x200);  // PE, C1
                mix(&out, 10);
                mix(&flags, 2);
                cases++;
                if (verbose)
                    printf("rc=%d pc=%d x=%04x:%016llx -> %04x:%016llx sw=%03x\n", rc, pc, x.se,
                            (unsigned long long) x.signif, out.se, (unsigned long long) out.signif, flags);
            }
        }
    }
    printf("%ld cases, hash %016llx\n", cases, (unsigned long long) hash);
    int ok = hash == EXPECTED;
    printf("x87_fsqrt: %s\n", ok ? "PASS" : "FAIL");
    return !ok;
}
