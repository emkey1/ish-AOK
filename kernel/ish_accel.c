// iSH crypto accelerator: guest-facing syscall glue around the host-native
// ChaCha20-Poly1305 (kernel/ish_accel_crypto.c). A guest issues syscall
// ISH_SYS_AEAD with a pointer to a struct ish_aead_req; the host runs the
// AEAD natively on the guest's buffers instead of the guest emulating its
// own libcrypto. Off by default (doEnableCryptoAccel); enabled only if the
// RFC 8439 self-test passes at startup.
//
// This is the Phase-1 proof interface (a private syscall). The productized
// path is an OpenSSL provider in the guest talking to this, so ssh/scp
// accelerate transparently.

#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "kernel/calls.h"
#include "kernel/task.h"
#include "kernel/errno.h"
#include "kernel/ish_accel_crypto.h"
#include "debug.h"

// Set from ISH_CRYPTO_ACCEL (main.c) or the app crypto-accel preference. The
// accelerator only actually runs if the RFC 8439 self-test passed -- which is
// checked lazily (pthread_once) on first use, so neither the CLI nor the app
// has to remember an init call. ish_accel_init() forces it eagerly.
bool doEnableCryptoAccel = false;
static bool accel_selftest_ok = false;
static pthread_once_t accel_selftest_once = PTHREAD_ONCE_INIT;

static void run_accel_selftest(void) {
    accel_selftest_ok = ish_accel_crypto_selftest();
    if (!accel_selftest_ok)
        printk("ish-accel: crypto self-test FAILED, accelerator disabled\n");
}

static bool accel_ready(void) {
    pthread_once(&accel_selftest_once, run_accel_selftest);
    return accel_selftest_ok;
}

// AES-256-GCM is a separate capability with its own self-test: it needs host
// AES/PMULL instructions, so it can be absent on a build host where ChaCha20
// is fine. Kept independent so a guest asking for one is never refused because
// the other is unavailable.
static bool accel_aes_selftest_ok = false;
static pthread_once_t accel_aes_selftest_once = PTHREAD_ONCE_INIT;

static void run_accel_aes_selftest(void) {
    accel_aes_selftest_ok = ish_aes_gcm_available() && ish_accel_aes_selftest();
    if (!accel_aes_selftest_ok)
        printk("ish-accel: AES-256-GCM unavailable (no host AES instructions "
                "or self-test failed); guests will use their own\n");
}

static bool accel_aes_ready(void) {
    pthread_once(&accel_aes_selftest_once, run_accel_aes_selftest);
    return accel_aes_selftest_ok;
}

void ish_accel_init(void) {
    (void) accel_ready();
}

// Guest ABI. Fixed-layout, all 64-bit fields after the two u32s so the
// struct is identical on arm64 and riscv64 guests. op: 0=seal, 1=open.
// alg: 0=chacha20-poly1305. All pointers are guest addresses.
struct ish_aead_req {
    uint32_t op;
    uint32_t alg;
    uint64_t key;    // 32 bytes
    uint64_t nonce;  // 12 bytes
    uint64_t aad;
    uint64_t aadlen;
    uint64_t in;     // plaintext (seal) or ciphertext (open)
    uint64_t inlen;
    uint64_t out;    // ciphertext (seal) or plaintext (open), inlen bytes
    uint64_t tag;    // 16 bytes: written (seal) or read (open)
};

enum {
    ISH_AEAD_ALG_CHACHA20_POLY1305 = 0,
    ISH_AEAD_ALG_CHACHA20 = 1,
    ISH_AEAD_ALG_AES256_GCM = 2,
};

// op 0=seal, 1=open, 2=raw stream (back-compat), 3=tag/MAC only, 4=query.
// The query op touches none of the request's pointers, so a provider can ask
// "is this algorithm accelerated here?" without arranging valid buffers, and
// gets _EOPNOTSUPP rather than a confusing fault when the answer is no.
enum { ISH_AEAD_OP_QUERY = 4 };

