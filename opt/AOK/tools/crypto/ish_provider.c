// iSH OpenSSL provider: routes ChaCha20 (the stream cipher OpenSSH's
// chacha20-poly1305@openssh.com uses via cipher-chachapoly-libcrypto) through
// the iSH crypto accelerator syscall, so ssh/scp encrypt at host-native speed
// instead of emulating libcrypto. Loaded via openssl.cnf; if the accelerator
// is unavailable (ISH_CRYPTO_ACCEL off, or self-test failed), the provider
// declines to register the algorithm so OpenSSL falls back to its default.
//
// Build (in-guest, against vendored OpenSSL core headers):
//   gcc -O2 -fPIC -shared -I<hdrs> -o ish.so ish_provider.c
//
// Ciphers offered:
//   ChaCha20            -- raw stream (what ssh's chacha20-poly1305@openssh.com
//                          uses); genuinely streaming, chains by block counter.
//   AES-256-GCM         -- whole AEAD, covering the three call sequences that
//                          matter: the generic/TLS-1.3 one, ssh's, and TLS
//                          1.2's. See the AES section below.
//   ChaCha20-Poly1305   -- whole AEAD (openssl speed, TLS records, one-shot
//                          libcrypto users); data via the raw-stream op, tag via
//                          the accelerator's MAC op -- shown bit-exact to OpenSSL.
//                          NOT registered; see the comment on ish_ciphers[].
//
// Scope: correct for the one-init + one-cipher-call pattern ssh uses, and for
// 64-byte-aligned streaming updates of the raw cipher. The AEAD tag covers the
// whole message and the accelerator's MAC op is one-shot, so the AEAD cipher is
// correct for a single data update (the dominant pattern); a second data update
// poisons the ctx so the caller errors rather than getting a wrong tag. Arbitrary
// sub-block-straddling / multi-chunk updates would need keystream+MAC buffering;
// not implemented -- such callers are rare and this provider simply must not be
// selected for them. Poisoning fails loudly; it never emits wrong output.
//
// The two accelerated algorithms are probed independently, because the kernel
// self-tests them independently: AES needs host AES/PMULL instructions that a
// build host can lack while ChaCha20 is fine, so neither is ever withheld on
// account of the other.

#include <openssl/core.h>
#include <openssl/core_dispatch.h>
#include <openssl/core_names.h>
#include <openssl/params.h>
#include <openssl/crypto.h>
#include <string.h>
#include <stdint.h>

extern long syscall(long, ...);
// Declared rather than pulled in from <unistd.h>, matching how this file gets
// at syscall(): musl and glibc agree on both prototypes, and the provider
// stays free of any libc header beyond <string.h>.
extern int getentropy(void *buf, size_t buflen);
#define ISH_SYS_AEAD 0xacc0
#define ISH_ALG_CHACHA20 1
#define ISH_ALG_AES256_GCM 2
// Answers "is this algorithm accelerated here?" without dereferencing any
// pointer in the request, so it is safe to ask with an all-zero one.
#define ISH_AEAD_OP_QUERY 4

struct ish_aead_req {
    uint32_t op, alg;
    uint64_t key, nonce, aad, aadlen, in, inlen, out, tag;
};

#define CC_KEYLEN 32
#define CC_IVLEN  16

struct cc_ctx {
    unsigned char key[CC_KEYLEN];
    unsigned char iv[CC_IVLEN];
    uint64_t offset;    // stream bytes processed so far
    int has_key;
    int has_partial;    // set if a non-64-aligned update happened (poison)
};

// Probe the accelerator once: encrypt 0 bytes. 0 => available.
static int accel_ok(void) {
    static int cached = -1;
    if (cached < 0) {
        unsigned char k[32] = {0}, iv[16] = {0};
        struct ish_aead_req r = {0};
        r.alg = ISH_ALG_CHACHA20;
        r.key = (uint64_t) (uintptr_t) k;
        r.nonce = (uint64_t) (uintptr_t) iv;
        cached = syscall(ISH_SYS_AEAD, &r) == 0 ? 1 : 0;
    }
    return cached;
}

// Run the accelerator over one buffer at the ctx's current stream offset.
// Returns 1 on success. Advances offset. Requires 64-alignment of offset.
static int accel_run(struct cc_ctx *c, unsigned char *out, const unsigned char *in, size_t len) {
    if (c->offset % 64 != 0) { c->has_partial = 1; return 0; }
    unsigned char iv[CC_IVLEN];
    memcpy(iv, c->iv, CC_IVLEN);
    // advance the 32-bit LE block counter by offset/64 (wraps like OpenSSL)
    uint32_t ctr;
    memcpy(&ctr, iv, 4);
    ctr += (uint32_t) (c->offset / 64);
    memcpy(iv, &ctr, 4);
    struct ish_aead_req r = {0};
    r.alg = ISH_ALG_CHACHA20;
    r.key = (uint64_t) (uintptr_t) c->key;
    r.nonce = (uint64_t) (uintptr_t) iv;
    r.in = (uint64_t) (uintptr_t) in;
    r.inlen = len;
    r.out = (uint64_t) (uintptr_t) out;
    if (syscall(ISH_SYS_AEAD, &r) != 0)
        return 0;
    c->offset += len;
    return 1;
}

static OSSL_FUNC_cipher_newctx_fn cc_newctx;
static OSSL_FUNC_cipher_freectx_fn cc_freectx;
static OSSL_FUNC_cipher_dupctx_fn cc_dupctx;
static OSSL_FUNC_cipher_encrypt_init_fn cc_einit;
static OSSL_FUNC_cipher_decrypt_init_fn cc_dinit;
static OSSL_FUNC_cipher_update_fn cc_update;
static OSSL_FUNC_cipher_final_fn cc_final;
static OSSL_FUNC_cipher_cipher_fn cc_cipher;
static OSSL_FUNC_cipher_get_params_fn cc_get_params;
static OSSL_FUNC_cipher_get_ctx_params_fn cc_get_ctx_params;
static OSSL_FUNC_cipher_set_ctx_params_fn cc_set_ctx_params;
static OSSL_FUNC_cipher_gettable_params_fn cc_gettable_params;
static OSSL_FUNC_cipher_gettable_ctx_params_fn cc_gettable_ctx_params;
static OSSL_FUNC_cipher_settable_ctx_params_fn cc_settable_ctx_params;

static void *cc_newctx(void *provctx) {
    (void) provctx;
    struct cc_ctx *c = OPENSSL_zalloc(sizeof(*c));
    return c;
}
static void cc_freectx(void *vctx) {
    struct cc_ctx *c = vctx;
    if (c) { OPENSSL_cleanse(c, sizeof(*c)); OPENSSL_free(c); }
}
static void *cc_dupctx(void *vctx) {
    struct cc_ctx *c = vctx, *d;
    if (c == NULL) return NULL;
    d = OPENSSL_malloc(sizeof(*d));
    if (d) memcpy(d, c, sizeof(*d));
    return d;
}

static int cc_init(struct cc_ctx *c, const unsigned char *key, size_t keylen,
        const unsigned char *iv, size_t ivlen) {
    if (key != NULL) {
        if (keylen != CC_KEYLEN) return 0;
        memcpy(c->key, key, CC_KEYLEN);
        c->has_key = 1;
    }
    if (iv != NULL) {
        if (ivlen != CC_IVLEN) return 0;
        memcpy(c->iv, iv, CC_IVLEN);
    }
    c->offset = 0;
    c->has_partial = 0;
    return 1;
}
static int cc_einit(void *vctx, const unsigned char *key, size_t keylen,
        const unsigned char *iv, size_t ivlen, const OSSL_PARAM params[]) {
    (void) params; return cc_init(vctx, key, keylen, iv, ivlen);
}
static int cc_dinit(void *vctx, const unsigned char *key, size_t keylen,
        const unsigned char *iv, size_t ivlen, const OSSL_PARAM params[]) {
    (void) params; return cc_init(vctx, key, keylen, iv, ivlen); // symmetric
}

