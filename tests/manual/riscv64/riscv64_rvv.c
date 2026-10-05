// riscv64_rvv.c -- AOK's V extension (jit/riscv64_vector.c) against scalar
// code. Every kernel is compiled twice from one body: once for rv64gcv at
// -O3, where gcc vectorizes it (strip-mined vsetvl, unit-stride, strided,
// indexed and segment accesses, masks, reductions, widening and narrowing,
// conversions), and once for plain rv64gc as the reference. Both run on the
// same random data at lengths 0..99 and unaligned starts; outputs must be
// bit-identical. One line per mismatch, PASS/FAIL at the end, exit 1.
//
//     gcc -O2 -o riscv64_rvv riscv64_rvv.c -lm   (an rv64gc toolchain with V support)

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

// fp-contract=off on both: a fused multiply-add in one and not the other
// would differ in the last bit by design
#define VEC __attribute__((target("arch=rv64gcv_zvbb"), optimize("O3", "fp-contract=off"), noinline))
#define REF __attribute__((target("arch=rv64gc"), optimize("O2", "no-tree-vectorize", "fp-contract=off"), noinline))

static unsigned long checks, bad;
static void report(const char *what, int n, int i, uint64_t got, uint64_t want) {
    checks++;
    if (got != want && bad++ < 40)
        printf("%s (n=%d) [%d] = %#llx, want %#llx\n", what, n, i, (unsigned long long) got,
               (unsigned long long) want);
}

static uint64_t rng = 0x853c49e6748fea9bull;
static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

#define N 128
#define PAD 8
// inputs (filled per round) and two output areas
static int8_t i8a[N + PAD], i8b[N + PAD];
static int16_t i16a[N + PAD], i16b[N + PAD];
static int32_t i32a[N + PAD], i32b[N + PAD], idx[N + PAD];
static int64_t i64a[N + PAD], i64b[N + PAD];
static float f32a[N + PAD], f32b[N + PAD];
static double f64a[N + PAD], f64b[N + PAD];

// ---- kernels: each a macro body instantiated as name_v (vector) and name_r ----
#define KERNEL(name, params, body) VEC static void name##_v params body REF static void name##_r params body

#define BIN(name, T, expr) \
    KERNEL(name, (T *restrict d, const T *restrict a, const T *restrict b, int n), \
           { for (int i = 0; i < n; i++) d[i] = (expr); })

BIN(add8, int8_t, (int8_t) (a[i] + b[i]))
BIN(mul16, int16_t, (int16_t) (a[i] * b[i]))
BIN(addmul32, int32_t, a[i] * 3 + b[i])
BIN(logic64, int64_t, (a[i] & b[i]) ^ (a[i] | 0x5555))
BIN(shift32, int32_t, (a[i] << (b[i] & 31)) ^ (int32_t) ((uint32_t) a[i] >> 3) ^ (a[i] >> 5))
BIN(minmax32, int32_t, (a[i] < b[i] ? a[i] : b[i]) + (a[i] > 7 ? a[i] : 7))
BIN(umin8, int8_t, (int8_t) ((uint8_t) a[i] < (uint8_t) b[i] ? a[i] : b[i]))
BIN(sel64, int64_t, a[i] > b[i] ? a[i] - b[i] : b[i] * 2)
BIN(div32, int32_t, b[i] != 0 ? a[i] / b[i] + a[i] % b[i] : 0)
BIN(udiv64, int64_t, b[i] != 0 ? (int64_t) ((uint64_t) a[i] / (uint64_t) b[i]) : -1)
BIN(abs16, int16_t, (int16_t) (a[i] < 0 ? -a[i] : a[i]))
BIN(mulh32, int32_t, (int32_t) (((int64_t) a[i] * b[i]) >> 32))
BIN(satadd8, int8_t, (int8_t) (a[i] + b[i] > 127 ? 127 : a[i] + b[i] < -128 ? -128 : a[i] + b[i]))
BIN(avgu8, int8_t, (int8_t) (((uint8_t) a[i] + (uint8_t) b[i] + 1) >> 1))
BIN(fadd32, float, a[i] + b[i] * 2.0f)
BIN(fma64, double, a[i] * b[i] + 1.5)
BIN(fdiv32, float, b[i] != 0 ? a[i] / b[i] : 0.0f)
BIN(fmin64, double, fmin(a[i], b[i]) - fmax(a[i], -b[i]))
BIN(fsel32, float, a[i] > b[i] ? a[i] : -b[i])
BIN(fsqrt64, double, sqrt(fabs(a[i])))
BIN(fabs32, float, fabsf(a[i]) + copysignf(1.0f, b[i]))
BIN(rotl32, int32_t, (int32_t) (((uint32_t) a[i] << 7) | ((uint32_t) a[i] >> 25)))
BIN(andn64, int64_t, a[i] & ~b[i])
BIN(clz32, int32_t, a[i] ? __builtin_clz((uint32_t) a[i]) : 32)
BIN(ctz64, int64_t, a[i] ? __builtin_ctzll((uint64_t) a[i]) : 64)
BIN(popc32, int32_t, __builtin_popcount((uint32_t) a[i]))
BIN(bswap32, int32_t, (int32_t) __builtin_bswap32((uint32_t) a[i]))

