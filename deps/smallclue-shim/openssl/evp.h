// Just enough of OpenSSL's EVP digest API for deps/smallclue/src/checksum_app.c,
// implemented on CommonCrypto.
//
// WHY THIS EXISTS. md5sum, sha1sum and sha256sum were the only applets AOK
// refused for a reason that was never really about the applet: checksum_app.c
// is a 200-line wrapper around EVP, SmallCLUE gets EVP from OpenSSL because
// libgit2 and OpenSSH already drag it in, and AOK links neither -- so three
// working commands were stubbed out over a dependency that only one file's
// #include line actually needed.
//
// CommonCrypto is the way out: it is part of libSystem on macOS and iOS, so
// nothing new is linked and neither the meson build nor the Xcode project
// gains a dependency. kernel/random.c already uses it for exactly that reason.
//
// SCOPE. This is not an OpenSSL compatibility layer and must not grow into
// one. It covers the eleven names checksum_app.c uses and nothing else; a
// second consumer wanting ciphers, HMAC or the algorithm-by-name lookup should
// get the real library rather than an ever-widening imitation of it. Anything
// missing is a compile error naming the symbol, which is the correct outcome.
//
// The digest calls themselves are pure transforms over the caller's buffer --
// no descriptors, no host state -- which is why tools/check-native-libc.py can
// allow CC_* at all. That is a property of the digest functions specifically,
// not of CommonCrypto as a whole (CCRandomGenerateBytes, for instance, is
// emphatically not pure).

#ifndef AOK_SMALLCLUE_SHIM_OPENSSL_EVP_H
#define AOK_SMALLCLUE_SHIM_OPENSSL_EVP_H

#include <stdlib.h>
#include <string.h>

#include <CommonCrypto/CommonDigest.h>

#define EVP_MAX_MD_SIZE 64

// The "algorithm" handle. OpenSSL's is an opaque method table; here it only
// has to say which of the three CommonCrypto contexts to drive, so it is an
// enum in a struct -- a struct rather than a bare enum so that `const EVP_MD *`
// stays the type checksum_app.c declares.
typedef struct { int aok_kind; } EVP_MD;

enum { AOK_EVP_MD5 = 1, AOK_EVP_SHA1 = 2, AOK_EVP_SHA256 = 3 };

typedef struct {
    const EVP_MD *md;
    union {
        CC_MD5_CTX md5;
        CC_SHA1_CTX sha1;
        CC_SHA256_CTX sha256;
    } u;
} EVP_MD_CTX;

static inline const EVP_MD *EVP_md5(void) {
    static const EVP_MD md = { AOK_EVP_MD5 };
    return &md;
}
static inline const EVP_MD *EVP_sha1(void) {
    static const EVP_MD md = { AOK_EVP_SHA1 };
    return &md;
}
static inline const EVP_MD *EVP_sha256(void) {
    static const EVP_MD md = { AOK_EVP_SHA256 };
    return &md;
}

static inline EVP_MD_CTX *EVP_MD_CTX_new(void) {
    return (EVP_MD_CTX *) calloc(1, sizeof(EVP_MD_CTX));
}

static inline void EVP_MD_CTX_free(EVP_MD_CTX *ctx) {
    if (ctx != NULL) {
        // The context can hold the tail of the message being hashed, so it is
        // wiped rather than merely released. Cheap, and the alternative is a
        // fragment of the input left in freed memory.
        memset(ctx, 0, sizeof(*ctx));
        free(ctx);
    }
}

// Returns 1 on success and 0 on failure, matching OpenSSL rather than the
// usual 0/-1: checksum_app.c compares against 1 explicitly. `engine` is
// OpenSSL's hardware-engine argument and is always NULL here.
static inline int EVP_DigestInit_ex(EVP_MD_CTX *ctx, const EVP_MD *md, void *engine) {
    (void) engine;
    if (ctx == NULL || md == NULL)
        return 0;
    ctx->md = md;
    switch (md->aok_kind) {
        case AOK_EVP_MD5:    return CC_MD5_Init(&ctx->u.md5) == 1;
        case AOK_EVP_SHA1:   return CC_SHA1_Init(&ctx->u.sha1) == 1;
        case AOK_EVP_SHA256: return CC_SHA256_Init(&ctx->u.sha256) == 1;
    }
    return 0;
}

static inline int EVP_DigestUpdate(EVP_MD_CTX *ctx, const void *data, size_t len) {
    if (ctx == NULL || ctx->md == NULL)
        return 0;
    // CC_LONG is 32-bit, so a single update is chunked rather than truncated.
    // checksum_app.c reads in 64 KiB blocks and never comes close, but a
    // silent truncation here would produce a WRONG digest rather than an
    // error, which is the one failure mode a checksum tool must not have.
    const unsigned char *p = (const unsigned char *) data;
    while (len > 0) {
        size_t chunk = len > 0x40000000u ? 0x40000000u : len;
        int ok;
        switch (ctx->md->aok_kind) {
            case AOK_EVP_MD5:    ok = CC_MD5_Update(&ctx->u.md5, p, (CC_LONG) chunk) == 1; break;
            case AOK_EVP_SHA1:   ok = CC_SHA1_Update(&ctx->u.sha1, p, (CC_LONG) chunk) == 1; break;
            case AOK_EVP_SHA256: ok = CC_SHA256_Update(&ctx->u.sha256, p, (CC_LONG) chunk) == 1; break;
            default: return 0;
        }
        if (!ok)
            return 0;
        p += chunk;
        len -= chunk;
    }
    return 1;
}

static inline int EVP_DigestFinal_ex(EVP_MD_CTX *ctx, unsigned char *out, unsigned int *outlen) {
    if (ctx == NULL || ctx->md == NULL || out == NULL)
        return 0;
    int ok = 0;
    unsigned int len = 0;
    switch (ctx->md->aok_kind) {
        case AOK_EVP_MD5:
            ok = CC_MD5_Final(out, &ctx->u.md5) == 1;
            len = CC_MD5_DIGEST_LENGTH;
            break;
        case AOK_EVP_SHA1:
            ok = CC_SHA1_Final(out, &ctx->u.sha1) == 1;
            len = CC_SHA1_DIGEST_LENGTH;
            break;
        case AOK_EVP_SHA256:
            ok = CC_SHA256_Final(out, &ctx->u.sha256) == 1;
            len = CC_SHA256_DIGEST_LENGTH;
            break;
        default:
            return 0;
    }
    if (ok && outlen != NULL)
        *outlen = len;
    return ok;
}

#endif /* AOK_SMALLCLUE_SHIM_OPENSSL_EVP_H */
