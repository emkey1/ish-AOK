// Host-native ChaCha20-Poly1305 AEAD (RFC 8439), used by the iSH crypto
// accelerator to run the cipher natively instead of emulating the guest's
// libcrypto instruction-by-instruction. Correctness is absolute here (a bug
// is silent data corruption or a security hole), so this is the plain,
// auditable reference construction, validated against the RFC 8439 test
// vectors by ish_accel_crypto_selftest() before the accelerator is enabled.
//
// This file is host code: it operates on host pointers. The syscall/device
// layer is responsible for safely resolving guest buffers to host memory
// (bounds-checked) before calling in here.

#include <stdint.h>
#include <string.h>
#include "kernel/ish_accel_crypto.h"

// ---- ChaCha20 (RFC 8439 sect 2.3) ---------------------------------------

static inline uint32_t rotl32(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }
static inline uint32_t load32le(const uint8_t *p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}
static inline void store32le(uint8_t *p, uint32_t v) {
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

#define QR(a, b, c, d) \
    a += b; d ^= a; d = rotl32(d, 16); \
    c += d; b ^= c; b = rotl32(b, 12); \
    a += b; d ^= a; d = rotl32(d, 8);  \
    c += d; b ^= c; b = rotl32(b, 7)

static void chacha20_block(const uint32_t in[16], uint8_t out[64]) {
    uint32_t x[16];
    memcpy(x, in, sizeof x);
    for (int i = 0; i < 10; i++) {
        QR(x[0], x[4], x[8],  x[12]);
        QR(x[1], x[5], x[9],  x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]);
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8],  x[13]);
        QR(x[3], x[4], x[9],  x[14]);
    }
    for (int i = 0; i < 16; i++)
        store32le(out + 4 * i, x[i] + in[i]);
}

static void chacha20_init(uint32_t st[16], const uint8_t key[32], uint32_t counter,
        const uint8_t nonce[12]) {
    st[0] = 0x61707865; st[1] = 0x3320646e; st[2] = 0x79622d32; st[3] = 0x6b206574;
    for (int i = 0; i < 8; i++) st[4 + i] = load32le(key + 4 * i);
    st[12] = counter;
    st[13] = load32le(nonce + 0);
    st[14] = load32le(nonce + 4);
    st[15] = load32le(nonce + 8);
}

// ---- Poly1305 (RFC 8439 sect 2.5), 64-bit-limb reference ----------------

struct poly1305 {
    uint64_t r[3];   // clamped key r, 44-bit limbs
    uint64_t h[3];   // accumulator
    uint64_t s[2];   // key s (added at the end)
    uint8_t buf[16];
    size_t buflen;
};

static void poly1305_init(struct poly1305 *st, const uint8_t key[32]) {
    // r, clamped, split into 44-bit limbs.
    uint64_t t0 = load32le(key + 0) | ((uint64_t) load32le(key + 4) << 32);
    uint64_t t1 = load32le(key + 8) | ((uint64_t) load32le(key + 12) << 32);
    st->r[0] =   t0                       & 0xffc0fffffff;
    st->r[1] = ((t0 >> 44) | (t1 << 20))  & 0xfffffc0ffff;
    st->r[2] =  (t1 >> 24)                & 0x00ffffffc0f;
    st->h[0] = st->h[1] = st->h[2] = 0;
    st->s[0] = load32le(key + 16) | ((uint64_t) load32le(key + 20) << 32);
    st->s[1] = load32le(key + 24) | ((uint64_t) load32le(key + 28) << 32);
    st->buflen = 0;
}