// widening and narrowing, conversions
KERNEL(wmul16, (int32_t *restrict d, const int16_t *restrict a, const int16_t *restrict b, int n),
       { for (int i = 0; i < n; i++) d[i] = (int32_t) a[i] * b[i] + d[i]; })
KERNEL(zext8, (int32_t *restrict d, const int8_t *restrict a, int n),
       { for (int i = 0; i < n; i++) d[i] = (uint8_t) a[i] + (int32_t) a[i]; })
KERNEL(narrow32, (int16_t *restrict d, const int32_t *restrict a, int n),
       { for (int i = 0; i < n; i++) d[i] = (int16_t) (a[i] >> 3); })
KERNEL(clip32, (int16_t *restrict d, const int32_t *restrict a, int n),
       { for (int i = 0; i < n; i++) d[i] = (int16_t) (a[i] > 32767 ? 32767 : a[i] < -32768 ? -32768 : a[i]); })
KERNEL(i2f32, (float *restrict d, const int32_t *restrict a, int n),
       { for (int i = 0; i < n; i++) d[i] = (float) a[i] * 0.5f; })
KERNEL(f2i64, (int64_t *restrict d, const double *restrict a, int n),
       { for (int i = 0; i < n; i++) d[i] = (int64_t) (a[i] * 1000.0); })
KERNEL(f2f, (double *restrict d, const float *restrict a, int n),
       { for (int i = 0; i < n; i++) d[i] = (double) a[i] * 3.0; })
KERNEL(d2f, (float *restrict d, const double *restrict a, int n),
       { for (int i = 0; i < n; i++) d[i] = (float) a[i]; })
KERNEL(u2d, (double *restrict d, const int64_t *restrict a, int n),
       { for (int i = 0; i < n; i++) d[i] = (double) (uint64_t) a[i]; })

// memory shapes: strided, gathered, segments, reversed, compressed
KERNEL(stride2, (int32_t *restrict d, const int32_t *restrict a, int n),
       { for (int i = 0; i < n / 2; i++) d[i] = a[2 * i] + a[2 * i + 1]; })
KERNEL(gather, (int64_t *restrict d, const int64_t *restrict a, const int32_t *restrict ix, int n),
       { for (int i = 0; i < n; i++) d[i] = a[ix[i]]; })
KERNEL(scatter, (int64_t *restrict d, const int64_t *restrict a, const int32_t *restrict ix, int n),
       { for (int i = 0; i < n; i++) d[ix[i]] = a[i] + 1; })
struct xyz { float x, y, z; };
KERNEL(seg3, (float *restrict d, const struct xyz *restrict p, int n),
       { for (int i = 0; i < n; i++) d[i] = p[i].x * p[i].x + p[i].y * p[i].y + p[i].z; })
KERNEL(rev16, (int16_t *restrict d, const int16_t *restrict a, int n),
       { for (int i = 0; i < n; i++) d[i] = a[n - 1 - i]; })
KERNEL(compress, (int32_t *restrict d, const int32_t *restrict a, int n, int *out),
       { int k = 0; for (int i = 0; i < n; i++) if (a[i] & 1) d[k++] = a[i]; *out = k; })

