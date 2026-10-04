// sqrtsd / sqrtss (F2/F3 0F 51), register and memory sources, on special and
// random bit patterns, with the destination's upper lanes set to a marker so
// a write that strays past the low lane shows. Prints every result in hex.
// Also the MXCSR exception flags each one raised (sqrt(-x) must raise Invalid).
// The oracle is the same binary under the amd64 interpreter:
//   ./amd64_sqrt_scalar > jit.txt
//   echo 0 > /proc/ish/amd64_jit; ./amd64_sqrt_scalar > interp.txt; cmp ...
// Built for x86_64 (gcc -O1); the JIT translates these since 2026-10-04,
// before which every one ran on the interpreter.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// MXCSR's six exception flags, cleared before each instruction and read after.
static unsigned mx_take(void) {
    unsigned m;
    __asm__ volatile("stmxcsr %0" : "=m"(m));
    unsigned flags = m & 0x3f;
    m &= ~0x3fu;
    __asm__ volatile("ldmxcsr %0" : : "m"(m));
    return flags;
}

static uint64_t rng = 0x243f6a8885a308d3ull;
static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

typedef struct { uint64_t lo, hi; } xmm_t;

int main(void) {
    static const uint64_t d_special[] = {
        0, 0x8000000000000000ull, 0x3ff0000000000000ull, 0xbff0000000000000ull,
        0x7ff0000000000000ull, 0xfff0000000000000ull, 0x7ff8000000000000ull,
        0x7ff4000000000001ull, 0xfff8000000001234ull, 0x0000000000000001ull,
        0x000fffffffffffffull, 0x7fefffffffffffffull, 0x4010000000000000ull,
    };
    static const uint32_t s_special[] = {
        0, 0x80000000u, 0x3f800000u, 0xbf800000u, 0x7f800000u, 0xff800000u,
        0x7fc00000u, 0x7fa00001u, 0xffc01234u, 1, 0x007fffffu, 0x7f7fffffu, 0x40800000u,
    };
    int n = 0;
    for (int i = 0; i < 400; i++) {
        uint64_t dv = i < 13 ? d_special[i] : rnd();
        uint32_t sv = i < 13 ? s_special[i] : (uint32_t) rnd();
        unsigned fx[4];
        mx_take();
        xmm_t src = {dv, rnd()}, dst, dmem;
        // reg form, double
        dst.lo = 0x1111111111111111ull; dst.hi = 0x2222222222222222ull;
        __asm__ volatile("movdqu %1, %%xmm1\n movdqu %0, %%xmm2\n sqrtsd %%xmm1, %%xmm2\n movdqu %%xmm2, %0"
                         : "+m"(dst) : "m"(src) : "xmm1", "xmm2");
        fx[0] = mx_take();
        // mem form, double
        dmem.lo = 0x3333333333333333ull; dmem.hi = 0x4444444444444444ull;
        __asm__ volatile("movdqu %0, %%xmm3\n sqrtsd %1, %%xmm3\n movdqu %%xmm3, %0"
                         : "+m"(dmem) : "m"(dv) : "xmm3");
        fx[1] = mx_take();
        // reg and mem forms, single
        xmm_t ssrc = {sv | (rnd() << 32), rnd()}, sdst, smem;
        sdst.lo = 0x5555555555555555ull; sdst.hi = 0x6666666666666666ull;
        __asm__ volatile("movdqu %1, %%xmm4\n movdqu %0, %%xmm5\n sqrtss %%xmm4, %%xmm5\n movdqu %%xmm5, %0"
                         : "+m"(sdst) : "m"(ssrc) : "xmm4", "xmm5");
        fx[2] = mx_take();
        smem.lo = 0x7777777777777777ull; smem.hi = 0x8888888888888888ull;
        __asm__ volatile("movdqu %0, %%xmm6\n sqrtss %1, %%xmm6\n movdqu %%xmm6, %0"
                         : "+m"(smem) : "m"(sv) : "xmm6");
        fx[3] = mx_take();
        printf("%016llx -> %016llx:%016llx m %016llx:%016llx | %08x -> %016llx:%016llx m %016llx:%016llx\n",
               (unsigned long long) dv, (unsigned long long) dst.hi, (unsigned long long) dst.lo,
               (unsigned long long) dmem.hi, (unsigned long long) dmem.lo, sv,
               (unsigned long long) sdst.hi, (unsigned long long) sdst.lo,
               (unsigned long long) smem.hi, (unsigned long long) smem.lo);
        printf("  mxcsr %02x %02x %02x %02x\n", fx[0], fx[1], fx[2], fx[3]);
        n++;
    }
    fprintf(stderr, "%d cases\n", n);
    return 0;
}