static void poly1305_blocks(struct poly1305 *st, const uint8_t *m, size_t bytes, int final) {
    const uint64_t hibit = final ? 0 : ((uint64_t) 1 << 40); // 2^128 in the top limb
    uint64_t r0 = st->r[0], r1 = st->r[1], r2 = st->r[2];
    uint64_t h0 = st->h[0], h1 = st->h[1], h2 = st->h[2];
    uint64_t s1 = r1 * (5 << 2), s2 = r2 * (5 << 2);
    while (bytes >= 16) {
        uint64_t t0 = load32le(m + 0) | ((uint64_t) load32le(m + 4) << 32);
        uint64_t t1 = load32le(m + 8) | ((uint64_t) load32le(m + 12) << 32);
        h0 += t0 & 0xfffffffffff;
        h1 += ((t0 >> 44) | (t1 << 20)) & 0xfffffffffff;
        h2 += ((t1 >> 24) & 0x3ffffffffff) | hibit;
        __uint128_t d0 = (__uint128_t) h0 * r0 + (__uint128_t) h1 * s2 + (__uint128_t) h2 * s1;
        __uint128_t d1 = (__uint128_t) h0 * r1 + (__uint128_t) h1 * r0 + (__uint128_t) h2 * s2;
        __uint128_t d2 = (__uint128_t) h0 * r2 + (__uint128_t) h1 * r1 + (__uint128_t) h2 * r0;
        uint64_t c;
        h0 = (uint64_t) d0 & 0xfffffffffff; c = (uint64_t) (d0 >> 44);
        d1 += c; h1 = (uint64_t) d1 & 0xfffffffffff; c = (uint64_t) (d1 >> 44);
        d2 += c; h2 = (uint64_t) d2 & 0x3ffffffffff; c = (uint64_t) (d2 >> 42);
        h0 += c * 5; c = h0 >> 44; h0 &= 0xfffffffffff;
        h1 += c;
        m += 16; bytes -= 16;
    }
    st->h[0] = h0; st->h[1] = h1; st->h[2] = h2;
}

static void poly1305_update(struct poly1305 *st, const uint8_t *m, size_t bytes) {
    if (st->buflen > 0) {
        size_t want = 16 - st->buflen;
        if (want > bytes) want = bytes;
        memcpy(st->buf + st->buflen, m, want);
        st->buflen += want; m += want; bytes -= want;
        if (st->buflen == 16) { poly1305_blocks(st, st->buf, 16, 0); st->buflen = 0; }
    }
    if (bytes >= 16) {
        size_t full = bytes & ~(size_t) 15;
        poly1305_blocks(st, m, full, 0);
        m += full; bytes -= full;
    }
    if (bytes > 0) { memcpy(st->buf, m, bytes); st->buflen = bytes; }
}

static void poly1305_finish(struct poly1305 *st, uint8_t mac[16]) {
    if (st->buflen > 0) {
        st->buf[st->buflen] = 1;
        memset(st->buf + st->buflen + 1, 0, 16 - st->buflen - 1);
        poly1305_blocks(st, st->buf, 16, 1);
    }
    uint64_t h0 = st->h[0], h1 = st->h[1], h2 = st->h[2];
    uint64_t c;
    c = h1 >> 44; h1 &= 0xfffffffffff; h2 += c;
    c = h2 >> 42; h2 &= 0x3ffffffffff; h0 += c * 5;
    c = h0 >> 44; h0 &= 0xfffffffffff; h1 += c;
    // compute h + -p (i.e. h - (2^130-5)); use it if there was no borrow
    uint64_t g0 = h0 + 5; c = g0 >> 44; g0 &= 0xfffffffffff;
    uint64_t g1 = h1 + c; c = g1 >> 44; g1 &= 0xfffffffffff;
    uint64_t g2 = h2 + c - ((uint64_t) 1 << 42);
    uint64_t mask = (g2 >> 63) - 1; // 0 if g2 borrowed (use h), all-ones if not (use g)
    g0 &= mask; g1 &= mask; g2 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0; h1 = (h1 & mask) | g1; h2 = (h2 & mask) | g2;
    // h = (h + s) mod 2^128, serialize little-endian
    uint64_t f0 = (h0 | (h1 << 44)) + st->s[0];
    uint64_t f1 = ((h1 >> 20) | (h2 << 24)) + st->s[1] + (f0 < st->s[0] ? 1 : 0);
    for (int i = 0; i < 8; i++) mac[i]     = (uint8_t) (f0 >> (8 * i));
    for (int i = 0; i < 8; i++) mac[8 + i] = (uint8_t) (f1 >> (8 * i));
}

