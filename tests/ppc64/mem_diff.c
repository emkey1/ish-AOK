// The C library's memory and string routines, and loops a compiler
// vectorises, over many sizes and alignments: a hash per case, for a diff
// between an oracle (qemu-ppc64le, a POWER machine) and the AOK guest.
// Built static and -O3 -mcpu=power8, so glibc's VSX/VMX routines and GCC's
// vector code are what runs.
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static uint64_t s = 0x243f6a8885a308d3ULL;
static uint64_t rnd(void) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
static uint64_t hash(const void *p, size_t n) {
    const unsigned char *b = p;
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ULL; }
    return h;
}
static unsigned char A[4096 + 64], B[4096 + 64];
static void fill(unsigned char *p, size_t n) {
    for (size_t i = 0; i < n; i++) p[i] = (unsigned char) rnd();
}

__attribute__((noinline)) static void vadd(float *d, const float *a, const float *b, int n) {
    for (int i = 0; i < n; i++) d[i] = a[i] * 1.5f + b[i];
}
__attribute__((noinline)) static void vaddd(double *d, const double *a, const double *b, int n) {
    for (int i = 0; i < n; i++) d[i] = a[i] * b[i] - 0.25;
}
__attribute__((noinline)) static void vint(uint32_t *d, const uint32_t *a, const uint32_t *b, int n) {
    for (int i = 0; i < n; i++) d[i] = (a[i] ^ (b[i] >> 3)) + (a[i] << 5) - b[i];
}
__attribute__((noinline)) static uint64_t vsum(const uint16_t *a, int n) {
    uint64_t t = 0;
    for (int i = 0; i < n; i++) t += a[i] * (uint32_t) a[i];
    return t;
}
__attribute__((noinline)) static void vbyte(uint8_t *d, const uint8_t *a, int n) {
    for (int i = 0; i < n; i++) d[i] = a[i] > 0x80 ? a[i] - 0x80 : 0x80 - a[i];
}

int main(void) {
    static const size_t sizes[] = {0, 1, 2, 3, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65,
                                   127, 128, 129, 255, 256, 511, 1000, 1024, 3000};
    for (unsigned si = 0; si < sizeof(sizes) / sizeof(sizes[0]); si++) {
        size_t n = sizes[si];
        for (unsigned da = 0; da < 16; da += 3) {
            for (unsigned sa = 0; sa < 16; sa += 5) {
                fill(A, sizeof(A)); fill(B, sizeof(B));
                memcpy(B + da, A + sa, n);
                printf("memcpy %zu %u %u %016llx\n", n, da, sa, (unsigned long long) hash(B, sizeof(B)));
                memmove(A + da, A + sa, n);
                printf("memmove %zu %u %u %016llx\n", n, da, sa, (unsigned long long) hash(A, sizeof(A)));
                memset(B + da, (int) sa * 17, n);
                printf("memset %zu %u %u %016llx\n", n, da, sa, (unsigned long long) hash(B, sizeof(B)));
                memcpy(B + da, A + sa, n);
                if (n) B[da + n / 2] ^= (unsigned char) (sa + 1);
                printf("memcmp %zu %u %u %d\n", n, da, sa, memcmp(B + da, A + sa, n) > 0 ? 1 : memcmp(B + da, A + sa, n) < 0 ? -1 : 0);
                for (size_t i = 0; i < n; i++) if (A[sa + i] == 0) A[sa + i] = 1;
                A[sa + n] = 0;
                printf("strlen %zu %u %u %zu\n", n, da, sa, strlen((char *) A + sa));
                printf("strchr %zu %u %u %ld\n", n, da, sa, (long) (strchr((char *) A + sa, A[sa + n / 2 + (n > 0)]) - (char *) A));
                strcpy((char *) B + da, (char *) A + sa);
                printf("strcpy %zu %u %u %016llx %d\n", n, da, sa, (unsigned long long) hash(B, sizeof(B)),
                       strcmp((char *) B + da, (char *) A + sa));
                printf("memchr %zu %u %u %ld\n", n, da, sa, (long) ((unsigned char *) memchr(A + sa, A[sa + n / 3], n + 1) - A));
                printf("memrchr %zu %u %u %ld\n", n, da, sa, (long) ((unsigned char *) memrchr(A + sa, A[sa], n + 1) - A));
            }
        }
    }
    static float fa[1031], fb[1031], fd[1031];
    static double da[1031], db[1031], dd[1031];
    static uint32_t ia[1031], ib[1031], id[1031];
    static uint16_t ha[1031];
    static uint8_t ba[1031], bd[1031];
    for (int i = 0; i < 1031; i++) {
        fa[i] = (float) (int32_t) rnd() / 65536.0f; fb[i] = (float) (int32_t) rnd() / 1024.0f;
        da[i] = (double) (int64_t) rnd() / 1e9; db[i] = (double) (int32_t) rnd() / 7.0;
        ia[i] = (uint32_t) rnd(); ib[i] = (uint32_t) rnd(); ha[i] = (uint16_t) rnd(); ba[i] = (uint8_t) rnd();
    }
    for (int n = 0; n < 1031; n += 37) {
        vadd(fd, fa, fb, n); vaddd(dd, da, db, n); vint(id, ia, ib, n); vbyte(bd, ba, n);
        printf("vec %d %016llx %016llx %016llx %016llx %llu\n", n,
               (unsigned long long) hash(fd, sizeof(fd)), (unsigned long long) hash(dd, sizeof(dd)),
               (unsigned long long) hash(id, sizeof(id)), (unsigned long long) hash(bd, sizeof(bd)),
               (unsigned long long) vsum(ha, n));
    }
    char buf[256];
    for (int i = 0; i < 40; i++) {
        double x = (double) (int64_t) rnd() / (double) (1ULL << (rnd() % 60));
        snprintf(buf, sizeof(buf), "%.17g %a %e %f", x, x, x * 1e-10, x / 3.0);
        printf("fmt %s %g %ld\n", buf, strtod(buf, NULL), strtol("-123456789", NULL, 10) + i);
    }
    return 0;
}