static int cc_do(struct cc_ctx *c, unsigned char *out, size_t *outl,
        const unsigned char *in, size_t inl) {
    if (!c->has_key || c->has_partial) return 0;
    if (inl > 0 && !accel_run(c, out, in, inl)) return 0;
    *outl = inl;
    return 1;
}
// Streaming update: whole-block updates chain correctly; a non-final update
// whose length isn't a multiple of 64 poisons the ctx (accel_run refuses).
static int cc_update(void *vctx, unsigned char *out, size_t *outl, size_t outsize,
        const unsigned char *in, size_t inl) {
    (void) outsize; return cc_do(vctx, out, outl, in, inl);
}
static int cc_final(void *vctx, unsigned char *out, size_t *outl, size_t outsize) {
    (void) vctx; (void) out; (void) outsize; *outl = 0; return 1;
}
// One-shot EVP_Cipher entry (what ssh uses): offset is 0, always aligned.
static int cc_cipher(void *vctx, unsigned char *out, size_t *outl, size_t outsize,
        const unsigned char *in, size_t inl) {
    (void) outsize; return cc_do(vctx, out, outl, in, inl);
}

static int cc_get_params(OSSL_PARAM params[]) {
    OSSL_PARAM *p;
    if ((p = OSSL_PARAM_locate(params, OSSL_CIPHER_PARAM_BLOCK_SIZE)) &&
            !OSSL_PARAM_set_size_t(p, 1)) return 0;
    if ((p = OSSL_PARAM_locate(params, OSSL_CIPHER_PARAM_KEYLEN)) &&
            !OSSL_PARAM_set_size_t(p, CC_KEYLEN)) return 0;
    if ((p = OSSL_PARAM_locate(params, OSSL_CIPHER_PARAM_IVLEN)) &&
            !OSSL_PARAM_set_size_t(p, CC_IVLEN)) return 0;
    if ((p = OSSL_PARAM_locate(params, OSSL_CIPHER_PARAM_MODE)) &&
            !OSSL_PARAM_set_uint(p, 0 /* EVP_CIPH_STREAM_CIPHER */)) return 0;
    return 1;
}
static int cc_get_ctx_params(void *vctx, OSSL_PARAM params[]) {
    (void) vctx; OSSL_PARAM *p;
    if ((p = OSSL_PARAM_locate(params, OSSL_CIPHER_PARAM_KEYLEN)) &&
            !OSSL_PARAM_set_size_t(p, CC_KEYLEN)) return 0;
    if ((p = OSSL_PARAM_locate(params, OSSL_CIPHER_PARAM_IVLEN)) &&
            !OSSL_PARAM_set_size_t(p, CC_IVLEN)) return 0;
    return 1;
}
static int cc_set_ctx_params(void *vctx, const OSSL_PARAM params[]) {
    (void) vctx; (void) params; return 1;
}
static const OSSL_PARAM cc_known_params[] = {
    OSSL_PARAM_size_t(OSSL_CIPHER_PARAM_BLOCK_SIZE, NULL),
    OSSL_PARAM_size_t(OSSL_CIPHER_PARAM_KEYLEN, NULL),
    OSSL_PARAM_size_t(OSSL_CIPHER_PARAM_IVLEN, NULL),
    OSSL_PARAM_uint(OSSL_CIPHER_PARAM_MODE, NULL),
    OSSL_PARAM_END
};
static const OSSL_PARAM *cc_gettable_params(void *p) { (void) p; return cc_known_params; }
static const OSSL_PARAM *cc_gettable_ctx_params(void *c, void *p) { (void) c; (void) p; return cc_known_params; }
static const OSSL_PARAM *cc_settable_ctx_params(void *c, void *p) { (void) c; (void) p; return cc_known_params; }

static const OSSL_DISPATCH cc_functions[] = {
    { OSSL_FUNC_CIPHER_NEWCTX, (void (*)(void)) cc_newctx },
    { OSSL_FUNC_CIPHER_FREECTX, (void (*)(void)) cc_freectx },
    { OSSL_FUNC_CIPHER_DUPCTX, (void (*)(void)) cc_dupctx },
    { OSSL_FUNC_CIPHER_ENCRYPT_INIT, (void (*)(void)) cc_einit },
    { OSSL_FUNC_CIPHER_DECRYPT_INIT, (void (*)(void)) cc_dinit },
    { OSSL_FUNC_CIPHER_UPDATE, (void (*)(void)) cc_update },
    { OSSL_FUNC_CIPHER_FINAL, (void (*)(void)) cc_final },
    { OSSL_FUNC_CIPHER_CIPHER, (void (*)(void)) cc_cipher },
    { OSSL_FUNC_CIPHER_GET_PARAMS, (void (*)(void)) cc_get_params },
    { OSSL_FUNC_CIPHER_GET_CTX_PARAMS, (void (*)(void)) cc_get_ctx_params },
    { OSSL_FUNC_CIPHER_SET_CTX_PARAMS, (void (*)(void)) cc_set_ctx_params },
    { OSSL_FUNC_CIPHER_GETTABLE_PARAMS, (void (*)(void)) cc_gettable_params },
    { OSSL_FUNC_CIPHER_GETTABLE_CTX_PARAMS, (void (*)(void)) cc_gettable_ctx_params },
    { OSSL_FUNC_CIPHER_SETTABLE_CTX_PARAMS, (void (*)(void)) cc_settable_ctx_params },
    { 0, NULL }
};

// ===== ChaCha20-Poly1305 AEAD ==========================================
// Ciphers the data via the accelerator's raw ChaCha20 stream (counter 1) and
// gets/verifies the Poly1305 tag via the MAC op -- the split shown bit-exact
// to OpenSSL. Correct for the common one-init + one-data-update pattern
// (openssl speed, TLS records, most apps); a second data update poisons the
// ctx so the caller errors rather than getting a wrong tag.
#define CP_IVLEN 12
#define CP_TAGLEN 16
#define CP_ALG_AEAD 0

struct cp_ctx {
    unsigned char key[CC_KEYLEN];
    unsigned char nonce[CP_IVLEN];
    unsigned char aad[512];
    size_t aadlen;
    unsigned char tag[CP_TAGLEN];
    uint64_t dec_ct;        // guest addr of the ciphertext (decrypt, for final MAC)
    size_t dec_ctlen;
    int enc, has_key, has_data, poison;
};

static int cp_syscall(uint32_t op, const struct cp_ctx *c, uint64_t in, uint64_t inlen,
        uint64_t out, uint64_t tag) {
    unsigned char iv16[16] = {1,0,0,0};              // AEAD data counter = 1
    memcpy(iv16 + 4, c->nonce, CP_IVLEN);
    struct ish_aead_req r = {0};
    r.op = op; r.alg = (op == 2) ? ISH_ALG_CHACHA20 : CP_ALG_AEAD;
    r.key = (uint64_t) (uintptr_t) c->key;
    r.nonce = (op == 2) ? (uint64_t) (uintptr_t) iv16 : (uint64_t) (uintptr_t) c->nonce;
    r.aad = (uint64_t) (uintptr_t) c->aad; r.aadlen = c->aadlen;
    r.in = in; r.inlen = inlen; r.out = out; r.tag = tag;
    return syscall(ISH_SYS_AEAD, &r);
}

static void *cp_newctx(void *p) { (void) p; return OPENSSL_zalloc(sizeof(struct cp_ctx)); }
static void cp_freectx(void *v) { struct cp_ctx *c = v; if (c) { OPENSSL_cleanse(c, sizeof(*c)); OPENSSL_free(c); } }
static void *cp_dupctx(void *v) { struct cp_ctx *c = v, *d; if (!c) return NULL; d = OPENSSL_malloc(sizeof(*d)); if (d) memcpy(d, c, sizeof(*d)); return d; }

