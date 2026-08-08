#ifndef ISH_ACCEL_CRYPTO_H
#define ISH_ACCEL_CRYPTO_H
#include <stdint.h>
#include <stddef.h>

// Host-native ChaCha20-Poly1305 AEAD (RFC 8439). Host pointers only; the
// syscall/device layer resolves and bounds-checks guest buffers first.
// ct must have room for ptlen bytes; tag is 16 bytes.
void ish_chacha20_poly1305_seal(const uint8_t key[32], const uint8_t nonce[12],
        const uint8_t *aad, size_t aadlen, const uint8_t *pt, size_t ptlen,
        uint8_t *ct, uint8_t tag[16]);

// Returns 0 on success (tag valid, pt filled), -1 on auth failure (pt untouched).
int ish_chacha20_poly1305_open(const uint8_t key[32], const uint8_t nonce[12],
        const uint8_t *aad, size_t aadlen, const uint8_t *ct, size_t ctlen,
        const uint8_t tag[16], uint8_t *pt);

// Raw ChaCha20 stream (matches OpenSSL EVP_chacha20): iv is 16 bytes = a
// 32-bit little-endian initial block counter followed by the 12-byte nonce.
// XORs the keystream over in -> out (in==out for in-place). One-shot.
void ish_chacha20_stream(const uint8_t key[32], const uint8_t iv[16],
        const uint8_t *in, uint8_t *out, size_t len);

// Streaming form (keystream position carries across update calls of any
// length) -- used by the accelerator to run over guest pages a span at a time.
struct ish_chacha_stream { _Alignas(16) unsigned char opaque[128]; };
void ish_chacha20_stream_begin(struct ish_chacha_stream *s,
        const uint8_t key[32], const uint8_t iv[16]);
void ish_chacha20_stream_update(struct ish_chacha_stream *s,
        const uint8_t *in, uint8_t *out, size_t len);

// AEAD tag ("MAC") only: compute the ChaCha20-Poly1305 tag over
// (aad || pad || ciphertext || pad || lengths) with the one-time key derived
// from the key+nonce -- no encryption. This is the same tag `seal` produces
// for the same aad+ciphertext, so a provider that ciphers the data separately
// (streaming) can obtain/verify the tag with these calls. Streaming: the
// ciphertext is fed one span at a time.
struct ish_aead_mac { _Alignas(16) unsigned char opaque[320]; };
void ish_aead_mac_begin(struct ish_aead_mac *s, const uint8_t key[32],
        const uint8_t nonce[12], const uint8_t *aad, size_t aadlen);
void ish_aead_mac_update(struct ish_aead_mac *s, const uint8_t *ct, size_t len);
void ish_aead_mac_final(struct ish_aead_mac *s, uint8_t tag[16]);

// ---- Streaming AEAD (lets a caller process the data one page-span at a
// time straight out of / into guest memory, no bounce buffer) --------------
// Opaque, stack-allocatable. Sized generously to hold the real context.
struct ish_aead_stream { _Alignas(16) unsigned char opaque[320]; };

// is_open: 0 = seal (in=pt, out=ct), 1 = open (in=ct, out=pt). aad is consumed
// up front. The total data length must be known so the final length block is
// correct; it is accumulated from the update calls.
void ish_aead_begin(struct ish_aead_stream *s, const uint8_t key[32],
        const uint8_t nonce[12], const uint8_t *aad, size_t aadlen, int is_open);
// Process one contiguous span. Spans of any length are fine; keystream and
// Poly1305 state carry across calls. For open, out receives the decrypted
// bytes eagerly -- the caller MUST discard/zero them if finish() fails.
void ish_aead_update(struct ish_aead_stream *s, const uint8_t *in, uint8_t *out, size_t len);
// seal: writes the 16-byte tag to tag_out. open: returns 0 iff tag_in matches
// (constant-time), else -1.
int ish_aead_finish(struct ish_aead_stream *s, const uint8_t *tag_in, uint8_t *tag_out);

// Validates the implementation against the RFC 8439 test vectors. Returns
// true on success. Called once before the accelerator is enabled.
_Bool ish_accel_crypto_selftest(void);

// ---- AES-256-GCM (kernel/ish_accel_aes.c) ---------------------------------
// Hardware-only: implemented with the host's AES and carry-less-multiply
// instructions, because the alternative fast enough to be worth a syscall is a
// table-driven AES, which leaks key material through the data cache. Where the
// host lacks them ish_aes_gcm_available() returns false and callers must not
// use the rest of this section; the guest keeps using its own AES.
_Bool ish_aes_gcm_available(void);

struct ish_aes_gcm_stream { _Alignas(16) unsigned char opaque[512]; };

// Same shape as the ChaCha20-Poly1305 streaming AEAD above. nonce is the
// 12-byte GCM IV (the only length wired up; J0 is then IV||1). is_open: 0 =
// seal (in=pt, out=ct), 1 = open (in=ct, out=pt). aad is consumed up front.
void ish_aes256_gcm_begin(struct ish_aes_gcm_stream *s, const uint8_t key[32],
        const uint8_t nonce[12], const uint8_t *aad, size_t aadlen, int is_open);
// Spans of any length; the counter position and GHASH block carry across
// calls. For open, out receives plaintext eagerly -- the caller MUST discard
// or zero it if finish() fails.
void ish_aes256_gcm_update(struct ish_aes_gcm_stream *s, const uint8_t *in,
        uint8_t *out, size_t len);
// seal: writes the 16-byte tag to tag_out. open: returns 0 iff tag_in matches
// (constant-time), else -1.
int ish_aes256_gcm_finish(struct ish_aes_gcm_stream *s, const uint8_t *tag_in,
        uint8_t *tag_out);

// Validates against NIST AES-256-GCM vectors. False if unavailable or wrong.
_Bool ish_accel_aes_selftest(void);

#endif