// ---- AEAD ChaCha20-Poly1305 (RFC 8439 sect 2.8) -------------------------

static const uint8_t poly_pad[16] = {0};

static void aead_otk(const uint8_t key[32], const uint8_t nonce[12], uint8_t otk[32]) {
    uint32_t st[16];
    chacha20_init(st, key, 0, nonce);
    uint8_t block0[64];
    chacha20_block(st, block0);
    memcpy(otk, block0, 32);
}

// The one-shot entry points are now thin wrappers over the streaming API
// (defined below), so there is a single implementation of the AEAD
// construction to audit and validate.
void ish_chacha20_poly1305_seal(const uint8_t key[32], const uint8_t nonce[12],
        const uint8_t *aad, size_t aadlen, const uint8_t *pt, size_t ptlen,
        uint8_t *ct, uint8_t tag[16]) {
    struct ish_aead_stream s;
    ish_aead_begin(&s, key, nonce, aad, aadlen, 0);
    if (ptlen) ish_aead_update(&s, pt, ct, ptlen);
    ish_aead_finish(&s, NULL, tag);
}

int ish_chacha20_poly1305_open(const uint8_t key[32], const uint8_t nonce[12],
        const uint8_t *aad, size_t aadlen, const uint8_t *ct, size_t ctlen,
        const uint8_t tag[16], uint8_t *pt) {
    struct ish_aead_stream s;
    ish_aead_begin(&s, key, nonce, aad, aadlen, 1);
    if (ctlen) ish_aead_update(&s, ct, pt, ctlen); // decrypts eagerly into pt
    if (ish_aead_finish(&s, tag, NULL) != 0) {
        if (ctlen) memset(pt, 0, ctlen); // auth failed: do not release plaintext
        return -1;
    }
    return 0;
}

// ---- Raw ChaCha20 stream (EVP_chacha20-compatible) ----------------------
// iv = [ctr32_le][nonce96]. Keystream block counter starts at that ctr and
// increments per 64-byte block, wrapping in the low 32 bits like OpenSSL.
void ish_chacha20_stream(const uint8_t key[32], const uint8_t iv[16],
        const uint8_t *in, uint8_t *out, size_t len) {
    uint32_t counter = load32le(iv);
    const uint8_t *nonce = iv + 4;
    uint8_t ks[64];
    while (len > 0) {
        uint32_t st[16];
        chacha20_init(st, key, counter++, nonce);
        chacha20_block(st, ks);
        size_t n = len < 64 ? len : 64;
        for (size_t i = 0; i < n; i++)
            out[i] = in[i] ^ ks[i];
        in += n; out += n; len -= n;
    }
}

struct chacha_stream {
    uint8_t key[32], nonce[12];
    uint32_t counter;
    uint8_t ks[64];
    unsigned ks_pos;
};
_Static_assert(sizeof(struct chacha_stream) <= sizeof(struct ish_chacha_stream),
        "ish_chacha_stream opaque buffer too small");

void ish_chacha20_stream_begin(struct ish_chacha_stream *sp,
        const uint8_t key[32], const uint8_t iv[16]) {
    struct chacha_stream *s = (struct chacha_stream *) sp;
    memcpy(s->key, key, 32);
    memcpy(s->nonce, iv + 4, 12);
    s->counter = load32le(iv);
    s->ks_pos = 64; // force refill on first byte
}

void ish_chacha20_stream_update(struct ish_chacha_stream *sp,
        const uint8_t *in, uint8_t *out, size_t len) {
    struct chacha_stream *s = (struct chacha_stream *) sp;
    size_t i = 0;
    while (i < len) {
        if (s->ks_pos == 64) {
            uint32_t st[16];
            chacha20_init(st, s->key, s->counter++, s->nonce);
            chacha20_block(st, s->ks);
            s->ks_pos = 0;
        }
        unsigned take = 64 - s->ks_pos;
        if (take > len - i) take = (unsigned) (len - i);
        for (unsigned j = 0; j < take; j++)
            out[i + j] = in[i + j] ^ s->ks[s->ks_pos + j];
        s->ks_pos += take;
        i += take;
    }
}