static int cp_init(struct cp_ctx *c, const unsigned char *key, size_t keylen,
        const unsigned char *iv, size_t ivlen, int enc) {
    c->enc = enc;
    if (key) { if (keylen != CC_KEYLEN) return 0; memcpy(c->key, key, CC_KEYLEN); c->has_key = 1; }
    if (iv)  { if (ivlen != CP_IVLEN) return 0; memcpy(c->nonce, iv, CP_IVLEN); }
    c->aadlen = 0; c->has_data = 0; c->poison = 0; c->dec_ctlen = 0;
    return 1;
}
static int cp_einit(void *v, const unsigned char *k, size_t kl, const unsigned char *iv, size_t il, const OSSL_PARAM p[]) { (void) p; return cp_init(v, k, kl, iv, il, 1); }
static int cp_dinit(void *v, const unsigned char *k, size_t kl, const unsigned char *iv, size_t il, const OSSL_PARAM p[]) { (void) p; return cp_init(v, k, kl, iv, il, 0); }

static int cp_update(void *vctx, unsigned char *out, size_t *outl, size_t outsize,
        const unsigned char *in, size_t inl) {
    (void) outsize;
    struct cp_ctx *c = vctx;
    if (!c->has_key || c->poison) return 0;
    if (out == NULL) {                    // AAD
        if (c->aadlen + inl > sizeof(c->aad)) { c->poison = 1; return 0; }
        if (inl) memcpy(c->aad + c->aadlen, in, inl);
        c->aadlen += inl; *outl = inl; return 1;
    }
    if (inl == 0) { *outl = 0; return 1; } // no data yet; tag handled at final
    if (c->has_data) { c->poison = 1; return 0; }   // one data update only
    c->has_data = 1;
    if (c->enc) {
        // seal: ct -> out, tag -> ctx (one-shot over the whole plaintext)
        if (cp_syscall(0, c, (uint64_t)(uintptr_t) in, inl,
                (uint64_t)(uintptr_t) out, (uint64_t)(uintptr_t) c->tag) != 0) return 0;
    } else {
        // decrypt: stream ct -> pt (raw op); MAC verified at final over the ct
        if (cp_syscall(2, c, (uint64_t)(uintptr_t) in, inl,
                (uint64_t)(uintptr_t) out, 0) != 0) return 0;
        c->dec_ct = (uint64_t)(uintptr_t) in; c->dec_ctlen = inl;
    }
    *outl = inl; return 1;
}

static int cp_final(void *vctx, unsigned char *out, size_t *outl, size_t outsize) {
    (void) out; (void) outsize;
    struct cp_ctx *c = vctx; *outl = 0;
    if (c->poison) return 0;
    if (c->enc) {
        // Empty-plaintext seal never ran in update: compute the AAD-only tag now.
        if (!c->has_data && cp_syscall(0, c, 0, 0, 0, (uint64_t)(uintptr_t) c->tag) != 0)
            return 0;
        return 1;
    }
    // Decrypt: MAC over the (possibly empty) ciphertext, compare to the set tag.
    unsigned char got[CP_TAGLEN];
    if (cp_syscall(3, c, c->dec_ct, c->dec_ctlen, 0, (uint64_t)(uintptr_t) got) != 0) return 0;
    unsigned char d = 0; for (int i = 0; i < CP_TAGLEN; i++) d |= got[i] ^ c->tag[i];
    return d == 0;                   // EVP_DecryptFinal fails if tag mismatch
}

static int cp_get_params(OSSL_PARAM params[]) {
    OSSL_PARAM *p;
    if ((p = OSSL_PARAM_locate(params, OSSL_CIPHER_PARAM_BLOCK_SIZE)) && !OSSL_PARAM_set_size_t(p, 1)) return 0;
    if ((p = OSSL_PARAM_locate(params, OSSL_CIPHER_PARAM_KEYLEN)) && !OSSL_PARAM_set_size_t(p, CC_KEYLEN)) return 0;
    if ((p = OSSL_PARAM_locate(params, OSSL_CIPHER_PARAM_IVLEN)) && !OSSL_PARAM_set_size_t(p, CP_IVLEN)) return 0;
    if ((p = OSSL_PARAM_locate(params, OSSL_CIPHER_PARAM_AEAD)) && !OSSL_PARAM_set_int(p, 1)) return 0;
    if ((p = OSSL_PARAM_locate(params, OSSL_CIPHER_PARAM_CUSTOM_IV)) && !OSSL_PARAM_set_int(p, 1)) return 0;
    return 1;
}
static int cp_get_ctx_params(void *vctx, OSSL_PARAM params[]) {
    struct cp_ctx *c = vctx; OSSL_PARAM *p;
    if ((p = OSSL_PARAM_locate(params, OSSL_CIPHER_PARAM_KEYLEN)) && !OSSL_PARAM_set_size_t(p, CC_KEYLEN)) return 0;
    if ((p = OSSL_PARAM_locate(params, OSSL_CIPHER_PARAM_IVLEN)) && !OSSL_PARAM_set_size_t(p, CP_IVLEN)) return 0;
    if ((p = OSSL_PARAM_locate(params, OSSL_CIPHER_PARAM_AEAD_TAGLEN)) && !OSSL_PARAM_set_size_t(p, CP_TAGLEN)) return 0;
    p = OSSL_PARAM_locate(params, OSSL_CIPHER_PARAM_AEAD_TAG);
    if (p != NULL) { // encrypt: hand back the computed tag
        if (p->data_size != CP_TAGLEN || !OSSL_PARAM_set_octet_string(p, c->tag, CP_TAGLEN)) return 0;
    }
    return 1;
}
static int cp_set_ctx_params(void *vctx, const OSSL_PARAM params[]) {
    struct cp_ctx *c = vctx; const OSSL_PARAM *p;
    if ((p = OSSL_PARAM_locate_const(params, OSSL_CIPHER_PARAM_AEAD_TAG)) != NULL) {
        if (p->data_size != CP_TAGLEN || p->data_type != OSSL_PARAM_OCTET_STRING) return 0;
        memcpy(c->tag, p->data, CP_TAGLEN); // decrypt: expected tag
    }
    return 1;
}
static const OSSL_PARAM cp_gettable[] = {
    OSSL_PARAM_size_t(OSSL_CIPHER_PARAM_BLOCK_SIZE, NULL),
    OSSL_PARAM_size_t(OSSL_CIPHER_PARAM_KEYLEN, NULL),
    OSSL_PARAM_size_t(OSSL_CIPHER_PARAM_IVLEN, NULL),
    OSSL_PARAM_int(OSSL_CIPHER_PARAM_AEAD, NULL),
    OSSL_PARAM_END
};
static const OSSL_PARAM cp_gettable_ctx[] = {
    OSSL_PARAM_size_t(OSSL_CIPHER_PARAM_KEYLEN, NULL),
    OSSL_PARAM_size_t(OSSL_CIPHER_PARAM_IVLEN, NULL),
    OSSL_PARAM_size_t(OSSL_CIPHER_PARAM_AEAD_TAGLEN, NULL),
    OSSL_PARAM_octet_string(OSSL_CIPHER_PARAM_AEAD_TAG, NULL, 0),
    OSSL_PARAM_END
};
static const OSSL_PARAM cp_settable_ctx[] = {
    OSSL_PARAM_octet_string(OSSL_CIPHER_PARAM_AEAD_TAG, NULL, 0),
    OSSL_PARAM_END
};
static const OSSL_PARAM *cp_gettable_params(void *p) { (void) p; return cp_gettable; }
static const OSSL_PARAM *cp_gettable_ctx_params(void *c, void *p) { (void) c; (void) p; return cp_gettable_ctx; }
static const OSSL_PARAM *cp_settable_ctx_params(void *c, void *p) { (void) c; (void) p; return cp_settable_ctx; }