// Per-span callback for user_transform_two: feed one page-span through the
// streaming cipher (keystream + Poly1305 state carry across calls).
static void ish_aead_span(const void *in_host, void *out_host, size_t span, void *ctx) {
    ish_aead_update((struct ish_aead_stream *) ctx,
            (const uint8_t *) in_host, (uint8_t *) out_host, span);
}
// Same, for the raw ChaCha20 stream op (no Poly1305).
static void ish_chacha_span(const void *in_host, void *out_host, size_t span, void *ctx) {
    ish_chacha20_stream_update((struct ish_chacha_stream *) ctx,
            (const uint8_t *) in_host, (uint8_t *) out_host, span);
}
// Read-only span callback for the tag/MAC op: Poly1305 the ciphertext in place.
static void ish_mac_span(const void *host, size_t span, void *ctx) {
    ish_aead_mac_update((struct ish_aead_mac *) ctx, (const uint8_t *) host, span);
}
// Same, for AES-256-GCM (counter position and GHASH block carry across spans).
static void ish_aes_span(const void *in_host, void *out_host, size_t span, void *ctx) {
    ish_aes256_gcm_update((struct ish_aes_gcm_stream *) ctx,
            (const uint8_t *) in_host, (uint8_t *) out_host, span);
}
// Bound per-call sizes so a bogus request can't drive a huge host malloc.
// SSH records are <= ~256 KiB; anything larger just falls back to the guest.
#define ISH_AEAD_MAX_IN   (4u << 20)   // 4 MiB
#define ISH_AEAD_MAX_AAD  4096u

