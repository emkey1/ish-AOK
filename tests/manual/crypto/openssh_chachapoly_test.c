// Guest test for the bundled native-mode sshd's accelerated cipher:
// OpenSSH's chacha20-poly1305@openssh.com through AOK's crypto accelerator
// (cipher-chachapoly.c built with AOK_GUEST_ACCEL) against the same file built
// without it, which is upstream OpenSSH's own ChaCha20 and Poly1305.
//
//  1. Differential: random keys, sequence numbers and lengths (0 to 70000,
//     every 64-byte block edge near the start), sealed by both, must give the
//     same bytes -- encrypted length, payload and tag. Each side must open
//     the other's packet, and refuse it with one bit flipped anywhere.
//     get_length must agree, and so must sealing in place.
//  2. Throughput: 32 KiB packets (scp's size) sealed and opened.
//
// Run it with the accelerator on (ISH_CRYPTO_ACCEL=1 for the CLI) and off:
// both must pass part 1. Part 2 is the evidence the accelerator ran, along
// with strace -c, which counts syscall 0xacc0.
//
// Build (needs a configured OpenSSH tree; tools/build-guest-sshd.sh makes one
// in its .build directory and removes it, so configure a copy the same way):
//   T=<configured tree>; CC="zig cc -target aarch64-linux-musl -O2 -I$T -I$T/openbsd-compat"
//   R="-Dchachapoly_new=acc_new -Dchachapoly_free=acc_free \
//      -Dchachapoly_crypt=acc_crypt -Dchachapoly_get_length=acc_get_length"
//   $CC -DAOK_GUEST_ACCEL $R -c $T/cipher-chachapoly.c -o acc.o
//   $CC -c $T/cipher-chachapoly.c -o sw.o
//   $CC -static -o chachapoly-test openssh_chachapoly_test.c acc.o sw.o \
//       $T/chacha.c $T/poly1305.c $T/openbsd-compat/explicit_bzero.c \
//       $T/openbsd-compat/timingsafe_bcmp.c $T/openbsd-compat/freezero.c
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef unsigned char u_char;
typedef unsigned int u_int;
struct chachapoly_ctx;

#define DECLARE(p) \
    struct chachapoly_ctx *p##_new(const u_char *key, u_int keylen); \
    void p##_free(struct chachapoly_ctx *ctx); \
    int p##_crypt(struct chachapoly_ctx *ctx, u_int seqnr, u_char *dest, \
            const u_char *src, u_int len, u_int aadlen, u_int authlen, int do_encrypt); \
    int p##_get_length(struct chachapoly_ctx *ctx, u_int *plenp, u_int seqnr, \
            const u_char *cp, u_int len);
DECLARE(acc)
DECLARE(chachapoly)

