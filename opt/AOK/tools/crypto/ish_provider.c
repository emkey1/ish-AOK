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
//   ChaCha20-Poly1305   -- whole AEAD (openssl speed, TLS records, one-shot
//                          libcrypto users); data via the raw-stream op, tag via
//                          the accelerator's MAC op -- shown bit-exact to OpenSSL.
//
// Scope: correct for the one-init + one-cipher-call pattern ssh uses, and for
// 64-byte-aligned streaming updates of the raw cipher. The AEAD tag covers the
// whole message and the accelerator's MAC op is one-shot, so the AEAD cipher is
// correct for a single data update (the dominant pattern); a second data update
// poisons the ctx so the caller errors rather than getting a wrong tag. Arbitrary
// sub-block-straddling / multi-chunk updates would need keystream+MAC buffering;
// not implemented -- such callers are rare and this provider simply must not be
// selected for them. Poisoning fails loudly; it never emits wrong output.

#include <openssl/core.h>
#include <openssl/core_dispatch.h>
#include <openssl/core_names.h>
#include <openssl/params.h>
#include <openssl/crypto.h>
#include <string.h>
#include <stdint.h>

extern long syscall(long, ...);
#define ISH_SYS_AEAD 0xacc0
#define ISH_ALG_CHACHA20 1

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

static const OSSL_ALGORITHM ish_ciphers[] = {
    { "ChaCha20", "provider=ish", cc_functions, "iSH-accelerated ChaCha20" },
    { "ChaCha20-Poly1305", "provider=ish", cp_functions, "iSH-accelerated ChaCha20-Poly1305" },
    { NULL, NULL, NULL, NULL }
};
static const OSSL_ALGORITHM ish_ciphers_none[] = { { NULL, NULL, NULL, NULL } };

static const OSSL_ALGORITHM *ish_query(void *provctx, int operation_id, int *no_cache) {
    (void) provctx; *no_cache = 0;
    if (operation_id == OSSL_OP_CIPHER)
        return accel_ok() ? ish_ciphers : ish_ciphers_none;
    return NULL;
}

static void ish_teardown(void *provctx) { (void) provctx; }

static const OSSL_DISPATCH ish_provider_funcs[] = {
    { OSSL_FUNC_PROVIDER_QUERY_OPERATION, (void (*)(void)) ish_query },
    { OSSL_FUNC_PROVIDER_TEARDOWN, (void (*)(void)) ish_teardown },
    { 0, NULL }
};

int OSSL_provider_init(const OSSL_CORE_HANDLE *handle, const OSSL_DISPATCH *in,
        const OSSL_DISPATCH **out, void **provctx) {
    (void) handle; (void) in; *provctx = NULL;
    *out = ish_provider_funcs;
    return 1;
}