// reductions and searches
VEC static int64_t sum16_v(const int16_t *a, int n) { int64_t s = 0; for (int i = 0; i < n; i++) s += a[i]; return s; }
REF static int64_t sum16_r(const int16_t *a, int n) { int64_t s = 0; for (int i = 0; i < n; i++) s += a[i]; return s; }
VEC static int32_t max32_v(const int32_t *a, int n) { int32_t m = INT32_MIN; for (int i = 0; i < n; i++) m = a[i] > m ? a[i] : m; return m; }
REF static int32_t max32_r(const int32_t *a, int n) { int32_t m = INT32_MIN; for (int i = 0; i < n; i++) m = a[i] > m ? a[i] : m; return m; }
VEC static double dot64_v(const double *a, const double *b, int n) { double s = 0; for (int i = 0; i < n; i++) s += a[i] * b[i]; return s; }
REF static double dot64_r(const double *a, const double *b, int n) { double s = 0; for (int i = 0; i < n; i++) s += a[i] * b[i]; return s; }
VEC static float fmax32_v(const float *a, int n) { float m = -INFINITY; for (int i = 0; i < n; i++) m = fmaxf(m, a[i]); return m; }
REF static float fmax32_r(const float *a, int n) { float m = -INFINITY; for (int i = 0; i < n; i++) m = fmaxf(m, a[i]); return m; }
VEC static int count8_v(const int8_t *a, int n) { int c = 0; for (int i = 0; i < n; i++) c += a[i] == 3; return c; }
REF static int count8_r(const int8_t *a, int n) { int c = 0; for (int i = 0; i < n; i++) c += a[i] == 3; return c; }
VEC static int find32_v(const int32_t *a, int n, int32_t x) { for (int i = 0; i < n; i++) if (a[i] == x) return i; return -1; }
REF static int find32_r(const int32_t *a, int n, int32_t x) { for (int i = 0; i < n; i++) if (a[i] == x) return i; return -1; }
VEC static uint64_t xor64_v(const int64_t *a, int n) { uint64_t s = 0; for (int i = 0; i < n; i++) s ^= (uint64_t) a[i] * 3; return s; }
REF static uint64_t xor64_r(const int64_t *a, int n) { uint64_t s = 0; for (int i = 0; i < n; i++) s ^= (uint64_t) a[i] * 3; return s; }

// vfrec7 / vfrsqrt7: the spec's worked examples, the special cases, and the
// 7-bit accuracy over random normal inputs.
static uint64_t est(uint64_t x, int rec, int sew) {
    uint64_t r;
    if (sew == 32 && rec)
        __asm__ volatile(".option push\n.option arch,+v\nvsetivli zero,1,e32,m1,ta,ma\nvmv.s.x v1,%1\nvfrec7.v v2,v1\nvmv.x.s %0,v2\n.option pop" : "=r"(r) : "r"(x));
    else if (sew == 32)
        __asm__ volatile(".option push\n.option arch,+v\nvsetivli zero,1,e32,m1,ta,ma\nvmv.s.x v1,%1\nvfrsqrt7.v v2,v1\nvmv.x.s %0,v2\n.option pop" : "=r"(r) : "r"(x));
    else if (rec)
        __asm__ volatile(".option push\n.option arch,+v\nvsetivli zero,1,e64,m1,ta,ma\nvmv.s.x v1,%1\nvfrec7.v v2,v1\nvmv.x.s %0,v2\n.option pop" : "=r"(r) : "r"(x));
    else
        __asm__ volatile(".option push\n.option arch,+v\nvsetivli zero,1,e64,m1,ta,ma\nvmv.s.x v1,%1\nvfrsqrt7.v v2,v1\nvmv.x.s %0,v2\n.option pop" : "=r"(r) : "r"(x));
    return sew == 32 ? (uint32_t) r : r;
}
static void check_estimates(void) {
    report("vfrsqrt7 spec example 1", 0, 0, est(0x00718abc, 0, 32), 0x5f080000);
    report("vfrsqrt7 spec example 2", 0, 0, est(0x7f765432, 0, 32), 0x1f820000);
    report("vfrec7 spec example 1", 0, 0, est(0x00718abc, 1, 32), 0x7e900000);
    report("vfrec7 spec example 2", 0, 0, est(0x7f765432, 1, 32), 0x00214000);
    report("vfrec7 +0", 0, 0, est(0, 1, 32), 0x7f800000);
    report("vfrec7 -inf", 0, 0, est(0xff800000, 1, 32), 0x80000000);
    report("vfrsqrt7 -1", 0, 0, est(0xbf800000, 0, 32), 0x7fc00000);
    report("vfrsqrt7 +inf", 0, 0, est(0x7ff0000000000000ull, 0, 64), 0);
    for (int i = 0; i < 2000; i++) {
        double x = ldexp(1.0 + (double) (rnd() % 1000000) / 1e6, (int) (rnd() % 200) - 100);
        uint64_t b; memcpy(&b, &x, 8);
        double r, q; uint64_t rb = est(b, 1, 64), qb = est(b, 0, 64);
        memcpy(&r, &rb, 8); memcpy(&q, &qb, 8);
        report("vfrec7.v e64 within 2^-7", i, 0, fabs(r * x - 1) < 1.0 / 128, 1);
        report("vfrsqrt7.v e64 within 2^-7", i, 0, fabs(q * sqrt(x) - 1) < 1.0 / 128, 1);
    }
}