#define TAGLEN 16
#define AADLEN 4
#define MAXLEN 70000
#define SSH_ERR_MAC_INVALID (-30)

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint64_t next(void) {
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return rng;
}
static void fill(u_char *p, size_t n) {
    for (size_t i = 0; i < n; i++) p[i] = (u_char) next();
}

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { \
    if (failures++ < 20) { printf("FAIL: " __VA_ARGS__); printf("\n"); } } } while (0)

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void one(u_int len, u_int seqnr, u_char *src, u_char *a, u_char *b, u_char *back) {
    u_char key[64];
    fill(key, sizeof key);
    struct chachapoly_ctx *acc = acc_new(key, sizeof key);
    struct chachapoly_ctx *sw = chachapoly_new(key, sizeof key);
    size_t total = AADLEN + len + TAGLEN;
    fill(src, AADLEN + len);
    src[0] = (u_char) (len >> 24); src[1] = (u_char) (len >> 16);
    src[2] = (u_char) (len >> 8); src[3] = (u_char) len;

    int ra = acc_crypt(acc, seqnr, a, src, len, AADLEN, TAGLEN, 1);
    int rb = chachapoly_crypt(sw, seqnr, b, src, len, AADLEN, TAGLEN, 1);
    CHECK(ra == 0 && rb == 0, "seal len %u: %d %d", len, ra, rb);
    CHECK(memcmp(a, b, total) == 0, "seal len %u seq %u: bytes differ", len, seqnr);

    u_int pa = 0, pb = 0;
    ra = acc_get_length(acc, &pa, seqnr, a, total);
    rb = chachapoly_get_length(sw, &pb, seqnr, b, total);
    CHECK(ra == 0 && rb == 0 && pa == len && pb == len,
            "get_length len %u: %d %d -> %u %u", len, ra, rb, pa, pb);

    // Each opens the other's packet.
    memset(back, 0, total);
    ra = acc_crypt(acc, seqnr, back, b, len, AADLEN, TAGLEN, 0);
    CHECK(ra == 0 && memcmp(back, src, AADLEN + len) == 0, "acc open len %u: %d", len, ra);
    memset(back, 0, total);
    rb = chachapoly_crypt(sw, seqnr, back, a, len, AADLEN, TAGLEN, 0);
    CHECK(rb == 0 && memcmp(back, src, AADLEN + len) == 0, "sw open len %u: %d", len, rb);

    // One flipped bit anywhere -- length, payload or tag -- is refused by both.
    size_t at = next() % total;
    a[at] ^= (u_char) (1u << (next() % 8));
    ra = acc_crypt(acc, seqnr, back, a, len, AADLEN, TAGLEN, 0);
    rb = chachapoly_crypt(sw, seqnr, back, a, len, AADLEN, TAGLEN, 0);
    CHECK(ra == SSH_ERR_MAC_INVALID && rb == SSH_ERR_MAC_INVALID,
            "tampered len %u at %zu: %d %d", len, at, ra, rb);
    // The wrong sequence number too, on a freshly sealed packet.
    acc_crypt(acc, seqnr, a, src, len, AADLEN, TAGLEN, 1);
    ra = acc_crypt(acc, seqnr + 1, back, a, len, AADLEN, TAGLEN, 0);
    CHECK(ra == SSH_ERR_MAC_INVALID, "wrong seqnr len %u: %d", len, ra);

    // Sealing in place gives the same bytes.
    memcpy(a, src, AADLEN + len);
    ra = acc_crypt(acc, seqnr, a, a, len, AADLEN, TAGLEN, 1);
    CHECK(ra == 0 && memcmp(a, b, total) == 0, "in-place seal len %u: %d", len, ra);

    acc_free(acc);
    chachapoly_free(sw);
}

static double bench(int accel, u_char *src, u_char *a, u_char *back) {
    u_char key[64];
    fill(key, sizeof key);
    struct chachapoly_ctx *c = accel ? acc_new(key, sizeof key) : chachapoly_new(key, sizeof key);
    const u_int len = 32768;
    const int n = 256; // 8 MiB each way
    fill(src, AADLEN + len);
    double t0 = now();
    for (int i = 0; i < n; i++) {
        int r = accel ? acc_crypt(c, i, a, src, len, AADLEN, TAGLEN, 1)
                      : chachapoly_crypt(c, i, a, src, len, AADLEN, TAGLEN, 1);
        int s = accel ? acc_crypt(c, i, back, a, len, AADLEN, TAGLEN, 0)
                      : chachapoly_crypt(c, i, back, a, len, AADLEN, TAGLEN, 0);
        if (r != 0 || s != 0) { printf("FAIL: bench %d: %d %d\n", i, r, s); failures++; break; }
    }
    double dt = now() - t0;
    accel ? acc_free(c) : chachapoly_free(c);
    return 2.0 * n * len / dt / 1e6;
}

int main(int argc, char **argv) {
    u_char *src = malloc(AADLEN + MAXLEN + TAGLEN);
    u_char *a = malloc(AADLEN + MAXLEN + TAGLEN);
    u_char *b = malloc(AADLEN + MAXLEN + TAGLEN);
    u_char *back = malloc(AADLEN + MAXLEN + TAGLEN);
    int cases = 0;

    for (u_int len = 0; len <= 260; len++, cases++)
        one(len, (u_int) next(), src, a, b, back);
    for (int i = 0; i < 400; i++, cases++)
        one((u_int) (next() % (MAXLEN + 1)), (u_int) next(), src, a, b, back);
    one(MAXLEN, 0xffffffffu, src, a, b, back); cases++;
    printf("differential: %d cases, %d failures\n", cases, failures);

    if (argc < 2 || strcmp(argv[1], "--no-bench") != 0) {
        double sw = bench(0, src, a, back);
        double acc = bench(1, src, a, back);
        printf("32 KiB packets, seal+open: OpenSSH's own %.1f MB/s, through accelerator %.1f MB/s (%.2fx)\n",
                sw, acc, acc / sw);
    }
    printf("%s\n", failures ? "FAILED" : "PASSED");
    return failures != 0;
}