// Kept compiled (so it cannot rot) but unreferenced unless the AEAD is
// re-registered above; see the comment on ish_ciphers[].
__attribute__((unused))
static const OSSL_DISPATCH cp_functions[] = {
    { OSSL_FUNC_CIPHER_NEWCTX, (void (*)(void)) cp_newctx },
    { OSSL_FUNC_CIPHER_FREECTX, (void (*)(void)) cp_freectx },
    { OSSL_FUNC_CIPHER_DUPCTX, (void (*)(void)) cp_dupctx },
    { OSSL_FUNC_CIPHER_ENCRYPT_INIT, (void (*)(void)) cp_einit },
    { OSSL_FUNC_CIPHER_DECRYPT_INIT, (void (*)(void)) cp_dinit },
    { OSSL_FUNC_CIPHER_UPDATE, (void (*)(void)) cp_update },
    { OSSL_FUNC_CIPHER_FINAL, (void (*)(void)) cp_final },
    { OSSL_FUNC_CIPHER_GET_PARAMS, (void (*)(void)) cp_get_params },
    { OSSL_FUNC_CIPHER_GET_CTX_PARAMS, (void (*)(void)) cp_get_ctx_params },
    { OSSL_FUNC_CIPHER_SET_CTX_PARAMS, (void (*)(void)) cp_set_ctx_params },
    { OSSL_FUNC_CIPHER_GETTABLE_PARAMS, (void (*)(void)) cp_gettable_params },
    { OSSL_FUNC_CIPHER_GETTABLE_CTX_PARAMS, (void (*)(void)) cp_gettable_ctx_params },
    { OSSL_FUNC_CIPHER_SETTABLE_CTX_PARAMS, (void (*)(void)) cp_settable_ctx_params },
    { 0, NULL }
};

// ===== AES-256-GCM =====================================================
// The accelerator's AES op (alg 2) is one-shot: key, nonce, AAD, the whole
// message and the tag in a single syscall. That is not what an EVP cipher
// looks like from the outside, so this code's job is to recognise each of the
// call sequences a real consumer uses and fold it onto that one call. Three
// sequences matter, and a system-wide install needs all three, because a
// provider is chosen by property and CANNOT fall back to the default once
// chosen -- registering "AES-256-GCM" routes every AES-GCM consumer on the
// system, all of https included, through here.
//
//  1. Generic / TLS 1.3 (ssl/record/methods/tls13_meth.c). Per record:
//     EVP_CipherInit_ex(ctx, NULL, NULL, NULL, iv, enc) re-inits with the IV
//     only; on decrypt EVP_CTRL_AEAD_SET_TAG follows; then CipherUpdate(NULL,
//     aad), one data CipherUpdate, CipherFinal, and GET_TAG when sending.
//  2. ssh (openssh cipher.c). EVP_CTRL_GCM_SET_IV_FIXED once at init, then per
//     packet EVP_CTRL_GCM_IV_GEN, SET_TAG before the data on decrypt, and AAD
//     /data/final all through EVP_Cipher -- the ccipher entry, not update.
//  3. TLS 1.2 (ssl/record/methods/tls1_meth.c). EVP_CTRL_AEAD_TLS1_AAD hands
//     over the 13-byte header and takes back the tag length as a pad value,
//     then a SINGLE CipherUpdate covers explicit-IV||ciphertext||tag in place.
//     Mandatory: failing that ctrl makes the record layer error out, and TLS
//     1.2 https dies.
//
// Every consumer sets the tag before the data on decrypt, which is what lets
// the one-shot op work at all. Every pattern NOT recognised here is a hard
// error -- never plausible-looking bytes. In particular a second data update
// within one record poisons the ctx, as do an AAD or a message larger than the
// syscall's caps, since the accelerator cannot resume GCM state across calls.
#define AG_KEYLEN 32
#define AG_IVLEN  12                 // 96-bit nonces only; see ag_set_ctx_params
#define AG_TAGLEN 16
#define AG_MAX_AAD 4096u             // kernel ISH_AEAD_MAX_AAD
#define AG_MAX_IN  (4u << 20)        // kernel ISH_AEAD_MAX_IN
#define AG_TLS_AAD_LEN 13            // EVP_AEAD_TLS1_AAD_LEN
#define AG_TLS_EXPLICIT_IV_LEN 8     // EVP_GCM_TLS_EXPLICIT_IV_LEN
#define AG_TLS_FIXED_IV_LEN 4        // EVP_GCM_TLS_FIXED_IV_LEN

struct ag_ctx {
    // Connection lifetime: survives the per-record reset. A re-init with a
    // NULL key must keep the key (TLS 1.3 re-inits per record with the IV
    // alone), and the IV must keep counting across packets (ssh installs it
    // once and never re-inits).
    unsigned char key[AG_KEYLEN];
    unsigned char iv[AG_IVLEN];
    int enc, key_set, iv_set, iv_gen, iv_gen_rand;

    // Record lifetime: cleared at every reset point.
    unsigned char aad[AG_MAX_AAD];
    size_t aadlen;
    unsigned char rec_iv[AG_IVLEN];  // this record's nonce, when it differs
    unsigned char tag[AG_TAGLEN];
    int rec_iv_set, tag_set, tag_ready, has_data, auth_failed, iv_finished;

    // TLS 1.2 latch: armed by the tlsaad param, consumed by the next data call.
    unsigned char tls_aad[AG_TLS_AAD_LEN];
    int tls_aad_set;

    // Sticky: an unsupported call pattern was seen. Only a full re-init clears
    // it, so a caller that strays cannot drift back into producing output.
    int poison;
};

// Run one AES-256-GCM operation. op 0 = seal, 1 = open. Returns 1 on success.
// The sizes are pre-checked so the only realistic failure of an open is the
// kernel rejecting the tag (it scrubs the output buffer itself in that case).
static int ag_run(struct ag_ctx *c, int op, const unsigned char *nonce,
        const unsigned char *aad, size_t aadlen,
        const unsigned char *in, size_t inlen, unsigned char *out,
        unsigned char *tag) {
    if (inlen > AG_MAX_IN || aadlen > AG_MAX_AAD)
        return 0;
    struct ish_aead_req r = {0};
    r.op = (uint32_t) op;
    r.alg = ISH_ALG_AES256_GCM;
    r.key = (uint64_t) (uintptr_t) c->key;
    r.nonce = (uint64_t) (uintptr_t) nonce;
    r.aad = (uint64_t) (uintptr_t) aad;
    r.aadlen = aadlen;
    r.in = (uint64_t) (uintptr_t) in;
    r.inlen = inlen;
    r.out = (uint64_t) (uintptr_t) out;
    r.tag = (uint64_t) (uintptr_t) tag;
    return syscall(ISH_SYS_AEAD, &r) == 0;
}

// Probe AES separately from ChaCha20: the kernel self-tests them separately.
static int aes_accel_ok(void) {
    static int cached = -1;
    if (cached < 0) {
        struct ish_aead_req r = {0};
        r.op = ISH_AEAD_OP_QUERY;
        r.alg = ISH_ALG_AES256_GCM;   // every pointer left NULL on purpose
        cached = syscall(ISH_SYS_AEAD, &r) == 0 ? 1 : 0;
    }
    return cached;
}

// Increment the 64-bit invocation field, as OpenSSL's ctr64_inc does.
static void ag_ctr64_inc(unsigned char *counter) {
    for (int n = 7; n >= 0; n--)
        if (++counter[n] != 0)
            return;
}