// ---- Streaming AEAD -----------------------------------------------------
// Reuses the validated chacha20_block / poly1305_* primitives; only the
// span-buffering plumbing is new. The Poly1305 tag is always computed over
// the CIPHERTEXT (out for seal, in for open), matching the one-shot path.

struct aead_stream {
    uint8_t key[32], nonce[12];
    uint32_t counter;      // next chacha data block (starts at 1)
    uint8_t ks[64];        // current keystream block
    unsigned ks_pos;       // 0..64; 64 => needs refill
    struct poly1305 poly;
    uint64_t aadlen, datalen;
    int is_open;
};
_Static_assert(sizeof(struct aead_stream) <= sizeof(struct ish_aead_stream),
        "ish_aead_stream opaque buffer too small");

void ish_aead_begin(struct ish_aead_stream *sp, const uint8_t key[32],
        const uint8_t nonce[12], const uint8_t *aad, size_t aadlen, int is_open) {
    struct aead_stream *s = (struct aead_stream *) sp;
    memcpy(s->key, key, 32);
    memcpy(s->nonce, nonce, 12);
    s->counter = 1;
    s->ks_pos = 64; // force a refill on the first data byte
    s->aadlen = aadlen;
    s->datalen = 0;
    s->is_open = is_open;
    uint8_t otk[32];
    aead_otk(key, nonce, otk);
    poly1305_init(&s->poly, otk);
    if (aadlen) poly1305_update(&s->poly, aad, aadlen);
    if (aadlen % 16) poly1305_update(&s->poly, poly_pad, 16 - (aadlen % 16));
}

void ish_aead_update(struct ish_aead_stream *sp, const uint8_t *in, uint8_t *out, size_t len) {
    struct aead_stream *s = (struct aead_stream *) sp;
    s->datalen += len;
    // For open, Poly1305 is over the input ciphertext -- feed it directly
    // (the whole span is contiguous). For seal it's over the output, fed
    // below after we produce it.
    if (s->is_open)
        poly1305_update(&s->poly, in, len);
    size_t i = 0;
    while (i < len) {
        if (s->ks_pos == 64) {
            uint32_t st[16];
            chacha20_init(st, s->key, s->counter++, s->nonce);
            chacha20_block(st, s->ks);
            s->ks_pos = 0;
        }
        unsigned take = 64 - s->ks_pos;
        if (take > len - i) take = (unsigned) (len - i);
        for (unsigned j = 0; j < take; j++)
            out[i + j] = in[i + j] ^ s->ks[s->ks_pos + j];
        s->ks_pos += take;
        i += take;
    }
    if (!s->is_open)
        poly1305_update(&s->poly, out, len);
}

int ish_aead_finish(struct ish_aead_stream *sp, const uint8_t *tag_in, uint8_t *tag_out) {
    struct aead_stream *s = (struct aead_stream *) sp;
    if (s->datalen % 16) poly1305_update(&s->poly, poly_pad, 16 - (s->datalen % 16));
    uint8_t lengths[16];
    for (int i = 0; i < 8; i++) lengths[i]     = (uint8_t) (s->aadlen  >> (8 * i));
    for (int i = 0; i < 8; i++) lengths[8 + i] = (uint8_t) (s->datalen >> (8 * i));
    poly1305_update(&s->poly, lengths, 16);
    uint8_t tag[16];
    poly1305_finish(&s->poly, tag);
    if (s->is_open) {
        uint8_t diff = 0;
        for (int i = 0; i < 16; i++) diff |= tag[i] ^ tag_in[i];
        return diff == 0 ? 0 : -1;
    }
    memcpy(tag_out, tag, 16);
    return 0;
}

// ---- AEAD tag ("MAC") only ----------------------------------------------
// Reuses the exact Poly1305 path of the streaming AEAD, minus the ChaCha20
// data encryption -- so the tag is identical to what ish_aead_seal produces
// for the same aad + ciphertext. Data length is accumulated from the update
// calls for the trailing length block.

