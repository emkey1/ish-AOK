// Host-native AES-256-GCM for the iSH-AOK crypto accelerator.
//
// Hardware only. AES lives here because the guest is emulating it instruction
// by instruction, and the point is to run it at host speed -- but a software
// AES fast enough to be worth the syscall would have to be table-driven, and a
// table-driven AES leaks key material through the data cache. So this uses the
// host's AES instructions (ARMv8 AESE/AESMC and PMULL for GHASH) or it reports
// itself unavailable and the guest keeps using its own code. There is no
// portable fallback on purpose: "slower" is a fine outcome, "constant-time on
// the device but not on some other host" is not.
//
// Every Apple device iSH-AOK runs on has these instructions, so in practice
// the unavailable path is only hit on an unusual CLI build host.
//
// The streaming shape mirrors kernel/ish_accel_crypto.c's ChaCha20-Poly1305:
// begin/update/finish, with update callable on arbitrary spans so the caller
// can walk guest pages directly with no bounce buffer.

#include <stdint.h>
#include <string.h>
#include "kernel/ish_accel_crypto.h"

#if defined(__aarch64__) && defined(__ARM_FEATURE_AES) && defined(__ARM_FEATURE_CRYPTO)
#define ISH_AES_HW 1
#include <arm_neon.h>
#endif

_Bool ish_aes_gcm_available(void) {
#ifdef ISH_AES_HW
    return 1;
#else
    return 0;
#endif
}

#ifndef ISH_AES_HW

// No hardware: every entry point is a no-op and ish_aes_gcm_available() has
// already told the caller not to use them.
void ish_aes256_gcm_begin(struct ish_aes_gcm_stream *s, const uint8_t key[32],
        const uint8_t nonce[12], const uint8_t *aad, size_t aadlen, int is_open) {
    (void) s; (void) key; (void) nonce; (void) aad; (void) aadlen; (void) is_open;
}
void ish_aes256_gcm_update(struct ish_aes_gcm_stream *s, const uint8_t *in,
        uint8_t *out, size_t len) {
    (void) s; (void) in; (void) out; (void) len;
}
int ish_aes256_gcm_finish(struct ish_aes_gcm_stream *s, const uint8_t *tag_in,
        uint8_t *tag_out) {
    (void) s; (void) tag_in; (void) tag_out;
    return -1;
}
_Bool ish_accel_aes_selftest(void) { return 0; }

#else // ISH_AES_HW

#define AES256_ROUNDS 14

struct aes_gcm {
    uint8x16_t rk[AES256_ROUNDS + 1];
    uint8x16_t h;          // GHASH key, reflected and pre-shifted (see ghash_mul)
    uint8x16_t acc;        // GHASH accumulator, reflected domain
    uint8_t j0[16];        // counter block 0 (IV || 1); its encryption masks the tag
    uint32_t counter;      // current CTR value (the low 32 bits of the block)
    uint8_t ks[16];        // leftover keystream from a partial block
    size_t ks_left;
    uint8_t gbuf[16];      // partial GHASH block
    size_t gbuf_len;
    uint64_t aad_len, data_len;
    int is_open;
};

_Static_assert(sizeof(struct aes_gcm) <= sizeof(struct ish_aes_gcm_stream),
        "ish_aes_gcm_stream too small");

// ---- AES ------------------------------------------------------------------

// SubWord via the AES instruction, so the key schedule touches no lookup
// table. AESE computes ShiftRows(SubBytes(state ^ key)); with the same word in
// all four columns every row is uniform, so ShiftRows is the identity and the
// result is SubWord(w) broadcast across the vector.
static inline uint32_t aes_subword(uint32_t w) {
    uint8x16_t v = vreinterpretq_u8_u32(vdupq_n_u32(w));
    v = vaeseq_u8(v, vdupq_n_u8(0));
    return vgetq_lane_u32(vreinterpretq_u32_u8(v), 0);
}

static void aes256_expand_key(struct aes_gcm *g, const uint8_t key[32]) {
    uint32_t w[4 * (AES256_ROUNDS + 1)];
    static const uint8_t rcon[] = { 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40 };
    for (int i = 0; i < 8; i++)
        memcpy(&w[i], key + 4 * i, 4);
    for (int i = 8; i < 4 * (AES256_ROUNDS + 1); i++) {
        uint32_t t = w[i - 1];
        if (i % 8 == 0) {
            t = (t >> 8) | (t << 24);            // RotWord (little-endian word)
            t = aes_subword(t);
            t ^= (uint32_t) rcon[i / 8 - 1];
        } else if (i % 8 == 4) {
            t = aes_subword(t);
        }
        w[i] = w[i - 8] ^ t;
    }
    for (int r = 0; r <= AES256_ROUNDS; r++)
        g->rk[r] = vreinterpretq_u8_u32(vld1q_u32(&w[4 * r]));
    memset(w, 0, sizeof(w));
}