// Start of a new record. All three reset points land here: an init, the
// tlsivgen get (ssh's per-packet IV bump) and the tlsaad set (TLS 1.2's
// per-record AAD). Missing any one of them would make that consumer's SECOND
// record hit the one-data-update poison, since ssh and TLS 1.2 never re-init.
// `poison` survives on purpose -- only ag_init clears it.
static void ag_new_record(struct ag_ctx *c) {
    c->aadlen = 0;
    c->rec_iv_set = 0;
    c->tag_set = 0;
    c->tag_ready = 0;
    c->has_data = 0;
    c->auth_failed = 0;
    c->iv_finished = 0;
    c->tls_aad_set = 0;
}

// The nonce this record actually uses: ssh and TLS 1.2 pin one per record
// while c->iv runs ahead to the next, so they cannot be the same buffer.
static const unsigned char *ag_nonce(const struct ag_ctx *c) {
    return c->rec_iv_set ? c->rec_iv : c->iv;
}

static void ag_pin_nonce(struct ag_ctx *c) {
    memcpy(c->rec_iv, c->iv, AG_IVLEN);
    c->rec_iv_set = 1;
}

static OSSL_FUNC_cipher_set_ctx_params_fn ag_set_ctx_params;

static void *ag_newctx(void *p) { (void) p; return OPENSSL_zalloc(sizeof(struct ag_ctx)); }
static void ag_freectx(void *v) {
    struct ag_ctx *c = v;
    if (c) { OPENSSL_cleanse(c, sizeof(*c)); OPENSSL_free(c); }
}
static void *ag_dupctx(void *v) {
    struct ag_ctx *c = v, *d;
    if (c == NULL) return NULL;
    d = OPENSSL_malloc(sizeof(*d));
    if (d) memcpy(d, c, sizeof(*d));
    return d;
}

static int ag_init(struct ag_ctx *c, const unsigned char *key, size_t keylen,
        const unsigned char *iv, size_t ivlen, int enc, const OSSL_PARAM params[]) {
    c->enc = enc;
    if (key != NULL) {
        if (keylen != AG_KEYLEN) return 0;
        memcpy(c->key, key, AG_KEYLEN);
        c->key_set = 1;
    }
    if (iv != NULL) {
        if (ivlen != AG_IVLEN) return 0;
        memcpy(c->iv, iv, AG_IVLEN);
        c->iv_set = 1;
        c->iv_gen_rand = 0;
    }
    ag_new_record(c);
    c->poison = 0;
    return ag_set_ctx_params(c, params);
}
static int ag_einit(void *v, const unsigned char *k, size_t kl,
        const unsigned char *iv, size_t il, const OSSL_PARAM p[]) {
    return ag_init(v, k, kl, iv, il, 1, p);
}
static int ag_dinit(void *v, const unsigned char *k, size_t kl,
        const unsigned char *iv, size_t il, const OSSL_PARAM p[]) {
    return ag_init(v, k, kl, iv, il, 0, p);
}

// EVP_CTRL_GCM_SET_IV_FIXED. Two callers, two shapes: ssh passes -1 to install
// a whole 12-byte IV it derived itself, and TLS 1.2 passes only the 4-byte
// fixed half, expecting the 8-byte invocation half to be generated here (it
// then travels in the clear at the front of every record).
static int ag_tls_iv_fixed(struct ag_ctx *c, const unsigned char *iv, size_t len) {
    if (len == (size_t) -1) {           // whole IV -- must be tested before any
        memcpy(c->iv, iv, AG_IVLEN);    // bounds check; it arrives as SIZE_MAX
        c->iv_gen = 1;
        c->iv_set = 1;
        return 1;
    }
    // With a 12-byte IV and an 8-byte invocation field the fixed half is
    // exactly 4 bytes; anything else would leave the two overlapping.
    if (len != AG_IVLEN - AG_TLS_EXPLICIT_IV_LEN)
        return 0;
    memcpy(c->iv, iv, len);
    if (c->enc) {
        // The invocation field must be unpredictable, and OpenSSL draws it
        // from its own DRBG here. A provider has no libctx RAND, so take it
        // from the kernel; failing loudly beats a guessable IV.
        if (getentropy(c->iv + len, AG_IVLEN - len) != 0)
            return 0;
        c->iv_gen_rand = 1;
    }
    c->iv_gen = 1;
    c->iv_set = 1;
    return 1;
}

// EVP_CTRL_GCM_IV_GEN. ssh calls this once per packet, in BOTH directions, and
// never re-inits -- so it is a record reset point. The IV handed back is the
// CURRENT one, which is also the one this packet is ciphered with; the stored
// copy is bumped afterwards, ready for the next packet.
static int ag_ivgen(struct ag_ctx *c, unsigned char *out, size_t olen) {
    if (!c->iv_gen || !c->key_set || out == NULL)
        return 0;
    ag_new_record(c);
    ag_pin_nonce(c);
    if (olen == 0 || olen > AG_IVLEN)
        olen = AG_IVLEN;
    memcpy(out, c->iv + AG_IVLEN - olen, olen);
    ag_ctr64_inc(c->iv + AG_IVLEN - 8);
    return 1;
}

// EVP_CTRL_GCM_SET_IV_INV: the decrypt-side counterpart, taking the invocation
// field from the wire instead of generating it. Unused by the three sequences
// above (TLS 1.2 decrypt reaches the same code inside ag_tls_cipher) but part
// of the GCM contract.
static int ag_set_iv_inv(struct ag_ctx *c, const unsigned char *in, size_t inl) {
    if (!c->iv_gen || !c->key_set || c->enc)
        return 0;
    if (inl == 0 || inl > AG_IVLEN)
        return 0;
    ag_new_record(c);
    memcpy(c->iv + AG_IVLEN - inl, in, inl);
    ag_pin_nonce(c);
    return 1;
}

// EVP_CTRL_AEAD_TLS1_AAD. TLS 1.2's record layer hands over the 13-byte
// sequence||type||version||length header and expects the tag length back as
// the padding it must account for. Rewriting the length field down to the
// payload length is what makes this AAD match what the peer authenticated;
// OpenSSL's gcm_tls_init does the same arithmetic.
static int ag_tls_aad(struct ag_ctx *c, const unsigned char *aad, size_t aadlen) {
    if (aad == NULL || aadlen != AG_TLS_AAD_LEN)
        return 0;
    ag_new_record(c);
    memcpy(c->tls_aad, aad, AG_TLS_AAD_LEN);
    size_t len = ((size_t) c->tls_aad[AG_TLS_AAD_LEN - 2] << 8) |
            c->tls_aad[AG_TLS_AAD_LEN - 1];
    if (len < AG_TLS_EXPLICIT_IV_LEN)
        return 0;
    len -= AG_TLS_EXPLICIT_IV_LEN;
    if (!c->enc) {                      // decrypting: the tag is in there too
        if (len < AG_TAGLEN)
            return 0;
        len -= AG_TAGLEN;
    }
    c->tls_aad[AG_TLS_AAD_LEN - 2] = (unsigned char) (len >> 8);
    c->tls_aad[AG_TLS_AAD_LEN - 1] = (unsigned char) (len & 0xff);
    c->tls_aad_set = 1;
    return 1;
}

