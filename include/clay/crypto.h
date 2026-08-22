#ifndef CLAY_CRYPTO_H
#define CLAY_CRYPTO_H

#include <stddef.h>

/* Computes the SHA-256 digest of arbitrary binary input. `out` must have 32
   bytes. Kept dependency-free for small protocol features such as PKCE. */
void clay_sha256(const void *data, size_t len, unsigned char out[32]);

#endif /* CLAY_CRYPTO_H */
