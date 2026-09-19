// SHA-crypt: the $5$ and $6$ password hashes a Linux /etc/shadow holds.
//
// This exists because native code runs on the HOST, and Darwin's crypt() is
// DES-only -- it cannot compute the function the guest's stored hash was made
// with, so it could never match. kernel/native_libc.c's nlibc_crypt was a
// deliberate refusal for that reason: for an authentication primitive, a
// plausible-looking wrong answer is an authentication BYPASS, not a bug. This
// is the implementation that note asked for.
//
// Ulrich Drepper's specification, which is what glibc, musl and shadow-utils
// all implement. The algorithm is fiddly in ways that do not announce
// themselves -- the digest-B feed depends on the bits of the password length,
// the salt is rehashed 16 + A[0] times, and the final base64 is a fixed
// PERMUTATION rather than a straight encode -- so it is checked against the
// published vectors in tests/manual/sha_crypt.c. An implementation that is
// subtly wrong still produces confident-looking hashes; only the vectors can
// tell the difference.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel/sha_crypt.h"

#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#endif

// The digest, behind three calls, so the algorithm below is ordinary portable C
// and gets compiled (and warned about) by the Linux CI even though the backend
// it needs is Apple's. Where there is no backend this fails closed and
// aok_sha_crypt returns NULL, which is what an unsupported salt has always
// returned.
struct shad {
#ifdef __APPLE__
    union { CC_SHA256_CTX s256; CC_SHA512_CTX s512; } ctx;
#endif
    bool is512;
    bool usable;
};

#define SHAD_MAX 64

static void shad_init(struct shad *d, bool is512) {
    memset(d, 0, sizeof(*d));
    d->is512 = is512;
#ifdef __APPLE__
    if (is512)
        CC_SHA512_Init(&d->ctx.s512);
    else
        CC_SHA256_Init(&d->ctx.s256);
    d->usable = true;
#else
    d->usable = false;
#endif
}

static void shad_update(struct shad *d, const void *p, size_t n) {
    if (!d->usable || n == 0)
        return;
#ifdef __APPLE__
    if (d->is512)
        CC_SHA512_Update(&d->ctx.s512, p, (CC_LONG) n);
    else
        CC_SHA256_Update(&d->ctx.s256, p, (CC_LONG) n);
#else
    (void) p;
#endif
}

static void shad_final(struct shad *d, unsigned char *out) {
    if (!d->usable) {
        memset(out, 0, SHAD_MAX);
        return;
    }
#ifdef __APPLE__
    if (d->is512)
        CC_SHA512_Final(out, &d->ctx.s512);
    else
        CC_SHA256_Final(out, &d->ctx.s256);
#endif
}

static size_t shad_len(bool is512) { return is512 ? 64 : 32; }

// crypt(3)'s alphabet, which is NOT standard base64: the order is different and
// there is no padding.
static const char b64_chars[] =
    "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

// Three bytes -> `n` characters, little-endian within the group.
static void b64_from_24bit(char **out, size_t *room, unsigned char b2,
                           unsigned char b1, unsigned char b0, int n) {
    unsigned w = ((unsigned) b2 << 16) | ((unsigned) b1 << 8) | b0;
    for (int i = 0; i < n && *room > 1; i++) {
        *(*out)++ = b64_chars[w & 0x3f];
        (*room)--;
        w >>= 6;
    }
}

// The output permutation. Drepper's reference writes these out as 21 (SHA-512)
// or 10 (SHA-256) literal calls; as a table it is the same thing and can be
// read against the spec side by side.
static const unsigned char perm512[][3] = {
    {0,21,42},{22,43,1},{44,2,23},{3,24,45},{25,46,4},{47,5,26},{6,27,48},
    {28,49,7},{50,8,29},{9,30,51},{31,52,10},{53,11,32},{12,33,54},{34,55,13},
    {56,14,35},{15,36,57},{37,58,16},{59,17,38},{18,39,60},{40,61,19},{62,20,41},
};
static const unsigned char perm256[][3] = {
    {0,10,20},{21,1,11},{12,22,2},{3,13,23},{24,4,14},{15,25,5},{6,16,26},
    {27,7,17},{18,28,8},{9,19,29},
};

#define ROUNDS_DEFAULT 5000
#define ROUNDS_MIN 1000
#define ROUNDS_MAX 999999999
#define SALT_MAX 16