// The whole TLS 1.2 record in one call: explicit IV, then payload, then tag,
// ciphered in place. There is no Final on this path, so an authentication
// failure has to be reported from right here.
static int ag_tls_cipher(struct ag_ctx *c, unsigned char *out, size_t *outl,
        const unsigned char *in, size_t inl) {
    unsigned char nonce[AG_IVLEN];
    c->tls_aad_set = 0;                 // consumed; the next record re-arms it
    if (!c->key_set || !c->iv_gen)
        return 0;
    if (out != in || inl < AG_TLS_EXPLICIT_IV_LEN + AG_TAGLEN)
        return 0;

    if (c->enc) {
        memcpy(nonce, c->iv, AG_IVLEN);
        memcpy(out, c->iv + AG_IVLEN - AG_TLS_EXPLICIT_IV_LEN,
                AG_TLS_EXPLICIT_IV_LEN);
        ag_ctr64_inc(c->iv + AG_IVLEN - 8);
    } else {
        memcpy(c->iv + AG_IVLEN - AG_TLS_EXPLICIT_IV_LEN, in,
                AG_TLS_EXPLICIT_IV_LEN);
        memcpy(nonce, c->iv, AG_IVLEN);
    }

    const unsigned char *pin = in + AG_TLS_EXPLICIT_IV_LEN;
    unsigned char *pout = out + AG_TLS_EXPLICIT_IV_LEN;
    size_t len = inl - AG_TLS_EXPLICIT_IV_LEN - AG_TAGLEN;
    unsigned char *tag = c->enc ? pout + len : (unsigned char *) pin + len;
    if (!ag_run(c, c->enc ? 0 : 1, nonce, c->tls_aad, AG_TLS_AAD_LEN,
                pin, len, pout, tag))
        return 0;
    // Sending, the record grew by the explicit IV and the tag; receiving, the
    // caller wants the payload length and skips the explicit IV itself.
    *outl = c->enc ? inl : len;
    return 1;
}

// The single point every data-bearing entry funnels through, shaped like
// OpenSSL's gcm_cipher_internal: out == NULL is AAD, in == NULL is final, and
// anything else is the message.
static int ag_do(struct ag_ctx *c, unsigned char *out, size_t *outl,
        const unsigned char *in, size_t inl) {
    *outl = 0;
    if (c->poison || !c->key_set)
        return 0;
    if (c->tls_aad_set)
        return ag_tls_cipher(c, out, outl, in, inl);

    if (in != NULL && out == NULL) {                    // AAD
        if (c->has_data) { c->poison = 1; return 0; }   // all AAD precedes data
        if (inl > AG_MAX_AAD - c->aadlen) { c->poison = 1; return 0; }
        if (inl) memcpy(c->aad + c->aadlen, in, inl);
        c->aadlen += inl;
        *outl = inl;
        return 1;
    }

    if (in != NULL) {                                   // message
        if (c->has_data) { c->poison = 1; return 0; }   // one update per record
        if (inl > AG_MAX_IN) { c->poison = 1; return 0; }
        if (c->iv_finished) return 0;                   // never reuse an IV
        if (!c->iv_set) {
            // No IV at all: OpenSSL generates one on the encrypt side rather
            // than fail, and some callers rely on that.
            if (!c->enc || getentropy(c->iv, AG_IVLEN) != 0)
                return 0;
            c->iv_set = 1;
            c->iv_gen_rand = 1;
        }
        c->has_data = 1;
        if (c->enc) {
            if (!ag_run(c, 0, ag_nonce(c), c->aad, c->aadlen, in, inl, out, c->tag))
                return 0;
        } else {
            if (!c->tag_set) return 0;      // the tag always precedes the data
            if (!ag_run(c, 1, ag_nonce(c), c->aad, c->aadlen, in, inl, out, c->tag))
                c->auth_failed = 1;         // the kernel already scrubbed `out`
        }
        *outl = inl;
        return 1;
    }

    // in == NULL: EVP Final, or ssh's EVP_Cipher(ctx, NULL, NULL, 0).
    if (c->iv_finished)
        return 0;
    if (!c->iv_set)
        return 0;
    if (c->enc) {
        // A seal with no message at all still owes a tag over the AAD.
        if (!c->has_data &&
                !ag_run(c, 0, ag_nonce(c), c->aad, c->aadlen, NULL, 0, NULL, c->tag))
            return 0;
        c->tag_ready = 1;
        c->iv_finished = 1;
        return 1;
    }
    if (!c->tag_set)
        return 0;
    // Likewise an open of an empty ciphertext: the tag still has to verify.
    if (!c->has_data &&
            !ag_run(c, 1, ag_nonce(c), c->aad, c->aadlen, NULL, 0, NULL, c->tag))
        return 0;
    c->iv_finished = 1;
    return !c->auth_failed;
}

// Streaming entries. A zero-length update is a no-op rather than a final,
// which is the split EVP relies on: final arrives only via cfinal.
static int ag_update(void *vctx, unsigned char *out, size_t *outl, size_t outsize,
        const unsigned char *in, size_t inl) {
    struct ag_ctx *c = vctx;
    if (inl == 0) { *outl = 0; return 1; }
    if (out != NULL && outsize < inl) return 0;
    return ag_do(c, out, outl, in, inl);
}
static int ag_final(void *vctx, unsigned char *out, size_t *outl, size_t outsize) {
    (void) out; (void) outsize;
    return ag_do(vctx, NULL, outl, NULL, 0);
}
// One-shot EVP_Cipher entry. ssh drives everything through here, including its
// AAD (out == NULL) and its authentication step (in == NULL).
static int ag_cipher(void *vctx, unsigned char *out, size_t *outl, size_t outsize,
        const unsigned char *in, size_t inl) {
    if (out != NULL && in != NULL && outsize < inl) return 0;
    return ag_do(vctx, out, outl, in, inl);
}

static int ag_get_params(OSSL_PARAM params[]) {
    OSSL_PARAM *p;
    if ((p = OSSL_PARAM_locate(params, OSSL_CIPHER_PARAM_BLOCK_SIZE)) &&
            !OSSL_PARAM_set_size_t(p, 1)) return 0;
    if ((p = OSSL_PARAM_locate(params, OSSL_CIPHER_PARAM_KEYLEN)) &&
            !OSSL_PARAM_set_size_t(p, AG_KEYLEN)) return 0;
    if ((p = OSSL_PARAM_locate(params, OSSL_CIPHER_PARAM_IVLEN)) &&
            !OSSL_PARAM_set_size_t(p, AG_IVLEN)) return 0;
    // Not decoration. `aead` is what puts the TLS record layer on its AEAD
    // path at all (and openssl speed on its AEAD loop), and `mode` is what
    // drives the record layer's explicit-IV arithmetic. Either one wrong and
    // every record is framed wrongly while looking perfectly well-formed.
    if ((p = OSSL_PARAM_locate(params, OSSL_CIPHER_PARAM_MODE)) &&
            !OSSL_PARAM_set_uint(p, 6 /* EVP_CIPH_GCM_MODE */)) return 0;
    if ((p = OSSL_PARAM_locate(params, OSSL_CIPHER_PARAM_AEAD)) &&
            !OSSL_PARAM_set_int(p, 1)) return 0;
    if ((p = OSSL_PARAM_locate(params, OSSL_CIPHER_PARAM_CUSTOM_IV)) &&
            !OSSL_PARAM_set_int(p, 1)) return 0;
    return 1;
}

