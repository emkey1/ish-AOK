#ifndef KERNEL_SHA_CRYPT_H
#define KERNEL_SHA_CRYPT_H

#include <stddef.h>

// The $5$ (SHA-256) and $6$ (SHA-512) hashes of a Linux /etc/shadow, computed
// on the host so native code can check a guest password. Returns `buf`, or
// NULL for a salt this does not implement ($1$, $2b$, $y$ ...) or a platform
// with no digest backend -- NULL being what an unsupported salt has always
// returned, and the only safe answer for an authentication primitive.
//
// buf should be at least 128 bytes. Not reentrant-hostile: no statics.
char *aok_sha_crypt(const char *key, const char *salt, char *buf, size_t buflen);

#endif