// vle8ff.v running into an unmapped page trims vl instead of faulting, and
// a unit-stride load crossing into a mapped page reads both pages (the
// gadget's one-page fast path must hand both to the general path).
static void check_ff_crosspage(void) {
    long pg = sysconf(_SC_PAGESIZE);
    char *p = mmap(0, 2 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    munmap(p + pg, pg);
    char *s = p + pg - 5;
    memcpy(s, "abcd", 5);
    unsigned long vl;
    unsigned char out[16] = {0};
    __asm__ volatile(".option push\n.option arch,+v\nvsetivli zero, 16, e8, m1, ta, ma\nvle8ff.v v1, (%1)\n"
                     "csrr %0, vl\nvse8.v v1, (%2)\n.option pop" : "=r"(vl) : "r"(s), "r"(out) : "memory");
    report("vle8ff.v vl trimmed at an unmapped page", 0, 0, vl, 5);
    report("vle8ff.v data", 0, 0, (uint64_t) memcmp(out, "abcd", 5), 0);
    char *q = mmap(0, 2 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    for (int i = 0; i < 32; i++)
        q[pg - 8 + i] = (char) i;
    unsigned char o2[16];
    __asm__ volatile(".option push\n.option arch,+v\nvsetivli zero, 16, e8, m1, ta, ma\nvle8.v v2, (%0)\n"
                     "vse8.v v2, (%1)\n.option pop" : : "r"(q + pg - 8), "r"(o2) : "memory");
    for (int i = 0; i < 16; i++)
        report("vle8.v across a page", 0, i, o2[i], (uint64_t) i);
    munmap(p, pg);
    munmap(q, 2 * pg);
}

// string searches: gcc vectorizes these early-exit loops with vle8ff.v,
// vmseq/vmsne and vfirst.m, as Ubuntu's RVA23 libc does
VEC static long slen_v(const char *p) { long n = 0; while (p[n]) n++; return n; }
REF static long slen_r(const char *p) { long n = 0; while (p[n]) n++; return n; }
VEC static long schr_v(const char *p, long n, char c) { for (long i = 0; i < n; i++) if (p[i] == c) return i; return -1; }
REF static long schr_r(const char *p, long n, char c) { for (long i = 0; i < n; i++) if (p[i] == c) return i; return -1; }
VEC static long sdiff_v(const char *a, const char *b, long n) { for (long i = 0; i < n; i++) if (a[i] != b[i]) return i; return -1; }
REF static long sdiff_r(const char *a, const char *b, long n) { for (long i = 0; i < n; i++) if (a[i] != b[i]) return i; return -1; }
static void check_strings(void) {
    static char buf[512], buf2[512];
    for (int round = 0; round < 3000; round++) {
        int len = (int) (rnd() % 300), off = (int) (rnd() % 64);
        for (int i = 0; i < 512; i++)
            buf[i] = (char) (1 + rnd() % 255);
        buf[off + len] = 0;
        memcpy(buf2, buf, sizeof(buf));
        if (len > 0 && (rnd() & 1))
            buf2[off + (int) (rnd() % (unsigned) len)] ^= 0x20;
        char c = buf[off + (len > 0 ? (int) (rnd() % (unsigned) len) : 0)];
        report("strlen-like", len, 0, (uint64_t) slen_v(buf + off), (uint64_t) slen_r(buf + off));
        report("memchr-like", len, 0, (uint64_t) schr_v(buf + off, len, c), (uint64_t) schr_r(buf + off, len, c));
        report("memcmp-like", len, 0, (uint64_t) sdiff_v(buf + off, buf2 + off, len), (uint64_t) sdiff_r(buf + off, buf2 + off, len));
    }
}

static void fill(void) {
    for (int i = 0; i < N + PAD; i++) {
        uint64_t r = rnd();
        i8a[i] = (int8_t) r; i8b[i] = (int8_t) (r >> 8);
        i16a[i] = (int16_t) (r >> 16); i16b[i] = (int16_t) (r >> 32);
        i32a[i] = (int32_t) rnd(); i32b[i] = (r & 3) == 0 ? 0 : (int32_t) (r >> 20);
        i64a[i] = (int64_t) rnd(); i64b[i] = (r & 7) == 0 ? 0 : (int64_t) rnd();
        idx[i] = (int32_t) (rnd() % N);
        double d = (double) (int64_t) (rnd() % 2000001 - 1000000) / 64.0;
        f32a[i] = (float) d; f32b[i] = (float) ((r & 15) == 0 ? 0.0 : d * 0.75 + 3.0);
        f64a[i] = d * 1.1; f64b[i] = (r & 15) == 1 ? -0.0 : d / 3.0;
        if ((r & 63) == 5) { f32a[i] = NAN; f64a[i] = INFINITY; }
        if ((r & 31) == 9) i8a[i] = 3;
    }
}

#define CMP(name, T, dv, dr, n) do { \
    for (int i_ = 0; i_ < (n); i_++) { \
        uint64_t g_ = 0, w_ = 0; memcpy(&g_, &(dv)[i_], sizeof(T)); memcpy(&w_, &(dr)[i_], sizeof(T)); \
        report(name, (n), i_, g_, w_); \
    } } while (0)

#define RUN_BIN(name, T, A, B) do { \
    T dv[N + PAD], dr[N + PAD]; memset(dv, 0, sizeof(dv)); memset(dr, 0, sizeof(dr)); \
    name##_v(dv + off, A + off, B + off, n); name##_r(dr + off, A + off, B + off, n); \
    CMP(#name, T, dv, dr, N + PAD); } while (0)
#define RUN_UN(name, DT, ST, A) do { \
    DT dv[N + PAD], dr[N + PAD]; memset(dv, 0x5a, sizeof(dv)); memset(dr, 0x5a, sizeof(dr)); \
    name##_v(dv + off, (const ST *) A + off, n); name##_r(dr + off, (const ST *) A + off, n); \
    CMP(#name, DT, dv, dr, N + PAD); } while (0)

int main(void) {
    check_estimates();
    check_ff_crosspage();
    check_strings();
    for (int round = 0; round < 300; round++) {
        fill();
        int n = (int) (rnd() % 100), off = (int) (rnd() % 4);
        RUN_BIN(add8, int8_t, i8a, i8b); RUN_BIN(umin8, int8_t, i8a, i8b);
        RUN_BIN(satadd8, int8_t, i8a, i8b); RUN_BIN(avgu8, int8_t, i8a, i8b);
        RUN_BIN(mul16, int16_t, i16a, i16b); RUN_BIN(abs16, int16_t, i16a, i16b);
        RUN_BIN(addmul32, int32_t, i32a, i32b); RUN_BIN(shift32, int32_t, i32a, i32b);
        RUN_BIN(minmax32, int32_t, i32a, i32b); RUN_BIN(div32, int32_t, i32a, i32b);
        RUN_BIN(mulh32, int32_t, i32a, i32b); RUN_BIN(rotl32, int32_t, i32a, i32b);
        RUN_BIN(clz32, int32_t, i32a, i32b); RUN_BIN(popc32, int32_t, i32a, i32b);
        RUN_BIN(bswap32, int32_t, i32a, i32b);
        RUN_BIN(logic64, int64_t, i64a, i64b); RUN_BIN(sel64, int64_t, i64a, i64b);
        RUN_BIN(udiv64, int64_t, i64a, i64b); RUN_BIN(andn64, int64_t, i64a, i64b);
        RUN_BIN(ctz64, int64_t, i64a, i64b);
        RUN_BIN(fadd32, float, f32a, f32b); RUN_BIN(fdiv32, float, f32a, f32b);
        RUN_BIN(fsel32, float, f32a, f32b); RUN_BIN(fabs32, float, f32a, f32b);
        RUN_BIN(fma64, double, f64a, f64b); RUN_BIN(fmin64, double, f64a, f64b);
        RUN_BIN(fsqrt64, double, f64a, f64b);
        {
            int32_t dv[N + PAD], dr[N + PAD];
            for (int i = 0; i < N + PAD; i++) dv[i] = dr[i] = i32a[i];
            wmul16_v(dv + off, i16a + off, i16b + off, n); wmul16_r(dr + off, i16a + off, i16b + off, n);
            CMP("wmul16", int32_t, dv, dr, N + PAD);
        }
        RUN_UN(zext8, int32_t, int8_t, i8a); RUN_UN(narrow32, int16_t, int32_t, i32a);
        RUN_UN(clip32, int16_t, int32_t, i32a); RUN_UN(i2f32, float, int32_t, i32a);
        RUN_UN(f2i64, int64_t, double, f64b); RUN_UN(f2f, double, float, f32a);
        RUN_UN(d2f, float, double, f64a); RUN_UN(u2d, double, int64_t, i64a);
        RUN_UN(stride2, int32_t, int32_t, i32a); RUN_UN(rev16, int16_t, int16_t, i16a);
        {
            int64_t dv[N + PAD], dr[N + PAD];
            memset(dv, 0, sizeof(dv)); memset(dr, 0, sizeof(dr));
            gather_v(dv, i64a, idx + off, n); gather_r(dr, i64a, idx + off, n);
            CMP("gather", int64_t, dv, dr, N + PAD);
            memset(dv, 0, sizeof(dv)); memset(dr, 0, sizeof(dr));
            scatter_v(dv, i64a, idx + off, n); scatter_r(dr, i64a, idx + off, n);
            CMP("scatter", int64_t, dv, dr, N + PAD);
        }
        {
            struct xyz p[N]; float dv[N], dr[N];
            for (int i = 0; i < N; i++) { p[i].x = f32a[i]; p[i].y = f32b[i]; p[i].z = (float) i; }
            memset(dv, 0, sizeof(dv)); memset(dr, 0, sizeof(dr));
            seg3_v(dv, p, n); seg3_r(dr, p, n);
            CMP("seg3", float, dv, dr, N);
        }
        {
            int32_t dv[N + PAD], dr[N + PAD]; int kv, kr;
            memset(dv, 0, sizeof(dv)); memset(dr, 0, sizeof(dr));
            compress_v(dv, i32a + off, n, &kv); compress_r(dr, i32a + off, n, &kr);
            report("compress count", n, 0, (uint64_t) kv, (uint64_t) kr);
            CMP("compress", int32_t, dv, dr, N + PAD);
        }
        report("sum16", n, 0, (uint64_t) sum16_v(i16a + off, n), (uint64_t) sum16_r(i16a + off, n));
        report("max32", n, 0, (uint64_t) max32_v(i32a + off, n), (uint64_t) max32_r(i32a + off, n));
        { double v = dot64_v(f64a + off, f64b + off, n), r = dot64_r(f64a + off, f64b + off, n);
          uint64_t a_, b_; memcpy(&a_, &v, 8); memcpy(&b_, &r, 8); report("dot64", n, 0, a_, b_); }
        { float v = fmax32_v(f32a + off, n), r = fmax32_r(f32a + off, n);
          uint32_t a_, b_; memcpy(&a_, &v, 4); memcpy(&b_, &r, 4); report("fmax32", n, 0, a_, b_); }
        report("count8", n, 0, (uint64_t) count8_v(i8a + off, n), (uint64_t) count8_r(i8a + off, n));
        { int32_t x = n > 0 ? i32a[off + (int) (rnd() % (unsigned) n)] : 0;
          report("find32", n, 0, (uint64_t) find32_v(i32a + off, n, x), (uint64_t) find32_r(i32a + off, n, x)); }
        report("xor64", n, 0, xor64_v(i64a + off, n), xor64_r(i64a + off, n));
    }
    printf("riscv64_rvv: %s (%lu checks, %lu mismatches)\n", bad ? "FAIL" : "PASS", checks, bad);
    return bad != 0;
}