struct aead_mac {
    struct poly1305 poly;
    uint64_t aadlen, ctlen;
};
_Static_assert(sizeof(struct aead_mac) <= sizeof(struct ish_aead_mac),
        "ish_aead_mac opaque buffer too small");

void ish_aead_mac_begin(struct ish_aead_mac *sp, const uint8_t key[32],
        const uint8_t nonce[12], const uint8_t *aad, size_t aadlen) {
    struct aead_mac *s = (struct aead_mac *) sp;
    uint8_t otk[32];
    aead_otk(key, nonce, otk);
    poly1305_init(&s->poly, otk);
    if (aadlen) poly1305_update(&s->poly, aad, aadlen);
    if (aadlen % 16) poly1305_update(&s->poly, poly_pad, 16 - (aadlen % 16));
    s->aadlen = aadlen;
    s->ctlen = 0;
}

void ish_aead_mac_update(struct ish_aead_mac *sp, const uint8_t *ct, size_t len) {
    struct aead_mac *s = (struct aead_mac *) sp;
    poly1305_update(&s->poly, ct, len);
    s->ctlen += len;
}

void ish_aead_mac_final(struct ish_aead_mac *sp, uint8_t tag[16]) {
    struct aead_mac *s = (struct aead_mac *) sp;
    if (s->ctlen % 16) poly1305_update(&s->poly, poly_pad, 16 - (s->ctlen % 16));
    uint8_t lengths[16];
    for (int i = 0; i < 8; i++) lengths[i]     = (uint8_t) (s->aadlen >> (8 * i));
    for (int i = 0; i < 8; i++) lengths[8 + i] = (uint8_t) (s->ctlen  >> (8 * i));
    poly1305_update(&s->poly, lengths, 16);
    poly1305_finish(&s->poly, tag);
}

// ---- Bare Poly1305 -------------------------------------------------------
// The validated primitive above, behind the opaque type the header exports.

_Static_assert(sizeof(struct poly1305) <= sizeof(struct ish_poly1305),
        "ish_poly1305 opaque buffer too small");

void ish_poly1305_begin(struct ish_poly1305 *sp, const uint8_t key[32]) {
    poly1305_init((struct poly1305 *) sp, key);
}

void ish_poly1305_update(struct ish_poly1305 *sp, const uint8_t *m, size_t len) {
    poly1305_update((struct poly1305 *) sp, m, len);
}

void ish_poly1305_final(struct ish_poly1305 *sp, uint8_t tag[16]) {
    poly1305_finish((struct poly1305 *) sp, tag);
}

// ---- Self-test against RFC 8439 vectors ---------------------------------

static _Bool ct_eq(const uint8_t *a, const uint8_t *b, size_t n) {
    uint8_t d = 0; for (size_t i = 0; i < n; i++) d |= a[i] ^ b[i]; return d == 0;
}