static int ag_get_ctx_params(void *vctx, OSSL_PARAM params[]) {
    struct ag_ctx *c = vctx;
    OSSL_PARAM *p;
    if ((p = OSSL_PARAM_locate(params, OSSL_CIPHER_PARAM_KEYLEN)) &&
            !OSSL_PARAM_set_size_t(p, AG_KEYLEN)) return 0;
    if ((p = OSSL_PARAM_locate(params, OSSL_CIPHER_PARAM_IVLEN)) &&
            !OSSL_PARAM_set_size_t(p, AG_IVLEN)) return 0;
    if ((p = OSSL_PARAM_locate(params, OSSL_CIPHER_PARAM_AEAD_TAGLEN)) &&
            !OSSL_PARAM_set_size_t(p, AG_TAGLEN)) return 0;
    if ((p = OSSL_PARAM_locate(params, OSSL_CIPHER_PARAM_IV)) != NULL) {
        if (!c->iv_set || !OSSL_PARAM_set_octet_string(p, c->iv, AG_IVLEN)) return 0;
    }
    if ((p = OSSL_PARAM_locate(params, OSSL_CIPHER_PARAM_UPDATED_IV)) != NULL) {
        if (!c->iv_set || !OSSL_PARAM_set_octet_string(p, c->iv, AG_IVLEN)) return 0;
    }
    // The pad value TLS 1.2 reads back straight after arming the AAD. Zero
    // when nothing armed it, which the record layer treats as a failure.
    if ((p = OSSL_PARAM_locate(params, OSSL_CIPHER_PARAM_AEAD_TLS1_AAD_PAD)) &&
            !OSSL_PARAM_set_size_t(p, c->tls_aad_set ? (size_t) AG_TAGLEN : 0)) return 0;
    if ((p = OSSL_PARAM_locate(params, OSSL_CIPHER_PARAM_AEAD_TAG)) != NULL) {
        // Encrypt only, and only once the tag exists -- i.e. after Final.
        if (p->data_size == 0 || p->data_size > AG_TAGLEN || !c->enc || !c->tag_ready)
            return 0;
        if (!OSSL_PARAM_set_octet_string(p, c->tag, p->data_size)) return 0;
    }
    if ((p = OSSL_PARAM_locate(params, OSSL_CIPHER_PARAM_AEAD_TLS1_GET_IV_GEN)) != NULL) {
        if (p->data_type != OSSL_PARAM_OCTET_STRING ||
                !ag_ivgen(c, p->data, p->data_size)) return 0;
    }
    if ((p = OSSL_PARAM_locate(params, OSSL_CIPHER_PARAM_AEAD_IV_GENERATED)) &&
            !OSSL_PARAM_set_uint(p, (unsigned int) c->iv_gen_rand)) return 0;
    return 1;
}

static int ag_set_ctx_params(void *vctx, const OSSL_PARAM params[]) {
    struct ag_ctx *c = vctx;
    const OSSL_PARAM *p;

    if ((p = OSSL_PARAM_locate_const(params, OSSL_CIPHER_PARAM_AEAD_TAG)) != NULL) {
        // Decrypt only, and only the full tag: the accelerator's open op runs
        // its own constant-time compare over all 16 bytes, so a truncated tag
        // cannot be checked here at all. Refusing is the loud outcome, and it
        // happens before any data, so the caller sees a clean EVP error.
        if (p->data == NULL || p->data_type != OSSL_PARAM_OCTET_STRING) return 0;
        if (p->data_size != AG_TAGLEN || c->enc) return 0;
        memcpy(c->tag, p->data, AG_TAGLEN);
        c->tag_set = 1;
    }
    if ((p = OSSL_PARAM_locate_const(params, OSSL_CIPHER_PARAM_KEYLEN)) != NULL) {
        size_t sz;
        if (!OSSL_PARAM_get_size_t(p, &sz) || sz != AG_KEYLEN) return 0;
    }
    if ((p = OSSL_PARAM_locate_const(params, OSSL_CIPHER_PARAM_AEAD_IVLEN)) != NULL) {
        size_t sz;
        // 96-bit nonces only: any other length needs the GHASH-derived J0 that
        // the accelerator does not expose.
        if (!OSSL_PARAM_get_size_t(p, &sz) || sz != AG_IVLEN) return 0;
    }
    if ((p = OSSL_PARAM_locate_const(params, OSSL_CIPHER_PARAM_AEAD_TLS1_AAD)) != NULL) {
        if (p->data_type != OSSL_PARAM_OCTET_STRING) return 0;
        if (!ag_tls_aad(c, p->data, p->data_size)) return 0;
    }
    if ((p = OSSL_PARAM_locate_const(params, OSSL_CIPHER_PARAM_AEAD_TLS1_IV_FIXED)) != NULL) {
        if (p->data == NULL || p->data_type != OSSL_PARAM_OCTET_STRING) return 0;
        if (!ag_tls_iv_fixed(c, p->data, p->data_size)) return 0;
    }
    if ((p = OSSL_PARAM_locate_const(params, OSSL_CIPHER_PARAM_AEAD_TLS1_SET_IV_INV)) != NULL) {
        if (p->data == NULL || p->data_type != OSSL_PARAM_OCTET_STRING) return 0;
        if (!ag_set_iv_inv(c, p->data, p->data_size)) return 0;
    }
    return 1;
}

static const OSSL_PARAM ag_gettable[] = {
    OSSL_PARAM_size_t(OSSL_CIPHER_PARAM_BLOCK_SIZE, NULL),
    OSSL_PARAM_size_t(OSSL_CIPHER_PARAM_KEYLEN, NULL),
    OSSL_PARAM_size_t(OSSL_CIPHER_PARAM_IVLEN, NULL),
    OSSL_PARAM_uint(OSSL_CIPHER_PARAM_MODE, NULL),
    OSSL_PARAM_int(OSSL_CIPHER_PARAM_AEAD, NULL),
    OSSL_PARAM_int(OSSL_CIPHER_PARAM_CUSTOM_IV, NULL),
    OSSL_PARAM_END
};
static const OSSL_PARAM ag_gettable_ctx[] = {
    OSSL_PARAM_size_t(OSSL_CIPHER_PARAM_KEYLEN, NULL),
    OSSL_PARAM_size_t(OSSL_CIPHER_PARAM_IVLEN, NULL),
    OSSL_PARAM_size_t(OSSL_CIPHER_PARAM_AEAD_TAGLEN, NULL),
    OSSL_PARAM_octet_string(OSSL_CIPHER_PARAM_IV, NULL, 0),
    OSSL_PARAM_octet_string(OSSL_CIPHER_PARAM_UPDATED_IV, NULL, 0),
    OSSL_PARAM_octet_string(OSSL_CIPHER_PARAM_AEAD_TAG, NULL, 0),
    OSSL_PARAM_size_t(OSSL_CIPHER_PARAM_AEAD_TLS1_AAD_PAD, NULL),
    OSSL_PARAM_octet_string(OSSL_CIPHER_PARAM_AEAD_TLS1_GET_IV_GEN, NULL, 0),
    OSSL_PARAM_uint(OSSL_CIPHER_PARAM_AEAD_IV_GENERATED, NULL),
    OSSL_PARAM_END
};
static const OSSL_PARAM ag_settable_ctx[] = {
    OSSL_PARAM_size_t(OSSL_CIPHER_PARAM_KEYLEN, NULL),
    OSSL_PARAM_size_t(OSSL_CIPHER_PARAM_AEAD_IVLEN, NULL),
    OSSL_PARAM_octet_string(OSSL_CIPHER_PARAM_AEAD_TAG, NULL, 0),
    OSSL_PARAM_octet_string(OSSL_CIPHER_PARAM_AEAD_TLS1_AAD, NULL, 0),
    OSSL_PARAM_octet_string(OSSL_CIPHER_PARAM_AEAD_TLS1_IV_FIXED, NULL, 0),
    OSSL_PARAM_octet_string(OSSL_CIPHER_PARAM_AEAD_TLS1_SET_IV_INV, NULL, 0),
    OSSL_PARAM_END
};
static const OSSL_PARAM *ag_gettable_params(void *p) { (void) p; return ag_gettable; }
static const OSSL_PARAM *ag_gettable_ctx_params(void *c, void *p) { (void) c; (void) p; return ag_gettable_ctx; }
static const OSSL_PARAM *ag_settable_ctx_params(void *c, void *p) { (void) c; (void) p; return ag_settable_ctx; }