static inline uint8x16_t aes256_encrypt_block(const struct aes_gcm *g, uint8x16_t s) {
    for (int r = 0; r < AES256_ROUNDS - 1; r++)
        s = vaesmcq_u8(vaeseq_u8(s, g->rk[r]));
    s = vaeseq_u8(s, g->rk[AES256_ROUNDS - 1]);
    return veorq_u8(s, g->rk[AES256_ROUNDS]);
}

// ---- GHASH ----------------------------------------------------------------

// GCM numbers the bits of a block most-significant-first *within each byte*:
// byte k supplies the coefficients of x^(8k) through x^(8k+7), in that order.
// A carry-less multiply wants coefficient x^i at bit i of a little-endian
// 128-bit value, and byte k already sits at bits 8k..8k+7 there, so reversing
// the bits inside every byte (and nothing else) is the whole conversion. It is
// its own inverse, so the same helper converts back.
//
// That leaves the product off by a single shift, which is folded into the
// GHASH key once in begin() so the per-block path is just multiply-and-reduce.
static inline uint8x16_t gcm_bits(uint8x16_t v) {
    return vrbitq_u8(v);
}

static inline uint64_t lane0(uint8x16_t v) { return vgetq_lane_u64(vreinterpretq_u64_u8(v), 0); }
static inline uint64_t lane1(uint8x16_t v) { return vgetq_lane_u64(vreinterpretq_u64_u8(v), 1); }

static inline uint8x16_t clmul64(uint64_t a, uint64_t b) {
    return vreinterpretq_u8_p128(vmull_p64((poly64_t) a, (poly64_t) b));
}

// 128x128 -> 256 carry-less product, Karatsuba.
static inline void clmul128(uint8x16_t a, uint8x16_t b, uint8x16_t *lo, uint8x16_t *hi) {
    uint64_t a0 = lane0(a), a1 = lane1(a), b0 = lane0(b), b1 = lane1(b);
    uint8x16_t l = clmul64(a0, b0);
    uint8x16_t h = clmul64(a1, b1);
    uint8x16_t m = clmul64(a0 ^ a1, b0 ^ b1);
    m = veorq_u8(m, veorq_u8(l, h));
    uint8x16_t zero = vdupq_n_u8(0);
    *lo = veorq_u8(l, vextq_u8(zero, m, 8));   // l ^ (m << 64)
    *hi = veorq_u8(h, vextq_u8(m, zero, 8));   // h ^ (m >> 64)
}

// Reduce a 256-bit product modulo x^128 + x^7 + x^2 + x + 1. In this reversed
// representation x^128 folds back as the constant 0x87, so the high half is
// multiplied by it and xored down; the product of that can overflow 128 bits
// by a few bits, so it folds a second time.
static inline uint8x16_t gf_reduce(uint8x16_t lo, uint8x16_t hi) {
    const uint64_t poly = 0x87;
    uint8x16_t t0 = clmul64(lane0(hi), poly);   // hi.lo * 0x87
    uint8x16_t t1 = clmul64(lane1(hi), poly);   // hi.hi * 0x87
    uint8x16_t zero = vdupq_n_u8(0);
    // fold hi.hi one word up, keeping whatever spills past 128 bits
    lo = veorq_u8(lo, t0);
    lo = veorq_u8(lo, vextq_u8(zero, t1, 8));
    // t1 << 64 spills t1's own high half past bit 127; fold that back too.
    // Unconditional: clmul by zero is zero, and a branch here would be a
    // branch on key-dependent data.
    lo = veorq_u8(lo, clmul64(lane1(t1), poly));
    return lo;
}

// acc = (acc ^ block) * H, everything in the reversed representation.
static inline void ghash_block(struct aes_gcm *g, const uint8_t block[16]) {
    uint8x16_t b = gcm_bits(vld1q_u8(block));
    uint8x16_t x = veorq_u8(g->acc, b);
    uint8x16_t lo, hi;
    clmul128(x, g->h, &lo, &hi);
    g->acc = gf_reduce(lo, hi);
}

