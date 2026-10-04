// SSE MIN/MAX (minsd maxsd minss maxss minpd maxpd minps maxps), register and
// memory sources, over every pair of special values: +-0, +-1, quiet and
// signalling NaNs with payloads, +inf. x86's rule is `dst < src ? dst : src`
// (MIN) and `dst > src ? dst : src` (MAX): a NaN on either side, or +0 vs -0,
// gives the SOURCE unchanged. Prints every result; the oracle is the amd64
// interpreter (echo 0 > /proc/ish/amd64_jit). The JIT used arm64 fmin/fmax and
// differed on 29 of 64 scalar-double pairs until 2026-10-04.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct { uint64_t lo, hi; } xmm_t;
static const uint64_t dv[] = {0, 0x8000000000000000ull, 0x3ff0000000000000ull, 0xbff0000000000000ull,
    0x7ff8000000000000ull, 0x7ff4000000000001ull, 0xfff8000000000005ull, 0x7ff0000000000000ull};
static const uint32_t sv[] = {0, 0x80000000u, 0x3f800000u, 0xbf800000u, 0x7fc00000u, 0x7fa00001u,
    0xffc00005u, 0x7f800000u};

#define SCALAR(op, a, b, out) \
    __asm__ volatile("movdqu %1, %%xmm0\n movdqu %2, %%xmm1\n " op " %%xmm1, %%xmm0\n movdqu %%xmm0, %0" \
                     : "=m"(out) : "m"(a), "m"(b) : "xmm0", "xmm1")
#define SCALAR_MEM(op, a, m, out) \
    __asm__ volatile("movdqu %1, %%xmm0\n " op " %2, %%xmm0\n movdqu %%xmm0, %0" \
                     : "=m"(out) : "m"(a), "m"(m) : "xmm0")

int main(void) {
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++) {
            xmm_t a = {dv[i], 0x1111111111111111ull}, b = {dv[j], 0x2222222222222222ull}, r[4];
            SCALAR("minsd", a, b, r[0]); SCALAR("maxsd", a, b, r[1]);
            SCALAR_MEM("minsd", a, dv[j], r[2]); SCALAR_MEM("maxsd", a, dv[j], r[3]);
            xmm_t sa = {sv[i] | 0x3333333300000000ull, 0x4444444444444444ull}, sb = {sv[j], 0}, q[4];
            SCALAR("minss", sa, sb, q[0]); SCALAR("maxss", sa, sb, q[1]);
            SCALAR_MEM("minss", sa, sv[j], q[2]); SCALAR_MEM("maxss", sa, sv[j], q[3]);
            // packed: both lanes / all four lanes hold the pair, shifted
            xmm_t pa = {dv[i], dv[(i + 3) & 7]}, pb = {dv[j], dv[(j + 5) & 7]}, p[4];
            SCALAR("minpd", pa, pb, p[0]); SCALAR("maxpd", pa, pb, p[1]);
            SCALAR_MEM("minpd", pa, pb, p[2]); SCALAR_MEM("maxpd", pa, pb, p[3]);
            xmm_t fa = {sv[i] | (uint64_t) sv[(i + 1) & 7] << 32, sv[(i + 2) & 7] | (uint64_t) sv[(i + 3) & 7] << 32};
            xmm_t fb = {sv[j] | (uint64_t) sv[(j + 5) & 7] << 32, sv[(j + 6) & 7] | (uint64_t) sv[(j + 7) & 7] << 32};
            xmm_t f[4];
            SCALAR("minps", fa, fb, f[0]); SCALAR("maxps", fa, fb, f[1]);
            SCALAR_MEM("minps", fa, fb, f[2]); SCALAR_MEM("maxps", fa, fb, f[3]);
            printf("%d,%d", i, j);
            for (int k = 0; k < 4; k++)
                printf(" %016llx:%016llx %016llx:%016llx %016llx:%016llx %016llx:%016llx",
                       (unsigned long long) r[k].hi, (unsigned long long) r[k].lo,
                       (unsigned long long) q[k].hi, (unsigned long long) q[k].lo,
                       (unsigned long long) p[k].hi, (unsigned long long) p[k].lo,
                       (unsigned long long) f[k].hi, (unsigned long long) f[k].lo);
            printf("\n");
        }
    return 0;
}