_Bool ish_accel_crypto_selftest(void) {
    // RFC 8439 sect 2.5.2: standalone Poly1305 (the highest-risk component).
    {
        static const uint8_t key[32] = {
            0x85,0xd6,0xbe,0x78,0x57,0x55,0x6d,0x33,0x7f,0x44,0x52,0xfe,0x42,0xd5,0x06,0xa8,
            0x01,0x03,0x80,0x8a,0xfb,0x0d,0xb2,0xfd,0x4a,0xbf,0xf6,0xaf,0x41,0x49,0xf5,0x1b};
        static const char msg[] = "Cryptographic Forum Research Group";
        static const uint8_t want[16] = {
            0xa8,0x06,0x1d,0xc1,0x30,0x51,0x36,0xc6,0xc2,0x2b,0x8b,0xaf,0x0c,0x01,0x27,0xa9};
        struct poly1305 st; uint8_t mac[16];
        poly1305_init(&st, key);
        poly1305_update(&st, (const uint8_t *) msg, sizeof msg - 1);
        poly1305_finish(&st, mac);
        if (!ct_eq(mac, want, 16)) return 0;
        // The same vector through the exported streaming API, split where no
        // 16-byte block boundary falls, because the guest's op feeds it page
        // spans of any length.
        struct ish_poly1305 sp;
        ish_poly1305_begin(&sp, key);
        ish_poly1305_update(&sp, (const uint8_t *) msg, 7);
        ish_poly1305_update(&sp, (const uint8_t *) msg + 7, 20);
        ish_poly1305_update(&sp, (const uint8_t *) msg + 27, sizeof msg - 1 - 27);
        ish_poly1305_final(&sp, mac);
        if (!ct_eq(mac, want, 16)) return 0;
    }
    // RFC 8439 sect 2.8.2: full AEAD seal (and round-trip open).
    {
        static const uint8_t key[32] = {
            0x80,0x81,0x82,0x83,0x84,0x85,0x86,0x87,0x88,0x89,0x8a,0x8b,0x8c,0x8d,0x8e,0x8f,
            0x90,0x91,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0x9b,0x9c,0x9d,0x9e,0x9f};
        static const uint8_t nonce[12] = {0x07,0,0,0,0x40,0x41,0x42,0x43,0x44,0x45,0x46,0x47};
        static const uint8_t aad[12] = {0x50,0x51,0x52,0x53,0xc0,0xc1,0xc2,0xc3,0xc4,0xc5,0xc6,0xc7};
        static const char pt[] = "Ladies and Gentlemen of the class of '99: "
            "If I could offer you only one tip for the future, sunscreen would be it.";
        static const uint8_t ct_want[114] = {
            0xd3,0x1a,0x8d,0x34,0x64,0x8e,0x60,0xdb,0x7b,0x86,0xaf,0xbc,0x53,0xef,0x7e,0xc2,
            0xa4,0xad,0xed,0x51,0x29,0x6e,0x08,0xfe,0xa9,0xe2,0xb5,0xa7,0x36,0xee,0x62,0xd6,
            0x3d,0xbe,0xa4,0x5e,0x8c,0xa9,0x67,0x12,0x82,0xfa,0xfb,0x69,0xda,0x92,0x72,0x8b,
            0x1a,0x71,0xde,0x0a,0x9e,0x06,0x0b,0x29,0x05,0xd6,0xa5,0xb6,0x7e,0xcd,0x3b,0x36,
            0x92,0xdd,0xbd,0x7f,0x2d,0x77,0x8b,0x8c,0x98,0x03,0xae,0xe3,0x28,0x09,0x1b,0x58,
            0xfa,0xb3,0x24,0xe4,0xfa,0xd6,0x75,0x94,0x55,0x85,0x80,0x8b,0x48,0x31,0xd7,0xbc,
            0x3f,0xf4,0xde,0xf0,0x8e,0x4b,0x7a,0x9d,0xe5,0x76,0xd2,0x65,0x86,0xce,0xc6,0x4b,
            0x61,0x16};
        static const uint8_t tag_want[16] = {
            0x1a,0xe1,0x0b,0x59,0x4f,0x09,0xe2,0x6a,0x7e,0x90,0x2e,0xcb,0xd0,0x60,0x06,0x91};
        size_t ptlen = sizeof pt - 1;
        uint8_t ct[128], tag[16], back[128];
        ish_chacha20_poly1305_seal(key, nonce, aad, sizeof aad,
                (const uint8_t *) pt, ptlen, ct, tag);
        if (!ct_eq(ct, ct_want, ptlen)) return 0;
        if (!ct_eq(tag, tag_want, 16)) return 0;
        // round-trip open must succeed and recover the plaintext
        if (ish_chacha20_poly1305_open(key, nonce, aad, sizeof aad, ct, ptlen, tag, back) != 0)
            return 0;
        if (!ct_eq(back, (const uint8_t *) pt, ptlen)) return 0;
        // a corrupted tag must be rejected
        uint8_t bad = tag[0] ^ 0x01, savetag[16];
        memcpy(savetag, tag, 16); savetag[0] = bad;
        if (ish_chacha20_poly1305_open(key, nonce, aad, sizeof aad, ct, ptlen, savetag, back) == 0)
            return 0;
    }
    return 1;
}