// Returns 0 on success, or a negative guest errno. _ENOSYS signals "not
// available" so a caller/provider falls back to its own software path.
dword_t sys_ish_aead_guest(guest_addr_t req_addr) {
    if (!doEnableCryptoAccel)
        return _ENOSYS;

    struct ish_aead_req req;
    if (user_read(req_addr, &req, sizeof(req)))
        return _EFAULT;
    // alg 0 = ChaCha20-Poly1305 AEAD (op 0=seal, 1=open, 3=tag/MAC-only);
    // alg 1 = raw ChaCha20 stream (op ignored); alg 2 = AES-256-GCM (op 0/1).
    // op 2 also selects the raw stream for back-compat.
    bool is_aes = req.alg == ISH_AEAD_ALG_AES256_GCM;
    bool raw_stream = (req.alg == ISH_AEAD_ALG_CHACHA20) || (!is_aes && req.op == 2);
    bool mac_only = (req.alg == ISH_AEAD_ALG_CHACHA20_POLY1305) && req.op == 3;
    if (req.alg != ISH_AEAD_ALG_CHACHA20_POLY1305 &&
            req.alg != ISH_AEAD_ALG_CHACHA20 && !is_aes)
        return _EOPNOTSUPP;

    // Per-algorithm availability. Answering the query op here keeps it free of
    // any buffer access, so a provider can ask before it has a request set up.
    bool ready = is_aes ? accel_aes_ready() : accel_ready();
    if (req.op == ISH_AEAD_OP_QUERY)
        return ready ? 0 : _EOPNOTSUPP;
    if (!ready)
        return is_aes ? _EOPNOTSUPP : _ENOSYS;

    if (!raw_stream && !mac_only && req.op > 1)
        return _EINVAL;
    if (req.inlen > ISH_AEAD_MAX_IN || req.aadlen > ISH_AEAD_MAX_AAD)
        return _EMSGSIZE; // too big for the fast path; caller falls back

    uint8_t key[32], nonce[12], tag[16];
    if (user_read(req.key, key, sizeof(key)))
        return _EFAULT;

    if (raw_stream) {
        // Raw ChaCha20 stream: req.nonce points to the 16-byte EVP IV
        // (ctr32_le + nonce96). No aad, no tag; XOR keystream in place.
        uint8_t iv[16];
        dword_t sret = 0;
        if (user_read(req.nonce, iv, sizeof(iv))) { memset(key, 0, sizeof(key)); return _EFAULT; }
        struct ish_chacha_stream cs;
        ish_chacha20_stream_begin(&cs, key, iv);
        if (req.inlen &&
                user_transform_two(req.in, req.out, req.inlen, ish_chacha_span, &cs))
            sret = _EFAULT;
        memset(key, 0, sizeof(key));
        memset(&cs, 0, sizeof(cs));
        return sret;
    }

    if (user_read(req.nonce, nonce, sizeof(nonce)))
        return _EFAULT;
    // aad is small and bounded; staging it in a per-thread scratch avoids a
    // second direct-pointer walk (the streaming begin consumes it up front).
    static __thread uint8_t scratch_aad[ISH_AEAD_MAX_AAD];
    if (req.aadlen && user_read(req.aad, scratch_aad, req.aadlen)) {
        memset(key, 0, sizeof(key));
        return _EFAULT;
    }

    if (is_aes) {
        // AES-256-GCM. Same zero-copy shape as the ChaCha AEAD below: the
        // data is transformed straight over guest memory a page-span at a
        // time, with the counter and GHASH state carrying across spans.
        dword_t aret = 0;
        struct ish_aes_gcm_stream st;
        ish_aes256_gcm_begin(&st, key, nonce, scratch_aad, req.aadlen, (int) req.op);
        if (req.inlen &&
                user_transform_two(req.in, req.out, req.inlen, ish_aes_span, &st)) {
            aret = _EFAULT;
            goto aes_out;
        }
        if (req.op == 0) { // seal
            ish_aes256_gcm_finish(&st, NULL, tag);
            if (user_write(req.tag, tag, sizeof(tag)))
                aret = _EFAULT;
        } else { // open
            if (user_read(req.tag, tag, sizeof(tag))) { aret = _EFAULT; goto aes_out; }
            if (ish_aes256_gcm_finish(&st, tag, NULL) != 0) {
                // Same rule as the ChaCha AEAD: open decrypted eagerly, so the
                // guest must not be left holding unauthenticated plaintext.
                if (req.inlen)
                    user_zero(req.out, req.inlen);
                aret = _EBADMSG;
            }
        }
    aes_out:
        memset(key, 0, sizeof(key));
        memset(&st, 0, sizeof(st)); // holds the key schedule and GHASH state
        return aret;
    }

    if (mac_only) {
        // Compute the AEAD tag over req.in (ciphertext), no encryption. Lets a
        // provider that ciphers the data via the raw-stream op verify/produce
        // the tag at EVP Final. req.tag receives the 16-byte tag.
        dword_t mret = 0;
        struct ish_aead_mac mac;
        ish_aead_mac_begin(&mac, key, nonce, scratch_aad, req.aadlen);
        if (req.inlen && user_read_walk(req.in, req.inlen, ish_mac_span, &mac))
            mret = _EFAULT;
        else {
            uint8_t tag[16];
            ish_aead_mac_final(&mac, tag);
            if (user_write(req.tag, tag, sizeof(tag)))
                mret = _EFAULT;
        }
        memset(key, 0, sizeof(key));
        memset(&mac, 0, sizeof(mac));
        return mret;
    }

    dword_t ret = 0;
    struct ish_aead_stream stream;
    ish_aead_begin(&stream, key, nonce, scratch_aad, req.aadlen, (int) req.op);
    // The data is transformed IN PLACE over guest memory, no bounce buffer:
    // the streaming cipher runs directly on each page-span (user_transform_two
    // resolves direct host pointers under the mem lock; seal encrypts pt->ct
    // and Poly1305s the ciphertext, open decrypts ct->pt eagerly).
    if (req.inlen &&
            user_transform_two(req.in, req.out, req.inlen, ish_aead_span, &stream)) {
        ret = _EFAULT;
        goto out;
    }

    if (req.op == 0) { // seal
        ish_aead_finish(&stream, NULL, tag);
        if (user_write(req.tag, tag, sizeof(tag))) { ret = _EFAULT; goto out; }
    } else { // open
        if (user_read(req.tag, tag, sizeof(tag))) { ret = _EFAULT; goto out; }
        if (ish_aead_finish(&stream, tag, NULL) != 0) {
            // Authentication failed. open decrypted eagerly into req.out, so
            // scrub the guest buffer before returning -- the guest must never
            // observe unauthenticated plaintext.
            if (req.inlen)
                user_zero(req.out, req.inlen);
            ret = _EBADMSG;
        }
    }

out:
    memset(key, 0, sizeof(key));
    memset(&stream, 0, sizeof(stream)); // holds keystream + poly state
    return ret;
}
