// AES-256-GCM through the iSH-AOK crypto accelerator syscall (ISH_SYS_AEAD,
// alg 2 -- kernel/ish_accel.c + kernel/ish_accel_aes.c).
//
// The accelerator previously offered only ChaCha20, which mattered because on
// an ARMv8-capable guest un-accelerated AES-256-GCM already outran accelerated
// ChaCha20: an A10X iPad pushed 8.07 MB/s of scp with aes256-gcm against 4.70
// with accelerated chacha20-poly1305. AES is where the remaining headroom is,
// so it gets its own host-native path.
//
// This exercises the SYSCALL directly rather than going through OpenSSL,
// because a libc or a provider reaching the same answer by its own software
// route would hide a broken accelerator completely.
//
// Verifies: the capability query answers without touching any buffer; a NIST
// vector seals bit-exact (ciphertext and tag); open round-trips it; a tampered
// ciphertext is rejected with EBADMSG *and* the output buffer is zeroed, since
// open decrypts eagerly and the guest must never be handed unauthenticated
// plaintext; AAD is authenticated (changing it alone changes the tag); a
// multi-page buffer crossing page boundaries works, which is the case that
// exercises the kernel's per-page-span walk; an oversized request is refused
// with EMSGSIZE rather than being truncated; and a bad guest pointer faults
// instead of crashing the emulator.
//
// SKIPs when the accelerator is off or unavailable (x86 guests, where the
// syscall raises SIGSYS by design, and any host without AES instructions).
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "test_common.h"

#define ISH_SYS_AEAD 0xacc0

enum { ALG_AES256_GCM = 2 };
enum { OP_SEAL = 0, OP_OPEN = 1, OP_QUERY = 4 };

struct ish_aead_req {
    uint32_t op, alg;
    uint64_t key, nonce, aad, aadlen, in, inlen, out, tag;
};

static long aead(struct ish_aead_req *r) {
    return syscall(ISH_SYS_AEAD, r);
}

static void check(int cond, const char *what) {
    if (cond) {
        test_logf("ok: %s\n", what);
    } else {
        printf("FAIL: %s (errno=%d %s)\n", what, errno, strerror(errno));
        failures_total++;
    }
}

// NIST/CAVP AES-256-GCM: 60-byte plaintext, 20-byte AAD.
static const uint8_t key[32] = {
    0xfe,0xff,0xe9,0x92,0x86,0x65,0x73,0x1c, 0x6d,0x6a,0x8f,0x94,0x67,0x30,0x83,0x08,
    0xfe,0xff,0xe9,0x92,0x86,0x65,0x73,0x1c, 0x6d,0x6a,0x8f,0x94,0x67,0x30,0x83,0x08,
};
static const uint8_t iv[12] = {
    0xca,0xfe,0xba,0xbe,0xfa,0xce,0xdb,0xad, 0xde,0xca,0xf8,0x88,
};
static const uint8_t pt[60] = {
    0xd9,0x31,0x32,0x25,0xf8,0x84,0x06,0xe5, 0xa5,0x59,0x09,0xc5,0xaf,0xf5,0x26,0x9a,
    0x86,0xa7,0xa9,0x53,0x15,0x34,0xf7,0xda, 0x2e,0x4c,0x30,0x3d,0x8a,0x31,0x8a,0x72,
    0x1c,0x3c,0x0c,0x95,0x95,0x68,0x09,0x53, 0x2f,0xcf,0x0e,0x24,0x49,0xa6,0xb5,0x25,
    0xb1,0x6a,0xed,0xf5,0xaa,0x0d,0xe6,0x57, 0xba,0x63,0x7b,0x39,
};
static const uint8_t aad[20] = {
    0xfe,0xed,0xfa,0xce,0xde,0xad,0xbe,0xef, 0xfe,0xed,0xfa,0xce,0xde,0xad,0xbe,0xef,
    0xab,0xad,0xda,0xd2,
};
static const uint8_t want_ct[60] = {
    0x52,0x2d,0xc1,0xf0,0x99,0x56,0x7d,0x07, 0xf4,0x7f,0x37,0xa3,0x2a,0x84,0x42,0x7d,
    0x64,0x3a,0x8c,0xdc,0xbf,0xe5,0xc0,0xc9, 0x75,0x98,0xa2,0xbd,0x25,0x55,0xd1,0xaa,
    0x8c,0xb0,0x8e,0x48,0x59,0x0d,0xbb,0x3d, 0xa7,0xb0,0x8b,0x10,0x56,0x82,0x88,0x38,
    0xc5,0xf6,0x1e,0x63,0x93,0xba,0x7a,0x0a, 0xbc,0xc9,0xf6,0x62,
};
static const uint8_t want_tag[16] = {
    0x76,0xfc,0x6e,0xce,0x0f,0x4e,0x17,0x68, 0xcd,0xdf,0x88,0x53,0xbb,0x2d,0x55,0x1b,
};

static void fill_req(struct ish_aead_req *r, uint32_t op,
        const void *in, size_t inlen, void *out, void *tag, const void *aadp, size_t aadlen) {
    memset(r, 0, sizeof(*r));
    r->op = op;
    r->alg = ALG_AES256_GCM;
    r->key = (uint64_t) (uintptr_t) key;
    r->nonce = (uint64_t) (uintptr_t) iv;
    r->aad = (uint64_t) (uintptr_t) aadp;
    r->aadlen = aadlen;
    r->in = (uint64_t) (uintptr_t) in;
    r->inlen = inlen;
    r->out = (uint64_t) (uintptr_t) out;
    r->tag = (uint64_t) (uintptr_t) tag;
}