static void ghash_bytes(struct aes_gcm *g, const uint8_t *p, size_t len) {
    if (g->gbuf_len != 0) {
        size_t take = 16 - g->gbuf_len;
        if (take > len) take = len;
        memcpy(g->gbuf + g->gbuf_len, p, take);
        g->gbuf_len += take; p += take; len -= take;
        if (g->gbuf_len == 16) { ghash_block(g, g->gbuf); g->gbuf_len = 0; }
    }
    while (len >= 16) { ghash_block(g, p); p += 16; len -= 16; }
    if (len != 0) { memcpy(g->gbuf, p, len); g->gbuf_len = len; }
}

static void ghash_pad(struct aes_gcm *g) {
    if (g->gbuf_len != 0) {
        memset(g->gbuf + g->gbuf_len, 0, 16 - g->gbuf_len);
        ghash_block(g, g->gbuf);
        g->gbuf_len = 0;
    }
}

// ---- GCM ------------------------------------------------------------------

static inline uint8x16_t ctr_block(const struct aes_gcm *g, uint32_t counter) {
    uint8_t b[16];
    memcpy(b, g->j0, 12);
    b[12] = (uint8_t) (counter >> 24); b[13] = (uint8_t) (counter >> 16);
    b[14] = (uint8_t) (counter >> 8);  b[15] = (uint8_t) counter;
    return vld1q_u8(b);
}

void ish_aes256_gcm_begin(struct ish_aes_gcm_stream *sp, const uint8_t key[32],
        const uint8_t nonce[12], const uint8_t *aad, size_t aadlen, int is_open) {
    struct aes_gcm *g = (struct aes_gcm *) sp;
    memset(g, 0, sizeof(*g));
    g->is_open = is_open;
    aes256_expand_key(g, key);

    // H = E_K(0), converted to the bit-per-coefficient form the multiply uses.
    // No pre-shift: implementations that keep blocks byte-swapped instead of
    // bit-reversed need one, but here bit i really is the coefficient of x^i,
    // so a carry-less product is already the polynomial product.
    uint8_t hbytes[16];
    vst1q_u8(hbytes, aes256_encrypt_block(g, vdupq_n_u8(0)));
    g->h = gcm_bits(vld1q_u8(hbytes));
    memset(hbytes, 0, sizeof(hbytes));

    // 12-byte IV: J0 is the IV followed by the 32-bit counter 1, and the data
    // blocks start at 2.
    memcpy(g->j0, nonce, 12);
    g->counter = 1;

    if (aadlen != 0) {
        ghash_bytes(g, aad, aadlen);
        g->aad_len = aadlen;
        ghash_pad(g);
    }
}

void ish_aes256_gcm_update(struct ish_aes_gcm_stream *sp, const uint8_t *in,
        uint8_t *out, size_t len) {
    struct aes_gcm *g = (struct aes_gcm *) sp;
    if (len == 0)
        return;
    g->data_len += len;

    // Decrypt authenticates the ciphertext, which is the input; encrypt
    // authenticates the ciphertext it just produced, which is the output.
    const uint8_t *ghash_src = g->is_open ? in : out;
    size_t ghash_len = len;

    // Finish any keystream left over from a previous partial span first, so a
    // caller may hand us spans of any length.
    while (g->ks_left != 0 && len != 0) {
        *out++ = (uint8_t) (*in++ ^ g->ks[16 - g->ks_left]);
        g->ks_left--;
        len--;
    }
    while (len >= 16) {
        uint8x16_t ks = aes256_encrypt_block(g, ctr_block(g, ++g->counter));
        vst1q_u8(out, veorq_u8(vld1q_u8(in), ks));
        in += 16; out += 16; len -= 16;
    }
    if (len != 0) {
        uint8x16_t ks = aes256_encrypt_block(g, ctr_block(g, ++g->counter));
        vst1q_u8(g->ks, ks);
        for (size_t i = 0; i < len; i++)
            out[i] = (uint8_t) (in[i] ^ g->ks[i]);
        // The unused tail ks[len..15] carries into the next span; the
        // consumption loop above indexes it as ks[16 - ks_left].
        g->ks_left = 16 - len;
    }

    ghash_bytes(g, ghash_src, ghash_len);
}

static int ct_eq16(const uint8_t *a, const uint8_t *b) {
    uint8_t d = 0;
    for (int i = 0; i < 16; i++)
        d = (uint8_t) (d | (a[i] ^ b[i]));
    return d == 0;
}