char *aok_sha_crypt(const char *key, const char *salt, char *buf, size_t buflen) {
    if (key == NULL || salt == NULL || buf == NULL)
        return NULL;

    bool is512;
    if (strncmp(salt, "$6$", 3) == 0)
        is512 = true;
    else if (strncmp(salt, "$5$", 3) == 0)
        is512 = false;
    else
        return NULL;   // $1$, $2b$, $y$ and the rest: fail closed, as before
    const char *magic = is512 ? "$6$" : "$5$";
    salt += 3;

    // "rounds=N$" is optional and clamped rather than rejected, which is what
    // the spec says and what the other implementations do.
    unsigned long rounds = ROUNDS_DEFAULT;
    bool rounds_custom = false;
    if (strncmp(salt, "rounds=", 7) == 0) {
        const char *num = salt + 7;
        char *end;
        unsigned long r = strtoul(num, &end, 10);
        if (*end == '$' && end != num) {
            salt = end + 1;
            rounds = r < ROUNDS_MIN ? ROUNDS_MIN : (r > ROUNDS_MAX ? ROUNDS_MAX : r);
            rounds_custom = true;
        }
    }

    size_t salt_len = strcspn(salt, "$");
    if (salt_len > SALT_MAX)
        salt_len = SALT_MAX;
    size_t key_len = strlen(key);
    size_t n = shad_len(is512);

    unsigned char alt[SHAD_MAX], temp[SHAD_MAX];
    struct shad ctx, alt_ctx;

    // B = H(key . salt . key)
    shad_init(&alt_ctx, is512);
    shad_update(&alt_ctx, key, key_len);
    shad_update(&alt_ctx, salt, salt_len);
    shad_update(&alt_ctx, key, key_len);
    shad_final(&alt_ctx, alt);
    if (!alt_ctx.usable)
        return NULL;   // no digest backend on this platform

    // A = H(key . salt . B-repeated-to-key_len . <bits of key_len>)
    shad_init(&ctx, is512);
    shad_update(&ctx, key, key_len);
    shad_update(&ctx, salt, salt_len);
    for (size_t cnt = key_len; cnt > n; cnt -= n)
        shad_update(&ctx, alt, n);
    shad_update(&ctx, alt, key_len % n);
    // The one step that surprises people: the PASSWORD LENGTH's bits choose
    // whether each round feeds the digest or the key.
    for (size_t cnt = key_len; cnt > 0; cnt >>= 1) {
        if (cnt & 1)
            shad_update(&ctx, alt, n);
        else
            shad_update(&ctx, key, key_len);
    }
    shad_final(&ctx, alt);

    // P = key-length bytes of H(key repeated key_len times)
    shad_init(&alt_ctx, is512);
    for (size_t i = 0; i < key_len; i++)
        shad_update(&alt_ctx, key, key_len);
    shad_final(&alt_ctx, temp);
    unsigned char *p_bytes = malloc(key_len ? key_len : 1);
    if (p_bytes == NULL)
        return NULL;
    for (size_t off = 0; off < key_len; off += n) {
        size_t take = key_len - off < n ? key_len - off : n;
        memcpy(p_bytes + off, temp, take);
    }

    // S = salt-length bytes of H(salt repeated 16 + A[0] times)
    shad_init(&alt_ctx, is512);
    for (unsigned i = 0; i < 16u + (unsigned) alt[0]; i++)
        shad_update(&alt_ctx, salt, salt_len);
    shad_final(&alt_ctx, temp);
    unsigned char *s_bytes = malloc(salt_len ? salt_len : 1);
    if (s_bytes == NULL) {
        free(p_bytes);
        return NULL;
    }
    for (size_t off = 0; off < salt_len; off += n) {
        size_t take = salt_len - off < n ? salt_len - off : n;
        memcpy(s_bytes + off, temp, take);
    }

    // The stretch. Deliberately serial and deliberately slow.
    for (unsigned long r = 0; r < rounds; r++) {
        shad_init(&ctx, is512);
        if (r & 1)
            shad_update(&ctx, p_bytes, key_len);
        else
            shad_update(&ctx, alt, n);
        if (r % 3 != 0)
            shad_update(&ctx, s_bytes, salt_len);
        if (r % 7 != 0)
            shad_update(&ctx, p_bytes, key_len);
        if (r & 1)
            shad_update(&ctx, alt, n);
        else
            shad_update(&ctx, p_bytes, key_len);
        shad_final(&ctx, alt);
    }

    // Wiped rather than just freed: these are derived from the password and
    // this process goes on to do other things.
    memset(p_bytes, 0, key_len);
    memset(s_bytes, 0, salt_len);
    memset(temp, 0, sizeof(temp));
    free(p_bytes);
    free(s_bytes);

    char *out = buf;
    size_t room = buflen;
    // The prefix is echoed back verbatim, rounds= included when the caller
    // gave one -- a verifier compares the whole string, so dropping it would
    // make every custom-rounds hash fail to match itself.
    int wrote;
    if (rounds_custom)
        wrote = snprintf(out, room, "%srounds=%lu$%.*s$", magic, rounds,
                         (int) salt_len, salt);
    else
        wrote = snprintf(out, room, "%s%.*s$", magic, (int) salt_len, salt);
    if (wrote < 0 || (size_t) wrote >= room)
        return NULL;
    out += wrote;
    room -= (size_t) wrote;

    size_t nperm = is512 ? sizeof(perm512) / 3 : sizeof(perm256) / 3;
    const unsigned char (*perm)[3] = is512 ? perm512 : perm256;
    for (size_t i = 0; i < nperm; i++)
        b64_from_24bit(&out, &room, alt[perm[i][0]], alt[perm[i][1]],
                       alt[perm[i][2]], 4);
    // The tail is short: two characters for SHA-512's 64th byte, three for
    // SHA-256's last pair.
    if (is512)
        b64_from_24bit(&out, &room, 0, 0, alt[63], 2);
    else
        b64_from_24bit(&out, &room, 0, alt[31], alt[30], 3);
    if (room < 1)
        return NULL;
    *out = '\0';

    memset(alt, 0, sizeof(alt));
    return buf;
}