static const OSSL_DISPATCH ag_functions[] = {
    { OSSL_FUNC_CIPHER_NEWCTX, (void (*)(void)) ag_newctx },
    { OSSL_FUNC_CIPHER_FREECTX, (void (*)(void)) ag_freectx },
    { OSSL_FUNC_CIPHER_DUPCTX, (void (*)(void)) ag_dupctx },
    { OSSL_FUNC_CIPHER_ENCRYPT_INIT, (void (*)(void)) ag_einit },
    { OSSL_FUNC_CIPHER_DECRYPT_INIT, (void (*)(void)) ag_dinit },
    { OSSL_FUNC_CIPHER_UPDATE, (void (*)(void)) ag_update },
    { OSSL_FUNC_CIPHER_FINAL, (void (*)(void)) ag_final },
    { OSSL_FUNC_CIPHER_CIPHER, (void (*)(void)) ag_cipher },
    { OSSL_FUNC_CIPHER_GET_PARAMS, (void (*)(void)) ag_get_params },
    { OSSL_FUNC_CIPHER_GET_CTX_PARAMS, (void (*)(void)) ag_get_ctx_params },
    { OSSL_FUNC_CIPHER_SET_CTX_PARAMS, (void (*)(void)) ag_set_ctx_params },
    { OSSL_FUNC_CIPHER_GETTABLE_PARAMS, (void (*)(void)) ag_gettable_params },
    { OSSL_FUNC_CIPHER_GETTABLE_CTX_PARAMS, (void (*)(void)) ag_gettable_ctx_params },
    { OSSL_FUNC_CIPHER_SETTABLE_CTX_PARAMS, (void (*)(void)) ag_settable_ctx_params },
    { 0, NULL }
};

// ChaCha20-Poly1305 is deliberately NOT registered. OpenSSL's TLS record layer
// drives the AEAD through a call sequence cp_* does not reproduce, and the
// result is a bad record MAC rather than a clean decline -- so every TLS 1.3
// connection negotiating TLS_CHACHA20_POLY1305_SHA256 fails outright. Measured
// on an A10X device: registered, a local s_server/s_client handshake dies with
// "decryption failed or bad record mac"; dropped, the same handshake succeeds.
// Nothing is lost for ssh/scp, which use the raw stream cipher (EVP_chacha20,
// via OpenSSH's cipher-chachapoly-libcrypto) and still measure 19 MB/s
// accelerated against 8.8 MB/s software. Since a provider is selected by
// property and cannot fall back once chosen, "register it and hope callers
// stay inside the supported pattern" is not a safe option: the unsupported
// pattern here is the system's TLS. Re-register only when the TLS AEAD surface
// is implemented and an s_client handshake passes -- which is exactly what the
// AES-256-GCM implementation above does, and what its gates prove.
// Build with -DISH_PROVIDER_ENABLE_AEAD to opt back in for that work.
#ifdef ISH_PROVIDER_ENABLE_AEAD
#define ISH_CHACHA_ALGS \
    { "ChaCha20", "provider=ish", cc_functions, "iSH-AOK-accelerated ChaCha20" }, \
    { "ChaCha20-Poly1305", "provider=ish", cp_functions, "iSH-AOK-accelerated ChaCha20-Poly1305" },
#else
#define ISH_CHACHA_ALGS \
    { "ChaCha20", "provider=ish", cc_functions, "iSH-AOK-accelerated ChaCha20" },
#endif
// The OID and the id-aes256-GCM short name are spelled out so an implicit
// fetch by either (OpenSSH asks for EVP_aes_256_gcm(), which resolves through
// OBJ_nid2sn) reaches this implementation without depending on the default
// provider having populated the namemap first.
#define ISH_AES_ALGS \
    { "AES-256-GCM:id-aes256-GCM:2.16.840.1.101.3.4.1.46", "provider=ish", \
      ag_functions, "iSH-AOK-accelerated AES-256-GCM" },
#define ISH_ALGS_END { NULL, NULL, NULL, NULL }

// One list per outcome of the two independent probes, all const so a query
// racing the first probe can never see a half-built list.
static const OSSL_ALGORITHM ish_ciphers_both[] = { ISH_CHACHA_ALGS ISH_AES_ALGS ISH_ALGS_END };
static const OSSL_ALGORITHM ish_ciphers_chacha[] = { ISH_CHACHA_ALGS ISH_ALGS_END };
static const OSSL_ALGORITHM ish_ciphers_aes[] = { ISH_AES_ALGS ISH_ALGS_END };
static const OSSL_ALGORITHM ish_ciphers_none[] = { ISH_ALGS_END };

static const OSSL_ALGORITHM *ish_query(void *provctx, int operation_id, int *no_cache) {
    (void) provctx; *no_cache = 0;
    if (operation_id != OSSL_OP_CIPHER)
        return NULL;
    // Offer each algorithm only if the kernel says it is actually accelerated.
    // The two probes are independent because the kernel's self-tests are: an
    // AES-less host must still get ChaCha20, and a guest on an older kernel
    // that answers EOPNOTSUPP for AES must still get ChaCha20 rather than have
    // AES-GCM routed into a syscall that will refuse it.
    int chacha = accel_ok(), aes = aes_accel_ok();
    if (chacha && aes) return ish_ciphers_both;
    if (chacha) return ish_ciphers_chacha;
    if (aes) return ish_ciphers_aes;
    return ish_ciphers_none;
}

static void ish_teardown(void *provctx) { (void) provctx; }

// Without these, `openssl list -providers` prints "Unable to query provider
// parameters for ish" -- which is exactly the command the docs send people to.
static const OSSL_PARAM ish_param_types[] = {
    OSSL_PARAM_DEFN(OSSL_PROV_PARAM_NAME, OSSL_PARAM_UTF8_PTR, NULL, 0),
    OSSL_PARAM_DEFN(OSSL_PROV_PARAM_VERSION, OSSL_PARAM_UTF8_PTR, NULL, 0),
    OSSL_PARAM_DEFN(OSSL_PROV_PARAM_BUILDINFO, OSSL_PARAM_UTF8_PTR, NULL, 0),
    OSSL_PARAM_DEFN(OSSL_PROV_PARAM_STATUS, OSSL_PARAM_INTEGER, NULL, 0),
    OSSL_PARAM_END
};
static const OSSL_PARAM *ish_gettable_params(void *provctx) {
    (void) provctx; return ish_param_types;
}
static int ish_get_params(void *provctx, OSSL_PARAM params[]) {
    (void) provctx;
    OSSL_PARAM *p;
    if ((p = OSSL_PARAM_locate(params, OSSL_PROV_PARAM_NAME)) != NULL &&
            !OSSL_PARAM_set_utf8_ptr(p, "iSH-AOK crypto accelerator")) return 0;
    if ((p = OSSL_PARAM_locate(params, OSSL_PROV_PARAM_VERSION)) != NULL &&
            !OSSL_PARAM_set_utf8_ptr(p, "1.1")) return 0;
    if ((p = OSSL_PARAM_locate(params, OSSL_PROV_PARAM_BUILDINFO)) != NULL &&
            !OSSL_PARAM_set_utf8_ptr(p, "ISH_SYS_AEAD")) return 0;
    // "Running" means the provider is usable, not that either algorithm is
    // accelerated -- that answer is ish_query's, per algorithm.
    if ((p = OSSL_PARAM_locate(params, OSSL_PROV_PARAM_STATUS)) != NULL &&
            !OSSL_PARAM_set_int(p, 1)) return 0;
    return 1;
}

static const OSSL_DISPATCH ish_provider_funcs[] = {
    { OSSL_FUNC_PROVIDER_QUERY_OPERATION, (void (*)(void)) ish_query },
    { OSSL_FUNC_PROVIDER_GET_PARAMS, (void (*)(void)) ish_get_params },
    { OSSL_FUNC_PROVIDER_GETTABLE_PARAMS, (void (*)(void)) ish_gettable_params },
    { OSSL_FUNC_PROVIDER_TEARDOWN, (void (*)(void)) ish_teardown },
    { 0, NULL }
};

int OSSL_provider_init(const OSSL_CORE_HANDLE *handle, const OSSL_DISPATCH *in,
        const OSSL_DISPATCH **out, void **provctx) {
    (void) handle; (void) in; *provctx = NULL;
    *out = ish_provider_funcs;
    return 1;
}