int ish_aes256_gcm_finish(struct ish_aes_gcm_stream *sp, const uint8_t *tag_in,
        uint8_t *tag_out) {
    struct aes_gcm *g = (struct aes_gcm *) sp;
    ghash_pad(g);

    uint8_t lens[16];
    uint64_t abits = g->aad_len * 8, dbits = g->data_len * 8;
    for (int i = 0; i < 8; i++) {
        lens[i]     = (uint8_t) (abits >> (56 - 8 * i));
        lens[8 + i] = (uint8_t) (dbits >> (56 - 8 * i));
    }
    ghash_block(g, lens);

    uint8_t s[16];
    vst1q_u8(s, gcm_bits(g->acc));
    uint8x16_t mask = aes256_encrypt_block(g, ctr_block(g, 1));  // E_K(J0)
    uint8_t tag[16];
    vst1q_u8(tag, veorq_u8(vld1q_u8(s), mask));

    int ret = 0;
    if (g->is_open)
        ret = ct_eq16(tag, tag_in) ? 0 : -1;
    else
        memcpy(tag_out, tag, 16);

    memset(s, 0, sizeof(s));
    memset(tag, 0, sizeof(tag));
    return ret;
}

// ---- self-test ------------------------------------------------------------

// NIST SP 800-38D / CAVP AES-256-GCM vectors. Case 3 has no AAD, case 4 adds
// AAD and a truncated final block, and the empty case pins the degenerate path
// where the tag is just E_K(J0) folded with the length block.
_Bool ish_accel_aes_selftest(void) {
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

    struct ish_aes_gcm_stream s;
    uint8_t ct[60], tag[16], back[60];

    // Seal in one span.
    ish_aes256_gcm_begin(&s, key, iv, aad, sizeof(aad), 0);
    ish_aes256_gcm_update(&s, pt, ct, sizeof(pt));
    ish_aes256_gcm_finish(&s, NULL, tag);
    if (memcmp(ct, want_ct, sizeof(ct)) != 0 || memcmp(tag, want_tag, 16) != 0)
        return 0;

    // Seal again in awkward spans: the same answer must come out, or the
    // partial-block keystream and GHASH buffering are wrong.
    memset(ct, 0, sizeof(ct));
    ish_aes256_gcm_begin(&s, key, iv, aad, sizeof(aad), 0);
    ish_aes256_gcm_update(&s, pt, ct, 1);
    ish_aes256_gcm_update(&s, pt + 1, ct + 1, 16);
    ish_aes256_gcm_update(&s, pt + 17, ct + 17, 7);
    ish_aes256_gcm_update(&s, pt + 24, ct + 24, sizeof(pt) - 24);
    ish_aes256_gcm_finish(&s, NULL, tag);
    if (memcmp(ct, want_ct, sizeof(ct)) != 0 || memcmp(tag, want_tag, 16) != 0)
        return 0;

    // Open, and reject a tampered tag.
    ish_aes256_gcm_begin(&s, key, iv, aad, sizeof(aad), 1);
    ish_aes256_gcm_update(&s, want_ct, back, sizeof(want_ct));
    if (ish_aes256_gcm_finish(&s, want_tag, NULL) != 0)
        return 0;
    if (memcmp(back, pt, sizeof(pt)) != 0)
        return 0;

    uint8_t bad[16];
    memcpy(bad, want_tag, 16);
    bad[0] ^= 0x01;
    ish_aes256_gcm_begin(&s, key, iv, aad, sizeof(aad), 1);
    ish_aes256_gcm_update(&s, want_ct, back, sizeof(want_ct));
    if (ish_aes256_gcm_finish(&s, bad, NULL) == 0)
        return 0;

    // Empty plaintext, empty AAD: tag is E_K(J0) xor GHASH of the length block.
    static const uint8_t want_empty_tag[16] = {
        0x53,0x0f,0x8a,0xfb,0xc7,0x45,0x36,0xb9, 0xa9,0x63,0xb4,0xf1,0xc4,0xcb,0x73,0x8b,
    };
    static const uint8_t zkey[32] = {0};
    static const uint8_t ziv[12] = {0};
    ish_aes256_gcm_begin(&s, zkey, ziv, NULL, 0, 0);
    ish_aes256_gcm_finish(&s, NULL, tag);
    if (memcmp(tag, want_empty_tag, 16) != 0)
        return 0;

    return 1;
}

#endif // ISH_AES_HW