static void test_vector(void) {
    uint8_t ct[60], tag[16], back[60];
    struct ish_aead_req r;

    fill_req(&r, OP_SEAL, pt, sizeof(pt), ct, tag, aad, sizeof(aad));
    check(aead(&r) == 0, "seal succeeds");
    check(memcmp(ct, want_ct, sizeof(ct)) == 0, "ciphertext matches the NIST vector");
    check(memcmp(tag, want_tag, sizeof(tag)) == 0, "tag matches the NIST vector");

    memset(back, 0xa5, sizeof(back));
    fill_req(&r, OP_OPEN, want_ct, sizeof(want_ct), back, (void *) want_tag, aad, sizeof(aad));
    check(aead(&r) == 0, "open accepts the valid tag");
    check(memcmp(back, pt, sizeof(pt)) == 0, "open recovers the plaintext");
}

static void test_tamper(void) {
    uint8_t ct[60], tag[16], back[60];
    struct ish_aead_req r;

    memcpy(ct, want_ct, sizeof(ct));
    memcpy(tag, want_tag, sizeof(tag));
    ct[7] ^= 0x40;
    memset(back, 0xa5, sizeof(back));
    fill_req(&r, OP_OPEN, ct, sizeof(ct), back, tag, aad, sizeof(aad));
    errno = 0;
    check(aead(&r) != 0 && errno == EBADMSG, "tampered ciphertext rejected with EBADMSG");

    // The kernel decrypts straight into the guest buffer before it can know
    // the tag is bad, so it must scrub what it wrote.
    int nonzero = 0;
    for (size_t i = 0; i < sizeof(back); i++)
        if (back[i] != 0) nonzero = 1;
    check(!nonzero, "rejected open leaves no plaintext in the output buffer");

    // AAD is authenticated even though it is not encrypted.
    uint8_t bad_aad[20];
    memcpy(bad_aad, aad, sizeof(bad_aad));
    bad_aad[3] ^= 0x01;
    fill_req(&r, OP_OPEN, want_ct, sizeof(want_ct), back, (void *) want_tag, bad_aad, sizeof(bad_aad));
    errno = 0;
    check(aead(&r) != 0 && errno == EBADMSG, "modified AAD rejected");
}

// The kernel transforms guest memory a page-span at a time; a buffer spanning
// several pages is what exercises that walk, and a round trip that survives it
// is what proves the counter and GHASH state carry across spans correctly.
static void test_multipage(void) {
    size_t len = 3 * 4096 + 1237;
    uint8_t *in = malloc(len), *out = malloc(len), *back = malloc(len);
    uint8_t tag[16];
    struct ish_aead_req r;
    if (in == NULL || out == NULL || back == NULL) {
        printf("FAIL: malloc\n");
        failures_total++;
        return;
    }
    for (size_t i = 0; i < len; i++)
        in[i] = (uint8_t) (i * 7 + (i >> 8));

    fill_req(&r, OP_SEAL, in, len, out, tag, aad, sizeof(aad));
    check(aead(&r) == 0, "multi-page seal succeeds");
    check(memcmp(out, in, len) != 0, "multi-page output is actually encrypted");

    fill_req(&r, OP_OPEN, out, len, back, tag, aad, sizeof(aad));
    check(aead(&r) == 0, "multi-page open succeeds");
    check(memcmp(back, in, len) == 0, "multi-page round trip is exact");

    // In-place is the shape a provider is most likely to use.
    fill_req(&r, OP_SEAL, in, len, in, tag, aad, sizeof(aad));
    check(aead(&r) == 0, "in-place seal succeeds");
    check(memcmp(in, out, len) == 0, "in-place seal matches the out-of-place result");

    free(in); free(out); free(back);
}

static void test_limits(void) {
    uint8_t ct[60], tag[16];
    struct ish_aead_req r;

    // Oversized: must be refused, not silently truncated. The caller falls
    // back to its own software path on this.
    fill_req(&r, OP_SEAL, pt, (size_t) 64 << 20, ct, tag, aad, sizeof(aad));
    errno = 0;
    check(aead(&r) != 0 && errno == EMSGSIZE, "oversized request refused with EMSGSIZE");

    // A bad pointer must fault cleanly rather than take the emulator down.
    void *bad = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (bad != MAP_FAILED) {
        fill_req(&r, OP_SEAL, bad, 4096, ct, tag, aad, sizeof(aad));
        errno = 0;
        check(aead(&r) != 0 && errno == EFAULT, "unreadable input faults with EFAULT");
        munmap(bad, 4096);
    }

    // An unsupported op on a supported alg is a plain error.
    fill_req(&r, 9, pt, sizeof(pt), ct, tag, aad, sizeof(aad));
    errno = 0;
    check(aead(&r) != 0 && errno == EINVAL, "unknown op rejected with EINVAL");
}

int main(int argc, char **argv) {
    test_init(argc, argv);

    // The query op must answer without dereferencing anything: every pointer
    // in this request is NULL on purpose.
    struct ish_aead_req q;
    memset(&q, 0, sizeof(q));
    q.op = OP_QUERY;
    q.alg = ALG_AES256_GCM;
    errno = 0;
    if (aead(&q) != 0) {
        printf("aes_gcm_accel: SKIP (AES-256-GCM accelerator unavailable, errno=%d %s)\n",
                errno, strerror(errno));
        return 0;
    }
    test_logf("ok: capability query answered with no buffers mapped\n");

    test_vector();
    test_tamper();
    test_multipage();
    test_limits();

    return finish_suite("aes_gcm_accel");
}
